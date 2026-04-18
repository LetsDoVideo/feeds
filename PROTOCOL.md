# Feeds IPC Protocol

This document defines the inter-process communication protocol between the Feeds OBS plugin (`feeds.dll`) and the Feeds engine subprocess (`FeedsEngine.exe`).

## Architecture Overview

The Feeds plugin is split into two processes:

- **`feeds.dll`** — Loaded in-process by OBS. Thin wrapper that registers source types, shows menu items, and relays user actions to the engine. Does NOT load the Zoom SDK.
- **`FeedsEngine.exe`** — Separate process launched by the plugin. Hosts the Zoom Meeting SDK, performs OAuth, joins meetings, captures video frames.

This separation exists because the Zoom SDK's `libcurl.dll` conflicts with OBS's `libcurl.dll` when loaded in-process. Running the SDK in a subprocess gives it its own DLL search path and eliminates the conflict.

## Transport

Two separate transport mechanisms are used:

- **Two unidirectional named pipes** — Control messages. `\\.\pipe\FeedsEngine_P2E` (plugin writes, engine reads) and `\\.\pipe\FeedsEngine_E2P` (engine writes, plugin reads). JSON messages, one per pipe write.
- **D3D11 shared NT handles** — Video frame data (Phase 6, not yet implemented). The engine will create shared textures and transmit the handles via the named pipes. Pixel data will never flow through the pipes; it will stay in GPU memory, accessible by both processes.

## Message Format

Each message is a single JSON object per pipe write:

```json
{"type": "message_name", "field1": "value1", "field2": 123}
```

All messages have a `type` field. Additional fields are message-specific. The engine's string parsing is minimal — it extracts specific fields by key lookup rather than full JSON parsing.

## Process Lifecycle

1. Plugin (in `obs_module_load`) creates a Windows Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`.
2. Plugin creates both named pipe servers.
3. Plugin launches `FeedsEngine.exe` and assigns it to the Job Object.
4. Engine connects to both pipes, creates a message-only window (required for Zoom SDK async callbacks), starts a background pipe reader thread, and enters a Windows message pump on the main thread.
5. Engine sends `engine_ready`.
6. Engine calls `InitSDK`. If tokens are in Windows Credential Manager, engine automatically calls `SDKAuth` to restore the previous session.
7. On OBS shutdown, plugin sends `shutdown`, waits briefly, then closes the Job Object handle (which guarantees engine termination even if graceful shutdown fails).

## Messages

Direction notation: **P→E** = plugin to engine, **E→P** = engine to plugin.

### Startup

#### `engine_ready` (E→P)
Sent once, when the engine has started and is ready to receive commands.

```json
{"type": "engine_ready", "version": "1.0.0"}
```

### Authentication

#### `login_start` (P→E)
User clicked "Login to Zoom". Engine opens browser to Zoom OAuth authorize endpoint, listens on the `FeedsAuth` pipe for the auth code delivered by `FeedsLogin.exe`, exchanges the code for tokens, saves them to Windows Credential Manager, triggers `SDKAuth`, then fetches user info and entitlement.

```json
{"type": "login_start"}
```

#### `login_succeeded` (E→P)
OAuth flow completed and user info has been fetched. Plugin should cache these fields — they are used for the PMI-or-link dialog, the "Logged in as X" about box, and tier enforcement.

```json
{"type": "login_succeeded",
 "display_name": "David Maldow",
 "pmi": "1234567890",
 "tier": 0}
```

#### `sdk_authenticated` (E→P)
SDK authentication succeeded. Plugin flips UI state to logged-in. Always arrives **after** `login_succeeded` so the cache is populated before the Connect menu becomes enabled.

```json
{"type": "sdk_authenticated"}
```

#### `sdk_auth_failed` (E→P)
SDK authentication failed. Plugin shows error dialog and re-enables the Login menu item.

```json
{"type": "sdk_auth_failed", "code": 15}
```

#### `login_failed` (E→P)
OAuth flow failed or was cancelled before SDK auth was attempted.

```json
{"type": "login_failed", "error": "user_cancelled"}
```

#### `logout` (P→E)
User clicked "Logout". Engine leaves any active meeting, calls `auth_service->LogOut()`, clears tokens from Credential Manager, clears cached user info.

```json
{"type": "logout"}
```

#### `logout_complete` (E→P)
Logout finished. Plugin resets UI to logged-out state and clears its own cache.

```json
{"type": "logout_complete"}
```

#### `session_expired` (E→P)
Engine attempted to use the access token, got a 401, and the refresh token was either missing, expired, or rejected. Plugin should behave as if the user logged out.

```json
{"type": "session_expired"}
```

#### `token_refreshed` (E→P)
Engine silently refreshed the OAuth token in the background. Informational only; engine has already persisted the new token to Credential Manager.

```json
{"type": "token_refreshed"}
```

### Meeting

#### `join_meeting` (P→E)
User completed the Connect dialog. Plugin sends the raw text from the input box (meeting number, PMI number, or URL — engine parses it), the password (may be empty), and whether the PMI option was chosen.

```json
{"type": "join_meeting",
 "input": "1234567890",
 "password": "",
 "is_pmi": true}
