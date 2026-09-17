// feeds-mac-login.mm — the macOS implementation of the Zoom login path.
//
// Logic mirrors the Windows engine (engine-oauth.cpp + engine-api.cpp) step for
// step; only the platform calls differ. Where the Windows code made a decision
// for a stated reason, that reason is restated here rather than assumed, because
// the failure modes are the ones that cost users their session:
//   * a failed user-info fetch must distinguish "credentials are dead" (clear
//     them) from "we could not reach Zoom" (keep them);
//   * a failed tier query must keep the last known good tier, or a paying
//     customer on a blocked network silently becomes a Free one;
//   * a cancelled login must not report itself as a failure.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "feeds-backend.h"
#include "feeds-json-lite.h"
#include "feeds-mac-login.h"
#include "feeds-mac-net.h"

namespace feeds_mac {

namespace {

// Keychain account names. The two OAuth tokens share one Keychain item: they
// are one lifecycle unit — written together, read together, deleted together —
// and every item carries its own access-control list, so storing them
// separately made macOS ask the user for permission twice in a row, two
// identical panels stacked on top of each other with no way to tell them
// apart. One item is one authorization.
//
// The access and refresh tokens are separated by a newline. Both are URL-safe
// OAuth token strings and cannot contain one, and the split takes the FIRST
// newline as the boundary, so nothing unexpected in the refresh token can
// corrupt the access token.
const char* const kSessionItem      = "Feeds_Session";
const char* const kCachedTierItem   = "Feeds_CachedTier";

// Superseded by kSessionItem. Read once to migrate an existing login, then
// deleted; a user who already signed in keeps their session across the change
// instead of being silently logged out.
const char* const kLegacyAccessItem  = "Feeds_AccessToken";
const char* const kLegacyRefreshItem = "Feeds_RefreshToken";

const char* const kRedirectUri = "https://letsdovideo.com/loginsuccess";

// ---------------------------------------------------------------------------
// Session state
// ---------------------------------------------------------------------------
std::mutex  g_stateMutex;
std::string g_accessToken;
std::string g_refreshToken;
std::string g_displayName;
std::string g_pmi;
int         g_currentTier    = 0;
bool        g_tierUnresolved = false;

std::mutex g_loginMutex;
bool       g_loginInProgress = false;
bool       g_loginCancelled  = false;

// session_expired is announced from deep inside an API call, so a caller that
// later decides to fail closed cannot otherwise tell whether the user has
// already been told. Sampling this counter across a fetch is what stops two
// identical modals landing back to back.
std::atomic<unsigned> g_sessionExpiredNotices{0};

void LogInfo(const std::string& m)  { EngineLog("info", m); }
void LogDebug(const std::string& m) { EngineLog("debug", m); }
void LogWarn(const std::string& m)  { EngineLog("warning", m); }
void LogError(const std::string& m) { EngineLog("error", m); }

void NotifySessionExpired()
{
    g_sessionExpiredNotices.fetch_add(1, std::memory_order_relaxed);
    EngineSend("{\"type\":\"session_expired\"}");
}

// A JSON number as its literal text, or "" when the key is absent. The PMI is a
// number in Zoom's JSON but a string everywhere else in this protocol, and an
// absent PMI must stay empty rather than become "0" — the plugin blocks a
// PMI join on an empty value, and "0" would look like a real meeting number.
std::string ExtractNumberText(const std::string& json, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '"')) ++pos;
    std::string out;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9')
        out += json[pos++];
    return out;
}

bool IsLoginCancelled()
{
    std::lock_guard<std::mutex> lock(g_loginMutex);
    return g_loginCancelled;
}

// ---------------------------------------------------------------------------
// Token storage
// ---------------------------------------------------------------------------
void SaveTokens()
{
    std::string access, refresh;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        access  = g_accessToken;
        refresh = g_refreshToken;
    }
    // One item means one write, so it is the whole session that gets stored or
    // nothing. A session with no refresh token cannot be restored anyway, and
    // writing one would replace a good stored refresh token with nothing — a
    // silent logout on the next start. Both callers hold both tokens by the
    // time they get here; this keeps that a guarantee rather than a habit.
    if (refresh.empty()) return;
    KeychainSet(kSessionItem, access + "\n" + refresh);
}

