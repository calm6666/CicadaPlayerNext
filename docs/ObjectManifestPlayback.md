# 对象清单播放（Object-based Playback）与 DRM

## 1. 背景

原播放器只支持 URL 传入（`SetDataSource("http://.../x.m3u8"` / `.mpd`）。
参考 Web 端 `hili-player` 的
[`manifest-to-hls.ts`](../../../front/hili-player/packages/plugins/src/vendor/manifest-to-hls.ts) 与
[`manifest-to-dash.ts`](../../../front/hili-player/packages/plugins/src/vendor/manifest-to-dash.ts) 的写法，
本仓库新增**对象模式播放**：直接传入统一的 `MediaManifest` 对象/JSON，多一种选择——
播放器内部**一次性**将对象转换为内部 playList 结构，**零 m3u8/mpd 文本、零清单网络请求**，
且该对象**支持 DRM 数字版权保护**（AES-128 分段加密 + CENC 内容保护 Widevine/FairPlay/ClearKey/PlayReady）。

## 2. API 入口（各平台）

| 平台 | 入口 |
|---|---|
| C++ | `MediaPlayer::SetDataSource(const Manifest::MediaManifest &)` / `SetDataSource(const std::string &json)` |
| C API | `CicadaSetDataSourceWithManifestObject(player, manifest)` / `CicadaSetDataSourceWithManifest(player, json)` |
| Android (Java) | `CicadaPlayer.setDataSource(JSONObject)` / `setDataSourceManifest(String json)`（默认方法，老实现不受影响） |
| HarmonyOS (NAPI) | `setDataSourceManifest(id, json)` |
| Flutter | 通过 Android/iOS 侧 API 透传 JSON 字符串 |

JSON 结构 = hili-player `MediaManifest` 的 camelCase 字段（`duration` / `video[]` / `audio[]` /
`segmentInfo` / `contentProtection[]` / `licenseServer` / `encryption` / `live` …）。

## 3. 最小示例

### 3.1 普通 fMP4（无加密，模板分片）

```json
{
  "duration": 596.0,
  "live": false,
  "mediaSourceType": "hls",
  "video": [{
    "id": "v0",
    "baseUrl": "https://cdn.example.com/bbb/",
    "bandwidth": 2500000,
    "mimeType": "video/mp4",
    "codecs": "avc1.640033",
    "width": 1920, "height": 1080,
    "segmentInfo": {
      "mode": "template",
      "initialization": "init-0.m4s",
      "media": "seg-*.m4s",
      "startNumber": 1,
      "targetDuration": 4,
      "totalCount": 149
    }
  }],
  "audio": [{
    "id": "a0",
    "baseUrl": "https://cdn.example.com/bbb/",
    "bandwidth": 128000,
    "mimeType": "audio/mp4",
    "codecs": "mp4a.40.2",
    "lang": "en",
    "segmentInfo": {
      "mode": "template",
      "initialization": "init-a0.m4s",
      "media": "seg-a-*.m4s",
      "startNumber": 1,
      "targetDuration": 4,
      "totalCount": 149
    }
  }]
}
```

### 3.2 AES-128 分段加密（HLS 语义）

```json
{
  "duration": 300.0,
  "live": true,
  "encryption": {
    "keyUrl": "https://license.example.com/key/bbb",
    "keyFormat": "identity",
    "iv": "0x00000000000000000000000000000000"
  },
  "video": [ { "id": "v0", "bandwidth": 2000000, "mimeType": "video/mp2t",
               "codecs": "avc1.64001f",
               "segmentInfo": { "mode": "list", "segments": [
                 { "duration": 4.0, "url": "seg-1.ts" },
                 { "duration": 4.0, "url": "seg-2.ts" }
               ] } } ]
}
```

> `expiresIn > 0` 时密钥视为临时密钥，播放器改从 `licenseServer.url` 获取。

### 3.3 CENC DRM —— Widevine（DASH 语义的 ContentProtection）

