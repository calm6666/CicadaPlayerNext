//
// Created by yuyuan on 2021/03/17.
//

#ifndef DEMUXER_DASH_DASH_STREAM_H
#define DEMUXER_DASH_DASH_STREAM_H

#include "demuxer/DemuxerMetaInfo.h"
#include "demuxer/demuxer_service.h"
#include "demuxer/play_list/AbstractStream.h"
#include "utils/CicadaJSON.h"
#include "utils/afThread.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>

namespace Cicada {

    class DashSegmentTracker;
    namespace Dash {
        class DashSegment;
    }

    class DashStream : public AbstractStream {

    public:
        DashStream(DashSegmentTracker *pTracker, int id);

        ~DashStream() override;

        int getStreamType() const;

        int GetRemainSegmentCount();

        int open() override;

        void close() override;

        int getId() const
        {
            return mId;
        }

        int read(unique_ptr<IAFPacket> &packet) override;

        int GetNbStreams() const override;

        int GetStreamMeta(Stream_meta *meta, int index, bool sub) const override;

        bool isOpened() override;

        int start() override;

        int preStop() override;

        int stop() override;

        int64_t seek(int64_t us, int flags) override;

        uint64_t getCurSegNum() override;


        int stopOnSegEnd(bool stop) override;

        int SetCurSegNum(uint64_t num) override;

        uint64_t getCurSegPosition() override;

        int setCurSegPosition(uint64_t position) override;

        bool isLive() override;

        int64_t getDuration() override;

        int getNBStream() const override;

        void interrupt(int inter) override;

        std::string GetProperty(const string &key);
        
        bool isRealTimeStream();

        int UpdateInitSection();

        int64_t getDurationToStartStream();

        vector<mediaSegmentListEntry> getSegmentList();

        void enableCache(bool enalbe);

        int64_t getBufferDuration() const;

        void setPreferAudio(bool preferAudio);

    private:

        static int read_callback(void *arg, uint8_t *buffer, int size);
        static int64_t seek_callback(void *arg, int64_t offset, int whence);


        int read_internal(std::unique_ptr<IAFPacket> &packet);

        int64_t seek_internal(uint64_t segNum, int64_t us);

        int open_internal();

        void resetSource();

        void recreateSource(const string &url);

        void clearDataFrames();

        void interrupt_internal(int inter);

        CicadaJSONArray openInfoArray;

        int openSegment(const string &uri, int64_t start = INT64_MIN, int64_t end = INT64_MIN);

        int tryOpenSegment(const string &uri, int64_t start, int64_t end);

        int64_t getPackedStreamPTS();

        int createDemuxer();

        int readSegment(const uint8_t *buffer, int size);

        int64_t seekSegment(off_t offset, int whence);

        int updateSegment();

        enum OpenType {
            SegNum, SegPosition
        };

        int reopenSegment(uint64_t num, OpenType openType);

    private:

        enum dataSourceStatus {
            dataSource_status_valid,
            dataSource_status_invalid,
            dataSource_status_error
        };
        DashSegmentTracker *mPTracker = nullptr;
        int mId = -1;
        std::unique_ptr<demuxer_service> mPDemuxer = nullptr;
        IDataSource *mPdataSource = nullptr;
        atomic_bool mIsOpened{false};
        atomic_bool mIsEOS{false}; //demuxer eos
        bool mIsDataEOS = false;
        bool mReopen = false;
        atomic_bool mSwitchNeedBreak{false};
        Dash::DashSegment *mCurSeg = nullptr;
        Dash::DashSegment *mCurInitSeg{nullptr};
        uint8_t *mInitSegBuffer{nullptr};
        int64_t mInitSegSize{0};
        int mInitSegPtr{0};
        std::atomic_bool mStopOnSegEnd{false};
        bool mLastReadSuccess{false};
        std::mutex mDataMutex;
        std::condition_variable mWaitCond;
        std::deque<unique_ptr<IAFPacket>> mQueue;
        IDataSource *mSegKeySource = nullptr;
        mutable std::mutex mHLSMutex;

