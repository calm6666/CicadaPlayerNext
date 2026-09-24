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
//
#ifndef CICADA_QT_CICADATEXTUREMETAL_H
#define CICADA_QT_CICADATETEXTUREMETAL_H

#include <QtCore/QString>

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

        void releaseResources();

        const char *backendName() const
        {
            return "Metal zero-copy (VideoToolbox CVPixelBuffer -> MTLTexture, IOSurface)";
        }

        /*
         * 实现细节（PIMPL）：结构体在 .mm 里定义，为的是把 Objective-C / CoreVideo
         * 的类型挡在头文件外面（这个头会被 C++ 的 .cpp 包含）。外部不要碰它。
         */
        struct Private;
        Private *d = nullptr;

    private:
        bool m_failed = false;
        bool m_loggedFirstFrame = false;
    };

}// namespace cicadaqt

#endif// CICADA_QT_CICADATEXTUREMETAL_H
