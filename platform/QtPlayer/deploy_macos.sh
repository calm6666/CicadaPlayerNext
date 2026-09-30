#!/usr/bin/env bash
#
# deploy_macos.sh —— macOS 上"编译 + 打包"Qt 播放器，语义对齐 deploy_win.bat。
#
#     ./deploy_macos.sh                       # 动态 Qt -> build/macos        -> deploy/
#     ./deploy_macos.sh --static              # 静态 Qt -> build/macos-static -> deploy-static/
#     ./deploy_macos.sh --static <QT-PREFIX>  # 指定静态 Qt 安装位置
#     ./deploy_macos.sh --shared              # 就算设了 QT_STATIC_DIR 也走动态
#     ./deploy_macos.sh --qt-prefix DIR       # 指定 Qt 前缀（模式仍由 --static/--shared 决定）
#     ./deploy_macos.sh --arch arm64|x86_64|universal --debug --clean --zip
#     ./deploy_macos.sh --no-build            # 只打包已经编好的
#     ./deploy_macos.sh --deploy-dir out --build-dir build/macos-test
#
# 约定（与 deploy_win.bat 逐条对应）：
#   * 不带参数 = **动态 Qt**，产物进 deploy/；
#   * --static  = 静态 Qt，产物进 deploy-static/，构建目录 build/macos-static；
#   * 静态 Qt 的位置按顺序找：--qt-prefix / --static 后面的参数 → 环境变量 QT_STATIC_DIR
#     （别名 CICADA_QT_STATIC_DIR）→ 脚本自己猜 ~/Qt-Static/macos、/opt/Qt-Static/macos、
#     /usr/local/Qt-Static/macos；
#   * **请求静态但没有可用的静态 Qt 就报错退出，绝不悄悄改成动态构建**（Windows 那份同理）。
#   * 构建逻辑不在这里抄第二份：编译 + cmake --install 全部交给 build_macos.sh
#     （本脚本只负责"选 Qt / 选目录 / 选模式"和 macOS 特有的打包后处理）。
#
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

MODE="dynamic"          # dynamic | static
QT_PREFIX_ARG=""
DEPLOY_DIR=""
BUILD_DIR=""
BUILD_TYPE="Release"
DO_CLEAN=0
DO_BUILD=1
DO_ZIP=0
DO_MACDEPLOYQT=1
ARCH="${CMAKE_OSX_ARCHITECTURES:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        --static|static)
            MODE="static"
            # 允许直接跟一个 Qt 前缀（和 deploy_win.bat "static D:\Qt-Static\msvc" 一样）
            if [ $# -ge 2 ] && [ "${2#-}" = "$2" ]; then
                QT_PREFIX_ARG="$2"; shift
            fi ;;
        --shared|shared) MODE="dynamic" ;;
        --qt-prefix)
            if [ $# -ge 2 ] && [ -n "$2" ]; then QT_PREFIX_ARG="$2"; shift
            else echo "[ERROR] --qt-prefix 需要目录参数" >&2; exit 1; fi ;;
        --deploy-dir)
            if [ $# -ge 2 ] && [ -n "$2" ]; then DEPLOY_DIR="$2"; shift
            else echo "[ERROR] --deploy-dir 需要目录参数" >&2; exit 1; fi ;;
        --build-dir)
            if [ $# -ge 2 ] && [ -n "$2" ]; then BUILD_DIR="$2"; shift
            else echo "[ERROR] --build-dir 需要目录参数" >&2; exit 1; fi ;;
        --debug)   BUILD_TYPE="Debug" ;;
        --arch)
            if [ $# -ge 2 ] && [ -n "$2" ]; then ARCH="$2"; shift
            else echo "[ERROR] --arch 需要参数（arm64 / x86_64 / universal）" >&2; exit 1; fi ;;
        --clean)   DO_CLEAN=1 ;;
        --zip)     DO_ZIP=1 ;;
        --no-build) DO_BUILD=0 ;;
        --no-macdeployqt) DO_MACDEPLOYQT=0 ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "[ERROR] 未知参数: $1（--help 看用法）" >&2; exit 1 ;;
    esac
    shift
