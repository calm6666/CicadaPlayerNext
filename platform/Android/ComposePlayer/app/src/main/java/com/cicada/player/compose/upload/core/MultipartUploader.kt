package com.cicada.player.compose.upload.core

import android.content.ContentResolver
import android.net.Uri
import android.provider.OpenableColumns
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext
import com.cicada.player.compose.upload.UploadConfig
import com.cicada.player.compose.upload.model.FileTask
import com.cicada.player.compose.upload.model.UploadMode
import com.cicada.player.compose.upload.model.UploadTaskStatus
import com.cicada.player.compose.upload.network.RequestError
import com.cicada.player.compose.upload.network.UploadApi
import java.io.IOException
import java.util.concurrent.ConcurrentHashMap
import kotlin.math.ceil
import kotlin.math.max
import kotlin.math.min
import kotlin.random.Random

/**
 * 签名失败异常：签名服务本身不可用时抛出（区别于 403 签名过期），
 * 上传器据此触发「重建上传会话」兜底。
 */
class SignException(message: String, cause: Throwable? = null) : IOException(message, cause)

/**
 * ============================================================================
 * MultipartUploader.kt —— 通用并发分片上传调度器（Android 版）
 * ============================================================================
 *
 * 自前端 useMultipartUploader.ts 原样移植调度模型，支持：
 *  - 多文件 / 多分片并发上传（文件级 + 分片级两级并发限制）；
 *  - 每个文件独立流水线：哈希提前（HASH_AHEAD）→ 批量签名 → 并发直传，
 *    哈希与签名都不占用上传并发槽位，保证进度条平滑、多文件真正并行；
 *  - 暂停 / 恢复 / 取消 / 失败重试 / 断点续传 / 秒传 / 会话重建兜底。
 *
 * 与前端差异说明：
 *  - 浏览器用 Worker 池做哈希，Android 用「全局 Semaphore + Dispatchers.Default」
 *    限制并行哈希数量（默认 min(3, CPU/2)），效果等价且不阻塞主线程；
 *  - 浏览器用 AbortController 中断请求，Android 用「协程取消 + OkHttp call.cancel()」
 *    实现同样的中断语义（见 ApiClient.executeCancellable）。
 *
 * 线程模型：
 *  - 每个文件任务一个 fileJob（协程树），暂停/取消时 cancel 整棵树；
 *  - 每个任务的 哈希泵 / 上传泵 / 签名泵 各为单一消费协程，通过 Channel 信号
 *    驱动，避免共享可变状态的多线程竞争；
 *  - 进度更新按 120ms 节流发布（StateFlow 浅拷贝列表），UI 直接收集即可。
 *
 * @param api                上传 API 适配层（网关 + S3 直传）
 * @param contentResolver    读取文件内容（分片按区间读取）
 * @param scope              调度器工作协程作用域（建议应用级长生命周期）
 * @param maxConcurrentFiles 最大同时上传文件数（全局并发，默认 CPU 核心数）
 * @param maxConcurrentChunks 每个文件分片上传最大并发数（默认 min(4, CPU)）
 * @param maxUploadWorkers   最大哈希 Worker 数量（默认 CPU/2）
 */
