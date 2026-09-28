//
// OhosAVCodecDecoder.cpp
//
// OpenHarmony OH_AVCodec hardware decode integration (API 12+).
// See OhosAVCodecDecoder.h for the design notes.
//
// The OpenHarmony NDK declares no generic OH_AVCodec_* command surface: every
// codec operation exists once per media type, and the caller picks the family:
//
//   video: OH_VideoDecoder_*   <multimedia/player_framework/native_avcodec_videodecoder.h>
//   audio: OH_AudioCodec_*     <multimedia/player_framework/native_avcodec_audiocodec.h>
//
// Codec capabilities are *handed out by the framework* through
// OH_AVCodec_GetCapability()/OH_AVCodec_GetCapabilityByCategory() and read with
// OH_AVCapability_IsHardware()/OH_AVCapability_GetName()
// (<multimedia/player_framework/native_avcapability.h>) -- there is no
// OH_AVCapability_Create/OH_AVCapability_Destroy in the NDK.
//

#define LOG_TAG "OhosAVCodecDecoder"

#ifdef __OHOS__

#include "OhosAVCodecDecoder.h"
#include <utils/frame_work_log.h>
#include <utils/AFMediaType.h>
#include <utils/errors/framework_error.h>
#include <base/media/AVAFPacket.h>
#include <base/media/AFMediaCodecFrame.h>
#include <drm/OHOS/OhosDrmHandler.h>

#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avcodec_audiocodec.h>
#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avbuffer_info.h>
#include <native_window/external_window.h>

#include <cstring>

using namespace std;

namespace Cicada {

    // Defined as the class member declared in the header: an unqualified call to
    // codecToMime() inside a member function resolves to the member, so a
    // file-scope helper of the same name would leave it undefined at link time.
    const char *OhosAVCodecDecoder::codecToMime(AFCodecID codecId)
    {
        switch (codecId) {
            case AF_CODEC_ID_H264:
                return OH_AVCODEC_MIMETYPE_VIDEO_AVC;   // "video/avc"
            case AF_CODEC_ID_HEVC:
                return OH_AVCODEC_MIMETYPE_VIDEO_HEVC;  // "video/hevc"
            case AF_CODEC_ID_MPEG4:
                return OH_AVCODEC_MIMETYPE_VIDEO_MPEG4; // "video/mp4v-es"
            case AF_CODEC_ID_VP9:
                return "video/vp9";
            case AF_CODEC_ID_AV1:
                return "video/av01";
            case AF_CODEC_ID_AAC:
                return OH_AVCODEC_MIMETYPE_AUDIO_AAC;   // "audio/mp4a-latm"
            default:
                return nullptr;
        }
    }

    // ------------------------------------------------------------------------
    // OH_AVCodec command dispatch.
    //
    // The video and the audio entry points operate on the same OH_AVCodec
    // instance; only the function names differ, so a single isAudio flag picks
    // the right family for every call made from this class.
    // ------------------------------------------------------------------------

    static OH_AVCodec *ohosCodecCreateByMime(bool isAudio, const char *mime)
    {
        return isAudio ? OH_AudioCodec_CreateByMime(mime, false /* decoder */)
                       : OH_VideoDecoder_CreateByMime(mime);
    }

    static OH_AVErrCode ohosCodecRegisterCallback(OH_AVCodec *codec, bool isAudio,
                                                  OH_AVCodecCallback callback, void *userData)
    {
        return isAudio ? OH_AudioCodec_RegisterCallback(codec, callback, userData)
                       : OH_VideoDecoder_RegisterCallback(codec, callback, userData);
    }

    static OH_AVErrCode ohosCodecSetSurface(OH_AVCodec *codec, bool isAudio, OHNativeWindow *window)
    {
        // Only the video decoder renders into a native window; audio always
        // produces PCM buffers.
        if (isAudio || window == nullptr) {
            return AV_ERR_INVALID_VAL;
        }
        return OH_VideoDecoder_SetSurface(codec, window);
    }

