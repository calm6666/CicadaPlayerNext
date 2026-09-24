package com.cicada.player.compose.upload.ui

import android.graphics.Bitmap
import android.media.MediaMetadataRetriever
import android.net.Uri
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.DateRange
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.InputChip
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import coil.compose.AsyncImage
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import com.cicada.player.compose.upload.UploadViewModel
import com.cicada.player.compose.upload.core.UploadFormat
import com.cicada.player.compose.upload.model.FileTask
import com.cicada.player.compose.upload.model.UploadTaskStatus

/**
 * ============================================================================
 * UploadScreen.kt —— 视频投稿页（B 站 / 抖音风格）
 * ============================================================================
 *
 * 设计语言（与后续 iOS 版保持一致）：
 *  - 品牌粉 #FB7299，浅灰背景 #F6F7FB，白色圆角卡片（16dp 圆角）；
 *  - 顶部：返回 + 「视频投稿」标题 + 右侧「发布」胶囊按钮（渐变禁用态）；
 *  - 中部：视频选择/上传任务卡片（缩略图、进度、速度、剩余时间、状态徽标、
 *          暂停/继续/取消/重试操作）+ 投稿信息（标题/分区/标签/简介/封面）
 *          + 发布设置；
 *  - 底部：上传队列汇总条（总进度 + 全部开始/暂停/清空）。
 *
 * 数据流：本页只读 UploadViewModel 暴露的 StateFlow（上传器节流发布），
 * 不直接触碰网络层；提示消息经 SharedFlow 收集后 Toast 展示。
 */

/** 上传页配色与常量（与品牌视觉规范对齐）。 */
private object UploadUi {
    /** 品牌粉（B 站粉，与播放器顶栏一致）。 */
    val BrandPink = Color(0xFFFB7299)

    /** 品牌粉浅色（芯片选中背景）。 */
    val BrandPinkLight = Color(0xFFFCE9EF)

    /** 页面浅灰背景。 */
    val Bg = Color(0xFFF6F7FB)

    /** 进度条轨道色。 */
    val Track = Color(0xFFF0E4E8)

    /** 次要文字色。 */
    val TextSecondary = Color(0xFF8A8A8E)

    /** 成功绿。 */
    val Success = Color(0xFF34C759)

    /** 警告橙（哈希中）。 */
    val Hashing = Color(0xFFF5A623)

    /** 失败红。 */
    val Error = Color(0xFFE53935)

    /** 暂停蓝灰。 */
    val Paused = Color(0xFF7A8CA8)

    /** 中性灰（排队/取消）。 */
    val Neutral = Color(0xFF9E9E9E)

    /** 缩略图底色。 */
    val ThumbBg = Color(0xFF2B2B2B)

    /** 可选分区（B 站风格主分区示例，可按运营配置扩展）。 */
    val CATEGORIES = listOf(
        "动画", "音乐", "舞蹈", "游戏", "知识", "科技",
        "生活", "美食", "影视", "时尚", "数码", "鬼畜",
    )
}

/**
 * 投稿页入口。
 *
 * @param onClose 返回回调
 */
