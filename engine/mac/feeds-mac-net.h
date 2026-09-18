// feeds-mac-net.h — the platform primitives the Mac login path needs:
// HTTPS, PKCE crypto, Keychain storage, and opening a browser.
//
// The Windows engine gets these from WinHTTP, CryptoAPI and Credential Manager
// (engine-oauth.cpp / engine-api.cpp). None of that exists here, so this is the
// macOS equivalent behind a narrow C++ interface: the login logic in
// feeds-mac-login.mm then reads as the same flow as the Windows one, with the
// platform confined to this file.
//
// THREADING: every call here BLOCKS. None of them may run on the main thread —
// that thread belongs to the Cocoa run loop, and starving it stops SDK delegate
// callbacks from being delivered at all. Callers run on the engine's own
// background threads.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace feeds_mac {

// ---------------------------------------------------------------------------
// HTTPS
// ---------------------------------------------------------------------------
struct HttpResponse {
    // True when we got an answer from the server, whatever its status. False
    // means the request never completed: DNS, TLS, proxy, firewall, timeout.
    // The distinction matters upstream: a transport failure must not be read as
    // "the account has no entitlement" or "the credentials are dead".
    bool        reached = false;
    int         status  = 0;
    std::string body;
    std::string error;   // transport error description, for logs only
};

// Both take a full https:// URL and a list of header name/value pairs.
// PostForm sends application/x-www-form-urlencoded unless a Content-Type is
// supplied in `headers`.
HttpResponse HttpGet(const std::string& url,
                     const std::vector<std::pair<std::string, std::string>>& headers,
                     int timeoutSeconds);
HttpResponse HttpPostForm(const std::string& url, const std::string& body,
                          const std::vector<std::pair<std::string, std::string>>& headers,
                          int timeoutSeconds);

// ---------------------------------------------------------------------------
// PKCE / encoding
// ---------------------------------------------------------------------------
std::string UrlEncode(const std::string& s);

// 32 cryptographically random bytes, base64url without padding. Used for both
// the PKCE verifier and the OAuth state, exactly as on Windows.
std::string RandomUrlSafeToken();

// base64url(SHA-256(input)), no padding: the PKCE S256 challenge.
std::string Sha256Base64Url(const std::string& input);

// ---------------------------------------------------------------------------
// Keychain (replaces Windows Credential Manager)
//
// One generic-password item per account name, all under one service. The items
// are per-macOS-user, which is the property that matters: a second user on the
// same Mac must never inherit the first user's tokens or tier.
//
// FIRST RUN PROMPTS ONCE. macOS asks the user to allow this binary to use the
// item it just created; that is normal and expected. A build signed with a
// different identity is a different binary to the Keychain, so an unsigned or
// ad-hoc CI build can prompt again after an update — worth knowing before it is
// mistaken for a bug.
// ---------------------------------------------------------------------------
bool        KeychainSet(const std::string& account, const std::string& value);
std::string KeychainGet(const std::string& account);   // "" when absent
void        KeychainDelete(const std::string& account);

// ---------------------------------------------------------------------------
// Preferences — per-user storage for things that are NOT secrets
//
// Every Keychain touch can cost the user a password prompt, so anything that
// does not need confidentiality does not belong there. These are NSUserDefaults
// under the engine's own domain: per-macOS-user, which is the property the
// cached tier actually needs (a second user on this Mac must not inherit the
// first user's entitlement), without the authorization cost.
//
// PrefsGetInt returns `fallback` when the key has never been set, which is how
// callers tell "never stored" from a stored zero.
// ---------------------------------------------------------------------------
void PrefsSetInt(const std::string& key, int value);
int  PrefsGetInt(const std::string& key, int fallback);
void PrefsRemove(const std::string& key);

// ---------------------------------------------------------------------------
// Browser
// ---------------------------------------------------------------------------
// Hands the URL to the user's default browser (the Windows engine's
// ShellExecute "open"). Returns false when Launch Services refuses it.
bool OpenInBrowser(const std::string& url);

} // namespace feeds_mac