// Both tokens in one Keychain read, so the user is asked once.
//
// A login stored by an earlier build lives in the two separate items; read
// those once, rewrite them as the single item, and remove them. That migration
// still costs the old two prompts, but only on the first run after the change.
// Returns false when there is no usable stored session, which is the ordinary
// state of a user who has not logged in yet, not an error. On true, refresh is
// non-empty: that is the token a restore actually needs.
bool LoadStoredTokens(std::string& access, std::string& refresh)
{
    access.clear();
    refresh.clear();

    const std::string blob = KeychainGet(kSessionItem);
    if (!blob.empty()) {
        const size_t nl = blob.find('\n');
        if (nl == std::string::npos) {
            // SaveTokens never writes a value without the separator, so this is
            // a truncated or hand-edited item. There is no way to tell which
            // token it holds, so treat the session as unusable and let the user
            // sign in again rather than guess.
            LogWarn("Login: the stored session is malformed; ignoring it");
            return false;
        }
        access  = blob.substr(0, nl);
        refresh = blob.substr(nl + 1);
        return !refresh.empty();
    }

    access  = KeychainGet(kLegacyAccessItem);
    refresh = KeychainGet(kLegacyRefreshItem);
    if (refresh.empty()) {
        // Nothing worth carrying over: without the refresh token there is no
        // session to restore. Leave the old items alone — the next successful
        // login writes the single item and this fallback stops running.
        access.clear();
        return false;
    }

    LogDebug("Login: migrating the stored session to a single Keychain item");
    if (KeychainSet(kSessionItem, access + "\n" + refresh)) {
        KeychainDelete(kLegacyAccessItem);
        KeychainDelete(kLegacyRefreshItem);
    }
    return true;
}

// The last-known-good tier lives beside the tokens on purpose: it inherits
// their per-user scope and dies with them on logout, so a second macOS user on
// the same machine can never inherit the first user's entitlement. The tier is
// not a secret; the Keychain is used here for lifecycle, not confidentiality.
void SaveCachedTier(int tier)
{
    KeychainSet(kCachedTierItem, std::to_string(tier));
}

// -1 means "never cached": the brand-new user who has not once reached the
// backend. Deliberately distinct from a cached 0, which is a real answer and
// must still suppress the "couldn't reach licensing" warning.
int LoadCachedTier()
{
    const std::string v = KeychainGet(kCachedTierItem);
    if (v.empty()) return -1;
    const int tier = atoi(v.c_str());
    return (tier >= 0 && tier <= 3) ? tier : -1;
}

// ---------------------------------------------------------------------------
// OAuth token refresh
// ---------------------------------------------------------------------------
bool RefreshAccessToken()
{
    std::string refresh;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        refresh = g_refreshToken;
    }
    if (refresh.empty()) {
        std::string storedAccess;
        if (!LoadStoredTokens(storedAccess, refresh)) return false;
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_refreshToken = refresh;
    }

    // Refresh tokens are already URL-safe, as on Windows.
    const std::string body =
        std::string("grant_type=refresh_token&refresh_token=") + refresh +
        "&client_id=" + FEEDS_ZOOM_CLIENT_ID;

    const HttpResponse r =
        HttpPostForm("https://zoom.us/oauth/token", body, {}, 15);
    if (!r.reached) {
        LogWarn("API: token refresh could not reach Zoom (" + r.error + ")");
        return false;
    }

    const std::string newAccess  = feeds::ExtractJsonString(r.body, "access_token");
    const std::string newRefresh = feeds::ExtractJsonString(r.body, "refresh_token");
    if (newAccess.empty()) {
        LogWarn("API: token refresh failed (no access_token in the response)");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_accessToken = newAccess;
        if (!newRefresh.empty()) g_refreshToken = newRefresh;
    }
    SaveTokens();
    EngineSend("{\"type\":\"token_refreshed\"}");
    LogDebug("API: access token refreshed");
    return true;
}

// ---------------------------------------------------------------------------
// Authenticated GET to api.zoom.us, with one transparent refresh-and-retry on
// 401 — the same contract as the Windows ZoomApiGetWithStatus.
// ---------------------------------------------------------------------------
std::string ZoomApiGet(const std::string& path, int& outStatus)
{
    auto once = [&](int& status) -> std::string {
        std::string token;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            token = g_accessToken;
        }
        const HttpResponse r = HttpGet("https://api.zoom.us" + path,
                                       {{"Authorization", "Bearer " + token}}, 15);
        status = r.reached ? r.status : 0;
        if (!r.reached) LogDebug("API: GET " + path + " failed: " + r.error);
        return r.body;
    };

    int status = 0;
    std::string body = once(status);

    if (status == 401) {
        LogDebug("API: 401, attempting a token refresh");
        if (RefreshAccessToken()) {
            body = once(status);
        } else {
            LogWarn("API: refresh failed, the session has expired");
            NotifySessionExpired();
            outStatus = 401;
            return {};
        }
    }

    outStatus = status;
    return body;
}

