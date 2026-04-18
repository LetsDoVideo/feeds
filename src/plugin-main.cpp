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
//   - Cache state (user info, participant list, meeting status) received
//     from the engine, so OBS's per-source properties UI can read it
//     synchronously without an IPC round-trip

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <windows.h>

#include <QMainWindow>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QInputDialog>
#include <QLineEdit>
#include <QRegularExpression>
#include <QTimer>

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
static QAction* g_loginAction      = nullptr;
static QAction* g_logoutAction     = nullptr;
static QAction* g_connectAction    = nullptr;
static QAction* g_disconnectAction = nullptr;

// ---------------------------------------------------------------------------
// Globals — cached state from engine
// ---------------------------------------------------------------------------
static bool g_isLoggedIn          = false;
static bool g_isInMeeting         = false;   // set true on meeting_joined
static bool g_rawLiveStreamGranted = false;  // set true on raw_livestream_granted
static bool g_pendingMeetingJoin   = false;  // Connect-while-not-logged-in queue

static std::string g_userDisplayName;
static std::string g_userPMI;
static int         g_currentTier = 0;

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
// Tier → limits
// 0 = Free (1 feed), 1 = Basic (3), 2 = Streamer (5), 3 = Broadcaster (8)
// ---------------------------------------------------------------------------
static int GetMaxFeedsForTier() {
    switch (g_currentTier) {
        case 1:  return 3;
        case 2:  return 5;
        case 3:  return 8;
        default: return 1;
    }
}

// ---------------------------------------------------------------------------
// Forward declarations for menu-action handlers
// ---------------------------------------------------------------------------
void OnLoginClick();
void OnLogoutClick();
void OnConnectClick();
void OnDisconnectClick();

// ---------------------------------------------------------------------------
// JSON helpers (tiny, just for what the plugin needs to read)
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

// Refresh all open source properties panels so OBS re-queries them
// against the updated cache. Called after any state change that would
// affect what sources display.
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
// Menu action: Login
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

// ---------------------------------------------------------------------------
// Menu action: Logout
// ---------------------------------------------------------------------------
void OnLogoutClick() {
    if (!g_isLoggedIn) {
        MessageBoxA(NULL, "You are not currently logged in to Zoom.",
                    "Feeds - Logout", MB_OK | MB_ICONINFORMATION);
        return;
    }
    feeds::SendToEngine("{\"type\":\"logout\"}");
}

// ---------------------------------------------------------------------------
// Menu action: Connect to Meeting
// Preserves v1.0.0 UX exactly: PMI-or-number dialog, then password prompt.
// ---------------------------------------------------------------------------
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
            "Use 'Disconnect from Meeting' to leave first.",
            "Feeds - Already Connected", MB_OK | MB_ICONINFORMATION);
        return;
    }

    QMainWindow* mainWindow = (QMainWindow*)obs_frontend_get_main_window();

    // Dialog 1: choose PMI or other meeting
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

    // Build join_meeting IPC message. Engine does the ZAK fetch and the
    // actual SDK Join call.
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

    // Disable Connect while we wait for the join to either succeed or fail.
    if (g_connectAction) g_connectAction->setEnabled(false);
}

// ---------------------------------------------------------------------------
// Menu action: Disconnect from Meeting
// ---------------------------------------------------------------------------
void OnDisconnectClick() {
    if (!g_isInMeeting) return;
    feeds::SendToEngine("{\"type\":\"leave_meeting\"}");
}

