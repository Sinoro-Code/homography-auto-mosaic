#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace fam {

inline constexpr char kFamTplMagic[8] = {'F', 'A', 'M', 'T', 'P', 'L', '0', '2'};
inline constexpr uint16_t kFamTplVersion = 0x0002;
inline constexpr size_t kOrbDescriptorBytes = 32; // 256-bit ORB descriptor
inline constexpr uint32_t kMaxTemplateKeypoints = 8192;
inline constexpr uint16_t kMaxTemplatePolygons = 64;
inline constexpr uint16_t kMaxTemplateVertices = 4096;

/**
 * FAMTPL02 64-byte cache-line-aligned binary header.
 *
 * Layout in file / mmap buffer (Little-Endian):
 *   [0 .. 63]                  FamTplHeader (64 bytes)
 *   [descriptorOffset .. ]     uint8_t descriptors[keypointCount * 32] (32-byte aligned)
 *   [keypointOffset .. ]       FamTplKeypoint keypoints[keypointCount] (16-byte aligned)
 *   [polygonOffset .. ]        FamTplPolygonSpan polygons[polygonCount] (8-byte aligned)
 *   [vertexOffset .. ]         FamTplVertex vertices[totalVertexCount] (8-byte aligned)
 */
struct alignas(8) FamTplHeader {
    char magic[8];              // [0..7]   "FAMTPL02"
    uint16_t version;           // [8..9]   0x0002
    uint16_t flags;             // [10..11] Feature flags (bit 0: multi-polygon)
    uint32_t refWidth;          // [12..15] Reference image width in pixels
    uint32_t refHeight;         // [16..19] Reference image height in pixels
    uint32_t keypointCount;     // [20..23] Number of ORB features (N)
    uint16_t polygonCount;      // [24..25] Number of masked polygons (P >= 1)
    uint16_t totalVertexCount;  // [26..27] Total polygon vertices across all polygons (V >= 3)
    uint32_t descriptorOffset;  // [28..31] Byte offset to descriptors (always 64)
    uint32_t keypointOffset;    // [32..35] Byte offset to FamTplKeypoint[N]
    uint32_t polygonOffset;     // [36..39] Byte offset to FamTplPolygonSpan[P]
    uint32_t vertexOffset;      // [40..43] Byte offset to FamTplVertex[V]
    uint32_t payloadBytes;      // [44..47] Total payload size after 64-byte header
    uint64_t checksum64;        // [48..55] FNV-1a 64-bit hash over [64 .. 64 + payloadBytes)
    uint64_t createdEpochMs;    // [56..63] Creation UTC timestamp in milliseconds
};
static_assert(sizeof(FamTplHeader) == 64, "FamTplHeader must occupy exactly one 64-byte cache line");

struct alignas(8) FamTplKeypoint {
    float normX;    // Normalized X in [0.0, 1.0] relative to refWidth
    float normY;    // Normalized Y in [0.0, 1.0] relative to refHeight
    float size;     // Normalized patch diameter relative to max(refWidth, refHeight)
    float angleDeg; // Orientation in degrees [0, 360)
};
static_assert(sizeof(FamTplKeypoint) == 16, "FamTplKeypoint must be 16 bytes");

struct alignas(4) FamTplPolygonSpan {
    uint32_t firstVertex; // Start index in vertices[]
    uint32_t vertexCount; // Number of vertices in this closed polygon (>= 3)
};
static_assert(sizeof(FamTplPolygonSpan) == 8, "FamTplPolygonSpan must be 8 bytes");

struct alignas(4) FamTplVertex {
    float normX; // Normalized polygon X in [0.0, 1.0]
    float normY; // Normalized polygon Y in [0.0, 1.0]
};
static_assert(sizeof(FamTplVertex) == 8, "FamTplVertex must be 8 bytes");

/**
 * Fast 64-bit FNV-1a checksum for sub-millisecond template integrity verification.
 */
