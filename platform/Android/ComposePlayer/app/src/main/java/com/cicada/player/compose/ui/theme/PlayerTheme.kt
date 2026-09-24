package com.cicada.player.compose.ui.theme

import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.ReadOnlyComposable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color

/**
 * 播放器主题色板。
 *
 * 为什么单独一层而不直接用 MaterialTheme：播放器控制层是**压在画面上的浮层**，
 * 它是固定的白色系文字/图标 + 底部黑色遮罩，不跟 App 的深浅主题走；
 * 而 App 面板（投稿页那种）跟主题走。两种语义混在 MaterialTheme 里会到处判空，所以明确分开。
 *
 * 色值就是**照着手机截图定的**（深浅两套只差主题色）。**没有 hover 这一档** ——
 * 手机没有鼠标，文字/图标只有 `controlText`（常态）和 `controlTextActive`（激活/选中）。
 *
 * **换主题色只改 Color.kt 里的 BrandAccentDark / BrandAccentLight 两行。**
 */
@Immutable
data class PlayerColors(
    val accent: Color,
    val accentActive: Color,
    val accentFaint: Color,
    val accentSoft: Color,

    val videoBg: Color,
    val controlText: Color,
    val controlTextActive: Color,
    val progressTrack: Color,
    val progressBuffer: Color,
    val controlMaskTop: Color,
    val controlMaskBottom: Color,

    val panelBg: Color,
    val panelText: Color,
    val panelSubText: Color,
    val panelDivider: Color,

    val thumbBadgeBg: Color,
    val previewPlaceholderBg: Color,
    /** 预览卡片下方时间文字的底（半透明黑，截图 3） */
    val previewTimeBg: Color,

    val surface: Color,
    val surfaceBorder: Color,
    val onSurface: Color,
    val windowBg: Color,
)

private val DarkPlayerColors = PlayerColors(
    accent = BrandAccentDark,
    accentActive = BrandAccentActiveDark,
    accentFaint = BrandAccentDark.copy(alpha = 0.18f),
    accentSoft = BrandAccentDark.copy(alpha = 0.20f),
    videoBg = PlayerVideoBg,
    controlText = PlayerCtrlText,
    controlTextActive = PlayerCtrlTextActive,
    progressTrack = PlayerProgressTrack,
    progressBuffer = PlayerProgressBuffer,
    controlMaskTop = PlayerControlMaskTop,
    controlMaskBottom = PlayerControlMaskBottom,
    panelBg = PlayerPanelBg,
    panelText = PlayerPanelText,
    panelSubText = PlayerPanelSubText,
    panelDivider = PlayerPanelDivider,
    thumbBadgeBg = PlayerThumbBadgeBg,
    previewPlaceholderBg = PlayerPreviewPlaceholderBg,
    previewTimeBg = PlayerPreviewTimeBg,
    surface = AppPanelBgDark,
    surfaceBorder = AppPanelBorderDark,
    onSurface = AppTextDark,
    windowBg = AppWindowBgDark,
)

private val LightPlayerColors = PlayerColors(
    accent = BrandAccentLight,
    accentActive = BrandAccentActiveLight,
    accentFaint = BrandAccentLight.copy(alpha = 0.18f),
    accentSoft = BrandAccentLight.copy(alpha = 0.20f),
    videoBg = PlayerVideoBg,
    /* 控制层压在画面上：浅色主题下同样是白色系 */
    controlText = PlayerCtrlText,
    controlTextActive = PlayerCtrlTextActive,
    progressTrack = PlayerProgressTrack,
    progressBuffer = PlayerProgressBuffer,
    controlMaskTop = PlayerControlMaskTop,
    controlMaskBottom = PlayerControlMaskBottom,
    panelBg = PlayerPanelBg,
    panelText = PlayerPanelText,
    panelSubText = PlayerPanelSubText,
    panelDivider = PlayerPanelDivider,
    thumbBadgeBg = PlayerThumbBadgeBg,
    previewPlaceholderBg = PlayerPreviewPlaceholderBg,
    previewTimeBg = PlayerPreviewTimeBg,
    surface = AppPanelBgLight,
    surfaceBorder = AppPanelBorderLight,
    onSurface = AppTextLight,
    windowBg = AppWindowBgLight,
)

val LocalPlayerColors = staticCompositionLocalOf { DarkPlayerColors }

fun playerColors(dark: Boolean): PlayerColors = if (dark) DarkPlayerColors else LightPlayerColors

/** 取色入口：`PlayerTheme.accent` */
object PlayerTheme {
    val colors: PlayerColors
        @Composable @ReadOnlyComposable get() = LocalPlayerColors.current

    val accent: Color @Composable @ReadOnlyComposable get() = colors.accent
    val accentActive: Color @Composable @ReadOnlyComposable get() = colors.accentActive
    val accentFaint: Color @Composable @ReadOnlyComposable get() = colors.accentFaint
    val accentSoft: Color @Composable @ReadOnlyComposable get() = colors.accentSoft
    val videoBg: Color @Composable @ReadOnlyComposable get() = colors.videoBg
    val controlText: Color @Composable @ReadOnlyComposable get() = colors.controlText
    val controlTextActive: Color @Composable @ReadOnlyComposable get() = colors.controlTextActive
    val progressTrack: Color @Composable @ReadOnlyComposable get() = colors.progressTrack
    val progressBuffer: Color @Composable @ReadOnlyComposable get() = colors.progressBuffer
    val panelBg: Color @Composable @ReadOnlyComposable get() = colors.panelBg
    val panelText: Color @Composable @ReadOnlyComposable get() = colors.panelText
    val panelSubText: Color @Composable @ReadOnlyComposable get() = colors.panelSubText
    val panelDivider: Color @Composable @ReadOnlyComposable get() = colors.panelDivider
    val thumbBadgeBg: Color @Composable @ReadOnlyComposable get() = colors.thumbBadgeBg
    val previewPlaceholderBg: Color @Composable @ReadOnlyComposable get() = colors.previewPlaceholderBg
    val previewTimeBg: Color @Composable @ReadOnlyComposable get() = colors.previewTimeBg
    val surface: Color @Composable @ReadOnlyComposable get() = colors.surface
    val surfaceBorder: Color @Composable @ReadOnlyComposable get() = colors.surfaceBorder
    val onSurface: Color @Composable @ReadOnlyComposable get() = colors.onSurface
    val windowBg: Color @Composable @ReadOnlyComposable get() = colors.windowBg
}
