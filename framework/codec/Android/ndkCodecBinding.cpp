//
// NDK 异步绑定的实现。设计与不变量见 ndkCodecBinding.h 与
// docs/ANDROID-NDK-ASYNC-DECODER.md。
//

#include "ndkCodecBinding.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <utility>

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <media/NdkMediaFormat.h>
#include <utils/Android/JniEnv.h>
#include <utils/frame_work_log.h>

/*
 * ============ 高于 minSdk 的 API：走**运行期符号解析**，不用 weak_import ============
 *
 * 本工程 Android 构建的 __ANDROID_API__ = minSdk = 24。下面这些 API 更高：
 *   · AMediaCodec_setAsyncNotifyCallback            API 28（本绑定的核心）
 *   · AMediaCodecActionCode_isRecoverable/_isTransient  API 28（错误分类）
 *   · AMediaCodec_setParameters                     API 26（追帧加速）
 *
 * NDK 头文件把它们标成 availability(android, introduced=N)，在 API 24 的构建里**直接调用
 * 会编译失败**（真机构建实证：`'AMediaCodec_setAsyncNotifyCallback' is unavailable:
 * introduced in Android 28`）；而自己再用 __attribute__((weak_import)) 重声明也压不住
 * 头文件里已有的 availability 属性（首个声明的属性生效）。
 *
 * 所以这里用 dlopen + dlsym 在**运行期**解析：判据直接是"这台设备的 libmediandk 到底有没有
 * 这个符号"，比比较版本号更准确，也不需要任何编译期开关；解析不到就是没有该能力，
 * 由调用方按平台能力回落到 Java 异步绑定（见 codecBinding.cpp 的路由）。
 */

using namespace Cicada;

namespace {
    /*
     * 进程内熔断状态：只写一次（第一次失败时），之后只读。
     * 用固定缓冲而不是 std::string，读侧就不必持锁、也不存在"读到正在被改写的 c_str"。
     */
    std::mutex gNdkStateMutex;
    bool gNdkUnavailable = false;
    char gNdkUnavailableReason[192] = {0};

    /* Java 侧 MediaFormat.KEY_OPERATING_RATE 的字符串，与 MediaCodecDecoder.java 一致。 */
    const char *const kOperatingRateKey = "operating-rate";
    const int kOperatingRateBoost = 240;

    bool contains(const std::deque<int32_t> &q, int32_t v)
    {
        for (size_t i = 0; i < q.size(); ++i) {
            if (q[i] == v) {
                return true;
            }
        }

        return false;
    }

    /* ==================== 运行期符号解析（见文件头说明） ==================== */
    typedef media_status_t (*SetAsyncNotifyCallbackFn)(AMediaCodec *, AMediaCodecOnAsyncNotifyCallback,
                                                       void *);
    typedef media_status_t (*SetParametersFn)(AMediaCodec *, const AMediaFormat *);
    typedef bool (*ActionCodePredicateFn)(int32_t);

    /*
     * 句柄进程内只解析一次、不 dlclose：libmediandk 是系统库，随进程存活；
     * 保留句柄也避免"库被卸载后函数指针悬垂"。
     */
    void *mediaNdkHandle()
    {
        static void *handle = dlopen("libmediandk.so", RTLD_NOW | RTLD_LOCAL);
        return handle;
    }

    SetAsyncNotifyCallbackFn setAsyncNotifyCallbackFn()
    {
        static SetAsyncNotifyCallbackFn fn = nullptr;
        static bool resolved = false;

        if (!resolved) {
            void *handle = mediaNdkHandle();

            if (handle != nullptr) {
                fn = (SetAsyncNotifyCallbackFn) dlsym(handle, "AMediaCodec_setAsyncNotifyCallback");
            }

            resolved = true;
        }

        return fn;
    }

    SetParametersFn setParametersFn()
    {
        static SetParametersFn fn = nullptr;
        static bool resolved = false;

        if (!resolved) {
            void *handle = mediaNdkHandle();

            if (handle != nullptr) {
                fn = (SetParametersFn) dlsym(handle, "AMediaCodec_setParameters");
            }

            resolved = true;
        }

        return fn;
    }

    ActionCodePredicateFn actionCodeIsRecoverableFn()
    {
        static ActionCodePredicateFn fn = nullptr;
        static bool resolved = false;

        if (!resolved) {
            void *handle = mediaNdkHandle();

            if (handle != nullptr) {
                fn = (ActionCodePredicateFn) dlsym(handle, "AMediaCodecActionCode_isRecoverable");
            }

            resolved = true;
        }

        return fn;
    }

