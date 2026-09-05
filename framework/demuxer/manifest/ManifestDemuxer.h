//
// ManifestDemuxer.h
//
// Object-based playback demuxer: converts a unified MediaManifest object
// (the C++ counterpart of hili-player's MediaManifest, see
// framework/demuxer/manifest/MediaManifest.h) into the internal playList
// object model in one shot — zero m3u8/mpd text, zero manifest network I/O —
// and delegates the actual segment scheduling/reading to the existing
// PlaylistManager (HLSManager by default, the DRM-capable pipeline).
//
// This is the C++ equivalent of manifest-to-hls.ts / manifest-to-dash.ts:
//   - SegmentInfo single/template/list modes -> SegmentList + init_section
//   - AES-128 (per-representation or global)    -> SegmentEncryption
//   - ContentProtection / LicenseServer         -> SegmentEncryption(AES_SAMPLE)
//                                                  with keyFormat=schemeIdUri,
//                                                  keyUrl=laUrl, which flows into
//                                                  Stream_meta.keyFormat/keyUrl
//                                                  and the DRM handler chain
//                                                  (Widevine/FairPlay/ClearKey).
//

#ifndef FRAMEWORK_DEMUXER_MANIFEST_MANIFEST_DEMUXER_H
#define FRAMEWORK_DEMUXER_MANIFEST_MANIFEST_DEMUXER_H

#include <demuxer/IDemuxer.h>
#include <demuxer/play_list/segment_decrypt/SegmentEncryption.h>
#include <memory>
#include "MediaManifest.h"

namespace Cicada {

    class PlaylistManager;
    class playList;
    class proxyDataSource;

    class ManifestDemuxer : public IDemuxer {
    public:
        /** Takes ownership of the manifest object. */
        explicit ManifestDemuxer(std::unique_ptr<Manifest::MediaManifest> manifest);

        ~ManifestDemuxer() override;

        int Open() override;

        int ReadPacket(std::unique_ptr<IAFPacket> &packet, int index) override;

        void Close() override;

        void Start() override;

        void Stop() override;

        void PreStop() override;

        int64_t Seek(int64_t us, int flags, int index) override;

        int GetNbStreams() const override;

        int GetNbSubStreams(int index) const override;

        int GetRemainSegmentCount(int index) override;

        int GetSourceMeta(Source_meta **meta) const override;

        int GetStreamMeta(Stream_meta *meta, int index, bool sub) const override;

        int GetMediaMeta(Media_meta *mediaMeta) const override;

        int OpenStream(int index) override;

        void CloseStream(int index) override;

        void interrupt(int inter) override;

        int SwitchStreamAligned(int from, int to) override;

        int64_t getMaxGopTimeUs() override;

        UTCTimer *getUTCTimer() override;

        void setClientBufferLevel(client_buffer_level level) override;

        int SetOption(const std::string &key, const int64_t value) override;

        void flush() override
        {}

        bool isPlayList() const override
        {
            return true;
        }

        const playList *GetPlayList() override;

        const std::string GetProperty(int index, const string &key) const override;

        bool isRealTimeStream(int index) override;

        bool isWallclockTimeSyncStream(int index) override;

        int64_t getDurationToStartStream(int index) override;

        bool isTSDiscontinue() override
        {
            return true;
        }

        int64_t getBufferDuration(int index) const override;

        void setUrlToUniqueIdCallback(UrlHashCB cb, void *userData) override;

    private:
        /** One-shot conversion MediaManifest -> playList object model. */
        playList *buildPlayList();

        static std::string resolveUrl(const std::string &baseUrl, const std::string &relativeUrl);

        static std::string deriveMediaPattern(const std::string &initialization);

        static std::vector<Manifest::Segment> expandTemplate(const Manifest::SegmentInfo &info,
                                                            double totalDuration, const std::string &baseUrl);

        static void buildSegmentEncryptions(const Manifest::MediaManifest &manifest,
                                            const Manifest::MediaRepresentation &rep,
                                            std::vector<SegmentEncryption> &out);

    private:
        std::unique_ptr<Manifest::MediaManifest> mManifest;
        playList *mPPlayList{nullptr};
        PlaylistManager *mPPlaylistManager{nullptr};
        proxyDataSource *mProxySource{nullptr};
        int64_t mFirstSeekPos{INT64_MIN};
        UrlHashCB mUrlHashCb{nullptr};
        void *mUrlHashCbUserData{nullptr};
    };

} // namespace Cicada

#endif // FRAMEWORK_DEMUXER_MANIFEST_MANIFEST_DEMUXER_H
