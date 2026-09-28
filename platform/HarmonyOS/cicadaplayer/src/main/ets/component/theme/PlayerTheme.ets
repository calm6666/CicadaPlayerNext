/*
 * 播放器主题 —— ArkTS 版，来源是 Compose 安卓版的
 *   platform/Android/ComposePlayer/app/src/main/java/com/cicada/player/compose/ui/theme/Color.kt
 *   platform/Android/ComposePlayer/app/src/main/java/com/cicada/player/compose/ui/theme/PlayerTheme.kt
 *   platform/Android/ComposePlayer/app/src/main/java/com/cicada/player/compose/ui/theme/Type.kt
 *
 * 逐个常量一一对应，命名与 Compose 对齐，方便逐项核对：
 *   · 品牌色 / 播放器控制层固定色 / 应用内面板色 —— 见下面 PlayerColors 的字段与注释，
 *     字段名与 Kotlin 的 PlayerColors data class 完全同名同序；
 *   · 尺寸、字号、圆角、间距 —— Compose 那边是散在 composable 里的字面量
 *     （在 PlayerControls.kt / PlayerPanels.kt / LottieIcon.kt 里），这里全部收拢成
 *     PlayerSize 常量，注释里给出它在 Compose 的出处，避免两处各写一个数。
 *
 * 颜色用 ArkUI 的 '#AARRGGBB' 字符串（Kotlin 是 0xAARRGGBB 的 Color），
 * 透明度换算：0.18 → 0x2E、0.20 → 0x33、0.12 → 0x1F。
 */

/** Color.kt：深色主题主题色 */
export const BRAND_ACCENT_DARK: string = '#FFD44E7D';
/** Color.kt：浅色主题主题色 */
export const BRAND_ACCENT_LIGHT: string = '#FFFF6699';
/** Color.kt：按下/激活（暗一档） */
export const BRAND_ACCENT_ACTIVE_DARK: string = '#FFB03E63';
export const BRAND_ACCENT_ACTIVE_LIGHT: string = '#FFE84B85';

/** Color.kt：播放区底色，永远黑 */
export const PLAYER_VIDEO_BG: string = '#FF000000';
/** Color.kt：控制条文字/图标常态（白 80%） */
export const PLAYER_CTRL_TEXT: string = '#CCFFFFFF';
/** Color.kt：控制条文字/图标激活/选中（纯白） */
export const PLAYER_CTRL_TEXT_ACTIVE: string = '#FFFFFFFF';
/** Color.kt：进度条轨道（白 20%） */
export const PLAYER_PROGRESS_TRACK: string = '#33FFFFFF';
/** Color.kt：进度条已缓冲（白 30%） */
export const PLAYER_PROGRESS_BUFFER: string = '#4DFFFFFF';
/** Color.kt：控制层底部遮罩（透明 → 黑 70%） */
export const PLAYER_CONTROL_MASK_TOP: string = '#00000000';
export const PLAYER_CONTROL_MASK_BOTTOM: string = '#B3000000';
/** Color.kt：浮层面板底（#212121 @90%、圆角 4） */
export const PLAYER_PANEL_BG: string = '#E6212121';
export const PLAYER_PANEL_TEXT: string = '#FFFFFFFF';
export const PLAYER_PANEL_SUB_TEXT: string = '#FF999999';
export const PLAYER_PANEL_DIVIDER: string = '#1AFFFFFF';
/** Color.kt：进度条 dot（Thumb）后面的深色底 */
export const PLAYER_THUMB_BADGE_BG: string = '#E6000000';
/** Color.kt：快进预览图的加载占位底（半透明白） */
export const PLAYER_PREVIEW_PLACEHOLDER_BG: string = '#33FFFFFF';
/** Color.kt：预览卡片下方时间文字的底（半透明黑） */
export const PLAYER_PREVIEW_TIME_BG: string = '#E6141414';

