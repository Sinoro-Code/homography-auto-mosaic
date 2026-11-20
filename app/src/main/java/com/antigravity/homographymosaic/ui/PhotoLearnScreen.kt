package com.antigravity.homographymosaic.ui

import android.graphics.SurfaceTexture
import android.net.Uri
import android.view.Surface
import android.view.TextureView
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.AutoFixHigh
import androidx.compose.material.icons.filled.ImageSearch
import androidx.compose.material.icons.filled.Undo
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.antigravity.homographymosaic.core.FamTemplateEntry
import com.antigravity.homographymosaic.core.JpegHeaderInfo
import com.antigravity.homographymosaic.core.MosaicNative
import com.antigravity.homographymosaic.core.TemplateRepository
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

private val VALID_TEMPLATE_NAME_REGEX =
    Regex("^[\\u4E00-\\u9FFF\\u3400-\\u4DBF\\uF900-\\uFAFFa-zA-Z0-9]+$")

@Composable
fun PhotoLearnScreen(
    initialReferenceUri: Uri,
    templateRepository: TemplateRepository,
    onCancel: () -> Unit,
    onLearnedSuccess: (FamTemplateEntry) -> Unit,
    modifier: Modifier = Modifier
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val editorState = rememberPolygonEditorState()

    var referenceUri by remember(initialReferenceUri) { mutableStateOf(initialReferenceUri) }
    var headerInfo by remember { mutableStateOf<JpegHeaderInfo?>(null) }
    var templateName by remember { mutableStateOf("") }
    var isNameError by remember { mutableStateOf(false) }
    var isLearning by remember { mutableStateOf(false) }
    var statusMessage by remember { mutableStateOf<String?>("正在載入範例相片...") }
    var isErrorStatus by remember { mutableStateOf(false) }

    val referencePicker = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.PickVisualMedia()
    ) { pickedUri ->
        if (pickedUri != null) {
            editorState.clearAll()
            statusMessage = "正在載入範例相片..."
            isErrorStatus = false
            referenceUri = pickedUri
        }
    }

    LaunchedEffect(referenceUri) {
        val info = withContext(Dispatchers.IO) {
            context.contentResolver.openFileDescriptor(referenceUri, "r")?.use { pfd ->
                MosaicNative.inspectJpegFd(pfd.fd)
            } ?: JpegHeaderInfo(false, 0, 0, 0, 0, 1)
        }
        if (info.valid && info.uprightWidth > 0 && info.uprightHeight > 0) {
            headerInfo = info
            statusMessage = null
            isErrorStatus = false
        } else {
            headerInfo = null
            statusMessage = "無法解析所選影像（支援 JPEG / HEIC / HEIF / PNG / WebP / AVIF，請確認檔案未損毀）"
            isErrorStatus = true
        }
    }

    Column(
        modifier = modifier
            .fillMaxSize()
            .imePadding()
            .padding(horizontal = 14.dp, vertical = 10.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        val activeUri = referenceUri
        val activeHeader = headerInfo

        if (activeHeader == null) {
            Box(
                modifier = Modifier
                    .weight(1f)
                    .fillMaxWidth(),
                contentAlignment = Alignment.Center
            ) {
                Card(
                    shape = RoundedCornerShape(22.dp),
                    colors = CardDefaults.cardColors(containerColor = Color(0xFF131B2E)),
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Column(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(24.dp),
                        horizontalAlignment = Alignment.CenterHorizontally,
                        verticalArrangement = Arrangement.spacedBy(12.dp)
                    ) {
                        if (statusMessage != null) {
                            Text(
                                text = statusMessage!!,
                                style = MaterialTheme.typography.bodySmall,
                                color = if (isErrorStatus) Color(0xFFF87171) else Color(0xFF94A3B8)
                            )
                        }
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.spacedBy(10.dp)
                        ) {
                            OutlinedButton(
                                onClick = onCancel,
                                modifier = Modifier.weight(1f)
                            ) {
                                Text("返回")
                            }
                            Button(
                                onClick = {
                                    referencePicker.launch(
                                        PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly)
                                    )
                                },
                                colors = ButtonDefaults.buttonColors(
                                    containerColor = Color(0xFF00E5A3),
                                    contentColor = Color(0xFF0B0F17)
                                ),
                                modifier = Modifier.weight(1.4f)
                            ) {
                                Icon(Icons.Default.ImageSearch, contentDescription = null)
                                Spacer(modifier = Modifier.width(6.dp))
                                Text("重選範例相片", fontWeight = FontWeight.Bold)
                            }
                        }
                    }
                }
            }
        } else {
            // Top bar: back button on left, undo button on right
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                IconButton(onClick = onCancel) {
                    Icon(
                        imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                        contentDescription = "返回",
                        tint = Color.White
                    )
                }
                IconButton(onClick = { editorState.undoLast() }) {
                    Icon(
                        imageVector = Icons.Default.Undo,
                        contentDescription = "復原",
                        tint = Color.White
                    )
                }
            }

            // Aspect-ratio-locked ANativeWindow Surface preview + 1:1 PolygonLearnCanvas
            BoxWithConstraints(
                modifier = Modifier
                    .weight(1f)
                    .fillMaxWidth(),
                contentAlignment = Alignment.Center
            ) {
                val aspect =
                    (activeHeader.uprightWidth.toFloat() / activeHeader.uprightHeight.toFloat())
                        .coerceIn(0.1f, 10f)
                val containerAspect = if (maxHeight > 0.dp) maxWidth / maxHeight else 1f
                val fitWidth = if (containerAspect > aspect) maxHeight * aspect else maxWidth
                val fitHeight = if (containerAspect > aspect) maxHeight else maxWidth / aspect

                Box(
                    modifier = Modifier
                        .size(fitWidth, fitHeight)
                        .clip(RoundedCornerShape(14.dp))
                        .background(Color(0xFF0F172A))
                        .border(1.dp, Color(0xFF1E293B), RoundedCornerShape(14.dp))
                ) {
                    key(activeUri) {
                        NativeJpegSurfacePreview(
                            jpegUri = activeUri,
                            modifier = Modifier.fillMaxSize()
                        )
                    }

                    PolygonLearnCanvas(
                        state = editorState,
                        modifier = Modifier.fillMaxSize()
                    )
                }
            }

            // Bottom Learn & Save control panel
            Surface(
                shape = RoundedCornerShape(18.dp),
                color = Color(0xFF131B2E),
                modifier = Modifier.fillMaxWidth()
            ) {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 14.dp, vertical = 12.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    val polygons = editorState.simplifiedPolygons
                    Box(modifier = Modifier.fillMaxWidth()) {
                        OutlinedTextField(
                            value = templateName,
                            onValueChange = {
                                templateName = it
                                if (isNameError) isNameError = false
                            },
                            singleLine = true,
                            isError = isNameError,
                            trailingIcon = {
                                Button(
                                    enabled = !isLearning && polygons.isNotEmpty(),
                                    onClick = {
                                        val trimmedName = templateName.trim()
                                        if (trimmedName.isEmpty() || !VALID_TEMPLATE_NAME_REGEX.matches(trimmedName)) {
                                            isNameError = true
                                            return@Button
                                        }
                                        isNameError = false
                                        isLearning = true
                                        scope.launch {
                                            val res = withContext(Dispatchers.IO) {
                                                val pfd = context.contentResolver.openFileDescriptor(activeUri, "r")
                                                if (pfd == null) {
                                                    Result.failure(IllegalStateException("無法讀取所選範例相片"))
                                                } else {
                                                    pfd.use {
                                                        templateRepository.learnAndSaveTemplateFromJpegFd(
                                                            displayName = trimmedName,
                                                            inFd = it.fd,
                                                            uprightPolygons = polygons,
                                                            makeExclusiveActive = false
                                                        )
                                                    }
                                                }
                                            }
                                            isLearning = false
                                            res.fold(
                                                onSuccess = { entry ->
                                                    templateName = ""
                                                    editorState.clearAll()
                                                    onLearnedSuccess(entry)
                                                },
                                                onFailure = { err ->
                                                    statusMessage = err.message ?: "儲存失敗"
                                                    isErrorStatus = true
                                                }
                                            )
                                        }
                                    },
                                    colors = ButtonDefaults.buttonColors(
                                        containerColor = Color(0xFF00E5A3),
                                        contentColor = Color(0xFF0B0F17)
                                    ),
                                    contentPadding = PaddingValues(horizontal = 12.dp, vertical = 6.dp),
                                    shape = RoundedCornerShape(10.dp),
                                    modifier = Modifier.padding(end = 6.dp)
                                ) {
                                    Text(
                                        text = "儲存範本",
                                        style = MaterialTheme.typography.labelMedium,
                                        fontWeight = FontWeight.Bold
                                    )
                                }
                            },
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(top = 6.dp)
                        )

                        Text(
                            text = "範本名稱",
                            style = MaterialTheme.typography.labelSmall,
                            color = if (isNameError) MaterialTheme.colorScheme.error else Color(0xFF94A3B8),
                            modifier = Modifier
                                .padding(start = 12.dp)
                                .background(Color(0xFF131B2E))
                                .padding(horizontal = 4.dp)
                        )
                    }
                }
            }
        }
    }
}

