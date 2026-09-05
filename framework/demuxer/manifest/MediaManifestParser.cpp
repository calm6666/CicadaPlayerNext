//
// MediaManifestParser.cpp
//
// JSON parsing for the unified MediaManifest object model. Field names are the
// camelCase names from hili-player's manifest.ts. All URL resolution happens
// later in ManifestDemuxer (single conversion point), so the parser stores
// values verbatim.
//

#include "MediaManifestParser.h"
#include <utils/CicadaJSON.h>
#include <utils/frame_work_log.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define LOG_TAG "MediaManifestParser"

using namespace Cicada::Manifest;

namespace {

    // ---- small helpers ------------------------------------------------------

    static bool parseSegmentMode(const std::string &value, SegmentMode &mode)
    {
        if (value == "single") {
            mode = SegmentMode::Single;
        } else if (value == "template") {
            mode = SegmentMode::Template;
        } else if (value == "list") {
            mode = SegmentMode::List;
        } else {
            return false;
        }
        return true;
    }

    static void parseContentProtection(const CicadaJSONItem &item, ContentProtection &out)
    {
        out.schemeIdUri = item.getString("schemeIdUri", "");
        out.value = item.getString("value", "");
        out.pssh = item.getString("pssh", "");
        out.keyId = item.getString("keyId", "");
        out.laUrl = item.getString("laUrl", "");
        out.pro = item.getString("pro", "");
    }

    static void parseLicenseServer(const CicadaJSONItem &item, LicenseServer &out)
    {
        out.url = item.getString("url", "");
        out.contentId = item.getString("contentId", "");
        out.keyType = item.getString("keyType", "clearkey");
    }

    static void parseEncryption(const CicadaJSONItem &item, Aes128Encryption &out)
    {
        out.keyUrl = item.getString("keyUrl", "");
        out.iv = item.getString("iv", "");
        out.keyFormat = item.getString("keyFormat", "");
        out.keyFormatVersions = item.getString("keyFormatVersions", "");
        out.expiresIn = item.getInt64("expiresIn", 0);
    }

    static void parseSegmentTimeline(const CicadaJSONItem &item, std::vector<SegmentTimelineEntry> &out)
    {
        CicadaJSONArray timeline = item.getArray("segmentTimeline");
        if (!timeline.isValid()) {
            return;
        }
        for (int i = 0; i < timeline.getSize(); ++i) {
            CicadaJSONItem &entry = timeline.getItem(i);
            SegmentTimelineEntry e{};
            e.d = entry.getInt64("d", 0);
            if (entry.hasItem("t")) {
                e.t = entry.getInt64("t", 0);
                e.hasT = true;
            }
            if (entry.hasItem("r")) {
                e.r = entry.getInt64("r", 0);
                e.hasR = true;
            }
            out.push_back(e);
        }
    }

    static void parseSegments(const CicadaJSONItem &item, std::vector<Segment> &out)
    {
        CicadaJSONArray segments = item.getArray("segments");
        if (!segments.isValid()) {
            return;
        }
        for (int i = 0; i < segments.getSize(); ++i) {
            CicadaJSONItem &seg = segments.getItem(i);
            Segment s{};
            s.duration = seg.getDouble("duration", 0);
            s.url = seg.getString("url", "");
            s.byteRange = seg.getString("byteRange", "");
            out.push_back(s);
        }
    }

    static void parseSegmentInfo(const CicadaJSONItem &item, SegmentInfo &out)
    {
        std::string modeStr = item.getString("mode", "");
        if (!modeStr.empty()) {
            parseSegmentMode(modeStr, out.mode);
        }
        out.timescale = item.getInt64("timescale", 1);
        if (item.hasItem("presentationTimeOffset")) {
            out.presentationTimeOffset = item.getInt64("presentationTimeOffset", 0);
            out.hasPresentationTimeOffset = true;
        }
        out.initialization = item.getString("initialization", "");
        out.indexRange = item.getString("indexRange", "");
        out.targetDuration = item.getDouble("targetDuration", 4);
        out.media = item.getString("media", "");
        if (item.hasItem("startNumber")) {
            out.startNumber = item.getInt64("startNumber", 1);
            out.hasStartNumber = true;
        }
        if (item.hasItem("totalCount")) {
            out.totalCount = item.getInt64("totalCount", 0);
            out.hasTotalCount = true;
        }
        out.suffix = item.getString("suffix", "");
        parseSegmentTimeline(item, out.segmentTimeline);
        parseSegments(item, out.segments);
        if (item.hasItem("mediaSequence")) {
            out.mediaSequence = item.getInt64("mediaSequence", 0);
            out.hasMediaSequence = true;
        }
    }