/** Color.kt：应用内面板（跟主题走的两套） */
export const APP_PANEL_BG_DARK: string = '#FF1E1E26';
export const APP_PANEL_BG_LIGHT: string = '#FFFFFFFF';
export const APP_PANEL_BORDER_DARK: string = '#FF3A3A48';
export const APP_PANEL_BORDER_LIGHT: string = '#FFD5D8DE';
export const APP_TEXT_DARK: string = '#FFD0D0E0';
export const APP_TEXT_LIGHT: string = '#FF2A2D33';
export const APP_WINDOW_BG_DARK: string = '#FF101014';
export const APP_WINDOW_BG_LIGHT: string = '#FFF2F3F6';

/**
 * PlayerTheme.kt 的 PlayerColors data class —— 字段名、顺序、语义完全一致。
 *
 * 控制层是压在画面上的浮层：固定白色系文字/图标 + 底部黑色遮罩，**不跟 App 深浅主题走**；
 * 只有 accent 系与 App 面板那几个字段随主题变。手机没有 hover，所以只有
 * controlText（常态）与 controlTextActive（激活/选中）两档。
 */
export class PlayerColors {
  accent: string = BRAND_ACCENT_DARK;
  accentActive: string = BRAND_ACCENT_ACTIVE_DARK;
  accentFaint: string = '#2ED44E7D';
  accentSoft: string = '#33D44E7D';

  videoBg: string = PLAYER_VIDEO_BG;
  controlText: string = PLAYER_CTRL_TEXT;
  controlTextActive: string = PLAYER_CTRL_TEXT_ACTIVE;
  progressTrack: string = PLAYER_PROGRESS_TRACK;
  progressBuffer: string = PLAYER_PROGRESS_BUFFER;
  controlMaskTop: string = PLAYER_CONTROL_MASK_TOP;
  controlMaskBottom: string = PLAYER_CONTROL_MASK_BOTTOM;

  panelBg: string = PLAYER_PANEL_BG;
  panelText: string = PLAYER_PANEL_TEXT;
  panelSubText: string = PLAYER_PANEL_SUB_TEXT;
  panelDivider: string = PLAYER_PANEL_DIVIDER;

  thumbBadgeBg: string = PLAYER_THUMB_BADGE_BG;
  previewPlaceholderBg: string = PLAYER_PREVIEW_PLACEHOLDER_BG;
  previewTimeBg: string = PLAYER_PREVIEW_TIME_BG;

  surface: string = APP_PANEL_BG_DARK;
  surfaceBorder: string = APP_PANEL_BORDER_DARK;
  onSurface: string = APP_TEXT_DARK;
  windowBg: string = APP_WINDOW_BG_DARK;
}

/** PlayerTheme.kt：`playerColors(dark)` —— 深色/浅色两套只差主题色与 App 面板色。 */
export function playerColors(dark: boolean): PlayerColors {
  const colors: PlayerColors = new PlayerColors();
  if (!dark) {
    colors.accent = BRAND_ACCENT_LIGHT;
    colors.accentActive = BRAND_ACCENT_ACTIVE_LIGHT;
    colors.accentFaint = '#2EFF6699';
    colors.accentSoft = '#33FF6699';
    /* 控制层压在画面上：浅色主题下同样是白色系 */
    colors.surface = APP_PANEL_BG_LIGHT;
    colors.surfaceBorder = APP_PANEL_BORDER_LIGHT;
    colors.onSurface = APP_TEXT_LIGHT;
    colors.windowBg = APP_WINDOW_BG_LIGHT;
  }
  return colors;
}

/**
 * PlayerTheme.kt 的 `object PlayerTheme` —— ArkTS 没有 CompositionLocal，
 * 所以这里用一个静态色板持有者代替：默认深色（与截图/安卓播放页一致）。
 */
export class PlayerTheme {
  private static current: PlayerColors = playerColors(true);

  /** 切换深浅色板（默认深色）。 */
  static setDark(dark: boolean): void {
    PlayerTheme.current = playerColors(dark);
  }

  static get colors(): PlayerColors {
    return PlayerTheme.current;
  }

