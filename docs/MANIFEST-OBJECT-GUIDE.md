# MediaManifest 播放对象开发者指南（对象模式 / 六端）

本文面向**使用播放器的开发者**，说明"对象模式播放"（用一份 `MediaManifest` 描述对象代替 `.m3u8`/`.mpd` 文件）的
正确用法、各端示例、错误处理与注意点。

> 一句话结论：**JSON 文本进内核、内核解析**。各端 SDK 提供类型化模型/构造器让你"写对象"，
> 但它最终都会被序列化成 JSON 文本，交给内核用 **cJSON** 解析；**应用层不做反序列化**，
> 六个端共用同一个解析器（`framework/demuxer/manifest/MediaManifestParser.cpp`）。

---

## 0. 两种下发方式：对象 vs JSON 文本（两条路等价）

同一种"播放对象"有**两种下发方式**，任选其一：

| | **对象方式**（推荐给"拼装"场景） | **JSON 文本方式**（推荐给"转发"场景） |
|---|---|---|
| 你怎么写 | `ManifestBuilder()…build()`（Android Java/Kotlin、Qt C++） | 自己拼字符串 / 直接把服务端返回的清单文本传进来 |
| 好处 | 类型安全、IDE 补全、字段名写错**编译期**就报 | 零转换、零依赖，手里有什么就传什么 |
| Android 入口 | `setDataSource(MediaManifest)` / `setDataSourceManifest(MediaManifest)`（**已提供**） | `setDataSourceManifest(String)`（既有） |
| Qt 入口 | `setManifest(builder)`（**已提供**，C++） | `setManifestJson(QString)`（既有）/ QML 的 `setManifest(QVariantMap)` |
| 其它端 | 目前给 JSON 写法 + 待补清单（见 §7） | `setDataSourceManifest(String)` |

**两条路完全等价，不是"两套实现"**：

```
对象方式:  MediaManifest 对象 --toJsonString()--> JSON 文本 ┐
                                                          ├─> setDataSourceManifest(String) / setManifestJson()
JSON 文本方式: 服务端原始清单文本 -------------------------┘        (Android)                (Qt)
                                                                    │
                                                                    ▼
                                                    JNI / C++ 的 std::string 重载
                                                                    │
                                                                    ▼
                              内核 Manifest::MediaManifestParser::parse()（**cJSON**，只解析一次）
                                                                    │
                                                                    ▼
                                        MSG_SETMANIFESTSOURCE -> ManifestDemuxer::buildPlayList()
```

- **SDK 里没有新增任何解析**：对象方式只调用一次 `manifest.toJsonString()`（纯字符串拼接），
  之后与手写 JSON 文本走**同一个**字符串入口、同一段内核代码。
- 因此：**运行时行为与性能完全相同**（同一份 JSON、同一个 `MediaManifest`、同一条分片管线）。
  对象方式多出的成本只有一次内存内 JSON 序列化，发生在调用 `setDataSource` 之前（毫秒级以下）。
- 两种方式可以**混用**：拼装出来的对象用它、服务端原样返回的清单用文本。

### 怎么选

- 需要**拼装 / 校验 / IDE 补全 / 多人协作改字段** → **对象方式**（编译期就能发现 `duration` 漏写、`mode` 拼错）。
- 只是**转发**后端/配置下发的原始清单（本来就是一坨 JSON） → **JSON 文本方式**（不要为了用它先解析成对象，多一次解析没有任何好处）。
- 两种方式的**错误处理完全一样**（见 §4：`0x20030007`）。

---


## 1. 两种数据源：URL vs 描述对象

| | URL 模式 | 对象模式（MediaManifest） |
|---|---|---|
| 传入 | `.m3u8` / `.mpd` / 直链 URL 字符串 | 描述对象（JSON 文本） |
| 谁去取清单 | 内核自己去下载并解析 m3u8/mpd | **不需要清单文件**：分片地址、加密、DRM 信息都在对象里 |
| 适用 | 标准 HLS/DASH 服务、普通点播/直播 | 自定义签名的 CDN、清单由业务后端生成、无 m3u8/mpd、需要精确控制分片模板/密钥/授权 |
| 入口 | `setDataSource(url)` | `setDataSourceManifest(json)`（Android/Apple/HarmonyOS/Flutter）、`SetDataSource(std::string)`（Qt C++） |

