//
// Created by moqi on 2018/8/10.
//
#define LOG_TAG "avcodecDecoder"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
};

#if defined(_WIN32)
/*
 * These must NOT go inside the extern "C" block above.
 * hwcontext_d3d11va.h pulls in the Windows SDK <d3d11.h>, which defines C++
 * comparison operators for D3D11_VIEWPORT / D3D11_RECT / D3D11_BOX. Inside an
 * extern "C" block those operators get C linkage and MSVC rejects every one of
 * them with C2733. FFmpeg's own headers carry their own extern "C" guards, so
 * nothing is lost by including them outside.
 */
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#endif

#include <utils/AFUtils.h>
#include <cstring>
#include <cstdlib>
#include <utils/frame_work_log.h>
#include <utils/mediaFrame.h>
#include <utils/ffmpeg_utils.h>
#include <cassert>
#include <deque>
#include "avcodecDecoder.h"
#include "base/media/AVAFPacket.h"
#include <utils/errors/framework_error.h>

#define  MAX_INPUT_SIZE 4

using namespace std;

namespace Cicada {
    avcodecDecoder avcodecDecoder::se(0);

#if defined(_WIN32)
    /*
     * Pixel format negotiation callback. FFmpeg hands us the list of formats the
     * codec can output; we accept the D3D11VA one and refuse everything else,
     * which makes avcodec_open2() fail cleanly (and the caller fall back to the
     * software decoder) rather than silently producing frames we cannot use.
     */
    enum AVPixelFormat avcodecDecoder::getD3D11vaFormat(AVCodecContext *ctx,
                                                        const enum AVPixelFormat *pixFmts)
    {
        auto *dec = static_cast<decoder_handle_v *>(ctx->opaque);

        for (const enum AVPixelFormat *p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
            if (*p == dec->hwPixFmt) {
                return *p;
            }
        }

        AF_LOGE("D3D11VA: decoder did not offer %s, refusing\n",
                av_get_pix_fmt_name(dec->hwPixFmt));
        return AV_PIX_FMT_NONE;
    }

    /*
     * Finds the D3D11VA hardware configuration for this codec.
     *
     * Only AV_PIX_FMT_D3D11 is accepted: that is the modern API, where FFmpeg
     * owns an AVHWFramesContext and av_hwframe_transfer_data() can download a
     * decoded surface. The legacy AV_PIX_FMT_D3D11VA_VLD format (produced by the
     * h264_d3d11va hwaccel, as opposed to h264_d3d11va2) carries a bare
     * ID3D11VideoDecoderOutputView pointer in frame->data[3] and no frames
     * context at all, so copy-back is impossible there - it is refused with a
     * warning rather than producing frames the renderer cannot use.
     */
    static bool hasD3D11vaHwConfig(const AVCodec *codec, enum AVPixelFormat *pixFmt)
    {
        bool onlyLegacyApi = false;

        for (int i = 0;; i++) {
            const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);

            if (config == nullptr) {
                break;
            }

            if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) == 0 ||
                    config->device_type != AV_HWDEVICE_TYPE_D3D11VA) {
                continue;
            }

            if (config->pix_fmt == AV_PIX_FMT_D3D11) {
                *pixFmt = config->pix_fmt;
                return true;
            }

            if (config->pix_fmt == AV_PIX_FMT_D3D11VA_VLD) {
                onlyLegacyApi = true;
            }
        }

        if (onlyLegacyApi) {
            AF_LOGW("D3D11VA: %s only offers the legacy AV_PIX_FMT_D3D11VA_VLD hwaccel, "
                    "which has no frames context to download from; copy-back needs "
                    "AV_PIX_FMT_D3D11 (enable the *_d3d11va2 hwaccels in "
                    "external/player_ffmpeg_config.sh)\n", codec->name);
        }

        return false;
    }
