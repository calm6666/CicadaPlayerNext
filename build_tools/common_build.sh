#!/usr/bin/env bash

source build_x264.sh
source build_openssl_3.sh
source build_curl.sh
source build_dav1d.sh
source build_fdk_aac.sh
source ffmpeg_commands.sh
source build_ffmpeg.sh
source build_librtmp.sh
source build_ares.sh
source build_boost.sh
source build_libxml2.sh
source build_nghttp2.sh

function apply_ffmpeg_config(){
    ffmpeg_config_reset
    local user_configs=$(cd ${CWD};ls *_ffmpeg_config.sh)
    local user_config
    if [ "$user_configs" == "" ];then
        source ffmpeg_default_config.sh
    else
        for user_config in ${user_configs}
        do
            echo apply ${user_config} to ffmpeg config
            source ${user_config}
        done
    fi
}
function build_static_lib(){
    local arch=$2
    export TARGET_ARCH=$2
    cd ${CWD}

    # Windows 检出/拷贝会丢失脚本可执行位，统一补齐常见构建入口
    # （configure / autogen.sh / buildconf / OpenSSL 的 Configure）
    local src
    for src in "${LIBXML2_SOURCE_DIR}" "${OPEN_SSL_SOURCE_DIR}" "${CURL_SOURCE_DIR}" \
               "${NGHTTP2_SOURCE_DIR}" "${FDK_AAC_SOURCE_DIR}" "${X264_SOURCE_DIR}" \
               "${FFMPEG_SOURCE_DIR}" "${ARES_SOURCE_DIR}"; do
        if [[ -n "$src" && -d "$src" ]]; then
            chmod +x "$src/configure" "$src/autogen.sh" "$src/buildconf" "$src/Configure" 2>/dev/null
        fi
    done

    local build_xml="true"
    if [[ "$1" == "iOS" ]] || [[ "$1" == "Darwin" ]] || [[ "$1" == "maccatalyst" ]];then
        if [[ "${XML_USE_NATIVE}" == "TRUE" ]];then
            build_xml="false"
        fi
    fi
    if [[ -d "${LIBXML2_SOURCE_DIR}" ]] && [[ "${build_xml}" == "true" ]];then
        build_libxml2  $1 ${arch}
    fi

    if [ -d "$BOOST_SOURCE_DIR" ];then
        build_boost $1 ${arch}
    else
        print_warning "boost source not found"
    fi

    if [[ -d "${ARES_SOURCE_DIR}" ]];then
        build_ares $1 ${arch}
        if [[ $? -ne 0 ]]; then
            echo "build_ares build failed"
            exit -1
        fi
    else
        print_warning "cares source not found"
    fi

    local build_openssl="true"
    if [[ "$1" == "iOS" ]] || [[ "$1" == "Darwin" ]] || [[ "$1" == "maccatalyst" ]];then
        if [[ "${SSL_USE_NATIVE}" == "TRUE" ]] && [[ "$CRYPTO_USE_OPENSSL" != "TRUE" ]];then
            build_openssl="false"
        fi
    fi

    if [[ -d "${OPEN_SSL_SOURCE_DIR}" ]] && [[ "${build_openssl}" == "true" ]];then
        # build_openssl_3 已按源码版本自动适配（OpenSSL 3.x 使用新配置清单与
        # 现代 NDK 布局；1.1.1 回退时自动 shim 旧 NDK 目录）。OPENSSL_VERSION_3
        # 标志保留为历史兼容，不再影响构建路径。
        build_openssl_3 $1 ${arch}
        if [[ $? -ne 0 ]]; then
            echo "build_openssl build failed"
            exit -1
        fi
    else
        print_warning "openssl source not found"
    fi

    if [[ -d "$RTMPDUMP_SOURCE_DIR" ]];then
        build_librtmp $1 ${arch}
        if [[ $? -ne 0 ]]; then
            echo "build_librtmp build failed"
            exit -1
        fi
    else
        print_warning "RTMPDUM source not found"
    fi

    if [[ -d "$NGHTTP2_SOURCE_DIR" ]];then

        build_nghttp2  $1 ${arch}
        if [[ $? -ne 0 ]]; then
            echo "build_nghttp2 build failed"
            exit -1
        fi

    fi

    if [[ -d "${CURL_SOURCE_DIR}" ]];then
        build_curl $1 ${arch}
        if [[ $? -ne 0 ]]; then
            echo "build_curl build failed"
            exit -1
        fi
    else
        print_warning "openssl source not found"
    fi
    if [[ -d "${FDK_AAC_SOURCE_DIR}" ]]
    then
        build_fdk_aac $1 ${arch}
        if [[ $? -ne 0 ]]; then
            echo "build_fdk_aac build failed"
            exit -1
        fi
    else
        print_warning "fdk-aac source not found"
    fi
    if [[ -d "${X264_SOURCE_DIR}" ]]
    then
        build_x264 $1 ${arch}
        if [[ $? -ne 0 ]]; then
            echo "build_x264 build failed"
            exit -1
        fi
    else
        print_warning "x264 source not found"
    fi

    if [[ -d "${DAV1D_SOURCE_DIR}" ]]
    then
        cd ${CWD}
        build_dav1d $1 ${arch}
        if [[ $? -ne 0 ]]; then
            echo "build_dav1d build failed"
            exit -1
        fi
    else
        print_warning "dav1d source not found"
    fi

    if [[ -d "${FFMPEG_SOURCE_DIR}" ]]
    then
        cd ${CWD}
        apply_ffmpeg_config
        build_ffmpeg $1 ${arch}
    else
        print_warning "ffmpeg source not found"
    fi
    cd ${CWD}

}
# OHOS：给不认识 ohos 三元组的旧版 config.sub 装一个 shim。
# curl/nghttp2 经 autoreconf 使用的是系统 automake 的 config.sub（Ubuntu 22.04
# 的版本早于 2021-11，不认识 ohos）；libxml2 已升级到 2.14.5（自带新版
# config.sub，本函数会自动跳过）。直接传 --host=aarch64-linux-ohos 会报
# "Invalid configuration"。
# shim 把 *-ohos* 三元组规范化为对应的 linux-gnu（OHOS sysroot 是 Linux
# 风格 musl，autoconf 层面按 Linux 处理是正确的），其余输入原样转发。
function patch_config_sub_for_ohos(){
    local sub="$1/config.sub"
    [[ -f "${sub}" ]] || { echo "config.sub not found: ${sub}"; return 1; }
    if grep -q 'ohos' "${sub}"; then
        return 0
    fi
    if [[ ! -f "${sub}.orig" ]]; then
        mv "${sub}" "${sub}.orig" || return 1
    fi
    cat > "${sub}" <<'CONFIG_SUB_SHIM'
#!/bin/sh
# config.sub shim (CicadaPlayerNext OHOS build):
# map *-ohos* / *-linux-ohos* triples to the matching linux-gnu triple,
# delegate everything else to the original config.sub.
arg=${1:-}
case "$arg" in
  *-ohos*)
    cpu=${arg%%-*}
    case "$cpu" in
      aarch64|arm64) echo "aarch64-unknown-linux-gnu" ;;
      arm*)          echo "arm-unknown-linux-gnu" ;;
      x86_64|amd64)  echo "x86_64-unknown-linux-gnu" ;;
      i?86)          echo "i686-unknown-linux-gnu" ;;
      *)             echo "${cpu}-unknown-linux-gnu" ;;
    esac
    ;;
  *)
    exec "$(dirname "$0")/config.sub.orig" "$@"
    ;;
