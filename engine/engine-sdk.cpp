// engine-sdk.cpp — Zoom Meeting SDK integration for FeedsEngine.

#include <windows.h>
#include <wincred.h>
#include "zoom_sdk.h"
#include "auth_service_interface.h"
#include <string>
#include <thread>
#include <cstdio>

extern void LogToFile(const char* msg);
extern bool SendToPlugin(const std::string& json);

namespace feeds_engine {

// From engine-api.cpp
bool FetchUserInfo();
void FetchAndApplyEntitlement();
const std::string& GetUserDisplayName();
const std::string& GetUserPMI();
int                GetCurrentTier();

// From engine-meeting.cpp
bool InitializeMeetingSession();

static bool g_sdkInitialized = false;
static ZOOM_SDK_NAMESPACE::IAuthService* g_authService = nullptr;

static std::string LoadAccessToken() {
    PCREDENTIALA pCred = nullptr;
    if (CredReadA("Feeds_AccessToken", CRED_TYPE_GENERIC, 0, &pCred)) {
        std::string token((char*)pCred->CredentialBlob, pCred->CredentialBlobSize);
        CredFree(pCred);
        return token;
    }
    return "";
}

class EngineAuthListener : public ZOOM_SDK_NAMESPACE::IAuthServiceEvent {
public:
    virtual void onAuthenticationReturn(
        ZOOM_SDK_NAMESPACE::AuthResult ret) override {
        if (ret != ZOOM_SDK_NAMESPACE::AUTHRET_SUCCESS) {
            char msg[256];
            sprintf_s(msg, "SDK: onAuthenticationReturn FAILED: %d", (int)ret);
            LogToFile(msg);
            char resp[256];
            sprintf_s(resp, "{\"type\":\"sdk_auth_failed\",\"code\":%d}", (int)ret);
            SendToPlugin(resp);
            return;
        }

        LogToFile("SDK: onAuthenticationReturn SUCCESS");

        // Create the meeting service now so it's ready when the user
        // clicks Connect. Singleton — only created once per process.
        InitializeMeetingSession();

        // Fetch user info + entitlement on a background thread. The SDK
        // main-thread callback should not block on network I/O.
        std::thread([]() {
            bool gotUser = FetchUserInfo();
            FetchAndApplyEntitlement();

            if (!gotUser) {
                LogToFile("SDK: post-auth user-info fetch failed");
            }

            // Build the login_succeeded message with everything the plugin
            // needs to populate its cache.
            const std::string& name = GetUserDisplayName();
            const std::string& pmi  = GetUserPMI();
            int tier = GetCurrentTier();

            // Escape the display name defensively. We don't expect quotes
            // or backslashes, but non-ASCII names are common and we want
            // to be safe.
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

            // Also send sdk_authenticated for the plugin's existing handler
            // to enable the Connect menu item. Order matters: login_succeeded
            // first (so the plugin's cache is populated), then the UI-state
            // flip.
            SendToPlugin("{\"type\":\"sdk_authenticated\"}");
        }).detach();
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
        LogToFile("SDK: AuthenticateSDK called but SDK not initialized");
        return false;
    }

    if (!g_authService) {
        ZOOM_SDK_NAMESPACE::SDKError err =
            ZOOM_SDK_NAMESPACE::CreateAuthService(&g_authService);
        if (err != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS || !g_authService) {
            char msg[256];
            sprintf_s(msg, "SDK: CreateAuthService failed: %d", (int)err);
            LogToFile(msg);
            return false;
        }
        g_authService->SetEvent(&g_authListener);
    }

    ZOOM_SDK_NAMESPACE::AuthContext authContext;
    static std::wstring s_clientId = L"JlP6KfRqTt6r0t67FcDuqQ";
    authContext.publicAppKey = s_clientId.c_str();

    ZOOM_SDK_NAMESPACE::SDKError err = g_authService->SDKAuth(authContext);
    if (err != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
        char msg[256];
        sprintf_s(msg, "SDK: SDKAuth call failed: %d", (int)err);
        LogToFile(msg);
        return false;
    }

    LogToFile("SDK: SDKAuth called, waiting for callback");
    return true;
}

bool InitializeSDK() {
    LogToFile("SDK: InitializeSDK starting");

    ZOOM_SDK_NAMESPACE::InitParam initParam;
    initParam.strWebDomain = L"https://zoom.us";

    ZOOM_SDK_NAMESPACE::SDKError err = ZOOM_SDK_NAMESPACE::InitSDK(initParam);
    if (err != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
        char msg[256];
        sprintf_s(msg, "SDK: InitSDK FAILED: %d", (int)err);
        LogToFile(msg);
        return false;
    }

    g_sdkInitialized = true;
    LogToFile("SDK: InitSDK SUCCESS");

    std::string token = LoadAccessToken();
    if (!token.empty()) {
        LogToFile("SDK: found existing access token, authenticating");
        AuthenticateSDK();
    } else {
        LogToFile("SDK: no access token stored, waiting for user login");
    }

    return true;
}

} // namespace feeds_engine
