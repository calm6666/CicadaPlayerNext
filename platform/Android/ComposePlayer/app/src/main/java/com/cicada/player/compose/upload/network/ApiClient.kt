package com.cicada.player.compose.upload.network

import com.google.gson.Gson
import com.google.gson.JsonObject
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import okhttp3.Call
import okhttp3.Callback
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import com.cicada.player.compose.upload.ApiConfig
import com.cicada.player.compose.upload.UploadConfig
import java.io.IOException
import java.util.concurrent.TimeUnit
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

/**
 * ============================================================================
 * ApiClient.kt —— 业务后端 HTTP 客户端（统一信封 + 驼峰↔下划线转换）
 * ============================================================================
 *
 * 与前端 @/utils/http/index 保持一致的行为：
 *
 *  1. 请求体/查询参数的键名：调用方一律传「驼峰」，本层发送前统一转为「下划线」
 *     （Go 后端约定，见 JsonNaming.kt）。
 *  2. 响应信封：{ "code": 0, "message": "...", "data": {...} }，
 *     code != 0 抛出 RequestError；data 的键名统一转为「驼峰」后返回。
 *  3. 自动携带 Authorization: Bearer <token>（由 ApiConfig.tokenProvider 提供）。
 *  4. 所有调用均挂起并可在协程取消时中断底层网络请求（暂停/取消上传依赖此能力）。
 *
 * 对象存储（S3）直传不走本类——它没有业务信封、也不需要键名转换，
 * 由 UploadApi 中的 putWithProgress() 直接构建标准 HTTP 请求。
 */
class ApiClient(private val config: ApiConfig) {

    private val gson = Gson()

    /** 上传网关的全局连接池：业务接口与 S3 直传共享，便于复用连接。 */
    internal val httpClient: OkHttpClient = OkHttpClient.Builder()
        .connectTimeout(UploadConfig.CONNECT_TIMEOUT_MS, TimeUnit.MILLISECONDS)
        .readTimeout(UploadConfig.READ_TIMEOUT_MS, TimeUnit.MILLISECONDS)
        .writeTimeout(UploadConfig.WRITE_TIMEOUT_MS, TimeUnit.MILLISECONDS)
        .build()

    /**
     * 发送一个业务后端请求并返回信封中的 data（键名已转为驼峰）。
     *
     * @param method HTTP 方法（GET/POST/DELETE 等）
     * @param path   相对路径，如 /api/uploads/init
     * @param query  查询参数（键为驼峰，发送前自动转下划线；值为 null 时跳过）
     * @param body   JSON 请求体（键为驼峰，发送前自动转下划线；null 表示无请求体）
     * @return 信封 data 对应的 JsonObject（键为驼峰）；data 为空时返回 null
     * @throws RequestError 业务码非 0 或 HTTP 非 2xx
     */
    suspend fun request(
        method: String,
        path: String,
        query: Map<String, Any?> = emptyMap(),
        body: JsonObject? = null,
    ): JsonObject? = withContext(Dispatchers.IO) {
        // 1. 组装 URL：查询参数键名 驼峰 → 下划线
        val urlBuilder = (config.baseUrl.trimEnd('/') + path).toHttpUrlOrNull()
            ?.newBuilder()
            ?: throw RequestError(-1, "非法网关地址: ${config.baseUrl}")
        query.forEach { (key, value) ->
            if (value != null) urlBuilder.addQueryParameter(JsonNaming.camelToSnake(key), value.toString())
        }

        // 2. 组装请求：请求体键名 驼峰 → 下划线
        val requestBuilder = Request.Builder().url(urlBuilder.build())
        config.tokenProvider()?.takeIf { it.isNotBlank() }?.let { token ->
            requestBuilder.header("Authorization", "Bearer $token")
        }
        val requestBody = body?.let {
            JsonNaming.toSnakeJsonObject(it).toString().toRequestBody(JSON_MEDIA_TYPE)
        }
        requestBuilder.method(method, requestBody)

        // 3. 执行请求（协程取消时同步取消底层网络调用）
        val response = executeCancellable(httpClient.newCall(requestBuilder.build()))
        response.use { resp ->
            val bodyText = resp.body?.string().orEmpty()

            // HTTP 层失败：尽量从响应体里抠出 message
            if (!resp.isSuccessful) {
                val serverMessage = runCatching {
                    gson.fromJson(bodyText, JsonObject::class.java)?.get("message")?.asString
                }.getOrNull()
                throw RequestError(resp.code, serverMessage ?: "请求失败 (${resp.code})")
            }

            // 解析统一信封 { code, message, data }
            val envelope = runCatching { gson.fromJson(bodyText, JsonObject::class.java) }
                .getOrNull() ?: throw RequestError(resp.code, "响应解析失败")
            val code = envelope["code"]?.asLong ?: 0L
            val message = envelope["message"]?.asString
            if (code != 0L) throw RequestError(code.toInt(), message ?: "业务错误 ($code)")

            // data 键名 下划线 → 驼峰（data 可能为 null 或非对象，按空处理）
            envelope["data"]
                ?.takeIf { it.isJsonObject }
                ?.let { JsonNaming.toCamelJsonObject(it.asJsonObject) }
        }
    }

    /**
     * 可取消的 OkHttp 执行器：
     * 使用 enqueue + suspendCancellableCoroutine，协程取消时调用 call.cancel()，
     * 使暂停/取消上传能真正中断正在传输的网络请求。
     */
    internal suspend fun executeCancellable(call: Call): Response = suspendCancellableCoroutine { cont ->
        call.enqueue(object : Callback {
            override fun onFailure(call: Call, e: IOException) {
                if (cont.isActive) cont.resumeWithException(e)
            }

            override fun onResponse(call: Call, response: Response) {
                if (cont.isActive) cont.resume(response)
            }
        })
        cont.invokeOnCancellation { call.cancel() }
    }

    companion object {
        /** JSON 请求体媒体类型。 */
        val JSON_MEDIA_TYPE = "application/json; charset=utf-8".toMediaType()
    }
}
