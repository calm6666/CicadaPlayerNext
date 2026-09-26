#ifndef QU_ANDROID_H264_DECODER_HH
#define QU_ANDROID_H264_DECODER_HH

#include <cstdio>
#include <cstdint>
#include <list>
#include <atomic>
#include <memory>
#include <mutex>
#include <jni.h>
#include <thread>
#include <condition_variable>
#include <codec/IDecoder.h>
#include <utils/afThread.h>
#include <base/media/AVAFPacket.h>
#include <queue>
#include <set>

#include <base/media/AFMediaCodecFrame.h>
#include <drm/WideVineDrmHandler.h>
#include "codec/ActiveDecoder.h"
#include "../codecPrototype.h"
#include "jni/MediaCodec_Decoder.h"


#define CODEC_VIDEO (0)
#define CODEC_AUDIO (1)

namespace Cicada{
    class mediaCodecDecoder : public ActiveDecoder, private codecPrototype {
    public:
        mediaCodecDecoder();

        ~mediaCodecDecoder() override;

    private:

        int init_decoder(const Stream_meta *meta, void *wnd, uint64_t flags, const DrmInfo *drmInfo) override;

        void close_decoder() override;

        int enqueue_decoder(std::unique_ptr<IAFPacket> &pPacket) override;

        int dequeue_decoder(std::unique_ptr<IAFFrame> &pFrame) override;

        void flush_decoder() override;

        int get_decoder_recover_size() override
        {
            return 0;
        };
        void decoder_updateMetaData(const Stream_meta *meta) override{};

        int setOutputSurface(void *surface) override;

        int64_t getLastRenderedVideoPts() override;

        /*
         * 【追帧加速】seek / 切档预滚窗口里让 codec 跑更高的性能点（不丢帧、不改时间轴）。
         * 追加在类末尾，保证增量 ABI 安全；语义见 IDecoder::setDecodeBoost。
         */
        int setDecodeBoost(bool boost) override;

        int64_t getLastRenderedVideoFrameDur() override;

        void setRenderGate(int64_t gatePts) override;

        bool isRenderGateHit() override;

    private:
        static bool checkSupport(const Stream_meta &meta, uint64_t flags, int maxSize);

        void updateCSD(const Stream_meta *meta, const uint8_t *extradata, int extradata_size);

        int initDrmHandler();

        void releaseDecoder();

        int configDecoder();

    private:
        explicit mediaCodecDecoder(int dummy)
        {
            addPrototype(this);
        };

        mediaCodecDecoder *clone() override
        {
            return new mediaCodecDecoder();
        };

        bool is_supported(const Stream_meta &meta, uint64_t flags, int maxSize) override
        {
            if (flags & DECFLAG_HW)
                return checkSupport(meta, flags, maxSize);
            return false;
        };

        bool is_drmSupport(const DrmInfo *drmInfo) override {
            if(drmInfo == nullptr){
                return false;
            }

            bool drmSupport = drmInfo->format == "urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed"
                               && DrmHandlerPrototype::isSupport(drmInfo);
            return drmSupport;
        }

        static mediaCodecDecoder se;

        bool supportReuse() override;

    private:
        int width{0};
        int height{0};
        int channel_count{0};
        int sample_rate{0};
        int format{0};

        int codecType = CODEC_VIDEO;
        std::string mMime{};
        std::list<std::unique_ptr<CodecSpecificData>> mCSDList{};
        MediaCodec_Decoder *mDecoder{nullptr};

