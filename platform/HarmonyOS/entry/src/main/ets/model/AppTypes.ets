/*
 * Demo-application types (NOT part of the SDK).
 *
 * 播放器自身的类型与枚举（ScaleMode / StreamType / StreamInfo / CicadaPlayerListener ...）
 * 已经随 SDK 搬到 HAR 里，请直接从 '@cicada/cicadaplayer' 导入；
 * 这里只留下本示例 App 自己的东西：sourceList.json 结构、路由参数、多实例选择。
 */

/** External subtitle URLs attached to a source entry. */
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
