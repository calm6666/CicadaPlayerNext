//
// NDK 异步绑定：AMediaCodec + AMediaCodec_setAsyncNotifyCallback（API 28+，弱符号）。
//
// 为什么是异步（设计见 docs/ANDROID-NDK-ASYNC-DECODER.md）：
//   · 同步模式（dequeue* 轮询）会引入"轮询周期"延迟，并且要自己交错 in/out 防互相阻塞；
//   · 异步模式下缓冲"可用"由平台推给我们，数据面全部在 C++ 内完成：
//       - 输入：AMediaCodec_getInputBuffer() 直接给 native 指针（不再 NewByteArray 到 Java 堆）；
//       - 输出：surface 模式 AMediaCodec_releaseOutputBuffer(idx, true) 直出，音频取直接指针；
//     ⇒ 每帧 0 次 JNI、0 次 Java 堆分配、0 次多余拷贝。
//
// 三条不变量（都是真机踩过的坑，见文档 §四.4 与 §六）：
//   1. flush 之后必须 start()：异步模式下平台在 flush 后不会再回调输入缓冲；
//   2. 回调线程只入队 + notify，绝不拷贝帧数据/构造帧/释放缓冲/打重日志（平台明文禁止重活）；
//   3. 永远至少留 1 个输入 index 不占用（防 codec 无输入可用而停滞，表现与"没有回调"一样难查）。
//

#ifndef CICADA_ANDROID_NDK_CODEC_BINDING_H_
#define CICADA_ANDROID_NDK_CODEC_BINDING_H_

#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <media/NdkMediaCodec.h>

#include "codecBinding.h"

namespace Cicada {

    class NdkCodecBinding : public IAndroidCodecBinding {
    public:
        /*
         * 控制面：codec **名**由 Java 侧选择（同一份 MediaCodecList 顺序、同一份黑名单、
         * 同一份 secure 判定），本类用 AMediaCodec_createCodecByName(name) 精确锁定同一颗
         * —— 按名字创建自 API 21 就有，minSdk 24 下无需弱符号。
         * 注入方式：调度器（codecBinding.cpp）把 Java 绑定作为控制面传进来。
         * 返回空串 = 选择失败（本绑定 configure 失败，由调度器回落 Java 绑定）。
         */
        using CodecNameProvider = std::function<std::string(bool isVideo, const std::string &mime,
                                                            int width, int height, int sampleRate,
                                                            int channelCount, int isADTS)>;

        NdkCodecBinding();
        ~NdkCodecBinding() override;

        /* libmediandk 是否提供异步回调注册（API 28+）。纯弱符号判空，无版本号解析。 */
        static bool isAvailable();

        /*
         * 进程内一次性熔断：本设备上 NDK 创建/配置失败过，就不再重试。
         * 语义是"平台能力"而非"配置开关"：调度器据此把整个实例交给 Java 异步绑定
         * （两者都是异步，本设计没有同步轮询路径）。
         */
        static void markUnavailable(const char *why);
        static bool isMarkedUnavailable();
        static const char *unavailableReason();

        void setCodecNameProvider(CodecNameProvider provider);

        /*
         * 控制面：1x1 DummySurface（Java 的 ensureDummySurface()）。NDK 在"无真 surface /
         * 占位面（切档 B2）"时用它维持 surface 模式 —— 与 Java 路径的做法、乃至**同一块**
         * dummy surface 都一致；拿不到就 configure 失败，由调度器整体回落 Java 绑定。
         */
        using DummySurfaceProvider = std::function<void *()>;
        void setDummySurfaceProvider(DummySurfaceProvider provider);

        // ---------------- IAndroidCodecBinding ----------------
        void setCodecSpecificData(const std::list<std::unique_ptr<CodecSpecificData>> &csds) override;

        /* secure 内容需要 Java MediaCrypto 会话 ⇒ 本绑定不支持，返回 MC_ERROR，
         * 由调度器把整个实例交给 Java 绑定（见 codecBinding.cpp 的路由说明）。 */
        int setDrmInfo(const std::string &uuid, const void *sessionId, int size) override;

        void setForceInsecureDecoder(bool force) override;

        int setDecodeBoost(bool boost) override;

