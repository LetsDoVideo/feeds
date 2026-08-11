// engine-http.cpp — proxy-aware WinHTTP session helper. See engine-http.h.

#include "engine-http.h"

#include <string>

// TLS 1.3 secure-protocol flag is absent from older Windows SDK headers.
// Define it defensively so we can request it regardless of SDK vintage; on a
// platform without TLS 1.3 the flag is simply ignored and TLS 1.2 is used.
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

// Defined in engine-main.cpp
extern void LogToFile(const char* msg);  // forwards at DEBUG
extern void LogInfo(const char* msg);
extern void LogWarn(const char* msg);
extern void LogError(const char* msg);

namespace feeds_engine {

// Narrow a proxy string (ASCII "host:port" lists) to UTF-8 for logging.
static std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        &s[0], n, nullptr, nullptr);
    return s;
}

ProxyResolution ResolveProxyForUrl(const std::wstring& url)
{
    ProxyResolution out;  // defaults: DIRECT, source "none"

    // FeedsEngine.exe runs as a normal user process (launched by the plugin in
    // the OBS session), so the current-user IE proxy config is available.
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ie = {};
    if (!WinHttpGetIEProxyConfigForCurrentUser(&ie)) {
        // No IE config to read — keep DIRECT (unchanged behavior).
        return out;
    }

    if (ie.lpszProxy) {
        // A static/manual proxy is configured — use it verbatim.
        out.useProxy = true;
        out.proxy    = ie.lpszProxy;
        if (ie.lpszProxyBypass) out.bypass = ie.lpszProxyBypass;
        out.source   = "user-config";
    } else if (ie.fAutoDetect || ie.lpszAutoConfigUrl) {
        // WPAD auto-detect and/or an explicit PAC URL — resolve for this URL.
        HINTERNET hResolve = WinHttpOpen(L"Feeds/1.0",
                                         WINHTTP_ACCESS_TYPE_NO_PROXY,
                                         WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS, 0);
        if (hResolve) {
            WINHTTP_AUTOPROXY_OPTIONS opt = {};
            if (ie.fAutoDetect) {
                opt.dwFlags |= WINHTTP_AUTOPROXY_AUTO_DETECT;
                opt.dwAutoDetectFlags =
                    WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
                out.source = "WPAD";
            }
            if (ie.lpszAutoConfigUrl) {
                opt.dwFlags |= WINHTTP_AUTOPROXY_CONFIG_URL;
                opt.lpszAutoConfigUrl = ie.lpszAutoConfigUrl;
                out.source = "PAC";
            }

            // Best practice: try without auto-logon first (fast common path),
            // and retry with default credentials only if the PAC host itself
            // demands authentication to be downloaded.
            opt.fAutoLogonIfChallenged = FALSE;
            WINHTTP_PROXY_INFO info = {};
            BOOL ok = WinHttpGetProxyForUrl(hResolve, url.c_str(), &opt, &info);
            if (!ok && GetLastError() == ERROR_WINHTTP_LOGIN_FAILURE) {
                opt.fAutoLogonIfChallenged = TRUE;
                ok = WinHttpGetProxyForUrl(hResolve, url.c_str(), &opt, &info);
            }

            if (ok) {
                if (info.dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY &&
                    info.lpszProxy) {
                    out.useProxy = true;
                    out.proxy    = info.lpszProxy;
                    if (info.lpszProxyBypass) out.bypass = info.lpszProxyBypass;
                } else {
                    // Resolver says go direct for this URL.
                    out.useProxy = false;
                    out.source   = "none";
                }
                if (info.lpszProxy)       GlobalFree(info.lpszProxy);
                if (info.lpszProxyBypass) GlobalFree(info.lpszProxyBypass);
            } else {
                // ERROR_WINHTTP_AUTODETECTION_FAILED / auto-proxy service error
                // — no proxy could be located. Fall back to DIRECT.
                out.useProxy = false;
                out.source   = "none";
            }
            WinHttpCloseHandle(hResolve);
        }
    }

    if (ie.lpszProxy)         GlobalFree(ie.lpszProxy);
    if (ie.lpszProxyBypass)   GlobalFree(ie.lpszProxyBypass);
    if (ie.lpszAutoConfigUrl) GlobalFree(ie.lpszAutoConfigUrl);
    return out;
}

void LogProxyResolution(const ProxyResolution& res, const char* flow, bool forceInfo)
{
    std::string summary = res.useProxy ? Narrow(res.proxy) : std::string("DIRECT");
    std::string msg = std::string(flow) + ": proxy resolved: " + summary +
                      " (source: " + res.source + ")";
    if (res.useProxy || forceInfo) LogInfo(msg.c_str());
    else                           LogToFile(msg.c_str());
}

HINTERNET OpenProxiedSession(const wchar_t* userAgent, const ProxyResolution& res)
{
    HINTERNET hSession;
    if (res.useProxy) {
        hSession = WinHttpOpen(userAgent, WINHTTP_ACCESS_TYPE_NAMED_PROXY,
                               res.proxy.c_str(),
                               res.bypass.empty() ? WINHTTP_NO_PROXY_BYPASS
                                                  : res.bypass.c_str(),
                               0);
    } else {
        // DIRECT — identical to the prior WinHttpOpen at every call site.
        hSession = WinHttpOpen(userAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!hSession) return nullptr;

    // Modern TLS: request 1.2 + 1.3 explicitly rather than inheriting whatever
    // the OS default happens to be. On the working majority (already on 1.2)
    // this changes nothing negotiated; it only raises the floor on machines
    // still defaulting to 1.0/1.1.
    DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 |
                            WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS,
                     &secureProtocols, sizeof(secureProtocols));

    // Let the logged-on user's default credentials answer an NTLM/Negotiate
    // 407 from the org proxy automatically. Only meaningful when proxied, and
    // gated on it so the DIRECT path's server-auth behavior is untouched. Our
    // destinations are fixed Bearer-token hosts over validated TLS that never
    // NTLM-challenge, so the only challenger is the org's own proxy.
    if (res.useProxy) {
        DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_LOW;
        WinHttpSetOption(hSession, WINHTTP_OPTION_AUTOLOGON_POLICY,
                         &autologon, sizeof(autologon));
    }
    return hSession;
}

} // namespace feeds_engine
