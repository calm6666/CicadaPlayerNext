#!/usr/bin/env bash
# ============================================================================
# Build all external C/C++ libraries for HarmonyOS / OpenHarmony.
# Requires: OHOS_SDK environment variable pointing at a full OpenHarmony SDK
#           (native/llvm + native/sysroot must exist).
# Output:   install/ffmpeg/OHOS/<abi>/  (libalivcffmpeg.so + headers + static libs)
# ============================================================================
BUILD_TOOLS_DIR=$(cd $(dirname ${BASH_SOURCE[0]}); pwd)

PATH=$PATH:${BUILD_TOOLS_DIR}

source env.sh
source common_build.sh
source utils.sh

CWD=$PWD

print_warning "build OHOS (OpenHarmony)"
if [ -z "${OHOS_SDK}" ] && [ -z "${OHOS_SDK_HOME}" ];then
    print_error "OHOS_SDK not set"
    exit 1
fi
if [ -n "${OHOS_SDK}" ] && [ ! -d "${OHOS_SDK}/native/llvm" ];then
    print_error "OHOS_SDK invalid: ${OHOS_SDK}/native/llvm not found"
    exit 1
fi
build_libs OHOS "${OHOS_ARCHS}"
