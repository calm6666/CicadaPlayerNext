package com.cicada.player.compose

import android.content.pm.ActivityInfo
import android.content.res.Configuration
import android.net.Uri
import android.provider.OpenableColumns
import androidx.activity.ComponentActivity
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.cicada.player.compose.components.video.CicadaVideoPlayer
import com.cicada.player.compose.player.LocalMediaAccess
import com.cicada.player.compose.player.rememberLocalVideoPicker
import com.cicada.player.compose.ui.theme.CicadaPlayerTheme
import com.cicada.player.compose.ui.theme.PlayerTheme
import java.net.HttpURLConnection
import java.net.URL
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext

/**
 * 播放页：**非全屏 / 全屏两套 UI**（照用户给的两张截图）。
 *
 * 非全屏：视频 16:9 在顶部 + 下方信息区（标题 / 在线人数 / 关注 / 视频信息 / 浅色主题 / 关闭播放器）。
 * 全屏：画面铺满，顶部返回+标题+在线人数+关注，底部播放键/弹幕开关/弹幕设置/弹幕输入框/字幕/倍速/自动。
 *
 * 播放内核 = 本工程 `:cicadaplayer`（`com.cicada.player`），**不用 ExoPlayer/Media3**。
 */
enum class PlayerDisplayMode { Inline, PortraitFull, LandscapeFull }

