// feeds-mac-video.mm — participant video, screenshare and the active-speaker
// target for the macOS engine.
//
// This is the macOS counterpart of engine-video.cpp + engine-screenshare.cpp +
// engine-speaker.cpp. None of those files could be ported: they are written
// against the Windows SDK's IZoomSDKRenderer / createRenderer C++ surface and
// the macOS SDK has ZoomSDKRenderer, an Objective-C class reached through
// ZoomSDKRawDataController. What IS carried over unchanged is the CONTRACT —
// the same JSON messages and byte-identical shared memory — so the plugin reads
// a macOS engine's frames with exactly the code that reads a Windows engine's.
//
// ── The flow, end to end ─────────────────────────────────────────────────────
//   1. The host grants the raw-livestream privilege; the live-stream delegate
//      calls VideoStartRawLiveStream, which calls startRawLiveStreaming. THAT
//      is the call that turns raw-data delivery on — the privilege alone moves
//      no pixels.
//   2. Our own user then appears in onUserRawLiveStreamingStatusChanged, which
//      is the SDK's only reliable "the renderer subsystem is up" signal, and
//      opens the gate (VideoNotifyRawRenderReady).
//   3. The plugin sends participant_source_subscribe / _recreate per source.
//      Each request is QUEUED, never turned into a renderer inline (see the
//      gate note below), and drained on a 300 ms tick.
//   4. A drained request creates a ZoomSDKRenderer, sets its resolution and
//      subscribes it to the user id, with one shared-memory region per source.
//   5. Frames arrive on onRawDataReceived: and are copied straight into that
//      region. The plugin's pump thread reads them and hands them to OBS.
//
// ── Threading: two domains and nothing in between ────────────────────────────
// MAIN QUEUE owns the subscription registry, the gate, the speaker state and
// every SDK call. It is main-queue-CONFINED, which is why none of it is behind
// a mutex: the IPC reader thread dispatches commands to the main queue, and SDK
// delegates are dispatched there too.
//
// The SDK's frame callback thread touches exactly one thing: a FrameSink, held
// by the renderer's delegate through a shared_ptr, whose shared-memory writer
// carries its own mutex. It never reads the registry, so it can never be
// holding a lock that the main thread needs.
//
// That split is not tidiness, it is the fix for a real deadlock. destroyRender
// invokes onRendererBeDestroyed SYNCHRONOUSLY on the calling thread, and it can
// also wait for an in-flight frame callback to return. Any design where the
// frame callback and the teardown path contend for one lock has a cycle in it.
// Here teardown holds no lock the frame callback wants, and — belt and braces —
// it clears renderer.delegate BEFORE destroyRender, so the re-entrant
// onRendererBeDestroyed cannot reach our code at all.
//
// ── Frame lifetime ───────────────────────────────────────────────────────────
// A ZoomSDKYUVRawDataI420 is valid only for the duration of the callback unless
// addRef'd. Raw data runs in heap memory mode, so a stashed pointer is not a
// stale-but-readable buffer, it is a use-after-free. Every frame is therefore
// COPIED into shared memory inside the callback and nothing is retained past
// it — which also means no addRef/releaseData pairing to get wrong.
//
// ── ARC ──────────────────────────────────────────────────────────────────────
// ARC is on for the whole project. The renderer's `delegate` property is
// `assign` (unowned), so each subscription holds its own strong reference to
// its delegate object for exactly as long as it holds the renderer.

#import <Cocoa/Cocoa.h>
#import <ZoomSDK/ZoomSDK.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include "feeds-json-lite.h"
#include "feeds-mac-login.h"   // EngineSend / EngineLog
#include "feeds-mac-video.h"
#include "shared-frame.h"

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
void LogInfo(const std::string& m)  { feeds_mac::EngineLog("info", m); }
void LogWarn(const std::string& m)  { feeds_mac::EngineLog("warning", m); }
void LogError(const std::string& m) { feeds_mac::EngineLog("error", m); }
void Send(const std::string& j)     { feeds_mac::EngineSend(j); }

uint64_t NowMs()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Every public entry point funnels through this, so a caller cannot put the
// registry on the wrong thread by accident. Inline when already on the main
// thread, because several callers are SDK delegates that are already there and
// a hop would reorder them against the state change that prompted the call.
void OnMain(void (^block)(void))
{
    if ([NSThread isMainThread]) block();
    else                        dispatch_async(dispatch_get_main_queue(), block);
}

ZoomSDKMeetingService* MeetingService()
{
    return [[ZoomSDK sharedSDK] getMeetingService];
}

ZoomSDKMeetingActionController* ActionController()
{
    ZoomSDKMeetingService* svc = MeetingService();
    return svc ? [svc getMeetingActionController] : nil;
}

ZoomSDKRawDataController* RawDataController()
{
    return [[ZoomSDK sharedSDK] getRawDataController];
}

unsigned int MyUserId()
{
    ZoomSDKMeetingActionController* action = ActionController();
    if (!action) return 0;
    ZoomSDKUserInfo* me = [action getMyself];
    return me ? [me getUserID] : 0;
}

// "Has this user's camera been reported off?" A MISSING user record is not
// treated as camera-off: the roster can lag a fresh join, and the next
// evaluation re-checks anyway.
bool UserCameraOff(unsigned int userId)
{
    ZoomSDKMeetingActionController* action = ActionController();
    if (!action) return false;
    ZoomSDKUserInfo* info = [action getUserByUserID:userId];
    if (!info) return false;
    return ![info isVideoOn];
}

// The broadcast URL and name the host sees. Same two strings the Windows engine
// sends, so one host sees one consistent request whichever platform the user
// is on. (Duplicated rather than shared with the engine's copy because a
// constant is cheaper to read twice than to plumb between translation units.)
NSString* const kRawBroadcastUrl  = @"https://letsdovideo.com/feeds-support/";
NSString* const kRawBroadcastName = @"Feeds";

// The sentinel the plugin sends for a source set to [Active Speaker]. Matches
// the sentinel in plugin-main.cpp and in engine-video.cpp.
constexpr unsigned int kActiveSpeakerSentinel = 1;

// ---------------------------------------------------------------------------
// MacFrameWriter — one POSIX shared-memory region, written frame by frame.
//
// The region's LAYOUT is feeds_shared's, identical to the Windows engine's, and
// must stay that way: the plugin maps it with the same structs. What differs is
// only how the memory is obtained, and there are two macOS traps in that.
//
// TRAP 1 — the name. Handled in shared-frame.h (POSIX shm names are capped at
// 31 characters and the logical names are roughly 55), so by the time a name
// reaches this class it already fits. Nothing here needs to know.
//
// TRAP 2 — ftruncate. A POSIX shm object on macOS is sized exactly ONCE;
// ftruncate on an object that already has a size fails with EINVAL, which is
// not how CreateFileMapping behaves and not what a port would expect. So Open
// never ftruncates an existing object: it creates with O_EXCL, and on the
// EEXIST path it unlinks the survivor and creates afresh rather than trying to
// resize it. A survivor under one of our names can only be a leak from a
// crashed engine with the same PID, so there is nothing in it worth keeping.
//
// The mutex is the ONLY lock the SDK's frame-callback thread ever takes. Its
// scope is deliberately this class and nothing else — see the threading note at
// the top of the file.
// ---------------------------------------------------------------------------
class MacFrameWriter {
public:
    MacFrameWriter() = default;
    ~MacFrameWriter() { Close(); }

    MacFrameWriter(const MacFrameWriter&)            = delete;
    MacFrameWriter& operator=(const MacFrameWriter&) = delete;

