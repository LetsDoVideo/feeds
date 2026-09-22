#pragma once

#include <cstdint>
#include <string>

#include <obs.h>

// ISO recording of one OBS source to its own file alongside OBS's main
// recording.
//
// Feeds records one ISO file per PERSON: the ISO registry in plugin-main.cpp
// creates one recorder per enrolled participant, parented to a hidden private
// source that carries only that participant's clean feed. A recorder starts
// when it is enabled while OBS is recording (or when OBS recording starts),
// stops when OBS recording stops or it is disabled/destroyed, and pauses when
// OBS recording pauses. No filter appears in the UI.
//
// Each file carries that participant's ISOLATED audio (their own voice only,
// fed by feeds_iso_recorder_push_audio), never the OBS program mix, so
// stacking ISO files in an edit doesn't multiply every voice. The full mix
// stays in OBS's main recording.
//
// Architecture adopts Exeldro's Source Record patterns (obs_view +
// audio_output + obs_output) used directly inside the source rather than as
// an OBS filter, with the six known Source Record bugs fixed. The folder,
// format, and video encoder + settings are inherited from OBS's main
// recording config. See
// C:\Dev\iso-recording-investigation.md for the full rationale.
//
// Threading: the public functions below are called from the OBS UI/graphics
// threads (source lifecycle + frontend events), except push_audio, which the
// source's pump thread calls. The private audio output's callback runs on its
// own libobs audio thread; the module guards the shared audio state with a
// dedicated mutex.

// Isolated per-participant audio is delivered by the Windows engine only.
// Until the macOS engine delivers it too, macOS ISO files keep OBS's program
// mix (the previous behavior) rather than going silent.
#ifdef _WIN32
#define FEEDS_ISO_ISOLATED_AUDIO 1
#else
#define FEEDS_ISO_ISOLATED_AUDIO 0
#endif

namespace feeds {

struct feeds_iso_recorder;

// Resolves the ISO file's base name (everything but the extension). Called
// exactly once per file, when that file starts (so later renames don't rewrite
// it). The recorder uses it as-is apart from filename sanitisation, falls back
// to "Feeds ISO" when it is empty, and adds " (2)", " (3)"... rather than
// overwrite an existing file. May be null, in which case the recorder uses
// obs_source_get_name(parent). Runs on the graphics thread with the recorder's
// lock held: must not block, call back into the recorder, or throw.
using feeds_iso_name_fn = std::string (*)(void *userdata);

// Told the full path of each file right after it starts recording. Same
// thread and restrictions as the name hook. May be null.
using feeds_iso_started_fn = void (*)(void *userdata, const std::string &path);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
// parent_source is borrowed; the recorder never releases it, but its render
// view holds a reference to it while recording, so destroy the recorder BEFORE
// releasing a parent you own. name_fn / started_fn / userdata: see above.
feeds_iso_recorder *feeds_iso_recorder_create(obs_source_t *parent_source, feeds_iso_name_fn name_fn,
					      feeds_iso_started_fn started_fn, void *userdata);

// Record at a fixed width x height instead of following the parent's size.
// The parent is drawn at its own size in the frame (black where it doesn't
// cover, including while it has no picture at all), and a parent size change
// never restarts the file. Call before recording starts. 0 x 0 restores
// follow-the-parent.
void feeds_iso_recorder_set_fixed_size(feeds_iso_recorder *rec, uint32_t width, uint32_t height);

// Gracefully stops any active recording (drain-aware, bounded timeout) and
// frees the recorder. Safe to call with null.
void feeds_iso_recorder_destroy(feeds_iso_recorder *rec);

// ---------------------------------------------------------------------------
// Enable/disable.
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
// Isolated participant audio — one chunk of the source's participant audio as
// the engine delivered it: interleaved s16 PCM at the SDK's rate/channels,
// stamped with the os_gettime_ns() time of its first sample. Placed on the
// recording's timeline by that timestamp; anything not covered (silence,
// muted participant, no audio at all) records as silence. A cheap no-op when
// the source isn't recording. Called from one producer thread (the source's
// pump thread). No-op when FEEDS_ISO_ISOLATED_AUDIO is 0.
// ---------------------------------------------------------------------------
void feeds_iso_recorder_push_audio(feeds_iso_recorder *rec, const int16_t *pcm, uint32_t frames,
				   uint32_t sample_rate, uint32_t channels, uint64_t timestamp_ns);

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
