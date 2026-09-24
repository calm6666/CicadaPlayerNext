package com.cicada.player.compose.components.video

import android.annotation.SuppressLint
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.util.Base64
import android.util.LruCache
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.Image
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.drag
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.Layout
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.compose.AsyncImagePainter
import coil.compose.rememberAsyncImagePainter
import com.cicada.player.compose.R
import com.cicada.player.compose.ui.theme.PlayerTheme
import kotlin.math.abs
import kotlin.math.roundToLong
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext

/**
 * 播放器控制层。**尺寸/位置全部按手机截图量，不引任何网页或电脑端的值；手机没有 hover。**
 *
 * 这一层只有截图里出现过的东西：
 *   * 进度条（细、已播=主题色、dot 用 Thumb 的 Lottie 动画）；
 *   * **快进预览图**（拖动时上方弹卡片：取到帧=缩略图+时间；没取到=圆形加载占位）；
 *   * 底部栏（全屏：播放键 + 弹幕开关图标 + 弹幕设置图标 + 弹幕输入框 + 字幕/倍速/自动；
 *     非全屏：播放键 + 时间 + 竖屏胶囊 + 竖屏/全屏两颗 SVG）；
 *   * 顶部栏（返回 + 标题 + 在线人数 + 关注）。
 * **没有**点赞/收藏/投币/充电/分享、三个点、锁屏、首页图标、听书/画中画。
 */

fun formatPlayerTime(timeMs: Long): String {
    if (timeMs <= 0) return "00:00"
    val total = timeMs / 1000
    val s = total % 60
    val m = (total / 60) % 60
    val h = total / 3600
    return if (h > 0) "%02d:%02d:%02d".format(h, m, s) else "%02d:%02d".format(m, s)
}

// ---------------------------------------------------------------------------
// 进度条
// ---------------------------------------------------------------------------

/**
 * 松手 seek 后：`positionMs` 落到"目标 ± 该容差"内才把进度条交还给它。
 * 容差覆盖内核"定位到最近关键帧"的偏差 —— 落在关键帧上时实际位置会略早于点击处。
 */
private const val SEEK_SETTLE_TOLERANCE_MS = 700L

/** 松手 seek 后挂起目标位置的兜底时长：超时仍未对上就交还给 `positionMs`（seek 被忽略时用） */
private const val SEEK_SETTLE_TIMEOUT_MS = 2500L

/**
 * **常驻细进度条**（控制栏隐藏后仍然显示，贴在播放器**最底部**）。
 * 已播 = 主题色，未播 = 白 20%，中间压一层已缓冲（白 30%）。
 * **不带 dot** —— dot 只有控制栏里那条 [PlayerProgressBar] 才有。
 */
@SuppressLint("UnusedBoxWithConstraintsScope")
@Composable
fun PlayerThinProgressBar(
    positionMs: Long,
    durationMs: Long,
    bufferedMs: Long,
    modifier: Modifier = Modifier,
    trackHeight: Dp = 2.dp,
) {
    val ratio = if (durationMs > 0) (positionMs.toFloat() / durationMs).coerceIn(0f, 1f) else 0f
    val bufRatio = if (durationMs > 0) (bufferedMs.toFloat() / durationMs).coerceIn(0f, 1f) else 0f

    /* 高度就是轨道高度：贴底时不会多出空白 */
    BoxWithConstraints(modifier = modifier.fillMaxWidth().height(trackHeight)) {
        val widthPx = constraints.maxWidth.toFloat()

        Box(Modifier.fillMaxWidth().height(trackHeight).background(PlayerTheme.progressTrack))
        Box(
            Modifier
                .width(with(LocalDensity.current) { (widthPx * bufRatio).toDp() })
                .height(trackHeight).background(PlayerTheme.progressBuffer)
        )
        Box(
            Modifier
                .width(with(LocalDensity.current) { (widthPx * ratio).toDp() })
                .height(trackHeight).background(PlayerTheme.accent)
        )
    }
}

