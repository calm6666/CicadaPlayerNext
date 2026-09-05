//
// OhosAVCodecDecoder.cpp
//
// OpenHarmony OH_AVCodec hardware decode integration (API 12+).
// See OhosAVCodecDecoder.h for the design notes.
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
#include <multimedia/player_framework/native_avcodec_audiocodec.h>
#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avbuffer_info.h>
#include <native_window/external_window.h>

#include <cstring>

using namespace std;

namespace Cicada {

    static const char *codecToMime(AFCodecID codecId)
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

    // ------------------------------------------------------------------ DRM --
    // Attach a DRM session when the stream is content-protected; the codec
    // service decrypts CENC samples in-pipeline.

    // ------------------------------------------------------------------ init --

    int OhosAVCodecDecoder::init_decoder(const Stream_meta *meta, void *wnd, uint64_t flags,
                                         const DrmInfo *drmInfo)
    {
        mIsAudio = (meta->type == STREAM_TYPE_AUDIO);
        mMime = codecToMime(meta->codec);
        if (mMime.empty()) {
            return gen_framework_errno(error_class_codec,
                                       mIsAudio ? codec_error_audio_not_support : codec_error_video_not_support);
        }
        mWidth = meta->width;
        mHeight = meta->height;
        mSampleRate = meta->samplerate;
        mChannels = meta->channels;

        // 1. Discover the vendor hardware decoder name for the MIME type.
        OH_AVCapability *capability = OH_AVCapability_Create();
        const char *hwName = nullptr;
        if (capability && !mIsAudio) {
            OH_AVErrCode err = OH_AVCapability_GetHardwareDecoderName(capability, HARDWARE, mMime.c_str(), &hwName);
            if (err != AV_ERR_OK) {
                hwName = nullptr;
                AF_LOGW("OH_AVCapability_GetHardwareDecoderName %s failed err %d\n", mMime.c_str(), err);
            }
        }
        if (capability) {
            OH_AVCapability_Destroy(capability);
        }

        if (hwName != nullptr) {
            mCodec = OH_AVCodec_CreateByName(hwName);
            AF_LOGI("create OHOS hardware decoder by name %s -> %p\n", hwName, mCodec);
        } else {
            mCodec = OH_AVCodec_CreateByMime(mMime.c_str());
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
        if (OH_AVCodec_SetCallback(mCodec, callback, this) != AV_ERR_OK) {
            AF_LOGE("OH_AVCodec_SetCallback failed\n");
            return gen_framework_errno(error_class_codec, codec_error_video_device_error);
        }

        // 2. Surface mode: zero-copy output into the XComponent window.
        if (!mIsAudio && wnd != nullptr) {
            mWindow = static_cast<OHNativeWindow *>(wnd);
            if (OH_AVCodec_SetSurface(mCodec, mWindow) == AV_ERR_OK) {
                mSurfaceMode = true;
                AF_LOGI("OHOS decoder surface mode on\n");
            } else {
                AF_LOGW("OH_AVCodec_SetSurface failed, fallback to buffer mode\n");
                mSurfaceMode = false;
            }
        }

        // 3. Configure.
        OH_AVFormat *format = OH_AVFormat_Create();
        OH_AVFormat_SetStringValue(format, OH_MD_KEY_CODEC_MIME, mMime.c_str());
        if (mIsAudio) {
            OH_AVFormat_SetIntValue(format, OH_MD_KEY_SAMPLE_RATE, mSampleRate);
            OH_AVFormat_SetIntValue(format, OH_MD_KEY_CHANNEL_COUNT, mChannels);
            OH_AVFormat_SetIntValue(format, OH_MD_KEY_MAX_INPUT_SIZE, 16 * 1024);
        } else {
            OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, mWidth);
            OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, mHeight);
            OH_AVFormat_SetIntValue(format, OH_MD_KEY_ROTATION, meta->rotate == 0 ? 0 : meta->rotate);
            if (!mSurfaceMode) {
                // Buffer mode: request NV12 so we can hand frames to the pipeline.
                OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
            }
        }

