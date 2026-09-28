//
// platform/ohos/ohos_egl_context.h
//
// 鸿蒙（OHOS）的 EGL 上下文实现：与 render_system/EGL/egl_context.h 里的 Android 版本
// 一一对应，只是把"窗口"从 ANativeWindow 换成 OHNativeWindow，并用真实存在的
// GLES3 头文件（OHOS sysroot 自带），不需要 Android 那套 gl3stub 动态加载。
//
// 窗口从哪来：
//   ArkTS 侧把 XComponent 的 surfaceId 交给 NAPI（napi_player.cpp 的 SetSurface），
//   NAPI 用 OH_NativeWindow_CreateNativeWindowFromSurfaceId 换成 OHNativeWindow*，
//   再经 CicadaSetView -> IVideoRender::setDisPlay 一路传到 GLRender，GLRender 每帧
//   调用本类的 SetView()/IsViewSizeChanged()，需要时 CreateSurface()。
//   这个指针是借的，本类绝不销毁窗口对象（见 DestroyView 的说明）。
//
// 上屏路径：CreateSurface() 用 eglCreateWindowSurface 在该窗口上建 EGL 窗口表面，
// renderActually() 画完后 Present() -> eglSwapBuffers 提交一帧。
//

#ifndef CICADA_GLRENDER_OHOS_EGL_CONTEXT_H
#define CICADA_GLRENDER_OHOS_EGL_CONTEXT_H

#include "platform/platform_config.h"

#if TARGET_PLATFORM == PLATFORM_OHOS

#include "platform/platform_gl.h"
#include "render_system/EGL/gl_context.h"

namespace Cicada
{

class CicadaOhosEGLContext : public GLContext
{
public:
    CicadaOhosEGLContext() = default;

    ~CicadaOhosEGLContext() override = default;

    int Init(void *sharedContext) override;

    void Destroy() override;

    void MakeCurrent(GLSurface *surface) override;

    int Present(const GLSurface *surface) override;

    void PresentationTime(const GLSurface *surface, int64_t pts) override;

    bool SetView(void *view) override;

    void DestroyView() override;

    bool IsViewSizeChanged() override;

    GLSurface *CreateSurface() override;

    void DestroySurface(GLSurface *&surface) override;

    int GetVisualFormat() override;

    int GetWidth() override;

    int GetHeight() override;

    void *GetContext() override;

    int GetGLVersion() override;

private:
    EGLConfig chooseBestConfig();

    /* 先问 EGL 表面要真实尺寸，问不到再退回窗口缓冲几何；都拿不到就保持 0。 */
    void updateSurfaceSize(EGLSurface surface);

    /* 宽高按无序对比较：GET_BUFFER_GEOMETRY 的输出顺序在头文件里写法不一致。 */
    static bool sameSize(int width, int height, int cachedWidth, int cachedHeight);

private:
    EGLContext mContext{EGL_NO_CONTEXT};
    EGLDisplay mDisplay{EGL_NO_DISPLAY};
    EGLConfig mConfig{nullptr};
    /* 没有窗口表面时用来承载上下文的 1x1 pbuffer。 */
    EGLSurface mNoSurface{EGL_NO_SURFACE};
    EGLint mMajor{0};
    EGLint mMinor{0};
    int mWidth{0};
    int mHeight{0};
    /* mView 是借来的 OHNativeWindow 指针；mViewRef 只用来识别"上层换了窗口"。 */
    void *mView{nullptr};
    void *mViewRef{nullptr};
    /*
     * Present() 每帧都被调用，eglSwapBuffers 一旦失败会每帧失败：
     * 只记第一条，避免日志被同一件事刷满。建出新表面后会复位。
     */
    bool mSwapErrorReported{false};
};

} // namespace Cicada

#endif // TARGET_PLATFORM == PLATFORM_OHOS

#endif // CICADA_GLRENDER_OHOS_EGL_CONTEXT_H
