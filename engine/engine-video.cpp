// engine-video.cpp — Zoom SDK raw-video renderers writing frames to shared
// memory for the plugin to read.
//
// One subscription = one IZoomSDKRenderer + one named shared-memory region.
// The plugin sends participant_source_subscribe with a source UUID and a
// participant user ID; we create a renderer, subscribe it to that user's
// video, and every incoming frame gets written into the ring buffer in
// shared memory. The plugin maps the same region and pulls frames out on
// its render thread.
//
// Ported in spirit from v1.0.0's ZoomVideoCatcher. The SDK callback
// pattern is identical; only the output is different (shared memory
// instead of obs_source_output_video).

#include <windows.h>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>
#include <cstdio>

#include "engine-shared.h"
#include "zoom_sdk.h"
#include "zoom_sdk_raw_data_def.h"
#include "meeting_service_interface.h"
#include "meeting_service_components/meeting_audio_interface.h"
#include "meeting_service_components/meeting_participants_ctrl_interface.h"
#include "rawdata/rawdata_renderer_interface.h"
#include "rawdata/zoom_rawdata_api.h"

#include "shared-frame.h"
#include "engine-frame-scaler.h"
#include "engine-speaker.h"
#include "engine-audio.h"

// Defined in engine-main.cpp
extern void LogToFile(const char* msg);  // forwards at DEBUG
extern void LogInfo(const char* msg);
extern void LogWarn(const char* msg);
extern void LogError(const char* msg);
extern bool SendToPlugin(const std::string& json);

namespace feeds_engine {

// From engine-api.cpp
int GetCurrentTier();

// From engine-meeting.cpp — the ladder watchdog's camera check. (Active-speaker
// filtering lives in engine-speaker.cpp.)
ZOOM_SDK_NAMESPACE::IMeetingService* GetMeetingService();

// The sentinel user ID the plugin sends when a source is set to
// "[Active Speaker]". Matches the sentinel in plugin-main.cpp.
static constexpr unsigned int ACTIVE_SPEAKER_SENTINEL = 1;

// Result of ParticipantSubscription::Start(). RetryNotReady means createRenderer
// returned a transient not-ready code (SDKERR_VIDEO_NOTREADY 11 / NO_PERMISSION
// 12) — the raw-data renderer subsystem isn't up yet, so the caller should keep
// the request queued and retry. Failed is a real, non-retryable failure.
// SubscribeFailed means createRenderer worked but subscribe() was REFUSED by the
// SDK — the resolution-ladder case; the caller steps the source down a rung and
// requeues rather than keeping a renderer that will never deliver.
enum class SubStart { Started, RetryNotReady, Failed, SubscribeFailed };

// ---------------------------------------------------------------------------
// Resolution fallback ladder
//
// Zoom's raw-data subscription budget is a whole-meeting resource, not a
// per-renderer one: Zoom publishes "at most 1 Full HD video subscription in a
// meeting", "at most 2" at HD, and "does not support mixing Full HD and HD".
// Over-request and the SDK does one of two things — refuse subscribe() outright
// with SDKERR_WRONG_USAGE (2), or accept it and never deliver a frame. Both used
// to leave the source permanently black with no recovery.
//
// So a subscription now carries a RUNG. It starts at its tier's top rung — the
// same resolution this code has always requested — and steps DOWN only when an
// attempt provably fails to deliver. It never steps up, and it never steps down
// because a feed merely arrived smaller than requested: delivering 720p when
// 1080p was asked for is the SDK working as designed (and is the normal case on
// an account without the Full HD entitlement), so it is NOT a ladder trigger.
// See the trigger notes on kLadderFirstFrameMs below.
//
// Consequence worth stating plainly, because it is what makes this safe to ship
// without an Enhanced-Media account to test on: where no Full HD is available,
// no over-budget failure can occur, so no rung ever changes and every code path
// below is inert. The first attempt is byte-identical to the previous release.
enum : int { kLadderRungs = 3 };
static const ZOOM_SDK_NAMESPACE::ZoomSDKResolution kLadderRes[kLadderRungs] = {
    ZOOM_SDK_NAMESPACE::ZoomSDKResolution_1080P,
    ZOOM_SDK_NAMESPACE::ZoomSDKResolution_720P,
    ZOOM_SDK_NAMESPACE::ZoomSDKResolution_360P,
};
static const int kLadderHeight[kLadderRungs] = { 1080, 720, 360 };

static const char* LadderRungName(int rung) {
    if (rung < 0 || rung >= kLadderRungs) return "?";
    return (rung == 0) ? "1080p" : (rung == 1) ? "720p" : "360p";
}

// Top rung for the current tier. Tier 0 (Free) starts at 720p, tiers 1+ at
// 1080p — exactly what GetResolutionForCurrentTier() returned before the ladder
// existed, so a first attempt asks for the same thing it always has.
static int GetTopRungForCurrentTier() {
    return (GetCurrentTier() >= 1) ? 0 : 1;
}

// Output dimensions for the engine-side frame scaler. Matches the tier's
// SDK resolution ceiling so the scaler is upsizing (or pass-through) in
// the common case. Keeps OBS seeing a stable get_width / get_height
// regardless of Zoom's bandwidth-driven resolution changes.
static void GetScalerTargetForCurrentTier(int& w, int& h) {
    if (GetCurrentTier() >= 1) { w = 1920; h = 1080; }
    else                        { w = 1280; h = 720;  }
}

// Once-per-failure-type log bits for the SDK-callback validator. Held
// per-subscription so a single misbehaving source doesn't spam the log
// every frame.
namespace validation_failures {
    static constexpr unsigned int NULL_PLANE = 1u << 0;
    static constexpr unsigned int ZERO_DIM   = 1u << 1;
}

// ---------------------------------------------------------------------------
// JSON helpers (same primitives as elsewhere in the engine)
// ---------------------------------------------------------------------------
static std::string JsonExtractString(const std::string& json,
                                     const std::string& key)
{
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + search.size() + 1);
    if (pos == std::string::npos) return "";
    pos++;
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

static uint32_t JsonExtractUint(const std::string& json,
                                const std::string& key)
{
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos = json.find_first_of("0123456789", pos + search.size());
    if (pos == std::string::npos) return 0;
    size_t end = json.find_first_not_of("0123456789", pos);
    std::string numStr = json.substr(pos, end == std::string::npos
                                          ? std::string::npos : end - pos);
    try { return (uint32_t)std::stoul(numStr); } catch (...) { return 0; }
}

// ---------------------------------------------------------------------------
// SharedMemoryWriter — owns one file mapping and writes frames into it.
// One instance per subscription.
// ---------------------------------------------------------------------------
class SharedMemoryWriter {
public:
    SharedMemoryWriter() = default;
    ~SharedMemoryWriter() { Close(); }

    // Create and map the shared memory region. Returns true on success.
    bool Open(const std::string& regionName) {
        m_regionName = regionName;

        m_mapping = CreateFileMappingA(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            0, (DWORD)feeds_shared::REGION_SIZE,
            regionName.c_str());

        if (!m_mapping) {
            char msg[256];
            sprintf_s(msg, "Video: CreateFileMapping failed for '%s', err=%lu",
                      regionName.c_str(), GetLastError());
            LogError(msg);
            return false;
        }

        // If the mapping already existed, that's fine — we're just writing
        // into it. Could happen if a subscription was remade quickly.
        // We don't treat ERROR_ALREADY_EXISTS as a failure.

        m_view = MapViewOfFile(m_mapping, FILE_MAP_WRITE, 0, 0,
                               feeds_shared::REGION_SIZE);
        if (!m_view) {
            char msg[256];
            sprintf_s(msg, "Video: MapViewOfFile failed for '%s', err=%lu",
                      regionName.c_str(), GetLastError());
            LogError(msg);
            CloseHandle(m_mapping);
            m_mapping = nullptr;
            return false;
        }

        // Initialize the header. The slots' contents are garbage until
        // we write them, which is fine because the plugin only reads a
        // slot once write_index has advanced to include it.
        m_header = (feeds_shared::SharedFrameHeader*)m_view;
        m_header->magic           = feeds_shared::REGION_MAGIC;
        m_header->version         = feeds_shared::REGION_VERSION;
        m_header->write_index     = 0;
        m_header->last_read_index = 0;

        m_slots = (feeds_shared::FrameSlot*)
            ((uint8_t*)m_view + sizeof(feeds_shared::SharedFrameHeader));

        char msg[256];
        sprintf_s(msg, "Video: opened shared memory region '%s' (%zu bytes)",
                  regionName.c_str(), feeds_shared::REGION_SIZE);
        LogToFile(msg);
        return true;
    }

