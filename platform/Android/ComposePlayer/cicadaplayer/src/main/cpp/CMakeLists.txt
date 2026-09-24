# ===========================================================================
# :cicadaplayer 原生构建入口 —— **AGP 官方位置**：src/main/cpp/CMakeLists.txt
# （Android Studio 的 "Link C++ Project with Gradle" 生成的就是这个位置）
#
# 本文件只做两件事：
#   1. 把内核里编 libCicadaPlayer.so 需要的三块 add_subdirectory 进来：
#          plugin、mediaPlayer（它自己再 add_subdirectory(framework)）
#      别的平台代码（platform/QtPlayer、platform/HarmonyOS、cmdline、danmaku、tests）
#      **不在任何编译目标里**，不会被编译、也不会被打包。
#   2. 编出 libCicadaPlayer.so（JNI 源码在本模块 src/main/jni/**）。
#
# 路径自己从本文件位置算（和内核自己的 CMake 一样），Gradle 只传内核依赖目录：
#     -DFFMPEG_INSTALL_DIR_ANDROID / -DEXTERN_INSTALL_DIR_ANDROID
# 也允许用 -DCICADA_ROOT / -DCICADA_JNI_DIR 覆盖（下面有 if(NOT DEFINED) 判断）。
# ===========================================================================
cmake_minimum_required(VERSION 3.6)

# 顶层 CMakeLists.txt 必须有 project()（否则 CMake 会警告 "No project() command is present"）。
project(CicadaPlayer C CXX)

if(NOT DEFINED CICADA_ROOT)
    # src/main/cpp → src/main → src → cicadaplayer → JetpackComposePlayer → platform → CicadaPlayerNext
    set(CICADA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..)
endif()
if(NOT DEFINED CICADA_JNI_DIR)
    set(CICADA_JNI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../jni)
endif()

# 万一路径带 Windows 反斜杠（`\h` 会被 CMake 当转义序列 → "Invalid character escape '\h'"），
# 统一转成 CMake 风格的正斜杠。
file(TO_CMAKE_PATH "${CICADA_ROOT}" CICADA_ROOT)
file(TO_CMAKE_PATH "${CICADA_JNI_DIR}" CICADA_JNI_DIR)

message("CicadaPlayer: CICADA_ROOT=${CICADA_ROOT}")
message("CicadaPlayer: CICADA_JNI_DIR=${CICADA_JNI_DIR}")

set(TOPDIR ${CICADA_ROOT})

include(${CMAKE_CURRENT_SOURCE_DIR}/android.cmake)

set(CMAKE_CXX_STANDARD 11)
set(TARGET_PLATFORM Android)

add_definitions(-D__STDC_FORMAT_MACROS)
add_subdirectory(${TOPDIR}/plugin plugin.out)

include_directories(
        ${COMMON_INC_DIR}
        ${TOPDIR}/mediaPlayer/
        ${TOPDIR}/mediaPlayer/external/install/cares/Android/${ANDROID_ABI}/include
)

link_directories(
        ${BOOST_LIB_DIR}
        ${COMMON_LIB_DIR}
)

add_subdirectory(${TOPDIR}/mediaPlayer/ mediaPlayer.out)

find_library(log-lib log)

add_library(CicadaPlayer SHARED
        ${CICADA_JNI_DIR}/JniLoader.cpp
        ${CICADA_JNI_DIR}/player/JavaPlayerConfig.cpp
        ${CICADA_JNI_DIR}/player/JavaCacheConfig.cpp
        ${CICADA_JNI_DIR}/player/NativeBase.cpp
        ${CICADA_JNI_DIR}/player/JavaTrackInfo.cpp
        ${CICADA_JNI_DIR}/player/JavaMediaInfo.cpp
        ${CICADA_JNI_DIR}/player/JavaGlobalSettings.cpp
        ${CICADA_JNI_DIR}/player/JavaExternalPlayer.cpp
        ${CICADA_JNI_DIR}/player/JavaOptions.cpp
        ${CICADA_JNI_DIR}/utils/JavaLogger.cpp
        ${CICADA_JNI_DIR}/utils/ass/JavaAssUtils.cpp
        ${CICADA_JNI_DIR}/utils/ass/JavaAssStyle.cpp
        ${CICADA_JNI_DIR}/utils/ass/JavaAssHeader.cpp
        ${CICADA_JNI_DIR}/utils/ass/JavaAssDialogue.cpp
        )

target_include_directories(CicadaPlayer PRIVATE
        ${CICADA_JNI_DIR}
        ${CICADA_JNI_DIR}/player
        ${CICADA_JNI_DIR}/utils
        )

target_link_libraries(CicadaPlayer
        media_player
        cacheModule
        muxer
        demuxer
        data_source
        render
        videodec
        framework_filter
        framework_utils
        framework_drm
        ffmpeg
        GLESv2
        EGL
        OpenSLES
        android
        z
        ${log-lib}
        )

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-inconsistent-missing-override")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -fstack-protector-all")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -fstack-protector-all")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror=return-type")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror=return-type")
