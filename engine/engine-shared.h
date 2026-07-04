// engine-shared.h — declarations shared between engine translation units
// that don't fit in a per-feature header. Single source of truth so we
// don't duplicate WM constants or function forward-declarations across
// .cpp files.

#pragma once

#include <windows.h>
#include <string>

// ---------------------------------------------------------------------------
// Main-thread marshalling for Zoom SDK calls.
// ---------------------------------------------------------------------------
//
// The Zoom Meeting SDK requires certain calls (chat send, possibly
// others) to run on the thread that owns the SDK window and pumps
// Windows messages. Our IPC handlers run on the pipe-reader background
// thread; for SDK calls that must be main-thread, the pipe handler
// posts a custom WM_ message to the engine's anchor window, and the
// window proc (running on the main thread) performs the actual call.
//
// WM_FEEDS_SEND_CHAT — pipe thread posts this with LPARAM = pointer to
// a heap-allocated std::string holding the message content. The window
// proc takes ownership of the string, calls
// feeds_engine::SendChatMessageOnMainThread, and frees it.
//
// WM_FEEDS_INIT_SDK — pipe thread posts this (no params) to defer Zoom SDK
// init+auth onto the main thread for the lazy bring-up on first connect. The
// SDK's InitSDK must run on the thread that created the anchor window and
// pumps messages; EnsureSdkUpThen (engine-sdk.cpp) posts this and the window
// proc calls feeds_engine::BringUpSdkOnMainThread. See engine-sdk.cpp.

// WM_FEEDS_PROCESS_RENDERERS — posted (no params) to drive the readiness-gated
// renderer-creation queue on the main thread. Posted by the pipe thread when a
// participant_source_subscribe is queued, and by the raw-render readiness
// notification. The window proc calls feeds_engine::ProcessPendingRenderers.
// A WM_TIMER on the anchor window drives the retry backstop into the same
// function. See engine-video.cpp.

#define WM_FEEDS_SEND_CHAT         (WM_APP + 1)
#define WM_FEEDS_INIT_SDK          (WM_APP + 2)
#define WM_FEEDS_PROCESS_RENDERERS (WM_APP + 3)
// WM_FEEDS_CAMERA_ON — the video listener (engine-meeting.cpp) posts this on an
// SDK thread when a participant's camera turns on; WPARAM carries the userId.
// EngineWndProc hands it to QueueCameraOnReestablish on the main thread.
#define WM_FEEDS_CAMERA_ON         (WM_APP + 4)

// Defined in engine-main.cpp. Set right after the anchor window is
// created in WinMain. NULL before that (which the pipe handler
// detects and reports as an internal error rather than crashing).
extern HWND g_anchorWnd;

namespace feeds_engine {

// Defined in engine-meeting.cpp. Invoked by EngineWndProc on the main
// thread when WM_FEEDS_SEND_CHAT arrives. Performs the actual SDK
// chat-send work and dispatches the chat_send_result IPC reply.
void SendChatMessageOnMainThread(const std::string& content);

// Defined in engine-sdk.cpp. Invoked by EngineWndProc on the main thread
// when WM_FEEDS_INIT_SDK arrives. Runs InitSDK + SDKAuth (the lazy bring-up
// triggered by the first connect); the queued join(s) drain once the auth
// callback returns.
void BringUpSdkOnMainThread();

// Defined in engine-video.cpp. Runs the readiness-gated renderer-creation
// queue on the main thread. Invoked by EngineWndProc on WM_FEEDS_PROCESS_RENDERERS
// and on the retry WM_TIMER.
void ProcessPendingRenderers();

// Defined in engine-video.cpp. Called from the raw-live-stream status callback
// (engine-meeting.cpp) when our own user appears in the raw-live-streaming list
// — the signal that the raw-data renderer subsystem is ready. Flips the gate
// open and drains any queued subscribes.
void NotifyRawRenderReady();

// Defined in engine-video.cpp. Main-thread WM_TIMER dispatcher — the engine now
// runs two timers (the renderer readiness/retry/gate poll and the camera-on
// debounce); this routes by timer id. Invoked by EngineWndProc on WM_TIMER.
void OnEngineTimer(UINT_PTR timerId);

// Defined in engine-video.cpp. Main-thread entry for a participant's camera
// turning on: debounces, then re-establishes that userId's sources through the
// delivery-gated recreate path. Invoked by EngineWndProc on WM_FEEDS_CAMERA_ON.
void QueueCameraOnReestablish(unsigned int userId);

} // namespace feeds_engine
