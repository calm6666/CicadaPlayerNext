package com.cicada.player.compose.upload

/**
 * ============================================================================
 * UploadConfig.kt —— 上传功能全局配置
 * ============================================================================
 *
 * 本文件集中管理两类配置：
 *  1. [ApiConfig]：网关地址与鉴权令牌（业务后端接口）。
 *  2. [UploadConfig]：分片上传调度参数（与前端 useMultipartUploader.ts 保持一致）。
 *
 * 命名约定（重要）：
 *  - 业务后端（Go）接口：请求参数一律「驼峰 → 下划线」发送，响应字段一律
 *    「下划线 → 驼峰」接收，转换逻辑位于 network/JsonNaming.kt。
 *  - 对象存储（S3 预签名 PUT 直传）：直接使用标准 HTTP 头命名（如
 *    x-amz-checksum-sha256、Content-Type、ETag），不做任何下划线转换。
 */

/**
 * 网关配置。
 *
 * @property baseUrl       后端网关基础地址（不带末尾斜杠）。
 * @property tokenProvider 鉴权令牌提供器；返回 null 表示当前未登录，不携带
 *                         Authorization 头。接入登录体系后替换该实现即可。
 */
data class ApiConfig(
    val baseUrl: String,
    val tokenProvider: () -> String? = { null },
) {
    companion object {
        /**
         * 默认配置。
         *
         * 说明：Android 模拟器中 10.0.2.2 指向宿主机（本机后端），真机调试请
         * 改为局域网 IP；生产环境务必使用 https 域名并移除清单中的明文流量开关。
         */
        fun default(): ApiConfig = ApiConfig(baseUrl = "http://192.168.0.15:8080")
    }
}

/**
 * 上传调度常量。除进度节流外，其余常量与前端 useMultipartUploader.ts
 * 及服务端配置保持一致，改动任一端时请同步核对。
 */
object UploadConfig {

    /** 小文件直传阈值：≤ 8 MiB 走 PutObject 单文件模式（服务端 single_file_limit 为准）。 */
    const val SINGLE_UPLOAD_LIMIT: Long = 8L * 1024 * 1024

    /** 分片身份哈希使用的分片边界，与服务端 default_part_size（16 MiB）一致。 */
    const val PART_HASH_SIZE: Long = 16L * 1024 * 1024

    /** 每个文件提前并行哈希的分片数（哈希不占用上传并发槽位）。 */
    const val HASH_AHEAD = 3

    /** 批量签名每批的分片数，避免逐片请求网关。 */
    const val URL_BATCH_SIZE = 32

    /** 单个分片最大重试次数。 */
    const val MAX_PART_RETRIES = 3

    /** 进度发布节流间隔（毫秒），与前端 120ms 节流一致，保证进度条平滑。 */
    const val PROGRESS_THROTTLE_MS = 120L

    /** 上传网络请求的超时（毫秒）：分片直传大文件时读/写超时放宽。 */
    const val CONNECT_TIMEOUT_MS = 30_000L
    const val READ_TIMEOUT_MS = 5 * 60_000L
    const val WRITE_TIMEOUT_MS = 5 * 60_000L
}
