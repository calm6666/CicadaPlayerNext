

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
        # NOTE: SDL2 is deliberately NOT hardcoded here any more. The tree
        # includes <SDL2/SDL.h>, which needs the parent of the SDL2 directory,
        # so the "/usr/include/SDL2" that used to sit on this line could never
        # satisfy it. It is detected at the bottom of this file instead.
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
        # hwcontext_vaapi.c reaches libva directly, plus a display backend, plus
        # that backend's own dependencies. The transitive ones matter: modern
        # distributions default to --no-copy-dt-needed-entries, so a library that
        # is merely a dependency of another one has to be named explicitly, and
        # omitting one gives the misleading
        #   libavutil.a(hwcontext_vaapi.o): undefined reference to symbol
        #       'drmGetRenderDeviceNameFromFd'
        #   /lib/x86_64-linux-gnu/libdrm.so.2: error adding symbols:
        #       DSO missing from command line
        # drm comes from libva-drm, Xext/Xfixes from libva-x11.
        #
        # Everything is looked up with find_library and only the libraries that
        # actually exist are linked, so a machine with a partial libva still
        # builds - it simply gets a smaller set of VAAPI entry points.
        find_library(CICADA_VA_LIB NAMES va)
        find_library(CICADA_VA_DRM_LIB NAMES va-drm)
        find_library(CICADA_VA_X11_LIB NAMES va-x11)
        find_library(CICADA_DRM_LIB NAMES drm)
        find_library(CICADA_X11_LIB NAMES X11)
        find_library(CICADA_XEXT_LIB NAMES Xext)
        find_library(CICADA_XFIXES_LIB NAMES Xfixes)

        set(CICADA_VAAPI_LIBS "")
        foreach (_va_lib ${CICADA_VA_LIB} ${CICADA_VA_DRM_LIB} ${CICADA_VA_X11_LIB}
                         ${CICADA_DRM_LIB} ${CICADA_X11_LIB} ${CICADA_XEXT_LIB}
                         ${CICADA_XFIXES_LIB})
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

# ---- SDL2 -------------------------------------------------------------------
# Must come after ENABLE_SDL is set above.
#
# ENABLE_SDL is ON here, so the whole render path needs SDL2. Every include in
# the tree is written as <SDL2/SDL.h> (framework/render/video/SdlAFVideoRender.h,
# framework/render/audio/SdlAFAudioRender{,2}.h, cmdline/SDLEventReceiver.h, ...),
# never as <SDL.h>. Two consequences:
#
#   1. The include path has to be the PARENT of the SDL2 directory. The
#      /usr/include/SDL2 that used to be listed in COMMON_INC_DIR can never
#      satisfy <SDL2/SDL.h> - it would only satisfy <SDL.h>.
#   2. On a machine without libsdl2-dev the build did not fail here, it failed
#      much later inside a header:
#        SdlAFAudioRender2.h:9:10: fatal error: SDL2/SDL.h: No such file or directory
#      which gives no hint that a package is missing. This block reports that
#      directly instead.
#
# pkg-config knows where SDL2 actually lives (a distribution package puts it in
# /usr/include, a source install in /usr/local/include), so it is asked first and
# the two usual roots are only probed as a fallback.
find_package(PkgConfig QUIET)

if (PKG_CONFIG_FOUND)
    pkg_check_modules(CICADA_SDL2 QUIET sdl2)
endif ()

set(CICADA_SDL2_FOUND FALSE)

if (CICADA_SDL2_INCLUDE_DIRS)
    list(APPEND COMMON_INC_DIR ${CICADA_SDL2_INCLUDE_DIRS})
    set(CICADA_SDL2_FOUND TRUE)
    message("Linux: SDL2 headers from pkg-config: ${CICADA_SDL2_INCLUDE_DIRS}")
else ()
    foreach (_sdl_root /usr/include /usr/local/include)
        if (EXISTS ${_sdl_root}/SDL2/SDL.h)
            list(APPEND COMMON_INC_DIR ${_sdl_root})
            set(CICADA_SDL2_FOUND TRUE)
            message("Linux: SDL2 headers found in ${_sdl_root}")
        endif ()
    endforeach ()
endif ()

if (ENABLE_SDL AND NOT CICADA_SDL2_FOUND)
    message(FATAL_ERROR
        "SDL2 headers not found, but ENABLE_SDL is enabled for Linux.\n"
        "  Install them with:  sudo apt install libsdl2-dev\n"
        "  (the whole render path includes <SDL2/SDL.h>, so the include path must\n"
        "   be the parent of the SDL2 directory, not the SDL2 directory itself)")
endif ()

