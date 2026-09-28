//
// platform/ohos/ohos_native_window.h
//
// OHNativeWindow（鸿蒙里的 struct NativeWindow）的极薄包装，供 glRender 的 OHOS 平台层用。
// 与 platform/android/native_window.h 的位置和职责对应，区别在于所有权的处理：
//
//   · 只借指针，不接管所有权 —— 窗口对象是上层（NAPI 侧 SetSurface）用
//     OH_NativeWindow_CreateNativeWindowFromSurfaceId 创建、用
//     OH_NativeWindow_DestroyNativeWindow 销毁的，渲染器自己销毁它会重复释放。
//   · Retain()/Release() 直接对应 OH_NativeWindow_NativeObjectReference /
//     OH_NativeWindow_NativeObjectUnreference：官方文档给的用法就是"存在并发销毁时，
//     引用计数加一减一成对使用"。渲染器在"EGL 窗口表面还引用着这块窗口"的这段时间里
//     把窗口对象拴住，表面销毁后立刻还回去。
//

#ifndef CICADA_GLRENDER_OHOS_NATIVE_WINDOW_H
#define CICADA_GLRENDER_OHOS_NATIVE_WINDOW_H

#include "platform/platform_config.h"

#if TARGET_PLATFORM == PLATFORM_OHOS

#include <native_window/external_window.h>

namespace Cicada
{

class OhosNativeWindow
{
public:
    explicit OhosNativeWindow(OHNativeWindow *window): mWindow(window) {}

    ~OhosNativeWindow() = default;

    /* 借来的指针：谁传进来谁负责，这里不释放。 */
    OHNativeWindow *Get() const
    {
        return mWindow;
    }

    void Retain() const;

    void Release() const;

    /*
     * 读一次当前缓冲几何，宽和高都大于 0 才算成功。
     *
     * 头文件里 GET_BUFFER_GEOMETRY 的输出参数顺序与 SET_BUFFER_GEOMETRY 的输入参数
     * 顺序写法并不一致，所以这里只保证"两个数值都拿到了"，调用方比较尺寸时按
     * 无序对比较（见 ohos_egl_context.cpp 的 sameSize）。
     */
    bool QuerySize(int &width, int &height) const;

private:
    OHNativeWindow *mWindow{nullptr};
};

} // namespace Cicada

#endif // TARGET_PLATFORM == PLATFORM_OHOS

#endif // CICADA_GLRENDER_OHOS_NATIVE_WINDOW_H
