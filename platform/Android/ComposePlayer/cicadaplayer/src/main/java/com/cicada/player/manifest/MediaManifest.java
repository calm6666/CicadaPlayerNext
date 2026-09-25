package com.cicada.player.manifest;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

/**
 * 对象模式（MediaManifest）的**类型化模型**（Android SDK 层）。
 *
 * <p>与内核的 C++ 结构体和 JSON 解析器逐字对应：
 * <ul>
 *   <li>结构体：{@code framework/demuxer/manifest/MediaManifest.h}</li>
 *   <li>解析器：{@code framework/demuxer/manifest/MediaManifestParser.cpp}（键名 camelCase）</li>
 * </ul>
 *
 * <p><b>链路</b>：本类只负责把对象**序列化成 JSON 文本**；文本经
 * {@code CicadaPlayer.setDataSourceManifest(String)} → JNI → 内核，由内核用 **cJSON**
 * 解析成 {@code Manifest::MediaManifest}，再交给 {@code ManifestDemuxer::buildPlayList()}。
 * 应用层不做反序列化，也不需要理解内核结构。
 *
 * <p><b>必填</b>：{@link #duration} &gt; 0 且 {@link #video} 非空；否则内核解析失败并通过
 * {@code MEDIA_PLAYER_ERROR_DEMUXER_MANIFEST_PARSE}（0x20030007）回调报错。
 *
 * <p><b>序列化规则</b>：可选字段为 null/空/默认值时**不输出**（内核缺失时用同一个默认值），
 * 因此生成的 JSON 与手写 JSON 等价、且不会因为多写了默认值改变内核判定
 * （例如 {@code SegmentInfo.mode} 一旦输出就必须是 {@code single}/{@code template}/{@code list}）。
 *
 * <p><b>本类未建模的字段</b>（内核支持、需要时请直接用原始 JSON 走 setDataSourceManifest）：
 * 顶层 {@code utcTiming} / {@code eventStreams} / {@code supplementalProperties} /
 * {@code essentialProperties} / {@code periods[].contentProtection[].pro} 之外的扩展字段。
 */
public class MediaManifest {

    // ------------------------------------------------------------------ 必填
    /** 媒体总时长（秒）。必填，必须 &gt; 0。 */
    public double duration;
    /** 视频档位（必填，至少一个）。 */
    public List<Representation> video = new ArrayList<>();
    /** 音频档位（可选）。 */
    public List<Representation> audio = new ArrayList<>();

    // ------------------------------------------------------------------ 可选
    public List<Subtitle> subtitle = new ArrayList<>();
    public List<Period> periods = new ArrayList<>();
    public List<ContentProtection> contentProtection = new ArrayList<>();
    public ContentSteering contentSteering;
    public Encryption encryption;
    public LicenseServer licenseServer;
    public boolean live;
    public LiveConfig liveConfig;
    /** null = 不输出；内核默认 1.5 秒。 */
    public Double minBufferTime;
    public Double maxSegmentDuration;
    public String title;
    public Double startTimeOffset;
    public Boolean startPrecise;
    /** "hls"（缺省，DRM 走 HLS 分片管线）或 "dash"。 */
    public String mediaSourceType;
    public String location;
    public Double minimumUpdatePeriod;

    // ================================================================== 子结构

    public static class ChannelConfig {
        public int value = 2;
        public String schemeIdUri;

        JSONObject toJson() {
            JSONObject o = new JSONObject();
            put(o, "value", value);
            put(o, "schemeIdUri", schemeIdUri);
            return o;
        }
    }

    public static class ContentProtection {
        public String schemeIdUri;
        public String value;
        public String pssh;
        public String keyId;
        public String laUrl;
        public String pro;

        JSONObject toJson() {
            JSONObject o = new JSONObject();
            put(o, "schemeIdUri", schemeIdUri);
            put(o, "value", value);
            put(o, "pssh", pssh);
            put(o, "keyId", keyId);
            put(o, "laUrl", laUrl);
            put(o, "pro", pro);
            return o;
        }
    }

    public static class LicenseServer {
        public String url;
        public String contentId;
        /** "clearkey"（缺省）| "aes128" | "widevine" | "fairplay" | "playready"。 */
        public String keyType;

