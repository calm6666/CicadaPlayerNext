#!/bin/sh
# ============================================================================
# NDK 交叉编译用的最小 pkg-config shim（FFmpeg configure 的 --pkg-config= 指向这里）
#
# 背景：NDK r25c 没有 llvm-pkg-config。FFmpeg configure 探测 libxml2 / openssl
# 走 pkg-config 路径，探测失败就会把 dash_demuxer（依赖 libxml2）和
# https_protocol（依赖 openssl/tls）禁用。这两个库由本项目自己编译安装
# （build_libxml2.sh / build_openssl_3.sh），没有 .pc 文件，所以本 shim
# 直接按环境变量应答：
#   LIBXML2_INSTALL_DIR / OPENSSL_INSTALL_DIR
# 未知包按"未安装"处理（--exists 返回 1），行为与真实 pkg-config 一致。
# ============================================================================

mode="$1"
pkg=""
for arg in "$@"; do
    case "$arg" in
        --exists|--print-errors|--short-errors|--cflags|--libs|--modversion|--version) ;;
        -*) ;;
        *) pkg="${arg%% *}"; break ;;
    esac
done

# configure 启动时用它探测 pkg-config 是否可用
if [ "$mode" = "--version" ]; then
    echo "0.29.2"
    exit 0
fi

case "$pkg" in
    libxml-2.0*|libxml2*)
        case "$mode" in
            --exists|--print-errors|--short-errors) exit 0 ;;
            --cflags)     echo "-I${LIBXML2_INSTALL_DIR}/include/libxml2"; exit 0 ;;
            --libs)       echo "-L${LIBXML2_INSTALL_DIR}/lib -lxml2 -lm"; exit 0 ;;
            --modversion) echo "2.14.5"; exit 0 ;;
        esac
        ;;
    openssl*|libssl*|libcrypto*)
        case "$mode" in
            --exists|--print-errors|--short-errors) exit 0 ;;
            --cflags)     echo "-I${OPENSSL_INSTALL_DIR}/include"; exit 0 ;;
            --libs)       echo "-L${OPENSSL_INSTALL_DIR}/lib -lssl -lcrypto"; exit 0 ;;
            --modversion) echo "3.0.17"; exit 0 ;;
        esac
        ;;
esac

# 未识别的包：按"未安装"处理
exit 1
