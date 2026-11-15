#include "jpeg_yuv_codec.hpp"

#include <turbojpeg.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define FAM_CODEC_NEON 1
#else
#define FAM_CODEC_NEON 0
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

namespace fam {

namespace {

using SteadyClock = std::chrono::steady_clock;

[[nodiscard]] inline uint64_t elapsedUs(SteadyClock::time_point start) noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - start).count());
}

[[nodiscard]] inline size_t alignUpSize(size_t val, size_t align) noexcept {
    return (val + align - 1) & ~(align - 1);
}

[[nodiscard]] inline int toTjSubsamp(ChromaSubsampling sub) noexcept {
    switch (sub) {
        case ChromaSubsampling::YUV420: return TJSAMP_420;
        case ChromaSubsampling::YUV422: return TJSAMP_422;
        case ChromaSubsampling::YUV444: return TJSAMP_444;
        case ChromaSubsampling::GRAY8:  return TJSAMP_GRAY;
    }
    return TJSAMP_420;
}

[[nodiscard]] inline bool fromTjSubsamp(int tjSub, ChromaSubsampling& outSub) noexcept {
    switch (tjSub) {
        case TJSAMP_420:  outSub = ChromaSubsampling::YUV420; return true;
        case TJSAMP_422:  outSub = ChromaSubsampling::YUV422; return true;
        case TJSAMP_444:  outSub = ChromaSubsampling::YUV444; return true;
        case TJSAMP_GRAY: outSub = ChromaSubsampling::GRAY8;  return true;
        default:          return false;
    }
}

[[nodiscard]] inline uint16_t readU16(const uint8_t* p, bool littleEndian) noexcept {
    return littleEndian
               ? static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8))
               : static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

[[nodiscard]] inline uint32_t readU32(const uint8_t* p, bool littleEndian) noexcept {
    return littleEndian
               ? (static_cast<uint32_t>(p[0]) |
                  (static_cast<uint32_t>(p[1]) << 8) |
                  (static_cast<uint32_t>(p[2]) << 16) |
                  (static_cast<uint32_t>(p[3]) << 24))
               : ((static_cast<uint32_t>(p[0]) << 24) |
                  (static_cast<uint32_t>(p[1]) << 16) |
                  (static_cast<uint32_t>(p[2]) << 8) |
                  static_cast<uint32_t>(p[3]));
}

/**
 * Builds a minimal 38-byte JEITA Exif 2.32 APP1 segment containing ONLY Tag 0x0112 (Orientation).
 * Preserves upright portrait/landscape display while stripping sensitive GPS/serial EXIF metadata.
 */
[[nodiscard]] std::array<uint8_t, 38> buildMinimalExifOrientationSegment(uint16_t orientation) noexcept {
    const uint8_t orientClamped = static_cast<uint8_t>((orientation >= 1 && orientation <= 8) ? orientation : 1);
    return {
        0xFF, 0xE1,                         // APP1 Marker
        0x00, 0x24,                         // Length = 36 bytes
        'E', 'x', 'i', 'f', 0x00, 0x00,     // Exif header
        'I', 'I',                           // Little-endian TIFF
        0x2A, 0x00,                         // TIFF magic (42)
        0x08, 0x00, 0x00, 0x00,             // Offset to IFD0 = 8
        0x01, 0x00,                         // IFD0 entry count = 1
        0x12, 0x01,                         // Tag 0x0112 (Orientation)
        0x03, 0x00,                         // Type = SHORT (3)
        0x01, 0x00, 0x00, 0x00,             // Count = 1
        orientClamped, 0x00, 0x00, 0x00,    // Orientation value inline
        0x00, 0x00, 0x00, 0x00,             // Next IFD offset = 0
        0x00, 0x00                          // Padding to 36-byte segment length
    };
}

