//
// platform/ohos/ohos_native_window.cpp
//
// 见 ohos_native_window.h。这里只在 OHOS 上编译出实体（CMake 只在 OHOS 分支里
// glob platform/ohos/*，这里再包一层平台判断，双保险）。
//

#include "platform/platform_config.h"

#if TARGET_PLATFORM == PLATFORM_OHOS

#include "ohos_native_window.h"

namespace Cicada
{

void OhosNativeWindow::Retain() const
{
    if (mWindow != nullptr) {
        OH_NativeWindow_NativeObjectReference(mWindow);
    }
}

void OhosNativeWindow::Release() const
{
    if (mWindow != nullptr) {
        OH_NativeWindow_NativeObjectUnreference(mWindow);
    }
}

bool OhosNativeWindow::QuerySize(int &width, int &height) const
{
    width = 0;
    height = 0;

    if (mWindow == nullptr) {
        return false;
    }

    int32_t first = 0;
    int32_t second = 0;

    if (OH_NativeWindow_NativeWindowHandleOpt(mWindow, GET_BUFFER_GEOMETRY, &first, &second) != 0) {
        return false;
    }

    if (first <= 0 || second <= 0) {
        return false;
    }

    width = first;
    height = second;
    return true;
}

} // namespace Cicada

#endif // TARGET_PLATFORM == PLATFORM_OHOS
