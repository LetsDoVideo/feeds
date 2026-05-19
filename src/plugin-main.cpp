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
#include <QDateTime>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QRegularExpression>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QTimer>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QUrl>
#include <QUrlQuery>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>

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
static QAction* g_loginAction   = nullptr;
static QAction* g_logoutAction  = nullptr;
static QAction* g_connectAction = nullptr;

// ---------------------------------------------------------------------------
// Globals — cached state from engine
// ---------------------------------------------------------------------------
static bool g_isLoggedIn          = false;
static bool g_isInMeeting         = false;
static bool g_rawLiveStreamGranted = false;
static bool g_pendingMeetingJoin   = false;

static std::string g_userDisplayName;
static std::string g_userPMI;
static int         g_currentTier = 0;
static uint32_t    g_enginePid   = 0;   // Populated from engine_ready

static unsigned long long g_currentMeetingNumber = 0;

// Set when the plugin sends create_instant_meeting, consumed (and cleared)
// by the meeting_joined handler so it knows to surface the share-this-meeting
// popup, plus defensively cleared on meeting_failed / meeting_left. The
// consumer is wired in commit 4; commit 3 only sets this so the protocol
// shape is in place when commit 4 lands.
static std::atomic<bool> g_pendingInstantMeeting{false};
static unsigned int       g_activeSharerUserId   = 0;
static unsigned int       g_cachedMyUserId       = 0;
static unsigned int       g_activeSpeakerUserId  = 0;

struct CachedParticipant {
    unsigned int id;
    std::string  name;
};
static std::vector<CachedParticipant> g_cachedParticipants;
static std::mutex                     g_participantsMutex;

static int g_activeParticipantSources = 0;

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

// ---------------------------------------------------------------------------
// Per-source data
// ---------------------------------------------------------------------------
struct ZpSourceData {
    obs_source_t* source          = nullptr;
    std::string   uuid;
    unsigned int  current_user_id = 0;
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
        "A new version of Feeds is available.", dlg);

    QLabel* curLabel = new QLabel(
        QString("<b>Current version:</b> ") + feeds_shared::VERSION, dlg);
    curLabel->setTextFormat(Qt::RichText);
    curLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    QLabel* latestLabel = new QLabel(
        QString("<b>Latest version:</b> ") +
        QString::fromStdString(latestVer), dlg);
    latestLabel->setTextFormat(Qt::RichText);
    latestLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    QPushButton* notesBtn = new QPushButton("View Release Notes", dlg);
    QObject::connect(notesBtn, &QPushButton::clicked, dlg, [dlg]() {
        QDesktopServices::openUrl(
            QUrl("https://github.com/LetsDoVideo/feeds/releases"));
        dlg->accept();
    });

    QPushButton* laterBtn = new QPushButton("Later", dlg);
    laterBtn->setDefault(true);
    QObject::connect(laterBtn, &QPushButton::clicked,
                     dlg, &QDialog::accept);

    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->addWidget(notesBtn);
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

        data->header->last_read_index = currentWrite;
        data->lastReadIndex = currentWrite;
    }

    blog(LOG_INFO, "[feeds] pump thread exiting for source=%s",
         data->uuid.c_str());
}

