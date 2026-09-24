package com.cicada.player.compose.components.video

import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.compose.ui.viewinterop.AndroidView
import com.cicada.player.CicadaPlayer
import com.cicada.player.nativeclass.TrackInfo
import com.cicada.player.compose.components.danmaku.Danmaku
import com.cicada.player.compose.components.danmaku.DanmakuConfig
import com.cicada.player.compose.components.danmaku.DanmakuType
import com.cicada.player.compose.components.danmaku.DanmakuView
import com.cicada.player.compose.player.CicadaPlayerController
import com.cicada.player.compose.ui.theme.PlayerTheme
import kotlinx.coroutines.delay

/** 同时只开一个浮层面板（和 Qt 的 openExclusivePanel 一致）；**没有 Color** */
private enum class PlayerPanel { None, Quality, Speed, Subtitle, Danmaku, Info, Volume }

/**
 * 播放器（UI 照用户给的四张截图；渲染走**本工程自己的内核** `:cicadaplayer`）。
 *
 * 交互（用户明确）：
 *   * **双击 = 播放/暂停**（手机端不是单击暂停）；
 *   * **单击 = 显示/隐藏 顶部栏 + 控制栏**，再点隐藏；
 *   * 不操作 **3 秒自动隐藏**控制栏；
 *   * **控制栏隐藏后，底部那条细进度条仍然常驻**。
 *
 * 画面：`SurfaceView` + `controller.setSurface(...)` —— 内核渲染器**直写屏幕（零拷贝）**，
 * **绝不换 TextureView**。
 */
