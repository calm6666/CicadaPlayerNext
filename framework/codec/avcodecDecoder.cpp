//
// Created by moqi on 2018/8/10.
//
#define LOG_TAG "avcodecDecoder"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#if defined(CICADA_HW_DEVICE_TYPE)
/*
 * These belong INSIDE extern "C": FFmpeg's headers carry no extern "C" guard of
 * their own -- libavutil/pixdesc.h and libswscale/swscale.h contain no
 * __cplusplus/extern "C" block at all -- so including them outside gives every
 * symbol C++ linkage and the link fails on mangled names that the C import
 * library does not export:
 *   videodec.lib(avcodecDecoder.obj) : error LNK2019: unresolved external symbol
 *   "char const * __cdecl av_get_pix_fmt_name(enum AVPixelFormat)"
 *   "void __cdecl sws_freeContext(struct SwsContext *)"
 *   "int __cdecl sws_scale(...)"
 *   "struct SwsContext * __cdecl sws_getCachedContext(...)"
 *   fatal error LNK1120: 4 unresolved externals
 *
 * libavutil/hwcontext.h is safe in here: it only pulls in buffer.h, frame.h,
 * log.h and pixfmt.h, never a Windows SDK header. It is where every hardware
 * decoding type and call this file needs comes from - AV_HWDEVICE_TYPE_D3D11VA
 * or AV_HWDEVICE_TYPE_VAAPI, av_hwdevice_ctx_create(),
 * av_hwframe_transfer_data() and AVHWFramesContext.
 *
 * libavutil/hwcontext_d3d11va.h is deliberately NOT included. It is the single
 * header that does #include <d3d11.h>, and the C++ comparison operators that
 * d3d11.h defines for D3D11_VIEWPORT / D3D11_RECT / D3D11_BOX would get C
 * linkage inside an extern "C" block, which makes MSVC reject every one of them
 * with C2733. It only declares AVD3D11VADeviceContext, which this file never
 * uses, so it is simply not needed.
 */
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#endif
};

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

#if defined(CICADA_HW_DEVICE_TYPE)
    /*
     * Pixel format negotiation callback. FFmpeg hands us the list of formats the
     * codec can output; we accept the D3D11VA one and refuse everything else,
     * which makes avcodec_open2() fail cleanly (and the caller fall back to the
     * software decoder) rather than silently producing frames we cannot use.
     */
    enum AVPixelFormat avcodecDecoder::getHwFormat(AVCodecContext *ctx,
                                                        const enum AVPixelFormat *pixFmts)
    {
        auto *dec = static_cast<decoder_handle_v *>(ctx->opaque);

        for (const enum AVPixelFormat *p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
            if (*p == dec->hwPixFmt) {
                return *p;
            }
        }

        AF_LOGE("%s: decoder did not offer %s, refusing\n",
                CICADA_HW_NAME, av_get_pix_fmt_name(dec->hwPixFmt));
        return AV_PIX_FMT_NONE;
    }

    /*
     * Finds the hardware configuration for this codec on this platform.
     *
     * The match is against CICADA_HW_DEVICE_TYPE / CICADA_HW_PIX_FMT, so the
     * same code serves D3D11VA on Windows and VAAPI on Linux.
     *
     * Windows needs one extra rule. FFmpeg ships two D3D11VA hwaccels per codec
     * and only the "2" one is usable here: h264_d3d11va2 reports
     * AV_PIX_FMT_D3D11, where FFmpeg owns an AVHWFramesContext and
     * av_hwframe_transfer_data() can download a surface, while h264_d3d11va
     * reports the legacy AV_PIX_FMT_D3D11VA_VLD, which carries a bare
     * ID3D11VideoDecoderOutputView in frame->data[3] and no frames context at
     * all. That one is refused with a warning rather than producing frames the
     * renderer cannot use. VAAPI has no such split.
     */
    static bool hasHwConfig(const AVCodec *codec, enum AVPixelFormat *pixFmt)
    {
        bool onlyLegacyApi = false;

        for (int i = 0;; i++) {
            const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);

            if (config == nullptr) {
                break;
            }

            if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) == 0 ||
                    config->device_type != CICADA_HW_DEVICE_TYPE) {
                continue;
            }

            if (config->pix_fmt == CICADA_HW_PIX_FMT) {
                *pixFmt = config->pix_fmt;
                return true;
            }