static void StartPumpThread(ZpSourceData* data) {
    if (!data) return;
    if (data->pumpThread.joinable()) return;

    data->pumpShouldExit = false;
    if (!data->pumpWakeEvent) {
        data->pumpWakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    data->pumpThread = std::thread(PumpThreadFunc, data);
}

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
        MessageBoxA(NULL, "You are already logged in to Zoom.",
                    "Feeds - Login", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (g_loginAction) g_loginAction->setEnabled(false);
    feeds::SendToEngine("{\"type\":\"login_start\"}");
}

void OnLogoutClick() {
    if (!g_isLoggedIn) {
        MessageBoxA(NULL, "You are not currently logged in to Zoom.",
                    "Feeds - Logout", MB_OK | MB_ICONINFORMATION);
        return;
    }
    feeds::SendToEngine("{\"type\":\"logout\"}");
}

void OnConnectClick() {
    if (!g_isLoggedIn) {
        g_pendingMeetingJoin = true;
        MessageBoxA(NULL,
            "You need to log in to Zoom first.\n\n"
            "Please log in and then try Connect to Zoom Meeting again.",
            "Feeds - Login Required", MB_OK | MB_ICONINFORMATION);
        OnLoginClick();
        return;
    }

    if (g_isInMeeting) {
        MessageBoxA(NULL,
            "You are already connected to a Zoom meeting.\n\n"
            "Use the Leave button in the Zoom window to disconnect.",
            "Feeds - Already Connected", MB_OK | MB_ICONINFORMATION);
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

    // Two-row choice screen. Top row groups the two "host your own meeting"
    // paths (Instant + PMI) — both put the user in as host with no permission
    // prompt and get equal emphasis. Bottom row holds the "join someone
    // else's meeting" path in the demoted secondary style. Captured `choice`
    // discriminates the branches:
    // 1 = Create Instant Meeting, 2 = PMI, 3 = Join by Number or Link.
    int choice = 0;
    {
        QDialog dlg(mainWindow);
        dlg.setWindowTitle("Connect to Zoom Meeting");

        QString pmiLabel = "My Personal Meeting Room";
        if (!g_userPMI.empty())
            pmiLabel += "\n(" + QString::fromStdString(g_userPMI) + ")";

        QPushButton* instantBtn = new QPushButton("Create Instant\nMeeting", &dlg);
        QPushButton* pmiBtn     = new QPushButton(pmiLabel, &dlg);
        QPushButton* linkBtn    = new QPushButton("Join by Number\nor Link", &dlg);

        // min-height: multi-line labels + padding need vertical room or Qt
        // crops the second line on some platforms. Both top-row buttons use
        // the emphasized style with the larger padding → 80px. Link uses
        // the compact secondary style → 70px.
        // Transparent border in the base state reserves space so the button
        // doesn't grow by 2px when the hover border appears — without this
        // reservation, Qt would re-layout on hover, jittering the row.
        const char* emphasizedBtnStyle =
            "QPushButton { "
                "background-color: palette(highlight); "
                "color: palette(highlighted-text); "
                "font-weight: bold; "
                "padding: 18px 28px; "
                "min-height: 80px; "
                "border: 1px solid transparent; "
            "} "
            "QPushButton:hover { "
                "border: 1px solid palette(highlighted-text); "
            "}";
        const char* secondaryBtnStyle =
            "QPushButton { "
                "padding: 12px 16px; "
                "min-height: 70px; "
            "}";
        instantBtn->setStyleSheet(emphasizedBtnStyle);
        pmiBtn    ->setStyleSheet(emphasizedBtnStyle);
        linkBtn   ->setStyleSheet(secondaryBtnStyle);
        // Pin button heights to match their stylesheet min-heights so the
        // QHBoxLayout's vertical sizeHint == its content's actual height.
        // Without this, Qt allocates the row extra vertical space (the
        // buttons' sizeHint exceeds their min-height when style padding
        // is factored in), which manifests as a gap between tip and row.
        instantBtn->setMaximumHeight(80);
        pmiBtn    ->setMaximumHeight(80);
        linkBtn   ->setMaximumHeight(70);

        // Both emphasized buttons get the same minimum width so they render
        // at equal sizes regardless of their text content lengths. Without
        // this, QPushButton sizes each button to fit its own label, which
        // makes PMI (long account name + meeting number) much wider than
        // Instant Meeting. The minimum is sized to comfortably fit
        // "My Personal Meeting Room" plus padding — the longest expected
        // label.
        instantBtn->setMinimumWidth(240);
        pmiBtn    ->setMinimumWidth(240);

        // Mouse-driven dialog — disable keyboard focus on all three buttons
        // so the initially-focused button doesn't render with a focus border
        // that looks identical to a hover border (made Instant appear "stuck
        // highlighted" until OBS lost Windows focus). Esc-to-cancel still
        // works because that's handled by the QDialog itself, not the buttons.
        instantBtn->setFocusPolicy(Qt::NoFocus);
        pmiBtn    ->setFocusPolicy(Qt::NoFocus);
        linkBtn   ->setFocusPolicy(Qt::NoFocus);

        QObject::connect(instantBtn, &QPushButton::clicked, &dlg,
                         [&]() { choice = 1; dlg.accept(); });
        QObject::connect(pmiBtn,     &QPushButton::clicked, &dlg,
                         [&]() { choice = 2; dlg.accept(); });
        QObject::connect(linkBtn,    &QPushButton::clicked, &dlg,
                         [&]() { choice = 3; dlg.accept(); });

        QLabel* tipLabel = new QLabel(
            "<span style=\"color:gray;font-style:italic\">"
            "Tip: Joining your own meeting avoids permission prompts."
            "</span>", &dlg);
        tipLabel->setTextFormat(Qt::RichText);
        tipLabel->setAlignment(Qt::AlignCenter);
        // Fixed vertical policy — without this Qt lets the label expand
        // vertically to absorb spare space, which manifests as a big gap
        // between the tip and the buttons below it.
        tipLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

        // Top row — the two "host your own meeting" paths, equal emphasis.
        // Per-widget Qt::AlignTop anchors each button to the top of the row's
        // allocated rect; row->setAlignment(Qt::AlignTop) positions the row
        // itself within the parent VBox. Both are needed (per the v1.0.8
        // debug session) to keep Qt from injecting vertical dead space.
        QHBoxLayout* topRow = new QHBoxLayout();
        topRow->addWidget(instantBtn, 0, Qt::AlignTop);
        topRow->addWidget(pmiBtn,     0, Qt::AlignTop);
        topRow->setAlignment(Qt::AlignTop);

        // Bottom row — the "join someone else's meeting" path, centered with
        // stretch on both sides so the button keeps its natural size rather
        // than spanning the row's full width.
        QHBoxLayout* bottomRow = new QHBoxLayout();
        bottomRow->addStretch();
        bottomRow->addWidget(linkBtn, 0, Qt::AlignTop);
        bottomRow->addStretch();
        bottomRow->setAlignment(Qt::AlignTop);

        QVBoxLayout* layout = new QVBoxLayout(&dlg);
        // SetFixedSize sizes the dialog to exactly the layout's sizeHint
        // with no slack — combined with per-widget Qt::AlignTop on the
        // row's children, the dialog renders flush with the contents.
        layout->setSizeConstraint(QLayout::SetFixedSize);
        layout->addWidget(tipLabel);
        layout->addSpacing(8);
        layout->addLayout(topRow);
        layout->addLayout(bottomRow);

        if (dlg.exec() != QDialog::Accepted || choice == 0) return;
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
            MessageBoxA(NULL,
                "Could not retrieve your Personal Meeting Room ID.\n"
                "Please use Join by Meeting Number instead.",
                "Feeds", MB_OK | MB_ICONWARNING);
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
    explicit FeedsChatDock(QWidget* parent = nullptr) : QWidget(parent) {
        m_list = new QListWidget(this);
        m_list->addItem("Not connected to a meeting");

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

        QVBoxLayout* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(m_list);

        setMinimumSize(200, 300);
    }

    // All three methods below must run on the Qt main thread. The IPC
    // handlers marshal via QTimer::singleShot onto the main window before
    // invoking them.

    void AppendMessage(const QString& senderName,
                       const QString& content,
                       qint64 timestamp) {
        // timestamp is kept in the signature so the engine's value
        // flows through unchanged — future surfaces (tooltip, an
        // opt-in "show timestamps" preference, the overlay source)
        // can pick it up without an IPC change.
        (void)timestamp;

        if (!m_messagesStarted) {
            // First real message — drop the placeholder. From here on the
            // dock shows only real chat history for the rest of the OBS
            // session. We don't re-add the placeholder after a meeting
            // ends; preserving history across meetings is the Phase 1
            // behavior per spec.
            m_list->clear();
            m_messagesStarted = true;
        }
        m_list->addItem(QString("%1: %2").arg(senderName, content));
        m_list->scrollToBottom();
    }

    void OnMeetingJoined() { SetPlaceholder("Connected"); }
    void OnMeetingLeft()   { SetPlaceholder("Not connected to a meeting"); }

private:
    void SetPlaceholder(const QString& text) {
        // Once real messages have arrived, the placeholder is permanently
        // off for the rest of the OBS session — joining/leaving meetings
        // must not stomp on accumulated chat history.
        if (m_messagesStarted) return;
        m_list->clear();
        m_list->addItem(text);
    }

    QListWidget* m_list             = nullptr;
    bool         m_messagesStarted  = false;
};

// Non-owning pointer to the registered dock instance. OBS owns the widget's
// lifetime once it's been handed to obs_frontend_add_dock_by_id; we hold
// this only so the chat_message IPC handler can route messages to it.
static FeedsChatDock* g_chatDock = nullptr;

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
static std::mutex                       g_avatarCacheMutex;
static std::map<unsigned int, QImage>   g_avatarCache;
static QImage                           g_fallbackAvatar;
static bool                             g_fallbackAvatarLoaded = false;

static QImage GetAvatarForSender(unsigned int senderId,
                                 const std::string& avatarPath) {
    std::lock_guard<std::mutex> lock(g_avatarCacheMutex);

    if (!g_fallbackAvatarLoaded) {
        g_fallbackAvatarLoaded = true;
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
    }

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

void SetupPluginMenu() {
    QMainWindow* mainWindow = (QMainWindow*)obs_frontend_get_main_window();
    QMenuBar*    menuBar    = mainWindow->menuBar();
    QMenu*       feedsMenu  = new QMenu("Feeds", menuBar);
    menuBar->addMenu(feedsMenu);

    g_loginAction   = feedsMenu->addAction("Login to Zoom");
    g_logoutAction  = feedsMenu->addAction("Logout of Zoom");
    feedsMenu->addSeparator();
    g_connectAction = feedsMenu->addAction("Connect to Zoom Meeting...");
    feedsMenu->addSeparator();
    QAction* aboutAction = feedsMenu->addAction("About / Tier Status");

    // Sync menu action states to the current plugin state. If the engine
    // has already finished authenticating (common on startup when a valid
    // refresh token was persisted from a previous session), the login
    // handler fired before this menu existed — its setEnabled() calls
    // silently no-op'd against the then-null action pointers. We re-apply
    // the correct state here now that the actions exist.
    if (g_isLoggedIn) {
        g_loginAction->setEnabled(false);
        g_logoutAction->setEnabled(true);
        g_connectAction->setEnabled(!g_isInMeeting);
    } else {
        g_logoutAction->setEnabled(false);
        g_connectAction->setEnabled(false);
    }

    QObject::connect(g_loginAction,   &QAction::triggered, []() { OnLoginClick(); });
    QObject::connect(g_logoutAction,  &QAction::triggered, []() { OnLogoutClick(); });
    QObject::connect(g_connectAction, &QAction::triggered, []() { OnConnectClick(); });
    QObject::connect(aboutAction, &QAction::triggered, []() {
        std::string tierName;
        switch (g_currentTier) {
            case 1:  tierName = "Basic";       break;
            case 2:  tierName = "Streamer";    break;
            case 3:  tierName = "Broadcaster"; break;
            default: tierName = "Free";        break;
        }
        std::string aboutText = std::string("Feeds v") + feeds_shared::VERSION + "\n";
        if (!g_userDisplayName.empty())
            aboutText += "Logged in as: " + g_userDisplayName + "\n";
        aboutText += "Tier: " + tierName;
        MessageBoxA(NULL, aboutText.c_str(), "About Feeds", MB_OK);
    });
}

// ---------------------------------------------------------------------------
// Source callbacks
// ---------------------------------------------------------------------------
// Throttle for the "upgrade required" popup. When OBS loads a saved scene,
// zp_create fires for every source in rapid succession — if the user has
// more saved sources than their current tier allows, we don't want to
// stack N popups. Show at most one per throttle window.
static std::atomic<uint64_t> g_lastTierPopupMs{0};
static constexpr uint64_t TIER_POPUP_THROTTLE_MS = 3000;

static bool ShouldShowTierPopup() {
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
static void ShowTierLimitDialog(const QString& title, const QString& html) {
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

static void* zp_create(obs_data_t* settings, obs_source_t* source) {
    (void)settings;

    // Tier gating: enforce max feeds per the current tier, but only if
    // logged in. On OBS startup, saved sources may be created before
    // the engine finishes logging the user in — at that point
    // g_currentTier is still 0 (default) and would spuriously block
    // users restoring a saved scene. Skipping the check pre-login means
    // the source gets created silently; we accept that if the user is
    // over-tier at login time, nothing re-enforces until next restart.
    // In practice this is fine: users don't log out and back in as a
    // lower tier mid-session as a normal workflow.
    //
    // When the check does fire (interactive creation while logged in,
    // or OBS restart after login completes), it's throttled so that
    // loading a saved scene with many over-tier sources doesn't stack
    // a popup per source — one popup per ~3 second window.
    if (g_isLoggedIn &&
        g_activeParticipantSources >= GetMaxFeedsForTier() &&
        ShouldShowTierPopup()) {
        if (g_currentTier >= 3) {
            ShowTierLimitDialog(
                "Feeds - Maximum Feeds Reached",
                "You've reached the maximum number of feeds for "
                "the Broadcaster plan.<br><br>"
                "<a href=\"mailto:support@letsdovideo.com\">Contact "
                "support</a> if you need a custom solution.");
        } else {
            ShowTierLimitDialog(
                "Feeds - Upgrade Required",
                "You've reached the maximum number of feeds for "
                "your current plan.<br><br>"
                "<a href=\"https://letsdovideo.com/feeds-upgrade\">"
                "Upgrade your plan</a> to add more.");
        }
        return nullptr;
    }
    // If we're over-tier but the throttle suppressed the popup, still
    // block creation silently — we don't want to let the user build
    // past their tier just because we chose not to annoy them.
    if (g_isLoggedIn && g_activeParticipantSources >= GetMaxFeedsForTier()) {
        return nullptr;
    }

    obs_source_set_async_unbuffered(source, true);

    ZpSourceData* data = new ZpSourceData();
    data->source = source;

    const char* uuid = obs_source_get_uuid(source);
    data->uuid = uuid ? uuid : "";

    {
        std::lock_guard<std::mutex> lock(g_sourcesMutex);
        g_allParticipantSources.push_back(data);
    }

    g_activeParticipantSources++;
    return data;
}

static void zp_destroy(void* vdata) {
    if (!vdata) return;
    ZpSourceData* data = static_cast<ZpSourceData*>(vdata);

    if (!data->uuid.empty()) {
        std::string msg = "{\"type\":\"participant_source_unsubscribe\","
                          "\"source_id\":\"" + data->uuid + "\"}";
        feeds::SendToEngine(msg);
    }

    // clearTexture=false: source is being destroyed, don't touch it.
    CloseSharedMemory(data, false);

    {
        std::lock_guard<std::mutex> lock(g_sourcesMutex);
        auto it = std::find(g_allParticipantSources.begin(),
                            g_allParticipantSources.end(), data);
        if (it != g_allParticipantSources.end())
            g_allParticipantSources.erase(it);
    }

    g_activeParticipantSources--;
    delete data;
}

static void zp_update(void* vdata, obs_data_t* settings) {
    if (!vdata) return;
    ZpSourceData* data = static_cast<ZpSourceData*>(vdata);

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
        if (!data->uuid.empty()) {
            std::string msg = "{\"type\":\"participant_source_unsubscribe\","
                              "\"source_id\":\"" + data->uuid + "\"}";
            feeds::SendToEngine(msg);
        }
        CloseSharedMemory(data);
        return;
    }

    // Tier enforcement lives in zp_create — by the time we get here the
    // source already exists, so blocking subscription wouldn't prevent
    // the user from creating over-tier sources. Create-time enforcement
    // is the simpler and more honest gate.
    data->current_user_id = selected_id;

    // selected_id == 1 is [Active Speaker] sentinel. Engine handles the
    // follow-speaker routing — we just pass the sentinel through.
    // selected_id > 1 is a real Zoom SDK user ID.
    if (!data->uuid.empty() && g_isInMeeting && g_rawLiveStreamGranted) {
        std::string msg = "{\"type\":\"participant_source_subscribe\","
                          "\"source_id\":\"" + data->uuid + "\","
                          "\"participant_id\":" + std::to_string(selected_id) + "}";
        feeds::SendToEngine(msg);
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

static obs_properties_t* zp_properties(void* data) {
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

    if (g_isInMeeting)
        feeds::SendToEngine("{\"type\":\"get_participants\"}");

    obs_properties_t* props = obs_properties_create();
    std::string verLabel = std::string("Feeds (v") + feeds_shared::VERSION + ")";
    obs_properties_add_text(props, "ver_label", verLabel.c_str(), OBS_TEXT_INFO);

    if (!g_isInMeeting) {
        if (!g_isLoggedIn) {
            obs_properties_add_button(props, "login_btn",
                "Not logged in to Zoom. Click to Login...",
                [](obs_properties_t*, obs_property_t*, void*) -> bool {
                    OnLoginClick();
                    return true;
                });
        } else {
            obs_properties_add_button(props, "connect_btn",
                "Logged in. Click to Connect to Zoom Meeting...",
                [](obs_properties_t*, obs_property_t*, void*) -> bool {
                    OnConnectClick();
                    return true;
                });
        }
    } else {
        std::string status_text = "Status: Connected";
        if (g_currentMeetingNumber != 0)
            status_text = "Status: Connected to Meeting " +
                          std::to_string(g_currentMeetingNumber);
        obs_properties_add_text(props, "status_label", status_text.c_str(),
                                OBS_TEXT_INFO);

        obs_properties_add_button(props, "refresh_btn",
            "Refresh Participant List",
            [](obs_properties_t*, obs_property_t*, void*) -> bool {
                feeds::SendToEngine("{\"type\":\"get_participants\"}");
                return true;
            });
    }

    obs_property_t* list = obs_properties_add_list(
        props, "participant_id", "Select Participant",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

    // Mirror the login/join button's state machine: the dropdown is only
    // enabled when there's actually someone selectable. In every other
    // state we replace the list with a single contextual placeholder and
    // grey the control out so users can't open it before login/meeting.
    // RefreshAllSourceProperties() is already called on login_success,
    // meeting_joined, meeting_ended, logout, and participant_list_changed,
    // so this re-evaluates live without a properties-dialog reopen.
    if (!g_isLoggedIn) {
        obs_property_list_add_int(list, "Login to Zoom first", 0);
        obs_property_set_enabled(list, false);
    } else if (!g_isInMeeting) {
        obs_property_list_add_int(list, "Join a meeting first", 0);
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

static void ZsStartPumpThread(ZsSourceData* data) {
    if (!data) return;
    if (data->pumpThread.joinable()) return;

    data->pumpShouldExit = false;
    if (!data->pumpWakeEvent) {
        data->pumpWakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    data->pumpThread = std::thread(ZsPumpThreadFunc, data);
}

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

    // Tier gating: screenshare is a paid feature (Basic tier and up).
    // Same login-deferred logic as zp_create — if the user has a saved
    // scene with a screenshare source and login hasn't completed yet,
    // we don't want to spuriously block creation. Once logged in, free
    // tier users get a friendly upgrade prompt.
    if (g_isLoggedIn && g_currentTier == 0 && ShouldShowTierPopup()) {
        ShowTierLimitDialog(
            "Feeds - Upgrade Required",
            "Screenshare is a paid feature.<br><br>"
            "Your current tier is Free. Upgrade to Basic, Streamer, "
            "or Broadcaster to use Zoom Screenshare in OBS.<br><br>"
            "<a href=\"https://letsdovideo.com/feeds-upgrade\">"
            "Upgrade your plan</a>");
        return nullptr;
    }
    // If popup was throttled, still block creation silently — same
    // reasoning as zp_create: we don't grant the feature just because
    // we chose not to annoy the user.
    if (g_isLoggedIn && g_currentTier == 0) {
        return nullptr;
    }

    // One screenshare source per scene. Zoom only exposes a single
    // active sharer at a time, so a second source would render the
    // same stream.
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

    // If we're already in a meeting with an active share, open the
    // mapping immediately. Otherwise, wait for share_status_changed.
    if (g_isInMeeting && g_rawLiveStreamGranted && g_activeSharerUserId != 0) {
        OpenShareSharedMemory(data);
    }

    return data;
}

static void zs_destroy(void* vdata) {
    if (!vdata) return;
    ZsSourceData* data = static_cast<ZsSourceData*>(vdata);

    // clearTexture=false: source is being torn down, don't touch it.
    // Same bug pattern that bit us on zp_destroy earlier.
    CloseShareSharedMemory(data, false);

    {
        std::lock_guard<std::mutex> lock(g_screenshareSourcesMutex);
        auto it = std::find(g_allScreenshareSources.begin(),
                            g_allScreenshareSources.end(), data);
        if (it != g_allScreenshareSources.end())
            g_allScreenshareSources.erase(it);
    }

    delete data;
}

static obs_properties_t* zs_properties(void* data) {
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
                "Not logged in to Zoom. Click to Login...",
                [](obs_properties_t*, obs_property_t*, void*) -> bool {
                    OnLoginClick();
                    return true;
                });
        } else {
            obs_properties_add_button(props, "connect_btn",
                "Logged in. Click to Connect to Zoom Meeting...",
                [](obs_properties_t*, obs_property_t*, void*) -> bool {
                    OnConnectClick();
                    return true;
                });
        }
    } else {
        std::string status_text;
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
        obs_properties_add_text(props, "status_label", status_text.c_str(),
                                OBS_TEXT_INFO);
    }

    return props;
}

// ---------------------------------------------------------------------------
// Source info
// ---------------------------------------------------------------------------
struct obs_source_info zoom_participant_info = {};
struct obs_source_info zoom_screenshare_info = {};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("feeds", "en-US")

// ---------------------------------------------------------------------------
// Protocol handler registration (ldvfeeds://)
// ---------------------------------------------------------------------------
static void RegisterProtocolHandler() {
    char pluginPath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, pluginPath, MAX_PATH);
    std::string obsPath(pluginPath);
    size_t binPos = obsPath.rfind("obs64.exe");
    if (binPos == std::string::npos) return;

    std::string helperExe = obsPath.substr(0, binPos) + "FeedsLogin.exe";
    std::string command = "\"" + helperExe + "\" \"%1\"";

    HKEY hKey;
    RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Classes\\ldvfeeds",
        0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
    RegSetValueExA(hKey, "", 0, REG_SZ, (BYTE*)"URL:Feeds Protocol", 19);
    RegSetValueExA(hKey, "URL Protocol", 0, REG_SZ, (BYTE*)"", 1);
    RegCloseKey(hKey);

    RegCreateKeyExA(HKEY_CURRENT_USER,
        "Software\\Classes\\ldvfeeds\\shell\\open\\command",
        0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
    RegSetValueExA(hKey, "", 0, REG_SZ,
        (BYTE*)command.c_str(), (DWORD)command.size() + 1);
    RegCloseKey(hKey);
}

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

    feeds::RegisterMessageHandler("login_succeeded", [](const std::string& json) {
        g_userDisplayName = ExtractJsonString(json, "display_name");
        g_userPMI         = ExtractJsonString(json, "pmi");
        g_currentTier     = (int)ExtractJsonNumber(json, "tier");
        blog(LOG_INFO, "[feeds] login_succeeded: name='%s', pmi='%s', tier=%d",
             g_userDisplayName.c_str(), g_userPMI.c_str(), g_currentTier);

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
        blog(LOG_ERROR, "[feeds] login_failed: %s", error.c_str());
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
            [error]() {
                std::string msg = "Login failed: " + error;
                MessageBoxA(NULL, msg.c_str(), "Feeds - Login", MB_OK | MB_ICONERROR);
                if (g_loginAction) g_loginAction->setEnabled(true);
                g_pendingMeetingJoin = false;
            });
    });

    feeds::RegisterMessageHandler("sdk_authenticated", [](const std::string&) {
        blog(LOG_INFO, "[feeds] sdk_authenticated");
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            g_isLoggedIn = true;
            if (g_loginAction)   g_loginAction->setEnabled(false);
            if (g_logoutAction)  g_logoutAction->setEnabled(true);
            if (g_connectAction) g_connectAction->setEnabled(true);
            RefreshAllSourceProperties();

            if (g_pendingMeetingJoin) {
                g_pendingMeetingJoin = false;
                QTimer::singleShot(500, []() { OnConnectClick(); });
            }
        });
    });

    feeds::RegisterMessageHandler("sdk_auth_failed", [](const std::string& json) {
        blog(LOG_ERROR, "[feeds] sdk_auth_failed: %s", json.c_str());
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            MessageBoxA(NULL,
                "Zoom authentication failed. Please try logging in again.",
                "Feeds - Auth Failed", MB_OK | MB_ICONERROR);
            if (g_loginAction) g_loginAction->setEnabled(true);
            g_pendingMeetingJoin = false;
        });
    });

    feeds::RegisterMessageHandler("logout_complete", [](const std::string&) {
        blog(LOG_INFO, "[feeds] logout_complete");

        {
            std::lock_guard<std::mutex> lock(g_sourcesMutex);
            for (ZpSourceData* s : g_allParticipantSources) {
                CloseSharedMemory(s);
            }
        }
        CloseSharedMemoryForAllScreenshareSources();

        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            g_isLoggedIn           = false;
            g_isInMeeting          = false;
            g_rawLiveStreamGranted = false;
            g_currentMeetingNumber = 0;
            g_activeSharerUserId   = 0;
            g_activeSpeakerUserId  = 0;
            g_cachedMyUserId       = 0;
            g_userDisplayName.clear();
            g_userPMI.clear();
            g_currentTier = 0;
            {
                std::lock_guard<std::mutex> lock(g_participantsMutex);
                g_cachedParticipants.clear();
            }

            if (g_loginAction)   g_loginAction->setEnabled(true);
            if (g_logoutAction)  g_logoutAction->setEnabled(false);
            if (g_connectAction) g_connectAction->setEnabled(false);
            RefreshAllSourceProperties();

            MessageBoxA(NULL, "You have been logged out of Zoom.",
                        "Feeds - Logout", MB_OK | MB_ICONINFORMATION);
        });
    });

    feeds::RegisterMessageHandler("session_expired", [](const std::string&) {
        blog(LOG_WARNING, "[feeds] session_expired");
        {
            std::lock_guard<std::mutex> lock(g_sourcesMutex);
            for (ZpSourceData* s : g_allParticipantSources) {
                CloseSharedMemory(s);
            }
        }
        CloseSharedMemoryForAllScreenshareSources();
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            g_isLoggedIn = false;
            g_isInMeeting = false;
            g_rawLiveStreamGranted = false;
            g_userDisplayName.clear();
            g_userPMI.clear();
            g_currentTier = 0;
            {
                std::lock_guard<std::mutex> lock(g_participantsMutex);
                g_cachedParticipants.clear();
            }
            if (g_loginAction)   g_loginAction->setEnabled(true);
            if (g_logoutAction)  g_logoutAction->setEnabled(false);
            if (g_connectAction) g_connectAction->setEnabled(false);
            RefreshAllSourceProperties();

            MessageBoxA(NULL,
                "Your Zoom login has expired and could not be renewed.\n\n"
                "Please log in again.",
                "Feeds - Session Expired", MB_OK | MB_ICONWARNING);
        });
    });

    feeds::RegisterMessageHandler("meeting_joined", [](const std::string& json) {
        std::string mn = ExtractJsonString(json, "meeting_number");
        blog(LOG_INFO, "[feeds] meeting_joined: %s", mn.c_str());
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
                MessageBoxA(NULL, msg.c_str(), "Feeds - Join Failed",
                            MB_OK | MB_ICONERROR);
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
            }
        }
        CloseSharedMemoryForAllScreenshareSources();

        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            g_isInMeeting          = false;
            g_rawLiveStreamGranted = false;
            g_currentMeetingNumber = 0;
            g_activeSharerUserId   = 0;
            g_activeSpeakerUserId  = 0;
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

        // Auto-subscribe any sources that had a participant (including
        // [Active Speaker], sentinel 1) picked before the meeting was
        // joined / privilege was granted.
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            std::lock_guard<std::mutex> lock(g_sourcesMutex);
            for (ZpSourceData* s : g_allParticipantSources) {
                if (s && s->current_user_id >= 1 && !s->uuid.empty()) {
                    std::string msg = "{\"type\":\"participant_source_subscribe\","
                                      "\"source_id\":\"" + s->uuid + "\","
                                      "\"participant_id\":" +
                                      std::to_string(s->current_user_id) + "}";
                    feeds::SendToEngine(msg);
                }
            }
        });
    });

    feeds::RegisterMessageHandler("raw_livestream_timeout", [](const std::string&) {
        blog(LOG_WARNING, "[feeds] raw_livestream_timeout - host did not approve");
    });

    feeds::RegisterMessageHandler("participant_list_changed",
    [](const std::string& json) {
        unsigned int myUserId = (unsigned int)ExtractJsonNumber(json, "my_user_id");

        std::vector<CachedParticipant> newList;
        size_t pos = json.find("\"participants\"");
        if (pos != std::string::npos) {
            pos = json.find('[', pos);
            if (pos != std::string::npos) {
                size_t end = json.find(']', pos);
                std::string arr = json.substr(pos, end == std::string::npos
                                                    ? std::string::npos
                                                    : end - pos);
                size_t cursor = 0;
                while ((cursor = arr.find('{', cursor)) != std::string::npos) {
                    size_t objEnd = arr.find('}', cursor);
                    if (objEnd == std::string::npos) break;
                    std::string obj = arr.substr(cursor, objEnd - cursor + 1);

                    CachedParticipant p;
                    p.id   = (unsigned int)ExtractJsonNumber(obj, "id");
                    p.name = ExtractJsonString(obj, "name");
                    if (p.id != 0 && !p.name.empty())
                        newList.push_back(p);
                    cursor = objEnd + 1;
                }
            }
        }

        bool changed = false;
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
            g_cachedMyUserId     = myUserId;
            g_cachedParticipants = std::move(newList);
        }

        if (!changed) return;

        blog(LOG_INFO, "[feeds] participant_list_changed: %zu participants",
             (size_t)g_cachedParticipants.size());

        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
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
        std::string messageId  = ExtractJsonString(json, "message_id");
        unsigned int senderId  =
            (unsigned int)ExtractJsonNumber(json, "sender_id");
        std::string senderName = ExtractJsonStringEscaped(json, "sender_name");
        std::string content    = ExtractJsonStringEscaped(json, "content");
        std::string avatarPath = ExtractJsonStringEscaped(json, "avatar_path");
        qint64      timestamp  = (qint64)ExtractJsonNumber(json, "timestamp");

        blog(LOG_INFO, "[feeds] chat_message id=%s", messageId.c_str());

        // Populate the avatar cache up-front on the IPC thread so the
        // chat popup source (coming in a later Phase 2 commit) can hit
        // the cache synchronously. Return value unused in this commit.
        (void)GetAvatarForSender(senderId, avatarPath);

        QString qSender  = QString::fromStdString(senderName);
        QString qContent = QString::fromStdString(content);

        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
            [qSender, qContent, timestamp]() {
                if (g_chatDock)
                    g_chatDock->AppendMessage(qSender, qContent, timestamp);
            });
    });

    feeds::RegisterMessageHandler("active_speaker_changed",
    [](const std::string& json) {
        g_activeSpeakerUserId =
            (unsigned int)ExtractJsonNumber(json, "participant_id");
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

    feeds::RegisterMessageHandler("engine_log", [](const std::string& json) {
        std::string message = ExtractJsonString(json, "message");
        blog(LOG_INFO, "[engine] %s", message.c_str());
    });

    feeds::RegisterMessageHandler("engine_error", [](const std::string& json) {
        std::string code    = ExtractJsonString(json, "code");
        std::string message = ExtractJsonString(json, "message");
        blog(LOG_ERROR, "[engine] %s: %s", code.c_str(), message.c_str());
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

static void OnSourceCreated(void* /*data*/, calldata_t* cd) {
    obs_source_t* source = (obs_source_t*)calldata_ptr(cd, "source");
    if (!source) return;

    const char* id = obs_source_get_id(source);
    if (!id) return;

    // Only apply to Feeds source types.
    const bool isFeedsSource =
        strcmp(id, "zoom_participant_source") == 0 ||
        strcmp(id, "zoom_screenshare_source") == 0;
    if (!isFeedsSource) return;

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

    std::thread([weak]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        obs_source_t* strong = obs_weak_source_get_source(weak);
        if (strong) {
            ApplyFeedsBoundsToSceneItem(strong);
            obs_source_release(strong);
        }
        obs_weak_source_release(weak);
    }).detach();
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

    signal_handler_t* sh = obs_get_signal_handler();
    if (sh) {
        signal_handler_connect(sh, "source_create", OnSourceCreated, nullptr);
    }

    RegisterProtocolHandler();

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
    }
    if (g_updateCheckThread.joinable()) g_updateCheckThread.join();
    feeds::StopEngine();

    {
        std::lock_guard<std::mutex> lock(g_avatarCacheMutex);
        g_avatarCache.clear();
        g_fallbackAvatar = QImage();
    }
}
