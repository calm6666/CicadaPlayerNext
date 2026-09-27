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

/**
 * 一行面板条目（所有面板共用）。
 *
 * [badge] 是行尾那个**小圆角徽标**的文本（当前只有清晰度面板用它显示编码，例如 "H.265"）：
 *   * 非空 → 在文字右边的行尾画一个小标签（半透明白底 + 次要文字色，风格跟面板一致）；
 *   * 空串 / null（默认）→ **整个徽标不渲染**，这一行的排版和以前完全一样。
 * 默认值让既有调用方（倍速/字幕/镜像/色觉…）一行都不用改。
 */
@Composable
fun PanelRow(
    text: String,
    selected: Boolean,
    badge: String? = null,
    onClick: () -> Unit,
) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(horizontal = 20.dp, vertical = 4.dp),
    ) {
        Text(
            text = text,
            color = if (selected) PlayerTheme.accent else PlayerTheme.panelText,
            fontSize = 13.sp,
            fontWeight = if (selected) FontWeight.Medium else FontWeight.Normal,
            maxLines = 1,
            /* fill = false：文字只占自己那么宽，剩下的空间留给行尾的徽标 */
            modifier = Modifier.weight(1f, fill = false),
        )

        if (!badge.isNullOrEmpty()) {
            Box(
                modifier = Modifier
                    .padding(start = 6.dp)
                    .clip(RoundedCornerShape(3.dp))
                    .background(Color.White.copy(alpha = 0.12f))
                    .padding(horizontal = 4.dp, vertical = 1.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    text = badge,
                    color = PlayerTheme.panelSubText,
                    fontSize = 9.sp,
                    maxLines = 1,
                )
            }
        }
    }
}

/**
 * 镜像画面（**内核 `setMirrorMode`**，不是 App 层翻转）。
 *
 * 三态：关 / 水平 / 垂直（用户明确：只做镜像翻转，不做旋转）。
 *
 * 【为什么改了名字 / 回退点 M2】内核 SDK 自带 `com.cicada.player.CicadaPlayer.MirrorMode`
 * （`MIRROR_MODE_NONE / MIRROR_MODE_HORIZONTAL / MIRROR_MODE_VERTICAL`，见 CicadaPlayer.java:345-366）。
 * 本枚举原来就叫 `MirrorMode`，与它同名容易在调用处看错类型（一个是 UI 三态、一个是内核参数），
 * 所以这里**改名成 `MirrorUiMode`**：面板参数/状态继续用它，下发内核时在
 * CicadaVideoPlayer 里显式映射一次（见 `LaunchedEffect(mirrorMode)`），两个类型各自只出现一次，
 * 不用在 UI 层 import 内核枚举。回退：把本枚举与下面 MIRROR_ROWS、PlayerInfoPanel 两个参数
 * 的名字改回 `MirrorMode`，并删掉 CicadaVideoPlayer 里的映射 + setMirrorMode 调用。
 *
 * 【为什么走内核（与上一轮相反的结论）】上一轮用 `Modifier.graphicsLayer { scaleX = -1f }`
 * 挂在承载画面的 AndroidView 上，真机点击后画面毫无变化：视频画在 **SurfaceView** 上，由
 * SurfaceFlinger 单独合成，Compose 图层的负缩放传不到它。内核这条路（JNI →
 * `SMPMessageControllerListener` 对当前渲染器 `setFlip` → `GLRender` → OES/YUV 两个 program
 * context）是已验证可用的。
 *
 * 【已知限制（不解决）】隧道 / direct 渲染（HDR、Widevine 被内核强制走 `FLAG_DUMMY`）下渲染器是
 * `DummyVideoRender`，它的 `setFlip` 是**空实现** ⇒ 那类片源镜像仍无效；**不要**再用 App 层
 * View 变换去兜。截图不带镜像（内核抓帧处对自身的 flip 做临时补偿）。
 */
enum class MirrorUiMode { None, Horizontal, Vertical }

