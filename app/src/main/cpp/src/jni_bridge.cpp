#include "famtpl_format.hpp"
#include "jpeg_yuv_codec.hpp"
#include "mosaic_engine.hpp"

#include <jni.h>

#if defined(__ANDROID__)
#include <android/log.h>
#define FAM_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "HomographyMosaicJNI", __VA_ARGS__)
#define FAM_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "HomographyMosaicJNI", __VA_ARGS__)
#else
#include <cstdio>
#define FAM_LOGI(...) do { std::fprintf(stdout, "[HomographyMosaicJNI] " __VA_ARGS__); std::fprintf(stdout, "\n"); } while (0)
#define FAM_LOGE(...) do { std::fprintf(stderr, "[HomographyMosaicJNI ERROR] " __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace {

using SteadyClock = std::chrono::steady_clock;

struct NativeEngineContext {
    fam::MosaicEngine engine;
    explicit NativeEngineContext(const fam::MosaicConfig& cfg) : engine(cfg) {}
};

// Thread-local JpegYuvCodec reuses 64B-aligned YUV arena & compressed scratch buffer per worker thread
fam::JpegYuvCodec& threadLocalCodec() {
    thread_local fam::JpegYuvCodec codec;
    return codec;
}

[[nodiscard]] std::string jstringToStdString(JNIEnv* env, jstring jstr) {
    if (!jstr) {
        return {};
    }
    const char* utf = env->GetStringUTFChars(jstr, nullptr);
    if (!utf) {
        return {};
    }
    std::string out(utf);
    env->ReleaseStringUTFChars(jstr, utf);
    return out;
}

[[nodiscard]] bool validateYPlaneCapacity(
    jlong cap, int32_t width, int32_t height, int32_t rowStride) noexcept {
    if (width < 16 || height < 16 || rowStride < width || cap <= 0) {
        return false;
    }
    const int64_t minRequired =
        static_cast<int64_t>(height - 1) * static_cast<int64_t>(rowStride) + width;
    return cap >= minRequired;
}

[[nodiscard]] bool validateYuv420DirectBuffers(
    JNIEnv* env,
    jobject yBuffer,
    jobject uBuffer,
    jobject vBuffer,
    int32_t width,
    int32_t height,
    int32_t yRowStride,
    int32_t uRowStride,
    int32_t vRowStride,
    int32_t uvPixelStride,
    fam::YuvPlaneView& outView) noexcept {

    if (!yBuffer || !uBuffer || !vBuffer) {
        return false;
    }
    auto* yPtr = static_cast<uint8_t*>(env->GetDirectBufferAddress(yBuffer));
    auto* uPtr = static_cast<uint8_t*>(env->GetDirectBufferAddress(uBuffer));
    auto* vPtr = static_cast<uint8_t*>(env->GetDirectBufferAddress(vBuffer));
    if (!yPtr || !uPtr || !vPtr) {
        return false;
    }

    const jlong yCap = env->GetDirectBufferCapacity(yBuffer);
    const jlong uCap = env->GetDirectBufferCapacity(uBuffer);
    const jlong vCap = env->GetDirectBufferCapacity(vBuffer);

    if (!validateYPlaneCapacity(yCap, width, height, yRowStride)) {
        return false;
    }
    if (uvPixelStride != 1 && uvPixelStride != 2) {
        return false;
    }

    const int64_t cw = (width + 1) / 2;
    const int64_t ch = (height + 1) / 2;
    const int64_t minUBytes = (ch - 1) * static_cast<int64_t>(uRowStride) +
                              (cw - 1) * static_cast<int64_t>(uvPixelStride) + 1;
    const int64_t minVBytes = (ch - 1) * static_cast<int64_t>(vRowStride) +
                              (cw - 1) * static_cast<int64_t>(uvPixelStride) + 1;

    if (uCap < minUBytes || vCap < minVBytes) {
        return false;
    }

    outView.yPtr = yPtr;
    outView.uPtr = uPtr;
    outView.vPtr = vPtr;
    outView.width = width;
    outView.height = height;
    outView.yRowStride = yRowStride;
    outView.uRowStride = uRowStride;
    outView.vRowStride = vRowStride;
    outView.uvPixelStride = uvPixelStride;
    outView.subsampling = fam::ChromaSubsampling::YUV420;
    return outView.isValid();
}

