package com.cicada.player.compose.player

import android.content.Context
import android.graphics.Bitmap
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Surface
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.cicada.player.CicadaPlayer
import com.cicada.player.CicadaPlayerFactory
import com.cicada.player.bean.ErrorInfo
import com.cicada.player.bean.InfoBean
import com.cicada.player.bean.InfoCode
/* 对象方式入口：playManifest(manifest: MediaManifest) 用的类型化模型 */
import com.cicada.player.manifest.MediaManifest
import com.cicada.player.nativeclass.MediaInfo
import com.cicada.player.nativeclass.TrackInfo
import org.json.JSONObject

/**
 * ============ 渲染路开关（B4：两条路，按内容自动选）============
 *
 * 现在有**两条**渲染路，内核按内容自己挑：
 *
 *   · 普通内容（含 4K SDR）→ **GL 路**：`GLRender` + SurfaceTexture + OES 外部纹理。
 *     解码器绑的是**内核自建**的 SurfaceTexture（`GLRender::getSurface()` →
 *     `SuperMediaPlayer.cpp` 的 `CreateVideoDecoder`），渲染器再画到 App 的 Surface。
 *     零 **CPU** 拷贝（解码 buffer 直接进 EGLImage 被 GPU 采样），代价是多一次 GPU
 *     合成、约半帧~1 帧延迟；换来的是"解码器与 App 的 Surface 生命周期解耦"。
 *
 *   · HDR / Widevine → **隧道直出**（内核强制，App 不用判断）：
 *     `SetUpVideoPath()` 里 `isHDRVideo(meta) ⇒ FLAG_DUMMY`、`isWideVine ⇒ FLAG_DUMMY`。
 *     HDR 需要显示链路拿到 HDR 元数据、DRM 的 secure buffer GPU 采不了 ⇒ 只有这条路成立。
 *
 * 所以这个开关的含义是"**普通内容走哪条路**"：
 *   false（默认，推荐）= 普通内容走 GL；HDR/DRM 由内核自动切隧道。
 *   true              = 普通内容也走隧道（解码器直出 App 的 SurfaceView）。
 *
 * 【一键回退】把这一行改成 true，就回到"全部走隧道"的旧行为（内核侧不用改）。
 *
 * 【为什么默认 GL（隧道把 App 的 Surface 交给 codec 带来的三件事，GL 都结构性消掉）】
 *   ① 退后台时 App 的 Surface 被系统销毁 = codec 输出面失效（后台黑屏/ACodec 报错
 *      的来源，只能靠 setOutputSurface 热切或重建解码器兜）；
 *   ② 一个 Surface 同时只允许一个 MediaCodec 连接 ⇒ 切清晰度只能靠"占位面 + 交接"；
 *   ③ 等比/居中/黑边/背景色只能由 App 布局硬凑（`DummyVideoRender` 的 setScale /
 *      setBackgroundColor 是空实现，而 SurfaceView 还会把 buffer 拉伸到 view 大小）。
 *   GL 路下 codec 只认内核的 SurfaceTexture（App 的 Surface 只给 EGL），三条都不存在：
 *   后台只是 EGL 窗口消失/重建，伸缩与黑边由内核 `OESProgramContext` 画。
 *
 * 【GL 下 App 侧要配合的两件事（本工程已做）】
 *   · `setScaleMode(SCALE_ASPECT_FIT)` + `setVideoBackgroundColor` 真正生效
 *     （`CicadaVideoPlayer.kt` 里已有）；
 *   · SurfaceView **铺满容器**，不再按视频比例缩（`videoSurfaceSize()` 已按本开关分流）。
 */
internal const val TUNNEL_RENDER_ENABLED = false

/**
 * 播放控制层：把**本工程自己的播放器内核**接进 Compose。
 *
 * 内核 = `:cicadaplayer` 模块（`com.cicada.player.CicadaPlayer`），
 * 编的是 CicadaPlayerNext 的 C++ 核心 —— 与 Qt 播放器**同一个内核**。
 * **不使用 ExoPlayer/Media3**。
 *
 * 接口出处（本工程 `cicadaplayer` 模块里的 `com.cicada.player.CicadaPlayer`）：
 *   createCicadaPlayer(Context) / setSurface(Surface) / setDataSource(String) /
 *   setDataSource(JSONObject)、setDataSourceManifest(String)（播放对象 = MediaManifest 同结构）/
 *   prepare / start / pause / stop / reload / release / seekTo(long, SeekMode) /
 *   setSpeed(0.5~2) / setVolume(0~2) / setMute / setLoop / enableHardwareDecoder /
 *   setScaleMode / setVideoBackgroundColor / snapshot / selectTrack(int) / currentTrack(TrackInfo.Type)
 *
 * **进度/缓冲/网速没有 getter**（接口里没有 getCurrentPosition）：由 `OnInfoListener` +
 * `InfoBean` 推过来，见 `InfoCode.CurrentPosition(2)` / `BufferedPosition(1)` /
 * `CurrentDownloadSpeed(4)`；`InfoCode.SwitchToSoftwareVideoDecoder(100)` = 硬解回退软解。
 */
class CicadaPlayerController(context: Context) {

    val player: CicadaPlayer = CicadaPlayerFactory.createCicadaPlayer(context.applicationContext)

    /**
     * 主线程 Handler：只给截图那条回调兜底用（SDK 本来就把截图结果送到主线程，
     * 见 registerListeners 里 setOnSnapShotListener 的说明）。
     */
    private val mainHandler = Handler(Looper.getMainLooper())

