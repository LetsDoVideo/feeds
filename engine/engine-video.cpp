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
#include <mutex>
#include <memory>
#include <cstdio>

#include "zoom_sdk.h"
#include "zoom_sdk_raw_data_def.h"
#include "rawdata/rawdata_renderer_interface.h"
#include "rawdata/zoom_rawdata_api.h"

#include "shared-frame.h"

// Defined in engine-main.cpp
extern void LogToFile(const char* msg);
extern bool SendToPlugin(const std::string& json);

namespace feeds_engine {

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
            LogToFile(msg);
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
            LogToFile(msg);
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

    // Write one I420 frame. Called from the SDK's raw-data callback thread.
    // The three Y/U/V buffers are copied into the next ring slot.
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

private:
    std::string m_regionName;
    HANDLE m_mapping = nullptr;
    void*  m_view = nullptr;
    feeds_shared::SharedFrameHeader* m_header = nullptr;
    feeds_shared::FrameSlot*         m_slots  = nullptr;
};

// ---------------------------------------------------------------------------
// Subscription — one renderer + one shared memory writer + the delegate
// that bridges them. One Subscription per Zoom Participant source.
// ---------------------------------------------------------------------------
class ParticipantSubscription
    : public ZOOM_SDK_NAMESPACE::IZoomSDKRendererDelegate {
public:
    ParticipantSubscription(const std::string& sourceUuid, unsigned int userId)
        : m_sourceUuid(sourceUuid), m_userId(userId) {}

    ~ParticipantSubscription() {
        TearDown();
    }

    // Create the renderer, subscribe it, open shared memory. Returns true
    // on success; on failure the caller can discard the object.
    bool Start() {
        // Shared memory first so it's ready before any frames arrive.
        uint32_t pid = GetCurrentProcessId();
        std::string name = feeds_shared::MakeFrameRegionName(pid, m_sourceUuid);
        if (!m_writer.Open(name)) {
            LogToFile("Video: failed to open shared memory, aborting subscription");
            return false;
        }

        // Create the SDK renderer with this object as the delegate.
        ZOOM_SDK_NAMESPACE::SDKError err =
            ZOOM_SDK_NAMESPACE::createRenderer(&m_renderer, this);
        if (err != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS || !m_renderer) {
            char msg[128];
            sprintf_s(msg, "Video: createRenderer failed: %d", (int)err);
            LogToFile(msg);
            m_writer.Close();
            return false;
        }

        // Set resolution. For the first subscription we use 720p; multi-
        // participant scenarios may lower this in a future commit. For
        // Phase 6 we're single-participant.
        m_renderer->setRawDataResolution(
            ZOOM_SDK_NAMESPACE::ZoomSDKResolution_720P);

        // Subscribe to the user's video.
        err = m_renderer->subscribe(m_userId,
                                     ZOOM_SDK_NAMESPACE::RAW_DATA_TYPE_VIDEO);
        if (err != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
            char msg[128];
            sprintf_s(msg, "Video: subscribe(userId=%u) failed: %d",
                      m_userId, (int)err);
            LogToFile(msg);
            // Continue anyway — renderer exists, shared memory exists, we'll
            // just never get frames. Not fatal.
        } else {
            char msg[128];
            sprintf_s(msg, "Video: subscribed source='%s' to userId=%u",
                      m_sourceUuid.c_str(), m_userId);
            LogToFile(msg);
        }

        return true;
    }

    // Re-point this subscription at a different user without tearing down
    // the renderer or shared memory. Used when the plugin changes the
    // dropdown selection.
    void Resubscribe(unsigned int newUserId) {
        if (!m_renderer) return;
        m_renderer->unSubscribe();
        m_userId = newUserId;
        ZOOM_SDK_NAMESPACE::SDKError err =
            m_renderer->subscribe(m_userId,
                                   ZOOM_SDK_NAMESPACE::RAW_DATA_TYPE_VIDEO);
        char msg[128];
        if (err == ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
            sprintf_s(msg, "Video: resubscribed source='%s' to userId=%u",
                      m_sourceUuid.c_str(), m_userId);
        } else {
            sprintf_s(msg, "Video: resubscribe(userId=%u) failed: %d",
                      m_userId, (int)err);
        }
        LogToFile(msg);
    }

    void TearDown() {
        if (m_renderer) {
            // unSubscribe before destroy — the SDK can be cranky if you
            // destroy a subscribed renderer. v1.0.0 did this dance too.
            try {
                m_renderer->unSubscribe();
                ZOOM_SDK_NAMESPACE::destroyRenderer(m_renderer);
            } catch (...) {
                LogToFile("Video: exception during renderer teardown (ignored)");
            }
            m_renderer = nullptr;
        }
        m_writer.Close();
    }

    // IZoomSDKRendererDelegate callbacks. Called by the SDK on its internal
    // thread. We write the frame to shared memory; the plugin's render
    // thread picks it up.
    virtual void onRawDataFrameReceived(YUVRawDataI420* data) override {
        if (!data) return;
        m_writer.WriteFrame(
            (const uint8_t*)data->GetYBuffer(),
            (const uint8_t*)data->GetUBuffer(),
            (const uint8_t*)data->GetVBuffer(),
            data->GetStreamWidth(),
            data->GetStreamHeight());
    }

    virtual void onRawDataStatusChanged(RawDataStatus status) override {
        char msg[128];
        sprintf_s(msg, "Video: source='%s' raw data status=%d",
                  m_sourceUuid.c_str(), (int)status);
        LogToFile(msg);
    }

    virtual void onRendererBeDestroyed() override {
        // SDK destroyed our renderer (probably meeting ended). Drop the
        // pointer so we don't try to use it. Our destructor's TearDown
        // will skip it.
        LogToFile("Video: SDK destroyed renderer");
        m_renderer = nullptr;
    }

    const std::string& SourceUuid() const { return m_sourceUuid; }
    unsigned int UserId() const { return m_userId; }

private:
    std::string  m_sourceUuid;
    unsigned int m_userId;
    ZOOM_SDK_NAMESPACE::IZoomSDKRenderer* m_renderer = nullptr;
    SharedMemoryWriter m_writer;
};

