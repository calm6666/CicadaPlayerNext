# HarmonyOS / OpenHarmony 打包流程（API 12+，硬件加速）

## 1. 环境要求

| 组件 | 版本 |
|---|---|
| HarmonyOS NEXT SDK / OpenHarmony SDK | **5.0.0(12) 及以上**（`OH_AVBuffer`/`OH_NativeWindow_CreateNativeWindowFromSurfaceId` 为 API 12+） |
| DevEco Studio | 5.0+（含 hvigor）或命令行工具 `hvigorw` |
| CMake | 3.15+（SDK 自带 build-tools 亦可） |
| nasm | 2.15+（**仅编译 x86_64 ABI 需要**：FFmpeg 的 x86 手写汇编用 nasm 语法；arm64/armeabi-v7a 不需要） |
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
2. `ffmpeg_cross_compile_set_OHOS`：`--target-os=linux --arch=aarch64 --disable-symver --disable-vulkan`
   （OHOS SDK 的 vulkan 头不完整——`vulkan.h` 引用缺失的 `vulkan_beta.h`，而播放器
   鸿蒙硬解走 OH_AVCodec 不用 FFmpeg vulkan，故直接禁用；+ 可选 `--disable-asm` 首启排障）
3. 每个 ABI：`build_static_lib OHOS <abi>` → `link_shared_lib_OHOS`（clang/lld 合并为 `libffmpeg.so`）

外部依赖链（`build_static_lib` 依序构建，全部已支持 OHOS 分支）：
- **libxml2 2.14.5**：自带新版 config.sub，原生识别 `*-ohos*` 三元组
  （2.9.9 的 2015 年 config.sub 会报 "Invalid configuration"）。
- **OpenSSL 3.0.17**（`build_openssl_3.sh`）：`linux-aarch64`/`linux-armv4`/
  `linux-x86_64` 目标 + OHOS SDK clang（`no-shared no-asm no-legacy`），
  Makefile 的 CC/AR/RANLIB 自动改写为 OHOS SDK llvm 工具链。
- **curl 8.14.1 / nghttp2 1.66.0**：`--host=<ohos 三元组>`；系统 automake 的
  config.sub 若不认识 ohos，`common_build.sh` 的 `patch_config_sub_for_ohos`
  会装一个 shim（config.sub 已支持时自动跳过，不做任何改动）。
- 可选库 ares/fdk-aac/x264/librtmp/dav1d/boost 同样带 OHOS 分支。
- 所有静态库均 `-fPIC`，与 FFmpeg 合并进 `libffmpeg.so`。

产物：
```
external/install/ffmpeg/OHOS/<abi>/libffmpeg.so
external/install/ffmpeg/OHOS/<abi>/include/   (FFmpeg 头文件)
```

## 3. 第二步：编译 SDK（HAR）与 Demo（hvigor）

工程分成两个模块：**可复用的 SDK（HAR）** + **只做 UI 的 Demo（HAP）**。

| 模块 | 类型 | 内容 |
|---|---|---|
| `platform/HarmonyOS/cicadaplayer` | **HAR（静态库，ohpm 包名 `@cicada/cicadaplayer`）** | `libcicadaplayer.so`（全量内核 + NAPI 桥）、ArkTS API（`CicadaPlayer` / `CicadaTypes`）、开箱即用组件（`CicadaVideo`、`PlayerControlBar`、`PlayerGestureLayer`、`PlayerTrackPanel`、`PlayerOptionPanel`、`PlayerTipsOverlay`）、native 模块类型声明 |
| `platform/HarmonyOS/entry` | HAP（Demo） | 页面与业务；播放相关一律通过 `@cicada/cicadaplayer` 使用，不再自带 native |

```bash
cd platform/HarmonyOS
ohpm install --all                       # 链接 file: 依赖（全部本地，离线可用）
hvigorw assembleHar --mode module -p module=cicadaplayer@default -p product=default
hvigorw assembleHap --mode module -p product=default
```

构建链：
1. `cicadaplayer/build-profile.json5` → `externalNativeOptions.path = ./src/main/cpp/CMakeLists.txt`，
   `abiFilters: ["arm64-v8a","x86_64"]`
2. hvigor 用 `$OHOS_SDK/<os>/native/build/cmake/ohos.toolchain.cmake` 调用 CMake
   （`-DOHOS_ARCH=<abi> -DOHOS_PLATFORM=OHOS -DOHOS_STL=c++_shared`）
