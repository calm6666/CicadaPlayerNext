#!/usr/bin/env bash
# ============================================================================
# Android toolchain configuration (modern NDK, clang only)
#
# FFmpeg 9.0 requires a C11/C17-capable toolchain. The legacy gcc 4.9 /
# android-14 configuration was removed. NDK r25+ is required; the platform
# level is pinned to android-24 to match the player's minimum Android 7.0
# (API 24) support.
# ============================================================================

# Default Android API level (Android 7.0). Override with ANDROID_API_LEVEL.
ANDROID_API_LEVEL=${ANDROID_API_LEVEL:-24}

function android_armv7_a_init_env(){
    ABI=armeabi-v7a
    CPU_ARCH=arm
    CROSS_COMPILE=armv7a-linux-androideabi
    CPU_FLAGS=""
    CPU_LD_FLAGS=""
}

function android_arm64_v8a_init_env(){
    ABI=arm64-v8a
    CPU_ARCH=arm64
    CROSS_COMPILE=aarch64-linux-android
    CPU_FLAGS=""
    CPU_LD_FLAGS=""
}

function android_init_env(){
    if [ "$1" == "armeabi-v7a" ]
    then
        android_armv7_a_init_env
    elif [ "$1" == "arm64-v8a" ]
    then
        android_arm64_v8a_init_env
    elif [ "$1" == "x86_64" ]
    then
        ABI=x86_64
        CPU_ARCH=x86_64
        CROSS_COMPILE=x86_64-linux-android
        CPU_FLAGS=""
        CPU_LD_FLAGS=""
    else
        echo unsupported Android ABI $1
        return
    fi

    # Modern NDK layout: toolchains/llvm/prebuilt/<host>/bin/clang
    local ndk_host
    case "$(uname -s)" in
        Darwin*) ndk_host=darwin-x86_64 ;;
        *)       ndk_host=linux-x86_64 ;;
    esac
    local toolchain=${ANDROID_NDK}/toolchains/llvm/prebuilt/${ndk_host}
    local clang="${toolchain}/bin/clang"
    if [[ ! -x "${clang}" ]];then
        echo "error: NDK clang not found at ${clang} (NDK r25+ required)"
        exit 1
    fi

    local TARGET=${CROSS_COMPILE}${ANDROID_API_LEVEL}
    CC="${clang} -target ${TARGET} --sysroot=${toolchain}/sysroot"
    AS="${clang} -target ${TARGET} --sysroot=${toolchain}/sysroot"
    CROSS_PREFIX="${toolchain}/bin/llvm-"
    TARGET_OS=Android
    SYSTEM_ROOT=${toolchain}/sysroot

    echo "CC is ${CC}"
    echo "AS is ${AS}"

    if [[ "${CPU_ARCH}" =~ "arm" ]]
    then
        NEON_SUPPORT="TRUE"
    else
        NEON_SUPPORT="FALSE"
    fi
}