[[nodiscard]] bool readEntireFd(int fd, std::vector<uint8_t>& buffer, size_t& outSize) {
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st) != 0) {
        return false;
    }

    if (st.st_size > 0) {
        outSize = static_cast<size_t>(st.st_size);
        if (buffer.size() < outSize) {
            buffer.resize(outSize);
        }
        size_t totalRead = 0;
        while (totalRead < outSize) {
            const ssize_t n = ::pread(fd, buffer.data() + totalRead, outSize - totalRead,
                                      static_cast<off_t>(totalRead));
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (n == 0) break;
            totalRead += static_cast<size_t>(n);
        }
        outSize = totalRead;
        return outSize >= 4;
    }

    // Fallback for pipe/stream FDs where st_size == 0
    (void)::lseek(fd, 0, SEEK_SET);
    outSize = 0;
    constexpr size_t kChunk = 65536;
    while (true) {
        if (buffer.size() < outSize + kChunk) {
            buffer.resize(outSize + kChunk);
        }
        const ssize_t n = ::read(fd, buffer.data() + outSize, kChunk);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) break;
        outSize += static_cast<size_t>(n);
    }
    return outSize >= 4;
}

[[nodiscard]] bool writeJpegWithOptionalExifToFd(
    int outFd,
    const uint8_t* jpegData,
    size_t jpegBytes,
    uint16_t exifOrientation) noexcept {

    if (outFd < 0 || jpegBytes < 4 || jpegData[0] != 0xFF || jpegData[1] != 0xD8) {
        return false;
    }

    (void)::ftruncate(outFd, 0);
    (void)::lseek(outFd, 0, SEEK_SET);

    auto writeAll = [outFd](const uint8_t* p, size_t len) noexcept -> bool {
        size_t done = 0;
        while (done < len) {
            const ssize_t w = ::write(outFd, p + done, len - done);
            if (w <= 0) {
                if (errno == EINTR) continue;
                return false;
            }
            done += static_cast<size_t>(w);
        }
        return true;
    };

    if (exifOrientation <= 1 || exifOrientation > 8) {
        return writeAll(jpegData, jpegBytes);
    }

    // Zero-copy scatter/gather write: [SOI 2B] + [Minimal EXIF APP1 38B] + [JPEG body]
    const auto app1 = buildMinimalExifOrientationSegment(exifOrientation);
    struct iovec iov[3]{};
    iov[0].iov_base = const_cast<uint8_t*>(jpegData);
    iov[0].iov_len = 2;
    iov[1].iov_base = const_cast<uint8_t*>(app1.data());
    iov[1].iov_len = app1.size();
    iov[2].iov_base = const_cast<uint8_t*>(jpegData + 2);
    iov[2].iov_len = jpegBytes - 2;

    const ssize_t totalExpected = static_cast<ssize_t>(2 + app1.size() + (jpegBytes - 2));
    const ssize_t written = ::writev(outFd, iov, 3);
    if (written == totalExpected) {
        return true;
    }

    // Fallback sequential write if partial writev occurred
    (void)::ftruncate(outFd, 0);
    (void)::lseek(outFd, 0, SEEK_SET);
    return writeAll(jpegData, 2) &&
           writeAll(app1.data(), app1.size()) &&
           writeAll(jpegData + 2, jpegBytes - 2);
}

/**
 * De-interleaves semi-planar NV12 or NV21 UV planes into 64-byte aligned planar U and V slices
 * using ARM NEON vld2q_u8 (zero RGB conversion).
 */