        /*
         * ============ 【B5-4】帧释放回调的共享状态（不再捕获裸 this）============
         *
         * 两个真机问题都由"帧的释放回调"引起：
         *   ① `MediaCodecDecoder E releaseOutputBuffer fail Error 0xfffffff3`
         *      （2026-09-25 11:02:56.763，一次 seek 之后 7 ms）：`flush_decoder()` 里的
         *      `mDecoder->flush()` 之后平台收回了 codec 自持的缓冲，而内核手里的
         *      `AFMediaCodecFrame` 的 index 从此失效；这些帧要等渲染线程**下一次 VSync**
         *      才被析构（GLRender / AFActiveVideoRender 的输入队列是异步清的），
         *      于是必然拿着失效 index 去 release，白刷一条 E。
         *   ② 更严重的是 use-after-free：回调原来是 `[this, framePts](...)`，而
         *      `~mediaCodecDecoder` 会 `delete mDecoder`（已经提交切换的 retired
         *      解码器是在播放线程**同步**销毁的）。旧代帧仍压在渲染器队列里等下一次
         *      VSync ⇒ 回调在**已释放的对象**上读成员并调用 mDecoder。
         *
         * 处置（只动本文件、只动 Android 后端）：
         *   · 把"解码器对象还在不在 / 这是第几代"放进下面这个 shared_ptr 共享状态；
         *     帧的回调**按值持有这个 shared_ptr** —— 解码器先死也不会悬垂（状态本身
         *     由 shared_ptr 保命）；
         *   · 回调在同一把锁内先校验：`!alive || decoder == nullptr`（对象正在/已经销毁）
         *     或 `gen != flushGen`（平台已 flush/close，index 已失效）⇒ 直接短路，
         *     **不碰任何成员、也不进 Java**；
         *   · 析构时**在锁内**置 alive=false 并销毁 mDecoder，与回调临界区互斥 ⇒
         *     "销毁"与"释放"被串行化，UAF 窗口关闭。
         *
         * 影响面：本文件是 Android 专用后端；Qt / macOS / Windows / Linux / iOS 的
         * 解码器实现一行都不动，接口与 ABI 无变化（新增的只是一个私有成员）。
         * 回退点：把本结构与 member 删掉、恢复 lambda 捕获裸 this、去掉析构/两处
         * invalidateFrameReleases() 调用即可。
         */
        struct FrameReleaseState {
            std::mutex mutex;
            MediaCodec_Decoder *decoder{nullptr};
            bool alive{true};
            /* flush / close 会让所有"已出队未释放"的帧失效：每失效一次就 +1，
             * 帧在创建时快照这个代。用原子量是为了让"创建帧"这条热路径不必加锁。 */
            std::atomic<uint64_t> flushGen{0};
        };
        std::shared_ptr<FrameReleaseState> mReleaseState;

        /* 让所有"已出队但还没释放"的帧立刻失效（平台 flush / 关闭解码器时调用）。 */
        void invalidateFrameReleases();

        std::recursive_mutex mFuncEntryMutex;
        bool mbInit{false};

        int mInputFrameCount{0};
        int mInputTryAgainCount{0};
        int mOutputFrameCount{0};
        bool mThrowFrame{false};
        bool mUseNdk{false};

        std::mutex mFlushInterruptMuex;
        int mFlushInterrupt{false};

        // mDecoder->flush() state  0: stop  1: flushed 2:Running 3: end-of-stream
        volatile int mFlushState{0};

        std::set<int64_t> mDiscardPTSSet;

        // 暂停帧恢复（surface 重建后精确恢复"暂停的那一帧"）：
        // 追踪最近一次实际渲染帧的 PTS/帧间隔，setRenderGate 设置渲染门
        std::atomic<int64_t> mLastRenderedVideoPts{INT64_MIN};
        std::atomic<int64_t> mPrevRenderedVideoPts{INT64_MIN};
        std::atomic<int64_t> mRenderGatePts{INT64_MIN};
        std::atomic<bool> mRenderHold{false};
        std::atomic<bool> mRenderGateHit{false};

        Stream_meta mMeta{};
        void* mVideoOutObser = nullptr;
        int naluLengthSize = 0;
        bool isADTS = false;

        std::shared_ptr<WideVineDrmHandler> mDrmHandler{nullptr};
    };
}

#endif // QU_ANDROID_H264_DECODER_HH