    static void parseRepresentation(const CicadaJSONItem &item, MediaRepresentation &out)
    {
        out.id = item.getString("id", "");
        out.baseUrl = item.getString("baseUrl", "");
        if (item.hasItem("backupUrls")) {
            CicadaJSONArray urls = item.getArray("backupUrls");
            if (urls.isValid()) {
                for (int i = 0; i < urls.getSize(); ++i) {
                    // Array elements are string-typed nodes: read their value
                    // directly (not a child by key).
                    out.backupUrls.push_back(urls.getItem(i).getStringValue(""));
                }
            }
        }
        out.bandwidth = item.getInt64("bandwidth", 0);
        if (item.hasItem("averageBandwidth")) {
            out.averageBandwidth = item.getInt64("averageBandwidth", 0);
            out.hasAverageBandwidth = true;
        }
        out.mimeType = item.getString("mimeType", "");
        out.codecs = item.getString("codecs", "");
        if (item.hasItem("width")) {
            out.width = item.getInt("width", 0);
            out.hasWidth = true;
        }
        if (item.hasItem("height")) {
            out.height = item.getInt("height", 0);
            out.hasHeight = true;
        }
        if (item.hasItem("frameRate")) {
            out.frameRate = item.getDouble("frameRate", 0);
            out.hasFrameRate = true;
        }
        out.sar = item.getString("sar", "");
        if (item.hasItem("audioSamplingRate")) {
            out.audioSamplingRate = item.getInt("audioSamplingRate", 0);
            out.hasAudioSamplingRate = true;
        }
        if (item.hasItem("channelConfig")) {
            CicadaJSONItem channel = item.getItem("channelConfig");
            if (channel.isValid()) {
                out.channelConfig.value = channel.getInt("value", 2);
                out.channelConfig.schemeIdUri =
                        channel.getString("schemeIdUri", "urn:mpeg:dash:23003:3:audio_channel_configuration:2011");
                out.hasChannelConfig = true;
            }
        }
        out.lang = item.getString("lang", "");
        out.role = item.getString("role", "");
        out.name = item.getString("name", "");
        out.isDefault = item.getBool("isDefault", false);
        out.autoSelect = item.getBool("autoSelect", false);
        if (item.hasItem("segmentInfo")) {
            CicadaJSONItem segInfo = item.getItem("segmentInfo");
            if (segInfo.isValid()) {
                parseSegmentInfo(segInfo, out.segmentInfo);
                out.hasSegmentInfo = true;
            }
        }
        if (item.hasItem("encryption")) {
            CicadaJSONItem enc = item.getItem("encryption");
            if (enc.isValid()) {
                parseEncryption(enc, out.encryption);
                out.hasEncryption = true;
            }
        }
    }

    static void parseSubtitle(const CicadaJSONItem &item, SubtitleRepresentation &out)
    {
        out.id = item.getString("id", "");
        out.baseUrl = item.getString("baseUrl", "");
        out.mimeType = item.getString("mimeType", "");
        out.codecs = item.getString("codecs", "");
        out.lang = item.getString("lang", "");
        out.role = item.getString("role", "subtitle");
        out.name = item.getString("name", "");
        out.isDefault = item.getBool("isDefault", false);
        out.forced = item.getBool("forced", false);
        out.characteristics = item.getString("characteristics", "");
        if (item.hasItem("segmentInfo")) {
            CicadaJSONItem segInfo = item.getItem("segmentInfo");
            if (segInfo.isValid()) {
                parseSegmentInfo(segInfo, out.segmentInfo);
                out.hasSegmentInfo = true;
            }
        }
    }

    static void parseContentSteering(const CicadaJSONItem &item, ContentSteeringConfig &out)
    {
        out.serverUrl = item.getString("serverUrl", "");
        out.defaultCdnId = item.getString("defaultCdnId", "");
        out.defaultRedirectUrl = item.getString("defaultRedirectUrl", "");
        out.proxyServerUrl = item.getString("proxyServerUrl", "");
    }

