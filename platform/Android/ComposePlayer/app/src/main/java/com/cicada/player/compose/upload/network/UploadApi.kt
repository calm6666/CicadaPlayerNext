package com.cicada.player.compose.upload.network

import android.content.ContentResolver
import android.net.Uri
import com.google.gson.JsonArray
import com.google.gson.JsonObject
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.Request
import okhttp3.RequestBody
import okio.BufferedSink
import okio.ForwardingSink
import okio.buffer
import com.cicada.player.compose.upload.core.Sha256Util
import java.util.concurrent.ConcurrentHashMap

/**
 * ============================================================================
 * UploadApi.kt —— 上传服务 API 适配层
 * ============================================================================
 *
 * 自前端 front/.../apis/system/multipart-upload.ts 原样移植，接口路径与方法一致：
 *
 *   init       → POST   /api/uploads/init
 *   part-url   → GET    /api/uploads/part-url?id=&part_number=&part_sha256=
 *   part-urls  → POST   /api/uploads/part-urls?id=           （批量签名）
 *   complete   → POST   /api/uploads/complete?id=
 *   abort      → DELETE /api/uploads/abort?id=
 *   分片/小文件直传 → PUT 预签名 S3 URL（分片携带 x-amz-checksum-sha256 头）
 *
 * 命名约定：
 *  - 上述业务接口经 ApiClient 自动完成「请求驼峰→下划线、响应下划线→驼峰」。
 *  - S3 直传为对象存储标准协议，直接使用标准命名（x-amz-checksum-sha256、
 *    Content-Type、ETag），不做任何键名转换。
 */
class UploadApi(private val client: ApiClient) {

    private companion object {
        const val API = "/api/uploads"
        /** 分片直传的媒体类型（对象存储标准写法）。 */
        val OCTET_STREAM = "application/octet-stream".toMediaType()
        /** 直传写出的分块粒度（64 KiB），用于稳定进度回调频率。 */
        const val PROGRESS_CHUNK_BYTES = 64 * 1024
    }

    /**
     * 客户端缓存的上传会话信息，避免完成请求再次暴露对象 key。
     */
    private data class ClientSession(
        /** 服务端生成的对象 key。 */
        val key: String,
        /** single 表示 PutObject，multipart 表示 UploadPart。 */
        val mode: String,
        /** 小文件 PutObject 的预签名 URL。 */
        val putUrl: String?,
    )

    /** 保存 uploadId 与对象 key 的对应关系，完成/取消请求只需传 uploadId。 */
    private val sessions = ConcurrentHashMap<String, ClientSession>()

    // =========================================================================
    // 服务端接口数据模型（字段已按驼峰命名；线上 JSON 由 ApiClient 转换）
    // =========================================================================

    /** 初始化上传会话的请求体。 */
    data class InitInput(
        /** 文件名。 */
        val fileName: String,
        /** 文件大小（字节）。 */
        val fileSize: Long,
        /** MIME 类型，如 video/mp4。 */
        val contentType: String,
        /** 目标目录路径，默认 "/"。 */
        val parentPath: String,
        /** 小文件的全文件 SHA-256（内容寻址秒传身份）。 */
        val fileSha256: String?,
        /** 大文件前 1~2 个分片的 SHA-256（分片级秒传/续传身份）。 */
        val partSha256s: List<String>?,
        /** 上述分片哈希的边界大小（16 MiB）。 */
        val partSha256Size: Long?,
    )

    /** 服务端初始化响应。 */
    data class InitUploadData(
        /** 上传模式。 */
        val mode: String,
        /** 服务端会话 ID。 */
        val uploadId: String,
        /** 服务端生成的对象 key。 */
        val key: String,
        /** 单文件模式的预签名 URL。 */
        val putUrl: String?,
        /** multipart 分片大小。 */
        val partSize: Long,
        /** multipart 总分片数。 */
        val totalParts: Int?,
        /** 签名 URL 有效秒数。 */
        val expiresIn: Long,
        /** 已存在的断点分片编号。 */
        val uploadedPartNumbers: List<Int>,
        /** 内容已经完整存在时为 true。 */
        val completed: Boolean,
        /** 命中同内容会话或对象时为 true。 */
        val deduplicated: Boolean,
        /** 与 key 相同，兼容旧上传 hook 的路径字段。 */
        val path: String,
    )

