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

#define WM_FEEDS_SEND_CHAT (WM_APP + 1)

// Defined in engine-main.cpp. Set right after the anchor window is
// created in WinMain. NULL before that (which the pipe handler
// detects and reports as an internal error rather than crashing).
extern HWND g_anchorWnd;

namespace feeds_engine {

// Defined in engine-meeting.cpp. Invoked by EngineWndProc on the main
// thread when WM_FEEDS_SEND_CHAT arrives. Performs the actual SDK
// chat-send work and dispatches the chat_send_result IPC reply.
void SendChatMessageOnMainThread(const std::string& content);

} // namespace feeds_engine
