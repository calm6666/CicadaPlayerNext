# DASH 的 DRM：方案覆盖、选优、谁来解

> 本文回答"**DASH 支持的几种 DRM 是不是都实现了**"。
> 每条都带**文件:函数**（或文件:行号）证据，并明确区分
> **【已实测】** / **【代码在位、未实测】** / **【有意不做】**。
>
> 施工记录与"加密→密钥服务器→播放器"那条链路见
> `docs/DRM-ENCRYPTION-DESIGN.md`；收口审计见 `docs/DRM-COMPLETENESS-AUDIT.md`；
> 软解性能见 `docs/DRM-SOFTWARE-DECRYPT-PERFORMANCE.md`。

---

## 0. 一句话结论

**DASH 清单里能出现的 DRM 方案，现在**一个不漏**地被识别、归类，并按"平台能不能硬解、
清单有没有给取密钥的地址"两条判据决定由谁解。** 之前只有"Widevine 一条 + 一堆硬编码
字面量"，而且**每条路上都有一个会让它彻底跑不起来的真 bug**（见 §5）。

需要说清楚的边界：
* **能识别 ≠ 能解密**。内核的软解只覆盖 **CENC**（`cenc`/`cens`/`cbc1`/`cbcs`）；
  HLS Clear Key、MPEG-2 TS 的条件接收、DASH-IF 的 SEA 不是 CENC，`DrmSchemes` 会
  明确把它们标成 `cencCapable = false`（见 §3），而不是拿不认识的字节去试。
* **Apple FairPlay 与 SEA 仍未实现**（需要证书/许可服务，或另一套密钥分发），
  见 §8。

---

## 1. 方案从哪来：DASH-IF 的内容保护标识登记表

`schemeIdUri` 的权威清单是 DASH-IF 的登记表：
<https://dashif.org/identifiers/content_protection/>。它分两类：

* **通用标识**：`urn:mpeg:dash:mp4protection:2011`（CENC 基线）、
  `urn:mpeg:dash:13818:1:CA_descriptor:2011`（MPEG-2 TS 条件接收）；
* **保护系统专有标识**：**28 个 UUID**，清单里写成 `urn:uuid:<uuid>`。

`framework/drm/DrmSchemes.cpp` 把这两类**逐条**落成一张静态表（28 + 3：额外收录了
DASH-IF 已废弃但历史上真实使用的 `urn:mpeg:dash:sea:2012`），并提供纯函数查询。
表里的 `cencCapable` 一列标的是"**内核的 `CENCDecrypter` 能不能动它**"。

登记表里的 28 个 UUID（`DrmSchemes::nameOf` 逐个可查，`drm_schemes_check` 抽查 13 条）：

