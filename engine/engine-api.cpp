// engine-api.cpp — Authenticated REST calls to api.zoom.us
//
// Ported from plugin-main.cpp v1.0.0. Handles /v2/users/me, /v2/users/me/zak,
// /v2/marketplace/users/me/entitlements, and silent access-token refresh on
// 401. All calls run inside FeedsEngine.exe.

#include <windows.h>
#include <winhttp.h>
#include <wincred.h>
#include <string>
#include <cstdio>

// Defined in engine-main.cpp
extern void LogToFile(const char* msg);
extern bool SendToPlugin(const std::string& json);

namespace feeds_engine {

// ---------------------------------------------------------------------------
// State owned by this translation unit. The access token is loaded from
// Credential Manager at startup (and replaced on login/refresh). Display
// name, PMI, and tier are populated by the post-login prefetch and then
// held for the engine's lifetime.
// ---------------------------------------------------------------------------
static std::string g_accessToken;
static std::string g_refreshToken;
static std::string g_userDisplayName;
static std::string g_userPMI;
static int         g_currentTier = 0;   // 0 = Free until proven otherwise

// ---------------------------------------------------------------------------
// Public accessors for other engine TUs
// ---------------------------------------------------------------------------
const std::string& GetAccessToken()     { return g_accessToken; }
const std::string& GetUserDisplayName() { return g_userDisplayName; }
const std::string& GetUserPMI()         { return g_userPMI; }
int                GetCurrentTier()     { return g_currentTier; }

void SetAccessToken(const std::string& t)  { g_accessToken  = t; }
void SetRefreshToken(const std::string& t) { g_refreshToken = t; }

void ClearUserInfo() {
    g_accessToken.clear();
    g_refreshToken.clear();
    g_userDisplayName.clear();
    g_userPMI.clear();
    g_currentTier = 0;
}

// ---------------------------------------------------------------------------
// JSON helpers (same primitives as elsewhere in the engine)
// ---------------------------------------------------------------------------
static std::string JsonExtractString(const std::string& json,
                                     const std::string& key) {
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

static std::string JsonExtractNumber(const std::string& json,
                                     const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find_first_of("0123456789", pos + search.size());
    if (pos == std::string::npos) return "";
    size_t end = json.find_first_not_of("0123456789", pos);
    return json.substr(pos, end == std::string::npos ? std::string::npos
                                                      : end - pos);
}

// ---------------------------------------------------------------------------
// Credential Manager helpers — save new tokens after a refresh
// ---------------------------------------------------------------------------
static void SaveTokensToCredentialManager() {
    if (!g_accessToken.empty()) {
        CREDENTIALA cred = {};
        cred.Type               = CRED_TYPE_GENERIC;
        cred.TargetName         = (LPSTR)"Feeds_AccessToken";
        cred.CredentialBlobSize = (DWORD)g_accessToken.size();
        cred.CredentialBlob     = (LPBYTE)g_accessToken.data();
        cred.Persist            = CRED_PERSIST_LOCAL_MACHINE;
        CredWriteA(&cred, 0);
    }
    if (!g_refreshToken.empty()) {
        CREDENTIALA cred = {};
        cred.Type               = CRED_TYPE_GENERIC;
        cred.TargetName         = (LPSTR)"Feeds_RefreshToken";
        cred.CredentialBlobSize = (DWORD)g_refreshToken.size();
        cred.CredentialBlob     = (LPBYTE)g_refreshToken.data();
        cred.Persist            = CRED_PERSIST_LOCAL_MACHINE;
        CredWriteA(&cred, 0);
    }
}

static void LoadRefreshTokenFromCredentialManager() {
    PCREDENTIALA pCred = nullptr;
    if (CredReadA("Feeds_RefreshToken", CRED_TYPE_GENERIC, 0, &pCred)) {
        g_refreshToken = std::string((char*)pCred->CredentialBlob,
                                     pCred->CredentialBlobSize);
        CredFree(pCred);
    }
}

// ---------------------------------------------------------------------------
// Refresh the access token using the stored refresh token.
// Returns true on success; false means the user needs to log in again.
// ---------------------------------------------------------------------------
static bool RefreshAccessToken() {
    if (g_refreshToken.empty()) {
        LoadRefreshTokenFromCredentialManager();
        if (g_refreshToken.empty()) return false;
    }

    std::string body =
        std::string("grant_type=refresh_token") +
        "&refresh_token=" + g_refreshToken +   // refresh tokens are already URL-safe
        "&client_id="     + std::string(FEEDS_ZOOM_CLIENT_ID);

    HINTERNET hSession = WinHttpOpen(L"Feeds/1.0",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    HINTERNET hConnect = WinHttpConnect(hSession, L"zoom.us",
                                         INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/oauth/token",
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE);
    WinHttpAddRequestHeaders(hRequest,
        L"Content-Type: application/x-www-form-urlencoded",
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                       (LPVOID)body.c_str(), (DWORD)body.size(),
                       (DWORD)body.size(), 0);
    WinHttpReceiveResponse(hRequest, nullptr);

    std::string response;
    char buf[4096];
    DWORD bytesRead = 0;
    while (WinHttpReadData(hRequest, buf, sizeof(buf) - 1, &bytesRead)
           && bytesRead > 0) {
        buf[bytesRead] = '\0';
        response += buf;
    }
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    std::string newAccess  = JsonExtractString(response, "access_token");
    std::string newRefresh = JsonExtractString(response, "refresh_token");

    if (newAccess.empty()) {
        LogToFile("API: RefreshAccessToken failed (no access_token in response)");
        return false;
    }

    g_accessToken = newAccess;
    if (!newRefresh.empty())
        g_refreshToken = newRefresh;
    SaveTokensToCredentialManager();

    // Notify plugin so it can persist the new refresh token if the
    // protocol ever calls for it (no-op today, future-safe).
    SendToPlugin("{\"type\":\"token_refreshed\"}");

    LogToFile("API: access token refreshed");
    return true;
}

// ---------------------------------------------------------------------------
// Authenticated GET to api.zoom.us with transparent 401-retry via refresh.
// Returns the response body on success, or empty on session-expired.
// ---------------------------------------------------------------------------
std::string ZoomApiGet(const std::wstring& path) {
    auto doRequest = [&]() -> std::string {
        HINTERNET hSession = WinHttpOpen(L"Feeds/1.0",
                                          WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                          WINHTTP_NO_PROXY_NAME,
                                          WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return "";
        HINTERNET hConnect = WinHttpConnect(hSession, L"api.zoom.us",
                                             INTERNET_DEFAULT_HTTPS_PORT, 0);
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                                                 nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 WINHTTP_FLAG_SECURE);
        std::wstring authHeader = L"Authorization: Bearer " +
            std::wstring(g_accessToken.begin(), g_accessToken.end());
        WinHttpAddRequestHeaders(hRequest, authHeader.c_str(),
                                 (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           nullptr, 0, 0, 0);
        WinHttpReceiveResponse(hRequest, nullptr);

        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &statusCode, &statusSize,
                            WINHTTP_NO_HEADER_INDEX);

        std::string response;
        char buf[4096];
        DWORD bytesRead = 0;
        while (WinHttpReadData(hRequest, buf, sizeof(buf) - 1, &bytesRead)
               && bytesRead > 0) {
            buf[bytesRead] = '\0';
            response += buf;
        }
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        return std::to_string(statusCode) + "|" + response;
    };

    std::string result = doRequest();
    size_t sep = result.find('|');
    if (sep == std::string::npos) return "";
    std::string status = result.substr(0, sep);
    std::string body   = result.substr(sep + 1);

    if (status == "401") {
        LogToFile("API: got 401, attempting refresh");
        if (RefreshAccessToken()) {
            result = doRequest();
            sep = result.find('|');
            body = (sep == std::string::npos) ? "" : result.substr(sep + 1);
        } else {
            LogToFile("API: refresh failed, session expired");
            SendToPlugin("{\"type\":\"session_expired\"}");
            return "";
        }
    }

    return body;
}

// ---------------------------------------------------------------------------
// Fetch user display name + PMI. Caches results in engine-side globals.
// Called once after SDK auth succeeds (pre-fetch so Connect is snappy).
// ---------------------------------------------------------------------------
bool FetchUserInfo() {
    LogToFile("API: FetchUserInfo starting");
    std::string response = ZoomApiGet(L"/v2/users/me");
    if (response.empty()) {
        LogToFile("API: FetchUserInfo got empty response");
        return false;
    }

    std::string name = JsonExtractString(response, "display_name");
    if (name.empty())
        name = JsonExtractString(response, "first_name");
    g_userDisplayName = name;
    g_userPMI         = JsonExtractNumber(response, "pmi");

    char msg[512];
    sprintf_s(msg, "API: FetchUserInfo got name='%s', pmi='%s'",
              g_userDisplayName.c_str(), g_userPMI.c_str());
    LogToFile(msg);

    return !g_userDisplayName.empty();
}

// ---------------------------------------------------------------------------
// Fetch a fresh ZAK token. Not cached — ZAKs are short-lived, so this is
// called at every meeting-join attempt.
// ---------------------------------------------------------------------------
std::string FetchZak() {
    LogToFile("API: FetchZak starting");
    std::string response = ZoomApiGet(L"/v2/users/me/zak");
    std::string zak = JsonExtractString(response, "token");
    if (zak.empty())
        LogToFile("API: FetchZak got empty ZAK");
    else
        LogToFile("API: FetchZak succeeded");
    return zak;
}

// ---------------------------------------------------------------------------
// Fetch the marketplace entitlement and map to a tier.
// Defaults to tier 0 (Free) — for now, until the app is live on Marketplace,
// this call returns nothing useful and the tier stays at 0. The substring
// check is intentional: the shape of the response isn't locked in until we
// see real responses post-launch, and presence of a tier name is a
// reasonable proxy.
// ---------------------------------------------------------------------------
void FetchAndApplyEntitlement() {
    LogToFile("API: FetchAndApplyEntitlement starting");
    // Build the path with the app_id from the compile-time macro.
    // WinHTTP wants wide strings; do the conversion once here.
    std::string appIdA = FEEDS_ZOOM_CLIENT_ID;
    std::wstring appIdW(appIdA.begin(), appIdA.end());
    std::wstring path = L"/v2/marketplace/users/me/entitlements?app_id=" + appIdW;
    std::string response = ZoomApiGet(path);

    g_currentTier = 0;  // Default to Free

    if (response.find("Broadcaster") != std::string::npos)
        g_currentTier = 3;
    else if (response.find("Streamer") != std::string::npos)
        g_currentTier = 2;
    else if (response.find("Basic") != std::string::npos)
        g_currentTier = 1;
// ================================================================
    // !! TESTING OVERRIDE — REMOVE BEFORE MARKETPLACE SUBMISSION !!
    // Forces tier to 3 (Broadcaster) regardless of actual entitlement.
    // Purpose: allow testing of multi-feed functionality before the
    // Zoom App Marketplace entitlement system is live.
    // ================================================================
    g_currentTier = 3;
    LogToFile("API: !! TIER TESTING OVERRIDE ACTIVE, tier forced to 3 !!");
    // ================================================================
    char msg[128];
    sprintf_s(msg, "API: entitlement applied, tier=%d", g_currentTier);
    LogToFile(msg);
}

} // namespace feeds_engine
