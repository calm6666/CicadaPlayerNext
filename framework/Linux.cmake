

set(INSTALL_DIR ${CMAKE_CURRENT_LIST_DIR}/../external/install)

set(COMMON_LIB_DIR ${COMMON_LIB_DIR}
        ${INSTALL_DIR}/curl/Linux/x86_64/lib
        ${INSTALL_DIR}/librtmp/Linux/x86_64/lib
        ${INSTALL_DIR}/openssl/Linux/x86_64/lib
        ${INSTALL_DIR}/ffmpeg/Linux/x86_64/lib
        ${INSTALL_DIR}/fdk-aac/Linux/x86_64/lib
        ${INSTALL_DIR}/cares/Linux/x86_64/lib
        ${INSTALL_DIR}/libxml2/Linux/x86_64/lib
        ${INSTALL_DIR}/nghttp2/Linux/x86_64/lib
        )
set(COMMON_INC_DIR ${COMMON_INC_DIR}
        ${INSTALL_DIR}/curl/Linux/x86_64/include
        ${INSTALL_DIR}/librtmp/Linux/x86_64/include
        ${INSTALL_DIR}/openssl/Linux/x86_64/include
        ${INSTALL_DIR}/ffmpeg/Linux/x86_64/include
        ${INSTALL_DIR}/cares/Linux/x86_64/include
        ${INSTALL_DIR}/libxml2/Linux/x86_64/include/libxml2
        ${INSTALL_DIR}/../build/ffmpeg/Linux/x86_64/
        ${INSTALL_DIR}/../boost/
        # Was ${INSTALL_DIR}/../external/external/ffmpeg/, which resolves to
        # external/external/external/ffmpeg and does not exist. FFMPEG_SOURCE_DIR
        # below is the real source tree.
        ${INSTALL_DIR}/../external/ffmpeg/
        /usr/include/SDL2
        ${PROJECT_SOURCE_DIR})


set(FFMPEG_SOURCE_DIR ${INSTALL_DIR}/../external/ffmpeg/)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror=return-type -fPIC")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror=return-type -fPIC")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-Bsymbolic")

# Same as windows.cmake: the framework logs with PRIx64 / PRId64 and the C++
# headers only guarantee those with this macro on older glibc.
add_definitions(-D __STDC_FORMAT_MACROS)

# ---- VAAPI ------------------------------------------------------------------
# Linux hardware video decoding goes through FFmpeg's VAAPI hwaccels, which
# external/player_ffmpeg_config.sh requests for this platform;
# framework/codec/avcodecDecoder.cpp drives them with the same copy-back design
# it uses for D3D11VA on Windows (see CICADA_HW_DEVICE_TYPE there).
#
# The link matters as much as the code. libavutil's hwcontext.c references every
# enabled hwcontext from its device-type table, so once FFmpeg is built with
# CONFIG_VAAPI the static libraries reference libva and the executable has to
# link it.
#
# That is detected rather than assumed, because linking -lva on a build machine
# without libva-dev would fail outright: if FFmpeg's own config.h does not say
# CONFIG_VAAPI 1 then its VAAPI hwaccels were disabled at configure time (with a
# WARN from ffmpeg_verify_requested_components), the libraries need no libva and
# nothing is added here - the player simply decodes in software.
set(CICADA_FFMPEG_CONFIG_H ${INSTALL_DIR}/../build/ffmpeg/Linux/x86_64/config.h)

if (EXISTS ${CICADA_FFMPEG_CONFIG_H})
    file(READ ${CICADA_FFMPEG_CONFIG_H} _cicada_ffmpeg_config)

    if (_cicada_ffmpeg_config MATCHES "#define CONFIG_VAAPI 1")
        # hwcontext_vaapi.c obtains a display through one of these; each is a
        # separate library, and libva-drm / libva-x11 come from libva-dev. They
        # are looked up with find_library so a machine that only has the base
        # libva still links.
        find_library(CICADA_VA_LIB NAMES va)
        find_library(CICADA_VA_DRM_LIB NAMES va-drm)
        find_library(CICADA_VA_X11_LIB NAMES va-x11)

        set(CICADA_VAAPI_LIBS "")
        foreach (_va_lib ${CICADA_VA_LIB} ${CICADA_VA_DRM_LIB} ${CICADA_VA_X11_LIB})
            if (_va_lib)
                list(APPEND CICADA_VAAPI_LIBS ${_va_lib})
            endif ()
        endforeach ()

        message("Linux: FFmpeg has VAAPI, linking ${CICADA_VAAPI_LIBS}")
    else ()
        message("Linux: FFmpeg was built without VAAPI (install libva-dev and rebuild it for hardware decoding)")
    endif ()
endif ()

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

if (USEMSAN)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=memory, -fsanitize-memory-track-origins -fno-omit-frame-pointer")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=memory, -fsanitize-memory-track-origins -fno-omit-frame-pointer")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=memory, -fsanitize-memory-track-origins -fno-omit-frame-pointer")
    #    set(CMAKE_STATIC_LINKER_FLAGS "${CMAKE_STATIC_LINKER_FLAGS} -fsanitize=address")
endif (USEMSAN)

set(TARGET_LIBRARY_TYPE STATIC)

set(ENABLE_GLRENDER OFF)

#set(BUILD_TEST ON)
if (TRAVIS)
    set(ENABLE_CHEAT_RENDER ON)
else ()
    set(ENABLE_SDL ON)
endif ()

