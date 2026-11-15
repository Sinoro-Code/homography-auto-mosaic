package com.antigravity.homographymosaic.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Brush
import androidx.compose.material.icons.filled.DeleteSweep
import androidx.compose.material.icons.filled.Polyline
import androidx.compose.material.icons.filled.Undo
import androidx.compose.material3.AssistChip
import androidx.compose.material3.AssistChipDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import com.antigravity.homographymosaic.core.NormalizedPoint
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.sqrt

enum class PolygonInputMode {
    FINGER_LASSO,   // 手指塗抹自由圈選 + 自動 Ramer-Douglas-Peucker 頂點簡化
    TAP_VERTICES    // 精準多邊形頂點點擊框選
}

/**
 * Holds view-space polygons (normalized to [0..1] of the overlay Canvas) prior to
 * sensor rotation/crop projection when a CameraX YUV_420_888 frame is captured.
 */
data class OverlayPolygonState(
    val viewNormalizedPolygons: List<List<NormalizedPoint>>,
    val viewportWidthPx: Float,
    val viewportHeightPx: Float
)

/**
 * Interactive Compose Canvas overlay for self-learning target regions without Bitmap allocation.
 * Features freehand finger brush/lasso, tap-to-place vertices, multi-region support, and
 * iterative Ramer-Douglas-Peucker (RDP) vertex simplification.
 */
