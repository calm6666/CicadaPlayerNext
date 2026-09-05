![Cicada Logo](doc/Cicada.svg)

[![coverity Status](https://scan.coverity.com/projects/20151/badge.svg?flat=1)](https://scan.coverity.com/projects/alibaba-cicadaplayer)
[![codecov](https://codecov.io/gh/alibaba/CicadaPlayer/branch/develop/graph/badge.svg)](https://codecov.io/gh/alibaba/CicadaPlayer/branch/develop)
[![LICENSE](https://img.shields.io/badge/license-MIT-orange.svg)](LICENSE)

[![iOS CI](https://github.com/alibaba/CicadaPlayer/actions/workflows/iOS.yml/badge.svg)](https://github.com/alibaba/CicadaPlayer/actions/workflows/iOS.yml)
[![macOS CI](https://github.com/alibaba/CicadaPlayer/actions/workflows/macOS.yml/badge.svg)](https://github.com/alibaba/CicadaPlayer/actions/workflows/macOS.yml)
[![Android CI](https://github.com/alibaba/CicadaPlayer/actions/workflows/Android.yml/badge.svg)](https://github.com/alibaba/CicadaPlayer/actions/workflows/Android.yml)
[![Linux CI](https://github.com/alibaba/CicadaPlayer/actions/workflows/Linux.yml/badge.svg)](https://github.com/alibaba/CicadaPlayer/actions/workflows/Linux.yml)

<h1 align="center">
  Keep the world free of difficult videos to play!
</h1>

Cicada Media Player is a multi platform player sdk，**Keep the world free of difficult videos to play**，using Cicada Media Player, build your multimedia apps happily.

## Try it on Android devices

[![deom](doc/demoQR.png)](https://alivc-demo-cms.alicdn.com/versionProduct/other/public/cicadaPlayer/cicadaPlayer.html)

## HOW TO compile

The default ffmpeg, curl, and openssl git url is the github mirror, if you want use another, set it before compile like:

```bash
export FFMPEG_GIT=https://gitee.com/mirrors/ffmpeg.git
export OPENSSL_GIT=https://gitee.com/mirrors/openssl.git
export CURL_GIT=https://gitee.com/mirrors/curl.git
```

国内镜像一键预设（构建时传入，不传则默认 github；镜像失败自动回退）：

```bash
source external/china_mirror_env.sh     # gitee 镜像预设
# 或 export GIT_MIRROR_PREFIX=https://gitclone.com   # 实时代理式镜像
```

完整说明（含 gradle/NDK/brew/Flutter 镜像）见 [`docs/ChinaMirrors.md`](docs/ChinaMirrors.md)。

- [1. compile iOS](doc/compile_ios.md)
- [2. compile Android](doc/compile_Android.md)
- [3. compile_Linux](doc/compile_Linux.md)
- [4. compile_Windows (cross compile)](doc/compile_Windows.md)
- [5.compile_Windows(msvc)](doc/compile_Windows_msvc.md)
- [6. compile_MacOS](doc/compile_mac.md)
- [7. compile HarmonyOS / OpenHarmony](docs/Packaging_HarmonyOS.md)
- 8.webAssembly coming soon


## How to use

- [1. cmdline (Windows/MacOS/Linux)](cmdline/README.md)
- [2. Android](platform/Android/README.md)
- [3. iOS/MacOS](platform/Apple/README.md)
- [4. HarmonyOS Demo (ArkTS + XComponent)](platform/HarmonyOS/README.md)
- 5.webAssembly coming soon


## Features
- HLS LL-HLS master play list support, seamless switch
- MPEG-DASH
- WideVine
- ABR
- hardware decode on Android, Apple and HarmonyOS (OH_AVCodec) platforms
- HEVC support
- OpenGL render
- HDR render
- change volume by software
- speed playback
- snapshot

## FFmpeg 9.0 + 对象清单播放（本仓库升级要点）

- **FFmpeg 9.0**：全部依赖升级至 FFmpeg n9.0，旧版 API 全部迁移（详见
  [`docs/FFmpeg9_Upgrade.md`](docs/FFmpeg9_Upgrade.md)）。
- **Android 最低版本 7.0 (API 24)**：全部 gradle/cmake/CI 已升级（详见
  [`docs/Packaging_Android.md`](docs/Packaging_Android.md)）。
- **HarmonyOS NEXT 硬件加速**：OH_AVCodec 硬解（surface 零拷贝）、OH_AudioRenderer、
  DRM Kit、NAPI + XComponent Demo（详见 [`docs/Packaging_HarmonyOS.md`](docs/Packaging_HarmonyOS.md)）。
- **对象模式播放（不再只能传 URL）**：与 Web 端 hili-player 的
  `manifest-to-hls.ts`/`manifest-to-dash.ts` 同款语义 —— 直接传入
  `MediaManifest` 对象/JSON（模板/列表/单文件分片、直播、AES-128、Widevine/FairPlay/ClearKey DRM），
  详见 [`docs/ObjectManifestPlayback.md`](docs/ObjectManifestPlayback.md)。
- **架构与各平台打包流程**：[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)。
- subtitle title and external subtitle title
- on playing cache
- background video playback
- http keep-alive
- customer ip resolve
- video rotation
- black list on Android hardware h264 decoder
- plugin supporting


## Contact

[![DingDing](doc/CicadaDingDing.png)](https://h5.dingtalk.com/invite-page/index.html?bizSource=____source____&corpId=ding42c495ce0dcfdb7f35c2f4657eb6378f&inviterUid=B8D63ADF200A8E9DFF4BBDCA828801C7&encodeDeptId=FE36B0936DFD1AC591E7FE61FE0552A6)

## License
```c++
MIT LICENSE

Copyright (c) 2019-present Alibaba Inc.
```
CicadaPlayerSDK using the projects:

- LGPL

   [FFmpeg](http://ffmpeg.org/)

   [libvlc](https://www.videolan.org/vlc/libvlc.html)

- CURL LICENSE

  [curl](https://curl.haxx.se)

- Apache License v2

  [OpenSSL](https://www.openssl.org/)

- zlib license

  [SDL](https://www.libsdl.org/)
  
- BSL-1.0 license

  [boost](https://www.boost.org/)
- MIT License

  [libxml2](http://xmlsoft.org/)