done

if [ "$(uname)" != "Darwin" ]; then
    echo "[ERROR] deploy_macos.sh 只能在 macOS 上跑（当前 $(uname)）" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 1. 目录：动态/静态分开，和 Windows 的 build\msvc vs build\msvc-static 对齐
# ---------------------------------------------------------------------------
if [ -z "$BUILD_DIR" ]; then
    if [ "$MODE" = "static" ]; then
        BUILD_DIR="${SRC_DIR}/build/macos-static"
    else
        BUILD_DIR="${SRC_DIR}/build/macos"
    fi
fi
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="${SRC_DIR}/${BUILD_DIR}" ;; esac

if [ -z "$DEPLOY_DIR" ]; then
    if [ "$MODE" = "static" ]; then
        DEPLOY_DIR="${SRC_DIR}/deploy-static"
    else
        DEPLOY_DIR="${SRC_DIR}/deploy"
    fi
fi
case "$DEPLOY_DIR" in /*) ;; *) DEPLOY_DIR="${SRC_DIR}/${DEPLOY_DIR}" ;; esac

# ---------------------------------------------------------------------------
# 2. 选 Qt
# ---------------------------------------------------------------------------
is_qt6()      { [ -d "$1/lib/cmake/Qt6" ]; }
# 判"这份 Qt 是不是静态"：
#   * 非 framework 的静态 Qt：lib/libQt6Core.a；
#   * **macOS 上很常见的静态 framework**：lib/QtCore.framework/QtCore 是一个 ar 归档
#     （动态 Qt 在同一个位置是 Mach-O dylib）。只看 libQt6Core.a 会把这种静态 Qt
#     误判成"不是静态"从而拒绝，所以这里用 file(1) 看文件类型。
is_static_qt() {
    local q="$1"
    [ -f "$q/lib/libQt6Core.a" ] && return 0
    [ -f "$q/lib/libQt6Core_debug.a" ] && return 0

    local fw="$q/lib/QtCore.framework/QtCore"
    [ -f "$fw" ] || fw="$q/lib/QtCore.framework/Versions/A/QtCore"
    [ -f "$fw" ] || return 1

    file -b "$fw" 2>/dev/null | grep -qi 'archive' && return 0
    return 1
}

if [ -n "$QT_PREFIX_ARG" ]; then
    QT_PREFIX="$QT_PREFIX_ARG"
elif [ "$MODE" = "static" ]; then
    QT_PREFIX="${QT_STATIC_DIR:-${CICADA_QT_STATIC_DIR:-}}"
    if [ -z "$QT_PREFIX" ]; then
        for cand in "${HOME}/Qt-Static/macos" "/opt/Qt-Static/macos" "/usr/local/Qt-Static/macos"; do
            if is_qt6 "$cand"; then QT_PREFIX="$cand"; break; fi
        done
    fi
else
    QT_PREFIX="${QTDIR:-}"
fi

if [ "$MODE" = "static" ]; then
    if [ -z "$QT_PREFIX" ] || ! is_qt6 "$QT_PREFIX"; then
        echo "[ERROR] 请求静态构建，但找不到可用的静态 Qt6。" >&2
        echo "        按顺序找过：--qt-prefix / --static 后面的参数 → QT_STATIC_DIR" >&2
        echo "                     → ~/Qt-Static/macos / /opt/Qt-Static/macos / /usr/local/Qt-Static/macos" >&2
        echo "        指定一个：./deploy_macos.sh --static ~/Qt-Static/macos" >&2
        echo "        （**不会**退回动态构建 —— 那会把'静态'这个请求悄悄改掉。）" >&2
        exit 1
    fi

    if ! is_static_qt "$QT_PREFIX"; then
        echo "[ERROR] ${QT_PREFIX} 看起来不是静态 Qt（没找到 lib/libQt6Core.a）。" >&2
        echo "        静态 Qt 要用 -static 编出来的那份；动态 Qt 只有 *.framework。" >&2
        exit 1
    fi

    echo "== 模式: 静态（Qt: ${QT_PREFIX}）"
else
    if [ -n "$QT_PREFIX" ] && ! is_qt6 "$QT_PREFIX"; then
        echo "[WARN] ${QT_PREFIX} 里没有 lib/cmake/Qt6，交给 build_macos.sh 自己探测" >&2
        QT_PREFIX=""
    fi

    if [ -n "$QT_PREFIX" ] && is_static_qt "$QT_PREFIX"; then
        # 不报错：有人用静态 Qt 走默认（动态）分支也能编过，只是不需要 macdeployqt。
        echo "[WARN] 动态模式下给的 Qt 前缀其实是静态 Qt（有 libQt6Core.a）；" >&2
        echo "       如果你要的是静态产物，请用 --static（产物会进 deploy-static/）。" >&2
    fi

    echo "== 模式: 动态（Qt: ${QT_PREFIX:-由 build_macos.sh 自动探测}）"
fi

echo "== 构建目录: ${BUILD_DIR}"
echo "== 部署目录: ${DEPLOY_DIR}"

# ---------------------------------------------------------------------------
# 3. 编译 + cmake --install（全部交给 build_macos.sh，避免两份逻辑走偏）
# ---------------------------------------------------------------------------
BUILD_ARGS=(--deploy "$DEPLOY_DIR" --build-dir "$BUILD_DIR")
# 静态/动态要一路传到 CMake：CMakeLists 里有个守卫会拦"开关与 Qt 实际形态不一致"
# （静态 Qt 没有 -DCICADA_QT_STATIC=ON 会直接 FATAL_ERROR，报错原文就写着
#  "deploy_win.ps1 -Static does it for you"）。所以这里按模式显式传。
if [ "$MODE" = "static" ]; then
    BUILD_ARGS+=(--static)
else
    BUILD_ARGS+=(--shared)
fi
[ "$BUILD_TYPE" = "Debug" ] && BUILD_ARGS+=(--debug)
[ "$DO_CLEAN" = "1" ] && BUILD_ARGS+=(--clean)
[ -n "$QT_PREFIX" ] && BUILD_ARGS+=(--qt-prefix "$QT_PREFIX")

if [ "$DO_BUILD" = "1" ]; then
    echo "== [1/3] 编译（build_macos.sh ${BUILD_ARGS[*]}）"
    if [ -n "$ARCH" ]; then
        CMAKE_OSX_ARCHITECTURES="$ARCH" bash "${SRC_DIR}/build_macos.sh" "${BUILD_ARGS[@]}"
    else
        bash "${SRC_DIR}/build_macos.sh" "${BUILD_ARGS[@]}"
    fi
else
    echo "== [1/3] 跳过编译（--no-build）"
fi

# ---------------------------------------------------------------------------
# 4. 找到 .app
# ---------------------------------------------------------------------------
APP="${DEPLOY_DIR}/appQtPlayer.app"
if [ ! -d "$APP" ]; then
    APP="$(find "$DEPLOY_DIR" -maxdepth 3 -name 'appQtPlayer.app' -type d 2>/dev/null | head -1 || true)"
fi
if [ -z "$APP" ] || [ ! -d "$APP" ]; then
    echo "[ERROR] 在 ${DEPLOY_DIR} 下找不到 appQtPlayer.app。先跑一次不带 --no-build 的构建。" >&2
    exit 1
fi
BIN="${APP}/Contents/MacOS/appQtPlayer"
if [ ! -x "$BIN" ]; then
    echo "[ERROR] ${APP} 里没有可执行文件 appQtPlayer" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 5. macOS 特有的打包后处理
# ---------------------------------------------------------------------------
if [ "$MODE" = "dynamic" ] && [ "$DO_MACDEPLOYQT" = "1" ]; then
    MACDEPLOYQT=""
    [ -n "$QT_PREFIX" ] && [ -x "${QT_PREFIX}/bin/macdeployqt" ] && MACDEPLOYQT="${QT_PREFIX}/bin/macdeployqt"
    [ -z "$MACDEPLOYQT" ] && command -v macdeployqt >/dev/null 2>&1 && MACDEPLOYQT="$(command -v macdeployqt)"

    if [ -n "$MACDEPLOYQT" ]; then
        echo "== [2/3] macdeployqt（把 Qt 框架 / QML 模块 / 插件拷进 .app）"
        "$MACDEPLOYQT" "$APP" -always-overwrite -qmldir="$SRC_DIR" || {
            echo "[WARN] macdeployqt 返回非 0 —— 下面的依赖自检会指出还缺什么" >&2
        }
    else
        echo "[WARN] 找不到 macdeployqt（在 Qt 的 bin/ 下）。动态产物换机器可能缺 Qt 框架；" >&2
        echo "       可以手工补：\"${QT_PREFIX}/bin/macdeployqt\" \"${APP}\" -qmldir=\"${SRC_DIR}\"" >&2
    fi
else
    echo "== [2/3] 跳过 macdeployqt（静态构建 / --no-macdeployqt）"
fi

# 5b. 动态库（Danmaku / 框架产物）——静态构建不需要。
FRAMEWORKS_DIR="${APP}/Contents/Frameworks"
mkdir -p "$FRAMEWORKS_DIR"

copy_dylib_if_needed() {
    local dylib="$1"
    [ -f "$dylib" ] || return 0
    local name; name="$(basename "$dylib")"
    if [ -f "${FRAMEWORKS_DIR}/${name}" ]; then
        return 0
    fi
    echo "   += ${name}"
    cp -f "$dylib" "${FRAMEWORKS_DIR}/${name}"
    install_name_tool -id "@rpath/${name}" "${FRAMEWORKS_DIR}/${name}" 2>/dev/null || true
}

# 弹幕引擎：Windows 上是把 Danmaku.dll 放在 exe 旁边，macOS 上是 dylib
for cand in "${BUILD_DIR}"/libDanmaku*.dylib "${BUILD_DIR}"/**/libDanmaku*.dylib; do
    [ -f "$cand" ] && copy_dylib_if_needed "$cand"
