
message("TOPDIR is ${TOPDIR}")

set(WINDOWS_INSTALL_DIR ${CMAKE_CURRENT_LIST_DIR}/../external/install)

message("WINDOWS_INSTALL_DIR is ${WINDOWS_INSTALL_DIR}")

if (${CMAKE_SIZEOF_VOID_P} MATCHES 4)
    set(ARCH i686)
else ()
    set(ARCH x86_64)
endif ()

set(SDL_DIR "$ENV{HOME}/Downloads/SDL2-2.0.10")
if (MSVC)
  set(FFMPEG_SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../external/external/ffmpeg/)
  set(FFMPEG_BUILD_DIR ${CMAKE_CURRENT_LIST_DIR}/../external/build/ffmpeg/win32/${ARCH})

  # MSVC 无法解析 FFmpeg 的 GCC 内联汇编：内部头 libavcodec/mathops.h 会
  # #include "x86/mathops.h"，而后者整体包在 #if HAVE_INLINE_ASM 中，用的是
  # __asm__ 的 GCC 操作数语法（:"=a"(rt), "=d"(dummy) : ...），MSVC 直接报
  # C2143。${FFMPEG_BUILD_DIR}/config.h 由 mingw 交叉构建生成
  # （HAVE_INLINE_ASM 1），所以这里按 FFmpeg 自带 msvc toolchain 的做法生成
  # 一份 HAVE_INLINE_ASM 0 的 config.h，并置于 include 路径最前，使内部头
  # 走通用 C 回退实现（mathops.h 里的 #ifndef MULH/MUL64/... 分支）。
  set(MSVC_FFMPEG_INC ${CMAKE_BINARY_DIR}/msvc_ffmpeg_shim)
  if (NOT EXISTS ${MSVC_FFMPEG_INC}/config.h)
    file(MAKE_DIRECTORY ${MSVC_FFMPEG_INC})
    file(READ ${FFMPEG_BUILD_DIR}/config.h _ffmpeg_config)
    string(REPLACE "#define HAVE_INLINE_ASM 1" "#define HAVE_INLINE_ASM 0"
            _ffmpeg_config "${_ffmpeg_config}")
    file(WRITE ${MSVC_FFMPEG_INC}/config.h "${_ffmpeg_config}")
    message("MSVC: wrote FFmpeg shim config.h (HAVE_INLINE_ASM=0) to ${MSVC_FFMPEG_INC}")
  endif ()
  list(FIND COMMON_INC_DIR ${MSVC_FFMPEG_INC} _ffmpeg_shim_pos)
  if (_ffmpeg_shim_pos EQUAL -1)
    list(INSERT COMMON_INC_DIR 0 ${MSVC_FFMPEG_INC})
  endif ()

  # MSVC 无法链接 mingw 的静态 .a；win32 交叉构建产物是合并库
  # libffmpeg.dll + libffmpeg.lib（--out-implib --kill-at，MSVC 可链），
  # libffmpeg.lib 就放在 install/ffmpeg/win32/${ARCH}/ 顶层
  set(COMMON_LIB_DIR ${COMMON_LIB_DIR}
          ${WINDOWS_INSTALL_DIR}/ffmpeg/win32/${ARCH}
          ${WINDOWS_INSTALL_DIR}/ffmpeg/win32/${ARCH}/bin)
  set(COMMON_INC_DIR ${COMMON_INC_DIR}
          ${WINDOWS_INSTALL_DIR}/ffmpeg/win32/${ARCH}/include
          ${WINDOWS_INSTALL_DIR}/curl/win32/${ARCH}/include
          ${WINDOWS_INSTALL_DIR}/openssl/win32/${ARCH}/include
          ${PROJECT_SOURCE_DIR}
          ${CMAKE_CURRENT_LIST_DIR}/../external/boost
          # 内部头（avc.h/hevc.h 等）与 config.h 直接从源码树/构建树取，
          # 不再需要手工拷贝（doc 第 2 步的拷贝可跳过）
          ${FFMPEG_SOURCE_DIR}
          ${FFMPEG_BUILD_DIR})
  link_libraries(${FFMPEG_LIBRARIES})
  find_package(sdl2 REQUIRED)
  link_libraries(SDL2::SDL2)
  # pthread（PThreads4W，按文档 doc/compile_Windows_msvc.md 第 2 步经
  # install_external.bat 的 vcpkg 安装）：不同 vcpkg 版本的包名/导出目标名
  # 不同（pthread / pthreads / PThreads4W），逐个探测；全部失败给出明确指引
  set(PTHREAD_LINK_TARGET "")
  foreach(_pthread_pkg IN ITEMS PThreads4W pthreads pthread)
    if (NOT PTHREAD_LINK_TARGET)
      find_package(${_pthread_pkg} CONFIG QUIET)
    endif ()
    if (TARGET PThreads4W::PThreads4W)
      set(PTHREAD_LINK_TARGET PThreads4W::PThreads4W)
    elseif (TARGET pthreads::pthreads)
      set(PTHREAD_LINK_TARGET pthreads::pthreads)
    endif ()
  endforeach ()
  if (PTHREAD_LINK_TARGET)
    link_libraries(${PTHREAD_LINK_TARGET})
  elseif (PThreads4W_LIBRARY)
    link_libraries(${PThreads4W_LIBRARY})
  else ()
    message(FATAL_ERROR "pthread (PThreads4W) not found.\n"
        "  Install it via vcpkg:  vcpkg install pthreads   (new vcpkg)\n"
        "                      or vcpkg install pthread    (old vcpkg, pinned in\n"
        "  external/win/install_external.bat; see doc/compile_Windows_msvc.md step 2),\n"
        "  and make sure CMAKE_TOOLCHAIN_FILE points to vcpkg.cmake.")
  endif ()
  find_package(LibXml2 REQUIRED)
  link_libraries(${LIBXML2_LIBRARIES})
  include_directories(${LIBXML2_INCLUDE_DIR})
  add_definitions(-DNOMINMAX)