    /** 内核状态：CicadaPlayer.idle/initalized/prepared/started/paused/stopped/completion/error */
    var state by mutableIntStateOf(CicadaPlayer.idle)
        private set
    var isPlaying by mutableStateOf(false)
        private set
    var durationMs by mutableLongStateOf(0L)
        private set
    var positionMs by mutableLongStateOf(0L)
        private set
    var bufferedMs by mutableLongStateOf(0L)
        private set

    /** 当前下载网速（bps），InfoCode.CurrentDownloadSpeed */
    var downloadSpeedBps by mutableLongStateOf(0L)
        private set
    var videoWidth by mutableIntStateOf(0)
        private set
    var videoHeight by mutableIntStateOf(0)
        private set

    /** 首帧已上屏（OnRenderingStartListener） */
    var firstFrameRendered by mutableStateOf(false)
        private set
    var loading by mutableStateOf(false)
        private set
    var loadingPercent by mutableIntStateOf(0)
        private set
    var errorCode by mutableIntStateOf(0)
        private set
    var errorMessage by mutableStateOf<String?>(null)
        private set

    /** 流信息（OnTrackReadyListener；清晰度/字幕菜单的数据源） */
    var mediaInfo by mutableStateOf<MediaInfo?>(null)
        private set

    /** 内核提示"硬解回退成软解"的说明（InfoCode.SwitchToSoftwareVideoDecoder） */
    var softwareDecoderNotice by mutableStateOf<String?>(null)
        private set

    var seeking by mutableStateOf(false)
        private set

    /**
     * 清晰度切换**进行中**（内核 `PLAYER_QUALITY_SWITCH_STARTED`）。
     *
     * 只由内核事件驱动：STARTED 置真并**一直显示**到终态；READY / FAILED / CANCELED
     * 置假。这里**没有**定时器 —— 内核状态机的每个 STARTED 都必然以三个终态之一收尾
     * （发事件的调用点见 mediaPlayer/SMPMessageControllerListener.cpp，
     * 收尾在 finishQualitySwitch）。
     *
     * 终态那一条"确认提示"是分开的状态（见 [qualityNoticeStatus]）：它才用 UI 定时器。
     */
    var switchingQuality by mutableStateOf(false)
        private set

    /** 切换目标视频流下标（STARTED 事件带过来；面板里查得到就用它的档位名做文案） */
    var switchingQualityIndex by mutableIntStateOf(TrackInfo.AUTO_SELECT_INDEX)
        private set

    /**
     * 清晰度切换**终态确认**提示：0=无 / 1=READY / 2=FAILED / 3=CANCELED。
     *
     * 与 [switchingQuality] 分开的原因：进行中要一直显示到终态，用户才来得及看到；
     * 终态这条允许由 **UI 定时器**自动消失（2600ms，与 Qt PlayerView.qml:220-225 同值，
     * 但 Qt 那边是为了配合鼠标交互，我们这里只是"别让提示赖在屏幕上"）。
     *
     * **红线说明**：那个 2600ms 计时只控制这条提示**自身的可见性**，不参与、不判断任何
     * 播放 / seek / 切档状态；它跟 Qt 的"控制栏 3 秒自动隐藏"是同一类纯 UI 计时。
     *
     * [qualityNoticeSerial] 每次终态 +1：UI 用 `LaunchedEffect(serial)` 重启计时，
     * 保证"新提示覆盖旧提示"时不会沿用旧剩余时间而被提前收掉。
     */
    var qualityNoticeStatus by mutableIntStateOf(QUALITY_NOTICE_NONE)
        private set

    /** 终态提示对应的目标流下标（面板里查得到就显示档位名） */
    var qualityNoticeIndex by mutableIntStateOf(TrackInfo.AUTO_SELECT_INDEX)
        private set

    /** 终态提示的版本号（见 [qualityNoticeStatus] 的说明） */
    var qualityNoticeSerial by mutableIntStateOf(0)
        private set

    /** prepare 完成后是否自动 start */
    var autoPlayWhenPrepared: Boolean = true

    /**
     * 用户选的解码方式（true = 硬解）。**只是"最近一次下发的意图"的副本**，
     * 不是从内核读回来的状态（内核 Java 接口没有 getter）。
     *
     * 为什么控制器要自己留一份：`resetForNewSource()` 里要在 setDataSource **之前**
     * 把解码方式补发一次（内核只在建视频路时读这个字段），不记住就没法补发。
     */
    private var hardwareDecodeEnabled: Boolean = true

    /** 最近一次拖动的目标位置（快进预览图用；松手后清空） */
    var previewPositionMs by mutableLongStateOf(0L)
        private set
    var previewVisible by mutableStateOf(false)
        private set

    init {
        registerListeners()
        applyRenderMode()
    }

    /**
     * 下发渲染模式（"普通内容走哪条路"的开关，见文件头的 [TUNNEL_RENDER_ENABLED]）。
     * 注意它**只影响普通内容**：HDR / Widevine 由内核在 `SetUpVideoPath()` 里强制走隧道。
     *
     * 用 getConfig → 改一个字段 → setConfig 的写法：拿到的是内核**当前**配置，
     * 只把 tunnel 那一项改掉再整体写回，其余字段原样保留（所以不会顺带把超时/
     * 缓冲/UA 这些选项重置成 Java 侧默认值 —— `getJPlayerConfig` 是把内核配置
     * 逐字段填进 Java 对象的，见 `JavaPlayerConfig.cpp:115-168`）。
     *
     * 调用链（已核实，与本轮无关的 SDK 一行未改）：
     *   CicadaPlayer.java:1274 → CicadaPlayerImpl.java:746 → NativePlayerBase.java:301
     *   → NativeBase.cpp:1112(nSetConfig) → JavaPlayerConfig.cpp:86-88/133
     *   → MediaPlayer.cpp:553("enableVideoTunnelRender") → SuperPlayer.cpp:1018
     *
     * 只调一次即可：这个选项写在 SuperMediaPlayer 的配置里，之后每次建解码器
     * （SetUpVideoPath）都会读它。getConfig 返回 null 只可能是 player 为空，
     * 那种情况保持默认（GL 路），不抛异常。
     */
    private fun applyRenderMode() {
        val config = player.config ?: return
        config.mEnableVideoTunnelRender = TUNNEL_RENDER_ENABLED
        player.setConfig(config)
    }

