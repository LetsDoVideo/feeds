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

}  // namespace feeds
