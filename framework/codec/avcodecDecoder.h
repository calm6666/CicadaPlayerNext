//
// Created by moqi on 2018/8/10.
//

#ifndef FRAMEWORK_AVCODECDECODER_H
#define FRAMEWORK_AVCODECDECODER_H

#include "codec/ActiveDecoder.h"

#include <atomic>
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
 *   macOS   : VideoToolbox / AV_PIX_FMT_VIDEOTOOLBOX（**默认关闭**，见下）
 *
 * Both are detected at run time through avcodec_get_hw_config(), so a build
 * whose FFmpeg has no such hwaccel (the Windows D3D11VA list is compiled out of
 * a --disable-hwaccels build, the Linux VAAPI list is dropped when libva was
 * missing) simply reports no hardware configuration and falls back to software.
 * Nothing extra needs to be compiled out on either side.
 *
 * Apple 是特例：framework/codec/Apple 有一套自己的 AFVTBDecoder（直接用
 * VideoToolbox 框架），历来不走 FFmpeg 的 hwaccel。macOS 上要做零拷贝必须拿到
 * CVPixelBuffer，所以这里也定义了 VideoToolbox 的宏，但**只有**应用显式打开全局
 * 设置 "video.decoder.ffmpeg_videotoolbox" 时才会激活（Qt 集成为了零拷贝会打开
 * 它），其它 Apple 应用的行为与改动前完全一致 —— 这层运行时开关在
 * initHwDecoder() 的开头。
 *
 * There is no equivalent block for Android or OHOS: those platforms have
 * dedicated decoder classes in framework/codec/{Android,OHOS}.
 */
#if defined(_WIN32)
#define CICADA_HW_DEVICE_TYPE AV_HWDEVICE_TYPE_D3D11VA
#define CICADA_HW_PIX_FMT     AV_PIX_FMT_D3D11
#define CICADA_HW_NAME        "D3D11VA"
/* 只有 D3D11 需要"借用外部设备"（Qt RHI 的 ID3D11Device）。 */
#define CICADA_HW_SUPPORTS_EXTERNAL_DEVICE 1
#elif defined(__linux__)
#define CICADA_HW_DEVICE_TYPE AV_HWDEVICE_TYPE_VAAPI
#define CICADA_HW_PIX_FMT     AV_PIX_FMT_VAAPI
#define CICADA_HW_NAME        "VAAPI"
#elif defined(__APPLE__)
/*
 * VideoToolbox：解码结果是 CVPixelBuffer（AVFrame::data[3]），macOS 上可以用
 * CVMetalTextureCache 直接包成 MTLTexture 交给 Qt 的 Metal 场景图，硬件解码 +
 * 零拷贝。默认不激活，理由见上面的大段说明。
 */
#define CICADA_HW_DEVICE_TYPE AV_HWDEVICE_TYPE_VIDEOTOOLBOX
#define CICADA_HW_PIX_FMT     AV_PIX_FMT_VIDEOTOOLBOX
#define CICADA_HW_NAME        "VideoToolbox"
#endif

