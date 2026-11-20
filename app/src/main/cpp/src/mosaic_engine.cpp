#include "mosaic_engine.hpp"

#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/core/ocl.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define FAM_HAS_NEON 1
#else
#define FAM_HAS_NEON 0
#endif

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <random>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fam {

namespace {

using SteadyClock = std::chrono::steady_clock;

[[nodiscard]] inline uint64_t elapsedMicroseconds(SteadyClock::time_point start) noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - start).count());
}

[[nodiscard]] inline uint64_t currentEpochMs() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

[[nodiscard]] inline size_t alignUp(size_t value, size_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

[[nodiscard]] bool writeAllToFd(int fd, const uint8_t* data, size_t totalBytes) noexcept {
    size_t written = 0;
    while (written < totalBytes) {
        const ssize_t rc = ::write(fd, data + written, totalBytes - written);
        if (rc <= 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += static_cast<size_t>(rc);
    }
    return true;
}

[[nodiscard]] inline uint64_t splitMix64(uint64_t& state) noexcept {
    uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

[[nodiscard]] inline uint32_t xorshift32(uint32_t& state) noexcept {
    uint32_t x = state ? state : 0x6D2B79F5U;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
}

/**
 * Single-pass strided Y-plane 2x2 or 4x4 box filter downsampler with ARM NEON SIMD.
 * Reads directly from ISP-padded yPtr (respecting yRowStride) without copying the full 12MP plane.
 */
void downsampleYPlaneStrided(
    const uint8_t* srcY,
    int32_t srcW,
    int32_t srcH,
    int32_t srcStride,
    int32_t factor, // 2 or 4
    std::vector<uint8_t>& dstBuf,
    int32_t& outW,
    int32_t& outH) {

    outW = srcW / factor;
    outH = srcH / factor;
    dstBuf.resize(static_cast<size_t>(outW) * static_cast<size_t>(outH));

    if (factor == 2) {
        for (int32_t dy = 0; dy < outH; ++dy) {
            const uint8_t* r0 = srcY + static_cast<size_t>(dy * 2) * srcStride;
            const uint8_t* r1 = r0 + srcStride;
            uint8_t* dstRow = dstBuf.data() + static_cast<size_t>(dy) * outW;
            int32_t dx = 0;
#if FAM_HAS_NEON
            for (; dx + 8 <= outW; dx += 8) {
                const uint8x16_t v0 = vld1q_u8(r0 + dx * 2);
                const uint8x16_t v1 = vld1q_u8(r1 + dx * 2);
                const uint16x8_t sum = vpadalq_u8(vpaddlq_u8(v0), v1);
                const uint8x8_t avg = vrshrn_n_u16(sum, 2);
                vst1_u8(dstRow + dx, avg);
            }
#endif
            for (; dx < outW; ++dx) {
                const int32_t sx = dx * 2;
                const uint32_t sum = static_cast<uint32_t>(r0[sx]) + r0[sx + 1] +
                                     r1[sx] + r1[sx + 1];
                dstRow[dx] = static_cast<uint8_t>((sum + 2U) >> 2);
            }
        }
        return;
    }

    // factor == 4 (fast path for 12MP..50MP frames -> ~1000x750 pyramid in < 0.2 ms)
    for (int32_t dy = 0; dy < outH; ++dy) {
        const uint8_t* r0 = srcY + static_cast<size_t>(dy * 4) * srcStride;
        const uint8_t* r1 = r0 + srcStride;
        const uint8_t* r2 = r1 + srcStride;
        const uint8_t* r3 = r2 + srcStride;
        uint8_t* dstRow = dstBuf.data() + static_cast<size_t>(dy) * outW;
        int32_t dx = 0;
#if FAM_HAS_NEON
        for (; dx + 8 <= outW; dx += 8) {
            const int32_t sx0 = dx * 4;
            const int32_t sx1 = sx0 + 16;

            uint16x8_t s0 = vpaddlq_u8(vld1q_u8(r0 + sx0));
            s0 = vpadalq_u8(s0, vld1q_u8(r1 + sx0));
            s0 = vpadalq_u8(s0, vld1q_u8(r2 + sx0));
            s0 = vpadalq_u8(s0, vld1q_u8(r3 + sx0));
            const uint32x4_t q0 = vpaddlq_u16(s0);

            uint16x8_t s1 = vpaddlq_u8(vld1q_u8(r0 + sx1));
            s1 = vpadalq_u8(s1, vld1q_u8(r1 + sx1));
            s1 = vpadalq_u8(s1, vld1q_u8(r2 + sx1));
            s1 = vpadalq_u8(s1, vld1q_u8(r3 + sx1));
            const uint32x4_t q1 = vpaddlq_u16(s1);

            const uint16x8_t avg16 = vcombine_u16(vrshrn_n_u32(q0, 4), vrshrn_n_u32(q1, 4));
            vst1_u8(dstRow + dx, vmovn_u16(avg16));
        }
#endif
        for (; dx < outW; ++dx) {
            const int32_t sx = dx * 4;
            const uint32_t sum =
                static_cast<uint32_t>(r0[sx]) + r0[sx + 1] + r0[sx + 2] + r0[sx + 3] +
                static_cast<uint32_t>(r1[sx]) + r1[sx + 1] + r1[sx + 2] + r1[sx + 3] +
                static_cast<uint32_t>(r2[sx]) + r2[sx + 1] + r2[sx + 2] + r2[sx + 3] +
                static_cast<uint32_t>(r3[sx]) + r3[sx + 1] + r3[sx + 2] + r3[sx + 3];
            dstRow[dx] = static_cast<uint8_t>((sum + 8U) >> 4);
        }
    }
}

/**
 * Hardware POPCNT 256-bit Hamming distance between two 32-byte ORB descriptors.
 */
[[nodiscard]] inline int32_t hamming256(const uint8_t* a, const uint8_t* b) noexcept {
#if FAM_HAS_NEON
    const uint8x16_t x0 = vcntq_u8(veorq_u8(vld1q_u8(a), vld1q_u8(b)));
    const uint8x16_t x1 = vcntq_u8(veorq_u8(vld1q_u8(a + 16), vld1q_u8(b + 16)));
    const uint8x16_t pairSum = vaddq_u8(x0, x1); // Max per lane = 16 (never overflows uint8_t)
#if defined(__aarch64__)
    return static_cast<int32_t>(vaddlvq_u8(pairSum));
#else
    const uint16x8_t s16 = vpaddlq_u8(pairSum);
    const uint32x4_t s32 = vpaddlq_u16(s16);
    const uint64x2_t s64 = vpaddlq_u32(s32);
    return static_cast<int32_t>(vgetq_lane_u64(s64, 0) + vgetq_lane_u64(s64, 1));
#endif
#else
    uint64_t a0, a1, a2, a3;
    uint64_t b0, b1, b2, b3;
    std::memcpy(&a0, a, 8);
    std::memcpy(&a1, a + 8, 8);
    std::memcpy(&a2, a + 16, 8);
    std::memcpy(&a3, a + 24, 8);
    std::memcpy(&b0, b, 8);
    std::memcpy(&b1, b + 8, 8);
    std::memcpy(&b2, b + 16, 8);
    std::memcpy(&b3, b + 24, 8);
    return std::popcount(a0 ^ b0) + std::popcount(a1 ^ b1) +
           std::popcount(a2 ^ b2) + std::popcount(a3 ^ b3);
#endif
}

/**
 * Validates that an estimated 3x3 projective homography H is non-degenerate, preserves
 * quad orientation without self-intersection, and keeps projected points in front of the camera.
 */
[[nodiscard]] bool isHomographyGeometricallyValid(
    const cv::Mat& H,
    double refW,
    double refH,
    double dstW,
    double dstH) noexcept {

    if (H.empty() || H.rows != 3 || H.cols != 3 || H.type() != CV_64F) {
        return false;
    }

    const double* h = H.ptr<double>(0);
    for (int i = 0; i < 9; ++i) {
        if (!std::isfinite(h[i])) {
            return false;
        }
    }

    // Project reference frame corners to verify positive depth and consistent winding
    const double corners[4][2] = {
        {0.0, 0.0},
        {refW, 0.0},
        {refW, refH},
        {0.0, refH}
    };
    double proj[4][2]{};
    for (int i = 0; i < 4; ++i) {
        const double x = corners[i][0];
        const double y = corners[i][1];
        const double w = h[6] * x + h[7] * y + h[8];
        if (std::abs(w) < 1e-4) {
            return false;
        }
        // Require consistent sign of homogeneous w across all 4 corners (no horizon crossing)
        if (i > 0) {
            const double w0 = h[8];
            if ((w > 0.0) != (w0 > 0.0)) {
                return false;
            }
        }
        proj[i][0] = (h[0] * x + h[1] * y + h[2]) / w;
        proj[i][1] = (h[3] * x + h[4] * y + h[5]) / w;
    }

    // Check strict convexity and consistent cross-product sign (prevents bowtie/reflection)
    double firstCross = 0.0;
    double signedArea2 = 0.0;
    for (int i = 0; i < 4; ++i) {
        const int next = (i + 1) & 3;
        const int next2 = (i + 2) & 3;
        const double dx1 = proj[next][0] - proj[i][0];
        const double dy1 = proj[next][1] - proj[i][1];
        const double dx2 = proj[next2][0] - proj[next][0];
        const double dy2 = proj[next2][1] - proj[next][1];
        const double cross = dx1 * dy2 - dy1 * dx2;
        if (i == 0) {
            firstCross = cross;
            if (std::abs(firstCross) < 1.0) {
                return false;
            }
        } else if (cross * firstCross <= 0.0) {
            return false; // Self-intersecting or collapsed quad
        }
        signedArea2 += proj[i][0] * proj[next][1] - proj[next][0] * proj[i][1];
    }

    // Reject mirrored reflections (reference corners have positive clockwise screen-space area)
    if (signedArea2 <= 0.0) {
        return false;
    }

    const double projectedArea = 0.5 * signedArea2;
    const double dstArea = dstW * dstH;
    // Reject absurd zoom-out (< 0.15% of frame) or extreme blow-up (> 64x frame area)
    return projectedArea >= (dstArea * 0.0015) && projectedArea <= (dstArea * 64.0);
}

[[nodiscard]] bool pointInPolygon(float px, float py, std::span<const FamTplVertex> poly) noexcept {
    bool inside = false;
    const size_t n = poly.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const float xi = poly[i].normX;
        const float yi = poly[i].normY;
        const float xj = poly[j].normX;
        const float yj = poly[j].normY;
        const bool intersect = ((yi > py) != (yj > py)) &&
                               (px < (xj - xi) * (py - yi) / ((yj - yi) + 1e-12f) + xi);
        if (intersect) {
            inside = !inside;
        }
    }
    return inside;
}

[[nodiscard]] bool segmentIntersectsAabb(
    float x0, float y0, float x1, float y1,
    float minX, float minY, float maxX, float maxY) noexcept {

    // Liang-Barsky parametric clipping against [minX, maxX] x [minY, maxY]
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {x0 - minX, maxX - x0, y0 - minY, maxY - y0};

    float t0 = 0.0f;
    float t1 = 1.0f;
    for (int i = 0; i < 4; ++i) {
        if (std::abs(p[i]) < 1e-7f) {
            if (q[i] < 0.0f) {
                return false;
            }
        } else {
            const float t = q[i] / p[i];
            if (p[i] < 0.0f) {
                if (t > t1) return false;
                if (t > t0) t0 = t;
            } else {
                if (t < t0) return false;
                if (t < t1) t1 = t;
            }
        }
    }
    return t0 <= t1;
}

/**
 * Exact conservative block-vs-polygon intersection test:
 * Guarantees 100% coverage of thin, diagonal, or concave polygon edges with zero edge leakage.
 */
[[nodiscard]] bool blockIntersectsPolygon(
    float bx0, float by0, float bx1, float by1,
    std::span<const FamTplVertex> poly) noexcept {

    const float cx = 0.5f * (bx0 + bx1);
    const float cy = 0.5f * (by0 + by1);
    if (pointInPolygon(cx, cy, poly) ||
        pointInPolygon(bx0 + 0.25f, by0 + 0.25f, poly) ||
        pointInPolygon(bx1 - 0.25f, by0 + 0.25f, poly) ||
        pointInPolygon(bx1 - 0.25f, by1 - 0.25f, poly) ||
        pointInPolygon(bx0 + 0.25f, by1 - 0.25f, poly)) {
        return true;
    }

    const size_t n = poly.size();
    for (size_t i = 0; i < n; ++i) {
        const float vx = poly[i].normX;
        const float vy = poly[i].normY;
        if (vx >= bx0 && vx <= bx1 && vy >= by0 && vy <= by1) {
            return true;
        }
        const size_t next = (i + 1 == n) ? 0 : (i + 1);
        if (segmentIntersectsAabb(vx, vy, poly[next].normX, poly[next].normY, bx0, by0, bx1, by1)) {
            return true;
        }
    }
    return false;
}

/**
 * ARM NEON accelerated Y-plane block mean + quantized Xorshift noise overwrite.
 */
void mosaicLumaBlockInPlace(
    uint8_t* yBase,
    int32_t yStride,
    int32_t x0,
    int32_t y0,
    int32_t bw,
    int32_t bh,
    uint8_t noiseAmp,
    uint32_t& rngState) noexcept {

    uint32_t lumaSum = 0;
#if FAM_HAS_NEON
    if (bw == 16) {
        uint32x4_t acc32 = vdupq_n_u32(0);
        for (int32_t r = 0; r < bh; ++r) {
            const uint8_t* row = yBase + static_cast<size_t>(y0 + r) * yStride + x0;
            const uint16x8_t p16 = vpaddlq_u8(vld1q_u8(row));
            acc32 = vpadalq_u16(acc32, p16);
        }
#if defined(__aarch64__)
        lumaSum = vaddvq_u32(acc32);
#else
        const uint64x2_t acc64 = vpaddlq_u32(acc32);
        lumaSum = static_cast<uint32_t>(vgetq_lane_u64(acc64, 0) + vgetq_lane_u64(acc64, 1));
#endif
    } else
#endif
    {
        for (int32_t r = 0; r < bh; ++r) {
            const uint8_t* row = yBase + static_cast<size_t>(y0 + r) * yStride + x0;
            int32_t c = 0;
#if FAM_HAS_NEON
            for (; c + 8 <= bw; c += 8) {
                const uint16x4_t p16 = vpaddl_u8(vld1_u8(row + c));
                const uint32x2_t p32 = vpaddl_u16(p16);
                lumaSum += vget_lane_u32(p32, 0) + vget_lane_u32(p32, 1);
            }
#endif
            for (; c < bw; ++c) {
                lumaSum += row[c];
            }
        }
    }

    const uint32_t count = static_cast<uint32_t>(bw * bh);
    const uint8_t rawMean = static_cast<uint8_t>((lumaSum + (count >> 1)) / count);

    // Anti-Depix non-invertible quantization (4-bit quantization + random block jitter)
    const int32_t blockJitter = static_cast<int32_t>(xorshift32(rngState) & 0x0FU) - 8;
    const int32_t quantizedBase = std::clamp(
        static_cast<int32_t>((rawMean & 0xF0U) | 0x08U) + blockJitter, 0, 255);

    if (noiseAmp == 0) {
        const uint8_t fill = static_cast<uint8_t>(quantizedBase);
        for (int32_t r = 0; r < bh; ++r) {
            std::memset(yBase + static_cast<size_t>(y0 + r) * yStride + x0, fill, static_cast<size_t>(bw));
        }
        return;
    }

    const uint32_t span = static_cast<uint32_t>(noiseAmp) * 2U + 1U;
#if FAM_HAS_NEON
    // Vectorized 4-lane Xorshift32 PRNG in NEON registers (16 random bytes per iteration)
    uint32x4_t vRng = {
        xorshift32(rngState),
        xorshift32(rngState) ^ 0x9E3779B9U,
        xorshift32(rngState) ^ 0x85EBCA6BU,
        xorshift32(rngState) ^ 0xC2B2AE35U
    };
    const uint8x16_t vBase = vdupq_n_u8(static_cast<uint8_t>(quantizedBase));
    const uint8x16_t vAmp = vdupq_n_u8(noiseAmp);
    // Power-of-two mask scaled down via widening multiply: ((rand8 * (2*amp + 1)) >> 8)
    const uint8x8_t vSpan = vdup_n_u8(static_cast<uint8_t>(std::min<uint32_t>(span, 255U)));

    for (int32_t r = 0; r < bh; ++r) {
        uint8_t* row = yBase + static_cast<size_t>(y0 + r) * yStride + x0;
        int32_t c = 0;
        for (; c + 16 <= bw; c += 16) {
            vRng = veorq_u32(vRng, vshlq_n_u32(vRng, 13));
            vRng = veorq_u32(vRng, vshrq_n_u32(vRng, 17));
            vRng = veorq_u32(vRng, vshlq_n_u32(vRng, 5));

            const uint8x16_t rnd8 = vreinterpretq_u8_u32(vRng);
            const uint8x8_t deltaLo = vshrn_n_u16(vmull_u8(vget_low_u8(rnd8), vSpan), 8);
            const uint8x8_t deltaHi = vshrn_n_u16(vmull_u8(vget_high_u8(rnd8), vSpan), 8);
            const uint8x16_t delta = vcombine_u8(deltaLo, deltaHi);

            // Saturating unsigned arithmetic: clamp(base - amp + delta, 0, 255)
            // Compute positive & negative parts separately to prevent low-end clipping bias:
            const uint8x16_t posMask = vcgeq_u8(delta, vAmp);
            const uint8x16_t posAdd = vandq_u8(vsubq_u8(delta, vAmp), posMask);
            const uint8x16_t negSub = vbicq_u8(vsubq_u8(vAmp, delta), posMask);
            const uint8x16_t noisy = vqsubq_u8(vqaddq_u8(vBase, posAdd), negSub);
            vst1q_u8(row + c, noisy);
        }
        for (; c < bw; ++c) {
            const uint32_t rnd = xorshift32(rngState) & 0xFFU;
            const int32_t delta = static_cast<int32_t>((rnd * span) >> 8) - static_cast<int32_t>(noiseAmp);
            row[c] = static_cast<uint8_t>(std::clamp(quantizedBase + delta, 0, 255));
        }
    }
#else
    for (int32_t r = 0; r < bh; ++r) {
        uint8_t* row = yBase + static_cast<size_t>(y0 + r) * yStride + x0;
        for (int32_t c = 0; c < bw; ++c) {
            const uint32_t rnd = xorshift32(rngState) & 0xFFU;
            const int32_t delta = static_cast<int32_t>((rnd * span) >> 8) - static_cast<int32_t>(noiseAmp);
            row[c] = static_cast<uint8_t>(std::clamp(quantizedBase + delta, 0, 255));
        }
    }
#endif
}

/**
 * Synchronized UV chroma block overwrite supporting:
 *  - Planar I420 / YV12 / 422 / 444 (uvPixelStride == 1) via NEON vst1
 *  - Semi-planar NV12 (uvPixelStride == 2, vPtr == uPtr + 1) via NEON vst2
 *  - Semi-planar NV21 (uvPixelStride == 2, uPtr == vPtr + 1) via NEON vst2
 *  - Arbitrary strided vendor HAL3 buffers
 */
void mosaicChromaBlockInPlace(
    const YuvPlaneView& frame,
    int32_t lumaX0,
    int32_t lumaY0,
    int32_t lumaX1,
    int32_t lumaY1,
    uint32_t& rngState) noexcept {

    if (frame.subsampling == ChromaSubsampling::GRAY8 || !frame.uPtr || !frame.vPtr) {
        return;
    }

    int32_t cx0 = lumaX0;
    int32_t cx1 = lumaX1;
    int32_t cy0 = lumaY0;
    int32_t cy1 = lumaY1;
    int32_t maxCw = frame.width;
    int32_t maxCh = frame.height;

    if (frame.subsampling == ChromaSubsampling::YUV420) {
        cx0 >>= 1;
        cx1 = (cx1 + 1) >> 1;
        cy0 >>= 1;
        cy1 = (cy1 + 1) >> 1;
        maxCw = (frame.width + 1) >> 1;
        maxCh = (frame.height + 1) >> 1;
    } else if (frame.subsampling == ChromaSubsampling::YUV422) {
        cx0 >>= 1;
        cx1 = (cx1 + 1) >> 1;
        maxCw = (frame.width + 1) >> 1;
    }

    cx0 = std::clamp(cx0, 0, maxCw);
    cx1 = std::clamp(cx1, cx0, maxCw);
    cy0 = std::clamp(cy0, 0, maxCh);
    cy1 = std::clamp(cy1, cy0, maxCh);
    const int32_t cw = cx1 - cx0;
    const int32_t ch = cy1 - cy0;
    if (cw <= 0 || ch <= 0) {
        return;
    }

    // Sample block mean U/V and strongly desaturate toward 128 + random jitter to prevent color leakage
    uint32_t sumU = 0;
    uint32_t sumV = 0;
    const int32_t pStride = frame.uvPixelStride;
    for (int32_t r = 0; r < ch; ++r) {
        const uint8_t* uRow = frame.uPtr + static_cast<size_t>(cy0 + r) * frame.uRowStride +
                              static_cast<size_t>(cx0) * pStride;
        const uint8_t* vRow = frame.vPtr + static_cast<size_t>(cy0 + r) * frame.vRowStride +
                              static_cast<size_t>(cx0) * pStride;
        for (int32_t c = 0; c < cw; ++c) {
            sumU += uRow[c * pStride];
            sumV += vRow[c * pStride];
        }
    }

    const int32_t totalSamples = cw * ch;
    const int32_t meanU = static_cast<int32_t>(sumU / static_cast<uint32_t>(totalSamples));
    const int32_t meanV = static_cast<int32_t>(sumV / static_cast<uint32_t>(totalSamples));
    const uint32_t jitterBits = xorshift32(rngState);
    const int32_t jitterU = static_cast<int32_t>(jitterBits & 0x07U) - 4;
    const int32_t jitterV = static_cast<int32_t>((jitterBits >> 4) & 0x07U) - 4;

    // Attenuate chroma saturation to 25% + jitter: removes ink/skin color signatures cleanly
    const uint8_t outU = static_cast<uint8_t>(std::clamp(128 + ((meanU - 128) >> 2) + jitterU, 16, 240));
    const uint8_t outV = static_cast<uint8_t>(std::clamp(128 + ((meanV - 128) >> 2) + jitterV, 16, 240));

    if (pStride == 1) {
        // Planar I420 / YV12
        for (int32_t r = 0; r < ch; ++r) {
            uint8_t* uRow = frame.uPtr + static_cast<size_t>(cy0 + r) * frame.uRowStride + cx0;
            uint8_t* vRow = frame.vPtr + static_cast<size_t>(cy0 + r) * frame.vRowStride + cx0;
#if FAM_HAS_NEON
            int32_t c = 0;
            const uint8x8_t vOutU = vdup_n_u8(outU);
            const uint8x8_t vOutV = vdup_n_u8(outV);
            for (; c + 8 <= cw; c += 8) {
                vst1_u8(uRow + c, vOutU);
                vst1_u8(vRow + c, vOutV);
            }
            for (; c < cw; ++c) {
                uRow[c] = outU;
                vRow[c] = outV;
            }
#else
            std::memset(uRow, outU, static_cast<size_t>(cw));
            std::memset(vRow, outV, static_cast<size_t>(cw));
#endif
        }
        return;
    }

    // Semi-planar pixelStride == 2 (NV12: V == U + 1, NV21: U == V + 1, or independent buffers)
    const ptrdiff_t uvDelta = frame.vPtr - frame.uPtr;
    const bool isNv12 = (uvDelta == 1 && frame.uRowStride == frame.vRowStride);
    const bool isNv21 = (uvDelta == -1 && frame.uRowStride == frame.vRowStride);

#if FAM_HAS_NEON
    if (isNv12 || isNv21) {
        uint8_t* interleavedBase = isNv12 ? frame.uPtr : frame.vPtr;
        const int32_t rowStride = frame.uRowStride;
        uint8x8x2_t pairVec;
        pairVec.val[0] = vdup_n_u8(isNv12 ? outU : outV);
        pairVec.val[1] = vdup_n_u8(isNv12 ? outV : outU);
        const uint8_t firstByte = isNv12 ? outU : outV;
        const uint8_t secondByte = isNv12 ? outV : outU;

        for (int32_t r = 0; r < ch; ++r) {
            uint8_t* row = interleavedBase + static_cast<size_t>(cy0 + r) * rowStride +
                           static_cast<size_t>(cx0) * 2U;
            int32_t c = 0;
            for (; c + 8 <= cw; c += 8) {
                vst2_u8(row + c * 2, pairVec);
            }
            for (; c < cw; ++c) {
                row[c * 2] = firstByte;
                row[c * 2 + 1] = secondByte;
            }
        }
        return;
    }
#else
    (void)isNv12;
    (void)isNv21;
#endif

    // Universal strided fallback (safe for all vendor HAL3 plane layouts)
    for (int32_t r = 0; r < ch; ++r) {
        uint8_t* uRow = frame.uPtr + static_cast<size_t>(cy0 + r) * frame.uRowStride +
                        static_cast<size_t>(cx0) * 2U;
        uint8_t* vRow = frame.vPtr + static_cast<size_t>(cy0 + r) * frame.vRowStride +
                        static_cast<size_t>(cx0) * 2U;
        for (int32_t c = 0; c < cw; ++c) {
            uRow[c * 2] = outU;
            vRow[c * 2] = outV;
        }
    }
}

} // namespace

// ============================================================================
// FAMTPL02 Binary Serialization & Zero-Copy mmap Implementation
// ============================================================================

MappedFamTemplate::~MappedFamTemplate() noexcept {
    reset();
}

MappedFamTemplate::MappedFamTemplate(MappedFamTemplate&& other) noexcept {
    *this = std::move(other);
}

MappedFamTemplate& MappedFamTemplate::operator=(MappedFamTemplate&& other) noexcept {
    if (this != &other) {
        reset();
        mappedAddr_ = other.mappedAddr_;
        mappedBytes_ = other.mappedBytes_;
        ownedCopy_ = std::move(other.ownedCopy_);
        header_ = other.header_;
        descriptors_ = other.descriptors_;
        keypoints_ = other.keypoints_;
        polygons_ = other.polygons_;
        vertices_ = other.vertices_;

        other.mappedAddr_ = nullptr;
        other.mappedBytes_ = 0;
        other.header_ = nullptr;
        other.descriptors_ = {};
        other.keypoints_ = {};
        other.polygons_ = {};
        other.vertices_ = {};
    }
    return *this;
}

void MappedFamTemplate::reset() noexcept {
    if (mappedAddr_ != nullptr && mappedBytes_ > 0) {
        ::munmap(mappedAddr_, mappedBytes_);
    }
    mappedAddr_ = nullptr;
    mappedBytes_ = 0;
    ownedCopy_.clear();
    header_ = nullptr;
    descriptors_ = {};
    keypoints_ = {};
    polygons_ = {};
    vertices_ = {};
}

bool MappedFamTemplate::mapFromFile(const std::string& path, bool verifyChecksum,
                                    std::string* errorOut) noexcept {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errorOut) *errorOut = "Failed to open FAMTPL02 file: " + path;
        return false;
    }
    const bool ok = mapFromFd(fd, verifyChecksum, errorOut);
    ::close(fd);
    return ok;
}