// ---------------------------------------------------------------------------
// Registry of active subscriptions. Keyed by source UUID.
// ---------------------------------------------------------------------------
static std::map<std::string, std::unique_ptr<ParticipantSubscription>> g_subs;
static std::mutex g_subsMutex;

// ---------------------------------------------------------------------------
// Public entry points: teardown hook for meeting end / logout
// ---------------------------------------------------------------------------
void TearDownAllVideoSubscriptions() {
    std::lock_guard<std::mutex> lock(g_subsMutex);
    if (!g_subs.empty()) {
        char msg[128];
        sprintf_s(msg, "Video: tearing down %zu subscriptions", g_subs.size());
        LogToFile(msg);
    }
    g_subs.clear();
}

// ---------------------------------------------------------------------------
// IPC handlers
// ---------------------------------------------------------------------------

// participant_source_subscribe — plugin requests video for a source.
//   {"type":"participant_source_subscribe",
//    "source_id":"<uuid>",
//    "participant_id":<uint>}
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

    std::lock_guard<std::mutex> lock(g_subsMutex);

    auto it = g_subs.find(sourceId);
    if (it != g_subs.end()) {
        // Existing subscription — just switch the user.
        it->second->Resubscribe(userId);
        // Still send source_texture_ready so the plugin knows the mapping
        // is still valid. Plugin uses this to (re-)open the shared memory
        // region if it hadn't already.
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

    // New subscription.
    auto sub = std::make_unique<ParticipantSubscription>(sourceId, userId);
    if (!sub->Start()) {
        LogToFile("Video: subscription Start failed");
        return;
    }

    g_subs[sourceId] = std::move(sub);

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
    LogToFile(msg);

    char resp[256];
    sprintf_s(resp,
        "{\"type\":\"source_texture_released\",\"source_id\":\"%s\"}",
        sourceId.c_str());
    SendToPlugin(resp);
}

} // namespace feeds_engine
