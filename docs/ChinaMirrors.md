# 国内镜像配置说明（构建时传入，不传则默认 github）

## 1. 设计原则

所有依赖源都遵循同一规则：**构建时通过环境变量传入镜像地址；不传就用默认源
（github.com / dl.google.com / google maven 等）**。镜像 clone 失败会自动回退默认源，
不会中断构建。

优先级（源码类依赖）：

```
单仓库显式覆盖 (*_GIT 环境变量)
      │
统一前缀镜像 (GIT_MIRROR_PREFIX，实时代理)
      │
默认 github.com ── 失败则直接报错（无镜像时不回退）
```

## 2. 依赖清单与获取方式

| 依赖 | 默认源 | 获取方式 | 镜像方式 |
|---|---|---|---|
| FFmpeg n9.0 | github.com/FFmpeg/FFmpeg.git | git clone（`player_git_source_list.sh`） | `FFMPEG_GIT` / `GIT_MIRROR_PREFIX` |
| OpenSSL 3.0.15 (LTS) | github.com/openssl/openssl.git | git clone | `OPENSSL_GIT` / `GIT_MIRROR_PREFIX` |
| curl 8.10.1 | github.com/curl/curl.git | git clone | `CURL_GIT` / `GIT_MIRROR_PREFIX` |
| libxml2 v2.9.9 | github.com/GNOME/libxml2.git | git clone | `LIBXML2_GIT` / `GIT_MIRROR_PREFIX` |
| nghttp2 v1.41.0 | github.com/nghttp2/nghttp2.git | git clone | `NGHTTP2_GIT` / `GIT_MIRROR_PREFIX` |
| dav1d 0.6.0 | github.com/videolan/dav1d.git | git clone（启用时） | `DAV1D_GIT` / `GIT_MIRROR_PREFIX` |
| x264 / fdk-aac | 用户本地源码目录（`user_env.sh`） | 无网络下载 | 可从 gitee 镜像自行 clone 后指向目录 |
| Android NDK r25c（CI） | dl.google.com | wget | `ANDROID_NDK_URL`（npmmirror） |
| gradle 插件/依赖 | google() / jcenter() | gradle | `USE_CHINA_MIRROR=true` → 阿里云 Maven |
| Homebrew（仅 macOS） | raw.githubusercontent.com | curl 脚本 | `HOMEBREW_INSTALL_URL` + 阿里云 brew 源 |
| OHOS SDK | 华为官方 / OpenHarmony 官网 | 手工下载 | 官方渠道本身在国内，无需镜像 |
| Flutter 依赖（如使用） | pub.dev | flutter pub | `PUB_HOSTED_URL=https://pub.flutter-io.cn`、`FLUTTER_STORAGE_BASE_URL=https://storage.flutter-io.cn` |

## 3. 开箱即用：国内预设脚本

新增 `external/china_mirror_env.sh`，构建前 source 一下即可：

```bash
. setup.env
source external/china_mirror_env.sh     # ← 国内镜像预设
cd external
./build_external.sh Android             # 或 iOS / OHOS / macOS / Linux / Windows
```

预设内容（gitee 官方镜像）：

```bash
export FFMPEG_GIT=https://gitee.com/mirrors/ffmpeg.git
export OPENSSL_GIT=https://gitee.com/mirrors/openssl.git
export CURL_GIT=https://gitee.com/mirrors/curl.git
export LIBXML2_GIT=https://gitee.com/mirrors/libxml2.git
export NGHTTP2_GIT=https://gitee.com/mirrors/nghttp2.git
export DAV1D_GIT=https://gitee.com/mirrors/dav1d.git
```

## 4. 两种镜像模式对比

### 4.1 单仓库覆盖（`*_GIT`，gitee 快照式）

```bash
export FFMPEG_GIT=https://gitee.com/mirrors/ffmpeg.git
```

- 优点：gitee 国内速度稳定、长期可用；地址直观。
- 缺点：`gitee.com/mirrors/*` 是**人工维护的快照仓库**，新 tag 偶尔滞后几天。
  本项目锁定的 `n9.0` 若镜像尚未同步，clone 会失败——脚本检测到失败后
  **自动回退 github** 重试，因此最坏情况只是慢，不会断。
- 适用：网络能通 github 但较慢、希望大部分流量走国内的环境。

### 4.2 统一前缀镜像（`GIT_MIRROR_PREFIX`，实时代理）

```bash
export GIT_MIRROR_PREFIX=https://gitclone.com
# 或
export GIT_MIRROR_PREFIX=https://mirror.ghproxy.com
```