/**
 * Hardware-composited TextureView backed by NDK ANativeWindow_fromSurface (zero Java RGB heap objects).
 */
@Composable
private fun NativeJpegSurfacePreview(
    jpegUri: Uri,
    modifier: Modifier = Modifier
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val renderMutex = remember(jpegUri) { Mutex() }

    AndroidView(
        modifier = modifier,
        factory = { ctx ->
            TextureView(ctx).apply {
                isOpaque = true
                surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                    private var boundSurface: Surface? = null

                    override fun onSurfaceTextureAvailable(
                        st: SurfaceTexture,
                        width: Int,
                        height: Int
                    ) {
                        val surface = Surface(st)
                        boundSurface = surface
                        scope.launch(Dispatchers.IO) {
                            renderMutex.withLock {
                                if (surface.isValid) {
                                    context.contentResolver.openFileDescriptor(jpegUri, "r")
                                        ?.use { pfd ->
                                            MosaicNative.renderJpegFdToSurface(
                                                inFd = pfd.fd,
                                                surface = surface,
                                                maxPreviewDim = 1600
                                            )
                                        }
                                }
                            }
                        }
                    }

                    override fun onSurfaceTextureSizeChanged(
                        st: SurfaceTexture,
                        width: Int,
                        height: Int
                    ) = Unit

                    override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
                        boundSurface?.release()
                        boundSurface = null
                        return true
                    }

                    override fun onSurfaceTextureUpdated(st: SurfaceTexture) = Unit
                }
            }
        }
    )
}
