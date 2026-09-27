//
// Created by yuyuan on 2021/03/17.
//

#ifndef DEMUXER_DASH_DASH_STREAM_H
#define DEMUXER_DASH_DASH_STREAM_H

#include "demuxer/DemuxerMetaInfo.h"
#include "demuxer/SeekLandingStage.h"
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
         * 唯一一份实现在 demuxer/SeekLandingStage.h（原来 DashStream 与 HLSStream 各有一份同形
         * 拷贝，已删除、收敛成那一份）。完整说明 —— 含"精度权威在 renderer 的单一落点过滤，
         * 这里只是降低解码前推距离的尽力优化、失败只慢不错"这句定性 —— 见那里的类注释。
         * 一句话：DASH 也只能把 seek 定位到"包含目标的那一个分片"（10 秒一个独立文件），落点因此
         * 是分片片首，实测比目标早 3~9 秒；分片内部的 IDR 又无法用字节范围落上去（分片里只有一个
         * moof，没有可以从内部进入的 box 边界，内层 demuxer 也不可 seek）。延迟线于是在**包**这一
         * 层把落点挪到"不晚于目标的最后一个关键帧"：它之前的包整体丢掉，之后的包按序交出。
         *
         * 三个引用是"放行时把包按原序交回 mQueue"需要的（锁与唤醒方式和 read_thread 推包一致）。
         * 本仓库约定：新成员一律追加在类末尾（中间插入会移动偏移、破坏增量构建）。
         */
        SeekLandingStage mSeekLanding{mQueue, mDataMutex, mWaitCond};
    };
}


#endif //DEMUXER_DASH_DASH_STREAM_H
