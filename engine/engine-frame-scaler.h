// engine-frame-scaler.h — fixed-output I420 scaling for the participant
// pipeline.
//
// Two classes:
//
//   I420Scaler          — pure bilinear scaler with aspect-preserving fit.
//                         Scales a variable-size I420 source into a fixed
//                         I420 target, letterboxing/pillarboxing the unused
//                         margin with video black (Y=0, U=V=128).
//
//   FrameScalerWorker   — one worker thread per ParticipantSubscription.
//                         Decouples the SDK's onRawDataFrameReceived
//                         callback from the scaling cost so the SDK
//                         callback stays cheap (validate + copy + signal).
//                         The worker pulls the most recently staged frame
//                         (older staged frames are dropped — newer wins,
//                         lower latency) and hands the scaled output to
//                         an injected callback.
//
// Design choices documented in C:\Dev\Greps\zoom-resolution-research.md
// §11/§12: stay on a per-subscription worker thread to avoid serialising
// scaling cost across multiple concurrent SDK callbacks at Broadcaster
// tier; output dimensions are fixed so OBS sees a constant `get_width` /
// `get_height` and the `obs_source_frame.width` matches.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace feeds_engine {

class I420Scaler {
public:
    // Target dimensions are rounded down to even (I420 chroma constraint).
    // Typical values: 1280×720 (Free tier), 1920×1080 (paid tiers).
    I420Scaler(int targetW, int targetH);

    int TargetWidth()  const { return m_targetW; }
    int TargetHeight() const { return m_targetH; }

    // Scale `src` (tight-packed I420 Y/U/V at srcW×srcH) into `dst` (tight-
    // packed I420 at target dimensions). Source is centered and aspect-fit;
    // pillarbox/letterbox margin is filled with video black. Source
    // dimensions must be > 0; even-ness is rounded down internally so odd
    // source sizes don't crash the chroma path.
    void Scale(const uint8_t* srcY, const uint8_t* srcU, const uint8_t* srcV,
               int srcW, int srcH,
               uint8_t* dstY, uint8_t* dstU, uint8_t* dstV) const;

private:
    int m_targetW;
    int m_targetH;
};

// Output callback: receives a fully-scaled tight-packed I420 frame at the
// worker's target dimensions. Invoked on the worker thread. Callers
// typically forward to a SharedMemoryWriter::WriteFrame.
using FrameOutputFn = std::function<void(const uint8_t* y,
                                         const uint8_t* u,
                                         const uint8_t* v,
                                         int width,
                                         int height)>;

class FrameScalerWorker {
public:
    FrameScalerWorker(int               targetW,
                      int               targetH,
                      FrameOutputFn     outputFn,
                      const std::string& subscriptionLabel);
    ~FrameScalerWorker();

    FrameScalerWorker(const FrameScalerWorker&) = delete;
    FrameScalerWorker& operator=(const FrameScalerWorker&) = delete;

    void Start();
    void Stop();   // signals exit + joins; safe to call multiple times

    // Called from the SDK callback thread. Copies the three planes into a
    // single staging buffer under a brief mutex hold, then wakes the
    // worker. If a previous frame is still staged (worker was busy), it's
    // overwritten — newer frame wins. Caller has already validated that
    // y/u/v are non-null and srcW/srcH are > 0 + even.
    void StageFrame(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                    int srcW, int srcH);

private:
    void WorkerLoop();

    I420Scaler              m_scaler;
    FrameOutputFn           m_outputFn;
    std::string             m_label;

    std::thread             m_thread;
    std::mutex              m_mutex;
    std::condition_variable m_cv;

    // Staging slot (guarded by m_mutex). Single buffer; new frames
    // overwrite older unprocessed ones.
    std::vector<uint8_t>    m_stagingBuf;
    int                     m_stagedW   = 0;
    int                     m_stagedH   = 0;
    bool                    m_hasStaged = false;
    bool                    m_shutdown  = false;

    // Output scratch reused across worker iterations.
    std::vector<uint8_t>    m_outputBuf;
};

}  // namespace feeds_engine
