/**
 * MediaManifest → dash.js 内部对象转换器
 *
 * 将统一的 MediaManifest 对象一次性转换为 dash.js 的 attachSource()
 * 所需的 manifest 对象格式，零网络请求。
 *
 * 支持的分片模式：
 * 1. SegmentTemplate + duration：固定时长模板（分片多时推荐，省略 segments）
 * 2. SegmentTemplate + SegmentTimeline：可变时长模板
 * 3. SegmentList：显式分片 URL 列表（分片少时推荐）
 * 4. SegmentBase：单文件字节范围模式
 *
 * 核心原则：
 * - 一次性转换，不进行多次中间转换
 * - 对象模式下 dash.js 不走 XML 解析，DurationMatcher/NumericMatcher 不执行
 * - ObjectIron 不执行属性继承，子元素必须包含完整属性
 * - 属性名不带 _asArray 后缀（dash.js 内部访问 Period/AdaptationSet/Representation）
 * - BaseURL 必须是数组格式
 * - AudioChannelConfiguration 必须是数组格式
 * - 零 MPD 文本，所有 URL 直接指向 .m4s 文件
 */

import type {
  DashManifestObject,
  DashAdaptationSet,
  DashRepresentation,
  DashSegmentTemplate,
  DashSegmentList,
  DashSegmentUrl,
  DashSegmentBase,
  DashAudioChannelConfiguration,
  DashContentProtection,
} from './types/index';
import type { MediaManifest, MediaRepresentation, SegmentInfo, Segment, LicenseServer } from './types/index';
import { resolveUrl, deriveMediaPattern } from './utils/index';

/**
 * 确保 URL 以 / 结尾
 *
 * dash.js 的 BaseURL 和 hls.js 的 variant.url 会与分片 URL 拼接，
 * 因此必须以 / 结尾。用户传入的 baseUrl/backupUrls 不应包含尾部 /，
 * 由转换器统一补全。
 */
function ensureTrailingSlash(url: string): string {
  return url.endsWith('/') ? url : url + '/';
}

/**
 * 获取 media 命名模式
 *
 * 优先使用显式指定的 media，否则从 initialization 自动推导。
 */
function resolveMediaPattern(segInfo: SegmentInfo): string | null {
  if (segInfo.media) return segInfo.media;
  if (segInfo.initialization) return deriveMediaPattern(segInfo.initialization);
  return null;
}

/**
 * 将 * 通配符模板转换为 DASH SegmentTemplate 的 media 字符串
 *
 * 统一对象使用 * 通配符表示分片序号位置：
 * - "0-*.m4s"   → "0-$Number$.m4s"
 * - "seg-*.m4s" → "seg-$Number$.m4s"
 *
 * DASH SegmentTemplate 使用 $Number$ 作为分片序号占位符
 */