[[nodiscard]] inline uint64_t computeFnv1a64(const uint8_t* data, size_t len) noexcept {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        hash ^= static_cast<uint64_t>(data[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

/**
 * Zero-copy memory-mapped FAMTPL02 template handle for Sub-1ms cold-start loading.
 * Directly maps file pages read-only without decoding reference images or recomputing ORB.
 */
class MappedFamTemplate {
public:
    MappedFamTemplate() noexcept = default;
    ~MappedFamTemplate() noexcept;

    MappedFamTemplate(const MappedFamTemplate&) = delete;
    MappedFamTemplate& operator=(const MappedFamTemplate&) = delete;

    MappedFamTemplate(MappedFamTemplate&& other) noexcept;
    MappedFamTemplate& operator=(MappedFamTemplate&& other) noexcept;

    [[nodiscard]] bool mapFromFile(const std::string& path, bool verifyChecksum = true,
                                   std::string* errorOut = nullptr) noexcept;
    [[nodiscard]] bool mapFromFd(int fd, bool verifyChecksum = true,
                                 std::string* errorOut = nullptr) noexcept;
    [[nodiscard]] bool loadFromMemoryCopy(std::span<const uint8_t> bytes, bool verifyChecksum = true,
                                          std::string* errorOut = nullptr);

    void reset() noexcept;

    [[nodiscard]] bool valid() const noexcept { return header_ != nullptr; }
    [[nodiscard]] const FamTplHeader* header() const noexcept { return header_; }
    [[nodiscard]] uint32_t keypointCount() const noexcept {
        return header_ ? header_->keypointCount : 0;
    }
    [[nodiscard]] uint16_t polygonCount() const noexcept {
        return header_ ? header_->polygonCount : 0;
    }

    [[nodiscard]] std::span<const uint8_t> descriptors() const noexcept { return descriptors_; }
    [[nodiscard]] std::span<const FamTplKeypoint> keypoints() const noexcept { return keypoints_; }
    [[nodiscard]] std::span<const FamTplPolygonSpan> polygons() const noexcept { return polygons_; }
    [[nodiscard]] std::span<const FamTplVertex> vertices() const noexcept { return vertices_; }

private:
    [[nodiscard]] bool bindAndValidate(const uint8_t* base, size_t sizeBytes, bool verifyChecksum,
                                       std::string* errorOut) noexcept;

    void* mappedAddr_ = nullptr;
    size_t mappedBytes_ = 0;
    std::vector<uint8_t> ownedCopy_; // Used only if loaded via loadFromMemoryCopy()

    const FamTplHeader* header_ = nullptr;
    std::span<const uint8_t> descriptors_;
    std::span<const FamTplKeypoint> keypoints_;
    std::span<const FamTplPolygonSpan> polygons_;
    std::span<const FamTplVertex> vertices_;
};

/**
 * Encodes reference EXIF Orientation (1..8) into FamTplHeader::flags bits [8..11].
 */
[[nodiscard]] constexpr uint16_t encodeFamTplFlags(
    bool multiPolygon, uint16_t refExifOrientation = 1) noexcept {
    const uint16_t orient = (refExifOrientation >= 1 && refExifOrientation <= 8)
                                ? refExifOrientation
                                : 1;
    return static_cast<uint16_t>((multiPolygon ? 0x0001U : 0x0000U) |
                                 ((orient & 0x000FU) << 8U));
}

/**
 * Extracts reference EXIF Orientation (1..8, defaulting to 1 for legacy templates) from flags.
 */
[[nodiscard]] constexpr uint16_t decodeFamTplExifOrientation(uint16_t flags) noexcept {
    const uint16_t orient = static_cast<uint16_t>((flags >> 8U) & 0x000FU);
    return (orient >= 1 && orient <= 8) ? orient : 1;
}

/**
 * Maps raw unrotated YUV plane normalized coordinates (rx, ry) in [0, 1]^2
 * into upright viewer coordinates (ux, uy) in [0, 1]^2 according to EXIF Orientation (1..8).
 */
[[nodiscard]] constexpr FamTplVertex rawToUprightNormalized(
    float rx, float ry, uint16_t exifOrientation) noexcept {
    switch (exifOrientation) {
        case 2:  return {1.0f - rx, ry};
        case 3:  return {1.0f - rx, 1.0f - ry};
        case 4:  return {rx, 1.0f - ry};
        case 5:  return {ry, rx};
        case 6:  return {1.0f - ry, rx};
        case 7:  return {1.0f - ry, 1.0f - rx};
        case 8:  return {ry, 1.0f - rx};
        default: return {rx, ry};
    }
}

/**
 * Maps upright viewer normalized coordinates (ux, uy) in [0, 1]^2 (as drawn by user on photo)
 * back into raw unrotated YUV plane normalized coordinates (rx, ry) in [0, 1]^2.
 */
[[nodiscard]] constexpr FamTplVertex uprightToRawNormalized(
    float ux, float uy, uint16_t exifOrientation) noexcept {
    switch (exifOrientation) {
        case 2:  return {1.0f - ux, uy};
        case 3:  return {1.0f - ux, 1.0f - uy};
        case 4:  return {ux, 1.0f - uy};
        case 5:  return {uy, ux};
        case 6:  return {uy, 1.0f - ux};
        case 7:  return {1.0f - uy, 1.0f - ux};
        case 8:  return {1.0f - uy, ux};
        default: return {ux, uy};
    }
}

/**
 * Serializes normalized ORB keypoints, 256-bit descriptors, and normalized polygons
 * into a FAMTPL02 byte vector or directly to a file/FD.
 */
[[nodiscard]] bool serializeFamTemplateToBuffer(
    uint32_t refWidth,
    uint32_t refHeight,
    std::span<const FamTplKeypoint> keypoints,
    std::span<const uint8_t> descriptors,
    std::span<const FamTplPolygonSpan> polygons,
    std::span<const FamTplVertex> vertices,
    uint64_t createdEpochMs,
    std::vector<uint8_t>& outBytes,
    std::string* errorOut = nullptr,
    uint16_t refExifOrientation = 1);

[[nodiscard]] bool writeFamTemplateToFile(
    const std::string& path,
    uint32_t refWidth,
    uint32_t refHeight,
    std::span<const FamTplKeypoint> keypoints,
    std::span<const uint8_t> descriptors,
    std::span<const FamTplPolygonSpan> polygons,
    std::span<const FamTplVertex> vertices,
    uint64_t createdEpochMs,
    std::string* errorOut = nullptr,
    uint16_t refExifOrientation = 1);

[[nodiscard]] bool writeFamTemplateToFd(
    int fd,
    uint32_t refWidth,
    uint32_t refHeight,
    std::span<const FamTplKeypoint> keypoints,
    std::span<const uint8_t> descriptors,
    std::span<const FamTplPolygonSpan> polygons,
    std::span<const FamTplVertex> vertices,
    uint64_t createdEpochMs,
    std::string* errorOut = nullptr,
    uint16_t refExifOrientation = 1);

} // namespace fam
