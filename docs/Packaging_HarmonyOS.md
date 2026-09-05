# HarmonyOS / OpenHarmony 打包流程（API 12+，硬件加速）

## 1. 环境要求

| 组件 | 版本 |
|---|---|
| HarmonyOS NEXT SDK / OpenHarmony SDK | **5.0.0(12) 及以上**（`OH_AVBuffer`/`OH_NativeWindow_CreateNativeWindowFromSurfaceId` 为 API 12+） |
| DevEco Studio | 5.0+（含 hvigor）或命令行工具 `hvigorw` |
| CMake | 3.15+（SDK 自带 build-tools 亦可） |
| 主机 | Linux / macOS / Windows |

SDK 目录结构（关键部分）：

```
$OHOS_SDK/
├── <linux|darwin|windows>/native/
│   ├── build/cmake/ohos.toolchain.cmake
│   ├── llvm/                          # clang（OHOS target）
│   └── sysroot/usr/
│       ├── include/                   # multimedia/, ohaudio/, native_window/…
│       └── lib/<abi>/                 # libnative_*.so 桩库 + libc++_shared.so
```

## 2. 第一步：交叉编译外部库（FFmpeg 9.0）

```bash
export OHOS_SDK=/path/to/ohos-sdk          # 顶层 SDK 目录（含 native/llvm）
export OHOS_ARCHS="arm64-v8a x86_64"       # 可选：armeabi-v7a
. setup.env
cd external
./build_external.sh OHOS
```

或者使用一键入口（`build_player.sh` 中的 `build_OHOS()`，同时会尝试用 hvigorw
构建 Demo hap 并把产物收集到 `output/`）：

```bash
export OHOS_SDK=/path/to/ohos-sdk
. setup.env
build_OHOS
```

内部流程：
1. `build_tools/OHOSConfig.sh` 初始化：`clang --target=aarch64-linux-ohos --sysroot=...`
2. `ffmpeg_cross_compile_set_OHOS`：`--target-os=linux --arch=aarch64 --disable-symver`（+ 可选 `--disable-asm` 首启排障）
3. 每个 ABI：`build_static_lib OHOS <abi>` → `link_shared_lib_OHOS`（clang/lld 合并为 `libalivcffmpeg.so`）

产物：
```
external/install/ffmpeg/OHOS/<abi>/libalivcffmpeg.so
external/install/ffmpeg/OHOS/<abi>/include/   (FFmpeg 头文件)
```

## 3. 第二步：编译原生模块（hvigor）

`platform/HarmonyOS/` 已提供完整 Demo（ArkTS XComponent + NAPI + 全量 native 播放器）：

```bash
cd platform/HarmonyOS
hvigorw assembleHap --mode module -p product=default
```

构建链：
1. `entry/build-profile.json5` → `externalNativeOptions.path = ./src/main/cpp/CMakeLists.txt`
   `abiFilters: ["arm64-v8a","x86_64"]`
2. hvigor 用 `$OHOS_SDK/<os>/native/build/cmake/ohos.toolchain.cmake` 调用 CMake
   （`-DOHOS_ARCH=<abi> -DOHOS_PLATFORM=OHOS -DOHOS_STL=c++_shared`）
3. `entry/src/main/cpp/CMakeLists.txt`：
   - `add_subdirectory(<repo>/mediaPlayer)` → 构建 framework（`framework/HarmonyOS.cmake`）+
     mediaPlayer 全量代码；
   - `add_library(entry SHARED napi_init.cpp)` 链接 `media_player`、
     `libalivcffmpeg.so` 与 NDK 多媒体桩库
     （`libnative_window.so`、`libnative_media_codecbase.so`、`libnative_media_vdec.so`、
     `libnative_media_audiocodec.so`、`libnative_media_core.so`、`libohaudio.so`、`libnative_drm.so`）。
4. 产物 `libentry.so`（+ `libc++_shared.so`）被打进 `.hap` 的 `libs/<abi>/`。

产物：`entry/build/default/outputs/default/entry-default-signed.hap`

## 4. 硬件加速通路（本仓库实现）

| 能力 | 实现 | 关键 API |
|---|---|---|
| 视频硬解 | `OhosAVCodecDecoder`（surface mode 零拷贝） | `OH_AVCapability_GetHardwareDecoderName` → `OH_AVCodec_CreateByName` → `OH_AVCodec_SetSurface` → `RenderOutputBuffer`；buffer mode（NV12）兜底 |
| 音频输出 | `OhosAudioRender`（回调拉流） | `OH_AudioStreamBuilder`/`OH_AudioRenderer_OnWriteData` |
| 上屏 | ArkTS `XComponentController.getXComponentSurfaceId()` → NAPI → `OH_NativeWindow_CreateNativeWindowFromSurfaceId` → `CicadaSetView` | `libnative_window.so` |
| DRM | `OhosDrmHandler`（DRM Kit） | `OH_MediaKeySystem_Create(uuid)` → `OH_MediaKeySession_Create` → `GenerateMediaKeyRequest` → license 回调 → `ProcessMediaKeyResponse` → `OH_AVCodec_SetMediakeySessionConfig` |
| 对象播放 | NAPI `setDataSourceManifest(id, json)` | 见 `docs/ObjectManifestPlayback.md` |

## 5. 注意事项

- **Widevine L3 为华为商用扩展**（仅 HarmonyOS NEXT 商用设备，需设备支持）；
  开源 OpenHarmony 自带 ClearKey 插件（UUID `e2719d58-…`）。
  用 `OH_MediaKeySystem_GetMaxContentProtectionLevel()` 探测能力。
- FFmpeg 上游**没有** OHOS hwaccel：本方案采用 FFmpeg 解封装 + OH_AVCodec 硬解直连，
  与社区 `ohos_ijkplayer` 同一模式。
- `OHOS_STL=c++_shared` 时 `libc++_shared.so` 由 hvigor 自动打包；对外 `.har` 需携带 `libs/<abi>/*.so`。
- 首启排障可用 `export OHOS_DISABLE_ASM=TRUE` 关掉 FFmpeg 手写汇编。
- Demo 中 `libalivcffmpeg.so` 路径由 `FFMPEG_INSTALL_DIR_OHOS` 覆盖（默认
  `<repo>/external/install/ffmpeg/OHOS/<abi>/`）。