    /** 一个需要签名的分片（批量签名请求体元素）。 */
    data class SignPartRequest(
        /** 分片编号（从 1 开始）。 */
        val partNumber: Int,
        /** 该分片的 SHA-256（十六进制）。 */
        val partSha256: String,
    )

    /** 批量签名返回的单条签名结果。 */
    data class SignPartResult(
        /** 分片编号。 */
        val partNumber: Int,
        /** 预签名直传 URL。 */
        val url: String,
    )

    /**
     * 初始化上传会话。
     *
     * @param input 文件名、大小、可选摘要和目录等元数据；分片摘要在上传分片前按需计算
     * @return 兼容旧 hook 的数据结构，path 对应对象 key
     */
    suspend fun initMultipartUpload(input: InitInput): InitUploadData {
        val body = JsonObject().apply {
            addProperty("fileName", input.fileName)
            addProperty("fileSize", input.fileSize)
            addProperty("contentType", input.contentType)
            addProperty("parentPath", input.parentPath)
            input.fileSha256?.let { addProperty("fileSha256", it) }
            input.partSha256s?.let { list ->
                val arr = JsonArray()
                list.forEach { arr.add(it) }
                add("partSha256s", arr)
            }
            input.partSha256Size?.let { addProperty("partSha256Size", it) }
        }
        val data = client.request("POST", "$API/init", body = body)
            ?: throw RequestError(-1, "初始化分片上传失败：服务器返回数据为空")

        val uploadId = data["uploadId"]?.asString
            ?: throw RequestError(-1, "初始化响应缺少 uploadId")
        val key = data["key"]?.asString ?: ""
        val putUrl = data["putUrl"]?.takeIf { !it.isJsonNull }?.asString
        val initData = InitUploadData(
            mode = data["mode"]?.asString ?: "multipart",
            uploadId = uploadId,
            key = key,
            putUrl = putUrl,
            partSize = data["partSize"]?.takeIf { !it.isJsonNull }?.asLong ?: input.fileSize,
            totalParts = data["totalParts"]?.takeIf { !it.isJsonNull }?.asInt,
            expiresIn = data["expiresIn"]?.takeIf { !it.isJsonNull }?.asLong ?: 0L,
            uploadedPartNumbers = data["uploadedPartNumbers"]
                ?.takeIf { it.isJsonArray }
                ?.asJsonArray
                ?.mapNotNull { it.takeIf { e -> !e.isJsonNull }?.asInt }
                ?: emptyList(),
            completed = data["completed"]?.takeIf { !it.isJsonNull }?.asBoolean ?: false,
            deduplicated = data["deduplicated"]?.takeIf { !it.isJsonNull }?.asBoolean ?: false,
            path = key,
        )
        sessions[uploadId] = ClientSession(key, initData.mode, putUrl)
        return initData
    }

    /**
     * 签发单个分片的预签名 URL（批量签名失败或签名过期时的回退）。
     */
    suspend fun signPart(uploadId: String, partNumber: Int, partSha256: String): String {
        val data = client.request(
            method = "GET",
            path = "$API/part-url",
            // 查询参数与请求体一样走统一转换：拦截器会把驼峰键转为下划线（id/part_number/part_sha256）
            query = mapOf("id" to uploadId, "partNumber" to partNumber, "partSha256" to partSha256),
        ) ?: throw RequestError(-1, "签名失败：服务器返回数据为空")
        return data["url"]?.asString ?: throw RequestError(-1, "签名失败：缺少 url 字段")
    }

