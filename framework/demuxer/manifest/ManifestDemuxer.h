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
//   - ContentProtection                          -> SegmentEncryption(CENC)
//                                                  selected by DrmSchemes over all
//                                                  declared systems; keyFormat is set
//                                                  ONLY when a platform CDM handles the
//                                                  chosen scheme (so the software decoder
//                                                  is never excluded by a non-null
//                                                  DrmInfo), keyUrl = the licence/key URL
//                                                  used by the on-demand software key
//                                                  resolver, pssh/keyId passed through.
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
        /*
         * SegmentBase（mode == "single"）展开：一个文件 + 字节范围。
         *
         * 取段来源两种，优先显式列表、其次 sidx：
         *   · info.segments 非空 → 直接用它（每条自带 byteRange / duration），
         *     这是"生成器同时把段表算好"的那条路，不依赖运行期解析；
         *   · 否则用 info.indexRange 按范围把 sidx 拉下来解析：偏移基准与
         *     DashSegmentTracker::parseIndex 同一套（first_offset + indexEnd + 1），
         *     每个 reference 生成一段（byteRange + duration）。
         *
         * 返回空表表示这段没法播（没有 segments[]、indexRange 非法、拉不到或解析不出 sidx），
         * 调用方据此跳过这个 representation 并打日志，不静默留一个空段表。
         */
        static std::vector<Manifest::Segment> expandSegmentBase(const std::string &fileUrl,
                                                               const Manifest::SegmentInfo &info);

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
