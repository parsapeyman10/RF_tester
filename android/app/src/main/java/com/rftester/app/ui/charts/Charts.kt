package com.rftester.app.ui.charts

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.dp
import com.rftester.app.domain.model.ChartPoint
import com.rftester.app.util.TimeFmt
import kotlinx.coroutines.launch
import kotlin.math.abs

enum class ChartMode { LINE, AREA, BAR }

/**
 * نمودار سبک Canvas — بدون کتابخانه سنگین:
 *  - LINE / AREA / BAR
 *  - تپ و درگ برای انتخاب نقطه (Tooltip)
 *  - انیمیشن ورود نرم
 *  - محور X بر اساس بازه زمانی برچسب‌گذاری می‌شود
 */
@Composable
fun TimeSeriesChart(
    points: List<ChartPoint>,
    modifier: Modifier = Modifier,
    mode: ChartMode = ChartMode.AREA,
    lineColor: Color = MaterialTheme.colorScheme.primary,
    fillColor: Color = lineColor.copy(alpha = 0.25f),
    suffix: String = "",
    rangeMs: Long = 24L * 3600_000L,
    showGrid: Boolean = true,
    emptyText: String = "داده‌ای برای نمایش نیست"
) {
    val scope = rememberCoroutineScope()
    val progress = remember { Animatable(0f) }
    var selectedIndex by remember { mutableIntStateOf(-1) }
    val density = LocalDensity.current
    val labelPaint = remember {
        android.graphics.Paint().apply {
            color = android.graphics.Color.parseColor("#8FA3C1")
            textSize = with(density) { 10.sp.toPx() }
            isAntiAlias = true
        }
    }

    LaunchedEffect(points.size, mode) {
        progress.snapTo(0f)
        progress.animateTo(1f, animationSpec = tween(650))
    }

    if (points.isEmpty()) {
        Box(
            modifier = modifier
                .height(220.dp)
                .background(
                    MaterialTheme.colorScheme.surface,
                    MaterialTheme.shapes.large
                ),
            contentAlignment = Alignment.Center
        ) {
            Text(
                emptyText,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
        return
    }

    val gridColor = MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)
    val textColor = MaterialTheme.colorScheme.onSurfaceVariant

    Box(modifier = modifier.height(240.dp)) {
        Canvas(
            modifier = Modifier
                .fillMaxSize()
                .background(MaterialTheme.colorScheme.surface, MaterialTheme.shapes.large)
                .pointerInput(points) {
                    detectTapGestures { offset ->
                        val w = size.width.toFloat()
                        val padLeft = 36f
                        val padRight = 12f
                        val usable = w - padLeft - padRight
                        val rel = ((offset.x - padLeft) / usable).coerceIn(0f, 1f)
                        selectedIndex = (rel * (points.size - 1)).toInt()
                    }
                }
                .pointerInput(points) {
                    var dragging = false
                    detectDragGestures(
                        onDragStart = { dragging = true },
                        onDragEnd = { dragging = false }
                    ) { change, _ ->
                        change.consume()
                        val w = size.width.toFloat()
                        val padLeft = 36f
                        val padRight = 12f
                        val usable = w - padLeft - padRight
                        val rel = ((change.position.x - padLeft) / usable).coerceIn(0f, 1f)
                        selectedIndex = (rel * (points.size - 1)).toInt()
                    }
                }
        ) {
            val w = size.width
            val h = size.height
            val padLeft = 36f
            val padRight = 12f
            val padTop = 18f
            val padBottom = 26f
            val chartW = w - padLeft - padRight
            val chartH = h - padTop - padBottom

            val values = points.map { it.value }
            var minV = values.min()
            var maxV = values.max()
            if (abs(maxV - minV) < 0.001f) {
                minV -= 1f
                maxV += 1f
            }
            val margin = (maxV - minV) * 0.08f
            minV -= margin
            maxV += margin

            fun xAt(i: Int): Float =
                padLeft + if (points.size == 1) chartW / 2f
                else chartW * i / (points.size - 1).toFloat()

            fun yAt(v: Float): Float =
                padTop + chartH * (1f - (v - minV) / (maxV - minV))

            val p = progress.value

            // --- گرید + برچسب Y ---
            if (showGrid) {
                for (g in 0..4) {
                    val frac = g / 4f
                    val y = padTop + chartH * frac
                    drawLine(
                        color = gridColor,
                        start = Offset(padLeft, y),
                        end = Offset(w - padRight, y),
                        strokeWidth = 1f
                    )
                    val v = maxV - (maxV - minV) * frac
                    drawContext.canvas.nativeCanvas.drawText(
                        if (abs(v) >= 100) v.toInt().toString() else "%.1f".format(v),
                        4f,
                        y + 4f,
                        labelPaint
                    )
                }
                // برچسب X
                val ticks = 4
                for (t in 0..ticks) {
                    val idx = (t * (points.size - 1) / ticks)
                    val x = xAt(idx)
                    val label = TimeFmt.axisLabel(points[idx].ts, rangeMs)
                    drawContext.canvas.nativeCanvas.drawText(
                        label,
                        (x - 18f).coerceAtLeast(2f),
                        h - 8f,
                        labelPaint
                    )
                }
            }

            // --- مسیر اصلی ---
            val visibleCount = ((points.size) * p).toInt().coerceAtLeast(2)
                .coerceAtMost(points.size)

            when (mode) {
                ChartMode.LINE -> {
                    val path = Path()
                    for (i in 0 until visibleCount) {
                        val x = xAt(i)
                        val y = yAt(points[i].value)
                        if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
                    }
                    drawPath(
                        path,
                        color = lineColor,
                        style = Stroke(width = 3.5f, cap = StrokeCap.Round)
                    )
                }

                ChartMode.AREA -> {
                    val path = Path()
                    val fill = Path()
                    for (i in 0 until visibleCount) {
                        val x = xAt(i)
                        val y = yAt(points[i].value)
                        if (i == 0) {
                            path.moveTo(x, y)
                            fill.moveTo(x, padTop + chartH)
                            fill.lineTo(x, y)
                        } else {
                            path.lineTo(x, y)
                            fill.lineTo(x, y)
                        }
                    }
                    val lastX = xAt(visibleCount - 1)
                    fill.lineTo(lastX, padTop + chartH)
                    fill.close()
                    drawPath(
                        fill,
                        brush = Brush.verticalGradient(
                            colors = listOf(
                                lineColor.copy(alpha = 0.45f),
                                lineColor.copy(alpha = 0.02f)
                            )
                        )
                    )
                    drawPath(
                        path,
                        color = lineColor,
                        style = Stroke(width = 3.2f, cap = StrokeCap.Round)
                    )
                }

                ChartMode.BAR -> {
                    val n = points.size
                    val slot = chartW / n
                    val barW = (slot * 0.62f).coerceAtLeast(1.5f)
                    for (i in 0 until visibleCount) {
                        val x = padLeft + slot * i + (slot - barW) / 2f
                        val y = yAt(points[i].value)
                        drawRoundRect(
                            color = lineColor.copy(alpha = if (i % 2 == 0) 0.95f else 0.7f),
                            topLeft = Offset(x, y),
                            size = androidx.compose.ui.geometry.Size(
                                barW,
                                (padTop + chartH - y).coerceAtLeast(1f)
                            ),
                            cornerRadius = androidx.compose.ui.geometry.CornerRadius(
                                barW / 3f,
                                barW / 3f
                            )
                        )
                    }
                }
            }

            // --- نشانگر انتخاب ---
            if (selectedIndex in points.indices) {
                val i = selectedIndex
                val x = xAt(i)
                val y = yAt(points[i].value)
                drawLine(
                    color = lineColor.copy(alpha = 0.7f),
                    start = Offset(x, padTop),
                    end = Offset(x, padTop + chartH),
                    strokeWidth = 1.5f
                )
                drawCircle(color = Color.White, radius = 7f, center = Offset(x, y))
                drawCircle(color = lineColor, radius = 5f, center = Offset(x, y))

                val label = "%.1f%s".format(points[i].value, suffix)
                val tw = labelPaint.measureText(label)
                val bx = (x - tw / 2f - 8f).coerceIn(4f, w - tw - 20f)
                val by = (y - 34f).coerceAtLeast(4f)
                drawRoundRect(
                    color = Color(0xFF0F172A),
                    topLeft = Offset(bx, by),
                    size = androidx.compose.ui.geometry.Size(tw + 16f, 26f),
                    cornerRadius = androidx.compose.ui.geometry.CornerRadius(8f, 8f)
                )
                drawContext.canvas.nativeCanvas.drawText(
                    label,
                    bx + 8f,
                    by + 18f,
                    labelPaint.apply { color = android.graphics.Color.WHITE }
                )
                labelPaint.color = android.graphics.Color.parseColor("#8FA3C1")
            }
        }
    }
}

/**
 * اسپارک‌لاین کوچک داخل کارت‌ها
 */
@Composable
fun Sparkline(
    values: List<Float>,
    modifier: Modifier = Modifier,
    color: Color = MaterialTheme.colorScheme.primary
) {
    if (values.size < 2) {
        Box(modifier = modifier.height(40.dp))
        return
    }
    val progress = remember { Animatable(0f) }
    LaunchedEffect(values.size) {
        progress.snapTo(0f)
        progress.animateTo(1f, tween(500))
    }
    Canvas(modifier = modifier.height(44.dp)) {
        val w = size.width
        val h = size.height
        val min = values.min()
        val max = values.max()
        val span = if (abs(max - min) < 0.001f) 1f else (max - min)
        val visible = ((values.size) * progress.value).toInt().coerceIn(2, values.size)

        val path = Path()
        val fill = Path()
        for (i in 0 until visible) {
            val x = w * i / (values.size - 1)
            val y = h * 0.9f - (h * 0.8f) * ((values[i] - min) / span)
            if (i == 0) {
                path.moveTo(x, y)
                fill.moveTo(x, h)
                fill.lineTo(x, y)
            } else {
                path.lineTo(x, y)
                fill.lineTo(x, y)
            }
        }
        fill.lineTo(w * (visible - 1) / (values.size - 1), h)
        fill.close()
        drawPath(fill, color.copy(alpha = 0.18f))
        drawPath(path, color, style = Stroke(width = 2.5f, cap = StrokeCap.Round))
    }
}

/**
 * نمودار میله‌ای دسته‌ای (مثلاً الگوی ساعتی ۲۴ ساعته)
 */
@Composable
fun CategoryBars(
    values: List<Pair<String, Float>>, // برچسب، مقدار (NaN مجاز نیست → از قبل فیلتر)
    modifier: Modifier = Modifier,
    color: Color = MaterialTheme.colorScheme.secondary,
    suffix: String = ""
) {
    if (values.isEmpty()) {
        Box(
            modifier = modifier
                .height(180.dp)
                .background(MaterialTheme.colorScheme.surface, MaterialTheme.shapes.large),
            contentAlignment = Alignment.Center
        ) {
            Text(
                "داده‌ای موجود نیست",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
        return
    }
    val progress = remember { Animatable(0f) }
    LaunchedEffect(values.size) {
        progress.snapTo(0f)
        progress.animateTo(1f, tween(600))
    }
    val labelPaint = remember {
        android.graphics.Paint().apply {
            color = android.graphics.Color.parseColor("#8FA3C1")
            textSize = 18f
            isAntiAlias = true
            textAlign = android.graphics.Paint.Align.CENTER
        }
    }

    Canvas(
        modifier = modifier
            .height(200.dp)
            .background(MaterialTheme.colorScheme.surface, MaterialTheme.shapes.large)
    ) {
        val w = size.width
        val h = size.height
        val padBottom = 26f
        val padTop = 14f
        val max = values.maxOf { it.second }
        val min = 0f
        val span = if (max <= min) 1f else (max - min)
        val slot = w / values.size
        val p = progress.value

        for ((i, pair) in values.withIndex()) {
            val bh = (h - padTop - padBottom) * ((pair.second - min) / span) * p
            val bw = slot * 0.6f
            val x = slot * i + (slot - bw) / 2f
            val y = h - padBottom - bh
            drawRoundRect(
                color = color.copy(alpha = 0.85f),
                topLeft = Offset(x, y),
                size = androidx.compose.ui.geometry.Size(bw, bh.coerceAtLeast(2f)),
                cornerRadius = androidx.compose.ui.geometry.CornerRadius(bw / 3f, bw / 3f)
            )
            if (values.size <= 24 || i % 2 == 0) {
                drawContext.canvas.nativeCanvas.drawText(
                    pair.first,
                    x + bw / 2f,
                    h - 8f,
                    labelPaint
                )
            }
        }
    }
}

/**
 * حلقه پیشرفت دایره‌ای برای شاخص‌ها (مثلاً رطوبت %)
 */
@Composable
fun RadialGauge(
    fraction: Float,
    valueText: String,
    label: String,
    modifier: Modifier = Modifier,
    color: Color = MaterialTheme.colorScheme.secondary
) {
    val p = remember { Animatable(0f) }
    LaunchedEffect(fraction) {
        p.snapTo(0f)
        p.animateTo(fraction.coerceIn(0f, 1f), tween(700))
    }
    androidx.compose.foundation.layout.Column(
        modifier = modifier,
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Canvas(modifier = Modifier.fillMaxSize().padding(8.dp)) {
            val stroke = 14f
            val inset = stroke + 4f
            drawArc(
                color = color.copy(alpha = 0.15f),
                startAngle = -90f,
                sweepAngle = 360f,
                useCenter = false,
                topLeft = Offset(inset, inset),
                size = androidx.compose.ui.geometry.Size(
                    size.width - inset * 2,
                    size.height - inset * 2
                ),
                style = Stroke(width = stroke, cap = StrokeCap.Round)
            )
            drawArc(
                color = color,
                startAngle = -90f,
                sweepAngle = 360f * p.value,
                useCenter = false,
                topLeft = Offset(inset, inset),
                size = androidx.compose.ui.geometry.Size(
                    size.width - inset * 2,
                    size.height - inset * 2
                ),
                style = Stroke(width = stroke, cap = StrokeCap.Round)
            )
        }
        androidx.compose.foundation.layout.Column(
            modifier = Modifier.padding(bottom = 8.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Text(valueText, style = MaterialTheme.typography.headlineSmall)
            Text(
                label,
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}
