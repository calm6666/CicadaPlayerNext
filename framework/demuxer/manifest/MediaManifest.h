//
// MediaManifest.h
//
// Unified media manifest object model for object-based playback.
//
// This is the C++ counterpart of hili-player's `MediaManifest` TypeScript type
// (front/hili-player/packages/plugins/src/vendor/types/manifest.ts). A single
// object describes a complete HLS/DASH-like stream set — representations,
// segment addressing (single file / template / explicit list), live config,
// AES-128 segment encryption, and DRM content protection (Widevine, FairPlay,
// PlayReady, ClearKey) — without any m3u8/mpd text.
//
// URL resolution rules (identical to the TS converters):
//   1. absolute http(s) URL  -> used as-is
//   2. URL starting with /   -> resolved against the CDN origin
//   3. relative URL          -> resolved against Representation baseUrl
//

#ifndef FRAMEWORK_DEMUXER_MANIFEST_MEDIA_MANIFEST_H
#define FRAMEWORK_DEMUXER_MANIFEST_MEDIA_MANIFEST_H

#include <cstdint>
#include <string>
#include <vector>

namespace Cicada {
namespace Manifest {

// ============================================================================
// DRM / content protection
// ============================================================================

struct ContentProtection {
    // e.g. "urn:mpeg:dash:mp4protection:2011",
    //      "urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed" (Widevine),
    //      "urn:uuid:9a04f079-9840-4286-ab92-e65be0885f95" (PlayReady),
    //      "urn:uuid:94ce86fb-07ff-4f43-adb8-93d2fa968ca2" (FairPlay),
    //      "urn:uuid:e2719d58-a985-b3c9-781a-b030af78d30e" (ClearKey)
    std::string schemeIdUri;
    std::string value;      // "cenc" / "cbcs" ...
    std::string pssh;       // base64 PSSH box
    std::string keyId;      // default key id (UUID)
    std::string laUrl;      // license acquisition URL
    std::string pro;        // PlayReady Object (base64)
};

struct LicenseServer {
    std::string url;
    std::string contentId;
    // "clearkey" | "aes128" | "widevine" | "fairplay" | "playready"
    std::string keyType = "clearkey";
};

struct Aes128Encryption {
    std::string keyUrl;
    std::string iv;             // hex string, 16 bytes; empty => sequence IV
    std::string keyFormat;
    std::string keyFormatVersions;
    int64_t expiresIn{0};       // 0 / unset => permanent key
};

// ============================================================================
// Segments
// ============================================================================

struct SegmentTimelineEntry {
    int64_t t{0};               // start time (timescale units)
    int64_t d{0};               // duration (timescale units)
    int64_t r{0};               // repeat count (excluding self); -1 = till end
    bool hasT{false};
    bool hasR{false};
};

struct Segment {
    double duration{0};         // seconds
    std::string url;
    std::string byteRange;      // "start-end"
};

enum class SegmentMode {
    Single,     // one file + byte ranges (SegmentBase)
    Template,   // URL template + timeline (SegmentTemplate)
    List,       // explicit segment list (SegmentList / HLS #EXTINF)
};

struct SegmentInfo {
    SegmentMode mode{SegmentMode::List};

    int64_t timescale{1};
    int64_t presentationTimeOffset{0};
    bool hasPresentationTimeOffset{false};

    std::string initialization; // single mode: byte range; otherwise URL
    std::string indexRange;     // single mode: sidx range

    double targetDuration{4};   // seconds
    std::string media;          // template with '*' placeholder
    int64_t startNumber{1};
    bool hasStartNumber{false};
    int64_t totalCount{0};
    bool hasTotalCount{false};
    std::string suffix;
    std::vector<SegmentTimelineEntry> segmentTimeline;
    std::vector<Segment> segments;

    int64_t mediaSequence{0};
    bool hasMediaSequence{false};
};

// ============================================================================
// Representations
// ============================================================================

struct AudioChannelConfig {
    int value{2};
    std::string schemeIdUri = "urn:mpeg:dash:23003:3:audio_channel_configuration:2011";
};

struct MediaRepresentation {
    std::string id;
    std::string baseUrl;
    std::vector<std::string> backupUrls;
    int64_t bandwidth{0};
    int64_t averageBandwidth{0};
    bool hasAverageBandwidth{false};
    std::string mimeType;
    std::string codecs;

