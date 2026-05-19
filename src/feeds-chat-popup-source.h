#pragma once

// Registration entry point for the feeds_chat_popup OBS source. Called
// once from obs_module_load. Source rendering, properties, and texture
// management all live in feeds-chat-popup-source.cpp.
//
// In commit 3b the source renders a single hardcoded test message. The
// dock-to-source signal that pushes real chat messages in arrives in
// commit 3c, and animation in 3d.

namespace feeds {
    void RegisterChatPopupSource();
}