    static void parseLiveConfig(const CicadaJSONItem &item, LiveConfig &out)
    {
        out.type = item.getString("type", "live");
        if (item.hasItem("timeShiftBufferDepth")) {
            out.timeShiftBufferDepth = item.getDouble("timeShiftBufferDepth", 0);
            out.hasTimeShiftBufferDepth = true;
        }
        if (item.hasItem("minimumUpdatePeriod")) {
            out.minimumUpdatePeriod = item.getDouble("minimumUpdatePeriod", 0);
            out.hasMinimumUpdatePeriod = true;
        }
        out.availabilityStartTime = item.getString("availabilityStartTime", "");
        out.publishTime = item.getString("publishTime", "");
        if (item.hasItem("partTargetDuration")) {
            out.partTargetDuration = item.getDouble("partTargetDuration", 0);
            out.hasPartTargetDuration = true;
        }
        out.canBlockReload = item.getBool("canBlockReload", false);
        if (item.hasItem("canSkipUntil")) {
            out.canSkipUntil = item.getDouble("canSkipUntil", 0);
            out.hasCanSkipUntil = true;
        }
        if (item.hasItem("holdBack")) {
            out.holdBack = item.getDouble("holdBack", 0);
            out.hasHoldBack = true;
        }
        if (item.hasItem("partHoldBack")) {
            out.partHoldBack = item.getDouble("partHoldBack", 0);
            out.hasPartHoldBack = true;
        }
    }

    static void parsePeriod(const CicadaJSONItem &item, Period &out)
    {
        out.id = item.getString("id", "");
        out.start = item.getDouble("start", 0);
        if (item.hasItem("duration")) {
            out.duration = item.getDouble("duration", 0);
            out.hasDuration = true;
        }
        CicadaJSONArray video = item.getArray("video");
        if (video.isValid()) {
            for (int i = 0; i < video.getSize(); ++i) {
                MediaRepresentation rep{};
                parseRepresentation(video.getItem(i), rep);
                out.video.push_back(rep);
            }
        }
        CicadaJSONArray audio = item.getArray("audio");
        if (audio.isValid()) {
            for (int i = 0; i < audio.getSize(); ++i) {
                MediaRepresentation rep{};
                parseRepresentation(audio.getItem(i), rep);
                out.audio.push_back(rep);
            }
        }
        CicadaJSONArray subtitle = item.getArray("subtitle");
        if (subtitle.isValid()) {
            for (int i = 0; i < subtitle.getSize(); ++i) {
                SubtitleRepresentation rep{};
                parseSubtitle(subtitle.getItem(i), rep);
                out.subtitle.push_back(rep);
            }
        }
        CicadaJSONArray protections = item.getArray("contentProtection");
        if (protections.isValid()) {
            for (int i = 0; i < protections.getSize(); ++i) {
                ContentProtection cp{};
                parseContentProtection(protections.getItem(i), cp);
                out.contentProtection.push_back(cp);
            }
        }
    }

    static void parseUtcTiming(const CicadaJSONItem &item, UtcTiming &out)
    {
        out.schemeIdUri = item.getString("schemeIdUri", "");
        out.value = item.getString("value", "");
    }

    static void parseEventStream(const CicadaJSONItem &item, EventStream &out)
    {
        out.schemeIdUri = item.getString("schemeIdUri", "");
        out.value = item.getString("value", "");
        out.timescale = item.getInt64("timescale", 1);
    }

    static void parseProperty(const CicadaJSONItem &item, Property &out)
    {
        out.schemeIdUri = item.getString("schemeIdUri", "");
        out.value = item.getString("value", "");
    }

    template<typename T>
    static void parseArrayOf(const CicadaJSONItem &root, const char *name, std::vector<T> &out,
                             void (*parse)(const CicadaJSONItem &, T &))
    {
        CicadaJSONArray array = root.getArray(name);
        if (!array.isValid()) {
            return;
        }
        for (int i = 0; i < array.getSize(); ++i) {
            T value{};
            parse(array.getItem(i), value);
            out.push_back(value);
        }
    }

} // anonymous namespace

namespace Cicada {
namespace Manifest {

