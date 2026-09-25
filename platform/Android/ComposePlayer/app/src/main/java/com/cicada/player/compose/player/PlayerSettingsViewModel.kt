package com.cicada.player.compose.player

import android.app.Application
import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import com.cicada.player.compose.components.video.ColorVisionMode
import com.cicada.player.compose.components.video.MirrorUiMode

/**
 * ============ 播放器设置的持有者（**唯一真源**）============
 *
 * 为什么是 **ViewModel** 而不是 `remember` / `rememberSaveable`：
 *
 *   ① `Navigation Compose` 里 `popBackStack()` 会把播放页的 composable 从组合里摘掉，
 *      它内部所有 `remember` 状态随之销毁 —— 这正是用户报的
 *      "关掉硬解 → 离开界面 → 再回来开关又打开了"。ViewModel 挂在 NavBackStackEntry /
 *      Activity 的 ViewModelStore 上，**不随一次组合的销毁而丢**，正好治这一类病；
 *   ② 只靠 ViewModel 还不够：用户在播放页里关掉硬解、然后**杀进程重开 App**，
 *      ViewModel 也没了。所以这里再叠一层 [PlayerSettings]（SharedPreferences）落盘，
 *      做到"跨界面 + 跨进程"存活。ViewModel 负责进程内响应式，落盘负责跨进程，
 *      两者缺一不可（这也是为什么没有选"只加 rememberSaveable"那种最次方案）。
 *
 * 为什么**不复用** `components/video/PlayerViewModel`：
 * 那个 ViewModel 自己持有一个 `CicadaPlayerController`（= 一个内核 player 实例），
 * 而 `CicadaVideoPlayer` 现在是自己 `remember { CicadaPlayerController(context) }` 建实例的。
 * 把两者合并会变成"一个页面两个内核 player 实例"（native 层两份解码器/两份 Surface 绑定），
 * 那不是本轮要解决的问题、风险也远大于收益 —— 所以本类**只负责设置，不碰播放器**。
 *
 * 线程：所有 setter 都只在 Compose 事件回调（主线程）里调用，`mutableStateOf` 的写入
 * 因此天然在主线程，不需要额外的 post 或同步。
 */
class PlayerSettingsViewModel(application: Application) : AndroidViewModel(application) {

    /** 当前设置（data class，整体替换 = 一次原子更新） */
    var settings by mutableStateOf(PlayerSettings.load(application))
        private set

    /**
     * 统一的写入点：**先更新内存里的响应式状态，再落盘，再打一行日志**。
     *
     * 顺序不能反：落盘是异步的（`apply()`），用户点完开关必须**立刻**看到 UI 变化；
     * 而日志放在最后，是为了让它记下"确实已经接受的这一份值"。
     *
     * 日志 tag = `CicadaSettings`：**点击即打**、事件驱动，**没有任何计时器/看门狗**。
     * 真机验收时用 `adb logcat -s CicadaSettings` 就能看到"用户确实点了、点了什么"。
     */
    private fun update(tag: String, next: PlayerSettings) {
        settings = next
        PlayerSettings.save(getApplication(), next)
        Log.i(TAG_SETTINGS, "$tag -> $next")
    }

    /** 硬解开关（软解验证入口）。**只影响下一次建解码器**，见 CicadaPlayerController 的说明。 */
    fun setHardwareDecodeEnabled(enabled: Boolean) {
        update(
            "hardwareDecode=$enabled",
            settings.copy(hardwareDecodeEnabled = enabled),
        )
    }

    /** 镜像画面三态 */
    fun setMirrorMode(mode: MirrorUiMode) {
        update("mirror=${mode.name}", settings.copy(mirrorMode = mode))
    }

    /** 色觉辅助四档（None = 单位矩阵 = 关闭） */
    fun setColorVisionMode(mode: ColorVisionMode) {
        update("colorVision=${mode.name}", settings.copy(colorVisionMode = mode))
    }

    /** 后台播放：开 = 切后台继续播；关 = 切后台暂停、回前台续播 */
    fun setBackgroundPlayEnabled(enabled: Boolean) {
        update(
            "backgroundPlay=$enabled",
            settings.copy(backgroundPlayEnabled = enabled),
        )
    }

    /** 弹幕总开关（GPU 弹幕层的 isEnabled） */
    fun setDanmakuEnabled(enabled: Boolean) {
        update("danmakuEnabled=$enabled", settings.copy(danmakuEnabled = enabled))
    }

    companion object {
        /** 设置类日志 tag（与硬解链路的 `CicadaDecode`、镜像的 `CicadaMirror` 分开，便于过滤） */
        private const val TAG_SETTINGS = "CicadaSettings"
    }
}