两种入口**互斥**：后设置的那个生效（内核里设 URL 会清掉 manifest，设 manifest 会清空 url）。

---

## 2. 必填 / 可选字段（与内核逐字一致）

**必填**（否则内核解析失败，见 §4）：

- `duration`：媒体总时长（秒），必须 **> 0**
- `video`：视频档位数组，必须**非空**

**可选**（缺省值与内核默认一致；不写就用默认值）：

| 字段 | 说明 |
|---|---|
| `audio[]` | 音频档位数组 |
| `subtitle[]` | 字幕轨（`id/baseUrl/mimeType/codecs/lang/role/name/isDefault/forced/characteristics/segmentInfo`） |
| `periods[]` | 多 Period（`id/start/duration/video[]/audio[]/subtitle[]/contentProtection[]`） |
| `segmentInfo` | 每个档位的分片描述，见下 |
| `encryption` | AES-128 分片加密：`keyUrl/iv/keyFormat/keyFormatVersions/expiresIn` |
| `licenseServer` | DRM 授权服务器：`url/contentId/keyType`（`clearkey`/`aes128`/`widevine`/`fairplay`/`playready`，缺省 `clearkey`） |
| `contentProtection[]` | `schemeIdUri/value/pssh/keyId/laUrl/pro` |
| `live` + `liveConfig` | 直播：`liveConfig.type`(`live`/`event`)、`timeShiftBufferDepth`、`minimumUpdatePeriod`、`availabilityStartTime`、`publishTime`、`partTargetDuration`、`canBlockReload`、`canSkipUntil`、`holdBack`、`partHoldBack` |
| `mediaSourceType` | **`hls`（缺省，DRM 能力在这条管线）** 或 `dash`，决定内核走哪条分片管线 |
| `startTimeOffset` / `startPrecise` | 起始时间偏移 |
| `location` / `minimumUpdatePeriod` / `title` | 元信息 |
| `minBufferTime`(缺省 1.5) / `maxSegmentDuration` | 缓冲与分片时长 |
| `contentSteering` | `serverUrl/defaultCdnId/defaultRedirectUrl/proxyServerUrl` |
| 档位内的字段 | `id/baseUrl/backupUrls[]/bandwidth/averageBandwidth/mimeType/codecs/width/height/frameRate/sar/audioSamplingRate/channelConfig/lang/role/name/isDefault/autoSelect/segmentInfo/encryption` |

### `segmentInfo.mode` 只有三个合法取值（内核 `parseSegmentMode` 逐字比对）

| mode | 含义 | 关键字段 |
|---|---|---|
| `single` | 一个文件 + byte range（SegmentBase） | `initialization`（byte range）、`indexRange`（sidx range） |
| `template` | URL 模板 + 可选时间线 | `media`（含 `*` 占位）、`startNumber`、`segmentTimeline[{t,d,r}]`、`suffix`、`timescale`、`presentationTimeOffset` |
| `list` | 显式分片列表（HLS `#EXTINF` 等价） | `segments[{duration,url,byteRange}]`、`mediaSequence`、`targetDuration` |

> 写错（例如 `"Template"`、`"segmentTemplate"`）内核会**忽略该值**并按结构体默认（`list`）处理 —— 分片会找不到。

### URL 解析规则（内核固定行为）

1. 绝对 `http(s)` URL：原样使用；
2. 以 `/` 开头：按 CDN origin 解析；
3. 相对 URL：按该 Representation 的 `baseUrl` 解析。

---

## 3. 各端调用示例

> 所有示例的实际链路都是：**对象 → JSON 文本 → 内核（cJSON 解析）→ ManifestDemuxer**。
> 老接口 `setDataSource(String url)` 语义**完全不变**，两种方式可共存。

### 3.1 Android（Kotlin / Java）

SDK 提供类型化模型与链式构造器（`additive`，不改老接口）：

- `com.cicada.player.manifest.MediaManifest`（模型，含 `toJsonString()`）
- `com.cicada.player.manifest.ManifestBuilder`（链式构造器）