@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
fun UploadScreen(onClose: () -> Unit) {
    val vm: UploadViewModel = viewModel()
    val context = LocalContext.current

    // 上传器状态（节流发布，进度平滑）
    val tasks by vm.tasks.collectAsStateWithLifecycle()
    val uploadingCount by vm.uploadingCount.collectAsStateWithLifecycle()
    val overallProgress by vm.overallProgress.collectAsStateWithLifecycle()

    // 标签输入框文本（本地瞬时状态）
    var tagInput by remember { mutableStateOf("") }

    // 视频多选（SAF：无需存储权限）
    val pickVideos = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenMultipleDocuments()
    ) { uris -> if (uris.isNotEmpty()) vm.addVideos(uris) }

    // 封面单选
    val pickCover = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri -> if (uri != null) vm.setCover(uri) }

    // 收集上传器提示消息（秒传/恢复分片/重复文件等）
    LaunchedEffect(Unit) {
        vm.messages.collect { Toast.makeText(context, it, Toast.LENGTH_SHORT).show() }
    }

    Scaffold(
        containerColor = UploadUi.Bg,
        topBar = {
            UploadTopBar(
                canPublish = vm.canPublish(),
                isPublishing = vm.isPublishing,
                onBack = onClose,
                onPublish = {
                    vm.publish { ok, message ->
                        Toast.makeText(context, message, Toast.LENGTH_SHORT).show()
                        if (ok) onClose()
                    }
                },
            )
        },
        bottomBar = {
            // 有任务时展示上传队列汇总条
            if (tasks.isNotEmpty()) {
                UploadBottomBar(
                    taskCount = tasks.size,
                    uploadingCount = uploadingCount,
                    overallProgress = overallProgress,
                    onStartAll = vm::startAll,
                    onPauseAll = vm::pauseAll,
                    onClearAll = vm::clearAllTasks,
                )
            }
        },
    ) { padding ->
        LazyColumn(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding),
            contentPadding = PaddingValues(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            // ---- 视频选择 + 上传任务 ----
            item {
                VideoSection(
                    tasks = tasks,
                    vm = vm,
                    onPickVideos = { pickVideos.launch(arrayOf("video/*")) },
                )
            }

            // ---- 投稿信息 ----
            item {
                PublishInfoSection(
                    vm = vm,
                    tagInput = tagInput,
                    onTagInputChange = { tagInput = it },
                    onAddTag = {
                        vm.addTag(tagInput)
                        tagInput = ""
                    },
                    onPickCover = { pickCover.launch(arrayOf("image/*")) },
                )
            }

            // ---- 发布设置 ----
            item { PublishSettingsSection() }

            item { Spacer(modifier = Modifier.height(4.dp)) }
        }
    }
}

// ============================================================================
// 顶部栏
// ============================================================================

/**
 * 顶部栏：返回 + 标题 + 右侧「发布」胶囊按钮。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun UploadTopBar(
    canPublish: Boolean,
    isPublishing: Boolean,
    onBack: () -> Unit,
    onPublish: () -> Unit,
) {
    TopAppBar(
        title = { Text("视频投稿", fontWeight = FontWeight.SemiBold) },
        navigationIcon = {
            IconButton(onClick = onBack) {
                Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "返回")
            }
        },
        actions = {
            Button(
                onClick = onPublish,
                enabled = canPublish && !isPublishing,
                shape = RoundedCornerShape(20.dp),
                colors = ButtonDefaults.buttonColors(
                    containerColor = UploadUi.BrandPink,
                    disabledContainerColor = UploadUi.BrandPink.copy(alpha = 0.4f),
                    disabledContentColor = Color.White,
                ),
                contentPadding = PaddingValues(horizontal = 22.dp, vertical = 8.dp),
                modifier = Modifier.padding(end = 12.dp),
            ) {
                if (isPublishing) {
                    CircularProgressIndicator(
                        modifier = Modifier.size(16.dp),
                        color = Color.White,
                        strokeWidth = 2.dp,
                    )
                } else {
                    Text("发布", fontWeight = FontWeight.Medium)
                }
            }
        },
        colors = TopAppBarDefaults.topAppBarColors(containerColor = Color.White),
    )
}

// ============================================================================
// 底部上传队列汇总条
// ============================================================================

/**
 * 底部汇总条：文件数、总进度、全部开始/暂停、清空。
 */
