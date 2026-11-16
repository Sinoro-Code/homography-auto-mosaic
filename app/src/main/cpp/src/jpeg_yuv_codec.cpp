#include "jpeg_yuv_codec.hpp"

#include <turbojpeg.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define FAM_CODEC_NEON 1
#else
#define FAM_CODEC_NEON 0
#endif

#if defined(__ANDROID__)
#include <dlfcn.h>
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
        const size_t expected = static_cast<size_t>(st.st_size);
        if (buffer.size() < expected) {
            buffer.resize(expected);
        }
        size_t totalRead = 0;
        bool preadFailed = false;
        while (totalRead < expected) {
            const ssize_t n = ::pread(fd, buffer.data() + totalRead, expected - totalRead,
                                      static_cast<off_t>(totalRead));
            if (n < 0) {
                if (errno == EINTR) continue;
                preadFailed = true;
                break;
            }
            if (n == 0) break;
            totalRead += static_cast<size_t>(n);
        }
        if (!preadFailed && totalRead >= 4) {
            outSize = totalRead;
            return true;
        }
    }

    // Fallback for pipe/stream/RedactingFileDescriptor/cloud ContentProvider FDs
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

/**
 * RAII zero-copy FD mapper with automatic fallback to buffered stream read.
 * Rejects anomalous all-zero virtual mappings and guarantees mapping lifetime
 * across multi-tier decoders (libjpeg-turbo and NDK AImageDecoder).
 */
struct MappedOrBufferedInput {
    void* mappedAddr = MAP_FAILED;
    size_t size = 0;
    const uint8_t* ptr = nullptr;

    MappedOrBufferedInput() noexcept = default;
    ~MappedOrBufferedInput() noexcept {
        if (mappedAddr != MAP_FAILED && mappedAddr != nullptr && size > 0) {
            ::munmap(mappedAddr, size);
        }
    }

    MappedOrBufferedInput(const MappedOrBufferedInput&) = delete;
    MappedOrBufferedInput& operator=(const MappedOrBufferedInput&) = delete;

    [[nodiscard]] bool load(int fd, std::vector<uint8_t>& fallbackBuf, bool sequentialAdvise = false) {
        if (fd < 0) {
            return false;
        }
        struct stat st{};
        if (::fstat(fd, &st) == 0 && st.st_size >= 4) {
            const size_t candidateSize = static_cast<size_t>(st.st_size);
            void* m = ::mmap(nullptr, candidateSize, PROT_READ, MAP_PRIVATE, fd, 0);
            if (m != MAP_FAILED && m != nullptr) {
                const auto* bytes = static_cast<const uint8_t*>(m);
                bool anyNonZero = false;
                const size_t probeLen = std::min<size_t>(candidateSize, 16);
                for (size_t i = 0; i < probeLen; ++i) {
                    if (bytes[i] != 0) {
                        anyNonZero = true;
                        break;
                    }
                }
                if (anyNonZero) {
                    if (sequentialAdvise) {
                        (void)::madvise(m, candidateSize, MADV_SEQUENTIAL);
                    }
                    mappedAddr = m;
                    size = candidateSize;
                    ptr = bytes;
                    return true;
                }
                ::munmap(m, candidateSize);
            }
        }

        if (!readEntireFd(fd, fallbackBuf, size)) {
            size = 0;
            ptr = nullptr;
            return false;
        }
        ptr = fallbackBuf.data();
        return true;
    }

    [[nodiscard]] std::span<const uint8_t> span() const noexcept {
        return {ptr, size};
    }
};

/**
 * Converts in-place 4-byte CMYK pixels (inverted Adobe/JPEG convention where 255 = no ink)
 * into 4-byte RGBA_8888 pixels.
 */
void convertCmykToRgbaInPlace(uint8_t* buf, size_t pixelCount) noexcept {
    for (size_t i = 0; i < pixelCount; ++i) {
        uint8_t* p = buf + i * 4U;
        const uint32_t c = p[0];
        const uint32_t m = p[1];
        const uint32_t y = p[2];
        const uint32_t k = p[3];
        p[0] = static_cast<uint8_t>((c * k + 127U) / 255U);
        p[1] = static_cast<uint8_t>((m * k + 127U) / 255U);
        p[2] = static_cast<uint8_t>((y * k + 127U) / 255U);
        p[3] = 255U;
    }
}

/**
 * Overflow-safe ARM NEON SIMD full-range JFIF BT.601 RGBA_8888 -> planar YUV420 converter.
 * Used when decoding HEIC/HEIF, PNG, WebP, AVIF, RAW, or non-standard subsampling JPEGs (4:4:0/4:1:1/CMYK).
 *
 * Full-range BT.601 integer equations (exact neutral gray R=G=B -> U=128, V=128, zero int16 overflow):
 *   Y = ( 77*R + 150*G +  29*B + 128) >> 8
 *   U = ((-43*R -  84*G + 127*B + 128) >> 8) + 128
 *   V = ((127*R - 106*G -  21*B + 128) >> 8) + 128
 */
