//
// CicadaVideoRender 的实现，设计说明见 CicadaVideoRender.h。
//

#define LOG_TAG "CicadaVideoRender"

#include "CicadaVideoRender.h"

#include <utils/frame_work_log.h>
#include <utils/CicadaJSON.h>
/* 节拍频率来自全局设置 "video.render.hz"。 */
#include <utils/globalSettings.h>
#include <utils/property.h>
/* PRId64：日志里打 pts 用。 */
#include <cinttypes>
#include <cstdlib>
#include <string>

/*
 * 截屏用的框架/FFmpeg 类型：
 *   * IAFrame / AVAFFrame / getAVFrame()  —— framework/base/media/AVAFPacket.h。
 *     "这帧到底是软解出来的 CPU 像素，还是硬解的 GPU 表面"就看它：
 *     AVAFFrame 里包着 AVFrame，而 AF_PIX_FMT_D3D11 这类硬解帧的 data[0] 是
 *     ID3D11Texture2D*，不是像素（见 framework/utils/AFMediaType.h:184-197）。
 *   * AVHWFramesContext / av_hwframe_transfer_data() —— libavutil/hwcontext.h。
 *     硬解帧的 GPU->CPU 回读就用 FFmpeg 这一句，它内部已经是"D3D11 staging 纹理 +
 *     Map(READ)"（external/external/ffmpeg/libavutil/hwcontext_d3d11va.c:389-412、465-480），
 *     我们一行 D3D 代码都不用写；VAAPI / VideoToolbox 走的是同一个入口。
 */
#include "base/media/AVAFPacket.h"

#if defined(__APPLE__)
/* macOS 的 CVPixelBuffer 帧（PBAFFrame）要转成 CPU 帧才能截，见下面 captureScreen。 */
#include "base/media/PBAFFrame.h"
#endif

