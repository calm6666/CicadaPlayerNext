package com.cicada.player.compose.components.video

import com.cicada.player.compose.player.CicadaPlayerController
import com.cicada.player.nativeclass.MediaInfo
import com.cicada.player.nativeclass.TrackInfo
import com.cicada.player.utils.HardwareNameUtil

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
    /*
     * ---- 「解码方式」那一栏的三个读数（**全部是内核事实，一个都不猜**）----
     *
     * 默认值就是"没有播放器 / 没有当前视频流"那一档：hasVideoStream = false ⇒ 整栏 "--"。
     * 拼装文案见 [decodeMethodText]，数据来源见 companion 里的 from()。
     */
    /** 当前视频流的编码短名（内核规范名 "H.264"/"H.265"/…）；**空串 = 内核认不出** */
    val videoCodecShort: String = "",
    /** 有没有"当前视频流"（内核 `currentTrack(TYPE_VIDEO)` 非空）；false ⇒ 整栏显示 "--" */
    val hasVideoStream: Boolean = false,
    /** 活动解码器**实际**是否在硬解（事实读数；不是"配置想用哪种"、也不是"设备支持不支持"） */
    val hardwareDecoder: Boolean = false,
    /** 硬件名（SoC/硬件/板级型号）；**空串 = 拿不到 ⇒ 不写括号**。软解时本字段不参与拼装 */
    val hardwareName: String = "",
) {
    fun resolutionText(): String =
        if (widthPx <= 0 || heightPx <= 0) "--"
        else "$widthPx x $heightPx" + (if (frameRate > 0f) "@%.3f".format(frameRate) else "")

    fun codecText(): String = sampleMimeType.ifEmpty { "--" }

    /**
     * 【「解码方式」的最终显示文案】—— 与 Qt 的 `refreshDecodeMethod()` 同一套口径
     * （platform/QtPlayer/src/CicadaPlayerItem.cpp:1087-1133，那边是权威标杆）：
     *
     *     硬解 HEVC（SM8650）   硬解 + 编码显示名 + （硬件名）
     *     硬解 HEVC             拿不到硬件名 ⇒ **不写括号**
     *     硬解 / 软解           内核认不出编码 ⇒ 只写硬解/软解，不写"未知"、不猜
     *     软解 H.264            **软解绝不写型号**（解码根本没在编解码单元上跑）
     *     --                    没有播放器 / 没有当前视频流
     *
     * 编码那一段走**同一处**短名→显示名映射 [codecDisplayNameOf]（H.265 → HEVC，其余原样），
     * 这里不另写一套；内部标识/分组键仍旧用内核短名。
     *
     * 三个读数全部由 `PlayerStats.from()` 从内核取（活动解码器的事实读数 + 当前视频流的
     * 编码 + 设备硬件名），本函数只负责拼文案，不做任何推断。
     */
    fun decodeMethodText(): String {
        /* 没有当前视频流（含没有播放器）：按用户要求整栏 "--"，不写"软解"也不写"未知" */
        if (!hasVideoStream) return "--"

        val codec = codecDisplayNameOf(videoCodecShort)

        return if (hardwareDecoder) {
            when {
                codec.isEmpty() -> "硬解"
                hardwareName.isEmpty() -> "硬解 $codec"
                else -> "硬解 $codec（$hardwareName）"
            }
        } else {
            /* 软解：**绝不写型号** —— 写上会让人以为硬解生效了 */
            if (codec.isEmpty()) "软解" else "软解 $codec"
        }
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
        fun from(controller: CicadaPlayerController): PlayerStats {
            /*
             * 【「解码方式」三个读数：每次重组重读一次，**不新增计时器**】
             *
             * 本函数是 `CicadaVideoPlayer` 里 `PlayerPanel.Info` 那一支的实参，函数体里读的是
             * controller 上的 Compose state（positionMs / bufferedMs / mediaInfo /
             * softwareDecoderNotice …）。内核每送一条 InfoCode.CurrentPosition（以及切档、
             * 硬解回退提示、视频尺寸变化）都会让那次重组重跑这里 —— 这就是安卓既有的统计刷新
             * 机制（旧的"解码方式"那一栏也是这么刷的，见 PlayerPanels.kt 的"视频信息"面板），
             * 所以这里照旧按需现读，**不加任何计时器**。
             *
             * 硬解/软解与"当前视频流"这两项必须是**内核事实读数**、且必须现读：
             * 内核那条"硬解退回软解"（InfoCode.SwitchToSoftwareVideoDecoder）不会改任何已经
             * 缓存在 controller 上的状态，只有现读才能跟着变。
             */
            val currentVideo = controller.currentVideoTrack()
            val hardware = controller.isVideoDecoderHardware()
            /* 编码短名用显式 getter（TrackInfo 同时有公有字段与 getter，属性语法可能歧义） */
            val codecShort = (currentVideo?.getVideoCodec() ?: "").trim()

            return PlayerStats(
                widthPx = controller.videoWidth,
                heightPx = controller.videoHeight,
                bufferedMs = (controller.bufferedMs - controller.positionMs).coerceAtLeast(0L),
                positionMs = controller.positionMs,
                durationMs = controller.durationMs,
                speedBps = controller.downloadSpeedBps,
                /* 硬解回退成软解时内核会通过 InfoCode 给一条说明，这里如实显示 */
                decoderName = controller.softwareDecoderNotice.orEmpty(),
                videoCodecShort = codecShort,
                hasVideoStream = currentVideo != null,
                hardwareDecoder = hardware,
                /*
                 * 硬件名：安卓没有"显卡型号"这种直读接口，取证分级与 API 级别要求见
                 * HardwareNameUtil 的类注释。三个实参分别是：
                 *   · 第 1 个 = "在**已有** GL 上下文的线程上读到的 GL_RENDERER"。
                 *     本工程没有这样的地方可读（唯一那份 EGL/GL 上下文在内核渲染线程里，
                 *     Java 侧拿不到），而且**不许**为了显示一行字去新建上下文/线程 ⇒ null
                 *     （第 1 级因此暂时取不到值）；
                 *   · 第 2 个 = 活动解码器是否真的在硬解（内核事实读数）—— 软解时它会挡掉
                 *     第 3 级，这就是"软解绝不写型号"的落实点；
                 *   · 第 3 个 = 当前视频流的**内核编码短名**，只在第 1、2 级都拿不到时才被
                 *     用来按 mime 查本机硬解组件名（MediaCodecInfo.getName()）。
                 * 于是常见设备上取到的是第 2 级：Build.SOC_MODEL（API 31+）→ HARDWARE →
                 * BOARD；都拿不到时落到第 3 级；仍拿不到就是空串 ⇒ 界面只写"硬解 <编码>"，
                 * 不写括号。**没有任何一级是猜的**（详见 HardwareNameUtil 的类注释）。
                 */
                hardwareName = HardwareNameUtil.resolve(null, hardware, codecShort),
            )
        }
    }
}