@Composable
fun VideoPlayerScreen(
    onClose: () -> Unit,
    initialUrl: String = "",
    initialMode: PlayerDisplayMode = PlayerDisplayMode.Inline,
    /**
     * 快进预览帧（**App 层**喂进来）：
     * `videoshot/preview.bin` 按 `\u001F` 切成的数组，**每 5 秒一档**，`arr[0]` 是空占位。
     * 空 = 拖动进度条时只显示圆形占位（截图 4）。
     */
    previewFrames: List<String> = emptyList(),
) {
    var source by remember { mutableStateOf(initialUrl) }
    var displayMode by remember { mutableStateOf(initialMode) }
    var isDarkMode by remember { mutableStateOf(true) }
    var infoPanelToggle by remember { mutableIntStateOf(0) }

    val activity = LocalContext.current as? ComponentActivity

    /* 预览帧：外部传进来的优先 */
    var frames by remember { mutableStateOf(previewFrames) }
    LaunchedEffect(previewFrames) {
        if (previewFrames.isNotEmpty()) frames = previewFrames
    }

    /* 调试用：--es previewBin <preview.bin 的 URL> 时，App 层拉下来切成帧数组再喂给播放器 */
    LaunchedEffect(activity) {
        if (previewFrames.isNotEmpty()) return@LaunchedEffect
        val binUrl = activity?.intent?.getStringExtra("previewBin").orEmpty()
        if (binUrl.isBlank()) return@LaunchedEffect
        val loaded = fetchPreviewBin(binUrl)
        if (loaded.isNotEmpty()) frames = loaded
    }

    val view = LocalView.current
    val isLandscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE

    /*
     * 全屏切换时序（大厂做法）：**先请求方向，等系统真的转过去了，再切布局**。
     * 以前是直接 displayMode = LandscapeFull，于是先按"全屏"排一次版、而设备还在竖屏，
     * 看起来就是"先竖屏全屏、再旋转"或者"闪一下"。
     * pendingMode != null = 方向还没到位、布局先别切；下面有一条 700ms 兜底，不会卡住。
     */
    var pendingMode by remember { mutableStateOf<PlayerDisplayMode?>(null) }

    /*
     * 需要变方向时才调 requestedOrientation（多余的一次也会多一次配置变更 → 闪）。
     *
     * **全屏用传感器方向**（系统内部读的就是设备的方向传感器/陀螺仪）：
     *   LandscapeFull → SCREEN_ORIENTATION_SENSOR_LANDSCAPE：**两个横向都认**，
     *                   手机翻 180°（正反横屏）画面跟着翻 —— 以前写死的
     *                   SCREEN_ORIENTATION_LANDSCAPE 只认一个横向，反过来就没反应。
     *   PortraitFull  → SCREEN_ORIENTATION_SENSOR_PORTRAIT：两个竖向都认（含倒竖屏）。
     *   Inline        → 锁竖屏。
     * 传感器方向变化时由系统做旋转过渡动画 —— 和"进/出横屏全屏"走的是同一套动画。
     */
    LaunchedEffect(displayMode, pendingMode) {
        val act = activity ?: return@LaunchedEffect
        val want = pendingMode ?: displayMode
        val target = when (want) {
            PlayerDisplayMode.LandscapeFull -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
            PlayerDisplayMode.PortraitFull -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
            PlayerDisplayMode.Inline -> ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
        }
        if (act.requestedOrientation != target) act.requestedOrientation = target
    }

    /* 方向到位后落地：横屏全屏要等真的横过来；其它模式立即生效 */
    LaunchedEffect(pendingMode, isLandscape) {
        val target = pendingMode ?: return@LaunchedEffect
        if (target != PlayerDisplayMode.LandscapeFull || isLandscape) {
            displayMode = target
            pendingMode = null
        }
    }

    /* 兜底：个别设备方向不上报，也不能一直停在 pending */
    LaunchedEffect(pendingMode) {
        val target = pendingMode ?: return@LaunchedEffect
        delay(700)
        if (pendingMode == target) {
            displayMode = target
            pendingMode = null
        }
    }

    /*
     * 系统栏：**只有横屏全屏才隐藏**（沉浸式，下拉临时出现）；
     * 竖屏全屏和非全屏都显示状态栏，图标用浅色（状态栏那一块是黑的）。
     */
    LaunchedEffect(displayMode) {
        val window = activity?.window ?: return@LaunchedEffect
        val bars = WindowCompat.getInsetsController(window, view)
        if (displayMode == PlayerDisplayMode.LandscapeFull) {
            bars.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            bars.hide(WindowInsetsCompat.Type.systemBars())
        } else {
            bars.show(WindowInsetsCompat.Type.systemBars())
            bars.isAppearanceLightStatusBars = false
        }
    }

    /* 离开播放页（Navigation 里 popBackStack）：恢复竖屏 + 显示系统栏，不然会留在横屏/沉浸式 */
    DisposableEffect(activity) {
        onDispose {
            activity?.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
            activity?.window?.let { window ->
                WindowCompat.getInsetsController(window, view).show(WindowInsetsCompat.Type.systemBars())
            }
        }
    }

    CicadaPlayerTheme(darkTheme = isDarkMode) {
        if (source.isBlank()) {
            SourceEntry(
                onPlay = { source = it },
                onClose = onClose,
                onToggleTheme = { isDarkMode = !isDarkMode },
            )
            return@CicadaPlayerTheme
        }

        val fullscreen = displayMode != PlayerDisplayMode.Inline

        /* 标题 = 文件名（本地文件 / 选择器给的 Uri 取真实文件名，网络地址取路径最后一段） */
        val title = rememberSourceTitle(source)

        /*
         * **全项目只有这一个 CicadaVideoPlayer 调用点**：切全屏只是改 modifier 和 isFullscreen，
         * 不会销毁重建。以前 inline / 全屏各写一处，切换时 SurfaceView 被销毁 + 播放器 release，
         * 触发内核解码器 flush 死循环（
         *   MediaCodecDecoder E dequeueOutputBufferIndex fail null / flush state 1
         * ），表现就是全屏后视频和 Lottie 图标一起卡死。
         */
        Column(
            modifier = Modifier
                .fillMaxSize()
                .background(PlayerTheme.windowBg)
                .then(if (fullscreen) Modifier else Modifier.verticalScroll(rememberScrollState())),
        ) {
            Box(
                modifier = if (fullscreen) {
                    Modifier.fillMaxSize()
                } else {
                    /* 非全屏：状态栏那一块刷黑，播放器在它下面（大厂做法） */
                    Modifier.fillMaxWidth().background(Color.Black).statusBarsPadding()
                },
            ) {
                CicadaVideoPlayer(
                    source = source,
                    isFullscreen = fullscreen,
                    modifier = if (fullscreen) {
                        Modifier.fillMaxSize()
                    } else {
                        Modifier.fillMaxWidth().aspectRatio(16f / 9f)
                    },
                    title = title,
                    watchingText = "35人正在看",
                    /* 全屏 → 退出全屏；非全屏 → 返回上一页（Navigation popBackStack） */
                    onBack = {
                        if (fullscreen) {
                            pendingMode = PlayerDisplayMode.Inline
                        } else {
                            onClose()
                        }
                    },
                    onToggleFullscreen = {
                        pendingMode = if (displayMode == PlayerDisplayMode.LandscapeFull) {
                            PlayerDisplayMode.Inline
                        } else {
                            PlayerDisplayMode.LandscapeFull
                        }
                    },
                    onTogglePortraitFullscreen = {
                        pendingMode = if (displayMode == PlayerDisplayMode.PortraitFull) {
                            PlayerDisplayMode.Inline
                        } else {
                            PlayerDisplayMode.PortraitFull
                        }
                    },
                    onError = { source = "" },
                    infoPanelToggle = infoPanelToggle,
                    previewFrames = frames,
                )
            }

            if (!fullscreen) {
                InfoSection(
                    title = title,
                    onShowInfo = { infoPanelToggle++ },
                    onToggleTheme = { isDarkMode = !isDarkMode },
                    onClose = onClose,
                )
            }
        }
    }
}