[[nodiscard]] bool rgbaToYuv420Arena(
    const uint8_t* rgba,
    size_t rgbaStride,
    int32_t width,
    int32_t height,
    AlignedYuvArena& outArena) noexcept {

    if (!rgba || width < 16 || height < 16 || rgbaStride < static_cast<size_t>(width) * 4U) {
        return false;
    }
    if (!outArena.ensureLayout(width, height, ChromaSubsampling::YUV420)) {
        return false;
    }

    const YuvPlaneView view = outArena.view();

#if FAM_CODEC_NEON
    const uint8x8_t kYR = vdup_n_u8(77);
    const uint8x8_t kYG = vdup_n_u8(150);
    const uint8x8_t kYB = vdup_n_u8(29);
    const uint16x8_t kYBias = vdupq_n_u16(128);

    const int16x8_t kUR = vdupq_n_s16(-43);
    const int16x8_t kUG = vdupq_n_s16(-84);
    const int16x8_t kUB = vdupq_n_s16(127);

    const int16x8_t kVR = vdupq_n_s16(127);
    const int16x8_t kVG = vdupq_n_s16(-106);
    const int16x8_t kVB = vdupq_n_s16(-21);

    const int16x8_t kUvRound = vdupq_n_s16(128);
    const int16x8_t kUvOffset = vdupq_n_s16(128);
#endif

    for (int32_t y = 0; y < height; y += 2) {
        const bool hasRow1 = (y + 1 < height);
        const uint8_t* row0 = rgba + static_cast<size_t>(y) * rgbaStride;
        const uint8_t* row1 = hasRow1 ? (rgba + static_cast<size_t>(y + 1) * rgbaStride) : row0;

        uint8_t* yOut0 = view.yPtr + static_cast<size_t>(y) * static_cast<size_t>(view.yRowStride);
        uint8_t* yOut1 = hasRow1
                             ? (view.yPtr + static_cast<size_t>(y + 1) * static_cast<size_t>(view.yRowStride))
                             : nullptr;
        uint8_t* uOut = view.uPtr + static_cast<size_t>(y / 2) * static_cast<size_t>(view.uRowStride);
        uint8_t* vOut = view.vPtr + static_cast<size_t>(y / 2) * static_cast<size_t>(view.vRowStride);

        int32_t x = 0;
#if FAM_CODEC_NEON
        for (; x + 16 <= width; x += 16) {
            const uint8x16x4_t p0 = vld4q_u8(row0 + static_cast<size_t>(x) * 4U);

            uint16x8_t y0Lo = vmlal_u8(kYBias, vget_low_u8(p0.val[0]), kYR);
            y0Lo = vmlal_u8(y0Lo, vget_low_u8(p0.val[1]), kYG);
            y0Lo = vmlal_u8(y0Lo, vget_low_u8(p0.val[2]), kYB);

            uint16x8_t y0Hi = vmlal_u8(kYBias, vget_high_u8(p0.val[0]), kYR);
            y0Hi = vmlal_u8(y0Hi, vget_high_u8(p0.val[1]), kYG);
            y0Hi = vmlal_u8(y0Hi, vget_high_u8(p0.val[2]), kYB);

            vst1q_u8(yOut0 + x, vcombine_u8(vshrn_n_u16(y0Lo, 8), vshrn_n_u16(y0Hi, 8)));

            uint8x16x4_t p1 = p0;
            if (hasRow1) {
                p1 = vld4q_u8(row1 + static_cast<size_t>(x) * 4U);

                uint16x8_t y1Lo = vmlal_u8(kYBias, vget_low_u8(p1.val[0]), kYR);
                y1Lo = vmlal_u8(y1Lo, vget_low_u8(p1.val[1]), kYG);
                y1Lo = vmlal_u8(y1Lo, vget_low_u8(p1.val[2]), kYB);

                uint16x8_t y1Hi = vmlal_u8(kYBias, vget_high_u8(p1.val[0]), kYR);
                y1Hi = vmlal_u8(y1Hi, vget_high_u8(p1.val[1]), kYG);
                y1Hi = vmlal_u8(y1Hi, vget_high_u8(p1.val[2]), kYB);

                vst1q_u8(yOut1 + x, vcombine_u8(vshrn_n_u16(y1Lo, 8), vshrn_n_u16(y1Hi, 8)));
            }

            const uint16x8_t sumR = vpadalq_u8(vpaddlq_u8(p0.val[0]), p1.val[0]);
            const uint16x8_t sumG = vpadalq_u8(vpaddlq_u8(p0.val[1]), p1.val[1]);
            const uint16x8_t sumB = vpadalq_u8(vpaddlq_u8(p0.val[2]), p1.val[2]);

            const int16x8_t rAvg = vreinterpretq_s16_u16(vrshrq_n_u16(sumR, 2));
            const int16x8_t gAvg = vreinterpretq_s16_u16(vrshrq_n_u16(sumG, 2));
            const int16x8_t bAvg = vreinterpretq_s16_u16(vrshrq_n_u16(sumB, 2));

            int16x8_t uAcc = vmlaq_s16(kUvRound, rAvg, kUR);
            uAcc = vmlaq_s16(uAcc, gAvg, kUG);
            uAcc = vmlaq_s16(uAcc, bAvg, kUB);
            const int16x8_t uVal = vaddq_s16(vshrq_n_s16(uAcc, 8), kUvOffset);

            int16x8_t vAcc = vmlaq_s16(kUvRound, rAvg, kVR);
            vAcc = vmlaq_s16(vAcc, gAvg, kVG);
            vAcc = vmlaq_s16(vAcc, bAvg, kVB);
            const int16x8_t vVal = vaddq_s16(vshrq_n_s16(vAcc, 8), kUvOffset);

            vst1_u8(uOut + (x >> 1), vqmovun_s16(uVal));
            vst1_u8(vOut + (x >> 1), vqmovun_s16(vVal));
        }
#endif

        // Bit-exact scalar tail for remaining columns (including odd widths/heights)
        for (; x < width; x += 2) {
            const uint8_t* p00 = row0 + static_cast<size_t>(x) * 4U;
            const int32_t r00 = p00[0];
            const int32_t g00 = p00[1];
            const int32_t b00 = p00[2];
            yOut0[x] = static_cast<uint8_t>((77 * r00 + 150 * g00 + 29 * b00 + 128) >> 8);

            int32_t r01 = r00;
            int32_t g01 = g00;
            int32_t b01 = b00;
            if (x + 1 < width) {
                const uint8_t* p01 = row0 + static_cast<size_t>(x + 1) * 4U;
                r01 = p01[0];
                g01 = p01[1];
                b01 = p01[2];
                yOut0[x + 1] = static_cast<uint8_t>((77 * r01 + 150 * g01 + 29 * b01 + 128) >> 8);
            }

            int32_t r10 = r00;
            int32_t g10 = g00;
            int32_t b10 = b00;
            int32_t r11 = r01;
            int32_t g11 = g01;
            int32_t b11 = b01;
            if (hasRow1) {
                const uint8_t* p10 = row1 + static_cast<size_t>(x) * 4U;
                r10 = p10[0];
                g10 = p10[1];
                b10 = p10[2];
                yOut1[x] = static_cast<uint8_t>((77 * r10 + 150 * g10 + 29 * b10 + 128) >> 8);

                r11 = r10;
                g11 = g10;
                b11 = b10;
                if (x + 1 < width) {
                    const uint8_t* p11 = row1 + static_cast<size_t>(x + 1) * 4U;
                    r11 = p11[0];
                    g11 = p11[1];
                    b11 = p11[2];
                    yOut1[x + 1] = static_cast<uint8_t>((77 * r11 + 150 * g11 + 29 * b11 + 128) >> 8);
                }
            }

            const int32_t rAvg = (r00 + r01 + r10 + r11 + 2) >> 2;
            const int32_t gAvg = (g00 + g01 + g10 + g11 + 2) >> 2;
            const int32_t bAvg = (b00 + b01 + b10 + b11 + 2) >> 2;

            const int32_t uVal = ((-43 * rAvg - 84 * gAvg + 127 * bAvg + 128) >> 8) + 128;
            const int32_t vVal = ((127 * rAvg - 106 * gAvg - 21 * bAvg + 128) >> 8) + 128;

            uOut[x >> 1] = static_cast<uint8_t>(std::clamp(uVal, 0, 255));
            vOut[x >> 1] = static_cast<uint8_t>(std::clamp(vVal, 0, 255));
        }
    }

    return true;
}