转换规则（去掉原 URL 的 scheme 再拼接，兼容 gitclone/ghproxy 两类服务）：

```
https://github.com/FFmpeg/FFmpeg.git
  + GIT_MIRROR_PREFIX=https://gitclone.com
  => https://gitclone.com/github.com/FFmpeg/FFmpeg.git
```

- 优点：**实时与上游同步**，不会缺 tag；一个变量覆盖全部 6 个仓库。
- 缺点：公共代理站点（ghproxy 类）域名更换频繁，需要关注可用性；
  同样有"失败自动回退 github"兜底。
- 适用：github 完全不可达、且不想逐个配置仓库地址的环境。

两种模式**互斥**：单仓库覆盖优先于前缀镜像；前缀镜像对未显式覆盖的仓库生效。
版本号同样可覆盖：`FFMPEG_BRANCH` / `OPENSSL_BRANCH` / `CURL_BRANCH` /
`LIBXML2_BRANCH` / `NGHTTP2_BRANCH` / `DAV1D_BRANCH`。

## 5. 其它环节的镜像

### 5.1 Android gradle 依赖

```bash
export USE_CHINA_MIRROR=true
./gradlew assembleRelease ...
```

生效位置（已按该开关改造）：
- `platform/Android/source/build.gradle`（buildscript + allprojects）
- `platform/Android/source/settings.gradle`（pluginManagement）

镜像列表（阿里云公共仓库）：
`maven.aliyun.com/repository/gradle-plugin`、`.../google`、`.../public`。
顺带修复：**jcenter() 已停止维护**，全部替换为 `google() + mavenCentral()`。

### 5.2 CI / 手工下载 NDK

`.github/workflows/Android*.yml` 已支持 `ANDROID_NDK_URL` 覆盖：

```bash
# 默认
ANDROID_NDK_URL=https://dl.google.com/android/repository/android-ndk-r25c-linux.zip
# npmmirror 二进制镜像（国内）
ANDROID_NDK_URL=https://registry.npmmirror.com/-/binary/android-ndk/android-ndk-r25c-linux.zip
```

本地手工下载同理：解压到 `${USER_HOME}/android-env/android-ndk-r25c` 即可，
`setup.env` 默认指向该路径（`export ANDROID_NDK=...` 可覆盖）。

### 5.3 macOS Homebrew

`check_brew()`（build_player.sh / build_external.sh）支持
`HOMEBREW_INSTALL_URL` 覆盖安装脚本地址；安装后建议配置阿里云 brew 源：

```bash
export HOMEBREW_BREW_GIT_REMOTE=https://mirrors.aliyun.com/homebrew/brew.git
export HOMEBREW_CORE_GIT_REMOTE=https://mirrors.aliyun.com/homebrew/homebrew-core.git
export HOMEBREW_BOTTLE_DOMAIN=https://mirrors.aliyun.com/homebrew/homebrew-bottles
```

### 5.4 x264 / fdk-aac（本地源码目录方式）

这两个库不走自动下载（由 `user_env.sh` 指向本地源码目录）。国内环境可手动克隆：

```bash
git clone --depth 1 https://gitee.com/mirrors/x264.git   external/x264
git clone --depth 1 https://gitee.com/mirrors/fdk-aac.git external/fdk-aac
```

然后在 `external/user_env.sh` 中指向它们（详见 `build_tools/README.md`）。

### 5.5 Flutter（如使用 plugin）

```bash
export PUB_HOSTED_URL=https://pub.flutter-io.cn
export FLUTTER_STORAGE_BASE_URL=https://storage.flutter-io.cn
```

## 6. 失败兜底与排障

- 所有 git 依赖：`clone_git_mirror()` 先试镜像、失败自动回退 github；
  日志会打印 `mirror clone failed (xxx), retry with yyy`。
- 想验证某个镜像是否可用（以 FFmpeg 为例）：

```bash
export FFMPEG_GIT=https://gitee.com/mirrors/ffmpeg.git
git ls-remote --tags "$FFMPEG_GIT" n9.0     # 输出 tag 哈希即镜像已同步
git ls-remote --tags "$FFMPEG_GIT"          # 查看镜像当前有哪些 tag
```

- gitee 镜像缺 tag 时：优先改用 `GIT_MIRROR_PREFIX` 代理模式（实时同步），
  或保持默认 github（脚本会自动回退）。
- 代理类镜像域名失效时：换一个前缀即可，无需改代码（如 `https://ghfast.top`、
  `https://gh-proxy.com` 等，形式相同：`<前缀>/github.com/owner/repo`）。