extern "C" {
/* swscale：把拿到的 CPU 平面转成回调约定的 4 字节/像素 RGBA。同一个库在
 * CicadaVideoTexture 的 CPU 回退路径上已经在用了（那份是转 BGRA 给 QImage）。 */
#include <libswscale/swscale.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

using namespace Cicada;

namespace cicadaqt {

    /*
     * 从全局设置里读节拍频率（"video.render.hz"，单位 Hz）。
     *
     * 框架自带的 SdlAFVideoRender 读的就是这个键（见 SdlAFVideoRender.cpp:31），
     * cmdline 在启动时把显示器刷新率写进去。基类 AFActiveVideoRender 默认 60Hz：
     *   * 4K60 这类片源 60 就够；高刷屏（120/144Hz）上把它提上去，帧的交接更贴合
     *     显示器的节奏，画面更顺；
     *   * 没设或者值不合理（写成 0、写错）就退回 60，免得节拍乱掉。
     */
    static float renderHzFromSettings()
    {
        const std::string &value =
            Cicada::globalSettings::getSetting().getProperty(PROPERTY_KEY_VIDEO_RENDER_HZ);

        if (value.empty()) {
            return 60.0f;
        }

        const float hz = static_cast<float>(atof(value.c_str()));

        if (hz < 24.0f || hz > 480.0f) {
            AF_LOGW("ignoring the bogus video.render.hz value \"%s\", using 60Hz\n",
                    value.c_str());
            return 60.0f;
        }

        return hz;
    }

    /*
     * 见头文件 setDecoderGenerationEndedHook()：进程内单槽（同一时刻只有一个播放器
     * 窗口）。用可变函数 + 互斥保护，因为"注册/注销"在 GUI 线程发生，而
     * releaseFrames() 可能跑在播放器线程或 VSync 线程上。
     */
    static std::function<void()> g_decoderGenerationEndedHook{};
    static std::mutex g_decoderGenerationEndedHookMutex{};

    CicadaVideoRender::CicadaVideoRender()
        : AFActiveVideoRender(renderHzFromSettings())
    {
        /*
         * 基类负责建 VSync 线程（按上面那个频率）、帧队列和按 pts 的节拍控制；
         * 我们重写两个 virtual：deviceRenderFrame()（把"该显示的帧"转交给 Qt 组件）
         * 和 device_captureScreen()（截屏）。画面怎么画全由 Qt 决定，这个类一行
         * 图形代码都没有。
         */
        AF_LOGD("CicadaVideoRender created (Qt owns the drawing), VSync %.2f Hz\n",
                (double) renderHzFromSettings());
    }

    CicadaVideoRender::~CicadaVideoRender()
    {
        /*
         * 注销"解码代际结束"的通知：本类可能比 QML 组件活得久（析构顺序不受我们控制），
         * 留着一个指向已死组件的回调就是在赌运气。
         */
        setDecoderGenerationEndedHook(nullptr);
        AF_LOGD("CicadaVideoRender destroyed\n");
    }

    void CicadaVideoRender::releaseFrames()
    {
        /*
         * 第 0 步：基类先把 VSync 线程停下来，把 mInputQueue 里排队的帧和
         * mRendingFrame 逐帧放掉（那一步是同步的：afThread::pause 会等到安全点）。
         */
        AFActiveVideoRender::releaseFrames();

        /* 第 1 步：截屏缓存的那一帧。本函数的契约是"返回时本渲染器不再持有任何解码帧"。 */
        {
            std::lock_guard<std::mutex> lock(m_lastFrameMutex);
            m_lastFrame.reset();
        }

        /*
         * 第 2 步：通知 QML 组件"这一代解码器结束了"。
         *
         * 为什么还要通知它：第 0/1 步之后，播放器/渲染器这一侧已经没有解码帧了，
         * 但组件那边还有两个持有者 —— m_currentFrame（正在显示的那一帧的克隆）与
         * D3D11 的输入视图（对解码纹理持有 D3D11 引用）。而那两个只能在 Qt 的
         * **渲染线程**上放（它们住在场景图的 RHI 设备那边），所以这里只发通知：
         * 组件收到后置一个原子标志 + 请求重绘，真正的"放视图 + Flush 立即上下文"
         * 在 updatePaintNode() 里做（见 CicadaPlayerItem 里那一处）。
         *
         * 回调先拷贝出来再调，绝不持 g_decoderGenerationEndedHookMutex ——
         * 回调里会去碰 QML 组件。
         */
        std::function<void()> hook;

        {
            std::lock_guard<std::mutex> lock(g_decoderGenerationEndedHookMutex);
            hook = g_decoderGenerationEndedHook;
        }

        if (hook) {
            hook();
        }
    }

    void setDecoderGenerationEndedHook(std::function<void()> hook)
    {
        std::lock_guard<std::mutex> lock(g_decoderGenerationEndedHookMutex);
        g_decoderGenerationEndedHook = std::move(hook);
    }

    bool CicadaVideoRender::deviceRenderFrame(IAFFrame *frame)
    {
        if (frame == nullptr) {
            return true;
        }

        /*
         * mRenderingCb 就是应用通过 MediaPlayer::SetVideoRenderingCallback() 注册的
         * 回调（QML 组件在构造播放器时注册）。它跑在**这条 VSync 线程**上，所以
         * 回调里只允许把帧交给 UI 线程，不允许做任何绘制。
         *
         * 回调返回 true 表示"这一帧我接了"；返回 false 或者压根没注册回调时，
         * 我们仍然返回 true —— 帧已经没人要了，丢掉它比让它烂在队列里好。
         */
        if (mRenderingCb != nullptr) {
            CicadaJSONItem params{};
            mRenderingCb(mRenderingCbUserData, frame, params);
        } else if (!m_loggedFirstFrame) {
            AF_LOGW("no video rendering callback registered: decoded frames are dropped "
                    "(did the QML item forget MediaPlayer::SetVideoRenderingCallback?)\n");
        }

        if (!m_loggedFirstFrame) {
            m_loggedFirstFrame = true;
            AF_LOGI("first frame handed over to the Qt item (pts %" PRId64 ", format %d)\n",
                    frame->getInfo().pts, (int) frame->getInfo().video.format);
        }

        /*
         * 留一份克隆给截屏用（"当前正在显示的那一帧"，见 device_captureScreen 的长注释）。
         *
         * 为什么必须在这里留、而不是截屏时再去要：
         *   * 框架在 deviceRenderFrame() 返回后立刻销毁这一帧
         *     （AFActiveVideoRender.cpp:152 的 mRendingFrame = nullptr）；
         *   * 截屏请求是在 onVSync 的**开头**被处理的（AFActiveVideoRender.cpp:88-93），
         *     那一刻本节拍的新帧还没进来，能截的只有"上一帧"——所以只能由渲染时留一份。
         *
         * clone() 的开销：AVAFFrame::clone() 是 av_frame_clone（引用计数 +1，
         * 不拷像素，framework/base/media/AVAFPacket.cpp:284-287），软解帧/硬解帧都适用；
         * 代价只有一次 shared_ptr 赋值 + 加锁。TextureFrame（Android/OHOS 的 GL 纹理
         * 零拷贝帧）的 clone() 返回 nullptr（framework/base/media/TextureFrame.h:37-40），
         * 这时缓存会变空，截屏会按"截不了"处理（有日志），不会崩。
         */
        {
            std::shared_ptr<IAFFrame> keep(frame->clone().release());

            std::lock_guard<std::mutex> lock(m_lastFrameMutex);
            m_lastFrame = std::move(keep);
        }

        return true;
    }

    /*
     * 这个 AF_PIX_FMT_* 里装的是 GPU 对象（纹理指针 / surface ID），不是像素。
     * 见 framework/utils/AFMediaType.h:184-197。**绝不能把它们交给 swscale**
     * ——那是拿句柄当地址解引用，轻则满屏花屏重则崩（同样的判断在
     * CicadaVideoTexture.cpp:212-226 的 CPU 回退里也有一次，理由一样）。
     */
    static bool isGpuOnlyPixelFormat(int format)
    {
        switch (format) {
            case AF_PIX_FMT_D3D11:
            case AF_PIX_FMT_DXVA2_VLD:
            case AF_PIX_FMT_VAAPI:
            case AF_PIX_FMT_APPLE_PIXEL_BUFFER:
            case AF_PIX_FMT_CICADA_MEDIA_CODEC:
            case AF_PIX_FMT_CICADA_TEXTURE:
                return true;

            default:
                return false;
        }
    }

    /*
     * 把硬解帧（GPU 表面）下载成 CPU 可读的一帧。
     *
     * 为什么用 av_hwframe_transfer_data()，而不是自己写 D3D11：
     *   1. 框架自己就是这么下载硬解帧的 —— framework/codec/avcodecDecoder.cpp:508
     *      的 retrieveHwFrame()，核心一句就是 :576 的
     *      av_hwframe_transfer_data(swFrame, hwFrame, 0)（硬解 copy-back 那条路每个
     *      解码帧都走它，是已经在跑的代码，不是新方案）；
     *   2. 对 D3D11VA 来说这一句内部就是标准的 GPU->CPU 回读：
     *      CopySubresourceRegion 到一张 D3D11_USAGE_STAGING 纹理 + Map(D3D11_MAP_READ)
     *      （external/external/ffmpeg/libavutil/hwcontext_d3d11va.c:389-412 建 staging、
     *      :465-480 拷贝并 Map），**这套 D3D 代码 FFmpeg 里已经有了**，我们再写一遍
     *      只会多出一堆容易出错的资源管理；
     *   3. 它是跨平台的：VAAPI（Linux）、VideoToolbox（macOS）走的是同一个入口，
     *      所以 QtPlayer 的三端渲染器共用这一段，不需要按平台分叉。
     *
     * 线程安全：下载用的是帧自带 hw_frames_ctx 所属设备的立即上下文，FFmpeg 用
     * device_hwctx->lock 把 staging 纹理和上下文调用串起来
     * （hwcontext_d3d11va.c:452、:499；这个锁在 device_init 里默认装好，:513-521），
     * 而 Qt 借给解码器的设备又开了 SetMultithreadProtected(TRUE)
     * （platform/QtPlayer/src/CicadaHardwareDevice.cpp:255-261），所以在 VSync 线程上
     * 调用是安全的（和 FFmpeg 解码线程并发使用同一个立即上下文也是允许的）。
     *
     * 调用方负责 av_frame_free() 返回值。
     */
    static AVFrame *downloadHardwareFrame(const AVFrame *hwFrame)
    {
        auto *framesCtx = reinterpret_cast<AVHWFramesContext *>(
                hwFrame->hw_frames_ctx ? hwFrame->hw_frames_ctx->data : nullptr);

        if (framesCtx == nullptr) {
            /*
             * 硬解帧没带池子信息就没法下载。框架在同一个坑上也有过告警
             * （avcodecDecoder.cpp:519-523），照抄同样的态度：宁可截不出图，
             * 也不要把 data[0] 里的纹理指针当像素用。
             */
            AF_LOGW("captureScreen: the decoded hardware frame carries no hw_frames_ctx, "
                    "so it cannot be downloaded to CPU pixels\n");
            return nullptr;
        }

        AVFrame *swFrame = av_frame_alloc();

        if (swFrame == nullptr) {
            return nullptr;
        }

        /*
         * 目标帧的 format 必须是池子的 sw_format（D3D11 上是 NV12，10bit HDR 是 P010），
         * 请求别的格式 av_hwframe_transfer_data() 直接 EINVAL
         * （hwcontext_d3d11va.c:449 就检查这一条）。宽高只是声明，真正的缓冲由
         * transfer_data_alloc() 按池子尺寸分配（libavutil/hwcontext.c:427-441），
         * 下载完 width/height 会被改成源帧的（也就是裁剪后的显示尺寸）。
         */
        swFrame->format = framesCtx->sw_format;
        swFrame->width = hwFrame->width;
        swFrame->height = hwFrame->height;

        const int ret = av_hwframe_transfer_data(swFrame, hwFrame, 0);

        if (ret < 0) {
            /* 这里不引 av_err2str（它在 C++ 下是复合字面量，MSVC 不认），报错误码足够定位。 */
            AF_LOGW("captureScreen: cannot download the hardware surface "
                    "(format %s -> %s): error %d\n",
                    av_get_pix_fmt_name(static_cast<AVPixelFormat>(hwFrame->format)),
                    av_get_pix_fmt_name(framesCtx->sw_format), ret);
            av_frame_free(&swFrame);
            return nullptr;
        }

        return swFrame;
    }

    /*
     * ===========================================================================
     * 截屏：把"当前正在显示的那一帧"变成像素，按框架的回调约定交出去。
     *
     * 【回调约定 —— 这是照别的平台抄的，不是自己发明的】
     *
     *   func(data, width, height)，数据布局 = **width * height * 4 字节，像素顺序
     *   R,G,B,A**（等于 AV_PIX_FMT_RGBA）。证据：
     *     * framework/render/video/glRender/GLRender.cpp:455-463 —— 框架自带的实现：
     *         GLsizei bufferSize = width * height * sizeof(GLubyte) * 4; //RGBA
     *         ... glReadPixels(..., GL_RGBA, GL_UNSIGNED_BYTE, bufferData);
     *         mCaptureFunc(bufferData, width, height);
     *     * mediaPlayer/player_notifier.cpp:199-200 —— 消费端按 width*height*4 做 memcpy，
     *       一个像素 4 字节是被硬编码在框架里的；
     *     * platform/Android/.../NativePlayerBase.java:1072-1074 —— Bitmap.Config.ARGB_8888
     *       + copyPixelsFromBuffer，内存里就是 R,G,B,A。
     *   注意内存排列，不是"ARGB 整数"：Android 的 ARGB_8888 在内存里也是 RGBA 字节序。
     *
     *   **buffer 归本类（生产者）所有，只保证在 func() 调用期间有效，func() 返回后
     *   立刻 free()** —— GLRender.cpp:455-465 就是"malloc -> func() -> free"。
     *   消费链（SuperMediaPlayer.cpp:207-211 -> player_notifier.cpp:188-203）会把像素
     *   拷进自己的缓冲再投递事件，并且**不会**释放我们这块（它 free 的是自己 malloc
     *   的那份，player_notifier.cpp:141-150），所以这里必须自己释放，也不能指望
     *   "框架会接手"。调用方要留下来的话必须自己拷贝。
     *
     * 【像素从哪来 —— 三种帧】
     *
     *   1. 软解帧 / 硬解 copy-back 帧：data[0..2] 就是 CPU 平面，直接 swscale 转。
     *      （硬解 copy-back 时解码器已经把 NV12 下载回内存了，见 avcodecDecoder.cpp:576。）
     *   2. 硬解零拷贝帧（Windows：AF_PIX_FMT_D3D11，data[0] = ID3D11Texture2D*、
     *      data[1] = 数组切片号，见 CicadaTextureD3D11.cpp:515-521）：**先做 GPU->CPU
     *      回读**（downloadHardwareFrame()，即 FFmpeg 的 staging 纹理 + Map），再 swscale。
     *      也就是说：硬解开着的时候截屏照样能拿到图，硬件解码本身不降级
     *      （解码仍然在 GPU 上，我们只是把"要截的那一帧"下载回来）。
     *   3. 拿不到像素的帧（TextureFrame：Android/OHOS 的 GL 纹理零拷贝，
     *      getData() 返回 nullptr，TextureFrame.h:22-25；或者硬解帧但缺 hw_frames_ctx）：
     *      打一条 AF_LOGW 说明原因，然后按框架约定的"没有画面"回调 func(nullptr, 0, 0)
     *      （IVideoRender.h:174-177 的默认实现、SuperMediaPlayer.cpp:212-215 都是这么
     *      表达的），绝不崩、也不把 GPU 句柄当像素喂给 swscale。
     *
     * 【为什么不用 Qt 场景图那条路回读】
     *
     * 屏幕上的 RGBA 纹理在 Qt 渲染线程上（CicadaTextureD3D11::m_outputTexture），
     * 从那里回读能拿到"和显示完全一致"的一帧（含 HDR 色调映射），但：
     *   * 视频处理器输出纹理只有渲染线程能碰，要做"VSync 线程 -> 渲染线程 -> 回调"
     *     的跨类跨线程管道（渲染器拿不到 CicadaPlayerItem 的指针）；
     *   * **暂停时 Qt 不再重绘**，那条路会永远不回调 —— 而"暂停后截图"恰恰是最常见的
     *     用法；
     *   * 而且零拷贝后端一旦运行时失败退回 CPU 路径（CicadaVideoTexture.cpp:170-190），
     *     屏幕上那帧的纹理就没了。
     * 所以这里走"从解码帧自己想办法"，任何播放状态下都能出图。代价：HDR(P010) 片源
     * 的截图是源色彩空间的（没有做 HDR->SDR 色调映射，可能比屏幕上暗/灰一点），
     * SDR 片源则和画面一致。
     *
     * 【线程】
     *
     * 本函数跑在框架的 VSync 线程上（AFActiveVideoRender.cpp:88-93）。一次截屏要做
     * 一次 GPU 回读 + swscale（4K 大约十几毫秒），是一次性的、不常发生，所以直接同步
     * 做完即可（GLRender 也是在渲染循环里同步 glReadPixels 的）；期间可能掉一帧，
     * 不影响后续播放。
     * ===========================================================================
     */
    void CicadaVideoRender::device_captureScreen(std::function<void(uint8_t *, int, int)> func)
    {
        if (func == nullptr) {
            /* 防御：基类的 captureScreen() 允许传空（AFActiveVideoRender.cpp:159-163）。 */
            return;
        }

        /* 1) 取"当前正在显示的那一帧"。取到的是 shared_ptr，松锁之后它仍然有效。 */
        std::shared_ptr<IAFFrame> frame;

        {
            std::lock_guard<std::mutex> lock(m_lastFrameMutex);
            frame = m_lastFrame;
        }

        if (frame == nullptr) {
            AF_LOGW("captureScreen: no frame is on screen yet (nothing decoded yet, or the "
                    "frame was one that cannot be kept alive), reporting an empty picture\n");
            func(nullptr, 0, 0);
            return;
        }

        /*
         * 2) 尺寸用帧信息里的 video.width/height：它是**裁剪后的真实画面尺寸**
         *    （例如 3840x2160），而硬解纹理是编码对齐的（3840x2176）。同一个取值口径
         *    见 CicadaTextureD3D11.cpp:539-548。
         */
        const IAFFrame::AFFrameInfo &info = frame->getInfo();
        const int width = info.video.width;
        const int height = info.video.height;

        if (width <= 0 || height <= 0) {
            AF_LOGW("captureScreen: the frame has no valid size (%dx%d)\n", width, height);
            func(nullptr, 0, 0);
            return;
        }

        /* 3) 找到 CPU 可读的平面。 */
        AVFrame *hwFrame = getAVFrame(frame.get());
        /* 硬解帧下载出来的那一帧（我们自己分配，最后自己释放）。 */
        AVFrame *swFrame = nullptr;
#if defined(__APPLE__)
        /*
         * macOS 的 CVPixelBuffer 帧转换出来的 CPU 帧（见下面 3c）。必须活到 sws_scale
         * 之后，所以声明在这里，而不是 3c 的块里。
         */
        std::unique_ptr<IAFFrame> appleConverted;
#endif
        uint8_t **planes = frame->getData();
        int *lineSizes = frame->getLineSize();
        AVPixelFormat sourceFormat = static_cast<AVPixelFormat>(info.video.format);

        if (hwFrame != nullptr && hwFrame->hw_frames_ctx != nullptr) {
            /* 3a) 硬解零拷贝帧：GPU->CPU 回读，这一条是"硬解也能截图"的关键。 */
            swFrame = downloadHardwareFrame(hwFrame);

            if (swFrame == nullptr) {
                if (!m_loggedCaptureFailure) {
                    m_loggedCaptureFailure = true;
                    AF_LOGW("captureScreen: the hardware frame could not be downloaded to CPU "
                            "pixels, so this snapshot is empty (playback is unaffected)\n");
                }

                func(nullptr, 0, 0);
                return;
            }

            planes = swFrame->data;
            lineSizes = swFrame->linesize;
            sourceFormat = static_cast<AVPixelFormat>(swFrame->format);
        } else if (isGpuOnlyPixelFormat(info.video.format)) {
            /*
             * 3b) 是 GPU 原生帧但没有池子信息（正常不该发生）：**绝不能**把 data[0]
             *     当像素交给 swscale，宁可给一张空图。
             */
            AF_LOGW("captureScreen: the frame is a raw GPU frame (format %d) without "
                    "hw_frames_ctx; refusing to treat the surface handle as pixels\n",
                    info.video.format);
            func(nullptr, 0, 0);
            return;
        } else if (planes == nullptr || lineSizes == nullptr || planes[0] == nullptr) {
            /*
             * 3c) 没有 CPU 平面：目前只有 TextureFrame（Android/OHOS 的 GL 渲染器把解码
             *     纹理直接交上来，getData() 返回 nullptr，TextureFrame.h:22-25；它的
             *     clone() 也返回 nullptr，所以连缓存都留不下）。要在这种帧上截图必须回到
             *     产生它的那个 GL 上下文里读回纹理，这个渲染器没有那个上下文 —— 所以
             *     明确说清楚原因并交空图，绝不把句柄当像素用。
             *
             * macOS 的 CVPixelBuffer 帧（PBAFFrame）是例外：它 getData() 给的是
             * CVPixelBuffer 指针（PBAFFrame.h:28-32），不能直接喂 swscale；但框架给了
             * "CVPixelBuffer -> CPU 帧"的转换运算符，CPU 回退路径用的就是它
             * （CicadaVideoTexture.cpp:235-254），这里照做。
             */
#if defined(__APPLE__)
            if (info.video.format == AF_PIX_FMT_APPLE_PIXEL_BUFFER) {
                if (auto *pbFrame = dynamic_cast<PBAFFrame *>(frame.get())) {
                    /* 这个转换运算符内部是 CVPixelBufferLockBaseAddress + av_image_copy，
                     * 结果是普通 CPU 帧（NV12/YUV420P）；new 出来的对象交给我们接管。 */
                    appleConverted.reset(static_cast<AVAFFrame *>(*pbFrame));
                }

                if (appleConverted != nullptr) {
                    planes = appleConverted->getData();
                    lineSizes = appleConverted->getLineSize();
                    /* 注意：像素格式要用**转换后**那一帧的，不是 AF_PIX_FMT_APPLE_PIXEL_BUFFER。 */
                    sourceFormat = static_cast<AVPixelFormat>(
                            appleConverted->getInfo().video.format);
                }
            }
#endif
            if (planes == nullptr || lineSizes == nullptr || planes[0] == nullptr) {
                if (!m_loggedCaptureFailure) {
                    m_loggedCaptureFailure = true;
                    AF_LOGW("captureScreen: this frame has no CPU readable pixels "
                            "(format %d); a zero-copy texture frame owned by a GL/Metal "
                            "context cannot be read back on the CPU\n", info.video.format);
                }

                func(nullptr, 0, 0);
                return;
            }
        }

        /*
         * 4) 转成回调约定的 4 字节/像素 RGBA。
         *
         *    源尺寸：硬解下载帧用**它自己的** width/height —— 下载完这两个值已经被改成
         *    源帧的尺寸（libavutil/hwcontext.c:438-441），正好是我们要的那块画面（左上角
         *    对齐，右下角是编码对齐的填充，不用它）；CPU 帧则等于帧信息里的显示尺寸。
         *    这里每次截屏现建一个 swscale 上下文（sws_getCachedContext(nullptr,...)），
         *    用完就放：截屏是低频操作，而缓存上下文就要考虑线程安全，不值当。
         */
        int sourceWidth = width;
        int sourceHeight = height;

        if (swFrame != nullptr) {
            sourceWidth = swFrame->width;
            sourceHeight = swFrame->height;
        }
#if defined(__APPLE__)
        else if (appleConverted != nullptr) {
            sourceWidth = appleConverted->getInfo().video.width;
            sourceHeight = appleConverted->getInfo().video.height;
        }
#endif

        if (sourceWidth <= 0 || sourceHeight <= 0) {
            /* 不该发生；真发生了就按显示尺寸读，总比拿个负数去建上下文好。 */
            sourceWidth = width;
            sourceHeight = height;
        }

        SwsContext *sws = sws_getCachedContext(nullptr, sourceWidth, sourceHeight, sourceFormat,
                                               width, height, AV_PIX_FMT_RGBA,
                                               SWS_BILINEAR, nullptr, nullptr, nullptr);

        if (sws == nullptr) {
            AF_LOGW("captureScreen: swscale cannot convert %s -> RGBA\n",
                    av_get_pix_fmt_name(sourceFormat));

            if (swFrame != nullptr) {
                av_frame_free(&swFrame);
            }

            func(nullptr, 0, 0);
            return;
        }

        const size_t bufferSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        /* malloc（不是 new[]）：这个缓冲要按框架约定交给外部，malloc/free 是它的惯例。 */
        auto *buffer = static_cast<uint8_t *>(malloc(bufferSize));

        if (buffer == nullptr) {
            AF_LOGE("captureScreen: cannot allocate the snapshot buffer (%dx%d)\n",
                    width, height);
            sws_freeContext(sws);

            if (swFrame != nullptr) {
                av_frame_free(&swFrame);
            }

            func(nullptr, 0, 0);
            return;
        }

        uint8_t *dstData[4] = {buffer, nullptr, nullptr, nullptr};
        /* 目标按行紧排（width*4），QImage/位图那边直接按这个跨度读最省事。 */
        int dstLineSize[4] = {width * 4, 0, 0, 0};

        sws_scale(sws, planes, lineSizes, 0, sourceHeight, dstData, dstLineSize);
        sws_freeContext(sws);

        if (swFrame != nullptr) {
            av_frame_free(&swFrame);
        }

        AF_LOGI("captureScreen: delivering a %dx%d RGBA snapshot\n", width, height);

        /*
         * 5) 交付：buffer 只在这次调用期间有效，回调返回后立刻释放
         *    （消费端会自己拷贝，见函数头部的约定）。
         */
        func(buffer, width, height);
        free(buffer);
    }

    std::unique_ptr<IVideoRender> createCicadaVideoRender()
    {
        return std::unique_ptr<IVideoRender>(new CicadaVideoRender());
    }

}// namespace cicadaqt