/**
 * 控制栏里的进度条：**点一下也能跳 + 拖动跳**，拖动时上方弹**快进预览图**。
 *
 * 取帧说明：预览帧由 **App 层**喂进来（`videoshot/preview.bin` 按 `\u001F` 切成 data URL 数组，
 * 每 5 秒一档、`arr[0]` 是空占位），播放器只负责按下标取图。
 * 内核 `CicadaPlayer` 只有 `snapshot()`（截**当前**帧），**不负责按时间取帧**，所以这里不做 seek 换帧。
 */
@SuppressLint("UnusedBoxWithConstraintsScope")
@Composable
fun PlayerProgressBar(
    positionMs: Long,
    durationMs: Long,
    bufferedMs: Long,
    onSeek: (Long) -> Unit,
    onSeekStart: () -> Unit,
    onSeekEnd: () -> Unit,
    modifier: Modifier = Modifier,
    previewPositionMs: Long = 0L,
    previewActive: Boolean = false,
    /**
     * 快进预览帧列表（data URL 或图片 URL；空 = 没有预览图）。
     * 数据格式：`videoshot/preview.bin` 按 `\u001F` 切出来的数组，**每 5 秒一档**，`arr[0]` 是空占位。
     */
    previewFrames: List<String> = emptyList(),
    touchHeight: Dp = 20.dp,
    trackHeight: Dp = 4.dp,
    thumbSize: Dp = 20.dp,
) {
    var dragging by remember { mutableStateOf(false) }
    var dragRatio by remember { mutableFloatStateOf(0f) }
    /*
     * **松手后不能立刻交还给 `positionMs`**：seek 是异步的（内核要 flush 解码器、再重新定位到
     * 关键帧），松手那一帧 `positionMs` 还是旧值。以前 `dragging = false` 一写下去，进度条和 dot
     * 立刻按旧的 `positionMs` 重算 → 先弹回旧位置，等内核 seek 完再跳过去，用户看到的就是
     * "点一下 → dot 到点击处 → 又弹回去 → 等一下才到点击处"。
     *
     * 所以松手后把**目标位置挂起**（pendingSeekMs）继续显示它，直到播放位置真的追上目标；
     * 再加一个超时兜底（seek 被忽略、或暂停态下位置不刷新时不会永久卡在点击处）。
     * 主流播放器（YouTube / ExoPlayer 系 UI）都是这个做法：bar 停在 seek 目标位，直到 position 追上。
     */
    var pendingSeekMs by remember { mutableStateOf<Long?>(null) }

    /*
     * 播放位置追到目标（±容差）即视为定位完成，撤掉挂起。
     * 判据必须是**双向** `abs()`：往回拖时内核还没定位完，`positionMs` 仍是旧的"更靠后"的值，
     * 单向的 `positionMs >= target - 容差` 会立刻误判成"已完成"，dot 又弹回去。
     * 挂起期间 `positionMs` 收到任何旧值（内核 seek 前后常推一帧旧位置）都一律忽略。
     */
    LaunchedEffect(pendingSeekMs, positionMs) {
        val target = pendingSeekMs ?: return@LaunchedEffect
        if (abs(positionMs - target) <= SEEK_SETTLE_TOLERANCE_MS) pendingSeekMs = null
    }
    /* 兜底：SEEK_SETTLE_TIMEOUT_MS 内没追上也不再挂着 */
    LaunchedEffect(pendingSeekMs) {
        val target = pendingSeekMs ?: return@LaunchedEffect
        delay(SEEK_SETTLE_TIMEOUT_MS)
        if (pendingSeekMs == target) pendingSeekMs = null
    }

    val safeDuration = durationMs.coerceAtLeast(1L)
    val pendingRatio = pendingSeekMs?.let { (it.toFloat() / safeDuration).coerceIn(0f, 1f) }
    val playedRatio = when {
        dragging -> dragRatio
        pendingRatio != null -> pendingRatio
        else -> (positionMs.toFloat() / safeDuration).coerceIn(0f, 1f)
    }
    val bufRatio = (bufferedMs.toFloat() / safeDuration).coerceIn(0f, 1f)
    /* 指示点（拖动 / 播放）在轨道上的比例：截图 4 的小圆占位锚在它上面 */
    val indicatorRatio = when {
        dragging -> dragRatio
        pendingRatio != null -> pendingRatio
        else -> (previewPositionMs.toFloat() / safeDuration).coerceIn(0f, 1f)
    }

    BoxWithConstraints(modifier = modifier.fillMaxWidth()) {
        val trackWidthDp = with(LocalDensity.current) { constraints.maxWidth.toDp() }
        /*
         * `constraints` 属于 BoxWithConstraintsScope，而 BoxWithConstraintsScope / BoxScope
         * 都带 @LayoutScopeMarker（@DslMarker）：在里面的 Box { } content lambda（隐式接收者
         * 是 BoxScope）里再取外层的 `constraints` 会报
         *   'val constraints: Constraints' cannot be called in this context with an implicit receiver
         * 所以在外层先取出来，内层只用局部变量。
         */
        val widthPx = constraints.maxWidth.toFloat()

        /*
         * 快进预览（**唯一依据是用户给的两张手机截图，不用 Qt 的任何 popup 尺寸/摆法**）。
         *
         * 显隐：**拖动中 / 预览中显示，松手即收起** —— 截图 3/4 就是拖动中的样子。
         * 尺寸与位置全部在 [SeekPreviewPopup] 里按截图定：有图 = 23% 宽、水平居中、底边离轨道
         * 上沿 90dp 的卡片；没图 = 锚在指示点上的小圆加载占位。
         */
        if (dragging || previewActive) {
            SeekPreviewPopup(
                positionMs = if (dragging) (dragRatio * safeDuration).roundToLong() else previewPositionMs,
                durationMs = durationMs,
                previewFrames = previewFrames,
                indicatorX = trackWidthDp * indicatorRatio,
                trackWidth = trackWidthDp,
                trackTop = (touchHeight - trackHeight) / 2,
            )
        }

        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(touchHeight)
                .pointerInput(safeDuration) {
                    awaitEachGesture {
                        val down = awaitFirstDown()
                        dragging = true
                        onSeekStart()

                        fun report(x: Float) {
                            dragRatio = (x / size.width.toFloat()).coerceIn(0f, 1f)
                        }

                        report(down.position.x)
                        drag(down.id) { change ->
                            report(change.position.x)
                            change.consume()
                        }

                        /*
                         * **松手才 seek**（拖动过程中只更新预览图/时间）。
                         * 以前每移动一下就 seek 一次，一次拖动会给内核灌几十个 seek，
                         * 每个 seek 都会 flush 解码器 —— 内核很容易被打进
                         * "dequeue_out occur error. flush state 1" 的死循环（日志刷屏那种）。
                         * 主流 App 也是这个行为：拖动看预览，松手才跳。
                         */
                        val target = (dragRatio * safeDuration).roundToLong()
                        /*
                         * **先挂起目标位置，再结束拖动**（顺序不能反）：
                         * 反了这一帧会先按旧的 positionMs 画一次，dot 弹回去再跳回来。
                         */
                        pendingSeekMs = target
                        onSeek(target)
                        dragging = false
                        onSeekEnd()
                    }
                },
            contentAlignment = Alignment.CenterStart,
        ) {
            Box(
                Modifier.fillMaxWidth().height(trackHeight)
                    .clip(RoundedCornerShape(trackHeight / 2)).background(PlayerTheme.progressTrack)
            )
            Box(
                Modifier.width(with(LocalDensity.current) { (widthPx * bufRatio).toDp() })
                    .height(trackHeight)
                    .clip(RoundedCornerShape(trackHeight / 2)).background(PlayerTheme.progressBuffer)
            )
            Box(
                Modifier.width(with(LocalDensity.current) { (widthPx * playedRatio).toDp() })
                    .height(trackHeight)
                    .clip(RoundedCornerShape(trackHeight / 2)).background(PlayerTheme.accent)
            )
            DanmakuDotThumb(
                size = thumbSize,
                modifier = Modifier
                    .align(Alignment.CenterStart)
                    .offset(x = with(LocalDensity.current) {
                        (widthPx * playedRatio - thumbSize.toPx() / 2f).coerceAtLeast(0f).toDp()
                    }),
            )
        }
    }
}

