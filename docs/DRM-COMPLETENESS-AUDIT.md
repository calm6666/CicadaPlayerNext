# DRM 收口审计：做完了没有、哪些位置还空着

> 目的：回答"DRM 是否已经完全做完、有没有缺的位置"。
> 每条都带**文件:行号**证据，并区分三类：
> **【已实现并实测】** / **【已实现未实测】** / **【未实现】**
> 最后给一份**按可验证性排序**的收口清单。
>
> 本文是"审计"，不是"设计"；设计与施工记录仍在
> `docs/DRM-ENCRYPTION-DESIGN.md`（一期 §4.8、二期 §4.9）。
>
> **【本轮订正】** DASH 侧那几条（硬解/软解的判据、方案的识别范围）已经**重做**，
> 完整设计与证据见 `docs/DASH-DRM-SCHEMES.md`。本文原来的"§1 硬解优先/软解兜底"
> 一行与 §2 的几处描述已被那份文档取代，下面凡出现的地方都标了【订正】。

---

## 0. 一句话结论

**"能加密、能解密、能播"这条主链路已经打通并逐环实测过；但没有完全做完。**
缺的位置分两类：

* **可实现、我这边能验的**：3 条（见 §4 清单 A/B/C）；
* **需要真机或 Apple/OHOS 工具链、本机无法验证的**：4 条（见 §4 清单 D~G）。

其中**真正影响"DRM 完不完整"的是 D 与 E**（真机硬解、CENC-in-DASH 全链路），
其余是覆盖度问题，不是链路缺口。

---

## 1. 【已实现并实测】—— 主链路的每一环

