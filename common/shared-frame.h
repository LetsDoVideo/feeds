// shared-frame.h — Shared memory frame protocol between FeedsEngine.exe
// and feeds.dll.
//
// Frames flow from the engine to the plugin via a named-file-mapping ring
// buffer. One mapping per active participant subscription. Engine writes,
// plugin reads. All frames are I420 YUV (the Zoom SDK's native format —
// no color conversion anywhere in the pipeline).
//
// Design goals:
//   - Zero format conversion (I420 from SDK straight to OBS)
//   - One memcpy per frame (SDK buffer into shared memory)
//   - No locks in the hot path (atomic indices only)
//   - Ring depth just large enough to absorb jitter, not so large it adds
//     visible latency. Three slots = ~100ms at 30fps, ~50ms at 60fps.
//
// Lifetime:
//   1. Plugin creates a Zoom Participant source, picks a user from the
//      dropdown, sends participant_source_subscribe to engine.
//   2. Engine creates the shared memory region using MakeFrameRegionName()
//      and begins writing frames into it.
//   3. Engine sends source_texture_ready (misnomer — it's shared memory,
//      not a GPU texture; name kept for protocol compatibility) back to
//      the plugin with max dimensions.
//   4. Plugin opens the shared memory region (OpenFileMappingA by the same
//      name) and starts reading frames on its render thread.
//   5. On unsubscribe or source destroy, plugin sends participant_source_
//      unsubscribe; engine tears down the renderer and closes the mapping.
//
// The shared memory layout is:
//   [SharedFrameHeader] [FrameSlot 0] [FrameSlot 1] [FrameSlot 2]
//
// Writer flow:
//   - Pick next slot (write_index % RING_SLOTS)
//   - Fill the slot's header (dimensions, stride, timestamp) and Y/U/V data
//   - Memory barrier
//   - Atomically increment write_index
//
// Reader flow:
//   - Read write_index
//   - If write_index > last_read_index, read slot ((write_index - 1) % RING_SLOTS)
//   - Update last_read_index
//   - Hand the frame to OBS
//
// If the writer laps the reader (reader is slow or stalled), the writer
// overwrites the oldest slot. That's fine — we'd rather drop a frame than
// buffer it and add latency. Matches OBS's async_unbuffered philosophy.

#pragma once

#include <stdint.h>
#include <string>

namespace feeds_shared {

// Ring depth. 3 slots is the minimum that allows the writer to be writing
// one slot while the reader reads another, with a third in flight for
// jitter. Four would be slightly safer but 3 is what v1.0.0 effectively
// had (no ring at all, just single-buffer direct handoff, but the SDK
// does internal buffering) and the latency was perfect.
static constexpr uint32_t RING_SLOTS = 3;

// Max frame dimensions. 1920x1080 in I420 = 1920*1080*1.5 bytes = ~3MB.
// At 3 slots that's ~9MB per subscription. For a Broadcaster tier with
// 8 feeds plus screenshare, ~80MB total. Comfortable.
//
// We size for 1080p because the Zoom Enhanced Media feature (enabled on
// our app by Andy) can deliver 1080p60. We do not size for 4K; if we
// ever need 4K we revisit this number.
static constexpr uint32_t MAX_FRAME_WIDTH  = 1920;
static constexpr uint32_t MAX_FRAME_HEIGHT = 1080;

// I420: Y plane is width*height, U/V planes are each (width/2)*(height/2).
// Total = width*height*1.5.
static constexpr uint32_t MAX_FRAME_BYTES =
    (MAX_FRAME_WIDTH * MAX_FRAME_HEIGHT * 3) / 2;

// Single frame slot in the ring. Each slot is self-describing so the
// reader doesn't need to trust the header's dimensions — it uses the
// slot's own dimensions for the current frame.
struct FrameSlot {
    // Actual frame dimensions for this slot. May be smaller than MAX_*
    // if the SDK is delivering lower resolution (common for non-primary
    // feeds where v1.0.0 set 360p).
    uint32_t width;
    uint32_t height;

    // Strides for Y / U / V. I420 has Y stride = width, U/V stride = width/2.
    // Stored explicitly in case the SDK ever returns padded rows.
    uint32_t stride_y;
    uint32_t stride_u;
    uint32_t stride_v;

    // Engine's wall-clock timestamp at frame capture (QueryPerformanceCounter
    // or similar). Plugin may use this for debugging but the frame handed
    // to OBS gets a fresh os_gettime_ns() — this matches v1.0.0 behavior
    // where we use wall-clock-at-receive-time to avoid drift.
    uint64_t timestamp_ns;

    // Actual payload. Fixed-size max; the actual used bytes are
    // (width*height) for Y, (width/2 * height/2) each for U and V.
    // Layout: Y buffer, then U buffer, then V buffer, all in this one
    // byte array so the whole slot is a single contiguous blob.
    uint8_t data[MAX_FRAME_BYTES];
};

// Header at the start of the shared memory region. Single page, then the
// ring of frame slots follows.
struct SharedFrameHeader {
    // Protocol magic + version for defensive validation. If the plugin
    // opens a region and sees the wrong magic, it bails gracefully.
    uint32_t magic;   // 'FEED' = 0x46454544
    uint32_t version; // Bump this if the layout changes.

    // Atomic frame counters. Writer increments write_index after filling
    // a slot. Reader tracks its own last-seen index locally.
    // Using uint32_t + memory barriers rather than std::atomic because
    // this struct lives in shared memory and must be layout-identical
    // between engine and plugin.
    volatile uint32_t write_index;

    // Reader's last-seen write_index. Writer reads this (non-atomically
    // is fine; it's only used for diagnostics — we never block on it).
    volatile uint32_t last_read_index;

    // Padding to push FrameSlot 0 to a cache-line boundary.
    uint8_t _padding[64 - 16];
};

static constexpr uint32_t REGION_MAGIC   = 0x46454544; // 'FEED' in ASCII
static constexpr uint32_t REGION_VERSION = 1;

// Total size of the shared memory region.
static constexpr size_t REGION_SIZE =
    sizeof(SharedFrameHeader) + (RING_SLOTS * sizeof(FrameSlot));

// Construct the shared memory name. Must be identical between engine
// and plugin for a given subscription. Format:
//   Local\FeedsFrames_<engine_pid>_<source_uuid>
//
// The "Local\" prefix scopes the name to the current session, which is
// what we want. Multiple OBS instances with different engine PIDs can
// coexist. The source UUID makes it unique within one engine.
inline std::string MakeFrameRegionName(uint32_t enginePid,
                                        const std::string& sourceUuid)
{
    return "Local\\FeedsFrames_" + std::to_string(enginePid) + "_" + sourceUuid;
}

// Shared memory name for the screenshare region. Unlike participant feeds
// (which are UUID-keyed because N sources can exist, each subscribed to a
// different user), screenshare is a singleton in the engine: one SDK
// renderer subscribed to whoever is currently sharing. Multiple plugin-
// side screenshare sources may exist (for filter variations, etc.); they
// all map this same region and independently pump frames out to OBS.
//
// Region lifetime: created when the first screenshare source opens it in
// an active meeting, lives until the engine process exits. When no share
// is active the region exists but no frames are written; readers just see
// write_index unchanged and output nothing.
inline std::string MakeScreenShareRegionName(uint32_t enginePid)
{
    return "Local\\FeedsShare_" + std::to_string(enginePid);
}

} // namespace feeds_shared
