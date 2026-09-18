// feeds-mac-login.h — Zoom login and REST for the macOS engine.
//
// The same flow the Windows engine runs in engine-oauth.cpp + engine-api.cpp,
// rewritten against macOS APIs (feeds-mac-net.h). The messages sent to the
// plugin are identical, so the plugin's existing login UI needs no macOS
// special case:
//
//   login_succeeded {display_name, pmi, tier}   a usable session exists
//   sdk_authenticated                            "ready to connect"
//   login_failed    {error}                      no_stored_token /
//                                                session_restore_unreachable /
//                                                login_timeout /
//                                                token_exchange_failed
//   session_expired                              stored credentials are dead
//   tier_unreachable                             tier unknown AND never cached
//   token_refreshed                              a silent refresh succeeded
//
// The Zoom SDK is deliberately NOT involved here. As on Windows, a logged-in
// but not-connected Feeds holds no SDK and no Zoom session; the SDK comes up on
// the first connect.

#pragma once

#include <string>

namespace feeds_mac {

// Implemented in feeds-engine-mac.mm: the engine's IPC writer and log sink.
// Declared here so this translation unit can report without depending on the
// engine's internals.
void EngineSend(const std::string& json);
void EngineLog(const char* level, const std::string& message);

// Startup: if a refresh token is stored, restore the logged-in appearance over
// REST and announce it; otherwise tell the plugin there is nothing to restore
// (login_failed / no_stored_token, which the plugin treats as "logged out", not
// as an error). Returns immediately; the work runs on its own thread.
void RestoreSessionFromStoredToken();

// Begin the OAuth PKCE flow: open the browser, poll the Feeds worker for the
// auth code, exchange it for tokens, store them, announce the login. Returns
// false when a login is already in flight. Returns immediately; the flow runs
// on its own thread.
bool StartLoginFlow();

// Ask an in-flight login to stop. The poll loop checks this every iteration and
// exits without sending login_failed — the user already knows they cancelled.
void CancelLoginFlow();

// Forget everything: both tokens and the cached tier, in memory and in the
// Keychain. The caller owns logging out of the SDK and sending logout_complete.
void ClearStoredCredentials();

// A fresh ZAK for a join. BLOCKS on the network: background threads only, never
// the main queue. Empty means the join must not proceed.
std::string FetchZak();

// Provision a NEW meeting and return its number, password and join URL. This is
// what separates Instant Meeting from a PMI join: a fresh meeting number every
// time rather than the account's one permanent room. BLOCKS on the network, so
// background threads only. False means nothing was created and the caller must
// tell the user; the most common cause is the meeting:write:meeting OAuth scope
// not being granted.
bool CreateInstantMeeting(const std::string& topic,
                          unsigned long long& outId,
                          std::string& outPassword,
                          std::string& outJoinUrl);

// ---------------------------------------------------------------------------
// Zoom Events
//
// Same OAuth token as everything else, against /v2/zoom_events/ endpoints — not
// a separate login. All three BLOCK on the network, so background threads only.
// ---------------------------------------------------------------------------

// The user's upcoming events as a JSON array, across both roles they can hold.
// authFailed means 401/403: the token predates the Events scopes and the user
// has to log out and back in to re-consent. The returned array is empty then.
std::string FetchEventsArray(bool& authFailed);

// The sessions of one event, as a JSON array. "[]" on any failure — a session
// list that cannot be fetched is indistinguishable, to the user, from an event
// with no sessions, and neither is worth an error dialog.
std::string FetchEventSessionsArray(const std::string& eventId);

// The just-in-time token for joining one session. Zoom documents no TTL for it,
// so it must be fetched immediately before the join and never cached. True only
// when code == 0 and a token came back; otherwise code carries Zoom's reason
// (1130 = no valid ticket, 1150 = revoked) for the caller to translate.
bool FetchEventJoinToken(const std::string& eventId, const std::string& sessionId,
                         int& code, std::string& joinToken,
                         std::string& errorMessage);

// The entitlement tier this session resolved to: 0 Free, 1 Basic, 2 Streamer,
// 3 Broadcaster. The video path reads it to decide the resolution ceiling a
// feed may ask Zoom for, so a Free account cannot be served paid-tier quality.
// Counterpart of the Windows engine's GetCurrentTier (engine-api.cpp).
// 0 until a login resolves, which is the correct conservative default: an
// unresolved tier must cap DOWN, never up.
int GetCurrentTier();

// The account display name from the last successful user-info fetch, or "" —
// the join path needs it and refuses to join without one.
std::string UserDisplayName();

} // namespace feeds_mac
