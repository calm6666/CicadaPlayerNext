/*
 * 「视频信息 / 统计」面板的数据与派生列表 —— ArkTS 版，来源是 Compose 安卓版的
 *   platform/Android/ComposePlayer/app/src/main/java/com/cicada/player/compose/components/video/PlayerStats.kt
 *
 * 覆盖 Compose 里同名的全部内容：
 *   · class PlayerStats（全部字段与 *_Text() 方法）；
 *   · QualityOption / SubtitleOption；
 *   · buildQualities / qualityGroupKey / qualityLabelOf / codecLabelOf / codecDisplayNameOf /
 *     qualityLabelFor / buildSubtitles。
 *
 * 与 Compose 的差异（都是"内核/桥接层给不出这个读数"造成的，逐条写在这里）：
 *   · Compose 的 `TrackInfo.getVideoCodec()`（编码短名，"H.264"/"H.265"）在鸿蒙的 NAPI 桥接层
 *     没有对应字段（StreamInfo 只有 description / bitrate / videoBandwidth / 宽高 / 语言…），
 *     所以 `codecLabelOf()` 返回空串 —— 与 Compose"内核认不出来就是空串"同一条口径：
 *     界面**不渲染编码徽标**，也不按分辨率/码率反推，不猜；
 *   · 「解码方式」按同一口径降级：有当前视频流就写"硬解"/"软解"（编码段为空），
 *     没有当前视频流整栏 "--"；
 *   · 帧率用 `getVideoDecodeFps()`（内核事实读数，实际解码帧率）。C API 不暴露"流的标称帧率"，
 *     所以这里不假装能给出标称值；
 *   · 「丢帧/总帧」读 `PROPERTY_KEY_VIDEO_DROPPED_INFO` 的字符串读数，读不到就是 "0"
 *     （与 Compose 的 `if (droppedFrames > 0) "$droppedFrames" else "0"` 同一条显示口径）。
 */

import { CicadaPlayer } from '../../api/CicadaPlayer';
import { PropertyKey, StreamInfo, StreamType, VideoResolution } from '../../api/CicadaTypes';
import { formatPlayerTime } from './PlayerControls';

/** Compose：`TrackInfo.AUTO_SELECT_INDEX = -1`（"自动/关闭"那一行）。 */
export const TRACK_INDEX_AUTO: number = -1;

/**
 * 「视频信息」面板的数据。
 * 默认值就是"没有播放器 / 没有当前视频流"那一档：hasVideoStream = false ⇒ 整栏 "--"。
 */
export class PlayerStats {
  widthPx: number = 0;
  heightPx: number = 0;
  sampleMimeType: string = '';
  decoderName: string = '';
  frameRate: number = 0;
  bitrate: number = 0;
  droppedFrames: number = 0;
  bufferedMs: number = 0;
  positionMs: number = 0;
  durationMs: number = 0;
  speedBps: number = 0;
  /** 当前视频流的编码短名；空串 = 桥接层给不出（见文件头说明） */
  videoCodecShort: string = '';
  /** 有没有"当前视频流"；false ⇒ 「解码方式」整栏 "--" */
  hasVideoStream: boolean = false;
  /** 活动解码器**实际**是否在硬解（内核事实读数） */
  hardwareDecoder: boolean = false;
  /** 硬件名（SoC/硬件/板级型号）；空串 = 拿不到 ⇒ 不写括号 */
  hardwareName: string = '';

  resolutionText(): string {
    if (this.widthPx <= 0 || this.heightPx <= 0) {
      return '--';
    }
    const fps: string = this.frameRate > 0 ? `@${this.frameRate.toFixed(3)}` : '';
    return `${this.widthPx} x ${this.heightPx}${fps}`;
  }

  codecText(): string {
    return this.sampleMimeType.length > 0 ? this.sampleMimeType : '--';
  }

