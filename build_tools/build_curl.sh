#!/usr/bin/env bash

source cross_compile_env.sh
source native_compile_env.sh

#
# Apple 平台的 TLS 后端选项名。
#
# curl **7.81** 起把 Darwin 的 TLS 后端从 `--with-darwinssl` 改名为
# `--with-secure-transport`。老名字在新版里是**无法识别的选项**：autotools 只会打一条
# "unrecognized options" 警告然后继续，结果一个 TLS 后端都没选中，configure 直接失败：
#     configure: error: select TLS backend(s) or disable TLS with --without-ssl.
# 本仓库锁的是 curl-8_14_1（external/player_git_source_list.sh:105），必须用新名字；
# 这里按源码里的 curlver.h 探一次版本，兼容还在用 7.x 源码树的场景。
function curl_apple_tls_option(){
    local header="${CURL_SOURCE_DIR}/include/curl/curlver.h"
    local major=0
    local minor=0

    if [ -f "${header}" ]; then
        major=$(sed -n 's/^#define LIBCURL_VERSION_MAJOR *\([0-9][0-9]*\).*/\1/p' "${header}" | head -1)
        minor=$(sed -n 's/^#define LIBCURL_VERSION_MINOR *\([0-9][0-9]*\).*/\1/p' "${header}" | head -1)
    fi

    # 探不到版本时按"新名字"给（本仓库唯一的版本就是 8.14.1）
    if [ -z "${major}" ]; then
        major=8
        minor=0
    fi

    if [ "${major}" -gt 7 ] || { [ "${major}" -eq 7 ] && [ "${minor}" -ge 81 ]; }; then
        echo "--with-secure-transport"
    else
        echo "--with-darwinssl"
    fi
}

function build_curl(){
    if [ ! -f ${CURL_SOURCE_DIR}/configure ]
    then
        cd ${CURL_SOURCE_DIR}
        autoreconf -fi
        if [ -n "$MTL" ]
        then
            cp ${BUILD_TOOLS_DIR}/automake_config/config.guess ./
            cp ${BUILD_TOOLS_DIR}/automake_config/config.sub ./
        fi
        cd -
    fi

    LIBSDEPEND=""

    local ssl_opt=

    export CFLAGS="${HARDENED_CFLAG}"

    if [ "$1" == "Android" ]
    then
        cross_compile_set_platform_Android  $2
        # 静态库最终要合并进 libffmpeg.so，ARM32 必须显式 -fPIC，
        # 否则 lld 报 "relocation R_ARM_REL32 ... recompile with -fPIC"
        export CFLAGS="${CFLAGS} -fPIC"
    elif [ "$1" == "iOS" ]
    then
        LIBSDEPEND="LIBS=-lresolv"
        cross_compile_set_platform_iOS $2
        if [ "$2" == "x86_64" ] || [ "$2" == "i386" ]; then
            SYSROOT=${IPHONESIMULATOR_SDK}
        else
            SYSROOT=${IPHONEOS_SDK}
        fi
        export CFLAGS="${CFLAGS} -arch $2 -fembed-bitcode --sysroot=$SYSROOT -isysroot $SYSROOT -miphoneos-version-min=$DEPLOYMENT_TARGET"
        export LDFLAGS="-arch $2 --sysroot=$SYSROOT"
        export CC=clang
        if [[ "${SSL_USE_NATIVE}" == "TRUE" ]];then
            ssl_opt="$(curl_apple_tls_option)"
        fi
    elif [[ "$1" == "win32" ]];then
        cross_compile_set_platform_win32 $2
        export CFLAGS="-DNGHTTP2_STATICLIB"
    elif [[ "$1" == "Darwin" ]];then
        LIBSDEPEND="LIBS=-lresolv"
        if [[ "${SSL_USE_NATIVE}" == "TRUE" ]];then
            ssl_opt="$(curl_apple_tls_option)"
        fi
        print_warning "native build curl for $1 $2"
        native_compile_set_platform_macOS $2
        export CFLAGS="${CFLAGS} $CPU_FLAGS"
    elif [[ "$1" == "Linux" ]];then
        LIBSDEPEND="LIBS=-lresolv"
        print_warning "native build for $1"
    elif [ "$1" == "maccatalyst" ];then
        LIBSDEPEND="LIBS=-lresolv"
        if [[ "${SSL_USE_NATIVE}" == "TRUE" ]];then
            ssl_opt="$(curl_apple_tls_option)"
        fi
        cross_compile_set_platform_maccatalyst "$2"
        export CFLAGS="${CPU_FLAGS}"
        export LDFLAGS="${CPU_LDFLAGS}"
        export CC=clang
    elif [ "$1" == "OHOS" ];then
        # 鸿蒙走 OpenSSL（不使用任何 darwinssl / Apple 相关选项）
        cross_compile_set_platform_OHOS  $2
        export CFLAGS="${CFLAGS} -fPIC"
        # 系统 automake 的 config.sub 可能不认识 ohos 三元组，先装 shim
        patch_config_sub_for_ohos "${CURL_SOURCE_DIR}" || exit 1
        # curl 的 -host 用 TARGET_TRIPLE（OHOSConfig 未定义 CROSS_COMPILE）
        CROSS_COMPILE=${TARGET_TRIPLE}
    else
        echo "curl Unsupported platform $1"
        exit 1;
    fi

# \
    local config="--enable-shared=no \
                --disable-symbol-hiding \
                --enable-proxy \
                --disable-debug \
                --enable-optimize \
                --disable-ftp \
                --disable-gopher \
                --disable-file \
                --disable-imap \
                --disable-ldap \
                --disable-ldaps \
                --disable-pop3 \
                --disable-rtsp \
                --disable-smtp \
                --disable-telnet \
                --disable-tftp \
                --disable-smb \
                --disable-smbs \
                --disable-mqtt \
                --disable-dict \
                --without-gnutls \
                --without-libidn2 \
                --without-librtmp \
                --without-brotli \
                --without-libidn \
                --without-zstd \
                --without-libpsl"
    local build_dir="${CWD}/build/curl/$1/$2"
    local install_dir="${CWD}/install/curl/$1/$2"

    mkdir -p ${build_dir}/
    if [ "${BUILD}" != "False" ];then
        cd ${build_dir}
        if [ -z "${ssl_opt}" ] && [ -d "${OPENSSL_INSTALL_DIR}" ];then
            # curl 8.x 规范选项 --with-openssl（旧 --with-ssl 为弃用别名）
            local ssl_opt="--with-openssl=${OPENSSL_INSTALL_DIR}"
        fi

        if [ -d "${ARES_INSTALL_DIR}" ];then
            local resolver_opt="--enable-ares=${ARES_INSTALL_DIR}"
        else
            local resolver_opt="--enable-threaded-resolver"
        fi

        if [ -d "${LIBRTMP_INSTALL_DIR}" ];then
            local rtmp_opt="--with-librtmp=${LIBRTMP_INSTALL_DIR}"
        fi

        if [[ -d "${NGHTTP2_INSTALL_DIR}" ]];then
            local nghttp2_opt="--with-nghttp2=${NGHTTP2_INSTALL_DIR}"
         else
            local nghttp2_opt="--without-nghttp2"
        fi

        ${CURL_SOURCE_DIR}/configure -host=${CROSS_COMPILE} CC="${CC}" ${ssl_opt} ${resolver_opt} ${config} ${rtmp_opt} ${nghttp2_opt} --prefix=${install_dir} ${LIBSDEPEND} || exit 1
        make -j8 install V=1 || exit 1
        cd -
    fi
    CURL_INSTALL_DIR=${install_dir}
    export CFLAGS=""
}