    private fun registerListeners() {
        player.setOnPreparedListener {
            durationMs = player.duration.coerceAtLeast(0L)
        }
        player.setOnRenderingStartListener {
            firstFrameRendered = true
            loading = false
        }
        player.setOnStateChangedListener { newState ->
            state = newState
            isPlaying = newState == CicadaPlayer.started
            if (newState == CicadaPlayer.prepared) {
                durationMs = player.duration.coerceAtLeast(0L)
                if (autoPlayWhenPrepared) player.start()
            }
        }
        player.setOnCompletionListener { isPlaying = false }

        player.setOnLoadingStatusListener(object : CicadaPlayer.OnLoadingStatusListener {
            override fun onLoadingBegin() { loading = true }
            override fun onLoadingProgress(percent: Int, netSpeed: Float) {
                loadingPercent = percent
                /* 内核给的是 kbps，这里统一存 bps */
                if (netSpeed > 0f) downloadSpeedBps = (netSpeed * 1000f).toLong()
            }
            override fun onLoadingEnd() { loading = false }
        })

        player.setOnErrorListener { errorInfo ->
            errorCode = errorInfo.code.ordinal
            errorMessage = errorInfo.msg ?: "播放出错"
            loading = false
            /* seek 状态也要收掉：出错时不会再有 OnSeekComplete，留着会让 UI 指示永远转圈 */
            seeking = false
            isPlaying = false
        }
        player.setOnTrackReadyListener { info ->
            mediaInfo = info
            /*
             * 【自动档必须在"列表就绪"这一刻把内核 ABR 打开 —— 与 Qt 完全同一处补偿】
             *
             * Qt：CicadaPlayerItem::notifyQualities()（CicadaPlayerItem.cpp:2459）里
             * `if (m_autoQuality && !m_qualitySwitching) SelectTrack(SELECT_TRACK_VIDEO_AUTO)`。
             * 原因（那边注释写得很详细，这里一字不改地照抄结论）：
             *   * 内核只有 SelectTrack(-1) 会 `mAbrManager->EnableAbr(true)`
             *     （mediaPlayer/MediaPlayer.cpp:312），不调它就永远是"界面写自动、实际手动"；
             *   * 不调它时内核按 `mSet->mDefaultBandWidth`（默认 0）挑"离 0 最近"的档，
             *     也就是码率最低的一档起播（SMPMessageControllerListener 选流处
             *     abs(mDefaultBandWidth - metaBandWidth) 最小者），之后 ABR 线程也不会跑。
             * 清单/清晰度列表就绪 = prepare 完成、ABR 需要的码率表已经注册好，正是补调的时机。
             */
            if (autoQuality) {
                player.selectTrack(TrackInfo.AUTO_SELECT_INDEX)
            }
        }

        player.setOnInfoListener { bean: InfoBean ->
            when (bean.code) {
                InfoCode.CurrentPosition -> {
                    val p = bean.extraValue
                    if (p >= 0) positionMs = p
                }
                InfoCode.BufferedPosition -> {
                    val b = bean.extraValue
                    if (b >= 0) bufferedMs = b
                }
                InfoCode.CurrentDownloadSpeed -> {
                    val s = bean.extraValue
                    if (s > 0) downloadSpeedBps = s
                }
                InfoCode.SwitchToSoftwareVideoDecoder ->
                    softwareDecoderNotice = bean.extraMsg ?: "已切换为软解"
                InfoCode.LowMemory -> softwareDecoderNotice = "系统可用内存不足"
                else -> Unit
            }
        }

        player.setOnVideoSizeChangedListener { w, h ->
            if (w > 0 && h > 0) {
                videoWidth = w
                videoHeight = h
            }
        }
        player.setOnSeekCompleteListener { seeking = false }

        /*
         * ============ 截图结果（内核 CaptureScreen 的那条回调）============
         *
         * 链路（Java 全链路已通，App 侧只接这一条）：
         *   CicadaPlayer.snapshot() → CicadaPlayerImpl.snapshot() → 内核 CaptureScreen
         *   → NativePlayerBase.onCaptureScreen(w,h,buffer) 自己 createBitmap
         *   → 本回调 onSnapShot(bm, w, h)。
         *
         * 【线程】SDK 是用 `mCurrentThreadHandler` 送出来的，而那个 Handler 绑的是
         * `Looper.getMainLooper()`（NativePlayerBase.java:109），也就是**本来就到主线程**。
         * 这里再显式 post 一次，是为了让"mutableStateOf 只在主线程写"这件事不依赖 SDK 实现细节。
         *
         * 【Bitmap 的归属】bm 由内核侧创建，SDK 明确要求用完后 recycle（CicadaPlayer.java:1298/1305）。
         * 这里只保留**最新一张**：
         *   · 新的一张到达时，若上一张还没被界面取走（takeSnapshot），就地回收 —— 内存有界；
         *   · 已经被界面取走的（snapshotBitmap 已置空）绝不在这里动，避免"界面正在压缩写盘、
         *     这边把它 recycle 掉"的竞态。
         *
         * 【隧道/HDR】内核隧道渲染（FLAG_DUMMY）下渲染器是 DummyVideoRender，没有实现
         * captureScreen ⇒ **这条回调永远不会来**；界面侧靠"发起后 N 秒没回调"给一次性提示，
         * 不在这里造超时（看门狗红线见 CicadaVideoPlayer 那段的说明）。
         */
        player.setOnSnapShotListener { bm, _, _ ->
            /* 内核 createBitmap 失败时回调里给的就是 null（NativePlayerBase.java:1102-1108）：
             * 用 `?.let` 而不是先 null 判断再赋值，避免平台类型上的智能转换问题。 */
            bm?.let { shot ->
                mainHandler.post {
                    /* 上一张还没被界面取走就地回收（取走过的已经被置空，不会误伤正在写盘的那张） */
                    snapshotBitmap?.recycle()
                    snapshotBitmap = shot
                    snapshotSerial++
                }
            }
        }

        /*
         * 清晰度切换：**只读内核事件**（不在 selectQuality() 里自己造提示 ——
         * 自己造的话"点了但内核没切成"也会显示正在切换）。
         *
         * 四态处理：
         *   STARTED  → 进行中置真（持续显示到终态）；
         *   READY    → 收起进行中 + "已切换到 X"；
         *   FAILED   → 收起进行中 + "切换失败，保持当前清晰度"；
         *   CANCELED → 收起进行中 + "已取消切换"。
         *
         * 终态那条提示由 UI 侧 2600ms 定时器收起（见 qualityNoticeStatus 的说明）。
         */
        player.setOnVideoQualitySwitchListener(object : CicadaPlayer.OnVideoQualitySwitchListener {
            override fun onVideoQualitySwitch(status: Int, streamIndex: Int, description: String?) {
                /*
                 * 安卓侧状态日志（本轮 ② 的 D 项，**零内核改动**）。
                 *
                 * 内核四个状态只在 notifier 里发事件、自己不打日志（STARTED 连调用点都没有
                 * 日志，CANCELED/FAILED 那两条调用点也没配），所以端侧这一行是**唯一**能把
                 * "四态到底有没有送达"变成可判定事实的地方。用 android.util.Log 而不是 SDK 的
                 * Logger：后者受内核日志等级影响（默认 INFO，DEBUG 行根本不出现）。
                 */
                Log.i(
                    TAG_QUALITY_SWITCH,
                    "quality switch status=$status stream=$streamIndex desc=$description"
                )

                if (status == CicadaPlayer.OnVideoQualitySwitchListener.STATUS_STARTED) {
                    switchingQuality = true
                    switchingQualityIndex = streamIndex
                    return
                }

                switchingQuality = false
                switchingQualityIndex = TrackInfo.AUTO_SELECT_INDEX
                qualityNoticeStatus = when (status) {
                    CicadaPlayer.OnVideoQualitySwitchListener.STATUS_READY -> QUALITY_NOTICE_READY
                    CicadaPlayer.OnVideoQualitySwitchListener.STATUS_FAILED -> QUALITY_NOTICE_FAILED
                    else -> QUALITY_NOTICE_CANCELED
                }
                qualityNoticeIndex = streamIndex
                /* 版本号 +1：UI 侧据此重启 2.6s 计时（新提示覆盖旧提示时不会提前消失） */
                qualityNoticeSerial++
            }
        })
    }

