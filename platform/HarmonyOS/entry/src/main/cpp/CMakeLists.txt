# CMakeLists for the HarmonyOS native module.
#
# Builds the whole Cicada player (framework + mediaPlayer) plus the NAPI
# wrapper into one shared library `libentry.so`.
#
# The hvigor externalNativeOptions invoke this file with the OHOS NDK
# toolchain (ohos.toolchain.cmake). Externals must be prebuilt with
# external/build_external.sh OHOS (see docs/Packaging_HarmonyOS.md).

cmake_minimum_required(VERSION 3.5.0)
project(cicada_ohos LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)

# CicadaPlayerNext source root (this repo layout: platform/HarmonyOS/entry/src/main/cpp)
get_filename_component(CPP_DIR ${CMAKE_CURRENT_SOURCE_DIR} ABSOLUTE)
set(CICADA_ROOT ${CPP_DIR}/../../../../../..)

if (DEFINED FFMPEG_INSTALL_DIR_OHOS)
    set(FFMPEG_INSTALL_DIR_OHOS_LOCAL ${FFMPEG_INSTALL_DIR_OHOS})
else()
    set(FFMPEG_INSTALL_DIR_OHOS_LOCAL ${CICADA_ROOT}/external/install/ffmpeg/OHOS/${OHOS_ARCH})
endif()

# --- build the player (framework + mediaPlayer) ------------------------------
set(BUILD_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(${CICADA_ROOT}/mediaPlayer ${CMAKE_CURRENT_BINARY_DIR}/mediaPlayer)

# --- NAPI wrapper -------------------------------------------------------------
add_library(entry SHARED napi_init.cpp)

target_include_directories(entry PRIVATE
        ${CICADA_ROOT}/mediaPlayer
        ${CICADA_ROOT}/framework
        ${CICADA_ROOT}/framework/base
        ${FFMPEG_INSTALL_DIR_OHOS_LOCAL}/include)

target_link_libraries(entry PUBLIC
        media_player
        libace_napi.z.so
        libnative_window.so
        libnative_buffer.so
        libnative_media_codecbase.so
        libnative_media_vdec.so
        libnative_media_audiocodec.so
        libnative_media_core.so
        libohaudio.so
        libnative_drm.so
        ${FFMPEG_INSTALL_DIR_OHOS_LOCAL}/libalivcffmpeg.so)