  /**
   * 「解码方式」的最终显示文案（与 Compose 的 decodeMethodText 同一套口径）：
   *   硬解 HEVC（SM8650） / 硬解 HEVC / 硬解 / 软解 H.264 / 软解 / --
   * 软解**绝不写型号**。编码那一段走同一处 codecDisplayNameOf。
   */
  decodeMethodText(): string {
    if (!this.hasVideoStream) {
      return '--';
    }
    const codec: string = codecDisplayNameOf(this.videoCodecShort);
    if (this.hardwareDecoder) {
      if (codec.length === 0) {
        return '硬解';
      }
      if (this.hardwareName.length === 0) {
        return `硬解 ${codec}`;
      }
      return `硬解 ${codec}（${this.hardwareName}）`;
    }
    return codec.length === 0 ? '软解' : `软解 ${codec}`;
  }

  decoderNameText(): string {
    return this.decoderName.length > 0 ? this.decoderName : '--';
  }

  frameRateText(): string {
    return this.frameRate > 0 ? `${this.frameRate.toFixed(2)} fps` : '--';
  }

  bitrateText(): string {
    return this.bitrate > 0 ? `${Math.floor(this.bitrate / 1000)} kbps` : '--';
  }

  droppedText(): string {
    return this.droppedFrames > 0 ? `${this.droppedFrames}` : '0';
  }

  bufferText(): string {
    return this.bufferedMs > 0 ? `${Math.floor(this.bufferedMs / 1000)} s` : '--';
  }

  positionText(): string {
    return `${formatPlayerTime(this.positionMs)} / ${formatPlayerTime(this.durationMs)}`;
  }

  speedText(): string {
    if (this.speedBps <= 0) {
      return '--';
    }
    if (this.speedBps >= 1024 * 1024) {
      return `${(this.speedBps / 1024 / 1024).toFixed(2)} MB/s`;
    }
    return `${(this.speedBps / 1024).toFixed(1)} KB/s`;
  }

  /**
   * 从内核取快照（取不到的字段保持默认 ⇒ 面板显示 "--"）。
   *
   * Compose 的 `from(controller)` 在这里对应 `from(player, ...)`：位置/缓冲/网速由页面
   * 通过事件送进来（鸿蒙侧是事件驱动，不在统计面板里新开计时器），其余全部现读内核。
   */
  static from(
    player: CicadaPlayer,
    positionMs: number,
    durationMs: number,
    bufferedMs: number,
    downloadSpeedBps: number,
    decoderNotice: string
  ): PlayerStats {
    const stats: PlayerStats = new PlayerStats();
    const video: StreamInfo | null = player.getCurrentStreamInfo(StreamType.ST_TYPE_VIDEO);
    const size: VideoResolution = player.getVideoResolution();

    stats.widthPx = size.width;
    stats.heightPx = size.height;
    stats.positionMs = positionMs;
    stats.durationMs = durationMs;
    stats.bufferedMs = bufferedMs > positionMs ? bufferedMs - positionMs : 0;
    stats.speedBps = downloadSpeedBps;
    stats.frameRate = player.getVideoDecodeFps();
    stats.hardwareDecoder = player.isVideoDecoderHardware();
    stats.hasVideoStream = video !== null;
    stats.decoderName = decoderNotice;
    /* 编码短名：桥接层没有这个读数（见文件头），保持空串 ⇒ 界面不渲染徽标、不猜 */
    stats.videoCodecShort = '';
    /* 硬件名：鸿蒙侧没有"在已有 GL 上下文线程上读 GL_RENDERER"的地方，也不许为一行字去新建上下文 */
    stats.hardwareName = '';
    if (video !== null) {
      stats.bitrate = video.bitrate > 0 ? video.bitrate : video.videoBandwidth;
      /* description 是桥接层唯一能给出的编码/格式描述，用于「编解码器」那一栏的显示 */
      stats.sampleMimeType = video.description;
    }
    /* 丢帧读数走属性通道；读不到就是空串 ⇒ droppedText() 显示 "0" */
    const dropped: string = player.getPropertyString(PropertyKey.PROPERTY_KEY_VIDEO_DROPPED_INFO, '');
    const parsed: number = dropped.length > 0 ? Number.parseInt(dropped) : 0;
    stats.droppedFrames = Number.isNaN(parsed) ? 0 : parsed;
    return stats;
  }
}