    ActionCodePredicateFn actionCodeIsTransientFn()
    {
        static ActionCodePredicateFn fn = nullptr;
        static bool resolved = false;

        if (!resolved) {
            void *handle = mediaNdkHandle();

            if (handle != nullptr) {
                fn = (ActionCodePredicateFn) dlsym(handle, "AMediaCodecActionCode_isTransient");
            }

            resolved = true;
        }

        return fn;
    }
} // namespace

namespace Cicada {

    NdkCodecBinding::NdkCodecBinding()
    {
    }

    NdkCodecBinding::~NdkCodecBinding()
    {
        release();
    }

    bool NdkCodecBinding::isAvailable()
    {
        if (isMarkedUnavailable()) {
            return false;
        }

        /* 运行期判空：解析不到该符号 ⇒ 本设备没有 NDK 异步能力 ⇒ 走 Java 异步绑定。 */
        return setAsyncNotifyCallbackFn() != nullptr;
    }

    void NdkCodecBinding::markUnavailable(const char *why)
    {
        std::lock_guard<std::mutex> lock(gNdkStateMutex);

        if (!gNdkUnavailable) {
            gNdkUnavailable = true;
            snprintf(gNdkUnavailableReason, sizeof(gNdkUnavailableReason), "%s",
                     (why != nullptr) ? why : "unknown");
            AF_LOGW("[ndk-codec] NDK async data plane disabled for this process: %s "
                    "(后续实例交给 Java 异步绑定；两者都是异步，本设计没有同步轮询路径)\n",
                    gNdkUnavailableReason);
        }
    }

    bool NdkCodecBinding::isMarkedUnavailable()
    {
        std::lock_guard<std::mutex> lock(gNdkStateMutex);
        return gNdkUnavailable;
    }

    const char *NdkCodecBinding::unavailableReason()
    {
        std::lock_guard<std::mutex> lock(gNdkStateMutex);
        return gNdkUnavailableReason;
    }

    void NdkCodecBinding::setCodecNameProvider(CodecNameProvider provider)
    {
        mNameProvider = std::move(provider);
    }

    void NdkCodecBinding::setCodecSpecificData(
            const std::list<std::unique_ptr<CodecSpecificData>> &csds)
    {
        /*
         * 这里**必须拷贝**：内核的 mCSDList 在本对象之外，configure 之前的生命周期不受本类控制
         * （老实现是立刻经 JNI 传给 Java 的 MediaFormat，不存在"晚用"的问题）。
         */
        mCsd.clear();

        for (std::list<std::unique_ptr<CodecSpecificData>>::const_iterator it = csds.begin();
             it != csds.end(); ++it) {
            const std::unique_ptr<CodecSpecificData> &csd = *it;

            if (csd == nullptr || csd->buffer == nullptr || csd->len <= 0) {
                continue;
            }

            CsdEntry entry;
            entry.key = csd->key;
            const uint8_t *begin = (const uint8_t *) csd->buffer;
            entry.data.assign(begin, begin + csd->len);
            mCsd.push_back(entry);
        }
    }

    int NdkCodecBinding::applyCsdToFormat(AMediaFormat *format)
    {
        if (format == nullptr) {
            return MC_ERROR;
        }

        for (size_t i = 0; i < mCsd.size(); ++i) {
            AMediaFormat_setBuffer(format, mCsd[i].key.c_str(), mCsd[i].data.data(),
                                   mCsd[i].data.size());
        }

        return 0;
    }

    ANativeWindow *NdkCodecBinding::toNativeWindow(void *javaSurface)
    {
        if (javaSurface == nullptr) {
            return nullptr;
        }

        JniEnv jniEnv;
        JNIEnv *env = jniEnv.getEnv();

        if (env == nullptr) {
            AF_LOGE("[ndk-codec] no JNIEnv, cannot convert Surface to ANativeWindow\n");
            return nullptr;
        }

        return ANativeWindow_fromSurface(env, (jobject) javaSurface);
    }

    void NdkCodecBinding::releaseNativeWindow()
    {
        if (mWindow != nullptr) {
            ANativeWindow_release(mWindow);
            mWindow = nullptr;
        }
    }

    void NdkCodecBinding::resetQueuesLocked()
    {
        mInputIndices.clear();
        mOutputIndices.clear();
        mPendingOutputs.clear();
        mFormatChangedPending = false;
        mOutstandingInput = 0;
        mTotalInputIndices = 0;
    }