        JSONObject toJson() {
            JSONObject o = new JSONObject();
            put(o, "url", url);
            put(o, "contentId", contentId);
            put(o, "keyType", keyType);
            return o;
        }
    }

    /** AES-128 分片加密（HLS EXT-X-KEY 等价物）。 */
    public static class Encryption {
        public String keyUrl;
        public String iv;
        public String keyFormat;
        public String keyFormatVersions;
        public long expiresIn;

        JSONObject toJson() {
            JSONObject o = new JSONObject();
            put(o, "keyUrl", keyUrl);
            put(o, "iv", iv);
            put(o, "keyFormat", keyFormat);
            put(o, "keyFormatVersions", keyFormatVersions);
            if (expiresIn > 0) {
                put(o, "expiresIn", expiresIn);
            }
            return o;
        }
    }

    public static class Segment {
        public double duration;
        public String url;
        public String byteRange;

        JSONObject toJson() {
            JSONObject o = new JSONObject();
            if (duration > 0) {
                put(o, "duration", duration);
            }
            put(o, "url", url);
            put(o, "byteRange", byteRange);
            return o;
        }
    }

    public static class SegmentTimelineEntry {
        public Long t;
        public long d;
        public Long r;

        JSONObject toJson() {
            JSONObject o = new JSONObject();
            put(o, "t", t);
            put(o, "d", d);
            put(o, "r", r);
            return o;
        }
    }

    /**
     * 分片描述。三种模式（与内核 {@code parseSegmentMode} 接受的字符串完全一致）：
     * <ul>
     *   <li>{@code "single"}：一个文件 + byteRange（SegmentBase）</li>
     *   <li>{@code "template"}：URL 模板（{@link #media} 含 {@code *}）+ 可选 timeline</li>
     *   <li>{@code "list"}：显式分片列表（{@link #segments}）</li>
     * </ul>
     */
    public static class SegmentInfo {
        public String mode;
        public long timescale = 1;
        public Long presentationTimeOffset;
        public String initialization;
        public String indexRange;
        public double targetDuration = 4;
        public String media;
        public Long startNumber;
        public Long totalCount;
        public String suffix;
        public List<SegmentTimelineEntry> segmentTimeline = new ArrayList<>();
        public List<Segment> segments = new ArrayList<>();
        public Long mediaSequence;

        JSONObject toJson() {
            JSONObject o = new JSONObject();
            put(o, "mode", mode);
            if (timescale != 1) {
                put(o, "timescale", timescale);
            }
            put(o, "presentationTimeOffset", presentationTimeOffset);
            put(o, "initialization", initialization);
            put(o, "indexRange", indexRange);
            if (targetDuration != 4) {
                put(o, "targetDuration", targetDuration);
            }
            put(o, "media", media);
            put(o, "startNumber", startNumber);
            put(o, "totalCount", totalCount);
            put(o, "suffix", suffix);
            putArray(o, "segmentTimeline", segmentTimeline);
            putArray(o, "segments", segments);
            put(o, "mediaSequence", mediaSequence);
            return o;
        }
    }

    public static class Representation {
        public String id;
        public String baseUrl;
        public List<String> backupUrls = new ArrayList<>();
        public long bandwidth;
        public Long averageBandwidth;
        public String mimeType;
        public String codecs;
        public Integer width;
        public Integer height;
        public Double frameRate;
        public String sar;
        public Integer audioSamplingRate;
        public ChannelConfig channelConfig;
        public String lang;
        public String role;
        public String name;
        public Boolean isDefault;
        public Boolean autoSelect;
        public SegmentInfo segmentInfo;
        public Encryption encryption;

