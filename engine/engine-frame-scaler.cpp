// engine-frame-scaler.cpp — see header for overview.

#include "engine-frame-scaler.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include <libyuv/scale.h>  // libyuv::I420Scale, libyuv::kFilterBilinear

// Logging hook from engine-main.cpp.
extern void LogToFile(const char* msg);

namespace feeds_engine {

// ---------------------------------------------------------------------------
// I420Scaler
// ---------------------------------------------------------------------------

I420Scaler::I420Scaler(int targetW, int targetH)
    // Round down to even — chroma planes are half-resolution and the
    // half-stride arithmetic below assumes even target dimensions.
    : m_targetW(targetW & ~1),
      m_targetH(targetH & ~1)
{}

void I420Scaler::Scale(const uint8_t* srcY, const uint8_t* srcU, const uint8_t* srcV,
                       int srcW, int srcH,
                       uint8_t* dstY, uint8_t* dstU, uint8_t* dstV) const
{
    if (m_targetW <= 0 || m_targetH <= 0) return;
    if (srcW <= 0 || srcH <= 0) return;

    // Round source dims down to even so chroma arithmetic stays exact.
    // The SDK delivers even dimensions in practice; this is defence.
    const int srcWe = srcW & ~1;
    const int srcHe = srcH & ~1;
    if (srcWe <= 0 || srcHe <= 0) return;

    // Aspect-preserving fit: pick the smaller of width-fit / height-fit
    // scale so the content lands wholly inside the target. libyuv
    // doesn't do letterbox/pillarbox itself — it scales source-aspect
    // into whatever dst dimensions you give it — so we keep the content
    // rect math here and ask libyuv to write only into the centered
    // subregion of the larger target buffer.
    const float scale = std::min(
        (float)m_targetW / (float)srcWe,
        (float)m_targetH / (float)srcHe);

    int contentW = (int)((float)srcWe * scale);
    int contentH = (int)((float)srcHe * scale);
    // Even-ness for chroma. Clamp to at least 2 (smallest valid I420).
    contentW &= ~1;
    contentH &= ~1;
    if (contentW < 2) contentW = 2;
    if (contentH < 2) contentH = 2;
    if (contentW > m_targetW) contentW = m_targetW;
    if (contentH > m_targetH) contentH = m_targetH;

    // Center offsets, rounded to even so the chroma subregion lines up.
    const int contentX = ((m_targetW - contentW) / 2) & ~1;
    const int contentY = ((m_targetH - contentH) / 2) & ~1;

    // Fill the full target planes with video black (Y=0, U=V=128 neutral
    // chroma). libyuv overwrites only the content subregion below; the
    // margin keeps the memset'd black as pillarbox / letterbox.
    std::memset(dstY, 0,   (size_t)m_targetW * m_targetH);
    std::memset(dstU, 128, (size_t)(m_targetW / 2) * (m_targetH / 2));
    std::memset(dstV, 128, (size_t)(m_targetW / 2) * (m_targetH / 2));

    // Compute per-plane write offsets into the target buffers. libyuv
    // writes (contentW × contentH) starting at the offset pointer,
    // stepping by the target's stride — so we can write into a
    // subregion of a larger buffer just by adjusting the start address
    // and keeping the stride at the full target row width.
    const int dstStrideY = m_targetW;
    const int dstStrideU = m_targetW / 2;
    const int dstStrideV = m_targetW / 2;
    uint8_t*  dstYRegion = dstY + (size_t)contentY * dstStrideY + contentX;
    uint8_t*  dstURegion = dstU + (size_t)(contentY / 2) * dstStrideU
                                  + (contentX / 2);
    uint8_t*  dstVRegion = dstV + (size_t)(contentY / 2) * dstStrideV
                                  + (contentX / 2);

    libyuv::I420Scale(
        srcY,       srcWe,
        srcU,       srcWe / 2,
        srcV,       srcWe / 2,
        srcWe,      srcHe,
        dstYRegion, dstStrideY,
        dstURegion, dstStrideU,
        dstVRegion, dstStrideV,
        contentW,   contentH,
        libyuv::kFilterBilinear);
}

// ---------------------------------------------------------------------------
// FrameScalerWorker
// ---------------------------------------------------------------------------

FrameScalerWorker::FrameScalerWorker(int                targetW,
                                     int                targetH,
                                     FrameOutputFn      outputFn,
                                     const std::string& subscriptionLabel)
    : m_scaler(targetW, targetH),
      m_outputFn(std::move(outputFn)),
      m_label(subscriptionLabel)
{
    // Pre-size the output buffer so the steady-state hot path doesn't
    // reallocate. Allocations themselves are still fine in the worker
    // (it's not the SDK callback thread), but cheap to avoid them.
    const size_t outY = (size_t)m_scaler.TargetWidth() * m_scaler.TargetHeight();
    const size_t outU = (size_t)(m_scaler.TargetWidth() / 2)
                         * (m_scaler.TargetHeight() / 2);
    m_outputBuf.resize(outY + 2 * outU);
}

FrameScalerWorker::~FrameScalerWorker() {
    Stop();
}

void FrameScalerWorker::Start() {
    if (m_thread.joinable()) return;
    m_thread = std::thread(&FrameScalerWorker::WorkerLoop, this);
}

void FrameScalerWorker::Stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shutdown = true;
    }
    m_cv.notify_one();
    if (m_thread.joinable()) m_thread.join();
}

