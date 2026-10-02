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

        /*
         * ============ DASH 的 DRM：清单声明 + 选优 + 软解兜底 ============
         *
         * 【分工】"谁来解 CENC"有两个可能的执行者，按**能力**决定，不是开关：
         *   1. **平台 CDM**（Android MediaCodec+MediaCrypto、OHOS DRM Kit）——
         *      只要 `DrmHandlerPrototype::isSupport()` 认选中的那个 scheme，
         *      就把 format/uri/pssh/KID 交给 Stream_meta，由平台在安全世界里解。
         *      这条优先（用户要求"能硬解的都硬解"）。
         *   2. **内核软解**（CENCDecrypter）—— 只在平台不认这个 scheme、
         *      而清单又明确给出了一个可以取到内容密钥的地址时启用，且样本由
         *      demuxer_service 在读取路径上就地解。
         *
         * 【选优】清单可以同时声明多条 ContentProtection（mp4protection + 各 DRM 系统），
         * 选哪一条由 `DrmSchemes::decide` 按"平台认不认 / 有没有可取密钥的地址 /
         * 有没有指名具体 DRM 系统"排序决定，排序判据见 framework/drm/DrmSchemes.h。
         * 结果缓存一次（mDrmDecision），因为 GetStreamMeta 会被反复调用。
         *
         * 新成员一律追加在类末尾（仓库约定，中间插入会移动偏移）。
         */

        /** 选优结果（懒计算 + 缓存）。清单没声明任何内容保护时 index 为 -1。 */
        DrmSchemes::Decision drmDecision() const;

        /** 平台 CDM 认这条 scheme ⇒ 把 DRM 参数写进 meta；否则一个字段都不写。 */
        void applyDrmFromContentProtection(Stream_meta *meta) const;

        /** 是否要由内核软解（平台不认 + 清单给了可取密钥的地址 + 内容确实是 CENC）。 */
        bool needsSoftwareCencDecryption() const;

        /** 清单是否声明了 CENC 保护的档位（决定内层 demuxer 的形态提示）。 */
        bool declaresCencProtectedContent() const;

        /*
         * 按清单声明的地址取内容密钥（软件兜底）。由 demuxer_service 在读到第一个带
         * 加密信息的包时回调（见 demuxer_service::setCencKeyResolver）。
         *
         * kidHex 只用于"每个 KID 只问一次"的去重；密钥内容由清单里那一个 laurl 决定。
         * 失败**不静默**：打一条 ERROR 说明"这条流解不开、以及为什么"。
         */
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
