//
// Created by moqi on 2018/4/27.
//

#ifndef FRAMEWORK_PLAYLISTMANAGER_H
#define FRAMEWORK_PLAYLISTMANAGER_H

#include <demuxer/demuxer_service.h>
#include "playList.h"
#include "utils/AFMediaType.h"
#include <memory>
#include <atomic>
#include <base/OptionOwner.h>

namespace Cicada{
    class PlaylistManager : public OptionOwner {
    public:
        explicit PlaylistManager(playList *pList);

        virtual ~PlaylistManager();

        virtual int init() = 0;

        virtual int start() = 0;

        virtual void stop() = 0;

        virtual void preStop() = 0;

        virtual int GetNbStreams() const = 0;

        virtual int GetStreamMeta(Stream_meta *meta, int index, bool sub) const = 0;

        virtual int ReadPacket(std::unique_ptr<IAFPacket> &, int index) = 0;

        virtual int OpenStream(int id) = 0;

        virtual void CloseStream(int id) = 0;

        virtual int64_t seek(int64_t us, int flags, int index) = 0;

        virtual int SwitchStreamAligned(int from, int to) = 0;

        virtual int getNBSubStream(int index) const = 0;

        virtual void interrupt(int inter) = 0;

        virtual const std::string GetProperty(int index, const string &key) = 0;

        virtual int GetRemainSegmentCount(int index) = 0;

        virtual void setExtDataSource(IDataSource *source)
        {
            mExtDataSource = source;
        }

        virtual void setDataSourceConfig(const IDataSource::SourceConfig &config)
        {
            mSourceConfig = config;
        }

        virtual void setBitStreamFormat(header_type vMergeHeader, header_type aMergeHeader)
        {
            mMergeVideoHeader = vMergeHeader;
            mMergerAudioHeader = aMergeHeader;
        }
        
        virtual bool isRealTimeStream(int index) = 0;
        virtual bool isWallclockTimeSyncStream(int index)
        {
            return false;
        }
        virtual int64_t getDurationToStartStream(int index)
        {
            return 0;
        }

        virtual int64_t getTargetDuration() = 0;

        virtual int64_t getBufferDuration(int index) const = 0;

        virtual void setUrlToUniqueIdCallback(UrlHashCB cb, void *userData)
        {
            mUrlHashCb = cb;
            mUrlHashCbUserData = userData;
        }

        virtual UTCTimer *getUTCTimer()
        {
            return nullptr;
        }

        virtual void setClientBufferLevel(client_buffer_level level)
        {}
        virtual void preferAudio(bool prefer)
        {}

        /*
         * ============ 【切换清晰度的定位基准：播放头节目时间】============
         *
         * 由播放器在"提交切换的那一刻"经 IDemuxer::SetOption("alignedSwitchPlayheadUs", us)
         * 送进来，边界块用它算"**播放头所在分片的下一个分片头**"：
         *   k = 目标流自己的分片表里**包含**该时刻的那一片，目标流起点 = k + 1。
         *
         * 【为什么是**进程内共享槽**、不是本实例的成员】
         *
         * 真机日志证明"键送到的那一个实例"与"跑边界块的那一个实例"不一定相同：
         *   · playList_demuxer.cpp:60-64 自己 new HLSManager/DashManager，:75 记进
         *     mPPlaylistManager（DASH 点播这条链的边界块就在这里跑）；
         *   · ManifestDemuxer.cpp:612 又 new 了**另一个** HLSManager；
         *   · demuxer_service.cpp:470-471 是无条件转发，但转发的是它持有的那一个 IDemuxer。
         * 值写进 A、边界块在 B 上读 ⇒ 读到 INT64_MIN ⇒ 走退化支（真机就是这条：
         * `set SegNum to 4` + FALLING BACK，按播放头定位那一支从未命中）。
         *
         * 所以值放在**进程内共享槽**：无论 SetOption 落到哪个 manager、边界块在哪个 manager
         * 上跑，读到的都是同一个值。写（播放器线程）与读（解复用层读线程）都用 std::atomic。
         * 它不是开关：是一个**每次新切换都被覆盖**的带值量；写播放头的同时把上一次的结果清零。
         * 进程内单槽的取舍与 platform/QtPlayer 的 g_decoderGenerationEndedHook 同源
         * （同一时刻只有一个播放器在切档）。
         */
        void setAlignedSwitchPlayhead(int64_t us)
        {
            sharedPlayhead().store(us);
            sharedResult().store(0);
        }

        int64_t getAlignedSwitchPlayheadUs() const
        {
            return sharedPlayhead().load();
        }

        /*
         * 【切换结果】0 = 正常（尚未失败）；< 0 = 边界块判定"这一片换不过去"
         * （例如点播末尾 k+1 不存在、SetCurSegNum 返回负）。同样走共享槽 —— 写的那个实例
         * 不一定等于读的那个实例。纯状态量，无计时器。
         */
        void setAlignedSwitchResult(int64_t result)
        {
            sharedResult().store(result);
        }

        int64_t getAlignedSwitchResult() const
        {
            return sharedResult().load();
        }

    protected:
        playList *mPList = nullptr;
        IDataSource *mExtDataSource = nullptr;
        IDataSource::SourceConfig mSourceConfig{};
        header_type mMergeVideoHeader = header_type::header_type_no_touch;
        header_type mMergerAudioHeader = header_type::header_type_no_touch;
        UrlHashCB mUrlHashCb{nullptr};
        void *mUrlHashCbUserData{nullptr};

        /*
         * 【待按播放头定位的目标流号】-1 = 没有待办（带值状态，不是布尔开关）。
         *
         * 为什么需要它：目标流的分片表/Representation 是在**该流自己被读**的时候才建立的
         * （真机：播放头 1.067 s 本该落在第一片 0~6 s 内，却在 OpenStream 之后仍报
         * "NOT covered by the target stream's segment table"）。所以"算 k + SetCurSegNum(k+1)"
         * 必须推迟到该流产出**第一个包**那一刻 —— 由两个 manager 的读路径识别（`ret > 0` 分支），
         * 判据就是这个流号。无计时器、无重试。
         * 放基类是因为边界块与读路径在同一个 manager 实例里；追加在成员区末尾。
         */
        int mPendingAlignedSwitchStream{-1};

    private:
        /*
         * 进程内共享槽（函数内静态 ⇒ 每进程一份，C++11 起初始化线程安全）。
         * 为什么不用本类的数据成员：见 setAlignedSwitchPlayhead 上面那段 ——
         * 写值的那一个实例不一定是跑边界块的那一个实例。
         */
        static std::atomic<int64_t> &sharedPlayhead()
        {
            static std::atomic<int64_t> value{INT64_MIN};
            return value;
        }

        static std::atomic<int64_t> &sharedResult()
        {
            static std::atomic<int64_t> value{0};
            return value;
        }
    };
}


#endif //FRAMEWORK_PLAYLISTMANAGER_H
