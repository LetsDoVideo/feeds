// engine-oauth.cpp — OAuth 2.0 with PKCE for Zoom Marketplace.
//
// Runs inside FeedsEngine.exe. Generates a PKCE verifier/challenge and a
// random state, opens the browser to Zoom's OAuth endpoint, then polls the
// Feeds worker for the auth code: the loginsuccess page POSTs the code to the
// worker keyed by our state, and the worker hands it back once on a GET. The
// engine exchanges the code for tokens via WinHTTP and saves them to Windows
// Credential Manager (DPAPI-protected, same Windows user account only).
//
// This replaced an older browser→desktop handoff (the loginsuccess page fired
// an ldvfeeds:// protocol that launched FeedsLogin.exe, which wrote the code
// into the \\.\pipe\FeedsAuth named pipe). Browsers now block that auto-launch
// without a user gesture, so the worker-poll path is the only handoff.
//
// The state is only an unguessable correlator; the auth code stays
// PKCE-protected, and the verifier never leaving the engine is what makes
// routing the code through the worker safe.

#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <wincred.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <thread>
#include <mutex>
#include <cstdio>

// Defined in engine-main.cpp
extern void LogToFile(const char* msg);  // forwards at DEBUG
extern void LogInfo(const char* msg);
extern void LogWarn(const char* msg);
extern void LogError(const char* msg);
extern bool SendToPlugin(const std::string& json);

namespace feeds_engine {
bool AuthenticateSDK();  // defined in engine-sdk.cpp

// ---------------------------------------------------------------------------
// In-flight login state — guards the OAuth thread and its cancel signal.
// Accessed from:
//   - the OAuth thread (LoginThreadFunc / the worker poll loop)
//   - the IPC pipe-reader thread (StartLoginFlow / CancelLoginFlow)
//
// Cancel is trivial now that the auth code arrives over HTTPS from the worker
// rather than through a named pipe: CancelLoginFlow just sets g_loginCancelled
// and the poll loop checks it every iteration, breaking out promptly. There is
// no pipe handle and no cancel event to coordinate — the whole overlapped-I/O
// apparatus that the FeedsAuth pipe required is gone.
//
// g_loginCancelled also tells the OAuth thread that its exit was a user cancel
// rather than a real failure, so it exits silently instead of sending
// login_failed: user_cancelled (which would pop an error MessageBox over a
// user-initiated cancel).
// ---------------------------------------------------------------------------
static std::mutex g_loginMutex;
static bool       g_loginInProgress = false;
static bool       g_loginCancelled  = false;

// ---------------------------------------------------------------------------
// Crypto, encoding, and JSON helpers
// (ported from plugin-main.cpp, unchanged in logic)
// ---------------------------------------------------------------------------

static std::string Base64UrlEncode(const unsigned char* data, size_t len)
{
    DWORD encoded_len = 0;
    CryptBinaryToStringA(data, (DWORD)len,
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                         nullptr, &encoded_len);
    std::string encoded(encoded_len, '\0');
    CryptBinaryToStringA(data, (DWORD)len,
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                         &encoded[0], &encoded_len);
    while (!encoded.empty() && encoded.back() == '\0')
        encoded.pop_back();
    for (char& c : encoded) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!encoded.empty() && encoded.back() == '=')
        encoded.pop_back();
    return encoded;
}

static std::string GenerateCodeVerifier()
{
    unsigned char buf[32];
    HCRYPTPROV hProv = 0;
    CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_FULL,
                        CRYPT_VERIFYCONTEXT);
    CryptGenRandom(hProv, sizeof(buf), buf);
    CryptReleaseContext(hProv, 0);
    return Base64UrlEncode(buf, sizeof(buf));
}