[[nodiscard]] bool unpackJniPolygons(
    JNIEnv* env,
    jintArray polygonCounts,
    jfloatArray flatVertices,
    std::vector<fam::NormalizedPolygon>& outPolys) {

    if (!polygonCounts || !flatVertices) {
        return false;
    }
    const jsize numPolys = env->GetArrayLength(polygonCounts);
    const jsize numFloats = env->GetArrayLength(flatVertices);
    if (numPolys <= 0 || numFloats < 6 || (numFloats % 2) != 0) {
        return false;
    }

    std::vector<jint> counts(static_cast<size_t>(numPolys));
    std::vector<jfloat> coords(static_cast<size_t>(numFloats));
    env->GetIntArrayRegion(polygonCounts, 0, numPolys, counts.data());
    env->GetFloatArrayRegion(flatVertices, 0, numFloats, coords.data());

    outPolys.clear();
    outPolys.reserve(static_cast<size_t>(numPolys));

    size_t floatIdx = 0;
    for (jsize p = 0; p < numPolys; ++p) {
        const jint vCount = counts[static_cast<size_t>(p)];
        if (vCount < 3 || floatIdx + static_cast<size_t>(vCount) * 2 > coords.size()) {
            return false;
        }
        fam::NormalizedPolygon poly;
        poly.vertices.reserve(static_cast<size_t>(vCount));
        for (jint i = 0; i < vCount; ++i) {
            poly.vertices.push_back(fam::FamTplVertex{
                coords[floatIdx],
                coords[floatIdx + 1]
            });
            floatIdx += 2;
        }
        outPolys.push_back(std::move(poly));
    }
    return true;
}

void writeStatsArray(JNIEnv* env, jlongArray outStats, std::initializer_list<jlong> values) {
    if (!outStats) {
        return;
    }
    const jsize len = env->GetArrayLength(outStats);
    std::vector<jlong> tmp;
    tmp.reserve(values.size());
    for (jlong v : values) {
        if (static_cast<jsize>(tmp.size()) >= len) break;
        tmp.push_back(v);
    }
    if (!tmp.empty()) {
        env->SetLongArrayRegion(outStats, 0, static_cast<jsize>(tmp.size()), tmp.data());
    }
}

} // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_antigravity_homographymosaic_core_MosaicNative_nativeCreateEngine(
    JNIEnv* /*env*/,
    jobject /*thiz*/,
    jint blockSize,
    jint noiseAmplitude) {

    try {
        fam::MosaicConfig cfg{};
        cfg.mosaicBlockSize = std::clamp(static_cast<int32_t>(blockSize), 8, 64) & ~1;
        cfg.noiseAmplitude =
            static_cast<uint8_t>(std::clamp(static_cast<int32_t>(noiseAmplitude), 0, 64));
        auto* ctx = new NativeEngineContext(cfg);
        return reinterpret_cast<jlong>(ctx);
    } catch (const std::exception& e) {
        FAM_LOGE("nativeCreateEngine exception: %s", e.what());
        return 0;
    }
}

JNIEXPORT void JNICALL
Java_com_antigravity_homographymosaic_core_MosaicNative_nativeDestroyEngine(
    JNIEnv* /*env*/,
    jobject /*thiz*/,
    jlong handle) {

    auto* ctx = reinterpret_cast<NativeEngineContext*>(handle);
    delete ctx;
}

JNIEXPORT void JNICALL
Java_com_antigravity_homographymosaic_core_MosaicNative_nativeUpdateConfig(
    JNIEnv* /*env*/,
    jobject /*thiz*/,
    jlong handle,
    jint blockSize,
    jint noiseAmplitude,
    jfloat loweRatio,
    jint minInliers) {

    auto* ctx = reinterpret_cast<NativeEngineContext*>(handle);
    if (!ctx) {
        return;
    }
    try {
        fam::MosaicConfig cfg = ctx->engine.getConfig();
        cfg.mosaicBlockSize = std::clamp(static_cast<int32_t>(blockSize), 8, 64) & ~1;
        cfg.noiseAmplitude =
            static_cast<uint8_t>(std::clamp(static_cast<int32_t>(noiseAmplitude), 0, 64));
        cfg.loweRatio = std::clamp(static_cast<float>(loweRatio), 0.55f, 0.92f);
        cfg.minInliers = std::clamp(static_cast<int32_t>(minInliers), 6, 100);
        ctx->engine.updateConfig(cfg);
    } catch (const std::exception& e) {
        FAM_LOGE("nativeUpdateConfig exception: %s", e.what());
    }
}