/** 「镜像画面」那三行的文案与顺序（与 [MirrorUiMode] 一一对应；三态文案保持不变） */
private val MIRROR_ROWS: List<Pair<MirrorUiMode, String>> = listOf(
    MirrorUiMode.None to "关闭",
    MirrorUiMode.Horizontal to "水平镜像",
    MirrorUiMode.Vertical to "垂直镜像",
)

/**
 * 【色觉辅助滤镜 / 回退点 K2】色觉辅助（红绿色盲等）4 档：关闭 / 红·绿 / 绿·红 / 蓝·黄。
 *
 * 与镜像那三行的 `MirrorUiMode` **不是一回事**：镜像走内核渲染器的 flip（隧道渲染那条无效），
 * 而色觉滤镜必须在**内核的 GL 着色器**里乘一个 3x3 矩阵（`CicadaPlayerController.setColorMatrix`
 * → JNI → `SuperMediaPlayer` → `GLRender` → `OESProgramContext`/`YUVProgramContext`）。
 *
 * 矩阵数值：Machado, Oliveira & Fernandes (2009) 的色盲**模拟**矩阵，强度 1.0，**行主序 3×3**。
 * 这三组是"正常三色觉 → 某型色盲所见"的正向矩阵；用在色盲用户屏幕上时，它把本来容易混淆的
 * 颜色拉开成他们能分辨的方向（业界常用的 daltonization 近似做法之一）。
 * 数值照用户给定的三组原样抄写，未做任何调整；实际观感必须在真机上核对。
 *
 * 【关闭】用**单位矩阵**表示（不用 null）：C++ 侧默认值也是单位矩阵，"乘上去等于没乘"
 * 让"关滤镜"和"滤镜没设置过"走完全同一条路径，少一个分支。**注意别传全 0 矩阵**，
 * 那在着色器里的语义是 clamp(0,0,1) = 画面全黑，不是关闭。
 */
