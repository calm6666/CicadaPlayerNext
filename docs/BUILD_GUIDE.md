# CicadaPlayerNext 构建指南（FFmpeg 9.0 / Android 7.0+ / HarmonyOS）

> 本文档汇总了升级 FFmpeg 9.0、HarmonyOS 硬件加速、对象清单播放之后的全新构建流程，
> 包含环境准备、国内镜像、各平台打包命令与常见问题排查。各平台打包细节见
> [`docs/Packaging_*.md`](.)，架构见 [`docs/ARCHITECTURE.md`](ARCHITECTURE.md)。

---

## 1. 环境准备

### 1.1 主机基础依赖

**Linux / WSL（Ubuntu 22.04）**：

```bash
sudo apt-get update
sudo apt-get install -y git make perl gcc g++ wget unzip zip tree \
    autoconf automake libtool pkg-config   # curl/nghttp2/libxml2 需要 autoreconf 生成 configure
sudo apt-get install -y cmake yasm         # cmake >= 3.14；yasm 供汇编编译
# dav1d 可选（默认不启用）：
sudo apt-get install -y meson ninja-build nasm
```

**macOS**：

```bash
brew install cmake yasm automake libtool pkg-config
# dav1d 可选：brew install meson ninja nasm
# 国内 Homebrew 加速见 docs/ChinaMirrors.md
```

### 1.2 Android 工具链

| 组件 | 版本要求 | 说明 |
|---|---|---|
| JDK | 17 | gradle 需要 |
| Android SDK | compileSdk 34 | `platforms;android-34` + `build-tools;34.x` |
| Android NDK | **r25c 推荐**（r25~r27 均可用） | FFmpeg 9.0 需要 C11 工具链（clang） |
| CMake | 3.15+ | 系统或 SDK 自带均可 |

NDK 下载（国内用 npmmirror 镜像）：

```bash
# 官方
wget https://dl.google.com/android/repository/android-ndk-r25c-linux.zip
# 国内镜像
wget https://registry.npmmirror.com/-/binary/android-ndk/android-ndk-r25c-linux.zip
unzip -q android-ndk-r25c-linux.zip
```

> **为什么推荐 r25c？** OpenSSL 1.1.1g 的 Android 配置按旧目录结构校验 NDK，
> 新 NDK 已移除 `platforms/` 目录。构建脚本已内置兼容 shim
> （`build_openssl_111.sh` 会自动创建符号链接指向统一 sysroot），
> 因此 r25/r26/r27 都能用；r25c 是全 CI 验证过的基线。

### 1.3 换行符与文件权限（WSL / Windows 检出必看）

从 Windows 检出/拷贝的脚本是 CRLF 换行、且丢失可执行位，会导致
`bash\r: No such file or directory`、`patch does not apply`、`Permission denied`
等一系列问题。**一次性根治：**

```bash
# A) 若为 git 仓库：用 .gitattributes 强制 LF（本仓库已提供），并关闭 autocrlf
git config core.autocrlf false
git add --renormalize . && git commit -m "normalize EOL"

# B) 若为手工拷贝：清行尾 + 补权限
cd /home/CicadaPlayerNext   # 仓库根目录
find . -type f \( -name "*.sh" -o -name "*.env" -o -name "*.patch" -o -name "*.pl" \
  -o -name "*.jam" -o -name "*.cmake" -o -name "*.json5" -o -name "*.yml" \
  -o -name "CMakeLists.txt" \) -exec sed -i 's/\r$//' {} +
chmod +x *.sh build_tools/*.sh external/*.sh

# C) 验证
bash -n setup.env && bash -n build_player.sh && bash -n external/build_external.sh && echo OK
```

> 注：`build_external.sh` 与各 `build_*.sh` 已做双重免疫——打补丁时自动转 LF、
> 子脚本用 `bash xxx.sh` 显式执行、源码树自动归一化行尾并补齐 `configure`/`autogen.sh`
> 的可执行位。即使再遇到 Windows 检出也能自愈，但建议仍按上述步骤处理一遍。

---

## 2. 依赖源码与国内镜像

依赖源码（FFmpeg n9.0 / OpenSSL 1.1.1g / curl / libxml2 / nghttp2 / dav1d）由
`external/player_git_source_list.sh` 统一 clone，规则：**构建时传入镜像变量则用镜像，
不传则默认 github；镜像失败自动回退 github**。

