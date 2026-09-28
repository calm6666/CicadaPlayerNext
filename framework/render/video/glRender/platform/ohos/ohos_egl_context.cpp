//
// platform/ohos/ohos_egl_context.cpp
//
// 见 ohos_egl_context.h。
//
// 失败处理原则：EGL 初始化、建表面任何一步失败都只打一条明确的 E 日志并返回错误码，
// 让 GLRender 走它本来就有的"没有表面就不画"的路径（丢帧、不 Present），
// 不阻塞调用线程、不崩溃。
//

#include "platform/platform_config.h"

#if TARGET_PLATFORM == PLATFORM_OHOS

#include "ohos_egl_context.h"

#include "ohos_native_window.h"
#include <render/video/glRender/base/error_event_def.h>
#include <utils/frame_work_log.h>

namespace
{
/*
 * eglGetError() 返回 EGLint（有符号），日志里按十六进制打出来，这里统一转成
 * unsigned int，让格式串与实参类型严格对齐。
 */
unsigned int eglErrorCode()
{
    return static_cast<unsigned int>(eglGetError());
}
}

namespace Cicada
{

int CicadaOhosEGLContext::Init(void *sharedContext)
{
    if (sharedContext == nullptr) {
        sharedContext = EGL_NO_CONTEXT;
    }

    mDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);

    if (mDisplay == EGL_NO_DISPLAY) {
        AF_LOGE("OHOS EGL: eglGetDisplay failed\n");
        return CICADA_FRAMEWORK_RENDER_ERROR_GL_CONTEXT_INIT_FAILED;
    }

    if (eglInitialize(mDisplay, &mMajor, &mMinor) != EGL_TRUE) {
        AF_LOGE("OHOS EGL: eglInitialize failed, error 0x%x\n", eglErrorCode());
        return CICADA_FRAMEWORK_RENDER_ERROR_GL_CONTEXT_INIT_FAILED;
    }

    AF_LOGI("OHOS EGL: client version %d.%d\n", mMajor, mMinor);

    mConfig = chooseBestConfig();

    if (mConfig == nullptr) {
        AF_LOGE("OHOS EGL: no suitable EGLConfig found\n");
        return CICADA_FRAMEWORK_RENDER_ERROR_GL_CONTEXT_INIT_FAILED;
    }