/**
 * "不参与父布局测量"的浮层容器：自己对父布局宣告 0x0，子项各自用 `Modifier.offset` 摆位置。
 * 否则预览卡片会被父 Box 当成子项尺寸，把进度条那一行撑到卡片那么高。
 * （这只是**布局机制**，不是照抄 Qt 的摆法。）
 */
@Composable
private fun PreviewOverlay(content: @Composable () -> Unit) {
    Layout(content = content) { measurables, constraints ->
        val loose = constraints.copy(minWidth = 0, minHeight = 0)
        val placeables = measurables.map { it.measure(loose) }
        layout(0, 0) { placeables.forEach { it.place(0, 0) } }
    }
}

/** 截图 3：卡片宽 ≈ 播放器宽度的 23%（截图里量出的比例），高按 16:9 */
private const val PREVIEW_CARD_WIDTH_RATIO = 0.23f

/** 截图 3：卡片底边离**轨道上沿**约 90dp（截图里卡片大致落在屏幕竖直中部） */
private val previewCardGapAboveTrack = 90.dp

/** 卡片是圆角矩形 + 一圈浅色描边 */
private val previewCardRadius = 4.dp
private val previewCardBorder = Color(0x66FFFFFF)

/** 卡片正下方那行 `当前 / 总时长` 的字号（截图里明显比控制栏文字大） */
private const val PREVIEW_TIME_FONT_SIZE = 16

