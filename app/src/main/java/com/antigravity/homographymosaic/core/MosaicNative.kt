package com.antigravity.homographymosaic.core

import android.view.Surface
import java.io.Closeable
import java.nio.ByteBuffer
import java.util.concurrent.locks.ReentrantReadWriteLock
import kotlin.concurrent.read
import kotlin.concurrent.write

data class NormalizedPoint(
    val x: Float,
    val y: Float
)

data class JpegHeaderInfo(
    val valid: Boolean,
    val rawWidth: Int,
    val rawHeight: Int,
    val uprightWidth: Int,
    val uprightHeight: Int,
    val exifOrientation: Int
)

data class LearnTelemetry(
    val success: Boolean,
    val extractedKeypoints: Int,
    val elapsedUs: Long
)

data class ColdLoadTelemetry(
    val success: Boolean,
    val loadElapsedUs: Long,
    val activeTemplateCount: Int
)

data class GalleryJpegTelemetry(
    val ioSuccess: Boolean,
    val matchedAny: Boolean,
    val templatesMatched: Int,
    val bestInliers: Int,
    val jpegDecodeUs: Long,
    val homographyAndOrbUs: Long,
    val neonMosaicUs: Long,
    val jpegEncodeUs: Long,
    val totalUs: Long,
    val peakBufferBytes: Long,
    val width: Int,
    val height: Int,
    val fixedPositionApplied: Int
)

/**
 * Low-level JNI declarations bound to libhomography_mosaic.so (C++20 + ARM NEON + POPCNT).
 * Operates strictly in YUV420 planes and ANativeWindow hardware surfaces with zero Java RGB heap objects.
 */
object MosaicNative {
    init {
        System.loadLibrary("homography_mosaic")
    }

    @JvmStatic
    external fun nativeCreateEngine(
        blockSize: Int,
        noiseAmplitude: Int
    ): Long

    @JvmStatic
    external fun nativeDestroyEngine(handle: Long)

    @JvmStatic
    external fun nativeUpdateConfig(
        handle: Long,
        blockSize: Int,
        noiseAmplitude: Int,
        loweRatio: Float,
        minInliers: Int
    )

    @JvmStatic
    external fun nativeInspectJpegFd(
        inFd: Int,
        outMeta: IntArray
    ): Boolean

    @JvmStatic
    external fun nativeRenderJpegFdToSurface(
        inFd: Int,
        surface: Surface,
        maxPreviewDim: Int
    ): Boolean

    @JvmStatic
    external fun nativeLearnTemplateFromJpegFd(
        handle: Long,
        inFd: Int,
        polygonCounts: IntArray,
        flatUprightVertices: FloatArray,
        outputTplPath: String,
        templateId: String,
        outStats: LongArray
    ): Boolean

    @JvmStatic
    external fun nativeLearnTemplateFromYPlane(
        handle: Long,
        yBuffer: ByteBuffer,
        width: Int,
        height: Int,
        yRowStride: Int,
        polygonCounts: IntArray,
        flatNormalizedVertices: FloatArray,
        outputTplPath: String,
        templateId: String,
        outStats: LongArray
    ): Boolean

    @JvmStatic
    external fun nativeLoadTemplateFile(
        handle: Long,
        templateId: String,
        famTplPath: String,
        outStats: LongArray
    ): Boolean

    @JvmStatic
    external fun nativeUnloadTemplate(
        handle: Long,
        templateId: String
    ): Boolean

    @JvmStatic
    external fun nativeClearTemplates(handle: Long)

    @JvmStatic
    external fun nativeGetLoadedTemplateCount(handle: Long): Int

    @JvmStatic
    external fun nativeProcessYuvInPlace(
        handle: Long,
        yBuffer: ByteBuffer,
        uBuffer: ByteBuffer,
        vBuffer: ByteBuffer,
        width: Int,
        height: Int,
        yRowStride: Int,
        uRowStride: Int,
        vRowStride: Int,
        uvPixelStride: Int,
        blockSize: Int,
        outStats: LongArray
    ): Int