        JSONObject toJson() {
            JSONObject o = new JSONObject();
            put(o, "id", id);
            put(o, "baseUrl", baseUrl);
            if (backupUrls != null && !backupUrls.isEmpty()) {
                JSONArray arr = new JSONArray();
                for (String u : backupUrls) {
                    arr.put(u);
                }
                putJson(o, "backupUrls", arr);
            }
            if (bandwidth > 0) {
                put(o, "bandwidth", bandwidth);
            }
            put(o, "averageBandwidth", averageBandwidth);
            put(o, "mimeType", mimeType);
            put(o, "codecs", codecs);
            put(o, "width", width);
            put(o, "height", height);
            put(o, "frameRate", frameRate);
            put(o, "sar", sar);
            put(o, "audioSamplingRate", audioSamplingRate);
            if (channelConfig != null) {
                putJson(o, "channelConfig", channelConfig.toJson());
            }
            put(o, "lang", lang);
            put(o, "role", role);
            put(o, "name", name);
            put(o, "isDefault", isDefault);
            put(o, "autoSelect", autoSelect);
            if (segmentInfo != null) {
                putJson(o, "segmentInfo", segmentInfo.toJson());
            }
            if (encryption != null) {
                putJson(o, "encryption", encryption.toJson());
            }
            return o;
        }
    }

    public static class Subtitle {
        public String id;
        public String baseUrl;
        public String mimeType;
        public String codecs;
        public String lang;
        public String role;
        public String name;
        public Boolean isDefault;
        public Boolean forced;
        public String characteristics;
        public SegmentInfo segmentInfo;

        JSONObject toJson() {
            JSONObject o = new JSONObject();
            put(o, "id", id);
            put(o, "baseUrl", baseUrl);
            put(o, "mimeType", mimeType);
            put(o, "codecs", codecs);
            put(o, "lang", lang);
            put(o, "role", role);
            put(o, "name", name);
            put(o, "isDefault", isDefault);
            put(o, "forced", forced);
            put(o, "characteristics", characteristics);
            if (segmentInfo != null) {
                putJson(o, "segmentInfo", segmentInfo.toJson());
            }
            return o;
        }
    }

    public static class LiveConfig {
        /** "live"（缺省）或 "event"。 */
        public String type;
        public Double timeShiftBufferDepth;
        public Double minimumUpdatePeriod;
        public String availabilityStartTime;
        public String publishTime;
        public Double partTargetDuration;
        public Boolean canBlockReload;
        public Double canSkipUntil;
        public Double holdBack;
        public Double partHoldBack;

        JSONObject toJson() {
            JSONObject o = new JSONObject();
            put(o, "type", type);
            put(o, "timeShiftBufferDepth", timeShiftBufferDepth);
            put(o, "minimumUpdatePeriod", minimumUpdatePeriod);
            put(o, "availabilityStartTime", availabilityStartTime);
            put(o, "publishTime", publishTime);
            put(o, "partTargetDuration", partTargetDuration);
            put(o, "canBlockReload", canBlockReload);
            put(o, "canSkipUntil", canSkipUntil);
            put(o, "holdBack", holdBack);
            put(o, "partHoldBack", partHoldBack);
            return o;
        }
    }

    public static class Period {
        public String id;
        public double start;
        public Double duration;
        public List<Representation> video = new ArrayList<>();
        public List<Representation> audio = new ArrayList<>();
        public List<Subtitle> subtitle = new ArrayList<>();
        public List<ContentProtection> contentProtection = new ArrayList<>();

        JSONObject toJson() {
            JSONObject o = new JSONObject();
            put(o, "id", id);
            if (start > 0) {
                put(o, "start", start);
            }
            put(o, "duration", duration);
            putArray(o, "video", video);
            putArray(o, "audio", audio);
            putArray(o, "subtitle", subtitle);
            putArray(o, "contentProtection", contentProtection);
            return o;
        }
    }

    public static class ContentSteering {
        public String serverUrl;
        public String defaultCdnId;
        public String defaultRedirectUrl;
        public String proxyServerUrl;

        JSONObject toJson() {
            JSONObject o = new JSONObject();
            put(o, "serverUrl", serverUrl);
            put(o, "defaultCdnId", defaultCdnId);
            put(o, "defaultRedirectUrl", defaultRedirectUrl);
            put(o, "proxyServerUrl", proxyServerUrl);
            return o;
        }
    }

    // ================================================================== 序列化