    /* 先要 ES3，拿不到再退 ES2（与 Android 版一致）。 */
    const EGLint contextAttribs3[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    mContext = eglCreateContext(mDisplay, mConfig, sharedContext, contextAttribs3);

    if (mContext == EGL_NO_CONTEXT) {
        AF_LOGW("OHOS EGL: ES3 context unavailable, trying ES2\n");
        const EGLint contextAttribs2[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        mContext = eglCreateContext(mDisplay, mConfig, sharedContext, contextAttribs2);
    }

    if (mContext == EGL_NO_CONTEXT) {
        AF_LOGE("OHOS EGL: eglCreateContext failed, error 0x%x\n", eglErrorCode());
        return CICADA_FRAMEWORK_RENDER_ERROR_GL_CONTEXT_INIT_FAILED;
    }

    const EGLint pbufferAttribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    mNoSurface = eglCreatePbufferSurface(mDisplay, mConfig, pbufferAttribs);

    if (mNoSurface == EGL_NO_SURFACE) {
        AF_LOGE("OHOS EGL: eglCreatePbufferSurface failed, error 0x%x\n", eglErrorCode());
        return CICADA_FRAMEWORK_RENDER_ERROR_GL_CONTEXT_INIT_FAILED;
    }

    if (eglMakeCurrent(mDisplay, mNoSurface, mNoSurface, mContext) != EGL_TRUE) {
        AF_LOGE("OHOS EGL: eglMakeCurrent failed, error 0x%x\n", eglErrorCode());
        return CICADA_FRAMEWORK_RENDER_ERROR_GL_CONTEXT_INIT_FAILED;
    }

    AF_LOGI("OHOS EGL: display %p context %p ready\n", mDisplay, mContext);
    return CICADA_COMMON_RETURN_SUCCESS;
}

void CicadaOhosEGLContext::Destroy()
{
    if (mContext != EGL_NO_CONTEXT) {
        eglMakeCurrent(mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        if (mNoSurface != EGL_NO_SURFACE) {
            if (eglDestroySurface(mDisplay, mNoSurface) != EGL_TRUE) {
                AF_LOGE("OHOS EGL: eglDestroySurface(pbuffer) failed, error 0x%x\n", eglErrorCode());
            }
            mNoSurface = EGL_NO_SURFACE;
        }

        eglDestroyContext(mDisplay, mContext);

        if (mMajor >= 1 && mMinor >= 2) {
            eglReleaseThread();
        }

        eglTerminate(mDisplay);
    }

    mDisplay = EGL_NO_DISPLAY;
    mContext = EGL_NO_CONTEXT;
    mConfig = nullptr;
    mWidth = 0;
    mHeight = 0;
    mView = nullptr;
    mViewRef = nullptr;
    mSwapErrorReported = false;
}

void CicadaOhosEGLContext::MakeCurrent(GLSurface *surface)
{
    EGLBoolean rv = EGL_FALSE;

    if (surface == nullptr || surface->surface == nullptr) {
        rv = eglMakeCurrent(mDisplay, mNoSurface, mNoSurface, mContext);
    } else {
        EGLSurface eglSurface = static_cast<EGLSurface>(surface->surface);
        rv = eglMakeCurrent(mDisplay, eglSurface, eglSurface, mContext);
    }

    if (rv != EGL_TRUE) {
        AF_LOGE("OHOS EGL: eglMakeCurrent failed, error 0x%x\n", eglErrorCode());
    }
}

int CicadaOhosEGLContext::Present(const GLSurface *surface)
{
    if (surface == nullptr || surface->surface == nullptr) {
        glFlush();
        return CICADA_COMMON_RETURN_SUCCESS;
    }

    if (eglSwapBuffers(mDisplay, static_cast<EGLSurface>(surface->surface)) != EGL_TRUE) {
        if (!mSwapErrorReported) {
            mSwapErrorReported = true;
            AF_LOGE("OHOS EGL: eglSwapBuffers failed, error 0x%x, this frame is dropped\n",
                    eglErrorCode());
        }
        return CICADA_FRAMEWORK_RENDER_ERROR_GL_SWAP_BUFFER_FAILED;
    }

    return CICADA_COMMON_RETURN_SUCCESS;
}

void CicadaOhosEGLContext::PresentationTime(const GLSurface *surface, int64_t pts)
{
    (void) surface;
    (void) pts;
    /*
     * 什么都不做：画面提交的节拍由 VSync（OHOSVSync）决定，这里没有系统保证的
     * "这一帧该什么时候上屏"的接口可依赖。
     * 说明：eglPresentationTimeANDROID 在 OHOS 的 eglext.h 里有声明，但运行库是否真的
     * 提供这个扩展、语义是否一致都不确定，所以这里不引用它 —— 免得既留一个链接期
     * 风险，又留一个运行期风险。
     */
}

bool CicadaOhosEGLContext::SetView(void *view)
{
    if (mViewRef == view) {
        return false;
    }

    mViewRef = view;
    mView = view;
    AF_LOGI("OHOS EGL: view changed to %p\n", view);
    return true;
}

void CicadaOhosEGLContext::DestroyView()
{
    /*
     * 只丢指针，不销毁窗口：窗口对象属于上层 NAPI（创建在 SetSurface，销毁在 Release
     * 或下一次 SetSurface），这里销毁它是重复释放。
     * 当前窗口表面占的那次引用在 DestroySurface 里还。
     */
    mView = nullptr;
    mViewRef = nullptr;
}

bool CicadaOhosEGLContext::IsViewSizeChanged()
{
    if (mView == nullptr) {
        return false;
    }

    int width = 0;
    int height = 0;
    OhosNativeWindow window(static_cast<OHNativeWindow *>(mView));

    if (!window.QuerySize(width, height)) {
        /* 读不到几何就当没变：宁可少重建一次表面，也不能每帧重建。 */
        return false;
    }

    return !sameSize(width, height, mWidth, mHeight);
}

GLSurface *CicadaOhosEGLContext::CreateSurface()
{
    if (mView == nullptr || mConfig == nullptr || mDisplay == EGL_NO_DISPLAY) {
        return nullptr;
    }

    OhosNativeWindow window(static_cast<OHNativeWindow *>(mView));
    /* 表面存在期间把窗口对象拴住：上层可能在别处销毁它。 */
    window.Retain();

    EGLSurface surface = eglCreateWindowSurface(mDisplay, mConfig,
                          reinterpret_cast<EGLNativeWindowType>(window.Get()), nullptr);

    if (surface == EGL_NO_SURFACE) {
        window.Release();
        AF_LOGE("OHOS EGL: eglCreateWindowSurface failed, error 0x%x\n", eglErrorCode());
        return nullptr;
    }

    auto *s = new GLSurface();
    s->surface = surface;
    /* 记下这块表面绑的是哪个窗口，DestroySurface 时按它把那次引用还掉。 */
    s->nativeWindow = window.Get();
    mSwapErrorReported = false;
    updateSurfaceSize(surface);
    AF_LOGI("OHOS EGL: window surface %p created, size %dx%d\n", surface, mWidth, mHeight);
    return s;
}

void CicadaOhosEGLContext::DestroySurface(GLSurface *&surface)
{
    if (surface == nullptr) {
        return;
    }

    if (mDisplay != EGL_NO_DISPLAY && surface->surface != nullptr) {
        if (mContext != EGL_NO_CONTEXT) {
            eglMakeCurrent(mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }

        if (eglDestroySurface(mDisplay, static_cast<EGLSurface>(surface->surface)) != EGL_TRUE) {
            AF_LOGE("OHOS EGL: eglDestroySurface failed, error 0x%x\n", eglErrorCode());
        }
    }

    /*
     * 表面已经没了，窗口的这次引用才可以还：顺序反过来的话，上层先把窗口对象销毁、
     * 而表面还引用着它。
     */
    if (surface->nativeWindow != nullptr) {
        OhosNativeWindow window(static_cast<OHNativeWindow *>(surface->nativeWindow));
        window.Release();
    }

    delete surface;
    surface = nullptr;
}

int CicadaOhosEGLContext::GetVisualFormat()
{
    EGLint format = 0;

    if (eglGetConfigAttrib(mDisplay, mConfig, EGL_NATIVE_VISUAL_ID, &format) != EGL_TRUE) {
        AF_LOGE("OHOS EGL: eglGetConfigAttrib(EGL_NATIVE_VISUAL_ID) failed, error 0x%x\n",
                eglErrorCode());
        return 0;
    }

    return format;
}

int CicadaOhosEGLContext::GetWidth()
{
    return mWidth;
}

int CicadaOhosEGLContext::GetHeight()
{
    return mHeight;
}

void *CicadaOhosEGLContext::GetContext()
{
    return mContext;
}

int CicadaOhosEGLContext::GetGLVersion()
{
    /* 与 Android 版一致：对外按 ES2 上报（YUV 程序用的都是 ES2 级别的接口）。 */
    return kOpenGLES2_0;
}

EGLConfig CicadaOhosEGLContext::chooseBestConfig()
{
    const EGLint attribList[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_NONE
    };
    EGLConfig configs[1] = {nullptr};
    EGLint numConfig = 0;

    if (eglChooseConfig(mDisplay, attribList, configs, 1, &numConfig) != EGL_TRUE || numConfig < 1) {
        AF_LOGE("OHOS EGL: eglChooseConfig failed, error 0x%x\n", eglErrorCode());
        return nullptr;
    }

    return configs[0];
}

void CicadaOhosEGLContext::updateSurfaceSize(EGLSurface surface)
{
    EGLint width = 0;
    EGLint height = 0;

    if (eglQuerySurface(mDisplay, surface, EGL_WIDTH, &width) == EGL_TRUE
        && eglQuerySurface(mDisplay, surface, EGL_HEIGHT, &height) == EGL_TRUE
        && width > 0 && height > 0) {
        mWidth = width;
        mHeight = height;
        return;
    }

    int windowWidth = 0;
    int windowHeight = 0;

    if (mView != nullptr
        && OhosNativeWindow(static_cast<OHNativeWindow *>(mView)).QuerySize(windowWidth, windowHeight)) {
        mWidth = windowWidth;
        mHeight = windowHeight;
        return;
    }

    /*
     * 两条路都拿不到尺寸就保持 0：上层拿到 0 时不会画（YUVProgramContext 的
     * updateDrawRegion / updateUProjection 对 0 尺寸都有判据），也不会除零。
     */
    mWidth = 0;
    mHeight = 0;
}

bool CicadaOhosEGLContext::sameSize(int width, int height, int cachedWidth, int cachedHeight)
{
    return (width == cachedWidth && height == cachedHeight)
           || (width == cachedHeight && height == cachedWidth);
}

} // namespace Cicada

#endif // TARGET_PLATFORM == PLATFORM_OHOS
