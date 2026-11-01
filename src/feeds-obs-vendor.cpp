#include "feeds-obs-vendor.h"

#include <assert.h>
#include <string.h>

#include <exception>
#include <string>

#include <obs-module.h>

// Include in this file ONLY: the header defines a file-static proc handler
// pointer, and it finds obs-websocket at runtime through libobs, so nothing links
// against it and a missing obs-websocket is an ordinary NULL here.
#include <obs-websocket-api.h>

// Wire format (see the contract note in feeds-obs-vendor.h):
//
//   speaker_changed           name, participant_id, slots
//   participant_N_speaking    same data, N = 1 to 8, one per slot in slots
//   meeting_joined            {}
//   meeting_left              {}
//   chat_message              name, participant_id, slots, message, is_self
//   share_started             name, participant_id, slots
//   share_stopped             name, participant_id, slots
//   participant_joined        name, participant_id, slots
//   participant_left          name, participant_id, slots
//   participant_muted         name, participant_id, slots
//   participant_unmuted       name, participant_id, slots
//
//   get_state                 locked: {"locked":true} and nothing else
//                             otherwise: in_meeting, speaker (as speaker_changed),
//                             participants (array, same fields), share_active
//
//   slots is a string: ascending, comma-separated, no spaces, "3,7". "" if none.
//   A string because Streamer.bot arguments are flat, and obs_data arrays can only
//   hold objects.

namespace feeds {
namespace {

constexpr const char* kVendorName   = "feeds";
constexpr const char* kGetStateName = "get_state";

// Null when obs-websocket is absent. Written at post_load and at frontend exit
// and read by the emits: all on the UI thread, so no lock. The request callback
// never touches it.
obs_websocket_vendor g_vendor = nullptr;

void WritePerson(obs_data_t* data, const VendorPerson& person) {
    std::string joined;
    for (int slot : person.slot_list) {
        if (!joined.empty()) joined += ',';
        joined += std::to_string(slot);
    }
    obs_data_set_string(data, "name", person.name.c_str());
    obs_data_set_int(data, "participant_id", person.participant_id);
    obs_data_set_string(data, "slots", joined.c_str());
}

void EmitEmpty(const char* type) {
    if (!g_vendor) return;
    obs_data_t* data = obs_data_create();
    obs_websocket_vendor_emit_event(g_vendor, type, data);
    obs_data_release(data);
}

void EmitPerson(const char* type, const VendorPerson& person) {
    if (!g_vendor) return;
    obs_data_t* data = obs_data_create();
    WritePerson(data, person);
    obs_websocket_vendor_emit_event(g_vendor, type, data);
    obs_data_release(data);
}

void WriteState(obs_data_t* response, const VendorState& state) {
    if (state.locked) {
        obs_data_set_bool(response, "locked", true);
        return;
    }

    obs_data_set_bool(response, "in_meeting", state.in_meeting);
    obs_data_set_bool(response, "share_active", state.share_active);

    obs_data_t* speaker = obs_data_create();
    WritePerson(speaker, state.speaker);
    obs_data_set_obj(response, "speaker", speaker);
    obs_data_release(speaker);

    obs_data_array_t* participants = obs_data_array_create();
    for (const VendorPerson& person : state.participants) {
        obs_data_t* item = obs_data_create();
        WritePerson(item, person);
        obs_data_array_push_back(participants, item);
        obs_data_release(item);
    }
    obs_data_set_array(response, "participants", participants);
    obs_data_array_release(participants);
}

// Runs on an obs-websocket thread pool thread with no obs-websocket lock held.
// VendorGetState has released every Feeds lock by the time it returns, so
// response_data is filled, and the reply sent, with nothing held. Reached through
// a C callback, so nothing may escape it.
void OnGetState(obs_data_t* /*request_data*/, obs_data_t* response_data,
                void* /*priv*/) {
    try {
        WriteState(response_data, VendorGetState());
    } catch (const std::exception& e) {
        blog(LOG_ERROR, "[feeds] get_state exception: %s", e.what());
    } catch (...) {
        blog(LOG_ERROR, "[feeds] get_state unknown exception");
    }
}

}  // namespace

void VendorRegister() {
    obs_websocket_vendor vendor = obs_websocket_register_vendor(kVendorName);
    if (!vendor) {
        blog(LOG_INFO, "[feeds] obs-websocket not available, vendor events are off");
        return;
    }
    if (!obs_websocket_vendor_register_request(vendor, kGetStateName, OnGetState,
                                               nullptr)) {
        blog(LOG_WARNING, "[feeds] obs-websocket: could not register %s",
             kGetStateName);
    }
    g_vendor = vendor;
    blog(LOG_INFO, "[feeds] obs-websocket vendor '%s' registered", kVendorName);
}

void VendorShutdown() {
    if (!g_vendor) return;
    obs_websocket_vendor_unregister_request(g_vendor, kGetStateName);
    g_vendor = nullptr;
}

bool VendorAvailable() {
    return g_vendor != nullptr;
}

void VendorEmitSpeakerChanged(const VendorPerson& speaker) {
    if (!g_vendor) return;
    obs_data_t* data = obs_data_create();
    WritePerson(data, speaker);
    obs_websocket_vendor_emit_event(g_vendor, "speaker_changed", data);
    for (int slot : speaker.slot_list) {
        const std::string type =
            "participant_" + std::to_string(slot) + "_speaking";
        obs_websocket_vendor_emit_event(g_vendor, type.c_str(), data);
    }
    obs_data_release(data);
}

void VendorEmitMeetingJoined() {
    EmitEmpty("meeting_joined");
}

void VendorEmitMeetingLeft() {
    EmitEmpty("meeting_left");
}

void VendorEmitChatMessage(const VendorPerson& sender, const std::string& message,
                           bool is_self) {
    if (!g_vendor) return;
    obs_data_t* data = obs_data_create();
    WritePerson(data, sender);
    obs_data_set_string(data, "message", message.c_str());
    obs_data_set_bool(data, "is_self", is_self);
    obs_websocket_vendor_emit_event(g_vendor, "chat_message", data);
    obs_data_release(data);
}

void VendorEmitShareStarted(const VendorPerson& sharer) {
    EmitPerson("share_started", sharer);
}

void VendorEmitShareStopped(const VendorPerson& sharer) {
    EmitPerson("share_stopped", sharer);
}

void VendorEmitParticipantJoined(const VendorPerson& person) {
    EmitPerson("participant_joined", person);
}

void VendorEmitParticipantLeft(const VendorPerson& person) {
    EmitPerson("participant_left", person);
}

void VendorEmitParticipantMuted(const VendorPerson& person, bool muted) {
    EmitPerson(muted ? "participant_muted" : "participant_unmuted", person);
}

}  // namespace feeds