esac
CONFIG_SUB_SHIM
    chmod +x "${sub}" "${sub}.orig" 2>/dev/null || true
    echo "patched config.sub for OHOS: ${sub}"
    return 0
}

function build_libs(){
    if [[ -d ${FFMPEG_SOURCE_DIR} ]];then
        ffmpeg_init_vars
    fi
    local ARCHS="$2"
    local arch
    for arch in ${ARCHS}
    do
       build_static_lib $1 ${arch}
       if [[ "$1" == "Android" ]];then
           link_shared_lib_Android $1 ${arch}
       fi
       if [[ "$1" == "OHOS" ]];then
           link_shared_lib_OHOS $1 ${arch}
       fi
       if [[ "$1" == "win32" ]];then
           link_shared_lib_win32 $1 ${arch}
       fi
    done
    cd ${CWD}

}

# ============================================================================
# 三个平台（Android/OHOS/win32）共用的"链接前完整性检查 + FFmpeg 档案收集"：
#   1) 对象树存在性/非空探针：缺或 0 字节则 purge 整个 libavcodec 对象集并
#      make 自愈重编
#   2) 收集 6 个 FFmpeg 静态库档案到全局 FFMPEG_LINK_LIBS——整包链接 make 产出
#      的 .a（成员清单由 FFmpeg 自身构建系统决定），不再手工 glob 裸 .o，
#      对 FFmpeg 7.0+ 的对象布局变化（改名/新子目录/.objs 清单机制）免疫
#   3) 档案级符号校验：对整个 .a 做一次 nm，验证关键符号存在（不假设符号在
#      哪个具体对象文件里）
# 失败时打印明确修复指令并返回非零；调用方退出。
# ============================================================================
function prepare_ffmpeg_link_input(){
    local platform="$1"
    local nmbin="${CROSS_PREFIX}nm"
    [[ -x "${nmbin}" ]] || nmbin=$(command -v nm 2>/dev/null || true)

    local probe_obj objf heal_needed=0
    while read -r probe_obj; do
        [[ -z "${probe_obj}" ]] && continue
        objf="${FFMPEG_BUILD_DIR}/libavcodec/${probe_obj}"
        if [[ ! -f "${objf}" ]]; then
            echo "WARN: FFmpeg object tree incomplete, missing libavcodec/${probe_obj}"
            heal_needed=1
        elif [[ ! -s "${objf}" ]]; then
            echo "WARN: truncated (0-byte) FFmpeg object libavcodec/${probe_obj}"
            heal_needed=1
        fi
    done <<'PROBE_OBJECTS'
allcodecs.o
aacdec.o
aacdec_fixed.o
hevcdec.o
opusdec.o
aactab.o
PROBE_OBJECTS

    if [[ ${heal_needed} -eq 1 ]]; then
        echo "self-heal: purge all libavcodec objects and rebuild (incomplete tree)"
        rm -f "${FFMPEG_BUILD_DIR}"/libavcodec/*.o "${FFMPEG_BUILD_DIR}"/libavcodec/*/*.o
        if ! ( cd "${FFMPEG_BUILD_DIR}" && make -j8 V=1 ); then
            echo "ERROR: self-heal rebuild failed"
            echo "       fix: rm -rf \"${FFMPEG_BUILD_DIR}\" && re-run ./build_external.sh ${platform}"
            return 1
        fi
        # 自愈后只复验存在性+非空（不猜符号位置）
        for probe_obj in allcodecs.o aacdec.o hevcdec.o; do
            objf="${FFMPEG_BUILD_DIR}/libavcodec/${probe_obj}"
            if [[ ! -s "${objf}" ]]; then
                echo "ERROR: libavcodec/${probe_obj} still missing/empty after self-heal rebuild"
                echo "       fix: rm -rf \"${FFMPEG_BUILD_DIR}\" && re-run ./build_external.sh ${platform}"
                return 1
            fi
        done
        echo "self-heal OK: libavcodec rebuilt"
    fi

    FFMPEG_LINK_LIBS=""
    local fflib
    for fflib in libavcodec libavformat libavutil libswresample libswscale libavfilter; do
        if [[ -f "${FFMPEG_BUILD_DIR}/${fflib}/${fflib}.a" ]]; then
            FFMPEG_LINK_LIBS="${FFMPEG_LINK_LIBS} ${FFMPEG_BUILD_DIR}/${fflib}/${fflib}.a"
        else
            echo "ERROR: missing FFmpeg static library ${FFMPEG_BUILD_DIR}/${fflib}/${fflib}.a"
            echo "       fix: rm -rf \"${FFMPEG_BUILD_DIR}\" && re-run ./build_external.sh ${platform}"
            return 1
        fi
    done

    if [[ -n "${nmbin}" ]]; then
        local symdump sym
        symdump=$("${nmbin}" --print-file-name ${FFMPEG_LINK_LIBS} 2>/dev/null || true)
        for sym in ff_aac_decoder ff_aac_fixed_decoder ff_aac_latm_decoder ff_hevc_decoder \
                   ff_opus_decoder ff_aac_sbr_vlc ff_hevc_parser ff_opus_parser \
                   ff_h264_mp4toannexb_bsf; do
            if ! grep -q " ${sym}$" <<< "${symdump}"; then
                echo "ERROR: symbol ${sym} missing from FFmpeg archives"
                echo "       object tree does not match current FFmpeg version/config"
                echo "       fix: rm -rf \"${FFMPEG_BUILD_DIR}\" && re-run ./build_external.sh ${platform}"
                return 1
            fi
        done
    fi
    return 0
}

