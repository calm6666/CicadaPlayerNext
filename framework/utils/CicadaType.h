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
     * ============ attribute_deprecated 与 FFmpeg 的重定义（为什么要显式抑制）============
     *
     * 本工程**必须**自己定义这个宏：它是我们对外 API 的一部分
     * （`mediaPlayer/media_player_api.h:45`、`framework/demuxer/IDemuxer.h:142`、
     * `AVAFPacket.h:39`、`demuxer_service.h:144` 都用它标注"已废弃但保留"的接口），
     * 所以不能为了避让 FFmpeg 而撤掉它。
     *
     * 而 FFmpeg 的 `libavutil/attributes.h:128-136` 也会**无条件**定义同名宏：
     *     #if AV_HAS_STD_ATTRIBUTE(deprecated)
     *     #    define attribute_deprecated [[deprecated]]     ← C++ 下走这一支
     * 它外面没有 `#ifndef` 保护，于是无论谁先定义，后定义的那个都会触发
     * `-Wmacro-redefined`（MSVC 是 C4005）—— 本文件里的 `#ifndef` 只能保护自己，
     * 保护不了别人。
     *
     * 取值本身**不冲突**：`[[deprecated]]` 与 `__attribute__((deprecated))` 语义等价，
     * 只是写法不同。真正会出问题的只有"我们包含了 FFmpeg 头再回到自己的声明"这种
     * 顺序，而两者混用在本工程里已经跑了很久、行为正常。
     *
     * 结论：这是**第三方头之间的宏撞名**，不是本工程的代码问题，但用户要求零告警，
     * 所以改为在**受影响的 target 上**加 `-Wno-macro-redefined` 显式抑制
     * （见 `framework/macOSX.cmake` 里那段说明），而不是把它当成"已知问题"留在日志里。
     * 这里保持原生写法（不改成 [[deprecated]]）：改了只会把同一条告警挪到另一侧
     * （vendored 的 `external/external/ffmpeg` 用的是 `__attribute__` 分支），
     * 而且 MSVC 分支还得继续用 `__declspec`。
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


