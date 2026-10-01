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

        /*
         * 把这一路 Representation 的 codecs 列表拼成清单原始形态
         * （逗号分隔，例如 "hvc1.1.6.L93.B0" 或 "avc1.64001f,mp4a.40.2"），
         * 交给 afCodecShortName()/afCodecIDFromManifestCodecs() 归一化。
         * 没有任何 codecs 时返回空串。
         * 非虚函数，不动 vtable 布局。
         */
        std::string getCodecsString() const;

        std::string contextualize(size_t number, const std::string &component, const Dash::SegmentTemplate *templ) const;

        int64_t getScaledTimeBySegmentNumber(uint64_t index, const Dash::SegmentTemplate *templ) const;

        virtual bool needsIndex() const;

        std::string getMimeType() const;

        void updateStreamType();

        bool getSegmentNumberByTime(int64_t time, uint64_t *ret) const;

        int64_t getMinAheadTime(uint64_t curnum) const;

        /*
         * ============ 内容保护（DASH <ContentProtection>）============
         *
         * 【为什么放在 Representation 上】ContentProtection 在 MPD 里可以出现在
         * **AdaptationSet** 或 **Representation** 任一层（Rep 层覆盖 AS 层），
         * 所以"这一路流受什么保护"必须落在 Representation 上才对 —— 只记在
         * AdaptationSet 上会在"同一 AS 下有的档加密、有的不加密"时判错。
         *
         * 【为什么不是布尔开关】这里存的是**清单里写了什么**（schemeIdUri / KID /
         * PSSH / 显式 keyUrl），一个字节都不加、一个默认值都不发明。下游按
         * "有没有值"决定行为，不需要任何 enable/disable。
         */
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
        /*
         * 按本仓库惯例：新成员追加在类末尾（中间插入会移动偏移、破坏增量构建）。
         * 上面那个结构体与这两个访问器是新增，原有字段一个没动、顺序没变。
         */
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