    void Close() {
        if (m_view) {
            UnmapViewOfFile(m_view);
            m_view = nullptr;
        }
        if (m_mapping) {
            CloseHandle(m_mapping);
            m_mapping = nullptr;
        }
        m_header = nullptr;
        m_slots  = nullptr;
    }

    // Write one I420 frame. Called from the scaler worker thread for
    // every scaled frame, and from the SDK status callbacks for the
    // blank-signal path. m_writeMutex serialises the two callers so a
    // status-change blank write can't trample an in-flight frame slot.
    //
    // TODO(stride): WriteFrame and the engine pipeline both assume
    // tight-packed I420 (stride == width). The Zoom SDK consumer API
    // (YUVRawDataI420) deliberately omits stride accessors that exist on
    // its sibling YUVProcessDataI420; the assumption holds in practice
    // today but is documented as fragile in research §12.1.
    void WriteFrame(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                    uint32_t width, uint32_t height)
    {
        if (!m_header || !m_slots) return;

        // Bounds check: refuse oversized frames rather than overrunning
        // the slot. Shouldn't happen in practice (we set resolution via
        // the SDK) but defensive.
        if (width  > feeds_shared::MAX_FRAME_WIDTH ||
            height > feeds_shared::MAX_FRAME_HEIGHT ||
            width  == 0 || height == 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_writeMutex);

        // Pick the next slot. We write to slot (write_index % RING_SLOTS),
        // then bump write_index. A reader seeing write_index = N knows
        // that slots 0..N-1 have been written at some point; the most
        // recent is slot ((N-1) % RING_SLOTS).
        uint32_t slot = m_header->write_index % feeds_shared::RING_SLOTS;
        feeds_shared::FrameSlot* dest = &m_slots[slot];

        dest->width    = width;
        dest->height   = height;
        dest->stride_y = width;
        dest->stride_u = width / 2;
        dest->stride_v = width / 2;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        dest->timestamp_ns = (uint64_t)now.QuadPart;

        // Copy Y plane, then U, then V. All three live contiguously in
        // dest->data.
        size_t ySize = (size_t)width * height;
        size_t uSize = (size_t)(width / 2) * (height / 2);
        size_t vSize = uSize;

        memcpy(dest->data, y, ySize);
        memcpy(dest->data + ySize, u, uSize);
        memcpy(dest->data + ySize + uSize, v, vSize);

        // Memory barrier before bumping write_index, so readers that see
        // the new index are guaranteed to see the new data.
        MemoryBarrier();

        m_header->write_index++;
    }

