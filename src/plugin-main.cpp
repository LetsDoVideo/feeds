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
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstring>
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
#include <QPainter>
#include <QRegularExpression>
#include <QStyledItemDelegate>
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

void OnLoginClick();
void OnLogoutClick();
void OnConnectClick();
static void UpdateLoginLogoutMenuItem();
// Shared tier-upgrade prompt (defined far below, with the other tier chrome).
// ShowTierLimitDialog is the reusable modal upgrade dialog (rich-text label with a
// clickable upgrade link); ShouldShowTierPopup throttles it to one per few seconds
// so rapid clicks don't stack dialogs. Used by the screenshare/chat create paths
// and, now, the footer's locked-button clicks.
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
static void CreateParticipantSourceInCurrentScene();
static void AddSourceReferenceToCurrentScene(const std::string& uuid);
// Footer add-source actions (defined with the scene helpers, far below).
// CreateSourceOfTypeInCurrentScene mints a fresh source of the given type and
// adds it (always-new, like "+ Add Participant"); AddOrReferenceSourceInCurrentScene
// references an existing source of that type if one exists, else creates one
// (reference-or-create) — this is how the screenshare button never trips the
// zs_create singleton block by trying to create a second instance.
static void CreateSourceOfTypeInCurrentScene(const char* typeId, const char* baseName);
static void AddOrReferenceSourceInCurrentScene(const char* typeId, const char* baseName);
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
// JSON helpers
// ---------------------------------------------------------------------------
static std::string ExtractJsonString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + search.size() + 1);
    if (pos == std::string::npos) return "";
    pos++;
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

static long long ExtractJsonNumber(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos = json.find_first_of("-0123456789", pos + search.size());
    if (pos == std::string::npos) return 0;
    size_t end = json.find_first_not_of("-0123456789", pos);
    std::string numStr = json.substr(pos, end == std::string::npos
                                          ? std::string::npos : end - pos);
    try { return std::stoll(numStr); } catch (...) { return 0; }
}

