#!/usr/bin/env bash
# ============================================================================
# HarmonyOS / OpenHarmony native toolchain configuration
#
# Requires the OpenHarmony SDK (command-line tools + native toolchain):
#   https://developer.huawei.com/consumer/cn/download/  (HarmonyOS NEXT SDK)
#   https://gitee.com/openharmony/docs (OpenHarmony SDK)
#
# Layout used:
#   $OHOS_SDK/native/llvm/bin/clang          - clang toolchain
#   $OHOS_SDK/native/sysroot                 - platform sysroot
#   $OHOS_SDK/native/sysroot/usr/lib/<triple> - NDK stub libs
# ============================================================================

function ohos_arm64_v8a_init_env(){
    ABI=arm64-v8a
    CPU_ARCH=aarch64
    TARGET_TRIPLE=aarch64-linux-ohos
    CPU_FLAGS=""
    CPU_LD_FLAGS=""
}

function ohos_armv7_a_init_env(){
    ABI=armeabi-v7a
    CPU_ARCH=arm
    TARGET_TRIPLE=arm-linux-ohos
    CPU_FLAGS=""
    CPU_LD_FLAGS=""
}

function ohos_x86_64_init_env(){
    ABI=x86_64
    CPU_ARCH=x86_64
    TARGET_TRIPLE=x86_64-linux-ohos
    CPU_FLAGS=""
    CPU_LD_FLAGS=""
}

function ohos_init_env(){
    if [[ -z "${OHOS_SDK}" ]];then
        OHOS_SDK=${OHOS_SDK_HOME}
    fi
    if [[ -z "${OHOS_SDK}" ]] || [[ ! -d "${OHOS_SDK}/native/llvm" ]];then
        echo "error: OHOS_SDK not set or invalid (expected \$OHOS_SDK/native/llvm)"
        return 1
    fi

    if [ "$1" == "arm64-v8a" ]
    then
        ohos_arm64_v8a_init_env
    elif [ "$1" == "armeabi-v7a" ]
    then
        ohos_armv7_a_init_env
    elif [ "$1" == "x86_64" ]
    then
        ohos_x86_64_init_env
    else
        echo unsupported OpenHarmony ABI $1
        return 1
    fi

    local native=${OHOS_SDK}/native
    local clang="${native}/llvm/bin/clang"
    local sysroot="${native}/sysroot"

    CC="${clang} --target=${TARGET_TRIPLE} --sysroot=${sysroot} -fPIC"
    AS="${clang} --target=${TARGET_TRIPLE} --sysroot=${sysroot} -fPIC"
    CROSS_PREFIX="${native}/llvm/bin/llvm-"
    TARGET_OS=Linux
    SYSTEM_ROOT=${sysroot}

    echo "OHOS CC is ${CC}"
    echo "OHOS AS is ${AS}"

    if [[ "${CPU_ARCH}" =~ "aarch64" ]]
    then
        NEON_SUPPORT="TRUE"
    else
        NEON_SUPPORT="FALSE"
    fi
    return 0
}
