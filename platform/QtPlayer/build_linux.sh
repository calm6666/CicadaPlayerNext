#!/usr/bin/env bash
#
# QtPlayer —— Linux 构建 / 部署脚本
#
# 用法：
#     cd <本目录>
#     ./build_linux.sh                     # 配置 + 编译 Release（产物在 build/linux/）
#     ./build_linux.sh --deploy            # 额外 cmake --install 到 ./deploy
#     ./build_linux.sh --deploy /tmp/out   # 部署到指定目录
#     ./build_linux.sh --debug             # Debug 构建
#     ./build_linux.sh --clean             # 删掉构建目录重新配置
#
# Qt 怎么找（按顺序）：
#   1. 环境变量 QTDIR（例：export QTDIR=~/Qt/6.11.1/gcc_64）—— Qt 官网安装包用这个；
#   2. PATH 里的 qmake6 / qmake —— 发行版包（apt/dnf/pacman 装的 qt6-*）走这个；
#   都不行就报错提示你装/设哪一个，**不写死任何机器上的路径**。
#
# 依赖（Ubuntu/Debian 为例，发行版包名差不多）：
#     sudo apt install build-essential cmake ninja-build pkg-config \
#                      qt6-declarative-dev qt6-base-dev \
#                      libsdl2-dev libavcodec-dev libavformat-dev libavutil-dev \
#                      libswscale-dev libswresample-dev libavfilter-dev \
#                      libcurl4-openssl-dev libnghttp2-dev libxml2-dev
#     # 想要 Linux 零拷贝（VAAPI + dmabuf/EGLImage）再加：
#     sudo apt install libva-dev libegl1-mesa-dev libdrm-dev
#
# 无边框窗口用的是 3rdparty/qwindowkit（见 README §3.6），它编译时要 Qt 的**私有头**
# （QtQuick/private/qquickitem_p.h 之类），发行版包得单独装这两个：
#     sudo apt install qt6-base-private-dev qt6-declarative-private-dev
# （Qt 官网安装包自带私有头，不用额外装；缺了会在配置/编译 QWindowKit 时报错。）
#
# 说明：SDL2 在 Linux 上是**必需**的 —— framework/Linux.cmake 里 ENABLE_SDL 被强制打开，
# 音频就是走 SDL2 那套 renderer（SdlAFAudioRender2），少了 SDL2 头文件配置会直接 FATAL_ERROR。
#
# 第三方库（ffmpeg / curl / nghttp2 / libxml2）CMake 会在这两处找：
#   * <repo>/external/install/<库>/Linux/x86_64/{include,lib}   —— 仓库自带的预编译；
#     注意：**本仓库当前没有** Linux 的那套预编译（只有 Android/Windows 的），所以要么
#     用仓库的 external 脚本自己编一份，要么用发行版包 / vcpkg。
#   * 系统默认路径（/usr/include、/usr/lib/...）—— 也就是 apt 装的 -dev 包。
# 用发行版包时有一个坑：框架里写的是 `#include <libxml/xmlreader.h>`（framework/utils/xml/
# DOMParser.h），所以 include 目录必须是"包含 libxml 子目录的那一层"：仓库预编译树是
# .../include/libxml2，而发行版包放在 /usr/include/libxml2。系统包这条路上它不在默认搜索
# 路径里，需要显式加上（脚本会用 $EXTRA_CXX_FLAGS 传）：
#     EXTRA_CXX_FLAGS="-I/usr/include/libxml2" ./build_linux.sh

set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SRC_DIR}/build/linux"
DEPLOY_DIR=""
BUILD_TYPE="Release"
DO_DEPLOY=0
DO_CLEAN=0

while [ $# -gt 0 ]; do
    case "$1" in
        --debug)   BUILD_TYPE="Debug" ;;
        --deploy)  DO_DEPLOY=1
                   # 可选的目录参数：下一个参数不是选项就当目录
                   if [ $# -ge 2 ] && [ "${2#-}" = "$2" ]; then
                       DEPLOY_DIR="$2"; shift
                   fi ;;
        --clean)   DO_CLEAN=1 ;;
        -h|--help) sed -n '2,48p' "$0"; exit 0 ;;
        *) echo "[ERROR] 未知参数: $1（--help 看用法）" >&2; exit 1 ;;
    esac
    shift
done

if [ -z "$DEPLOY_DIR" ]; then
    DEPLOY_DIR="${SRC_DIR}/deploy"
fi

# ---------------------------------------------------------------------------
# 1. 找工具链
# ---------------------------------------------------------------------------
for tool in cmake c++ pkg-config; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "[ERROR] 找不到 $tool —— 先装构建工具（见脚本头部注释）" >&2
        exit 1
    fi
done

# 生成器：有 ninja 用 ninja（快），没有就用 make
GENERATOR="Unix Makefiles"
if command -v ninja >/dev/null 2>&1; then
    GENERATOR="Ninja"
fi

# ---------------------------------------------------------------------------
# 2. 找 Qt
# ---------------------------------------------------------------------------
QT_PREFIX=""
if [ -n "${QTDIR:-}" ] && [ -d "${QTDIR}/lib/cmake/Qt6" ]; then
    QT_PREFIX="${QTDIR}"
