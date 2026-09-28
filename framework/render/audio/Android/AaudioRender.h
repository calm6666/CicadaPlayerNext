//
// Android AAudio 音频渲染器（默认走它；能力不可用时自动回退 AudioTrackRender）。
//
// 方案与依据见 docs/ANDROID-AAUDIO-RENDER.md：本文件是**自研实现**，
// 只参考了 mpv `audio/out/ao_aaudio.c` 的**行为与公开 C API 用法**（mpv 主体是 GPLv2+，
// 不逐字拷贝它的代码）。
//

#ifndef SOURCE_AAUDIORENDER_H
#define SOURCE_AAUDIORENDER_H

#include "../audioRenderPrototype.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <render/audio/filterAudioRender.h>

/*
 * AAudio 的 C 句柄只做**前向声明**：本工程 minSdk 24，而 AAudio 是 API 26+ 的库，
 * 因此头文件里绝不 include <aaudio/AAudio.h>、也绝不直接调用任何 AAudio 符号 ——
 * 全部符号在 .cpp 里 dlopen("libaaudio.so") + dlsym 解析（见 AaudioRender.cpp 的 loadApi）。
 * 这里用前向声明只是为了让成员指针有类型，不产生任何对 AAudio 的链接依赖。
 */
struct AAudioStream;
struct AAudioStreamBuilder;

/*
 * AAudio 数据回调的返回枚举。放在头文件里只因为**声明**里要用到它（函数签名的一部分）；
 * 它同样是公开 C API 的事实，声明一个 enum 不产生任何链接依赖。
 * 其它枚举/typedef（方向、格式、共享模式、性能模式、回调 typedef）只在 .cpp 里需要，
 * 因此都留在 .cpp —— 那里是唯一的"AAudio ABI 细节"所在之处。
 */
enum aaudio_data_callback_result_t {
    AAUDIO_CALLBACK_RESULT_CONTINUE = 0,
    AAUDIO_CALLBACK_RESULT_STOP = 1,
};

