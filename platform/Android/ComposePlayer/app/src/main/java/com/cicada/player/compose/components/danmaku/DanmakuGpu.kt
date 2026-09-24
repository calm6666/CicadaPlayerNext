package com.cicada.player.compose.components.danmaku

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Typeface
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap

/**
 * 弹幕 GPU 渲染的两块基础设施：**文字位图缓存** + **轨道引擎**。
 *
 * 为什么要这样：文字排版（TextMeasurer/Paint.measureText）是 CPU 活，
 * 如果每帧给每条弹幕排一次版，弹幕一多主线程就满、画面就卡。
 * 所以：**每条弹幕进场时只排版一次 → 存成 ImageBitmap**；之后每帧只做 `drawImage`
 * （GPU blit，绘制跑在 RenderThread 上），主线程不再碰文字。
 */
class DanmakuSprite(
    val id: String,
    /** 文字位图（含黑描边），进场时生成一次 */
    val image: ImageBitmap,
    val widthPx: Float,
    val laneIndex: Int,
    val spawnClockMs: Float,
    val durationMs: Float,
    val type: DanmakuType,
) {
    fun progress(clockMs: Float): Float =
        if (durationMs <= 0f) 0f else ((clockMs - spawnClockMs) / durationMs).coerceIn(0f, 1f)

    fun elapsedMs(clockMs: Float): Float = clockMs - spawnClockMs

    fun isExpired(clockMs: Float): Boolean = elapsedMs(clockMs) >= durationMs
}

/** 文字位图 LRU 缓存（键＝文本+字号+颜色） */
class DanmakuBitmapCache(private val maxEntries: Int = 200) {

    private data class Key(val text: String, val sizePx: Int, val color: Int)

    private val cache = object : LinkedHashMap<Key, ImageBitmap>(32, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<Key, ImageBitmap>): Boolean =
            size > maxEntries
    }

    /** 生成一条弹幕的位图（B 站风格：黑描边 + 实心文字），**只做一次** */
    fun bitmapFor(text: String, textSizePx: Float, colorArgb: Int): ImageBitmap {
        val sizePx = textSizePx.toInt().coerceAtLeast(1)
        val key = Key(text, sizePx, colorArgb)
        cache[key]?.let { return it }

        val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            textSize = textSizePx
            color = colorArgb
            typeface = Typeface.DEFAULT_BOLD
        }
        val metrics = paint.fontMetrics
        val pad = 4f
        val w = (paint.measureText(text) + pad * 2).toInt().coerceAtLeast(1)
        val h = (metrics.descent - metrics.ascent + pad * 2).toInt().coerceAtLeast(1)

        val bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bitmap)
        val stroke = Paint(paint).apply {
            color = android.graphics.Color.BLACK
            style = Paint.Style.STROKE
            strokeWidth = 3f
            strokeJoin = Paint.Join.ROUND
        }
        val baseline = pad - metrics.ascent
        canvas.drawText(text, pad, baseline, stroke)
        canvas.drawText(text, pad, baseline, paint)

        return bitmap.asImageBitmap().also { cache[key] = it }
    }

    fun clear() = cache.clear()
}

/**
 * 轨道引擎。规则**照 Qt 的引擎**（`CicadaPlayerNext/danmaku/src/DanmakuEngine.cpp`）：
 *   * 滚动弹幕：安全距离判"塞不塞得下"，并做追尾判定（新弹幕更快且旧弹幕还在右半边 → 换道）；
 *   * 固定弹幕（顶/底）：**跨类型也占**（顶部不许占用底部正在用的轨道，反之亦然）；底部**自下而上**找空轨。
 * 纯计算、无副作用。
 */
class DanmakuLaneEngine(private val laneCount: Int, private val canvasWidthPx: Float) {

    fun pickLane(
        type: DanmakuType,
        spriteWidthPx: Float,
        durationMs: Float,
        active: List<DanmakuSprite>,
        clockMs: Float,
        safeDistancePx: Float,
    ): Int = when (type) {
        DanmakuType.SCROLL -> pickScrollLane(spriteWidthPx, durationMs, active, clockMs, safeDistancePx)
        DanmakuType.TOP -> (0 until laneCount).firstOrNull { fixedLaneFree(it, active, clockMs) } ?: 0
        DanmakuType.BOTTOM -> (laneCount - 1 downTo 0)
            .firstOrNull { fixedLaneFree(it, active, clockMs) } ?: (laneCount - 1)
    }

    private fun fixedLaneFree(lane: Int, active: List<DanmakuSprite>, clockMs: Float): Boolean =
        active.none { it.laneIndex == lane && it.type != DanmakuType.SCROLL && !it.isExpired(clockMs) }

    private fun pickScrollLane(
        newWidthPx: Float,
        newDurationMs: Float,
        active: List<DanmakuSprite>,
        clockMs: Float,
        safeDistancePx: Float,
    ): Int {
        val w = canvasWidthPx
        val minRemaining = if (safeDistancePx > 80f) 2000f else 1000f

        for (lane in 0 until laneCount) {
            var ok = true
            for (sprite in active) {
                if (sprite.laneIndex != lane) continue

                if (sprite.type != DanmakuType.SCROLL) {
                    if (!sprite.isExpired(clockMs)) { ok = false; break }
                    continue
                }

                val existingX = scrollX(sprite, clockMs)
                if (existingX > w - newWidthPx - safeDistancePx * 2f) { ok = false; break }

                if (existingX + sprite.widthPx > 0f) {
                    if (sprite.durationMs - sprite.elapsedMs(clockMs) < minRemaining) continue
                    val newSpeed = (w + newWidthPx) / newDurationMs.coerceAtLeast(1f)
                    val oldSpeed = (w + sprite.widthPx) / sprite.durationMs.coerceAtLeast(1f)
                    if (newSpeed > oldSpeed && existingX > w / 2f) { ok = false; break }
                }
            }
            if (ok) return lane
        }

        return (0 until laneCount).minByOrNull { lane ->
            active.filter { it.laneIndex == lane && it.type == DanmakuType.SCROLL }
                .maxOfOrNull { it.spawnClockMs } ?: Float.NEGATIVE_INFINITY
        } ?: 0
    }

    /** 滚动弹幕的 x：从右侧进，滚到完全出左侧 */
    fun scrollX(sprite: DanmakuSprite, clockMs: Float): Float =
        canvasWidthPx - (canvasWidthPx + sprite.widthPx) * sprite.progress(clockMs)

    companion object {
        /** 滚动弹幕存活时长（整屏穿越约 8 秒，速度倍率放大则更短） */
        fun scrollDurationMs(config: DanmakuConfig): Float =
            8000f / config.speed.multiplier.coerceAtLeast(0.1f)

        /** 固定弹幕停留时长 */
        const val FIXED_DURATION_MS = 5000f

        fun safeDistancePx(textSizePx: Float): Float = textSizePx * 2f + 20f

        fun laneCountFor(canvasHeightPx: Float, lineHeightPx: Float, maxLines: Int): Int =
            if (lineHeightPx <= 0f) 1
            else (canvasHeightPx / lineHeightPx).toInt().coerceIn(1, maxLines.coerceAtLeast(1))
    }
}