// ---------------------------------------------------------------------------
// Menu setup
// ---------------------------------------------------------------------------
void SetupPluginMenu() {
    QMainWindow* mainWindow = (QMainWindow*)obs_frontend_get_main_window();
    QMenuBar*    menuBar    = mainWindow->menuBar();
    QMenu*       feedsMenu  = new QMenu("Feeds", menuBar);
    menuBar->addMenu(feedsMenu);

    g_loginAction      = feedsMenu->addAction("Login to Zoom");
    g_logoutAction     = feedsMenu->addAction("Logout of Zoom");
    feedsMenu->addSeparator();
    g_connectAction    = feedsMenu->addAction("Connect to Zoom Meeting...");
    g_disconnectAction = feedsMenu->addAction("Disconnect from Meeting");
    feedsMenu->addSeparator();
    QAction* aboutAction = feedsMenu->addAction("About / Tier Status");

    g_logoutAction->setEnabled(false);
    g_connectAction->setEnabled(false);
    g_disconnectAction->setEnabled(false);

    QObject::connect(g_loginAction,      &QAction::triggered, []() { OnLoginClick(); });
    QObject::connect(g_logoutAction,     &QAction::triggered, []() { OnLogoutClick(); });
    QObject::connect(g_connectAction,    &QAction::triggered, []() { OnConnectClick(); });
    QObject::connect(g_disconnectAction, &QAction::triggered, []() { OnDisconnectClick(); });
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
// Source type callbacks — video rendering is Phase 6. These stubs manage
// source lifetime and provide the properties UI.
// ---------------------------------------------------------------------------

// Per-source data — just enough to identify the source for future IPC
// subscription. Phase 6 will add the D3D11 shared texture state.
struct ZpSourceData {
    obs_source_t* source          = nullptr;
    unsigned int  current_user_id = 0;
};

static void* zp_create(obs_data_t* settings, obs_source_t* source) {
    (void)settings;

    if (g_activeParticipantSources >= GetMaxFeedsForTier()) {
        std::string msg = "Your current tier allows a maximum of " +
                          std::to_string(GetMaxFeedsForTier()) +
                          " participant feed(s).\n\nUpgrade your plan at:\n"
                          "https://marketplace.zoom.us";
        MessageBoxA(NULL, msg.c_str(), "Feeds - Upgrade Required",
                    MB_OK | MB_ICONINFORMATION);
        return nullptr;
    }

    obs_source_set_async_unbuffered(source, true);
    ZpSourceData* data = new ZpSourceData();
    data->source = source;
    g_activeParticipantSources++;
    return data;
}

static void zp_destroy(void* data) {
    if (!data) return;
    g_activeParticipantSources--;
    delete static_cast<ZpSourceData*>(data);
}

static void zp_update(void* data, obs_data_t* settings) {
    if (!data) return;
    ZpSourceData* src = static_cast<ZpSourceData*>(data);
    src->current_user_id =
        (unsigned int)obs_data_get_int(settings, "participant_id");
    // Phase 6 will send participant_source_subscribe here.
}

// ---------------------------------------------------------------------------
// Participant source properties — full v1.0.0 parity, reading from cache.
// Also sends get_participants on every open so the list stays fresh as
// people join/leave (effectively live updates without live SDK events).
// ---------------------------------------------------------------------------
static obs_properties_t* zp_properties(void* data) {
    (void)data;

    // Fire-and-forget refresh. Response arrives async; if the list has
    // changed, we'll call obs_source_update_properties again and OBS
    // will re-invoke this function with fresh data.
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

    // The participant dropdown — always present. Populated from cache.
    obs_property_t* list = obs_properties_add_list(
        props, "participant_id", "Select Participant",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(list, "--- Select Participant ---", 0);
    obs_property_list_add_int(list, "[Active Speaker]", 1);

    if (g_isInMeeting) {
        std::lock_guard<std::mutex> lock(g_participantsMutex);
        for (const auto& p : g_cachedParticipants) {
            // Skip ourselves — matches v1.0.0 "filter by ID, not name"
            if (g_cachedMyUserId != 0 && p.id == g_cachedMyUserId) continue;
            obs_property_list_add_int(list, p.name.c_str(), (long long)p.id);
        }
    }

    return props;
}

// ---------------------------------------------------------------------------
// Screenshare source
// ---------------------------------------------------------------------------
static void* zs_create(obs_data_t* settings, obs_source_t* source) {
    (void)settings;
    obs_source_set_async_unbuffered(source, true);
    // Return a sentinel non-null pointer — we don't need per-source state yet
    return bmalloc(1);
}

static void zs_destroy(void* data) {
    if (data) bfree(data);
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
        std::string status_text = (g_activeSharerUserId != 0)
            ? "Status: Receiving screenshare"
            : "Status: Connected - waiting for screenshare";
        obs_properties_add_text(props, "status_label", status_text.c_str(),
                                OBS_TEXT_INFO);
    }

    return props;
}

// ---------------------------------------------------------------------------
// Source info structs
// ---------------------------------------------------------------------------
struct obs_source_info zoom_participant_info = {};
struct obs_source_info zoom_screenshare_info = {};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("feeds", "en-US")

// ---------------------------------------------------------------------------
// Protocol handler registration (for ldvfeeds:// URLs)
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
// IPC message handlers — all the engine → plugin messages.
//
// These run on the pipe reader thread. Any UI manipulation (menu actions,
// message boxes, property refreshes) must be marshaled to the Qt main
// thread via QTimer::singleShot.
// ---------------------------------------------------------------------------
static void RegisterEngineHandlers() {

    // -----------------------------------------------------------------------
    // Authentication
    // -----------------------------------------------------------------------
    feeds::RegisterMessageHandler("login_succeeded", [](const std::string& json) {
        // Populate cache from engine's post-auth user-info fetch.
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
        blog(LOG_INFO, "[feeds] sdk_authenticated: enabling Connect menu");
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            g_isLoggedIn = true;
            if (g_loginAction)   g_loginAction->setEnabled(false);
            if (g_logoutAction)  g_logoutAction->setEnabled(true);
            if (g_connectAction) g_connectAction->setEnabled(true);
            RefreshAllSourceProperties();

            // If the user clicked Connect while not logged in, fulfill it now.
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

            if (g_loginAction)      g_loginAction->setEnabled(true);
            if (g_logoutAction)     g_logoutAction->setEnabled(false);
            if (g_connectAction)    g_connectAction->setEnabled(false);
            if (g_disconnectAction) g_disconnectAction->setEnabled(false);
            RefreshAllSourceProperties();

            MessageBoxA(NULL, "You have been logged out of Zoom.",
                        "Feeds - Logout", MB_OK | MB_ICONINFORMATION);
        });
    });

    feeds::RegisterMessageHandler("session_expired", [](const std::string&) {
        blog(LOG_WARNING, "[feeds] session_expired");
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
            if (g_loginAction)      g_loginAction->setEnabled(true);
            if (g_logoutAction)     g_logoutAction->setEnabled(false);
            if (g_connectAction)    g_connectAction->setEnabled(false);
            if (g_disconnectAction) g_disconnectAction->setEnabled(false);
            RefreshAllSourceProperties();

            MessageBoxA(NULL,
                "Your Zoom login has expired and could not be renewed.\n\n"
                "Please log in again.",
                "Feeds - Session Expired", MB_OK | MB_ICONWARNING);
        });
    });

    // -----------------------------------------------------------------------
    // Meeting
    // -----------------------------------------------------------------------
    feeds::RegisterMessageHandler("meeting_joined", [](const std::string& json) {
        std::string mn = ExtractJsonString(json, "meeting_number");
        blog(LOG_INFO, "[feeds] meeting_joined: %s", mn.c_str());
        try { g_currentMeetingNumber = std::stoull(mn); }
        catch (...) { g_currentMeetingNumber = 0; }
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            g_isInMeeting = true;
            if (g_connectAction)    g_connectAction->setEnabled(false);
            if (g_disconnectAction) g_disconnectAction->setEnabled(true);
            RefreshAllSourceProperties();
        });
    });

    feeds::RegisterMessageHandler("meeting_failed", [](const std::string& json) {
        int code = (int)ExtractJsonNumber(json, "code");
        blog(LOG_ERROR, "[feeds] meeting_failed: code=%d", code);

        // V1.0.0 error-code-to-message mapping. These are the SDK
        // MEETING_FAIL_* constants, as integers.
        std::string msg;
        switch (code) {
            case 1901: // MEETING_FAIL_PASSWORD_ERR
                msg = "Incorrect meeting password. Please try again.";
                break;
            case 1903: // MEETING_FAIL_MEETING_NOT_EXIST
                msg = "Meeting not found. Please check the meeting number or link.";
                break;
            case 1005: // MEETING_FAIL_CONNECTION_ERR (varies; fall-through fine)
                msg = "Connection error. Please check your internet connection and try again.";
                break;
            case 1143: // MEETING_FAIL_HOST_DISALLOW_OUTSIDE_USER_JOIN
                msg = "The host has disabled external participants from joining this meeting.";
                break;
            case 1144: // MEETING_FAIL_UNABLE_TO_JOIN_EXTERNAL_MEETING
                msg = "This app must be published on the Zoom Marketplace before joining external meetings.";
                break;
            case 1145: // MEETING_FAIL_APP_CAN_NOT_ANONYMOUS_JOIN_MEETING
                msg = "This meeting requires you to be logged in to Zoom. Please log in and try again.";
                break;
            case 1146: // MEETING_FAIL_BLOCKED_BY_ACCOUNT_ADMIN
                msg = "Your Zoom account administrator has blocked this application.";
                break;
            case 1147: // MEETING_FAIL_NEED_SIGN_IN_FOR_PRIVATE_MEETING
                msg = "This is a private meeting. Please log in to Zoom and try again.";
                break;
            default:
                msg = "Failed to join meeting. Error code: " + std::to_string(code);
                break;
        }

        // NOTE: The numeric constants above are illustrative; the SDK
        // assigns them and v1.0.0 used the named enums. Because we don't
        // pull the SDK header into the plugin anymore, we use the integer
        // values the engine sends. If Zoom changes these, the mapping
        // falls through to the default branch which is harmless — just
        // a less-friendly message. Revisit if any user reports the wrong
        // message for a known failure cause.

        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(),
            [msg]() {
                if (g_connectAction && g_isLoggedIn)
                    g_connectAction->setEnabled(true);
                if (g_disconnectAction)
                    g_disconnectAction->setEnabled(false);
                MessageBoxA(NULL, msg.c_str(), "Feeds - Join Failed",
                            MB_OK | MB_ICONERROR);
            });
    });

    feeds::RegisterMessageHandler("meeting_left", [](const std::string&) {
        blog(LOG_INFO, "[feeds] meeting_left");
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
            if (g_disconnectAction)
                g_disconnectAction->setEnabled(false);
            RefreshAllSourceProperties();
        });
    });

    feeds::RegisterMessageHandler("raw_livestream_granted", [](const std::string&) {
        blog(LOG_INFO, "[feeds] raw_livestream_granted");
        g_rawLiveStreamGranted = true;
        // RefreshAllSourceProperties will be triggered by participant_list_changed
        // which follows immediately.
    });

    feeds::RegisterMessageHandler("raw_livestream_timeout", [](const std::string&) {
        blog(LOG_WARNING, "[feeds] raw_livestream_timeout - host did not approve");
        // Future enhancement: show user-visible notification.
        // For Phase 5 we just log it.
    });

    // -----------------------------------------------------------------------
    // Participants
    // -----------------------------------------------------------------------
    feeds::RegisterMessageHandler("participant_list_changed",
    [](const std::string& json) {
        g_cachedMyUserId = (unsigned int)ExtractJsonNumber(json, "my_user_id");

        // Parse the participants array. We do this by finding each
        // "{\"id\":N,\"name\":\"...\"}" object and extracting fields.
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

        {
            std::lock_guard<std::mutex> lock(g_participantsMutex);
            g_cachedParticipants = std::move(newList);
        }

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
        // No UI refresh needed for the dropdown — [Active Speaker] is a
        // fixed entry. Phase 6 uses this for the renderer subscription.
    });

    feeds::RegisterMessageHandler("share_status_changed",
    [](const std::string& json) {
        g_activeSharerUserId =
            (unsigned int)ExtractJsonNumber(json, "sharer_user_id");
        blog(LOG_INFO, "[feeds] share_status_changed: sharer_user_id=%u",
             g_activeSharerUserId);
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            RefreshAllSourceProperties();
        });
    });

    feeds::RegisterMessageHandler("token_refreshed", [](const std::string&) {
        blog(LOG_INFO, "[feeds] token_refreshed (engine refreshed silently)");
        // No-op plugin-side; the engine already persisted the new token.
    });

    // -----------------------------------------------------------------------
    // Diagnostics
    // -----------------------------------------------------------------------
    feeds::RegisterMessageHandler("engine_log", [](const std::string& json) {
        std::string message = ExtractJsonString(json, "message");
        blog(LOG_INFO, "[engine] %s", message.c_str());
    });

    feeds::RegisterMessageHandler("engine_error", [](const std::string& json) {
        std::string code    = ExtractJsonString(json, "code");
        std::string message = ExtractJsonString(json, "message");
        blog(LOG_ERROR, "[engine] %s: %s", code.c_str(), message.c_str());
    });

    feeds::RegisterMessageHandler("engine_ready", [](const std::string& json) {
        std::string version = ExtractJsonString(json, "version");
        blog(LOG_INFO, "[feeds] engine_ready: version=%s", version.c_str());
    });
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