  static get accent(): string {
    return PlayerTheme.current.accent;
  }

  static get accentActive(): string {
    return PlayerTheme.current.accentActive;
  }

  static get accentFaint(): string {
    return PlayerTheme.current.accentFaint;
  }

  static get accentSoft(): string {
    return PlayerTheme.current.accentSoft;
  }

  static get videoBg(): string {
    return PlayerTheme.current.videoBg;
  }

  static get controlText(): string {
    return PlayerTheme.current.controlText;
  }

  static get controlTextActive(): string {
    return PlayerTheme.current.controlTextActive;
  }

  static get progressTrack(): string {
    return PlayerTheme.current.progressTrack;
  }

  static get progressBuffer(): string {
    return PlayerTheme.current.progressBuffer;
  }

  static get panelBg(): string {
    return PlayerTheme.current.panelBg;
  }

  static get panelText(): string {
    return PlayerTheme.current.panelText;
  }

  static get panelSubText(): string {
    return PlayerTheme.current.panelSubText;
  }

  static get panelDivider(): string {
    return PlayerTheme.current.panelDivider;
  }

  static get thumbBadgeBg(): string {
    return PlayerTheme.current.thumbBadgeBg;
  }

  static get previewPlaceholderBg(): string {
    return PlayerTheme.current.previewPlaceholderBg;
  }

  static get previewTimeBg(): string {
    return PlayerTheme.current.previewTimeBg;
  }

  static get surface(): string {
    return PlayerTheme.current.surface;
  }

  static get surfaceBorder(): string {
    return PlayerTheme.current.surfaceBorder;
  }

  static get onSurface(): string {
    return PlayerTheme.current.onSurface;
  }

  static get windowBg(): string {
    return PlayerTheme.current.windowBg;
  }
}

/**
 * Type.kt 的 Typography（整个 Compose 工程只覆盖了 bodyLarge 一项）。
 * ArkUI 的 fontSize/lineHeight 单位是 vp（≈ Compose 的 dp），字号按 sp 同值落。
 */
export class PlayerType {
  static readonly bodyLargeFontSize: number = 16;
  static readonly bodyLargeLineHeight: number = 24;
  static readonly bodyLargeLetterSpacing: number = 0.5;
}

/**
 * 播放器 UI 的尺寸/字号/圆角/间距 —— Compose 那边的字面量出处写在每一项的注释里。
 * 单位全部是 vp；时间/比例类常量是无单位数值。
 */
export class PlayerSize {
  // ---- PlayerControls.kt：进度条 ----
  /** PlayerThinProgressBar(trackHeight = 2.dp) */
  static readonly thinTrackHeight: number = 2;
  /** PlayerProgressBar(touchHeight = 20.dp) */
  static readonly progressTouchHeight: number = 20;
  /** PlayerProgressBar(trackHeight = 4.dp) */
  static readonly progressTrackHeight: number = 4;
  /** PlayerProgressBar(thumbSize = 20.dp) */
  static readonly progressThumbSize: number = 20;
  /** SEEK_SETTLE_TOLERANCE_MS */
  static readonly seekSettleToleranceMs: number = 700;
  /** PREVIEW_CARD_WIDTH_RATIO */
  static readonly previewCardWidthRatio: number = 0.23;
  /** previewCardGapAboveTrack = 90.dp */
  static readonly previewCardGapAboveTrack: number = 90;
  /** previewCardRadius = 4.dp */
  static readonly previewCardRadius: number = 4;
  /** previewCardBorder = Color(0x66FFFFFF) */
  static readonly previewCardBorder: string = '#66FFFFFF';
  /** PREVIEW_TIME_FONT_SIZE = 16（sp） */
  static readonly previewTimeFontSize: number = 16;
  /** PreviewCard 里 Spacer(Modifier.height(4.dp)) */
  static readonly previewCardTimeGap: number = 4;
  /** PreviewCard 时间文字：RoundedCornerShape(3.dp) + padding(horizontal 6, vertical 2) */
  static readonly previewTimeRadius: number = 3;
  static readonly previewTimePaddingH: number = 6;
  static readonly previewTimePaddingV: number = 2;
  /** previewDotSize = 36.dp / previewDotOffsetX = 4.dp / previewDotGap = 6.dp */
  static readonly previewDotSize: number = 36;
  static readonly previewDotOffsetX: number = 4;
  static readonly previewDotGap: number = 6;
  /** PreviewLoadingDot 里的 CircularProgressIndicator(size 18.dp, strokeWidth 2.dp) */
  static readonly previewSpinnerSize: number = 18;
  static readonly previewSpinnerStroke: number = 2;
  /** previewCardBorder 那圈描边：border(1.dp, ...) */
  static readonly previewCardBorderWidth: number = 1;