@Composable
fun CicadaVideoPlayer(
    source: String,
    isFullscreen: Boolean,
    modifier: Modifier = Modifier,
    title: String = "",
    watchingText: String? = null,
    onBack: () -> Unit = {},
    onToggleFullscreen: () -> Unit = {},
    onTogglePortraitFullscreen: () -> Unit = {},
    onDanmakuSend: (String) -> Unit = {},
    onError: () -> Unit = {},
    infoPanelToggle: Int = 0,
    /**
     * 快进预览帧（App 层喂进来：`videoshot/preview.bin` 按 `\u001F` 切出的数组，
     * 每 5 秒一档、`arr[0]` 是空占位）。空 = 拖动时只显示圆形占位（截图 4）。
     */
    previewFrames: List<String> = emptyList(),
) {
    val context = LocalContext.current
    val controller = remember { CicadaPlayerController(context) }

    var showControls by remember { mutableStateOf(true) }
    var interactionTick by remember { mutableIntStateOf(0) }
    var openPanel by remember { mutableStateOf(PlayerPanel.None) }

    /* 弹幕输入框是否聚焦（键盘抬起期间控制栏不自动隐藏，键盘收起后再等 3 秒） */
    var danmakuInputFocused by remember { mutableStateOf(false) }

    /*
     * 系统返回键 / 返回手势：
     *   有面板开着 → 先关面板；全屏 → 退出全屏；否则交给系统（退出页面）。
     */
    BackHandler(enabled = isFullscreen || openPanel != PlayerPanel.None) {
        if (openPanel != PlayerPanel.None) {
            openPanel = PlayerPanel.None
        } else {
            onBack()
        }
    }

    /*
     * **后台播放开关（默认打开）**：
     *   开 → 切到后台继续播（音频不断，回前台画面接上）；
     *   关 → 切后台自动暂停，回前台自动续播（只续播"因为切后台才停的"）。
     * 面板里的开关见 PlayerInfoPanel（"后台播放"那一行）。
     */
    var backgroundPlayEnabled by remember { mutableStateOf(true) }
    var pausedByBackground by remember { mutableStateOf(false) }

    val lifecycleOwner = LocalLifecycleOwner.current
    DisposableEffect(lifecycleOwner, backgroundPlayEnabled) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_STOP -> {
                    if (!backgroundPlayEnabled && controller.isPlaying) {
                        controller.pause()
                        pausedByBackground = true
                    }
                }

                Lifecycle.Event.ON_START -> {
                    if (pausedByBackground) {
                        pausedByBackground = false
                        controller.start()
                    }
                }

                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    /* 弹幕 */
    val danmakus = remember { mutableStateListOf<Danmaku>() }
    val danmakuConfig = remember { DanmakuConfig() }
    var danmakuEnabled by remember { mutableStateOf(true) }
    var danmakuOpacity by remember { mutableIntStateOf(100) }
    var danmakuFontScale by remember { mutableIntStateOf(100) }
    var danmakuDensity by remember { mutableIntStateOf(8) }

    /* 音量 / 倍速（下发给内核 setVolume / setMute / setSpeed） */
    var volume by remember { mutableFloatStateOf(0.9f) }
    var muted by remember { mutableStateOf(false) }
    var speed by remember { mutableStateOf(1f) }

    /* 调试开关：--ez demoDanmaku true 灌示例弹幕；--es panel xxx 启动即开面板 */
    val activity = context as? android.app.Activity
    val demoDanmaku = activity?.intent?.getBooleanExtra("demoDanmaku", false) == true
    val debugPanel = activity?.intent?.getStringExtra("panel").orEmpty()

    LaunchedEffect(demoDanmaku) { if (demoDanmaku) seedDemoDanmaku(danmakus) }

    LaunchedEffect(debugPanel) {
        openPanel = when (debugPanel.lowercase()) {
            "quality" -> PlayerPanel.Quality
            "speed" -> PlayerPanel.Speed
            "subtitle" -> PlayerPanel.Subtitle
            "danmaku" -> PlayerPanel.Danmaku
            "volume" -> PlayerPanel.Volume
            "info" -> PlayerPanel.Info
            else -> PlayerPanel.None
        }
    }

    LaunchedEffect(infoPanelToggle) {
        if (infoPanelToggle > 0) {
            openPanel = if (openPanel == PlayerPanel.Info) PlayerPanel.None else PlayerPanel.Info
            showControls = true
        }
    }

    /* 弹幕设置 → DanmakuConfig（GPU 渲染层直接读，改完立刻生效） */
    LaunchedEffect(danmakuEnabled, danmakuOpacity, danmakuFontScale, danmakuDensity) {
        danmakuConfig.isEnabled = danmakuEnabled
        danmakuConfig.opacity = danmakuOpacity / 100f
        danmakuConfig.textSize = 32f * danmakuFontScale / 100f
        danmakuConfig.maxLines = danmakuDensity
    }

    /* 片源：地址或播放对象，全部交给内核（DASH/HLS/FLV/清单对象） */
    LaunchedEffect(source, controller) {
        if (source.isBlank()) return@LaunchedEffect
        controller.enableHardwareDecoder(true)
        controller.setScaleMode(CicadaPlayer.ScaleMode.SCALE_ASPECT_FIT)
        controller.setVideoBackgroundColor(android.graphics.Color.BLACK)
        controller.playAny(source)
    }

    DisposableEffect(controller) {
        onDispose { controller.release() }
    }

    /* 控制栏自动隐藏：3 秒（开面板 / 弹幕输入框聚焦（键盘抬起）时不隐藏） */
    LaunchedEffect(showControls, controller.isPlaying, interactionTick, openPanel, danmakuInputFocused) {
        if (showControls && controller.isPlaying && openPanel == PlayerPanel.None && !danmakuInputFocused) {
            delay(3000)
            showControls = false
        }
    }

    Box(modifier = modifier.fillMaxSize().background(PlayerTheme.videoBg)) {
        /* ---------------- 画面：SurfaceView（零拷贝直通内核渲染器） ---------------- */
        AndroidView(
            factory = { ctx ->
                SurfaceView(ctx).apply {
                    holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(holder: SurfaceHolder) =
                            controller.setSurface(holder.surface)

                        override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, hh: Int) =
                            controller.setSurface(h.surface)

                        /* 内核要求：销毁时必须置 null */
                        override fun surfaceDestroyed(holder: SurfaceHolder) =
                            controller.setSurface(null)
                    })
                    keepScreenOn = true
                }
            },
            modifier = Modifier
                .fillMaxSize()
                .pointerInput(openPanel) {
                    detectTapGestures(
                        /* 双击 = 播放/暂停 */
                        onDoubleTap = { controller.togglePlay() },
                        /* 单击 = 显隐 top 栏 + 控制栏；开面板时先关面板 */
                        onTap = {
                            if (openPanel != PlayerPanel.None) {
                                openPanel = PlayerPanel.None
                            } else {
                                showControls = !showControls
                            }
                            interactionTick++
                        },
                    )
                },
        )

        /* ---------------- 弹幕层（GPU 渲染，不占 CPU） ---------------- */
        DanmakuView(
            danmakus = danmakus,
            config = danmakuConfig,
            isPlaying = controller.isPlaying,
            currentTime = controller.positionMs,
            modifier = Modifier.fillMaxSize(),
        )

        /* ---------------- 加载 / 错误 ---------------- */
        if (controller.loading && !controller.firstFrameRendered) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                CircularProgressIndicator(
                    modifier = Modifier.size(44.dp),
                    color = PlayerTheme.accent,
                    strokeWidth = 3.dp,
                )
            }
        }

        controller.errorMessage?.let { msg ->
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Column(
                    modifier = Modifier
                        .background(PlayerTheme.panelBg, RoundedCornerShape(8.dp))
                        .padding(20.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    Text("播放出错", color = PlayerTheme.panelText, fontSize = 16.sp)
                    Spacer(Modifier.height(6.dp))
                    Text(msg, color = PlayerTheme.panelSubText, fontSize = 13.sp)
                    Spacer(Modifier.height(12.dp))
                    Button(
                        onClick = { onError() },
                        colors = ButtonDefaults.buttonColors(
                            containerColor = PlayerTheme.accent,
                            contentColor = Color.White,
                        ),
                    ) { Text("重新输入链接") }
                }
            }
        }

        /* ---------------- 控制层（单击显隐；3 秒自动隐藏） ---------------- */
        AnimatedVisibility(
            visible = showControls && controller.errorMessage == null,
            enter = fadeIn(),
            exit = fadeOut(),
            modifier = Modifier.fillMaxSize(),
        ) {
            Box(Modifier.fillMaxSize()) {
                if (isFullscreen) {
                    PlayerTopBar(
                        title = title,
                        watchingText = watchingText,
                        onBack = onBack,
                        onTitleClick = {
                            openPanel = if (openPanel == PlayerPanel.Info) PlayerPanel.None else PlayerPanel.Info
                            interactionTick++
                        },
                        modifier = Modifier
                            .align(Alignment.TopCenter)
                            .background(
                                Brush.verticalGradient(
                                    listOf(Color.Black.copy(alpha = 0.55f), Color.Transparent)
                                )
                            ),
                    )
                }

                Column(modifier = Modifier.align(Alignment.BottomCenter).fillMaxWidth()) {
                    /* 浮层面板：贴在控制条上方、右对齐 */
                    Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.CenterEnd) {
                        Box(Modifier.padding(end = 12.dp, bottom = 4.dp)) {
                            when (openPanel) {
                                PlayerPanel.Quality -> PlayerQualityPanel(
                                    /* 列表来自内核 MediaInfo 的视频轨（TrackInfo）；自动 = AUTO_SELECT_INDEX(-1) */
                                    qualities = buildQualities(controller.mediaInfo, controller.currentVideoIndex),
                                    onPick = { q ->
                                        controller.selectQuality(q.index)
                                        openPanel = PlayerPanel.None
                                    },
                                )

                                PlayerPanel.Speed -> PlayerSpeedPanel(
                                    current = speed,
                                    onPick = { s ->
                                        speed = s
                                        controller.setSpeed(s)
                                        openPanel = PlayerPanel.None
                                    },
                                )

                                PlayerPanel.Subtitle -> PlayerSubtitlePanel(
                                    /* 列表来自内核 MediaInfo 的字幕轨；第一行「关闭字幕」= AUTO_SELECT_INDEX */
                                    tracks = buildSubtitles(
                                        controller.mediaInfo,
                                        controller.currentSubtitleIndex,
                                        controller.subtitlesEnabled,
                                    ),
                                    onPick = { o ->
                                        if (o.index == TrackInfo.AUTO_SELECT_INDEX) {
                                            controller.closeSubtitle()
                                        } else {
                                            controller.selectSubtitle(o.index)
                                        }
                                        openPanel = PlayerPanel.None
                                    },
                                )

                                PlayerPanel.Danmaku -> PlayerDanmakuPanel(
                                    enabled = danmakuEnabled,
                                    opacity = danmakuOpacity,
                                    fontScale = danmakuFontScale,
                                    density = danmakuDensity,
                                    showScroll = danmakuConfig.showScroll,
                                    showTop = danmakuConfig.showTop,
                                    showBottom = danmakuConfig.showBottom,
                                    onToggleEnabled = { danmakuEnabled = !danmakuEnabled },
                                    onOpacity = { danmakuOpacity = it },
                                    onFontScale = { danmakuFontScale = it },
                                    onDensity = { danmakuDensity = it },
                                    onToggleScroll = { danmakuConfig.showScroll = !danmakuConfig.showScroll },
                                    onToggleTop = { danmakuConfig.showTop = !danmakuConfig.showTop },
                                    onToggleBottom = { danmakuConfig.showBottom = !danmakuConfig.showBottom },
                                )

                                PlayerPanel.Volume -> PlayerVolumePanel(
                                    volume = volume,
                                    muted = muted,
                                    onVolume = { v ->
                                        volume = v
                                        muted = false
                                        controller.setMute(false)
                                        controller.setVolume(v)
                                    },
                                    onToggleMute = {
                                        muted = !muted
                                        controller.setMute(muted)
                                    },
                                )

                                PlayerPanel.Info -> PlayerInfoPanel(
                                    stats = PlayerStats.from(controller),
                                    backgroundPlayEnabled = backgroundPlayEnabled,
                                    onToggleBackgroundPlay = { backgroundPlayEnabled = !backgroundPlayEnabled },
                                )

                                else -> Unit
                            }
                        }
                    }

                    Box(
                        Modifier
                            .fillMaxWidth()
                            .background(
                                Brush.verticalGradient(
                                    listOf(PlayerTheme.colors.controlMaskTop, PlayerTheme.colors.controlMaskBottom)
                                )
                            )
                    ) {
                        PlayerBottomBar(
                            isPlaying = controller.isPlaying,
                            positionMs = controller.positionMs,
                            durationMs = controller.durationMs,
                            bufferedMs = controller.bufferedMs,
                            isFullscreen = isFullscreen,
                            danmakuEnabled = danmakuEnabled,
                            onPlayPause = { controller.togglePlay() },
                            onSeek = { controller.seekTo(it) },
                            onSeekStart = { controller.beginPreview(controller.positionMs) },
                            onSeekEnd = { controller.endPreview(); interactionTick++ },
                            onTogglePortraitFullscreen = onTogglePortraitFullscreen,
                            onToggleFullscreen = onToggleFullscreen,
                            onToggleDanmaku = { danmakuEnabled = !danmakuEnabled },
                            onDanmakuSettings = { openPanel = PlayerPanel.Danmaku },
                            onDanmakuSend = { text ->
                                danmakus.add(
                                    Danmaku(
                                        text = text,
                                        time = controller.positionMs,
                                        type = DanmakuType.SCROLL,
                                    )
                                )
                                onDanmakuSend(text)
                            },
                            onSubtitleClick = { openPanel = PlayerPanel.Subtitle },
                            onSpeedClick = { openPanel = PlayerPanel.Speed },
                            onQualityClick = { openPanel = PlayerPanel.Quality },
                            onInteraction = { interactionTick++ },
                            onDanmakuInputFocusChanged = { danmakuInputFocused = it },
                            qualityLabel = if (controller.videoHeight > 0) "${controller.videoHeight}P" else "自动",
                            speedLabel = if (speed == 1f) "倍速" else "${speed}x",
                            previewFrames = previewFrames,
                        )
                    }
                }
            }
        }

        /* ---------------- 非全屏：左上角返回按钮（全屏时的返回在顶部栏里） ---------------- */
        if (!isFullscreen) {
            IconButtonBox(
                onClick = onBack,
                modifier = Modifier
                    .align(Alignment.TopStart)
                    .padding(start = 2.dp, top = 2.dp),
            ) {
                Icon(
                    imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                    contentDescription = "返回",
                    tint = PlayerTheme.controlTextActive,
                    modifier = Modifier.size(22.dp),
                )
            }
        }

        /* ---------------- 常驻细进度条（**控制栏隐藏后才显示**，贴最底部、通宽、不带 dot） ---------------- */
        if (!showControls) {
            PlayerThinProgressBar(
                positionMs = controller.positionMs,
                durationMs = controller.durationMs,
                bufferedMs = controller.bufferedMs,
                modifier = Modifier.align(Alignment.BottomCenter),
            )
        }
    }
}

