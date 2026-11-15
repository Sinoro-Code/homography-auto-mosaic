#include "famtpl_format.hpp"
#include "jpeg_yuv_codec.hpp"
#include "mosaic_engine.hpp"

#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <numeric>
#include <string>
#include <unistd.h>
#include <vector>

#define FAM_REQUIRE(cond, msg)                                                                  \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "[FAILED] %s:%d: %s (%s)\n", __FILE__, __LINE__, msg, #cond); \
            std::exit(1);                                                                       \
        }                                                                                       \
    } while (0)

namespace {

// Synthesizes a structured document/ID-card grayscale Y image with rich multi-scale corners & text bars
cv::Mat createSyntheticDocumentScene(int width, int height) {
    cv::Mat img(height, width, CV_8UC1, cv::Scalar(218));

    // Subtle background paper texture deterministic grid
    for (int y = 0; y < height; y += 24) {
        cv::line(img, cv::Point(0, y), cv::Point(width, y), cv::Scalar(208), 1);
    }
    for (int x = 0; x < width; x += 24) {
        cv::line(img, cv::Point(x, 0), cv::Point(x, height), cv::Scalar(208), 1);
    }

    // Card outer frame [20% .. 80%]
    const int cx0 = width * 20 / 100;
    const int cy0 = height * 18 / 100;
    const int cx1 = width * 80 / 100;
    const int cy1 = height * 82 / 100;
    cv::rectangle(img, cv::Point(cx0, cy0), cv::Point(cx1, cy1), cv::Scalar(35), 4);
    cv::rectangle(img, cv::Point(cx0 + 12, cy0 + 12), cv::Point(cx1 - 12, cy1 - 12), cv::Scalar(90), 2);

    // Rich asymmetric fiducial patterns & simulated text blocks (abundant ORB corners)
    uint32_t seed = 0x13579BDFU;
    auto nextRand = [&seed]() -> uint32_t {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    };

    // Header emblem & QR-like matrix in top-left of card
    const int qrX = cx0 + (cx1 - cx0) * 6 / 100;
    const int qrY = cy0 + (cy1 - cy0) * 10 / 100;
    const int cell = std::max(4, width / 240);
    for (int r = 0; r < 18; ++r) {
        for (int c = 0; c < 18; ++c) {
            if ((nextRand() & 1U) != 0 || r == 0 || r == 17 || c == 0 || c == 17) {
                cv::rectangle(
                    img,
                    cv::Point(qrX + c * cell, qrY + r * cell),
                    cv::Point(qrX + (c + 1) * cell - 1, qrY + (r + 1) * cell - 1),
                    cv::Scalar(25),
                    cv::FILLED);
            }
        }
    }

    // Simulated typography glyph blocks across upper & lower card body
    for (int row = 0; row < 8; ++row) {
        const int lineY = cy0 + (cy1 - cy0) * (15 + row * 9) / 100;
        int curX = cx0 + (cx1 - cx0) * 28 / 100;
        const int maxX = cx1 - (cx1 - cx0) * 8 / 100;
        while (curX + cell * 4 < maxX) {
            const int glyphW = cell * static_cast<int>(2 + (nextRand() % 5));
            const int glyphH = cell * static_cast<int>(2 + (nextRand() % 2));
            const uint8_t shade = static_cast<uint8_t>(20 + (nextRand() % 60));
            cv::rectangle(
                img,
                cv::Point(curX, lineY),
                cv::Point(curX + glyphW, lineY + glyphH),
                cv::Scalar(shade),
                cv::FILLED);
            // Inner notch for crisp FAST corners
            if (glyphW >= cell * 3) {
                cv::rectangle(
                    img,
                    cv::Point(curX + cell, lineY + 1),
                    cv::Point(curX + cell * 2, lineY + glyphH / 2),
                    cv::Scalar(215),
                    cv::FILLED);
            }
            curX += glyphW + cell * static_cast<int>(1 + (nextRand() % 3));
        }
    }

    // Sensitive ID zone at normalized [0.36, 0.56] .. [0.72, 0.66] with high-contrast stripes
    const int sx0 = static_cast<int>(width * 0.36f);
    const int sy0 = static_cast<int>(height * 0.56f);
    const int sx1 = static_cast<int>(width * 0.72f);
    const int sy1 = static_cast<int>(height * 0.66f);
    cv::rectangle(img, cv::Point(sx0, sy0), cv::Point(sx1, sy1), cv::Scalar(245), cv::FILLED);
    for (int x = sx0 + 6; x + 8 < sx1; x += 14) {
        cv::rectangle(img, cv::Point(x, sy0 + 6), cv::Point(x + 7, sy1 - 6), cv::Scalar(15), cv::FILLED);
    }

    return img;
}

struct PaddedNvBuffer {
    std::vector<uint8_t> yStorage;
    std::vector<uint8_t> uvStorage; // Used for semi-planar NV12/NV21
    std::vector<uint8_t> uPlanar;   // Used for planar I420
    std::vector<uint8_t> vPlanar;
    fam::YuvPlaneView view{};
};

enum class TestUvMode { I420_PLANAR, NV12_SEMIPLANAR, NV21_SEMIPLANAR };

PaddedNvBuffer makeVendorPaddedYuv420(const cv::Mat& grayY, TestUvMode mode) {
    PaddedNvBuffer buf;
    const int32_t w = grayY.cols;
    const int32_t h = grayY.rows;
    // Simulate 128-byte ISP rowStride padding (e.g. width=1920 -> stride=2048)
    const int32_t yStride = ((w + 127) & ~127) + 128;
    const int32_t cw = (w + 1) / 2;
    const int32_t ch = (h + 1) / 2;

    buf.yStorage.assign(static_cast<size_t>(yStride) * h, 0xAA);
    for (int32_t r = 0; r < h; ++r) {
        std::memcpy(buf.yStorage.data() + static_cast<size_t>(r) * yStride, grayY.ptr<uint8_t>(r), w);
    }

    buf.view.yPtr = buf.yStorage.data();
    buf.view.width = w;
    buf.view.height = h;
    buf.view.yRowStride = yStride;
    buf.view.subsampling = fam::ChromaSubsampling::YUV420;

    if (mode == TestUvMode::I420_PLANAR) {
        const int32_t uvStride = ((cw + 63) & ~63) + 64;
        buf.uPlanar.assign(static_cast<size_t>(uvStride) * ch, 90);  // Distinct blue-ish tint
        buf.vPlanar.assign(static_cast<size_t>(uvStride) * ch, 195); // Distinct red-ish ink tint
        buf.view.uPtr = buf.uPlanar.data();
        buf.view.vPtr = buf.vPlanar.data();
        buf.view.uRowStride = uvStride;
        buf.view.vRowStride = uvStride;
        buf.view.uvPixelStride = 1;
    } else {
        const int32_t uvStride = ((cw * 2 + 127) & ~127) + 128;
        buf.uvStorage.assign(static_cast<size_t>(uvStride) * ch, 0);
        for (int32_t r = 0; r < ch; ++r) {
            uint8_t* row = buf.uvStorage.data() + static_cast<size_t>(r) * uvStride;
            for (int32_t c = 0; c < cw; ++c) {
                if (mode == TestUvMode::NV12_SEMIPLANAR) {
                    row[c * 2] = 90;      // U
                    row[c * 2 + 1] = 195; // V
                } else {
                    row[c * 2] = 195;     // V
                    row[c * 2 + 1] = 90;  // U
                }
            }
        }
        buf.view.uRowStride = uvStride;
        buf.view.vRowStride = uvStride;
        buf.view.uvPixelStride = 2;
        if (mode == TestUvMode::NV12_SEMIPLANAR) {
            buf.view.uPtr = buf.uvStorage.data();
            buf.view.vPtr = buf.uvStorage.data() + 1;
        } else {
            buf.view.vPtr = buf.uvStorage.data();
            buf.view.uPtr = buf.uvStorage.data() + 1;
        }
    }
    return buf;
}

} // namespace