class MultipartUploader(
    private val api: UploadApi,
    private val contentResolver: ContentResolver,
    private val scope: CoroutineScope,
    private val maxConcurrentFiles: Int = defaultMaxConcurrentFiles(),
    private val maxConcurrentChunks: Int = defaultMaxConcurrentChunks(),
    private val maxUploadWorkers: Int = defaultMaxUploadWorkers(),
) {

    companion object {
        /** 获取 CPU 核心数，用于控制并发数量。 */
        private fun cpuCores(): Int = Runtime.getRuntime().availableProcessors().coerceAtLeast(1)

        /** 最大同时上传文件数：默认等于 CPU 核心数。 */
        fun defaultMaxConcurrentFiles(): Int = cpuCores()

        /** 每个文件分片并发数：默认 min(4, CPU 核心数)。 */
        fun defaultMaxConcurrentChunks(): Int = min(4, cpuCores()).coerceAtLeast(1)

        /** 哈希 Worker 池大小：默认 CPU/2（与前端 maxUploadWorkers 一致）。 */
        fun defaultMaxUploadWorkers(): Int = (cpuCores() / 2).coerceAtLeast(1)
    }

    // =========================================================================
    // 对外可观察状态
    // =========================================================================

    /** 所有上传任务列表（浅拷贝发布；任务对象原地修改字段）。 */
    private val _fileTasks = MutableStateFlow<List<FileTask>>(emptyList())
    val fileTasks: StateFlow<List<FileTask>> = _fileTasks.asStateFlow()

    /** 当前正在上传（含哈希中）的文件数量。 */
    val uploadingCount: StateFlow<Int> = _fileTasks
        .map { list -> list.count { it.status == UploadTaskStatus.UPLOADING || it.status == UploadTaskStatus.HASHING } }
        .stateIn(scope, SharingStarted.Eagerly, 0)

    /** 正在计算全文件/分片摘要的任务 uid（UI 用于展示「计算摘要中」）。 */
    val sha256CalculatingTaskUid = MutableStateFlow<String?>(null)

    /** 一次性提示消息（如秒传、恢复分片、重复文件跳过等），UI 收集后 Toast 展示。 */
    val messageEvents = MutableSharedFlow<String>(extraBufferCapacity = 16)

    // =========================================================================
    // 内部运行时
    // =========================================================================

    /** 全局哈希信号量：跨文件共享，最多 HASH_POOL_SIZE 个并行哈希任务。 */
    private val hashSemaphore = Semaphore(min(UploadConfig.HASH_AHEAD, maxUploadWorkers).coerceAtLeast(1))

    /** 每个文件任务的流水线运行时状态表。 */
    private val runtimes = ConcurrentHashMap<String, TaskRuntime>()

    /**
     * 单个文件任务的流水线运行时状态。
     *
     * 队列/集合的访问者约定（单消费者模型）：
     *  - hashQueue/hashing/ready：哈希泵（drainHash）唯一消费者；
     *  - uploading：上传泵唯一添加者，分片协程 finally 中移除；
     *  - urlCache/urlWaiters/urlRequests：签名泵唯一消费者，上传协程写入请求。
     */
    private class TaskRuntime {
        /** 等待哈希的分片编号。 */
        val hashQueue = ArrayDeque<Int>()

        /** 正在哈希的分片编号。 */
        val hashing = mutableSetOf<Int>()

        /** 哈希完成、等待上传的分片编号。 */
        val ready = ArrayDeque<Int>()

        /** 正在直传的分片编号。 */
        val uploading = mutableSetOf<Int>()

        /** 已签好的 URL 缓存，键为分片编号。 */
        val urlCache = mutableMapOf<Int, String>()

        /** 等待 URL 的 Promise（CompletableDeferred），键为分片编号。 */
        val urlWaiters = mutableMapOf<Int, CompletableDeferred<String>>()

        /** 需要签名的分片请求通道（签名泵消费）。 */
        val urlRequests = Channel<Int>(Channel.UNLIMITED)

        /** 哈希泵信号（CONFLATED：合并重复唤醒）。 */
        val hashPumpSignal = Channel<Unit>(Channel.CONFLATED)

        /** 上传泵信号（CONFLATED：合并重复唤醒）。 */
        val uploadPumpSignal = Channel<Unit>(Channel.CONFLATED)

        /** 是否正在执行完成合并。 */
        @Volatile var finishing = false

        /** 进度节流锁与状态。 */
        val progressLock = Any()
        var lastProgressPublish = 0L
        var progressUpdateScheduled = false
    }

    private fun runtime(task: FileTask): TaskRuntime =
        runtimes.getOrPut(task.uid) { TaskRuntime() }

    /**
     * 释放任务的流水线状态：
     * 丢弃已签名的 URL（暂停/失败后旧签名可能已过期，恢复时全部重新批量签名），
     * 唤醒并失败所有等待签名的分片协程，清空信号通道残留。
     */
    private fun releaseRuntime(task: FileTask) {
        val rt = runtimes[task.uid] ?: return
        rt.hashQueue.clear()
        rt.hashing.clear()
        rt.ready.clear()
        rt.uploading.clear()
        rt.urlCache.clear()
        rt.urlWaiters.values.forEach { it.completeExceptionally(SignException("任务已结束")) }
        rt.urlWaiters.clear()
        while (rt.urlRequests.tryReceive().isSuccess) { /* 丢弃残留签名请求 */ }
        rt.hashPumpSignal.tryReceive()
        rt.uploadPumpSignal.tryReceive()
    }

    // =========================================================================
    // 哈希 Worker 池（全局 Semaphore + Dispatchers.Default）
    // =========================================================================

    /** 计算文件区间的 SHA-256，不阻塞主线程且不占用上传并发槽位。 */
    private suspend fun hashPart(task: FileTask, start: Long, end: Long): String =
        hashSemaphore.withPermit {
            withContext(Dispatchers.Default) {
                Sha256Util.hashRange(contentResolver, task.uri, start, end)
            }
        }

    // =========================================================================
    // 批量签名 URL 池：每批最多 URL_BATCH_SIZE 片，避免逐片请求网关
    // =========================================================================

    /**
     * 获取分片直传 URL：命中缓存直接返回，否则登记等待者并通知签名泵，
     * 批量签名失败时逐片回退单签名。
     */
    private suspend fun ensurePartUrl(task: FileTask, rt: TaskRuntime, number: Int): String {
        rt.urlCache[number]?.let { return it }
        rt.urlWaiters[number]?.let { existing -> return existing.await() }
        val deferred = CompletableDeferred<String>()
        rt.urlWaiters[number] = deferred
        rt.urlRequests.send(number)
        return deferred.await()
    }

    /**
     * 签名泵：消费签名请求，按批（最多 URL_BATCH_SIZE）调用批量签名接口，
     * 批量失败时逐片回退单签名，保证上传不被整体阻塞；签名都失败时，
     * 唤醒等待者抛 SignException，由上传协程触发会话重建。
     */
    private suspend fun urlActorLoop(task: FileTask, rt: TaskRuntime) {
        for (number in rt.urlRequests) {
            // 收集本批请求（最多 URL_BATCH_SIZE 片）
            val batch = mutableListOf(number)
            while (batch.size < UploadConfig.URL_BATCH_SIZE) {
                val next = rt.urlRequests.tryReceive().getOrNull() ?: break
                batch.add(next)
            }
            // 批量签名
            var failure: Exception? = null
            try {
                val results = api.signParts(
                    task.uploadId!!,
                    batch.map { UploadApi.SignPartRequest(it, task.partSha256s[it] ?: "") },
                )
                for (item in results) rt.urlCache[item.partNumber] = item.url
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                failure = e
            }
            // 逐片兜底单签名并唤醒等待者
            for (n in batch) {
                var url = rt.urlCache[n]
                if (url == null) {
                    try {
                        url = api.signPart(task.uploadId!!, n, task.partSha256s[n] ?: "")
                        rt.urlCache[n] = url
                    } catch (e: CancellationException) {
                        throw e
                    } catch (_: Exception) {
                        // 保持 null，由下方统一失败
                    }
                }
                rt.urlWaiters.remove(n)?.let { waiter ->
                    if (url != null) waiter.complete(url)
                    else waiter.completeExceptionally(failure ?: SignException("分片 $n 签名失败"))
                }
            }
        }
    }

    // =========================================================================
    // 每文件流水线：哈希提前（HASH_AHEAD）→ 上传并发（maxConcurrentChunks）
    // =========================================================================

    /** 把分片编号加入哈希队列并唤醒哈希泵。 */
    private fun enqueueChunk(task: FileTask, rt: TaskRuntime, number: Int) {
        if (number in task.uploadedChunks) return
        if (number in rt.hashQueue || number in rt.ready || number in rt.hashing || number in rt.uploading) return
        rt.hashQueue.addLast(number)
        rt.hashPumpSignal.trySend(Unit)
    }

    /** 哈希泵循环：单消费者，串行 drainHash（作为文件任务协程的子协程运行）。 */
    private suspend fun CoroutineScope.pumpHashLoop(task: FileTask, rt: TaskRuntime) {
        for (signal in rt.hashPumpSignal) {
            drainHash(task, rt)
        }
    }

    /** 取哈希队列头，保持最多 HASH_AHEAD 个并行哈希任务。 */
    private fun CoroutineScope.drainHash(task: FileTask, rt: TaskRuntime) {
        while (task.status == UploadTaskStatus.UPLOADING &&
            rt.hashing.size < UploadConfig.HASH_AHEAD &&
            rt.hashQueue.isNotEmpty()
        ) {
            val number = rt.hashQueue.removeFirst()
            if (number in task.uploadedChunks) continue
            // init 前已按相同边界算过该片哈希时直接复用，避免重复读取文件
            val cached = task.partSha256s[number]
            if (cached != null && task.partSizeForHash == task.chunkSize) {
                rt.ready.addLast(number)
                rt.uploadPumpSignal.trySend(Unit)
                continue
            }
            rt.hashing.add(number)
            launch {
                val start = (number - 1L) * task.chunkSize
                val end = min(start + task.chunkSize, task.fileSize)
                try {
                    val hash = hashPart(task, start, end)
                    rt.hashing.remove(number)
                    if (task.status != UploadTaskStatus.UPLOADING) return@launch
                    task.partSha256s[number] = hash
                    rt.ready.addLast(number)
                    rt.uploadPumpSignal.trySend(Unit)
                    rt.hashPumpSignal.trySend(Unit)
                } catch (e: CancellationException) {
                    rt.hashing.remove(number)
                    throw e
                } catch (_: Exception) {
                    // 哈希失败：回队重试（300ms 后唤醒哈希泵）
                    rt.hashing.remove(number)
                    if (task.status == UploadTaskStatus.UPLOADING) {
                        rt.hashQueue.addFirst(number)
                        scope.launch {
                            delay(300)
                            rt.hashPumpSignal.trySend(Unit)
                        }
                    }
                }
            }
        }
    }

    /** 上传泵循环：单消费者，串行 drainUpload（作为文件任务协程的子协程运行）。 */
    private suspend fun CoroutineScope.pumpUploadLoop(task: FileTask, rt: TaskRuntime) {
        for (signal in rt.uploadPumpSignal) {
            drainUpload(task, rt)
        }
    }

    /** 取就绪分片，保持最多 maxConcurrentChunks 个并行直传。 */
    private fun CoroutineScope.drainUpload(task: FileTask, rt: TaskRuntime) {
        while (task.status == UploadTaskStatus.UPLOADING &&
            rt.uploading.size < maxConcurrentChunks &&
            rt.ready.isNotEmpty()
        ) {
            val number = rt.ready.removeFirst()
            if (number in task.uploadedChunks) continue
            rt.uploading.add(number)
            launch { uploadChunk(task, rt, number) }
        }
    }

    /**
     * 重建上传会话：当单签/批量签名接口也失败（uploadId 会话已过期）时兜底。
     * 保留 fileSha256 与 partSha256sForInit（内容哈希），重新 init 时服务端
     * 会按内容寻址恢复已上传分片继续上传，或返回全新会话从零开始。
     */
    private fun restartSession(task: FileTask) {
        if (task.isRestarting) return
        task.isRestarting = true
        val rt = runtime(task)
        task.fileJob?.cancel()
        releaseRuntime(task)
        task.uploadId = null
        task.putUrl = null
        task.path = null
        task.uploadedChunks.clear()
        task.partETags.clear()
        task.totalChunks = 0
        task.uploadedBytes = 0
        task.remainingBytes = task.fileSize
        task.progress = 0f
        task.uploadStartedAt = null
        task.elapsedUploadMs = 0
        task.uploadSpeed = 0.0
        task.remainingSeconds = 0.0
        task.partProgress.clear()
        task.status = UploadTaskStatus.UPLOADING
        task.isUploading = true
        emitTasks()
        task.fileJob = scope.launch {
            try {
                runFileUpload(task)
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                if (task.status == UploadTaskStatus.UPLOADING) {
                    task.status = UploadTaskStatus.FAILED
                    task.errorMessage = e.message ?: "重建上传会话失败"
                    emitTasks()
                    task.isUploading = false
                    startNextTasks()
                }
            } finally {
                task.isRestarting = false
            }
        }
    }

    /**
     * 上传单个分片：批量签好的 URL 直传，403 过期时单签重试，
     * 可重试错误按 1s/2s 退避重试，签名失败触发会话重建。
     */
    private suspend fun uploadChunk(task: FileTask, rt: TaskRuntime, number: Int) {
        try {
            if (task.uploadStartedAt == null) task.uploadStartedAt = System.currentTimeMillis()
            val start = (number - 1L) * task.chunkSize
            val end = min(start + task.chunkSize, task.fileSize)
            // 哈希兜底：正常情况下哈希泵已提前算好
            val hash = task.partSha256s[number]
                ?: hashPart(task, start, end).also { task.partSha256s[number] = it }
            var url = try {
                ensurePartUrl(task, rt, number)
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                throw SignException("分片 $number 签名失败", e)
            }

            for (attempt in 0 until UploadConfig.MAX_PART_RETRIES) {
                if (task.status != UploadTaskStatus.UPLOADING) return
                try {
                    val etag = api.uploadPartToS3(
                        url = url,
                        resolver = contentResolver,
                        uri = task.uri,
                        start = start,
                        end = end,
                        partSha256Hex = hash,
                    ) { loaded, total -> handlePartProgress(task, number, loaded, total) }

                    task.partETags.add(UploadApi.PartETag(number, etag))
                    task.uploadedChunks.add(number)
                    task.partProgress.remove(number)
                    updateTaskProgress(task, task.totalChunks)
                    // 全部上传完成后调用服务端合并
                    if (task.uploadedChunks.size >= task.totalChunks) finishTask(task, rt)
                    return
                } catch (e: CancellationException) {
                    throw e
                } catch (e: Exception) {
                    if (task.status == UploadTaskStatus.PAUSED || task.status == UploadTaskStatus.CANCELLED) return
                    if (e is RequestError && e.status == 403) {
                        // 签名过期：丢弃缓存 URL，单签后立即重试
                        rt.urlCache.remove(number)
                        try {
                            url = api.signPart(task.uploadId!!, number, hash)
                            rt.urlCache[number] = url
                            continue
                        } catch (se: CancellationException) {
                            throw se
                        } catch (_: Exception) {
                            // 单签也失败：uploadId 会话已过期，重建会话（服务端按内容续传或从零）
                            restartSession(task)
                            return
                        }
                    }
                    // 签名接口本身失败同样重建会话兜底
                    if (e is SignException) {
                        restartSession(task)
                        return
                    }
                    if (attempt + 1 < UploadConfig.MAX_PART_RETRIES && isRetryableError(e)) {
                        delay(1000L * (attempt + 1))
                        continue
                    }
                    throw e
                }
            }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            if (task.status == UploadTaskStatus.UPLOADING) {
                task.status = UploadTaskStatus.FAILED
                task.errorMessage = e.message ?: "分片 $number 上传失败"
                emitTasks()
                releaseRuntime(task)
                task.isUploading = false
                startNextTasks()
            }
        } finally {
            rt.uploading.remove(number)
            if (task.status == UploadTaskStatus.UPLOADING) rt.uploadPumpSignal.trySend(Unit)
        }
    }

    /** 全部上传完成后调用服务端合并。服务端自行 ListParts 校验，不信任客户端 ETag。 */
    private suspend fun finishTask(task: FileTask, rt: TaskRuntime) {
        if (rt.finishing) return
        rt.finishing = true
        try {
            task.uploadId?.let {
                api.completeMultipartUpload(UploadApi.CompleteInput(it, task.partETags.toList()))
            }
            task.status = UploadTaskStatus.COMPLETED
            task.progress = 1f
            task.uploadedBytes = task.fileSize
            task.remainingBytes = 0
            task.remainingSeconds = 0.0
            task.uploadSpeed = task.uploadStartedAt?.let { startedAt ->
                val seconds = ((System.currentTimeMillis() - startedAt + task.elapsedUploadMs) / 1000.0).coerceAtLeast(0.001)
                task.fileSize / seconds
            } ?: 0.0
            emitTasks()
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            task.status = UploadTaskStatus.FAILED
            task.errorMessage = "合并失败: ${e.message ?: "未知错误"}"
            emitTasks()
        } finally {
            rt.finishing = false
            task.isUploading = false
            releaseRuntime(task)
            startNextTasks()
            task.fileJob?.cancel() // 流水线泵已无任务，收尾取消整棵协程树
        }
    }

    // =========================================================================
    // 进度与统计
    // =========================================================================

    /** 统一处理分片直传进度。 */
    private fun handlePartProgress(task: FileTask, number: Int, loaded: Long, total: Long) {
        if (task.uploadStartedAt == null) task.uploadStartedAt = System.currentTimeMillis()
        task.partProgress[number] = min(loaded, total)
        updateTaskProgress(task, task.totalChunks)
    }

    /** 统一处理小文件直传进度。 */
    private fun handleSingleProgress(task: FileTask, loaded: Long, total: Long) {
        if (task.uploadStartedAt == null) task.uploadStartedAt = System.currentTimeMillis()
        task.uploadedBytes = min(loaded, task.fileSize)
        task.remainingBytes = max(0L, task.fileSize - task.uploadedBytes)
        updateTaskProgress(task, task.totalChunks)
    }

    /**
     * 节流发布进度：按 [UploadConfig.PROGRESS_THROTTLE_MS] 节流，
     * 高频回调合并为「立即一次 + 延迟收尾一次」，保证 UI 平滑且不刷爆状态流。
     */
    private fun updateTaskProgress(task: FileTask, totalChunks: Int, force: Boolean = false) {
        val rt = runtimes[task.uid] ?: return
        if (force) {
            publishProgress(task, totalChunks)
            return
        }
        val now = System.currentTimeMillis()
        synchronized(rt.progressLock) {
            if (rt.progressUpdateScheduled) return
            if (now - rt.lastProgressPublish >= UploadConfig.PROGRESS_THROTTLE_MS) {
                rt.lastProgressPublish = now
                publishProgress(task, totalChunks)
            } else {
                rt.progressUpdateScheduled = true
                scope.launch {
                    delay(UploadConfig.PROGRESS_THROTTLE_MS - (now - rt.lastProgressPublish))
                    synchronized(rt.progressLock) {
                        rt.progressUpdateScheduled = false
                        rt.lastProgressPublish = System.currentTimeMillis()
                    }
                    publishProgress(task, task.totalChunks)
                }
            }
        }
    }

    /**
     * 根据已完成分片和正在传输分片的真实字节数刷新统计信息。
     * 速度按真实传输时长计算：当前计时段 + 暂停前累计时长（排除暂停时间）。
     */
    private fun publishProgress(task: FileTask, totalChunks: Int) {
        var uploadedBytes = 0L
        if (task.uploadMode != UploadMode.SINGLE) {
            for (chunkNumber in task.uploadedChunks) {
                val start = (chunkNumber - 1L) * task.chunkSize
                uploadedBytes += max(0L, min(task.chunkSize, task.fileSize - start))
            }
            for (loaded in task.partProgress.values) uploadedBytes += loaded
        } else {
            uploadedBytes = task.uploadedBytes
        }
        uploadedBytes = min(uploadedBytes, task.fileSize)
        task.uploadedBytes = uploadedBytes
        task.remainingBytes = max(0L, task.fileSize - uploadedBytes)
        task.progress = if (task.fileSize > 0) (uploadedBytes.toDouble() / task.fileSize).toFloat() else 0f
        val startedAt = task.uploadStartedAt
        if (startedAt != null && uploadedBytes > 0) {
            val activeMs = System.currentTimeMillis() - startedAt + task.elapsedUploadMs
            val elapsedSeconds = (activeMs / 1000.0).coerceAtLeast(0.001)
            task.uploadSpeed = uploadedBytes / elapsedSeconds
            task.remainingSeconds = if (task.uploadSpeed > 0) task.remainingBytes / task.uploadSpeed else 0.0
        }
        if (totalChunks == 0) task.progress = 0f
        emitTasks()
    }

    /** 暂停计时结算：把当前计时段并入累计时长并复位起点。 */
    private fun pauseTimer(task: FileTask) {
        val startedAt = task.uploadStartedAt ?: return
        task.elapsedUploadMs += System.currentTimeMillis() - startedAt
        task.uploadStartedAt = null
    }

    // =========================================================================
    // 文件级调度
    // =========================================================================

    /** 把等待中的任务按文件级并发上限逐个启动。 */
    private fun startNextTasks() {
        val active = _fileTasks.value.count { it.status == UploadTaskStatus.UPLOADING }
        var available = maxConcurrentFiles - active
        if (available <= 0) return
        var started = false
        for (task in _fileTasks.value) {
            if (available <= 0) break
            if (task.status == UploadTaskStatus.WAITING && !task.isUploading) {
                task.status = UploadTaskStatus.UPLOADING
                task.isUploading = true
                val rt = runtime(task)
                task.fileJob = scope.launch { runFileUpload(task) }
                available--
                started = true
            }
        }
        if (started) emitTasks()
    }

    /**
     * 分片上传核心流程（单个文件）：
     *  1. 初始化前算身份哈希（小文件全量 / 大文件前 1~2 片），随 init 提交；
     *  2. init 返回 uploadId（命中秒传直接完成；已存在分片计入进度断点续传）；
     *  3. 小文件走 PutObject 单次直传；大文件进入「哈希→签名→上传」流水线。
     */
    private suspend fun runFileUpload(task: FileTask) {
        try {
            // ---- 1. 初始化分片上传，获取 uploadId ----
            if (task.uploadId == null) {
                val needWholeHash = task.fileSize <= UploadConfig.SINGLE_UPLOAD_LIMIT
                if ((needWholeHash && task.fileSha256.isNullOrEmpty()) ||
                    (!needWholeHash && task.partSha256sForInit == null)
                ) {
                    task.status = UploadTaskStatus.HASHING
                    emitTasks()
                    sha256CalculatingTaskUid.value = task.uid
                    try {
                        if (needWholeHash) {
                            // 小文件：全文件 SHA-256（≤8MiB），走内容寻址秒传
                            task.fileSha256 = hashPart(task, 0, task.fileSize)
                        } else {
                            // 大文件：只哈希前 1~2 个分片（≤32MiB），随 init 提交，
                            // 不做全量哈希，超大文件也无需全量预读
                            val need = min(2, ceil(task.fileSize / UploadConfig.PART_HASH_SIZE.toDouble()).toInt())
                            val hashes = mutableListOf<String>()
                            for (i in 1..need) {
                                val start = (i - 1L) * UploadConfig.PART_HASH_SIZE
                                val end = min(start + UploadConfig.PART_HASH_SIZE, task.fileSize)
                                val hash = hashPart(task, start, end)
                                task.partSha256s[i] = hash
                                hashes.add(hash)
                            }
                            task.partSizeForHash = UploadConfig.PART_HASH_SIZE
                            task.partSha256sForInit = hashes
                        }
                    } catch (e: CancellationException) {
                        throw e
                    } catch (_: Exception) {
                        task.status = UploadTaskStatus.FAILED
                        task.errorMessage = "文件摘要计算失败"
                        emitTasks()
                        task.isUploading = false
                        startNextTasks()
                        return
                    } finally {
                        sha256CalculatingTaskUid.value = null
                    }
                    // 哈希期间被暂停：结束本次流程，等待 resume 重新进入
                    if (task.status != UploadTaskStatus.HASHING) {
                        task.isUploading = false
                        return
                    }
                    task.status = UploadTaskStatus.UPLOADING
                    emitTasks()
                }

                val parentPath = task.parentPath.ifBlank { "/" }
                try {
                    val res = api.initMultipartUpload(
                        UploadApi.InitInput(
                            fileName = task.fileName,
                            fileSize = task.fileSize,
                            contentType = task.fileType.ifBlank { "application/octet-stream" },
                            parentPath = parentPath,
                            fileSha256 = task.fileSha256,
                            partSha256s = task.partSha256sForInit,
                            partSha256Size = task.partSizeForHash,
                        )
                    )
                    task.uploadId = res.uploadId
                    task.uploadMode = if (res.mode == "single") UploadMode.SINGLE else UploadMode.MULTIPART
                    task.putUrl = res.putUrl
                    task.chunkSize = if (res.partSize > 0) res.partSize else task.fileSize
                    task.path = res.path

                    // 秒传：内容已完整存在，直接完成
                    if (res.completed) {
                        task.status = UploadTaskStatus.COMPLETED
                        task.progress = 1f
                        task.uploadedBytes = task.fileSize
                        task.remainingBytes = 0
                        task.remainingSeconds = 0.0
                        emitTasks()
                        messageEvents.tryEmit("文件 \"${task.fileName}\" 已存在，已完成秒传")
                        task.isUploading = false
                        startNextTasks()
                        return
                    }

                    // 断点续传：服务端已存在分片直接计入进度
                    if (res.uploadedPartNumbers.isNotEmpty()) {
                        task.uploadedChunks.addAll(res.uploadedPartNumbers)
                        val totalChunks = ceil(task.fileSize / task.chunkSize.toDouble()).toInt().coerceAtLeast(1)
                        task.totalChunks = totalChunks
                        updateTaskProgress(task, totalChunks, force = true)
                        messageEvents.tryEmit("已恢复 ${task.uploadedChunks.size}/$totalChunks 个分片，继续上传")
                    }
                } catch (e: CancellationException) {
                    throw e
                } catch (e: Exception) {
                    val errorMsg = (e as? RequestError)?.message ?: e.message ?: "未知错误"
                    task.status = UploadTaskStatus.FAILED
                    task.errorMessage = when {
                        errorMsg.contains("文件名已存在") -> "文件名已存在"
                        errorMsg.contains("不支持的文件类型") -> "不支持的文件类型"
                        else -> "初始化失败: $errorMsg"
                    }
                    emitTasks()
                    messageEvents.tryEmit("文件 \"${task.fileName}\" 上传失败：${task.errorMessage}")
                    task.isUploading = false
                    startNextTasks()
                    return
                }
            }

            if (task.status != UploadTaskStatus.UPLOADING) return

            // ---- 2. 小文件：PutObject 单次直传 + 完成校验 ----
            if (task.uploadMode == UploadMode.SINGLE || task.fileSize <= UploadConfig.SINGLE_UPLOAD_LIMIT) {
                task.uploadMode = UploadMode.SINGLE
                task.totalChunks = 1
                task.chunkSize = task.fileSize
                if (task.uploadStartedAt == null) task.uploadStartedAt = System.currentTimeMillis()
                runSingleFlow(task)
                return
            }

            // ---- 3. 大文件：进入 哈希→签名→上传 流水线 ----
            val totalChunks = ceil(task.fileSize / task.chunkSize.toDouble()).toInt().coerceAtLeast(1)
            task.totalChunks = totalChunks
            task.partETags.clear()
            task.progress = 0f
            task.uploadedBytes = 0
            task.remainingBytes = task.fileSize
            task.partProgress.clear()
            updateTaskProgress(task, totalChunks, force = true)
            coroutineScope { startMultipartPipeline(task, runtime(task)) }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            // 捕获并处理所有未处理的异常；暂停/取消是主动中断，不是失败
            if (task.status == UploadTaskStatus.PAUSED || task.status == UploadTaskStatus.CANCELLED) return
            if (task.status != UploadTaskStatus.FAILED) {
                task.status = UploadTaskStatus.FAILED
                task.errorMessage = "上传失败: ${e.message ?: "未知错误"}"
                emitTasks()
            }
            releaseRuntime(task)
            task.isUploading = false
            startNextTasks()
        }
    }

    /** 启动大文件分片流水线（供首次上传、恢复、重试共用）。 */
    private suspend fun CoroutineScope.startMultipartPipeline(task: FileTask, rt: TaskRuntime) {
        val totalChunks = ceil(task.fileSize / task.chunkSize.toDouble()).toInt().coerceAtLeast(1)
        task.totalChunks = totalChunks
        // 三个常驻泵：哈希泵、上传泵、签名泵（作为当前协程子任务，随文件任务一起取消）
        launch { pumpHashLoop(task, rt) }
        launch { pumpUploadLoop(task, rt) }
        launch { urlActorLoop(task, rt) }
        // 入队所有缺失分片并唤醒泵
        for (i in 1..totalChunks) {
            if (i !in task.uploadedChunks) enqueueChunk(task, rt, i)
        }
        rt.hashPumpSignal.trySend(Unit)
        rt.uploadPumpSignal.trySend(Unit)
    }

    /** 小文件直传并完成校验的独立流程，供首次与恢复共用。 */
    private suspend fun runSingleFlow(task: FileTask) {
        try {
            val url = task.putUrl ?: throw IllegalStateException("小文件上传签名为空")
            api.uploadSingleFileToS3(
                url = url,
                resolver = contentResolver,
                uri = task.uri,
                fileSize = task.fileSize,
                contentType = task.fileType,
            ) { loaded, total -> handleSingleProgress(task, loaded, total) }

            if (task.status != UploadTaskStatus.UPLOADING) return
            task.uploadId?.let { api.completeMultipartUpload(UploadApi.CompleteInput(it)) }

            task.status = UploadTaskStatus.COMPLETED
            task.progress = 1f
            task.uploadedBytes = task.fileSize
            task.remainingBytes = 0
            task.remainingSeconds = 0.0
            emitTasks()
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            if (task.status == UploadTaskStatus.PAUSED || task.status == UploadTaskStatus.CANCELLED) return
            if (e is RequestError && e.status == 403) {
                // 小文件直传签名过期：重建会话重新获取签名（fileSha256 仍在，init 可秒传或返回新签名）
                restartSession(task)
                return
            }
            task.status = UploadTaskStatus.FAILED
            task.errorMessage = e.message ?: "上传失败"
            emitTasks()
        } finally {
            // 重建会话进行中（status=UPLOADING）时不接管生命周期
            if (task.status != UploadTaskStatus.UPLOADING) {
                task.isUploading = false
                startNextTasks()
            }
        }
    }

    // =========================================================================
    // 对外控制接口
    // =========================================================================

    /**
     * 添加文件到上传队列。
     *
     * @param uris       待上传文件 Uri 列表
     * @param parentPath 父目录（服务端存储目录，默认 "/"）
     * @param isFolder   是否为文件夹（Android 版暂只支持文件多选；参数与前端保持一致）
     * @param rootPath   文件夹根路径（文件夹模式下使用）
     */
    suspend fun addFiles(uris: List<Uri>, parentPath: String, isFolder: Boolean = false, rootPath: String? = null) {
        // 1. 读取元数据（后台 IO）并过滤无效文件（空文件 / 空文件名 / 无法读取）
        val resolved = withContext(Dispatchers.IO) {
            uris.mapNotNull { uri ->
                val meta = readMeta(uri) ?: return@mapNotNull null
                if (meta.size <= 0 || meta.name.isBlank()) null else meta
            }
        }
        if (resolved.isEmpty()) return

        // 2. 归一化目录路径（去末尾斜杠、合并双斜杠）
        var parent = parentPath.ifBlank { "/" }
        if (parent.length > 1 && parent.endsWith("/")) parent = parent.dropLast(1)
        if (parent.startsWith("//")) parent = parent.drop(1)

        // 3. 同目录文件名去重（与队列中任务 / 本批次内重复均跳过）
        val duplicateKeys = mutableSetOf<String>()
        val newKeys = mutableSetOf<String>()
        for (meta in resolved) {
            val key = "$parent/${meta.name}"
            val existsInTasks = _fileTasks.value.any { it.parentPath == parent && it.fileName == meta.name }
            if (existsInTasks || !newKeys.add(key)) duplicateKeys.add(key)
        }
        if (duplicateKeys.isNotEmpty()) {
            val names = duplicateKeys.joinToString(", ") { it.substringAfterLast('/') }
            messageEvents.tryEmit("以下文件已存在，已跳过：$names")
        }

        // 4. 创建任务并入队
        val tasks = resolved.mapNotNull { meta ->
            val key = "$parent/${meta.name}"
            if (key in duplicateKeys) return@mapNotNull null
            FileTask(
                uid = "${meta.name}-${System.currentTimeMillis()}-${Random.nextInt(100000)}",
                uri = meta.uri,
                fileName = meta.name,
                fileType = meta.mime,
                fileSize = meta.size,
                relativePath = meta.name,
                parentPath = parent,
            )
        }
        if (tasks.isNotEmpty()) {
            _fileTasks.update { it + tasks }
        }
    }

    /** 读取 Uri 的显示名、大小与 MIME 类型（后台 IO 调用）。 */
    private fun readMeta(uri: Uri): ResolvedFile? = runCatching {
        var name = uri.lastPathSegment ?: "unknown"
        var size = 0L
        contentResolver.query(uri, null, null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) {
                val nameIdx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (nameIdx >= 0 && !cursor.isNull(nameIdx)) name = cursor.getString(nameIdx) ?: name
                val sizeIdx = cursor.getColumnIndex(OpenableColumns.SIZE)
                if (sizeIdx >= 0 && !cursor.isNull(sizeIdx)) size = cursor.getLong(sizeIdx)
            }
        }
        val mime = contentResolver.getType(uri) ?: "application/octet-stream"
        ResolvedFile(uri, name, mime, size)
    }.getOrNull()

    /** 文件元数据。 */
    private data class ResolvedFile(
        val uri: Uri,
        val name: String,
        val mime: String,
        val size: Long,
    )

    /** 全部开始上传（恢复所有暂停任务并启动等待任务）。 */
    fun startAllUpload() {
        for (task in _fileTasks.value) {
            if (task.status == UploadTaskStatus.PAUSED) resumeTask(task)
        }
        startNextTasks()
    }

    /** 启动单个等待中的任务。 */
    fun startTask(task: FileTask) {
        if (task.status == UploadTaskStatus.WAITING) startNextTasks()
    }

    /** 暂停单个任务：取消其协程树并释放流水线。 */
    fun pauseTask(task: FileTask) {
        if (task.status != UploadTaskStatus.UPLOADING && task.status != UploadTaskStatus.HASHING) return
        pauseTimer(task)
        task.status = UploadTaskStatus.PAUSED
        task.isUploading = false
        val rt = runtime(task)
        task.fileJob?.cancel()
        releaseRuntime(task)
        emitTasks()
        startNextTasks()
    }

    /** 恢复单个任务：按会话状态选择 单文件直传 / 分片续传 / 重新初始化。 */
    fun resumeTask(task: FileTask) {
        if (task.status != UploadTaskStatus.PAUSED) return
        task.status = UploadTaskStatus.UPLOADING
        task.errorMessage = ""
        task.isUploading = true
        val rt = runtime(task)
        task.fileJob = scope.launch {
            when {
                // 小文件直传已拿到签名：继续 PutObject（403 时内部会重建会话）
                task.uploadId != null &&
                    (task.uploadMode == UploadMode.SINGLE || task.fileSize <= UploadConfig.SINGLE_UPLOAD_LIMIT) ->
                    runSingleFlow(task)

                // 大文件会话仍有效：重建流水线，只补传缺失分片（URL 全部重新批量签名）
                task.uploadId != null ->
                    coroutineScope { startMultipartPipeline(task, rt) }

                // 哈希阶段被暂停（尚无会话）：重新进入完整流程（已算哈希可复用）
                else ->
                    runFileUpload(task)
            }
        }
        emitTasks()
    }

    /** 取消单个任务：中断传输并通知服务端清理 multipart 会话。 */
    fun cancelTask(task: FileTask) {
        if (task.status == UploadTaskStatus.CANCELLED || task.status == UploadTaskStatus.COMPLETED) return
        pauseTimer(task)
        task.status = UploadTaskStatus.CANCELLED
        task.isUploading = false
        val rt = runtime(task)
        task.fileJob?.cancel()
        releaseRuntime(task)
        val uploadId = task.uploadId
        if (uploadId != null) {
            scope.launch { runCatching { api.abortUpload(uploadId) } }
        }
        emitTasks()
        startNextTasks()
    }

    /** 失败重试单个任务：保留已上传分片，只补传缺失部分。 */
    fun retryTask(task: FileTask) {
        if (task.status != UploadTaskStatus.FAILED) return
        task.status = UploadTaskStatus.UPLOADING
        task.errorMessage = ""
        if (task.uploadId != null && task.uploadMode != UploadMode.SINGLE) {
            // 会话仍有效：直接重建流水线，只补传缺失分片
            task.isUploading = true
            task.totalChunks = ceil(
                task.fileSize / (task.chunkSize.takeIf { it > 0 } ?: task.fileSize).toDouble()
            ).toInt().coerceAtLeast(1)
            val rt = runtime(task)
            task.fileJob = scope.launch { coroutineScope { startMultipartPipeline(task, rt) } }
        } else {
            // 会话无效：清空会话信息，重新走初始化（保留内容哈希，服务端可秒传/续传）
            task.isUploading = false
            task.uploadId = null
            task.putUrl = null
            task.path = null
            startNextTasks()
        }
        emitTasks()
    }

    /** 重新建立任务状态并再次排队，保留 SHA-256 以便服务端继续秒传或续传。 */
    fun restartTask(task: FileTask) {
        val rt = runtime(task)
        task.fileJob?.cancel()
        releaseRuntime(task)
        task.status = UploadTaskStatus.WAITING
        task.isUploading = false
        task.errorMessage = ""
        task.uploadStartedAt = null
        task.elapsedUploadMs = 0
        task.uploadSpeed = 0.0
        task.remainingSeconds = 0.0
        task.uploadedBytes = 0
        task.remainingBytes = task.fileSize
        emitTasks()
        startNextTasks()
    }

    /** 删除单个任务（活跃任务先取消并通知服务端清理）。 */
    fun removeTask(task: FileTask) {
        if (task.status == UploadTaskStatus.UPLOADING || task.status == UploadTaskStatus.WAITING ||
            task.status == UploadTaskStatus.PAUSED || task.status == UploadTaskStatus.HASHING
        ) {
            task.status = UploadTaskStatus.CANCELLED
            task.isUploading = false
            val rt = runtime(task)
            task.fileJob?.cancel()
            releaseRuntime(task)
            val uploadId = task.uploadId
            if (uploadId != null) {
                scope.launch { runCatching { api.abortUpload(uploadId) } }
            }
        }
        _fileTasks.update { list -> list.filterNot { it.uid == task.uid } }
        runtimes.remove(task.uid)
        startNextTasks()
    }

    /** 清空所有上传任务。 */
    fun clearAllTasks() {
        for (task in _fileTasks.value) {
            if (task.status == UploadTaskStatus.UPLOADING || task.status == UploadTaskStatus.HASHING ||
                task.status == UploadTaskStatus.PAUSED || task.status == UploadTaskStatus.WAITING
            ) {
                task.status = UploadTaskStatus.CANCELLED
                task.isUploading = false
                val rt = runtime(task)
                task.fileJob?.cancel()
                releaseRuntime(task)
                val uploadId = task.uploadId
                if (uploadId != null) {
                    scope.launch { runCatching { api.abortUpload(uploadId) } }
                }
            }
        }
        _fileTasks.value = emptyList()
        runtimes.clear()
    }

    // =========================================================================
    // 错误分类与状态发布
    // =========================================================================

    /** 判断错误是否适合自动重试（网络异常 / 429 限流 / 5xx 服务端错误）。 */
    private fun isRetryableError(e: Exception): Boolean = when (e) {
        is RequestError -> e.status == 429 || e.status >= 500
        is IOException -> true
        else -> e.message?.let { it.contains("Network") || it.contains("网络") } == true
    }

    /**
     * 发布任务列表快照（浅拷贝），驱动 Compose 重组。
     *
     * 任务对象是**原地修改**的，所以必须先递增每个任务的 `revision`：StateFlow 只在
     * `newValue != oldValue` 时发射，而 List 的比较落到 FileTask.equals（含 revision）——
     * 不递增的话新旧列表逐元素相等，发布等于没发生，界面永远不刷新。
     */
    private fun emitTasks() {
        val list = _fileTasks.value

        for (task in list) {
            task.revision += 1
        }

        _fileTasks.value = list.toList()
    }
}
