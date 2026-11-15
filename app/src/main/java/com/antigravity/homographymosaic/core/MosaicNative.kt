package com.antigravity.homographymosaic.core

import androidx.camera.core.ImageProxy
import java.io.Closeable
import java.nio.ByteBuffer
import java.util.concurrent.locks.ReentrantReadWriteLock
import kotlin.concurrent.read
import kotlin.concurrent.write

data class NormalizedPoint(
    val x: Float,
    val y: Float
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

data class CameraCaptureMosaicTelemetry(
    val ioSuccess: Boolean,
    val matchedAny: Boolean,
    val templatesMatched: Int,
    val bestInliers: Int,
    val orbAndHomographyUs: Long,
    val neonMosaicUs: Long,
    val jpegEncodeUs: Long,
    val totalUs: Long
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
    val height: Int
)

/**
 * Low-level JNI declarations bound to libhomography_mosaic.so (C++20 + ARM NEON + POPCNT).
 * Zero android.graphics.Bitmap imports or RGB conversions exist anywhere in this pipeline.
 */
object MosaicNative {
    init {
        System.loadLibrary("homography_mosaic")
    }

    @JvmStatic
    external fun nativeCreateEngine(blockSize: Int, noiseAmplitude: Int): Long

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
    external fun nativeComputeSharpness(
        yBuffer: ByteBuffer,
        width: Int,
        height: Int,
        yRowStride: Int
    ): Float

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
    external fun nativeProcessCameraYuvAndSaveJpegFd(
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
        rotationDegrees: Int,
        outFd: Int,
        jpegQuality: Int,
        blockSize: Int,
        outStats: LongArray
    ): Boolean

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
     * Zero-allocation sharpness calculation directly on CameraX YUV_420_888 Plane[0].
     */
    fun computeSharpnessZeroCopy(image: ImageProxy): Float {
        val planes = image.planes
        if (planes.isEmpty()) return 0f
        val yPlane = planes[0]
        val yBuf = zeroCopyBaseBuffer(yPlane.buffer)
        if (!yBuf.isDirect) return 0f
        return nativeComputeSharpness(
            yBuffer = yBuf,
            width = image.width,
            height = image.height,
            yRowStride = yPlane.rowStride
        )
    }

    internal fun zeroCopyBaseBuffer(buffer: ByteBuffer): ByteBuffer {
        return if (buffer.position() == 0) buffer else buffer.slice()
    }
}

/**
 * Thread-safe RAII session managing a native C++20 MosaicEngine handle.
 * Uses ReentrantReadWriteLock so multi-core batch workers execute concurrently while
 * lifecycle destruction and template mutations remain strictly serialized.
 */
class MosaicEngineSession(
    initialBlockSize: Int = 16,
    initialNoiseAmplitude: Int = 18
) : Closeable {

    private val rwLock = ReentrantReadWriteLock()
    private var nativeHandle: Long = MosaicNative.nativeCreateEngine(
        blockSize = initialBlockSize,
        noiseAmplitude = initialNoiseAmplitude
    )

    private val threadLocalStats = object : ThreadLocal<LongArray>() {
        override fun initialValue(): LongArray = LongArray(12)
    }

    fun updateParameters(
        blockSize: Int,
        noiseAmplitude: Int = 18,
        loweRatio: Float = 0.78f,
        minInliers: Int = 10
    ) = rwLock.write {
        if (nativeHandle != 0L) {
            MosaicNative.nativeUpdateConfig(
                handle = nativeHandle,
                blockSize = blockSize,
                noiseAmplitude = noiseAmplitude,
                loweRatio = loweRatio,
                minInliers = minInliers
            )
        }
    }

    fun learnTemplateFromImageProxy(
        image: ImageProxy,
        normalizedPolygons: List<List<NormalizedPoint>>,
        outputTplPath: String,
        templateId: String
    ): LearnTelemetry = rwLock.write {
        check(nativeHandle != 0L) { "MosaicEngineSession already closed" }
        val validPolys = normalizedPolygons.filter { it.size >= 3 }
        if (validPolys.isEmpty() || image.planes.isEmpty()) {
            return@write LearnTelemetry(false, 0, 0L)
        }

        val yPlane = image.planes[0]
        val yBuf = MosaicNative.zeroCopyBaseBuffer(yPlane.buffer)
        require(yBuf.isDirect) { "CameraX Y plane must be a DirectByteBuffer" }

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
        val ok = MosaicNative.nativeLearnTemplateFromYPlane(
            handle = nativeHandle,
            yBuffer = yBuf,
            width = image.width,
            height = image.height,
            yRowStride = yPlane.rowStride,
            polygonCounts = polyCounts,
            flatNormalizedVertices = flatCoords,
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
     * Zero-copy CameraX YUV_420_888 in-place Homography Auto-Mosaic + direct JPEG FD output.
     * Automatically handles vendor rowStride padding & I420 (stride=1) / NV12 / NV21 (stride=2).
     */
    fun processCameraYuvToJpegFd(
        image: ImageProxy,
        outFd: Int,
        jpegQuality: Int = 92,
        blockSize: Int = 16
    ): CameraCaptureMosaicTelemetry = rwLock.read {
        if (nativeHandle == 0L || image.planes.size < 3) {
            return@read CameraCaptureMosaicTelemetry(false, false, 0, 0, 0L, 0L, 0L, 0L)
        }

        val yPlane = image.planes[0]
        val uPlane = image.planes[1]
        val vPlane = image.planes[2]

        val stats = threadLocalStats.get()!!
        stats.fill(0L)
        val ok = MosaicNative.nativeProcessCameraYuvAndSaveJpegFd(
            handle = nativeHandle,
            yBuffer = MosaicNative.zeroCopyBaseBuffer(yPlane.buffer),
            uBuffer = MosaicNative.zeroCopyBaseBuffer(uPlane.buffer),
            vBuffer = MosaicNative.zeroCopyBaseBuffer(vPlane.buffer),
            width = image.width,
            height = image.height,
            yRowStride = yPlane.rowStride,
            uRowStride = uPlane.rowStride,
            vRowStride = vPlane.rowStride,
            uvPixelStride = uPlane.pixelStride,
            rotationDegrees = image.imageInfo.rotationDegrees,
            outFd = outFd,
            jpegQuality = jpegQuality,
            blockSize = blockSize,
            outStats = stats
        )

        CameraCaptureMosaicTelemetry(
            ioSuccess = ok,
            matchedAny = stats[0] == 1L,
            templatesMatched = stats[1].toInt(),
            bestInliers = stats[2].toInt(),
            orbAndHomographyUs = stats[3],
            neonMosaicUs = stats[4],
            jpegEncodeUs = stats[5],
            totalUs = stats[6]
        )
    }

    /**
     * Ultra-fast Gallery single/batch JPEG FD-to-FD pipeline (zero Bitmap / zero RGB).
     */
    fun processGalleryJpegFdToFd(
        inFd: Int,
        outFd: Int,
        jpegQuality: Int = 92,
        blockSize: Int = 16
    ): GalleryJpegTelemetry = rwLock.read {
        if (nativeHandle == 0L) {
            return@read GalleryJpegTelemetry(false, false, 0, 0, 0L, 0L, 0L, 0L, 0L, 0L, 0, 0)
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
            height = stats[10].toInt()
        )
    }

    override fun close() = rwLock.write {
        if (nativeHandle != 0L) {
            MosaicNative.nativeDestroyEngine(nativeHandle)
            nativeHandle = 0L
        }
    }
}
