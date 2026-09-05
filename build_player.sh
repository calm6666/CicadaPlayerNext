#!/usr/bin/env bash

BUILD_TOOLS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PATH=$PATH:${BUILD_TOOLS_DIR}


function check_brew() {
    if [[ ! `which brew` ]]
    then
        echo 'Homebrew not found. Trying to install...'
        # 国内可用镜像覆盖，如：
        #   export HOMEBREW_INSTALL_URL=https://mirrors.tuna.tsinghua.edu.cn/git/homebrew/install.git
        # （该地址需 git clone 后执行 install.sh，代理式直链可用 gitee 的 HomebrewCN 脚本）
        local install_url=${HOMEBREW_INSTALL_URL:-https://raw.githubusercontent.com/Homebrew/install/master/install}
        ruby -e "$(curl -fsSL ${install_url})" || exit 1
    fi
}

function check_tool() {
    if [[ -z "$1" ]];then
        echo "error no tool to check"
    else
        if [[ ! `which $1` ]]
        then
            brew install $1 || exit 1
        fi
    fi
}

function check_cmake(){
    if [[ ! `which cmake` ]]
    then
        echo 'cmake not found'
        echo 'Trying to install cmake...'
        brew install cmake || exit 1
    else
        local major=`cmake -version | head -1 | cut -d ' ' -f3 | cut -d '-' -f1 | cut -d '.' -f1`
        local minor=`cmake -version | head -1 | cut -d ' ' -f3 | cut -d '-' -f1 | cut -d '.' -f2`
        # require cmake >= 3.14 (handles 4.x too)
        if [[ ${major} -gt 3 ]] || [[ ${major} -eq 3 && ${minor} -ge 14 ]];then
            echo cmake version is ok ${major}.${minor}
        else
            brew upgrade cmake
        fi
    fi
}

function build_Android(){

    git submodule init
    git submodule update

    JAVA_HOME_OPT=
    if [ -n "$JAVA_HOME" ]
    then
        JAVA_HOME_OPT=-Dorg.gradle.java.home=${JAVA_HOME}
    fi

    cd ${TOP_DIR}/external
    ./build_external.sh Android
    if [ $? -ne 0 ]; then
        echo "build_external Android break"
        return 1
    fi
    export EXTERN_INSTALL_DIR_ANDROID=$PWD/install/
    export FFMPEG_INSTALL_DIR_ANDROID=$PWD/install/ffmpeg/Android/

    cd ${TOP_DIR}/platform/Android
    cd source/
    sh gradlew clean
    export ANDROID_FULL_PACKAGE='true'
    sh gradlew assembleRelease --refresh-dependencies --stacktrace $JAVA_HOME_OPT
    if [ $? -ne 0 ]; then
        echo "gradlew assembleRelease break"
        return 1
    fi

    cd ${TOP_DIR}/platform/Android
    ./package.sh

    cd $TOP_DIR
    mkdir -p output
    local apks=`find platform/Android -name "*.apk"`
    local aars=`find platform/Android/source/releaseLibs/ -name "*.aar"`
    local zips=`find platform/Android/release -name "*.zip"`
    [[ -n "${apks}" ]] && cp ${apks} output
    [[ -n "${aars}" ]] && cp ${aars} output
    [[ -n "${zips}" ]] && cp ${zips} output

    mkdir -p output/armeabi-v7a/
    mkdir -p output/arm64-v8a/

    cp  platform/Android/source/premierlibrary/build/intermediates/cmake/corePlayerRelease/obj/armeabi-v7a/*.so output/armeabi-v7a/
    cp  platform/Android/source/premierlibrary/build/intermediates/cmake/corePlayerRelease/obj/arm64-v8a/*.so output/arm64-v8a/

    cp  external/install/ffmpeg/Android/armeabi-v7a/libalivcffmpeg.so   output/armeabi-v7a/
    cp  external/install/ffmpeg/Android/arm64-v8a/libalivcffmpeg.so     output/arm64-v8a/

    cd output
    tree
}

# ============================================================================
# HarmonyOS / OpenHarmony 编译入口
# 用法: . setup.env && build_OHOS
# 要求: export OHOS_SDK=/path/to/ohos-sdk (含 native/llvm 与 native/sysroot)
# 详见 docs/Packaging_HarmonyOS.md
# ============================================================================
function build_OHOS(){
    if [[ -z "${OHOS_SDK}" ]] && [[ -z "${OHOS_SDK_HOME}" ]]; then
        echo "OHOS_SDK not set"
        echo "  e.g. export OHOS_SDK=/path/to/ohos-sdk   # must contain native/llvm + native/sysroot"
        return 1
    fi

    # 1. 交叉编译外部库 (FFmpeg 9.0 等) -> external/install/ffmpeg/OHOS/<abi>/
    cd ${TOP_DIR}/external
    ./build_external.sh OHOS
    if [ $? -ne 0 ]; then
        echo "build_external OHOS break"
        return 1
    fi

    # 2. 编译 HarmonyOS Demo hap (可选, 需要 DevEco 命令行工具 hvigorw)
    cd ${TOP_DIR}/platform/HarmonyOS
    if command -v hvigorw >/dev/null 2>&1; then
        hvigorw assembleHap --mode module -p product=default
        if [ $? -ne 0 ]; then
            echo "hvigorw assembleHap break"
            return 1
        fi
    else
        echo "hvigorw not found, skip demo hap build (native libraries are already built)"
    fi

    # 3. 收集产物
    cd ${TOP_DIR}
    mkdir -p output
    cp -r external/install/ffmpeg/OHOS output/ffmpeg-OHOS
    find platform/HarmonyOS -name "*.hap" -exec cp {} output/ \; 2>/dev/null
    echo "OHOS build done, artifacts in ${TOP_DIR}/output"
}

function packet_iOS(){
    rm -rf ${TOP_DIR}/output
    mkdir -p ${TOP_DIR}/output/
    mkdir -p ${TOP_DIR}/output/CicadaPlayerSDK/
    mkdir -p ${TOP_DIR}/output/CicadaPlayerSDK/CicadaDemo
    CicadaSDK_PATH=${TOP_DIR}/output/CicadaPlayerSDK/SDK/
    CicadaSDK_ALL=${CicadaSDK_PATH}/ARM_SIMULATOR
    CicadaSDK_ARM=${TOP_DIR}/output/CicadaPlayerSDK/SDK/ARM
    mkdir -p ${CicadaSDK_ALL}
    mkdir -p ${CicadaSDK_ARM}

    #cp clear CicadaDemo at first
    cp -rf CicadaDemo/ ${TOP_DIR}/output/CicadaPlayerSDK/CicadaDemo

    cd ${DEMO_SOURCE_DIR_IOS}/SDK/
    ALL_FRAMEWORK_PATH=${DEMO_SOURCE_DIR_IOS}/SDK/ARM_SIMULATOR
    mkdir -p ${ALL_FRAMEWORK_PATH}
    #build iphoneos
    xcodebuild -scheme ALL_BUILD ONLY_ACTIVE_ARCH=NO -configuration MinSizeRel -sdk iphoneos VALID_ARCHS="armv7 arm64"
    cp -rf ./MinSizeRel/*.framework ${ALL_FRAMEWORK_PATH}
    cp -rf ./MinSizeRel/*.dSYM ${CicadaSDK_ALL}/

    cp -rf ./MinSizeRel/*.framework ${CicadaSDK_ARM}/

    #build simulator
    xcodebuild -scheme ALL_BUILD ONLY_ACTIVE_ARCH=NO -configuration Release -sdk iphonesimulator VALID_ARCHS="x86_64 i386"
    if [ $? -ne 0 ]; then
        echo "simulator build failed"
    else
        lipo -create "${ALL_FRAMEWORK_PATH}/CicadaPlayerSDK.framework/CicadaPlayerSDK" "./Release/CicadaPlayerSDK.framework/CicadaPlayerSDK" -output "${ALL_FRAMEWORK_PATH}/CicadaPlayerSDK.framework/CicadaPlayerSDK"
        lipo -create "${ALL_FRAMEWORK_PATH}/alivcffmpeg.framework/alivcffmpeg" "./Release/alivcffmpeg.framework/alivcffmpeg" -output "${ALL_FRAMEWORK_PATH}/alivcffmpeg.framework/alivcffmpeg"
    fi

    # copy to SDK folder
    cp -rf ${ALL_FRAMEWORK_PATH}/*.framework ${CicadaSDK_ALL}/

    nobit_path=${CicadaSDK_PATH}/ARM_NO_BITCODE
    mkdir -p ${nobit_path}
    cp -rf ${CicadaSDK_ARM}/*.framework ${nobit_path}
    xcrun bitcode_strip ${nobit_path}/CicadaPlayerSDK.framework/CicadaPlayerSDK -r -o ${nobit_path}/CicadaPlayerSDK.framework/CicadaPlayerSDK
    xcrun bitcode_strip ${nobit_path}/alivcffmpeg.framework/alivcffmpeg -r -o ${nobit_path}/alivcffmpeg.framework/alivcffmpeg

    cd ${TOP_DIR}/output/
    tar -cjvf ${TOP_DIR}/output/CicadaPlayerSDK_${MUPP_BUILD_ID}.bz2 CicadaPlayerSDK/
}

function build_iOS_new(){
    DEMO_SOURCE_DIR_IOS=${TOP_DIR}/platform/Apple/demo/iOS
    cd ${DEMO_SOURCE_DIR_IOS}
    ./genxcodeproj.sh
    packet_iOS
}

function build_iOS(){
    check_brew

    if [[ -n "$MTL" ]];then
        check_cmake
    else
       check_tool "cmake"
    fi
    check_tool "yasm"
    check_tool "automake"

    if [[ $? -ne 0 ]]; then
        return 1
    fi

    cd ${TOP_DIR}/external
    ./build_external.sh iOS

    if [[ $? -ne 0 ]]; then
        echo "build_external break"
        return 1
    fi

    build_iOS_new
    if [[ ! -f "${TOP_DIR}/output/CicadaPlayerSDK/SDK/ARM_SIMULATOR/CicadaPlayerSDK.framework/CicadaPlayerSDK" ]]; then
        echo "CicadaPlayerSDK.framework build failed"
        return  1
    fi
}

function packet_mac(){
    mkdir ${TOP_DIR}/output
    OUT_PUT_IDR=${TOP_DIR}/output

    cd ${DEMO_SOURCE_DIR_MAC}/SDK
    xcodebuild -scheme install ONLY_ACTIVE_ARCH=NO -configuration MinSizeRel
    #build app without SDK
    mv CicadaPlayerSDK.xcodeproj CicadaPlayerSDKBak.xcodeproj

    cd ${DEMO_SOURCE_DIR_MAC}/
    mkdir CicadaDemo_release
    cp -rf CicadaDemo CicadaDemo_release/
    mkdir -p CicadaDemo_release/SDK/

    cd ${DEMO_SOURCE_DIR_MAC}/CicadaDemo
    xcodebuild -workspace CicadaDemo.xcworkspace -scheme CicadaDemo ONLY_ACTIVE_ARCH=NO -configuration Release

    cd ${DEMO_SOURCE_DIR_MAC}/Release
    tar -cjvf ${OUT_PUT_IDR}/CicadaDemo.app.bz2  CicadaDemo.app
    tar -cjvf ${OUT_PUT_IDR}/CicadaDemo.app.dSYM.bz2  CicadaDemo.app.dSYM

    cd ${DEMO_SOURCE_DIR_MAC}
    mv ./SDK/CicadaPlayerSDKBak.xcodeproj ./SDK/CicadaPlayerSDK.xcodeproj
    mv  SDK/*.framework CicadaDemo_release/SDK/
    mv  SDK/MinSizeRel/*.dSYM CicadaDemo_release/SDK/
    tar -cjvf ${OUT_PUT_IDR}/CicadaDemo_release.bz2 CicadaDemo_release
}

function build_mac(){
    if [[ -n "$MTL" ]];then
        export HOMEBREW_NO_AUTO_UPDATE=true
    fi
    check_brew

    if [[ -n "$MTL" ]];then
        check_cmake
    else
       check_tool "cmake"
    fi
    check_tool "yasm"
    check_tool "automake"

    if [[ $? -ne 0 ]]; then
        return 1
    fi

    cd ${TOP_DIR}/external
    ./build_external.sh macOS

    if [[ $? -ne 0 ]]; then
        echo "build_external break"
        return  1
    fi

    DEMO_SOURCE_DIR_MAC=${TOP_DIR}/platform/Apple/demo/macOS
    cd ${DEMO_SOURCE_DIR_MAC}
    ./Genxcodeproj.sh

    if [[ -n "$MTL" ]];then
        packet_mac
    fi

    cd ${TOP_DIR}
}
