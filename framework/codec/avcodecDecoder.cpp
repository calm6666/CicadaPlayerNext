//
// Created by moqi on 2018/8/10.
//
#define LOG_TAG "avcodecDecoder"

#if defined(_WIN32)
/*
 * d3d11.h 必须先以 C++ 链接进来。libavutil/hwcontext_d3d11va.h 会包含它，而下面
 * 的 FFmpeg 头都包在 extern "C" 里，整块进去会让 d3d11.h 为 D3D11_VIEWPORT /
 * D3D11_RECT / D3D11_BOX 定义的 C++ 比较运算符变成 C 链接，MSVC 全部报 C2733。
 * d3d11.h 有 include guard，所以下面那次包含是空操作，直接拿到结构体定义。
 */
#include <d3d11.h>
#endif

extern "C" {
#include <libavformat/avformat.h>
/*
 * avcodec.h 自带 extern "C" 保护（和 pixdesc.h / swscale.h 不同），放在这里
 * 只是为了不依赖别的头间接把它带进来：getHwFormat() 用的 avcodec_is_open()
 * 声明在它里面。
 */
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
/*
 * The platform test here is deliberately NOT `defined(CICADA_HW_DEVICE_TYPE)`.
 * Those macros live in avcodecDecoder.h, which this file only includes further
 * down, so testing them at this point would be false on every platform and these
 * three headers would never be pulled in - which is exactly how av_get_pix_fmt_name
 * and SwsContext ended up undeclared:
 *   avcodecDecoder.cpp(79,9): error C3861: "av_get_pix_fmt_name": identifier not found
 *   avcodecDecoder.cpp(148,41): error C2061: syntax error: identifier "SwsContext"
 *
 * __APPLE__ 也在列表里：Apple 上现在也定义了 CICADA_HW_DEVICE_TYPE
 * （VideoToolbox，默认不激活，见 avcodecDecoder.h），少了这几个头会直接编不过。
 */
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
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
 * libavutil/hwcontext_d3d11va.h is the single header that does #include <d3d11.h>,
 * and the C++ comparison operators that d3d11.h defines for D3D11_VIEWPORT /
 * D3D11_RECT / D3D11_BOX would get C linkage inside an extern "C" block, which
 * makes MSVC reject every one of them with C2733. It is needed only for
 * AVD3D11VADeviceContext, which the zero-copy path touches in initHwDecoder(),
 * and the way out is to include d3d11.h with C++ linkage FIRST (see the top of
 * this file): its include guard then makes this second, extern "C" include a
 * no-op and the struct arrives intact.
 */
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#if defined(_WIN32)
/*
 * 只为了 AVD3D11VADeviceContext：零拷贝直通要在设备级补 BindFlags，见
 * initHwDecoder()。d3d11.h 已经在文件最上面以 C++ 链接进来过了。
 */
#include <libavutil/hwcontext_d3d11va.h>
#endif
#endif
};