| 环节 | 实现位置 | 实测证据 |
|---|---|---|
| **Go 密钥服务器**（铸钥/持久化/裸密钥/签名 URL/CENC 许可证） | `server/drm-keyserver/`（纯标准库） | **27 个单测全过**（含 NIST SP 800-38A F.5.1 官方向量）；实测 `POST /admin/keys` → 201、`GET /key/{kid}` → 200/16 字节、未签名 → 401、签名后 → 200 |
| **HLS AES-128 整片加密**（转码脚本） | `转码脚本/transcode_all.py`（`load_hls_key_info` / `run_encryption`）+ `aes128.py`（纯 Python FIPS-197，带官方向量自检） | 用服务器铸的密钥加密真分片：明文 254675 → 密文 254688（PKCS7 +13）、`%16==0`、头 12 字节与明文不同；独立解密后**逐字节等于明文** |
| **AES-128 解密（内核）** | `AES_128Decrypter` + `HLSStream::updateKey/updateSegDecrypter/updateIV` | 同上（同一份实现既加密也验收） |
| **内核 AES-128 状态机三个真 bug** | `HLSStream.cpp`：`createDemuxer` 每片独立判定 + `clearDecrypterState`；`readSegment`/`seekSegment` 统一用 `hasActiveDecrypter()`；`updateDecrypter` 放掉样本级解密器 | 见 `docs/DRM-ENCRYPTION-DESIGN.md` §4.5 与本仓提交历史 |
| **HLS `KEYID` 解析** | `HlsParser.cpp::hlsKeyIdOf()`（三种 METHOD 都解析，小写 hex，与 DASH 的 `cenc:default_KID` 同口径） | 改动前 `SegmentEncryption::keyId` 恒空 ⇒ `Stream_meta.drmKeyId` 恒 nullptr |
| **CENC 软件解密器** | `framework/demuxer/sample_decrypt/CENCDecrypter.{h,cpp}` | **Bento4 `MPEG-CENC` 真产物：150/150 样本逐字节解回明文，PASS**；ffmpeg 产物 149/150（唯一例外见 §5） |
| **CENC 接进真实读取路径** | `demuxer_service::setCencKey/hasCencKey/clearCencKeys` + `readPacket` → `applyCencDecryption` | **运行时实测**：不注册密钥 ⇒ 150/150 包原样放行、0 个等于明文（**不会抢走 CDM 那条路**）；注册密钥 ⇒ 150/150 逐字节等于明文 |
| **DASH `ContentProtection` 解析** | `Representation::ContentProtection` + `MPDParser::parseContentProtections()`（AS/Rep 两层，只取直接子节点） | **用仓库自己的 `MPDParser` 解析手写 MPD，全部断言 PASS**：继承、Rep 覆盖 AS、`cenc:pssh`、带连字符 KID、空占位被忽略、**未受保护的音轨一条都不挂** |
| **硬解优先 / 软解兜底的判据** | 【订正，本轮重做】`framework/drm/DrmSchemes.{h,cpp}`：DASH-IF 登记表全量（31 条）+ 归一化 + 选优；`DashStream::drmDecision/applyDrmFromContentProtection/fetchSoftwareCencKey` 按它决定"谁来解" | 判据无平台宏；**只在该平台 CDM 认这个 scheme 时**才把 DRM 参数交给解码器 —— 否则 `codecPrototype::create` 的 `drmInfo == nullptr \|\| codec->is_drmSupport(drmInfo)` 会把**所有软解解码器**排除（`avcodecDecoder::is_drmSupport` 恒 false）。详见 `docs/DASH-DRM-SCHEMES.md` §5 |
| **DASH DRM 方案覆盖** | 【订正，本轮新增】`DrmSchemes` 认得登记表里 **28 个保护系统 UUID + 3 个通用方案**，并区分"是不是 CENC 内容" | `tools/drm_bench/drm_schemes_check.cpp`：**55 条断言全过**（`PASS: 55 checks, 0 failures`） |
| **按需取内容密钥** | 【订正，本轮新增】`demuxer_service::setCencKeyResolver/resolveCencKey` + `ContentKeyFetcher`（裸 16 字节 / W3C ClearKey JSON / hex JSON 三种形状） | 每个 KID 只问一次；解决"清单只写 pssh、KID 只在 init 段 tenc 里"时**永远登记不上软解密钥**的问题（原来 `meta->drmKeyId` 根本没有代码去填） |
| **ClearKey DASH 的许可证** | 【订正，本轮新增】`server/drm-keyserver/clearkey.go` + `?format=clearkey`（`/key/{kid}` 与 `/license/{kid}`） | Go 单测新增 3 个（base64url 字母表、无填充、字段不串味、`scheme=` 冲突报 400、默认裸字节形状不变） |
| **软件解密性能** | `tools/drm_bench/drm_bench.cpp` + `core_share.py` | CENC `cenc` 656 MB/s ⇒ 1080p/8Mbps 占单核 **0.15%**、4K/100Mbps **1.9%**；HLS AES-128 3250 MB/s。软解不是瓶颈 |
| **curl `_T` 弃用迁移**（数据源，非 DRM 但影响取密钥） | `CURLConnection{.cpp,2.cpp}`、`curl_data_source{,2}.cpp` | **端到端实测**：真数据源经 HTTP 读出 `1048576`（与服务器 Content-Length 一致）、range 读正常 |

---

## 2. 【已实现未实测】—— 代码在位，但没有真机/真片源验证