/** 小圆形占位贴着指示点 —— 圆在拇指右侧一点、底边贴着轨道上沿 */
private val previewDotSize = 36.dp
private val previewDotOffsetX = 4.dp
private val previewDotGap = 6.dp

/**
 * 快进预览（**唯一依据 = 用户给的两张手机截图**，不用 Qt 的 popup 尺寸/摆法/跟随算法）。
 *
 * **截图 3（该档的图已就绪）**
 *   * 圆角矩形卡片 + 一圈浅色描边，**水平居中于播放器**（不跟着拇指走）；
 *   * 宽 = 播放器宽度的 23%（截图里量出的比例）、高 = 16:9；
 *   * 卡片**底边离轨道上沿约 90dp**（截图里卡片大致落在屏幕竖直中部）；
 *   * 卡片**正下方居中**一行 `当前 / 总时长`：大号白字 + 半透明黑底。
 *
 * **截图 4（图还没就绪 / 该档没有帧）**
 *   * **小圆形占位 + 中间加载指示**，锚在**当前拖动/播放的指示点**上（拇指右侧一点、贴着进度条），
 *     不居中；
 *   * 图一就绪就换成截图 3 的居中卡片。
 *
 * 帧下标 = `floor(毫秒 / 5000) + 1` —— 这是**取帧语义**（每 5 秒一档、`arr[0]` 是空占位），不是 UI。
 * 该档没有帧时**不退回内核的"当前帧"**：那是播放头所在那一秒的画面，跟手指指的那一秒不是同一格。
 */