| UUID | 名称 |
|---|---|
| `1077efec-c0b2-4d02-ace3-3c1e52e2fb4b` | W3C Common PSSH box |
| `1f83e1e8-6ee9-4f0d-ba2f-5ec4e3ed1a66` | SecureMedia |
| `279fe473-512c-48fe-ade8-d176fee6b40f` | Arris Titanium |
| `35bf197b-530e-42d7-8b65-1b4bf415070f` | DivX DRM Series 5 |
| `37c33258-7b99-4c7e-b15d-19af74482154` | Commscope Titanium V3 |
| `3d5e6d35-9b9a-41e8-b843-dd3c6e72c42c` | ChinaDRM |
| `3ea8778f-7742-4bf9-b18b-e834b2acbd47` | Clear Key AES-128（HLS CBC）**非 CENC** |
| `45d481cb-8fe0-49c0-ada9-ab2d2455b2f2` | CoreCrypt |
| `5e629af5-38da-4063-8977-97ffbd9902d4` | Marlin Adaptive Streaming SP |
| `616c7469-6361-7374-2d50-726f74656374` | Alticast |
| `644fe7b5-260f-4fad-949a-0762ffb054b4` | CMLA (OMA DRM) |
| `6a99532d-869f-5922-9a91-113ab7b1e2f3` | MobiTV DRM (MobiDRM) |
| `6dd8b3c3-45f4-4a68-bf3a-64168d01a4a6` | ABV DRM (MoDRM) |
| `793b7956-9f94-4946-a942-23e7ef7e44b4` | VisionCrypt |
| `80a6be7e-1448-4c37-9e70-d5aebe04c8d2` | Irdeto Content Protection |
| `94ce86fb-07ff-4f43-adb8-93d2fa968ca2` | Apple FairPlay |
| `992c46e6-c437-4899-b6a0-50fa91ad0e39` | SecureMedia SteelKnot |
| `9a04f079-9840-4286-ab92-e65be0885f95` | Microsoft PlayReady |
| `9a27dd82-fde2-4725-8cbc-4234aa06ec09` | Verimatrix VCAS |
| `a68129d3-575b-4f1a-9cba-3223846cf7c3` | Synamedia/Cisco/NDS VideoGuard |
| `aa11967f-cc01-4a4a-8e99-c5d3dddfea2d` | Unitend DRM (UDRM) |
| `adb41c24-2dbf-4a6d-958b-4457c0d27b95` | Nagra MediaAccess PRM 3.0 |
| `b4413586-c58c-ffb0-94a5-d4896c1af6c3` | Viaccess-Orca DRM (VODRM) |
| `be58615b-19c4-4684-88b3-c8c57e99e957` | Clear Key SAMPLE-AES（HLS CBCS）**非 CENC** |
| `dcf4e3e3-62f1-5818-7ba6-0a6fe33ff3dd` | DigiCAP SmartXess |
| `e2719d58-a985-b3c9-781a-b030af78d30e` | W3C ClearKey（DASH-IF Clear Key） |
| `edef8ba9-79d6-4ace-a3c8-27dcd51d21ed` | Widevine |
| `f239e769-efa3-4850-9c16-a903c6932efb` | Adobe Primetime DRM v4 |

> **【订正】** 转码脚本原来把 `urn:uuid:d0ee2730-09b5-459f-8452-200e52bec0f7` 当成
> "DASH-IF 的 AES-128 标识"写进 `output.mpd` —— 那个 UUID **不在登记表里**，它只出现在
> DASH-IF IOP 文档的示例中（配 `value="FirstDRM 2.0"`）。现在换成登记表里语义一字不差的
> `3ea8778f-…`（"Identifier for HLS Clear Key encryption using CBC mode"），
> 内核也认得出它（归类为 `HlsClearKeyCbc`，**非** CENC）。

---

## 2. 归一化：清单里的写法不统一，但**不猜**

`DrmSchemes::canonical()` 把下面这些写法收敛到同一个规范形式 `urn:uuid:<小写>`：

| 清单/KEYFORMAT 里的写法 | 归一化结果 |
|---|---|
| `urn:uuid:EDEF8BA9-79D6-4ACE-A3C8-27DCD51D21ED` | `urn:uuid:edef8ba9-…` |
| `  urn:uuid:edef8ba9-…  `（带空白） | 同上 |
| `edef8ba9-79d6-4ace-a3c8-27dcd51d21ed`（裸 UUID） | `urn:uuid:edef8ba9-…` |
| `{edef8ba9-…}`（带花括号） | `urn:uuid:edef8ba9-…` |
| `EDEF8BA979D64ACEA3C827DCD51D21ED`（32 hex） | `urn:uuid:edef8ba9-…` |
| `com.widevine.alpha` / `org.w3.clearkey` / `com.microsoft.playready` / `com.apple.fps*` / `com.adobe.primetime` | 对应的 `urn:uuid:…` |
| `urn:mpeg:dash:mp4protection:2011` | 原样（**它不是一个 DRM 系统**） |
| 认不出的 `urn:uuid:…` / 认不出的别名 | **原样返回**，标成"未登记" |

最后一行是刻意的：把不认识的写法"猜"成某个已知系统，比认不出来**更糟**（会让播放器去
建一个错误的 DRM 会话）。这一点由 `drm_schemes_check` 的第 1 组断言钉死。

