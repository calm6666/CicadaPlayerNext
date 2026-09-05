//
// ManifestDemuxer.cpp
//
// See ManifestDemuxer.h for the design. The conversion mirrors
// manifest-to-hls.ts / manifest-to-dash.ts semantics:
//   - '*' placeholder in media patterns -> segment numbers
//   - URL resolution: absolute/root URL kept, otherwise baseUrl + '/'
//   - segmentTimeline (timescale units) preferred over targetDuration math
//   - init segment -> segment::init_section
//   - AES-128 / DRM -> SegmentEncryption on every segment
//

#define LOG_TAG "ManifestDemuxer"

#include "ManifestDemuxer.h"
#include "MediaManifestParser.h"
#include <demuxer/play_list/HLSManager.h>
#include <demuxer/play_list/PlaylistManager.h>
#include <demuxer/play_list/AdaptationSet.h>
#include <demuxer/play_list/Period.h>
#include <demuxer/play_list/Representation.h>
#include <demuxer/play_list/SegmentList.h>
#include <demuxer/play_list/playList.h>
#include <demuxer/play_list/segment.h>
#include <demuxer/play_list/segment_decrypt/SegmentEncryption.h>
#include <data_source/proxyDataSource.h>
#include <utils/frame_work_log.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#define CLOCK_FREQ_US (1000000LL)

using namespace Cicada::Manifest;

namespace Cicada {

    ManifestDemuxer::ManifestDemuxer(std::unique_ptr<MediaManifest> manifest)
        : IDemuxer(""), mManifest(std::move(manifest))
    {
        mName = "manifestDemuxer";
    }

    ManifestDemuxer::~ManifestDemuxer()
    {
        Close();
        delete mProxySource;
    }

    // ------------------------------------------------------------ URL helpers --

    std::string ManifestDemuxer::resolveUrl(const std::string &baseUrl, const std::string &relativeUrl)
    {
        if (relativeUrl.empty()) {
            return relativeUrl;
        }
        if (relativeUrl.compare(0, 7, "http://") == 0 || relativeUrl.compare(0, 8, "https://") == 0
                || relativeUrl[0] == '/') {
            return relativeUrl;
        }
        if (baseUrl.empty()) {
            return relativeUrl;
        }
        if (baseUrl.back() == '/') {
            return baseUrl + relativeUrl;
        }
        return baseUrl + '/' + relativeUrl;
    }

    std::string ManifestDemuxer::deriveMediaPattern(const std::string &initialization)
    {
        // "0-0.m4s" -> "0-*.m4s", "1-0.m4s" -> "1-*.m4s"
        size_t dot = initialization.rfind('.');
        size_t dash = initialization.rfind('-', dot == std::string::npos ? initialization.size() : dot);
        if (dot == std::string::npos || dash == std::string::npos || dash == 0) {
            return std::string();
        }
        return initialization.substr(0, dash + 1) + "*" + initialization.substr(dot);
    }

    std::vector<Segment> ManifestDemuxer::expandTemplate(const SegmentInfo &info,
                                                         double totalDuration, const std::string &baseUrl)
    {
        std::vector<Segment> segments;
        const double scale = info.timescale > 0 ? info.timescale : 1;
        const int64_t startNumber = info.startNumber;
        std::string pattern = info.media;
        if (pattern.empty() && !info.initialization.empty()) {
            pattern = deriveMediaPattern(info.initialization);
        }
        if (pattern.empty()) {
            return segments;
        }
        if (!info.suffix.empty()) {
            size_t dot = pattern.rfind('.');
            if (dot != std::string::npos) {
                pattern = pattern.substr(0, dot + 1) + info.suffix;
            }
        }

        // Prefer exact durations from segmentTimeline.
        std::vector<double> durations;
        if (!info.segmentTimeline.empty()) {
            for (const SegmentTimelineEntry &entry : info.segmentTimeline) {
                int64_t repeat = entry.hasR ? (entry.r >= 0 ? entry.r + 1 : 0) : 1;
                if (entry.r < 0) {
                    // r=-1: repeats to the end (live); approximated with targetDuration
                    repeat = 0;
                }
                for (int64_t j = 0; j < repeat; ++j) {
                    durations.push_back(entry.d / scale);
                }
            }
        }

        const int64_t count = info.totalCount > 0 ? info.totalCount
                              : static_cast<int64_t>(std::ceil(totalDuration / info.targetDuration));
        for (int64_t i = 0; i < count; ++i) {
            Segment seg{};
            seg.duration = i < static_cast<int64_t>(durations.size()) ? durations[i] : info.targetDuration;
            const std::string number = std::to_string(startNumber + i);
            size_t pos = pattern.find('*');
            std::string rawUrl = pattern;
            if (pos != std::string::npos) {
                rawUrl.replace(pos, 1, number);
            }
            seg.url = resolveUrl(baseUrl, rawUrl);
            segments.push_back(seg);
        }
        return segments;
    }