done

# 5c. rpath：让 .app 内部的 Frameworks 能被找到（重复添加会让 install_name_tool 报错，先查）
if ! otool -l "$BIN" 2>/dev/null | grep -q "@executable_path/../Frameworks"; then
    install_name_tool -add_rpath "@executable_path/../Frameworks" "$BIN" 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# 6. 依赖自检：只剩系统库 / @rpath / .app 内部，才算"换机器能跑"
# ---------------------------------------------------------------------------
echo "== [3/3] 依赖自检（otool -L）"
MISSING="$(otool -L "$BIN" 2>/dev/null | tail -n +2 | awk '{print $1}' | \
           grep -vE '^(/usr/lib/|/System/|@rpath/|@executable_path/|@loader_path/)' || true)"

if [ -n "$MISSING" ]; then
    echo "[WARN] 下面这些依赖不在系统目录、也不在 .app 里（拷到别的机器可能起不来）：" >&2
    echo "$MISSING" | sed 's/^/       /' >&2
    echo "       动态构建：确认 macdeployqt 跑过、并且这些库在 Qt/brew 前缀里；" >&2
    echo "       静态构建：说明还有动态依赖没被静态链上（检查 external/install 里的 .a）。" >&2
else
    echo "   依赖干净：只剩系统库与 @rpath/@executable_path。"
fi

# ---------------------------------------------------------------------------
# 7. 可选：打 zip（macOS 上对 .app 用 ditto 才是正确的）
# ---------------------------------------------------------------------------
if [ "$DO_ZIP" = "1" ]; then
    ZIP_NAME="appQtPlayer-macos-${ARCH:-$(uname -m)}.zip"
    ZIP_PATH="${DEPLOY_DIR}/${ZIP_NAME}"
    rm -f "$ZIP_PATH"
    echo "== 打包 ${ZIP_NAME}"
    ditto -c -k --sequesterRsrc --keepParent "$APP" "$ZIP_PATH"
    echo "== 产物: ${ZIP_PATH}"
fi

echo
echo "== 完成"
echo "   模式    : ${MODE}"
echo "   Qt      : ${QT_PREFIX:-（自动探测）}"
echo "   .app    : ${APP}"
echo "   部署目录: ${DEPLOY_DIR}"
echo "   运行    : open \"${APP}\""