    bool Open(const std::string& regionName, const std::string& label)
    {
        Close();

        std::lock_guard<std::mutex> lock(m_mutex);
        m_name = regionName;

        int fd = shm_open(regionName.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0 && errno == EEXIST) {
            // A region under this name outlived its creator. Drop it and take
            // the name; see TRAP 2 above for why we do not try to reuse it.
            LogWarn("Video: shared memory region '" + regionName +
                    "' already existed (leaked by an earlier engine); "
                    "unlinking and recreating it");
            shm_unlink(regionName.c_str());
            fd = shm_open(regionName.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        }
        if (fd < 0) {
            LogError("Video: shm_open failed for '" + regionName + "' (errno " +
                     std::to_string(errno) + ") — source '" + label +
                     "' can receive no frames");
            return false;
        }

        if (ftruncate(fd, (off_t)feeds_shared::REGION_SIZE) < 0) {
            LogError("Video: ftruncate failed for '" + regionName + "' (errno " +
                     std::to_string(errno) + ")");
            close(fd);
            shm_unlink(regionName.c_str());
            return false;
        }

        void* view = mmap(nullptr, feeds_shared::REGION_SIZE,
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (view == MAP_FAILED) {
            LogError("Video: mmap failed for '" + regionName + "' (errno " +
                     std::to_string(errno) + ")");
            close(fd);
            shm_unlink(regionName.c_str());
            return false;
        }

        m_fd     = fd;
        m_view   = view;
        m_header = (feeds_shared::SharedFrameHeader*)view;
        m_slots  = (feeds_shared::FrameSlot*)
            ((uint8_t*)view + sizeof(feeds_shared::SharedFrameHeader));

        // The slots stay uninitialised — the plugin reads a slot only once
        // write_index has advanced past it — but the header must be right
        // before the plugin can be told the region exists.
        m_header->magic           = feeds_shared::REGION_MAGIC;
        m_header->version         = feeds_shared::REGION_VERSION;
        m_header->write_index     = 0;
        m_header->last_read_index = 0;

        LogInfo("Video: opened shared memory '" + regionName + "' for source '" +
                label + "' (" + std::to_string(feeds_shared::REGION_SIZE) +
                " bytes)");
        return true;
    }

    // Safe to call from anywhere, and safe to call twice. Blocks for at most
    // one in-flight frame copy.
    void Close()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_view) {
            munmap(m_view, feeds_shared::REGION_SIZE);
            m_view = nullptr;
        }
        if (m_fd >= 0) {
            close(m_fd);
            m_fd = -1;
        }
        if (!m_name.empty()) {
            // The engine creates these regions, so the engine unlinks them. A
            // plugin still holding a mapping keeps reading valid memory until
            // it closes; unlink removes the NAME, not the object.
            shm_unlink(m_name.c_str());
            m_name.clear();
        }
        m_header = nullptr;
        m_slots  = nullptr;
    }

    // Copy one I420 frame into the ring. Called on the SDK's frame-callback
    // thread.
    //
    // srcW/srcH are what the SDK delivered; the frame is CROPPED to even
    // dimensions before it is written. That is not cosmetic. The slot's payload
    // is laid out as width*height + two (width/2)*(height/2) planes, which only
    // describes an I420 frame whose dimensions are both even — for an odd width
    // the real chroma planes are ceil(w/2) wide, and the reader's stride
    // arithmetic would walk off by a column per row. Camera video is always
    // even so this is a no-op there, but a shared WINDOW is whatever size the
    // window happens to be, and on macOS that is routinely odd. Dropping those
    // frames as invalid would leave screenshare permanently black; cropping one
    // row and column is imperceptible.
    void WriteFrame(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                    uint32_t srcW, uint32_t srcH)
    {
        if (!y || !u || !v || srcW == 0 || srcH == 0) return;

        const uint32_t w = srcW & ~1u;
        const uint32_t h = srcH & ~1u;
        if (w == 0 || h == 0) return;
        if (w > feeds_shared::MAX_FRAME_WIDTH ||
            h > feeds_shared::MAX_FRAME_HEIGHT) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_header || !m_slots) return;

        const uint32_t slot = m_header->write_index % feeds_shared::RING_SLOTS;
        feeds_shared::FrameSlot* dest = &m_slots[slot];

        dest->width    = w;
        dest->height   = h;
        dest->stride_y = w;
        dest->stride_u = w / 2;
        dest->stride_v = w / 2;
        dest->timestamp_ns = (uint64_t)
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

        const size_t ySize = (size_t)w * h;
        const size_t cW    = w / 2;
        const size_t cH    = h / 2;
        const size_t cSize = cW * cH;

        // Source strides. The macOS SDK exposes no stride accessors on
        // ZoomSDKYUVRawDataI420 (unlike its sender-side sibling), so the planes
        // are taken as tightly packed at the DELIVERED width — which is why the
        // crop above must not change what we read, only what we write.
        const size_t srcYStride = srcW;
        const size_t srcCStride = (srcW + 1) / 2;

        uint8_t* dy = dest->data;
        uint8_t* du = dy + ySize;
        uint8_t* dv = du + cSize;

        if (srcYStride == w) {
            memcpy(dy, y, ySize);
        } else {
            for (uint32_t r = 0; r < h; ++r)
                memcpy(dy + (size_t)r * w, y + (size_t)r * srcYStride, w);
        }
        if (srcCStride == cW) {
            memcpy(du, u, cSize);
            memcpy(dv, v, cSize);
        } else {
            for (uint32_t r = 0; r < cH; ++r) {
                memcpy(du + (size_t)r * cW, u + (size_t)r * srcCStride, cW);
                memcpy(dv + (size_t)r * cW, v + (size_t)r * srcCStride, cW);
            }
        }

        // Publish the slot before the index that makes it visible.
        std::atomic_thread_fence(std::memory_order_release);
        m_header->write_index++;
    }

    // A zero-dimension slot is the protocol's "clear the source" sentinel: the
    // plugin blanks its OBS source rather than freezing on the last frame.
    void WriteBlankSignal()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_header || !m_slots) return;

        const uint32_t slot = m_header->write_index % feeds_shared::RING_SLOTS;
        feeds_shared::FrameSlot* dest = &m_slots[slot];

        dest->width    = 0;
        dest->height   = 0;
        dest->stride_y = 0;
        dest->stride_u = 0;
        dest->stride_v = 0;
        dest->timestamp_ns = (uint64_t)
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

        std::atomic_thread_fence(std::memory_order_release);
        m_header->write_index++;
    }

private:
    std::mutex  m_mutex;
    std::string m_name;
    int         m_fd   = -1;
    void*       m_view = nullptr;
    feeds_shared::SharedFrameHeader* m_header = nullptr;
    feeds_shared::FrameSlot*         m_slots  = nullptr;
};

// ---------------------------------------------------------------------------
// FrameSink — everything the SDK's frame-callback thread is allowed to touch.
//
// Held by the renderer's delegate through a shared_ptr, so a callback that is
// already in flight when a subscription is torn down still has a live object to
// write into: the writer will simply have been closed and the write becomes a
// no-op. Nothing here reaches the registry.
// ---------------------------------------------------------------------------
struct FrameSink {
    MacFrameWriter    writer;
    std::string       label;                 // source uuid, or "screenshare"
    std::atomic<bool> gotFirstFrame{false};

    // Touched only from the frame callback. Frames for one renderer are
    // delivered in order, so these need no synchronisation; they exist to keep
    // the log to one line per real change rather than one per frame.
    int          lastW          = 0;
    int          lastH          = 0;
    unsigned int loggedFailures = 0;
    int          requestedHeight = 0;
};

enum : unsigned int {
    kFailNullPlane = 1u << 0,
    kFailZeroDim   = 1u << 1,
};

// Posted to the main queue the first time a renderer delivers, so the gate can
// release the next same-user request promptly instead of waiting out its
// timeout. Defined below; declared here because the frame path calls it.
void NotifyFirstFrame();