void deinterleaveSemiPlanarUvToPlanar(
    const YuvPlaneView& frame,
    int32_t cw,
    int32_t ch,
    int32_t dstUvStride,
    uint8_t* dstU,
    uint8_t* dstV) noexcept {

    const ptrdiff_t delta = frame.vPtr - frame.uPtr;
    const bool isNv12 = (delta == 1 && frame.uRowStride == frame.vRowStride);
    const bool isNv21 = (delta == -1 && frame.uRowStride == frame.vRowStride);

#if FAM_CODEC_NEON
    if (isNv12 || isNv21) {
        const uint8_t* srcBase = isNv12 ? frame.uPtr : frame.vPtr;
        for (int32_t r = 0; r < ch; ++r) {
            const uint8_t* srcRow = srcBase + static_cast<size_t>(r) * frame.uRowStride;
            uint8_t* uOut = dstU + static_cast<size_t>(r) * dstUvStride;
            uint8_t* vOut = dstV + static_cast<size_t>(r) * dstUvStride;
            int32_t c = 0;
            for (; c + 16 <= cw; c += 16) {
                const uint8x16x2_t uv = vld2q_u8(srcRow + c * 2);
                if (isNv12) {
                    vst1q_u8(uOut + c, uv.val[0]);
                    vst1q_u8(vOut + c, uv.val[1]);
                } else {
                    vst1q_u8(vOut + c, uv.val[0]);
                    vst1q_u8(uOut + c, uv.val[1]);
                }
            }
            for (; c < cw; ++c) {
                if (isNv12) {
                    uOut[c] = srcRow[c * 2];
                    vOut[c] = srcRow[c * 2 + 1];
                } else {
                    vOut[c] = srcRow[c * 2];
                    uOut[c] = srcRow[c * 2 + 1];
                }
            }
        }
        return;
    }
#else
    (void)isNv12;
    (void)isNv21;
#endif

    for (int32_t r = 0; r < ch; ++r) {
        const uint8_t* uSrc = frame.uPtr + static_cast<size_t>(r) * frame.uRowStride;
        const uint8_t* vSrc = frame.vPtr + static_cast<size_t>(r) * frame.vRowStride;
        uint8_t* uOut = dstU + static_cast<size_t>(r) * dstUvStride;
        uint8_t* vOut = dstV + static_cast<size_t>(r) * dstUvStride;
        for (int32_t c = 0; c < cw; ++c) {
            uOut[c] = uSrc[c * frame.uvPixelStride];
            vOut[c] = vSrc[c * frame.uvPixelStride];
        }
    }
}

} // namespace

// ============================================================================
// AlignedYuvArena Implementation (64-byte SIMD + MCU safe padding)
// ============================================================================

AlignedYuvArena::~AlignedYuvArena() noexcept {
    release();
}

AlignedYuvArena::AlignedYuvArena(AlignedYuvArena&& other) noexcept {
    *this = std::move(other);
}

AlignedYuvArena& AlignedYuvArena::operator=(AlignedYuvArena&& other) noexcept {
    if (this != &other) {
        release();
        data_ = other.data_;
        capacityBytes_ = other.capacityBytes_;
        view_ = other.view_;

        other.data_ = nullptr;
        other.capacityBytes_ = 0;
        other.view_ = {};
    }
    return *this;
}

void AlignedYuvArena::release() noexcept {
    if (data_ != nullptr) {
        std::free(data_);
        data_ = nullptr;
    }
    capacityBytes_ = 0;
    view_ = {};
}

bool AlignedYuvArena::ensureLayout(int32_t width, int32_t height,
                                   ChromaSubsampling subsampling) noexcept {
    if (width < 16 || height < 16) {
        return false;
    }

    // Pad rows to 64 bytes (ARM cache line & NEON) and heights to 16 rows (JPEG MCU safe boundary)
    const size_t yStride = alignUpSize(static_cast<size_t>(width), 64);
    const size_t yRowsPadded = alignUpSize(static_cast<size_t>(height), 16);
    const size_t yBytes = alignUpSize(yStride * yRowsPadded, 64);

    size_t uvStride = 0;
    size_t uvBytesEach = 0;
    if (subsampling != ChromaSubsampling::GRAY8) {
        const size_t cw = (subsampling == ChromaSubsampling::YUV444)
                              ? static_cast<size_t>(width)
                              : static_cast<size_t>((width + 1) / 2);
        const size_t ch = (subsampling == ChromaSubsampling::YUV420)
                              ? static_cast<size_t>((height + 1) / 2)
                              : static_cast<size_t>(height);
        uvStride = alignUpSize(cw, 64);
        const size_t uvRowsPadded = alignUpSize(ch, 16);
        uvBytesEach = alignUpSize(uvStride * uvRowsPadded, 64);
    }

    const size_t requiredBytes = yBytes + uvBytesEach * 2;
    if (requiredBytes > capacityBytes_ || data_ == nullptr) {
        void* newMem = nullptr;
        if (::posix_memalign(&newMem, 64, requiredBytes) != 0 || newMem == nullptr) {
            return false;
        }
        std::free(data_);
        data_ = static_cast<uint8_t*>(newMem);
        capacityBytes_ = requiredBytes;
    }

    view_.yPtr = data_;
    view_.width = width;
    view_.height = height;
    view_.yRowStride = static_cast<int32_t>(yStride);
    view_.subsampling = subsampling;
    view_.uvPixelStride = 1;

    if (subsampling == ChromaSubsampling::GRAY8) {
        view_.uPtr = nullptr;
        view_.vPtr = nullptr;
        view_.uRowStride = 0;
        view_.vRowStride = 0;
    } else {
        view_.uPtr = data_ + yBytes;
        view_.vPtr = data_ + yBytes + uvBytesEach;
        view_.uRowStride = static_cast<int32_t>(uvStride);
        view_.vRowStride = static_cast<int32_t>(uvStride);
    }
    return true;
}

