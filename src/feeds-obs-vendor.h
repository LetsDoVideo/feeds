#pragma once

#include <string>
#include <vector>

// The "feeds" obs-websocket vendor: how Streamer.bot and any other obs-websocket
// client hears about the meeting without Feeds knowing it exists.
//
// PERMANENT PUBLIC CONTRACT. Customer automation is keyed on these strings, so
// they are never renamed once shipped: the vendor name "feeds", the events
// speaker_changed, meeting_joined, meeting_left, participant_1_speaking through
// participant_8_speaking, chat_message, share_started, share_stopped,
// participant_joined, participant_left, participant_muted and participant_unmuted,
// the request get_state, and the field names name, participant_id, slots, message,
// is_self, in_meeting, speaker, participants, share_active and locked. The slot
// numbers themselves (feeds_slot, plugin-main.cpp) are part of the same contract.
// New fields may be added. See feeds-obs-vendor.cpp for the wire format.
//
// Optional by design: with obs-websocket missing or disabled, every function
// here is a no-op and Feeds behaves exactly as it does without this file.
//
// This file owns the obs-websocket side only. What the events say, and whether
// the account's tier allows them, is decided in plugin-main.cpp, which owns the
// roster and the sources.
namespace feeds {

// One meeting participant, as a client sees them. Also the payload of
// speaker_changed and participant_N_speaking, so a client handles all of them
// with the same code.
struct VendorPerson {
    std::string      name;                // roster display name; "" if unknown
    unsigned int     participant_id = 0;  // runtime user id; 0 = nobody
    // Slots pinned to them, ascending. Sent as the field "slots", but not named
    // that here: Qt defines slots as a macro, and plugin-main.cpp includes this
    // header before its #undef.
    std::vector<int> slot_list;
};

struct VendorState {
    bool                      locked = false;  // Free or logged out
    bool                      in_meeting = false;
    VendorPerson              speaker;
    std::vector<VendorPerson> participants;
    bool                      share_active = false;
};

// Register the vendor and its request. From obs_module_post_load only: that is
// the first point obs-websocket is guaranteed to have loaded.
void VendorRegister();

// Stop emitting and forget the vendor. obs-websocket frees it during its own
// module unload, so this must run first: at the frontend exit event.
void VendorShutdown();

// True between a successful VendorRegister and VendorShutdown.
bool VendorAvailable();

// UI thread only. Build the payload first, and hold no Feeds lock across these.
// VendorEmitSpeakerChanged sends speaker_changed, then one participant_N_speaking
// for each of the speaker's slots.
void VendorEmitSpeakerChanged(const VendorPerson& speaker);
void VendorEmitMeetingJoined();
void VendorEmitMeetingLeft();
void VendorEmitChatMessage(const VendorPerson& sender, const std::string& message,
                           bool is_self);
void VendorEmitShareStarted(const VendorPerson& sharer);
void VendorEmitShareStopped(const VendorPerson& sharer);
void VendorEmitParticipantJoined(const VendorPerson& person);
void VendorEmitParticipantLeft(const VendorPerson& person);
// Sends participant_muted, or participant_unmuted when muted is false.
void VendorEmitParticipantMuted(const VendorPerson& person, bool muted);

// The get_state snapshot. Defined in plugin-main.cpp. Called on an obs-websocket
// thread pool thread, so it must be safe from any thread and must never wait on
// the UI thread.
VendorState VendorGetState();

}  // namespace feeds
