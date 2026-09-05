#!/usr/bin/env bash

source cross_compile_env.sh
source native_compile_env.sh
source utils.sh
function build_openssl_111(){

    local config_platform;
    local config_opt="no-tests";
    config_opt="${config_opt} no-afalgeng no-async no-autoalginit no-autoerrinit no-capieng
     no-cms no-dynamic-engine no-engine  no-ec2m no-filenames no-gost
     no-hw-padlock no-nextprotoneg no-ocsp no-psk no-rfc3779 no-srp no-ts" #ec is must for keyless server
     # no-dgram Don’t build support for datagram based BIOs. Selecting this option will also force the disabling of DTLS.
     # no-srtp
     #  no-<prot>        Don't build support for negotiating the specified SSL/TLS
     #                   protocol (one of ssl, ssl3, tls, tls1, tls1_1, tls1_2,
     #                   tls1_3, dtls, dtls1 or dtls1_2). If "no-tls" is selected then
     #                   all of tls1, tls1_1, tls1_2 and tls1_3 are disabled.
     #                   Similarly "no-dtls" will disable dtls1 and dtls1_2. The
     #                   "no-ssl" option is synonymous with "no-ssl3". Note this only
     #                   affects version negotiation. OpenSSL will still provide the
     #                   methods for applications to explicitly select the individual
     #                   protocol versions.
     ### useless for cut the lib size ,enable all
     config_opt="${config_opt}"

     #  no-<alg> aria, bf, blake2, camellia, cast, chacha,
     #                   cmac, des, dh, dsa, ecdh, ecdsa, idea, md4, mdc2, ocb,
     #                   poly1305, rc2, rc4, rmd160, scrypt, seed, siphash, sm2, sm3,
     #                   sm4 or whirlpool
     #des dh sock for curl use
     config_opt="${config_opt} no-aria no-bf no-blake2 no-camellia no-cast no-chacha
                 no-cmac no-dsa no-ecdh no-ecdsa no-idea no-md4 no-mdc2 no-ocb
                 no-poly1305 no-rc2 no-rc4 no-rmd160 no-scrypt no-seed no-siphash no-sm2 no-sm3
                 no-sm4 no-whirlpool"

    if [[ "$1" == "Android" ]]
    then
        cross_compile_set_platform_Android  $2
        config_platform="android-${CPU_ARCH}"
        # NDK r25+ 由 AndroidConfig.sh 提供 ANDROID_API_LEVEL（默认 24）
        local cross_compile_opt="-D__ANDROID_API__=${ANDROID_API_LEVEL:-24}"
        config_opt="${config_opt} no-shared no-asm"

        # OpenSSL 1.1.1 的 15-android.conf 校验旧的 NDK 目录结构
        # ($ndk/platforms/android-<api>/arch-<arch>)，而 NDK r23+ 已移除
        # platforms/，改用统一 sysroot。用符号链接把旧路径指到统一 sysroot
        # （两者目录结构一致：usr/include、usr/lib），让 1.1.1 的检查与
        # --sysroot 推导在新 NDK (r25/r26/r27) 上同样成立。
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

#        export CROSS_TOP="${platform}/Developer"
#        export CROSS_SDK="${os}${IPHONEOS_SDK_VERSION}.sdk"
#        export BUILD_TOOLS="${DEVELOPER}"
#        export CC="${BUILD_TOOLS}/usr/bin/gcc"
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
        print_warning "native build openssl111 for $1"
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

        # OpenSSL 1.1.1 会给 clang 传入旧版 gcc-4.9 工具链路径
        # （-gcc-toolchain $ndk/toolchains/<triarch>-4.9/prebuilt/<host>），
        # 现代 NDK 已无该目录，且 NDK clang 自带 sysroot 不需要它。
        # Configure 生成 Makefile 后统一去掉该参数。
        if [[ "$1" == "Android" ]]; then
            sed -i 's# -gcc-toolchain [^ ]*##g' Makefile
        fi

        make -j8 V=1 || exit 1
        make  install_sw ||exit 1

        cd -
    fi
    OPENSSL_INSTALL_DIR=${install_dir}

}
