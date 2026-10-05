//
// Created by moqi on 2018/4/25.
//

#ifndef FRAMEWORK_REPRESENTATION_H
#define FRAMEWORK_REPRESENTATION_H

#include "SegmentPart.h"
#include "demuxer/dash/SegmentInformation.h"
#include "utils/AFMediaType.h"
#include <ctime>
#include <list>

namespace Cicada{

    class AdaptationSet;
    class SegmentList;
    class playList;

    namespace Dash {
        class SegmentTemplate;
    }

    class Representation : public Dash::SegmentInformation {
    public:
        Representation(AdaptationSet *adapt);
        ~Representation();

        void SetSegmentList(SegmentList *pSegList);

        SegmentList *GetSegmentList();

        void setPlaylistUrl(const std::string &);

        const std::string &getPlaylistUrl();

        void setBaseUrl(const std::string &url);

        const std::string &getBaseUrl();

        void setBandwidth(uint64_t bandwidth);

        void setWidth(int width);

        void setHeight(int height);

        void print();

        int getStreamInfo(int *width, int *height, uint64_t *bandwidth, std::string &language);

        playList *getPlaylist();

        AdaptationSet *getAdaptationSet();

        void addCodecs(const std::string &codecs);
        std::string getCodecsString() const;

        std::string contextualize(size_t number, const std::string &component, const Dash::SegmentTemplate *templ) const;

        int64_t getScaledTimeBySegmentNumber(uint64_t index, const Dash::SegmentTemplate *templ) const;

        virtual bool needsIndex() const;

        std::string getMimeType() const;

        void updateStreamType();

        bool getSegmentNumberByTime(int64_t time, uint64_t *ret) const;
        int64_t getMinAheadTime(uint64_t curnum) const;
        struct ContentProtection {
            // <ContentProtection schemeIdUri="...">；mp4protection 那一条表示
            // "本段受 CENC 保护"，各 DRM 系统（Widevine/PlayReady/FairPlay）各有一条。
            std::string schemeIdUri;
            // <ContentProtection value="...">，一般与 schemeIdUri 配合（如 cenc。
            std::string value;
            // cenc:default_KID（UUID 文本，带连字符或纯 hex 都可能）。
            std::string keyId;
            // <cenc:pssh> 的内容（base64）。
            std::string pssh;
            // 该条 ContentProtection 上声明的许可/密钥地址（laurl 之类）。
            // **不发明**：清单里没有就是空串，下游据此决定"能不能自己去取密钥"。
            std::string licenseUrl;
        };

        const std::vector<ContentProtection> &getContentProtections() const
        {
            return mContentProtections;
        }

        void addContentProtection(const ContentProtection &protection)
        {
            mContentProtections.push_back(protection);
        }

        // 清单里是否声明了内容保护（**不判断能不能播**，只判断"写了没有"）。
        bool hasContentProtection() const
        {
            return !mContentProtections.empty();
        }

    public:
        // TODO use set and get
        int64_t targetDuration = 0;
        int64_t partTargetDuration = 0;
        bool b_live = false;
        int mPlayListType{0};
        Stream_type mStreamType = STREAM_TYPE_MIXED;
        std::string mLang = "";
        std::list<std::string> codecs;
        std::string mimeType;
        bool mCanBlockReload{false};
        PreloadHint mPreloadHint;
        double mCanSkipUntil{0.0};
        double mHoldBack{0.0};
        double mPartHoldBack{0.0};
        std::vector<RenditionReport> mRenditionReport;
        std::vector<ContentProtection> mContentProtections;

    private:
        SegmentList *mPSegList = nullptr;
        AdaptationSet *mAdapt = nullptr;
        std::string mBaseUrl = "";
        std::string mPlaylistUrl;
        uint64_t mBandWidth = 0;
        int mWidth = 0;
        int mHeight = 0;
        //std::list<std::string> lang;
    };
}


#endif //FRAMEWORK_REPRESENTATION_H
