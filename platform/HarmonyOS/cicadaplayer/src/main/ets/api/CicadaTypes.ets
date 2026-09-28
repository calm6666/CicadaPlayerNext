/*
 * CicadaPlayerNext HarmonyOS SDK —— 播放器类型/枚举/回调定义（HAR 对外导出）。
 *
 * Mirrors the enums/structures of the native player
 * (mediaPlayer/native_cicada_player_def.h) plus the JSON payloads produced by
 * the NAPI event bridge (entry/src/main/cpp/napi_events.cpp).
 */

/** ScaleMode (native: SM_FIT / SM_CROP / SM_EXTRACTTOFIT). */
export enum ScaleMode {
  SM_FIT = 0,
  SM_CROP = 1,
  SM_EXTRACTTOFIT = 2
}

/** RotateMode (native values are the degrees). */
export enum RotateMode {
  ROTATE_0 = 0,
  ROTATE_90 = 90,
  ROTATE_180 = 180,
  ROTATE_270 = 270
}

/** MirrorMode. */
export enum MirrorMode {
  MIRROR_MODE_NONE = 0,
  MIRROR_MODE_HORIZONTAL = 1,
  MIRROR_MODE_VERTICAL = 2
}

/** StreamType (native: ST_TYPE_*). */
export enum StreamType {
  ST_TYPE_UNKNOWN = -1,
  ST_TYPE_VIDEO = 0,
  ST_TYPE_AUDIO = 1,
  ST_TYPE_SUB = 2
}

/** DecoderType. */
export enum DecoderType {
  DT_HARDWARE = 0,
  DT_SOFTWARE = 1
}

/**
 * PlayerStatus (native: PlayerStatus).
 * 99 is PLAYER_ERROR.
 */
export enum PlayerStatus {
  PLAYER_IDLE = 0,
  PLAYER_INITIALZED = 1,
  PLAYER_PREPARINIT = 2,
  PLAYER_PREPARING = 3,
  PLAYER_PREPARED = 4,
  PLAYER_PLAYING = 5,
  PLAYER_PAUSED = 6,
  PLAYER_STOPPED = 7,
  PLAYER_COMPLETION = 8,
  PLAYER_ERROR = 99
}

/** PropertyKey (native: PropertyKey) used by getPropertyLong(). */
export enum PropertyKey {
  PROPERTY_KEY_RESPONSE_INFO = 0,
  PROPERTY_KEY_CONNECT_INFO = 1,
  PROPERTY_KEY_OPEN_TIME_STR = 2,
  PROPERTY_KEY_PROBE_STR = 3,
  PROPERTY_KEY_VIDEO_BUFFER_LEN = 4,
  PROPERTY_KEY_DELAY_INFO = 5,
  PROPERTY_KEY_REMAIN_LIVE_SEG = 6,
  PROPERTY_KEY_NETWORK_IS_CONNECTED = 7,
  PROPERTY_KEY_PLAY_CONFIG = 8,
  PROPERTY_KEY_DECODE_INFO = 9,
  PROPERTY_KEY_HLS_KEY_URL = 10,
  PROPERTY_KEY_RE_BUFFERING = 11,
  PROPERTY_KEY_DOWNLOAD_COMPLETED = 12,
  PROPERTY_KEY_VIDEO_DROPPED_INFO = 13,
  PROPERTY_KEY_NETWORK_SPEED = 14,
  PROPERTY_KEY_BUFFER_INFO = 15,
  PROPERTY_KEY_NETWORK_REQUEST_LIST = 16,
  PROPERTY_KEY_RENDER_INFO = 17,
  PROPERTY_KEY_CONTAINER_INFO = 18
}

/** One track, as serialised by streamInfoToJson() in napi_events.cpp. */
export interface StreamInfo {
  streamIndex: number;
  type: string;
  description: string;
  bitrate: number;
  videoBandwidth: number;
  videoWidth: number;
  videoHeight: number;
  hdrType: number;
  audioLang: string;
  nChannels: number;
  sampleRate: number;
  sampleFormat: number;
  subtitleLang: string;
}

/** The whole track list, as serialised by mediaInfoToJson(). */
export interface MediaInfo {
  totalBitrate: number;
  streams: StreamInfo[];
}

/** Payload of getVideoResolution(). */
export class VideoResolution {
  width: number = 0;
  height: number = 0;

  constructor(width: number = 0, height: number = 0) {
    this.width = width;
    this.height = height;
  }
}