    int NdkCodecBinding::setDrmInfo(const std::string &uuid, const void *sessionId, int size)
    {
        /*
         * secure 内容需要 Java MediaCrypto 会话（现有 DRM 全链路在 Java 侧）。
         * 这里明确"本绑定不支持"，由调度器把整个实例交给 Java 绑定 —— 不在这里做半套 DRM。
         */
        AF_LOGW("[ndk-codec] secure content requested (uuid=%s, sessionBytes=%d): NDK 绑定不支持，"
                "本实例交由 Java 异步绑定\n", uuid.c_str(), size);
        return MC_ERROR;
    }

    void NdkCodecBinding::setForceInsecureDecoder(bool force)
    {
        /* 只影响"选哪颗 codec"（Java 侧控制面），本绑定不需要保存。 */
        (void) force;
    }

    int NdkCodecBinding::setDecodeBoost(bool boost)
    {
        if (mCodec == nullptr) {
            return MC_ERROR;
        }

        SetParametersFn setParameters = setParametersFn();

        if (setParameters == nullptr) {
            /* API 26 以下没有这个入口：追帧加速只是性能提示，静默跳过。 */
            return MC_ERROR;
        }

        AMediaFormat *params = AMediaFormat_new();

        if (params == nullptr) {
            return MC_ERROR;
        }

        /* 与 Java 侧同 key 同值：boost ? 240 : -1（OPERATING_RATE_UNSPECIFIED）。 */
        AMediaFormat_setInt32(params, kOperatingRateKey, boost ? kOperatingRateBoost : -1);
        media_status_t status = setParameters(mCodec, params);
        AMediaFormat_delete(params);
        return (status == AMEDIA_OK) ? 0 : MC_ERROR;
    }

    int NdkCodecBinding::configureVideo(const std::string &mime, int width, int height, int angle,
                                        void *surface, bool usePlaceholderSurface)
    {
        mMime = mime;
        mVideo = true;

        if (mCodec != nullptr) {
            /* 同一实例重复 configure 不在本工程的使用方式里（重建走新实例），直接拒绝以免歧义。 */
            AF_LOGE("[ndk-codec] configureVideo called twice without release\n");
            return MC_ERROR;
        }

        if (!isAvailable()) {
            return MC_ERROR;
        }

        /*
         * 占位/空 surface 走 Java 绑定：那是 B2（切档交接）里用 DummySurface 的技巧，
         * 是 Java API 侧的能力；NDK 侧 Configure(null) 会退化成 byte-buffer 输出模式，
         * 不是本内核要的 surface 直出。这是"平台能力"判断，不是行为开关。
         */
        if (surface == nullptr || usePlaceholderSurface) {
            AF_LOGI("[ndk-codec] video needs the Java placeholder/dummy surface path "
                    "(surface=%p placeholder=%d) ⇒ Java 异步绑定\n", surface,
                    (int) usePlaceholderSurface);
            return MC_ERROR;
        }

        if (!mNameProvider) {
            AF_LOGE("[ndk-codec] no codec-name provider (control plane missing)\n");
            return MC_ERROR;
        }

        /* 控制面：codec 名由 Java 侧用同一套选择逻辑给出，这里精确锁定同一颗。 */
        std::string name = mNameProvider(true, mime, width, height, 0, 0, 0);

        if (name.empty()) {
            AF_LOGE("[ndk-codec] java control plane returned no video codec for %s\n", mime.c_str());
            return MC_ERROR;
        }

        mCodec = AMediaCodec_createCodecByName(name.c_str());

        if (mCodec == nullptr) {
            markUnavailable("AMediaCodec_createCodecByName(video) failed");
            return MC_ERROR;
        }

        mFormat = AMediaFormat_new();

        if (mFormat == nullptr) {
            markUnavailable("AMediaFormat_new(video) failed");
            return MC_ERROR;
        }

        AMediaFormat_setString(mFormat, AMEDIAFORMAT_KEY_MIME, mime.c_str());
        AMediaFormat_setInt32(mFormat, AMEDIAFORMAT_KEY_WIDTH, width);
        AMediaFormat_setInt32(mFormat, AMEDIAFORMAT_KEY_HEIGHT, height);

        if (angle != 0) {
            AMediaFormat_setInt32(mFormat, "rotation-degrees", angle);
        }

        applyCsdToFormat(mFormat);

        releaseNativeWindow();
        mWindow = toNativeWindow(surface);

        if (mWindow == nullptr) {
            AF_LOGE("[ndk-codec] ANativeWindow_fromSurface failed\n");
            return MC_ERROR;
        }

        media_status_t status = AMediaCodec_configure(mCodec, mFormat, mWindow, nullptr, 0);

        if (status != AMEDIA_OK) {
            markUnavailable("AMediaCodec_configure(video) failed");
            return MC_ERROR;
        }

        AMediaCodecOnAsyncNotifyCallback callback;
        callback.onAsyncInputAvailable = &NdkCodecBinding::onInputAvailable;
        callback.onAsyncOutputAvailable = &NdkCodecBinding::onOutputAvailable;
        callback.onAsyncFormatChanged = &NdkCodecBinding::onFormatChanged;
        callback.onAsyncError = &NdkCodecBinding::onError;

        /* 必须在 start() 之前注册（否则首批输入回调会丢）。 */
        SetAsyncNotifyCallbackFn registerFn = setAsyncNotifyCallbackFn();

        if (registerFn == nullptr) {
            markUnavailable("AMediaCodec_setAsyncNotifyCallback unavailable (video)");
            return MC_ERROR;
        }

        status = registerFn(mCodec, callback, this);

        if (status != AMEDIA_OK) {
            markUnavailable("AMediaCodec_setAsyncNotifyCallback(video) failed");
            return MC_ERROR;
        }

        const char *formatText = AMediaFormat_toString(mFormat);
        AF_LOGI("[ndk-codec] ndk-async video configured: codec=%s %dx%d angle=%d surface=%p "
                "csd=%zu format=%s\n", name.c_str(), width, height, angle, surface, mCsd.size(),
                (formatText != nullptr) ? formatText : "?");

        return 0;
    }

