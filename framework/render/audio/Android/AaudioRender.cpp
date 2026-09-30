//
// Android AAudio 音频渲染器（默认走它；能力不可用时自动回退 AudioTrackRender）。
//
// 方案依据：docs/ANDROID-AAUDIO-RENDER.md。
// 参考：mpv `audio/out/ao_aaudio.c`（**行为与公开 C API 用法**；mpv 主体是 GPLv2+，
//       本文件为自研实现，未逐字拷贝它的任何代码）。
//
// ============ 为什么整条路径都是 dlopen/dlsym，而不是直接链接 ============
//
// 本工程 minSdk 24（platform/Android/.../CMakeLists.txt 里没有抬到 26），而 AAudio 是
// API 26 才有的平台库：
//   · 若在 minSdk 24 的包上把 libaaudio.so 记成 DT_NEEDED，装到 API 24/25 的设备上
//     动态链接器会因为找不到该库而**整包加载失败** —— 那是启动即崩，比"没有 AAudio"严重得多；
//   · 直接调用 AAudio 符号还会让链接期就需要一个 minSdk 24 不提供的 stub 库。
// 因此这里：不 include <aaudio/AAudio.h>、不调用任何 AAudio 符号，全部用 dlopen + dlsym
// 解析到函数指针（与 mpv 的 load_lib_functions 同一判据：**解析不全就整体不可用** ⇒
// is_supported() 返回 false ⇒ 由 AudioTrackRender 接管，不需要任何配置开关）。
// 需要的 C 类型/枚举在下面自行声明：它们是公开 API 的事实（ABI 与 NDK 头一致），
// 声明它们不产生任何链接依赖。
//

#define LOG_TAG "AaudioRender"

#include "AaudioRender.h"

#include <utils/frame_work_log.h>
#include <utils/ffmpeg_utils.h>

#include <algorithm>
#include <android/api-level.h>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <time.h>

using namespace Cicada;

/*
 * ============================================================================
 * 【"高版本安卓用 AAudio、其余回退 AudioTrack"里的"高版本"到底是哪一版】
 *
 * 这个门槛是**唯一**的一处定义，改这一个常量就能整体挪动（26 / 27 / 28）。
 *
 * 取 27（Android 8.1）的依据（不是拍脑袋）：
 *   · AAudio 的核心符号是 `__INTRODUCED_IN(26)`（AOSP
 *     frameworks/av/media/libaaudio/include/aaudio/AAudio.h）⇒ **Android 8.0 上
 *     libaaudio.so 就在、符号也齐**，光看符号会把 8.0 一起圈进来；
 *   · 但 Google 自己的 Oboe 库在 isAAudioRecommended() 的文档里明确写着：
 *     "AAudio may be supported but not recommended because of version specific issues.
 *      AAudio is not recommended for Android 8.0 or earlier versions."
 *     （Android Developers 的 Oboe/AAudio 说明；Oboe 用的阈值就是 27）
 *   · 结论：**"支持"要按"推荐"算** —— 8.1+ 才算高版本；8.0（API 26）与 7.x（24/25）
 *     都交给 AudioTrack。这与 Google 对第三方播放器的建议一致，也是"新设备要性能、
 *     旧设备要能正常播"这条要求最稳的落点。
 *
 * 注意：本判断只决定**用哪个实现**，与 minSdk 无关 —— minSdk 保持 24，
 * 所以 7.0/7.1 设备照样能装、能用 AudioTrack 正常播放。
 * ==========================================================================*/
static const int AAUDIO_MIN_RECOMMENDED_API = 27;

namespace {
    /*
     * 【进程级 sticky 自降级】真实建流失败过（符号齐全、但设备/ROM 就是打不开 AAudio）
     * ⇒ 之后本进程一律不再用 AAudio，由 AudioTrack 接管。
     *
     * 为什么必须有它：可用性判据只看"符号是否齐全 + 系统版本"时，"存在但坏了"也是一种
     * "不支持"：那种设备会在 init() 里直接报 OPEN_AUDIO_DEVICE_FAILED，让整次播放出错，
     * 而不是按产品要求回退。
     * 为什么是 sticky、且没有任何计时器：AAudio 打不开是**设备/ROM 的性质**，不是瞬时状态；
     * 用一个一次性标记表达它，既不需要重试循环，也不会反复去撞同一个错误。
     */
    std::atomic_bool s_streamOpenFailed{false};

    /* 一次性日志闩：isAvailable() 会被反复调用（每次 create 都会问），只有首次判定各打一条。 */
    std::atomic_bool s_lowApiLogged{false};
    std::atomic_bool s_openFailedLogged{false};
}// namespace

/* ============================================================================
 * AAudio 公开 C API 的**最小自声明**（不 include <aaudio/AAudio.h>，理由见文件头）
 * ==========================================================================*/
typedef int32_t aaudio_result_t;

#define AAUDIO_OK 0

enum aaudio_direction_t {
    AAUDIO_DIRECTION_OUTPUT = 0,
    AAUDIO_DIRECTION_INPUT = 1,
};

enum aaudio_format_t {
    AAUDIO_FORMAT_INVALID = -1,
    AAUDIO_FORMAT_UNSPECIFIED = 0,
    AAUDIO_FORMAT_PCM_I16 = 1,
    AAUDIO_FORMAT_PCM_FLOAT = 2,
    AAUDIO_FORMAT_PCM_I24_PACKED = 3,
    AAUDIO_FORMAT_PCM_I32 = 4,
};

enum aaudio_sharing_mode_t {
    AAUDIO_SHARING_MODE_EXCLUSIVE = 0,
    AAUDIO_SHARING_MODE_SHARED = 1,
};

enum aaudio_performance_mode_t {
    AAUDIO_PERFORMANCE_MODE_NONE = 10,
    AAUDIO_PERFORMANCE_MODE_POWER_SAVING = 11,
    AAUDIO_PERFORMANCE_MODE_LOW_LATENCY = 12,
};

/* aaudio_data_callback_result_t 声明在头文件里（函数签名要用），这里不再重复定义。 */

typedef aaudio_data_callback_result_t (*AAudioStream_dataCallback)(AAudioStream *stream, void *userData,
                                                                  void *audioData, int32_t numFrames);

typedef void (*AAudioStream_errorCallback)(AAudioStream *stream, void *userData, aaudio_result_t error);

/* ============================================================================
 * 符号表：必需项解析失败即"AAudio 不可用"；可选项缺失不算不可用。
 * ==========================================================================*/
