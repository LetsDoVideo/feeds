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
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include <windows.h>

#include <QMainWindow>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QInputDialog>
#include <QLineEdit>
#include <QRegularExpression>
#include <QTimer>

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

    QStringList options;
    QString pmiOption = "My Personal Meeting Room (PMI)";
    if (!g_userPMI.empty())
        pmiOption += " - " + QString::fromStdString(g_userPMI);
    options << pmiOption
            << "Join by Meeting Number or Link";

    bool ok = false;
    QString choice = QInputDialog::getItem(
        mainWindow, "Join Zoom Meeting",
        "How would you like to join?",
        options, 0, false, &ok);
    if (!ok) return;

    QString input;
    QString password;
    bool    isPmi = false;

    if (choice.startsWith("My Personal")) {
        if (g_userPMI.empty()) {
            MessageBoxA(NULL,
                "Could not retrieve your Personal Meeting Room ID.\n"
                "Please use Join by Meeting Number instead.",
                "Feeds", MB_OK | MB_ICONWARNING);
            return;
        }
        input = QString::fromStdString(g_userPMI);
        isPmi = true;
        bool okPwd = false;
        password = QInputDialog::getText(
            mainWindow, "Meeting Password",
            "Enter your PMI password (leave blank if none):",
            QLineEdit::Normal, "", &okPwd);
        if (!okPwd) return;
    } else {
        bool okInput = false;
        input = QInputDialog::getText(
            mainWindow, "Join Zoom Meeting",
            "Enter your Zoom Meeting number or link:",
            QLineEdit::Normal, "", &okInput);
        if (!okInput || input.trimmed().isEmpty()) return;
        input = input.trimmed();

        bool okPwd = false;
        password = QInputDialog::getText(
            mainWindow, "Meeting Password",
            "Enter meeting password (leave blank if none):",
            QLineEdit::Normal, "", &okPwd);
        if (!okPwd) return;
    }

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

    std::string msg = "{\"type\":\"join_meeting\","
                      "\"input\":\"" + jsonEscape(input) + "\","
                      "\"password\":\"" + jsonEscape(password) + "\","
                      "\"is_pmi\":" + std::string(isPmi ? "true" : "false") + "}";
    feeds::SendToEngine(msg);

    if (g_connectAction) g_connectAction->setEnabled(false);
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
        std::string aboutText = "Feeds v1.0\n";
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
        int maxFeeds = GetMaxFeedsForTier();
        std::string msg;
        const char* title;
        if (g_currentTier >= 3) {
            msg = "You've reached the " + std::to_string(maxFeeds) +
                  "-feed limit for your tier (Broadcaster).\n\n"
                  "This is the current maximum. If you need more, "
                  "please contact support@letsdovideo.com.";
            title = "Feeds - Maximum Feeds Reached";
        } else {
            msg = "Your current tier allows a maximum of " +
                  std::to_string(maxFeeds) +
                  " participant feed(s).\n\nUpgrade your plan at:\n"
                  "https://marketplace.zoom.us";
            title = "Feeds - Upgrade Required";
        }
        MessageBoxA(NULL, msg.c_str(), title, MB_OK | MB_ICONINFORMATION);
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
static obs_properties_t* zp_properties(void* data) {
    (void)data;

    if (g_isInMeeting)
        feeds::SendToEngine("{\"type\":\"get_participants\"}");

    obs_properties_t* props = obs_properties_create();
    obs_properties_add_text(props, "ver_label", "Feeds (v1.0)", OBS_TEXT_INFO);

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
    obs_property_list_add_int(list, "--- Select Participant ---", 0);
    obs_property_list_add_int(list, "[Active Speaker]", 1);

    if (g_isInMeeting) {
        std::lock_guard<std::mutex> lock(g_participantsMutex);
        for (const auto& p : g_cachedParticipants) {
            if (g_cachedMyUserId != 0 && p.id == g_cachedMyUserId) continue;
            obs_property_list_add_int(list, p.name.c_str(), (long long)p.id);
        }
    }

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
        if (s && !s->mapping) OpenShareSharedMemory(s);
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
    (void)data;
    obs_properties_t* props = obs_properties_create();
    obs_properties_add_text(props, "ver_label",
                            "Feeds - Screenshare (v1.0)", OBS_TEXT_INFO);

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
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            g_isInMeeting = true;
            if (g_connectAction) g_connectAction->setEnabled(false);
            RefreshAllSourceProperties();
        });
    });

    feeds::RegisterMessageHandler("meeting_failed", [](const std::string& json) {
        int code         = (int)ExtractJsonNumber(json, "code");
        std::string msg  = ExtractJsonString(json, "message");
        blog(LOG_ERROR, "[feeds] meeting_failed: code=%d, msg=%s",
             code, msg.c_str());

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

// Return the advertised native size of the source. OBS calls this to
// determine how to lay out the source in scenes. By returning a constant
// rather than letting OBS derive the size from per-frame dimensions, we
// prevent Zoom's dynamic resolution changes from resizing the source in
// the user's scene. The actual frame pixels are still rendered at their
// native resolution; OBS's compositor scales them into the 1920x1080
// bounding box we advertise here.
//
// This matches the behavior of other well-behaved OBS async sources
// (webcam, NDI, Zoom ISO) which all report a stable native size despite
// receiving frames at varying resolutions.
static uint32_t zp_get_width(void* data) {
    (void)data;
    return feeds_shared::MAX_FRAME_WIDTH;
}

static uint32_t zp_get_height(void* data) {
    (void)data;
    return feeds_shared::MAX_FRAME_HEIGHT;
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
    zoom_participant_info.get_width      = zp_get_width;
    zoom_participant_info.get_height     = zp_get_height;
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

    RegisterProtocolHandler();

    feeds::StartEngine();
    RegisterEngineHandlers();

    obs_frontend_add_event_callback([](enum obs_frontend_event event, void*) {
        if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
            SetupPluginMenu();
        }
    }, nullptr);

    return true;
}

void obs_module_unload(void) {
    feeds::StopEngine();
}