    /** 生成内核期望的 JSON 对象（camelCase，与 MediaManifestParser.cpp 的键名一致）。 */
    public JSONObject toJson() {
        JSONObject o = new JSONObject();
        put(o, "duration", duration);
        putArray(o, "video", video);
        putArray(o, "audio", audio);
        putArray(o, "subtitle", subtitle);
        putArray(o, "periods", periods);
        putArray(o, "contentProtection", contentProtection);
        if (contentSteering != null) {
            putJson(o, "contentSteering", contentSteering.toJson());
        }
        if (encryption != null) {
            putJson(o, "encryption", encryption.toJson());
        }
        if (licenseServer != null) {
            putJson(o, "licenseServer", licenseServer.toJson());
        }
        put(o, "live", live);
        if (liveConfig != null) {
            putJson(o, "liveConfig", liveConfig.toJson());
        }
        put(o, "minBufferTime", minBufferTime);
        put(o, "maxSegmentDuration", maxSegmentDuration);
        put(o, "title", title);
        put(o, "startTimeOffset", startTimeOffset);
        put(o, "startPrecise", startPrecise);
        put(o, "mediaSourceType", mediaSourceType);
        put(o, "location", location);
        put(o, "minimumUpdatePeriod", minimumUpdatePeriod);
        return o;
    }

    /** 直接交给 {@code CicadaPlayer.setDataSourceManifest(String)} 的文本。 */
    public String toJsonString() {
        return toJson().toString();
    }

    // ------------------------------------------------------------- 小工具

    static void put(JSONObject o, String key, String value) {
        if (value == null || value.isEmpty()) {
            return;
        }
        try {
            o.put(key, value);
        } catch (Exception ignored) {
        }
    }

    static void put(JSONObject o, String key, Double value) {
        if (value == null) {
            return;
        }
        try {
            o.put(key, value.doubleValue());
        } catch (Exception ignored) {
        }
    }

    static void put(JSONObject o, String key, Integer value) {
        if (value == null) {
            return;
        }
        try {
            o.put(key, value.intValue());
        } catch (Exception ignored) {
        }
    }

    static void put(JSONObject o, String key, Long value) {
        if (value == null) {
            return;
        }
        try {
            o.put(key, value.longValue());
        } catch (Exception ignored) {
        }
    }

    static void put(JSONObject o, String key, Boolean value) {
        if (value == null) {
            return;
        }
        try {
            o.put(key, value.booleanValue());
        } catch (Exception ignored) {
        }
    }

    static void put(JSONObject o, String key, long value) {
        try {
            o.put(key, value);
        } catch (Exception ignored) {
        }
    }

    static void put(JSONObject o, String key, boolean value) {
        try {
            o.put(key, value);
        } catch (Exception ignored) {
        }
    }

    /*
     * 嵌套 JSON 的统一写法：**对象和数组都走 putJson**（两个显式重载，避免依赖重载推导）。
     * 加数组重载的原因：JSONObject 与 JSONArray 之间没有继承关系，
     * 原来只有 JSONObject 重载时，putJson(o, "backupUrls", arr) 里的 JSONArray 无法转换 —— Android
     * 编译期报"不兼容的类型: JSONArray 无法转换为 JSONObject"。这是唯一一处类型错误。
     */
    static void putJson(JSONObject o, String key, JSONObject value) {
        try {
            o.put(key, value);
        } catch (Exception ignored) {
        }
    }

    static void putJson(JSONObject o, String key, JSONArray value) {
        try {
            o.put(key, value);
        } catch (Exception ignored) {
        }
    }

    static void putArray(JSONObject o, String key, List<? extends Object> list) {
        if (list == null || list.isEmpty()) {
            return;
        }
        JSONArray arr = new JSONArray();
        for (Object item : list) {
            try {
                if (item instanceof String) {
                    arr.put(item);
                } else if (item instanceof Representation) {
                    arr.put(((Representation) item).toJson());
                } else if (item instanceof Subtitle) {
                    arr.put(((Subtitle) item).toJson());
                } else if (item instanceof Period) {
                    arr.put(((Period) item).toJson());
                } else if (item instanceof ContentProtection) {
                    arr.put(((ContentProtection) item).toJson());
                } else if (item instanceof Segment) {
                    arr.put(((Segment) item).toJson());
                } else if (item instanceof SegmentTimelineEntry) {
                    arr.put(((SegmentTimelineEntry) item).toJson());
                }
            } catch (Exception ignored) {
            }
        }
        /* 与上面的嵌套写法统一：数组也交给 putJson(JSONArray) 重载。 */
        putJson(o, key, arr);
    }
}