/**
 * 解析 `videoshot/preview.bin`：**UTF-8 文本，按 `\u001F` 切成一串 data URL**，每 5 秒一档。
 * `arr[0]` 是空占位，**必须原样保留**（进度条的 `index = floor(ms/5000) + 1` 依赖它）。
 */
private fun parsePreviewBin(text: String): List<String> =
    text.split('\u001F').map { it.trim() }

/** 拉取 `preview.bin`（App 层拉好、切好再喂给播放器） */
private suspend fun fetchPreviewBin(url: String): List<String> = withContext(Dispatchers.IO) {
    runCatching {
        val connection = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 5_000
            readTimeout = 10_000
        }
        connection.inputStream.bufferedReader(Charsets.UTF_8).use { parsePreviewBin(it.readText()) }
    }.getOrDefault(emptyList())
}

/**
 * 播放页标题 = **文件名**。
 *   · `content://`（系统选择器给的）→ 查 OpenableColumns.DISPLAY_NAME；
 *   · 本地路径 / `file://` → 路径最后一段；
 *   · 网络地址 → 路径最后一段（去掉 query）；播放对象（`{` 开头）→ "播放对象"。
 * 取不到就显示"视频"，不再显示任何写死的文案。
 */
@Composable
private fun rememberSourceTitle(source: String): String {
    val context = LocalContext.current
    return remember(source) {
        val fromProvider = runCatching {
            if (source.startsWith("content://")) {
                context.contentResolver
                    .query(Uri.parse(source), arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
                    ?.use { cursor -> if (cursor.moveToFirst()) cursor.getString(0) else null }
            } else {
                null
            }
        }.getOrNull()

        val name = fromProvider
            ?: if (source.startsWith("{")) {
                "播放对象"
            } else {
                runCatching { Uri.parse(source).lastPathSegment }.getOrNull()
            }

        name?.substringBefore('?')?.substringAfterLast('/')?.takeIf { it.isNotBlank() } ?: "视频"
    }
}

/** 非全屏时视频下方的信息区（截图 1 右侧那块"视频信息"，手机上放画面下方） */
@Composable
private fun InfoSection(
    title: String,
    onShowInfo: () -> Unit,
    onToggleTheme: () -> Unit,
    onClose: () -> Unit,
) {
    Column(
        modifier = Modifier.fillMaxWidth().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text(
            text = title,
            color = PlayerTheme.onSurface,
            fontSize = 16.sp,
            fontWeight = FontWeight.Medium,
        )
        Text(text = "35人正在看", color = PlayerTheme.panelSubText, fontSize = 12.sp)

        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                text = "+ 关注",
                color = Color.White,
                fontSize = 13.sp,
                modifier = Modifier
                    .background(PlayerTheme.accent, RoundedCornerShape(16.dp))
                    .padding(horizontal = 16.dp, vertical = 6.dp),
            )
            Spacer(Modifier.width(0.dp))
        }

        Row(verticalAlignment = Alignment.CenterVertically) {
            TextButton(onClick = onShowInfo) {
                Text("视频信息", color = PlayerTheme.accent, fontSize = 13.sp)
            }
            TextButton(onClick = onToggleTheme) {
                Text("浅色主题", color = PlayerTheme.accent, fontSize = 13.sp)
            }
            TextButton(onClick = onClose) {
                Text("关闭播放器", color = PlayerTheme.panelSubText, fontSize = 13.sp)
            }
        }
    }
}

