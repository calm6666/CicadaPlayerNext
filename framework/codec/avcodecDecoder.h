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

namespace Cicada{
    class CICADA_CPLUS_EXTERN avcodecDecoder : public ActiveDecoder, private codecPrototype {
    private:
        struct decoder_handle_v {
            AVCodecContext *codecCont;
            const AVCodec *codec;
            AVFrame *avFrame;
            video_info vInfo;
#if defined(_WIN32)
            /*
             * D3D11VA hardware decoding (copy-back).
             *
             * The decoder feeds the GPU through hwDeviceRef, and each decoded
             * surface is downloaded back to system memory (swFrame) so the
             * existing SDL renderer keeps working unchanged.
             *
             * hwDecodeActive stays false - and none of these are used - when the
             * linked FFmpeg has no D3D11VA hwaccel compiled in. That is the case
             * for the prebuilt external/install/ffmpeg/win32/*\/libffmpeg.dll,
             * which was configured with --disable-hwaccels; the fix is to
             * cross-compile FFmpeg on Linux with the *_d3d11va2 hwaccels that
             * external/player_ffmpeg_config.sh now requests.
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

#if defined(_WIN32)
        /*
         * Pixel format negotiation callback handed to FFmpeg: accepts the
         * D3D11VA format and refuses everything else. It is a member so it can
         * see the private decoder_handle_v stored in AVCodecContext::opaque.
         */
        static enum AVPixelFormat getD3D11vaFormat(AVCodecContext *ctx,
                                                   const enum AVPixelFormat *pixFmts);

        /*
         * Tries to set the decoder up for D3D11VA hardware decoding. Returns
         * true only when the device, the frames context and the pixel format
         * negotiation all succeeded; on any failure the decoder is left
         * untouched so it runs in software, and the caller reports DECFLAG_SW.
         */
        bool initD3D11va(const Stream_meta *meta);

        /*
         * Downloads a decoded D3D11 surface into system memory and returns the
         * frame the caller should use. Returns hwFrame unchanged when the
         * download is not possible.
         */
        AVFrame *retrieveHwFrame(AVFrame *hwFrame);
#endif

    private:
        decoder_handle_v *mPDecoder = nullptr;
    };
}


#endif //FRAMEWORK_AVCODECDECODER_H
