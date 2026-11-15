package com.antigravity.homographymosaic.ui

import android.content.ContentValues
import android.content.Context
import android.graphics.ImageFormat
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageCapture
import androidx.camera.core.ImageCaptureException
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.AutoFixHigh
import androidx.compose.material.icons.filled.CameraAlt
import androidx.compose.material.icons.filled.CenterFocusStrong
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.CropFree
import androidx.compose.material.icons.filled.WarningAmber
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.antigravity.homographymosaic.core.MosaicEngineSession
import com.antigravity.homographymosaic.core.MosaicNative
import com.antigravity.homographymosaic.core.TemplateRepository
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.Executors

private enum class CameraWorkMode {
    AUTO_MOSAIC_SHUTTER,
    LEARN_FAMTPL02
}

private const val ANTI_SHAKE_SHARPNESS_THRESHOLD = 13.5f

@Composable
fun CameraCaptureScreen(
    engineSession: MosaicEngineSession,
    templateRepository: TemplateRepository,
    blockSize: Int,
    jpegQuality: Int,
    modifier: Modifier = Modifier
) {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val coroutineScope = rememberCoroutineScope()

    val templates by templateRepository.templates.collectAsState()
    val activeTemplatesCount = templates.count { it.enabled }

    var workMode by remember { mutableStateOf(CameraWorkMode.AUTO_MOSAIC_SHUTTER) }
    var sharpnessEma by remember { mutableFloatStateOf(0f) }
    var antiShakeGateEnabled by remember { mutableStateOf(true) }
    var isCapturing by remember { mutableStateOf(false) }
    var statusBanner by remember {
        mutableStateOf("YUV_420_888 零拷貝管線就緒（無 Bitmap / 純 CPU NEON）")
    }

    var templateNameInput by remember { mutableStateOf("") }
    var overlayPolygonState by remember {
        mutableStateOf(OverlayPolygonState(emptyList(), 0f, 0f))
    }

    val cameraExecutor = remember { Executors.newSingleThreadExecutor() }
    val previewView = remember {
        PreviewView(context).apply {
            implementationMode = PreviewView.ImplementationMode.PERFORMANCE
            scaleType = PreviewView.ScaleType.FILL_CENTER
        }
    }
    val imageCapture = remember {
        ImageCapture.Builder()
            .setCaptureMode(ImageCapture.CAPTURE_MODE_MINIMIZE_LATENCY)
            .setBufferFormat(ImageFormat.YUV_420_888)
            .build()
    }

    DisposableEffect(lifecycleOwner) {
        val cameraProviderFuture = ProcessCameraProvider.getInstance(context)
        val mainExecutor = ContextCompat.getMainExecutor(context)

        cameraProviderFuture.addListener({
            runCatching {
                val cameraProvider = cameraProviderFuture.get()
                val preview = Preview.Builder().build().also {
                    it.surfaceProvider = previewView.surfaceProvider
                }

                val sharpnessAnalyzer = ImageAnalysis.Builder()
                    .setOutputImageFormat(ImageAnalysis.OUTPUT_IMAGE_FORMAT_YUV_420_888)
                    .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                    .build()
                    .also { analysis ->
                        analysis.setAnalyzer(cameraExecutor) { imageProxy ->
                            try {
                                val rawScore = MosaicNative.computeSharpnessZeroCopy(imageProxy)
                                sharpnessEma = if (sharpnessEma == 0f) {
                                    rawScore
                                } else {
                                    sharpnessEma * 0.72f + rawScore * 0.28f
                                }
                            } finally {
                                imageProxy.close()
                            }
                        }
                    }

                cameraProvider.unbindAll()
                cameraProvider.bindToLifecycle(
                    lifecycleOwner,
                    CameraSelector.DEFAULT_BACK_CAMERA,
                    preview,
                    sharpnessAnalyzer,
                    imageCapture
                )
            }.onFailure { err ->
                statusBanner = "相機初始化失敗: ${err.localizedMessage}"
            }
        }, mainExecutor)

        onDispose {
            runCatching {
                cameraProviderFuture.get().unbindAll()
            }
            cameraExecutor.shutdown()
        }
    }

    val isSharpEnough = sharpnessEma >= ANTI_SHAKE_SHARPNESS_THRESHOLD || !antiShakeGateEnabled

    Box(modifier = modifier.fillMaxSize()) {
        // Direct ISP -> SurfaceFlinger hardware preview (Zero CPU RGB / Zero Bitmap)
        AndroidView(
            factory = { previewView },
            modifier = Modifier.fillMaxSize()
        )

        // Interactive polygon learning overlay when in LEARN_FAMTPL02 mode
        if (workMode == CameraWorkMode.LEARN_FAMTPL02) {
            PolygonLearnOverlay(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(bottom = 195.dp),
                onPolygonsChanged = { state -> overlayPolygonState = state }
            )
        }

        // Bottom tactical control console
        Surface(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth(),
            shape = RoundedCornerShape(topStart = 24.dp, topEnd = 24.dp),
            color = Color(0xE60B0F17)
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp, vertical = 14.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp)
            ) {
                // Real-time anti-shake sharpness HUD + gate switch
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(
                            imageVector = if (isSharpEnough) {
                                Icons.Default.CheckCircle
                            } else {
                                Icons.Default.WarningAmber
                            },
                            contentDescription = null,
                            tint = if (isSharpEnough) Color(0xFF00E5A3) else Color(0xFFFBBF24),
                            modifier = Modifier.size(18.dp)
                        )
                        Spacer(modifier = Modifier.width(6.dp))
                        Text(
                            text = "清晰度 ${"%.1f".format(sharpnessEma)} · " +
                                if (isSharpEnough) "防手震鎖定" else "偵測到晃動/失焦",
                            style = MaterialTheme.typography.labelMedium,
                            fontWeight = FontWeight.SemiBold,
                            color = if (isSharpEnough) Color(0xFF00E5A3) else Color(0xFFFBBF24)
                        )
                    }

                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(
                            text = "防手震鎖",
                            style = MaterialTheme.typography.labelSmall,
                            color = Color(0xFF94A3B8)
                        )
                        Spacer(modifier = Modifier.width(6.dp))
                        Switch(
                            checked = antiShakeGateEnabled,
                            onCheckedChange = { antiShakeGateEnabled = it }
                        )
                    }
                }

                // Mode switcher chips
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(10.dp)
                ) {
                    FilterChip(
                        selected = workMode == CameraWorkMode.AUTO_MOSAIC_SHUTTER,
                        onClick = { workMode = CameraWorkMode.AUTO_MOSAIC_SHUTTER },
                        label = { Text("自動單應性打碼拍照 (啟用範本: $activeTemplatesCount)") },
                        leadingIcon = {
                            Icon(
                                Icons.Default.AutoFixHigh,
                                contentDescription = null,
                                modifier = Modifier.size(16.dp)
                            )
                        },
                        colors = FilterChipDefaults.filterChipColors(
                            selectedContainerColor = Color(0xFF00E5A3).copy(alpha = 0.22f),
                            selectedLabelColor = Color(0xFF00E5A3)
                        ),
                        modifier = Modifier.weight(1f)
                    )

                    FilterChip(
                        selected = workMode == CameraWorkMode.LEARN_FAMTPL02,
                        onClick = { workMode = CameraWorkMode.LEARN_FAMTPL02 },
                        label = { Text("自學習框選") },
                        leadingIcon = {
                            Icon(
                                Icons.Default.CropFree,
                                contentDescription = null,
                                modifier = Modifier.size(16.dp)
                            )
                        },
                        colors = FilterChipDefaults.filterChipColors(
                            selectedContainerColor = Color(0xFF38BDF8).copy(alpha = 0.24f),
                            selectedLabelColor = Color(0xFF38BDF8)
                        )
                    )
                }

                if (workMode == CameraWorkMode.LEARN_FAMTPL02) {
                    OutlinedTextField(
                        value = templateNameInput,
                        onValueChange = { templateNameInput = it },
                        singleLine = true,
                        placeholder = { Text("輸入範本名稱（如：員工證姓名欄、車牌遮罩）") },
                        colors = OutlinedTextFieldDefaults.colors(
                            focusedBorderColor = Color(0xFF38BDF8),
                            unfocusedBorderColor = Color(0xFF334155),
                            focusedTextColor = Color.White,
                            unfocusedTextColor = Color.White
                        ),
                        modifier = Modifier.fillMaxWidth()
                    )
                }

                // Telemetry / status readout
                Text(
                    text = statusBanner,
                    style = MaterialTheme.typography.bodySmall,
                    color = Color(0xFFE2E8F0)
                )

                // Primary action button
                Button(
                    enabled = !isCapturing && isSharpEnough &&
                        (workMode == CameraWorkMode.AUTO_MOSAIC_SHUTTER ||
                            overlayPolygonState.viewNormalizedPolygons.isNotEmpty()),
                    onClick = {
                        isCapturing = true
                        if (workMode == CameraWorkMode.LEARN_FAMTPL02) {
                            val snapshotOverlay = overlayPolygonState
                            val desiredName = templateNameInput.trim()
                            statusBanner = "正在擷取 YUV_420_888 平面並萃取 ORB 特徵..."

                            imageCapture.takePicture(
                                cameraExecutor,
                                object : ImageCapture.OnImageCapturedCallback() {
                                    override fun onCaptureSuccess(image: ImageProxy) {
                                        coroutineScope.launch {
                                            try {
                                                val sensorPolygons =
                                                    RdpSimplifier.mapOverlayToRawSensorPolygons(
                                                        overlay = snapshotOverlay,
                                                        rawSensorWidth = image.width,
                                                        rawSensorHeight = image.height,
                                                        rotationDegrees = image.imageInfo.rotationDegrees
                                                    )
                                                val res = templateRepository.learnAndSaveTemplate(
                                                    displayName = desiredName,
                                                    image = image,
                                                    normalizedPolygons = sensorPolygons
                                                )
                                                res.fold(
                                                    onSuccess = { entry ->
                                                        val ms = entry.lastMmapLoadUs / 1000.0
                                                        statusBanner =
                                                            "已存檔 FAMTPL02「${entry.displayName}」" +
                                                                "(${entry.keypointCount} ORB 點 · ${entry.fileBytes / 1024}KB · ${"%.2f".format(ms)}ms)"
                                                        templateNameInput = ""
                                                        workMode = CameraWorkMode.AUTO_MOSAIC_SHUTTER
                                                    },
                                                    onFailure = { e ->
                                                        statusBanner = "學習失敗: ${e.message}"
                                                    }
                                                )
                                            } finally {
                                                image.close()
                                                isCapturing = false
                                            }
                                        }
                                    }

                                    override fun onError(exception: ImageCaptureException) {
                                        coroutineScope.launch {
                                            statusBanner = "擷取失敗: ${exception.localizedMessage}"
                                            isCapturing = false
                                        }
                                    }
                                }
                            )
                        } else {
                            statusBanner = "正在進行 YUV420 原地單應性投影與 NEON 防還原打碼..."
                            imageCapture.takePicture(
                                cameraExecutor,
                                object : ImageCapture.OnImageCapturedCallback() {
                                    override fun onCaptureSuccess(image: ImageProxy) {
                                        coroutineScope.launch {
                                            try {
                                                val savedMsg = saveMosaickedCameraYuvToMediaStore(
                                                    context = context,
                                                    engineSession = engineSession,
                                                    image = image,
                                                    jpegQuality = jpegQuality,
                                                    blockSize = blockSize
                                                )
                                                statusBanner = savedMsg
                                            } finally {
                                                image.close()
                                                isCapturing = false
                                            }
                                        }
                                    }

                                    override fun onError(exception: ImageCaptureException) {
                                        coroutineScope.launch {
                                            statusBanner = "拍照失敗: ${exception.localizedMessage}"
                                            isCapturing = false
                                        }
                                    }
                                }
                            )
                        }
                    },
                    shape = CircleShape,
                    colors = ButtonDefaults.buttonColors(
                        containerColor = if (workMode == CameraWorkMode.LEARN_FAMTPL02) {
                            Color(0xFF38BDF8)
                        } else {
                            Color(0xFF00E5A3)
                        },
                        contentColor = Color(0xFF0B0F17)
                    ),
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(52.dp)
                ) {
                    Icon(
                        imageVector = if (workMode == CameraWorkMode.LEARN_FAMTPL02) {
                            Icons.Default.CenterFocusStrong
                        } else {
                            Icons.Default.CameraAlt
                        },
                        contentDescription = null
                    )
                    Spacer(modifier = Modifier.width(8.dp))
                    Text(
                        text = if (workMode == CameraWorkMode.LEARN_FAMTPL02) {
                            "擷取並序列化 FAMTPL02 範本"
                        } else {
                            "極速零拷貝拍照打碼輸出"
                        },
                        fontWeight = FontWeight.Bold
                    )
                }
            }
        }
    }
}