    @JvmStatic
    external fun nativeProcessJpegFdToFd(
        handle: Long,
        inFd: Int,
        outFd: Int,
        jpegQuality: Int,
        blockSize: Int,
        outStats: LongArray
    ): Boolean

    /**
     * Zero-pixel header + EXIF orientation inspection directly from a ParcelFileDescriptor.
     */
    fun inspectJpegFd(inFd: Int): JpegHeaderInfo {
        if (inFd < 0) {
            return JpegHeaderInfo(false, 0, 0, 0, 0, 1)
        }
        val meta = IntArray(5)
        val ok = nativeInspectJpegFd(inFd, meta)
        return if (ok) {
            JpegHeaderInfo(
                valid = true,
                rawWidth = meta[0],
                rawHeight = meta[1],
                uprightWidth = meta[2],
                uprightHeight = meta[3],
                exifOrientation = meta[4]
            )
        } else {
            JpegHeaderInfo(false, 0, 0, 0, 0, 1)
        }
    }

    /**
     * Renders an EXIF-upright IDCT-downscaled preview directly into an ANativeWindow Surface.
     */
    fun renderJpegFdToSurface(
        inFd: Int,
        surface: Surface,
        maxPreviewDim: Int = 1600
    ): Boolean {
        if (inFd < 0 || !surface.isValid) return false
        return nativeRenderJpegFdToSurface(inFd, surface, maxPreviewDim)
    }
}

/**
 * Thread-safe RAII session managing a native C++20 MosaicEngine handle.
 * Uses ReentrantReadWriteLock so batch workers execute concurrently while
 * lifecycle destruction and template mutations remain strictly serialized.
 */
