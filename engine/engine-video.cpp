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
#include <vector>
#include <mutex>
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

// Defined in engine-main.cpp
extern void LogToFile(const char* msg);  // forwards at DEBUG
extern void LogInfo(const char* msg);
extern void LogWarn(const char* msg);
extern void LogError(const char* msg);
extern bool SendToPlugin(const std::string& json);

namespace feeds_engine {

// From engine-api.cpp
int GetCurrentTier();

// From engine-meeting.cpp — needed for active-speaker filtering.
ZOOM_SDK_NAMESPACE::IMeetingService* GetMeetingService();
unsigned int GetMySelfUserId();

// The sentinel user ID the plugin sends when a source is set to
// "[Active Speaker]". Matches the sentinel in plugin-main.cpp.
static constexpr unsigned int ACTIVE_SPEAKER_SENTINEL = 1;

// Result of ParticipantSubscription::Start(). RetryNotReady means createRenderer
// returned a transient not-ready code (SDKERR_VIDEO_NOTREADY 11 / NO_PERMISSION
// 12) — the raw-data renderer subsystem isn't up yet, so the caller should keep
// the request queued and retry. Failed is a real, non-retryable failure.
enum class SubStart { Started, RetryNotReady, Failed };

// Map the current tier to the SDK resolution enum. Same values as v1.0.0.
// Tier 0 (Free) = 720p, everything else = 1080p.
static ZOOM_SDK_NAMESPACE::ZoomSDKResolution GetResolutionForCurrentTier() {
    return (GetCurrentTier() >= 1)
        ? ZOOM_SDK_NAMESPACE::ZoomSDKResolution_1080P
        : ZOOM_SDK_NAMESPACE::ZoomSDKResolution_720P;
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

        // TEMPORARY diagnostic ([feeds-rls], grep to remove) — per-source
        // frame-write counter, so a multi-source-one-participant rejoin log
        // shows, per region, whether write_index is actually advancing (i.e.
        // the engine is writing real frames). First write logged explicitly,
        // then ~once/second (every 30th). Pairs with the frame-recv counter in
        // onRawDataFrameReceived: recv-but-no-write => engine write path;
        // no-recv => SDK never delivers to this renderer.
        ++m_writeCount;
        if (m_writeCount == 1 || (m_writeCount % 30) == 0) {
            std::string tag = m_regionName;
            size_t us = tag.rfind('_');
            if (us != std::string::npos) tag = tag.substr(us + 1, 8);
            char dbg[160];
            sprintf_s(dbg, "[feeds-rls] frame-write source=%s writes=%llu",
                      tag.c_str(), (unsigned long long)m_writeCount);
            LogInfo(dbg);
        }
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
    unsigned long long               m_writeCount = 0;  // TEMPORARY ([feeds-rls])
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
                            bool followActiveSpeaker)
        : m_sourceUuid(sourceUuid),
          m_userId(userId),
          m_followActiveSpeaker(followActiveSpeaker) {}

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