// ============================================================================
// JpegYuvCodec Implementation
// ============================================================================

JpegYuvCodec::JpegYuvCodec() {
    tjDecompressor_ = tjInitDecompress();
    tjCompressor_ = tjInitCompress();
}

JpegYuvCodec::~JpegYuvCodec() {
    if (tjOutBuf_) {
        tjFree(tjOutBuf_);
        tjOutBuf_ = nullptr;
        tjOutCapacity_ = 0;
    }
    if (tjDecompressor_) {
        tjDestroy(static_cast<tjhandle>(tjDecompressor_));
        tjDecompressor_ = nullptr;
    }
    if (tjCompressor_) {
        tjDestroy(static_cast<tjhandle>(tjCompressor_));
        tjCompressor_ = nullptr;
    }
}

uint16_t JpegYuvCodec::parseExifOrientation(std::span<const uint8_t> jpegBytes) noexcept {
    if (jpegBytes.size() < 14 || jpegBytes[0] != 0xFF || jpegBytes[1] != 0xD8) {
        return 1;
    }

    size_t pos = 2;
    while (pos + 4 <= jpegBytes.size()) {
        if (jpegBytes[pos] != 0xFF) {
            ++pos;
            continue;
        }
        while (pos < jpegBytes.size() && jpegBytes[pos] == 0xFF) {
            ++pos;
        }
        if (pos >= jpegBytes.size()) {
            break;
        }
        const uint8_t marker = jpegBytes[pos++];
        if (marker == 0xDA || marker == 0xD9) {
            break; // Start of Scan or End of Image
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue;
        }
        if (pos + 2 > jpegBytes.size()) {
            break;
        }

        const uint16_t segLen = static_cast<uint16_t>(
            (static_cast<uint16_t>(jpegBytes[pos]) << 8) | jpegBytes[pos + 1]);
        if (segLen < 2 || pos + segLen > jpegBytes.size()) {
            break;
        }

        if (marker == 0xE1 && segLen >= 16) {
            const uint8_t* payload = jpegBytes.data() + pos + 2;
            const size_t payloadLen = segLen - 2;
            if (std::memcmp(payload, "Exif\0\0", 6) == 0) {
                const uint8_t* tiff = payload + 6;
                const size_t tiffLen = payloadLen - 6;
                if (tiffLen >= 8) {
                    const bool le = (tiff[0] == 'I' && tiff[1] == 'I');
                    const bool be = (tiff[0] == 'M' && tiff[1] == 'M');
                    if ((le || be) && readU16(tiff + 2, le) == 0x002A) {
                        const uint32_t ifdOffset = readU32(tiff + 4, le);
                        if (ifdOffset + 2 <= tiffLen) {
                            const uint16_t numEntries = readU16(tiff + ifdOffset, le);
                            size_t entryPos = ifdOffset + 2;
                            for (uint16_t i = 0; i < numEntries && entryPos + 12 <= tiffLen;
                                 ++i, entryPos += 12) {
                                const uint16_t tag = readU16(tiff + entryPos, le);
                                if (tag == 0x0112) {
                                    const uint16_t val = readU16(tiff + entryPos + 8, le);
                                    return (val >= 1 && val <= 8) ? val : 1;
                                }
                            }
                        }
                    }
                }
            }
        }
        pos += segLen;
    }
    return 1;
}

uint16_t JpegYuvCodec::rotationDegreesToExifOrientation(int32_t rotationDegrees) noexcept {
    const int32_t norm = ((rotationDegrees % 360) + 360) % 360;
    switch (norm) {
        case 90:  return 6; // Rotate 90 CW
        case 180: return 3; // Rotate 180
        case 270: return 8; // Rotate 270 CW
        default:  return 1; // Normal upright
    }
}