@Composable
private fun UploadBottomBar(
    taskCount: Int,
    uploadingCount: Int,
    overallProgress: Float,
    onStartAll: () -> Unit,
    onPauseAll: () -> Unit,
    onClearAll: () -> Unit,
) {
    Surface(shadowElevation = 12.dp, color = Color.White) {
        Column(modifier = Modifier.padding(horizontal = 16.dp, vertical = 10.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    text = "$taskCount 个视频 · 上传中 $uploadingCount 个",
                    fontSize = 13.sp,
                    color = UploadUi.TextSecondary,
                    modifier = Modifier.weight(1f),
                )
                Text(
                    text = "已上传 ${(overallProgress * 100).toInt()}%",
                    fontSize = 13.sp,
                    fontWeight = FontWeight.SemiBold,
                    color = UploadUi.BrandPink,
                )
            }
            Spacer(modifier = Modifier.height(8.dp))
            GradientProgressBar(overallProgress)
            Spacer(modifier = Modifier.height(4.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                TextButton(onClick = onStartAll) {
                    Text("全部开始", color = UploadUi.BrandPink, fontSize = 13.sp)
                }
                TextButton(onClick = onPauseAll) {
                    Text("全部暂停", color = UploadUi.Paused, fontSize = 13.sp)
                }
                Spacer(modifier = Modifier.weight(1f))
                TextButton(onClick = onClearAll) {
                    Icon(
                        Icons.Default.Delete,
                        contentDescription = null,
                        modifier = Modifier.size(16.dp),
                        tint = UploadUi.Neutral,
                    )
                    Spacer(modifier = Modifier.width(4.dp))
                    Text("清空", color = UploadUi.Neutral, fontSize = 13.sp)
                }
            }
        }
    }
}

// ============================================================================
// 视频选择与上传任务
// ============================================================================

/**
 * 视频选择区：无任务时展示大选择卡片，有任务时展示任务卡片列表 + 添加按钮。
 */
@Composable
private fun VideoSection(
    tasks: List<FileTask>,
    vm: UploadViewModel,
    onPickVideos: () -> Unit,
) {
    if (tasks.isEmpty()) {
        // 空态：大号选择卡片（虚线效果以淡粉实线边框模拟）
        // 注意：必须挂 clickable 才能弹出视频选择器（点击卡片任意位置均可触发）
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .height(170.dp)
                .clip(RoundedCornerShape(16.dp))
                .background(Color.White)
                .border(1.5.dp, UploadUi.BrandPink.copy(alpha = 0.45f), RoundedCornerShape(16.dp))
                .clickable(
                    role = Role.Button,
                    onClickLabel = "选择视频",
                    onClick = onPickVideos,
                )
                .padding(16.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            Box(
                modifier = Modifier
                    .size(56.dp)
                    .clip(CircleShape)
                    .background(UploadUi.BrandPinkLight),
                contentAlignment = Alignment.Center,
            ) {
                Icon(
                    Icons.Default.PlayArrow,
                    contentDescription = null,
                    tint = UploadUi.BrandPink,
                    modifier = Modifier.size(30.dp),
                )
            }
            Spacer(modifier = Modifier.height(12.dp))
            Text(
                text = "选择视频",
                fontSize = 16.sp,
                fontWeight = FontWeight.SemiBold,
            )
            Spacer(modifier = Modifier.height(4.dp))
            Text(
                text = "支持多选，将自动开始上传",
                fontSize = 12.sp,
                color = UploadUi.TextSecondary,
            )
        }
        Spacer(modifier = Modifier.height(0.dp))
        return
    }

    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        tasks.forEach { task -> UploadTaskCard(task = task, vm = vm) }
        OutlinedButton(
            onClick = onPickVideos,
            modifier = Modifier.fillMaxWidth(),
            shape = RoundedCornerShape(12.dp),
            border = androidx.compose.foundation.BorderStroke(1.dp, UploadUi.BrandPink),
        ) {
            Icon(
                Icons.Default.Add,
                contentDescription = null,
                modifier = Modifier.size(16.dp),
                tint = UploadUi.BrandPink,
            )
            Spacer(modifier = Modifier.width(4.dp))
            Text("添加更多视频", color = UploadUi.BrandPink)
        }
    }
}

/**
 * 单个上传任务卡片：缩略图、名称、大小、状态徽标、进度、速度、剩余时间与操作。
 */
