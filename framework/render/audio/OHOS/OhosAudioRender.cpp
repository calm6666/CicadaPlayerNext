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

using namespace std;

namespace Cicada {

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
        OH_AudioStreamBuilder_SetLatencyMode(builder, AUDIOSTREAM_LATENCY_NORMAL);
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
        return 0;
    }

    int64_t OhosAudioRender::getPosition()
    {
        if (mRenderer == nullptr) {
            return INT64_MIN;
        }
        return mPlayedBytes / static_cast<int64_t>(mChannels * mBytesPerSample)
               * 1000000LL / static_cast<int64_t>(mSampleRate);
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
        std::lock_guard<std::mutex> lock(mQueueMutex);
        mPcmQueue.clear();
        mQueueReadPos = 0;
        mQueuedBytes = 0;
        mPlayedBytes = 0;
        if (mRenderer != nullptr) {
            OH_AudioRenderer_Flush(mRenderer);
        }
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

    int32_t OhosAudioRender::onStreamEvent(OH_AudioRenderer *renderer, void *userData, int32_t event)
    {
        (void) renderer;
        (void) userData;
        return 0;
    }

    int32_t OhosAudioRender::onInterruptEvent(OH_AudioRenderer *renderer, void *userData,
                                             int32_t type, int32_t hint)
    {
        (void) renderer;
        (void) userData;
        (void) type;
        (void) hint;
        return 0;
    }

    int32_t OhosAudioRender::onError(OH_AudioRenderer *renderer, void *userData, int32_t error)
    {
        AF_LOGE("OH_AudioRenderer error %d\n", error);
        return 0;
    }

} // namespace Cicada

#endif // __OHOS__
