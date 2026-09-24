//
// CicadaQtTextureWrap —— 把"原生 GPU 纹理"包成 Qt 的 QSGTexture。
//
// 为什么需要这一层
// ----------------
// Qt 5 有 QQuickWindow::createTextureFromNativeObject()，Qt 6 **删掉了**它。
// Qt 6 里把外部纹理交给场景图有两条官方路径，本文件把差异封在这里，三个零拷贝
// 后端（D3D11 / Metal / VAAPI-EGL）只调下面的三个函数：
//
//   1. Qt 6.8 及以上（首选）
//      QNativeInterface::QSG{D3D11,Metal,OpenGL}Texture::fromNative(...)
//      直接把原生纹理交给 Qt，Qt 自己建 QSGTexture 包装（内部会按后端补
//      SRV / EGLImage 之类的细节），这是最省事的官方接口。
//
//   2. Qt 6.5 ~ 6.7（兼容旧版本）
//      先 QRhiTexture::createFrom(NativeTexture) 把原生纹理包成一个 QRhi 纹理，
//      再 QQuickWindow::createTextureFromRhiTexture() 交给场景图。
//      这两个接口从 Qt 6.0 就有，所以 6.5 也能编能跑。
//
// 两条路都**不需要我们自己写 OpenGL/着色器**：采样、材质、批处理全部由 Qt 的场景图
// 完成，我们只负责"把解码出来的纹理递进去"。
//
// 所有权（重要）
// --------------
// 返回的 QSGTexture **不接管**原生纹理：
//   * Windows：ID3D11Texture2D 由 CicadaTextureD3D11 自己 Release；
//   * macOS  ：id<MTLTexture> 由 CVMetalTextureCache 的引用计数管，我们只放
//              CVMetalTextureRef；
//   * Linux  ：GL 纹理由 CicadaTextureVAAPI 自己 glDeleteTextures。
// （GL 那条还有一个 QQuickWindow::TextureOwnsGLTexture 选项可以反过来让 Qt 持有，
// 这里刻意不用 —— 纹理是我们复用的双缓冲，生命周期必须握在自己手里。）
//
// 线程约定：只能在 Qt 的渲染线程上调用（要有 Qt 的图形设备/上下文）。
//
#ifndef CICADA_QT_CICADAQTTEXTUREWRAP_H
#define CICADA_QT_CICADAQTTEXTUREWRAP_H

#include <QtCore/QSize>
#include <QtCore/QtGlobal>

class QQuickWindow;
class QSGTexture;

namespace cicadaqt {

    /*
     * 内部函数：Qt 6.5 ~ 6.7 的兜底实现
     * （QRhiTexture::createFrom(NativeTexture) + QQuickWindow::createTextureFromRhiTexture）。
     *
     * object：原生纹理句柄（ID3D11Texture2D* / id<MTLTexture> / GLuint，统一当整数传）；
     * layout：原生布局 —— OpenGL 后端要填纹理目标（GL_TEXTURE_2D），D3D11/Metal 传 0。
     *
     * 之所以放在头文件里导出，是因为 macOS 的 Metal 实现写在 .mm 里（要有 Objective-C），
     * 而 D3D11/GL 的实现和这里共用同一份兜底代码。
     */
    QSGTexture *wrapNativeTextureWithRhi(QQuickWindow *window, quint64 object, int layout,
                                         const QSize &size);

#if defined(Q_OS_WIN)
    /*
     * Windows：把 ID3D11Texture2D* 包成 QSGTexture。
     *
     * texture 是 ID3D11Texture2D*（视频处理器输出的 RGBA 纹理），它必须同时满足：
     *   * 带 D3D11_BIND_SHADER_RESOURCE（Qt 要从它建 SRV）；
     *   * 格式是 DXGI_FORMAT_R8G8B8A8_UNORM —— 和 Qt 包装时使用的格式一致。D3D11
     *     不允许给有类型的纹理建"另一种格式"的视图，格式不一致就是 E_INVALIDARG
     *     （日志里的 "Failed to create srv"），表现为画面全黑。
     * 详见 CicadaTextureD3D11.cpp 的 CICADA_QT_D3D11_OUTPUT_FORMAT。
     */
    QSGTexture *wrapD3D11Texture(QQuickWindow *window, void *texture, const QSize &size);
#endif

#if defined(Q_OS_MACOS)
    /*
     * macOS：把 id<MTLTexture> 包成 QSGTexture。
     * 传 void* 是为了让本头文件不必引入 Objective-C（调用方在 .mm 里 __bridge
     * 成 void* 即可）。
     */
    QSGTexture *wrapMetalTexture(QQuickWindow *window, void *texture, const QSize &size);
#endif

#if defined(Q_OS_LINUX)
    /*
     * Linux：把已经绑好 EGLImage 的 GL 纹理包成 QSGTexture。
     * 只能用在场景图跑 OpenGL 的时候（调用方负责判断）。
     */
    QSGTexture *wrapGLTexture(QQuickWindow *window, quint32 textureId, const QSize &size);
#endif

}// namespace cicadaqt

#endif// CICADA_QT_CICADAQTTEXTUREWRAP_H