@Composable
private fun UploadTaskCard(task: FileTask, vm: UploadViewModel) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(16.dp),
        colors = CardDefaults.cardColors(containerColor = Color.White),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            // ---- 头部：缩略图 + 名称/状态 ----
            Row(verticalAlignment = Alignment.CenterVertically) {
                VideoThumbnail(uri = task.uri)
                Spacer(modifier = Modifier.width(12.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = task.fileName,
                        fontSize = 14.sp,
                        fontWeight = FontWeight.SemiBold,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    Spacer(modifier = Modifier.height(6.dp))
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        StatusBadge(task = task)
                        Spacer(modifier = Modifier.width(8.dp))
                        Text(
                            text = UploadFormat.formatFileSize(task.fileSize),
                            fontSize = 12.sp,
                            color = UploadUi.TextSecondary,
                        )
                    }
                }
            }

            Spacer(modifier = Modifier.height(10.dp))

            // ---- 进度条 + 百分比 + 速度/剩余时间 ----
            GradientProgressBar(task.progress)
            Spacer(modifier = Modifier.height(6.dp))
            Row {
                Text(
                    text = "${(task.progress * 100).toInt()}%",
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                    color = UploadUi.BrandPink,
                    modifier = Modifier.weight(1f),
                )
                if (task.status == UploadTaskStatus.UPLOADING) {
                    Text(
                        text = "${UploadFormat.formatUploadSpeed(task.uploadSpeed)} · 剩余 ${UploadFormat.formatRemainingTime(task.remainingSeconds)}",
                        fontSize = 11.sp,
                        color = UploadUi.TextSecondary,
                    )
                }
            }

            // ---- 错误信息 ----
            if (task.status == UploadTaskStatus.FAILED && task.errorMessage.isNotBlank()) {
                Spacer(modifier = Modifier.height(4.dp))
                Text(
                    text = task.errorMessage,
                    fontSize = 11.sp,
                    color = UploadUi.Error,
                )
            }

            // ---- 操作按钮 ----
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.End,
            ) {
                TaskActions(task = task, vm = vm)
            }
        }
    }
}

/** 视频缩略图：后台 IO 取首帧并缩放，带播放角标。 */
@Composable
private fun VideoThumbnail(uri: Uri) {
    val frame = rememberVideoFrame(uri)
    Box(
        modifier = Modifier
            .size(width = 96.dp, height = 68.dp)
            .clip(RoundedCornerShape(10.dp))
            .background(UploadUi.ThumbBg),
    ) {
        if (frame != null) {
            Image(
                bitmap = frame.asImageBitmap(),
                contentDescription = "视频缩略图",
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.Crop,
            )
        }
        Icon(
            Icons.Default.PlayArrow,
            contentDescription = null,
            tint = Color.White.copy(alpha = 0.92f),
            modifier = Modifier.align(Alignment.Center),
        )
    }
}