  // ---- PlayerControls.kt：底部栏 ----
  /** Column.padding(horizontal = 12.dp, vertical = 4.dp) */
  static readonly bottomBarPaddingH: number = 12;
  static readonly bottomBarPaddingV: number = 4;
  /** PlayerProgressBar(modifier = Modifier.padding(bottom = 2.dp)) */
  static readonly bottomBarProgressGap: number = 2;
  /** Row(Modifier.fillMaxWidth().height(36.dp)) */
  static readonly bottomBarRowHeight: number = 36;
  /** PlayPauseIconButton(size = 40.dp) */
  static readonly playPauseSize: number = 40;
  /** Spacer(Modifier.width(6.dp))（非全屏时间文字前） */
  static readonly bottomBarTimeGap: number = 6;
  /** 非全屏时间文字 fontSize 12.sp + padding(horizontal 6.dp) */
  static readonly bottomBarTimeFontSize: number = 12;
  static readonly bottomBarTimePaddingH: number = 6;
  /** BarText：fontSize 13.sp + padding(horizontal 8.dp) */
  static readonly barTextFontSize: number = 13;
  static readonly barTextPaddingH: number = 8;
  /** 全屏底部栏里弹幕输入框两侧：padding(horizontal = 8.dp) */
  static readonly bottomBarInputPaddingH: number = 8;
  /** 「竖屏」胶囊：RoundedCornerShape(11.dp) + padding(horizontal 10, vertical 3) + 字号 12.sp */
  static readonly portraitPillRadius: number = 11;
  static readonly portraitPillPaddingH: number = 10;
  static readonly portraitPillPaddingV: number = 3;
  static readonly portraitPillFontSize: number = 12;
  static readonly portraitPillMarginH: number = 6;

  // ---- PlayerControls.kt：弹幕输入框 ----
  /** Row(height 32.dp, RoundedCornerShape(16.dp), padding(horizontal 12.dp)) */
  static readonly danmakuInputHeight: number = 32;
  static readonly danmakuInputRadius: number = 16;
  static readonly danmakuInputPaddingH: number = 12;
  /** 输入文字/占位/发送：fontSize 13.sp、lineHeight 18.sp */
  static readonly danmakuInputFontSize: number = 13;
  static readonly danmakuInputLineHeight: number = 18;

  // ---- PlayerControls.kt：顶部栏 ----
  /** Row.padding(horizontal = 8.dp, vertical = 6.dp) */
  static readonly topBarPaddingH: number = 8;
  static readonly topBarPaddingV: number = 6;
  /** 返回箭头 Modifier.size(24.dp) */
  static readonly topBarIconSize: number = 24;
  /** 标题列 padding(start = 4.dp) */
  static readonly topBarTitleGap: number = 4;
  /** 标题 15.sp Medium；在线人数 12.sp */
  static readonly topBarTitleFontSize: number = 15;
  static readonly topBarWatchingFontSize: number = 12;
  /** 「+ 关注」：RoundedCornerShape(14.dp) + padding(horizontal 12, vertical 4) + 13.sp */
  static readonly followRadius: number = 14;
  static readonly followPaddingH: number = 12;
  static readonly followPaddingV: number = 4;
  static readonly followFontSize: number = 13;

