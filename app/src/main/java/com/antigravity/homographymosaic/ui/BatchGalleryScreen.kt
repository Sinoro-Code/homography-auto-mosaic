package com.antigravity.homographymosaic.ui

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.waitForUpOrCancellation
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Photo
import androidx.compose.material.icons.filled.PhotoLibrary
import androidx.compose.material.icons.filled.RemoveCircleOutline
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import com.antigravity.homographymosaic.core.FamTemplateEntry
import com.antigravity.homographymosaic.core.GalleryJpegTelemetry
import com.antigravity.homographymosaic.core.MosaicEngineSession
import com.antigravity.homographymosaic.core.TemplateRepository
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

data class BatchItemReport(
    val index: Int,
    val outputName: String,
    val telemetry: GalleryJpegTelemetry
)

private const val FIXED_MOSAIC_BLOCK_SIZE_PX = 32
private const val FIXED_OUTPUT_JPEG_QUALITY = 85

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun BatchGalleryScreen(
    engineSession: MosaicEngineSession,
    templateRepository: TemplateRepository,
    onReferencePhotoPicked: (Uri) -> Unit,
    modifier: Modifier = Modifier
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    val templates by templateRepository.templates.collectAsState()
    val activeCount = templates.count { it.enabled }

    var isProcessing by remember { mutableStateOf(false) }
    var completedCount by remember { mutableIntStateOf(0) }
    var totalSelectedCount by remember { mutableIntStateOf(0) }
    var showTemplatesSheet by remember { mutableStateOf(false) }
    var pendingDeleteTemplate by remember { mutableStateOf<FamTemplateEntry?>(null) }
    val reports = remember { mutableStateListOf<BatchItemReport>() }

    fun launchFdBatch(uris: List<Uri>) {
        if (uris.isEmpty() || isProcessing) return
        isProcessing = true
        completedCount = 0
        totalSelectedCount = uris.size
        reports.clear()

        scope.launch {
            for ((idx, srcUri) in uris.withIndex()) {
                val report = processSingleUriFdToFd(
                    context = context,
                    engineSession = engineSession,
                    sourceUri = srcUri,
                    itemIndex = idx + 1,
                    jpegQuality = FIXED_OUTPUT_JPEG_QUALITY,
                    blockSize = FIXED_MOSAIC_BLOCK_SIZE_PX
                )
                reports.add(0, report)
                completedCount = idx + 1
            }

            isProcessing = false
        }
    }

    val referencePicker = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.PickVisualMedia()
    ) { uri ->
        if (uri != null) onReferencePhotoPicked(uri)
    }

    val singlePicker = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.PickVisualMedia()
    ) { uri ->
        if (uri != null) launchFdBatch(listOf(uri))
    }

    val multiPicker = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.PickMultipleVisualMedia(maxItems = 50)
    ) { uris ->
        if (uris.isNotEmpty()) launchFdBatch(uris)
    }

    Box(modifier = modifier.fillMaxSize()) {
        LazyColumn(
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(start = 16.dp, top = 16.dp, end = 16.dp, bottom = 88.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp)
        ) {
            // 1. Photo Selection & Auto-Mosaic Action Card
            if (activeCount > 0 || isProcessing) {
                item {
                    Card(
                        shape = RoundedCornerShape(20.dp),
                        colors = CardDefaults.cardColors(containerColor = Color(0xFF131B2E))
                    ) {
                        Column(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(16.dp),
                            verticalArrangement = Arrangement.spacedBy(12.dp)
                        ) {
                            if (isProcessing && totalSelectedCount > 0) {
                                LinearProgressIndicator(
                                    progress = { completedCount.toFloat() / totalSelectedCount.toFloat() },
                                    color = Color(0xFF00E5A3),
                                    trackColor = Color(0xFF1E293B),
                                    modifier = Modifier.fillMaxWidth()
                                )
                                Text(
                                    text = "正在處理 $completedCount / $totalSelectedCount ...",
                                    style = MaterialTheme.typography.labelSmall,
                                    color = Color(0xFF00E5A3)
                                )
                            }

                            if (activeCount > 0) {
                                Row(
                                    modifier = Modifier.fillMaxWidth(),
                                    horizontalArrangement = Arrangement.spacedBy(10.dp)
                                ) {
                                    TacticalCapsuleButton(
                                        enabled = !isProcessing,
                                        onClick = {
                                            singlePicker.launch(
                                                PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly)
                                            )
                                        },
                                        modifier = Modifier.weight(1f)
                                    ) {
                                        Icon(
                                            Icons.Default.Photo,
                                            contentDescription = null,
                                            modifier = Modifier.size(18.dp)
                                        )
                                        Text("選取單張打碼", maxLines = 1)
                                    }

                                    TacticalCapsuleButton(
                                        enabled = !isProcessing,
                                        onClick = {
                                            multiPicker.launch(
                                                PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly)
                                            )
                                        },
                                        modifier = Modifier.weight(1.35f)
                                    ) {
                                        Icon(
                                            Icons.Default.PhotoLibrary,
                                            contentDescription = null,
                                            modifier = Modifier.size(18.dp)
                                        )
                                        Text("選取多張自動打碼", maxLines = 1)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // 2. Per-Photo Performance Telemetry Reports
            if (reports.isNotEmpty()) {
                item {
                    Text(
                        text = "最近打碼結果明細 (${reports.size})",
                        style = MaterialTheme.typography.titleSmall,
                        fontWeight = FontWeight.SemiBold,
                        color = Color(0xFFE2E8F0),
                        modifier = Modifier.padding(top = 6.dp)
                    )
                }
                items(reports, key = { "${it.index}_${it.outputName}" }) { rep ->
                    BatchTelemetryRow(rep)
                }
            }
        }

        // Bottom-left floating button: 範本
        TacticalCapsuleButton(
            onClick = { showTemplatesSheet = true },
            modifier = Modifier
                .align(Alignment.BottomStart)
                .padding(16.dp)
        ) {
            Text("範本")
        }

        // Bottom-right floating button: 建立範本
        TacticalCapsuleButton(
            onClick = {
                referencePicker.launch(
                    PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly)
                )
            },
            modifier = Modifier
                .align(Alignment.BottomEnd)
                .padding(16.dp)
        ) {
            Text("建立範本")
        }
    }

    if (showTemplatesSheet) {
        ModalBottomSheet(
            onDismissRequest = { showTemplatesSheet = false },
            containerColor = Color(0xFF0F172A)
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(start = 16.dp, end = 16.dp, bottom = 28.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                Text(
                    text = "範本",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                    color = Color(0xFF00E5A3)
                )

                if (templates.isEmpty()) {
                    Text(
                        text = "尚未建立範本",
                        style = MaterialTheme.typography.bodyMedium,
                        color = Color(0xFF94A3B8),
                        modifier = Modifier.padding(vertical = 12.dp)
                    )
                } else {
                    LazyColumn(
                        verticalArrangement = Arrangement.spacedBy(10.dp)
                    ) {
                        val rows = templates.chunked(2)
                        items(rows, key = { row -> row.joinToString("_") { it.id } }) { rowItems ->
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.spacedBy(10.dp)
                            ) {
                                for (tpl in rowItems) {
                                    TemplateEntryCard(
                                        entry = tpl,
                                        onToggleEnabled = { enabled ->
                                            scope.launch { templateRepository.setTemplateEnabled(tpl.id, enabled) }
                                        },
                                        onLongPressDelete = {
                                            pendingDeleteTemplate = tpl
                                        },
                                        modifier = Modifier.weight(1f)
                                    )
                                }
                                if (rowItems.size == 1) {
                                    Spacer(modifier = Modifier.weight(1f))
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    val deleteTarget = pendingDeleteTemplate
    if (deleteTarget != null) {
        AlertDialog(
            onDismissRequest = { pendingDeleteTemplate = null },
            containerColor = Color(0xFF131B2E),
            text = {
                Text(
                    text = buildAnnotatedString {
                        append("確定刪除 ")
                        withStyle(
                            SpanStyle(
                                color = Color(0xFFF87171),
                                fontWeight = FontWeight.Bold
                            )
                        ) {
                            append(deleteTarget.displayName)
                        }
                        append(" 範本?")
                    },
                    style = MaterialTheme.typography.bodyLarge,
                    color = Color.White
                )
            },
            confirmButton = {
                TacticalCapsuleButton(
                    onClick = {
                        val idToDelete = deleteTarget.id
                        pendingDeleteTemplate = null
                        scope.launch { templateRepository.deleteTemplate(idToDelete) }
                    },
                    contentPadding = PaddingValues(horizontal = 16.dp, vertical = 8.dp)
                ) {
                    Text("刪除", color = Color(0xFFF87171))
                }
            },
            dismissButton = {
                TacticalCapsuleButton(
                    onClick = { pendingDeleteTemplate = null },
                    inactiveGray = true,
                    contentPadding = PaddingValues(horizontal = 16.dp, vertical = 8.dp)
                ) {
                    Text("取消")
                }
            }
        )
    }
}

@Composable
private fun TemplateEntryCard(
    entry: FamTemplateEntry,
    onToggleEnabled: (Boolean) -> Unit,
    onLongPressDelete: () -> Unit,
    modifier: Modifier = Modifier
) {
    val isCompact = LocalConfiguration.current.screenWidthDp <= 380
    val resolvedPadding = if (isCompact) {
        PaddingValues(horizontal = 16.dp, vertical = 9.dp)
    } else {
        PaddingValues(horizontal = 22.dp, vertical = 10.dp)
    }
    val fontSize = if (isCompact) 13.sp else 14.sp

    val currentEnabled by rememberUpdatedState(entry.enabled)
    val currentOnToggle by rememberUpdatedState(onToggleEnabled)
    val currentOnLongPress by rememberUpdatedState(onLongPressDelete)
    var isPressed by remember { mutableStateOf(false) }

    val scale by animateFloatAsState(
        targetValue = if (isPressed) 0.97f else 1f,
        animationSpec = tween(durationMillis = 120),
        label = "tplScale"
    )
    val (bgColor, borderColor) = tacticalCapsuleColors(
        pressed = isPressed,
        selected = entry.enabled,
        inactiveGray = !entry.enabled
    )

    Box(
        modifier = modifier
            .graphicsLayer {
                scaleX = scale
                scaleY = scale
            }
            .shadow(
                elevation = 12.dp,
                shape = TacticalCapsuleShape,
                ambientColor = Color.Black.copy(alpha = 0.5f),
                spotColor = Color.Black.copy(alpha = 0.5f)
            )
            .clip(TacticalCapsuleShape)
            .background(bgColor)
            .border(1.dp, borderColor, TacticalCapsuleShape)
            .pointerInput(entry.id) {
                awaitEachGesture {
                    awaitFirstDown(requireUnconsumed = false)
                    isPressed = true
                    try {
                        var longPressTriggered = false
                        val upEvent = withTimeoutOrNull(500L) {
                            waitForUpOrCancellation()
                        }
                        if (upEvent == null) {
                            longPressTriggered = true
                            currentOnLongPress()
                            waitForUpOrCancellation()
                        } else if (!longPressTriggered) {
                            currentOnToggle(!currentEnabled)
                        }
                    } finally {
                        isPressed = false
                    }
                }
            }
            .padding(resolvedPadding),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = entry.displayName,
            fontSize = fontSize,
            fontWeight = FontWeight.Bold,
            letterSpacing = 0.03.em,
            maxLines = 1,
            color = if (entry.enabled) TacticalTextPrimary else Color(0xFFCBD5E1)
        )
    }
}

@Composable
private fun BatchTelemetryRow(report: BatchItemReport) {
    val t = report.telemetry
    val decMs = t.jpegDecodeUs / 1000.0
    val orbMs = t.homographyAndOrbUs / 1000.0
    val mosMs = t.neonMosaicUs / 1000.0
    val encMs = t.jpegEncodeUs / 1000.0
    val totMs = t.totalUs / 1000.0

    val alignDesc = when {
        !t.matchedAny -> "未套用"
        t.bestInliers > 0 && t.fixedPositionApplied == 0 -> "單應性追蹤(內點:${t.bestInliers})"
        t.bestInliers == 0 && t.fixedPositionApplied > 0 -> "學習位置直接套用"
        else -> "追蹤+位置(${t.templatesMatched}組)"
    }

    Surface(
        shape = RoundedCornerShape(14.dp),
        color = Color(0xFF0F172A),
        modifier = Modifier.fillMaxWidth()
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Icon(
                imageVector = if (t.matchedAny) {
                    Icons.Default.CheckCircle
                } else {
                    Icons.Default.RemoveCircleOutline
                },
                contentDescription = null,
                tint = if (t.matchedAny) Color(0xFF00E5A3) else Color(0xFF64748B),
                modifier = Modifier.size(20.dp)
            )
            Spacer(modifier = Modifier.width(10.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "${report.outputName} (${t.width}×${t.height}) · $alignDesc",
                    style = MaterialTheme.typography.bodySmall,
                    fontWeight = FontWeight.SemiBold,
                    color = Color.White
                )
                Text(
                    text = if (t.ioSuccess) {
                        "直解:${"%.1f".format(decMs)}ms | 對位:${"%.2f".format(orbMs)}ms | " +
                            "NEON打碼:${"%.2f".format(mosMs)}ms | 直壓:${"%.1f".format(encMs)}ms | 總計:${"%.1f".format(totMs)}ms"
                    } else {
                        "略過（無法解析此影像格式或讀取失敗）"
                    },
                    style = MaterialTheme.typography.labelSmall,
                    color = Color(0xFF94A3B8)
                )
            }
        }
    }
}

private suspend fun processSingleUriFdToFd(
    context: Context,
    engineSession: MosaicEngineSession,
    sourceUri: Uri,
    itemIndex: Int,
    jpegQuality: Int,
    blockSize: Int
): BatchItemReport = withContext(Dispatchers.Default) {
    val resolver = context.contentResolver
    val stamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
    val outName = "FAM_MOSAIC_${stamp}_$itemIndex.jpg"

    val values = ContentValues().apply {
        put(MediaStore.Images.Media.DISPLAY_NAME, outName)
        put(MediaStore.Images.Media.MIME_TYPE, "image/jpeg")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            put(
                MediaStore.Images.Media.RELATIVE_PATH,
                "${Environment.DIRECTORY_PICTURES}/HomographyMosaic"
            )
            put(MediaStore.Images.Media.IS_PENDING, 1)
        }
    }

    val outUri = resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
        ?: return@withContext BatchItemReport(
            index = itemIndex,
            outputName = outName,
            telemetry = GalleryJpegTelemetry(
                false, false, 0, 0, 0L, 0L, 0L, 0L, 0L, 0L, 0, 0, 0
            )
        )

    val telemetry = resolver.openFileDescriptor(sourceUri, "r")?.use { inPfd ->
        resolver.openFileDescriptor(outUri, "w")?.use { outPfd ->
            engineSession.processGalleryJpegFdToFd(
                inFd = inPfd.fd,
                outFd = outPfd.fd,
                jpegQuality = jpegQuality,
                blockSize = blockSize
            )
        }
    } ?: GalleryJpegTelemetry(false, false, 0, 0, 0L, 0L, 0L, 0L, 0L, 0L, 0, 0, 0)

    if (telemetry.ioSuccess) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val ready = ContentValues().apply { put(MediaStore.Images.Media.IS_PENDING, 0) }
            resolver.update(outUri, ready, null, null)
        }
    } else {
        resolver.delete(outUri, null, null)
    }

    BatchItemReport(
        index = itemIndex,
        outputName = outName,
        telemetry = telemetry
    )
}