        int configureVideo(const std::string &mime, int width, int height, int angle, void *surface,
                           bool usePlaceholderSurface) override;

        int configureAudio(const std::string &mime, int sampleRate, int channelCount,
                           int isADTS) override;

        int setOutputSurface(void *surface) override;

        int start() override;

        /* flush + 清空本地队列 + start（不变量 1 写在这一个函数里） */
        int flush() override;

        int stop() override;

        int release() override;

        int dequeueInputBufferIndex(int64_t timeoutUs) override;

        int queueInputBuffer(int index, void *buffer, size_t size, int64_t pts,
                             bool isConfig) override;

        int queueSecureInputBuffer(int index, void *buffer, size_t size,
                                   IAFPacket::EncryptionInfo *pEncryptionInfo, int64_t pts,
                                   bool isConfig) override;

        int dequeueOutputBufferIndex(int64_t timeoutUs) override;

        int getOutput(int index, mc_out *out, bool readBuffer) override;

        int releaseOutputBuffer(int index, bool render) override;

        const char *bindingName() const override { return "ndk-async"; }

    private:
        struct OutputEntry {
            int32_t index;
            int64_t ptsUs;
            uint32_t flags;
            int32_t size;
        };

        struct CsdEntry {
            std::string key;
            std::vector<uint8_t> data;
        };

        /* 平台回调（同一个内部线程）：只入队 + notify。 */
        static void onInputAvailable(AMediaCodec *codec, void *userdata, int32_t index);
        static void onOutputAvailable(AMediaCodec *codec, void *userdata, int32_t index,
                                      AMediaCodecBufferInfo *info);
        static void onFormatChanged(AMediaCodec *codec, void *userdata, AMediaFormat *format);
        static void onError(AMediaCodec *codec, void *userdata, media_status_t error,
                            int32_t actionCode, const char *detail);

        int applyCsdToFormat(AMediaFormat *format);
        ANativeWindow *toNativeWindow(void *javaSurface);
        void releaseNativeWindow();
        void resetQueuesLocked();
        bool waitForInput(int64_t timeoutUs);

        AMediaCodec *mCodec{nullptr};
        AMediaFormat *mFormat{nullptr};
        ANativeWindow *mWindow{nullptr};
        std::string mMime{};
        bool mVideo{true};
        bool mStarted{false};
        std::vector<CsdEntry> mCsd{};

        /* 输出格式（onFormatChanged 收到后存下来，getOutput 在 FORMAT_CHANGED 分支读它） */
        int mOutWidth{0};
        int mOutHeight{0};
        int mCropLeft{-1};
        int mCropRight{-1};
        int mCropTop{-1};
        int mCropBottom{-1};
        int mOutSampleRate{0};
        int mOutChannels{0};
        int mOutAudioFormat{2};   /* 2 = S16，与 Java 侧 MediaFormat 的编码值一致 */

        std::mutex mQueueMutex;
        std::condition_variable mQueueCond;
        std::deque<int32_t> mInputIndices;
        std::deque<OutputEntry> mOutputIndices;
        /* 已出队、尚未 getOutput/release 的输出：getOutput 按 index 取 pts/size/flags
         * （与 Java 侧 mOutputBufferInfos 同构；releaseOutputBuffer 时删除）。 */
        std::map<int32_t, OutputEntry> mPendingOutputs;
        /* 最近一次取出的输入 index（getOutput/dequeue 的调用序列是"取出→立刻喂回"）。 */
        int32_t mPendingInputIndex{-1};
        bool mFormatChangedPending{false};

        /* 防饿死：codec 手里的输入 index 总数（历史最大）与"已取出未喂回"的数量。 */
        int mTotalInputIndices{0};
        int mOutstandingInput{0};

        /* 错误：onError 只记录，由 dequeueOutputBufferIndex 上报出去（不在回调里做动作）。 */
        bool mErrorPending{false};
        bool mErrorFatal{false};
        int mErrorStatus{0};
        int mErrorActionCode{0};

        CodecNameProvider mNameProvider;
        DummySurfaceProvider mDummySurfaceProvider;
    };

} // namespace Cicada

#endif // CICADA_ANDROID_NDK_CODEC_BINDING_H_
