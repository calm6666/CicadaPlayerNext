//
// macOS 零拷贝后端：VideoToolbox 的 CVPixelBuffer -> MTLTexture -> Qt 场景图。
//
// 这条路为什么比"再转一道"更好
// ---------------------------
// 框架在 Apple 上默认就用自研的 AFVTBDecoder（framework/codec/Apple），它解码完
// 直接交出一个 **PBAFFrame**：里面就是 VideoToolbox 给的 CVPixelBuffer，
// **没有任何 CPU 下载**（`mVTOutFmt` 只在模拟器上被设成 YUV420P 才会转换，
// 真机/桌面上默认直出）。所以 Apple 这条零拷贝路的关键只有两步：
//
//   1. 让 VideoToolbox 直接输出 **BGRA**（kCVPixelFormatType_32BGRA）：
//      通过 MediaPlayerConfig::pixelBufferOutputFormat（或同名全局设置）告诉框架，
//      框架把它当作目标 OSType 交给解码器。为什么不用 NV12：Metal 采样双平面
//      NV12 需要"Y 平面 + UV 平面两张纹理 + 自定义着色器合成"，而 Qt 的默认材质
//      采样不了两张纹理；BGRA 是一张普通纹理，Qt 直接就能用。
//   2. 用 CVMetalTextureCache 把 CVPixelBuffer **包成** MTLTexture：底层是
//      IOSurface 直通，不拷贝像素；再用
//      QQuickWindow::createTextureFromNativeObject(NativeObjectMetalTexture, ...)
//      交给 Qt 场景图。
//
// 前提（和 Windows 上必须同一个 ID3D11Device 是同一个道理）
// --------------------------------------------------------
// CVMetalTextureCache 必须用 **Qt 场景图正在用的那个 MTLDevice** 创建：不同
// MTLDevice 的纹理不能互相采样。设备从 QSGRendererInterface::DeviceResource 拿
// （渲染线程），见 prepare()。
//
// 线程约定：本文件所有函数只能在 Qt 的**渲染线程**上调用（updatePaintNode 里）。
// 唯一的例外是 setColorAdjust()：它从 GUI 线程调（用户拖滑块），只写三个原子量，
// 真正生效在渲染线程的 textureForFrame() 里（和 CicadaTextureD3D11 一样的写法）。
//
#ifndef CICADA_QT_CICADATEXTUREMETAL_H
#define CICADA_QT_CICADATEXTUREMETAL_H

#include <QtCore/QString>
#include <atomic>

class QQuickWindow;
class QSGTexture;
class IAFFrame;

namespace cicadaqt {

    /*
     * Metal 零拷贝纹理生产者。
     *
     * 生命周期跟着 QQuickItem 的渲染资源走：prepare() 在第一次渲染时调用，
     * releaseResources() 在场景图失效或 item 销毁时调用。
     */
    class CicadaTextureMetal {
    public:
        CicadaTextureMetal();
        ~CicadaTextureMetal();

        CicadaTextureMetal(const CicadaTextureMetal &) = delete;
        CicadaTextureMetal &operator=(const CicadaTextureMetal &) = delete;

        /* 渲染线程：拿 Qt 的 MTLDevice 并建纹理缓存。失败返回 false（走 CPU 回退）。 */
        bool prepare(QQuickWindow *window);

        /*
         * 渲染线程：把一帧（PBAFFrame/CVPixelBuffer）包成 QSGTexture。
         * 返回的纹理归调用方所有，失败返回 nullptr。
         */
        QSGTexture *textureForFrame(QQuickWindow *window, IAFFrame *frame, bool *flipVertically);

