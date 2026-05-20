// engine-frame-scaler.cpp — see header for overview.

#include "engine-frame-scaler.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

// Logging hook from engine-main.cpp.
extern void LogToFile(const char* msg);

namespace feeds_engine {

namespace {

// Bilinear-scale one tight-packed plane (src is srcW×srcH, stride == srcW)
// into a subregion of dst (dst is dstStride-wide, subregion is dstW×dstH
// at offset (dstX,dstY)). Caller is responsible for filling the rest of
// the destination plane (the letterbox/pillarbox margin) with the
// appropriate neutral value before calling — this function only writes
// the content subregion.
//
// Standard half-pixel-aligned bilinear: output sample at integer (x,y)
// maps to source coord (x+0.5)*srcW/dstW - 0.5, clamped to [0, srcW-1].
// Soft on upscale, fine on small downscales; we're nearly always upscaling
// since the SDK delivers below or at our target ceiling.
static void BilinearScalePlane(const uint8_t* src, int srcW, int srcH,
                               uint8_t*       dst, int dstStride,
                               int dstX, int dstY, int dstW, int dstH)
{
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;

    const float xRatio = (float)srcW / (float)dstW;
    const float yRatio = (float)srcH / (float)dstH;
    const float srcMaxX = (float)(srcW - 1);
    const float srcMaxY = (float)(srcH - 1);

    for (int y = 0; y < dstH; ++y) {
        float sy = (y + 0.5f) * yRatio - 0.5f;
        if (sy < 0.0f)    sy = 0.0f;
        if (sy > srcMaxY) sy = srcMaxY;
        const int   sy0 = (int)sy;
        const int   sy1 = std::min(sy0 + 1, srcH - 1);
        const float wy  = sy - (float)sy0;

        const uint8_t* row0 = src + (size_t)sy0 * srcW;
        const uint8_t* row1 = src + (size_t)sy1 * srcW;
        uint8_t*       out  = dst + (size_t)(dstY + y) * dstStride + dstX;

        for (int x = 0; x < dstW; ++x) {
            float sx = (x + 0.5f) * xRatio - 0.5f;
            if (sx < 0.0f)    sx = 0.0f;
            if (sx > srcMaxX) sx = srcMaxX;
            const int   sx0 = (int)sx;
            const int   sx1 = std::min(sx0 + 1, srcW - 1);
            const float wx  = sx - (float)sx0;

            const float a = (float)row0[sx0] * (1.0f - wx) + (float)row0[sx1] * wx;
            const float b = (float)row1[sx0] * (1.0f - wx) + (float)row1[sx1] * wx;
            out[x] = (uint8_t)(a * (1.0f - wy) + b * wy + 0.5f);
        }
    }
}

}  // namespace

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
    // scale so the content lands wholly inside the target.
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
    int contentX = ((m_targetW - contentW) / 2) & ~1;
    int contentY = ((m_targetH - contentH) / 2) & ~1;

    // Fill the full target planes with video black. Y=0, U=V=128 (neutral
    // chroma). The bilinear pass below overwrites only the content
    // subregion, leaving the margin as the memset'd black.
    std::memset(dstY, 0,   (size_t)m_targetW * m_targetH);
    std::memset(dstU, 128, (size_t)(m_targetW / 2) * (m_targetH / 2));
    std::memset(dstV, 128, (size_t)(m_targetW / 2) * (m_targetH / 2));

    // Y plane: full resolution.
    BilinearScalePlane(srcY, srcWe, srcHe,
                       dstY, m_targetW,
                       contentX, contentY, contentW, contentH);

    // U and V planes: half resolution in both dimensions.
    BilinearScalePlane(srcU, srcWe / 2, srcHe / 2,
                       dstU, m_targetW / 2,
                       contentX / 2, contentY / 2,
                       contentW / 2, contentH / 2);
    BilinearScalePlane(srcV, srcWe / 2, srcHe / 2,
                       dstV, m_targetW / 2,
                       contentX / 2, contentY / 2,
                       contentW / 2, contentH / 2);
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
