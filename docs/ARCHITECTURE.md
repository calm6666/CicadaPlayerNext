# CicadaPlayerNext 架构图与模块说明

> 本文档描述升级到 **FFmpeg 9.0**、支持 **Android 7.0+ (API 24)**、**HarmonyOS NEXT (API 12+)**
> 硬件加速与**对象清单播放（DRM）**之后的整体架构。

## 1. 总体架构

```mermaid
flowchart TB
    subgraph APP["业务层 (多平台)"]
        A1["Android Demo (paasApp)"]
        A2["Flutter Plugin"]
        A3["HarmonyOS Demo (ArkTS + XComponent)"]
        A4["iOS / macOS Demo"]
        A5["Windows / Linux cmdline"]
    end

    subgraph SDK["SDK API 层 (mediaPlayer)"]
        B1["Java: CicadaPlayer / CicadaPlayerImpl / NativePlayerBase(JNI)"]
        B2["C++: MediaPlayer / ICicadaPlayer"]
        B3["C API: media_player_api.h (CicadaXXX)"]
        B4["HarmonyOS: NAPI (napi_init.cpp)"]
        B5["Object Manifest: MediaManifest / MediaManifestParser / SetDataSource(json|object)"]
    end

    subgraph PLAYER["播放内核 (SuperMediaPlayer)"]
        C1["消息控制 (player_msg_control)"]
        C2["AVDeviceManager (解码器/渲染/DRM 管理)"]
        C3["BufferController / 音视频同步 (MasterClock)"]
        C4["demuxer_service (探测/分派)"]
    end

    subgraph FRAMEWORK["framework 核心"]
        D1["demuxer: avFormatDemuxer / playList_demuxer(HLS,DASH) / ManifestDemuxer(对象模式)"]
        D2["data_source: curl / cachedSource / proxyDataSource / file"]
        D3["codec: avcodecDecoder(软解) / mediaCodecDecoder(安卓硬解) / VideoToolBox(iOS) / OhosAVCodecDecoder(鸿蒙硬解)"]
        D4["render: GLRender / DummyVideoRender / AudioTrackRender / OhosAudioRender"]
        D5["drm: DrmManager / WideVineDrmHandler / OhosDrmHandler"]
        D6["filter: ffmpegVideoFilter / ffmpegAudioFilter"]
    end

    subgraph FFMPEG["FFmpeg 9.0 (external)"]
        E1["libavformat (解封装)"]
        E2["libavcodec (软件解码)"]
        E3["libavfilter / libswresample / libswscale"]
        E4["协议: http/https/rtmp/file/crypto/hls…"]
    end

    A1 --> B1
    A2 --> B1
    A3 --> B4
    A4 --> B2
    A5 --> B3
    B1 --> B2
    B4 --> B3
    B2 --> B3
    B5 --> B2
    B3 --> PLAYER
    PLAYER --> FRAMEWORK
    FRAMEWORK --> FFMPEG
```

## 2. 数据流（URL 模式 vs 对象清单模式）

```mermaid
sequenceDiagram
    participant APP as App
    participant API as MediaPlayer
    participant SMP as SuperMediaPlayer
    participant DS as demuxer_service
    participant DEMUX as Demuxer
    participant DEC as Decoder
    participant REN as Render

    alt URL 播放
        APP->>API: SetDataSource("http://.../x.m3u8")
        API->>SMP: MSG_SETDATASOURCE
        SMP->>DS: openUrl() → curl_data_source
        DS->>DEMUX: probe → playList_demuxer(HLS/DASH)
    else 对象清单播放 (DRM)
        APP->>API: SetDataSource(MediaManifest JSON)
        API->>SMP: MSG_SETMANIFESTSOURCE
        SMP->>DS: setManifestSource(manifest) → demuxer_type_manifest
        DS->>DEMUX: ManifestDemuxer（一次性对象→playList 转换，零网络清单请求）
        DEMUX->>DEMUX: ContentProtection→SegmentEncryption(AES_SAMPLE) + PSSH/KID
    end

    DS->>DEMUX: Open()/ReadPacket()
    DEMUX->>DS: AVPacket/IAFPacket (HLSStream/DashStream 拉分段)
    SMP->>DEC: send_packet (setUpDecoder: DrmInfo→DrmManager→DrmHandler)
    DEC->>REN: IAFFrame (surface mode: 硬件直绘)
    REN->>APP: 画面/声音
```

## 3. 对象清单播放管线（对应 hili-player 的 manifest-to-hls/dash）

```mermaid
flowchart LR
    JSON["MediaManifest JSON<br/>(video[]/audio[]/periods[]<br/>contentProtection[]/licenseServer<br/>encryption(AES-128)…)"]
    PARSER["MediaManifestParser<br/>(CicadaJSON 一次性解析)"]
    STRUCT["Cicada::Manifest::MediaManifest<br/>(C++ 结构体)"]
    CONV["ManifestDemuxer<br/>(一次转换: 与 manifest-to-hls.ts /<br/>manifest-to-dash.ts 同语义)"]
    PL["内部 playList 对象模型<br/>Period→AdaptationSet→Representation→SegmentList"]
    MGR["HLSManager (DRM 能力管线)<br/>HLSStream + SegmentTracker"]
    DRM["SegmentEncryption:<br/>AES-128→软件分段解密<br/>AES_SAMPLE+keyFormat→DrmHandler<br/>(Widevine/FairPlay/ClearKey/OHOS DRM Kit)"]

    JSON --> PARSER --> STRUCT --> CONV --> PL --> MGR --> DRM
```

