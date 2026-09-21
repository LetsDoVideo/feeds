// shared-audio.h — Shared memory protocol for per-participant (isolated) audio
// between FeedsEngine.exe and feeds.dll.
//
// One region per participant subscription, alongside that subscription's
// video frame region (shared-frame.h). The engine writes the isolated audio of
// whichever participant the subscription currently shows; the plugin drains
// it and hands it to the source's ISO recorder.
//
// Unlike the video ring, which deliberately keeps only the newest frame, this
// is a real FIFO: every slot is consumed in order. Audio can't drop "stale"
// chunks the way video drops frames without leaving holes in the recording.
//
// Writer flow (engine, SDK audio callback thread):
//   - Fill slot (write_index % AUDIO_RING_SLOTS): header fields + PCM
//   - Memory barrier
//   - Increment write_index
//
// Reader flow (plugin pump thread):
//   - For each index from the last one read up to write_index: copy the slot,
//     then re-read write_index; if the writer has lapped that slot while it
//     was being copied, the copy may be torn and is discarded.
//   - If write_index has run more than AUDIO_RING_SLOTS ahead (reader
//     stalled), or moved backwards (the engine re-created the region), resync
//     rather than read garbage.
//
// Timestamps are the engine's QueryPerformanceCounter reading scaled to
// nanoseconds, the same clock and scaling as libobs's os_gettime_ns() on
// Windows, so they are directly comparable in the plugin process.

#pragma once

#include <stdint.h>
#include <string>

#ifndef _WIN32
#include "shared-frame.h"  // PosixShmName
#endif

namespace feeds_shared {

// Ring depth. Zoom delivers ~10 ms chunks, so 64 slots is ~640 ms of slack
// against the plugin's 8 ms poll: far more than any healthy stall.
static constexpr uint32_t AUDIO_RING_SLOTS = 64;

// Interleaved int16 samples per slot: 20 ms of 48 kHz stereo. Zoom's default
// chunk (10 ms of 32 kHz mono) is 320. A larger chunk is split across slots.
static constexpr uint32_t AUDIO_SLOT_MAX_SAMPLES = 1920;

struct AudioSlot {
    uint32_t user_id;       // Zoom user id the audio belongs to
    uint32_t sample_rate;   // as delivered by the SDK (default 32000)
    uint32_t channels;      // 1 or 2
    uint32_t frames;        // samples per channel in pcm[]
    uint64_t timestamp_ns;  // time of the FIRST sample in this slot (see above)
    int16_t  pcm[AUDIO_SLOT_MAX_SAMPLES];  // interleaved s16le
};

struct SharedAudioHeader {
    uint32_t magic;    // 'FEEA' = 0x46454541
    uint32_t version;

    // Free-running count of slots written. Same shared-memory conventions as
    // SharedFrameHeader::write_index (volatile + explicit barriers, because
    // the struct must be layout-identical in both processes).
    volatile uint32_t write_index;

    uint8_t _padding[64 - 12];
};

static constexpr uint32_t AUDIO_REGION_MAGIC   = 0x46454541; // 'FEEA'
static constexpr uint32_t AUDIO_REGION_VERSION = 1;

static constexpr size_t AUDIO_REGION_SIZE =
    sizeof(SharedAudioHeader) + (AUDIO_RING_SLOTS * sizeof(AudioSlot));

// Region name. Must be identical between engine and plugin for a given
// subscription. Format: Local\FeedsAudio_<engine_pid>_<source_uuid>.
// On POSIX the logical name is hashed to fit the shm-name cap, exactly as
// MakeFrameRegionName does.
inline std::string MakeAudioRegionName(uint32_t enginePid,
                                        const std::string& sourceUuid)
{
    const std::string logical =
        "Local\\FeedsAudio_" + std::to_string(enginePid) + "_" + sourceUuid;
#ifdef _WIN32
    return logical;
#else
    return PosixShmName(logical);
#endif
}

} // namespace feeds_shared
