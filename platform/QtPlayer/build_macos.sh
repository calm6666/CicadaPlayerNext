#!/usr/bin/env bash
#
# QtPlayer —— macOS 构建 / 部署脚本
#
# 用法：
#     cd <本目录>
#     ./build_macos.sh                     # 配置 + 编译 Release（产物在 build/macos/）
#     ./build_macos.sh --deploy            # 额外 cmake --install 到 ./deploy（会做 macdeployqt）
#     ./build_macos.sh --deploy /tmp/out   # 部署到指定目录
#     ./build_macos.sh --debug             # Debug 构建
#     ./build_macos.sh --clean             # 删掉构建目录重新配置
#     ./build_macos.sh --qt-prefix ~/Qt/6.8.0/macos   # 指定 Qt 前缀（跳过自动探测）
#     ./build_macos.sh --build-dir build/macos-static # 指定构建目录（deploy_macos.sh 用）
#
# Qt 怎么找（按顺序）：
#   1. 环境变量 QTDIR（例：export QTDIR=~/Qt/6.11.1/macos）—— Qt 官网安装包用这个；
#   2. PATH 里的 qmake6 / qmake —— brew 装的 qt 走这个；
#   都不行就报错提示你设哪一个，**不写死任何机器上的路径**。
#
# 依赖：
#     xcode-select --install                       # clang / SDK
#     brew install cmake ninja ffmpeg curl nghttp2 # ffmpeg/curl/nghttp2 是框架要的
#     # 可选：brew install sdl2                    # 只有把 ENABLE_SDL 打开时才需要
#
# 关于 ffmpeg/curl/nghttp2 的位置：framework/macOSX.cmake 会去这两个地方找
#   * <repo>/external/install/<库>/Darwin/<arch>/{include,lib}   （仓库自带的预编译）
#   * /opt/homebrew/{include,lib}                                 （Apple Silicon 的 brew）
# 所以 Apple Silicon 上用 brew 装就行；Intel Mac 的 brew 在 /usr/local，那个路径没被探测 ——
# 要么把库装到 external/install/Darwin/x86_64 下，要么给 CMake 传 -DCMAKE_PREFIX_PATH 指到
# 你的 brew 前缀（本脚本会把 QTDIR 之外的 CMAKE_PREFIX_PATH 原样保留，见下面 CMAKE_ARGS）。
#
# 关于零拷贝：macOS 这条路是 VideoToolbox 直出 CVPixelBuffer(BGRA) -> CVMetalTextureCache
# -> MTLTexture，用的是系统框架，不需要额外装东西；场景图为 Metal 时自动生效。
#
# 关于无边框窗口：用的是 3rdparty/qwindowkit（见 README §3.6），它编译时要 Qt 的私有头
# （QtQuick/private/...）。Qt 官网安装包自带，brew 装的 qt 一般也带；如果配置时报
# find_package(Qt6QuickPrivate) 失败，说明这个 Qt 是精简打包的，换官网版即可。

set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SRC_DIR}/build/macos"
DEPLOY_DIR=""
BUILD_TYPE="Release"
DO_DEPLOY=0
DO_CLEAN=0
EXTRA_PREFIX=""
# 命令行指定的 Qt 前缀（空 = 走下面的自动探测）。deploy_macos.sh 用它把"静态/动态"
# 那条 Qt 前缀传进来，而不用在这里再抄一份探测逻辑。
QT_PREFIX_OVERRIDE=""
# deploy_macos.sh 传进来的构建目录（把它动态/静态分开：build/macos 与 build/macos-static）。
DO_CLEAN_REQUESTED_DIR=""

while [ $# -gt 0 ]; do
    case "$1" in
        --debug)   BUILD_TYPE="Debug" ;;
        --deploy)  DO_DEPLOY=1
                   if [ $# -ge 2 ] && [ "${2#-}" = "$2" ]; then
                       DEPLOY_DIR="$2"; shift
                   fi ;;
        --qt-prefix)
                   if [ $# -ge 2 ] && [ -n "$2" ]; then
                       QT_PREFIX_OVERRIDE="$2"; shift
                   else
                       echo "[ERROR] --qt-prefix 需要目录参数" >&2; exit 1
                   fi ;;
        --build-dir)
                   if [ $# -ge 2 ] && [ -n "$2" ]; then
                       DO_CLEAN_REQUESTED_DIR="$2"; shift
                   else
                       echo "[ERROR] --build-dir 需要目录参数" >&2; exit 1
                   fi ;;
        --clean)   DO_CLEAN=1 ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "[ERROR] 未知参数: $1（--help 看用法）" >&2; exit 1 ;;
    esac
    shift