enum class UserInfoResult { Ok, SessionExpired, Unreachable };

// Ok is the ONLY result that proves the stored credentials work. The two
// failures are kept apart because only one of them may delete the tokens.
UserInfoResult FetchUserInfo()
{
    LogDebug("API: fetching user info");
    int status = 0;
    const std::string response = ZoomApiGet("/v2/users/me", status);

    if (status == 401) {
        LogWarn("API: user info unauthorized — the stored credentials are dead");
        return UserInfoResult::SessionExpired;
    }
    if (response.empty()) {
        LogDebug("API: user info returned nothing (transport, proxy or 5xx)");
        return UserInfoResult::Unreachable;
    }

    std::string name = feeds::ExtractJsonString(response, "display_name");
    if (name.empty()) name = feeds::ExtractJsonString(response, "first_name");

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_displayName = name;
        g_pmi         = ExtractNumberText(response, "pmi");
    }

    // A 200 with no usable name leaves the join paths without the display name
    // they require, so it is not a usable login — but it is not proof the
    // credentials are dead either, so it takes the keep-the-tokens path.
    if (name.empty()) {
        LogWarn("API: user info had no display name");
        return UserInfoResult::Unreachable;
    }

    // Redaction: never log the name, PMI or email — this lands in the shared
    // OBS log.
    LogDebug("API: user info fetched");
    return UserInfoResult::Ok;
}

// The tier comes from the Feeds entitlement backend, not from Zoom: Zoom's
// entitlements endpoint does not report Local Test licenses, and the
// monetization signal arrives at the backend by webhook.
void FetchAndApplyEntitlement()
{
    const int cached = LoadCachedTier();
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_currentTier    = (cached >= 0) ? cached : 0;
        g_tierUnresolved = false;
    }

    // One place decides what a failure means, so no early return has to reason
    // about the cache: with a cached tier we keep it and stay quiet; without
    // one the user really is landing on Free involuntarily, and the plugin
    // surfaces that as a network warning.
    auto giveUp = [&](const std::string& why) {
        if (cached >= 0) {
            LogWarn("API: tier query failed (" + why + ") — keeping cached tier " +
                    std::to_string(cached));
        } else {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_tierUnresolved = true;
            LogWarn("API: tier query failed (" + why +
                    ") — no cached tier, applying Free");
        }
    };

    std::string token;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        token = g_accessToken;
    }

    const HttpResponse r = HttpGet(FEEDS_BACKEND_ORIGIN "/tier",
                                   {{"Authorization", "Bearer " + token}}, 15);
    if (!r.reached)      { giveUp(r.error.empty() ? "network error" : r.error); return; }
    if (r.status != 200) { giveUp("HTTP " + std::to_string(r.status));          return; }

    const std::string tierText = ExtractNumberText(r.body, "tier");
    if (tierText.empty()) { giveUp("no tier field in the response"); return; }

    const int tier = atoi(tierText.c_str());
    if (tier < 0 || tier > 3) { giveUp("tier out of range"); return; }

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_currentTier = tier;
    }
    SaveCachedTier(tier);
    LogInfo("API: entitlement tier " + std::to_string(tier));
}