| # | 位置 | 现状 | 为什么没验 |
|---|---|---|---|
| a | **Android Widevine / PlayReady / ClearKey 硬解** | 【订正，本轮扩到三家】通路完整：`SMPAVDeviceManager.cpp:49-59` 组 `DrmInfo` → `mediaCodecDecoder.cpp:258` 建 DRM 会话 → `setDrmInfo` → `queueSecureInputBuffer`；C++ 侧 `WideVineDrmHandler::is_supported` 认三家 UUID，Java 侧 `DrmSessionManager` 按 keyFormat 选 `MediaDrm` 的 UUID 并用 `isCryptoSchemeSupported` 做设备能力判定；`pssh`/`keyId` 现在真的传进 Java 了（JNI 签名从 2 个 String 改成 5 个） | **需要真机 + 授权内容**。本机只有 Windows；模拟器通常没有 Widevine L1/L3 授权。**Java 侧改动只做过 `javac`（android-36）类型检查，没有跑过真机** |
| b | **OHOS DRM Kit** | 【订正，本轮修了一个恒假条件】`framework/drm/OHOS/OhosDrmHandler.cpp` 认 Widevine/PlayReady/FairPlay/ClearKey 四个 UUID。原来比的是**裸 UUID**（`"edef8ba9-…"`）而全工程口径是 `urn:uuid:…` ⇒ `is_supported` 恒假 ⇒ 这套代码**一次都没被触发过** | 需要 OHOS 真机（本机有 DevEco，但 DRM 依赖设备能力） |
| c | **iOS/macOS FairPlay** | **未实现**：`AppleVideoToolBox.cpp` 只有 `is_drmSupport` 钩子，没有任何 `AVContentKeySession` 代码 | 需要 FairPlay 证书 + 真实许可服务，且 Apple 要求走它自己的渲染路径（本工程 Apple 侧是自绘 VTB）。见 `docs/DASH-DRM-SCHEMES.md` §8 |
| d | **CENC-in-DASH 全链路播放** | 解析、映射、注册、解密各环都单独验过；**【订正】本轮修掉了三个会让它必然失败的真 bug**：(1) 无条件写 `keyFormat` 使 `codecPrototype::create` 排除所有软解解码器；(2) `meta->drmKeyId` 从来没有代码填，"清单没写 default_KID"的片源登记不上密钥 ⇒ 改成按需取密钥；(3) `ManifestDemuxer` 把 CENC 记成 HLS 的 `AES_SAMPLE`（会拿错解密器把流解坏） | **但依然没串起来播过。** 需要把 CENC 分片 + MPD 喂进内核的 DASH 管线并出帧；本机没有现成 harness（`framework/tests` 需 `-DBUILD_TEST=ON`，Windows 下被 `framework/windows.cmake:119` 关掉），也没有真 CENC 片源 |
| e | **转码脚本的 `--drm-keyserver` 全流程** | 脚本侧已实现（铸钥 → 取回 16 字节 → 写 key_info → 写清单） | 脚本硬编码依赖 `input.mp4`/固定产物布局，需要准备完整输入目录 |

---

## 3. 【未实现】—— 有意的取舍，不是遗漏

| # | 项 | 判断 | 理由 |
|---|---|---|---|
| f | **`#EXT-X-SESSION-KEY` 解析** | **不做（至少现在不做）** | **【已证实】** `HlsTags.h:80-91` 的 `AttributesTag` 枚举里**没有** SESSION-KEY（只有 `EXTXKEY`、`EXTXMAP`、`EXTXMEDIA`…），`HlsTags.cpp:290` 的映射表里也没有。但**加了也不改变播放行为**：`#EXT-X-SESSION-KEY` 只出现在 **master** 播放列表里、是给播放器"预取密钥"的提示，而**真正的密钥入口永远是媒体播放列表里的 `#EXT-X-KEY`**（内核读的是那一条）。而且 `playList.h` 上**没有**可以挂 master 级密钥的字段（全文件只有 `class playList : public Dash::IDashUrl`），要加就得新加字段+新加标签+新加映射，**收益是 0 行行为变化**。所以这是"报告里可以写、代码里刻意不加"的一条 —— 写在这里是为了让下一个人知道这是**判断**，不是漏掉。 |
| g | **CENC `cbcs` 的 CTS 变体** | 已实现 **CBC-CS3**，但**无一手规范可引** | ISO/IEC 23001-7 原文在本环境抓不到（付费墙），CTS 变体（CS1/CS2/CS3）是按推理选的。已在 `docs/CENC-DECRYPTION.md` 与代码注释里显式标注为判断取舍。 |
| h | **CENC 离线许可证（persistent license）** | 未实现 | `framework/drm/` 只有在线会话；离线需要各平台 CDM 的 offline license 通路。一期用静态密钥文件已经天然支持离线（`docs/DRM-ENCRYPTION-DESIGN.md` §5.3）。 |

