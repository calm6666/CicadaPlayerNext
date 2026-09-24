package com.cicada.player.compose.components.video

import com.cicada.player.compose.player.CicadaPlayerController
import com.cicada.player.nativeclass.MediaInfo
import com.cicada.player.nativeclass.TrackInfo

/**
 * 「视频信息」面板的数据（**全部来自本工程内核的真实读数**，不引 Media3）。
 *
 * 来源：`CicadaPlayerController` 的 state（位置/缓冲/网速/分辨率/硬解回退提示）+
 * `MediaInfo`（`OnTrackReadyListener` 回来的流信息）。
 */
data class PlayerStats(
    val widthPx: Int = 0,
    val heightPx: Int = 0,
    val sampleMimeType: String = "",
    val decoderName: String = "",
    val frameRate: Float = 0f,
    val bitrate: Int = 0,
    val droppedFrames: Int = 0,
    val bufferedMs: Long = 0,
    val positionMs: Long = 0,
    val durationMs: Long = 0,
    val speedBps: Long = 0,
) {
    fun resolutionText(): String =
        if (widthPx <= 0 || heightPx <= 0) "--"
        else "$widthPx x $heightPx" + (if (frameRate > 0f) "@%.3f".format(frameRate) else "")

    fun codecText(): String = sampleMimeType.ifEmpty { "--" }

    /** 硬解 / 软解（判据：解码器名以 c2.android./omx.google. 开头或含 .sw./ffmpeg 为软解） */
    fun decoderText(): String {
        if (decoderName.isEmpty()) return "--"
        val lower = decoderName.lowercase()
        val software = lower.startsWith("c2.android.") || lower.startsWith("omx.google.") ||
            lower.contains(".sw.") || lower.contains("ffmpeg")
        return if (software) "软解" else "硬解"
    }

    fun decoderNameText(): String = decoderName.ifEmpty { "--" }
    fun frameRateText(): String = if (frameRate > 0f) "%.2f fps".format(frameRate) else "--"
    fun bitrateText(): String = if (bitrate > 0) "${bitrate / 1000} kbps" else "--"
    fun droppedText(): String = if (droppedFrames > 0) "$droppedFrames" else "0"
    fun bufferText(): String = if (bufferedMs > 0) "${bufferedMs / 1000} s" else "--"
    fun positionText(): String = formatPlayerTime(positionMs) + " / " + formatPlayerTime(durationMs)
    fun speedText(): String = when {
        speedBps <= 0 -> "--"
        speedBps >= 1024 * 1024 -> "%.2f MB/s".format(speedBps / 1024.0 / 1024.0)
        else -> "%.1f KB/s".format(speedBps / 1024.0)
    }

    companion object {
        /** 从内核控制器取快照（取不到的字段保持默认 → 面板显示 "--"） */
        fun from(controller: CicadaPlayerController): PlayerStats = PlayerStats(
            widthPx = controller.videoWidth,
            heightPx = controller.videoHeight,
            bufferedMs = (controller.bufferedMs - controller.positionMs).coerceAtLeast(0L),
            positionMs = controller.positionMs,
            durationMs = controller.durationMs,
            speedBps = controller.downloadSpeedBps,
            /* 硬解回退成软解时内核会通过 InfoCode 给一条说明，这里如实显示 */
            decoderName = controller.softwareDecoderNotice.orEmpty(),
        )
    }
}

/** 清晰度一档（来自 `TrackInfo` 的视频轨） */
data class QualityOption(
    val index: Int,
    val label: String,
    val selected: Boolean,
)

/** 字幕一档（来自 `TrackInfo` 的字幕轨） */
data class SubtitleOption(
    val index: Int,
    val label: String,
    val selected: Boolean,
)

/**
 * 由内核 `MediaInfo` 组清晰度列表。
 *
 * 真实字段（已核 `nativeclass/TrackInfo.java`）：`index`、`mType`(`Type.TYPE_VIDEO`)、
 * `videoWidth`/`videoHeight`/`videoBitrate`；**`TrackInfo.AUTO_SELECT_INDEX = -1`** 表示自动切码率。
 * 只有一档视频轨时按用户要求**只显示"自动"**。
 */
fun buildQualities(mediaInfo: MediaInfo?, currentIndex: Int): List<QualityOption> {
    /* 用显式 getter：TrackInfo 同时有公有字段与 getter，Kotlin 属性语法可能歧义 */
    val videos = mediaInfo?.trackInfos?.filter {
        it.getType() == TrackInfo.Type.TYPE_VIDEO
    }.orEmpty()

    val auto = QualityOption(
        index = TrackInfo.AUTO_SELECT_INDEX,
        label = "自动",
        selected = currentIndex == TrackInfo.AUTO_SELECT_INDEX || videos.size <= 1,
    )
    if (videos.size <= 1) return listOf(auto)

    return listOf(auto) + videos.map { t ->
        val w = t.getVideoWidth()
        val h = t.getVideoHeight()
        val res = if (w > 0 && h > 0) "$w x $h" else "未知分辨率"
        val br = if (t.getVideoBitrate() > 0) "  ${t.getVideoBitrate() / 1000} kbps" else ""
        QualityOption(index = t.getIndex(), label = res + br, selected = t.getIndex() == currentIndex)
    }
}

/**
 * 由内核 `MediaInfo` 组字幕列表；第一行是「关闭字幕」。
 * 名字优先 `getSubtitleLang()`，其次 `getDescription()`，都没有就 `字幕 N`。
 */
fun buildSubtitles(mediaInfo: MediaInfo?, currentIndex: Int, subtitlesEnabled: Boolean): List<SubtitleOption> {
    val subs = mediaInfo?.trackInfos?.filter {
        it.getType() == TrackInfo.Type.TYPE_SUBTITLE
    }.orEmpty()

    val off = SubtitleOption(
        index = TrackInfo.AUTO_SELECT_INDEX,
        label = "关闭字幕",
        selected = !subtitlesEnabled || subs.isEmpty(),
    )

    return listOf(off) + subs.mapIndexed { i, t ->
        val lang = t.getSubtitleLang()
        val desc = t.getDescription()
        val name = lang?.takeIf { it.isNotBlank() }
            ?: desc?.takeIf { it.isNotBlank() }
            ?: "字幕 ${i + 1}"
        SubtitleOption(
            index = t.getIndex(),
            label = name,
            selected = subtitlesEnabled && t.getIndex() == currentIndex,
        )
    }
}