// Announce a restored or completed login. Order matters: login_succeeded
// populates the plugin's cache, then sdk_authenticated flips the UI to
// logged-in. The Zoom SDK is not touched — "ready to connect" is what the
// plugin's handler actually needs, and the SDK comes up on the first connect.
void AnnounceLoginSucceeded()
{
    const unsigned noticesBefore =
        g_sessionExpiredNotices.load(std::memory_order_relaxed);

    const UserInfoResult info = FetchUserInfo();

    // Fail closed: anything short of a live 200 means we cannot honestly claim
    // the credentials work, so announce the logged-out state instead. What
    // differs between the two failures is only whether we may throw the stored
    // credentials away.
    if (info != UserInfoResult::Ok) {
        if (info == UserInfoResult::SessionExpired) {
            LogWarn("Login: stored credentials are dead — clearing them");
            ClearStoredCredentials();
            if (g_sessionExpiredNotices.load(std::memory_order_relaxed) == noticesBefore)
                EngineSend("{\"type\":\"session_expired\"}");
        } else {
            // Unreachable: proxy, firewall, no network. The credentials are
            // very likely fine and deleting them would cost a full re-login
            // over a blip, so keep them and report something retryable.
            LogWarn("Login: could not reach Zoom — staying logged out, "
                    "keeping the stored credentials");
            EngineSend("{\"type\":\"login_failed\","
                       "\"error\":\"session_restore_unreachable\"}");
        }
        return;
    }

    FetchAndApplyEntitlement();

    std::string name, pmi;
    int tier = 0;
    bool tierUnresolved = false;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        name           = g_displayName;
        pmi            = g_pmi;
        tier           = g_currentTier;
        tierUnresolved = g_tierUnresolved;
    }

    EngineSend("{\"type\":\"login_succeeded\",\"display_name\":\"" +
               feeds::JsonEscape(name) + "\",\"pmi\":\"" + pmi +
               "\",\"tier\":" + std::to_string(tier) + "}");
    EngineSend("{\"type\":\"sdk_authenticated\"}");

    // Last, deliberately: the plugin puts a modal up for this, and a modal
    // blocks its event loop — announcing it earlier would stall the two
    // messages above behind it and leave the window looking stuck mid-login.
    if (tierUnresolved) EngineSend("{\"type\":\"tier_unreachable\"}");
}

// ---------------------------------------------------------------------------
// The OAuth flow
// ---------------------------------------------------------------------------

