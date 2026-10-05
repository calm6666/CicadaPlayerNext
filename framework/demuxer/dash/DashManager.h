//
// Created by yuyuan on 2021/03/17.
//

#ifndef DEMUXER_DASH_DASH_MANAGER_H
#define DEMUXER_DASH_DASH_MANAGER_H

#include "demuxer/play_list/PlaylistManager.h"

namespace Cicada {

    class DashStream;
    class AdaptationSet;
    class Period;

    class DashManager : public PlaylistManager {

        class DashStreamInfo {
        public:
            DashStream *mPStream = nullptr;
            std::unique_ptr<IAFPacket> mPFrame{};
            bool selected = false;
            bool stopOnSegEnd = false;
            int toStreamId = -1;
            bool eos = false;
        };

    public:
        explicit DashManager(playList *pList);

        ~DashManager() override;

        int init() override;

        void preStop() override;

        void stop() override;

        int GetNbStreams() const override;

        int GetStreamMeta(Stream_meta *meta, int index, bool sub) const override;

        int ReadPacket(std::unique_ptr<IAFPacket> &packet, int index) override;

        int OpenStream(int id) override;

        void CloseStream(int id) override;

        int start() override;

        int64_t seek(int64_t us, int flags, int index) override;

        int SwitchStreamAligned(int from, int to) override;

        int getNBSubStream(int index) const override;

        void interrupt(int inter) override;

        const std::string GetProperty(int index, const string &key) override;

        int GetRemainSegmentCount(int index) override;

        bool isRealTimeStream(int index) override;
        bool isWallclockTimeSyncStream(int index) override;
        int64_t getDurationToStartStream(int index) override;

        int64_t getTargetDuration() override;

        int64_t getBufferDuration(int index) const override;

        UTCTimer *getUTCTimer() override;

        void setClientBufferLevel(client_buffer_level level) override;

        void preferAudio(bool prefer) override;

    private:
        std::list<AdaptationSet *> FindSuitableAdaptationSets(Period* period);
        std::list<DashStreamInfo *> mStreamInfoList{};
        DashStream *mMuxedStream = nullptr;
        bool mStarted = false;
        int64_t mFirstSeekPos = INT64_MIN;
        bool mPreferAudioEnabled{false};
        client_buffer_level mBufferLevel{client_buffer_level_unknown};
        int32_t mOpenAudioStreamCount{0};
        int32_t mLowestBandwidthVideoId{-1};

        /*
         * 【待打开的目标档号】-1 = 无（乙-2 的预取意图）。
         *
         * 与 mPendingAlignedSwitchStream（首包定位）生命周期不同：这个在 ReadPacket 的
         * **读循环之后**消费（打开目标档并让它开始取数据），那个在目标档首个包到达时消费
         * （按播放头定位）。带值状态，不是布尔开关；无计时器、无重试。
         * 追加在成员区末尾（本工程硬规则：只有追加才是增量 ABI 安全的）。
         */
        int mPendingPrefetchStream{-1};

        /*
         * 【边界已换流的标记】-1 = 还没换流；否则 = 边界块刚刚换成的那个目标档号。
         *
         * 用途：**提交必须由"分片边界"触发**，不能由"目标档首个包到达"触发 —— 乙-2 之后
         * 目标档在请求那一刻就被打开并开始产包，首包几乎是点击那一刻就到，按它提交会把
         * 旧档待播的包整批丢掉（用户看到"点下去立刻切换成功 + 旧流加速/跳帧"）。
         * 播放器在切换在途期间经既有 GetProperty("alignedSwitchBoundaryReached") 读它，
         * 读到 "1"（= 边界块已经完成换流，也就是旧流播到它当前分片末尾那一刻）才提交。
         * 新一次切换在 SwitchStreamAligned 里抹回 -1。
         * 带值状态（流号），不是布尔开关；无计时器、无重试。追加在成员区末尾。
         */
        int mAlignedSwitchBoundaryStream{-1};
    };
}// namespace Cicada


#endif//DEMUXER_DASH_DASH_MANAGER_H
