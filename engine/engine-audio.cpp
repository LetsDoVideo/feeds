// engine-audio.cpp — per-participant (isolated) raw audio from the Zoom SDK.
// See engine-audio.h for the model and common/shared-audio.h for the ring.
//
// Threads:
//   - SDK audio callback thread: onOneWayAudioRawDataReceived stamps the chunk
//     and copies it into the matching sinks' rings. It takes g_sinksMutex
//     briefly and nothing else, and makes no SDK calls.
//   - Main (pump) thread: subscribe / unsubscribe (SDK calls).
//   - Any engine thread (in practice the pump thread, from engine-video.cpp):
//     sink open / re-point / close, under g_sinksMutex.

#include <windows.h>
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "engine-audio.h"
#include "engine-shared.h"
#include "zoom_sdk.h"
#include "zoom_sdk_def.h"
#include "zoom_sdk_raw_data_def.h"
#include "rawdata/rawdata_audio_helper_interface.h"
#include "rawdata/zoom_rawdata_api.h"

#include "shared-audio.h"

// Defined in engine-main.cpp
extern void LogToFile(const char* msg);  // forwards at DEBUG
extern void LogInfo(const char* msg);
extern void LogWarn(const char* msg);
extern void LogError(const char* msg);

namespace feeds_engine {

namespace {

// ---------------------------------------------------------------------------
// Clock: QueryPerformanceCounter scaled to nanoseconds with the same integer
// arithmetic libobs uses for os_gettime_ns() on Windows (util_mul_div64), so a
// timestamp taken here means the same instant to the plugin.
// ---------------------------------------------------------------------------
uint64_t MulDiv64(uint64_t num, uint64_t mul, uint64_t div)
{
    const uint64_t rem = num % div;
    return (num / div) * mul + (rem * mul) / div;
}

uint64_t QpcNowNs()
{
    static const uint64_t freq = []() {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return (uint64_t)f.QuadPart;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return MulDiv64((uint64_t)now.QuadPart, 1000000000ULL, freq);
}

// ---------------------------------------------------------------------------
// AudioRegionWriter — owns one audio shared-memory region and appends slots.
// Single writer (the SDK audio callback, serialised by g_sinksMutex).
// ---------------------------------------------------------------------------
class AudioRegionWriter {
public:
    ~AudioRegionWriter() { Close(); }

    bool Open(const std::string& regionName) {
        m_mapping = CreateFileMappingA(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, (DWORD)feeds_shared::AUDIO_REGION_SIZE, regionName.c_str());
        if (!m_mapping) {
            char msg[256];
            sprintf_s(msg, "Audio: CreateFileMapping failed for '%s', err=%lu",
                      regionName.c_str(), GetLastError());
            LogError(msg);
            return false;
        }
        // ERROR_ALREADY_EXISTS is fine: the plugin may still hold the region
        // from a subscription this one replaces. Re-initialising the header
        // moves write_index backwards, which the reader treats as a resync.
        m_view = MapViewOfFile(m_mapping, FILE_MAP_WRITE, 0, 0,
                               feeds_shared::AUDIO_REGION_SIZE);
        if (!m_view) {
            char msg[256];
            sprintf_s(msg, "Audio: MapViewOfFile failed for '%s', err=%lu",
                      regionName.c_str(), GetLastError());
            LogError(msg);
            CloseHandle(m_mapping);
            m_mapping = nullptr;
            return false;
        }
        m_header = (feeds_shared::SharedAudioHeader*)m_view;
        m_slots  = (feeds_shared::AudioSlot*)
            ((uint8_t*)m_view + sizeof(feeds_shared::SharedAudioHeader));
        m_header->magic       = feeds_shared::AUDIO_REGION_MAGIC;
        m_header->version     = feeds_shared::AUDIO_REGION_VERSION;
        m_header->write_index = 0;
        return true;
    }

    void Close() {
        if (m_view)    { UnmapViewOfFile(m_view); m_view = nullptr; }
        if (m_mapping) { CloseHandle(m_mapping);  m_mapping = nullptr; }
        m_header = nullptr;
        m_slots  = nullptr;
    }

    // Append one SDK chunk, splitting it across slots if it's larger than a
    // slot holds. startNs is the time of the chunk's first sample.
    void Write(uint32_t userId, uint32_t rate, uint32_t channels,
               const int16_t* pcm, uint32_t frames, uint64_t startNs) {
        if (!m_header || !m_slots) return;
        const uint32_t maxFrames = feeds_shared::AUDIO_SLOT_MAX_SAMPLES / channels;
        while (frames > 0) {
            const uint32_t n = frames < maxFrames ? frames : maxFrames;
            feeds_shared::AudioSlot* slot =
                &m_slots[m_header->write_index % feeds_shared::AUDIO_RING_SLOTS];
            slot->user_id      = userId;
            slot->sample_rate  = rate;
            slot->channels     = channels;
            slot->frames       = n;
            slot->timestamp_ns = startNs;
            memcpy(slot->pcm, pcm, (size_t)n * channels * sizeof(int16_t));
            MemoryBarrier();
            m_header->write_index++;

            pcm     += (size_t)n * channels;
            frames  -= n;
            startNs += MulDiv64(n, 1000000000ULL, rate);
        }
    }

private:
    HANDLE m_mapping = nullptr;
    void*  m_view    = nullptr;
    feeds_shared::SharedAudioHeader* m_header = nullptr;
    feeds_shared::AudioSlot*         m_slots  = nullptr;
};

struct Sink {
    unsigned int      userId = 0;
    AudioRegionWriter writer;
};

std::mutex                                g_sinksMutex;
std::map<uint64_t, std::unique_ptr<Sink>> g_sinks;      // by token
uint64_t                                  g_nextToken = 1;

// ---------------------------------------------------------------------------
// SDK delegate. Only the one-way (per-participant) callback is used: the mixed
// callback is the whole meeting and must never reach an ISO recording, and
// share / interpreter audio are out of scope for participant sources.
// ---------------------------------------------------------------------------
class IsolatedAudioDelegate
    : public ZOOM_SDK_NAMESPACE::IZoomSDKAudioRawDataDelegate {
public:
    virtual void onMixedAudioRawDataReceived(AudioRawData*) override {}

    virtual void onOneWayAudioRawDataReceived(AudioRawData* data,
                                              uint32_t userId) override {
        // Stamp first: this is the arrival instant the A/V sync is built on.
        const uint64_t arrivalNs = QpcNowNs();
        if (!data) return;

        const char*    buf      = data->GetBuffer();
        const uint32_t len      = data->GetBufferLen();
        const uint32_t rate     = data->GetSampleRate();
        const uint32_t channels = data->GetChannelNum();
        if (!buf || rate == 0 || (channels != 1 && channels != 2)) return;
        const uint32_t frames = len / (channels * (uint32_t)sizeof(int16_t));
        if (frames == 0) return;

        // The chunk has just finished arriving; its first sample is one chunk
        // duration earlier.
        const uint64_t durNs   = MulDiv64(frames, 1000000000ULL, rate);
        const uint64_t startNs = arrivalNs > durNs ? arrivalNs - durNs : 0;

        if (!m_loggedFormat.exchange(true)) {
            char msg[160];
            sprintf_s(msg,
                "Audio: isolated participant audio flowing (%u Hz, %u ch, "
                "%u frames per chunk)", rate, channels, frames);
            LogInfo(msg);
        }

        std::lock_guard<std::mutex> lock(g_sinksMutex);
        for (auto& kv : g_sinks) {
            Sink* s = kv.second.get();
            if (s->userId == userId)
                s->writer.Write(userId, rate, channels,
                                (const int16_t*)buf, frames, startNs);
        }
    }

    virtual void onShareAudioRawDataReceived(AudioRawData*, uint32_t) override {}
    virtual void onOneWayInterpreterAudioRawDataReceived(
        AudioRawData*, const zchar_t*) override {}

    void ResetForNewMeeting() { m_loggedFormat = false; }

private:
    std::atomic<bool> m_loggedFormat{false};
};

IsolatedAudioDelegate g_delegate;

// Subscription state. g_subscribed / g_subscribeGaveUp are main-thread only;
// g_awaitingAudioJoin is set on the main thread and consumed on the SDK
// audio-status thread.
bool              g_subscribed        = false;
bool              g_subscribeGaveUp   = false;
std::atomic<bool> g_awaitingAudioJoin{false};

} // namespace

// ---------------------------------------------------------------------------
// Sinks
// ---------------------------------------------------------------------------
uint64_t OpenIsolatedAudioSink(const std::string& sourceUuid, unsigned int userId)
{
    auto sink = std::make_unique<Sink>();
    sink->userId = userId;
    const std::string name =
        feeds_shared::MakeAudioRegionName(GetCurrentProcessId(), sourceUuid);
    if (!sink->writer.Open(name)) {
        // Non-fatal: video carries on; this source's ISO file just gets no
        // participant audio.
        char msg[256];
        sprintf_s(msg, "Audio: no isolated-audio region for source='%s'",
                  sourceUuid.c_str());
        LogWarn(msg);
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_sinksMutex);
    const uint64_t token = g_nextToken++;
    g_sinks[token] = std::move(sink);
    return token;
}

void SetIsolatedAudioSinkUser(uint64_t token, unsigned int userId)
{
    if (token == 0) return;
    std::lock_guard<std::mutex> lock(g_sinksMutex);
    auto it = g_sinks.find(token);
    if (it != g_sinks.end()) it->second->userId = userId;
}

void CloseIsolatedAudioSink(uint64_t token)
{
    if (token == 0) return;
    std::unique_ptr<Sink> dead;
    {
        std::lock_guard<std::mutex> lock(g_sinksMutex);
        auto it = g_sinks.find(token);
        if (it == g_sinks.end()) return;
        dead = std::move(it->second);
        g_sinks.erase(it);
    }
    // Unmapped here, outside the lock, once no callback can reach it.
}

// ---------------------------------------------------------------------------
// Subscription
// ---------------------------------------------------------------------------
void RequestIsolatedAudioSubscribe()
{
    if (g_anchorWnd)
        PostMessageW(g_anchorWnd, WM_FEEDS_AUDIO_SUBSCRIBE, 0, 0);
}

void IsolatedAudioSubscribeOnMainThread()
{
    if (g_subscribed || g_subscribeGaveUp) return;

    ZOOM_SDK_NAMESPACE::IZoomSDKAudioRawDataHelper* helper =
        ZOOM_SDK_NAMESPACE::GetAudioRawdataHelper();
    if (!helper) {
        LogWarn("Audio: GetAudioRawdataHelper returned null; ISO recordings "
                "will have no participant audio this meeting");
        g_subscribeGaveUp = true;
        return;
    }

    const ZOOM_SDK_NAMESPACE::SDKError err = helper->subscribe(&g_delegate);
    if (err == ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
        g_subscribed = true;
        LogInfo("Audio: subscribed to per-participant audio for ISO recordings");
        return;
    }

    char msg[256];
    if (err == ZOOM_SDK_NAMESPACE::SDKERR_NOT_JOIN_AUDIO) {
        // Not a privilege problem: this client hasn't joined computer audio.
        // Retry when its audio status next changes (i.e. when it joins).
        sprintf_s(msg,
            "Audio: per-participant audio unavailable (code=%d): Feeds has not "
            "joined computer audio in this meeting. ISO recordings will have "
            "no participant audio until it does; retrying when audio joins.",
            (int)err);
        LogWarn(msg);
        g_awaitingAudioJoin = true;
        return;
    }

    sprintf_s(msg,
        "Audio: per-participant audio subscribe failed (code=%d); ISO "
        "recordings will have no participant audio this meeting", (int)err);
    LogWarn(msg);
    g_subscribeGaveUp = true;
}

void IsolatedAudioOnAudioStatusChanged()
{
    if (g_awaitingAudioJoin.exchange(false))
        RequestIsolatedAudioSubscribe();
}

void IsolatedAudioMeetingEnded()
{
    if (g_subscribed) {
        if (auto* helper = ZOOM_SDK_NAMESPACE::GetAudioRawdataHelper())
            helper->unSubscribe();
        LogInfo("Audio: unsubscribed from per-participant audio");
    }
    g_subscribed      = false;
    g_subscribeGaveUp = false;
    g_awaitingAudioJoin = false;
    g_delegate.ResetForNewMeeting();
}

} // namespace feeds_engine
