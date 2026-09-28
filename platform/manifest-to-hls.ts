/**
 * MediaManifest → hls.js ManifestVariant/ManifestAudioGroup 转换器
 *
 * 将统一的 MediaManifest 对象一次性转换为 hls.js 的 loadManifest 方法
 * 所需的 ManifestVariant[] 和 ManifestAudioGroup[] 格式。
 *
 * 核心原则：
 * - 一次性转换，不进行多次中间转换
 * - 输出类型与 hls.js 构建产物导出的类型完全一致
 * - 零 m3u8 文本，所有 URL 直接指向 .m4s 文件
 * - hls.js 内部负责将 ManifestVariant 转换为 LevelParsed/LevelDetails/Fragment
 */

import type {
  ManifestVariant,
  ManifestAudioGroup,
  ManifestPlaylistDetails,
  ManifestSegment,
} from 'hls.js';
import type {
  MediaManifest,
  MediaRepresentation,
  SegmentInfo,
  Segment,
  Aes128Encryption,
  LicenseServer,
} from './types/index';
import { resolveUrl, deriveMediaPattern, applySuffix } from './utils/index';

/**
 * 确保 URL 以 / 结尾
 *
 * hls.js 的 variant.url / audioGroup.url 会与分片 URL 拼接，
 * 因此必须以 / 结尾。用户传入的 baseUrl 不应包含尾部 /，
 * 由转换器统一补全。
 */
function ensureTrailingSlash(url: string): string {
  return url.endsWith('/') ? url : url + '/';
}

/**
 * DASH 记法的字节范围（"start-end"）→ HLS 记法（"长度@起点"）
 *
 * 清单 JSON 里 byteRange / single 模式的 initialization 用的是 DASH 闭区间（"908-588221"）；
 * hls.js 的 setByteRange() 按 #EXT-X-BYTERANGE 语义解析「长度[@起点]」（内部 end = 起点 + 长度）。
 * 两种记法不换算会整体错位，所以在这里统一转一次。已经是 HLS 记法（含 @）的原样返回。
 */
function toHlsByteRange(range?: string): string | undefined {
  if (!range) return undefined;
  const text = range.trim();
  if (!text) return undefined;
  if (text.includes('@')) return text;
  const matched = /^(\d+)-(\d+)$/.exec(text);
  if (!matched) return undefined;
  const start = Number(matched[1]);
  const end = Number(matched[2]);
  if (!Number.isFinite(start) || !Number.isFinite(end) || end < start) return undefined;
  return `${end - start + 1}@${start}`;
}

/**
 * single（单文件 + 字节范围）模式下 baseUrl 是**媒体文件本身**而不是目录：
 * 补尾部斜杠会让 hls.js 拼出 "xxx.m4s/" 这种错地址。
 */
function isSingleFileMode(segInfo?: SegmentInfo): boolean {
  return segInfo?.mode === 'single';
}

/**
 * AES-128 配置写入 PlaylistDetails（两种模式共用）
 */
function applyEncryption(
  result: ManifestPlaylistDetails,
  encryption?: Aes128Encryption,
  licenseServer?: LicenseServer,
): void {
  if (!encryption) return;
  /* expiresIn > 0 表示临时密钥，播放器需从 License Server 获取
   * 此时 keyUrl 应指向 License Server 端点
   * expiresIn 为 0 或 undefined 表示永久密钥（M3U8 内嵌模式） */
  const isTemporaryKey = encryption.expiresIn !== undefined && encryption.expiresIn > 0;
  const keyUrl = (isTemporaryKey && licenseServer?.url) ? licenseServer.url : encryption.keyUrl;

  result.encryption = {
    keyUrl,
    iv: encryption.iv,
    keyFormat: encryption.keyFormat,
    keyFormatVersions: encryption.keyFormatVersions,
  };
}

/**
 * 获取 media 命名模式
 *
 * 优先使用显式指定的 media，否则从 initialization 自动推导。
 * 如果指定了 suffix，替换模式中的文件后缀（用于 HLS .ts 支持）。
 */
function resolveMediaPattern(segInfo: SegmentInfo): string | null {
  let pattern: string | null = segInfo.media ?? null;
  if (!pattern && segInfo.initialization) {
    pattern = deriveMediaPattern(segInfo.initialization);
  }
  if (pattern && segInfo.suffix) {
    pattern = applySuffix(pattern, segInfo.suffix);
  }
  return pattern;
}

/**
 * 将 * 通配符模板展开为具体 URL
 *
 * 优先使用 segmentTimeline 提供精确的每分片时长，
 * 其次用 totalCount + targetDuration 推算（最后一个分片时长取余数）。
 */
