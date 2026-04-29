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

// Max frame dimensions. 3840x2160 in I420 = 3840*2160*1.5 bytes = ~12.4MB.
// At 3 slots that's ~37MB per subscription. For a Broadcaster tier with
// 8 feeds plus screenshare, ~330MB total. Acceptable on modern systems.
//
// History: v1.0.0 through v1.0.3 used 1920x1080 here, sized for the Zoom
// Enhanced Media 1080p60 path. v1.0.4 bumped to 4K because participant
// video stays 1080p-capped by Zoom but screenshare frames can be much
// larger — phone-portrait shares come through at 1080x1920 (height
// exceeds 1080), and 1440p/4K monitor shares exceed the old buffer in
// both dimensions. Frames exceeding the buffer are dropped by the
// engine's WriteFrame, so the old 1080p ceiling silently broke
// screenshare for any source larger than a 1080p landscape display.
//
// v1.0.5 adds an optional alpha plane. Slot capacity grows from 1.5x to
// 2.5x of width*height to fit Y+U+V+alpha simultaneously. Shared-memory
// regions are fixed-size and we don't want to recreate them when alpha
// toggles, so the slot is sized for the worst case (alpha present) and
// frames without alpha simply leave the trailing region unused.
static constexpr uint32_t MAX_FRAME_WIDTH  = 3840;
static constexpr uint32_t MAX_FRAME_HEIGHT = 2160;

// I420: Y plane is width*height, U/V planes are each (width/2)*(height/2),
// total = width*height*1.5. v1.0.5 adds an optional alpha plane the same
// size as Y, bringing the worst-case total to width*height*2.5 = 5/2.
static constexpr uint32_t MAX_FRAME_BYTES =
    (MAX_FRAME_WIDTH * MAX_FRAME_HEIGHT * 5) / 2;

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

    // Stride of alpha plane in bytes. 0 = no alpha plane present
    // (I420 only). Non-zero = alpha plane is present in data[]
    // immediately after the V plane, with size (height * stride_a)
    // bytes. Used from v1.0.5 onward. Fills the 4 bytes of padding the
    // compiler would otherwise insert before timestamp_ns to keep it
    // 8-byte aligned, so existing field offsets are unchanged.
    uint32_t stride_a;

    // Engine's wall-clock timestamp at frame capture (QueryPerformanceCounter
    // or similar). Plugin may use this for debugging but the frame handed
    // to OBS gets a fresh os_gettime_ns() — this matches v1.0.0 behavior
    // where we use wall-clock-at-receive-time to avoid drift.
    uint64_t timestamp_ns;

    // Actual payload. Fixed-size max. Layout: Y buffer (width*height),
    // then U buffer (width/2 * height/2), then V buffer (same as U), then
    // optional alpha buffer (width*height — same size as Y) when stride_a
    // is non-zero. Readers must check stride_a before reading past the V
    // plane.
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
static constexpr uint32_t REGION_VERSION = 3; // bumped to 3 in v1.0.5 to add alpha plane support

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
