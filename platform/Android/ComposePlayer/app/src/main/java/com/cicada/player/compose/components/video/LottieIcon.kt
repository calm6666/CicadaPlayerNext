package com.cicada.player.compose.components.video

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.tween
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.airbnb.lottie.LottieComposition
import com.airbnb.lottie.compose.LottieAnimation
import com.airbnb.lottie.compose.LottieCompositionSpec
import com.airbnb.lottie.compose.rememberLottieComposition

/**
 * Lottie 图标（**逻辑照 Qt 的 `LottieIcon.qml`**）。
 *
 * 槽位语义一一对应：
 *   source / autoplay / startFrame / stopFrame / complete(next|stop|loop) / reverse；
 *   `playToken` 每 +1 = 播一遍当前槽（**先 snapTo 起点帧再 animateTo 末帧**，
 *   播完停在末帧 = Qt 的 `gotoAndStop(末帧)`）；
 *   `playToken<=0` 或 `!running` 时停到静止帧（stopFrame > startFrame(>0) > 第 0 帧）。
 *
 * 注意：lottie-compose 的 `LottieAnimation(composition, progress, modifier, ...)` 这个重载
 * **没有** contentDescription / colorFilter 参数，无障碍描述只能挂在 Modifier.semantics 上。
 */
enum class LottieComplete { Next, Stop, Loop }

@Immutable
data class LottieSlot(
    val source: String,
    val autoplay: Boolean = true,
    val startFrame: Int = -1,
    val stopFrame: Int = -1,
    val complete: LottieComplete = LottieComplete.Next,
    val reverse: Boolean = false,
)

private fun frameProgress(frame: Int, composition: LottieComposition): Float {
    val frames = composition.durationFrames
    return if (frames <= 0) 0f else (frame.toFloat() / frames).coerceIn(0f, 1f)
}

private fun restProgress(slot: LottieSlot, composition: LottieComposition): Float = when {
    slot.stopFrame >= 0 -> frameProgress(slot.stopFrame, composition)
    slot.startFrame > 0 -> frameProgress(slot.startFrame, composition)
    else -> 0f
}

private fun startProgress(slot: LottieSlot, composition: LottieComposition): Float =
    if (slot.startFrame > 0) frameProgress(slot.startFrame, composition) else 0f

@Composable
fun LottieIcon(
    sequence: List<LottieSlot>,
    slotIndex: Int,
    modifier: Modifier = Modifier,
    playToken: Int = 0,
    running: Boolean = true,
    contentDescription: String? = null,
    /**
     * 没有动画时的静止帧（0f = 第 0 帧，1f = 末帧）。
     * `null` = 按槽位自己的 stopFrame / startFrame / 第 0 帧算（Qt 的行为）。
     * 播放/暂停键会传它：这段 UI 会因为控制栏隐藏/显示被重建，重建时必须停在
     * **与当前播放状态相符**的那一帧，否则图标会退回"暂停"的样子。
     */
    restProgressOverride: Float? = null,
    onSlotFinished: (Int) -> Unit = {},
    onAdvanceSlot: () -> Unit = {},
) {
    val slot = sequence.getOrNull(slotIndex)
    if (slot == null) {
        Box(modifier)
        return
    }

    val composition by rememberLottieComposition(LottieCompositionSpec.Asset(slot.source))
    val progress = remember { Animatable(0f) }

    LaunchedEffect(playToken, slotIndex, composition, running) {
        val comp = composition ?: return@LaunchedEffect
        val rest = restProgressOverride ?: restProgress(slot, comp)

        if (!running || playToken <= 0) {
            progress.snapTo(rest)
            return@LaunchedEffect
        }

        val start = startProgress(slot, comp)
        val durationMs = comp.duration.toInt().coerceAtLeast(1)

        if (slot.complete == LottieComplete.Loop) {
            progress.snapTo(start)
            progress.animateTo(
                1f,
                infiniteRepeatable(tween(durationMs, easing = LinearEasing), RepeatMode.Restart),
            )
            return@LaunchedEffect
        }

        if (slot.reverse) {
            progress.snapTo(1f)
            progress.animateTo(start, tween(durationMs, easing = LinearEasing))
        } else {
            progress.snapTo(start)
            progress.animateTo(1f, tween(durationMs, easing = LinearEasing))
        }

        onSlotFinished(slotIndex)
        if (slot.complete == LottieComplete.Next) onAdvanceSlot()
    }

    val described = if (contentDescription != null) {
        modifier.semantics { this.contentDescription = contentDescription }
    } else {
        modifier
    }

    LottieAnimation(
        composition = composition,
        progress = { progress.value },
        modifier = described,
    )
}