    /**
     * 批量签发多个分片的预签名 URL。
     * 上传器按批（如 32 片）预取 URL，分片直传不再逐片请求网关。
     *
     * @param uploadId 会话 ID
     * @param parts    需要签名的分片编号与摘要
     * @return 与请求一一对应的签名 URL 列表
     */
    suspend fun signParts(uploadId: String, parts: List<SignPartRequest>): List<SignPartResult> {
        val partsArr = JsonArray()
        parts.forEach { part ->
            partsArr.add(
                JsonObject().apply {
                    addProperty("partNumber", part.partNumber)
                    addProperty("partSha256", part.partSha256)
                }
            )
        }
        val data = client.request(
            method = "POST",
            path = "$API/part-urls",
            query = mapOf("id" to uploadId),
            body = JsonObject().apply { add("parts", partsArr) },
        ) ?: return emptyList()
        val list = data["parts"]?.takeIf { it.isJsonArray }?.asJsonArray ?: return emptyList()
        return list.mapNotNull { item ->
            val obj = item.takeIf { it.isJsonObject }?.asJsonObject ?: return@mapNotNull null
            val number = obj["partNumber"]?.asInt ?: return@mapNotNull null
            val url = obj["url"]?.asString ?: return@mapNotNull null
            SignPartResult(number, url)
        }
    }

    /**
     * 通知服务端完成上传。服务端会自行 ListParts 并校验真实分片，不信任 partETags。
     *
     * @param input uploadId 以及旧 hook 传入的 ETag 列表（仅保留签名兼容）
     */
    suspend fun completeMultipartUpload(input: CompleteInput) {
        client.request(
            method = "POST",
            path = "$API/complete",
            query = mapOf("id" to input.uploadId),
            // 统一封装会把无 body 的 POST 请求参数迁移为请求体，这里显式传空对象，保证 id 走查询参数
            body = JsonObject(),
        )
    }

    /** 完成接口的输入（partETags 仅保留签名兼容，服务端不信任）。 */
    data class CompleteInput(
        /** 要完成的上传会话 ID。 */
        val uploadId: String,
        /** 旧 hook 传入的 ETag 列表（兼容字段，实际不参与合并决策）。 */
        val partETags: List<PartETag>? = null,
    )

    /** 分片 ETag 记录（仅用于界面展示，不参与服务端合并决策）。 */
    data class PartETag(
        /** 分片编号。 */
        val partNumber: Int,
        /** 对象存储返回的 ETag。 */
        val eTag: String,
    )

    /**
     * 取消上传并清理对象存储中的未完成 multipart 会话。
     * 失败时静默忽略（与前端一致：取消接口失败不影响本地清理）。
     */
    suspend fun abortUpload(uploadId: String) {
        runCatching {
            client.request(
                method = "DELETE",
                path = "$API/abort",
                query = mapOf("id" to uploadId),
            )
        }
        sessions.remove(uploadId)
    }

    // =========================================================================
    // 对象存储直传（S3 预签名 PUT，标准命名，不做键名转换）
    // =========================================================================

    /**
     * 上传一个分片到对象存储（PUT 预签名 URL）。
     *
     * 携带 x-amz-checksum-sha256 头：值为该分片 SHA-256 摘要的 Base64 字节串，
     * 对象存储据此校验数据完整性（分片 ETag 仅用于界面显示，不用于合并决策）。
     *
     * @param url             预签名直传 URL
     * @param resolver        用于读取文件内容的 ContentResolver
     * @param uri             文件 URI
     * @param start           分片起始偏移（含）
     * @param end             分片结束偏移（不含）
     * @param partSha256Hex   分片 SHA-256（十六进制）
     * @param onProgress      进度回调：已发送字节数 / 总字节数
     * @return 对象存储返回的 ETag（可能为空字符串）
     */
    suspend fun uploadPartToS3(
        url: String,
        resolver: ContentResolver,
        uri: Uri,
        start: Long,
        end: Long,
        partSha256Hex: String,
        onProgress: (loaded: Long, total: Long) -> Unit,
    ): String {
        // 分片字节读取为阻塞 IO，切到 IO 调度器执行
        val bytes = withContext(Dispatchers.IO) { readRange(resolver, uri, start, end) }
        val etag = putWithProgress(
            url = url,
            bytes = bytes,
            contentType = OCTET_STREAM,
            // 对象存储标准头：分片校验和（Base64 编码的 SHA-256）
            checksumSha256Base64 = Sha256Util.hexToBase64(partSha256Hex),
            onProgress = onProgress,
        )
        return etag
    }

