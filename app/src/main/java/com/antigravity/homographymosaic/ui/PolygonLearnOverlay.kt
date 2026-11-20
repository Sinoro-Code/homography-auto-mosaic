package com.antigravity.homographymosaic.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
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
import androidx.compose.material.icons.filled.CropFree
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
import androidx.compose.runtime.Stable
import androidx.compose.runtime.derivedStateOf
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
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sqrt

enum class PolygonInputMode {
    RECTANGLE_DRAG, // 矩形拖曳框選（最適合證件欄位、車牌、浮水印固定區域）
    FINGER_LASSO,   // 手指塗抹自由圈選 + Ramer-Douglas-Peucker 頂點簡化
    TAP_VERTICES    // 多邊形頂點精準點擊框選
}

@Stable
class PolygonEditorState {
    var canvasSize by mutableStateOf(IntSize.Zero)
    var inputMode by mutableStateOf(PolygonInputMode.RECTANGLE_DRAG)
    var rdpEpsilonRatio by mutableFloatStateOf(0.015f)

    // Completed polygons in upright [0..1] space (pair of points + whether RDP should simplify)
    val rawPolygons = mutableStateListOf<Pair<List<NormalizedPoint>, Boolean>>()

    // Active gesture points in canvas pixel coordinates
    val activePoints = mutableStateListOf<Offset>()
    var rectDragStart by mutableStateOf<Offset?>(null)
    var rectDragEnd by mutableStateOf<Offset?>(null)

    val simplifiedPolygons: List<List<NormalizedPoint>> by derivedStateOf {
        val out = ArrayList<List<NormalizedPoint>>(rawPolygons.size + 1)
        for ((poly, needsRdp) in rawPolygons) {
            val simp = if (needsRdp) {
                RdpSimplifier.simplifyClosedPolygon(poly, rdpEpsilonRatio)
            } else {
                poly
            }
            if (simp.size >= 3) out.add(simp)
        }
        if (inputMode == PolygonInputMode.TAP_VERTICES &&
            activePoints.size >= 3 &&
            canvasSize.width > 0 &&
            canvasSize.height > 0
        ) {
            val w = canvasSize.width.toFloat()
            val h = canvasSize.height.toFloat()
            out.add(
                activePoints.map {
                    NormalizedPoint(
                        (it.x / w).coerceIn(0f, 1f),
                        (it.y / h).coerceIn(0f, 1f)
                    )
                }
            )
        }
        out
    }

    fun commitActiveTapPolygon() {
        if (inputMode == PolygonInputMode.TAP_VERTICES &&
            activePoints.size >= 3 &&
            canvasSize.width > 0 &&
            canvasSize.height > 0
        ) {
            val w = canvasSize.width.toFloat()
            val h = canvasSize.height.toFloat()
            rawPolygons.add(
                activePoints.map {
                    NormalizedPoint(
                        (it.x / w).coerceIn(0f, 1f),
                        (it.y / h).coerceIn(0f, 1f)
                    )
                } to false
            )
        }
        activePoints.clear()
        rectDragStart = null
        rectDragEnd = null
    }

    fun switchMode(newMode: PolygonInputMode) {
        if (newMode == inputMode) return
        commitActiveTapPolygon()
        inputMode = newMode
    }

    fun undoLast() {
        if (activePoints.isNotEmpty()) {
            activePoints.removeAt(activePoints.lastIndex)
        } else if (rawPolygons.isNotEmpty()) {
            rawPolygons.removeAt(rawPolygons.lastIndex)
        }
        rectDragStart = null
        rectDragEnd = null
    }

    fun clearAll() {
        activePoints.clear()
        rawPolygons.clear()
        rectDragStart = null
        rectDragEnd = null
    }
}

@Composable
fun rememberPolygonEditorState(): PolygonEditorState {
    return remember { PolygonEditorState() }
}

/**
 * External toolbar placed cleanly above the aspect-fitted photo preview so no edge or corner
 * of the reference photo is ever occluded while drawing mosaic regions.
 */
