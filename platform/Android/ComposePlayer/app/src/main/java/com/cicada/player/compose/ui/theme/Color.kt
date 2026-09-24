package com.cicada.player.compose.ui.theme

import androidx.compose.ui.graphics.Color

// ===========================================================================
// 主题色 —— **全项目唯一来源**
//
// 用户要求：深色 `#D44E7D`、浅色 `#FF6699`。
// **只在下面两行定义**，其它文件一律用 PlayerTheme 里的语义色，不许写颜色字面量。
// ===========================================================================

/** ★ 深色主题主题色 */
val BrandAccentDark = Color(0xFFD44E7D)

/** ★ 浅色主题主题色 */
val BrandAccentLight = Color(0xFFFF6699)

/** 按下/激活：暗一档（手机只有"按下"这一种反馈，**没有 hover**） */
val BrandAccentActiveDark = Color(0xFFB03E63)
val BrandAccentActiveLight = Color(0xFFE84B85)

// ---------------------------------------------------------------------------
// 播放器控制层：压在画面上的浮层，**不跟深浅主题走**（白色系文字/图标 + 底部黑色遮罩）
//
// 手机端**没有 hover**：文字/图标只有"常态"和"激活/选中"两种状态，
// 按压反馈交给 clickable 自带的水波纹，不再额外做别的状态色。
// ---------------------------------------------------------------------------

/** 播放区底色：永远黑 */
val PlayerVideoBg = Color(0xFF000000)

/** 控制条文字/图标常态：白 80% */
val PlayerCtrlText = Color(0xCCFFFFFF)

/** 控制条文字/图标激活/选中态（当前选中的倍速、清晰度等）：纯白 */
val PlayerCtrlTextActive = Color(0xFFFFFFFF)

/** 进度条轨道：白 20% */
val PlayerProgressTrack = Color(0x33FFFFFF)

/** 进度条已缓冲：白 30% */
val PlayerProgressBuffer = Color(0x4DFFFFFF)

/** 控制层底部遮罩：透明 → 黑 70% */
val PlayerControlMaskTop = Color(0x00000000)
val PlayerControlMaskBottom = Color(0xB3000000)

/** 浮层面板底：#212121 @ 90%、圆角 4 */
val PlayerPanelBg = Color(0xE6212121)
val PlayerPanelText = Color(0xFFFFFFFF)
val PlayerPanelSubText = Color(0xFF999999)
val PlayerPanelDivider = Color(0x1AFFFFFF)

/** 进度条 dot（Thumb）后面的深色底（截图里 dot 是黑底圆角徽标） */
val PlayerThumbBadgeBg = Color(0xE6000000)

/** 快进预览图的加载占位底：半透明白（截图 4 的圆形占位） */
val PlayerPreviewPlaceholderBg = Color(0x33FFFFFF)

/** 预览卡片下方时间文字的底：半透明黑（截图 3 里是压在图/画面上的深色底 + 纯白大字） */
val PlayerPreviewTimeBg = Color(0xE6141414)

// ---------------------------------------------------------------------------
// 应用内面板（跟主题走的两套）
// ---------------------------------------------------------------------------

val AppPanelBgDark = Color(0xFF1E1E26)
val AppPanelBgLight = Color(0xFFFFFFFF)
val AppPanelBorderDark = Color(0xFF3A3A48)
val AppPanelBorderLight = Color(0xFFD5D8DE)
val AppTextDark = Color(0xFFD0D0E0)
val AppTextLight = Color(0xFF2A2D33)
val AppWindowBgDark = Color(0xFF101014)
val AppWindowBgLight = Color(0xFFF2F3F6)