struct AaudioRender::AaudioApi {
    /* ---- 必需：builder ---- */
    aaudio_result_t (*createStreamBuilder)(AAudioStreamBuilder **builder);
    void (*builderSetDirection)(AAudioStreamBuilder *builder, aaudio_direction_t direction);
    void (*builderSetSampleRate)(AAudioStreamBuilder *builder, int32_t sampleRate);
    void (*builderSetChannelCount)(AAudioStreamBuilder *builder, int32_t channelCount);
    void (*builderSetFormat)(AAudioStreamBuilder *builder, aaudio_format_t format);
    void (*builderSetSharingMode)(AAudioStreamBuilder *builder, aaudio_sharing_mode_t sharingMode);
    void (*builderSetPerformanceMode)(AAudioStreamBuilder *builder, aaudio_performance_mode_t mode);
    void (*builderSetBufferCapacityInFrames)(AAudioStreamBuilder *builder, int32_t numFrames);
    void (*builderSetDataCallback)(AAudioStreamBuilder *builder, AAudioStream_dataCallback callback, void *userData);
    void (*builderSetErrorCallback)(AAudioStreamBuilder *builder, AAudioStream_errorCallback callback, void *userData);
    aaudio_result_t (*builderOpenStream)(AAudioStreamBuilder *builder, AAudioStream **stream);
    aaudio_result_t (*builderDelete)(AAudioStreamBuilder *builder);
    /* ---- 必需：stream 控制 ---- */
    aaudio_result_t (*streamRequestStart)(AAudioStream *stream);
    aaudio_result_t (*streamRequestPause)(AAudioStream *stream);
    aaudio_result_t (*streamRequestFlush)(AAudioStream *stream);
    aaudio_result_t (*streamRequestStop)(AAudioStream *stream);
    aaudio_result_t (*streamClose)(AAudioStream *stream);
    /* ---- 必需：stream 查询 ---- */
    int32_t (*streamGetSampleRate)(AAudioStream *stream);
    int32_t (*streamGetChannelCount)(AAudioStream *stream);
    aaudio_format_t (*streamGetFormat)(AAudioStream *stream);
    int32_t (*streamGetBufferSizeInFrames)(AAudioStream *stream);
    int32_t (*streamGetBufferCapacityInFrames)(AAudioStream *stream);
    int64_t (*streamGetFramesWritten)(AAudioStream *stream);
    int64_t (*streamGetFramesRead)(AAudioStream *stream);
    aaudio_result_t (*streamGetTimestamp)(AAudioStream *stream, clockid_t clockId,
                                         int64_t *framePosition, int64_t *timeNanoseconds);
    /* ---- 可选：存在就用，不存在不影响可用性 ---- */
    void (*builderSetUsage)(AAudioStreamBuilder *builder, int32_t usage);       /* API 28+ */
    int32_t (*streamSetBufferSizeInFrames)(AAudioStream *stream, int32_t numFrames);
    int32_t (*streamGetXRunCount)(AAudioStream *stream);
    /*
     * 【注意】公开的 AAudio API 里**没有** AAudioStream_setVolume（API 26/28/32 的符号表都没有；
     * 见 D:\hilihili\mpv\audio\out\aaudio_functions{26,28,32}.inc）。这里仍然按任务要求去探测它：
     *   · 若某版本/某 OEM 真的提供，就用设备侧音量；
     *   · 否则退回**数据回调里的软件增益**（见 device_setVolume），并且仍然如实上报
     *     A_FILTER_FLAG_VOLUME —— 绝不能谎报"不支持音量"：filterAudioRender::applyVolume()
     *     在"设备不支持"的分支里是 `assert(mFilter); mFilter->setOption(...)`，而 mFilter 只在
     *     需要格式转换/变速时才创建，gain<=1 的常见路径上会是空指针（Release 下直接崩）。
     */
    int32_t (*streamSetVolume)(AAudioStream *stream, float volume);
    const char *(*convertResultToText)(aaudio_result_t returnCode);
};

namespace {

    /* 把 dlsym 的样板收在一处；每个符号都先查库句柄、再查 RTLD_DEFAULT（与 mpv 同法：
       个别设备把 AAudio 符号导出在全局命名空间）。 */
    template<typename T>
    void resolve(void *handle, const char *name, T &target)
    {
        target = reinterpret_cast<T>(dlsym(handle, name));

        if (target == nullptr) {
            target = reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
        }
    }