bool MappedFamTemplate::mapFromFd(int fd, bool verifyChecksum,
                                  std::string* errorOut) noexcept {
    reset();
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(FamTplHeader))) {
        if (errorOut) *errorOut = "Invalid file descriptor or truncated FAMTPL02 header";
        return false;
    }

    const size_t fileBytes = static_cast<size_t>(st.st_size);
    void* addr = ::mmap(nullptr, fileBytes, PROT_READ, MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED) {
        if (errorOut) *errorOut = "mmap() failed for FAMTPL02 file";
        return false;
    }
    (void)::madvise(addr, fileBytes, MADV_SEQUENTIAL | MADV_WILLNEED);

    mappedAddr_ = addr;
    mappedBytes_ = fileBytes;
    if (!bindAndValidate(static_cast<const uint8_t*>(addr), fileBytes, verifyChecksum, errorOut)) {
        reset();
        return false;
    }
    return true;
}

bool MappedFamTemplate::loadFromMemoryCopy(std::span<const uint8_t> bytes, bool verifyChecksum,
                                           std::string* errorOut) {
    reset();
    if (bytes.size() < sizeof(FamTplHeader)) {
        if (errorOut) *errorOut = "Buffer smaller than FAMTPL02 header";
        return false;
    }
    ownedCopy_.assign(bytes.begin(), bytes.end());
    if (!bindAndValidate(ownedCopy_.data(), ownedCopy_.size(), verifyChecksum, errorOut)) {
        reset();
        return false;
    }
    return true;
}