@Composable
fun SeekPreviewPopup(
    positionMs: Long,
    durationMs: Long,
    previewFrames: List<String>,
    /** 拖动/播放指示点在轨道上的 x（截图 4 的小圆锚点） */
    indicatorX: Dp,
    /** 轨道（播放器）宽度：卡片 23% 宽 + 水平居中都用它 */
    trackWidth: Dp,
    /** 轨道上沿相对本层的 y：卡片"底边离轨道上沿 90dp"用它 */
    trackTop: Dp,
) {
    val frameUrl = previewFrames
        .getOrNull((positionMs / PREVIEW_FRAME_INTERVAL_MS).toInt() + 1)
        ?.takeIf { it.isNotBlank() }

    val cardWidth = trackWidth * PREVIEW_CARD_WIDTH_RATIO
    val cardHeight = cardWidth * 9f / 16f
    val cardX = (trackWidth - cardWidth) / 2
    /* 卡片那块图的下边 = 轨道上沿往上 90dp（时间文字在卡片下面，所以这里按卡片高算） */
    val cardY = trackTop - previewCardGapAboveTrack - cardHeight

    PreviewOverlay {
        when {
            /* 该档没有帧 → 截图 4 的样子 */
            frameUrl == null -> PreviewLoadingDot(indicatorX = indicatorX, trackTop = trackTop)

            /* 帧是 data URL（预览接口给的就是这种）：App 层自己解，解完切卡片 */
            frameUrl.startsWith("data:", ignoreCase = true) -> {
                val bitmap by produceState<Bitmap?>(previewFrameCache.get(frameUrl), frameUrl) {
                    if (value == null) {
                        val decoded = withContext(Dispatchers.IO) { decodePreviewDataUrl(frameUrl) }
                        if (decoded != null) {
                            previewFrameCache.put(frameUrl, decoded)
                            value = decoded
                        }
                    }
                }

                val frame = bitmap
                if (frame == null) {
                    PreviewLoadingDot(indicatorX = indicatorX, trackTop = trackTop)
                } else {
                    PreviewCard(
                        x = cardX,
                        y = cardY,
                        width = cardWidth,
                        height = cardHeight,
                        positionMs = positionMs,
                        durationMs = durationMs,
                    ) {
                        Image(
                            bitmap = frame.asImageBitmap(),
                            contentDescription = null,
                            /* 帧就是整张卡片那么大，直接铺满 */
                            contentScale = ContentScale.FillBounds,
                            modifier = Modifier.fillMaxSize(),
                        )
                    }
                }
            }

            /* 帧是普通图片 URL：交给 Coil，Success 才切卡片，否则还是小圆占位 */
            else -> {
                val painter = rememberAsyncImagePainter(model = frameUrl)
                if (painter.state is AsyncImagePainter.State.Success) {
                    PreviewCard(
                        x = cardX,
                        y = cardY,
                        width = cardWidth,
                        height = cardHeight,
                        positionMs = positionMs,
                        durationMs = durationMs,
                    ) {
                        Image(
                            painter = painter,
                            contentDescription = null,
                            contentScale = ContentScale.FillBounds,
                            modifier = Modifier.fillMaxSize(),
                        )
                    }
                } else {
                    PreviewLoadingDot(indicatorX = indicatorX, trackTop = trackTop)
                }
            }
        }
    }
}

/** 截图 3 的卡片：圆角矩形 + 浅色描边，图铺满；下面居中一行 `当前 / 总时长`（大号白字 + 半透明黑底） */
@Composable
private fun PreviewCard(
    x: Dp,
    y: Dp,
    width: Dp,
    height: Dp,
    positionMs: Long,
    durationMs: Long,
    content: @Composable () -> Unit,
) {
    Column(
        modifier = Modifier.offset(x = x, y = y).width(width),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(height)
                .clip(RoundedCornerShape(previewCardRadius))
                .background(Color.Black)
                .border(1.dp, previewCardBorder, RoundedCornerShape(previewCardRadius)),
            contentAlignment = Alignment.Center,
        ) { content() }

        Spacer(Modifier.height(4.dp))

        Text(
            text = formatPlayerTime(positionMs) + " / " + formatPlayerTime(durationMs),
            color = Color.White,
            fontSize = PREVIEW_TIME_FONT_SIZE.sp,
            maxLines = 1,
            modifier = Modifier
                .clip(RoundedCornerShape(3.dp))
                .background(PlayerTheme.previewTimeBg)
                .padding(horizontal = 6.dp, vertical = 2.dp),
        )
    }
}

/** 截图 4：小圆形占位 + 中间加载指示，锚在指示点上（拇指右侧一点、底边贴着轨道上沿） */
@Composable
private fun PreviewLoadingDot(indicatorX: Dp, trackTop: Dp) {
    Box(
        modifier = Modifier
            .offset(x = indicatorX + previewDotOffsetX, y = trackTop - previewDotGap - previewDotSize)
            .size(previewDotSize)
            .clip(CircleShape)
            .background(PlayerTheme.previewPlaceholderBg),
        contentAlignment = Alignment.Center,
    ) {
        CircularProgressIndicator(
            modifier = Modifier.size(18.dp),
            color = Color.White,
            strokeWidth = 2.dp,
        )
    }
}