done

# --build-dir 要在"默认值"之后生效（也允许相对路径）
if [ -n "$DO_CLEAN_REQUESTED_DIR" ]; then
    case "$DO_CLEAN_REQUESTED_DIR" in
        /*) BUILD_DIR="$DO_CLEAN_REQUESTED_DIR" ;;
        *)  BUILD_DIR="${SRC_DIR}/${DO_CLEAN_REQUESTED_DIR}" ;;
    esac
fi

if [ -z "$DEPLOY_DIR" ]; then
    DEPLOY_DIR="${SRC_DIR}/deploy"
fi

# ---------------------------------------------------------------------------
# 1. 找工具链
# ---------------------------------------------------------------------------
if ! xcode-select -p >/dev/null 2>&1; then
    echo "[ERROR] 没有 Xcode command line tools：xcode-select --install" >&2
    exit 1
fi

for tool in cmake c++; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "[ERROR] 找不到 $tool —— brew install cmake（编译器跟着 Xcode 走）" >&2
        exit 1
    fi
done

GENERATOR="Unix Makefiles"
if command -v ninja >/dev/null 2>&1; then
    GENERATOR="Ninja"
fi

# ---------------------------------------------------------------------------
# 2. 找 Qt
# ---------------------------------------------------------------------------
QT_PREFIX=""
if [ -n "${QT_PREFIX_OVERRIDE}" ]; then
    # 命令行指定优先：不再做任何"猜"，但目录必须真的是一份 Qt6 安装。
    if [ ! -d "${QT_PREFIX_OVERRIDE}/lib/cmake/Qt6" ]; then
        echo "[ERROR] --qt-prefix 指向的目录里没有 lib/cmake/Qt6：${QT_PREFIX_OVERRIDE}" >&2
        exit 1
    fi
    QT_PREFIX="${QT_PREFIX_OVERRIDE}"
    echo "== 命令行指定的 Qt: ${QT_PREFIX}"
elif [ -n "${QTDIR:-}" ] && [ -d "${QTDIR}/lib/cmake/Qt6" ]; then
    QT_PREFIX="${QTDIR}"
else
    for q in qmake6 qmake; do
        if command -v "$q" >/dev/null 2>&1; then
            QT_PREFIX="$("$q" -query QT_INSTALL_PREFIX 2>/dev/null || true)"
            [ -n "$QT_PREFIX" ] && break
        fi
    done

    # 官网安装包的默认位置：~/Qt/<版本>/macos
    # 取第一个匹配到的（glob 顺序），打印出来 —— 想指定版本就 export QTDIR=...
    if [ -z "$QT_PREFIX" ] && [ -d "${HOME}/Qt" ]; then
        for cand in "${HOME}"/Qt/*/macos; do
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
    echo "        * 官网安装包：export QTDIR=~/Qt/6.11.1/macos" >&2
    echo "        * brew：brew install qt" >&2
    exit 1
fi

# CMAKE_PREFIX_PATH 里同时给 Qt 和（可能的）brew 前缀，两边都能找到。
if [ -d /opt/homebrew ]; then
    EXTRA_PREFIX="/opt/homebrew"
elif [ -d /usr/local/opt ]; then
    # Intel Mac 的 brew 默认前缀；框架自己只探测 /opt/homebrew，这里补上。
    EXTRA_PREFIX="/usr/local"
fi

echo "== Qt: ${QT_PREFIX}"
[ -n "$EXTRA_PREFIX" ] && echo "== 额外的库前缀: ${EXTRA_PREFIX}"
echo "== 生成器: ${GENERATOR}, 构建类型: ${BUILD_TYPE}"

# ---------------------------------------------------------------------------
# 3. 配置 + 编译
# ---------------------------------------------------------------------------
if [ "$DO_CLEAN" = "1" ] && [ -d "$BUILD_DIR" ]; then
    echo "== --clean：删除 ${BUILD_DIR}"
    rm -rf "$BUILD_DIR"
fi

PREFIX_PATH="${QT_PREFIX}"
[ -n "$EXTRA_PREFIX" ] && PREFIX_PATH="${PREFIX_PATH};${EXTRA_PREFIX}"

CMAKE_ARGS=(-S "$SRC_DIR" -B "$BUILD_DIR" -G "$GENERATOR"
            "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
            "-DCMAKE_PREFIX_PATH=${PREFIX_PATH}")