  // ---- PlayerControls.kt：通用小件 ----
  /** PlayerIconButton(buttonSize = 36.dp, iconSize = 26.dp) */
  static readonly iconButtonSize: number = 36;
  static readonly iconButtonIconSize: number = 26;

  // ---- CicadaVideoPlayer.kt ----
  /** 控制栏自动隐藏 3 秒（LaunchedEffect 里的 delay(3000)） */
  static readonly controlsAutoHideMs: number = 3000;
  /** 加载圈 Modifier.size(44.dp)、strokeWidth 3.dp */
  static readonly loadingIndicatorSize: number = 44;
  static readonly loadingIndicatorStroke: number = 3;
  /** 错误卡片：RoundedCornerShape(8.dp) + padding(20.dp)；标题 16.sp、正文 13.sp、间隔 6/12 */
  static readonly errorCardRadius: number = 8;
  static readonly errorCardPadding: number = 20;
  static readonly errorTitleFontSize: number = 16;
  static readonly errorMessageFontSize: number = 13;
  static readonly errorTitleGap: number = 6;
  static readonly errorButtonGap: number = 12;
  /** 清晰度切换提示：底部留 76dp、左 12dp、RoundedCornerShape(4.dp)、padding(12, 8)、13.sp */
  static readonly noticePaddingStart: number = 12;
  static readonly noticePaddingBottom: number = 76;
  static readonly noticeRadius: number = 4;
  static readonly noticePaddingH: number = 12;
  static readonly noticePaddingV: number = 8;
  static readonly noticeFontSize: number = 13;
  /** 终态提示自动收起 2600ms（LaunchedEffect 里的 delay(2600)） */
  static readonly noticeDismissMs: number = 2600;
  /** 非全屏左上角返回：padding(start 2.dp, top 2.dp)、图标 22.dp */
  static readonly backButtonPadding: number = 2;
  static readonly backButtonIconSize: number = 22;
  /** 非全屏返回按钮外圈 36dp 热区 + 顶部栏渐变（黑 55% → 透明） */
  static readonly topBarMaskTop: string = '#8C000000';
  static readonly topBarMaskBottom: string = '#00000000';

  // ---- LottieIcon.kt ----
  /** PlayPauseIconButton(size = 40.dp) */
  static readonly lottiePlayPauseSize: number = 40;
  /** DanmakuDotThumb(size = 20.dp) */
  static readonly lottieThumbSize: number = 20;
  /** play/pause 两个 JSON 的合成画布（JSON 里的 w/h） */
  static readonly lottiePlayPauseCanvas: number = 28;
  /** Thumb JSON 的合成画布（JSON 里的 w/h） */
  static readonly lottieThumbCanvas: number = 18;
  /** 两段动画时长：fr = 60、op = 60 ⇒ 1000ms（LottieIcon 用 tween(durationMs, LinearEasing)） */
  static readonly lottiePlayPauseDurationMs: number = 1000;