    int NdkCodecBinding::configureAudio(const std::string &mime, int sampleRate, int channelCount,
                                        int isADTS)
    {
        mMime = mime;
        mVideo = false;

        if (mCodec != nullptr) {
            AF_LOGE("[ndk-codec] configureAudio called twice without release\n");
            return MC_ERROR;
        }

        if (!isAvailable()) {
            return MC_ERROR;
        }

        if (!mNameProvider) {
            AF_LOGE("[ndk-codec] no codec-name provider (control plane missing)\n");
            return MC_ERROR;
        }

        std::string name = mNameProvider(false, mime, 0, 0, sampleRate, channelCount, isADTS);

        if (name.empty()) {
            AF_LOGE("[ndk-codec] java control plane returned no audio codec for %s\n", mime.c_str());
            return MC_ERROR;
        }

        mCodec = AMediaCodec_createCodecByName(name.c_str());

        if (mCodec == nullptr) {
            markUnavailable("AMediaCodec_createCodecByName(audio) failed");
            return MC_ERROR;
        }

        mFormat = AMediaFormat_new();

        if (mFormat == nullptr) {
            markUnavailable("AMediaFormat_new(audio) failed");
            return MC_ERROR;
        }

        AMediaFormat_setString(mFormat, AMEDIAFORMAT_KEY_MIME, mime.c_str());
        AMediaFormat_setInt32(mFormat, AMEDIAFORMAT_KEY_SAMPLE_RATE, sampleRate);
        AMediaFormat_setInt32(mFormat, AMEDIAFORMAT_KEY_CHANNEL_COUNT, channelCount);

        /*
         * 【与 Java 侧逐字对齐】Java 的 configureAudio 是**无条件**设
         * audioFormat.setInteger(KEY_IS_ADTS, isADTS)（0/1 都设）。这里保持同样行为：
         * "显式 0"与"根本不设"在某些 codec 上并不等价（与 configure 字段逐项对齐是本阶段的验收项）。
         */
        AMediaFormat_setInt32(mFormat, AMEDIAFORMAT_KEY_IS_ADTS, isADTS ? 1 : 0);

        applyCsdToFormat(mFormat);

        /* 音频输出取直接指针（readBuffer=true），不绑 surface。 */
        media_status_t status = AMediaCodec_configure(mCodec, mFormat, nullptr, nullptr, 0);

        if (status != AMEDIA_OK) {
            markUnavailable("AMediaCodec_configure(audio) failed");
            return MC_ERROR;
        }

        AMediaCodecOnAsyncNotifyCallback callback;
        callback.onAsyncInputAvailable = &NdkCodecBinding::onInputAvailable;
        callback.onAsyncOutputAvailable = &NdkCodecBinding::onOutputAvailable;
        callback.onAsyncFormatChanged = &NdkCodecBinding::onFormatChanged;
        callback.onAsyncError = &NdkCodecBinding::onError;

        /* 必须在 start() 之前注册（否则首批输入回调会丢）。 */
        SetAsyncNotifyCallbackFn registerFn = setAsyncNotifyCallbackFn();

        if (registerFn == nullptr) {
            markUnavailable("AMediaCodec_setAsyncNotifyCallback unavailable (audio)");
            return MC_ERROR;
        }

        status = registerFn(mCodec, callback, this);

        if (status != AMEDIA_OK) {
            markUnavailable("AMediaCodec_setAsyncNotifyCallback(audio) failed");
            return MC_ERROR;
        }

        const char *formatText = AMediaFormat_toString(mFormat);
        AF_LOGI("[ndk-codec] ndk-async audio configured: codec=%s rate=%d channels=%d adts=%d "
                "csd=%zu format=%s\n", name.c_str(), sampleRate, channelCount, isADTS, mCsd.size(),
                (formatText != nullptr) ? formatText : "?");

        return 0;
    }

