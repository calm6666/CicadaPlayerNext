# HarmonyOS / OpenHarmony：SDK（HAR）+ Demo（API 12+）

本目录下有两个模块：

* **`cicadaplayer/` —— 对外发布的 SDK**，ohpm 包名 `@cicada/cicadaplayer`，产物是
  `cicadaplayer.har`。自研内核（framework + mediaPlayer，含 OH_AVCodec 硬解、OHAudio
  渲染、DRM Kit、DASH/HLS/对象清单播放）编成 `libcicadaplayer.so`，再加上 ArkTS API
  （`CicadaPlayer` / `CicadaTypes`）与开箱即用组件（`CicadaVideo`、控制条、手势层、
  轨道面板、选项面板、提示浮层）。别的工程只要依赖这个 HAR 即可，接入方式见
  `docs/Packaging_HarmonyOS.md` 第 6 节。
* **`entry/` —— Demo（HAP）**，ArkTS 版的 Android demo（`paasApp` + zxing 扫描）。
  它自己**不再带 native**：播放相关全部通过 `@cicada/cicadaplayer` 使用，用来证明
  这个包在"只写 UI 的工程"里可以直接用。

## Build

DevEco Studio bundles everything needed (SDK, hvigor, node, ohpm); only the
environment variables below have to be set. `OHOS_SDK` is **not** required.

```powershell
$D = 'C:\Program Files\Huawei\DevEco Studio'
$env:DEVECO_SDK_HOME = "$D\sdk"
$env:NODE_HOME       = "$D\tools\node"
$env:PATH            = "$D\tools\node;$D\tools\ohpm\bin;$env:PATH"

Set-Location <repo>\platform\HarmonyOS
& "$D\tools\ohpm\bin\ohpm.bat" install --all          # 链接 file: 依赖（纯本地，离线可用）
& "$D\tools\hvigor\bin\hvigorw.bat" assembleHar --mode module -p module=cicadaplayer@default -p product=default --no-daemon
& "$D\tools\hvigor\bin\hvigorw.bat" assembleHap --mode module -p product=default --no-daemon
```

Outputs:
`cicadaplayer/build/default/outputs/default/cicadaplayer.har`（含 `libs/<abi>/` 下的
`libcicadaplayer.so` + `libffmpeg.so` + `libc++_shared.so`）与
`entry/build/default/outputs/default/entry-default-unsigned.hap`
（~37 MB，两个 ABI：arm64-v8a / x86_64）。

Verified against DevEco Studio 6.0.2 / bundled SDK **API 22 (HarmonyOS 6.0.2)** /
hvigor **6.22.3**. `build-profile.json5` keeps `compatibleSdkVersion 5.0.0(12)`.

`external/install/ffmpeg/OHOS/<abi>/` must be prebuilt
(see `docs/Packaging_HarmonyOS.md`); the ABIs built here are `arm64-v8a` and
`x86_64`.

## Layout

```
cicadaplayer/                     SDK 模块（HAR）
  Index.ets                       对外唯一入口（re-export 全部 API 与组件）
  oh-package.json5                @cicada/cicadaplayer
  src/main/cpp/
    CMakeLists.txt                内核 + NAPI 桥 → libcicadaplayer.so
    napi_init.cpp                 模块注册 + 导出符号表（70 个函数）
    napi_player.h/.cpp            注册表、参数编组、控制/配置 API
    napi_events.cpp               27 个 playerListener_t 回调 → 一条 threadsafe fn
    types/libcicadaplayer/        index.d.ts（native 模块类型声明）
  src/main/ets/
    api/CicadaTypes.ets           枚举 / 结构 / 事件名 / 监听器
    api/CicadaPlayer.ets          播放器封装
    component/CicadaVideo.ets     极简视图（XComponent + 生命周期 + 播放器装配）
    component/theme/PlayerTheme.ets  与 Compose 安卓版 1:1 的主题（颜色/尺寸/字号/圆角）
    component/compose/            播放器 UI（逐项对齐 Compose 安卓版）
      CicadaComposePlayer.ets     总装：画面 + 控制层 + 面板 + 手势 + 弹幕 + 浮层
      PlayerControls.ets          顶部栏 / 底部栏 / 进度条 / 细进度条 / seek 预览 / 弹幕输入条
      PlayerIcons.ets             图标与图标按钮（SVG path 复刻）
      LottieIcon.ets              播放暂停 Lottie 按钮、进度条 dot
      PlayerPanels.ets            清晰度 / 倍速 / 字幕 / 弹幕 / 音量 / 信息 六个浮层面板
      PlayerStats.ets             信息统计与清晰度、字幕列表的派生
      PlayerDanmakuLayer.ets      弹幕层（轨道引擎 + 滚动）
      PlayerGestureLayer.ets      亮度 / 音量 / 横滑 seek / 双击 / 长按倍速
    util/Strings.ets              UI 文案
    util/TimeFormater.ets         时间/速率格式化
  src/main/resources/rawfile/
    lottie-icon/                  play-to-pause / pause-to-play / Thumb 三个动画 JSON（与 Compose 源**字节一致**）
    icons/                        6 个 SVG 图标（pathData 与 Android vector 一致）
entry/                            Demo 模块（HAP，无 native）
  src/main/ets/
    model/AppTypes.ets            本 App 自己的类型（sourceList、路由参数）
    pages/                        SourceChoosePage, SourceChooseListPage,
                                  SourceInputUrlPage, SettingPage, MultiPlayerPage,
                                  SnapShotPage, AvPlayerPage, Index（播放页）
    view/                         TitleBar, MultiPlayerItem
    util/                          Settings, SourceListParser, LocalVideoSource,
                                  NetWatchdog, SnapshotSaver
  src/main/resources/rawfile/     sourceList.json, test.ass
```

