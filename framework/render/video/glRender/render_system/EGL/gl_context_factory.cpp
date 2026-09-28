#include "gl_context_factory.h"
#include <platform/platform_gl.h>
#if TARGET_PLATFORM == PLATFORM_IOS
    #include "platform/ios/eagl_context.h"
#elif TARGET_PLATFORM == PLATFORM_MAC
    #include "platform/mac/mgl_context.h"
#elif TARGET_PLATFORM == PLATFORM_WIN32
    #include "platform/window/wgl_context.h"
#elif TARGET_PLATFORM == PLATFORM_OHOS
    #include "platform/ohos/ohos_egl_context.h"
#else
    #include "egl_context.h"
#endif

namespace Cicada {
    GLContext *GLContextFactory::NewInstance()
    {
#if TARGET_PLATFORM == PLATFORM_IOS
        return new CicadaEAGLContext();
#elif TARGET_PLATFORM == PLATFORM_ANDROID
        return new CicadaEGLContext();
#elif TARGET_PLATFORM == PLATFORM_MAC
        return new CicadaMGLContext();
#elif TARGET_PLATFORM == PLATFORM_WIN32
        return new CicadaWGLContext();
#elif TARGET_PLATFORM == PLATFORM_OHOS
        /*
         * 鸿蒙：EGL + GLES3，窗口是 OHNativeWindow。
         * 必须返回实体：GLRender::VSyncOnInit 会直接对返回值调 Init()，
         * 这里返回 nullptr 会空指针解引用。
         */
        return new CicadaOhosEGLContext();
#else
        return nullptr;
#endif
    }

} // namespace Cicada