/** Event names emitted by the native bridge. */
export class PlayerEventName {
  static readonly LOOPING_START: string = 'loopingStart';
  static readonly PREPARED: string = 'prepared';
  static readonly COMPLETION: string = 'completion';
  static readonly FIRST_FRAME_SHOW: string = 'firstFrameShow';
  static readonly LOADING_START: string = 'loadingStart';
  static readonly LOADING_END: string = 'loadingEnd';
  static readonly AUTO_PLAY_START: string = 'autoPlayStart';
  static readonly SEEKING: string = 'seeking';
  static readonly SEEK_END: string = 'seekEnd';
  static readonly POSITION_UPDATE: string = 'positionUpdate';
  static readonly UTC_TIME_UPDATE: string = 'utcTimeUpdate';
  static readonly BUFFER_POSITION_UPDATE: string = 'bufferPositionUpdate';
  static readonly LOADING_PROGRESS: string = 'loadingProgress';
  static readonly DOWNLOAD_SPEED: string = 'currentDownLoadSpeed';
  static readonly VIDEO_SIZE_CHANGED: string = 'videoSizeChanged';
  static readonly STATUS_CHANGED: string = 'statusChanged';
  static readonly VIDEO_RENDERED: string = 'videoRendered';
  static readonly AUDIO_RENDERED: string = 'audioRendered';
  static readonly ERROR: string = 'error';
  static readonly INFO: string = 'event';
  static readonly MEDIA_INFO: string = 'mediaInfo';
  static readonly STREAM_SWITCH_SUC: string = 'streamSwitchSuc';
  static readonly SNAPSHOT: string = 'snapshot';
  static readonly SUBTITLE_HIDE: string = 'subtitleHide';
  static readonly SUBTITLE_SHOW: string = 'subtitleShow';
  static readonly SUBTITLE_EXT_ADD: string = 'subtitleExtAdd';
  static readonly VIDEO_QUALITY_SWITCH: string = 'videoQualitySwitch';
  static readonly SUBTITLE_HEADER: string = 'subtitleHeader';
  /** 播放缓存写成（内核 MEDIA_PLAYER_EVENT_CACHE_SUCCESS）。 */
  static readonly CACHE_SUCCESS: string = 'cacheSuccess';
  /** 播放缓存失败（内核 MEDIA_PLAYER_EVENT_CACHE_ERROR）。 */
  static readonly CACHE_ERROR: string = 'cacheError';
}

/**
 * 内核 MediaPlayerEventType 的数值（mediaPlayer/media_player_error_def.h）。
 *
 * 缓存这两条事件是走既有的 EventCallback 通道上报的，在 NAPI 桥上它的名字统一是
 * 'event'（对应 PlayerEventName.INFO），**事件身份只有一个数字 code**，所以这里
 * 必须按 code 分辨，不能按名字分辨。
 *
 * 数值推导（枚举在 media_player_error_def.h 里是隐式递增的，起点是
 * MEDIA_PLAYER_EVENT_START = 0，且 SW_VIDEO_DECODER 与 START 同值）：
 *   0 START/SW_VIDEO_DECODER, 1 AUDIO_CODEC_NOT_SUPPORT, 2 AUDIO_DECODER_DEVICE_ERROR,
 *   3 VIDEO_CODEC_NOT_SUPPORT, 4 VIDEO_DECODER_DEVICE_ERROR, 5 VIDEO_RENDER_INIT_ERROR,
 *   6 DEMUXER_TRACE_ID, 7 DEMUXER_EOF, 8 NETWORK_RETRY, 9 NETWORK_RETRY_SUCCESS,
 *   10 CACHE_SUCCESS, 11 CACHE_ERROR, 12 SYSTEM_LOW_MEMORY, ...
 * 与本文件里其它"镜像内核枚举"的写法一致，改内核那份枚举时这里要同步。
 */
export class PlayerEventCode {
  static readonly CACHE_SUCCESS: number = 10;
  static readonly CACHE_ERROR: number = 11;
}

/**
 * Callback surface of the player, mirroring the Android demo's listener set.
 * Every member is optional so a page only implements what it needs.
 */
export interface CicadaPlayerListener {
  onPrepared?: () => void;
  onCompletion?: () => void;
  onFirstFrameShow?: () => void;
  onAutoPlayStart?: () => void;
  onLoopingStart?: () => void;
  onError?: (code: number, msg: string) => void;
  onInfo?: (code: number, msg: string) => void;
  onPositionUpdate?: (positionMs: number) => void;
  onBufferPositionUpdate?: (positionMs: number) => void;
  onSeeking?: (positionMs: number) => void;
  onSeekEnd?: (positionMs: number) => void;
  onLoadingStart?: () => void;
  onLoadingProgress?: (percent: number) => void;
  onLoadingEnd?: () => void;
  onDownloadSpeed?: (bytesPerSecond: number) => void;
  onVideoSizeChanged?: (width: number, height: number) => void;
  onStatusChanged?: (oldStatus: number, newStatus: number) => void;
  onVideoRendered?: (timeMs: number, pts: number) => void;
  onAudioRendered?: (timeMs: number, pts: number) => void;
  onMediaInfo?: (info: MediaInfo) => void;
  onStreamSwitchSuc?: (streamType: number, info: StreamInfo) => void;
  onSnapshot?: (width: number, height: number, rgba: ArrayBuffer) => void;
  onSubtitleShow?: (id: number, text: string) => void;
  onSubtitleHide?: (id: number) => void;
  onSubtitleExtAdd?: (index: number, url: string) => void;
  onVideoQualitySwitch?: (status: number, streamIndex: number, desc: string) => void;
  /** 播放缓存写成（内核 MEDIA_PLAYER_EVENT_CACHE_SUCCESS，code = 10）。 */
  onCacheSuccess?: () => void;
  /** 播放缓存失败（内核 MEDIA_PLAYER_EVENT_CACHE_ERROR，code = 11），msg 是失败原因。 */
  onCacheError?: (msg: string) => void;
}

