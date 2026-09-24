cmake_minimum_required(VERSION 3.15)
project(cicadaPlayer)

# 项目源码是 UTF-8（无 BOM），而 MSVC 默认按系统 ANSI(cp936) 解码：中文注释
# 结尾的全角括号（U+FF08/U+FF09 等）在 cp936 下会留下一个悬空的 GBK 首字节，
# 连行尾换行一起吃掉，于是紧随其后的 #include 整行被并入注释。例如
# demuxer/sample_decrypt/SampleDecryptDemuxer.cpp 的
# <libavformat/avformat.h> 就是这样消失的，AVFormatContext 只剩前置声明 ->
# C2027；构建时满屏的 C4819 也是同一病根。显式声明源码/执行字符集均为
# UTF-8（字面量字节与当前一致，只修解码）。
# 必须放在下面的 add_subdirectory 之前，才能覆盖 mediaPlayer / plugin /
# framework / cicadaPlayer 全部目标（plugin 不包含 framework/windows.cmake）。
if (MSVC)
    add_compile_options(/utf-8)
endif ()

# ---------------------------------------------------------------------------
# 强制预包含标准头（非 MSVC 平台）
#
# 这棵树里大量 .cpp / .h 依赖"其它标准头会顺带把 C 标准头拉进来"这一 MSVC 行为，
# 自己却没有显式 #include。MSVC 的 <string>/<vector> 等会带出 <cstring>、
# <cstdint> 之类，libstdc++ 的包含顺序不同，于是同一个文件在 GCC 下报"标识符
# 未声明"。已经暴露过的例子：
#   framework/demuxer/dash/MPDParser.cpp        strstr       需要 <cstring>
#   framework/utils/af_string.h                 uint8_t      需要 <cstdint>
#   mediaPlayer/MediaPlayerConfig.h             uint32_t     需要 <cstdint>
#   framework/demuxer/play_list/SegmentPart.h   INT64_MIN    需要 <cstdint>
# 这类错误只有重编才暴露，逐个补是慢性失血，所以在这里一次性兜住整类。
#
# 做法等价于还原 MSVC 的隐式行为：非 MSVC 平台给每个编译单元预包含这几个头。
# 标准头都有 include guard，重复包含是空操作，所以对已经自己 include 过的文件
# 没有任何影响。Windows 构建完全不经过这个分支。
#
# 只列真正被隐式依赖的那几个：stdint（定长整型）、string（strstr/memcpy/
# strlen）、stdio（printf/snprintf/FILE）、stdlib（malloc/free/atoi/strtol）。
# C 源文件用 C 版本的头，因为 <cstdint>/<cstring> 是 C++ 专用头，加到 .c 上会
# 直接报错。
#
# 必须放在下面的 add_subdirectory 之前：CMAKE_*_FLAGS 只对之后创建的目录生效，
# 而报错的 mediaPlayer / demuxer 目标正是在下面 add_subdirectory 出来的。
# 改完这里需要重新跑一次 cmake 配置，只 make 不会重新生成编译命令。
# ---------------------------------------------------------------------------
if (NOT MSVC)
    foreach (_cicada_std_header cstdint cstring cstdio cstdlib)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -include ${_cicada_std_header}")
    endforeach ()

    foreach (_cicada_std_header stdint.h string.h stdio.h stdlib.h)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -include ${_cicada_std_header}")
    endforeach ()
endif ()

set(CMAKE_CXX_STANDARD 11)
option(CMDLINE_BUILD "build command line example" OFF)
set(CMDLINE_BUILD ON)

