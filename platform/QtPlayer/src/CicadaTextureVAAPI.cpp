//
// Linux 零拷贝后端的实现，设计说明见 CicadaTextureVAAPI.h。
//

#include "CicadaTextureVAAPI.h"
#include "CicadaQtTextureWrap.h"

#if defined(Q_OS_LINUX)

#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>
#include <QtCore/QSize>
#include <QtGui/QOpenGLContext>
#include <QtGui/QOpenGLFunctions>

#include <utils/frame_work_log.h>
#include "base/media/AVAFPacket.h"

#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/pixfmt.h>
}

/*
 * CICADA_QT_HAVE_VAAPI 由 CMake 在 Linux 上探测到 libva + EGL 之后定义。
 * 没有它就整体退化成"这条后端不可用"，工程仍然能编（走 CPU 路径）。
 */
#if defined(CICADA_QT_HAVE_VAAPI)

#include <va/va.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

/* close()：导出 dmabuf 之后要关掉 fd（EGLImage 已经 dup 过了）。 */
#include <unistd.h>
#include <cstring>

#endif// CICADA_QT_HAVE_VAAPI

namespace cicadaqt {

#if defined(CICADA_QT_HAVE_VAAPI)

    /*
     * 输出缓冲的份数：VPP 正在写第 N 帧时，第 N-1 帧可能还在被 Qt 采样，
     * 所以 surface / EGLImage / GL 纹理都准备两份轮换。
     */
    static const int kBufferCount = 2;

    /* 一次导出+绑定好的输出缓冲（surface + EGLImage + GL 纹理）。 */
    struct OutputBuffer {
        VASurfaceID surface = VA_INVALID_SURFACE;
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        GLuint texture = 0;
    };

    struct CicadaTextureVAAPI::Private {
        /* ---- VAAPI（跟着帧里的 VADisplay 走，可能中途变化） ---- */
        VADisplay display = nullptr;
        VAConfigID config = VA_INVALID_ID;
        VAContextID context = VA_INVALID_ID;

        /* ---- 输出缓冲 ---- */
        OutputBuffer buffers[kBufferCount];
        int next = 0;
        int width = 0;
        int height = 0;

        /*
         * GL 函数表：不用直接链接 libGL（Qt 自己也是动态取的，直接链接在部分平台上
         * 会拿不到符号），统一通过当前上下文的 QOpenGLFunctions。
         */
        QOpenGLFunctions *gl = nullptr;

        /* ---- EGL / GL 扩展函数指针 ---- */
        PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = nullptr;
        PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = nullptr;
        PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = nullptr;
        /* 驱动是否支持 dmabuf modifier（EGL_EXT_image_dma_buf_import_modifiers）。 */
        bool hasModifiers = false;

        /* ---- 解码纹理尺寸（判断要不要重建输出缓冲） ---- */
        int inputWidth = 0;
        int inputHeight = 0;
    };

    CicadaTextureVAAPI::CicadaTextureVAAPI()
        : d(new Private())
    {
    }

    CicadaTextureVAAPI::~CicadaTextureVAAPI()
    {
        /*
         * 析构可能不在渲染线程上（item 销毁），此时销毁 GL/EGL/VAAPI 对象不安全。
         * 正常路径下 releaseResources() 已经在场景图失效时调用过了。
         */
        delete d;
        d = nullptr;
    }