#if defined(__ANDROID__)
struct AImageDecoder;
struct AImageDecoderHeaderInfo;

/**
 * Dynamic NDK AImageDecoder resolver (`libjnigraphics.so`, API 30+).
 * Keeps minSdk 24 binary link compatibility while unlocking native zero-Java-Bitmap decoding
 * for HEIC / HEIF, AVIF, PNG, WebP, BMP, GIF, DNG RAW, and 10-bit/HDR camera photos.
 */
struct NdkImageDecoderDynamicApi {
    using CreateFromFdFn = int (*)(int, AImageDecoder**);
    using CreateFromBufferFn = int (*)(const void*, size_t, AImageDecoder**);
    using DeleteFn = void (*)(AImageDecoder*);
    using GetHeaderInfoFn = const AImageDecoderHeaderInfo* (*)(const AImageDecoder*);
    using HeaderGetWidthFn = int32_t (*)(const AImageDecoderHeaderInfo*);
    using HeaderGetHeightFn = int32_t (*)(const AImageDecoderHeaderInfo*);
    using SetAndroidBitmapFormatFn = int (*)(AImageDecoder*, int32_t);
    using SetTargetSizeFn = int (*)(AImageDecoder*, int32_t, int32_t);
    using GetMinimumStrideFn = size_t (*)(AImageDecoder*);
    using DecodeImageFn = int (*)(AImageDecoder*, void*, size_t, size_t);

    CreateFromFdFn createFromFd = nullptr;
    CreateFromBufferFn createFromBuffer = nullptr;
    DeleteFn deleteDecoder = nullptr;
    GetHeaderInfoFn getHeaderInfo = nullptr;
    HeaderGetWidthFn getWidth = nullptr;
    HeaderGetHeightFn getHeight = nullptr;
    SetAndroidBitmapFormatFn setBitmapFormat = nullptr;
    SetTargetSizeFn setTargetSize = nullptr;
    GetMinimumStrideFn getMinStride = nullptr;
    DecodeImageFn decodeImage = nullptr;
    bool available = false;