int main() {
    std::printf("=== Homography Auto-Mosaic C++20 Native Verification Suite ===\n");

    const std::string tplPath = "/tmp/homography_test_template.famtpl";
    ::unlink(tplPath.c_str());

    // -------------------------------------------------------------------------
    // 1. Sharpness Anti-Shake Score Verification
    // -------------------------------------------------------------------------
    const cv::Mat refScene = createSyntheticDocumentScene(1920, 1080);
    cv::Mat blurredScene;
    cv::GaussianBlur(refScene, blurredScene, cv::Size(21, 21), 7.0);

    const float sharpVal = fam::MosaicEngine::computeSharpnessScore(
        {refScene.ptr<uint8_t>(0), refScene.cols, refScene.rows, static_cast<int32_t>(refScene.step)});
    const float blurVal = fam::MosaicEngine::computeSharpnessScore(
        {blurredScene.ptr<uint8_t>(0), blurredScene.cols, blurredScene.rows,
         static_cast<int32_t>(blurredScene.step)});
    std::printf("[Test 1] Sharpness score: crisp=%.2f, motion-blurred=%.2f\n", sharpVal, blurVal);
    FAM_REQUIRE(sharpVal > blurVal * 2.5f, "Sharpness score must distinguish crisp vs blurred");

    // -------------------------------------------------------------------------
    // 2. Self-Learning & FAMTPL02 Sub-1ms mmap Cold-Start Verification
    // -------------------------------------------------------------------------
    PaddedNvBuffer refPadded = makeVendorPaddedYuv420(refScene, TestUvMode::NV21_SEMIPLANAR);
    fam::NormalizedPolygon sensitiveZone;
    sensitiveZone.vertices = {
        {0.36f, 0.56f},
        {0.72f, 0.56f},
        {0.72f, 0.66f},
        {0.36f, 0.66f}
    };

    {
        fam::MosaicEngine learner;
        const fam::LearnResult learnRes = learner.learnTemplateToFile(
            {refPadded.view.yPtr, refPadded.view.width, refPadded.view.height, refPadded.view.yRowStride},
            std::span<const fam::NormalizedPolygon>(&sensitiveZone, 1),
            tplPath,
            "id_card_sensitive");

        std::printf("[Test 2] Learned FAMTPL02: success=%d, keypoints=%u, learnTime=%.3f ms\n",
                    learnRes.success ? 1 : 0,
                    learnRes.extractedKeypoints,
                    static_cast<double>(learnRes.elapsedUs) / 1000.0);
        FAM_REQUIRE(learnRes.success, "learnTemplateToFile failed");
        FAM_REQUIRE(learnRes.extractedKeypoints >= 100, "Too few keypoints extracted");
    }

    // Cold-start new engine instance and mmap FAMTPL02 without touching reference image
    fam::MosaicEngine engine;
    uint64_t coldLoadUs = 0;
    std::string loadErr;
    const bool loaded = engine.loadTemplateFromFile("id_card_sensitive", tplPath, &coldLoadUs, &loadErr);
    std::printf("[Test 2] Cold-start FAMTPL02 mmap load: ok=%d, elapsed=%llu us (%.4f ms)\n",
                loaded ? 1 : 0,
                static_cast<unsigned long long>(coldLoadUs),
                static_cast<double>(coldLoadUs) / 1000.0);
    FAM_REQUIRE(loaded, "loadTemplateFromFile failed");
    FAM_REQUIRE(coldLoadUs < 1000, "FAMTPL02 cold-start mmap must be Sub-1ms (< 1000 us)");

    // Verify corrupted checksum rejection
    {
        const std::string corruptPath = "/tmp/homography_corrupt.famtpl";
        FILE* fIn = std::fopen(tplPath.c_str(), "rb");
        FAM_REQUIRE(fIn != nullptr, "Failed opening template for corruption test");
        std::fseek(fIn, 0, SEEK_END);
        const long sz = std::ftell(fIn);
        std::fseek(fIn, 0, SEEK_SET);
        std::vector<uint8_t> raw(static_cast<size_t>(sz));
        FAM_REQUIRE(std::fread(raw.data(), 1, raw.size(), fIn) == raw.size(), "Short read");
        std::fclose(fIn);

        raw[80] ^= 0x40U; // Flip 1 bit in descriptor payload
        FILE* fOut = std::fopen(corruptPath.c_str(), "wb");
        FAM_REQUIRE(fOut != nullptr, "Failed creating corrupt test file");
        FAM_REQUIRE(std::fwrite(raw.data(), 1, raw.size(), fOut) == raw.size(), "Short write");
        std::fclose(fOut);

        fam::MappedFamTemplate badMap;
        FAM_REQUIRE(!badMap.mapFromFile(corruptPath, true), "Corrupted FAMTPL02 must be rejected");
        ::unlink(corruptPath.c_str());
    }

    // -------------------------------------------------------------------------
    // 3. Perspective-Warped Homography + Cross-Vendor I420 / NV12 / NV21 Tests
    // -------------------------------------------------------------------------
    const std::vector<cv::Point2f> srcQuad = {
        {0.0f, 0.0f}, {1920.0f, 0.0f}, {1920.0f, 1080.0f}, {0.0f, 1080.0f}
    };
    const std::vector<cv::Point2f> dstQuad = {
        {210.0f, 130.0f}, {1740.0f, 65.0f}, {1660.0f, 980.0f}, {140.0f, 920.0f}
    };
    const cv::Mat H_gt = cv::getPerspectiveTransform(srcQuad, dstQuad);
    cv::Mat warpedScene;
    cv::warpPerspective(refScene, warpedScene, H_gt, cv::Size(1920, 1080),
                        cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(180));

    std::vector<cv::Point2f> gtSrcPoly = {
        {0.36f * 1920.0f, 0.56f * 1080.0f},
        {0.72f * 1920.0f, 0.56f * 1080.0f},
        {0.72f * 1920.0f, 0.66f * 1080.0f},
        {0.36f * 1920.0f, 0.66f * 1080.0f}
    };
    std::vector<cv::Point2f> gtDstPoly;
    cv::perspectiveTransform(gtSrcPoly, gtDstPoly, H_gt);

    const TestUvMode modes[3] = {
        TestUvMode::I420_PLANAR,
        TestUvMode::NV12_SEMIPLANAR,
        TestUvMode::NV21_SEMIPLANAR
    };
    const char* modeNames[3] = {"I420 (stride=1)", "NV12 (stride=2, UV)", "NV21 (stride=2, VU)"};

    for (int m = 0; m < 3; ++m) {
        PaddedNvBuffer frame = makeVendorPaddedYuv420(warpedScene, modes[m]);
        const std::vector<uint8_t> origY = frame.yStorage;

        const fam::MosaicProcessResult res = engine.processYuvInPlace(frame.view, 16);
        std::printf("[Test 3 - %s] matched=%d, inliers=%u, ORB=%lluus, MAGSAC=%lluus, Mosaic=%lluus, Total=%.3fms\n",
                    modeNames[m],
                    res.matchedAny ? 1 : 0,
                    res.bestInlierCount,
                    static_cast<unsigned long long>(res.downsampleAndOrbUs),
                    static_cast<unsigned long long>(res.matchAndMagsacUs),
                    static_cast<unsigned long long>(res.neonMosaicUs),
                    static_cast<double>(res.totalUs) / 1000.0);

        FAM_REQUIRE(res.matchedAny, "Homography match failed on warped scene");
        FAM_REQUIRE(res.bestInlierCount >= 20, "Insufficient USAC_MAGSAC inliers");
        FAM_REQUIRE(!res.projectedPolygons.empty(), "Missing projected polygons");

        // Check homography projection accuracy against ground truth (< 6 px corner error)
        const auto& estPoly = res.projectedPolygons[0].pixelVertices;
        FAM_REQUIRE(estPoly.size() == 4, "Expected 4 projected polygon vertices");
        for (size_t i = 0; i < 4; ++i) {
            const float err = std::hypot(estPoly[i].normX - gtDstPoly[i].x,
                                         estPoly[i].normY - gtDstPoly[i].y);
            FAM_REQUIRE(err < 6.0f, "Projected homography polygon vertex error exceeded tolerance");
        }

        // Verify center of projected sensitive box was irreversibly modified (both Y noise & UV chroma)
        const int cx = static_cast<int>((gtDstPoly[0].x + gtDstPoly[2].x) * 0.5f);
        const int cy = static_cast<int>((gtDstPoly[0].y + gtDstPoly[2].y) * 0.5f);
        int changedYCount = 0;
        std::vector<int> blockSamples;
        for (int dy = -8; dy < 8; ++dy) {
            for (int dx = -8; dx < 8; ++dx) {
                const size_t off = static_cast<size_t>(cy + dy) * frame.view.yRowStride + (cx + dx);
                if (frame.yStorage[off] != origY[off]) {
                    ++changedYCount;
                }
                blockSamples.push_back(frame.yStorage[off]);
            }
        }
        FAM_REQUIRE(changedYCount > 180, "Luma pixels inside sensitive polygon must be overwritten");

        // Verify non-zero intra-block variance from Xorshift noise (anti-Depix protection)
        const double meanSample = std::accumulate(blockSamples.begin(), blockSamples.end(), 0.0) /
                                  static_cast<double>(blockSamples.size());
        double sqDiff = 0.0;
        for (int v : blockSamples) {
            sqDiff += (v - meanSample) * (v - meanSample);
        }
        FAM_REQUIRE(sqDiff > 50.0, "Xorshift anti-reconstruction noise must inject intra-block variance");

        // Verify chroma U/V at center was neutralized away from raw (90, 195)
        const int uCenter = frame.view.uPtr[(cy / 2) * frame.view.uRowStride +
                                            (cx / 2) * frame.view.uvPixelStride];
        const int vCenter = frame.view.vPtr[(cy / 2) * frame.view.vRowStride +
                                            (cx / 2) * frame.view.uvPixelStride];
        FAM_REQUIRE(std::abs(uCenter - 128) < 20 && std::abs(vCenter - 128) < 28,
                    "UV chroma inside polygon must be desaturated and overwritten");

        // Verify untouched top-left exterior pixel [20, 20] is 100% bit-exact
        const size_t extOff = static_cast<size_t>(20) * frame.view.yRowStride + 20;
        FAM_REQUIRE(frame.yStorage[extOff] == origY[extOff],
                    "Exterior pixels outside polygon must remain bit-exact");
    }

    // -------------------------------------------------------------------------
    // 4. 12MP (4000 x 3000) Zero-RGB JPEG FD-to-FD Benchmark & <25MB Memory Check
    // -------------------------------------------------------------------------
    {
        const cv::Mat scene12Mp = createSyntheticDocumentScene(4000, 3000);
        const std::vector<cv::Point2f> qSrc = {
            {0.0f, 0.0f}, {4000.0f, 0.0f}, {4000.0f, 3000.0f}, {0.0f, 3000.0f}
        };
        const std::vector<cv::Point2f> qDst = {
            {180.0f, 120.0f}, {3860.0f, 90.0f}, {3790.0f, 2890.0f}, {220.0f, 2840.0f}
        };
        cv::Mat warped12Mp;
        cv::warpPerspective(scene12Mp, warped12Mp, cv::getPerspectiveTransform(qSrc, qDst),
                            cv::Size(4000, 3000), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(190));

        PaddedNvBuffer raw12Mp = makeVendorPaddedYuv420(warped12Mp, TestUvMode::I420_PLANAR);

        const std::string inJpegPath = "/tmp/test_12mp_input.jpg";
        const std::string outJpegPath = "/tmp/test_12mp_mosaicked.jpg";
        ::unlink(inJpegPath.c_str());
        ::unlink(outJpegPath.c_str());

        fam::JpegYuvCodec codec;
        {
            const int setupFd = ::open(inJpegPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            FAM_REQUIRE(setupFd >= 0, "Failed opening setup 12MP JPEG FD");
            std::string encErr;
            const bool ok = codec.encodeYuvViewToFd(raw12Mp.view, setupFd, 90, 6, &encErr);
            FAM_REQUIRE(ok, "encodeYuvViewToFd failed on 12MP setup");
            ::close(setupFd);
        }

        // Warm-up + 3-iteration 12MP FD-to-FD batch measurement
        fam::JpegBatchTelemetry bestTel{};
        for (int iter = 0; iter < 3; ++iter) {
            const int inFd = ::open(inJpegPath.c_str(), O_RDONLY);
            const int outFd = ::open(outJpegPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            FAM_REQUIRE(inFd >= 0 && outFd >= 0, "Failed opening 12MP batch FDs");

            const fam::JpegBatchTelemetry tel = codec.processFdToFd(engine, inFd, outFd, 90, 24);
            ::close(inFd);
            ::close(outFd);

            FAM_REQUIRE(tel.ioSuccess, "12MP processFdToFd IO failed");
            FAM_REQUIRE(tel.matchedAny, "12MP processFdToFd homography match failed");
            FAM_REQUIRE(tel.exifOrientation == 6, "EXIF Orientation 6 not parsed from input");
            if (iter == 0 || tel.totalUs < bestTel.totalUs) {
                bestTel = tel;
            }
        }

        const double peakMb = static_cast<double>(bestTel.peakBufferBytes) / (1024.0 * 1024.0);
        std::printf(
            "[Test 4 - 12MP JPEG FD->FD] %dx%d | Decode=%.2fms | ORB+MAGSAC=%.2fms | "
            "Mosaic=%.3fms | Encode=%.2fms | Total=%.2fms | PeakArena=%.2f MB\n",
            bestTel.width,
            bestTel.height,
            static_cast<double>(bestTel.jpegDecodeUs) / 1000.0,
            static_cast<double>(bestTel.homographyAndOrbUs) / 1000.0,
            static_cast<double>(bestTel.neonMosaicUs) / 1000.0,
            static_cast<double>(bestTel.jpegEncodeUs) / 1000.0,
            static_cast<double>(bestTel.totalUs) / 1000.0,
            peakMb);

        FAM_REQUIRE(peakMb < 25.0, "12MP YUV420 + JPEG codec arena must stay strictly under 25 MB");

        FILE* fOut = std::fopen(outJpegPath.c_str(), "rb");
        FAM_REQUIRE(fOut != nullptr, "Failed opening 12MP output JPEG");
        std::vector<uint8_t> outHead(256);
        const size_t nRead = std::fread(outHead.data(), 1, outHead.size(), fOut);
        std::fclose(fOut);
        outHead.resize(nRead);
        FAM_REQUIRE(fam::JpegYuvCodec::parseExifOrientation(outHead) == 6,
                    "Output JPEG failed to preserve EXIF Orientation 6");

        ::unlink(inJpegPath.c_str());
        ::unlink(outJpegPath.c_str());
    }

    ::unlink(tplPath.c_str());
    std::printf("=== ALL C++20 NATIVE PIPELINE TESTS PASSED ===\n");
    return 0;
}