```

Engine fetches a fresh ZAK token, constructs `JoinParam4WithoutLogin` with the cached display name, and calls `meetingService->Join()`.

#### `meeting_joined` (E→P)
Meeting reached `MEETING_STATUS_INMEETING`. Plugin enables the Disconnect menu item. Note: participant list is not yet available at this point — it arrives via `participant_list_changed` after `raw_livestream_granted`.

```json
{"type": "meeting_joined", "meeting_number": "1234567890"}
```

#### `meeting_failed` (E→P)
Meeting join failed. Engine maps the SDK error code to a user-readable message and sends both. Plugin displays `message` directly; `code` is for logging only.

```json
{"type": "meeting_failed",
 "code": 4,
 "message": "Incorrect meeting password. Please try again."}
```

#### `leave_meeting` (P→E)
User clicked Disconnect. Engine calls `meetingService->Leave()`.

```json
{"type": "leave_meeting"}
```

#### `meeting_left` (E→P)
Engine has observed the SDK reporting `MEETING_STATUS_ENDED` or `MEETING_STATUS_DISCONNECTING`. Plugin resets meeting-related state.

```json
{"type": "meeting_left"}
```

#### `raw_livestream_granted` (E→P)
`IMeetingLiveStreamCtrlEvent::onRawLiveStreamPrivilegeChanged(true)` fired. This is the critical moment where the plugin can actually render participant video. If the user is the host (e.g. joining their own PMI), this fires almost immediately after `meeting_joined`. Otherwise it fires only after the host clicks "Allow" on the "Request to livestream" popup in the Zoom client.

```json
{"type": "raw_livestream_granted"}
```

#### `raw_livestream_timeout` (E→P)
The SDK timed out waiting for the host to approve the raw-livestream request. Plugin logs but does not currently show a user-visible notification (future enhancement).

```json
{"type": "raw_livestream_timeout"}
```

### Participants

#### `get_participants` (P→E)
Plugin requests the current participant list. Sent from `zp_properties` on every open (so the list stays fresh without live SDK subscriptions), and from the "Refresh Participant List" button.

```json
{"type": "get_participants"}
```

#### `participant_list_changed` (E→P)
Sent as a response to `get_participants`, and also unsolicited after `raw_livestream_granted` so the plugin gets an initial list. Includes the full current list, not a diff. `my_user_id` is the plugin's own user in the meeting, which the plugin filters out of the dropdown.

```json
{"type": "participant_list_changed",
 "my_user_id": 12345678,
 "participants": [
    {"id": 12345679, "name": "Alice"},
    {"id": 12345680, "name": "Bob"}
 ]}
```

#### `active_speaker_changed` (E→P)
Active speaker changed in the meeting. Plugin caches this for use with the [Active Speaker] dropdown option.

```json
{"type": "active_speaker_changed", "participant_id": 12345679}
```

### Screenshare

#### `share_status_changed` (E→P)
Screenshare started or ended. Plugin uses this to update the Zoom Screenshare source's properties text ("Receiving screenshare" vs "Waiting for screenshare"). `sharer_user_id` is 0 when no share is active.

```json
{"type": "share_status_changed", "sharer_user_id": 12345679}
```

### Video Frames (Phase 6 — not yet implemented)

#### `participant_source_subscribe` (P→E)
Plugin requests video from a specific participant for a specific source.

```json
{"type": "participant_source_subscribe",
 "source_id": "obs_source_uuid",
 "participant_id": 12345679}
```

#### `source_texture_ready` (E→P)
Engine has created a shared texture for this source.

```json
{"type": "source_texture_ready",
 "source_id": "obs_source_uuid",
 "handle": 12345678,
 "width": 1920,
 "height": 1080,
 "format": "NV12"}
```

#### `participant_source_unsubscribe` (P→E)
Plugin no longer needs video for this source.

```json
{"type": "participant_source_unsubscribe", "source_id": "obs_source_uuid"}
```

### Shutdown

#### `shutdown` (P→E)
OBS is closing. Engine should leave any active meeting, tear down the Zoom SDK cleanly, and exit.

```json
{"type": "shutdown"}
```

#### `shutdown_complete` (E→P)
Engine has finished cleanup and is about to exit.

```json
{"type": "shutdown_complete"}
```

### Diagnostics

#### `engine_log` (E→P)
Routine log message from the engine. Plugin forwards to the OBS log so everything is visible in one place.

```json
{"type": "engine_log", "level": "info", "message": "..."}
```

#### `engine_error` (E→P)
Something unrecoverable happened.

```json
{"type": "engine_error", "code": "sdk_init_failed", "message": "..."}
```

## Design Principles

**Authoritative state lives in the engine.** The plugin is a UI and (eventually) frame-rendering layer. The engine is the source of truth for authentication, meeting state, and participants. The plugin maintains a cache that's populated by engine messages, and reads from the cache during OBS's synchronous property-function calls.

**Error messages are engine-generated.** The engine has the Zoom SDK headers and knows what each error code means. Rather than the plugin maintaining a duplicate table of codes and strings that can drift out of sync with the SDK, the engine sends both a numeric `code` (for logs) and a human-readable `message` (for display). Plugin just shows whatever the engine says.

**Small, explicit messages.** Rather than generic RPC with method names and argument arrays, each message type has a clear purpose and explicit fields.

**Video data will not flow through the pipe.** Frame pixels will live in GPU memory via shared textures. The pipe carries handles and notifications only.

**Request/response is loose.** Most P→E messages trigger an eventual E→P response, but the plugin does not block waiting. All pipe I/O happens on a dedicated pipe-reader thread in the plugin; responses are dispatched to handlers asynchronously. Handlers marshal UI work to the Qt main thread via `QTimer::singleShot`.

**Extensibility.** Both sides ignore message types they don't recognize. Adding fields to existing messages is safe, as long as old required fields are preserved.
