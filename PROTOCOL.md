# Feeds IPC Protocol

This document defines the inter-process communication protocol between the Feeds OBS plugin (`feeds.dll`) and the Feeds engine subprocess (`FeedsEngine.exe`).

## Architecture Overview

The Feeds plugin is split into two processes:

- **`feeds.dll`** — Loaded in-process by OBS. Thin wrapper that registers source types, shows menu items, and relays user actions to the engine. Does NOT load the Zoom SDK.
- **`FeedsEngine.exe`** — Separate process launched lazily by the plugin when Feeds functionality is first used. Hosts the Zoom Meeting SDK, performs OAuth, joins meetings, captures video frames.

This separation exists because the Zoom SDK's `libcurl.dll` conflicts with OBS's `libcurl.dll` when loaded in-process. Running the SDK in a subprocess gives it its own DLL search path and eliminates the conflict.

## Transport

Two separate transport mechanisms are used:

- **Named pipe (`\\.\pipe\FeedsEngine`)** — Control messages. Bidirectional. JSON messages, newline-terminated.
- **D3D11 shared NT handles** — Video frame data. The engine creates shared textures and transmits the handles via the named pipe. Pixel data never flows through the pipe; it stays in GPU memory, accessible by both processes.

## Message Format

Each message is a single JSON object on one line, terminated by `\n`:

```json
{"type": "message_name", "field1": "value1", "field2": 123}
```

All messages have a `type` field. Additional fields are message-specific.

## Process Lifecycle

