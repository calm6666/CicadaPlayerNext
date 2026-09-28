/*
 * CicadaPlayerNext HarmonyOS SDK —— 对外唯一入口。
 *
 * 其它 HarmonyOS 工程只要依赖本 HAR（见 docs/Packaging_HarmonyOS.md 的接入一节），
 * 即可拿到三层能力：
 *   1) 播放器内核：自研 CicadaPlayerNext（FFmpeg 解封装/软解 + OH_AVCodec 硬解 +
 *      OHAudio 渲染 + DRM Kit + DASH/HLS/对象清单），编在 libcicadaplayer.so 里；
 *   2) ArkTS SDK：CicadaPlayer 句柄封装 + 事件回调 + 全部类型/枚举；
 *   3) 开箱即用组件：
 *      · CicadaVideo（XComponent + 生命周期 + 播放器装配）；
 *      · **播放器 UI（compose 一套，逐项对齐 Compose 安卓版）**：
 *        CicadaComposePlayer 总装、顶部栏/底部栏/进度条/seek 预览、播放暂停 Lottie 按钮、
 *        6 个浮层面板、视频信息、弹幕层、手势层；
 *      · 播放器主题与尺寸常量（theme/PlayerTheme）。
 */

// ---- ArkTS SDK：播放器 ----
export { CicadaPlayer } from './src/main/ets/api/CicadaPlayer';

// ---- ArkTS SDK：类型与枚举 ----
export {
  ScaleMode,
  RotateMode,
  MirrorMode,
  StreamType,
  DecoderType,
  PlayerStatus,
  PropertyKey,
  PlayerEventName,
  VideoQualitySwitchStatus,
  VideoResolution
} from './src/main/ets/api/CicadaTypes';
export type {
  StreamInfo,
  StreamMeta,
  MediaInfo,
  CicadaPlayerListener,
  CicadaDrmCallback,
  VideoCodecSupport
} from './src/main/ets/api/CicadaTypes';

// ---- 工具 ----
export { S, UrlDefaults } from './src/main/ets/util/Strings';
export { formatMs, formatSpeed } from './src/main/ets/util/TimeFormater';

// ---- 开箱即用组件：播放器视图 ----
export { CicadaVideo, CicadaVideoController, CICADA_VIDEO_DEFAULT_SCALE_MODE } from './src/main/ets/component/CicadaVideo';

// ---- 播放器主题（颜色 / 尺寸 / 字号 / 圆角） ----
export {
  PlayerTheme,
  PlayerColors,
  PlayerType,
  PlayerSize,
  playerColors,
  BRAND_ACCENT_DARK,
  BRAND_ACCENT_LIGHT,
  BRAND_ACCENT_ACTIVE_DARK,
  BRAND_ACCENT_ACTIVE_LIGHT,
  PLAYER_VIDEO_BG,
  PLAYER_CTRL_TEXT,
  PLAYER_CTRL_TEXT_ACTIVE,
  PLAYER_PROGRESS_TRACK,
  PLAYER_PROGRESS_BUFFER,
  PLAYER_CONTROL_MASK_TOP,
  PLAYER_CONTROL_MASK_BOTTOM,
  PLAYER_PANEL_BG,
  PLAYER_PANEL_TEXT,
  PLAYER_PANEL_SUB_TEXT,
  PLAYER_PANEL_DIVIDER,
  PLAYER_THUMB_BADGE_BG,
  PLAYER_PREVIEW_PLACEHOLDER_BG,
  PLAYER_PREVIEW_TIME_BG,
  APP_PANEL_BG_DARK,
  APP_PANEL_BG_LIGHT,
  APP_PANEL_BORDER_DARK,
  APP_PANEL_BORDER_LIGHT,
  APP_TEXT_DARK,
  APP_TEXT_LIGHT,
  APP_WINDOW_BG_DARK,
  APP_WINDOW_BG_LIGHT
} from './src/main/ets/component/theme/PlayerTheme';

// ---- 播放器 UI：总装 ----
export { CicadaComposePlayer, PlayerPanelKind } from './src/main/ets/component/compose/CicadaComposePlayer';

// ---- 播放器 UI：控制层（顶部栏 / 底部栏 / 进度条 / seek 预览 / 弹幕输入条 / 图标按钮） ----
export {
  PlayerTopBar,
  PlayerBottomBar,
  PlayerProgressBar,
  PlayerThinProgressBar,
  SeekPreviewPopup,
  PreviewLoadingDot,
  BarText,
  DanmakuInputBar,
  formatPlayerTime
} from './src/main/ets/component/compose/PlayerControls';
export {
  PlayerIcon,
  PlayerIconButton,
  PlayerBackIcon,
  IconButtonBox,
  ICON_OPACITY_CONTROL_TEXT,
  ICON_OPACITY_ACTIVE
} from './src/main/ets/component/compose/PlayerIcons';

// ---- 播放器 UI：Lottie 图标（播放/暂停键、进度条 dot） ----
export {
  PlayPauseIconButton,
  DanmakuDotThumb,
  PlayerLottieAsset,
  LOTTIE_SLOT_PLAYING,
  LOTTIE_SLOT_PAUSED
} from './src/main/ets/component/compose/LottieIcon';

// ---- 播放器 UI：浮层面板 ----
export {
  PlayerPanelShell,
  PanelRow,
  PlayerQualityPanel,
  PlayerSpeedPanel,
  PlayerSubtitlePanel,
  PlayerDanmakuPanel,
  PlayerVolumePanel,
  PlayerInfoPanel,
  InfoRow,
  MirrorUiMode,
  MirrorRow,
  MIRROR_ROWS,
  ColorVisionKind,
  ColorVisionRow,
  COLOR_VISION_ROWS,
  colorVisionMatrix
} from './src/main/ets/component/compose/PlayerPanels';

// ---- 播放器 UI：信息 / 统计与清晰度、字幕列表的派生 ----
export {
  PlayerStats,
  QualityOption,
  SubtitleOption,
  TRACK_INDEX_AUTO,
  buildQualities,
  buildSubtitles,
  qualityLabelOf,
  qualityLabelFor,
  codecLabelOf,
  codecDisplayNameOf
} from './src/main/ets/component/compose/PlayerStats';

// ---- 播放器 UI：弹幕层 ----
export {
  DanmakuView,
  Danmaku,
  DanmakuType,
  DanmakuSpeed,
  DanmakuConfig,
  DanmakuSprite,
  DanmakuLaneEngine,
  danmakuSpeedMultiplier
} from './src/main/ets/component/compose/PlayerDanmakuLayer';

// ---- 播放器 UI：手势层 ----
export { PlayerGestureLayer } from './src/main/ets/component/compose/PlayerGestureLayer';