@Composable
fun PolygonLearnToolbar(
    state: PolygonEditorState,
    modifier: Modifier = Modifier
) {
    val polygons = state.simplifiedPolygons
    val totalVerts = polygons.sumOf { it.size }

    Surface(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(16.dp),
        color = Color(0xFF131B2E)
    ) {
        Column(modifier = Modifier.padding(horizontal = 12.dp, vertical = 8.dp)) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    AssistChip(
                        onClick = { state.switchMode(PolygonInputMode.RECTANGLE_DRAG) },
                        label = { Text("矩形框選") },
                        leadingIcon = {
                            Icon(
                                Icons.Default.CropFree,
                                contentDescription = null,
                                modifier = Modifier.size(15.dp)
                            )
                        },
                        colors = AssistChipDefaults.assistChipColors(
                            containerColor = if (state.inputMode == PolygonInputMode.RECTANGLE_DRAG) {
                                Color(0xFF00E5A3).copy(alpha = 0.22f)
                            } else {
                                Color.Transparent
                            },
                            labelColor = Color.White
                        )
                    )

                    AssistChip(
                        onClick = { state.switchMode(PolygonInputMode.FINGER_LASSO) },
                        label = { Text("塗抹圈選") },
                        leadingIcon = {
                            Icon(
                                Icons.Default.Brush,
                                contentDescription = null,
                                modifier = Modifier.size(15.dp)
                            )
                        },
                        colors = AssistChipDefaults.assistChipColors(
                            containerColor = if (state.inputMode == PolygonInputMode.FINGER_LASSO) {
                                Color(0xFF00E5A3).copy(alpha = 0.22f)
                            } else {
                                Color.Transparent
                            },
                            labelColor = Color.White
                        )
                    )

                    AssistChip(
                        onClick = { state.switchMode(PolygonInputMode.TAP_VERTICES) },
                        label = { Text("多邊形") },
                        leadingIcon = {
                            Icon(
                                Icons.Default.Polyline,
                                contentDescription = null,
                                modifier = Modifier.size(15.dp)
                            )
                        },
                        colors = AssistChipDefaults.assistChipColors(
                            containerColor = if (state.inputMode == PolygonInputMode.TAP_VERTICES) {
                                Color(0xFF38BDF8).copy(alpha = 0.25f)
                            } else {
                                Color.Transparent
                            },
                            labelColor = Color.White
                        )
                    )
                }

                Row(verticalAlignment = Alignment.CenterVertically) {
                    if (state.inputMode == PolygonInputMode.TAP_VERTICES && state.activePoints.size >= 3) {
                        IconButton(onClick = { state.commitActiveTapPolygon() }) {
                            Icon(
                                Icons.Default.Add,
                                contentDescription = "鎖定此多邊形並新增下一區",
                                tint = Color(0xFF00E5A3)
                            )
                        }
                    }

                    IconButton(onClick = { state.undoLast() }) {
                        Icon(Icons.Default.Undo, contentDescription = "復原", tint = Color.White)
                    }

                    IconButton(onClick = { state.clearAll() }) {
                        Icon(
                            Icons.Default.DeleteSweep,
                            contentDescription = "清除全部打碼區",
                            tint = Color(0xFFF87171)
                        )
                    }
                }
            }

            if (state.inputMode == PolygonInputMode.FINGER_LASSO) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        text = "DP 簡化 ε=${"%.3f".format(state.rdpEpsilonRatio)} | 區域:${polygons.size} 頂點:$totalVerts",
                        style = MaterialTheme.typography.labelSmall,
                        fontWeight = FontWeight.Medium,
                        color = Color(0xFFCBD5E1)
                    )
                    Spacer(modifier = Modifier.width(8.dp))
                    Slider(
                        value = state.rdpEpsilonRatio,
                        onValueChange = { state.rdpEpsilonRatio = it },
                        valueRange = 0.004f..0.045f,
                        modifier = Modifier.weight(1f)
                    )
                }
            } else {
                Text(
                    text = "已設定打碼區域: ${polygons.size} 區（共 $totalVerts 頂點）· 可在同一張相片框選多處",
                    style = MaterialTheme.typography.labelSmall,
                    color = Color(0xFF94A3B8),
                    modifier = Modifier.padding(top = 2.dp)
                )
            }
        }
    }
}

/**
 * 1:1 aspect-matched interactive Canvas placed directly over the upright reference photo Surface.
 * Coordinates map directly to upright [0..1] normalized image space.
 */
