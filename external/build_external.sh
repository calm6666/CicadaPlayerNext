#!/usr/bin/env bash

BUILD_TOOLS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${BUILD_TOOLS_DIR}/.." && pwd)
export TOP_DIR=${PWD}
PATH=$PATH:${BUILD_TOOLS_DIR}
# 用绝对路径 source，避免 CRLF/工作目录不同导致函数加载失败
source "${REPO_ROOT}/build_tools/utils.sh"
source user_env.sh

function git_am_patch() {
    local patch=$1
    local tmp_patch
    tmp_patch=$(mktemp /tmp/cicada-patch-XXXXXX.patch)
    # 补丁或源码可能是 Windows 检出(CRLF)：统一转 LF 后再打，避免上下文不匹配
    tr -d '\r' < "${patch}" > "${tmp_patch}"
    git config core.autocrlf false
    git am --abort 2>/dev/null      # 清理上一次失败残留的 am 状态
    git checkout -- . 2>/dev/null   # 把 CRLF worktree 还原成仓库内的 LF 内容
    git -c core.whitespace=cr-at-eol am "${tmp_patch}"
    local ret=$?
    rm -f "${tmp_patch}"
    if [ ${ret} -ne 0 ]; then
        print_warning "patch error, may be patched (${patch})"
        git am --abort
    fi
}

function patch_ffmpeg() {

    cd ${FFMPEG_SOURCE_DIR}
#    git_am_patch ../../contribute/ffmpeg/0001-chore-win32-fix-not-find-openssl-1.1.1.patch
#    git_am_patch ../../contribute/ffmpeg/0002-chore-avformat-change-MAX_PES_PAYLOAD-to-5M.patch
    git_am_patch ../../contribute/ffmpeg/0003-chore-libavformat-exoport-some-functions.patch
#    git_am_patch ../../contribute/ffmpeg/0004-chore-fix-Linux-build.patch
#    git_am_patch ../../contribute/ffmpeg/0005-chore-disable-libdav1d-check.patch
    git_am_patch ../../contribute/ffmpeg/0006-flv-add-extensions-for-H.265-HEVC.patch
    git_am_patch ../../contribute/ffmpeg/0007-build-libavformat-compile-avc.c-and-hevc.c-default.patch
    git_am_patch ../../contribute/ffmpeg/0008-avformat-flvdec-add-aliyun-extend-auio-codec-id.patch
    git_am_patch ../../contribute/ffmpeg/0009-chore-configure-xml2-use-check-lib.patch
    git_am_patch ../../contribute/ffmpeg/0010-build-export-some-func-for-windows.patch
    git_am_patch ../../contribute/ffmpeg/0011-libavformat-flvdec-add-flv_strict_header-option-to-d.patch
}

function patch_openssl() {
    cd "${OPEN_SSL_SOURCE_DIR}" || exit
    local major
    major=$(sed -n 's/^# *define *OPENSSL_VERSION_MAJOR *\([0-9][0-9]*\).*/\1/p' \
                 include/openssl/opensslv.h 2>/dev/null | head -1)
    if [[ "${major:-1}" -lt 3 ]]; then
        # Apple silicon 补丁仅适用于 1.1.1（OpenSSL 3.x 原生支持 darwin64-arm64-cc）
        git_am_patch ../../contribute/openssl/0001-Configuration-darwin64-arm64-cc-for-Apple-silicon.patch
    else
        echo "openssl ${major}.x: skip 1.1.1-era patch"
    fi
}

function patch_curl(){
    cd "${CURL_SOURCE_DIR}" || exit
    # m4 AC_REQUIRE 补丁仅适用于 curl 7.68（curl 8.x 上游已修复）
    local ver
    ver=$(git describe --tags 2>/dev/null | cut -d- -f1)
    if [[ "${ver}" == curl-7_* ]]; then
        git_am_patch ../../contribute/curl/0001-curl-functions.m4-remove-inappropriate-AC_REQUIRE.patch
    else
        echo "curl ${ver:-8.x}: skip 7.x-era patch"
    fi
}