static std::vector<unsigned char> SHA256Hash(const std::string& input)
{
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    std::vector<unsigned char> result(32);
    CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_AES,
                        CRYPT_VERIFYCONTEXT);
    CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash);
    CryptHashData(hHash, (const BYTE*)input.data(), (DWORD)input.size(), 0);
    DWORD hashLen = 32;
    CryptGetHashParam(hHash, HP_HASHVAL, result.data(), &hashLen, 0);
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    return result;
}

static std::string DeriveCodeChallenge(const std::string& verifier)
{
    auto hash = SHA256Hash(verifier);
    return Base64UrlEncode(hash.data(), hash.size());
}

static std::string UrlEncode(const std::string& s)
{
    std::ostringstream out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << c;
        } else {
            out << '%' << std::uppercase << std::hex
                << std::setw(2) << std::setfill('0') << (int)c;
        }
    }
    return out.str();
}

static std::string JsonExtractString(const std::string& json, const std::string& key)
{
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

// ---------------------------------------------------------------------------
// Token exchange (WinHTTP POST to zoom.us/oauth/token)
// ---------------------------------------------------------------------------

static std::string ExchangeCodeForToken(const std::string& code, const std::string& verifier)
{
    std::string body =
        std::string("grant_type=authorization_code") +
        "&code="          + UrlEncode(code) +
        "&client_id="     + FEEDS_ZOOM_CLIENT_ID +
        "&redirect_uri="  + UrlEncode("https://letsdovideo.com/loginsuccess") +
        "&code_verifier=" + UrlEncode(verifier);

    HINTERNET hSession = WinHttpOpen(L"Feeds/1.0",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

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
    while (WinHttpReadData(hRequest, buf, sizeof(buf) - 1, &bytesRead) &&
           bytesRead > 0) {
        buf[bytesRead] = '\0';
        response += buf;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

// ---------------------------------------------------------------------------
// Token storage via Windows Credential Manager
// ---------------------------------------------------------------------------

static void SaveTokensToCredentialManager(const std::string& accessToken,
                                          const std::string& refreshToken)
{
    {
        CREDENTIALA cred = {};
        cred.Type               = CRED_TYPE_GENERIC;
        cred.TargetName         = (LPSTR)"Feeds_AccessToken";
        cred.CredentialBlobSize = (DWORD)accessToken.size();
        cred.CredentialBlob     = (LPBYTE)accessToken.data();
        // ENTERPRISE persists across sessions like LOCAL_MACHINE but also
        // roams with the user's Windows profile on domain-joined systems
        // with roaming profiles, and is allowed under stricter enterprise
        // IT policies that disallow LOCAL_MACHINE. Functionally identical
        // for standalone home PCs.
        cred.Persist            = CRED_PERSIST_ENTERPRISE;
        CredWriteA(&cred, 0);
    }
    {
        CREDENTIALA cred = {};
        cred.Type               = CRED_TYPE_GENERIC;
        cred.TargetName         = (LPSTR)"Feeds_RefreshToken";
        cred.CredentialBlobSize = (DWORD)refreshToken.size();
        cred.CredentialBlob     = (LPBYTE)refreshToken.data();
        cred.Persist            = CRED_PERSIST_ENTERPRISE;
        CredWriteA(&cred, 0);
    }
}

// ---------------------------------------------------------------------------
// Worker poll — retrieve the auth code the loginsuccess page POSTed to the
// Feeds worker, keyed by our OAuth state.
// ---------------------------------------------------------------------------

static bool IsLoginCancelled()
{
    std::lock_guard<std::mutex> lock(g_loginMutex);
    return g_loginCancelled;
}

// Polls GET /authresult?state=<state> on the same worker host the engine uses
// for the entitlement /tier check (same host, different path):
//   200 {"code":"..."} — the code is ready; pull it and return.
//   204               — not arrived yet; wait and poll again.
// Polls about every 1.5s and gives up after roughly two minutes. The cancel
// flag is checked every iteration (and during the wait, in short slices) so
// CancelLoginFlow breaks the loop promptly. On cancel the returned string is
// empty and outCancelled is set; the caller must NOT send login_failed.
static std::string PollWorkerForAuthCode(const std::string& state, bool& outCancelled)
{
    outCancelled = false;

    const int kPollIntervalMs = 1500;
    const int kTimeoutMs      = 120000;  // ~2 minutes
    const int kMaxAttempts    = kTimeoutMs / kPollIntervalMs;

    std::wstring path = L"/authresult?state=" +
        std::wstring(state.begin(), state.end());

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (IsLoginCancelled()) { outCancelled = true; return ""; }

        std::string code;
        DWORD       statusCode = 0;

        HINTERNET hSession = WinHttpOpen(L"Feeds/1.0",
                                          WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                          WINHTTP_NO_PROXY_NAME,
                                          WINHTTP_NO_PROXY_BYPASS, 0);
        if (hSession) {
            // Short per-phase timeouts (resolve/connect/send/receive, in ms)
            // so a stalled request can't hang a single poll, leave Cancel
            // unresponsive, or overrun the ~2-minute budget.
            WinHttpSetTimeouts(hSession, 8000, 8000, 8000, 8000);

            HINTERNET hConnect = WinHttpConnect(hSession,
                L"feeds-entitlement.square-dust-0e00.workers.dev",
                INTERNET_DEFAULT_HTTPS_PORT, 0);
            if (hConnect) {
                HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET",
                    path.c_str(), nullptr, WINHTTP_NO_REFERER,
                    WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
                if (hRequest) {
                    BOOL sentOk = WinHttpSendRequest(hRequest,
                        WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0);
                    if (sentOk && WinHttpReceiveResponse(hRequest, nullptr)) {
                        DWORD statusSize = sizeof(statusCode);
                        WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

                        std::string response;
                        char  buf[4096];
                        DWORD bytesRead = 0;
                        while (WinHttpReadData(hRequest, buf, sizeof(buf) - 1,
                                               &bytesRead) && bytesRead > 0) {
                            buf[bytesRead] = '\0';
                            response += buf;
                        }
                        if (statusCode == 200)
                            code = JsonExtractString(response, "code");
                    }
                    WinHttpCloseHandle(hRequest);
                }
                WinHttpCloseHandle(hConnect);
            }
            WinHttpCloseHandle(hSession);
        }

        if (statusCode == 200 && !code.empty()) {
            LogToFile("OAuth: worker returned auth code");
            return code;
        }

        // 204 (not ready yet) or a transient error — wait before the next
        // poll, but stay responsive to cancel by checking the flag in short
        // slices rather than sleeping the whole interval at once.
        for (int slept = 0; slept < kPollIntervalMs; slept += 100) {
            if (IsLoginCancelled()) { outCancelled = true; return ""; }
            Sleep(100);
        }
    }

    LogWarn("OAuth: timed out waiting for auth code from worker");
    return "";
}

// ---------------------------------------------------------------------------
// The full login flow, runs on a background thread.
// ---------------------------------------------------------------------------

// RAII clear of in-flight state on thread exit. Guarantees every return path
// resets the flags so the next StartLoginFlow isn't rejected, even if a
// future edit adds a new early-return.
namespace {
struct LoginGuard {
    ~LoginGuard() {
        std::lock_guard<std::mutex> lock(g_loginMutex);
        g_loginInProgress = false;
        g_loginCancelled  = false;
    }
};
}

static void LoginThreadFunc()
{
    LoginGuard guard;
    LogToFile("OAuth: LoginThreadFunc started");

    std::string verifier  = GenerateCodeVerifier();
    std::string challenge = DeriveCodeChallenge(verifier);

    // Random, URL-safe, unguessable correlator for the worker handoff. Same
    // RNG and encoding as the PKCE verifier (32 random bytes, base64url, so no
    // URL-encoding needed when appended below). The state only correlates our
    // browser session with the worker's stored code — the code itself stays
    // PKCE-protected and the verifier never leaves this engine.
    std::string state = GenerateCodeVerifier();

    std::string authUrl =
        std::string("https://zoom.us/oauth/authorize") +
        "?response_type=code" +
        "&client_id="          + std::string(FEEDS_ZOOM_CLIENT_ID) +
        "&redirect_uri="       + UrlEncode("https://letsdovideo.com/loginsuccess") +
        "&code_challenge="     + challenge +
        "&code_challenge_method=S256" +
        "&state="              + state +
        "&prompt=consent";

    LogToFile("OAuth: opening browser to Zoom authorize endpoint");
    ShellExecuteA(NULL, "open", authUrl.c_str(), NULL, NULL, SW_SHOWNORMAL);

    LogToFile("OAuth: polling worker for auth code");
    bool wasCancelled = false;
    std::string code = PollWorkerForAuthCode(state, wasCancelled);

    if (wasCancelled) {
        // The plugin already cleared its own auth-in-progress state when
        // the user clicked Cancel login; sending login_failed here would
        // pop a redundant "Login failed: user_cancelled" MessageBox on
        // top of a user-initiated cancel. Exit silently.
        LogToFile("OAuth: login cancelled by user — exiting without notifying plugin");
        return;
    }

    if (code.empty()) {
        LogToFile("OAuth: auth code was empty (worker timeout or error, not a user cancel)");
        SendToPlugin("{\"type\":\"login_failed\",\"error\":\"user_cancelled\"}");
        return;
    }

    LogToFile("OAuth: got auth code, exchanging for tokens");
    std::string response = ExchangeCodeForToken(code, verifier);

    std::string accessToken  = JsonExtractString(response, "access_token");
    std::string refreshToken = JsonExtractString(response, "refresh_token");

    if (accessToken.empty()) {
        LogError("OAuth: token exchange failed, no access_token in response");
        SendToPlugin("{\"type\":\"login_failed\",\"error\":\"token_exchange_failed\"}");
        return;
    }

    LogToFile("OAuth: got tokens, saving to Credential Manager");
    SaveTokensToCredentialManager(accessToken, refreshToken);

    LogToFile("OAuth: triggering SDK authentication with new token");
    AuthenticateSDK();

    LogToFile("OAuth: login complete, notifying plugin");
    SendToPlugin("{\"type\":\"login_succeeded\"}");
}

// ---------------------------------------------------------------------------
// Public entry points — called from engine-main.cpp's IPC handlers
// ---------------------------------------------------------------------------

bool StartLoginFlow()
{
    LogToFile("OAuth: StartLoginFlow called");

    {
        std::lock_guard<std::mutex> lock(g_loginMutex);
        if (g_loginInProgress) {
            // Reject duplicate. Don't send login_failed: the plugin's
            // g_authInProgress is already true and showing "Cancel login",
            // which is the correct state — the previous OAuth attempt is
            // still wrapping up, and the user can click Cancel again to
            // get out (that path resets the plugin's flag and sets
            // g_loginCancelled so the poll loop exits).
            LogToFile("OAuth: StartLoginFlow rejected — login already in progress");
            return false;
        }
        g_loginInProgress = true;
        g_loginCancelled  = false;
    }

    std::thread t(LoginThreadFunc);
    t.detach();
    return true;
}

void CancelLoginFlow()
{
    LogToFile("OAuth: CancelLoginFlow called");
    std::lock_guard<std::mutex> lock(g_loginMutex);
    if (!g_loginInProgress) {
        // No-op if no OAuth flow is running. Happens when the plugin
        // sends a defensive cancel and the engine already cleared the
        // flag from a prior cancel, or before any login attempt at all.
        LogToFile("OAuth: cancel ignored — no login in progress");
        return;
    }
    g_loginCancelled = true;
    // The poll loop checks g_loginCancelled every iteration and exits
    // promptly. The PKCE verifier and state live on the OAuth thread's
    // stack and die with it.
}

} // namespace feeds_engine