#if defined(_WIN32)
            if (config->pix_fmt == AV_PIX_FMT_D3D11VA_VLD) {
                onlyLegacyApi = true;
            }
#endif
        }

        if (onlyLegacyApi) {
            AF_LOGW("%s: %s only offers the legacy AV_PIX_FMT_D3D11VA_VLD hwaccel, "
                    "which has no frames context to download from; copy-back needs "
                    "AV_PIX_FMT_D3D11 (enable the *_d3d11va2 hwaccels in "
                    "external/player_ffmpeg_config.sh)\n", CICADA_HW_NAME, codec->name);
        }

        return false;
    }
#endif

    void avcodecDecoder::close_decoder()
    {
        if (mPDecoder == nullptr) {
            return;
        }

#if defined(CICADA_HW_DEVICE_TYPE)
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

#if defined(CICADA_HW_DEVICE_TYPE)

        // D3D11VA is set up here, still before avcodec_open2() so the codec
        // context is opened straight onto the GPU. It has to come after the
        // software thread pool configuration above because avcodecOpen2()
        // finalises both, and hardware decoding overrides the thread count
        // (there is nothing left for the CPU threads to do).
        //
        // Only video, and only when the caller asked for hardware decoding. On
        // any failure the decoder is left exactly as a software decoder.
        if (!isAudio && (flags & DECFLAG_HW) && initHwDecoder(meta)) {
            mPDecoder->flags = DECFLAG_HW;
            mPDecoder->codecCont->thread_count = 1;
        }

#endif

        if (avcodec_open2(mPDecoder->codecCont, mPDecoder->codec, nullptr) < 0) {
            AF_LOGE("could not open codec\n");
            avcodec_free_context(&mPDecoder->codecCont);
            return -1;
        }

#if defined(CICADA_HW_DEVICE_TYPE)

        /*
         * Do NOT look for codecCont->hw_frames_ctx here.
         *
         * For H.264, HEVC and most other codecs FFmpeg only calls the get_format
         * callback - and therefore only allocates the D3D11 surface pool - when
         * the first frame is parsed, not during avcodec_open2(). Checking at this
         * point finds a null hw_frames_ctx for a perfectly healthy hardware
         * decoder, and acting on that is actively harmful: clearing
         * hwDecodeActive without also removing get_format/hw_device_ctx leaves
         * FFmpeg decoding into D3D11 surfaces while the download step is skipped,
         * so the renderer receives raw GPU frames whose data[0] is an
         * ID3D11Texture2D pointer and whose linesize[0] is a surface index. SDL
         * then reports "Parameter 'Ypitch' is invalid" for every frame and paints
         * a green window.
         *
         * The surface pool is inspected lazily instead, in retrieveHwFrame(),
         * from the decoded frame's own hw_frames_ctx.
         */
        if (mPDecoder->hwDecodeActive) {
            AF_LOGI("%s: codec opened; the surface pool is allocated on the first frame\n",
                    CICADA_HW_NAME);
        }

#endif

        mPDecoder->avFrame = av_frame_alloc();
        mPDecoder->vInfo.height = mPDecoder->codecCont->height;
        mPDecoder->vInfo.width = mPDecoder->codecCont->width;
        mPDecoder->vInfo.pix_fmt = mPDecoder->codecCont->pix_fmt;
        return 0;
    }

