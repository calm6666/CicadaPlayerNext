package com.cicada.player.compose.components.video

import android.Manifest
import android.content.ContentValues
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
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
import androidx.core.content.ContextCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.compose.ui.viewinterop.AndroidView
import com.cicada.player.CicadaPlayer
import com.cicada.player.nativeclass.TrackInfo
import com.cicada.player.compose.R
import com.cicada.player.compose.components.danmaku.Danmaku
import com.cicada.player.compose.components.danmaku.DanmakuConfig
import com.cicada.player.compose.components.danmaku.DanmakuType
import com.cicada.player.compose.components.danmaku.DanmakuView
import com.cicada.player.compose.player.CicadaPlayerController
import com.cicada.player.compose.player.PlayerSettings
import com.cicada.player.compose.player.TUNNEL_RENDER_ENABLED
import com.cicada.player.compose.ui.theme.PlayerTheme
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext

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
    /**
     * **横屏全屏铺满**：画面按 AspectFill 居中裁切铺到四边（不留黑边）；
     * false = 按 AspectFit 保完整画面（可能留黑边）。默认 false = 保持原行为。
     * 只影响缩放模式，不影响播放/解码/切档。
     */
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
    /**
     * **硬解开关（软解验证用）**：false ⇒ 起播前调用 `enableHardwareDecoder(false)`，
     * 内核 `SetUpVideoPath()` 里 `bEnableHwVideoDecode=0` ⇒ `bHW=0`，解码走软解；
     * 同时 `if (!bEnableHwVideoDecode || !bHW) tunnelRender = false` ⇒ 渲染自动落 GL
     * （软解帧是 CPU 帧，走不了 tunnel 的直出）。
     * 默认 true = 与今天完全一致。只在**下一次起播**生效（见下面片源 Effect 的 key）。
     */
    hardwareDecodeEnabled: Boolean = true,
    /**
     * **持久化的用户设置**（`player/PlayerSettings`，由 `PlayerSettingsViewModel` 落盘）。
     *
     * 【为什么加这个参数（用户报的"关闭后返回又打开了"）】镜像、色觉辅助、后台播放、
     * 弹幕开关原来都是本函数内部的 `remember`：播放页退出（`popBackStack`）→ composable
     * 被移出组合 → 状态销毁 → 下次进来全回默认。传进来的这一份是**跨界面/跨进程**
     * 存活的真源（SharedPreferences），本函数只读它、只通过回调改它。
     *
     * 默认 `null` = 老行为（各自回默认值），**不破坏任何已有调用点**。
     * 【回退点 P3】删掉这个参数 + 下面四处 `settings?.x ?: ...` 的兜底写法，
     * 把 `mirrorMode` / `colorVisionMode` / `backgroundPlayEnabled` / `danmakuEnabled`
     * 换回 `remember { mutableStateOf(...) }` 即回到本轮之前。
     */
    settings: PlayerSettings? = null,
    /**
     * 用户改设置的回调（**只有这一条出口**，由 `PlayerSettingsViewModel` 的 setter 接住：
     * 内存响应式状态 + SharedPreferences 落盘 + `CicadaSettings` 日志）。
     * 默认空实现 —— 不传 `settings` 的调用点行为与本轮之前一致。
     *
     * 【回退点 P3】删掉这四个参数 + 上一行的 `settings`，用下面注释里的 `remember` 写法还原。
     */
    onBackgroundPlayChange: ((Boolean) -> Unit)? = null,
    onDanmakuEnabledChange: ((Boolean) -> Unit)? = null,
    onMirrorChange: ((MirrorUiMode) -> Unit)? = null,
    onColorVisionChange: ((ColorVisionMode) -> Unit)? = null,
) {
    val context = LocalContext.current
    val controller = remember { CicadaPlayerController(context) }

    var showControls by remember { mutableStateOf(true) }
    var interactionTick by remember { mutableIntStateOf(0) }
    var openPanel by remember { mutableStateOf(PlayerPanel.None) }

    /* 弹幕输入框是否聚焦（键盘抬起期间控制栏不自动隐藏，键盘收起后再等 3 秒） */
    var danmakuInputFocused by remember { mutableStateOf(false) }

    /*
     * ============ 镜像画面（**内核 setMirrorMode**，本轮修复）============
     *
     * 三态：关 / 水平 / 垂直。**只翻转，不做旋转**（用户明确）。
     *
     * 【上一轮为什么无效（已定性，不再推翻）】上一轮是把
     * `Modifier.graphicsLayer { scaleX = -1f }` 挂在承载画面的 `AndroidView` 上。真机点击后
     * 画面**毫无变化** —— 视频是画在 **SurfaceView** 上的，它由 SurfaceFlinger 单独合成，
     * Compose 图层的负缩放传不到它的 buffer（那条路是空实现 **对 SurfaceView 而言** 的先天缺陷，
     * 不是参数写错）。所以本轮**删掉**那段 `.then(...)` 变换，改走内核：
     *   [controller.setMirrorMode] → `CicadaPlayer.setMirrorMode` → JNI → `SuperMediaPlayer`
     *   → `SMPMessageControllerListener` 对**当前**渲染器 `setFlip` → `GLRender`
     *   → `OESProgramContext::updateFlip` / `YUVProgramContext::updateFlip`（硬解 OES 与
     *   软解 YUV **两条路都翻**）。
     * 面板选择的还是 UI 三态枚举 [MirrorUiMode]（改名以免与内核 `CicadaPlayer.MirrorMode` 混淆），
     * 下发时在下面的 LaunchedEffect 里映射一次。
     *
     * 【边界（写明，不试图解决）】
     *   · 隧道 / direct 渲染（`FLAG_DUMMY`，HDR / Widevine 会被内核强制走那条）下渲染器是
     *     `DummyVideoRender`，它的 `setFlip` 是**空实现** ⇒ 那类片源镜像**仍无效**。
     *     **不要**再用 App 层变换去兜 —— 那正是这次被证明无效的做法。
     *   · 截图（`snapshot()`）**不带镜像**：内核抓帧处会临时补偿内核自己的 flip，那是抓帧内部的
     *     补偿，不影响本次改动，本轮也不做额外处理。
     *
     * 【回退点 B（本轮改写）】删掉这个状态变量 + 下面那条 `LaunchedEffect(mirrorMode)` +
     * 面板里 `mirror/onMirrorChange` 两个实参（PlayerPanels.kt 里对应的两行），即回到"没有镜像"的
     * 状态。上一轮那段 graphicsLayer 变换**不回退回去**（它本来就是无效的）。
     */
    /*
     * 【本轮修复①：状态来源改成"传入的持久化设置"】原来这里是
     * `var mirrorMode by remember { mutableStateOf(MirrorUiMode.None) }` —— 退出播放页
     * 就丢。现在读 `settings`（SharedPreferences 落盘，见 player/PlayerSettings.kt），
     * 用户上一次选的镜像档位在**下次进页面、甚至重开 App** 后仍然生效。
     * 【修复②：日志】点击处打 `CicadaMirror`（下面 onMirrorChange），下发处 controller 再打一条，
     * 两条同 tag 成对，真机一 filter 就能看清"点击 → 下发"的时序。
     * 【回退点 P3】把这一行换回 `var mirrorMode by remember { mutableStateOf(MirrorUiMode.None) }`，
     * 并把下面 onMirrorChange 里的 `settings` 写入改成只改这个局部变量。
     */
    val mirrorMode: MirrorUiMode = settings?.mirrorMode ?: MirrorUiMode.None

    /*
     * ============ 色觉辅助滤镜（**内核 GL 着色器**，本轮新增）============
     *
     * 与上面的镜像**刚好相反**：镜像走 App 层 View 变换，色觉滤镜必须在**内核渲染器的
     * GL 着色器**里乘一个 3x3 矩阵（App 层复刻一遍色彩变换既没意义也做不对 —— 隧道渲染下
     * 画面根本不经 App 的 GL）。
     *
     * 通道（完全照内核 `SetMirrorMode` 那条已验证通道新增的独立一条）：
     *   [controller.setColorMatrix] → Java `CicadaPlayer.setColorMatrix` → JNI `nSetColorMatrix`
     *   → `MediaPlayer::SetColorMatrix` → `CicadaSetColorMatrix` → `SuperMediaPlayer::SetColorMatrix`
     *   → `MSG_SET_COLOR_MATRIX` → `SMPMessageControllerListener::ProcessSetColorMatrix`
     *   → `IVideoRender::setColorMatrix`（**空默认实现**，其它平台一行都不用改）
     *   → `GLRender` → `OESProgramContext` / `YUVProgramContext` 的片元着色器每帧下发。
     *
     * 改完立刻生效，不重设 Surface、不碰解码/播放状态。
     *
     * 【已知限制（不解决，如实写在这里）】
     *   · 隧道/direct 渲染（HDR、Widevine 被内核强制走那条）没有着色器 ⇒ 滤镜**无效**；
     *     本工程默认走 GL 路（TUNNEL_RENDER_ENABLED = false），所以正常内容生效；
     *   · 截图（snapshot）抓的是加滤镜之前的帧 ⇒ 存下来的图**不带**滤镜（Qt 截图链路同源）。
     *
     * 【回退点 K3】删掉这个变量 + 下面那一个 LaunchedEffect + 面板里那两个实参即可。
     */
    /*
     * 【本轮修复：状态来源改成"传入的持久化设置"】原来这里是
     * `var colorVisionMode by remember { mutableStateOf(ColorVisionMode.None) }`：退出播放页就丢。
     * 现在读 `settings`，用户上一次选的滤镜档位跨界面/跨进程都还在。
     * 【回退点 P3】换回 `var colorVisionMode by remember { mutableStateOf(ColorVisionMode.None) }`。
     */
    val colorVisionMode: ColorVisionMode = settings?.colorVisionMode ?: ColorVisionMode.None

    /*
     * ============ 截图 → 存相册（带权限，本轮新增）============
     *
     * 链路：这里的 [startCapture] → `controller.snapshot()` → 内核 CaptureScreen
     *       → controller 的 `OnSnapShotListener`（主线程）→ 下面那条 LaunchedEffect 写相册。
     *
     * 权限：
     *   · API 29+ 写自己插入的 MediaStore 条目**免权限**（且不需要 WRITE_EXTERNAL_STORAGE）；
     *   · API < 29 需要 WRITE_EXTERNAL_STORAGE（manifest 里 maxSdkVersion=28），
     *     所以在真正发起截图前用 RequestPermission 运行时申请一次；拒了就如实提示，不假装存了。
     * 本工程 minSdk=24 ⇒ 两条路都要留。
     */
    var capturePending by remember { mutableStateOf(false) }

    /* 用户每一次"要截图"都 +1：用于重启下面那个"回调没来"的一次性提示计时 */
    var captureRequestSerial by remember { mutableIntStateOf(0) }

    /* 真正发起截图（权限已就绪）：只做两件事 —— 请求内核截当前帧、起一次性 UI 计时提示 */
    val startCapture: () -> Unit = {
        capturePending = true
        captureRequestSerial++
        /* 下发即打（事件驱动）：与下面写盘失败那条同一个 tag，真机 `-s CicadaCapture` 可看全程 */
        android.util.Log.i("CicadaCapture", "request snapshot (permission ready)")
        controller.snapshot()
    }

    /* 旧系统（< API 29）的写存储权限：拿到才继续，拿不到就如实提示 */
    val requestWritePermission = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (granted && capturePending) {
            /* 权限到手才真的发截图请求（capturePending 是 requestCapture 置的"等待中的请求"标记） */
            startCapture()
        } else {
            capturePending = false
            android.util.Log.i("CicadaCapture", "WRITE_EXTERNAL_STORAGE denied -> snapshot canceled")
            Toast.makeText(context, "没有存储权限，截图没有保存", Toast.LENGTH_SHORT).show()
        }
    }

    /* 截图入口（面板里那一行调用它）：按系统版本决定要不要先申请权限 */
    fun requestCapture() {
        if (capturePending) return
        val needsLegacyPermission = Build.VERSION.SDK_INT < Build.VERSION_CODES.Q &&
            ContextCompat.checkSelfPermission(context, Manifest.permission.WRITE_EXTERNAL_STORAGE) !=
            PackageManager.PERMISSION_GRANTED
        if (needsLegacyPermission) {
            /* 旧系统这条分支原来**没有任何日志**：真机上"点了截图没反应"分不清是
             * "权限框没弹"、"用户拒了"还是"内核没回调"。这里把"为什么先申请权限"记下来。 */
            android.util.Log.i(
                "CicadaCapture",
                "API ${Build.VERSION.SDK_INT} < 29 -> request WRITE_EXTERNAL_STORAGE first",
            )
            capturePending = true
            requestWritePermission.launch(Manifest.permission.WRITE_EXTERNAL_STORAGE)
        } else {
            startCapture()
        }
    }

    /*
     * 内核回调到达（`snapshotSerial` 变了）→ **取走所有权** → 压 PNG 写进相册（IO 线程）
     * → 回收 Bitmap（SDK 明确要求，见 CicadaPlayer.java:1298/1305）→ Toast 结果。
     *
     * 先 takeSnapshot() 再写盘：控制器里的引用立刻置空，下一张到达时不会把**正在写的**这张
     * 回收掉（回收放在 finally，写盘结束/被取消后才执行）。
     * 串号 0 = 本次会话还没有过任何截图，直接返回（LaunchedEffect 首帧会跑一次）。
     */
    LaunchedEffect(controller.snapshotSerial) {
        if (controller.snapshotSerial == 0) return@LaunchedEffect
        val bmp = controller.takeSnapshot() ?: return@LaunchedEffect
        var saved = false
        try {
            saved = withContext(Dispatchers.IO) { saveSnapshotToGallery(context, bmp) }
        } finally {
            bmp.recycle()
        }
        capturePending = false
        Toast.makeText(
            context,
            if (saved) "截图已保存到相册" else "截图保存失败",
            Toast.LENGTH_SHORT,
        ).show()
    }

    /*
     * 「发起截图后 N 秒没有回调」的一次性提示（**只提示一次，不重试、不轮询**）。
     *
     * 为什么会有"永远没有回调"：隧道渲染（HDR / Widevine 被内核强制 FLAG_DUMMY）下渲染器是
     * `DummyVideoRender`，它没有实现 captureScreen ⇒ 内核根本不会回调截图结果。
     *
     * 【红线说明】这个 delay **只决定一句 Toast 显不显示**，不看门狗、不重试、不参与任何
     * 播放 / seek / 解码 / 切档状态判断 —— 与上面清晰度终态提示那条 2600ms 是同一类纯 UI 计时。
     * 之所以放在 UI 而不是 controller：它要的是"用户看得见的一句提示"，而不是后台定时任务。
     */
    LaunchedEffect(captureRequestSerial) {
        if (captureRequestSerial == 0) return@LaunchedEffect
        val requestedAt = controller.snapshotSerial
        delay(CAPTURE_CALLBACK_TIMEOUT_MS)
        if (controller.snapshotSerial == requestedAt) {
            capturePending = false
            Toast.makeText(context, "该片源当前渲染模式下不支持截图", Toast.LENGTH_LONG).show()
        }
    }

    /*
     * 清晰度列表（面板与"能不能开面板"**共用同一份数据**）。
     * Qt 的判据就是同一个列表长度（RightControls.qml:1571 `hasQuality = qualities.length > 0`），
     * 所以这里只算一次、下面两处都用它。
     */
    val qualityOptions = buildQualities(
        controller.mediaInfo,
        controller.currentVideoIndex,
        controller.autoQuality,
    )

    /*
     * 控制栏那颗按钮上的清晰度文案 = **当前真实在播的那一档**（自动档下就是 ABR 实际选中的档）。
     *
     * 与 Qt 完全一致（RightControls.qml:1579-1586）：`qualityIndex < 0` 或查不到 → "自动"，
     * 否则显示那一档的档位名（4K/2K/1080P…）。以前这里拼的是 `${videoHeight}P`：
     * 那是**渲染分辨率**，会把 4K 写成 "2160P"、跟面板里的档位名对不上；
     * 而且自动档下也看不出 ABR 到底选了哪一档。
     *
     * 【回退点 A2】恢复旧文案：把下面这行换回
     * `val currentQualityLabel = if (controller.videoHeight > 0) "${controller.videoHeight}P" else "自动"`。
     */
    val currentQualityLabel: String = qualityLabelFor(controller.mediaInfo, controller.currentVideoIndex)
        ?: if (controller.autoQuality) "自动" else "清晰度"

    /*
     * 已经下发给内核的那个 Surface（**只认实例变化**）。
     *
     * A 方案下 SurfaceView 会随 videoSurfaceSize() 改尺寸 ⇒ surfaceChanged 会被调用；
     * 若在那里无条件 setSurface，就会触发主线程 ProcessSetViewMsg →
     * setOutputSurface / RestartVideoDecoder，与播放线程正在建的切档解码器并发
     * （真机 SIGSEGV fault addr 0x4 那次）。所以这里记住上次的实例，变了才设。
     */
    val lastSurface = remember { mutableStateOf<Surface?>(null) }

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
     *
     * 【本轮修复】原来这个开关是 `remember { mutableStateOf(true) }`：退出播放页就回默认"开"，
     * 用户关过之后重进页面又变开。现在读 [settings]（落盘），跨界面/跨进程都记得住。
     * 【回退点 P3】换回 `var backgroundPlayEnabled by remember { mutableStateOf(true) }`。
     */
    val backgroundPlayEnabled: Boolean = settings?.backgroundPlayEnabled ?: true
    /* pausedByBackground 是**本次播放的瞬时状态**（不是用户选择），继续用 remember —— 不该落盘 */
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
    /*
     * 【本轮修复】弹幕总开关改成读持久化设置（退出播放页不再是"又开回来"）。
     * 不透明度/字号/密度这三项仍是本次会话内的 UI 偏好（`remember`），
     * 理由见报告"一览表"：它们属于"看这一部片时的调节"，本轮不落盘。
     * 【回退点 P3】换回 `var danmakuEnabled by remember { mutableStateOf(true) }`。
     */
    val danmakuEnabled: Boolean = settings?.danmakuEnabled ?: true
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

    /*
     * 镜像画面 → **内核渲染器**（改完立刻生效；不重设 Surface、不碰解码/播放状态）。
     *
     * UI 三态 [MirrorUiMode] 在这里映射到内核枚举 `CicadaPlayer.MirrorMode`
     * （`MIRROR_MODE_NONE / MIRROR_MODE_HORIZONTAL / MIRROR_MODE_VERTICAL`）后下发一次，
     * 不做计时器/轮询类逻辑。key 含 controller：换片/重建 player 时会重放一次，所以
     * "先选镜像再开播"也不会丢。
     * 【本轮补的这一点很重要】因为 mirrorMode 现在来自**落盘设置**，首帧组合时这条 Effect
     * 就会用用户上次选的档位下发一次 —— 也就是"退出页面再进来，镜像自动恢复"，
     * 不是靠 App 层变换，而是内核 setFlip 重新生效。
     * 注：日志有两条同 tag（点击处一条、controller 一条），便于真机对时序；
     * 隧道/direct 渲染下内核 `DummyVideoRender::setFlip` 是空实现 ⇒ 那类片源仍无效。
     */
    LaunchedEffect(mirrorMode, controller) {
        controller.setMirrorMode(
            when (mirrorMode) {
                MirrorUiMode.None -> CicadaPlayer.MirrorMode.MIRROR_MODE_NONE
                MirrorUiMode.Horizontal -> CicadaPlayer.MirrorMode.MIRROR_MODE_HORIZONTAL
                MirrorUiMode.Vertical -> CicadaPlayer.MirrorMode.MIRROR_MODE_VERTICAL
            }
        )
    }

    /*
     * 色觉辅助滤镜 → 内核渲染器（改完立刻生效；单位矩阵 = 关闭）。
     * 面板一改就下发一次，不做计时器/轮询类逻辑。
     * 注：滤镜要在**渲染器建好之后**才吃得到；本 Effect 的 key 含 controller，
     * 换片/重建 player 时会重放一次，所以"先选滤镜再开播"也不会丢。
     *
     * 【本轮补的两点】
     *   ① 同镜像：colorVisionMode 来自落盘设置，进页面首帧就会把上次选的滤镜重新下发
     *      （退出再进来不会"悄悄回到关闭"）；
     *   ② 补一条下发日志（tag `CicadaColorVision`），与点击处那条成对 —— 以前只有镜像有日志，
     *      色觉这条链路点了之后**没有任何痕迹**，真机没法判断"是没点击、还是没下发"。
     */
    LaunchedEffect(colorVisionMode, controller) {
        controller.setColorMatrix(colorVisionMode.matrix)
        android.util.Log.i(
            "CicadaColorVision",
            "apply colorMatrix=${colorVisionMode.name} (identity = off)"
        )
    }

    /* 片源：地址或播放对象，全部交给内核（DASH/HLS/FLV/清单对象） */
    LaunchedEffect(source, controller) {
        if (source.isBlank()) return@LaunchedEffect
        /*
         * 解码方式：由"片源输入页"的硬解开关决定（默认硬解，与今天一致）。
         * 关闭时内核 bHW=0：解码走软解、渲染自动落 GL（软解不支持 tunnel 直出）。
         *
         * 【顺序为什么必须在这里（本轮特意写清楚 + 加了保险）】
         * 内核 `SuperMediaPlayer::SetDecoderType`（SuperMediaPlayer.cpp:1282-1285）只是
         * `mSet->bEnableHwVideoDecode = (type == DT_HARDWARE)` —— **一个字段赋值，不重建解码器**；
         * 真正读它的是建视频路时的 `SetUpVideoPath()`（:9112-9136）。所以必须在
         * `setDataSource/prepare`（下面 playAny）**之前**下发，否则本次起播建的还是上一个值。
         * 现在 `CicadaPlayerController` 也自己记住了这个选项，并在换源时（`resetForNewSource`）
         * 再下发一次 —— 就算以后有人把这里的调用顺序改了，"起播前下发"这条契约也不会破。
         *
         * 【什么时候**不**生效（如实说明，不假装）】播放中改这个开关**没有用**：内核不会因为
         * 一次字段赋值就去重建已经建好的解码器（没有任何 `EnableHardwareDecoder → 重建` 的通道，
         * `rebuildVideoDecoder()` 只在 codec 报错/后台 surface 失效时被调用，且它刻意沿用
         * 上一次的解码方式，见 SuperMediaPlayer.cpp:9462-9467）。想换解码方式就得重新起播。
         * 本工程 UI 上这个开关只出现在**起播前**的片源输入页，所以不存在"播放中点了没反应"的假开关；
         * 文案也已经如实写成"下次开始播放时生效"。
         *
         * 日志里可确认：`CicadaDecode` 两行（App 侧）+ `SetUpVideoPath tunnelRender=0 hw=0 renderFlags=0`
         * + `CreateVideoDecoder bHW=0 …`（内核侧）。
         */
        controller.enableHardwareDecoder(hardwareDecodeEnabled)
        controller.setVideoBackgroundColor(android.graphics.Color.BLACK)
        controller.playAny(source)
        /*
         * 这里**不需要**等 Surface 就绪再加任何等待逻辑：tunnel 模式下 view 为空时
         * 内核只是本轮不建解码器（SuperMediaPlayer.cpp:8125-8128），而 setUpAVPath()
         * 在消息循环里每轮都会重试（:1773）—— Surface 一到，下一次循环就建解码器。
         * 端侧要是加"轮询/超时"反而是红线（禁时间看门狗）。
         */
    }

    // 画面缩放：**永远用 AspectFit**（和 Qt 端一致，任何显示模式都不变）。
    //
    // 语义：按视频原始比例在窗口内**等比缩放并居中**，比例不一致时左右或上下留黑边；
    // **绝不裁切、绝不拉伸变形**。全屏也一样 —— 19.5:9 的屏放 16:9 的片，要"完整画面"
    // 就必然留黑边，这是几何决定的；想铺满只能靠裁切（SCALE_ASPECT_FILL = SM_CROP =
    // IVideoRender::Scale_AspectFill）或拉伸成变形，两者都不是我们要的行为。
    //
    // 只切 scaleMode：内核 SetScaleMode → MSG_SET_DISPLAY_MODE → 渲染器 setScale，
    // 播放中立即生效，**不重跑 playAny、不重新 prepare**（切全屏绝不能重建播放器，
    // 见上面片源 effect 的注释）。
    //
    // 【GL 路（默认）下这句真正生效】：内核 OESProgramContext 按它做等比/居中/黑边，
    // 与 Qt 一致。隧道路下渲染器是 DummyVideoRender、setScale 是空实现，那时
    // 等比/居中由 videoSurfaceSize() 的 aspectRatio 布局负责 —— 两条路都保留这句调用，
    // 切换模式时这里不用改。
    LaunchedEffect(controller) {
        controller.setScaleMode(CicadaPlayer.ScaleMode.SCALE_ASPECT_FIT)
    }

    DisposableEffect(controller) {
        onDispose { controller.release() }
    }

    /*
     * 控制栏自动隐藏：3 秒。
     *
     * 【只吸收 Qt 里与触摸等价的部分，鼠标特有的规则一律不搬】
     * 对齐的是 Qt `PlayerView.qml:619-631` 那条**隐藏条件**："正在等状态时不要收"：
     *   · Qt 靠"指针还在控制栏上/浮层开着/暂停中"来豁免；安卓没有指针悬停，
     *     等价物就是"还没播起来 / 正在缓冲 / 正在 seek / 正在切档"。
     * **不搬**的 Qt 规则（安卓没有鼠标，搬了只会添乱）：
     *   · 鼠标移动/悬停控制栏就重置倒计时（hover 不存在）；
     *   · 鼠标离开播放器 120ms 就收起（触屏没有"离开"事件）；
     *   · 控制栏隐藏时把鼠标指针变 BlankCursor；
     *   · 单击延迟 400ms 播放/暂停、双击切全屏 —— 安卓按用户明确要求保持
     *     **单击显隐控制栏、双击播放/暂停**（见下面手势那段）。
     *
     * 【红线】这个 3 秒 delay 只影响控制栏**显隐**，不参与、不判断任何播放 /
     * seek / 切档状态（与 Qt 的 3 秒自动隐藏同类）。
     */
    LaunchedEffect(
        showControls,
        controller.isPlaying,
        interactionTick,
        openPanel,
        danmakuInputFocused,
        controller.loading,
        controller.seeking,
        controller.switchingQuality,
    ) {
        if (showControls && controller.isPlaying && openPanel == PlayerPanel.None && !danmakuInputFocused &&
            !controller.loading && !controller.seeking && !controller.switchingQuality) {
            delay(3000)
            showControls = false
        }
    }

    Box(modifier = modifier.fillMaxSize().background(PlayerTheme.videoBg)) {
        /* ---------------- 画面：SurfaceView ----------------
         *
         * 【两条渲染路，SurfaceView 的尺寸规则不同（见 videoSurfaceSize()）】
         *
         * · GL 路（默认，TUNNEL_RENDER_ENABLED = false）：内核把解码器接到**它自己**的
         *   SurfaceTexture 上，再经 `GLRender` / `OESProgramContext` 画到这里的 Surface。
         *   等比/居中/黑边由**内核**做（setScaleMode(SCALE_ASPECT_FIT) +
         *   setVideoBackgroundColor），所以这里 **fillMaxSize**，绝不再按视频比例缩 ——
         *   否则 EGL 窗口跟着变小、白丢分辨率。
         * · 隧道路（TUNNEL_RENDER_ENABLED = true）：内核把解码器直接接到这个 Surface
         *   （`view = mSet->mView` + DECFLAG_DIRECT），渲染器是 DummyVideoRender，它的
         *   setScale / setBackgroundColor **是空实现**，而 SurfaceView 默认按
         *   SCALE_TO_WINDOW 把 buffer **拉伸**到 view 大小 ⇒ 必须由布局按视频比例定尺寸：
         *   外层居中 Box + aspectRatio(宽/高) = 等比 + 居中 + 留黑边。
         * 两种模式都**绝不裁切、绝不拉伸变形**；横屏全屏只是容器更大，规则不变。
         *
         * 手势挂在**外层 Box**、不是 AndroidView：黑边区域也要能
         * 单击显隐控制栏 / 双击暂停（否则只有画面那一块可点）。
         */
        Box(
            modifier = Modifier
                .fillMaxSize()
                .pointerInput(openPanel) {
                    detectTapGestures(
                        /* 双击 = 播放/暂停（也算一次交互：别刚点完就把控制栏收走） */
                        onDoubleTap = { controller.togglePlay(); interactionTick++ },
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
            contentAlignment = Alignment.Center,
        ) {
            AndroidView(
                factory = { ctx ->
                    SurfaceView(ctx).apply {
                        holder.addCallback(object : SurfaceHolder.Callback {
                            /*
                             * 【只认"Surface 实例变化"，尺寸变化绝不重设】
                             * 见上面 lastSurface 的说明：重设会触发主线程
                             * ProcessSetViewMsg → RestartVideoDecoder，与切档建解码器并发。
                             */
                            override fun surfaceCreated(holder: SurfaceHolder) {
                                if (lastSurface.value !== holder.surface) {
                                    lastSurface.value = holder.surface
                                    controller.setSurface(holder.surface)
                                }
                            }

                            override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, hh: Int) {
                                /* 尺寸变化不重设；只有 Surface 实例真的换了才设 */
                                if (lastSurface.value !== h.surface) {
                                    lastSurface.value = h.surface
                                    controller.setSurface(h.surface)
                                }
                            }

                            /* 内核要求：销毁时必须置 null */
                            override fun surfaceDestroyed(holder: SurfaceHolder) {
                                lastSurface.value = null
                                controller.setSurface(null)
                            }
                        })
                        keepScreenOn = true
                    }
                },
                /*
                 * 只改 **layout**：SurfaceView 实例和它的 Surface 都不重建。
                 * 方向/全屏不会重建 Activity（AndroidManifest.xml:40-41 声明了
                 * configChanges=orientation|screenSize|keyboardHidden），本工程也只有
                 * 这一个 CicadaVideoPlayer 调用点（VideoPlayerScreen.kt:245），
                 * 所以全屏/面板切换不会重建 SurfaceView —— 那正是历史上触发
                 * 解码器 flush 死循环的那件事，不能再犯。
                 *
                 * 宽高未知（首帧回调之前）先铺满，onVideoSizeChanged 一到就自动收敛。
                 */
                modifier = videoSurfaceSize(controller.videoWidth, controller.videoHeight),
            )
        }

        /* ---------------- 弹幕层（GPU 渲染，不占 CPU） ---------------- */
        DanmakuView(
            danmakus = danmakus,
            config = danmakuConfig,
            isPlaying = controller.isPlaying,
            currentTime = controller.positionMs,
            modifier = Modifier.fillMaxSize(),
        )

        /* ---------------- 加载 / 缓冲 / seek 反馈 ----------------
         * 原来这里是 `controller.loading && !controller.firstFrameRendered`：首帧之后
         * loading 再为真（seek 等待、缓冲、S2 的"视频路手里没东西"停顿）就**完全没有提示**，
         * 用户看到的是"画面冻住、什么都没有" —— 这正是"seek 要卡半天"的体感来源。
         * 现在只要 loading 或 seeking 就出指示；画面保持上一帧（内核不清屏，不会黑）。
         */
        if (controller.loading || controller.seeking) {
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
                                    /* 列表来自内核 MediaInfo 的视频轨（TrackInfo）；自动 = AUTO_SELECT_INDEX(-1)。
                                     * 选中态用 controller.autoQuality（用户意图，= 内核 ABR 是否开着），
                                     * 不能用 currentVideoIndex：自动档下内核报回来的是 ABR 自己选中的真实档位。
                                     * 【回退点 A3-2】这里的列表就是上面算好的 qualityOptions（面板与开启闸门共用）。 */
                                    qualities = qualityOptions,
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
                                    /* 【回退点 P3】原来是 `danmakuEnabled = !danmakuEnabled`（局部 remember） */
                                    onToggleEnabled = { onDanmakuEnabledChange?.invoke(!danmakuEnabled) },
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
                                    /* 【回退点 P3】原来是 `backgroundPlayEnabled = !backgroundPlayEnabled` */
                                    onToggleBackgroundPlay = {
                                        onBackgroundPlayChange?.invoke(!backgroundPlayEnabled)
                                    },
                                    /* 镜像画面（走内核 setMirrorMode；映射与下发见上面那条 LaunchedEffect）。
                                     * 日志**点击即打**（事件驱动，便于真机确认点击确实到达），
                                     * tag = CicadaMirror，与 controller 里那条同 tag 成对看。
                                     * 回退点见文件上方「镜像画面」那段注释。 */
                                    mirror = mirrorMode,
                                    /* 【回退点 P3】原来是 `mirrorMode = it`（局部 remember） */
                                    onMirrorChange = {
                                        onMirrorChange?.invoke(it)
                                        android.util.Log.i("CicadaMirror", "user picked mirror=$it")
                                    },
                                    /* 本轮新增：色觉辅助（走内核 GL 着色器）。回退点见 PlayerPanels.kt */
                                    colorVision = colorVisionMode,
                                    /* 【回退点 P3】原来是 `colorVisionMode = it` */
                                    onColorVisionChange = {
                                        onColorVisionChange?.invoke(it)
                                        android.util.Log.i("CicadaColorVision", "user picked colorVision=$it")
                                    },
                                    captureInFlight = capturePending,
                                    onCapture = { requestCapture() },
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
                            /* 拖动进度条的**全过程**都算交互并强制显示控制栏：
                             * 不能在用户正拖着的时候被 3 秒倒计时收走（Qt 那边进度条
                             * 就在控制栏里，等价于"指针在控制栏上"，我们靠这个补上）。 */
                            onSeek = {
                                showControls = true
                                interactionTick++
                                controller.seekTo(it)
                            },
                            onSeekStart = {
                                showControls = true
                                interactionTick++
                                controller.beginPreview(controller.positionMs)
                            },
                            onSeekEnd = { controller.endPreview(); interactionTick++ },
                            onTogglePortraitFullscreen = onTogglePortraitFullscreen,
                            onToggleFullscreen = onToggleFullscreen,
                            /* 【回退点 P3】原来是 `danmakuEnabled = !danmakuEnabled`（局部 remember） */
                            onToggleDanmaku = { onDanmakuEnabledChange?.invoke(!danmakuEnabled) },
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
                            /*
                             * 【回退点 A3-3】清晰度面板的开启闸门：**列表非空才允许打开**。
                             * 对齐 Qt RightControls.qml:1671-1680（`hasQuality = qualities.length > 0`，
                             * 为假时 onEntered 直接不 open()）—— 面板里因此不再需要塞假条目。
                             * 回退：去掉这个 if，写成 `openPanel = PlayerPanel.Quality`。
                             */
                            onQualityClick = {
                                if (qualityOptions.isNotEmpty()) openPanel = PlayerPanel.Quality
                            },
                            onInteraction = { interactionTick++ },
                            onDanmakuInputFocusChanged = { danmakuInputFocused = it },
                            /* 【回退点 A2】文案 = 当前档 label（见上面 currentQualityLabel 的说明） */
                            qualityLabel = currentQualityLabel,
                            speedLabel = if (speed == 1f) "倍速" else "${speed}x",
                            previewFrames = previewFrames,
                        )
                    }
                }
            }
        }

        /* ---------------- 清晰度切换提示（三段式） ----------------
         *
         * 【进行中】内核 STARTED → `正在切换到 X…`，**一直显示到终态**（没有定时器）。
         * 【终态确认】READY / FAILED / CANCELED → 换成
         *   `已切换到 X` / `切换失败，保持当前清晰度` / `已取消切换`，
         *   由下面那条 **UI 定时器 2600ms** 自动收起。
         *
         * 【红线】那个 delay **只控制这条提示自身的可见性**，不参与、不判断任何
         * 播放 / seek / 切档状态；内核状态机的每个 STARTED 仍由内核自己收敛到终态。
         * 2600ms 与 Qt PlayerView.qml:220-225 同值，但 Qt 是为鼠标交互服务，
         * 我们这里只是"别让提示赖在屏幕上"——**不是**照搬 Qt 的鼠标逻辑。
         *
         * 优先级：新的 STARTED（进行中）覆盖还没消失的终态提示（`when` 里进行中在前）。
         *
         * 位置：在弹幕层与加载圈之后绘制（Compose 里后写的在上层），底部留 76dp
         * 避开控制条。用户原话是"加载圈之上、弹幕层之下"，但弹幕层本身写在加载圈
         * 之前（即弹幕在加载圈**下面**），两句话在 Compose 绘制顺序里不可能同时成立；
         * 按"提示必须看得见、又不压住控制栏/进度条"落地。
         */
        val switchingLabel =
            qualityLabelFor(controller.mediaInfo, controller.switchingQualityIndex)
        val noticeLabel = qualityLabelFor(controller.mediaInfo, controller.qualityNoticeIndex)
        val noticeText = when (controller.qualityNoticeStatus) {
            CicadaPlayerController.QUALITY_NOTICE_READY ->
                if (noticeLabel != null) "已切换到 $noticeLabel" else "清晰度切换完成"
            CicadaPlayerController.QUALITY_NOTICE_FAILED -> "切换失败，保持当前清晰度"
            CicadaPlayerController.QUALITY_NOTICE_CANCELED -> "已取消切换"
            else -> null
        }
        val noticeVisibleText = when {
            controller.switchingQuality ->
                if (switchingLabel != null) "正在切换到 $switchingLabel…" else "正在切换清晰度…"
            noticeText != null -> noticeText
            else -> null
        }

        if (noticeVisibleText != null) {
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(start = 12.dp, bottom = 76.dp),
                contentAlignment = Alignment.BottomStart,
            ) {
                Text(
                    text = noticeVisibleText,
                    color = PlayerTheme.panelText,
                    fontSize = 13.sp,
                    modifier = Modifier
                        .background(PlayerTheme.panelBg, RoundedCornerShape(4.dp))
                        .padding(horizontal = 12.dp, vertical = 8.dp),
                )
            }
        }

        /* 终态确认提示的自动消失：**只改提示自身**；serial 变了就重新计时
         *（新提示覆盖旧提示时不会沿用旧剩余时间而被提前收掉）。 */
        LaunchedEffect(controller.qualityNoticeSerial) {
            if (controller.qualityNoticeStatus != CicadaPlayerController.QUALITY_NOTICE_NONE) {
                delay(2600)
                controller.clearQualityNotice()
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
 * 画面尺寸（**两条渲染路规则不同**，见 SurfaceView 那段注释）：
 *
 *   · 隧道路（`TUNNEL_RENDER_ENABLED = true`）：等比/居中只能由**布局**做
 *     （内核渲染器是 DummyVideoRender，setScale 空实现，而 SurfaceView 会把解码器
 *     buffer 拉伸到 view 大小）⇒ 按视频比例定 view 尺寸。
 *   · GL 路（默认）：SurfaceView **铺满容器**，等比/居中/黑边由**内核渲染器**画
 *     （`setScaleMode(SCALE_ASPECT_FIT)` + `setVideoBackgroundColor`，与 Qt 一致）。
 *     这里再套 aspectRatio 只会把 EGL 窗口一起缩小、白丢分辨率。
 *
 * 只返回 layout modifier：**不碰 SurfaceView 实例、不碰它的 Surface**，
 * 所以尺寸变化（首帧、切档换分辨率、横竖屏）都不会重建 Surface。
 */
private fun videoSurfaceSize(width: Int, height: Int): Modifier =
    if (TUNNEL_RENDER_ENABLED) {
        if (width > 0 && height > 0) {
            Modifier.aspectRatio(width.toFloat() / height.toFloat())
        } else {
            Modifier.fillMaxSize()
        }
    } else {
        Modifier.fillMaxSize()
    }

/**
 * 「发起截图后多久没等到内核回调就提示」的时长（纯 UI 计时，见上面那条 LaunchedEffect）。
 *
 * 为什么取 3 秒：隧道渲染下是**永远不会回调**，但 GL 路下内核确实要等下一次渲染才抓帧，
 * 取值太短会把"慢一点"误报成"不支持"；3 秒对两种情况都够用，且只影响一句提示。
 */
private const val CAPTURE_CALLBACK_TIMEOUT_MS = 3_000L

/**
 * 把截图写进相册的 `Pictures/<AppName>` 目录（**只用系统 MediaStore，不引任何第三方库**）。
 *
 * 两条路（minSdk=24，所以都要留）：
 *   · API 29（Q）及以上：`RELATIVE_PATH` + `IS_PENDING`。写的是**自己刚插入的那条**记录，
 *     所以**不需要任何存储权限**；先置 IS_PENDING=1，写完再置 0（避免相册里出现半张图）。
 *   · API 28 及以下：MediaStore 只认 `DATA` 绝对路径，先建目录再插入；这一步需要
 *     `WRITE_EXTERNAL_STORAGE`（manifest 里带 maxSdkVersion=28，运行时申请见截图入口）。
 *
 * 只在 IO 线程调用（压缩大图会阻塞）；**不在这里 recycle Bitmap** —— 回收由调用方
 * 在写盘结束后统一做（可能要被压缩多次/失败重试的语义都在调用方那一边更清楚）。
 */
@Suppress("DEPRECATION")
private fun saveSnapshotToGallery(context: Context, bitmap: Bitmap): Boolean {
    val appName = context.getString(R.string.app_name)
    val fileName = "cicada_" +
        SimpleDateFormat("yyyyMMdd_HHmmss_SSS", Locale.US).format(Date()) + ".png"

    val values = ContentValues().apply {
        put(MediaStore.Images.Media.DISPLAY_NAME, fileName)
        put(MediaStore.Images.Media.MIME_TYPE, "image/png")
    }
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
        values.put(MediaStore.Images.Media.RELATIVE_PATH, Environment.DIRECTORY_PICTURES + "/" + appName)
        values.put(MediaStore.Images.Media.IS_PENDING, 1)
    } else {
        /* 旧系统：先把目录建出来，再把绝对路径交给 MediaStore（否则相册里查不到这张图） */
        val dir = File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_PICTURES), appName)
        if (!dir.exists() && !dir.mkdirs()) return false
        values.put(MediaStore.Images.Media.DATA, File(dir, fileName).absolutePath)
    }

    val resolver = context.contentResolver
    val uri = runCatching {
        resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
    }.getOrNull() ?: return false

    return try {
        val out = resolver.openOutputStream(uri) ?: return false
        out.use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val done = ContentValues().apply { put(MediaStore.Images.Media.IS_PENDING, 0) }
            resolver.update(uri, done, null, null)
        }
        true
    } catch (t: Throwable) {
        /* 失败就把半截记录删掉：不要让相册里留一条打不开的空图 */
        android.util.Log.w("CicadaCapture", "save snapshot failed: ${t.message}")
        runCatching { resolver.delete(uri, null, null) }
        false
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
