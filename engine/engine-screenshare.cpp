// engine-screenshare.cpp — Zoom SDK raw-video renderer for screenshare.
//
// Unlike participant video (N renderers, one per OBS source), screenshare
// is a singleton in the engine: one IZoomSDKRenderer subscribed to
// whoever is currently sharing. Frames go into one well-known shared-
// memory region (see MakeScreenShareRegionName). The plugin can have N
// screenshare sources, all reading from the same region — each one has
// its own pump thread and delivers frames independently to its OBS source.
// This matches v1.0.0's single-renderer model but allows the user to
// copy/paste-reference the screenshare source in OBS for filter variants.
//
// State transitions:
//   - Engine startup: nothing allocated
//   - Meeting join: nothing allocated yet (wait for actual share)
//   - Someone starts sharing: ZoomShareListener updates globals, calls
//     UpdateShareSubscription() which lazily creates the renderer + SMW
//     and subscribes to the share source
//   - Sharer changes: Listener updates globals, calls UpdateShareSubscription()
//     which unsubscribes then resubscribes to the new sharer
//   - Share ends: Listener zeroes globals, calls UpdateShareSubscription()
//     which just unsubscribes (keeps renderer allocated for reuse)
//   - Meeting end / logout: TearDownScreenShare() fully releases the
//     renderer and shared-memory region

#include <windows.h>
#include <string>
#include <mutex>
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

// From engine-api.cpp
int GetCurrentTier();

// From engine-meeting.cpp — we read the current share state from the
// globals maintained there, but those are static to that TU. Accessors
// let us read them without moving the state.
unsigned int GetActiveSharerUserId();
unsigned int GetActiveShareSourceId();

// Tier-aware resolution policy. Same rule as participant video for now.
// If screenshare becomes its own tier feature (higher tiers get 1080p
// share, free tier gets 720p), this function is where that lives.
static ZOOM_SDK_NAMESPACE::ZoomSDKResolution GetShareResolution() {
    return (GetCurrentTier() >= 1)
        ? ZOOM_SDK_NAMESPACE::ZoomSDKResolution_1080P
        : ZOOM_SDK_NAMESPACE::ZoomSDKResolution_720P;
}

// ---------------------------------------------------------------------------
// Shared-memory writer for the screenshare region. Same layout and API
// as the participant one, but keyed on engine PID only (no UUID).
// ---------------------------------------------------------------------------
class ShareMemoryWriter {
public:
    ShareMemoryWriter()  = default;
    ~ShareMemoryWriter() { Close(); }

