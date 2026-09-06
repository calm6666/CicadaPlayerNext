#!/usr/bin/env bash
source cross_compile_env.sh
source native_compile_env.sh

function ffmpeg_cross_compile_config_reset(){
    FFMPEG_CROSS_COMPILE_CONFIG="--enable-cross-compile \
        --enable-small \
        --disable-runtime-cpudetect"
}

function ffmpeg_cross_compile_config_add(){
    FFMPEG_CROSS_COMPILE_CONFIG="${FFMPEG_CROSS_COMPILE_CONFIG} $1"
}
function ffmpeg_cross_compile_set_Android(){
    cross_compile_set_platform_Android "$1";
    ffmpeg_cross_compile_config_add "--target-os=linux"
    ffmpeg_cross_compile_config_add "--arch=${CPU_ARCH}"
    ffmpeg_cross_compile_config_add "--cross-prefix="${CROSS_PREFIX}""
    ffmpeg_cross_compile_config_add "--disable-linux-perf"
    # NDK r25c 没有 llvm-pkg-config：用项目 shim 应答 libxml2 / openssl 探测
    # （dash_demuxer 依赖 libxml2、https/tls 依赖 openssl，避免被禁用）
    chmod +x "${BUILD_TOOLS_DIR}/ndk_pkg_config.sh" 2>/dev/null || true
    ffmpeg_cross_compile_config_add "--pkg-config=${BUILD_TOOLS_DIR}/ndk_pkg_config.sh"
    if [ "${NEON_SUPPORT}" == "TRUE" ]
    then
         ffmpeg_cross_compile_config_add "--enable-neon"
         ffmpeg_cross_compile_config_add "--enable-thumb"
    fi
}

function ffmpeg_cross_compile_set_iOS(){
    cross_compile_set_platform_iOS "$1"
    ffmpeg_cross_compile_config_add "--target-os=darwin"
    ffmpeg_cross_compile_config_add "--arch=${CPU_ARCH}"
    if [ "${NEON_SUPPORT}" == "TRUE" ]
    then
         ffmpeg_cross_compile_config_add "--enable-neon"
         ffmpeg_cross_compile_config_add "--enable-thumb"
    fi
    if [ "$1" = "i386" -o "$1" = "x86_64" ]
    then
        ffmpeg_cross_compile_config_add "--disable-asm"
    fi
}

function ffmpeg_cross_compile_set_win32(){
    cross_compile_set_platform_win32 "$1"
    ffmpeg_cross_compile_config_add "--target-os=${TARGET_OS}"
    ffmpeg_cross_compile_config_add "--arch=${CPU_ARCH}"
    ffmpeg_cross_compile_config_add "--cross-prefix="${CROSS_COMPILE}-""

    ffmpeg_cross_compile_config_add "--disable-dxva2"
    ffmpeg_cross_compile_config_add "--disable-schannel" #make tls_openssl enable
    if [ "${NEON_SUPPORT}" == "TRUE" ]
    then
         ffmpeg_cross_compile_config_add "--enable-neon"
         ffmpeg_cross_compile_config_add "--enable-thumb"
    fi
}

function ffmpeg_cross_compile_set_OHOS(){
    cross_compile_set_platform_OHOS "$1" || return 1
    ffmpeg_cross_compile_config_add "--target-os=linux"
    ffmpeg_cross_compile_config_add "--arch=${CPU_ARCH}"
    ffmpeg_cross_compile_config_add "--disable-linux-perf"
    # OHOS NDK 同样没有 pkg-config：shim 应答 libxml2 / openssl 探测
    chmod +x "${BUILD_TOOLS_DIR}/ndk_pkg_config.sh" 2>/dev/null || true
    ffmpeg_cross_compile_config_add "--pkg-config=${BUILD_TOOLS_DIR}/ndk_pkg_config.sh"
    # OHOS NDK has no pkg-config and no glibc-specific headers
    ffmpeg_cross_compile_config_add "--disable-symver"
    # OHOS SDK 自带的 vulkan 头不完整：vulkan.h 在 VK_ENABLE_BETA_EXTENSIONS
    # 下引用缺失的 vulkan_beta.h，FFmpeg 的 vulkan.h 恰好定义该宏 → 编译
    # 报 "fatal error: 'vulkan_beta.h' file not found"。播放器鸿蒙硬解走
    # OH_AVCodec（不用 FFmpeg vulkan），显式禁用即可，还省一点体积。
    ffmpeg_cross_compile_config_add "--disable-vulkan"
    if [ "${NEON_SUPPORT}" == "TRUE" ]
    then
         ffmpeg_cross_compile_config_add "--enable-neon"
    fi
    # Optional: disable hand-written asm when bringing up on a new toolchain
    if [[ "${OHOS_DISABLE_ASM}" == "TRUE" ]];then
         ffmpeg_cross_compile_config_add "--disable-asm"
    fi
}
function ffmpeg_native_compile_set_macOS(){
    native_compile_set_platform_macOS "$1"
    ffmpeg_cross_compile_config_add "--target-os=darwin"
    ffmpeg_cross_compile_config_add "--arch=${CPU_ARCH}"
    if [ "${NEON_SUPPORT}" == "TRUE" ]
    then
         ffmpeg_cross_compile_config_add "--enable-neon"
         ffmpeg_cross_compile_config_add "--enable-thumb"
    fi
}

function ffmpeg_native_compile_set_maccatalyst(){
    cross_compile_set_platform_maccatalyst "$1"
    ffmpeg_cross_compile_config_add "--target-os=darwin"
    ffmpeg_cross_compile_config_add "--arch=${CPU_ARCH}"
    if [ "${NEON_SUPPORT}" == "TRUE" ]
    then
         ffmpeg_cross_compile_config_add "--enable-neon"
         ffmpeg_cross_compile_config_add "--enable-thumb"
    fi

    if [ "$1" == "x86_64" ]; then
        ffmpeg_cross_compile_config_add "--disable-asm"
    fi
}