    /* dlopen 只做一次；解析结果缓存。失败时把"缺哪个符号"写进日志，便于真机定位。 */
    AaudioRender::AaudioApi *loadApi()
    {
        /*
         * RTLD_NOW|RTLD_LOCAL：立刻解析本库自身的重定位（拿不到就失败），且不污染全局符号表。
         * 不要求 RTLD_GLOBAL：我们只用自己的函数指针，没有"让别的库看到 AAudio 符号"的需求。
         */
        void *handle = dlopen("libaaudio.so", RTLD_NOW | RTLD_LOCAL);

        if (handle == nullptr) {
            const char *err = dlerror();
            AF_LOGW("[aaudio] libaaudio unavailable ⇒ fall back to AudioTrack (dlopen failed: %s)\n",
                    err != nullptr ? err : "unknown");
            return nullptr;
        }

        AaudioRender::AaudioApi *table = new AaudioRender::AaudioApi();

        /* ---- 必需符号 ---- */
        resolve(handle, "AAudio_createStreamBuilder", table->createStreamBuilder);
        resolve(handle, "AAudioStreamBuilder_setDirection", table->builderSetDirection);
        resolve(handle, "AAudioStreamBuilder_setSampleRate", table->builderSetSampleRate);
        resolve(handle, "AAudioStreamBuilder_setChannelCount", table->builderSetChannelCount);
        resolve(handle, "AAudioStreamBuilder_setFormat", table->builderSetFormat);
        resolve(handle, "AAudioStreamBuilder_setSharingMode", table->builderSetSharingMode);
        resolve(handle, "AAudioStreamBuilder_setPerformanceMode", table->builderSetPerformanceMode);
        resolve(handle, "AAudioStreamBuilder_setBufferCapacityInFrames", table->builderSetBufferCapacityInFrames);
        resolve(handle, "AAudioStreamBuilder_setDataCallback", table->builderSetDataCallback);
        resolve(handle, "AAudioStreamBuilder_setErrorCallback", table->builderSetErrorCallback);
        resolve(handle, "AAudioStreamBuilder_openStream", table->builderOpenStream);
        resolve(handle, "AAudioStreamBuilder_delete", table->builderDelete);

        resolve(handle, "AAudioStream_requestStart", table->streamRequestStart);
        resolve(handle, "AAudioStream_requestPause", table->streamRequestPause);
        resolve(handle, "AAudioStream_requestFlush", table->streamRequestFlush);
        resolve(handle, "AAudioStream_requestStop", table->streamRequestStop);
        resolve(handle, "AAudioStream_close", table->streamClose);

        resolve(handle, "AAudioStream_getSampleRate", table->streamGetSampleRate);
        resolve(handle, "AAudioStream_getChannelCount", table->streamGetChannelCount);
        resolve(handle, "AAudioStream_getFormat", table->streamGetFormat);
        resolve(handle, "AAudioStream_getBufferSizeInFrames", table->streamGetBufferSizeInFrames);
        resolve(handle, "AAudioStream_getBufferCapacityInFrames", table->streamGetBufferCapacityInFrames);
        resolve(handle, "AAudioStream_getFramesWritten", table->streamGetFramesWritten);
        resolve(handle, "AAudioStream_getFramesRead", table->streamGetFramesRead);
        resolve(handle, "AAudioStream_getTimestamp", table->streamGetTimestamp);

        /* ---- 可选符号（缺失是常态，不算不可用） ---- */
        resolve(handle, "AAudioStreamBuilder_setUsage", table->builderSetUsage);
        resolve(handle, "AAudioStream_setBufferSizeInFrames", table->streamSetBufferSizeInFrames);
        resolve(handle, "AAudioStream_getXRunCount", table->streamGetXRunCount);
        resolve(handle, "AAudioStream_setVolume", table->streamSetVolume);
        resolve(handle, "AAudio_convertResultToText", table->convertResultToText);

        /* 逐个必需项判空：任一缺失就整体判不可用（与 mpv "load_lib_functions 失败即换输出"等价）。 */
        if (table->createStreamBuilder == nullptr || table->builderSetDirection == nullptr ||
            table->builderSetSampleRate == nullptr || table->builderSetChannelCount == nullptr ||
            table->builderSetFormat == nullptr || table->builderSetSharingMode == nullptr ||
            table->builderSetPerformanceMode == nullptr || table->builderSetBufferCapacityInFrames == nullptr ||
            table->builderSetDataCallback == nullptr || table->builderSetErrorCallback == nullptr ||
            table->builderOpenStream == nullptr || table->builderDelete == nullptr ||
            table->streamRequestStart == nullptr || table->streamRequestPause == nullptr ||
            table->streamRequestFlush == nullptr || table->streamRequestStop == nullptr ||
            table->streamClose == nullptr || table->streamGetSampleRate == nullptr ||
            table->streamGetChannelCount == nullptr || table->streamGetFormat == nullptr ||
            table->streamGetBufferSizeInFrames == nullptr || table->streamGetBufferCapacityInFrames == nullptr ||
            table->streamGetFramesWritten == nullptr || table->streamGetFramesRead == nullptr ||
            table->streamGetTimestamp == nullptr) {
            AF_LOGW("[aaudio] libaaudio unavailable ⇒ fall back to AudioTrack "
                    "(libaaudio.so exists but some required symbols are missing; API < 26 or a trimmed system image)\n");
            delete table;
            dlclose(handle);
            return nullptr;
        }

        AF_LOGI("[aaudio] libaaudio loaded, all required symbols resolved "
                "(setVolume=%s, setUsage=%s, xRunCount=%s)\n",
                table->streamSetVolume != nullptr ? "yes" : "no",
                table->builderSetUsage != nullptr ? "yes" : "no",
                table->streamGetXRunCount != nullptr ? "yes" : "no");
        return table;
    }

}// namespace

const AaudioRender::AaudioApi *AaudioRender::api()
{
    /*
     * C++11 保证函数内静态变量的初始化是线程安全的（编译器生成的 once 语义），
     * 所以探测只做一次、也不需要在 is_supported() 里加锁。探测结果是"这台设备的能力事实"，
     * 不是配置开关，也不会随时间变化（dlopen 失败不会自动变好），所以缓存是安全的。
     */
    static const AaudioApi *s_api = loadApi();
    return s_api;
}

bool AaudioRender::isAvailable()
{
    /*
     * ============ 【三个条件同时成立才用 AAudio，否则回退 AudioTrack】============
     *   1. libaaudio.so 能 dlopen 且必需符号全部解析成功（loadApi；失败时它自己打日志）；
     *   2. 设备 API level >= AAUDIO_MIN_RECOMMENDED_API（见文件头的依据：8.1+ 才算高版本）；
     *   3. 本进程还没有"AAudio 流真的打不开"的记录（sticky 自降级）。
     * 全部是"设备能力/设备事实"的判据，没有配置开关、没有计时器。
     */
    if (api() == nullptr) {
        return false;
    }

    const int deviceApi = android_get_device_api_level();

    if (deviceApi < AAUDIO_MIN_RECOMMENDED_API) {
        if (!s_lowApiLogged.exchange(true)) {
            AF_LOGI("[aaudio] not used on this device (API level=%d < %d) ⇒ fall back to AudioTrack. "
                    "AAudio's core symbols exist from API 26, but Google documents AAudio as NOT recommended "
                    "for Android 8.0 and earlier (Oboe's isAAudioRecommended() is false there), so those "
                    "devices keep the AudioTrack path\n",
                    deviceApi, AAUDIO_MIN_RECOMMENDED_API);
        }

        return false;
    }

    if (s_streamOpenFailed.load()) {
        if (!s_openFailedLogged.exchange(true)) {
            AF_LOGW("[aaudio] not used any more in this process (a previous AAudio stream could not be opened "
                    "on this device) ⇒ fall back to AudioTrack (sticky flag, no timer, no retry loop)\n");
        }

        return false;
    }

    return true;
}

/* 与 AudioTrackRender 同形：静态对象在构造时把自己注册进 audioRenderPrototype。
 * 注册顺序**不是**本方案的正确性依赖（两个 .cpp 的静态对象构造顺序未定义），
 * 顺序无关性由 AudioTrackRender::is_supported() 调用我们的 isAvailable() 来保证。 */
AaudioRender AaudioRender::se(0);

AaudioRender::AaudioRender() = default;

AaudioRender::~AaudioRender()
{
    /* 先关流再让基类析构收线程：AAudio 的 data callback 会访问 this 的成员，
     * 必须先保证流不再拉数据（close 会保证回调不再被调用）。 */
    closeStream();
}

/* ============================================================================
 * 格式：与 AudioTrackRender 归一成同一套契约
 * ==========================================================================*/
void AaudioRender::adjustOutputInfo()
{
    if (mRequireFormat) {
        mOutputInfo = *mRequireFormat;
        return;
    }

    if (mInputInfo.format != AF_SAMPLE_FMT_S16) {
        mOutputInfo.format = AF_SAMPLE_FMT_S16;
        needFilter = true;
    }

    if (mInputInfo.sample_rate > 48000) {
        mOutputInfo.sample_rate = 48000;
        needFilter = true;
    }

    if (mInputInfo.channels > 2) {
        mOutputInfo.channels = 2;
        needFilter = true;
    }
}