---

## 4. 收口清单（按"我这边能不能验"排序）

**A. 转码脚本 `--drm-keyserver` 全流程实跑**（清单 e）—— **【本轮已完成，见 §6】**
   2026-10-03 实跑：编排打通，11 个密文分片 + 全套清单产出，openssl 独立解密逐字节一致、
   密钥与清单 URI 逐字节一致；过程中修掉 2 个脚本 bug，并查出 3 处验收器误报（详见 §6）。
   **剩下的收尾**：把那 3 处误报修掉（否则它作为门禁不可信），然后再把这条当"门禁"用。
   （原计划文字保留如下）
   需要：准备 `input.mp4` + 输出目录布局，起 Go 服务器，跑
   `python transcode_all.py --hls-key-info <f> --drm-keyserver http://127.0.0.1:<port>`，
   然后用 `verify_segmentbase.py --expect-encrypted --key-file <f>` 验收。
   **价值最高**：它是"转码 → 密钥服务器 → 播放器"里唯一还没跑过的**编排**环节
   （各环都单独验过）。

**B. CENC-in-DASH 走内核 DASH 管线出帧**（清单 d）
   需要：Bento4 产 CENC 分片 + 手写 MPD（带 `ContentProtection` 与自建 key URL），
   再接一个 H.264 解码确认出帧。**这是"DRM 完不完整"的真正判据**，但工作量与
   风险都大于 A。

**C. 内核 AES-128 解密器对 ffmpeg 产物的 149/150 例外**（§5）
   已定性到"不是计数器语义问题"，但要彻底钉死需要第二个独立实现交叉验证。

**D. Android 真机 Widevine 硬解回归**（清单 a）—— 只能你在真机上做。

**E. OHOS DRM Kit 回归**（清单 b）—— 同上。

**F. FairPlay 实现**（清单 c）—— 需要证书与许可服务，属新立项。

**G. 【本轮已收口的部分】**
   · **DASH 方案覆盖**：`framework/drm/DrmSchemes.{h,cpp}`（登记表 31 条 + 归一化 + 选优）
     —— 连同"选优、许可证形状"的 55 条断言，`tools/drm_bench/drm_schemes_check.cpp`
     **全过**；
   · **按需取密钥**：`demuxer_service::setCencKeyResolver` + `ContentKeyFetcher`
     —— 解决"清单只写 pssh 就取不到密钥"；
   · **ClearKey 的许可证**：Go 服务器 `?format=clearkey`（W3C 形状）+ 3 个新单测；
   · **逐片 `#EXT-X-KEY`**：`转码脚本/transcode_all.py`（`_segment_anchors` /
     `_insert_per_segment_key_lines` / `_write_media_playlist`）+ 验收器
     `verify_segmentbase.py` 的逐片口径，另有离线自检
     `转码脚本/_hls_key_selfcheck.py`（3 种清单形态 + 反向对照，**PASS**）；
   · **HLS 的平台判定**：`DrmUtils::isSupport`（Android-only、只认 Widevine 一条、
     带平台宏）**删除**，改问平台无关的 `DrmHandlerPrototype::isSupport`
     —— 顺带修掉"OHOS 上连它自己实现的四种 DRM 都被跳过"。

---

## 5. 已知的最后一块不确定：ffmpeg CENC 的那个样本

用 `tools/drm_bench/cenc_verify.cpp`（四个竞争模型对照 + 计数器扫描）定性到：

* 150 个样本里 149 个逐字节解对；
* 唯一例外是**唯一一个多 subsample 的样本**（`5 clear+593`、`5 clear+6929`）：
  第一个受保护区间对，**第二个区间用"连续"(counter 38)与"重置"(counter 0)都复现不出**，
  把第二区间起始 counter 从 30 扫到 50 **没有任何命中值**；
* 也就是说该区间用的**不是"同一把 IV + 某个 counter"**，属 **ffmpeg 那个 muxer**
  在多 subsample 上的未定行为。

