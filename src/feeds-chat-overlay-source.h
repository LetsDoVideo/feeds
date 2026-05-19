#pragma once

#include <string>

#include <QtGlobal>  // qint64

// Public interface for the feeds_chat_overlay OBS source. Registration is
// called once from obs_module_load. The overlay is a persistent Twitch-style
// chat feed showing recent messages — pairs with the popup (which highlights
// one message at a time) to give streamers both an always-visible scroll of
// recent chat and a "now showing this" callout.

namespace feeds {

void RegisterChatOverlaySource();

// Push a new message into the centralised overlay history. Called from
// the chat IPC handler in plugin-main alongside the existing dock append
// and avatar-cache warming. Thread-safe — the underlying history is
// guarded by a mutex, and all overlay source instances are marked
// texture_dirty after the append so they re-render on next frame.
//
// The overlay keeps the most recent ~50 messages; only the last few are
// typically visible. Cap is internal — caller doesn't need to trim.
void AppendChatMessageToOverlay(unsigned int       senderId,
                                const std::string& senderName,
                                const std::string& content,
                                qint64             timestamp);

// Drop all messages from the overlay history. Called on meeting-left
// so stale messages don't carry into the next meeting.
void ClearChatOverlay();

}  // namespace feeds
