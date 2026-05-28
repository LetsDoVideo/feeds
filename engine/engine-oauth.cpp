// engine-oauth.cpp — OAuth 2.0 with PKCE for Zoom Marketplace.
//
// Runs inside FeedsEngine.exe. Generates a PKCE verifier/challenge,
// opens the browser to Zoom's OAuth endpoint, listens on the FeedsAuth
// named pipe for FeedsLogin.exe to deliver the auth code, exchanges
// the code for tokens via WinHTTP, and saves them to Windows Credential
// Manager (DPAPI-protected, same Windows user account only).

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
extern void LogToFile(const char* msg);
extern bool SendToPlugin(const std::string& json);

namespace feeds_engine {
bool AuthenticateSDK();  // defined in engine-sdk.cpp

// ---------------------------------------------------------------------------
// In-flight login state — guards the OAuth thread and its cancel signal.
// Accessed from:
//   - the OAuth thread (LoginThreadFunc / WaitForAuthCode)
//   - the IPC pipe-reader thread (StartLoginFlow / CancelLoginFlow)
//
// FeedsAuth pipe ownership: the OAuth thread owns the pipe handle for its
// entire lifetime. Cancel never touches it. Earlier designs (e4d190c) gave
// cancel co-ownership and had it CloseHandle the pipe to unblock the OAuth
// thread, but CloseHandle on a server pipe with a pending synchronous
// ConnectNamedPipe issued from another thread hangs the calling thread —
// the kernel can't release the file object until the pending IRP drains,
// and on a non-overlapped pipe there's no mechanism to drain it. That
// hang wedged the IPC reader (cancel runs on it) so the engine went
// completely silent after the first cancel. Fix: pipe is now created with
// FILE_FLAG_OVERLAPPED and waited on via WaitForMultipleObjects against
// g_loginCancelEvent.
//
// g_loginCancelEvent is a manual-reset event the OAuth thread waits on
// alongside each overlapped pipe operation. Cancel signals it via a single
// SetEvent call (no handle touches, no blocking). Manual-reset so once
// signaled, both the ConnectNamedPipe wait AND a subsequent ReadFile wait
// see it. Reset by LoginGuard at thread exit.
//
// g_loginCancelled tells the OAuth thread that its unblock was a user
// cancel rather than a real auth code, so it exits silently instead of
// sending login_failed: user_cancelled (which would pop an error MessageBox
// over a user-initiated cancel).
// ---------------------------------------------------------------------------
static std::mutex g_loginMutex;
static bool       g_loginInProgress  = false;
static bool       g_loginCancelled   = false;
static HANDLE     g_loginCancelEvent = nullptr;

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
// Named pipe listener — waits for FeedsLogin.exe to deliver the auth code
// via \\.\pipe\FeedsAuth
// ---------------------------------------------------------------------------

// Wait for one overlapped op (ConnectNamedPipe or ReadFile) plus the cancel
// event. Returns true if the op completed successfully, false on cancel or
// real failure. On cancel, outCancelled is set; the caller drains the IRP
// before closing the pipe via CancelIoEx + GetOverlappedResult(..., TRUE).
static bool WaitOpOrCancel(HANDLE pipe, OVERLAPPED& ov, bool& outCancelled)
{
    HANDLE waits[2] = { ov.hEvent, g_loginCancelEvent };
    DWORD  result   = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (result == WAIT_OBJECT_0 + 1) {
        outCancelled = true;
        CancelIoEx(pipe, &ov);
        DWORD discarded = 0;
        GetOverlappedResult(pipe, &ov, &discarded, TRUE);
        return false;
    }
    return result == WAIT_OBJECT_0;
}

// outCancelled is set to true if CancelLoginFlow signaled g_loginCancelEvent
// while we were waiting. On cancel the returned string is empty and the
// caller must NOT send login_failed to the plugin.
static std::string WaitForAuthCode(bool& outCancelled)
{
    outCancelled = false;

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength        = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = FALSE;

    HANDLE pipe = CreateNamedPipeA(
        "\\\\.\\pipe\\FeedsAuth",
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 512, 512, 0, &sa);

    if (pipe == INVALID_HANDLE_VALUE) {
        LogToFile("OAuth: failed to create FeedsAuth pipe");
        return "";
    }

    // Re-check cancel after creating the pipe — a cancel that arrived
    // between LoginThreadFunc setting g_loginInProgress and us reaching
    // here would have signaled g_loginCancelEvent, but only the wait
    // call below actually observes the event. Bail out early to avoid
    // an unnecessary ConnectNamedPipe round-trip.
    {
        std::lock_guard<std::mutex> lock(g_loginMutex);
        if (g_loginCancelled) {
            CloseHandle(pipe);
            outCancelled = true;
            return "";
        }
    }

    // Phase 1: wait for the client (FeedsLogin.exe) to connect.
    OVERLAPPED connectOv = {};
    connectOv.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);  // manual-reset
    if (!connectOv.hEvent) {
        LogToFile("OAuth: CreateEvent for ConnectNamedPipe failed");
        CloseHandle(pipe);
        return "";
    }

    BOOL  connected  = ConnectNamedPipe(pipe, &connectOv);
    DWORD connectErr = GetLastError();
    bool  needWait   = false;
    if (!connected) {
        if (connectErr == ERROR_IO_PENDING) {
            needWait = true;
        } else if (connectErr == ERROR_PIPE_CONNECTED) {
            // Client connected before ConnectNamedPipe got there; treat
            // as success without waiting.
        } else {
            LogToFile("OAuth: ConnectNamedPipe on FeedsAuth returned an error");
            CloseHandle(connectOv.hEvent);
            CloseHandle(pipe);
            return "";
        }
    }

