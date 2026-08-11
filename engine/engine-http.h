// engine-http.h — proxy-aware WinHTTP session helper (engine-side).
//
// Every engine WinHTTP call site used to open its session with
// WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, which reads ONLY the machine-wide WinHTTP
// proxy (netsh winhttp / proxycfg) and ignores WPAD/PAC and the per-user IE
// settings a browser honors. On managed networks (corporate/library) that
// forces every engine request DIRECT while the browser goes through the proxy
// — so OAuth completes in the browser but the engine's poll never reaches the
// worker, and it timed out and mislabeled the failure as user_cancelled.
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

#pragma once

#include <windows.h>
#include <winhttp.h>
#include <string>

namespace feeds_engine {

// Result of resolving the proxy for one target URL. Owns its strings (copied
// out of the WinHTTP structs, which are freed during resolution), so it is
// cheap to keep for the lifetime of a flow and re-apply to many sessions.
struct ProxyResolution {
    bool         useProxy = false;   // false => DIRECT (byte-for-byte as before)
    std::wstring proxy;              // "host:port" list, when useProxy
    std::wstring bypass;             // optional bypass list, when useProxy
    const char*  source = "none";    // "WPAD" | "PAC" | "user-config" | "none"
};

// Resolves the proxy for `url` using the current user's IE/WinINet settings:
//   - a static manual proxy               -> use it            (source "user-config")
//   - WPAD auto-detect / explicit PAC URL -> WinHttpGetProxyForUrl for this URL
//                                            (source "WPAD" / "PAC")
//   - none, or auto-detection failed      -> DIRECT            (source "none")
// Opens and closes its own short-lived session for the WPAD call and frees all
// GlobalFree-owned strings. Safe to call once per flow.
ProxyResolution ResolveProxyForUrl(const std::wstring& url);

// Emits the single per-flow "proxy resolved: <proxy|DIRECT> (source: X)" line.
// Logs at INFO when a proxy is in use (always worth recording) or when
// forceInfo is set (the login flow, where a DIRECT result is itself the key
// diagnostic); otherwise at DEBUG so routine DIRECT API calls don't spam.
void LogProxyResolution(const ProxyResolution& res, const char* flow, bool forceInfo);

// Opens a WinHTTP session for `userAgent` and applies, in order:
//   1. WINHTTP_OPTION_SECURE_PROTOCOLS = TLS 1.2 | TLS 1.3
//   2. the resolved proxy via WINHTTP_ACCESS_TYPE_NAMED_PROXY (or DIRECT)
//   3. WINHTTP_OPTION_AUTOLOGON_POLICY = LOW, when proxied, so the org proxy
//      gets the logged-on user's default credentials on a 407
// Returns the session handle (caller owns it and does WinHttpConnect /
// WinHttpOpenRequest exactly as before), or NULL if WinHttpOpen itself fails.
HINTERNET OpenProxiedSession(const wchar_t* userAgent, const ProxyResolution& res);

} // namespace feeds_engine
