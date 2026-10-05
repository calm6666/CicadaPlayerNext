//
// Created by moqi on 2018/4/28.
//

#ifndef FRAMEWORK_HLSSTREAM_H
#define FRAMEWORK_HLSSTREAM_H

#include "AbstractStream.h"
#include "SegmentTracker.h"
#include "demuxer/DemuxerMetaInfo.h"
#include "demuxer/SeekLandingStage.h"
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

        /*
         * ============ CENC 软解兜底：按清单声明的地址取内容密钥（本轮新增）============
         *
         * 由 demuxer_service 在读到**第一个**带加密信息的包时回调
         * （见 demuxer_service::setCencKeyResolver）。被调用说明：
         *   · 本片是 CENC（SegmentEncryption::CENC）；
         *   · 没有平台 CDM 认这条 scheme（认了就不会装 resolver）；
         *   · 清单给了一个取密钥的地址（keyUrl）。
         *
         * URL 由调用方按值捕获进来（内层 demuxer 是每分片重建的），组合成绝对地址的
         * 那一步在装回调时就已经做过。kidHex 只用于"每个 KID 只问一次"的去重。
         *
         * 取密钥的实现共用 ContentKeyFetcher：**裸 16 字节 / 十六进制 JSON /
         * W3C ClearKey 许可证 JSON** 三种形状都认（ClearKey 的服务端返回的正是第三种）。
         * 失败**不静默**：打一条 ERROR 说清是"打不开"还是"形状不认识"。
         */
        bool fetchCencKey(const std::string &keyUrl, const std::string &kidHex, uint8_t *key, int *keySize);

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

        /*
         * 【按时间取"包含该时刻的那一片"的分片号】给切换清晰度的定位基准用。
         *
         * 语义与 DashStream::getSegmentNumByTime 相同（**包含**该时刻那一片），但实现细节不同：
         * HLS 走 SegmentTracker::getSegmentNumberByTime（SegmentTracker.h:50 / .cpp:623，
         * 最终落到 play_list/SegmentList.cpp:96），那个实现会**把第一个入参 time 就地改写成
         * 该片的 startTime**（SegmentList.cpp:115-118 的 `startTime + duration > time` 判据 + `time = i->startTime`）。
         * 关键：**num 拿到的仍是分片号**，startTime 只体现在被改写的入参上，不会混进 num
         * ⇒ 调用方拿到 num 之后 +1 就是"下一个分片"，不需要再做任何时间换算。
         *
         * **非虚 + 追加在 public 段末尾**：不动 vtable、不动既有成员偏移。
         * 返回 false 表示查不到（时间为空 / 分片表里没有覆盖该时刻的片）。
         */
        bool getSegmentNumByTime(int64_t timeUs, uint64_t &num);

    private:
        /*
         * ============ seek 落点延迟线（只对点播视频路生效）============
         *
         * 唯一一份实现在 demuxer/SeekLandingStage.h（原来 HLSStream 与 DashStream 各有一份同形
         * 拷贝，已删除、收敛成那一份）。完整说明 —— 含"精度权威在 renderer 的单一落点过滤，
         * 这里只是降低解码前推距离的尽力优化、失败只慢不错"这句定性 —— 见那里的类注释。
         * 一句话：HLS 也只能把 seek 定位到"包含目标的那一片"（10 秒一个独立文件），落点因此是
         * 分片片首；延迟线在**包**这一层把落点挪到"不晚于目标的最后一个关键帧"——它之前的包整体
         * 丢掉，之后的包按序交出。精度语义（包含目标、不晚于目标）一个字节不改。
         *
         * 三个引用是"放行时把包按原序交回 mQueue"需要的（锁与唤醒方式和 read_thread 推包一致）。
         * 本仓库约定：新成员一律追加在类末尾（中间插入会移动偏移、破坏增量构建）。
         */
        SeekLandingStage mSeekLanding{mQueue, mDataMutex, mWaitCond};

        /*
         * ============ key 拉取失败的失败语义（本轮新增）============
         *
         * 【要修的三件事，都是"失败被当成成功"的后半截】
         *   1. mKeyUrl 原来在 Open() **之前**就赋值：只要 key URL 没变，updateKey() 下一次
         *      直接 return false ⇒ **同一个 URL 永不重试**。而"密钥还没上传好/网络抖一下"
         *      恰恰是第一片最常见的失败。现在改成**只在真的拿到 16 字节之后才记住 URL**。
         *   2. 拉 key 失败时 updateSegDecrypter() 不再建 decrypter，但读段那条路
         *      （readSegment）会把**密文当明文**交给 demuxer ⇒ 花屏 + 一条通用 demux 错误，
         *      日志里**没有一句**"解密失败"。现在 readSegment 会明确报错并只打一条 ERROR。
         *   3. 失败原因要能区分：打不开（可能是暂时性）与长度不是 16（密钥内容错）是两回事。
         *
         * 【为什么不是"重试/兜底"】这里没有任何计时器、没有重试次数、没有超时：
         *   · "重试"= 下一次 updateKey() 自然会再试一次（因为 mKeyUrl 没被记住），
         *     它是**事件驱动**的（每次换分片/打开流都会走到这里）；
         *   · "保留上一个可用 key"= mSegDecrypter 与 mKey 在失败时**不被破坏**，
         *     上一片的 decrypter 仍然有效（多 key 轮换里换到坏 URL 时不会把好 key 冲掉）。
         */
        bool mKeyFetchFailedLogged{false};

        /*
         * ============ 把"上一片残留的解密状态"交还（本轮新增）============
         *
         * 这两个函数只服务一件事：**从密文片切回明文片时，不能让上一片的解密器/密钥 URL
         * 继续对明文片生效**。为什么必须是独立函数而不是塞进 updateDecrypter()：
         * updateDecrypter() 是按 mCurrentEncryption.method 分支的，而"本片是明文"这条路
         * （method == NONE）两个分支都不进 —— 在它里面够不到"清掉密文态"这个动作。
         * 调用点因此放在 createDemuxer() 里"已经确定本片一条加密记录都没接受"之后。
         *
         * 两个函数的分工：
         *   · clearDecrypterState()：**只在确实残留时才动**（纯明文片源里它是空操作），
         *     释放两个解密器并清掉 mKeyUrl（URL 是"这把 key 已经拉过"的记号，
         *     清掉之后下次遇到密文片会重新拉 —— 这是"解密器被重建"的必要条件，不是重试）。
         *   · releaseSampleAesDecrypter()：切换加密**方法**时清掉样本级解密器（见其调用点）。
         */
        void clearDecrypterState();

        void releaseSampleAesDecrypter();

        bool hasActiveDecrypter() const;

        /*
         * CENC 软解：取密钥成/败各只打一条日志（同 mKeyFetchFailedLogged 的做法），
         * 避免"每个包都问一次"变成刷屏。追加在类末尾。
         */
        bool mCencKeyLogged{false};
    };
}


#endif //FRAMEWORK_HLSSTREAM_H
