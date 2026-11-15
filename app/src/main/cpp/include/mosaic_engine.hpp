#pragma once

#include "famtpl_format.hpp"

#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace fam {

enum class ChromaSubsampling : uint8_t {
    YUV420 = 0, // 2x2 luma per chroma sample (CameraX YUV_420_888: I420 / NV12 / NV21, JPEG 4:2:0)
    YUV422 = 1, // 2x1 luma per chroma sample (JPEG 4:2:2 horizontal subsampling)
    YUV444 = 2, // 1x1 full chroma (JPEG 4:4:4)
    GRAY8  = 3  // Luma only (monochrome JPEG or Y-only buffer)
};

/**
 * Zero-copy descriptor of an in-memory YUV frame with explicit ISP rowStride and uvPixelStride.
 * Supports:
 *  - Planar I420 / YV12: uvPixelStride == 1, arbitrary 64/128-byte padded yRowStride & uvRowStride
 *  - Semi-planar NV12:   uvPixelStride == 2, vPtr == uPtr + 1
 *  - Semi-planar NV21:   uvPixelStride == 2, uPtr == vPtr + 1
 *  - Arbitrary strided HAL3 buffers: uvPixelStride == 1 or 2 with independent uPtr/vPtr
 */
struct YuvPlaneView {
    uint8_t* yPtr = nullptr;
    uint8_t* uPtr = nullptr;
    uint8_t* vPtr = nullptr;
    int32_t width = 0;
    int32_t height = 0;
    int32_t yRowStride = 0;
    int32_t uRowStride = 0;
    int32_t vRowStride = 0;
    int32_t uvPixelStride = 1;
    ChromaSubsampling subsampling = ChromaSubsampling::YUV420;

    [[nodiscard]] bool isValid() const noexcept {
        if (!yPtr || width < 16 || height < 16 || yRowStride < width) {
            return false;
        }
        if (subsampling == ChromaSubsampling::GRAY8) {
            return true;
        }
        if (!uPtr || !vPtr || (uvPixelStride != 1 && uvPixelStride != 2)) {
            return false;
        }
        const int32_t cw = (subsampling == ChromaSubsampling::YUV444) ? width : (width + 1) / 2;
        const int32_t minSpan = (cw - 1) * uvPixelStride + 1;
        return uRowStride >= minSpan && vRowStride >= minSpan;
    }
};

/**
 * Read-only Y-plane view for zero-copy sharpness analysis and template learning.
 */
struct YPlaneConstView {
    const uint8_t* yPtr = nullptr;
    int32_t width = 0;
    int32_t height = 0;
    int32_t yRowStride = 0;

    [[nodiscard]] bool isValid() const noexcept {
        return yPtr != nullptr && width >= 16 && height >= 16 && yRowStride >= width;
    }
};

struct MosaicConfig {
    int32_t learnOrbFeatures = 700;    // Richer feature set during one-time template learning
    int32_t targetOrbFeatures = 500;   // Fast target extraction budget (< 1.5 ms on 640p work pyramid)
    int32_t orbWorkMaxDim = 640;       // Max working dimension for NEON strided Y downsample
    float loweRatio = 0.78f;           // Lowe's 2-NN Hamming ratio test threshold
    int32_t maxHammingDist = 64;       // Hard Hamming distance cutoff out of 256 bits
    int32_t minInliers = 10;           // Minimum USAC_MAGSAC geometric inliers to accept homography
    double magsacReprojThreshold = 3.5;// Reprojection threshold in working-pyramid pixels
    int32_t magsacMaxIters = 1500;     // USAC_MAGSAC iteration cap
    int32_t mosaicBlockSize = 16;      // Luma mosaic cell size (snapped to even >= 8)
    uint8_t noiseAmplitude = 18;       // Xorshift luma noise amplitude for anti-Depix protection
};

struct NormalizedPolygon {
    std::vector<FamTplVertex> vertices; // Normalized [0, 1] coordinates in raw frame space
};

struct LearnResult {
    bool success = false;
    uint32_t extractedKeypoints = 0;
    uint16_t polygonCount = 0;
    uint64_t elapsedUs = 0;
    std::string errorMessage;
};

struct ProjectedPolygon {
    std::string templateId;
    std::vector<FamTplVertex> pixelVertices; // Pixel coordinates in [0, width] x [0, height]
};