```bash
# 方式 A：gitee 快照镜像（一键预设）
source external/china_mirror_env.sh

# 方式 B：实时代理前缀（一个变量覆盖全部仓库）
export GIT_MIRROR_PREFIX=https://gitclone.com

# 方式 C：单仓库覆盖
export FFMPEG_GIT=https://gitee.com/mirrors/ffmpeg.git
```

gradle 依赖镜像（可选）：

```bash
export USE_CHINA_MIRROR=true    # 阿里云 Maven 镜像
```

完整分析（依赖清单、镜像对比、失败回退机制）见 [`docs/ChinaMirrors.md`](ChinaMirrors.md)。

---

## 3. Android 一键构建（minSdk 24 / Android 7.0+）

```bash
# 0) 环境
export ANDROID_NDK=/home/android-ndk-r25c          # 指向 NDK 根目录
export ANDROID_NDK_HOME=$ANDROID_NDK
export ANDROID_SDK_HOME=/home/android-sdk          # Android SDK（gradle 需要）

# 1) 加载环境（定义 build_Android / build_OHOS / build_iOS / build_mac 等函数）
cd /home/CicadaPlayerNext
. setup.env

# 2)（可选）国内镜像
source external/china_mirror_env.sh
export USE_CHINA_MIRROR=true

# 3) 编译外部库（FFmpeg 9.0 等）
cd external
./build_external.sh Android
# 产物：external/install/ffmpeg/Android/{armeabi-v7a,arm64-v8a}/libalivcffmpeg.so + include/

# 4) 编译 SDK AAR 与 Demo APK
cd ../platform/Android/source
export ANDROID_FULL_PACKAGE=true   # true: AAR 内打包 libalivcffmpeg.so；false: 宿主 APP 提供
./gradlew assembleDebug assembleRelease
```

或使用一键入口（第 1-3 步 + 打包收产物到 `output/`）：

```bash
. setup.env
build_Android
```

产物：

```
platform/Android/source/paasApp/build/outputs/apk/{debug,release}/CicadaPlayerDemo_*.apk
platform/Android/source/premierlibrary/build/outputs/aar/premierlibrary-corePlayer-release.aar
（copy 任务重命名为 CicadaPlayer-4.7.0-{part,full}.aar）
```

### 3.1 外部库编译流程拆解

```
external/build_external.sh Android
 ├─ load_source: clone 6 个依赖（镜像→回退 github）
 │   ├─ 源码树 EOL 归一化（LF）+ 补丁 CRLF 免疫
 │   └─ git am 打补丁：libxml2 / openssl / curl（失败自动 abort，不中断）
 └─ build_tools/build_Android.sh
     └─ 对每个 ABI (armeabi-v7a, arm64-v8a):
         ├─ libxml2  → configure → make
         ├─ boost*   → 可选（无源码目录则跳过）
         ├─ cares*   → 可选
         ├─ openssl  → Configure android-arm(64)（自动 shim platforms/ 目录）
         ├─ nghttp2  → autoreconf → configure
         ├─ curl     → autoreconf → configure --with-ssl --with-nghttp2
         ├─ fdk-aac* → 可选
         ├─ x264*    → 可选
         ├─ dav1d*   → 可选（默认关闭）
         └─ ffmpeg   → configure（组件清单见 external/player_ffmpeg_config.sh）
                       → make → clang/lld 合并所有 .a → libalivcffmpeg.so
```

\* 标记项未配置源码目录时打印 `xxx source not found` 警告并跳过（**非致命**）。

---

## 4. 其它平台构建

| 平台 | 命令 | 产物 |
|---|---|---|
| HarmonyOS NEXT (API 12+) | `export OHOS_SDK=...` → `. setup.env` → `cd external && ./build_external.sh OHOS` → `build_OHOS`（含 Demo hap） | `external/install/ffmpeg/OHOS/<abi>/libalivcffmpeg.so` + `output/*.hap` |
| iOS | `cd external && ./build_external.sh iOS` + Xcode 工程 | 动态 framework（`build_tools/build_iOS.sh`） |
| macOS | `cd external && ./build_external.sh macOS` + cmake | `cmdline/cicadaPlayer` |
| Linux | `cd external && ./build_external.sh Linux` + cmake | `cmdline/cicadaPlayer` |
| Windows (MinGW) | `cd external && ./build_external.sh Windows` + cmake | `cicadaPlayer.exe` + `libalivcffmpeg.dll` |