    static const NdkImageDecoderDynamicApi& instance() noexcept {
        static const NdkImageDecoderDynamicApi api = []() noexcept {
            NdkImageDecoderDynamicApi res{};
            void* lib = ::dlopen("libjnigraphics.so", RTLD_LAZY | RTLD_LOCAL);
            if (!lib) {
                return res;
            }
            res.createFromFd = reinterpret_cast<CreateFromFdFn>(
                ::dlsym(lib, "AImageDecoder_createFromFd"));
            res.createFromBuffer = reinterpret_cast<CreateFromBufferFn>(
                ::dlsym(lib, "AImageDecoder_createFromBuffer"));
            res.deleteDecoder = reinterpret_cast<DeleteFn>(
                ::dlsym(lib, "AImageDecoder_delete"));
            res.getHeaderInfo = reinterpret_cast<GetHeaderInfoFn>(
                ::dlsym(lib, "AImageDecoder_getHeaderInfo"));
            res.getWidth = reinterpret_cast<HeaderGetWidthFn>(
                ::dlsym(lib, "AImageDecoderHeaderInfo_getWidth"));
            res.getHeight = reinterpret_cast<HeaderGetHeightFn>(
                ::dlsym(lib, "AImageDecoderHeaderInfo_getHeight"));
            res.setBitmapFormat = reinterpret_cast<SetAndroidBitmapFormatFn>(
                ::dlsym(lib, "AImageDecoder_setAndroidBitmapFormat"));
            res.setTargetSize = reinterpret_cast<SetTargetSizeFn>(
                ::dlsym(lib, "AImageDecoder_setTargetSize"));
            res.getMinStride = reinterpret_cast<GetMinimumStrideFn>(
                ::dlsym(lib, "AImageDecoder_getMinimumStride"));
            res.decodeImage = reinterpret_cast<DecodeImageFn>(
                ::dlsym(lib, "AImageDecoder_decodeImage"));

            res.available = res.createFromFd && res.createFromBuffer && res.deleteDecoder &&
                            res.getHeaderInfo && res.getWidth && res.getHeight &&
                            res.setBitmapFormat && res.setTargetSize &&
                            res.getMinStride && res.decodeImage;
            return res;
        }();
        return api;
    }
};

[[nodiscard]] AImageDecoder* openNdkImageDecoder(
    const NdkImageDecoderDynamicApi& api,
    int inFd,
    std::span<const uint8_t> fallbackBytes) noexcept {

    AImageDecoder* dec = nullptr;
    // Prefer createFromFd so Stagefright HeifDecoder uses direct FileSource FD without Binder stream copies
    if (inFd > 0 && ::lseek(inFd, 0, SEEK_SET) == 0) {
        if (api.createFromFd(inFd, &dec) != 0) {
            dec = nullptr;
        }
    }
    if (!dec && fallbackBytes.size() >= 4) {
        if (api.createFromBuffer(fallbackBytes.data(), fallbackBytes.size(), &dec) != 0) {
            dec = nullptr;
        }
    }
    return dec;
}
#endif

[[nodiscard]] bool inspectViaNdkImageDecoder(
    int inFd,
    std::span<const uint8_t> fallbackBytes,
    JpegHeaderMeta& outMeta) {

#if defined(__ANDROID__)
    const auto& api = NdkImageDecoderDynamicApi::instance();
    if (!api.available) {
        return false;
    }
    AImageDecoder* dec = openNdkImageDecoder(api, inFd, fallbackBytes);
    if (!dec) {
        return false;
    }

    const AImageDecoderHeaderInfo* info = api.getHeaderInfo(dec);
    const int32_t w = info ? api.getWidth(info) : 0;
    const int32_t h = info ? api.getHeight(info) : 0;
    api.deleteDecoder(dec);

    if (w < 16 || h < 16) {
        return false;
    }

    // AImageDecoder natively applies EXIF / HEIF irot / AVIF orientation to header dimensions
    outMeta.valid = true;
    outMeta.rawWidth = w;
    outMeta.rawHeight = h;
    outMeta.uprightWidth = w;
    outMeta.uprightHeight = h;
    outMeta.exifOrientation = 1;
    outMeta.errorMessage.clear();
    return true;
#else
    (void)inFd;
    (void)fallbackBytes;
    (void)outMeta;
    return false;
#endif
}