        /*
         * 渲染线程：**解码代际结束**时放掉我们替解码器握着的那些缓冲引用。
         *
         * 为什么需要它（和 Windows 那条 `releaseInputView()` 是**同一件事的两端**）：
         * D3D11 那边解码器的 20 片 surface 装在**我们自己**的一张输出纹理里，输入视图
         * 是唯一的外部引用；而 macOS 这边我们包的是 **VideoToolbox 自己的
         * CVPixelBuffer（IOSurface）**，持有者是
         *   * CVMetalTextureCache 里的缓存条目，以及
         *   * 帧环 d->refs[3] 里的 CVMetalTextureRef。
         * 这两者都钉着 IOSurface ⇒ 解码器的缓冲池不能复用/释放。所以关解码器之前同样
         * 要放掉它们。
         *
         * 与 Windows 的**关键差别**（照抄 Windows 会花屏/崩）：这边包出来的 MTLTexture
         * 不是拷贝，就是解码器那块内存。所以不能"全部放掉"：
         *   * CVMetalTextureCacheFlush(cache, 0) 只清**没在用**的缓存条目 —— 正在被采样的
         *     那些它自己会留着，安全；
         *   * 帧环里**保留最新那一帧**（Qt 的场景图是异步的，上一帧的命令缓冲可能还在跑），
         *     只放掉更老的两帧。
         * 这样既把"跨解码代际的多余引用"清干净，又不给正在采样的纹理抽底。
         */
        void releaseInputState();

        void releaseResources();

        const char *backendName() const
        {
            return "Metal zero-copy (VideoToolbox CVPixelBuffer -> MTLTexture, IOSurface)";
        }

        /*
         * 视频色彩调整（右键菜单 →「视频色彩调整」）。
         *
         * 取值与 CicadaTextureD3D11::setColorAdjust() **完全同量纲**：0~200，100 = 中性。
         * 可以**从 GUI 线程调**（拖滑块时），所以只写三个原子量；真正应用到 GPU 在渲染线程的
         * textureForFrame() 里（见 .mm 里的实现说明）。
         *
         * macOS 的做法（和 Windows 一样是"GPU 上过一遍"，绝不落到 CPU）：
         *   * 三个值**全中性时完全不进这条路** —— 仍旧把 VideoToolbox 的 MTLTexture 直接交给
         *     Qt，零额外 pass、零额外开销，画面与没有这个功能时逐像素一致；
         *   * 非中性时才用 Core Image 的 CIColorControls 在 GPU 上渲染到我们自己复用的一张
         *     输出纹理（3 槽环，有界），再交给 Qt 场景图。
         * 全程**零 CPU 下载**：没有 CVPixelBufferGetBaseAddress、没有 QImage、没有 swscale。
         */
        void setColorAdjust(float brightness, float contrast, float saturation);

        /*
         * 实现细节（PIMPL）：结构体在 .mm 里定义，为的是把 Objective-C / CoreVideo
         * 的类型挡在头文件外面（这个头会被 C++ 的 .cpp 包含）。外部不要碰它。
         */
        struct Private;
        Private *d = nullptr;

    private:
        bool m_failed = false;
        bool m_loggedFirstFrame = false;

        /*
         * 色彩调整的三个值：GUI 线程写（拖滑块）、渲染线程读（每帧），所以是原子量。
         * 取值 0~200，100 = 中性（和 CicadaTextureD3D11 一致）。
         */
        std::atomic<float> m_brightness{100.0f};
        std::atomic<float> m_contrast{100.0f};
        std::atomic<float> m_saturation{100.0f};

        /*
         * 三条**一次性**日志（绝不做每帧打印，拖滑块时更不能刷屏）：
         *   * 第一次真的走 CIColorControls 那条额外 pass；
         *   * 第一次从"有效果"回到中性（直通）；
         *   * 第一次这一帧没能应用（画面仍旧走直通，不黑屏）。
         */
        bool m_loggedColorAdjustActive = false;
        bool m_loggedColorAdjustBypass = false;
        bool m_loggedColorAdjustFailure = false;
    };

}// namespace cicadaqt

#endif// CICADA_QT_CICADATEXTUREMETAL_H