    // --------------------------------------------------------- DRM / encryption --

    void ManifestDemuxer::buildSegmentEncryptions(const MediaManifest &manifest,
                                                  const MediaRepresentation &rep,
                                                  std::vector<SegmentEncryption> &out)
    {
        const Aes128Encryption *aes = rep.hasEncryption ? &rep.encryption
                                       : (manifest.hasEncryption ? &manifest.encryption : nullptr);
        if (aes != nullptr && !aes->keyUrl.empty()) {
            SegmentEncryption enc{};
            enc.method = SegmentEncryption::AES_128;
            // expiresIn > 0 => temporary key fetched from the license server
            enc.keyUrl = (aes->expiresIn > 0 && manifest.hasLicenseServer && !manifest.licenseServer.url.empty())
                         ? manifest.licenseServer.url : aes->keyUrl;
            if (!aes->iv.empty()) {
                // hex string -> bytes
                for (size_t i = 0; i + 1 < aes->iv.size() && i < 31; i += 2) {
                    char byteStr[3] = {aes->iv[i], aes->iv[i + 1], 0};
                    enc.iv.push_back(static_cast<uint8_t>(strtoul(byteStr, nullptr, 16)));
                }
                enc.ivStatic = true;
            }
            if (!aes->keyFormat.empty()) {
                enc.keyFormat = aes->keyFormat;
            }
            out.push_back(enc);
            return;
        }

        // CENC DRM (Widevine / FairPlay / PlayReady / ClearKey) via ContentProtection.
        // The DRM-capable pipeline uses method AES_SAMPLE + keyFormat (schemeIdUri)
        // + keyUrl (license URL): HLSStream marks the stream DRM-protected and the
        // decoder chain obtains a DrmHandler from the DRM framework.
        const std::vector<ContentProtection> *protections = &manifest.contentProtection;
        for (const ContentProtection &cp : *protections) {
            if (cp.schemeIdUri.empty() || cp.schemeIdUri == "urn:mpeg:dash:mp4protection:2011") {
                continue;
            }
            SegmentEncryption enc{};
            enc.method = SegmentEncryption::AES_SAMPLE;
            enc.keyFormat = cp.schemeIdUri;
            enc.pssh = cp.pssh;
            enc.keyId = cp.keyId;
            if (!cp.laUrl.empty()) {
                enc.keyUrl = cp.laUrl;
            } else if (manifest.hasLicenseServer && !manifest.licenseServer.url.empty()) {
                enc.keyUrl = manifest.licenseServer.url;
            }
            out.push_back(enc);
            return; // first supported DRM system wins
        }
    }

    // --------------------------------------------------------- playList building --

