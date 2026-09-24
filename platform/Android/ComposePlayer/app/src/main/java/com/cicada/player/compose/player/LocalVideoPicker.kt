package com.cicada.player.compose.player

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.ContextCompat
import java.io.File

/**
 * 本地视频的**读取权限**（不硬读：没权限就不碰文件，先申请）。
 *
 * 权限按系统版本分：
 *   - Android 13（API 33）及以上：`READ_MEDIA_VIDEO`；
 *   - Android 14（API 34）及以上再加 `READ_MEDIA_VISUAL_USER_SELECTED`，
 *     这样用户在"仅允许访问选中的照片和视频"那一档也能用；
 *   - Android 12（API 32）及以下：`READ_EXTERNAL_STORAGE`（manifest 里带 maxSdkVersion=32）。
 */
object LocalMediaAccess {

    /** 需要申请的读视频权限（按当前系统版本） */
    fun requiredPermissions(): Array<String> = when {
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE -> arrayOf(
            Manifest.permission.READ_MEDIA_VIDEO,
            Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED,
        )

        Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU ->
            arrayOf(Manifest.permission.READ_MEDIA_VIDEO)

        else -> arrayOf(Manifest.permission.READ_EXTERNAL_STORAGE)
    }

    /** 任意一档读权限拿到了就算有权限（API 34 的"部分允许"也算） */
    fun hasReadPermission(context: Context): Boolean = requiredPermissions().any { permission ->
        ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED
    }

    /**
     * 是不是**本地片源**：`content://`（系统选择器给的）、`file://`、或者绝对路径（`/sdcard/…`）。
     * 播放对象（`{` 开头）和网络地址（http/hls/mpd…）不算。
     */
    fun isLocalSource(source: String): Boolean {
        val value = source.trim()
        if (value.isEmpty() || value.startsWith("{")) return false
        return value.startsWith("content://") ||
            value.startsWith("file://") ||
            value.startsWith("/")
    }

    /** `file://` / 绝对路径 → 真实文件路径；`content://` 没有文件路径（返回 null） */
    fun filePathOf(source: String): String? {
        val value = source.trim()
        return when {
            value.startsWith("content://") -> null
            value.startsWith("file://") -> runCatching { Uri.parse(value).path }.getOrNull()
            value.startsWith("/") -> value
            else -> null
        }
    }

    /**
     * **真的去试读一下**（这就是"不要硬读"：先确认能读，读不到就如实报错，
     * 而不是把读不了的东西丢给内核去撞 `EACCES`）。
     */
    fun canRead(context: Context, source: String): Boolean {
        val value = source.trim()
        if (value.startsWith("content://")) {
            return runCatching {
                context.contentResolver.openInputStream(Uri.parse(value))?.use { true } ?: false
            }.getOrDefault(false)
        }
        val path = filePathOf(value) ?: return false
        return runCatching { File(path).canRead() }.getOrDefault(false)
    }

    /** 权限被拒时的提示文案（不含任何"已经播放"之类的假话） */
    const val DENIED_HINT: String =
        "还没有读取视频的权限，不能播放本地视频。请在系统设置 → 应用 → 权限里允许" +
            "“音乐和音频/视频”后重试。"

    /**
     * 跳到本应用的系统设置页。
     * 用户选了"拒绝且不再询问"之后，系统不会再弹授权框，**只能由用户自己去设置里打开**，
     * 这里只是把入口给出来，不替用户做任何决定。
     */
    fun openAppSettings(context: Context) {
        val intent = Intent(
            Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
            Uri.fromParts("package", context.packageName, null),
        ).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        runCatching { context.startActivity(intent) }
    }
}

/**
 * 本地视频选择器：**先申请权限 → 再打开系统选择器 → 把选中的 Uri 交给播放器**。
 *
 * 选择器用系统的 `ACTION_OPEN_DOCUMENT`（MIME 类型按视频通配，见下面的 launch 调用），拿到的 `content://` Uri
 * 由内核自己的 `ContentDataSource`（`framework/data_source/ContentDataSource`）读取 ——
 * 内核支持 `content://`，不需要 App 先拷贝一份文件。
 */
class LocalVideoPicker internal constructor(
    /** 申请权限 → 打开系统视频选择器 */
    private val openPicker: () -> Unit,
    /** 先确保拿到读权限，再做 [action]（没权限就弹系统授权框） */
    val ensurePermissionThen: (action: () -> Unit) -> Unit,
) {
    /** 选择本地视频 */
    fun pickVideo() = openPicker()
}

/**
 * 记住一个本地视频选择器。[onPicked] 拿到的是系统给的 `content://` Uri，
 * [onDenied] 在用户拒绝权限时调用（调用方负责提示，**不伪造播放**）。
 */
@Composable
fun rememberLocalVideoPicker(
    onPicked: (Uri) -> Unit,
    onDenied: () -> Unit,
): LocalVideoPicker {
    val context = LocalContext.current

    /* 回调都是最新的（避免闭包捕获旧值） */
    val currentOnPicked by rememberUpdatedState(onPicked)
    val currentOnDenied by rememberUpdatedState(onDenied)

    /* 权限拿到后要执行的动作（选文件 / 播放某个本地路径） */
    var pendingAction by remember { mutableStateOf<(() -> Unit)?>(null) }

    val openDocument = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        if (uri != null) {
            /* 有的 Provider 支持持久授权（下次进来还能读）；不支持就算了，不当作失败 */
            runCatching {
                context.contentResolver.takePersistableUriPermission(
                    uri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION,
                )
            }
            currentOnPicked(uri)
        }
    }

    val requestPermission = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { result ->
        val granted = result.values.any { it } || LocalMediaAccess.hasReadPermission(context)
        val action = pendingAction
        pendingAction = null
        if (granted) action?.invoke() else currentOnDenied()
    }

    /* 每次重组重新构造：里面只包两个 lambda，开销可以忽略，也不会留下过期的 launcher 引用 */
    return LocalVideoPicker(
        openPicker = {
            if (LocalMediaAccess.hasReadPermission(context)) {
                openDocument.launch(arrayOf("video/*"))
            } else {
                pendingAction = { openDocument.launch(arrayOf("video/*")) }
                requestPermission.launch(LocalMediaAccess.requiredPermissions())
            }
        },
        ensurePermissionThen = { action ->
            if (LocalMediaAccess.hasReadPermission(context)) {
                action()
            } else {
                pendingAction = action
                requestPermission.launch(LocalMediaAccess.requiredPermissions())
            }
        },
    )
}