@Composable
fun PolygonLearnOverlay(
    modifier: Modifier = Modifier,
    onPolygonsChanged: (OverlayPolygonState) -> Unit
) {
    var canvasSize by remember { mutableStateOf(IntSize.Zero) }
    var inputMode by remember { mutableStateOf(PolygonInputMode.FINGER_LASSO) }
    var rdpEpsilonRatio by remember { mutableFloatStateOf(0.016f) }

    // Completed raw view-normalized polygons (before or after RDP)
    val rawPolygons = remember { mutableStateListOf<List<NormalizedPoint>>() }
    // Active stroke or tap vertices currently being drawn
    val activePoints = remember { mutableStateListOf<Offset>() }

    // Recompute RDP-simplified polygons whenever rawPolygons, activePoints, or epsilon changes
    val simplifiedPolygons: List<List<NormalizedPoint>> = remember(
        rawPolygons.toList(),
        activePoints.toList(),
        rdpEpsilonRatio,
        inputMode,
        canvasSize
    ) {
        val out = ArrayList<List<NormalizedPoint>>(rawPolygons.size + 1)
        for (poly in rawPolygons) {
            val simp = RdpSimplifier.simplifyClosedPolygon(poly, rdpEpsilonRatio)
            if (simp.size >= 3) out.add(simp)
        }
        if (inputMode == PolygonInputMode.TAP_VERTICES && activePoints.size >= 3 &&
            canvasSize.width > 0 && canvasSize.height > 0
        ) {
            val normTap = activePoints.map {
                NormalizedPoint(
                    (it.x / canvasSize.width.toFloat()).coerceIn(0f, 1f),
                    (it.y / canvasSize.height.toFloat()).coerceIn(0f, 1f)
                )
            }
            out.add(normTap)
        }
        out
    }

    LaunchedEffect(simplifiedPolygons, canvasSize) {
        if (canvasSize.width > 0 && canvasSize.height > 0) {
            onPolygonsChanged(
                OverlayPolygonState(
                    viewNormalizedPolygons = simplifiedPolygons,
                    viewportWidthPx = canvasSize.width.toFloat(),
                    viewportHeightPx = canvasSize.height.toFloat()
                )
            )
        }
    }

    Box(modifier = modifier.fillMaxSize()) {
        Canvas(
            modifier = Modifier
                .fillMaxSize()
                .onSizeChanged { canvasSize = it }
                .pointerInput(inputMode, canvasSize) {
                    if (inputMode == PolygonInputMode.FINGER_LASSO) {
                        detectDragGestures(
                            onDragStart = { start ->
                                activePoints.clear()
                                activePoints.add(start)
                            },
                            onDrag = { change, _ ->
                                change.consume()
                                val pos = change.position
                                val last = activePoints.lastOrNull()
                                if (last == null || hypot(pos.x - last.x, pos.y - last.y) >= 5f) {
                                    activePoints.add(pos)
                                }
                            },
                            onDragEnd = {
                                if (activePoints.size >= 3 &&
                                    canvasSize.width > 0 && canvasSize.height > 0
                                ) {
                                    val w = canvasSize.width.toFloat()
                                    val h = canvasSize.height.toFloat()
                                    val normalized = activePoints.map { pt ->
                                        NormalizedPoint(
                                            (pt.x / w).coerceIn(0f, 1f),
                                            (pt.y / h).coerceIn(0f, 1f)
                                        )
                                    }
                                    rawPolygons.add(normalized)
                                }
                                activePoints.clear()
                            },
                            onDragCancel = {
                                activePoints.clear()
                            }
                        )
                    } else {
                        detectTapGestures { tapOffset ->
                            activePoints.add(tapOffset)
                        }
                    }
                }
        ) {
            val w = size.width
            val h = size.height

            // Draw finalized / simplified polygons
            for (poly in simplifiedPolygons) {
                if (poly.size < 3) continue
                val path = Path().apply {
                    moveTo(poly[0].x * w, poly[0].y * h)
                    for (i in 1 until poly.size) {
                        lineTo(poly[i].x * w, poly[i].y * h)
                    }
                    close()
                }
                drawPath(
                    path = path,
                    color = Color(0x4400E5A3)
                )
                drawPath(
                    path = path,
                    color = Color(0xFF00E5A3),
                    style = Stroke(
                        width = 3.dp.toPx(),
                        cap = StrokeCap.Round,
                        join = StrokeJoin.Round
                    )
                )
                for (v in poly) {
                    drawCircle(
                        color = Color(0xFF0B0F17),
                        radius = 6.dp.toPx(),
                        center = Offset(v.x * w, v.y * h)
                    )
                    drawCircle(
                        color = Color(0xFF38BDF8),
                        radius = 4.dp.toPx(),
                        center = Offset(v.x * w, v.y * h)
                    )
                }
            }

            // Draw live finger lasso trail while dragging
            if (inputMode == PolygonInputMode.FINGER_LASSO && activePoints.size >= 2) {
                val livePath = Path().apply {
                    moveTo(activePoints[0].x, activePoints[0].y)
                    for (i in 1 until activePoints.size) {
                        lineTo(activePoints[i].x, activePoints[i].y)
                    }
                    close()
                }
                drawPath(path = livePath, color = Color(0x3338BDF8))
                drawPath(
                    path = livePath,
                    color = Color(0xFF38BDF8),
                    style = Stroke(
                        width = 2.5.dp.toPx(),
                        pathEffect = PathEffect.dashPathEffect(floatArrayOf(14f, 10f), 0f)
                    )
                )
            } else if (inputMode == PolygonInputMode.TAP_VERTICES && activePoints.size in 1..2) {
                for (pt in activePoints) {
                    drawCircle(color = Color(0xFF38BDF8), radius = 6.dp.toPx(), center = pt)
                }
                if (activePoints.size == 2) {
                    drawLine(
                        color = Color(0xFF38BDF8),
                        start = activePoints[0],
                        end = activePoints[1],
                        strokeWidth = 2.5.dp.toPx()
                    )
                }
            }

            // Subtle central alignment reticle for CameraX 60% sharpness ROI
            val rx0 = w * 0.15f
            val ry0 = h * 0.20f
            drawRect(
                color = Color(0x33FFFFFF),
                topLeft = Offset(rx0, ry0),
                size = Size(w * 0.70f, h * 0.55f),
                style = Stroke(
                    width = 1.dp.toPx(),
                    pathEffect = PathEffect.dashPathEffect(floatArrayOf(12f, 12f), 0f)
                )
            )
        }

        // Compact floating Douglas-Peucker & polygon control bar
        Surface(
            modifier = Modifier
                .align(Alignment.TopCenter)
                .padding(horizontal = 12.dp, vertical = 10.dp)
                .fillMaxWidth(),
            shape = RoundedCornerShape(16.dp),
            color = Color(0xCC0B0F17)
        ) {
            Column(modifier = Modifier.padding(horizontal = 12.dp, vertical = 8.dp)) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        AssistChip(
                            onClick = {
                                if (inputMode == PolygonInputMode.TAP_VERTICES &&
                                    activePoints.size >= 3 &&
                                    canvasSize.width > 0 && canvasSize.height > 0
                                ) {
                                    val w = canvasSize.width.toFloat()
                                    val h = canvasSize.height.toFloat()
                                    rawPolygons.add(
                                        activePoints.map {
                                            NormalizedPoint(
                                                (it.x / w).coerceIn(0f, 1f),
                                                (it.y / h).coerceIn(0f, 1f)
                                            )
                                        }
                                    )
                                }
                                activePoints.clear()
                                inputMode = PolygonInputMode.FINGER_LASSO
                            },
                            label = { Text("塗抹圈選") },
                            leadingIcon = {
                                Icon(
                                    Icons.Default.Brush,
                                    contentDescription = null,
                                    modifier = Modifier.size(16.dp)
                                )
                            },
                            colors = AssistChipDefaults.assistChipColors(
                                containerColor = if (inputMode == PolygonInputMode.FINGER_LASSO) {
                                    Color(0xFF00E5A3).copy(alpha = 0.22f)
                                } else {
                                    Color.Transparent
                                },
                                labelColor = Color.White
                            )
                        )

                        AssistChip(
                            onClick = {
                                activePoints.clear()
                                inputMode = PolygonInputMode.TAP_VERTICES
                            },
                            label = { Text("頂點框選") },
                            leadingIcon = {
                                Icon(
                                    Icons.Default.Polyline,
                                    contentDescription = null,
                                    modifier = Modifier.size(16.dp)
                                )
                            },
                            colors = AssistChipDefaults.assistChipColors(
                                containerColor = if (inputMode == PolygonInputMode.TAP_VERTICES) {
                                    Color(0xFF38BDF8).copy(alpha = 0.25f)
                                } else {
                                    Color.Transparent
                                },
                                labelColor = Color.White
                            )
                        )
                    }

                    Row(verticalAlignment = Alignment.CenterVertically) {
                        if (inputMode == PolygonInputMode.TAP_VERTICES && activePoints.size >= 3) {
                            IconButton(
                                onClick = {
                                    if (canvasSize.width > 0 && canvasSize.height > 0) {
                                        val w = canvasSize.width.toFloat()
                                        val h = canvasSize.height.toFloat()
                                        rawPolygons.add(
                                            activePoints.map {
                                                NormalizedPoint(
                                                    (it.x / w).coerceIn(0f, 1f),
                                                    (it.y / h).coerceIn(0f, 1f)
                                                )
                                            }
                                        )
                                        activePoints.clear()
                                    }
                                }
                            ) {
                                Icon(
                                    Icons.Default.Add,
                                    contentDescription = "鎖定此多邊形並新增下一區",
                                    tint = Color(0xFF00E5A3)
                                )
                            }
                        }

                        IconButton(
                            onClick = {
                                if (activePoints.isNotEmpty()) {
                                    activePoints.removeAt(activePoints.lastIndex)
                                } else if (rawPolygons.isNotEmpty()) {
                                    rawPolygons.removeAt(rawPolygons.lastIndex)
                                }
                            }
                        ) {
                            Icon(Icons.Default.Undo, contentDescription = "復原", tint = Color.White)
                        }

                        IconButton(
                            onClick = {
                                activePoints.clear()
                                rawPolygons.clear()
                            }
                        ) {
                            Icon(
                                Icons.Default.DeleteSweep,
                                contentDescription = "清除全部遮罩",
                                tint = Color(0xFFF87171)
                            )
                        }
                    }
                }

                val totalVerts = simplifiedPolygons.sumOf { it.size }
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        text = "DP 簡化 ε=${"%.3f".format(rdpEpsilonRatio)} | 遮罩:${simplifiedPolygons.size} 頂點:$totalVerts",
                        style = MaterialTheme.typography.labelSmall,
                        fontWeight = FontWeight.Medium,
                        color = Color(0xFFCBD5E1)
                    )
                    Spacer(modifier = Modifier.width(8.dp))
                    Slider(
                        value = rdpEpsilonRatio,
                        onValueChange = { rdpEpsilonRatio = it },
                        valueRange = 0.004f..0.045f,
                        modifier = Modifier.weight(1f)
                    )
                }
            }
        }
    }
}