[[nodiscard]] bool decodeRgbaViaNdkImageDecoder(
    int inFd,
    std::span<const uint8_t> fallbackBytes,
    int32_t maxDim,
    std::vector<uint8_t>& outRgbaBytes,
    size_t& outStrideBytes,
    int32_t& outWidth,
    int32_t& outHeight,
    std::string* errorOut) {

#if defined(__ANDROID__)
    const auto& api = NdkImageDecoderDynamicApi::instance();
    if (!api.available) {
        if (errorOut) *errorOut = "NDK AImageDecoder unavailable (requires Android 11+ for HEIC/PNG/WebP)";
        return false;
    }

    AImageDecoder* dec = openNdkImageDecoder(api, inFd, fallbackBytes);
    if (!dec) {
        if (errorOut) *errorOut = "AImageDecoder could not recognize image format";
        return false;
    }

    const AImageDecoderHeaderInfo* info = api.getHeaderInfo(dec);
    const int32_t origW = info ? api.getWidth(info) : 0;
    const int32_t origH = info ? api.getHeight(info) : 0;
    if (origW < 16 || origH < 16) {
        api.deleteDecoder(dec);
        if (errorOut) *errorOut = "Image dimensions smaller than 16x16 minimum";
        return false;
    }

    constexpr int32_t kAndroidBitmapFormatRgba8888 = 1;
    if (api.setBitmapFormat(dec, kAndroidBitmapFormatRgba8888) != 0) {
        api.deleteDecoder(dec);
        if (errorOut) *errorOut = "AImageDecoder failed setting RGBA_8888 format";
        return false;
    }

    int32_t targetW = origW;
    int32_t targetH = origH;
    if (maxDim >= 16 && std::max(origW, origH) > maxDim) {
        if (origW >= origH) {
            targetW = maxDim;
            targetH = std::max<int32_t>(
                16, static_cast<int32_t>((static_cast<int64_t>(origH) * maxDim + origW / 2) / origW));
        } else {
            targetH = maxDim;
            targetW = std::max<int32_t>(
                16, static_cast<int32_t>((static_cast<int64_t>(origW) * maxDim + origH / 2) / origH));
        }
        if (api.setTargetSize(dec, targetW, targetH) != 0) {
            api.deleteDecoder(dec);
            if (errorOut) *errorOut = "AImageDecoder failed setting target dimensions";
            return false;
        }
    }

    const size_t minStride = api.getMinStride(dec);
    const size_t strideBytes = std::max(minStride, static_cast<size_t>(targetW) * 4U);
    const size_t totalBytes = strideBytes * static_cast<size_t>(targetH);

    outRgbaBytes.resize(totalBytes);
    const int rc = api.decodeImage(dec, outRgbaBytes.data(), strideBytes, totalBytes);
    api.deleteDecoder(dec);

    if (rc != 0) {
        if (errorOut) *errorOut = "AImageDecoder_decodeImage failed";
        return false;
    }

    outStrideBytes = strideBytes;
    outWidth = targetW;
    outHeight = targetH;
    return true;
#else
    (void)inFd;
    (void)fallbackBytes;
    (void)maxDim;
    (void)outRgbaBytes;
    (void)outStrideBytes;
    (void)outWidth;
    (void)outHeight;
    if (errorOut) *errorOut = "Non-JPEG decoding requires Android NDK AImageDecoder";
    return false;
#endif
}

