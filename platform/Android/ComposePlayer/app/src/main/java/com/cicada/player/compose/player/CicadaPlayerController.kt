package com.cicada.player.compose.player

import android.content.Context
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
import com.cicada.player.nativeclass.MediaInfo
import com.cicada.player.nativeclass.TrackInfo
import org.json.JSONObject

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

    /** prepare 完成后是否自动 start */
    var autoPlayWhenPrepared: Boolean = true

    /** 最近一次拖动的目标位置（快进预览图用；松手后清空） */
    var previewPositionMs by mutableLongStateOf(0L)
        private set
    var previewVisible by mutableStateOf(false)
        private set

    init {
        registerListeners()
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
            isPlaying = false
        }
        player.setOnTrackReadyListener { info -> mediaInfo = info }

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
        positionMs = 0L
        bufferedMs = 0L
        mediaInfo = null
        softwareDecoderNotice = null
    }

    fun start() = player.start()
    fun pause() = player.pause()
    fun togglePlay() { if (isPlaying) pause() else start() }
    fun stop() = player.stop()
    fun reload() = player.reload()
    fun release() {
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

    fun setSpeed(speed: Float) = player.setSpeed(speed.coerceIn(0.5f, 2.0f))
    fun setVolume(gain: Float) = player.setVolume(gain.coerceIn(0f, 2f))
    fun setMute(mute: Boolean) = player.setMute(mute)
    fun setLoop(loop: Boolean) = player.setLoop(loop)
    fun enableHardwareDecoder(enable: Boolean) = player.enableHardwareDecoder(enable)
    fun setScaleMode(mode: CicadaPlayer.ScaleMode) = player.setScaleMode(mode)
    fun setVideoBackgroundColor(argb: Int) = player.setVideoBackgroundColor(argb)

    /** 清晰度/字幕切档：内核按 index 选流（`TrackInfo.AUTO_SELECT_INDEX = -1` 表示自动切码率） */
    fun selectTrack(index: Int) = player.selectTrack(index)

    /** 当前选中的视频轨 / 字幕轨下标（-1 = 自动；来自内核回调，不猜） */
    var currentVideoIndex by mutableIntStateOf(TrackInfo.AUTO_SELECT_INDEX)
        private set
    var currentSubtitleIndex by mutableIntStateOf(TrackInfo.AUTO_SELECT_INDEX)
        private set

    /** 字幕是否开着（关闭字幕那一行用） */
    var subtitlesEnabled by mutableStateOf(false)
        private set

    /** 切档失败提示（内核 onChangedFail 给的 errorInfo.msg） */
    var trackChangeMessage by mutableStateOf<String?>(null)
        private set

    /** 清晰度切档：index 传 `TrackInfo.AUTO_SELECT_INDEX` 就是"自动" */
    fun selectQuality(index: Int) {
        currentVideoIndex = index
        player.selectTrack(index)
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

    /* 切档结果回调 → 面板选中态 + 失败提示（内核 onChangedSuccess / onChangedFail） */
    init {
        player.setOnTrackChangedListener(object : CicadaPlayer.OnTrackChangedListener {
            override fun onChangedSuccess(trackInfo: TrackInfo) {
                trackChangeMessage = null
                /* 用显式 getter：TrackInfo 同时有公有字段与 getter，Kotlin 属性语法可能歧义 */
                when (trackInfo.getType()) {
                    TrackInfo.Type.TYPE_VIDEO -> currentVideoIndex = trackInfo.getIndex()
                    TrackInfo.Type.TYPE_SUBTITLE -> currentSubtitleIndex = trackInfo.getIndex()
                    else -> Unit
                }
            }

            override fun onChangedFail(trackInfo: TrackInfo, errorInfo: ErrorInfo) {
                trackChangeMessage = errorInfo.msg ?: "切档失败"
            }
        })
    }
    /** 截图（快进预览图的取帧入口：内核只有"截当前帧"，按时间取帧要等内核提供） */
    fun snapshot() = player.snapshot()
}