bool AaudioRender::device_require_format(const IAFFrame::audioInfo &info)
{
    /* 与 AudioTrackRender 逐字同判据：只接受 S16 / <=48k / <=2ch，其余交给软件滤波器转换。
     * 这条不是"AAudio 的限制"（AAudio 支持更高采样率与更多声道），而是**本工程音频链的既有契约**：
     * 解码后到渲染之间的 PCM 约定就是 S16，保持两者一致才不会出现"AAudio 收下了但我们送错格式"。
     * 若将来要放开，请连同 AudioTrackRender 一起改，并给出真机 A/B 日志。 */
    if (info.format != AF_SAMPLE_FMT_S16 || info.sample_rate > 48000 || info.channels > 2) {
        return false;
    }

    mRequireFormat = std::unique_ptr<IAFFrame::audioInfo>(new IAFFrame::audioInfo());
    (*mRequireFormat) = info;
    return true;
}

/* ============================================================================
 * 建流 / 关流
 * ==========================================================================*/
int AaudioRender::openStream()
{
    const AaudioApi *a = api();

    if (a == nullptr || a->createStreamBuilder == nullptr) {
        return OPEN_AUDIO_DEVICE_FAILED;
    }

    mOutSampleRate = mOutputInfo.sample_rate;
    mOutChannels = mOutputInfo.channels;

    /* 目标缓冲：约 50ms。给一个**正数**即可 —— AAudio 会按设备能力夹到它的合法区间，
     * 不会因为值太大而失败（getBufferCapacityInFrames 之后回读实际值只用于日志/统计）。 */
    int32_t desiredCapacity = mOutSampleRate / 20;

    if (desiredCapacity < 2 * 256) {
        desiredCapacity = 2 * 256;
    }

    /*
     * 三轮尝试，全部是纯状态判据（没有任何计时器），与 mpv "先试低延迟、失败再退"同形：
     *   1. LOW_LATENCY + 期望缓冲  —— 媒体播放想要低延迟；
     *   2. NONE + 期望缓冲         —— 个别设备/ROM 在低延迟模式下 openStream 会失败；
     *   3. NONE + 不指定缓冲       —— 极少数 ROM 对"期望缓冲容量"这个参数敏感（会拒），
     *                                这一轮把容量交回平台默认值，尽最大可能把流建起来。
     * 三轮都失败才认"这台设备打不开 AAudio"（并置 sticky 标记让后续改用 AudioTrack）。
     */
    struct OpenAttempt {
        aaudio_performance_mode_t mode;
        bool setBufferCapacity;
    };
    const OpenAttempt attempts[3] = {
            {AAUDIO_PERFORMANCE_MODE_LOW_LATENCY, true},
            {AAUDIO_PERFORMANCE_MODE_NONE, true},
            {AAUDIO_PERFORMANCE_MODE_NONE, false},
    };
    aaudio_result_t ret = -1;

    for (int attempt = 0; attempt < 3; ++attempt) {
        AAudioStreamBuilder *builder = nullptr;
        ret = a->createStreamBuilder(&builder);

        if (ret != AAUDIO_OK || builder == nullptr) {
            AF_LOGE("[aaudio] AAudio_createStreamBuilder failed: %d\n", (int) ret);
            /*
             * 所有建流失败出口都置 sticky：任何一次"AAudio 就是建不起流"都说明这台设备/这个 ROM
             * 用不了它，于是上层 SMPAVDeviceManager::setUpAudioRender 的那一次重试里
             * create() 会落到 AudioTrack（isAvailable() 见标记返回 false）。
             */
            s_streamOpenFailed = true;
            return OPEN_AUDIO_DEVICE_FAILED;
        }

        a->builderSetDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        a->builderSetFormat(builder, AAUDIO_FORMAT_PCM_I16);
        a->builderSetSampleRate(builder, mOutSampleRate);
        a->builderSetChannelCount(builder, mOutChannels);
        /*
         * SHARED：与系统混音器共享输出。EXCLUSIVE（MMAP）延迟更低，但要求设备/通路独占，
         * 在多数消费级设备上会被拒；本工程是媒体播放器（不是低延迟乐器应用），
         * 共享模式的兼容性收益远大于那点延迟收益，所以固定 SHARED。
         */
        a->builderSetSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        a->builderSetPerformanceMode(builder, attempts[attempt].mode);

        if (attempts[attempt].setBufferCapacity) {
            a->builderSetBufferCapacityInFrames(builder, desiredCapacity);
        }

        if (a->builderSetUsage != nullptr) {
            /* AAUDIO_USAGE_MEDIA = 1（API 28+ 才有这个 setter；有就标成媒体流，
             * 让系统按媒体音量策略处理它 —— 不做音量路由的话某些 ROM 会把流当成通知音）。 */
            a->builderSetUsage(builder, 1);
        }

        a->builderSetDataCallback(builder, &AaudioRender::dataCallbackEntry, this);
        a->builderSetErrorCallback(builder, &AaudioRender::errorCallbackEntry, this);

        AAudioStream *stream = nullptr;
        ret = a->builderOpenStream(builder, &stream);
        a->builderDelete(builder);

        if (ret == AAUDIO_OK && stream != nullptr) {
            mStream = stream;
            mPerformanceMode = (int) attempts[attempt].mode;
            break;
        }

        AF_LOGW("[aaudio] openStream failed (attempt %d/3 performanceMode=%d setBufferCapacity=%d): %d\n",
                attempt + 1, (int) attempts[attempt].mode, (int) attempts[attempt].setBufferCapacity, (int) ret);
        mStream = nullptr;
    }

    if (mStream == nullptr) {
        /*
         * 三轮都建不起流 ⇒ 这台设备"有 AAudio 但用不了"。置 sticky 标记：
         *   · 本次 init 仍然会失败（如实上报，不假装成功）；
         *   · 但之后本进程的 create() 会直接落到 AudioTrack（isAvailable() 见标记返回 false），
         *     也就是不再反复去撞同一个错误。这就是"存在但坏了"也按回退处理的那条路。
         */
        s_streamOpenFailed = true;
        AF_LOGE("[aaudio] cannot open an output stream after 3 attempts (last result=%d) ⇒ AAudio disabled for "
                "the rest of this process; the AudioTrack path will be used from the next audio setup on\n",
                (int) ret);
        return OPEN_AUDIO_DEVICE_FAILED;
    }

    /* 回读设备实际给的参数：AAudio 允许在能力范围内调整请求值，必须以设备实际值为准。 */
    const int32_t rate = a->streamGetSampleRate(mStream);
    const int32_t channels = a->streamGetChannelCount(mStream);
    const aaudio_format_t format = a->streamGetFormat(mStream);
    mStreamBufferCapacityFrames = a->streamGetBufferCapacityInFrames(mStream);

    if (format != AAUDIO_FORMAT_PCM_I16) {
        /* 我们只送 S16（build 时也只要了 S16）。设备给了别的格式说明它没接受我们的请求，
         * 继续送 S16 会得到噪声 —— 直接判失败，让上层看到明确错误（不做静默降级）。
         * 同样置 sticky：这台设备按我们的配置就是用不了 AAudio，回退 AudioTrack 更稳。 */
        AF_LOGE("[aaudio] device forced format=%d instead of PCM_I16 — refusing to feed S16 into it\n", (int) format);
        s_streamOpenFailed = true;
        closeStream();
        return OPEN_AUDIO_DEVICE_FAILED;
    }

    if (rate > 0 && channels > 0 && (rate != mOutSampleRate || channels != mOutChannels)) {
        /*
         * 设备没有接受我们要的采样率/声道。**不能**照旧把同样内容的 PCM 送进去 ——
         * AAudio 在非 MMAP 路径上由 AudioFlinger 混音（会重采样），但在 MMAP/SHARED 组合下
         * 不会替我们做采样率转换，硬送就会变调。
         *
         * 处置：把"设备实际值"写进 mOutputInfo 并置 needFilter —— filterAudioRender::init()
         * 是在 init_device() **之后**才创建滤波器的（filterAudioRender.cpp:53 vs :79），
         * 所以这里设置会被如实采用，由软件滤波器把 mInputInfo 重采样/混音到 mOutputInfo。
         * 这是本工程既有的"格式不符就上滤波器"通路，不是新开关。
         */
        AF_LOGW("[aaudio] device refused rate/channels %d/%d, using %d/%d — a software resampler/remixer "
                "will be created for this stream\n",
                mOutSampleRate, mOutChannels, (int) rate, (int) channels);
        mOutSampleRate = rate;
        mOutChannels = channels;
        mOutputInfo.sample_rate = rate;
        mOutputInfo.channels = channels;
        needFilter = true;
    }

    /* 设备侧音量符号（公开 API 里并不存在，见 AaudioApi::streamSetVolume 的说明）。 */
    mUseHwVolume = (a->streamSetVolume != nullptr);

    /*
     * 无锁 PCM 环：容量取"约 200ms"与"4 倍平台缓冲"的较大者，向上取到 2 的幂
     * （这样取模就是一次按位与）。回调只在 requestStart 之后才会被调用，而 requestStart
     * 发生在 start_device()（= 播放器要开始播的时候），所以这里分配是安全的。
     */
    const int bytesPerFrame = mOutChannels * 2;
    size_t wantedBytes = (size_t) (mOutSampleRate / 5) * (size_t) bytesPerFrame;              /* 200ms */
    const size_t platformBytes = (size_t) std::max(0, mStreamBufferCapacityFrames) * (size_t) bytesPerFrame;
    wantedBytes = std::max(wantedBytes, platformBytes * 4);
    size_t capacity = 1024;

    while (capacity < wantedBytes) {
        capacity <<= 1;
    }

    mRingCapacity = capacity;
    mRing = std::unique_ptr<uint8_t[]>(new uint8_t[mRingCapacity]);
    mRingRead = 0;
    mRingWrite = 0;
    mUnderrunFrames = 0;
    mUnderrunEpisodeActive = false;
    mStreamBroken = false;
    mPresentedBaseFrames = 0;

    /*
     * 基线：此刻流还没 start，已播出帧数的读数就是"零消费"的参考点。
     * 拿不到时间戳也没关系 —— getPosition() 会返回 INT64_MIN（音频时钟暂不可用），
     * 等设备真的开始播出后再惰性重钉，见 device_get_position()。
     */
    const int64_t presented = queryPresentedFrames();

    if (presented != INT64_MIN) {
        mPresentedBaseFrames = presented;
    }

    AF_LOGI("[aaudio] output=aaudio stream opened: performanceMode=%d rate=%d channels=%d "
            "bufferCapacityFrames=%d ringBytes=%u (S16, shared, target ~50ms buffer)\n",
            mPerformanceMode, mOutSampleRate, mOutChannels, (int) mStreamBufferCapacityFrames,
            (unsigned) mRingCapacity);
    return 0;
}