    // Write a "blank" sentinel slot. The plugin reads (width==0 ||
    // height==0) as "clear the OBS source" rather than render a frame.
    // Used when the SDK signals raw-data-off so the source goes
    // transparent instead of freezing on the last received frame.
    // Same mutex as WriteFrame so concurrent worker output + blank
    // signal can't end up writing to the same slot.
    void WriteBlankSignal()
    {
        if (!m_header || !m_slots) return;

        std::lock_guard<std::mutex> lock(m_writeMutex);

        uint32_t slot = m_header->write_index % feeds_shared::RING_SLOTS;
        feeds_shared::FrameSlot* dest = &m_slots[slot];

        dest->width    = 0;
        dest->height   = 0;
        dest->stride_y = 0;
        dest->stride_u = 0;
        dest->stride_v = 0;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        dest->timestamp_ns = (uint64_t)now.QuadPart;

        MemoryBarrier();

        m_header->write_index++;
    }

private:
    std::string m_regionName;
    HANDLE m_mapping = nullptr;
    void*  m_view = nullptr;
    feeds_shared::SharedFrameHeader* m_header = nullptr;
    feeds_shared::FrameSlot*         m_slots  = nullptr;
    std::mutex                       m_writeMutex;
};

// ---------------------------------------------------------------------------
// Subscription — one renderer + one shared memory writer + the delegate
// that bridges them. One Subscription per Zoom Participant source.
// ---------------------------------------------------------------------------
class ParticipantSubscription
    : public ZOOM_SDK_NAMESPACE::IZoomSDKRendererDelegate {
public:
    ParticipantSubscription(const std::string& sourceUuid,
                            unsigned int userId,
                            bool followActiveSpeaker,
                            int rung)
        : m_sourceUuid(sourceUuid),
          m_userId(userId),
          m_followActiveSpeaker(followActiveSpeaker),
          m_rung(rung < 0 ? 0 : (rung >= kLadderRungs ? kLadderRungs - 1 : rung)) {}

    ~ParticipantSubscription() {
        TearDown();
    }

    // Create the renderer and shared memory. If m_followActiveSpeaker is
    // true and no speaker is yet known (m_userId == ACTIVE_SPEAKER_SENTINEL),
    // we skip the subscribe call; caller should call Resubscribe once an
    // active speaker is available. Returns true on success.
    SubStart Start() {
        // Shared memory first so it's ready before any frames arrive.
        uint32_t pid = GetCurrentProcessId();
        std::string name = feeds_shared::MakeFrameRegionName(pid, m_sourceUuid);
        if (!m_writer.Open(name)) {
            LogError("Video: failed to open shared memory, aborting subscription");
            return SubStart::Failed;
        }

        // This source's isolated-audio ring, routed to the same user as the
        // video (re-pointed in Resubscribe). Non-fatal if it can't open.
        m_audioSink = OpenIsolatedAudioSink(m_sourceUuid, m_userId);

        // Create the SDK renderer with this object as the delegate.
        ZOOM_SDK_NAMESPACE::SDKError err =
            ZOOM_SDK_NAMESPACE::createRenderer(&m_renderer, this);
        if (err != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS || !m_renderer) {
            char msg[128];
            sprintf_s(msg, "Video: createRenderer failed: %d", (int)err);
            LogError(msg);
            m_writer.Close();
            CloseIsolatedAudioSink(m_audioSink);
            m_audioSink = 0;
            m_renderer = nullptr;
            // 11 (VIDEO_NOTREADY) / 12 (NO_PERMISSION) are the transient
            // raw-data-subsystem-not-ready codes seen at/just after the grant —
            // tell the caller to keep the request queued and retry. Any other
            // code is a real failure.
            if (err == ZOOM_SDK_NAMESPACE::SDKERR_VIDEO_NOTREADY ||
                err == ZOOM_SDK_NAMESPACE::SDKERR_NO_PERMISSION) {
                return SubStart::RetryNotReady;
            }
            return SubStart::Failed;
        }

        // Set resolution from this subscription's ladder rung. A brand-new
        // source is constructed at the tier's top rung, so the first attempt
        // requests exactly what it always did (720p on Free, 1080p on paid);
        // a lower rung is only ever reached after a failed attempt stepped it
        // down and requeued.
        //
        // The return code used to be discarded, which cost us the one signal
        // that separates "the meeting/account cannot carry this resolution"
        // from "bandwidth dipped for this participant". A refusal here means
        // the request was rejected outright and the SDK will deliver whatever
        // it picked instead, so the delivered size logged on the first frame
        // below is the only thing that tells us what we actually got. Not
        // fatal either way — we keep the subscription and report honestly, and
        // it is deliberately NOT a ladder trigger: the ladder steps down on
        // absence of delivery, never on small delivery.
        m_requestedHeight = kLadderHeight[m_rung];
        const ZOOM_SDK_NAMESPACE::SDKError resErr =
            m_renderer->setRawDataResolution(kLadderRes[m_rung]);
        {
            char msg[256];
            sprintf_s(msg,
                "Video: source='%s' requested %dp, setRawDataResolution "
                "returned %d (%s)",
                m_sourceUuid.c_str(), m_requestedHeight, (int)resErr,
                resErr == ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS
                    ? "accepted" : "REFUSED — SDK will pick its own");
            if (resErr == ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) LogInfo(msg);
            else                                              LogWarn(msg);
        }

        // Spin up the scaler worker. SDK callbacks stage frames here; the
        // worker libyuv-scales to the tier ceiling and hands the result
        // to m_writer.WriteFrame, so OBS sees constant dimensions
        // regardless of Zoom's bandwidth-driven resolution changes.
        // Capturing `this` is safe — TearDown joins the worker before
        // the subscription destructs.
        int targetW, targetH;
        GetScalerTargetForCurrentTier(targetW, targetH);
        m_worker = std::make_unique<FrameScalerWorker>(
            targetW, targetH,
            [this](const uint8_t* y, const uint8_t* u, const uint8_t* v,
                   int w, int h) {
                m_writer.WriteFrame(y, u, v, (uint32_t)w, (uint32_t)h);
            },
            m_sourceUuid);
        m_worker->Start();

        // Subscribe to the user's video, unless we're in follow-active-
        // speaker mode and no speaker has been designated yet.
        if (!m_followActiveSpeaker || m_userId != ACTIVE_SPEAKER_SENTINEL) {
            err = m_renderer->subscribe(m_userId,
                                         ZOOM_SDK_NAMESPACE::RAW_DATA_TYPE_VIDEO);
            if (err != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
                // MODE 2 — the SDK refused the subscription outright. The
                // budget case returns SDKERR_WRONG_USAGE (2): this meeting
                // already holds as much high-resolution video as it can carry.
                // This used to be logged and ignored, which left a renderer
                // that could never deliver and a source that stayed black for
                // the rest of the show. Report it up so the caller can step
                // this source down a rung and requeue.
                char msg[192];
                sprintf_s(msg,
                    "Video: LADDER source='%s' userId=%u subscribe REFUSED at "
                    "%s (code=%d%s)",
                    m_sourceUuid.c_str(), m_userId, LadderRungName(m_rung),
                    (int)err,
                    err == ZOOM_SDK_NAMESPACE::SDKERR_WRONG_USAGE
                        ? " WRONG_USAGE — over the meeting's resolution budget"
                        : "");
                LogWarn(msg);
                return SubStart::SubscribeFailed;
            } else {
                // Start the establishment clock: the ladder's MODE 3 watchdog
                // measures from here to the first delivered frame.
                m_subscribeTick = GetTickCount64();
                char msg[160];
                sprintf_s(msg,
                          "Video: subscribed source='%s' to userId=%u at %s%s",
                          m_sourceUuid.c_str(), m_userId,
                          LadderRungName(m_rung),
                          m_followActiveSpeaker ? " [follow-speaker]" : "");
                LogInfo(msg);
            }
        } else {
            char msg[128];
            sprintf_s(msg, "Video: source='%s' waiting for active speaker",
                      m_sourceUuid.c_str());
            LogToFile(msg);
        }

        return SubStart::Started;
    }

    bool FollowsActiveSpeaker() const { return m_followActiveSpeaker; }

    // Update the follow-mode flag without touching the renderer. Used by
    // HandleParticipantSourceSubscribe when the plugin switches an existing
    // source's dropdown into or out of [Active Speaker]. Resubscribe()
    // deliberately leaves the flag alone — it's also called from the
    // RetargetFollowSources loop, where the flag must not
    // change (the source is staying in follow mode, we're just pointing
    // it at a new user). So the handler, not Resubscribe, owns mode flips.
    void SetFollowsActiveSpeaker(bool v) { m_followActiveSpeaker = v; }

    // Re-point this subscription at a different user without tearing down
    // the renderer or shared memory. Used when the plugin changes the
    // dropdown selection, and by the active-speaker retarget loop.
    //
    // Returns false ONLY when the SDK refused the subscribe — the caller then
    // steps this source down a rung and requeues, exactly as it would for a
    // failed initial Start(). Returns true when the subscribe succeeded or was
    // intentionally skipped (follow-speaker with no speaker yet), neither of
    // which is a ladder trigger.
    bool Resubscribe(unsigned int newUserId) {
        if (!m_renderer) return true;

        // Always unsubscribe first — drops any existing subscription so
        // the source goes black while we wait.
        m_renderer->unSubscribe();
        m_userId = newUserId;
        // Audio follows the video's user. For a follow-speaker source still
        // waiting on a speaker this is the sentinel, which matches no one.
        SetIsolatedAudioSinkUser(m_audioSink, m_userId);

        // If this is a follow-speaker source and we don't yet know who's
        // speaking (newUserId == ACTIVE_SPEAKER_SENTINEL), skip the SDK
        // subscribe call entirely. Passing the sentinel would just get
        // rejected with "invalid user" — log noise. We stay unsubscribed
        // until RetargetFollowSources calls us again with a real ID.
        if (m_followActiveSpeaker && m_userId == ACTIVE_SPEAKER_SENTINEL) {
            // Not subscribed by choice — stop the establishment clock so the
            // ladder watchdog does not count this as a missing first frame.
            m_subscribeTick = 0;
            char msg[128];
            sprintf_s(msg,
                "Video: source='%s' waiting for active speaker",
                m_sourceUuid.c_str());
            LogToFile(msg);
            return true;
        }

        ZOOM_SDK_NAMESPACE::SDKError err =
            m_renderer->subscribe(m_userId,
                                   ZOOM_SDK_NAMESPACE::RAW_DATA_TYPE_VIDEO);
        if (err != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
            // A refused re-point used to log at debug — which OBS drops — and
            // leave the source black with nothing in the normal log to explain
            // it. Now it is a WARN and a ladder trigger.
            char msg[192];
            sprintf_s(msg,
                "Video: LADDER source='%s' userId=%u re-point REFUSED at %s "
                "(code=%d%s)",
                m_sourceUuid.c_str(), m_userId, LadderRungName(m_rung),
                (int)err,
                err == ZOOM_SDK_NAMESPACE::SDKERR_WRONG_USAGE
                    ? " WRONG_USAGE — over the meeting's resolution budget"
                    : "");
            LogWarn(msg);
            m_subscribeTick = 0;   // caller recreates; no watchdog on a dead sub
            return false;
        }

        // Fresh target, fresh establishment window. m_gotFirstFrame is atomic,
        // so clearing it here (pump thread) is safe against the SDK callback
        // thread that sets it. m_lastSrcW/H are deliberately NOT reset — they
        // are callback-thread-only, and the dimension-change detector spanning
        // the re-point is what reveals a re-point that cost resolution.
        m_gotFirstFrame.store(false, std::memory_order_release);
        m_subscribeTick = GetTickCount64();

        char msg[160];
        sprintf_s(msg, "Video: resubscribed source='%s' to userId=%u at %s%s",
                  m_sourceUuid.c_str(), m_userId, LadderRungName(m_rung),
                  m_followActiveSpeaker ? " [follow-speaker]" : "");
        LogToFile(msg);
        return true;
    }

    void TearDown() {
        // Order matters:
        //   1. SDK teardown first — after destroyRenderer returns, the
        //      SDK can no longer fire onRawDataFrameReceived (and
        //      therefore can't call m_worker->StageFrame after we
        //      destroy it).
        //   2. Worker stop+join — drains any in-flight scale + write.
        //      Synchronous join means no risk of the worker touching
        //      m_writer after Close.
        //   3. Writer close — releases the file mapping.
        // The isolated-audio sink is independent of all three (its writer is
        // the SDK audio callback, serialised inside engine-audio.cpp), so it
        // just closes last.
        if (m_renderer) {
            try {
                m_renderer->unSubscribe();
                ZOOM_SDK_NAMESPACE::destroyRenderer(m_renderer);
            } catch (...) {
                LogToFile("Video: exception during renderer teardown (ignored)");
            }
            m_renderer = nullptr;
        }
        if (m_worker) {
            m_worker->Stop();
            m_worker.reset();
        }
        m_writer.Close();
        CloseIsolatedAudioSink(m_audioSink);
        m_audioSink = 0;
    }

    // IZoomSDKRendererDelegate callbacks. Called by the SDK on its
    // internal thread. v1.2.2: the callback validates the frame, copies
    // the planes into the worker's staging slot, and signals. The worker
    // thread libyuv-scales to the tier ceiling and writes the result to
    // shared memory. Keeping the SDK callback cheap is what stops
    // Broadcaster-tier multi-renderer setups from serialising past one
    // frame's budget (research §12.4).
    virtual void onRawDataFrameReceived(YUVRawDataI420* data) override {
        // Delivery is confirmed: this renderer is actually receiving frames.
        // Signal the sequencing gate ONCE, on the 0->1 transition only, so the
        // next same-userId create proceeds promptly instead of waiting for the
        // poll tick. exchange() makes the post fire exactly once per renderer
        // (never per frame). PostMessageW is thread-safe from this SDK thread;
        // the WM_TIMER poll is the fallback if a post is ever missed. This
        // callback touches no mutex and never advances the queue itself.
        if (!m_gotFirstFrame.exchange(true, std::memory_order_release)) {
            if (g_anchorWnd)
                PostMessageW(g_anchorWnd, WM_FEEDS_PROCESS_RENDERERS, 0, 0);
        }

        if (!data || !m_worker) return;

        const uint8_t* y = (const uint8_t*)data->GetYBuffer();
        const uint8_t* u = (const uint8_t*)data->GetUBuffer();
        const uint8_t* v = (const uint8_t*)data->GetVBuffer();
        const int      w = (int)data->GetStreamWidth();
        const int      h = (int)data->GetStreamHeight();

        // Defensive validation. Reject + log-once-per-type rather than
        // forwarding a malformed frame and crashing the scaler.
        if (w <= 0 || h <= 0) {
            if (!(m_loggedFailures & validation_failures::ZERO_DIM)) {
                char msg[256];
                sprintf_s(msg,
                    "Video: source='%s' received frame with zero dimension "
                    "(%dx%d); dropping (logged once per subscription)",
                    m_sourceUuid.c_str(), w, h);
                LogWarn(msg);
                m_loggedFailures |= validation_failures::ZERO_DIM;
            }
            return;
        }
        if (!y || !u || !v) {
            if (!(m_loggedFailures & validation_failures::NULL_PLANE)) {
                char msg[256];
                sprintf_s(msg,
                    "Video: source='%s' received frame with null plane "
                    "buffer; dropping (logged once per subscription)",
                    m_sourceUuid.c_str());
                LogWarn(msg);
                m_loggedFailures |= validation_failures::NULL_PLANE;
            }
            return;
        }

        // Delivered-resolution visibility. INFO, not DEBUG: this used to log
        // at debug, which OBS drops from the normal log, so in every real
        // session the one measurement that says whether a feed is genuinely
        // 1080p was computed and thrown away. The engine's frame scaler
        // normalises everything to the tier size before OBS sees it, so a
        // participant delivering 720p is upscaled and looks like 1080p to
        // every other part of the system — these two lines are the ONLY place
        // the true delivered size is observable.
        //
        // Bounded by construction: one line on the first frame, then one per
        // actual change. A steady feed logs twice per session (subscribe +
        // first frame) and never again.
        if (w != m_lastSrcW || h != m_lastSrcH) {
            char msg[256];
            if (m_lastSrcW == 0 && m_lastSrcH == 0) {
                // First frame: state what arrived against what was asked for,
                // so one line answers "did we get the resolution we promised".
                sprintf_s(msg,
                    "Video: source='%s' first frame %dx%d (requested %dp)%s",
                    m_sourceUuid.c_str(), w, h, m_requestedHeight,
                    (h < m_requestedHeight) ? " — BELOW REQUEST" : "");
            } else {
                sprintf_s(msg,
                    "Video: frame dimensions changed for source='%s': "
                    "%dx%d -> %dx%d (requested %dp)",
                    m_sourceUuid.c_str(), m_lastSrcW, m_lastSrcH, w, h,
                    m_requestedHeight);
            }
            LogInfo(msg);
            m_lastSrcW = w;
            m_lastSrcH = h;
        }

        m_worker->StageFrame(y, u, v, w, h);
    }

    virtual void onRawDataStatusChanged(RawDataStatus status) override {
        char msg[128];
        sprintf_s(msg, "Video: source='%s' raw data status=%d",
                  m_sourceUuid.c_str(), (int)status);
        LogToFile(msg);

        // RawData_Off means frames have stopped flowing (camera off, user
        // left, host removed, network disconnect). Push a blank-sentinel
        // slot so the plugin clears its OBS source instead of freezing on
        // the last received frame. Recovery is automatic: when frames
        // resume the SDK fires onRawDataFrameReceived again and the plugin
        // renders the new slot normally.
        if (status == RawData_Off) {
            m_writer.WriteBlankSignal();
        }
    }

    virtual void onRendererBeDestroyed() override {
        // SDK destroyed our renderer (probably meeting ended). Push a
        // blank sentinel before nulling the renderer pointer — defensive
        // cover in case the SDK doesn't fire RawData_Off first on this
        // path. m_writer is still valid here: this callback runs while
        // our object is alive (either before TearDown starts, or
        // synchronously inside destroyRenderer before m_writer.Close()).
        LogToFile("Video: SDK destroyed renderer");
        m_writer.WriteBlankSignal();
        m_renderer = nullptr;
    }

    const std::string& SourceUuid() const { return m_sourceUuid; }
    unsigned int UserId() const { return m_userId; }

    // Push a blank-sentinel slot to the plugin without touching the SDK
    // renderer or our subscription state. Used when the bound participant
    // leaves the meeting — Zoom doesn't fire RawData_Off in that path,
    // so the plugin would otherwise freeze on the last received frame.
    // Keeping the renderer alive means a same-user-id rejoin resumes
    // automatically when frames flow again.
    void BlankSource() {
        m_writer.WriteBlankSignal();
    }

public:
    // --- Resolution-ladder accessors (main/pump thread, under g_subsMutex) ---
    int          Rung() const { return m_rung; }

    // Establishment clock for the ladder's MODE 3 watchdog: GetTickCount64() at
    // the moment subscribe() succeeded, or 0 when this subscription is not
    // currently awaiting a first frame (never subscribed, deliberately skipped,
    // already delivered, or already judged). Written and read only on the
    // pump thread under g_subsMutex.
    ULONGLONG    SubscribeTick() const       { return m_subscribeTick; }
    void         ClearSubscribeTick()        { m_subscribeTick = 0; }

private:
    std::string  m_sourceUuid;
    unsigned int m_userId;
    bool         m_followActiveSpeaker = false;

    // Current ladder rung (index into kLadderRes/kLadderHeight). Set at
    // construction and never mutated — a step-down builds a NEW subscription at
    // the next rung through the pending-render gate rather than re-pointing this
    // one, so the rung is immutable for the life of the object.
    int          m_rung = 0;
    ULONGLONG    m_subscribeTick = 0;
    ZOOM_SDK_NAMESPACE::IZoomSDKRenderer* m_renderer = nullptr;
    SharedMemoryWriter m_writer;

    // Token for this source's isolated-audio sink (engine-audio.h); 0 = none.
    // Set in Start, re-pointed in Resubscribe, closed in TearDown: the same
    // call sites (and locking) as the rest of this object's lifecycle.
    uint64_t           m_audioSink = 0;

    // Frame-scaler worker. Owns its own thread; SDK callback stages
    // frames here. Lives only while the subscription is active.
    std::unique_ptr<FrameScalerWorker> m_worker;

    // SDK-callback-only state; never read from outside that thread, so
    // no synchronisation needed.
    int                m_lastSrcW       = 0;  // last frame's width (0 = none yet)
    int                m_lastSrcH       = 0;
    unsigned int       m_loggedFailures = 0;  // bitfield from validation_failures

    // Requested resolution height (720 / 1080), for the delivered-vs-requested
    // log lines only — nothing reads it to make a decision. Written once in
    // Start() before any frame can arrive, then read on the SDK callback
    // thread, so it needs no synchronisation.
    int                m_requestedHeight = 0;

public:
    // Delivery-established signal for the per-userId sequencing gate: set true
    // on the FIRST frame the SDK routes to this renderer. Written (release) on
    // the SDK callback thread, read (acquire) by ProcessPendingRenderers on the
    // main thread — a lock-free cross-thread flag so the frame callback takes no
    // mutex. Lifetime-safe: TearDown unSubscribe()s + destroyRenderer()s before
    // the object destructs, so no callback races the read/free.
    std::atomic<bool> m_gotFirstFrame{false};
};

// ---------------------------------------------------------------------------
// Registry of active subscriptions. Keyed by source UUID.
// ---------------------------------------------------------------------------
static std::map<std::string, std::unique_ptr<ParticipantSubscription>> g_subs;
static std::mutex g_subsMutex;

// ---------------------------------------------------------------------------
// Public entry points: teardown hook for meeting end / logout
// ---------------------------------------------------------------------------

// There is deliberately no active-speaker state in this file. The on-screen
// target for a follow-active-speaker source is read from engine-speaker.cpp
// (GetResolvedActiveSpeakerTarget) every time it is needed. Lock order:
// g_subsMutex -> g_speakerMutex, never the reverse.

// ---------------------------------------------------------------------------
// Raw-render readiness gate + retry backstop (createRenderer-at-grant fix).
//
// createRenderer returns SDKERR_VIDEO_NOTREADY (11) / SDKERR_NO_PERMISSION (12)
// when called before the SDK's raw-data renderer subsystem is ready — the case
// for tens of ms right after raw_livestream_granted. The reliable readiness
// signal is our own user appearing in onUserRawLiveStreamingStatusChanged
// (-> NotifyRawRenderReady). So new-subscription requests are NOT turned into
// renderers immediately: they are queued here and drained once readiness is
// observed. A bounded retry (every kRenderRetryIntervalMs, up to kRenderGiveUpMs)
// backstops the residual case where the renderer is still briefly unready after
// the signal; on terminal give-up we tell the plugin so its subscribed-state
// guard can't wedge the source black forever.
//
// SEQUENCING (delivery-gated, per-userId): a burst of createRenderer+subscribe
// calls for the SAME userId makes the Zoom SDK establish delivery for only one
// renderer and silently drop the rest (proven: a spaced manual setup feeds many
// same-user renderers; a burst feeds only one — and blind ~300ms pacing was
// shown insufficient, leaving createRenderer=success with zero frames). So we do
// NOT create same-userId renderer N+1 until renderer N confirms delivery by
// actually receiving its first frame (m_gotFirstFrame), or a generous timeout
// (kEstablishTimeoutMs) elapses — replicating the manual reselect, which
// establishes one renderer in confirmed isolation. The gate is keyed on userId
// (g_inflightByUser): different-userId entries never wait on each other and are
// created immediately even while another userId is establishing; only same-userId
// siblings serialise. On timeout we proceed (leave the renderer created; that
// source recovers via a manual reselect as before) — no requeue-for-retry here.
//
// All renderer creation runs on the MAIN (pump) thread in ProcessPendingRenderers,
// reached via WM_FEEDS_PROCESS_RENDERERS (posted on queue/ready, and once from the
// frame callback on a renderer's first frame) and a WM_TIMER poll tick that
// re-checks the gate. State guarded by g_subsMutex.
// ---------------------------------------------------------------------------
struct PendingRender {
    std::string  sourceId;
    unsigned int userId;               // resolved id (follow entries re-resolved at drain)
    bool         followActiveSpeaker;
    ULONGLONG    deadlineTick;          // GetTickCount64() past which we give up
    int          rung;                  // resolution ladder rung to create at
};

// Per-userId in-flight establishment tracking for the delivery gate. One entry
// per userId that has a just-created renderer not yet confirmed delivering (or
// timed out). A same-userId queued entry waits while its userId is in-flight;
// different-userId entries are unaffected.
struct InflightEstablish {
    std::string sourceId;    // the renderer we're waiting on for this userId
    ULONGLONG   createTick;  // GetTickCount64() at create — establishment clock
};
static std::map<unsigned int, InflightEstablish> g_inflightByUser;

static bool                       g_rawRenderReady       = false;
static std::vector<PendingRender> g_pendingRenders;
static bool                       g_retryTimerActive     = false;
static const UINT_PTR  kRenderRetryTimerId    = 1;
static const UINT      kRenderRetryIntervalMs = 300;  // gate poll + not-ready retry
static const ULONGLONG kRenderGiveUpMs        = 15000;
// Per-renderer establishment timeout: how long to wait for a created renderer's
// first frame before proceeding to the next same-userId create. Deliberately
// generous — first frame arrives ~26ms when it works, but the legitimate worst
// case is unknown, so err high so a slow-but-real establishment is never
// abandoned into timeout-paced creation. Distinct from kRenderGiveUpMs (which is
// the never-created deadline); this is the created-but-not-yet-delivering clock.
static const ULONGLONG kEstablishTimeoutMs    = 2000;

// Resolution-ladder MODE 3 timeout: how long a SUBSCRIBED, camera-ON source may
// go without a first frame before the ladder concludes the SDK accepted the
// subscription and will never deliver it, and steps the source down a rung.
//
// Deliberately much longer than kEstablishTimeoutMs, and NOT a reuse of it.
// That one is a sibling-sequencing convenience where proceeding early is free;
// this one is destructive — acting on it tears down a renderer — so it needs
// real margin over a slow-but-healthy first frame (cold camera, congested
// uplink, a participant still finishing their join). A feed that takes six
// seconds to appear is not the bug this is hunting.
static const ULONGLONG kLadderFirstFrameMs    = 7000;

// Camera-on re-establishment debounce (trailing-edge). A participant's Video_ON
// posts WM_FEEDS_CAMERA_ON; a flickering camera is coalesced by (re)arming this
// timer and only re-establishing once it settles. Main-thread-owned, alongside
// the renderer-timer state above.
static const UINT_PTR         kCameraOnDebounceTimerId = 2;
static const UINT             kCameraOnDebounceMs      = 600;
static std::set<unsigned int> g_cameraOnPending;   // userIds awaiting re-establish

void TearDownAllVideoSubscriptions() {
    std::lock_guard<std::mutex> lock(g_subsMutex);
    if (!g_subs.empty()) {
        char msg[128];
        sprintf_s(msg, "Video: tearing down %zu subscriptions", g_subs.size());
        LogToFile(msg);
    }
    g_subs.clear();
    // The raw-data subsystem is gone until the next grant re-readies it: reset
    // the gate and drop any queued/retrying requests. The retry WM_TIMER, if
    // running, stops itself on its next tick when it finds the queue empty
    // (KillTimer is owned by ProcessPendingRenderers on the main thread).
    g_rawRenderReady = false;
    g_pendingRenders.clear();
    g_inflightByUser.clear();     // no renderers in flight; next create is immediate
}

// Blank any subscriptions currently bound to the given user ID. Called
// from the participants listener's onUserLeft so OBS sources tied to a
// departed participant clear immediately instead of freezing on the last
// frame — Zoom does not fire RawData_Off when a user leaves the meeting,
// only when their video stops while they remain in the meeting.
//
// We deliberately leave the subscription record and SDK renderer in
// place: the user_id binding is preserved so a same-id rejoin resumes
// automatically, and the OBS source stays positioned in the scene.
void BlankSubscriptionsForUser(unsigned int userId) {
    std::lock_guard<std::mutex> lock(g_subsMutex);
    int blanked = 0;
    for (auto& kv : g_subs) {
        if (kv.second && kv.second->UserId() == userId) {
            kv.second->BlankSource();
            ++blanked;
        }
    }
    if (blanked > 0) {
        char msg[128];
        sprintf_s(msg, "Video: blanked %d subscription(s) for departed user %u",
                  blanked, userId);
        LogToFile(msg);
    }
}

// ---------------------------------------------------------------------------
// IPC handlers
// ---------------------------------------------------------------------------

// Queue a (re)create request for the readiness-gated, SEQUENCED renderer
// creation in ProcessPendingRenderers (one per kRenderRetryIntervalMs).
// Replaces any prior queued request for the same source. Caller MUST hold
// g_subsMutex.
static void EnqueuePendingRenderLocked(const std::string& sourceId,
                                       unsigned int actualUserId,
                                       bool followActiveSpeaker,
                                       int rung) {
    if (rung < 0) rung = 0;
    if (rung >= kLadderRungs) rung = kLadderRungs - 1;
    bool replaced = false;
    for (auto& p : g_pendingRenders) {
        if (p.sourceId == sourceId) {
            p.userId              = actualUserId;
            p.followActiveSpeaker = followActiveSpeaker;
            p.deadlineTick        = GetTickCount64() + kRenderGiveUpMs;
            // Keep the LOWER of the two rungs (higher index). A queued
            // step-down must not be undone by a plain re-request arriving
            // behind it, or the ladder would oscillate against whatever keeps
            // re-queuing the source at the top rung.
            if (rung > p.rung) p.rung = rung;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        g_pendingRenders.push_back(
            {sourceId, actualUserId, followActiveSpeaker,
             GetTickCount64() + kRenderGiveUpMs, rung});
    }

    if (g_anchorWnd)
        PostMessageW(g_anchorWnd, WM_FEEDS_PROCESS_RENDERERS, 0, 0);
    else
        LogError("Video: no anchor window to drive pending renderer creation");
}

// Step one source DOWN the resolution ladder and requeue it, or retire it at the
// terminal rung. Caller MUST hold g_subsMutex.
//
// The step-down is deliberately a REQUEUE, not an inline destroy/recreate. The
// pending-render gate already serialises same-userId creates behind a confirmed
// first frame, which is exactly the race that makes an immediate recreate
// dangerous: recreating a renderer while the SDK is still asynchronously
// releasing the previous one for that participant is what returns WRONG_USAGE
// and can take out every source sharing the participant. Routing through the
// queue inherits that protection instead of reimplementing it, and it is the
// same reason HandleParticipantSourceRecreate always queues.
//
// `reason` is a short machine-greppable tag; it lands in the INFO line that is
// the only field evidence available for this path, because the trigger cannot be
// reproduced on an account without the Full HD entitlement.
static void LadderStepDownLocked(const std::string& sourceId,
                                 ParticipantSubscription* sub,
                                 const char* reason) {
    if (!sub) return;
    const int  cur      = sub->Rung();
    const int  next     = cur + 1;
    const unsigned int uid = sub->UserId();

    // Stop the watchdog on the current object either way: it has been judged.
    sub->ClearSubscribeTick();

    if (next >= kLadderRungs) {
        char msg[256];
        sprintf_s(msg,
            "Video: LADDER source='%s' userId=%u EXHAUSTED at %s "
            "(reason=%s) — no lower rung; notifying plugin",
            sourceId.c_str(), uid, LadderRungName(cur), reason);
        LogWarn(msg);
        // Tell the plugin once so its subscribed-state guard cannot pin the
        // source black forever; a later grant or a manual reselect can retry.
        SendToPlugin("{\"type\":\"participant_source_subscribe_failed\","
                     "\"source_id\":\"" + sourceId + "\"}");
        return;
    }

    char msg[256];
    sprintf_s(msg,
        "Video: LADDER source='%s' userId=%u stepping down %s -> %s "
        "(reason=%s, step %d of %d)",
        sourceId.c_str(), uid, LadderRungName(cur), LadderRungName(next),
        reason, next, kLadderRungs - 1);
    LogInfo(msg);

    EnqueuePendingRenderLocked(sourceId, uid, sub->FollowsActiveSpeaker(), next);
}

// participant_source_subscribe — plugin requests video for a source.
//   {"type":"participant_source_subscribe",
//    "source_id":"<uuid>",
//    "participant_id":<uint>}
//
// If participant_id == ACTIVE_SPEAKER_SENTINEL (1), this source follows
// whoever is currently the active speaker — subscribe to the current
// speaker now (if known) and re-point on speaker changes.
//
// If a subscription for this source already exists, we reuse the renderer
// and just re-point it at the new user. Cheaper than tear-down + recreate,
// and avoids a brief black-frame gap when the user changes the dropdown.
void HandleParticipantSourceSubscribe(const std::string& json) {
    std::string sourceId = JsonExtractString(json, "source_id");
    uint32_t    userId   = JsonExtractUint(json, "participant_id");

    if (sourceId.empty() || userId == 0) {
        LogToFile("Video: subscribe received with missing source_id or participant_id");
        return;
    }

    bool followActiveSpeaker = (userId == ACTIVE_SPEAKER_SENTINEL);

    // A follow-speaker subscribe — including a reselect of [Active Speaker] on a
    // source that already follows — asks the pump thread for a fresh derivation.
    // Posted before the lock, so it is dispatched ahead of any renderer drain
    // this request queues; RetargetFollowSources then corrects this source if
    // the target read below was out of date.
    if (followActiveSpeaker) RequestSpeakerEvaluation();

    std::lock_guard<std::mutex> lock(g_subsMutex);

    // For follow-active-speaker subscriptions, subscribe to the target
    // engine-speaker.cpp resolved. It is the sentinel when no one is
    // displayable yet, and Start()/Resubscribe() then skip the SDK subscribe
    // until RetargetFollowSources re-points us. A specific-participant
    // subscription uses its own userId and never reads active-speaker state.
    unsigned int actualUserId = followActiveSpeaker
        ? GetResolvedActiveSpeakerTarget()
        : userId;

    auto it = g_subs.find(sourceId);
    if (it != g_subs.end()) {
        // Carry the follow-mode flag through. Resubscribe() re-points the
        // user but deliberately leaves m_followActiveSpeaker alone (it's
        // also called from RetargetFollowSources' loop,
        // where the flag must not change). So if the plugin's dropdown
        // was switched into [Active Speaker] on an existing subscription,
        // this is where the follow flag flips on; switching to a specific
        // user flips it off. Without this line the source would stay
        // pointed at its initial user forever (the retarget loop checks
        // FollowsActiveSpeaker() and skips subs whose flag is stale).
        it->second->SetFollowsActiveSpeaker(followActiveSpeaker);

        // Existing subscription — just switch the user. A refused re-point is
        // a ladder trigger: step down and requeue rather than leaving a live
        // renderer pointed at a user it will never receive frames for.
        if (!it->second->Resubscribe(actualUserId)) {
            LadderStepDownLocked(sourceId, it->second.get(), "repoint_refused");
            return;
        }

        uint32_t pid = GetCurrentProcessId();
        char resp[512];
        sprintf_s(resp,
            "{\"type\":\"source_texture_ready\","
            "\"source_id\":\"%s\","
            "\"pid\":%u,"
            "\"width\":%u,"
            "\"height\":%u}",
            sourceId.c_str(), pid,
            feeds_shared::MAX_FRAME_WIDTH,
            feeds_shared::MAX_FRAME_HEIGHT);
        SendToPlugin(resp);
        return;
    }

    // New subscription. Do NOT create the renderer here: it must wait for the
    // raw-data renderer subsystem to be ready, and creation is sequenced. Queue
    // it for the gate, at the tier's top rung — a fresh source always asks for
    // the full resolution first and only ladders down if that provably fails.
    EnqueuePendingRenderLocked(sourceId, actualUserId, followActiveSpeaker,
                               GetTopRungForCurrentTier());
}

// participant_source_recreate — re-establish a source with a FRESH renderer
// (the gate destroys any existing one, then creates new) through the SEQUENCED
// gate. Sent by the plugin's auto-rebind / grant / rejoin path
// (SubscribeBoundSourceLocked), because a kept or re-pointed renderer loses SDK
// frame delivery on a participant drop→rejoin; only a fresh createRenderer,
// sequenced (not bursted), reliably recovers it — the automated equivalent of
// the manual deselect/reselect. Unlike subscribe this ALWAYS queues (no inline
// Resubscribe), so multi-source rejoins flow through the one-per-tick gate.
// Manual dropdown changes still use participant_source_subscribe (gentle inline
// re-point), which is fine because it's a single, user-paced operation.
void HandleParticipantSourceRecreate(const std::string& json) {
    std::string sourceId = JsonExtractString(json, "source_id");
    uint32_t    userId   = JsonExtractUint(json, "participant_id");
    if (sourceId.empty() || userId == 0) {
        LogToFile("Video: recreate received with missing source_id or participant_id");
        return;
    }
    bool followActiveSpeaker = (userId == ACTIVE_SPEAKER_SENTINEL);
    if (followActiveSpeaker) RequestSpeakerEvaluation();   // as in subscribe

    std::lock_guard<std::mutex> lock(g_subsMutex);
    // Follow-speaker: provisional target only — ProcessPendingRenderers
    // re-resolves it at drain time. Specific participant: userId unchanged.
    unsigned int actualUserId = followActiveSpeaker
        ? GetResolvedActiveSpeakerTarget()
        : userId;
    // Top rung: a recreate is a fresh start (rejoin / grant / auto-rebind), not
    // a ladder step, so it re-asks for full resolution. If the budget still
    // cannot carry it the ladder will step it down again from there.
    EnqueuePendingRenderLocked(sourceId, actualUserId, followActiveSpeaker,
                               GetTopRungForCurrentTier());
}

// ---------------------------------------------------------------------------
// Readiness-gated renderer creation — runs on the MAIN (pump) thread, reached
// from EngineWndProc via WM_FEEDS_PROCESS_RENDERERS and the retry WM_TIMER.
// Creates renderers for queued subscribes once the raw-data subsystem is ready,
// retries the transient not-ready failures, and gives up (notifying the plugin)
// past the per-request deadline.
// ---------------------------------------------------------------------------
void ProcessPendingRenderers() {
    std::lock_guard<std::mutex> lock(g_subsMutex);
    const ULONGLONG now = GetTickCount64();

    // DELIVERY-GATED, PER-USERID SEQUENCING: scan the queue and create the first
    // entry of each userId whose previous renderer has confirmed delivery (or
    // timed out); skip (do not break on) an entry whose userId is still
    // establishing. Different userIds proceed in parallel; only same-userId
    // siblings serialise. Non-create handling (give-ups, not-ready) still runs.
    for (size_t i = 0; i < g_pendingRenders.size(); ) {
        const std::string  sourceId = g_pendingRenders[i].sourceId;
        unsigned int       uid      = g_pendingRenders[i].userId;
        const bool         follow   = g_pendingRenders[i].followActiveSpeaker;
        const ULONGLONG    deadline = g_pendingRenders[i].deadlineTick;
        const int          rung     = g_pendingRenders[i].rung;

        // A follow-active-speaker request is ALWAYS re-resolved here, not only
        // when it was queued as the sentinel: the id captured at enqueue time
        // (by a recreate, a ladder step-down, or a camera-on re-establish) can
        // be several speakers old after the gate wait, and building the fresh
        // renderer on it would undo a newer retarget. Specific-participant
        // entries (follow == false) keep their queued userId untouched.
        if (follow)
            uid = GetResolvedActiveSpeakerTarget();

        // Past the deadline — give up and tell the plugin so its
        // subscribed_user_id guard can't pin the source black forever.
        if (now > deadline) {
            LogWarn("Video: subscribe gave up (renderer not ready in time) "
                    "— notifying plugin");
            SendToPlugin("{\"type\":\"participant_source_subscribe_failed\","
                         "\"source_id\":\"" + sourceId + "\"}");
            g_pendingRenders.erase(g_pendingRenders.begin() + i);
            continue;
        }

        // Gate: until the subsystem is ready, leave it queued (the readiness
        // notification and the retry tick will bring us back here).
        if (!g_rawRenderReady) { ++i; continue; }

        // Per-userId delivery gate: if this userId already has an in-flight
        // renderer, hold this sibling until that renderer's first frame confirms
        // the SDK routed delivery, or the establishment timeout elapses.
        auto inf = g_inflightByUser.find(uid);
        if (inf != g_inflightByUser.end()) {
            const ULONGLONG elapsed = now - inf->second.createTick;
            bool confirmed = false, timedOut = false;
            auto sit = g_subs.find(inf->second.sourceId);
            if (sit != g_subs.end() && sit->second) {
                if (sit->second->m_gotFirstFrame.load(std::memory_order_acquire))
                    confirmed = true;
                else if (elapsed >= kEstablishTimeoutMs)
                    timedOut = true;
            } else {
                // The in-flight renderer is gone (replaced/unsubscribed) — this
                // userId is no longer establishing; free it.
                confirmed = true;
            }

            if (confirmed) {
                g_inflightByUser.erase(inf);
                // uid is now free — fall through and create this entry.
            } else if (timedOut) {
                g_inflightByUser.erase(inf);
                // Proceed only: leave the timed-out renderer created (no worse
                // than before; recovers via manual reselect). uid now free.
            } else {
                // Still establishing — skip this sibling, keep it queued.
                ++i;
                continue;
            }
        }

        // uid is free — produce a FRESH renderer: destroy any existing
        // subscription for this source first (a rejoin's kept renderer is stale),
        // then create new. Mirrors the manual deselect/reselect.
        //
        // Faithful-reselect on the region surface: if there WAS a prior
        // subscription, emit source_texture_released after destroying it —
        // exactly what HandleParticipantSourceUnsubscribe does — so the plugin
        // closes its shared-memory region and reopens it cleanly on the
        // source_texture_ready below (Close -> Open, in order over E2P), instead
        // of reusing a stale region. Skipped for a brand-new source that never
        // had a region, so a first-time create doesn't double-release.
        const bool hadPriorSub = (g_subs.find(sourceId) != g_subs.end());
        g_subs.erase(sourceId);
        if (hadPriorSub) {
            SendToPlugin("{\"type\":\"source_texture_released\",\"source_id\":\""
                         + sourceId + "\"}");
        }

        auto sub = std::make_unique<ParticipantSubscription>(
            sourceId, uid, follow, rung);
        SubStart r = sub->Start();
        if (r == SubStart::Started) {
            g_subs[sourceId] = std::move(sub);
            g_pendingRenders.erase(g_pendingRenders.begin() + i);
            // Mark uid in-flight: same-userId siblings now wait for this
            // renderer's first frame (or timeout) before they are created.
            g_inflightByUser[uid] = { sourceId, now };

            uint32_t pid = GetCurrentProcessId();
            char resp[512];
            sprintf_s(resp,
                "{\"type\":\"source_texture_ready\","
                "\"source_id\":\"%s\",\"pid\":%u,\"width\":%u,\"height\":%u}",
                sourceId.c_str(), pid,
                feeds_shared::MAX_FRAME_WIDTH, feeds_shared::MAX_FRAME_HEIGHT);
            SendToPlugin(resp);
            // Do NOT break — keep scanning to create entries for OTHER userIds
            // that are free (different participants don't wait on each other).
            // The just-created entry was erased, so the next entry slides into i.
        } else if (r == SubStart::RetryNotReady) {
            // Transient — keep queued; a later tick re-attempts.
            ++i;
        } else if (r == SubStart::SubscribeFailed) {
            // MODE 2 — the SDK refused this subscription (the budget case is
            // SDKERR_WRONG_USAGE). `sub` is discarded here, which tears the
            // half-built renderer down cleanly via its destructor before we
            // re-attempt, so the retry does not stack renderers.
            //
            // Dequeue FIRST, then step down: LadderStepDownLocked re-enqueues
            // this same sourceId at the next rung, and leaving the old entry in
            // place would make that a same-source update of an entry we are
            // about to erase by index.
            g_pendingRenders.erase(g_pendingRenders.begin() + i);
            const int next = rung + 1;
            if (next >= kLadderRungs) {
                char msg[256];
                sprintf_s(msg,
                    "Video: LADDER source='%s' userId=%u EXHAUSTED at %s "
                    "(reason=subscribe_refused) — no lower rung; notifying plugin",
                    sourceId.c_str(), uid, LadderRungName(rung));
                LogWarn(msg);
                SendToPlugin("{\"type\":\"participant_source_subscribe_failed\","
                             "\"source_id\":\"" + sourceId + "\"}");
            } else {
                char msg[256];
                sprintf_s(msg,
                    "Video: LADDER source='%s' userId=%u stepping down %s -> %s "
                    "(reason=subscribe_refused, step %d of %d)",
                    sourceId.c_str(), uid, LadderRungName(rung),
                    LadderRungName(next), next, kLadderRungs - 1);
                LogInfo(msg);
                EnqueuePendingRenderLocked(sourceId, uid, follow, next);
                // Stop scanning this tick. The requeued entry was appended to
                // the very vector we are walking, so continuing here would
                // re-create this source immediately — microseconds after its
                // failed renderer was destroyed at the end of this iteration,
                // and with no gate entry to space them (a failed Start never
                // registers one). That back-to-back destroy/create against the
                // SDK's asynchronous participant release is the race this whole
                // design routes around. The next 300ms tick picks it up, which
                // is also the right pace for retrying into a budget that just
                // told us it was full.
                break;
            }
        } else {
            // Non-retryable failure.
            LogError("Video: subscription Start failed (non-retryable) "
                     "— notifying plugin");
            SendToPlugin("{\"type\":\"participant_source_subscribe_failed\","
                         "\"source_id\":\"" + sourceId + "\"}");
            g_pendingRenders.erase(g_pendingRenders.begin() + i);
        }
    }

    // -----------------------------------------------------------------------
    // MODE 3 watchdog — subscribed, camera ON, but no first frame ever arrived.
    //
    // The SDK can accept a subscription and simply never deliver it; this
    // engine proved that in-house (see the SEQUENCING note above: "leaving
    // createRenderer=success with zero frames"), and it is also how an
    // over-budget subscribe presents on some platforms instead of an error.
    // There is no callback for it, so the only detectable signal is absence of
    // a first frame past a generous deadline.
    //
    // THE CAMERA GATE IS LOAD-BEARING. A participant with their camera off
    // legitimately never sends a frame. Without IsVideoOn() this sweep would
    // fire on every camera-off person in every show, churning renderers and
    // ratcheting healthy sources down to 360p — on exactly the accounts where
    // the real trigger can never occur. Camera-off is ALREADY handled, and
    // handled correctly, by onUserVideoStatusChange -> QueueCameraOnReestablish,
    // which re-establishes the source when the camera comes back on. So when
    // the camera is off we stop watching and defer to that path; if the camera
    // returns, the recreate builds a fresh subscription with a fresh clock.
    // ONLY a camera-ON source that never delivered is a ladder trigger.
    // -----------------------------------------------------------------------
    {
        ZOOM_SDK_NAMESPACE::IMeetingParticipantsController* pc = nullptr;
        if (ZOOM_SDK_NAMESPACE::IMeetingService* ms = GetMeetingService())
            pc = ms->GetMeetingParticipantsController();

        for (auto& kv : g_subs) {
            ParticipantSubscription* s = kv.second.get();
            if (!s) continue;

            const ULONGLONG tick = s->SubscribeTick();
            if (tick == 0) continue;                    // not awaiting a first frame
            if (s->m_gotFirstFrame.load(std::memory_order_acquire)) {
                s->ClearSubscribeTick();                // delivered — stop watching
                continue;
            }
            if (now <= tick || now - tick < kLadderFirstFrameMs) continue;

            // Past the deadline. Decide whether this is a real failure.
            bool videoOn = false;
            if (pc) {
                if (auto* ui = pc->GetUserByUserID(s->UserId()))
                    videoOn = ui->IsVideoOn();
            }

            if (!videoOn) {
                // Camera off, or the participant is gone. Not a failure, and
                // not ours to act on. Debug-level: this is the common case in a
                // real show and must not flood the normal log.
                char msg[192];
                sprintf_s(msg,
                    "Video: ladder watchdog stood down for source='%s' "
                    "userId=%u (video off or user gone) — camera-on path owns "
                    "recovery",
                    kv.first.c_str(), s->UserId());
                LogToFile(msg);
                s->ClearSubscribeTick();
                continue;
            }

            // Camera is ON and nothing has arrived. Step down.
            char reason[64];
            sprintf_s(reason, "no_first_frame_%llums",
                      (unsigned long long)kLadderFirstFrameMs);
            LadderStepDownLocked(kv.first, s, reason);
        }
    }

    // Timer lifecycle: tick while anything is still queued (readiness-gated,
    // retrying, or waiting on a same-userId establishment), OR while any live
    // subscription is still inside its ladder establishment window — the
    // watchdog above is polled from this same tick, so it must stay armed even
    // when the create queue is empty. Stops once both are clear. (A confirmed
    // first frame also posts WM_FEEDS_PROCESS_RENDERERS directly, so the common
    // case doesn't wait for a tick.) Owned here so it's only touched on the
    // main thread.
    bool ladderWatchPending = false;
    for (const auto& kv : g_subs) {
        if (kv.second && kv.second->SubscribeTick() != 0) {
            ladderWatchPending = true;
            break;
        }
    }

    if (!g_pendingRenders.empty() || ladderWatchPending) {
        if (!g_retryTimerActive && g_anchorWnd) {
            SetTimer(g_anchorWnd, kRenderRetryTimerId, kRenderRetryIntervalMs,
                     nullptr);
            g_retryTimerActive = true;
        }
    } else if (g_retryTimerActive && g_anchorWnd) {
        KillTimer(g_anchorWnd, kRenderRetryTimerId);
        g_retryTimerActive = false;
    }
}

// Re-establish every source currently bound to userId through the SAME
// delivery-gated recreate path a rejoin uses: enqueue each into the gate, which
// does g_subs.erase + source_texture_released (Fix 2) + a fresh, first-frame-
// confirmed Start. Caller MUST hold g_subsMutex (mirrors EnqueuePendingRenderLocked).
// Scoped strictly to this participant's sources — not others, not all sources —
// with each source's follow-active-speaker flag preserved. When a camera turns
// on, this recovers renderers left created-but-unfed while it was off.
static void ReestablishSourcesForUserLocked(unsigned int userId) {
    for (auto& kv : g_subs) {
        if (kv.second && kv.second->UserId() == userId) {
            // Preserve the source's CURRENT rung rather than resetting to the
            // tier top. A camera toggle is not new information about the
            // meeting's resolution budget, so re-asking for a rung that already
            // failed would just fail again — and on a camera that flickers it
            // would ladder down, spring back up, and fail once per toggle. A
            // source only climbs back to full resolution on a deliberate fresh
            // start (recreate / rejoin / manual reselect).
            EnqueuePendingRenderLocked(kv.first, userId,
                                       kv.second->FollowsActiveSpeaker(),
                                       kv.second->Rung());
        }
    }
}

// Debounce fired (main thread): re-establish all pending userIds, then clear.
// KillTimer at the top makes the (periodic) debounce timer one-shot per settle.
static void ProcessCameraOnDebounce() {
    if (g_anchorWnd) KillTimer(g_anchorWnd, kCameraOnDebounceTimerId);

    std::vector<unsigned int> users(g_cameraOnPending.begin(),
                                    g_cameraOnPending.end());
    g_cameraOnPending.clear();
    if (users.empty()) return;

    std::lock_guard<std::mutex> lock(g_subsMutex);
    for (unsigned int uid : users)
        ReestablishSourcesForUserLocked(uid);
}

// Main-thread entry from WM_FEEDS_CAMERA_ON (posted by the video listener on a
// participant's Video_ON). Add the userId to the pending set and (re)arm the
// trailing-edge debounce timer so a flickering camera collapses to one
// re-establishment pass after it settles.
void QueueCameraOnReestablish(unsigned int userId) {
    if (userId == 0) return;
    g_cameraOnPending.insert(userId);
    if (g_anchorWnd)
        SetTimer(g_anchorWnd, kCameraOnDebounceTimerId, kCameraOnDebounceMs, nullptr);
}

// Single WM_TIMER dispatcher — the engine now runs two timers (the camera-on
// debounce and the renderer readiness/retry/gate poll); route by timer id.
void OnEngineTimer(UINT_PTR timerId) {
    if (timerId == kCameraOnDebounceTimerId) ProcessCameraOnDebounce();
    else                                     ProcessPendingRenderers();
}

// Called from engine-meeting.cpp's onUserRawLiveStreamingStatusChanged when our
// own user appears in the raw-live-streaming list — the renderer subsystem is
// ready. Opens the gate and nudges the main thread to drain queued subscribes.
void NotifyRawRenderReady() {
    {
        std::lock_guard<std::mutex> lock(g_subsMutex);
        if (g_rawRenderReady) return;   // already open — nothing to do
        g_rawRenderReady = true;
    }
    LogInfo("Video: raw render subsystem READY (self present) — "
            "draining queued subscribes");
    if (g_anchorWnd)
        PostMessageW(g_anchorWnd, WM_FEEDS_PROCESS_RENDERERS, 0, 0);
}

// Called on the pump thread by engine-speaker.cpp after every active-speaker
// evaluation (see engine-speaker.h). Deciding WHO to show — the Feeds-user and
// camera-off filters — happens there; this only applies the result.
//
// Level-triggered: re-points every follow-speaker source whose current user
// differs from the resolved target, rather than acting only when the target
// changes, so a follow source left on a stale user by any path converges on
// the next evaluation. A source already on the target is not touched, so a
// steady state issues no SDK calls. Specific-participant subscriptions are
// skipped by the FollowsActiveSpeaker() check and never touched.
//
// Must be called with g_speakerMutex NOT held (lock order).
FollowRetargetResult RetargetFollowSources() {
    FollowRetargetResult result;
    std::lock_guard<std::mutex> lock(g_subsMutex);

    // Read the target UNDER g_subsMutex (g_subsMutex -> g_speakerMutex is the
    // permitted order), so this always applies the newest committed
    // resolution, never a value captured before the lock was acquired.
    const unsigned int target = GetResolvedActiveSpeakerTarget();
    result.target = target;

    // A refused re-point is a ladder trigger, and it is handled by REQUEUEING
    // (LadderStepDownLocked touches g_pendingRenders, never g_subs) — an inline
    // destroy/recreate here would mutate the very container this loop is walking.
    for (auto& kv : g_subs) {
        ParticipantSubscription* s = kv.second.get();
        if (!s || !s->FollowsActiveSpeaker()) continue;
        ++result.follow;

        if (s->UserId() != target) {
            ++result.repointed;
            if (!s->Resubscribe(target))
                LadderStepDownLocked(kv.first, s, "repoint_refused");
        }

        if (!result.bound.empty()) result.bound += ",";
        result.bound += kv.first + "=" + std::to_string(s->UserId());
    }

    if (result.repointed > 0) {
        char msg[160];
        sprintf_s(msg,
            "Video: active speaker target userId=%u — re-pointed %d of %d "
            "follow source(s)",
            target, result.repointed, result.follow);
        LogInfo(msg);
    }
    return result;
}

// participant_source_unsubscribe — plugin no longer needs frames for this
// source. Tears down the renderer and closes the shared memory region.
//   {"type":"participant_source_unsubscribe","source_id":"<uuid>"}
void HandleParticipantSourceUnsubscribe(const std::string& json) {
    std::string sourceId = JsonExtractString(json, "source_id");
    if (sourceId.empty()) return;

    std::lock_guard<std::mutex> lock(g_subsMutex);
    auto it = g_subs.find(sourceId);
    if (it == g_subs.end()) return;

    g_subs.erase(it);

    char msg[256];
    sprintf_s(msg, "Video: unsubscribed source='%s'", sourceId.c_str());
    LogInfo(msg);

    char resp[256];
    sprintf_s(resp,
        "{\"type\":\"source_texture_released\",\"source_id\":\"%s\"}",
        sourceId.c_str());
    SendToPlugin(resp);
}

} // namespace feeds_engine