---

## 3. 归类：这个方案的内核能不能解

`DrmSchemes::isCencCapable(scheme)`：

* 表里有的 ⇒ 用表里的 `cencCapable`。**false** 的只有四条：
  `urn:mpeg:dash:13818:1:CA_descriptor:2011`（MPEG-2 TS CA）、
  `urn:mpeg:dash:sea:2012`（SEA 有自己的密钥分发）、
  `3ea8778f-…`/`be58615b-…`（登记表原话就是它们描述的是 **HLS** 的 Clear Key
  AES-128/SAMPLE-AES，不是 CENC 的样本加密格式）。
* 表里没有、但形如 `urn:uuid:<uuid>` 的 ⇒ **true**。理由不是猜测：DASH 里用 DRM 系统
  UUID 指名的 Representation，其媒体段就是按 ISO/IEC 23001-7 封装的 —— 这正是
  `schemeIdUri="urn:uuid:…"` 这个写法的定义（登记表开头那段：UUID 表示 PSSH 的
  SystemID）。所以**新出现、本表还没登记的** DRM 系统也能走同一条软解路。

---

## 4. 选优：清单声明了多条时选哪一条

DASH 允许同一个 AdaptationSet/Representation 挂多条 `<ContentProtection>`
（典型：`mp4protection` + Widevine + PlayReady + ClearKey）。
`DrmSchemes::decide(candidates)` 的排序判据（从高到低）：

1. **不是 CENC 方案的一律扣 100000**（内核解不了，平台也基本不认）；
2. **本平台 CDM 认的 +100000** —— 能硬解就硬解；
3. **带许可/密钥地址的 +1000** —— 没有 CDM 的平台上，能取到密钥比"名字好听"重要：
   一个只有 `mp4protection` + `laurl` 的清单**能软解**，而一个指名 Widevine 却没有
   `laurl` 的清单在没有 CDM 的平台上**不能**；
4. 指名了具体 DRM 系统的 +100（只有 `mp4protection` 的通用声明拿不到这一分）；
5. 同分时**后声明的赢** —— `MPDParser` 先塞 AdaptationSet 层再塞 Representation 层，
   所以这正好是 DASH 的"Rep 覆盖 AS"继承语义。

结论是 `Decision`（纯值，没有开关）：
`index` / `usable` / `handledByPlatform` / `schemeIdUri` / `systemId` / `licenseUrl` /
`pssh` / `keyId`。**一条都用不了时给 `index = -1` 并让调用方打一条说明原因的错误**，
不猜、不静默降级。

---

## 5. 修掉的真 bug（这一段是"为什么必须改"的证据）

