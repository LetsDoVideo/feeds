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

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <string>
#include <vector>
#include <functional>
#include <windows.h>

#include <QMainWindow>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QInputDialog>
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
// Globals
// ---------------------------------------------------------------------------
static QAction* g_loginAction   = nullptr;
static QAction* g_logoutAction  = nullptr;
static QAction* g_connectAction = nullptr;
static bool     g_isLoggedIn    = false;
static int      g_currentTier   = 0;

// Tier info
// 0 = Free (1 feed), 1 = Basic (3 feeds), 2 = Streamer (5 feeds), 3 = Broadcaster (8 feeds)
static int GetMaxFeedsForTier() {
    switch (g_currentTier) {
        case 1:  return 3;
        case 2:  return 5;
        case 3:  return 8;
        default: return 1;
    }
}

// ---------------------------------------------------------------------------
// Menu action handlers
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
    feeds::SendToEngine("{\"type\":\"logout\"}");
}

void OnConnectClick() {
    if (!g_isLoggedIn) {
        MessageBoxA(NULL, "Please log in to Zoom first.",
                    "Feeds - Not Logged In", MB_OK | MB_ICONWARNING);
        return;
    }

    bool ok = false;
    QString meetingInput = QInputDialog::getText(
        nullptr, "Connect to Zoom Meeting",
        "Meeting Number or PMI:", QLineEdit::Normal, "", &ok);

    if (!ok || meetingInput.isEmpty())
        return;

    // Strip non-digits
    QString meetingNumber = meetingInput;
    meetingNumber.replace(QRegularExpression("[^0-9]"), "");

    std::string msg = "{\"type\":\"join_meeting\",\"meeting_number\":\"" +
                      meetingNumber.toStdString() + "\"}";
    feeds::SendToEngine(msg);
}

// ---------------------------------------------------------------------------
// Plugin menu setup
// ---------------------------------------------------------------------------
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

    g_logoutAction->setEnabled(false);
    g_connectAction->setEnabled(false);

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
        std::string aboutText = "Feeds v1.0\nTier: " + tierName;
        MessageBoxA(NULL, aboutText.c_str(), "About Feeds", MB_OK);
    });
}

// ---------------------------------------------------------------------------
// Source type callbacks — stubs for Phase 4, wired up in Phase 5/6
// ---------------------------------------------------------------------------
static void* zp_create(obs_data_t* settings, obs_source_t* source) {
    (void)settings;
    (void)source;
    blog(LOG_INFO, "[feeds] zp_create stub (Phase 5/6 will implement)");
    return bmalloc(1);
}

static void zp_destroy(void* data) {
    bfree(data);
}

static void zp_update(void* data, obs_data_t* settings) {
    (void)data;
    (void)settings;
}

static obs_properties_t* zp_properties(void* data) {
    (void)data;
    obs_properties_t* props = obs_properties_create();
    obs_properties_add_text(props, "ver_label", "Feeds (v1.0)", OBS_TEXT_INFO);

    if (!g_isLoggedIn) {
        obs_properties_add_button(props, "login_btn",
            "Not logged in to Zoom. Click to Login...",
            [](obs_properties_t*, obs_property_t*, void*) -> bool {
                OnLoginClick();
                return true;
            });
    } else {
        obs_properties_add_text(props, "status_label",
            "Logged in. Meeting features coming in next update.",
            OBS_TEXT_INFO);
    }
    return props;
}

static void* zs_create(obs_data_t* settings, obs_source_t* source) {
    (void)settings;
    (void)source;
    return bmalloc(1);
}

static void zs_destroy(void* data) {
    bfree(data);
}

static obs_properties_t* zs_properties(void* data) {
    (void)data;
    obs_properties_t* props = obs_properties_create();
    obs_properties_add_text(props, "ver_label", "Feeds - Screenshare (v1.0)",
                            OBS_TEXT_INFO);
    if (!g_isLoggedIn) {
        obs_properties_add_button(props, "login_btn",
            "Not logged in to Zoom. Click to Login...",
            [](obs_properties_t*, obs_property_t*, void*) -> bool {
                OnLoginClick();
                return true;
            });
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
// IPC message handlers — react to messages from the engine
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

static void RegisterEngineHandlers() {
    feeds::RegisterMessageHandler("login_succeeded", [](const std::string& json) {
        blog(LOG_INFO, "[feeds] login_succeeded: OAuth done, waiting for SDK auth");
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
            });
    });

    feeds::RegisterMessageHandler("sdk_authenticated", [](const std::string& json) {
        blog(LOG_INFO, "[feeds] sdk_authenticated: updating UI to logged-in state");
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            g_isLoggedIn = true;
            if (g_loginAction)   g_loginAction->setEnabled(false);
            if (g_logoutAction)  g_logoutAction->setEnabled(true);
            if (g_connectAction) g_connectAction->setEnabled(true);
        });
    });

    feeds::RegisterMessageHandler("sdk_auth_failed", [](const std::string& json) {
        blog(LOG_ERROR, "[feeds] sdk_auth_failed: %s", json.c_str());
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            MessageBoxA(NULL,
                "Zoom authentication failed. Please try logging in again.",
                "Feeds - Auth Failed", MB_OK | MB_ICONERROR);
            if (g_loginAction) g_loginAction->setEnabled(true);
        });
    });

    feeds::RegisterMessageHandler("logout_complete", [](const std::string& json) {
        blog(LOG_INFO, "[feeds] logout_complete");
        QTimer::singleShot(0, (QObject*)obs_frontend_get_main_window(), []() {
            g_isLoggedIn = false;
            if (g_loginAction)   g_loginAction->setEnabled(true);
            if (g_logoutAction)  g_logoutAction->setEnabled(false);
            if (g_connectAction) g_connectAction->setEnabled(false);
        });
    });

    feeds::RegisterMessageHandler("engine_log", [](const std::string& json) {
        std::string level = ExtractJsonString(json, "level");
        std::string message = ExtractJsonString(json, "message");
        blog(LOG_INFO, "[engine] %s", message.c_str());
    });
}

// ---------------------------------------------------------------------------
// Module load/unload
// ---------------------------------------------------------------------------
bool obs_module_load(void) {
    // Register source types so menu items always show up in OBS
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

    // Register the ldvfeeds:// URL protocol handler
    RegisterProtocolHandler();

    // Launch the engine subprocess that hosts the Zoom SDK
    feeds::StartEngine();

    // Register handlers for messages we expect from the engine
    RegisterEngineHandlers();

    // Set up the Feeds menu after OBS UI is ready
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