    int NdkCodecBinding::setOutputSurface(void *surface)
    {
        if (mCodec == nullptr || !mVideo) {
            return MC_ERROR;
        }

        if (surface == nullptr) {
            /* 与 Java 侧一致：null = 交给 Java 的 DummySurface 语义，NDK 侧不实现。 */
            return MC_ERROR;
        }

        ANativeWindow *window = toNativeWindow(surface);

        if (window == nullptr) {
            return MC_ERROR;
        }

        media_status_t status = AMediaCodec_setOutputSurface(mCodec, window);

        if (status != AMEDIA_OK) {
            ANativeWindow_release(window);
            return MC_ERROR;
        }

        releaseNativeWindow();
        mWindow = window;
        return 0;
    }

    int NdkCodecBinding::start()
    {
        if (mCodec == nullptr) {
            return MC_ERROR;
        }

        media_status_t status = AMediaCodec_start(mCodec);

        if (status != AMEDIA_OK) {
            AF_LOGE("[ndk-codec] AMediaCodec_start failed: %d\n", (int) status);
            return MC_ERROR;
        }

        mStarted = true;
        return 0;
    }

    int NdkCodecBinding::flush()
    {
        if (mCodec == nullptr) {
            return MC_ERROR;
        }

        media_status_t status = AMediaCodec_flush(mCodec);

        if (status != AMEDIA_OK) {
            AF_LOGE("[ndk-codec] AMediaCodec_flush failed: %d\n", (int) status);
            return MC_ERROR;
        }

        {
            std::lock_guard<std::mutex> lock(mQueueMutex);
            /*
             * 契约：flush 之后先前交出去的 index 全部作废、缓冲全部归 codec 所有。
             * 本地队列必须跟着作废，否则会拿着失效 index 去 queue/release。
             */
            resetQueuesLocked();
        }

        /*
         * ============ 【不变量】异步模式下 flush 之后必须 start() ============
         *
         * AOSP MediaCodec.flush() 原文："If the codec is configured in asynchronous mode,
         * call start after flush has returned to resume codec operations. The codec will not
         * request input buffers until this has happened."
         *
         * 不调这一句，平台就**永久不再回调 onInputBufferAvailable** ⇒ 解码器拿不到落点关键帧
         * ⇒ 画面永久卡死（真机已复现两次：async input path looks dead /
         * no-input-callback-after-flush）。Java 侧同一坑已修（commit 99cfd107），这里原样保留。
         */
        status = AMediaCodec_start(mCodec);

        if (status != AMEDIA_OK) {
            AF_LOGE("[ndk-codec] AMediaCodec_start after flush failed: %d\n", (int) status);
            return MC_ERROR;
        }

        mStarted = true;
        return 0;
    }

    int NdkCodecBinding::stop()
    {
        if (mCodec == nullptr) {
            return MC_ERROR;
        }

        media_status_t status = AMediaCodec_stop(mCodec);
        mStarted = false;
        return (status == AMEDIA_OK) ? 0 : MC_ERROR;
    }

    int NdkCodecBinding::release()
    {
        int ret = 0;

        if (mCodec != nullptr) {
            if (mStarted) {
                AMediaCodec_stop(mCodec);
                mStarted = false;
            }

            media_status_t status = AMediaCodec_delete(mCodec);
            ret = (status == AMEDIA_OK) ? 0 : MC_ERROR;
            mCodec = nullptr;
        }

        if (mFormat != nullptr) {
            AMediaFormat_delete(mFormat);
            mFormat = nullptr;
        }

        releaseNativeWindow();
        mCsd.clear();

        {
            std::lock_guard<std::mutex> lock(mQueueMutex);
            resetQueuesLocked();
        }

        return ret;
    }