bool MappedFamTemplate::bindAndValidate(const uint8_t* base, size_t sizeBytes, bool verifyChecksum,
                                        std::string* errorOut) noexcept {
    if ((reinterpret_cast<uintptr_t>(base) % alignof(FamTplHeader)) != 0) {
        if (errorOut) *errorOut = "Unaligned FAMTPL02 base pointer";
        return false;
    }

    const auto* hdr = reinterpret_cast<const FamTplHeader*>(base);
    if (std::memcmp(hdr->magic, kFamTplMagic, sizeof(kFamTplMagic)) != 0) {
        if (errorOut) *errorOut = "Invalid FAMTPL02 magic header";
        return false;
    }
    if (hdr->version != kFamTplVersion) {
        if (errorOut) *errorOut = "Unsupported FAMTPL02 version";
        return false;
    }
    if (hdr->refWidth < 16 || hdr->refHeight < 16 ||
        hdr->keypointCount > kMaxTemplateKeypoints ||
        hdr->polygonCount < 1 || hdr->polygonCount > kMaxTemplatePolygons ||
        hdr->totalVertexCount < 3 || hdr->totalVertexCount > kMaxTemplateVertices) {
        if (errorOut) *errorOut = "FAMTPL02 counts or reference dimensions out of valid range";
        return false;
    }

    const size_t totalRequired = sizeof(FamTplHeader) + static_cast<size_t>(hdr->payloadBytes);
    if (totalRequired > sizeBytes) {
        if (errorOut) *errorOut = "FAMTPL02 payload truncated";
        return false;
    }

    const size_t descBytes = static_cast<size_t>(hdr->keypointCount) * kOrbDescriptorBytes;
    const size_t kpBytes = static_cast<size_t>(hdr->keypointCount) * sizeof(FamTplKeypoint);
    const size_t polyBytes = static_cast<size_t>(hdr->polygonCount) * sizeof(FamTplPolygonSpan);
    const size_t vertBytes = static_cast<size_t>(hdr->totalVertexCount) * sizeof(FamTplVertex);

    if (hdr->descriptorOffset != sizeof(FamTplHeader) ||
        (hdr->keypointOffset % alignof(FamTplKeypoint)) != 0 ||
        (hdr->polygonOffset % alignof(FamTplPolygonSpan)) != 0 ||
        (hdr->vertexOffset % alignof(FamTplVertex)) != 0 ||
        hdr->descriptorOffset + descBytes > hdr->keypointOffset ||
        hdr->keypointOffset + kpBytes > hdr->polygonOffset ||
        hdr->polygonOffset + polyBytes > hdr->vertexOffset ||
        hdr->vertexOffset + vertBytes > totalRequired) {
        if (errorOut) *errorOut = "FAMTPL02 section offsets corrupt or overlapping";
        return false;
    }

    if (verifyChecksum) {
        const uint64_t actualHash = computeFnv1a64(base + sizeof(FamTplHeader), hdr->payloadBytes);
        if (actualHash != hdr->checksum64) {
            if (errorOut) *errorOut = "FAMTPL02 FNV-1a64 checksum mismatch";
            return false;
        }
    }

    const auto* polyPtr = reinterpret_cast<const FamTplPolygonSpan*>(base + hdr->polygonOffset);
    const auto* vertPtr = reinterpret_cast<const FamTplVertex*>(base + hdr->vertexOffset);

    for (uint16_t p = 0; p < hdr->polygonCount; ++p) {
        if (polyPtr[p].vertexCount < 3 ||
            static_cast<uint64_t>(polyPtr[p].firstVertex) + polyPtr[p].vertexCount >
                hdr->totalVertexCount) {
            if (errorOut) *errorOut = "FAMTPL02 polygon span out of bounds";
            return false;
        }
    }
    for (uint16_t v = 0; v < hdr->totalVertexCount; ++v) {
        if (!std::isfinite(vertPtr[v].normX) || !std::isfinite(vertPtr[v].normY)) {
            if (errorOut) *errorOut = "FAMTPL02 polygon contains non-finite vertex";
            return false;
        }
    }

    header_ = hdr;
    descriptors_ = {base + hdr->descriptorOffset, descBytes};
    keypoints_ = {reinterpret_cast<const FamTplKeypoint*>(base + hdr->keypointOffset),
                  hdr->keypointCount};
    polygons_ = {polyPtr, hdr->polygonCount};
    vertices_ = {vertPtr, hdr->totalVertexCount};
    return true;
}