function toDashMediaTemplate(pattern: string): string {
  return pattern.replace('*', '$Number$');
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
        durations.push(entry.d / scale);
      }
    }
    const count = totalCount ?? durations.length;
    const segments: Segment[] = [];
    for (let i = 0; i < count; i++) {
      const number = startNumber + i;
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
 * 获取完整的分片列表
 *
 * 优先使用显式 segments，其次从 media 模式展开
 */
function resolveSegments(segInfo: SegmentInfo, duration: number, baseUrl?: string): Segment[] {
  if (segInfo.segments && segInfo.segments.length > 0) {
    return segInfo.segments.map((seg) => ({
      ...seg,
      url: resolveUrl(baseUrl, seg.url),
    }));
  }
  const pattern = resolveMediaPattern(segInfo);
  if (pattern) {
    return expandSegmentTemplate(
      pattern,
      duration,
      segInfo.targetDuration ?? 4,
      segInfo.startNumber ?? 1,
      baseUrl,
      segInfo.totalCount,
      segInfo.segmentTimeline,
      segInfo.timescale,
    );
  }
  return [];
}

/**
 * 将 SegmentInfo 转换为 DASH SegmentTemplate
 *
 * 使用固定 duration + endNumber 模式（与 MPD 文件一致）：
 * - duration：固定分片时长（timescale 单位），dash.js 用 index * duration 计算分片位置
 * - endNumber：最后一个分片编号，防止快进到末尾时请求超出范围的分片
 *
 * 不使用 SegmentTimeline 的原因：
 * dash.js 在对象模式下处理 SegmentTemplate+SegmentTimeline 时，
 * TimelineSegmentsGetter 的分片定位逻辑不如 TemplateSegmentsGetter 可靠，
 * 可能导致快进时请求 init segment 或分片位置计算错误。
 * 固定 duration + endNumber 是 MPD 文件的标准写法，dash.js 完全支持。
 */
function toSegmentTemplate(
  segInfo: SegmentInfo,
  duration: number,
  baseUrl?: string,
): DashSegmentTemplate | undefined {
  if (segInfo.mode === 'single') return undefined;
  /* 有显式 segments 时用 SegmentList，不用 SegmentTemplate */
  if (segInfo.segments && segInfo.segments.length > 0) return undefined;
  /* 获取 media 命名模式（显式指定或从 initialization 自动推导） */
  const pattern = resolveMediaPattern(segInfo);
  if (!pattern) return undefined;

  const timescale = segInfo.timescale ?? 1;
  const startNumber = segInfo.startNumber ?? 1;

  /* 固定分片时长（timescale 单位）
   * dash.js 用 segmentDuration = duration / timescale 计算每个分片的边界
   *
   * 重要：不能用 targetDuration（最大分片时长）！
   * 实际分片时长通常不均匀（如 12.5s 和 8.333s 交替），
   * 用 targetDuration 会导致 dash.js 计算的分片位置严重偏移。
   * 正确做法：用 总时长 / 分片数 计算平均分片时长，
   * 这样 24 × 平均时长 ≈ 总时长，与 mediaPresentationDuration 一致。
   * MPD 文件中也是这样计算的（如 duration=10000000 → 10s，24×10=240≈235.5） */
  const segmentDuration = segInfo.totalCount
    ? Math.round(duration / segInfo.totalCount * timescale)
    : Math.round((segInfo.targetDuration ?? 4) * timescale);

  const template: DashSegmentTemplate = {
    timescale,
    duration: segmentDuration,
    initialization: segInfo.initialization ? resolveUrl(baseUrl, segInfo.initialization) : undefined,
    media: resolveUrl(baseUrl, toDashMediaTemplate(pattern)),
    startNumber,
  };

  /* endNumber：最后一个分片的编号
   * dash.js 用此字段确定分片范围边界，防止快进到末尾时
   * 请求超出范围的分片（如把 init segment -0.m4s 当作 media segment 请求） */
  if (segInfo.totalCount !== undefined) {
    template.endNumber = startNumber + segInfo.totalCount - 1;
  }

  return template;
}

/**
 * 将 SegmentInfo 转换为 DASH SegmentList（显式分片 URL 列表）
 *
 * 仅在 mode='multi' 且有显式 segments 列表时生成。
 *
 * 重要：不使用 SegmentTimeline！
 * dash.js 的 TimelineSegmentsGetter._onSegmentFound 中存在 bug：
 * _getMediaUrl(segmentBase, segmentURL, sElementCounter) 使用 sElementCounter
 * （S 元素索引）而非 sElementCounterIncludingRepeats（绝对分片索引）来查找 URL，
 * 导致同一 S 元素下的所有分片返回相同 URL，画面重复。
 *
 * 因此使用 ListSegmentsGetter（无 SegmentTimeline）+ duration 字段：
 * - URL 从 SegmentURL[index].media 获取（index 正确）
 * - 时间从 (startNumber + index - 1) * segmentDuration 计算
 * - segmentDuration = duration / timescale
 * - 最后一个分片的时长可能不精确，但 SourceBuffer 使用 m4s 实际 PTS 定位，
 *   不依赖 dash.js 计算的时长，因此不影响播放
 */
function toSegmentList(
  segInfo: SegmentInfo,
  duration: number,
  baseUrl?: string,
): DashSegmentList | undefined {
  if (segInfo.mode === 'single') return undefined;

  /* 获取分片列表：优先显式 segments，其次展开模板 */
  const segments = resolveSegments(segInfo, duration, baseUrl);
  if (segments.length === 0) return undefined;

  /* 有 media 模式或能自动推导 media 时用 SegmentTemplate，不用 SegmentList
   * 条件：无显式 segments 列表 + 有媒体命名模式（显式 media 或可从 initialization 推导）
   * 如果同时设置 SegmentTemplate 和 SegmentList，dash.js 优先使用 SegmentList，
   * 导致 SegmentTemplate 的 duration/endNumber 被忽略 */
  const pattern = resolveMediaPattern(segInfo);
  if (pattern && !segInfo.segments?.length) return undefined;

  const segmentUrls: DashSegmentUrl[] = segments.map((seg) => ({
    media: seg.url,
  }));

  const timescale = segInfo.timescale ?? 1;
  /* 固定分片时长（timescale 单位）
   * 必须用 总时长/分片数 计算平均值，不能用 targetDuration！
   * targetDuration 是最大分片时长（如 12.5s），实际分片交替 12.5s/8.333s，
   * 用 targetDuration 会导致 dash.js 计算的分片位置超出 mediaPresentationDuration，
   * 快进到末尾时请求不到后续分片（如 24 个分片只加载到第 19 个） */
  const segCount = segInfo.totalCount ?? segInfo.segments?.length;
  const segmentDuration = segCount
    ? Math.round(duration / segCount * timescale)
    : Math.round((segInfo.targetDuration ?? 4) * timescale);
  const result: DashSegmentList = {
    timescale,
    duration: segmentDuration,
    SegmentURL: segmentUrls,
  };

  if (segInfo.initialization) {
    result.Initialization = { sourceURL: resolveUrl(baseUrl, segInfo.initialization) };
  }

  if (segInfo.startNumber !== undefined) {
    result.startNumber = segInfo.startNumber;
  }

  return result;
}

/**
 * 将 SegmentInfo 转换为 DASH SegmentBase
 *
 * 仅处理 mode='single' 的情况。
 */
function toSegmentBase(segInfo: SegmentInfo): DashSegmentBase | undefined {
  if (segInfo.mode !== 'single') return undefined;

  const result: DashSegmentBase = {};

  if (segInfo.indexRange) {
    result.indexRange = segInfo.indexRange;
  }

  if (segInfo.initialization) {
    result.Initialization = { range: segInfo.initialization };
  }

  return result;
}

/**
 * 将 MediaRepresentation 转换为 DASH Representation
 *
 * 注意：ObjectIron 不执行属性继承，Representation 必须包含完整属性。
 *
 * 注意：encryption（AES-128）是 HLS 专用的分片加密方式，DASH 不使用此字段。
 * DASH 通过 contentProtection 字段（ContentProtection 元素）实现 DRM 保护，
 * 已在 MediaManifest.contentProtection 中支持。
 */
function toDashRepresentation(rep: MediaRepresentation, duration: number): DashRepresentation {
  /* BaseURL 数组：主 URL + 备用 URL
   * dash.js 遇到多个 BaseURL 时自动故障切换
   * 备用 URL 带 serviceLocation 属性用于分组
   * baseUrl 未提供时不设置 BaseURL
   *
   * dash.js 的 BaseURL 会与分片 URL 拼接，因此必须以 / 结尾。
   * 用户传入的 baseUrl/backupUrls 不应包含尾部 /，由转换器自动补全。 */
  const baseURLs: string[] = [];
  if (rep.baseUrl) {
    baseURLs.push(ensureTrailingSlash(rep.baseUrl));
  }
  if (rep.backupUrls && rep.backupUrls.length > 0) {
    baseURLs.push(...rep.backupUrls.map(ensureTrailingSlash));
  }

  const representation: DashRepresentation = {
    id: String(rep.id),
    bandwidth: rep.bandwidth,
    /* ObjectIron 不执行继承，必须显式提供 mimeType 和 codecs */
    mimeType: rep.mimeType,
    codecs: rep.codecs,
  };

  /* BaseURL 必须是数组格式，有值时才设置 */
  if (baseURLs.length > 0) {
    representation.BaseURL = baseURLs;
  }

  /* 视频特有字段 */
  if (rep.width !== undefined) representation.width = rep.width;
  if (rep.height !== undefined) representation.height = rep.height;
  if (rep.frameRate !== undefined) representation.frameRate = rep.frameRate;
  if (rep.sar !== undefined) representation.sar = rep.sar;

  /* 音频特有字段 */
  if (rep.audioSamplingRate !== undefined) {
    representation.audioSamplingRate = rep.audioSamplingRate;
  }
  if (rep.channelConfig !== undefined) {
    /* AudioChannelConfiguration 必须是数组格式 */
    const audioChannelConfig: DashAudioChannelConfiguration = {
      schemeIdUri: rep.channelConfig.schemeIdUri ?? 'urn:mpeg:dash:23003:3:audio_channel_configuration:2011',
      value: String(rep.channelConfig.value),
    };
    representation.AudioChannelConfiguration = [audioChannelConfig];
  }

  /* 分片信息：按优先级选择 SegmentTemplate / SegmentList / SegmentBase */
  if (rep.segmentInfo) {
    const segmentTemplate = toSegmentTemplate(rep.segmentInfo, duration, rep.baseUrl);
    if (segmentTemplate) {
      representation.SegmentTemplate = segmentTemplate;
    }

    const segmentList = toSegmentList(rep.segmentInfo, duration, rep.baseUrl);
    if (segmentList) {
      representation.SegmentList = segmentList;
    }

    const segmentBase = toSegmentBase(rep.segmentInfo);
    if (segmentBase) {
      representation.SegmentBase = segmentBase;
    }
  }

  return representation;
}

/**
 * 根据 LicenseServer 配置生成 ClearKey ContentProtection 元素列表
 *
 * ClearKey ContentProtection 映射：
 * - urn:mpeg:dash:mp4protection:2011 value="cenc" — CENC 通用加密声明
 * - urn:uuid:e2719d58-a985-b3c9-781a-b030af78d30e — ClearKey 方案
 *
 * dash.js 通过 schemeIdUri 识别 DRM 系统，
 * ClearKey 的 UUID 为 e2719d58-a985-b3c9-781a-b030af78d30e
 */
function buildClearKeyContentProtection(licenseServer: LicenseServer): DashContentProtection[] {
  const protections: DashContentProtection[] = [
    /* CENC 通用加密声明 */
    {
      schemeIdUri: 'urn:mpeg:dash:mp4protection:2011',
      value: 'cenc',
    },
    /* ClearKey 方案 */
    {
      schemeIdUri: 'urn:uuid:e2719d58-a985-b3c9-781a-b030af78d30e',
    },
  ];

  /* 如果有 contentId，设置为默认密钥 ID */
  if (licenseServer.contentId) {
    protections[1]['cenc:defaultKId'] = licenseServer.contentId;
  }

  return protections;
}

/**
 * 将 MediaManifest 转换为 dash.js 的 manifest 对象
 *
 * 转换规则：
 * - video[] → AdaptationSet(contentType=video) + Representation[]
 * - audio[] → AdaptationSet(contentType=audio) + Representation[]
 * - 整体 → Period + ManifestObject
 *
 * @param manifest - 统一清单对象
 * @returns DashManifestObject，供 dash.js attachSource() 使用
 */
export function manifestToDash(manifest: MediaManifest): DashManifestObject {
  const adaptationSets: DashAdaptationSet[] = [];

  /* 根据 licenseServer 构建 ClearKey ContentProtection */
  const contentProtection = manifest.licenseServer
    ? buildClearKeyContentProtection(manifest.licenseServer)
    : undefined;

  /* 视频 AdaptationSet：每个 Representation 单独一个 AdaptationSet
   * 与 MPD 原始格式一致，避免 dash.js 内部处理差异。
   * MPD 中每个 Representation 在独立的 AdaptationSet 中，
   * 如果把多个 Representation 放在同一个 AdaptationSet 中，
   * dash.js 的 processAdaptation 会按 bandwidth 排序 Representation，
   * 可能导致 _getSegmentBase 查找 SegmentTemplate 时索引不匹配，
   * 以及 BufferController 在 switchStream 时 timestampOffset 处理异常，
   * 造成 0-4 秒画面重叠。*/
  for (let i = 0; i < manifest.video.length; i++) {
    const rep = manifest.video[i];
    const adaptationSet: DashAdaptationSet = {
      id: String(adaptationSets.length),
      contentType: 'video',
      mimeType: 'video/mp4',
      startWithSAP: 1,
      segmentAlignment: true,
      bitstreamSwitching: true,
      Representation: [toDashRepresentation(rep, manifest.duration)],
    };
    if (contentProtection) {
      adaptationSet.ContentProtection = contentProtection;
    }
    adaptationSets.push(adaptationSet);
  }

  /* 音频 AdaptationSet */
  if (manifest.audio && manifest.audio.length > 0) {
    for (let i = 0; i < manifest.audio.length; i++) {
      const rep = manifest.audio[i];
      const adaptationSet: DashAdaptationSet = {
        id: String(adaptationSets.length),
        contentType: 'audio',
        mimeType: 'audio/mp4',
        startWithSAP: 1,
        segmentAlignment: true,
        bitstreamSwitching: true,
        lang: rep.lang ?? 'und',
        Representation: [toDashRepresentation(rep, manifest.duration)],
      };
      if (rep.role) {
        adaptationSet.Role = [{ schemeIdUri: 'urn:mpeg:dash:role:2011', value: rep.role }];
      }
      if (contentProtection) {
        adaptationSet.ContentProtection = contentProtection;
      }
      adaptationSets.push(adaptationSet);
    }
  }

  return {
    type: manifest.live ? 'dynamic' : 'static',
    /* protocol 必须设为 'DASH'，dash.js 内部依赖此字段 */
    protocol: 'DASH',
    /* loadedTime 必须为 Date 对象，dash.js _update 中调用 .getTime() */
    loadedTime: new Date(),
    /* 对象模式下 dash.js 不走 XML 解析，数值字段必须直接传数字 */
    mediaPresentationDuration: manifest.duration,
    minBufferTime: manifest.minBufferTime ?? 1.5,
    Period: [
      {
        id: '0',
        /* start 必须是数字（秒），不能是 ISO 8601 字符串 */
        start: 0,
        /* Period 时长（秒） */
        duration: manifest.duration,
        AdaptationSet: adaptationSets,
      },
    ],
  };
}