// JSON string escaper for outgoing IPC. Mirrors the engine's JsonEscape
// — they need to agree on what counts as an escape so both sides can
// round-trip user-entered chat content (quotes, backslashes, newlines).
static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    sprintf_s(buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

// JSON-escape-aware string extractor. ExtractJsonString stops at the first
// quote without handling escapes, which truncates chat content containing
// quotes — extremely common in actual chat traffic. This walks the value
// and decodes the escape sequences emitted by the engine's JsonEscape: \",
// \\, \/, \b, \f, \n, \r, \t, and \uXXXX. \uXXXX is dropped silently —
// engine only emits it for control chars below 0x20, which aren't visible
// in QListWidget items anyway.
static std::string ExtractJsonStringEscaped(const std::string& json,
                                            const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();

    std::string out;
    out.reserve(json.size() - pos);
    while (pos < json.size()) {
        char c = json[pos];
        if (c == '"') break;
        if (c == '\\' && pos + 1 < json.size()) {
            char n = json[pos + 1];
            switch (n) {
                case '"': case '\\': case '/': out += n;   pos += 2; break;
                case 'b': out += '\b'; pos += 2; break;
                case 'f': out += '\f'; pos += 2; break;
                case 'n': out += '\n'; pos += 2; break;
                case 'r': out += '\r'; pos += 2; break;
                case 't': out += '\t'; pos += 2; break;
                case 'u': pos += 6; break;
                default:  out += n; pos += 2; break;
            }
        } else {
            out += c;
            pos += 1;
        }
    }
    return out;
}

// Return the text of the array value for `key` (from its '[' to the matching
// ']'), string- and bracket-aware. Used to pull the events[]/sessions[] arrays
// out of the Zoom Events IPC payloads. Mirrors the engine's JsonExtractArrayBody.
static std::string JsonExtractArrayBody(const std::string& json,
                                        const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find('[', pos + search.size());
    if (pos == std::string::npos) return "";

    int depth = 0; bool inStr = false; bool esc = false;
    for (size_t i = pos; i < json.size(); ++i) {
        char c = json[i];
        if (inStr) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') inStr = true;
        else if (c == '[') depth++;
        else if (c == ']') { if (--depth == 0) return json.substr(pos, i - pos + 1); }
    }
    return "";
}

// Split a JSON array body into its top-level object substrings ("{...}"),
// skipping nested braces and braces inside strings.
static std::vector<std::string> SplitJsonObjects(const std::string& arr) {
    std::vector<std::string> out;
    int depth = 0; bool inStr = false; bool esc = false; size_t start = 0;
    for (size_t i = 0; i < arr.size(); ++i) {
        char c = arr[i];
        if (inStr) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') inStr = true;
        else if (c == '{') { if (depth++ == 0) start = i; }
        else if (c == '}') { if (--depth == 0) out.push_back(arr.substr(start, i - start + 1)); }
    }
    return out;
}

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
    HINTERNET hSession = WinHttpOpen(
        L"Feeds-OBS-Plugin/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
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
    if (s->tier_disabled) return false;
    if (s->current_user_id < 1) return false;            // unbound (0)
    if (!(g_isInMeeting && g_rawLiveStreamGranted)) return false;
    if (s->subscribed_user_id == s->current_user_id) return false;  // already subscribed

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
        if (pid == 1) {
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
                if (justJoined) {
                    s->subscribed_user_id = 0;   // bypass already-subscribed guard
                    SubscribeBoundSourceLocked(s);
                }
                obs_data_release(settings);
                continue;
            }
            // Bound participant is absent (dropped) — fall through and try to
            // re-bind them by remembered name (handles the rejoin).
        }

        // Case B: unbound, or bound-but-absent. Auto-bind only on an exact
        // name match with exactly one present candidate.
        if (remembered.empty()) {
            obs_data_release(settings);
            continue;
        }
        auto cit = countByName.find(remembered);
        if (cit == countByName.end() || cit->second != 1) {
            obs_data_release(settings);  // no match, or duplicate names
            continue;
        }
        unsigned int newId = idByName[remembered];
        if (s->bound_this_session && newId == s->current_user_id) {
            obs_data_release(settings);  // already correctly bound
            continue;
        }

        // Bind: persist the runtime id (keeps the dropdown selection in sync
        // and survives same-session scene save), mark the binding live, and
        // subscribe. If privilege isn't granted yet (initial join sweep),
        // SubscribeBoundSourceLocked is a no-op and the raw_livestream_granted
        // handler subscribes this source once privilege exists.
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

        QString text = index.data(Qt::DisplayRole).toString();
        QTextLayout layout(text, option.font);
        layout.setTextOption(MakeTextOption());

        qreal y = LayoutLines(layout, width);
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

        QString text = index.data(Qt::DisplayRole).toString();
        QTextLayout layout(text, option.font);
        layout.setTextOption(MakeTextOption());

        LayoutLines(layout, option.rect.width());
        layout.draw(painter, option.rect.topLeft());

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

        m_input = new QLineEdit(this);
        m_input->setPlaceholderText("Send message to everyone...");
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
        layout->addWidget(m_list);
        layout->addLayout(inputRow);

        setMinimumSize(200, 300);
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
        m_list->addItem(item);
        m_list->scrollToBottom();
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
    }
    void OnMeetingLeft() {
        SetPlaceholder(CurrentPlaceholderText());
        m_input->setEnabled(false);
        m_sendBtn->setEnabled(false);
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
    }

    bool IsTierDisabled() const { return m_tierDisabled; }