| # | 位置（改前） | 症状 | 现在 |
|---|---|---|---|
| 1 | `DashStream` 把 `schemeIdUri` **无条件**写进 `Stream_meta.keyFormat` | `codecPrototype::create` 的判据是 `drmInfo == nullptr \|\| codec->is_drmSupport(drmInfo)`（`codecPrototype.cpp:29/37`），而 `avcodecDecoder::is_drmSupport` **恒返回 false**（`avcodecDecoder.h:204`）。于是一个只有 `mp4protection` 的标准 MPD 会让 **所有软解解码器被排除**，起播直接失败 —— 这正是"CENC-in-DASH 从没串起来播过"的直接原因 | 只有**平台 CDM 认**这个 scheme 时才填 `keyFormat`（`DashStream::applyDrmFromContentProtection`），否则留空，样本交给内核软解 |
| 2 | `DrmUtils::isSupport()` 只在 `#ifdef ANDROID` 下认 Widevine 一条 | **OHOS 上连它自己已经实现的** Widevine/PlayReady/FairPlay/ClearKey 都被判成"不支持"而整条跳过（`HlsParser` 产出的 DRM 记录直接丢）；而且这是 L1 共享代码里的平台宏 | `HLSStream.cpp:725` 改问平台无关的 `DrmHandlerPrototype::isSupport`；`framework/utils/DrmUtils.{h,cpp}` **删除**（全仓已无引用），CMakeLists 同步 |
| 3 | `OhosDrmHandler::is_supported()` 比的是**裸 UUID**（`"edef8ba9-…"`），而全工程 `keyFormat` 的口径是 `urn:uuid:…` | 条件恒假 ⇒ **OHOS 的 DRM Kit 代码一次都没被触发过** | 常量改成规范形式，并经 `DrmSchemes::canonical` 归一化（`OhosDrmHandler.cpp:80`） |
| 4 | `mediaCodecDecoder::is_drmSupport()` 硬编码"必须是 Widevine" | Android 上声明 PlayReady / ClearKey 的清单被判成"没有平台 DRM"，走软解并失败 | 只问 `DrmHandlerPrototype::isSupport`（`mediaCodecDecoder.h`），与 `WideVineDrmHandler::is_supported` 的三家 UUID 一致 |
| 5 | `WideVineDrmHandler::is_supported()` 只认 Widevine | 同上；Android 的 `DrmSessionManager` 里根本没有第二家的 MediaDrm UUID | C++ 侧认三家（`WideVineDrmHandler.cpp:44-48,153`），Java 侧按 keyFormat 选 Widevine/PlayReady/ClearKey 的 `MediaDrm` UUID |
| 6 | `SuperMediaPlayer` 用"是不是 Widevine"决定要不要强制硬解 + dummy 渲染面 | PlayReady/ClearKey 的 DRM 内容会被送进软解路径，而软解解码器又被判"不支持 DRM" ⇒ 一个解码器都建不出来 | 新增 `SuperMediaPlayer::isDrmVideo()`（`SuperMediaPlayer.cpp:8617`），问平台认不认；`VIDEO_TAG_WIDEVINE_L1` 那个**标签**仍保持只给 Widevine |
| 7 | `DrmSessionManager.requestKey()` 里 `keyUrl.substring(indexOf(','))` | 连逗号一起切，解出来必然不是合法 box；而且 `pssh` 从来没从 native 传过来（JNI 签名只有两个 String） | 传 `keyFormat/licenseUrl/pssh/keyId/mime` 五个参数（JNI 签名同步改），initData 优先用 `cenc:pssh`；逗号改成 `indexOf(',') + 1` |
| 8 | `DashStream` 声称"容器里的 KID 会由底层 demuxer 填进 `meta->drmKeyId`" | **没有任何代码填它**（全仓 `drmKeyId` 的写入点只有清单侧）。所以"清单没写 `cenc:default_KID`"的片源永远登记不上软解密钥 | 改成**按需取密钥**：`demuxer_service::setCencKeyResolver` + `resolveCencKey`（`demuxer_service.cpp:215/253/353`），在读到**第一个带 `AV_PKT_DATA_ENCRYPTION_INFO` 的包**时用它自带的 KID 回调清单层取密钥 —— 那才是解密查表用的那个 KID |
| 9 | `ManifestDemuxer` 把 CENC 记成 `SegmentEncryption::AES_SAMPLE` | HLS 的 `SAMPLE-AES` 与 CENC 的密文布局/IV 语义/解密器**完全不同**，复用会让 `HLSStream` 拿 `HLSSampleAesDecrypter` 去解 CENC 样本（把流解坏且无日志） | 新增 `SegmentEncryption::CENC`（`SegmentEncryption.h:40`）：`HLSStream` 对它**不建任何解密器**，并把分片字节原样交给内层 demuxer |

---

## 6. 谁来解：硬解优先，软解兜底

```
清单声明 ──► DrmSchemes::decide
                │
                ├─ handledByPlatform = true ──► Stream_meta.keyFormat/keyUrl/drmPssh/drmKeyId
                │                                └─► SMPAVDeviceManager 组 DrmInfo
                │                                     └─► Android: MediaCodec + MediaCrypto
                │                                          queueSecureInputBuffer
                │                                         OHOS: DRM Kit + SetDecryptionConfig
                │
                └─ handledByPlatform = false 且清单给了地址
                       └─► 内核软解：demuxer_service::readPacket → CENCDecrypter
```

