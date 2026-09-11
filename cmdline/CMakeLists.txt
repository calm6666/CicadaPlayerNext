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
            SDLEventReceiver.h
            nativeWindow/CocoaWindow.m
            nativeWindow/X11Window.c
            nativeWindow/WindowsWindow.c)
    # The on-screen control bar is Windows only: on Windows the demo takes over
    # presentation itself so it can draw the bar into the same SDL_Renderer.
    if (MSVC)
        list(APPEND SRC_FILE
                SdlControlBar.cpp
                SdlControlBar.h)
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
    target_link_libraries(cicadaPlayer PUBLIC
            SDL2
            )
endif ()

if (${CMAKE_SYSTEM_NAME} MATCHES "Windows")
    target_link_libraries(cicadaPlayer PUBLIC
            bcrypt
            # GetOpenFileNameW / OPENFILENAMEW live in comdlg32
            comdlg32
            ole32
            )
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
