

# 最低系统版本：Metal / CVMetalTextureCache、VideoToolbox 的 HEVC 探测（__builtin_available
# (macOS 10.13)）以及 Qt 6.x 自身都要求比 10.11 更高，而 arm64 的 Mac 起步就是 11.0。
# 旧值 10.11 会让 Xcode 每次都报 "deployment target 10.11 ... supported ... 10.13 to 15.x"。
set(MACOSX_DEPLOYMENT_TARGET 11.0)

set(CMAKE_XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET ${MACOSX_DEPLOYMENT_TARGET})
set(MAC_INSTALL_DIR ${CMAKE_CURRENT_LIST_DIR}/../external/install)

set(MAC_ARCH ${CMAKE_SYSTEM_PROCESSOR})

set(COMMON_LIB_DIR ${COMMON_LIB_DIR}
        ${MAC_INSTALL_DIR}/curl/Darwin/${MAC_ARCH}/lib
        ${MAC_INSTALL_DIR}/librtmp/Darwin/${MAC_ARCH}/lib
        ${MAC_INSTALL_DIR}/openssl/Darwin/${MAC_ARCH}/lib
        ${MAC_INSTALL_DIR}/ffmpeg/Darwin/${MAC_ARCH}/lib
        ${MAC_INSTALL_DIR}/fdk-aac/Darwin/${MAC_ARCH}/lib
        ${MAC_INSTALL_DIR}/cares/Darwin/${MAC_ARCH}/lib
        ${MAC_INSTALL_DIR}/dav1d/Darwin/${MAC_ARCH}/lib
        ${MAC_INSTALL_DIR}/nghttp2/Darwin/${MAC_ARCH}/lib
        /opt/homebrew/lib/
        )
set(COMMON_INC_DIR ${COMMON_INC_DIR}
        ${MAC_INSTALL_DIR}/curl/Darwin/${MAC_ARCH}/include
        ${MAC_INSTALL_DIR}/librtmp/Darwin/${MAC_ARCH}/include
        ${MAC_INSTALL_DIR}/openssl/Darwin/${MAC_ARCH}/include
        ${MAC_INSTALL_DIR}/ffmpeg/Darwin/${MAC_ARCH}/include
        ${MAC_INSTALL_DIR}/cares/Darwin/${MAC_ARCH}/include
        ${MAC_INSTALL_DIR}/../build/ffmpeg/Darwin/${MAC_ARCH}/
        ${MAC_INSTALL_DIR}/../boost/
        ${MAC_INSTALL_DIR}/../external/ffmpeg/
        /opt/homebrew/include/
        ${PROJECT_SOURCE_DIR})


set(FFMPEG_SOURCE_DIR ${TOPDIR}/external/external/ffmpeg/)

find_library(VIDEO_TOOL_BOX VideoToolbox)
find_library(AUDIO_TOOL_BOX AudioToolbox)
find_library(COREMEDIA CoreMedia)
find_library(COREVIDEO CoreVideo)
find_library(COREFOUNDATION CoreFoundation)
find_library(VIDEODECODERACCELERATION VideoDecodeAcceleration)
find_library(COREFOUNDATION CoreFoundation)
find_library(SECURITY Security)
find_library(OPENGL OpenGL)
find_library(APPKIT AppKit)
find_library(AVFOUNDATION AVFoundation)
find_library(QUARTZCORE QuartzCore)

set(FRAMEWORK_LIBS
        ${VIDEO_TOOL_BOX}
        ${AUDIO_TOOL_BOX}
        ${COREMEDIA}
        ${COREVIDEO}
        ${VIDEODECODERACCELERATION}
        ${COREFOUNDATION}
        ${SECURITY}
        ${COREFOUNDATION}
        ${OPENGL}
        ${APPKIT}
        ${AVFOUNDATION}
        ${QUARTZCORE}
        iconv
        z)

set(TARGET_LIBRARY_TYPE STATIC)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror=return-type")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror=return-type")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mmacosx-version-min=${MACOSX_DEPLOYMENT_TARGET}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mmacosx-version-min=${MACOSX_DEPLOYMENT_TARGET}")

