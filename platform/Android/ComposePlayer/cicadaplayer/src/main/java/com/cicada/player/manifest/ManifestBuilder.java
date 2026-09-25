package com.cicada.player.manifest;

import java.util.ArrayList;
import java.util.List;

/**
 * MediaManifest 的**链式构造器**（Android SDK 层，additive）。
 *
 * <p>用法（Java / Kotlin 均可，Kotlin 见 docs/MANIFEST-OBJECT-GUIDE.md）：
 * <pre>{@code
 * String json = new ManifestBuilder()
 *         .duration(120)
 *         .mediaSourceType("hls")
 *         .addVideo(new ManifestBuilder.RepresentationBuilder("v0", "https://cdn/v/")
 *                 .bandwidth(2_000_000).codecs("avc1.64001f").width(1920).height(1080)
 *                 .segmentInfo(new ManifestBuilder.SegmentInfoBuilder("template")
 *                         .initialization("init.mp4").media("seg-*.m4s").startNumber(1)))
 *         .addAudio(new ManifestBuilder.RepresentationBuilder("a0", "https://cdn/a/")
 *                 .bandwidth(128_000).codecs("mp4a.40.2").lang("zh"))
 *         .build()
 *         .toJsonString();
 * player.setDataSourceManifest(json);        // 老接口，语义不变
 * // 或 player.setDataSource(new JSONObject(json))（setDataSource(JSONObject) 会 toString 后走同一条路）
 * }</pre>
 *
 * <p><b>链路</b>：对象 → JSON 文本 → {@code setDataSourceManifest(String)} → JNI →
 * 内核 cJSON 解析 → {@code ManifestDemuxer}。构造器**不做**任何网络请求、不解析，
 * 只是拼 JSON；因此它不改变既有 {@code setDataSource(String)} 的语义。
 */
public class ManifestBuilder {

    private final MediaManifest manifest = new MediaManifest();

    /** 媒体总时长（秒），必填且必须 &gt; 0。 */
    public ManifestBuilder duration(double seconds) {
        manifest.duration = seconds;
        return this;
    }

    /** 缺省 "hls"；传 "dash" 走 DASH 分片管线。 */
    public ManifestBuilder mediaSourceType(String type) {
        manifest.mediaSourceType = type;
        return this;
    }

    public ManifestBuilder minBufferTime(double seconds) {
        manifest.minBufferTime = seconds;
        return this;
    }

    public ManifestBuilder maxSegmentDuration(double seconds) {
        manifest.maxSegmentDuration = seconds;
        return this;
    }

    public ManifestBuilder title(String title) {
        manifest.title = title;
        return this;
    }

    public ManifestBuilder live(boolean live) {
        manifest.live = live;
        return this;
    }

    public ManifestBuilder liveConfig(MediaManifest.LiveConfig config) {
        manifest.liveConfig = config;
        return this;
    }

    public ManifestBuilder startTimeOffset(double seconds) {
        manifest.startTimeOffset = seconds;
        return this;
    }

    public ManifestBuilder startPrecise(boolean precise) {
        manifest.startPrecise = precise;
        return this;
    }

    public ManifestBuilder location(String location) {
        manifest.location = location;
        return this;
    }

    public ManifestBuilder minimumUpdatePeriod(double seconds) {
        manifest.minimumUpdatePeriod = seconds;
        return this;
    }

    public ManifestBuilder addVideo(MediaManifest.Representation representation) {
        if (representation != null) {
            manifest.video.add(representation);
        }
        return this;
    }

    public ManifestBuilder addAudio(MediaManifest.Representation representation) {
        if (representation != null) {
            manifest.audio.add(representation);
        }
        return this;
    }

    public ManifestBuilder addSubtitle(MediaManifest.Subtitle subtitle) {
        if (subtitle != null) {
            manifest.subtitle.add(subtitle);
        }
        return this;
    }

    public ManifestBuilder addPeriod(MediaManifest.Period period) {
        if (period != null) {
            manifest.periods.add(period);
        }
        return this;
    }

    public ManifestBuilder addContentProtection(MediaManifest.ContentProtection protection) {
        if (protection != null) {
            manifest.contentProtection.add(protection);
        }
        return this;
    }

    /** AES-128 分片加密（HLS EXT-X-KEY 等价物）。 */
    public ManifestBuilder encryption(MediaManifest.Encryption encryption) {
        manifest.encryption = encryption;
        return this;
    }

    public ManifestBuilder licenseServer(MediaManifest.LicenseServer server) {
        manifest.licenseServer = server;
        return this;
    }

    public ManifestBuilder contentSteering(MediaManifest.ContentSteering steering) {
        manifest.contentSteering = steering;
        return this;
    }

    public MediaManifest build() {
        return manifest;
    }

    /** 直接拿到可下发的 JSON 文本（等价于 {@code build().toJsonString()}）。 */
    public String toJsonString() {
        return manifest.toJsonString();
    }

    // ================================================================== 子构造器

    /** 视频/音频档位构造器。 */
    public static class RepresentationBuilder {
        private final MediaManifest.Representation r = new MediaManifest.Representation();

        public RepresentationBuilder(String id, String baseUrl) {
            r.id = id;
            r.baseUrl = baseUrl;
        }

        public RepresentationBuilder bandwidth(long bps) {
            r.bandwidth = bps;
            return this;
        }

        public RepresentationBuilder averageBandwidth(long bps) {
            r.averageBandwidth = bps;
            return this;
        }

        public RepresentationBuilder mimeType(String mimeType) {
            r.mimeType = mimeType;
            return this;
        }