else ()
  set(COMMON_LIB_DIR ${COMMON_LIB_DIR}
        ${WINDOWS_INSTALL_DIR}/curl/win32/${ARCH}/lib
        ${WINDOWS_INSTALL_DIR}/librtmp/win32/${ARCH}/lib
        ${WINDOWS_INSTALL_DIR}/openssl/win32/${ARCH}/lib
        ${WINDOWS_INSTALL_DIR}/ffmpeg/win32/${ARCH}/lib
        #        ${WINDOWS_INSTALL_DIR}/pthread/win32/${ARCH}/lib
        ${WINDOWS_INSTALL_DIR}/fdk-aac/win32/${ARCH}/lib
        ${SDL_DIR}/${ARCH}-w64-mingw32/lib
        )
  set(FFMPEG_SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../external/external/ffmpeg/)
  set(FFMPEG_BUILD_DIR ${CMAKE_CURRENT_LIST_DIR}/../external/build/ffmpeg/win32/${ARCH})
  set(COMMON_INC_DIR ${COMMON_INC_DIR}
        ${WINDOWS_INSTALL_DIR}/curl/win32/${ARCH}/include
        ${WINDOWS_INSTALL_DIR}/librtmp/win32/${ARCH}/include
        ${WINDOWS_INSTALL_DIR}/openssl/win32/${ARCH}/include
        ${WINDOWS_INSTALL_DIR}/ffmpeg/win32/${ARCH}/include
        ${PROJECT_SOURCE_DIR}
        ${FFMPEG_SOURCE_DIR}
        ${FFMPEG_BUILD_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/../external/boost
        ${SDL_DIR}/${ARCH}-w64-mingw32/include)
endif ()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DBUILDING_LIBCURL")
link_libraries(ws2_32)

set(TARGET_LIBRARY_TYPE STATIC)

set(ENABLE_GLRENDER OFF)
set(ENABLE_SDL ON)
set(BUILD_TEST OFF)
add_definitions(-D __STDC_FORMAT_MACROS)