```json
{
  "duration": 7200.0,
  "live": false,
  "mediaSourceType": "dash",
  "contentProtection": [
    { "schemeIdUri": "urn:mpeg:dash:mp4protection:2011", "value": "cenc" },
    {
      "schemeIdUri": "urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed",
      "pssh": "AAAAW3Bzc2gAAAAA7e+LqXnWSs6jyCfc1R0h7QAAADsIARIQ62dqu8s0Xpa7z2FmMPGj2hoNd2lkZXZpbmVfdGVzdCIQZmtqM2xqYVNkZmFsa3IzaioCSEyVAg==",
      "keyId": "eb676abb-cb34-5e96-bbcf-616630f1a3da"
    }
  ],
  "licenseServer": { "url": "https://license.example.com/widevine", "keyType": "widevine" },
  "video": [ { "id": "v0", "bandwidth": 4500000, "mimeType": "video/mp4",
               "codecs": "avc1.640028", "width": 1920, "height": 1080,
               "segmentInfo": { "mode": "template", "initialization": "init-0.m4s",
                                "media": "0-*.m4s", "startNumber": 1,
                                "segmentTimeline": [ { "t": 0, "d": 240240, "r": 299 } ],
                                "timescale": 60000 } } ],
  "audio": [ { "id": "a0", "bandwidth": 192000, "mimeType": "audio/mp4",
               "codecs": "mp4a.40.2", "lang": "en",
               "segmentInfo": { "mode": "template", "initialization": "init-a0.m4s",
                                "media": "a-*.m4s", "startNumber": 1,
                                "segmentTimeline": [ { "t": 0, "d": 192512, "r": 299 } ],
                                "timescale": 48000 } } ]
}
```

### 3.4 CENC DRM —— ClearKey（自建 license server）

与 3.3 相同结构，仅替换：

```json
"contentProtection": [
  { "schemeIdUri": "urn:mpeg:dash:mp4protection:2011", "value": "cenc" },
  { "schemeIdUri": "urn:uuid:e2719d58-a985-b3c9-781a-b030af78d30e",
    "keyId": "01234567-89ab-cdef-0123-456789abcdef" }
],
"licenseServer": { "url": "http://localhost:8765/api/clearkey/bbb", "keyType": "clearkey" }
```

## 4. DRM 内部通路（与 URL 模式一致）

```
ContentProtection[] ──► ManifestDemuxer ──► SegmentEncryption{
      AES_SAMPLE, keyFormat = schemeIdUri(UUID), keyUrl = laUrl|licenseServer.url,
      pssh, keyId }
        │ 挂在每个 segment 上（setEncryption）
        ▼
HLSStream::GetStreamMeta ──► Stream_meta.keyFormat / keyUrl / drmPssh / drmKeyId
        ▼
SMPAVDeviceManager::setUpDecoder ──► DrmInfo{format,uri,pssh,keyId}
        ▼
DrmManager::require ──► 平台 DrmHandler：
   Android: WideVineDrmHandler (Java MediaDrm 会话, MediaCodec CryptoInfo)
   iOS:     FairPlay 通道 (VideoToolbox + keyFormat)
   HarmonyOS: OhosDrmHandler (OH_MediaKeySystem/OH_MediaKeySession
             → OH_AVCodec_SetMediakeySessionConfig 管道内解密)
        ▼
解码器输出明文帧 → 渲染
```

- **AES-128**（`encryption` 字段）：`SegmentEncryption{AES_128, keyUrl, iv}` → HLSStream 拉取 16 字节密钥、软件分段解密；
- **CENC**（`contentProtection` 字段）：`AES_SAMPLE + keyFormat`，HLSStream 将流标记为 DRM 保护，
  解密交给硬件安全路径（Widevine L1/L3、DRM Kit）；
- `setDrmRequestCallback`（C++）/ `DrmCallback`（Java）在 DRM Handler 请求许可证时回调业务方，
  业务方负责把 challenge POST 到 `licenseServer.url` 并返回 license 响应。

## 5. 分段模式语义

| mode | 字段 | 内部转换 |
|---|---|---|
| `template` | `initialization` + `media`（`*` 占位）或显式 `media`；`startNumber`/`totalCount`/`segmentTimeline`/`timescale` | 展开为显式 segment 列表 + `init_section`（同 manifest-to-hls.ts 的 `expandSegmentTemplate`） |
| `list` | `segments[]`（`duration`/`url`/`byteRange`） | 直接构造 SegmentList，byteRange → `setByteRange` |
| `single` | `initialization`/`indexRange`（字节范围） | SegmentBase 语义（字节范围定位 init/sidx/媒体） |

URL 解析优先级（与 TS 端 `resolveUrl` 一致）：
`http(s)://` 绝对地址 > `/` 根路径 > `baseUrl` 相对拼接 > 原样返回。

## 6. 直播

`"live": true` + `liveConfig{type:"live"|"event", timeShiftBufferDepth, minimumUpdatePeriod, holdBack…}`
映射到 playList `type="dynamic"` / `timeShiftBufferDepth` / `minUpdatePeriod`，
HLSManager 按直播窗口调度刷新（刷新来源仍为 `location` 或分片端点，不产生清单文本解析）。