JNIEXPORT jfloat JNICALL
Java_com_antigravity_homographymosaic_core_MosaicNative_nativeComputeSharpness(
    JNIEnv* env,
    jobject /*thiz*/,
    jobject yBuffer,
    jint width,
    jint height,
    jint yRowStride) {

    if (!yBuffer) {
        return 0.0f;
    }
    const auto* yPtr = static_cast<const uint8_t*>(env->GetDirectBufferAddress(yBuffer));
    const jlong yCap = env->GetDirectBufferCapacity(yBuffer);
    if (!yPtr || !validateYPlaneCapacity(yCap, width, height, yRowStride)) {
        return 0.0f;
    }

    const fam::YPlaneConstView yView{yPtr, width, height, yRowStride};
    return fam::MosaicEngine::computeSharpnessScore(yView);
}

JNIEXPORT jboolean JNICALL
Java_com_antigravity_homographymosaic_core_MosaicNative_nativeLearnTemplateFromYPlane(
    JNIEnv* env,
    jobject /*thiz*/,
    jlong handle,
    jobject yBuffer,
    jint width,
    jint height,
    jint yRowStride,
    jintArray polygonCounts,
    jfloatArray flatNormalizedVertices,
    jstring outputTplPath,
    jstring templateId,
    jlongArray outStats) {

    try {
        auto* ctx = reinterpret_cast<NativeEngineContext*>(handle);
        if (!ctx || !yBuffer) {
            return JNI_FALSE;
        }

        const auto* yPtr = static_cast<const uint8_t*>(env->GetDirectBufferAddress(yBuffer));
        const jlong yCap = env->GetDirectBufferCapacity(yBuffer);
        if (!yPtr || !validateYPlaneCapacity(yCap, width, height, yRowStride)) {
            FAM_LOGE("nativeLearnTemplateFromYPlane: invalid Y DirectByteBuffer or stride");
            return JNI_FALSE;
        }

        std::vector<fam::NormalizedPolygon> polygons;
        if (!unpackJniPolygons(env, polygonCounts, flatNormalizedVertices, polygons)) {
            FAM_LOGE("nativeLearnTemplateFromYPlane: invalid polygon vertices");
            return JNI_FALSE;
        }

        const std::string outPath = jstringToStdString(env, outputTplPath);
        const std::string tplId = jstringToStdString(env, templateId);
        if (outPath.empty()) {
            return JNI_FALSE;
        }

        const fam::YPlaneConstView yView{yPtr, width, height, yRowStride};
        const fam::LearnResult res =
            ctx->engine.learnTemplateToFile(yView, polygons, outPath, tplId);

        writeStatsArray(env, outStats, {
            static_cast<jlong>(res.extractedKeypoints),
            static_cast<jlong>(res.elapsedUs)
        });

        if (!res.success) {
            FAM_LOGE("learnTemplateToFile failed: %s", res.errorMessage.c_str());
            return JNI_FALSE;
        }
        return JNI_TRUE;
    } catch (const std::exception& e) {
        FAM_LOGE("nativeLearnTemplateFromYPlane exception: %s", e.what());
        return JNI_FALSE;
    }
}