#endif

    void avcodecDecoder::close_decoder()
    {
        if (mPDecoder == nullptr) {
            return;
        }

#if defined(_WIN32)
        mPDecoder->hwDecodeActive = false;

        if (mPDecoder->swsCtx != nullptr) {
            sws_freeContext(static_cast<SwsContext *>(mPDecoder->swsCtx));
            mPDecoder->swsCtx = nullptr;
        }

        if (mPDecoder->convFrame != nullptr) {
            av_frame_free(&mPDecoder->convFrame);
        }

        if (mPDecoder->swFrame != nullptr) {
            av_frame_free(&mPDecoder->swFrame);
        }

        // Must go before avcodec_free_context(): the codec context holds its own
        // reference and releases it on free.
        if (mPDecoder->hwDeviceRef != nullptr) {
            av_buffer_unref(&mPDecoder->hwDeviceRef);
        }
#endif

        if (mPDecoder->codecCont != nullptr) {
            // avcodec_close was removed in FFmpeg 6.0; avcodec_free_context()
            // closes and frees the context in one call.
            avcodec_free_context(&mPDecoder->codecCont);
            mPDecoder->codecCont = nullptr;
        }

        mPDecoder->codec = nullptr;
        av_frame_free(&mPDecoder->avFrame);
        delete mPDecoder;
        mPDecoder = nullptr;
    }

    int avcodecDecoder::init_decoder(const Stream_meta *meta, void *wnd, uint64_t flags,
                                     const DrmInfo *drmInfo)
    {
        auto codecId = (enum AVCodecID) CodecID2AVCodecID(meta->codec);
        mPDecoder->codec = avcodec_find_decoder(codecId);
        bool isAudio = meta->channels > 0;

        if (mPDecoder->codec == nullptr) {
            return gen_framework_errno(error_class_codec, isAudio ? codec_error_audio_not_support : codec_error_video_not_support);
        }

        mPDecoder->codecCont = avcodec_alloc_context3((const AVCodec *) mPDecoder->codec);

        if (mPDecoder->codecCont == nullptr) {
            AF_LOGE("init_decoder error");
            return gen_framework_errno(error_class_codec, isAudio ? codec_error_audio_not_support : codec_error_video_not_support);
        }

        if (isAudio) {
            mPDecoder->codecCont->ch_layout.nb_channels = meta->channels;
            mPDecoder->codecCont->sample_rate = meta->samplerate;
        }

        // TODO: not set extradata when XXvc

        if (meta->extradata != nullptr && meta->extradata_size > 0) {
            mPDecoder->codecCont->extradata = (uint8_t *) av_mallocz(meta->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            memcpy(mPDecoder->codecCont->extradata, meta->extradata, meta->extradata_size);
            mPDecoder->codecCont->extradata_size = meta->extradata_size;
        }

        mPDecoder->flags = DECFLAG_SW;
        // "refcounted_frames" was removed in FFmpeg 5.0: frames are always
        // refcounted in the modern send/receive API.
        int threadcount = (AFGetCpuCount() > 0 ? AFGetCpuCount() + 1 : 0);

        if ((flags & DECFLAG_OUTPUT_FRAME_ASAP)
                && ((0 == threadcount) || (threadcount > 2))) {
            // set too much thread need more video buffer in ffmpeg
            threadcount = 2;
        }

        AF_LOGI("set decoder thread as :%d\n", threadcount);
        mPDecoder->codecCont->thread_count = threadcount;

#if defined(_WIN32)

        // D3D11VA is set up here, still before avcodec_open2() so the codec
        // context is opened straight onto the GPU. It has to come after the
        // software thread pool configuration above because avcodecOpen2()
        // finalises both, and hardware decoding overrides the thread count
        // (there is nothing left for the CPU threads to do).
        //
        // Only video, and only when the caller asked for hardware decoding. On
        // any failure the decoder is left exactly as a software decoder.
        if (!isAudio && (flags & DECFLAG_HW) && initD3D11va(meta)) {
            mPDecoder->flags = DECFLAG_HW;
            mPDecoder->codecCont->thread_count = 1;
        }

#endif

        if (avcodec_open2(mPDecoder->codecCont, mPDecoder->codec, nullptr) < 0) {
            AF_LOGE("could not open codec\n");
            avcodec_free_context(&mPDecoder->codecCont);
            return -1;
        }

#if defined(_WIN32)

        // Now that avcodec_open2() ran, FFmpeg has created the D3D11 surface pool
        // and we can learn which software layout the surfaces download to. NV12
        // is the usual answer for 8-bit 4:2:0; that is fine for the copy-back
        // itself, but the SDL renderer uploads Y/U/V planes and therefore needs
        // YUV420P - so this is the value to watch when debugging a green or
        // empty picture with hardware decoding on.
        if (mPDecoder->hwDecodeActive) {
            auto *framesCtx = reinterpret_cast<AVHWFramesContext *>(
                    mPDecoder->codecCont->hw_frames_ctx ? mPDecoder->codecCont->hw_frames_ctx->data : nullptr);

            if (framesCtx != nullptr) {
                mPDecoder->swPixFmt = framesCtx->sw_format;
                AF_LOGI("D3D11VA: surfaces %dx%d, download format %s\n",
                        framesCtx->width, framesCtx->height,
                        av_get_pix_fmt_name(framesCtx->sw_format));

                if (framesCtx->sw_format != AV_PIX_FMT_YUV420P) {
                    AF_LOGI("D3D11VA: converting the download from %s to YUV420P for the renderer\n",
                            av_get_pix_fmt_name(framesCtx->sw_format));
                }
            } else {
                AF_LOGW("D3D11VA: no frames context after open, falling back to software\n");
                mPDecoder->hwDecodeActive = false;
                mPDecoder->flags = DECFLAG_SW;
            }
        }

#endif

        mPDecoder->avFrame = av_frame_alloc();
        mPDecoder->vInfo.height = mPDecoder->codecCont->height;
        mPDecoder->vInfo.width = mPDecoder->codecCont->width;
        mPDecoder->vInfo.pix_fmt = mPDecoder->codecCont->pix_fmt;
        return 0;
    }

#if defined(_WIN32)
    bool avcodecDecoder::initD3D11va(const Stream_meta *meta)
    {
        (void) meta;
        enum AVPixelFormat hwPixFmt = AV_PIX_FMT_NONE;

        // 1. Does the linked FFmpeg even have a D3D11VA decoder for this codec?
        //    With the stock prebuilt win32 libffmpeg.dll (--disable-hwaccels)
        //    this is false, and we leave the decoder entirely untouched.
        if (!hasD3D11vaHwConfig(mPDecoder->codec, &hwPixFmt)) {
            AF_LOGI("D3D11VA: %s has no d3d11va hwaccel in this FFmpeg build, "
                    "using the software decoder\n", mPDecoder->codec->name);
            return false;
        }

        // 2. Create the D3D11 device (and its immediate context) FFmpeg will
        //    decode on. A nullptr device string selects the default adapter.
        AVBufferRef *deviceRef = nullptr;
        int ret = av_hwdevice_ctx_create(&deviceRef, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);

        if (ret < 0) {
            AF_LOGW("D3D11VA: av_hwdevice_ctx_create failed: %s, using the software decoder\n",
                    getErrorString(ret));
            return false;
        }

        mPDecoder->hwDeviceRef = deviceRef;
        mPDecoder->hwPixFmt = hwPixFmt;
        mPDecoder->hwDecodeActive = true;

        // 3. Hand the device to the codec context and install the format
        //    negotiation callback. FFmpeg allocates the D3D11 surface pool for
        //    us once it knows coded_width/coded_height from the bitstream.
        mPDecoder->codecCont->hw_device_ctx = av_buffer_ref(deviceRef);
        mPDecoder->codecCont->opaque = mPDecoder;
        mPDecoder->codecCont->get_format = getD3D11vaFormat;

        AF_LOGI("D3D11VA: hardware decoding enabled for %s (hw pixel format %s)\n",
                mPDecoder->codec->name, av_get_pix_fmt_name(hwPixFmt));
        return true;
    }

    /*
     * Downloads a decoded D3D11 surface into system memory and, when needed,
     * converts it to the YUV420P layout the renderer uploads.
     *
     * The SDL renderer feeds SDL_UpdateYUVTexture() with three separate Y/U/V
     * planes, so it only accepts YUV420P. D3D11VA surfaces, however, download as
     * NV12 - hwcontext_d3d11va.c only ever reports ctx->sw_format from
     * d3d11va_transfer_get_formats() and explicitly refuses the opaque 420
     * layout that would map to YUV420P. So the chroma plane is re-split here
     * rather than adding NV12 handling to the renderer, which is shared with the
     * Linux and macOS builds.
     *
     * Decoding itself (entropy decode + motion compensation) still runs on the
     * GPU; this is only the download plus a cheap chroma copy.
     *
     * Returns the frame to hand to the caller, or hwFrame unchanged when the
     * download was not possible.
     */
    AVFrame *avcodecDecoder::retrieveHwFrame(AVFrame *hwFrame)
    {
        if (mPDecoder->swFrame == nullptr) {
            mPDecoder->swFrame = av_frame_alloc();

            if (mPDecoder->swFrame == nullptr) {
                AF_LOGE("D3D11VA: cannot allocate the copy-back frame\n");
                return hwFrame;
            }
        }

        AVFrame *swFrame = mPDecoder->swFrame;
        av_frame_unref(swFrame);

        // Ask for the layout the surface pool actually provides; requesting
        // anything else makes av_hwframe_transfer_data() fail.
        if (mPDecoder->swPixFmt != AV_PIX_FMT_NONE) {
            swFrame->format = mPDecoder->swPixFmt;
        }

        int ret = av_hwframe_transfer_data(swFrame, hwFrame, 0);

        if (ret < 0) {
            AF_LOGE("D3D11VA: av_hwframe_transfer_data failed: %s\n", getErrorString(ret));
            return hwFrame;
        }

        // Keep pts / duration / metadata - av_hwframe_transfer_data only copies
        // the pixel data.
        av_frame_copy_props(swFrame, hwFrame);

        if (swFrame->format == AV_PIX_FMT_YUV420P) {
            return swFrame;
        }

        // Convert to YUV420P for the renderer.
        if (mPDecoder->convFrame == nullptr) {
            mPDecoder->convFrame = av_frame_alloc();

            if (mPDecoder->convFrame == nullptr) {
                AF_LOGE("D3D11VA: cannot allocate the conversion frame\n");
                return swFrame;
            }
        }

        AVFrame *out = mPDecoder->convFrame;
        av_frame_unref(out);
        out->format = AV_PIX_FMT_YUV420P;
        out->width = swFrame->width;
        out->height = swFrame->height;

        if (av_frame_get_buffer(out, 0) < 0) {
            AF_LOGE("D3D11VA: cannot allocate the YUV420P conversion buffer\n");
            return swFrame;
        }

        mPDecoder->swsCtx = sws_getCachedContext(static_cast<SwsContext *>(mPDecoder->swsCtx),
                                                 swFrame->width, swFrame->height,
                                                 (enum AVPixelFormat) swFrame->format,
                                                 swFrame->width, swFrame->height,
                                                 AV_PIX_FMT_YUV420P,
                                                 SWS_POINT, nullptr, nullptr, nullptr);

        if (mPDecoder->swsCtx == nullptr) {
            AF_LOGE("D3D11VA: cannot create the swscale context\n");
            return swFrame;
        }

        sws_scale(static_cast<SwsContext *>(mPDecoder->swsCtx), swFrame->data, swFrame->linesize,
                  0, swFrame->height, out->data, out->linesize);
        av_frame_copy_props(out, swFrame);
        return out;
    }
#endif

    avcodecDecoder::avcodecDecoder() : ActiveDecoder()
    {
        mName = "VD.avcodec";
        mPDecoder = new decoder_handle_v();
        memset(mPDecoder, 0, sizeof(decoder_handle_v));
#if defined(_WIN32)
        mPDecoder->hwPixFmt = AV_PIX_FMT_NONE;
        mPDecoder->swPixFmt = AV_PIX_FMT_NONE;
#endif
        // avcodec_register_all was removed in FFmpeg 5.0 (decoders self-register).
        mFlags |= DECFLAG_PASSTHROUGH_INFO;
    }

    avcodecDecoder::~avcodecDecoder()
    {
        close();
    }

    bool avcodecDecoder::is_supported(enum AFCodecID codec)
    {
//        return codec == AF_CODEC_ID_H264
//               || codec == AF_CODEC_ID_MPEG4
//               || codec == AF_CODEC_ID_HEVC
//               || codec == AF_CODEC_ID_AAC
//               || codec == AF_CODEC_ID_MP1
//               || codec == AF_CODEC_ID_MP2
//               || codec == AF_CODEC_ID_MP3
//               || codec == AF_CODEC_ID_PCM_S16LE;
        if (avcodec_find_decoder(CodecID2AVCodecID(codec))) {
            return true;
        }

        return false;
    }

    void avcodecDecoder::flush_decoder()
    {
#if defined(_WIN32)

        // Drop the in-flight copy-back buffers so a seek cannot hand the
        // renderer a frame from before the flush.
        if (mPDecoder->swFrame != nullptr) {
            av_frame_unref(mPDecoder->swFrame);
        }

        if (mPDecoder->convFrame != nullptr) {
            av_frame_unref(mPDecoder->convFrame);
        }

#endif
        avcodec_flush_buffers(mPDecoder->codecCont);
    }

    int avcodecDecoder::dequeue_decoder(unique_ptr<IAFFrame> &pFrame)
    {
        int ret = avcodec_receive_frame(mPDecoder->codecCont, mPDecoder->avFrame);

        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                return STATUS_EOS;
            }

            return ret;
        }

        // FFmpeg 5.0+ 起解码器会给正常关键帧打上 AV_FRAME_FLAG_KEY 等标志，
        // 旧代码把 flags != 0 整体当成坏帧丢掉，导致 FFmpeg 9 下所有关键帧
        // 都被丢弃、画面永远出不来（日志刷 get a error frame）。
        // 只丢弃真正标记损坏的帧（AV_FRAME_FLAG_CORRUPT）。
        if (mPDecoder->avFrame->decode_error_flags ||
            (mPDecoder->avFrame->flags & AV_FRAME_FLAG_CORRUPT)) {
            AF_LOGW("get a error frame\n");
            return -EAGAIN;
        }

        AVFrame *outFrame = mPDecoder->avFrame;