/**
 * Iterative (zero-recursion) Ramer-Douglas-Peucker polygon simplifier + PreviewView
 * FILL_CENTER to raw CameraX YUV_420_888 coordinate projector.
 */
object RdpSimplifier {

    fun simplifyClosedPolygon(
        points: List<NormalizedPoint>,
        epsilon: Float
    ): List<NormalizedPoint> {
        if (points.size <= 4) return points

        // Anchor split at the antipodal pair (0, farthestFrom0) so closed loops simplify cleanly
        var farIdx = 1
        var maxDistSq = -1f
        val p0 = points[0]
        for (i in 1 until points.size) {
            val dx = points[i].x - p0.x
            val dy = points[i].y - p0.y
            val d2 = dx * dx + dy * dy
            if (d2 > maxDistSq) {
                maxDistSq = d2
                farIdx = i
            }
        }
        if (maxDistSq < 1e-6f) return emptyList()

        val chainA = points.subList(0, farIdx + 1)
        val chainB = ArrayList<NormalizedPoint>(points.size - farIdx + 1).apply {
            addAll(points.subList(farIdx, points.size))
            add(points[0])
        }

        val simpA = simplifyPolylineIterative(chainA, epsilon)
        val simpB = simplifyPolylineIterative(chainB, epsilon)

        val merged = ArrayList<NormalizedPoint>(simpA.size + simpB.size)
        merged.addAll(simpA.dropLast(1))
        merged.addAll(simpB.dropLast(1))

        return if (merged.size >= 3) merged else points.take(3)
    }

