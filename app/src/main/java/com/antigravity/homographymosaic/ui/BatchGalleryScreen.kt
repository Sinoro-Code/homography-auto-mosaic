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
import androidx.compose.foundation.layout.Arrangement
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
import androidx.compose.material.icons.filled.AddCircleOutline
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.DeleteOutline
import androidx.compose.material.icons.filled.FolderSpecial
import androidx.compose.material.icons.filled.ImageSearch
import androidx.compose.material.icons.filled.Photo
import androidx.compose.material.icons.filled.PhotoLibrary
import androidx.compose.material.icons.filled.RemoveCircleOutline
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
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

@Composable
fun BatchGalleryScreen(
    engineSession: MosaicEngineSession,
    templateRepository: TemplateRepository,
    blockSize: Int,
    onBlockSizeChange: (Int) -> Unit,
    jpegQuality: Int,
    onJpegQualityChange: (Int) -> Unit,
    fallbackToFixedPosition: Boolean,
    onFallbackToFixedPositionChange: (Boolean) -> Unit,
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
                    jpegQuality = jpegQuality,
                    blockSize = blockSize
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

    LazyColumn(
        modifier = modifier.fillMaxSize(),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp)
    ) {
        // 1. Photo Selection & Auto-Mosaic Action Card
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
                    // Alignment Mode selector (Homography + Fixed Position vs Strict Homography only)
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        FilterChip(
                            selected = fallbackToFixedPosition,
                            onClick = {
                                onFallbackToFixedPositionChange(true)
                                engineSession.updateParameters(fallbackToFixedPosition = true)
                            },
                            label = { Text("特徵或區域打碼") },
                            colors = FilterChipDefaults.filterChipColors(
                                selectedContainerColor = Color(0xFF00E5A3).copy(alpha = 0.22f),
                                selectedLabelColor = Color(0xFF00E5A3)
                            )
                        )
                        FilterChip(
                            selected = !fallbackToFixedPosition,
                            onClick = {
                                onFallbackToFixedPositionChange(false)
                                engineSession.updateParameters(fallbackToFixedPosition = false)
                            },
                            label = { Text("特徵打碼") },
                            colors = FilterChipDefaults.filterChipColors(
                                selectedContainerColor = Color(0xFF38BDF8).copy(alpha = 0.24f),
                                selectedLabelColor = Color(0xFF38BDF8)
                            )
                        )
                    }

                    // Mosaic Block Size selector
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        Text(
                            text = "馬賽克粒度:",
                            style = MaterialTheme.typography.labelMedium,
                            color = Color(0xFFCBD5E1)
                        )
                        listOf(12, 16, 24, 32).forEach { sizeOption ->
                            FilterChip(
                                selected = blockSize == sizeOption,
                                onClick = {
                                    onBlockSizeChange(sizeOption)
                                    engineSession.updateParameters(blockSize = sizeOption)
                                },
                                label = { Text("${sizeOption}px") },
                                colors = FilterChipDefaults.filterChipColors(
                                    selectedContainerColor = Color(0xFF00E5A3).copy(alpha = 0.22f),
                                    selectedLabelColor = Color(0xFF00E5A3)
                                )
                            )
                        }
                    }

                    // JPEG Quality quick selector
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        Text(
                            text = "輸出品質:",
                            style = MaterialTheme.typography.labelMedium,
                            color = Color(0xFFCBD5E1)
                        )
                        listOf(85, 92, 96).forEach { qOption ->
                            FilterChip(
                                selected = jpegQuality == qOption,
                                onClick = { onJpegQualityChange(qOption) },
                                label = { Text("Q$qOption") },
                                colors = FilterChipDefaults.filterChipColors(
                                    selectedContainerColor = Color(0xFF38BDF8).copy(alpha = 0.24f),
                                    selectedLabelColor = Color(0xFF38BDF8)
                                )
                            )
                        }
                    }

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

                    if (activeCount == 0) {
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
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Icon(Icons.Default.ImageSearch, contentDescription = null)
                            Spacer(modifier = Modifier.width(8.dp))
                            Text("建立範本", fontWeight = FontWeight.Bold)
                        }
                    } else {
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.spacedBy(10.dp)
                        ) {
                            OutlinedButton(
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
                                Spacer(modifier = Modifier.width(6.dp))
                                Text("選取單張打碼")
                            }

                            Button(
                                enabled = !isProcessing,
                                onClick = {
                                    multiPicker.launch(
                                        PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly)
                                    )
                                },
                                colors = ButtonDefaults.buttonColors(
                                    containerColor = Color(0xFF00E5A3),
                                    contentColor = Color(0xFF0B0F17)
                                ),
                                modifier = Modifier.weight(1.35f)
                            ) {
                                Icon(
                                    Icons.Default.PhotoLibrary,
                                    contentDescription = null,
                                    modifier = Modifier.size(18.dp)
                                )
                                Spacer(modifier = Modifier.width(6.dp))
                                Text("選取多張自動打碼", fontWeight = FontWeight.Bold)
                            }
                        }
                    }
                }
            }
        }

        // 2. Learned Mosaic Positions (FAMTPL02 Vault)
        item {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(top = 2.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(
                        Icons.Default.FolderSpecial,
                        contentDescription = null,
                        tint = Color(0xFF38BDF8),
                        modifier = Modifier.size(18.dp)
                    )
                    Spacer(modifier = Modifier.width(6.dp))
                    Text(
                        text = "已學習打碼位置 (${templates.size})",
                        style = MaterialTheme.typography.titleSmall,
                        fontWeight = FontWeight.SemiBold,
                        color = Color(0xFFE2E8F0)
                    )
                }

                TextButton(
                    onClick = {
                        referencePicker.launch(
                            PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly)
                        )
                    }
                ) {
                    Icon(
                        Icons.Default.AddCircleOutline,
                        contentDescription = null,
                        tint = Color(0xFF00E5A3),
                        modifier = Modifier.size(16.dp)
                    )
                    Spacer(modifier = Modifier.width(4.dp))
                    Text("建立範本", color = Color(0xFF00E5A3))
                }
            }
        }

        items(templates, key = { it.id }) { tpl ->
            TemplateEntryCard(
                entry = tpl,
                onToggleEnabled = { enabled ->
                    scope.launch { templateRepository.setTemplateEnabled(tpl.id, enabled) }
                },
                onDelete = {
                    scope.launch { templateRepository.deleteTemplate(tpl.id) }
                }
            )
        }

        // 3. Per-Photo Performance Telemetry Reports
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
}

@Composable
private fun TemplateEntryCard(
    entry: FamTemplateEntry,
    onToggleEnabled: (Boolean) -> Unit,
    onDelete: () -> Unit
) {
    Surface(
        shape = RoundedCornerShape(14.dp),
        color = Color(0xFF111827),
        modifier = Modifier.fillMaxWidth()
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 14.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = entry.displayName,
                    style = MaterialTheme.typography.bodyMedium,
                    fontWeight = FontWeight.SemiBold,
                    color = Color.White
                )
                Text(
                    text = "區域:${entry.polygonCount}處(${entry.vertexCount}點) · 特徵:${entry.keypointCount} · " +
                        "${"%.1f".format(entry.fileBytes / 1024.0)}KB · 載入:${entry.lastMmapLoadUs}µs",
                    style = MaterialTheme.typography.labelSmall,
                    color = Color(0xFF94A3B8)
                )
            }
            Switch(
                checked = entry.enabled,
                onCheckedChange = onToggleEnabled
            )
            IconButton(onClick = onDelete) {
                Icon(
                    Icons.Default.DeleteOutline,
                    contentDescription = "刪除此打碼位置",
                    tint = Color(0xFFF87171)
                )
            }
        }
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