        public RepresentationBuilder codecs(String codecs) {
            r.codecs = codecs;
            return this;
        }

        public RepresentationBuilder width(int width) {
            r.width = width;
            return this;
        }

        public RepresentationBuilder height(int height) {
            r.height = height;
            return this;
        }

        public RepresentationBuilder frameRate(double fps) {
            r.frameRate = fps;
            return this;
        }

        public RepresentationBuilder sar(String sar) {
            r.sar = sar;
            return this;
        }

        public RepresentationBuilder audioSamplingRate(int hz) {
            r.audioSamplingRate = hz;
            return this;
        }

        public RepresentationBuilder channelConfig(int value) {
            MediaManifest.ChannelConfig c = new MediaManifest.ChannelConfig();
            c.value = value;
            r.channelConfig = c;
            return this;
        }

        public RepresentationBuilder lang(String lang) {
            r.lang = lang;
            return this;
        }

        public RepresentationBuilder role(String role) {
            r.role = role;
            return this;
        }

        public RepresentationBuilder name(String name) {
            r.name = name;
            return this;
        }

        public RepresentationBuilder isDefault(boolean isDefault) {
            r.isDefault = isDefault;
            return this;
        }

        public RepresentationBuilder autoSelect(boolean autoSelect) {
            r.autoSelect = autoSelect;
            return this;
        }

        public RepresentationBuilder backupUrls(List<String> urls) {
            if (urls != null) {
                r.backupUrls = new ArrayList<>(urls);
            }
            return this;
        }

        public RepresentationBuilder segmentInfo(MediaManifest.SegmentInfo segmentInfo) {
            r.segmentInfo = segmentInfo;
            return this;
        }

        public RepresentationBuilder encryption(MediaManifest.Encryption encryption) {
            r.encryption = encryption;
            return this;
        }

        public MediaManifest.Representation build() {
            return r;
        }
    }

    /**
     * 分片描述构造器。mode 只接受与内核 {@code parseSegmentMode} 一致的三个字符串：
     * {@code "single"} / {@code "template"} / {@code "list"}。
     */
    public static class SegmentInfoBuilder {
        private final MediaManifest.SegmentInfo s = new MediaManifest.SegmentInfo();

        public SegmentInfoBuilder(String mode) {
            s.mode = mode;
        }

        public SegmentInfoBuilder timescale(long timescale) {
            s.timescale = timescale;
            return this;
        }

        public SegmentInfoBuilder presentationTimeOffset(long us) {
            s.presentationTimeOffset = us;
            return this;
        }

        /** single 模式：byte range；其余模式：初始化分片 URL。 */
        public SegmentInfoBuilder initialization(String initialization) {
            s.initialization = initialization;
            return this;
        }

        public SegmentInfoBuilder indexRange(String indexRange) {
            s.indexRange = indexRange;
            return this;
        }

        public SegmentInfoBuilder targetDuration(double seconds) {
            s.targetDuration = seconds;
            return this;
        }

        /** template 模式的 URL 模板，用 {@code *} 占位分片序号。 */
        public SegmentInfoBuilder media(String media) {
            s.media = media;
            return this;
        }

        public SegmentInfoBuilder startNumber(long startNumber) {
            s.startNumber = startNumber;
            return this;
        }

        public SegmentInfoBuilder totalCount(long totalCount) {
            s.totalCount = totalCount;
            return this;
        }

        public SegmentInfoBuilder suffix(String suffix) {
            s.suffix = suffix;
            return this;
        }

        public SegmentInfoBuilder mediaSequence(long mediaSequence) {
            s.mediaSequence = mediaSequence;
            return this;
        }

        public SegmentInfoBuilder addTimelineEntry(Long t, long d, Long r) {
            MediaManifest.SegmentTimelineEntry e = new MediaManifest.SegmentTimelineEntry();
            e.t = t;
            e.d = d;
            e.r = r;
            s.segmentTimeline.add(e);
            return this;
        }

        public SegmentInfoBuilder addSegment(double duration, String url, String byteRange) {
            MediaManifest.Segment seg = new MediaManifest.Segment();
            seg.duration = duration;
            seg.url = url;
            seg.byteRange = byteRange;
            s.segments.add(seg);
            return this;
        }

        public MediaManifest.SegmentInfo build() {
            return s;
        }
    }

    /** AES-128 加密构造器。 */
    public static class EncryptionBuilder {
        private final MediaManifest.Encryption e = new MediaManifest.Encryption();

        public EncryptionBuilder keyUrl(String keyUrl) {
            e.keyUrl = keyUrl;
            return this;
        }

        public EncryptionBuilder iv(String ivHex) {
            e.iv = ivHex;
            return this;
        }

        public EncryptionBuilder keyFormat(String keyFormat) {
            e.keyFormat = keyFormat;
            return this;
        }

        public EncryptionBuilder keyFormatVersions(String versions) {
            e.keyFormatVersions = versions;
            return this;
        }

        public EncryptionBuilder expiresIn(long seconds) {
            e.expiresIn = seconds;
            return this;
        }

        public MediaManifest.Encryption build() {
            return e;
        }
    }

    /** DRM 授权服务器构造器。 */
    public static class LicenseServerBuilder {
        private final MediaManifest.LicenseServer l = new MediaManifest.LicenseServer();

        public LicenseServerBuilder url(String url) {
            l.url = url;
            return this;
        }

        public LicenseServerBuilder contentId(String contentId) {
            l.contentId = contentId;
            return this;
        }

        public LicenseServerBuilder keyType(String keyType) {
            l.keyType = keyType;
            return this;
        }

        public MediaManifest.LicenseServer build() {
            return l;
        }
    }
}
