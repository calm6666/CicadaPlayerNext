#ifndef CICADA_BUFFER_CONTROLLER_H
#define CICADA_BUFFER_CONTROLLER_H

#include <deque>
#include <mutex>

#include "media_packet_queue.h"

namespace Cicada {

    typedef enum BUFFER_TYPE {
        BUFFER_TYPE_VIDEO = 1,
        BUFFER_TYPE_AUDIO = (1 << 1),
        BUFFER_TYPE_SUBTITLE = (1 << 2),

        BUFFER_TYPE_AV = (BUFFER_TYPE_VIDEO | BUFFER_TYPE_AUDIO),
        BUFFER_TYPE_ALL = (BUFFER_TYPE_VIDEO | BUFFER_TYPE_AUDIO | BUFFER_TYPE_SUBTITLE),
    } BUFFER_TYPE;

    class BufferController {
    public:
        BufferController();

        ~BufferController();

        int64_t GetPacketDuration(BUFFER_TYPE type);

        void SetOnePacketDuration(BUFFER_TYPE type, int64_t duration);

        int64_t GetOnePacketDuration(BUFFER_TYPE type);

        int GetPacketSize(BUFFER_TYPE type);

        bool IsPacketEmtpy(BUFFER_TYPE type);

        std::unique_ptr<IAFPacket> getPacket(BUFFER_TYPE type);

        void AddPacket(std::unique_ptr<IAFPacket> packet, BUFFER_TYPE type);

        void ClearPacket(BUFFER_TYPE type);

        int64_t GetPacketPts(BUFFER_TYPE type);

        int64_t GetPacketLastKeyTimePos(BUFFER_TYPE type);

        int64_t ClearPacketBeforeTimePos(BUFFER_TYPE type, int64_t pts);

        int64_t ClearPacketBeforePts(BUFFER_TYPE type, int64_t pts);

        /*
         * 一次性丢掉某个 streamIndex 的全部包（见 MediaPacketQueue::DropPacketsByStream）。
         * 清晰度切换提交后用它把退役 Representation 的残留包一次清掉，避免解码循环
         * 以主循环频率（实测 ~82 个/秒）逐个丢，把新路的包饿死。
         */
        int DropPacketsByStream(BUFFER_TYPE type, int streamIndex);

        int64_t GetKeyTimePositionBefore(BUFFER_TYPE type, int64_t pts);

        int64_t GetKeyTimePositionBeforeUtcTime(BUFFER_TYPE type, int64_t time);

        void Rewind(BUFFER_TYPE type);

        int64_t GetFirstKeyPTS(BUFFER_TYPE type, int64_t pts);

        /*
         * pts 之后（含）的第一个关键帧。只在还没被读走的区间里找；
         * 和 GetFirstKeyPTS() 不是一回事，后者给的是已经交给解码器那一段里
         * “之前/当前”的关键帧。
         * 【当前状态（2026-09-23）】原来的周期性“丢包追赶”已删除，内核里没有调用者，
         * 作为公共 API 保留。
         */
        int64_t GetFirstKeyPTSAfter(BUFFER_TYPE type, int64_t pts);

        /*
         * pts 之前（含）的最后一个关键帧 pts。当时那次周期性“丢包追赶”要用它，
         * 而不是 GetFirstKeyPTSAfter()。见 media_packet_queue.h 里的详细说明：
         * 跳到“主时钟之后的第一个关键帧”会把画面停在时钟**前面**，反而永久冻死。
         * 【当前状态（2026-09-23）】那套追赶已删除，本接口在内核里没有调用者，
         * 作为公共 API 保留。
         */
        int64_t GetLastKeyPTSAtOrBefore(BUFFER_TYPE type, int64_t pts);

        int64_t GetPacketLastTimePos(BUFFER_TYPE type);

        int64_t GetPacketFirstTimePos(BUFFER_TYPE type);

        int64_t GetPacketLastPTS(BUFFER_TYPE type);

        int64_t FindSeamlessPointTimePosition(BUFFER_TYPE type, int &count);

        void ClearPacketAfterTimePosition(BUFFER_TYPE type, int64_t pts);

        void SetMaxBackwardDuration(BUFFER_TYPE type, uint64_t duration);

    private:
        MediaPacketQueue mVideoPacketQueue;
        MediaPacketQueue mAudioPacketQueue;
        MediaPacketQueue mSubtitlePacketQueue;
    };

}// namespace Cicada
#endif// CICADA_BUFFER_CONTROLLER_H