    bool NdkCodecBinding::waitForInput(int64_t timeoutUs)
    {
        std::unique_lock<std::mutex> lock(mQueueMutex);

        if (mInputIndices.empty()) {
            mQueueCond.wait_for(lock, std::chrono::microseconds(timeoutUs));
        }

        if (mInputIndices.empty()) {
            return false;
        }

        /*
         * 【防饿死】永远至少留 1 个输入 index 不占用：codec 手里一个输入缓冲都没有时
         * 它无法推进（表现与"没有回调"一样是解码停摆，且更难查）。取用前先看总数。
         */
        if (mTotalInputIndices >= 2 && mOutstandingInput >= mTotalInputIndices - 1) {
            return false;
        }

        int32_t index = mInputIndices.front();
        mInputIndices.pop_front();
        ++mOutstandingInput;
        mPendingInputIndex = index;
        return true;
    }

    int NdkCodecBinding::dequeueInputBufferIndex(int64_t timeoutUs)
    {
        if (mCodec == nullptr) {
            return MC_ERROR;
        }

        if (mErrorPending) {
            return MC_ERROR;
        }

        if (!waitForInput(timeoutUs)) {
            return MC_INFO_TRYAGAIN;
        }

        return mPendingInputIndex;
    }

    int NdkCodecBinding::queueInputBuffer(int index, void *buffer, size_t size, int64_t pts,
                                          bool isConfig)
    {
        if (mCodec == nullptr) {
            return MC_ERROR;
        }

        size_t capacity = 0;
        uint8_t *dst = AMediaCodec_getInputBuffer(mCodec, (size_t) index, &capacity);

        if (dst == nullptr) {
            AF_LOGE("[ndk-codec] getInputBuffer(%d) returned null\n", index);

            {
                std::lock_guard<std::mutex> lock(mQueueMutex);

                if (mOutstandingInput > 0) {
                    --mOutstandingInput;
                }
            }

            return MC_ERROR;
        }

        if (size > capacity) {
            AF_LOGE("[ndk-codec] input buffer too small: need %zu, capacity %zu\n", size, capacity);

            {
                std::lock_guard<std::mutex> lock(mQueueMutex);

                if (mOutstandingInput > 0) {
                    --mOutstandingInput;
                }
            }

            return MC_ERROR;
        }

        /* 零拷贝的落点：直接写进平台给的 native 缓冲（不再 NewByteArray 到 Java 堆）。 */
        if (size > 0 && buffer != nullptr) {
            memcpy(dst, buffer, size);
        }

        uint32_t flags = isConfig ? (uint32_t) AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG : 0;
        media_status_t status = AMediaCodec_queueInputBuffer(mCodec, (size_t) index, 0, size,
                                                             (uint64_t) pts, flags);

        {
            std::lock_guard<std::mutex> lock(mQueueMutex);

            if (mOutstandingInput > 0) {
                --mOutstandingInput;
            }
        }

        if (status != AMEDIA_OK) {
            AF_LOGE("[ndk-codec] queueInputBuffer failed: %d\n", (int) status);
            return MC_ERROR;
        }

        return 0;
    }

    int NdkCodecBinding::queueSecureInputBuffer(int index, void *buffer, size_t size,
                                                IAFPacket::EncryptionInfo *pEncryptionInfo,
                                                int64_t pts, bool isConfig)
    {
        (void) index;
        (void) buffer;
        (void) size;
        (void) pEncryptionInfo;
        (void) pts;
        (void) isConfig;
        /* secure 内容由调度器整体交给 Java 绑定，这里不该被调用。 */
        AF_LOGE("[ndk-codec] queueSecureInputBuffer on NDK binding (unexpected)\n");
        return MC_ERROR;
    }

    int NdkCodecBinding::dequeueOutputBufferIndex(int64_t timeoutUs)
    {
        if (mCodec == nullptr) {
            return MC_ERROR;
        }

        std::unique_lock<std::mutex> lock(mQueueMutex);

        if (mOutputIndices.empty() && !mFormatChangedPending && !mErrorPending) {
            mQueueCond.wait_for(lock, std::chrono::microseconds(timeoutUs));
        }

        if (mErrorPending) {
            const bool fatal = mErrorFatal;
            const int status = mErrorStatus;
            mErrorPending = false;
            lock.unlock();
            AF_LOGE("[ndk-codec] output reports codec error status=%d fatal=%d\n", status,
                    (int) fatal);
            return MC_ERROR;
        }

        if (!mOutputIndices.empty()) {
            OutputEntry entry = mOutputIndices.front();
            mOutputIndices.pop_front();
            return entry.index;
        }

        if (mFormatChangedPending) {
            mFormatChangedPending = false;
            return MC_INFO_OUTPUT_FORMAT_CHANGED;
        }

        return MC_INFO_TRYAGAIN;
    }