struct MosaicProcessResult {
    bool matchedAny = false;
    uint32_t templatesEvaluated = 0;
    uint32_t templatesMatched = 0;
    uint32_t targetKeypoints = 0;
    uint32_t bestMatchCount = 0;
    uint32_t bestInlierCount = 0;
    uint64_t downsampleAndOrbUs = 0;
    uint64_t matchAndMagsacUs = 0;
    uint64_t neonMosaicUs = 0;
    uint64_t totalUs = 0;
    std::vector<ProjectedPolygon> projectedPolygons;
    std::string statusMessage;
};

/**
 * Pure-CPU (ARM NEON SIMD + POPCNT) Homography Auto-Mosaic Engine.
 * Zero GPU / Zero NNAPI / Zero OpenCL / Zero RGB / Zero Bitmap.
 */
class MosaicEngine {
public:
    explicit MosaicEngine(const MosaicConfig& config = MosaicConfig{});
    ~MosaicEngine();

    MosaicEngine(const MosaicEngine&) = delete;
    MosaicEngine& operator=(const MosaicEngine&) = delete;

    void updateConfig(const MosaicConfig& config);
    [[nodiscard]] MosaicConfig getConfig() const;

    /**
     * Computes a real-time Tenengrad/Laplacian sharpness score directly on a strided Y plane
     * using ARM NEON SIMD (< 0.15 ms on 1080p) for CameraX anti-shake gating.
     */
    [[nodiscard]] static float computeSharpnessScore(const YPlaneConstView& yView) noexcept;

    /**
     * Self-learning stage: extracts normalized ORB keypoints & 256-bit descriptors on Y plane
     * and serializes directly to FAMTPL02 binary format (file path or FD).
     */
    [[nodiscard]] LearnResult learnTemplateToFile(
        const YPlaneConstView& yView,
        std::span<const NormalizedPolygon> polygons,
        const std::string& outputTplPath,
        const std::string& registerTemplateId = "");

    [[nodiscard]] LearnResult learnTemplateToBuffer(
        const YPlaneConstView& yView,
        std::span<const NormalizedPolygon> polygons,
        std::vector<uint8_t>& outFamTplBytes,
        const std::string& registerTemplateId = "");

    /**
     * Sub-1ms cold-start template loading via zero-copy mmap (FAMTPL02).
     */
    [[nodiscard]] bool loadTemplateFromFile(const std::string& templateId,
                                            const std::string& famTplPath,
                                            uint64_t* loadElapsedUs = nullptr,
                                            std::string* errorOut = nullptr);

    [[nodiscard]] bool loadTemplateFromFd(const std::string& templateId,
                                          int fd,
                                          uint64_t* loadElapsedUs = nullptr,
                                          std::string* errorOut = nullptr);

    bool unloadTemplate(const std::string& templateId);
    void clearTemplates();
    [[nodiscard]] size_t loadedTemplateCount() const;

    /**
     * Runs single-pass ORB extraction on target Y plane, matches against all active FAMTPL02
     * templates via NEON hardware POPCNT, estimates projective homography via USAC_MAGSAC,
     * projects polygon masks, and applies irreversible NEON + Xorshift mosaic IN-PLACE on YUV.
     */
    [[nodiscard]] MosaicProcessResult processYuvInPlace(
        const YuvPlaneView& frame,
        int32_t overrideBlockSize = 0);

    /**
     * Directly applies irreversible NEON + Xorshift mosaic in-place to explicit pixel-space
     * polygons (also usable for previewing or manual touch-up without RGB conversion).
     */
    static void applyPolygonMosaicInPlace(
        const YuvPlaneView& frame,
        std::span<const FamTplVertex> polygonPx,
        int32_t blockSize,
        uint8_t noiseAmplitude,
        uint64_t rngSeed) noexcept;

    /**
     * Exposed for deterministic unit benchmarking: 256-bit Hamming matcher using NEON vcntq_u8.
     */
    static void matchDescriptorsHammingPopcnt(
        std::span<const uint8_t> queryDescriptors,   // [Q * 32]
        std::span<const uint8_t> trainDescriptors,   // [T * 32]
        float loweRatio,
        int32_t maxDist,
        std::vector<std::pair<int32_t, int32_t>>& outMatches);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    mutable std::shared_mutex mutex_;
};

} // namespace fam
