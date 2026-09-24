//
// macOS 零拷贝后端的实现，设计说明见 CicadaTextureMetal.h。
//

#include "CicadaTextureMetal.h"
#include "CicadaQtTextureWrap.h"

#if defined(Q_OS_MACOS)

#include <CoreVideo/CoreVideo.h>
#include <CoreVideo/CVMetalTextureCache.h>
#include <Metal/Metal.h>

#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>
#include <QtCore/QSize>

#include <utils/frame_work_log.h>
#include "base/media/PBAFFrame.h"

namespace cicadaqt {

    /* Objective-C / CoreVideo 类型都藏在这里，头文件保持干净。 */
    struct CicadaTextureMetal::Private {
        id<MTLDevice> device = nil;
        CVMetalTextureCacheRef cache = nullptr;
        /*
         * 近几帧的纹理引用。
         *
         * CVMetalTextureRef 必须活到 Qt 真正采样完 —— Qt 的场景图是异步的，上一两帧
         * 的命令缓冲可能还在跑，提前释放会让 IOSurface 映射失效（花屏或崩溃）。
         * 用一个 3 槽的环：每帧覆盖最老的那一个，既保证有余量，又不会无限增长。
         */
        CVMetalTextureRef refs[3] = {nullptr, nullptr, nullptr};
        int next = 0;
    };

    CicadaTextureMetal::CicadaTextureMetal()
        : d(new Private())
    {
    }

    CicadaTextureMetal::~CicadaTextureMetal()
    {
        releaseResources();
        delete d;
        d = nullptr;
    }

    bool CicadaTextureMetal::prepare(QQuickWindow *window)
    {
        if (d->cache != nullptr) {
            return true;
        }

        if (window == nullptr) {
            return false;
        }

        auto *rif = window->rendererInterface();

        if (rif == nullptr || rif->graphicsApi() != QSGRendererInterface::Metal) {
            AF_LOGW("Qt scene graph is not using Metal (api=%d), "
                    "the Metal zero-copy path is unavailable\n",
                    rif != nullptr ? (int) rif->graphicsApi() : -1);
            m_failed = true;
            return false;
        }

        /*
         * 场景图的 Metal 设备。只能在渲染线程问（我们现在就在），
         * 而且必须是**这一个**设备：CVMetalTextureCache 用它建的纹理才能被
         * Qt 的场景图直接采样。
         */
        d->device = (__bridge id<MTLDevice>) rif->getResource(
                        window, QSGRendererInterface::DeviceResource);

        if (d->device == nil) {
            AF_LOGE("Qt gave no Metal device, the Metal zero-copy path is unavailable");
            m_failed = true;
            return false;
        }

        /*
         * 纹理缓存：iOSurface 支撑的 CVPixelBuffer 通过它包成 MTLTexture，
         * 不发生像素拷贝（同一个 IOSurface）。
         */
        const CVReturn err = CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr,
                             d->device, nullptr, &d->cache);

        if (err != kCVReturnSuccess || d->cache == nullptr) {
            AF_LOGE("CVMetalTextureCacheCreate failed (%d), falling back to the CPU path\n",
                    (int) err);
            m_failed = true;
            return false;
        }

