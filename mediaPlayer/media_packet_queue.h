#ifndef CICADA_MEDIA_BUFFER_CONTROL_H
#define CICADA_MEDIA_BUFFER_CONTROL_H

#include <mutex>
#include <deque>
#include <utils/AFMediaType.h>
#include <base/media/IAFPacket.h>

namespace Cicada {
    class MediaPacketQueue {
    public:
        MediaPacketQueue();

        ~MediaPacketQueue();

        typedef std::unique_ptr<IAFPacket> mediaPacket;

        void SetOnePacketDuration(int64_t duration);

        int64_t GetOnePacketDuration();

    public:
        void ClearQueue();

        void AddPacket(mediaPacket frame);

        std::unique_ptr<IAFPacket> getPacket();

        int GetSize();

        int64_t GetDuration();

        int64_t GetPts();

        int64_t GetKeyTimePositionBefore(int64_t pts);

        int64_t GetKeyTimePositionBeforeUTCTime(int64_t time);

        /*
         * 返回 pts **之后**（含 pts 本身）的第一个关键帧的 pts，找不到返回 INT64_MIN。
         * 只在**还没被读走**的区间（[mCurrent, end)）里找。
         *
         * 别和 GetFirstKeyPTS() 混：那个从队首扫到 mCurrent 就停，也就是只在
         * 已经交给解码器的那一段里找“pts <= 参数”的关键帧。
         *
         * 【当前状态（2026-09-23）】原来的周期性“丢包追赶”已从内核物理删除，
         * SuperMediaPlayer 不再有调用者；本接口作为公共 API 保留
         * （BufferController::GetFirstKeyPTSAfter() 仍转发到这里），
         * 下面 GetLastKeyPTSAtOrBefore() 里“追赶要用它”是当时的设计记录。
         */
        int64_t GetFirstKeyPTSAfter(int64_t pts);

        /*
         * pts **之前**（含 pts 本身）的最后一个关键帧 pts，只在 [mCurrent, end) 里找。
         *
         * 当时那次周期性“丢包追赶”要用它，而**不是** GetFirstKeyPTSAfter()：
         * 实测（2026-09-21 20:42 那份日志）里，视频一“落后”就去跳
         * “主时钟之后的第一个关键帧”，而那个关键帧往往在**下一个分片边界**上
         * （DASH 10s / HLS 4.2~12.5s 之后）。跳过去的结果是：视频被停在了时钟
         * **前面** 3~16 秒，RenderVideo() 认为“帧太早”不上屏，于是解码器一路空转、
         * 每秒再跳一次（37.5→41.7→45.9→50.0→54.2s…），gap 从 3.4 秒涨到 16.7 秒，
         * 画面永久冻死。正确落点是**不晚于主时钟**的最近关键帧。
         *
         * 【当前状态（2026-09-23）】那次周期性追赶已删除，本接口在内核里没有调用者，
         * 作为公共 API 保留（BufferController::GetLastKeyPTSAtOrBefore() 转发到这里）。
         */
        int64_t GetLastKeyPTSAtOrBefore(int64_t pts);

        int64_t GetFirstKeyPTS(int64_t pts);

        int64_t GetLastKeyTimePos();

        int64_t ClearPacketBeforeTimePos(int64_t pts);

        int64_t ClearPacketBeforePTS(int64_t pts);

        /*
         * 一次性丢弃某个 streamIndex 的**全部**包（不看 pts），返回丢掉的个数。
         *
         * 清晰度切换提交后，退役的旧 Representation 可能已经在公共视频队列里
         * 堆了几百上千个包。只靠解码循环“发现一个是旧的、丢一个”去清，速率上限
         * 就是主循环频率（实测约 82 个/秒）—— 日志里那段
         * `[flood] drop stale video packet after quality switch: 82 more lines suppressed`
         * 连续刷了 6 秒以上，期间排在它们后面的**新** Representation 的包一个都
         * 轮不到解码，画面就是 0 FPS（HLS 那次切换明明 READY 成功了还是冻住）。
         *
         * 语义上这些包在新 active 路上已经是垃圾（codec/分辨率/时间轴都可能不同），
         * 一起删掉和逐个删掉等价，只是快了几个数量级。
         */
        int DropPacketsByStream(int streamIndex);

        void Rewind();

        int64_t GetLastTimePos();

        int64_t GetFirstTimePos();

        int64_t GetLastPTS();

        int64_t FindSeamlessPointTimePosition(int &count);

        void ClearPacketAfterTimePosition(int64_t pts);

        void SetMaxBackwardDuration(uint64_t duration)
        {
            mMAXBackwardDuration = duration;
        }

        int mMediaType = 0;

    private:
        void PopFrontPacket();

    private:
        std::list<mediaPacket> mQueue;
        std::list<mediaPacket>::iterator mCurrent;
        std::recursive_mutex mMutex;
        int64_t mPacketDuration = 0;
        int64_t mDuration = 0;
        int64_t mTotalDuration = 0;
        uint64_t mMAXBackwardDuration{0};

        uint8_t *mDropedExtra_data{nullptr};
        int mDropedExtra_data_size{0};
    };

} // namespace Cicada
#endif // CICADA_MEDIA_BUFFER_CONTROL_H