    private fun simplifyPolylineIterative(
        pts: List<NormalizedPoint>,
        epsilon: Float
    ): List<NormalizedPoint> {
        val n = pts.size
        if (n <= 2) return pts

        val keep = BooleanArray(n)
        keep[0] = true
        keep[n - 1] = true

        val stack = ArrayDeque<IntArray>()
        stack.addLast(intArrayOf(0, n - 1))

        while (stack.isNotEmpty()) {
            val range = stack.removeLast()
            val start = range[0]
            val end = range[1]
            if (end - start <= 1) continue

            var maxDist = 0f
            var splitIdx = start
            val a = pts[start]
            val b = pts[end]

            for (i in (start + 1) until end) {
                val d = perpendicularDistance(pts[i], a, b)
                if (d > maxDist) {
                    maxDist = d
                    splitIdx = i
                }
            }

            if (maxDist > epsilon) {
                keep[splitIdx] = true
                stack.addLast(intArrayOf(start, splitIdx))
                stack.addLast(intArrayOf(splitIdx, end))
            }
        }

        val result = ArrayList<NormalizedPoint>()
        for (i in 0 until n) {
            if (keep[i]) result.add(pts[i])
        }
        return result
    }

    private fun perpendicularDistance(
        pt: NormalizedPoint,
        lineStart: NormalizedPoint,
        lineEnd: NormalizedPoint
    ): Float {
        val dx = lineEnd.x - lineStart.x
        val dy = lineEnd.y - lineStart.y
        val lenSq = dx * dx + dy * dy
        if (lenSq < 1e-10f) {
            return hypot(pt.x - lineStart.x, pt.y - lineStart.y)
        }
        val num = kotlin.math.abs(dy * pt.x - dx * pt.y + lineEnd.x * lineStart.y - lineEnd.y * lineStart.x)
        return num / sqrt(lenSq)
    }

    /**
     * Maps view-normalized polygons drawn on a FILL_CENTER PreviewView into raw unrotated
     * CameraX YUV_420_888 plane normalized [0, 1] coordinates without rotating pixels.
     */
    fun mapOverlayToRawSensorPolygons(
        overlay: OverlayPolygonState,
        rawSensorWidth: Int,
        rawSensorHeight: Int,
        rotationDegrees: Int
    ): List<List<NormalizedPoint>> {
        if (overlay.viewportWidthPx <= 0f || overlay.viewportHeightPx <= 0f ||
            rawSensorWidth <= 0 || rawSensorHeight <= 0
        ) {
            return overlay.viewNormalizedPolygons
        }

        val rot = ((rotationDegrees % 360) + 360) % 360
        val uprightW = if (rot == 90 || rot == 270) rawSensorHeight.toFloat() else rawSensorWidth.toFloat()
        val uprightH = if (rot == 90 || rot == 270) rawSensorWidth.toFloat() else rawSensorHeight.toFloat()

        // PreviewView FILL_CENTER center-crop inverse transform
        val scale = max(overlay.viewportWidthPx / uprightW, overlay.viewportHeightPx / uprightH)
        val scaledW = uprightW * scale
        val scaledH = uprightH * scale
        val cropOffsetX = (scaledW - overlay.viewportWidthPx) * 0.5f
        val cropOffsetY = (scaledH - overlay.viewportHeightPx) * 0.5f

        return overlay.viewNormalizedPolygons.map { poly ->
            poly.map { v ->
                val vxPx = v.x * overlay.viewportWidthPx
                val vyPx = v.y * overlay.viewportHeightPx
                val ux = ((vxPx + cropOffsetX) / scaledW).coerceIn(0f, 1f)
                val uy = ((vyPx + cropOffsetY) / scaledH).coerceIn(0f, 1f)

                when (rot) {
                    90 -> NormalizedPoint(x = uy, y = (1f - ux).coerceIn(0f, 1f))
                    180 -> NormalizedPoint(x = (1f - ux).coerceIn(0f, 1f), y = (1f - uy).coerceIn(0f, 1f))
                    270 -> NormalizedPoint(x = (1f - uy).coerceIn(0f, 1f), y = ux)
                    else -> NormalizedPoint(x = ux, y = uy)
                }
            }
        }
    }
}
