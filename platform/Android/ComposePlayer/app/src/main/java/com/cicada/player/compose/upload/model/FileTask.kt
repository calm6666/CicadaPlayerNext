package com.cicada.player.compose.upload.model

import android.net.Uri
import com.cicada.player.compose.upload.network.UploadApi
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CopyOnWriteArrayList

/**
 * ============================================================================
 * FileTask.kt —— 文件上传任务模型与状态机
 * ============================================================================
 *
 * 对应前端 useMultipartUploader.ts 的 FileTask：每个待上传文件一个任务实例，
 * 由 MultipartUploader 原地修改字段（progress、status 等），UI 通过
 * StateFlow<List<FileTask>> 的浅拷贝列表感知变化（节流后发布）。
 *
 * Android 与浏览器的差异：浏览器为内存中的 File 对象，Android 使用
 * ContentResolver + Uri 按区间（分片）读取文件内容，避免整文件驻留内存。
 *
 * 状态机：
 *   WAITING ──startNextTasks──▶ HASHING ──哈希完成──▶ UPLOADING
 *   UPLOADING ──全部完成──▶ COMPLETED
 *   HASHING/UPLOADING ──pause──▶ PAUSED ──resume──▶ UPLOADING
 *   HASHING/UPLOADING ──cancel──▶ CANCELLED
 *   任意 ──失败──▶ FAILED ──retry/restart──▶ UPLOADING/WAITING
 */

/** 上传任务状态。 */
enum class UploadTaskStatus {
    /** 等待调度（队列中）。 */
    WAITING,

    /** 正在计算身份哈希（SHA-256）。 */
    HASHING,

    /** 正在上传数据。 */
    UPLOADING,

    /** 已暂停。 */
    PAUSED,

    /** 上传完成（含秒传/合并完成）。 */
    COMPLETED,

    /** 上传失败。 */
    FAILED,

    /** 已取消。 */
    CANCELLED,
}

/** 上传模式。 */
enum class UploadMode {
    /** 小文件：PutObject 单次直传。 */
    SINGLE,

    /** 大文件：分片直传 + 服务端合并。 */
    MULTIPART,
}

/**
 * 单个文件的上传任务。
 *
 * 说明：
 *  - 状态与进度字段为 [Volatile]，由多个协程并发读写（分片上传并发回调）；
 *  - [uploadedChunks]/[partETags]/[partProgress]/[partSha256s] 使用并发集合，
 *    保证多分片并发完成时的线程安全；
 *  - [errorMessage] 保存最近一次失败原因，供 UI 展示与重试判断。
 */
class FileTask(
    /** 任务唯一标识（文件名-大小-时间戳-随机数）。 */
    val uid: String,
    /** 文件 URI（Android 以 Uri 代替浏览器 File）。 */
    val uri: Uri,
    /** 文件名。 */
    val fileName: String,
    /** MIME 类型，如 video/mp4。 */
    val fileType: String,
    /** 文件大小（字节）。 */
    val fileSize: Long,
    /** 相对路径（支持文件夹结构；单文件时等于文件名）。 */
    val relativePath: String,
    /** 目录路径（服务端存储目录，默认 "/"）。 */
    val parentPath: String,
) {
    // ------------------------------------------------------------------ 状态
    /** 任务状态。 */
    @Volatile var status: UploadTaskStatus = UploadTaskStatus.WAITING

    /** 上传进度（0f~1f）。 */
    @Volatile var progress: Float = 0f

    /** 错误信息（失败原因）。 */
    @Volatile var errorMessage: String = ""

    /** 标记任务是否已占用上传并发槽位（防止重复调度）。 */
    @Volatile var isUploading: Boolean = false

    /** 标记是否正在重建上传会话（防止并发重建）。 */
    @Volatile var isRestarting: Boolean = false

    // --------------------------------------------------------------- 会话信息
    /** 服务端选择的上传模式。 */
    @Volatile var uploadMode: UploadMode = UploadMode.MULTIPART

    /** 分片上传会话 ID（服务端返回）。 */
    @Volatile var uploadId: String? = null

    /** 小文件 PutObject 预签名地址。 */
    @Volatile var putUrl: String? = null

    /** 文件路径（对象 key，由后端返回）。 */
    @Volatile var path: String? = null

    /** 分片大小（由后端返回，通常 16 MiB）。 */
    @Volatile var chunkSize: Long = 0L

    /** 总分片数。 */
    @Volatile var totalChunks: Int = 0

    // --------------------------------------------------------------- 分片数据
    /** 已上传分片编号集合（并发安全）。 */
    val uploadedChunks: MutableSet<Int> = ConcurrentHashMap.newKeySet()

    /** 分片 ETag 列表（仅界面展示；服务端合并时自行 ListParts 校验）。 */
    val partETags: MutableList<UploadApi.PartETag> = CopyOnWriteArrayList()

    /** 并发分片的实时进度：键为分片号，值为已发送字节数。 */
    val partProgress: MutableMap<Int, Long> = ConcurrentHashMap()

    /** 已计算的分片 SHA-256（十六进制），键为分片编号。 */
    val partSha256s: MutableMap<Int, String> = ConcurrentHashMap()

    /** 计算分片哈希时使用的分片边界大小；与服务端 chunkSize 一致时可复用哈希。 */
    @Volatile var partSizeForHash: Long? = null

    /** 随 init 一并提交的前两个分片哈希（分片级秒传/续传身份）。 */
    @Volatile var partSha256sForInit: List<String>? = null

    /** 文件标准 SHA-256 摘要（小文件内容寻址秒传身份）。 */
    @Volatile var fileSha256: String? = null

    // --------------------------------------------------------------- 统计信息
    /** 已经成功发送或正在发送的文件字节数。 */
    @Volatile var uploadedBytes: Long = 0L

    /** 根据文件总大小计算出的尚未上传字节数。 */
    @Volatile var remainingBytes: Long = fileSize

    /** 从开始发送数据起计算的平均上传速度（字节/秒）。 */
    @Volatile var uploadSpeed: Double = 0.0

    /** 根据当前平均速度估算的剩余上传秒数。 */
    @Volatile var remainingSeconds: Double = 0.0

    /** 第一次发送文件数据时的时间戳（毫秒），用于计算速度。 */
    @Volatile var uploadStartedAt: Long? = null

    /** 暂停前已累计的上传时长（毫秒），用于排除暂停时间段的速度计算。 */
    @Volatile var elapsedUploadMs: Long = 0L

    /** 当前文件的任务协程 Job（暂停/取消时中断所有子流程）。 */
    @Volatile var fileJob: kotlinx.coroutines.Job? = null

    // --------------------------------------------------------------- 便捷方法
    /** 是否处于活跃传输状态（哈希中或上传中）。 */
    fun isActive(): Boolean =
        status == UploadTaskStatus.HASHING || status == UploadTaskStatus.UPLOADING

    /** 是否处于终态（不再变化，除非重试/重新开始）。 */
    fun isFinished(): Boolean =
        status == UploadTaskStatus.COMPLETED ||
            status == UploadTaskStatus.FAILED ||
            status == UploadTaskStatus.CANCELLED
}