    if (needWait) {
        bool cancelled = false;
        if (!WaitOpOrCancel(pipe, connectOv, cancelled)) {
            CloseHandle(connectOv.hEvent);
            CloseHandle(pipe);
            outCancelled = cancelled;
            return "";
        }
    }
    CloseHandle(connectOv.hEvent);

    // Phase 2: read the auth code. Same overlapped + cancel-event pattern.
    OVERLAPPED readOv = {};
    readOv.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!readOv.hEvent) {
        LogToFile("OAuth: CreateEvent for ReadFile failed");
        CloseHandle(pipe);
        return "";
    }

    char  buf[512]  = {};
    DWORD bytesRead = 0;
    BOOL  readOk    = ReadFile(pipe, buf, sizeof(buf) - 1, &bytesRead, &readOv);
    DWORD readErr   = GetLastError();
    if (!readOk && readErr == ERROR_IO_PENDING) {
        bool cancelled = false;
        if (!WaitOpOrCancel(pipe, readOv, cancelled)) {
            CloseHandle(readOv.hEvent);
            CloseHandle(pipe);
            outCancelled = cancelled;
            return "";
        }
        GetOverlappedResult(pipe, &readOv, &bytesRead, FALSE);
    }
    CloseHandle(readOv.hEvent);
    CloseHandle(pipe);

    return std::string(buf, bytesRead);
}

// ---------------------------------------------------------------------------
// The full login flow, runs on a background thread.
// ---------------------------------------------------------------------------

// RAII clear of in-flight state on thread exit. Guarantees every return path
// resets the flags so the next StartLoginFlow isn't rejected, even if a
// future edit adds a new early-return. Also closes g_loginCancelEvent —
// the OAuth thread is the sole owner; cancel only ever calls SetEvent on
// it and never closes it.
namespace {
struct LoginGuard {
    ~LoginGuard() {
        HANDLE eventToClose = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_loginMutex);
            g_loginInProgress  = false;
            g_loginCancelled   = false;
            eventToClose       = g_loginCancelEvent;
            g_loginCancelEvent = nullptr;
        }
        if (eventToClose) CloseHandle(eventToClose);
    }
};
}

static void LoginThreadFunc()
{
    LoginGuard guard;
    LogToFile("OAuth: LoginThreadFunc started");

    std::string verifier  = GenerateCodeVerifier();
    std::string challenge = DeriveCodeChallenge(verifier);

    std::string authUrl =
        std::string("https://zoom.us/oauth/authorize") +
        "?response_type=code" +
        "&client_id="          + std::string(FEEDS_ZOOM_CLIENT_ID) +
        "&redirect_uri="       + UrlEncode("https://letsdovideo.com/loginsuccess") +
        "&code_challenge="     + challenge +
        "&code_challenge_method=S256" +
        "&prompt=consent";

    LogToFile("OAuth: opening browser to Zoom authorize endpoint");
    ShellExecuteA(NULL, "open", authUrl.c_str(), NULL, NULL, SW_SHOWNORMAL);

    LogToFile("OAuth: waiting for auth code via FeedsAuth pipe");
    bool wasCancelled = false;
    std::string code = WaitForAuthCode(wasCancelled);

    if (wasCancelled) {
        // The plugin already cleared its own auth-in-progress state when
        // the user clicked Cancel login; sending login_failed here would
        // pop a redundant "Login failed: user_cancelled" MessageBox on
        // top of a user-initiated cancel. Exit silently.
        LogToFile("OAuth: login cancelled by user — exiting without notifying plugin");
        return;
    }

    if (code.empty()) {
        LogToFile("OAuth: auth code was empty (pipe error, not a user cancel)");
        SendToPlugin("{\"type\":\"login_failed\",\"error\":\"user_cancelled\"}");
        return;
    }

    LogToFile("OAuth: got auth code, exchanging for tokens");
    std::string response = ExchangeCodeForToken(code, verifier);

    std::string accessToken  = JsonExtractString(response, "access_token");
    std::string refreshToken = JsonExtractString(response, "refresh_token");

    if (accessToken.empty()) {
        LogToFile("OAuth: token exchange failed, no access_token in response");
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

    // Create the cancel event before flipping the in-progress flag so a
    // failure here doesn't leave the flag set with no cancel mechanism.
    HANDLE cancelEvent = CreateEventA(NULL, TRUE, FALSE, NULL);  // manual-reset
    if (!cancelEvent) {
        LogToFile("OAuth: CreateEvent for cancel failed");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_loginMutex);
        if (g_loginInProgress) {
            // Reject duplicate. Don't send login_failed: the plugin's
            // g_authInProgress is already true and showing "Cancel login",
            // which is the correct state — the previous OAuth attempt is
            // still wrapping up, and the user can click Cancel again to
            // get out (that path is a no-op on the engine but resets the
            // plugin's flag, restoring the "Login to Zoom" affordance).
            LogToFile("OAuth: StartLoginFlow rejected — login already in progress");
            CloseHandle(cancelEvent);
            return false;
        }
        g_loginInProgress  = true;
        g_loginCancelled   = false;
        g_loginCancelEvent = cancelEvent;
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
    if (g_loginCancelEvent) SetEvent(g_loginCancelEvent);
    // No handle closes here — SetEvent is non-blocking and the OAuth
    // thread owns g_loginCancelEvent's lifetime. PKCE verifier lives on
    // the OAuth thread's stack and dies with it.
}

} // namespace feeds_engine