void SinkOnFrame(const std::shared_ptr<FrameSink>& sink,
                 ZoomSDKYUVRawDataI420* data)
{
    if (!sink || !data) return;

    const int w = (int)[data getStreamWidth];
    const int h = (int)[data getStreamHeight];
    const uint8_t* y = (const uint8_t*)[data getYBuffer];
    const uint8_t* u = (const uint8_t*)[data getUBuffer];
    const uint8_t* v = (const uint8_t*)[data getVBuffer];

    if (w <= 0 || h <= 0) {
        if (!(sink->loggedFailures & kFailZeroDim)) {
            sink->loggedFailures |= kFailZeroDim;
            LogWarn("Video: source '" + sink->label + "' received a frame with "
                    "a zero dimension (" + std::to_string(w) + "x" +
                    std::to_string(h) + "); dropping (logged once)");
        }
        return;
    }
    if (!y || !u || !v) {
        if (!(sink->loggedFailures & kFailNullPlane)) {
            sink->loggedFailures |= kFailNullPlane;
            LogWarn("Video: source '" + sink->label + "' received a frame with "
                    "a null plane buffer; dropping (logged once)");
        }
        return;
    }

    // The one undocumented unknown in the feasibility plan was which thread the
    // SDK delivers frames on, because everything else it does arrives on the
    // main run loop. Answer it in the log, once per subscription, rather than
    // leaving the next person to guess: the shared-memory write has to be safe
    // from whatever this thread is, and it is (the writer carries its own
    // mutex), but nobody should have to re-derive that.
    if (!sink->gotFirstFrame.exchange(true, std::memory_order_acq_rel)) {
        char tname[64] = {0};
        pthread_getname_np(pthread_self(), tname, sizeof(tname));
        LogInfo("Video: source '" + sink->label + "' FIRST FRAME " +
                std::to_string(w) + "x" + std::to_string(h) + " (requested " +
                std::to_string(sink->requestedHeight) + "p) — delivered on the " +
                ([NSThread isMainThread] ? std::string("MAIN thread")
                                         : std::string("SDK callback thread")) +
                " '" + (tname[0] ? tname : "unnamed") + "'");
        NotifyFirstFrame();
        sink->lastW = w;
        sink->lastH = h;
    } else if (w != sink->lastW || h != sink->lastH) {
        LogInfo("Video: source '" + sink->label + "' frame dimensions changed " +
                std::to_string(sink->lastW) + "x" + std::to_string(sink->lastH) +
                " -> " + std::to_string(w) + "x" + std::to_string(h));
        sink->lastW = w;
        sink->lastH = h;
    }

    // Copy now, inside the callback. The frame is freed the moment this returns
    // unless it was addRef'd, and in heap memory mode a stashed pointer is a
    // use-after-free rather than a stale read.
    sink->writer.WriteFrame(y, u, v, (uint32_t)w, (uint32_t)h);
}

}  // namespace

// ---------------------------------------------------------------------------
// The renderer delegate.
//
// One instance per renderer, holding only a shared_ptr to its sink. It
// deliberately knows nothing about the subscription registry, so nothing it can
// be called on can contend with the main thread's teardown path.
// ---------------------------------------------------------------------------
@interface FeedsRendererDelegate : NSObject <ZoomSDKRendererDelegate>
- (instancetype)initWithSink:(std::shared_ptr<FrameSink>)sink;
@end

@implementation FeedsRendererDelegate {
    std::shared_ptr<FrameSink> _sink;
}

- (instancetype)initWithSink:(std::shared_ptr<FrameSink>)sink
{
    if ((self = [super init])) _sink = std::move(sink);
    return self;
}

- (void)onRawDataReceived:(ZoomSDKYUVRawDataI420*)data
{
    SinkOnFrame(_sink, data);
}

- (void)onSubscribedUserDataOn
{
    if (_sink)
        LogInfo("Video: source '" + _sink->label + "' raw data ON");
}

- (void)onSubscribedUserDataOff
{
    // Frames have stopped (camera off, network drop, the sharer paused). Push
    // the blank sentinel so the plugin clears its OBS source instead of
    // freezing on the last frame; recovery is automatic when frames resume.
    if (!_sink) return;
    LogInfo("Video: source '" + _sink->label + "' raw data OFF — blanking");
    _sink->writer.WriteBlankSignal();
}

- (void)onSubscribedUserLeft {}

- (void)onRendererBeDestroyed
{
    // Reached only when the SDK destroys a renderer on its own initiative — our
    // own teardown clears this delegate before calling destroyRender precisely
    // so it does NOT arrive re-entrantly from inside that call. Touch only the
    // sink: taking any lock here is what the ordering in TearDownSubscription
    // exists to make unnecessary.
    if (!_sink) return;
    LogInfo("Video: the SDK destroyed the renderer for source '" +
            _sink->label + "'");
    _sink->writer.WriteBlankSignal();
}

@end

namespace {

// ---------------------------------------------------------------------------
// Subscription registry — MAIN QUEUE ONLY, and therefore unlocked.
// ---------------------------------------------------------------------------
struct MacSubscription {
    std::string  sourceUuid;
    unsigned int userId              = 0;
    bool         followActiveSpeaker = false;
    int          rung                = 0;
    uint64_t     subscribeTick       = 0;   // 0 = not awaiting a first frame