        int read_thread();

        std::atomic_int mError{0};
        int mDataSourceError = 0;
        int64_t mSeekPendingUs = -1;
        // seek 后第一个分片点不参与"媒体重置"判断（seek 造成的 pts 跳变不是 Period 切换）
        bool mSeekSuppressResetOnce = false;
        std::atomic<bool> mIsOpened_internal{false};
        std::atomic_bool mInterrupted{false};
        std::atomic_bool mExited{false};
        afThread *mThreadPtr = nullptr;
        string mKeyUrl = "";
        uint8_t mKey[16];


        struct segmentTimeInfo {
            bool seamlessPoint = false;
            int64_t timePosition = INT64_MIN;
            int64_t time2ptsDelta = INT64_MIN;
            int64_t utcTime = INT64_MIN;
            int64_t utc2ptsDelta = INT64_MIN;
            int64_t frameDuration = INT64_MIN;
            int64_t lastFramePts = INT64_MIN;
        };

        std::map<int, segmentTimeInfo> mStreamStartTimeMap;

        int64_t mPacketFirstPts = INT64_MAX;

        std::unique_ptr<DemuxerMetaInfo> mDemuxerMeta = nullptr;
        int OpenedStreamIndex = 0;
        bool mProtectedBuffer{false};

        int64_t mStreamStartTime = 0;
        int64_t mSuggestedPresentationDelay = 0;
        atomic_bool mIsFirstOpen{true};
        bool mEnableCache{false};
        bool mIsStartSegment{false};
        bool mIsPreload{false};
        bool mPreloadSucc{false};
        std::atomic<bool> mPreferAudio{false};

        /*
         * ============ seek 落点延迟线（只对点播视频路生效）============
         *
         * 完整说明见 DashStream::seekLandingFilter 上面的长注释。一句话：DASH 只能把 seek
         * 定位到"包含目标的那一个分片"（10 秒一个独立文件），落点因此是分片片首，实测比目标
         * 早 3~9 秒；分片内部的 IDR 又无法用字节范围落上去（分片里只有一个 moof，没有可以从
         * 内部进入的 box 边界，内层 demuxer 也不可 seek）。所以改成在**包**这一层把落点挪到
         * "不晚于目标的最后一个关键帧"：它之前的包整体丢掉，之后的包按序交出。解码起点于是从
         * "分片片首"变成"≤ 一个关键帧间隔"，而精度语义（包含目标、不晚于目标）一个字节不改。
         *
         * 全部状态迁移由包自带的事件驱动（timePosition 单调 + AF_PKT_FLAG_KEY），没有计时器、
         * 没有预算。本仓库约定：新成员一律追加在类末尾（中间插入会移动偏移、破坏增量构建）。
         */
        int64_t mSeekLandingTargetUs{INT64_MIN};
        bool mSeekLandingStarted{false};
        bool mSeekLandingHaveKey{false};
        bool mSeekLandingProgress{false};
        std::deque<unique_ptr<IAFPacket>> mSeekLandingStage{};
        std::vector<uint8_t> mSeekLandingExtraData{};
        /* 已经丢掉的"落点之前"的包数（只用于日志，证明延迟线真的接管了这次 seek） */
        int mSeekLandingDropped{0};
        /* "换更近的落点"这类日志每轮装弹的硬上限（防极端 GOP 刷屏；其余状态各只打一条） */
        int mSeekLandingLogCount{0};

        void seekLandingArm(int64_t targetUs);

        bool seekLandingFilter(std::unique_ptr<IAFPacket> &packet);

        void seekLandingFlush();

        void seekLandingReset();

        /* 只丢"已收下还没交出"的包，保留 seek 目标（stop/start 会重开同一个分片，目标仍有效） */
        void seekLandingDropStage();
    };
}


#endif //DEMUXER_DASH_DASH_STREAM_H