/** `data:image/...;base64,<payload>` → Bitmap（失败返回 null，交给占位）。只在 IO 线程调用 */
private fun decodePreviewDataUrl(dataUrl: String): Bitmap? {
    val comma = dataUrl.indexOf(',')
    if (comma <= 0) return null
    if (!dataUrl.substring(0, comma).contains("base64", ignoreCase = true)) return null
    return runCatching {
        val bytes = Base64.decode(dataUrl.substring(comma + 1), Base64.DEFAULT)
        BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
    }.getOrNull()
}

/** 预览帧缓存：最多 12 帧（一屏拖动最多经过几档，够用且不占内存） */
private val previewFrameCache = object : LruCache<String, Bitmap>(12) {
    override fun sizeOf(key: String, value: Bitmap): Int = 1
}

/** 预览帧每 5 秒一档（**取帧语义**：和上游预览接口 `videoshot/preview.bin` 一致，不是 UI 数值） */
private const val PREVIEW_FRAME_INTERVAL_MS = 5_000L

// ---------------------------------------------------------------------------
// 底部栏
// ---------------------------------------------------------------------------

/**
 * 底部栏。两种模式内容不同（照两张截图）：
 *   * 非全屏：进度条一行 + `[播放键][时间] …… [竖屏胶囊][竖屏 SVG][全屏 SVG]`
 *   * 全屏：`[播放键][弹幕开关][弹幕设置][弹幕输入框] …… [字幕][倍速][自动]`
 * 整块吃掉空白处的点击，避免穿到画面区把视频点暂停。
 */