function git_apply_patch() {
    git apply $1
    if [[ $? -ne 0 ]]; then
        print_warning "patch error, may be patched"
        git am --abort
    fi
}

function patch_dav1d() {
    cd ${DAV1D_SOURCE_DIR}
    if [[ "$TARGET_PLATFORM" == "iOS" ]];then
        git_apply_patch ${REPO_ROOT}/external/contribute/dav1d/0001-chore-enable-bitcode.patch
    else
        git reset --hard HEAD #reset the ios patch
    fi

}

function load_source() {
    local user_sources=$(cd ${PWD};ls ../*_git_source_list.sh)
    local user_source
    echo "user_sources is ${user_sources}"
    if [[ "${user_sources}" == "" ]];then
        print_error "no source list config found"
        return 1;
    else
        for user_source in ${user_sources}
        do
            print_warning "apply "${user_source}""
            source ${user_source}
        done
    fi

    # 统一修复 Windows 检出问题：源码树行尾统一为 LF
    # （core.autocrlf=false + checkout 会按仓库内容重写 worktree）
    local src_dir
    for src_dir in "${FFMPEG_SOURCE_DIR}" "${OPEN_SSL_SOURCE_DIR}" "${NGHTTP2_SOURCE_DIR}" \
                   "${CURL_SOURCE_DIR}" "${LIBXML2_SOURCE_DIR}" "${DAV1D_SOURCE_DIR}"; do
        if [[ -n "${src_dir}" && -d "${src_dir}/.git" ]]; then
            echo "normalize EOL for ${src_dir}"
            (cd "${src_dir}" && git config core.autocrlf false && git checkout -- . 2>/dev/null || true)
        fi
    done

    if [[ ${FFMPEG_NEED_PATCH} == "TRUE" ]];then
        patch_ffmpeg
    fi

    if [[ -d ${LIBXML2_SOURCE_DIR} ]];then
        cd ${LIBXML2_SOURCE_DIR}
        git_am_patch ../../contribute/libxml2/0001-disable-check-libtool.patch
    fi

    if [[ -d ${OPEN_SSL_SOURCE_DIR} ]];then
        patch_openssl
    fi

    if [[ -d ${CURL_SOURCE_DIR} ]];then
        patch_curl
    fi
    return 0;
}

function check_android_tools() {
    if  [ -z "${ANDROID_NDK}" ];then
        ANDROID_NDK=$ANDROID_NDK_HOME #mtl's Android ndk
    else
        ANDROID_NDK_HOME=$ANDROID_NDK
    fi
    echo ANDROID_NDK is ${ANDROID_NDK}
    # NDK r25+: toolchain lives in toolchains/llvm/prebuilt/<host>/bin.
    # 无条件把 NDK bin 放在 PATH 最前，确保 configure/clang 探测优先使用
    # NDK 自带的 clang（避免被系统 clang 干扰导致交叉编译问题）。
    local ndk_host
    case "$(uname -s)" in
        Darwin*) ndk_host=darwin-x86_64 ;;
        *)       ndk_host=linux-x86_64 ;;
    esac
    if [[ -d "${ANDROID_NDK}/toolchains/llvm/prebuilt/${ndk_host}/bin" ]]; then
        PATH=${ANDROID_NDK}/toolchains/llvm/prebuilt/${ndk_host}/bin:$PATH
        echo "prepend NDK clang bin: ${ANDROID_NDK}/toolchains/llvm/prebuilt/${ndk_host}/bin"
    else
        echo "WARNING: NDK clang bin not found under ${ANDROID_NDK}/toolchains/llvm/prebuilt/${ndk_host}/bin"
    fi
}

function apply_config() {
    local configs="$1";
    local config;

    for config in $configs ; do
        cp config/${config}_ffmpeg_config.sh ./
        cp config/${config}_git_source_list.sh ./
    done
}

function check_cmake(){
    if [ ! `which cmake` ]
    then
        echo 'cmake not found'
        if [ ! `which brew` ]
        then
            echo 'Homebrew not found. Trying to install...'
            # 国内可用 HOMEBREW_INSTALL_URL 覆盖安装脚本地址
            local install_url=${HOMEBREW_INSTALL_URL:-https://raw.githubusercontent.com/Homebrew/install/master/install}
            ruby -e "$(curl -fsSL ${install_url})" || exit 1
        fi
        echo 'Trying to install cmake...'
        brew install cmake || exit 1
    fi
#    brew upgrade cmake
}

function check_meson(){
    if [ ! `which meson` ]
    then
        echo 'meson not found'
        echo 'Trying to install meson...'
        brew install meson || exit 1
    fi
#    brew upgrade meson
}

function check_ninja(){
    if [ ! `which ninja` ]
    then
        echo 'ninja not found'
        echo 'Trying to install ninja...'
        brew install ninja || exit 1
    fi
#    brew upgrade ninja
}

function check_nasm(){
    if [ ! `which nasm` ]
    then
        echo 'nasm not found'
        echo 'Trying to install nasm...'
        brew install nasm || exit 1
    fi
#    brew upgrade nasm
}

function check_dav1d(){
    if [ -d "${DAV1D_SOURCE_DIR}" ]
    then
        check_meson
        check_ninja
        check_nasm
        patch_dav1d
        cd ${TOP_DIR}
    else
        echo "DAV1D_SOURCE_DIR not enable"
    fi
}

function check_yasm(){
    if [ ! `which yasm` ]
    then
        echo 'yasm not found'
        if [ ! `which brew` ]
        then
            echo 'Homebrew not found. Trying to install...'
            # 国内可用 HOMEBREW_INSTALL_URL 覆盖安装脚本地址
            local install_url=${HOMEBREW_INSTALL_URL:-https://raw.githubusercontent.com/Homebrew/install/master/install}
            ruby -e "$(curl -fsSL ${install_url})" || exit 1
        fi
        echo 'Trying to install yasm...'
        brew install yasm
    fi
    echo `yasm --version`

    if [ ! `which nasm` ] || [ `which nasm` == "/usr/bin/nasm" ]
    then
        echo 'nasm not found'
        brew install nasm
        alias nasm=/usr/local/bin/nasm
        PATH=/usr/local/bin:$PATH
    else
        echo nasm is `which nasm`
    fi
    echo nasm is `which nasm`
    echo `nasm --version`
}

if [[ -f "${CICADA_FFMPEG_CONFIG_FILE}" ]]; then
    rm player_ffmpeg_config.sh
    cp "${CICADA_FFMPEG_CONFIG_FILE}" ./
fi

if [[ -f "${CICADA_GIT_SOURCE_LIST_FILE}" ]]; then
    rm  player_git_source_list.sh
    cp "${CICADA_GIT_SOURCE_LIST_FILE}" ./
fi

mkdir external
cd external
load_source
if [[ $? -ne 0 ]]; then
    echo "load_source error break"
    exit 1;
fi

cd ${TOP_DIR}

export TARGET_PLATFORM=$1

if [[ "$1" == "Android" ]];then
    if  [[ -z "${ANDROID_NDK}" ]];then
        export ANDROID_NDK=~/Android-env/android-ndk-r25c/
    fi
    check_android_tools
    check_dav1d
    # 用 bash 显式执行，避免脚本丢失可执行位时报 Permission denied
    bash ${REPO_ROOT}/build_tools/build_Android.sh

elif [[ "$1" == "iOS" ]];then
    #export HOMEBREW_NO_AUTO_UPDATE=true
    check_cmake
    check_yasm
    check_dav1d
    bash ${REPO_ROOT}/build_tools/build_iOS.sh
elif [[ "$1" == "macOS" ]];then
    bash ${REPO_ROOT}/build_tools/build_native.sh
elif [[ "$1" == "Linux" ]];then
    bash ${REPO_ROOT}/build_tools/build_native.sh
elif [[ "$1" == "Windows" ]];then
    bash ${REPO_ROOT}/build_tools/build_win32.sh
elif [[ "$1" == "maccatalyst" ]];then
    bash ${REPO_ROOT}/build_tools/build_maccatalyst.sh
elif [[ "$1" == "OHOS" ]];then
    bash ${REPO_ROOT}/build_tools/build_ohos.sh
fi