function link_shared_lib_Android(){
    if [[ "$1" != "Android" ]];then
        return;
    fi
    local install_dir=${CWD}/install/ffmpeg/Android/$2/
    cross_compile_set_platform_Android  $2
    local cup_arch;
    cup_arch=${CPU_ARCH}
    if [[ "$CPU_ARCH" = arm64 ]]
    then
        cup_arch=aarch64
    fi

    if [[ -z "${LIB_NAME}" ]];then
        export LIB_NAME=alivcffmpeg
    fi

    echo ABI is $2 FFMPEG_BUILD_DIR is $FFMPEG_BUILD_DIR

    # 链接前完整性检查 + 档案收集（三平台共用实现，见 prepare_ffmpeg_link_input）
    prepare_ffmpeg_link_input "Android" || exit 1
    local ff_libs="${FFMPEG_LINK_LIBS}"

    local ldflags=""

    if [[ -d "${CURL_INSTALL_DIR}" ]];then
        ldflags="$ldflags -lcurl -L${CURL_INSTALL_DIR}/lib/"
    fi

    if [[ -d "${ARES_INSTALL_DIR}" ]];then
        ldflags="$ldflags -lcares -L${ARES_INSTALL_DIR}/lib/"
    fi

    if [[ -d "${LIBRTMP_INSTALL_DIR}" ]];then
        ldflags="$ldflags -lrtmp -L${LIBRTMP_INSTALL_DIR}/lib/"
    fi

    if [[ -d "${FDK_AAC_INSTALL_DIR}" ]];then
        ldflags="$ldflags -lfdk-aac -L${FDK_AAC_INSTALL_DIR}/lib/"
    fi

    if [[ -d "${OPENSSL_INSTALL_DIR}" ]];then
        ldflags="$ldflags -lssl -lcrypto -L${OPENSSL_INSTALL_DIR}/lib/"
    fi

    if [[ -d "${DAV1D_INSTALL_DIR}" ]];then
        ldflags="$ldflags -ldav1d -L${DAV1D_INSTALL_DIR}/lib/"
    fi

    if [[ -d "${X264_INSTALL_DIR}" ]];then
        ldflags="$ldflags -lx264 -L${X264_INSTALL_DIR}/lib/"
    fi

    if [[ -d "${LIBXML2_INSTALL_DIR}" ]];then
        ldflags="$ldflags -lxml2 -L${LIBXML2_INSTALL_DIR}/lib/"
    fi

    if [[ -d "${NGHTTP2_INSTALL_DIR}" ]];then
        ldflags="$ldflags -lnghttp2 -L${NGHTTP2_INSTALL_DIR}/lib/"
    fi

    cp ${BUILD_TOOLS_DIR}/src/build_version.cpp ./
    sh ${BUILD_TOOLS_DIR}/gen_build_version.sh > version.h

    # NDK r25+: link with clang instead of the removed gcc toolchain.
    local ndk_host
    case "$(uname -s)" in
        Darwin*) ndk_host=darwin-x86_64 ;;
        *)       ndk_host=linux-x86_64 ;;
    esac
    local toolchain=${ANDROID_NDK}/toolchains/llvm/prebuilt/${ndk_host}
    local TARGET=${CROSS_COMPILE}${ANDROID_API_LEVEL:-24}

    ${toolchain}/bin/clang -target ${TARGET} --sysroot=${SYSTEM_ROOT} -fuse-ld=lld \
      -std=c++11 build_version.cpp -lm -lz -shared -I${FFMPEG_INSTALL_DIR}/include \
      -Wl,--no-undefined -Wl,-z,noexecstack ${CPU_LD_FLAGS}  -landroid -llog \
      -Wl,--allow-multiple-definition -Wl,-Bsymbolic \
      -Wl,-soname,lib${LIB_NAME}.so \
      -Wl,--whole-archive ${ff_libs} -Wl,--no-whole-archive \
      -o ${install_dir}/lib${LIB_NAME}.so \
      -Wl,--whole-archive   ${ldflags} -Wl,--no-whole-archive -Wl,--build-id=sha1

    rm build_version.cpp version.h
}
function link_shared_lib_win32(){
    if [[ "$1" != "win32" ]];then
        return;
    fi
    local install_dir=${CWD}/install/ffmpeg/win32/$2
    cross_compile_set_platform_win32  $2
    cup_arch=x86
    if [[ -z "${LIB_NAME}" ]];then
        export LIB_NAME=alivcffmpeg
    fi

    echo ABI is $2 FFMPEG_BUILD_DIR is $FFMPEG_BUILD_DIR

    # 链接前完整性检查 + 档案收集（三平台共用实现，见 prepare_ffmpeg_link_input）
    prepare_ffmpeg_link_input "win32" || exit 1
    local ff_libs="${FFMPEG_LINK_LIBS}"

    local ldflags=""

    if [[ -d "${CURL_INSTALL_DIR}" ]];then
        ldflags="$ldflags -lcurl -L${CURL_INSTALL_DIR}/lib/"
    fi
#
#    if [[ -d "${ARES_INSTALL_DIR}" ]];then
#        ldflags="$ldflags -lcares -L${ARES_INSTALL_DIR}/lib/"
#    fi
#
#    if [[ -d "${LIBRTMP_INSTALL_DIR}" ]];then
#        ldflags="$ldflags -lrtmp -L${LIBRTMP_INSTALL_DIR}/lib/"
#    fi
#
#    if [[ -d "${FDK_AAC_INSTALL_DIR}" ]];then
#        ldflags="$ldflags -lfdk-aac -L${FDK_AAC_INSTALL_DIR}/lib/"
#    fi
    if [[ -d "${OPENSSL_INSTALL_DIR}" ]];then
        ldflags="$ldflags -lssl -lcrypto -L${OPENSSL_INSTALL_DIR}/lib/"
    fi
#    if [[ -d "${DAV1D_INSTALL_DIR}" ]];then
#        ldflags="$ldflags -ldav1d -L${DAV1D_INSTALL_DIR}/lib/"
#    fi
#
#    if [[ -d "${X264_INSTALL_DIR}" ]];then
#        ldflags="$ldflags -lx264 -L${X264_INSTALL_DIR}/lib/"
#    fi
#
#    if [[ -d "${LIBXML2_INSTALL_DIR}" ]];then
#        ldflags="$ldflags -lxml2 -L${LIBXML2_INSTALL_DIR}/lib/"
#    fi
    if [[ -d "${NGHTTP2_INSTALL_DIR}" ]];then
        ldflags="$ldflags -lnghttp2 -L${NGHTTP2_INSTALL_DIR}/lib/"
    fi

    echo ldflags is ${ldflags}
    cp ${BUILD_TOOLS_DIR}/src/build_version.cpp ./
    sh ${BUILD_TOOLS_DIR}/gen_build_version.sh > version.h

    ${CROSS_COMPILE}-gcc -std=c++11 ${CPU_FLAGS} build_version.cpp -static-libgcc  -static -lm  -shared  -I${FFMPEG_INSTALL_DIR}/include \
     -Wl,--no-undefined  ${CPU_LD_FLAGS}  -Wl,--allow-multiple-definition -Wl,-Bsymbolic -Wl,-soname,lib${LIB_NAME}.so \
    -Wl,--whole-archive ${ff_libs} -Wl,--no-whole-archive \
    -o ${install_dir}/lib${LIB_NAME}.dll \
    -Wl,--kill-at,--out-implib=${install_dir}/lib${LIB_NAME}.lib   \
    -Wl,--whole-archive   ${ldflags} -Wl,--no-whole-archive -Wl,--build-id=sha1 -lws2_32 -lbcrypt -lcrypt32

    rm build_version.cpp version.h
}