class MosaicEngineSession(
    initialBlockSize: Int = 32,
    initialNoiseAmplitude: Int = 18
) : Closeable {

    private val rwLock = ReentrantReadWriteLock()
    private var currentBlockSize: Int = initialBlockSize
    private var currentNoiseAmplitude: Int = initialNoiseAmplitude
    private var currentLoweRatio: Float = 0.82f
    private var currentMinInliers: Int = 8

    private var nativeHandle: Long = MosaicNative.nativeCreateEngine(
        blockSize = initialBlockSize,
        noiseAmplitude = initialNoiseAmplitude
    )

    private val threadLocalStats = object : ThreadLocal<LongArray>() {
        override fun initialValue(): LongArray = LongArray(12)
    }

    fun updateParameters(
        blockSize: Int = currentBlockSize,
        noiseAmplitude: Int = currentNoiseAmplitude,
        loweRatio: Float = currentLoweRatio,
        minInliers: Int = currentMinInliers
    ) = rwLock.write {
        currentBlockSize = blockSize
        currentNoiseAmplitude = noiseAmplitude
        currentLoweRatio = loweRatio
        currentMinInliers = minInliers
        if (nativeHandle != 0L) {
            MosaicNative.nativeUpdateConfig(
                handle = nativeHandle,
                blockSize = currentBlockSize,
                noiseAmplitude = currentNoiseAmplitude,
                loweRatio = currentLoweRatio,
                minInliers = currentMinInliers
            )
        }
    }

    /**
     * Learns a FAMTPL02 binary template directly from a gallery JPEG file descriptor in YUV420 space,
     * mapping upright [0..1] polygon coordinates into raw sensor coordinates via EXIF orientation.
     */
    fun learnTemplateFromJpegFd(
        inFd: Int,
        uprightPolygons: List<List<NormalizedPoint>>,
        outputTplPath: String,
        templateId: String
    ): LearnTelemetry = rwLock.write {
        check(nativeHandle != 0L) { "MosaicEngineSession already closed" }
        val validPolys = uprightPolygons.filter { it.size >= 3 }
        if (validPolys.isEmpty() || inFd < 0) {
            return@write LearnTelemetry(false, 0, 0L)
        }

        val polyCounts = IntArray(validPolys.size) { idx -> validPolys[idx].size }
        val totalVerts = validPolys.sumOf { it.size }
        val flatCoords = FloatArray(totalVerts * 2)
        var cursor = 0
        for (poly in validPolys) {
            for (pt in poly) {
                flatCoords[cursor++] = pt.x.coerceIn(0f, 1f)
                flatCoords[cursor++] = pt.y.coerceIn(0f, 1f)
            }
        }

        val stats = threadLocalStats.get()!!
        stats.fill(0L)
        val ok = MosaicNative.nativeLearnTemplateFromJpegFd(
            handle = nativeHandle,
            inFd = inFd,
            polygonCounts = polyCounts,
            flatUprightVertices = flatCoords,
            outputTplPath = outputTplPath,
            templateId = templateId,
            outStats = stats
        )
        LearnTelemetry(
            success = ok,
            extractedKeypoints = stats[0].toInt(),
            elapsedUs = stats[1]
        )
    }

    fun loadTemplateMmap(templateId: String, famTplPath: String): ColdLoadTelemetry = rwLock.write {
        if (nativeHandle == 0L) return@write ColdLoadTelemetry(false, 0L, 0)
        val stats = threadLocalStats.get()!!
        stats.fill(0L)
        val ok = MosaicNative.nativeLoadTemplateFile(
            handle = nativeHandle,
            templateId = templateId,
            famTplPath = famTplPath,
            outStats = stats
        )
        ColdLoadTelemetry(
            success = ok,
            loadElapsedUs = stats[0],
            activeTemplateCount = stats[1].toInt()
        )
    }

    fun unloadTemplate(templateId: String): Boolean = rwLock.write {
        if (nativeHandle == 0L) false else MosaicNative.nativeUnloadTemplate(nativeHandle, templateId)
    }

    fun clearAllTemplates() = rwLock.write {
        if (nativeHandle != 0L) {
            MosaicNative.nativeClearTemplates(nativeHandle)
        }
    }

    fun activeTemplateCount(): Int = rwLock.read {
        if (nativeHandle == 0L) 0 else MosaicNative.nativeGetLoadedTemplateCount(nativeHandle)
    }

    /**
     * Ultra-fast Gallery single/batch JPEG FD-to-FD pipeline in YUV420 planes.
     */
    fun processGalleryJpegFdToFd(
        inFd: Int,
        outFd: Int,
        jpegQuality: Int = 92,
        blockSize: Int = 16
    ): GalleryJpegTelemetry = rwLock.read {
        if (nativeHandle == 0L) {
            return@read GalleryJpegTelemetry(
                false, false, 0, 0, 0L, 0L, 0L, 0L, 0L, 0L, 0, 0, 0
            )
        }
        val stats = threadLocalStats.get()!!
        stats.fill(0L)
        val ok = MosaicNative.nativeProcessJpegFdToFd(
            handle = nativeHandle,
            inFd = inFd,
            outFd = outFd,
            jpegQuality = jpegQuality,
            blockSize = blockSize,
            outStats = stats
        )

        GalleryJpegTelemetry(
            ioSuccess = ok,
            matchedAny = stats[0] == 1L,
            templatesMatched = stats[1].toInt(),
            bestInliers = stats[2].toInt(),
            jpegDecodeUs = stats[3],
            homographyAndOrbUs = stats[4],
            neonMosaicUs = stats[5],
            jpegEncodeUs = stats[6],
            totalUs = stats[7],
            peakBufferBytes = stats[8],
            width = stats[9].toInt(),
            height = stats[10].toInt(),
            fixedPositionApplied = stats[11].toInt()
        )
    }

    override fun close() = rwLock.write {
        if (nativeHandle != 0L) {
            MosaicNative.nativeDestroyEngine(nativeHandle)
            nativeHandle = 0L
        }
    }
}