        AF_LOGI("Metal zero-copy is ready: VideoToolbox CVPixelBuffers are wrapped into "
                "MTLTextures (IOSurface, no copy) and handed to the Qt scene graph\n");
        return true;
    }

    void CicadaTextureMetal::releaseResources()
    {
        if (d == nullptr) {
            return;
        }

        for (int i = 0; i < 3; ++i) {
            if (d->refs[i] != nullptr) {
                CFRelease(d->refs[i]);
                d->refs[i] = nullptr;
            }
        }

        d->next = 0;

        if (d->cache != nullptr) {
            /* 先把缓存里没用的条目清掉再释放，避免驱动侧残留。 */
            CVMetalTextureCacheFlush(d->cache, 0);
            CFRelease(d->cache);
            d->cache = nullptr;
        }

        d->device = nil;
    }

    QSGTexture *CicadaTextureMetal::textureForFrame(QQuickWindow *window, IAFFrame *frame,
            bool *flipVertically)
    {
        if (flipVertically != nullptr) {
            /* Metal 的纹理原点在左上，Qt 的 Metal 后端也是，不需要翻转。 */
            *flipVertically = false;
        }

        if (m_failed || frame == nullptr || window == nullptr || d->cache == nullptr) {
            return nullptr;
        }

        /*
         * Apple 平台的解码帧是 PBAFFrame（见 framework/base/media/PBAFFrame.h）：
         * 里面就是 VideoToolbox 的 CVPixelBuffer。不是这种帧（软解、或者走了别的
         * 解码器）就交给 CPU 路径。
         */
        auto *pbFrame = dynamic_cast<PBAFFrame *>(frame);

        if (pbFrame == nullptr) {
            return nullptr;
        }

        CVPixelBufferRef pixelBuffer = pbFrame->getPixelBuffer();

        if (pixelBuffer == nullptr) {
            return nullptr;
        }

        const int width = static_cast<int>(CVPixelBufferGetWidth(pixelBuffer));
        const int height = static_cast<int>(CVPixelBufferGetHeight(pixelBuffer));
        const OSType pixelFormat = CVPixelBufferGetPixelFormatType(pixelBuffer);

        /*
         * 只处理 BGRA。
         *
         * 为什么必须让解码器直接输出 BGRA（见 CicadaPlayerItem 里设的
         * pixelBufferOutputFormat）：双平面 NV12 需要两张纹理 + 自定义着色器才能
         * 采样，Qt 的默认材质做不到；BGRA 是一张普通纹理，Qt 直接可用。
         *
         * 32ARGB / 32ABGR 这些也能映射到 Metal 格式，但字节序和 Qt 期望的 RGBA8
         * 不一致（颜色会错），所以这里只认 32BGRA。
         */
        MTLPixelFormat mtlFormat = MTLPixelFormatInvalid;

        switch (pixelFormat) {
            case kCVPixelFormatType_32BGRA:
                mtlFormat = MTLPixelFormatBGRA8Unorm;
                break;
            default:
                /*
                 * 不是 BGRA：说明 MediaPlayerConfig::pixelBufferOutputFormat 没生效
                 * （或者拿到了 10bit HDR 的 x420）。交给 CPU 路径，画面不会丢。
                 */
                if (!m_loggedFirstFrame) {
                    m_loggedFirstFrame = true;
                    AF_LOGW("the decoded CVPixelBuffer is not 32BGRA (format 0x%08x); "
                            "the Metal zero-copy path needs BGRA, using the CPU path\n",
                            (unsigned) pixelFormat);
                }

                return nullptr;
        }

        CVMetalTextureRef metalTexture = nullptr;

        /*
         * 把 CVPixelBuffer 的第 0 个平面包成 MTLTexture。
         *
         * BGRA 是单平面格式，planeIndex = 0；width/height 必须和平面一致。
         * 这一步是 IOSurface 直通，不拷贝像素。
         */
        const CVReturn err = CVMetalTextureCacheCreateTextureFromImage(
                                 kCFAllocatorDefault, d->cache, pixelBuffer, nullptr,
                                 mtlFormat, static_cast<size_t>(width),
                                 static_cast<size_t>(height), 0, &metalTexture);

        if (err != kCVReturnSuccess || metalTexture == nullptr) {
            AF_LOGW("CVMetalTextureCacheCreateTextureFromImage failed (%d)\n", (int) err);
            return nullptr;
        }

        id<MTLTexture> texture = CVMetalTextureGetTexture(metalTexture);

        if (texture == nil) {
            CFRelease(metalTexture);
            return nullptr;
        }

        /*
         * 换帧：写进环里的下一个槽，顺便放掉 3 帧之前那一份。
         *
         * 为什么至少要留几帧：Qt 的场景图是异步的，前几帧的命令缓冲可能还在执行，
         * 对应的 IOSurface 映射不能提前释放。环状保留既保证有余量，又不会无限增长。
         */
        if (d->refs[d->next] != nullptr) {
            CFRelease(d->refs[d->next]);
        }

        d->refs[d->next] = metalTexture;
        d->next = (d->next + 1) % 3;

        if (!m_loggedFirstFrame) {
            m_loggedFirstFrame = true;
            AF_LOGI("first zero-copy frame: %dx%d BGRA CVPixelBuffer -> MTLTexture "
                    "(IOSurface, no copy) -> Qt\n", width, height);
        }

        /*
         * 交给 Qt 场景图（具体的包装方式按 Qt 版本分两条路，见 CicadaQtTextureWrap）。
         *
         * 注意：CVMetalTextureRef 由我们自己持有（d->refs 那个 3 槽环），
         * 而这里返回的 QSGTexture 是 Qt 自己新建的包装对象，归调用方所有
         * （QSGSimpleTextureNode::setOwnsTexture(true) 会释放它）。
         */
        QSGTexture *qsTexture = wrapMetalTexture(window, (__bridge void *) texture,
                                QSize(width, height));

        if (qsTexture == nullptr) {
            return nullptr;
        }

        return qsTexture;
    }

}// namespace cicadaqt

#endif// Q_OS_MACOS