void AaudioRender::closeStream()
{
    /*
     * 先判 mStream，再取符号表 —— 顺序有实际意义：
     * 原型对象 `AaudioRender::se` 的析构发生在进程退出、静态对象销毁阶段，它的 mStream 永远是
     * nullptr（它只用于注册，从不播放）。先判空就能在那种时刻**完全不碰 dlopen**，
     * 避免在静态销毁期做库加载。
     */
    if (mStream == nullptr) {
        return;
    }

    const AaudioApi *a = api();

    if (a != nullptr) {
        /* requestStop 让流离开 STARTED/PAUSED，close 才是干净的状态迁移。 */
        if (a->streamRequestStop != nullptr) {
            a->streamRequestStop(mStream);
        }

        if (a->streamClose != nullptr) {
            a->streamClose(mStream);

            /*
             * 【释放探针】AAudio 输出流真的被关掉了。
             * 放在 streamClose 成功调用之后：没有这个符号时不打，日志不会说谎。
             * 安卓侧每退出一次播放页，这条应该出现一次（音频侧不留打开的输出流）。
             */
            AF_LOGI("[mem] AAudio output stream closed\n");
        }
    }

    mStream = nullptr;
    mStreamRunning = false;
}

int AaudioRender::init_device()
{
    /*
     * 输出格式按本工程既有契约归一（S16 / <=48k / <=2ch）；被改过就置 needFilter，
     * 由 filterAudioRender 起软件滤波器转换。与 AudioTrackRender::init_device 的处理顺序一致。
     */
    adjustOutputInfo();

    if (openStream() < 0) {
        return OPEN_AUDIO_DEVICE_FAILED;
    }

    /* 注意：这里**不** requestStart。IAudioRender 的契约是"init 不开始播放"，
     * 真正的开始由播放器调 pause(false) → start_device() 触发。AAudio 在 openStream
     * 之后处于 OPEN 状态，不会拉数据，正好符合这个契约。 */
    return 0;
}

int AaudioRender::pause_device()
{
    const AaudioApi *a = api();

    if (a == nullptr || mStream == nullptr) {
        return -EINVAL;
    }

    /* 幂等：AAudio 对"已经 pause 的流再 pause"会返回 INVALID_STATE 并打日志；
     * 播放器的 pause() 会被反复调用（暂停/缓冲/seek 都会走），所以用状态闸掉重复调用。
     * 这是纯状态判断，不是计时器。 */
    if (!mStreamRunning.exchange(false)) {
        return 0;
    }

    const aaudio_result_t ret = a->streamRequestPause(mStream);

    if (ret != AAUDIO_OK) {
        AF_LOGW("[aaudio] requestPause returned %d\n", (int) ret);
    }

    return 0;
}