    int NdkCodecBinding::getOutput(int index, mc_out *out, bool readBuffer)
    {
        if (out == nullptr) {
            return MC_ERROR;
        }

        if (index == MC_INFO_OUTPUT_FORMAT_CHANGED) {
            std::lock_guard<std::mutex> lock(mQueueMutex);

            if (mVideo) {
                out->conf.video.width = (unsigned int) mOutWidth;
                out->conf.video.height = (unsigned int) mOutHeight;
                out->conf.video.stride = 0;
                out->conf.video.slice_height = 0;
                out->conf.video.pixel_format = 0;
                out->conf.video.crop_left = mCropLeft;
                out->conf.video.crop_right = mCropRight;
                out->conf.video.crop_top = mCropTop;
                out->conf.video.crop_bottom = mCropBottom;
            } else {
                out->conf.audio.channel_count = mOutChannels;
                out->conf.audio.channel_mask = 0;
                out->conf.audio.sample_rate = mOutSampleRate;
                out->conf.audio.format = mOutAudioFormat;
            }

            return 0;
        }

        out->buf.index = index;
        out->buf.p_ptr = nullptr;
        out->buf.pts = INT64_MIN;
        out->buf.size = 0;
        out->b_eos = false;

        {
            std::lock_guard<std::mutex> lock(mQueueMutex);
            std::map<int32_t, OutputEntry>::iterator it = mPendingOutputs.find(index);

            if (it != mPendingOutputs.end()) {
                out->buf.pts = it->second.ptsUs;
                out->buf.size = it->second.size;
                out->b_eos = ((it->second.flags & (uint32_t) AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
                              != 0);
            }
        }

        if (readBuffer && mCodec != nullptr) {
            size_t capacity = 0;
            uint8_t *ptr = AMediaCodec_getOutputBuffer(mCodec, (size_t) index, &capacity);
            out->buf.p_ptr = ptr;

            if (ptr != nullptr && out->buf.size > (int) capacity) {
                out->buf.size = (int) capacity;
            }
        }

        return 0;
    }

    int NdkCodecBinding::releaseOutputBuffer(int index, bool render)
    {
        if (mCodec == nullptr) {
            return MC_ERROR;
        }

        {
            std::lock_guard<std::mutex> lock(mQueueMutex);
            mPendingOutputs.erase(index);
        }

        media_status_t status = AMediaCodec_releaseOutputBuffer(mCodec, (size_t) index, render);
        return (status == AMEDIA_OK) ? 0 : MC_ERROR;
    }

    /* ==================== 平台回调：只入队 + notify ==================== */

    void NdkCodecBinding::onInputAvailable(AMediaCodec *codec, void *userdata, int32_t index)
    {
        (void) codec;
        NdkCodecBinding *self = (NdkCodecBinding *) userdata;

        if (self == nullptr) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(self->mQueueMutex);

            /* flush 竞态下同一 index 可能回调两次：去重。 */
            if (!contains(self->mInputIndices, index)) {
                self->mInputIndices.push_back(index);
            }

            const int total = self->mOutstandingInput + (int) self->mInputIndices.size();

            if (total > self->mTotalInputIndices) {
                self->mTotalInputIndices = total;
            }
        }

        self->mQueueCond.notify_all();
    }

    void NdkCodecBinding::onOutputAvailable(AMediaCodec *codec, void *userdata, int32_t index,
                                            AMediaCodecBufferInfo *info)
    {
        (void) codec;
        NdkCodecBinding *self = (NdkCodecBinding *) userdata;

        if (self == nullptr) {
            return;
        }

        OutputEntry entry;
        entry.index = index;
        entry.ptsUs = (info != nullptr) ? info->presentationTimeUs : INT64_MIN;
        entry.flags = (info != nullptr) ? info->flags : 0;
        entry.size = (info != nullptr) ? info->size : 0;

        {
            std::lock_guard<std::mutex> lock(self->mQueueMutex);
            self->mOutputIndices.push_back(entry);
            self->mPendingOutputs[index] = entry;
        }

        self->mQueueCond.notify_all();
    }

