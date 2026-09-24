package com.cicada.player.compose.components.danmaku

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.dp

/**
 * 弹幕层（**硬件级 GPU 渲染，不占 CPU**）。
 *
 * 做法（每一帧的主线程工作量只有"N 个 drawImage"）：
 *   1. 文字**只在进场时**排版一次 → [DanmakuBitmapCache] 里的 ImageBitmap；
 *   2. 每帧只 `drawImage`（GPU blit；Compose 的绘制在 RenderThread 上跑）；
 *   3. 位移由**媒体时钟**算：播放时按帧间隔推进、播放器位置来了就软收敛（>250ms 硬对齐），
 *      暂停时时钟不动 ⇒ 弹幕自动冻结；
 *   4. 轨道交给 [DanmakuLaneEngine]（规则照 Qt 引擎），不会随机落行、不会固定弹幕重叠。
 *
 * 状态读写约定（别破坏，否则又会变成"每帧重组"）：`active` 只在 LaunchedEffect 里增删；
 * 绘制 lambda 只**读** `clockMs` / `active` ⇒ 每帧只让绘制阶段失效，不重组本组件。
 */
@Composable
fun DanmakuView(
    danmakus: List<Danmaku>,
    config: DanmakuConfig,
    isPlaying: Boolean,
    currentTime: Long,
    modifier: Modifier = Modifier,
) {
    val density = LocalDensity.current
    val cache = remember { DanmakuBitmapCache() }
    var clockMs by remember { mutableFloatStateOf(currentTime.toFloat()) }
    val active = remember { mutableStateListOf<DanmakuSprite>() }
    val spawned = remember { mutableSetOf<String>() }

    /* 时钟：只在播放时按帧推进（暂停即冻结，不空转） */
    LaunchedEffect(isPlaying) {
        if (!isPlaying) return@LaunchedEffect
        var lastNano = 0L
        while (true) {
            withFrameNanos { now ->
                if (lastNano != 0L) clockMs += (now - lastNano) / 1_000_000f
                lastNano = now
            }
        }
    }

    /* 与播放器位置对齐：小偏差软收敛（防抖），大偏差硬对齐（seek/切档） */
    LaunchedEffect(currentTime) {
        val pos = currentTime.toFloat()
        val delta = pos - clockMs
        if (delta > 250f || delta < -250f) {
            clockMs = pos
            active.clear()
            spawned.clear()
        } else {
            clockMs += delta * 0.2f
        }
    }

    /* 显示开关/字号/速度变了：已有位图和时长失效，清掉重来 */
    LaunchedEffect(config.textSize, config.speed, config.isEnabled) {
        active.clear()
        spawned.clear()
        cache.clear()
    }

    BoxWithConstraints(modifier = modifier) {
        val widthPx = with(density) { maxWidth.toPx() }
        val heightPx = with(density) { maxHeight.toPx() }
        val textSizePx = config.textSize * density.density
        val lineHeightPx = textSizePx + 12f
        val laneCount = DanmakuLaneEngine.laneCountFor(heightPx, lineHeightPx, config.maxLines)
        val engine = remember(laneCount, widthPx) { DanmakuLaneEngine(laneCount, widthPx) }
        val scrollDuration = DanmakuLaneEngine.scrollDurationMs(config)
        val safeDistance = DanmakuLaneEngine.safeDistancePx(textSizePx)

        /* 进场：唯一的 CPU 文字排版（每条一次）+ 轨道分配 */
        LaunchedEffect(currentTime, danmakus, config.isEnabled) {
            if (!config.isEnabled) {
                active.clear()
                spawned.clear()
                return@LaunchedEffect
            }

            val clock = clockMs
            active.removeAll { it.isExpired(clock) }

            danmakus.forEach { d ->
                if (spawned.contains(d.id)) return@forEach

                val age = clock - d.time
                if (age < 0f || age > 1500f) return@forEach

                val show = when (d.type) {
                    DanmakuType.SCROLL -> config.showScroll
                    DanmakuType.TOP -> config.showTop
                    DanmakuType.BOTTOM -> config.showBottom
                }
                if (!show) { spawned.add(d.id); return@forEach }

                val image = cache.bitmapFor(d.text, textSizePx, d.color.toArgb())
                val w = image.width.toFloat()
                val duration =
                    if (d.type == DanmakuType.SCROLL) scrollDuration
                    else DanmakuLaneEngine.FIXED_DURATION_MS

                val lane = engine.pickLane(d.type, w, duration, active, clock, safeDistance)

                active.add(
                    DanmakuSprite(
                        id = d.id,
                        image = image,
                        widthPx = w,
                        laneIndex = lane,
                        spawnClockMs = clock,
                        durationMs = duration,
                        type = d.type,
                    )
                )
                spawned.add(d.id)
            }

            while (active.size > 300) active.removeAt(0)
        }

        /* 绘制：每帧只提交 GPU blit，不排版、不分配 */
        Canvas(modifier = Modifier.fillMaxSize()) {
            if (!config.isEnabled) return@Canvas

            val clock = clockMs
            for (i in 0 until active.size) {
                val s = active[i]
                val x = if (s.type == DanmakuType.SCROLL) engine.scrollX(s, clock)
                else (size.width - s.widthPx) / 2f

                val alpha = if (s.type == DanmakuType.SCROLL) {
                    config.opacity
                } else {
                    val remain = s.durationMs - s.elapsedMs(clock)
                    config.opacity * (remain / 300f).coerceIn(0f, 1f)
                }
                if (alpha <= 0.01f) continue

                drawImage(
                    image = s.image,
                    topLeft = Offset(x, s.laneIndex * lineHeightPx + 4.dp.toPx()),
                    alpha = alpha,
                )
            }
        }
    }
}
