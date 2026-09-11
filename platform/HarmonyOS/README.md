# HarmonyOS / OpenHarmony demo application (API 12+)

ArkTS + NAPI port of the Android demo in `platform/Android` (`paasApp` + the
zxing scanner). The native player (framework + mediaPlayer, including OH_AVCodec
hardware decoding and the DRM Kit) is compiled into `libentry.so` by hvigor.

## Build

DevEco Studio bundles everything needed (SDK, hvigor, node, ohpm); only the
environment variables below have to be set. `OHOS_SDK` is **not** required.

```powershell
$D = 'C:\Program Files\Huawei\DevEco Studio'
$env:DEVECO_SDK_HOME = "$D\sdk"
$env:NODE_HOME       = "$D\tools\node"
$env:PATH            = "$D\tools\node;$D\tools\ohpm\bin;$env:PATH"

Set-Location <repo>\platform\HarmonyOS
& "$D\tools\hvigor\bin\hvigorw.bat" assembleHap --mode module -p product=default --no-daemon
```

Output: `entry/build/default/outputs/default/entry-default-unsigned.hap`
(~37 MB; `libentry.so` + `libffmpeg.so` + `libc++_shared.so` for arm64-v8a and
x86_64).

Verified against DevEco Studio 6.0.2 / bundled SDK **API 22 (HarmonyOS 6.0.2)** /
hvigor **6.22.3**. `build-profile.json5` keeps `compatibleSdkVersion 5.0.0(12)`.

`external/install/ffmpeg/OHOS/<abi>/` must be prebuilt
(see `docs/Packaging_HarmonyOS.md`); the ABIs built here are `arm64-v8a` and
`x86_64`.

## Layout

```
entry/src/main/cpp/
  napi_init.cpp      module registration + exported symbol table
  napi_player.h/.cpp registry, argument marshalling, ~60 control/config API
  napi_events.cpp    all 26 playerListener_t callbacks -> one threadsafe fn
  types/libentry/    index.d.ts (typed native module) + oh-package.json5
entry/src/main/ets/
  model/CicadaTypes.ets   enums/structs mirrored from native + route params
  player/CicadaPlayer.ets typed wrapper over libentry.so
  pages/                  SourceChoosePage, SourceChooseListPage,
                          SourceInputUrlPage, SettingPage, MultiPlayerPage,
                          SnapShotPage, AvPlayerPage, Index (the player)
  view/                   TitleBar, PlayerControlBar, PlayerGestureLayer,
                          PlayerTrackPanel, PlayerOptionPanel,
                          PlayerTipsOverlay, MultiPlayerItem
  util/                   Strings, SourceListParser, Settings, NetWatchdog,
                          SnapshotSaver, TimeFormater
entry/src/main/resources/rawfile/  sourceList.json, test.ass
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
callbacks the pages implement.

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
| premierlibrary JNI | `napi_*` | 26 events + ~60 functions |

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

1. **Play-and-cache config is not wired.** `CacheConfig` lives on the
   `MediaPlayer` C++ facade, which the Android SDK's JNI binds directly; the C
   API handle is an `ICicadaPlayer` (`SuperMediaPlayer`) with no cache-config
   method and no cache implementation. The cache tab is kept for parity and logs
   the gap. Wiring it means implementing caching on the `ICicadaPlayer` path.
2. **No DRM licence callback in ArkTS.** `OhosDrmHandler` now exposes the licence
   challenge (`DrmRequestParam::mKeyRequest`) and the server URL
   (`mLicenseUrl`), but `drmCallback` is invoked synchronously on a player
   thread, so a JS callback would need a blocking handshake that NAPI cannot do
   from a non-JS thread. `setDrmCallback` is therefore not exposed yet.
3. **Audio AAC: `OH_MD_KEY_AAC_IS_ADTS` is not set**, so ADTS streams may not
   decode. Pre-existing; `mediaCodecDecoder.cpp` derives it from extra-data sync
   words and could be mirrored.
4. **Compile-verified only.** There is no HarmonyOS device or emulator available
   here, so nothing has been run: gestures, window brightness, orientation
   switching, hardware decoding, DRM and ScanKit all compile and link but are
   unverified at runtime.

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
