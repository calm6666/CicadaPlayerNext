# ============================================================================
# HarmonyOS / OpenHarmony platform configuration for CicadaPlayerNext
#
# Consumed from framework/CMakeLists.txt when CMAKE_SYSTEM_NAME is "OHOS"
# (the ohos.toolchain.cmake sets it) or when TARGET_PLATFORM is HarmonyOS.
#
# Required inputs (passed by hvigor or on the cmake command line):
#   OHOS_SDK          - OpenHarmony SDK root (contains native/llvm, native/sysroot)
#   OHOS_ARCH         - arm64-v8a | armeabi-v7a | x86_64
#   FFMPEG_INSTALL_DIR_OHOS / EXTERN_INSTALL_DIR_OHOS - external libs (optional)
# ============================================================================

if (NOT DEFINED OHOS_ARCH)
    set(OHOS_ARCH arm64-v8a)
endif ()

set(FFMPEG_INSTALL_DIR_OHOS ${CMAKE_CURRENT_LIST_DIR}/../external/install/ffmpeg/OHOS)
set(EXTERN_INSTALL_DIR_OHOS ${CMAKE_CURRENT_LIST_DIR}/../external/install)

message("FFMPEG_INSTALL_DIR_OHOS is ${FFMPEG_INSTALL_DIR_OHOS}")
message("EXTERN_INSTALL_DIR_OHOS is ${EXTERN_INSTALL_DIR_OHOS}")

set(COMMON_LIB_DIR ${FFMPEG_INSTALL_DIR_OHOS}/${OHOS_ARCH}/)
set(FFMPEG_SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../external/ffmpeg/)

set(COMMON_INC_DIR ${COMMON_INC_DIR}
        ${EXTERN_INSTALL_DIR_OHOS}/ffmpeg/OHOS/${OHOS_ARCH}/include/
        ${EXTERN_INSTALL_DIR_OHOS}/../build/ffmpeg/OHOS/${OHOS_ARCH}/
        ${EXTERN_INSTALL_DIR_OHOS}/curl/OHOS/${OHOS_ARCH}/include/
        ${EXTERN_INSTALL_DIR_OHOS}/openssl/OHOS/${OHOS_ARCH}/include/
        ${CMAKE_CURRENT_LIST_DIR}/../external/boost/
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/../
        ${FFMPEG_SOURCE_DIR}
        )

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D__STDC_CONSTANT_MACROS")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D__OHOS__ -DOHOS")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D__OHOS__ -DOHOS")

set(TARGET_LIBRARY_TYPE STATIC)

# HW codec / audio / DRM / surface support via the OpenHarmony NDK
set(ENABLE_OHOS_AVCODEC_DECODER ON)
set(ENABLE_OHOS_AUDIO_RENDER ON)
set(ENABLE_GLRENDER OFF)

# NDK multimedia system libraries (stub libs resolved at runtime on device)
set(OHOS_SYSTEM_LIBS
        libnative_window.so
        libnative_buffer.so
        libnative_media_codecbase.so
        libnative_media_vdec.so
        libnative_media_audiocodec.so
        libnative_media_core.so
        libohaudio.so
        libnative_drm.so
        )