private suspend fun saveMosaickedCameraYuvToMediaStore(
    context: Context,
    engineSession: MosaicEngineSession,
    image: ImageProxy,
    jpegQuality: Int,
    blockSize: Int
): String = withContext(Dispatchers.IO) {
    val resolver = context.contentResolver
    val stamp = SimpleDateFormat("yyyyMMdd_HHmmss_SSS", Locale.US).format(Date())
    val fileName = "FAM_CAM_$stamp.jpg"

    val values = ContentValues().apply {
        put(MediaStore.Images.Media.DISPLAY_NAME, fileName)
        put(MediaStore.Images.Media.MIME_TYPE, "image/jpeg")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            put(
                MediaStore.Images.Media.RELATIVE_PATH,
                "${Environment.DIRECTORY_PICTURES}/HomographyMosaic"
            )
            put(MediaStore.Images.Media.IS_PENDING, 1)
        }
    }

    val outUri: Uri = resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
        ?: return@withContext "無法建立 MediaStore 輸出項目"

    val tel = resolver.openFileDescriptor(outUri, "w")?.use { pfd ->
        engineSession.processCameraYuvToJpegFd(
            image = image,
            outFd = pfd.fd,
            jpegQuality = jpegQuality,
            blockSize = blockSize
        )
    }

    if (tel == null || !tel.ioSuccess) {
        resolver.delete(outUri, null, null)
        return@withContext "YUV420 直壓輸出失敗"
    }

    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
        val ready = ContentValues().apply { put(MediaStore.Images.Media.IS_PENDING, 0) }
        resolver.update(outUri, ready, null, null)
    }

    val orbMs = tel.orbAndHomographyUs / 1000.0
    val mosaicMs = tel.neonMosaicUs / 1000.0
    val jpegMs = tel.jpegEncodeUs / 1000.0
    if (tel.matchedAny) {
        "已打碼儲存 $fileName | 內點:${tel.bestInliers} | " +
            "ORB+MAGSAC:${"%.2f".format(orbMs)}ms | NEON:${"%.2f".format(mosaicMs)}ms | 直壓:${"%.1f".format(jpegMs)}ms"
    } else {
        "已原圖直壓 $fileName（畫面未偵測到目標範本 · ORB:${"%.2f".format(orbMs)}ms）"
    }
}
