//
// CicadaQtTextureWrap 的实现，设计说明见 CicadaQtTextureWrap.h。
//
// 本文件负责 Windows（D3D11）和 Linux（OpenGL）两条路；macOS 的 Metal 版本写在
// CicadaQtTextureWrap.mm 里（那段代码需要 Objective-C 语法）。
// 平台无关的兜底实现 wrapNativeTextureWithRhi() 也放在这里，三个平台共用。
//

#include "CicadaQtTextureWrap.h"

#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGTexture>
#include <QtQuick/QSGRendererInterface>
#include <utils/frame_work_log.h>

/*
 * QRhi 兜底路线（Qt 6.5 ~ 6.7 才需要，6.8 起走 native interface）。
 *
 * rhi/qrhi.h 不在普通的 Qt6::Gui 包含路径里 —— 它放在 Qt 的"版本化头目录"
 * （include/QtGui/<版本>/QtGui/rhi/qrhi.h），只有链了 Qt6::GuiPrivate 才会出现
 * （见 Qt6GuiPrivateTargets.cmake 里的 INTERFACE_INCLUDE_DIRECTORIES）。
 * 所以这里用 __has_include 探测：拿得到就编出兜底路线，拿不到就编成"没有这条路线"，
 * 工程照样能编（那台机器上零拷贝会自动退到 CPU 路径，日志里会说明原因）。
 */
#if QT_VERSION < QT_VERSION_CHECK(6, 8, 0) && defined(__has_include)
#  if __has_include(<QtGui/rhi/qrhi.h>)
#    include <QtGui/rhi/qrhi.h>
#    define CICADA_QT_HAVE_QRHI 1
#  elif __has_include(<rhi/qrhi.h>)
#    include <rhi/qrhi.h>
#    define CICADA_QT_HAVE_QRHI 1
#  endif
#endif

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
/* QNativeInterface::QSG{D3D11,OpenGL}Texture::fromNative() 就在这个头里。 */
#include <QtQuick/qsgtexture_platform.h>
#endif

namespace cicadaqt {

    QSGTexture *wrapNativeTextureWithRhi(QQuickWindow *window, quint64 object, int layout,
                                         const QSize &size, bool hasAlpha)
    {
        if (window == nullptr || object == 0 || size.isEmpty()) {
            return nullptr;
        }

#if !defined(CICADA_QT_HAVE_QRHI)
        /*
         * 走到这里说明 Qt < 6.8 且包含路径里没有 QRhi 头（没链 Qt6::GuiPrivate）。
         * 不是错误，只是这条兜底路线没编出来 —— 交给 CPU 回退。
         */
        (void) layout;
        AF_LOGW("this Qt version has no QRhi headers on the include path, so the "
                "native-texture fallback is not compiled in; the video goes through the "
                "CPU path (link Qt6::GuiPrivate to enable it, or use Qt 6.8+)\n");
        return nullptr;
#else
        auto *rif = window->rendererInterface();

        if (rif == nullptr) {
            AF_LOGW("no QSGRendererInterface, cannot wrap the native texture\n");
            return nullptr;
        }

        /*
         * QRhi 对象必须从场景图里拿：它是 Qt 正在用的那一套图形设备，
         * 自己 new 一个 QRhi 会得到另一个设备，包出来的纹理照样没法采样。
         */
        auto *rhi = static_cast<QRhi *>(
                        rif->getResource(window, QSGRendererInterface::RhiResource));

        if (rhi == nullptr) {
            AF_LOGW("Qt gave no QRhi, cannot wrap the native texture\n");
            return nullptr;
        }

        /*
         * 格式**不是**"只作登记"：QRhiTexture 的 NativeTexture 结构里只有 object 和
         * layout，没有像素格式字段，D3D11 后端就是拿这里声明的格式去建 SRV 的 ——
         * 而 D3D11 规定有类型的（非 typeless）纹理只能用**完全相同**的格式建视图，
         * 对不上就直接 E_INVALIDARG（日志里那句 "Failed to create srv: COM error
         * 0x80070057"），材质采不到纹理，画面全黑。这一点踩过坑，别再假设"后端会自己
         * 读原生纹理的格式"。
         *
         * 所以这里的 RGBA8 必须和被包装的那张原生纹理保持一致：
         *   * Windows：视频处理器输出的纹理是 DXGI_FORMAT_R8G8B8A8_UNORM
         *     （见 CicadaTextureD3D11.cpp 的 CICADA_QT_D3D11_OUTPUT_FORMAT）；
         *   * Linux：EGLImage 绑出来的那层 GL 纹理没有"像素格式"这回事，QRhi 直接沿用
         *     既有纹理，这个参数不影响结果。
         *
         * 用 RGBA8 而不是 BGRA8：BGRA8 这个枚举在 Qt 6.5 上不一定有，而 6.5 正是这条
         * 兜底路径要照顾的版本。
         *
         * sampleCount 必须是 1（视频纹理不带 MSAA）。
         */
        QRhiTexture *texture = rhi->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::Flags());