1. User takes a Feeds action (Login, join meeting, add source) for the first time in the OBS session.
2. Plugin creates a Windows Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` flag.
3. Plugin launches `FeedsEngine.exe` (located next to `feeds.dll`) and assigns it to the Job Object.
4. Engine starts, opens named pipe to plugin, sends `engine_ready`.
5. Plugin sends `initialize` with any stored auth token.
6. Engine sends `initialized`, and normal operation begins.
7. On OBS shutdown, plugin sends `shutdown`, waits briefly, then closes the Job Object handle (which guarantees engine termination even if graceful shutdown fails).

If the engine dies unexpectedly, a background thread in the plugin (waiting on the engine's process handle) detects it and triggers recovery: relaunch engine, re-initialize, restore meeting state if applicable.

## Messages

Direction notation: **P→E** = plugin to engine, **E→P** = engine to plugin.

### Startup

#### `engine_ready` (E→P)
Sent once, when the engine has started and is ready to receive commands. Plugin should not send any messages before receiving this.

```json
{"type": "engine_ready", "version": "1.0.0"}
```

#### `initialize` (P→E)
Plugin's first real message. Includes stored refresh token if one exists, so the engine can restore the previous session without requiring re-authentication.

```json
{"type": "initialize", "refresh_token": "..." | null}
```

#### `initialized` (E→P)
Engine confirms it has received initialization data. If a refresh token was provided and is still valid, `logged_in` will be true.

```json
{"type": "initialized", "logged_in": true, "tier": 0 | 1 | 2 | 3 | null}
```

### Authentication

#### `login_start` (P→E)
User clicked "Login to Zoom". Engine launches `FeedsLogin.exe`, opens browser, handles OAuth callback.

```json
{"type": "login_start"}
```

#### `login_succeeded` (E→P)
OAuth flow completed successfully.

```json
{"type": "login_succeeded", "user_name": "...", "user_email": "...", "tier": 0 | 1 | 2 | 3}
```

#### `login_failed` (E→P)
OAuth flow failed or was cancelled.

```json
{"type": "login_failed", "error": "user_cancelled" | "network_error" | "..."}
```

#### `logout` (P→E)
User clicked "Logout". Engine clears tokens and any cached state.

```json
{"type": "logout"}
```

#### `logout_complete` (E→P)
Logout finished.

```json
{"type": "logout_complete"}
```

#### `token_refreshed` (E→P)
Engine silently refreshed the OAuth token in the background. Plugin stores the new refresh token for use on next startup.

```json
{"type": "token_refreshed", "refresh_token": "..."}
```

### Meeting

#### `join_meeting` (P→E)
User requested meeting join. Engine uses Zoom SDK to join as guest.

```json
{"type": "join_meeting", "meeting_number": "1234567890", "password": "..." | null, "display_name": "..."}
```

#### `meeting_joined` (E→P)
Meeting join succeeded. Engine is now in the meeting.

```json
{"type": "meeting_joined", "meeting_number": "1234567890"}
```

#### `meeting_failed` (E→P)
Meeting join failed.

```json
{"type": "meeting_failed", "error": "invalid_password" | "meeting_not_found" | "..."}
```

#### `leave_meeting` (P→E)
User disconnected from meeting.

```json
{"type": "leave_meeting"}
```

#### `meeting_left` (E→P)
Engine has left the meeting.

```json
{"type": "meeting_left"}
```

#### `meeting_ended` (E→P)
Host ended the meeting, or the engine was kicked/disconnected. Plugin should update UI and clear active source subscriptions.

```json
{"type": "meeting_ended", "reason": "host_ended" | "disconnected" | "..."}
```

### Participants

#### `participant_list_changed` (E→P)
Sent whenever the participant list changes (join, leave, rename). Includes the full current list, not a diff.

```json
{"type": "participant_list_changed", "participants": [
    {"id": "user_id_1", "name": "Alice"},
    {"id": "user_id_2", "name": "Bob"}
]}
```

#### `active_speaker_changed` (E→P)
Optional. Sent when Zoom reports the active speaker has changed. Plugin may use this for future auto-switching features.

```json
{"type": "active_speaker_changed", "participant_id": "user_id_1"}
```

### Video Frames

#### `participant_source_subscribe` (P→E)
Plugin requests video from a specific participant for a specific source.

```json
{"type": "participant_source_subscribe", "source_id": "obs_source_uuid", "participant_id": "user_id_1"}
```

#### `source_texture_ready` (E→P)
Engine has created a shared texture for this source. Plugin calls `gs_texture_open_shared_nt()` with the provided handle to get an OBS texture.

```json
{"type": "source_texture_ready", "source_id": "obs_source_uuid", "handle": 12345678, "width": 1920, "height": 1080, "format": "NV12"}
```

#### `participant_source_unsubscribe` (P→E)
Plugin no longer needs video for this source (source destroyed, participant changed, etc.).

```json
{"type": "participant_source_unsubscribe", "source_id": "obs_source_uuid"}
```

#### `source_texture_released` (E→P)
Engine has released the shared texture. Plugin should release its OBS texture reference.

```json
{"type": "source_texture_released", "source_id": "obs_source_uuid"}
```

#### `texture_handle_changed` (E→P)
The shared texture for a source had to be recreated (typically due to resolution change). Plugin must re-import the handle.

```json
{"type": "texture_handle_changed", "source_id": "obs_source_uuid", "handle": 87654321, "width": 1280, "height": 720, "format": "NV12"}
```

### Screenshare

#### `screenshare_subscribe` (P→E)
Plugin requests the meeting's active screenshare feed for a source.

```json
{"type": "screenshare_subscribe", "source_id": "obs_source_uuid"}
```

#### `screenshare_texture_ready` (E→P)
Engine created a shared texture for the screenshare. Same semantics as `source_texture_ready`.

```json
{"type": "screenshare_texture_ready", "source_id": "obs_source_uuid", "handle": 11223344, "width": 1920, "height": 1080, "format": "NV12"}
```

#### `screenshare_unsubscribe` (P→E)
Plugin no longer needs screenshare.

```json
{"type": "screenshare_unsubscribe", "source_id": "obs_source_uuid"}
```

### Tier

#### `tier_info` (E→P)
Sent after login with the user's current tier. Plugin uses this to enforce source count and resolution limits.

Tier values:
- 0 = Free (1 feed, 720p)
- 1 = Basic (3 feeds, 1080p)
- 2 = Streamer (5 feeds, 1080p)
- 3 = Broadcaster (8 feeds, 1080p)

```json
{"type": "tier_info", "tier": 0, "max_feeds": 1, "max_resolution": 720}
```

### Shutdown

#### `shutdown` (P→E)
OBS is closing. Engine should leave any active meeting, tear down the Zoom SDK cleanly, and exit.

```json
{"type": "shutdown"}
```

#### `shutdown_complete` (E→P)
Engine has finished cleanup and is about to exit. Plugin may close the pipe.

```json
{"type": "shutdown_complete"}
```

### Diagnostics

#### `engine_error` (E→P)
Something unrecoverable happened. Plugin logs and optionally shows the user a notification.

```json
{"type": "engine_error", "code": "sdk_init_failed" | "...", "message": "..."}
```

#### `engine_log` (E→P)
Routine log message from the engine. Plugin forwards to the OBS log so everything is visible in one place.

```json
{"type": "engine_log", "level": "info" | "warning" | "error", "message": "..."}
```

## Design Principles

**Authoritative state lives in the engine.** The plugin is mostly a UI and frame-rendering layer. The engine is the source of truth for authentication, meeting state, and participants. If they disagree, the engine wins.

**Small, explicit messages.** Rather than generic RPC with method names and argument arrays, each message type has a clear purpose and explicit fields. This makes the protocol easy to debug, easy to extend, and resistant to accidental misuse.

**Video data does not flow through the pipe.** Frame pixels live in GPU memory via shared textures. The pipe only carries handles and notifications. This keeps pipe traffic tiny regardless of frame rate or resolution.

**Request/response is loose.** Most P→E messages trigger an eventual E→P response, but the plugin does not block waiting. All pipe I/O happens on a dedicated pipe-reader thread in the plugin; responses are dispatched to handlers asynchronously.

**Extensibility.** Adding a new message type does not require protocol versioning — both sides ignore message types they don't recognize. Adding fields to existing messages is also safe, as long as old required fields are preserved.
