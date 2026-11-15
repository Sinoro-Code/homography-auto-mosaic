#pragma once

#include "mosaic_engine.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fam {

/**
 * 64-byte cache/SIMD-aligned reusable planar YUV memory arena.
 * Eliminates per-photo allocations during batch gallery processing and keeps
 * 12MP peak native memory strictly under the 25 MB target (~18 MB YUV420 + ~4 MB JPEG buffer).
 */
class AlignedYuvArena {
public:
    AlignedYuvArena() noexcept = default;
    ~AlignedYuvArena() noexcept;

    AlignedYuvArena(const AlignedYuvArena&) = delete;
    AlignedYuvArena& operator=(const AlignedYuvArena&) = delete;

    AlignedYuvArena(AlignedYuvArena&& other) noexcept;
    AlignedYuvArena& operator=(AlignedYuvArena&& other) noexcept;

    [[nodiscard]] bool ensureLayout(int32_t width, int32_t height,
                                    ChromaSubsampling subsampling) noexcept;

    void release() noexcept;

    [[nodiscard]] YuvPlaneView view() noexcept { return view_; }
    [[nodiscard]] size_t capacityBytes() const noexcept { return capacityBytes_; }

private:
    uint8_t* data_ = nullptr;
    size_t capacityBytes_ = 0;
    YuvPlaneView view_{};
};

struct JpegBatchTelemetry {
    bool ioSuccess = false;
    bool matchedAny = false;
    int32_t width = 0;
    int32_t height = 0;
    uint16_t exifOrientation = 1;
    uint32_t templatesMatched = 0;
    uint32_t bestInliers = 0;
    uint64_t jpegDecodeUs = 0;
    uint64_t homographyAndOrbUs = 0;
    uint64_t neonMosaicUs = 0;
    uint64_t jpegEncodeUs = 0;
    uint64_t totalUs = 0;
    size_t peakBufferBytes = 0;
    std::string message;
};

/**
 * Zero-RGB JPEG <-> YUV420 direct codec built on libjpeg-turbo 3.0+ (WITH_SIMD=ON NEON).
 * Uses tjDecompressToYUVPlanes and tjCompressFromYUVPlanes directly on 64-byte aligned planes.
 */
class JpegYuvCodec {
public:
    JpegYuvCodec();
    ~JpegYuvCodec();

    JpegYuvCodec(const JpegYuvCodec&) = delete;
    JpegYuvCodec& operator=(const JpegYuvCodec&) = delete;

    /**
     * Decodes a JPEG memory buffer directly into 64-byte aligned YUV planes (zero RGB).
     * Also extracts the EXIF Orientation tag (1..8) if present so upright display is preserved
     * without rotating 12MP pixel buffers in CPU memory.
     */
    [[nodiscard]] bool decodeToYuvPlanes(
        std::span<const uint8_t> jpegBytes,
        AlignedYuvArena& outArena,
        uint16_t* outExifOrientation = nullptr,
        std::string* errorOut = nullptr);

    /**
     * Compresses a YUV frame (supporting both planar I420/422/444 and CameraX semi-planar
     * NV12/NV21 via NEON vld2q_u8 de-interleave) directly to JPEG using tjCompressFromYUVPlanes.
     * Injects a privacy-clean minimal EXIF APP1 Orientation segment when exifOrientation > 1.
     */
    [[nodiscard]] bool encodeFromYuvView(
        const YuvPlaneView& frame,
        int quality,
        uint16_t exifOrientation,
        std::vector<uint8_t>& outJpegBytes,
        std::string* errorOut = nullptr);

    /**
     * Encodes a YUV frame directly to an open writable file descriptor (e.g. MediaStore PFD).
     */
    [[nodiscard]] bool encodeYuvViewToFd(
        const YuvPlaneView& frame,
        int outFd,
        int quality,
        uint16_t exifOrientation = 1,
        std::string* errorOut = nullptr);

    /**
     * Ultra-fast zero-Bitmap FD-to-FD pipeline for Android Photo Picker single/batch mode:
     *   inFd -> pread -> tjDecompressToYUVPlanes -> Homography + NEON Mosaic ->
     *   tjCompressFromYUVPlanes -> pwrite(outFd).
     * Reuses internal YUV arena + single compressed scratch buffer to stay < 25 MB peak RAM.
     */
    [[nodiscard]] JpegBatchTelemetry processFdToFd(
        MosaicEngine& engine,
        int inFd,
        int outFd,
        int jpegQuality = 92,
        int32_t overrideBlockSize = 0);

    /**
     * Extracts EXIF Orientation (1..8, default 1) from raw JPEG header markers.
     */
    [[nodiscard]] static uint16_t parseExifOrientation(std::span<const uint8_t> jpegBytes) noexcept;

    /**
     * Converts CameraX clockwise rotation degrees (0, 90, 180, 270) to EXIF Orientation code.
     */
    [[nodiscard]] static uint16_t rotationDegreesToExifOrientation(int32_t rotationDegrees) noexcept;

private:
    void* tjDecompressor_ = nullptr;
    void* tjCompressor_ = nullptr;
    AlignedYuvArena arena_;
    unsigned char* tjOutBuf_ = nullptr;       // Reusable libjpeg-turbo compressed output buffer (~4 MB)
    unsigned long tjOutCapacity_ = 0;
    std::vector<uint8_t> streamFallbackBuf_;  // Used only if input FD is a non-mmapable pipe/stream
    std::vector<uint8_t> uvPlanarScratch_;    // Used only when encoding semi-planar NV12/NV21 CameraX frames
};

} // namespace fam