bool JpegYuvCodec::decodeToYuvPlanes(
    std::span<const uint8_t> jpegBytes,
    AlignedYuvArena& outArena,
    uint16_t* outExifOrientation,
    std::string* errorOut) {

    if (!tjDecompressor_ || jpegBytes.size() < 4) {
        if (errorOut) *errorOut = "Invalid TurboJPEG decompressor or empty JPEG input";
        return false;
    }

    if (outExifOrientation) {
        *outExifOrientation = parseExifOrientation(jpegBytes);
    }

    int width = 0;
    int height = 0;
    int jpegSubsamp = TJSAMP_420;
    int jpegColorspace = TJCS_YCbCr;
    if (tjDecompressHeader3(
            static_cast<tjhandle>(tjDecompressor_),
            jpegBytes.data(),
            static_cast<unsigned long>(jpegBytes.size()),
            &width, &height, &jpegSubsamp, &jpegColorspace) != 0) {
        if (errorOut) {
            *errorOut = std::string("tjDecompressHeader3 failed: ") +
                        tjGetErrorStr2(static_cast<tjhandle>(tjDecompressor_));
        }
        return false;
    }

    ChromaSubsampling subsampling = ChromaSubsampling::YUV420;
    if (!fromTjSubsamp(jpegSubsamp, subsampling)) {
        if (errorOut) *errorOut = "Unsupported JPEG chroma subsampling mode";
        return false;
    }

    if (!outArena.ensureLayout(width, height, subsampling)) {
        if (errorOut) *errorOut = "Failed allocating 64-byte aligned YUV plane arena";
        return false;
    }

    const YuvPlaneView v = outArena.view();
    unsigned char* dstPlanes[3] = {v.yPtr, v.uPtr, v.vPtr};
    int strides[3] = {v.yRowStride, v.uRowStride, v.vRowStride};

    if (tjDecompressToYUVPlanes(
            static_cast<tjhandle>(tjDecompressor_),
            jpegBytes.data(),
            static_cast<unsigned long>(jpegBytes.size()),
            dstPlanes,
            width,
            strides,
            height,
            TJFLAG_FASTDCT | TJFLAG_FASTUPSAMPLE) != 0) {
        if (errorOut) {
            *errorOut = std::string("tjDecompressToYUVPlanes failed: ") +
                        tjGetErrorStr2(static_cast<tjhandle>(tjDecompressor_));
        }
        return false;
    }
    return true;
}

bool JpegYuvCodec::encodeFromYuvView(
    const YuvPlaneView& frame,
    int quality,
    uint16_t exifOrientation,
    std::vector<uint8_t>& outJpegBytes,
    std::string* errorOut) {

    if (!tjCompressor_ || !frame.isValid()) {
        if (errorOut) *errorOut = "Invalid TurboJPEG compressor or YuvPlaneView";
        return false;
    }

    const int clampedQual = std::clamp(quality, 50, 100);
    const int tjSubsamp = toTjSubsamp(frame.subsampling);

    const unsigned char* srcPlanes[3] = {frame.yPtr, nullptr, nullptr};
    int strides[3] = {frame.yRowStride, 0, 0};

    if (frame.subsampling != ChromaSubsampling::GRAY8) {
        if (frame.uvPixelStride == 1) {
            srcPlanes[1] = frame.uPtr;
            srcPlanes[2] = frame.vPtr;
            strides[1] = frame.uRowStride;
            strides[2] = frame.vRowStride;
        } else {
            // CameraX semi-planar NV12/NV21 -> NEON vld2q_u8 de-interleave into planar UV scratch
            const int32_t cw = (frame.subsampling == ChromaSubsampling::YUV444)
                                   ? frame.width
                                   : (frame.width + 1) / 2;
            const int32_t ch = (frame.subsampling == ChromaSubsampling::YUV420)
                                   ? (frame.height + 1) / 2
                                   : frame.height;
            const int32_t uvStride = static_cast<int32_t>(alignUpSize(static_cast<size_t>(cw), 64));
            const size_t planeBytes = static_cast<size_t>(uvStride) *
                                      alignUpSize(static_cast<size_t>(ch), 16);
            uvPlanarScratch_.resize(planeBytes * 2);

            uint8_t* uPlanar = uvPlanarScratch_.data();
            uint8_t* vPlanar = uvPlanarScratch_.data() + planeBytes;
            deinterleaveSemiPlanarUvToPlanar(frame, cw, ch, uvStride, uPlanar, vPlanar);

            srcPlanes[1] = uPlanar;
            srcPlanes[2] = vPlanar;
            strides[1] = uvStride;
            strides[2] = uvStride;
        }
    }

    constexpr unsigned long kInitialOutBufBytes = 3UL * 1024UL * 1024UL;
    if (!tjOutBuf_) {
        tjOutBuf_ = tjAlloc(static_cast<int>(kInitialOutBufBytes));
        tjOutCapacity_ = tjOutBuf_ ? kInitialOutBufBytes : 0;
    }

    unsigned long jpegSize = tjOutCapacity_;
    if (tjCompressFromYUVPlanes(
            static_cast<tjhandle>(tjCompressor_),
            srcPlanes,
            frame.width,
            strides,
            frame.height,
            tjSubsamp,
            &tjOutBuf_,
            &jpegSize,
            clampedQual,
            TJFLAG_FASTDCT) != 0) {
        if (errorOut) {
            *errorOut = std::string("tjCompressFromYUVPlanes failed: ") +
                        tjGetErrorStr2(static_cast<tjhandle>(tjCompressor_));
        }
        return false;
    }
    tjOutCapacity_ = std::max(tjOutCapacity_, jpegSize);

    if (exifOrientation >= 2 && exifOrientation <= 8 && jpegSize >= 4) {
        const auto app1 = buildMinimalExifOrientationSegment(exifOrientation);
        outJpegBytes.resize(jpegSize + app1.size());
        outJpegBytes[0] = 0xFF;
        outJpegBytes[1] = 0xD8;
        std::memcpy(outJpegBytes.data() + 2, app1.data(), app1.size());
        std::memcpy(outJpegBytes.data() + 2 + app1.size(), tjOutBuf_ + 2, jpegSize - 2);
    } else {
        outJpegBytes.assign(tjOutBuf_, tjOutBuf_ + jpegSize);
    }
    return true;
}