if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set(TARGET_PLATFORM macOSX)
elseif (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    set(TARGET_PLATFORM Linux)
elseif (${CMAKE_SYSTEM_NAME} MATCHES "Windows")
    set(TARGET_PLATFORM windows)
endif ()

include(../framework/code_coverage.cmake)
add_subdirectory(../mediaPlayer mediaPlayer.out)
add_subdirectory(../plugin plugin.out)

include(../framework/${TARGET_PLATFORM}.cmake)

include(../framework/module_config.cmake)

list(APPEND SRC_FILE
        cicadaPlayer.cpp
        NetWorkEventReceiver.cpp
        cicadaEventListener.cpp
        )

if (ENABLE_SDL)
    list(APPEND SRC_FILE
            SDLEventReceiver.cpp
            SDLEventReceiver.h)

    # One native-window source per platform.
    #
    # All three used to be listed on every platform. That is harmless for the
    # two C files - their whole body sits behind #ifdef TEST_NATIVE_X11 /
    # TEST_NATIVE_WINDOWS, neither of which is ever defined, so they compile to
    # empty objects - but it is fatal for the Objective-C one: gcc hands a .m
    # file to cc1obj, which only ships with the separate gobjc package, and the
    # Linux build died with
    #   cc: fatal error: cannot execute 'cc1obj': execvp: No such file or directory
    # long before reaching anything that needed Objective-C. Build the file that
    # belongs to the platform instead.
    #
    # (USE_NATIVE_WINDOW is 0 in SDLEventReceiver.h, so none of these is
    # actually called at the moment; they are kept because enabling that path is
    # only a matter of defining TEST_NATIVE_*.)
    if (APPLE)
        list(APPEND SRC_FILE nativeWindow/CocoaWindow.m)
    elseif (WIN32)
        list(APPEND SRC_FILE nativeWindow/WindowsWindow.c)
    else ()
        list(APPEND SRC_FILE nativeWindow/X11Window.c)
    endif ()
    # The on-screen control bar is Windows only: on Windows the demo takes over
    # presentation itself so it can draw the bar into the same SDL_Renderer.
    if (MSVC)
        list(APPEND SRC_FILE
                SdlControlBar.cpp
                SdlControlBar.h
                # 零拷贝（-direct）：解码 D3D11 纹理直接上屏，走自己的交换链和
                # 视频处理器。同样只有 Windows 有这条路。
                D3d11DirectPresenter.cpp
                D3d11DirectPresenter.h)
    endif ()
endif ()

add_executable(cicadaPlayer
        ${SRC_FILE})
target_include_directories(cicadaPlayer PUBLIC ../mediaPlayer)
target_link_directories(cicadaPlayer PRIVATE
        ${COMMON_LIB_DIR})

target_link_libraries(cicadaPlayer PRIVATE
        media_player
        demuxer
        data_source
        render
        videodec)
if (ENABLE_CACHE_MODULE)
    target_link_libraries(cicadaPlayer PRIVATE
            cacheModule)
endif ()
if (ENABLE_MUXER)
    target_link_libraries(cicadaPlayer PRIVATE
            muxer)
endif ()
target_link_libraries(cicadaPlayer PRIVATE
        framework_filter
        framework_utils
        framework_drm)
if (MSVC)
    # MSVC 链合并库 libffmpeg.lib（win32 交叉构建用 --out-implib --kill-at
    # 生成，MSVC 可直接链；运行时把 libffmpeg.dll 拷到 exe 同目录）
    # 库名必须写成 libffmpeg：磁盘上的文件名就是 libffmpeg.lib，写成 ffmpeg
    # 会让链接器去找并不存在的 ffmpeg.lib，报 LNK1181。
    target_link_libraries(cicadaPlayer PRIVATE libffmpeg)
else ()
    target_link_libraries(cicadaPlayer PRIVATE
        avfilter
        avformat
        avcodec
        swresample
        swscale
        avutil)
endif ()
target_link_libraries(cicadaPlayer PRIVATE
        ${FRAMEWORK_LIBS})
if (NOT MSVC)
	target_link_libraries(cicadaPlayer PRIVATE
        curl
        nghttp2
        xml2)
endif()
if (ENABLE_SDL AND NOT MSVC)
    # Linux.cmake asks pkg-config for SDL2 and reports a clear error when the
    # headers are missing; macOS leaves CICADA_SDL2_* unset and keeps using the
    # plain library name.
    if (CICADA_SDL2_LIBRARIES)
        if (CICADA_SDL2_LIBRARY_DIRS)
            target_link_directories(cicadaPlayer PRIVATE ${CICADA_SDL2_LIBRARY_DIRS})
        endif ()
        target_link_libraries(cicadaPlayer PUBLIC ${CICADA_SDL2_LIBRARIES})
    else ()
        target_link_libraries(cicadaPlayer PUBLIC SDL2)
    endif ()
endif ()

if (${CMAKE_SYSTEM_NAME} MATCHES "Windows")
    target_link_libraries(cicadaPlayer PUBLIC
            bcrypt
            # GetOpenFileNameW / OPENFILENAMEW live in comdlg32
            comdlg32
            ole32
            )
    # 零拷贝呈现（D3d11DirectPresenter.cpp）：D3D11 设备/纹理/视频处理器在
    # d3d11，交换链和 DXGI 工厂在 dxgi。这两个是 Windows SDK 自带的导入库，
    # 不需要 vcpkg，也不会影响其它平台。
    # 注意：视频处理器用 GPU 做 NV12->BGRA，不做运行时着色器编译，所以
    # d3dcompiler 不需要链。
    if (MSVC)
        target_link_libraries(cicadaPlayer PUBLIC
                d3d11
                dxgi
                )
    endif ()
else ()
    target_link_libraries(cicadaPlayer PUBLIC
            z
            dl
            )
endif ()

if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    target_link_libraries(cicadaPlayer PUBLIC
            bz2
            )
endif ()

if (APPLE)
    find_library(AVFOUNDATION AVFoundation)
    find_library(QUARTZCORE QuartzCore)
    target_link_libraries(
            cicadaPlayer PUBLIC
            iconv
            bz2
            ${FRAMEWORK_LIBS}
            ${AVFOUNDATION}
            ${QUARTZCORE}
    )
elseif (MSVC)
else ()
    target_link_libraries(
            cicadaPlayer PUBLIC
            ssl
            crypto
            pthread
    )
endif ()
target_link_libraries(cicadaPlayer PUBLIC coverage_config)
#if (${CMAKE_SYSTEM_NAME} MATCHES "Windows")
#else ()
#    add_subdirectory(example)
#endif ()

if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    target_link_libraries(cicadaPlayer PUBLIC X11)
    # libva, only when the FFmpeg the player links was built with VAAPI.
    # Linux.cmake reads FFmpeg's config.h to decide and leaves CICADA_VAAPI_LIBS
    # empty otherwise, so a build machine without libva-dev still links.
    if (CICADA_VAAPI_LIBS)
        target_link_libraries(cicadaPlayer PRIVATE ${CICADA_VAAPI_LIBS})
    endif ()
endif()

if(MSVC AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /SAFESEH:NO")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /SAFESEH:NO")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} /SAFESEH:NO")
endif()


