#pragma once

#include <string>

#include <QtGlobal>  // qint64

// Public interface for the feeds_chat_overlay OBS source. Registration is
// called once from obs_module_load. The overlay is a persistent Twitch-style
// chat feed showing recent messages — pairs with the popup (which highlights
// one message at a time) to give streamers both an always-visible scroll of
// recent chat and a "now showing this" callout.

namespace feeds {

// Platform a chat message came from — tags each overlay-history entry so a
// per-instance filter (Zoom / YouTube / Twitch / Both) can show or drop it.
// Values are deliberately 1/2/3 so they equal the non-"Both" dropdown filter
// values (PLAT_*).
enum class ChatMsgOrigin { Zoom = 1, YouTube = 2, Twitch = 3 };

void RegisterChatOverlaySource();

// Push a new message into the centralised overlay history. Called from the chat
// IPC handler (Zoom) and the YouTube poll loop, off the graphics thread.
// Thread-safe — the underlying history is guarded by a mutex, and all overlay
// source instances are marked texture_dirty after the append so they re-render
// on next frame. Each instance's platform filter decides at render time whether
// to show the message. The avatar is resolved at render time from the origin-
// appropriate cache (Zoom: senderId; YouTube: channelId), so senderId is 0 /
// channelId "" for the platform that doesn't apply.
//
// The overlay keeps the most recent ~50 messages; only the last few are
// typically visible. Cap is internal — caller doesn't need to trim.
void AppendChatMessageToOverlay(ChatMsgOrigin      origin,
                                unsigned int       senderId,
                                const std::string& channelId,
                                const std::string& senderName,
                                const std::string& content,
                                qint64             timestamp);

// Drop all messages from the overlay history. Called on meeting-left
// so stale messages don't carry into the next meeting.
void ClearChatOverlay();

// Mark every overlay instance dirty so it re-renders on the next frame.
// Called from the avatar download workers when a late-arriving avatar lands
// in its cache after the row already rendered with the neutral circle —
// without this the row keeps its stale texture until incidental chat
// activity forces a repaint. Thread-safe (guards only the instances mutex +
// bool writes, touches no Qt/graphics objects), so the off-thread workers
// call it directly, the same way the IPC thread does on a new message.
void MarkChatOverlayDirty();

// Re-evaluate every overlay source instance's tier_disabled flag against
// the current g_currentTier (overlay is a Streamer-tier feature, gated
// at >= 2). Called from plugin-main's ReconcileSourcesToTier on
// login_succeeded. Mirrors ReconcileChatPopupSources.
void ReconcileChatOverlaySources();

}  // namespace feeds