namespace Cicada{
    /*
     * 硬解下载缓冲池的槽数。池子存在的唯一目的是别再每帧 malloc/munmap
     * 12.4MB（4K 一帧的大小）：一轮有 4~5 帧同时在解码器输出队列、播放器帧
     * 队列和渲染器手里，所以取 6 个槽就能稳定命中一个已经没人持有的缓冲。
     * 槽位仍然被占用时不会强行复用，而是照旧重新分配，绝不会把正在上屏的
     * 数据改花。
     */
#define CICADA_HW_SW_FRAME_POOL 6

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
             * surface is downloaded back to system memory (swFrames) so the
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
            /*
             * 下载缓冲池：每帧下载出来的 NV12 放在其中一个槽里，轮转使用。
             * 槽里的缓冲只在“没人再持有它”时才被覆盖，否则重新分配，因此
             * 交给渲染器的帧不会被后来的帧改花。
             */
            AVFrame *swFrames[CICADA_HW_SW_FRAME_POOL];
            int swFrameIndex;
            AVFrame *convFrame;
            // Kept as void* so this header does not have to pull in
            // libswscale/swscale.h; cast to SwsContext* at the single use site.
            void *swsCtx;
            bool hwDecodeActive;
            bool hwDownloadLogged;
            /*
             * 是否把下载后的 NV12 直接交给渲染器（跳过 swscale 转换）。
             * 由全局设置 "video.render.hw.copyback_nv12" 决定，只有明确声明
             * 自己能吃 NV12 的呈现方（cmdline 的 onVideoRendering）才会打开，
             * 默认关闭即维持原来的 YUV420P 行为。
             */
            bool outputNv12;
            /*
             * 零拷贝直通：解码出来的 GPU 纹理不下载，原样交给上层去画。
             * 由全局设置 "video.render.hw.direct_texture" 打开，只有自己会用
             * D3D11 呈现解码纹理的调用方（cmdline -direct）才应该打开。
             */
            bool directOutput;
#endif
            int flags;
            /*
             * 反向指针：getHwFormat() 是给 FFmpeg 的**静态**回调（ctx->opaque 里只能
             * 拿到本结构），而"硬解在不在用"这个对外读数记在 avcodecDecoder 自己身上，
             * 所以要借它回到那个对象上写。构造函数里 memset 之后立刻赋值。
             */
            avcodecDecoder *owner = nullptr;
        };
    public:
        avcodecDecoder();

        ~avcodecDecoder() override;

        static bool is_supported(enum AFCodecID codec);

        /*
         * 【设备硬解能力查询（Windows / Linux / Apple 的 FFmpeg 后端实现）】
         *
         * 只回答"**这一份 FFmpeg 构建**里这个编码有没有本平台的硬解配置"
         * （avcodec_get_hw_config 匹配 CICADA_HW_DEVICE_TYPE / CICADA_HW_PIX_FMT，
         * 与 initHwDecoder() 用的是同一个 hasHwConfig()），**不创建设备**。
         *
         * 因此返回值只有两种含义：
         *   true  = 构建里有该编码的硬解配置（设备是否真的能解，交给创建时的
         *           hw->sw 回退兜底，这里不猜）；
         *   false = 构建里明确没有该编码的硬解配置，硬件解码不可能发生。
         * 非视频编码、以及本平台没有硬解后端（未定义 CICADA_HW_DEVICE_TYPE）时返回 false。
         * 编码未知（AF_CODEC_ID_NONE）时不在这里处理，由
         * decoderFactory::isHardwareDecodeSupported() 统一按"视为支持"返回 true。
         *
         * 只声明，不新增虚函数：类布局与 vtable 不变。
         */
        static bool is_hardware_decode_supported(enum AFCodecID codec);

        void setEOF() override
        {
        }

        /*
         * 【当前解码方式（硬解/软解）的事实读数，界面显示用】
         *
         * 只看**本解码器实例实际在用**的那条路，不回答"设备支持不支持"：
         *   * initHwDecoder() 真的把硬解设备建起来、并且 FFmpeg 一路都用硬解格式
         *     ⇒ true；
         *   * 构建里没有该编码的硬解配置、硬解设备建不出来、或者解码途中 FFmpeg
         *     把硬解格式从协商列表里摘掉（getHwFormat() 的运行期降级，画面继续播、
         *     这条流退回软解）⇒ false。
         *
         * 覆盖 IDecoder::isHardwareDecoderInUse()：基类那个默认实现只看打开时留下的
         * 标志位，而这里的状态在解码途中还会变（运行期降级），必须单独跟踪。
         *
         * 【同一状态镜像在 IDecoder::mFlags 的 DECFLAG_HW 位上】打开成功时两份一起置真，
         * 运行期降级/拒绝协商/关闭时两份一起复位（见 .cpp 的 init_decoder() 与
         * getHwFormat()）。界面读的是这个方法，而 SMPAVDeviceManager::getVideoDecoderFlags()
         * 读的是那一位（解码器重建时靠它继续请求硬解），两者必须永远一致。
         */
        bool isHardwareDecoderInUse() override;

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
        /*
         * "这条流的解码器现在在不在用硬解"的对外读数（**追加在类成员末尾**）。
         *
         * 用原子量：写它的有两处线程 —— 初始化/关闭在调用 open/close 的线程上，
         * 而运行期降级发生在 getHwFormat()（FFmpeg 解码线程）；读它的是界面线程
         * （Qt 每 2Hz 组装"解码方式"那一栏）。普通 bool 跨线程读写是数据竞争，
         * 这里只求一个一致的单值，原子量正合适。
         *
         * 同一事实还镜像在 IDecoder::mFlags 的 DECFLAG_HW 位上（只有解码器自己那条
         * 线程会写，读取方是 getVideoDecoderFlags()），两份状态在 .cpp 里成对写。
         */
        std::atomic_bool mHwDecodeInUse{false};
    };
}


#endif //FRAMEWORK_AVCODECDECODER_H
