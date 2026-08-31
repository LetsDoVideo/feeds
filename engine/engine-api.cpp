// engine-api.cpp — Authenticated REST calls to api.zoom.us
//
// Ported from plugin-main.cpp v1.0.0. Handles /v2/users/me, /v2/users/me/zak,
// /v2/marketplace/users/me/entitlements, and silent access-token refresh on
// 401. All calls run inside FeedsEngine.exe.

#include <windows.h>
#include <winhttp.h>
#include <wincred.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>

#include "feeds-http.h"      // proxy-aware WinHTTP session helper (shared, common/)
#include "feeds-backend.h"   // entitlement backend hostname (shared, common/)

// Defined in engine-main.cpp
extern void LogToFile(const char* msg);  // forwards at DEBUG
extern void LogInfo(const char* msg);
extern void LogWarn(const char* msg);
extern void LogError(const char* msg);
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
static std::string g_userEmail;
static int         g_currentTier = 0;   // 0 = Free until proven otherwise

// True only when the tier fetch failed AND there was no cached tier to fall
// back on — i.e. the one case where the user involuntarily lands on Free. Read
// by engine-sdk.cpp after the login announce to decide whether to warn.
static bool        g_tierUnresolved = false;

// ---------------------------------------------------------------------------
// Public accessors for other engine TUs
// ---------------------------------------------------------------------------
const std::string& GetAccessToken()     { return g_accessToken; }
const std::string& GetUserDisplayName() { return g_userDisplayName; }
const std::string& GetUserPMI()         { return g_userPMI; }
const std::string& GetUserEmail()       { return g_userEmail; }
int                GetCurrentTier()     { return g_currentTier; }
bool               WasTierUnresolved()  { return g_tierUnresolved; }

void SetAccessToken(const std::string& t)  { g_accessToken  = t; }
void SetRefreshToken(const std::string& t) { g_refreshToken = t; }