/** 清晰度切换状态（native: player_quality_switch_status）。 */
export enum VideoQualitySwitchStatus {
  STARTED = 0,
  READY = 1,
  FAILED = 2,
  CANCELED = 3
}

/**
 * 设备的硬解能力与效率偏好（native: CicadaGet/SetVideoCodecSupport 的 JSON 契约）。
 * 应用侧可以把它持久化后回传，内核就不再自己探测设备。
 */
export interface VideoCodecSupport {
  /** get 时说明数据来自应用层（app）还是内核探测（kernel）；set 时忽略。 */
  source: string;
  /** 设备能硬解的编码短名集合，顺序无关（H.264 / H.265 / AV1 / VP9 / MPEG-4 / MPEG-2）。 */
  hwDecode: string[];
  /** 从高到低的效率序覆盖；缺省时内核用自己的效率序。 */
  preference: string[];
  /** 手动指定的默认视频格式；空串 = 不指定（全自动）。 */
  preferred: string;
}

/**
 * DRM 许可证回调。
 *
 * 内核在播放线程上同步索要许可证，回调必须当场返回：ArrayBuffer（原始字节）
 * 或 base64 文本都可以；返回 null / 空表示拿不到许可证。回调里不要调用播放器
 * 的同步接口，否则会与播放线程互相等待。
 */
export type CicadaDrmCallback =
  (drmType: string, requestType: string, url: string, data: ArrayBuffer) => ArrayBuffer | string | null;

/**
 * 一个流的原始元数据（native: Stream_meta）。
 *
 * 与 StreamInfo 的分工：StreamInfo 是展示用字段（码率/分辨率/语言…），这里是内核
 * 原样数值，排查"这个流到底被解成了什么"时用它。codec 是内核 AFCodecID 的枚举值；
 * 编码短名（H.264 / H.265 / AV1 …）统一从 getVideoCodecSupport() 取，内核里那份
 * 短名表只有一份。
 */
export interface StreamMeta {
  index: number;
  /** video / audio / subtitle / mixed / unknown。 */
  type: string;
  /** 内核 AFCodecID 枚举值。 */
  codec: number;
  codecTag: number;
  duration: number;
  bitrate: number;
  bandwidth: number;
  width: number;
  height: number;
  displayWidth: number;
  displayHeight: number;
  rotate: number;
  avgFps: number;
  /** InterlacedType 枚举值。 */
  interlaced: number;
  /** AFPixelFormat 枚举值。 */
  pixelFormat: number;
  channels: number;
  sampleRate: number;
  frameSize: number;
  profile: number;
  /** AFSampleFormat 枚举值。 */
  sampleFormat: number;
  bitsPerCodedSample: number;
  ptsTimeBase: number;
  extradataSize: number;
  /** 该流是否带独立密钥 URL（标准 DRM）。 */
  hasKeyUrl: boolean;
  lang: string;
  title: string;
  description: string;
}

/**
 * 播放缓存配置（native: cacheModule/cache/CacheConfig.h 的成员，字段一一对应）。
 *
 * 对应 NAPI 的 setCacheConfig(id, json)，json 就是本接口的 JSON.stringify；
 * 字段名与内核 CacheConfig 的成员同名，**不要另造名字**。
 *
 * 用法：必须在 setDataSource() 之前调 setCacheConfig()，否则对本次播放无效
 * （缓存代理 URL 是 setDataSource 那一刻算出来的）。
 */
export interface CacheConfig {
  /** 是否开启播放缓存；false 或整体传 null = 关闭。 */
  enable: boolean;
  /** 只缓存不超过这个时长的媒体（秒）；0 = 不按这条限制。 */
  maxDurationS: number;
  /** 缓存目录容量上限（MB），也是"只在缓存目录内腾空间"的判据。 */
  maxDirSizeMB: number;
  /** 缓存目录（必填，否则算不出缓存文件路径）。 */
  cacheDir: string;
  /** 缓存文件名；留空则按源 URL 的 MD5 生成。 */
  cacheFileName: string;
  /** 源文件大小（字节）；内核拿不到 Content-Length 时用它做容量判断。 */
  sourceSize: number;
}