    bool Open() {
        if (m_view) return true;

        uint32_t pid = GetCurrentProcessId();
        std::string regionName = feeds_shared::MakeScreenShareRegionName(pid);

        m_mapping = CreateFileMappingA(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, (DWORD)feeds_shared::REGION_SIZE,
            regionName.c_str());

        if (!m_mapping) {
            char msg[256];
            sprintf_s(msg, "Share: CreateFileMapping failed for '%s', err=%lu",
                      regionName.c_str(), GetLastError());
            LogToFile(msg);
            return false;
        }

        m_view = MapViewOfFile(m_mapping, FILE_MAP_WRITE, 0, 0,
                               feeds_shared::REGION_SIZE);
        if (!m_view) {
            char msg[256];
            sprintf_s(msg, "Share: MapViewOfFile failed for '%s', err=%lu",
                      regionName.c_str(), GetLastError());
            LogToFile(msg);
            CloseHandle(m_mapping);
            m_mapping = nullptr;
            return false;
        }

        m_header = (feeds_shared::SharedFrameHeader*)m_view;
        m_header->magic           = feeds_shared::REGION_MAGIC;
        m_header->version         = feeds_shared::REGION_VERSION;
        m_header->write_index     = 0;
        m_header->last_read_index = 0;

        m_slots = (feeds_shared::FrameSlot*)
            ((uint8_t*)m_view + sizeof(feeds_shared::SharedFrameHeader));

        char msg[256];
        sprintf_s(msg, "Share: opened shared memory region '%s' (%zu bytes)",
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

    void WriteFrame(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                    uint32_t width, uint32_t height)
    {
        if (!m_header || !m_slots) return;
        if (width  > feeds_shared::MAX_FRAME_WIDTH ||
            height > feeds_shared::MAX_FRAME_HEIGHT ||
            width  == 0 || height == 0) {
            return;
        }

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

        size_t ySize = (size_t)width * height;
        size_t uSize = (size_t)(width / 2) * (height / 2);
        size_t vSize = uSize;

        if (ySize + uSize + vSize > feeds_shared::MAX_FRAME_BYTES) return;

        memcpy(dest->data,                   y, ySize);
        memcpy(dest->data + ySize,           u, uSize);
        memcpy(dest->data + ySize + uSize,   v, vSize);

        MemoryBarrier();
        m_header->write_index++;
    }

private:
    HANDLE m_mapping = nullptr;
    void*  m_view    = nullptr;
    feeds_shared::SharedFrameHeader* m_header = nullptr;
    feeds_shared::FrameSlot*         m_slots  = nullptr;
};

// ---------------------------------------------------------------------------
// Renderer delegate — receives I420 frames from the SDK and writes them
// to the shared-memory region.
// ---------------------------------------------------------------------------
class ShareRenderer : public ZOOM_SDK_NAMESPACE::IZoomSDKRendererDelegate {
public:
    void SetWriter(ShareMemoryWriter* w) { m_writer = w; }

    virtual void onRawDataFrameReceived(YUVRawDataI420* data) override {
        if (!m_writer || !data) return;
        m_writer->WriteFrame(
            (const uint8_t*)data->GetYBuffer(),
            (const uint8_t*)data->GetUBuffer(),
            (const uint8_t*)data->GetVBuffer(),
            data->GetStreamWidth(),
            data->GetStreamHeight());
    }

    virtual void onRawDataStatusChanged(RawDataStatus status) override {
        char msg[128];
        sprintf_s(msg, "Share: raw data status=%d", (int)status);
        LogToFile(msg);
    }

    virtual void onRendererBeDestroyed() override {
        // SDK destroyed our renderer (meeting ended, etc.). Drop the
        // pointer from our side so we don't try to use it. TearDown will
        // skip the destroyRenderer call.
        LogToFile("Share: SDK destroyed renderer");
    }

private:
    ShareMemoryWriter* m_writer = nullptr;
};

// ---------------------------------------------------------------------------
// Singleton state
// ---------------------------------------------------------------------------
static std::mutex                             g_shareMutex;
static ShareMemoryWriter                      g_shareWriter;
static ShareRenderer                          g_shareDelegate;
static ZOOM_SDK_NAMESPACE::IZoomSDKRenderer*  g_shareRenderer = nullptr;
static unsigned int                           g_currentSubscribeId = 0;  // what we're subscribed to (sharer or source id)

// ---------------------------------------------------------------------------
// Lazily create renderer + writer the first time we need them. Caller
// must hold g_shareMutex.
// ---------------------------------------------------------------------------
static bool EnsureShareInfrastructure() {
    if (g_shareRenderer) return true;

    if (!g_shareWriter.Open()) {
        LogToFile("Share: failed to open shared memory region");
        return false;
    }
    g_shareDelegate.SetWriter(&g_shareWriter);

    ZOOM_SDK_NAMESPACE::SDKError err =
        ZOOM_SDK_NAMESPACE::createRenderer(&g_shareRenderer, &g_shareDelegate);
    if (err != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS || !g_shareRenderer) {
        char msg[128];
        sprintf_s(msg, "Share: createRenderer failed: %d", (int)err);
        LogToFile(msg);
        g_shareWriter.Close();
        g_shareRenderer = nullptr;
        return false;
    }

    g_shareRenderer->setRawDataResolution(GetShareResolution());
    LogToFile("Share: renderer + shared memory ready");
    return true;
}

// ---------------------------------------------------------------------------
// Called from engine-meeting.cpp's ZoomShareListener::onSharingStatus and
// from the raw-livestream-granted callback path (so if someone's already
// sharing when we join, we catch up).
//
// Reads the current sharer/share-source globals (via the accessors above)
// and subscribes the renderer to the right thing. If no one is sharing,
// unsubscribes but keeps the renderer around for reuse.
// ---------------------------------------------------------------------------
void UpdateShareSubscription() {
    std::lock_guard<std::mutex> lock(g_shareMutex);

    unsigned int sharerUserId  = GetActiveSharerUserId();
    unsigned int shareSourceId = GetActiveShareSourceId();

    // No active share: unsubscribe if currently subscribed, but keep the
    // renderer around so the next share starts fast.
    if (sharerUserId == 0 && shareSourceId == 0) {
        if (g_shareRenderer && g_currentSubscribeId != 0) {
            g_shareRenderer->unSubscribe();
            g_currentSubscribeId = 0;
            LogToFile("Share: unsubscribed (no active sharer)");
        }
        return;
    }

    // Someone's sharing. Prefer the share source ID (more specific —
    // identifies which of possibly-multiple shares from one user). Fall
    // back to the user ID if source ID is zero (older SDK versions or
    // certain share types). Matches v1.0.0's preference order.
    unsigned int targetId =
        (shareSourceId != 0) ? shareSourceId : sharerUserId;

    if (g_currentSubscribeId == targetId && g_shareRenderer) {
        // Already subscribed to this exact source — no-op. Happens when
        // the same share fires multiple status callbacks.
        return;
    }

    if (!EnsureShareInfrastructure()) return;

    // Unsubscribe from whatever we were on before (if anything) and
    // resubscribe to the new target.
    if (g_currentSubscribeId != 0) {
        g_shareRenderer->unSubscribe();
    }

    ZOOM_SDK_NAMESPACE::SDKError err = g_shareRenderer->subscribe(
        targetId, ZOOM_SDK_NAMESPACE::RAW_DATA_TYPE_SHARE);

    if (err == ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
        g_currentSubscribeId = targetId;
        char msg[128];
        sprintf_s(msg, "Share: subscribed to id=%u (user=%u, source=%u)",
                  targetId, sharerUserId, shareSourceId);
        LogToFile(msg);
    } else {
        g_currentSubscribeId = 0;
        char msg[128];
        sprintf_s(msg, "Share: subscribe(id=%u) failed: %d",
                  targetId, (int)err);
        LogToFile(msg);
    }
}

// ---------------------------------------------------------------------------
// Full teardown — meeting end, logout, or engine shutdown. Releases the
// renderer and closes the shared-memory region. Next share will re-create.
// ---------------------------------------------------------------------------
void TearDownScreenShare() {
    std::lock_guard<std::mutex> lock(g_shareMutex);

    if (g_shareRenderer) {
        // unSubscribe before destroy — the SDK can be cranky if you
        // destroy a subscribed renderer. Matches the pattern in
        // engine-video.cpp.
        try {
            g_shareRenderer->unSubscribe();
            ZOOM_SDK_NAMESPACE::destroyRenderer(g_shareRenderer);
        } catch (...) {
            LogToFile("Share: exception during renderer teardown (ignored)");
        }
        g_shareRenderer      = nullptr;
        g_currentSubscribeId = 0;
        LogToFile("Share: tore down renderer");
    }

    g_shareDelegate.SetWriter(nullptr);
    g_shareWriter.Close();
}

} // namespace feeds_engine
