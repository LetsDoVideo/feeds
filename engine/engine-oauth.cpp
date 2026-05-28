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
// In-flight login state — guards the OAuth thread, the FeedsAuth pipe handle,
// and the cancel signal. Accessed from:
//   - the OAuth thread (LoginThreadFunc / WaitForAuthCode)
//   - the IPC pipe-reader thread (StartLoginFlow / CancelLoginFlow)
//
// g_loginThreadHandle is a duplicated HANDLE to the OAuth thread, captured
// before std::thread::detach() releases the original. CancelLoginFlow uses
// it to call CancelSynchronousIo, which is the only mechanism that actually
// unblocks the OAuth thread's synchronous ConnectNamedPipe / ReadFile on
// the FeedsAuth pipe. (CloseHandle on the pipe handle does NOT interrupt a
// synchronous wait on Windows — the pipe was created without
// FILE_FLAG_OVERLAPPED. Verified empirically: prior to this, cancel set the
// flags and closed the pipe but the OAuth thread stayed parked, so the next
// StartLoginFlow was silently rejected and the user could not log in
// without restarting OBS.)
//
// g_loginPipe is published by WaitForAuthCode after CreateNamedPipe; cancel
// still closes it as belt-and-suspenders, but CancelSynchronousIo is the
// operative call. Whichever side wins the ownership-transfer race
// (g_loginPipe -> nullptr) is responsible for the CloseHandle — the other
// side must not double-close. Same ownership model for g_loginThreadHandle.
//
// g_loginCancelled tells the OAuth thread that its unblock was a user cancel
// rather than a real auth code, so it exits silently instead of sending
// login_failed: user_cancelled (which would pop an error MessageBox over a
// user-initiated cancel).
// ---------------------------------------------------------------------------
static std::mutex g_loginMutex;
static bool       g_loginInProgress   = false;
static bool       g_loginCancelled    = false;
static HANDLE     g_loginPipe         = nullptr;
static HANDLE     g_loginThreadHandle = nullptr;

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

// outCancelled is set to true if CancelLoginFlow tore down the pipe while
// we were blocked on it (or before we got far enough to publish it). On
// cancel, the returned string is empty and the caller must NOT send
// login_failed to the plugin.
static std::string WaitForAuthCode(bool& outCancelled)
{
    outCancelled = false;

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = FALSE;

    HANDLE pipe = CreateNamedPipeA(
        "\\\\.\\pipe\\FeedsAuth",
        PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 256, 256, 300000, &sa);

    if (pipe == INVALID_HANDLE_VALUE) {
        LogToFile("OAuth: failed to create FeedsAuth pipe");
        return "";
    }

    // Publish the handle for CancelLoginFlow, after re-checking the cancel
    // flag — a cancel that arrived between LoginThreadFunc setting
    // g_loginInProgress and us reaching this point would otherwise be
    // lost (nothing to close yet) and the thread would block forever.
    {
        std::lock_guard<std::mutex> lock(g_loginMutex);
        if (g_loginCancelled) {
            CloseHandle(pipe);
            outCancelled = true;
            return "";
        }
        g_loginPipe = pipe;
    }

    BOOL  connected = ConnectNamedPipe(pipe, nullptr);
    DWORD connectErr = GetLastError();

    {
        char dbg[160];
        sprintf_s(dbg, "OAuth: [diag] ConnectNamedPipe returned connected=%d err=%lu",
                  (int)connected, connectErr);
        LogToFile(dbg);
    }

    char  buf[512]    = {};
    DWORD bytesRead   = 0;
    if (connected || connectErr == ERROR_PIPE_CONNECTED) {
        ReadFile(pipe, buf, sizeof(buf) - 1, &bytesRead, nullptr);
    } else {
        // Either CancelLoginFlow closed the handle from under us
        // (ERROR_INVALID_HANDLE / ERROR_OPERATION_ABORTED) or a real
        // failure. The cancel-flag check below disambiguates.
        LogToFile("OAuth: ConnectNamedPipe on FeedsAuth returned an error");
    }

    // Take ownership of the pipe handle back. If cancel already grabbed it
    // (g_loginPipe == nullptr), it has closed/will close the handle and we
    // must not double-close.
    bool cancelled;
    {
        std::lock_guard<std::mutex> lock(g_loginMutex);
        cancelled = g_loginCancelled;
        if (g_loginPipe == pipe) {
            CloseHandle(pipe);
            g_loginPipe = nullptr;
        }
    }
    outCancelled = cancelled;

    if (cancelled) return "";
    return std::string(buf, bytesRead);
}

// ---------------------------------------------------------------------------
// The full login flow, runs on a background thread.
// ---------------------------------------------------------------------------