        // 4. DRM: attach an OH_MediaKeySession before Configure when protected.
        if (drmInfo != nullptr && !drmInfo->format.empty()) {
            mDrmHandler = std::dynamic_pointer_cast<OhosDrmHandler>(mRequireDrmHandlerCallback
                    ? mRequireDrmHandlerCallback(*drmInfo) : nullptr);
            if (mDrmHandler != nullptr) {
                // Create the media key system/session and run the license
                // request before attaching the session to the decoder.
                mDrmHandler->open();
            }
            if (mDrmHandler && mDrmHandler->getMediaKeySession() != nullptr) {
                if (OH_AVCodec_SetMediakeySessionConfig(mCodec, mDrmHandler->getMediaKeySession()) != AV_ERR_OK) {
                    AF_LOGW("OH_AVCodec_SetMediakeySessionConfig failed\n");
                } else {
                    AF_LOGI("DRM session attached to OHOS decoder\n");
                }
            }
        }

        OH_AVErrCode err = OH_AVCodec_Configure(mCodec, format);
        OH_AVFormat_Destroy(format);
        if (err != AV_ERR_OK) {
            AF_LOGE("OH_AVCodec_Configure failed err %d\n", err);
            return gen_framework_errno(error_class_codec, codec_error_video_device_error);
        }

        err = OH_AVCodec_Prepare(mCodec);
        if (err != AV_ERR_OK) {
            AF_LOGE("OH_AVCodec_Prepare failed err %d\n", err);
            return gen_framework_errno(error_class_codec, codec_error_video_device_error);
        }
        err = OH_AVCodec_Start(mCodec);
        if (err != AV_ERR_OK) {
            AF_LOGE("OH_AVCodec_Start failed err %d\n", err);
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
            OH_AVCodec_Stop(mCodec);
            OH_AVCodec_Destroy(mCodec);
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
                attr.flags |= OH_AVCODEC_BUFFER_FLAGS_SYNC_FRAME;
            }
            if (packet->getData() != nullptr && attr.size > 0) {
                uint8_t *dst = OH_AVBuffer_GetAddr(buffer);
                size_t capacity = OH_AVBuffer_GetCapacity(buffer);
                if (dst != nullptr && capacity >= static_cast<size_t>(attr.size)) {
                    memcpy(dst, packet->getData(), attr.size);
                } else {
                    attr.size = 0;
                }
            }
        } else if (!decoder->mInputEosSent) {
            // Nothing pending -> signal EOS once.
            decoder->mInputEosSent = true;
            attr.flags |= OH_AVCODEC_BUFFER_FLAGS_EOS;
        } else {
            // Decoder asks for more input after EOS was already submitted;
            // submit an empty buffer to keep the pipeline alive.
        }

        OH_AVBuffer_SetBufferAttr(buffer, &attr);
        OH_AVCodec_PushInputBuffer(codec, index);
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

        bool eos = (attr.flags & OH_AVCODEC_BUFFER_FLAGS_EOS) != 0;
        int64_t ptsUs = attr.pts;

        if (decoder->mSurfaceMode) {
            // Zero-copy: commit the frame to the XComponent surface and hand
            // the pipeline a data-less frame (render happened in hardware).
            OH_AVCodec_RenderOutputBuffer(codec, index);
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
            // Audio PCM (interleaved S16 by default).
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
            OH_AVCodec_FreeOutputBuffer(codec, index);
            {
                std::lock_guard<std::mutex> lock(decoder->mOutputMutex);
                decoder->mOutputFrames.push_back(std::move(frame));
            }
            decoder->mOutputCond.notify_one();
        } else {
            // Video buffer mode: NV12 -> I420 conversion by the pipeline.
            uint8_t *data = OH_AVBuffer_GetAddr(buffer);
            int32_t size = attr.size;
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
            OH_AVCodec_FreeOutputBuffer(codec, index);
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
        OH_AVCodec_Flush(mCodec);
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