JNIEXPORT jboolean JNICALL
Java_com_antigravity_homographymosaic_core_MosaicNative_nativeLoadTemplateFile(
    JNIEnv* env,
    jobject /*thiz*/,
    jlong handle,
    jstring templateId,
    jstring famTplPath,
    jlongArray outStats) {

    try {
        auto* ctx = reinterpret_cast<NativeEngineContext*>(handle);
        if (!ctx) {
            return JNI_FALSE;
        }

        const std::string tplId = jstringToStdString(env, templateId);
        const std::string path = jstringToStdString(env, famTplPath);
        if (tplId.empty() || path.empty()) {
            return JNI_FALSE;
        }

        uint64_t loadUs = 0;
        std::string err;
        if (!ctx->engine.loadTemplateFromFile(tplId, path, &loadUs, &err)) {
            FAM_LOGE("loadTemplateFromFile(%s) failed: %s", path.c_str(), err.c_str());
            return JNI_FALSE;
        }

        writeStatsArray(env, outStats, {
            static_cast<jlong>(loadUs),
            static_cast<jlong>(ctx->engine.loadedTemplateCount())
        });
        return JNI_TRUE;
    } catch (const std::exception& e) {
        FAM_LOGE("nativeLoadTemplateFile exception: %s", e.what());
        return JNI_FALSE;
    }
}

