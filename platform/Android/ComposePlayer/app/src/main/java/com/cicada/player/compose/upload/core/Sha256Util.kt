package com.cicada.player.compose.upload.core

import android.content.ContentResolver
import android.net.Uri
import android.util.Base64
import java.io.IOException
import java.security.MessageDigest

/**
 * ============================================================================
 * Sha256Util.kt —— SHA-256 摘要工具
 * ============================================================================
 *
 * 上传体系中的哈希职责（与服务端/前端一致）：
 *  1. 小文件（≤8 MiB）：全文件 SHA-256，作为「内容寻址秒传」身份；
 *  2. 大文件：每个分片 SHA-256，作为「分片级秒传/续传」身份；
 *  3. 分片直传时：将十六进制 SHA-256 转为 Base64 字节串，填入对象存储
 *     标准头 x-amz-checksum-sha256（S3 对每个分片做完整性校验）。
 *
 * 所有方法均为同步阻塞 IO + CPU 计算，调用方需在 Dispatchers.Default/IO
 * 协程中执行（MultipartUploader 内部已用 Semaphore 限制并行哈希数量）。
 */
object Sha256Util {

    /** 流式读取缓冲区大小（64 KiB），平衡内存与 IO 次数。 */
    private const val BUFFER_SIZE = 64 * 1024

    /** 十六进制字符表（避免 String.format 的 Locale 坑）。 */
    private val HEX_CHARS = "0123456789abcdef".toCharArray()

    /**
     * 计算文件区间 [start, end) 的 SHA-256。
     *
     * @param resolver  ContentResolver，用于打开 Uri 输入流
     * @param uri       文件 Uri
     * @param start     起始偏移（含）
     * @param end       结束偏移（不含）；end <= 0 时按 0 处理
     * @return 十六进制摘要字符串（64 位小写）
     */
    fun hashRange(resolver: ContentResolver, uri: Uri, start: Long, end: Long): String {
        val length = (end - start).coerceAtLeast(0L)
        val digest = MessageDigest.getInstance("SHA-256")
        val input = resolver.openInputStream(uri)
            ?: throw IOException("无法读取文件: $uri")
        input.use { stream ->
            // 1. 跳过起始偏移之前的字节
            var toSkip = start
            while (toSkip > 0) {
                val skipped = stream.skip(toSkip)
                if (skipped <= 0) throw IOException("文件读取越界: $uri")
                toSkip -= skipped
            }
            // 2. 按区间读取并喂给摘要器
            var toRead = length
            val buffer = ByteArray(BUFFER_SIZE)
            while (toRead > 0) {
                val readSize = minOf(BUFFER_SIZE.toLong(), toRead).toInt()
                val read = stream.read(buffer, 0, readSize)
                if (read < 0) throw IOException("文件长度不足: $uri")
                digest.update(buffer, 0, read)
                toRead -= read
            }
        }
        return toHex(digest.digest())
    }

    /** 计算字节数组的 SHA-256（十六进制）。 */
    fun hashBytes(bytes: ByteArray): String =
        toHex(MessageDigest.getInstance("SHA-256").digest(bytes))

    /**
     * 将十六进制 SHA-256 转为 Base64 字节串（对象存储 x-amz-checksum-sha256
     * 头要求的形式，与前端 toBase64() 行为一致）。
     */
    fun hexToBase64(hex: String): String {
        val bytes = ByteArray(hex.length / 2)
        for (i in bytes.indices) {
            val high = hexDigit(hex[i * 2])
            val low = hexDigit(hex[i * 2 + 1])
            bytes[i] = ((high shl 4) or low).toByte()
        }
        return Base64.encodeToString(bytes, Base64.NO_WRAP)
    }

    /** 字节数组转十六进制字符串。 */
    fun toHex(bytes: ByteArray): String = buildString(bytes.size * 2) {
        for (b in bytes) {
            val v = b.toInt() and 0xFF
            append(HEX_CHARS[v ushr 4])
            append(HEX_CHARS[v and 0x0F])
        }
    }

    /** 单个十六进制字符转数值。 */
    private fun hexDigit(c: Char): Int = when (c) {
        in '0'..'9' -> c - '0'
        in 'a'..'f' -> c - 'a' + 10
        in 'A'..'F' -> c - 'A' + 10
        else -> throw IllegalArgumentException("非法十六进制字符: $c")
    }
}