void FrameScalerWorker::StageFrame(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                                   int srcW, int srcH)
{
    // Tight-packed I420 = Y(w*h) + U(w/2 * h/2) + V(w/2 * h/2).
    const size_t ySize = (size_t)srcW * srcH;
    const size_t uSize = (size_t)(srcW / 2) * (srcH / 2);
    const size_t vSize = uSize;
    const size_t total = ySize + uSize + vSize;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Resize-if-needed then copy. resize() only allocates when growing.
        if (m_stagingBuf.size() < total) m_stagingBuf.resize(total);
        std::memcpy(m_stagingBuf.data(),                 y, ySize);
        std::memcpy(m_stagingBuf.data() + ySize,         u, uSize);
        std::memcpy(m_stagingBuf.data() + ySize + uSize, v, vSize);
        m_stagedW   = srcW;
        m_stagedH   = srcH;
        m_hasStaged = true;
    }
    m_cv.notify_one();
}

void FrameScalerWorker::WorkerLoop() {
    // Local buffer reused across iterations. We swap with the staging
    // buffer under the lock so the lock-held memcpy is a vector swap
    // (constant-time pointer swap) rather than the full I420 payload.
    std::vector<uint8_t> localBuf;
    int                  localW = 0;
    int                  localH = 0;

    char logbuf[256];

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [&]{ return m_hasStaged || m_shutdown; });
            if (m_shutdown) {
                sprintf_s(logbuf, "Video: scaler worker exiting for source='%s'",
                          m_label.c_str());
                LogToFile(logbuf);
                return;
            }
            localBuf.swap(m_stagingBuf);
            localW      = m_stagedW;
            localH      = m_stagedH;
            m_hasStaged = false;
        }

        // Process the frame outside the lock so a concurrent StageFrame
        // can begin writing the next frame immediately.
        const size_t ySize = (size_t)localW * localH;
        const size_t uSize = (size_t)(localW / 2) * (localH / 2);
        const uint8_t* srcY = localBuf.data();
        const uint8_t* srcU = srcY + ySize;
        const uint8_t* srcV = srcU + uSize;

        const int    tgtW   = m_scaler.TargetWidth();
        const int    tgtH   = m_scaler.TargetHeight();
        const size_t outY   = (size_t)tgtW * tgtH;
        const size_t outU   = (size_t)(tgtW / 2) * (tgtH / 2);
        const size_t outTotal = outY + 2 * outU;
        if (m_outputBuf.size() < outTotal) m_outputBuf.resize(outTotal);
        uint8_t* dstY = m_outputBuf.data();
        uint8_t* dstU = dstY + outY;
        uint8_t* dstV = dstU + outU;

        m_scaler.Scale(srcY, srcU, srcV, localW, localH,
                       dstY, dstU, dstV);

        if (m_outputFn) {
            m_outputFn(dstY, dstU, dstV, tgtW, tgtH);
        }
    }
}

}  // namespace feeds_engine