function expandSegmentTemplate(
  pattern: string,
  duration: number,
  targetDuration: number,
  startNumber: number,
  baseUrl?: string,
  totalCount?: number,
  segmentTimeline?: { t?: number; d: number; r?: number }[],
  timescale?: number,
): Segment[] {
  /* 从 segmentTimeline 计算精确的分片时长列表 */
  if (segmentTimeline && segmentTimeline.length > 0) {
    const scale = timescale ?? 1;
    const durations: number[] = [];
    for (const entry of segmentTimeline) {
      const repeat = entry.r !== undefined ? entry.r + 1 : 1;
      for (let j = 0; j < repeat; j++) {
        /* d 是 timescale 单位，转为秒 */
        durations.push(entry.d / scale);
      }
    }
    const count = totalCount ?? durations.length;
    const segments: Segment[] = [];
    for (let i = 0; i < count; i++) {
      const number = startNumber + i;
      /* 如果 timeline 提供的时长不够，用 targetDuration 补充 */
      const segDuration = i < durations.length ? durations[i] : targetDuration;
      const rawUrl = pattern.replace('*', String(number));
      segments.push({ duration: segDuration, url: resolveUrl(baseUrl, rawUrl) });
    }
    return segments;
  }

  /* 无 segmentTimeline 时，用 targetDuration 推算 */
  const count = totalCount ?? Math.ceil(duration / targetDuration);
  const segments: Segment[] = [];
  for (let i = 0; i < count; i++) {
    const number = startNumber + i;
    const isLast = i === count - 1;
    const segDuration = isLast
      ? duration - (count - 1) * targetDuration
      : targetDuration;
    const rawUrl = pattern.replace('*', String(number));
    segments.push({ duration: segDuration, url: resolveUrl(baseUrl, rawUrl) });
  }
  return segments;
}

/**
 * 将 SegmentInfo 转换为 ManifestPlaylistDetails
 *
 * @param segInfo - 分片信息
 * @param duration - 总时长（秒）
 * @param isLive - 是否为直播流
 */
function toPlaylistDetails(
  segInfo: SegmentInfo,
  duration: number,
  isLive: boolean,
  baseUrl?: string,
  encryption?: Aes128Encryption,
  licenseServer?: LicenseServer,
): ManifestPlaylistDetails | undefined {
  /* 单文件 + 字节范围（SegmentBase / mode='single'）：
     HLS 用 #EXT-X-BYTERANGE 表达 —— 每个分片都指向同一个文件、只是字节范围不同，
     init 段用 #EXT-X-MAP（同一个文件 + BYTERANGE）。与 _buildLevelDetails 里的
     setByteRange 是 hls.js 解析 #EXT-X-BYTERANGE 的同一个入口。
     注意：HLS 没有 sidx 概念，所以这条路**必须有显式 segments[]**；只有 indexRange 时
     无法枚举分片，返回 undefined（调用方就不会挂 playlistDetails）。 */
  if (segInfo.mode === 'single') {
    const file = baseUrl;
    const list = segInfo.segments ?? [];
    if (!file || list.length === 0) {
      return undefined;
    }

    const singleResult: ManifestPlaylistDetails = {
      targetDuration: segInfo.targetDuration ?? list[0].duration,
      live: isLive,
      mediaSequence: segInfo.mediaSequence,
      segments: list.map((seg) => ({
        duration: seg.duration,
        url: file,
        byteRange: toHlsByteRange(seg.byteRange),
      })),
    };

    if (segInfo.initialization) {
      singleResult.initSegmentUrl = file;
      singleResult.initSegmentRange = toHlsByteRange(segInfo.initialization);
    }

    applyEncryption(singleResult, encryption, licenseServer);
    return singleResult;
  }

  /* 获取分片列表 */
  let segments: Segment[];
  if (segInfo.segments && segInfo.segments.length > 0) {
    segments = segInfo.segments.map((seg) => ({
      ...seg,
      url: resolveUrl(baseUrl, seg.url),
    }));
  } else {
    const pattern = resolveMediaPattern(segInfo);
    if (pattern) {
      segments = expandSegmentTemplate(
        pattern,
        duration,
        segInfo.targetDuration ?? 4,
        segInfo.startNumber ?? 1,
        baseUrl,
        segInfo.totalCount,
        segInfo.segmentTimeline,
        segInfo.timescale,
      );
    } else {
      segments = [];
    }
  }

  /* 转换为 ManifestSegment 格式（带字节范围的段要一起带上，否则会被整文件当分片拉） */
  const manifestSegments: ManifestSegment[] = segments.map((seg) => {
    const item: ManifestSegment = {
      duration: seg.duration,
      url: seg.url,
    };
    const byteRange = toHlsByteRange(seg.byteRange);
    if (byteRange) item.byteRange = byteRange;
    return item;
  });

  const result: ManifestPlaylistDetails = {
    targetDuration: segInfo.targetDuration ?? 4,
    live: isLive,
    mediaSequence: segInfo.mediaSequence,
    segments: manifestSegments,
  };

  if (segInfo.initialization) {
    result.initSegmentUrl = resolveUrl(baseUrl, segInfo.initialization);
  }

  if (encryption) {
    applyEncryption(result, encryption, licenseServer);
  }

  return result;
}