if (USEASAN)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer -fsanitize-address-use-after-scope")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer -fsanitize-address-use-after-scope")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")
    #    set(CMAKE_STATIC_LINKER_FLAGS "${CMAKE_STATIC_LINKER_FLAGS} -fsanitize=address")
endif (USEASAN)

if (USETSAN)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=thread")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=thread")
    #    set(CMAKE_STATIC_LINKER_FLAGS "${CMAKE_STATIC_LINKER_FLAGS} -fsanitize=address")
endif (USETSAN)

if (USEUBSAN)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=undefined, -fsanitize=integer,  -fsanitize=nullability")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=undefined, -fsanitize=integer,  -fsanitize=nullability")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=undefined, -fsanitize=integer, -fsanitize=nullability")
    #    set(CMAKE_STATIC_LINKER_FLAGS "${CMAKE_STATIC_LINKER_FLAGS} -fsanitize=address")
endif (USEUBSAN)

if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    if (USEMSAN)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=memory, -fsanitize-memory-track-origins -fno-omit-frame-pointer")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=memory, -fsanitize-memory-track-origins -fno-omit-frame-pointer")
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=memory, -fsanitize-memory-track-origins -fno-omit-frame-pointer")
        #    set(CMAKE_STATIC_LINKER_FLAGS "${CMAKE_STATIC_LINKER_FLAGS} -fsanitize=address")
    endif (USEMSAN)
endif ()