        // Create the SDK renderer with this object as the delegate.
        ZOOM_SDK_NAMESPACE::SDKError err =
            ZOOM_SDK_NAMESPACE::createRenderer(&m_renderer, this);
        // TEMPORARY diagnostic ([feeds-rls], grep to remove) — record every
        // createRenderer attempt and its result code (12 == SDKERR_NO_PERMISSION
        // is the grant-instant readiness failure we're chasing) so the ordering
        // of grant sent -> readiness-callback firings -> createRenderer attempts
        // is visible in one rejoin's log. No behavior change.
        {
            char rls[160];
            sprintf_s(rls,
                "[feeds-rls] createRenderer attempt source='%s' userId=%u result=%d",
                m_sourceUuid.c_str(), m_userId, (int)err);
            LogInfo(rls);
        }
        if (err != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS || !m_renderer) {
            char msg[128];
            sprintf_s(msg, "Video: createRenderer failed: %d", (int)err);
            LogError(msg);
            m_writer.Close();
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

        // Set resolution based on the current tier. Tier 0 caps at 720p;
        // tiers 1+ get 1080p. Multi-participant support (future commit)
        // will apply the "first feed gets full resolution, extras drop to
        // 360p" rule — but Andy enabled an override on our account that
        // allows 1080p for all feeds up to tier max. For now we just use
        // the tier resolution straight.
        m_renderer->setRawDataResolution(GetResolutionForCurrentTier());

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
                char msg[128];
                sprintf_s(msg, "Video: subscribe(userId=%u) failed: %d",
                          m_userId, (int)err);
                LogError(msg);
                // Continue anyway — renderer exists, shared memory exists,
                // we'll just never get frames. Not fatal.
            } else {
                char msg[128];
                sprintf_s(msg, "Video: subscribed source='%s' to userId=%u%s",
                          m_sourceUuid.c_str(), m_userId,
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
    // NotifyActiveSpeakerChanged retarget loop, where the flag must not
    // change (the source is staying in follow mode, we're just pointing
    // it at a new user). So the handler, not Resubscribe, owns mode flips.
    void SetFollowsActiveSpeaker(bool v) { m_followActiveSpeaker = v; }

    // Re-point this subscription at a different user without tearing down
    // the renderer or shared memory. Used when the plugin changes the
    // dropdown selection.
    void Resubscribe(unsigned int newUserId) {
        if (!m_renderer) return;

        // Always unsubscribe first — drops any existing subscription so
        // the source goes black while we wait.
        m_renderer->unSubscribe();
        m_userId = newUserId;

        // If this is a follow-speaker source and we don't yet know who's
        // speaking (newUserId == ACTIVE_SPEAKER_SENTINEL), skip the SDK
        // subscribe call entirely. Passing the sentinel would just get
        // rejected with "invalid user" — log noise. We stay unsubscribed
        // until NotifyActiveSpeakerChanged calls us again with a real ID.
        if (m_followActiveSpeaker && m_userId == ACTIVE_SPEAKER_SENTINEL) {
            char msg[128];
            sprintf_s(msg,
                "Video: source='%s' waiting for active speaker",
                m_sourceUuid.c_str());
            LogToFile(msg);
            return;
        }

        ZOOM_SDK_NAMESPACE::SDKError err =
            m_renderer->subscribe(m_userId,
                                   ZOOM_SDK_NAMESPACE::RAW_DATA_TYPE_VIDEO);
        char msg[128];
        if (err == ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
            sprintf_s(msg, "Video: resubscribed source='%s' to userId=%u%s",
                      m_sourceUuid.c_str(), m_userId,
                      m_followActiveSpeaker ? " [follow-speaker]" : "");
        } else {
            sprintf_s(msg, "Video: resubscribe(userId=%u) failed: %d",
                      m_userId, (int)err);
        }
        LogToFile(msg);
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
    }

    // IZoomSDKRendererDelegate callbacks. Called by the SDK on its
    // internal thread. v1.2.2: the callback validates the frame, copies
    // the planes into the worker's staging slot, and signals. The worker
    // thread libyuv-scales to the tier ceiling and writes the result to
    // shared memory. Keeping the SDK callback cheap is what stops
    // Broadcaster-tier multi-renderer setups from serialising past one
    // frame's budget (research §12.4).
    virtual void onRawDataFrameReceived(YUVRawDataI420* data) override {
        // TEMPORARY diagnostic ([feeds-rls], grep to remove) — per-source SDK
        // frame-delivery counter, logged BEFORE any guard so it counts every
        // delivery the SDK makes to THIS renderer. If a black source never logs
        // a frame-recv line, the SDK isn't delivering to its (recreated)
        // renderer; if it logs frame-recv but no frame-write, the drop is in
        // the engine write path. First delivery explicit, then ~every 30th.
        ++m_frameRecvCount;
        if (m_frameRecvCount == 1 || (m_frameRecvCount % 30) == 0) {
            std::string tag = m_sourceUuid.substr(0, 8);
            char dbg[160];
            sprintf_s(dbg, "[feeds-rls] frame-recv source=%s recv=%llu",
                      tag.c_str(), (unsigned long long)m_frameRecvCount);
            LogInfo(dbg);
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

        // Dimension-change logging — helps diagnose future regressions
        // around resolution shifts. Skip the first valid frame (no
        // prior to compare against).
        if (w != m_lastSrcW || h != m_lastSrcH) {
            if (m_lastSrcW != 0 && m_lastSrcH != 0) {
                char msg[256];
                sprintf_s(msg,
                    "Video: frame dimensions changed for source='%s': "
                    "%dx%d -> %dx%d",
                    m_sourceUuid.c_str(), m_lastSrcW, m_lastSrcH, w, h);
                LogToFile(msg);
            }
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

private:
    std::string  m_sourceUuid;
    unsigned int m_userId;
    bool         m_followActiveSpeaker = false;
    ZOOM_SDK_NAMESPACE::IZoomSDKRenderer* m_renderer = nullptr;
    SharedMemoryWriter m_writer;

    // Frame-scaler worker. Owns its own thread; SDK callback stages
    // frames here. Lives only while the subscription is active.
    std::unique_ptr<FrameScalerWorker> m_worker;

    // SDK-callback-only state; never read from outside that thread, so
    // no synchronisation needed.
    int                m_lastSrcW       = 0;  // last frame's width (0 = none yet)
    int                m_lastSrcH       = 0;
    unsigned int       m_loggedFailures = 0;  // bitfield from validation_failures
    unsigned long long m_frameRecvCount = 0;  // TEMPORARY ([feeds-rls])
};

// ---------------------------------------------------------------------------
// Registry of active subscriptions. Keyed by source UUID.
// ---------------------------------------------------------------------------
static std::map<std::string, std::unique_ptr<ParticipantSubscription>> g_subs;
static std::mutex g_subsMutex;

// ---------------------------------------------------------------------------
// Public entry points: teardown hook for meeting end / logout
// ---------------------------------------------------------------------------

// Current active speaker ID, updated by NotifyActiveSpeakerChanged. 0 if
// no active speaker is known yet. Guarded by g_subsMutex for consistency
// with the subscription map.
static unsigned int g_currentActiveSpeaker = 0;

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
// All renderer creation runs on the MAIN (pump) thread in ProcessPendingRenderers,
// reached via WM_FEEDS_PROCESS_RENDERERS (posted on queue/ready) and a WM_TIMER
// retry tick. State guarded by g_subsMutex.
// ---------------------------------------------------------------------------
struct PendingRender {
    std::string  sourceId;
    unsigned int userId;               // resolved id (active-speaker pre-resolved)
    bool         followActiveSpeaker;
    ULONGLONG    deadlineTick;          // GetTickCount64() past which we give up
};
static bool                       g_rawRenderReady   = false;
static std::vector<PendingRender> g_pendingRenders;
static bool                       g_retryTimerActive = false;
static const UINT_PTR  kRenderRetryTimerId    = 1;
static const UINT      kRenderRetryIntervalMs = 300;
static const ULONGLONG kRenderGiveUpMs        = 15000;

void TearDownAllVideoSubscriptions() {
    std::lock_guard<std::mutex> lock(g_subsMutex);
    if (!g_subs.empty()) {
        char msg[128];
        sprintf_s(msg, "Video: tearing down %zu subscriptions", g_subs.size());
        LogToFile(msg);
    }
    g_subs.clear();
    g_currentActiveSpeaker = 0;
    // The raw-data subsystem is gone until the next grant re-readies it: reset
    // the gate and drop any queued/retrying requests. The retry WM_TIMER, if
    // running, stops itself on its next tick when it finds the queue empty
    // (KillTimer is owned by ProcessPendingRenderers on the main thread).
    g_rawRenderReady = false;
    g_pendingRenders.clear();
}

// === TEMPORARY (freeze diagnostic): live-subscription count for the engine
// heartbeat in engine-main.cpp. Remove together with the heartbeat block. ===
size_t GetActiveSubscriptionCount() {
    std::lock_guard<std::mutex> lock(g_subsMutex);
    return g_subs.size();
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

    std::lock_guard<std::mutex> lock(g_subsMutex);

    // For follow-active-speaker subscriptions, resolve the actual user ID
    // we should subscribe to right now. If no speaker is known yet, we
    // pass the sentinel through and Start()/Resubscribe() will skip the
    // SDK subscribe call until NotifyActiveSpeakerChanged re-points us.
    unsigned int actualUserId = followActiveSpeaker
        ? (g_currentActiveSpeaker != 0 ? g_currentActiveSpeaker : ACTIVE_SPEAKER_SENTINEL)
        : userId;

    auto it = g_subs.find(sourceId);
    if (it != g_subs.end()) {
        // Carry the follow-mode flag through. Resubscribe() re-points the
        // user but deliberately leaves m_followActiveSpeaker alone (it's
        // also called from NotifyActiveSpeakerChanged's retarget loop,
        // where the flag must not change). So if the plugin's dropdown
        // was switched into [Active Speaker] on an existing subscription,
        // this is where the follow flag flips on; switching to a specific
        // user flips it off. Without this line the source would stay
        // pointed at its initial user forever (the retarget loop checks
        // FollowsActiveSpeaker() and skips subs whose flag is stale).
        it->second->SetFollowsActiveSpeaker(followActiveSpeaker);

        // Existing subscription — just switch the user.
        it->second->Resubscribe(actualUserId);

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
    // raw-data renderer subsystem to be ready (else createRenderer returns
    // NO_PERMISSION/VIDEO_NOTREADY and the source stays black). Queue the
    // request (replacing any prior queued one for this source) and nudge the
    // main thread, which owns all renderer creation via ProcessPendingRenderers.
    bool replaced = false;
    for (auto& p : g_pendingRenders) {
        if (p.sourceId == sourceId) {
            p.userId              = actualUserId;
            p.followActiveSpeaker = followActiveSpeaker;
            p.deadlineTick        = GetTickCount64() + kRenderGiveUpMs;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        g_pendingRenders.push_back(
            {sourceId, actualUserId, followActiveSpeaker,
             GetTickCount64() + kRenderGiveUpMs});
    }

    char rls[256];
    sprintf_s(rls,
        "[feeds-rls] subscribe queued source='%s' userId=%u ready=%d pending=%zu",
        sourceId.c_str(), actualUserId, g_rawRenderReady ? 1 : 0,
        g_pendingRenders.size());
    LogInfo(rls);

    if (g_anchorWnd)
        PostMessageW(g_anchorWnd, WM_FEEDS_PROCESS_RENDERERS, 0, 0);
    else
        LogError("Video: no anchor window to drive pending renderer creation");
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

    for (size_t i = 0; i < g_pendingRenders.size(); ) {
        const std::string  sourceId = g_pendingRenders[i].sourceId;
        unsigned int       uid      = g_pendingRenders[i].userId;
        const bool         follow   = g_pendingRenders[i].followActiveSpeaker;
        const ULONGLONG    deadline = g_pendingRenders[i].deadlineTick;

        // Re-resolve a follow-active-speaker request that was queued before any
        // speaker was known, in case one became active during the gate wait.
        if (follow && uid == ACTIVE_SPEAKER_SENTINEL && g_currentActiveSpeaker != 0)
            uid = g_currentActiveSpeaker;

        // Already created (e.g. a prior tick, or an out-of-order request that
        // hit the existing-sub path): re-point and drop the queue entry.
        auto existing = g_subs.find(sourceId);
        if (existing != g_subs.end()) {
            existing->second->SetFollowsActiveSpeaker(follow);
            existing->second->Resubscribe(uid);
            g_pendingRenders.erase(g_pendingRenders.begin() + i);
            continue;
        }

        // Past the deadline — give up and tell the plugin so its
        // subscribed_user_id guard can't pin the source black forever.
        if (now > deadline) {
            LogWarn("[feeds-rls] subscribe gave up (renderer not ready in time) "
                    "— notifying plugin");
            SendToPlugin("{\"type\":\"participant_source_subscribe_failed\","
                         "\"source_id\":\"" + sourceId + "\"}");
            g_pendingRenders.erase(g_pendingRenders.begin() + i);
            continue;
        }

        // Gate: until the subsystem is ready, leave it queued (the readiness
        // notification and the retry tick will bring us back here).
        if (!g_rawRenderReady) { ++i; continue; }

        // Ready — attempt to create the renderer now.
        auto sub = std::make_unique<ParticipantSubscription>(
            sourceId, uid, follow);
        SubStart r = sub->Start();
        if (r == SubStart::Started) {
            g_subs[sourceId] = std::move(sub);
            g_pendingRenders.erase(g_pendingRenders.begin() + i);

            uint32_t pid = GetCurrentProcessId();
            char resp[512];
            sprintf_s(resp,
                "{\"type\":\"source_texture_ready\","
                "\"source_id\":\"%s\",\"pid\":%u,\"width\":%u,\"height\":%u}",
                sourceId.c_str(), pid,
                feeds_shared::MAX_FRAME_WIDTH, feeds_shared::MAX_FRAME_HEIGHT);
            SendToPlugin(resp);
            continue;
        }
        if (r == SubStart::RetryNotReady) {
            // Transient — keep queued; the retry timer will re-attempt.
            ++i;
            continue;
        }
        // Non-retryable failure.
        LogError("[feeds-rls] subscription Start failed (non-retryable) "
                 "— notifying plugin");
        SendToPlugin("{\"type\":\"participant_source_subscribe_failed\","
                     "\"source_id\":\"" + sourceId + "\"}");
        g_pendingRenders.erase(g_pendingRenders.begin() + i);
    }

    // Timer lifecycle: tick while anything is still queued (gated or retrying),
    // stop once the queue drains. Owned here so it's only touched on the main
    // thread.
    if (!g_pendingRenders.empty()) {
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

// Called from engine-meeting.cpp's onUserRawLiveStreamingStatusChanged when our
// own user appears in the raw-live-streaming list — the renderer subsystem is
// ready. Opens the gate and nudges the main thread to drain queued subscribes.
void NotifyRawRenderReady() {
    {
        std::lock_guard<std::mutex> lock(g_subsMutex);
        if (g_rawRenderReady) return;   // already open — nothing to do
        g_rawRenderReady = true;
    }
    LogInfo("[feeds-rls] raw render subsystem READY (self present) — "
            "draining queued subscribes");
    if (g_anchorWnd)
        PostMessageW(g_anchorWnd, WM_FEEDS_PROCESS_RENDERERS, 0, 0);
}

// Called from engine-meeting.cpp when the SDK reports an active speaker
// change. Filters out the Feeds user (virtual-camera loop risk) and
// speakers with video off (would show black frames — better to keep the
// last valid speaker on screen). If the new speaker passes filters,
// re-points all follow-speaker subscriptions to them.
void NotifyActiveSpeakerChanged(unsigned int newSpeakerId) {
    std::lock_guard<std::mutex> lock(g_subsMutex);

    if (newSpeakerId == 0) return;

    // Filter 1: Never subscribe to the Feeds user themselves. They're
    // running OBS and likely using virtual camera back to Zoom —
    // subscribing would create a recursive loop.
    unsigned int myUserId = GetMySelfUserId();
    if (myUserId != 0 && newSpeakerId == myUserId) {
        LogToFile("Video: active speaker is Feeds user, ignoring");
        return;
    }

    // Filter 2: Skip speakers with video off. Keeps the last valid
    // speaker on screen rather than showing a black frame for someone
    // who can't be displayed anyway.
    ZOOM_SDK_NAMESPACE::IMeetingService* ms = GetMeetingService();
    if (ms) {
        auto* participantCtrl = ms->GetMeetingParticipantsController();
        if (participantCtrl) {
            auto* userInfo = participantCtrl->GetUserByUserID(newSpeakerId);
            if (userInfo && !userInfo->IsVideoOn()) {
                char msg[128];
                sprintf_s(msg,
                    "Video: active speaker userId=%u has video off, keeping previous",
                    newSpeakerId);
                LogToFile(msg);
                return;
            }
        }
    }

    // Speaker passes both filters. Update state and retarget.
    if (g_currentActiveSpeaker == newSpeakerId) return;
    g_currentActiveSpeaker = newSpeakerId;

    int retargeted = 0;
    for (auto& kv : g_subs) {
        if (kv.second->FollowsActiveSpeaker()) {
            kv.second->Resubscribe(newSpeakerId);
            retargeted++;
        }
    }
    if (retargeted > 0) {
        char msg[128];
        sprintf_s(msg,
            "Video: active speaker changed to userId=%u, retargeted %d source(s)",
            newSpeakerId, retargeted);
        LogToFile(msg);
    }
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
