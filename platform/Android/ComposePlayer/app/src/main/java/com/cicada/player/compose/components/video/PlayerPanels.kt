package com.cicada.player.compose.components.video

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.drag
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.cicada.player.compose.ui.theme.PlayerTheme

/**
 * 播放器浮层面板（底 #212121 @90%、圆角 4、上下留白 9dp、每项左右留白 20dp、选中项用主题色）。
 * 尺寸就是照着手机截图量的。
 *
 * **没有色彩调整面板**（用户明确：那一块 UI 后续再给）。
 */
@Composable
fun PlayerPanelShell(
    modifier: Modifier = Modifier,
    width: Dp = 160.dp,
    content: @Composable () -> Unit,
) {
    val interaction = remember { MutableInteractionSource() }

    Column(
        modifier = modifier
            .width(width)
            /* 上限 + 可滚动：density 420 下每行被放大，面板太高会把控制条顶出画面 */
            .heightIn(max = 220.dp)
            .background(PlayerTheme.panelBg, RoundedCornerShape(4.dp))
            /* 面板自己吃掉点击：点面板空白处不能穿到画面区把视频点暂停 */
            .clickable(interactionSource = interaction, indication = null) { }
            .verticalScroll(rememberScrollState())
            .padding(vertical = 9.dp),
        content = { content() },
    )
}

@Composable
private fun PanelTitle(text: String) {
    Text(
        text = text,
        color = PlayerTheme.panelSubText,
        fontSize = 10.sp,
        modifier = Modifier.padding(horizontal = 20.dp, vertical = 1.dp),
    )
}

@Composable
fun PanelRow(text: String, selected: Boolean, onClick: () -> Unit) {
    Text(
        text = text,
        color = if (selected) PlayerTheme.accent else PlayerTheme.panelText,
        fontSize = 13.sp,
        fontWeight = if (selected) FontWeight.Medium else FontWeight.Normal,
        maxLines = 1,
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(horizontal = 20.dp, vertical = 4.dp),
    )
}

/** 清晰度（数据来自内核 `MediaInfo` 的视频轨，见 PlayerStats.buildQualities） */
@Composable
fun PlayerQualityPanel(
    qualities: List<QualityOption>,
    onPick: (QualityOption) -> Unit,
    modifier: Modifier = Modifier,
) {
    PlayerPanelShell(modifier = modifier) {
        PanelTitle("清晰度")
        if (qualities.isEmpty()) {
            PanelRow("自动（当前分辨率）", selected = true) { }
        } else {
            qualities.forEach { q -> PanelRow(q.label, q.selected) { onPick(q) } }
        }
        /* 用户要求：只有一档时只显示"自动"（buildQualities 已处理） */
    }
}

/** 倍速 */
@Composable
fun PlayerSpeedPanel(current: Float, onPick: (Float) -> Unit, modifier: Modifier = Modifier) {
    PlayerPanelShell(modifier = modifier) {
        PanelTitle("倍速播放")
        listOf(0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f).forEach { s ->
            PanelRow(if (s == 1.0f) "正常" else "${s}x", kotlin.math.abs(current - s) < 0.01f) { onPick(s) }
        }
    }
}

/** 字幕（数据来自内核 MediaInfo 的字幕轨，见 PlayerStats.buildSubtitles） */
@Composable
fun PlayerSubtitlePanel(
    tracks: List<SubtitleOption>,
    onPick: (SubtitleOption) -> Unit,
    modifier: Modifier = Modifier,
) {
    PlayerPanelShell(modifier = modifier) {
        PanelTitle("字幕")
        if (tracks.isEmpty()) {
            Text(
                text = "当前片源没有字幕轨",
                color = PlayerTheme.panelSubText,
                fontSize = 12.sp,
                modifier = Modifier.padding(horizontal = 20.dp, vertical = 4.dp),
            )
        } else {
            tracks.forEach { t -> PanelRow(t.label, t.selected) { onPick(t) } }
        }
    }
}