int AaudioRender::start_device()
{
    const AaudioApi *a = api();

    if (a == nullptr || mStream == nullptr) {
        return -EINVAL;
    }

    if (mStreamRunning.exchange(true)) {
        return 0;
    }

    const aaudio_result_t ret = a->streamRequestStart(mStream);

    if (ret != AAUDIO_OK) {
        AF_LOGW("[aaudio] requestStart returned %d\n", (int) ret);
    }

    return 0;
}

void AaudioRender::flush_device()
{
    const AaudioApi *a = api();

    if (a == nullptr || mStream == nullptr) {
        return;
    }

    /*
     * 标准序列 Pause → Flush →（原来在跑就）Start：
     *   · requestFlush 要求流不处于 STARTED（否则 INVALID_STATE），所以必须先 pause；
     *   · 这里**不**用 AAudioStream_waitForStateChange 去等状态迁移完成 —— 那需要一个等待超时，
     *     属于计时语义，本项目明确不要。取而代之：flush 的失败只记日志，而"位置从零起算"
     *     这件真正重要的事由下面**重钉基线 + 清环**保证（它们不依赖设备状态迁移是否完成）。
     */
    const bool wasRunning = mStreamRunning.exchange(false);

    if (wasRunning) {
        a->streamRequestPause(mStream);
    }

    const aaudio_result_t ret = a->streamRequestFlush(mStream);

    if (ret != AAUDIO_OK) {
        /* 常见且无害：流此刻不在 PAUSED（异步迁移没走完）。位置正确性不依赖它。 */
        AF_LOGI("[aaudio] requestFlush returned %d (state transition is asynchronous; the position base is "
                "re-pinned below anyway)\n", (int) ret);
    }

    /* 丢弃还没交给平台的 PCM（filterAudioRender::flush 已经清过它的帧队列）。 */
    mRingRead = 0;
    mRingWrite = 0;
    mUnderrunEpisodeActive = false;

    /*
     * 重钉基线 = "flush 后位置从零起算"的唯一实现，也是本工程反复踩过的坑
     * （见 SuperMediaPlayer::FlushAudioPath 里"基准必须与设备 flush 配对"的说明）。
     *
     * 读不到时间戳时**保留旧基线**而不是置 0：置 0 会让下一次读数直接变成"设备开机以来的
     * 全部帧数"（对上层无害但不必要地跳变），而保留旧基线配合 device_get_position() 里
     * 那条"presented < base 就就地重钉"的自愈，任意一种设备语义都能收敛。
     */
    const int64_t presented = queryPresentedFrames();

    if (presented != INT64_MIN) {
        mPresentedBaseFrames = presented;
    }

    if (wasRunning) {
        mStreamRunning = true;
        a->streamRequestStart(mStream);
    }
}

/* ============================================================================
 * 音量 / 静音
 * ==========================================================================*/
void AaudioRender::device_setVolume(float gain)
{
    mGain = gain;

    const AaudioApi *a = api();

    if (a != nullptr && mUseHwVolume && mStream != nullptr && a->streamSetVolume != nullptr) {
        a->streamSetVolume(mStream, gain);
    }

    /* 硬件音量不可用时，软件增益在数据回调里生效（onData）。这里不做任何重采样/滤波器动作，
     * 也不改动 filterAudioRender 的音量链 —— 那套（gain = v^3 后交给滤波器或设备）仍然照旧，
     * 因为 filterAudioRender 只在"设备不支持音量"时才走滤波器分支，而我们是支持的那一支。 */
}

void AaudioRender::device_mute(bool bMute)
{
    mMute = bMute;

    if (bMute) {
        const AaudioApi *a = api();

        if (a != nullptr && mUseHwVolume && mStream != nullptr && a->streamSetVolume != nullptr) {
            a->streamSetVolume(mStream, 0.0f);
        }
    } else {
        device_setVolume(mGain.load());
    }
}

/* ============================================================================
 * 位置 / 已消费量
 * ==========================================================================*/
int64_t AaudioRender::queryPresentedFrames() const
{
    const AaudioApi *a = api();

    if (a == nullptr || mStream == nullptr || a->streamGetTimestamp == nullptr) {
        return INT64_MIN;
    }

    int64_t frames = 0;
    int64_t nanoTime = 0;
    /*
     * CLOCK_MONOTONIC：与 mpv 同法（ao_aaudio.c 的数据回调里就是
     * AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &presented, &present_time)）。
     * 它给的是"已经真正播出的帧数"（硬件时间戳），比 Java AudioTrack.getPlaybackHeadPosition() 更准。
     */
    const aaudio_result_t ret = a->streamGetTimestamp(mStream, CLOCK_MONOTONIC, &frames, &nanoTime);

    if (ret != AAUDIO_OK) {
        /* 流刚 open / 刚 flush 时 AAudio 常返回 UNAVAILABLE；**绝不**用墙钟自己造时间。 */
        return INT64_MIN;
    }

    return frames;
}

int64_t AaudioRender::device_get_position()
{
    if (mStreamBroken.load()) {
        return INT64_MIN;
    }

    const int64_t presented = queryPresentedFrames();

    if (presented == INT64_MIN) {
        return INT64_MIN;
    }

    int64_t base = mPresentedBaseFrames.load();

    if (presented < base) {
        /*
         * 结构性自愈：个别实现/路径下 flush 会把"已播出帧数"从头起算（AAudio 文档没有保证它
         * flush 后是否归零）。此时若照原样相减就是**负值**，而 SuperMediaPlayer::
         * getAudioPlayTimeStamp() 把"负值/哨兵"一律当成坏值 ⇒ 音频时钟会**永久**不可用
         * （主时钟一直退回系统时钟、音画基准漂移）。所以这里把基线就地重钉到当前读数 ——
         * 纯状态自愈，与播放器里"delta<0 就地重取快照"是同一条设计。
         */
        mPresentedBaseFrames = presented;
        base = presented;
    }

    if (mOutSampleRate <= 0) {
        return INT64_MIN;
    }

    /* IAudioRender 的契约：返回"flush 后从零起算的设备已消费微秒数"。 */
    return (presented - base) * 1000000 / mOutSampleRate;
}

uint64_t AaudioRender::device_get_que_duration()
{
    const AaudioApi *a = api();

    if (a == nullptr || mStream == nullptr || mOutSampleRate <= 0) {
        return 0;
    }

    /* 我们自己的环里还没交给平台的帧数。 */
    const uint64_t ringFrames = (uint64_t) (ringBytesAvailable() / (size_t) std::max(1, bytesPerFrame()));

    /*
     * 平台侧还没播出的帧数 = framesWritten - presented：
     *   · getFramesWritten 是"数据回调已经交给流的总帧数"（含我们补的静音）；
     *   · presented 是"真的播出去的帧数"。
     * 两者的差就是还压在平台缓冲里的量。取不到 presented 时只报环里的量（宁可少报，不多报）。
     */
    uint64_t platformFrames = 0;
    const int64_t written = a->streamGetFramesWritten(mStream);
    const int64_t presented = queryPresentedFrames();

    if (written > 0 && presented != INT64_MIN && written > presented) {
        platformFrames = (uint64_t) (written - presented);
    }

    const uint64_t totalFrames = ringFrames + platformFrames;
    return totalFrames * 1000000 / (uint64_t) mOutSampleRate;
}

