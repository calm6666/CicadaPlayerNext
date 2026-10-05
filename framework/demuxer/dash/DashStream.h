//
// Created by yuyuan on 2021/03/17.
//

#ifndef DEMUXER_DASH_DASH_STREAM_H
#define DEMUXER_DASH_DASH_STREAM_H

#include "demuxer/DemuxerMetaInfo.h"
#include "demuxer/SeekLandingStage.h"
#include "demuxer/demuxer_service.h"
#include "demuxer/play_list/AbstractStream.h"
// DrmSchemes::Decision 是**按值**缓存在本类里的成员，所以这里必须拿到完整定义
// （只前向声明不够）。DrmSchemes 本身是平台无关的纯函数集合，见 framework/drm/。
#include "drm/DrmSchemes.h"
#include "utils/CicadaJSON.h"
#include "utils/afThread.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>

namespace Cicada {

    class Representation;
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
        bool getSegmentNumByTime(int64_t timeUs, uint64_t &num);

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
        SeekLandingStage mSeekLanding{mQueue, mDataMutex, mWaitCond};

        /** 选优结果（懒计算 + 缓存）。清单没声明任何内容保护时 index 为 -1。 */
        DrmSchemes::Decision drmDecision() const;

        /** 平台 CDM 认这条 scheme ⇒ 把 DRM 参数写进 meta；否则一个字段都不写。 */
        void applyDrmFromContentProtection(Stream_meta *meta) const;

        /** 是否要由内核软解（平台不认 + 清单给了可取密钥的地址 + 内容确实是 CENC）。 */
        bool needsSoftwareCencDecryption() const;

        /** 清单是否声明了 CENC 保护的档位（决定内层 demuxer 的形态提示）。 */
        bool declaresCencProtectedContent() const;
        bool fetchSoftwareCencKey(const std::string &kidHex, uint8_t *key, int *keySize) const;

        Representation *getCurrentRepresentation() const;

        // mutable: GetStreamMeta 是 const 覆盖（基类 AbstractStream 的签名），
        // 而这几个成员只是"这件事算过了/日志打过了"，不改对外可见状态。
        // 【缓存键】mDrmDecisionRep 记的是算这份结论时的当前 Representation：
        // 同一条 DashStream 会在 AdaptationSet 内部换档，而 ContentProtection 允许被
        // Representation 层覆盖，所以换了档必须重算（见 drmDecision 的说明）。
        mutable DrmSchemes::Decision mDrmDecision{};
        mutable bool mDrmDecisionValid{false};
        mutable Representation *mDrmDecisionRep{nullptr};
        // 取密钥失败/成功各只打一条日志（同 HLSStream::mKeyFetchFailedLogged 的做法）。
        mutable bool mSoftwareCencLogged{false};
    };
}


#endif //DEMUXER_DASH_DASH_STREAM_H
