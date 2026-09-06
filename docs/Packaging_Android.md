# Android 打包流程（minSdk 24 / Android 7.0+，FFmpeg 9.0）

> 全新环境从零开始请先看 [`docs/BUILD_GUIDE.md`](BUILD_GUIDE.md)
> （环境准备、CRLF/权限免疫、国内镜像、常见问题排查）。

## 1. 环境要求

| 组件 | 版本 |
|---|---|
| JDK | 17 |
| Android SDK | compileSdk 34（build-tools 34.x） |
| Android NDK | **r25c 推荐**（r25~r27 可用，脚本已 shim 新 NDK 布局） |
| CMake | 3.15+（AGP 自带或系统安装） |
| 主机 | Linux / macOS（建议 Ubuntu 22.04）；需安装 autoconf/automake/libtool/pkg-config |
| 换行符 | 脚本必须 LF（Windows 检出需清理，见 BUILD_GUIDE 第 1.3 节） |

## 2. 一键编译脚本（GitHub Actions 同款流程）

```bash
# 0) 准备 NDK
export ANDROID_NDK=~/android-env/android-ndk-r25c
export ANDROID_NDK_HOME=$ANDROID_NDK

# 1) 加载环境（定义 build_Android 等函数）
. setup.env

# 2)（可选）国内镜像
source external/china_mirror_env.sh     # gitee 镜像预设（详见 docs/ChinaMirrors.md）
export USE_CHINA_MIRROR=true            # gradle 走阿里云 Maven

# 3) 拉取并编译外部库（FFmpeg n9.0 等）
cd external
./build_external.sh Android
# 产物: external/install/ffmpeg/Android/{armeabi-v7a,arm64-v8a}/libffmpeg.so + include

# 4) 编译 SDK 与 Demo APK
cd ../platform/Android/source
export ANDROID_FULL_PACKAGE=true   # true: AAR 内打包 libffmpeg.so; false: 由宿主 APP 提供
./gradlew assembleDebug assembleRelease
```

> **调试 vs 发布符号**：默认（不定义 `BUILD_TYPE`）为发布构建——FFmpeg 用
> `--disable-debug`（无 `-g`、无 assert）+ 链接后 `llvm-strip --strip-unneeded`，
> 单个 ABI 的 `libffmpeg.so` 约 8~12MB。需要带符号调试时，
> `export BUILD_TYPE=Debug` 后再执行步骤 3：FFmpeg 改按 `--enable-debug=3`
> 编译（`-g3` + assert），且 `.so` 不做 strip，便于崩溃回溯。
> （播放器自身 native 代码的符号由 gradle `debugSymbolLevel` 控制，与本脚本无关。）

或一步到位（含产物收集到 `output/`）：

```bash
. setup.env
build_Android
```

产物：
- Demo APK：`paasApp/build/outputs/apk/{debug,release}/CicadaPlayerDemo_*.apk`
- SDK AAR：`premierlibrary/build/outputs/aar/premierlibrary-corePlayer-release.aar`
  （拷贝为 `CicadaPlayer-4.7.0-{part,full}.aar`，见 `premierlibrary/build.gradle` 的 copy 任务）

## 3. 手动步骤拆解

### 3.1 外部库（FFmpeg + 依赖）

`external/build_external.sh Android` 依次执行：

```
load_source（player_git_source_list.sh）
 ├─ clone FFmpeg n9.0 / openssl 3.0.17 / curl 8.14.1 / libxml2 2.14.5 /
 │  nghttp2 1.66.0（镜像→回退 github）
 ├─ 源码树 EOL 归一化（LF）+ 补丁 CRLF 免疫
 └─ git am 打补丁：libxml2 / openssl / curl（按版本+平台条件跳过，
    失败自动 abort，不中断构建）
build_tools/build_Android.sh → 对每个 ABI:
   build_static_lib（统一补齐 configure/autogen.sh 可执行位后逐库编译）
     libxml2 → boost* → cares* → openssl → nghttp2 → curl → fdk-aac* → x264* → dav1d* → ffmpeg
     * 可选：无源码目录则跳过（打印 not found 警告，非致命）
   link_shared_lib_Android → clang/lld 把全部 .a 合并为 libffmpeg.so
```