/** 状态徽标：不同状态不同配色。 */
@Composable
private fun StatusBadge(task: FileTask) {
    val (text, color) = when (task.status) {
        UploadTaskStatus.WAITING -> "排队中" to UploadUi.Neutral
        UploadTaskStatus.HASHING -> "计算摘要中" to UploadUi.Hashing
        UploadTaskStatus.UPLOADING -> "上传中" to UploadUi.BrandPink
        UploadTaskStatus.PAUSED -> "已暂停" to UploadUi.Paused
        UploadTaskStatus.COMPLETED -> "已完成" to UploadUi.Success
        UploadTaskStatus.FAILED -> "上传失败" to UploadUi.Error
        UploadTaskStatus.CANCELLED -> "已取消" to UploadUi.Neutral
    }
    Row(
        modifier = Modifier
            .clip(RoundedCornerShape(6.dp))
            .background(color.copy(alpha = 0.12f))
            .padding(horizontal = 6.dp, vertical = 2.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (task.status == UploadTaskStatus.COMPLETED) {
            Icon(
                Icons.Default.CheckCircle,
                contentDescription = null,
                modifier = Modifier.size(12.dp),
                tint = color,
            )
            Spacer(modifier = Modifier.width(3.dp))
        }
        Text(text = text, fontSize = 10.sp, color = color)
    }
}

/** 任务操作按钮：随状态切换 暂停/继续/取消/重试/移除/重新开始。 */
@Composable
private fun TaskActions(task: FileTask, vm: UploadViewModel) {
    when (task.status) {
        UploadTaskStatus.WAITING, UploadTaskStatus.HASHING -> {
            TextButton(onClick = { vm.cancelTask(task) }) {
                Text("取消", fontSize = 13.sp, color = UploadUi.Neutral)
            }
        }

        UploadTaskStatus.UPLOADING -> {
            TextButton(onClick = { vm.pauseTask(task) }) {
                Text("暂停", fontSize = 13.sp, color = UploadUi.BrandPink)
            }
        }

        UploadTaskStatus.PAUSED -> {
            TextButton(onClick = { vm.resumeTask(task) }) {
                Text("继续", fontSize = 13.sp, color = UploadUi.BrandPink)
            }
            TextButton(onClick = { vm.cancelTask(task) }) {
                Text("取消", fontSize = 13.sp, color = UploadUi.Neutral)
            }
        }

        UploadTaskStatus.FAILED -> {
            TextButton(onClick = { vm.retryTask(task) }) {
                Text("重试", fontSize = 13.sp, color = UploadUi.BrandPink)
            }
            TextButton(onClick = { vm.removeTask(task) }) {
                Text("移除", fontSize = 13.sp, color = UploadUi.Neutral)
            }
        }

        UploadTaskStatus.COMPLETED -> {
            TextButton(onClick = { vm.removeTask(task) }) {
                Text("移除", fontSize = 13.sp, color = UploadUi.Neutral)
            }
        }

        UploadTaskStatus.CANCELLED -> {
            TextButton(onClick = { vm.restartTask(task) }) {
                Text("重新开始", fontSize = 13.sp, color = UploadUi.BrandPink)
            }
            TextButton(onClick = { vm.removeTask(task) }) {
                Text("移除", fontSize = 13.sp, color = UploadUi.Neutral)
            }
        }
    }
}

// ============================================================================
// 投稿信息（标题 / 分区 / 标签 / 简介 / 封面）
// ============================================================================

/**
 * 投稿信息卡片。
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun PublishInfoSection(
    vm: UploadViewModel,
    tagInput: String,
    onTagInputChange: (String) -> Unit,
    onAddTag: () -> Unit,
    onPickCover: () -> Unit,
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(16.dp),
        colors = CardDefaults.cardColors(containerColor = Color.White),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            SectionHeader(title = "投稿信息", subtitle = "完善内容更容易被推荐哦")

            // ---- 标题 ----
            OutlinedTextField(
                value = vm.title,
                onValueChange = vm::updateTitle,
                modifier = Modifier.fillMaxWidth(),
                placeholder = { Text("填写一个吸引人的标题吧～", color = UploadUi.TextSecondary) },
                singleLine = true,
                shape = RoundedCornerShape(12.dp),
                supportingText = { Text("${vm.title.length}/80", color = UploadUi.TextSecondary) },
            )

            Spacer(modifier = Modifier.height(14.dp))

            // ---- 分区 ----
            FieldLabel("选择分区")
            FlowRow(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                UploadUi.CATEGORIES.forEach { category ->
                    FilterChip(
                        selected = vm.category == category,
                        onClick = { vm.updateCategory(category) },
                        label = { Text(category, fontSize = 12.sp) },
                        shape = RoundedCornerShape(8.dp),
                        colors = FilterChipDefaults.filterChipColors(
                            selectedContainerColor = UploadUi.BrandPinkLight,
                            selectedLabelColor = UploadUi.BrandPink,
                        ),
                    )
                }
            }

            Spacer(modifier = Modifier.height(14.dp))

            // ---- 标签 ----
            FieldLabel("添加标签（最多 10 个）")
            Row(verticalAlignment = Alignment.CenterVertically) {
                OutlinedTextField(
                    value = tagInput,
                    onValueChange = onTagInputChange,
                    modifier = Modifier.weight(1f),
                    placeholder = { Text("输入标签后点添加", fontSize = 13.sp, color = UploadUi.TextSecondary) },
                    singleLine = true,
                    shape = RoundedCornerShape(12.dp),
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                    keyboardActions = KeyboardActions(onDone = { onAddTag() }),
                )
                Spacer(modifier = Modifier.width(8.dp))
                OutlinedButton(
                    onClick = onAddTag,
                    shape = RoundedCornerShape(12.dp),
                    border = androidx.compose.foundation.BorderStroke(1.dp, UploadUi.BrandPink),
                ) {
                    Icon(
                        Icons.Default.Add,
                        contentDescription = null,
                        modifier = Modifier.size(16.dp),
                        tint = UploadUi.BrandPink,
                    )
                }
            }
            if (vm.tags.isNotEmpty()) {
                Spacer(modifier = Modifier.height(8.dp))
                FlowRow(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    vm.tags.forEach { tag ->
                        InputChip(
                            selected = false,
                            onClick = { vm.removeTag(tag) },
                            label = { Text(tag, fontSize = 12.sp) },
                            trailingIcon = {
                                Icon(
                                    Icons.Default.Close,
                                    contentDescription = "移除标签",
                                    modifier = Modifier.size(14.dp),
                                    tint = UploadUi.TextSecondary,
                                )
                            },
                            shape = RoundedCornerShape(8.dp),
                        )
                    }
                }
            }

            Spacer(modifier = Modifier.height(14.dp))

            // ---- 简介 ----
            OutlinedTextField(
                value = vm.description,
                onValueChange = vm::updateDescription,
                modifier = Modifier.fillMaxWidth(),
                placeholder = { Text("介绍一下你的视频内容吧～", color = UploadUi.TextSecondary) },
                minLines = 4,
                shape = RoundedCornerShape(12.dp),
            )

            Spacer(modifier = Modifier.height(14.dp))

            // ---- 封面 ----
            FieldLabel("设置封面")
            CoverPicker(coverUri = vm.coverUri, onPickCover = onPickCover)
        }
    }
}

/** 分区/标签小标题。 */
@Composable
private fun FieldLabel(text: String) {
    Text(
        text = text,
        fontSize = 12.sp,
        color = UploadUi.TextSecondary,
        modifier = Modifier.padding(bottom = 8.dp),
    )
}

/** 区块标题。 */
@Composable
private fun SectionHeader(title: String, subtitle: String) {
    Text(text = title, fontSize = 16.sp, fontWeight = FontWeight.SemiBold)
    Spacer(modifier = Modifier.height(2.dp))
    Text(text = subtitle, fontSize = 12.sp, color = UploadUi.TextSecondary)
    Spacer(modifier = Modifier.height(14.dp))
}

/** 封面选择器：16:9 预览 + 上传/更换按钮。 */
@Composable
private fun CoverPicker(coverUri: Uri?, onPickCover: () -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(
            modifier = Modifier
                .size(width = 132.dp, height = 76.dp)
                .clip(RoundedCornerShape(10.dp))
                .background(UploadUi.Track),
            contentAlignment = Alignment.Center,
        ) {
            if (coverUri != null) {
                AsyncImage(
                    model = coverUri,
                    contentDescription = "封面预览",
                    modifier = Modifier.fillMaxSize(),
                    contentScale = ContentScale.Crop,
                )
            } else {
                Text(
                    text = "16 : 9",
                    fontSize = 12.sp,
                    color = UploadUi.TextSecondary,
                )
            }
        }
        Spacer(modifier = Modifier.width(12.dp))
        Column {
            OutlinedButton(
                onClick = onPickCover,
                shape = RoundedCornerShape(10.dp),
                border = androidx.compose.foundation.BorderStroke(1.dp, UploadUi.BrandPink),
            ) {
                Icon(
                    Icons.Default.Add,
                    contentDescription = null,
                    modifier = Modifier.size(16.dp),
                    tint = UploadUi.BrandPink,
                )
                Spacer(modifier = Modifier.width(4.dp))
                Text(
                    text = if (coverUri != null) "更换封面" else "上传封面",
                    color = UploadUi.BrandPink,
                    fontSize = 13.sp,
                )
            }
            Spacer(modifier = Modifier.height(6.dp))
            Text(
                text = "推荐 16:9，大小不超过 2MB",
                fontSize = 11.sp,
                color = UploadUi.TextSecondary,
            )
        }
    }
}