        if (texture == nullptr) {
            AF_LOGW("QRhi::newTexture failed\n");
            return nullptr;
        }

        QRhiTexture::NativeTexture native = {};
        native.object = object;
        native.layout = layout;

        if (!texture->createFrom(native)) {
            AF_LOGW("QRhiTexture::createFrom failed (object %p)\n",
                    reinterpret_cast<void *>(static_cast<quintptr>(object)));
            delete texture;
            return nullptr;
        }

        /* createTextureFromRhiTexture() 成功之后，纹理的所有权归 QSGTexture。
         * hasAlpha 必须如实声明：这条兜底路上它是唯一能告诉 Qt"这张纹理有透明区"的渠道。 */
        const QQuickWindow::CreateTextureOptions options =
            hasAlpha ? QQuickWindow::CreateTextureOptions(QQuickWindow::TextureHasAlphaChannel)
                     : QQuickWindow::CreateTextureOptions();
        QSGTexture *qsTexture = window->createTextureFromRhiTexture(texture, options);

        if (qsTexture == nullptr) {
            AF_LOGW("createTextureFromRhiTexture failed\n");
            delete texture;
            return nullptr;
        }

        return qsTexture;
#endif// CICADA_QT_HAVE_QRHI
    }

#if defined(Q_OS_WIN)

    QSGTexture *wrapD3D11Texture(QQuickWindow *window, void *texture, const QSize &size,
                                 bool hasAlpha)
    {
        if (window == nullptr || texture == nullptr || size.isEmpty()) {
            return nullptr;
        }

        const QQuickWindow::CreateTextureOptions options =
            hasAlpha ? QQuickWindow::CreateTextureOptions(QQuickWindow::TextureHasAlphaChannel)
                     : QQuickWindow::CreateTextureOptions();

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        /* Qt 6.8+：官方接口，Qt 自己从纹理建 SRV 并组装 QSGTexture。 */
        QSGTexture *qsTexture = QNativeInterface::QSGD3D11Texture::fromNative(texture, window, size,
                                                                              options);
#else
        /*
         * Qt 6.5 ~ 6.7：包成 QRhi 纹理再交给场景图。
         * D3D11 的 layout 参数（资源状态）这里用 0：QRhi 的 D3D11 后端会按着色器
         * 资源视图来用这张纹理，不需要我们指定状态。
         */
        QSGTexture *qsTexture = wrapNativeTextureWithRhi(
                                    window, reinterpret_cast<quint64>(texture), 0, size, hasAlpha);
#endif

        if (qsTexture == nullptr) {
            AF_LOGW("Qt refused to wrap the D3D11 texture (%dx%d); falling back\n",
                    size.width(), size.height());
        }

        return qsTexture;
    }

#elif defined(Q_OS_LINUX)

    QSGTexture *wrapGLTexture(QQuickWindow *window, quint32 textureId, const QSize &size)
    {
        if (window == nullptr || textureId == 0 || size.isEmpty()) {
            return nullptr;
        }

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        /*
         * Qt 6.8+：官方接口。
         *
         * 不传 QQuickWindow::TextureOwnsGLTexture —— 这个 GL 纹理是我们自己的
         * 双缓冲（EGLImage 绑上去的那一张），生命周期必须由 CicadaTextureVAAPI 管。
         */
        QSGTexture *qsTexture = QNativeInterface::QSGOpenGLTexture::fromNative(textureId, window, size);
#else
        /*
         * Qt 6.5 ~ 6.7：layout 要填 GL 纹理目标（这里是 GL_TEXTURE_2D），
         * QRhi 的 GL 后端靠它知道该把哪张纹理绑起来采样。
         */
        QSGTexture *qsTexture = wrapNativeTextureWithRhi(window, textureId, 0x0DE1 /* GL_TEXTURE_2D */,
                                size);
#endif

        if (qsTexture == nullptr) {
            AF_LOGW("Qt refused to wrap the GL texture %u (%dx%d); falling back\n",
                    textureId, size.width(), size.height());
        }

        return qsTexture;
    }

#endif// Q_OS_WIN / Q_OS_LINUX

}// namespace cicadaqt