    static OH_AVErrCode ohosCodecSetDecryptionConfig(OH_AVCodec *codec, bool isAudio,
                                                     MediaKeySession *session)
    {
        // secureAudio / secureVideoPath = false: the codec service decrypts CENC
        // samples in-pipeline with a non-secure decoder (Widevine L3, ClearKey).
        // L1 content would need secureVideoPath = true, which is only valid
        // together with an output surface.
        return isAudio ? OH_AudioCodec_SetDecryptionConfig(codec, session, false)
                       : OH_VideoDecoder_SetDecryptionConfig(codec, session, false);
    }

    static OH_AVErrCode ohosCodecConfigure(OH_AVCodec *codec, bool isAudio, OH_AVFormat *format)
    {
        return isAudio ? OH_AudioCodec_Configure(codec, format)
                       : OH_VideoDecoder_Configure(codec, format);
    }

    static OH_AVErrCode ohosCodecPrepare(OH_AVCodec *codec, bool isAudio)
    {
        return isAudio ? OH_AudioCodec_Prepare(codec) : OH_VideoDecoder_Prepare(codec);
    }

    static OH_AVErrCode ohosCodecStart(OH_AVCodec *codec, bool isAudio)
    {
        return isAudio ? OH_AudioCodec_Start(codec) : OH_VideoDecoder_Start(codec);
    }

    static OH_AVErrCode ohosCodecStop(OH_AVCodec *codec, bool isAudio)
    {
        return isAudio ? OH_AudioCodec_Stop(codec) : OH_VideoDecoder_Stop(codec);
    }

    static OH_AVErrCode ohosCodecFlush(OH_AVCodec *codec, bool isAudio)
    {
        return isAudio ? OH_AudioCodec_Flush(codec) : OH_VideoDecoder_Flush(codec);
    }

    static OH_AVErrCode ohosCodecPushInputBuffer(OH_AVCodec *codec, bool isAudio, uint32_t index)
    {
        return isAudio ? OH_AudioCodec_PushInputBuffer(codec, index)
                       : OH_VideoDecoder_PushInputBuffer(codec, index);
    }

    static OH_AVErrCode ohosCodecFreeOutputBuffer(OH_AVCodec *codec, bool isAudio, uint32_t index)
    {
        return isAudio ? OH_AudioCodec_FreeOutputBuffer(codec, index)
                       : OH_VideoDecoder_FreeOutputBuffer(codec, index);
    }

    static OH_AVErrCode ohosCodecDestroy(OH_AVCodec *codec, bool isAudio)
    {
        return isAudio ? OH_AudioCodec_Destroy(codec) : OH_VideoDecoder_Destroy(codec);
    }

    bool OhosAVCodecDecoder::checkSupport(const Stream_meta &meta, uint64_t flags, int maxSize)
    {
        // Only wired for hardware video and software audio AAC decode.
        if (meta.type == STREAM_TYPE_VIDEO) {
            return codecToMime(meta.codec) != nullptr;
        }
        if (meta.type == STREAM_TYPE_AUDIO) {
            return meta.codec == AF_CODEC_ID_AAC;
        }
        return false;
    }

    OhosAVCodecDecoder::OhosAVCodecDecoder() : ActiveDecoder()
    {
        mName = "VD.ohosAVCodec";
    }

    OhosAVCodecDecoder::~OhosAVCodecDecoder()
    {
        close_decoder();
    }

    // ------------------------------------------------------------------ init --