转换规则与 Web 端完全对齐：
- `mode: template` → `*` 通配符展开成分片 URL（`media`/`initialization` 推导）；
- `mode: list` → 显式分片列表（`byteRange` 映射 `setByteRange`）；
- `mode: single` → init/索引字节范围（SegmentBase 语义）；
- URL 解析：绝对/根路径原样返回，相对路径拼 `baseUrl`；
- `encryption.expiresIn > 0` → 密钥改从 `licenseServer.url` 获取；
- `contentProtection` 第一个可用系统（Widevine/ClearKey/FairPlay/PlayReady）→
  `SegmentEncryption{AES_SAMPLE, keyFormat=schemeIdUri, keyUrl=laUrl|licenseServer.url, pssh, keyId}`
  → `Stream_meta.keyFormat/keyUrl/drmPssh/drmKeyId` → `DrmManager` → 各平台 DrmHandler。

## 4. 硬件解码矩阵

| 平台 | 视频硬解 | 音频输出 | DRM |
|---|---|---|---|
| Android 7.0+ | MediaCodec (surface mode) | AudioTrack | Widevine L1/L3 (DrmManager) |
| iOS / macOS | VideoToolbox | AudioQueue/AudioUnit | FairPlay (keyFormat 通道) |
| HarmonyOS NEXT | **OH_AVCodec 硬解 (surface mode, 零拷贝)** | **OH_AudioRenderer** | **DRM Kit: OH_MediaKeySystem/Session + SetMediakeySessionConfig** |
| Windows | D3D11VA (FFmpeg hwaccel) | SDL | – |
| Linux | VAAPI/VDPAU (FFmpeg hwaccel) | SDL/ALSA | – |
| WebAssembly | 软件解码 | Web Audio | – |

## 5. 鸿蒙平台硬件解码栈

```mermaid
flowchart TB
    ARKTS["ArkTS 页面: XComponent(SURFACE) + XComponentController"]
    ID["getXComponentSurfaceId() → decimal string"]
    NAPI["NAPI: setSurface(id) → strtoull"]
    WIN["OH_NativeWindow_CreateNativeWindowFromSurfaceId(API12+)"]
    SMP["CicadaSetView(window) → SuperMediaPlayer"]
    DEC["OhosAVCodecDecoder"]
    CAP["OH_AVCapability_GetHardwareDecoderName(HARDWARE, video/avc|hevc)"]
    CODEC["OH_AVCodec_CreateByName → SetSurface(window) → Configure → Prepare → Start"]
    CB["回调: onNeedInputBuffer(Annex-B) / onNewOutputBuffer"]
    RENDER["OH_AVCodec_RenderOutputBuffer (零拷贝上屏)"]
    DRM["DRM: OH_MediaKeySystem(uuid) → OH_MediaKeySession →<br/>GenerateMediaKeyRequest → licenseServer → ProcessMediaKeyResponse →<br/>OH_AVCodec_SetMediakeySessionConfig"]

    ARKTS --> ID --> NAPI --> WIN --> SMP --> DEC
    DEC --> CAP --> CODEC --> CB --> RENDER
    DEC --> DRM
```

## 6. 目录结构（新增/修改要点）

```
CicadaPlayerNext/
├── external/
│   ├── player_git_source_list.sh        # FFMPEG_BRANCH=n9.0, patch 默认关闭
│   └── player_ffmpeg_config.sh          # FFmpeg 9 组件清单
├── build_tools/
│   ├── ffmpeg_commands.sh               # 移除 --enable-avresample
│   ├── ffmpeg_cross_compile_config.sh   # + ffmpeg_cross_compile_set_OHOS
│   ├── AndroidConfig.sh                 # 现代 NDK clang + API 24
│   ├── OHOSConfig.sh                    # 鸿蒙 NDK 交叉编译环境
│   ├── build_ohos.sh                    # 鸿蒙外部库一键编译
│   ├── common_build.sh                  # clang 链接 + link_shared_lib_OHOS
│   └── boost/user-config-Android.jam    # clang/libc++ toolset
├── framework/
│   ├── HarmonyOS.cmake                  # 鸿蒙平台配置
│   ├── demuxer/manifest/                # ★ 对象清单播放
│   │   ├── MediaManifest.h              #   统一清单 C++ 结构
│   │   ├── MediaManifestParser.cpp      #   JSON ↔ 结构
│   │   └── ManifestDemuxer.cpp          #   对象 → playList 一次转换
│   ├── codec/OHOS/OhosAVCodecDecoder.*  # ★ 鸿蒙硬解
│   ├── render/audio/OHOS/OhosAudioRender.*  # ★ 鸿蒙音频
│   ├── drm/OHOS/OhosDrmHandler.*        # ★ 鸿蒙 DRM Kit
│   └── utils/ffmpeg_utils.c             # FFmpeg 9 API 迁移
├── mediaPlayer/
│   ├── ICicadaPlayer.h / MediaPlayer.h / media_player_api.h   # + SetDataSource(manifest/json)
│   ├── SuperMediaPlayer.* / player_msg_control.* / SMPMessageControllerListener.*
│   └── player_types.h                   # + manifest 成员
├── platform/
│   ├── Android/…                        # minSdk 24 + setDataSourceManifest(JNI)
│   ├── Flutter/…                        # minSdk 24
│   └── HarmonyOS/                       # ★ 鸿蒙 Demo (hvigor + NAPI)
└── docs/                                # ★ 本文档集
```
