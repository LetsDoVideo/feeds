// plugin-main.cpp — Feeds OBS plugin (thin wrapper).
//
// This plugin does NOT load the Zoom SDK. All Zoom SDK interaction happens
// in FeedsEngine.exe, a subprocess launched by this plugin. The plugin's
// responsibilities are:
//   - Register the Feeds menu in OBS
//   - Register Zoom Participant and Zoom Screenshare source types
//   - Launch and manage the engine subprocess (see engine-client.cpp)
//   - Send IPC messages to the engine for user actions
//   - React to IPC messages from the engine to update UI state
//   - Cache state received from the engine so OBS property callbacks can
//     read it synchronously
//   - Read video frames from shared memory (written by the engine) and
//     hand them to OBS via obs_source_output_video

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/config-file.h>
#include <media-io/video-frame.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <windows.h>
#include <winhttp.h>

#include <QMainWindow>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QSignalBlocker>
#include <QDateTime>
#include <QInputDialog>
#include <QComboBox>
#include <QListView>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QScrollBar>
#include <QPainter>
#include <QRegularExpression>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QStyleOption>
#include <QTextLayout>
#include <QTimer>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QImage>
#include <QPixmap>
#include <QIcon>
#include <QScreen>
#include <QSvgRenderer>
#include <QFile>
#include <QByteArray>
#include <QCursor>
#include <QPointer>
#include <QPalette>
#include <QBrush>
#include <QStandardItemModel>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QEnterEvent>
#include <QResizeEvent>
#include <QScrollArea>
#include <QFontMetrics>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>

#include "feeds-chat-popup-source.h"
#include "feeds-chat-overlay-source.h"
#include "feeds-iso-recorder.h"

// Qt defines `slots` and `signals` as preprocessor macros (expanding to
// empty or to annotations for the Meta-Object Compiler). Any non-Qt code
// below that happens to use identifiers named `slots`, `signals`, or
// `emit` as variables or fields will be silently mangled by the
// preprocessor and produce cryptic syntax errors that point at the wrong
// line. We guard our shared headers and data structures by undefining
// these macros; we don't use Qt's signals/slots mechanism anywhere in
// this file (connections are done via lambdas) so this is safe.
#undef slots
#undef signals
#undef emit

#include "shared-frame.h"
#include "feeds-version.h"
#include "feeds-json-lite.h"
#include "feeds-http.h"

// The plugin's half of feeds-http.h's one outward dependency (the engine
// defines the same function over its Log* stack). DEBUG for a routine DIRECT
// resolution, INFO when a proxy is actually in play — the line that makes a
// "chat doesn't work on my work network" report self-diagnosing.
namespace feeds_http {
void HttpLog(bool important, const char* msg)
{
    blog(important ? LOG_INFO : LOG_DEBUG, "[feeds] %s", msg);
}
}

// LogProxyResolution logs at INFO whenever a proxy is actually in use. Three of
// the four plugin call sites sit in polling loops (YouTube chat polls every few
// seconds; Twitch reconnects), so logging every call would flood the OBS log on
// exactly the managed networks we most need a readable log from. Log the first
// resolution per flow and stay quiet after: the first line is the diagnostic,
// the two-hundredth adds nothing. Deduped on the flow label, so if a PAC hands
// out different answers for, say, youtube.com and the ggpht avatar host, only
// the first is recorded — acceptable for a diagnostic breadcrumb.
static void LogProxyResolutionOnce(const feeds_http::ProxyResolution& px,
                                   const char* flow)
{
    static std::mutex            mtx;
    static std::set<std::string> logged;
    {
        std::lock_guard<std::mutex> lk(mtx);
        if (!logged.insert(flow).second) return;
    }
    feeds_http::LogProxyResolution(px, flow, /*forceInfo=*/false);
}

// The IPC JSON helpers now live in feeds-json-lite.h (shared, unit-tested in
// tests/test_e2p_reader.cpp). Pull them into this TU's global scope so the ~68
// existing unqualified call sites resolve unchanged.
using feeds::ExtractJsonString;
using feeds::ExtractJsonNumber;
using feeds::ExtractJsonStringEscaped;
using feeds::JsonEscape;
using feeds::JsonExtractArrayBody;
using feeds::SplitJsonObjects;

// ---------------------------------------------------------------------------
// Engine client API (from engine-client.cpp)
// ---------------------------------------------------------------------------
namespace feeds {
    bool StartEngine();
    void StopEngine();
    bool SendToEngine(const std::string& jsonMessage);
    void RegisterMessageHandler(const std::string& messageType,
                                std::function<void(const std::string&)> handler);
}

// ---------------------------------------------------------------------------
// Globals — menu actions
// ---------------------------------------------------------------------------
static QAction* g_loginLogoutAction     = nullptr;
static QAction* g_connectAction         = nullptr;
static QAction* g_isoRecordingAction    = nullptr;
static QAction* g_connectOnStartupAction = nullptr;

// Global ISO recording toggle. When true (and tier >= 1), every participant
// source's recorder is enabled — they all start writing when OBS main
// recording starts. Driven by the Feeds menu item; replaces the old
// per-source checkbox.
static bool g_isoRecordingEnabled = false;

// "Connect to Zoom on Startup" toggle. Unlike the ISO toggle (runtime-only),
// this persists across OBS sessions in the user-level frontend config (see
// LoadConnectOnStartupSetting / SaveConnectOnStartupSetting). When enabled,
// the join dialog is auto-opened once per session after login completes.
static bool g_connectOnStartupEnabled = false;
// One-shot guard so the startup auto-open fires at most once per OBS session
// — a mid-session logout/login must not re-pop the dialog.
static bool g_startupConnectDone = false;

// ---------------------------------------------------------------------------
// Globals — cached state from engine
// ---------------------------------------------------------------------------
// Non-static so feeds-chat-popup-source.cpp and feeds-chat-overlay-source.cpp
// can extern this for their tier-gating checks (mirrors the avatar-cache
// linkage pattern lower in this file).
bool g_isLoggedIn                 = false;
// True once we've heard back from the engine on a login attempt — either
// login_succeeded (stored token valid, or user just completed OAuth) or
// login_failed (user cancelled, token exchange broke). The create
// callbacks use this to defer the logged-out popup until we have a
// confirmed answer, otherwise a saved scene loading Feeds sources at
// OBS startup will pop "Please log in to Zoom" before the engine
// finishes authenticating an already-logged-in user.
//
// Caveat: on a fresh install with no stored token, the engine doesn't
// send anything at startup, so this stays false until the user clicks
// Login from the Feeds menu. That means the first source-creation
// attempt by a never-logged-in user silently fails instead of prompting
// — they have to discover the menu. Same behaviour as the existing
// tier check, which also requires g_isLoggedIn.
bool g_loginAttemptCompleted      = false;
// Set when an auth round-trip (login or logout) is in flight; cleared at
// every termination handler. Drives the Login/Logout menu item's
// "Logging in..." / "Logging out..." disabled states so a double-click
// during the in-progress window can't double-fire the request.
static bool g_authInProgress      = false;
static bool g_isInMeeting         = false;
static bool g_rawLiveStreamGranted = false;
static bool g_pendingMeetingJoin   = false;

// Raw livestream privilege state. Mirrors g_rawLiveStreamGranted but with
// the extra states the properties panel needs to render the
// "waiting / denied / timed out" UI. g_rawLiveStreamGranted stays for
// subscribe gating to keep the diff minimal — kept in sync with this
// enum in every transition. Non-static for parity with g_isLoggedIn /
// g_currentTier, though no other TU references it today.
//
//   NotRequested  before joining, or after leaving — initial state
//   Pending       engine sent RequestRawLiveStreaming, awaiting host
//   Granted       host approved (g_rawLiveStreamGranted also true)
//   Denied        host declined, or revoked mid-meeting
//   TimedOut      no host response within the SDK's request window
enum class RawPrivilegeState {
    NotRequested,
    Pending,
    Granted,
    Denied,
    TimedOut,
};
RawPrivilegeState g_rawPrivilegeState = RawPrivilegeState::NotRequested;

// Single network-requirements page that BOTH network-failure messages point
// at (login_timeout and tier_unreachable), so a blocked user — or the IT desk
// they forward the message to — lands in one place that lists every host Feeds
// needs, instead of two half-answers.
static const char* kNetworkRequirementsUrl =
    "https://letsdovideo.com/feeds-network";

static std::string g_userDisplayName;
static std::string g_userPMI;
// Non-static for the same reason as g_isLoggedIn above.
int                g_currentTier = 0;
static uint32_t    g_enginePid   = 0;   // Populated from engine_ready

static unsigned long long g_currentMeetingNumber = 0;

// Set when the plugin sends create_instant_meeting, consumed (and cleared)
// by the meeting_joined handler so it knows to surface the share-this-meeting
// popup, plus defensively cleared on meeting_failed / meeting_left. The
// consumer is wired in commit 4; commit 3 only sets this so the protocol
// shape is in place when commit 4 lands.
static std::atomic<bool> g_pendingInstantMeeting{false};

// Zoom Events connect flow (async, driven over IPC). These pending flags gate
// the events_list / sessions_list handlers so only a user-initiated "Zoom
// Events" click pops a picker — a stray reply can't.
static std::atomic<bool> g_eventsListPending{false};
static std::atomic<bool> g_eventsSessionsPending{false};

static unsigned int       g_activeSharerUserId   = 0;
static unsigned int       g_cachedMyUserId       = 0;
// UI-thread-owned: written only in marshalled lambdas (the active_speaker_changed
// handler and the meeting-end/left resets), read only by the dock on the UI
// thread. The dock's Active-Speaker row (participant_id sentinel 1) resolves its
// mute mark through this. 0 = no active speaker yet -> that row shows no mark.
static unsigned int       g_activeSpeakerUserId  = 0;

struct CachedParticipant {
    unsigned int id;
    std::string  name;
};
static std::vector<CachedParticipant> g_cachedParticipants;
static std::mutex                     g_participantsMutex;

// ---------------------------------------------------------------------------
// Tier → limits (matches v1.0.0)
// 0 = Free (1 feed, 720p)
// 1 = Basic (3, 1080p30)
// 2 = Streamer (5, 1080p30)
// 3 = Broadcaster (8, 1080p30)
// ---------------------------------------------------------------------------
static int GetMaxFeedsForTier() {
    switch (g_currentTier) {
        case 1:  return 3;
        case 2:  return 5;
        case 3:  return 8;
        default: return 1;
    }
}

// Product-wide hard maximum for participant sources — equal to the Broadcaster
// (top tier) cap. There is no tier above Broadcaster, so a source beyond this
// can never be activated by anyone; more than this is unsupported. Both the
// ceiling messaging (a tier whose cap IS this max gets "maximum supported"
// wording with no upgrade CTA) and the 9th-source hard block key off it.
static constexpr int kMaxParticipantSourcesEver = 8;

// True while OBS is loading a scene collection (initial startup load or a
// collection switch). The 9th-source hard block (OnSourceCreated) skips removal
// while this is set, so a legacy scene saved with >8 participant sources isn't
// silently pruned — auto-save would bake in the loss — and instead loads its
// extras as dormant/over-cap. Starts true because source_create is connected at
// module-load, before OBS's startup scene load fires; cleared on
// FINISHED_LOADING / SCENE_COLLECTION_CHANGED.
static std::atomic<bool> g_sceneCollectionLoading{true};

void OnLoginClick();
void OnLogoutClick();
void OnConnectClick();
static void UpdateLoginLogoutMenuItem();
// Shared tier-upgrade prompt (defined far below, with the other tier chrome).
// ShowTierLimitDialog is the reusable modal upgrade dialog (rich-text label with a
// clickable upgrade link); ShouldShowTierPopup throttles it to one per few seconds
// so rapid clicks don't stack dialogs. Used by the screenshare/chat create paths
// and, now, the header's locked-button clicks.
bool ShouldShowTierPopup();
void ShowTierLimitDialog(const QString& title, const QString& html);
// Marshal a read-only participant-dock rebuild onto the Qt UI thread. Safe to
// call from any thread (defined after the dock class + g_participantDock).
static void PostParticipantDockRefresh();
// Dock combo pick handler — commit a participant reassignment for the source
// with the given UUID (defined after RecordParticipantBinding; UI thread only).
static void OnDockParticipantPicked(const std::string& uuid, long long selectedId);
// Dock create/reference actions (defined with the placement helpers, far below).
// ResolveCurrentEditScene returns the scene the Source dock edits — preview in
// Studio Mode, current otherwise — as a ref to release, or null. The two actions
// operate on OBS's real scene graph; the dock reflects results via existing signals.
static obs_source_t* ResolveCurrentEditScene();
static void ApplyFitCenterGeometry(obs_sceneitem_t* item, obs_source_t* source);
static void ApplyChatPopupPlacementToItem(obs_sceneitem_t* item);
static void CreateParticipantSourceInCurrentScene();
static void AddSourceReferenceToCurrentScene(const std::string& uuid);

// Placement applied to the newly-added scene-item on a reference-add. The
// reference path fires no source_create, so it must place the item itself (the
// deferred ApplyChat*/ApplyParticipantPlacement hooks only run on create) — and
// the right default differs per type: screenshare fills the canvas (fit-center),
// the chat popup sits bottom-center at natural size.
enum class RefAddPlacement { FitCenter, PopupDefault };

// Header add-source actions (defined with the scene helpers, far below).
// CreateSourceOfTypeInCurrentScene mints a fresh source of the given type and
// adds it (always-new, like "Create Participant Feed"); AddOrReferenceSourceInCurrentScene
// references an existing source of that type if one exists, else creates one
// (reference-or-create) — this is how the screenshare button never trips the
// zs_create singleton block by trying to create a second instance.
static void CreateSourceOfTypeInCurrentScene(const char* typeId, const char* baseName);
static void AddOrReferenceSourceInCurrentScene(const char* typeId, const char* baseName,
                                               RefAddPlacement placement);
// Per-source dock header actions (defined with the scene helpers, far below).
// OpenSourceFilters opens OBS's native Filters dialog; ShowIncludedScenesMenu
// pops a menu of the scenes the source is in, switching to a clicked one
// (Studio-Mode-aware). Both use confirmed obs_frontend_* exports.
static void OpenSourceFilters(const std::string& uuid);
static void ShowIncludedScenesMenu(const std::string& uuid);

// ---------------------------------------------------------------------------
// Per-source data
// ---------------------------------------------------------------------------

// obs_data key for the durable "remembered participant" — the display name a
// participant-pinned source was last bound to. Unlike participant_id (a Zoom
// runtime user ID that is reassigned every session/rejoin), the name survives
// a mid-session drop/rejoin, an OBS restart, and scene-collection save/load,
// so it is the key we auto-rebind on. See ReconcileRememberedParticipants.
static constexpr const char* kParticipantNameKey = "participant_name";

struct ZpSourceData {
    obs_source_t* source          = nullptr;
    std::string   uuid;
    unsigned int  current_user_id = 0;
    // Runtime-only (never persisted): true once current_user_id was bound to a
    // real present participant THIS session — via a manual pick or a name
    // match in ReconcileRememberedParticipants. Distinguishes a live, trusted
    // binding from a stale participant_id loaded from a prior session (whose
    // runtime ID can coincidentally collide with a different person). Only a
    // session-confirmed binding is trusted for rename-refresh; a stale loaded
    // ID is re-bound by remembered name instead. Reset on meeting_left /
    // logout / session_expired.
    bool          bound_this_session = false;
    // Runtime-only (never persisted): the participant id we currently have an
    // active engine subscription for (0 = not subscribed). The single guard
    // against double-subscribing the same source/id when more than one of the
    // subscribe paths (manual pick, name reconcile, privilege-granted handler)
    // fires for it. A rebind changes current_user_id, so this stops matching
    // and the source is correctly re-subscribed to the new id. Cleared to 0
    // when the subscription is torn down (unselect, meeting_left, logout,
    // expiry) and, authoritatively, at raw_livestream_granted — the engine
    // holds no subscriptions on meeting (re)entry, so the grant-time sweep
    // always re-subscribes every bound source fresh regardless of leave-side
    // teardown ordering.
    unsigned int  subscribed_user_id = 0;
    // Per-source ISO recorder (feeds-iso-recorder). Created in zp_create,
    // torn down in zp_destroy. Records this source to its own MP4 alongside
    // OBS's main recording when the properties checkbox is enabled.
    feeds::feeds_iso_recorder* iso = nullptr;
    // In-memory only; recomputed on every login_succeeded by
    // ReconcileSourcesToTier. Not persisted in scene-collection data
    // because the same scene can be tier-legal for one Feeds user and
    // over-tier for another (Broadcaster sharing an OBS config with a
    // Basic user, etc.).
    bool          tier_disabled   = false;
    // 1-based position across all participant sources in the scene
    // collection (vector order = creation order). Set by
    // ReconcileSourcesToTier. 0 means "not yet computed."
    int           source_position = 0;

    HANDLE mapping = nullptr;
    void*  view    = nullptr;
    feeds_shared::SharedFrameHeader* header = nullptr;
    feeds_shared::FrameSlot*         frameSlots  = nullptr;

    std::thread       pumpThread;
    std::atomic<bool> pumpShouldExit{false};
    HANDLE            pumpWakeEvent = nullptr;
    uint32_t          lastReadIndex = 0;

    // Monotonic os_gettime_ns() of the last REAL frame (width>0) the pump output
    // for this source — the "receiving video now" signal the dock's live
    // indicator reads. Stamped only on the real-frame branch (never on the
    // width==0 camera-off sentinel, never when the write index isn't advancing),
    // so a camera-off OR a silent stall both let it go stale, and "live" is
    // simply (os_gettime_ns() - tick) < a short threshold. Written lock-free by
    // the pump thread, read lock-free by the dock poll; 0 = no real frame yet.
    std::atomic<uint64_t> lastRealFrameTick{0};

    // Serialises the pump-thread + shared-memory lifecycle for this source.
    // Held across the full body of Open/CloseSharedMemory (which themselves
    // call StartPumpThread / StopPumpThread). Without this, the graphics
    // thread (zp_update) and PipeReaderThread (source_texture_released,
    // meeting_left, etc.) could enter StopPumpThread concurrently and race
    // on std::thread::join() — see v1.2.3 release notes.
    //
    // Lock ordering: g_sourcesMutex (outer) → ZpSourceData::lifecycleMutex
    // (inner). Every caller that holds g_sourcesMutex and reaches into
    // Open/CloseSharedMemory acquires them in that order; zp_destroy
    // releases g_sourcesMutex before taking lifecycleMutex.
    std::mutex        lifecycleMutex;
};

static std::mutex g_sourcesMutex;
static std::vector<ZpSourceData*> g_allParticipantSources;

static ZpSourceData* FindSourceByUuid(const std::string& uuid) {
    for (ZpSourceData* s : g_allParticipantSources) {
        if (s && s->uuid == uuid) return s;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// JSON helpers — moved to common/feeds-json-lite.h (shared with the >4KB reader
// unit test) and pulled into scope via the using-declarations near the includes.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Update check — polls GitHub Releases on plugin load, surfaces a one-time
// modal popup when a newer Feeds version is available. Failure-silent: any
// network/parse/rate-limit error just suppresses the popup, never blocks
// startup or shows an error. One popup per OBS session; the user sees it
// again on the next OBS start until they update.
// ---------------------------------------------------------------------------
static std::atomic<bool> g_updatePopupShown{false};
static std::thread       g_updateCheckThread;

static bool ParseSemver(const std::string& s, int& maj, int& min, int& patch) {
    std::string v = s;
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);

    size_t dot1 = v.find('.');
    if (dot1 == std::string::npos) return false;
    size_t dot2 = v.find('.', dot1 + 1);
    if (dot2 == std::string::npos) return false;

    try {
        maj   = std::stoi(v.substr(0, dot1));
        min   = std::stoi(v.substr(dot1 + 1, dot2 - dot1 - 1));
        patch = std::stoi(v.substr(dot2 + 1));
    } catch (...) {
        return false;
    }
    return maj >= 0 && min >= 0 && patch >= 0;
}

static bool IsNewer(const std::string& latest, const std::string& current) {
    int lM, lm, lp, cM, cm, cp;
    if (!ParseSemver(latest,  lM, lm, lp)) return false;
    if (!ParseSemver(current, cM, cm, cp)) return false;
    if (lM != cM) return lM > cM;
    if (lm != cm) return lm > cm;
    return lp > cp;
}

// One-shot GET against api.github.com. Returns response body on HTTP 200,
// empty string on any failure. Mirrors the engine's WinHTTP pattern
// (engine/engine-api.cpp::FetchAndApplyEntitlement). Tight timeouts so
// plugin unload never waits more than a few seconds for this to finish.
static std::string FetchGitHubLatestRelease() {
    feeds_http::ProxyResolution px = feeds_http::ResolveProxyForUrl(
        L"https://api.github.com/repos/LetsDoVideo/feeds/releases/latest");
    feeds_http::LogProxyResolution(px, "UpdateCheck", /*forceInfo=*/false);
    HINTERNET hSession =
        feeds_http::OpenProxiedSession(L"Feeds-OBS-Plugin/1.0", px);
    if (!hSession) return "";

    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);

    HINTERNET hConnect = WinHttpConnect(hSession, L"api.github.com",
                                        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return "";
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET",
        L"/repos/LetsDoVideo/feeds/releases/latest",
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    WinHttpAddRequestHeaders(hRequest,
        L"Accept: application/vnd.github+json",
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    std::string response;
    BOOL sentOk = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS,
                                     0, nullptr, 0, 0, 0);
    if (sentOk && WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status, &statusSize, WINHTTP_NO_HEADER_INDEX);

        if (status == 200) {
            char buf[4096];
            DWORD bytesRead = 0;
            while (WinHttpReadData(hRequest, buf, sizeof(buf), &bytesRead)
                   && bytesRead > 0) {
                response.append(buf, bytesRead);
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

// Must run on the Qt main thread.
static void ShowUpdatePopup(const std::string& latestVer) {
    QMainWindow* main = (QMainWindow*)obs_frontend_get_main_window();
    QDialog* dlg = new QDialog(main);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle("Feeds Update Available");
    dlg->setWindowModality(Qt::ApplicationModal);

    QLabel* intro = new QLabel(
        "A new version of Feeds is available. Download and install it to update.",
        dlg);

    QLabel* curLabel = new QLabel(
        QString("<b>Current version:</b> ") + feeds_shared::VERSION, dlg);
    curLabel->setTextFormat(Qt::RichText);
    curLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    QLabel* latestLabel = new QLabel(
        QString("<b>Latest version:</b> ") +
        QString::fromStdString(latestVer), dlg);
    latestLabel->setTextFormat(Qt::RichText);
    latestLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    // Opens the GitHub releases page — where the installer download lives — so the
    // label names the action (download), not "release notes" (which read as done
    // after reading, leaving the user still on the old version). Same destination.
    QPushButton* downloadBtn = new QPushButton("Download Update", dlg);
    QObject::connect(downloadBtn, &QPushButton::clicked, dlg, [dlg]() {
        QDesktopServices::openUrl(
            QUrl("https://github.com/LetsDoVideo/feeds/releases"));
        dlg->accept();
    });

    QPushButton* laterBtn = new QPushButton("Later", dlg);
    laterBtn->setDefault(true);
    QObject::connect(laterBtn, &QPushButton::clicked,
                     dlg, &QDialog::accept);

    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->addWidget(downloadBtn);
    btnRow->addStretch();
    btnRow->addWidget(laterBtn);

    QVBoxLayout* layout = new QVBoxLayout(dlg);
    layout->addWidget(intro);
    layout->addSpacing(4);
    layout->addWidget(curLabel);
    layout->addWidget(latestLabel);
    layout->addSpacing(8);
    layout->addLayout(btnRow);

    dlg->show();
}

static void CheckForUpdateAsync() {
    if (g_updatePopupShown.load()) return;

    g_updateCheckThread = std::thread([]() {
        std::string json = FetchGitHubLatestRelease();
        if (json.empty()) return;

        std::string tag = ExtractJsonString(json, "tag_name");
        if (tag.empty()) return;

        if (!IsNewer(tag, feeds_shared::VERSION)) return;

        if (g_updatePopupShown.exchange(true)) return;

        std::string displayTag = tag;
        if (!displayTag.empty() &&
            (displayTag[0] == 'v' || displayTag[0] == 'V'))
            displayTag.erase(0, 1);

        QTimer::singleShot(0,
            (QObject*)obs_frontend_get_main_window(),
            [displayTag]() { ShowUpdatePopup(displayTag); });
    });
}

static void RefreshAllSourceProperties() {
    obs_enum_sources([](void*, obs_source_t* src) -> bool {
        const char* id = obs_source_get_id(src);
        if (id && (strcmp(id, "zoom_participant_source") == 0 ||
                   strcmp(id, "zoom_screenshare_source") == 0)) {
            obs_source_update_properties(src);
        }
        return true;
    }, nullptr);

    // Co-located dock rebuild: every RefreshAllSourceProperties call site (roster
    // change, meeting join/leave, login/logout/expiry, all raw-privilege
    // transitions, and tier/cap change) also needs the participant dock rebuilt.
    // This runs only from UI-thread contexts, but PostParticipantDockRefresh
    // marshals uniformly regardless.
    PostParticipantDockRefresh();
}

// (Re)subscribe a participant source to its currently-bound participant, if it
// is bound (current_user_id >= 1, which covers both a pinned participant and
// the [Active Speaker] sentinel 1), we are in a meeting with raw-livestream
// privilege, and we are not already subscribed to that same id. Returns true
// if a subscribe was sent. Caller MUST hold g_sourcesMutex.
//
// This is the single choke point for "(re)subscribe a bound source." It is
// called from name reconcile and the raw_livestream_granted handler — either of
// which can be the first to run once privilege exists — and the
// subscribed_user_id guard keeps them from double-subscribing the same
// source/id. (zp_update, the manual-pick path, maintains subscribed_user_id
// inline; its own "selection unchanged" guard already prevents a redundant
// resubscribe.) A rebind moves current_user_id off subscribed_user_id, so the
// guard correctly lets the source re-subscribe to the new id.
static bool SubscribeBoundSourceLocked(ZpSourceData* s) {
    if (!s || s->uuid.empty()) return false;
    const char* srcName = s->source ? obs_source_get_name(s->source) : "";
    if (srcName == nullptr) srcName = "";
    if (s->tier_disabled) {
        blog(LOG_INFO, "[feeds] bind-decision: REFUSED subscribe source='%s' "
             "userId=%u bound_this_session=%d reason=tier_disabled",
             srcName, s->current_user_id, s->bound_this_session ? 1 : 0);
        return false;
    }
    if (s->current_user_id < 1) return false;            // unbound (0) — quiet

    // Fallback removal (participant auto-bind fix). A source is (re)subscribed
    // ONLY when its binding was CONFIRMED this session — a manual pick, a
    // remembered display-name reconcile match, or the active-speaker sentinel
    // (all three set bound_this_session=true before we get here). A
    // current_user_id with bound_this_session=0 is a runtime id left over from
    // a prior meeting (meeting_left / logout / session_expired clear the flag
    // but the id lingered): NOT a binding. Zoom reassigns runtime ids per
    // meeting, so re-subscribing that dead id lands whoever now holds it into
    // this source — the never-assigned auto-bind. Refuse: the source stays
    // unbound and off-screen until the operator assigns someone. There is no
    // substitute-source behavior.
    if (!s->bound_this_session) {
        blog(LOG_INFO, "[feeds] bind-decision: REFUSED subscribe source='%s' "
             "userId=%u bound_this_session=0 reason=not-confirmed-this-session "
             "(stale runtime id — leave unbound, no auto-place)",
             srcName, s->current_user_id);
        return false;
    }

    if (!(g_isInMeeting && g_rawLiveStreamGranted)) {
        blog(LOG_INFO, "[feeds] bind-decision: DEFERRED subscribe source='%s' "
             "userId=%u bound_this_session=1 reason=no_privilege_yet",
             srcName, s->current_user_id);
        return false;
    }
    if (s->subscribed_user_id == s->current_user_id) return false;  // already subscribed

    // Single choke point where a CONFIRMED binding is (re)subscribed.
    blog(LOG_INFO, "[feeds] bind-decision: SUBSCRIBE (confirmed) source='%s' "
         "uuid=%s userId=%u bound_this_session=1",
         srcName, s->uuid.c_str(), s->current_user_id);

    // Auto-rebind / grant / rejoin all route through the engine's RECREATE path
    // (full destroy + fresh createRenderer via the sequenced gate), not a plain
    // subscribe. A kept or re-pointed renderer loses SDK delivery on a
    // participant rejoin; only a fresh, sequenced createRenderer recovers it.
    // (Manual dropdown changes still send participant_source_subscribe from
    // zp_update — a single, user-paced gentle re-point.)
    std::string msg = "{\"type\":\"participant_source_recreate\","
                      "\"source_id\":\"" + s->uuid + "\","
                      "\"participant_id\":" +
                      std::to_string(s->current_user_id) + "}";
    feeds::SendToEngine(msg);
    s->subscribed_user_id = s->current_user_id;
    return true;
}

// ---------------------------------------------------------------------------
// Persistent participant selection — auto-rebind by remembered display name.
//
// Each participant-pinned source remembers (in obs_data, key kParticipantNameKey)
// the display name it was last bound to. This runs on the UI thread on every
// participant_list_changed — which the engine sends at meeting-join (the
// initial sweep of everyone already present), on each newcomer (onUserJoin),
// on rename (onUserNamesChanged), and on leave — so a single pass covers the
// join sweep, the per-newcomer rebind (the mid-session drop/rejoin case), and
// keeping the durable name fresh on rename.
//
// Rules (deliberately fail-closed to manual selection):
//   - Match on exact display name only; never fuzzy/partial. The name is the
//     only identifier present for every participant including guests, and a
//     rejoining participant almost always keeps the same name.
//   - Auto-bind only when the remembered name matches exactly ONE present
//     candidate. Zero matches → leave unbound and waiting. Two+ matches
//     (duplicate names) → leave for manual selection. A wrong auto-bind is
//     worse than none.
//   - Active-speaker sources (participant_id == 1) bind to a role, not a
//     person, so a remembered name is meaningless — skip them entirely.
//   - Self is excluded (mirrors the dropdown), as are tier-disabled sources.
// ---------------------------------------------------------------------------
// joinedIds: participant ids present now that weren't in the prior roster (a
// join or rejoin). A source already bound to such an id (Case A) is force-
// re-established, because its engine renderer went stale across the drop.
static void ReconcileRememberedParticipants(
    const std::vector<unsigned int>& joinedIds) {
    // Snapshot the present roster: id → name, plus per-name occurrence count
    // and the single matching id (the count guards against duplicate names).
    // Self is excluded so a source can never auto-bind to the Feeds user, the
    // same exclusion the participant dropdown applies.
    std::map<unsigned int, std::string> presentById;
    std::map<std::string, unsigned int> idByName;
    std::map<std::string, int>          countByName;
    {
        std::lock_guard<std::mutex> lock(g_participantsMutex);
        for (const auto& p : g_cachedParticipants) {
            if (g_cachedMyUserId != 0 && p.id == g_cachedMyUserId) continue;
            if (p.name.empty()) continue;
            presentById[p.id] = p.name;
            idByName[p.name]  = p.id;
            countByName[p.name]++;
        }
    }

    std::lock_guard<std::mutex> lock(g_sourcesMutex);
    for (ZpSourceData* s : g_allParticipantSources) {
        if (!s || !s->source || s->uuid.empty()) continue;
        if (s->tier_disabled) continue;

        obs_data_t* settings = obs_source_get_settings(s->source);
        if (!settings) continue;

        long long pid = obs_data_get_int(settings, "participant_id");
        // Active-speaker source (sentinel 1): binds to a role, not a name, so the
        // name-rebind logic below is meaningless — take its own path here, before
        // Case A / Case B. Seed the runtime binding: current_user_id stays 0 across
        // a fresh load (zp_create ignores settings), and reconcile is where every
        // regular source gets its runtime id from the roster — the sentinel gets
        // its here too. Then subscribe: SubscribeBoundSourceLocked sends
        // participant_source_recreate with participant_id 1, and its
        // subscribed_user_id guard makes repeat reconcile passes idempotent (no
        // re-fire, so the follow doesn't churn). Everything downstream — the
        // engine's recreate -> gate -> Start-in-waiting -> NotifyActiveSpeakerChanged
        // -> Resubscribe follow path — already exists; fresh start just never
        // reached it because the source was never marked bound. bound_this_session
        // is harmless for a sentinel (Case A needs current_user_id > 1, and nothing
        // name-rebinds it).
        const char* srcName = s->source ? obs_source_get_name(s->source) : "";
        if (srcName == nullptr) srcName = "";

        if (pid == 1) {
            blog(LOG_INFO, "[feeds] bind-decision: rule=active-speaker source='%s' "
                 "uuid=%s (sentinel) bound_this_session=%d",
                 srcName, s->uuid.c_str(), s->bound_this_session ? 1 : 0);
            s->current_user_id    = 1;
            s->bound_this_session = true;
            SubscribeBoundSourceLocked(s);
            obs_data_release(settings);
            continue;
        }

        const char* rememberedC = obs_data_get_string(settings, kParticipantNameKey);
        std::string remembered  = rememberedC ? rememberedC : "";

        // Case A: a live, this-session binding whose participant is still
        // present. Keep the binding; only refresh the durable name if they
        // renamed (so a later rejoin under the new name still matches). The
        // bound_this_session guard is essential: a participant_id loaded from a
        // prior session is NOT trusted here, so a stale ID that coincidentally
        // collides with a different present person can't hijack the name.
        if (s->bound_this_session && s->current_user_id > 1) {
            auto it = presentById.find(s->current_user_id);
            if (it != presentById.end()) {
                if (!it->second.empty() && it->second != remembered)
                    obs_data_set_string(settings, kParticipantNameKey,
                                        it->second.c_str());
                // If this participant JUST (re)joined, the engine's kept
                // renderer is stale (the SDK stopped delivering across the
                // drop) — force a fresh re-establishment via the recreate path.
                // Bypass the already-subscribed guard, which is stale-equal here
                // (that's exactly the wedged state). A participant that was
                // present all along is left untouched (working binding).
                bool justJoined = std::find(joinedIds.begin(), joinedIds.end(),
                                            s->current_user_id) != joinedIds.end();
                blog(LOG_INFO, "[feeds] bind-decision: rule=%s source='%s' key='%s' "
                     "participant='%s' userId=%u bound_this_session=1",
                     justJoined ? "same-session-rejoin (force recreate)"
                                : "same-session-keep",
                     srcName, remembered.c_str(), it->second.c_str(),
                     s->current_user_id);
                if (justJoined) {
                    s->subscribed_user_id = 0;   // bypass already-subscribed guard
                    SubscribeBoundSourceLocked(s);
                }
                obs_data_release(settings);
                continue;
            }
            // Bound participant is absent (dropped) — fall through and try to
            // re-bind them by remembered name (handles the rejoin).
            blog(LOG_INFO, "[feeds] bind-decision: source='%s' key='%s' "
                 "bound userId=%u now ABSENT — trying name rebind",
                 srcName, remembered.c_str(), s->current_user_id);
        }

        // Case B: unbound, or bound-but-absent. Auto-bind only on an exact
        // name match with exactly one present candidate.
        if (remembered.empty()) {
            // No saved persistent key and no confirmed this-session binding —
            // a never-assigned source. Left unbound by design; log only when a
            // stale runtime id lingers (would be a fallback-subscribe candidate).
            if (s->current_user_id > 1)
                blog(LOG_INFO, "[feeds] bind-decision: REFUSED source='%s' "
                     "key='' userId=%u bound_this_session=%d "
                     "reason=no-saved-key (leave unbound)",
                     srcName, s->current_user_id, s->bound_this_session ? 1 : 0);
            obs_data_release(settings);
            continue;
        }
        auto cit = countByName.find(remembered);
        if (cit == countByName.end() || cit->second != 1) {
            blog(LOG_INFO, "[feeds] bind-decision: REFUSED source='%s' key='%s' "
                 "bound_this_session=%d reason=%s (leave unbound)",
                 srcName, remembered.c_str(), s->bound_this_session ? 1 : 0,
                 cit == countByName.end() ? "no-name-match"
                                          : "duplicate-names");
            obs_data_release(settings);  // no match, or duplicate names
            continue;
        }
        unsigned int newId = idByName[remembered];
        if (s->bound_this_session && newId == s->current_user_id) {
            obs_data_release(settings);  // already correctly bound — quiet
            continue;
        }

        // Bind: persist the runtime id (keeps the dropdown selection in sync
        // and survives same-session scene save), mark the binding live, and
        // subscribe. If privilege isn't granted yet (initial join sweep),
        // SubscribeBoundSourceLocked is a no-op and the raw_livestream_granted
        // handler subscribes this source once privilege exists.
        blog(LOG_INFO, "[feeds] bind-decision: rule=matched-saved-persistent-key "
             "source='%s' key='%s' participant='%s' userId=%u "
             "bound_this_session(prev)=%d",
             srcName, remembered.c_str(), remembered.c_str(), newId,
             s->bound_this_session ? 1 : 0);
        s->current_user_id    = newId;
        s->bound_this_session = true;
        obs_data_set_int(settings, "participant_id", (long long)newId);

        SubscribeBoundSourceLocked(s);
        obs_data_release(settings);
    }
}

// ---------------------------------------------------------------------------
// Pump thread — reads frames from shared memory, feeds them to OBS.
//
// One instance runs per active Zoom Participant source with a live shared
// memory mapping. The thread waits on an event with an 8ms timeout; on
// wake, checks for new frames. Max added latency: ~8ms worst case.
//
// Engine doesn't signal our event today (would require cross-process
// event handle sharing). So we fall through on the timeout. Fine for
// the latency budget — Zoom delivers at ~33ms (30fps) or ~16ms (60fps)
// intervals, and 8ms is well under either.
// ---------------------------------------------------------------------------
static void PumpThreadFunc(ZpSourceData* data) {
    if (!data || !data->source) return;

    blog(LOG_INFO, "[feeds] pump thread started for source=%s",
         data->uuid.c_str());

    while (!data->pumpShouldExit) {
        WaitForSingleObject(data->pumpWakeEvent, 8);

        if (data->pumpShouldExit) break;
        if (!data->header || !data->frameSlots) continue;

        uint32_t currentWrite = data->header->write_index;
        if (currentWrite == data->lastReadIndex) continue;

        // Read the most recent slot; skip older ones if we're behind.
        // Zero-buffering philosophy: drop frames rather than buffer them.
        uint32_t slotIdx = (currentWrite - 1) % feeds_shared::RING_SLOTS;
        feeds_shared::FrameSlot* slot = &data->frameSlots[slotIdx];

        MemoryBarrier();

        uint32_t width  = slot->width;
        uint32_t height = slot->height;

        // width==0 (or height==0) is the engine's "blank" sentinel,
        // written when the SDK signals raw-data-off. Clear the OBS
        // source so it goes transparent instead of freezing on the
        // last frame. When frames resume, the engine writes a normal
        // slot with real dimensions and the source renders again.
        if (width == 0 || height == 0) {
            obs_source_output_video(data->source, nullptr);
            data->lastReadIndex = currentWrite;
            continue;
        }

        // Genuine corruption guard — refuse oversized frames rather
        // than overrunning the slot's data buffer.
        if (width  > feeds_shared::MAX_FRAME_WIDTH ||
            height > feeds_shared::MAX_FRAME_HEIGHT) {
            data->lastReadIndex = currentWrite;
            continue;
        }

        size_t ySize = (size_t)width * height;
        size_t uSize = (size_t)(width / 2) * (height / 2);

        struct obs_source_frame obsFrame = {};
        obsFrame.format      = VIDEO_FORMAT_I420;
        obsFrame.width       = width;
        obsFrame.height      = height;
        obsFrame.data[0]     = slot->data;
        obsFrame.data[1]     = slot->data + ySize;
        obsFrame.data[2]     = slot->data + ySize + uSize;
        obsFrame.linesize[0] = slot->stride_y;
        obsFrame.linesize[1] = slot->stride_u;
        obsFrame.linesize[2] = slot->stride_v;

        video_format_get_parameters(VIDEO_CS_DEFAULT, VIDEO_RANGE_PARTIAL,
                                    obsFrame.color_matrix,
                                    obsFrame.color_range_min,
                                    obsFrame.color_range_max);

        // Wall-clock timestamp at delivery time. Zero added latency.
        // See discussion in v1.0.0 — fixed-increment or SDK-provided
        // timestamps caused OBS to accumulate buffered frames.
        obsFrame.timestamp = os_gettime_ns();

        obs_source_output_video(data->source, &obsFrame);

        // Real frame delivered — stamp the "live" tick (this branch only, so a
        // camera-off sentinel or a stall leaves it stale). Lock-free; the dock
        // poll reads it to drive the live indicator.
        data->lastRealFrameTick.store(os_gettime_ns(), std::memory_order_relaxed);

        data->header->last_read_index = currentWrite;
        data->lastReadIndex = currentWrite;
    }

    blog(LOG_INFO, "[feeds] pump thread exiting for source=%s",
         data->uuid.c_str());
}

// Caller MUST hold data->lifecycleMutex. Only called from OpenSharedMemory;
// kept as a private helper so the lock is taken once at the Open/Close
// boundary rather than re-entered here.
static void StartPumpThread(ZpSourceData* data) {
    if (!data) return;
    if (data->pumpThread.joinable()) return;

    data->pumpShouldExit = false;
    if (!data->pumpWakeEvent) {
        data->pumpWakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    data->pumpThread = std::thread(PumpThreadFunc, data);
}

// Caller MUST hold data->lifecycleMutex. Only called from Open/CloseSharedMemory.
// Without the lock, two threads could pass joinable() and call join() on the
// same std::thread — UB, observed as std::system_error crashing the process.
static void StopPumpThread(ZpSourceData* data) {
    if (!data) return;

    data->pumpShouldExit = true;
    if (data->pumpWakeEvent) SetEvent(data->pumpWakeEvent);

    if (data->pumpThread.joinable()) {
        data->pumpThread.join();
    }

    if (data->pumpWakeEvent) {
        CloseHandle(data->pumpWakeEvent);
        data->pumpWakeEvent = nullptr;
    }
}

static void OpenSharedMemory(ZpSourceData* data) {
    if (!data || g_enginePid == 0) return;

    std::lock_guard<std::mutex> lifecycleLock(data->lifecycleMutex);

    if (data->mapping) {
        StopPumpThread(data);
        if (data->view)    { UnmapViewOfFile(data->view); data->view = nullptr; }
        if (data->mapping) { CloseHandle(data->mapping); data->mapping = nullptr; }
        data->header = nullptr;
        data->frameSlots  = nullptr;
    }

    std::string name = feeds_shared::MakeFrameRegionName(g_enginePid, data->uuid);

    data->mapping = OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE, FALSE,
                                     name.c_str());
    if (!data->mapping) {
        blog(LOG_ERROR, "[feeds] OpenFileMapping failed for '%s', err=%lu",
             name.c_str(), GetLastError());
        return;
    }

    data->view = MapViewOfFile(data->mapping, FILE_MAP_READ | FILE_MAP_WRITE,
                               0, 0, feeds_shared::REGION_SIZE);
    if (!data->view) {
        blog(LOG_ERROR, "[feeds] MapViewOfFile failed for '%s', err=%lu",
             name.c_str(), GetLastError());
        CloseHandle(data->mapping);
        data->mapping = nullptr;
        return;
    }

    data->header = (feeds_shared::SharedFrameHeader*)data->view;
    data->frameSlots  = (feeds_shared::FrameSlot*)
        ((uint8_t*)data->view + sizeof(feeds_shared::SharedFrameHeader));

    if (data->header->magic != feeds_shared::REGION_MAGIC ||
        data->header->version != feeds_shared::REGION_VERSION) {
        blog(LOG_ERROR, "[feeds] shared memory wrong magic/version for '%s'",
             name.c_str());
        UnmapViewOfFile(data->view);
        CloseHandle(data->mapping);
        data->view = nullptr;
        data->mapping = nullptr;
        data->header = nullptr;
        data->frameSlots = nullptr;
        return;
    }

    data->lastReadIndex = data->header->write_index;

    blog(LOG_INFO, "[feeds] opened shared memory '%s' for source=%s",
         name.c_str(), data->uuid.c_str());

    StartPumpThread(data);
}

// clearTexture: if true, call obs_source_output_video(nullptr) to clear
// any lingering frame from display. Should be true when the user
// unsubscribes mid-session (so the source goes black) but false during
// source destruction (the source may be half-torn-down and touching it
// can crash — same bug class as STM v1.0.1).
static void CloseSharedMemory(ZpSourceData* data, bool clearTexture = true) {
    if (!data) return;

    std::lock_guard<std::mutex> lifecycleLock(data->lifecycleMutex);

    StopPumpThread(data);

    if (data->view)    { UnmapViewOfFile(data->view); data->view = nullptr; }
    if (data->mapping) { CloseHandle(data->mapping); data->mapping = nullptr; }
    data->header = nullptr;
    data->frameSlots  = nullptr;
    data->lastReadIndex = 0;

    // Clear the OBS source's current frame. Without this, OBS keeps
    // displaying the last frame we delivered, producing a frozen image
    // when the user unsubscribes mid-session. Skip during destruction
    // because the source is being torn down and touching it is unsafe.
    if (clearTexture && data->source) {
        obs_source_output_video(data->source, nullptr);
    }
}

// ---------------------------------------------------------------------------
// Menu handlers
// ---------------------------------------------------------------------------
void OnLoginClick() {
    if (g_isLoggedIn) {
        QMessageBox::information(
            static_cast<QWidget*>(obs_frontend_get_main_window()),
            QString::fromUtf8("Feeds - Login"),
            QString::fromUtf8("You are already logged in to Zoom."));
        return;
    }
    g_authInProgress = true;
    UpdateLoginLogoutMenuItem();
    feeds::SendToEngine("{\"type\":\"login_start\"}");
}

void OnLogoutClick() {
    if (!g_isLoggedIn) {
        QMessageBox::information(
            static_cast<QWidget*>(obs_frontend_get_main_window()),
            QString::fromUtf8("Feeds - Logout"),
            QString::fromUtf8("You are not currently logged in to Zoom."));
        return;
    }
    g_authInProgress = true;
    UpdateLoginLogoutMenuItem();
    feeds::SendToEngine("{\"type\":\"logout\"}");
}

// ---------------------------------------------------------------------------
// Connect-chooser tile support
//
// The Connect-to-Zoom chooser is a 2x2 launcher-style grid of icon tiles.
// Each tile is a rounded colored square holding a centered Tabler glyph, with
// a text label on the dialog background beneath it. The glyphs ship as bundled
// monochrome SVGs in data/icons (stroke="currentColor"); we recolor them per
// tile by substituting the stroke color and rasterizing with QSvgRenderer.
// ---------------------------------------------------------------------------

// Load a bundled SVG from the plugin data dir, recolor its `currentColor`
// stroke to `color`, and rasterize it to a `sizePx`-logical-pixel pixmap.
// Rendered at 2x for crispness on HiDPI displays. Returns a null pixmap if the
// asset is missing or unparseable — the tile just shows an empty square then.
static QPixmap LoadTintedIcon(const char* fileName, const QString& color,
                              int sizePx) {
    char* resolved = obs_module_file(fileName);
    if (!resolved) {
        blog(LOG_WARNING, "[feeds] chooser icon not found: %s", fileName);
        return QPixmap();
    }
    QFile f(QString::fromUtf8(resolved));
    bfree(resolved);
    if (!f.open(QIODevice::ReadOnly)) return QPixmap();
    QByteArray svg = f.readAll();
    f.close();
    svg.replace("currentColor", color.toUtf8());

    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) return QPixmap();

    const qreal dpr = 2.0;
    QPixmap pm(int(sizePx * dpr), int(sizePx * dpr));
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    renderer.render(&p);
    p.end();
    pm.setDevicePixelRatio(dpr);
    return pm;
}

// A launcher-style clickable tile: a rounded colored square with a centered
// icon, and a label on the dialog background beneath it. Hover lightens the
// square (plus a pointer cursor); press darkens it; releasing inside fires the
// click. The whole tile is the hit area — the child labels are transparent to
// mouse events so enter/leave/press/release all land on the tile itself.
//
// Deliberately NOT a Q_OBJECT: it carries no signals/slots, so it needs no moc
// pass and compiles cleanly inside this translation unit (which #undefs the Qt
// keyword macros). The click is delivered through a std::function instead.
class ConnectTile : public QWidget {
public:
    ConnectTile(const QString& labelText, const QPixmap& icon,
                const QString& baseColor, const QString& hoverColor,
                const QString& pressColor, std::function<void()> onClick,
                QWidget* parent = nullptr)
        : QWidget(parent),
          m_base(baseColor), m_hover(hoverColor), m_press(pressColor),
          m_onClick(std::move(onClick)) {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);

        m_square = new QLabel(this);
        m_square->setPixmap(icon);
        m_square->setAlignment(Qt::AlignCenter);
        m_square->setFixedSize(kSquare, kSquare);
        m_square->setAttribute(Qt::WA_TransparentForMouseEvents);
        ApplyColor(m_base);

        m_label = new QLabel(labelText, this);
        m_label->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        m_label->setWordWrap(true);
        m_label->setFixedWidth(kTileWidth);
        m_label->setStyleSheet("QLabel { color: #d6d8da; }");
        m_label->setAttribute(Qt::WA_TransparentForMouseEvents);
        // Reserve two text lines so all four tiles are the same height and the
        // colored squares stay row-aligned regardless of label wrapping.
        QFontMetrics fm(m_label->font());
        m_label->setFixedHeight(fm.height() * 2 + 4);

        QVBoxLayout* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(8);
        v->addWidget(m_square, 0, Qt::AlignHCenter);
        v->addWidget(m_label, 0, Qt::AlignHCenter | Qt::AlignTop);
    }

protected:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    void enterEvent(QEnterEvent*) override { ApplyColor(m_hover); }
#else
    void enterEvent(QEvent*) override { ApplyColor(m_hover); }
#endif
    void leaveEvent(QEvent*) override {
        m_pressed = false;
        ApplyColor(m_base);
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) {
            m_pressed = true;
            ApplyColor(m_press);
        }
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton || !m_pressed) return;
        m_pressed = false;
        const bool inside = rect().contains(e->pos());
        ApplyColor(inside ? m_hover : m_base);
        if (inside && m_onClick) m_onClick();
    }

private:
    void ApplyColor(const QString& c) {
        m_square->setStyleSheet(
            "QLabel { background-color: " + c +
            "; border-radius: 16px; }");
    }

    static constexpr int kSquare = 112;     // colored square edge, px
    static constexpr int kTileWidth = 132;  // tile/label width, px

    QLabel* m_square = nullptr;
    QLabel* m_label = nullptr;
    QString m_base, m_hover, m_press;
    std::function<void()> m_onClick;
    bool m_pressed = false;
};

void OnConnectClick() {
    if (!g_isLoggedIn) {
        g_pendingMeetingJoin = true;
        QMessageBox::information(
            static_cast<QWidget*>(obs_frontend_get_main_window()),
            QString::fromUtf8("Feeds - Login Required"),
            QString::fromUtf8(
                "You need to log in to Zoom first.\n\n"
                "Please log in and then try Connect to Zoom Meeting again."));
        OnLoginClick();
        return;
    }

    if (g_isInMeeting) {
        QMessageBox::information(
            static_cast<QWidget*>(obs_frontend_get_main_window()),
            QString::fromUtf8("Feeds - Already Connected"),
            QString::fromUtf8(
                "You are already connected to a Zoom meeting.\n\n"
                "Use the Leave button in the Zoom window to disconnect."));
        return;
    }

    QMainWindow* mainWindow = (QMainWindow*)obs_frontend_get_main_window();

    auto jsonEscape = [](const QString& s) -> std::string {
        std::string out;
        QByteArray utf8 = s.toUtf8();
        for (char ch : utf8) {
            unsigned char c = (unsigned char)ch;
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (c < 0x20) { /* drop */ }
                    else out += (char)c;
            }
        }
        return out;
    };

    // Icon-tile chooser — a 2x2 launcher-style grid of equal square tiles.
    // Top row is blue ("your own room, no permission prompt"): Instant +
    // PMI. Bottom row is grey (the rest): Join-by-number/link + Zoom Events.
    // The blue/grey split carries the meaning the old "Tip:" line used to.
    // Captured `choice` discriminates the downstream branches:
    // 1 = Create Instant Meeting, 2 = PMI, 3 = Join by Number or Link,
    // 4 = Zoom Events. (Behavior is unchanged from the old button chooser.)
    int choice = 0;
    {
        QDialog dlg(mainWindow);
        dlg.setWindowTitle("Connect to Zoom Meeting");

        // Tile palette. Blue marks the two host-your-own-room actions; grey
        // marks the rest. Hover lightens the fill, press darkens it — same
        // "obviously clickable" affordance the old stylesheet buttons gave.
        const QString kBlueBase  = "#3a6fe0";
        const QString kBlueHover = "#5285ea";
        const QString kBluePress = "#2f5cc0";
        const QString kGreyBase  = "#4a4f55";
        const QString kGreyHover = "#5c626a";
        const QString kGreyPress = "#3e4147";
        const QString kBlueIcon  = "#ffffff";
        const QString kGreyIcon  = "#d6d8da";
        const int kIconPx = 52;  // glyph size inside the 112px square

        QPixmap instantIcon = LoadTintedIcon("icons/video.svg",  kBlueIcon, kIconPx);
        QPixmap pmiIcon     = LoadTintedIcon("icons/user.svg",   kBlueIcon, kIconPx);
        QPixmap linkIcon    = LoadTintedIcon("icons/link.svg",   kGreyIcon, kIconPx);
        QPixmap eventsIcon  = LoadTintedIcon("icons/ticket.svg", kGreyIcon, kIconPx);

        ConnectTile* instantTile = new ConnectTile(
            "Instant Meeting", instantIcon, kBlueBase, kBlueHover, kBluePress,
            [&]() { choice = 1; dlg.accept(); }, &dlg);
        ConnectTile* pmiTile = new ConnectTile(
            "Personal Meeting (PMI)", pmiIcon, kBlueBase, kBlueHover, kBluePress,
            [&]() { choice = 2; dlg.accept(); }, &dlg);
        ConnectTile* linkTile = new ConnectTile(
            "Join by Number or Link", linkIcon, kGreyBase, kGreyHover, kGreyPress,
            [&]() { choice = 3; dlg.accept(); }, &dlg);
        ConnectTile* eventsTile = new ConnectTile(
            "Zoom Events", eventsIcon, kGreyBase, kGreyHover, kGreyPress,
            [&]() { choice = 4; dlg.accept(); }, &dlg);

        // 2x2 grid: both columns equal width so all four tiles align. No
        // centered-and-narrower bottom row like the old layout had.
        QGridLayout* grid = new QGridLayout();
        grid->setHorizontalSpacing(16);
        grid->setVerticalSpacing(14);
        grid->addWidget(instantTile, 0, 0);
        grid->addWidget(pmiTile,     0, 1);
        grid->addWidget(linkTile,    1, 0);
        grid->addWidget(eventsTile,  1, 1);

        QVBoxLayout* layout = new QVBoxLayout(&dlg);
        // SetFixedSize sizes the dialog to exactly the grid's sizeHint with no
        // leftover empty space.
        layout->setSizeConstraint(QLayout::SetFixedSize);
        layout->addLayout(grid);

        // Mouse-driven dialog: tiles take no keyboard focus (so none renders a
        // focus border that reads as a stuck hover). Esc-to-cancel still works
        // — that's handled by the QDialog itself, not the tiles.
        if (dlg.exec() != QDialog::Accepted || choice == 0) return;
    }

    // ----- Zoom Events branch -----
    if (choice == 4) {
        // Async: ask the engine for the user's events. The events_list reply
        // (handled on the IPC reader) pops the event picker, which leads to the
        // session picker and finally join_event_session. Gated by a pending
        // flag so a stray events_list can't pop a picker unprompted.
        g_eventsListPending = true;
        feeds::SendToEngine("{\"type\":\"request_events\"}");
        return;
    }

    // ----- Create Instant Meeting branch -----
    if (choice == 1) {
        // Single dialog with just the display-name field (prefilled). No
        // password — host doesn't enter their own meeting's password. No
        // meeting input — the engine provisions a fresh meeting via REST.
        QString instantName;
        {
            QDialog dlg(mainWindow);
            dlg.setWindowTitle("Create Instant Meeting");

            QLabel* nameLabel = new QLabel("Display name:", &dlg);
            QLineEdit* nameEdit = new QLineEdit(
                QString::fromStdString(g_userDisplayName), &dlg);

            QDialogButtonBox* buttons = new QDialogButtonBox(
                QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
            QObject::connect(buttons, &QDialogButtonBox::accepted,
                             &dlg, &QDialog::accept);
            QObject::connect(buttons, &QDialogButtonBox::rejected,
                             &dlg, &QDialog::reject);

            QVBoxLayout* layout = new QVBoxLayout(&dlg);
            layout->addWidget(nameLabel);
            layout->addWidget(nameEdit);
            layout->addWidget(buttons);

            if (dlg.exec() != QDialog::Accepted) return;
            instantName = nameEdit->text().trimmed();
        }

        // Set the flag BEFORE sending the IPC — engine's meeting_joined
        // reply could arrive on the read thread before SendToEngine even
        // returns here, so the flag must be visible by then. (Reader is
        // wired in commit 4.)
        g_pendingInstantMeeting = true;
        std::string msg = "{\"type\":\"create_instant_meeting\","
                          "\"display_name\":\""
                          + jsonEscape(instantName) + "\"}";
        feeds::SendToEngine(msg);
        if (g_connectAction) g_connectAction->setEnabled(false);
        return;
    }

    QString input;
    QString password;
    QString displayName;
    QString webinarToken;
    bool    isPmi = false;

    if (choice == 2) {
        if (g_userPMI.empty()) {
            QMessageBox::warning(
                static_cast<QWidget*>(obs_frontend_get_main_window()),
                QString::fromUtf8("Feeds"),
                QString::fromUtf8(
                    "Could not retrieve your Personal Meeting Room ID.\n"
                    "Please use Join by Meeting Number instead."));
            return;
        }
        input = QString::fromStdString(g_userPMI);
        isPmi = true;

        // PMI dialog: password + optional display-name override. Styled to
        // match the by-number-or-link dialog below for consistency.
        QDialog dlg(mainWindow);
        dlg.setWindowTitle("Join Personal Meeting Room");

        QLabel* pwdLabel = new QLabel(
            "Enter your PMI password (leave blank if none):", &dlg);
        QLineEdit* pwdEdit = new QLineEdit(&dlg);
        QLabel* nameLabel = new QLabel("Display name:", &dlg);
        QLineEdit* nameEdit = new QLineEdit(
            QString::fromStdString(g_userDisplayName), &dlg);

        QDialogButtonBox* buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        QObject::connect(buttons, &QDialogButtonBox::accepted,
                         &dlg, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected,
                         &dlg, &QDialog::reject);

        QVBoxLayout* layout = new QVBoxLayout(&dlg);
        layout->addWidget(pwdLabel);
        layout->addWidget(pwdEdit);
        layout->addWidget(nameLabel);
        layout->addWidget(nameEdit);
        layout->addWidget(buttons);

        if (dlg.exec() != QDialog::Accepted) return;
        password    = pwdEdit->text();
        displayName = nameEdit->text().trimmed();
    } else {
        // Single dialog with both fields visible so that pasting a Zoom
        // URL with a pwd= query parameter auto-fills the password field.
        QDialog dlg(mainWindow);
        dlg.setWindowTitle("Join Zoom Meeting");

        QLabel* meetingLabel = new QLabel(
            "Enter your Zoom Meeting number or link:", &dlg);
        QLineEdit* meetingEdit = new QLineEdit(&dlg);
        QLabel* pwdLabel = new QLabel(
            "Meeting password (leave blank if none):", &dlg);
        QLineEdit* pwdEdit = new QLineEdit(&dlg);
        QLabel* nameLabel = new QLabel("Display name:", &dlg);
        QLineEdit* nameEdit = new QLineEdit(
            QString::fromStdString(g_userDisplayName), &dlg);

        QDialogButtonBox* buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        QObject::connect(buttons, &QDialogButtonBox::accepted,
                         &dlg, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected,
                         &dlg, &QDialog::reject);

        // Auto-extract pwd= from a pasted Zoom URL. Cheap substring gate
        // first so we don't run QUrl on every keystroke for plain meeting
        // numbers. Overwrite the password field on success — the spec
        // explicitly prefers paste-clobbers-manual-edit since the more
        // likely flow is paste-URL → password just appears.
        auto extractPwd = [](const QString& text) -> QString {
            QString t = text.trimmed();
            if (!t.contains("zoom.us", Qt::CaseInsensitive)) return QString();
            if (!t.contains("pwd=", Qt::CaseInsensitive))    return QString();

            QUrl url = QUrl::fromUserInput(t);
            if (!url.isValid()) return QString();

            QUrlQuery q(url);
            if (q.hasQueryItem("pwd"))
                return q.queryItemValue("pwd", QUrl::FullyDecoded);

            // Some Zoom share links put pwd in the fragment (#pwd=...).
            QString frag = url.fragment();
            if (!frag.isEmpty()) {
                QUrlQuery fq(frag);
                if (fq.hasQueryItem("pwd"))
                    return fq.queryItemValue("pwd", QUrl::FullyDecoded);
            }
            return QString();
        };

        // Panelist invite URLs carry a tk= per-panelist registration token.
        // Silent extraction — no UI; the engine consumes it.
        auto extractTk = [](const QString& text) -> QString {
            QString t = text.trimmed();
            if (!t.contains("zoom.us", Qt::CaseInsensitive)) return QString();
            if (!t.contains("tk=", Qt::CaseInsensitive))     return QString();

            QUrl url = QUrl::fromUserInput(t);
            if (!url.isValid()) return QString();

            QUrlQuery q(url);
            if (q.hasQueryItem("tk"))
                return q.queryItemValue("tk", QUrl::FullyDecoded);

            QString frag = url.fragment();
            if (!frag.isEmpty()) {
                QUrlQuery fq(frag);
                if (fq.hasQueryItem("tk"))
                    return fq.queryItemValue("tk", QUrl::FullyDecoded);
            }
            return QString();
        };

        QObject::connect(meetingEdit, &QLineEdit::textChanged, &dlg,
            [pwdEdit, extractPwd, extractTk, &webinarToken](const QString& text) {
                QString pwd = extractPwd(text);
                if (!pwd.isEmpty()) pwdEdit->setText(pwd);
                QString tk = extractTk(text);
                if (!tk.isEmpty()) webinarToken = tk;
            });

        QVBoxLayout* layout = new QVBoxLayout(&dlg);
        layout->addWidget(meetingLabel);
        layout->addWidget(meetingEdit);
        layout->addWidget(pwdLabel);
        layout->addWidget(pwdEdit);
        layout->addWidget(nameLabel);
        layout->addWidget(nameEdit);
        layout->addWidget(buttons);

        if (dlg.exec() != QDialog::Accepted) return;
        input = meetingEdit->text().trimmed();
        if (input.isEmpty()) return;
        password    = pwdEdit->text();
        displayName = nameEdit->text().trimmed();
    }

    std::string msg = "{\"type\":\"join_meeting\","
                      "\"input\":\""         + jsonEscape(input)        + "\","
                      "\"password\":\""      + jsonEscape(password)     + "\","
                      "\"webinar_token\":\"" + jsonEscape(webinarToken) + "\","
                      "\"display_name\":\""  + jsonEscape(displayName)  + "\","
                      "\"is_pmi\":" + std::string(isPmi ? "true" : "false") + "}";
    feeds::SendToEngine(msg);

    if (g_connectAction) g_connectAction->setEnabled(false);
}

// ---------------------------------------------------------------------------
// Zoom Events pickers. Driven async from the IPC handlers (events_list /
// sessions_list) via QTimer::singleShot, so they always run on the Qt main
// thread. Each parses the normalized array the engine emitted and, on
// selection, sends the next IPC step.
// ---------------------------------------------------------------------------
static void ShowEventPickerDialog(const std::string& json) {
    QMainWindow* mainWindow = (QMainWindow*)obs_frontend_get_main_window();
    std::vector<std::string> objs =
        SplitJsonObjects(JsonExtractArrayBody(json, "events"));

    if (objs.empty()) {
        QMessageBox::information(static_cast<QWidget*>(mainWindow),
            QString::fromUtf8("Feeds - Zoom Events"),
            QString::fromUtf8(
                "No upcoming Zoom Events were found on your account."));
        return;
    }

    QDialog dlg(mainWindow);
    dlg.setWindowTitle("Zoom Events");

    QLabel* label = new QLabel("Select an event:", &dlg);
    QListWidget* list = new QListWidget(&dlg);
    // Elide long event titles on the right rather than forcing a horizontal
    // scrollbar; the full name lives in each item's tooltip (set below).
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setTextElideMode(Qt::ElideRight);
    for (const std::string& o : objs) {
        std::string id   = ExtractJsonString(o, "event_id");
        std::string name = ExtractJsonStringEscaped(o, "name");
        std::string role = ExtractJsonString(o, "role");
        if (name.empty()) name = "(unnamed event)";
        QString text = QString::fromStdString(name);
        if (role == "host")          text += "  [Host]";
        else if (role == "attendee") text += "  [Attendee]";
        QListWidgetItem* item = new QListWidgetItem(text, list);
        item->setData(Qt::UserRole, QString::fromStdString(id));
    }
    list->setCurrentRow(0);

    QDialogButtonBox* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dlg, &QDialog::accept);

    QVBoxLayout* layout = new QVBoxLayout(&dlg);
    layout->addWidget(label);
    layout->addWidget(list);
    layout->addWidget(buttons);

    // Open comfortably wide so event names aren't cut off. A resize (not a
    // fixed size) keeps the dialog user-resizable for the occasional very long
    // title; the minimum width stops it collapsing back to the cramped state.
    dlg.resize(580, 420);
    dlg.setMinimumWidth(480);

    if (dlg.exec() != QDialog::Accepted) return;
    QListWidgetItem* sel = list->currentItem();
    if (!sel) return;
    std::string eventId = sel->data(Qt::UserRole).toString().toStdString();
    if (eventId.empty()) return;

    g_eventsSessionsPending = true;
    feeds::SendToEngine("{\"type\":\"request_sessions\",\"event_id\":\"" +
                        JsonEscape(eventId) + "\"}");
}

static void ShowSessionPickerDialog(const std::string& json) {
    QMainWindow* mainWindow = (QMainWindow*)obs_frontend_get_main_window();
    std::string eventId = ExtractJsonString(json, "event_id");
    std::vector<std::string> objs =
        SplitJsonObjects(JsonExtractArrayBody(json, "sessions"));

    if (objs.empty()) {
        QMessageBox::information(static_cast<QWidget*>(mainWindow),
            QString::fromUtf8("Feeds - Zoom Events"),
            QString::fromUtf8("This event has no sessions you can join."));
        return;
    }

    QDialog dlg(mainWindow);
    dlg.setWindowTitle("Zoom Events - Sessions");

    QLabel* label = new QLabel("Select a session to join:", &dlg);
    QListWidget* list = new QListWidget(&dlg);
    // Elide long session labels on the right rather than forcing a horizontal
    // scrollbar; the full label lives in each item's tooltip (set below).
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setTextElideMode(Qt::ElideRight);
    for (const std::string& o : objs) {
        std::string id    = ExtractJsonString(o, "session_id");
        std::string name  = ExtractJsonStringEscaped(o, "name");
        std::string start = ExtractJsonStringEscaped(o, "start_time");
        long long   type  = ExtractJsonNumber(o, "type");
        if (name.empty()) name = "(unnamed session)";
        // type: 0 = meeting, 2 = webinar, 4 = neither.
        const char* typeStr = (type == 0) ? "Meeting"
                            : (type == 2) ? "Webinar" : "Session";
        QString text = QString::fromStdString(name);
        if (!start.empty()) text += "  —  " + QString::fromStdString(start);
        text += QString("  (") + typeStr + ")";
        QListWidgetItem* item = new QListWidgetItem(text, list);
        item->setData(Qt::UserRole, QString::fromStdString(id));
    }
    list->setCurrentRow(0);

    QDialogButtonBox* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dlg, &QDialog::accept);

    QVBoxLayout* layout = new QVBoxLayout(&dlg);
    layout->addWidget(label);
    layout->addWidget(list);
    layout->addWidget(buttons);

    // Open comfortably wide so session labels aren't cut off. A resize (not a
    // fixed size) keeps the dialog user-resizable for the occasional very long
    // title; the minimum width stops it collapsing back to the cramped state.
    dlg.resize(580, 420);
    dlg.setMinimumWidth(480);

    if (dlg.exec() != QDialog::Accepted) return;
    QListWidgetItem* sel = list->currentItem();
    if (!sel) return;
    std::string sessionId = sel->data(Qt::UserRole).toString().toStdString();
    if (sessionId.empty() || eventId.empty()) return;

    // Mirrors the logged-in join_meeting path: fire the join and disable
    // Connect until meeting_joined / meeting_failed comes back. (We don't set
    // g_pendingMeetingJoin — that's only for joins deferred until login
    // completes; here the user is already authenticated.) The engine fetches
    // the just-in-time join token and joins by token.
    std::string msg = "{\"type\":\"join_event_session\","
                      "\"event_id\":\""     + JsonEscape(eventId)          + "\","
                      "\"session_id\":\""   + JsonEscape(sessionId)        + "\","
                      "\"display_name\":\"" + JsonEscape(g_userDisplayName) + "\"}";
    feeds::SendToEngine(msg);
    if (g_connectAction) g_connectAction->setEnabled(false);
}

// Chat-message origin, stored per row as kRoleChatOrigin so the delegate can
// draw a platform glyph and OnMessageClicked can gate platform-specific actions
// (Phase 1: YouTube rows open no popup). File-scope (not inside FeedsChatDock)
// because ChatMessageDelegate — defined below, before the dock — reads the role.
// Placeholder rows carry no origin (role absent) and get no glyph/indent.
static constexpr int kRoleChatOrigin  = Qt::UserRole + 10;  // int: kChatOrigin*
static constexpr int kRoleChatMsgId   = Qt::UserRole + 11;  // platform message id (deletion): YT id / Twitch id tag
static constexpr int kRoleChatChannel = Qt::UserRole + 12;  // by-author id: YT channel id / Twitch user-id
static constexpr int kRoleChatAvatar  = Qt::UserRole + 13;  // avatar URL (YouTube only; Twitch has none)
enum ChatOrigin { kChatOriginZoom = 1, kChatOriginYouTube = 2, kChatOriginTwitch = 3 };

// Avatar resolvers (defined further down: Zoom's uint-keyed cache / YouTube's
// channel-id-keyed cache). Forward-declared here because the dock's click
// handler — above their definitions — resolves an avatar to hand to the popup.
static QImage GetAvatarForSender(unsigned int senderId, const std::string& avatarPath);
static QImage YtResolveAvatar(const std::string& channelId);
static QImage TwResolveAvatar(const std::string& userId);

// Draw a small, code-rendered origin glyph in `box` (no embedded brand logos —
// trademark): YouTube = red rounded rect + white play triangle; Twitch = purple
// rounded rect + white speech bubble; Zoom = blue rounded rect + white dot. Each
// platform is marked with a generic shape in its brand colour, consistently.
static void DrawChatOriginGlyph(QPainter* p, const QRectF& box, int origin) {
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setPen(Qt::NoPen);
    if (origin == kChatOriginYouTube) {
        p->setBrush(QColor(0xE6, 0x2b, 0x1e));               // YouTube-ish red
        p->drawRoundedRect(box, box.height() * 0.28, box.height() * 0.28);
        const qreal w = box.width(), h = box.height();
        const QPointF tri[3] = {
            QPointF(box.left() + w * 0.40, box.top() + h * 0.30),
            QPointF(box.left() + w * 0.40, box.top() + h * 0.70),
            QPointF(box.left() + w * 0.68, box.top() + h * 0.50),
        };
        p->setBrush(Qt::white);
        p->drawPolygon(tri, 3);
    } else if (origin == kChatOriginTwitch) {
        p->setBrush(QColor(0x91, 0x46, 0xFF));               // Twitch-ish purple
        p->drawRoundedRect(box, box.height() * 0.28, box.height() * 0.28);
        // Generic white speech bubble (rounded body + a small tail at lower-left)
        // — no real logo, same trademark rule as YouTube/Zoom.
        const qreal w = box.width(), h = box.height();
        QRectF body(box.left() + w * 0.24, box.top() + h * 0.26,
                    w * 0.52, h * 0.38);
        p->setBrush(Qt::white);
        p->drawRoundedRect(body, h * 0.10, h * 0.10);
        const QPointF tail[3] = {
            QPointF(box.left() + w * 0.38, box.top() + h * 0.60),
            QPointF(box.left() + w * 0.38, box.top() + h * 0.78),
            QPointF(box.left() + w * 0.54, box.top() + h * 0.62),
        };
        p->drawPolygon(tail, 3);
    } else {  // Zoom (and any other non-marked origin)
        p->setBrush(QColor(0x2D, 0x8C, 0xFF));               // Zoom-ish blue
        p->drawRoundedRect(box, box.height() * 0.28, box.height() * 0.28);
        p->setBrush(Qt::white);
        const qreal d = box.height() * 0.34;
        p->drawEllipse(box.center(), d * 0.5, d * 0.5);
    }
    p->restore();
}

// Horizontal space (glyph + gap) reserved at the left of a marked row.
static constexpr int kChatGlyphSize   = 14;
static constexpr int kChatGlyphIndent = kChatGlyphSize + 6;

// Render a chat-origin glyph to a transparent pixmap for use as a QLabel icon in
// the dock header (reuses the same code-drawn glyph the message rows use).
static QPixmap MakeChatOriginPixmap(int origin, int size) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    DrawChatOriginGlyph(&p, QRectF(0, 0, size, size), origin);
    p.end();
    return pm;
}

// A platform poller's raw connection state, reported to the chat dock header.
// Off = no target; Waiting = target set but not connected (not-live / connecting
// / backoff / transient failure — the self-healing loops make failures
// indistinguishable, so there's no separate "Error"); Live = connected. Shared by
// the YouTube and Twitch rows; the header's ResolveRowStatus turns this plus the
// account state into the final label (Off / Log In / Upgrade / Waiting /
// Connected — the displayed word for kChatConnLive says "Connected" because the
// chat socket being up doesn't mean the stream is on air).
enum { kChatConnOff = 0, kChatConnWaiting = 1, kChatConnLive = 2 };

// YouTube target: a channel handle (resolve via /@handle/live) or a pinned video
// id (bootstrap /watch?v=<id> directly). Declared here — the chat dock's header
// (below) parses field input via NormalizeYouTubeTarget and commits it via
// YtSetTargetFromInput; both are defined with the poller further down, keeping
// the dock free of the poller's globals/config.
struct YtTarget {
    bool        isVideo = false;
    std::string value;            // bare handle (no '@') or 11-char video id
};
static YtTarget    NormalizeYouTubeTarget(std::string s);
static std::string YtSetTargetFromInput(const std::string& input);

// Twitch target: a bare lowercase login name. The chat dock's header commits its
// field input via TwSetTargetFromInput (normalize + persist + wake), defined with
// the Twitch poller further down; NormalizeTwitchChannel is exposed so the header
// can preview the normalized form. Empty = off (same model as YouTube).
static std::string NormalizeTwitchChannel(std::string s);
static std::string TwSetTargetFromInput(const std::string& input);

// Format a Zoom meeting number the way Zoom does: 3-3-4 groups ("5106269156" ->
// "510 626 9156"). Non-10-digit lengths group in threes/threes/four and let any
// remainder trail as a final group.
static QString FormatMeetingNumber(unsigned long long n) {
    const QString d = QString::number(n);
    const int len = (int)d.size();
    static const int cuts[] = { 3, 3, 4 };
    QString out;
    int pos = 0;
    for (int c : cuts) {
        if (pos >= len) break;
        int take = (len - pos < c) ? (len - pos) : c;
        if (!out.isEmpty()) out += ' ';
        out += d.mid(pos, take);
        pos += take;
    }
    if (pos < len) { out += ' '; out += d.mid(pos); }  // remainder trails
    return out;
}

// ---------------------------------------------------------------------------
// Item delegate for the chat dock's QListWidget. setWordWrap(true) alone
// handles soft-wrapping at whitespace but breaks down on long unbreakable
// tokens (URLs without spaces, hashes, "ooooo..." spam): the viewport
// widens to fit the token, a horizontal scrollbar appears, and prior
// wrapped messages re-flow to the new width.
//
// QTextLayout with WrapAtWordBoundaryOrAnywhere fixes that: wrap at word
// boundaries by default, fall back to mid-token breaks only when no word
// boundary fits inside the available width. sizeHint and paint share the
// same LayoutLines helper so reported heights match what we actually draw.
//
// Width comes from option.rect.width(), which the view keeps in sync with
// viewport-minus-scrollbar thanks to setResizeMode(QListView::Adjust) on
// the list.
// ---------------------------------------------------------------------------
class ChatMessageDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override {
        int width = option.rect.width();
        if (width <= 0) {
            // First-layout pass — the view hasn't assigned a width yet.
            // Fall back to the base implementation; the next layout
            // pass will re-call us with a real width.
            return QStyledItemDelegate::sizeHint(option, index);
        }

        // Marked rows (Zoom/YouTube) reserve a left indent for the origin glyph;
        // placeholder rows (no origin) use the full width.
        const bool marked = index.data(kRoleChatOrigin).isValid();
        const int textW = marked ? (width - kChatGlyphIndent) : width;

        QString text = index.data(Qt::DisplayRole).toString();
        QTextLayout layout(text, option.font);
        layout.setTextOption(MakeTextOption());

        qreal y = LayoutLines(layout, textW > 0 ? textW : width);
        // +2 px so adjacent items aren't visually flush.
        return QSize(width, static_cast<int>(y) + 2);
    }

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();

        if (option.state & QStyle::State_Selected) {
            painter->fillRect(option.rect, option.palette.highlight());
            painter->setPen(option.palette.highlightedText().color());
        } else {
            painter->setPen(option.palette.text().color());
        }

        // Origin glyph + text indent for marked rows; placeholders draw flush.
        const QVariant originVar = index.data(kRoleChatOrigin);
        QPointF textOrigin = option.rect.topLeft();
        int textW = option.rect.width();
        if (originVar.isValid()) {
            QRectF glyphBox(option.rect.left() + 2, option.rect.top() + 2.0,
                            kChatGlyphSize, kChatGlyphSize);
            DrawChatOriginGlyph(painter, glyphBox, originVar.toInt());
            textOrigin.setX(textOrigin.x() + kChatGlyphIndent);
            textW -= kChatGlyphIndent;
        }

        QString text = index.data(Qt::DisplayRole).toString();
        QTextLayout layout(text, option.font);
        layout.setTextOption(MakeTextOption());

        LayoutLines(layout, textW > 0 ? textW : option.rect.width());
        layout.draw(painter, textOrigin);

        painter->restore();
    }

private:
    static QTextOption MakeTextOption() {
        QTextOption opt;
        opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        opt.setAlignment(Qt::AlignLeft | Qt::AlignTop);
        return opt;
    }

    // Lays out lines into `layout` at the given width; returns total
    // text height. Shared by sizeHint and paint so both compute against
    // the same wrap rules.
    static qreal LayoutLines(QTextLayout& layout, int width) {
        layout.beginLayout();
        qreal y = 0;
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) break;
            line.setLineWidth(width);
            line.setPosition(QPointF(0, y));
            y += line.height();
        }
        layout.endLayout();
        return y;
    }
};

// ---------------------------------------------------------------------------
// Zoom Chat dock. Phase 1: register a dock that displays placeholder content.
// Commit 3 wires the chat_message IPC from the engine into this widget so it
// reflects live meeting chat. Once handed to obs_frontend_add_dock_by_id,
// OBS owns the widget's lifetime — no destruction logic needed here.
// ---------------------------------------------------------------------------
class FeedsChatDock : public QWidget {
public:
    // Custom QListWidgetItem data roles. Each dock item stores the
    // sender_id, name, and content as item data so OnMessageClicked can
    // recover them without re-parsing the rendered label.
    static constexpr int RoleSenderId   = Qt::UserRole + 1;
    static constexpr int RoleSenderName = Qt::UserRole + 2;
    static constexpr int RoleContent    = Qt::UserRole + 3;

    // Sticky-bottom tolerance: a scroll value "within this many px of the
    // maximum" counts as at-bottom, so normal reading-the-latest stays pinned
    // but any deliberate scroll-up breaks the stick.
    static constexpr int kStickyBottomTolerancePx = 8;
    // Retained-row cap across all platforms (a unified display cap, not
    // per-platform). Oldest rows trim as new ones arrive.
    static constexpr int kMaxChatRows = 500;

    explicit FeedsChatDock(QWidget* parent = nullptr) : QWidget(parent) {
        m_list = new QListWidget(this);
        m_list->addItem(CurrentPlaceholderText());

        // Word wrap — without these three, long messages render on one
        // line with a horizontal scrollbar. setResizeMode(Adjust) makes
        // item heights recompute when the dock is resized; disabling
        // uniform item sizes lets each item have its own wrapped height.
        // setWordWrap remains as a fallback if the custom delegate is
        // ever bypassed; the delegate is what enforces mid-token breaks
        // for unbreakable strings (long URLs, hashes, etc.).
        m_list->setWordWrap(true);
        m_list->setResizeMode(QListView::Adjust);
        m_list->setUniformItemSizes(false);
        m_list->setItemDelegate(new ChatMessageDelegate(m_list));

        QObject::connect(m_list, &QListWidget::itemClicked,
                         this, [this](QListWidgetItem* item) {
                             OnMessageClicked(item);
                         });

        // Pixel-granular scrolling so the sticky-bottom tolerance and the
        // resize/relayout preservation below can reason in pixels — the
        // default per-item mode makes "near the bottom" coarse and jumpy
        // with variable-height wrapped rows.
        m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

        // Floating "new messages" pill — parented to the viewport so it hovers
        // over the bottom of the list without taking part in the layout. Styled
        // to match the dock's subtle grey chrome (rgba greys + thin border, as
        // in the participant header bar). Hidden until a message arrives while
        // the user is scrolled up.
        m_newMsgPill = new QPushButton(
            QString::fromUtf8("\xE2\x86\x93 New messages"), m_list->viewport());
        m_newMsgPill->setCursor(Qt::PointingHandCursor);
        m_newMsgPill->setFocusPolicy(Qt::NoFocus);
        m_newMsgPill->setStyleSheet(
            "QPushButton {"
            " background: rgba(40,40,40,0.92);"
            " color: #d6d8da;"
            " border: 1px solid rgba(128,128,128,0.35);"
            " border-radius: 10px;"
            " padding: 3px 12px; }"
            "QPushButton:hover { background: rgba(64,64,64,0.95); }");
        m_newMsgPill->hide();
        QObject::connect(m_newMsgPill, &QPushButton::clicked, this, [this]() {
            m_list->scrollToBottom();
            HideNewMessagesPill();
        });

        // Reposition the pill on viewport resize and preserve the reading
        // position across the width-driven word-wrap reflow (see eventFilter).
        m_list->viewport()->installEventFilter(this);

        // Dismiss the pill the moment the view is back at the bottom — whether
        // via the pill, a manual drag, or a wheel scroll.
        QObject::connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged,
                         this, [this](int) {
                             if (IsAtBottom()) HideNewMessagesPill();
                         });

        m_input = new QLineEdit(this);
        m_input->setPlaceholderText(QString::fromUtf8("Message Zoom chat…"));
        m_sendBtn = new QPushButton("Send", this);

        // Disabled until we're in a meeting. OnMeetingJoined/OnMeetingLeft
        // toggle these based on engine state.
        m_input->setEnabled(false);
        m_sendBtn->setEnabled(false);

        QObject::connect(m_input, &QLineEdit::returnPressed,
                         this, [this]() { SendCurrentMessage(); });
        QObject::connect(m_sendBtn, &QPushButton::clicked,
                         this, [this]() { SendCurrentMessage(); });

        QHBoxLayout* inputRow = new QHBoxLayout();
        inputRow->setContentsMargins(6, 6, 6, 6);
        inputRow->setSpacing(6);
        inputRow->addWidget(m_input);
        inputRow->addWidget(m_sendBtn);

        QVBoxLayout* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        m_chatHeader = BuildChatHeader();   // platform config/status, pinned on top
        layout->addWidget(m_chatHeader);
        layout->addWidget(m_list, 1);
        layout->addLayout(inputRow);

        setMinimumSize(200, 300);

        UpdateYouTubeStatus(kChatConnOff, QString());
        UpdateTwitchStatus(kChatConnOff);
        UpdateChatHeaderState();
    }

    // --- Sticky-bottom scroll helpers ---------------------------------------

    // True when the view is at (or within a few px of) the bottom. Read BEFORE
    // appending a row, since adding a row raises the scrollbar maximum. With
    // content shorter than the viewport, maximum is 0 and this is trivially
    // true, so the first messages pin to the bottom.
    bool IsAtBottom() const {
        QScrollBar* sb = m_list->verticalScrollBar();
        if (!sb) return true;
        return sb->value() >= sb->maximum() - kStickyBottomTolerancePx;
    }

    void ShowNewMessagesPill() {
        if (!m_newMsgPill) return;
        PositionNewMessagesPill();
        m_newMsgPill->show();
        m_newMsgPill->raise();
    }
    void HideNewMessagesPill() {
        if (m_newMsgPill) m_newMsgPill->hide();
    }

    // Centre the pill horizontally, a small margin above the viewport bottom.
    void PositionNewMessagesPill() {
        if (!m_newMsgPill) return;
        QWidget* vp = m_list->viewport();
        m_newMsgPill->adjustSize();
        int x = (vp->width()  - m_newMsgPill->width())  / 2;
        int y =  vp->height() - m_newMsgPill->height() - 8;
        m_newMsgPill->move(x < 0 ? 0 : x, y < 0 ? 0 : y);
    }

    // Enforce the retained-row cap by trimming the oldest rows. When the user
    // is scrolled up, subtract the removed rows' height from the scroll value
    // so the messages they're reading don't shift under them. When at the
    // bottom the caller re-pins with scrollToBottom(), so no adjustment needed.
    void TrimToCap(bool atBottom) {
        const int over = m_list->count() - kMaxChatRows;
        if (over <= 0) return;
        if (atBottom) {
            for (int i = 0; i < over; ++i) delete m_list->takeItem(0);
            return;
        }
        QScrollBar* sb = m_list->verticalScrollBar();
        int removedH = 0;
        for (int i = 0; i < over; ++i) removedH += m_list->sizeHintForRow(i);
        const int oldVal = sb ? sb->value() : 0;
        for (int i = 0; i < over; ++i) delete m_list->takeItem(0);
        if (sb) { int nv = oldVal - removedH; sb->setValue(nv < 0 ? 0 : nv); }
    }

    // Shared tail of the three platform append paths: gate scroll-to-bottom on
    // whether the view was already at the bottom, enforce the row cap, and
    // surface the "new messages" pill when a message lands while scrolled up.
    void AppendChatItem(QListWidgetItem* item) {
        const bool atBottom = IsAtBottom();
        m_list->addItem(item);
        TrimToCap(atBottom);
        if (atBottom) {
            m_list->scrollToBottom();
            HideNewMessagesPill();
        } else {
            ShowNewMessagesPill();
        }
    }

    // All methods below must run on the Qt main thread. The IPC
    // handlers marshal via QTimer::singleShot onto the main window before
    // invoking them.

    void AppendMessage(unsigned int   senderId,
                       const QString& senderName,
                       const QString& content,
                       qint64         timestamp) {
        // timestamp is kept in the signature so the engine's value
        // flows through unchanged — future surfaces (tooltip, an
        // opt-in "show timestamps" preference, the overlay source)
        // can pick it up without an IPC change.
        (void)timestamp;

        // Defensive — the IPC handler already drops messages on tier < 1,
        // but if SetTierDisabled fires after a message is queued for the
        // UI thread we'd want to swallow it here too. Belt-and-braces.
        if (m_tierDisabled) return;

        if (!m_messagesStarted) {
            // First real message — drop the placeholder. From here on the
            // dock shows only real chat history for the rest of the OBS
            // session. We don't re-add the placeholder after a meeting
            // ends; preserving history across meetings is the Phase 1
            // behavior per spec.
            m_list->clear();
            m_messagesStarted = true;
        }
        QListWidgetItem* item =
            new QListWidgetItem(QString("%1: %2").arg(senderName, content));
        item->setData(RoleSenderId,   senderId);
        item->setData(RoleSenderName, senderName);
        item->setData(RoleContent,    content);
        item->setData(kRoleChatOrigin, kChatOriginZoom);   // blue Zoom glyph
        AppendChatItem(item);
    }

    // Inject a YouTube live-chat message into the dock. Phase 1 is dock-only:
    // this deliberately does NOT touch the overlay/popup surfaces (the poller
    // never calls AppendChatMessageToOverlay, and OnMessageClicked opens no
    // popup for a YouTube row). Tier gating mirrors the Zoom path exactly —
    // the poller skips on tier < 1, and m_tierDisabled is the belt-and-braces
    // guard here. msgId / channelId / avatarUrl are stored for Phase 2
    // (deletion needs the ids, avatar download needs the URL) but unused now.
    void AppendYouTubeMessage(const QString& author,
                              const QString& content,
                              const QString& msgId,
                              const QString& channelId,
                              const QString& avatarUrl) {
        if (m_tierDisabled) return;
        if (!m_messagesStarted) { m_list->clear(); m_messagesStarted = true; }

        QListWidgetItem* item =
            new QListWidgetItem(QString("%1: %2").arg(author, content));
        item->setData(RoleSenderName,  author);
        item->setData(RoleContent,     content);
        item->setData(kRoleChatOrigin, kChatOriginYouTube);  // red YouTube glyph
        item->setData(kRoleChatMsgId,  msgId);
        item->setData(kRoleChatChannel, channelId);
        item->setData(kRoleChatAvatar, avatarUrl);
        // Note: no RoleSenderId — YouTube has no Zoom uint id; OnMessageClicked
        // keys off kRoleChatOrigin, not the presence of RoleSenderId.
        AppendChatItem(item);
    }

    // Moderator deletion of a single YouTube message (by message id). Removes the
    // matching row(s) and pulls the message off the popup if it's showing. Zoom
    // rows are never matched (origin + YT-only roles).
    void RemoveYouTubeMessageById(const QString& msgId) {
        RemoveRowsMatching(kChatOriginYouTube, kRoleChatMsgId, msgId);
    }

    // Moderator ban / author deletion (by channel id) — remove all of that
    // author's YouTube rows and clear any of them from the popup.
    void RemoveYouTubeMessagesByChannel(const QString& channelId) {
        RemoveRowsMatching(kChatOriginYouTube, kRoleChatChannel, channelId);
    }

    // Inject a Twitch IRC message into the dock. Mirrors AppendYouTubeMessage:
    // dock is Basic+ (m_tierDisabled belt-and-braces), stores the same roles so
    // deletion (CLEARMSG by msg id / CLEARCHAT by user id) and the click-to-popup
    // path work the platform-generic way. No avatar (Twitch IRC carries none), so
    // no kRoleChatAvatar; the popup renders its neutral circle. userId goes in the
    // by-author role so a CLEARCHAT purge can find all of a user's rows.
    void AppendTwitchMessage(const QString& author,
                             const QString& content,
                             const QString& msgId,
                             const QString& userId) {
        if (m_tierDisabled) return;
        if (!m_messagesStarted) { m_list->clear(); m_messagesStarted = true; }

        QListWidgetItem* item =
            new QListWidgetItem(QString("%1: %2").arg(author, content));
        item->setData(RoleSenderName,  author);
        item->setData(RoleContent,     content);
        item->setData(kRoleChatOrigin, kChatOriginTwitch);  // purple Twitch glyph
        item->setData(kRoleChatMsgId,  msgId);
        item->setData(kRoleChatChannel, userId);
        AppendChatItem(item);
    }

    // Twitch CLEARMSG (single message removed by id).
    void RemoveTwitchMessageById(const QString& msgId) {
        RemoveRowsMatching(kChatOriginTwitch, kRoleChatMsgId, msgId);
    }

    // Twitch CLEARCHAT for one user (timeout/ban) — remove all of that user's rows.
    void RemoveTwitchMessagesByUser(const QString& userId) {
        RemoveRowsMatching(kChatOriginTwitch, kRoleChatChannel, userId);
    }

    // Twitch CLEARCHAT with no target user — the whole chat was cleared. Drop every
    // Twitch row (leaving Zoom/YouTube history intact) and clear any Twitch message
    // showing in the popup.
    void RemoveAllTwitchMessages() {
        for (int i = m_list->count() - 1; i >= 0; --i) {
            QListWidgetItem* it = m_list->item(i);
            if (!it || it->data(kRoleChatOrigin).toInt() != kChatOriginTwitch)
                continue;
            const std::string content = it->data(RoleContent).toString().toStdString();
            delete m_list->takeItem(i);
            feeds::ClearChatPopupIfMatches(0, content);
        }
    }

    void OnMeetingJoined() {
        // Free tier: the dock stays locked even after a meeting connects.
        // Leave the upgrade-prompt placeholder in place and keep the input
        // disabled. Without this guard a Free user gets a working send box
        // and a "Connected" message on join.
        if (m_tierDisabled) {
            return;
        }
        SetPlaceholder("Connected");
        m_input->setEnabled(true);
        m_sendBtn->setEnabled(true);
        UpdateChatHeaderState();   // Zoom row -> "In meeting (#N)"
    }
    void OnMeetingLeft() {
        SetPlaceholder(CurrentPlaceholderText());
        m_input->setEnabled(false);
        m_sendBtn->setEnabled(false);
        UpdateChatHeaderState();   // Zoom row -> "Not connected"
        // Hide any active popup and drop the overlay history so stale
        // messages don't carry into the next meeting.
        feeds::ClearChatPopup();
        feeds::ClearChatOverlay();
    }

    // Called from the IPC handlers when login state changes, so the
    // placeholder reflects "click to login" vs "click to connect" as
    // the user transitions between states without needing to leave a
    // meeting first. No-op once chat history is showing.
    void RefreshPlaceholder() {
        UpdateChatHeaderState();   // login/meeting state changed -> refresh Zoom row
        if (m_messagesStarted) return;
        SetPlaceholder(CurrentPlaceholderText());
    }

    // Toggle the dock between "normal" and "tier-locked" states.
    // Called from ReconcileSourcesToTier on login_succeeded. Entering
    // the locked state clears any accumulated chat history (so a Free
    // user picking up a shared OBS doesn't see a paid user's history)
    // and swaps the placeholder to the upgrade prompt. Exiting the
    // locked state restores the normal placeholder; input stays
    // disabled until OnMeetingJoined fires from a real connect.
    void SetTierDisabled(bool disabled) {
        if (m_tierDisabled == disabled) return;
        m_tierDisabled = disabled;

        if (disabled) {
            m_messagesStarted = false;
            m_list->clear();
            m_input->clear();
            m_input->setEnabled(false);
            m_sendBtn->setEnabled(false);
        }
        SetPlaceholder(CurrentPlaceholderText());
        UpdateChatHeaderState();   // show/hide header per tier lock
    }

    bool IsTierDisabled() const { return m_tierDisabled; }

    // --- YouTube header, driven by the poller (all called on the UI thread) ----

    // YouTube row status: store the poller's reported connection state (+ the Live
    // video-id detail) and re-resolve the row against the current account state.
    // Marshaled from YtReportStatus.
    void UpdateYouTubeStatus(int status, const QString& videoId) {
        m_ytConn = status;
        m_ytLiveDetail = videoId.isEmpty()
            ? QString("Connected")
            : QString("Connected \xE2\x80\x94 video %1").arg(videoId);
        RefreshYouTubeRowStatus();
    }

    // Twitch row status: store the poller's reported connection state and
    // re-resolve. Marshaled from TwReportStatus.
    void UpdateTwitchStatus(int status) {
        m_twConn = status;
        RefreshTwitchRowStatus();
    }

    // Correct the field to the canonical owner handle casing on connect (display
    // only — NOT the poller's working handle, whose change would spuriously
    // re-bootstrap). Skips if the user is editing or has since changed the target.
    void UpdateYouTubeHandleText(const QString& canonicalHandle) {
        if (!m_ytField || m_ytField->hasFocus()) return;
        YtTarget t = NormalizeYouTubeTarget(m_ytField->text().toStdString());
        if (t.isVideo) return;
        if (QString::fromStdString(t.value).compare(canonicalHandle,
                                                    Qt::CaseInsensitive) != 0)
            return;
        QString disp = "@" + canonicalHandle;
        if (m_ytField->text() == disp) return;
        QSignalBlocker block(m_ytField);
        m_ytField->setText(disp);
    }

    // Populate the field from persisted config at startup (FINISHED_LOADING).
    void SetYouTubeTargetDisplay(const std::string& handle,
                                 const std::string& videoId) {
        if (!m_ytField) return;
        QString disp = !videoId.empty()
            ? QString::fromStdString(videoId)
            : (handle.empty() ? QString() : QString::fromStdString("@" + handle));
        QSignalBlocker block(m_ytField);
        m_ytField->setText(disp);
    }

    // Populate the Twitch field from persisted config at startup. Login names are
    // already canonical lowercase, so no casing-correction path is needed.
    void SetTwitchTargetDisplay(const std::string& channel) {
        if (!m_twField) return;
        QSignalBlocker block(m_twField);
        m_twField->setText(QString::fromStdString(channel));
    }

private:
    // Build the fixed platform header: a read-only Zoom status row and an
    // editable YouTube row (glyph + field + status dot + word). Compact — a
    // couple of chat lines tall. Row glyphs reuse DrawChatOriginGlyph (icon
    // labels; no text labels needed). Twitch/TikTok rows are intentionally not
    // built until those platforms ship.
    QWidget* BuildChatHeader() {
        QWidget* header = new QWidget(this);
        QVBoxLayout* hv = new QVBoxLayout(header);
        hv->setContentsMargins(8, 6, 8, 6);
        hv->setSpacing(4);

        // Zoom row — read-only status (connect flows stay in the list placeholder).
        QHBoxLayout* zoomRow = new QHBoxLayout();
        zoomRow->setSpacing(6);
        QLabel* zoomGlyph = new QLabel(header);
        zoomGlyph->setPixmap(MakeChatOriginPixmap(kChatOriginZoom, kChatGlyphSize));
        zoomGlyph->setFixedSize(kChatGlyphSize, kChatGlyphSize);
        zoomGlyph->setToolTip("Zoom chat");
        m_zoomStatus = new QLabel(header);
        // Clickable (not restyled as a button) — routes to the shared
        // click-to-connect flow, same as the message-list placeholder. Cursor is
        // set per meeting-state in UpdateChatHeaderState.
        m_zoomStatus->installEventFilter(this);
        zoomRow->addWidget(zoomGlyph);
        zoomRow->addWidget(m_zoomStatus, 1);
        hv->addLayout(zoomRow);

        // YouTube row — editable target + live status.
        QHBoxLayout* ytRow = new QHBoxLayout();
        ytRow->setSpacing(6);
        QLabel* ytGlyph = new QLabel(header);
        ytGlyph->setPixmap(MakeChatOriginPixmap(kChatOriginYouTube, kChatGlyphSize));
        ytGlyph->setFixedSize(kChatGlyphSize, kChatGlyphSize);
        ytGlyph->setToolTip("YouTube chat");
        m_ytField = new QLineEdit(header);
        m_ytField->setPlaceholderText("Enter YouTube Channel or Link");
        m_ytDot = new QLabel(header);
        m_ytDot->setFixedSize(10, 10);
        m_ytWord = new QLabel(header);
        // Clickable status word (Log In / Upgrade states) — routes to the shared
        // connect flow, same unstyled-label pattern as the Zoom row. The cursor +
        // whether a click acts is driven by ResolveRowStatus via m_ytWordClickable.
        m_ytWord->installEventFilter(this);
        ytRow->addWidget(ytGlyph);
        ytRow->addWidget(m_ytField, 1);
        ytRow->addWidget(m_ytDot);
        ytRow->addWidget(m_ytWord);
        hv->addLayout(ytRow);

        QObject::connect(m_ytField, &QLineEdit::editingFinished, this,
                         [this]() { CommitYouTubeField(); });

        // Twitch row — editable channel login + live status (mirrors YouTube).
        QHBoxLayout* twRow = new QHBoxLayout();
        twRow->setSpacing(6);
        QLabel* twGlyph = new QLabel(header);
        twGlyph->setPixmap(MakeChatOriginPixmap(kChatOriginTwitch, kChatGlyphSize));
        twGlyph->setFixedSize(kChatGlyphSize, kChatGlyphSize);
        twGlyph->setToolTip("Twitch chat");
        m_twField = new QLineEdit(header);
        m_twField->setPlaceholderText("Enter Twitch Channel");
        m_twDot = new QLabel(header);
        m_twDot->setFixedSize(10, 10);
        m_twWord = new QLabel(header);
        m_twWord->installEventFilter(this);
        twRow->addWidget(twGlyph);
        twRow->addWidget(m_twField, 1);
        twRow->addWidget(m_twDot);
        twRow->addWidget(m_twWord);
        hv->addLayout(twRow);

        QObject::connect(m_twField, &QLineEdit::editingFinished, this,
                         [this]() { CommitTwitchField(); });
        return header;
    }

    // Refresh the Zoom status text + tier presentation. The header is ALWAYS
    // visible (a visible "Waiting" row honestly says "configured, works once
    // connected/upgraded" — hiding it reintroduced the discoverability problem
    // this redesign solves, and tier-hiding didn't reliably track logout anyway).
    void UpdateChatHeaderState() {
        if (!m_chatHeader) return;
        if (m_zoomStatus) {
            // Label + click stay in lockstep with TriggerConnectFlow's precedence,
            // so what the row says always matches what clicking it does:
            //   tier-locked      -> "Upgrade Required" (click: upgrade URL) — the
            //                       upgrade is the real blocker, so it wins over
            //                       the login/connect labels.
            //   in a meeting     -> "In Meeting (number)" (click: inert)
            //   logged in        -> "Not Connected"       (click: connect)
            //   logged out       -> "Not Logged In"       (click: login)
            QString z;
            bool actionable;
            if (m_tierDisabled) {
                z = "Upgrade Required"; actionable = true;
            } else if (g_isInMeeting) {
                z = g_currentMeetingNumber
                        ? QString("In Meeting (%1)")
                              .arg(FormatMeetingNumber(g_currentMeetingNumber))
                        : QString("In Meeting");
                actionable = false;
            } else if (g_isLoggedIn) {
                z = "Not Connected"; actionable = true;
            } else {
                z = "Not Logged In"; actionable = true;
            }
            m_zoomStatus->setText(z);
            m_zoomStatus->setCursor(actionable ? Qt::PointingHandCursor
                                               : Qt::ArrowCursor);
        }
        // Grey (but readable) the platform fields while tier-locked so the user sees
        // their configured target but understands it's inactive. Enable transitions
        // (upgrade -> reconcile -> SetTierDisabled) fire reliably; a disabled field
        // fires no editingFinished, and setText still updates it for display.
        if (m_ytField) m_ytField->setEnabled(!m_tierDisabled);
        if (m_twField) m_twField->setEnabled(!m_tierDisabled);

        // Login/tier state just changed — re-resolve both platform rows from their
        // stored connection state so an account-side block (Log In / Upgrade)
        // updates without waiting for the poller to report again.
        RefreshYouTubeRowStatus();
        RefreshTwitchRowStatus();
    }

    // Commit the YouTube field on Enter / focus-out: hand the raw text to the
    // poller side (normalize + persist + wake, deduped there) and rewrite the
    // field to the returned display form (@handle or bare id).
    void CommitYouTubeField() {
        if (!m_ytField) return;
        QString disp = QString::fromStdString(
            YtSetTargetFromInput(m_ytField->text().toStdString()));
        if (m_ytField->text() != disp) {
            QSignalBlocker block(m_ytField);   // don't re-fire editingFinished
            m_ytField->setText(disp);
        }
    }

    // Commit the Twitch field on Enter / focus-out: normalize to a bare login,
    // persist + wake (deduped in TwSetTargetFromInput), rewrite the field.
    void CommitTwitchField() {
        if (!m_twField) return;
        QString disp = QString::fromStdString(
            TwSetTargetFromInput(m_twField->text().toStdString()));
        if (m_twField->text() != disp) {
            QSignalBlocker block(m_twField);
            m_twField->setText(disp);
        }
    }

    // --- Shared per-row status resolver (item 5) -------------------------------
    // Turn a platform's raw connection state (kChatConn*) plus the current account
    // state into the row's presentation. The account-side blocks win over the raw
    // connecting/live states so the row reads honestly: you can't know the tier
    // until logged in, so logged-out shows "Log In" (the actionable first step)
    // ahead of any Upgrade/Waiting. Used by both the YouTube and Twitch rows.
    struct RowStatus { const char* color; const char* word; QString tip; bool clickable; };
    RowStatus ResolveRowStatus(int conn, const char* platform,
                               const QString& liveDetail) const {
        if (conn == kChatConnOff)
            return { "#7a7d80", "Off",
                     QString("No %1 target set").arg(platform), false };
        if (!g_isLoggedIn)
            return { "#e0a020", "Log In",
                     QString("Log in to enable Feeds Chat"), true };
        if (m_tierDisabled || g_currentTier < 1)
            return { "#e0a020", "Upgrade",
                     QString("Upgrade your plan to enable Feeds Chat"), true };
        // "Connected" (not "Live"): this reports the chat connection, not the
        // broadcast. A scheduled stream that hasn't gone on-air yet still has a
        // live-chat connection, and "Live" would wrongly claim it's airing.
        if (conn == kChatConnLive)
            return { "#40c060", "Connected", liveDetail, false };
        return { "#e0a020", "Waiting",
                 QString("Target set \xE2\x80\x94 connecting"), false };
    }

    // Apply a resolved status to a row's dot + word widgets, and record whether the
    // word is a clickable call-to-action (drives the cursor + eventFilter routing).
    void ApplyRowStatus(QLabel* dot, QLabel* word, bool& clickableOut,
                        const RowStatus& s) {
        clickableOut = s.clickable;
        if (dot) {
            dot->setStyleSheet(
                QString("background:%1; border-radius:5px;").arg(s.color));
            dot->setToolTip(s.tip);
        }
        if (word) {
            word->setText(s.word);
            word->setStyleSheet(QString("color:%1;").arg(s.color));
            word->setToolTip(s.tip);
            word->setCursor(s.clickable ? Qt::PointingHandCursor
                                        : Qt::ArrowCursor);
        }
    }

    void RefreshYouTubeRowStatus() {
        ApplyRowStatus(m_ytDot, m_ytWord, m_ytWordClickable,
                       ResolveRowStatus(m_ytConn, "YouTube", m_ytLiveDetail));
    }
    void RefreshTwitchRowStatus() {
        ApplyRowStatus(m_twDot, m_twWord, m_twWordClickable,
                       ResolveRowStatus(m_twConn, "Twitch", QString("Connected")));
    }

    // Remove every row of `origin` whose `role` data equals `value`, clearing any
    // of them from the popup. Backs the YouTube (by msg-id / channel-id) and Twitch
    // (by msg-id / user-id) deletion paths; matching on origin keeps a value
    // collision across platforms from touching the wrong rows.
    void RemoveRowsMatching(int origin, int role, const QString& value) {
        if (value.isEmpty()) return;
        for (int i = m_list->count() - 1; i >= 0; --i) {
            QListWidgetItem* it = m_list->item(i);
            if (!it || it->data(kRoleChatOrigin).toInt() != origin) continue;
            if (it->data(role).toString() != value) continue;
            const std::string content = it->data(RoleContent).toString().toStdString();
            delete m_list->takeItem(i);
            feeds::ClearChatPopupIfMatches(0, content);
        }
    }

    // Shared click-to-connect entry point: the message-list placeholder and the
    // header's Zoom status label both route here. Tier-locked -> upgrade URL;
    // already in a meeting -> inert; logged out -> login; logged in -> connect.
    void TriggerConnectFlow() {
        if (m_tierDisabled) {
            QDesktopServices::openUrl(QUrl("https://letsdovideo.com/feeds-upgrade"));
            return;
        }
        if (g_isInMeeting) return;   // inert while connected
        if (!g_isLoggedIn) OnLoginClick();
        else               OnConnectClick();
    }

    // Route a click on an (unstyled) status label to the shared connect flow. The
    // Zoom row is always actionable; the YouTube/Twitch status words act only in
    // their clickable states (Log In / Upgrade) — TriggerConnectFlow resolves to
    // login when logged out and the upgrade URL when tier-locked, matching what
    // ResolveRowStatus rendered.
    bool eventFilter(QObject* obj, QEvent* ev) override {
        // Preserve the user's reading position across the list's width-driven
        // word-wrap reflow (and keep the floating pill positioned). When this
        // Resize arrives the view hasn't relaid out yet, so we can read the
        // current at-bottom state / top row now and restore it after the
        // relayout via a queued call (scrollTo* flushes the pending layout).
        if (ev->type() == QEvent::Resize && m_list && obj == m_list->viewport()) {
            const bool atBottom = IsAtBottom();
            const QModelIndex topIdx =
                atBottom ? QModelIndex() : m_list->indexAt(QPoint(2, 2));
            const int topRow = topIdx.isValid() ? topIdx.row() : -1;
            QTimer::singleShot(0, this, [this, atBottom, topRow]() {
                if (atBottom) {
                    m_list->scrollToBottom();
                } else if (topRow >= 0 && topRow < m_list->count()) {
                    m_list->scrollToItem(m_list->item(topRow),
                                         QAbstractItemView::PositionAtTop);
                }
                PositionNewMessagesPill();
            });
            PositionNewMessagesPill();
            return false;  // let the view handle the resize / relayout
        }
        if (ev->type() == QEvent::MouseButtonRelease &&
            (obj == m_zoomStatus ||
             (obj == m_ytWord && m_ytWordClickable) ||
             (obj == m_twWord && m_twWordClickable))) {
            TriggerConnectFlow();
            return true;
        }
        return QWidget::eventFilter(obj, ev);
    }

    void SetPlaceholder(const QString& text) {
        // Once real messages have arrived, the placeholder is permanently
        // off for the rest of the OBS session — joining/leaving meetings
        // must not stomp on accumulated chat history.
        if (m_messagesStarted) return;
        m_list->clear();
        m_list->addItem(text);
    }

    void OnMessageClicked(QListWidgetItem* item) {
        if (!item) return;

        // YouTube row -> popup (Phase 2). Resolve the avatar from the YT cache by
        // channel id (null => neutral circle) and toggle the popup with senderId
        // 0 (YouTube has no Zoom uint id). This branch MUST precede the
        // placeholder detection below: a YouTube row carries no RoleSenderId, so
        // without this it would be misread as a placeholder/login click.
        const QVariant originVar = item->data(kRoleChatOrigin);
        if (originVar.isValid() && originVar.toInt() == kChatOriginYouTube) {
            QString name    = item->data(RoleSenderName).toString();
            QString content = item->data(RoleContent).toString();
            std::string channelId =
                item->data(kRoleChatChannel).toString().toStdString();
            feeds::ToggleChatPopup(0, name.toStdString(), content.toStdString(),
                                   YtResolveAvatar(channelId));
            return;
        }

        // Twitch row -> popup with senderId 0. Resolve the avatar from the Twitch
        // cache by user-id (stored in the by-author role); a miss/null renders the
        // popup's neutral circle. Same precede-the-placeholder rule as YouTube: a
        // Twitch row has no RoleSenderId.
        if (originVar.isValid() && originVar.toInt() == kChatOriginTwitch) {
            QString name    = item->data(RoleSenderName).toString();
            QString content = item->data(RoleContent).toString();
            std::string userId = item->data(kRoleChatChannel).toString().toStdString();
            feeds::ToggleChatPopup(0, name.toStdString(), content.toStdString(),
                                   TwResolveAvatar(userId));
            return;
        }

        // Placeholders carry no sender_id data — route the click through the
        // shared connect flow (also used by the header's Zoom status label). The
        // flow handles the tier-locked (upgrade), already-in-meeting (inert),
        // logged-out (login) and logged-in (connect) cases.
        QVariant idVar = item->data(RoleSenderId);
        if (!idVar.isValid()) {
            TriggerConnectFlow();
            return;
        }

        unsigned int senderId = idVar.toUInt();
        QString      sender   = item->data(RoleSenderName).toString();
        QString      content  = item->data(RoleContent).toString();

        // Resolve the Zoom avatar from the uint-keyed cache (warmed on message
        // arrival; falls back to the Feeds logo). The popup renders whatever
        // image it's handed — same handoff the YouTube path uses.
        feeds::ToggleChatPopup(senderId,
                               sender.toStdString(),
                               content.toStdString(),
                               GetAvatarForSender(senderId, std::string()));
    }

    QString CurrentPlaceholderText() const {
        if (m_tierDisabled) {
            return "Feeds Chat is a paid feature. Click to upgrade your plan.";
        }
        return g_isLoggedIn
            ? "Logged in. Click to Connect to Zoom Meeting."
            : "Not logged in to Zoom. Click to Login.";
    }

    void SendCurrentMessage() {
        QString text = m_input->text().trimmed();
        if (text.isEmpty()) return;

        // Disable while sending to absorb double-clicks / mash-enter.
        // We don't await the engine's response — re-enabling immediately
        // after dispatch keeps the input responsive; if the send fails,
        // chat_send_result surfaces it via MessageBox.
        m_input->setEnabled(false);
        m_sendBtn->setEnabled(false);

        std::string msg =
            "{\"type\":\"send_chat_message\",\"content\":\"" +
            JsonEscape(text.toStdString()) + "\"}";
        feeds::SendToEngine(msg);

        m_input->clear();
        m_input->setEnabled(true);
        m_sendBtn->setEnabled(true);
        m_input->setFocus();
    }

    QListWidget* m_list             = nullptr;
    QLineEdit*   m_input            = nullptr;
    QPushButton* m_sendBtn          = nullptr;
    QPushButton* m_newMsgPill       = nullptr;  // floating "↓ New messages"
    bool         m_messagesStarted  = false;

    // Platform header widgets (see BuildChatHeader).
    QWidget*     m_chatHeader       = nullptr;
    QLabel*      m_zoomStatus       = nullptr;
    QLineEdit*   m_ytField          = nullptr;
    QLabel*      m_ytDot            = nullptr;
    QLabel*      m_ytWord           = nullptr;
    QLineEdit*   m_twField          = nullptr;
    QLabel*      m_twDot            = nullptr;
    QLabel*      m_twWord           = nullptr;
    // Last connection state each poller reported (kChatConn*), retained so the row
    // can be re-resolved on an account-state change without a fresh poller report.
    int          m_ytConn           = kChatConnOff;
    int          m_twConn           = kChatConnOff;
    QString      m_ytLiveDetail     = QString("Connected");  // YT Live tooltip (video id)
    // Whether each platform's status word is currently a clickable CTA (Log In /
    // Upgrade) — gates the eventFilter routing and the pointing-hand cursor.
    bool         m_ytWordClickable  = false;
    bool         m_twWordClickable  = false;
    // True iff the current logged-in tier is Free (< 1). Dock starts
    // false (pre-login state is identical to a normal Basic+ session)
    // and toggles via SetTierDisabled from ReconcileSourcesToTier.
    bool         m_tierDisabled     = false;
};

// Non-owning pointer to the registered dock instance. OBS owns the widget's
// lifetime once it's been handed to obs_frontend_add_dock_by_id; we hold
// this only so the chat_message IPC handler can route messages to it.
static FeedsChatDock* g_chatDock = nullptr;

// Single-line label that elides its (full) text to the current width with a
// trailing ellipsis and carries the full text as a tooltip. Used for source-box
// headers so a long source name never wraps (wrapping makes box heights jump) —
// it stays one line and truncates with "…", re-eliding as the dock is resized.
// minimumSizeHint width is 0 so a long name can't force the box (or the dock)
// wider; the box shrinks to the dock width and the header elides to fit.
class ElidingLabel : public QLabel {
public:
    explicit ElidingLabel(const QString& text, QWidget* parent = nullptr)
        : QLabel(parent), m_full(text) {
        // No tooltip by design: this label is only used for the source-box header,
        // whose name is already shown in the box; a tooltip would be redundant.
        setTextFormat(Qt::PlainText);
        QLabel::setText(text);   // re-elided on first resizeEvent
    }
    QSize minimumSizeHint() const override {
        return QSize(0, QFontMetrics(font()).height());
    }
    // Optional double-click affordance (source-box header rename). No signal/moc:
    // the owner installs a plain callback, invoked from the event override below.
    std::function<void()> onDoubleClick;
protected:
    void resizeEvent(QResizeEvent* e) override {
        QLabel::resizeEvent(e);
        // Elide against the content width (inside this label's own margins).
        QFontMetrics fm(fontMetrics());
        QLabel::setText(fm.elidedText(m_full, Qt::ElideRight, contentsRect().width()));
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        if (onDoubleClick) onDoubleClick();
        else QLabel::mouseDoubleClickEvent(e);
    }
private:
    QString m_full;
};

// A QPushButton that truncates its label with a trailing ellipsis when the label
// doesn't fit — the OBS dock-tab convention ("Chat O…") — rather than clipping the
// glyphs hard, shrinking the font, or forcing the dock wider. Used for the header's
// three equal-thirds add-source buttons: with their reduced horizontal padding (see
// StyleHeaderButton) the short labels ("Screenshare", "Chat Overlay", "Chat Popup")
// fit whole at or near the min dock width; a narrower dock elides instead of
// overflowing. The un-elided label is kept for re-eliding on resize; minimumSizeHint
// drops the natural-width floor so the equal-thirds layout may shrink a button below
// its text width and let it elide. No icon path: the header buttons carry no icons.
class ElidingPushButton : public QPushButton {
public:
    explicit ElidingPushButton(const QString& text, QWidget* parent = nullptr)
        : QPushButton(parent), m_full(text) {
        QPushButton::setText(text);   // re-elided on first resizeEvent
    }
    QSize minimumSizeHint() const override {
        // Keep the style's height; floor the width at about an ellipsis so three
        // buttons can share a narrow dock without overflowing it.
        QSize s = QPushButton::minimumSizeHint();
        int floor = QFontMetrics(font()).horizontalAdvance(QStringLiteral("…")) + 12;
        return QSize(qMin(s.width(), floor), s.height());
    }
protected:
    void resizeEvent(QResizeEvent* e) override {
        QPushButton::resizeEvent(e);
        // Elide against the content rect (inside the style's frame + our reduced
        // horizontal padding). The guarded setText avoids a resize feedback loop.
        QStyleOptionButton opt;
        initStyleOption(&opt);
        QRect cr = style()->subElementRect(QStyle::SE_PushButtonContents, &opt, this);
        QString shown = QFontMetrics(font()).elidedText(
            m_full, Qt::ElideRight, qMax(0, cr.width()));
        if (shown != text()) QPushButton::setText(shown);
    }
private:
    QString m_full;
};

// Inline rename editor for a source-box header. Double-clicking the header swaps
// one of these in (FeedsParticipantDock::BeginRename); Enter or focus-out commits,
// Escape cancels. No Q_OBJECT/signals — the two outcomes are delivered through
// std::function callbacks fired exactly once. The m_done latch guards against
// Escape-then-focus-out (or commit-then-teardown) double-firing; the owner's
// EndRenameUi does the widget swap-back and deletion.
class RenameLineEdit : public QLineEdit {
public:
    explicit RenameLineEdit(const QString& text, QWidget* parent = nullptr)
        : QLineEdit(text, parent) {}

    std::function<void(const QString&)> onCommit;  // Enter or focus-out
    std::function<void()>               onCancel;  // Escape

    // Neutralise pending callbacks (dock teardown): a focus-out fired while the
    // dock is being destroyed must not re-enter a half-torn-down dock.
    void Detach() { m_done = true; onCommit = nullptr; onCancel = nullptr; }

protected:
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Escape)                                { fireCancel(); return; }
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)   { fireCommit(); return; }
        QLineEdit::keyPressEvent(e);
    }
    void focusOutEvent(QFocusEvent* e) override {
        QLineEdit::focusOutEvent(e);
        fireCommit();   // commit on focus loss (mirrors OBS's own rename editor)
    }
private:
    bool m_done = false;
    // Copy the callback + text before invoking: the callback may deleteLater this
    // widget, so we must touch no members after the call.
    void fireCommit() {
        if (m_done) return;
        m_done = true;
        auto f = onCommit; const QString t = text();
        if (f) f(t);
    }
    void fireCancel() {
        if (m_done) return;
        m_done = true;
        auto f = onCancel;
        if (f) f();
    }
};

// Current mute state per participant userId, for the dock's mute indicator.
// UI-thread-owned (no mutex): written only inside the marshalled lambdas of the
// participant_list_changed seed and the participant_audio_status live update,
// and read only by the dock on the UI thread (MakeSourceBox seed). Absence of an
// entry = unknown -> no mute mark.
static std::map<unsigned int, bool> g_muteByUserId;

// Per-userId connection-quality legs, for the dock's connection dot. Levels are
// Zoom's ConnectionQuality as raw ints (0 Unknown, 1 Very_Bad, 2 Bad, 3 Not_Good,
// 4 Normal, 5 Good, 6 Excellent) — the plugin doesn't link the SDK, so it works
// off the int the engine forwards. We keep video + audio, up + down, so the dot
// can prioritize the video-uplink leg (how well the guest's video reaches us) and
// fall back if that leg doesn't fire; share/def legs are dropped (not dot-useful).
// UI-thread-owned like g_muteByUserId: written only in the participant_conn_quality
// marshalled lambda, read only by the dock's poll. 0 on a leg = no level yet.
struct ConnQualityLegs {
    int videoUp = 0, videoDown = 0, audioUp = 0, audioDown = 0;
};
static std::map<unsigned int, ConnQualityLegs> g_connQualityByUserId;

// Reserved fixed slot (px) for the mute mark in a source-box header. The slot is
// always present (paint-only toggle, no reflow); 14px reads the mic glyph clearly.
static constexpr int kMuteSlotPx = 14;

// ---------------------------------------------------------------------------
// Read-only participant dock — lists every participant source and its current
// assignment, live-reacting to roster / source / tier changes. Phase 1: no
// dropdown, no create button (later phases). Rebuilds its whole content on
// each Refresh() from a lock-safe snapshot.
// ---------------------------------------------------------------------------
class FeedsParticipantDock : public QWidget {
public:
    explicit FeedsParticipantDock(QWidget* parent = nullptr) : QWidget(parent) {
        // A scroll area wraps the content so a tall stack (several boxes + cap
        // chrome + upgrade prompt) can overflow the dock height reachably. Matches
        // the chat dock's QListWidget behavior: vertical scrollbar only when the
        // content overflows; horizontal scrolling off (single-column boxes track
        // the viewport width and elide). The boxes live in the inner content
        // widget's layout (m_root), NOT the dock's outer layout — ClearContent
        // walks m_root, so it still sees exactly the boxes/divider/chrome.
        QVBoxLayout* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);

        // Thin border around the whole dock so it reads as a distinct panel (like
        // OBS's own docks / the Feeds chat dock) instead of blending into the app
        // background. The border lives on a real QFrame that wraps ALL the dock
        // content (header + scroll), NOT on this FeedsParticipantDock (a plain
        // QWidget subclass): a QWidget subclass does not reliably paint a stylesheet
        // border even with WA_StyledBackground, whereas QFrame natively paints the
        // CSS box — the same reason the chat dock's QListWidget shows a frame and our
        // borderless QScrollArea did not. A semi-transparent line (not a hardcoded
        // colour or a background fill) so it adapts across themes (dark, light, the
        // user's custom theme). Scoped by objectName so only this frame gets the
        // border; the CSS box model insets its children 1px, so the header/scroll sit
        // inside the line. This frame is the dock's single top-level child and fills
        // the QDockWidget content rect OBS gives us.
        QFrame* dockFrame = new QFrame(this);
        dockFrame->setObjectName("feedsParticipantDock");
        dockFrame->setStyleSheet("QFrame#feedsParticipantDock {"
                                 " border: 1px solid rgba(128,128,128,0.35); }");
        QVBoxLayout* frameLayout = new QVBoxLayout(dockFrame);
        // 1px inner margin so the header/scroll never paint over the border line,
        // independent of whether the CSS box model already insets them — the header
        // has a translucent fill and the scroll viewport an opaque one that would
        // otherwise cover the edge. Guarantees the 1px ring is always visible.
        frameLayout->setContentsMargins(1, 1, 1, 1);
        frameLayout->setSpacing(0);
        outer->addWidget(dockFrame);

        QScrollArea* scroll = new QScrollArea(dockFrame);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

        QWidget* content = new QWidget();
        m_root = new QVBoxLayout(content);
        m_root->setContentsMargins(8, 8, 8, 8);
        m_root->setSpacing(8);   // between boxes/chrome; box-internal spacing is tighter
        m_root->setAlignment(Qt::AlignTop);

        scroll->setWidget(content);

        // Fixed header chrome pinned to the dock TOP — added to the OUTER layout
        // ABOVE the scroll area, so it never scrolls with the source list and never
        // moves as participants join/leave. It carries the state-driven Connect/
        // Login button (top slot, rebuilt each Refresh) and, beneath it, the three
        // always-present add-source buttons. The three source buttons are static,
        // roster-independent actions, so they belong here as a fixed header rather
        // than mixed into the moving participant content (or competing with OBS's
        // own dock tab strip, which sits directly below a bottom-pinned bar). Their
        // per-button state (tier lock + screenshare live dot) is updated in place by
        // UpdateHeaderState() on every Refresh(), riding the existing tier/login/
        // meeting/share refresh path with no separate mechanism.
        m_header = BuildHeaderBar();
        frameLayout->addWidget(m_header, 0);
        frameLayout->addWidget(scroll, 1);   // takes the stretch; header stays pinned above

        // Width is derived from the live theme/font in Refresh() (setMinimumWidth
        // on the dock root, propagating to the QDockWidget). Height floor as before.
        setMinimumHeight(300);
        Refresh();

        // Live-indicator poll. Parented to the dock (destroyed with it) and the
        // slot uses `this` as context, so it can't fire against a destroyed
        // widget — same teardown discipline as the marshalled refreshes. Each
        // tick reads per-source lastRealFrameTick (atomics, under g_sourcesMutex
        // briefly) and updates each row's live dot in place — no Refresh(), no
        // box rebuild. Gated on isVisible() inside the poll.
        m_liveTimer = new QTimer(this);
        m_liveTimer->setInterval(400);
        QObject::connect(m_liveTimer, &QTimer::timeout, this,
                         [this]() { PollLiveIndicators(); });
        m_liveTimer->start();
    }

    ~FeedsParticipantDock() override {
        // If an inline rename is somehow still open at teardown, neutralise its
        // callbacks so the focus-out Qt fires while destroying children can't
        // re-enter this half-destroyed dock. The editor itself is a child widget,
        // freed by the QWidget base destructor.
        if (m_activeEdit) m_activeEdit->Detach();
    }

    // MUST run on the Qt UI thread (mutates widgets). All callers marshal via
    // PostParticipantDockRefresh. The gather half takes the two mutexes; the
    // build half touches Qt only after both are released.
    void Refresh() {
        // Header chrome first: it's persistent (outer layout, not m_root) and
        // independent of the inline-rename editor, so update it unconditionally —
        // even on the deferred-rename and empty-rows early returns below — so a
        // tier/login/share change always re-styles the header's source buttons.
        UpdateHeaderState();

        // An inline rename is in progress — don't tear the box (and the edit
        // field) out from under the user. Coalesce: remember a refresh is due and
        // run it once the edit commits or cancels (EndRenameUi flushes it). Every
        // edit resolves via focus-out at the latest, so this can't wedge the dock
        // into a permanently-deferred state.
        if (m_activeEdit) { m_refreshPending = true; return; }

        // --- Gather a plain-value snapshot (no OBS/Qt handles retained) ---
        // Roster copy first, under g_participantsMutex, then release it before
        // touching g_sourcesMutex — never nest the two (matches
        // ReconcileRememberedParticipants' ordering).
        std::map<unsigned int, std::string> roster;
        unsigned int myId = 0;
        {
            std::lock_guard<std::mutex> lock(g_participantsMutex);
            // g_cachedMyUserId is guarded by g_participantsMutex (same as the
            // roster) — capture it here, in the same locked block, to exclude
            // self from the "others present" count below (mirrors zp_properties).
            myId = g_cachedMyUserId;
            for (const auto& p : g_cachedParticipants)
                roster[p.id] = p.name;
        }

        std::vector<Row> rows;
        size_t sourceCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_sourcesMutex);
            sourceCount = g_allParticipantSources.size();
            for (ZpSourceData* s : g_allParticipantSources) {
                if (!s || !s->source) continue;
                Row r;
                r.uuid = s->uuid;
                const char* nm = obs_source_get_name(s->source);
                r.name = nm ? nm : "";
                // Only participant_id is needed for display; the remembered-name
                // key (kParticipantNameKey) is no longer a display input (names
                // come from the live roster only). obs_source_get_settings is
                // refcounted — release each iteration.
                obs_data_t* st = obs_source_get_settings(s->source);
                if (st) {
                    r.pid = obs_data_get_int(st, "participant_id");
                    obs_data_release(st);
                }
                r.disabled = s->tier_disabled;
                // Initial live state for the box's dot (kept updated by the poll
                // between rebuilds). Read the atomic here under g_sourcesMutex.
                uint64_t tick = s->lastRealFrameTick.load(std::memory_order_relaxed);
                r.liveNow = (tick != 0) &&
                            (os_gettime_ns() - tick) < kLiveThresholdNs;
                rows.push_back(std::move(r));
            }
        }
        // --- Both mutexes released. From here on, Qt only. ---

        const bool   loggedIn    = g_isLoggedIn;
        const bool   inMeeting   = g_isInMeeting;
        const bool   granted     = (g_rawPrivilegeState == RawPrivilegeState::Granted);
        // Others present: roster entries excluding self (myId). Same count the
        // properties dialog uses to decide between a live dropdown and the
        // "Waiting for participants..." state. Computed from the released roster
        // snapshot — no lock held here.
        size_t othersPresent = 0;
        for (const auto& kv : roster)
            if (myId == 0 || kv.first != myId) ++othersPresent;

        // "Live" is the single per-Refresh authority for un-greying rows and
        // showing roster names: in a meeting, raw-livestream privilege granted,
        // AND at least one other participant present — matching the properties
        // dialog, which omits the live dropdown (and the [Active Speaker] entry)
        // while alone. Connected-but-alone and the post-join / pre-grant window
        // are both non-live: greyed rows — "connected, feeds coming up". The
        // others-present clause is what hides "[Active Speaker]" until someone
        // joins, with no special-casing of pid == 1. Evaluated once here so every
        // row keys off the same value; no row can read live while another greyed.
        const bool   live        = inMeeting && granted && (othersPresent > 0);
        const int    maxFeeds    = GetMaxFeedsForTier();
        // Cap chrome only when logged in (logged out, tier defaults to a cap of
        // 1 and tier_disabled is not authoritative, which would falsely read a
        // multi-source logged-out user as over-cap). Independent of meeting state.
        const bool   atOrOverCap = loggedIn && (sourceCount >= (size_t)maxFeeds);

        // Derive the dock's minimum width from the live theme/font so a box's
        // single column (header + combo) is comfortable with margin — not tight
        // on the current setup, not clipping on a larger-font theme. Set on the
        // dock root (propagates to the QDockWidget), only when it actually changes
        // (theme/font change), so normal refreshes don't churn geometry.
        int mw = ComputeMinDockWidth();
        if (mw != m_minWidth) { m_minWidth = mw; setMinimumWidth(mw); }

        ClearContent();

        // Top slot — three states, mirroring the properties dialog's single
        // state-driven top slot:
        //   not in a meeting            -> connect/login button (in the FIXED header)
        //   in a meeting, granted, alone-> greyed "Waiting for participants..."
        //   live (other participant)    -> neither
        // The pre-grant window (in a meeting, not granted) shows neither.
        //
        // The connect/login button lives in the fixed header (above the three
        // source buttons), NOT in this scrolling content, so it never scrolls under
        // the roster; when it's gone (connected), the source buttons become the
        // dock's top row. Rebuilt here from the freshly gathered login/meeting
        // state. The "Waiting for participants..." line stays in the scrolling
        // content — it's roster feedback that belongs above the rows, and keeping it
        // out of the header means the source buttons don't shift when it comes/goes.
        RebuildHeaderTopSlot(loggedIn, inMeeting);

        if (inMeeting && granted && othersPresent == 0) {
            // Connected but alone — one greyed waiting line at the top (not per
            // row). Same string and #7a7d80 styling as the properties dialog.
            QLabel* waiting = new QLabel("Waiting for participants...");
            waiting->setStyleSheet("QLabel { color: #7a7d80; }");
            waiting->setWordWrap(true);
            m_root->addWidget(waiting);
        }

        if (rows.empty()) {
            QLabel* empty = new QLabel(
                "No Feeds Participant sources yet.\n"
                "Click “Create Participant Feed” below to create one.");
            empty->setWordWrap(true);
            empty->setStyleSheet("QLabel { color: #7a7d80; }");
            m_root->addWidget(empty);
            AppendCreateButton();
            m_root->addStretch(1);
            return;
        }

        // Multi-source guidance, two counts, recomputed every refresh:
        //  - pidCount: raw assignments across ALL rows (live, non-live, over-cap).
        //    Drives the dropdown "already used" treatment — a persisted assignment
        //    reserves a participant even while absent, so the guidance must warn off
        //    picking them elsewhere before they join.
        //  - livePidCount: only rows whose assignment is LIVE-bound and not self
        //    (BoundUserId != 0), keyed by the raw pid. Drives the collision warning,
        //    so two sources warn only when they share a raw assignment AND both are
        //    actually present — never for stale/absent/self assignments. Keyed by raw
        //    pid keeps [Active Speaker] (pid 1) from ever colliding with a named
        //    participant, even when the active speaker currently is that person.
        std::map<long long, int> pidCount, livePidCount;
        for (const auto& r : rows) {
            if (r.pid > 0)                             pidCount[r.pid]++;
            if (r.pid > 0 && BoundUserId(r.pid) != 0)  livePidCount[r.pid]++;
        }

        // Enabled (tier-active) sources on top, in vector = creation order,
        // each a self-contained framed box. Logged out, no row is treated as
        // disabled, so all render here. A live source is an interactive box
        // (header + functional combo); a non-live one is the same box, frame
        // dimmed and combo-less (header + "Waiting for participants…") so its
        // identity/position stays stable across the live/non-live flip rather
        // than the box appearing/disappearing.
        for (const auto& r : rows) {
            if (loggedIn && r.disabled) continue;
            m_root->addWidget(MakeSourceBox(r, live, myId, roster, pidCount, livePidCount));
        }

        if (atOrOverCap) {
            QFrame* line = new QFrame();
            line->setFrameShape(QFrame::HLine);
            line->setFrameShadow(QFrame::Sunken);
            m_root->addWidget(line);

            // Over-cap sources: grey name only, no assignment, no controls —
            // but still renamable. Grey is a tier-position status signal, not a
            // lock (the Source dock lets you rename a source in any state). An
            // ElidingLabel with the same double-click handler as the framed-box
            // headers; BeginRename swaps it in place within this (m_root) layout.
            // Empty at exactly-cap — then only the prompt sits below the divider.
            for (const auto& r : rows) {
                if (!(loggedIn && r.disabled)) continue;
                ElidingLabel* lbl = new ElidingLabel(QString::fromStdString(r.name));
                lbl->setStyleSheet("QLabel { color: #7a7d80; }");
                const std::string uuid = r.uuid;
                lbl->onDoubleClick = [this, uuid, lbl]() { BeginRename(uuid, lbl); };
                m_root->addWidget(lbl);
            }

            // One shared, group-level prompt pinned at the bottom. At the product
            // ceiling (a tier whose cap is the hard max — Broadcaster at 8) there's
            // nothing to upgrade to, so the message says so and no upgrade button is
            // shown. Lower tiers keep the upgrade CTA.
            const bool atCeiling = (maxFeeds >= kMaxParticipantSourcesEver);
            QLabel* msg = new QLabel(
                atCeiling
                  ? QString("%1 is the maximum supported number of participant "
                            "feeds.").arg(maxFeeds)
                  : QString("Your current plan allows %1 participant source%2. "
                            "Upgrade to activate more feeds.")
                        .arg(maxFeeds)
                        .arg(maxFeeds == 1 ? "" : "s"));
            msg->setWordWrap(true);
            m_root->addWidget(msg);

            if (!atCeiling) {
                QPushButton* btn =
                    new QPushButton("Upgrade your plan to activate more feeds");
                // Same upgrade destination as the properties-dialog upgrade button.
                QObject::connect(btn, &QPushButton::clicked, []() {
                    QDesktopServices::openUrl(
                        QUrl("https://letsdovideo.com/feeds-upgrade"));
                });
                m_root->addWidget(btn);
            }
        }

        // Hide the create button at/over the tier cap so it doesn't compete with
        // the upgrade prompt (a deliberate divergence from the Source dock, which
        // still creates above cap). Same count-vs-cap test the greying/divider
        // uses, so "at cap" means the same thing everywhere; logged out / tier
        // unknown keeps atOrOverCap false, so the button still shows there. It
        // returns automatically once a delete drops back under cap (rebuilt here).
        if (!atOrOverCap)
            AppendCreateButton();
        m_root->addStretch(1);
    }

private:
    // Full-width "create a Feeds participant source" button, pinned at the bottom
    // (the top slot is state-driven). Never disabled by the tier cap — it always
    // creates; an over-cap result is greyed by the identical existing logic. The
    // create + scene-add + placement + dock-row all ride the existing signal paths.
    // "Create Participant Feed" (no "+") names what it uniquely does — brings a new
    // feed into existence — vs. the header buttons that place/reference a source.
    void AppendCreateButton() {
        QPushButton* addBtn = new QPushButton("Create Participant Feed");
        m_createBtn = addBtn;   // current instance (rebuilt each Refresh) for the cooldown restyle
        // Creating a feed triggers a dock rebuild (zp_create -> refresh), which
        // replaces this button — so the cooldown state lives on the dock, not the
        // widget: re-grey the fresh button here whenever a cooldown is still in
        // flight across that rebuild.
        StyleCreateButton(addBtn, m_cdCreateFeed);
        QObject::connect(addBtn, &QPushButton::clicked, [this]() {
            if (m_cdCreateFeed) return;   // on add-cooldown: absorb the rapid re-click
            CreateParticipantSourceInCurrentScene();
            m_cdCreateFeed = true;
            if (m_createBtn) StyleCreateButton(m_createBtn, true);
            QTimer::singleShot(kAddCooldownMs, this, [this]() {
                m_cdCreateFeed = false;
                if (m_createBtn) StyleCreateButton(m_createBtn, false);
            });
        });
        m_root->addWidget(addBtn);
    }

    // Create-button resting style is the theme default (no stylesheet); on the add-
    // cooldown it greys to the same #7a7d80 as the tier-locked buttons, then reverts.
    // Colour only — the cursor is left at the button default in both states, so the
    // resting appearance is unchanged from before the cooldown existed.
    static void StyleCreateButton(QPushButton* b, bool cooling) {
        if (!b) return;
        b->setStyleSheet(cooling ? "QPushButton { color: #7a7d80; }" : QString());
    }

    // --- Fixed header bar: add-source buttons for the other three source types ---
    // A toolbar-style bar pinned to the dock TOP (built once, in the outer layout),
    // completing "all four Feeds source types addable from the dock". Three
    // equal-width buttons; each honors its type's multiplicity rule:
    //   Screenshare  — reference-or-create (hard singleton; zs_create refuses a 2nd)
    //   Chat Overlay — ALWAYS create new (per-scene width/rows/count differ)
    //   Chat Popup   — reference-or-create (no reason for duplicates)
    // Tier gates match the source types themselves: screenshare Basic+ (tier >= 1),
    // chat overlay/popup Streamer+ (tier >= 2). g_currentTier is the same value the
    // cap chrome reads, so the header re-locks in the same refresh path.

    // One equal-thirds header button. Tooltip is the constant "Add to Current
    // Scene" in every state, locked or unlocked — the tier explanation lives in the
    // upgrade prompt a locked click opens, so it's set once here and never changed.
    // Eliding (ElidingPushButton) truncates the label with an ellipsis if a narrow
    // dock can't fit it whole, matching OBS's dock-tab convention. Per-state cursor,
    // padding, and label color are applied by StyleHeaderButton on each Refresh.
    static ElidingPushButton* MakeHeaderButton(const QString& label) {
        ElidingPushButton* b = new ElidingPushButton(label);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        b->setToolTip("Add to Current Scene");
        return b;
    }

    // Rebuild the fixed header's top slot: the Connect/Login button when not in a
    // meeting, empty (and collapsed to zero height) once connected — at which point
    // the three source buttons below become the dock's top row. Split on login
    // state only; no tier lock (this dock isn't tier-gated, so a Free user sees and
    // uses it). Same labels/actions as the properties dialog's buttons. Called from
    // Refresh() with the freshly gathered login/meeting state.
    void RebuildHeaderTopSlot(bool loggedIn, bool inMeeting) {
        if (!m_headerTopLayout) return;
        QLayoutItem* item;
        while ((item = m_headerTopLayout->takeAt(0)) != nullptr) {
            if (QWidget* w = item->widget()) { w->hide(); w->deleteLater(); }
            delete item;
        }
        if (inMeeting) { m_headerTopSlot->setVisible(false); return; }

        QPushButton* btn = new QPushButton(
            loggedIn ? "Logged in. Click to Connect to Zoom Meeting."
                     : "Not logged in to Zoom. Click to Login.");
        if (loggedIn)
            QObject::connect(btn, &QPushButton::clicked, []() { OnConnectClick(); });
        else
            QObject::connect(btn, &QPushButton::clicked, []() { OnLoginClick(); });
        m_headerTopLayout->addWidget(btn);
        m_headerTopSlot->setVisible(true);
    }

    QWidget* BuildHeaderBar() {
        QWidget* header = new QWidget();
        QVBoxLayout* hv = new QVBoxLayout(header);
        hv->setContentsMargins(0, 0, 0, 0);
        hv->setSpacing(0);

        // Top slot: the state-driven Connect/Login button, rebuilt each Refresh by
        // RebuildHeaderTopSlot. Hidden (zero height) whenever it has no button — i.e.
        // once connected — so the three source buttons sit flush at the dock top.
        // Its own margins pad the button, matching the old in-content top slot.
        m_headerTopSlot = new QWidget();
        m_headerTopLayout = new QVBoxLayout(m_headerTopSlot);
        m_headerTopLayout->setContentsMargins(8, 8, 8, 6);
        m_headerTopLayout->setSpacing(0);
        m_headerTopSlot->setVisible(false);
        hv->addWidget(m_headerTopSlot);

        QFrame* bar = new QFrame();
        bar->setObjectName("feedsHeaderBar");
        // Toolbar chrome: a bottom divider + faint fill so it reads as fixed header
        // chrome, distinct from the scrolling participant rows below.
        bar->setStyleSheet(
            "QFrame#feedsHeaderBar { border-bottom: 1px solid rgba(128,128,128,0.35);"
            " background: rgba(128,128,128,0.06); }");

        QHBoxLayout* l = new QHBoxLayout(bar);
        l->setContentsMargins(8, 6, 8, 6);
        l->setSpacing(6);

        m_hdrScreenshare = MakeHeaderButton("Screenshare");
        QObject::connect(m_hdrScreenshare, &QPushButton::clicked, [this]() {
            if (g_currentTier < 1) {   // tier-locked (Basic+): actionable upgrade prompt
                ShowSourceUpgradePrompt("Feeds Screenshare", "Basic");
                return;   // locked: no add to cool down (upgrade-prompt throttle handles it)
            }
            if (m_cdScreenshare) return;   // on add-cooldown: absorb the rapid re-click
            AddOrReferenceSourceInCurrentScene("zoom_screenshare_source", "Feeds Screenshare",
                                               RefAddPlacement::FitCenter);
            BeginHeaderCooldown(&m_cdScreenshare);
        });

        m_hdrChatOverlay = MakeHeaderButton("Chat Overlay");
        QObject::connect(m_hdrChatOverlay, &QPushButton::clicked, [this]() {
            if (g_currentTier < 2) {   // tier-locked (Streamer+): actionable upgrade prompt
                ShowSourceUpgradePrompt("Feeds Chat Overlay", "Streamer");
                return;
            }
            if (m_cdChatOverlay) return;   // on add-cooldown: absorb the rapid re-click
            CreateSourceOfTypeInCurrentScene("feeds_chat_overlay", "Feeds Chat Overlay");
            BeginHeaderCooldown(&m_cdChatOverlay);
        });

        m_hdrChatPopup = MakeHeaderButton("Chat Popup");
        QObject::connect(m_hdrChatPopup, &QPushButton::clicked, [this]() {
            if (g_currentTier < 2) {   // tier-locked (Streamer+): actionable upgrade prompt
                ShowSourceUpgradePrompt("Feeds Chat Popup", "Streamer");
                return;
            }
            if (m_cdChatPopup) return;   // on add-cooldown: absorb the rapid re-click
            AddOrReferenceSourceInCurrentScene("feeds_chat_popup", "Feeds Chat Popup",
                                               RefAddPlacement::PopupDefault);
            BeginHeaderCooldown(&m_cdChatPopup);
        });

        l->addWidget(m_hdrScreenshare, 1);   // equal thirds
        l->addWidget(m_hdrChatOverlay, 1);
        l->addWidget(m_hdrChatPopup, 1);

        hv->addWidget(bar);
        return header;
    }

    // Style one header button for its tier-lock and share state. Kept ENABLED even
    // when locked: a disabled Qt widget receives no hover events, so its tooltip
    // wouldn't show — and the button must still open the upgrade prompt on click.
    // Locked = greyed text (#7a7d80) + arrow cursor; the click lambda short-circuits
    // the add on the tier check and instead opens the upgrade prompt
    // (ShowSourceUpgradePrompt), so it never performs the source action. The tooltip
    // ("Add to Current Scene") is constant in both states — set once at creation — so
    // this doesn't touch it.
    //
    // Color precedence: tier-locked greying wins over the share-active green. Only
    // the Screenshare button ever passes sharing=true, and only when within tier, so
    // a locked button is never green (nor sharing). The green (#40c060, the tone the
    // old status dot used) is clearly distinct from the locked grey.
    //
    // The stylesheet also sets a reduced horizontal padding (~half the themed
    // default) so the labels reclaim text room and fit at/near the min dock width;
    // it's the per-button *minimum* — the Expanding buttons still grow the padding by
    // centering when the dock is widened. Matches the padding-stylesheet pattern the
    // row buttons already use. Padding lives in the same stylesheet as the color so a
    // restyle never drops it.
    // `cooling` is the transient add-cooldown state (see BeginHeaderCooldown): it
    // greys the button while active and takes visual precedence over the sharing
    // green, but not over the tier-lock grey (a locked button never performs an add,
    // so it never cools). Because this recomputes colour from the live lock/share
    // state every call, the post-cooldown restyle restores the correct resting look
    // — normal, still-green if sharing, etc. — rather than clobbering it.
    static void StyleHeaderButton(QPushButton* b, bool locked, bool sharing, bool cooling) {
        if (!b) return;
        QString color;
        if (locked)       color = " color: #7a7d80;";   // greyed: tier-locked (wins)
        else if (cooling) color = " color: #7a7d80;";   // greyed: on add-cooldown
        else if (sharing) color = " color: #40c060;";   // green: someone is sharing
        b->setCursor((locked || cooling) ? Qt::ArrowCursor : Qt::PointingHandCursor);
        b->setStyleSheet(QString(
            "QPushButton { padding-left: 4px; padding-right: 4px;%1 }").arg(color));
    }

    // Begin a 1s add-cooldown on a header button: set its flag, grey it now via the
    // shared style pass, and schedule the flag-clear + restyle. The single-shot uses
    // the dock (`this`) as context so it auto-cancels if the dock is destroyed; the
    // flag is a dock member (pointer captured by value), so the reference stays valid
    // for the timer's lifetime. Restyle goes through UpdateHeaderState so the resting
    // style is recomputed, never blindly reset.
    void BeginHeaderCooldown(bool* flag) {
        *flag = true;
        UpdateHeaderState();
        QTimer::singleShot(kAddCooldownMs, this, [this, flag]() {
            *flag = false;
            UpdateHeaderState();
        });
    }

    // Clicking a tier-locked header button opens the shared upgrade dialog
    // (ShowTierLimitDialog + the ShouldShowTierPopup throttle) — the same modal
    // used by the screenshare/chat create paths, with a clickable link to the
    // same upgrade URL as the dock cap chrome. `feature` is the source display
    // name; `tierWord` names the plan it needs (Basic / Streamer).
    static void ShowSourceUpgradePrompt(const QString& feature, const QString& tierWord) {
        if (!ShouldShowTierPopup()) return;
        // Explanation on one line, then the upgrade link on its own line below it as
        // a clear call-to-action (the <br><br> break) rather than jammed mid-sentence.
        ShowTierLimitDialog(
            QString("Feeds: %1").arg(feature),
            QString("%1 is a %2-tier feature.<br><br>"
                    "<a href=\"https://letsdovideo.com/feeds-upgrade\">Click here "
                    "to upgrade your plan</a>").arg(feature, tierWord));
    }

    // Re-style the header source buttons from current tier + share state. Called
    // from Refresh(), so tier/login/meeting/share changes (which all already route
    // through RefreshAllSourceProperties -> PostParticipantDockRefresh) update them
    // with no separate mechanism. The buttons are always visible (like "Create
    // Participant Feed"); gating is per-button greying, for discoverability/upgrade.
    void UpdateHeaderState() {
        if (!m_header) return;

        const bool ssLocked   = g_currentTier < 1;   // screenshare: Basic+
        const bool chatLocked = g_currentTier < 2;   // overlay/popup: Streamer+

        // Screenshare label turns green while someone in the meeting is sharing and
        // the button is within tier (never on a tier-locked button — greying wins).
        // Same signal the old status dot used (g_activeSharerUserId); its
        // share_status_changed handler already marshals a dock refresh here, so no
        // new mechanism.
        const bool sharing = !ssLocked && g_activeSharerUserId != 0;

        StyleHeaderButton(m_hdrScreenshare, ssLocked,   sharing, m_cdScreenshare);
        StyleHeaderButton(m_hdrChatOverlay, chatLocked, false,   m_cdChatOverlay);
        StyleHeaderButton(m_hdrChatPopup,   chatLocked, false,   m_cdChatPopup);
    }

    // Small right-aligned per-row button: add THIS source to the current edit
    // scene as a Paste Reference (new scene-item on the same source). A separate
    // sibling from the header label, so it doesn't collide with the rename
    // double-click. Shared by the live and non-live box header rows.
    QPushButton* MakeAddToSceneButton(const std::string& uuid) {
        QPushButton* b = new QPushButton("+");
        b->setFixedSize(18, 18);
        b->setToolTip("Add to Current Scene");
        b->setCursor(Qt::PointingHandCursor);
        // Track this row's current "+" by uuid (rebuilt each Refresh) so the
        // cooldown-expiry restyle reaches whichever button is live, and re-grey a
        // freshly-built button if its uuid is still cooling across a rebuild. The
        // cooling set (m_cdRows) is the enforcement — it survives rebuilds; the
        // greying is just its feedback.
        m_rowAddButtons[uuid] = b;
        StyleRowAddButton(b, m_cdRows.count(uuid) != 0);
        QObject::connect(b, &QPushButton::clicked, [this, uuid]() {
            if (m_cdRows.count(uuid)) return;   // on add-cooldown: absorb the rapid re-click
            AddSourceReferenceToCurrentScene(uuid);
            m_cdRows.insert(uuid);
            if (auto it = m_rowAddButtons.find(uuid);
                it != m_rowAddButtons.end() && it->second)
                StyleRowAddButton(it->second, true);
            QTimer::singleShot(kAddCooldownMs, this, [this, uuid]() {
                m_cdRows.erase(uuid);
                auto it = m_rowAddButtons.find(uuid);
                if (it != m_rowAddButtons.end() && it->second)
                    StyleRowAddButton(it->second, false);
            });
        });
        return b;
    }

    // Row "+" resting style (bold, no padding) vs. cooling (greyed to #7a7d80,
    // matching the other cooled/locked buttons). One place so the cooldown reverts
    // to the exact resting look.
    static void StyleRowAddButton(QPushButton* b, bool cooling) {
        if (!b) return;
        b->setStyleSheet(cooling
            ? "QPushButton { padding: 0; font-weight: bold; color: #7a7d80; }"
            : "QPushButton { padding: 0; font-weight: bold; }");
    }

    // Small icon button matched to the "+" above (18px slot), for the provisional
    // header-row cluster. Separate sibling from the header label, so it doesn't
    // collide with the double-click rename. onClick is a plain functor.
    static QPushButton* MakeIconButton(const QIcon& icon, const QString& tooltip,
                                       std::function<void()> onClick) {
        QPushButton* b = new QPushButton();
        b->setIcon(icon);
        b->setIconSize(QSize(12, 12));
        b->setFixedSize(18, 18);
        b->setToolTip(tooltip);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet("QPushButton { padding: 0; }");
        QObject::connect(b, &QPushButton::clicked, onClick);
        return b;
    }

    // A theme-adaptive drawn glyph, used only as an icon fallback (palette color,
    // so it reads on dark and light). painter(p, w) draws in a wxw box.
    static QIcon DrawnGlyph(std::function<void(QPainter&, int)> painter) {
        const int sz = 16;
        const QColor fg = qApp ? qApp->palette().color(QPalette::WindowText)
                               : QColor(200, 200, 200);
        QPixmap pm(sz, sz);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(fg);
        pen.setWidthF(1.6);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        painter(p, sz);
        p.end();
        return QIcon(pm);
    }

    // Scenes-list button icon: OBS's own scene source-type icon, exposed as a
    // readable Q_PROPERTY on the main window (theme-correct, license-clean). Falls
    // back to a drawn stacked-frames glyph if the property is ever absent. Cached;
    // a one-time log records which path was taken.
    static const QIcon& ScenesIcon() {
        static QIcon cached;
        static bool  built = false;
        if (built) return cached;
        built = true;
        if (QWidget* mw = (QWidget*)obs_frontend_get_main_window())
            cached = qvariant_cast<QIcon>(mw->property("sceneIcon"));
        const bool fromObs = !cached.isNull();
        if (!fromObs) {
            cached = DrawnGlyph([](QPainter& p, int) {
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(QRectF(2.5, 5.5, 8, 7), 1.5, 1.5);   // front frame
                p.drawRoundedRect(QRectF(5.5, 2.5, 8, 7), 1.5, 1.5);   // back frame
            });
        }
        blog(LOG_INFO, "[feeds] scenes-button icon: %s",
             fromObs ? "OBS sceneIcon" : "drawn fallback");
        return cached;
    }

    // Filters button icon: OBS's own filter glyph. It's not a main-window
    // Q_PROPERTY (unlike sceneIcon) — it's the compiled Qt resource the source
    // toolbar uses — so load it by resource path, checking a rendered pixmap
    // (QIcon(path).isNull() is unreliable for a missing resource). Falls back to a
    // drawn funnel. Cached; one-time log records the path.
    static const QIcon& FiltersIcon() {
        static QIcon cached;
        static bool  built = false;
        if (built) return cached;
        built = true;
        QIcon obsIcon(":/res/images/filter.svg");
        const bool fromObs = !obsIcon.pixmap(16, 16).isNull();
        if (fromObs) {
            cached = obsIcon;
        } else {
            cached = DrawnGlyph([](QPainter& p, int) {
                p.drawLine(QPointF(3, 4),  QPointF(13, 4));    // funnel: wide
                p.drawLine(QPointF(5, 8),  QPointF(11, 8));    //         mid
                p.drawLine(QPointF(7, 12), QPointF(9, 12));    //         narrow
            });
        }
        blog(LOG_INFO, "[feeds] filters-button icon: %s",
             fromObs ? "OBS :/res/images/filter.svg" : "drawn fallback");
        return cached;
    }

    // The provisional per-source header-row action cluster, shared by the live and
    // non-live boxes: [ + add-to-scene | scenes-list | filters ]. Placement is
    // temporary — this crowds the row and the eliding name loses width on a narrow
    // dock (accepted for now; the row gets redesigned). Each button is a separate
    // sibling from the header label, so none collides with the double-click rename.
    void AppendHeaderButtons(QHBoxLayout* headerL, const std::string& uuid) {
        headerL->addWidget(MakeAddToSceneButton(uuid));
        headerL->addWidget(MakeIconButton(ScenesIcon(), "Included Scenes",
            [uuid]() { ShowIncludedScenesMenu(uuid); }));
        headerL->addWidget(MakeIconButton(FiltersIcon(), "Filters",
            [uuid]() { OpenSourceFilters(uuid); }));
    }

    // --- Multi-source guidance (shared collision computation) ------------------
    // Two maps from an assignment (real userId, or Active Speaker sentinel 1) to a
    // source count, built once per Refresh from the row snapshot and threaded into
    // each box: pidCount over ALL sources (live, non-live, over-cap) drives the
    // dropdown "already used" reservation treatment; livePidCount over only
    // live-present, non-self sources (BoundUserId != 0) drives the collision
    // warning. A refresh (which every assignment change already triggers via
    // zp_update -> PostParticipantDockRefresh) recomputes both everywhere for free
    // — no separate targeted update.

    // For a dropdown in a source whose current pick is ownPid: is option q
    // "used by another source"? Unselect (0) never is; the source's OWN current
    // pick (q == ownPid) is never demoted (it's the current value). Otherwise q is
    // used elsewhere iff any source is set to it (q != ownPid, so any such source
    // is a different one). Active Speaker (1) is treated exactly like a participant.
    static bool UsedByOtherSource(long long q, long long ownPid,
                                  const std::map<long long, int>& pidCount) {
        if (q <= 0 || q == ownPid) return false;
        auto it = pidCount.find(q);
        return it != pidCount.end() && it->second >= 1;
    }

    // Does this source's assignment collide with another source's? pid>0 and at
    // least two LIVE-present, non-self sources share the same raw pid (this one plus
    // another). Callers pass livePidCount, so stale/absent/self assignments never
    // count — a source only warns once its participant is actually present.
    static bool SourceCollides(long long pid, const std::map<long long, int>& livePidCount) {
        if (pid <= 0) return false;
        auto it = livePidCount.find(pid);
        return it != livePidCount.end() && it->second >= 2;
    }

    // A quiet amber warning glyph (outline triangle + exclamation), drawn so it's
    // theme-agnostic and semi-transparent like the other indicators. Cached.
    static const QPixmap& WarningPixmap() {
        static QPixmap cached;
        static bool    built = false;
        if (built) return cached;
        built = true;
        const int sz = 14;
        qreal dpr = 1.0;
        if (QScreen* s = (qApp ? qApp->primaryScreen() : nullptr))
            dpr = s->devicePixelRatio();
        cached = QPixmap((int)(sz * dpr), (int)(sz * dpr));
        cached.setDevicePixelRatio(dpr);
        cached.fill(Qt::transparent);
        const qreal w = sz;
        const QColor amber(230, 150, 60, 235);
        QPainter p(&cached);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(amber);
        pen.setWidthF(1.3);
        pen.setJoinStyle(Qt::RoundJoin);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(QPointF(w * 0.50, w * 0.12), QPointF(w * 0.92, w * 0.86));  // triangle
        p.drawLine(QPointF(w * 0.92, w * 0.86), QPointF(w * 0.08, w * 0.86));
        p.drawLine(QPointF(w * 0.08, w * 0.86), QPointF(w * 0.50, w * 0.12));
        p.drawLine(QPointF(w * 0.50, w * 0.40), QPointF(w * 0.50, w * 0.62));  // "!" stem
        p.drawPoint(QPointF(w * 0.50, w * 0.74));                              // "!" dot
        p.end();
        return cached;
    }

    // Plain-value snapshot of one participant source (no OBS/Qt handles retained).
    struct Row {
        std::string uuid;        // stable source id — the pick handler resolves
                                 // combo -> source by this (name can change/collide)
        std::string name;        // OBS source name (copied — pointer not owned)
        long long   pid = 0;     // participant_id setting
        bool        disabled = false;  // tier_disabled (authoritative only when logged in)
        bool        liveNow = false;   // receiving real frames now (for the live dot)
    };

    // In-place per-row indicator handles, keyed by source uuid, rebuilt on each
    // Refresh() alongside the boxes and cleared in ClearContent(). Between
    // refreshes the poll/status paths update a row's indicator by uuid without a
    // full rebuild. `live` (and, later, a mute icon) are owned by their box —
    // this map only borrows the pointers, so it is cleared, never deleted.
    struct RowIndicators {
        QLabel*   live      = nullptr; // the connection dot (fixed 10px slot)
        int       dotCode   = -1;      // last-applied dot render code (-1 = none applied yet)
        QLabel*   mute      = nullptr; // always-on mic (fixed slot; grey none / red muted / green unmuted)
        int       micTone   = -1;      // last-applied mic tone (-1 = none applied yet)
        long long pid       = 0;       // bound userId, for userId -> row resolution
    };
    std::map<std::string, RowIndicators> m_rowIndicators;

    // "Live" = a real frame within this window. 500ms comfortably spans a 30fps
    // (~33ms) or 60fps gap; a camera-off or stall stops advancing the tick and
    // the dot flips off within ~this window plus one poll interval.
    static constexpr uint64_t kLiveThresholdNs = 500000000ULL;

    // Resolve a row's effective userId: an Active-Speaker row (pid 1) follows the
    // current speaker (0 before anyone speaks); pid > 1 is a real userId; pid <= 0
    // is unbound. 0 means "no participant resolvable". UI thread (reads
    // g_activeSpeakerUserId, UI-thread-owned).
    static unsigned int EffectiveUserId(long long pid) {
        if (pid == 1) return g_activeSpeakerUserId;   // 0 if no speaker yet
        if (pid > 1)  return (unsigned int)pid;
        return 0;                                     // unbound / unselected
    }

    // The userId this row is LIVE-bound to right now, or 0 if none. Resolves the
    // effective userId (Active Speaker -> current speaker), then requires it to be
    // an actually-present participant — otherwise a participant_id persisted from a
    // prior session (a stale runtime id after an OBS restart) would read as a live
    // participant when nobody is there. Presence is checked against g_muteByUserId,
    // which is seeded wholesale from the roster on every participant_list_changed
    // (and cleared on meeting end/leave), so its keys are exactly the present
    // participants: empty across a fresh restart, and pruned when a participant
    // leaves. This is the "no participant" gate shared by the dot, the mic, and the
    // collision warning, so all three fall back to their hollow/grey/no-warning
    // states together, driven off live binding rather than the persisted setting.
    // Self (g_cachedMyUserId) is also excluded — never a renderable participant feed
    // — so a source resolving to the local user falls back the same way. UI-thread-
    // owned reads, no lock.
    static unsigned int BoundUserId(long long pid) {
        unsigned int uid = EffectiveUserId(pid);
        if (uid == 0) return 0;
        // Self is never a participant feed: a stale persisted participant_id (or an
        // Active-Speaker row following self) can resolve to the local user, who sits
        // in the present-set (g_muteByUserId) and would otherwise pass the presence
        // gate below and light the indicators for a source that renders nothing.
        // g_cachedMyUserId is UI-thread-read here (same as g_activeSpeakerUserId in
        // EffectiveUserId, and the value the dropdown filter already excludes on).
        if (g_cachedMyUserId != 0 && uid == g_cachedMyUserId) return 0;
        return (g_muteByUserId.count(uid) > 0) ? uid : 0;   // 0 if not currently present
    }

    // Best-known quality level for a userId: prioritize the video-uplink leg (how
    // well the guest's video reaches us), then fall back to the other legs so a
    // dot still colors if uplink-video never fires. 0 = no level known yet.
    static int QualityLevelForUser(unsigned int uid) {
        auto it = g_connQualityByUserId.find(uid);
        if (it == g_connQualityByUserId.end()) return 0;
        const ConnQualityLegs& q = it->second;
        if (q.videoUp)   return q.videoUp;
        if (q.videoDown) return q.videoDown;
        if (q.audioUp)   return q.audioUp;
        if (q.audioDown) return q.audioDown;
        return 0;
    }

    // The four-case dot resolution, as a single render code so one comparison
    // covers both color and tooltip level (skip redundant restyles):
    //   0            -> case 1: no LIVE participant -> hollow no-participant ring
    //   1            -> case 2: participant present but not receiving frames -> dark
    //   2            -> case 3: receiving, no quality level yet -> grey
    //   10 + level   -> case 4: receiving + known ConnectionQuality (level 1..6)
    // "Not receiving" (case 2) always beats a stale color, so a source going dark
    // can't linger red/yellow/green. BoundUserId gates case 1 on live binding, so a
    // stale persisted participant_id (no present participant) reads as case 1.
    static int ResolveDotCode(long long pid, bool receiving) {
        unsigned int uid = BoundUserId(pid);
        if (uid == 0)     return 0;               // case 1
        if (!receiving)   return 1;               // case 2
        int level = QualityLevelForUser(uid);
        if (level <= 0)   return 2;               // case 3
        return 10 + level;                        // case 4
    }

    // Paint the dot for a render code. Fixed 10px slot always present (like the
    // mute mark): the no-dot case paints transparent, never hides/collapses, so
    // show/hide is paint-only with no row reflow. Colors are semi-transparent so
    // they read on dark and light OBS themes. Tooltip is word-level only — the
    // SDK exposes no per-guest numeric telemetry, so we never imply kbps/ms/loss.
    static void ApplyDot(QLabel* dot, int code) {
        if (!dot) return;
        QString css, tip;
        if (code == 0) {              // case 1: no participant assigned
            // Hollow outline: transparent center + thin grey ring — reads "empty".
            // Deliberately distinct from the camera-off dot below, which is FILLED
            // dark (present but dark). Empty vs filled is the at-a-glance cue; the
            // ring is visible on both dark and light themes.
            css = "QLabel { background: transparent;"
                  " border: 1px solid rgba(150,150,150,0.65); border-radius: 5px; }";
            tip = "No participant";
        } else if (code == 1) {       // case 2: present but dark (camera off)
            // FILLED dark disc + a thin ring so it still reads on a dark OBS theme
            // (a pure-black dot would vanish) and reads "present but dark" — filled,
            // not the hollow no-participant ring above. Border draws inside the
            // fixed 10px, so no size change vs the other states — no reflow.
            css = "QLabel { background: rgba(30,30,30,0.90);"
                  " border: 1px solid rgba(150,150,150,0.55); border-radius: 5px; }";
            tip = "Camera off";
        } else if (code == 2) {       // case 3: receiving, quality unknown
            css = "QLabel { background: rgba(128,128,128,0.55); border-radius: 5px; }";
            tip = "Receiving video";
        } else {                      // case 4: known level 1..6
            const int level = code - 10;
            const char* color = (level >= 5) ? "rgba(64,192,96,0.95)"    // Good/Excellent
                              : (level >= 3) ? "rgba(220,180,60,0.95)"   // Not_Good/Normal
                                             : "rgba(210,70,70,0.95)";   // Very_Bad/Bad
            css = QString("QLabel { background: %1; border-radius: 5px; }").arg(color);
            const char* word = level == 6 ? "Excellent"
                             : level == 5 ? "Good"
                             : level == 4 ? "Normal"
                             : level == 3 ? "Fair"
                             : level == 2 ? "Poor"
                                          : "Very poor";   // level 1
            tip = QString("Connection: %1").arg(word);
        }
        dot->setStyleSheet(css);
        dot->setToolTip(tip);
    }

    // Connection-dot poll (UI thread, ~every 400ms). Reads per-source
    // lastRealFrameTick under g_sourcesMutex (atomics only — no widget work while
    // holding the lock), then resolves all four dot cases in ONE place per tick,
    // folding in g_connQualityByUserId (and g_activeSpeakerUserId via the row's
    // pid). This is the single dot update path — the participant_conn_quality
    // event only updates the map; the dot picks the change up on the next tick
    // (quality isn't sub-second-critical). Takes ONLY g_sourcesMutex, briefly, so
    // no lock-ordering interaction with g_participantsMutex. Skips when hidden.
    void PollLiveIndicators() {
        if (!isVisible() || m_rowIndicators.empty()) return;

        const uint64_t now = os_gettime_ns();
        std::map<std::string, bool> liveByUuid;
        {
            std::lock_guard<std::mutex> lock(g_sourcesMutex);
            for (ZpSourceData* s : g_allParticipantSources) {
                if (!s) continue;
                uint64_t tick = s->lastRealFrameTick.load(std::memory_order_relaxed);
                liveByUuid[s->uuid] =
                    (tick != 0) && (now - tick) < kLiveThresholdNs;
            }
        }
        for (auto& kv : m_rowIndicators) {
            auto it = liveByUuid.find(kv.first);
            const bool receiving = (it != liveByUuid.end()) && it->second;
            const int  code = ResolveDotCode(kv.second.pid, receiving);
            if (code != kv.second.dotCode) {
                ApplyDot(kv.second.live, code);
                kv.second.dotCode = code;
            }
        }
    }

    // Resolve a row's mute state (UI thread). The Active-Speaker row uses the
    // sentinel participant_id 1: it has no fixed userId, so its mute follows the
    // *current* active speaker (g_activeSpeakerUserId) — muted iff that user is
    // muted, and no mark before anyone speaks (id 0). pid <= 0 is a genuinely
    // unbound row -> no mark. pid > 1 is a real userId -> direct lookup. Unknown
    // (no map entry) reads unmuted -> no mark. (MicToneForPid gates on BoundUserId
    // first, which now excludes self, so the mic never reaches here for a self-
    // resolved Active-Speaker row — it reads grey/no-participant like any other
    // self binding, consistent with self rendering no feed.)
    static bool MutedForPid(long long pid) {
        if (pid == 1) {                             // Active Speaker
            if (g_activeSpeakerUserId == 0) return false;   // no speaker yet
            pid = (long long)g_activeSpeakerUserId;
        }
        if (pid <= 0) return false;                 // unbound
        auto it = g_muteByUserId.find((unsigned int)pid);
        return it != g_muteByUserId.end() && it->second;
    }

    // Mic tone: 0 = grey (no participant assigned), 1 = red (muted), 2 = green
    // (unmuted). Kept in sync with the dot's no-participant state via the shared
    // EffectiveUserId(pid) == 0 check, so mic and dot show "nobody assigned"
    // together (grey mic + hollow dot).
    enum { MicGrey = 0, MicRed = 1, MicGreen = 2 };

    // The mute mic pixmap, tinted by tone. OBS's own Audio-Input microphone icon
    // is a readable Q_PROPERTY on the main window, so we get the theme-rendered
    // QIcon with no OBSBasic header/link and no QtSvg dependency. The glyph is
    // alpha-shape only, so we tint by compositing the color into its alpha
    // (SourceIn) — identical on dark/light and at HiDPI. If the property read
    // yields a null icon, we fall back to an asset-free drawn mic. All three tones
    // are built once (from one icon read) and cached; a one-time log records the path.
    static const QPixmap& MicPixmap(int tone) {
        static QPixmap cache[3];
        static bool    built = false;
        if (!built) {
            built = true;
            const QColor colors[3] = { QColor(150, 150, 150),   // grey  (no participant)
                                       QColor(220, 50, 50),      // red   (muted)
                                       QColor(64, 180, 96) };    // green (unmuted)
            QIcon mic;
            if (QWidget* mw = (QWidget*)obs_frontend_get_main_window())
                mic = qvariant_cast<QIcon>(mw->property("audioInputIcon"));
            const bool haveObs = !mic.isNull();
            blog(LOG_INFO, "[feeds] mute-mic icon source: %s",
                 haveObs ? "OBS audioInputIcon (tinted)" : "drawn fallback");

            auto tint = [&](const QColor& c) -> QPixmap {
                if (haveObs) {
                    // Keep the glyph shape (and its HiDPI pixmap / DPR), replace
                    // its color with c via SourceIn.
                    QPixmap src = mic.pixmap(QSize(kMuteSlotPx, kMuteSlotPx));
                    QPixmap out(src.size());
                    out.setDevicePixelRatio(src.devicePixelRatio());
                    out.fill(Qt::transparent);
                    QPainter p(&out);
                    p.drawPixmap(0, 0, src);
                    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
                    p.fillRect(out.rect(), c);
                    p.end();
                    return out;
                }
                // Drawn fallback: capsule body + stem + base, at screen DPR.
                qreal dpr = 1.0;
                if (QScreen* s = (qApp ? qApp->primaryScreen() : nullptr))
                    dpr = s->devicePixelRatio();
                const int px = (int)(kMuteSlotPx * dpr);
                QPixmap out(px, px);
                out.setDevicePixelRatio(dpr);
                out.fill(Qt::transparent);
                const qreal w = kMuteSlotPx, h = kMuteSlotPx;
                QPainter p(&out);
                p.setRenderHint(QPainter::Antialiasing, true);
                p.setPen(Qt::NoPen);
                p.setBrush(c);
                p.drawRoundedRect(QRectF(w * 0.34, h * 0.10, w * 0.32, h * 0.48),
                                  w * 0.16, w * 0.16);
                QPen pen(c);
                pen.setWidthF(w * 0.10);
                pen.setCapStyle(Qt::RoundCap);
                p.setPen(pen);
                p.drawLine(QPointF(w * 0.50, h * 0.66), QPointF(w * 0.50, h * 0.86));
                p.drawLine(QPointF(w * 0.34, h * 0.88), QPointF(w * 0.66, h * 0.88));
                p.end();
                return out;
            };
            for (int i = 0; i < 3; ++i) cache[i] = tint(colors[i]);
        }
        return cache[(tone >= 0 && tone < 3) ? tone : MicGrey];
    }

    // Resolve a row's mic tone from the shared no-participant gate + mute state:
    // no LIVE participant (BoundUserId == 0) -> grey; else muted -> red, unmuted
    // -> green. Same BoundUserId gate the dot uses for its hollow no-participant
    // ring, so the two indicators agree by construction — including reading grey
    // for a stale persisted participant_id that maps to nobody present.
    static int MicToneForPid(long long pid) {
        if (BoundUserId(pid) == 0) return MicGrey;
        return MutedForPid(pid) ? MicRed : MicGreen;
    }

    // Always-on mic indicator: grey (no participant) / red (muted) / green
    // (unmuted). Painted every rebuild and flipped in place by RecomputeMuteMarks —
    // never hidden, so its fixed slot never reflows. Only the tone/tooltip changes.
    static void ApplyMic(QLabel* icon, int tone) {
        if (!icon) return;
        icon->setPixmap(MicPixmap(tone));
        icon->setToolTip(tone == MicGrey ? "No participant"
                       : tone == MicRed  ? "Muted"
                                         : "Unmuted");
    }

    // Warning glyph on a permanently-reserved fixed slot: paint the amber triangle
    // (+ guidance tooltip) when the source collides with another, paint nothing
    // when it doesn't. Reserved either way, so a collision appearing/clearing
    // across a rebuild never shifts the name or the buttons.
    static void SetWarnIcon(QLabel* icon, bool collides) {
        if (!icon) return;
        if (collides) {
            icon->setPixmap(WarningPixmap());
            // Rich text (<qt>…</qt>) so Qt word-wraps the tooltip to a sensible width
            // instead of rendering one long unwrapped line.
            icon->setToolTip(QString::fromUtf8(
                "<qt>This selection is also used by another source. To show one "
                "participant in multiple scenes, use a copy of the source "
                "(the + button) instead.</qt>"));
        } else {
            icon->setPixmap(QPixmap());
            icon->setToolTip(QString());
        }
    }

    // Recompute every row's mic tone in place from current UI-thread state
    // (g_muteByUserId + g_activeSpeakerUserId). Both triggers route through here: a
    // participant_audio_status update (after writing g_muteByUserId) and an
    // active_speaker_changed (after writing g_activeSpeakerUserId). Resolving per
    // row via MicToneForPid means an Active-Speaker row (pid 1) re-resolves through
    // the current speaker automatically — including flipping to/from grey as a
    // speaker appears or clears. No Refresh(), no rebuild; O(rows). Public.
public:
    void RecomputeMuteMarks() {
        for (auto& kv : m_rowIndicators) {
            const int tone = MicToneForPid(kv.second.pid);
            if (tone != kv.second.micTone) {
                ApplyMic(kv.second.mute, tone);
                kv.second.micTone = tone;
            }
        }
    }
private:

    // --- Inline source-box rename (double-click the header) --------------------
    //
    // BeginRename swaps the header ElidingLabel for a RenameLineEdit in the same
    // layout slot (so the live dot and mute mark don't move); Commit/Cancel swap
    // it back. While m_activeEdit is set, Refresh() defers (m_refreshPending), so
    // an unrelated roster-change rebuild can't destroy the field mid-type; the
    // deferred refresh is flushed when the edit ends. The commit itself renames
    // the OBS source, whose source_rename signal (OnSourceRenamed) rebuilds the
    // box with the new name — so on a real rename we let that be the single
    // rebuild and don't also flush the deferred one.

    void BeginRename(const std::string& uuid, ElidingLabel* header) {
        if (m_activeEdit) return;                      // one edit at a time
        if (!header || !header->parentWidget()) return;
        QBoxLayout* lay = qobject_cast<QBoxLayout*>(header->parentWidget()->layout());
        if (!lay) return;
        const int idx = lay->indexOf(header);
        if (idx < 0) return;

        // Authoritative current name from the source (not the elided label text).
        QString cur;
        if (obs_source_t* src = obs_get_source_by_uuid(uuid.c_str())) {
            const char* nm = obs_source_get_name(src);
            cur = nm ? QString::fromUtf8(nm) : QString();
            obs_source_release(src);
        }

        RenameLineEdit* edit = new RenameLineEdit(cur);
        edit->setObjectName("feedsRenameEdit");
        edit->setStyleSheet(
            "QLineEdit#feedsRenameEdit { background: rgba(128,128,128,0.18);"
            " border: 1px solid rgba(128,128,128,0.55); border-radius: 3px;"
            " padding: 2px 5px; font-weight: bold; }");

        // Swap header -> edit in the same slot, preserving the stretch factor so
        // the live dot (before) and mute mark (after) keep their positions.
        const int stretch = lay->stretch(idx);
        delete lay->replaceWidget(header, edit);       // frees the old wrapper item only
        if (stretch > 0) lay->setStretch(lay->indexOf(edit), stretch);
        header->hide();

        edit->onCommit = [this, uuid, header](const QString& t) {
            CommitRename(uuid, header, t);
        };
        edit->onCancel = [this, header]() { CancelRename(header); };
        m_activeEdit = edit;
        edit->setFocus(Qt::MouseFocusReason);
        edit->selectAll();
    }

    // Restore the header in the editor's slot and dispose of the editor. Clears
    // m_activeEdit FIRST so any Refresh triggered hereafter runs normally.
    void EndRenameUi(ElidingLabel* header) {
        RenameLineEdit* edit = m_activeEdit;
        if (!edit) return;
        m_activeEdit = nullptr;

        QWidget*    parent = edit->parentWidget();
        QBoxLayout* lay = parent ? qobject_cast<QBoxLayout*>(parent->layout()) : nullptr;
        if (lay && header) {
            const int idx     = lay->indexOf(edit);
            const int stretch = idx >= 0 ? lay->stretch(idx) : 0;
            delete lay->replaceWidget(edit, header);
            if (stretch > 0) lay->setStretch(lay->indexOf(header), stretch);
        }
        if (header) header->show();
        edit->Detach();
        edit->hide();
        edit->deleteLater();
    }

    // OBS's own localized string for a frontend key (e.g. "NameExists.Text"), so
    // our dialogs read identically to a Sources-dock rename and in the user's
    // language. Null-guarded: falls back to the English literal if the key is
    // ever absent.
    static QString ObsLocaleStr(const char* key, const char* fallback) {
        const char* s = obs_frontend_get_locale_string(key);
        return QString::fromUtf8((s && *s) ? s : fallback);
    }

    void CommitRename(const std::string& uuid, ElidingLabel* header,
                      const QString& newText) {
        EndRenameUi(header);                           // revert UI first (mirrors OBS)
        bool renamed = false;

        // Reproduce OBS's rename validation exactly (SourceTreeItem::
        // ExitEditModeInternal), on the RAW editor text — OBS does not trim, so a
        // whitespace-only name is a valid rename. Same checks, same order, and the
        // dialog text comes from OBS's own localization so it matches a Sources-
        // dock rename. OBS imposes no length or character rules, so we add none.
        QWidget*          mainWin = (QWidget*)obs_frontend_get_main_window();
        const std::string raw     = newText.toUtf8().constData();  // untrimmed

        if (raw.empty()) {
            // Empty name -> reject, revert.
            QMessageBox::information(
                mainWin,
                ObsLocaleStr("NoNameEntered.Title", "Please enter a valid name"),
                ObsLocaleStr("NoNameEntered.Text",  "You cannot use empty names."));
        } else if (obs_source_t* src = obs_get_source_by_uuid(uuid.c_str())) {
            const char* curC = obs_source_get_name(src);
            if (curC && raw == curC) {
                // Unchanged -> silent no-op, no dialog (OBS returns here).
            } else {
                // A DIFFERENT source already using the name -> reject, revert. The
                // clash != src guard means renaming to our own name is never a
                // collision (case above catches the exact-same-name path first).
                obs_source_t* clash = obs_get_source_by_name(raw.c_str());
                if (clash && clash != src) {
                    QMessageBox::information(
                        mainWin,
                        ObsLocaleStr("NameExists.Title", "Name already exists"),
                        ObsLocaleStr("NameExists.Text",  "The name is already in use."));
                } else {
                    obs_source_set_name(src, raw.c_str());
                    renamed = true;   // source_rename -> OnSourceRenamed -> refresh
                }
                if (clash) obs_source_release(clash);
            }
            obs_source_release(src);
        }

        // Flush a refresh deferred during the edit. If we renamed, the
        // source_rename rebuild already reflects current state (roster included),
        // so don't fire a redundant second rebuild.
        if (m_refreshPending) { m_refreshPending = false; if (!renamed) Refresh(); }
    }

    void CancelRename(ElidingLabel* header) {
        EndRenameUi(header);
        if (m_refreshPending) { m_refreshPending = false; Refresh(); }
    }

    // Build one enabled source's framed box. Live -> header + functional combo;
    // non-live -> the same box, frame dimmed and combo-less (header + "Waiting
    // for participants…"). A QFrame
    // container (not QGroupBox: that renders inconsistently across OBS themes and
    // its title isn't a swappable widget for the later rename). Frame border and
    // header fill use semi-transparent neutral greys so one rule reads on both
    // dark and light themes; the non-live frame is dimmed (lower alpha) so a
    // greyed source doesn't read as more prominent than an active one.
    QWidget* MakeSourceBox(const Row& r, bool live, unsigned int myId,
                           const std::map<unsigned int, std::string>& roster,
                           const std::map<long long, int>& pidCount,
                           const std::map<long long, int>& livePidCount) {
        QFrame* box = new QFrame();
        box->setObjectName("feedsSourceBox");
        box->setStyleSheet(QString(
            "QFrame#feedsSourceBox { border: 1px solid rgba(128,128,128,%1);"
            " border-radius: 4px; }").arg(live ? "0.40" : "0.22"));

        QVBoxLayout* boxL = new QVBoxLayout(box);
        boxL->setContentsMargins(8, 6, 8, 8);
        boxL->setSpacing(4);   // tight header <-> combo so the box reads as one unit

        // Shared header state. collides drives the reserved warning slot; dotCode
        // and micTone seed the connection dot and the always-on mic. All three key
        // off the same self-excluding live-binding gate: the dot's no-participant
        // case (dotCode 0) and the grey mic tone via BoundUserId(pid) == 0, and the
        // warning via livePidCount (which is itself built from BoundUserId != 0), so
        // the row reads consistently — hollow dot + grey mic + no warning whenever it
        // isn't live-bound to a present, non-self participant.
        const bool collides = SourceCollides(r.pid, livePidCount);
        const int  dotCode  = ResolveDotCode(r.pid, r.liveNow);
        const int  micTone  = MicToneForPid(r.pid);

        // Header name — a swappable child QLabel (objectName so rename can
        // find/replace it). Elides to one line, normal color in every state.
        ElidingLabel* header = new ElidingLabel(QString::fromStdString(r.name));
        header->setObjectName("feedsSourceHeader");
        header->setStyleSheet(
            "QLabel#feedsSourceHeader { background: rgba(128,128,128,0.15);"
            " border-radius: 3px; padding: 3px 6px; font-weight: bold; }");
        // Double-click to rename the underlying OBS source. A separate sibling from
        // the clusters, so it can't interfere with the buttons/indicators.
        {
            const std::string uuid = r.uuid;
            header->onDoubleClick = [this, uuid, header]() { BeginRename(uuid, header); };
        }

        // Two-cluster header row, identical on live and non-live boxes:
        //   [ dot | mute | warn ]  [ name (stretch, elides) ]  [ + | scenes | filters ]
        // Left = status indicators (grouped, tight); right = action buttons; name
        // between. Every left-cluster slot is fixed-size and always present, so the
        // only thing that ever changes width is the elided name — nothing reflows on
        // a mute toggle (always shown) or a collision change (reserved warn slot).
        QWidget*     headerRow = new QWidget();
        QHBoxLayout* headerL   = new QHBoxLayout(headerRow);
        headerL->setContentsMargins(0, 0, 0, 0);
        headerL->setSpacing(8);   // gap between the three clusters

        QWidget*     left  = new QWidget();
        QHBoxLayout* leftL = new QHBoxLayout(left);
        leftL->setContentsMargins(0, 0, 0, 0);
        leftL->setSpacing(5);     // within the status cluster — keep the glyphs distinct
        QLabel* dot = new QLabel();
        dot->setFixedSize(10, 10);
        ApplyDot(dot, dotCode);   // round: green/yellow/red/black/none
        leftL->addWidget(dot);
        QLabel* mute = new QLabel();
        mute->setFixedSize(kMuteSlotPx, kMuteSlotPx);
        mute->setAlignment(Qt::AlignCenter);
        ApplyMic(mute, micTone);   // mic: grey none / green unmuted / red muted
        leftL->addWidget(mute);
        QLabel* warn = new QLabel();
        warn->setFixedSize(kMuteSlotPx, kMuteSlotPx);
        warn->setAlignment(Qt::AlignCenter);
        SetWarnIcon(warn, collides);   // amber triangle when colliding, else blank
        leftL->addWidget(warn);
        headerL->addWidget(left);

        headerL->addWidget(header, 1);          // middle: name takes the stretch
        AppendHeaderButtons(headerL, r.uuid);   // right: + add-to-scene | scenes | filters
        boxL->addWidget(headerRow);

        if (!live) {
            // Non-live: same header/clusters, but a muted "Waiting…" line in place
            // of the combo. The dot/mute/warn are painted once (static — the dock
            // is not in an active meeting state, so nothing updates them in place;
            // the non-live->live transition rebuilds). Occupies the combo's slot so
            // the box height stays stable across the flip.
            QLabel* status = new QLabel("Waiting for participants…");
            status->setStyleSheet("QLabel { color: #7a7d80; }");
            boxL->addWidget(status);
            return box;
        }

        // Live box: register the dot + mute for in-place updates (the poll and
        // RecomputeMuteMarks flip them without a rebuild), then build the combo.
        m_rowIndicators[r.uuid] = { dot, dotCode, mute, micTone, r.pid };

        // Functional combo. Current-selection handling + multi-source guidance:
        //  - the current pick is NOT a selectable list item (avoids showing the
        //    assigned participant twice — once as the closed value, once in the
        //    list). It's shown as the closed value via a hidden index-0 item;
        //    unassigned/absent shows a greyed "Select participant…" placeholder.
        //  - "None" is the clear option. Active Speaker is treated like a
        //    participant. Selections used by ANOTHER source are demoted below a
        //    non-selectable "── Already used ──" divider and greyed — still fully
        //    selectable (nothing is ever blocked).
        QComboBox* combo = new QComboBox();
        const long long ownPid = r.pid;

        // Closed-state display of the current pick (NOT added to the list itself).
        QString currentText;
        bool    haveCurrent = (ownPid != 0);
        if (ownPid == 1) {
            currentText = "[Active Speaker]";
        } else if (ownPid > 1) {
            // A stale persisted id that resolves to self is not a real pick — the
            // indicators already treat self as unbound (BoundUserId), so prompt here
            // too rather than showing the local user's own name as the current value.
            if (myId != 0 && (unsigned int)ownPid == myId) {
                haveCurrent = false;
            } else {
                auto it = roster.find((unsigned int)ownPid);
                if (it != roster.end()) currentText = QString::fromStdString(it->second);
                else haveCurrent = false;   // bound but absent -> prompt (as before)
            }
        }
        if (haveCurrent)
            combo->addItem(currentText, QVariant((qulonglong)ownPid));   // index 0, hidden below

        // Selectable list: None + AS (unless it's the current pick) + roster minus
        // self and minus the current pick, split normal / already-used.
        struct Cand { QString label; qulonglong data; };
        std::vector<Cand> normalC, usedC;
        normalC.push_back({ "None", 0 });   // clear option
        if (ownPid != 1) {
            Cand as{ "[Active Speaker]", 1 };
            (UsedByOtherSource(1, ownPid, pidCount) ? usedC : normalC).push_back(as);
        }
        for (const auto& kv : roster) {
            if (myId != 0 && kv.first == myId) continue;          // exclude self
            if ((long long)kv.first == ownPid)  continue;         // exclude current pick
            Cand c{ QString::fromStdString(kv.second), (qulonglong)kv.first };
            (UsedByOtherSource((long long)kv.first, ownPid, pidCount) ? usedC : normalC)
                .push_back(c);
        }
        for (const auto& c : normalC)
            combo->addItem(c.label, QVariant(c.data));
        if (!usedC.empty()) {
            combo->addItem(QString::fromUtf8("── Already used ──"));
            // Disable the divider so it isn't selectable (keyboard/mouse skip it).
            if (auto* model = qobject_cast<QStandardItemModel*>(combo->model()))
                if (QStandardItem* d = model->item(combo->count() - 1)) d->setEnabled(false);
            const QBrush grey(QColor(0x7a, 0x7d, 0x80));
            for (const auto& c : usedC) {
                combo->addItem(c.label, QVariant(c.data));
                combo->setItemData(combo->count() - 1, grey, Qt::ForegroundRole);
            }
        }

        // Closed display: select the hidden current item and hide its row from the
        // popup (shown closed, omitted from the list), or show the placeholder when
        // unassigned/absent. Set BEFORE connecting so this can't reach the handler.
        if (haveCurrent) {
            combo->setCurrentIndex(0);
            if (auto* lv = qobject_cast<QListView*>(combo->view()))
                lv->setRowHidden(0, true);
        } else {
            combo->setPlaceholderText(QString::fromUtf8("Select participant…"));
            combo->setCurrentIndex(-1);
        }

        // Connect to activated (genuine user interaction only) — never fires on
        // setCurrentIndex / addItem, so the programmatic setup above is inert. A
        // rebuild that recreates every combo can't trigger a write.
        const std::string uuid = r.uuid;
        QObject::connect(combo, QOverload<int>::of(&QComboBox::activated),
            [combo, uuid](int) {
                OnDockParticipantPicked(
                    uuid, (long long)combo->currentData().toULongLong());
            });
        boxL->addWidget(combo);
        return box;
    }

    // Derive a comfortable dock minimum width from the live theme/font: measure a
    // representative worst-case combo and source-name header (sizeHint, which
    // includes themed padding/frame/arrow at the current font), take the wider,
    // add box + root margins and a comfort margin. Recomputed per Refresh so it
    // tracks a theme/font change; the caller only re-applies it when it changes.
    static int ComputeMinDockWidth() {
        QComboBox probe;
        probe.addItem(QStringLiteral("Jonathan Featherstonehaugh (Guest)"));
        probe.ensurePolished();
        const int comboW = probe.sizeHint().width();

        QLabel hdr(QStringLiteral("Jonathan Featherstonehaugh Camera"));
        hdr.ensurePolished();
        const int hdrW = hdr.sizeHint().width();

        const int contentW = qMax(comboW, hdrW);
        // box contents margins (8+8) + frame (2) + root layout margins (8+8) + comfort.
        return contentW + 16 + 2 + 16 + 24;
    }

    // Remove every widget/spacer from the root layout so Refresh can rebuild.
    //
    // Widgets are DEFERRED-deleted (deleteLater), not deleted immediately.
    // Today's read-only widget set emits nothing re-entrant and every Refresh
    // arrives via a queued singleShot(0), so immediate delete would be safe now
    // — but Phase 2 adds an interactive QComboBox, and a teardown that fires
    // while that combo is mid-signal-emission must not free it out from under
    // its own executing slot. deleteLater defers the free until the current slot
    // unwinds and the event loop turns, making teardown safe regardless of what
    // the write path (or OBS internals it calls) does. This is forward-looking
    // hardening landed in isolation, before any interactive widget exists.
    //
    // takeAt removes the widget from layout management but leaves it parented to
    // the scrolled content widget at its last geometry, so a deferred-deleted
    // widget would render on top of the rebuilt content for one event-loop turn
    // (a ghost/overlap). hide() it immediately to stop that render now while the
    // free stays deferred; hide() only flips visibility (frees nothing), so it's
    // safe to call from a widget's own slot and preserves the point of deleteLater.
    //
    // No unbounded pending-delete pile: each Refresh runs to completion and
    // arrives via its own singleShot(0), so the event loop turns (processing
    // DeferredDelete events) between successive refreshes — at most ~one refresh
    // of widgets is ever awaiting free, even under rapid roster/meeting/tier churn.
    //
    // Enumerates only m_root (the inner content layout), so each item is a box /
    // divider / prompt / waiting-label at that level — never a box's nested
    // header or combo (nor the fixed header bar, which lives in the outer layout).
    // Each box is one QObject whose deleteLater frees its header
    // and combo transitively (they're parented into the box by its layout), so a
    // box's children are freed exactly once, none orphaned, no combo surviving.
    //
    // The QLayoutItem wrappers are not QObjects (no deleteLater) — keep deleting
    // those immediately; only the widget's own free is deferred.
    void ClearContent() {
        // Drop borrowed indicator pointers BEFORE deleting the boxes that own
        // them, so the poll can never touch a freed dot between clear and rebuild.
        m_rowIndicators.clear();
        // Row "+" pointers are borrowed too (their boxes own them); the cooldown
        // SET (m_cdRows) is deliberately NOT cleared here, so a row's cooldown
        // survives an unrelated rebuild and re-applies when the row is rebuilt.
        m_rowAddButtons.clear();
        // The create button is about to be deleted; drop the pointer so a pending
        // cooldown timer can't touch a freed widget if the rebuild omits it (e.g.
        // hidden at/over the tier cap). AppendCreateButton re-sets it when present.
        m_createBtn = nullptr;
        QLayoutItem* item;
        while ((item = m_root->takeAt(0)) != nullptr) {
            if (QWidget* w = item->widget()) {
                w->hide();
                w->deleteLater();
            }
            delete item;
        }
    }

    QVBoxLayout* m_root = nullptr;   // inner (scrolled) content layout; boxes live here
    int          m_minWidth = 0;     // last-applied theme-derived dock min width
    QTimer*      m_liveTimer = nullptr;  // live-indicator poll (parented to dock)

    // Fixed header bar (outer layout, pinned to the dock top — does not scroll).
    // Built once in the ctor; UpdateHeaderState() re-styles the source buttons in
    // place, and RebuildHeaderTopSlot() rebuilds the Connect/Login top slot each
    // Refresh. m_headerTopSlot is hidden (collapsed) whenever it holds no button.
    QWidget*           m_header          = nullptr;
    QWidget*           m_headerTopSlot   = nullptr;  // state-driven Connect/Login slot
    QVBoxLayout*       m_headerTopLayout = nullptr;  // top-slot layout (cleared each Refresh)
    ElidingPushButton* m_hdrScreenshare  = nullptr;
    ElidingPushButton* m_hdrChatOverlay  = nullptr;
    ElidingPushButton* m_hdrChatPopup    = nullptr;

    // --- Per-button add-cooldowns (1s each) -----------------------------------
    // After a button performs an add, it greys and no-ops for 1s to absorb
    // accidental rapid re-clicks. Per-button, so adding different sources in quick
    // succession stays fully allowed. Enforcement lives in these flags/set (which
    // survive the content rebuilds that add-actions can trigger), NOT on the
    // widgets; the greying composes with the existing style paths without
    // clobbering a button's resting state (tier-lock grey / screenshare green /
    // normal). Timers are QTimer::singleShot with the dock as context, so they
    // clean up with the dock.
    static constexpr int kAddCooldownMs = 1000;
    bool m_cdScreenshare = false;   // header: Screenshare
    bool m_cdChatOverlay = false;   // header: Chat Overlay
    bool m_cdChatPopup   = false;   // header: Chat Popup
    bool m_cdCreateFeed  = false;   // "Create Participant Feed" (rebuilt each Refresh)
    QPushButton* m_createBtn = nullptr;            // current Create-Feed button instance
    std::set<std::string> m_cdRows;                // row uuids currently cooling
    std::map<std::string, QPointer<QPushButton>> m_rowAddButtons;  // uuid -> current "+"

    // Inline rename state (UI thread). m_activeEdit != null means an edit is open:
    // Refresh() defers while it is, and m_refreshPending records that a deferred
    // refresh is owed (flushed when the edit ends). See BeginRename/EndRenameUi.
    RenameLineEdit* m_activeEdit     = nullptr;
    bool            m_refreshPending = false;
};

// Non-owning pointer to the participant dock — same ownership model as
// g_chatDock (OBS owns the widget after registration). Read as the QObject
// context for marshalled refreshes so a destroyed widget auto-cancels them.
static FeedsParticipantDock* g_participantDock = nullptr;

static void PostParticipantDockRefresh() {
    if (!g_participantDock) return;
    // Marshal onto the dock's (UI) thread. Using the dock itself as the context
    // object means the queued call auto-cancels if OBS destroys the widget —
    // the primary guard against a shutdown-time use-after-free, since source
    // destroy (which posts these) runs on OBS's destruction worker thread.
    QTimer::singleShot(0, g_participantDock, []() {
        if (g_participantDock) g_participantDock->Refresh();
    });
}

// ---------------------------------------------------------------------------
// Avatar cache
// ---------------------------------------------------------------------------
// Populated by the chat_message IPC handler as messages arrive (one entry
// per distinct sender_id), consumed by the chat popup source in a later
// Phase 2 commit. Keyed by Zoom SDK user ID; bounded by the number of
// distinct senders in a session (~tens), entries ~10KB each.
//
// Zoom SDK writes avatars to %APPDATA%\ZoomSDK\data\ConfAvatar\ — a
// per-user roaming location accessible to both engine and plugin
// processes — so the IPC carries the path itself rather than image bytes.
// QImage::load is thread-safe and runs off the IPC reader thread.
//
// GetAvatarPath() returns null for users without a profile picture
// (engine sends empty string in that case). Falls back to the bundled
// Feeds logo from the plugin's data directory. The fallback is loaded
// lazily on first lookup so we pay nothing if chat is never used.
// External linkage — feeds-chat-popup-source.cpp consumes these via
// extern declarations. Keeping them in plugin-main as the source of
// truth; the popup source is a pure consumer.
std::mutex                              g_avatarCacheMutex;
std::map<unsigned int, QImage>          g_avatarCache;
QImage                                  g_fallbackAvatar;
static std::once_flag                   g_fallbackAvatarOnce;

// Loads g_fallbackAvatar from the plugin data directory. Idempotent —
// call_once guarantees the load body runs exactly once across all
// threads, so callers don't need to coordinate. The chat IPC handler
// hits this via GetAvatarForSender on the first message; the popup
// source bypasses that path, so obs_module_load calls it eagerly to
// ensure the fallback is ready before any render fires.
static void EnsureFallbackAvatarLoaded() {
    std::call_once(g_fallbackAvatarOnce, []() {
        char* path = obs_module_file("feeds-logo.png");
        if (path) {
            if (!g_fallbackAvatar.load(QString::fromUtf8(path))) {
                blog(LOG_WARNING,
                     "[feeds] failed to load fallback avatar from %s",
                     path);
            }
            bfree(path);
        } else {
            blog(LOG_WARNING,
                 "[feeds] feeds-logo.png not found in plugin data dir");
        }
    });
}

static QImage GetAvatarForSender(unsigned int senderId,
                                 const std::string& avatarPath) {
    EnsureFallbackAvatarLoaded();

    std::lock_guard<std::mutex> lock(g_avatarCacheMutex);

    auto it = g_avatarCache.find(senderId);
    if (it != g_avatarCache.end()) return it->second;

    QImage img;
    if (!avatarPath.empty()) {
        img.load(QString::fromStdString(avatarPath));
    }
    // Null QImage means either no path or load failure. Cache the
    // fallback so we don't re-try the load on every message.
    if (img.isNull()) {
        img = g_fallbackAvatar;
    }
    g_avatarCache[senderId] = img;
    return img;
}

static void SetupChatDock() {
    // Stable id — never change this across versions; OBS uses it as the
    // key for the user's saved dock visibility/position state.
    g_chatDock = new FeedsChatDock();
    // Title is display-only; the id ("feeds_chat_dock") is the persistence key
    // for saved layout/visibility and MUST stay stable — so renaming the visible
    // title from "Zoom Chat" to "Feeds Chat" (now that it also carries YouTube
    // chat) does not disturb existing saved layouts.
    obs_frontend_add_dock_by_id(
        "feeds_chat_dock", "Feeds Chat", g_chatDock);
    blog(LOG_INFO, "[feeds] chat dock registered");
}

static void SetupParticipantDock() {
    // Stable id — never change this across versions (see SetupChatDock).
    g_participantDock = new FeedsParticipantDock();
    obs_frontend_add_dock_by_id(
        "feeds_participant_dock", "Feeds Controls", g_participantDock);
    blog(LOG_INFO, "[feeds] participant dock registered");
}

// Push the global ISO-recording state to every active participant source's
// recorder. The recorder itself also gates internally on tier >= 1, but
// gating here keeps the menu state and observable behavior consistent.
// MUST NOT be called while holding g_sourcesMutex (it acquires it here).
static void ApplyIsoRecordingStateToAllSources() {
    bool enabled = g_isoRecordingEnabled && g_currentTier >= 1;
    std::lock_guard<std::mutex> lock(g_sourcesMutex);
    for (ZpSourceData* s : g_allParticipantSources) {
        if (s && s->iso)
            feeds::feeds_iso_recorder_set_enabled(s->iso, enabled);
    }
}

// Sync the menu item's enabled state and label to the current tier. Free /
// logged-out shows greyed with the "paid feature" wording (matches the
// Chat Dock); paid shows enabled with the plain label.
static void UpdateIsoMenuItemForTier() {
    if (!g_isoRecordingAction) return;
    bool paid = g_currentTier >= 1;
    g_isoRecordingAction->setEnabled(paid);
    g_isoRecordingAction->setText(paid
        ? "ISO Recording (All Participants)"
        : "ISO Recording is a Paid Feature");
}

// Sync the collapsed Login/Logout item's label and enabled state.
// Settled labels follow g_isLoggedIn. While an auth round-trip is in
// flight (g_authInProgress):
//   - Logout shows "Logging out..." disabled (logout is fast; no cancel
//     affordance needed; disabled blocks the double-click double-fire).
//   - Login shows "Cancel login" enabled, so the user can escape the
//     in-progress state if they close the OAuth browser tab — the
//     engine never sends a termination message in that case, so without
//     this the menu would be stuck on "Logging in..." until OBS restart.
static void UpdateLoginLogoutMenuItem() {
    if (!g_loginLogoutAction) return;
    if (g_authInProgress) {
        if (g_isLoggedIn) {
            g_loginLogoutAction->setText("Logging out...");
            g_loginLogoutAction->setEnabled(false);
        } else {
            g_loginLogoutAction->setText("Cancel login");
            g_loginLogoutAction->setEnabled(true);
        }
        return;
    }
    g_loginLogoutAction->setText(g_isLoggedIn ? "Logout of Zoom" : "Login to Zoom");
    g_loginLogoutAction->setEnabled(true);
}

// Build and exec the About dialog. Stack-allocated; exec() blocks until the
// user closes it, so the dialog and its child widgets are torn down before
// we return — no WA_DeleteOnClose dance needed. Parent is the OBS main
// window so the dialog picks up OBS's stylesheet, stays modal-ish (no system
// alert sound, unlike QMessageBox), and dies with OBS rather than outliving
// it.
static void ShowAboutDialog() {
    std::string tierName;
    switch (g_currentTier) {
        case 1:  tierName = "Basic";       break;
        case 2:  tierName = "Streamer";    break;
        case 3:  tierName = "Broadcaster"; break;
        default: tierName = "Free";        break;
    }

    QDialog dlg(static_cast<QWidget*>(obs_frontend_get_main_window()));
    dlg.setWindowTitle(QString::fromUtf8("About Feeds"));
    dlg.setMinimumWidth(360);

    QVBoxLayout* layout = new QVBoxLayout(&dlg);
    layout->setSpacing(12);

    // Logo — reuse the same asset the chat dock uses for fallback avatars
    // (EnsureFallbackAvatarLoaded). bfree the path on every branch; the
    // pixmap then owns its own pixel data.
    QLabel* logo = new QLabel(&dlg);
    char* logoPath = obs_module_file("feeds-logo.png");
    if (logoPath) {
        QPixmap pixmap;
        if (pixmap.load(QString::fromUtf8(logoPath))) {
            logo->setPixmap(pixmap.scaledToHeight(96, Qt::SmoothTransformation));
        }
        bfree(logoPath);
    }
    logo->setAlignment(Qt::AlignCenter);
    layout->addWidget(logo);

    QString infoText = QString::fromUtf8("Feeds v") +
                       QString::fromUtf8(feeds_shared::VERSION);
    if (!g_userDisplayName.empty()) {
        infoText += "\n" + QString::fromUtf8("Logged in as: ") +
                    QString::fromUtf8(g_userDisplayName.c_str());
    }
    infoText += "\n" + QString::fromUtf8("Tier: ") +
                QString::fromUtf8(tierName.c_str());
    QLabel* info = new QLabel(infoText, &dlg);
    info->setAlignment(Qt::AlignCenter);
    layout->addWidget(info);

    // Link row — rich-text hyperlinks on a single label so they sit on one
    // line and inherit OBS's link color. setOpenExternalLinks routes the
    // clicks through QDesktopServices automatically.
    QLabel* links = new QLabel(
        QString::fromUtf8(
            "<a href=\"https://marketplace.zoom.us/apps/9ZWYWHNdRCSuI-9pFTuPJw\">"
            "View on Zoom Marketplace</a>"
            "&nbsp;&nbsp;·&nbsp;&nbsp;"
            "<a href=\"https://letsdovideo.com/feeds\">letsdovideo.com/feeds</a>"),
        &dlg);
    links->setAlignment(Qt::AlignCenter);
    links->setTextFormat(Qt::RichText);
    links->setOpenExternalLinks(true);
    layout->addWidget(links);

    QPushButton* closeBtn = new QPushButton(QString::fromUtf8("Close"), &dlg);
    closeBtn->setDefault(true);
    layout->addWidget(closeBtn, 0, Qt::AlignCenter);

    QLabel* footer = new QLabel(
        QString::fromUtf8("Made by Let's Do Video · 2026"), &dlg);
    footer->setAlignment(Qt::AlignCenter);
    footer->setStyleSheet("color: #888; font-size: 10px;");
    layout->addWidget(footer);

    QObject::connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    dlg.exec();
}

// ---------------------------------------------------------------------------
// Persistent global preferences
// ---------------------------------------------------------------------------
// Stored in OBS's user-level (global) frontend config, NOT the per-profile
// config — "Connect to Zoom on Startup" is a global preference. OBS 30+
// renamed the accessor from obs_frontend_get_global_config (now deprecated)
// to obs_frontend_get_user_config; Feeds builds against OBS 31, so we use the
// new one. This is Feeds' first persisted non-source preference.
static const char* kFeedsConfigSection      = "Feeds";
static const char* kCfgConnectOnStartup      = "ConnectOnStartup";
static const char* kCfgYouTubeHandle         = "YouTubeChannelHandle";
static const char* kCfgYouTubeVideoId        = "YouTubeVideoId";  // pinned stream (exclusive with handle)
static const char* kCfgTwitchChannel         = "TwitchChannel";   // bare lowercase login

static bool LoadConnectOnStartupSetting() {
    config_t* cfg = obs_frontend_get_user_config();
    if (!cfg) return false;
    // config_get_bool returns false for an absent key, which is our default.
    return config_get_bool(cfg, kFeedsConfigSection, kCfgConnectOnStartup);
}

static void SaveConnectOnStartupSetting(bool enabled) {
    config_t* cfg = obs_frontend_get_user_config();
    if (!cfg) return;
    config_set_bool(cfg, kFeedsConfigSection, kCfgConnectOnStartup, enabled);
    config_save(cfg);
}

static std::string LoadYouTubeHandleSetting() {
    config_t* cfg = obs_frontend_get_user_config();
    if (!cfg) return "";
    const char* s = config_get_string(cfg, kFeedsConfigSection, kCfgYouTubeHandle);
    return s ? s : "";
}

static void SaveYouTubeHandleSetting(const std::string& handle) {
    config_t* cfg = obs_frontend_get_user_config();
    if (!cfg) return;
    config_set_string(cfg, kFeedsConfigSection, kCfgYouTubeHandle, handle.c_str());
    config_save(cfg);
}

static std::string LoadYouTubeVideoIdSetting() {
    config_t* cfg = obs_frontend_get_user_config();
    if (!cfg) return "";
    const char* s = config_get_string(cfg, kFeedsConfigSection, kCfgYouTubeVideoId);
    return s ? s : "";
}

static void SaveYouTubeVideoIdSetting(const std::string& videoId) {
    config_t* cfg = obs_frontend_get_user_config();
    if (!cfg) return;
    config_set_string(cfg, kFeedsConfigSection, kCfgYouTubeVideoId, videoId.c_str());
    config_save(cfg);
}

static std::string LoadTwitchChannelSetting() {
    config_t* cfg = obs_frontend_get_user_config();
    if (!cfg) return "";
    const char* s = config_get_string(cfg, kFeedsConfigSection, kCfgTwitchChannel);
    return s ? s : "";
}

static void SaveTwitchChannelSetting(const std::string& channel) {
    config_t* cfg = obs_frontend_get_user_config();
    if (!cfg) return;
    config_set_string(cfg, kFeedsConfigSection, kCfgTwitchChannel, channel.c_str());
    config_save(cfg);
}

// ===========================================================================
// YouTube live-chat poller (Phase 1: receive -> Feeds chat dock only)
// ===========================================================================
// Plugin-side, engine-uninvolved. Runs whenever OBS runs (meeting or not).
// A single thread for the plugin's lifetime, gated internally on the enable
// flag + a configured @handle; when idle it blocks on the wake event. Protocol
// (verified in the investigation round, notes in C:\Dev\Greps\): GET /@handle/
// live -> scrape InnerTube config (clientVersion is date-stamped, scraped every
// bootstrap, never hardcoded) + the initial reload continuation -> POST
// get_live_chat -> take the ALL-messages "Live chat" token from the response's
// viewSelector -> loop honoring the returned continuation + timeoutMs. Any
// failure logs one [feeds] line, backs off, and re-bootstraps from /live —
// chat problems never affect anything else in Feeds (fail-soft, forever).
// Responses are parsed with obs_data_create_from_json (not the flat engine-IPC
// extractors, which can't traverse nested arrays).

// The poller is "enabled" purely by having a target configured — the dock's
// YouTube field drives the handle/video-id settings directly (no separate enable
// flag). Empty target = off.
static std::mutex        g_ytStateMutex;      // guards g_ytHandle + g_ytVideoId
static std::string       g_ytHandle;          // normalized (no leading @); "" = unset
static std::string       g_ytVideoId;         // pinned 11-char video id; non-empty => skip /live resolution
static std::thread       g_ytThread;
static std::atomic<bool> g_ytShouldExit{false};
static HANDLE            g_ytWakeEvent = nullptr;  // auto-reset; wakes on target-change/stream-start/exit

// Wait up to `ms`, returning early if the wake event fires (target change, stream
// start, or shutdown). Keeps the poller responsive without busy-waiting.
static void YtInterruptibleWait(DWORD ms) {
    if (g_ytWakeEvent) WaitForSingleObject(g_ytWakeEvent, ms);
    else               Sleep(ms == INFINITE ? 1000 : ms);
}
static void YtWakePoller() { if (g_ytWakeEvent) SetEvent(g_ytWakeEvent); }

// (kChatConn* enum is defined up near the chat dock, which consumes it.)

static bool YtHasTarget() {
    std::lock_guard<std::mutex> l(g_ytStateMutex);
    return !g_ytHandle.empty() || !g_ytVideoId.empty();
}

// A YouTube video id is exactly 11 chars from [A-Za-z0-9_-].
static bool IsYouTubeVideoId(const std::string& s) {
    if (s.size() != 11) return false;
    for (char c : s) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// YtTarget (handle vs pinned video id) is declared above the chat dock, which
// consumes it; the definitions here are the implementation.
//
// Detect + normalize whatever the user typed/pasted:
//   "@Name" / "Name" / ".../@Name/live"      -> handle mode
//   ".../watch?v=<id>" / "youtu.be/<id>"     -> video mode
//   bare 11-char id (no '@')                 -> video mode
// The '@' is the handle signal, so a channel name is never mistaken for a video
// id; a bare 11-char name typed without '@' is the only ambiguous case (rare,
// and a failed bootstrap just retries — the prompt tells users to '@'-prefix
// handles). Empty input clears the target.
static YtTarget NormalizeYouTubeTarget(std::string s) {
    auto trim = [](std::string& x) {
        size_t a = x.find_first_not_of(" \t\r\n");
        size_t b = x.find_last_not_of(" \t\r\n");
        x = (a == std::string::npos) ? "" : x.substr(a, b - a + 1);
    };
    trim(s);
    YtTarget t;
    if (s.empty()) return t;

    // No '@' -> look for a video id (watch URL, youtu.be, or bare id).
    if (s.find('@') == std::string::npos) {
        std::string id;
        size_t vp = s.find("v=");
        if (vp != std::string::npos && s.size() >= vp + 2 + 11)
            id = s.substr(vp + 2, 11);
        if (id.empty()) {
            size_t yb = s.find("youtu.be/");
            if (yb != std::string::npos && s.size() >= yb + 9 + 11)
                id = s.substr(yb + 9, 11);
        }
        if (id.empty() && IsYouTubeVideoId(s)) id = s;
        if (IsYouTubeVideoId(id)) { t.isVideo = true; t.value = id; return t; }
    }

    // Handle mode: strip a leading '@' / URL prefix and any trailing path.
    size_t at = s.rfind('@');
    if (at != std::string::npos) s = s.substr(at + 1);
    size_t sl = s.find('/');
    if (sl != std::string::npos) s = s.substr(0, sl);
    trim(s);
    t.isVideo = false;
    t.value = s;
    return t;
}

// Apply a dock-field input as the poller target: normalize, store exclusively
// (handle XOR video id), persist, and wake the poller if it actually changed.
// Returns the normalized display form (@handle / bare id / "") for the field.
// The chat dock routes its field commits through here so it never touches the
// poller's globals/config directly.
static std::string YtSetTargetFromInput(const std::string& input) {
    YtTarget t = NormalizeYouTubeTarget(input);
    const std::string newH = t.isVideo ? std::string() : t.value;
    const std::string newV = t.isVideo ? t.value : std::string();
    const std::string disp = t.value.empty()
        ? std::string()
        : (t.isVideo ? t.value : "@" + t.value);

    bool changed;
    {
        std::lock_guard<std::mutex> l(g_ytStateMutex);
        changed = (newH != g_ytHandle || newV != g_ytVideoId);
        g_ytHandle = newH;
        g_ytVideoId = newV;
    }
    if (changed) {
        SaveYouTubeHandleSetting(newH);
        SaveYouTubeVideoIdSetting(newV);
        YtWakePoller();
    }
    return disp;
}

// Is OBS currently streaming to YouTube? Detection only — used as an accelerator,
// never a gate. The service pointer is borrowed (frontend-owned; do NOT release).
static bool IsStreamingToYouTube() {
    if (!obs_frontend_streaming_active()) return false;
    obs_service_t* svc = obs_frontend_get_streaming_service();
    if (!svc) return false;
    const char* name = obs_service_get_name(svc);
    if (name && strstr(name, "YouTube")) return true;
    const char* url = obs_service_get_connect_info(
        svc, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
    return url && strstr(url, "youtube.com/live2");
}

// --- WinHTTP one-shot to www.youtube.com (GET or POST) --------------------
// Follows redirects (default). Sends a preemptive consent cookie so an EU
// consent interstitial doesn't block the scrape. Returns true if a response was
// received (any status); fills status + body. Timeouts bound worst-case unload.
static bool YtHttpRequest(const std::wstring& host, const wchar_t* method,
                          const std::wstring& path, const std::string* body,
                          DWORD& status, std::string& respBody) {
    status = 0;
    respBody.clear();
    // Resolved per call rather than cached per host: this is a leaf used by the
    // chat poller and by avatar downloads, and a process-lifetime cache would
    // pin a stale proxy when the user moves between networks (office -> home),
    // breaking chat until OBS restarts. WinHTTP caches the PAC script and its
    // results internally, so the repeat cost is small; a stale proxy is not.
    feeds_http::ProxyResolution px =
        feeds_http::ResolveProxyForUrl(L"https://" + host + path);
    LogProxyResolutionOnce(px, "YouTube");
    HINTERNET hSession = feeds_http::OpenProxiedSession(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) FeedsOBS", px);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 8000, 8000, 10000, 15000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
                                        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    WinHttpAddRequestHeaders(hRequest,
        L"Cookie: SOCS=CAI; CONSENT=YES+cb\r\n"
        L"Accept-Language: en-US,en;q=0.9\r\n",
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    if (body)
        WinHttpAddRequestHeaders(hRequest, L"Content-Type: application/json\r\n",
                                 (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    BOOL sentOk = WinHttpSendRequest(
        hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        body ? (LPVOID)body->data() : WINHTTP_NO_REQUEST_DATA,
        body ? (DWORD)body->size() : 0,
        body ? (DWORD)body->size() : 0, 0);

    bool ok = false;
    if (sentOk && WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD st = 0, stSize = sizeof(st);
        WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &st, &stSize, WINHTTP_NO_HEADER_INDEX);
        status = st;
        char buf[16384];
        DWORD n = 0;
        while (WinHttpReadData(hRequest, buf, sizeof(buf), &n) && n > 0)
            respBody.append(buf, n);
        ok = true;
    }
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

// --- YT avatar cache + download worker (Phase 2) --------------------------
// Parallel to the Zoom avatar cache (g_avatarCache): that one is keyed by uint
// senderId and fed by engine-written files; YT authors have string channel ids
// and ggpht URLs, so they need their own cache. Downloads run on a dedicated
// worker (never the UI thread); one attempt per channel id per session (chat is
// bursty from few authors); a cached null QImage records a failed/absent avatar
// so we neither re-fetch nor block — the popup falls back to a neutral circle.
// The dock click handler resolves the QImage from here for the popup; the chat
// overlay reads it directly at render time (extern), the way it reads the Zoom
// g_avatarCache — so these two have external linkage (not static).
std::mutex                                   g_ytAvatarCacheMutex;
std::map<std::string, QImage>                g_ytAvatarCacheByChannel;  // channelId -> avatar (null = tried, none)
static std::set<std::string>                 g_ytAvatarInFlight;
static constexpr size_t                      kYtAvatarCacheCap = 512;

static std::mutex                            g_ytAvatarQueueMutex;
static std::vector<std::pair<std::string, std::string>> g_ytAvatarQueue;  // (channelId, url)
static std::thread                           g_ytAvatarThread;
static std::atomic<bool>                     g_ytAvatarExit{false};
static HANDLE                                g_ytAvatarEvent = nullptr;  // auto-reset wake

// Resolve a channel's avatar from the cache (UI thread, at click time). Returns
// a null QImage if not downloaded yet or the download failed — the popup renders
// its neutral placeholder circle for a null avatar.
static QImage YtResolveAvatar(const std::string& channelId) {
    if (channelId.empty()) return QImage();
    std::lock_guard<std::mutex> l(g_ytAvatarCacheMutex);
    auto it = g_ytAvatarCacheByChannel.find(channelId);
    return it != g_ytAvatarCacheByChannel.end() ? it->second : QImage();
}

// Split "https://host/path" into wide host + path. Avatar hosts/paths are ASCII.
static bool YtSplitHttpsUrl(const std::string& url, std::wstring& host,
                            std::wstring& path) {
    const std::string pfx = "https://";
    if (url.compare(0, pfx.size(), pfx) != 0) return false;
    size_t hostStart = pfx.size();
    size_t slash = url.find('/', hostStart);
    std::string h = (slash == std::string::npos)
                        ? url.substr(hostStart) : url.substr(hostStart, slash - hostStart);
    std::string p = (slash == std::string::npos) ? "/" : url.substr(slash);
    if (h.empty()) return false;
    host.assign(h.begin(), h.end());
    path.assign(p.begin(), p.end());
    return true;
}

// Download + decode one avatar. Null QImage on any failure (bad URL, non-200,
// or a format Qt can't decode — ggpht usually serves JPEG/PNG; WebP would need
// Qt's webp image plugin, and a decode miss just fails soft to the circle).
static QImage YtDownloadAvatarImage(const std::string& url) {
    std::wstring host, path;
    if (!YtSplitHttpsUrl(url, host, path)) return QImage();
    DWORD status = 0;
    std::string body;
    if (!YtHttpRequest(host, L"GET", path, nullptr, status, body)) return QImage();
    if (status != 200 || body.empty()) return QImage();
    QImage img;
    img.loadFromData(reinterpret_cast<const uchar*>(body.data()), (int)body.size());
    return img;  // null if decode failed
}

// Queue an avatar download for `channelId` from `url` if we don't already have
// it (or one in flight), and the cache isn't at cap. Called from the poll loop
// (poller thread) as messages arrive — warms the cache before the user clicks.
static void YtRequestAvatar(const std::string& channelId, const std::string& url) {
    if (channelId.empty() || url.empty()) return;
    {
        std::lock_guard<std::mutex> l(g_ytAvatarCacheMutex);
        if (g_ytAvatarCacheByChannel.count(channelId)) return;   // already have (or tried)
        if (g_ytAvatarInFlight.count(channelId))        return;   // already downloading
        if (g_ytAvatarCacheByChannel.size() >= kYtAvatarCacheCap) return;  // bounded
        g_ytAvatarInFlight.insert(channelId);
    }
    {
        std::lock_guard<std::mutex> l(g_ytAvatarQueueMutex);
        g_ytAvatarQueue.emplace_back(channelId, url);
    }
    if (g_ytAvatarEvent) SetEvent(g_ytAvatarEvent);
}

static void YtAvatarWorkerLoop() {
    while (!g_ytAvatarExit.load()) {
        std::pair<std::string, std::string> job;
        bool have = false;
        {
            std::lock_guard<std::mutex> l(g_ytAvatarQueueMutex);
            if (!g_ytAvatarQueue.empty()) {
                job = g_ytAvatarQueue.front();
                g_ytAvatarQueue.erase(g_ytAvatarQueue.begin());
                have = true;
            }
        }
        if (!have) {
            if (g_ytAvatarEvent) WaitForSingleObject(g_ytAvatarEvent, INFINITE);
            continue;
        }
        QImage img = YtDownloadAvatarImage(job.second);   // null on failure
        {
            std::lock_guard<std::mutex> l(g_ytAvatarCacheMutex);
            g_ytAvatarCacheByChannel[job.first] = img;     // cache result (even null: one try/session)
            g_ytAvatarInFlight.erase(job.first);
        }
        // A resolved avatar can land after its row already rendered with the
        // neutral circle — repaint the overlay now (which resolves avatars at
        // render time) instead of waiting for incidental chat to force it. Skip a
        // cached miss (null): nothing would change. Mark is thread-safe (instances
        // mutex + bool writes only), so it's safe from this worker thread.
        if (!img.isNull()) feeds::MarkChatOverlayDirty();
    }
}

// --- HTML scrape helpers (flat literals in the watch page; NOT the JSON API) --
static std::string YtScrapeString(const std::string& s, const std::string& key,
                                  size_t from = 0) {
    std::string pat = "\"" + key + "\":\"";
    size_t p = s.find(pat, from);
    if (p == std::string::npos) return "";
    p += pat.size();
    size_t e = s.find('"', p);
    return e == std::string::npos ? "" : s.substr(p, e - p);
}

// Scrape the owner channel's handle (bare, no '@') from a watch page for the
// dock's canonical-casing correction. The page has many "canonicalBaseUrl"
// literals (owner + related channels, and the owner's own /channel/UC.. form),
// so we anchor on the "videoSecondaryInfoRenderer" marker (the owner block) and
// take the first "/@<handle>" after it — verified to resolve to the owner. ""
// if the owner has no @-handle (only a /channel/ URL) or the marker is absent.
static std::string YtScrapeCanonicalHandle(const std::string& html) {
    size_t anchor = html.find("videoSecondaryInfoRenderer");
    const std::string pat = "\"canonicalBaseUrl\":\"/@";
    size_t p = html.find(pat, anchor == std::string::npos ? 0 : anchor);
    if (p == std::string::npos) return "";
    p += pat.size();
    size_t e = html.find('"', p);
    return e == std::string::npos ? "" : html.substr(p, e - p);
}

// Extract a balanced {...} object following `marker`, string/escape aware so
// braces inside strings don't unbalance the depth count.
static std::string YtExtractJsonObject(const std::string& s,
                                       const std::string& marker) {
    size_t m = s.find(marker);
    if (m == std::string::npos) return "";
    size_t start = s.find('{', m);
    if (start == std::string::npos) return "";
    int depth = 0;
    bool inStr = false, esc = false;
    for (size_t i = start; i < s.size(); ++i) {
        char c = s[i];
        if (inStr) {
            if (esc)            esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"')  inStr = false;
        } else if (c == '"')    inStr = true;
        else if (c == '{')      depth++;
        else if (c == '}') { if (--depth == 0) return s.substr(start, i - start + 1); }
    }
    return "";
}

// --- Minimal leak-free obs_data traversal wrappers ------------------------
struct YtObj {
    obs_data_t* d = nullptr;
    YtObj() = default;
    explicit YtObj(obs_data_t* p) : d(p) {}
    ~YtObj() { if (d) obs_data_release(d); }
    YtObj(const YtObj&) = delete;
    YtObj& operator=(const YtObj&) = delete;
    YtObj(YtObj&& o) noexcept : d(o.d) { o.d = nullptr; }
    YtObj& operator=(YtObj&& o) noexcept {
        if (this != &o) { if (d) obs_data_release(d); d = o.d; o.d = nullptr; }
        return *this;
    }
    explicit operator bool() const { return d != nullptr; }
    bool has(const char* k) const { return d && obs_data_has_user_value(d, k); }
    YtObj obj(const char* k) const {
        if (!has(k)) return YtObj();
        return YtObj(obs_data_get_obj(d, k));   // addref'd (or null); released by dtor
    }
    std::string str(const char* k) const {
        if (!d) return "";
        const char* s = obs_data_get_string(d, k);
        return s ? s : "";
    }
    long long num(const char* k) const { return d ? obs_data_get_int(d, k) : 0; }
};
struct YtArr {
    obs_data_array_t* a = nullptr;
    YtArr() = default;
    explicit YtArr(obs_data_array_t* p) : a(p) {}
    ~YtArr() { if (a) obs_data_array_release(a); }
    YtArr(const YtArr&) = delete;
    YtArr& operator=(const YtArr&) = delete;
    YtArr(YtArr&& o) noexcept : a(o.a) { o.a = nullptr; }
    size_t count() const { return a ? obs_data_array_count(a) : 0; }
    YtObj  at(size_t i) const { return a ? YtObj(obs_data_array_item(a, i)) : YtObj(); }
};
static YtArr YtArrayOf(const YtObj& o, const char* k) {
    if (!o.d || !obs_data_has_user_value(o.d, k)) return YtArr();
    return YtArr(obs_data_get_array(o.d, k));
}

struct YtMsg {
    std::string id, author, channelId, avatarUrl, text;
};

// Pull the display fields out of one liveChatTextMessageRenderer. Returns false
// if there's nothing worth showing. (Badges are parsed structurally elsewhere;
// Phase 1 renders name + text + glyph only.)
static bool YtParseTextMessage(const YtObj& r, YtMsg& out) {
    out = YtMsg();
    out.id        = r.str("id");
    out.channelId = r.str("authorExternalChannelId");
    { YtObj an = r.obj("authorName"); out.author = an.str("simpleText"); }
    {   // avatar URL = largest (last) thumbnail — stored for Phase 2 download
        YtObj ap = r.obj("authorPhoto");
        YtArr th = YtArrayOf(ap, "thumbnails");
        if (th.count()) { YtObj t = th.at(th.count() - 1); out.avatarUrl = t.str("url"); }
    }
    {   // message runs: concat text; render each emoji run as :label: with a
        // space inserted where it's adjacent to another run (Slack/Discord
        // text-emote convention) so consecutive emotes don't smush into
        // "waving-handpink-heart". Real emoji images are out of scope. Assembled
        // once here, so both the dock and the popup get the same rendered text.
        YtObj msg = r.obj("message");
        YtArr runs = YtArrayOf(msg, "runs");
        std::string text;
        bool lastWasEmoji = false;
        auto append = [&](const std::string& tok, bool isEmoji) {
            if (tok.empty()) return;
            if (!text.empty() && (isEmoji || lastWasEmoji) &&
                text.back() != ' ' && tok.front() != ' ')
                text += ' ';
            text += tok;
            lastWasEmoji = isEmoji;
        };
        for (size_t i = 0; i < runs.count(); ++i) {
            YtObj run = runs.at(i);
            if (run.has("text")) {
                append(run.str("text"), false);
            } else if (run.has("emoji")) {
                std::string label = run.obj("emoji").obj("image")
                    .obj("accessibility").obj("accessibilityData").str("label");
                if (!label.empty()) append(":" + label + ":", true);
            }
        }
        out.text = text;
    }
    return !(out.author.empty() && out.text.empty());
}

static std::string YtBuildBody(const std::string& clientVer,
                               const std::string& visitor,
                               const std::string& cont) {
    std::string b = "{\"context\":{\"client\":{\"clientName\":\"WEB\","
                    "\"clientVersion\":\"" + JsonEscape(clientVer) + "\"";
    if (!visitor.empty())
        b += ",\"visitorData\":\"" + JsonEscape(visitor) + "\"";
    b += "}},\"continuation\":\"" + JsonEscape(cont) + "\"}";
    return b;
}

// Outputs the HTTP status (0 = transport failure) so the caller can log it on a
// failed poll. Returns true only on a 200 with a non-empty body.
static bool YtPostLiveChat(const std::string& apiKey, const std::string& body,
                           std::string& respBody, DWORD& httpStatus) {
    std::wstring path =
        L"/youtubei/v1/live_chat/get_live_chat?prettyPrint=false&key=" +
        std::wstring(apiKey.begin(), apiKey.end());   // apiKey is ASCII
    httpStatus = 0;
    if (!YtHttpRequest(L"www.youtube.com", L"POST", path, &body, httpStatus, respBody))
        return false;
    return httpStatus == 200 && !respBody.empty();
}

struct YtChatPage {
    std::vector<YtMsg> messages;
    std::vector<std::string> deletedMsgIds;      // markChatItemAsDeletedAction
    std::vector<std::string> deletedChannelIds;  // markChatItemsByAuthorAsDeletedAction
    std::string nextContinuation;
    DWORD       timeoutMs = 0;
    std::string allMessagesToken;   // from viewSelector; only in the first response
    bool        ok = false;
};

static YtChatPage YtParseLiveChat(const std::string& json) {
    YtChatPage pg;
    obs_data_t* root = obs_data_create_from_json(json.c_str());
    if (!root) return pg;
    YtObj rootO(root);

    YtObj lc = rootO.obj("continuationContents").obj("liveChatContinuation");
    if (!lc) return pg;   // error response / not a chat payload -> caller retries

    YtArr actions = YtArrayOf(lc, "actions");
    for (size_t i = 0; i < actions.count(); ++i) {
        YtObj a = actions.at(i);
        // Moderation (Phase 2, YT-scope): a single message removed by id, or all
        // of an author's messages removed by channel id. Field paths per the
        // investigation notes — parsed defensively (real mod samples weren't
        // captured on the quiet stream; str() on a non-deletion action is "").
        { std::string tid = a.obj("markChatItemAsDeletedAction").str("targetItemId");
          if (!tid.empty()) { pg.deletedMsgIds.push_back(tid); continue; } }
        { std::string cid = a.obj("markChatItemsByAuthorAsDeletedAction")
                             .str("externalChannelId");
          if (!cid.empty()) { pg.deletedChannelIds.push_back(cid); continue; } }

        // Adds: only addChatItemAction + liveChatTextMessageRenderer. Every other
        // action/renderer (system, banner, ticker, paid, membership) is skipped
        // defensively — never crash/stall on the unknown.
        YtObj r = a.obj("addChatItemAction").obj("item").obj("liveChatTextMessageRenderer");
        if (!r) continue;
        YtMsg m;
        if (YtParseTextMessage(r, m)) pg.messages.push_back(std::move(m));
    }

    YtArr conts = YtArrayOf(lc, "continuations");
    if (conts.count()) {
        YtObj c0 = conts.at(0);
        for (const char* t : {"invalidationContinuationData",
                              "timedContinuationData",
                              "reloadContinuationData"}) {
            YtObj cd = c0.obj(t);
            if (cd) { pg.nextContinuation = cd.str("continuation");
                      pg.timeoutMs = (DWORD)cd.num("timeoutMs"); break; }
        }
    }

    // ALL-messages ("Live chat") token from the header viewSelector — present in
    // the first (bootstrap) response; used to switch off the default Top-chat feed.
    YtArr items = YtArrayOf(
        lc.obj("header").obj("liveChatHeaderRenderer").obj("viewSelector")
          .obj("sortFilterSubMenuRenderer"), "subMenuItems");
    for (size_t i = 0; i < items.count(); ++i) {
        YtObj it = items.at(i);
        std::string title = it.str("title");
        if (title.find("Live chat") != std::string::npos ||
            title.find("Live Chat") != std::string::npos) {
            pg.allMessagesToken =
                it.obj("continuation").obj("reloadContinuationData").str("continuation");
            break;
        }
    }
    pg.ok = true;
    return pg;
}

struct YtBootstrap {
    std::string apiKey, clientVer, visitor, initialContinuation;
    std::string videoId;          // resolved stream video id (for the connected log)
    std::string canonicalHandle;  // owner handle (bare), handle mode only, for casing correction
    bool ok = false;
};

// GET the watch page at `pathU` (either "/@handle/live" or "/watch?v=<id>") and
// scrape config + initial continuation. `knownVideoId` is set for a pinned
// stream (used verbatim); for handle mode it's "" and we scrape the resolved id
// from the page. Not-live (or a transient failure) -> ok=false, caller retries.
static YtBootstrap YtFetchBootstrap(const std::string& pathU,
                                    const std::string& knownVideoId) {
    YtBootstrap bs;
    std::wstring path(pathU.begin(), pathU.end());   // handle/id/path chars are ASCII
    DWORD status = 0;
    std::string html;
    if (!YtHttpRequest(L"www.youtube.com", L"GET", path, nullptr, status, html))
        return bs;
    if (status != 200 || html.find("ytInitialData") == std::string::npos) return bs;

    bs.apiKey    = YtScrapeString(html, "INNERTUBE_API_KEY");
    bs.clientVer = YtScrapeString(html, "INNERTUBE_CONTEXT_CLIENT_VERSION");
    bs.visitor   = YtScrapeString(html, "VISITOR_DATA");
    if (bs.apiKey.empty() || bs.clientVer.empty()) return bs;

    // Resolved video id for diagnostics: the pinned id if given, else scrape the
    // page's own video (videoDetails.videoId, biased past the videoDetails marker
    // so related-video ids don't win).
    if (!knownVideoId.empty()) {
        bs.videoId = knownVideoId;
    } else {
        size_t vd = html.find("\"videoDetails\"");
        bs.videoId = YtScrapeString(html, "videoId",
                                    vd == std::string::npos ? 0 : vd);
        // Canonical owner handle for the dock's casing correction (handle mode
        // only — a pinned video needs no handle).
        bs.canonicalHandle = YtScrapeCanonicalHandle(html);
    }

    std::string blob = YtExtractJsonObject(html, "ytInitialData");
    if (blob.empty()) return bs;
    obs_data_t* root = obs_data_create_from_json(blob.c_str());
    if (!root) return bs;
    YtObj rootO(root);
    YtObj lcr = rootO.obj("contents").obj("twoColumnWatchNextResults")
                     .obj("conversationBar").obj("liveChatRenderer");
    YtArr conts = YtArrayOf(lcr, "continuations");
    if (conts.count())
        bs.initialContinuation =
            conts.at(0).obj("reloadContinuationData").str("continuation");
    // Empty continuation => not currently live (no chat renderer).
    bs.ok = !bs.initialContinuation.empty();
    return bs;
}

// Marshal one message onto the Qt main thread into the dock. Tier gate mirrors
// the Zoom chat_message handler exactly (dock chat is Basic+); the dock also
// re-checks m_tierDisabled. YouTube never reaches the overlay/popup (Phase 1).
static void YtDeliverToDock(const YtMsg& m) {
    if (g_currentTier < 1) return;
    QString author = QString::fromStdString(m.author);
    QString content = QString::fromStdString(m.text);
    QString id = QString::fromStdString(m.id);
    QString ch = QString::fromStdString(m.channelId);
    QString av = QString::fromStdString(m.avatarUrl);
    QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
        [author, content, id, ch, av]() {
            if (g_chatDock)
                g_chatDock->AppendYouTubeMessage(author, content, id, ch, av);
        });
}

// Push a YouTube message into the chat overlay's centralised history (Streamer+,
// matching the Zoom path's overlay gate). Tagged YouTube so each overlay
// instance's platform filter includes/excludes it; the avatar is resolved at
// render time from the YT cache by channel id. Thread-safe append — no marshaling
// (the overlay reads its history under its own mutex on the graphics thread).
static void YtDeliverToOverlay(const YtMsg& m) {
    if (g_currentTier < 2) return;
    feeds::AppendChatMessageToOverlay(
        feeds::ChatMsgOrigin::YouTube, 0 /*no Zoom id*/, m.channelId,
        m.author, m.text, 0 /*timestamp unused for ordering*/);
}

// Deletion fan-out to the dock (Qt main thread). The dock removes matching rows
// and clears the popup if it's showing one of them. YT-scope only.
static void YtDeliverDeletionById(const std::string& id) {
    QString qid = QString::fromStdString(id);
    QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
        [qid]() { if (g_chatDock) g_chatDock->RemoveYouTubeMessageById(qid); });
}
static void YtDeliverDeletionByChannel(const std::string& channelId) {
    QString qc = QString::fromStdString(channelId);
    QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
        [qc]() { if (g_chatDock) g_chatDock->RemoveYouTubeMessagesByChannel(qc); });
}

// Marshal the YouTube connection status to the dock header. Deduped against the
// last report (poller-thread-only state) so a repeated-failure loop doesn't spam
// the UI queue. videoId is the resolved id for the Live tooltip.
static int         g_ytStatusLast = -1;
static std::string g_ytStatusLastVid;
static void YtReportStatus(int status, const std::string& videoId) {
    if (status == g_ytStatusLast && videoId == g_ytStatusLastVid) return;
    g_ytStatusLast = status;
    g_ytStatusLastVid = videoId;
    QString vid = QString::fromStdString(videoId);
    QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
        [status, vid]() { if (g_chatDock) g_chatDock->UpdateYouTubeStatus(status, vid); });
}

// Marshal the canonical owner handle to the dock so the field's casing is
// corrected on connect (display only — see FeedsChatDock::UpdateYouTubeHandleText).
static void YtReportCanonicalHandle(const std::string& handle) {
    if (handle.empty()) return;
    QString h = QString::fromStdString(handle);
    QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
        [h]() { if (g_chatDock) g_chatDock->UpdateYouTubeHandleText(h); });
}

static void YtChatPollLoop() {
    blog(LOG_INFO, "[feeds] youtube chat poller thread started");
    while (!g_ytShouldExit.load()) {
        // Idle gate: enabled == has a target. Empty -> Off, block on the wake.
        std::string handle, videoId;
        { std::lock_guard<std::mutex> l(g_ytStateMutex);
          handle = g_ytHandle; videoId = g_ytVideoId; }
        if (handle.empty() && videoId.empty()) {
            YtReportStatus(kChatConnOff, "");
            YtInterruptibleWait(INFINITE); continue;
        }
        // Target set but not yet connected -> Waiting (covers the tier-locked and
        // not-live cases below).
        YtReportStatus(kChatConnWaiting, "");
        // Inherit the chat dock's Basic+ gating exactly: don't even poll YouTube
        // for a Free user (the header is hidden while tier-locked anyway). Bounded
        // re-check so a login/upgrade is picked up without wiring a wake.
        if (g_currentTier < 1) { YtInterruptibleWait(30000); continue; }

        // Bootstrap the watch page. Video mode pins /watch?v=<id> (skips /live
        // resolution — the fix for channels running several concurrent streams);
        // handle mode resolves /@handle/live. Retries on the ~60s slow cadence if
        // not live yet.
        std::string pathU = !videoId.empty()
                              ? ("/watch?v=" + videoId)
                              : ("/@" + handle + "/live");
        std::string targetDesc = !videoId.empty()
                              ? ("video " + videoId) : ("@" + handle);
        YtBootstrap bs = YtFetchBootstrap(pathU, videoId);
        if (g_ytShouldExit.load()) break;
        if (!bs.ok) { YtInterruptibleWait(60000); continue; }
        blog(LOG_INFO, "[feeds] youtube chat: connected to %s live (video=%s)",
             targetDesc.c_str(), bs.videoId.c_str());

        // Initial get_live_chat (Top-chat default) -> ALL-messages token from resp.
        std::string resp;
        DWORD initStatus = 0;
        if (!YtPostLiveChat(bs.apiKey,
                YtBuildBody(bs.clientVer, bs.visitor, bs.initialContinuation),
                resp, initStatus)) {
            blog(LOG_INFO, "[feeds] youtube chat: initial poll failed "
                 "(status=%lu, %lu bytes); re-bootstrapping",
                 (unsigned long)initStatus, (unsigned long)resp.size());
            YtInterruptibleWait(15000); continue;
        }
        YtChatPage first = YtParseLiveChat(resp);
        std::string token = !first.allMessagesToken.empty()
                              ? first.allMessagesToken : first.nextContinuation;
        if (token.empty()) {
            blog(LOG_INFO, "[feeds] youtube chat: no continuation in initial "
                 "response (%lu bytes); re-bootstrapping", (unsigned long)resp.size());
            YtInterruptibleWait(15000); continue;
        }

        // Connected. Report Live (with resolved video id) and, in handle mode,
        // the canonical owner handle so the dock corrects the field's casing.
        YtReportStatus(kChatConnLive, bs.videoId);
        if (videoId.empty() && !bs.canonicalHandle.empty())
            YtReportCanonicalHandle(bs.canonicalHandle);

        // Steady-state loop on the ALL-messages feed. The first page after the
        // switch is a backlog (70+); skip it (prime the continuation) so the dock
        // fills with messages from connect-time onward rather than a history dump.
        // EVERY exit path below logs a distinct [feeds] line — the loop must never
        // die mute (that's the bug this rework targets).
        bool primed = false;
        int  cycle  = 0;
        for (;;) {
            if (g_ytShouldExit.load()) {
                blog(LOG_INFO, "[feeds] youtube chat: shutdown; stopping loop");
                break;
            }
            if (g_currentTier < 1) {
                blog(LOG_INFO, "[feeds] youtube chat: tier dropped below Basic; stopping loop");
                break;
            }
            { std::lock_guard<std::mutex> l(g_ytStateMutex);
              if (g_ytHandle != handle || g_ytVideoId != videoId) {
                  blog(LOG_INFO, "[feeds] youtube chat: target changed; re-bootstrapping");
                  break;
              } }

            std::string r;
            DWORD st = 0;
            if (!YtPostLiveChat(bs.apiKey, YtBuildBody(bs.clientVer, bs.visitor, token), r, st)) {
                blog(LOG_INFO, "[feeds] youtube chat: poll HTTP failed "
                     "(status=%lu, %lu bytes); re-bootstrapping",
                     (unsigned long)st, (unsigned long)r.size());
                break;
            }
            YtChatPage pg = YtParseLiveChat(r);
            if (!pg.ok) {
                blog(LOG_INFO, "[feeds] youtube chat: parse failure "
                     "(%lu bytes); re-bootstrapping", (unsigned long)r.size());
                break;
            }
            if (pg.nextContinuation.empty()) {
                blog(LOG_INFO, "[feeds] youtube chat: empty next continuation "
                     "(%lu bytes, %zu actions); re-bootstrapping",
                     (unsigned long)r.size(),
                     pg.messages.size() + pg.deletedMsgIds.size()
                         + pg.deletedChannelIds.size());
                break;
            }
            if (primed) {
                for (const auto& m : pg.messages) {
                    // Warm the avatar as the message is shown, so it's cached
                    // before the user can click it into the popup. Scoped to
                    // shown messages — the skipped backlog isn't downloaded.
                    YtRequestAvatar(m.channelId, m.avatarUrl);
                    YtDeliverToDock(m);
                    YtDeliverToOverlay(m);   // overlay honors per-instance filter
                }
                for (const auto& id : pg.deletedMsgIds)      YtDeliverDeletionById(id);
                for (const auto& cid : pg.deletedChannelIds)  YtDeliverDeletionByChannel(cid);
            }
            primed = true;

            // Coarse proof-of-life + big-chat visibility: every ~20 cycles
            // (~60s at the 3s cap) log the cycle count and last response size /
            // message count, so a busy channel's cadence and payload sizes are
            // visible without per-cycle spam.
            if (++cycle % 20 == 0) {
                blog(LOG_INFO, "[feeds] youtube chat: polling ok "
                     "(cycle %d, last %lu bytes, %zu msgs)",
                     cycle, (unsigned long)r.size(), pg.messages.size());
            }

            token = pg.nextContinuation;
            // Cap the wait at ~3s regardless of the server's (usually ~10s)
            // timeoutMs — invalidation continuations are safe to poll early and
            // return whatever's new (often empty), turning 10s batches into a
            // near-live feel. 1s floor.
            DWORD wait = pg.timeoutMs ? pg.timeoutMs : 3000;
            if (wait < 1000) wait = 1000;
            if (wait > 3000) wait = 3000;
            YtInterruptibleWait(wait);
        }
        // Fell out (a distinct line was logged above). Brief backoff, then the
        // outer loop re-bootstraps (self-healing, forever).
        if (!g_ytShouldExit.load()) YtInterruptibleWait(5000);
    }
    blog(LOG_INFO, "[feeds] youtube chat poller thread exiting");
}

static void YtStartPollerThread() {
    if (g_ytThread.joinable()) return;
    g_ytShouldExit = false;
    if (!g_ytWakeEvent)
        g_ytWakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
    // Avatar download worker (Phase 2) — shares the poller's lifetime.
    g_ytAvatarExit = false;
    if (!g_ytAvatarEvent)
        g_ytAvatarEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_ytAvatarThread = std::thread(YtAvatarWorkerLoop);
    g_ytThread = std::thread(YtChatPollLoop);
}

static void YtStopPollerThread() {
    g_ytShouldExit = true;
    if (g_ytWakeEvent) SetEvent(g_ytWakeEvent);
    if (g_ytThread.joinable()) g_ytThread.join();
    if (g_ytWakeEvent) { CloseHandle(g_ytWakeEvent); g_ytWakeEvent = nullptr; }

    g_ytAvatarExit = true;
    if (g_ytAvatarEvent) SetEvent(g_ytAvatarEvent);
    if (g_ytAvatarThread.joinable()) g_ytAvatarThread.join();
    if (g_ytAvatarEvent) { CloseHandle(g_ytAvatarEvent); g_ytAvatarEvent = nullptr; }
}

// ===========================================================================
// Twitch live-chat reader (receive only)
// ===========================================================================
// Plugin-side, engine-uninvolved — mirrors the YouTube poller's shape and
// lifecycle, but the transport is a persistent connection, not a poll loop:
// Twitch chat is the sanctioned anonymous IRC interface, read over a secure
// WebSocket (wss://irc-ws.chat.twitch.tv:443) via WinHTTP — the same session
// idiom as YtHttpRequest, no new TLS stack. The anonymous handshake needs no
// OAuth/token (verified against current Twitch docs + tmi.js):
//   PASS SCHMOOPIIE            (any dummy value; no token)
//   NICK justinfan<random>     (the anonymous-justinfan convention)
//   CAP REQ :twitch.tv/tags twitch.tv/commands
//       tags   -> PRIVMSG metadata (display-name, id, user-id, color, ...)
//       commands -> the CLEARMSG/CLEARCHAT moderation messages we honor.
//       membership is deliberately NOT requested (only JOIN/PART member lists).
//   JOIN #<login>
// Server PING :tmi.twitch.tv is answered with PONG :tmi.twitch.tv to stay
// connected. Whispers can't reach an anonymous connection at all (needs an
// authed account + whisper cap), so there is no whisper path to guard. Any
// socket drop/error logs one distinct [feeds] line, backs off, and reconnects
// forever — fail-soft, exactly like YouTube; Twitch breaking never touches the
// stream or other surfaces. Twitch IRC carries no avatar (that needs the Helix
// API + a client-id/token — a known backburner), so Twitch messages get the
// popup's neutral circle and no overlay avatar.

// Enabled purely by having a channel configured (empty = off), same model as
// YouTube. The WebSocket handle is guarded so a shutdown or target change can
// abort a blocked WinHttpWebSocketReceive by closing it (the sanctioned cancel
// for a synchronous WinHTTP call); the receive timeout is left at its 0/infinite
// default so an idle channel never drops on a timeout.
static std::mutex        g_twStateMutex;    // guards g_twChannel
static std::string       g_twChannel;       // normalized login; "" = unset/off
static std::thread       g_twThread;
static std::atomic<bool> g_twShouldExit{false};
static HANDLE            g_twWakeEvent = nullptr;  // auto-reset; wakes on target-change/shutdown
static std::mutex        g_twSockMutex;     // guards g_twWebSocket
static HINTERNET         g_twWebSocket = nullptr;  // live WS handle (null when not connected)

static void TwInterruptibleWait(DWORD ms) {
    if (g_twWakeEvent) WaitForSingleObject(g_twWakeEvent, ms);
    else               Sleep(ms == INFINITE ? 1000 : ms);
}

// Close the live WebSocket if any (atomically taking ownership so a concurrent
// caller no-ops). Called from the read loop's normal cleanup AND cross-thread
// from the wake/stop path to abort a blocked receive — a null-check under the
// lock guarantees the handle is closed exactly once.
static void TwCloseSocket() {
    HINTERNET h = nullptr;
    { std::lock_guard<std::mutex> l(g_twSockMutex); h = g_twWebSocket; g_twWebSocket = nullptr; }
    if (h) WinHttpCloseHandle(h);
}

// Wake the poller: signal the wait AND abort any blocked receive so a target
// change / shutdown is picked up immediately (not on the next server message).
static void TwWakePoller() {
    if (g_twWakeEvent) SetEvent(g_twWakeEvent);
    TwCloseSocket();
}

// Normalize whatever the user typed/pasted to a bare lowercase login: strip a
// scheme, a twitch.tv/ host, a leading '@' or '#', any trailing path/query, and
// keep only login-legal chars ([a-z0-9_]). Twitch logins are already canonical
// lowercase, so there's no casing-correction round trip (unlike YouTube).
static std::string NormalizeTwitchChannel(std::string s) {
    auto trim = [](std::string& x) {
        size_t a = x.find_first_not_of(" \t\r\n");
        size_t b = x.find_last_not_of(" \t\r\n");
        x = (a == std::string::npos) ? "" : x.substr(a, b - a + 1);
    };
    trim(s);
    if (s.empty()) return "";
    for (char& c : s) c = (char)tolower((unsigned char)c);
    auto stripPrefix = [&](const char* pfx) {
        size_t n = strlen(pfx);
        if (s.compare(0, n, pfx) == 0) s = s.substr(n);
    };
    stripPrefix("https://");
    stripPrefix("http://");
    stripPrefix("www.twitch.tv/");
    stripPrefix("m.twitch.tv/");
    stripPrefix("twitch.tv/");
    while (!s.empty() && (s.front() == '@' || s.front() == '#')) s.erase(0, 1);
    size_t cut = s.find_first_of("/?#");
    if (cut != std::string::npos) s = s.substr(0, cut);
    std::string out;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (ok) out += c;
    }
    return out;
}

// Apply a dock-field input as the poller target: normalize, store, persist, and
// wake if it changed. Returns the normalized login for the field. The dock routes
// its Twitch field commits through here (never touches the poller globals).
static std::string TwSetTargetFromInput(const std::string& input) {
    std::string ch = NormalizeTwitchChannel(input);
    bool changed;
    {
        std::lock_guard<std::mutex> l(g_twStateMutex);
        changed = (ch != g_twChannel);
        g_twChannel = ch;
    }
    if (changed) {
        SaveTwitchChannelSetting(ch);
        TwWakePoller();
    }
    return ch;
}

// Marshal the Twitch connection status to the dock header (deduped, poller-thread
// state only — mirrors YtReportStatus).
static int g_twStatusLast = -1;
static void TwReportStatus(int status) {
    if (status == g_twStatusLast) return;
    g_twStatusLast = status;
    QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
        [status]() { if (g_chatDock) g_chatDock->UpdateTwitchStatus(status); });
}

// --- Twitch avatar cache + GraphQL resolver + download worker --------------
// Twitch IRC carries no avatar, so (parallel to the YouTube cache) resolving one
// takes an extra hop: query Twitch's unofficial GraphQL endpoint by login to get
// the profile-image URL, then download it from the CDN. Same posture as YouTube's
// InnerTube — undocumented, uses Twitch's PUBLIC web Client-ID (ships in every
// browser; NOT a secret, no OAuth, no registration). Keyed by user-id (already
// the row's by-author identity + what the overlay carries); the login is only
// needed transiently for the GQL query. One attempt per user per session; a
// cached null QImage records a miss so we neither refetch nor block — the popup/
// overlay fall back to the neutral circle. Cache mutex/map are external-linkage
// (like the YT cache) so the overlay can read them at render time.
std::mutex                    g_twAvatarCacheMutex;
std::map<std::string, QImage> g_twAvatarCacheByUser;   // userId -> avatar (null = tried, none)
static std::set<std::string>  g_twAvatarInFlight;
static constexpr size_t       kTwAvatarCacheCap = 512;

static std::mutex             g_twAvatarQueueMutex;
static std::vector<std::pair<std::string, std::string>> g_twAvatarQueue;  // (userId, login)
static std::thread            g_twAvatarThread;
static std::atomic<bool>      g_twAvatarExit{false};
static HANDLE                 g_twAvatarEvent = nullptr;  // auto-reset wake

// The well-known PUBLIC Twitch web Client-ID (the value the twitch.tv site and
// OSS libraries send). Not a secret — safe to embed, like the YouTube InnerTube
// key. Verified live 2026-07-08 (see C:\Dev\Greps\twitch_gql_avatar.json).
static const wchar_t* kTwitchGqlHeaders =
    L"Client-ID: kimne78kx3ncx6brgo4mv6wki5h1ko\r\n"
    L"Content-Type: application/json\r\n";

// Resolve a user's avatar from the cache (UI thread, at popup-click time). Null
// QImage if not resolved yet or resolution failed -> popup draws its neutral circle.
static QImage TwResolveAvatar(const std::string& userId) {
    if (userId.empty()) return QImage();
    std::lock_guard<std::mutex> l(g_twAvatarCacheMutex);
    auto it = g_twAvatarCacheByUser.find(userId);
    return it != g_twAvatarCacheByUser.end() ? it->second : QImage();
}

// POST a GraphQL query to gql.twitch.tv with the public Client-ID header. Returns
// true on a 200 with a non-empty body. Dedicated (not YtHttpRequest) because GQL
// needs the Client-ID header and not YouTube's consent cookie.
static bool TwGqlPost(const std::string& body, std::string& respBody) {
    respBody.clear();
    feeds_http::ProxyResolution px =
        feeds_http::ResolveProxyForUrl(L"https://gql.twitch.tv/gql");
    LogProxyResolutionOnce(px, "TwitchGQL");
    HINTERNET hSession = feeds_http::OpenProxiedSession(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) FeedsOBS", px);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 8000, 8000, 10000, 15000);
    HINTERNET hConnect = WinHttpConnect(hSession, L"gql.twitch.tv",
                                        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"POST", L"/gql", nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
    WinHttpAddRequestHeaders(hRequest, kTwitchGqlHeaders, (DWORD)-1,
                             WINHTTP_ADDREQ_FLAG_ADD);
    BOOL sentOk = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                     (LPVOID)body.data(), (DWORD)body.size(),
                                     (DWORD)body.size(), 0);
    bool ok = false;
    if (sentOk && WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD st = 0, stSize = sizeof(st);
        WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &st, &stSize, WINHTTP_NO_HEADER_INDEX);
        char buf[8192]; DWORD n = 0;
        while (WinHttpReadData(hRequest, buf, sizeof(buf), &n) && n > 0)
            respBody.append(buf, n);
        ok = (st == 200 && !respBody.empty());
    }
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

// Query GQL for a login's profile-image URL (small 70px variant). "" on any
// failure or a nonexistent login (data.user = null).
static std::string TwGqlFetchAvatarUrl(const std::string& login) {
    if (login.empty()) return "";
    std::string body =
        "{\"query\":\"query{user(login:\\\"" + JsonEscape(login) +
        "\\\"){profileImageURL(width:70)}}\"}";
    std::string resp;
    if (!TwGqlPost(body, resp)) return "";
    obs_data_t* root = obs_data_create_from_json(resp.c_str());
    if (!root) return "";
    YtObj rootO(root);
    return rootO.obj("data").obj("user").str("profileImageURL");
}

// Queue an avatar resolution for `userId` (query by `login`) if we don't already
// have it / one isn't in flight / the cache isn't at cap. Called from the reader
// thread as messages arrive. Gated at Streamer tier: only the popup/overlay show
// avatars, and Twitch resolution costs an extra GQL round trip (unlike YouTube,
// whose URL rides in the message) — no point warming for a dock-only Basic user.
static void TwRequestAvatar(const std::string& userId, const std::string& login) {
    if (userId.empty() || login.empty()) return;
    if (g_currentTier < 2) return;
    {
        std::lock_guard<std::mutex> l(g_twAvatarCacheMutex);
        if (g_twAvatarCacheByUser.count(userId)) return;   // already have (or tried)
        if (g_twAvatarInFlight.count(userId))     return;   // already resolving
        if (g_twAvatarCacheByUser.size() >= kTwAvatarCacheCap) return;  // bounded
        g_twAvatarInFlight.insert(userId);
    }
    {
        std::lock_guard<std::mutex> l(g_twAvatarQueueMutex);
        g_twAvatarQueue.emplace_back(userId, login);
    }
    if (g_twAvatarEvent) SetEvent(g_twAvatarEvent);
}

static void TwAvatarWorkerLoop() {
    while (!g_twAvatarExit.load()) {
        std::pair<std::string, std::string> job;
        bool have = false;
        {
            std::lock_guard<std::mutex> l(g_twAvatarQueueMutex);
            if (!g_twAvatarQueue.empty()) {
                job = g_twAvatarQueue.front();
                g_twAvatarQueue.erase(g_twAvatarQueue.begin());
                have = true;
            }
        }
        if (!have) {
            if (g_twAvatarEvent) WaitForSingleObject(g_twAvatarEvent, INFINITE);
            continue;
        }
        // job.first = userId, job.second = login. GQL -> URL -> download, reusing
        // the YouTube CDN image path (the URL host is static-cdn.jtvnw.net).
        std::string url = TwGqlFetchAvatarUrl(job.second);
        QImage img = url.empty() ? QImage() : YtDownloadAvatarImage(url);
        {
            std::lock_guard<std::mutex> l(g_twAvatarCacheMutex);
            g_twAvatarCacheByUser[job.first] = img;   // cache result (even null: one try/session)
            g_twAvatarInFlight.erase(job.first);
        }
        // Repaint the overlay so this just-resolved avatar replaces the neutral
        // circle immediately (see YtAvatarWorkerLoop for the rationale). Twitch is
        // where this is most visible — the GQL discovery hop widens the window
        // between row-render and avatar-ready. Skip a cached miss (null).
        if (!img.isNull()) feeds::MarkChatOverlayDirty();
    }
}

// --- Message / deletion fan-out (mirrors the Yt* delivery helpers) ---------
static void TwDeliverToDock(const std::string& author, const std::string& text,
                            const std::string& msgId, const std::string& userId) {
    if (g_currentTier < 1) return;
    QString a = QString::fromStdString(author);
    QString t = QString::fromStdString(text);
    QString id = QString::fromStdString(msgId);
    QString uid = QString::fromStdString(userId);
    QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
        [a, t, id, uid]() {
            if (g_chatDock) g_chatDock->AppendTwitchMessage(a, t, id, uid);
        });
}
static void TwDeliverToOverlay(const std::string& author, const std::string& text,
                               const std::string& userId) {
    if (g_currentTier < 2) return;
    // userId is the overlay's string avatar key (Twitch branch resolves the QImage
    // from g_twAvatarCacheByUser at render time, the way YouTube uses channel id).
    feeds::AppendChatMessageToOverlay(
        feeds::ChatMsgOrigin::Twitch, 0 /*no Zoom id*/, userId, author, text, 0);
}
static void TwDeliverDeletionById(const std::string& msgId) {
    QString id = QString::fromStdString(msgId);
    QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
        [id]() { if (g_chatDock) g_chatDock->RemoveTwitchMessageById(id); });
}
static void TwDeliverDeletionByUser(const std::string& userId) {
    QString uid = QString::fromStdString(userId);
    QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
        [uid]() { if (g_chatDock) g_chatDock->RemoveTwitchMessagesByUser(uid); });
}
static void TwDeliverClearAll() {
    QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
        []() { if (g_chatDock) g_chatDock->RemoveAllTwitchMessages(); });
}

// --- WebSocket transport ---------------------------------------------------
// Open the secure WebSocket to Twitch IRC. On success all three handles are open
// and hWs is returned to the caller (which publishes it under g_twSockMutex);
// on any failure everything opened here is closed and false is returned.
static bool TwWsConnect(HINTERNET& hSession, HINTERNET& hConnect, HINTERNET& hWs) {
    hSession = hConnect = hWs = nullptr;
    // A proxied session works for the upgrade: this is a wss:// request
    // (WINHTTP_FLAG_SECURE to :443), so through a NAMED_PROXY WinHTTP issues
    // CONNECT irc-ws.chat.twitch.tv:443 and runs TLS end-to-end inside that
    // tunnel. The proxy never sees the Upgrade header — it only decides whether
    // to allow the tunnel — and the upgrade options below act on the request
    // handle, which is unaffected by how the session was opened. A proxy that
    // refuses CONNECT, or a TLS-inspecting proxy that re-terminates and drops
    // the upgrade, still fails here: that is the existing fail-soft path
    // (Twitch chat simply doesn't connect), not a regression.
    feeds_http::ProxyResolution px =
        feeds_http::ResolveProxyForUrl(L"https://irc-ws.chat.twitch.tv/");
    LogProxyResolutionOnce(px, "TwitchChat");
    hSession = feeds_http::OpenProxiedSession(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) FeedsOBS", px);
    if (!hSession) return false;
    // Bound connect/send; leave receive at 0 (infinite) — the read loop blocks and
    // is unblocked by TwCloseSocket, not by a timeout (which would drop idle chats).
    WinHttpSetTimeouts(hSession, 8000, 8000, 10000, 0);
    hConnect = WinHttpConnect(hSession, L"irc-ws.chat.twitch.tv",
                              INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); hSession = nullptr; return false; }
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", L"/", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (hReq &&
        WinHttpSetOption(hReq, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
        WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hReq, nullptr)) {
        hWs = WinHttpWebSocketCompleteUpgrade(hReq, 0);
    }
    if (hReq) WinHttpCloseHandle(hReq);   // request handle unneeded post-upgrade
    if (!hWs) {
        WinHttpCloseHandle(hConnect); hConnect = nullptr;
        WinHttpCloseHandle(hSession); hSession = nullptr;
        return false;
    }
    return true;
}

// Send one IRC line (CRLF appended) as a UTF-8 WebSocket message.
static bool TwWsSendLine(HINTERNET hWs, const std::string& line) {
    std::string wire = line + "\r\n";
    return WinHttpWebSocketSend(hWs, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                (PVOID)wire.data(), (DWORD)wire.size()) == NO_ERROR;
}

// --- IRC line parsing ------------------------------------------------------
struct TwIrcLine {
    std::map<std::string, std::string> tags;
    std::string nick;
    std::string command;
    std::string trailing;
};

// Unescape an IRCv3 tag value (\s -> space, \: -> ';', \\ -> '\', \r/\n).
static std::string TwUnescapeTag(const std::string& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            char n = v[++i];
            switch (n) {
            case 's': out += ' ';  break;
            case ':': out += ';';  break;
            case 'r': out += '\r'; break;
            case 'n': out += '\n'; break;
            case '\\': out += '\\'; break;
            default:  out += n;    break;
            }
        } else {
            out += v[i];
        }
    }
    return out;
}

static TwIrcLine TwParseIrcLine(const std::string& raw) {
    TwIrcLine ln;
    size_t pos = 0;
    // Tags: "@k=v;k2=v2 "
    if (pos < raw.size() && raw[pos] == '@') {
        size_t sp = raw.find(' ', pos);
        std::string tagstr = raw.substr(1, (sp == std::string::npos ? raw.size() : sp) - 1);
        size_t start = 0;
        while (start <= tagstr.size()) {
            size_t semi = tagstr.find(';', start);
            std::string kv = tagstr.substr(start, (semi == std::string::npos ? tagstr.size() : semi) - start);
            size_t eq = kv.find('=');
            if (!kv.empty()) {
                if (eq == std::string::npos) ln.tags[kv] = "";
                else ln.tags[kv.substr(0, eq)] = TwUnescapeTag(kv.substr(eq + 1));
            }
            if (semi == std::string::npos) break;
            start = semi + 1;
        }
        pos = (sp == std::string::npos) ? raw.size() : sp + 1;
    }
    while (pos < raw.size() && raw[pos] == ' ') ++pos;
    // Prefix: ":nick!user@host "
    if (pos < raw.size() && raw[pos] == ':') {
        size_t sp = raw.find(' ', pos);
        std::string prefix = raw.substr(pos + 1, (sp == std::string::npos ? raw.size() : sp) - pos - 1);
        size_t bang = prefix.find('!');
        ln.nick = (bang == std::string::npos) ? prefix : prefix.substr(0, bang);
        pos = (sp == std::string::npos) ? raw.size() : sp + 1;
    }
    while (pos < raw.size() && raw[pos] == ' ') ++pos;
    // Command
    { size_t sp = raw.find(' ', pos);
      ln.command = raw.substr(pos, (sp == std::string::npos ? raw.size() : sp) - pos);
      pos = (sp == std::string::npos) ? raw.size() : sp + 1; }
    // Params: scan for the trailing param (" :rest"); middle params are ignored
    // (we key off tags + command + trailing only).
    while (pos < raw.size()) {
        while (pos < raw.size() && raw[pos] == ' ') ++pos;
        if (pos >= raw.size()) break;
        if (raw[pos] == ':') { ln.trailing = raw.substr(pos + 1); break; }
        size_t sp = raw.find(' ', pos);
        pos = (sp == std::string::npos) ? raw.size() : sp + 1;
    }
    return ln;
}

// Handle one complete IRC line: PING keepalive, welcome (-> Live), a chat message,
// or a moderation clear. Unknown commands are skipped defensively. Returns via
// hWs for the PONG reply.
static void TwHandleIrcLine(HINTERNET hWs, const std::string& raw) {
    if (raw.empty()) return;
    // PING can arrive with no tags/prefix — check it before the full parse.
    if (raw.compare(0, 4, "PING") == 0) {
        std::string token = (raw.size() > 5) ? raw.substr(5) : std::string(":tmi.twitch.tv");
        TwWsSendLine(hWs, "PONG " + token);
        return;
    }
    TwIrcLine ln = TwParseIrcLine(raw);
    if (ln.command == "PRIVMSG") {
        auto dn = ln.tags.find("display-name");
        std::string author = (dn != ln.tags.end() && !dn->second.empty()) ? dn->second : ln.nick;
        std::string id     = ln.tags.count("id") ? ln.tags["id"] : "";
        std::string userId = ln.tags.count("user-id") ? ln.tags["user-id"] : "";
        std::string login  = ln.nick;   // IRC nick == lowercase login (GQL query key)
        std::string text   = ln.trailing;
        if (author.empty() && text.empty()) return;
        TwRequestAvatar(userId, login);   // warm the avatar cache (Streamer-gated)
        TwDeliverToDock(author, text, id, userId);
        TwDeliverToOverlay(author, text, userId);   // overlay honors per-instance filter
    } else if (ln.command == "CLEARMSG") {
        std::string tmid = ln.tags.count("target-msg-id") ? ln.tags["target-msg-id"] : "";
        if (!tmid.empty()) TwDeliverDeletionById(tmid);
    } else if (ln.command == "CLEARCHAT") {
        std::string tuid = ln.tags.count("target-user-id") ? ln.tags["target-user-id"] : "";
        if (!tuid.empty()) TwDeliverDeletionByUser(tuid);   // one user timed out/banned
        else               TwDeliverClearAll();             // whole chat cleared
    } else if (ln.command == "001") {
        // Welcome (anonymous auth accepted) — we're connected; JOIN follows.
        TwReportStatus(kChatConnLive);
    }
    // Everything else (CAP/002-004/353/366/JOIN/PART/ROOMSTATE/USERSTATE/
    // USERNOTICE/NOTICE/HOSTTARGET/RECONNECT/...) is intentionally ignored.
}

static void TwChatLoop() {
    blog(LOG_INFO, "[feeds] twitch chat reader thread started");
    while (!g_twShouldExit.load()) {
        std::string channel;
        { std::lock_guard<std::mutex> l(g_twStateMutex); channel = g_twChannel; }
        if (channel.empty()) {
            TwReportStatus(kChatConnOff);
            TwInterruptibleWait(INFINITE); continue;
        }
        TwReportStatus(kChatConnWaiting);
        // Inherit the dock's Basic+ gate exactly — don't even connect for a Free
        // user. Bounded re-check so a login/upgrade is picked up without a wake.
        if (g_currentTier < 1) { TwInterruptibleWait(30000); continue; }

        HINTERNET hSession, hConnect, hWs;
        if (!TwWsConnect(hSession, hConnect, hWs)) {
            blog(LOG_INFO, "[feeds] twitch chat: connect failed (#%s); retrying",
                 channel.c_str());
            if (!g_twShouldExit.load()) TwInterruptibleWait(5000);
            continue;
        }
        // Publish the handle so a shutdown / target change can abort the blocked
        // receive below. If we were told to stop meanwhile, drop it immediately.
        { std::lock_guard<std::mutex> l(g_twSockMutex);
          if (g_twShouldExit.load()) { WinHttpCloseHandle(hWs); hWs = nullptr; }
          else g_twWebSocket = hWs; }
        if (!hWs) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); break; }

        // Anonymous handshake. justinfan<random> avoids collisions across instances;
        // GetTickCount is monotonic-enough for a nonce (no PRNG seeding needed).
        char nick[32];
        snprintf(nick, sizeof(nick), "justinfan%lu",
                 (unsigned long)(GetTickCount() % 1000000u));
        bool sent =
            TwWsSendLine(hWs, "PASS SCHMOOPIIE") &&
            TwWsSendLine(hWs, std::string("NICK ") + nick) &&
            TwWsSendLine(hWs, "CAP REQ :twitch.tv/tags twitch.tv/commands") &&
            TwWsSendLine(hWs, "JOIN #" + channel);
        if (!sent) {
            blog(LOG_INFO, "[feeds] twitch chat: handshake send failed (#%s); reconnecting",
                 channel.c_str());
            TwCloseSocket();
            WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
            if (!g_twShouldExit.load()) TwInterruptibleWait(5000);
            continue;
        }
        blog(LOG_INFO, "[feeds] twitch chat: connected to #%s", channel.c_str());

        // Read loop: accumulate bytes, split on CRLF (Twitch batches lines and can
        // fragment WebSocket frames), handle each complete line. Any receive error
        // (including a cross-thread TwCloseSocket abort) or a CLOSE frame drops out
        // to reconnect / exit.
        std::string rx;
        for (;;) {
            if (g_twShouldExit.load()) break;
            { std::lock_guard<std::mutex> l(g_twStateMutex);
              if (g_twChannel != channel) break; }   // target changed -> reconnect
            if (g_currentTier < 1) {
                blog(LOG_INFO, "[feeds] twitch chat: tier dropped below Basic; disconnecting");
                break;
            }
            BYTE buf[4096];
            DWORD read = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE bt = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
            DWORD rc = WinHttpWebSocketReceive(hWs, buf, sizeof(buf), &read, &bt);
            if (rc != NO_ERROR) {
                blog(LOG_INFO, "[feeds] twitch chat: receive ended (rc=%lu, #%s); reconnecting",
                     (unsigned long)rc, channel.c_str());
                break;
            }
            if (bt == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                blog(LOG_INFO, "[feeds] twitch chat: server closed (#%s); reconnecting",
                     channel.c_str());
                break;
            }
            rx.append((const char*)buf, read);
            size_t nl;
            while ((nl = rx.find('\n')) != std::string::npos) {
                std::string line = rx.substr(0, nl);
                rx.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) TwHandleIrcLine(hWs, line);
            }
        }

        TwCloseSocket();   // closes hWs once (no-op if an abort already did)
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        if (!g_twShouldExit.load()) TwInterruptibleWait(5000);   // backoff, then reconnect
    }
    blog(LOG_INFO, "[feeds] twitch chat reader thread exiting");
}

static void TwStartReaderThread() {
    if (g_twThread.joinable()) return;
    g_twShouldExit = false;
    if (!g_twWakeEvent)
        g_twWakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
    // Avatar resolver worker — shares the reader's lifetime (like the YT worker).
    g_twAvatarExit = false;
    if (!g_twAvatarEvent)
        g_twAvatarEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_twAvatarThread = std::thread(TwAvatarWorkerLoop);
    g_twThread = std::thread(TwChatLoop);
}

static void TwStopReaderThread() {
    g_twShouldExit = true;
    if (g_twWakeEvent) SetEvent(g_twWakeEvent);
    TwCloseSocket();   // abort a blocked receive so the join returns promptly
    if (g_twThread.joinable()) g_twThread.join();
    if (g_twWakeEvent) { CloseHandle(g_twWakeEvent); g_twWakeEvent = nullptr; }

    g_twAvatarExit = true;
    if (g_twAvatarEvent) SetEvent(g_twAvatarEvent);
    if (g_twAvatarThread.joinable()) g_twAvatarThread.join();
    if (g_twAvatarEvent) { CloseHandle(g_twAvatarEvent); g_twAvatarEvent = nullptr; }
}

void SetupPluginMenu() {
    QMainWindow* mainWindow = (QMainWindow*)obs_frontend_get_main_window();
    QMenuBar*    menuBar    = mainWindow->menuBar();
    QMenu*       feedsMenu  = new QMenu("Feeds", menuBar);
    menuBar->addMenu(feedsMenu);

    g_loginLogoutAction  = feedsMenu->addAction("Login to Zoom");
    g_connectAction      = feedsMenu->addAction("Connect to Zoom Meeting");
    feedsMenu->addSeparator();
    g_isoRecordingAction = feedsMenu->addAction("ISO Recording (All Participants)");
    g_isoRecordingAction->setCheckable(true);
    g_isoRecordingAction->setChecked(g_isoRecordingEnabled);
    g_connectOnStartupAction = feedsMenu->addAction("Connect to Zoom on Startup");
    g_connectOnStartupAction->setCheckable(true);
    g_connectOnStartupEnabled = LoadConnectOnStartupSetting();
    g_connectOnStartupAction->setChecked(g_connectOnStartupEnabled);

    // YouTube live chat is configured from the Feeds Chat dock header now (no
    // menu items). Load the persisted target into the poller globals here
    // (FINISHED_LOADING — user config is ready), populate the dock field, and
    // wake the poller so it connects immediately if a target was saved.
    std::string ytHandle, ytVideoId;
    { std::lock_guard<std::mutex> l(g_ytStateMutex);
      g_ytHandle  = LoadYouTubeHandleSetting();
      g_ytVideoId = LoadYouTubeVideoIdSetting();
      ytHandle = g_ytHandle; ytVideoId = g_ytVideoId; }
    if (g_chatDock) g_chatDock->SetYouTubeTargetDisplay(ytHandle, ytVideoId);
    YtWakePoller();

    // Same for the Twitch channel (also configured from the dock header).
    std::string twChannel;
    { std::lock_guard<std::mutex> l(g_twStateMutex);
      g_twChannel = LoadTwitchChannelSetting();
      twChannel = g_twChannel; }
    if (g_chatDock) g_chatDock->SetTwitchTargetDisplay(twChannel);
    TwWakePoller();

    feedsMenu->addSeparator();
    QAction* aboutAction = feedsMenu->addAction("About / Tier Status");

    // Sync menu action states to the current plugin state. If the engine
    // has already finished authenticating (common on startup when a valid
    // refresh token was persisted from a previous session), the login
    // handler fired before this menu existed — its setEnabled() calls
    // silently no-op'd against the then-null action pointers. We re-apply
    // the correct state here now that the actions exist.
    g_connectAction->setEnabled(g_isLoggedIn && !g_isInMeeting);
    UpdateLoginLogoutMenuItem();
    UpdateIsoMenuItemForTier();

    QObject::connect(g_loginLogoutAction, &QAction::triggered, []() {
        if (g_authInProgress && !g_isLoggedIn) {
            // User clicked "Cancel login". Tell the engine first so it sets
            // its cancel flag and the worker poll loop exits promptly,
            // discarding the PKCE verifier/state with the OAuth thread.
            // Then clear local state.
            blog(LOG_INFO, "[feeds] login cancelled by user");
            feeds::SendToEngine("{\"type\":\"login_cancel\"}");
            g_authInProgress = false;
            g_pendingMeetingJoin = false;  // matches what login_failed clears
            UpdateLoginLogoutMenuItem();
            return;
        }
        if (g_isLoggedIn) OnLogoutClick();
        else              OnLoginClick();
    });
    QObject::connect(g_connectAction, &QAction::triggered, []() { OnConnectClick(); });
    QObject::connect(g_isoRecordingAction, &QAction::toggled, [](bool checked) {
        g_isoRecordingEnabled = checked;
        ApplyIsoRecordingStateToAllSources();
    });
    QObject::connect(g_connectOnStartupAction, &QAction::toggled, [](bool checked) {
        g_connectOnStartupEnabled = checked;
        SaveConnectOnStartupSetting(checked);
    });
    QObject::connect(aboutAction, &QAction::triggered, []() {
        ShowAboutDialog();
    });
}

// ---------------------------------------------------------------------------
// Source callbacks
// ---------------------------------------------------------------------------
// Throttle for the tier "upgrade required" popup. When OBS loads a saved
// scene, create callbacks fire for every source in rapid succession — if the
// user has more sources than their current tier allows, we don't want to
// stack N popups. Show at most one per throttle window. Used by the
// screenshare and chat-source create paths; the participant source no longer
// pops a create-time dialog — it creates a dormant source instead.
static std::atomic<uint64_t> g_lastTierPopupMs{0};
static constexpr uint64_t TIER_POPUP_THROTTLE_MS = 3000;

// Non-static so the chat popup and chat overlay sources can call it
// during their own tier-gating create paths.
bool ShouldShowTierPopup() {
    uint64_t now  = GetTickCount64();
    uint64_t last = g_lastTierPopupMs.load();
    if (now - last < TIER_POPUP_THROTTLE_MS) return false;
    g_lastTierPopupMs.store(now);
    return true;
}

// Modal popup with a clickable hyperlink, parented to the OBS main window
// so it inherits OBS title-bar chrome. Called from zp_create / zs_create,
// which may run on non-UI threads — dispatch to the main thread via
// QTimer::singleShot, matching the pattern used elsewhere in this file.
// Uses show() + ApplicationModal + WA_DeleteOnClose rather than exec(),
// since we're inside a queued lambda and don't want to nest event loops.
// Non-static so the chat popup and chat overlay sources can show their
// own upgrade dialogs from their tier-gating create paths.
void ShowTierLimitDialog(const QString& title, const QString& html) {
    QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
        [title, html]() {
            QMainWindow* mainWindow =
                (QMainWindow*)obs_frontend_get_main_window();

            QDialog* dlg = new QDialog(mainWindow);
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            dlg->setWindowTitle(title);
            dlg->setWindowModality(Qt::ApplicationModal);
            // Wider by default so the body isn't cramped and a call-to-action link on
            // its own line has room to read as one. Shared by every caller of this
            // dialog (the header locked-click upgrade prompt and the screenshare
            // "already exists" notice), so both get the roomier layout.
            dlg->setMinimumWidth(420);

            QLabel* label = new QLabel(html, dlg);
            label->setTextFormat(Qt::RichText);
            label->setTextInteractionFlags(Qt::TextBrowserInteraction);
            label->setOpenExternalLinks(true);
            label->setWordWrap(true);

            QPushButton* okBtn = new QPushButton("OK", dlg);
            okBtn->setDefault(true);
            QObject::connect(okBtn, &QPushButton::clicked,
                             dlg, &QDialog::accept);

            QVBoxLayout* layout = new QVBoxLayout(dlg);
            layout->addWidget(label);
            QHBoxLayout* btnRow = new QHBoxLayout();
            btnRow->addStretch();
            btnRow->addWidget(okBtn);
            layout->addLayout(btnRow);

            dlg->show();
        });
}

// Name hook for the ISO recorder — resolves the filename's "<participant
// name>" once, at record start. Called from the recorder (graphics thread).
// Fallback chain: selected participant's Zoom name -> OBS source name (the
// recorder applies the final "Feeds ISO" guard if even that is empty). Never
// blocks beyond a brief mutex, never throws.
static std::string ResolveParticipantName(void* userdata) {
    ZpSourceData* d = static_cast<ZpSourceData*>(userdata);
    if (!d) return "";

    // current_user_id 0 = no selection, 1 = [Active Speaker] sentinel — both
    // lack a stable participant name, so fall through to the source name.
    if (d->current_user_id > 1) {
        std::lock_guard<std::mutex> lock(g_participantsMutex);
        for (const auto& p : g_cachedParticipants) {
            if (p.id == d->current_user_id) {
                if (!p.name.empty() && p.name != "Unknown")
                    return p.name;
                break;
            }
        }
    }

    const char* sn = obs_source_get_name(d->source);
    return sn ? std::string(sn) : std::string();
}

static void* zp_create(obs_data_t* settings, obs_source_t* source) {
    (void)settings;

    // A participant source must ALWAYS be created. Returning nullptr from a
    // create callback makes OBS keep an invalid husk and discard the saved
    // source during scene load (Failed to create source -> Tried to add a
    // removed source -> dropped), and the next auto-save bakes in the loss —
    // so opening OBS while logged out used to permanently delete the user's
    // participant sources. nullptr is reserved for genuine allocation/exception
    // failures and the hard-cap refusal below — which is gated on NOT loading a
    // scene collection, so it can never drop a saved source. A source that can't
    // pull a live feed yet (not logged in, engine not ready, no participant
    // bound, or over the tier's active limit) is created in a dormant state
    // instead: its properties panel explains why (logged-out branch / upgrade
    // prompt), and selecting a participant once live brings it up like any other.
    //
    // C++ exceptions must not escape into libobs C frames, so the body stays
    // wrapped; on a real failure nullptr + OnSourceCreated's husk cleanup is
    // still far cheaper than a process-wide crash.
  try {
    // Hard cap: refuse an interactive attempt to create a 9th participant source
    // (see kMaxParticipantSourcesEver) up front — immediate dialog, no source, no
    // OBS name/properties ceremony — mirroring how zs_create refuses a second
    // screenshare. This replaces the clunky create-then-remove UX for the common
    // case (the OnSourceCreated block still backstops any path where this count
    // is stale). Gated on !g_sceneCollectionLoading: returning null during a
    // scene load is the husk/data-loss bug (OBS drops the saved source, auto-save
    // bakes it in), so during load we fall through and construct untouched. Count
    // is read here BEFORE this source registers, so the test is >= (vs. the > the
    // OnSourceCreated block uses after zp_create has pushed the new source).
    if (!g_sceneCollectionLoading.load()) {
        size_t count;
        {
            std::lock_guard<std::mutex> lock(g_sourcesMutex);
            count = g_allParticipantSources.size();
        }
        if (count >= (size_t)kMaxParticipantSourcesEver) {
            if (ShouldShowTierPopup()) {
                ShowTierLimitDialog(
                    "Feeds: Participant feed limit",
                    QString("%1 is the maximum supported number of participant "
                            "feeds, so this source can't be added.")
                        .arg(kMaxParticipantSourcesEver));
            }
            return nullptr;
        }
    }

    obs_source_set_async_unbuffered(source, true);

    ZpSourceData* data = new ZpSourceData();
    data->source = source;

    const char* uuid = obs_source_get_uuid(source);
    data->uuid = uuid ? uuid : "";

    {
        std::lock_guard<std::mutex> lock(g_sourcesMutex);
        g_allParticipantSources.push_back(data);

        // Tier enforcement is now "create dormant," never "refuse to
        // create." Creation-order position (1-based, matching
        // ReconcileSourcesToTier) decides eligibility: only the first
        // GetMaxFeedsForTier() sources may actively pull a feed; the rest
        // sit tier-disabled and show the upgrade prompt in their
        // properties, and zp_update refuses to bind them. Only mark it
        // here when we already know the tier (logged in) — a source
        // created while logged out stays eligible, and
        // ReconcileSourcesToTier recomputes every source's state on login.
        data->source_position = (int)g_allParticipantSources.size();
        if (g_isLoggedIn)
            data->tier_disabled =
                (data->source_position > GetMaxFeedsForTier());
    }

    // A new source joined the registry — rebuild the participant dock. Marshalled
    // to the UI thread; the new source shows as unassigned until it's bound.
    PostParticipantDockRefresh();

    // Per-source ISO recorder. The name hook reads this source's selected
    // participant; the tier seed gates recording until the user is Basic+.
    // Seed the enabled state from the global toggle so a source created
    // while the menu toggle is already on starts in the right state.
    data->iso = feeds::feeds_iso_recorder_create(source, ResolveParticipantName, data);
    feeds::feeds_iso_recorder_set_tier(data->iso, g_currentTier);
    feeds::feeds_iso_recorder_set_enabled(
        data->iso, g_isoRecordingEnabled && g_currentTier >= 1);

    return data;
  } catch (const std::exception& e) {
    blog(LOG_ERROR, "[feeds] zp_create exception: %s", e.what());
    return nullptr;
  } catch (...) {
    blog(LOG_ERROR, "[feeds] zp_create unknown exception");
    return nullptr;
  }
}

static void zp_destroy(void* vdata) {
    if (!vdata) return;
    ZpSourceData* data = static_cast<ZpSourceData*>(vdata);

    try {
        if (!data->uuid.empty()) {
            std::string msg = "{\"type\":\"participant_source_unsubscribe\","
                              "\"source_id\":\"" + data->uuid + "\"}";
            feeds::SendToEngine(msg);
        }

        // Erase from the global registry FIRST so any IPC handler arriving
        // mid-destruction (source_texture_released, meeting_left, etc.)
        // can't find this source via FindSourceByUuid and won't try to
        // operate on it while we're tearing it down. CloseSharedMemory's
        // per-source lifecycleMutex still protects against any handler
        // that captured the pointer before this erase.
        {
            std::lock_guard<std::mutex> lock(g_sourcesMutex);
            auto it = std::find(g_allParticipantSources.begin(),
                                g_allParticipantSources.end(), data);
            if (it != g_allParticipantSources.end())
                g_allParticipantSources.erase(it);
        }

        // Source left the registry — rebuild the dock. Captures nothing
        // source-specific (this ZpSourceData is about to be deleted); the
        // marshalled Refresh runs on the UI thread AFTER this erase, so its
        // re-snapshot already excludes the dead source. zp_destroy runs on
        // OBS's destruction worker thread, so marshalling is mandatory here.
        PostParticipantDockRefresh();

        // Tear down the ISO recorder while the source is still valid (it
        // borrows the source pointer). Synchronous, drain-aware, bounded.
        feeds::feeds_iso_recorder_destroy(data->iso);
        data->iso = nullptr;

        // clearTexture=false: source is being destroyed, don't touch it.
        // CloseSharedMemory takes data->lifecycleMutex internally, so any
        // concurrent handler still inside it serialises here.
        CloseSharedMemory(data, false);
    } catch (const std::exception& e) {
        blog(LOG_ERROR, "[feeds] zp_destroy exception: %s", e.what());
    } catch (...) {
        blog(LOG_ERROR, "[feeds] zp_destroy unknown exception");
    }
    delete data;
}

static void zp_update(void* vdata, obs_data_t* settings) {
    if (!vdata) return;
    ZpSourceData* data = static_cast<ZpSourceData*>(vdata);

    // OBS calls this from a C frame with no exception handler. Any C++
    // exception that escapes here is unhandled (SEH 0xE06D7363) and
    // terminates OBS. Swallow + log so the user sees a log message
    // instead of a crash.
    try {
        // ISO recording enable state is driven by the global Feeds menu
        // toggle, not per-source settings — applied via
        // ApplyIsoRecordingStateToAllSources() when the toggle or tier
        // changes, and seeded in zp_create for newly created sources.

        // Defensive: zp_properties replaces the dropdown with an upgrade
        // message for tier-disabled sources, so a normal user flow can't
        // reach here. Guards against unexpected paths (scripted update,
        // scene-collection import) that would otherwise let a disabled
        // source send subscribe IPC.
        if (data->tier_disabled) return;

        unsigned int selected_id =
            (unsigned int)obs_data_get_int(settings, "participant_id");

        if (selected_id == data->current_user_id) return;

        // 0 is "--- Select Participant ---" — no subscription.
        if (selected_id == 0) {
            data->current_user_id = 0;
            data->subscribed_user_id = 0;  // subscription torn down below
            if (!data->uuid.empty()) {
                std::string msg = "{\"type\":\"participant_source_unsubscribe\","
                                  "\"source_id\":\"" + data->uuid + "\"}";
                feeds::SendToEngine(msg);
            }
            CloseSharedMemory(data);
            return;
        }

        // Tier enforcement is the tier_disabled flag (set by creation-order
        // position in zp_create and recomputed by ReconcileSourcesToTier on
        // login). A tier-disabled source already returned above, so reaching
        // here means this source is within the tier's active limit and may
        // bind a feed — only up to GetMaxFeedsForTier() sources can.
        data->current_user_id = selected_id;

        // selected_id == 1 is [Active Speaker] sentinel. Engine handles the
        // follow-speaker routing — we just pass the sentinel through.
        // selected_id > 1 is a real Zoom SDK user ID.
        if (!data->uuid.empty() && g_isInMeeting && g_rawLiveStreamGranted) {
            std::string msg = "{\"type\":\"participant_source_subscribe\","
                              "\"source_id\":\"" + data->uuid + "\","
                              "\"participant_id\":" + std::to_string(selected_id) + "}";
            feeds::SendToEngine(msg);
            // Record what we subscribed so the reconcile / privilege-granted
            // paths don't redundantly re-subscribe this same id.
            data->subscribed_user_id = selected_id;
        }
    } catch (const std::exception& e) {
        blog(LOG_ERROR, "[feeds] zp_update exception: %s", e.what());
    } catch (...) {
        blog(LOG_ERROR, "[feeds] zp_update unknown exception");
    }
}

// Drives the ISO recorder's per-frame state machine (lazy view creation,
// start/stop, pause). Exception-guarded like the other libobs C callbacks.
static void zp_video_tick(void* vdata, float seconds) {
    if (!vdata) return;
    ZpSourceData* data = static_cast<ZpSourceData*>(vdata);
    try {
        feeds::feeds_iso_recorder_tick(data->iso, seconds);
    } catch (const std::exception& e) {
        blog(LOG_ERROR, "[feeds] zp_video_tick exception: %s", e.what());
    } catch (...) {
        blog(LOG_ERROR, "[feeds] zp_video_tick unknown exception");
    }
}

// ---------------------------------------------------------------------------
// Properties panel
// ---------------------------------------------------------------------------
// English ordinal suffix for a positive integer. 11/12/13 are the
// classic exceptions ("11th" not "11st"); written defensively even
// though tier limits cap us at 8 today.
static const char* OrdinalSuffix(int n) {
    int abs_n = n < 0 ? -n : n;
    int lastTwo = abs_n % 100;
    if (lastTwo >= 11 && lastTwo <= 13) return "th";
    switch (abs_n % 10) {
        case 1:  return "st";
        case 2:  return "nd";
        case 3:  return "rd";
        default: return "th";
    }
}

// Returns the privilege-aware status-label text for an in-meeting source
// properties panel, or an empty string when state == Granted (caller
// uses its own normal status text in that case). Shared by zp_properties
// and zs_properties so wording stays in lockstep.
//
// State mapping:
//   Granted        → "" (caller uses normal "Connected to Meeting X" text)
//   Pending        → "Waiting for host..." message (popup definitely up)
//   NotRequested   → same as Pending (covers the brief gap between
//                    meeting_joined and raw_livestream_pending/granted;
//                    no actionable state for the user yet)
//   Denied         → "Host denied..." with leave-and-rejoin instructions.
//                    Only set on a real onRawLiveStreamPrivilegeChanged
//                    (false) callback — pre-timeout user action, or
//                    post-grant revoke. Both unambiguously mean rejoin
//                    is required.
//   TimedOut       → "Still waiting... please check with the host" — the
//                    SDK has stopped reporting host actions on the popup
//                    (Phase B-Extended diagnostics confirmed: zero
//                    callbacks or query-state changes for post-timeout
//                    Deny). We honestly don't know what the host did,
//                    so the wording is honest about that uncertainty.
static std::string PrivilegeStatusText() {
    if (g_rawPrivilegeState == RawPrivilegeState::Granted) {
        return std::string();
    }
    switch (g_rawPrivilegeState) {
        case RawPrivilegeState::Denied:
            return "Status: Host denied use of Feeds. Please leave the "
                   "meeting, rejoin, and have the host click 'Grant "
                   "Permission' on the Request to Livestream popup.";
        case RawPrivilegeState::TimedOut:
            return "Status: Still waiting for permission. Please check "
                   "with the host. If they closed the popup, rejoin the "
                   "meeting.";
        case RawPrivilegeState::Pending:
        case RawPrivilegeState::NotRequested:
        default:
            return "Status: Waiting for host to grant Feeds permission. "
                   "A popup is visible on the host's screen.";
    }
}

// Record the "binding-record" half of an interactive participant selection into
// the given source's settings/data: resolve the selected id (read from the
// passed-in settings) to a display name, persist the durable remembered-name key
// only when a non-empty name resolves, and set bound_this_session from the id
// bucket. This is the shared choke point both interactive UIs drive — the
// properties dropdown (zp_participant_modified) today, and the participant dock
// later — so both produce identical binding behavior. It does NOT subscribe;
// the subscribe half stays in the deferred zp_update on the graphics thread.
//
// Contract — MUST be called on the UI (Qt main) thread. It writes
// bound_this_session WITHOUT any lock, which is safe only because every writer
// of that flag is serialized on the UI thread (or holds g_sourcesMutex, which
// this helper deliberately does not). Callers pass the ZpSourceData* directly
// (as zp_participant_modified does via priv) and must NOT do a g_sourcesMutex
// source lookup around this call: that would nest g_sourcesMutex -> the
// g_participantsMutex taken below, the opposite of ReconcileRememberedParticipants'
// ordering (which never holds the two at once), and a latent deadlock. This
// helper takes g_participantsMutex only, never g_sourcesMutex.
static void RecordParticipantBinding(ZpSourceData* data, obs_data_t* settings) {
    if (!settings) return;
    long long sel = obs_data_get_int(settings, "participant_id");

    if (sel > 1) {
        std::string name;
        {
            std::lock_guard<std::mutex> lock(g_participantsMutex);
            for (const auto& p : g_cachedParticipants) {
                if (p.id == (unsigned int)sel) { name = p.name; break; }
            }
        }
        // Only overwrite the durable key when we actually resolved a name;
        // a momentary cache miss must not erase a good remembered name.
        if (!name.empty())
            obs_data_set_string(settings, kParticipantNameKey, name.c_str());
        if (data) data->bound_this_session = true;
        blog(LOG_INFO, "[feeds] bind-decision: rule=manual-assign source='%s' "
             "key='%s' userId=%lld bound_this_session=1",
             (data && data->source) ? obs_source_get_name(data->source) : "",
             name.c_str(), sel);
    } else {
        obs_data_set_string(settings, kParticipantNameKey, "");
        if (data) data->bound_this_session = false;
        blog(LOG_INFO, "[feeds] bind-decision: rule=manual-unassign source='%s' "
             "userId=%lld bound_this_session=0",
             (data && data->source) ? obs_source_get_name(data->source) : "", sel);
    }

    // Interactive assignment changed — refresh the participant dock. This is the
    // per-user-pick choke point for every interactive UI (properties dropdown
    // today, dock dropdown later), and it's the only assignment-mutation path
    // not already covered by the RefreshAllSourceProperties co-located refresh
    // (reconcile / grant / tier changes all route through that). Marshalled, so
    // by the time Refresh() runs the new participant_id is already committed to
    // the source's settings (the properties view edits the source's live
    // settings object, so the combo write lands synchronously before this).
    PostParticipantDockRefresh();
}

// Commit a participant reassignment picked in the dock combo, reproducing the
// properties path's binding sequence exactly so the two UIs are identical by
// construction. UI-thread only (called from a QComboBox::activated slot).
//
// Reaches the source without g_sourcesMutex: obs_get_source_by_uuid returns an
// addref'd source whose ref keeps the source AND its ZpSourceData alive for the
// handler's duration (zp_destroy can't run while ref-held) — exactly how OBS
// keeps the source alive during the properties modified-callback. ZpSourceData
// comes from obs_obj_get_data (the create-returned instance data; borrowed, may
// be null for a husk — RecordParticipantBinding null-guards it). Never touches
// g_sourcesMutex, so the two locks are never nested; the helper locks only
// g_participantsMutex internally with nothing held across it.
//
// Sequence order is load-bearing: set_int commits participant_id to the source's
// LIVE settings object (obs_source_get_settings returns it, not a copy), so
// RecordParticipantBinding then reads the just-committed id, and the deferred
// zp_update (scheduled by obs_source_update on this VIDEO-flagged source) reads
// it on the next graphics tick to drive the subscribe. The subscribe is
// asynchronous — the engine has not been messaged when this returns; we don't
// wait on or check it. RecordParticipantBinding already posts the single dock
// refresh; the handler must not post another (double-rebuild).
static void OnDockParticipantPicked(const std::string& uuid, long long selectedId) {
    obs_source_t* src = obs_get_source_by_uuid(uuid.c_str());
    if (!src) return;   // source destroyed since the combo was built — nothing to do

    ZpSourceData* data = static_cast<ZpSourceData*>(obs_obj_get_data(src));
    obs_data_t* settings = obs_source_get_settings(src);
    if (settings) {
        obs_data_set_int(settings, "participant_id", selectedId);
        RecordParticipantBinding(data, settings);
        obs_source_update(src, settings);
        obs_data_release(settings);
    }
    obs_source_release(src);
}

// Fires ONLY when the user changes the participant dropdown in the properties
// dialog (not on scene load / programmatic update) — exactly the "manual
// selection" hook. Manual selection overrides and updates the remembered name
// so it doesn't revert next session; picking a real participant (>1) records
// their name and marks the binding live, while unselect (0) or [Active Speaker]
// (1) clears the remembered name (clearing on unselect is what lets the user
// actually unbind a present participant without ReconcileRememberedParticipants
// immediately re-binding them). Writing the name only on a genuine user action
// is deliberate: it avoids clobbering the durable name from a stale
// participant_id loaded on OBS restart, whose runtime ID can collide with a
// different present person.
//
// Thin wrapper over RecordParticipantBinding — the shared binding-record path.
// Runs on the UI thread (properties-dialog modified callback), satisfying the
// helper's UI-thread contract.
static bool zp_participant_modified(void* priv, obs_properties_t* props,
                                    obs_property_t* property,
                                    obs_data_t* settings) {
    (void)props;
    (void)property;
    ZpSourceData* data = static_cast<ZpSourceData*>(priv);
    RecordParticipantBinding(data, settings);
    return false;  // settings change persists; no property layout change needed
}

static obs_properties_t* zp_properties(void* data) {
  // C++ exceptions must not escape into libobs C frames. On exception we
  // return an empty properties object so the dialog still opens (instead
  // of crashing OBS).
  try {
    // data can be nullptr in some Qt code paths (e.g., properties
    // queried before source creation completes); null-guard the
    // tier_disabled access.
    ZpSourceData* d = static_cast<ZpSourceData*>(data);
    if (d && d->tier_disabled) {
        obs_properties_t* props = obs_properties_create();
        std::string verLabel = std::string("Feeds (v") +
                               feeds_shared::VERSION + ")";
        obs_properties_add_text(props, "ver_label", verLabel.c_str(),
                                OBS_TEXT_INFO);

        int maxFeeds = GetMaxFeedsForTier();
        int pos      = d->source_position;
        // A source beyond the product max (position > the hard cap) can never be
        // activated by ANY upgrade — no tier goes higher — so it gets the
        // "maximum supported" wording with no upgrade button, regardless of the
        // user's current tier. A lower tier's over-cap-but-within-max source
        // (e.g. a Streamer's 6th-8th) keeps the upgrade path, since upgrading to
        // Broadcaster would activate it.
        const bool atCeiling = (pos > kMaxParticipantSourcesEver);
        std::string msg =
            atCeiling
              ? (std::to_string(maxFeeds) +
                 " is the maximum supported number of participant feeds. This is "
                 "your " + std::to_string(pos) + OrdinalSuffix(pos) +
                 " participant source, so it can't be activated.")
              : ("Your current tier only allows " + std::to_string(maxFeeds) +
                 " participant source" + (maxFeeds == 1 ? "" : "s") +
                 ". This is your " + std::to_string(pos) + OrdinalSuffix(pos) +
                 " participant source. Please upgrade to activate this feed.");
        obs_properties_add_text(props, "tier_disabled_msg", msg.c_str(),
                                OBS_TEXT_INFO);
        if (!atCeiling) {
            obs_properties_add_button(props, "upgrade_btn",
                "Upgrade your plan to activate more feeds",
                [](obs_properties_t*, obs_property_t*, void*) -> bool {
                    QDesktopServices::openUrl(
                        QUrl("https://letsdovideo.com/feeds-upgrade"));
                    return true;
                });
        }
        return props;
    }

    // Skip the participant-list request when privilege isn't granted —
    // the engine can't deliver one, and there's no dropdown to populate.
    if (g_isInMeeting && g_rawPrivilegeState == RawPrivilegeState::Granted)
        feeds::SendToEngine("{\"type\":\"get_participants\"}");

    obs_properties_t* props = obs_properties_create();

    // One state-driven slot at the top: a button when not in a meeting, a
    // dropdown (greyed or live) when in a meeting. RefreshAllSourceProperties()
    // fires on login_success, meeting_joined, meeting_ended, logout,
    // participant_list_changed, and every raw_livestream_* transition, so
    // this re-evaluates without a properties-dialog reopen.
    if (!g_isInMeeting) {
        if (!g_isLoggedIn) {
            obs_properties_add_button(props, "login_btn",
                "Not logged in to Zoom. Click to Login.",
                [](obs_properties_t*, obs_property_t*, void*) -> bool {
                    OnLoginClick();
                    return true;
                });
        } else {
            obs_properties_add_button(props, "connect_btn",
                "Logged in. Click to Connect to Zoom Meeting.",
                [](obs_properties_t*, obs_property_t*, void*) -> bool {
                    OnConnectClick();
                    return true;
                });
        }
    } else {
        obs_property_t* list = obs_properties_add_list(
            props, "participant_id", "Select Participant",
            OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

        if (g_rawPrivilegeState != RawPrivilegeState::Granted) {
            std::string msg = PrivilegeStatusText();
            if (msg.empty()) msg = "Waiting for permission";
            obs_property_list_add_int(list, msg.c_str(), 0);
            obs_property_set_enabled(list, false);
        } else {
            std::lock_guard<std::mutex> lock(g_participantsMutex);
            size_t selectable = 0;
            for (const auto& p : g_cachedParticipants) {
                if (g_cachedMyUserId != 0 && p.id == g_cachedMyUserId) continue;
                ++selectable;
            }
            if (selectable == 0) {
                obs_property_list_add_int(list, "Waiting for participants...", 0);
                obs_property_set_enabled(list, false);
            } else {
                obs_property_list_add_int(list, "--- Select Participant ---", 0);
                obs_property_list_add_int(list, "[Active Speaker]", 1);
                for (const auto& p : g_cachedParticipants) {
                    if (g_cachedMyUserId != 0 && p.id == g_cachedMyUserId) continue;
                    obs_property_list_add_int(list, p.name.c_str(), (long long)p.id);
                }
                // Capture manual selections into the durable remembered-name
                // key (priv = this source's data, for the bound_this_session
                // flag). Only attached to the live, selectable dropdown.
                obs_property_set_modified_callback2(
                    list, zp_participant_modified, data);
            }
        }
    }

    // Contextual hints for users who skipped the tutorial video. Plain
    // OBS_TEXT_INFO entries render as muted info text in the properties
    // panel — visually distinct from the interactive controls above.
    obs_properties_add_text(props, "tip_audio",
        "AUDIO: The OBS default desktop audio source should capture "
        "what you hear from Zoom. Adjust if needed.",
        OBS_TEXT_INFO);
    obs_properties_add_text(props, "tip_vcam",
        "RETURN VIDEO: Use OBS Virtual Camera to send the show video "
        "back to Zoom.",
        OBS_TEXT_INFO);

    return props;
  } catch (const std::exception& e) {
    blog(LOG_ERROR, "[feeds] zp_properties exception: %s", e.what());
    return obs_properties_create();
  } catch (...) {
    blog(LOG_ERROR, "[feeds] zp_properties unknown exception");
    return obs_properties_create();
  }
}

// ---------------------------------------------------------------------------
// Screenshare source
// ---------------------------------------------------------------------------
//
// Multiple screenshare sources can coexist in a scene (for filter variants,
// different crops, etc.). They all map the same shared-memory region —
// written by the single engine-side SDK renderer — and each runs its own
// pump thread to deliver frames to its own OBS source.
//
// No tier gating, no participant dropdown, no multi-select. The source
// automatically follows whoever is currently sharing (state is driven
// from the engine, forwarded to the plugin via share_status_changed).

struct ZsSourceData {
    obs_source_t* source = nullptr;
    // See ZpSourceData::tier_disabled — same semantics. For screenshare
    // the rule is simpler: true iff g_currentTier == 0 (Free).
    bool          tier_disabled = false;

    HANDLE mapping = nullptr;
    void*  view    = nullptr;
    feeds_shared::SharedFrameHeader* header     = nullptr;
    feeds_shared::FrameSlot*         frameSlots = nullptr;

    std::thread       pumpThread;
    std::atomic<bool> pumpShouldExit{false};
    HANDLE            pumpWakeEvent = nullptr;
    uint32_t          lastReadIndex = 0;

    // Visibility gate + show-redeliver for the stale-frame-on-reshow fix.
    // Flipped by the .show/.hide source callbacks (graphics/tick thread) and
    // read by the pump (its own thread) — hence atomic. Per-source: several
    // screenshare sources can read the same ring, each with its own gate.
    //   showing          — false while the source is hidden; the pump skips ALL
    //                       delivery then, so OBS never caches a frame into a
    //                       source nobody sees (which is what re-arms
    //                       async_active and reintroduces the stale paint). The
    //                       source starts not-showing; OBS fires .show on first
    //                       display.
    //   redeliverPending — set by .show so the pump re-emits the newest ring
    //                       slot once on re-show (even with no new write),
    //                       overwriting OBS's stale async texture with current
    //                       content within ~1 tick.
    std::atomic<bool> showing{false};
    std::atomic<bool> redeliverPending{false};

    // Same role as ZpSourceData::lifecycleMutex. Held across Open/Close
    // ShareSharedMemory bodies (which call ZsStart/ZsStopPumpThread).
    // Lock ordering: g_screenshareSourcesMutex (outer) →
    // ZsSourceData::lifecycleMutex (inner).
    std::mutex        lifecycleMutex;
};

static std::mutex                 g_screenshareSourcesMutex;
static std::vector<ZsSourceData*> g_allScreenshareSources;

static void ZsPumpThreadFunc(ZsSourceData* data) {
    if (!data || !data->source) return;

    blog(LOG_INFO, "[feeds] screenshare pump thread started");

    while (!data->pumpShouldExit) {
        WaitForSingleObject(data->pumpWakeEvent, 8);

        if (data->pumpShouldExit) break;
        if (!data->header || !data->frameSlots) continue;

        // Visibility gate: deliver nothing while the source is hidden. This is
        // what makes the .hide NULL-clear robust — without it, a mid-hide
        // content change would re-arm OBS's async_active and reintroduce the
        // stale paint on re-show — and it avoids copying frames into a source
        // nobody can see. The newest ring slot stays available for the
        // show-redeliver below. Per-source flag; other screenshare sources on
        // the same ring gate independently.
        if (!data->showing.load(std::memory_order_acquire)) continue;

        uint32_t currentWrite = data->header->write_index;

        // Show-redeliver: on re-show, re-emit the newest ring slot once even if
        // the write index hasn't advanced, so re-show overwrites OBS's stale
        // async texture with Zoom's current content. Guard on a real frame
        // existing (write_index > 0) so a never-written region isn't read
        // (avoids the (currentWrite - 1) underflow below).
        bool redeliver = data->redeliverPending.exchange(false, std::memory_order_acq_rel);
        if (redeliver && currentWrite == 0) redeliver = false;

        if (!redeliver && currentWrite == data->lastReadIndex) continue;

        uint32_t slotIdx = (currentWrite - 1) % feeds_shared::RING_SLOTS;
        feeds_shared::FrameSlot* slot = &data->frameSlots[slotIdx];

        MemoryBarrier();

        uint32_t width  = slot->width;
        uint32_t height = slot->height;

        if (width  == 0 || height == 0 ||
            width  > feeds_shared::MAX_FRAME_WIDTH ||
            height > feeds_shared::MAX_FRAME_HEIGHT) {
            data->lastReadIndex = currentWrite;
            continue;
        }

        size_t ySize = (size_t)width * height;
        size_t uSize = (size_t)(width / 2) * (height / 2);

        struct obs_source_frame obsFrame = {};
        obsFrame.format      = VIDEO_FORMAT_I420;
        obsFrame.width       = width;
        obsFrame.height      = height;
        obsFrame.data[0]     = slot->data;
        obsFrame.data[1]     = slot->data + ySize;
        obsFrame.data[2]     = slot->data + ySize + uSize;
        obsFrame.linesize[0] = slot->stride_y;
        obsFrame.linesize[1] = slot->stride_u;
        obsFrame.linesize[2] = slot->stride_v;

        video_format_get_parameters(VIDEO_CS_DEFAULT, VIDEO_RANGE_PARTIAL,
                                    obsFrame.color_matrix,
                                    obsFrame.color_range_min,
                                    obsFrame.color_range_max);

        obsFrame.timestamp = os_gettime_ns();

        obs_source_output_video(data->source, &obsFrame);

        // NOTE: don't update header->last_read_index from here — multiple
        // screenshare sources share the same region. If we each touched
        // that field we'd be racing. The writer doesn't actually use it
        // for flow control (it's informational only), so leaving it
        // untouched is safe and correct.
        data->lastReadIndex = currentWrite;
    }

    blog(LOG_INFO, "[feeds] screenshare pump thread exiting");
}

// Caller MUST hold data->lifecycleMutex — see StartPumpThread comment.
static void ZsStartPumpThread(ZsSourceData* data) {
    if (!data) return;
    if (data->pumpThread.joinable()) return;

    data->pumpShouldExit = false;
    if (!data->pumpWakeEvent) {
        data->pumpWakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    data->pumpThread = std::thread(ZsPumpThreadFunc, data);
}

// Caller MUST hold data->lifecycleMutex — see StopPumpThread comment.
static void ZsStopPumpThread(ZsSourceData* data) {
    if (!data) return;

    data->pumpShouldExit = true;
    if (data->pumpWakeEvent) SetEvent(data->pumpWakeEvent);

    if (data->pumpThread.joinable()) {
        data->pumpThread.join();
    }

    if (data->pumpWakeEvent) {
        CloseHandle(data->pumpWakeEvent);
        data->pumpWakeEvent = nullptr;
    }
}

// Opens the well-known screenshare shared-memory region and starts the
// pump thread. Safe to call before the engine has actually created the
// region (we'll just fail silently and the source will stay black until
// a later call succeeds).
static void OpenShareSharedMemory(ZsSourceData* data) {
    if (!data || g_enginePid == 0) return;

    std::lock_guard<std::mutex> lifecycleLock(data->lifecycleMutex);

    if (data->mapping) return;  // already open

    std::string name = feeds_shared::MakeScreenShareRegionName(g_enginePid);

    data->mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, name.c_str());
    if (!data->mapping) {
        // Not yet created on engine side — that's fine. We'll try again
        // next time share_status_changed arrives with a non-zero sharer.
        return;
    }

    data->view = MapViewOfFile(data->mapping, FILE_MAP_READ, 0, 0,
                               feeds_shared::REGION_SIZE);
    if (!data->view) {
        CloseHandle(data->mapping);
        data->mapping = nullptr;
        return;
    }

    data->header = (feeds_shared::SharedFrameHeader*)data->view;

    // Defensive magic check — if the region exists but has the wrong
    // magic, something is off. Don't trust it.
    if (data->header->magic != feeds_shared::REGION_MAGIC) {
        UnmapViewOfFile(data->view);
        data->view = nullptr;
        CloseHandle(data->mapping);
        data->mapping = nullptr;
        data->header = nullptr;
        blog(LOG_WARNING,
             "[feeds] screenshare shared memory has bad magic, ignoring");
        return;
    }

    data->frameSlots = (feeds_shared::FrameSlot*)
        ((uint8_t*)data->view + sizeof(feeds_shared::SharedFrameHeader));

    // Start reading from the most recent frame — don't replay stale
    // frames that were written before this source opened the region.
    data->lastReadIndex = data->header->write_index;

    blog(LOG_INFO, "[feeds] opened screenshare shared memory '%s'",
         name.c_str());

    ZsStartPumpThread(data);
}

// clearTexture: if true, call obs_source_output_video(nullptr) to clear
// any lingering frame. Same reasoning as CloseSharedMemory — skip during
// destruction to avoid touching a half-torn-down source.
static void CloseShareSharedMemory(ZsSourceData* data, bool clearTexture = true) {
    if (!data) return;

    std::lock_guard<std::mutex> lifecycleLock(data->lifecycleMutex);

    ZsStopPumpThread(data);

    if (data->view)    { UnmapViewOfFile(data->view); data->view = nullptr; }
    if (data->mapping) { CloseHandle(data->mapping); data->mapping = nullptr; }
    data->header = nullptr;
    data->frameSlots  = nullptr;
    data->lastReadIndex = 0;

    if (clearTexture && data->source) {
        obs_source_output_video(data->source, nullptr);
    }
}

// Open (or no-op) the shared memory on every live screenshare source.
// Called when share_status_changed arrives with a non-zero sharer — at
// that point the engine has definitely created the region.
static void OpenSharedMemoryForAllScreenshareSources() {
    // Eligibility gate: never open a region when the user can't use
    // screenshare (logged out, or below Basic tier). tier_disabled normally
    // encodes this, but it defaults to false until the first reconcile after
    // login — so a source created while logged out would otherwise reach for
    // a region the engine hasn't created.
    if (!g_isLoggedIn || g_currentTier < 1) return;

    std::lock_guard<std::mutex> lock(g_screenshareSourcesMutex);
    for (ZsSourceData* s : g_allScreenshareSources) {
        // Skip tier-disabled sources — share_status_changed firing
        // mid-meeting shouldn't re-open mappings that reconciliation
        // closed for over-tier sources.
        if (s && !s->mapping && !s->tier_disabled) OpenShareSharedMemory(s);
    }
}

// Close the shared memory on every live screenshare source. Called when
// share ends — we could leave the mappings open, but closing frees the
// pump threads and any paused frame in OBS.
static void CloseSharedMemoryForAllScreenshareSources() {
    std::lock_guard<std::mutex> lock(g_screenshareSourcesMutex);
    for (ZsSourceData* s : g_allScreenshareSources) {
        if (s) CloseShareSharedMemory(s);
    }
}

static void* zs_create(obs_data_t* settings, obs_source_t* source) {
    (void)settings;

    // Same exception-boundary reasoning as zp_create.
  try {
    // A screenshare source must ALWAYS be created. Returning nullptr from a
    // create callback makes OBS keep an invalid husk that OnSourceCreated
    // removes, and the next save bakes in the loss — so a screenshare source
    // used to vanish from a scene loaded while logged out or on the Free
    // tier. nullptr is reserved for genuine allocation/exception failures
    // (and the one-instance rule below). A source that can't show content
    // yet is created dormant: it sits blank, its properties panel explains
    // why (logged-out / upgrade branch), and ReconcileSourcesToTier re-tiers
    // it on login. The screenshare opens its own shared-memory region rather
    // than binding on a user selection, so the eligibility gate lives on the
    // open paths below instead.

    // One screenshare source per scene. Zoom only exposes a single
    // active sharer at a time, so a second source would render the
    // same stream. This is a genuine one-instance constraint, not an
    // external-state gate, so it still refuses (with its own dialog).
    bool alreadyHaveScreenshare;
    {
        std::lock_guard<std::mutex> lock(g_screenshareSourcesMutex);
        alreadyHaveScreenshare = !g_allScreenshareSources.empty();
    }
    if (alreadyHaveScreenshare) {
        if (ShouldShowTierPopup()) {
            ShowTierLimitDialog(
                "Feeds: Screenshare Source Already Exists",
                "Feeds only supports one screenshare source. "
                "You can create copies of that source to place "
                "it in multiple locations.");
        }
        return nullptr;
    }

    obs_source_set_async_unbuffered(source, true);

    ZsSourceData* data = new ZsSourceData();
    data->source = source;

    {
        std::lock_guard<std::mutex> lock(g_screenshareSourcesMutex);
        g_allScreenshareSources.push_back(data);
    }

    // Open the mapping immediately only when eligible (logged in, Basic+
    // tier) AND already in a meeting with an active share. The eligibility
    // check matters now that creation is never refused: without it a Free or
    // logged-out source created mid-share would reach for a region the engine
    // hasn't authorized. Otherwise we wait for share_status_changed, whose
    // open path is eligibility-gated too.
    if (g_isLoggedIn && g_currentTier >= 1 &&
        g_isInMeeting && g_rawLiveStreamGranted && g_activeSharerUserId != 0) {
        OpenShareSharedMemory(data);
    }

    return data;
  } catch (const std::exception& e) {
    blog(LOG_ERROR, "[feeds] zs_create exception: %s", e.what());
    return nullptr;
  } catch (...) {
    blog(LOG_ERROR, "[feeds] zs_create unknown exception");
    return nullptr;
  }
}

static void zs_destroy(void* vdata) {
    if (!vdata) return;
    ZsSourceData* data = static_cast<ZsSourceData*>(vdata);

    try {
        // Erase from the global registry FIRST — same reasoning as
        // zp_destroy. Prevents share_status_changed (and any other handler
        // that iterates g_allScreenshareSources) from finding this source
        // mid-destruction. Per-source lifecycleMutex inside
        // CloseShareSharedMemory serialises any handler that already
        // captured the pointer.
        {
            std::lock_guard<std::mutex> lock(g_screenshareSourcesMutex);
            auto it = std::find(g_allScreenshareSources.begin(),
                                g_allScreenshareSources.end(), data);
            if (it != g_allScreenshareSources.end())
                g_allScreenshareSources.erase(it);
        }

        // clearTexture=false: source is being torn down, don't touch it.
        CloseShareSharedMemory(data, false);
    } catch (const std::exception& e) {
        blog(LOG_ERROR, "[feeds] zs_destroy exception: %s", e.what());
    } catch (...) {
        blog(LOG_ERROR, "[feeds] zs_destroy unknown exception");
    }
    delete data;
}

// Source became visible (added to preview/program — the eye-toggle "showing"
// transition, NOT program-only "active"). Open the visibility gate, then request
// a one-shot redeliver of the newest ring frame so re-show paints Zoom's current
// content instead of OBS's stale async texture. Order matters: set showing FIRST
// so the pump's gate is already open when it services the redeliver wake;
// otherwise the gate would swallow it. Fires on the graphics/tick thread. The
// wake is best-effort — the pump also polls every 8ms, so the flags are
// authoritative even if pumpWakeEvent isn't up yet (source shown before a share
// started). Screenshare only; participants get no show/hide callbacks.
static void zs_show(void* vdata) {
    if (!vdata) return;
    ZsSourceData* data = static_cast<ZsSourceData*>(vdata);
    data->showing.store(true, std::memory_order_release);
    data->redeliverPending.store(true, std::memory_order_release);
    if (data->pumpWakeEvent) SetEvent(data->pumpWakeEvent);
}

// Source became hidden. Close the visibility gate FIRST, then clear the async
// output — ordering so a pump delivery can't race in between the two and re-arm
// async_active after we've cleared it. obs_source_output_video(NULL) deactivates
// the async output (nothing paints) so no stale frame can show in the residual
// window before the next redeliver. Same NULL-clear pattern the pump teardown
// uses. Fires on the graphics/tick thread.
static void zs_hide(void* vdata) {
    if (!vdata) return;
    ZsSourceData* data = static_cast<ZsSourceData*>(vdata);
    data->showing.store(false, std::memory_order_release);
    if (data->source) obs_source_output_video(data->source, nullptr);
}

static obs_properties_t* zs_properties(void* data) {
  // Same exception-boundary reasoning as zp_properties.
  try {
    // data can be nullptr in some Qt code paths; null-guard the
    // tier_disabled access.
    ZsSourceData* d = static_cast<ZsSourceData*>(data);
    if (d && d->tier_disabled) {
        obs_properties_t* props = obs_properties_create();
        std::string verLabel = std::string("Feeds - Screenshare (v") +
                               feeds_shared::VERSION + ")";
        obs_properties_add_text(props, "ver_label", verLabel.c_str(),
                                OBS_TEXT_INFO);
        obs_properties_add_text(props, "tier_disabled_msg",
            "Screenshare is a paid feature. Your current Feeds tier "
            "is Free.",
            OBS_TEXT_INFO);
        obs_properties_add_button(props, "upgrade_btn",
            "Upgrade your plan to activate screenshare",
            [](obs_properties_t*, obs_property_t*, void*) -> bool {
                QDesktopServices::openUrl(
                    QUrl("https://letsdovideo.com/feeds-upgrade"));
                return true;
            });
        return props;
    }

    obs_properties_t* props = obs_properties_create();
    std::string verLabel = std::string("Feeds - Screenshare (v") + feeds_shared::VERSION + ")";
    obs_properties_add_text(props, "ver_label", verLabel.c_str(), OBS_TEXT_INFO);

    if (!g_isInMeeting) {
        if (!g_isLoggedIn) {
            obs_properties_add_button(props, "login_btn",
                "Not logged in to Zoom. Click to Login.",
                [](obs_properties_t*, obs_property_t*, void*) -> bool {
                    OnLoginClick();
                    return true;
                });
        } else {
            obs_properties_add_button(props, "connect_btn",
                "Logged in. Click to Connect to Zoom Meeting.",
                [](obs_properties_t*, obs_property_t*, void*) -> bool {
                    OnConnectClick();
                    return true;
                });
        }
    } else {
        // Privilege state takes precedence over the share-status text.
        // PrivilegeStatusText returns empty when Granted so the normal
        // share-status text renders.
        std::string status_text = PrivilegeStatusText();
        if (status_text.empty()) {
            if (g_activeSharerUserId == 0) {
                status_text = "Status: Connected - waiting for screenshare";
            } else if (g_cachedMyUserId != 0 &&
                       g_activeSharerUserId == g_cachedMyUserId) {
                // The Feeds user is sharing their own screen. Not blocked —
                // it works fine and is sometimes useful (different encoder
                // path than OBS display capture, testing workflow, etc.) —
                // but we flag it because OBS's own Display Capture source
                // is usually lower-latency for one's own screen.
                status_text =
                    "Status: Receiving screenshare (your own)\n"
                    "Tip: OBS Display Capture may give lower latency for "
                    "your own screen.";
            } else {
                status_text = "Status: Receiving screenshare";
            }
        }
        obs_properties_add_text(props, "status_label", status_text.c_str(),
                                OBS_TEXT_INFO);
    }

    return props;
  } catch (const std::exception& e) {
    blog(LOG_ERROR, "[feeds] zs_properties exception: %s", e.what());
    return obs_properties_create();
  } catch (...) {
    blog(LOG_ERROR, "[feeds] zs_properties unknown exception");
    return obs_properties_create();
  }
}

// ---------------------------------------------------------------------------
// Source info
// ---------------------------------------------------------------------------
struct obs_source_info zoom_participant_info = {};
struct obs_source_info zoom_screenshare_info = {};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("feeds", "en-US")

// ---------------------------------------------------------------------------
// Tier reconciliation
// ---------------------------------------------------------------------------
// Walks both source vectors and applies tier rules. Called on every
// login_succeeded after g_currentTier is updated.
//
// The common scenario isn't an actual downgrade — it's a Broadcaster
// who built an OBS scene with N participant sources and a screenshare
// source, then shared the OBS configuration with a teammate who logs
// into Feeds at a lower tier. Same scene, different user, different
// rules. That's why this runs on every login rather than only on
// detected tier changes, and why tier_disabled is recomputed each
// time rather than persisted.
//
// Rules:
//   Participant: the first GetMaxFeedsForTier() sources (creation
//   order, which is vector order) stay active. The rest become
//   tier-disabled.
//   Screenshare: all sources become tier-disabled iff tier == 0.
//
// On false → true (newly disabled): unsubscribe via IPC, close
// shared memory, reset current_user_id — leaving the source in the
// same "fresh" state as just after construction.
//
// On true → false (re-enabled by upgrade): no engine state to set
// up. The user picks a participant via the dropdown when they want
// it back; that re-triggers the normal zp_update subscribe path.
static void ReconcileSourcesToTier() {
    int maxFeeds = GetMaxFeedsForTier();

    {
        std::lock_guard<std::mutex> lock(g_sourcesMutex);
        int idx = 0;
        for (ZpSourceData* s : g_allParticipantSources) {
            if (!s) continue;
            bool shouldDisable = (idx >= maxFeeds);
            ++idx;
            if (shouldDisable && !s->tier_disabled) {
                if (!s->uuid.empty()) {
                    std::string msg =
                        "{\"type\":\"participant_source_unsubscribe\","
                        "\"source_id\":\"" + s->uuid + "\"}";
                    feeds::SendToEngine(msg);
                }
                s->current_user_id = 0;
                CloseSharedMemory(s);
            }
            s->tier_disabled   = shouldDisable;
            s->source_position = idx;

            // Forward the new tier to the ISO recorder; it stops any active
            // recording if the tier drops below Basic.
            feeds::feeds_iso_recorder_set_tier(s->iso, g_currentTier);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_screenshareSourcesMutex);
        bool shouldDisable = (g_currentTier == 0);
        for (ZsSourceData* s : g_allScreenshareSources) {
            if (!s) continue;
            // Screenshare has no per-source IPC subscription — the
            // engine streams one shared memory region to whoever
            // wants it. Disabling is just "stop reading."
            if (shouldDisable && !s->tier_disabled) {
                CloseShareSharedMemory(s);
            }
            s->tier_disabled = shouldDisable;
        }
    }

    // Chat popup sources — gated at Streamer (>= 2). Walks the source's
    // own instance registry under its own mutex; no shared state with
    // the participant/screenshare reconciliation above.
    feeds::ReconcileChatPopupSources();

    // Chat overlay sources — same threshold and pattern as the popup.
    feeds::ReconcileChatOverlaySources();

    // Chat dock — gated at Basic (>= 1). The dock object always exists
    // (registered at module-load); SetTierDisabled flips it between
    // normal and upgrade-prompt states.
    if (g_chatDock) {
        g_chatDock->SetTierDisabled(g_currentTier < 1);
    }

    // ISO recording menu + recorder state follow the tier: an upgrade
    // re-enables the menu and resumes the user's chosen toggle; a downgrade
    // greys the menu and stops any active recording (also enforced inside
    // the recorder via set_tier above, but we mirror it here for clarity).
    UpdateIsoMenuItemForTier();
    ApplyIsoRecordingStateToAllSources();

    RefreshAllSourceProperties();
}

// ---------------------------------------------------------------------------
// IPC message handlers
// ---------------------------------------------------------------------------
static void RegisterEngineHandlers() {
    feeds::RegisterMessageHandler("engine_ready", [](const std::string& json) {
        std::string version = ExtractJsonString(json, "version");
        g_enginePid = (uint32_t)ExtractJsonNumber(json, "pid");
        blog(LOG_INFO, "[feeds] engine_ready: version=%s, pid=%u",
             version.c_str(), g_enginePid);
    });

    // Engine log forwarding (Phase 1). The engine no longer writes its own
    // file — it sends each line as {"type":"log","level":"...","message":"..."}
    // and we re-emit it into the OBS log via blog(). The "[feeds-engine]"
    // prefix keeps these distinguishable from the plugin's own "[feeds]" lines.
    // message is escape-decoded; level maps to the matching OBS log level
    // (everything is "info" this phase — per-line levels come in a later one).
    feeds::RegisterMessageHandler("log", [](const std::string& json) {
        std::string level   = ExtractJsonString(json, "level");
        std::string message = ExtractJsonStringEscaped(json, "message");

        int obsLevel = LOG_INFO;
        if      (level == "debug")   obsLevel = LOG_DEBUG;
        else if (level == "warning") obsLevel = LOG_WARNING;
        else if (level == "error")   obsLevel = LOG_ERROR;
        // "info" (and anything unrecognized) falls through to LOG_INFO.

        blog(obsLevel, "[feeds-engine] %s", message.c_str());
    });

    feeds::RegisterMessageHandler("login_succeeded", [](const std::string& json) {
        g_userDisplayName = ExtractJsonString(json, "display_name");
        g_userPMI         = ExtractJsonString(json, "pmi");
        g_currentTier     = (int)ExtractJsonNumber(json, "tier");
        // NOTE: g_loginAttemptCompleted is intentionally NOT armed here.
        // It is set together with g_isLoggedIn in the sdk_authenticated
        // handler so the popup gate (g_loginAttemptCompleted && !g_isLoggedIn)
        // never sees a window where the gate is armed but g_isLoggedIn is
        // still false. See sdk_authenticated below.
        // Redaction: never log the user's name or PMI. The assignments above
        // stay — the UI still needs them — only the log text is sign-in
        // completion plus the non-sensitive tier.
        blog(LOG_INFO, "[feeds] login_succeeded: sign-in completed, tier=%d",
             g_currentTier);

        // Reconcile on the UI thread — ReconcileSourcesToTier touches
        // OBS source state and refreshes properties, both of which
        // belong on the main thread.
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            ReconcileSourcesToTier();
        });
    });

    feeds::RegisterMessageHandler("login_failed", [](const std::string& json) {
        std::string error = ExtractJsonString(json, "error");
        if (error.empty()) error = "unknown";
        g_loginAttemptCompleted = true;

        // "no_stored_token" is the engine's startup signal that there's
        // no saved credential to authenticate with — not an actual
        // failure. We still want g_loginAttemptCompleted=true (already
        // set above) so source-creation callbacks pop the "Please log
        // in" prompt, but suppress the error MessageBox and don't
        // re-enable the login menu item (it's already enabled in this
        // state). Logged as INFO rather than ERROR for the same reason.
        if (error == "no_stored_token") {
            blog(LOG_INFO, "[feeds] login_failed: no_stored_token (expected on first run / after logout)");
            // Defensive: this branch fires at engine startup before any
            // user click, so the flag should already be false. Clear it
            // anyway so a future code path that routes a user-initiated
            // login through this branch can't wedge the menu.
            QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
                g_authInProgress = false;
                UpdateLoginLogoutMenuItem();
            });
            return;
        }

        blog(LOG_ERROR, "[feeds] login_failed: %s", error.c_str());
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
            [error]() {
                // login_timeout is the engine's honest signal that the worker
                // poll never got the auth code (timeout / network failure),
                // most often an unreachable login server on a proxied or
                // firewalled managed network. Give a message that points at
                // that cause instead of the raw error slug. Every other error
                // keeps the generic "Login failed: <error>" form.
                std::string msg;
                if (error == "login_timeout") {
                    msg = "Feeds couldn't reach the login server. On managed "
                          "or corporate networks this can be caused by a proxy "
                          "or firewall. Please try again, or contact support "
                          "if it continues.\n\nNetwork requirements for IT: " +
                          std::string(kNetworkRequirementsUrl);
                } else {
                    msg = "Login failed: " + error;
                }
                QMessageBox::critical(
                    static_cast<QWidget*>(obs_frontend_get_main_window()),
                    QString::fromUtf8("Feeds - Login"),
                    QString::fromUtf8(msg.c_str()));
                g_authInProgress = false;
                UpdateLoginLogoutMenuItem();
                g_pendingMeetingJoin = false;
            });
    });

    // The engine resolved no tier and had no cached tier to fall back on, so
    // the user is sitting on Free without having chosen it. Every other tier
    // outcome is silent: a successful fetch is normal, and a failed fetch that
    // falls back to a cached tier worked as far as the user is concerned.
    // Warning (not critical) because Feeds is running — just limited.
    feeds::RegisterMessageHandler("tier_unreachable", [](const std::string&) {
        blog(LOG_WARNING, "[feeds] tier_unreachable: licensing server "
                          "unreachable and no cached tier — running as Free");
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            // Once per OBS session. The engine re-announces on every session
            // restore, so without this a user parked on a blocking network
            // would get this dialog on every launch.
            static bool shown = false;
            if (shown) return;
            shown = true;

            const std::string msg =
                "Feeds couldn't reach the licensing server, so it's running "
                "in Free mode. On managed or corporate networks this can be "
                "caused by a proxy or firewall.\n\nIf you have a paid plan, "
                "it will be restored automatically once Feeds can reach the "
                "server.\n\nNetwork requirements for IT: " +
                std::string(kNetworkRequirementsUrl);

            QMessageBox::warning(
                static_cast<QWidget*>(obs_frontend_get_main_window()),
                QString::fromUtf8("Feeds - Licensing"),
                QString::fromUtf8(msg.c_str()));
        });
    });

    // sdk_authenticated now means "credentials are valid and Feeds is ready to
    // connect" — NOT "the Zoom SDK is initialized and authenticated." As of the
    // idle-footprint change, the engine defers SDK init+auth until the first
    // connect, so at this point no SDK is up: the engine sends this purely to
    // flip the UI to logged-in and enable Connect. Clicking Connect is what
    // brings the SDK up (lazily, engine-side). The send sites are documented in
    // engine-sdk.cpp's AnnounceLoginSucceeded. Nothing here needs to change for
    // that semantic — enabling Connect on valid credentials is exactly right.
    feeds::RegisterMessageHandler("sdk_authenticated", [](const std::string&) {
        blog(LOG_INFO, "[feeds] sdk_authenticated");
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            // Arm the popup gate together with g_isLoggedIn, on the same
            // thread in the same step, so a source-creation popup check can
            // never observe g_loginAttemptCompleted=true while g_isLoggedIn
            // is still false (the false "Please log in" race).
            g_loginAttemptCompleted = true;
            g_isLoggedIn = true;
            g_authInProgress = false;
            UpdateLoginLogoutMenuItem();
            if (g_connectAction) g_connectAction->setEnabled(true);
            RefreshAllSourceProperties();
            if (g_chatDock) g_chatDock->RefreshPlaceholder();

            // "Connect to Zoom on Startup": the first time we reach an
            // authenticated state this OBS session, auto-open the join dialog
            // if the user enabled the setting. This point — g_isLoggedIn just
            // set true on the UI thread — implies the engine is connected and
            // login succeeded. The one-shot guard ensures a mid-session
            // logout/login can't re-pop it. Skipped when a user-initiated join
            // is already pending (handled just below). Deferred with
            // singleShot(0) so the dialog opens cleanly after this handler
            // returns rather than reentrantly inside it.
            if (!g_startupConnectDone) {
                g_startupConnectDone = true;
                if (g_connectOnStartupEnabled && !g_pendingMeetingJoin) {
                    QTimer::singleShot(0, []() { OnConnectClick(); });
                }
            }

            if (g_pendingMeetingJoin) {
                g_pendingMeetingJoin = false;
                QTimer::singleShot(500, []() { OnConnectClick(); });
            }
        });
    });

    feeds::RegisterMessageHandler("sdk_auth_failed", [](const std::string& json) {
        blog(LOG_ERROR, "[feeds] sdk_auth_failed: %s", json.c_str());
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            QMessageBox::critical(
                static_cast<QWidget*>(obs_frontend_get_main_window()),
                QString::fromUtf8("Feeds - Auth Failed"),
                QString::fromUtf8(
                    "Zoom authentication failed. Please try logging in again."));
            g_authInProgress = false;
            UpdateLoginLogoutMenuItem();
            g_pendingMeetingJoin = false;
        });
    });

    feeds::RegisterMessageHandler("logout_complete", [](const std::string&) {
        blog(LOG_INFO, "[feeds] logout_complete");

        {
            std::lock_guard<std::mutex> lock(g_sourcesMutex);
            for (ZpSourceData* s : g_allParticipantSources) {
                CloseSharedMemory(s);
                // Runtime IDs are dead and the engine tore down subscriptions.
                // Clear current_user_id too so no stale id can be re-subscribed
                // on the next login/meeting (auto-bind fix).
                if (s) {
                    s->bound_this_session = false;
                    s->current_user_id    = 0;
                    s->subscribed_user_id = 0;
                }
            }
        }
        CloseSharedMemoryForAllScreenshareSources();

        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            g_isLoggedIn           = false;
            g_isInMeeting          = false;
            g_rawLiveStreamGranted = false;
            g_rawPrivilegeState    = RawPrivilegeState::NotRequested;
            g_currentMeetingNumber = 0;
            g_activeSharerUserId   = 0;
            g_activeSpeakerUserId  = 0;
            g_connQualityByUserId.clear();
            g_muteByUserId.clear();   // present-participant set -> empty when out of a meeting
            g_cachedMyUserId       = 0;
            g_userDisplayName.clear();
            g_userPMI.clear();
            g_currentTier = 0;
            {
                std::lock_guard<std::mutex> lock(g_participantsMutex);
                g_cachedParticipants.clear();
            }

            g_authInProgress = false;
            UpdateLoginLogoutMenuItem();
            if (g_connectAction) g_connectAction->setEnabled(false);

            // Reset the global ISO toggle so the next login starts in a
            // known state (the menu item also greys out via the tier
            // update, but we clear the checked state to match).
            g_isoRecordingEnabled = false;
            if (g_isoRecordingAction) {
                QSignalBlocker blocker(g_isoRecordingAction);
                g_isoRecordingAction->setChecked(false);
            }
            UpdateIsoMenuItemForTier();
            ApplyIsoRecordingStateToAllSources();

            RefreshAllSourceProperties();
            // Clear any stale tier-disabled state from the prior login.
            // Without this, logging out from a Free account leaves
            // m_tierDisabled == true and the dock shows the "paid
            // feature" upgrade prompt instead of the logged-out
            // placeholder (CurrentPlaceholderText checks tierDisabled
            // before login state).
            if (g_chatDock) {
                g_chatDock->SetTierDisabled(false);
                g_chatDock->RefreshPlaceholder();
            }

            QMessageBox::information(
                static_cast<QWidget*>(obs_frontend_get_main_window()),
                QString::fromUtf8("Feeds - Logout"),
                QString::fromUtf8("You have been logged out of Zoom."));
        });
    });

    feeds::RegisterMessageHandler("session_expired", [](const std::string&) {
        blog(LOG_WARNING, "[feeds] session_expired");
        {
            std::lock_guard<std::mutex> lock(g_sourcesMutex);
            for (ZpSourceData* s : g_allParticipantSources) {
                CloseSharedMemory(s);
                // Runtime IDs are dead and the engine tore down subscriptions.
                // Clear current_user_id too so no stale id can be re-subscribed
                // on the next login/meeting (auto-bind fix).
                if (s) {
                    s->bound_this_session = false;
                    s->current_user_id    = 0;
                    s->subscribed_user_id = 0;
                }
            }
        }
        CloseSharedMemoryForAllScreenshareSources();
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            g_isLoggedIn = false;
            g_isInMeeting = false;
            g_rawLiveStreamGranted = false;
            g_rawPrivilegeState    = RawPrivilegeState::NotRequested;
            g_userDisplayName.clear();
            g_userPMI.clear();
            g_currentTier = 0;
            {
                std::lock_guard<std::mutex> lock(g_participantsMutex);
                g_cachedParticipants.clear();
            }
            g_authInProgress = false;
            UpdateLoginLogoutMenuItem();
            if (g_connectAction) g_connectAction->setEnabled(false);

            // Same reset as logout_complete: clear the global ISO toggle
            // and refresh the menu so the user starts fresh on next login.
            g_isoRecordingEnabled = false;
            if (g_isoRecordingAction) {
                QSignalBlocker blocker(g_isoRecordingAction);
                g_isoRecordingAction->setChecked(false);
            }
            UpdateIsoMenuItemForTier();
            ApplyIsoRecordingStateToAllSources();

            RefreshAllSourceProperties();
            // Clear stale tier-disabled state — same gap as
            // logout_complete: a Free-tier session expiry would
            // otherwise leave the dock stuck on the upgrade prompt.
            if (g_chatDock) {
                g_chatDock->SetTierDisabled(false);
                g_chatDock->RefreshPlaceholder();
            }

            QMessageBox::warning(
                static_cast<QWidget*>(obs_frontend_get_main_window()),
                QString::fromUtf8("Feeds - Session Expired"),
                QString::fromUtf8(
                    "Your Zoom login has expired and could not be renewed.\n\n"
                    "Please log in again."));
        });
    });

    feeds::RegisterMessageHandler("meeting_joined", [](const std::string& json) {
        std::string mn = ExtractJsonString(json, "meeting_number");
        // Redaction: the meeting number is the user's PMI for a PMI join, so
        // keep it out of the normal log — DEBUG only.
        blog(LOG_DEBUG, "[feeds] meeting_joined: %s", mn.c_str());
        try { g_currentMeetingNumber = std::stoull(mn); }
        catch (...) { g_currentMeetingNumber = 0; }

        // Engine attaches join_url + password to meeting_joined ONLY for
        // the instant-meeting Start() flow (regular Join paths omit them).
        // Combined with the plugin-side g_pendingInstantMeeting flag, this
        // tells us whether to show the share-this-meeting popup.
        bool wasInstant = g_pendingInstantMeeting.exchange(false);
        std::string joinUrl  = ExtractJsonString(json, "join_url");
        std::string password = ExtractJsonString(json, "password");
        unsigned long long meetingNum = g_currentMeetingNumber;

        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
            [wasInstant, meetingNum, joinUrl, password]() {
                g_isInMeeting = true;
                if (g_connectAction) g_connectAction->setEnabled(false);
                RefreshAllSourceProperties();
                if (g_chatDock) g_chatDock->OnMeetingJoined();

                if (!wasInstant || meetingNum == 0) return;

                // Share-this-meeting popup: modal, self-cleaning. Gives
                // the user the number + password + join URL with a
                // one-click copy so they can share with viewers.
                QMainWindow* main =
                    (QMainWindow*)obs_frontend_get_main_window();
                QDialog* dlg = new QDialog(main);
                dlg->setAttribute(Qt::WA_DeleteOnClose);
                dlg->setWindowTitle("Instant Meeting Started");
                dlg->setWindowModality(Qt::ApplicationModal);

                QString qJoinUrl  = QString::fromStdString(joinUrl);
                QString qPassword = QString::fromStdString(password);

                QLabel* numLabel = new QLabel(
                    "<b>Meeting Number:</b> " +
                    QString::number(meetingNum), dlg);
                numLabel->setTextFormat(Qt::RichText);
                numLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

                QLabel* pwdLabel = nullptr;
                if (!qPassword.isEmpty()) {
                    pwdLabel = new QLabel(
                        "<b>Password:</b> " + qPassword, dlg);
                    pwdLabel->setTextFormat(Qt::RichText);
                    pwdLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
                }

                QLabel* urlLabel = nullptr;
                if (!qJoinUrl.isEmpty()) {
                    urlLabel = new QLabel(
                        "<b>Join URL:</b> <a href=\"" + qJoinUrl + "\">" +
                        qJoinUrl + "</a>", dlg);
                    urlLabel->setTextFormat(Qt::RichText);
                    urlLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
                    urlLabel->setOpenExternalLinks(true);
                }

                QPushButton* copyBtn = new QPushButton("Copy Link", dlg);
                copyBtn->setEnabled(!qJoinUrl.isEmpty());
                QObject::connect(copyBtn, &QPushButton::clicked, dlg,
                    [copyBtn, qJoinUrl]() {
                        QApplication::clipboard()->setText(qJoinUrl);
                        copyBtn->setText("Copied!");
                    });
                QPushButton* okBtn = new QPushButton("OK", dlg);
                okBtn->setDefault(true);
                QObject::connect(okBtn, &QPushButton::clicked,
                                 dlg, &QDialog::accept);

                QHBoxLayout* btnRow = new QHBoxLayout();
                btnRow->addWidget(copyBtn);
                btnRow->addStretch();
                btnRow->addWidget(okBtn);

                QVBoxLayout* layout = new QVBoxLayout(dlg);
                layout->addWidget(numLabel);
                if (pwdLabel) layout->addWidget(pwdLabel);
                if (urlLabel) layout->addWidget(urlLabel);
                layout->addSpacing(8);
                layout->addLayout(btnRow);

                dlg->show();
            });
    });

    feeds::RegisterMessageHandler("meeting_failed", [](const std::string& json) {
        int code         = (int)ExtractJsonNumber(json, "code");
        std::string msg  = ExtractJsonString(json, "message");
        blog(LOG_ERROR, "[feeds] meeting_failed: code=%d, msg=%s",
             code, msg.c_str());

        // Defensive: instant flow set the flag; meeting_joined would have
        // cleared it on success, so on failure we need to.
        g_pendingInstantMeeting = false;

        if (msg.empty())
            msg = "Failed to join meeting. Error code: " + std::to_string(code);

        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
            [msg]() {
                if (g_connectAction && g_isLoggedIn)
                    g_connectAction->setEnabled(true);
                QMessageBox::critical(
                    static_cast<QWidget*>(obs_frontend_get_main_window()),
                    QString::fromUtf8("Feeds - Join Failed"),
                    QString::fromUtf8(msg.c_str()));
            });
    });

    // ---- Zoom Events flow ----
    feeds::RegisterMessageHandler("events_list", [](const std::string& json) {
        // Ignore unless a "Zoom Events" click is awaiting this reply.
        if (!g_eventsListPending.exchange(false)) return;
        std::string payload = json;
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
            [payload]() { ShowEventPickerDialog(payload); });
    });

    feeds::RegisterMessageHandler("sessions_list", [](const std::string& json) {
        if (!g_eventsSessionsPending.exchange(false)) return;
        std::string payload = json;
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
            [payload]() { ShowSessionPickerDialog(payload); });
    });

    feeds::RegisterMessageHandler("events_auth_required", [](const std::string&) {
        // The user authorized before the Zoom Events scopes existed. Don't show
        // a raw error — tell them to re-consent by logging out and back in.
        g_eventsListPending     = false;
        g_eventsSessionsPending = false;
        blog(LOG_WARNING, "[feeds] events_auth_required — Events scope not granted");
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            QMessageBox::information(
                static_cast<QWidget*>(obs_frontend_get_main_window()),
                QString::fromUtf8("Feeds - Zoom Events"),
                QString::fromUtf8(
                    "Feeds needs to be re-authorized to access your Zoom Events.\n\n"
                    "Open the Feeds menu, log out of Zoom, then log back in to "
                    "grant access — and try Zoom Events again."));
        });
    });

    feeds::RegisterMessageHandler("meeting_left", [](const std::string&) {
        blog(LOG_INFO, "[feeds] meeting_left");
        // Defensive — already cleared in meeting_joined / meeting_failed.
        g_pendingInstantMeeting = false;

        // Engine has torn down shared memory. Close our mappings to
        // keep things clean. Safe on this thread — no OBS API calls.
        {
            std::lock_guard<std::mutex> lock(g_sourcesMutex);
            for (ZpSourceData* s : g_allParticipantSources) {
                CloseSharedMemory(s);
                // The meeting's runtime user IDs are now dead. Drop the
                // session-confirmed flag so the next join re-binds purely by
                // remembered name (and a reused ID can't masquerade as a live
                // binding). Also clear current_user_id: the runtime id is dead,
                // and leaving it set is what let the grant sweep re-subscribe a
                // stale id to whoever now holds it (the auto-bind bug). The
                // remembered name in obs_data is untouched, so a legitimate
                // rejoin still re-binds by display name. The engine tore down
                // the subscriptions, so clear the subscribed marker too.
                if (s) {
                    s->bound_this_session = false;
                    s->current_user_id    = 0;
                    s->subscribed_user_id = 0;
                }
            }
        }
        CloseSharedMemoryForAllScreenshareSources();

        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            g_isInMeeting          = false;
            g_rawLiveStreamGranted = false;
            g_rawPrivilegeState    = RawPrivilegeState::NotRequested;
            g_currentMeetingNumber = 0;
            g_activeSharerUserId   = 0;
            g_activeSpeakerUserId  = 0;
            g_connQualityByUserId.clear();
            g_muteByUserId.clear();   // present-participant set -> empty when out of a meeting
            g_cachedMyUserId       = 0;
            {
                std::lock_guard<std::mutex> lock(g_participantsMutex);
                g_cachedParticipants.clear();
            }
            if (g_connectAction && g_isLoggedIn)
                g_connectAction->setEnabled(true);
            RefreshAllSourceProperties();
            if (g_chatDock) g_chatDock->OnMeetingLeft();
        });
    });

    feeds::RegisterMessageHandler("raw_livestream_granted", [](const std::string&) {
        blog(LOG_INFO, "[feeds] raw_livestream_granted");
        g_rawLiveStreamGranted = true;
        g_rawPrivilegeState    = RawPrivilegeState::Granted;

        // Privilege now exists — this is the reliable single place that
        // subscribes every currently-bound source, regardless of whether the
        // binding came from a manual dropdown pick or from name-based reconcile
        // (including [Active Speaker], sentinel 1).
        //
        // Engine truth on (re)entry: the engine holds NO participant
        // subscriptions at this point — they are torn down on leave
        // (TearDownAllVideoSubscriptions), and a fresh process has none. So
        // clear every subscribed_user_id FIRST, making the engine's state
        // authoritative and decoupling us from leave-side teardown ordering.
        // This closes the leave/rejoin seam: when a returning participant reused
        // its old runtime id, reconcile would keep the binding (current_user_id
        // unchanged) while a stale subscribed_user_id still equalled it, so
        // SubscribeBoundSourceLocked suppressed the re-subscribe and the source
        // stayed black even though the engine had dropped the subscription.
        //
        // After the clear, re-derive bindings from the live roster (reconcile,
        // now with privilege true, subscribes freshly-bound sources) and then
        // sweep every source through SubscribeBoundSourceLocked to catch those
        // bound before the grant. The guard now only prevents double-subscribing
        // within this single grant cycle (reconcile subscribes, the sweep skips).
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            {
                std::lock_guard<std::mutex> lock(g_sourcesMutex);
                for (ZpSourceData* s : g_allParticipantSources)
                    if (s) s->subscribed_user_id = 0;
            }
            // No join delta at grant (the sweep below subscribes everything);
            // pass an empty joinedIds so reconcile's Case A doesn't force-
            // recreate. The sweep's SubscribeBoundSourceLocked then sends
            // recreate for every bound source, sequenced by the engine gate.
            ReconcileRememberedParticipants({});
            {
                std::lock_guard<std::mutex> lock(g_sourcesMutex);
                blog(LOG_INFO, "[feeds] bind-decision: grant sweep — "
                     "(re)subscribing all bound sources after raw_livestream_granted");
                for (ZpSourceData* s : g_allParticipantSources)
                    SubscribeBoundSourceLocked(s);
            }
            RefreshAllSourceProperties();
        });
    });

    feeds::RegisterMessageHandler("raw_livestream_pending", [](const std::string&) {
        blog(LOG_INFO, "[feeds] raw_livestream_pending");
        g_rawPrivilegeState = RawPrivilegeState::Pending;
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            RefreshAllSourceProperties();
        });
    });

    feeds::RegisterMessageHandler("raw_livestream_denied", [](const std::string&) {
        // A Denied callback arriving when we're already Granted is a
        // mid-meeting host revoke. Continue processing — frame delivery
        // needs to stop either way and the engine has already cleared
        // its g_rawLiveStreamGranted flag.
        blog(LOG_WARNING, "[feeds] raw_livestream_denied");
        g_rawLiveStreamGranted = false;
        g_rawPrivilegeState    = RawPrivilegeState::Denied;
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            RefreshAllSourceProperties();
        });
    });

    feeds::RegisterMessageHandler("raw_livestream_timeout", [](const std::string&) {
        // Defence in depth — engine already suppresses the timeout when
        // g_rawLiveStreamGranted is true (the SDK has been observed
        // firing the timeout 10–25 s after a successful grant). Backstop.
        if (g_rawPrivilegeState == RawPrivilegeState::Granted) {
            blog(LOG_INFO, "[feeds] ignoring raw_livestream_timeout — already Granted");
            return;
        }
        // Post-timeout, the SDK fires no callbacks for host actions on
        // the popup — Grant still works via Changed(true), but Deny and
        // popup-close are invisible. Transition to TimedOut so the UI
        // ("Still waiting... please check with the host") is honest
        // about the uncertainty.
        blog(LOG_WARNING, "[feeds] raw_livestream_timeout");
        g_rawPrivilegeState = RawPrivilegeState::TimedOut;
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            RefreshAllSourceProperties();
        });
    });

    feeds::RegisterMessageHandler("participant_list_changed",
    [](const std::string& json) {
        unsigned int myUserId = (unsigned int)ExtractJsonNumber(json, "my_user_id");

        std::vector<CachedParticipant> newList;
        std::map<unsigned int, bool>   mutedSnap;  // userId -> muted, dock seed
        // String-aware array/object splitting: a display name can contain '[',
        // ']', '{' or '}' (e.g. "{TAG} Bob" or "Bob :]"), which a naive
        // find(']')/find('{')/find('}') scan would treat as structure —
        // truncating an object or the whole array and dropping every
        // participant after the offending name (breaking their name-rebind).
        // Use the same helpers the Events path uses (JsonExtractArrayBody +
        // SplitJsonObjects), which skip brackets/braces inside quoted strings.
        for (const std::string& obj :
             SplitJsonObjects(JsonExtractArrayBody(json, "participants"))) {
            CachedParticipant p;
            p.id   = (unsigned int)ExtractJsonNumber(obj, "id");
            p.name = ExtractJsonString(obj, "name");
            if (p.id != 0 && !p.name.empty()) {
                newList.push_back(p);
                mutedSnap[p.id] = ExtractJsonNumber(obj, "muted") != 0;
            }
        }

        bool changed = false;
        std::vector<unsigned int> joinedIds;   // ids present now but not before
        {
            std::lock_guard<std::mutex> lock(g_participantsMutex);
            if (myUserId != g_cachedMyUserId ||
                newList.size() != g_cachedParticipants.size()) {
                changed = true;
            } else {
                for (size_t i = 0; i < newList.size(); i++) {
                    if (newList[i].id != g_cachedParticipants[i].id ||
                        newList[i].name != g_cachedParticipants[i].name) {
                        changed = true;
                        break;
                    }
                }
            }
            // Detect (re)joins: any id present now that wasn't in the previous
            // roster. Passed to ReconcileRememberedParticipants, which force-
            // re-establishes a source already bound to a just-(re)joined id
            // (Case A). A rejoining participant — same id or new id — appears
            // here because the drop removed them from the prior roster first.
            for (const auto& np : newList) {
                bool wasPresent = false;
                for (const auto& op : g_cachedParticipants) {
                    if (op.id == np.id) { wasPresent = true; break; }
                }
                if (!wasPresent) joinedIds.push_back(np.id);
            }
            g_cachedMyUserId     = myUserId;
            g_cachedParticipants = std::move(newList);
        }

        if (!changed) return;

        blog(LOG_INFO, "[feeds] participant_list_changed: %zu participants",
             (size_t)g_cachedParticipants.size());

        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
            [joinedIds, mutedSnap = std::move(mutedSnap)]() {
                // Re-seed the UI-thread-owned mute map from this roster's fresh
                // muted fields (wholesale: drops departed users, reflects current
                // state) BEFORE the refresh so the dock rebuild seeds correct
                // mute marks. participant_audio_status keeps it live in between.
                g_muteByUserId = mutedSnap;
                // Auto-rebind participant-pinned sources to their remembered
                // name, then refresh properties. joinedIds drives the in-
                // reconcile rejoin re-establishment: a source already bound to
                // a just-(re)joined participant is force-recreated (Case A),
                // and a new-id rejoin / rename re-binds and recreates (Case B).
                // All re-establishment flows through SubscribeBoundSourceLocked
                // -> participant_source_recreate -> the engine's sequenced gate,
                // so multi-source rejoins are spaced, not bursted. Covers both a
                // participant joining and a participant renaming to a remembered
                // name (both arrive as participant_list_changed).
                ReconcileRememberedParticipants(joinedIds);
                RefreshAllSourceProperties();
            });
    });

    feeds::RegisterMessageHandler("chat_message",
    [](const std::string& json) {
        // Handler runs on the IPC reader thread; parse here, dispatch
        // UI mutation to the Qt main thread via QTimer::singleShot —
        // same pattern the participant_list_changed handler above uses.
        // Engine already filtered to public ("to everyone") messages,
        // so we just render whatever arrives. message_id is parsed for
        // logging only in Phase 1; future phases may use it for edit/
        // delete propagation or dedup.

        // Tier gating at the IPC entry point. Sinks (dock, popup,
        // overlay) gate themselves too, but cutting traffic here saves
        // the avatar fetch + overlay history mutation for Free users
        // and prevents stale messages from piling up in the overlay
        // history during a tier-locked session.
        //   Tier 0 (Free):       skip entirely
        //   Tier 1 (Basic):      dock-only (skip overlay append)
        //   Tier 2+ (Streamer+): full processing
        if (g_currentTier < 1) return;

        std::string messageId  = ExtractJsonString(json, "message_id");
        unsigned int senderId  =
            (unsigned int)ExtractJsonNumber(json, "sender_id");
        std::string senderName = ExtractJsonStringEscaped(json, "sender_name");
        std::string content    = ExtractJsonStringEscaped(json, "content");
        std::string avatarPath = ExtractJsonStringEscaped(json, "avatar_path");
        qint64      timestamp  = (qint64)ExtractJsonNumber(json, "timestamp");

        blog(LOG_INFO, "[feeds] chat_message id=%s", messageId.c_str());

        // Populate the avatar cache up-front on the IPC thread so the
        // chat popup source can hit the cache synchronously when
        // ToggleChatPopup runs. Return value unused here.
        (void)GetAvatarForSender(senderId, avatarPath);

        // Push into the overlay's centralised history (Streamer+ only), tagged
        // as Zoom so per-instance platform filters can include/exclude it.
        // Thread-safe; no need to marshal to the Qt main thread — the overlay
        // source reads under its own mutex on the graphics thread.
        if (g_currentTier >= 2) {
            feeds::AppendChatMessageToOverlay(
                feeds::ChatMsgOrigin::Zoom, senderId, std::string(),
                senderName, content, timestamp);
        }

        QString qSender  = QString::fromStdString(senderName);
        QString qContent = QString::fromStdString(content);

        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
            [senderId, qSender, qContent, timestamp]() {
                if (g_chatDock)
                    g_chatDock->AppendMessage(senderId, qSender, qContent,
                                              timestamp);
            });
    });

    feeds::RegisterMessageHandler("chat_send_result",
    [](const std::string& json) {
        // Success path: nothing to do — Zoom echoes our message back via
        // onChatMsgNotification and it renders through the normal chat
        // pipeline. Only surface failures, since the user otherwise has
        // no indication that the send didn't land.
        bool success =
            json.find("\"success\":true") != std::string::npos;
        if (success) return;

        std::string error = ExtractJsonStringEscaped(json, "error");
        if (error.empty()) error = "Unknown error";
        QString qError = QString::fromStdString(error);

        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
            [qError]() {
                QMainWindow* main =
                    (QMainWindow*)obs_frontend_get_main_window();
                QMessageBox::warning(main, "Feeds — Send Failed", qError);
            });
    });

    // Active speaker changed. Marshal the write onto the UI thread (dock-as-
    // context) so g_activeSpeakerUserId is fully UI-thread-owned, then recompute
    // mute marks: an Active-Speaker dock row (pid 1) re-resolves to the new
    // speaker's mute state. Runs on the pipe-reader thread here.
    feeds::RegisterMessageHandler("active_speaker_changed",
    [](const std::string& json) {
        if (!g_participantDock) return;   // only the dock reads g_activeSpeakerUserId
        unsigned int userId = (unsigned int)ExtractJsonNumber(json, "participant_id");
        QTimer::singleShot(0, g_participantDock, [userId]() {
            g_activeSpeakerUserId = userId;
            if (g_participantDock) g_participantDock->RecomputeMuteMarks();
        });
    });

    // Per-user mute change from the engine's audio listener. Marshal to the UI
    // thread (dock-as-context, auto-cancels if the dock is gone): update the
    // UI-thread-owned mute map, then recompute mute marks in place — no Refresh(),
    // no box rebuild. A pid==1 row picks this up when userId is the active
    // speaker; direct rows update by pid. Runs on the pipe-reader thread here.
    feeds::RegisterMessageHandler("participant_audio_status",
    [](const std::string& json) {
        unsigned int userId = (unsigned int)ExtractJsonNumber(json, "participant_id");
        if (userId == 0 || !g_participantDock) return;
        bool muted = ExtractJsonNumber(json, "muted") != 0;
        QTimer::singleShot(0, g_participantDock, [userId, muted]() {
            g_muteByUserId[userId] = muted;
            if (g_participantDock) g_participantDock->RecomputeMuteMarks();
        });
    });

    // Per-user connection quality from the engine's onUserNetworkStatusChanged.
    // Store the leg into the UI-thread-owned g_connQualityByUserId; the dock's
    // poll resolves it into the dot on its next tick (no direct dot update here —
    // one update path). component: 1 Audio, 2 Video (0 Def / 3 Share dropped —
    // not dot-useful). level: ConnectionQuality 0..6.
    feeds::RegisterMessageHandler("participant_conn_quality",
    [](const std::string& json) {
        unsigned int userId = (unsigned int)ExtractJsonNumber(json, "participant_id");
        if (userId == 0 || !g_participantDock) return;
        int component = (int)ExtractJsonNumber(json, "component");
        int uplink    = (int)ExtractJsonNumber(json, "uplink");
        int level     = (int)ExtractJsonNumber(json, "level");
        QTimer::singleShot(0, g_participantDock, [userId, component, uplink, level]() {
            ConnQualityLegs& q = g_connQualityByUserId[userId];
            if (component == 2)      { if (uplink) q.videoUp = level; else q.videoDown = level; }
            else if (component == 1) { if (uplink) q.audioUp = level; else q.audioDown = level; }
            // Def(0)/Share(3) dropped — the dot only uses video/audio legs.
        });
    });

    feeds::RegisterMessageHandler("share_status_changed",
    [](const std::string& json) {
        unsigned int newSharer =
            (unsigned int)ExtractJsonNumber(json, "sharer_user_id");
        g_activeSharerUserId = newSharer;

        // On share start: open shared memory on all screenshare sources
        // so they start pumping frames. On share end: close so the pump
        // threads exit and the sources clear their last frame.
        if (newSharer != 0) {
            OpenSharedMemoryForAllScreenshareSources();
        } else {
            CloseSharedMemoryForAllScreenshareSources();
        }

        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            RefreshAllSourceProperties();
        });
    });

    feeds::RegisterMessageHandler("token_refreshed", [](const std::string&) {
        blog(LOG_INFO, "[feeds] token_refreshed");
    });

    // ---- Video frame path ----

    feeds::RegisterMessageHandler("source_texture_ready",
    [](const std::string& json) {
        std::string sourceId = ExtractJsonString(json, "source_id");
        if (sourceId.empty()) return;

        blog(LOG_INFO, "[feeds] source_texture_ready: source=%s",
             sourceId.c_str());

        std::lock_guard<std::mutex> lock(g_sourcesMutex);
        ZpSourceData* s = FindSourceByUuid(sourceId);
        if (s) OpenSharedMemory(s);
    });

    feeds::RegisterMessageHandler("source_texture_released",
    [](const std::string& json) {
        std::string sourceId = ExtractJsonString(json, "source_id");
        if (sourceId.empty()) return;

        blog(LOG_INFO, "[feeds] source_texture_released: source=%s",
             sourceId.c_str());

        std::lock_guard<std::mutex> lock(g_sourcesMutex);
        ZpSourceData* s = FindSourceByUuid(sourceId);
        if (s) CloseSharedMemory(s);
    });

    feeds::RegisterMessageHandler("participant_source_subscribe_failed",
    [](const std::string& json) {
        std::string sourceId = ExtractJsonString(json, "source_id");
        if (sourceId.empty()) return;

        // The engine's readiness-gated renderer creation gave up for this
        // source (retries exhausted, or a non-retryable error). Clear our
        // subscribed marker so SubscribeBoundSourceLocked isn't wedged into
        // believing the source is still subscribed — a later grant /
        // participant_list_changed, or a manual reselect, can then retry it.
        // (The remembered participant_name / current_user_id binding is left
        // intact; only the subscribe-state guard is reset.)
        blog(LOG_WARNING,
             "[feeds] participant_source_subscribe_failed: source=%s "
             "(clearing subscribed marker so it can retry)", sourceId.c_str());

        std::lock_guard<std::mutex> lock(g_sourcesMutex);
        ZpSourceData* s = FindSourceByUuid(sourceId);
        if (s) s->subscribed_user_id = 0;
    });
}

// Fit-center geometry for one participant scene-item: Automatic bounds (no bbox,
// so Alt-drag crops naturally), center alignment, aspect-preserving fit scale to
// the canvas (the geometry OBS_BOUNDS_SCALE_INNER yields), centered position. The
// single definition shared by ApplyParticipantPlacement's initial-placement enum
// and the dock's add-reference action. No-op if the source has no real
// dimensions yet (0x0).
static void ApplyFitCenterGeometry(obs_sceneitem_t* item, obs_source_t* source) {
    if (!item || !source) return;
    const uint32_t srcW = obs_source_get_width(source);
    const uint32_t srcH = obs_source_get_height(source);
    if (srcW == 0 || srcH == 0) return;

    obs_video_info ovi;
    uint32_t canvasW = 1920, canvasH = 1080;
    if (obs_get_video_info(&ovi)) { canvasW = ovi.base_width; canvasH = ovi.base_height; }

    const float scaleW = (float)canvasW / (float)srcW;
    const float scaleH = (float)canvasH / (float)srcH;
    const float fit    = (scaleW < scaleH) ? scaleW : scaleH;

    vec2 scaleVec; scaleVec.x = fit; scaleVec.y = fit;
    vec2 pos;      pos.x = (float)canvasW * 0.5f; pos.y = (float)canvasH * 0.5f;

    obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_NONE);
    obs_sceneitem_set_alignment(item, OBS_ALIGN_CENTER);
    obs_sceneitem_set_scale(item, &scaleVec);
    obs_sceneitem_set_pos(item, &pos);
}

// Initial placement for participant video and screenshare — both use Automatic
// bounds (OBS_BOUNDS_NONE) so the user can crop naturally with Alt-drag, rather
// than the old Fit (SCALE_INNER) box. Participant video was moved to Automatic in
// v1.4.0 (the engine scaler already held its size constant); screenshare joins the
// same path now that its frames are likewise scaled to a constant per-tier size in
// the engine (see engine-screenshare.cpp's FrameScalerWorker). With a constant
// output size, Fit is no longer needed to absorb Zoom's resolution/content-size
// changes, so OnSourceCreated routes both types here (chat overlay/popup keep
// their own placement helpers).
//
// NOTE: if the engine scaler is ever reverted for screenshare (see the text-
// quality gate), this Automatic placement still applies to screenshare on the
// plugin side — the two changes are independent. Screenshare would then be
// variable-size under Automatic, which is acceptable but less stable than with
// the scaler holding its size constant.
//
// To preserve today's "drop it in and it fills the screen" default we set
// an initial scale that aspect-fits the source inside the canvas (the same
// geometry SCALE_INNER produced) and center it. Automatic needs the
// source's real pixel dimensions to compute that scale, but a freshly
// created participant source reports 0x0 until its first frame arrives, so
// we poll for non-zero dimensions before applying (bounded so a camera-off
// participant doesn't spin a thread forever).
//
// One-shot: a sentinel in the source's settings records that we've placed
// it, so reloading a saved scene does NOT stomp a crop/position the user
// set earlier. obs_data_has_user_value is false on fresh creation and true
// for a source restored from a saved scene collection (the sentinel is
// serialized with it), same trick ApplyChatOverlayDefaults uses for width.
static void ApplyParticipantPlacement(obs_source_t* source) {
    if (!source) return;

    // Skip if we've already placed this source in a previous session — the
    // user's saved scene-item geometry (including any crop) wins on reload.
    {
        obs_data_t* settings = obs_source_get_settings(source);
        bool alreadyPlaced = settings &&
            obs_data_has_user_value(settings, "feeds_initial_placement_done");
        if (settings) obs_data_release(settings);
        if (alreadyPlaced) return;
    }

    // Wait for the source to report real dimensions (first frame). The
    // engine scaler guarantees these are constant once they appear, so a
    // one-shot placement is correct and survives Zoom resolution drops.
    // Bounded to ~10s (100 × 100ms): comfortably covers the first frame and
    // a "turn the camera on after adding the source" delay, but a source
    // that never produces frames just stays at OBS defaults rather than
    // leaking a polling thread.
    uint32_t srcW = 0;
    uint32_t srcH = 0;
    for (int i = 0; i < 100; ++i) {
        srcW = obs_source_get_width(source);
        srcH = obs_source_get_height(source);
        if (srcW > 0 && srcH > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (srcW == 0 || srcH == 0) return;

    // Apply the shared fit-center geometry to every scene-item backed by this
    // source (the add-source flow adds exactly one, but a source already
    // referenced into several scenes gets all of them placed consistently).
    auto enum_cb = [](void* param, obs_source_t* scene_src) -> bool {
        obs_source_t* target = (obs_source_t*)param;
        obs_scene_t* scene = obs_scene_from_source(scene_src);
        if (!scene) return true;
        auto item_cb = [](obs_scene_t*, obs_sceneitem_t* item, void* p) -> bool {
            obs_source_t* target = (obs_source_t*)p;
            if (obs_sceneitem_get_source(item) == target)
                ApplyFitCenterGeometry(item, target);
            return true;
        };
        obs_scene_enum_items(scene, item_cb, target);
        return true;
    };
    obs_enum_scenes(enum_cb, source);

    // Record that we've done the initial placement so future reloads don't
    // override the user's adjustments. Written after the geometry is set.
    obs_data_t* settings = obs_source_get_settings(source);
    if (settings) {
        obs_data_set_bool(settings, "feeds_initial_placement_done", true);
        obs_source_update(source, settings);
        obs_data_release(settings);
    }
}

// Resolve the scene the Source dock edits: preview scene in Studio Mode, current
// scene otherwise. Do NOT use obs_frontend_get_current_scene alone — it returns
// the PROGRAM scene in Studio Mode, which would drop sources onto live output.
// Returns a ref the caller must release, or null (no scenes / edge cases).
static obs_source_t* ResolveCurrentEditScene() {
    obs_source_t* s = obs_frontend_get_current_preview_scene();  // studio: preview; else null
    if (!s) s = obs_frontend_get_current_scene();                // normal: current
    return s;
}

// Action A: create a new Feeds source of `typeId` and add it to the current edit
// scene, exactly as the OBS add-source flow does — obs_source_create fires the
// type's create callback (dock row via PostParticipantDockRefresh for participant
// sources) and the source_create signal (OnSourceCreated's deferred, per-type
// placement, which finds the scene-item we add here). No resolvable scene -> safe
// no-op (don't leak an orphan source). This is the always-new path — used by
// "Create Participant Feed" and the header's Chat Overlay button. UI thread.
static void CreateSourceOfTypeInCurrentScene(const char* typeId, const char* baseName) {
    obs_source_t* sceneSrc = ResolveCurrentEditScene();
    if (!sceneSrc) return;
    obs_scene_t* scene = obs_scene_from_source(sceneSrc);   // borrowed
    if (!scene) { obs_source_release(sceneSrc); return; }

    // Unique name, OBS's scheme: the display name, then " 2", " 3", ... until free
    // (obs_source_create does not dedupe).
    std::string name = baseName;
    for (int i = 2; ; ++i) {
        obs_source_t* existing = obs_get_source_by_name(name.c_str());
        if (!existing) break;
        obs_source_release(existing);
        name = std::string(baseName) + " " + std::to_string(i);
    }

    obs_source_t* source = obs_source_create(typeId, name.c_str(), nullptr, nullptr);
    if (source) {
        // Add promptly so the ~100ms-deferred placement thread finds a scene-item.
        obs_scene_add(scene, source);   // scene-item takes its own ref
        obs_source_release(source);     // release our create ref
    }
    obs_source_release(sceneSrc);
}

static void CreateParticipantSourceInCurrentScene() {
    CreateSourceOfTypeInCurrentScene("zoom_participant_source", "Feeds Participant");
}

// Find the first live (non-removed) source of the given type, returning an owned
// ref (caller releases) or null. Skips OnSourceCreated "husks" (removed flag set)
// and any source mid-destroy (obs_source_get_ref returns null). Used by the
// reference-or-create header buttons to decide reference vs. create. UI thread.
static obs_source_t* FindFirstSourceOfType(const char* typeId) {
    struct Ctx { const char* id; obs_source_t* found; } ctx{ typeId, nullptr };
    obs_enum_sources([](void* p, obs_source_t* s) -> bool {
        Ctx* c = (Ctx*)p;
        if (obs_source_removed(s)) return true;          // skip husks / pending-remove
        const char* sid = obs_source_get_id(s);
        if (sid && strcmp(sid, c->id) == 0) {
            c->found = obs_source_get_ref(s);            // strong ref (null if invalid)
            if (c->found) return false;                  // stop at first valid match
        }
        return true;
    }, &ctx);
    return ctx.found;
}

// Reference-or-create: if a source of `typeId` already exists, add a Paste
// Reference of it to the current edit scene (Action B semantics); otherwise
// create one (Action A). This is the screenshare / chat-popup header path: it
// NEVER creates a second instance of a type that already exists, so the
// screenshare singleton block in zs_create is never reached. UI thread.
static void AddOrReferenceSourceInCurrentScene(const char* typeId, const char* baseName,
                                               RefAddPlacement placement) {
    obs_source_t* existing = FindFirstSourceOfType(typeId);
    if (existing) {
        obs_source_t* sceneSrc = ResolveCurrentEditScene();
        if (sceneSrc) {
            obs_scene_t* scene = obs_scene_from_source(sceneSrc);   // borrowed
            if (scene) {
                obs_sceneitem_t* item = obs_scene_add(scene, existing);
                // Place the newly-added item per the type's default — the
                // reference path fires no source_create, so without this a
                // second+ add lands at OBS's raw default. Screenshare fills the
                // canvas (fit-center; a dormant 0x0 source safely skips it);
                // the popup sits bottom-center at natural size (matching what
                // ApplyChatPopupDefaultPosition gives the first, created instance).
                if (placement == RefAddPlacement::PopupDefault)
                    ApplyChatPopupPlacementToItem(item);
                else
                    ApplyFitCenterGeometry(item, existing);
            }
            obs_source_release(sceneSrc);
        }
        obs_source_release(existing);
        return;
    }
    CreateSourceOfTypeInCurrentScene(typeId, baseName);
}

// Action B: add an existing source to the current edit scene as a Paste Reference
// — a new scene-item pointing at the SAME source (no new source, no new dock row),
// repeatable. NOT obs_source_duplicate (that is Paste Duplicate). A fresh
// reference lands at OBS default (top-left, native), so apply fit-center to the
// new item directly; the source is already live, so its dimensions are known
// (no polling). No resolvable scene -> safe no-op. UI thread.
static void AddSourceReferenceToCurrentScene(const std::string& uuid) {
    obs_source_t* source = obs_get_source_by_uuid(uuid.c_str());
    if (!source) return;
    obs_source_t* sceneSrc = ResolveCurrentEditScene();
    if (!sceneSrc) { obs_source_release(source); return; }
    obs_scene_t* scene = obs_scene_from_source(sceneSrc);   // borrowed
    if (scene) {
        obs_sceneitem_t* item = obs_scene_add(scene, source);  // borrowed (scene owns)
        ApplyFitCenterGeometry(item, source);
    }
    obs_source_release(sceneSrc);
    obs_source_release(source);
}

// Open OBS's native Filters dialog for a source — the same window as right-
// clicking the source in the Sources dock. obs_frontend_open_source_filters is a
// confirmed frontend export that marshals to the UI thread itself and holds its
// own ref, so we just resolve -> call -> release our resolve ref. UI thread.
static void OpenSourceFilters(const std::string& uuid) {
    obs_source_t* src = obs_get_source_by_uuid(uuid.c_str());
    if (!src) return;
    obs_frontend_open_source_filters(src);
    obs_source_release(src);
}

// Pop a menu of the scenes this source appears in; clicking one switches to it
// (modeled on QAU's "parent scenes"). Each scene is listed once — a source
// referenced several times in one scene still yields a single entry (we stop at
// the first matching item per scene, and OBS scene names are unique). The switch
// is Studio-Mode-aware: preview in Studio Mode (never program/live), current
// scene otherwise — matching how add-to-scene resolves its target. All three
// obs_frontend_* calls are confirmed exports. Built fresh each click, so it
// always reflects the current scene set. UI thread (button click).
static void ShowIncludedScenesMenu(const std::string& uuid) {
    obs_source_t* src = obs_get_source_by_uuid(uuid.c_str());
    if (!src) return;

    struct EnumCtx { obs_source_t* target; std::vector<std::string> scenes; };
    EnumCtx ctx{ src, {} };
    obs_enum_scenes(
        [](void* param, obs_source_t* scene_src) -> bool {
            EnumCtx* c = (EnumCtx*)param;
            obs_scene_t* scene = obs_scene_from_source(scene_src);
            if (!scene) return true;
            struct ItemCtx { obs_source_t* target; bool found; } ic{ c->target, false };
            obs_scene_enum_items(scene,
                [](obs_scene_t*, obs_sceneitem_t* item, void* p) -> bool {
                    ItemCtx* i = (ItemCtx*)p;
                    if (obs_sceneitem_get_source(item) == i->target) {
                        i->found = true;
                        return false;   // stop: this scene counts once
                    }
                    return true;
                }, &ic);
            if (ic.found) {
                const char* nm = obs_source_get_name(scene_src);
                if (nm && *nm) c->scenes.emplace_back(nm);
            }
            return true;
        }, &ctx);
    obs_source_release(src);

    QMenu menu;
    if (ctx.scenes.empty()) {
        QAction* none = menu.addAction("No scenes");
        none->setEnabled(false);
    } else {
        for (const std::string& name : ctx.scenes) {
            QAction* act = menu.addAction(QString::fromStdString(name));
            QObject::connect(act, &QAction::triggered, [name]() {
                obs_source_t* scene = obs_get_source_by_name(name.c_str());
                if (obs_frontend_preview_program_mode_active())
                    obs_frontend_set_current_preview_scene(scene);  // studio: preview only
                else
                    obs_frontend_set_current_scene(scene);
                if (scene) obs_source_release(scene);
            });
        }
    }
    menu.exec(QCursor::pos());
}

// Place a newly-created popup source at bottom-center of the canvas with
// a 50px margin from the bottom edge. Chat overlays conventionally live
// there; the (0,0) default that OBS gives new sources is wrong for ~all
// users.
//
// Pairs the position with bottom-center alignment so taller popups (long
// messages wrap and the source's height grows) extend upward from the
// anchor rather than pushing the bubble's bottom edge offscreen. With
// top-left alignment the bbox grows downward and a long message would
// trail past the canvas edge; with bottom-center it grows upward and the
// 50px gap from the bottom stays constant.
//
// Sets position + alignment rather than bounds, since the popup is meant to
// keep its natural size, not fit the canvas.
// Place one popup scene-item at the canonical default: bottom-center, 4% of
// canvas height off the bottom edge, with bottom-center alignment so a taller
// (wrapped) bubble grows upward and the bottom gap stays constant. Natural size
// (position + alignment only, no bounds). Shared by the deferred create hook
// (ApplyChatPopupDefaultPosition) and the reference-add path — the item is in
// hand in both, so no source-dimension arithmetic is needed.
static void ApplyChatPopupPlacementToItem(obs_sceneitem_t* item) {
    if (!item) return;

    obs_video_info ovi;
    uint32_t canvasW = 1920;
    uint32_t canvasH = 1080;
    if (obs_get_video_info(&ovi)) {
        canvasW = ovi.base_width;
        canvasH = ovi.base_height;
    }

    const int marginFromBottom = (int)((float)canvasH * 0.04f);
    vec2 pos;
    pos.x = (float)canvasW * 0.5f;
    pos.y = (float)canvasH - (float)marginFromBottom;

    // OBS_ALIGN_CENTER is the zero value (horizontal centre is the default when
    // neither LEFT nor RIGHT is set); OR'ing it in is self-documentation.
    obs_sceneitem_set_pos(item, &pos);
    obs_sceneitem_set_alignment(item, OBS_ALIGN_BOTTOM | OBS_ALIGN_CENTER);
}

static void ApplyChatPopupDefaultPosition(obs_source_t* source) {
    if (!source) return;

    // Deferred create hook: place every scene-item currently backed by this
    // source. (The add flow adds one at a time, but a source referenced into
    // several scenes gets them all placed consistently.)
    auto enum_cb = [](void* param, obs_source_t* scene_src) -> bool {
        obs_source_t* target = (obs_source_t*)param;
        obs_scene_t* scene = obs_scene_from_source(scene_src);
        if (!scene) return true;

        auto item_cb = [](obs_scene_t*, obs_sceneitem_t* item, void* p) -> bool {
            obs_source_t* target = (obs_source_t*)p;
            if (obs_sceneitem_get_source(item) == target)
                ApplyChatPopupPlacementToItem(item);
            return true;
        };

        obs_scene_enum_items(scene, item_cb, target);
        return true;
    };

    obs_enum_scenes(enum_cb, source);
}

// Apply the overlay's per-canvas defaults: a Width setting (35% of canvas
// width) on the source itself, plus position (top-right, 2% × 5% inset)
// and alignment on the scene-item. The Width default is only written if
// the user hasn't already set a value — so a saved scene's width survives
// reloads — but the position is written unconditionally (same pattern as
// the popup's positioning hook).
static void ApplyChatOverlayDefaults(obs_source_t* source) {
    if (!source) return;

    obs_video_info ovi;
    uint32_t canvasW = 1920;
    uint32_t canvasH = 1080;
    if (obs_get_video_info(&ovi)) {
        canvasW = ovi.base_width;
        canvasH = ovi.base_height;
    }

    // Per-canvas Width default. obs_data_has_user_value returns false for
    // fresh creations (only get_defaults values are present) and true for
    // sources loaded from a saved scene — so we only write the canvas-
    // relative default in the fresh case, preserving user-set widths
    // across OBS restarts.
    obs_data_t* settings = obs_source_get_settings(source);
    if (settings) {
        if (!obs_data_has_user_value(settings, "width")) {
            const int defaultW = (int)((float)canvasW * 0.35f);
            obs_data_set_int(settings, "width", defaultW);
            obs_source_update(source, settings);
        }
        obs_data_release(settings);
    }

    const int marginFromRight = (int)((float)canvasW * 0.02f);
    const int marginFromTop   = (int)((float)canvasH * 0.05f);

    vec2 pos;
    pos.x = (float)canvasW - (float)marginFromRight;
    pos.y = (float)marginFromTop;

    struct SearchContext {
        obs_source_t* target;
        vec2          pos;
    };
    SearchContext ctx = { source, pos };

    auto enum_cb = [](void* param, obs_source_t* scene_src) -> bool {
        SearchContext* c = (SearchContext*)param;
        obs_scene_t* scene = obs_scene_from_source(scene_src);
        if (!scene) return true;

        auto item_cb = [](obs_scene_t*, obs_sceneitem_t* item, void* p) -> bool {
            SearchContext* c = (SearchContext*)p;
            obs_source_t* item_src = obs_sceneitem_get_source(item);
            if (item_src == c->target) {
                obs_sceneitem_set_pos(item, &c->pos);
                obs_sceneitem_set_alignment(
                    item, OBS_ALIGN_TOP | OBS_ALIGN_RIGHT);
            }
            return true;
        };

        obs_scene_enum_items(scene, item_cb, c);
        return true;
    };

    obs_enum_scenes(enum_cb, &ctx);
}

static void OnSourceCreated(void* /*data*/, calldata_t* cd) {
    obs_source_t* source = (obs_source_t*)calldata_ptr(cd, "source");
    if (!source) return;

    const char* id = obs_source_get_id(source);
    if (!id) return;

    // Only apply to Feeds source types. Participant video and screenshare share
    // the same Automatic-bounds placement (the engine scaler holds both at a
    // constant per-tier size); the chat overlay/popup types have their own.
    const bool isParticipant = strcmp(id, "zoom_participant_source") == 0;
    const bool isScreenshare = strcmp(id, "zoom_screenshare_source") == 0;
    const bool isChatPopup   = strcmp(id, "feeds_chat_popup")   == 0;
    const bool isChatOverlay = strcmp(id, "feeds_chat_overlay") == 0;
    if (!isParticipant && !isScreenshare && !isChatPopup && !isChatOverlay)
        return;

    // When our create callback returns NULL on a tier block, OBS keeps
    // the obs_source_t alive with context.data == NULL ("husk"). Mark
    // it removed synchronously: a deferred remove loses the race with
    // OBS auto-save, which serializes the husk into the scene JSON
    // before our cleanup runs. obs_source_remove just flips the
    // removed flag and fires the remove signal, which is safe to call
    // from inside source_create — the source isn't in any scene yet,
    // and the frontend's remove handler posts a Qt event rather than
    // touching the source synchronously.
    if (obs_obj_invalid(source)) {
        obs_source_remove(source);
        return;
    }

    // Hard cap: at most kMaxParticipantSourcesEver (8) participant sources may
    // ever exist. 8 is the product maximum at any tier, so a 9th could never be
    // activated by anyone (unlike a lower tier's over-cap sources 6-8, which are
    // a genuine upgrade incentive). Block interactive creation of a 9th — OBS
    // Sources "+", paste, duplicate — by removing it the instant it's created:
    // source_create fires before the source is added to any scene, so
    // obs_source_remove drops it cleanly, the same mechanism the husk path above
    // uses. zp_create already pushed it, so the count includes this new source.
    //
    // Skip while a scene collection is loading: a legacy scene saved with >8
    // participant sources must NOT be pruned (auto-save would bake in the loss);
    // those load as dormant/over-cap and surface the ceiling messaging instead.
    // The dock's Create button is already hidden at the tier cap, so this is the
    // backstop for the OBS-native paths it can't gate.
    if (isParticipant && !g_sceneCollectionLoading.load()) {
        size_t count;
        {
            std::lock_guard<std::mutex> lock(g_sourcesMutex);
            count = g_allParticipantSources.size();
        }
        if (count > (size_t)kMaxParticipantSourcesEver) {
            if (ShouldShowTierPopup()) {
                ShowTierLimitDialog(
                    "Feeds: Participant feed limit",
                    QString("%1 is the maximum supported number of participant "
                            "feeds, so this source can't be added.")
                        .arg(kMaxParticipantSourcesEver));
            }
            obs_source_remove(source);
            return;
        }
    }

    // Hold a weak reference; promote to strong reference inside the
    // deferred callback. This avoids holding the source alive if the user
    // (or OBS during shutdown) removes it in the interim.
    obs_weak_source_t* weak = obs_source_get_weak_source(source);
    if (!weak) return;

    std::thread([weak, isChatPopup, isChatOverlay]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        obs_source_t* strong = obs_weak_source_get_source(weak);
        if (strong) {
            if (isChatPopup) {
                ApplyChatPopupDefaultPosition(strong);
            } else if (isChatOverlay) {
                ApplyChatOverlayDefaults(strong);
            } else {
                // Participant OR screenshare: Automatic bounds + fill-canvas-
                // centered initial placement (screenshare joined this path with
                // the engine scaler change; see ApplyParticipantPlacement). No
                // other Feeds type reaches here — the guard above filtered them.
                ApplyParticipantPlacement(strong);
            }
            obs_source_release(strong);
        }
        obs_weak_source_release(weak);
    }).detach();
}

// Global source_rename signal handler — the participant dock shows OBS source
// names, so a rename must rebuild it. Filtered to our participant source type
// (same id check OnSourceCreated uses). Fires on whatever thread renamed the
// source; PostParticipantDockRefresh marshals to the UI thread.
static void OnSourceRenamed(void* /*data*/, calldata_t* cd) {
    obs_source_t* source = (obs_source_t*)calldata_ptr(cd, "source");
    if (!source) return;
    const char* id = obs_source_get_id(source);
    if (!id || strcmp(id, "zoom_participant_source") != 0) return;
    PostParticipantDockRefresh();
}

// ---------------------------------------------------------------------------
// Module load/unload
// ---------------------------------------------------------------------------
bool obs_module_load(void) {
    zoom_participant_info.id             = "zoom_participant_source";
    zoom_participant_info.type           = OBS_SOURCE_TYPE_INPUT;
    zoom_participant_info.output_flags   = OBS_SOURCE_ASYNC_VIDEO;
    zoom_participant_info.get_name       = [](void*) { return "Feeds Participant"; };
    zoom_participant_info.create         = zp_create;
    zoom_participant_info.destroy        = zp_destroy;
    zoom_participant_info.get_properties = zp_properties;
    zoom_participant_info.update         = zp_update;
    zoom_participant_info.video_tick     = zp_video_tick;
    zoom_participant_info.icon_type      = OBS_ICON_TYPE_CAMERA;
    obs_register_source(&zoom_participant_info);

    zoom_screenshare_info.id             = "zoom_screenshare_source";
    zoom_screenshare_info.type           = OBS_SOURCE_TYPE_INPUT;
    zoom_screenshare_info.output_flags   = OBS_SOURCE_ASYNC_VIDEO;
    zoom_screenshare_info.get_name       = [](void*) { return "Feeds Screenshare"; };
    zoom_screenshare_info.create         = zs_create;
    zoom_screenshare_info.destroy        = zs_destroy;
    zoom_screenshare_info.get_properties = zs_properties;
    // Show/hide (not activate/deactivate — we want the eye-toggle "showing"
    // transition, not program-only) drive the stale-frame-on-reshow fix:
    // visibility-gate the pump + redeliver the newest ring frame on re-show.
    // Screenshare only; participants deliver continuously and show no symptom.
    zoom_screenshare_info.show           = zs_show;
    zoom_screenshare_info.hide           = zs_hide;
    zoom_screenshare_info.icon_type      = OBS_ICON_TYPE_DESKTOP_CAPTURE;
    obs_register_source(&zoom_screenshare_info);

    feeds::RegisterChatPopupSource();
    feeds::RegisterChatOverlaySource();

    // ISO recorder: registers the single frontend-event callback that fans
    // recording start/stop/pause out to every per-source recorder.
    feeds::feeds_iso_recorder_module_load();

    // Eagerly load the fallback avatar so the popup source renders the
    // Feeds logo on its first frame (rather than the grey null-circle).
    // The popup source bypasses GetAvatarForSender's lazy path; without
    // this, the first render before any chat message has no fallback.
    EnsureFallbackAvatarLoaded();

    signal_handler_t* sh = obs_get_signal_handler();
    if (sh) {
        signal_handler_connect(sh, "source_create", OnSourceCreated, nullptr);
        // Participant dock reacts to OBS source renames (create/destroy are
        // driven from zp_create/zp_destroy directly). Disconnected in unload.
        signal_handler_connect(sh, "source_rename", OnSourceRenamed, nullptr);
    }

    feeds::StartEngine();
    RegisterEngineHandlers();

    // Register the chat dock here, not in FINISHED_LOADING. OBSBasic::OBSInit
    // calls restoreState() before plugins finish loading, so a dock added
    // post-FINISHED_LOADING gets default state instead of the user's saved
    // visibility/position/floating settings. Registering at module-load
    // means the dock exists in the QMainWindow when restoreState runs, and
    // saved state applies the normal way. Three reference OBS plugins
    // (Countdown, AudioMonitor, DSK) all do dock registration here for the
    // same reason.
    SetupChatDock();
    SetupParticipantDock();

    // YouTube chat poller. Starts here (idle/gated) and picks up the persisted
    // enable+handle once SetupPluginMenu loads them at FINISHED_LOADING; runs for
    // the plugin's lifetime, engine-independent.
    YtStartPollerThread();

    // Twitch chat reader — same lifecycle: starts idle, picks up the persisted
    // channel at FINISHED_LOADING, runs for the plugin's lifetime.
    TwStartReaderThread();

    obs_frontend_add_event_callback([](enum obs_frontend_event event, void*) {
        switch (event) {
        // Gate the 9th-source hard block off during scene-collection loads so a
        // legacy scene with >8 participant sources isn't pruned on load.
        case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
            g_sceneCollectionLoading.store(true);
            break;
        case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
            g_sceneCollectionLoading.store(false);
            break;
        case OBS_FRONTEND_EVENT_FINISHED_LOADING:
            g_sceneCollectionLoading.store(false);   // startup load done
            SetupPluginMenu();
            QTimer::singleShot(5000,
                (QObject*)obs_frontend_get_main_window(),
                []() { CheckForUpdateAsync(); });
            break;
        // Accelerator (not a gate): when OBS starts streaming to YouTube, re-check
        // /live immediately instead of waiting out the slow bootstrap cadence.
        case OBS_FRONTEND_EVENT_STREAMING_STARTED:
            if (YtHasTarget() && IsStreamingToYouTube()) YtWakePoller();
            break;
        default:
            break;
        }
    }, nullptr);

    return true;
}

void obs_module_unload(void) {
    signal_handler_t* sh = obs_get_signal_handler();
    if (sh) {
        signal_handler_disconnect(sh, "source_create", OnSourceCreated, nullptr);
        // Stop posting dock refreshes during teardown. Combined with the
        // dock-as-context single-shot (auto-cancels on widget destruction) and
        // the g_participantDock null-check, this closes the shutdown-time
        // use-after-free window as OBS destroys every source in a burst.
        signal_handler_disconnect(sh, "source_rename", OnSourceRenamed, nullptr);
    }
    if (g_updateCheckThread.joinable()) g_updateCheckThread.join();

    // Stop the YouTube chat poller (signals exit, wakes it, joins). Worst case it
    // waits out one in-flight HTTP request (bounded by the WinHTTP timeouts).
    YtStopPollerThread();

    // Stop the Twitch chat reader (signals exit, closes the socket to abort a
    // blocked receive, joins).
    TwStopReaderThread();

    // Remove the ISO recorder's frontend callback and drain any leftovers
    // before the engine + Qt teardown below.
    feeds::feeds_iso_recorder_module_unload();

    feeds::StopEngine();

    {
        std::lock_guard<std::mutex> lock(g_avatarCacheMutex);
        g_avatarCache.clear();
        g_fallbackAvatar = QImage();
    }
}