JNIEXPORT jboolean JNICALL
Java_com_antigravity_homographymosaic_core_MosaicNative_nativeUnloadTemplate(
    JNIEnv* env,
    jobject /*thiz*/,
    jlong handle,
    jstring templateId) {

    auto* ctx = reinterpret_cast<NativeEngineContext*>(handle);
    if (!ctx) {
        return JNI_FALSE;
    }
    return ctx->engine.unloadTemplate(jstringToStdString(env, templateId)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_antigravity_homographymosaic_core_MosaicNative_nativeClearTemplates(
    JNIEnv* /*env*/,
    jobject /*thiz*/,
    jlong handle) {

    auto* ctx = reinterpret_cast<NativeEngineContext*>(handle);
    if (ctx) {
        ctx->engine.clearTemplates();
    }
}

JNIEXPORT jint JNICALL
Java_com_antigravity_homographymosaic_core_MosaicNative_nativeGetLoadedTemplateCount(
    JNIEnv* /*env*/,
    jobject /*thiz*/,
    jlong handle) {

    auto* ctx = reinterpret_cast<NativeEngineContext*>(handle);
    return ctx ? static_cast<jint>(ctx->engine.loadedTemplateCount()) : 0;
}

JNIEXPORT jint JNICALL
Java_com_antigravity_homographymosaic_core_MosaicNative_nativeProcessYuvInPlace(
    JNIEnv* env,
    jobject /*thiz*/,
    jlong handle,
    jobject yBuffer,
    jobject uBuffer,
    jobject vBuffer,
    jint width,
    jint height,
    jint yRowStride,
    jint uRowStride,
    jint vRowStride,
    jint uvPixelStride,
    jint blockSize,
    jlongArray outStats) {

    try {
        auto* ctx = reinterpret_cast<NativeEngineContext*>(handle);
        if (!ctx) {
            return -1;
        }

        fam::YuvPlaneView view{};
        if (!validateYuv420DirectBuffers(
                env, yBuffer, uBuffer, vBuffer,
                width, height, yRowStride, uRowStride, vRowStride, uvPixelStride, view)) {
            FAM_LOGE("nativeProcessYuvInPlace: invalid YUV_420_888 plane strides or buffer capacity");
            return -1;
        }

        const fam::MosaicProcessResult res = ctx->engine.processYuvInPlace(view, blockSize);
        writeStatsArray(env, outStats, {
            res.matchedAny ? 1LL : 0LL,
            static_cast<jlong>(res.templatesMatched),
            static_cast<jlong>(res.bestInlierCount),
            static_cast<jlong>(res.downsampleAndOrbUs + res.matchAndMagsacUs),
            static_cast<jlong>(res.neonMosaicUs),
            static_cast<jlong>(res.totalUs)
        });
        return static_cast<jint>(res.templatesMatched);
    } catch (const std::exception& e) {
        FAM_LOGE("nativeProcessYuvInPlace exception: %s", e.what());
        return -1;
    }
}

JNIEXPORT jboolean JNICALL
Java_com_antigravity_homographymosaic_core_MosaicNative_nativeProcessCameraYuvAndSaveJpegFd(
    JNIEnv* env,
    jobject /*thiz*/,
    jlong handle,
    jobject yBuffer,
    jobject uBuffer,
    jobject vBuffer,
    jint width,
    jint height,
    jint yRowStride,
    jint uRowStride,
    jint vRowStride,
    jint uvPixelStride,
    jint rotationDegrees,
    jint outFd,
    jint jpegQuality,
    jint blockSize,
    jlongArray outStats) {

    try {
        const auto tStart = SteadyClock::now();
        auto* ctx = reinterpret_cast<NativeEngineContext*>(handle);
        if (!ctx || outFd < 0) {
            return JNI_FALSE;
        }

        fam::YuvPlaneView view{};
        if (!validateYuv420DirectBuffers(
                env, yBuffer, uBuffer, vBuffer,
                width, height, yRowStride, uRowStride, vRowStride, uvPixelStride, view)) {
            FAM_LOGE("nativeProcessCameraYuvAndSaveJpegFd: invalid YUV_420_888 DirectByteBuffers");
            return JNI_FALSE;
        }

        const fam::MosaicProcessResult proc = ctx->engine.processYuvInPlace(view, blockSize);

        const auto tEncStart = SteadyClock::now();
        const uint16_t exifOrient =
            fam::JpegYuvCodec::rotationDegreesToExifOrientation(rotationDegrees);
        std::string err;
        if (!threadLocalCodec().encodeYuvViewToFd(view, outFd, jpegQuality, exifOrient, &err)) {
            FAM_LOGE("encodeYuvViewToFd failed: %s", err.c_str());
            return JNI_FALSE;
        }
        const auto encUs = std::chrono::duration_cast<std::chrono::microseconds>(
                               SteadyClock::now() - tEncStart)
                               .count();
        const auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                 SteadyClock::now() - tStart)
                                 .count();

        writeStatsArray(env, outStats, {
            proc.matchedAny ? 1LL : 0LL,
            static_cast<jlong>(proc.templatesMatched),
            static_cast<jlong>(proc.bestInlierCount),
            static_cast<jlong>(proc.downsampleAndOrbUs + proc.matchAndMagsacUs),
            static_cast<jlong>(proc.neonMosaicUs),
            static_cast<jlong>(encUs),
            static_cast<jlong>(totalUs)
        });
        return JNI_TRUE;
    } catch (const std::exception& e) {
        FAM_LOGE("nativeProcessCameraYuvAndSaveJpegFd exception: %s", e.what());
        return JNI_FALSE;
    }
}

JNIEXPORT jboolean JNICALL
Java_com_antigravity_homographymosaic_core_MosaicNative_nativeProcessJpegFdToFd(
    JNIEnv* env,
    jobject /*thiz*/,
    jlong handle,
    jint inFd,
    jint outFd,
    jint jpegQuality,
    jint blockSize,
    jlongArray outStats) {

    try {
        auto* ctx = reinterpret_cast<NativeEngineContext*>(handle);
        if (!ctx || inFd < 0 || outFd < 0) {
            return JNI_FALSE;
        }

        const fam::JpegBatchTelemetry tel =
            threadLocalCodec().processFdToFd(ctx->engine, inFd, outFd, jpegQuality, blockSize);

        writeStatsArray(env, outStats, {
            tel.matchedAny ? 1LL : 0LL,
            static_cast<jlong>(tel.templatesMatched),
            static_cast<jlong>(tel.bestInliers),
            static_cast<jlong>(tel.jpegDecodeUs),
            static_cast<jlong>(tel.homographyAndOrbUs),
            static_cast<jlong>(tel.neonMosaicUs),
            static_cast<jlong>(tel.jpegEncodeUs),
            static_cast<jlong>(tel.totalUs),
            static_cast<jlong>(tel.peakBufferBytes),
            static_cast<jlong>(tel.width),
            static_cast<jlong>(tel.height)
        });

        if (!tel.ioSuccess) {
            FAM_LOGE("nativeProcessJpegFdToFd failed: %s", tel.message.c_str());
            return JNI_FALSE;
        }
        return JNI_TRUE;
    } catch (const std::exception& e) {
        FAM_LOGE("nativeProcessJpegFdToFd exception: %s", e.what());
        return JNI_FALSE;
    }
}

} // extern "C"