// ---------------------------------------------------------------------------
// 播放 / 暂停键（**照 Qt 的 controls/LeftControls.qml 原样移植**）
//   · 两槽顺序：slot 0 = pause-to-play，slot 1 = play-to-pause，都 complete=Stop、autoplay=false；
//   · 状态映射（Qt 的 onPlayingChanged）：**playing → 槽 0；暂停 → 槽 1**，
//     槽不对就切槽，然后播一遍（play()）；播完停在末帧：
//       槽 0 的末帧 = 播放态该显示的样子（两条竖线）
//       槽 1 的末帧 = 暂停态该显示的样子（播放三角）
//   · 初始槽固定 0、挂载不播（Qt 明确写了"不要按 playing 去算初始槽"：
//     play-to-pause 的第 0 帧是空帧）。
//   · **Compose 的额外处理**：控制栏隐藏/显示会让这段 UI 重建，`remember` 里的槽位/token 会丢，
//     重建后若一律停"槽 0 第 0 帧"就会变成播放三角（播着却显示播放）。
//     所以重建时用 restProgressOverride 按**真实播放状态**停帧：
//       正在播放 → 1f（槽 0 末帧，两条竖线）；暂停 → 0f（播放三角）。
// ---------------------------------------------------------------------------

private val PLAY_PAUSE_SEQUENCE = listOf(
    LottieSlot("lottie-icon/pause-to-play-animation.json", autoplay = false, complete = LottieComplete.Stop),
    LottieSlot("lottie-icon/play-to-pause-animation.json", autoplay = false, complete = LottieComplete.Stop),
)

/** Qt 的映射：**播放中用 slot 0，暂停中用 slot 1**（不是按文件名猜的） */
private const val SLOT_PLAYING = 0
private const val SLOT_PAUSED = 1

/** 进度条上的 dot（Qt 的 Thumb 动画，静止在第 0 帧当静态徽标用） */
private val THUMB_SEQUENCE = listOf(
    LottieSlot("lottie-icon/Thumb-animation.json", autoplay = false, complete = LottieComplete.Loop),
)

@Composable
fun PlayPauseIconButton(
    isPlaying: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    size: Dp = 40.dp,
) {
    var slot by remember { mutableIntStateOf(0) }          // 初始槽 0（Qt 同款）
    var playToken by remember { mutableIntStateOf(0) }
    var mounted by remember { mutableStateOf(false) }

    /* 重建时（控制栏隐藏/显示）按真实状态停帧，修"切换后图标变回错的"那个 bug；null = 之后走正常动画 */
    var restOverride by remember { mutableStateOf<Float?>(if (isPlaying) 1f else 0f) }

    LaunchedEffect(isPlaying) {
        if (!mounted) {
            mounted = true
            return@LaunchedEffect
        }
        restOverride = null                                 // 之后走正常动画
        val want = if (isPlaying) SLOT_PLAYING else SLOT_PAUSED
        if (slot != want) slot = want                       // Qt：槽不对才切
        playToken++                                         // Qt：槽对也要 play() 一遍
    }

    Box(
        modifier = modifier
            .size(size)
            .clickable(onClick = onClick)
            .semantics { contentDescription = if (isPlaying) "暂停" else "播放" },
        contentAlignment = Alignment.Center,
    ) {
        LottieIcon(
            sequence = PLAY_PAUSE_SEQUENCE,
            slotIndex = slot,
            playToken = playToken,
            restProgressOverride = restOverride,
            modifier = Modifier.fillMaxSize(),
        )
    }
}

/** 进度条上的 dot 徽标（Thumb） */
@Composable
fun DanmakuDotThumb(modifier: Modifier = Modifier, size: Dp = 20.dp) {
    LottieIcon(
        sequence = THUMB_SEQUENCE,
        slotIndex = 0,
        playToken = 0,
        running = false,
        modifier = modifier.size(size),
    )
}