/**
 * 清晰度一档。
 * `index` 是**内核 streamIndex**（= StreamInfo.streamIndex），不是本列表的下标：
 * 本列表要"按分辨率+编码分组 + 按带宽降序"，顺序与 streamIndex 毫无关系，
 * 点击时必须原样交给 `CicadaPlayer.selectTrack(index)`。
 */
export class QualityOption {
  index: number = TRACK_INDEX_AUTO;
  label: string = '';
  selected: boolean = false;
  /** 编码显示名（徽标文本）；空串 ⇒ 不渲染徽标 */
  codec: string = '';

  constructor(index: number = TRACK_INDEX_AUTO, label: string = '', selected: boolean = false, codec: string = '') {
    this.index = index;
    this.label = label;
    this.selected = selected;
    this.codec = codec;
  }
}

/** 字幕一档。 */
export class SubtitleOption {
  index: number = TRACK_INDEX_AUTO;
  label: string = '';
  selected: boolean = false;

  constructor(index: number = TRACK_INDEX_AUTO, label: string = '', selected: boolean = false) {
    this.index = index;
    this.label = label;
    this.selected = selected;
  }
}

/**
 * 一档视频轨的**编码短名**（徽标文本的原料）。
 * 鸿蒙桥接层不暴露编码短名 ⇒ 恒为空串；界面按"空串 = 不渲染徽标"处理。
 */
export function codecLabelOf(track: StreamInfo): string {
  /* 内核短名在桥接层没有对应字段：只在"轨有效"的前提下谈编码，认不出就是空串 */
  if (track.streamIndex === TRACK_INDEX_AUTO) {
    return '';
  }
  return '';
}

/**
 * 【编码短名 → 显示名】整个播放器唯一的一处映射，**只用于显示**：
 *     H.265 → HEVC；其余（H.264 / AV1 / VP9 / 空串 …）原样返回。
 */
export function codecDisplayNameOf(codec: string): string {
  const trimmed: string = codec.trim();
  return trimmed === 'H.265' ? 'HEVC' : trimmed;
}

/**
 * 一档视频轨的显示名（与 Compose 完全一致的写法）：
 *   2160 及以上 → 4K；1440 及以上 → 2K；有高度 → 1080P；
 *   没高度 → 宽 x 高；连宽高都没有 → kbps；都没有 → 未知。
 */
export function qualityLabelOf(track: StreamInfo): string {
  const w: number = track.videoWidth;
  const h: number = track.videoHeight;
  const kbps: number = Math.floor(track.videoBandwidth / 1000);
  if (h >= 2160) {
    return '4K';
  }
  if (h >= 1440) {
    return '2K';
  }
  if (h > 0) {
    return `${h}P`;
  }
  if (w > 0) {
    return `${w}`;
  }
  if (kbps > 0) {
    return `${kbps} kbps`;
  }
  return '未知';
}

/** 合并分组的键：**分辨率 + 编码**（编码拿不到时退回"只按分辨率合并"，与 Compose 同兜底）。 */
function qualityGroupKey(track: StreamInfo): string {
  const h: number = track.videoHeight;
  const w: number = track.videoWidth;
  let resolution: string = '';
  if (h > 0) {
    resolution = `h${h}`;
  } else if (w > 0) {
    resolution = `w${w}`;
  } else {
    resolution = `#${track.streamIndex}`;
  }
  return `${resolution}|${codecLabelOf(track)}`;
}

/** 按内核给的视频流下标找档位名（清晰度切换提示的文案用）；找不到返回空串。 */
export function qualityLabelFor(streams: StreamInfo[], index: number): string {
  if (index < 0) {
    return '';
  }
  for (let i = 0; i < streams.length; i++) {
    const stream: StreamInfo = streams[i];
    if (stream.type === 'video' && stream.streamIndex === index) {
      return qualityLabelOf(stream);
    }
  }
  return '';
}