### 6.1 平台 CDM 侧【代码在位、未实测】

| 平台 | 认哪几家 | 实现 |
|---|---|---|
| Android | Widevine / PlayReady / ClearKey | `WideVineDrmHandler::is_supported`（`framework/drm/WideVineDrmHandler.cpp`）+ `DrmSessionManager.java`：按 keyFormat 选 `MediaDrm` 的 UUID，并用 `MediaDrm.isCryptoSchemeSupported` 做**设备能力**判定；不支持就明确置 `ERROR_CODE_UNSUPPORT_SCHEME` |
| OHOS | Widevine / PlayReady / FairPlay / ClearKey | `OhosDrmHandler::is_supported`（`framework/drm/OHOS/OhosDrmHandler.cpp`），走 DRM Kit 的 `OH_MediaKeySystem_Create` |

**未实测的原因**：本机是 Windows，没有真机也没有授权内容。Android 的候选集合是确定的，
但"设备到底有没有这个 scheme 的插件"只有真机能答（Java 那一步就是它在答）。

### 6.2 内核软解侧【已实测（解密器与读取路径）+ 代码在位（DASH 全链路）】

* 取密钥：`ContentKeyFetcher`（`framework/demuxer/sample_decrypt/ContentKeyFetcher.{h,cpp}`）
  认**三种**响应形状：
  1. 正好 **16 字节**的裸密钥（本工程密钥服务器 `/key/{kid}` 的默认形状，也是 HLS
     `#EXT-X-KEY` URI 必须返回的东西）；
  2. **W3C ClearKey 许可证** `{"keys":[{"kty":"oct","k":"<base64url>","kid":"…"}],"type":"temporary"}`
     —— 这是 ClearKey 的**标准**形状（base64**url** 字母表，无填充）；
  3. 本工程密钥服务器的 JSON 视图 `{"kid":"…","key":"<32 hex>",…}`。
  其余一律 `Unrecognized`/`Malformed` 并说清原因 —— **绝不把半截字节当密钥**。
* 解密：`demuxer_service::applyCencDecryption` → `CENCDecrypter`（既有实现，Bento4 产物
  150/150 逐字节等于明文）。
* **不是重试机制**：每个 KID 只问一次 resolver（成功失败都记，`mCencResolveAttempted`）——
  这是"这个 KID 问过了"的备忘录，没有计时器、没有次数上限、失败不会因为下一个包再来一遍。

### 6.3 ClearKey 的端到端

Go 密钥服务器（`server/drm-keyserver/`）新增 W3C ClearKey 许可证：

```bash
# 裸 16 字节（HLS #EXT-X-KEY 用；默认形状，行为一个字节都没变）
curl "http://127.0.0.1:PORT/key/<kid>"
# W3C ClearKey 许可证（ClearKey DASH 的 laurl 用）
curl "http://127.0.0.1:PORT/key/<kid>?format=clearkey"
curl "http://127.0.0.1:PORT/license/<kid>?format=clearkey"
```

`?format=clearkey` 是**可选**的：默认形状不变，所以 HLS AES-128 那条路完全不受影响
（`clearkey.go` 的注释与 `main_test.go::TestClearKeyDoesNotChangeRawBytes` 都在钉这一条）。
单测覆盖：base64url 字母表（`0xfb 0xff 0xbf` 前缀会逼出 `-`/`_`）、无填充、
"k" 与 "kid" 都解回原始 16 字节、不泄漏 hex、`format=clearkey` 与 `scheme=` 同时出现是 400。

Android 侧 ClearKey 还有一条：清单只写了 `cenc:default_KID` 而没有 `<cenc:pssh>` 时，
`DrmSessionManager.buildCommonPssh()` 按 **W3C EME 的 Common PSSH box** 格式
（SystemID `1077efec-c0b2-4d02-ace3-3c1e52e2fb4b`，version 1，KID 列表）**按规范构造**
一个 48 字节的 `pssh` box —— 这是翻译，不是猜。

