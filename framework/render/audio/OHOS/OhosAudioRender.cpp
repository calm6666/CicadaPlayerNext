//
// OhosAudioRender.cpp
//
// OHAudio renderer implementation. See OhosAudioRender.h.
//

#define LOG_TAG "OhosAudioRender"

#ifdef __OHOS__

#include "OhosAudioRender.h"
#include <utils/frame_work_log.h>
#include <utils/timer.h>

#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostreambuilder.h>
#include <ohaudio/native_audiostream_base.h>

#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <mutex>

using namespace std;

namespace Cicada {

    namespace {
        /*
         * OH_AudioRenderer_GetAudioTimestampInfo 是 API 15 才引入的符号，而本 SDK 的
         * compatibleSdkVersion 是 5.0.0(12)：直接调用会让 12~14 的设备在**加载期**就
         * 找不到符号。所以按"能力路由"在运行期解析（与 Android 侧解析 API 28 的异步
         * 回调同一手法，见 docs/ANDROID-NDK-ASYNC-DECODER.md），拿不到就退回 API 10
         * 起就存在的 OH_AudioRenderer_GetTimestamp。
         */
        using GetAudioTimestampInfoFn = OH_AudioStream_Result (*)(OH_AudioRenderer *, int64_t *, int64_t *);

        GetAudioTimestampInfoFn resolveGetAudioTimestampInfo()
        {
            static std::once_flag once;
            static GetAudioTimestampInfoFn fn = nullptr;
            std::call_once(once, []() {
                fn = reinterpret_cast<GetAudioTimestampInfoFn>(
                        dlsym(RTLD_DEFAULT, "OH_AudioRenderer_GetAudioTimestampInfo"));
            });
            return fn;
        }
    } // namespace

    OhosAudioRender::OhosAudioRender() = default;

    OhosAudioRender::~OhosAudioRender()
    {
        if (mRenderer != nullptr) {
            OH_AudioRenderer_Stop(mRenderer);
            OH_AudioRenderer_Release(mRenderer);
            mRenderer = nullptr;
        }
    }

    int OhosAudioRender::init(const IAFFrame::audioInfo *info)
    {
        if (info == nullptr) {
            return -EINVAL;
        }

        mSampleRate = info->sample_rate;
        mChannels = info->channels;
        // The pipeline feeds S16 PCM; other formats are resampled upstream.
        if (info->format != AF_SAMPLE_FMT_S16) {
            AF_LOGW("OhosAudioRender: format %d not S16, will still assume S16LE\n", info->format);
        }
        mBytesPerSample = 2;

        OH_AudioStreamBuilder *builder = nullptr;
        if (OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_RENDERER) != AUDIOSTREAM_SUCCESS) {
            return OPEN_AUDIO_DEVICE_FAILED;
        }

        OH_AudioStreamBuilder_SetSamplingRate(builder, mSampleRate);
        OH_AudioStreamBuilder_SetChannelCount(builder, mChannels);
        OH_AudioStreamBuilder_SetSampleFormat(builder, AUDIOSTREAM_SAMPLE_S16LE);
        OH_AudioStreamBuilder_SetEncodingType(builder, AUDIOSTREAM_ENCODING_TYPE_RAW);
        OH_AudioStreamBuilder_SetLatencyMode(builder, AUDIOSTREAM_LATENCY_MODE_NORMAL);
        OH_AudioStreamBuilder_SetRendererInfo(builder, AUDIOSTREAM_USAGE_MUSIC);

        OH_AudioRenderer_Callbacks callbacks{};
        callbacks.OH_AudioRenderer_OnWriteData = onWriteData;
        callbacks.OH_AudioRenderer_OnStreamEvent = onStreamEvent;
        callbacks.OH_AudioRenderer_OnInterruptEvent = onInterruptEvent;
        callbacks.OH_AudioRenderer_OnError = onError;
        OH_AudioStreamBuilder_SetRendererCallback(builder, callbacks, this);

