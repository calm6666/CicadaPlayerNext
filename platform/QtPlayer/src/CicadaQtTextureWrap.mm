//
// CicadaQtTextureWrap 的 macOS 实现：把 VideoToolbox 出来的 CVPixelBuffer 纹理
// （id<MTLTexture>，实际内存是 IOSurface）包成 QSGTexture。
//
// 用 .mm 是因为要写 Objective-C：id<MTLTexture> 是 ObjC 协议指针，
// 在纯 C++ 里没法声明（头文件里那个函数签名用的是 void*，这里 __bridge 过来）。
//
// 设计说明见 CicadaQtTextureWrap.h。
//

#include "CicadaQtTextureWrap.h"

#import <Metal/Metal.h>

#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGTexture>

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
/* QNativeInterface::QSGMetalTexture::fromNative()。 */
#include <QtQuick/qsgtexture_platform.h>
#endif

#include <utils/frame_work_log.h>

namespace cicadaqt {

    QSGTexture *wrapMetalTexture(QQuickWindow *window, void *texture, const QSize &size)
    {
        if (window == nullptr || texture == nullptr || size.isEmpty()) {
            return nullptr;
        }

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        /*
         * Qt 6.8+：官方接口。纹理不转移所有权（它归 CVMetalTextureCache /
         * 我们持有的 CVMetalTextureRef 管），所以不传额外选项。
         */
        QSGTexture *qsTexture = QNativeInterface::QSGMetalTexture::fromNative(
                                    (__bridge id<MTLTexture>) texture, window, size);
#else
        /*
         * Qt 6.5 ~ 6.7：包成 QRhi 纹理再交给场景图。
         * Metal 没有 layout/状态这一说，填 0。
         */
        QSGTexture *qsTexture = wrapNativeTextureWithRhi(
                                    window, static_cast<quint64>(reinterpret_cast<uintptr_t>(texture)), 0, size);
#endif

        if (qsTexture == nullptr) {
            AF_LOGW("Qt refused to wrap the Metal texture (%dx%d); falling back\n",
                    size.width(), size.height());
        }

        return qsTexture;
    }

}// namespace cicadaqt