    /* 解析 EGL/GL 扩展函数：都通过当前上下文的 getProcAddress 拿，不做直接链接。 */
    static bool resolveExtensions(CicadaTextureVAAPI::Private *d)
    {
        if (d->eglCreateImageKHR != nullptr) {
            return true;
        }

        QOpenGLContext *context = QOpenGLContext::currentContext();

        if (context == nullptr) {
            AF_LOGE("no current OpenGL context, cannot resolve the EGL/GL extensions\n");
            return false;
        }

        d->gl = context->functions();

        if (d->gl == nullptr) {
            AF_LOGE("the current OpenGL context gave no function table\n");
            return false;
        }

        d->eglCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
                                   context->getProcAddress("eglCreateImageKHR"));
        d->eglDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
                                    context->getProcAddress("eglDestroyImageKHR"));
        d->glEGLImageTargetTexture2DOES = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
                                              context->getProcAddress("glEGLImageTargetTexture2DOES"));

        if (d->eglCreateImageKHR == nullptr || d->eglDestroyImageKHR == nullptr ||
                d->glEGLImageTargetTexture2DOES == nullptr) {
            AF_LOGW("EGL/GL dmabuf extensions are missing "
                    "(eglCreateImageKHR=%p, glEGLImageTargetTexture2DOES=%p); "
                    "the VAAPI zero-copy path is unavailable\n",
                    (void *) d->eglCreateImageKHR, (void *) d->glEGLImageTargetTexture2DOES);
            return false;
        }

        /*
         * modifier 是 dmabuf 布局的一部分（tiling/压缩）。老驱动只支持
         * EGL_EXT_image_dma_buf_import（不带 modifiers），那时不能传 modifier 属性，
         * 否则 eglCreateImageKHR 直接返回 EGL_NO_IMAGE_KHR。
         */
        const char *extensions = eglQueryString(eglGetCurrentDisplay(), EGL_EXTENSIONS);
        d->hasModifiers = extensions != nullptr &&
                          strstr(extensions, "EGL_EXT_image_dma_buf_import_modifiers") != nullptr;

        return true;
    }

    bool CicadaTextureVAAPI::prepare(QQuickWindow *window)
    {
        if (m_failed) {
            return false;
        }

        if (window == nullptr) {
            return false;
        }

        auto *rif = window->rendererInterface();

        if (rif == nullptr || rif->graphicsApi() != QSGRendererInterface::OpenGL) {
            /*
             * 只有 OpenGL 后端能走这条路：EGLImage 的 dmabuf 导入是 EGL/GL 的能力。
             * Vulkan 后端要另做 VkImage 导入，暂不支持。
             */
            AF_LOGW("Qt scene graph is not using OpenGL (api=%d); the VAAPI/EGL zero-copy "
                    "path needs OpenGL, falling back to the CPU path\n",
                    rif != nullptr ? (int) rif->graphicsApi() : -1);
            m_failed = true;
            return false;
        }

        if (!resolveExtensions(d)) {
            m_failed = true;
            return false;
        }

        AF_LOGI("VAAPI zero-copy is ready: decoded surfaces are converted to BGRA by the "
                "VAAPI video processor, exported as dmabuf and bound to a GL texture that "
                "the Qt scene graph samples directly\n");
        return true;
    }

    /* 释放一个输出缓冲（VAAPI surface / EGLImage / GL 纹理）。 */
    static void releaseBuffer(CicadaTextureVAAPI::Private *d, OutputBuffer &buffer)
    {
        /*
         * GL/EGL 的销毁必须在"有当前上下文"的渲染线程上做。
         *
         * 正常路径是场景图失效（sceneGraphInvalidated，渲染线程）时释放；但 item 在
         * 主线程析构时也会走一遍这里，那时没有 current context —— 直接调
         * glDeleteTextures 是未定义行为。这种情况下跳过：纹理随上下文一起销毁，
         * 不会有跨进程的泄漏。
         */
        const bool hasContext = (QOpenGLContext::currentContext() != nullptr);

        if (buffer.texture != 0 && d->gl != nullptr) {
            if (hasContext) {
                d->gl->glDeleteTextures(1, &buffer.texture);
            } else {
                AF_LOGD("no current OpenGL context: skipping glDeleteTextures "
                        "(the texture goes away with the context)\n");
            }

            buffer.texture = 0;
        }

        if (buffer.image != EGL_NO_IMAGE_KHR && d->eglDestroyImageKHR != nullptr) {
            if (hasContext) {
                d->eglDestroyImageKHR(eglGetCurrentDisplay(), buffer.image);
            }

            buffer.image = EGL_NO_IMAGE_KHR;
        }

        if (buffer.surface != VA_INVALID_SURFACE && d->display != nullptr) {
            /* VAAPI 对象是线程安全的，渲染线程/主线程都能销毁。 */
            vaDestroySurfaces(d->display, &buffer.surface, 1);
            buffer.surface = VA_INVALID_SURFACE;
        }
    }

    void CicadaTextureVAAPI::releaseResources()
    {
        if (d == nullptr) {
            return;
        }

        for (int i = 0; i < kBufferCount; ++i) {
            releaseBuffer(d, d->buffers[i]);
        }

        if (d->context != VA_INVALID_ID && d->display != nullptr) {
            vaDestroyContext(d->display, d->context);
            d->context = VA_INVALID_ID;
        }

        if (d->config != VA_INVALID_ID && d->display != nullptr) {
            vaDestroyConfig(d->display, d->config);
            d->config = VA_INVALID_ID;
        }

        d->display = nullptr;
        d->next = 0;
        d->width = 0;
        d->height = 0;
        d->inputWidth = 0;
        d->inputHeight = 0;
        m_failed = false;
    }

    /*
     * 建 VPP 的 config/context（VADisplay 从中途可能变，所以带 display 参数）。
     *
     * VAProfileNone + VAEntrypointVideoProc 是 VAAPI 里"纯视频处理"的标准组合：
     * 不做解码，只做格式转换/缩放。
     */
    static bool ensureVppContext(CicadaTextureVAAPI::Private *d, VADisplay display,
                                 int width, int height)
    {
        if (d->display == display && d->context != VA_INVALID_ID &&
                d->width == width && d->height == height) {
            return true;
        }

        if (d->display != nullptr && d->display != display) {
            /* 换了 VADisplay：旧的对象都不能用了。 */
            for (int i = 0; i < kBufferCount; ++i) {
                releaseBuffer(d, d->buffers[i]);
            }

            if (d->context != VA_INVALID_ID) {
                vaDestroyContext(d->display, d->context);
                d->context = VA_INVALID_ID;
            }

            if (d->config != VA_INVALID_ID) {
                vaDestroyConfig(d->display, d->config);
                d->config = VA_INVALID_ID;
            }
        }

        d->display = display;

        if (d->config == VA_INVALID_ID) {
            VAConfigAttrib attrib = {};
            attrib.type = VAConfigAttribRTFormat;
            attrib.value = VA_RT_FORMAT_YUV420;

            VAStatus status = vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc,
                                             &attrib, 1, &d->config);

            if (status != VA_STATUS_SUCCESS) {
                /*
                 * 有的驱动不接受显式指定 RTFormat：退一步不带属性再试一次。
                 * 两次都失败说明这台机器的 VPP 用不了，交给 CPU 回退。
                 */
                status = vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc,
                                        nullptr, 0, &d->config);
            }

            if (status != VA_STATUS_SUCCESS) {
                AF_LOGW("vaCreateConfig(VideoProc) failed: %s\n", vaErrorStr(status));
                return false;
            }
        }

        if (d->context == VA_INVALID_ID || d->width != width || d->height != height) {
            if (d->context != VA_INVALID_ID) {
                vaDestroyContext(display, d->context);
                d->context = VA_INVALID_ID;
            }

            const VAStatus status = vaCreateContext(display, d->config, width, height,
                                                    VA_PROGRESSIVE, nullptr, 0, &d->context);

            if (status != VA_STATUS_SUCCESS) {
                AF_LOGW("vaCreateContext failed: %s\n", vaErrorStr(status));
                return false;
            }
        }

        d->width = width;
        d->height = height;
        return true;
    }

    /* 建一个输出 surface 并把它导出成 dmabuf、绑到 GL 纹理上。 */
    static bool createOutputBuffer(CicadaTextureVAAPI::Private *d, OutputBuffer &buffer,
                                   int width, int height)
    {
        /*
         * 输出 surface 必须显式声明"要能以 DRM PRIME 2 的方式导出"，否则
         * vaExportSurfaceHandle 会失败（驱动不会为你保留可导出的内存）。
         */
        VASurfaceAttrib attribs[2] = {};
        attribs[0].type = VASurfaceAttribMemoryType;
        attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
        attribs[0].value.type = VAGenericValueTypeInteger;
        attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
        attribs[1].type = VASurfaceAttribPixelFormat;
        attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
        attribs[1].value.type = VAGenericValueTypeInteger;
        /*
         * 单平面 32bit 打包 RGB。这里填的是 **fourcc**（不是 RT format）：
         * VA_FOURCC_BGRA 对应内存里的 B,G,R,A 字节序，EGL 导入后就是一张普通
         * GL_TEXTURE_2D，Qt 的默认材质可以直接采样。个别驱动只认 VA_FOURCC_ARGB，
         * 那种情况把这里换掉即可（画面颜色顺序也会跟着变）。
         */
        attribs[1].value.value.i = VA_FOURCC_BGRA;

        VAStatus status = vaCreateSurfaces(d->display, VA_RT_FORMAT_RGB32, width, height,
                                           &buffer.surface, 1, attribs, 2);

        if (status != VA_STATUS_SUCCESS) {
            AF_LOGW("vaCreateSurfaces(RGB32) failed: %s\n", vaErrorStr(status));
            return false;
        }

        /* 导出成 dmabuf（只导出句柄，不搬数据）。 */
        VADRMPRIMESurfaceDescriptor desc = {};
        status = vaExportSurfaceHandle(d->display, buffer.surface,
                                       VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                       VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                                       &desc);

        if (status != VA_STATUS_SUCCESS) {
            AF_LOGW("vaExportSurfaceHandle failed: %s\n", vaErrorStr(status));
            return false;
        }

        if (desc.num_layers != 1 || desc.layers[0].num_planes != 1) {
            /* BGRA 应该是单层单平面；多平面的情况（驱动给了别的布局）这里不支持。 */
            AF_LOGW("unexpected dmabuf layout (layers=%u planes=%u)\n",
                    desc.num_layers, desc.layers[0].num_planes);

            /* 导出出来的 fd 是我们持有的，失败路径上也要还回去，否则每次重试漏一个 fd。 */
            for (uint32_t i = 0; i < desc.num_objects; ++i) {
                close(desc.objects[i].fd);
            }

            return false;
        }

        /*
         * EGL：把 dmabuf 包成 EGLImage。
         *
         * 关键属性是 fourcc + offset + pitch（+ 支持时还有 modifier）—— 少一个、或者
         * 和驱动实际布局不一致，eglCreateImageKHR 就会失败（这也是这条路上最容易出
         * 问题的地方，所以全部属性都按 description 里的原值填）。
         */
        EGLint imageAttribs[16] = {};
        int n = 0;
        imageAttribs[n++] = EGL_WIDTH;
        imageAttribs[n++] = width;
        imageAttribs[n++] = EGL_HEIGHT;
        imageAttribs[n++] = height;
        imageAttribs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
        imageAttribs[n++] = static_cast<EGLint>(desc.layers[0].fourcc);
        imageAttribs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;
        imageAttribs[n++] = desc.layers[0].planes[0].fd;
        imageAttribs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
        imageAttribs[n++] = static_cast<EGLint>(desc.layers[0].planes[0].offset);
        imageAttribs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
        imageAttribs[n++] = static_cast<EGLint>(desc.layers[0].planes[0].pitch);

        if (d->hasModifiers) {
            const uint64_t modifier =
                desc.objects[desc.layers[0].planes[0].object_index].drm_format_modifier;
            imageAttribs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
            imageAttribs[n++] = static_cast<EGLint>(modifier & 0xffffffffu);
            imageAttribs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
            imageAttribs[n++] = static_cast<EGLint>(modifier >> 32);
        }

        imageAttribs[n++] = EGL_NONE;

        buffer.image = d->eglCreateImageKHR(eglGetCurrentDisplay(), EGL_NO_CONTEXT,
                                            EGL_LINUX_DMA_BUF_EXT, nullptr, imageAttribs);

        /*
         * 导出完成之后 fd 就可以关掉了：EGLImage 已经持有自己的引用
         * （EGL_LINUX_DMA_BUF_EXT 会 dup）。
         */
        for (uint32_t i = 0; i < desc.num_objects; ++i) {
            close(desc.objects[i].fd);
        }

        if (buffer.image == EGL_NO_IMAGE_KHR) {
            AF_LOGW("eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT) failed (fourcc 0x%08x)\n",
                    desc.layers[0].fourcc);
            return false;
        }

        /* 建 GL 纹理并把 EGLImage 绑上去（GL_TEXTURE_2D：单平面 RGB 可以这么用）。 */
        d->gl->glGenTextures(1, &buffer.texture);
        d->gl->glBindTexture(GL_TEXTURE_2D, buffer.texture);
        d->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, buffer.image);
        d->gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        d->gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        d->gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        d->gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        d->gl->glBindTexture(GL_TEXTURE_2D, 0);

        AF_LOGD("VAAPI output buffer ready: %dx%d fourcc 0x%08x -> EGLImage %p -> GL texture %u\n",
                width, height, desc.layers[0].fourcc, (void *) buffer.image, buffer.texture);
        return true;
    }

    QSGTexture *CicadaTextureVAAPI::textureForFrame(QQuickWindow *window, IAFFrame *frame,
            bool *flipVertically)
    {
        if (flipVertically != nullptr) {
            /*
             * OpenGL 的纹理原点在左下，而视频数据是从左上开始的第一行，
             * 所以采样时要上下翻转。
             */
            *flipVertically = true;
        }

        if (m_failed || frame == nullptr || window == nullptr) {
            return nullptr;
        }

        if (frame->getInfo().video.format != AF_PIX_FMT_VAAPI) {
            /* 不是 VAAPI 硬解帧（软解 / 别的 hwaccel）：交给 CPU 路径。 */
            return nullptr;
        }

        auto *avFrameWrapper = dynamic_cast<AVAFFrame *>(frame);

        if (avFrameWrapper == nullptr) {
            return nullptr;
        }

        AVFrame *avFrame = avFrameWrapper->ToAVFrame();

        if (avFrame == nullptr || avFrame->hw_frames_ctx == nullptr) {
            return nullptr;
        }

        /*
         * VAAPI 帧的布局（libavutil/hwcontext_vaapi.c）：
         *   data[3] = VASurfaceID
         * 设备（VADisplay）在 hw_frames_ctx->device_ref->data 的 AVVAAPIDeviceContext 里。
         */
        const VASurfaceID inputSurface =
            static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(avFrame->data[3]));

        if (inputSurface == VA_INVALID_SURFACE) {
            return nullptr;
        }

        auto *framesCtx = reinterpret_cast<AVHWFramesContext *>(avFrame->hw_frames_ctx->data);

        if (framesCtx == nullptr || framesCtx->device_ref == nullptr) {
            return nullptr;
        }

        auto *deviceCtx = reinterpret_cast<AVHWDeviceContext *>(framesCtx->device_ref->data);

        if (deviceCtx == nullptr || deviceCtx->hwctx == nullptr) {
            return nullptr;
        }

        auto *vaapiDevice = static_cast<AVVAAPIDeviceContext *>(deviceCtx->hwctx);
        VADisplay display = vaapiDevice->display;

        if (display == nullptr) {
            return nullptr;
        }

        /* 显示尺寸（裁剪后）优先，拿不到就用纹理尺寸。 */
        int width = frame->getInfo().video.width;
        int height = frame->getInfo().video.height;

        if (width <= 0 || height <= 0) {
            width = static_cast<int>(framesCtx->width);
            height = static_cast<int>(framesCtx->height);
        }

        if (width <= 0 || height <= 0) {
            return nullptr;
        }

        if (!ensureVppContext(d, display, width, height)) {
            m_failed = true;
            return nullptr;
        }

        /* 按需建/重建输出缓冲（尺寸变了就全部重建）。 */
        if (d->inputWidth != width || d->inputHeight != height) {
            for (int i = 0; i < kBufferCount; ++i) {
                releaseBuffer(d, d->buffers[i]);
            }

            for (int i = 0; i < kBufferCount; ++i) {
                if (!createOutputBuffer(d, d->buffers[i], width, height)) {
                    m_failed = true;
                    return nullptr;
                }
            }

            d->inputWidth = width;
            d->inputHeight = height;
        }

        OutputBuffer &buffer = d->buffers[d->next];
        d->next = (d->next + 1) % kBufferCount;

        /*
         * VPP：NV12 输入 surface -> BGRA 输出 surface。
         *
         * 参数来自 VAProcPipelineParameterBuffer：surface 是输入，output_region 决定
         * 输出区域（留空表示整幅），surface_region 决定裁掉编码对齐的填充行。
         */
        VABufferID pipelineBuffer = VA_INVALID_ID;
        VAProcPipelineParameterBuffer params = {};
        params.surface = inputSurface;
        params.surface_region = nullptr; /* 整幅输入 */
        params.output_region = nullptr;  /* 整幅输出 */
        params.output_background_color = 0xff000000;
        params.filter_flags = VA_FILTER_SCALING_DEFAULT;

        VAStatus status = vaCreateBuffer(d->display, d->context, VAProcPipelineParameterBufferType,
                                         sizeof(params), 1, &params, &pipelineBuffer);

        if (status != VA_STATUS_SUCCESS) {
            AF_LOGW("vaCreateBuffer(VAProcPipelineParameterBufferType) failed: %s\n",
                    vaErrorStr(status));
            return nullptr;
        }

        status = vaBeginPicture(d->display, d->context, buffer.surface);

        if (status == VA_STATUS_SUCCESS) {
            status = vaRenderPicture(d->display, d->context, &pipelineBuffer, 1);
        }

        if (status == VA_STATUS_SUCCESS) {
            status = vaEndPicture(d->display, d->context);
        }

        vaDestroyBuffer(d->display, pipelineBuffer);

        if (status != VA_STATUS_SUCCESS) {
            AF_LOGW("VAAPI VPP failed: %s\n", vaErrorStr(status));
            return nullptr;
        }

        /* 等 VPP 写完再交给 Qt 采样（同步一次，避免画面撕裂/花屏）。 */
        status = vaSyncSurface(d->display, buffer.surface);

        if (status != VA_STATUS_SUCCESS) {
            AF_LOGW("vaSyncSurface failed: %s\n", vaErrorStr(status));
            return nullptr;
        }

        if (!m_loggedFirstFrame) {
            m_loggedFirstFrame = true;
            AF_LOGI("first zero-copy frame: %dx%d VAAPI surface -> VPP BGRA -> dmabuf -> "
                    "EGLImage -> GL texture %u -> Qt\n", width, height, buffer.texture);
        }

        /*
         * 交给 Qt（具体的包装方式按 Qt 版本分两条路，见 CicadaQtTextureWrap）。
         *
         * 纹理的所有权：Qt 不会删除这个外部 GL 纹理（它不知道上下文），所以由我们在
         * releaseResources()/重建时 glDeleteTextures 释放。
         */
        QSGTexture *qsTexture = wrapGLTexture(window, buffer.texture, QSize(width, height));

        return qsTexture;
    }

#else// !CICADA_QT_HAVE_VAAPI

    CicadaTextureVAAPI::CicadaTextureVAAPI() = default;
    CicadaTextureVAAPI::~CicadaTextureVAAPI() = default;

    bool CicadaTextureVAAPI::prepare(QQuickWindow *window)
    {
        (void) window;
        AF_LOGW("this build has no VAAPI/EGL support (libva or EGL headers were missing at "
                "configure time); video goes through the CPU path\n");
        m_failed = true;
        return false;
    }

    QSGTexture *CicadaTextureVAAPI::textureForFrame(QQuickWindow *window, IAFFrame *frame,
            bool *flipVertically)
    {
        (void) window;
        (void) frame;
        (void) flipVertically;
        return nullptr;
    }

    void CicadaTextureVAAPI::releaseResources()
    {
    }

#endif// CICADA_QT_HAVE_VAAPI

}// namespace cicadaqt

#endif// Q_OS_LINUX
