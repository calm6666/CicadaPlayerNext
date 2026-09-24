//
// CicadaVideoRender —— 交给框架的"无窗口"视频渲染器。
//
// 为什么需要它
// ------------
// 框架里的视频渲染器是编译期定的（framework/render/renderFactory.cpp）：
//   * SdlAFVideoRender：自己建 SDL 窗口，还要 SDL_Renderer —— Qt 应用里不能有第二个
//     窗口系统；
//   * DummyVideoRender：AFActiveVideoRender 的 VSync 线程会按 pts 取帧，然后
//     deviceRenderFrame() 直接返回 true 把帧丢掉 —— 拿不到画面；
//   * GLRender：Android/OHOS 的 EGL 渲染器，要求有自己的 EGL surface。
//
// 所以 Qt 端注册一个自己的渲染器（framework/render/renderFactory.h 新加的
// setRenderCreator()）：它只做"节拍"这件事 —— 继承 AFActiveVideoRender，复用框架
// 现成的 VSync 线程、帧队列、按 pts 的节拍控制，然后重写 deviceRenderFrame()，
// 把帧转交给**应用注册的渲染回调**（MediaPlayer::SetVideoRenderingCallback）。
// 画面怎么画由 Qt 决定，这个类一行图形代码都没有。
//
// 线程模型（很重要）
// ------------------
//   * deviceRenderFrame() 跑在框架的 VSync 线程上（timedVSync，默认 60Hz，
//     可以用全局设置 "video.render.hz" 提到显示器刷新率）；
//   * 它只是把帧 clone() 一份交给 QML 组件，**绝对不能碰任何 Qt 的图形 API**
//     （QSGTexture / QRhi / 原生设备句柄都只能在 Qt 的渲染线程上用）；
//   * QML 组件收到帧后调用 QQuickItem::update()，由 Qt 的渲染线程在
//     updatePaintNode() 里把帧变成 GPU 纹理并交给场景图。
//
// 截屏（device_captureScreen）也是这个类负责的
// --------------------------------------------
// 框架的应用层 API 是 MediaPlayer::CaptureScreen()（mediaPlayer/MediaPlayer.cpp:380）
//   -> SuperMediaPlayer::CaptureScreen()（mediaPlayer/SuperMediaPlayer.cpp:202）
//   -> IVideoRender::captureScreen()（framework/render/video/IVideoRender.h:174）
//   -> AFActiveVideoRender 在 VSync 线程上回调 device_captureScreen()（:88-93）
//   -> 这里 -> 应用注册的 listener CaptureScreen(width, height, buffer)。
// 基类 AFActiveVideoRender.h:53 的那个空实现就是整条链上唯一的缺口（回调永远不来，
// 于是"截屏"什么都不会发生），本类把它补上：像素怎么拿、缓冲区归谁、硬解怎么办，
// 全部写在 .cpp 里（**那段注释是实现契约，改代码前先读它**）。
//
#ifndef CICADA_QT_CICADAVIDEORENDER_H
#define CICADA_QT_CICADAVIDEORENDER_H

#include <render/video/AFActiveVideoRender.h>
#include <render/renderFactory.h>
#include <functional>
#include <memory>
#include <mutex>

namespace cicadaqt {

    /*
     * 供框架调用的工厂函数（注册给 videoRenderFactory::setRenderCreator）。
     * 返回 nullptr 时框架会退回它编译期选出来的渲染器。
     */
    std::unique_ptr<IVideoRender> createCicadaVideoRender();

    /* 把当前的视频渲染回调转交给 QML 组件的那个渲染器实现。 */
    class CicadaVideoRender : public AFActiveVideoRender {
    public:
        CicadaVideoRender();

        ~CicadaVideoRender() override;

        /* ---- IVideoRender：Qt 端不需要做的部分，全部空实现 ---- */

        int init() override
        {
            return 0;
        }

        int clearScreen() override
        {
            // 画面是 Qt 场景图里的一个节点，清屏由 QQuickWindow 自己管。
            /*
             * 顺手把截屏缓存的那一帧放掉：框架在两处会调 clearScreen() ——
             * SuperMediaPlayer::Stop()（"停了就别留着画面"，SuperMediaPlayer.cpp:458）
             * 和 SMPAVDeviceManager::createVideoRender() 换渲染参数时
             * （SMPAVDeviceManager.cpp:353）。这两个时刻上一帧已经不该再被截出来了，
             * 而且硬解帧的克隆会钉住解码器表面池里的一张 surface（见 .cpp 里
             * "为什么要缓存"那段），早点放掉更干净。
             *
             * 加锁是因为这个函数可能跑在播放器线程上，而 deviceRenderFrame /
             * device_captureScreen 跑在 VSync 线程上。
             */
            std::lock_guard<std::mutex> lock(m_lastFrameMutex);
            m_lastFrame.reset();
            return 0;
        }

