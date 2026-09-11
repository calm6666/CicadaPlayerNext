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
# FFmpeg 源码树在 external/external/ffmpeg（不是 external/ffmpeg）——播放器会直接
# 包含内部头（libavformat/avc.h、libavcodec/h264_parse.h 等），这些只存在于源码树。
set(FFMPEG_SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../external/external/ffmpeg/)

set(COMMON_INC_DIR ${COMMON_INC_DIR}
        ${EXTERN_INSTALL_DIR_OHOS}/ffmpeg/OHOS/${OHOS_ARCH}/include/
        ${EXTERN_INSTALL_DIR_OHOS}/../build/ffmpeg/OHOS/${OHOS_ARCH}/
        ${EXTERN_INSTALL_DIR_OHOS}/curl/OHOS/${OHOS_ARCH}/include/
        ${EXTERN_INSTALL_DIR_OHOS}/openssl/OHOS/${OHOS_ARCH}/include/
        # DOMHelper/DOMParser 使用 <libxml/xmlreader.h>，因此要指向 include/libxml2
        ${EXTERN_INSTALL_DIR_OHOS}/libxml2/OHOS/${OHOS_ARCH}/include/libxml2/
        ${EXTERN_INSTALL_DIR_OHOS}/nghttp2/OHOS/${OHOS_ARCH}/include/
        ${CMAKE_CURRENT_LIST_DIR}/../external/boost/
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/../
        ${FFMPEG_SOURCE_DIR}
        )

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D__STDC_CONSTANT_MACROS")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D__OHOS__ -DOHOS")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D__OHOS__ -DOHOS")

# 播放器兼容宏（attribute_deprecated / AV_DISPOSITION_ATTACHED_PIC）与 FFmpeg 9
# 头文件中的同名宏在混编时会互相重定义（展开值相同），降级为警告而非错误。
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-macro-redefined -Wno-deprecated-declarations")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-macro-redefined -Wno-deprecated-declarations")

set(TARGET_LIBRARY_TYPE STATIC)

# HW codec / audio / DRM / surface support via the OpenHarmony NDK.
#
# OHOS/OhosAVCodecDecoder.cpp and drm/OHOS/OhosDrmHandler.cpp were both written
# against an API that does not exist in the NDK and have been rewritten against
# the real API 22 surface (OH_AVCodec_GetCapabilityByCategory +
# OH_AVCapability_IsHardware/GetName, the per-family OH_VideoDecoder_* /
# OH_AudioCodec_* entry points, and MediaKeySystem / MediaKeySession from
# native_drm_common.h).
# Forced into the cache because module_config.cmake's option() would otherwise
# reset it -- CMP0077 is not set in this project.
set(ENABLE_OHOS_AVCODEC_DECODER ON CACHE BOOL
        "enable OpenHarmony OH_AVCodec hardware decoder" FORCE)
set(ENABLE_OHOS_AUDIO_RENDER ON)
set(ENABLE_GLRENDER OFF)

# NDK multimedia system libraries (stub libs resolved at runtime on device).
# NOTE: the audio-codec stub is libnative_media_acodec.so -- there is no
# libnative_media_audiocodec.so in the OHOS SDK. Audio output itself goes
# through libohaudio; this stub is only kept because it is part of the
# multimedia set the decoder pulls in.
set(OHOS_SYSTEM_LIBS
        libnative_window.so
        libnative_buffer.so
        libnative_media_codecbase.so
        libnative_media_vdec.so
        libnative_media_acodec.so
        libnative_media_core.so
        libohaudio.so
        libnative_drm.so
        )
