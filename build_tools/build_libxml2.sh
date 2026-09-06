#!/usr/bin/env bash

function build_libxml2(){
    if [[ ! -f ${LIBXML2_SOURCE_DIR}/configure ]]
    then
        cd ${LIBXML2_SOURCE_DIR}
        # Windows 检出可能丢失可执行位：先补上再用 sh 兜底执行
        chmod +x autogen.sh configure 2>/dev/null
        sh ./autogen.sh
        make distclean 2>/dev/null
        if [[ -n "$MTL" ]]
        then
            cp ${BUILD_TOOLS_DIR}/automake_config/config.guess ./
            cp ${BUILD_TOOLS_DIR}/automake_config/config.sub ./
        fi
        cd -
    fi

    local shared="false"
    local native_build="no"

    if [[ "$1" == "Android" ]]
    then
        cross_compile_set_platform_Android  $2
    elif [[ "$1" == "iOS" ]]
    then
        cross_compile_set_platform_iOS $2
    elif [[ "$1" == "win32" ]];then
        cross_compile_set_platform_win32 $2
        # only support shared lib for now
        shared="true"
    elif [[ "$1" == "Darwin" ]];then
        print_warning "native build libxml2 for $1"
        native_build="yes"
    elif [[ "$1" == "Linux" ]];then
        print_warning "native build libxml2 for $1"
        native_build="yes"
    elif [[ "$1" == "OHOS" ]];then
        cross_compile_set_platform_OHOS $2
        # libxml2 2.9.9 自带的 config.sub 不认识 ohos/aarch64 三元组，先装 shim
        patch_config_sub_for_ohos "${LIBXML2_SOURCE_DIR}" || exit 1
        # libxml2 的 --host 用 TARGET_TRIPLE（OHOSConfig 未定义 CROSS_COMPILE）
        CROSS_COMPILE=${TARGET_TRIPLE}
    else
        echo "Unsupported platform"
        exit 1;
    fi

    local shared_opt;

    if [[ "${shared}" == "false" ]];then
        shared_opt="--enable-shared=no"
    else
        shared_opt="--enable-static=no"
    fi

    # libxml2 2.14 已把 Python 绑定拆到独立仓库，configure 不再有
    # --without-python 选项；2.9.x 则需要它来禁用 python 绑定构建。
    local python_opt=""
    if [[ -d "${LIBXML2_SOURCE_DIR}/python" ]];then
        python_opt="--without-python"
    fi

    local build_dir="build/libxml2/$1/$2"
    local install_dir="$PWD/install/libxml2/$1/$2"
    mkdir -p ${build_dir}/
     if [[ "${BUILD}" != "False" ]];then
        cd ${build_dir}
        if [[ "$native_build" == "yes" ]];then
        sh ${LIBXML2_SOURCE_DIR}/configure \
            ${shared_opt}            \
            --with-pic=yes \
            --without-lzma \
            ${python_opt} \
            --without-debug \
            --without-zlib  \
            --without-iconv \
            --prefix=${install_dir} \
            CFLAGS="${CPU_FLAGS}"   \
            CPPFLAGS="${CPU_FLAGS}" \
            LDFLAGS="${CPU_LDFLAGS}"
        else
        sh ${LIBXML2_SOURCE_DIR}/configure \
            --host="${CROSS_COMPILE}"     \
            ${shared_opt}            \
            --with-pic=yes  \
            --without-lzma \
            ${python_opt} \
            --without-debug \
            --without-zlib  \
            --without-iconv \
            --prefix=${install_dir} \
            CC="${CC}"              \
            CXX="$CC"               \
            CPP="$CC -E"            \
            AS="${AS}"              \
            CFLAGS="${CPU_FLAGS}"   \
            CPPFLAGS="${CPU_FLAGS}" \
            LDFLAGS="${CPU_LDFLAGS}"
        fi
        make -j8 V=1
        make install
        cd -
    fi

    export LIBXML2_INSTALL_DIR=${install_dir}
}