**影响面**：单 subsample 样本两种写法等价，而 149/150 都是这一类。
**Bento4 产出的 150/150 全对**，且它的样本是**单 subsample**
（验证器会打印形状普查：`150 single-range, 0 multi-range`）——
所以"多 subsample 路径"至今**没有**被一个权威产物验证过（这也是清单 B 值得做的原因）。

---

## 6. 清单 A 实跑记录（2026-10-03）：编排打通了，同时挖出 2 个脚本 bug + 3 个验收器误报

### 6.1 怎么跑的、跑出了什么

| 项 | 值 |
|---|---|
| 工作目录 | `D:\hilihili\.dsh_tmp\drmflow\A3`（仓库外临时目录） |
| 输入 | `input.mp4` = ffmpeg `testsrc2` 12 秒 720p30 视频 + `sine` 音轨（4.5 MB） |
| 密钥服务器 | `server/drm-keyserver`：`go run . -addr 127.0.0.1:9101 -data-dir <tmp>`（demo 模式，无 secret） |
| 命令 | `python transcode_all.py --hls-key-info key_info.txt --drm-keyserver http://127.0.0.1:9101` |
| 产物 | 11 个密文分片 · 5 条媒体列表（每条**逐片** `#EXT-X-KEY`）· `master.m3u8`（含 `#EXT-X-SESSION-KEY`）· `output.mpd`（5 个 AdaptationSet + `ContentProtection`）· `output-segmentbase.mpd` · 6 个 JSON 清单 · `_clear/` 明文对照 |

**关键独立证据：openssl 当预言机**（用清单里那个 URI 背后的**服务器密钥** + 清单声明的**逐片 IV** 解交付的密文，比对 `_clear/`）：

| 分片 | 明文 md5 | openssl 解出 md5 | 明文/密文字节 |
|---|---|---|---|
| `…video-h264-1280_720-1.m4s` | `55d9249e586fc20442377a4d2698ad60` | **同** | 2404522 / 2404528 |
| `…video-h264-1280_720-2.m4s` | `36f2f18af0e6013574f0fd3240da7696` | **同** | 2278271 / 2278272 |
| `…audio-1.m4s` | `20baefa9b01053e46e6af8319ac23040` | **同** | 93072 / 93088 |
| `…audio-3.m4s` | `d68a4490c75ebbc675c866eb1ec7190e` | **同** | 1473 / 1488 |

⇒ `AES-128-CBC + PKCS7 + 逐片 IV(…01/…02/…)` 的交付密文，能被一个**独立实现**逐字节解回明文；
密文长度全部整除 16（PKCS7 补 1~16 字节）✓。

**密钥一致性（红线）**：`GET /key/125ee3969fdcab16bf7247c1aa43474e` 返回的 16 字节与脚本落地的那把
**逐字节相同**（`b9b004c135d3270ee2921cac55ea1878`）⇒「清单 URI 指向的密钥 == 实际加密用的密钥」成立。

**验收器里 DRM 相关的断言全过**：A6（密文对照 11/11 都在 `_clear/`）、A9（JSON 的
`encryption.keyUrl/iv` 与 `#EXT-X-KEY` 同源）、A10（segmentbase single 清单确实**没有** encryption）、
A12（`master.m3u8` 的 `#EXT-X-SESSION-KEY` 与媒体列表一致）。

### 6.2 脚本端两个真 bug（本轮都已修）

