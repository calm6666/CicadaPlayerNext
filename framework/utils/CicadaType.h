//
//  CicadaType.h
//
//  Created by huang_jiafa on 2019/3/6.
//  Copyright (c) 2019 Aliyun. All rights reserved.
//

#ifndef Cicada_H
#define Cicada_H

#if defined(_MSC_VER)
  #ifndef attribute_deprecated
  #define attribute_deprecated __declspec(deprecated)
  #endif
  #define CICADA_CPLUS_EXTERN __declspec(dllexport)
#elif defined _WIN32 || defined __CYGWIN__
  #ifndef attribute_deprecated
  #define attribute_deprecated __attribute__((deprecated))
  #endif
  #ifdef __GNUC__
    #define CICADA_CPLUS_EXTERN __attribute__ ((dllexport))
  #else
    #define CICADA_CPLUS_EXTERN __declspec(dllexport) // Note: actually gcc seems to also supports this syntax.
  #endif
#else
  #if __GNUC__ >= 4
    #define CICADA_CPLUS_EXTERN __attribute__((visibility ("default")))
    /*
     * 取值保持工程原样：ffmpeg 自己的两份头文件对 attribute_deprecated 的取值都不同
     * （external/install/.../attributes.h 在 C++ 下用 [[deprecated]]，而 vendored 的
     * external/external/ffmpeg/... 用编译器原生写法），我们跟着任何一边改都会让另一边
     * 报"宏重定义"。这是既有的、来自第三方头的告警（Windows C4005 / macOS
     * -Wmacro-redefined），不作为本工程的代码问题处理。
     */
    #ifndef attribute_deprecated
    #define attribute_deprecated __attribute__((deprecated))
    #endif
  #else
    #define CICADA_CPLUS_EXTERN
    #ifndef attribute_deprecated
    #define attribute_deprecated
    #endif
  #endif
#endif

#ifdef __GNUC__
#    define AV_GCC_VERSION_AT_LEAST(x,y) (__GNUC__ > (x) || __GNUC__ == (x) && __GNUC_MINOR__ >= (y))
#    define AV_GCC_VERSION_AT_MOST(x,y)  (__GNUC__ < (x) || __GNUC__ == (x) && __GNUC_MINOR__ <= (y))
#else
#    define AV_GCC_VERSION_AT_LEAST(x,y) 0
#    define AV_GCC_VERSION_AT_MOST(x,y)  0
#endif

#if AV_GCC_VERSION_AT_LEAST(3,4)
#    define attribute_warn_unused_result __attribute__((warn_unused_result))
#else
#    define attribute_warn_unused_result
#endif

#define CICADA_EXTERN CICADA_CPLUS_EXTERN

#endif //Cicada_H