/**
 * 灌示例弹幕（调试开关）：`--ez demoDanmaku true`。
 * 90 条铺在 0~81 秒：滚动 70% / 顶部 15% / 底部 15%（专门掺同一时刻的顶+底，验证固定弹幕跨类型占位）。
 */
private fun seedDemoDanmaku(target: MutableList<Danmaku>) {
    val palette = listOf(Color.White, Color(0xFFFFD700), Color(0xFF00FFFF), Color(0xFFD44E7D), Color(0xFF7CFC00))
    val texts = listOf(
        "前方高能", "awsl", "这个转场绝了", "第一次看，好家伙", "弹幕护体",
        "GPU 渲染测试", "轨道避让测试", "固定弹幕不重叠", "666", "一键三连",
        "画质真不错", "音画同步吗", "2333", "再来亿遍", "笑死我了",
    )
    var t = 0f
    for (i in 0 until 90) {
        val type = when {
            i % 10 == 3 -> DanmakuType.TOP
            i % 10 == 7 -> DanmakuType.BOTTOM
            else -> DanmakuType.SCROLL
        }
        target.add(
            Danmaku(
                id = "demo-$i",
                text = texts[i % texts.size] + " #" + (i + 1),
                color = palette[i % palette.size],
                textSize = 32f,
                type = type,
                time = t.toLong(),
            )
        )
        t += 900f
    }
}