    /**
     * UI 侧 2.6s 计时到点后调用：只清"终态提示"本身。
     * **不碰** switchingQuality、不碰任何播放 / seek / 切档状态。
     */
    fun clearQualityNotice() {
        qualityNoticeStatus = QUALITY_NOTICE_NONE
    }

    // ---------------- 命令 ----------------

    /** 画面：SurfaceView 的 Surface（内核渲染器直通 = 零拷贝）。销毁时必须传 null。 */
    fun setSurface(surface: Surface?) = player.setSurface(surface)

    /** 普通地址（mp4 / flv / m3u8 / mpd…），全部由内核对接 */
    fun playUrl(url: String) {
        resetForNewSource()
        player.setDataSource(url)
        player.prepare()
    }

    /** **播放对象**（MediaManifest JSON；清单对象结构由内核对接，App 只负责原样传进去） */
    fun playManifest(manifest: JSONObject) {
        resetForNewSource()
        player.setDataSource(manifest)
        player.prepare()
    }

    fun playManifest(manifestJson: String) = playManifest(JSONObject(manifestJson))

    /**
     * **播放对象（对象方式）**：直接传类型化的 MediaManifest（见
     * `com.cicada.player.manifest.MediaManifest` / `ManifestBuilder`）。
     *
     * 与 playManifest(String)/playManifest(JSONObject) 是**同一条路**：SDK 内部
     * `manifest.toJsonString()` 之后走既有的字符串入口，内核仍然只解析一次 JSON。
     * 好处是拼装清单时有类型检查与 IDE 补全，字段写错在编译期就报。
     */
    fun playManifest(manifest: MediaManifest) {
        resetForNewSource()
        player.setDataSource(manifest)
        player.prepare()
    }

    /** 地址 or 播放对象（`{` 开头按播放对象走） */
    fun playAny(input: String) {
        val text = input.trim()
        if (text.startsWith("{")) playManifest(text) else playUrl(text)
    }

    fun beginPreview(position: Long) {
        previewPositionMs = position
        previewVisible = true
    }

    fun endPreview() {
        previewVisible = false
    }

