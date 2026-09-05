#!/usr/bin/env bash
# ============================================================================
# 依赖源码获取脚本（FFmpeg 9.0 / OpenSSL / curl / nghttp2 / libxml2 / dav1d）
#
# 支持国内镜像，优先级（构建时通过环境变量传入，不传则默认 github）：
#   1. 单仓库显式覆盖：
#        FFMPEG_GIT / OPENSSL_GIT / CURL_GIT / LIBXML2_GIT /
#        NGHTTP2_GIT / DAV1D_GIT
#      例：export FFMPEG_GIT=https://gitee.com/mirrors/ffmpeg.git
#   2. 统一前缀镜像 GIT_MIRROR_PREFIX（对 github 地址做前缀拼接，
#      适配 gitclone / ghproxy 等代理式镜像，实时与上游同步）：
#      例：export GIT_MIRROR_PREFIX=https://gitclone.com
#      例：export GIT_MIRROR_PREFIX=https://mirror.ghproxy.com
#   3. 默认：github.com
#
# 镜像 clone 失败时自动回退 github 源重试，不会中断构建。
# 版本也可覆盖：FFMPEG_BRANCH / OPENSSL_BRANCH / NGHTTP2_BRANCH /
#               CURL_BRANCH / LIBXML2_BRANCH / DAV1D_BRANCH
# 开箱即用的国内预设见同目录 china_mirror_env.sh，使用说明见 docs/ChinaMirrors.md
# ============================================================================

function clone_git() {
  if [[ -n "$3" ]];then
    git clone --single-branch -b "$2" "$1" "$3"
  else
    git clone --single-branch -b "$2" "$1"
  fi
}

# 将地址按镜像前缀改写：去掉原 scheme 后拼到 GIT_MIRROR_PREFIX 之后。
#   https://github.com/FFmpeg/FFmpeg.git
#     + GIT_MIRROR_PREFIX=https://gitclone.com
#     => https://gitclone.com/github.com/FFmpeg/FFmpeg.git
function apply_mirror() {
    local url=$1
    if [[ -n "${GIT_MIRROR_PREFIX}" ]]; then
        echo "${GIT_MIRROR_PREFIX}/$(echo "$url" | sed 's#^https\?://##')"
    else
        echo "$url"
    fi
}

# clone（优先镜像），镜像失败时自动回退默认 github 源
function clone_git_mirror() {
    local default_url=$1
    local branch=$2
    local dir=${3:-}
    local url
    url=$(apply_mirror "$default_url")

    if clone_git "$url" "$branch" "$dir"; then
        return 0
    fi

    if [[ "$url" != "$default_url" ]]; then
        echo "mirror clone failed (${url}), retry with ${default_url}"
        clone_git "$default_url" "$branch" "$dir"
        return $?
    fi

    return 1
}

if [[ -z "${FFMPEG_GIT}" ]];then
    FFMPEG_GIT=https://github.com/FFmpeg/FFmpeg.git
fi
if [[ -z "${FFMPEG_BRANCH}" ]];then
    # FFmpeg 9.0 "Lei" (2025). Requires C11-capable compiler; NDK r25+/Xcode 14+.
    FFMPEG_BRANCH=n9.0
fi

# The patches under external/contribute/ffmpeg/ were written for FFmpeg 4.3.x and
# are NOT compatible with FFmpeg 9.0 (libavformat.v no longer exists, HEVC-in-FLV
# is upstream since FFmpeg 5.0). Keep them OFF by default; only enable when
# deliberately backporting a vendor extension against the pinned branch.
if [[ -z "${FFMPEG_NEED_PATCH}" ]];then
    FFMPEG_NEED_PATCH=FALSE
fi
clone_git_mirror "$FFMPEG_GIT" "$FFMPEG_BRANCH" ffmpeg

if [[ -z "${OPENSSL_GIT}" ]];then
    OPENSSL_GIT="https://github.com/openssl/openssl.git"
fi
if [[ -z "${OPENSSL_BRANCH}" ]];then
    # OpenSSL 3.0 LTS（3.0.15 为最后一个 3.0.x 补丁版）。
    # 1.1.1 已 EOL 且不兼容新 NDK 布局；如需回退用 OPENSSL_BRANCH=OpenSSL_1_1_1g
    # （构建脚本仍内置旧 NDK 兼容 shim）。
    OPENSSL_BRANCH="openssl-3.0.15"
fi
clone_git_mirror "$OPENSSL_GIT" "$OPENSSL_BRANCH"

if [[ -z "${NGHTTP2_GIT}" ]];then
    NGHTTP2_GIT="https://github.com/nghttp2/nghttp2.git"
fi
if [[ -z "${NGHTTP2_BRANCH}" ]];then
    NGHTTP2_BRANCH="v1.41.0"
fi
clone_git_mirror "${NGHTTP2_GIT}" "${NGHTTP2_BRANCH}"

if [[ -z "${CURL_GIT}" ]];then
    CURL_GIT="https://github.com/curl/curl.git"
fi
if [[ -z "${CURL_BRANCH}" ]];then
    # curl 8.x：原生支持 OpenSSL 3.x（7.68 及更早版本无法与 OpenSSL 3 编译）。
    CURL_BRANCH="curl-8_10_1"
fi
clone_git_mirror "$CURL_GIT" "$CURL_BRANCH"

if [[ -z "${LIBXML2_GIT}" ]];then
    LIBXML2_GIT="https://github.com/GNOME/libxml2.git"
fi
if [[ -z "${LIBXML2_BRANCH}" ]];then
    LIBXML2_BRANCH="v2.9.9"
fi
clone_git_mirror "${LIBXML2_GIT}" "${LIBXML2_BRANCH}"

if [[ -z "${DAV1D_EXTERNAL_DIR}" ]];then
    if [[ -n "${DAV1D_SOURCE_DIR}" ]];then
        echo "check out dav1d"
        if [[ -z "${DAV1D_GIT}" ]];then
            DAV1D_GIT="https://github.com/videolan/dav1d.git"
        fi
        if [[ -z "${DAV1D_BRANCH}" ]];then
            DAV1D_BRANCH="0.6.0"
        fi
        clone_git_mirror "$DAV1D_GIT" "$DAV1D_BRANCH"
    fi
fi
