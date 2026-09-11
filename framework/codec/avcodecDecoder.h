//
// Created by moqi on 2018/8/10.
//

#ifndef FRAMEWORK_AVCODECDECODER_H
#define FRAMEWORK_AVCODECDECODER_H

#include "codec/ActiveDecoder.h"

#include <mutex>
#include <codec/IDecoder.h>
#include "base/media/AVAFPacket.h"
#include "codecPrototype.h"

typedef struct cicada_decoder_handle_v_t cicada_decoder_handle_v;

/*
 * Hardware decoding backend selection.
 *
 * The design is the same everywhere: FFmpeg decodes on the GPU into a surface
 * pool and every frame is downloaded back to system memory (copy-back) so the
 * RAM based renderer keeps working unchanged. Only two things differ per
 * platform - which device FFmpeg is asked for, and which pixel format marks a
 * frame as still living on the GPU:
 *
 *   Windows : D3D11VA / AV_PIX_FMT_D3D11
 *   Linux   : VAAPI   / AV_PIX_FMT_VAAPI
 *
 * Both are detected at run time through avcodec_get_hw_config(), so a build
 * whose FFmpeg has no such hwaccel (the Windows D3D11VA list is compiled out of
 * a --disable-hwaccels build, the Linux VAAPI list is dropped when libva was
 * missing) simply reports no hardware configuration and falls back to software.
 * Nothing extra needs to be compiled out on either side.
 *
 * There is no equivalent block for Android, Apple or OHOS: those platforms have
 * dedicated decoder classes in framework/codec/{Android,Apple,OHOS}.
 */
#if defined(_WIN32)
#define CICADA_HW_DEVICE_TYPE AV_HWDEVICE_TYPE_D3D11VA
#define CICADA_HW_PIX_FMT     AV_PIX_FMT_D3D11
#define CICADA_HW_NAME        "D3D11VA"
#elif defined(__linux__)
#define CICADA_HW_DEVICE_TYPE AV_HWDEVICE_TYPE_VAAPI
#define CICADA_HW_PIX_FMT     AV_PIX_FMT_VAAPI
#define CICADA_HW_NAME        "VAAPI"
#endif

namespace Cicada{
    class CICADA_CPLUS_EXTERN avcodecDecoder : public ActiveDecoder, private codecPrototype {
    private:
        struct decoder_handle_v {
            AVCodecContext *codecCont;
            const AVCodec *codec;
            AVFrame *avFrame;
            video_info vInfo;
#if defined(CICADA_HW_DEVICE_TYPE)
            /*
             * Hardware decoding (copy-back) state.
             *
             * The decoder feeds the GPU through hwDeviceRef, and each decoded
             * surface is downloaded back to system memory (swFrame) so the
             * existing SDL renderer keeps working unchanged.
             *
             * hwDecodeActive stays false - and none of these are used - when the
             * linked FFmpeg has no hwaccel compiled in for this codec, which is
             * decided by avcodec_get_hw_config() before a single frame is
             * decoded.
             */
            AVBufferRef *hwDeviceRef;
            enum AVPixelFormat hwPixFmt;
            enum AVPixelFormat swPixFmt;
            AVFrame *swFrame;
            AVFrame *convFrame;
            // Kept as void* so this header does not have to pull in
            // libswscale/swscale.h; cast to SwsContext* at the single use site.
            void *swsCtx;
            bool hwDecodeActive;
            bool hwDownloadLogged;
#endif
            int flags;
        };
    public:
        avcodecDecoder();

        ~avcodecDecoder() override;

        static bool is_supported(enum AFCodecID codec);

        void setEOF() override
        {
        }

    private:
        explicit avcodecDecoder(int dummy)
        {
            addPrototype(this);
        };

        avcodecDecoder *clone() override
        {
            return new avcodecDecoder();
        };

        bool is_supported(const Stream_meta& meta, uint64_t flags, int maxSize) override
        {
            if (flags & DECFLAG_SW)
                return is_supported(meta.codec);
            return false;
        };

        bool is_drmSupport(const DrmInfo *drmInfo) override {
            return false;
        }

        static avcodecDecoder se;

    private:

        int enqueue_decoder(std::unique_ptr<IAFPacket> &pPacket) override;

        int dequeue_decoder(std::unique_ptr<IAFFrame> &pFrame) override;

        int init_decoder(const Stream_meta *meta, void *wnd, uint64_t flags, const DrmInfo *drmInfo) override;

        void close_decoder() override;

        void flush_decoder() override;

        int get_decoder_recover_size() override
        {
            return 0;
        };
        virtual bool supportReuse() override;

        void decoder_updateMetaData(const Stream_meta *meta) override
        {}

#if defined(CICADA_HW_DEVICE_TYPE)
        /*
         * Pixel format negotiation callback handed to FFmpeg: accepts the
         * hardware format and refuses everything else. It is a member so it can
         * see the private decoder_handle_v stored in AVCodecContext::opaque.
         */
        static enum AVPixelFormat getHwFormat(AVCodecContext *ctx,
                                              const enum AVPixelFormat *pixFmts);

        /*
         * Tries to set the decoder up for hardware decoding. Returns true only
         * when the device was created; on any failure the decoder is left
         * untouched so it runs in software, and the caller reports DECFLAG_SW.
         */
        bool initHwDecoder(const Stream_meta *meta);

        /*
         * Downloads a decoded GPU surface into system memory and returns the
         * frame the caller should use, or nullptr when the download is not
         * possible (the caller must then drop the frame rather than hand a GPU
         * frame to the renderer).
         */
        AVFrame *retrieveHwFrame(AVFrame *hwFrame);
#endif

    private:
        decoder_handle_v *mPDecoder = nullptr;
    };
}


#endif //FRAMEWORK_AVCODECDECODER_H
