//
// Linux 零拷贝后端：FFmpeg VAAPI 的 VASurface -> dmabuf -> EGLImage -> GL 纹理 ->
// 包成 QSGTexture 交给 Qt 场景图。
//
// 完整链路（每一步都在 GPU 上，没有 CPU 拷贝）
// ------------------------------------------
//   1. 解码：框架在 Linux 上走 FFmpeg 的 VAAPI hwaccel，帧是 AV_PIX_FMT_VAAPI，
//      VASurfaceID 在 AVFrame::data[3]（GPU 上解码，不下载）；
//   2. 转换：用 **VAAPI VPP**（vaCreateConfig(VAProfileNone/VAEntrypointVideoProc)
//      + vaRenderPicture）把 NV12 的输入 surface 转成 **BGRA** 的输出 surface。
//      为什么先转 BGRA 而不是直接导 NV12：Qt 的默认材质只会采样"一张普通 2D 纹理"，
//      而 NV12 的 dmabuf 只能用 GL_TEXTURE_EXTERNAL_OES 采样（需要自定义
//      QSGMaterial + samplerExternalOES 着色器）。转成单平面 BGRA 之后，dmabuf 可以
//      直接绑成 GL_TEXTURE_2D，Qt 的默认材质就能用 —— 少一个易碎环节，同样零拷贝。
//      （VPP 的输出格式支持性会先探测：不支持就返回失败，上层自动走 CPU 回退。）
//   3. 导出：vaExportSurfaceHandle(..., VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)
//      拿到 VADRMPRIMESurfaceDescriptor（fd / offset / pitch / fourcc），只导出句柄；
//   4. EGL：eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT, ...) 得到 EGLImage，
//      glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image) 绑到一张 GL 纹理上；
//   5. Qt：QQuickWindow::createTextureFromNativeObject(NativeObjectTexture,
//      (void*)(uintptr_t)glTextureId, 0, size)。
//
// 双缓冲：VPP 写第 N 帧的时候，第 N-1 帧可能还在被 Qt 采样，所以输出 surface /
// EGLImage / GL 纹理各准备两份轮换（见 kBufferCount）。
//
// 前置条件
// --------
//   * Qt 场景图必须跑在 **OpenGL** 上（EGL/GL 这条路要求）。如果是 Vulkan/其它后端，
//     prepare() 返回 false，走 CPU 回退（要支持 Vulkan 得走 VkImage 的 dmabuf 导入，
//     复杂度高得多，暂不做）；
//   * libva（含 va_drmcommon.h）+ EGL 头文件。CMake 里用 Linux.cmake 探测的
//     CICADA_VAAPI_LIBS 和 EGL 库；缺任一依赖时 CMake 不会定义
//     CICADA_QT_HAVE_VAAPI，本文件整体退化成"不可用"，工程照样能编能跑（CPU 路径）。
//
// 线程约定：所有 GL/EGL/VAAPI 调用都只能在 Qt 的**渲染线程**上做（updatePaintNode
// 里，那时 GL 上下文是 current 的）。
//
#ifndef CICADA_QT_CICADATEXTUREVAAPI_H
#define CICADA_QT_CICADATEXTUREVAAPI_H

#include <QtCore/QString>

class QQuickWindow;
class QSGTexture;
class IAFFrame;

namespace cicadaqt {

    class CicadaTextureVAAPI {
    public:
        CicadaTextureVAAPI();
        ~CicadaTextureVAAPI();

        CicadaTextureVAAPI(const CicadaTextureVAAPI &) = delete;
        CicadaTextureVAAPI &operator=(const CicadaTextureVAAPI &) = delete;

        /* 返回 false = 零拷贝不可用，走 CPU 回退。 */
        bool prepare(QQuickWindow *window);

        QSGTexture *textureForFrame(QQuickWindow *window, IAFFrame *frame, bool *flipVertically);

        void releaseResources();

        const char *backendName() const
        {
            return "VAAPI zero-copy (surface -> VPP BGRA -> dmabuf -> EGLImage -> GL texture)";
        }

        /*
         * 实现细节（PIMPL）。
         *
         * 放在 public 区是因为：结构体本身在 .cpp 里定义，而 .cpp 里还有几个文件内
         * 的自由辅助函数要拿它的指针 —— 私有嵌套类型在类外既不能定义也不能命名
         * （会撞访问控制）。约定：**外部不要碰它**。
         */
        struct Private;
        Private *d = nullptr;

    private:
        bool m_failed = false;
        bool m_loggedFirstFrame = false;
        /* 没有编译进 VAAPI/EGL 支持时（缺依赖）直接走 CPU 路径。 */
        bool m_available = true;
    };

}// namespace cicadaqt

#endif// CICADA_QT_CICADATEXTUREVAAPI_H