/** 片源输入页：普通地址（DASH/HLS/FLV/MP4）、**播放对象**（JSON 清单）、**本地视频** */
@Composable
private fun SourceEntry(
    onPlay: (String) -> Unit,
    onClose: () -> Unit,
    onToggleTheme: () -> Unit,
) {
    val context = LocalContext.current
    var text by remember { mutableStateOf("") }
    var hint by remember { mutableStateOf<String?>(null) }
    var denied by remember { mutableStateOf(false) }

    /*
     * 本地视频：**先申请读取权限，再打开系统选择器**（不硬读文件）。
     * 选中的 `content://` 直接交给内核 —— 内核自己带 ContentDataSource
     * （framework/data_source/ContentDataSource，probe 匹配 "content://"），
     * App 不需要先拷贝文件。
     */
    val picker = rememberLocalVideoPicker(
        onPicked = { uri ->
            val value = uri.toString()
            hint = null
            denied = false
            text = value
            onPlay(value)
        },
        onDenied = {
            denied = true
            hint = LocalMediaAccess.DENIED_HINT
        },
    )

    /** 点"开始播放"：网络地址直接播；本地片源先拿权限、再确认真的读得到，绝不硬读 */
    fun submit() {
        val value = text.trim()
        if (value.isEmpty()) return
        if (!LocalMediaAccess.isLocalSource(value)) {
            hint = null
            onPlay(value)
            return
        }
        picker.ensurePermissionThen {
            if (LocalMediaAccess.canRead(context, value)) {
                hint = null
                onPlay(value)
            } else {
                hint = "读不到这个本地文件（$value）：可能已删除，或系统不允许读这个目录。"
            }
        }
    }

    Box(
        modifier = Modifier.fillMaxSize().background(PlayerTheme.windowBg),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            modifier = Modifier.fillMaxWidth().padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text(
                text = "CicadaPlayer 播放器",
                color = PlayerTheme.onSurface,
                fontSize = 22.sp,
                fontWeight = FontWeight.Bold,
            )
            Text(
                text = "支持 DASH / HLS / FLV / MP4、" +
                    "**播放对象**（MediaManifest JSON）与**本地视频**（DASH/HLS/FLV 全部由内核对接）",
                color = PlayerTheme.panelSubText,
                fontSize = 12.sp,
            )

            OutlinedTextField(
                value = text,
                onValueChange = { text = it },
                placeholder = {
                    Text("粘贴视频地址或 MediaManifest JSON…", color = PlayerTheme.panelSubText)
                },
                modifier = Modifier.fillMaxWidth().height(120.dp),
                shape = RoundedCornerShape(12.dp),
                colors = OutlinedTextFieldDefaults.colors(
                    focusedTextColor = PlayerTheme.onSurface,
                    unfocusedTextColor = PlayerTheme.onSurface,
                    focusedBorderColor = PlayerTheme.accent,
                    unfocusedBorderColor = PlayerTheme.surfaceBorder,
                    focusedContainerColor = PlayerTheme.surface,
                    unfocusedContainerColor = PlayerTheme.surface,
                ),
            )

            Button(
                onClick = { submit() },
                modifier = Modifier.fillMaxWidth().height(48.dp),
                shape = RoundedCornerShape(24.dp),
                colors = ButtonDefaults.buttonColors(
                    containerColor = PlayerTheme.accent,
                    contentColor = Color.White,
                ),
                enabled = text.isNotBlank(),
            ) { Text("开始播放", fontSize = 15.sp) }

            /* 本地视频：申请权限 → 系统选择器 → 选中即播（零拷贝硬解） */
            Button(
                onClick = {
                    hint = null
                    picker.pickVideo()
                },
                modifier = Modifier.fillMaxWidth().height(48.dp),
                shape = RoundedCornerShape(24.dp),
                colors = ButtonDefaults.buttonColors(
                    containerColor = PlayerTheme.surface,
                    contentColor = PlayerTheme.onSurface,
                ),
            ) { Text("选择本地视频", fontSize = 15.sp) }

            hint?.let {
                Text(text = it, color = Color(0xFFFF7F9F), fontSize = 12.sp)
            }

            /* 权限被拒（尤其是"拒绝且不再询问"）：给出系统设置入口，不假装能播 */
            if (denied) {
                TextButton(onClick = { LocalMediaAccess.openAppSettings(context) }) {
                    Text("去系统设置授权", color = PlayerTheme.accent, fontSize = 13.sp)
                }
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                TextButton(onClick = onToggleTheme) {
                    Text("切换深浅主题", color = PlayerTheme.accent, fontSize = 13.sp)
                }
                TextButton(onClick = onClose) {
                    Text("返回首页", color = PlayerTheme.panelSubText, fontSize = 13.sp)
                }
            }
        }
    }
}