    playList *ManifestDemuxer::buildPlayList()
    {
        auto *playlist = new playList();
        playlist->setDuration(static_cast<int64_t>(mManifest->duration * CLOCK_FREQ_US));
        playlist->type = mManifest->live ? "dynamic" : "static";
        playlist->minBufferTime = static_cast<int64_t>(mManifest->minBufferTime * CLOCK_FREQ_US);
        if (mManifest->hasMinimumUpdatePeriod) {
            playlist->minUpdatePeriod = static_cast<int64_t>(mManifest->minimumUpdatePeriod * CLOCK_FREQ_US);
        }
        if (mManifest->hasLiveConfig && mManifest->liveConfig.hasTimeShiftBufferDepth) {
            playlist->timeShiftBufferDepth =
                    static_cast<int64_t>(mManifest->liveConfig.timeShiftBufferDepth * CLOCK_FREQ_US);
        }

        // One Period for the whole manifest (multi-period handled by the HLS
        // pipeline as adaptation sets when periods[] is supplied).
        auto *period = new Period(playlist);

        const bool separateAudio = !mManifest->audio.empty();

        // Video (or muxed) adaptation set: all video representations together,
        // mirroring the HLS master playlist default adaptation set.
        const std::vector<MediaRepresentation> *videoList = &mManifest->video;
        auto *videoAdaptSet = new AdaptationSet(period);
        int representationIndex = 0;

        for (const MediaRepresentation &rep : *videoList) {
            auto *representation = new Representation(videoAdaptSet);
            representation->setBandwidth(static_cast<uint64_t>(rep.bandwidth));
            if (rep.hasAverageBandwidth) {
                // averageBandwidth is used for ABR hints; stored via codecs string? keep bandwidth only.
            }
            if (rep.hasWidth && rep.hasHeight) {
                representation->setWidth(rep.width);
                representation->setHeight(rep.height);
            }
            representation->mStreamType = separateAudio ? STREAM_TYPE_VIDEO : STREAM_TYPE_MIXED;
            representation->b_live = mManifest->live;
            representation->targetDuration = static_cast<int64_t>(
                    (rep.hasSegmentInfo ? rep.segmentInfo.targetDuration : 4) * CLOCK_FREQ_US);
            if (rep.hasSegmentInfo && rep.segmentInfo.mode != SegmentMode::Single) {
                representation->setBaseUrl(rep.baseUrl);
            }
            if (!rep.codecs.empty()) {
                representation->addCodecs(rep.codecs);
            }
            if (!rep.lang.empty()) {
                representation->mLang = rep.lang;
            }

            // Segment list construction.
            if (rep.hasSegmentInfo) {
                const SegmentInfo &segInfo = rep.segmentInfo;
                auto *segmentList = new SegmentList(representation);

                std::vector<Segment> segments;
                if (segInfo.mode == SegmentMode::List || segInfo.mode == SegmentMode::Template) {
                    if (!segInfo.segments.empty()) {
                        for (const Segment &seg : segInfo.segments) {
                            Segment resolved = seg;
                            resolved.url = resolveUrl(rep.baseUrl, seg.url);
                            segments.push_back(resolved);
                        }
                    } else if (segInfo.mode == SegmentMode::Template) {
                        segments = expandTemplate(segInfo, mManifest->duration, rep.baseUrl);
                    }
                }

                uint64_t sequence = segInfo.hasMediaSequence ? static_cast<uint64_t>(segInfo.mediaSequence)
                                    : static_cast<uint64_t>(segInfo.startNumber > 0 ? segInfo.startNumber : 1);

                // init segment (EXT-X-MAP style) when present
                std::shared_ptr<segment> initSegment;
                if (!segInfo.initialization.empty() && segInfo.mode != SegmentMode::Single) {
                    initSegment = std::make_shared<segment>(sequence++);
                    initSegment->setSourceUrl(resolveUrl(rep.baseUrl, segInfo.initialization));
                    segmentList->addInitSegment(initSegment);
                }

                std::vector<SegmentEncryption> encryptions;
                buildSegmentEncryptions(*mManifest, rep, encryptions);

                int64_t startTimeUs = 0;
                for (const Segment &seg : segments) {
                    auto pSegment = std::make_shared<segment>(sequence++);
                    pSegment->setSourceUrl(seg.url);
                    pSegment->duration = static_cast<int64_t>(seg.duration * CLOCK_FREQ_US);
                    pSegment->startTime = static_cast<uint64_t>(startTimeUs);
                    startTimeUs += pSegment->duration;
                    pSegment->init_section = initSegment;
                    if (!seg.byteRange.empty()) {
                        int64_t rangeStart = 0, rangeEnd = 0;
                        if (sscanf(seg.byteRange.c_str(), "%lld-%lld", (long long *) &rangeStart, (long long *) &rangeEnd) == 2) {
                            pSegment->setByteRange(rangeStart, rangeEnd);
                        }
                    }
                    if (!encryptions.empty()) {
                        pSegment->setEncryption(encryptions);
                    }
                    segmentList->addSegment(pSegment);
                }

                representation->SetSegmentList(segmentList);
            }

            videoAdaptSet->addRepresentation(representation);
            representationIndex++;
        }
        if (!videoAdaptSet->getRepresentations().empty()) {
            period->addAdaptationSet(videoAdaptSet);
        } else {
            delete videoAdaptSet;
        }

        // Audio adaptation sets: one per audio representation (mirrors EXT-X-MEDIA groups).
        int audioIndex = 0;
        for (const MediaRepresentation &rep : mManifest->audio) {
            (void) audioIndex;
            auto *audioAdaptSet = new AdaptationSet(period);
            auto *representation = new Representation(audioAdaptSet);
            representation->setBandwidth(static_cast<uint64_t>(rep.bandwidth));
            representation->mStreamType = STREAM_TYPE_AUDIO;
            representation->b_live = mManifest->live;
            representation->targetDuration = static_cast<int64_t>(
                    (rep.hasSegmentInfo ? rep.segmentInfo.targetDuration : 4) * CLOCK_FREQ_US);
            representation->mLang = rep.lang;
            if (!rep.codecs.empty()) {
                representation->addCodecs(rep.codecs);
            }

            if (rep.hasSegmentInfo) {
                const SegmentInfo &segInfo = rep.segmentInfo;
                auto *segmentList = new SegmentList(representation);
                std::vector<Segment> segments;
                if (!segInfo.segments.empty()) {
                    for (const Segment &seg : segInfo.segments) {
                        Segment resolved = seg;
                        resolved.url = resolveUrl(rep.baseUrl, seg.url);
                        segments.push_back(resolved);
                    }
                } else if (segInfo.mode == SegmentMode::Template) {
                    segments = expandTemplate(segInfo, mManifest->duration, rep.baseUrl);
                }

                uint64_t sequence = static_cast<uint64_t>(segInfo.startNumber > 0 ? segInfo.startNumber : 1);
                std::shared_ptr<segment> initSegment;
                if (!segInfo.initialization.empty() && segInfo.mode != SegmentMode::Single) {
                    initSegment = std::make_shared<segment>(sequence++);
                    initSegment->setSourceUrl(resolveUrl(rep.baseUrl, segInfo.initialization));
                    segmentList->addInitSegment(initSegment);
                }

                std::vector<SegmentEncryption> encryptions;
                buildSegmentEncryptions(*mManifest, rep, encryptions);

                int64_t startTimeUs = 0;
                for (const Segment &seg : segments) {
                    auto pSegment = std::make_shared<segment>(sequence++);
                    pSegment->setSourceUrl(seg.url);
                    pSegment->duration = static_cast<int64_t>(seg.duration * CLOCK_FREQ_US);
                    pSegment->startTime = static_cast<uint64_t>(startTimeUs);
                    startTimeUs += pSegment->duration;
                    pSegment->init_section = initSegment;
                    if (!encryptions.empty()) {
                        pSegment->setEncryption(encryptions);
                    }
                    segmentList->addSegment(pSegment);
                }
                representation->SetSegmentList(segmentList);
            }

            audioAdaptSet->addRepresentation(representation);
            period->addAdaptationSet(audioAdaptSet);
            audioIndex++;
        }

        playlist->addPeriod(period);
        return playlist;
    }