@Composable
fun PolygonLearnCanvas(
    state: PolygonEditorState,
    modifier: Modifier = Modifier
) {
    val polygons = state.simplifiedPolygons

    Canvas(
        modifier = modifier
            .fillMaxSize()
            .onSizeChanged { state.canvasSize = it }
            .pointerInput(state.canvasSize) {
                val cornerHitRadiusPx = 28.dp.toPx()
                val minRectNorm = 0.02f

                // Active drag interaction state
                var activeMode = 0 // 0=none, 1=drawNew, 2=resizeCorner, 3=moveRect
                var activeRectIdx = -1
                var anchorNormX = 0f
                var anchorNormY = 0f

                detectDragGestures(
                    onDragStart = { start ->
                        val w = state.canvasSize.width.toFloat()
                        val h = state.canvasSize.height.toFloat()
                        if (w <= 0f || h <= 0f) return@detectDragGestures

                        activeMode = 0
                        activeRectIdx = -1

                        // 1. Hit-test 4 corners of existing rectangles (topmost first)
                        for (idx in state.rawPolygons.lastIndex downTo 0) {
                            val pts = state.rawPolygons[idx].first
                            if (pts.size != 4) continue
                            var bestCorner = -1
                            var bestDist = cornerHitRadiusPx
                            for (c in 0..3) {
                                val cx = pts[c].x * w
                                val cy = pts[c].y * h
                                val d = hypot(start.x - cx, start.y - cy)
                                if (d <= bestDist) {
                                    bestDist = d
                                    bestCorner = c
                                }
                            }
                            if (bestCorner >= 0) {
                                activeMode = 2
                                activeRectIdx = idx
                                // Opposite corner stays anchored while dragging this corner
                                val opp = (bestCorner + 2) % 4
                                anchorNormX = pts[opp].x
                                anchorNormY = pts[opp].y
                                break
                            }
                        }

                        // 2. Hit-test interior of existing rectangles (topmost first) for drag-to-move
                        if (activeMode == 0) {
                            for (idx in state.rawPolygons.lastIndex downTo 0) {
                                val pts = state.rawPolygons[idx].first
                                if (pts.size != 4) continue
                                val minX = pts.minOf { it.x } * w
                                val maxX = pts.maxOf { it.x } * w
                                val minY = pts.minOf { it.y } * h
                                val maxY = pts.maxOf { it.y } * h
                                if (start.x in minX..maxX && start.y in minY..maxY) {
                                    activeMode = 3
                                    activeRectIdx = idx
                                    break
                                }
                            }
                        }

                        // 3. Otherwise start drawing a new rectangle
                        if (activeMode == 0) {
                            activeMode = 1
                            state.rectDragStart = start
                            state.rectDragEnd = start
                        }
                    },
                    onDrag = { change, dragAmount ->
                        change.consume()
                        val w = state.canvasSize.width.toFloat()
                        val h = state.canvasSize.height.toFloat()
                        if (w <= 0f || h <= 0f) return@detectDragGestures

                        when (activeMode) {
                            1 -> {
                                state.rectDragEnd = change.position
                            }
                            2 -> {
                                if (activeRectIdx in state.rawPolygons.indices) {
                                    val curX = (change.position.x / w).coerceIn(0f, 1f)
                                    val curY = (change.position.y / h).coerceIn(0f, 1f)
                                    var x0 = min(anchorNormX, curX)
                                    var x1 = max(anchorNormX, curX)
                                    var y0 = min(anchorNormY, curY)
                                    var y1 = max(anchorNormY, curY)
                                    if (x1 - x0 < minRectNorm) {
                                        if (curX >= anchorNormX) x1 = (x0 + minRectNorm).coerceAtMost(1f)
                                        else x0 = (x1 - minRectNorm).coerceAtLeast(0f)
                                    }
                                    if (y1 - y0 < minRectNorm) {
                                        if (curY >= anchorNormY) y1 = (y0 + minRectNorm).coerceAtMost(1f)
                                        else y0 = (y1 - minRectNorm).coerceAtLeast(0f)
                                    }
                                    state.rawPolygons[activeRectIdx] = listOf(
                                        NormalizedPoint(x0, y0),
                                        NormalizedPoint(x1, y0),
                                        NormalizedPoint(x1, y1),
                                        NormalizedPoint(x0, y1)
                                    ) to false
                                }
                            }
                            3 -> {
                                if (activeRectIdx in state.rawPolygons.indices) {
                                    val pts = state.rawPolygons[activeRectIdx].first
                                    if (pts.size == 4) {
                                        val x0 = pts.minOf { it.x }
                                        val x1 = pts.maxOf { it.x }
                                        val y0 = pts.minOf { it.y }
                                        val y1 = pts.maxOf { it.y }
                                        val dx = (dragAmount.x / w).coerceIn(-x0, 1f - x1)
                                        val dy = (dragAmount.y / h).coerceIn(-y0, 1f - y1)
                                        val nx0 = x0 + dx
                                        val nx1 = x1 + dx
                                        val ny0 = y0 + dy
                                        val ny1 = y1 + dy
                                        state.rawPolygons[activeRectIdx] = listOf(
                                            NormalizedPoint(nx0, ny0),
                                            NormalizedPoint(nx1, ny0),
                                            NormalizedPoint(nx1, ny1),
                                            NormalizedPoint(nx0, ny1)
                                        ) to false
                                    }
                                }
                            }
                        }
                    },
                    onDragEnd = {
                        if (activeMode == 1) {
                            val s = state.rectDragStart
                            val e = state.rectDragEnd
                            val w = state.canvasSize.width.toFloat()
                            val h = state.canvasSize.height.toFloat()
                            if (s != null && e != null && w > 0f && h > 0f &&
                                abs(e.x - s.x) >= 8f && abs(e.y - s.y) >= 8f
                            ) {
                                val x0 = (min(s.x, e.x) / w).coerceIn(0f, 1f)
                                val y0 = (min(s.y, e.y) / h).coerceIn(0f, 1f)
                                val x1 = (max(s.x, e.x) / w).coerceIn(0f, 1f)
                                val y1 = (max(s.y, e.y) / h).coerceIn(0f, 1f)
                                state.rawPolygons.add(
                                    listOf(
                                        NormalizedPoint(x0, y0),
                                        NormalizedPoint(x1, y0),
                                        NormalizedPoint(x1, y1),
                                        NormalizedPoint(x0, y1)
                                    ) to false
                                )
                            }
                        }
                        activeMode = 0
                        activeRectIdx = -1
                        state.rectDragStart = null
                        state.rectDragEnd = null
                    },
                    onDragCancel = {
                        activeMode = 0
                        activeRectIdx = -1
                        state.rectDragStart = null
                        state.rectDragEnd = null
                    }
                )
            }
    ) {
        val w = size.width
        val h = size.height

        // Draw finalized / simplified mosaic regions
        for (poly in polygons) {
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
                color = Color(0x4800E5A3)
            )
            drawPath(
                path = path,
                color = Color(0xFF00E5A3),
                style = Stroke(
                    width = 2.8.dp.toPx(),
                    cap = StrokeCap.Round,
                    join = StrokeJoin.Round
                )
            )
            for (v in poly) {
                drawCircle(
                    color = Color(0xFF0B0F17),
                    radius = 7.dp.toPx(),
                    center = Offset(v.x * w, v.y * h)
                )
                drawCircle(
                    color = Color(0xFF38BDF8),
                    radius = 4.8.dp.toPx(),
                    center = Offset(v.x * w, v.y * h)
                )
            }
        }

        // Draw live rectangle drag preview
        val rStart = state.rectDragStart
        val rEnd = state.rectDragEnd
        if (rStart != null && rEnd != null) {
            val left = min(rStart.x, rEnd.x).coerceIn(0f, w)
            val top = min(rStart.y, rEnd.y).coerceIn(0f, h)
            val right = max(rStart.x, rEnd.x).coerceIn(0f, w)
            val bottom = max(rStart.y, rEnd.y).coerceIn(0f, h)
            val rw = right - left
            val rh = bottom - top
            if (rw > 2f && rh > 2f) {
                drawRect(
                    color = Color(0x3838BDF8),
                    topLeft = Offset(left, top),
                    size = Size(rw, rh)
                )
                drawRect(
                    color = Color(0xFF38BDF8),
                    topLeft = Offset(left, top),
                    size = Size(rw, rh),
                    style = Stroke(
                        width = 2.5.dp.toPx(),
                        pathEffect = PathEffect.dashPathEffect(floatArrayOf(14f, 10f), 0f)
                    )
                )
            }
        }

        // Draw live finger lasso trail while dragging
        if (state.inputMode == PolygonInputMode.FINGER_LASSO && state.activePoints.size >= 2) {
            val livePath = Path().apply {
                moveTo(state.activePoints[0].x, state.activePoints[0].y)
                for (i in 1 until state.activePoints.size) {
                    lineTo(state.activePoints[i].x, state.activePoints[i].y)
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
        } else if (state.inputMode == PolygonInputMode.TAP_VERTICES && state.activePoints.size in 1..2) {
            for (pt in state.activePoints) {
                drawCircle(color = Color(0xFF38BDF8), radius = 6.dp.toPx(), center = pt)
            }
            if (state.activePoints.size == 2) {
                drawLine(
                    color = Color(0xFF38BDF8),
                    start = state.activePoints[0],
                    end = state.activePoints[1],
                    strokeWidth = 2.5.dp.toPx()
                )
            }
        }
    }
}

/**
 * Iterative (zero-recursion) Ramer-Douglas-Peucker closed polygon simplifier.
 */
object RdpSimplifier {

    fun simplifyClosedPolygon(
        points: List<NormalizedPoint>,
        epsilon: Float
    ): List<NormalizedPoint> {
        if (points.size <= 4) return points

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
        val num = abs(dy * pt.x - dx * pt.y + lineEnd.x * lineStart.y - lineEnd.y * lineStart.x)
        return num / sqrt(lenSq)
    }
}