class AaudioRender :
        public Cicada::filterAudioRender,
        private audioRenderPrototype {
public:
    AaudioRender();

    ~AaudioRender() override;

    /*
     * ============ 【能力探测：AAudio 在这台设备上该不该用】============
     *
     * 三个条件**同时**成立才返回 true（缺一即回退 AudioTrack，不新增任何配置开关）：
     *   1. `libaaudio.so` 能 dlopen，且**全部必需符号**都能 dlsym 到 —— 只看符号、不看版本号，
     *      与 mpv `ao_aaudio.c` 的 load_lib_functions 同判据；
     *   2. 设备 API level >= 27（Android 8.1）—— AAudio 的核心符号虽然是 API 26 引入的
     *      （Android 8.0 上 libaaudio.so 就在、符号也齐），但 Google 明确不推荐在 8.0 及更早
     *      使用它（Oboe 的 isAAudioRecommended() 文档原文），所以"支持"按"推荐"算；
     *      这个门槛定义在 .cpp 的 AAUDIO_MIN_RECOMMENDED_API，改那一处即可整体挪动；
     *   3. 本进程还没有"AAudio 流真的打不开"的记录（sticky 自降级）—— "存在但坏了"也按
     *      不支持处理，之后的 create() 直接落到 AudioTrack。
     * 用 minSdk 24 的包在 7.0/7.1 上照样能装、能用 AudioTrack 正常播放。
     *
     * 它必须是**静态 + 幂等 + 可任意线程调用**：AudioTrackRender::is_supported() 会用它
     * 表达"AAudio 优先"（见 AudioTrackRender.h 的说明），而 is_supported() 由
     * audioRenderPrototype::create() 在任意线程调用。
     */
    static bool isAvailable();

    /*
     * 动态符号表的前向声明。**必须是 public**：填表的是 .cpp 里匿名命名空间的探测函数
     * （它拿不到本类的私有访问权），而具体的签名/成员只在 .cpp 里定义 ——
     * 于是"符号是否存在"这件事实完全封装在实现里，其它 TU 看不到 AAudio 的任何细节。
     */
    struct AaudioApi;

private:
    /* ---------- filterAudioRender 的 device_* 钩子（与 AudioTrackRender 同一组） ---------- */
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

    /*
     * 我们**确实**实现了音量：优先用设备侧（若该符号存在），否则在数据回调里做软件增益。
     * 因此这里如实上报 A_FILTER_FLAG_VOLUME —— 这一点很重要：filterAudioRender::applyVolume()
     * 在"设备不支持音量"的分支里是 `assert(mFilter); mFilter->setOption(...)`，而 mFilter 只在
     * 需要格式转换/变速时才被创建；若我们谎报"不支持"，在 gain<=1 的常见路径上会走到那个
     * assert（Release 下更是直接对空 unique_ptr 取用）。详见 .cpp 里 device_setVolume 的说明。
     */
    uint64_t device_get_ability() override
    {
        return A_FILTER_FLAG_VOLUME;
    }

    void device_preClose() override;

    /* ---------- 原型注册（与 AudioTrackRender 同形） ---------- */
    AaudioRender(int dummy)
    {
        /* 参数只为"与本工程其它渲染器的 dummy-ctor 写法保持一致"（它用不到），
         * 显式标注避免未使用参数告警。 */
        (void) dummy;
        addPrototype(this);
    }

    bool is_supported(AFCodecID codec) override
    {
        return isAvailable();
    }

    Cicada::IAudioRender *clone() override
    {
        return new AaudioRender();
    }

    static AaudioRender se;

private:
    /*
     * 动态符号表。签名与成员只在 .cpp 里定义（见上面的前向声明），头文件只留一个指针。
     */
    static const AaudioApi *api();

    /* 输出格式/声道/采样率按本工程既有音频链的契约归一（S16、<=48k、<=2ch）。 */
    void adjustOutputInfo();

    /* 按当前 mOutputInfo 建流并 requestStart 之前的一切准备；失败返回 <0。 */
    int openStream();

    void closeStream();

    /* 设备侧时间戳：已播出的帧数。取不到返回 INT64_MIN（**绝不用墙钟自己造时间**）。 */
    int64_t queryPresentedFrames() const;

    /* 平台回调（运行在 AAudio 自己的线程上）：只做 memcpy / 补零 / 原子计数。 */
    static aaudio_data_callback_result_t dataCallbackEntry(AAudioStream *stream, void *userData,
                                                           void *audioData, int32_t numFrames);

    void onData(void *audioData, int32_t numFrames);

    static void errorCallbackEntry(AAudioStream *stream, void *userData, int32_t error);

    void onError(int32_t error);

    /* 无锁 PCM 环（单生产者=渲染线程、单消费者=AAudio 回调线程）。 */
    size_t ringBytesAvailable() const;

    size_t ringBytesFree() const;

    size_t ringWrite(const uint8_t *data, size_t bytes);

    size_t ringRead(uint8_t *dst, size_t bytes);

    int bytesPerFrame() const;

    /*
     * ============ 成员一律追加在类末尾 ============
     *
     * （本工程有"新成员只追加、不动既有布局"的约定：friend TU 会按偏移直接访问成员，
     *  插入/收窄会移动其后成员的偏移。下面这些全是本类自己的新成员。）
     */
    /* AAudio 流句柄；nullptr = 不可用或已关闭。只由 init_device/flush/dtor 触碰。 */
    AAudioStream *mStream{nullptr};
    /* 平台实际给我们的缓冲能力（帧），只用于日志与 getQueDuration 折算。 */
    int32_t mStreamBufferCapacityFrames{0};
    /* 流当前是否处于"已 requestStart"状态（pause/start 幂等用，纯状态）。 */
    std::atomic_bool mStreamRunning{false};
    /* 设备报过致命错误（errorCallback）后置真：getPosition 退回 INT64_MIN，不再假装有读数。 */
    std::atomic_bool mStreamBroken{false};
    /*
     * flush/init 时的"已播出帧数"基线。getPosition() = (presented - 基线) 换算微秒，
     * 也就是 IAudioRender 要求的"flush 后从零起算"。跨线程读写（设备回调/渲染线程写、
     * 播放器线程读）所以是原子。
     */
    std::atomic<int64_t> mPresentedBaseFrames{0};
    /* 软件增益（硬件音量符号不存在时用）。回调线程读、device_setVolume 写。 */
    std::atomic<float> mGain{1.0f};
    std::atomic_bool mMute{false};
    /* 是否用上了设备侧音量符号（探测结果，只读）。 */
    bool mUseHwVolume{false};
    /* 实际生效的 AAudio performanceMode，只进日志。 */
    int mPerformanceMode{0};
    /* 输出采样率/声道（= mOutputInfo 的快照，回调里要用，避免回调线程读非原子成员）。 */
    int mOutSampleRate{0};
    int mOutChannels{0};
    /*
     * device_require_format() 给出的"应用层要求的输出格式"（可选功能的载体，与
     * AudioTrackRender::mRequireFormat 同形同义）。只有全局设置
     * protected.audio.render.change_format 打开时才会被写入。
     */
    std::unique_ptr<IAFFrame::audioInfo> mRequireFormat{};
    /* 无锁 PCM 环。容量为 2 的幂，读写游标单调递增后按容量掩码取模。 */
    std::unique_ptr<uint8_t[]> mRing{nullptr};
    size_t mRingCapacity{0};
    std::atomic<size_t> mRingRead{0};
    std::atomic<size_t> mRingWrite{0};
    /* 欠载统计（回调里补零的帧数）；只在"这一轮欠载的第一次"打一条日志，避免刷屏。 */
    std::atomic<uint64_t> mUnderrunFrames{0};
    /* 欠载回合闩：回调线程置位、flush_device（另一线程）复位 —— 所以是原子，纯状态、不是计时器。 */
    std::atomic_bool mUnderrunEpisodeActive{false};
    /* errorCallback 每条流的生命周期内只打一条日志（纯状态闩，不是计时器）。 */
    bool mErrorLogged{false};
};

#endif //SOURCE_AAUDIORENDER_H
