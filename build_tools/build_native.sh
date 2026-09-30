#!/usr/bin/env bash


BUILD_TOOLS_DIR=$(cd $(dirname ${BASH_SOURCE[0]}); pwd)

PATH=$PATH:${BUILD_TOOLS_DIR}

source env.sh
source common_build.sh
source utils.sh

CWD=$PWD

function create_cmake_config(){
   echo "cmake_minimum_required(VERSION 3.6)" >> $CONFIG_FILE
   echo "set(LIB_NAME ${LIB_NAME})" >> $CONFIG_FILE
   echo "find_library(AUDIO_TOOL_BOX AudioToolbox)" >> $CONFIG_FILE
   echo "find_library(VIDEO_TOOL_BOX VideoToolbox)" >> $CONFIG_FILE
   echo "find_library(COREMEDIA CoreMedia)" >> $CONFIG_FILE
   echo "find_library(COREVIDEO CoreVideo)" >> $CONFIG_FILE
   echo "find_library(COREFOUNDATION CoreFoundation)" >> $CONFIG_FILE
   echo "find_library(SECURITY Security)" >> $CONFIG_FILE
   echo "find_library(LIBXML2 Xml2)" >> $CONFIG_FILE
   echo -n "set(SRC_LIBRARIES ${SRC_LIBRARIES}" >> $CONFIG_FILE
   echo -n ' ${AUDIO_TOOL_BOX}' >> $CONFIG_FILE
   echo -n ' ${VIDEO_TOOL_BOX}' >> $CONFIG_FILE
   echo -n ' ${COREMEDIA}' >> $CONFIG_FILE
   echo -n ' ${COREVIDEO}' >> $CONFIG_FILE
   echo -n ' ${COREFOUNDATION}' >> $CONFIG_FILE
   if [[ "${SSL_USE_NATIVE}" == "TRUE" ]];then
      echo -n ' ${SECURITY}' >> $CONFIG_FILE
   fi

   if [[ "${XML_USE_NATIVE}" == "TRUE" ]];then
       echo -n ' ${LIBXML2}' >> $CONFIG_FILE
   fi
   echo ")" >> $CONFIG_FILE
   echo "set(SRC_LIBRARIES_DIR ${SRC_LIBRARIES_DIR})" >>$CONFIG_FILE
}

#build to ffmpeg
function build_shared_framework(){
    #
    # 这一步要用 cmake 生成 Xcode 工程、再调 xcodebuild。**两个都必须先检查**：
    # 脚本没有 set -e，缺了 cmake 时以前会一路跑到 xcodebuild，然后报一句
    #     xcodebuild: error: The directory .../install/ffmpeg/Darwin/Xcode does not
    #     contain an Xcode project.
    # 那是"假象"—— 真因是上面那次 cmake 直接 `command not found` 了。这里先报清楚。
    #
    if ! command -v cmake >/dev/null 2>&1; then
        # brew 装好的 cmake 可能在 /opt/homebrew/bin（Apple Silicon）或 /usr/local/bin，
        # 而非交互式 shell 的 PATH 里未必有 —— 先补一次再判。
        local prefix
        for prefix in /opt/homebrew/bin /usr/local/bin; do
            if [ -x "${prefix}/cmake" ]; then
                PATH="${prefix}:${PATH}"
                export PATH
                break
            fi
        done
    fi

    if ! command -v cmake >/dev/null 2>&1; then
        echo "ERROR: cmake not found —— 这一步要用它生成 Xcode 工程（紧接着会调 xcodebuild）。" >&2
        echo "       装好再重跑： brew install cmake" >&2
        echo "       （只给 Qt 播放器编静态库的话，也可以 BUILD_SHARED_LIB=FALSE 跳过这一步；" >&2
        echo "         这一步产出的是给 iOS/macOS 框架用的 libffmpeg.dylib。）" >&2
        exit 1
    fi

    if ! command -v xcodebuild >/dev/null 2>&1; then
        echo "ERROR: xcodebuild not found —— 装 Xcode 命令行工具： xcode-select --install" >&2
        exit 1
    fi

    if [ -z "${LIB_NAME}" ];then
        export LIB_NAME=ffmpeg
    fi
    SRC_LIBRARIES="$(cd ./install/ffmpeg/Darwin/x86_64/lib; ls *.a)"
    SRC_LIBRARIES="$SRC_LIBRARIES z iconv resolv bz2"
    export CC=
    export AS=
    export CPU_FLAGS=
    export LDFLAGS=

    local support_libs="fdk-aac x264 curl librtmp cares dav1d nghttp2"

    if [[ "${XML_USE_NATIVE}" == "FALSE" ]];then
        support_libs="${support_libs} libxml2"
    fi

    if [[ "${SSL_USE_NATIVE}" != "TRUE" ]];then
        support_libs="${support_libs} openssl"
    fi
    MAC_ARCH=$(uname -m)
    SRC_LIBRARIES_DIR="$CWD/install/ffmpeg/Darwin/${MAC_ARCH}/lib"

    for support_lib in ${support_libs}
    do
        if [ -d "install/${support_lib}/Darwin/${MAC_ARCH}/lib" ];then
            SRC_LIBRARIES_DIR="$SRC_LIBRARIES_DIR $CWD/install/${support_lib}/Darwin/${MAC_ARCH}/lib"
            local libs="$(cd install/${support_lib}/Darwin/${MAC_ARCH}/lib; ls *.a)"
            SRC_LIBRARIES="$SRC_LIBRARIES $libs"
        fi
    done

    cd ./install/ffmpeg/Darwin/
    cp ${BUILD_TOOLS_DIR}/iOS/CMakeLists.txt ./

    CONFIG_FILE=config.cmake
    rm -rf ${CONFIG_FILE}

    create_cmake_config

    cp ${BUILD_TOOLS_DIR}/src/build_version.cpp ./
    sh ${BUILD_TOOLS_DIR}/gen_build_version.sh > version.h
    rm -rf Xcode/
    mkdir -p Xcode
    cd Xcode
    cmake ../ -G Xcode
    xcodebuild -configuration MinSizeRel -target ALL_BUILD

    return;
}

if [[ "$(uname)" == "Darwin" ]];then
  build_libs Darwin "${MACOS_ARCHS}"
else
  build_libs $(uname) x86_64 #$((uname -m)) Travis use uname -m get a 0...
fi

if [[ "$(uname)" == "Darwin"  &&  "${BUILD_SHARED_LIB}" != "FALSE" ]];then
    build_shared_framework
fi