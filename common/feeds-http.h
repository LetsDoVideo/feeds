// feeds-http.h — proxy-aware WinHTTP session helper, shared by BOTH binaries.
//
// Relocated from engine/engine-http.{h,cpp} so FeedsEngine.exe and the OBS
// plugin module use one copy of this logic instead of two. The proxy
// resolution itself exists in exactly one place: this file.
//
// Why it exists: opening a WinHTTP session with WINHTTP_ACCESS_TYPE_DEFAULT_PROXY
// reads ONLY the machine-wide WinHTTP proxy (netsh winhttp / proxycfg) and
// ignores WPAD/PAC and the per-user IE settings a browser honors. On managed
// networks (corporate/library) that forces every request DIRECT while the
// browser goes through the proxy — so the request silently never arrives.
// 1.6.1 fixed that for the engine's login/tier calls; this file is what lets
// the plugin's chat and update-check calls share the same fix.
//
// This helper resolves the user's proxy for a target URL the same way the
// browser does (WinHttpGetIEProxyConfigForCurrentUser → WinHttpGetProxyForUrl),
// then opens a session that applies it, enables TLS 1.2/1.3 explicitly, and
// sets the auto-logon policy so an NTLM/Kerberos org proxy authenticates
// transparently. When no proxy is configured it falls back to exactly the
// prior DIRECT behavior, so the working majority is unchanged.
//
// Usage — resolve ONCE per flow (so WPAD/PAC runs once and the proxy line is
// logged once), then open a session per request from the cached result:
//   ProxyResolution px = ResolveProxyForUrl(L"https://host/path");
//   LogProxyResolution(px, "OAuth", /*forceInfo=*/true);
//   HINTERNET h = OpenProxiedSession(L"Feeds/1.0", px);
//   // ... WinHttpSetTimeouts / WinHttpConnect / WinHttpOpenRequest as before
//
// Header-only, following the common/ precedent (feeds-json-lite.h,
// feeds-ipc-reassembly.h): both targets already have common/ on their include
// path, so sharing this needs no new build-target source entries.

#pragma once

#include <windows.h>
#include <winhttp.h>
#include <string>

// TLS 1.3 secure-protocol flag is absent from older Windows SDK headers.
// Define it defensively so we can request it regardless of SDK vintage; on a
// platform without TLS 1.3 the flag is simply ignored and TLS 1.2 is used.
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

namespace feeds_http {

// Result of resolving the proxy for one target URL. Owns its strings (copied
// out of the WinHTTP structs, which are freed during resolution), so it is
// cheap to keep for the lifetime of a flow and re-apply to many sessions.
struct ProxyResolution {
    bool         useProxy = false;   // false => DIRECT (byte-for-byte as before)
    std::wstring proxy;              // "host:port" list, when useProxy
    std::wstring bypass;             // optional bypass list, when useProxy
    const char*  source = "none";    // "WPAD" | "PAC" | "user-config" | "none"
};

// ---------------------------------------------------------------------------
// The helper's ONE outward dependency, and the reason it can live in common/.
//
// Each binary defines this exactly once, forwarding to its own logging stack:
//   FeedsEngine.exe   -> engine-main.cpp, forwards to LogInfo / LogToFile
//   the plugin module -> plugin-main.cpp, forwards to blog(LOG_INFO/LOG_DEBUG)
//
// `important` is true when the line deserves INFO (a proxy is actually in use,
// or the caller forced it); false means DEBUG-level detail. One shim function
// per binary rather than the four Log* externs the engine-only version used —
// ResolveProxyForUrl and OpenProxiedSession log nothing at all, so LogWarn and
// LogError were never called and are not carried over.
// ---------------------------------------------------------------------------
void HttpLog(bool important, const char* msg);

namespace detail {

// Narrow a proxy string (ASCII "host:port" lists) to UTF-8 for logging.
inline std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        &s[0], n, nullptr, nullptr);
    return s;
}

} // namespace detail

// Resolves the proxy for `url` using the current user's IE/WinINet settings:
//   - a static manual proxy               -> use it            (source "user-config")
//   - WPAD auto-detect / explicit PAC URL -> WinHttpGetProxyForUrl for this URL
//                                            (source "WPAD" / "PAC")
//   - none, or auto-detection failed      -> DIRECT            (source "none")
// Opens and closes its own short-lived session for the WPAD call and frees all
// GlobalFree-owned strings. Safe to call once per flow.
inline ProxyResolution ResolveProxyForUrl(const std::wstring& url)
{
    ProxyResolution out;  // defaults: DIRECT, source "none"

    // Both binaries run as normal user processes in the OBS session (the engine
    // is launched by the plugin), so the current-user IE proxy config is
    // available to each of them.
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
        // The UA on this handle is only ever seen by a PAC/WPAD host during
        // resolution; it never carries application traffic, so it stays the
        // neutral "Feeds/1.0" regardless of the caller's own user agent.
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

// Emits the single per-flow "proxy resolved: <proxy|DIRECT> (source: X)" line.
// Logs at INFO when a proxy is in use (always worth recording) or when
// forceInfo is set (the login flow, where a DIRECT result is itself the key
// diagnostic); otherwise at DEBUG so routine DIRECT API calls don't spam.
inline void LogProxyResolution(const ProxyResolution& res, const char* flow,
                               bool forceInfo)
{
    std::string summary =
        res.useProxy ? detail::Narrow(res.proxy) : std::string("DIRECT");
    std::string msg = std::string(flow) + ": proxy resolved: " + summary +
                      " (source: " + res.source + ")";
    HttpLog(res.useProxy || forceInfo, msg.c_str());
}

// Opens a WinHTTP session for `userAgent` and applies, in order:
//   1. WINHTTP_OPTION_SECURE_PROTOCOLS = TLS 1.2 | TLS 1.3
//   2. the resolved proxy via WINHTTP_ACCESS_TYPE_NAMED_PROXY (or DIRECT)
//   3. WINHTTP_OPTION_AUTOLOGON_POLICY = LOW, when proxied, so the org proxy
//      gets the logged-on user's default credentials on a 407
// Returns the session handle (caller owns it and does WinHttpConnect /
// WinHttpOpenRequest exactly as before), or NULL if WinHttpOpen itself fails.
inline HINTERNET OpenProxiedSession(const wchar_t* userAgent,
                                    const ProxyResolution& res)
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
    // destinations are fixed public hosts over validated TLS that never
    // NTLM-challenge, so the only challenger is the org's own proxy.
    if (res.useProxy) {
        DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_LOW;
        WinHttpSetOption(hSession, WINHTTP_OPTION_AUTOLOGON_POLICY,
                         &autologon, sizeof(autologon));
    }
    return hSession;
}

} // namespace feeds_http

// ---------------------------------------------------------------------------
// The engine's six call sites live inside `namespace feeds_engine` and call
// these unqualified. Re-export the names so relocating the helper out of
// engine/ needs no edits at any of them — the only engine change is the
// #include path. New code should prefer feeds_http:: directly.
// ---------------------------------------------------------------------------
namespace feeds_engine {
using feeds_http::ProxyResolution;
using feeds_http::ResolveProxyForUrl;
using feeds_http::LogProxyResolution;
using feeds_http::OpenProxiedSession;
}