bool serializeFamTemplateToBuffer(
    uint32_t refWidth,
    uint32_t refHeight,
    std::span<const FamTplKeypoint> keypoints,
    std::span<const uint8_t> descriptors,
    std::span<const FamTplPolygonSpan> polygons,
    std::span<const FamTplVertex> vertices,
    uint64_t createdEpochMs,
    std::vector<uint8_t>& outBytes,
    std::string* errorOut,
    uint16_t refExifOrientation) {

    if (refWidth < 16 || refHeight < 16) {
        if (errorOut) *errorOut = "Invalid reference dimensions";
        return false;
    }
    if (keypoints.size() > kMaxTemplateKeypoints ||
        descriptors.size() != keypoints.size() * kOrbDescriptorBytes) {
        if (errorOut) *errorOut = "Invalid ORB keypoint/descriptor count";
        return false;
    }
    if (polygons.empty() || polygons.size() > kMaxTemplatePolygons ||
        vertices.size() < 3 || vertices.size() > kMaxTemplateVertices) {
        if (errorOut) *errorOut = "Invalid polygon mask specification";
        return false;
    }

    const size_t descOffset = sizeof(FamTplHeader); // 64-byte aligned
    const size_t kpOffset = alignUp(descOffset + descriptors.size_bytes(), alignof(FamTplKeypoint));
    const size_t polyOffset = alignUp(kpOffset + keypoints.size_bytes(), alignof(FamTplPolygonSpan));
    const size_t vertOffset = alignUp(polyOffset + polygons.size_bytes(), alignof(FamTplVertex));
    const size_t totalBytes = alignUp(vertOffset + vertices.size_bytes(), 8);

    outBytes.assign(totalBytes, 0);

    if (!descriptors.empty()) {
        std::memcpy(outBytes.data() + descOffset, descriptors.data(), descriptors.size_bytes());
    }
    if (!keypoints.empty()) {
        std::memcpy(outBytes.data() + kpOffset, keypoints.data(), keypoints.size_bytes());
    }
    std::memcpy(outBytes.data() + polyOffset, polygons.data(), polygons.size_bytes());
    std::memcpy(outBytes.data() + vertOffset, vertices.data(), vertices.size_bytes());

    FamTplHeader hdr{};
    std::memcpy(hdr.magic, kFamTplMagic, sizeof(kFamTplMagic));
    hdr.version = kFamTplVersion;
    hdr.flags = encodeFamTplFlags(polygons.size() > 1, refExifOrientation);
    hdr.refWidth = refWidth;
    hdr.refHeight = refHeight;
    hdr.keypointCount = static_cast<uint32_t>(keypoints.size());
    hdr.polygonCount = static_cast<uint16_t>(polygons.size());
    hdr.totalVertexCount = static_cast<uint16_t>(vertices.size());
    hdr.descriptorOffset = static_cast<uint32_t>(descOffset);
    hdr.keypointOffset = static_cast<uint32_t>(kpOffset);
    hdr.polygonOffset = static_cast<uint32_t>(polyOffset);
    hdr.vertexOffset = static_cast<uint32_t>(vertOffset);
    hdr.payloadBytes = static_cast<uint32_t>(totalBytes - sizeof(FamTplHeader));
    hdr.checksum64 = computeFnv1a64(outBytes.data() + sizeof(FamTplHeader), hdr.payloadBytes);
    hdr.createdEpochMs = createdEpochMs ? createdEpochMs : currentEpochMs();

    std::memcpy(outBytes.data(), &hdr, sizeof(FamTplHeader));
    return true;
}