enum class ColorVisionMode(val label: String, val matrix: FloatArray) {
    None(
        "关闭",
        floatArrayOf(
            1f, 0f, 0f,
            0f, 1f, 0f,
            0f, 0f, 1f,
        ),
    ),
    Protan(
        "红/绿（红色盲）",
        floatArrayOf(
            0.152286f, 1.052583f, -0.204868f,
            0.114503f, 0.786281f, 0.099216f,
            -0.003882f, -0.048116f, 1.051998f,
        ),
    ),
    Deutan(
        "绿/红（绿色盲）",
        floatArrayOf(
            0.367322f, 0.860646f, -0.227968f,
            0.280085f, 0.672501f, 0.047413f,
            -0.011820f, 0.042940f, 0.968881f,
        ),
    ),
    Tritan(
        "蓝/黄（蓝色盲）",
        floatArrayOf(
            1.255528f, -0.076749f, -0.178779f,
            -0.078411f, 0.930809f, 0.147602f,
            0.004733f, 0.691367f, 0.303900f,
        ),
    ),
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
        /*
         * 【回退点 A3】这里原来是 `PanelRow("自动（当前分辨率）", selected = true) { }`
         * —— 一个既不可点、也不来自内核的**假条目**。已删除：列表为空时如实说明。
         *
         * 正常路径下这一支根本走不到：调用方只在列表非空时才允许打开本面板
         * （CicadaVideoPlayer 的 onQualityClick，对齐 Qt RightControls.qml:1671-1680
         * 的 `hasQuality = qualities.length > 0` / `if (qualityButton.hasQuality) open()`）。
         * 留着这一支是为了 debug 入口（--es panel quality）和其它调用方不出现空壳面板。
         */
        if (qualities.isEmpty()) {
            Text(
                text = "当前片源没有清晰度信息",
                color = PlayerTheme.panelSubText,
                fontSize = 12.sp,
                modifier = Modifier.padding(horizontal = 20.dp, vertical = 4.dp),
            )
        } else {
            /*
             * 每一档一行，行尾带一个**编码徽标**（`QualityOption.codec`，来自内核
             * `TrackInfo.getVideoCodec()`）：同分辨率不同编码现在是两行（buildQualities 按
             * "分辨率 + 编码"分组），徽标就是用来区分它们的（`1080P [H.264]` / `1080P [H.265]`）。
             * codec 为空串时 PanelRow 不渲染徽标 —— 内核认不出编码就不显示，不猜。
             */
            qualities.forEach { q -> PanelRow(q.label, q.selected, q.codec) { onPick(q) } }
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

/**
 * 视频信息（真实读数）+ 后台播放开关 + **画面设置（镜像 / 截图）**。
 *
 * 【回退点】`mirror / onMirrorChange / captureInFlight / onCapture` 这四个参数与下面
 * 「镜像画面」「截图」两段是**本轮新增**的：整段删掉 + 调用方（CicadaVideoPlayer 的
 * PlayerPanel.Info 那一支）去掉这四个实参，即可回到只有信息+后台播放的旧状态。
 *
 * 为什么这两行放在**这个已有面板**里，而不是控制栏再加一颗按钮：
 * 控制栏那一行在竖屏下已经排满（清晰度/倍速/字幕三颗文字按钮 + 竖屏胶囊 + 两颗图标，
 * 见 PlayerControls.kt 里那段宽度说明），再塞一颗会把弹幕输入框挤到不可用。
 * 位置对应 Qt 的设置面板（platform/QtPlayer/SettingsPanel.qml:389-394「镜像画面」）。
 */
@Composable
fun PlayerInfoPanel(
    stats: PlayerStats,
    backgroundPlayEnabled: Boolean,
    onToggleBackgroundPlay: () -> Unit,
    mirror: MirrorUiMode = MirrorUiMode.None,
    onMirrorChange: (MirrorUiMode) -> Unit = {},
    /* 【色觉辅助滤镜 / 回退点 K3】这两个参数与下面「色觉辅助」一段是本轮新增的：
     * 整段删掉 + 调用方（CicadaVideoPlayer 的 PlayerPanel.Info 那一支）去掉这两个实参，
     * 即可回到只有镜像/截图的状态。*/
    colorVision: ColorVisionMode = ColorVisionMode.None,
    onColorVisionChange: (ColorVisionMode) -> Unit = {},
    captureInFlight: Boolean = false,
    onCapture: () -> Unit = {},
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

        /* ---------------- 镜像画面（本轮修复：改走内核 setMirrorMode） ----------------
         * 三态互斥、当前档用主题色高亮 —— 与面板其它选中项同一套写法（PanelRow 的 selected）。
         * 这里只改状态；**下发内核**在 CicadaVideoPlayer 的 LaunchedEffect(mirrorMode) 里做
         * （与色觉滤镜同一种写法）。改完立刻生效：不重设 Surface、不碰解码/播放状态。 */
        PanelTitle("镜像画面")
        MIRROR_ROWS.forEach { (mode, label) ->
            PanelRow(label, mirror == mode) { onMirrorChange(mode) }
        }

        /* ---------------- 色觉辅助（本轮新增；走**内核 GL 着色器**，与镜像不是一条路） ----------------
         * 4 档互斥、当前档用主题色高亮（同一套 PanelRow(selected) 写法）。改完立刻生效。
         * 在哪无效、截图为什么不带滤镜：见 ColorVisionMode 和 CicadaPlayerController.setColorMatrix。 */
        PanelTitle("色觉辅助")
        ColorVisionMode.entries.forEach { mode ->
            PanelRow(mode.label, colorVision == mode) { onColorVisionChange(mode) }
        }

        /* ---------------- 截图 → 存相册（本轮新增） ----------------
         * 点击 = 请求内核截当前帧，成功后由 CicadaVideoPlayer 写进相册（Pictures/CicadaPlayer）
         * 并给一句 Toast。`captureInFlight` 只用于把文案换成"正在截图…"，防止连点。 */
        PanelTitle("截图")
        PanelRow(
            text = if (captureInFlight) "正在截图…" else "保存当前画面到相册",
            selected = false,
        ) { onCapture() }
    }
}