#include <utils/AFUtils.h>
#include <cstring>
#include <cstdlib>
#include <utils/frame_work_log.h>
#include <utils/globalSettings.h>
#include <utils/mediaFrame.h>
#include <utils/ffmpeg_utils.h>
#include <utils/property.h>
/* 零拷贝：借用平台（Qt）已有的 GPU 设备，见 hwDeviceBridge.h。 */
#include <utils/hwDeviceBridge.h>
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
     * codec can output; we accept the D3D11VA one.
     *
     * 列表里没有硬解格式时怎么处理，取决于协商发生在什么时候：
     *   - 打开之前（第一次协商）：返回 NONE，让 avcodec_open2() 干净地失败，
     *     调用方随后退回软解。
     *   - 打开之后：说明是解码途中 FFmpeg 把硬件加速摘掉了（hwaccel 初始化失败、
     *     设备丢失之类）。这时再返回 NONE 会让**每一帧**都解码失败：
     *     "no frame!" / "Error while decoding frame -1094995529" 刷屏，一帧都
     *     出不来，窗口就是一片空白（实测就是这样）。所以改成让 FFmpeg 挑一个
     *     软件格式继续解，画面接着播，只是这条流退回软解。
     *
     * ============ 【这个回调什么时候被调用，以及"复位会不会误伤"】 ============
     *
     * 唯一的调用入口是 FFmpeg 的 ff_get_format()（本仓库自带的 FFmpeg 源码：
     * external/external/ffmpeg/libavcodec/decode.c:1229），而它**每一次都会调本回调**
     * （没有"结果缓存、命中就不再回调"这回事）：
     *
     *   user_choice = avctx->get_format(avctx, choices);        decode.c:1258
     *
     * 而且它是个重试循环：本回调选中的硬解格式如果 hwaccel_init() 失败
     * （decode.c:1341-1343），它会 goto try_again，**把那个格式从候选列表 choices
     * 里删掉**再回调一次（decode.c:1348-1357；每轮进循环前还会 ff_hwaccel_uninit()）。
     * 所以"第二次进来时列表里已经没有 d3d11"只有一个含义：**FFmpeg 已经决定这条流
     * 不再用 GPU 解**（硬件初始化失败或设备丢失）。这时复位 mHwDecodeInUse 是如实反映
     * 事实，不是误伤 —— 界面必须跟着变成"软解"。
     *
     * 那 initHwDecoder() 里置的那个真值会不会被"打开期间的第一次协商"覆盖掉？不会，
     * 因为 h264/hevc 都**不在 avcodec_open2() 里**协商像素格式：
     *   * h264：调用点是 h264_slice.c:921（get_pixel_format()，由
     *     h264_slice_header_init() 调），也就是第一个 slice 解析的时候；
     *   * hevc：调用点是 hevcdec.c:712（get_format()，由 picture header 那段
     *     :3260 调），同样在第一帧的图像头到达时。
     * avcodec_open2() 里只有 codec->init()（avcodec.c:339-349），它只解析 extradata
     * 里的参数集，走不到这两个调用点。
     *
     * 另一个要写清楚的时序事实：avcodec_is_open() 就是 `!!s->internal`
     * （avcodec.c:702-705），而 internal 在 avcodec_open2() 的一开头就分配好了
     * （avcodec.c:194-201，早于 codec->init()）。所以在 avcodec_open2() 内部这个判断
     * 已经为真 —— 下面"打开之后才降级"这条分支的判据**不是**"在不在 open 里"，而是
     * "候选列表里真的没有硬解格式了"，这正是我们要的。
     */
    enum AVPixelFormat avcodecDecoder::getHwFormat(AVCodecContext *ctx,
                                                        const enum AVPixelFormat *pixFmts)
    {
        auto *dec = static_cast<decoder_handle_v *>(ctx->opaque);

        for (const enum AVPixelFormat *p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
            if (*p == dec->hwPixFmt) {
                /*
                 * 硬解格式回到列表里了（例如上一轮协商把它摘掉、现在又给了）：
                 * 标记也恢复，否则后面交上来的 D3D11 帧会被当成"硬解已关"丢掉，
                 * 画面就停住了。
                 */
                if (!dec->hwDecodeActive) {
                    AF_LOGW("%s: %s is in the format list again, going back to hardware "
                            "decoding\n", CICADA_HW_NAME, av_get_pix_fmt_name(dec->hwPixFmt));
                }

                dec->hwDecodeActive = true;
                dec->owner->mHwDecodeInUse.store(true);
                /* 两份状态一起写：界面读的是上面那个，IDecoder::getFlags() 读的是下面这个。 */
                dec->owner->mFlags |= (int) DECFLAG_HW;
                dec->flags = DECFLAG_HW;
                return *p;
            }
        }

        if (avcodec_is_open(ctx)) {
            for (const enum AVPixelFormat *p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
                const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);

                if (desc != nullptr && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0) {
                    AF_LOGW("%s: %s is gone from the format list, decoding the rest of "
                            "this stream as %s\n", CICADA_HW_NAME,
                            av_get_pix_fmt_name(dec->hwPixFmt), av_get_pix_fmt_name(*p));
                    dec->hwDecodeActive = false;
                    /*
                     * 运行期降级：界面上的"硬解"必须跟着变成"软解"。两份状态一起写 ——
                     * mHwDecodeInUse 是界面读数，IDecoder::mFlags 那个位是解码器重建时
                     * 用来"继续请求硬解"的（见 init_decoder() 里的说明）。
                     */
                    dec->owner->mHwDecodeInUse.store(false);
                    dec->owner->mFlags &= ~(int) DECFLAG_HW;
                    dec->flags = DECFLAG_SW;
                    return *p;
                }
            }
        }

        /*
         * 拒绝协商（返回 NONE）：这一轮硬解没成，调用方会走软解那条路，
         * 两份对外读数一并复位，别让"上一次打开留下来的真值"骗过 getFlags()。
         */
        dec->hwDecodeActive = false;
        dec->owner->mHwDecodeInUse.store(false);
        dec->owner->mFlags &= ~(int) DECFLAG_HW;
        dec->flags = DECFLAG_SW;
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
        /* 解码器已经关掉：对外读数回到"没有硬解在用"（下次 open 会重新判定）。 */
        mHwDecodeInUse.store(false);
        /* IDecoder::mFlags 那一份同步复位（理由见 init_decoder() 里的说明）。 */
        mFlags &= ~(int) DECFLAG_HW;

        if (mPDecoder->swsCtx != nullptr) {
            sws_freeContext(static_cast<SwsContext *>(mPDecoder->swsCtx));
            mPDecoder->swsCtx = nullptr;
        }

        if (mPDecoder->convFrame != nullptr) {
            av_frame_free(&mPDecoder->convFrame);
        }

        for (AVFrame *&frame : mPDecoder->swFrames) {
            if (frame != nullptr) {
                av_frame_free(&frame);
            }
        }

        // Must go before avcodec_free_context(): the codec context holds its own
        // reference and releases it on free.
        if (mPDecoder->hwDeviceRef != nullptr) {
            /*
             * ============ 【探针修正·2026-09-30：原来那两条读数恒为常量】============
             *
             * 这里原来用 av_buffer_is_writable() 判断"还有没有别人拿着"，
             * 那是**恒为 0** 的，原因在 FFmpeg 源码里：
             *   * av_hwdevice_ctx_alloc() / av_hwframe_ctx_alloc() 建 buffer 时都带了
             *     AV_BUFFER_FLAG_READONLY（libavutil/hwcontext.c:204-206 与 :286-288）；
             *   * av_buffer_is_writable() 对带 READONLY 的 buffer **直接返回 0**
             *     （libavutil/buffer.c:147-153），与引用计数完全无关。
             *
             * 所以旧日志里 `exclusively owned=0`（设备那条）和 `externally pinned=1`
             * （池那条）是**常量**，不是结论 —— 拿它们当"池被钉住"的证据是错的。
             * 真正有信息量的是引用计数：此处持有者 = 解码器自己(1) + 我们的探针(1)
             * + 其它还活着的帧(N)。
             */
            AF_LOGI("[mem] hw device ref: holders outside this object=%d "
                    "(refcount uses the same READONLY-buffer caveat as the pool probe)\n",
                    av_buffer_get_ref_count(mPDecoder->hwDeviceRef) - 1);

            av_buffer_unref(&mPDecoder->hwDeviceRef);
        }