```kotlin
// Kotlin：对象/构造器 -> JSON 文本 -> 内核解析
val json = ManifestBuilder()
    .duration(120.0)
    .mediaSourceType("hls")
    .addVideo(
        ManifestBuilder.RepresentationBuilder("v0", "https://cdn.example.com/v/")
            .bandwidth(2_000_000).codecs("avc1.64001f").width(1920).height(1080)
            .segmentInfo(
                ManifestBuilder.SegmentInfoBuilder("template")
                    .initialization("init.mp4").media("seg-*.m4s").startNumber(1).targetDuration(4.0)
            )
    )
    .addAudio(
        ManifestBuilder.RepresentationBuilder("a0", "https://cdn.example.com/a/")
            .bandwidth(128_000).codecs("mp4a.40.2").lang("zh")
    )
    .toJsonString()

player.setDataSourceManifest(json)                  // JSON 文本方式（推荐转发场景）
// 或：player.setDataSource(JSONObject(json))        // 老入口，内部 toString() 后走同一条路
```

**对象方式（一等公民入口，推荐拼装场景）**：直接把对象交给 SDK，不需要自己 `toJsonString()`：

```kotlin
// Kotlin
val manifest = ManifestBuilder()
    .duration(120.0)
    .addVideo(ManifestBuilder.RepresentationBuilder("v0", "https://cdn.example.com/v/")
        .bandwidth(2_000_000).codecs("avc1.64001f").width(1920).height(1080))
    .build()
player.setDataSource(manifest)                       // <= 新增：SDK 内部 toJsonString() -> 既有字符串入口
player.setDataSourceManifest(manifest)               //    同上（两个名字等价，按习惯挑一个）
```

```java
// Java
player.setDataSource(manifestBuilder.build());
```

```kotlin
// Compose 应用层的顺手入口（与 playManifest(String) 并列）
controller.playManifest(manifest)                    // 对象方式
controller.playManifest(serverJson)                  // JSON 文本方式
```

```java
// Java 写法完全一样；也可以直接构造模型再序列化
MediaManifest m = new ManifestBuilder().duration(120).addVideo(videoRep).build();
player.setDataSourceManifest(m.toJsonString());
```

Kotlin 更顺手的入口（可选，当前未随包提供，见 §7 未完成清单）：
`fun CicadaPlayer.setDataSource(manifest: MediaManifest) = setDataSourceManifest(manifest.toJsonString())`
—— 一行扩展即可；不提供也不影响使用（上面的 `toJsonString()` 已经是一行）。

### 3.2 Qt（C++）

header-only 构造器：`platform/QtPlayer/src/CicadaManifestBuilder.h`（不改 CMake）。

```cpp
#include "CicadaManifestBuilder.h"

CicadaManifest::Builder b;
b.duration(120).mediaSourceType(QStringLiteral("hls"))
 .addVideo(CicadaManifest::RepresentationBuilder(QStringLiteral("v0"), QStringLiteral("https://cdn.example.com/v/"))
               .bandwidth(2000000).codecs(QStringLiteral("avc1.64001f")).size(1920, 1080)
               .segmentTemplate(QStringLiteral("init.mp4"), QStringLiteral("seg-*.m4s")))
 .addAudio(CicadaManifest::RepresentationBuilder(QStringLiteral("a0"), QStringLiteral("https://cdn.example.com/a/"))
               .bandwidth(128000).codecs(QStringLiteral("mp4a.40.2")).lang(QStringLiteral("zh")));

// ★ 必须传 std::string：传 const char* 会命中 SetDataSource(const char *url) 那个重载，
//   清单会被当成 URL 去打开（表现是"点了没反应"）。
player->SetDataSource(std::string(b.toJsonString().toUtf8().constData()));
```

**对象方式（一等公民入口，推荐拼装场景）**：直接交给 `CicadaPlayerItem`，不用自己序列化：

```cpp
// QML 里拿到的 item：playerItem->setManifest(b);
// 内部链路与上面完全一致：builder.toJsonString() -> setManifestJson(json)
//   -> SetDataSource(std::string) -> 内核 cJSON 解析
playerItem->setManifest(b);                       // <= 新增的并列重载（C++）
playerItem->setManifestJson(serverJson);          // JSON 文本方式（既有，命令行/转发场景）
```

UI 静态信息（清晰度菜单/codecs）**用同一份对象**取，不要再去解析 JSON：