/* ============================================================================
 * 无锁 PCM 环（单生产者 = 渲染线程、单消费者 = AAudio 回调线程）
 *
 * 为什么必须有它：AAudio 的数据回调由平台在自己的线程上**拉取**，回调里绝不能等锁、绝不能阻塞
 * （阻塞会导致 xrun / 爆音，甚至被系统判定为不合格的实现）。而 filterAudioRender 的帧队列是
 * mutex + queue 的，回调线程碰不得。所以设备侧数据面用这个只有两个原子游标的环。
 * ==========================================================================*/
int AaudioRender::bytesPerFrame() const
{
    return mOutChannels * 2;   /* S16 */
}

size_t AaudioRender::ringBytesAvailable() const
{
    const size_t write = mRingWrite.load();
    const size_t read = mRingRead.load();

    if (mRingCapacity == 0 || write < read) {
        return 0;
    }

    return write - read;
}

size_t AaudioRender::ringBytesFree() const
{
    if (mRingCapacity == 0) {
        return 0;
    }

    return mRingCapacity - ringBytesAvailable();
}

size_t AaudioRender::ringWrite(const uint8_t *data, size_t bytes)
{
    if (mRing == nullptr || mRingCapacity == 0 || data == nullptr) {
        return 0;
    }

    const size_t freeBytes = ringBytesFree();
    const size_t toWrite = std::min(bytes, freeBytes);
    const size_t writeIndex = mRingWrite.load() & (mRingCapacity - 1);
    const size_t firstChunk = std::min(toWrite, mRingCapacity - writeIndex);

    memcpy(mRing.get() + writeIndex, data, firstChunk);

    if (toWrite > firstChunk) {
        memcpy(mRing.get(), data + firstChunk, toWrite - firstChunk);
    }

    /* release：保证消费者看到数据写完之前不会看到新的写游标。 */
    mRingWrite.store(mRingWrite.load() + toWrite, std::memory_order_release);
    return toWrite;
}

size_t AaudioRender::ringRead(uint8_t *dst, size_t bytes)
{
    if (mRing == nullptr || mRingCapacity == 0 || dst == nullptr) {
        return 0;
    }

    const size_t available = ringBytesAvailable();
    const size_t toRead = std::min(bytes, available);
    const size_t readIndex = mRingRead.load() & (mRingCapacity - 1);
    const size_t firstChunk = std::min(toRead, mRingCapacity - readIndex);

    memcpy(dst, mRing.get() + readIndex, firstChunk);

    if (toRead > firstChunk) {
        memcpy(dst + firstChunk, mRing.get(), toRead - firstChunk);
    }

    mRingRead.store(mRingRead.load() + toRead, std::memory_order_release);
    return toRead;
}

/* ============================================================================
 * 设备写入（渲染线程）→ 只入环
 * ==========================================================================*/
int AaudioRender::device_write(unique_ptr<IAFFrame> &frame)
{
    if (mStream == nullptr || mStreamBroken.load()) {
        return OPEN_AUDIO_DEVICE_FAILED;
    }

    if (frame == nullptr) {
        return -EINVAL;
    }

    IAFFrame::audioInfo *audioInfo = &(frame->getInfo().audio);

    if (audioInfo->format != AF_SAMPLE_FMT_S16) {
        /* 到这里的都应该是 S16（filterAudioRender 在 renderFrame 里已经比过格式），
         * Release 下也要挡住 —— assert 只存在于 Debug。 */
        AF_LOGW("[aaudio] unexpected pcm format %d (only S16 is fed to AAudio)\n", (int) audioInfo->format);
        return FORMAT_NOT_SUPPORT;
    }

    const int len = getPCMDataLen(audioInfo->channels, static_cast<AVSampleFormat>(audioInfo->format),
                                 audioInfo->nb_samples);

    if (len <= 0 || frame->getData() == nullptr || frame->getData()[0] == nullptr) {
        return -EINVAL;
    }

    /*
     * 反压：环里放不下整帧就返回 -EAGAIN。filterAudioRender::renderLoop 收到 -EAGAIN 会
     * 收缩它的帧队列上限并稍后再试（见 filterAudioRender.cpp:284-290），于是"解码侧不会
     * 无限往前跑"，而设备侧靠回调自己把数据拉走 —— 这就是 AAudio 模型下的流控，不用计时器。
     */
    if (ringBytesFree() < (size_t) len) {
        return -EAGAIN;
    }

    if (mRenderingCb != nullptr) {
        /* 这是**应用自定义渲染**回调（`IAudioRender::setRenderingCb`）：应用想自己把这一帧
         * 交给别处渲染时注册，没注册就是 nullptr —— 它**不是**"这一帧已经交给设备"那条上报，
         * 播放器侧的位置/统计不能靠它（那条上报在下面 ring 写入成功之后）。
         * 返回值在本路径没有语义，只在需要时用于诊断。 */
        (void) mRenderingCb(mRenderingCbUserData, frame.get());
    }

    const size_t written = ringWrite(frame->getData()[0], (size_t) len);

    if (written != (size_t) len) {
        /* ringWrite 已经做过 free 检查，这里只可能出现在与回调并发消费的极端交错下。 */
        AF_LOGW("[aaudio] short ring write: %u of %d bytes\n", (unsigned) written, len);
        return -EAGAIN;
    }

    /*
     * ============ 【必须上报"这一帧已经交给设备"（与 AudioTrackRender 同一约定）】============
     *
     * 上面那个 mRenderingCb 是**应用自己渲染这一帧**的可选回调（没注册就是 nullptr），
     * 它顶替不了这条上报 —— 语义不同，而且本工程（以及任何没注册它的应用）根本不走。
     * 位置/统计要的是"这一帧真的进了设备"，那只有 onFrameInfoUpdate 表达得了。
     *
     * 为什么缺了它进度条会不动（有音轨的片源）：
     *   播放器侧唯一会在普通播放里推进内容位置的，就是这条链 ——
     *     AaudioRender::device_write
     *     → IAudioRenderListener::onFrameInfoUpdate(info, true)
     *     → SuperMediaPlayer::ApsaraAudioRenderCallback::onFrameInfoUpdate
     *     → RenderCallback(ST_TYPE_AUDIO, true, info) → MSG_INTERNAL_RENDERED
     *     → SMPMessageControllerListener::ProcessRenderedMsg(ST_TYPE_AUDIO, …) 里的
     *       `mCurrentPos = info.timePosition`（见该函数里 audio 分支）。
     *   而 getCurrentPosition() 在"还没建立过不连续点"（起播后没 seek 过，或 Reset 之后）
     *   时**逐字返回 mCurrentPos**；视频那一路在"有音轨且音频没 EOS"时被
     *   `mCurrentAudioIndex < 0 || mAudioEOS` 挡住、同样不写它。
     *   ⇒ 少了这条上报，mCurrentPos 就一直是它的初值 0：每 500ms 一次的
     *   NotifyPosition 推给界面的都是 0，进度条不动；用户手动 seek 一次之后，
     *   seek 会建立不连续点，getCurrentPosition() 改从 mMasterClock 读（那根轴由
     *   设备的 presented 帧数驱动，与本回调无关），进度条才开始走。
     *
     * 放置位置与口径都对齐 AudioTrackRender：**整帧真的被设备接收之后**才报，
     * 参数恒为 true（本接口的返回值没有语义，与 AudioTrackRender 的处理一致）。
     */
    if (mListener != nullptr) {
        mListener->onFrameInfoUpdate(frame->getInfo(), true);
    }

    return 0;
}