---

## 7. 两条 DASH 播放路径都接上了

| 路径 | 入口 | 本轮改动 |
|---|---|---|
| URL 直连 `.mpd` | `MPDParser` → `Representation::ContentProtection` → `DashStream` | `DashStream::drmDecision/applyDrmFromContentProtection/needsSoftwareCencDecryption/declaresCencProtectedContent/fetchSoftwareCencKey`；`createDemuxer` 里按 `declaresCencProtectedContent()` 设形态提示（`DashStream.cpp:442`），并在需要软解时装 resolver（`:413`） |
| 对象化 JSON 清单（CMAF/CMAF-CENC） | `MediaManifestParser` → `ManifestDemuxer` → `HLSStream` | `ManifestDemuxer::buildSegmentEncryptions` 改走 `DrmSchemes::decide`，方法记成 `SegmentEncryption::CENC`（`:208`）；`HLSStream` 对 CENC **不建解密器**、不把"没有整片解密器"判成失败（`HLSStream.cpp:204`），并在有地址时装 resolver（`:807`） |

> `HLSStream.cpp:204` 那条判据值得单说：原来的逻辑是"清单声明了加密但解密器没建出来
> ⇒ 返回 `-EIO`，拒绝把密文当明文交出去"。CENC 恰恰**不该在这一层有解密器**
> （逐样本解在 `demuxer_service` 里做），所以它必须被排除在那条判据之外 ——
> 否则 CENC 的分片一个字节都读不出来。

---

## 8. 有意不做的，以及为什么

| 项 | 判断 | 理由 |
|---|---|---|
| **Apple FairPlay 的解密** | **不做** | FairPlay 的密钥从不出 CDM，且 Apple 要求走它自己的渲染路径；本工程的 Apple 侧是自绘 VTB 管线。要做等于新立项（需要 FairPlay 证书 + 真实许可服务），而且**本机没有 Apple 工具链**，写了也无法编译验证。`DrmSchemes` 认得它的 UUID，OHOS 上也能交给 DRM Kit，Android 上则不会被误判成"平台认" |
| **DASH-IF SEA（`urn:mpeg:dash:sea:2012`）** | **不做** | SEA 有自己的密钥分发（密钥在清单里），不是 CENC。已登记为 `Kind::Sea` 且 `cencCapable = false`，所以现场日志会明确说"这不是 CENC 方案"，而不是让 `CENCDecrypter` 去试 |
| **MPEG-2 TS 条件接收（`urn:mpeg:dash:13818:1:CA_descriptor:2011`）** | **不做** | 那是 TS 层的加扰（`CA_descriptor` + ECM/EMM），与本工程的 fMP4/CENC 通路完全不同 |
| **CENC 离线/持久化许可证** | **不做** | `framework/drm/` 只有在线会话；离线需要各平台 CDM 的 offline license 通路。一期用静态密钥文件已经天然支持离线（见 `docs/DRM-ENCRYPTION-DESIGN.md` §5.3） |
| **`#EXT-X-SESSION-KEY` 解析** | **不做** | 见 `docs/DRM-COMPLETENESS-AUDIT.md` §3 |

---

## 9. 怎么自己验一遍

```bat
:: 1) 方案覆盖 + 选优 + 许可证解析（不需要片源，55 条断言）
tools\drm_bench\README.md  §4  的编译命令
:: 期望末行： PASS: 55 checks, 0 failures

:: 2) 整个 Windows 树编过、零 error 零 warning
cmdline\msvc_build\build_win.bat x64

:: 3) 密钥服务器的 ClearKey 许可证
cd server\drm-keyserver
go test ./...
:: 期望： ok  drm-keyserver
```

**必须真机才能做的**（本机无法验证，列出来是为了别把"没验"当成"验过了"）：
Android 的 Widevine/PlayReady/ClearKey 硬解、OHOS 的 DRM Kit、
以及"CENC 分片 + MPD 走内核 DASH 管线出帧"这条全链路（需要真片源 + 出帧判据）。
