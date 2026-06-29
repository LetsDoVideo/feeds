// engine-sdk.cpp — Zoom Meeting SDK integration for FeedsEngine.

#include <windows.h>
#include <wincred.h>
#include "zoom_sdk.h"
#include "auth_service_interface.h"
#include "engine-shared.h"
#include <string>
#include <thread>
#include <vector>
#include <functional>
#include <mutex>
#include <cstdio>

extern void LogToFile(const char* msg);  // forwards at DEBUG
extern void LogInfo(const char* msg);
extern void LogWarn(const char* msg);
extern void LogError(const char* msg);
extern bool SendToPlugin(const std::string& json);

namespace feeds_engine {

// From engine-api.cpp
bool FetchUserInfo();
void FetchAndApplyEntitlement();
const std::string& GetUserDisplayName();
const std::string& GetUserPMI();
const std::string& GetUserEmail();
int                GetCurrentTier();

// From engine-meeting.cpp
bool InitializeMeetingSession();

bool AuthenticateSDK();  // defined below

static bool g_sdkInitialized = false;
static ZOOM_SDK_NAMESPACE::IAuthService* g_authService = nullptr;

// ---------------------------------------------------------------------------
// Lazy SDK bring-up state.
//
// As of the idle-footprint change, the engine no longer calls InitSDK at
// startup. A logged-in-but-not-connected Feeds holds no SDK and no Zoom
// authenticated session, so it adds no steady load. The SDK is brought up
// the first time the user actually connects (join/create/event), gated
// through EnsureSdkUpThen below.
//
// g_sdkAuthenticated  — true once onAuthenticationReturn(SUCCESS) has fired
//                       and the meeting service exists; the SDK is ready to
//                       Join. Reset on logout so a later connect re-auths.
// g_sdkBringupInProgress — true between the first EnsureSdkUpThen that kicks
//                       off init+auth and the auth callback resolving; keeps
//                       us from posting WM_FEEDS_INIT_SDK more than once.
// g_pendingSdkActions — join actions queued while the SDK comes up; drained
//                       on auth success, failed (meeting_failed each) on auth
//                       failure. All three guarded by g_sdkStateMutex.
// ---------------------------------------------------------------------------
static std::mutex                         g_sdkStateMutex;
static bool                               g_sdkAuthenticated     = false;
static bool                               g_sdkBringupInProgress = false;
static std::vector<std::function<void()>> g_pendingSdkActions;

static std::string LoadAccessToken() {
    PCREDENTIALA pCred = nullptr;
    if (CredReadA("Feeds_AccessToken", CRED_TYPE_GENERIC, 0, &pCred)) {
        std::string token((char*)pCred->CredentialBlob, pCred->CredentialBlobSize);
        CredFree(pCred);
        return token;
    }
    return "";
}

// Forward decls — defined after the listener class.
static void DrainPendingSdkActions();
static void FailPendingSdkActions(const char* reason);

class EngineAuthListener : public ZOOM_SDK_NAMESPACE::IAuthServiceEvent {
public:
    // Fires on the main thread once the lazy bring-up's SDKAuth resolves.
    // By this point the plugin is already in the logged-in state — the
    // login_succeeded / sdk_authenticated announce happened at the REST-only
    // session restore (or fresh login), not here — so this callback's only
    // jobs are to stand up the meeting service and release the join(s) that
    // were queued while the SDK was coming up.
    virtual void onAuthenticationReturn(
        ZOOM_SDK_NAMESPACE::AuthResult ret) override {
        if (ret != ZOOM_SDK_NAMESPACE::AUTHRET_SUCCESS) {
            char msg[256];
            sprintf_s(msg, "SDK: onAuthenticationReturn FAILED: %d", (int)ret);
            LogError(msg);
            char resp[256];
            sprintf_s(resp, "{\"type\":\"sdk_auth_failed\",\"code\":%d}", (int)ret);
            SendToPlugin(resp);
            // Release every queued join with a meeting_failed so the plugin's
            // Connect menu re-enables (sdk_auth_failed alone doesn't clear the
            // per-join "joining" state). Resets the bring-up flags so a later
            // connect can retry.
            FailPendingSdkActions("Zoom SDK authentication failed.");
            return;
        }

        LogInfo("SDK: onAuthenticationReturn SUCCESS");

        // Create the meeting service now that auth succeeded. Singleton —
        // only created once per process. Done on this (main) thread because
        // the SDK expects its services created on the pump thread.
        InitializeMeetingSession();

        // Mark the SDK ready and release the queued join(s). Draining runs
        // the join actions on a background thread (they do network I/O —
        // ZAK fetch — and we must not block the main message pump, same
        // reason the post-auth fetch used a thread before).
        DrainPendingSdkActions();
    }