void ClearUserInfo() {
    g_accessToken.clear();
    g_refreshToken.clear();
    g_userDisplayName.clear();
    g_userPMI.clear();
    g_userEmail.clear();
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
    // Persist class must match the OAuth login-flow save
    // (engine-oauth.cpp: CRED_PERSIST_ENTERPRISE) — otherwise the first silent
    // token refresh would rewrite the credential with LOCAL_MACHINE, the class
    // the login path deliberately avoids (ENTERPRISE roams with the profile and
    // is allowed under stricter enterprise IT policies).
    if (!g_accessToken.empty()) {
        CREDENTIALA cred = {};
        cred.Type               = CRED_TYPE_GENERIC;
        cred.TargetName         = (LPSTR)"Feeds_AccessToken";
        cred.CredentialBlobSize = (DWORD)g_accessToken.size();
        cred.CredentialBlob     = (LPBYTE)g_accessToken.data();
        cred.Persist            = CRED_PERSIST_ENTERPRISE;
        CredWriteA(&cred, 0);
    }
    if (!g_refreshToken.empty()) {
        CREDENTIALA cred = {};
        cred.Type               = CRED_TYPE_GENERIC;
        cred.TargetName         = (LPSTR)"Feeds_RefreshToken";
        cred.CredentialBlobSize = (DWORD)g_refreshToken.size();
        cred.CredentialBlob     = (LPBYTE)g_refreshToken.data();
        cred.Persist            = CRED_PERSIST_ENTERPRISE;
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
// Last-known-good tier cache.
//
// The tier query is the one network call whose failure silently changes what
// the user is allowed to do. Before this cache every failure path in
// FetchAndApplyEntitlement applied tier=0, so a paying customer on a network
// that blocks the worker host (proxy, firewall, deny-by-default institutional
// filter) was demoted to Free with no message — which reads as "I paid and got
// nothing."
//
// The cache lives in Credential Manager beside the tokens deliberately: it
// inherits their per-Windows-user scope and their CRED_PERSIST_ENTERPRISE
// class, and the logout path that deletes them (engine-meeting.cpp) deletes
// this in the same place, so a second Windows user on the same PC can never
// inherit the first user's tier. The tier is not a secret; Credential Manager
// is used here for lifecycle coupling, not confidentiality.
//
// Written ONLY on a clean 200 + parse. Read once at the top of the fetch, so
// every failure path keeps the cached value without each one having to know
// the cache exists.
// ---------------------------------------------------------------------------
static void SaveCachedTier(int tier) {
    char buf[16];
    sprintf_s(buf, "%d", tier);
    std::string blob(buf);

    CREDENTIALA cred = {};
    cred.Type               = CRED_TYPE_GENERIC;
    cred.TargetName         = (LPSTR)"Feeds_CachedTier";
    cred.CredentialBlobSize = (DWORD)blob.size();
    cred.CredentialBlob     = (LPBYTE)blob.data();
    cred.Persist            = CRED_PERSIST_ENTERPRISE;
    CredWriteA(&cred, 0);
}

// Returns the cached tier, or -1 when nothing has ever been cached — the
// brand-new user who has never once reached the backend. -1 is deliberately
// distinct from a cached 0: a cached 0 is a real answer the backend gave us
// and must still suppress the "couldn't reach licensing" warning.
static int LoadCachedTier() {
    PCREDENTIALA pCred = nullptr;
    if (!CredReadA("Feeds_CachedTier", CRED_TYPE_GENERIC, 0, &pCred))
        return -1;
    std::string v((char*)pCred->CredentialBlob, pCred->CredentialBlobSize);
    CredFree(pCred);
    if (v.empty()) return -1;
    int tier = atoi(v.c_str());
    return (tier >= 0 && tier <= 3) ? tier : -1;
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

    ProxyResolution px = ResolveProxyForUrl(L"https://zoom.us/oauth/token");
    LogProxyResolution(px, "API", /*forceInfo=*/false);
    HINTERNET hSession = OpenProxiedSession(L"Feeds/1.0", px);
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
        LogWarn("API: RefreshAccessToken failed (no access_token in response)");
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
// ZoomApiGetWithStatus additionally reports the final HTTP status, so callers
// like the Zoom Events flow can tell a missing-scope 401/403 apart from a real
// empty result. ZoomApiGet keeps its original body-only contract: returns the
// response body on success, or empty on session-expired.
// ---------------------------------------------------------------------------
std::string ZoomApiGetWithStatus(const std::wstring& path, int& outStatus) {
    outStatus = 0;
    auto doRequest = [&](int& st) -> std::string {
        st = 0;
        ProxyResolution px = ResolveProxyForUrl(L"https://api.zoom.us" +
                                                std::wstring(path));
        LogProxyResolution(px, "API", /*forceInfo=*/false);
        HINTERNET hSession = OpenProxiedSession(L"Feeds/1.0", px);
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

        st = (int)statusCode;
        return response;
    };

    int st = 0;
    std::string body = doRequest(st);

    if (st == 401) {
        LogToFile("API: got 401, attempting refresh");
        if (RefreshAccessToken()) {
            body = doRequest(st);
        } else {
            LogWarn("API: refresh failed, session expired");
            SendToPlugin("{\"type\":\"session_expired\"}");
            outStatus = 401;
            return "";
        }
    }

    outStatus = st;
    return body;
}

std::string ZoomApiGet(const std::wstring& path) {
    int status = 0;
    return ZoomApiGetWithStatus(path, status);
}

// ---------------------------------------------------------------------------
// Authenticated POST to api.zoom.us with a JSON body. Mirrors ZoomApiGet's
// 401 → refresh → retry-once semantics and session-expired notification.
// Returns the response body on success, or empty on session-expired.
//
// Non-401 4xx/5xx responses return the body unchanged so the caller can
// surface the JSON error from Zoom (e.g. for a 403 with a missing-scope
// message). Status code is intentionally not exposed — callers should
// validate the response by parsing expected fields out of the body.
// ---------------------------------------------------------------------------
std::string ZoomApiPost(const std::wstring& path, const std::string& jsonBody) {
    auto doRequest = [&]() -> std::string {
        ProxyResolution px = ResolveProxyForUrl(L"https://api.zoom.us" +
                                                std::wstring(path));
        LogProxyResolution(px, "API", /*forceInfo=*/false);
        HINTERNET hSession = OpenProxiedSession(L"Feeds/1.0", px);
        if (!hSession) return "";
        HINTERNET hConnect = WinHttpConnect(hSession, L"api.zoom.us",
                                             INTERNET_DEFAULT_HTTPS_PORT, 0);
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(),
                                                 nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 WINHTTP_FLAG_SECURE);
        std::wstring authHeader = L"Authorization: Bearer " +
            std::wstring(g_accessToken.begin(), g_accessToken.end());
        WinHttpAddRequestHeaders(hRequest, authHeader.c_str(),
                                 (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        WinHttpAddRequestHeaders(hRequest,
                                 L"Content-Type: application/json",
                                 (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           (LPVOID)jsonBody.c_str(), (DWORD)jsonBody.size(),
                           (DWORD)jsonBody.size(), 0);
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
        LogToFile("API: POST got 401, attempting refresh");
        if (RefreshAccessToken()) {
            result = doRequest();
            sep = result.find('|');
            body = (sep == std::string::npos) ? "" : result.substr(sep + 1);
        } else {
            LogWarn("API: refresh failed, session expired");
            SendToPlugin("{\"type\":\"session_expired\"}");
            return "";
        }
    }

    // Surface non-success status (403 missing-scope, 5xx, etc.) in the
    // engine log. 200/201 are the happy paths; 401 already logged above.
    if (status != "200" && status != "201" && status != "401") {
        LogToFile(("API: POST got HTTP " + status).c_str());
    }

    return body;
}

// ---------------------------------------------------------------------------
// Create an instant meeting via Zoom REST API.
//
// POST /v2/users/me/meetings  with  {"type":1, "topic":"...", "settings":{...}}
//   type=1               instant meeting (provisioned, not yet started)
//   approval_type=2      no registration required (open join)
//   use_pmi=false        force a fresh meeting number — without this Zoom's
//                        type:1 default is to recycle the user's PMI, which
//                        defeats the point of "create a new meeting"
//
// Note: even with use_pmi=false, a Zoom account-level setting ("Use PMI for
// instant meetings") can still override and produce a PMI-numbered meeting.
// If that happens, the user must disable it at zoom.us/profile/setting.
//
// On success populates outId / outPassword / outJoinUrl and returns true.
// The host then enters this meeting via IMeetingService::Start() — REST
// creates it, SDK starts it. Don't try to Join() — a freshly-created
// meeting is in MEETING_STATUS_WAITINGFORHOST until Start() is called.
//
// Requires meeting:write:meeting OAuth scope on the access token.
// Without the scope the call returns 403, which ZoomApiPost surfaces in
// the engine log and we return false (caller emits meeting_failed).
// ---------------------------------------------------------------------------
bool CreateInstantMeeting(const std::string& topic,
                          unsigned long long& outId,
                          std::string& outPassword,
                          std::string& outJoinUrl) {
    outId = 0;
    outPassword.clear();
    outJoinUrl.clear();

    // Minimal JSON escape on the topic — quote and backslash only. Feeds
    // generates the topic internally so it's already controlled input,
    // but defensive in case the caller ever passes user-provided text.
    std::string escTopic;
    for (char c : topic) {
        if      (c == '"')  escTopic += "\\\"";
        else if (c == '\\') escTopic += "\\\\";
        else                escTopic += c;
    }
    std::string body = "{\"type\":1,\"topic\":\"" + escTopic + "\","
                       "\"settings\":{\"approval_type\":2,"
                                     "\"use_pmi\":false}}";

    LogToFile("API: CreateInstantMeeting POST /v2/users/me/meetings");
    std::string response = ZoomApiPost(L"/v2/users/me/meetings", body);
    if (response.empty()) {
        LogError("API: CreateInstantMeeting got empty response "
                 "(session expired or network error)");
        return false;
    }

    std::string idStr = JsonExtractNumber(response, "id");
    if (idStr.empty()) {
        LogError("API: CreateInstantMeeting response missing 'id' field");
        // First 200 chars of response to help diagnose what came back
        // (likely a 403 error body with a missing-scope message).
        std::string snippet = response.substr(0, 200);
        LogToFile(("API: response snippet: " + snippet).c_str());
        return false;
    }
    outId       = _strtoui64(idStr.c_str(), nullptr, 10);
    outPassword = JsonExtractString(response, "password");
    outJoinUrl  = JsonExtractString(response, "join_url");

    // Log the id only — never the password (not even present/empty) in the
    // shared OBS log.
    char log[256];
    sprintf_s(log, "API: CreateInstantMeeting got id=%llu", outId);
    LogInfo(log);
    return true;
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
    g_userEmail       = JsonExtractString(response, "email");

    // Redaction: never log the user's name, PMI, or email — these must not
    // reach the shared OBS log. Log success only.
    LogToFile("API: FetchUserInfo succeeded");

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
        LogWarn("API: FetchZak got empty ZAK");
    else
        LogToFile("API: FetchZak succeeded");
    return zak;
}

// ---------------------------------------------------------------------------
// Zoom Events API (GET /v2/zoom_events/...). All three calls and their parsing
// live here; the plugin only renders the normalized arrays we emit and drives
// selection over IPC.
// ---------------------------------------------------------------------------

// JSON string escaper for the event/session names we re-emit (they can contain
// quotes, backslashes, or non-ASCII). Same shape as the engine's other escapers.
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
                if (c < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", c); out += b; }
                else out += (char)c;
        }
    }
    return out;
}

// Return the text of the array value for `key` (from its '[' to the matching
// ']'), string- and bracket-aware so nested arrays/objects don't confuse it.
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
// skipping nested braces and brace characters inside strings.
static std::vector<std::string> JsonSplitObjects(const std::string& arr) {
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

// Fetch one role_type's upcoming events, appending normalized objects to `out`.
// Follows next_page_token. Sets authFailed on a 401/403 — the user authorized
// before the Events scopes existed and must log out / back in to re-consent.
static void FetchEventsForRole(const std::string& roleType, std::string& out,
                               bool& authFailed) {
    std::string nextToken;
    for (int page = 0; page < 20; ++page) {  // page cap is defensive
        std::wstring path =
            L"/v2/zoom_events/events?event_status_type=upcoming&role_type=" +
            std::wstring(roleType.begin(), roleType.end());
        if (!nextToken.empty())
            path += L"&next_page_token=" +
                    std::wstring(nextToken.begin(), nextToken.end());

        int status = 0;
        std::string resp = ZoomApiGetWithStatus(path, status);
        if (status == 401 || status == 403) { authFailed = true; return; }
        if (resp.empty()) return;

        std::string body = JsonExtractArrayBody(resp, "events");
        for (const std::string& obj : JsonSplitObjects(body)) {
            std::string id = JsonExtractString(obj, "event_id");
            if (id.empty()) continue;
            std::string name  = JsonExtractString(obj, "name");
            std::string etype = JsonExtractString(obj, "event_type");
            if (!out.empty()) out += ",";
            out += "{\"event_id\":\""  + JsonEscape(id)    + "\","
                    "\"name\":\""       + JsonEscape(name)  + "\","
                    "\"event_type\":\"" + JsonEscape(etype) + "\","
                    "\"role\":\""       + roleType          + "\"}";
        }

        nextToken = JsonExtractString(resp, "next_page_token");
        if (nextToken.empty()) return;
    }
}

// Returns a JSON array string of normalized events across both role types
// (attendee + host). On a 401/403 from either call, authFailed is set and ""
// is returned so the handler can prompt the user to re-consent.
std::string FetchEventsArray(bool& authFailed) {
    authFailed = false;
    LogToFile("API: FetchEventsArray starting");
    std::string out;
    FetchEventsForRole("attendee", out, authFailed);
    if (authFailed) return "";
    FetchEventsForRole("host", out, authFailed);
    if (authFailed) return "";
    return "[" + out + "]";
}

// Returns a JSON array string of normalized sessions for an event.
//   type: 0 = meeting, 2 = webinar, 4 = neither. meeting_id/webinar_id are
//   present in the response but informational — we never join by them.
std::string FetchEventSessionsArray(const std::string& eventId) {
    LogToFile("API: FetchEventSessionsArray starting");
    std::wstring path = L"/v2/zoom_events/events/" +
        std::wstring(eventId.begin(), eventId.end()) + L"/sessions";
    std::string resp = ZoomApiGet(path);
    if (resp.empty()) return "[]";

    std::string out;
    std::string body = JsonExtractArrayBody(resp, "sessions");
    for (const std::string& obj : JsonSplitObjects(body)) {
        std::string id = JsonExtractString(obj, "session_id");
        if (id.empty()) continue;
        std::string name  = JsonExtractString(obj, "name");
        std::string start = JsonExtractString(obj, "start_time");
        std::string type  = JsonExtractNumber(obj, "type");
        if (type.empty()) type = "4";  // 4 = neither meeting nor webinar
        if (!out.empty()) out += ",";
        out += "{\"session_id\":\"" + JsonEscape(id)    + "\","
                "\"name\":\""        + JsonEscape(name)  + "\","
                "\"start_time\":\""  + JsonEscape(start) + "\","
                "\"type\":"          + type              + "}";
    }
    return "[" + out + "]";
}

// Fetch the just-in-time join token for a session. The token has no documented
// TTL, so callers must fetch it immediately before joining and never cache it.
// Returns true only when code == 0 and a token came back; otherwise code and
// errorMessage carry the failure (1130 = no valid ticket, 1150 = revoked).
bool FetchEventJoinToken(const std::string& eventId, const std::string& sessionId,
                         int& code, std::string& joinToken,
                         std::string& errorMessage) {
    LogToFile("API: FetchEventJoinToken starting");
    code = -1; joinToken.clear(); errorMessage.clear();

    std::wstring path = L"/v2/zoom_events/events/" +
        std::wstring(eventId.begin(), eventId.end()) + L"/sessions/" +
        std::wstring(sessionId.begin(), sessionId.end()) + L"/join_token";
    std::string resp = ZoomApiGet(path);
    if (resp.empty()) {
        errorMessage = "No response from Zoom Events.";
        return false;
    }

    std::string codeStr = JsonExtractNumber(resp, "code");
    code         = codeStr.empty() ? -1 : atoi(codeStr.c_str());
    joinToken    = JsonExtractString(resp, "join_token");
    errorMessage = JsonExtractString(resp, "error_message");
    return code == 0 && !joinToken.empty();
}

// ---------------------------------------------------------------------------
// Fetch the user's tier from the Feeds entitlement backend.
//
// The backend at FEEDS_BACKEND_URL receives Zoom monetization webhooks and
// exposes a /tier query that returns the caller's current tier (0-3) as a
// JSON integer. We pass the user's Zoom access token as a Bearer credential;
// the backend validates it and looks up the matching account.
//
// Why not Zoom's REST entitlements endpoint? Empirically that endpoint does
// not return Local Test licenses, and Zoom's monetization signal lives in
// webhooks rather than a queryable resource. The Feeds backend is the
// authoritative source.
//
// On any failure (network, non-200 status, missing tier field) we log and
// fall back to tier 0 (Free).
// ---------------------------------------------------------------------------
// Host and origin come from common/feeds-backend.h — the one place the Worker
// hostname is spelled out.
static const std::string FEEDS_BACKEND_URL = FEEDS_BACKEND_ORIGIN "/tier";
// Wide form for proxy resolution (ResolveProxyForUrl takes a std::wstring).
static const std::wstring FEEDS_BACKEND_URL_W = FEEDS_BACKEND_ORIGIN_W L"/tier";

void FetchAndApplyEntitlement() {
    LogToFile("API: FetchAndApplyEntitlement starting");

    // Start from the last successfully-resolved tier rather than Free, so every
    // failure path below leaves a returning user's entitlement intact. Only a
    // clean 200 + parse changes it (and re-caches it).
    const int cachedTier = LoadCachedTier();
    g_currentTier    = (cachedTier >= 0) ? cachedTier : 0;
    g_tierUnresolved = false;

    // One place that decides what a failure means, so the six early-returns
    // below don't each have to reason about the cache. With a cached tier we
    // keep it and stay quiet; without one the user really is landing on Free
    // involuntarily, which the plugin surfaces as a network warning.
    auto giveUp = [&](const char* why) {
        char msg[192];
        if (cachedTier >= 0) {
            sprintf_s(msg, "API: tier query failed (%s) — keeping cached tier=%d",
                      why, cachedTier);
        } else {
            g_tierUnresolved = true;
            sprintf_s(msg, "API: tier query failed (%s) — no cached tier, "
                           "applying tier=0", why);
        }
        LogWarn(msg);
    };

    ProxyResolution px = ResolveProxyForUrl(FEEDS_BACKEND_URL_W);
    LogProxyResolution(px, "API", /*forceInfo=*/false);
    HINTERNET hSession = OpenProxiedSession(L"Feeds/1.0", px);
    if (!hSession) {
        giveUp("WinHttpOpen");
        return;
    }
    HINTERNET hConnect = WinHttpConnect(hSession,
        FEEDS_BACKEND_HOST_W,
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        giveUp("WinHttpConnect");
        return;
    }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/tier",
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        giveUp("WinHttpOpenRequest");
        return;
    }

    std::wstring authHeader = L"Authorization: Bearer " +
        std::wstring(g_accessToken.begin(), g_accessToken.end());
    WinHttpAddRequestHeaders(hRequest, authHeader.c_str(),
                             (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    BOOL sentOk = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                      nullptr, 0, 0, 0);
    if (!sentOk || !WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        giveUp("network error");
        return;
    }

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

    // A 401 means our access token is stale, not that the user's entitlement
    // changed — so it takes the same keep-the-cache path as a network failure
    // rather than demoting someone whose subscription is fine.
    if (statusCode == 401) {
        giveUp("HTTP 401");
        return;
    }

    if (statusCode != 200) {
        char why[32];
        sprintf_s(why, "HTTP %lu", statusCode);
        giveUp(why);
        return;
    }

    std::string tierStr = JsonExtractNumber(response, "tier");
    if (tierStr.empty()) {
        giveUp("missing tier field");
        return;
    }

    // The one path that is allowed to change the tier. Note this legitimately
    // caches a 0: a backend that answers "tier 0" is a real downgrade (expired
    // or cancelled subscription) and must stick, unlike an unanswered query.
    g_currentTier = atoi(tierStr.c_str());
    SaveCachedTier(g_currentTier);

    char msg[128];
    sprintf_s(msg, "API: entitlement applied, tier=%d (from backend)",
              g_currentTier);
    LogInfo(msg);
}

} // namespace feeds_engine