bool writeFamTemplateToFd(
    int fd,
    uint32_t refWidth,
    uint32_t refHeight,
    std::span<const FamTplKeypoint> keypoints,
    std::span<const uint8_t> descriptors,
    std::span<const FamTplPolygonSpan> polygons,
    std::span<const FamTplVertex> vertices,
    uint64_t createdEpochMs,
    std::string* errorOut,
    uint16_t refExifOrientation) {

    std::vector<uint8_t> bytes;
    if (!serializeFamTemplateToBuffer(refWidth, refHeight, keypoints, descriptors,
                                      polygons, vertices, createdEpochMs, bytes, errorOut,
                                      refExifOrientation)) {
        return false;
    }
    if (::ftruncate(fd, 0) != 0 && errno != EINVAL) {
        if (errorOut) *errorOut = "ftruncate failed on FAMTPL02 output FD";
        return false;
    }
    (void)::lseek(fd, 0, SEEK_SET);
    if (!writeAllToFd(fd, bytes.data(), bytes.size())) {
        if (errorOut) *errorOut = "Failed writing FAMTPL02 payload to FD";
        return false;
    }
    return true;
}

bool writeFamTemplateToFile(
    const std::string& path,
    uint32_t refWidth,
    uint32_t refHeight,
    std::span<const FamTplKeypoint> keypoints,
    std::span<const uint8_t> descriptors,
    std::span<const FamTplPolygonSpan> polygons,
    std::span<const FamTplVertex> vertices,
    uint64_t createdEpochMs,
    std::string* errorOut,
    uint16_t refExifOrientation) {

    const std::string tmpPath = path + ".tmp";
    const int fd = ::open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (errorOut) *errorOut = "Failed creating temporary FAMTPL02 file: " + tmpPath;
        return false;
    }
    const bool ok = writeFamTemplateToFd(fd, refWidth, refHeight, keypoints, descriptors,
                                         polygons, vertices, createdEpochMs, errorOut,
                                         refExifOrientation);
    if (ok) {
        (void)::fsync(fd);
    }
    ::close(fd);
    if (!ok) {
        ::unlink(tmpPath.c_str());
        return false;
    }
    if (::rename(tmpPath.c_str(), path.c_str()) != 0) {
        ::unlink(tmpPath.c_str());
        if (errorOut) *errorOut = "Atomic rename failed for FAMTPL02 file: " + path;
        return false;
    }
    return true;
}

// ============================================================================
// MosaicEngine Implementation
// ============================================================================

struct MosaicEngine::Impl {
    MosaicConfig config;
    cv::Ptr<cv::ORB> learnOrb;
    cv::Ptr<cv::ORB> targetOrb;
    cv::Ptr<cv::CLAHE> clahe;
    std::unordered_map<std::string, MappedFamTemplate> templates;

    explicit Impl(const MosaicConfig& cfg) : config(cfg) {
        // Strictly enforce Red Line #1: 100% Pure CPU, disable any OpenCL dispatch inside OpenCV
        cv::ocl::setUseOpenCL(false);
        cv::setNumThreads(2); // Keep big/mid cores responsive without thread-pool thrashing
        clahe = cv::createCLAHE(2.0, cv::Size(8, 8));
        rebuildDetectors();
    }

    std::vector<uint8_t> pyramidStage1;
    std::vector<uint8_t> pyramidStage2;
    cv::Mat claheWorkY;

    void rebuildDetectors() {
        learnOrb = cv::ORB::create(
            std::clamp(config.learnOrbFeatures, 100, 4000),
            1.2f,
            8,
            15,
            0,
            2,
            cv::ORB::HARRIS_SCORE,
            31,
            12);

        targetOrb = cv::ORB::create(
            std::clamp(config.targetOrbFeatures, 100, 2500),
            1.2f,
            6,
            15,
            0,
            2,
            cv::ORB::HARRIS_SCORE,
            31,
            14);
    }

    cv::Mat enhanceWorkY(const cv::Mat& workY) {
        if (clahe && workY.cols >= 32 && workY.rows >= 32) {
            clahe->apply(workY, claheWorkY);
            return claheWorkY;
        }
        return workY;
    }

    cv::Mat prepareWorkYMat(
        const uint8_t* yPtr,
        int32_t width,
        int32_t height,
        int32_t yRowStride,
        bool isLearnMode,
        float& outScaleToFullX,
        float& outScaleToFullY) {

        const int32_t maxDim = std::max(width, height);
        const int32_t minDim = std::min(width, height);
        const int32_t targetMax = std::max(320, config.orbWorkMaxDim);

        if (!isLearnMode && maxDim >= targetMax * 5 && minDim >= 1600) {
            // 12MP+ (e.g. 4000x3000 -> 1000x750 -> 500x375) two-stage NEON 4x4 + 2x2 cascade
            int32_t midW = 0, midH = 0;
            int32_t workW = 0, workH = 0;
            downsampleYPlaneStrided(yPtr, width, height, yRowStride, 4, pyramidStage1, midW, midH);
            downsampleYPlaneStrided(pyramidStage1.data(), midW, midH, midW, 2, pyramidStage2, workW, workH);
            outScaleToFullX = static_cast<float>(width) / static_cast<float>(workW);
            outScaleToFullY = static_cast<float>(height) / static_cast<float>(workH);
            return cv::Mat(workH, workW, CV_8UC1, pyramidStage2.data(), static_cast<size_t>(workW));
        }

        int32_t factor = 1;
        if (maxDim >= targetMax * 3) {
            factor = 4;
        } else if (maxDim >= static_cast<int32_t>(targetMax * 1.5f)) {
            factor = 2;
        }

        if (factor == 1) {
            outScaleToFullX = 1.0f;
            outScaleToFullY = 1.0f;
            return cv::Mat(height, width, CV_8UC1, const_cast<uint8_t*>(yPtr),
                           static_cast<size_t>(yRowStride));
        }

        int32_t workW = 0;
        int32_t workH = 0;
        downsampleYPlaneStrided(yPtr, width, height, yRowStride, factor, pyramidStage1, workW, workH);
        outScaleToFullX = static_cast<float>(width) / static_cast<float>(workW);
        outScaleToFullY = static_cast<float>(height) / static_cast<float>(workH);
        return cv::Mat(workH, workW, CV_8UC1, pyramidStage1.data(), static_cast<size_t>(workW));
    }
};