        OH_AudioRenderer *renderer = nullptr;
        OH_AudioStream_Result result = OH_AudioStreamBuilder_GenerateRenderer(builder, &renderer);
        OH_AudioStreamBuilder_Destroy(builder);

        if (result != AUDIOSTREAM_SUCCESS || renderer == nullptr) {
            AF_LOGE("GenerateRenderer failed %d\n", result);
            return OPEN_AUDIO_DEVICE_FAILED;
        }

        mRenderer = renderer;
        mPaused = false;
        mPlayedBytes = 0;
        mPresentedFrameBase.store(0);
        return OH_AudioRenderer_Start(mRenderer) == AUDIOSTREAM_SUCCESS ? 0 : OPEN_AUDIO_DEVICE_FAILED;
    }

    int OhosAudioRender::renderFrame(std::unique_ptr<IAFFrame> &frame, int timeOut)
    {
        if (frame == nullptr || mRenderer == nullptr) {
            return -EINVAL;
        }

        uint8_t **data = frame->getData();
        int *lineSize = frame->getLineSize();
        if (data == nullptr || data[0] == nullptr || lineSize == nullptr || lineSize[0] <= 0) {
            return 0;
        }

        size_t bytes = static_cast<size_t>(lineSize[0]);
        {
            std::unique_lock<std::mutex> lock(mQueueMutex);
            // Bound the queue to a couple of seconds of audio.
            size_t maxBytes = static_cast<size_t>(mSampleRate * mChannels * mBytesPerSample * 4);
            if (mQueuedBytes + bytes > maxBytes) {
                if (timeOut == 0) {
                    return -EAGAIN;
                }
                mQueueCond.wait_for(lock, std::chrono::microseconds(timeOut));
            }
            if (mQueuedBytes + bytes > maxBytes) {
                return -EAGAIN;
            }
            mPcmQueue.insert(mPcmQueue.end(), data[0], data[0] + bytes);
            mQueuedBytes += bytes;
        }
        mQueueCond.notify_all();

        /*
         * ============ 【整帧已经交给设备：上报 + 交出所有权（两个都必须有）】============
         *
         * 走到这里，这一帧的 PCM 已经**整段**进了设备的取数队列（OnWriteData 正是从这里拉），
         * 这就是本渲染器上"整帧真的被设备消费"的那一点，判据是纯结构性的：
         *   · 队列放不下时上面直接 return -EAGAIN，一个字节都没写进去 ⇒ 这里只剩"整帧成功"；
         *   · 写进去的字节数恒等于整帧的 PCM 长度（本路径没有"部分写入"的语义）。
         * 因此它与 Android 的 AudioTrackRender（:691-693，设备接收整帧之后才报）和
         * AaudioRender（整帧进设备环之后才报）是同一口径；**不能**在"写尝试"处上报，
         * 那会让播放位置超前于真正送出去的音频。
         *
         * 【为什么必须上报】播放器侧唯一会在普通播放里推进内容位置 mCurrentPos 的写点就是
         *   onFrameInfoUpdate → ApsaraAudioRenderCallback → RenderCallback(ST_TYPE_AUDIO, true,
         *   info) → MSG_INTERNAL_RENDERED → ProcessRenderedMsg 里的
         *   `mCurrentPos = info.timePosition`；
         * 而 getCurrentPosition() 在"还没建立过不连续点"（起播后没 seek 过、或 Reset 之后）
         * 时逐字返回 mCurrentPos，视频那一路在有音轨时又被
         * `mCurrentAudioIndex < 0 || mAudioEOS` 挡住不写它 ⇒ 少了这条上报，位置恒为初值 0，
         * 进度条不动，直到用户手动 seek 一次（那时位置改从 mMasterClock 读）才开始走。
         *
         * 【为什么必须交出所有权】`IAudioRender::renderFrame` 的约定是"返回成功即接走这一帧"：
         * 基类 filterAudioRender 用 `mFrameQue.push(std::move(frame))`（filterAudioRender.cpp:157），
         * 另一个直接实现 SdlAFAudioRender 用 `frame = nullptr`（SdlAFAudioRender.cpp:152）。
         * 播放器就是按"回到自己手里时那个 unique_ptr 已经为空"来判断该出队的
         * （SuperMediaPlayer.cpp:4729-4734）。这里以前既不置空也不上报，于是：
         *   ① 播放器每次 render() 都拿**同一帧**再调一次 renderFrame，同一段 PCM 被反复灌进
         *      设备队列（直到队列满），音频实际上是在循环这一帧；
         *   ② 出队之后才会执行的音频记账（mPlayedAudioPts / mAudioTime 建立、纯音频片源的
         *      NotifyFirstFrame、音频切流通知、seek 窗口内的 mCurrentPos 兜底）在这条路上
         *      从不执行。
         * 两个后果都与本次"位置不动"同源，所以一起修；上报必须在置空之前（要读帧自己的 info）。
         */
        if (mListener != nullptr) {
            mListener->onFrameInfoUpdate(frame->getInfo(), true);
        }

        frame = nullptr;
        return 0;
    }

    int64_t OhosAudioRender::getPosition()
    {
        if (mRenderer == nullptr || mSampleRate <= 0) {
            return INT64_MIN;
        }

        int64_t presentedFrames = 0;
        int64_t timestampNs = 0;
        if (!readDeviceFramePosition(presentedFrames, timestampNs)) {
            // 设备时间戳拿不到：宁可报"不可用"，也不要拿"写出去多少"冒充"播了多少"。
            return INT64_MIN;
        }

        int64_t base = mPresentedFrameBase.load();
        if (presentedFrames < base) {
            // 设备侧计数被重置（重新起流 / 换输出设备）：就地重钉基线，绝不返回负值。
            mPresentedFrameBase.store(presentedFrames);
            base = presentedFrames;
        }
        const int64_t frames = presentedFrames - base;
        return frames * 1000000LL / static_cast<int64_t>(mSampleRate);
    }

    bool OhosAudioRender::readDeviceFramePosition(int64_t &frames, int64_t &timestampNs)
    {
        if (mRenderer == nullptr) {
            return false;
        }

        int64_t framePosition = 0;
        int64_t timestamp = 0;
        const GetAudioTimestampInfoFn getInfo = resolveGetAudioTimestampInfo();
        if (getInfo != nullptr && getInfo(mRenderer, &framePosition, &timestamp) == AUDIOSTREAM_SUCCESS) {
            frames = framePosition;
            timestampNs = timestamp;
            return true;
        }
        if (OH_AudioRenderer_GetTimestamp(mRenderer, CLOCK_MONOTONIC, &framePosition, &timestamp) ==
            AUDIOSTREAM_SUCCESS) {
            frames = framePosition;
            timestampNs = timestamp;
            return true;
        }
        return false;
    }

    void OhosAudioRender::mute(bool bMute)
    {
        if (mRenderer != nullptr) {
            OH_AudioRenderer_SetVolume(mRenderer, bMute ? 0.0f : mVolume.load());
        }
    }

    int OhosAudioRender::setVolume(float volume)
    {
        mVolume = volume;
        if (mRenderer != nullptr) {
            return OH_AudioRenderer_SetVolume(mRenderer, volume) == AUDIOSTREAM_SUCCESS ? 0 : -1;
        }
        return 0;
    }

    int OhosAudioRender::setSpeed(float speed)
    {
        mSpeed = speed;
        if (mRenderer != nullptr) {
            return OH_AudioRenderer_SetSpeed(mRenderer, speed) == AUDIOSTREAM_SUCCESS ? 0 : -1;
        }
        return 0;
    }

    void OhosAudioRender::pause(bool bPause)
    {
        if (mRenderer == nullptr) {
            return;
        }
        if (bPause && !mPaused.load()) {
            OH_AudioRenderer_Pause(mRenderer);
            mPaused = true;
        } else if (!bPause && mPaused.load()) {
            OH_AudioRenderer_Start(mRenderer);
            mPaused = false;
        }
    }

    void OhosAudioRender::flush()
    {
        /*
         * 顺序有纪律，四步都不能换（与 Android 侧 flush 同一套）：
         *   1) 先暂停设备 —— 在途的 OnWriteData 返回后不会再拉新数据，
         *      否则清完队列马上又被回调送进旧数据；
         *   2) 清本地队列（拿队列锁），但**不持锁**调用设备接口：
         *      OH_AudioRenderer_Flush 要等在途的写回调返回，而写回调要拿同一把
         *      队列锁，持锁调用就是必然自锁（表现为 seek/切档后整条音频卡死）；
         *   3) 清设备缓冲，并重钉"已播出帧"基线，让 getPosition() 从 0 重新开始；
         *   4) 恢复原先的运行态。
         */
        const bool wasPaused = mPaused.load();
        if (mRenderer != nullptr && !wasPaused) {
            OH_AudioRenderer_Pause(mRenderer);
            mPaused = true;
        }

        {
            std::lock_guard<std::mutex> lock(mQueueMutex);
            mPcmQueue.clear();
            mQueueReadPos = 0;
            mQueuedBytes = 0;
            mPlayedBytes = 0;
        }

        if (mRenderer != nullptr) {
            OH_AudioRenderer_Flush(mRenderer);
            int64_t presentedFrames = 0;
            int64_t timestampNs = 0;
            mPresentedFrameBase.store(readDeviceFramePosition(presentedFrames, timestampNs)
                                              ? presentedFrames
                                              : 0);
        } else {
            mPresentedFrameBase.store(0);
        }

        if (mRenderer != nullptr && !wasPaused) {
            OH_AudioRenderer_Start(mRenderer);
            mPaused = false;
        }
        mQueueCond.notify_all();
    }

    uint64_t OhosAudioRender::getQueDuration()
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        return static_cast<uint64_t>(mQueuedBytes / (mChannels * mBytesPerSample)
                                     * 1000000LL / mSampleRate);
    }

    void OhosAudioRender::prePause()
    {
        pause(true);
    }

    // -------------------------------------------------------------- callbacks --

    int32_t OhosAudioRender::onWriteData(OH_AudioRenderer *renderer, void *userData,
                                        void *buffer, int32_t length)
    {
        auto *self = static_cast<OhosAudioRender *>(userData);
        if (self == nullptr || buffer == nullptr || length <= 0) {
            return 0;
        }

        std::lock_guard<std::mutex> lock(self->mQueueMutex);
        size_t available = self->mQueuedBytes;
        if (available == 0) {
            memset(buffer, 0, static_cast<size_t>(length));
            return length; // underrun: play silence
        }
        int32_t written = static_cast<int32_t>(std::min<size_t>(static_cast<size_t>(length), available));

        // Copy out of the ring (simple erase-from-front deque).
        auto *dst = static_cast<uint8_t *>(buffer);
        memcpy(dst, self->mPcmQueue.data(), written);
        self->mPcmQueue.erase(self->mPcmQueue.begin(), self->mPcmQueue.begin() + written);
        self->mQueuedBytes -= written;
        self->mPlayedBytes += written;

        if (written < length) {
            memset(dst + written, 0, static_cast<size_t>(length - written));
        }
        self->mQueueCond.notify_all();
        return length;
    }

    int32_t OhosAudioRender::onStreamEvent(OH_AudioRenderer *renderer, void *userData,
                                           OH_AudioStream_Event event)
    {
        (void) renderer;
        (void) userData;
        (void) event;
        return 0;
    }

    int32_t OhosAudioRender::onInterruptEvent(OH_AudioRenderer *renderer, void *userData,
                                             OH_AudioInterrupt_ForceType type, OH_AudioInterrupt_Hint hint)
    {
        (void) renderer;
        (void) userData;
        (void) type;
        (void) hint;
        return 0;
    }

    int32_t OhosAudioRender::onError(OH_AudioRenderer *renderer, void *userData, OH_AudioStream_Result error)
    {
        AF_LOGE("OH_AudioRenderer error %d\n", (int) error);
        return 0;
    }

} // namespace Cicada

#endif // __OHOS__