@Composable
fun PlayerBottomBar(
    isPlaying: Boolean,
    positionMs: Long,
    durationMs: Long,
    bufferedMs: Long,
    isFullscreen: Boolean,
    danmakuEnabled: Boolean,
    onPlayPause: () -> Unit,
    onSeek: (Long) -> Unit,
    onSeekStart: () -> Unit,
    onSeekEnd: () -> Unit,
    onTogglePortraitFullscreen: () -> Unit,
    onToggleFullscreen: () -> Unit,
    onToggleDanmaku: () -> Unit,
    onDanmakuSettings: () -> Unit,
    onDanmakuSend: (String) -> Unit,
    onSubtitleClick: () -> Unit,
    onSpeedClick: () -> Unit,
    onQualityClick: () -> Unit,
    modifier: Modifier = Modifier,
    onInteraction: () -> Unit = {},
    /** 弹幕输入框聚焦状态（聚焦=键盘抬起期间控制栏不要自动隐藏） */
    onDanmakuInputFocusChanged: (Boolean) -> Unit = {},
    qualityLabel: String? = null,
    speedLabel: String? = null,
    /** 快进预览帧列表（App 层喂进来），转给进度条；空表示没有预览图 */
    previewFrames: List<String> = emptyList(),
) {
    Column(
        modifier = modifier
            .fillMaxWidth()
            .clickable(onClick = onInteraction)
            .padding(horizontal = 12.dp, vertical = 4.dp),
    ) {
        /* 进度条（全屏在底部栏里也有一条；常驻的那条在播放器层，见 PlayerThinProgressBar） */
        PlayerProgressBar(
            positionMs = positionMs,
            durationMs = durationMs,
            bufferedMs = bufferedMs,
            onSeek = { onInteraction(); onSeek(it) },
            onSeekStart = { onInteraction(); onSeekStart() },
            onSeekEnd = onSeekEnd,
            modifier = Modifier.padding(bottom = 2.dp),
            previewFrames = previewFrames,
        )

        Row(
            modifier = Modifier.fillMaxWidth().height(36.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            PlayPauseIconButton(
                isPlaying = isPlaying,
                onClick = { onInteraction(); onPlayPause() },
                size = 40.dp,
            )

            if (isFullscreen) {
                /* 截图 2：弹幕开关 + 弹幕设置（两颗图标从 Qt 工程拷贝复用） */
                PlayerIconButton(
                    iconRes = if (danmakuEnabled) R.drawable.ic_danmaku_switch_on else R.drawable.ic_danmaku_switch_off,
                    contentDescription = if (danmakuEnabled) "关闭弹幕" else "开启弹幕",
                    onClick = { onInteraction(); onToggleDanmaku() },
                )
                /* 弹幕设置图标（Qt 的 DanmakuSetting.svg 转成 drawable 后直接复用） */
                PlayerIconButton(
                    iconRes = R.drawable.ic_danmaku_setting,
                    contentDescription = "弹幕设置",
                    onClick = { onInteraction(); onDanmakuSettings() },
                )

                /* 弹幕输入框（截图 2 那条胶囊） */
                DanmakuInputBar(
                    modifier = Modifier.weight(1f).padding(horizontal = 8.dp),
                    onSend = { text ->
                        onDanmakuSend(text)
                        onInteraction()
                    },
                    onInteraction = onInteraction,
                    onFocusChanged = onDanmakuInputFocusChanged,
                )

                BarText(if (speedLabel.isNullOrEmpty()) "倍速" else speedLabel) {
                    onInteraction(); onSpeedClick()
                }
                BarText(qualityLabel ?: "自动") {
                    onInteraction(); onQualityClick()
                }
            } else {
                Spacer(Modifier.width(6.dp))
                Text(
                    text = formatPlayerTime(positionMs) + " / " + formatPlayerTime(durationMs),
                    color = PlayerTheme.controlText,
                    fontSize = 12.sp,
                    maxLines = 1,
                    modifier = Modifier.padding(horizontal = 6.dp),
                )

                Spacer(Modifier.weight(1f))

                BarText(if (speedLabel.isNullOrEmpty()) "倍速" else speedLabel) {
                    onInteraction(); onSpeedClick()
                }
                BarText(qualityLabel ?: "自动") {
                    onInteraction(); onQualityClick()
                }

                /* 截图 1：竖屏胶囊 + 竖屏 SVG + 全屏 SVG */
                Text(
                    text = "竖屏",
                    color = PlayerTheme.controlTextActive,
                    fontSize = 12.sp,
                    modifier = Modifier
                        .padding(horizontal = 6.dp)
                        .clip(RoundedCornerShape(11.dp))
                        .background(PlayerTheme.thumbBadgeBg)
                        .padding(horizontal = 10.dp, vertical = 3.dp)
                        .clickable { onInteraction(); onTogglePortraitFullscreen() },
                )
                PlayerIconButton(
                    iconRes = R.drawable.ic_player_portrait_fullscreen,
                    contentDescription = "竖屏全屏",
                    onClick = { onInteraction(); onTogglePortraitFullscreen() },
                )
                PlayerIconButton(
                    iconRes = R.drawable.ic_player_fullscreen,
                    contentDescription = "全屏",
                    onClick = { onInteraction(); onToggleFullscreen() },
                )
            }
        }

        /* 字幕只在全屏那一行（截图 2）；放这里保持和截图一致的行内顺序 */
        if (isFullscreen) {
            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                BarText("字幕") { onInteraction(); onSubtitleClick() }
            }
        }
    }
}

/** 底部栏的文字入口（字幕 / 倍速 / 自动这种小字按钮） */
@Composable
private fun BarText(text: String, onClick: () -> Unit) {
    Text(
        text = text,
        color = PlayerTheme.controlTextActive,
        fontSize = 13.sp,
        maxLines = 1,
        modifier = Modifier
            .clickable(onClick = onClick)
            .padding(horizontal = 8.dp),
    )
}

/**
 * 弹幕输入框（截图 2 的胶囊：占位文案 + 发送）
 *   · 宽度：外层 Box 吃掉 weight（BasicTextField 自带 280dp 最小宽，直接 weight 会把两边按钮挤变形）；
 *   · 光标：白色（默认黑色，和文字不一致）；
 *   · 光标位置：装饰盒居中，不然输入框比文字高时初始光标会贴顶（偏上）；
 *   · 聚焦状态通过 [onFocusChanged] 上报，控制栏在输入时不自动隐藏。
 */