/** 弹幕设置 */
@Composable
fun PlayerDanmakuPanel(
    enabled: Boolean,
    opacity: Int,
    fontScale: Int,
    density: Int,
    showScroll: Boolean,
    showTop: Boolean,
    showBottom: Boolean,
    onToggleEnabled: () -> Unit,
    onOpacity: (Int) -> Unit,
    onFontScale: (Int) -> Unit,
    onDensity: (Int) -> Unit,
    onToggleScroll: () -> Unit,
    onToggleTop: () -> Unit,
    onToggleBottom: () -> Unit,
    modifier: Modifier = Modifier,
) {
    PlayerPanelShell(modifier = modifier, width = 180.dp) {
        PanelTitle("弹幕设置")
        PanelRow("显示弹幕", enabled) { onToggleEnabled() }

        PanelTitle("不透明度")
        listOf(100, 75, 50, 25).forEach { v -> PanelRow("$v%", opacity == v) { onOpacity(v) } }

        PanelTitle("字号缩放")
        listOf(50, 75, 100, 125, 150).forEach { v -> PanelRow("$v%", fontScale == v) { onFontScale(v) } }

        PanelTitle("同屏密度（最多几行）")
        listOf(4, 6, 8, 12).forEach { v -> PanelRow("$v 行", density == v) { onDensity(v) } }

        PanelTitle("显示区域")
        PanelRow("滚动弹幕", showScroll) { onToggleScroll() }
        PanelRow("顶部弹幕", showTop) { onToggleTop() }
        PanelRow("底部弹幕", showBottom) { onToggleBottom() }
    }
}

/** 音量（竖向滑条：轨道 2dp、已填充主题色、圆点 12dp） */
@Composable
fun PlayerVolumePanel(
    volume: Float,
    muted: Boolean,
    onVolume: (Float) -> Unit,
    onToggleMute: () -> Unit,
    modifier: Modifier = Modifier,
) {
    PlayerPanelShell(modifier = modifier, width = 170.dp) {
        PanelTitle("音量")
        Row(
            modifier = Modifier.fillMaxWidth().padding(vertical = 6.dp),
            horizontalArrangement = Arrangement.Center,
        ) {
            Text(
                text = if (muted) "静音" else "${(volume * 100).toInt()}",
                color = Color(0xFFE5E9EF),
                fontSize = 14.sp,
                fontWeight = FontWeight.Bold,
            )
        }
        Box(modifier = Modifier.fillMaxWidth().height(70.dp), contentAlignment = Alignment.Center) {
            VerticalVolumeSlider(volume = if (muted) 0f else volume, onVolume = onVolume)
        }
        PanelRow(if (muted) "取消静音" else "静音", muted) { onToggleMute() }
    }
}

@Composable
private fun VerticalVolumeSlider(volume: Float, onVolume: (Float) -> Unit) {
    BoxWithConstraints(
        modifier = Modifier
            .width(32.dp)
            .height(60.dp)
            .pointerInput(Unit) {
                awaitEachGesture {
                    val down = awaitFirstDown()
                    fun report(y: Float) = onVolume(1f - (y / size.height.toFloat()).coerceIn(0f, 1f))
                    report(down.position.y)
                    drag(down.id) { c ->
                        report(c.position.y)
                        c.consume()
                    }
                }
            },
        contentAlignment = Alignment.Center,
    ) {
        val filled = 60.dp * volume.coerceIn(0f, 1f)
        Box(Modifier.width(2.dp).height(60.dp).background(Color(0xFFE7E7E7)))
        Box(Modifier.align(Alignment.BottomCenter).width(2.dp).height(filled).background(PlayerTheme.accent))
        Box(
            Modifier.align(Alignment.BottomCenter).offset(y = -(filled - 6.dp))
                .size(12.dp).clip(CircleShape).background(PlayerTheme.accent)
        )
    }
}

/** 视频信息（真实读数）+ 后台播放开关 */
@Composable
fun PlayerInfoPanel(
    stats: PlayerStats,
    backgroundPlayEnabled: Boolean,
    onToggleBackgroundPlay: () -> Unit,
    modifier: Modifier = Modifier,
) {
    PlayerPanelShell(modifier = modifier, width = 300.dp) {
        PanelTitle("视频信息")
        listOf(
            "分辨率" to stats.resolutionText(),
            "编解码器" to stats.codecText(),
            "解码方式" to stats.decoderText(),
            "解码器" to stats.decoderNameText(),
            "帧率" to stats.frameRateText(),
            "视频码率" to stats.bitrateText(),
            "丢帧/总帧" to stats.droppedText(),
            "缓冲时长" to stats.bufferText(),
            "播放位置" to stats.positionText(),
            "下载速度（合计）" to stats.speedText(),
        ).forEach { (k, v) ->
            Row(
                modifier = Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 3.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(k, color = PlayerTheme.panelSubText, fontSize = 12.sp)
                Text(
                    text = v,
                    color = PlayerTheme.panelText,
                    fontSize = 12.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(start = 8.dp),
                )
            }
        }

        /* 后台播放开关（**默认打开**：切到后台继续播；关掉则切后台自动暂停、回来自动续播） */
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .clickable { onToggleBackgroundPlay() }
                .padding(horizontal = 20.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = "后台播放",
                color = PlayerTheme.panelText,
                fontSize = 13.sp,
                modifier = Modifier.weight(1f),
            )
            Switch(
                checked = backgroundPlayEnabled,
                onCheckedChange = { onToggleBackgroundPlay() },
            )
        }
    }
}