    private fun resetForNewSource() {
        firstFrameRendered = false
        errorMessage = null
        errorCode = 0
        loading = true
        /* 换源要把 seek 状态收掉：上一个源的在途 seek 不会再回 OnSeekComplete */
        seeking = false
        positionMs = 0L
        bufferedMs = 0L
        mediaInfo = null
        softwareDecoderNotice = null
        /* 换源会中止在途的清晰度切换（内核会发 CANCELED，但那一路继承的是旧片源的回调），
         * 这里先把提示收掉，避免"新片源刚起来就挂着上一部片的切换中提示"。 */
        switchingQuality = false
        switchingQualityIndex = TrackInfo.AUTO_SELECT_INDEX
        /* 终态确认提示也一起收掉：否则新片源刚起来会挂着上一部片的"已切换/失败"提示 */
        qualityNoticeStatus = QUALITY_NOTICE_NONE
        qualityNoticeIndex = TrackInfo.AUTO_SELECT_INDEX
        /* 换源时上一部片还没被取走的截图要释放：Bitmap 不回收就是白白占着内存（SDK 明确要求 recycle） */
        releaseSnapshot()
        /*
         * 【本轮新增：把"用户选的解码方式"在换源时补一次】
         *
         * 为什么要在这里补：内核 `SetDecoderType` 只写字段、不重建解码器，真正读它的是建视频路时的
         * `SetUpVideoPath()`（SuperMediaPlayer.cpp:9112-9136）。也就是说这条选项**必须在
         * setDataSource/prepare 之前**就是对的。放在 `resetForNewSource()`（下面三个 playXxx
         * 都在调它之后才 setDataSource）里= 顺序由这一个地方保证，不再依赖调用方记得先调
         * `enableHardwareDecoder`。这样"用户关了硬解 ⇒ 下一次起播真的是软解"是结构上成立的，
         * 不是靠某一句调用顺序凑出来的。
         */
        player.enableHardwareDecoder(hardwareDecodeEnabled)
        Log.i(TAG_DECODE, "source reset -> request hardware decode=$hardwareDecodeEnabled")
    }

    fun start() = player.start()
    fun pause() = player.pause()
    fun togglePlay() { if (isPlaying) pause() else start() }
    fun stop() = player.stop()
    fun reload() = player.reload()
    fun release() {
        /* 还没被取走的截图先回收：release 之后不会再有回调来接管它 */
        releaseSnapshot()
        player.setSurface(null)
        player.release()
    }

    fun seekTo(positionMs: Long, accurate: Boolean = true) {
        seeking = true
        player.seekTo(
            positionMs,
            if (accurate) CicadaPlayer.SeekMode.Accurate else CicadaPlayer.SeekMode.Inaccurate
        )
    }

    /*
     * 倍速 / 音量 / 静音：**下发即打一行**（tag `CicadaAudio`）。
     *
     * 为什么这几条值得打日志：它们原来**一行日志都没有**，而真机上"拖了音量条没反应/倍速不对"
     * 这类问题只能靠"到底下发了什么值"来分辨是 UI 没变还是内核没吃。
     * 事件驱动（用户一操作就写一行），**没有**任何计时器/轮询。
     * 【回退点 S1】把这三行 Log 删掉即可，其它行为不受影响。
     */
    fun setSpeed(speed: Float) {
        val v = speed.coerceIn(0.5f, 2.0f)
        Log.i(TAG_AUDIO, "setSpeed=$v")
        player.setSpeed(v)
    }

    fun setVolume(gain: Float) {
        val v = gain.coerceIn(0f, 2f)
        Log.i(TAG_AUDIO, "setVolume=$v")
        player.setVolume(v)
    }

    fun setMute(mute: Boolean) {
        Log.i(TAG_AUDIO, "setMute=$mute")
        player.setMute(mute)
    }

    /** 循环播放：**App 目前没有 UI 入口**（只有这个方法），保留接口 + 下发日志，便于以后接 UI 时可验。 */
    fun setLoop(loop: Boolean) {
        Log.i(TAG_AUDIO, "setLoop=$loop")
        player.setLoop(loop)
    }

    /**
     * 硬解 / 软解（`com.cicada.player.CicadaPlayer.enableHardwareDecoder`）。
     *
     * 【内核语义（只读确认，未改内核）】`NativePlayerBase.java:296-299` → JNI
     * `NativeBase.cpp:190-199` → `MediaPlayer::EnableHardwareDecoder`（MediaPlayer.cpp:196-206，
     * 只是把 bool 翻成 `DT_SOFTWARE/DT_HARDWARE`）→ `SuperMediaPlayer::SetDecoderType`
     * （SuperMediaPlayer.cpp:1282-1285）= **只写 `mSet->bEnableHwVideoDecode`**。
     * 它**不会立刻重建解码器**：读这个字段的是 `SetUpVideoPath()`（:9112-9136），
     * 也就是"每次建视频路/建解码器"时。⇒ **只对下一次起播（或下一次重建解码器）生效**。
     * 所以本方法现在**同时记住用户的意图**（[hardwareDecodeEnabled]），换源时自动补发一次。
     */
    fun enableHardwareDecoder(enable: Boolean) {
        hardwareDecodeEnabled = enable
        player.enableHardwareDecoder(enable)
        /*
         * 日志（tag `CicadaDecode`，**下发即打**、事件驱动）：与 CicadaVideoPlayer 起播前那条
         * 同 tag，真机 `adb logcat -s CicadaDecode` 能把"用户选择 → 下发 → 起播"整条链看全。
         */
        Log.i(TAG_DECODE, "enableHardwareDecoder=$enable (takes effect when the decoder is created)")
    }

    fun setScaleMode(mode: CicadaPlayer.ScaleMode) = player.setScaleMode(mode)
    fun setVideoBackgroundColor(argb: Int) = player.setVideoBackgroundColor(argb)

