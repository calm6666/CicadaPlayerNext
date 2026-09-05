# Android 打包流程（minSdk 24 / Android 7.0+，FFmpeg 9.0）

## 1. 环境要求

| 组件 | 版本 |
|---|---|
| JDK | 17 |
| Android SDK | compileSdk 34（build-tools 34.x） |
| Android NDK | **r25c**（或更高；FFmpeg 9 需要 C11 工具链，gcc/gnustl 已移除） |
| CMake | 3.15+（AGP 自带或系统安装） |
| 主机 | Linux / macOS（建议 Ubuntu 22.04） |

## 2. 一键编译脚本（GitHub Actions 同款流程）

```bash
# 0) 准备 NDK
export ANDROID_NDK=~/android-env/android-ndk-r25c
export ANDROID_NDK_HOME=$ANDROID_NDK

# 1) 拉取并编译外部库（FFmpeg n9.0 等）
. setup.env
cd external
./build_external.sh Android
# 产物: external/install/ffmpeg/Android/{armeabi-v7a,arm64-v8a}/libalivcffmpeg.so + include

# 2) 编译 SDK 与 Demo APK
cd ../platform/Android/source
export ANDROID_FULL_PACKAGE=true   # true: AAR 内打包 libalivcffmpeg.so; false: 由宿主 APP 提供
./gradlew assembleDebug assembleRelease
```

产物：
- Demo APK：`paasApp/build/outputs/apk/{debug,release}/CicadaPlayerDemo_*.apk`
- SDK AAR：`premierlibrary/build/outputs/aar/premierlibrary-corePlayer-release.aar`
  （拷贝为 `CicadaPlayer-4.7.0-{part,full}.aar`，见 `premierlibrary/build.gradle` 的 copy 任务）

## 3. 手动步骤拆解

### 3.1 外部库（FFmpeg + 依赖）

`external/build_external.sh Android` 依次执行：

```
player_git_source_list.sh   → clone FFmpeg n9.0（FFMPEG_BRANCH=n9.0，补丁默认关闭）
player_ffmpeg_config.sh     → FFmpeg 9.0 组件清单（解码器/解析器/bsf/协议/滤镜）
../build_tools/build_Android.sh → 对每个 ABI:
   build_static_lib (libxml2/boost/openssl/curl/fdk-aac/x264/dav1d/ffmpeg)
   link_shared_lib_Android      → clang/lld 把全部 .a 合并为 libalivcffmpeg.so
```

关键变量（`build_tools/AndroidConfig.sh`）：`ANDROID_API_LEVEL=24`（默认），NDK llvm clang
`--target=aarch64-linux-android24` / `armv7a-linux-androideabi24`。

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
- `ANDROID_FULL_PACKAGE=false` 时 AAR 不含 `libalivcffmpeg.so`（宿主 App 自行放入 jniLibs）

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
- **`libc++_shared.so` 冲突**：premierlibrary 使用 `c++_static`，宿主 App 无需额外处理。
- **ExoPlayer 旧版本**：`ExternPlayerExo` 仍为 2.9.6；如需现代 Exo/Media3 请自行升级该模块（不影响核心库）。
