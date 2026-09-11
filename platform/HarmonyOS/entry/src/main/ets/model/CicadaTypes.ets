/*
 * Shared types for the HarmonyOS demo.
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
  static readonly SUBTITLE_HEADER: string = 'subtitleHeader';
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
}

/**
 * External subtitle URLs attached to a source entry.
 * Mirrors the Android demo's sourceList.json "subtitle" object.
 */
export interface SourceSubtitle {
  /** zh WebVTT url. */
  cn?: string;
  /** en-US WebVTT url. */
  en?: string;
  /** Local .ass path. (The Android demo never reads this when passed by Intent.) */
  ass?: string;
}

/**
 * One media source entry, mirroring sourceList.json's samples[].
 * `type` is "url" or "live" for list entries; the local-file fallback uses "URL".
 */
export interface SourceSample {
  url: string;
  type: string;
  name: string;
  subtitle?: SourceSubtitle;
}

/** A named group of sources (sourceList.json's top-level array element). */
export interface SourceGroup {
  name: string;
  samples: SourceSample[];
}

/** Values used by SourceSample.type, matching CicadaPlayerActivity's constants. */
export class SourceType {
  static readonly URL: string = 'url';
  static readonly LIVE: string = 'live';
  static readonly LOCAL: string = 'local';
  static readonly MANIFEST: string = 'manifest';
}

/** router params: SourceChoosePage -> SourceChooseListPage. */
export interface SourceGroupParams {
  groupName: string;
}

/** router params: any source entry -> the player page. */
export interface PlayerRouteParams {
  url: string;
  /** SourceType.URL / LIVE / LOCAL / MANIFEST. */
  playType: string;
  /** Group title, used as the player's title bar text. */
  groupName?: string;
  /** External subtitle urls, when the entry carried them. */
  subtitleCn?: string;
  subtitleEn?: string;
}

/** One selection in the multi-instance list. */
export interface MultiPlayerSelection {
  url: string;
  name: string;
}