    /**
     * 【镜像画面 / 回退点 M1】把镜像开关交给**内核**渲染器（GL 的 OES / YUV 两条着色器路）。
     *
     * 链路（已验证可用）：
     *   `com.cicada.player.CicadaPlayer.setMirrorMode`（`CicadaPlayer.java:399`；参数是内核自带的
     *   `CicadaPlayer.MirrorMode` 枚举 `MIRROR_MODE_NONE / MIRROR_MODE_HORIZONTAL /
     *   MIRROR_MODE_VERTICAL`，定义在同文件 `:345-366`）
     *   → `NativePlayerBase.java:371-385` → JNI → `SuperMediaPlayer.cpp:1216-1224`
     *   → `SMPMessageControllerListener.cpp:470-473` 对**当前**渲染器 `setFlip`
     *   → `GLRender` → `OESProgramContext::updateFlip` / `YUVProgramContext::updateFlip`。
     * 硬解（OES）与软解（YUV）两条路都翻。改完**立刻生效**：不重设 Surface（不触发
     * ProcessSetViewMsg / RestartVideoDecoder）、不改尺寸、不碰解码与播放状态。
     *
     * 【已知限制（不解决，如实写在这里）】隧道 / direct 渲染（`FLAG_DUMMY`，HDR、Widevine 会被
     * 内核强制走那条）下渲染器是 `DummyVideoRender`，它的 `setFlip` 是**空实现** ⇒ 那类片源
     * 镜像**仍无效**。**不要**再拿 App 层的 View 变换去兜那条路 —— 本工程实际做过
     * `Modifier.graphicsLayer { scaleX = -1f }` 挂在承载画面的 `AndroidView` 上，真机点击后画面
     * 毫无变化：视频画在 **SurfaceView** 上、由 SurfaceFlinger 单独合成，Compose 图层的负缩放
     * 传不到它（那种做法的先天缺陷，不是参数问题）。
     *
     * 【截图】`snapshot()` 抓帧处会**临时补偿**内核自己的 flip，那是抓帧内部的补偿，所以存下来的
     * 截图不带镜像；本次改动不碰截图链路。
     *
     * 回退：删掉这个方法即可（调用方见 CicadaVideoPlayer.kt 镜像段注释里的同组回退点）。
     */
    fun setMirrorMode(mode: CicadaPlayer.MirrorMode) {
        /* 点击即打（事件驱动）：与 CicadaVideoPlayer 面板 onClick 里那条同 tag，便于真机 logcat 对时序 */
        Log.i("CicadaMirror", "controller.setMirrorMode -> ${mode.name}")
        player.setMirrorMode(mode)
    }

    /**
     * 【色觉辅助滤镜 / 回退点 K1】把 3x3 颜色矩阵交给**内核**渲染器（GL 的 OES / YUV 两条
     * 着色器路），与 `setMirrorMode` 同一层通道。
     *
     * 单位矩阵 = 关闭；[ColorVisionMode.matrix] 已保证"关闭"返回单位矩阵（见该枚举）。
     * 已知限制（不解决，写在这里免得以后当 bug 查）：
     *  · 隧道/direct 渲染（HDR、Widevine 被内核强制走那条，`DummyVideoRender`）**没有着色器**
     *    ⇒ 滤镜在这条路上无效。App 层不需要为它做什么特殊处理；
     *  · `snapshot()` 抓的是加滤镜之前的画面 ⇒ 存下来的截图**不带**滤镜（Qt 截图链路同源）。
     *
     * 【本轮补日志】以前这条链路**一行日志都没有**（镜像那边有、它没有），真机上"点了没变化"
     * 时根本分不清是"点击没到"、"没下发"还是"内核不吃"。现在下发处打一条 `CicadaColorVision`
     * （下发即打、事件驱动，无计时器）。
     */
    fun setColorMatrix(matrix: FloatArray) {
        Log.i("CicadaColorVision", "controller.setColorMatrix len=${matrix.size}")
        player.setColorMatrix(matrix)
    }

    /** 清晰度/字幕切档：内核按 index 选流（`TrackInfo.AUTO_SELECT_INDEX = -1` 表示自动切码率） */
    fun selectTrack(index: Int) = player.selectTrack(index)

    /** 当前选中的视频轨 / 字幕轨下标（-1 = 自动；来自内核回调，不猜） */
    var currentVideoIndex by mutableIntStateOf(TrackInfo.AUTO_SELECT_INDEX)
        private set
    var currentSubtitleIndex by mutableIntStateOf(TrackInfo.AUTO_SELECT_INDEX)
        private set

    /*
     * ---- 「解码方式」那一栏的两个事实读数（为了面板显示，按需现读）----
     *
     * 这两个是**只读快照**：`PlayerStats.from()` 每次重组时各取一次，跟着既有统计/信息
     * 刷新机制走 —— **不在这里缓存、不新增计时器**（理由写在 PlayerStats.from() 里）。
     */

    /**
     * 当前视频流（内核 `GetCurrentStreamInfo(ST_TYPE_VIDEO)` 的 Java 落点：
     * `currentTrack(TrackInfo.Type.TYPE_VIDEO)`）。
     *
     * **没有当前视频流时返回 null**（没有播放器 / 没有片源 / 还没解析出视频轨）——
     * 界面据此把「解码方式」整栏显示成 `--`。判据与 Qt 的 `refreshDecodeMethod()`
     * （platform/QtPlayer/src/CicadaPlayerItem.cpp:1092）完全一致：那边也是先看
     * `GetCurrentStreamInfo(ST_TYPE_VIDEO)` 拿到没有；拿不到就整栏为空 → 界面显示 "--"。
     *
     * 编码短名请用返回值上的 `getVideoCodec()`（显式 getter：`TrackInfo` 同时有公有字段
     * 与 getter，Kotlin 属性语法可能歧义）。**认不出来时它是空串**，界面此时不写编码。
     */
    fun currentVideoTrack(): TrackInfo? = player.currentTrack(TrackInfo.Type.TYPE_VIDEO)