    ZoomSDKRenderer*           renderer = nil;
    FeedsRendererDelegate*     delegate = nil;   // strong: the SDK's is `assign`
    std::shared_ptr<FrameSink> sink;
};

std::map<std::string, std::unique_ptr<MacSubscription>> g_subs;

// ── Resolution ladder ───────────────────────────────────────────────────────
// Zoom's raw-data subscription budget is a whole-MEETING resource: roughly one
// Full HD subscription per meeting, two at HD, and no mixing of the two. Asking
// for more than the meeting can carry gets the request refused outright, so a
// subscription starts at the top and steps DOWN a rung each time an attempt is
// provably refused, never up.
//
// The step-down is a REQUEUE through the gate, not an inline retry. Recreating
// a renderer for a participant while the SDK is still asynchronously releasing
// the previous one is exactly the race the gate exists to avoid, and an inline
// ladder walk would reintroduce it one rung at a time.
//
// Unlike Windows this does not start from the licence tier, because the macOS
// engine has no tier plumbing yet: every source asks for 1080p first and lets
// the refusals settle it. On an account with no Full HD entitlement the first
// rung simply succeeds at whatever the SDK negotiates, exactly as 720p would
// have, and nothing below ever runs.
enum : int { kLadderRungs = 3 };
const ZoomSDKResolution kLadderRes[kLadderRungs] = {
    ZoomSDKResolution_1080P,
    ZoomSDKResolution_720P,
    ZoomSDKResolution_360P,
};
const int kLadderHeight[kLadderRungs] = { 1080, 720, 360 };

const char* LadderRungName(int rung)
{
    if (rung < 0 || rung >= kLadderRungs) return "?";
    return (rung == 0) ? "1080p" : (rung == 1) ? "720p" : "360p";
}

// ── The gate ────────────────────────────────────────────────────────────────
// Renderer creation is never inline. Two things have to be true first, and
// neither is observable at the moment the plugin asks:
//
//   * The raw-data renderer subsystem must be up. createRenderer before that
//     fails transiently, and the reliable signal is our own user appearing in
//     the SDK's raw-live-streaming list (VideoNotifyRawRenderReady).
//   * No OTHER renderer for the SAME user id may still be establishing. A burst
//     of creates for one user makes the SDK establish delivery for exactly one
//     of them and silently drop the rest — createRenderer succeeds, frames
//     never arrive. So a same-user sibling waits for the in-flight renderer's
//     first frame, or for a generous timeout. Different user ids never wait on
//     each other.
struct PendingRender {
    std::string  sourceId;
    unsigned int userId;
    bool         follow;
    uint64_t     deadlineMs;
    int          rung;
    // A queued entry is retried every tick until its deadline, so the reason it
    // could not be built yet must be logged ONCE, not fifty times over fifteen
    // seconds.
    bool         loggedNotReady = false;
};

struct InflightEstablish {
    std::string sourceId;
    uint64_t    createMs;
};

std::vector<PendingRender>                 g_pending;
std::map<unsigned int, InflightEstablish>  g_inflightByUser;
bool                                       g_rawStreamActive = false;
bool                                       g_rawRenderReady  = false;
bool                                       g_inMeeting       = false;

const uint64_t kGateTickMs          = 300;    // drain / retry cadence
const uint64_t kGiveUpMs            = 15000;  // never-created deadline
const uint64_t kEstablishTimeoutMs  = 2000;   // created-but-not-delivering clock
const uint64_t kSpeakerHeartbeatMs  = 5000;   // re-announce cadence

dispatch_source_t g_tickTimer = nil;
uint64_t          g_lastHeartbeatMs = 0;

// ── Active speaker ──────────────────────────────────────────────────────────
// The stored value is the INPUT (what the SDK last reported), never a decision.
// The on-screen target is DERIVED fresh on every evaluation, so a speaker who
// is not displayable now is simply reconsidered next time instead of being
// thrown away. Same model as engine-speaker.cpp, minus its generation guard:
// there is no second thread to race here, because everything runs on the main
// queue.
unsigned int g_rawSpeakerId      = 0;
unsigned int g_resolvedSpeakerId = 0;
unsigned int g_loggedRaw         = 0;
unsigned int g_loggedResolved    = 0;
const char*  g_loggedDecision    = "";

// ── Screenshare ─────────────────────────────────────────────────────────────
// One renderer for whatever share is currently viewable, not one per source:
// the plugin may hold several screenshare sources and they all map the one
// well-known region. The share source id is the SDK's, changes whenever someone
// starts, stops or switches a share, and is NOT a user id.
ZoomSDKRenderer*           g_shareRenderer   = nil;
FeedsRendererDelegate*     g_shareDelegate   = nil;
std::shared_ptr<FrameSink> g_shareSink;
unsigned int               g_shareSourceId   = 0;
unsigned int               g_sharerUserId    = 0;

void ProcessPendingRenderers();
void EvaluateActiveSpeaker(bool heartbeat);

// ---------------------------------------------------------------------------
// Renderer construction and teardown
// ---------------------------------------------------------------------------

// Tear a renderer down in the one order that is safe.
//
//   1. Clear renderer.delegate. destroyRender invokes onRendererBeDestroyed
//      SYNCHRONOUSLY on this thread; leaving the delegate attached means that
//      callback runs re-entrantly in the middle of our teardown. Clearing it
//      first removes the re-entry entirely.
//   2. unSubscribe, then destroyRender. After destroyRender returns the SDK
//      can no longer deliver a frame to this renderer.
//   3. Close the writer LAST. Any frame callback still in flight when we
//      started holds the writer's mutex, so Close waits for it — which is the
//      right way round, and is only ever one frame's copy.
void TearDownRenderer(ZoomSDKRenderer* __strong* renderer,
                      FeedsRendererDelegate* __strong* delegate,
                      const std::shared_ptr<FrameSink>& sink)
{
    if (renderer && *renderer) {
        [*renderer setDelegate:nil];
        [*renderer unSubscribe];
        ZoomSDKRawDataController* rdc = RawDataController();
        if (rdc) [rdc destroyRender:*renderer];
        *renderer = nil;
    }
    if (delegate) *delegate = nil;
    if (sink) sink->writer.Close();
}

void TearDownSubscription(const std::string& sourceId)
{
    auto it = g_subs.find(sourceId);
    if (it == g_subs.end()) return;

    MacSubscription* sub = it->second.get();
    TearDownRenderer(&sub->renderer, &sub->delegate, sub->sink);

    // Whatever this source was establishing, it no longer is.
    for (auto gi = g_inflightByUser.begin(); gi != g_inflightByUser.end(); ) {
        if (gi->second.sourceId == sourceId) gi = g_inflightByUser.erase(gi);
        else                                 ++gi;
    }

    g_subs.erase(it);
}

void SendTextureReady(const std::string& sourceId)
{
    char resp[512];
    std::snprintf(resp, sizeof(resp),
        "{\"type\":\"source_texture_ready\",\"source_id\":\"%s\",\"pid\":%u,"
        "\"width\":%u,\"height\":%u}",
        sourceId.c_str(), (unsigned)getpid(),
        feeds_shared::MAX_FRAME_WIDTH, feeds_shared::MAX_FRAME_HEIGHT);
    Send(resp);
}

void SendSubscribeFailed(const std::string& sourceId)
{
    // Tell the plugin once, so its subscribed-state guard cannot pin the source
    // black forever; a later grant or a manual reselect can retry.
    Send("{\"type\":\"participant_source_subscribe_failed\",\"source_id\":\"" +
         feeds::JsonEscape(sourceId) + "\"}");
}

// Queue (or re-queue) a create request. Replaces any pending request for the
// same source, keeping the LOWER rung of the two so a queued step-down cannot
// be undone by a plain re-request arriving behind it.
void EnqueuePendingRender(const std::string& sourceId, unsigned int userId,
                          bool follow, int rung)
{
    if (rung < 0) rung = 0;
    if (rung >= kLadderRungs) rung = kLadderRungs - 1;

    for (auto& p : g_pending) {
        if (p.sourceId == sourceId) {
            p.userId         = userId;
            p.follow         = follow;
            p.deadlineMs     = NowMs() + kGiveUpMs;
            p.loggedNotReady = false;   // a fresh request may state its reason
            if (rung > p.rung) p.rung = rung;
            return;
        }
    }
    g_pending.push_back({sourceId, userId, follow, NowMs() + kGiveUpMs, rung,
                         false});
}

// What one attempt at building a subscription came to. Same four outcomes the
// Windows engine distinguishes, because the same four things can happen:
//
//   Started     the renderer exists and is subscribed (or deliberately waiting
//               for an active speaker).
//   RetryLater  transient: the raw-data subsystem is not up yet. Stay queued
//               and let the deadline, not this attempt, decide when to give up.
//   StepDown    the SDK REFUSED the resolution or the subscription. A lower
//               rung might survive it, so ladder down and requeue.
//   Failed      nothing a retry or a smaller frame can fix (the shared-memory
//               region would not open). Tell the plugin and drop the request.
enum class SubStart { Started, RetryLater, StepDown, Failed };

// Build one subscription: region, renderer, resolution, subscribe.
// `logNotReady` is cleared once the caller has reported a RetryLater, so a
// source waiting out its deadline logs its reason once rather than per tick.
SubStart StartSubscription(const std::string& sourceId, unsigned int userId,
                           bool follow, int rung, bool& logNotReady)
{
    ZoomSDKRawDataController* rdc = RawDataController();
    if (!rdc) {
        if (logNotReady) {
            logNotReady = false;
            LogWarn("Video: raw data controller unavailable; source '" +
                    sourceId + "' is queued until it appears");
        }
        return SubStart::RetryLater;
    }

    auto sub = std::make_unique<MacSubscription>();
    sub->sourceUuid          = sourceId;
    sub->userId              = userId;
    sub->followActiveSpeaker = follow;
    sub->rung                = rung;
    sub->sink                = std::make_shared<FrameSink>();
    sub->sink->label           = sourceId;
    sub->sink->requestedHeight = kLadderHeight[rung];

    // The region first, so it is ready before any frame can arrive.
    const std::string regionName =
        feeds_shared::MakeFrameRegionName((uint32_t)getpid(), sourceId);
    if (!sub->sink->writer.Open(regionName, sourceId)) return SubStart::Failed;

    ZoomSDKRenderer* renderer = nil;
    const ZoomSDKError createErr = [rdc createRender:&renderer];
    if (createErr != ZoomSDKError_Success || !renderer) {
        // Not a ladder case: a renderer that cannot be created at all is not
        // going to be created at a smaller size. It is the transient
        // subsystem-not-ready case, which the gate already exists to wait out.
        if (logNotReady) {
            logNotReady = false;
            LogWarn("Video: createRender failed for source '" + sourceId +
                    "' (code " + std::to_string((int)createErr) +
                    "); staying queued until the raw-data subsystem is ready");
        }
        sub->sink->writer.Close();
        return SubStart::RetryLater;
    }

    FeedsRendererDelegate* delegate =
        [[FeedsRendererDelegate alloc] initWithSink:sub->sink];
    renderer.delegate = delegate;
    sub->renderer     = renderer;
    sub->delegate     = delegate;

    const ZoomSDKError resErr = [renderer setResolution:kLadderRes[rung]];
    if (resErr != ZoomSDKError_Success) {
        // An unaccepted request is not negotiated quality: the SDK did not take
        // the setting at all. Step down rather than subscribe a renderer whose
        // resolution is unknown.
        LogWarn("Video: source '" + sourceId + "' setResolution(" +
                LadderRungName(rung) + ") REFUSED (code " +
                std::to_string((int)resErr) + ") — stepping down");
        TearDownRenderer(&sub->renderer, &sub->delegate, sub->sink);
        return SubStart::StepDown;
    }

    // A follow-active-speaker source with no displayable speaker yet stays
    // deliberately unsubscribed: passing the sentinel would just be rejected as
    // an invalid user. RetargetFollowSources points it at a real id the moment
    // one resolves.
    const bool waitingForSpeaker = follow && userId == kActiveSpeakerSentinel;
    if (!waitingForSpeaker) {
        const ZoomSDKError subErr =
            [renderer subscribe:userId rawDataType:ZoomSDKRawDataType_Video];
        if (subErr != ZoomSDKError_Success) {
            LogWarn("Video: LADDER source='" + sourceId + "' userId=" +
                    std::to_string(userId) + " subscribe REFUSED at " +
                    LadderRungName(rung) + " (code " +
                    std::to_string((int)subErr) +
                    ") — over the meeting's resolution budget, or the user is "
                    "not subscribable");
            TearDownRenderer(&sub->renderer, &sub->delegate, sub->sink);
            return SubStart::StepDown;
        }
        sub->subscribeTick = NowMs();
        LogInfo("Video: subscribed source='" + sourceId + "' to userId=" +
                std::to_string(userId) + " at " + LadderRungName(rung) +
                (follow ? " [follow-speaker]" : ""));
    } else {
        LogInfo("Video: source='" + sourceId + "' waiting for an active speaker");
    }

    g_subs[sourceId] = std::move(sub);
    return SubStart::Started;
}

// Re-point a live subscription at a different user without rebuilding the
// renderer or the region. Returns false when the SDK refused the re-point, in
// which case the caller steps the source down a rung and requeues.
bool Resubscribe(MacSubscription* sub, unsigned int newUserId)
{
    if (!sub || !sub->renderer) return true;

    [sub->renderer unSubscribe];
    sub->userId = newUserId;

    if (sub->followActiveSpeaker && newUserId == kActiveSpeakerSentinel) {
        // Unsubscribed by choice: stop the establishment clock so the watchdog
        // does not read this as a missing first frame.
        sub->subscribeTick = 0;
        sub->sink->writer.WriteBlankSignal();
        LogInfo("Video: source='" + sub->sourceUuid +
                "' waiting for an active speaker");
        return true;
    }

    const ZoomSDKError err =
        [sub->renderer subscribe:newUserId rawDataType:ZoomSDKRawDataType_Video];
    if (err != ZoomSDKError_Success) {
        LogWarn("Video: LADDER source='" + sub->sourceUuid + "' userId=" +
                std::to_string(newUserId) + " re-point REFUSED at " +
                LadderRungName(sub->rung) + " (code " +
                std::to_string((int)err) + ")");
        sub->subscribeTick = 0;
        return false;
    }

    // Fresh target, fresh establishment window.
    sub->sink->gotFirstFrame.store(false, std::memory_order_release);
    sub->subscribeTick = NowMs();
    LogInfo("Video: resubscribed source='" + sub->sourceUuid + "' to userId=" +
            std::to_string(newUserId) + " at " + LadderRungName(sub->rung) +
            (sub->followActiveSpeaker ? " [follow-speaker]" : ""));
    return true;
}

// Step one source down the ladder and requeue it, or retire it at the bottom.
void LadderStepDown(const std::string& sourceId, unsigned int userId,
                    bool follow, int currentRung, const char* reason)
{
    const int next = currentRung + 1;
    if (next >= kLadderRungs) {
        LogWarn("Video: LADDER source='" + sourceId + "' userId=" +
                std::to_string(userId) + " EXHAUSTED at " +
                LadderRungName(currentRung) + " (reason=" + reason +
                ") — no lower rung; notifying the plugin");
        SendSubscribeFailed(sourceId);
        return;
    }
    LogInfo("Video: LADDER source='" + sourceId + "' userId=" +
            std::to_string(userId) + " stepping down " +
            LadderRungName(currentRung) + " -> " + LadderRungName(next) +
            " (reason=" + reason + ")");
    EnqueuePendingRender(sourceId, userId, follow, next);
}

// ---------------------------------------------------------------------------
// Active speaker
// ---------------------------------------------------------------------------

unsigned int ResolvedActiveSpeakerTarget()
{
    return g_resolvedSpeakerId != 0 ? g_resolvedSpeakerId
                                    : kActiveSpeakerSentinel;
}

// Point every follow-speaker source at the resolved target. LEVEL-triggered:
// it re-points anything not already on the target rather than acting only on a
// change, so a source that missed an edge cannot stay stuck on an old speaker.
void RetargetFollowSources(unsigned int target, int& outFollow, int& outMoved,
                           std::string& outBound)
{
    outFollow = 0;
    outMoved  = 0;
    outBound.clear();

    // Collect step-downs and apply them after the loop: LadderStepDown queues
    // work that can rebuild a subscription, and mutating the map underneath
    // this walk would invalidate it.
    std::vector<std::tuple<std::string, unsigned int, int>> stepDowns;

    for (auto& kv : g_subs) {
        MacSubscription* s = kv.second.get();
        if (!s || !s->followActiveSpeaker) continue;
        ++outFollow;

        if (s->userId != target) {
            ++outMoved;
            if (!Resubscribe(s, target))
                stepDowns.emplace_back(kv.first, target, s->rung);
        }
        if (!outBound.empty()) outBound += ",";
        outBound += kv.first + "=" + std::to_string(s->userId);
    }

    for (const auto& sd : stepDowns) {
        LadderStepDown(std::get<0>(sd), std::get<1>(sd), true, std::get<2>(sd),
                       "repoint_refused");
    }
}

void EvaluateActiveSpeaker(bool heartbeat)
{
    if (!g_inMeeting) return;

    const unsigned int me   = MyUserId();
    const unsigned int raw  = g_rawSpeakerId;
    unsigned int       held = g_resolvedSpeakerId;
    const char*        decision = "";

    // The Feeds user is never a target: OBS is very likely feeding them back
    // into Zoom as a virtual camera, so subscribing would loop. That applies to
    // a held target accepted before our own id was known, too.
    if (me != 0 && held == me) held = 0;

    unsigned int resolved;
    if (raw == 0)                   { decision = "no_speaker_yet"; resolved = held; }
    else if (me != 0 && raw == me)  { decision = "hold_self";      resolved = held; }
    else if (UserCameraOff(raw))    { decision = "hold_video_off"; resolved = held; }
    else                            { decision = "accept";         resolved = raw;  }

    const unsigned int previous = g_resolvedSpeakerId;
    g_resolvedSpeakerId = resolved;

    if (raw != g_loggedRaw || resolved != g_loggedResolved ||
        std::strcmp(decision, g_loggedDecision) != 0) {
        LogInfo("Speaker: state raw=" + std::to_string(raw) + " resolved=" +
                std::to_string(resolved) + " decision=" + decision +
                " (was raw=" + std::to_string(g_loggedRaw) + " resolved=" +
                std::to_string(g_loggedResolved) + ")");
        g_loggedRaw      = raw;
        g_loggedResolved = resolved;
        g_loggedDecision = decision;
    }

    int follow = 0, moved = 0;
    std::string bound;
    const unsigned int target = ResolvedActiveSpeakerTarget();
    RetargetFollowSources(target, follow, moved, bound);
    if (moved > 0) {
        LogInfo("Speaker: target userId=" + std::to_string(target) +
                " — re-pointed " + std::to_string(moved) + " of " +
                std::to_string(follow) + " follow source(s)");
    }

    // The plugin mirrors the RESOLVED target — the person a follow source is
    // actually showing, which is who its dock row and nameplate must name. Sent
    // on change, and re-sent on every heartbeat so the mirror cannot go stale.
    if (resolved != previous || heartbeat) {
        Send("{\"type\":\"active_speaker_changed\",\"participant_id\":" +
             std::to_string(resolved) + "}");
    }

    if (heartbeat && follow > 0) {
        LogInfo("Speaker: heartbeat raw=" + std::to_string(raw) + " resolved=" +
                std::to_string(resolved) + " decision=" + decision + " me=" +
                std::to_string(me) + " follow_sources=" +
                std::to_string(follow) + " bound=[" + bound + "]");
    }
}

// ---------------------------------------------------------------------------
// The gate drain — main queue, on the tick and on demand
// ---------------------------------------------------------------------------
void ProcessPendingRenderers()
{
    const uint64_t now = NowMs();

    for (size_t i = 0; i < g_pending.size(); ) {
        const std::string sourceId = g_pending[i].sourceId;
        const bool        follow   = g_pending[i].follow;
        const uint64_t    deadline = g_pending[i].deadlineMs;
        const int         rung     = g_pending[i].rung;

        // A follow request is ALWAYS re-resolved at drain time, not only when
        // it was queued as the sentinel: the id captured at enqueue can be
        // several speakers old by the time the gate lets it through, and
        // building on it would undo a newer retarget.
        unsigned int uid = follow ? ResolvedActiveSpeakerTarget()
                                  : g_pending[i].userId;

        if (now > deadline) {
            LogWarn("Video: source '" + sourceId + "' gave up waiting for the "
                    "raw-data renderer subsystem — notifying the plugin");
            SendSubscribeFailed(sourceId);
            g_pending.erase(g_pending.begin() + (long)i);
            continue;
        }

        if (!g_rawStreamActive || !g_rawRenderReady) { ++i; continue; }

        // Per-user delivery gate.
        auto inf = g_inflightByUser.find(uid);
        if (inf != g_inflightByUser.end()) {
            bool free = false;
            auto sit = g_subs.find(inf->second.sourceId);
            if (sit == g_subs.end() || !sit->second) {
                free = true;                       // that renderer is gone
            } else if (sit->second->sink &&
                       sit->second->sink->gotFirstFrame.load(
                           std::memory_order_acquire)) {
                free = true;                       // delivery confirmed
            } else if (now - inf->second.createMs >= kEstablishTimeoutMs) {
                free = true;                       // generous timeout elapsed
            }
            if (!free) { ++i; continue; }          // sibling waits, stays queued
            g_inflightByUser.erase(inf);
        }

        // A drained request always produces a FRESH renderer: a kept one is
        // stale across a participant's rejoin. Destroy any existing
        // subscription for this source first, and tell the plugin to close its
        // region so it reopens cleanly on the source_texture_ready below rather
        // than reading a region we are about to unlink.
        const bool hadPrior = g_subs.find(sourceId) != g_subs.end();
        if (hadPrior) {
            TearDownSubscription(sourceId);
            Send("{\"type\":\"source_texture_released\",\"source_id\":\"" +
                 feeds::JsonEscape(sourceId) + "\"}");
        }

        bool logNotReady = !g_pending[i].loggedNotReady;
        const SubStart r =
            StartSubscription(sourceId, uid, follow, rung, logNotReady);

        switch (r) {
        case SubStart::Started:
            g_pending.erase(g_pending.begin() + (long)i);
            g_inflightByUser[uid] = { sourceId, now };
            SendTextureReady(sourceId);
            // Keep scanning: entries for OTHER user ids do not wait on this one.
            continue;

        case SubStart::RetryLater:
            // Leave the entry exactly where it is, at the same rung and — this
            // is the part that matters — with its ORIGINAL deadline. Requeuing
            // would push the deadline out by another kGiveUpMs every attempt
            // and turn the give-up into an infinite retry.
            g_pending[i].loggedNotReady = true;   // reason stated; now stay quiet
            ++i;
            continue;

        case SubStart::StepDown:
            g_pending.erase(g_pending.begin() + (long)i);
            LadderStepDown(sourceId, uid, follow, rung, "subscribe_refused");
            // Stop scanning this tick. The requeued entry was appended to the
            // vector we are walking, and taking it now would recreate this
            // source microseconds after its failed renderer was destroyed —
            // back-to-back against the SDK's asynchronous release, which is the
            // race this whole gate routes around. The next tick picks it up.
            return;

        case SubStart::Failed:
            LogError("Video: source '" + sourceId + "' could not be given a "
                     "shared-memory region; giving up and notifying the plugin");
            SendSubscribeFailed(sourceId);
            g_pending.erase(g_pending.begin() + (long)i);
            continue;
        }
    }
}

// ---------------------------------------------------------------------------
// Watchdog: subscribed, camera on, and no first frame ever arrived.
//
// The SDK can accept a subscription and simply never deliver it, and there is
// no callback for that — the only detectable signal is the absence of a first
// frame past a generous deadline.
//
// THE CAMERA CHECK IS LOAD-BEARING. A participant with their camera off
// legitimately never sends a frame; without it this sweep would fire on every
// camera-off person in every show and ratchet healthy sources down to 360p.
// Camera-off is already handled by the video-status callback, which re-queues
// the source when the camera comes back. Only a camera-ON source that never
// delivered is a ladder trigger.
// ---------------------------------------------------------------------------
const uint64_t kFirstFrameWatchdogMs = 7000;

void SweepUndeliveredSubscriptions()
{
    const uint64_t now = NowMs();
    std::vector<std::tuple<std::string, unsigned int, bool, int>> stepDowns;

    for (auto& kv : g_subs) {
        MacSubscription* s = kv.second.get();
        if (!s || s->subscribeTick == 0) continue;
        if (s->sink && s->sink->gotFirstFrame.load(std::memory_order_acquire)) {
            s->subscribeTick = 0;               // delivered; stop watching
            continue;
        }
        if (now - s->subscribeTick < kFirstFrameWatchdogMs) continue;

        s->subscribeTick = 0;                   // judged either way
        if (UserCameraOff(s->userId)) continue; // not our case; the camera path owns it

        LogWarn("Video: source='" + kv.first + "' userId=" +
                std::to_string(s->userId) + " subscribed at " +
                LadderRungName(s->rung) + " with the camera on but has "
                "received no frame in " +
                std::to_string(kFirstFrameWatchdogMs / 1000) + "s");
        stepDowns.emplace_back(kv.first, s->userId, s->followActiveSpeaker,
                               s->rung);
    }

    for (const auto& sd : stepDowns) {
        TearDownSubscription(std::get<0>(sd));
        Send("{\"type\":\"source_texture_released\",\"source_id\":\"" +
             feeds::JsonEscape(std::get<0>(sd)) + "\"}");
        LadderStepDown(std::get<0>(sd), std::get<1>(sd), std::get<2>(sd),
                       std::get<3>(sd), "no_first_frame");
    }
}

// ---------------------------------------------------------------------------
// The one main-queue tick that drives the gate, the watchdog and the heartbeat
// ---------------------------------------------------------------------------
void Tick()
{
    ProcessPendingRenderers();
    SweepUndeliveredSubscriptions();

    const uint64_t now = NowMs();
    if (now - g_lastHeartbeatMs >= kSpeakerHeartbeatMs) {
        g_lastHeartbeatMs = now;
        EvaluateActiveSpeaker(true);
    }
}

void StartTick()
{
    if (g_tickTimer) return;
    g_tickTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                         dispatch_get_main_queue());
    dispatch_source_set_timer(g_tickTimer,
                              dispatch_time(DISPATCH_TIME_NOW,
                                            (int64_t)(kGateTickMs * NSEC_PER_MSEC)),
                              kGateTickMs * NSEC_PER_MSEC,
                              50ull * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(g_tickTimer, ^{ Tick(); });
    dispatch_resume(g_tickTimer);
}

void StopTick()
{
    if (!g_tickTimer) return;
    dispatch_source_cancel(g_tickTimer);
    g_tickTimer = nil;
}

void NotifyFirstFrame()
{
    // Called on the SDK's frame-callback thread: hop, never touch the registry
    // from here. The 300 ms tick is the fallback if a hop is ever dropped, so
    // this is an optimisation for how QUICKLY a waiting sibling proceeds, not
    // the only path by which it does.
    dispatch_async(dispatch_get_main_queue(), ^{ ProcessPendingRenderers(); });
}

// ---------------------------------------------------------------------------
// Screenshare
// ---------------------------------------------------------------------------
void ShareTeardown()
{
    TearDownRenderer(&g_shareRenderer, &g_shareDelegate, g_shareSink);
    g_shareSink.reset();
    g_shareSourceId = 0;
}

void ShareSubscribe(unsigned int shareSourceId)
{
    if (g_shareRenderer && g_shareSourceId == shareSourceId) return;

    ZoomSDKRawDataController* rdc = RawDataController();
    if (!rdc) {
        LogError("Share: raw data controller unavailable; screenshare cannot "
                 "be subscribed");
        return;
    }

    // Keep the region across a share switch — the plugin's sources stay mapped
    // to it — but rebuild the renderer, because the share source id it is
    // subscribed to has changed. Delegate cleared before destroyRender for the
    // same reason as TearDownRenderer: destroyRender re-enters
    // onRendererBeDestroyed synchronously on this thread.
    if (g_shareRenderer) {
        g_shareRenderer.delegate = nil;
        [g_shareRenderer unSubscribe];
        [rdc destroyRender:g_shareRenderer];
        g_shareRenderer = nil;
        g_shareDelegate = nil;
    }

    if (!g_shareSink) {
        g_shareSink = std::make_shared<FrameSink>();
        g_shareSink->label = "screenshare";
        const std::string regionName =
            feeds_shared::MakeScreenShareRegionName((uint32_t)getpid());
        if (!g_shareSink->writer.Open(regionName, "screenshare")) {
            g_shareSink.reset();
            return;
        }
    }
    // A new share is a new stream: let the first frame of it be logged (and
    // measured) as one.
    g_shareSink->gotFirstFrame.store(false, std::memory_order_release);

    ZoomSDKRenderer* renderer = nil;
    const ZoomSDKError createErr = [rdc createRender:&renderer];
    if (createErr != ZoomSDKError_Success || !renderer) {
        LogWarn("Share: createRender failed (code " +
                std::to_string((int)createErr) + ")");
        return;
    }

    FeedsRendererDelegate* delegate =
        [[FeedsRendererDelegate alloc] initWithSink:g_shareSink];
    renderer.delegate = delegate;

    // Screenshare is not subject to the participant resolution budget in the
    // same way, and a shared screen's own size is what matters, so it asks for
    // the top rung and takes whatever the SDK negotiates.
    [renderer setResolution:ZoomSDKResolution_1080P];
    g_shareSink->requestedHeight = 1080;

    // NOTE the argument: for a share subscription the SDK's subscribeID is the
    // SHARE SOURCE id, not a user id. Passing the sharer's user id here is a
    // silent no-frames failure rather than an error.
    const ZoomSDKError subErr =
        [renderer subscribe:shareSourceId rawDataType:ZoomSDKRawDataType_Share];
    if (subErr != ZoomSDKError_Success) {
        LogWarn("Share: subscribe(shareSourceID=" +
                std::to_string(shareSourceId) + ") failed (code " +
                std::to_string((int)subErr) + ")");
        renderer.delegate = nil;
        [rdc destroyRender:renderer];
        return;
    }

    g_shareRenderer = renderer;
    g_shareDelegate = delegate;
    g_shareSourceId = shareSourceId;
    LogInfo("Share: subscribed to shareSourceID=" +
            std::to_string(shareSourceId));
}

void ShareUnsubscribe()
{
    if (!g_shareRenderer) return;

    ZoomSDKRawDataController* rdc = RawDataController();
    g_shareRenderer.delegate = nil;
    [g_shareRenderer unSubscribe];
    if (rdc) [rdc destroyRender:g_shareRenderer];
    g_shareRenderer = nil;
    g_shareDelegate = nil;
    g_shareSourceId = 0;

    // Blank rather than close: the plugin's screenshare sources stay mapped to
    // this region for the life of the meeting and would otherwise freeze on the
    // last frame of the share that just ended.
    if (g_shareSink) g_shareSink->writer.WriteBlankSignal();
    LogInfo("Share: unsubscribed (no viewable share)");
}

}  // namespace

