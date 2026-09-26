//
// Created by moqi on 2018/4/28.
//

#ifndef FRAMEWORK_HLSSTREAM_H
#define FRAMEWORK_HLSSTREAM_H

#include "AbstractStream.h"
#include "SegmentTracker.h"
#include "demuxer/DemuxerMetaInfo.h"
#include "demuxer/demuxer_service.h"
#include "demuxer/play_list/segment_decrypt/ISegDecrypter.h"
#include "demuxer/sample_decrypt/HLSSampleAesDecrypter.h"
#include "segment_decrypt/SegmentEncryption.h"
#include "utils/CicadaJSON.h"
#include "utils/afThread.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <set>
#include <mutex>

namespace Cicada {

    enum MoveToNextPart { tryAgain = -11, segmentEnd = 0, moveSuccess = 1 };

    class HLSStream : public AbstractStream {

        class WebVttParser {

        public:
            WebVttParser();

            ~WebVttParser();

            int64_t addBuffer(uint8_t *buffer, int size);

            void rest();

        private:
            uint8_t *mBuffer = nullptr;
            int mSize = 0;
            int64_t mMapPTS = INT64_MIN;
            bool bFinished = false;
        };

    public:
        HLSStream(SegmentTracker *pTracker, int id);

        ~HLSStream() override;

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

        bool CloseSubStream(int index) override;

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

        int setCurSegInfo(CurSegInfo &curSegInfo) override;

        void setCurRenditionInfo(const std::vector<RenditionReport> &renditions) override;
        std::vector<RenditionReport> getCurRenditionInfo() override;

        bool isLive() override;

        int64_t getDuration() override;

        int getNBStream() const override;

        void interrupt(int inter) override;

        std::string GetProperty(const string &key);
        
        bool isRealTimeStream();


        int64_t getTargetDuration();

        vector<mediaSegmentListEntry> getSegmentList();

        int64_t getBufferDuration() const;

    private:

        static const char *hls_id3;

        static int read_callback(void *arg, uint8_t *buffer, int size);

        static int Decrypter_read_callback(void *arg, uint8_t *buffer, int size);

//        static int64_t seek_callback(void *arg, int64_t offset, int whence);


        int read_internal(std::unique_ptr<IAFPacket> &packet);

        int64_t seek_internal(uint64_t segNum, int64_t us);

        int open_internal();

        void resetSource();

        void recreateSource(const string &url);

        void clearDataFrames();

        int updateSegDecrypter();

        bool updateKey();

        int updateSampleAesDecrypter();

        int updateDecrypter();

        void interrupt_internal(int inter);

        CicadaJSONArray openInfoArray;

        int openSegment(const string &uri, int64_t start = INT64_MIN, int64_t end = INT64_MIN);

        int tryOpenSegment(const string &uri, int64_t start, int64_t end);

        int tryOpenSegment(std::shared_ptr<segment> seg);

        int createDemuxer();

        int readSegment(const uint8_t *buffer, int size);

        MoveToNextPart moveToNextPartialSegment();

        int upDateInitSection();

        int64_t seekSegment(off_t offset, int whence);

        int updateSegment();

        bool updateIV();

        enum OpenType {
            SegNum, SegPosition
        };

        int reopenSegment(std::map<OpenType, uint64_t> &params);

        int64_t getPackedStreamPTS();

    private:

        enum dataSourceStatus {
            dataSource_status_valid,
            dataSource_status_invalid,
            dataSource_status_error
        };
        SegmentTracker *mPTracker = nullptr;
        int mId = -1;
        std::unique_ptr<demuxer_service> mPDemuxer = nullptr;
        IDataSource *mPdataSource = nullptr;
        atomic_bool mIsOpened{false};
        atomic_bool mIsEOS{false}; //demuxer eos
        bool mIsDataEOS = false;
        bool mReopen = false;
        atomic_bool mSwitchNeedBreak{false};
        std::shared_ptr<segment> mCurSeg = nullptr;
        std::shared_ptr<segment> mCurInitSeg{nullptr};
        uint8_t *mInitSegBuffer{nullptr};
        int64_t mInitSegSize{0};
        int mInitSegPtr{0};
        std::atomic_bool mStopOnSegEnd{false};
        /* "停在本片界"的日志每次只打一行（否则 read_thread 每 10ms 一轮会刷屏） */
        bool mStopOnSegEndLogged{false};
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
        bool mIsOpened_internal = false;
        std::atomic_bool mInterrupted{false};
        std::atomic_bool mExited{false};
        afThread *mThreadPtr = nullptr;
        std::unique_ptr<ISegDecrypter> mSegDecrypter = nullptr;
        std::unique_ptr<HLSSampleAesDecrypter> mSampeAesDecrypter = nullptr;
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

        WebVttParser mWVTTParser;
        int64_t mVttPtsOffSet = INT64_MIN;

        std::unique_ptr<DemuxerMetaInfo> mDemuxerMeta = nullptr;
        int OpenedStreamIndex = 0;
        bool mProtectedBuffer{false};

        //segment index to start live streams at (negative values are from the end). for lhls, it is partial segment index
        int64_t mLiveStartIndex{-3};

        std::string mDRMMagicKey{};
        SegmentEncryption mCurrentEncryption{};

        std::atomic_bool mIsFirstOpen{true};

        int64_t mLastPts{INT64_MIN};
        int64_t mDiscardPts{INT64_MIN};
        int discardCount{0};

        std::set<int> mClosedSubStreams;

        bool mSegmentOpened{false};

    public:
        int64_t getLastPts()
        {
            return mLastPts;
        }

        void setDiscardPts(int64_t pts)
        {
            mDiscardPts = pts;
        }

    private:
        /*
         * ============ seek 落点延迟线（只对点播视频路生效）============
         *
         * 与 DashStream 里那一套**同形**：完整说明见 HLSStream::seekLandingFilter 上面的长注释。
         * 一句话：HLS 也只能把 seek 定位到"包含目标的那一片"（10 秒一个独立文件），落点因此是
         * 分片片首；所以在**包**这一层把落点挪到"不晚于目标的最后一个关键帧"——它之前的包整体
         * 丢掉，之后的包按序交出。精度语义（包含目标、不晚于目标）一个字节不改。
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


#endif //FRAMEWORK_HLSSTREAM_H