3. `cicadaplayer/src/main/cpp/CMakeLists.txt`：
   - `add_subdirectory(<repo>/mediaPlayer)` → 构建 framework（`framework/HarmonyOS.cmake`）+
     mediaPlayer 全量代码；
   - `add_library(cicadaplayer SHARED napi_init.cpp napi_player.cpp napi_events.cpp)` 链接
     `media_player`、`libffmpeg.so` 与 NDK 桩库（`libace_napi.z.so`、`libnative_window.so`、
     `libnative_buffer.so`、`libnative_media_codecbase.so`、`libnative_media_vdec.so`、
     `libnative_media_acodec.so`、`libnative_media_core.so`、`libnative_vsync.so`、
     `libohaudio.so`、`libnative_drm.so`）。
4. 模块名必须是 `cicadaplayer`：它与 `napi_init.cpp` 里 `napi_module_register` 的 `nm_modname`
   一致，ArkTS 侧才能 `import cicada from 'libcicadaplayer.so'`。
5. 产物 `libcicadaplayer.so`（+ `libc++_shared.so`）随 HAR 打进 `libs/<abi>/`，HAR 被 entry
   依赖后再进 `.hap` 的 `libs/<abi>/`。

产物：
- `cicadaplayer/build/default/outputs/default/cicadaplayer.har`
- `entry/build/default/outputs/default/entry-default-signed.hap`

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
- **FFmpeg 上游有 OHOS 解码器，但本工程的 FFmpeg 构建没启用它**【2026-09-27 更正】：
  本仓库自带的 FFmpeg 源码树里 `libavcodec/ohcodec.c` / `ohdec.c` / `ohenc.c` 都在，
  而 `build_tools/**` 里 `ohcodec|ohdec` **零命中** ⇒ 现状是"有能力、未开启"。
  当前方案仍是 FFmpeg 解封装 + `OH_AVCodec` 硬解直连（与社区 `ohos_ijkplayer` 同一模式），
  因为本内核依赖的语义（选路/切档占位面/DRM/上屏取证）在 FFmpeg 的 ohdec 上没有对应通路。
  若将来要统一到 FFmpeg 解码栈，需要先启用对应开关并重新验证 `.so`。
- `OHOS_STL=c++_shared` 时 `libc++_shared.so` 由 hvigor 自动打包；对外 `.har` 需携带 `libs/<abi>/*.so`。
- 首启排障可用 `export OHOS_DISABLE_ASM=TRUE` 关掉 FFmpeg 手写汇编。
- Demo 中 `libffmpeg.so` 路径由 `FFMPEG_INSTALL_DIR_OHOS` 覆盖（默认
  `<repo>/external/install/ffmpeg/OHOS/<abi>/`）。

## 6. 把 SDK 接进别的 HarmonyOS 工程

### 6.1 三种接入方式

| 方式 | 做法 | 适用 |
|---|---|---|
| 源码依赖（开发期推荐） | 消费者 `oh-package.json5` 写 `"@cicada/cicadaplayer": "file:../CicadaPlayerNext/platform/HarmonyOS/cicadaplayer"`，再 `ohpm install` | 与内核源码同仓演进、可断点调试 |
| 本地 har 包 | `hvigorw assembleHar ...` 产出 `cicadaplayer.har`，拷到消费者工程后写 `"@cicada/cicadaplayer": "file:./libs/cicadaplayer.har"` | 交付二进制、不带源码 |
| 私仓发布 | `ohpm publish cicadaplayer.har`（先在 `oh-package.json5` 补 `author` / `license` / `repository`），消费者写版本号 | 多工程统一升级 |

前置：消费者工程 `compatibleSdkVersion ≥ 5.0.0(12)`（`OH_NativeWindow_CreateNativeWindowFromSurfaceId`
是 API 12+）。FFmpeg 由 SDK 打进 HAR 一起交付，消费者不需要再单独放 `libffmpeg.so`。

### 6.2 最小可用示例

```ts
import { CicadaPlayer, CicadaPlayerListener } from '@cicada/cicadaplayer';

@Entry
@Component
struct PlayerPage {
  private player: CicadaPlayer = new CicadaPlayer();
  private xc: XComponentController = new XComponentController();
  @State positionMs: number = 0;

  private readonly listener: CicadaPlayerListener = {
    onPrepared: () => this.player.start(),
    onFirstFrameShow: () => { /* 首帧已上屏 */ },
    onPositionUpdate: (positionMs: number) => { this.positionMs = positionMs; },
    onVideoSizeChanged: (width: number, height: number) => { /* 按需调整容器比例 */ },
    onError: (code: number, msg: string) => { /* 错误提示 */ }
  };

  aboutToDisappear(): void {
    this.player.release();
  }

  build() {
    Stack() {
      XComponent({ id: 'surface', type: XComponentType.SURFACE, controller: this.xc })
        .width('100%')
        .height('100%')
        .backgroundColor(Color.Black)
        .onLoad(() => {
          const surfaceId: string = this.xc.getXComponentSurfaceId();
          if (!this.player.setSurface(surfaceId)) {
            return;
          }
          this.player.create(this.listener);
          this.player.setDataSource('https://example.com/video.m3u8');
          this.player.prepare();
        })
    }
  }
}
```