/**
 * 由内核 MediaInfo 组清晰度列表。
 *
 * ① 分组键 = 分辨率 + 编码；同组取带宽最大者当代表；
 * ② 合并后按带宽降序排；
 * ③ "自动"追加到**末尾**（自动档在 ABR 已选出实际档位后带上括号显示它）；
 * ④ 行的身份 = 内核 streamIndex，绝不是列表下标。
 */
export function buildQualities(
  streams: StreamInfo[],
  currentIndex: number,
  autoQuality: boolean
): QualityOption[] {
  const videos: StreamInfo[] = [];
  for (let i = 0; i < streams.length; i++) {
    if (streams[i].type === 'video') {
      videos.push(streams[i]);
    }
  }

  if (videos.length <= 1) {
    return [new QualityOption(TRACK_INDEX_AUTO, '自动', true, '')];
  }

  const grouped: Map<string, StreamInfo[]> = new Map<string, StreamInfo[]>();
  for (let i = 0; i < videos.length; i++) {
    const key: string = qualityGroupKey(videos[i]);
    const bucket: StreamInfo[] | undefined = grouped.get(key);
    if (bucket === undefined) {
      grouped.set(key, [videos[i]]);
    } else {
      bucket.push(videos[i]);
    }
  }

  const groups: StreamInfo[][] = [];
  grouped.forEach((bucket: StreamInfo[]) => {
    bucket.sort((a: StreamInfo, b: StreamInfo) => b.videoBandwidth - a.videoBandwidth);
    groups.push(bucket);
  });
  groups.sort((a: StreamInfo[], b: StreamInfo[]) => b[0].videoBandwidth - a[0].videoBandwidth);

  /* 自动档在 ABR 已经选出实际档位之后带上括号显示它（认行按"是不是本组任一成员"） */
  let actualLabel: string = '';
  if (autoQuality) {
    for (let i = 0; i < groups.length; i++) {
      const group: StreamInfo[] = groups[i];
      for (let j = 0; j < group.length; j++) {
        if (group[j].streamIndex === currentIndex) {
          actualLabel = qualityLabelOf(group[0]);
          break;
        }
      }
      if (actualLabel.length > 0) {
        break;
      }
    }
  }
  const auto: QualityOption = new QualityOption(
    TRACK_INDEX_AUTO,
    actualLabel.length > 0 ? `自动（${actualLabel}）` : '自动',
    autoQuality,
    ''
  );

  const result: QualityOption[] = [];
  for (let i = 0; i < groups.length; i++) {
    const group: StreamInfo[] = groups[i];
    const representative: StreamInfo = group[0];
    let hit: boolean = false;
    for (let j = 0; j < group.length; j++) {
      if (group[j].streamIndex === currentIndex) {
        hit = true;
        break;
      }
    }
    result.push(new QualityOption(
      representative.streamIndex,
      qualityLabelOf(representative),
      !autoQuality && hit,
      codecDisplayNameOf(codecLabelOf(representative))
    ));
  }
  result.push(auto);
  return result;
}

/**
 * 由内核 MediaInfo 组字幕列表；第一行是「关闭字幕」。
 * 名字优先 subtitleLang，其次 description，都没有就 `字幕 N`。
 */
export function buildSubtitles(
  streams: StreamInfo[],
  currentIndex: number,
  subtitlesEnabled: boolean
): SubtitleOption[] {
  const subs: StreamInfo[] = [];
  for (let i = 0; i < streams.length; i++) {
    if (streams[i].type === 'subtitle') {
      subs.push(streams[i]);
    }
  }

  const result: SubtitleOption[] = [];
  result.push(new SubtitleOption(TRACK_INDEX_AUTO, '关闭字幕', !subtitlesEnabled || subs.length === 0));

  for (let i = 0; i < subs.length; i++) {
    const track: StreamInfo = subs[i];
    let name: string = '';
    if (track.subtitleLang.trim().length > 0) {
      name = track.subtitleLang;
    } else if (track.description.trim().length > 0) {
      name = track.description;
    } else {
      name = `字幕 ${i + 1}`;
    }
    result.push(new SubtitleOption(
      track.streamIndex,
      name,
      subtitlesEnabled && track.streamIndex === currentIndex
    ));
  }
  return result;
}
