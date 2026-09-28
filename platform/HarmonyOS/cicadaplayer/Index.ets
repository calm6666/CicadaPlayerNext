/*
 * CicadaPlayerNext HarmonyOS SDK —— 对外唯一入口。
 *
 * 其它 HarmonyOS 工程只要依赖本 HAR（见 docs/Packaging_HarmonyOS.md 的接入一节），
 * 即可拿到三层能力：
 *   1) 播放器内核：自研 CicadaPlayerNext（FFmpeg 解封装/软解 + OH_AVCodec 硬解 +
 *      OHAudio 渲染 + DRM Kit + DASH/HLS/对象清单），编在 libcicadaplayer.so 里；
 *   2) ArkTS SDK：CicadaPlayer 句柄封装 + 事件回调 + 全部类型/枚举；
 *   3) 开箱即用组件：CicadaVideo（XComponent + 生命周期 + 播放器装配）以及
 *      控制条 / 手势层 / 轨道面板 / 选项面板 / 提示浮层。
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

// ---- 开箱即用组件 ----
export { CicadaVideo } from './src/main/ets/component/CicadaVideo';
export { PlayerControlBar } from './src/main/ets/component/PlayerControlBar';
export { PlayerGestureLayer } from './src/main/ets/component/PlayerGestureLayer';
export { PlayerTipsOverlay } from './src/main/ets/component/PlayerTipsOverlay';
export { PlayerTrackPanel, TRACK_INDEX_AUTO } from './src/main/ets/component/PlayerTrackPanel';
export { PlayerOptionPanel } from './src/main/ets/component/PlayerOptionPanel';
