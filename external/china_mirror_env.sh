#!/usr/bin/env bash
# ============================================================================
# 国内镜像预设（可选）：构建前 source 本文件即可切换依赖源
#
# 用法（二选一）：
#   在 external/ 目录内：  . ./china_mirror_env.sh
#   或在仓库根目录：        source external/china_mirror_env.sh
#   然后正常执行 ./build_external.sh Android|iOS|OHOS|...
#
# 也可以不 source 本文件，直接 export 对应的变量，效果相同。
# 所有 *_GIT 与 GIT_MIRROR_PREFIX 都是"构建时传入，不传就用 github"。
# 镜像 clone 失败会自动回退 github，详见 docs/ChinaMirrors.md。
# ============================================================================

# ---------------- 方式 A：gitee 官方镜像（快照式） ----------------
# 说明：gitee.com/mirrors 为人工维护的快照仓库，多数时间与上游同步，
#       但新发布的 tag（如 FFmpeg n9.0）偶尔会滞后几天；若 tag 缺失，
#       脚本会自动回退 github 完成 clone。
export FFMPEG_GIT=https://gitee.com/mirrors/ffmpeg.git
export OPENSSL_GIT=https://gitee.com/mirrors/openssl.git
export CURL_GIT=https://gitee.com/mirrors/curl.git
export LIBXML2_GIT=https://gitee.com/mirrors/libxml2.git
export NGHTTP2_GIT=https://gitee.com/mirrors/nghttp2.git
export DAV1D_GIT=https://gitee.com/mirrors/dav1d.git

# ---------------- 方式 B：统一前缀代理镜像（实时同步） ----------------
# 与方式 A 互斥使用（把上面 A 的 export 注释掉再打开下面任一行）。
# 这类服务是 github 的实时代理，永远与上游一致，但公共代理站点域名偶尔会变，
# 失效时脚本同样会自动回退 github。
#
# export GIT_MIRROR_PREFIX=https://gitclone.com
# export GIT_MIRROR_PREFIX=https://mirror.ghproxy.com
# （其他前缀型代理同样适用，只要支持 "<前缀>/github.com/<owner>/<repo>" 的 URL 形式）

# ---------------- 其它国内加速（可选） ----------------
# Android gradle 依赖走阿里云 Maven 镜像（gradle 构建前 export）：
#   export USE_CHINA_MIRROR=true
#
# NDK 下载走 npmmirror 二进制镜像（CI 或手工下载时）：
#   ANDROID_NDK_URL=https://registry.npmmirror.com/-/binary/android-ndk/android-ndk-r25c-linux.zip
#
# macOS 上 Homebrew 加速（阿里云镜像，可选）：
#   export HOMEBREW_BREW_GIT_REMOTE=https://mirrors.aliyun.com/homebrew/brew.git
#   export HOMEBREW_CORE_GIT_REMOTE=https://mirrors.aliyun.com/homebrew/homebrew-core.git