/**
 * 将视频 MediaRepresentation 转换为 ManifestVariant
 */
function toManifestVariant(
  rep: MediaRepresentation,
  hasAudio: boolean,
  duration: number,
  isLive: boolean,
  encryption?: Aes128Encryption,
  licenseServer?: LicenseServer,
): ManifestVariant {
  const variant: ManifestVariant = {
    bandwidth: rep.bandwidth,
    /* single（单文件字节范围）模式下 baseUrl 是媒体文件本身，补斜杠会拼错地址 */
    url: rep.baseUrl ? (isSingleFileMode(rep.segmentInfo) ? rep.baseUrl : ensureTrailingSlash(rep.baseUrl)) : '',
    codecs: rep.codecs,
  };

  if (rep.width !== undefined && rep.height !== undefined) {
    variant.resolution = { width: rep.width, height: rep.height };
  }

  if (rep.frameRate !== undefined) {
    variant.frameRate = rep.frameRate;
  }

  if (hasAudio) {
    variant.audioGroupId = 'audio-group';
  }

  if (rep.segmentInfo) {
    const details = toPlaylistDetails(rep.segmentInfo, duration, isLive, rep.baseUrl, encryption, licenseServer);
    if (details) {
      variant.playlistDetails = details;
    }
  }

  return variant;
}

/**
 * 将音频 MediaRepresentation 转换为 ManifestAudioGroup
 */
function toManifestAudioGroup(
  rep: MediaRepresentation,
  index: number,
  duration: number,
  isLive: boolean,
  encryption?: Aes128Encryption,
  licenseServer?: LicenseServer,
): ManifestAudioGroup {
  const group: ManifestAudioGroup = {
    groupId: 'audio-group',
    /* 同 variant：single 模式下 baseUrl 是文件本身 */
    url: rep.baseUrl ? (isSingleFileMode(rep.segmentInfo) ? rep.baseUrl : ensureTrailingSlash(rep.baseUrl)) : '',
    codecs: rep.codecs,
    name: `Audio ${index + 1}`,
    default: index === 0,
    autoselect: index === 0,
  };

  if (rep.channelConfig !== undefined) {
    group.channels = String(rep.channelConfig.value);
  }

  if (rep.bandwidth !== undefined) {
    group.bandwidth = rep.bandwidth;
  }

  if (rep.segmentInfo) {
    const details = toPlaylistDetails(rep.segmentInfo, duration, isLive, rep.baseUrl, encryption, licenseServer);
    if (details) {
      group.playlistDetails = details;
    }
  }

  return group;
}

/**
 * 转换器输出格式
 */
export interface HlsManifestData {
  /** 变体流列表，传给 hls.loadManifest() */
  variants: ManifestVariant[];
  /** 音频轨道列表，传给 hls.loadManifest() */
  audioGroups: ManifestAudioGroup[];
}

/**
 * 将 MediaManifest 转换为 hls.js loadManifest 所需的格式
 *
 * @param manifest - 统一清单对象
 * @returns HlsManifestData，供 hls.loadManifest() 直接使用
 */
export function manifestToHls(manifest: MediaManifest): HlsManifestData {
  const isLive = manifest.live ?? false;
  const hasAudio = (manifest.audio?.length ?? 0) > 0;
  const duration = manifest.duration;
  const licenseServer = manifest.licenseServer;

  /* 视频 → ManifestVariant[]（一次性传入正确的 duration） */
  const variants: ManifestVariant[] = manifest.video.map((rep) =>
    toManifestVariant(rep, hasAudio, duration, isLive, rep.encryption ?? manifest.encryption, licenseServer),
  );

  /* 音频 → ManifestAudioGroup[]（一次性传入正确的 duration） */
  const audioGroups: ManifestAudioGroup[] = (manifest.audio ?? []).map((rep, index) =>
    toManifestAudioGroup(rep, index, duration, isLive, rep.encryption ?? manifest.encryption, licenseServer),
  );

  return { variants, audioGroups };
}