1. **`convert-to-manifest.py` 落的是末片 IV ⇒ 交付被红线阻断**
   `parse_media_m3u8()` 里每遇到一条 `#EXT-X-KEY` 就 `result['encryption'] = enc` 覆盖；
   逐片写 KEY 之后，留下的一定是**末片**的 IV。而契约是「JSON 每路清单只有一个 `encryption.iv`，
   对应**首片**」（`transcode_all.py` §4.6 A9 与 `_review_json_manifests` 的注释）。
   实测报错：`test-dash-v4.json: encryption.iv='…0002' 与媒体播放列表 #EXT-X-KEY 里的 IV ['…0001'] 不一致`
   —— 脚本"宁可不交付"的行为是对的，错的是这一段。
   修法：只在**第一条** KEY 时赋值。★判据必须写 `result['encryption'] is None`，
   **不能**写 `'encryption' not in result`：该函数开头就把 `'encryption': None` 预置进了 result，
   那样写恒假 ⇒ 一条都不赋值 ⇒ 清单里干脆没有 `encryption`（这个坑本轮实测踩过一次）。
2. **非交互运行必然 `EOFError`，把成功报成失败**
   结尾无条件 `input("按 Enter 键退出...")`；CI / 重定向没有 stdin ⇒ 产物齐全、退出码却是 1。
   修法：`if sys.stdin.isatty(): input(...)`（这不是开关，是"有没有人在看"）。

### 6.3 验收器 `verify_segmentbase.py` 三处误报（是判据错，不是产物错）

| # | 报错 | 实测反证 |
|---|---|---|
| 1 | `解密验证不一致 … PKCS7 … 密文长度 2404522 不是 16 的倍数` | **2404522 是明文长度**；密文是 2404528（整除 16、补 6 字节）。openssl 独立解密逐字节一致（§6.1）⇒ 它把明文长度当成了密文长度 |
| 2 | `Initialization 缺失 … #EXT-X-MAP BYTERANGE 推断 [0-855] 与 initialization [0--] 不一致` | box 遍历实测**单文件**：`ftyp(32)+moov(824)=856 → sidx@856(64B) → moof@920`，与 MPD 的 `indexRange="856-919"` 严丝合缝；audio 同理（`769-844`，`ftyp(32)+moov(737)`）✓。它是拿**独立 init 文件**（840 B，另一套 box 布局：ftyp 28 + moov 812）去比**单文件**的 indexRange —— 两个文件各自自洽 |
| 3 | `有分片共用同一个 IV（4 个 video rendition 的第 1 片都是 …01）` | 按脚本 §4.1 约定 `IV = 基 IV + 分片序号`，**不同 rendition 的同一序号本来就该是同一个 IV**；同一 rendition 内逐片唯一 ✓。这条判据与约定冲突，得先定口径再改 |

**这三条必须先修**：它既是清单 B（B6 门禁）的基准，也是 1B/2/3 的验收基准 —— 基准自己误报，
后面所有"通过"都不可信。

### 6.4 复现（一条不差）

```powershell
# 1) 工作目录 + 输入
mkdir D:\tmp\drmflow; cd D:\tmp\drmflow
ffmpeg -y -f lavfi -i "testsrc2=size=1280x720:rate=30" -f lavfi -i "sine=frequency=440" `
       -t 12 -c:v libx264 -preset veryfast -b:v 3000k -pix_fmt yuv420p -g 60 `
       -c:a aac -b:a 128k -shortest input.mp4
# 1b) 初始 key_info（3 行：URI / 密钥文件路径 / 可选 IV）+ 16 字节本地密钥
#     —— 脚本 main() 先 load_hls_key_info() 再换成服务器那把，所以这份文件必须先存在

# 2) 密钥服务器（另一终端）
cd <repo>\server\drm-keyserver; go run . -addr 127.0.0.1:9101 -data-dir D:\tmp\drmdata

# 3) 转码 + 铸钥 + 加密
python <转码脚本>\transcode_all.py --hls-key-info key_info.txt --drm-keyserver http://127.0.0.1:9101

# 4) 验收
python <转码脚本>\verify_segmentbase.py . v4 --expect-encrypted --key-file <16字节密钥文件> --clear-dir _clear

# 5) 独立复核（本轮的判据）——随机挑几片用 openssl 解，和 _clear/ 比 md5
openssl enc -d -aes-128-cbc -K <32位hex密钥> -iv 00000000000000000000000000000001 `
        -in <片>.m4s -out out.bin
```