function link_shared_lib_OHOS(){
    if [[ "$1" != "OHOS" ]];then
        return;
    fi
    local install_dir=${CWD}/install/ffmpeg/OHOS/$2/
    cross_compile_set_platform_OHOS  $2 || return 1
    local cup_arch;
    cup_arch=${CPU_ARCH}

    if [[ -z "${LIB_NAME}" ]];then
        export LIB_NAME=alivcffmpeg
    fi

    echo ABI is $2 FFMPEG_BUILD_DIR is $FFMPEG_BUILD_DIR

    # 链接前完整性检查 + 档案收集（三平台共用实现，见 prepare_ffmpeg_link_input）
    prepare_ffmpeg_link_input "OHOS" || exit 1
    local ff_libs="${FFMPEG_LINK_LIBS}"

    local ldflags=""

    if [[ -d "${OPENSSL_INSTALL_DIR}" ]];then
        ldflags="$ldflags -lssl -lcrypto -L${OPENSSL_INSTALL_DIR}/lib/"
    fi
    if [[ -d "${DAV1D_INSTALL_DIR}" ]];then
        ldflags="$ldflags -ldav1d -L${DAV1D_INSTALL_DIR}/lib/"
    fi
    if [[ -d "${NGHTTP2_INSTALL_DIR}" ]];then
        ldflags="$ldflags -lnghttp2 -L${NGHTTP2_INSTALL_DIR}/lib/"
    fi

    cp ${BUILD_TOOLS_DIR}/src/build_version.cpp ./
    sh ${BUILD_TOOLS_DIR}/gen_build_version.sh > version.h

    # Link with the OHOS NDK clang; the target triple is already in $CC.
    local clang="${OHOS_SDK}/native/llvm/bin/clang"
    if [[ -z "${OHOS_SDK}" ]];then
        clang="${OHOS_SDK_HOME}/native/llvm/bin/clang"
    fi

    ${clang} --target=${TARGET_TRIPLE} --sysroot=${SYSTEM_ROOT} -fuse-ld=lld \
      -std=c++11 build_version.cpp -lm -lz -shared -I${FFMPEG_INSTALL_DIR}/include \
      -Wl,--no-undefined -Wl,-z,noexecstack ${CPU_LD_FLAGS} \
      -Wl,--allow-multiple-definition -Wl,-Bsymbolic \
      -Wl,-soname,lib${LIB_NAME}.so \
      -Wl,--whole-archive ${ff_libs} -Wl,--no-whole-archive \
      -o ${install_dir}/lib${LIB_NAME}.so \
      -Wl,--whole-archive   ${ldflags} -Wl,--no-whole-archive -Wl,--build-id=sha1

    rm build_version.cpp version.h
}