/* ============================================================================
 * 平台回调（AAudio 自己的线程）
 * ==========================================================================*/
aaudio_data_callback_result_t AaudioRender::dataCallbackEntry(AAudioStream *stream, void *userData,
                                                             void *audioData, int32_t numFrames)
{
    (void) stream;
    AaudioRender *render = static_cast<AaudioRender *>(userData);

    if (render == nullptr || audioData == nullptr || numFrames <= 0) {
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    render->onData(audioData, numFrames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AaudioRender::onData(void *audioData, int32_t numFrames)
{
    /*
     * 【回调纪律】这里只做四件事：memcpy、补零、软件增益、原子计数。
     * 绝不等锁、绝不 sleep、绝不打重日志（除了"这一轮欠载的第一次"那一条）。
     */
    const int bpf = bytesPerFrame();

    if (bpf <= 0 || mRing == nullptr) {
        memset(audioData, 0, (size_t) std::max(0, numFrames) * (size_t) std::max(0, bpf));
        return;
    }

    const size_t want = (size_t) bpf * (size_t) numFrames;

    if (mMute.load()) {
        /* 静音不是欠载：补零即可，不计入欠载统计（否则暂停/静音期间会刷出假的欠载）。 */
        memset(audioData, 0, want);
        return;
    }

    const size_t got = ringRead(static_cast<uint8_t *>(audioData), want);
    const float gain = mGain.load();

    if (got > 0 && gain != 1.0f) {
        /* 软件增益（硬件音量符号不存在时的回退）。只处理我们真的读到的那些样本。 */
        int16_t *samples = static_cast<int16_t *>(audioData);
        const size_t count = got / sizeof(int16_t);

        for (size_t i = 0; i < count; ++i) {
            int value = (int) ((float) samples[i] * gain);

            if (value > 32767) {
                value = 32767;
            } else if (value < -32768) {
                value = -32768;
            }

            samples[i] = (int16_t) value;
        }
    }

    if (got < want) {
        /* 欠载：补零填满这一块（沉默但不断流）。这就是 AAudio 模型下"永远不需要静音保活"的地方 ——
         * 回调是平台拉取的，框架不会因为"一段时间没写入"把流停掉，所以不存在 AudioTrack 那条
         * `onAudioException -1003 → baseStop` 的形态（见 docs/ANDROID-AAUDIO-RENDER.md 第二节第 5 条）。 */
        memset(static_cast<uint8_t *>(audioData) + got, 0, want - got);
        mUnderrunFrames.store(mUnderrunFrames.load() + (uint64_t) ((want - got) / (size_t) bpf));

        if (!mUnderrunEpisodeActive.exchange(true)) {
            AF_LOGW("[aaudio] data callback underrun: the ring had %u of %u bytes — filling the rest with "
                    "silence. The platform pulls data from this callback, so the framework never judges the "
                    "stream as 'not written for too long' and never stops it: that is exactly why the "
                    "AudioTrack keep-alive machinery is not needed on this path\n",
                    (unsigned) got, (unsigned) want);
        }
    } else if (mUnderrunEpisodeActive.exchange(false)) {
        AF_LOGI("[aaudio] data callback recovered: pcm resumed, total underrun frames=%llu\n",
                (unsigned long long) mUnderrunFrames.load());
    }
}

void AaudioRender::errorCallbackEntry(AAudioStream *stream, void *userData, int32_t error)
{
    (void) stream;
    AaudioRender *render = static_cast<AaudioRender *>(userData);

    if (render != nullptr) {
        render->onError(error);
    }
}

void AaudioRender::onError(int32_t error)
{
    const AaudioApi *a = api();
    const char *text = (a != nullptr && a->convertResultToText != nullptr) ? a->convertResultToText(error) : nullptr;

    /*
     * AAudio 的 errorCallback 只在**致命**错误时被调用（例如设备被拔掉/被其它应用抢占导致
     * DISCONNECTED）。按任务要求：只上报，不做局部重试、不用计时器。
     *   · 先置 mStreamBroken ⇒ getPosition() 退回 INT64_MIN（音频时钟暂不可用，绝不假装有读数）；
     *   · 再走既有 listener 的 onInterrupt(true)（IAudioRenderListener 里语义最接近"播放被打断"的
     *     一个），让播放器侧已有的恢复逻辑决定后续；
     *   · 日志每条流的生命周期内只打一条（纯状态闩，不是限频计时器）。
     */
    mStreamBroken = true;

    if (!mErrorLogged) {
        mErrorLogged = true;
        AF_LOGE("[aaudio] fatal stream error %d (%s) — reporting an interrupt to the player; no local retry "
                "and no timer (AAudio's error callback only fires for fatal errors)\n",
                (int) error, text != nullptr ? text : "unknown");
    }

    if (mListener != nullptr) {
        mListener->onInterrupt(true);
    }
}

void AaudioRender::device_preClose()
{
    /*
     * 与 AudioTrackRender::device_preClose() 同一语义：**只停写、不释放设备**。
     * AudioTrackRender 那里只停它的写线程（不 release AudioTrack），因为上层随后还会复用
     * 同一个渲染器继续播（setUpAudioRender 只在设备 invalid 时才重建）。AAudio 侧我们
     * 没有自己的写线程，回调由平台驱动，所以这里**什么也不用做** —— 若在这里 close 流，
     * 之后的 device_write / start_device 就没有流可用，声音再也回不来。
     * 真正的释放只在析构函数里做。
     */
    AF_LOGI("[aaudio] prePause: keeping the stream open (release happens in the destructor, mirroring "
            "AudioTrackRender which also keeps its track)\n");
}
