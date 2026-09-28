#ifndef PLATFORM_GL_H
#define PLATFORM_GL_H
#include "platform_config.h"
#include <utils/frame_work_log.h>

#if TARGET_PLATFORM == PLATFORM_IOS
#include <OpenGLES/ES3/gl.h>
#include <OpenGLES/ES3/glext.h>
#define GL_ES
#elif TARGET_PLATFORM == PLATFORM_ANDROID
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "android/gl3stub.h"
#define GL_ES
#elif TARGET_PLATFORM == PLATFORM_MAC
    #include <OpenGL/gl.h>
    #include <OpenGL/glext.h>
#undef GL_ES
#elif TARGET_PLATFORM == PLATFORM_WIN32
#include <windows.h>
#include <gl/glew.h>
#include <gl/wglew.h>
#undef GL_ES
#elif TARGET_PLATFORM == PLATFORM_OHOS
    /*
     * 鸿蒙（OHOS NDK）：
     *   · eglplatform.h 用 OHOS_PLATFORM 这个宏来挑它自己的平台分支，
     *     而 ohos.toolchain.cmake 并不定义它（只定义 __MUSL__），所以这里自己补上：
     *     这样 EGLNativeWindowType 就是 struct NativeWindow*，与 OHNativeWindow 同一个
     *     类型，不依赖编译器是否预定义了 __unix__。
     *   · GLES 走真实头文件：OHOS 的 sysroot 里有 GLES3/gl3.h，桩库 libGLESv3.so 也在，
     *     不需要 Android 那套 gl3stub 动态加载。
     */
    #ifndef OHOS_PLATFORM
    #define OHOS_PLATFORM
    #endif
    #include <EGL/egl.h>
    #include <EGL/eglext.h>
    #include <GLES3/gl3.h>
#define GL_ES
#endif

#define GL_TEXTURE_EXTERNAL_OES 0x8D65
namespace cicada
{

#if defined(NDEBUG)
#define CHECK_GL_ERROR_DEBUG()
#else
#define CHECK_GL_ERROR_DEBUG() \
    do { \
        GLenum __error = glGetError(); \
        if(__error) { \
            AF_LOGE("OpenGL error 0x%04X in %s %s %d\n", __error, __FILE__, __FUNCTION__, __LINE__); \
        } \
    } while (false)
#endif


} // namespace cicada
#endif // PLATFORM_GL_H
