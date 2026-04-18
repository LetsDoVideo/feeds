// engine-sdk.cpp — Zoom Meeting SDK integration for FeedsEngine.

#include <windows.h>
#include <wincred.h>
#include "zoom_sdk.h"
#include "auth_service_interface.h"
#include <string>
#include <cstdio>

extern void LogToFile(const char* msg);
extern bool SendToPlugin(const std::string& json);

namespace feeds_engine {

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
        if (ret == ZOOM_SDK_NAMESPACE::AUTHRET_SUCCESS) {
            LogToFile("SDK: onAuthenticationReturn SUCCESS");
            SendToPlugin("{\"type\":\"sdk_authenticated\"}");
        } else {
            char msg[256];
            sprintf_s(msg, "SDK: onAuthenticationReturn FAILED: %d", (int)ret);
            LogToFile(msg);
            char resp[256];
            sprintf_s(resp, "{\"type\":\"sdk_auth_failed\",\"code\":%d}", (int)ret);
            SendToPlugin(resp);
        }
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
