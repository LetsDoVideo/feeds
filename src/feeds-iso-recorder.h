#pragma once

#include <string>

#include <obs.h>

// Per-source ISO recording for Feeds participant sources (Phase 1).
//
// When the user ticks "Enable ISO recording" in a participant source's
// properties and starts OBS's main recording, that source records to its own
// MP4 alongside the main recording, frame-aligned with every other enabled
// ISO recording. It stops when OBS recording stops and pauses when OBS
// recording pauses. No filter appears in the UI — the recording machinery
// lives directly inside the participant source via this module.
//
// Architecture adopts Exeldro's Source Record patterns (obs_view +
// audio_output + obs_output) used directly inside the source rather than as
// an OBS filter, with the six known Source Record bugs fixed. Everything
// except the enable checkbox is inherited from OBS's main recording config:
// folder, format, video encoder + settings, audio encoder. See
// C:\Dev\iso-recording-investigation.md for the full rationale.
//
// Threading: the public functions below are called from the OBS UI/graphics
// threads (source lifecycle + frontend events). The audio callback runs on
// libobs's dedicated audio thread; the module guards its shared state
// accordingly.

namespace feeds {

struct feeds_iso_recorder;

// Resolves the human-readable name used in the ISO filename. Called exactly
// once, at record start (so mid-recording renames don't rewrite the file).
// Returns the chosen name; the recorder applies a final "Feeds ISO" fallback
// when this is empty, plus filename sanitisation. May be null, in which case
// the recorder uses obs_source_get_name(parent) only. Must not block, retry,
// or throw — it runs inline at start time.
using feeds_iso_name_fn = std::string (*)(void *userdata);

// ---------------------------------------------------------------------------
// Lifecycle — called from the participant source create/destroy callbacks.
// ---------------------------------------------------------------------------
// parent_source is borrowed (owned by the participant source); the recorder
// never releases it. name_fn/userdata supply the filename (see above).
feeds_iso_recorder *feeds_iso_recorder_create(obs_source_t *parent_source, feeds_iso_name_fn name_fn, void *userdata);

// Gracefully stops any active recording (drain-aware, bounded timeout) and
// frees the recorder. Safe to call with null.
void feeds_iso_recorder_destroy(feeds_iso_recorder *rec);

// ---------------------------------------------------------------------------
// Enable/disable from the checkbox in participant source properties.
// ---------------------------------------------------------------------------
// Honors the tier gate internally (records only when enabled && tier >= 1).
// On a false->true transition it resets the stuck-state guard and, if OBS is
// already recording, starts immediately. On true->false it gracefully stops.
void feeds_iso_recorder_set_enabled(feeds_iso_recorder *rec, bool enabled);
bool feeds_iso_recorder_is_enabled(const feeds_iso_recorder *rec);

// Called from the participant source's video_tick. Lazily (re)creates the
// private render view when the parent's dimensions become valid or change.
void feeds_iso_recorder_tick(feeds_iso_recorder *rec, float seconds);

// ---------------------------------------------------------------------------
// OBS frontend recording event hooks. These are invoked by the module's own
// single frontend-event callback (registered in feeds_iso_recorder_module_load)
// which fans out to every active recorder — participant sources do not call
// these directly.
// ---------------------------------------------------------------------------
void feeds_iso_recorder_on_obs_recording_started(feeds_iso_recorder *rec);
void feeds_iso_recorder_on_obs_recording_stopping(feeds_iso_recorder *rec);
void feeds_iso_recorder_on_obs_recording_paused(feeds_iso_recorder *rec);
void feeds_iso_recorder_on_obs_recording_unpaused(feeds_iso_recorder *rec);

// ---------------------------------------------------------------------------
// Tier gating — called when g_currentTier changes (from ReconcileSourcesToTier).
// Stores the new tier and gracefully stops any active recording if tier < 1.
// ---------------------------------------------------------------------------
void feeds_iso_recorder_set_tier(feeds_iso_recorder *rec, int tier);

// ---------------------------------------------------------------------------
// Module wiring — called once from obs_module_load / obs_module_unload.
// module_load registers the single frontend-event callback; module_unload
// removes it and drains any still-active recorders.
// ---------------------------------------------------------------------------
void feeds_iso_recorder_module_load();
void feeds_iso_recorder_module_unload();

}  // namespace feeds