也可以用组件版（组件内部已处理好 surface 与生命周期）：

```ts
import { CicadaVideo, CicadaVideoController } from '@cicada/cicadaplayer';

private controller: CicadaVideoController = new CicadaVideoController();
// ...
CicadaVideo({ controller: this.controller, listener: this.listener })
// 之后 this.controller.player.xxx() 就是完整的播放器接口
```

### 6.3 顺序与线程约束（必读）

1. **先拿 surface，再 setDataSource / prepare**：OHOS 走 surface 模式硬解直出，画面由解码器
   直接写进 XComponent 的原生窗口；surface 未绑定时起播没有画面。
2. **不要重复 setListener**：`PlayerNotifier::setListener()` 会 `afThread::pause()`，在"已运行且
   空闲"的播放器上会永久阻塞调用线程（UI 线程上表现为 APP_INPUT_BLOCK）。SDK 只在
   `create()` 时注册一次监听。
3. **DRM 回调是同步的**：内核在播放线程上等许可证响应，回调里不要再调用播放器的同步接口，
   否则两边互相等待。JS 环境拆除时在途请求会被标记为 aborted 放行（确定性出口，不用超时兜底）。
4. **`release()` 之后句柄失效**：再调用任何接口都是空操作（返回默认值）。

### 6.4 完整能力清单（ArkTS 侧）

- 生命周期：`create` / `prepare` / `start` / `pause` / `stop` / `reload` / `release`
- 画面：`setSurface` / `clearScreen` / `setScaleMode` / `setRotateMode` / `setMirrorMode` /
  `setVideoBackgroundColor` / `setColorMatrix` / `setFilterConfig` / `updateFilterConfig` /
  `setFilterInvalid`
- 传输：`seekTo` / `setSpeed` / `setVolume` / `setMute` / `setLoop` / `setAutoPlay`
- 进度：`getDuration` / `getCurrentPosition` / `getCurrentBufferedPosition` / `getMasterClockPts` /
  `getVideoResolution` / `getVideoRotation`
- 清晰度与轨道：`selectTrack`（含自动档）/ `getCurrentStreamIndex` / `getCurrentStreamInfo` /
  `isStreamSwitchInFlight` / `setDefaultBandWidth` / `getVideoCodecSupport` /
  `setVideoCodecSupport`
- 字幕：`addExtSubtitle` / `selectExtSubtitle` / `setStreamDelayTime`
- 解码：`enableHardwareDecoder` / `getDecoderType` / `isVideoDecoderHardware`
- 网络：`setTimeout` / `setDropBufferThreshold` / `setReferer` / `setUserAgent` /
  `addCustomHttpHeader` / `removeAllCustomHttpHeader`
- 截图：`snapshot`
- DRM：`setDrmCallback` / `setDataSourceWithManifest`
- 诊断：`setOption` / `getOption` / `getPropertyLong` / `getPropertyString` / `getPlayerName` /
  `getVideoRenderFps` / `getVideoDecodeFps` / `invokeComponent` / `getCurrentStreamMeta`
  （内核 `Stream_meta` 原样字段：profile / frame_size / sample_fmt / extradata 大小 /
  ptsTimeBase / interlaced…，`codec` 是内核 `AFCodecID` 的数值）
- 事件：27 个回调，含 `onVideoQualitySwitch`（STARTED / READY / FAILED / CANCELED）

### 6.5 尚未覆盖的能力（如实记录）

- **播放缓存（play-and-cache）**：缓存配置落在 C++ `MediaPlayer` 门面上，C API 句柄
  （`ICicadaPlayer` / `SuperMediaPlayer`）这条路径还没有 CacheConfig 入口，因此 HAR 暂不提供
  `setCacheConfig`；Demo 的缓存页保持原样（只记录缺口）。
- **软件解码上屏**：OHOS 侧 `videoRenderFactory` 对非 surface 路径返回 `DummyVideoRender`，
  即软解帧不出画（硬解直出不受影响）。**显示节拍这一半已经就位**：`VSyncFactory` 在鸿蒙返回
  `OHOSVSync`（系统 `OH_NativeVSync`，周期用 `OH_NativeVSync_GetPeriod` 读回来，取不到才退回
  `timedVSync`）；还差的只是渲染器本身 —— 经 `videoRenderFactory::setRenderCreator()` 注入一个
  基于 EGL/GLES 的渲染器（复用 `framework/render/video/glRender`），属独立一件事。
- **播放器级 ASS 字幕渲染**：内核给事件与文本，渲染由应用层负责。