    /**
     * 活动视频解码器**实际在用硬解还是软解**的事实读数。
     *
     * 链路：内核 `IDecoder::isHardwareDecoderInUse()`
     * → `MediaPlayer::IsVideoDecoderHardware()` → C API `CicadaIsVideoDecoderHardware(player)`
     * → JNI `NativePlayerBase.nIsVideoDecoderHardware()`（本轮新增）
     * → `CicadaPlayer.isVideoDecoderHardware()`。
     *
     * 与"设备/构建支持不支持硬解"（`getVideoCodecSupport()` 那份 JSON 里的 hwDecode）
     * **不是一件事**：这里回答的是眼下真正走的那条路，所以"建解码器时硬解没能起来"和
     * "解码途中退回软解"之后都会是 false —— 界面显示的正是这个。
     * 没有播放器 / 没有视频解码器时返回 false。
     */
    fun isVideoDecoderHardware(): Boolean = player.isVideoDecoderHardware()

    /**
     * 用户**意图**是不是"自动清晰度"（内核 ABR）。
     *
     * 必须和 [currentVideoIndex] 分开：自动档下内核会把 ABR 自己选中的**真实**档位
     * 报回来（onChangedSuccess → currentVideoIndex = 0/3/7…），如果拿它当选中态，
     * 面板会在自动档运行时高亮某一档具体清晰度 —— 与 Qt 的
     * qualityIndex（内核实际档位）/ autoQuality（用户意图）两个属性同一个划分
     * （见 platform/QtPlayer/src/CicadaPlayerItem.h:302、:311）。
     *
     * 默认 true = 起播默认自动档，和 Qt 的 `m_autoQuality = true` 初值一致。
     */
    var autoQuality by mutableStateOf(true)
        private set

    /** 字幕是否开着（关闭字幕那一行用） */
    var subtitlesEnabled by mutableStateOf(false)
        private set

    /** 切档失败提示（内核 onChangedFail 给的 errorInfo.msg） */
    var trackChangeMessage by mutableStateOf<String?>(null)
        private set

    /**
     * 最新一张截图（内核 `OnSnapShotListener` 回调送来的，已在主线程）。
     *
     * 界面通过 [takeSnapshot] 取走所有权；**只保留最新一张**（见 registerListeners 里的说明）。
     * 内存纪律：SDK 要求用完 recycle（CicadaPlayer.java:1298/1305），所以这里的引用是
     * "待消费"队列长度为 1，不是可以随手丢的缓存。
     */
    var snapshotBitmap by mutableStateOf<Bitmap?>(null)
        private set

    /**
     * 截图结果的版本号：每来一张新图 +1。
     *
     * 界面用 `LaunchedEffect(snapshotSerial)` 等"新图到达"这件事 —— 不能只看
     * snapshotBitmap 是不是非空，否则同一张图会被反复消费。
     */
    var snapshotSerial by mutableIntStateOf(0)
        private set

    /** 清晰度切档：index 传 `TrackInfo.AUTO_SELECT_INDEX` 就是"自动"（见 [selectAutoQuality]） */
    fun selectQuality(index: Int) {
        if (index == TrackInfo.AUTO_SELECT_INDEX) {
            selectAutoQuality()
            return
        }
        /* 手动选档 = 退出自动（内核在手动切流时自己会把 ABR 关掉，
         * MediaPlayer::SelectTrack 对视频流调 mAbrManager->EnableAbr(false)） */
        autoQuality = false
        /*
         * 【回退点 A1】这里原来还有一行 `currentVideoIndex = index`（点击即"乐观置位"）。
         * 本轮删掉：**高亮不在点击瞬间改**，只有内核确认新档位真的切过去之后，
         * onChangedSuccess 那条回调（见下面第二个 init 块）才把 currentVideoIndex 写成新值。
         *
         * 依据：Qt 的 selectQuality 就是"只 SelectTrack + 把 autoQuality 置假"，
         * m_qualityIndex 明确**不在点击瞬间修改**，要等 notifyStreamSwitched()
         * （platform/QtPlayer/src/CicadaPlayerItem.cpp:2380-2383）。
         * 这样"点了但内核没切成"时面板不会先亮到新档位再退回旧档位。
         *
         * 【回退方法】把 `currentVideoIndex = index` 这一行加回来即可恢复旧行为。
         */
        player.selectTrack(index)
    }

    /**
     * 回到"自动清晰度"（内核 ABR）—— 与 Qt 的 CicadaPlayerItem::useAutoQuality()
     * （platform/QtPlayer/src/CicadaPlayerItem.cpp:2389）一一对应：
     * 都是"SelectTrack(自动) + 把用户意图置回自动"。
     *
     * 这是**唯一**会打开内核 ABR 的入口：内核 MediaPlayer::SelectTrack 只在
     * `index == SELECT_TRACK_VIDEO_AUTO`（-1）时 `mAbrManager->EnableAbr(true)`
     * （mediaPlayer/MediaPlayer.cpp:312）。界面写着"自动"却不调它，就等于：
     * 起播被内核按 mDefaultBandWidth=0 挑成最低档，而且 ABR 线程永不启动
     * —— 用户报的"默认自动清晰度从来不升档"正是这一条。
     */
    fun selectAutoQuality() {
        autoQuality = true
        player.selectTrack(TrackInfo.AUTO_SELECT_INDEX)
    }

