#!/usr/bin/env bash
# ============================================================================
# OpenSSL 构建脚本（支持 1.1.1 与 3.x，按源码版本自动选择配置）
#
#   OpenSSL 3.0 LTS（默认，external/player_git_source_list.sh 固定 openssl-3.0.15）：
#     - 原生支持现代 NDK 布局（toolchains/llvm/prebuilt/<host>/sysroot），
#       无需 platforms/ shim 与 -gcc-toolchain 清理
#     - 3.0 移除了一批旧选项（no-engine/no-async/no-gost 等），配置清单已按
#       3.x 重写
#   OpenSSL 1.1.1（通过 OPENSSL_BRANCH=OpenSSL_1_1_1g 回退时）：
#     - 自动创建 platforms/android-<api>/arch-* 符号链接指向统一 sysroot
#     - 自动从 15-android.conf / Makefile 删除旧 gcc-4.9 工具链参数
# ============================================================================

source cross_compile_env.sh
source native_compile_env.sh
source utils.sh

# 探测源码版本（1.1.1 -> 1，3.x -> 3）
function openssl_detect_major() {
    local major=1
    if [[ -f "${OPEN_SSL_SOURCE_DIR}/include/openssl/opensslv.h" ]]; then
        major=$(sed -n 's/^# *define *OPENSSL_VERSION_MAJOR *\([0-9][0-9]*\).*/\1/p' \
                    "${OPEN_SSL_SOURCE_DIR}/include/openssl/opensslv.h" | head -1)
    fi
    echo "${major:-1}"
}