    bool MediaManifestParser::parse(const std::string &json, MediaManifest &manifest, std::string &error)
    {
        CicadaJSONItem root(json);
        if (!root.isValid()) {
            error = "invalid JSON document";
            return false;
        }

        manifest = MediaManifest{};

        if (root.hasItem("contentSteering")) {
            CicadaJSONItem steering = root.getItem("contentSteering");
            if (steering.isValid()) {
                parseContentSteering(steering, manifest.contentSteering);
                manifest.hasContentSteering = true;
            }
        }

        manifest.duration = root.getDouble("duration", 0);
        manifest.minBufferTime = root.getDouble("minBufferTime", 1.5);
        manifest.maxSegmentDuration = root.getDouble("maxSegmentDuration", 0);
        manifest.title = root.getString("title", "");

        parseArrayOf<MediaRepresentation>(root, "video", manifest.video, parseRepresentation);
        parseArrayOf<MediaRepresentation>(root, "audio", manifest.audio, parseRepresentation);
        parseArrayOf<SubtitleRepresentation>(root, "subtitle", manifest.subtitle, parseSubtitle);
        parseArrayOf<Period>(root, "periods", manifest.periods, parsePeriod);
        parseArrayOf<ContentProtection>(root, "contentProtection", manifest.contentProtection, parseContentProtection);

        if (root.hasItem("encryption")) {
            CicadaJSONItem enc = root.getItem("encryption");
            if (enc.isValid()) {
                parseEncryption(enc, manifest.encryption);
                manifest.hasEncryption = true;
            }
        }
        if (root.hasItem("licenseServer")) {
            CicadaJSONItem license = root.getItem("licenseServer");
            if (license.isValid()) {
                parseLicenseServer(license, manifest.licenseServer);
                manifest.hasLicenseServer = true;
            }
        }

        manifest.live = root.getBool("live", false);
        if (root.hasItem("liveConfig")) {
            CicadaJSONItem liveCfg = root.getItem("liveConfig");
            if (liveCfg.isValid()) {
                parseLiveConfig(liveCfg, manifest.liveConfig);
                manifest.hasLiveConfig = true;
            }
        }

        if (root.hasItem("startTimeOffset")) {
            manifest.startTimeOffset = root.getDouble("startTimeOffset", 0);
            manifest.hasStartTimeOffset = true;
        }
        manifest.startPrecise = root.getBool("startPrecise", false);
        manifest.mediaSourceType = root.getString("mediaSourceType", "hls");
        manifest.location = root.getString("location", "");
        if (root.hasItem("minimumUpdatePeriod")) {
            manifest.minimumUpdatePeriod = root.getDouble("minimumUpdatePeriod", 0);
            manifest.hasMinimumUpdatePeriod = true;
        }

        parseArrayOf<UtcTiming>(root, "utcTiming", manifest.utcTiming, parseUtcTiming);
        parseArrayOf<EventStream>(root, "eventStreams", manifest.eventStreams, parseEventStream);
        parseArrayOf<Property>(root, "supplementalProperties", manifest.supplementalProperties, parseProperty);
        parseArrayOf<Property>(root, "essentialProperties", manifest.essentialProperties, parseProperty);

        return validate(manifest, error);
    }

    bool MediaManifestParser::validate(const MediaManifest &manifest, std::string &error)
    {
        if (manifest.periods.empty() && manifest.video.empty()) {
            error = "manifest has no video representations (video[] or periods[] required)";
            return false;
        }
        if (manifest.duration <= 0 && !manifest.live) {
            error = "manifest duration must be > 0 for VOD (or set live=true)";
            return false;
        }
        return true;
    }

    std::string MediaManifestParser::toJson(const MediaManifest &manifest)
    {
        // Debug/tooling serialization; hand-built to keep the dependency surface
        // small. Not used on the playback hot path.
        std::string json = "{";
        char buf[64]{};
        snprintf(buf, sizeof(buf), "\"duration\":%.3f", manifest.duration);
        json += buf;
        if (manifest.live) {
            json += ",\"live\":true";
        }
        if (!manifest.mediaSourceType.empty()) {
            json += ",\"mediaSourceType\":\"" + manifest.mediaSourceType + "\"";
        }
        json += ",\"video\":[";
        for (size_t i = 0; i < manifest.video.size(); ++i) {
            if (i > 0) {
                json += ",";
            }
            const MediaRepresentation &rep = manifest.video[i];
            json += "{";
            snprintf(buf, sizeof(buf), "\"id\":\"%s\",\"bandwidth\":%lld", rep.id.c_str(),
                     (long long) rep.bandwidth);
            json += buf;
            if (rep.hasWidth && rep.hasHeight) {
                snprintf(buf, sizeof(buf), ",\"width\":%d,\"height\":%d", rep.width, rep.height);
                json += buf;
            }
            json += "}";
        }
        json += "],\"audio\":[";
        for (size_t i = 0; i < manifest.audio.size(); ++i) {
            if (i > 0) {
                json += ",";
            }
            const MediaRepresentation &rep = manifest.audio[i];
            json += "{";
            snprintf(buf, sizeof(buf), "\"id\":\"%s\",\"bandwidth\":%lld", rep.id.c_str(),
                     (long long) rep.bandwidth);
            json += buf;
            json += "}";
        }
        json += "]}";
        return json;
    }

} // namespace Manifest
} // namespace Cicada