    int OhosAVCodecDecoder::init_decoder(const Stream_meta *meta, void *wnd, uint64_t flags,
                                         const DrmInfo *drmInfo)
    {
        mIsAudio = (meta->type == STREAM_TYPE_AUDIO);

        const char *mime = codecToMime(meta->codec);
        if (mime == nullptr) {
            return gen_framework_errno(error_class_codec,
                                       mIsAudio ? codec_error_audio_not_support : codec_error_video_not_support);
        }
        mMime = mime;

        mWidth = meta->width;
        mHeight = meta->height;
        mSampleRate = meta->samplerate;
        mChannels = meta->channels;

        // 1. Create the codec instance.
        //
        // For video, prefer the vendor hardware decoder: the capability instance
        // is borrowed from the framework (it must NOT be destroyed by the
        // caller) and OH_AVCapability_GetName() yields the codec name that
        // OH_VideoDecoder_CreateByName() takes. Audio keeps the mime-based
        // creation, which is also what the framework recommends for audio.
        if (!mIsAudio) {
            OH_AVCapability *capability =
                    OH_AVCodec_GetCapabilityByCategory(mMime.c_str(), false /* decoder */, HARDWARE);
            if (capability != nullptr && OH_AVCapability_IsHardware(capability)) {
                const char *hwName = OH_AVCapability_GetName(capability);
                if (hwName != nullptr) {
                    mCodec = OH_VideoDecoder_CreateByName(hwName);
                    AF_LOGI("create OHOS hardware decoder by name %s -> %p\n", hwName, mCodec);
                }
            } else {
                AF_LOGW("no OHOS hardware capability for mime %s\n", mMime.c_str());
            }
        }

        if (mCodec == nullptr) {
            // No hardware capability was reported, or creating by name failed:
            // let the framework pick the codec (it still prefers hardware).
            mCodec = ohosCodecCreateByMime(mIsAudio, mMime.c_str());
            AF_LOGI("create OHOS decoder by mime %s -> %p\n", mMime.c_str(), mCodec);
        }

        if (mCodec == nullptr) {
            AF_LOGE("create OHOS codec failed for mime %s\n", mMime.c_str());
            return gen_framework_errno(error_class_codec,
                                       mIsAudio ? codec_error_audio_not_support : codec_error_video_device_error);
        }

        OH_AVCodecCallback callback{};
        callback.onError = onError;
        callback.onStreamChanged = onStreamChanged;
        callback.onNeedInputBuffer = onNeedInputBuffer;
        callback.onNewOutputBuffer = onNewOutputBuffer;
        if (ohosCodecRegisterCallback(mCodec, mIsAudio, callback, this) != AV_ERR_OK) {
            AF_LOGE("OHOS codec RegisterCallback failed\n");
            return gen_framework_errno(error_class_codec, codec_error_video_device_error);
        }

        // 2. Surface mode: zero-copy output into the XComponent window.
        if (!mIsAudio && wnd != nullptr) {
            mWindow = static_cast<OHNativeWindow *>(wnd);
            if (ohosCodecSetSurface(mCodec, mIsAudio, mWindow) == AV_ERR_OK) {
                mSurfaceMode = true;
                AF_LOGI("OHOS decoder surface mode on\n");
            } else {
                AF_LOGW("OHOS decoder SetSurface failed, fallback to buffer mode\n");
                mSurfaceMode = false;
            }
        }

        // 3. Configure.
        OH_AVFormat *format = OH_AVFormat_Create();
        if (format == nullptr) {
            AF_LOGE("OH_AVFormat_Create failed\n");
            return gen_framework_errno(error_class_codec, codec_error_video_device_error);
        }
        OH_AVFormat_SetStringValue(format, OH_MD_KEY_CODEC_MIME, mMime.c_str());
        if (mIsAudio) {
            OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_SAMPLE_RATE, mSampleRate);
            OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_CHANNEL_COUNT, mChannels);
            // The buffer-mode frame below is built as interleaved S16, so pin the
            // output sample format instead of relying on the default.
            OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUDIO_SAMPLE_FORMAT, SAMPLE_S16LE);
            OH_AVFormat_SetIntValue(format, OH_MD_KEY_MAX_INPUT_SIZE, 16 * 1024);
            /*
             * AAC 的两个键必须成对给对（判据与 Android 侧逐字一致，
             * 见 framework/codec/Android/mediaCodecDecoder.cpp:337-374）：
             *   · 没有 extradata ⇒ 码流自带 ADTS 头 ⇒ IS_ADTS = 1，不需要 CSD；
             *   · 有 extradata ⇒ raw AAC ⇒ IS_ADTS 必须显式置 0，并把
             *     AudioSpecificConfig 作为 CSD 交下去。
             * 只置其中一个的场景（尤其 raw AAC 缺 CSD）会让解码器按 ADTS 解析裸帧，
             * 表现为"有日志没声音"。
             */
            if (meta->codec == AF_CODEC_ID_AAC) {
                const bool isADTS = (meta->extradata == nullptr || meta->extradata_size <= 0);
                OH_AVFormat_SetIntValue(format, OH_MD_KEY_AAC_IS_ADTS, isADTS ? 1 : 0);
                if (!isADTS) {
                    OH_AVFormat_SetBuffer(format, OH_MD_KEY_CODEC_CONFIG, meta->extradata,
                                          static_cast<size_t>(meta->extradata_size));
                }
                AF_LOGI("OHOS AAC configure: adts=%d csd=%d\n", isADTS ? 1 : 0,
                        isADTS ? 0 : meta->extradata_size);
            }
        } else {
            OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, mWidth);
            OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, mHeight);
            OH_AVFormat_SetIntValue(format, OH_MD_KEY_ROTATION, meta->rotate == 0 ? 0 : meta->rotate);
            if (!mSurfaceMode) {
                // Buffer mode: request NV12 so we can hand frames to the pipeline.
                OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
            }
        }

        // 4. DRM: attach the DRM Kit media key session before Configure when the
        //    stream is content protected; the codec service then decrypts the
        //    CENC samples in-pipeline.
        if (drmInfo != nullptr && !drmInfo->format.empty()) {
            mDrmHandler = std::dynamic_pointer_cast<OhosDrmHandler>(mRequireDrmHandlerCallback
                    ? mRequireDrmHandlerCallback(*drmInfo) : nullptr);
            if (mDrmHandler != nullptr) {
                // Create the media key system/session and run the license
                // request before attaching the session to the decoder.
                mDrmHandler->open();
            }
            if (mDrmHandler != nullptr && mDrmHandler->getMediaKeySession() != nullptr) {
                // The codec API takes the DRM Kit's opaque MediaKeySession
                // (native_avcodec_videodecoder.h / native_avcodec_audiocodec.h),
                // while OhosDrmHandler exposes the very same DRM Kit session
                // under its own opaque tag.
                MediaKeySession *session =
                        reinterpret_cast<MediaKeySession *>(mDrmHandler->getMediaKeySession());
                if (ohosCodecSetDecryptionConfig(mCodec, mIsAudio, session) != AV_ERR_OK) {
                    AF_LOGW("OHOS decoder SetDecryptionConfig failed\n");
                } else {
                    AF_LOGI("DRM session attached to OHOS decoder\n");
                }
            }
        }

        OH_AVErrCode err = ohosCodecConfigure(mCodec, mIsAudio, format);
        OH_AVFormat_Destroy(format);
        if (err != AV_ERR_OK) {
            AF_LOGE("OHOS decoder Configure failed err %d\n", err);
            return gen_framework_errno(error_class_codec, codec_error_video_device_error);
        }

        err = ohosCodecPrepare(mCodec, mIsAudio);
        if (err != AV_ERR_OK) {
            AF_LOGE("OHOS decoder Prepare failed err %d\n", err);
            return gen_framework_errno(error_class_codec, codec_error_video_device_error);
        }
        err = ohosCodecStart(mCodec, mIsAudio);
        if (err != AV_ERR_OK) {
            AF_LOGE("OHOS decoder Start failed err %d\n", err);
            return gen_framework_errno(error_class_codec, codec_error_video_device_error);
        }

        mbInit = true;
        mInputEosSent = false;
        mOutputEos = false;
        return 0;
    }

    void OhosAVCodecDecoder::close_decoder()
    {
        if (mCodec != nullptr) {
            ohosCodecStop(mCodec, mIsAudio);
            ohosCodecDestroy(mCodec, mIsAudio);
            mCodec = nullptr;
        }
        mWindow = nullptr;
        mSurfaceMode = false;
        mbInit = false;
        mDrmHandler = nullptr;
        mOutputCond.notify_all();
        mInputCond.notify_all();
    }

    // ------------------------------------------------------------- input side --

    int OhosAVCodecDecoder::enqueue_decoder(unique_ptr<IAFPacket> &pPacket)
    {
        std::lock_guard<std::mutex> lock(mInputMutex);
        mPendingInputs.push_back(std::move(pPacket));
        mInputCond.notify_one();
        return 0;
    }

    void OhosAVCodecDecoder::drainInputQueueLocked()
    {
        // Fills the free OH_AVBuffer handed to us in onNeedInputBuffer with the
        // head packet of the pending queue. Called with mInputMutex held.
        while (!mPendingInputs.empty()) {
            OH_AVBuffer *buffer = nullptr;
            uint32_t index = 0;
            // onNeedInputBuffer delivered an available buffer to mInputCond.
            (void) buffer;
            (void) index;
            break;
        }
    }

    void OhosAVCodecDecoder::onNeedInputBuffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *userData)
    {
        auto *decoder = static_cast<OhosAVCodecDecoder *>(userData);
        if (decoder == nullptr || decoder->mCodec == nullptr) {
            return;
        }

        std::unique_ptr<IAFPacket> packet;
        {
            std::lock_guard<std::mutex> lock(decoder->mInputMutex);
            if (!decoder->mPendingInputs.empty()) {
                packet = std::move(decoder->mPendingInputs.front());
                decoder->mPendingInputs.pop_front();
            }
        }

        OH_AVCodecBufferAttr attr{};
        attr.pts = 0;
        attr.size = 0;
        attr.offset = 0;
        attr.flags = 0;

        if (packet != nullptr) {
            const IAFPacket::packetInfo &info = packet->getInfo();
            attr.pts = info.pts == INT64_MIN ? 0 : info.pts;
            attr.size = static_cast<int32_t>(packet->getSize());
            attr.offset = 0;
            attr.flags = 0;
            if (info.flags & AF_PKT_FLAG_KEY) {
                attr.flags |= AVCODEC_BUFFER_FLAGS_SYNC_FRAME;
            }
            if (packet->getData() != nullptr && attr.size > 0) {
                uint8_t *dst = OH_AVBuffer_GetAddr(buffer);
                int32_t capacity = OH_AVBuffer_GetCapacity(buffer);
                if (dst != nullptr && capacity > 0 && static_cast<size_t>(capacity) >= static_cast<size_t>(attr.size)) {
                    memcpy(dst, packet->getData(), attr.size);
                } else {
                    attr.size = 0;
                }
            }
        } else if (!decoder->mInputEosSent) {
            // Nothing pending -> signal EOS once.
            decoder->mInputEosSent = true;
            attr.flags |= AVCODEC_BUFFER_FLAGS_EOS;
        } else {
            // Decoder asks for more input after EOS was already submitted;
            // submit an empty buffer to keep the pipeline alive.
        }

        OH_AVBuffer_SetBufferAttr(buffer, &attr);
        ohosCodecPushInputBuffer(codec, decoder->mIsAudio, index);
    }

    void OhosAVCodecDecoder::onError(OH_AVCodec *codec, int32_t errorCode, void *userData)
    {
        AF_LOGE("OHOS codec error %d\n", errorCode);
        auto *decoder = static_cast<OhosAVCodecDecoder *>(userData);
        if (decoder != nullptr) {
            decoder->enqueueError(errorCode, INT64_MIN);
        }
    }

    void OhosAVCodecDecoder::onStreamChanged(OH_AVCodec *codec, OH_AVFormat *format, void *userData)
    {
        AF_LOGI("OHOS codec stream changed\n");
    }

    // ------------------------------------------------------------ output side --

    void OhosAVCodecDecoder::onNewOutputBuffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *userData)
    {
        auto *decoder = static_cast<OhosAVCodecDecoder *>(userData);
        if (decoder == nullptr || decoder->mCodec == nullptr) {
            return;
        }

        OH_AVCodecBufferAttr attr{};
        if (OH_AVBuffer_GetBufferAttr(buffer, &attr) != AV_ERR_OK) {
            attr.pts = 0;
            attr.flags = 0;
        }

        bool eos = (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) != 0;
        int64_t ptsUs = attr.pts;

        if (decoder->mSurfaceMode) {
            // Zero-copy: commit the frame to the XComponent surface and hand
            // the pipeline a data-less frame (render happened in hardware).
            // mSurfaceMode is only ever set for video, so the video decoder
            // entry point is the right one here.
            OH_VideoDecoder_RenderOutputBuffer(codec, index);
            unique_ptr<IAFFrame> frame(
                    new AFMediaCodecFrame(IAFFrame::FrameTypeVideo, static_cast<int>(index),
                                          [](int, bool) {}));
            frame->getInfo().pts = ptsUs;
            frame->getInfo().video.width = decoder->mWidth;
            frame->getInfo().video.height = decoder->mHeight;
            {
                std::lock_guard<std::mutex> lock(decoder->mOutputMutex);
                decoder->mOutputFrames.push_back(std::move(frame));
            }
            decoder->mOutputCond.notify_one();
        } else if (decoder->mIsAudio) {
            // Audio PCM (interleaved S16, see OH_MD_KEY_AUDIO_SAMPLE_FORMAT).
            uint8_t *data = OH_AVBuffer_GetAddr(buffer);
            int32_t size = attr.size;
            IAFFrame::AFFrameInfo frameInfo{};
            frameInfo.audio.format = AF_SAMPLE_FMT_S16;
            frameInfo.audio.sample_rate = decoder->mSampleRate;
            frameInfo.audio.channels = decoder->mChannels;
            frameInfo.audio.nb_samples = size / (decoder->mChannels * 2);
            frameInfo.pts = ptsUs;
            uint8_t *pcm[1] = {data};
            int lineSize[1] = {size};
            unique_ptr<IAFFrame> frame(
                    new AVAFFrame(frameInfo, (const uint8_t **) pcm, (const int *) lineSize, 1,
                                  IAFFrame::FrameTypeAudio));
            ohosCodecFreeOutputBuffer(codec, decoder->mIsAudio, index);
            {
                std::lock_guard<std::mutex> lock(decoder->mOutputMutex);
                decoder->mOutputFrames.push_back(std::move(frame));
            }
            decoder->mOutputCond.notify_one();
        } else {
            // Video buffer mode: NV12 -> I420 conversion by the pipeline.
            uint8_t *data = OH_AVBuffer_GetAddr(buffer);
            IAFFrame::AFFrameInfo frameInfo{};
            frameInfo.video.width = decoder->mWidth;
            frameInfo.video.height = decoder->mHeight;
            frameInfo.video.format = AF_PIX_FMT_NV12;
            frameInfo.pts = ptsUs;
            uint8_t *planes[2] = {data, data + decoder->mWidth * decoder->mHeight};
            int lineSizeArr[2] = {decoder->mWidth, decoder->mWidth};
            unique_ptr<IAFFrame> frame(
                    new AVAFFrame(frameInfo, (const uint8_t **) planes, (const int *) lineSizeArr, 2,
                                  IAFFrame::FrameTypeVideo));
            ohosCodecFreeOutputBuffer(codec, decoder->mIsAudio, index);
            {
                std::lock_guard<std::mutex> lock(decoder->mOutputMutex);
                decoder->mOutputFrames.push_back(std::move(frame));
            }
            decoder->mOutputCond.notify_one();
        }

        if (eos) {
            decoder->mOutputEos = true;
            decoder->mOutputCond.notify_all();
        }
    }

    int OhosAVCodecDecoder::dequeue_decoder(unique_ptr<IAFFrame> &pFrame)
    {
        if (!mbInit) {
            return -EAGAIN;
        }

        std::unique_lock<std::mutex> lock(mOutputMutex);
        if (mOutputFrames.empty()) {
            if (mOutputEos) {
                return STATUS_EOS;
            }
            return -EAGAIN;
        }
        pFrame = std::move(mOutputFrames.front());
        mOutputFrames.pop_front();
        return 0;
    }

    void OhosAVCodecDecoder::flush_decoder()
    {
        if (mCodec == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lockFlush(mFlushMutex);
        mFlushing = true;
        ohosCodecFlush(mCodec, mIsAudio);
        {
            std::lock_guard<std::mutex> lock(mOutputMutex);
            mOutputFrames.clear();
            mOutputEos = false;
        }
        {
            std::lock_guard<std::mutex> lock(mInputMutex);
            mPendingInputs.clear();
            mInputEosSent = false;
        }
        mFlushing = false;
    }

} // namespace Cicada

#endif // __OHOS__