// Polls the Feeds worker for the auth code the loginsuccess page posted there,
// keyed by our state:
//   200 {"code":"..."} — ready
//   204                — not yet; wait and poll again
// About every 1.5 s for roughly two minutes. The cancel flag is checked every
// iteration and during the wait, so Cancel takes effect promptly. On cancel the
// result is empty and outCancelled is set, and the caller must NOT report a
// failure.
std::string PollWorkerForAuthCode(const std::string& state, bool& outCancelled)
{
    outCancelled = false;

    const int kPollIntervalMs = 1500;
    const int kMaxAttempts    = 120000 / kPollIntervalMs;
    const std::string url =
        FEEDS_BACKEND_ORIGIN "/authresult?state=" + UrlEncode(state);

    int failures = 0;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (IsLoginCancelled()) { outCancelled = true; return {}; }

        const HttpResponse r = HttpGet(url, {}, 8);
        if (r.reached && r.status == 200) {
            const std::string code = feeds::ExtractJsonString(r.body, "code");
            if (!code.empty()) return code;
        }

        // Diagnostics for a login that never completes: the first failure and
        // then sparsely, so a stuck login's log separates a plain timeout from
        // a blocked host or a TLS-inspecting proxy.
        if (!r.reached || (r.status != 200 && r.status != 204)) {
            ++failures;
            if (failures == 1 || failures % 20 == 0) {
                LogWarn("OAuth: worker poll attempt " + std::to_string(attempt + 1) +
                        (r.reached ? " got HTTP " + std::to_string(r.status)
                                   : " failed: " + r.error));
            }
        }

        // Wait in slices so a cancel does not have to wait out the interval.
        for (int slept = 0; slept < kPollIntervalMs; slept += 100) {
            if (IsLoginCancelled()) { outCancelled = true; return {}; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    return {};
}

void LoginThread()
{
    // Clear the in-flight state on every exit path, including future ones.
    struct Guard {
        ~Guard() {
            std::lock_guard<std::mutex> lock(g_loginMutex);
            g_loginInProgress = false;
            g_loginCancelled  = false;
        }
    } guard;

    const std::string verifier  = RandomUrlSafeToken();
    const std::string challenge = Sha256Base64Url(verifier);
    // The state is only an unguessable correlator between our browser session
    // and the worker's stored code; the code itself stays PKCE-protected and
    // the verifier never leaves this process.
    const std::string state = RandomUrlSafeToken();

    if (verifier.empty() || state.empty()) {
        LogError("OAuth: the system random source failed; login aborted");
        EngineSend("{\"type\":\"login_failed\",\"error\":\"token_exchange_failed\"}");
        return;
    }

    const std::string authUrl =
        std::string("https://zoom.us/oauth/authorize?response_type=code") +
        "&client_id=" + FEEDS_ZOOM_CLIENT_ID +
        "&redirect_uri=" + UrlEncode(kRedirectUri) +
        "&code_challenge=" + challenge +
        "&code_challenge_method=S256" +
        "&state=" + state +
        "&prompt=consent";

    LogInfo("OAuth: opening the browser for Zoom sign-in");
    if (!OpenInBrowser(authUrl)) {
        LogError("OAuth: could not open a browser");
        EngineSend("{\"type\":\"login_failed\",\"error\":\"login_timeout\"}");
        return;
    }

    bool cancelled = false;
    const std::string code = PollWorkerForAuthCode(state, cancelled);

    if (cancelled) {
        // The plugin already cleared its own in-progress state when the user
        // clicked Cancel; a login_failed here would put an error box on top of
        // a deliberate cancellation.
        LogInfo("OAuth: login cancelled by the user");
        return;
    }
    if (code.empty()) {
        LogWarn("OAuth: no auth code arrived (worker timeout or network error)");
        EngineSend("{\"type\":\"login_failed\",\"error\":\"login_timeout\"}");
        return;
    }

    const std::string body =
        std::string("grant_type=authorization_code&code=") + UrlEncode(code) +
        "&client_id=" + FEEDS_ZOOM_CLIENT_ID +
        "&redirect_uri=" + UrlEncode(kRedirectUri) +
        "&code_verifier=" + UrlEncode(verifier);

    const HttpResponse r =
        HttpPostForm("https://zoom.us/oauth/token", body, {}, 20);
    const std::string access  = feeds::ExtractJsonString(r.body, "access_token");
    const std::string refresh = feeds::ExtractJsonString(r.body, "refresh_token");
    if (access.empty()) {
        LogError("OAuth: token exchange failed");
        EngineSend("{\"type\":\"login_failed\",\"error\":\"token_exchange_failed\"}");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_accessToken  = access;
        g_refreshToken = refresh;
    }
    SaveTokens();

    LogInfo("OAuth: login complete");
    AnnounceLoginSucceeded();
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

void RestoreSessionFromStoredToken()
{
    std::thread([]() {
        // One Keychain read for both tokens: two reads here meant two identical
        // permission panels stacked on top of each other, and the one in front
        // looked like the one behind it had failed.
        std::string access, refresh;
        if (!LoadStoredTokens(access, refresh)) {
            LogDebug("Login: no stored token; waiting for the user to sign in");
            // Not a failure — the user simply has not logged in yet — but the
            // plugin needs the signal to stop waiting. It special-cases this
            // error and shows no dialog.
            EngineSend("{\"type\":\"login_failed\",\"error\":\"no_stored_token\"}");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_refreshToken = refresh;
            g_accessToken  = access;
        }
        LogInfo("Login: restoring the stored session");
        AnnounceLoginSucceeded();
    }).detach();
}

bool StartLoginFlow()
{
    {
        std::lock_guard<std::mutex> lock(g_loginMutex);
        if (g_loginInProgress) {
            // Reject the duplicate silently: the plugin is already showing
            // "Cancel login", which is the correct state.
            LogDebug("OAuth: a login is already in progress");
            return false;
        }
        g_loginInProgress = true;
        g_loginCancelled  = false;
    }
    std::thread(LoginThread).detach();
    return true;
}

void CancelLoginFlow()
{
    std::lock_guard<std::mutex> lock(g_loginMutex);
    if (!g_loginInProgress) return;   // defensive cancel with nothing running
    g_loginCancelled = true;
}

void ClearStoredCredentials()
{
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_accessToken.clear();
        g_refreshToken.clear();
        g_displayName.clear();
        g_pmi.clear();
        g_currentTier = 0;
    }
    KeychainDelete(kSessionItem);
    // Harmless no-ops once the migration in LoadStoredTokens has run, but a
    // logout must not be the one path that leaves an old token behind.
    KeychainDelete(kLegacyAccessItem);
    KeychainDelete(kLegacyRefreshItem);
    // The cached tier is per-account and must die with the tokens, or the next
    // user to sign in on this Mac inherits the previous user's entitlement
    // until their own tier query lands.
    KeychainDelete(kCachedTierItem);
}

std::string FetchZak()
{
    // Not cached: ZAKs are short-lived, so this runs at every join attempt.
    int status = 0;
    const std::string response = ZoomApiGet("/v2/users/me/zak", status);
    const std::string zak = feeds::ExtractJsonString(response, "token");
    if (zak.empty()) LogWarn("API: the ZAK request returned nothing");
    return zak;
}

std::string UserDisplayName()
{
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_displayName;
}

int GetCurrentTier()
{
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_currentTier;
}

} // namespace feeds_mac
