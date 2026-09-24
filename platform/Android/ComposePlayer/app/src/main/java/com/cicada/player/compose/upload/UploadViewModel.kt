package com.cicada.player.compose.upload

import android.app.Application
import android.content.Context
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import com.cicada.player.compose.upload.core.MultipartUploader
import com.cicada.player.compose.upload.model.FileTask
import com.cicada.player.compose.upload.model.UploadTaskStatus
import com.cicada.player.compose.upload.network.ApiClient
import com.cicada.player.compose.upload.network.UploadApi

/**
 * ============================================================================
 * UploaderHolder.kt —— 上传器进程级单例装配
 * ============================================================================
 *
 * 上传器持有「应用级」协程作用域（SupervisorJob + Default 调度器），
 * 因此即便投稿页面被关闭，后台传输仍可继续（符合 B 站/抖音上传体验）。
 *
 * 依赖装配顺序：
 *   ApiConfig（网关地址/令牌） → ApiClient（信封+命名转换）
 *   → UploadApi（业务接口+S3直传） → MultipartUploader（并发调度）
 */
object UploaderHolder {

    /** 应用级上传调度作用域：子任务互不影响（SupervisorJob）。 */
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    @Volatile
    private var instance: MultipartUploader? = null

    /** 获取（或创建）进程级单例上传器。 */
    fun get(context: Context): MultipartUploader = instance ?: synchronized(this) {
        instance ?: MultipartUploader(
            api = UploadApi(ApiClient(ApiConfig.default())),
            contentResolver = context.applicationContext.contentResolver,
            scope = scope,
        ).also { instance = it }
    }
}

/**
 * ============================================================================
 * UploadViewModel.kt —— 投稿页视图模型
 * ============================================================================
 *
 * 职责：
 *  1. 持有上传器单例，向 UI 暴露任务列表 / 上传中数量 / 提示消息；
 *  2. 管理投稿草稿（标题、分区、标签、简介、封面），发布动作预留后端接口；
 *  3. 选择视频后自动加入队列并开始上传（B 站式体验）。
 *
 * 说明：上传任务状态保存在 MultipartUploader 中，本 VM 不复制状态，
 * 仅做 UI 所需的派生计算（总进度、可发布判定等）。
 */
class UploadViewModel(application: Application) : AndroidViewModel(application) {

    // ------------------------------------------------------------- 上传核心
    /** 上传调度器（进程级单例，页面销毁后上传继续）。 */
    val uploader: MultipartUploader = UploaderHolder.get(application)

    /** 所有上传任务。 */
    val tasks: StateFlow<List<FileTask>> = uploader.fileTasks

    /** 正在上传（含哈希中）的文件数量。 */
    val uploadingCount: StateFlow<Int> = uploader.uploadingCount

    /** 一次性提示消息（秒传/恢复分片/重复文件等）。 */
    val messages: SharedFlow<String> = uploader.messageEvents

    /** 总进度（所有任务已上传字节 / 总字节）。 */
    val overallProgress: StateFlow<Float> = tasks
        .map { list ->
            val total = list.sumOf { it.fileSize }
            if (total <= 0L) 0f
            else (list.sumOf { it.uploadedBytes }.toDouble() / total).toFloat().coerceIn(0f, 1f)
        }
        .stateIn(viewModelScope, kotlinx.coroutines.flow.SharingStarted.Eagerly, 0f)

    // ------------------------------------------------------------- 投稿草稿
    /** 投稿标题（限制 80 字）。 */
    var title by mutableStateOf("")
        private set

    /** 投稿简介。 */
    var description by mutableStateOf("")
        private set

    /** 已选分区。 */
    var category by mutableStateOf("")
        private set

    /** 已添加的标签。 */
    var tags by mutableStateOf(emptyList<String>())
        private set

    /** 封面 Uri（可为空，表示未设置封面）。 */
    var coverUri by mutableStateOf<android.net.Uri?>(null)
        private set

    /** 发布中标记（防止重复提交）。 */
    var isPublishing by mutableStateOf(false)
        private set

    /** 更新标题（截断到 80 字）。 */
    fun updateTitle(value: String) {
        title = value.take(80)
    }

    /** 更新简介（截断到 1000 字）。 */
    fun updateDescription(value: String) {
        description = value.take(1000)
    }

    /** 选择分区。 */
    fun updateCategory(value: String) {
        category = value
    }

    /** 添加标签（去重、上限 10 个、单个标签 ≤ 12 字）。 */
    fun addTag(tag: String) {
        val trimmed = tag.trim().take(12)
        if (trimmed.isEmpty() || tags.size >= 10 || trimmed in tags) return
        tags = tags + trimmed
    }

    /** 移除标签。 */
    fun removeTag(tag: String) {
        tags = tags.filterNot { it == tag }
    }

    /** 设置封面。 */
    fun setCover(uri: android.net.Uri?) {
        coverUri = uri
    }

    // ------------------------------------------------------------- 上传操作
    /**
     * 选择视频后调用：加入队列并立即开始上传（等待中的任务自动调度）。
     * 文件级并发由 uploader 控制，超出并发上限的任务保持 WAITING。
     */
    fun addVideos(uris: List<android.net.Uri>) {
        if (uris.isEmpty()) return
        viewModelScope.launch {
            uploader.addFiles(uris, parentPath = "/")
            uploader.startAllUpload()
        }
    }

    /** 全部开始（恢复暂停任务）。 */
    fun startAll() = uploader.startAllUpload()

    /** 全部暂停。 */
    fun pauseAll() {
        tasks.value.forEach { uploader.pauseTask(it) }
    }

    /** 暂停单个任务。 */
    fun pauseTask(task: FileTask) = uploader.pauseTask(task)

    /** 恢复单个任务。 */
    fun resumeTask(task: FileTask) = uploader.resumeTask(task)

    /** 取消单个任务。 */
    fun cancelTask(task: FileTask) = uploader.cancelTask(task)

    /** 失败重试。 */
    fun retryTask(task: FileTask) = uploader.retryTask(task)

    /** 重新开始（保留内容哈希，服务端可秒传/续传）。 */
    fun restartTask(task: FileTask) = uploader.restartTask(task)

    /** 移除任务。 */
    fun removeTask(task: FileTask) = uploader.removeTask(task)

    /** 清空所有任务。 */
    fun clearAllTasks() = uploader.clearAllTasks()

    // ------------------------------------------------------------- 发布判定
    /** 是否满足发布条件：标题非空 且 所有视频已上传完成。 */
    fun canPublish(): Boolean {
        val list = tasks.value
        return title.isNotBlank() &&
            list.isNotEmpty() &&
            list.all { it.status == UploadTaskStatus.COMPLETED }
    }

    /**
     * 发布投稿。
     *
     * TODO: 接入后端「投稿提交」接口（投稿标题/分区/标签/简介/封面/视频 key 等），
     * 请求体按项目约定「驼峰转下划线」发送（可复用 network/JsonNaming）。
     * 当前为占位实现：校验通过后仅提示发布成功。
     */
    fun publish(onResult: (Boolean, String) -> Unit) {
        if (!canPublish()) {
            onResult(false, "请先填写标题并等待视频上传完成")
            return
        }
        if (isPublishing) return
        isPublishing = true
        viewModelScope.launch {
            // 占位：模拟提交耗时
            kotlinx.coroutines.delay(800)
            isPublishing = false
            onResult(true, "发布成功！")
        }
    }
}