  // ---- PlayerPanels.kt ----
  /** PlayerPanelShell(width = 160.dp, heightIn(max = 220.dp), 圆角 4.dp, padding(vertical 9.dp)) */
  static readonly panelWidth: number = 160;
  static readonly panelMaxHeight: number = 220;
  static readonly panelRadius: number = 4;
  static readonly panelPaddingV: number = 9;
  /** PlayerDanmakuPanel(width = 180.dp) / PlayerVolumePanel(width = 170.dp) / PlayerInfoPanel(width = 300.dp) */
  static readonly panelDanmakuWidth: number = 180;
  static readonly panelVolumeWidth: number = 170;
  static readonly panelInfoWidth: number = 300;
  /** PanelTitle：10.sp + padding(horizontal 20.dp, vertical 1.dp) */
  static readonly panelTitleFontSize: number = 10;
  static readonly panelTitlePaddingH: number = 20;
  static readonly panelTitlePaddingV: number = 1;
  /** PanelRow：padding(horizontal 20.dp, vertical 4.dp) + 13.sp */
  static readonly panelRowPaddingH: number = 20;
  static readonly panelRowPaddingV: number = 4;
  static readonly panelRowFontSize: number = 13;
  /** 行尾编码徽标：padding(start 6.dp)、圆角 3.dp、白 12% 底、padding(4, 1)、9.sp */
  static readonly panelBadgeGap: number = 6;
  static readonly panelBadgeRadius: number = 3;
  static readonly panelBadgeBg: string = '#1FFFFFFF';
  static readonly panelBadgePaddingH: number = 4;
  static readonly panelBadgePaddingV: number = 1;
  static readonly panelBadgeFontSize: number = 9;
  /** 面板里"没有数据"那行：12.sp + padding(horizontal 20.dp, vertical 4.dp) */
  static readonly panelEmptyFontSize: number = 12;
  /** 视频信息面板的 key/value 行：padding(horizontal 20, vertical 3)、12.sp、值 padding(start 8) */
  static readonly infoRowPaddingV: number = 3;
  static readonly infoRowFontSize: number = 12;
  static readonly infoValueGap: number = 8;
  /** 后台播放行：padding(horizontal 20.dp, vertical 6.dp) + 13.sp */
  static readonly infoSwitchRowPaddingV: number = 6;
  static readonly infoSwitchRowFontSize: number = 13;
  /** 音量面板的数值：14.sp Bold；滑条轨道 2dp、高 60dp、外层高 70dp、圆点 12dp */
  static readonly volumeValueFontSize: number = 14;
  static readonly volumeSliderWidth: number = 32;
  static readonly volumeSliderHeight: number = 60;
  static readonly volumeTrackWidth: number = 2;
  static readonly volumeTrackBg: string = '#FFE7E7E7';
  static readonly volumeBoxHeight: number = 70;
  static readonly volumeThumbSize: number = 12;
  static readonly volumeValueRowPaddingV: number = 6;
  static readonly volumeValueColor: string = '#FFE5E9EF';

  // ---- 面板挂在控制条上方时的位置（CicadaVideoPlayer.kt） ----
  /** Box(Modifier.padding(end = 12.dp, bottom = 4.dp)) */
  static readonly panelAnchorEnd: number = 12;
  static readonly panelAnchorBottom: number = 4;

  // ---- 弹幕（DanmakuGpu.kt / DanmakuView.kt） ----
  /** 文字位图内边距 pad = 4f、描边 3f */
  static readonly danmakuBitmapPad: number = 4;
  static readonly danmakuStrokeWidth: number = 3;
  /** 位图缓存上限 maxEntries = 200 */
  static readonly danmakuCacheEntries: number = 200;
  /** 行高 = textSizePx + 12f */
  static readonly danmakuLineHeightExtra: number = 12;
  /** 滚动弹幕 8000ms 一屏 */
  static readonly danmakuScrollDurationMs: number = 8000;
  /** 固定弹幕停留 5000ms */
  static readonly danmakuFixedDurationMs: number = 5000;
  /** 安全距离 textSizePx * 2 + 20 */
  static readonly danmakuSafeDistanceScale: number = 2;
  static readonly danmakuSafeDistanceExtra: number = 20;
  /** 进场年龄窗口 age <= 1500f */
  static readonly danmakuSpawnWindowMs: number = 1500;
  /** 与播放位置对齐的软收敛阈值 250f 与系数 0.2 */
  static readonly danmakuResyncThresholdMs: number = 250;
  static readonly danmakuResyncFactor: number = 0.2;
  /** 固定弹幕最后 300ms 淡出 */
  static readonly danmakuFadeOutMs: number = 300;
  /** 同屏最多保留 300 条 */
  static readonly danmakuMaxActive: number = 300;
  /** 弹幕字号基准 32f（DanmakuConfig.textSize 与面板的 100% 档） */
  static readonly danmakuBaseTextSize: number = 32;
}
