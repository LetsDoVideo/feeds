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
#include <cstdio>

// Defined in engine-main.cpp
extern void LogToFile(const char* msg);
extern bool SendToPlugin(const std::string& json);

namespace feeds_engine {
bool AuthenticateSDK();  // defined in engine-sdk.cpp

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
        "&client_id=JlP6KfRqTt6r0t67FcDuqQ" +
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
        cred.Persist            = CRED_PERSIST_LOCAL_MACHINE;
        CredWriteA(&cred, 0);
    }
    {
        CREDENTIALA cred = {};
        cred.Type               = CRED_TYPE_GENERIC;
        cred.TargetName         = (LPSTR)"Feeds_RefreshToken";
        cred.CredentialBlobSize = (DWORD)refreshToken.size();
        cred.CredentialBlob     = (LPBYTE)refreshToken.data();
        cred.Persist            = CRED_PERSIST_LOCAL_MACHINE;
        CredWriteA(&cred, 0);
    }
}

// ---------------------------------------------------------------------------
// Named pipe listener — waits for FeedsLogin.exe to deliver the auth code
// via \\.\pipe\FeedsAuth
// ---------------------------------------------------------------------------

static std::string WaitForAuthCode()
{
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

    BOOL connected = ConnectNamedPipe(pipe, nullptr);
    if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
        LogToFile("OAuth: ConnectNamedPipe on FeedsAuth failed");
        CloseHandle(pipe);
        return "";
    }

    char buf[512] = {};
    DWORD bytesRead = 0;
    ReadFile(pipe, buf, sizeof(buf) - 1, &bytesRead, nullptr);
    CloseHandle(pipe);

    return std::string(buf, bytesRead);
}

// ---------------------------------------------------------------------------
// The full login flow, runs on a background thread.
// ---------------------------------------------------------------------------

static void LoginThreadFunc()
{
    LogToFile("OAuth: LoginThreadFunc started");

    std::string verifier  = GenerateCodeVerifier();
    std::string challenge = DeriveCodeChallenge(verifier);

    std::string authUrl =
        std::string("https://zoom.us/oauth/authorize") +
        "?response_type=code" +
        "&client_id=JlP6KfRqTt6r0t67FcDuqQ" +
        "&redirect_uri="       + UrlEncode("https://letsdovideo.com/loginsuccess") +
        "&code_challenge="     + challenge +
        "&code_challenge_method=S256" +
        "&prompt=consent";

    LogToFile("OAuth: opening browser to Zoom authorize endpoint");
    ShellExecuteA(NULL, "open", authUrl.c_str(), NULL, NULL, SW_SHOWNORMAL);

    LogToFile("OAuth: waiting for auth code via FeedsAuth pipe");
    std::string code = WaitForAuthCode();

    if (code.empty()) {
        LogToFile("OAuth: auth code was empty (user cancelled or pipe failed)");
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
// Public entry point — called from engine-main.cpp's HandleLoginStart
// ---------------------------------------------------------------------------

bool StartLoginFlow()
{
    LogToFile("OAuth: StartLoginFlow called");
    std::thread t(LoginThreadFunc);
    t.detach();
    return true;
}

} // namespace feeds_engine
