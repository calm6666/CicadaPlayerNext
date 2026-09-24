package com.cicada.player.compose.upload.core

import kotlin.math.ceil
import kotlin.math.roundToLong

/**
 * ============================================================================
 * UploadFormat.kt —— 上传展示用格式化工具
 * ============================================================================
 *
 * 与前端 useMultipartUploader.ts 的 formatFileSize / formatUploadSpeed /
 * formatRemainingTime 保持一致，供 UI 层直接复用。
 */
object UploadFormat {

    /**
     * 文件大小格式化。
     * ≤500KB 用 KB、≤500MB 用 MB、更大用 GB，均保留 3 位小数。
     */
    fun formatFileSize(bytes: Long): String {
        val kilobytes = bytes / 1024.0
        val megabytes = bytes / (1024.0 * 1024.0)
        val gigabytes = bytes / (1024.0 * 1024.0 * 1024.0)
        return when {
            bytes <= 500L * 1024 -> String.format("%.3f KB", kilobytes)
            bytes <= 500L * 1024 * 1024 -> String.format("%.3f MB", megabytes)
            else -> String.format("%.3f GB", gigabytes)
        }
    }

    /** 将字节每秒转换为适合界面展示的上传速度。 */
    fun formatUploadSpeed(bytesPerSecond: Double): String {
        if (bytesPerSecond <= 0) return "计算中"
        return "${formatFileSize(bytesPerSecond.roundToLong())}/s"
    }

    /** 将剩余秒数转换为简短的人类可读时间。 */
    fun formatRemainingTime(seconds: Double): String {
        if (!seconds.isFinite() || seconds <= 0) return "计算中"
        val rounded = ceil(seconds).toLong()
        if (rounded < 60) return "$rounded 秒"
        val minutes = rounded / 60
        val remainder = rounded % 60
        if (minutes < 60) return "$minutes 分 $remainder 秒"
        val hours = minutes / 60
        return "$hours 小时 ${minutes % 60} 分"
    }
}