#endif

        /*
         * 探针要用的解码器名字（下面会把 mPDecoder->codec 清空，所以先取）。
         * 一轮关窗会看到**两个**解码器实例的这套日志：视频一个、音频一个 ——
         * 音频那个没有 hw frames ctx（日志里显示 `none`），带池的是视频这个。
         * 名字打在日志里，就不必再靠顺序猜哪个是哪个。
         */
        const char *probeCodecName = (mPDecoder->codec != nullptr) ? mPDecoder->codec->name : "?";

        /*
         * ============ 【顺序修复·本轮】先放解码器自己那只复用帧，再 free codec context ============
         *
         * 为什么必须换顺序：`dequeue_decoder()` 里那句
         * `avcodec_receive_frame(codecCont, mPDecoder->avFrame)` 会把**最近解出的那一帧**
         * 留在 mPDecoder->avFrame 里（FFmpeg 只先 unref 再填，不会替我们放掉），
         * 而零拷贝帧的 buf[] 里握着的正是 hw frames 上下文（= 那张 20 片表面数组）。
         *
         * 原来的顺序是 `avcodec_free_context()` 在前、`av_frame_free(&mPDecoder->avFrame)`
         * 在后，于是"池还被谁钉着"这个读数里**永远混着我们自己这一份** —— 无条件
         * writable=0，根本看不出外部是否还有帧（上一轮那条 `=0` 就是这么来的，
         * 它并不能证明"有外部帧"）。
         *
         * 换到前面之后，紧跟着那条 hw frames ctx 探针才第一次**有信息量**：
         * 它报的"外部持有者数"归零才真的意味着池会随解码器一起走。
         */
        av_frame_free(&mPDecoder->avFrame);

        AVBufferRef *poolProbeRef = nullptr;

        if (mPDecoder->codecCont != nullptr) {
            /*
             * 【释放探针·表面池】hw frames 上下文就是那张表面数组（1080p 20 片 ≈62MB）。
             *
             * 走到这里时，"我们自己这边"只剩 codec context 这一份引用（上面 avFrame 已放），
             * 所以引用计数 = 1 + 外部还活着的帧数。**判据是引用计数**，
             * 不是 av_buffer_is_writable()（后者对这个 READONLY 的 buffer 恒为 0，
             * 见函数开头那段修正说明）。
             */
            if (mPDecoder->codecCont->hw_frames_ctx != nullptr) {
                auto *framesCtx = reinterpret_cast<AVHWFramesContext *>(
                                      mPDecoder->codecCont->hw_frames_ctx->data);

                /*
                 * 引用计数才是判据（av_buffer_is_writable 对 READONLY 的 hw frames ctx
                 * 恒为 0，见上面的修正说明）：此刻 = 解码器自己(1) + 外部帧(N)。
                 */
                const int poolRefCount = av_buffer_get_ref_count(mPDecoder->codecCont->hw_frames_ctx);

                AF_LOGI("[mem] hw frames ctx (the surface pool) refcount=%d before our probe ref: "
                        "holders outside the codec context=%d (codec %s)\n",
                        poolRefCount, poolRefCount - 1, probeCodecName);

                /*
                 * 池有多大，让日志自己说清楚（不然"到底是多少 MB 没回来"只能靠猜）。
                 * NV12 = 1.5 字节/像素；P010 的每个分量装在 16 位里 = 3 字节/像素。
                 */
                if (framesCtx != nullptr && framesCtx->initial_pool_size > 0) {
                    const long long pixels = (long long) framesCtx->width * framesCtx->height;
                    const char *formatName = av_get_pix_fmt_name(framesCtx->sw_format);
                    long long bytesPerSlice = 0;

                    if (framesCtx->sw_format == AV_PIX_FMT_NV12) {
                        bytesPerSlice = pixels * 3 / 2;
                    } else if (framesCtx->sw_format == AV_PIX_FMT_P010) {
                        bytesPerSlice = pixels * 3;
                    }

                    if (formatName == nullptr) {
                        formatName = "?";
                    }

                    if (bytesPerSlice > 0) {
                        AF_LOGI("[mem] pool geometry: %dx%d %s x %d slice(s) -> about %lld MB\n",
                                framesCtx->width, framesCtx->height, formatName,
                                framesCtx->initial_pool_size,
                                bytesPerSlice * framesCtx->initial_pool_size / (1024 * 1024));
                    } else {
                        AF_LOGI("[mem] pool geometry: %dx%d %s x %d slice(s) (no size estimate for "
                                "this pixel format)\n", framesCtx->width, framesCtx->height,
                                formatName, framesCtx->initial_pool_size);
                    }
                }

                /* 取一份临时引用，供下面"我们全放完之后再看一次"用。 */
                poolProbeRef = av_buffer_ref(mPDecoder->codecCont->hw_frames_ctx);
            } else {
                AF_LOGI("[mem] hw frames ctx: none (codec %s; software decoding, or the pool was never created)\n",
                        probeCodecName);
            }

            /*
             * ============ 【顺序：先放掉表面池的那一份引用，再销毁解码器】============
             *
             * 用户要求把顺序写死成"先释放 d3d11va，再销毁解码器"，这里就是那一步：
             * codec context 自己持有的 hw_frames_ctx 引用（= 表面池本体）在
             * `avcodec_free_context()` **之前**显式放掉，池的生死因此不再取决于
             * avcodec_free_context() 内部的时序。
             *
             * 为什么这样做是安全的、且不是"重复释放"：
             *   * avcodec_free_context() -> avcodec_close() -> ff_hwaccel_uninit()
             *     本来也会 unref 这一份（本仓库自带源码：libavcodec/decode.c:1226），
             *     而 av_buffer_unref() 对 nullptr 是空操作，所以这里先放一次之后
             *     内部那次就是无事发生；
             *   * 本仓库自带的 libavcodec/d3d11va.c 全程不读 hw_frames_ctx，
             *     hwaccel 的 uninit 不需要它非空。
             *
             * 注意：**这一份引用归零并不等于池就释放了** —— 只要解码器之外还有一帧
             * 活着，池就活着（就是下面那条 externally pinned 探针要报的东西）。
             * 所以真正让池回收的动作有两个，都在别处：
             *   1. 渲染器/呈现方在解码器关闭之前同步放帧（IVideoRender::releaseFrames()，
             *      由 SuperMediaPlayer::FlushVideoPath()/rebuildVideoDecoder()/CreateVideoDecoder()
             *      调用）；
             *   2. D3D11 那一层还要 Flush 一次立即上下文 —— 官方文档写明 D3D11
             *      **默认延迟销毁**对象，引用计数归零只代表"可以销毁"
             *      （见 platform/QtPlayer/src/CicadaTextureD3D11.h 里引的原文）。
             */
            if (mPDecoder->codecCont->hw_frames_ctx != nullptr) {
                AF_LOGI("[mem] dropping the codec context's own reference to the pool "
                        "(refcount %d before, codec %s)\n",
                        av_buffer_get_ref_count(mPDecoder->codecCont->hw_frames_ctx),
                        probeCodecName);

                av_buffer_unref(&mPDecoder->codecCont->hw_frames_ctx);
            }

            // avcodec_close was removed in FFmpeg 6.0; avcodec_free_context()
            // closes and frees the context in one call.
            avcodec_free_context(&mPDecoder->codecCont);
            mPDecoder->codecCont = nullptr;
        }

        mPDecoder->codec = nullptr;
        delete mPDecoder;
        mPDecoder = nullptr;

        /*
         * 【释放探针·决定性的一条】我们（复用帧 + codec context）**全放完之后**再问一次：
         * 这张池在外面还有没有人拿着？
         *
         * 这一条不受"我们自己的 avFrame / codec context"干扰，是"表面池到底有没有被
         * 外部帧钉住"的最终判据：
         *   * `externally pinned=0` ⇒ 池随解码器一起走了（关窗就该这样）；
         *   * `=1` ⇒ **解码器之外确实还有帧活着**，62MB 的账记在它头上，
         *     下一步就是按 frames ctx 的引用链找那一帧是谁（Qt 侧 m_currentFrame /
         *     渲染线程那一帧 / 框架侧队列里还没放掉的帧）。
         */
        if (poolProbeRef != nullptr) {
            /*
             * 我们自己的探针引用占 1，剩下几就是"解码器之外还活着的持有者"。
             * 0 ⇒ 池随解码器一起释放（这是期望值）。
             */
            const int outsideHolders = av_buffer_get_ref_count(poolProbeRef) - 1;

            AF_LOGI("[mem] surface pool after decoder teardown (codec %s): holders outside the "
                    "decoder=%d (0 = freed together with the decoder)\n",
                    probeCodecName, outsideHolders);

            if (outsideHolders > 0) {
                /*
                 * 报 1 的时候把"已经排除了谁"写清楚，免得下一轮又从同一批嫌疑里重新找：
                 * 走到这条日志时，下面这些都已经在**解码器之前**放掉了 ——
                 *   * 播放器侧帧队列（SuperMediaPlayer::mVideoFrameQue，FlushVideoPath 清）；
                 *   * 渲染器队列 / 正在渲染的那一帧 / 上屏后备帧 / 截屏缓存
                 *     （IVideoRender::releaseFrames()，同步）；
                 *   * 解码器自己的复用帧与 codec context 的那一份引用（本函数上面）。
                 * 所以还剩的那一帧只可能在**呈现方自己手上**（Qt 组件 m_currentFrame 或
                 * m_pendingFrame 的克隆、TextureFrame 等），它会在下一帧到达时被顶掉；
                 * 若要"立刻"放掉，需要呈现方在收到"解码代际结束"通知后主动放
                 * （Qt 侧已经接了：见 CicadaVideoRender::releaseFrames() 与 QML 组件里
                 * m_decoderGenerationEnded 那一段）。
                 */
                AF_LOGW("[mem] the pool is still pinned by a decoded frame outside the decoder: "
                        "the player queue, the renderer queue/back frame and the capture cache were "
                        "already released before this point, so what is left is the presenter's own "
                        "current frame (Qt item) -- it goes away when the next frame replaces it, or "
                        "immediately if the presenter handles the decoder-generation-ended hook\n");
            }

            av_buffer_unref(&poolProbeRef);
        }

        /*
         * 【释放探针】解码器关闭的完成点。
         *
         * 走到这里意味着：codec context 已 free（它拥有的 hw_frames_ctx 一起走）、
         * 硬解设备引用已 unref、copy-back 的下载帧池（swFrames[]，见上面那个循环）
         * 已逐个释放。**D3D11VA 那 20 片 surface 的整张纹理数组就是在这里失去最后一个
         * 引用的**（1080p NV12 约 62MB、4K 约 249MB），所以这一行日志是"关窗后表面池
         * 真的还回去了"的直接判据。
         */
        AF_LOGI("decoder closed: codec context freed, hardware device reference released, "
                "copy-back frame pool freed (the hardware surface pool goes away with them)\n");
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
            /*
             * ============ 【IDecoder::mFlags 里的 DECFLAG_HW 必须跟着置上】 ============
             *
             * 这个位是"解码器打开之后对外报的解码方式"（IDecoder::getFlags()），
             * SMPAVDeviceManager::getVideoDecoderFlags() 读的就是它，而它有三个消费者：
             *   * SuperMediaPlayer::GetDecoderType()（对外 API 的硬解/软解读数）；
             *   * SuperMediaPlayer::rebuildVideoDecoder()（:7189）—— 解码器重建时用
             *     "上一次是不是硬解"决定这一次还要不要请求硬解，这个位是假的话，一次
             *     解码器重建（错误恢复、切档时 meta 不匹配）就把整条流**悄悄换成软解**，
             *     而用户的"硬解"开关还开着 —— 这正是"我开的是硬解、面板却显示软件"；
             *   * setUpVideoDecoder() 里那条"已切到软解"的用户通知（:6955-6960）。
             *
             * 以前这里只写了 mPDecoder->flags（本对象内部那份），IDecoder::mFlags 从头到尾
             * 没有 DECFLAG_HW —— 于是上面三处在 Windows/Linux 的 FFmpeg 路上永远把"正在
             * 硬解"看成"软解"（avcodecDecoder 用的就是基类那份按位判定的语义，只是它自己
             * 从来不含这个位）。两份状态必须成对写：置真的两处（这里、getHwFormat()）、
             * 复位的四处（getHwFormat() 的运行期降级与拒绝协商、close_decoder()、下面
             * 没进硬解时的兜底）。
             */
            mFlags |= (int) DECFLAG_HW;
            mPDecoder->codecCont->thread_count = 1;
        } else if (!isAudio) {
            /*
             * 没进硬解（调用方没要求，或 initHwDecoder() 失败、本构建里没有该编码的硬解
             * 配置）：明确清掉这个位，别让上一次打开留下的状态骗过 getFlags()。
             */
            mFlags &= ~(int) DECFLAG_HW;
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

#if defined(__APPLE__)
        /*
         * Apple 平台默认走 framework/codec/Apple 的 AFVTBDecoder（自研
         * VideoToolbox 封装），这条 FFmpeg videotoolbox 路径只在应用显式打开
         * 全局设置时才启用 —— 为了零拷贝（拿到 CVPixelBuffer）需要它的应用，
         * 例如 platform/QtPlayer，会打开 "video.decoder.ffmpeg_videotoolbox"。
         * 开关的完整说明见 property.h。
         */
        if (globalSettings::getSetting().getProperty(PROPERTY_KEY_DECODER_FFMPEG_VT) != "ON") {
            return false;
        }
#endif

        /*
         * 下载出来的帧要不要保持 NV12，由全局设置决定（键名见 property.h）。
         * 默认关闭，也就是维持老的“转成 YUV420P 再交出去”；只有明确声明
         * 自己能吃 NV12 的呈现方才会打开。在 Windows 上 cmdline 的
         * onVideoRendering() 就是这样一个呈现方，它把开关打开后每帧能省掉
         * 一次 4K 全画幅的 swscale 转换。
         */
        mPDecoder->outputNv12 =
                (globalSettings::getSetting().getProperty(PROPERTY_KEY_HW_COPYBACK_NV12) == "ON");

        /*
         * 零拷贝直通：解码纹理不下载，上层自己用 D3D11 呈现。见 property.h。
         */
        mPDecoder->directOutput =
                (globalSettings::getSetting().getProperty(PROPERTY_KEY_HW_DIRECT_TEXTURE) == "ON");

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
        int ret = 0;

        /*
         * 先问平台集成方有没有"现成的设备"（Qt 会把 RHI 的 ID3D11Device 交过来）。
         *
         * 零拷贝的前提是解码纹理和呈现方在**同一个** GPU 设备上：D3D11 里两个不同
         * ID3D11Device 的纹理不能互相采样，Qt 的场景图也一样，只有同设备的纹理才能
         * 直接包成 QSGTexture。拿不到外部设备就退回自己创建一个（copy-back 仍然可用，
         * 只是没有零拷贝）。详见 framework/utils/hwDeviceBridge.h。
         */
#if defined(CICADA_HW_SUPPORTS_EXTERNAL_DEVICE)
        auto *externalDevice = static_cast<AVBufferRef *>(HwDeviceBridge::acquireExternalDevice());

        if (externalDevice != nullptr) {
            deviceRef = externalDevice;
        } else
#endif
        {
            ret = av_hwdevice_ctx_create(&deviceRef, CICADA_HW_DEVICE_TYPE, nullptr, nullptr, 0);
        }

        if (deviceRef == nullptr) {
            AF_LOGW("%s: av_hwdevice_ctx_create failed: %s, using the software decoder\n",
                    CICADA_HW_NAME, getErrorString(ret));
            return false;
        }

        mPDecoder->hwDeviceRef = deviceRef;
        mPDecoder->hwPixFmt = hwPixFmt;
        mPDecoder->hwDecodeActive = true;
        /*
         * 设备真的建起来了 ⇒ 这条流在用硬解。这是"当前解码方式"的对外读数
         * （见 isHardwareDecoderInUse()）：解码途中若 FFmpeg 把硬解格式摘掉，
         * getHwFormat() 会把它复位。
         */
        mHwDecodeInUse.store(true);

        // 3. Hand the device to the codec context and install the format
        //    negotiation callback. FFmpeg allocates the D3D11 surface pool for
        //    us once it knows coded_width/coded_height from the bitstream.
        mPDecoder->codecCont->hw_device_ctx = av_buffer_ref(deviceRef);
        mPDecoder->codecCont->opaque = mPDecoder;
        mPDecoder->codecCont->get_format = getHwFormat;

        AF_LOGI("%s: hardware decoding enabled for %s (hw pixel format %s)\n",
                CICADA_HW_NAME, mPDecoder->codec->name, av_get_pix_fmt_name(hwPixFmt));

        if (mPDecoder->outputNv12) {
            AF_LOGI("%s: the presenter takes NV12, the download is handed over "
                    "as is (no swscale conversion)\n", CICADA_HW_NAME);
        }

        if (mPDecoder->directOutput) {
            /*
             * 零拷贝直通：解码表面原样交给上层（Windows 上是 AF_PIX_FMT_D3D11，
             * macOS 上是 AV_PIX_FMT_VIDEOTOOLBOX，Linux 上是 AV_PIX_FMT_VAAPI）。
             * 上层拿到原生句柄后自己把它包成 GPU 纹理，全程不下载、不经过 CPU。
             *
             * **Windows 上千万不要动 BindFlags。** 曾经的写法是在这里补
             * D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE
             * （想让视频处理器的输入视图更好用），结果每一帧都失败：
             *
             *   [AVHWFramesContext] Could not create the texture (80070057)
             *   [h264] Failed setup for format d3d11: hwaccel initialisation returned error
             *
             * 80070057 是 E_INVALIDARG：D3D11 不允许解码器输出面同时当渲染目标
             * （dxva2.c 给解码表面只加 D3D11_BIND_DECODER 就是这个原因），
             * 而 hwcontext_d3d11va.c 的 d3d11va_frames_init() 会把这里的
             * BindFlags 并进每一张表面纹理，于是整池子纹理都建不出来，硬解直接
             * 废掉、一帧都出不来（表现是白屏 + 播放器退出）。
             *
             * 表面的用途由**呈现方**自己解决：Windows 上呈现方可以给视频处理器
             * 建一张自有的 RENDER_TARGET 中转纹理（GPU 内部拷一次，仍然不经过
             * CPU），macOS/Linux 上呈现方直接用 CVPixelBuffer / VAAPI surface。
             */
            AF_LOGI("%s: zero-copy direct output, decoded textures go straight to the "
                    "presenter (no download, no CPU copy)\n", CICADA_HW_NAME);
        }

        return true;
    }

    /*
     * Downloads a decoded GPU surface into system memory and returns the frame
     * to hand to the caller.
     *
     * The download always comes back as NV12: hwcontext_d3d11va.c only ever
     * reports ctx->sw_format from d3d11va_transfer_get_formats() and explicitly
     * refuses the opaque 420 layout that would map to YUV420P. What the caller
     * gets depends on who renders the frame:
     *
     *   - 4K60 硬解每帧要搬的内存是：GPU 下载 12.4MB (3840x2160 NV12)
     *     + swscale 转 YUV420P（读 12.4MB、写 12.4MB）+ 上传纹理 12.4MB，
     *     合计约 50MB/帧；60fps 就是 3GB/s。这是 4K 追不上实时、进而把
     *     播放器拖进死锁的根源，所以能省的一定要省。
     *   - 呈现方声明自己能直接吃 NV12 时（全局设置
     *     "video.render.hw.copyback_nv12" == "ON"，目前由 cmdline 的
     *     onVideoRendering() 用 SDL_PIXELFORMAT_NV12 纹理打开），NV12 直接
     *     交出去，一次全画幅转换就省掉了。
     *   - 否则保持原行为转成 YUV420P：SDL_UpdateYUVTexture() 要三个独立
     *     平面，而这里只能下载到 NV12，于是就地拆开色度平面。
     *
     * 解码本身（熵解码 + 运动补偿）始终在 GPU 上，这里只是下载 + 格式整理。
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

        if (mPDecoder->swFrames[mPDecoder->swFrameIndex] == nullptr) {
            mPDecoder->swFrames[mPDecoder->swFrameIndex] = av_frame_alloc();

            if (mPDecoder->swFrames[mPDecoder->swFrameIndex] == nullptr) {
                AF_LOGE("%s: cannot allocate the copy-back frame\n", CICADA_HW_NAME);
                return nullptr;
            }
        }

        /*
         * 轮转使用下载缓冲池（见 decoder_handle_v::swFrames）。
         *
         * 不做复用的话，4K 每帧都要 malloc 12.4MB、写的时候再缺页 3000 次、
         * 用完全部 munmap：每帧 1~3ms 白扔，60fps 下就是几个百分点的算力。
         *
         * 只有当这块缓冲已经没人持有时（av_buffer_is_writable() 为真，说明
         * 消费者已经上屏完、只剩池子自己这一份引用）才原地复用；否则必须
         * 重新分配，不然会把正在显示的帧改花。分辨率/像素格式变了同理。
         */
        AVFrame *swFrame = mPDecoder->swFrames[mPDecoder->swFrameIndex];
        mPDecoder->swFrameIndex = (mPDecoder->swFrameIndex + 1) % CICADA_HW_SW_FRAME_POOL;

        if (swFrame->buf[0] != nullptr &&
                (swFrame->format != mPDecoder->swPixFmt ||
                 swFrame->width != hwFrame->width || swFrame->height != hwFrame->height ||
                 !av_buffer_is_writable(swFrame->buf[0]))) {
            av_frame_unref(swFrame);
        } else if (swFrame->buf[0] != nullptr) {
            /*
             * 缓冲复用时元数据字典要单独清：av_frame_copy_props() 只覆盖同名字段，
             * 上一帧的 timePosition / utcTime 会残留下来，seek 时的丢帧判断
             * （NeedDrop(mSeekPos)）会因此用错时间戳。
             */
            av_dict_free(&swFrame->metadata);
        }

        // Ask for the layout the surface pool actually provides; requesting
        // anything else makes av_hwframe_transfer_data() fail.
        if (mPDecoder->swPixFmt != AV_PIX_FMT_NONE) {
            swFrame->format = mPDecoder->swPixFmt;
            swFrame->width = hwFrame->width;
            swFrame->height = hwFrame->height;
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

        /*
         * NV12 直接交给声明过自己能吃 NV12 的呈现方，跳过下面整段转换。
         * 4K 一帧就省掉 12.4MB 读 + 12.4MB 写，是这里最大的一笔开销。
         */
        if (mPDecoder->outputNv12 && swFrame->format == AV_PIX_FMT_NV12) {
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

        /*
         * 转换缓冲同样复用：只有没人持有、且尺寸没变时才原地写，否则重新
         * 分配（sws_scale 直接写进 out->data，所以必须在分配后保持不动）。
         */
        if (out->buf[0] != nullptr &&
                (out->format != AV_PIX_FMT_YUV420P ||
                 out->width != swFrame->width || out->height != swFrame->height ||
                 !av_buffer_is_writable(out->buf[0]))) {
            av_frame_unref(out);
        } else if (out->buf[0] != nullptr) {
            // 同下载缓冲：复用时清掉上一帧残留的 timePosition / utcTime
            av_dict_free(&out->metadata);
        }

        if (out->buf[0] == nullptr) {
            out->format = AV_PIX_FMT_YUV420P;
            out->width = swFrame->width;
            out->height = swFrame->height;

            if (av_frame_get_buffer(out, 0) < 0) {
                AF_LOGE("%s: cannot allocate the YUV420P conversion buffer\n", CICADA_HW_NAME);
                return nullptr;
            }
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
        /*
         * 反向指针（见 decoder_handle_v::owner）：静态回调 getHwFormat() 只能从
         * ctx->opaque 拿到本结构，而"硬解在不在用"的对外读数记在本对象上。
         * 必须在 memset **之后**赋值。
         */
        mPDecoder->owner = this;
#if defined(CICADA_HW_DEVICE_TYPE)
        mPDecoder->hwPixFmt = AV_PIX_FMT_NONE;
        mPDecoder->swPixFmt = AV_PIX_FMT_NONE;
#endif
        // avcodec_register_all was removed in FFmpeg 5.0 (decoders self-register).
        mFlags |= DECFLAG_PASSTHROUGH_INFO;
    }

    bool avcodecDecoder::isHardwareDecoderInUse()
    {
        /*
         * 只报**活动解码器**眼下走的那条路（写它的地方：initHwDecoder() 建成设备时置真，
         * getHwFormat() 的运行期降级、close_decoder() 复位）。构建里没有硬解配置、
         * 硬解设备建不出来时这个读数一直是假，也就是如实报"软解"。
         * 没有平台分支：本平台没有硬解后端时 initHwDecoder() 根本不会被调用。
         *
         * 同一个状态还**镜像**在 IDecoder::mFlags 的 DECFLAG_HW 位上（六处成对写，
         * 见 init_decoder() 的说明）：界面读这个方法，而
         * SMPAVDeviceManager::getVideoDecoderFlags() 读的是那一位，两者必须永远一致。
         */
        return mHwDecodeInUse.load();
    }

    avcodecDecoder::~avcodecDecoder()
    {
        close();

        /*
         * 【释放探针】解码器**对象**销毁的完成点（无条件打印，每个解码器实例一次：
         * 正常一轮退出 = 视频 1 次 + 音频 1 次）。
         *
         * 为什么还要这一条：close_decoder() 开头有 `if (mPDecoder == nullptr) return;`
         * 的早退分支，**早退时不打印** —— 所以日志里没有 `decoder closed` 只说明
         * "这一次没有活着的 codec context"，**不能**据此断定解码器没死。
         * 这一条放在析构里、无条件打印，才是"解码器对象（连同它的 hw frames 上下文与
         * 硬件表面池）确实死了"的判据。**没有这一行 = 有东西还在持有解码器。**
         */
        AF_LOGI("[mem] avcodecDecoder object destroyed (its codec context, hardware frames "
                "context and surface pool are gone with it)\n");
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

    bool avcodecDecoder::is_hardware_decode_supported(enum AFCodecID codec)
    {
#if defined(CICADA_HW_DEVICE_TYPE)
        /*
         * 与 initHwDecoder() 用**同一个** hasHwConfig() 判据（同一个
         * CICADA_HW_DEVICE_TYPE / CICADA_HW_PIX_FMT、同样拒绝 Windows 上只有
         * AV_PIX_FMT_D3D11VA_VLD 的 legacy hwaccel），所以这里说"有硬解配置"
         * 和真正打开解码器时说"能硬解"永远是同一个答案，不会各说各话。
         *
         * 只查配置，**不调 av_hwdevice_ctx_create()**：建 D3D11/VAAPI 设备有成本，
         * 而且会为一个不一定真正解码的编码白占 GPU 资源。"构建里有配置、设备实际
         * 解不了"由解码器创建时的 hw->sw 回退兜底（见 SuperMediaPlayer.cpp）。
         *
         * 只有视频编码会带本平台的硬解配置，音频/字幕编码自然返回 false。
         */
        const AVCodec *avCodec = avcodec_find_decoder(CodecID2AVCodecID(codec));

        if (avCodec == nullptr) {
            return false;
        }

        enum AVPixelFormat hwPixFmt = AV_PIX_FMT_NONE;
        const bool supported = hasHwConfig(avCodec, &hwPixFmt);
        /* 这里只要"有没有"，不关心是哪个像素格式；显式收掉避免"设了没用"的告警。 */
        (void) hwPixFmt;
        return supported;
#else
        /* 本平台没有 FFmpeg 硬解后端（未定义 CICADA_HW_DEVICE_TYPE）。 */
        (void) codec;
        return false;
#endif
    }

    void avcodecDecoder::flush_decoder()
    {
#if defined(CICADA_HW_DEVICE_TYPE)

        // Drop the in-flight copy-back buffers so a seek cannot hand the
        // renderer a frame from before the flush. Only this decoder's own
        // reference goes away; a frame already queued for rendering keeps its
        // own, and the pool slot is simply reallocated when it comes round again.
        for (AVFrame *&frame : mPDecoder->swFrames) {
            if (frame != nullptr) {
                av_frame_unref(frame);
            }
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
            if (mPDecoder->directOutput) {
                /*
                 * 零拷贝直通：GPU 纹理原样交出去，一个字节都不下载。
                 *
                 * 布局来自 FFmpeg hwcontext_d3d11va.c 的 d3d11va_get_buffer()：
                 *   data[0] = ID3D11Texture2D*，data[1] = 数组切片索引。
                 * 纹理的引用计数由 frame->buf[0]（AVD3D11FrameDescriptor）持有，
                 * 而下面用 AVAFFrame 克隆一次，所以只要这一帧还活着，纹理就有效，
                 * 解码器的表面池也回收不了它。
                 */
                outFrame = mPDecoder->avFrame;
            } else if (!mPDecoder->hwDecodeActive) {
                AF_LOGW("%s: dropping a hardware frame while hardware decoding is disabled\n",
                        CICADA_HW_NAME);
                return -EAGAIN;
            } else {
                outFrame = retrieveHwFrame(mPDecoder->avFrame);

                if (outFrame == nullptr) {
                    AF_LOGW("%s: dropping a hardware frame that could not be downloaded\n",
                            CICADA_HW_NAME);
                    return -EAGAIN;
                }
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