/** 清晰度一档（来自 `TrackInfo` 的视频轨） */
data class QualityOption(
    val index: Int,
    val label: String,
    val selected: Boolean,
    /*
     * 这一档的**编码显示名**（徽标文本）：来自 `TrackInfo.getVideoCodec()`（内核归一化后的
     * 短名 "H.264"/"H.265"/"AV1"…），再经 `codecDisplayNameOf()` 换成用户认得的写法
     * （H.265 → **HEVC**；其余原样）。**内核认不出来时是空串**，界面此时不渲染徽标
     * （不写"未知"、也不按分辨率/码率反推）。默认空串：只改文案的调用方不必关心它。
     */
    val codec: String = "",
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
 *
 * [autoQuality] 是**用户意图**（内核 ABR 开着），[currentIndex] 是内核回报的**实际**档位。
 * 两者必须分开判：自动档下内核会把 ABR 自己选中的真实档位报回来（0/3/7…），只按
 * currentIndex 高亮的话，"自动"这一行会在 ABR 运行时失去高亮、而某一档具体清晰度被点亮
 * —— 那正是"只有标签、实际手动"的界面来源。Qt 端同样是两个属性
 * （CicadaPlayerItem.h:302 qualityIndex / :311 autoQuality）。
 */
fun buildQualities(mediaInfo: MediaInfo?, currentIndex: Int, autoQuality: Boolean): List<QualityOption> {
    /* 用显式 getter：TrackInfo 同时有公有字段与 getter，Kotlin 属性语法可能歧义 */
    val videos = mediaInfo?.trackInfos?.filter {
        it.getType() == TrackInfo.Type.TYPE_VIDEO
    }.orEmpty()

    /* 只有一档视频轨：按用户要求只显示"自动"一行（不带实际清晰度的括号） */
    if (videos.size <= 1) {
        return listOf(
            QualityOption(
                index = TrackInfo.AUTO_SELECT_INDEX,
                label = "自动",
                selected = true,
            )
        )
    }

    /*
     * ============ 照 Qt 的 onMediaInfoGetCb 逐条移植 ============
     * 标杆：platform/QtPlayer/src/CicadaPlayerItem.cpp:522-580（合并 + 排序）、:470-491（文案）。
     *
     * ① 分组键 = **分辨率 + 编码**（qualityGroupKey，Qt :535-550 的 "h1080|H.265" 同一口径）：
     *    · 同分辨率**不同编码**（DASH 下同一分辨率常同时有 H.264 与 H.265）→ **两行独立条目**，
     *      各带自己的编码徽标、各切各的流 —— 这正是用户要的"不要再合并成一条"；
     *    · 同分辨率**同编码**若仍有多条（同一套编码挂多条流）→ 合成一条，取带宽最大者当代表；
     *    · 编码为空串（内核没解析出来）→ 分组键的编码部分为空，退回"只按分辨率合并"，
     *      和上一版行为一样（拿不到编码就不假装能区分）。
     *    为什么必须要分组：内核 Java 侧是原样把全部视频流都给出来的
     *    （cicadaplayer/.../jni/player/JavaTrackInfo.cpp:195-209），同一编码的多条流不合并，
     *    列表里就会出现"1080P / 1080P"这种真重复项。
     * ② 合并后按**带宽降序**排（Qt :558-561）。
     * ③ "自动"追加到**末尾**（Qt 控制栏是各档在前、自动在最后：RightControls.qml:1784-1790），
     *    而不是像以前那样放在第一行。
     */
    val merged = videos
        .groupBy { qualityGroupKey(it) }
        .map { (_, group) -> group.maxByOrNull { it.getVideoBitrate() } ?: group.first() }
        .sortedByDescending { it.getVideoBitrate() }

    /*
     * 自动档在 ABR 已经选出实际档位之后带上括号显示它 —— 与 Qt 控制栏的
     * "自动（实际清晰度）"同一个意思（RightControls.qml 的自动档那一行）。
     * 这样用户不用去翻日志就能看出"自动"是不是真的在动（这正是 ① 的验收点）。
     */
    val actual = if (autoQuality) merged.firstOrNull { it.getIndex() == currentIndex } else null
    val auto = QualityOption(
        index = TrackInfo.AUTO_SELECT_INDEX,
        label = if (actual != null) "自动（${qualityLabelOf(actual)}）" else "自动",
        selected = autoQuality,
    )

    return merged.map { t ->
        QualityOption(
            index = t.getIndex(),
            label = qualityLabelOf(t),
            selected = !autoQuality && t.getIndex() == currentIndex,
            /* 徽标是**显示**，所以走短名→显示名映射（H.265 → HEVC）；见 codecDisplayNameOf。 */
            codec = codecDisplayNameOf(codecLabelOf(t)),
        )
    } + auto
}

/**
 * 合并分组的键：**分辨率 + 编码**（例如 `"h1080|H.265"`，与 Qt 的分组键同一口径）。
 *
 * 分辨率那半优先高度，其次宽度，都没有就按各自 index 单独成组（Qt 同兜底）；
 * 编码那半取归一化短名，**空串就是"未知"** —— 此时同分辨率的未知编码流仍旧并成一条，
 * 等于回到上一版"只按分辨率合并"的行为。两组之间用 `"|"` 分隔，避免和短名里的字符混淆。
 */
private fun qualityGroupKey(track: TrackInfo): String {
    val h = track.getVideoHeight()
    val w = track.getVideoWidth()
    val resolution = when {
        h > 0 -> "h$h"
        w > 0 -> "w$w"
        else -> "#${track.getIndex()}"
    }
    return "$resolution|${codecLabelOf(track)}"
}

/**
 * 一档视频轨的**编码短名**（内核的规范名，徽标文本的原料）——
 * 直接取内核归一化后的名字（`TrackInfo.getVideoCodec()`）。
 *
 * **认不出来时内核给空串**，这里原样返回空串（界面按"空串 = 不渲染徽标"处理）：
 * 不做任何映射/猜测，既不显示"未知"，也不按分辨率/码率反推编码。
 * 用显式 getter（TrackInfo 同时有公有字段与 getter，Kotlin 属性语法可能歧义）。
 *
 * ⚠ 本函数**同时是合并分组键的一半**（见 [qualityGroupKey]），所以它必须保持"内核短名"这个
 * 身份口径 —— **显示名不要塞进这里**，否则以后谁改一句文案就可能悄悄把两档并成一档。
 * 徽标要显示成用户认得的写法，请再过一道 [codecDisplayNameOf]。
 */
fun codecLabelOf(track: TrackInfo): String =
    (track.getVideoCodec() ?: "").trim()

/**
 * 【编码短名 → 显示名】整个 App 唯一的一处映射，**只用于显示**（对应 Qt 的
 * `CicadaPlayerItem.cpp` 里的 `codecDisplayName()`，两处口径必须一致）：
 *
 *     H.265 → HEVC；其余（H.264 / AV1 / VP9 / MPEG-4 / MPEG-2 / 空串 …）原样返回。
 *
 * 内核（以及 `TrackInfo.getVideoCodec()`、合并分组键）用的是**规范短名** "H.265"；
 * 对用户说 HEVC 才是通行的叫法（`硬解 HEVC（显卡名）`）。所以只在**显示**这一层换名。
 *
 * ⚠ 别拿它当内部标识用：分组 [qualityGroupKey]、认轨、切档比较一律继续用
 * [codecLabelOf]（内核短名）。空串仍返回空串 —— "认不出来"的语义不变，界面照旧不画徽标。
 */
fun codecDisplayNameOf(codec: String): String {
    val trimmed = codec.trim()

    return if (trimmed == "H.265") "HEVC" else trimmed
}

/**
 * 一档视频轨的显示名 —— 与 Qt 完全一致的写法（`CicadaPlayerItem.cpp:470-491`）：
 *   2160 及以上 → `4K`；1440 及以上 → `2K`；有高度 → `1080P`；
 *   没高度 → `宽 x 高`；连宽高都没有 → `kbps`；都没有 → `未知`。
 * 清晰度列表、切换提示、按钮文案共用这一套。
 */
fun qualityLabelOf(track: TrackInfo): String {
    val w = track.getVideoWidth()
    val h = track.getVideoHeight()
    val kbps = track.getVideoBitrate() / 1000
    return when {
        h >= 2160 -> "4K"
        h >= 1440 -> "2K"
        h > 0 -> "${h}P"
        w > 0 -> if (h > 0) "$w x $h" else "$w"
        kbps > 0 -> "$kbps kbps"
        else -> "未知"
    }
}

/**
 * 按内核给的视频流下标找档位名（清晰度切换提示的文案用）。
 *
 * 下标来自内核 `PLAYER_QUALITY_SWITCH_STARTED` 事件的 `streamIndex`，与
 * `TrackInfo.getIndex()` 同一套编号（Qt 端 `PlayerView.qml` 的
 * `qualityLabelForStream()` 做的是同一件事）。找不到就返回 null，界面退回通用文案。
 */
fun qualityLabelFor(mediaInfo: MediaInfo?, index: Int): String? {
    if (index < 0) return null
    return mediaInfo?.trackInfos
        ?.firstOrNull { it.getType() == TrackInfo.Type.TYPE_VIDEO && it.getIndex() == index }
        ?.let { qualityLabelOf(it) }
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