private:
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

        // Placeholders carry no sender_id data. Use that as the signal to
        // route the click through the same login / connect flows the menu
        // and source properties already drive — gives the dock its own
        // call-to-action entry point.
        QVariant idVar = item->data(RoleSenderId);
        if (!idVar.isValid()) {
            // Tier-locked placeholder routes to the upgrade URL instead
            // of the login/connect flows. Free users can't use the dock
            // at all, so the only useful action is "go upgrade."
            if (m_tierDisabled) {
                QDesktopServices::openUrl(
                    QUrl("https://letsdovideo.com/feeds-upgrade"));
                return;
            }

            // Bail on the "Connected" placeholder — already in a meeting,
            // the dock click shouldn't re-enter the connect flow (which
            // would hit the defensive Already-Connected MessageBox in
            // OnConnectClick that's correct for menu/properties callers
            // but noise here). g_isInMeeting is the truthful state;
            // m_messagesStarted only flips on first chat message and
            // would miss the "joined, no chat yet" window.
            if (g_isInMeeting) return;

            if (!g_isLoggedIn) {
                OnLoginClick();
            } else {
                OnConnectClick();
            }
            return;
        }

        unsigned int senderId = idVar.toUInt();
        QString      sender   = item->data(RoleSenderName).toString();
        QString      content  = item->data(RoleContent).toString();

        feeds::ToggleChatPopup(senderId,
                               sender.toStdString(),
                               content.toStdString());
    }

    QString CurrentPlaceholderText() const {
        if (m_tierDisabled) {
            return "Zoom Chat is a paid feature. Click to upgrade your plan.";
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
    bool         m_messagesStarted  = false;
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

        QScrollArea* scroll = new QScrollArea(this);
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
        outer->addWidget(scroll, 1);   // takes the stretch; footer stays pinned below

        // Fixed footer chrome pinned to the true dock bottom — added to the OUTER
        // layout (below the scroll area), so it never scrolls with the source list
        // and never floats after the content with a gap. Built once here; its
        // per-button state (tier lock + screenshare live dot) is updated in place
        // by UpdateFooterState() on every Refresh(), so it rides the existing
        // tier/login/meeting/share refresh path with no separate mechanism.
        m_footer = BuildFooterBar();
        outer->addWidget(m_footer, 0);

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
        // Footer chrome first: it's persistent (outer layout, not m_root) and
        // independent of the inline-rename editor, so update it unconditionally —
        // even on the deferred-rename and empty-rows early returns below — so a
        // tier/login/share change always re-styles the footer.
        UpdateFooterState();

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
        //   not in a meeting            -> connect/login button
        //   in a meeting, granted, alone-> greyed "Waiting for participants..."
        //   live (other participant)    -> neither
        // The pre-grant window (in a meeting, not granted) shows neither.
        if (!inMeeting) {
            // Button independent of row content (renders above the empty-state
            // hint too). Split on login state only; no tier lock (this dock is
            // not tier-gated, so a Free user sees and uses it). Same labels/
            // actions as the properties dialog's buttons.
            QPushButton* btn = new QPushButton(
                loggedIn ? "Logged in. Click to Connect to Zoom Meeting."
                         : "Not logged in to Zoom. Click to Login.");
            if (loggedIn)
                QObject::connect(btn, &QPushButton::clicked, []() { OnConnectClick(); });
            else
                QObject::connect(btn, &QPushButton::clicked, []() { OnLoginClick(); });
            m_root->addWidget(btn);
        } else if (granted && othersPresent == 0) {
            // Connected but alone — one greyed waiting line at the top (not per
            // row). Same string and #7a7d80 styling as the properties dialog.
            QLabel* waiting = new QLabel("Waiting for participants...");
            waiting->setStyleSheet("QLabel { color: #7a7d80; }");
            waiting->setWordWrap(true);
            m_root->addWidget(waiting);
        }

        if (rows.empty()) {
            QLabel* empty = new QLabel(
                "No Zoom Participant sources yet.\n"
                "Click “+ Add Participant” below to create one.");
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

            // One shared, group-level upgrade prompt pinned at the bottom.
            QLabel* msg = new QLabel(
                QString("Your current plan allows %1 participant source%2. "
                        "Upgrade to activate more feeds.")
                    .arg(maxFeeds)
                    .arg(maxFeeds == 1 ? "" : "s"));
            msg->setWordWrap(true);
            m_root->addWidget(msg);

            QPushButton* btn =
                new QPushButton("Upgrade your plan to activate more feeds");
            // Same upgrade destination as the properties-dialog upgrade button.
            QObject::connect(btn, &QPushButton::clicked, []() {
                QDesktopServices::openUrl(
                    QUrl("https://letsdovideo.com/feeds-upgrade"));
            });
            m_root->addWidget(btn);
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
    void AppendCreateButton() {
        QPushButton* addBtn = new QPushButton("+ Add Participant");
        QObject::connect(addBtn, &QPushButton::clicked, []() {
            CreateParticipantSourceInCurrentScene();
        });
        m_root->addWidget(addBtn);
    }

    // --- Fixed footer bar: add-source buttons for the other three source types ---
    // A toolbar-style bar pinned to the dock bottom (built once, in the outer
    // layout), completing "all four Feeds source types addable from the dock".
    // Three equal-width buttons; each honors its type's multiplicity rule:
    //   Screenshare  — reference-or-create (hard singleton; zs_create refuses a 2nd)
    //   Chat Overlay — ALWAYS create new (per-scene width/rows/count differ)
    //   Chat Popup   — reference-or-create (no reason for duplicates)
    // Tier gates match the source types themselves: screenshare Basic+ (tier >= 1),
    // chat overlay/popup Streamer+ (tier >= 2). g_currentTier is the same value the
    // cap chrome reads, so the footer re-locks in the same refresh path.

    // Small colored status dot for the Screenshare button icon: green when someone
    // is sharing, grey when idle. Tones match the row connection dot.
    static QPixmap ShareDotPixmap(bool sharing) {
        const int sz = 10;
        qreal dpr = 1.0;
        if (QScreen* s = (qApp ? qApp->primaryScreen() : nullptr))
            dpr = s->devicePixelRatio();
        QPixmap pm((int)(sz * dpr), (int)(sz * dpr));
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(sharing ? QColor(64, 192, 96, 235)      // green: sharing
                           : QColor(150, 150, 150, 170));   // grey: idle
        p.drawEllipse(0, 0, sz, sz);
        p.end();
        return pm;
    }

    static QPushButton* MakeFooterButton(const QString& label, const QString& tip) {
        QPushButton* b = new QPushButton(label);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(tip);
        return b;
    }

    QWidget* BuildFooterBar() {
        QFrame* bar = new QFrame();
        bar->setObjectName("feedsFooterBar");
        // Toolbar chrome: a top divider + faint fill so it reads as part of the
        // dock frame, distinct from the scrolling rows above.
        bar->setStyleSheet(
            "QFrame#feedsFooterBar { border-top: 1px solid rgba(128,128,128,0.35);"
            " background: rgba(128,128,128,0.06); }");

        QHBoxLayout* l = new QHBoxLayout(bar);
        l->setContentsMargins(8, 6, 8, 6);
        l->setSpacing(6);

        m_footScreenshare = MakeFooterButton("Screenshare", QString());
        QObject::connect(m_footScreenshare, &QPushButton::clicked, []() {
            if (g_currentTier < 1) {   // tier-locked (Basic+): actionable upgrade prompt
                ShowFooterUpgradePrompt("Zoom Screenshare", "Basic");
                return;
            }
            AddOrReferenceSourceInCurrentScene("zoom_screenshare_source", "Zoom Screenshare");
        });

        m_footChatOverlay = MakeFooterButton("Chat Overlay", QString());
        QObject::connect(m_footChatOverlay, &QPushButton::clicked, []() {
            if (g_currentTier < 2) {   // tier-locked (Streamer+): actionable upgrade prompt
                ShowFooterUpgradePrompt("Zoom Chat Overlay", "Streamer");
                return;
            }
            CreateSourceOfTypeInCurrentScene("feeds_chat_overlay", "Zoom Chat Overlay");
        });

        m_footChatPopup = MakeFooterButton("Chat Popup", QString());
        QObject::connect(m_footChatPopup, &QPushButton::clicked, []() {
            if (g_currentTier < 2) {   // tier-locked (Streamer+): actionable upgrade prompt
                ShowFooterUpgradePrompt("Zoom Chat Popup", "Streamer");
                return;
            }
            AddOrReferenceSourceInCurrentScene("feeds_chat_popup", "Zoom Chat Popup");
        });

        l->addWidget(m_footScreenshare, 1);   // equal thirds
        l->addWidget(m_footChatOverlay, 1);
        l->addWidget(m_footChatPopup, 1);
        return bar;
    }

    // Style one footer button for its tier-lock state. Kept ENABLED even when
    // locked: a disabled Qt widget receives no hover events, so its tooltip
    // wouldn't show — and the upgrade hint is the discoverability point. Locked =
    // greyed text + arrow cursor + upgrade tooltip; the click lambda short-circuits
    // the add on the tier check and instead opens the upgrade prompt
    // (ShowFooterUpgradePrompt), so it never performs the source action.
    static void ApplyFooterLock(QPushButton* b, bool locked, const QString& tip) {
        if (!b) return;
        b->setToolTip(tip);
        b->setCursor(locked ? Qt::ArrowCursor : Qt::PointingHandCursor);
        b->setStyleSheet(locked ? "QPushButton { color: #7a7d80; }" : QString());
    }

    // Clicking a tier-locked footer button opens the shared upgrade dialog
    // (ShowTierLimitDialog + the ShouldShowTierPopup throttle) — the same modal
    // used by the screenshare/chat create paths, with a clickable link to the
    // same upgrade URL as the dock cap chrome. `feature` is the source display
    // name; `tierWord` names the plan it needs (Basic / Streamer).
    static void ShowFooterUpgradePrompt(const QString& feature, const QString& tierWord) {
        if (!ShouldShowTierPopup()) return;
        ShowTierLimitDialog(
            QString("Feeds: %1").arg(feature),
            QString("%1 is a %2-tier feature. "
                    "<a href=\"https://letsdovideo.com/feeds-upgrade\">Click here "
                    "to upgrade your plan</a>.").arg(feature, tierWord));
    }

    // Re-style the footer from current tier + share state. Called from Refresh(),
    // so tier/login/meeting/share changes (which all already route through
    // RefreshAllSourceProperties -> PostParticipantDockRefresh) update it with no
    // separate mechanism. The footer itself is always visible (like "+ Add
    // Participant"); gating is per-button greying, for discoverability/upgrade.
    void UpdateFooterState() {
        if (!m_footer) return;

        const bool ssLocked   = g_currentTier < 1;   // screenshare: Basic+
        const bool chatLocked = g_currentTier < 2;   // overlay/popup: Streamer+

        ApplyFooterLock(m_footScreenshare, ssLocked, ssLocked
            ? "Screenshare needs the Basic plan or higher — upgrade to enable."
            : "Add the Zoom Screenshare source to the current scene");
        ApplyFooterLock(m_footChatOverlay, chatLocked, chatLocked
            ? "Zoom Chat Overlay is a Streamer-tier feature — upgrade to enable."
            : "Add a new Zoom Chat Overlay source to the current scene");
        ApplyFooterLock(m_footChatPopup, chatLocked, chatLocked
            ? "Zoom Chat Popup is a Streamer-tier feature — upgrade to enable."
            : "Add the Zoom Chat Popup source to the current scene");

        // Screenshare live status dot — only when the button is usable (no status
        // when tier-locked). Green if someone is sharing, grey otherwise. Reads
        // g_activeSharerUserId, the same signal zs_properties uses; its
        // share_status_changed handler already marshals a dock refresh here.
        int shareState = ssLocked ? -1 : (g_activeSharerUserId != 0 ? 1 : 0);
        if (shareState != m_footShareState) {
            m_footShareState = shareState;
            if (shareState < 0) {
                m_footScreenshare->setIcon(QIcon());          // locked: no dot
            } else {
                m_footScreenshare->setIcon(QIcon(ShareDotPixmap(shareState == 1)));
                m_footScreenshare->setIconSize(QSize(10, 10));
            }
        }
    }

    // Small right-aligned per-row button: add THIS source to the current edit
    // scene as a Paste Reference (new scene-item on the same source). A separate
    // sibling from the header label, so it doesn't collide with the rename
    // double-click. Shared by the live and non-live box header rows.
    QPushButton* MakeAddToSceneButton(const std::string& uuid) {
        QPushButton* b = new QPushButton("+");
        b->setFixedSize(18, 18);
        b->setToolTip("Add to current scene");
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet("QPushButton { padding: 0; font-weight: bold; }");
        QObject::connect(b, &QPushButton::clicked, [uuid]() {
            AddSourceReferenceToCurrentScene(uuid);
        });
        return b;
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
        headerL->addWidget(MakeIconButton(ScenesIcon(), "Included scenes",
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
    // divider / prompt / top-slot widget at that level — never a box's nested
    // header or combo. Each box is one QObject whose deleteLater frees its header
    // and combo transitively (they're parented into the box by its layout), so a
    // box's children are freed exactly once, none orphaned, no combo surviving.
    //
    // The QLayoutItem wrappers are not QObjects (no deleteLater) — keep deleting
    // those immediately; only the widget's own free is deferred.
    void ClearContent() {
        // Drop borrowed indicator pointers BEFORE deleting the boxes that own
        // them, so the poll can never touch a freed dot between clear and rebuild.
        m_rowIndicators.clear();
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

    // Fixed footer bar (outer layout, pinned to the dock bottom — does not scroll).
    // Built once in the ctor; UpdateFooterState() re-styles the buttons in place.
    QWidget*     m_footer          = nullptr;
    QPushButton* m_footScreenshare = nullptr;
    QPushButton* m_footChatOverlay = nullptr;
    QPushButton* m_footChatPopup   = nullptr;
    int          m_footShareState  = -2;  // last-applied screenshare dot: -2 none/-1 locked/0 grey/1 green

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
    obs_frontend_add_dock_by_id(
        "feeds_chat_dock", "Zoom Chat", g_chatDock);
    blog(LOG_INFO, "[feeds] chat dock registered");
}

static void SetupParticipantDock() {
    // Stable id — never change this across versions (see SetupChatDock).
    g_participantDock = new FeedsParticipantDock();
    obs_frontend_add_dock_by_id(
        "feeds_participant_dock", "Zoom Participants", g_participantDock);
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
    // participant sources. nullptr is now reserved for genuine
    // allocation/exception failures only. A source that can't pull a live
    // feed yet (not logged in, engine not ready, no participant bound, or
    // over the tier's active limit) is created in a dormant state instead:
    // its properties panel explains why (logged-out branch / upgrade prompt),
    // and selecting a participant once live brings it up like any other.
    //
    // C++ exceptions must not escape into libobs C frames, so the body stays
    // wrapped; on a real failure nullptr + OnSourceCreated's husk cleanup is
    // still far cheaper than a process-wide crash.
  try {
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
    } else {
        obs_data_set_string(settings, kParticipantNameKey, "");
        if (data) data->bound_this_session = false;
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
        std::string msg =
            "Your current tier only allows " + std::to_string(maxFeeds) +
            " participant source" + (maxFeeds == 1 ? "" : "s") +
            ". This is your " + std::to_string(pos) + OrdinalSuffix(pos) +
            " participant source. Please upgrade to activate this feed.";
        obs_properties_add_text(props, "tier_disabled_msg", msg.c_str(),
                                OBS_TEXT_INFO);
        obs_properties_add_button(props, "upgrade_btn",
            "Upgrade your plan to activate more feeds",
            [](obs_properties_t*, obs_property_t*, void*) -> bool {
                QDesktopServices::openUrl(
                    QUrl("https://letsdovideo.com/feeds-upgrade"));
                return true;
            });
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

        uint32_t currentWrite = data->header->write_index;
        if (currentWrite == data->lastReadIndex) continue;

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
                std::string msg = "Login failed: " + error;
                QMessageBox::critical(
                    static_cast<QWidget*>(obs_frontend_get_main_window()),
                    QString::fromUtf8("Feeds - Login"),
                    QString::fromUtf8(msg.c_str()));
                g_authInProgress = false;
                UpdateLoginLogoutMenuItem();
                g_pendingMeetingJoin = false;
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
                if (s) { s->bound_this_session = false; s->subscribed_user_id = 0; }
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
                if (s) { s->bound_this_session = false; s->subscribed_user_id = 0; }
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
                // binding). The remembered name in obs_data is untouched.
                // The engine tore down the subscriptions, so clear the
                // subscribed marker too — the next join must re-subscribe.
                if (s) { s->bound_this_session = false; s->subscribed_user_id = 0; }
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

        // Push into the overlay's centralised history (Streamer+ only).
        // Thread-safe; no need to marshal to the Qt main thread — the
        // overlay source reads under its own mutex on the graphics
        // thread.
        if (g_currentTier >= 2) {
            feeds::AppendChatMessageToOverlay(senderId, senderName, content,
                                              timestamp);
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

// Find the scene item(s) for a given source across all scenes and apply
// our default Fit bounds at the current OBS canvas resolution. Called
// deferred after source_create, because at source_create time the source
// has not yet been added to any scene.
static void ApplyFeedsBoundsToSceneItem(obs_source_t* source) {
    if (!source) return;

    // Determine bounds dimensions from the current OBS canvas. This makes
    // the fix adaptive to unusual canvas sizes (vertical, 4K, 720p, etc.)
    // rather than hardcoding 1920x1080.
    obs_video_info ovi;
    uint32_t boundsW = 1920;
    uint32_t boundsH = 1080;
    if (obs_get_video_info(&ovi)) {
        boundsW = ovi.base_width;
        boundsH = ovi.base_height;
    }

    struct SearchContext {
        obs_source_t* target;
        uint32_t w;
        uint32_t h;
    };
    SearchContext ctx = { source, boundsW, boundsH };

    auto enum_cb = [](void* param, obs_source_t* scene_src) -> bool {
        SearchContext* c = (SearchContext*)param;
        obs_scene_t* scene = obs_scene_from_source(scene_src);
        if (!scene) return true;

        auto item_cb = [](obs_scene_t*, obs_sceneitem_t* item, void* p) -> bool {
            SearchContext* c = (SearchContext*)p;
            obs_source_t* item_src = obs_sceneitem_get_source(item);
            if (item_src == c->target) {
                vec2 bounds;
                bounds.x = (float)c->w;
                bounds.y = (float)c->h;
                obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
                obs_sceneitem_set_bounds(item, &bounds);
                obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
            }
            return true;
        };

        obs_scene_enum_items(scene, item_cb, c);
        return true;
    };

    obs_enum_scenes(enum_cb, &ctx);
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

// Participant-video initial placement (v1.4.0). Unlike screenshare (which
// keeps the Fit bounds in ApplyFeedsBoundsToSceneItem above), participant
// video uses Automatic bounds (OBS_BOUNDS_NONE) so the user can crop it
// naturally with Alt-drag. The engine frame scaler already normalizes
// participant frames to a constant per-tier size before OBS sees them, so
// Fit is no longer needed to absorb Zoom's resolution drops — see the
// resolution-change investigation. Screenshare is variable-size and MUST
// stay on Fit; that's why the OnSourceCreated dispatch routes the two types
// to different functions.
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
// "+ Add Participant" and the footer's Chat Overlay button. UI thread.
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
    CreateSourceOfTypeInCurrentScene("zoom_participant_source", "Zoom Participant");
}

// Find the first live (non-removed) source of the given type, returning an owned
// ref (caller releases) or null. Skips OnSourceCreated "husks" (removed flag set)
// and any source mid-destroy (obs_source_get_ref returns null). Used by the
// reference-or-create footer buttons to decide reference vs. create. UI thread.
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
// create one (Action A). This is the screenshare / chat-popup footer path: it
// NEVER creates a second instance of a type that already exists, so the
// screenshare singleton block in zs_create is never reached. UI thread.
static void AddOrReferenceSourceInCurrentScene(const char* typeId, const char* baseName) {
    obs_source_t* existing = FindFirstSourceOfType(typeId);
    if (existing) {
        obs_source_t* sceneSrc = ResolveCurrentEditScene();
        if (sceneSrc) {
            obs_scene_t* scene = obs_scene_from_source(sceneSrc);   // borrowed
            if (scene) {
                obs_sceneitem_t* item = obs_scene_add(scene, existing);
                // Live source -> dims known -> fit-center; a dormant source
                // (0x0, e.g. screenshare with nobody sharing) lands at OBS
                // default, which ApplyFitCenterGeometry safely skips.
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
// Shares the deferred-enumeration scaffolding with
// ApplyFeedsBoundsToSceneItem — sets position + alignment rather than
// bounds, since the popup is meant to keep its natural size, not fit
// the canvas.
static void ApplyChatPopupDefaultPosition(obs_source_t* source) {
    if (!source) return;

    obs_video_info ovi;
    uint32_t canvasW = 1920;
    uint32_t canvasH = 1080;
    if (obs_get_video_info(&ovi)) {
        canvasW = ovi.base_width;
        canvasH = ovi.base_height;
    }

    // With OBS_ALIGN_BOTTOM | OBS_ALIGN_CENTER alignment the position is
    // the anchor point — the bottom-center of the scene-item's bbox
    // lands here, no source-dimension arithmetic needed. Margin from
    // the bottom edge is 4% of canvas height (~43px at 1080p, ~86px at
    // 4K) so the visual gap scales with the streamer's resolution.
    const int marginFromBottom = (int)((float)canvasH * 0.04f);
    vec2 pos;
    pos.x = (float)canvasW * 0.5f;
    pos.y = (float)canvasH - (float)marginFromBottom;

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
                // OBS_ALIGN_CENTER is the zero value (horizontal centre
                // is the default when neither LEFT nor RIGHT is set);
                // OR'ing it in is self-documentation.
                obs_sceneitem_set_pos(item, &c->pos);
                obs_sceneitem_set_alignment(
                    item, OBS_ALIGN_BOTTOM | OBS_ALIGN_CENTER);
            }
            return true;
        };

        obs_scene_enum_items(scene, item_cb, c);
        return true;
    };

    obs_enum_scenes(enum_cb, &ctx);
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

    // Only apply to Feeds source types. Participant and screenshare are
    // split deliberately: participant video gets Automatic bounds (the
    // engine scaler holds its size constant), screenshare keeps Fit bounds
    // (its frames are variable-size and Fit absorbs content-size changes).
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

    // Hold a weak reference; promote to strong reference inside the
    // deferred callback. This avoids holding the source alive if the user
    // (or OBS during shutdown) removes it in the interim.
    obs_weak_source_t* weak = obs_source_get_weak_source(source);
    if (!weak) return;

    std::thread([weak, isParticipant, isChatPopup, isChatOverlay]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        obs_source_t* strong = obs_weak_source_get_source(weak);
        if (strong) {
            if (isChatPopup) {
                ApplyChatPopupDefaultPosition(strong);
            } else if (isChatOverlay) {
                ApplyChatOverlayDefaults(strong);
            } else if (isParticipant) {
                // Automatic bounds + fill-canvas-centered initial placement.
                ApplyParticipantPlacement(strong);
            } else {
                // Screenshare: keep the existing Fit bounds path, unchanged.
                ApplyFeedsBoundsToSceneItem(strong);
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
    zoom_participant_info.get_name       = [](void*) { return "Zoom Participant"; };
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
    zoom_screenshare_info.get_name       = [](void*) { return "Zoom Screenshare"; };
    zoom_screenshare_info.create         = zs_create;
    zoom_screenshare_info.destroy        = zs_destroy;
    zoom_screenshare_info.get_properties = zs_properties;
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

    obs_frontend_add_event_callback([](enum obs_frontend_event event, void*) {
        if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
            SetupPluginMenu();
            QTimer::singleShot(5000,
                (QObject*)obs_frontend_get_main_window(),
                []() { CheckForUpdateAsync(); });
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