    // -------------------------------------------------------------- IDemuxer API --

    int ManifestDemuxer::Open()
    {
        if (mManifest == nullptr) {
            return -EINVAL;
        }

        mProxySource = new proxyDataSource();
        mProxySource->setImpl(mReadCb, mSeekCb, mOpenCb, mInterruptCb, mSetSegmentList,
                              mGetBufferDuration, mEnableCache, mUserArg);

        mPPlayList = buildPlayList();
        if (mPPlayList == nullptr) {
            return -EINVAL;
        }

        if (mManifest->mediaSourceType == "dash") {
            // DASH pipeline: needs DashManager; see demuxer/dash/DashManager.h
            AF_LOGW("mediaSourceType dash: falling back to the HLS segment pipeline (DRM-capable)\n");
        }
        mPPlaylistManager = new HLSManager(mPPlayList);
        mPPlaylistManager->setOptions(mOpts);
        mPPlaylistManager->setExtDataSource(mProxySource);
        mPPlaylistManager->setDataSourceConfig(sourceConfig);
        mPPlaylistManager->setBitStreamFormat(mMergeVideoHeader, mMergeAudioHeader);
        mPPlaylistManager->setUrlToUniqueIdCallback(mUrlHashCb, mUrlHashCbUserData);

        int ret = mPPlaylistManager->init();
        if (mFirstSeekPos != INT64_MIN) {
            mPPlaylistManager->seek(mFirstSeekPos, 0, -1);
        }
        return ret;
    }

    int ManifestDemuxer::ReadPacket(std::unique_ptr<IAFPacket> &packet, int index)
    {
        if (mPPlaylistManager) {
            return mPPlaylistManager->ReadPacket(packet, index);
        }
        return -EINVAL;
    }

    void ManifestDemuxer::Close()
    {
        delete mPPlaylistManager;
        mPPlaylistManager = nullptr;
        delete mPPlayList;
        mPPlayList = nullptr;
    }

    void ManifestDemuxer::Start()
    {
        if (mPPlaylistManager) {
            mPPlaylistManager->start();
        }
    }

