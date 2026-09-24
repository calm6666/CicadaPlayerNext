// ===========================================================================
// Danmaku —— 导出宏
//
// 由 CMake 驱动的编译定义决定：
//   * DANMAKU_BUILDING —— 由本模块的 CMakeLists 在编译**本库**时定义
//   * DANMAKU_STATIC   —— 静态库构建时由本模块 PUBLIC 透出给使用方
//
// 使用方（宿主）如果通过 CMake 的 target_link_libraries 链接 Danmaku，
// 这些定义会自动带过去；如果手写编译命令，那么：
//   * 动态库：什么都不用定义（Windows 上会走 dllimport）
//   * 静态库：必须自己加 -DDANMAKU_STATIC
// ===========================================================================
#ifndef DANMAKU_EXPORT_H
#define DANMAKU_EXPORT_H

#if defined(DANMAKU_STATIC)
#  define DANMAKU_API
#elif defined(_WIN32) || defined(__CYGWIN__)
#  if defined(DANMAKU_BUILDING)
#    define DANMAKU_API __declspec(dllexport)
#  else
#    define DANMAKU_API __declspec(dllimport)
#  endif
#else
#  if defined(DANMAKU_BUILDING) && (defined(__GNUC__) || defined(__clang__))
#    define DANMAKU_API __attribute__((visibility("default")))
#  else
#    define DANMAKU_API
#  endif
#endif

#endif // DANMAKU_EXPORT_H