### Native event bridge

Player callbacks run on native player threads, so they must not touch NAPI
directly. Each player owns one `napi_threadsafe_function`; every callback posts a
small payload and ArkTS receives a uniform signature:

```ts
(event: string, a: number, b: number, text: string) => void
('snapshot', width, height, rgba: ArrayBuffer) => void
('mediaInfo', count, 0, json: string) => void
```

`CicadaPlayer.ets` dispatches those onto the optional `CicadaPlayerListener`
callbacks the pages implement。

## Feature parity with the Android demo

| Android | HarmonyOS | Notes |
|---|---|---|
| SourceChooseActivity | `SourceChoosePage` | group list from `rawfile/sourceList.json` |
| SourceChooseListActivity | `SourceChooseListPage` | selection + multi-instance handoff |
| SourceInputUrlActivity | `SourceInputUrlPage` | URL / DASH / HLS / normal / manifest JSON |
| CicadaPlayerActivity | `Index` | control bar, gestures, panels, overlays |
| ControlView | `PlayerControlBar` | |
| GestureView / GestureControl | `PlayerGestureLayer` | |
| PlayerTrackFragment | `PlayerTrackPanel` | bitrate / audio / external subtitle / subtitle |
| PlayerOperationFragment + PlayerConfigFragment + PlayerCacheConfigFragment | `PlayerOptionPanel` | three tabs merged (ArkUI has no FragmentManager) |
| TipsView (loading/error/replay/netchange) | `PlayerTipsOverlay` | |
| SettingActivity | `SettingPage` | |
| SnapShotActivity | `SnapShotPage` | 5 s auto-close |
| MultiPlayerActivity | `MultiPlayerPage` | |
| NetWatchdog | `util/NetWatchdog.ets` | |
| ExoPlayerActivity (ExoPlayer kernel) | **deleted** | no HarmonyOS counterpart |
| system-player kernel (Android `MediaPlayer`) | `AvPlayerPage` | built on HarmonyOS AVPlayer |
| zxing CaptureActivity | `@kit.ScanKit` | |
| premierlibrary JNI | `napi_*` in the SDK module | 27 events + 70 functions |

## Deliberate deviations

* **Horizontal-swipe seek is enabled.** Android's implementation exists but its
  show/commit calls are commented out, so the gesture does nothing there. Set
  `ENABLE_HORIZONTAL_SEEK = false` in `PlayerGestureLayer.ets` for strict parity.
  The `SeekDialog` divisor table is reproduced exactly.
* **Gesture indicator uses text glyphs** (☀ / 🔊 / 🔇 / ⏩ / ⏪) instead of the
  Android `cicada_brightness` / `cicada_volume_*` / `cicada_seek_*` mipmaps,
  which have not been re-authored for HarmonyOS. The same applies to the ~22
  mipmap backgrounds the Android layouts reference.