详情：`docs/Packaging_HarmonyOS.md`、`docs/Packaging_iOS.md`、
`docs/Packaging_macOS_Linux_Windows.md`。

---

## 5. 常见问题排查

| 现象 | 原因 | 解决 |
|---|---|---|
| `bash\r: No such file or directory` | 脚本是 CRLF | 第 1.3 节 B 的 `sed -i 's/\r$//'` 清理 |
| `patch does not apply` / `does not match index` | `.patch` 或源码树 CRLF | 新版 `build_external.sh` 已免疫；手工修复见第 1.3 节 |
| `$ANDROID_NDK_HOME=... is invalid`（OpenSSL） | 仅 1.1.1 回退版本会校验旧 `platforms/` 目录；默认 3.0.15 原生支持新 NDK | 保持默认 3.0.15；回退 1.1.1 时脚本已内置 shim |
| `clang: unknown argument: '-gcc-toolchain'` + `no such file or directory: ...-4.9/prebuilt/...`（OpenSSL 编译期） | 仅 1.1.1 会传旧版 gcc-4.9 工具链路径；3.0.15 无此问题 | 保持默认 3.0.15；回退 1.1.1 时脚本已自动清理 |
| `ld: error: cannot open crtbegin_so.o / crtend_so.o`（OpenSSL 3.x 链接期） | 裸 clang + 不带 API 的 `-target` 找不到 Android 运行时目标文件 | 新版脚本：`no-legacy` 关掉模块链接 + 用 `armv7a-linux-androideabi24-clang` 包装器替换 CC 并去掉裸 `-target` |
| `./autogen.sh: Permission denied` | Windows 检出丢可执行位 | `chmod +x`（新版脚本自动补齐） |
| `autoreconf: command not found` | 缺 autotools | `apt-get install autoconf automake libtool pkg-config` |
| `boost/cares source not found` | `user_env.sh` 未配置该源码目录 | 非致命警告；需要时按 `build_tools/README.md` 配置 |
| gradle 依赖下载慢/失败 | 默认源访问慢（jcenter 已废弃） | `export USE_CHINA_MIRROR=true`（阿里云 Maven） |
| `git clone` 慢/超时 | github 不可达 | `source external/china_mirror_env.sh` 或 `GIT_MIRROR_PREFIX` |
| NDK 下载慢 | dl.google.com 慢 | 用 npmmirror 的 `ANDROID_NDK_URL`（CI）或手动下载 |
| 重复构建报 `destination path ... already exists` | 上次 clone 的源码还在 | 直接复用即可（版本一致时无害）；要重来就 `rm -rf external/external/<repo>` |

---

## 6. 对象清单播放快速验证（可选）

编译产物里已包含对象播放能力。桌面端命令行可直接验证：

```bash
# URL 模式
./cmdline/cicadaPlayer https://example.com/video.m3u8
# 对象模式（MediaManifest JSON，支持 AES-128 / Widevine / ClearKey DRM）
./cmdline/cicadaPlayer -m manifest.json
```

JSON 示例与 DRM 说明见 [`docs/ObjectManifestPlayback.md`](ObjectManifestPlayback.md)。

---

## 7. 文档索引

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — 架构图（mermaid）与模块说明
- [`FFmpeg9_Upgrade.md`](FFmpeg9_Upgrade.md) — FFmpeg 9.0 迁移清单
- [`ChinaMirrors.md`](ChinaMirrors.md) — 国内镜像配置分析
- [`Packaging_Android.md`](Packaging_Android.md) — Android 打包细节
- [`Packaging_HarmonyOS.md`](Packaging_HarmonyOS.md) — HarmonyOS 打包细节
- [`Packaging_iOS.md`](Packaging_iOS.md) / [`Packaging_macOS_Linux_Windows.md`](Packaging_macOS_Linux_Windows.md)
- [`ObjectManifestPlayback.md`](ObjectManifestPlayback.md) — 对象播放 + DRM
