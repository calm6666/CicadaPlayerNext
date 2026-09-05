# HarmonyOS / OpenHarmony demo application (API 12+)

Native + ArkTS demo for CicadaPlayerNext on HarmonyOS NEXT:

- `entry/src/main/cpp/`  — NAPI wrapper over the Cicada C API + full native build
  of the player (framework + mediaPlayer) via CMake.
- `entry/src/main/ets/`  — ArkTS page with an `XComponent` (SURFACE type); the
  surfaceId string is handed to native, which creates an `OHNativeWindow` and
  uses OH_AVCodec **surface mode** hardware decoding (zero-copy to the window).
- DRM: CENC streams use the DRM Kit (`OH_MediaKeySystem`/`OH_MediaKeySession`)
  attached to the decoder; AES-128 HLS-style segments are decrypted in software.

## Build

```bash
export OHOS_SDK=/path/to/ohos-sdk   # contains native/ + <os>/native/
# toolchains: hvigorw is expected in PATH (DevEco Studio command line tools)
hvigorw assembleHap --mode module -p product=default
```

or open the `platform/HarmonyOS` folder in DevEco Studio.

Requirements:
- HarmonyOS NEXT SDK (API 12+; modern zero-copy stack needs API 12).
- The native build pulls in `D:\..\external/install/ffmpeg/OHOS/<abi>/` — build the
  externals first with `external/build_external.sh OHOS` (see docs/Packaging_HarmonyOS.md).