// ============================================================================
// 发布设置
// ============================================================================

/**
 * 发布设置卡片：仅粉丝可见 / 定时发布（本地占位状态，可接后端草稿接口）。
 */
@Composable
private fun PublishSettingsSection() {
    var fansOnly by remember { mutableStateOf(false) }
    var scheduled by remember { mutableStateOf(false) }

    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(16.dp),
        colors = CardDefaults.cardColors(containerColor = Color.White),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
    ) {
        Column {
            SettingRow(
                icon = Icons.Default.Lock,
                title = "仅粉丝可见",
                checked = fansOnly,
                onCheckedChange = { fansOnly = it },
            )
            HorizontalDivider(color = UploadUi.Bg, thickness = 1.dp)
            SettingRow(
                icon = Icons.Default.DateRange,
                title = "定时发布",
                checked = scheduled,
                onCheckedChange = { scheduled = it },
            )
        }
    }
}

/** 设置行：图标 + 标题 + 开关。 */
@Composable
private fun SettingRow(
    icon: ImageVector,
    title: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(
            imageVector = icon,
            contentDescription = null,
            modifier = Modifier.size(18.dp),
            tint = UploadUi.TextSecondary,
        )
        Spacer(modifier = Modifier.width(12.dp))
        Text(
            text = title,
            fontSize = 14.sp,
            modifier = Modifier.weight(1f),
        )
        Switch(
            checked = checked,
            onCheckedChange = onCheckedChange,
            colors = SwitchDefaults.colors(checkedTrackColor = UploadUi.BrandPink),
        )
    }
}