MosaicEngine::MosaicEngine(const MosaicConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

MosaicEngine::~MosaicEngine() = default;

void MosaicEngine::updateConfig(const MosaicConfig& config) {
    std::unique_lock lock(mutex_);
    impl_->config = config;
    impl_->rebuildDetectors();
}

MosaicConfig MosaicEngine::getConfig() const {
    std::shared_lock lock(mutex_);
    return impl_->config;
}

float MosaicEngine::computeSharpnessScore(const YPlaneConstView& yView) noexcept {
    if (!yView.isValid()) {
        return 0.0f;
    }

    // Evaluate central 60% ROI with adaptive row stride to run in < 0.12 ms on 1080p/4K
    const int32_t xStart = (yView.width * 2) / 10;
    const int32_t xEnd = (yView.width * 8) / 10;
    const int32_t yStart = std::max(1, (yView.height * 2) / 10);
    const int32_t yEnd = std::min(yView.height - 1, (yView.height * 8) / 10);
    const int32_t yStep = (yView.height > 1080) ? 4 : 2;
    const int32_t roiW = xEnd - xStart;
    if (roiW < 16 || yEnd <= yStart) {
        return 0.0f;
    }

    uint64_t gradEnergySum = 0;
    uint64_t sampleCount = 0;

    for (int32_t y = yStart; y < yEnd; y += yStep) {
        const uint8_t* prev = yView.yPtr + static_cast<size_t>(y - 1) * yView.yRowStride + xStart;
        const uint8_t* curr = yView.yPtr + static_cast<size_t>(y) * yView.yRowStride + xStart;
        const uint8_t* next = yView.yPtr + static_cast<size_t>(y + 1) * yView.yRowStride + xStart;

        int32_t x = 1;
#if FAM_HAS_NEON
        uint32x4_t rowAcc = vdupq_n_u32(0);
        for (; x + 16 < roiW - 1; x += 16) {
            const uint8x16_t c = vld1q_u8(curr + x);
            const uint8x16_t l = vld1q_u8(curr + x - 1);
            const uint8x16_t r = vld1q_u8(curr + x + 1);
            const uint8x16_t u = vld1q_u8(prev + x);
            const uint8x16_t d = vld1q_u8(next + x);

            const uint16x8_t gx = vpaddlq_u8(vabdq_u8(l, r));
            const uint16x8_t gy = vpaddlq_u8(vabdq_u8(u, d));
            const uint16x8_t lapX = vpaddlq_u8(vabdq_u8(c, vrhaddq_u8(l, r)));
            const uint16x8_t lapY = vpaddlq_u8(vabdq_u8(c, vrhaddq_u8(u, d)));
            const uint16x8_t gSum = vaddq_u16(vaddq_u16(gx, gy), vaddq_u16(lapX, lapY));
            rowAcc = vpadalq_u16(rowAcc, gSum);
            sampleCount += 16;
        }
#if defined(__aarch64__)
        gradEnergySum += vaddvq_u32(rowAcc);
#else
        const uint64x2_t s64 = vpaddlq_u32(rowAcc);
        gradEnergySum += vgetq_lane_u64(s64, 0) + vgetq_lane_u64(s64, 1);
#endif
#endif
        for (; x < roiW - 1; ++x) {
            const int32_t c = curr[x];
            const int32_t l = curr[x - 1];
            const int32_t r = curr[x + 1];
            const int32_t u = prev[x];
            const int32_t d = next[x];
            const int32_t gx = std::abs(r - l);
            const int32_t gy = std::abs(d - u);
            const int32_t lapX = std::abs(c - ((l + r + 1) >> 1));
            const int32_t lapY = std::abs(c - ((u + d + 1) >> 1));
            gradEnergySum += static_cast<uint64_t>(gx + gy + lapX + lapY);
            ++sampleCount;
        }
    }

    if (sampleCount == 0) {
        return 0.0f;
    }
    // Scale mean Tenengrad + Laplacian energy into intuitive [0..100] sharpness index
    const float meanGrad = static_cast<float>(gradEnergySum) / static_cast<float>(sampleCount);
    return std::clamp(meanGrad * 1.6f, 0.0f, 100.0f);
}

void MosaicEngine::matchDescriptorsHammingPopcnt(
    std::span<const uint8_t> queryDescriptors,
    std::span<const uint8_t> trainDescriptors,
    float loweRatio,
    int32_t maxDist,
    std::vector<std::pair<int32_t, int32_t>>& outMatches) {

    outMatches.clear();
    const size_t nQuery = queryDescriptors.size() / kOrbDescriptorBytes;
    const size_t nTrain = trainDescriptors.size() / kOrbDescriptorBytes;
    if (nQuery < 4 || nTrain < 4) {
        return;
    }

    outMatches.reserve(std::min(nQuery, nTrain));
    const float ratioSq = loweRatio;

    // Keep unique best match per train index (lightweight one-to-one disambiguation)
    std::vector<int32_t> bestQueryForTrain(nTrain, -1);
    std::vector<int32_t> bestDistForTrain(nTrain, std::numeric_limits<int32_t>::max());

    for (size_t qi = 0; qi < nQuery; ++qi) {
        const uint8_t* qPtr = queryDescriptors.data() + qi * kOrbDescriptorBytes;
        int32_t best1 = std::numeric_limits<int32_t>::max();
        int32_t best2 = std::numeric_limits<int32_t>::max();
        int32_t bestIdx = -1;

        for (size_t ti = 0; ti < nTrain; ++ti) {
            const uint8_t* tPtr = trainDescriptors.data() + ti * kOrbDescriptorBytes;
            const int32_t d = hamming256(qPtr, tPtr);
            if (d < best1) {
                best2 = best1;
                best1 = d;
                bestIdx = static_cast<int32_t>(ti);
            } else if (d < best2) {
                best2 = d;
            }
        }

        if (bestIdx >= 0 && best1 <= maxDist &&
            static_cast<float>(best1) < ratioSq * static_cast<float>(best2)) {
            if (best1 < bestDistForTrain[static_cast<size_t>(bestIdx)]) {
                bestDistForTrain[static_cast<size_t>(bestIdx)] = best1;
                bestQueryForTrain[static_cast<size_t>(bestIdx)] = static_cast<int32_t>(qi);
            }
        }
    }

    for (size_t ti = 0; ti < nTrain; ++ti) {
        if (bestQueryForTrain[ti] >= 0) {
            outMatches.emplace_back(bestQueryForTrain[ti], static_cast<int32_t>(ti));
        }
    }
}

LearnResult MosaicEngine::learnTemplateToBuffer(
    const YPlaneConstView& yView,
    std::span<const NormalizedPolygon> polygons,
    std::vector<uint8_t>& outFamTplBytes,
    const std::string& registerTemplateId,
    uint16_t refExifOrientation) {

    const auto tStart = SteadyClock::now();
    LearnResult res{};

    if (!yView.isValid()) {
        res.errorMessage = "Invalid Y-plane buffer or dimensions";
        return res;
    }
    if (polygons.empty()) {
        res.errorMessage = "At least one polygon mask (>= 3 vertices) is required";
        return res;
    }

    std::vector<FamTplPolygonSpan> spans;
    std::vector<FamTplVertex> flatVertices;
    spans.reserve(polygons.size());
    for (const auto& poly : polygons) {
        if (poly.vertices.size() < 3) {
            res.errorMessage = "Polygon mask must have at least 3 vertices";
            return res;
        }
        FamTplPolygonSpan span{};
        span.firstVertex = static_cast<uint32_t>(flatVertices.size());
        span.vertexCount = static_cast<uint32_t>(poly.vertices.size());
        spans.push_back(span);
        for (const auto& v : poly.vertices) {
            flatVertices.push_back(FamTplVertex{
                std::clamp(v.normX, 0.0f, 1.0f),
                std::clamp(v.normY, 0.0f, 1.0f)
            });
        }
    }

    std::vector<cv::KeyPoint> cvKps;
    cv::Mat cvDesc;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    {
        std::unique_lock lock(mutex_);
        cv::Mat workY = impl_->prepareWorkYMat(
            yView.yPtr, yView.width, yView.height, yView.yRowStride,
            true, scaleX, scaleY);
        cv::Mat enhancedY = impl_->enhanceWorkY(workY);

        // Exclude user-drawn mosaic polygons so variable content inside the masked area
        // (e.g. changing ID numbers/names/faces) does not pollute template keypoints
        cv::Mat learnMask(enhancedY.rows, enhancedY.cols, CV_8UC1, cv::Scalar(255));
        std::vector<std::vector<cv::Point>> maskPolys;
        maskPolys.reserve(polygons.size());
        for (const auto& poly : polygons) {
            std::vector<cv::Point> pts;
            pts.reserve(poly.vertices.size());
            for (const auto& v : poly.vertices) {
                const int px = static_cast<int>(
                    std::clamp(v.normX, 0.0f, 1.0f) * static_cast<float>(enhancedY.cols - 1));
                const int py = static_cast<int>(
                    std::clamp(v.normY, 0.0f, 1.0f) * static_cast<float>(enhancedY.rows - 1));
                pts.emplace_back(px, py);
            }
            if (pts.size() >= 3) {
                maskPolys.push_back(std::move(pts));
            }
        }
        if (!maskPolys.empty()) {
            cv::fillPoly(learnMask, maskPolys, cv::Scalar(0));
            impl_->learnOrb->detectAndCompute(enhancedY, learnMask, cvKps, cvDesc);
        }
        // Fallback to full-frame extraction if user masked almost the entire image
        if (cvKps.size() < 40) {
            cvKps.clear();
            cvDesc.release();
            impl_->learnOrb->detectAndCompute(enhancedY, cv::noArray(), cvKps, cvDesc);
        }
    }

    std::vector<FamTplKeypoint> tplKps;
    std::vector<uint8_t> tplDesc;
    if (!cvKps.empty() && !cvDesc.empty() &&
        cvDesc.cols == static_cast<int>(kOrbDescriptorBytes)) {
        const float invW = 1.0f / static_cast<float>(yView.width);
        const float invH = 1.0f / static_cast<float>(yView.height);
        const float invMaxDim = 1.0f / static_cast<float>(std::max(yView.width, yView.height));

        tplKps.resize(cvKps.size());
        tplDesc.resize(cvKps.size() * kOrbDescriptorBytes);

        for (size_t i = 0; i < cvKps.size(); ++i) {
            tplKps[i].normX = std::clamp((cvKps[i].pt.x * scaleX) * invW, 0.0f, 1.0f);
            tplKps[i].normY = std::clamp((cvKps[i].pt.y * scaleY) * invH, 0.0f, 1.0f);
            tplKps[i].size = (cvKps[i].size * std::max(scaleX, scaleY)) * invMaxDim;
            tplKps[i].angleDeg = cvKps[i].angle;
            std::memcpy(
                tplDesc.data() + i * kOrbDescriptorBytes,
                cvDesc.ptr<uint8_t>(static_cast<int>(i)),
                kOrbDescriptorBytes);
        }
    }

    std::string err;
    if (!serializeFamTemplateToBuffer(
            static_cast<uint32_t>(yView.width),
            static_cast<uint32_t>(yView.height),
            tplKps, tplDesc, spans, flatVertices,
            currentEpochMs(), outFamTplBytes, &err, refExifOrientation)) {
        res.errorMessage = err;
        res.elapsedUs = elapsedMicroseconds(tStart);
        return res;
    }

    if (!registerTemplateId.empty()) {
        MappedFamTemplate mapped;
        if (!mapped.loadFromMemoryCopy(outFamTplBytes, false, &err)) {
            res.errorMessage = err;
            res.elapsedUs = elapsedMicroseconds(tStart);
            return res;
        }
        std::unique_lock lock(mutex_);
        impl_->templates[registerTemplateId] = std::move(mapped);
    }

    res.success = true;
    res.extractedKeypoints = static_cast<uint32_t>(tplKps.size());
    res.polygonCount = static_cast<uint16_t>(spans.size());
    res.elapsedUs = elapsedMicroseconds(tStart);
    return res;
}

LearnResult MosaicEngine::learnTemplateToFile(
    const YPlaneConstView& yView,
    std::span<const NormalizedPolygon> polygons,
    const std::string& outputTplPath,
    const std::string& registerTemplateId,
    uint16_t refExifOrientation) {

    const auto tStart = SteadyClock::now();
    std::vector<uint8_t> bytes;
    LearnResult res = learnTemplateToBuffer(yView, polygons, bytes, "", refExifOrientation);
    if (!res.success) {
        return res;
    }

    const std::string tmpPath = outputTplPath + ".tmp";
    const int fd = ::open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        res.success = false;
        res.errorMessage = "Failed opening output FAMTPL02 file: " + outputTplPath;
        return res;
    }
    const bool writeOk = writeAllToFd(fd, bytes.data(), bytes.size());
    if (writeOk) {
        (void)::fsync(fd);
    }
    ::close(fd);
    if (!writeOk || ::rename(tmpPath.c_str(), outputTplPath.c_str()) != 0) {
        ::unlink(tmpPath.c_str());
        res.success = false;
        res.errorMessage = "Failed persisting FAMTPL02 file atomically";
        return res;
    }

    if (!registerTemplateId.empty()) {
        std::string mapErr;
        if (!loadTemplateFromFile(registerTemplateId, outputTplPath, nullptr, &mapErr)) {
            res.success = false;
            res.errorMessage = mapErr;
            return res;
        }
    }

    res.elapsedUs = elapsedMicroseconds(tStart);
    return res;
}

