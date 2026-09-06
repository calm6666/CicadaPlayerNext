#!/usr/bin/env bash
# ============================================================================
# 旧版 build_openssl.sh（1.1.1 时代实现，配置为 android/iphoneos-cross
# 等已废弃目标）不再使用。
# 统一走 build_openssl_3.sh：按源码版本自动适配 OpenSSL 3.x / 1.1.1。
# 本文件保留为兼容转发：定义旧函数名别名，指向新实现。
# ============================================================================
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/build_openssl_3.sh"

function build_openssl(){ build_openssl_3 "$@"; }