bool JpegYuvCodec::encodeYuvViewToFd(
    const YuvPlaneView& frame,
    int outFd,
    int quality,
    uint16_t exifOrientation,
    std::string* errorOut) {

    if (!tjCompressor_ || !frame.isValid() || outFd < 0) {
        if (errorOut) *errorOut = "Invalid compressor, frame, or output FD";
        return false;
    }

    const int clampedQual = std::clamp(quality, 50, 100);
    const int tjSubsamp = toTjSubsamp(frame.subsampling);

    const unsigned char* srcPlanes[3] = {frame.yPtr, nullptr, nullptr};
    int strides[3] = {frame.yRowStride, 0, 0};

    if (frame.subsampling != ChromaSubsampling::GRAY8) {
        if (frame.uvPixelStride == 1) {
            srcPlanes[1] = frame.uPtr;
            srcPlanes[2] = frame.vPtr;
            strides[1] = frame.uRowStride;
            strides[2] = frame.vRowStride;
        } else {
            const int32_t cw = (frame.subsampling == ChromaSubsampling::YUV444)
                                   ? frame.width
                                   : (frame.width + 1) / 2;
            const int32_t ch = (frame.subsampling == ChromaSubsampling::YUV420)
                                   ? (frame.height + 1) / 2
                                   : frame.height;
            const int32_t uvStride = static_cast<int32_t>(alignUpSize(static_cast<size_t>(cw), 64));
            const size_t planeBytes = static_cast<size_t>(uvStride) *
                                      alignUpSize(static_cast<size_t>(ch), 16);
            uvPlanarScratch_.resize(planeBytes * 2);

            uint8_t* uPlanar = uvPlanarScratch_.data();
            uint8_t* vPlanar = uvPlanarScratch_.data() + planeBytes;
            deinterleaveSemiPlanarUvToPlanar(frame, cw, ch, uvStride, uPlanar, vPlanar);

            srcPlanes[1] = uPlanar;
            srcPlanes[2] = vPlanar;
            strides[1] = uvStride;
            strides[2] = uvStride;
        }
    }

    constexpr unsigned long kInitialOutBufBytes = 3UL * 1024UL * 1024UL;
    if (!tjOutBuf_) {
        tjOutBuf_ = tjAlloc(static_cast<int>(kInitialOutBufBytes));
        tjOutCapacity_ = tjOutBuf_ ? kInitialOutBufBytes : 0;
    }

    unsigned long jpegSize = tjOutCapacity_;
    if (tjCompressFromYUVPlanes(
            static_cast<tjhandle>(tjCompressor_),
            srcPlanes,
            frame.width,
            strides,
            frame.height,
            tjSubsamp,
            &tjOutBuf_,
            &jpegSize,
            clampedQual,
            TJFLAG_FASTDCT) != 0) {
        if (errorOut) {
            *errorOut = std::string("tjCompressFromYUVPlanes failed: ") +
                        tjGetErrorStr2(static_cast<tjhandle>(tjCompressor_));
        }
        return false;
    }
    tjOutCapacity_ = std::max(tjOutCapacity_, jpegSize);

    if (!writeJpegWithOptionalExifToFd(outFd, tjOutBuf_, jpegSize, exifOrientation)) {
        if (errorOut) *errorOut = "Failed writing compressed JPEG bytes to output FD";
        return false;
    }
    return true;
}