bool MosaicEngine::loadTemplateFromFile(const std::string& templateId,
                                        const std::string& famTplPath,
                                        uint64_t* loadElapsedUs,
                                        std::string* errorOut) {
    const auto tStart = SteadyClock::now();
    MappedFamTemplate mapped;
    if (!mapped.mapFromFile(famTplPath, true, errorOut)) {
        return false;
    }
    {
        std::unique_lock lock(mutex_);
        impl_->templates[templateId] = std::move(mapped);
    }
    if (loadElapsedUs) {
        *loadElapsedUs = elapsedMicroseconds(tStart);
    }
    return true;
}

bool MosaicEngine::loadTemplateFromFd(const std::string& templateId,
                                      int fd,
                                      uint64_t* loadElapsedUs,
                                      std::string* errorOut) {
    const auto tStart = SteadyClock::now();
    MappedFamTemplate mapped;
    if (!mapped.mapFromFd(fd, true, errorOut)) {
        return false;
    }
    {
        std::unique_lock lock(mutex_);
        impl_->templates[templateId] = std::move(mapped);
    }
    if (loadElapsedUs) {
        *loadElapsedUs = elapsedMicroseconds(tStart);
    }
    return true;
}

bool MosaicEngine::unloadTemplate(const std::string& templateId) {
    std::unique_lock lock(mutex_);
    return impl_->templates.erase(templateId) > 0;
}

void MosaicEngine::clearTemplates() {
    std::unique_lock lock(mutex_);
    impl_->templates.clear();
}

size_t MosaicEngine::loadedTemplateCount() const {
    std::shared_lock lock(mutex_);
    return impl_->templates.size();
}

void MosaicEngine::applyPolygonMosaicInPlace(
    const YuvPlaneView& frame,
    std::span<const FamTplVertex> polygonPx,
    int32_t blockSize,
    uint8_t noiseAmplitude,
    uint64_t rngSeed) noexcept {

    if (!frame.isValid() || polygonPx.size() < 3) {
        return;
    }

    const int32_t B = std::clamp((blockSize >= 8 ? blockSize : 16) & ~1, 8, 64);

    float minXf = static_cast<float>(frame.width);
    float minYf = static_cast<float>(frame.height);
    float maxXf = 0.0f;
    float maxYf = 0.0f;
    for (const auto& pt : polygonPx) {
        minXf = std::min(minXf, pt.normX);
        minYf = std::min(minYf, pt.normY);
        maxXf = std::max(maxXf, pt.normX);
        maxYf = std::max(maxYf, pt.normY);
    }

    // 2px protective pad snapped to 2x2 YUV420 alignment
    int32_t roiX0 = std::clamp((static_cast<int32_t>(std::floor(minXf)) - 2) & ~1, 0, frame.width);
    int32_t roiY0 = std::clamp((static_cast<int32_t>(std::floor(minYf)) - 2) & ~1, 0, frame.height);
    int32_t roiX1 = std::clamp(((static_cast<int32_t>(std::ceil(maxXf)) + 3) & ~1), roiX0, frame.width);
    int32_t roiY1 = std::clamp(((static_cast<int32_t>(std::ceil(maxYf)) + 3) & ~1), roiY0, frame.height);

    if (roiX1 - roiX0 < 2 || roiY1 - roiY0 < 2) {
        return;
    }

    uint64_t seedState = rngSeed ? rngSeed : 0xA5A5F00DCAFEBABEULL;
    uint32_t rng32 = static_cast<uint32_t>(splitMix64(seedState));

    for (int32_t by0 = roiY0; by0 < roiY1; by0 += B) {
        const int32_t by1 = std::min(by0 + B, roiY1);
        for (int32_t bx0 = roiX0; bx0 < roiX1; bx0 += B) {
            const int32_t bx1 = std::min(bx0 + B, roiX1);
            if (!blockIntersectsPolygon(
                    static_cast<float>(bx0), static_cast<float>(by0),
                    static_cast<float>(bx1), static_cast<float>(by1),
                    polygonPx)) {
                continue;
            }

            mosaicLumaBlockInPlace(
                frame.yPtr, frame.yRowStride,
                bx0, by0, bx1 - bx0, by1 - by0,
                noiseAmplitude, rng32);

            mosaicChromaBlockInPlace(
                frame, bx0, by0, bx1, by1, rng32);
        }
    }
}

