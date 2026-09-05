//
// OhosAVCodecDecoder.h
//
// Hardware video/audio decoder for HarmonyOS / OpenHarmony built on the
// OH_AVCodec native API (API 12+):
//
//   - Video:  OH_AVCodec hardware codec discovered through OH_AVCapability,
//             zero-copy surface output into an OHNativeWindow created from the
//             ArkTS XComponent surfaceId (surface mode), or NV12 buffer mode
//             when no window is attached.
//   - Audio:  software AAC/audio decode via OH_AVCodec (audio has no hardware
//             renderer requirement; the decoded PCM is fed to OhosAudioRender).
//   - DRM:    an OhosDrmHandler (OH_MediaKeySystem/OH_MediaKeySession) is
//             attached through OH_AVCodec_SetMediakeySessionConfig() so the
//             codec service decrypts CENC samples in-pipeline (Widevine L3 on
//             commercial HarmonyOS NEXT devices, ClearKey on open OpenHarmony).
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

typedef struct OH_AVCodecNative OH_AVCodec;
typedef struct OH_AVFormatNative OH_AVFormat;
typedef struct OH_AVBufferNative OH_AVBuffer;
typedef struct OHNativeWindow OHNativeWindow;
typedef struct OH_MediaKeySessionNative OH_MediaKeySession;

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