#if defined(_WIN32)

        // Hardware frames must be downloaded to system memory before the
        // renderer can upload them into an SDL texture.
        if (mPDecoder->hwDecodeActive && mPDecoder->avFrame->format == mPDecoder->hwPixFmt) {
            outFrame = retrieveHwFrame(mPDecoder->avFrame);
        }

#endif
        int64_t timePosition = INT64_MIN;
        int64_t utcTime = INT64_MIN;
        if (outFrame->metadata){
            AVDictionaryEntry *t = av_dict_get(outFrame->metadata,"timePosition", nullptr,AV_DICT_IGNORE_SUFFIX);
            if (t){
                timePosition = atoll(t->value);
            }
            AVDictionaryEntry *utcEntry = av_dict_get(outFrame->metadata, "utcTime", nullptr, AV_DICT_IGNORE_SUFFIX);
            if (utcEntry) {
                utcTime = atoll(utcEntry->value);
            }
        }
        pFrame = unique_ptr<IAFFrame>(new AVAFFrame(outFrame));
        pFrame->getInfo().timePosition = timePosition;
        pFrame->getInfo().utcTime = utcTime;
        return ret;
    };

    int avcodecDecoder::enqueue_decoder(unique_ptr<IAFPacket> &pPacket)
    {
        int ret;
        AVPacket *pkt = nullptr;

        if (pPacket) {
            auto *avAFPacket = dynamic_cast<AVAFPacket *>(pPacket.get());
            assert(avAFPacket);

            if (avAFPacket == nullptr) {
                // TODO: tobe impl
            } else {
                pkt = avAFPacket->ToAVPacket();
                pkt->pts = pPacket->getInfo().pts;
                pkt->dts = pPacket->getInfo().dts;
                assert(pkt != nullptr);
            }
        }

        if (pkt == nullptr) {
            AF_LOGD("send null to decoder\n");
        }

        if (pkt){
            AVDictionary *dict = nullptr;
            // FFmpeg 9: av_packet_pack_dictionary / *_side_data 的尺寸参数为 size_t*
            size_t size = 0;
            av_dict_set_int(&dict,"timePosition",pPacket->getInfo().timePosition,0);
            av_dict_set_int(&dict,"utcTime",pPacket->getInfo().utcTime,0);
            uint8_t *metadata = av_packet_pack_dictionary(dict, &size);
            av_dict_free(&dict);

            if (pPacket->getInfo().extra_data_size > 0) {
                size_t new_extradata_size = 0;
                const uint8_t *new_extradata = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, &new_extradata_size);
                if (new_extradata == nullptr) {
                    uint8_t *side = av_packet_new_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, pPacket->getInfo().extra_data_size);
                    if (side) {
                        memcpy(side, pPacket->getInfo().extra_data, pPacket->getInfo().extra_data_size);
                    }
                }
            }
            int addRet = av_packet_add_side_data(pkt, AV_PKT_DATA_STRINGS_METADATA, metadata, size);
            assert(metadata);
            assert(addRet >= 0);
        }

        ret = avcodec_send_packet(mPDecoder->codecCont, pkt);

        if (0 == ret) {
            pPacket = nullptr;
        } else if (ret == AVERROR(EAGAIN)) {
        } else if (ret == AVERROR_EOF) {
            AF_LOGD("Decode EOF\n");
            ret = 0;
        } else {
            AF_LOGE("Error while decoding frame %d :%s\n", ret,  getErrorString(ret));
        }

        return ret;
    }
    bool avcodecDecoder::supportReuse()
    {
        if (mPDecoder->codecCont == nullptr) {
            return true;
        }
        // TODO: check the data format whether changed (avcc adts ...)
        //return mPDecoder->codecCont->extradata_size == 0;
        return false;
    }
}
