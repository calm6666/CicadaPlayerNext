//
// Created by lifujun on 2019/8/29.
//

#ifndef SOURCE_AUDIOTRACKRENDER_H
#define SOURCE_AUDIOTRACKRENDER_H


#include "../audioRenderPrototype.h"
#include "AaudioRender.h"
#include <atomic>
#include <base/media/spsc_queue.h>
#include <jni.h>
#include <render/audio/filterAudioRender.h>
#include <utils/afThread.h>

class AudioTrackRender :
        public Cicada::filterAudioRender,
        private audioRenderPrototype {
public:
    AudioTrackRender();

    ~AudioTrackRender();

private:
    bool device_require_format(const IAFFrame::audioInfo &info) override;

    int init_device() override;

    int pause_device() override;

    int start_device() override;

    void flush_device() override;

    void device_setVolume(float gain) override;

    void device_mute(bool bMute) override;

    int64_t device_get_position() override;

    int device_write(unique_ptr<IAFFrame> &frame) override;

    uint64_t device_get_que_duration() override;

    uint64_t device_get_ability() override
    {
        return A_FILTER_FLAG_VOLUME;
    }

    void device_preClose() override;

private:
    AudioTrackRender(int dummy) : mFrameQueue(1)
    {
        addPrototype(this);
    }

    /*
     * ============ 【AAudio 优先：默认 AAudio、能力不可用自动回退 AudioTrack】============
     *
     * audioRenderPrototype::create() 取"注册顺序中第一个 is_supported 为真的实现"
     * （audioRenderPrototype.cpp:15-21），而注册发生在各 .cpp 的静态对象构造里
     * （本文件的 AudioTrackRender::se 与 AaudioRender.cpp 的 AaudioRender::se）。
     * **两个不同翻译单元的静态对象构造顺序是未定义的**（静态初始化顺序问题），
     * 所以"把 AAudio 写在前面"这件事靠注册顺序无法保证。
     *
     * 这里改用**能力互斥**表达同一条产品要求，与顺序完全无关：
     *   · AAudio 可用（libaaudio.so 能 dlopen 且必需符号齐全）⇒ 本实现返回 false，
     *     create() 无论先注册谁都会选中 AaudioRender；
     *   · AAudio 不可用（API < 26 / 符号缺失）⇒ 返回 true，AudioTrack 接管 = 回退。
     * 这不是配置开关，判据只是"这台设备有没有这个能力"（AaudioRender::isAvailable()）。
     *
     * 顺带解决一个链接期问题：这个调用使 AaudioRender.cpp 成为 AudioTrackRender.cpp 的
     * 必需依赖 —— 静态库在把 AudioTrackRender.o 拉进来时**一定**也会把 AaudioRender.o 拉进来。
     * 否则仅靠"静态对象自注册"，AaudioRender.o 有可能被链接器整个丢掉（没有任何符号被引用），
     * AAudio 就会静默地永远不被注册，而那不会有任何编译/链接错误提示。
     */
    bool is_supported(AFCodecID codec) override
    {
        (void) codec;
        return !AaudioRender::isAvailable();
    }

    Cicada::IAudioRender *clone() override
    {
        return new AudioTrackRender();
    }

    int device_write_internal(IAFFrame *frame);

    static AudioTrackRender se;

private:

    void adjustOutputInfo();

    int init_jni();

    uint64_t getDevicePlayedSimples();

    uint64_t getDeviceQuequedDuration();

    void flush_device_inner(bool clearFrameQueue);

    int write_loop();

private:

    jobject audio_track{nullptr};
    jmethodID method_write{nullptr};
    jmethodID method_play{nullptr};
    jmethodID method_pause{nullptr};
    jmethodID method_flush{nullptr};
    jmethodID method_stop{nullptr};
    jmethodID method_release{nullptr};
    jmethodID method_setVolume{nullptr};
    jmethodID method_getPlayState{nullptr};
    jmethodID method_getPosition{nullptr};


private:
    uint8_t *mPcmBuffer = nullptr;
    int64_t mPcmBufferLen = 0;

    int64_t mPlayedBufferLen = 0;
    std::atomic<uint64_t> mAudioFlushPosition{0};

    std::atomic<uint64_t> mSendSimples{0};

    std::atomic<uint64_t> mOverFlowPlayedSimples{0};
    int jBufferLen{0};
    jobject jbuffer{nullptr};

    Cicada::SpscQueue<IAFFrame *> mFrameQueue;
    afThread *mWriteThread{nullptr};
    std::atomic_bool mRunning{false};
    // 渲染队列的初始深度：与内核的音频缓冲下限一致（ffplay 的音频 PCM 队列也是 9 帧）。
    // 原来初始/复位都是 2，起播与每次 seek 之后只有约 40ms 余量，一抖动就断供。
    int mMaxQueSize{9};
    float mVolume{1.0};
    bool mMute{false};
    /*
     * 设备侧"静音窗口"状态，只用于起止各一条的日志（与内核侧 AUDIO_SILENCE_* 同编码）：
     *   0 有声；2 设备写失败/短写；3 设备被 flush（等数据重新灌满）。
     * 判据只有状态变化，没有时间阈值；本成员不参与任何播放决策。
     */
    int mSilenceReason{0};
    void logSilence(int reason, const char *detail);
    std::unique_ptr<IAFFrame::audioInfo> mRequireFormat{};
    /*
     * ============ B 方案：静音保活（Android 音频写线程）============
     *
     * 目的：没有 PCM 可写时不要**沉默地空转** —— 那会让 AudioTrack 超过约 1 秒没有
     * 写入而判 underrun：实测 14:34:52.214 `onAudioException -1003` →
     * `baseTimeout()` → `baseStop()`，音轨被停掉，直到下一次写入才 restartIfDisabled
     * 恢复，用户听到 1.45~1.6 秒的硬静音（视频侧同时还有数据）。
     * 做法：无 PCM 时按**当前 AudioTrack 完全相同的格式**写一小段静音，把音轨"喂"住，
     * 真实 PCM 一到立刻接上。真实数据洞并没有被消除，只是不再被框架放大成停轨。
     *
     * 记账（关键，见 .cpp 里 writeKeepAliveSilence() 的等式证明）：
     *   静音样本**不计入** mSendSimples（它代表媒体样本），而是等量加进
     *   mAudioFlushPosition —— device_get_position() = over + playedDevice - flush，
     *   因此音频时钟在保活期间**不前进**，A/V 不会漂移。
     *
     * 成员全部追加在成员列表末尾（本工程有增量构建偏移契约）。
     * jSilenceBuffer 是懒创建的 JNI 全局引用，NewByteArray 保证内容全 0，因此可以
     * 一直复用（不写它就不会被污染），避免每次分配。
     */
    jobject jSilenceBuffer{nullptr};
    int jSilenceBufferLen{0};
    /* 本回合已写出的静音帧数；真实 PCM 写成功即清零。 */
    int mKeepAliveSilenceWrites{0};
    /* 只在"开始/结束"两个状态点上各打一条日志。 */
    bool mKeepAliveActive{false};
    /* 到达资源上界后置真，停止填充，直到真实 PCM 回来（资源保护，不是判死）。 */
    bool mKeepAliveCapped{false};
    /* 无 PCM 时尝试写一段静音保活；返回 false 表示不满足前提（调用方按原样空转）。 */
    bool writeKeepAliveSilence();
};


#endif //SOURCE_AUDIOTRACKRENDER_H
