package com.cicada.player.compose

import android.content.pm.ActivityInfo
import android.content.res.Configuration
import android.net.Uri
import android.os.Build
import android.provider.OpenableColumns
import android.view.WindowManager
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
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
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
import androidx.lifecycle.viewmodel.compose.viewModel
import com.cicada.player.compose.components.video.CicadaVideoPlayer
import com.cicada.player.compose.player.LocalMediaAccess
import com.cicada.player.compose.player.PlayerSettings
import com.cicada.player.compose.player.PlayerSettingsViewModel
import com.cicada.player.compose.player.rememberLocalVideoPicker
import com.cicada.player.compose.ui.theme.CicadaPlayerTheme
import com.cicada.player.compose.ui.theme.PlayerTheme
import java.net.HttpURLConnection
import java.net.URL
import kotlinx.coroutines.Dispatchers
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

    /*
     * ============ 硬解开关（**软解验证用**）============
     *
     * 关闭后：起播前下发 `enableHardwareDecoder(false)` → 内核 `SetUpVideoPath()` 里
     * `bEnableHwVideoDecode=0 ⇒ bHW=0` ⇒ 解码走**软解**；同时
     * `if (!bEnableHwVideoDecode || !bHW) tunnelRender = false` ⇒ 渲染自动落 **GL**
     * （软解出的是 CPU 帧，走不了 tunnel 直出）。
     *
     * 用途：同一台设备、同一个片源，对比"硬解 / 软解"下 seek、切清晰度、自动切档(ABR) 的行为。
     * 生效时机：只在**进入播放前**（下面片源 Effect 的 key 不含它）—— 播放中不偷偷换解码器；
     * 想切换就先退回本页再起播。
     *
     * 【本轮修复：为什么不再用 `remember`（用户报的"关闭后返回又打开"就是这一条）】
     * 原来这里是 `var hardwareDecodeEnabled by remember { mutableStateOf(true) }`：
     * `remember` 只活在**这一次组合**里。播放页是 Navigation 的一个目的地
     * （`MainActivity.kt:63-65`），`onClose` 走 `popBackStack()` 时这个 composable
     * 会被移出组合、状态一并销毁，下次进来重新 `remember` ⇒ 又回到默认 `true`（硬解开）。
     * 于是"我明明关了硬解"在下一次播放里不成立 —— 这既是现象 2，也是现象 1 的一半。
     *
     * 现在改成 ViewModel + SharedPreferences（见 `player/PlayerSettings.kt` 与
     * `player/PlayerSettingsViewModel.kt`）：**跨界面 + 跨进程**都还在。
     * 为什么不用 `rememberSaveable`：它只扛得住同一任务内的重建，扛不住"退出页面/杀进程"，
     * 而这里需要的恰恰是后者（详见 PlayerSettings 的类注释）。
     *
     * 【回退点 P1】把下面这行换回
     * `var hardwareDecodeEnabled by remember { mutableStateOf(true) }`
     * 并删掉 `settingsViewModel` / `settings` 的引用（SourceEntry 的两个实参也要改回去）。
     */
    val settingsViewModel: PlayerSettingsViewModel = viewModel()
    val settings: PlayerSettings = settingsViewModel.settings
    val hardwareDecodeEnabled = settings.hardwareDecodeEnabled
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

    /*
     * 这里原来有一个 700ms 的"方向不上报就强行落地"的兜底定时器。
     * 本轮按红线（禁止看门狗 / 超时兜底 / 周期动作）删除：落地只由**真实配置事件**驱动 ——
     * 上面那条 LaunchedEffect(pendingMode, isLandscape) 监听的就是系统真的转过去这件事；
     * requestedOrientation 是强制方向，系统一定会推配置变化，不需要用时钟去猜。
     */

    /*
     * 系统栏：**只有横屏全屏才隐藏**（沉浸式，下拉临时出现）；
     * 竖屏全屏和非全屏都显示状态栏。
     *
     * 【状态栏图标明暗**不在**这里写】它由下面那条 SideEffect 负责，而且必须在
     * "正在放视频"的分支里写 —— 原因是它要和 CicadaPlayerTheme 的默认值抢，
     * 详见那条 SideEffect 的注释（这里只做 show/hide，不碰 appearance）。
     */
    LaunchedEffect(displayMode) {
        val window = activity?.window ?: return@LaunchedEffect
        // 挖孔屏：显式声明窗口可以铺进挖孔区（短边 = 横屏时的左右两边）。
        //
        // 不声明时系统按默认策略（windowLayoutInDisplayCutoutMode 缺省，见
        // AndroidManifest / themes.xml 里都没有配）在横屏把整个窗口让开挖孔，
        // 表现就是**一侧**永远有一条补不掉的黑带（挖孔那一侧）。
        // 视频全屏要的就是画面到边，这里不让系统替我们留边；画面会被挖孔挡住
        // 一点，那是硬件遮挡，属于"真全屏"的正常代价。
        //
        // 只做一次判断，避免每次 displayMode 变化都触发一次窗口 relayout。
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P &&
            window.attributes.layoutInDisplayCutoutMode !=
            WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        ) {
            window.attributes = window.attributes.apply {
                layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            }
        }
        val bars = WindowCompat.getInsetsController(window, view)
        if (displayMode == PlayerDisplayMode.LandscapeFull) {
            bars.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            bars.hide(WindowInsetsCompat.Type.systemBars())
        } else {
            bars.show(WindowInsetsCompat.Type.systemBars())
        }
    }

    /* 离开播放页（Navigation 里 popBackStack）：恢复竖屏 + 显示系统栏，不然会留在横屏/沉浸式 */
    DisposableEffect(activity) {
        onDispose {
            activity?.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
            activity?.window?.let { window ->
                val bars = WindowCompat.getInsetsController(window, view)
                bars.show(WindowInsetsCompat.Type.systemBars())
                /*
                 * 状态栏图标明暗**交还给主题**：本页强制的是"浅色图标"（黑底），
                 * 离开这一页后必须按主题取值，否则会把下一个页面（浅色主题、浅色底）
                 * 也留在"白图标"上 —— 那正是"状态栏文字不见"的同一个病。
                 */
                bars.isAppearanceLightStatusBars = !isDarkMode
            }
        }
    }

    CicadaPlayerTheme(darkTheme = isDarkMode) {
        if (source.isBlank()) {
            SourceEntry(
                onPlay = { source = it },
                onClose = onClose,
                onToggleTheme = { isDarkMode = !isDarkMode },
                /* 【回退点 P1】这里原来是 hardwareDecodeEnabled / onHardwareDecodeChange 两个实参 */
                settings = settings,
                onHardwareDecodeChange = { settingsViewModel.setHardwareDecodeEnabled(it) },
            )
            return@CicadaPlayerTheme
        }

        val fullscreen = displayMode != PlayerDisplayMode.Inline

        /*
         * ============ 状态栏图标明暗：本页自己说了算，而且**每次组合都要重声明** ============
         *
         * 现象（用户报的）：进全屏 → 退出全屏之后，状态栏的文字/图标**不见了**。
         *
         * 成因是两个写者互相覆盖，而主题那个写者的取值在**本页是错的**：
         *   · 本页状态栏那一块**永远是深色**：Inline 分支自己刷了一条
         *     `background(Color.Black)`（见下面那条分支），全屏两支是 fillMaxSize、
         *     画面与 AspectFit 黑边一直铺到状态栏底下 ⇒ 正确取值是
         *     `isAppearanceLightStatusBars = false`（＝用**浅色**图标）；
         *   · 而 `CicadaPlayerTheme` 里有个 SideEffect，**每次组合**都把它设成
         *     `!darkTheme`（浅色主题 ⇒ true ⇒ **深色**图标）。于是浅色主题下退出全屏，
         *     图标被改回深色、落在我们的黑底上 ⇒ 看不见。
         *
         * 为什么这里必须是 SideEffect 而不是 LaunchedEffect(displayMode)：
         * 后者只在 displayMode 变化时写一次，而主题那个写者每次组合都会写；
         * SideEffect 每次成功组合都会执行，并且**主题的 SideEffect 先注册先执行**
         * （它在 content 之前），所以本页的值稳定生效（后写者赢）。
         *
         * 只放在"真的在放视频"这一支里（`source.isBlank()` 已在上面 return）：
         * 选片页 / 其它页面仍然由主题决定，不会被这里带偏。
         */
        SideEffect {
            val window = activity?.window ?: return@SideEffect
            WindowCompat.getInsetsController(window, view).isAppearanceLightStatusBars = false
        }

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
                    /*
                     * 非全屏专用：状态栏那一块刷黑，播放器在它下面（大厂做法）。
                     *
                     * statusBarsPadding() **只在这一支（Inline）里**，全屏那两支
                     * （LandscapeFull / PortraitFull）走的是上面的 fillMaxSize()，
                     * 整条全屏链路（本 Box → CicadaVideoPlayer → 内部 Box → SurfaceView）
                     * 没有任何 inset / padding / margin。排查全屏黑边时不要再来动这一行：
                     * 横屏全屏下系统栏已 hide，statusBars inset 本身就报 0；
                     * 全屏那条黑边来自**播放器内的 AspectFit 留边** —— tunnel render（A 方案）
                     * 下是 CicadaVideoPlayer 里 aspectRatio 布局留的，GL 模式下是内核
                     * 按 AspectFit 居中留的，两处注释都写了。
                     */
                    Modifier.fillMaxWidth().background(Color.Black).statusBarsPadding()
                },
            ) {
                CicadaVideoPlayer(
                    source = source,
                    isFullscreen = fullscreen,
                    hardwareDecodeEnabled = hardwareDecodeEnabled,
                    /*
                     * 【回退点 P2】镜像 / 色觉 / 后台播放的持久化靠这一个实参进去：
                     * 去掉它（用默认 null）就回到"这几项只活在 CicadaVideoPlayer 内部的
                     * remember 里"的旧行为（注意那种情况下旧行为已经不在代码里了，
                     * 要一并按 CicadaVideoPlayer 里的回退点 P3 改回去）。
                     */
                    settings = settings,
                    /*
                     * 【回退点 P3】四个设置回调：全部落到 PlayerSettingsViewModel 的 setter
                     * （内存 + 落盘 + `CicadaSettings` 日志）。删掉这四个实参就回到
                     * "这几项只活在本页/播放器内部 remember"的旧行为（配合 CicadaVideoPlayer
                     * 里同名的回退点 P3 一起改）。
                     */
                    onBackgroundPlayChange = { settingsViewModel.setBackgroundPlayEnabled(it) },
                    onDanmakuEnabledChange = { settingsViewModel.setDanmakuEnabled(it) },
                    onMirrorChange = { settingsViewModel.setMirrorMode(it) },
                    onColorVisionChange = { settingsViewModel.setColorVisionMode(it) },
                    // 画面缩放统一由 CicadaVideoPlayer 内部固定为 AspectFit（和 Qt 一致）：
                    // 全屏也是居中等比 + 留黑边，绝不裁切/拉伸。
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
    /*
     * 设置通过**数据 + 回调**进来，而不是两个散参数：
     * 页面上要展示的（硬解当前档位）用 [settings] 读，用户一改只走 [onHardwareDecodeChange]
     * 一个出口（它就是 PlayerSettingsViewModel 的 setter，负责"内存 + 落盘 + 日志"）。
     */
    settings: PlayerSettings,
    onHardwareDecodeChange: (Boolean) -> Unit,
) {
    val hardwareDecodeEnabled = settings.hardwareDecodeEnabled
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

            /*
             * ============ 硬解 / 软解开关（软解验证入口）============
             *
             * 关掉它，再用下面的地址起播 ⇒ 纯软解链路（内核 `SetUpVideoPath hw=0`、
             * `CreateVideoDecoder bHW=0`，且渲染自动落 GL）。用来验证"软解下 seek、
             * 切清晰度、自动切档(ABR)"是否正常。
             * 只影响**下一次起播**：本页就是"还没起播"的状态，所以不存在"播放中偷偷换解码器"。
             *
             * 【本轮修复①：这个开关现在**记得住**（跨界面 / 跨进程）】
             * 状态来自 PlayerSettingsViewModel（SharedPreferences 落盘），不再是本页的
             * `remember` —— 退出/重进播放页、甚至杀进程重开，都是用户上次选的那一档。
             *
             * 【本轮修复②：文案如实说清"什么时候生效"】
             * 内核 `SuperMediaPlayer::SetDecoderType`（SuperMediaPlayer.cpp:1282-1285）只是
             * 写了一个字段，**不会立刻重建解码器**；真正读它的地方是建视频路时的
             * `SetUpVideoPath()`（:9112-9136）。所以这里必须写"下次起播生效"，
             * 不能给用户一个"点了就立刻换解码器"的假印象。
             */
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween,
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = if (hardwareDecodeEnabled) "硬件解码（硬解）" else "软件解码（软解）",
                        color = PlayerTheme.onSurface,
                        fontSize = 14.sp,
                    )
                    Text(
                        text = "关掉 = 纯软解起播（渲染自动落 GL）。**下次开始播放时生效**，"
                            + "开关本身会记住（退出页面/重开 App 都还在）。"
                            + "日志确认：CicadaDecode 两行、内核 SetUpVideoPath hw=0、"
                            + "CreateVideoDecoder bHW=0。",
                        color = PlayerTheme.panelSubText,
                        fontSize = 11.sp,
                    )
                }
                Switch(
                    checked = hardwareDecodeEnabled,
                    onCheckedChange = onHardwareDecodeChange,
                )
            }

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