else
    for q in qmake6 qmake; do
        if command -v "$q" >/dev/null 2>&1; then
            QT_PREFIX="$("$q" -query QT_INSTALL_PREFIX 2>/dev/null || true)"
            [ -n "$QT_PREFIX" ] && break
        fi
    done

    # 官网安装包的默认位置：~/Qt/<版本>/gcc_64
    # 取第一个匹配到的（glob 顺序），打印出来 —— 想指定版本就 export QTDIR=...
    if [ -z "$QT_PREFIX" ] && [ -d "${HOME}/Qt" ]; then
        for cand in "${HOME}"/Qt/*/gcc_64; do
            if [ -d "${cand}/lib/cmake/Qt6" ]; then
                QT_PREFIX="$cand"
                echo "== 自动找到 Qt: ${QT_PREFIX}（要用别的版本就 export QTDIR=...）"
                break
            fi
        done
    fi
fi

if [ -z "$QT_PREFIX" ]; then
    echo "[ERROR] 找不到 Qt6。两种办法二选一：" >&2
    echo "        * 发行版包：sudo apt install qt6-declarative-dev qt6-base-dev" >&2
    echo "        * 官网安装包：export QTDIR=~/Qt/6.11.1/gcc_64" >&2
    exit 1
fi

echo "== Qt: ${QT_PREFIX}"
echo "== 生成器: ${GENERATOR}, 构建类型: ${BUILD_TYPE}"

# ---------------------------------------------------------------------------
# 3. 配置 + 编译
# ---------------------------------------------------------------------------
if [ "$DO_CLEAN" = "1" ] && [ -d "$BUILD_DIR" ]; then
    echo "== --clean：删除 ${BUILD_DIR}"
    rm -rf "$BUILD_DIR"
fi

CMAKE_ARGS=(-S "$SRC_DIR" -B "$BUILD_DIR" -G "$GENERATOR"
            "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
            "-DCMAKE_PREFIX_PATH=${QT_PREFIX}")

# 额外的编译参数（可选）。典型用法就是补 libxml2 的 include 路径：
#     EXTRA_CXX_FLAGS="-I/usr/include/libxml2" ./build_linux.sh
if [ -n "${EXTRA_CXX_FLAGS:-}" ]; then
    CMAKE_ARGS+=("-DCMAKE_CXX_FLAGS=${EXTRA_CXX_FLAGS}" "-DCMAKE_C_FLAGS=${EXTRA_CXX_FLAGS}")
    echo "== 额外的编译参数: ${EXTRA_CXX_FLAGS}"
fi

# vcpkg 的库如果在这台机器上（Linux 上也可以用它装 FFmpeg/curl/xml2），
# 有 VCPKG_ROOT + 工具链就带上；没有就走系统包。
if [ -n "${VCPKG_ROOT:-}" ] && [ -f "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" ]; then
    echo "== 用 vcpkg 工具链: ${VCPKG_ROOT}"
    CMAKE_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
fi

echo "== [1/3] configure"
cmake "${CMAKE_ARGS[@]}"

echo "== [2/3] build"
cmake --build "$BUILD_DIR" --parallel

BIN="${BUILD_DIR}/appQtPlayer"
if [ ! -x "$BIN" ]; then
    echo "[ERROR] 构建结束但没有 ${BIN}" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 4. 部署（可选）+ 自检
# ---------------------------------------------------------------------------
if [ "$DO_DEPLOY" = "1" ]; then
    echo "== [3/3] install -> ${DEPLOY_DIR}"
    cmake --install "$BUILD_DIR" --prefix "$DEPLOY_DIR"

    echo
    echo "---- 依赖自检 ----"
    TARGET="${DEPLOY_DIR}/appQtPlayer"
    if [ -x "$TARGET" ]; then
        echo "  [ok]      appQtPlayer"
    else
        echo "  [缺失]    appQtPlayer" >&2
        exit 1
    fi

    # ldd 会说清楚缺哪个 .so（Qt 是系统包时这里什么都不缺，属正常）
    if command -v ldd >/dev/null 2>&1; then
        missing="$(ldd "$TARGET" 2>/dev/null | awk '/not found/ {print $1}' || true)"
        if [ -n "$missing" ]; then
            echo "  [缺失]    动态库：$missing" >&2
            echo "            Qt 是系统包的话通常装 qt6-* 就行；否则设 QTDIR 指向完整 Qt。" >&2
            exit 1
        fi
        echo "  [ok]      ldd 没有 not found"
    fi

    # QML 模块：系统 Qt 时在 QT_INSTALL_QML 下；官网 Qt 部署树里在 <prefix>/qml。
    # 注意 Linux 上 QML 插件的文件名是 libqtquick2plugin.so（Windows 上是 .dll）。
    QML_DIR="$(qmake6 -query QT_INSTALL_QML 2>/dev/null || qmake -query QT_INSTALL_QML 2>/dev/null || true)"
    found_qml=0
    for cand in "${DEPLOY_DIR}/qml" "${QML_DIR}"; do
        if [ -n "$cand" ] && [ -e "${cand}/QtQuick/libqtquick2plugin.so" ]; then
            echo "  [ok]      QtQuick QML 模块：${cand}/QtQuick"
            found_qml=1
            break
        fi
    done
    if [ "$found_qml" = "0" ]; then
        echo "  [警告]    没找到 QtQuick 的 QML 插件（libqtquick2plugin.so）。"
        echo "            系统 Qt 上一般由 qt6-declarative-dev 提供；官网 Qt 请确认部署树里有 qml/。"
    fi

    echo
    echo "== 部署完成：${DEPLOY_DIR}"
    echo "   运行：${DEPLOY_DIR}/appQtPlayer [视频路径]"
else
    echo
    echo "== 构建完成：${BIN}"
    echo "   运行：${BIN} [视频路径]"
    echo "   （想装成一棵独立目录树：./build_linux.sh --deploy）"
fi
