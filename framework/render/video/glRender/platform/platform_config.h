#ifndef __PLATFORM_CONFIG_H__
#define __PLATFORM_CONFIG_H__

#define PLATFORM_UNKNOWN            0
#define PLATFORM_IOS                1
#define PLATFORM_ANDROID            2
#define PLATFORM_WIN32              3
#define PLATFORM_LINUX              4
#define PLATFORM_MAC                5
#define PLATFORM_OHOS               6

// Apple: Mac and iOS
#if defined(__APPLE__) && !defined(ANDROID)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#undef  TARGET_PLATFORM
#define TARGET_PLATFORM         PLATFORM_IOS
#else
#undef  TARGET_PLATFORM
#define  TARGET_PLATFORM        PLATFORM_MAC
#endif
#endif

// android
#if defined(ANDROID)
#undef  TARGET_PLATFORM
#define TARGET_PLATFORM         PLATFORM_ANDROID
#endif

// win32
#if defined(_WIN32) || defined(_WINDOWS)
#undef  TARGET_PLATFORM
#define TARGET_PLATFORM         PLATFORM_WIN32
#endif

// linux
#if defined(LINUX) && !defined(__APPLE__)
#undef  TARGET_PLATFORM
#define TARGET_PLATFORM         PLATFORM_LINUX
#endif

// harmonyos / openharmony
//
// 放在最后：__OHOS__ 由构建系统给出（framework/HarmonyOS.cmake 里的 -D__OHOS__），
// 万一别的工具链同时也定义了 LINUX，这里也能把 TARGET_PLATFORM 纠正回 OHOS。
#if defined(__OHOS__)
#undef  TARGET_PLATFORM
#define TARGET_PLATFORM         PLATFORM_OHOS
#endif

// check user set platform
#if ! TARGET_PLATFORM
#error  "Cannot recognize the target platform; are you targeting an unsupported platform?"
#endif

#if (TARGET_PLATFORM == PLATFORM_WIN32)
#ifndef __MINGW32__
#pragma warning (disable:4127)
#endif
#endif  // PLATFORM_WIN32

// 手机/平板一类的触摸设备：OHOS 也要算进来，否则会落到 PLATFORM_PC 那边。
#if ((TARGET_PLATFORM == PLATFORM_ANDROID) || (TARGET_PLATFORM == PLATFORM_IOS) \
     || (TARGET_PLATFORM == PLATFORM_OHOS))
#define PLATFORM_MOBILE
#else
#define PLATFORM_PC
#endif


#endif  // __PLATFORM_CONFIG_H__