# ---------------------------------------------------------------------------
# -Wno-macro-redefined：只为一处"第三方头之间的宏撞名"而关
#
# 【现象】macOS 构建里有 37 条
#     libavutil/attributes.h:129: warning: 'attribute_deprecated' macro redefined
#
# 【为什么不能靠改我们自己的代码消掉】
#   · 这个宏是本工程对外 API 的一部分，必须保留（`media_player_api.h`、
#     `framework/demuxer/IDemuxer.h`、`AVAFPacket.h`、`demuxer_service.h` 都用它标注
#     "已废弃但保留"的接口），所以不能撤掉 `CicadaType.h` 里的定义；
#   · FFmpeg 的 `libavutil/attributes.h:128-136` **无条件**定义同名宏
#     （C++ 下走 `[[deprecated]]` 那一支），外面**没有 `#ifndef` 保护**：
#         #if AV_HAS_STD_ATTRIBUTE(deprecated)
#         #    define attribute_deprecated [[deprecated]]
#     于是无论谁先定义，后定义的那个都必然触发 -Wmacro-redefined。
#     `CicadaType.h` 里的 `#ifndef` 只能保护它自己，保护不了 FFmpeg 那边。
#   · 把我们的取值改成 `[[deprecated]]` 也没用：vendored 的
#     `external/external/ffmpeg/libavutil/attributes.h` 在 clang 下走的是
#     `__attribute__((deprecated))` 分支，只会把同一条告警挪到另一侧；而且 MSVC 分支
#     还得继续用 `__declspec(deprecated)`。
#
# 【取值本身不冲突】`[[deprecated]]` 与 `__attribute__((deprecated))` 语义等价。
#
# 【为什么是关整个告警而不是逐处 pragma】
#   FFmpeg 头是被各文件**间接**包含的（`avcodec.h`/`avformat.h`/`avutil/*` 分散在
#   framework 各处），没有单一的包含点可以套 `#pragma clang diagnostic push/pop`；
#   而 `-isystem` 也挡不住它（宏重定义发生在**我们**的编译单元里）。
#   这是"两套第三方头撞名"的固有代价，用一条显式、有注释的开关处理，比留 37 条
#   噪声（或让后来人以为项目在忽略真问题）更诚实。
#
# 【影响面】这个开关只关 `macro redefined` 这一类。**本工程自己代码里的宏重定义
#   不会被藏住**——那种情况本来就是 bug，应该被修掉；这条注释的意义就是让人知道
#   为什么这里有一个看起来"太宽"的开关。
# ---------------------------------------------------------------------------
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-macro-redefined")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-macro-redefined")

# ---------------------------------------------------------------------------
# GL_SILENCE_DEPRECATION：macOS 10.14 起 OpenGL 的函数族被标记废弃
#
# 【现象】约 110 条 deprecation，几乎全是 macOS SDK 自己发出来的
#     'glTexParameteri' is deprecated: first deprecated in macOS 10.14 - OpenGL API
#     deprecated. (Define GL_SILENCE_DEPRECATION to silence these warnings)
#   ——**告警文本自己就给了官方消音宏**。
#
# 【为什么不改代码】本工程 macOS 的 GL 渲染后端（framework/render/video/glRender/）
#   本来就是 OpenGL 实现，源码里没有"用错 API"这回事：这些函数就是该这样调，
#   只是 Apple 想推 Metal。逐条改写成 Metal 是**另一个工程**（与 QtPlayer 侧的
#   Metal 零拷贝链是两件事），不属于"迁移 FFmpeg API / 修告警"的范围。
#   Apple 官方给的正解就是定义这个宏，所以这里按官方口径消音。
#
# 【它管不到什么】这个宏**只管 OpenGL 函数**，管不到 AppKit 那两个类：
#   · 'NSOpenGLContext' is deprecated: ... Please use Metal or MetalKit.   （5 条）
#   · 'NSOpenGLView'    is deprecated: ... Please use MTKView instead.    （4 条）
#   这 9 条来自 framework/render/video/glRender/platform/mac/，改掉它们等于把
#   macOS 的 GL 视图层重写成 MTKView —— 那是上面说的"另一个工程"。**这 9 条本轮
#   刻意保留**，不假装它们已经消掉；要消必须单独立项。
# ---------------------------------------------------------------------------
add_compile_definitions(GL_SILENCE_DEPRECATION)

if (USEASAN)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer -fsanitize-address-use-after-scope")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer -fsanitize-address-use-after-scope")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")
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

if (TRAVIS)
    set(ENABLE_CHEAT_RENDER ON)
    set(ENABLE_SDL OFF)
else ()
    set(ENABLE_SDL ON)
endif ()

if (CMDLINE_BUILD)
    message("CMDLINE_BUILD")
    set(BUILD_TEST ON)
#    set(ENABLE_SDL ON)
    set(ENABLE_GLRENDER OFF)
else ()
    set(ENABLE_GLRENDER ON)
    set(ENABLE_SDL OFF)
endif ()