    void NdkCodecBinding::onFormatChanged(AMediaCodec *codec, void *userdata, AMediaFormat *format)
    {
        (void) codec;
        NdkCodecBinding *self = (NdkCodecBinding *) userdata;

        if (self == nullptr || format == nullptr) {
            return;
        }

        int32_t width = 0;
        int32_t height = 0;
        int32_t cropLeft = -1;
        int32_t cropRight = -1;
        int32_t cropTop = -1;
        int32_t cropBottom = -1;
        int32_t sampleRate = 0;
        int32_t channels = 0;

        /* 实际输出尺寸 / 裁剪（crop 存在时以 crop 为准，与 Java 侧同一口径；
         * 读不到就保持 0 / -1，内核侧本来就会用解码前的 meta 尺寸兜底）。 */
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width);
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height);

        if (!AMediaFormat_getInt32(format, "crop-left", &cropLeft)) {
            cropLeft = -1;
        }

        if (!AMediaFormat_getInt32(format, "crop-right", &cropRight)) {
            cropRight = -1;
        }

        if (!AMediaFormat_getInt32(format, "crop-top", &cropTop)) {
            cropTop = -1;
        }

        if (!AMediaFormat_getInt32(format, "crop-bottom", &cropBottom)) {
            cropBottom = -1;
        }

        if (!AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sampleRate)) {
            sampleRate = 0;
        }

        if (!AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels)) {
            channels = 0;
        }

        /*
         * 音频输出编码：Java 侧同一处读的是 "pcm-encoding"
         * （MediaCodecDecoder.getOutputBufferInfo → info.audioFormat = getFormatInteger(format,
         * "pcm-encoding")），内核按 2=S16 / 3=U8 / 4=S32 映射。读不到就保持 S16（=2），
         * 与 Java 侧"缺省即走 S16"同义。
         */
        int32_t pcmEncoding = 2;

        if (!AMediaFormat_getInt32(format, "pcm-encoding", &pcmEncoding)) {
            pcmEncoding = 2;
        }

        {
            std::lock_guard<std::mutex> lock(self->mQueueMutex);
            self->mOutWidth = width;
            self->mOutHeight = height;
            self->mCropLeft = cropLeft;
            self->mCropRight = cropRight;
            self->mCropTop = cropTop;
            self->mCropBottom = cropBottom;

            if (sampleRate > 0) {
                self->mOutSampleRate = sampleRate;
            }

            if (channels > 0) {
                self->mOutChannels = channels;
            }

            if (!self->mVideo) {
                /* 音频：把输出编码一并交给内核（视频不读这个字段）。 */
                self->mOutAudioFormat = pcmEncoding;
            }

            self->mFormatChangedPending = true;
        }

        self->mQueueCond.notify_all();
    }

    void NdkCodecBinding::onError(AMediaCodec *codec, void *userdata, media_status_t error,
                                  int32_t actionCode, const char *detail)
    {
        (void) codec;
        NdkCodecBinding *self = (NdkCodecBinding *) userdata;

        if (self == nullptr) {
            return;
        }

        bool recoverable = false;
        bool transientError = false;
        ActionCodePredicateFn isRecoverable = actionCodeIsRecoverableFn();
        ActionCodePredicateFn isTransient = actionCodeIsTransientFn();

        if (isRecoverable != nullptr) {
            recoverable = isRecoverable(actionCode);
        }

        if (isTransient != nullptr) {
            transientError = isTransient(actionCode);
        }

        /*
         * 分类语义（头文件原文）：两者都 false ⇒ 致命，必须删除该 codec。
         * 这里只记录，由 dequeueOutputBufferIndex 上报 MC_ERROR —— 与既有
         * "错误 → close_decoder()/STATUS_EOS → 重建"路径对接，回调里不做任何动作。
         */
        const bool fatal = (!recoverable && !transientError);

        {
            std::lock_guard<std::mutex> lock(self->mQueueMutex);
            self->mErrorPending = true;
            self->mErrorStatus = (int) error;
            self->mErrorActionCode = actionCode;
            self->mErrorFatal = fatal;
        }

        AF_LOGE("[ndk-codec] onAsyncError error=%d actionCode=%d recoverable=%d transient=%d "
                "fatal=%d detail=%s\n", (int) error, actionCode, (int) recoverable,
                (int) transientError, (int) fatal, (detail != nullptr) ? detail : "-");
        self->mQueueCond.notify_all();
    }

} // namespace Cicada