    int width{0};
    int height{0};
    bool hasWidth{false};
    bool hasHeight{false};
    double frameRate{0};
    bool hasFrameRate{false};
    std::string sar;

    int audioSamplingRate{0};
    bool hasAudioSamplingRate{false};
    AudioChannelConfig channelConfig;
    bool hasChannelConfig{false};

    std::string lang;
    std::string role;
    std::string name;
    bool isDefault{false};
    bool autoSelect{false};

    bool hasSegmentInfo{false};
    SegmentInfo segmentInfo;

    Aes128Encryption encryption;
    bool hasEncryption{false};
};

struct SubtitleRepresentation {
    std::string id;
    std::string baseUrl;
    std::string mimeType;
    std::string codecs;
    std::string lang;
    std::string role = "subtitle";
    std::string name;
    bool isDefault{false};
    bool forced{false};
    std::string characteristics;
    bool hasSegmentInfo{false};
    SegmentInfo segmentInfo;
};

// ============================================================================
// Live / Periods / misc
// ============================================================================

struct ContentSteeringConfig {
    std::string serverUrl;
    std::string defaultCdnId;
    std::string defaultRedirectUrl;
    std::string proxyServerUrl;
};

struct LiveConfig {
    std::string type = "live";      // "live" | "event"
    double timeShiftBufferDepth{0};
    bool hasTimeShiftBufferDepth{false};
    double minimumUpdatePeriod{0};
    bool hasMinimumUpdatePeriod{false};
    std::string availabilityStartTime;
    std::string publishTime;
    double partTargetDuration{0};
    bool hasPartTargetDuration{false};
    bool canBlockReload{false};
    double canSkipUntil{0};
    bool hasCanSkipUntil{false};
    double holdBack{0};
    bool hasHoldBack{false};
    double partHoldBack{0};
    bool hasPartHoldBack{false};
};

struct Period {
    std::string id;
    double start{0};
    double duration{0};
    bool hasDuration{false};
    std::vector<MediaRepresentation> video;
    std::vector<MediaRepresentation> audio;
    std::vector<SubtitleRepresentation> subtitle;
    std::vector<ContentProtection> contentProtection;
};

struct UtcTiming {
    std::string schemeIdUri;
    std::string value;
};

struct EventStream {
    std::string schemeIdUri;
    std::string value;
    int64_t timescale{1};
};

struct Property {
    std::string schemeIdUri;
    std::string value;
};

// ============================================================================
// Top-level manifest
// ============================================================================

struct MediaManifest {
    ContentSteeringConfig contentSteering;
    bool hasContentSteering{false};

    double duration{0};            // seconds (required)
    double minBufferTime{1.5};
    double maxSegmentDuration{0};
    std::string title;

    std::vector<MediaRepresentation> video;
    std::vector<MediaRepresentation> audio;
    std::vector<SubtitleRepresentation> subtitle;

    std::vector<Period> periods;

    std::vector<ContentProtection> contentProtection;
    Aes128Encryption encryption;
    bool hasEncryption{false};
    LicenseServer licenseServer;
    bool hasLicenseServer{false};

    bool live{false};
    LiveConfig liveConfig;
    bool hasLiveConfig{false};

    double startTimeOffset{0};
    bool hasStartTimeOffset{false};
    bool startPrecise{false};

    // Preferred internal pipeline: "hls" (default, fMP4/TS + AES-128/DRM via
    // the HLS segment pipeline) or "dash" (DASH segment pipeline). The HLS
    // pipeline is the DRM-capable path (EXT-X-KEY / CENC via SegmentEncryption).
    std::string mediaSourceType = "hls";

    std::string location;
    double minimumUpdatePeriod{0};
    bool hasMinimumUpdatePeriod{false};

    std::vector<UtcTiming> utcTiming;
    std::vector<EventStream> eventStreams;
    std::vector<Property> supplementalProperties;
    std::vector<Property> essentialProperties;
};

} // namespace Manifest
} // namespace Cicada

#endif // FRAMEWORK_DEMUXER_MANIFEST_MEDIA_MANIFEST_H