#function link_shared_lib_win321(){
#    if [[ "$1" != "win32" ]];then
#        return;
#    fi
#    local install_dir=${CWD}/install/ffmpeg/win32/$2/
#    cross_compile_set_platform_win32  $2
#    if [[ -z "${LIB_NAME}" ]];then
#        export LIB_NAME=alivcffmpeg
#    fi
#
#    local curr_dir=${CWD}
#    cd ${install_dir}
#    echo install_dir is ${install_dir}
#    echo BUILD_TOOLS_DIR is ${BUILD_TOOLS_DIR}
#    echo OPENSSL_INSTALL_DIR is ${OPENSSL_INSTALL_DIR}
#    echo curr_dir is ${curr_dir}
#
#    cp lib/*.a ./
#    local ldflags="-lavformat -lavcodec -lavutil -lavfilter -lswscale -lswresample"
#    if [[ -d "${OPENSSL_INSTALL_DIR}" ]];then
#        ldflags="$ldflags -lssl -lcrypto"
#        cp ${OPENSSL_INSTALL_DIR}/lib/*.a ./
#    fi
##    ldflags="$ldflags -lbcrypt  -lws2_32 -llz32 -lsecur32"
#
#    cp ${BUILD_TOOLS_DIR}/src/build_version.cpp ./
#    sh ${BUILD_TOOLS_DIR}/gen_build_version.sh > version.h
#
#    local platf=""
#    if [[ "$2" == "i686" ]];then
#        platf="-m32"
#    fi
#    if [[ "$2" == "x86_64" ]];then
#        platf="-m64"
#    fi
#    ${CROSS_COMPILE}-gcc ${platf} build_version.cpp -Wall -static-libgcc -static-libstdc++ -static -shared -o ${LIB_NAME}.dll \
#     -O2  -I./include  -L./  -Wl,--kill-at,--out-implib=${LIB_NAME}.lib \
#     -Wl,--whole-archive ${ldflags} -Wl,--no-whole-archive -lbcrypt  -lws2_32 #-llz32 -lsecur32
#    rm build_version.cpp version.h
#    cd ${curr_dir}
#}