    /**
     * 小文件使用 PutObject 直传（不携带校验和头，与服务端完成接口的对象大小
     * 校验配合），不创建也不提交 multipart。
     *
     * @return 对象存储返回的 ETag
     */
    suspend fun uploadSingleFileToS3(
        url: String,
        resolver: ContentResolver,
        uri: Uri,
        fileSize: Long,
        contentType: String?,
        onProgress: (loaded: Long, total: Long) -> Unit,
    ): String {
        // 全文件字节读取为阻塞 IO，切到 IO 调度器执行（小文件 ≤8MiB，内存可控）
        val bytes = withContext(Dispatchers.IO) { readRange(resolver, uri, 0, fileSize) }
        return putWithProgress(
            url = url,
            bytes = bytes,
            contentType = contentType?.takeIf { it.isNotBlank() }?.toMediaType() ?: OCTET_STREAM,
            checksumSha256Base64 = null,
            onProgress = onProgress,
        )
    }

    /**
     * 执行一次带进度回调的 PUT 直传。
     *
     * 进度实现：通过 okio ForwardingSink 包装写出流，按 64 KiB 粒度分批写出，
     * 保证进度回调频率稳定且不额外缓存大块数据。请求失败时按 HTTP 状态码抛出
     * RequestError，供上层做 403 重签 / 429、5xx 重试判断。
     */
    private suspend fun putWithProgress(
        url: String,
        bytes: ByteArray,
        contentType: okhttp3.MediaType,
        checksumSha256Base64: String?,
        onProgress: (loaded: Long, total: Long) -> Unit,
    ): String = withContext(Dispatchers.IO) {
        val body = object : RequestBody() {
            override fun contentType() = contentType
            override fun contentLength() = bytes.size.toLong()

            override fun writeTo(sink: BufferedSink) {
                // ForwardingSink：把「已写出字节数」转成进度回调
                val total = bytes.size.toLong()
                val progressSink = object : ForwardingSink(sink) {
                    var written = 0L
                    override fun write(source: okio.Buffer, byteCount: Long) {
                        super.write(source, byteCount)
                        written += byteCount
                        onProgress(written, total)
                    }
                }
                val buffered = progressSink.buffer()
                var offset = 0
                while (offset < bytes.size) {
                    val remaining = bytes.size - offset
                    val writeSize = minOf(remaining, PROGRESS_CHUNK_BYTES)
                    buffered.write(bytes, offset, writeSize)
                    buffered.flush()
                    offset += writeSize
                }
                buffered.flush()
            }
        }

        val requestBuilder = Request.Builder()
            .url(url)
            .header("Content-Type", contentType.toString())
            .put(body)
        // 对象存储标准校验和头（Base64 字节串）；小文件模式不携带
        checksumSha256Base64?.let { requestBuilder.header("x-amz-checksum-sha256", it) }

        val response = client.executeCancellable(client.httpClient.newCall(requestBuilder.build()))
        response.use { resp ->
            if (!resp.isSuccessful) {
                throw RequestError(resp.code, "上传失败 (${resp.code})")
            }
            // ETag 仅用于界面展示；服务端合并时自行 ListParts 校验，不信任客户端上报
            resp.header("ETag") ?: ""
        }
    }

    /**
     * 从 ContentResolver 读取文件指定区间 [start, end) 的字节。
     * 分片大小由服务端决定（通常 16 MiB），一次性读入内存可控。
     */
    private fun readRange(resolver: ContentResolver, uri: Uri, start: Long, end: Long): ByteArray {
        val length = end - start
        if (length <= 0) return ByteArray(0)
        val input = resolver.openInputStream(uri)
            ?: throw RequestError(-1, "无法读取文件: $uri")
        input.use { stream ->
            var remaining = start
            while (remaining > 0) {
                val skipped = stream.skip(remaining)
                if (skipped <= 0) throw RequestError(-1, "文件读取越界")
                remaining -= skipped
            }
            val buffer = ByteArray(length.toInt())
            var offset = 0
            while (offset < buffer.size) {
                val read = stream.read(buffer, offset, buffer.size - offset)
                if (read < 0) throw RequestError(-1, "文件长度不足")
                offset += read
            }
            return buffer
        }
    }
}
