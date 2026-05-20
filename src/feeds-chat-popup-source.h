#pragma once

#include <string>

// Public interface for the feeds_chat_popup OBS source. Registration is
// called once from obs_module_load. ToggleChatPopup / ClearChatPopup are
// called from the dock click handler and meeting-left handler to drive
// popup visibility across all active popup instances.

namespace feeds {

void RegisterChatPopupSource();

// Show the given message in every active popup source instance. If the
// same message (matched by sender_id + content) is currently showing,
// hide instead — gives the dock a single click-to-toggle affordance.
void ToggleChatPopup(unsigned int senderId,
                     const std::string& senderName,
                     const std::string& content);

// Hide any active popup and clear the stored message. Called on
// meeting-left so stale state doesn't carry into the next meeting.
void ClearChatPopup();

// Re-evaluate every popup source instance's tier_disabled flag against
// the current g_currentTier (popup is a Streamer-tier feature, gated at
// >= 2). Called from plugin-main's ReconcileSourcesToTier on
// login_succeeded so a tier downgrade puts every popup into the
// "show nothing, properties show upgrade message" state, and a re-upgrade
// flips them back to normal rendering.
void ReconcileChatPopupSources();

}  // namespace feeds
