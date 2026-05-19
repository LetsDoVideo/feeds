#pragma once

// Public interface for the feeds_chat_overlay OBS source. Registration is
// called once from obs_module_load. The overlay is a persistent Twitch-style
// chat feed showing recent messages — pairs with the popup (which highlights
// one message at a time) to give streamers both an always-visible scroll of
// recent chat and a "now showing this" callout.
//
// O1 establishes the source type, default position (top-right, 25% of canvas
// width), and renders a hardcoded list of test messages so layout/scaling
// can be validated. O2 replaces the test list with real chat history fed by
// the chat IPC handler.

namespace feeds {

void RegisterChatOverlaySource();

}  // namespace feeds