// RAII clear of in-flight state on thread exit. Guarantees every return path
// resets the flags so the next StartLoginFlow isn't rejected, even if a
// future edit adds a new early-return.
//
// Also closes our duplicated thread handle if cancel didn't already take it.
// The handle has to be closed exactly once — whichever of LoginGuard or
// CancelLoginFlow nulls g_loginThreadHandle first owns the CloseHandle.
namespace {
struct LoginGuard {
    ~LoginGuard() {
        HANDLE handleToClose = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_loginMutex);
            g_loginInProgress = false;
            g_loginCancelled  = false;
            handleToClose       = g_loginThreadHandle;
            g_loginThreadHandle = nullptr;
            // g_loginPipe is cleared by WaitForAuthCode in both branches;
            // if we somehow got here with it still set, the handle is
            // still ours and we leak it rather than risk a double-close
            // vs. a racing CancelLoginFlow that may have already started.
        }
        if (handleToClose) CloseHandle(handleToClose);
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
            return false;
        }
        g_loginInProgress   = true;
        g_loginCancelled    = false;
        g_loginPipe         = nullptr;
        g_loginThreadHandle = nullptr;
    }

    std::thread t(LoginThreadFunc);

    // Duplicate the thread handle so CancelLoginFlow can call
    // CancelSynchronousIo on it after t.detach() releases the original.
    // THREAD_TERMINATE is the access right MSDN requires for
    // CancelSynchronousIo. If the duplicate fails the OAuth thread still
    // runs to completion normally; only cancel is degraded (back to the
    // pipe-close-only path, which empirically does not interrupt the
    // synchronous ConnectNamedPipe wait — so cancel becomes effectively
    // a no-op until the SDK side times out).
    HANDLE threadDup = nullptr;
    HANDLE nativeHandle = t.native_handle();
    if (!DuplicateHandle(GetCurrentProcess(), nativeHandle,
                         GetCurrentProcess(), &threadDup,
                         THREAD_TERMINATE, FALSE, 0)) {
        char buf[128];
        sprintf_s(buf, "OAuth: DuplicateHandle for OAuth thread failed: %lu",
                  GetLastError());
        LogToFile(buf);
        threadDup = nullptr;
    }
    {
        char dbg[160];
        sprintf_s(dbg, "OAuth: [diag] native_handle=%p threadDup=%p",
                  nativeHandle, threadDup);
        LogToFile(dbg);
    }
    {
        std::lock_guard<std::mutex> lock(g_loginMutex);
        g_loginThreadHandle = threadDup;
    }

    t.detach();
    return true;
}

void CancelLoginFlow()
{
    LogToFile("OAuth: CancelLoginFlow called");
    HANDLE pipeToClose   = nullptr;
    HANDLE threadToCancel = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_loginMutex);
        if (!g_loginInProgress) {
            // No-op if no OAuth flow is running. Happens when the plugin
            // sends a defensive cancel during the post-cancel race window
            // (engine already cleared the flag from a prior cancel), or
            // when the plugin's auth-in-progress was set without a real
            // round-trip (shouldn't happen, but harmless).
            LogToFile("OAuth: cancel ignored — no login in progress");
            return;
        }
        g_loginCancelled    = true;
        pipeToClose         = g_loginPipe;
        g_loginPipe         = nullptr;
        threadToCancel      = g_loginThreadHandle;
        g_loginThreadHandle = nullptr;  // take ownership; LoginGuard will skip close
    }

    {
        char dbg[160];
        sprintf_s(dbg, "OAuth: [diag] cancel: pipeToClose=%p threadToCancel=%p",
                  pipeToClose, threadToCancel);
        LogToFile(dbg);
    }

    // Belt: closes the pipe handle. On its own this does NOT unblock a
    // synchronous ConnectNamedPipe — kept anyway because it does reliably
    // unblock a synchronous ReadFile (the later phase) on some Windows
    // versions, and costs nothing if the thread is already past it.
    LogToFile("OAuth: [diag] cancel: about to CloseHandle(pipe)");
    if (pipeToClose) CloseHandle(pipeToClose);
    LogToFile("OAuth: [diag] cancel: pipe CloseHandle returned");

    // Suspenders: the operative call. CancelSynchronousIo aborts whatever
    // synchronous syscall the OAuth thread is currently parked on
    // (ConnectNamedPipe or ReadFile), which returns ERROR_OPERATION_ABORTED.
    // WaitForAuthCode's takeover block then sees g_loginCancelled and the
    // thread exits via LoginGuard without sending login_failed.
    //
    // ERROR_NOT_FOUND means the thread isn't currently in a cancellable
    // synchronous op (e.g. cancel arrived during token exchange, which
    // uses async WinHTTP and can't be cancelled this way). That's fine —
    // we accept that a late-stage cancel may complete the login anyway
    // per the design contract.
    if (threadToCancel) {
        BOOL  ok  = CancelSynchronousIo(threadToCancel);
        DWORD err = GetLastError();
        {
            char dbg[160];
            sprintf_s(dbg, "OAuth: [diag] CancelSynchronousIo=%s lastError=%lu",
                      ok ? "TRUE" : "FALSE", err);
            LogToFile(dbg);
        }
        if (!ok && err != ERROR_NOT_FOUND) {
            char buf[128];
            sprintf_s(buf, "OAuth: CancelSynchronousIo failed: %lu", err);
            LogToFile(buf);
        }
        LogToFile("OAuth: [diag] cancel: about to CloseHandle(thread)");
        CloseHandle(threadToCancel);
        LogToFile("OAuth: [diag] cancel: thread CloseHandle returned");
    }
    LogToFile("OAuth: [diag] cancel: returning");
    // PKCE verifier lives on LoginThreadFunc's stack and dies with the
    // thread, so there's nothing additional to drop here. Same for the
    // refresh/access tokens (never reach this side on cancel).
}

} // namespace feeds_engine