// ---------------------------------------------------------------------------
// The share/annotation delegate.
//
// ZoomSDKASControllerDelegate is @optional throughout, so only the two
// share-status methods are implemented here.
// ---------------------------------------------------------------------------
@interface FeedsShareDelegate : NSObject <ZoomSDKASControllerDelegate>
@end

@implementation FeedsShareDelegate

// Both callbacks do the same thing, and deliberately ignore what they were
// handed. Driving the renderer off individual transitions means trusting a
// status enum whose initialisation value (None) is indistinguishable from an
// end, and an id that the SDK's own documentation says may not have settled
// when a share begins. Re-deriving what is viewable from the controller covers
// every transition with one code path, and covers the case that produces no
// transition at all: a share already in progress when we joined.
- (void)onSharingStatusChanged:(ZoomSDKSharingSourceInfo*)shareInfo
{
    feeds_mac::VideoRefreshShareSubscription();
}

- (void)onShareContentChanged:(ZoomSDKSharingSourceInfo*)shareInfo
{
    feeds_mac::VideoRefreshShareSubscription();
}

@end

namespace {
// Strong, process-lifetime, for the same reason the engine's other delegates
// are: the SDK's delegate properties are `assign`, so nothing else would hold
// this object and the first callback would land on a dangling pointer.
FeedsShareDelegate* g_shareControllerDelegate = nil;
}  // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------
namespace feeds_mac {

void VideoStartRawLiveStream()
{
    OnMain(^{
        ZoomSDKMeetingService* svc = MeetingService();
        if (!svc) return;

        ZoomSDKLiveStreamHelper* helper = [svc getLiveStreamHelper];
        if (!helper) {
            LogError("Video: live-stream helper unavailable; raw livestreaming "
                     "cannot be started, so no frames will be delivered");
            return;
        }

        const ZoomSDKError err =
            [helper startRawLiveStreaming:kRawBroadcastUrl
                            broadcastName:kRawBroadcastName];
        if (err != ZoomSDKError_Success) {
            LogError("Video: startRawLiveStreaming FAILED (code " +
                     std::to_string((int)err) + "); the privilege was granted "
                     "but raw-data delivery is not on, so sources will stay "
                     "black");
            return;
        }

        g_rawStreamActive = true;
        LogInfo("Video: raw livestreaming STARTED — raw-data delivery is on");

        // Wire the share listener now that the share controller is usable, then
        // ask it what is already viewable: a share that started before we
        // joined produces no status callback at all.
        ZoomSDKASController* as = [svc getASController];
        if (as) {
            if (!g_shareControllerDelegate)
                g_shareControllerDelegate = [[FeedsShareDelegate alloc] init];
            as.delegate = g_shareControllerDelegate;
            VideoRefreshShareSubscription();
        } else {
            LogWarn("Video: annotation/share controller unavailable; the Zoom "
                    "Screenshare source will not receive frames");
        }

        ProcessPendingRenderers();
    });
}

void VideoNotifyRawRenderReady(unsigned int userId)
{
    OnMain(^{
        if (g_rawRenderReady) return;
        if (userId == 0 || userId != MyUserId()) return;
        g_rawRenderReady = true;
        LogInfo("Video: raw render subsystem READY (our own user is live "
                "streaming) — draining queued subscribes");
        ProcessPendingRenderers();
    });
}

void VideoHandleSubscribe(const std::string& json, bool recreate)
{
    const std::string sourceId = feeds::ExtractJsonString(json, "source_id");
    const unsigned int userId =
        (unsigned int)feeds::ExtractJsonNumber(json, "participant_id");
    if (sourceId.empty() || userId == 0) {
        LogWarn("Video: subscribe received with no source_id or participant_id");
        return;
    }

    OnMain(^{
        const bool follow = (userId == kActiveSpeakerSentinel);

        // A follow subscribe — including reselecting [Active Speaker] on a
        // source that already follows — asks for a fresh derivation first, so
        // the target this request is built on is current.
        if (follow) EvaluateActiveSpeaker(false);

        const unsigned int actualUserId =
            follow ? ResolvedActiveSpeakerTarget() : userId;

        auto it = g_subs.find(sourceId);
        if (!recreate && it != g_subs.end() && it->second) {
            // Cheap in-place re-point, the manual-dropdown case. Carry the
            // follow flag through: Resubscribe deliberately leaves it alone,
            // because the retarget loop also calls it and must not flip it.
            MacSubscription* sub = it->second.get();
            sub->followActiveSpeaker = follow;
            if (!Resubscribe(sub, actualUserId)) {
                const int  rung = sub->rung;
                TearDownSubscription(sourceId);
                Send("{\"type\":\"source_texture_released\",\"source_id\":\"" +
                     feeds::JsonEscape(sourceId) + "\"}");
                LadderStepDown(sourceId, actualUserId, follow, rung,
                               "repoint_refused");
                return;
            }
            SendTextureReady(sourceId);
            return;
        }

        // New source, or a recreate. Always through the gate, at the top rung:
        // a recreate is a fresh start (rejoin, grant, auto-rebind), not a
        // ladder step, so it re-asks for full resolution and ladders down again
        // from there if the budget still cannot carry it.
        EnqueuePendingRender(sourceId, actualUserId, follow, 0);
        ProcessPendingRenderers();
    });
}

void VideoHandleUnsubscribe(const std::string& json)
{
    const std::string sourceId = feeds::ExtractJsonString(json, "source_id");
    if (sourceId.empty()) return;

    OnMain(^{
        // Drop any queued request too, or the gate would rebuild a source the
        // plugin has just told us it no longer wants.
        for (size_t i = 0; i < g_pending.size(); ) {
            if (g_pending[i].sourceId == sourceId)
                g_pending.erase(g_pending.begin() + (long)i);
            else
                ++i;
        }

        if (g_subs.find(sourceId) == g_subs.end()) return;
        TearDownSubscription(sourceId);
        LogInfo("Video: unsubscribed source='" + sourceId + "'");
        Send("{\"type\":\"source_texture_released\",\"source_id\":\"" +
             feeds::JsonEscape(sourceId) + "\"}");
    });
}

void VideoMeetingStarted()
{
    OnMain(^{
        g_inMeeting         = true;
        g_rawStreamActive   = false;
        g_rawRenderReady    = false;
        g_rawSpeakerId      = 0;
        g_resolvedSpeakerId = 0;
        g_lastHeartbeatMs   = NowMs();
        StartTick();
        LogInfo("Video: meeting started — renderer gate and active-speaker "
                "evaluation armed");
    });
}

void VideoMeetingEnded()
{
    OnMain(^{
        StopTick();
        g_inMeeting       = false;
        g_rawStreamActive = false;
        g_rawRenderReady  = false;

        std::vector<std::string> ids;
        ids.reserve(g_subs.size());
        for (const auto& kv : g_subs) ids.push_back(kv.first);
        for (const auto& id : ids) TearDownSubscription(id);

        g_pending.clear();
        g_inflightByUser.clear();
        g_rawSpeakerId      = 0;
        g_resolvedSpeakerId = 0;

        ShareTeardown();
        g_sharerUserId = 0;

        if (!ids.empty())
            LogInfo("Video: meeting ended — tore down " +
                    std::to_string(ids.size()) + " subscription(s)");
    });
}

void VideoOnActiveAudio(unsigned int rawUserId)
{
    if (rawUserId == 0) return;
    OnMain(^{
        g_rawSpeakerId = rawUserId;
        EvaluateActiveSpeaker(false);
    });
}

void VideoOnUserVideoStatusChanged(unsigned int userId, bool videoOn)
{
    OnMain(^{
        // A camera coming back on is the recovery path for a source the
        // watchdog stopped watching, and for one that was subscribed while the
        // camera was off. Re-establish it with a fresh renderer through the
        // gate rather than hoping the existing subscription wakes up.
        if (videoOn) {
            for (auto& kv : g_subs) {
                MacSubscription* s = kv.second.get();
                if (!s || s->userId != userId) continue;
                if (s->sink &&
                    s->sink->gotFirstFrame.load(std::memory_order_acquire))
                    continue;                       // already delivering
                EnqueuePendingRender(kv.first, userId, s->followActiveSpeaker,
                                     s->rung);
            }
            ProcessPendingRenderers();
        }
        // The speaker filter reads camera state, so a change on either edge can
        // change who is displayable.
        EvaluateActiveSpeaker(false);
    });
}

void VideoOnUserLeft(unsigned int userId)
{
    OnMain(^{
        // Zoom does not fire a raw-data-off for a user who LEAVES, only for one
        // whose video stops while they stay, so without this the source would
        // freeze on their last frame. Blank it but keep the subscription: the
        // binding is preserved, so a same-id rejoin resumes on its own.
        int blanked = 0;
        for (auto& kv : g_subs) {
            MacSubscription* s = kv.second.get();
            if (!s || s->userId != userId || !s->sink) continue;
            s->sink->writer.WriteBlankSignal();
            ++blanked;
        }
        if (blanked > 0)
            LogInfo("Video: blanked " + std::to_string(blanked) +
                    " source(s) bound to departed user " +
                    std::to_string(userId));

        if (g_rawSpeakerId == userId) g_rawSpeakerId = 0;
        if (g_resolvedSpeakerId == userId) g_resolvedSpeakerId = 0;
        EvaluateActiveSpeaker(false);
    });
}

void VideoRefreshShareSubscription()
{
    OnMain(^{
        unsigned int srcId  = 0;
        unsigned int sharer = 0;

        ZoomSDKMeetingService* svc = MeetingService();
        ZoomSDKASController*   as  = svc ? [svc getASController] : nil;
        if (as) {
            for (NSNumber* entry in [as getViewableSharingUserList]) {
                if (![entry isKindOfClass:[NSNumber class]]) continue;
                const unsigned int uid = [entry unsignedIntValue];
                NSArray<ZoomSDKSharingSourceInfo*>* sources =
                    [as getSharingSourceInfoList:uid];
                if (sources.count == 0) continue;
                srcId  = sources[0].shareSourceID;
                sharer = uid;
                break;
            }
        }

        if (srcId == 0) {
            if (g_sharerUserId == 0 && g_shareSourceId == 0) return;
            ShareUnsubscribe();
            g_sharerUserId = 0;
            Send("{\"type\":\"share_status_changed\",\"sharer_user_id\":0}");
            return;
        }

        const bool changed =
            (sharer != g_sharerUserId) || (srcId != g_shareSourceId);

        if (g_rawStreamActive) {
            ShareSubscribe(srcId);
        }
        // Otherwise raw delivery is not on yet, so there is nothing to
        // subscribe: VideoStartRawLiveStream calls back here the moment it is.
        // The plugin is still told who is sharing, because that drives the
        // screenshare source's properties text either way.

        g_sharerUserId = sharer;
        if (!changed) return;

        // Sent AFTER the region exists and the renderer is subscribed: this is
        // the message the plugin opens the region on, and an earlier one would
        // just fail and wait for the next.
        Send("{\"type\":\"share_status_changed\",\"sharer_user_id\":" +
             std::to_string(sharer) + "}");
    });
}

}  // namespace feeds_mac
