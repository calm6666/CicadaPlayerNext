//
// OhosAudioRender.h
//
// PCM audio output for HarmonyOS / OpenHarmony via OHAudio's
// OH_AudioRenderer (libohaudio.so). The renderer is callback/pull-driven:
// decoded PCM frames are queued by renderFrame() and consumed inside
// OH_AudioRenderer_OnWriteData on the audio service thread.
//

#ifndef FRAMEWORK_RENDER_AUDIO_OHOS_OHOSAUDIORENDER_H
#define FRAMEWORK_RENDER_AUDIO_OHOS_OHOSAUDIORENDER_H

#ifdef __OHOS__

#include <render/audio/IAudioRender.h>

// Use the SDK's own declarations. Re-declaring the opaque type with an invented
// tag ("struct OH_AudioRendererNative") collides with the real typedef in
// ohaudio/native_audiostream_base.h ("typedef struct OH_AudioRendererStruct
// OH_AudioRenderer;") and the SDK header then fails with
//   error: typedef redefinition with different types
#include <ohaudio/native_audiostream_base.h>
#include <ohaudio/native_audiorenderer.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

namespace Cicada {

    class OhosAudioRender : public IAudioRender {
    public:
        OhosAudioRender();
        ~OhosAudioRender() override;

        bool isSupport(AFCodecID id) override
        {
            return id == AF_CODEC_ID_NONE;
        }

        int init(const IAFFrame::audioInfo *info) override;

        int renderFrame(std::unique_ptr<IAFFrame> &frame, int timeOut) override;

        int renderFrame(std::unique_ptr<IAFPacket> &packet, int timeOut) override
        {
            return -ENOTSUP;
        }

        int64_t getPosition() override;

        void mute(bool bMute) override;

        int setVolume(float volume) override;

        int setSpeed(float speed) override;

        void pause(bool bPause) override;

        void flush() override;

        uint64_t getQueDuration() override;

        void prePause() override;

    private:
        static int32_t onWriteData(OH_AudioRenderer *renderer, void *userData,
                                   void *buffer, int32_t length);
        // The SDK passes the real enum types here, not int32_t.
        static int32_t onStreamEvent(OH_AudioRenderer *renderer, void *userData,
                                     OH_AudioStream_Event event);
        static int32_t onInterruptEvent(OH_AudioRenderer *renderer, void *userData,
                                        OH_AudioInterrupt_ForceType type, OH_AudioInterrupt_Hint hint);
        static int32_t onError(OH_AudioRenderer *renderer, void *userData, OH_AudioStream_Result error);

        // 读设备"已播出帧数"：优先 API 15 的 GetAudioTimestampInfo（运行期解析），
        // 退回 API 10 的 GetTimestamp。成功返回 true。
        bool readDeviceFramePosition(int64_t &frames, int64_t &timestampNs);

    private:
        OH_AudioRenderer *mRenderer{nullptr};
        int mSampleRate{48000};
        int mChannels{2};
        int mBytesPerSample{2};

        std::mutex mQueueMutex;
        std::condition_variable mQueueCond;
        std::vector<uint8_t> mPcmQueue;
        size_t mQueueReadPos{0};
        size_t mQueuedBytes{0};

        std::atomic<bool> mPaused{false};
        std::atomic<float> mVolume{1.0f};
        std::atomic<float> mSpeed{1.0f};
        int64_t mPlayedBytes{0};

        /*
         * 【音频时钟的基线】getPosition() =（设备已播出帧 - 本基线）换算成微秒。
         * 基线在每次 flush 之后重钉，因此位置语义与 Android 侧完全一致：
         * "自上一次 flush 起设备真正播出去的时长"；设备时间戳拿不到时返回
         * INT64_MIN（表示不可用），绝不返回负值、也不拿"写出去多少"冒充"播了多少"。
         */
        std::atomic<int64_t> mPresentedFrameBase{0};
    };
} // namespace Cicada

#endif // __OHOS__
#endif // FRAMEWORK_RENDER_AUDIO_OHOS_OHOSAUDIORENDER_H