```cpp
const CicadaManifest::StaticInfo info = b.staticInfo();   // videos/audios/primaryVideoCodecs ...
```

### 3.3 HarmonyOS（ArkTS）

```ts
// 当前推荐用法：拼 JSON 文本后调用 SDK 的 setDataSourceManifest
const manifest = {
  duration: 120,
  mediaSourceType: 'hls',
  video: [{
    id: 'v0',
    baseUrl: 'https://cdn.example.com/v/',
    bandwidth: 2000000,
    codecs: 'avc1.64001f',
    width: 1920,
    height: 1080,
    segmentInfo: { mode: 'template', initialization: 'init.mp4', media: 'seg-*.m4s', startNumber: 1 }
  }],
  audio: [{ id: 'a0', baseUrl: 'https://cdn.example.com/a/', bandwidth: 128000, codecs: 'mp4a.40.2', lang: 'zh' }]
};
player.setDataSourceManifest(JSON.stringify(manifest));   // 文本进内核
```

（ArkTS 的类型化 interface + builder 见 §7 未完成清单。）

### 3.4 Flutter（Dart）

```dart
final manifest = {
  'duration': 120.0,
  'mediaSourceType': 'hls',
  'video': [
    {
      'id': 'v0',
      'baseUrl': 'https://cdn.example.com/v/',
      'bandwidth': 2000000,
      'codecs': 'avc1.64001f',
      'width': 1920,
      'height': 1080,
      'segmentInfo': {'mode': 'template', 'initialization': 'init.mp4', 'media': 'seg-*.m4s', 'startNumber': 1},
    }
  ],
  'audio': [
    {'id': 'a0', 'baseUrl': 'https://cdn.example.com/a/', 'bandwidth': 128000, 'codecs': 'mp4a.40.2', 'lang': 'zh'}
  ],
};
await player.setDataSourceManifest(jsonEncode(manifest));   // 通道里传字符串
```

（Dart 类型化 class + builder 见 §7 未完成清单。）

### 3.5 Apple（Swift / Objective-C）

```objc
// Objective-C：SDK 已有 CicadaManifestSource（NSString 承载 JSON 文本）
CicadaManifestSource *src = [CicadaManifestSource manifestWithJson:jsonString];
[self.player setManifestSource:src];
```

```swift
// Swift：先组字典 -> JSON 文本
let manifest: [String: Any] = [
    "duration": 120,
    "mediaSourceType": "hls",
    "video": [[
        "id": "v0", "baseUrl": "https://cdn.example.com/v/",
        "bandwidth": 2_000_000, "codecs": "avc1.64001f", "width": 1920, "height": 1080,
        "segmentInfo": ["mode": "template", "initialization": "init.mp4", "media": "seg-*.m4s", "startNumber": 1]
    ]],
    "audio": [["id": "a0", "baseUrl": "https://cdn.example.com/a/", "codecs": "mp4a.40.2", "lang": "zh"]]
]
let data = try! JSONSerialization.data(withJSONObject: manifest, options: [])
let json = String(data: data, encoding: .utf8)!
player.setManifestSource(CicadaManifestSource.manifest(withJson: json))
```

（Swift/ObjC 的类型化 struct/Builder 见 §7 未完成清单。）

---

## 4. 错误处理（解析失败现在会回调）

解析失败时内核会通过**既有 error 通道**上报：

| 项 | 值 |
|---|---|
| 错误码 | `MEDIA_PLAYER_ERROR_DEMUXER_MANIFEST_PARSE`（应用可见值 **0x20030007**） |
| 文案 | `MediaManifest JSON parse failed: <原因>`（原因来自解析器，例如缺 `video`） |
| 触发时机 | 调用 `setDataSourceManifest(json)` / `SetDataSource(std::string)` 之后**同步**回调 |

各端接收方式（沿用既有 error 回调，无需额外配置）：

- **Android**：`CicadaPlayer.OnErrorListener#onError(ErrorInfo)`，`ErrorInfo.getCode() == 0x20030007`
- **Qt**：`playerListener.onError`（`CicadaPlayerItem` 已把 error 事件转成信号）
- **其它端**：各自 SDK 的 error/onError 回调

**最容易踩的两种误用**：