* **Source list is read straight from `rawfile`.** Android re-extracts
  `sourceList.json` to external storage on every open (deleting any on-device
  copy) and needs `READ/WRITE_EXTERNAL_STORAGE`; neither is required here.
* **Local video is not copied.** `DocumentViewPicker` returns a document URI
  (`file://docs/...`), which FFmpeg cannot open — passing it straight to
  `CicadaSetDataSourceWithUrl` fails in PREPARINIT with ENOENT
  (`0x20080020`). `util/LocalVideoSource.ets` instead resolves an openable path
  without copying: it first asks `fileUri.FileUri(uri).path` for a real path and
  verifies it with `accessSync`, and failing that holds the descriptor open and
  returns `/proc/self/fd/<fd>` — the same trick the Android demo uses, which
  works because OHOS is Linux-based. The Cicada kernel needs a path; the AVPlayer
  kernel gets the URI and opens it itself into `fdSrc`, AVPlayer's supported
  no-copy route. Copying into the cache (Android's last-resort fallback) is
  deliberately avoided so large videos are not duplicated.
* **Snapshots** are written to the app cache directory (no permission) rather
  than `/sdcard`.
* **Brightness gestures** write `window.setWindowBrightness()`. HarmonyOS has no
  read-back and no "system default" sentinel, so the page owns the value and
  samples it at gesture start, like Android samples
  `window.attributes.screenBrightness`.
* **Multi-instance** gives each row its own player for its whole lifetime;
  Android keys players by URL in a `HashMap` and rebinds a recycled
  `TextureView`.
* **Config fields start empty**, where "empty" means "leave unchanged". Android
  prefills every field from `getConfig()` and overwrites with the widget default.
* **Scan result** only fills the URL field and is never auto-played, matching
  Android. The `REQ_CODE = 156` / `"qr_scan_result"` Intent contract has no ArkTS
  equivalent: ScanKit supplies its own viewfinder and camera-permission flow.

## Not portable

* **ExoPlayer is deleted.** It is Android-only and has no HarmonyOS counterpart,
  so neither the kernel nor its activity is ported. Android's `MediaPlayer`
  (system player) slot is filled by **AVPlayer** — HarmonyOS's own system
  player — implemented in `pages/AvPlayerPage.ets`. The settings kernel switch
  therefore offers CicadaPlayer / AVPlayer and the URL page's second button
  (Android's "exoPlayer") now starts AVPlayer.
  `CicadaPlayerFactory.addBlackDevice()` similarly has no C API, so the blacklist
  action only disables the hardware decoder locally.
* `SurfaceView`/`TextureView` + `player.setSurface`/`redraw` semantics →
  `XComponent` + `CicadaSetView`.
* Video tunnel rendering, `MediaCodecList` codec enumeration, `/proc` and sysfs
  reads for the CPU/GPU perf overlay.
* The ASS subtitle renderer (`AssSubtitleView`), thumbnail generation and
  `Bitmap` → PNG (`ImagePacker` is used instead).

## Known gaps

1. **Play-and-cache is now wired on the `ICicadaPlayer` / C API path.**
   `CacheConfig` used to live only on the `MediaPlayer` C++ facade; the C API
   handle (`SuperMediaPlayer`) had no cache entry point at all. It now does:
   `ICicadaPlayer::SetCacheConfig` / `GetCachePathByURL` (appended at the end of
   the vtable with empty default bodies, so no other platform changed a line),
   implemented in `SuperMediaPlayer` on top of the very same
   `CacheManager` + `ICacheDataSource` machinery the facade uses
   (`SuperMediaPlayerCacheDataSource`; the manager is held by `shared_ptr` so a
   player thread that took a snapshot can never touch a freed manager), plus
   `CicadaSetCacheConfig` / `CicadaGetCachePath` in the C API, the NAPI functions
   `setCacheConfig` / `getCachePath` (JSON contract, field names identical to
   `CacheConfig`'s members) and the ArkTS `setCacheConfig(config)` /
   `getCachePath(url)` with `onCacheSuccess` / `onCacheError` events
   (`MEDIA_PLAYER_EVENT_CACHE_SUCCESS` = 10, `_CACHE_ERROR` = 11).
   Boundary: the two manifest (object-mode) `SetDataSource` overloads do **not**
   take part — the final URL lives inside the manifest, so this layer cannot match
   a cache entry to it.
2. **DRM licence callback: exposed, with a thread contract.** `setDrmCallback`
   is now on the SDK. The kernel asks for the licence **synchronously on a player
   thread**, so the callback must return immediately (`ArrayBuffer` or base64
   text) and must not call the player's synchronous APIs, or the two threads wait
   for each other. The blocking handshake is a threadsafe function plus a
   condition variable with two deterministic exits — the JS callback returning,
   or the JS environment being torn down (pending requests are then marked
   aborted). No timeout fallback is involved.
3. **Audio AAC ADTS: fixed.** `OhosAVCodecDecoder` now sets
   `OH_MD_KEY_AAC_IS_ADTS` and, for raw AAC, passes the `AudioSpecificConfig` as
   `OH_MD_KEY_CODEC_CONFIG`, using the same rule as the Android side
   (`mediaCodecDecoder.cpp`): no extradata means in-band ADTS, extradata means
   raw AAC and ADTS must be explicitly 0.
4. **Audio clock: now the device's own position.** `OhosAudioRender::getPosition()`
   reports the frames actually presented by the audio service (API 15
   `OH_AudioRenderer_GetAudioTimestampInfo`, resolved at runtime, falling back to
   the API 10 `OH_AudioRenderer_GetTimestamp`) minus the baseline pinned at the
   last flush — i.e. microseconds since flush, `INT64_MIN` when unavailable,
   never negative and never a write-ahead byte count. `flush()` no longer calls
   into the device while holding the queue lock: `OH_AudioRenderer_Flush` waits
   for the in-flight write callback, which needs that same lock, so the old order
   was a guaranteed self-deadlock on every seek.
5. **Software-decoded video now has an on-screen path** (was: `DummyVideoRender`
   dropped the frames). The framework's own `glRender` — the same renderer the
   Android software path uses — is compiled in for OHOS
   (`framework/render/video/glRender/platform/ohos/{ohos_native_window,ohos_egl_context}`,
   `ENABLE_GLRENDER=ON`, links `libEGL.so`/`libGLESv3.so`), so scaling, rotation,
   mirror and the colour-vision matrix all go through the same shaders as Android.
   Hardware decode is deliberately **not** moved onto GLRender: the platform
   reports `videoRenderFactory::preferDirectSurfaceForHardwareDecode() == true`,
   so a hardware stream still carries `FLAG_DUMMY` and keeps today's zero-copy
   `OH_AVCodec` surface-direct path; only software frames (and the existing
   "hardware failed ⇒ software" fallback) land on GLRender. Untestable here: the
   EGL window-surface creation and buffer geometry on a real device.
6. **Compile-verified only.** There is no HarmonyOS device or emulator available
   here, so nothing has been run: gestures, window brightness, orientation
   switching, hardware decoding, audio output, DRM and ScanKit all compile and
   link but are unverified at runtime.
7. **C API coverage is 67 / 79.** The 12 unbound entry points are all C++ callback
   or object injections that NAPI cannot express and that no application calls
   (raw-frame render callbacks, a custom data source, platform-internal view/url/
   clock callbacks, `ErrorConverter`/component-factory injection, the struct
   variant of the manifest setter, and the `CicadaFree` alias of
   `CicadaFreeString`) — the full list is in
   `docs/Packaging_HarmonyOS.md` §6.5.

## Framework note: never re-set the listener on a running player

`PlayerNotifier::setListener()` calls `afThread::pause()`, which blocks until the
notifier loop comes back around the top of its `while` loop. That loop sits inside
`PlayerNotifier::post_loop()`, which waits on

```cpp
mCondition.wait(uMutex, [this]() { return !mRunning || !mEventQueue.empty(); });
```

— there is no pause flag and no timeout. On a **running but idle** player the loop
never comes back around, so `setListener()` blocks the calling thread forever; on
the UI thread that is an immediate `APP_INPUT_BLOCK` freeze (observed as a 5 s ANR
with the main thread inside `afThread::pause()`).

The first call from `create()` is safe only because the notifier thread has not
been started yet, so `afThread::pause()` sees a non-`RUNNING` status and returns at
once. The NAPI bridge therefore **never calls `CicadaSetListener` again**:
`detachListener()` only closes the threadsafe function, and `Release()` destroys
the native player first (`CicadaReleasePlayer` stops and joins the notifier, whose
destructor sets `mRunning = false`), then closes the JS side. Any consumer that
re-sets the listener on a live player — the Android JNI included, if it ever grows
such a call — will hit the same deadlock.