[[nodiscard]] tjscalingfactor chooseTurboJpegScalingFactor(
    int rawW, int rawH, int32_t maxDim) noexcept {

    if (maxDim <= 0 || std::max(rawW, rawH) <= maxDim) {
        return TJUNSCALED;
    }

    int numFactors = 0;
    const tjscalingfactor* factors = tj3GetScalingFactors(&numFactors);
    if (!factors || numFactors <= 0) {
        return TJUNSCALED;
    }

    tjscalingfactor bestLe = {0, 1};
    int bestLeMax = 0;
    tjscalingfactor smallest = TJUNSCALED;
    int smallestMax = std::max(rawW, rawH);

    for (int i = 0; i < numFactors; ++i) {
        if (factors[i].num > factors[i].denom) {
            continue; // Never upscale beyond 1/1
        }
        const int candW = TJSCALED(rawW, factors[i]);
        const int candH = TJSCALED(rawH, factors[i]);
        const int candMax = std::max(candW, candH);
        if (candMax < smallestMax && candW >= 16 && candH >= 16) {
            smallestMax = candMax;
            smallest = factors[i];
        }
        if (candMax <= maxDim && candMax > bestLeMax && candW >= 16 && candH >= 16) {
            bestLeMax = candMax;
            bestLe = factors[i];
        }
    }

    return (bestLeMax > 0) ? bestLe : smallest;
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
    std::string* errorOut,
    int optionalInFd) {

    if (!tjDecompressor_ || (jpegBytes.size() < 4 && optionalInFd < 0)) {
        if (errorOut) *errorOut = "Invalid TurboJPEG decompressor or empty image input";
        return false;
    }

    auto* tj = static_cast<tjhandle>(tjDecompressor_);
    (void)tj3SetScalingFactor(tj, TJUNSCALED);

    // Tier 1 & Tier 2: libjpeg-turbo 3.0+ SIMD fast paths for all 8-bit JPEGs
    if (jpegBytes.size() >= 4 && jpegBytes[0] == 0xFF && jpegBytes[1] == 0xD8 &&
        tj3DecompressHeader(tj, jpegBytes.data(), jpegBytes.size()) == 0) {

        const int precision = tj3Get(tj, TJPARAM_PRECISION);
        const int rawW = tj3Get(tj, TJPARAM_JPEGWIDTH);
        const int rawH = tj3Get(tj, TJPARAM_JPEGHEIGHT);
        const int jpegSubsamp = tj3Get(tj, TJPARAM_SUBSAMP);
        const int jpegColorspace = tj3Get(tj, TJPARAM_COLORSPACE);

        if (precision == 8 && rawW >= 16 && rawH >= 16) {
            const uint16_t jpegOrient = parseExifOrientation(jpegBytes);

            // Tier 1: Direct zero-RGB YUV plane decode (4:2:0, 4:2:2, 4:4:4, Grayscale)
            ChromaSubsampling subsampling = ChromaSubsampling::YUV420;
            if ((jpegColorspace == TJCS_YCbCr || jpegColorspace == TJCS_GRAY) &&
                fromTjSubsamp(jpegSubsamp, subsampling) &&
                std::max(rawW, rawH) <= 8192) {

                if (outArena.ensureLayout(rawW, rawH, subsampling)) {
                    const YuvPlaneView v = outArena.view();
                    unsigned char* dstPlanes[3] = {v.yPtr, v.uPtr, v.vPtr};
                    int strides[3] = {v.yRowStride, v.uRowStride, v.vRowStride};

                    if (tjDecompressToYUVPlanes(
                            tj,
                            jpegBytes.data(),
                            static_cast<unsigned long>(jpegBytes.size()),
                            dstPlanes,
                            rawW,
                            strides,
                            rawH,
                            TJFLAG_FASTDCT | TJFLAG_FASTUPSAMPLE) == 0) {
                        if (outExifOrientation) {
                            *outExifOrientation = jpegOrient;
                        }
                        return true;
                    }
                }
            }

            // Tier 2: Universal 8-bit JPEG fallback (4:4:0, 4:1:1, 4:4:1, TJSAMP_UNKNOWN, RGB, CMYK, >8192px)
            const tjscalingfactor factor = chooseTurboJpegScalingFactor(rawW, rawH, 4096);
            (void)tj3Set(tj, TJPARAM_FASTDCT, 1);
            (void)tj3Set(tj, TJPARAM_FASTUPSAMPLE, 1);
            (void)tj3SetScalingFactor(tj, factor);

            const int scaledW = std::max(16, TJSCALED(rawW, factor));
            const int scaledH = std::max(16, TJSCALED(rawH, factor));
            const bool isCmyk = (jpegColorspace == TJCS_CMYK || jpegColorspace == TJCS_YCCK);

            std::vector<uint8_t> rgbaBuf(
                static_cast<size_t>(scaledW) * static_cast<size_t>(scaledH) * 4U);
            const int decRc = tj3Decompress8(
                tj,
                jpegBytes.data(),
                jpegBytes.size(),
                rgbaBuf.data(),
                scaledW * 4,
                isCmyk ? TJPF_CMYK : TJPF_RGBA);
            (void)tj3SetScalingFactor(tj, TJUNSCALED);

            if (decRc == 0) {
                if (isCmyk) {
                    convertCmykToRgbaInPlace(
                        rgbaBuf.data(),
                        static_cast<size_t>(scaledW) * static_cast<size_t>(scaledH));
                }
                if (rgbaToYuv420Arena(
                        rgbaBuf.data(),
                        static_cast<size_t>(scaledW) * 4U,
                        scaledW,
                        scaledH,
                        outArena)) {
                    if (outExifOrientation) {
                        *outExifOrientation = jpegOrient;
                    }
                    return true;
                }
            }
        }
    }

    // Tier 3: NDK AImageDecoder + ARM NEON BT.601 YUV420 (HEIC/HEIF, PNG, WebP, AVIF, BMP, GIF, RAW, 10-bit)
    std::vector<uint8_t> decodedRgba;
    size_t rgbaStride = 0;
    int32_t decW = 0;
    int32_t decH = 0;
    if (decodeRgbaViaNdkImageDecoder(
            optionalInFd, jpegBytes, 4096,
            decodedRgba, rgbaStride, decW, decH, errorOut)) {
        if (rgbaToYuv420Arena(decodedRgba.data(), rgbaStride, decW, decH, outArena)) {
            if (outExifOrientation) {
                // AImageDecoder outputs pixels already oriented upright
                *outExifOrientation = 1;
            }
            return true;
        }
        if (errorOut) *errorOut = "Failed allocating YUV420 arena for decoded image";
        return false;
    }

    if (errorOut && errorOut->empty()) {
        *errorOut = "Unsupported or corrupted image file";
    }
    return false;
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
            // Semi-planar NV12/NV21 -> NEON vld2q_u8 de-interleave into planar UV scratch
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

JpegHeaderMeta JpegYuvCodec::inspectFd(int inFd) {
    JpegHeaderMeta meta{};
    if (!tjDecompressor_ || inFd < 0) {
        meta.errorMessage = "Invalid TurboJPEG handle or input FD";
        return meta;
    }

    MappedOrBufferedInput input;
    if (!input.load(inFd, streamFallbackBuf_, false)) {
        // Even if full read failed, give AImageDecoder FD path one chance before reporting failure
        if (inspectViaNdkImageDecoder(inFd, {}, meta)) {
            return meta;
        }
        meta.errorMessage = "Failed reading image header from FD";
        return meta;
    }

    auto* tj = static_cast<tjhandle>(tjDecompressor_);
    const auto bytes = input.span();

    if (bytes.size() >= 4 && bytes[0] == 0xFF && bytes[1] == 0xD8 &&
        tj3DecompressHeader(tj, bytes.data(), bytes.size()) == 0) {

        const int precision = tj3Get(tj, TJPARAM_PRECISION);
        const int w = tj3Get(tj, TJPARAM_JPEGWIDTH);
        const int h = tj3Get(tj, TJPARAM_JPEGHEIGHT);

        if (precision == 8 && w >= 16 && h >= 16) {
            meta.valid = true;
            meta.exifOrientation = parseExifOrientation(bytes);
            meta.rawWidth = w;
            meta.rawHeight = h;
            if (meta.exifOrientation >= 5 && meta.exifOrientation <= 8) {
                meta.uprightWidth = h;
                meta.uprightHeight = w;
            } else {
                meta.uprightWidth = w;
                meta.uprightHeight = h;
            }
            return meta;
        }
    }

    // Fallback to NDK AImageDecoder for HEIC/HEIF, PNG, WebP, AVIF, BMP, GIF, RAW, or 10-bit/12-bit JPEG
    if (inspectViaNdkImageDecoder(inFd, bytes, meta)) {
        return meta;
    }

    meta.errorMessage = "Unsupported or corrupted image format";
    return meta;
}

LearnResult JpegYuvCodec::learnTemplateFromFd(
    MosaicEngine& engine,
    int inFd,
    std::span<const NormalizedPolygon> uprightPolygons,
    const std::string& outputTplPath,
    const std::string& registerTemplateId) {

    const auto tStart = SteadyClock::now();
    LearnResult res{};
    if (inFd < 0 || uprightPolygons.empty()) {
        res.errorMessage = "Invalid input FD or empty polygon list";
        return res;
    }

    MappedOrBufferedInput input;
    (void)input.load(inFd, streamFallbackBuf_, true);

    std::string err;
    uint16_t exifOrientation = 1;
    if (!decodeToYuvPlanes(input.span(), arena_, &exifOrientation, &err, inFd)) {
        res.errorMessage = err;
        return res;
    }

    // Map user-drawn upright [0, 1] polygons into raw unrotated Y-plane [0, 1] coordinates
    std::vector<NormalizedPolygon> rawPolygons;
    rawPolygons.reserve(uprightPolygons.size());
    for (const auto& upPoly : uprightPolygons) {
        NormalizedPolygon rp;
        rp.vertices.reserve(upPoly.vertices.size());
        for (const auto& v : upPoly.vertices) {
            const float ux = std::clamp(v.normX, 0.0f, 1.0f);
            const float uy = std::clamp(v.normY, 0.0f, 1.0f);
            rp.vertices.push_back(uprightToRawNormalized(ux, uy, exifOrientation));
        }
        rawPolygons.push_back(std::move(rp));
    }

    const YuvPlaneView yuvView = arena_.view();
    const YPlaneConstView yView{
        yuvView.yPtr,
        yuvView.width,
        yuvView.height,
        yuvView.yRowStride
    };

    res = engine.learnTemplateToFile(
        yView,
        rawPolygons,
        outputTplPath,
        registerTemplateId,
        exifOrientation);
    res.elapsedUs = elapsedUs(tStart);
    return res;
}

bool JpegYuvCodec::decodePreviewRgbaFromFd(
    int inFd,
    int32_t maxPreviewDim,
    std::vector<uint32_t>& outUprightRgba,
    int32_t& outUprightWidth,
    int32_t& outUprightHeight,
    std::string* errorOut) {

    if (!tjDecompressor_ || inFd < 0) {
        if (errorOut) *errorOut = "Invalid TurboJPEG decompressor or FD";
        return false;
    }

    MappedOrBufferedInput input;
    (void)input.load(inFd, streamFallbackBuf_, false);
    const auto bytes = input.span();
    const int32_t targetMax = std::clamp(maxPreviewDim, 320, 2048);

    auto* tj = static_cast<tjhandle>(tjDecompressor_);
    (void)tj3SetScalingFactor(tj, TJUNSCALED);

    // Fast Path: Hardware-SIMD IDCT scaled decode via libjpeg-turbo 3.0+ for all 8-bit JPEGs
    if (bytes.size() >= 4 && bytes[0] == 0xFF && bytes[1] == 0xD8 &&
        tj3DecompressHeader(tj, bytes.data(), bytes.size()) == 0) {

        const int precision = tj3Get(tj, TJPARAM_PRECISION);
        const int rawW = tj3Get(tj, TJPARAM_JPEGWIDTH);
        const int rawH = tj3Get(tj, TJPARAM_JPEGHEIGHT);
        const int cs = tj3Get(tj, TJPARAM_COLORSPACE);

        if (precision == 8 && rawW >= 16 && rawH >= 16) {
            const uint16_t orient = parseExifOrientation(bytes);
            const tjscalingfactor factor = chooseTurboJpegScalingFactor(rawW, rawH, targetMax);

            (void)tj3Set(tj, TJPARAM_FASTDCT, 1);
            (void)tj3Set(tj, TJPARAM_FASTUPSAMPLE, 1);
            (void)tj3SetScalingFactor(tj, factor);

            const int scaledW = std::max(16, TJSCALED(rawW, factor));
            const int scaledH = std::max(16, TJSCALED(rawH, factor));
            const bool isCmyk = (cs == TJCS_CMYK || cs == TJCS_YCCK);

            std::vector<uint32_t> rawRgba(
                static_cast<size_t>(scaledW) * static_cast<size_t>(scaledH));
            const int rc = tj3Decompress8(
                tj,
                bytes.data(),
                bytes.size(),
                reinterpret_cast<unsigned char*>(rawRgba.data()),
                scaledW * 4,
                isCmyk ? TJPF_CMYK : TJPF_RGBA);
            (void)tj3SetScalingFactor(tj, TJUNSCALED);

            if (rc == 0) {
                if (isCmyk) {
                    convertCmykToRgbaInPlace(
                        reinterpret_cast<uint8_t*>(rawRgba.data()),
                        rawRgba.size());
                }

                if (orient == 1) {
                    outUprightWidth = scaledW;
                    outUprightHeight = scaledH;
                    outUprightRgba = std::move(rawRgba);
                    return true;
                }

                const bool swapAxes = (orient >= 5 && orient <= 8);
                const int32_t upW = swapAxes ? scaledH : scaledW;
                const int32_t upH = swapAxes ? scaledW : scaledH;
                outUprightWidth = upW;
                outUprightHeight = upH;
                outUprightRgba.resize(static_cast<size_t>(upW) * static_cast<size_t>(upH));

                for (int32_t uy = 0; uy < upH; ++uy) {
                    uint32_t* dstRow = outUprightRgba.data() + static_cast<size_t>(uy) * upW;
                    for (int32_t ux = 0; ux < upW; ++ux) {
                        int32_t rx = ux;
                        int32_t ry = uy;
                        switch (orient) {
                            case 2: rx = (scaledW - 1) - ux; ry = uy; break;
                            case 3: rx = (scaledW - 1) - ux; ry = (scaledH - 1) - uy; break;
                            case 4: rx = ux;                 ry = (scaledH - 1) - uy; break;
                            case 5: rx = uy;                 ry = ux; break;
                            case 6: rx = uy;                 ry = (scaledH - 1) - ux; break;
                            case 7: rx = (scaledW - 1) - uy; ry = (scaledH - 1) - ux; break;
                            case 8: rx = (scaledW - 1) - uy; ry = ux; break;
                            default: break;
                        }
                        dstRow[ux] = rawRgba[static_cast<size_t>(ry) * scaledW + rx];
                    }
                }
                return true;
            }
        }
    }

    // Fallback: NDK AImageDecoder scaled upright preview for HEIC/HEIF, PNG, WebP, AVIF, BMP, GIF, RAW
    std::vector<uint8_t> rgbaBytes;
    size_t strideBytes = 0;
    int32_t decW = 0;
    int32_t decH = 0;
    if (!decodeRgbaViaNdkImageDecoder(
            inFd, bytes, targetMax, rgbaBytes, strideBytes, decW, decH, errorOut)) {
        return false;
    }

    outUprightWidth = decW;
    outUprightHeight = decH;
    outUprightRgba.resize(static_cast<size_t>(decW) * static_cast<size_t>(decH));

    const size_t rowBytes = static_cast<size_t>(decW) * sizeof(uint32_t);
    if (strideBytes == rowBytes) {
        std::memcpy(outUprightRgba.data(), rgbaBytes.data(), rowBytes * static_cast<size_t>(decH));
    } else {
        for (int32_t y = 0; y < decH; ++y) {
            std::memcpy(
                outUprightRgba.data() + static_cast<size_t>(y) * static_cast<size_t>(decW),
                rgbaBytes.data() + static_cast<size_t>(y) * strideBytes,
                rowBytes);
        }
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

    if (inFd < 0 || outFd < 0) {
        tel.message = "Invalid input or output FD";
        return tel;
    }

    // Step 1: Zero-copy mmap source photo FD (or stream/FD decode for HEIC/PNG/WebP/Cloud FDs)
    const auto tDec = SteadyClock::now();
    MappedOrBufferedInput input;
    (void)input.load(inFd, streamFallbackBuf_, true);

    std::string err;
    uint16_t exifOrientation = 1;
    const bool decOk = decodeToYuvPlanes(input.span(), arena_, &exifOrientation, &err, inFd);
    if (!decOk) {
        tel.message = err;
        return tel;
    }
    tel.jpegDecodeUs = elapsedUs(tDec);
    tel.exifOrientation = exifOrientation;

    YuvPlaneView view = arena_.view();
    tel.width = view.width;
    tel.height = view.height;

    // Step 2: Run Homography Auto-Mosaic (+ EXIF-aligned fixed fallback) IN-PLACE on YUV planes
    const MosaicProcessResult proc =
        engine.processYuvInPlace(view, overrideBlockSize, exifOrientation);
    tel.matchedAny = proc.matchedAny;
    tel.templatesMatched = proc.templatesMatched;
    tel.fixedPositionApplied = proc.fixedPositionApplied;
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