@Composable
private fun DanmakuInputBar(
    modifier: Modifier = Modifier,
    onSend: (String) -> Unit,
    onInteraction: () -> Unit,
    onFocusChanged: (Boolean) -> Unit = {},
) {
    var text by remember { mutableStateOf("") }

    Row(
        modifier = modifier
            .height(32.dp)
            .clip(RoundedCornerShape(16.dp))
            .background(PlayerTheme.previewPlaceholderBg)
            .padding(horizontal = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(modifier = Modifier.weight(1f), contentAlignment = Alignment.CenterStart) {
            androidx.compose.foundation.text.BasicTextField(
                value = text,
                onValueChange = { text = it; onInteraction() },
                singleLine = true,
                textStyle = androidx.compose.ui.text.TextStyle(
                    color = PlayerTheme.controlTextActive,
                    fontSize = 13.sp,
                    lineHeight = 18.sp,
                ),
                cursorBrush = SolidColor(PlayerTheme.controlTextActive),
                modifier = Modifier
                    .fillMaxWidth()
                    .onFocusChanged { onFocusChanged(it.isFocused) },
                decorationBox = { inner ->
                    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.CenterStart) {
                        if (text.isEmpty()) {
                            Text(
                                text = "发个友善的弹幕见证当下",
                                color = PlayerTheme.controlText,
                                fontSize = 13.sp,
                            )
                        }
                        inner()
                    }
                },
            )
        }
        Text(
            text = "发送",
            color = PlayerTheme.controlText,
            fontSize = 13.sp,
            modifier = Modifier.clickable {
                if (text.isNotBlank()) {
                    onSend(text.trim())
                    text = ""
                }
            },
        )
    }
}

// ---------------------------------------------------------------------------
// 顶部栏（截图 2：返回 + 标题 + 在线人数 + 关注）
// ---------------------------------------------------------------------------

@Composable
fun PlayerTopBar(
    title: String,
    watchingText: String?,
    onBack: () -> Unit,
    modifier: Modifier = Modifier,
    onTitleClick: (() -> Unit)? = null,
    onFollowClick: (() -> Unit)? = null,
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .clickable(enabled = true) { }
            .padding(horizontal = 8.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        IconButtonBox(onClick = onBack) {
            Icon(
                imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                contentDescription = "返回",
                tint = PlayerTheme.controlTextActive,
                modifier = Modifier.size(24.dp),
            )
        }

        Column(
            modifier = Modifier
                .weight(1f)
                .padding(start = 4.dp)
                .then(if (onTitleClick != null) Modifier.clickable { onTitleClick() } else Modifier),
        ) {
            if (title.isNotEmpty()) {
                Text(
                    text = title,
                    color = PlayerTheme.controlTextActive,
                    fontSize = 15.sp,
                    fontWeight = FontWeight.Medium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            if (!watchingText.isNullOrEmpty()) {
                Text(text = watchingText, color = PlayerTheme.controlText, fontSize = 12.sp, maxLines = 1)
            }
        }

        if (onFollowClick != null) {
            Text(
                text = "+ 关注",
                color = PlayerTheme.controlTextActive,
                fontSize = 13.sp,
                modifier = Modifier
                    .clip(RoundedCornerShape(14.dp))
                    .background(PlayerTheme.previewPlaceholderBg)
                    .padding(horizontal = 12.dp, vertical = 4.dp)
                    .clickable { onFollowClick() },
            )
        }
    }
}

// ---------------------------------------------------------------------------
// 通用小件
// ---------------------------------------------------------------------------

/** 36dp 热区的图标按钮（按压反馈就是 clickable 自带的水波纹；手机端没有 hover 状态） */
@Composable
fun PlayerIconButton(
    iconRes: Int,
    contentDescription: String?,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    buttonSize: Dp = 36.dp,
    iconSize: Dp = 26.dp,
) {
    IconButtonBox(onClick = onClick, modifier = modifier, size = buttonSize) {
        Icon(
            painter = painterResource(iconRes),
            contentDescription = contentDescription,
            tint = PlayerTheme.controlText,
            modifier = Modifier.size(iconSize),
        )
    }
}

@Composable
fun IconButtonBox(
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    size: Dp = 36.dp,
    content: @Composable () -> Unit,
) {
    Box(
        modifier = modifier.size(size).clip(CircleShape).clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) { content() }
}