#if defined(CICADA_HW_DEVICE_TYPE)
    bool avcodecDecoder::initHwDecoder(const Stream_meta *meta)
    {
        (void) meta;
        enum AVPixelFormat hwPixFmt = AV_PIX_FMT_NONE;

        // 1. Does the linked FFmpeg even have a D3D11VA decoder for this codec?
        //    With the stock prebuilt win32 libffmpeg.dll (--disable-hwaccels)
        //    this is false, and we leave the decoder entirely untouched.
        if (!hasHwConfig(mPDecoder->codec, &hwPixFmt)) {
            AF_LOGI("%s: %s has no hardware decoder in this FFmpeg build, "
                    "using the software decoder\n", CICADA_HW_NAME, mPDecoder->codec->name);
            return false;
        }

        // 2. Create the D3D11 device (and its immediate context) FFmpeg will
        //    decode on. A nullptr device string selects the default adapter.
        AVBufferRef *deviceRef = nullptr;
        int ret = av_hwdevice_ctx_create(&deviceRef, CICADA_HW_DEVICE_TYPE, nullptr, nullptr, 0);

        if (ret < 0) {
            AF_LOGW("%s: av_hwdevice_ctx_create failed: %s, using the software decoder\n",
                    CICADA_HW_NAME, getErrorString(ret));
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
        mPDecoder->codecCont->get_format = getHwFormat;

        AF_LOGI("%s: hardware decoding enabled for %s (hw pixel format %s)\n",
                CICADA_HW_NAME, mPDecoder->codec->name, av_get_pix_fmt_name(hwPixFmt));
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
     * Returns the frame to hand to the caller, or nullptr when the download was
     * not possible. nullptr must be treated as "drop this frame": a D3D11 frame
     * handed on as if it were CPU data has a texture pointer in data[0] and a
     * surface index in linesize[0], which is what produced a green window with
     * "Parameter 'Ypitch' is invalid" in the log.
     */
    AVFrame *avcodecDecoder::retrieveHwFrame(AVFrame *hwFrame)
    {
        /*
         * The decoded frame carries the surface pool it came from, and that is
         * the only reliable place to read the software layout: for H.264 / HEVC
         * FFmpeg allocates the pool lazily when the first frame is parsed, so
         * codecCont->hw_frames_ctx is still null right after avcodec_open2().
         */
        auto *framesCtx = reinterpret_cast<AVHWFramesContext *>(
                hwFrame->hw_frames_ctx ? hwFrame->hw_frames_ctx->data : nullptr);

        if (framesCtx == nullptr) {
            AF_LOGE("%s: the decoded hardware frame carries no hw_frames_ctx, "
                    "so it cannot be downloaded\n", CICADA_HW_NAME);
            return nullptr;
        }

        if (mPDecoder->swPixFmt == AV_PIX_FMT_NONE) {
            mPDecoder->swPixFmt = framesCtx->sw_format;
            AF_LOGI("%s: surfaces %dx%d, download format %s\n", CICADA_HW_NAME,
                    framesCtx->width, framesCtx->height,
                    av_get_pix_fmt_name(framesCtx->sw_format));
        }

        if (mPDecoder->swFrame == nullptr) {
            mPDecoder->swFrame = av_frame_alloc();

            if (mPDecoder->swFrame == nullptr) {
                AF_LOGE("%s: cannot allocate the copy-back frame\n", CICADA_HW_NAME);
                return nullptr;
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
            AF_LOGE("%s: av_hwframe_transfer_data failed: %s\n",
                    CICADA_HW_NAME, getErrorString(ret));
            return nullptr;
        }

        // Keep pts / duration / metadata - av_hwframe_transfer_data only copies
        // the pixel data.
        av_frame_copy_props(swFrame, hwFrame);

        /*
         * One-off diagnostic on the first downloaded frame.
         *
         * A luminance plane that is entirely zero is exactly what a solid green
         * picture looks like: Y=U=V=0 converts to RGB(0,135,0). So when the
         * D3D11 texture download silently returns no data the user sees a green
         * window and an otherwise working player, with nothing in the log to
         * explain it. Checking once and saying so turns that into a concrete
         * message.
         */
        if (!mPDecoder->hwDownloadLogged) {
            mPDecoder->hwDownloadLogged = true;

            const uint8_t *y = swFrame->data[0];
            bool allZero = (y != nullptr);
            int ySize = swFrame->linesize[0] * swFrame->height;

            for (int i = 0; y != nullptr && i < ySize; ++i) {
                if (y[i] != 0) {
                    allZero = false;
                    break;
                }
            }

            AF_LOGI("%s: downloaded %s %dx%d linesize=%d/%d\n", CICADA_HW_NAME,
                    av_get_pix_fmt_name((enum AVPixelFormat) swFrame->format),
                    swFrame->width, swFrame->height,
                    swFrame->linesize[0], swFrame->linesize[1]);

            if (allZero) {
                AF_LOGE("%s: the downloaded Y plane is ALL ZERO - the GPU surface "
                        "copy returned no pixel data, which renders as a solid "
                        "green picture. Run with -sw to confirm, then check that "
                        "the GPU driver and the device FFmpeg created agree on the "
                        "decoder surfaces.\n", CICADA_HW_NAME);
            }
        }

        if (swFrame->format == AV_PIX_FMT_YUV420P) {
            return swFrame;
        }

        // Convert to YUV420P for the renderer.
        if (mPDecoder->convFrame == nullptr) {
            mPDecoder->convFrame = av_frame_alloc();

            if (mPDecoder->convFrame == nullptr) {
                AF_LOGE("%s: cannot allocate the conversion frame\n", CICADA_HW_NAME);
                return nullptr;
            }
        }

        AVFrame *out = mPDecoder->convFrame;
        av_frame_unref(out);
        out->format = AV_PIX_FMT_YUV420P;
        out->width = swFrame->width;
        out->height = swFrame->height;

        if (av_frame_get_buffer(out, 0) < 0) {
            AF_LOGE("%s: cannot allocate the YUV420P conversion buffer\n", CICADA_HW_NAME);
            return nullptr;
        }

        mPDecoder->swsCtx = sws_getCachedContext(static_cast<SwsContext *>(mPDecoder->swsCtx),
                                                 swFrame->width, swFrame->height,
                                                 (enum AVPixelFormat) swFrame->format,
                                                 swFrame->width, swFrame->height,
                                                 AV_PIX_FMT_YUV420P,
                                                 SWS_POINT, nullptr, nullptr, nullptr);

        if (mPDecoder->swsCtx == nullptr) {
            AF_LOGE("%s: cannot create the swscale context\n", CICADA_HW_NAME);
            return nullptr;
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
#if defined(CICADA_HW_DEVICE_TYPE)
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
#if defined(CICADA_HW_DEVICE_TYPE)

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
#if defined(CICADA_HW_DEVICE_TYPE)

        /*
         * A D3D11 frame must never reach the renderer as if it were CPU data:
         * its data[0] is an ID3D11Texture2D pointer and its linesize[0] is a
         * surface index, which SDL rejects with "Parameter 'Ypitch' is invalid"
         * for every frame and draws as a green window. Download it, or drop it.
         *
         * The format test also catches the case where FFmpeg still decodes into
         * D3D11 surfaces while hwDecodeActive is false (get_format installed but
         * the download disabled) - that is exactly how the green screen appeared.
         */
        if (mPDecoder->avFrame->format == mPDecoder->hwPixFmt) {
            if (!mPDecoder->hwDecodeActive) {
                AF_LOGW("%s: dropping a hardware frame while hardware decoding is disabled\n",
                        CICADA_HW_NAME);
                return -EAGAIN;
            }

            outFrame = retrieveHwFrame(mPDecoder->avFrame);

            if (outFrame == nullptr) {
                AF_LOGW("%s: dropping a hardware frame that could not be downloaded\n",
                        CICADA_HW_NAME);
                return -EAGAIN;
            }
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