// ============================================================================
// 公共小组件
// ============================================================================

/**
 * 渐变进度条（品牌粉渐变），带 0→目标值 平滑动画。
 */
@Composable
private fun GradientProgressBar(progress: Float) {
    val animated by animateFloatAsState(
        targetValue = progress.coerceIn(0f, 1f),
        label = "uploadProgress",
    )
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .height(6.dp)
            .clip(RoundedCornerShape(3.dp))
            .background(UploadUi.Track),
    ) {
        Box(
            modifier = Modifier
                .fillMaxHeight()
                .fillMaxWidth(animated)
                .clip(RoundedCornerShape(3.dp))
                .background(
                    Brush.horizontalGradient(
                        listOf(UploadUi.BrandPink, Color(0xFFFC8FA9))
                    )
                ),
        )
    }
}

/**
 * 读取视频首帧缩略图（后台 IO，按宽度缩放，失败返回 null）。
 */
@Composable
private fun rememberVideoFrame(uri: Uri): Bitmap? {
    val context = LocalContext.current
    var bitmap by remember(uri) { mutableStateOf<Bitmap?>(null) }
    LaunchedEffect(uri) {
        bitmap = withContext(Dispatchers.IO) {
            runCatching {
                val retriever = MediaMetadataRetriever()
                try {
                    retriever.setDataSource(context, uri)
                    val frame = retriever.getFrameAtTime(
                        0,
                        MediaMetadataRetriever.OPTION_CLOSEST_SYNC,
                    ) ?: return@runCatching null
                    // 缩略图只需 480px 宽，避免大图占内存
                    val targetWidth = 480
                    val scale = targetWidth.toFloat() / frame.width
                    val targetHeight = (frame.height * scale).toInt().coerceAtLeast(1)
                    Bitmap.createScaledBitmap(frame, targetWidth, targetHeight, true)
                } finally {
                    retriever.release()
                }
            }.getOrNull()
        }
    }
    return bitmap
}