| 误用 | 结果 | 正确写法 |
|---|---|---|
| JSON 文本传给 `setDataSource(String url)` | 被当成 URL 去打开（失败信息与网络错误混在一起） | 传 `setDataSourceManifest(json)`（Android/Apple/HarmonyOS/Flutter）或 Qt 的 `SetDataSource(std::string)` |
| URL 传给 manifest 入口 | 解析失败 → 0x20030007 | 传 `setDataSource(url)` |

> 说明：Compose 示例 App 里"以 `{` 开头就按播放对象处理"**只是应用层约定**，
> 内核**没有**任何"按首字符自动判定 URL/JSON"的规则；其它 App 需要自己判断。

---

## 5. 注意点

1. **内核用 cJSON 解析**：字段名区分大小写（camelCase），多写/少写键不会报错但会**静默用默认值**
   （`mode` 写错是唯一会明显"分片找不到"的一种）。
2. **必填就两个**：`duration > 0`、`video[]` 非空；其余都能省。
3. **不要重复解析**：Qt 应用层过去为了清晰度菜单又 `QJsonDocument::fromJson` 了一遍同一份 JSON，
   现在请改用构造器的 `staticInfo()`（同一份对象），避免两套 schema 知识漂移。
4. **`mediaSourceType` 决定管线**：默认 `hls`（DRM 能力在这条）；需要 DASH 分片管线时显式写 `dash`。
5. **URL 解析规则**见 §2；`backupUrls` 用于主 URL 失败时回退。
6. **直播**：`live: true` + `liveConfig`；DASH 直播建议同时给 `minimumUpdatePeriod` 与 `availabilityStartTime`。
7. 老接口零影响：`setDataSource(String)` / `setDataSourceManifest(String)` 的签名与语义都没变。

---

## 6. 示例 JSON

### 6.1 最小可用（单档 + 显式分片列表）

```json
{
  "duration": 60,
  "video": [
    {
      "id": "v0",
      "baseUrl": "https://cdn.example.com/v/",
      "bandwidth": 1500000,
      "codecs": "avc1.64001f",
      "width": 1280,
      "height": 720,
      "segmentInfo": {
        "mode": "list",
        "initialization": "init.mp4",
        "targetDuration": 4,
        "segments": [
          { "duration": 4, "url": "seg-1.m4s" },
          { "duration": 4, "url": "seg-2.m4s" }
        ]
      }
    }
  ]
}
```

### 6.2 多码率 + 分片模板 + AES-128 加密

```json
{
  "duration": 600,
  "mediaSourceType": "hls",
  "video": [
    {
      "id": "v-1080", "baseUrl": "https://cdn.example.com/1080/",
      "bandwidth": 5000000, "averageBandwidth": 4200000,
      "codecs": "avc1.640028", "width": 1920, "height": 1080, "frameRate": 25,
      "segmentInfo": {
        "mode": "template", "timescale": 90000, "targetDuration": 4,
        "initialization": "init.mp4", "media": "seg-$Number$.m4s", "startNumber": 1,
        "segmentTimeline": [ { "t": 0, "d": 360000, "r": 149 } ]
      },
      "encryption": { "keyUrl": "https://cdn.example.com/key?kid=1080", "iv": "00000000000000000000000000000001" }
    },
    {
      "id": "v-720", "baseUrl": "https://cdn.example.com/720/",
      "bandwidth": 1500000, "codecs": "avc1.64001f", "width": 1280, "height": 720,
      "segmentInfo": { "mode": "template", "initialization": "init.mp4", "media": "seg-$Number$.m4s", "startNumber": 1 },
      "encryption": { "keyUrl": "https://cdn.example.com/key?kid=720", "iv": "00000000000000000000000000000001" }
    }
  ],
  "audio": [
    {
      "id": "a-zh", "baseUrl": "https://cdn.example.com/audio/", "bandwidth": 128000,
      "codecs": "mp4a.40.2", "lang": "zh", "isDefault": true,
      "segmentInfo": { "mode": "template", "initialization": "init.mp4", "media": "seg-$Number$.m4s", "startNumber": 1 }
    }
  ],
  "licenseServer": { "url": "https://drm.example.com/license", "contentId": "movie-123", "keyType": "widevine" }
}
```

### 6.3 DASH 管线 + 单文件 + byte range