MosaicProcessResult MosaicEngine::processYuvInPlace(
    const YuvPlaneView& frame,
    int32_t overrideBlockSize,
    uint16_t targetExifOrientation) {

    const auto tTotalStart = SteadyClock::now();
    MosaicProcessResult result{};

    if (!frame.isValid()) {
        result.statusMessage = "Invalid YUVPlaneView frame or strides";
        return result;
    }

    MosaicConfig cfg;
    std::vector<cv::KeyPoint> targetKps;
    cv::Mat targetDesc;
    float scaleToFullX = 1.0f;
    float scaleToFullY = 1.0f;
    int32_t workW = frame.width;
    int32_t workH = frame.height;
    bool anyTemplateHasFeatures = false;

    // Step 1: Extract target ORB features ONCE on NEON-downsampled strided Y plane
    {
        const auto tOrbStart = SteadyClock::now();
        std::unique_lock lock(mutex_);
        if (impl_->templates.empty()) {
            result.statusMessage = "No FAMTPL02 templates loaded";
            return result;
        }
        cfg = impl_->config;

        for (const auto& [_, tpl] : impl_->templates) {
            if (tpl.valid() &&
                static_cast<int32_t>(tpl.keypointCount()) >= cfg.minInliers) {
                anyTemplateHasFeatures = true;
                break;
            }
        }

        if (anyTemplateHasFeatures) {
            cv::Mat workY = impl_->prepareWorkYMat(
                frame.yPtr, frame.width, frame.height, frame.yRowStride,
                false, scaleToFullX, scaleToFullY);
            cv::Mat enhancedY = impl_->enhanceWorkY(workY);
            workW = enhancedY.cols;
            workH = enhancedY.rows;
            // Note: cv::ORB uses OpenCV internal buffers so lock guards detector state cleanly
            impl_->targetOrb->detectAndCompute(enhancedY, cv::noArray(), targetKps, targetDesc);
        }
        result.downsampleAndOrbUs = elapsedMicroseconds(tOrbStart);
    }

    result.targetKeypoints = static_cast<uint32_t>(targetKps.size());
    const bool canAttemptHomography =
        anyTemplateHasFeatures &&
        targetKps.size() >= static_cast<size_t>(cfg.minInliers) &&
        !targetDesc.empty();

    if (!canAttemptHomography) {
        result.totalUs = elapsedMicroseconds(tTotalStart);
        result.statusMessage = "Insufficient ORB keypoints on target frame";
        return result;
    }

    const std::span<const uint8_t> targetDescSpan(
        targetDesc.ptr<uint8_t>(0),
        targetKps.size() * kOrbDescriptorBytes);

    // Step 2: Match against all loaded memory-mapped FAMTPL02 templates + USAC_MAGSAC
    const auto tMatchStart = SteadyClock::now();
    std::vector<ProjectedPolygon> allProjected;

    {
        std::shared_lock lock(mutex_);
        result.templatesEvaluated = static_cast<uint32_t>(impl_->templates.size());

        std::vector<std::pair<int32_t, int32_t>> rawMatches;
        std::vector<cv::Point2f> srcPts;
        std::vector<cv::Point2f> dstWorkPts;

        for (const auto& [tplId, tpl] : impl_->templates) {
            if (!tpl.valid()) {
                continue;
            }

            const auto* hdr = tpl.header();
            const auto polySpans = tpl.polygons();
            const auto tplVerts = tpl.vertices();

            if (static_cast<int32_t>(tpl.keypointCount()) >= cfg.minInliers) {
                // Query = target frame descriptors, Train = template descriptors
                matchDescriptorsHammingPopcnt(
                    targetDescSpan,
                    tpl.descriptors(),
                    cfg.loweRatio,
                    cfg.maxHammingDist,
                    rawMatches);

                result.bestMatchCount = std::max(
                    result.bestMatchCount, static_cast<uint32_t>(rawMatches.size()));

                if (static_cast<int32_t>(rawMatches.size()) >= cfg.minInliers) {
                    const float refWorkScaleX = static_cast<float>(hdr->refWidth);
                    const float refWorkScaleY = static_cast<float>(hdr->refHeight);
                    const auto tplKps = tpl.keypoints();

                    srcPts.clear();
                    dstWorkPts.clear();
                    srcPts.reserve(rawMatches.size());
                    dstWorkPts.reserve(rawMatches.size());

                    for (const auto& [qIdx, tIdx] : rawMatches) {
                        const auto& tkp = tplKps[static_cast<size_t>(tIdx)];
                        const auto& qkp = targetKps[static_cast<size_t>(qIdx)];
                        srcPts.emplace_back(tkp.normX * refWorkScaleX, tkp.normY * refWorkScaleY);
                        dstWorkPts.emplace_back(qkp.pt.x, qkp.pt.y);
                    }

                    cv::Mat inlierMask;
                    cv::UsacParams usacParams{};
                    usacParams.sampler = cv::SAMPLING_UNIFORM;
                    usacParams.score = cv::SCORE_METHOD_MAGSAC;
                    usacParams.loMethod = cv::LOCAL_OPTIM_SIGMA;
                    usacParams.maxIterations = cfg.magsacMaxIters;
                    usacParams.confidence = 0.995;
                    usacParams.threshold = cfg.magsacReprojThreshold;
                    usacParams.final_polisher = cv::MAGSAC;

                    cv::Mat H_work = cv::findHomography(srcPts, dstWorkPts, inlierMask, usacParams);
                    if (!H_work.empty()) {
                        uint32_t inliers = 0;
                        for (int i = 0; i < inlierMask.rows; ++i) {
                            if (inlierMask.at<uint8_t>(i, 0) != 0) {
                                ++inliers;
                            }
                        }
                        result.bestInlierCount = std::max(result.bestInlierCount, inliers);

                        if (static_cast<int32_t>(inliers) >= cfg.minInliers &&
                            isHomographyGeometricallyValid(
                                H_work,
                                static_cast<double>(refWorkScaleX),
                                static_cast<double>(refWorkScaleY),
                                static_cast<double>(workW),
                                static_cast<double>(workH))) {
                            // Scale H_work from working pyramid pixels directly into full frame pixels
                            cv::Mat H_full = H_work.clone();
                            H_full.row(0) *= static_cast<double>(scaleToFullX);
                            H_full.row(1) *= static_cast<double>(scaleToFullY);

                            for (const auto& span : polySpans) {
                                std::vector<cv::Point2f> polySrc;
                                polySrc.reserve(span.vertexCount);
                                for (uint32_t vi = 0; vi < span.vertexCount; ++vi) {
                                    const auto& v = tplVerts[span.firstVertex + vi];
                                    polySrc.emplace_back(
                                        v.normX * refWorkScaleX, v.normY * refWorkScaleY);
                                }

                                std::vector<cv::Point2f> polyDst;
                                cv::perspectiveTransform(polySrc, polyDst, H_full);

                                ProjectedPolygon projPoly;
                                projPoly.templateId = tplId;
                                projPoly.pixelVertices.reserve(polyDst.size());
                                for (const auto& p : polyDst) {
                                    projPoly.pixelVertices.push_back(FamTplVertex{p.x, p.y});
                                }
                                allProjected.push_back(std::move(projPoly));
                            }
                            ++result.templatesMatched;
                        }
                    }
                }
            }
        }
    }
    result.matchAndMagsacUs = elapsedMicroseconds(tMatchStart);

    // Step 3: In-place NEON + Xorshift anti-reconstruction mosaic over all projected polygons
    if (!allProjected.empty()) {
        const auto tMosaicStart = SteadyClock::now();
        const int32_t blockSize = overrideBlockSize > 0 ? overrideBlockSize : cfg.mosaicBlockSize;

        // Per-frame high-entropy seed prevents multi-frame noise averaging attacks
        std::random_device rd;
        uint64_t frameSeed = (static_cast<uint64_t>(rd()) << 32) ^
                             static_cast<uint64_t>(SteadyClock::now().time_since_epoch().count());

        for (const auto& poly : allProjected) {
            frameSeed ^= 0x9E3779B97F4A7C15ULL;
            applyPolygonMosaicInPlace(
                frame, poly.pixelVertices, blockSize, cfg.noiseAmplitude, frameSeed);
        }
        result.neonMosaicUs = elapsedMicroseconds(tMosaicStart);
        result.matchedAny = true;
        result.projectedPolygons = std::move(allProjected);
        result.statusMessage = "Matched Homography and applied NEON mosaic in-place";
    } else {
        result.statusMessage = "Target not present in frame";
    }

    result.totalUs = elapsedMicroseconds(tTotalStart);
    return result;
}

} // namespace fam
