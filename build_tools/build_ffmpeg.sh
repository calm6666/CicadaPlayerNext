#!/usr/bin/env bash

#set -x

source ffmpeg_cross_compile_config.sh
source ffmpeg_commands.sh
function build_ffmpeg(){
    if [[ "${FFMPEG_USE_OPENSSL}" != "TRUE" ]];then
        use_openssl="FALSE"
    else
        use_openssl="TRUE"
    fi

    ffmpeg_cross_compile_config_reset
    if [[ "$1" == "Android" ]]
    then
        ffmpeg_cross_compile_set_Android  $2
    elif [[ "$1" == "iOS" ]]
    then
        ffmpeg_cross_compile_set_iOS $2
        if [[ "${SSL_USE_NATIVE}" != "TRUE" ]];then
            ffmpeg_config_add_user "--disable-securetransport"
        else
              use_openssl="FALSE"
        fi
    elif [[ "$1" == "win32" ]];then
        ffmpeg_cross_compile_set_win32 $2
    elif [[ "$1" == "Darwin" ]];then
        print_warning "native build ffmpeg for $1  $2"
 #       local native_build=yes
        if [[ "${SSL_USE_NATIVE}" != "TRUE" ]];then
            ffmpeg_config_add_user "--disable-securetransport"
        else
              use_openssl="FALSE"
        fi
        ffmpeg_config_add_extra_cflags "-fno-stack-check"

        ffmpeg_native_compile_set_macOS $2
    elif [[ "$1" == "Linux" ]];then
        local native_build=yes
    elif [[ "$1" == "OHOS" ]];then
        # HarmonyOS / OpenHarmony cross-compile via the OHOS NDK clang toolchain
        ffmpeg_cross_compile_set_OHOS $2
    elif [ "$1" == "maccatalyst" ];then
      if [[ "${SSL_USE_NATIVE}" != "TRUE" ]];then
          ffmpeg_config_add_user "--disable-securetransport"
      else
          use_openssl="FALSE"
      fi
           ffmpeg_native_compile_set_maccatalyst "$2"
    else
        echo "ffmpeg Unsupported platform $2"
        return -1;
    fi

    local ffmpeg_build_dir="build/ffmpeg/$1/$2"
    ffmpeg_config_add_extra_cflags "${CPU_FLAGS}"
    ffmpeg_config_add_extra_ldflags "${CPU_LDFLAGS}"
    ffmpeg_config_add_extra_cflags "${HARDENED_CFLAG}"

    # dash_demuxer 依赖 libxml2。libxml2 由本项目 build_libxml2.sh 编译安装，
    # 显式链入 FFmpeg，消除
    # "WARNING: Disabled dash_demuxer because not all dependencies are satisfied: libxml2"。
    #
    # FFmpeg 9.0 的 libxml2 探测只有 pkg-config 一条路（require_pkg_config），
    # 而 NDK r25c 没有 llvm-pkg-config，会直接 die：
    #   "ERROR: libxml-2.0 not found using pkg-config"
    # 所以按项目 0009 补丁（check_lib 方案）的思路，把探测改成 check_lib，
    # 头文件/库路径已由上面的 -I/-L 提供；sed 幂等，可重复构建。
    if [[ -n "${LIBXML2_INSTALL_DIR}" ]] && [[ "$1" == "Android" || "$1" == "OHOS" ]]; then
        ffmpeg_config_add_user "--enable-libxml2"
        ffmpeg_config_add_extra_cflags "-I${LIBXML2_INSTALL_DIR}/include/libxml2"
        ffmpeg_config_add_extra_ldflags "-L${LIBXML2_INSTALL_DIR}/lib"
        sed -i 's#^enabled libxml2 .*require_pkg_config libxml2.*$#enabled libxml2           \&\& check_lib xml2 libxml/xmlversion.h xmlCheckVersion -lxml2#' \
            "${FFMPEG_SOURCE_DIR}/configure"
        if ! grep -q '^enabled libxml2 .*check_lib xml2' "${FFMPEG_SOURCE_DIR}/configure"; then
            echo "ERROR: failed to patch libxml2 detection in ${FFMPEG_SOURCE_DIR}/configure"
            exit 1
        fi
        echo "ffmpeg $1: libxml2 detection -> check_lib (NDK has no pkg-config)"
    fi
    if [[ -n "${FDK_AAC_INSTALL_DIR}" ]]; then
        ffmpeg_config_add_user "--enable-libfdk-aac"
        # libfdk-aac is nonfree-gated since FFmpeg 5.1; enable it explicitly.
        ffmpeg_config_add_user "--enable-nonfree"
        ffmpeg_config_add_extra_cflags "-I${FDK_AAC_INSTALL_DIR}/include"
        ffmpeg_config_add_extra_ldflags "-L${FDK_AAC_INSTALL_DIR}/lib"
        if [[ "$1" == "Android" ]];then
            ffmpeg_config_add_extra_ldflags "-lm"
        fi
    fi

    if [[ -n "${X264_INSTALL_DIR}" ]]; then
        ffmpeg_config_add_user "--enable-libx264"
        ffmpeg_config_add_user "--enable-nonfree --enable-gpl --enable-version3"
        ffmpeg_config_add_extra_cflags "-I${X264_INSTALL_DIR}/include"
        ffmpeg_config_add_extra_ldflags "-L${X264_INSTALL_DIR}/lib"
    fi

    if [[ -n "${OPENSSL_INSTALL_DIR}" ]] && [[ "$use_openssl" == "TRUE" ]];then
        ffmpeg_config_add_user "--enable-openssl"
        ffmpeg_config_add_extra_cflags "-I${OPENSSL_INSTALL_DIR}/include"
        ffmpeg_config_add_extra_ldflags "-L${OPENSSL_INSTALL_DIR}/lib"
    fi

    if [[ -d "${DAV1D_INSTALL_DIR}" ]];then
        ffmpeg_config_add_user "--enable-libdav1d --enable-decoder=libdav1d"
        ffmpeg_config_add_extra_cflags "-I${DAV1D_INSTALL_DIR}/include"
        ffmpeg_config_add_extra_ldflags "-L${DAV1D_INSTALL_DIR}/lib -ldav1d"
    fi
    if [[ "$native_build" == "yes" ]];then
        echo native_build
    else
        ffmpeg_config_set_cross_config "${FFMPEG_CROSS_COMPILE_CONFIG}"
        ffmpeg_config_set_cc "$CC"
        ffmpeg_config_set_as "$AS"
    fi

    ffmpeg_config_set_install "$PWD/install/ffmpeg/$1/$2"
    mkdir -p ${ffmpeg_build_dir}
    cd ${ffmpeg_build_dir}
    ffmpeg_config
    if [[ $? -eq 0 ]]
    then
        echo build succese
    else
        echo build error
        # configure 失败时自动打印 config.log 末尾（最后一次探测的编译/链接
        # 命令与真实报错），例如 openssl 探测失败会显示缺哪个符号/头文件
        echo "----- ffbuild/config.log tail (diagnostic) -----"
        tail -60 ffbuild/config.log 2>/dev/null || true
        exit 1;
    fi

    # 强制 PIC：合并 libalivcffmpeg.so 要求所有对象位置无关。
    # --enable-pic 只作用于 C 代码；汇编(.S)需要在 ASFLAGS 里带 -fPIC
    # 才会生成 GOT 寻址（否则链接报 R_AARCH64_ADR_PREL_PG_HI21 /
    # R_ARM_REL32 "recompile with -fPIC"）。
    if [[ "$1" == "Android" || "$1" == "OHOS" ]]; then
        if [[ -f ffbuild/config.mak ]]; then
            sed -i 's#^CFLAGS=#CFLAGS=-fPIC #; s#^ASFLAGS=#ASFLAGS=-fPIC #' ffbuild/config.mak
            echo "ffmpeg $1: force -fPIC in CFLAGS/ASFLAGS"
        fi
    fi

    # 组件清单 vs 实际配置核对：configure 静默禁用（如 FFmpeg 9 依赖变化）
    # 会在这里直接报出原因，而不是等 15 分钟编译后在链接期爆 undefined symbol
    ffmpeg_verify_requested_components || exit 1

    ffmpeg_build
    if [[ $? -eq 0 ]]
    then
        echo build succese
        return 0;
    else
        echo build error
        exit 1;
    fi
}