        void setBackgroundColor(uint32_t color) override
        {
            (void) color;
        }

        int setRotate(Rotate rotate) override
        {
            /*
             * 旋转/镜像/缩放都交给 QML 组件处理（它知道画布尺寸，做等比适配更准），
             * 这里只记下来，不参与绘制。
             */
            m_rotate = rotate;
            return 0;
        }

        int setFlip(Flip flip) override
        {
            m_flip = flip;
            return 0;
        }

        int setScale(Scale scale) override
        {
            m_scale = scale;
            return 0;
        }

        /*
         * 返回 0 而不是 FLAG_DUMMY：
         *   * FLAG_DUMMY 会让框架把 HDR 视频/DRM 之类判成"不需要渲染"，把帧丢掉；
         *   * 我们确实"渲染"了（交给 Qt），所以不能声明自己是 dummy。
         */
        uint64_t getFlags() override
        {
            return m_hdr ? FLAG_HDR : 0;
        }

        /* 应用（QML 组件）可以直接调用，用来告诉渲染器当前内容的色彩特性。 */
        void setHdrContent(bool hdr)
        {
            m_hdr = hdr;
        }

    protected:
        /*
         * AFActiveVideoRender 的 VSync 线程按节拍决定"这一帧该显示了"，就调到这里。
         * 我们把帧交给应用的回调即可。
         *
         * 返回值语义：true = 这一帧由我们处理掉了（框架不再管它）。
         */
        bool deviceRenderFrame(IAFFrame *frame) override;

        /*
         * 截屏：把"当前正在显示的那一帧"变成 4 字节/像素的像素缓冲交给框架。
         *
         * 基类（AFActiveVideoRender.h:53）是个空实现，这就是原来 CaptureScreen()
         * 拿不到任何像素的原因。**缓冲区归本类分配、也归本类释放**：回调返回之后
         * 立刻 free()，和框架自带的 GLRender 一模一样
         * （framework/render/video/glRender/GLRender.cpp:455-465），调用方要留就自己拷
         * （PlayerNotifier 正是这么做的，见 player_notifier.cpp:199-200）。
         * 完整约定（像素格式、硬解回读、失败时的行为）写在 .cpp 里。
         */
        void device_captureScreen(std::function<void(uint8_t *, int, int)> func) override;

    private:
        Rotate m_rotate = Rotate_None;
        Flip m_flip = Flip_None;
        Scale m_scale = Scale_AspectFit;
        bool m_hdr = false;
        /* 只打一次"已经接通 Qt"的日志，免得每帧刷屏。 */
        bool m_loggedFirstFrame = false;

        /*
         * "当前正在显示的那一帧"的克隆，专给截屏用。
         *
         * 为什么需要它：AFActiveVideoRender 在 deviceRenderFrame() 返回之后立刻
         * 销毁那一帧（AFActiveVideoRender.cpp:152），而截屏请求到达时
         * （device_captureScreen 在 onVSync 的**开头**被调，:88-93）手上只有
         * "上一帧"——所以必须在渲染时留一份引用。clone() 不拷像素
         * （AVAFFrame::clone -> av_frame_clone 只是引用计数 +1，
         * framework/base/media/AVAFPacket.cpp:284-287），每帧的代价就是一次
         * shared_ptr 赋值。
         *
         * 用 shared_ptr + 互斥锁：device_captureScreen 拿一份引用后就松锁干活，
         * 即使 clearScreen()（播放器线程）同时把缓存清掉，手上那份也还是有效的。
         */
        std::shared_ptr<IAFFrame> m_lastFrame;
        std::mutex m_lastFrameMutex;
        /* 只打一次"这一帧截不了，原因是什么"，免得每次截屏都刷屏。 */
        bool m_loggedCaptureFailure = false;
    };

}// namespace cicadaqt

#endif// CICADA_QT_CICADAVIDEORENDER_H