    void ManifestDemuxer::Stop()
    {
        if (mPPlaylistManager) {
            mPPlaylistManager->stop();
        }
    }

    void ManifestDemuxer::PreStop()
    {
        if (mPPlaylistManager) {
            mPPlaylistManager->preStop();
        }
    }

    int64_t ManifestDemuxer::Seek(int64_t us, int flags, int index)
    {
        if (mPPlaylistManager) {
            return mPPlaylistManager->seek(us, flags, index);
        }
        mFirstSeekPos = us;
        return 0;
    }

    int ManifestDemuxer::GetNbStreams() const
    {
        return mPPlaylistManager ? mPPlaylistManager->GetNbStreams() : 0;
    }

    int ManifestDemuxer::GetNbSubStreams(int index) const
    {
        return mPPlaylistManager ? mPPlaylistManager->getNBSubStream(index) : -1;
    }

    int ManifestDemuxer::GetRemainSegmentCount(int index)
    {
        return mPPlaylistManager ? mPPlaylistManager->GetRemainSegmentCount(index) : -1;
    }

    int ManifestDemuxer::GetSourceMeta(Source_meta **meta) const
    {
        return 0;
    }

    int ManifestDemuxer::GetStreamMeta(Stream_meta *meta, int index, bool sub) const
    {
        return mPPlaylistManager ? mPPlaylistManager->GetStreamMeta(meta, index, sub) : -EINVAL;
    }

    int ManifestDemuxer::GetMediaMeta(Media_meta *mediaMeta) const
    {
        return 0;
    }

    int ManifestDemuxer::OpenStream(int index)
    {
        return mPPlaylistManager ? mPPlaylistManager->OpenStream(index) : -EINVAL;
    }

    void ManifestDemuxer::CloseStream(int index)
    {
        if (mPPlaylistManager) {
            mPPlaylistManager->CloseStream(index);
        }
    }

    void ManifestDemuxer::interrupt(int inter)
    {
        if (mPPlaylistManager) {
            mPPlaylistManager->interrupt(inter);
        }
    }

    int ManifestDemuxer::SwitchStreamAligned(int from, int to)
    {
        return mPPlaylistManager ? mPPlaylistManager->SwitchStreamAligned(from, to) : -1;
    }

    int64_t ManifestDemuxer::getMaxGopTimeUs()
    {
        // The HLSManager does not expose a per-stream GOP duration; the
        // playlist demuxer behaves the same way.
        return INT64_MIN;
    }

    UTCTimer *ManifestDemuxer::getUTCTimer()
    {
        return mPPlaylistManager ? mPPlaylistManager->getUTCTimer() : nullptr;
    }

    void ManifestDemuxer::setClientBufferLevel(client_buffer_level level)
    {
        if (mPPlaylistManager) {
            mPPlaylistManager->setClientBufferLevel(level);
        }
    }

    int ManifestDemuxer::SetOption(const std::string &key, const int64_t value)
    {
        if (key == "preferAudio" && mPPlaylistManager) {
            mPPlaylistManager->preferAudio(value != 0);
            return 0;
        }
        return IDemuxer::SetOption(key, value);
    }

    const playList *ManifestDemuxer::GetPlayList()
    {
        return mPPlayList;
    }

    const std::string ManifestDemuxer::GetProperty(int index, const string &key) const
    {
        return mPPlaylistManager ? mPPlaylistManager->GetProperty(index, key) : "";
    }

    bool ManifestDemuxer::isRealTimeStream(int index)
    {
        return mPPlaylistManager ? mPPlaylistManager->isRealTimeStream(index) : false;
    }

    bool ManifestDemuxer::isWallclockTimeSyncStream(int index)
    {
        return mPPlaylistManager ? mPPlaylistManager->isWallclockTimeSyncStream(index) : false;
    }

    int64_t ManifestDemuxer::getDurationToStartStream(int index)
    {
        return mPPlaylistManager ? mPPlaylistManager->getDurationToStartStream(index) : 0;
    }

    int64_t ManifestDemuxer::getBufferDuration(int index) const
    {
        return mPPlaylistManager ? mPPlaylistManager->getBufferDuration(index) : 0;
    }

    void ManifestDemuxer::setUrlToUniqueIdCallback(UrlHashCB cb, void *userData)
    {
        mUrlHashCb = cb;
        mUrlHashCbUserData = userData;
        if (mPPlaylistManager) {
            mPPlaylistManager->setUrlToUniqueIdCallback(cb, userData);
        }
    }

} // namespace Cicada
