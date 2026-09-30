//
// CicadaVideoTexture 的实现，设计说明见 CicadaVideoTexture.h。
//

#include "CicadaVideoTexture.h"

#include <QtGui/QImage>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGTexture>
#include <QtCore/QDebug>

#include <utils/frame_work_log.h>
#include "base/media/AVAFPacket.h"

#if defined(Q_OS_MACOS)
/*
 * macOS 的 CPU 回退要把 PBAFFrame（CVPixelBuffer）下载成普通 CPU 帧，
 * 需要这个头（它只在 Apple 上编译，见 framework/utils/CMakeLists.txt）。
 */
#include "base/media/PBAFFrame.h"
#endif

#if defined(Q_OS_WIN)
#include "CicadaTextureD3D11.h"
#elif defined(Q_OS_MACOS)
#include "CicadaTextureMetal.h"
#elif defined(Q_OS_LINUX)
#include "CicadaTextureVAAPI.h"
#endif

extern "C" {
/*
 * swscale 只用在 CPU 回退路径上：把 YUV420P/NV12 转成 BGRA 再交给 Qt 上传。
 * 平台相关的零拷贝后端不需要它。
 */
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

/*
 * 这里不需要 using namespace Cicada：框架的帧/包类型（IAFFrame、AVAFPacket）和
 * 像素格式枚举（AF_PIX_FMT_*）都声明在**全局命名空间**里，见
 * framework/base/media/IAFPacket.h 和 framework/utils/AFMediaType.h。
 */

namespace cicadaqt {

    CicadaVideoTexture::CicadaVideoTexture() = default;

    CicadaVideoTexture::~CicadaVideoTexture()
    {
        /*
         * 析构可能在 UI 线程上（item 销毁），此时碰 GPU 资源不安全，所以真正的
         * 释放交给 releaseResources()（场景图失效时在渲染线程调用过）。
         * 这里只清理与 GPU 无关的东西。
         */
        if (m_sws != nullptr) {
            sws_freeContext(m_sws);
            m_sws = nullptr;
        }

        delete m_bgraImage;
        m_bgraImage = nullptr;
    }

    bool CicadaVideoTexture::reset(QQuickWindow *window, bool allowZeroCopy)
    {
        /*
         * releaseResources() 会把 GPU 侧的东西放掉，并把 m_prepared 打回 false
         * （它本来就是给"场景图失效"用的，语义正好）。之后 prepare() 会按新的
         * allowZeroCopy 重新探测一遍。
         */
        releaseResources();

        m_zeroCopyAllowed = allowZeroCopy;
        /* 换了判断依据，回退日志也重新来过。 */
        m_loggedFallback = false;

        return prepare(window);
    }

    bool CicadaVideoTexture::prepare(QQuickWindow *window)
    {
        if (m_prepared) {
            return m_zeroCopyActive;
        }

        m_prepared = true;

        /*
         * 应用层把硬解关了：解码器交出来的是内存里的 CPU 帧（YUV420P/NV12），
         * 零拷贝这条路从根上不适用 —— 直接判 CPU 路径，连后端都不进。
         *
         * m_loggedFallback 置真，是因为这不是"回退"（没有尝试过零拷贝），
         * 不需要那条 W 级日志再提醒一次。
         */
        if (!m_zeroCopyAllowed) {
            m_loggedFallback = true;
            AF_LOGI("hardware decoding is off: the video uses the CPU path "
                    "(swscale -> QImage -> QSGTexture)\n");
            return false;
        }

#if defined(Q_OS_WIN)
        m_d3d11.reset(new CicadaTextureD3D11());

        if (m_d3d11->prepare(window)) {
            m_zeroCopyActive = true;
            return true;
        }

        m_d3d11.reset();
        AF_LOGW("D3D11 zero-copy is unavailable, the video will go through the CPU path "
                "(download + upload). Playback still works, just with more memory traffic\n");
#elif defined(Q_OS_MACOS)
        m_metal.reset(new CicadaTextureMetal());

        if (m_metal->prepare(window)) {
            m_zeroCopyActive = true;
            return true;
        }

        m_metal.reset();
        AF_LOGW("Metal zero-copy is unavailable, falling back to the CPU path\n");
#elif defined(Q_OS_LINUX)
        m_vaapi.reset(new CicadaTextureVAAPI());

        if (m_vaapi->prepare(window)) {
            m_zeroCopyActive = true;
            return true;
        }

        m_vaapi.reset();
        AF_LOGW("VAAPI/EGL zero-copy is unavailable (the scene graph must run on OpenGL), "
                "falling back to the CPU path\n");
#else
        AF_LOGW("no zero-copy backend for this platform, using the CPU path\n");
#endif

        m_loggedFallback = true;
        return false;
    }

    QSGTexture *CicadaVideoTexture::textureForFrame(QQuickWindow *window, IAFFrame *frame,
            bool *flipVertically)
    {
        if (flipVertically != nullptr) {
            *flipVertically = false;
        }

        if (frame == nullptr || window == nullptr) {
            return nullptr;
        }

        if (m_zeroCopyActive) {
            QSGTexture *texture = nullptr;

#if defined(Q_OS_WIN)
            texture = m_d3d11->textureForFrame(window, frame, flipVertically);
#elif defined(Q_OS_MACOS)
            texture = m_metal->textureForFrame(window, frame, flipVertically);
#elif defined(Q_OS_LINUX)
            texture = m_vaapi->textureForFrame(window, frame, flipVertically);
#endif

            if (texture != nullptr) {
                return texture;
            }

            /*
             * 零拷贝跑到一半失败（驱动拒绝某种格式、设备丢了……）。不再重试，
             * 永久切到 CPU 路径，保证画面不会因此停住。
             */
            m_zeroCopyActive = false;

            if (!m_loggedFallback) {
                m_loggedFallback = true;
                AF_LOGW("zero-copy failed at runtime, switching to the CPU path for this session\n");
            }

            /*
             * 重要：平台后端可能在失败**之前**就把 flipVertically 置成了 true
             * （例如 Linux 的 VAAPI 后端在入口处就按 OpenGL 的约定置 true），
             * 而 CPU 路径用的是 QImage 上传出来的纹理，方向约定完全不同。
             * 不重置的话画面会上下颠倒。
             */
            if (flipVertically != nullptr) {
                *flipVertically = false;
            }
        }

        return textureForFrameCpu(window, frame);
    }

    QSGTexture *CicadaVideoTexture::textureForFrameCpu(QQuickWindow *window, IAFFrame *frame)
    {
        const int srcFormat = static_cast<int>(frame->getInfo().video.format);

        /*
         * **绝对不能让未下载的 GPU 帧走到 swscale。**
         *
         * 硬解直通（"video.render.hw.direct_texture"）的帧里没有像素数据：
         *   * AF_PIX_FMT_D3D11     -> data[0] 是 ID3D11Texture2D*，data[1] 是切片号
         *   * AF_PIX_FMT_VAAPI     -> data[3] 是 VASurfaceID
         * 把它们当平面指针交给 swscale（或 QImage）会直接读野地址 —— 崩溃或满屏花屏。
         *
         * 正常情况下不会走到这里：组件只有在零拷贝可用时才打开那个开关（见
         * CicadaPlayerItem::startPlaybackWhenReady）。这里是兜底：真走到来说明
         * 零拷贝在中途失败了，那就保持上一帧不动（画面停住总比崩了好），并打一条
         * 说清楚原因的日志。
         */
        switch (srcFormat) {
            case AF_PIX_FMT_D3D11:
            case AF_PIX_FMT_VAAPI:
                if (!m_loggedFallback) {
                    m_loggedFallback = true;
                    AF_LOGE("the frame is still a raw GPU frame (format %d) but zero-copy is "
                            "not available; the picture will stay on the last frame. "
                            "Restart without the direct-texture switch to use copy-back\n",
                            srcFormat);
                }

                return nullptr;
            default:
                break;
        }

#if defined(Q_OS_MACOS)
        /*
         * macOS 的 PBAFFrame（CVPixelBuffer）可以"下载"成普通 CPU 帧：
         * PBAFFrame 提供了这个转换运算符（内部是 CVPixelBufferLockBaseAddress +
         * av_image_copy，最后得到 NV12/YUV420P），这样即使 Metal 那条路不可用，
         * CPU 回退也能出画面。转换结果的 AVAFFrame 是 new 出来的，由我们接管。
         */
        std::unique_ptr<IAFFrame> downloaded;
        AVAFFrame *converted = nullptr;

        if (srcFormat == AF_PIX_FMT_APPLE_PIXEL_BUFFER) {
            if (auto *pbFrame = dynamic_cast<PBAFFrame *>(frame)) {
                converted = static_cast<AVAFFrame *>(*pbFrame);
            }

            if (converted == nullptr) {
                if (!m_loggedFallback) {
                    m_loggedFallback = true;
                    AF_LOGW("cannot download the CVPixelBuffer frame for the CPU path\n");
                }

                return nullptr;
            }

            downloaded.reset(converted);
            frame = downloaded.get();
        }
#endif

        uint8_t **data = frame->getData();
        int *lineSize = frame->getLineSize();
        const int width = frame->getInfo().video.width;
        const int height = frame->getInfo().video.height;

        if (data == nullptr || lineSize == nullptr || data[0] == nullptr ||
                width <= 0 || height <= 0) {
            return nullptr;
        }

        /*
         * CPU 回退的输入只有软件解码帧和硬解码 copy-back 下来的帧，都是 CPU 可访问的
         * 平面数据（YUV420P / NV12 / NV21 之类）。swscale 直接转成 BGRA：
         *   * 目标用 Qt 自己分配内存的 QImage（Format_ARGB32 在内存里就是 BGRA），
         *     这样 QImage 独占数据，交给 Qt 上传时不用担心外部缓冲被回收；
         *   * QImage 复用一个，只有分辨率变了才重新分配，避免每帧 33MB 的分配。
         */
        if (m_bgraImage == nullptr || m_bgraImage->width() != width ||
                m_bgraImage->height() != height) {
            delete m_bgraImage;
            m_bgraImage = new QImage(width, height, QImage::Format_ARGB32);
        }

        if (m_sws == nullptr || m_swsSrcWidth != width || m_swsSrcHeight != height ||
                m_swsSrcFormat != srcFormat) {
            m_sws = sws_getCachedContext(m_sws, width, height, static_cast<AVPixelFormat>(srcFormat),
                                         width, height, AV_PIX_FMT_BGRA, SWS_BILINEAR,
                                         nullptr, nullptr, nullptr);
            m_swsSrcWidth = width;
            m_swsSrcHeight = height;
            m_swsSrcFormat = srcFormat;

            if (m_sws == nullptr) {
                AF_LOGE("cannot create the swscale context for format %d\n", srcFormat);
                return nullptr;
            }
        }

        uint8_t *dstData[4] = {m_bgraImage->bits(), nullptr, nullptr, nullptr};
        int dstLineSize[4] = {static_cast<int>(m_bgraImage->bytesPerLine()), 0, 0, 0};

        sws_scale(m_sws, data, lineSize, 0, height, dstData, dstLineSize);

        /*
         * createTextureFromImage() 会把 QImage 上传成 GPU 纹理（Qt 内部完成，Qt 6
         * 的 RHI 会按当前后端选合适的路径）。这一步有一次 CPU->GPU 拷贝，
         * 这正是零拷贝省掉的东西。
         */
        return window->createTextureFromImage(*m_bgraImage);
    }

    void CicadaVideoTexture::releaseInputState()
    {
#if defined(Q_OS_WIN)
        if (m_d3d11) {
            m_d3d11->releaseInputView();
        }
#elif defined(Q_OS_MACOS)
        /*
         * macOS：同上一条契约，但**放的东西不一样**。
         *
         * D3D11 那边解码器的整池 surface 装在我们自己的输出纹理里，输入视图是唯一外部
         * 引用；macOS 这边我们包的**就是** VideoToolbox 的 CVPixelBuffer（IOSurface），
         * 钉住它的是 CVMetalTextureCache 的缓存条目 + 帧环里那几个 CVMetalTextureRef。
         * 关解码器之前同样要放，否则解码器的缓冲池跨代际留着。
         *
         * 具体怎么放、为什么不能"全放"（正在被采样的纹理不能抽底）写在
         * CicadaTextureMetal::releaseInputState() 上 —— 那一头是权威。
         */
        if (m_metal) {
            m_metal->releaseInputState();
        }
#endif

        /*
         * Linux/VAAPI 仍是有意的空实现：这一轮没有在 VAAPI 上实测过，不凭空写"释放"动作
         * （放错了会当场花屏或崩）。以后真在那边量到"停播后显存不回收"，在它自己的后端里
         * 加同名方法，而不是在这里瞎放东西。
         *
         * 注意：这里**不碰** m_prepared / m_zeroCopyActive —— 那两个是给
         * startPlaybackWhenReady() 判断"要不要开 direct_texture"用的，动了就会
         * 让直通决策读到错的探测结果（见 releaseResources 和它的调用点）。
         */
    }

    void CicadaVideoTexture::flushDeferredDestruction()
    {
#if defined(Q_OS_WIN)
        if (m_d3d11) {
            m_d3d11->flushDeferredDestruction();
        }
#endif

        /*
         * macOS / Linux / CPU 回退是**有意为空**的：这三个后端没有"D3D11 延迟销毁"
         * 这件事 —— Metal 的纹理释放是即时的，VAAPI/CPU 根本没有 D3D 对象，所以没有
         * 任何"再推一把"的动作可做。以后哪个后端真量到"引用放了、内存不降"，
         * 在它自己的后端里补同名动作，而不是在这里瞎放东西。
         */
    }

    void CicadaVideoTexture::forgetOutputTextureWrapper()
    {
#if defined(Q_OS_WIN)
        if (m_d3d11) {
            m_d3d11->forgetOutputTextureWrapper();
        }
#endif

        /*
         * macOS / Linux 是有意的空实现：那两条路上没有"输出纹理包装"这个中间层
         * （Metal 那条直接包解码纹理、VAAPI 那条是 EGLImage 绑定的 GL 纹理，
         * 生命周期各自在自己的后端里管），所以没有需要"忘掉"的记录。
         */
    }

    void CicadaVideoTexture::releaseResources()
    {
#if defined(Q_OS_WIN)
        if (m_d3d11) {
            m_d3d11->releaseResources();
        }
#elif defined(Q_OS_MACOS)
        if (m_metal) {
            m_metal->releaseResources();
        }
#elif defined(Q_OS_LINUX)
        if (m_vaapi) {
            m_vaapi->releaseResources();
        }
#endif

        /*
         * CPU 回退那两件大东西也一起放掉。
         *
         *   * m_bgraImage 是 swscale 的目标位图，尺寸 = 视频原始画布：
         *     1080p = 1920*1080*4 = 8.29MB，4K = 3840*2160*4 = 33.18MB；
         *   * m_sws 是 swscale 上下文，里面挂着它自己的转换表。
         *
         * 两个都是**纯缓存**：下一帧 textureForFrameCpu() 会按需重新分配
         * （它本来就是"分辨率变了才重新分配"的写法），所以在这里释放不影响任何行为、
         * 也不影响精度（同一套 sws 参数，重建出来的转换完全一样）。
         *
         * 为什么放这里而不是只靠析构：原来它们**只在 ~CicadaVideoTexture() 里**释放，
         * 而本函数才是"这一套渲染资源不要了"的既有落点（场景图失效 / 后端重建 /
         * item 析构都会走到）。窗口关掉到 item 析构之间有一段时间，这期间
         * 零拷贝失败退回 CPU 路径的那些会话就会白挂着这块整幅位图。
         *
         * 只碰内存、不碰任何 GPU / COM 对象，所以放在这个函数里对线程没有新要求
         *（它本来就在渲染线程与析构路径上被调用）。
         */

        if (m_sws != nullptr) {
            sws_freeContext(m_sws);
            m_sws = nullptr;
        }

        m_swsSrcWidth = 0;
        m_swsSrcHeight = 0;
        m_swsSrcFormat = 0;

        /* 记下"放掉了多大一张目标位图"用于日志（下一句就把它删掉）。 */
        const qsizetype releasedBgraBytes = (m_bgraImage != nullptr) ? m_bgraImage->sizeInBytes() : 0;

        delete m_bgraImage;
        m_bgraImage = nullptr;

        /* 下一次 prepare() 重新探测一遍（例如窗口换了图形后端）。 */
        m_prepared = false;
        m_zeroCopyActive = false;

        /*
         * 【释放探针】CPU 回退路径那两件大东西的释放记录：目标位图是**整幅** BGRA
         * （1080p = 1920*1080*4 = 8.29MB，4K = 3840*2160*4 = 33.18MB）。零拷贝会话里
         * 它从来没被分配过，所以这里打印 0 是正常的。
         * 场景图失效 / 后端重建 / item 析构三条路都会走到这一句。
         */
        AF_LOGI("[mem] video texture backend released: CPU fallback bitmap %lld bytes, "
                "swscale context freed (platform backend released above)\n",
                (long long) releasedBgraBytes);
    }

    QString CicadaVideoTexture::backendName() const
    {
        if (!m_zeroCopyActive) {
            return QStringLiteral("CPU copy-back (swscale -> QImage -> QSGTexture)");
        }

#if defined(Q_OS_WIN)
        return QString::fromLatin1(m_d3d11->backendName());
#elif defined(Q_OS_MACOS)
        return QString::fromLatin1(m_metal->backendName());
#elif defined(Q_OS_LINUX)
        return QString::fromLatin1(m_vaapi->backendName());
#else
        return QStringLiteral("unknown");
#endif
    }

    /* ---- 色彩调整（GUI 线程写、渲染线程读）---- */

    void CicadaVideoTexture::setColorAdjust(int brightness, int contrast, int saturation)
    {
        /* 夹到 0~200（100 = 中性）：QML 那边也是这个范围，双保险 */
        m_brightness.store(qBound(0, brightness, 200));
        m_contrast.store(qBound(0, contrast, 200));
        m_saturation.store(qBound(0, saturation, 200));
        /* 标脏：渲染线程下一帧会把它应用到后端（见 takeColorAdjustDirty 的调用点） */
        m_colorAdjustDirty.store(true);

#if defined(Q_OS_WIN)
        /* 后端已经建好了就直接转给它（它自己也是线程安全的：3 个原子量 + 脏标记）；
         * 后端还没建（比如刚换过片源）时不用管 —— textureForFrame() 里会补一次。 */
        if (m_d3d11) {
            m_d3d11->setColorAdjust(m_brightness.load(), m_contrast.load(), m_saturation.load());
        }
#endif
    }

    int CicadaVideoTexture::brightness() const
    {
        return m_brightness.load();
    }

    int CicadaVideoTexture::contrast() const
    {
        return m_contrast.load();
    }

    int CicadaVideoTexture::saturation() const
    {
        return m_saturation.load();
    }

    bool CicadaVideoTexture::colorAdjustSupported() const
    {
        /*
         * 目前只有 Windows 的 D3D11 零拷贝路径实现了（视频处理器的过滤器）。
         * 其它后端（Metal / VAAPI / CPU 回退）要在各自的像素转换里另做，
         * 所以这里如实返回 false —— 界面据此显示"当前后端不支持"。
         */
#if defined(Q_OS_WIN)
        return true;
#else
        return false;
#endif
    }

    bool CicadaVideoTexture::takeColorAdjustDirty()
    {
        return m_colorAdjustDirty.exchange(false);
    }

    void CicadaVideoTexture::setColorAdjustDirty()
    {
        m_colorAdjustDirty.store(true);
    }

}// namespace cicadaqt
