package com.cicada.player.compose.player

import android.content.Context
import com.cicada.player.compose.components.video.ColorVisionMode
import com.cicada.player.compose.components.video.MirrorUiMode

/**
 * ============ 播放器「用户选择」的持久化 ============
 *
 * 【为什么要有这个类（用户报的现象）】
 * 「关掉硬解开关 → 离开播放页再回来 → 开关又变成打开的」：开关状态原来是
 * `VideoPlayerScreen.kt` 里的一句 `remember { mutableStateOf(true) }`，
 * 它只活在**那一次组合**里 —— 退出播放页（Navigation popBackStack）会把那个 composable
 * 从组合里摘掉、状态随之销毁，下次进来重新 `remember` ⇒ 又回到默认的"硬解开"。
 * 于是"关硬解"这件事对**下一次播放**根本不成立（用户看到的就是"硬解开关没有实际用"）。
 *
 * 【为什么用 SharedPreferences，不用 DataStore / 新依赖】
 * 工程里已经有 `SharedPreferences`（Android 平台自带，零依赖、**不用改 Gradle**），
 * 而这里要存的只是几个布尔/少量枚举值，没有任何并发或大数据的诉求。
 * DataStore 会引入新依赖（本任务红线：不加依赖、不动 Gradle），收益为零。
 *
 * 【为什么不用 rememberSaveable】
 * `rememberSaveable` 只扛得住"配置变更 / 进程被系统回收后重建"这一类**同一个 Activity
 * 任务**里的重建，且它依赖 `SavedStateRegistry` 能一路带上这个 key。而这里要的是
 * "用户关掉硬解后，退出播放页、以后再进来、甚至杀进程重开 App 都算数"
 * —— 那是**跨界面、跨进程**的存活，只有落盘（SharedPreferences）才做得到。
 * 所以本类是最低满足方案，rememberSaveable 只是最次的兜底，本轮不用。
 *
 * 【存的是什么】
 * 只存"**用户选择**"这一类设置（硬解 / 镜像 / 色觉辅助 / 后台播放 / 弹幕开关）。
 * **不存**倍速、音量、清晰度：它们是"这一部片的观看状态"，Qt 端与主流播放器都不跨片继承，
 * 这里也不伪造那种行为（理由写在报告里）。
 *
 * 【枚举怎么存】存枚举的 **name**（不是 ordinal）：万一以后往 `MirrorUiMode` /
 * `ColorVisionMode` 中间插一档，之前存的设置仍然认得出来；认不出来的值一律回落默认档
 * （见 [PlayerSettingsViewModel.mirrorModeOf] / [PlayerSettingsViewModel.colorVisionOf]），
 * 不会因为"存了个 7"就把界面搞成非法状态。
 */
data class PlayerSettings(
    /** 硬解开关：true = 硬解，false = 纯软解起播（内核 `bEnableHwVideoDecode`） */
    val hardwareDecodeEnabled: Boolean = true,
    /** 镜像画面（三态，UI 枚举 `MirrorUiMode`） */
    val mirrorMode: MirrorUiMode = MirrorUiMode.None,
    /** 色觉辅助滤镜（四档，UI 枚举 `ColorVisionMode`；None = 单位矩阵 = 关闭） */
    val colorVisionMode: ColorVisionMode = ColorVisionMode.None,
    /** 后台播放（默认开，与旧行为一致） */
    val backgroundPlayEnabled: Boolean = true,
    /** 弹幕总开关（默认开，与旧行为一致） */
    val danmakuEnabled: Boolean = true,
) {
    companion object {
        /**
         * 落盘用的 SharedPreferences 文件名。
         * **改名 = 老用户的设置全部丢失**，所以这个名字本身也算格式的一部分，别随手改。
         */
        private const val PREFS_NAME = "cicada_player_settings"

        private const val KEY_HARDWARE_DECODE = "hardware_decode_enabled"
        private const val KEY_MIRROR = "mirror_mode"
        private const val KEY_COLOR_VISION = "color_vision_mode"
        private const val KEY_BACKGROUND_PLAY = "background_play_enabled"
        private const val KEY_DANMAKU = "danmaku_enabled"

        /*
         * 默认值：全部是**加持久化之前的行为**（硬解=开、镜像=关、色觉=关、
         * 后台播放=开、弹幕=开）。这样"老用户第一次升级上来"看到的界面与以前一模一样，
         * 不引入任何行为突变。
         */
        const val DEFAULT_HARDWARE_DECODE = true
        const val DEFAULT_BACKGROUND_PLAY = true
        const val DEFAULT_DANMAKU = true

        /**
         * 从磁盘读一份设置。
         *
         * 【为什么不用 `Context.MODE_PRIVATE` 以外的模式】只有本 App 自己读写，
         * 没有跨进程共享的需求，MODE_PRIVATE 就是"不给自己找事"的默认。
         */
        fun load(context: Context): PlayerSettings {
            val prefs = context.applicationContext
                .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            return PlayerSettings(
                hardwareDecodeEnabled = prefs.getBoolean(
                    KEY_HARDWARE_DECODE, DEFAULT_HARDWARE_DECODE
                ),
                /* 存的是枚举名；认不出来的值（老版本/被手改过）一律回落默认档 */
                mirrorMode = prefs.getString(KEY_MIRROR, null)
                    ?.let { raw -> MirrorUiMode.entries.firstOrNull { it.name == raw } }
                    ?: MirrorUiMode.None,
                colorVisionMode = prefs.getString(KEY_COLOR_VISION, null)
                    ?.let { raw -> ColorVisionMode.entries.firstOrNull { it.name == raw } }
                    ?: ColorVisionMode.None,
                backgroundPlayEnabled = prefs.getBoolean(
                    KEY_BACKGROUND_PLAY, DEFAULT_BACKGROUND_PLAY
                ),
                danmakuEnabled = prefs.getBoolean(KEY_DANMAKU, DEFAULT_DANMAKU),
            )
        }

        /**
         * 写一份设置（`apply()` = 异步落盘，内存里的值立刻可见）。
         *
         * 为什么每次整体写回而不是"改一个字段写一个 key"：调用方（ViewModel）手里本来
         * 就持有完整的一份 [PlayerSettings]（data class 的 copy 语义），整体写回少一层
         * "哪个 key 对应哪个字段"的映射，漏写/写错的可能性更小。
         */
        fun save(context: Context, settings: PlayerSettings) {
            context.applicationContext
                .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
                .edit()
                .putBoolean(KEY_HARDWARE_DECODE, settings.hardwareDecodeEnabled)
                .putString(KEY_MIRROR, settings.mirrorMode.name)
                .putString(KEY_COLOR_VISION, settings.colorVisionMode.name)
                .putBoolean(KEY_BACKGROUND_PLAY, settings.backgroundPlayEnabled)
                .putBoolean(KEY_DANMAKU, settings.danmakuEnabled)
                .apply()
        }
    }
}
