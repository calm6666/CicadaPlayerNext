package com.cicada.player.compose.upload.network

import com.google.gson.JsonObject
import java.io.IOException

/**
 * ============================================================================
 * RequestError.kt —— 强类型请求异常
 * ============================================================================
 *
 * 用于统一表达两种失败：
 *  1. 网关业务失败：信封 code != 0（status 为业务码）。
 *  2. HTTP 层失败：非 2xx 状态码（status 为 HTTP 状态码，如 403/429/500）。
 *
 * 上传器（MultipartUploader）依据 [status] 与 [message] 判断：
 *  - 403 → 预签名 URL 过期，触发单签重试 / 会话重建；
 *  - 429 / 5xx / IOException → 可自动重试；
 *  - message 含「签名」关键字 → 签名服务失败，触发会话重建。
 */
class RequestError(
    /** HTTP 状态码或业务码。 */
    val status: Int,
    /** 服务端返回的错误信息（信封 message 字段或 HTTP 兜底文案）。 */
    message: String,
    /** 服务端返回的原始错误体（可为空）。 */
    val body: JsonObject? = null,
) : IOException(message)
