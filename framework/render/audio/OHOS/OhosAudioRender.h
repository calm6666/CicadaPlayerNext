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
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

typedef struct OH_AudioRendererNative OH_AudioRenderer;

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
        static int32_t onStreamEvent(OH_AudioRenderer *renderer, void *userData, int32_t event);
        static int32_t onInterruptEvent(OH_AudioRenderer *renderer, void *userData,
                                        int32_t type, int32_t hint);
        static int32_t onError(OH_AudioRenderer *renderer, void *userData, int32_t error);

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
    };
} // namespace Cicada

#endif // __OHOS__
#endif // FRAMEWORK_RENDER_AUDIO_OHOS_OHOSAUDIORENDER_H