```json
{
  "duration": 600,
  "mediaSourceType": "dash",
  "video": [
    {
      "id": "v0", "baseUrl": "https://cdn.example.com/dash/v.mp4",
      "bandwidth": 4000000, "codecs": "avc1.640028", "width": 1920, "height": 1080,
      "segmentInfo": { "mode": "single", "initialization": "0-1999", "indexRange": "2000-2455" }
    }
  ],
  "audio": [
    {
      "id": "a0", "baseUrl": "https://cdn.example.com/dash/a.mp4",
      "bandwidth": 128000, "codecs": "mp4a.40.2", "audioSamplingRate": 48000,
      "segmentInfo": { "mode": "single", "initialization": "0-1999", "indexRange": "2000-2455" }
    }
  ]
}
```

---

## 7. 各端落地状态（本仓库当前状态）

| 端 | 类型化模型 / 构造器 | 对象方式入口（直接传对象） | 状态 |
|---|---|---|---|
| Android（Compose SDK，同时服务 App 与 AAR 接入方） | `com.cicada.player.manifest.MediaManifest` + `ManifestBuilder` | `setDataSource(MediaManifest)` / `setDataSourceManifest(MediaManifest)`（接口 default 方法）；Compose 侧 `controller.playManifest(manifest)` | **已提供** |
| Qt | `platform/QtPlayer/src/CicadaManifestBuilder.h`（header-only + `StaticInfo`） | `CicadaPlayerItem::setManifest(const CicadaManifest::Builder &)`（C++；与 `setManifest(QVariantMap)`、`setManifestJson(QString)` 并列） | **已提供**（UI 收敛到 `staticInfo()` 未做，见下） |
| HarmonyOS | ArkTS interface + builder | 待补（当前用 `setDataSourceManifest(JSON.stringify(obj))`） | **仅文档** |
| Flutter | Dart class + builder | 待补（当前用 `setDataSourceManifest(jsonEncode(obj))`） | **仅文档** |
| Apple | Swift/ObjC struct + builder | 待补（当前用 `CicadaManifestSource.manifest(withJson:)`） | **仅文档** |

未完成清单（additive，可后续补）：

1. **AAR 侧同一套 Java 模型与对象入口**：`platform/Android/source/premierlibrary/...` 是另一份 SDK 源码树，
   需要把 `com/cicada/player/manifest/` 两个文件复制过去，并在那份 `CicadaPlayer.java` 里补同样两个 default 方法。
2. **Qt UI 收敛**：`CicadaPlayerItem.cpp` 里 `QJsonDocument::fromJson(m_manifestJson)`（约 `:1741`）
   与 `pickCodecs(...)`（约 `:1776`）改成用 `CicadaManifest::StaticInfo::fromJson(obj)` /
   `Builder::staticInfo()`，消掉"同一份 JSON 解析两遍"。
3. **Kotlin 扩展入口**：`fun CicadaPlayer.setDataSource(manifest: MediaManifest)`
   （SDK 已用 Java default 方法提供同名入口，Kotlin 直接可调，因此这条是可选的语法糖）。
4. **HarmonyOS/Flutter/Apple 的类型化 builder 与对象入口**：按 §3 的 JSON 形状镜像实现即可。


---

## 8. 内核侧实现索引（排查用）

| 事项 | 位置 |
|---|---|
| 结构体定义（字段全集） | `framework/demuxer/manifest/MediaManifest.h` |
| JSON 解析（键名、默认值、必填校验） | `framework/demuxer/manifest/MediaManifestParser.cpp` |
| 解析器用 cJSON | `framework/utils/CicadaJSON.h/.cpp`（`#include "cJSON.h"`） |
| 三个数据源重载 | `mediaPlayer/ICicadaPlayer.h`、`mediaPlayer/SuperMediaPlayer.cpp`（url / obj / json） |
| 解析入口（错误回调在此） | `mediaPlayer/SuperMediaPlayer.cpp` `SetDataSource(const std::string &jsonManifest)` |
| 错误码 | `mediaPlayer/media_player_error_def.h` + `mediaPlayer/ErrorCodeMap.cpp`（0x20030007） |
| 对象 → 播放列表 | `framework/demuxer/manifest/ManifestDemuxer.cpp`（`buildPlayList()`） |