# 想指定架构（例如在 Apple Silicon 上编 x86_64）时：
#     CMAKE_OSX_ARCHITECTURES=x86_64 ./build_macos.sh
if [ -n "${CMAKE_OSX_ARCHITECTURES:-}" ]; then
    CMAKE_ARGS+=("-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
    echo "== 目标架构: ${CMAKE_OSX_ARCHITECTURES}（注意：框架要的 ffmpeg 等库必须是同架构的）"
fi

echo "== [1/3] configure"
cmake "${CMAKE_ARGS[@]}"

echo "== [2/3] build"
cmake --build "$BUILD_DIR" --parallel

APP="${BUILD_DIR}/appQtPlayer.app"
BIN="${APP}/Contents/MacOS/appQtPlayer"
if [ ! -x "$BIN" ]; then
    # 万一没生成 bundle（比如有人把 MACOSX_BUNDLE 关了），退回普通可执行文件
    if [ -x "${BUILD_DIR}/appQtPlayer" ]; then
        APP=""
        BIN="${BUILD_DIR}/appQtPlayer"
    else
        echo "[ERROR] 构建结束但找不到 appQtPlayer.app / appQtPlayer" >&2
        exit 1
    fi
fi
echo "== 产物: ${BIN}"

# ---------------------------------------------------------------------------
# 4. 部署（可选）+ 自检
# ---------------------------------------------------------------------------
if [ "$DO_DEPLOY" = "1" ]; then
    echo "== [3/3] install -> ${DEPLOY_DIR}"
    # Qt 的部署脚本在 macOS 上会把 Qt 框架和 QML 模块拷进 .app（内部就是 macdeployqt）：
    #   Contents/Frameworks/  Qt*.framework
    #   Contents/Resources/qml/  QtQuick、QtQuick/Dialogs……（有的版本放在 Contents/qml）
    cmake --install "$BUILD_DIR" --prefix "$DEPLOY_DIR"

    echo
    echo "---- 依赖自检 ----"
    D_APP="${DEPLOY_DIR}/appQtPlayer.app"
    D_BIN="${D_APP}/Contents/MacOS/appQtPlayer"
    if [ -x "$D_BIN" ]; then
        echo "  [ok]      appQtPlayer.app/Contents/MacOS/appQtPlayer"
    else
        echo "  [缺失]    部署目录里没有 appQtPlayer.app" >&2
        exit 1
    fi

    # otool -L 应该只看到系统库 + @rpath（即 bundle 内部的 Qt），不应出现 Qt 安装目录的绝对路径
    if command -v otool >/dev/null 2>&1; then
        leaked="$(otool -L "$D_BIN" | awk 'NR>1 {print $1}' | grep -E '/(Qt|Homebrew|opt)/' || true)"
        if [ -n "$leaked" ]; then
            echo "  [警告]    可执行文件仍引用外部路径（换机器可能起不来）："
            echo "$leaked" | sed 's/^/            /'
        else
            echo "  [ok]      otool 没看到外部 Qt/Homebrew 路径"
        fi
    fi

    # QML 模块（macOS 上是 .dylib）
    found_qml=0
    for cand in "${D_APP}/Contents/Resources/qml" "${D_APP}/Contents/qml"; do
        if [ -e "${cand}/QtQuick/libqtquick2plugin.dylib" ]; then
            echo "  [ok]      QtQuick QML 模块：${cand}/QtQuick"
            found_qml=1
            break
        fi
    done
    if [ "$found_qml" = "0" ]; then
        echo "  [警告]    bundle 里没找到 QtQuick 的 QML 插件（libqtquick2plugin.dylib）——"
        echo "            运行会报 'Module QtPlayer contains no type named Main' 之类的错。"
        echo "            可以手工补一次：macdeployqt \"${D_APP}\" -qmldir=\"${SRC_DIR}\""
    fi

    echo
    echo "== 部署完成：${D_APP}"
    echo "   双击/打开：open \"${D_APP}\""
    echo "   想直接看日志（框架的日志是打到 stdout 的）："
    echo "       \"${D_BIN}\" [视频路径]"
    echo "   （要分发给别人还需要 Developer ID 签名 + 公证，这里只做本机可用。）"
else
    echo
    echo "== 构建完成：${BIN}"
    echo "   运行：open \"${APP}\"           # 或直接跑 ${BIN} 看日志"
    echo "   （想装成 .app + Qt 框架齐全的样子：./build_macos.sh --deploy）"
fi