function build_openssl_111(){

    local openssl_major
    openssl_major=$(openssl_detect_major)
    echo "OpenSSL major version: ${openssl_major}"

    local config_platform;
    local config_opt="no-tests";

    if [[ "${openssl_major}" -ge 3 ]]; then
        # ---- OpenSSL 3.x 配置（3.0 已移除 engine/async/gost/ec2m 等旧选项）----
        config_opt="${config_opt} no-cms no-filenames no-ocsp no-psk no-srp no-ts"
        config_opt="${config_opt} no-aria no-bf no-blake2 no-camellia no-cast no-chacha
             no-cmac no-dsa no-ecdh no-ecdsa no-idea no-md4 no-mdc2 no-ocb
             no-poly1305 no-rc2 no-rc4 no-rmd160 no-scrypt no-seed no-siphash no-sm2 no-sm3
             no-sm4 no-whirlpool"
        # 不需要 RC4/MD4 等 legacy 算法，关闭 legacy provider：
        # 1) 减小库体积；2) 避免 providers/legacy.so 的动态链接（裸 clang 缺
        #    crtbegin_so.o/crtend_so.o 的坑只出现在模块链接这一步）
        config_opt="${config_opt} no-legacy"
    else
        # ---- OpenSSL 1.1.1 配置（历史保留）----
        config_opt="${config_opt} no-afalgeng no-async no-autoalginit no-autoerrinit no-capieng
         no-cms no-dynamic-engine no-engine  no-ec2m no-filenames no-gost
         no-hw-padlock no-nextprotoneg no-ocsp no-psk no-rfc3779 no-srp no-ts"
        config_opt="${config_opt} no-aria no-bf no-blake2 no-camellia no-cast no-chacha
                 no-cmac no-dsa no-ecdh no-ecdsa no-idea no-md4 no-mdc2 no-ocb
                 no-poly1305 no-rc2 no-rc4 no-rmd160 no-scrypt no-seed no-siphash no-sm2 no-sm3
                 no-sm4 no-whirlpool"
    fi

    if [[ "$1" == "Android" ]]
    then
        cross_compile_set_platform_Android  $2
        config_platform="android-${CPU_ARCH}"
        # NDK r25+ 由 AndroidConfig.sh 提供 ANDROID_API_LEVEL（默认 24）
        # OpenSSL 3.x 同样支持 -D__ANDROID_API__=NN（见其 NOTES-ANDROID.md）
        local cross_compile_opt="-D__ANDROID_API__=${ANDROID_API_LEVEL:-24}"
        config_opt="${config_opt} no-shared no-asm"

        if [[ "${openssl_major}" -lt 3 ]]; then
            # 仅 1.1.1 需要：校验/推导旧 NDK 目录结构。
            # 用符号链接把旧 platforms/ 路径指到统一 sysroot（目录结构一致），
            # 并删除 15-android.conf 中旧 gcc-4.9 工具链参数。
            local ndk_host
            case "$(uname -s)" in
                Darwin*) ndk_host=darwin-x86_64 ;;
                *)       ndk_host=linux-x86_64 ;;
            esac
            local api=${ANDROID_API_LEVEL:-24}
            local sysroot="${ANDROID_NDK}/toolchains/llvm/prebuilt/${ndk_host}/sysroot"
            local plat_dir="${ANDROID_NDK}/platforms/android-${api}"
            if [[ -d "${sysroot}" ]]; then
                mkdir -p "${plat_dir}"
                for a in arch-arm arch-arm64 arch-x86 arch-x86_64; do
                    [[ -e "${plat_dir}/$a" ]] || ln -s "${sysroot}" "${plat_dir}/$a"
                done
            fi
            sed -i 's#-gcc-toolchain \$ndk/toolchains/\$triarch-4.9/prebuilt/\$host##g' \
                "${OPEN_SSL_SOURCE_DIR}/Configurations/15-android.conf"
        fi
    elif [[ "$1" == "iOS" ]]
    then
        cross_compile_set_platform_iOS $2
        config_opt="${config_opt} no-dso"
        CROSS_COMPILE=
        local platform
        local os
        if [[ "$2" == "x86_64" ]] || [[ "$2" == "i386" ]]; then
            platform=${IPHONESIMULATOR_PLATFORM}
            os=iPhoneSimulator
            config_platform="iossimulator-xcrun"
            CFLAGS=
            config_opt=
        else
            platform=${IPHONEOS_PLATFORM}
            os=iPhoneOS
            CFLAGS="-fembed-bitcode"
            if [[ "$2" == "armv7"  ]];then
                config_platform="ios-xcrun"
            else
                config_platform="ios64-xcrun"
            fi
        fi

        export CFLAGS="${CFLAGS} -arch ${2}"
        export LDFLAGS=
    elif [ "$1" == "win32" ];then
        cross_compile_set_platform_win32 $2
        if [ "$2" == "x86_64" ];then
            config_platform="mingw64"
        else
            config_platform="mingw"
        fi
        export   CROSS_COMPILE=${CROSS_COMPILE}-
    elif [ "$1" == "Darwin" ];then
        print_warning "native build openssl for $1"
        if [ "$2" == "x86_64" ];then
            config_platform="darwin64-x86_64-cc"
        elif [ "$2" == "arm64" ];then
            config_platform="darwin64-arm64-cc"
        fi
        config_opt="${config_opt} no-shared"
        native_compile_set_platform_macOS $2
        export CFLAGS="${CFLAGS} $CPU_FLAGS"
    elif [ "$1" == "Linux" ];then
        config_platform="linux-x86_64";
    elif [ "$1" == "maccatalyst" ];then
        cross_compile_set_platform_maccatalyst "$2"
        if [ "$2" == "x86_64" ];then
            config_platform="darwin64-x86_64-cc"
        elif [ "$2" == "arm64" ];then
            config_platform="darwin64-arm64-cc"
        fi
        export CFLAGS="$CPU_FLAGS"
        export LDFLAGS=
        config_opt="${config_opt} no-shared"
    else
        echo "Unsupported platform $1"
        exit 1;
    fi

    local build_dir="build/openssl/$1/$2"
    local install_dir="$PWD/install/openssl/$1/$2"
    mkdir -p ${install_dir}/lib
    mkdir -p ${build_dir}
    cd ${build_dir}
    if [ "${BUILD}" != "False" ];then

        ${OPEN_SSL_SOURCE_DIR}/Configure ${config_platform} ${config_opt} ${cross_compile_opt} ${HARDENED_CFLAG} --prefix=${install_dir}  --openssldir=${install_dir}

        if [[ "$1" == "Android" ]]; then
            # 用带 API 级别的 NDK clang 包装器替换裸 clang，并去掉不带 API 的
            # -target 参数：包装器会根据自身名字注入正确的 sysroot 与
            # crtbegin_so.o/crtend_so.o 等运行时目标文件路径，保证 .so 链接成功。
            local wrapper="${CROSS_COMPILE}${ANDROID_API_LEVEL:-24}-clang"
            if command -v "${wrapper}" >/dev/null 2>&1; then
                sed -i "s#^CC=clang\$#CC=${wrapper}#; s#^CC= clang\$#CC=${wrapper}#" Makefile
                sed -i 's# -target armv7a-linux-androideabi##g; s# -target aarch64-linux-android##g; s# -target arm-linux-androideabi##g; s# -target x86_64-linux-android##g; s# -target i686-linux-android##g' Makefile
                echo "openssl Android: use NDK wrapper compiler ${wrapper}"
            fi

            if [[ "${openssl_major}" -lt 3 ]]; then
                # 1.1.1 兜底：Makefile 若仍残留旧 gcc-toolchain 参数则删除
                sed -i 's# -gcc-toolchain [^ ]*##g' Makefile
            fi
        fi

        make -j8 V=1 || exit 1
        make  install_sw ||exit 1

        cd -
    fi
    OPENSSL_INSTALL_DIR=${install_dir}

}