JpegBatchTelemetry JpegYuvCodec::processFdToFd(
    MosaicEngine& engine,
    int inFd,
    int outFd,
    int jpegQuality,
    int32_t overrideBlockSize) {

    const auto tTotal = SteadyClock::now();
    JpegBatchTelemetry tel{};

    // Step 1: Zero-copy mmap source JPEG FD (fallback to stream read only if non-mmapable)
    const auto tDec = SteadyClock::now();
    struct stat st{};
    void* mappedIn = MAP_FAILED;
    size_t inputBytes = 0;
    const uint8_t* inputPtr = nullptr;

    if (inFd >= 0 && ::fstat(inFd, &st) == 0 && st.st_size >= 4) {
        inputBytes = static_cast<size_t>(st.st_size);
        mappedIn = ::mmap(nullptr, inputBytes, PROT_READ, MAP_PRIVATE, inFd, 0);
        if (mappedIn != MAP_FAILED) {
            (void)::madvise(mappedIn, inputBytes, MADV_SEQUENTIAL);
            inputPtr = static_cast<const uint8_t*>(mappedIn);
        }
    }

    if (!inputPtr) {
        if (!readEntireFd(inFd, streamFallbackBuf_, inputBytes)) {
            tel.message = "Failed reading source JPEG FD";
            return tel;
        }
        inputPtr = streamFallbackBuf_.data();
    }

    std::string err;
    uint16_t exifOrientation = 1;
    const bool decOk = decodeToYuvPlanes({inputPtr, inputBytes}, arena_, &exifOrientation, &err);
    if (mappedIn != MAP_FAILED) {
        ::munmap(mappedIn, inputBytes);
    }
    if (!decOk) {
        tel.message = err;
        return tel;
    }
    tel.jpegDecodeUs = elapsedUs(tDec);
    tel.exifOrientation = exifOrientation;

    YuvPlaneView view = arena_.view();
    tel.width = view.width;
    tel.height = view.height;

    // Step 2: Run Homography Auto-Mosaic IN-PLACE on decoded YUV planes
    const MosaicProcessResult proc = engine.processYuvInPlace(view, overrideBlockSize);
    tel.matchedAny = proc.matchedAny;
    tel.templatesMatched = proc.templatesMatched;
    tel.bestInliers = proc.bestInlierCount;
    tel.homographyAndOrbUs = proc.downsampleAndOrbUs + proc.matchAndMagsacUs;
    tel.neonMosaicUs = proc.neonMosaicUs;

    // Step 3: Re-encode YUV planes directly using reusable ~3MB tjOutBuf_ (keeps peak RAM < 25 MB!)
    const auto tEnc = SteadyClock::now();
    if (!encodeYuvViewToFd(view, outFd, jpegQuality, exifOrientation, &err)) {
        tel.message = err;
        return tel;
    }
    tel.jpegEncodeUs = elapsedUs(tEnc);

    tel.ioSuccess = true;
    tel.peakBufferBytes = arena_.capacityBytes() + static_cast<size_t>(tjOutCapacity_) +
                          streamFallbackBuf_.capacity() + uvPlanarScratch_.capacity();
    tel.totalUs = elapsedUs(tTotal);
    tel.message = proc.statusMessage;
    return tel;
}

} // namespace fam