    virtual void onLoginReturnWithReason(
        ZOOM_SDK_NAMESPACE::LOGINSTATUS ret,
        ZOOM_SDK_NAMESPACE::IAccountInfo* pAccountInfo,
        ZOOM_SDK_NAMESPACE::LoginFailReason reason) override {}
    virtual void onLogout() override {}
    virtual void onZoomIdentityExpired() override {}
    virtual void onZoomAuthIdentityExpired() override {}
#if defined(WIN32)
    virtual void onNotificationServiceStatus(
        ZOOM_SDK_NAMESPACE::SDKNotificationServiceStatus status,
        ZOOM_SDK_NAMESPACE::SDKNotificationServiceError error) override {}
#endif
};

static EngineAuthListener g_authListener;

bool AuthenticateSDK() {
    if (!g_sdkInitialized) {
        LogError("SDK: AuthenticateSDK called but SDK not initialized");
        return false;
    }

    if (!g_authService) {
        ZOOM_SDK_NAMESPACE::SDKError err =
            ZOOM_SDK_NAMESPACE::CreateAuthService(&g_authService);
        if (err != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS || !g_authService) {
            char msg[256];
            sprintf_s(msg, "SDK: CreateAuthService failed: %d", (int)err);
            LogError(msg);
            return false;
        }
        g_authService->SetEvent(&g_authListener);
    }

    // Build the wide-string version of the client ID from the compile-time
    // macro. Done once per-process at first auth.
    static std::wstring s_clientId = []() {
        std::string a = FEEDS_ZOOM_CLIENT_ID;
        return std::wstring(a.begin(), a.end());
    }();

    ZOOM_SDK_NAMESPACE::AuthContext authContext;
    authContext.publicAppKey = s_clientId.c_str();

    ZOOM_SDK_NAMESPACE::SDKError err = g_authService->SDKAuth(authContext);
    if (err != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
        char msg[256];
        sprintf_s(msg, "SDK: SDKAuth call failed: %d", (int)err);
        LogError(msg);
        return false;
    }

    LogToFile("SDK: SDKAuth called, waiting for callback");
    return true;
}

// Pure SDK core init (InitSDK only). Idempotent — safe to call more than once;
// the actual InitSDK runs at most once per process. No longer touches tokens,
// auth, or login state: those moved to RestoreSessionFromStoredToken (the
// REST-only startup restore) and the lazy bring-up path. Must run on the
// thread that created the anchor window and pumps messages.
bool InitializeSDK() {
    if (g_sdkInitialized) return true;

    LogToFile("SDK: InitializeSDK starting");

    ZOOM_SDK_NAMESPACE::InitParam initParam;
    initParam.strWebDomain = L"https://zoom.us";

    ZOOM_SDK_NAMESPACE::SDKError err = ZOOM_SDK_NAMESPACE::InitSDK(initParam);
    if (err != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
        char msg[256];
        sprintf_s(msg, "SDK: InitSDK FAILED: %d", (int)err);
        LogError(msg);
        return false;
    }

    g_sdkInitialized = true;
    LogToFile("SDK: InitSDK SUCCESS");
    return true;
}

// ---------------------------------------------------------------------------
// Announce a restored / completed login to the plugin — WITHOUT bringing up
// the SDK. Fetches user info + entitlement over REST, then sends
// login_succeeded (populates the plugin's cache) followed by sdk_authenticated.
//
// sdk_authenticated here is deliberately reused with the redefined meaning
// "credentials are valid and Feeds is ready to connect" — NOT "the Zoom SDK
// is initialized and authenticated." The SDK isn't up at this point and won't
// be until the first connect (see EnsureSdkUpThen). The plugin's handler only
// needs the "ready to connect" semantic (enable Connect, flip logged-in), so
// no new message type is required. The matching note lives at the plugin's
// sdk_authenticated handler.
//
// Runs synchronously on the caller's thread — callers that are on the main
// thread must invoke it from a background thread (FetchUserInfo does network
// I/O). Order matters: FetchUserInfo first (its 401→refresh populates the
// access token used by the tier query), then FetchAndApplyEntitlement.
static void AnnounceLoginSucceeded() {
    bool gotUser = FetchUserInfo();
    FetchAndApplyEntitlement();

    if (!gotUser) {
        LogWarn("SDK: session-restore user-info fetch failed");
    }

    // Build the login_succeeded message with everything the plugin needs to
    // populate its cache.
    const std::string& name = GetUserDisplayName();
    const std::string& pmi  = GetUserPMI();
    int tier = GetCurrentTier();

    // Escape the display name defensively. We don't expect quotes or
    // backslashes, but non-ASCII names are common and we want to be safe.
    std::string escName;
    for (unsigned char c : name) {
        if (c == '"')       escName += "\\\"";
        else if (c == '\\') escName += "\\\\";
        else if (c < 0x20) { /* drop control chars */ }
        else                escName += (char)c;
    }

    char resp[1024];
    sprintf_s(resp,
        "{\"type\":\"login_succeeded\","
        "\"display_name\":\"%s\","
        "\"pmi\":\"%s\","
        "\"tier\":%d}",
        escName.c_str(), pmi.c_str(), tier);
    SendToPlugin(resp);

    // Then the UI-state flip. Order matters: login_succeeded first (so the
    // plugin's cache is populated), then sdk_authenticated ("ready to
    // connect"). See the semantic note above.
    SendToPlugin("{\"type\":\"sdk_authenticated\"}");
}

// Called by engine-oauth.cpp's LoginThreadFunc after a fresh OAuth login saves
// new tokens. It's already on the OAuth background thread, so announce inline.
void AnnounceLoginSucceededFromLoginThread() {
    AnnounceLoginSucceeded();
}

// ---------------------------------------------------------------------------
// REST-only session restore — replaces the old startup InitializeSDK() call
// in WinMain. If a token is stored, restore the logged-in appearance over REST
// (no SDK, no Zoom authenticated session); otherwise tell the plugin there's
// nothing to restore. Either way the SDK stays down until the first connect.
// ---------------------------------------------------------------------------
void RestoreSessionFromStoredToken() {
    std::string token = LoadAccessToken();
    if (token.empty()) {
        LogToFile("SDK: no access token stored, waiting for user login");
        // Not a "failure" in the usual sense — the user simply hasn't logged
        // in yet — but the plugin needs the signal to flip
        // g_loginAttemptCompleted so its source-creation callbacks surface the
        // "Please log in" prompt instead of silently blocking. The plugin
        // special-cases this error code to skip its error dialog.
        SendToPlugin("{\"type\":\"login_failed\",\"error\":\"no_stored_token\"}");
        return;
    }

    LogToFile("SDK: stored token found, restoring session over REST (SDK stays down)");
    // Background thread: AnnounceLoginSucceeded does network I/O and WinMain
    // must get back to its message pump for the engine to stay responsive.
    std::thread([]() { AnnounceLoginSucceeded(); }).detach();
}

// ---------------------------------------------------------------------------
// Lazy SDK bring-up.
//
// EnsureSdkUpThen is the gate the three join handlers call first. If the SDK
// is already authenticated it returns false and the caller proceeds inline
// (the steady-state path once connected). Otherwise it queues `action`, kicks
// off init+auth on the main thread (once), and returns true so the caller
// returns immediately; the queued action re-runs after onAuthenticationReturn.
// ---------------------------------------------------------------------------
bool EnsureSdkUpThen(std::function<void()> action) {
    std::lock_guard<std::mutex> lock(g_sdkStateMutex);
    if (g_sdkAuthenticated)
        return false;  // SDK is up — caller proceeds inline now.

    g_pendingSdkActions.push_back(std::move(action));

    if (!g_sdkBringupInProgress) {
        g_sdkBringupInProgress = true;
        LogInfo("SDK: first connect — bringing up Zoom SDK (init + auth)");
        // InitSDK/SDKAuth must run on the main (pump) thread; marshal there.
        if (g_anchorWnd) {
            PostMessageW(g_anchorWnd, WM_FEEDS_INIT_SDK, 0, 0);
        } else {
            LogError("SDK: no anchor window to marshal SDK init to");
        }
    }
    return true;  // queued — caller returns; we'll run it after auth.
}

// Main-thread bring-up, invoked by EngineWndProc on WM_FEEDS_INIT_SDK.
// Runs InitSDK then SDKAuth; success/failure is reported asynchronously via
// onAuthenticationReturn. If either synchronous step fails, fail the queued
// join(s) right here so Connect re-enables.
void BringUpSdkOnMainThread() {
    if (!InitializeSDK()) {
        SendToPlugin("{\"type\":\"sdk_auth_failed\",\"code\":-1}");
        FailPendingSdkActions("Could not initialize the Zoom SDK.");
        return;
    }
    if (!AuthenticateSDK()) {
        SendToPlugin("{\"type\":\"sdk_auth_failed\",\"code\":-1}");
        FailPendingSdkActions("Could not start Zoom SDK authentication.");
        return;
    }
    // Otherwise wait for onAuthenticationReturn to drain or fail the queue.
}

// Auth succeeded: flip the ready flag and run the queued join(s). The actions
// re-enter the join handlers, which now sail past EnsureSdkUpThen (returns
// false) and do the real work. Run on a detached background thread so the
// network I/O inside the joins doesn't stall the main message pump.
static void DrainPendingSdkActions() {
    std::vector<std::function<void()>> actions;
    {
        std::lock_guard<std::mutex> lock(g_sdkStateMutex);
        g_sdkAuthenticated     = true;
        g_sdkBringupInProgress = false;
        actions.swap(g_pendingSdkActions);
    }
    if (actions.empty()) return;

    char buf[96];
    sprintf_s(buf, "SDK: bring-up complete, draining %zu queued join(s)",
              actions.size());
    LogInfo(buf);

    std::thread([actions = std::move(actions)]() {
        for (const auto& a : actions) a();
    }).detach();
}

// Auth (or a synchronous init step) failed: drop the queued join(s) and tell
// the plugin one meeting_failed per queued join so Connect re-enables. Resets
// the bring-up flags so the next connect retries from scratch.
static void FailPendingSdkActions(const char* reason) {
    std::vector<std::function<void()>> actions;
    {
        std::lock_guard<std::mutex> lock(g_sdkStateMutex);
        g_sdkAuthenticated     = false;
        g_sdkBringupInProgress = false;
        actions.swap(g_pendingSdkActions);
    }

    // One meeting_failed per queued join. We never run the join actions on
    // failure — the SDK isn't up — so synthesize the failures from the count.
    std::string esc;
    for (const char* p = reason ? reason : ""; *p; ++p) {
        if (*p == '"')       esc += "\\\"";
        else if (*p == '\\') esc += "\\\\";
        else                 esc += *p;
    }
    std::string failMsg = "{\"type\":\"meeting_failed\",\"code\":-1,"
                          "\"message\":\"" + esc + "\"}";
    for (size_t i = 0; i < actions.size(); ++i)
        SendToPlugin(failMsg);
}

// Reset the lazy bring-up so the next connect re-runs init+auth. Called from
// HandleLogout: after the SDK logs out, the prior SDKAuth no longer counts as
// "ready to connect," so a subsequent login + connect must auth again.
void ResetSdkBringupState() {
    std::lock_guard<std::mutex> lock(g_sdkStateMutex);
    g_sdkAuthenticated     = false;
    g_sdkBringupInProgress = false;
    g_pendingSdkActions.clear();
}

} // namespace feeds_engine