关键变量（`build_tools/AndroidConfig.sh`）：`ANDROID_API_LEVEL=24`（默认），NDK llvm clang
`--target=aarch64-linux-android24` / `armv7a-linux-androideabi24`。
OpenSSL 默认使用 **3.0.17 (LTS)**（原生支持新 NDK 布局）；如通过
`OPENSSL_BRANCH=OpenSSL_1_1_1g` 回退旧版，`build_openssl_3.sh` 会自动做
`platforms/` 目录 shim 与 `-gcc-toolchain` 清理。curl 默认 **8.14.1**
（7.68 无法与 OpenSSL 3 编译）。

### 3.2 native 层（premierlibrary）

`premierlibrary/build.gradle`（已升级）：

```groovy
ndkVersion '25.2.9519653'
defaultConfig {
    minSdkVersion 24            // Android 7.0+
    targetSdkVersion 34
    externalNativeBuild { cmake {
        arguments '-DANDROID_TOOLCHAIN=clang',
                  "-DFFMPEG_INSTALL_DIR_ANDROID=$rootPath/../../../external/install/ffmpeg/Android/",
                  '-DANDROID_STL=c++_static'
    } }
}
sourceSets.main.jniLibs.srcDirs = ['../../../../external/install/ffmpeg/Android']
```

`premierlibrary/CMakeLists.txt` → `add_subdirectory(mediaPlayer)` → `framework/*.cmake`
（`framework/Android.cmake`）。

### 3.3 签名与包体

- 签名：`source/signature/Cicada.keystore`（demo 用，正式发布替换）
- `ANDROID_FULL_PACKAGE=false` 时 AAR 不含 `libffmpeg.so`（宿主 App 自行放入 jniLibs）

## 4. minSdk 24 升级点（本仓库已全部完成）

| 文件 | 变更 |
|---|---|
| 6 个 `build.gradle`（paasApp/premierlibrary/Exo/zxing/Flutter×2） | `minSdkVersion 18/14 → 24` |
| `framework/application.cmake` | `android-18/gcc/gnustl_static → android-24/clang/c++_static` |
| `build_tools/AndroidConfig.sh` | NDK r14/21 平台 → NDK r25 clang + API 24 |
| `build_tools/common_build.sh` | `${CROSS_COMPILE}-gcc` 链接 → clang/lld |
| `build_tools/boost/user-config-Android.jam`、`build_boost.sh` | gcc-4.9/gnustl → clang/libc++ |
| `build_tools/build_dav1d.sh`、`external/build_external.sh`、`setup.env` | gcc 工具链路径 → llvm 工具链 |
| `.github/workflows/Android*.yml` | NDK r14b → r25c，ubuntu-22.04，JDK 17 |

## 5. 常见问题

- **`avresample` 配置失败**：确认使用新 `ffmpeg_commands.sh`（已移除该选项）。
- **NDK r25 以下报 clang 缺失**：FFmpeg 9 与播放器 API 24 要求 NDK r25+。
- **`$ANDROID_NDK_HOME=... is invalid` / `clang: unknown argument: '-gcc-toolchain'`
  （OpenSSL）**：仅回退到 1.1.1 时会出现，`build_openssl_3.sh` 已内置 shim 与清理；
  默认的 OpenSSL 3.0.17 原生支持新 NDK，不会遇到这两个问题。
- **`bash\r` / `patch does not apply` / `Permission denied`**：Windows 检出的
  CRLF 与权限问题，处理与免疫机制见 `BUILD_GUIDE.md` 第 1.3 节。
- **`autoreconf: command not found`**：`apt-get install autoconf automake libtool pkg-config`。
- **依赖下载慢**：`source external/china_mirror_env.sh` + `USE_CHINA_MIRROR=true`。
- **`libc++_shared.so` 冲突**：premierlibrary 使用 `c++_static`，宿主 App 无需额外处理。
- **ExoPlayer 旧版本**：`ExternPlayerExo` 仍为 2.9.6；如需现代 Exo/Media3 请自行升级该模块（不影响核心库）。
