//
// OhosAVCodecDecoder.h
//
// Hardware video/audio decoder for HarmonyOS / OpenHarmony built on the
// OH_AVCodec native API (API 12+). The NDK declares its codec surface once per
// media type -- OH_VideoDecoder_* and OH_AudioCodec_* -- so this class keeps a
// single OH_AVCodec handle and dispatches every call to the matching family:
//
//   - Video:  hardware decoder taken from the framework capabilities
//             (OH_AVCodec_GetCapabilityByCategory + OH_AVCapability_GetName ->
//             OH_VideoDecoder_CreateByName), zero-copy surface output into an
//             OHNativeWindow created from the ArkTS XComponent surfaceId
//             (surface mode), or NV12 buffer mode when no window is attached.
//   - Audio:  software AAC/audio decode via OH_AudioCodec_* (audio has no
//             hardware renderer requirement; the decoded PCM is fed to
//             OhosAudioRender).
//   - DRM:    an OhosDrmHandler (DRM Kit) media key session is attached with
//             OH_VideoDecoder_SetDecryptionConfig() /
//             OH_AudioCodec_SetDecryptionConfig() so the codec service decrypts
//             CENC samples in-pipeline (Widevine L3 on commercial HarmonyOS
//             NEXT devices, ClearKey on open OpenHarmony).
//

#ifndef FRAMEWORK_CODEC_OHOS_OHOSAVCODECDECODER_H
#define FRAMEWORK_CODEC_OHOS_OHOSAVCODECDECODER_H

#ifdef __OHOS__

#include <codec/IDecoder.h>
#include <codec/ActiveDecoder.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>

// Do NOT re-declare the SDK's opaque types here. Inventing struct tags such as
// "struct OH_AVFormatNative" collides with the real typedefs in the multimedia
// headers ("typedef struct OH_AVFormat OH_AVFormat;"), after which the SDK
// headers themselves fail with
//   error: typedef 'OH_AVFormat' cannot be referenced with a struct specifier
// Pull in the SDK declarations instead.
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <native_window/external_window.h>

// NOTE: "OH_MediaKeySession" is not a type of the OpenHarmony NDK -- the DRM Kit
// names its opaque session type MediaKeySession
// (<multimedia/drm_framework/native_drm_common.h>), and that is what the decoder
// decryption entry points take. Nothing in this header names it, and the .cpp
// gets it from <native_avcodec_videodecoder.h>/<native_avcodec_audiocodec.h>.

namespace Cicada {

    class OhosDrmHandler;

    class OhosAVCodecDecoder : public ActiveDecoder {
    public:
        OhosAVCodecDecoder();
        ~OhosAVCodecDecoder() override;

        bool supportReuse() override
        {
            return false;
        }

        /** Codec capability check used by decoderFactory (public entry). */
        static bool checkSupport(const Stream_meta &meta, uint64_t flags, int maxSize);

    private:
        int init_decoder(const Stream_meta *meta, void *wnd, uint64_t flags,
                         const DrmInfo *drmInfo) override;

        void close_decoder() override;

        int enqueue_decoder(std::unique_ptr<IAFPacket> &pPacket) override;

        int dequeue_decoder(std::unique_ptr<IAFFrame> &pFrame) override;

        void flush_decoder() override;

        int get_decoder_recover_size() override
        {
            return 0;
        }

        void decoder_updateMetaData(const Stream_meta *meta) override
        {}

    private:
        static const char *codecToMime(AFCodecID codecId);

        void drainInputQueueLocked();
        bool waitForInputBuffer(OH_AVBuffer **outBuffer, uint32_t *outIndex, int timeoutUs);
        void pushOutputFrame(int64_t ptsUs, int64_t durationUs, bool eos);
        int handleOutputBuffer(uint32_t index, OH_AVBuffer *buffer);

        // static OH_AVCodec callbacks
        static void onError(OH_AVCodec *codec, int32_t errorCode, void *userData);
        static void onStreamChanged(OH_AVCodec *codec, OH_AVFormat *format, void *userData);
        static void onNeedInputBuffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *userData);
        static void onNewOutputBuffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *userData);

    private:
        OH_AVCodec *mCodec{nullptr};
        OHNativeWindow *mWindow{nullptr};   // borrowed from SetView (ArkTS XComponent)
        bool mSurfaceMode{false};
        bool mIsAudio{false};
        std::string mMime{};

        int mWidth{0};
        int mHeight{0};
        int mSampleRate{0};
        int mChannels{0};

        bool mbInit{false};
        bool mInputEosSent{false};
        bool mOutputEos{false};

        // input side: packets waiting for an OH_AVBuffer
        std::mutex mInputMutex;
        std::condition_variable mInputCond;
        std::deque<std::unique_ptr<IAFPacket>> mPendingInputs;

        // output side: decoded frames handed over by onNewOutputBuffer
        std::mutex mOutputMutex;
        std::condition_variable mOutputCond;
        std::deque<std::unique_ptr<IAFFrame>> mOutputFrames;

        std::mutex mFlushMutex;
        bool mFlushing{false};

        std::shared_ptr<OhosDrmHandler> mDrmHandler{nullptr};
    };
} // namespace Cicada

#endif // __OHOS__
#endif // FRAMEWORK_CODEC_OHOS_OHOSAVCODECDECODER_H