    /** 字幕切档：真实字幕轨用 `selectTrack(index)`；关闭字幕见下面的说明 */
    fun selectSubtitle(index: Int) {
        if (index == TrackInfo.AUTO_SELECT_INDEX) {
            closeSubtitle()
        } else {
            subtitlesEnabled = true
            currentSubtitleIndex = index
            player.selectTrack(index)
        }
    }

    /**
     * 关闭字幕。
     *
     * 【如实说明】内核 Java 接口里**没有**"关闭嵌入字幕轨"的独立方法（只有外挂字幕的
     * `selectExtSubtitle(trackIndex, false)`）。这里按外挂字幕的语义调用
     * `selectExtSubtitle(currentSubtitleIndex, false)`；内核若不支持，行为由内核决定 ——
     * **不在这里伪造"已关闭"以外的效果**（UI 的选中态只反映用户的选择）。
     */
    fun closeSubtitle() {
        subtitlesEnabled = false
        if (currentSubtitleIndex >= 0) {
            runCatching { player.selectExtSubtitle(currentSubtitleIndex, false) }
        }
    }

    /*
     * 切档结果回调 → 面板选中态 + 失败提示（内核 onChangedSuccess / onChangedFail）。
     *
     * 【为什么这里加一行 Log】A1 之后**面板高亮只由这条回调驱动**（点击不再乐观置位），
     * 而这条回调原来一行日志都没有 —— 一旦真机上"点了没反应"，没有日志就无法判定是
     * "内核没回调"还是"界面没画对"。链路：内核 StreamSwitchSuc
     * （mediaPlayer/MediaPlayer.cpp:1020 streamChangedSucCallback，ST_TYPE_VIDEO 也走这里）
     * → NativeBase.cpp:1661 → NativePlayerBase.onSwitchStreamSuccess → 这里。日志过滤 `CicadaTrack`。
     */
    init {
        player.setOnTrackChangedListener(object : CicadaPlayer.OnTrackChangedListener {
            override fun onChangedSuccess(trackInfo: TrackInfo) {
                trackChangeMessage = null
                /* 用显式 getter：TrackInfo 同时有公有字段与 getter，Kotlin 属性语法可能歧义 */
                Log.i(TAG_TRACK_CHANGE, "onChangedSuccess type=${trackInfo.getType()} index=${trackInfo.getIndex()}")
                when (trackInfo.getType()) {
                    TrackInfo.Type.TYPE_VIDEO -> currentVideoIndex = trackInfo.getIndex()
                    TrackInfo.Type.TYPE_SUBTITLE -> currentSubtitleIndex = trackInfo.getIndex()
                    else -> Unit
                }
            }

            override fun onChangedFail(trackInfo: TrackInfo, errorInfo: ErrorInfo) {
                Log.i(TAG_TRACK_CHANGE, "onChangedFail type=${trackInfo.getType()} msg=${errorInfo.msg}")
                trackChangeMessage = errorInfo.msg ?: "切档失败"
            }
        })
    }
    /**
     * 截图（快进预览图的取帧入口：内核只有"截当前帧"，按时间取帧要等内核提供）。
     *
     * 结果**不在这里**同步返回：内核抓帧是异步的，图从
     * [snapshotBitmap] / [snapshotSerial]（见 registerListeners 里的 setOnSnapShotListener）出来。
     * 界面发起的姿势：先 [snapshot]，再用 `LaunchedEffect(snapshotSerial)` 等图 —— 串号变了才算新图。
     */
    fun snapshot() = player.snapshot()

    /**
     * 最新一张截图（**所有权归调用方**：取走后本类不再持有，下一张到达时不会把它回收）。
     *
     * 取到之后由调用方负责 recycle（写盘结束、且不再画到界面上时）。
     */
    fun takeSnapshot(): Bitmap? {
        val b = snapshotBitmap
        snapshotBitmap = null
        return b
    }

    /**
     * 释放还没被取走的截图（换源/退出时用）。
     * **不动串号** [snapshotSerial]：串号是"新图到达"的判据，回收不产生新图。
     */
    fun releaseSnapshot() {
        snapshotBitmap?.recycle()
        snapshotBitmap = null
    }

    companion object {
        /** 安卓侧切档状态日志的 tag（见 onVideoQualitySwitch 回调里的 Log.i） */
        private const val TAG_QUALITY_SWITCH = "CicadaQuality"

        /**
         * 解码方式（硬解/软解）日志的 tag。
         * 与 `CicadaVideoPlayer` 起播前那条 `CicadaDecode` **同 tag**：真机
         * `adb logcat -s CicadaDecode` 一次就能看到"用户选择 → 下发 → 起播"三步。
         */
        private const val TAG_DECODE = "CicadaDecode"

        /** 倍速/音量/静音/循环的日志 tag（见 setSpeed / setVolume / setMute / setLoop） */
        private const val TAG_AUDIO = "CicadaAudio"

        /** 安卓侧切档**结果**日志的 tag（见 onChangedSuccess / onChangedFail 里的 Log.i） */
        private const val TAG_TRACK_CHANGE = "CicadaTrack"

        /*
         * 切档终态提示的状态码（UI 据此选文案）。用常量而不是裸数字：
         * UI 侧写成 `CicadaPlayerController.QUALITY_NOTICE_READY` 一眼能看懂。
         */
        const val QUALITY_NOTICE_NONE = 0
        const val QUALITY_NOTICE_READY = 1
        const val QUALITY_NOTICE_FAILED = 2
        const val QUALITY_NOTICE_CANCELED = 3
    }
}
