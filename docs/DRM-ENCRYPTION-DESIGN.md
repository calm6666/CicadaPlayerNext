# 转码产物加密（DRM）与播放器解密播放 —— 设计文档

> 目标：回答"能不能在转码脚本里加上加密、把产物变成 DRM 文件、播放器能解密播放"，
> 并给出一份可直接照着施工的设计。
>
> **本文最初只写文档、不改任何代码；一期施工完成后做了"施工后回写"**（见 §4.8）。
> 所有"现状"论断都标注 `文件:行号`，并区分：
> * **【已证实】** —— 本次真的读过该文件该行；
> * **【推测/待验证】** —— 从上下文推断、或取决于外部工具版本/运行环境，施工前必须先验证。
>
> ⚠ **行号有两套**：§2 / §7 里带 `[施工前]` 或未标注的是**设计初稿**写下的行号；
> 一期施工后 `转码脚本/transcode_all.py`、`verify_segmentbase.py`、
> `hls.mjs` 都发生了变化，**施工后的真实行号见 §4.8.1 与 §7.3**。
> **哪些是实测、哪些仍是待验证，看 §4.8.7（待验证清单，每条附最小验证命令）。**
>
> 涉及三个代码域：
> * **内核** `D:\hilihili\CicadaPlayerNext`（C++）——**一期零改动**
> * **web 前端** `D:\hilihili\front\hili-player`（TypeScript + vendored hls.js/dash.js）
> * **转码脚本** `D:\hilihili\转码脚本`（Python）

---

## 目录

1. [结论与可行性](#1-结论与可行性)
2. [现状盘点（全部带 file:line 证据）](#2-现状盘点全部带-fileline-证据)
3. [加密方案对比](#3-加密方案对比)
4. [一期落地设计](#4-一期落地设计)
5. [风险与边界](#5-风险与边界)
6. [明确不能做的事](#6-明确不能做的事)
7. [附录：证据索引](#7-附录证据索引)

---

## 1. 结论与可行性

**结论：可以实现。** 而且这个工程比一般项目更容易做到 —— 因为**解密侧的通路基本已经写好了**，
缺的是**转码侧一行加密都没有**、以及**清单里没有任何加密声明**。

拆成三条路线：

| 路线 | 是什么 | 难度 | 适用面 | 结论 |
|---|---|---|---|---|
| **A. HLS AES-128 整片加密** | `#EXT-X-KEY:METHOD=AES-128`，整个分片字节做 AES-128-CBC | **低** | 内核（HLS 管线 + 对象清单，两路都已实现）；web 侧**有一个必修的缺口**（见 §2.2.3，**已完成**） | **一期推荐**（**已落地**，见 §4.8） |
| **B. CENC `cenc-aes-ctr`** | 逐采样加密 + `ContentProtection`/`SAMPLE-AES` 声明 | **中～高** | 内核对象清单已能把 `ContentProtection` 转成 `AES_SAMPLE`；但最终解密要落到**平台 DRM/CDM**（Android Widevine、OHOS DRM Kit）——**桌面 Qt 无 CDM**（§2.1.5） | **二期** |
| **C. 平台 DRM（Widevine/PlayReady/FairPlay）** | PSSH + CDM + 许可证服务器 | **高** | Android / OHOS 有实现；iOS FairPlay 只有 `keyFormat` 通道；**桌面 Qt 现实约束大** | 三期（或跟随平台能力推进） |

### 一期为什么推荐 A

1. **内核侧零改动就能播**（这是最大的理由）：
   * HLS **文本**片源路径已经完整实现：解析 `#EXT-X-KEY`（`HlsParser.cpp:294-355`）→
     拉 16 字节密钥（`HLSStream.cpp:789-833`）→ IV 处理（`HLSStream.cpp:835-853`）→
     分段 AES-128-CBC 解密 + PKCS7 去填充（`AES_128Decrypter.cpp:27-101`）。
   * **对象清单**（`test-*.json`）路径也已实现：`ManifestDemuxer::buildSegmentEncryptions()`
     把 `encryption` 字段转成 `SegmentEncryption{AES_128,...}`（`ManifestDemuxer.cpp:142-163`）。
   * 更关键的是：**对象片源在内核里一律走 HLS 分片管线**
     （`ManifestDemuxer.cpp:579-583`：`mediaSourceType == "dash"` 只打一条 WARN 就照样 `new HLSManager`），
     所以对象清单里的加密声明**走的正是上面那条已被验证的解密路径**。
2. **转码侧的工具链最短**：~~AES-128 可以由 ffmpeg 在**同一个 muxer 里**顺手做掉~~ ——
   **【施工后更正】** 这条前提**不成立**：本工程的主 muxer 是 `-f dash`（`-hls_playlist 1`），
   `-hls_key_info_file` 不适用；而另起一趟 `-f hls` 又只能给整条播放列表一个 IV。
   一期最终由**脚本自己做字节级 AES-128-CBC**（原语来自标准库的 OpenSSL 后端），
   **不引入任何新外部工具、不动 sidx、不动主转码与再封装命令** ——
   "最短工具链"这个结论仍然成立，只是实现路径换了（详见 §3.1 第 3 条 / §4.8.3）。
3. **播放器兼容矩阵最宽**：AES-128 是 HLS 的"人人都能解"档；CENC 需要 EME/CDM。

**一期不推荐 B 的原因**：`ContentProtection` 声明写起来不难（清单工具都能写），
但**解密端要 CDM**；本工程桌面 Qt 构建里**一个 DRM handler 都没编进去**（§2.1.5），
一期做 B 会得到"文件确实是 CENC 加密的，但桌面播放器播不了"的结论 —— 属于把风险前置到最贵的地方。

> **一句话口径**：一期做"**AES-128 + 清单声明 + 密钥分发 + 验收脚本**"，
> 让整条链路（转码 → 清单 → 内核播放 → web 播放）真正跑通；
> CENC/平台 DRM 作为二期，等"确实需要硬件级保护"时再上。

---

## 2. 现状盘点（全部带 file:line 证据）

### 2.1 内核侧（`D:\hilihili\CicadaPlayerNext`）

#### 2.1.1 `segment::encryptions` / `SegmentEncryption` 有哪些字段

**【已证实】** `framework/demuxer/play_list/segment_decrypt/SegmentEncryption.h:13-33`：

```cpp
class SegmentEncryption {
public:
    enum encryption_method {
        NONE,          // :19
        AES_128,       // :20   整片加密（HLS METHOD=AES-128）
        AES_SAMPLE,    // :21   样本级加密（HLS SAMPLE-AES / DASH CENC）
        AES_PRIVATE,   // :22   私有方案（HLS METHOD=AES-PRIVATE + DATE）
    } method;                       // :23

    std::string keyUrl;             // :25   密钥/许可证 URL
    std::vector<uint8_t> iv;        // :26   IV 字节
    std::string keyFormat;          // :27   HLS KEYFORMAT / DASH schemeIdUri
    bool ivStatic = false;          // :28   true = 清单给了显式 IV
    std::string pssh;               // :31   base64 PSSH（注释 :29-30 说由 manifest demuxer 填）
    std::string keyId;              // :32   default_KID（UUID 字符串）
};
```

**【已证实】** `segment` 上挂的是**一个向量**（一个分片可以声明多套加密）：

* `segment.h:38` `void setEncryption(const std::vector<SegmentEncryption> &enc);`
* `segment.h:61` `std::vector<SegmentEncryption> encryptions;`
* 实现：`segment.cpp:45-48`。
* **构造函数默认 `method = NONE`**：`SegmentEncryption.cpp:7-9`（【已证实】）。

> 注意：`keyFormat` / `pssh` / `keyId` 是**已经加过**的字段（§2.1.4、§2.1.5 会用到），
> 不是本设计新造的。

#### 2.1.2 `HlsTags.*` 的 `#EXT-X-KEY` 解析

**【已证实】** `framework/demuxer/play_list/HlsTags.h:78-105` —— `AttributesTag` 的 tag 枚举里
`EXTXKEY = 20`（`HlsTags.h:81`）。

**【已证实】** `HlsTags.cpp:290` 把 `"EXT-X-KEY"` 映射到 `AttributesTag::EXTXKEY`
（`HlsTags.cpp:336` 是工厂里的分支）。

**【已证实】** `HlsTags.h:15-35` 的 `Attribute` 提供 `quotedString()`（去引号）、
`hexSequence()`（`0x...` → 字节数组）、`decimal()`／`floatingPoint()`。

**【已证实】** **没有** `#EXT-X-SESSION-KEY`：全仓 grep `SESSION-KEY` 只命中
`framework/demuxer/HLSSupport.md:37` 的一句"待支持"链接，**没有任何解析代码**。
`HlsTags.h:78-91` 的枚举里也没有对应的 tag 类型。

**【已证实】** **没有** `#EXT-X-KEY:METHOD=NONE` 的语义处理：`HlsParser.cpp:346-351`
把未知/不合法做法一律落到 `method = NONE`，但 `HlsParser.cpp:267-270` 是
"`encryptionArray` 非空就 `setEncryption()`" —— 也就是 **`METHOD=NONE` 也会被当成一条
encryption 记录挂到分片上**。

#### 2.1.3 `HlsParser` 怎么读 `#EXT-X-KEY`

**【已证实】** `framework/demuxer/play_list/HlsParser.cpp:294-355`：

| 做法 | 条件 | 结果 |
|---|---|---|
| `AES-128` | 有 `METHOD` 且 == `"AES-128"` 且**有 `URI`** | `method=AES_128`；`keyUrl = URI`（`quotedString()`）；有 `IV` 则 `iv = hexSequence()` 且 `ivStatic=true`（`:304-315`） |
| `AES-PRIVATE` | `METHOD == "AES-PRIVATE"` 且有 `DATE` | `method=AES_PRIVATE`；`keyUrl = DATE`（`:316-326`） |
| `SAMPLE-AES` | `METHOD == "SAMPLE-AES"` 且有 `URI` | `method=AES_SAMPLE`；`keyUrl = URI`；`IV` 同上；再读 `KEYFORMAT` → `keyFormat`（`:327-344`）。**不读 `KEYID`** |
| 其它 | — | `method = NONE`，`keyUrl=""`，`iv.clear()`（`:346-351`） |

**【已证实】** 挂载时机：`HlsParser.cpp:267-270`（遇到分片 URI 时把当前
`encryptionArray` 拷给该分片）；`HlsParser.cpp:296-299` 用 `clearKeyArray` 保证
"新的一组 `#EXT-X-KEY` 到了就清空上一组"。

> **施工含义**：`#EXT-X-KEY` 必须是**逐分片生效**的（HLS 语义），
> 因此"整条播放列表用一个 key"时，把 `#EXT-X-KEY` 写在**第一个 `#EXTINF` 之前**即可；
> 解析器会把它带给后面所有分片。

#### 2.1.4 `HLSStream` / `PlaylistManager` 怎么用密钥与 IV

**【已证实】** 选一条加密记录：`framework/demuxer/play_list/HLSStream.cpp:545-550`

```cpp
for (SegmentEncryption &item: mCurSeg->encryptions) {
    if (item.keyFormat.empty() || DrmUtils::isSupport(item.keyFormat)) {
        mCurrentEncryption = item;
        break;
    }
}
```

> `keyFormat` 为空（AES-128 的标准情形）→ **无条件接受**；非空 → 必须是平台支持的 DRM 格式。

**【已证实】** 拉密钥：`HLSStream.cpp:789-833`（`updateKey()`）

* URL 由**播放列表基址**拼出来：`Helper::combinePaths(mPTracker->getBaseUri(), mCurrentEncryption.keyUrl)`（`:791-792`）；
* **`mKeyUrl == keyUrl` 就直接 return false（不重复拉）**（`:794-796`）—— 密钥轮换要靠 URL 不同来触发；
* 用普通 `dataSource` 打开并**读满 16 字节**，不足 16 报错返回（`:812-829`）。

**【已证实】** IV 处理：`HLSStream.cpp:835-853`（`updateIV()`）

* `ivStatic == true` → `return false`（不覆盖清单给的 IV，沿用 `SegmentEncryption::iv`）；
* `ivStatic == false` → **按分片序号构造 IV**：`iv.resize(16)`，把 `sequence` 的大端写进
  `iv[12..15]`（`:838-849`）。

**【已证实】** 两条解密路径的选择：`HLSStream.cpp:1080-1102`（`updateDecrypter()`）

```cpp
mProtectedBuffer = mCurrentEncryption.method != SegmentEncryption::NONE;          // :1083
if (method == AES_128 || method == AES_PRIVATE)  updateSegDecrypter();            // :1085-1088
else if (method == AES_SAMPLE && keyFormat.empty()) updateSampleAesDecrypter();   // :1092-1094
// method == AES_SAMPLE && !keyFormat.empty() ⇒ 走平台 DRM，这里什么都不做
```

* `updateSegDecrypter()`：`HLSStream.cpp:855-902`；AES-128 分支在 `:857-874`，
  把密钥/IV 塞给 `mSegDecrypter`（`AES_128Decrypter`，工厂见 `SegDecryptorFactory.cpp:14`）；
* `updateSampleAesDecrypter()`：`HLSStream.cpp:905-929`；构造并使用
  `HLSSampleAesDecrypter`（成员声明 `HLSStream.h:224`，成员 `mCurrentEncryption` 在 `HLSStream.h:254`）；
* 解密器只在 `createDemuxer()` 里挂给内层 demuxer：`HLSStream.cpp:573`
  `mPDemuxer->setSampleDecryptor(this->mSampeAesDecrypter.get());`
* **样本级加密的形态提示**：`HLSStream.cpp:597`
  `setStreamEncrypted(mCurrentEncryption.method == SegmentEncryption::AES_SAMPLE)`
  （长注释在 `:582-596` 解释了为什么整片解密要建 bsf、样本级解密不能建）。

**【已证实】** 元信息出口：`HLSStream.cpp:1517-1520`

```cpp
meta->keyUrl    = mCurrentEncryption.keyUrl.empty()    ? nullptr : strdup(...);
meta->keyFormat = mCurrentEncryption.keyFormat.empty() ? nullptr : strdup(...);
meta->drmPssh   = mCurrentEncryption.pssh.empty()      ? nullptr : strdup(...);
meta->drmKeyId  = mCurrentEncryption.keyId.empty()     ? nullptr : strdup(...);
```

（字段定义：`framework/utils/AFMediaType.h:438` 起，`keyFormat` 在 `:438`。）

**【已证实】AES-128 解密器实现细节**（决定"解密后能不能逐字节还原"）：

* 算法：`avAESDecrypt::decrypt()` → `av_aes_crypt(mAes, dst, src, count, iv, 1)`
  （`framework/demuxer/decrypto/avAESDecrypt.cpp:23-26`，`setKey` 用 `key_bits = 8*16 = 128`，`:28-31`）
  —— 即 **AES-128-CBC**；
* **按块解密、末尾去掉 PKCS7 填充**：`play_list/segment_decrypt/AES_128Decrypter.cpp:56-83`
  （`:79-83` `int padding = mOutBuffer[mOutData - 1]; mOutData -= padding;`）；
* 流式实现会**故意留最后一个块不吐**，直到 EOS（`:63-65`）——**这是流式行为，不是 bug**；
* **`seekSegment()` 在解密路径上直接返回 `-EINVAL`**：`HLSStream.cpp:206-215`
  （解密器不支持随机 seek，seek 靠"换分片重建"完成）。

**【已证实】AES_SAMPLE（样本级）能力边界**（对二期重要）：

* `HLSSampleAesDecrypter::decrypt()` 只处理 **H264** 与 **AAC**，其它 codec 原样返回
  （`framework/demuxer/sample_decrypt/HLSSampleAesDecrypter.cpp:54-77`）——
  **HEVC 不在里面**（本工程转码产物恰恰有 h265 档！）；
* 明文头部长度常量：`VIDEO_CLEAR_LEAD = 32`、`AUDIO_CLEAR_LEAD = 16`
  （`HLSSampleAesDecrypter.h:34-35`；使用处 `:178-179`、`:229`、`:267`）；
* 密钥无效时**只打日志并把密文原样返回**：`:56-59`
  `AF_LOGE("key info is not valid\n"); return size;` —— 也就是**不会抛错**，会往下游吐垃圾。

#### 2.1.5 DASH 侧 `ContentProtection` 与 `DrmUtils::isSupport`

**【已证实】`MPDParser` 完全不解析 `ContentProtection`**：

* grep `framework/demuxer/dash/` 下 `ContentProtection|pssh|default_KID|schemeIdUri` 只命中
  `MPDParser.cpp:231-238`，那是 **`UTCTiming`** 的 `schemeIdUri`，与 DRM 无关；
* `MPDParser::parseAdaptationSets()`（`MPDParser.cpp:343-384`）与
  `parseRepresentations()`（`MPDParser.cpp:386-433`）里**没有任何** DRM/`ContentProtection` 分支；
* 代码注释直接承认这一点：`framework/demuxer/dash/DashStream.cpp:394-406`
  > "DASH 侧今天没有任何 ContentProtection 解析（MPDParser 不产出加密信息，DashSegment 上也没有加密字段），
  > 所以这里如实按'非加密'处理…… `setStreamEncrypted(false)`"
* 结论：**URL 直连的 `.mpd` 片源，即使带 `ContentProtection`，内核也读不到加密信息**。

**【已证实】`DrmUtils::isSupport` 只认 Android Widevine，且只认一个 UUID**：

`framework/utils/DrmUtils.cpp:7-12`

```cpp
bool DrmUtils::isSupport(const std::string &drmFormat) {
#ifdef ANDROID
    return drmFormat == "urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed";
#endif
    return false;      // 非 Android 一律 false
}
```

* 调用点唯一：`HLSStream.cpp:546`；
* **后果**：非 Android 平台（桌面 Qt / iOS）如果清单里的 `ContentProtection` 用的是
  `schemeIdUri = urn:mpeg:dash:mp4protection:2011`（cenc 通用声明）或 ClearKey UUID，
  这条记录会被**跳过**；如果清单里**只剩**这一条，`mCurrentEncryption` 会保持默认的 `NONE` ⇒
  **不当作加密处理，直接解出来是垃圾/报解复用错误**。这是一个真实的"静默降级"风险点。

**【已证实】DRM handler 只有 Android 与 OHOS 两个实现，桌面 Qt 构建里一个都没有**：

* `framework/drm/CMakeLists.txt:11-15`：`WideVineDrmHandler.cpp` **只在 `if (ANDROID)` 里编**；
* `framework/drm/CMakeLists.txt:17-24`：`OHOS/OhosDrmHandler.cpp` 只在 `OHOS` 时编；
* `framework/drm/WideVineDrmHandler.cpp:110-113` 全是 JNI 方法注册（`native_requestProvision/requestKey/...`）；
* `framework/drm/OHOS/OhosDrmHandler.cpp:15` 整个文件包在 `#ifdef __OHOS__` 里；
* `DrmHandlerPrototype::isSupport()`（`framework/drm/DrmHandlerPrototype.cpp:25-36`）遍历的
  就是这些 handler 的原型队列 —— **桌面 Qt 上队列为空（`_nextSlot == 0`）⇒ 永远 false**。

**【已证实】消费入口已经接好了**（所以 Android/OHOS 端不用改）：
`mediaPlayer/SMPAVDeviceManager.cpp:49-59` 从 `Stream_meta` 组 `DrmInfo{format,uri,pssh,keyId}`，
`:96-97` 把 `DrmManager::require` 作为回调交给解码器。

**【已证实】错误码现状**：`framework/codec/IDecoder.h:46`
`#define STATUS_DRM_ERROR 1 << decoder_drm_error`，使用处 `framework/codec/ActiveDecoder.cpp:267`。
**没有**"密钥缺失/许可证失败"这类细分错误码 —— 失败会退化成通用 codec/demux 错误
（如 `codec_error_video_not_support`，`framework/utils/errors/framework_error.h:50`）。

#### 2.1.6 `framework/demuxer/manifest/`（对象清单）

**【已证实】数据模型**（`framework/demuxer/manifest/MediaManifest.h`）：

* `struct ContentProtection`（`:33-45`）：`schemeIdUri` / `value` / `pssh`（base64）/ `keyId` / `laUrl` / `pro`；
  注释里列了四种 schemeIdUri 取值（`:34-38`）；
* `struct LicenseServer`（`:47-52`）：`url` / `contentId` / `keyType`（默认 `"clearkey"`，注释列出
  `clearkey|aes128|widevine|fairplay|playready`，`:50-51`）；
* `struct Aes128Encryption`（`:54-60`）：`keyUrl` / `iv`（**hex 字符串，空 = 用序号当 IV**）/ `keyFormat` /
  `keyFormatVersions` / `expiresIn`（`0` = 永久密钥）；
* rep 级：`MediaRepresentation::encryption` + `hasEncryption`（`:151-152`）；
* 顶层：`MediaManifest::contentProtection`（`:246`）、`encryption` + `hasEncryption`（`:247-248`）、
  `licenseServer` + `hasLicenseServer`（`:249-250`）；
* **管线口径注释**：`:260-263` —— `mediaSourceType` 默认 `"hls"`，
  "The HLS pipeline is the DRM-capable path (EXT-X-KEY / CENC via SegmentEncryption)"。

**【已证实】parser 怎么读**（`framework/demuxer/manifest/MediaManifestParser.cpp`）：

* `parseContentProtection()`：`:39-47`（`schemeIdUri/value/pssh/keyId/laUrl/pro`）；
* `parseLicenseServer()`：`:49-54`（`url/contentId/keyType`，`keyType` 缺省 `"clearkey"`）；
* `parseEncryption()`：`:56-63`（`keyUrl/iv/keyFormat/keyFormatVersions/expiresIn`）；
* rep 级挂载：`:194-200`（有 `encryption` 就 `parseEncryption` 并 `hasEncryption = true`）；
* 顶层挂载：`:372`（`contentProtection`）、`:374-377`（`encryption`）、`:381-384`（`licenseServer`）。

**【已证实】转成 `SegmentEncryption` 的地方**：`ManifestDemuxer.cpp:138-187`
（`buildSegmentEncryptions()`）：

* AAC-128 分支（`:142-163`）：**rep 级优先于顶层**；要求 `keyUrl` 非空；
  `expiresIn > 0 && 有 licenseServer.url` → `keyUrl` 换成许可证服务器地址（`:147-149`）；
  `iv` 是 hex 字符串 → 两个字符一个字节（`:150-157`），并置 `ivStatic = true`；
  `keyFormat` 原样带走（`:158-160`）；
* CENC 分支（`:169-186`）：跳过 `schemeIdUri` 为空或等于 `urn:mpeg:dash:mp4protection:2011` 的记录
  （`:171-173`），其余产出 `AES_SAMPLE + keyFormat=schemeIdUri + pssh + keyId`，
  `keyUrl = laUrl ?? licenseServer.url`（`:176-183`），**`return`（第一条支持的胜出）**（`:185`）；
* 挂到分片：`ManifestDemuxer.cpp:432-450`（模板/列表）与 `:522-546`（SegmentBase）。

> **施工含义**：对象清单里**顶层** `encryption`（不是 rep 级）就够了 ——
> `buildSegmentEncryptions()` 会把它套给**所有** rep（`:142-144`）。

### 2.2 web 侧（`D:\hilihili\front\hili-player`）

#### 2.2.1 类型定义：`Aes128Encryption` / `LicenseServer` / `ContentProtection`

**【已证实】** `packages/plugins/src/vendor/types/manifest.ts`：

* `ContentProtection`：`:64-105`（`schemeIdUri` 必传，`:73`；`value`/`pssh`/`keyId`/`laUrl`/`pro` 选传；
  注释明确 "HLS: 映射为 `#EXT-X-KEY` 标签"（`:61`）、"HLS: 映射为 `#EXT-X-KEY:KEYID`"（`:90`））；
* `LicenseServer`：`:115-138`（`url` 必传 `:122`；`contentId` `:128`；`keyType` 缺省 `clearkey` `:137`；
  注释给了四种 URL 形状，`:117-120`）；
* `Aes128Encryption`：`:150-180`（`keyUrl` 必传 `:155`；`iv` 16 字节 hex `:161`；
  `keyFormat` 默认 `"identity"` `:168`；`expiresIn` >0 = 临时密钥 `:179`）。

**【已证实】** rep 级 `encryption`（`:1009`）与顶层 `encryption`（`:622`）都存在，
注释说明"rep 级优先于全局"（`:1005`）。

#### 2.2.2 HLS 侧映射：`applyEncryption`（`manifest-to-hls.ts`）

**【已证实】** `packages/plugins/src/vendor/manifest-to-hls.ts:90-108`：

```ts
function applyEncryption(result, encryption?, licenseServer?): void {
  if (!encryption) return;
  const isTemporaryKey = encryption.expiresIn !== undefined && encryption.expiresIn > 0;
  const keyUrl = (isTemporaryKey && licenseServer?.url) ? licenseServer.url : encryption.keyUrl;
  result.encryption = { keyUrl, iv: encryption.iv, keyFormat: encryption.keyFormat,
                        keyFormatVersions: encryption.keyFormatVersions };
}
```

* 两种模式都调它：单文件模式 `:226`、模板/列表模式 `:278`；
* 调用链：`manifestToHls()`（`:398-415`）→ `rep.encryption ?? manifest.encryption`（`:406`、`:411`）；
* 输出类型：`packages/plugins/src/hls/vendor/hls.d.ts:3483-3511`
  （`ManifestPlaylistDetails.encryption` 在 `:3500`；`ManifestEncryption` 在 `:3502-3511`）。

#### 2.2.3 ⚠️ **web HLS 对象注入路径的 AES-128 缺口（本次盘点最重要的发现之一）**

**【已证实】** vendored hls.js 的 `_buildLevelDetails()`（`packages/plugins/src/hls/vendor/hls.mjs:38934-38993`）
**只填了 live / targetduration / startSN / initSegment / fragments / totalduration / endSN / endCC**，
**从头到尾没有读 `playlist.encryption`** —— 既没有设 `details.key`，也没有给 `Fragment` 设
`decryptdata`。函数末尾就是 `details.endCC = 0; return details;`（`:38990-38992`）。

对照证据（同一个文件里，**文本 HLS 解析**路径是完整的）：

* `parseKey()` 定义在 `:8317`；调用点 `:7686`（`#EXT-X-SESSION-KEY`）与
  `:8055`（`#EXT-X-KEY`，赋给 level）；
* `Key` 类的 `getDecryptData(sn, levelKeys)`（`:7478-7484` 起）：`isFullSegmentEncryption(method)`
  为真时按序号推 IV —— 说明 **AES-128 的解密能力本身是有的**；
* 也就是说：**能力在，接线不在**。对象注入是这条 fork 自己写的新入口，AES-128 这条线漏了。

**【已证实】** 全局 grep `hls.mjs` 里 `encryption` 只命中 `:7379/:7384`（EME 的
`audioEncryptionScheme/videoEncryptionScheme` 默认值）、`:7458`（注释）、`:15029`（日志文案）、
`:17493/:17497`（一个局部变量 `encryptionType`）—— **没有一处把 `ManifestEncryption` 接进 `Key/decryptdata`**。

> **施工含义（一期必做）**：一期如果只在清单里写 `encryption` 而不打这个补丁，
> **web 对象注入播放加密流会得到"画面花屏/解码失败"**（密文被当明文解）。
> 缺口很小（在 `_buildLevelDetails` 里按 `playlist.encryption` 造一个 `Key` 挂到
> `details.key` / 每个 `frag.decryptdata`），**但必须显式列为一期的改动项**，不能漏。

#### 2.2.4 DASH 侧 `ContentProtection` 的映射

**【已证实】** `packages/plugins/src/vendor/manifest-to-dash.ts:414-433`
`buildClearKeyContentProtection(licenseServer)` —— **无论如何都会写两条**：

```ts
{ schemeIdUri: 'urn:mpeg:dash:mp4protection:2011', value: 'cenc' },              // :417-420
{ schemeIdUri: 'urn:uuid:e2719d58-a985-b3c9-781a-b030af78d30e' },                 // :422-425 ClearKey
// 有 contentId 时：protections[1]['cenc:defaultKId'] = licenseServer.contentId;   // :428-430
```

* 触发条件：`manifestToDash()` 里 `manifest.licenseServer ? buildClearKeyContentProtection(...) : undefined`
  （`:449-452`），然后挂到**每个视频 AdaptationSet**（`:473-475`）与音频（`:480-497` 一段）。
* **注意**：它只认 `licenseServer`，**不看 `manifest.contentProtection`** ——
  也就是 web 侧今天**无法表达 Widevine/PlayReady 的 PSSH**。
* rep 的 `encryption` 字段在 DASH 侧**被明确忽略**：注释 `manifest-to-dash.ts:328-330`
  "encryption（AES-128）是 HLS 专用的分片加密方式，DASH 不使用此字段"。

**【已证实】** dash 插件入口：`packages/plugins/src/dash/DashPlugin.ts:327`、`:369` 调 `manifestToDash()`。

### 2.3 转码侧（`D:\hilihili\转码脚本`）

#### 2.3.1 `transcode_all.py` —— 主转码 + SegmentBase 再封装 + 产物生成位置

**【已证实】** 关键常量（文件头配置区，`:22-102`）：

| 常量 | 行 | 值/含义 |
|---|---|---|
| `SEGMENT_DURATION` | `:44` | `6`（秒） |
| `IDR_INTERVAL` | `:45` | `2`（秒），硬约束：段长必须是它的整数倍（`:43`、`:2066`） |
| `CODECS` | `:50-53` | `h264/h264_nvenc/avc`、`h265/hevc_nvenc/hevc` |
| `RESOLUTIONS` | `:57-62` | 4K/1080p/720p/480p |
| `JSON_CONVERT_SCRIPT` | `:76` | `"convert-to-manifest.py"` |
| `JSON_VERSION` | `:77` | `"v4"` |
| `JSON_BASE_URL` | `:79` | `http://127.0.0.1:9000/video/dash2/` |
| `SEGMENTBASE_MPD_NAME` | `:92` | `"output-segmentbase.mpd"` |
| `SB_TMP_TEMPLATE` | `:93` | `"sb-$RepresentationID$.m4s"` |
| `FFMPEG_PATH` | `:96` | `"ffmpeg"` |

**【已证实】** 关键函数与产物落点：

| 函数 | 行 | 产物 |
|---|---|---|
| `build_ffmpeg_cmd()` | `:254-401` | 一条 ffmpeg 命令同时产出 |
| ↑ DASH 输出段 | `:384-400` | `-f dash`、`-init_seg_name $RepresentationID$-0.m4s`（`:386`）、`-media_seg_name $RepresentationID$-$Number$.m4s`（`:387`）、**`-hls_playlist 1`**（`:388`）、**`-hls_master_name master.m3u8`**（`:389`）、`-use_template 1`（`:390`）、`-use_timeline 1`（`:397`）、`-seg_duration 6`（`:398`）、输出 `output.mpd`（`:399`） |
| `run_transcode()` | `:403-429` | 起子进程、非零退出即 `return False`（`:419-421`） |
| `build_segmentbase_cmd()` | `:442-462` | **`-c copy` 纯再封装**（`:448`）、`-i output.mpd`（`:446`）、`-single_file 1`（`:451`）、`-single_file_name sb-$RepresentationID$.m4s`（`:452`）、`-global_sidx 1`（`:453`）、`-use_timeline 0`（`:457`）、**`-hls_playlist 0`**（`:459`）、输出 `output-segmentbase.mpd`（`:461`） |
| `run_segmentbase_remux()` | `:465-493` | 非零退出/清单没落盘 → `False`（`:483-491`） |
| `prepare_segmentbase_artifacts()` | `:1067` | 解析 → sidx → 交叉校验 → 命名冲突检查 → 改写 MPD → 重命名 → 写回 |
| `rewrite_segmentbase_mpd()` | `:997` | 把 ffmpeg 的 `BaseURL + SegmentList(mediaRange)` 改写成真 `<SegmentBase indexRange>` |
| `rename_files()` | `:1813-1953` | 重命名分片 → 改 `output.mpd`（补 codecs `:1867`、加 `endNumber` `:1873`）→ 改 `master.m3u8`（`fill_master_codecs` `:1489`）→ 生成 JSON |
| `fill_mpd_codecs()` | `:1466-1486` | 给 MPD 每个 Representation 补 `codecs` |
| `_hls_attr()/_hls_set_attr()` | `:1446-1463` | **现成的 HLS 属性读写工具**（一期改清单可复用） |
| `build_segmentbase_hls_playlist()` | `:1231-1283` | 单文件的 HLS 媒体播放列表（`#EXT-X-MAP` 在 `:1273-1274`，逐段 `#EXT-X-BYTERANGE` 在 `:1278-1281`） |
| `build_segmentbase_hls_master()` | `:1286` 起 | `master-segmentbase.m3u8` |
| `generate_segmentbase_hls()` | `:1336` | 落盘上述 HLS 产物 |
| `build_json_manifest()` | `:1555-1616` | **内置兜底** JSON（注意：返回值 `:1610-1616` 里**没有 encryption**） |
| `generate_segmentbase_json()` | `:1624` | `test-dash-<版本>-segmentbase.json` |
| `_sb_hls_segmentbase_manifest()` | `:1691` | HLS 版 single 清单（只改 `mediaSourceType`） |
| `write_json_manifests()` | `:1727-1759` | 内置实现的 6 个 JSON |
| `generate_json_manifests()` | `:1762-1811` | **优先调 `convert-to-manifest.py`**，失败才退回内置（`:1774-1806`） |
| `verify_keyframe()` | `:1955-2043` | 抽查段内 IDR 间隔 |
| `main()` | `:2045-2101` | 串流程，见下 |

**【已证实】** `main()` 的流程与失败语义（`:2045-2101`）：

```
probe 源 → build_ladder → init_reps                        (:2053-2061)
run_transcode()         失败 → sys.exit(1)                  (:2071-2073)
run_segmentbase_remux() 失败 → segmentbase_ok=False          (:2078)
rename_files(segmentbase_ok)                                 (:2080)
verify_keyframe()                                            (:2081)
if not segmentbase_ok: 明确报错 + sys.exit(1)                (:2096-2100)
```

> **现成的"不静默失败"范式**：`:2075-2077` 的注释写得很清楚 ——
> "失败也不静默 —— 现有的分段式产物照旧收尾，最后明确报错并以非零码退出"。
> 一期的加密步骤应当**照抄这个范式**。

**【已证实】转码侧现在没有任何加密步骤**：全文件 grep
`encrypt|drm|ContentProtection|EXT-X-KEY` **零命中**（只有 `keyint` / `-keyint_min` 这种
关键帧参数，与密钥无关，见 `:262-263`、`:349`、`:363-364`）。
=> 产物 `output.mpd` / `master.m3u8` / `media_*.m3u8` / `output-segmentbase.mpd` /
`*.m4s` **全是明文**。

#### 2.3.2 `convert-to-manifest.py` —— JSON 清单生成（**已经是"加密感知"的**）

**【已证实】** 它**已经会解析 `#EXT-X-KEY` 并写入 `encryption` 字段**：

* `parse_media_m3u8()` 初始化 `'encryption': None`（`:122`）；
* `#EXT-X-KEY:` 分支：`:143-162` —— 读 `METHOD`（**`NONE` 跳过**，`:147`）→ `URI`→`keyUrl`（`:149-151`）
  → `IV`（**去掉 `0x` 前缀**，`:152-155`）→ `KEYFORMAT`（`:156-158`）→ `KEYFORMATVERSIONS`（`:159-161`）；
* `build_manifest()`：视频优先、音频兜底地挑一条 `encryption`（`:738-753`；
  **去掉 `method` 字段**，注释 `:744` 说明"由 keyUrl 存在即表示 AES-128"），
  然后写进**顶层** `manifest['encryption']`（`:762-764`）；
* License Server 分支：`fetch_encryption_info()`（`:602-638`）——
  `POST {license_server_url}/api/keys`，body `{"content_id","mode":"drm","expires_in"}`
  （`:607-618`），期望响应里有 `iv`（`:624`），产出
  `encryption = {"keyUrl": "<ls>/api/key/<content_id>", "iv": ...}`（`:622-627`）与
  `licenseServer = {"url": "<ls>/api/clearkey/<content_id>", "contentId", "keyType":"clearkey"}`（`:629-633`）；
  连不上 Server 只打警告并返回 `None, None`（`:636-638`）；
* 配置入口：`LICENSE_SERVER_URL = None` / `CONTENT_ID = None` / `KEY_EXPIRES_IN = 0`
  （`:35-37`），**代码里写死的模块级变量，不在 CLI 参数里**；触发条件 `:992-995`；
* `main()` 的 CLI 只有位置参数：`<输入目录> <输出目录> <版本号> [baseUrl] [backupUrl...]`
  （`:954-964`）。

**【已证实】⚠️ SegmentBase（single）版清单会丢掉 `encryption`**：
`build_segmentbase_manifest()`（`:484-567`）的返回字典只有
`mediaSourceType/duration/minBufferTime/video/audio`（`:561-567`），
**没有 `encryption`、没有 `licenseServer`**；
`build_hls_segmentbase_manifest()`（`:570-595`）只在它上面改一个 `mediaSourceType`。
=> 即使 `#EXT-X-KEY` 已经写进了单文件的 HLS 播放列表，**`test-dash/hls-*-segmentbase.json` 里也没有任何加密声明**。

#### 2.3.3 `verify_segmentbase.py` —— 现有断言风格

**【已证实】** 文件头 `:3-46` 是"逐项断言"清单，实现是
**`problems: list[str]` 累积 + 末尾非零退出**（`main()` 里 `problems = []` `:621`），
纯标准库、可直接进流水线（`:45` "任一不符 → 打印明细并以退出码 1 结束"）。

**【已证实】** 现成的可复用断言工具：

| 工具 | 行 | 用途 |
|---|---|---|
| `find_sidx(data)` | `:71` | 扫顶层 box 找 sidx 及其字节范围 |
| `sidx_ranges(sidx)` | `:120` | 段范围列表 |
| `parse_range(text)` | `:131` | `"a-b"` → `(a,b)` |
| `parse_hls_byterange(text)` | `:138` | `"长度@起点"` → `(起点, 终点)` |
| `hls_attr(text, name)` | `:147` | **读 HLS 标签属性**（§2.1.3 的 `#EXT-X-KEY` 断言要用） |
| `mp4_box_types(data)` | `:153` | 顶层 box 类型列表（校验 moov/ftyp 完整性） |
| `split_hls_attrs(text)` | `:178` | HLS 属性串切分 |
| `load_manifest_file(path, problems, what)` | `:187-198` | 读 JSON |
| `manifest_rep_groups(manifest, problems, what)` | `:201-211` | 展平 video/audio |
| `check_single_rep_json(rep, what, si, ref, problems, notes)` | `:214-278` | single 模式 rep 逐项核对范式 |
| `media_playlist_name(single_name)` | `:281-284` | 单文件名 → 播放列表名 |
| `check_hls_segmentbase(...)` | `:287` 起 | HLS 文本产物核对 |
| `TOP_FIELDS` | `:64` | `("duration","minBufferTime")` —— **HLS/dash 两版清单逐字段一致**的核对入口（使用处 `:556-559`） |

**【已证实】** 常量：`TMP_NAME_RE`（`:56`）、`SB_SINGLE_FILE_RE`（`:60`）、
`HLS_BYTERANGE_RE`（`:62`）、`SEG_FIELDS`（`:67`）、`SINGLE_FORBIDDEN_FIELDS`（`:68`）。

#### 2.3.4 ⚠️ **现在缺什么（清单）**

| # | 缺什么 | 证据 |
|---|---|---|
| 1 | **转码侧没有任何加密步骤** | `transcode_all.py` 全文无 `encrypt/drm/EXT-X-KEY` 命中 |
| 2 | **产物 MPD 里没有 `ContentProtection`** | ffmpeg dashenc 不会自己写；`transcode_all.py:1845-1950`（`rename_files` 的 MPD 收尾）只补 `codecs`/`endNumber`/改 id |
| 3 | **产物 HLS 里没有 `#EXT-X-KEY`** | `master.m3u8` 与 `media_*.m3u8` 由 ffmpeg 直接写（`:388-389`），脚本只补 `CODECS/RESOLUTION`（`fill_master_codecs()` `:1489`） |
| 4 | **单文件的 HLS 播放列表没有 `#EXT-X-KEY`** | `build_segmentbase_hls_playlist()`（`:1231-1283`）只写 `#EXT-X-MAP` + `#EXT-X-BYTERANGE` |
| 5 | **JSON 清单的 `encryption` 字段"从未被写入"（在分段式路径下也不会有值，因为没有 KEY；在 single 路径下即使有也会被丢）** | 上游没有 KEY ⇒ `parse_media_m3u8()` 的 `result['encryption']` 恒为 `None`（`convert-to-manifest.py:122`、`:143-162`、`:762-764`）；single 路径根本不带这个字段（`:561-567`） |
| 6 | **License Server 不是可调用入口** | `LICENSE_SERVER_URL/CONTENT_ID` 是写死的 `None`（`convert-to-manifest.py:35-36`），CLI 里没有对应参数（`:954-964`） |
| 7 | **内置兜底 JSON 完全不认识加密** | `build_json_manifest()` 返回值 `transcode_all.py:1610-1616` 无 `encryption`；`write_json_manifests()` `:1727-1759` 也不补 |
| 8 | **`#EXT-X-SESSION-KEY` 内核不解析** | `HlsTags.h:78-91` 枚举里没有；全仓只有 `HLSSupport.md:37` 一句待支持 |
| 9 | **DASH（URL 直连 `.mpd`）内核不读 `ContentProtection`** | `MPDParser.cpp:343-433` 无该分支；`DashStream.cpp:394-406` 明确 `setStreamEncrypted(false)` |
| 10 | **桌面 Qt 没有任何 CDM / DRM handler** | `framework/drm/CMakeLists.txt:11-24`（只有 Android / OHOS 分支） |
| 11 | **web HLS 对象注入路径不消费 `playlistDetails.encryption`** | `hls.mjs:38934-38993`（`_buildLevelDetails` 全程没读 `playlist.encryption`） |
| 12 | **web DASH 侧无法表达 Widevine PSSH** | `manifest-to-dash.ts:414-433` 只按 `licenseServer` 写 ClearKey 两条；`ContentProtection` 字段没被消费 |
| 13 | **没有 DRM 专属错误码/日志** | 只有 `STATUS_DRM_ERROR`（`IDecoder.h:46`）；`HLSSampleAesDecrypter.cpp:56-59` 密钥无效时**静默返回密文** |
| 14 | **AES_SAMPLE 软件解密只支持 H264/AAC** | `HLSSampleAesDecrypter.cpp:54-77`（**HEVC 缺失**，而本工程产 h265 档） |

---

## 3. 加密方案对比

| 维度 | **方案 A：HLS AES-128 整片加密** | **方案 B：CENC `cenc-aes-ctr`（逐采样）** | **方案 C：平台 DRM（Widevine/PlayReady/FairPlay）** |
|---|---|---|---|
| **加密粒度** | 整个分片文件字节（连续 AES-128-CBC 块） | 每个 sample（`senc`/`saio`/`saiz` + `tenc`），MP4 结构内 | 同 B（CENC），额外带 PSSH |
| **产物形态** | `.m4s`/`.ts` 就是密文；`#EXT-X-KEY` 声明 | MP4 box 结构加密：`moov` 里 `pssh`/`tenc`，`trak` 里 `senc/saio/saiz`；`init` 段与媒体段都变 | 同 B + `pssh` box + 许可证服务器 |
| **播放器依赖** | 只要 HLS AES-128 解密（本工程内核/web 都有） | 需要 CENC 解密：软件 CENC（ClearKey）或 CDM（Widevine/PlayReady） | 必须 CDM |
| **密钥分发** | 一个 HTTP GET 取 16 字节裸密钥（`HLSStream.cpp:789-833`） | ClearKey：JSON（`kty/k/kid`）；Widevine/PlayReady：challenge→license | 许可证服务器 + 设备证书/会话 |
| **是否需 CDM** | **不需要** | ClearKey 可以纯软件；Widevine/PlayReady 需要 CDM | **必须** |
| **实现成本** | **低**（转码侧加参数 + 清单声明 + web 一处接线） | **中～高**（引入外部打包器；`ContentProtection` 声明；清单双路；验证复杂） | **高**（证书、服务器、设备能力探测） |
| **风险** | 密钥明文、整片加密对 CDN 缓存/字节范围无影响；**不能防"拿到密钥"**；web 对象注入路径需补一行接线 | 单文件 SegmentBase + sidx 与 CENC 的兼容性需要验证；HEVC 软件样本解密内核不支持 | 桌面 Qt 无 CDM ⇒ 直接播不了；合规/IP 成本 |

### 3.1 方案 A：HLS AES-128 工具链

**【已证实（本工程侧）】** 内核与 web 的解密能力都在（§2.1.4 / §2.2.3），
`HlsParser` 明确支持 `METHOD=AES-128` + `URI` + 可选 `IV`（`HlsParser.cpp:304-315`）。

**【已证实（施工时读代码确认）】** 主转码**不是** `-f hls` muxer：它是
`-f dash` + `-hls_playlist 1` + `-hls_master_name master.m3u8`（`transcode_all.py:384-389`，
施工后行号见 §4.8）。⇒ `-hls_key_info_file` 这类 **hls muxer 的私有选项对主命令无效**，
设计初稿 §7.4 的待验证项 1 到此**确认为"不可用"**（不是"待测"）。

**【已证实（工具常识 + 选项语义）】** 即使另起一趟 `ffmpeg -f hls -hls_key_info_file`，
它也只能给**整条播放列表一个 IV**（`-hls_key_info_file` 的第三行是**单个** IV；
多个 hls 输出各带自己的 key_info 文件并不能让"同一路分片"获得不同 IV）。
这与一期想要的"**每分片 IV 唯一**"（§4.1、§4.6 A5）直接冲突。

因此**一期实际采用的工具链是第三种**（施工记录见 §4.8）：

3. **转码脚本自己做字节级加密（一期落地选择）**：主转码与 SegmentBase 再封装命令
   **一个参数都不动**；`rename_files()` 之后由脚本对**分段式分片**原地做
   AES-128-CBC + PKCS7，并在清单里写 `#EXT-X-KEY` / `#EXT-X-SESSION-KEY` /
   MPD `ContentProtection`。加密原语是**新增的 `转码脚本/aes128.py`**
   （纯 Python 的 FIPS-197 AES-128 实现，S-box / 密钥扩展 / 列混淆全在里面），
   **不引入第二趟 ffmpeg、不引入第三方 Python 依赖、也不依赖 `openssl` 命令**
   （细节与两条官方向量见 §4.8.3）。
   * 收益：每分片显式 IV（与内核/web 的序号推导**同一条**）；加密端与验收端**共用同一份
     实现**，所以"解密回来逐字节一致"这件事可以在脚本内自证；
     失败语义完全可控（原子写、非零退出）。
   * 代价：加密由 Python 层逐分组完成，比"一条 ffmpeg 命令"慢（量级见 §4.8.4）；
     换工具链的收益（速度）与硬约束（每分片唯一 IV）冲突，故一期不做。

**AES-128 的 IV 约定**（二期/商用播放器通用结论，一期照此实现）：

* 清单显式给 `IV=0x…` ⇒ 内核 `ivStatic=true`，直接用（`HlsParser.cpp:311-315`）；
* 不给 IV ⇒ 播放器**按分片序号**推：序号写在 IV 的**最后 4 字节、大端**
  （内核 `HLSStream.cpp:835-853`；web `hls.mjs:7593-7599` 的 `createInitializationVector`）。
* **一期选择"显式给 IV，且写的值与隐式推导完全一致"**：基 IV = 全 0，
  第 N 片的 IV = `00…00` + N 的大端 4 字节 ⇒ 显式与隐式**同一条 IV**，双保险。

### 3.2 方案 B：CENC `cenc-aes-ctr` 三条工具链

下面的具体命令行**属于"工具知识"，本次无法运行验证**，标注为【推测/待验证】，
施工前必须用本机版本核对（`--help`）。

| 工具链 | 能产出什么 | 谁负责写清单里的加密声明 | 对 SegmentBase 单文件 |
|---|---|---|---|
| **ffmpeg（8.x）** `-encryption_scheme cenc-aes-ctr -encryption_key <hex> -encryption_kid <hex>` | 由 mov/mp4 muxer 写 CENC（`tenc`/`senc`/`saio`/`saiz` + `pssh`）。**注**：这些是 **mov muxer** 的选项；`-f dash` 输出能不能用、以及能否与 `-single_file 1` 并存，**必须实测** | **ffmpeg 不会写 `ContentProtection`**（dashenc 不写）⇒ 声明必须**由脚本后处理写入**（`fill_mpd_codecs()` 的同一位置，`transcode_all.py:1867` 附近） | 风险最高：CENC 会改变 box 布局，**sidx 与所有 mediaRange 都要重算**，而现有 `rewrite_segmentbase_mpd()`（`:997`）与全套 sidx 断言（`verify_segmentbase.py:12-22`）都建立在"加密前后布局不变"上 |
| **shaka-packager** | 输入明文 MP4/分片 → 输出**加密后的 init+media 分片**，并（可选）生成 MPD/HLS。是"打包器"而不是 muxer | **packager 能写 `ContentProtection`（`--mpd_output`）**，也能写 HLS 的 `#EXT-X-KEY:METHOD=SAMPLE-AES`（`--hls_master_playlist_output`） | 单文件 SegmentBase **不在 shaka 的原生输出形态里**（它按分片/DASH 模型工作）⇒ 要么放弃单文件模式的加密，要么另行处理 |
| **Bento4（`mp4fragment` + `mp4encrypt`）** | `mp4fragment` 造 fMP4 + sidx；`mp4encrypt --method MPEG-CENC --key … --property KID:… ` 逐采样加密 | **由脚本自己写**（Bento4 不管清单）。好处：**加密前后 box 顺序可控，sidx 可最后再生成**，能保住 SegmentBase 单文件形态 | **这条最适合单文件模式**：先 `mp4encrypt`，再 `mp4fragment`/重写 `sidx`，让"加密后的字节范围"与 sidx 同源 |

**【已证实（本工程约束）】** 两条硬约束会让方案 B 更贵：

* **内核读不到 CENC 的软件解密路径（HEVC）**：`HLSSampleAesDecrypter.cpp:54-77` 只处理 H264/AAC；
  本工程转码产 h264 **和** h265 两路（`transcode_all.py:50-53`）⇒ 走 ClearKey 软件路径时
  **h265 档解不了**。
* **桌面 Qt 没有 CDM**（`framework/drm/CMakeLists.txt:11-24`）⇒ 方案 B 在一期只能在
  Android/OHOS 上验证"真解密播放"，桌面只能用 ClearKey + 软件 CENC（而软件 CENC 的 HEVC 又不支持）。

### 3.3 方案 C：平台 DRM

* **PSSH**：需要在 init 段（`moov/pssh`）与 MPD `<ContentProtection><cenc:pssh>` **两处一致**；
  本工程对象清单模型里 `ContentProtection::pssh`（`MediaManifest.h:41`）与
  `SegmentEncryption::pssh`（`SegmentEncryption.h:31`）已经留好位置，
  `ManifestDemuxer.cpp:177` 会把 `cp.pssh` 带进去，`HLSStream.cpp:1519` 会导出成
  `Stream_meta.drmPssh`，`SMPAVDeviceManager.cpp:54-56` 会塞进 `DrmInfo.pssh`。
  **通路是通的 —— 但只在 Android/OHOS 上通**。
* **CDM**：`WideVineDrmHandler.cpp`（Android/JNI，`:110-113`）、
  `OHOS/OhosDrmHandler.cpp`（`#ifdef __OHOS__`）。桌面 Qt：**无**。
* **许可证服务器**：`DrmManager::require()`（`framework/drm/DrmManager.cpp:19`）→
  handler 请求许可证 → 通过回调交给业务方（文档口径见
  `docs/ObjectManifestPlayback.md:136-161`）。
* **Qt 桌面现实约束**：没有 CDM 就没法在设备内解密 Widevine/PlayReady 内容。
  如果一期目标是"桌面 Qt 能播"，方案 C **现在不可行**；如果目标是 Android/OHOS，
  方案 C 的瓶颈在**服务器侧（许可证签发）与内容侧（PSSH/KID 一致性）**，不在播放器。

---

## 4. 一期落地设计

> 一期 = **方案 A（HLS AES-128 整片加密）**，目标：
> **转码产出密文 + 清单如实声明 + 内核与 web 都能解密播放 + 有可进流水线的自检。**
> 下面写到"能直接照着施工"的粒度，但**不写完整实现代码**。

### 4.0 总体数据流（一期）

```
input.mp4
  │
  ├─①主转码（明文，命令与今天完全一致：output.mpd + master.m3u8 + media_*.m3u8 + *.m4s）
  │
  ├─②SegmentBase 再封装（明文，-c copy，命令与今天完全一致）
  │
  ├─③【新】加密阶段（只在分段式产物上做；插在 ② 之后、重命名之前/之后见 §4.2）
  │     产出：密文分片 + 带 #EXT-X-KEY 的 HLS 播放列表 + 带 ContentProtection 的 MPD 副本
  │     密钥来源：命令行参数 / 环境变量（绝不硬编码）
  │
  ├─④重命名与清单收尾（复用现有 rename_files）
  │
  ├─⑤【新】清单加密声明：HLS 文本 + MPD + 对象化 JSON 的 encryption 字段
  │
  └─⑥【新】verify_drm.py 与现有 verify_segmentbase.py 一起跑；失败 → 非零退出
```

### 4.1 密钥生成与保存

**原则：绝不硬编码；密钥必须从"参数"进来（不是开关 —— 见 §5.5）。**

> **【施工后口径】** 一期实际只落地了**一个**参数：`--hls-key-info <文件>`，
> 文件格式就是 HLS/ffmpeg 生态的 `key_info_file`（三行：URI / 密钥文件路径 / 可选 IV）。
> 理由：它一次把"密钥内容 + 写进清单的 URI + IV 基"三件事都说清楚，与外部工具（ffmpeg
> `-hls_key_info_file`、shaka、Bento4）口径一致，将来换工具链不用改参数；
> 而"多个散参数"（`--key-hex/--key-file/--key-env`）会让"三选一"的优先级规则变成又一处
> 需要记住的隐式行为。**缺值 = 错误**这一条不变（`--hls-key-info` 后面没有值 ⇒ 立刻非零退出）。
> 参数名与完整用法见 §4.8.2。

| 项 | 设计 |
|---|---|
| **KEY（16 字节）来源** | 一期：`--hls-key-info <文件>` 的**第 2 行**（密钥文件路径；内容为 32 个 hex 字符，或 16 字节裸二进制）。**没有给这个参数 ⇒ 明文产物**（并在日志里明确提示"本次全部为明文"）；**给了但读不到/长度不对 ⇒ 非零退出**，绝不回落到明文 |
| **KID（二期 CENC 才需要）** | `--kid <uuid>`；未给时由 `--key-id-env` 或从 KEY 派生（**二期再定**，一期不用） |
| **密钥文件权限** | POSIX：手工 `chmod 600`（脚本**只读不 chmod**，不悄悄改别人的文件权限）；Windows：落在输出目录**之外**（例如 `<输出目录>/../keys/<内容ID>.key`），并明确不随产物上传 CDN |
| **密钥文件落盘格式** | 简单优先：**一个文件 = 32 个 hex 字符**（16 字节）。`convert-to-manifest.py` 已有的 `iv` 也是 hex（`:152-155`），口径一致；16 字节裸二进制也接受 |
| **绝不做的事** | ① 不把密钥写进 `master.m3u8`/JSON；② 不把密钥 base64 塞进清单；③ 不在日志里打印密钥（只打印密钥指纹 = SHA-256 前 8 字节，`transcode_all.py` 的 `_key_fingerprint()`） |
| **IV / 轮换** | 一期：**全片一个密钥 + 每分片显式 IV**（IV = 全 0 基 IV，最后 4 字节按分片序号递增，与内核 `HLSStream.cpp:838-849` / web `createInitializationVector()` 的推导**完全一致**）。轮换（多密钥）留二期：内核 `updateKey()` 靠 **URL 变化**触发重拉（`HLSStream.cpp:794-796`），所以多密钥就是"多个 URI"，不需要改内核 |

**key_info 文件示例**（三行；第 3 行可省略）：

```
key.bin
D:/hilihili/keys/379995093081395200.key
0x00000000000000000000000000000000
```

* 第 1 行 `key.bin` 是**写进清单的相对 URI**：播放器按"媒体播放列表所在目录"解析它
  （内核 `Helper::combinePaths(basePath, keyUrl)`；web `M3U8Parser.resolve`），
  所以"密钥与分片同目录"是最不容易配错的形态；
* 第 2 行是**本机路径**，绝不进清单；相对路径按**脚本所在目录**解析（与 `WATERMARK_IMAGE` 同口径）；
* 第 3 行是 IV 基（`0x` + 32 hex）。写全 0 时，"显式 IV"与"播放器自己推的 IV"是同一条。

### 4.2 转码脚本流程改动：加密插在**哪一步之后**

**必须插在"切片之后"（即主转码完成之后），不能插在切片之前。** 理由：

* 本工程的主转码是**一条 ffmpeg 命令同时产 DASH+HLS**（`transcode_all.py:384-400`），
  分片由 dashenc 在关键帧边界切出来（`-use_timeline 1`，`:397`）。
  "切片前加密"意味着"对编码器输出提前加密" —— 那会把 MP4 **box 结构**也一起加密，
  sidx/init 段全部不可读，`-c copy` 再封装（`:442-462`）与 `parse_m4s_sidx()`
  都直接失效。
* AES-128 的语义本来就是"**整个分片文件**是密文"（HLS 规范口径），
  所以加密**天然是切片后动作**。

**对新阶段的位置**（**【施工后已按此落地】**，实际代码行号见 §4.8.1）：

```
main():
  run_transcode()                # 明文产出（不动）
  run_segmentbase_remux()        # 明文产出（不动）★必须在加密之前：它读 output.mpd 与明文分片
  rename_files()                 # ★ 先重命名：加密阶段用"最终文件名"写 #EXT-X-KEY 与 IV，
                                 #   也要把密文写成最终名，避免再引入第三套命名规则
  verify_keyframe()              # ★ 也必须在加密之前：它把 init 段 + 媒体段拼给 ffprobe 解复用，
                                 #   密文喂进去只会得到垃圾（加密不改变 GOP 结构，§5.4）
  run_encryption()               # ★ 新增：只加密"分段式"产物（见下）
  generate_json_manifests(...)   # ★ 移到加密之后：好让 convert-to-manifest.py 从
                                 #   带 #EXT-X-KEY 的 media_*.m3u8 里读到 encryption
```

> **施工补充（设计初稿没写到的两处顺序约束）**：
> 1. **`run_segmentbase_remux()` 必须早于加密**：它 `-i output.mpd -c copy`，而加密阶段会给
>    `output.mpd` 插 `ContentProtection`；虽然它读的是分片字节、不看 MPD 的 DRM 元素，
>    但"再封装的输入是加密前的清单/产物"这件事让失败语义最干净（不会出现"单文件里混进密文"）。
> 2. **`verify_keyframe()` 必须在加密之前**：它是"拼接 init + 媒体段 → ffprobe"的抽查，
>    密文过 ffprobe 只会得到误报。

> 把新阶段放在 `rename_files()` **之后**（而不是之前）的理由：现有
> `rename_files()` 里对 HLS/MPD 的文本改写（补 codecs `fill_master_codecs()` `:1489`、
> 补 `endNumber` `:1873`）都靠文件名与 rep id 的映射；
> 加密阶段如果先跑，就会让这些改写面对"文件名带密钥语义"的中间态。
> 放在之后 = 加密阶段只面对**最终形态**的产物，逻辑最干净。

**分段式模式 vs SegmentBase 单文件模式 —— 分别怎么处理：**

| 模式 | 产物 | 一期处理 | 理由 |
|---|---|---|---|
| **分段式（主产物）** | `output.mpd` + `master.m3u8` + `media_N.m3u8` + `<ID>-<suffix>-<seq>.m4s` | **加密**：分片 → 密文（原地替换，先写 `.enc.tmp` 再原子替换）；`media_N.m3u8` 的第一个 `#EXTINF` 之前插 `#EXT-X-KEY`；`master.m3u8` 顶部插 `#EXT-X-SESSION-KEY`；`output.mpd` 的每个 `AdaptationSet` 插 `ContentProtection`；明文另存 `_clear/` 供 A6 比对 | sidx 不存在（分段式靠 `segmentTimeline`，`transcode_all.py:1584`），加密不影响段定位 |
| **SegmentBase 单文件** | `output-segmentbase.mpd` + `<单文件>.m4s` + `<单文件>.m4s.m3u8` + `master-segmentbase.m3u8` + `test-*-segmentbase.json` | **一期不加密，并如实声明**（见下）；并且**由断言双向钉死**（转码侧 `_assert_segmentbase_plain()` / `_assert_segmentbase_not_encrypted_on_disk()`；验收侧 `check_drm_not_encrypted()`） | ⚠️ 该文件里有一个 **sidx**，`#EXT-X-BYTERANGE`/`indexRange`/`initialization` 全是**绝对字节范围**（`build_segmentbase_hls_playlist():1278-1281`、`convert-to-manifest.py:511-535`）。AES-128 整片加密要**按 16 字节块补齐（PKCS7）** ⇒ 文件长度会变 ⇒ **所有范围与 sidx 全部作废**。要么放弃"整片加密"改逐块原地加密（就不是 HLS AES-128 语义了），要么重算 sidx —— 都超出"一期"收益 |

**单文件模式的具体落地口径（一期）**：

1. 加密阶段**跳过** `output-segmentbase.mpd` 与其单文件；
2. 在**日志与清单里如实说明**：`test-*-segmentbase.json` **不含** `encryption`，
   `master-segmentbase.m3u8` **不含** `#EXT-X-KEY`；
3. `verify_drm.py` 里对这两种产物各写一条**互斥断言**：
   "分段式产物必须是密文且清单有 KEY；SegmentBase 产物必须是明文且清单**没有** encryption"
   —— 用断言把"未加密"这件事钉死，杜绝"以为加了其实没加"。

> 二期如果要单文件也加密，走 §3.2 的 **Bento4 路线**（先加密再生成 sidx），
> 那时 `verify_segmentbase.py` 的整套 sidx 断言正好当回归网。

**失败语义（照抄现有范式）**：

* 加密阶段任何一步失败（工具非零退出 / 密文长度不是 16 的整数倍 / 清单没写上 KEY /
  密钥文件不可读）⇒
  * **不产出半成品**：密文先写临时名（如 `<final>.enc.tmp`），清单先在内存里改，
    全部成功后才 `os.replace()` 原子落盘；
  * **明确报错 + 非零退出**：`log("[错误] …")` + `sys.exit(1)`，
    与 `transcode_all.py:2096-2100` 现有做法一致；
  * **绝不**"加密失败就把明文当成功交付"（§6）。

### 4.3 清单改动

#### 4.3.1 MPD：`ContentProtection`

`output.mpd` 由 ffmpeg 直出（`transcode_all.py:399`），**ffmpeg 不会写 `ContentProtection`**
⇒ 由脚本后处理插入。位置：`rename_files()` 里 `fill_mpd_codecs(content)`（`:1867`）**之前或之后**，
对 `<AdaptationSet>` 元素做一次插入（每个 AdaptationSet 一份）：

```xml
<!-- 一期：AES-128 没有真正的 ContentProtection 语义，这里只作为"这条流被保护"的显式声明。
     schemeIdUri 用 DASH-IF 的 AES-128 标识；不要写 cenc:default_KID（那是 CENC 的 KID）。 -->
<ContentProtection schemeIdUri="urn:uuid:d0ee2730-09b5-459f-8452-200e52bec0f7"/>
```

> **重要提醒（已证实的内核行为）**：这条声明对**内核的 URL 直连 `.mpd` 路径毫无作用** ——
> `MPDParser` 不解析 `ContentProtection`（§2.1.5）。
> **它的价值在 web 的 dash.js 路径**（dash.js 会自己解析 MPD 文本拿到 `ContentProtection`，
> 再走 EME）。所以：
> * 一期如果要让 **web 用 dash.js 播 MPD**，`ContentProtection` 必须写，且要写成 dash.js 认的形态
>   （AES-128 在 DASH 里没有标准表达 —— 这一点标注为**【待验证】**，
>   保守做法是 web 侧**只走 HLS 路径**播加密内容，MPD 的声明作为"语义完备性"补充）；
> * 一期如果要让**内核**播加密内容，正确入口是 **HLS 文本 + 对象化 JSON**，不是 `.mpd`。
>
> 二期（CENC）时这里才换成标准形态，并必须**与 init 段的 `pssh`/`tenc` 一致**：
> ```xml
> <ContentProtection schemeIdUri="urn:mpeg:dash:mp4protection:2011" value="cenc"
>                    cenc:default_KID="<KID 小写带连字符>"/>
> <ContentProtection schemeIdUri="urn:uuid:e2719d58-a985-b3c9-781a-b030af78d30e">
>   <cenc:pssh>AAAA…（base64）</cenc:pssh>
> </ContentProtection>
> ```
> （字段名与结构对照本工程既有模型：`MediaManifest.h:33-45`、
> `docs/ObjectManifestPlayback.md:99-106`。注意 `convert-to-manifest.py:186-191`
> 已经会**补 `xmlns:cenc`**，`cenc:` 前缀不会造成解析失败。）

#### 4.3.2 HLS：`#EXT-X-KEY` 与 `#EXT-X-SESSION-KEY`

**媒体播放列表（`media_N.m3u8`）**：插在**第一个 `#EXTINF` 之前**
（必须在 `#EXT-X-MAP` 之后也无所谓，HLS 只要求"对后续分片生效"）。
**【施工后实际产物样例】**（`#EXT-X-KEY` 的行号 = 最后一个 `#EXT-X-MEDIA-SEQUENCE` / `#EXT-X-MAP` 之后）：

```
#EXTM3U
#EXT-X-VERSION:7
#EXT-X-TARGETDURATION:6
#EXT-X-MEDIA-SEQUENCE:0
#EXT-X-KEY:METHOD=AES-128,URI="key.bin",IV=0x00000000000000000000000000000000
#EXT-X-MAP:URI="379995093081395200-video-h265-1920_1080-0.m4s"
#EXTINF:6.000,
379995093081395200-video-h265-1920_1080-1.m4s
…
```

> ⚠ 每片 IV 不同这一点在 **HLS 文本里看不出来**：`#EXT-X-KEY` 只写一条、带的是**第一个
> 分片的 IV**，后续分片由播放器按序号递推（内核 `HLSStream.cpp:835-853`、web
> `createInitializationVector`）。一期让"递推结果"与"真实加密用的 IV"**逐位相同**
> （基 IV 全 0 + 序号写在最后 4 字节），所以不需要给每个分片写一条 `#EXT-X-KEY`
> （HLS 也不允许在 `#EXTINF` 之后插标签）。

要点：

* `METHOD=AES-128` + `URI` 是**必需**的（内核在 `HlsParser.cpp:304-306` 要求两者同时存在，
  否则落到 `NONE`）；
* `IV` 用 `0x` 前缀（`Attribute::hexSequence()` 认这个格式，`HlsTags.h:27`）；
* **`URI` 写成相对路径**（如 `key.bin`）：内核用 `Helper::combinePaths(baseUri, keyUrl)`
  解析（`HLSStream.cpp:791-792`），相对路径会拼到播放列表同级 —— 最省事、也最不容易配错；
* **不要**写 `KEYFORMAT`（写了就是非空 ⇒ 内核会去问 `DrmUtils::isSupport`，
  见 `HLSStream.cpp:546`；非 Android 一律 `false` ⇒ 这条加密记录被跳过 ⇒ **静默降级**！）。
  要写就写 `KEYFORMAT="identity"`，但那还是非空 —— **一期统一不写**。
  （脚本与验收脚本都各有一条断言堵这个坑：`_review_json_manifests()` / A4。）

**主播放列表（`master.m3u8`）**：加 `#EXT-X-SESSION-KEY` 供播放器**预取**密钥。
**【施工后实际产物样例】**（插在第一条 `#EXT-X-MEDIA` / `#EXT-X-STREAM-INF` 之前）：

```
#EXTM3U
#EXT-X-VERSION:7
#EXT-X-SESSION-KEY:METHOD=AES-128,URI="key.bin"
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="group_A1",…,URI="media_4.m3u8"
#EXT-X-STREAM-INF:BANDWIDTH=5134063,RESOLUTION=1920x1080,CODECS="avc1.640028,mp4a.40.2",AUDIO="group_A1"
media_0.m3u8
…
```

> **已证实的内核现状**：`#EXT-X-SESSION-KEY` **内核不解析**（§2.1.2）。
> 它**不会报错**（HlsParser 的 switch 没这个 tag ⇒ 被忽略），
> 只是个"给商用播放器/浏览器 hls.js 的优化提示"。
> 因此**媒体播放列表里的 `#EXT-X-KEY` 是必须的，`SESSION-KEY` 是可选增强**。
> **【施工后】** 一期**确实写了** `#EXT-X-SESSION-KEY`（照本文档；内核忽略无害），
> 并由验收脚本 A12 断言它与媒体播放列表的 `METHOD`/`URI` 逐字一致。

#### 4.3.3 对象化 JSON 清单的 `encryption` 字段（**复用现有字段，不新造**）

**好消息：这条路的代码已经写好了，只缺上游给 KEY。**
`convert-to-manifest.py:143-162` 会从 `media_*.m3u8` 读到 `#EXT-X-KEY` 并写进
`manifest['encryption']`（`:762-764`）。所以 §4.3.2 只要把 `#EXT-X-KEY` 写进去，
**分段式的 4 个 JSON 会自动带上 `encryption`** —— 不需要改 `convert-to-manifest.py` 的解析逻辑。

**具体 JSON 片段**（顶层，字段名与 `MediaManifest.h:54-60` / `MediaManifestParser.cpp:56-63` 一一对应）。
**【施工后实际样例】**（`convert-to-manifest.py` 从带 `#EXT-X-KEY` 的 `media_*.m3u8` 生成；
`iv` 就是 key_info 第 3 行的 IV 基，**不带 `0x`**）：

```json
{
  "mediaSourceType": "hls",
  "duration": 268.235,
  "minBufferTime": 1.5,
  "encryption": {
    "keyUrl": "key.bin",
    "iv": "00000000000000000000000000000000"
  },
  "video": [ { "id": 0, "baseUrl": "http://127.0.0.1:9000/video/dash2/", "...": "..." } ],
  "audio": [ { "...": "..." } ]
}
```

口径说明（**都对得上实现**）：

* `iv` 是 **hex 字符串、不带 `0x`**（`convert-to-manifest.py:155` 去掉了前缀；
  `MediaManifestParser.cpp:59` 原样收；`ManifestDemuxer.cpp:150-157` 两字符一字节地转回来）；
* `keyUrl` 相对路径可用（`ManifestDemuxer` 有自己的 `resolveUrl()`，
  `ManifestDemuxer.cpp:55-71`）；
* **`method` 字段不要写**：`convert-to-manifest.py:744-745` 主动把它剔除了，
  内核侧 `Aes128Encryption` 结构里也没有这个字段（`MediaManifest.h:54-60`）——
  "有 `keyUrl` 就是 AES-128"（`ManifestDemuxer.cpp:144`）；
* **必须写在顶层**（不是 rep 级）：`buildSegmentEncryptions()` 会把它套给所有 rep
  （`ManifestDemuxer.cpp:142-144`）；
* `keyFormat` / `keyFormatVersions`：**一期一律不写**。写了非 `identity` 的值会在
  `ManifestDemuxer.cpp:158-160` 带进 `SegmentEncryption::keyFormat`，
  然后被 `HLSStream.cpp:546` 拿去做平台支持判定 —— 非 Android 会**跳过这条加密**。
  这是**最危险的一个字段**；`transcode_all.py` 的 `_review_json_manifests()` 与
  验收脚本都会把它当硬错误抓（本设计初稿的示例里曾写 `"keyFormat": "identity"`，
  施工时改成**不写**，与"内核会把它当非空 keyFormat"这条事实对齐）。

**⚠️ single（SegmentBase）版 JSON**：`build_segmentbase_manifest()` 的返回字典
（`convert-to-manifest.py:561-567`）**没有 `encryption`**。
一期既然不加密单文件，这正是**期望行为**，且**已有双向断言**：
转码侧 `transcode_all.py:1920` 的 `_assert_segmentbase_plain()`、
`:1936` 的 `_assert_segmentbase_not_encrypted_on_disk()`；
验收侧 `verify_segmentbase.py:1040` 的 `check_drm_not_encrypted()`（A10）。

**⚠️ 内置兜底路径（§4.3.3 点的那个"静默缺口"）**：如果 `convert-to-manifest.py` 调用失败，
`transcode_all.py` 过去会退回 `build_json_manifest()`，而它**不带 `encryption`**
⇒ 会产出"密文分片 + 不声明加密"的清单 ⇒ **播放器直接花屏**。
**【施工后处置（两条都做了）】**：

1. 内置实现已经**认识加密**：`build_json_manifest()`（`transcode_all.py:1827`）的返回值统一
   经过 `_with_encryption()`（`:1906`），加的是与 `#EXT-X-KEY` **同一份**状态
   （`_json_encryption()`，`:1891`），口径不可能再分叉；
2. **加密模式下禁用兜底**：`generate_json_manifests()`（`:2121`）在 `HLS_ENCRYPTED` 为真时，
   若权威实现不可用/产出不完整 ⇒ **直接抛错、非零退出**，绝不"退回内置实现"当交付。
   收尾还有 `_review_json_manifests()`（`:2178`）逐字段核对
   `keyUrl` / `iv` / `keyFormat` 与 single 清单"不许有 encryption"。

### 4.4 密钥/许可证分发

#### 4.4.1 播放器期望什么形状

**【已证实】AES-128（内核）**：`HLSStream::updateKey()`（`HLSStream.cpp:789-833`）
对 `keyUrl` 发一次普通 HTTP GET，**期望响应体正好 16 字节裸密钥**
（`:812-829`：读满 16 字节；`size != 16` ⇒ 报错 `-1`）。
**不期望** JSON、不期望 base64、不期望 HTTP 头里的额外信息。

**【已证实】AES-128（web）**：`ManifestEncryption.keyUrl` → hls.js 的 `Key.uri`，
hls.js 的密钥加载器同样按"裸 16 字节"处理（`hls.mjs:36838`
`keyInfo.decryptdata.key = new Uint8Array(response.data)`）。

**【已证实】License Server 的分工（清单侧语义）**：
`encryption.expiresIn > 0` ⇒ 密钥视为**临时密钥**，
`keyUrl` 改由 `licenseServer.url` 提供（内核：`ManifestDemuxer.cpp:147-149`；
web：`manifest-to-hls.ts:99-100`；Qt 侧 builder：`CicadaManifestBuilder.h:181-189`）。
宽泛地说：`licenseServer` 是"**DRM/CENC 许可证**"语义
（`ContentProtection`/`laUrl`/`pssh` 一起用，`ManifestDemuxer.cpp:179-183`），
**不是** AES-128 取密钥的常规入口 —— 常规入口就是 `encryption.keyUrl`。

**【已证实】`convert-to-manifest.py` 已经假定的 License Server 形状**（可作为最小方案的口径）：
* `POST <ls>/api/keys`，body `{"content_id":…, "mode":"drm", "expires_in":…}`，
  成功响应 JSON 里要有 `iv`（`:607-624`）；
* 它自己会拼出 `<ls>/api/key/<content_id>` 作为最终 `keyUrl`（`:623`）与
  `<ls>/api/clearkey/<content_id>` 作为 `licenseServer.url`（`:630`）。

#### 4.4.2 最小可用方案（一期）

**阶段 0（一期即可交付）：静态 key 文件。**
* 产出一个 `key.bin`（16 字节裸二进制），与分片同目录（或同 CDN 目录）；
* 清单里 `keyUrl: "key.bin"`；用服务器上的**路径级访问控制**（签名 URL / referer / IP 白名单）
  保护它 —— 这是"最小可用"，安全强度取决于这层访问控制；
* **优点**：内核零改动、web 零改动、无服务器；**缺点**：拿到 URL 就能拿密钥（明文 key URL 在清单里）。

**阶段 1（一期可选，把 key URL 藏起来）：简易 key 服务。**
* 两个端点：
  * `GET /api/key/<contentId>?token=…` → **`Content-Type: application/octet-stream`，body = 16 字节裸密钥**
    （必须是裸字节，见 §4.4.1）；
  * `POST /api/license/<scheme>` → 给 CENC/DRM 用（二期；一期可以只返回 501）。
* 部署：一个几十行的服务即可；**不需要** CDM、不需要证书（AES-128 不涉及 EME）。
* 与现有脚本对接：把 `convert-to-manifest.py` 的 `LICENSE_SERVER_URL`/`CONTENT_ID`
  （`:35-36`）从"写死的模块级 `None`"改成**命令行参数 / 环境变量**
  （注意：这是**参数**，不是"新增配置开关" —— §5.5）。

> **一期落地的稳妥顺序**：先做**阶段 0（静态 key 文件）**把整条链路打通并验收；
> key 服务作为可选增强，不阻塞一期验收。

### 4.5 播放器侧要动的点

#### 4.5.1 内核（C++）：**预期零改动**

**【已证实】AES-128 路径已经够用**（逐条对过）：

| 环节 | 已有实现 | 结论 |
|---|---|---|
| HLS 文本读 `#EXT-X-KEY` | `HlsParser.cpp:294-355` | 够用 |
| 对象清单读 `encryption` | `MediaManifestParser.cpp:56-63`、`ManifestDemuxer.cpp:142-163` | 够用 |
| 拉 16 字节密钥 | `HLSStream.cpp:789-833` | 够用 |
| IV（显式 / 序号推） | `HlsParser.cpp:311-315`、`HLSStream.cpp:835-853` | 够用 |
| 解密 + 去 PKCS7 | `AES_128Decrypter.cpp:27-101` | 够用 |
| 解密后喂 demuxer | `HLSStream.cpp:552-573` | 够用 |

**一期内核改动 = 0 行。** 需要做的只是**验证**（§4.6）。

**唯一需要留意的既有行为（不是 bug，但要写进验收）**：
解密路径上 `seekSegment()` 返回 `-EINVAL`（`HLSStream.cpp:206-215`）⇒
seek 只能靠"重开分片"完成。这条在明文路径也一样（`mSegDecrypter != nullptr` 时），
**加密不会让它变差**，但验收时要**专门测 seek**（§5.4）。

#### 4.5.2 web（TypeScript/vendored hls.js）：**一处必修 + 一处可选**

* **必修（§2.2.3 的缺口）**：`packages/plugins/src/hls/vendor/hls.mjs` 的
  `_buildLevelDetails()`（`:38934-38993`）要在构造函数里消费 `playlist.encryption`：
  按它造一个 `Key(method='AES-128', uri, format='identity', iv)`（`Key` 类见 `:7431-7453`），
  挂到 `details.key`，并让每个 `Fragment` 能拿到 `decryptdata`
  （由 `Key.getDecryptData(frag.sn, details.levelKeys)` 推 IV，`:7478-7484`）。
  工作量很小，但**不做就播不了**。
* **可选**：`manifest-to-dash.ts` 目前只按 `licenseServer` 写 ClearKey
  （`:414-433`）、不消费 `manifest.contentProtection`
  ⇒ web 一期**建议只用 HLS 路径**播加密内容；DASH 加密留二期。

#### 4.5.3 DRM 失败的错误码与日志

**现状（已证实，且不理想）：**

* 整片 AES-128 密钥错误 ⇒ `updateKey()` 报 `open key file error` / `key size is %d not 16`
  （`HLSStream.cpp:808`、`:825`）并返回负值 ⇒ 上层拿到的是**通用 demux/IO 错误**，
  **不是"DRM 错误"**；
* 样本级 AES_SAMPLE 密钥无效 ⇒ **只打一条日志并把密文原样返回**
  （`HLSSampleAesDecrypter.cpp:56-59`）⇒ **静默吐垃圾**，最终表现为花屏/解码错误；
* `DrmUtils::isSupport()` 返回 false 导致加密记录被跳过（`HLSStream.cpp:546`）
  ⇒ **完全没有任何日志**，表现为"它以为自己播的是明文"。

**一期要做的（小、但必须）：**

1. **在 `HLSStream::createDemuxer()` 的加密选择处加一条 ERROR 日志**：
   当 `mCurSeg->encryptions` 非空但**没有任何一条被接受**（全部因 `keyFormat` 不支持被跳过）时，
   打印"这一段声明了加密，但没有任何一条被当前平台支持（keyFormat=…）"，**不静默**。
   改动点在 `HLSStream.cpp:545-550` 的循环之后，约 4~6 行。
2. **给 "加密已启用但密钥拉取失败" 一个可识别的错误**：建议复用现有错误类，
   在 `updateKey()` 的失败分支（`HLSStream.cpp:807-810`、`:824-829`）把
   `error_class_demuxer` 系下的一个**已存在**的"资源/IO 错误码"作为返回值即可
   （`framework/utils/errors/framework_error.h:19` 起的枚举），
   **不新增错误码枚举值、不新增开关**；同时在日志里带上 `keyUrl`（**绝不带密钥内容**）。
3. **绝不做**：不用 `sleep`/重试次数/"等 500ms 再看"这类**墙钟兜底**（§5.5）。

> 二期（CENC）才会需要"许可证失败"这类细分错误（可复用 `STATUS_DRM_ERROR`，
> `IDecoder.h:46`），一期不准各新造。

### 4.6 验收与自检

**【施工后口径】** 一期**没有**新开 `verify_drm.py`，而是把断言**并进** `verify_segmentbase.py`
（同一个"`problems` 累积 + 非零退出"风格、同一套现成工具 `hls_attr()` `:167`、
`mp4_box_types()` `:173`、`parse_hls_byterange()` `:158`、`find_sidx()` `:91`、
`load_manifest_file()` `:207`）。理由：加密产物与 SegmentBase 产物**在同一次转码里一起产出**，
分两个脚本跑会带来"跑了一个忘了另一个"的运维风险；并进同一个脚本后
"明文/密文互斥断言"（A2/A10）天然成对。

**验收器的第 0 步是 AES 原语自检**（`verify_segmentbase.py` 的 `main()`）：
本脚本用**脚本内**的 AES-128 —— 也就是与加密端共用的 `aes128.py` —— 当判据，
所以必须先把两条官方向量跑通；不通过就**直接非零退出**（宁可说"验收器不可信"，
也不给一个错的 PASS）。

命令行参数：`--key-file <密钥文件>`（验加密产物时必需：驱动 A1/A6/A8 的解密比对；
解密走**脚本内纯 Python AES**，**不依赖 openssl**）、`--clear-dir <目录>`（默认
`<输出目录>/_clear`）、`--expect-encrypted`（要求分段式产物**必须**是密文，
加密流水线请务必带上它）。`openssl` **只在机器上装了的时候**作为可选交叉验证
（两条独立实现解出同一份明文）；**缺它不影响任何 DRM 断言的成立与否**。

**断言清单（一期）：**

| # | 断言 | 实现位置 | 为什么 |
|---|---|---|---|
| A0 | **（施工新增）AES 原语可信**：两条官方向量（FIPS-197 C.1 单分组 / NIST SP 800-38A F.2.1 CBC 首块）全部通过，否则**不给任何验收结论** | `aes128.self_test()`，在 `main()` 开头调用 | 验收的尺子本身必须先校准；否则"能解回来"这个判据毫无意义 |
| A1 | **分段式分片是密文**：每个分片用密钥 AES-128-CBC（脚本内实现）解密后，**必须**得到合法 MP4 box 头（前 4 字节是**大端长度**、接着 4 字节是可打印 box type） | `decrypt_segment()` + `mp4_plaintext_head()`（需 `--key-file`） | 用"结构可解"当"确实加密了"的判据；同时能抓"加密了两次" |
| A2 | **密文与 sidx 范围一致（仅当产物含 sidx 时）**：一期分段式产物**不应有 sidx** ⇒ 这条由"A10 单文件必须明文"承担哨兵职责（若误加密单文件，A10 与 `find_sidx` 那批断言会立刻炸） | `check_drm_not_encrypted()` + 既有 sidx 断言 | 防止把单文件模式也"顺手加密"（那会让 §4.2 的所有范围作废） |
| A3 | **每个清单分片都能找到对应的 `#EXT-X-KEY`**：`media_*.m3u8` 的 `#EXTINF` 条数 == 清单分片行数 == 目录里真实存在的分片文件；第一个 `#EXTINF` 之前**必须**有一行 `#EXT-X-KEY:METHOD=AES-128` 且带 `URI=` 与 `IV=0x…` | `check_drm_encryption()` | 清单与产物**一一对应**；`METHOD=AES-128` 是内核唯一入口条件（`HlsParser.cpp:304-306`） |
| A4 | **`#EXT-X-KEY` 里不许出现 `KEYFORMAT`**（或只许是 `identity`） | 同上 | 防 §4.4.1 的静默降级（`HLSStream.cpp:546`） |
| A5 | **每个分片 IV 唯一且与序号同源**：IV 唯一；且**最后 4 字节按大端解出来的值 == 该分片的 HLS 序号**（= `#EXT-X-MEDIA-SEQUENCE` + 下标） | 同上 | 校验 §4.1 的 IV 口径；重复 IV 是 CBC 的致命错误 |
| A6 | **用密钥解密后与未加密产物逐字节相同**：明文对照留在 `<输出目录>/_clear/`，逐个分片 `AES-128-CBC(IV=该分片IV) 解密 → 去 PKCS7 → 逐字节比对`（另附 SHA-256 摘要便于定位）；装了 openssl 时再做一次交叉验证 | `check_drm_encryption()`（需 `--key-file`）+ `crosscheck_with_openssl()`（可选） | **最强的端到端证据**：证明"加密只是重编码字节，媒体内容一字未改" |
| A7 | **密文长度满足 CBC 块对齐**：`len(cipher) % 16 == 0`，且 `len(cipher) - len(clear)` ∈ `[1, 16]`（PKCS7 补的 1..16 字节） | `check_drm_encryption()` | 长度不合规 ⇒ 播放器一定解错（内核 `AES_128Decrypter.cpp:56-83` 按块算） |
| A8 | **mp4 box 完整性（明文侧）**：解密后的 `init` 段必须有 `ftyp`+`moov`；媒体段必须有 `moof` 且 `moof` 与 `mdat` 数配对 | 同上（需 `--key-file`） | 抓"解出来是垃圾"（密钥/IV 配错） |
| A9 | **清单字段与产物一一对应**：`test-{hls,dash}-<版本>[-explicit].json` 顶层 `encryption.keyUrl` == `#EXT-X-KEY` 的 `URI`；`encryption.iv` == **首个分片的 IV**；且**不许有 `keyFormat`** | 同上 | 三处（HLS 文本 / JSON / 交付）口径必须一致 |
| A10 | **SegmentBase 产物必须是明文且清单不许有 `encryption`**：`test-{dash,hls}-<版本>-segmentbase.json` **不得**含 `encryption`/`licenseServer`；单文件的 `.m3u8` **不得**有 `#EXT-X-KEY`；`master-segmentbase.m3u8` **不得**有 `#EXT-X-SESSION-KEY`；单文件**前 8 字节必须是 `ftyp`**（明文 fMP4 的固定开头） | `check_drm_not_encrypted()` | §4.2 的显式选择必须被断言钉死，杜绝"以为加密了/以为没加密" |
| A11 | **clear-lead / clear-tail 约定（仅二期 CENC 用）**：一期不适用（没有 SAMPLE-AES）。**一期不写 TODO 空断言位**——空的断言位会让人误以为"已经检查了"；二期上 `SAMPLE-AES` 时再加 | — | 内核样本级解密器**硬性假设**了这个布局，二期必须做 |
| A12 | **`master.m3u8` 的 `#EXT-X-SESSION-KEY`**：`METHOD` 与 `URI` 必须和媒体播放列表里的 `#EXT-X-KEY` **完全一致**；`master-segmentbase.m3u8` 不许有它 | 同上 | 不一致会让浏览器选错密钥 |
| A13 | **（施工新增）声明与产物同真同假**：分段式 JSON 声明了 `encryption` ⇒ `media_*.m3u8` 必须有 `#EXT-X-KEY`；反之亦然；`*.enc.tmp` 临时名不许残留；`_clear/` 里的文件必须与分片**一一对应** | `check_drm_declaration_consistent()`、`check_no_enc_tmp()` | §6 红线 1、2 的直接落地 |

**验收执行方式**（与现有惯例一致）：

```bat
python verify_segmentbase.py <输出目录> <版本号> --expect-encrypted ^
       --key-file <密钥文件>
```

任一 `problems` 非空 ⇒ **非零退出**。加密流水线请带 `--expect-encrypted`
（否则"明文产物"不会被判 FAIL，只是打一条 note —— 因为同一条断言清单要能同时验收
明文流水线与加密流水线）。

### 4.7 分期计划与回滚

**一期（本设计）—— 目标：HLS AES-128 端到端跑通**
（下表是**设计初稿的计划**；实际落地情况与偏差见 §4.8 的施工记录。）

| 序 | 改动 | 落点 | 量级 | 落地情况 |
|---|---|---|---|---|
| 1 | 密钥参数化（`--key-file` / `--key-env` / 权限 0600） | `transcode_all.py` 配置区 + `main()` | 小 | **改成单个 `--hls-key-info <文件>`**（理由见 §4.1；权限改为手工 `chmod 600`，脚本只读） |
| 2 | 新增加密阶段 `run_encryption()`，插在 `rename_files()` 之后 | `transcode_all.py`（新函数） | 中 | 已落地；另**新增** `verify_keyframe()` 也要在加密之前这条顺序约束 |
| 3 | HLS 清单写 `#EXT-X-KEY`（媒体）+ `#EXT-X-SESSION-KEY`（主） | 复用 `_hls_attr/_hls_set_attr` | 小 | 已落地（另有专门的 `_insert_key_lines()` 处理标签跨度顺序） |
| 4 | MPD 插 `ContentProtection` | `rename_files()` 里 `fill_mpd_codecs` 附近 | 小 | **改到 `run_encryption()` 末尾**（不是 `rename_files()` 里）：因为 `run_segmentbase_remux()` 在 `rename_files()` 之后跑，必须让它读到"还没有 ContentProtection"的 MPD |
| 5 | 内置兜底 JSON 补 `encryption`（或加密启用时禁用兜底并报错） | `build_json_manifest()` / `write_json_manifests()` | 小 | **两条都做了**（补字段 + 加密模式下禁用兜底） |
| 6 | `LICENSE_SERVER_URL/CONTENT_ID` 改成参数（可选） | `convert-to-manifest.py` | 小 | **未做**（一期用静态 key 文件，不需要 License Server；见 §4.4.2 阶段 0） |
| 7 | 新增 `verify_drm.py`（A1~A10, A12） | 新文件 | 中 | **并进 `verify_segmentbase.py`**（理由见 §4.6），新增 A13 |
| 8 | **web：`_buildLevelDetails` 消费 `playlist.encryption`** | `hls.mjs:38934-38993` | 小（但必修） | 已落地（施工后 `:38993-39041`） |
| 9 | **内核：加密记录被全部跳过时打 ERROR 日志**（§4.5.3） | `HLSStream.cpp:545-550` | 极小 | **未做**（见 §4.8.5 的说明：本任务硬约束"非必要不动内核"，这条是"可观测性增强"，不影响一期能否播） |

**内核改动合计 ≈ 5 行日志**（一期实际改动 **0 行**，见 §4.8.5）；其余全在脚本与 web。
**一期不改任何解密算法。**

**二期（CENC / 平台 DRM）**：明确需要时再做，见 §3.2 / §3.3。二期必然要面对
§5.1（PSSH/KID 一致性）、§5.3（桌面无 CDM）、以及 HEVC 软件样本解密的缺失
（`HLSSampleAesDecrypter.cpp:54-77`）。

**回滚方式**

1. **施工期回滚**：加密阶段是**独立函数**，且**先写临时名、全成功才原子替换**
   ⇒ 直接把 `run_encryption()` 这一行调用去掉，产物就回到今天的样子；
2. **产物级回滚**：主转码命令与 SegmentBase 再封装命令**一个参数都不改**
   （沿用 `transcode_all.py:431-440` 的既有取舍），所以**明文产物永远还在**，
   密文是"另一份"或"替换后的同一份"——按 A6 保留 `_clear/` 对照即可随时换回；
3. **清单级回滚**：`#EXT-X-KEY` / `ContentProtection` / JSON `encryption`
   都是**可加可减的声明**，删掉即变回明文语义（**前提是同时把密文换回明文**——这两件事必须一起做，见 §6）；
4. **web 回滚**：`_buildLevelDetails` 的改动对"清单里没有 `encryption`"的片源**完全无影响**
   （施工后实际是 `if (encryption && encryption.keyUrl) { … }` 一段**条件块**，
   不是早退；对"清单里没有 `encryption`"的片源仍然是**一个字节都不改**，风险同样极低）；
5. **git 层面**：脚本、web、内核三处改动彼此不耦合，可分别 revert。
   （本期内核**零改动**，见 §4.8.5。）

---

### 4.8 一期施工记录（**本次实现**：哪些是实测、哪些仍是待验证）

> 本节是"施工后回写"，用来和前面的设计对照。所有行号都是**施工后**的行号；
> 【已证实】= 本次真的读过该文件该行或该结论由代码结构直接推出；
> 【待验证】= 本机无法运行（本会话 shell 不可用：不能跑 ffmpeg / python / 编译），
> 必须由你在本机用 §4.8.7 的命令实测。

#### 4.8.1 改了哪些文件（改动点 + 语义）

| 文件 | 位置 | 语义 |
|---|---|---|
| `D:\hilihili\转码脚本\aes128.py` | **新文件**（整份） | **真正的 AES-128 原语**（纯 Python，FIPS-197）：查表构造（`_build_mul_tables`、`SBOX`、`INV_SBOX`、`RCON`）、`expand_key()`、`encrypt_block()` / `decrypt_block()`、`pkcs7_pad()` / `pkcs7_unpad()`、`aes128_cbc_encrypt()` / `aes128_cbc_decrypt()`、两条官方向量的 `self_test()`（`:391-441`），以及 `python aes128.py` 的命令行自检入口 |
| `D:\hilihili\转码脚本\transcode_all.py` | `:100-139` | 新增加密配置区：`HLS_KEY_INFO_ARG = "--hls-key-info"`、`HLS_CLEAR_COPY_DIR = "_clear"`、`HLS_CLEAR_COPY_ENABLE`、`HLS_KEY_MIN_BYTES = 16`、`MPD_AES128_SCHEME_ID`（**没有**任何布尔开关） |
| 同上 | `:196-202` | 加密状态全局：`HLS_KEY_INFO_PATH/KEY_URI/KEY_BYTES/IV_BASE/IV_BASE_STR/ENCRYPTED`（与 `BASE_ID`/`REP_MAP` 同一风格） |
| 同上 | `:287-384` | **原语换成 `aes128.py`**（import 前把脚本目录补进 `sys.path`）+ 加密侧封装：`aes128_self_test` / `_xor_bytes` / `_iv_for_index` / `aes128_cbc_encrypt` / `aes128_cbc_decrypt` / **`roundtrip_self_test`** / `pkcs7_unpad`（旧的 `_aes128_backend_probe` / `_aes128_encrypt_block`（PBKDF2 版）**已删除**） |
| 同上 | `:1827-1956` | 内置兜底 JSON 认识加密：`build_json_manifest()` 返回值经 `_with_encryption()`；新增 `_json_encryption()` / `_with_encryption()` / `_assert_segmentbase_plain()` / `_assert_segmentbase_not_encrypted_on_disk()` |
| 同上 | `:1963-2072` | SegmentBase 清单断言：`generate_segmentbase_json()` 与 `_sb_hls_segmentbase_manifest()` 里钉死"single 不许有 encryption/licenseServer" |
| 同上 | `:2075-2242` | `write_json_manifests()` 写 encryption；`generate_json_manifests()` 改成"权威实现 → 缺什么补 → `_review_json_manifests()` 核"，**加密模式下禁用兜底**（不完整即抛错） |
| 同上 | `:2244-2401` | `rename_files()`：**不再生成 JSON 清单**，返回值改成 `(segmentbase_ok, mpd_manifest)`（JSON 要等加密之后才生成） |
| 同上 | `:2404-2710` | **新增加密阶段**：`_seg_name_from_url/_parse_media_playlist/_found_key_names/_insert_key_lines/_write_media_playlist/_write_master_session_key/insert_mpd_content_protection/run_encryption` |
| 同上 | `:2857-2905` | `main()`：**第 0 步跑官方向量自检**（`:2860-2878`）→ 密钥装载 → **往返自检** → 其余接线；`_parse_cli_key_info()` 在 `:2840` 附近 |
| `D:\hilihili\转码脚本\verify_segmentbase.py` | `:60-91` | 新增常量与 **`aes128.py` 的 import**（与 transcode_all.py 共用同一份实现） |
| 同上 | `:700-800` | 新增验收函数：`parse_media_playlist/parse_hex_bytes/mp4_plaintext_head/`**`aes128_self_test`/`decrypt_segment`/`crosscheck_with_openssl`**`/check_drm_encryption/check_drm_not_encrypted/check_no_enc_tmp/check_drm_declaration_consistent`（旧的 `decrypt_segment_with_openssl` / `decrypt_segment_from_bytes` **已删除**） |
| 同上 | `:1214-1260` | `main()`：`--key-file` / `--clear-dir` / `--expect-encrypted` 参数；**第 0 步跑官方向量自检**，不通过直接 `return 1` |
| `D:\hilihili\front\hili-player\packages\plugins\src\hls\vendor\hls.mjs` | `:38993-39041` | **对象注入路径补上 AES-128 消费**：`_buildLevelDetails()` 末尾读 `playlist.encryption` → `new LevelKey('AES-128', keyUri, keyFormat, keyFormatVersions, iv)` → 给 `details.fragments[*]` 与 `initSeg` 挂 `levelkeys` |
| `D:\hilihili\front\hili-player\packages\plugins\src\hls\vendor\hls.d.ts` | `:3502-3526` | `ManifestEncryption` 补文档：消费点、`keyFormat` 必须留空/`identity`、`iv` 的 hex 口径 |
| `D:\hilihili\CicadaPlayerNext\docs\DRM-ENCRYPTION-DESIGN.md` | §3.1 / §4.1 / §4.2 / §4.3.2 / §4.3.3 / §4.5 / §4.6 / §4.7 / §4.8 / §7.3 / §7.4 | 把与实现不一致的地方回写（工具链选择、参数名、插入点、A11/A13、**AES 原语**） |

**没有改动的**：`convert-to-manifest.py`（权威清单实现本来就认识 `#EXT-X-KEY`）、
`_sb_name_selfcheck.py`、以及 **`CicadaPlayerNext` 下的任何 C++ 代码**（见 §4.8.5）。

#### 4.8.2 参数与密钥/IV 来源（**参数，不是开关**）

```bat
python transcode_all.py --hls-key-info <key_info 文件>
```

* **不给这个参数** ⇒ 明文流水线：不加密、不写任何加密声明，日志明确打印
  "本次产物**全部为明文**"；
* **给了但缺值 / 文件不存在 / 行数不对 / 密钥长度不是 16 字节 / IV 不是 32 hex** ⇒
  **立刻非零退出**（在任何转码动作之前校验），**绝不回落到明文**；
  AES 原语的**两条官方向量自检**更早跑（`main()` 第 0 步），不通过同样一个产物都不产；
* 不认识的命令行参数（例如有人写 `--enable-drm`）⇒ 报错退出（免得"以为加密了其实没加"）。

**key_info 文件的三行格式**（= HLS/ffmpeg 生态的 `key_info_file`）：

| 行 | 内容 | 说明 |
|---|---|---|
| 1 | 密钥 **URI** | **写进清单**的相对/绝对地址。**它指向什么**取决于你：一期阶段 0 就是与分片同目录的静态文件 `key.bin`（播放器对它发一次普通 HTTP GET，**期望响应体正好 16 字节裸密钥**：内核 `HLSStream.cpp:812-829`、web `hls.mjs:36838`）。阶段 1 可以把它换成"简易 key 服务的端点"（§4.4.2），**清单格式不用变**。这块**不是**许可证服务：AES-128 不需要 CDM、不需要 EME |
| 2 | 密钥文件 **本机路径** | 32 个 hex 字符，或 16 字节裸二进制；相对路径按**脚本所在目录**解析。**绝不进清单、绝不进日志**（日志只打 SHA-256 前 8 字节指纹） |
| 3 | **IV**（可省略） | `0x` + 32 hex。省略 = 全 0。脚本把**最后 4 字节**当基序号，第 N 片用"前 12 字节原样 + (基 + N) 的大端 4 字节" |

**IV 的唯一性与"同源"**：全 0 基 IV 时，第 N 片的 IV = `00…00` + N 的大端 4 字节 ——
**与内核 `HLSStream.cpp:835-853`、web `createInitializationVector()` 的隐式推导逐位相同**
（【已证实】读了这三处推导代码）。这就是"A5 每分片 IV 唯一且与序号同源"能成立的原因。

#### 4.8.3 为什么加密不用 ffmpeg（与设计初稿的差异 + 理由）

* 【已证实】主转码是 `-f dash` + `-hls_playlist 1`（`transcode_all.py:526` 起的 `build_ffmpeg_cmd()`，
  DASH 输出段在 `:655-672`），**不是** `-f hls` muxer ⇒ `-hls_key_info_file`（hls muxer 私有选项）
  **确认不可用**。设计初稿 §7.4 的待验证项 1 → **结论：不可用**。
* 【已证实（选项语义）】即使另起一趟 `ffmpeg -f hls -hls_key_info_file`，
  它也只能给**整条播放列表一个 IV**，与"每分片 IV 唯一"（A5）冲突；
  而且还要处理"每变体一个 hls 输出怎么命名 init/分片"的额外问题
  （`%v` 之类的模板变量在本机版本上是否可用**又是**一条待实测项）。
* 于是采用**第三种**：脚本自己做字节级 AES-128-CBC + PKCS7。
  加密原语 = **`转码脚本/aes128.py`**（纯 Python，FIPS-197 AES-128）：
  `expand_key()`（密钥扩展，11 组轮密钥）、`encrypt_block()` / `decrypt_block()`、
  `aes128_cbc_encrypt()` / `aes128_cbc_decrypt()`、`pkcs7_pad()` / `pkcs7_unpad()`，
  再加 `self_test()` 跑两条官方向量。
  **加密端（`transcode_all.py`）与验收端（`verify_segmentbase.py`）import 的是同一份实现**，
  所以不存在"两份实现漂移导致验收形同虚设"。
  * ❌ 不用 `hashlib.pbkdf2_hmac` 之类"借 OpenSSL 后端凑"的做法 ——
    **这是本次施工中真实犯过的错误**：PBKDF2-HMAC-SHA256 是**密钥派生函数**，
    与 AES 分组密码在数学上无关，永远不可能输出 FIPS-197 的 `69c4e0d8…`。
    用它"加密"出来的不是 AES 密文，播放器必然解不出来。已在 `transcode_all.py:293-297`
    与 `aes128.py` 文件头留了记号，**不要走回去**；
  * ❌ 不用 `hashlib.new('aes-128-cbc')`：那个名字只在个别发行版的 OpenSSL 里有；
  * ❌ 不用 `ctypes` 调 libcrypto：跨平台 DSO 名字不稳定；
  * ❌ **不用 `openssl` 命令**做加密/解密：用户机器上有没有 openssl 尚未确认，
    交付不能建立在未确认的外部命令上；而且 `openssl enc` 是"一条流一个 IV"，
    表达不出本工程要的"每分片唯一 IV"。

**【已证实】两条官方向量自检（`aes128.self_test()`，`aes128.py:391-441`）**：

| # | 向量 | 输入 | 期望 |
|---|---|---|---|
| ① | FIPS-197 C.1（AES-128 单分组） | key `000102030405060708090a0b0c0d0e0f`，明文 `00112233445566778899aabbccddeeff` | 密文 `69c4e0d86a7b0430d8cdb78070b4c55a` |
| ② | NIST SP 800-38A F.2.1（AES-128-CBC 首块） | key `2b7e151628aed2a6abf7158809cf4f3c`，IV `000102030405060708090a0b0c0d0e0f`，明文首块 `6bc1bee22e409f96e93d7e117393172a` | 密文首块 `7649abac8119b246cee98e9b12e9197d` |

每条向量**双向都验**（加密得到期望密文 + 解密回到原文）；任一条不符 ⇒ 抛异常。
调用时机是硬前置，两个脚本都在**任何转码/判定动作之前**跑：

* `transcode_all.py:2860-2878`（`main()` 的第一件事，早于密钥解析与 ffprobe）；
* `verify_segmentbase.py:1225-1243`（在任何 DRM 断言之前；不通过就**直接非零退出**，
  连"验收器不可信"都如实说出来，而不是给一个错 PASS）。

**【已证实】往返自检（`transcode_all.py:361-379` 的 `roundtrip_self_test()`）**：
用**用户这一把真密钥**对三种长度（16 / 48 / 17 字节）做"加密 → 同实现解密 → 逐字节比对"，
失败即非零退出（在 `main()` 里紧跟 `load_hls_key_info` 之后跑）。
它证明"本实现加出来的东西本实现能解回来"，也顺带证明了 PKCS7 与 CBC 链式自洽。

#### 4.8.4 已知代价与实测项

* **性能**：加密是纯 Python 逐分组实现，比"走 C 的 openssl/hashlib"慢一个数量级。
  【推测】量级是每秒几十 KB ～ 几百 KB（取决于机器与实现），一条 4K 片段可能几 GB，
  所以加密阶段可能耗时**较久**（脚本会打印处理了多少 MB，便于实测这一档）。
  **这是刻意接受的代价**：§5.5 红线 2 禁止"为了省时间动媒体内容"，
  而"每分片唯一 IV"（A5）又排除了"一条 `openssl enc` 命令搞定 8 路"的省事做法。
  若这一档代价不可接受，正确做法是**换工具链**（Bento4 / shaka）而不是放松断言。
  ⚠ 这条**必须实测**（§4.8.7 第 5 条）：如果慢到不可用，一期要么接受，要么换工具链。
* **磁盘**：默认额外留一份明文对照 `_clear/`（与分段式分片等量）。
  这是 A6 的工具，也是"随时切回明文"的回滚材料；**同步到 CDN 时必须排除**。
  `transcode_all.HLS_CLEAR_COPY_ENABLE = False` 或 `HLS_CLEAR_COPY_DIR = ""` 可关掉
  （同时 A6 会自动跳过，不会假通过）。

#### 4.8.5 内核为什么这次没动（§4.5.3 的第 9 项没做）

设计稿 §4.5.3 要求"加密记录被全部跳过时打一条 ERROR 日志"（约 5 行）。
本次**没有改内核**，理由：

1. 一期 AES-128 的清单**不写 `KEYFORMAT`** ⇒ `HLSStream.cpp:546` 的
   `keyFormat.empty()` 分支**无条件接受**这条加密记录 ⇒ **不存在"被全部跳过"的路径**
   （【已证实】读了 `HLSStream.cpp:545-550`）；这条日志在"严格遵守 §4.3.2 口径"时**永不触发**。
2. 它属于**可观测性增强**，不影响"一期能不能播"；而本次任务有硬约束
   "`mediaPlayer/`、`framework/` 非平台代码不得出现平台宏；若认为必须动内核，先停下并说明理由"。
   既然不是必须，就**不动内核**，把这条留给需要的同学按 §4.5.3 单独施工。
3. 同理，`updateKey()` 的错误码复用（§4.5.3 第 2 条）也**没做**：现有行为
   （`open key file error` / `key size is %d not 16` + 负返回值）能定位问题，
   一期不新增任何错误码语义。

#### 4.8.6 一期实现的**实际数据流**（与 §4.0 的差异标注）

```
input.mp4
  │
  ├─①主转码（明文，命令与今天完全一致：output.mpd + master.m3u8 + media_*.m3u8 + *.m4s）  [未改]
  ├─②SegmentBase 再封装（明文，-c copy，命令与今天完全一致）                               [未改]
  ├─③重命名与清单收尾（复用 rename_files；**不再在这里生成 JSON**）                        [改了返回值]
  ├─④关键帧抽查（verify_keyframe）——★必须在加密之前（它要 ffprobe 解复用明文分片）        [顺序约束]
  ├─⑤加密阶段 run_encryption（**仅当给了 --hls-key-info**）                                [新增]
  │      · media_*.m3u8 引用的分片：明文 → _clear/ 备份 → AES-128-CBC+PKCS7 → .enc.tmp → 原子替换
  │      · 每个 media_*.m3u8 的第一个 #EXTINF 之前插 #EXT-X-KEY
  │      · master.m3u8 插 #EXT-X-SESSION-KEY
  │      · output.mpd 每个 AdaptationSet 插 ContentProtection
  ├─⑥JSON 清单（convert-to-manifest.py 从带 #EXT-X-KEY 的 media_*.m3u8 读 encryption；       [移到最后]
  │     加密模式下**禁用**内置兜底：不完整即非零退出）
  └─⑦verify_segmentbase.py --expect-encrypted --key-file <key>                              [新增断言]
```

**只加密分段式产物**：`output-segmentbase.mpd` / `<单文件>.m4s` / `<单文件>.m4s.m3u8` /
`master-segmentbase.m3u8` / `test-*-segmentbase.json` **一个字节都不动**
（转码侧有 `_assert_segmentbase_plain()` + `_assert_segmentbase_not_encrypted_on_disk()` 两道断言）。

#### 4.8.7 【待验证清单】—— 本机跑一条命令就能验

> 本会话 **shell 不可用**（不能跑 ffmpeg/python/编译），所以下面每一条都**没有本地实测**。
> 编号越小 = 风险越高。

**先跑这一条（一条命令判定"整期是否成立"）**：

```bat
python D:\hilihili\转码脚本\aes128.py
```

期望输出：两条向量（FIPS-197 C.1 单分组、NIST SP 800-38A F.2.1 CBC 首块）逐条打印，
最后一行 `[OK] 两条向量全部通过；本实现可用于加解密`，退出码 0。
（**注意**：自检/加密**不依赖 openssl**；只有"可选交叉验证"会在装了的时候跑。）

| # | 待验证的事 | 最小验证命令 | 期望结果 |
|---|---|---|---|
| **1** | **AES-128 原语本身**（已经换掉那次 PBKDF2 的错误实现；现在要确认这份纯 Python 实现真的对） | `python D:\hilihili\转码脚本\aes128.py` | 打印两条向量并 `[OK] …`；**退出码 0**。若 `[FAIL]`：`transcode_all.py` 会因此**拒绝产出任何产物**（这是设计好的行为），把 FAIL 那行原文发我 |
| **2** | **转码 + 加密整条流程真的能跑通**（唯一真正重要的端到端验证） | 造 key：`echo -n 000102030405060708090a0b0c0d0e0f > keys\v4.key`；写 `keys\v4.info` 三行（`key.bin` / `D:/hilihili/keys/v4.key` / `0x00000000000000000000000000000000`）；然后 `python transcode_all.py --hls-key-info keys\v4.info` | 日志先出现两条向量的 `[OK]` 与 `[OK] 往返自检通过`，再出现"HLS AES-128 整片加密 / 加密完成：N 个密文分片"，**退出码 0**，目录里出现密文分片 + `_clear/` |
| **3** | **密文与明文逐字节一致（A6，最强证据）** | `python verify_segmentbase.py <输出目录> v4 --expect-encrypted --key-file keys\v4.key` | `SEGMENTBASE-VERIFY: PASS`，notes 里有 A6/A8/A9/A10/A12 的行。**没有 openssl 也一样 PASS**（解密走脚本内实现；只多一条"跳过可选交叉验证"的 note） |
| **4** | **手动复核一个分片**（不依赖验收脚本；验证"内核之外第三方也能解"） | 从 `media_0.m3u8` 取该片 `IV=0x…`（首片是全 0 + 序号），然后<br>`openssl enc -d -aes-128-cbc -K 000102030405060708090a0b0c0d0e0f -iv 00000000000000000000000000000001 -in <分片>-1.m4s -out plain.m4s`<br>`fc /b plain.m4s _clear\<分片>-1.m4s` | 无差异（`fc` 报"找不到差异"）。**这条是"原语与 OpenSSL 逐位等价"的独立证据**（脚本内自检用的是官方 NIST 向量，两者互相印证） |
| **5** | **性能量级**（决定一期是否可接受） | 看转码日志里"密文分片 N 个 / 合计 X MB"与加密阶段前后时间戳 | 【推测】纯 Python 实现比 C 慢一个数量级；若慢到不可接受，见 §4.8.4 的结论（换工具链，**不要**放松"每分片唯一 IV"） |
| **6** | **内核能播**（HLS 文本路径）——【一期最关键的运行时待验证】 | 起本地 http 服务（如 `python -m http.server 9000`），用本工程内核播 `master.m3u8`（或对象清单 JSON） | 正常播放、不花屏；seek 正常（解密路径的 seek 靠换分片重建，§5.4） |
| **7** | **web 对象注入能播**（`_buildLevelDetails` 补丁生效） | 前端按既有方式注入 `test-hls-v4.json`（baseUrl 指向密文目录 + 能取到 `key.bin`） | 正常播放；控制台能看到 hls.js 去拉 `key.bin`（且响应体是 16 字节） |
| **8** | **逐片 IV 与内核推导一致**（"显式 IV == 播放器隐式推导"这条双保险是否真成立） | 用内核播放时抓 `#EXT-X-KEY` 的 IV 与内核 `HLSStream::updateIV()` 推出来的值；或离线：对第 N 片比较 `IV.last4 == N`（验收脚本 A5 已自动化这条） | A5 通过（验收脚本已断言"IV 尾 4 字节 == 该片序号"）。真机播放正常即为最终证据 |
| **9** | **`#EXT-X-SESSION-KEY` 不被内核误伤** | `grep -n "SESSION-KEY" CicadaPlayerNext/framework/demuxer/play_list/HlsTags.*` | **无命中** ⇒ 内核不解析它、被忽略（【已证实】本次读过：`HlsTags.h:78-91` 枚举里没有这个 tag） |
| **10** | **web 端 `encryption.iv` 的 hex 解析** | 造一条 iv 带 `0x` 前缀 / 大写 hex / 短 hex 的清单，看控制台 | 合法的一律解析成 16 字节；不合法的打印 `[manifest] invalid IV …` 并**回退到按序号推 IV** |
| **11** | **`convert-to-manifest.py` 对加密清单的处理** | `python convert-to-manifest.py <输出目录> <输出目录> v4 http://127.0.0.1:9000/video/dash2/`，再看 `test-hls-v4.json` | 顶层出现 `encryption: {"keyUrl": "key.bin", "iv": "000…0"}`，且**没有** `method`/`keyFormat`（【已证实】读了 `:143-162`、`:738-753`） |

**风险最大的两条（按影响面排序）**：

1. **#1 AES 原语正确性**：如果不成立，"加密"出来的密文任何播放器都解不了 —— 整期白做。
   它同时也是**最容易验的**（`python aes128.py`，一条命令）。本次已经把那次错误的
   PBKDF2 实现换掉，但**这份纯 Python 实现同样必须在真机上跑过向量**才算数。
2. **#2/#3 端到端与逐字节一致**：把 #1 的结论放到真实产物上复核；
   尤其 PKCS7 行为必须与内核 `AES_128Decrypter` 的"末尾减填充"对上
   （【已证实】读了 `AES_128Decrypter.cpp` 的 `mOutData -= padding`），
   否则每个分片尾部会多/少 1~16 字节。

**如果这次真的上不了（自检/验收不过）**，如实停在这里，不要绕过：
`transcode_all.py` 与原语自检是绑死的 —— 自检不过就一个产物都不产（§6 红线）。

#### 4.8.8 【已证实（本次读了代码）】 与 【推测/待验证】的明确分界

**已证实**：

* 主转码用 `-f dash`（不是 `-f hls`）⇒ `-hls_key_info_file` 不适用（`transcode_all.py:655-672`）；
* 内核 `HlsParser`/`HLSStream`/`AES_128Decrypter` 的 AES-128 通路与 IV 推导口径
  （`HlsParser.cpp:294-355`、`HLSStream.cpp:545-550,789-853`）；
* 内核**不解析** `#EXT-X-SESSION-KEY`（`HlsTags.h:78-91` 无该 tag）；
* 内核 `MPDParser` **不解析** `ContentProtection`（`MPDParser.cpp:343-433`）
  ⇒ 写在 `output.mpd` 里的那条声明对内核"URL 直连 .mpd"路径无作用；
* `convert-to-manifest.py` 会从 `#EXT-X-KEY` 生成顶层 `encryption` 并剔除 `method`
  （`:143-162`、`:738-753`）；
* web 文本路径的最小充分改动点是"给 Fragment 挂 `levelkeys`"
  （`hls.mjs:711-733` 的 `decryptdata` getter、`:7478-7499` 的 `getDecryptData`、
  `:8053-8076` 的 `#EXT-X-KEY` 分支、`:8397-8405` 的 `setFragLevelKeys`）；
  **本 fork 的 `LevelDetails` 没有 `.key` 字段**（`:7028-7082` 的构造函数里没有），
  所以按文本路径的做法挂 `levelkeys`，而不是设计稿说的"挂 `details.key`"；
* 脚本侧改动本身（加密/声明/断言）——**读的是我自己写的代码**，逻辑自洽性已逐行复核；
  **但 `aes128.py` 这份 AES 实现属于"按标准写的、尚未运行"**：它的正确性只能由
  `python aes128.py` 的两条官方向量证明（见 §4.8.7 #1）。在跑过之前，
  "加密产物能被解出来"这件事**仍未证实**。

**推测/待验证**：

* **§4.8.7 全部 11 条**（本机没跑过任何 ffmpeg/python/openssl/编译）；
  其中**最关键的三条**：真机播放内核 `master.m3u8`（#6）、web 对象注入拉 `key.bin`（#7）、
  逐片 IV 与内核推导一致性（#8，脚本侧 A5 已自动化，真机播放是最终证据）；
* **`aes128.py` 这份纯 Python 实现本身**也是"已读代码、未运行" —— 它按 FIPS-197 写，
  并自带两条官方向量自检（跑 `python aes128.py` 即知）。**在真机跑过向量之前，
  不能把它当已证实**；
* ffmpeg 的 `-f dash` 是否会在 MPD 里因为多了一个
  `<ContentProtection/>` 子元素而解析失败 ——【推测】不会（dashenc 只在第一次产出时写 MPD，
  我们的插入发生在所有 ffmpeg 调用**之后**；但"以后再跑 `-i output.mpd`"的场景没试过）。
  **这一条请用 #2 的命令覆盖**：`run_segmentbase_remux()` 就在加密之前跑，
  如果顺序被将来的改动换错，`ffmpeg -i output.mpd` 会因为 `ContentProtection` 报错 ——
  这也是**把顺序写进注释**的原因；
* `#EXT-X-SESSION-KEY` 对某些**商用播放器**是否有副作用 ——【推测】没有
  （RFC 8216 定义它的语义就是"预取提示"），但本项目只实测内核与 web；
* 纯 Python AES 的**性能**（#5）：【推测】每秒几十 KB ～ 几百 KB，一条 4K 片段可能很慢。
  必须实测；若不可接受，正确做法是换工具链而不是放松"每分片唯一 IV"。

---

## 5. 风险与边界

### 5.1 PSSH / KID 在 MPD 与 init 段不一致会怎样（二期主风险）

**【已证实】** 三方数据源：
`MPD` 的 `<ContentProtection cenc:default_KID=…><cenc:pssh>…`、
`init` 段 `moov/pssh` + `tenc`（含 KID）、以及**许可证服务器签发时的 KID**。

* 内核读的是清单（`ManifestDemuxer.cpp:176-178` → `SegmentEncryption::pssh/keyId` →
  `Stream_meta.drmPssh/drmKeyId`，`HLSStream.cpp:1519-1520`）⇒
  **如果清单写了一个 KID、init 段里是另一个**，内核会把错的 KID 交给 CDM，
  签发回来的密钥与 `tenc` 里的 KID 不匹配 ⇒ **解密失败但表现极其模糊**：
  可能是"能建会话、能拿到 license、但样本解出来是垃圾"（花屏），
  也可能是解码器直接报 `STATUS_DRM_ERROR`（`IDecoder.h:46`）。
* **可观测性差**：`DrmUtils::isSupport()` 非 Android 恒 false（`DrmUtils.cpp:7-12`），
  **跳过时无日志** ⇒ 现场最难查的一类问题。
* **边界对策（写进二期验收）**：
  1. 脚本侧断言"MPD 的 `default_KID` == 从 init 段 `tenc` 里读出的 KID"（**必须做**，可脚本化）；
  2. 脚本侧断言"MPD 的 `pssh` 与 init 段的 `moof`/`moov` 里的 `pssh` 至少有一个系统一致"；
  3. 一期不做 CENC ⇒ 本条**不影响一期**，但要在文档里留位置。

### 5.2 key 并发请求

**【已证实】** 内核每个 `HLSStream` 实例有自己的 `mSegKeySource` / `mKey`
（`HLSStream.cpp:799-833`），并且 `mKeyUrl == keyUrl` 时**直接跳过重复拉取**
（`:794-796`）。⇒

* **同一个档位/同一个 HLSStream**：一次拉取，之后复用 —— 不会并发；
* **多档位（6~8 路视频 + 1 音频）同时播放/切换**：每路一个 `HLSStream`，
  **每个会各拉一次同一个 key URL** ⇒ 一次冷启动可能有 **N 次并发请求**。
  对静态文件/CDN 无所谓；对"简易 key 服务"要注意：
  * 要么让 key 服务天然幂等（GET + 无状态）；
  * 要么在 key 响应上放 `Cache-Control: public, max-age=…`（**注意**：这会把密钥放进缓存，
    与"用签名 URL 限时"冲突 —— 二者需业务权衡）；
  * **不要**在内核里加"密钥请求合并/全局缓存"（那是改核心语义，收益小风险大）；
* **IV 与 key 是分开的**：key 是拉来的，IV 是清单里带的或按序号推的
  ⇒ 并发拉 key **不会**导致 IV 串号。

### 5.3 离线播放

* **一期 AES-128**：离线播放 = 把密文分片 + `key.bin` + 清单一起缓存到本地。
  内核用 `Helper::combinePaths(baseUri, keyUrl)`（`HLSStream.cpp:791-792`）解析
  ⇒ **只要清单与 `key.bin` 的相对位置保持不变，离线与在线走的是同一条路径**，
  没有额外实现。
* **风险**：如果 `keyUrl` 指向一个**必须联网的许可证服务器**，
  离线就取不到密钥 ⇒ 一期**建议用静态 key 文件**正是为了避免这个坑。
* **二期 CENC/DRM**：离线需要"持久化许可证"（各平台 CDM 的 offline license），
  本工程内核**没有**这条通路（`framework/drm/` 里只有在线会话）⇒ 二期要单独设计。

### 5.4 seek 与加密边界

**【已证实】关键事实**：

1. **解密路径下 `seekSegment()` 直接 `return -EINVAL`**（`HLSStream.cpp:206-215`）
   ⇒ AES-128（整片解密）时，**不能在密文流里做字节级 seek**；
   seek 必须走"换分片 + 重建解密器"。
2. **解密器是流式的、会扣住最后一个块**（`AES_128Decrypter.cpp:56-83`），
   靠 `flush()` 复位（`AES_128Decrypter.h:23-30` 的 `flush()`）。
   `updateSegDecrypter()` 每次换分片都调 `flush()`（`HLSStream.cpp:870`）
   ⇒ 每个分片**独立解密、独立去 PKCS7**。
3. **因此一期必须遵守的铁律**：**每个分片必须独立可解密** ——
   即"每分片独立密钥（同一密钥也行）+ **该分片自己的 IV**"，
   **绝对不能**做成"整片一个大 CBC 流、跨分片连续"。
   跨分片连续 CBC 会：① 任何 seek 后的第一个分片解不出来；
   ② PKCS7 只在整片末尾 ⇒ 中间的 `mOutData -= padding`（`:82`）会**误删真实数据**。
4. **段内 seek**：依赖 IDR 密度（本工程 IDR 2s，`transcode_all.py:45`），
   与加密无关 —— 加密不改变 GOP 结构（`-c copy` 级操作）。

### 5.5 播放器兼容矩阵与"本工程硬约束"

#### 兼容矩阵（一期 AES-128）

| 播放端 | 路径 | 一期能否播 | 依据 |
|---|---|---|---|
| 内核 · HLS 文本 URL | `HlsParser` + `AES_128Decrypter` | ✅ **能，零改动** | `HlsParser.cpp:304-315`、`HLSStream.cpp:789-901` |
| 内核 · 对象化 JSON | `ManifestDemuxer` → `HLSManager` → 同上 | ✅ **能，零改动** | `ManifestDemuxer.cpp:142-163`、`:579-583` |
| 内核 · 单文件 SegmentBase | 一期不加密 | ⚠️ **不适用**（明文） | §4.2 |
| 内核 · URL 直连 `.mpd` | `MPDParser` 不读 `ContentProtection` | ❌ **不适用于加密内容** | `MPDParser.cpp:343-433`、`DashStream.cpp:394-406` |
| web · HLS 对象注入 | vendored hls.js fork | ⚠️ **需补 `_buildLevelDetails` 一处** | `hls.mjs:38934-38993`（缺口） |
| web · HLS 文本 | 标准 hls.js 解析路径 | ✅ 能（`parseKey` `:8317`/`:8055`、`Key.getDecryptData` `:7478`） | 同一个 `hls.mjs` |
| web · DASH | dash.js + `manifest-to-dash.ts` | ❌ 一期不支持（AES-128 在 DASH 无标准表达） | `manifest-to-dash.ts:328-330` |
| 桌面 Qt（`platform/QtPlayer`） | 同内核对象清单路径 | ✅ 能播加密内容（**软件解密，不需要 CDM**） | `ManifestDemuxer.cpp:579-583` + `ManifestBuilder` 已有 `encryption()`/`aes128Encryption()`（`CicadaManifestBuilder.h:163-189`） |
| 商用播放器 / Safari / ExoPlayer | 标准 HLS AES-128 | ✅ 能 | 协议标准 |

#### 本工程硬约束（**必须遵守**）

1. **不新增配置开关。**
   * 加密所需的是**参数**，不是开关：
     * **参数** = "密钥从哪来"（`--key-file` / `--key-env`）与"要不要保护"由**是否提供密钥**表达，
       ——**没有布尔开关**。给了密钥 = 加密；不给 = 明文且**必须明确告警**（不能悄悄跳过）。
     * 参数的性质：**必须有值才有意义**（一个路径、一个 URL、一个 hex 串），
       缺值是**错误**，而不是"回落到默认行为"。
     * **反例（禁止）**：`--enable-drm`、`--encryption on/off`、
       JSON 里加 `"encrypted": true` 这类**布尔开关**——
       它们会让"加密"变成一条"可被静默关闭的路径"，正是本工程反复禁止的模式
       （对齐 `docs/ANDROID-NDK-ASYNC-DECODER.md:114`、`docs/DASH-SEGMENTBASE.md:81`、
       `docs/SINGLE-DECODER-REFACTOR.md:171` 的口径："按**平台能力/必需参数**绑定，不是配置开关"）。
   * 内核侧同理：**不加任何 `setOption`/`options::SET` 开关**（§4.5.1 一期内核改动 = 几行日志）。
2. **不允许"精度换流畅"**：加密阶段**禁止**为了省时间而
   ① 降低码率/分辨率、② 跳过部分分片（"抽帧加密"）、③ 只加密部分档位却声明全加密。
   加密是**字节级原样变换**，不许触碰媒体内容（用 §4.6 的 A6 逐字节比对钉死）。
3. **不允许墙钟死线 / 看门狗 / 超时兜底**：
   * 加密阶段**禁止** `timeout=`、看门狗线程、"等了 N 秒没完成就放过"；
   * 子进程等待只用 `subprocess.run(...)` 的**自然退出**（现有 `run_segmentbase_remux()`
     `transcode_all.py:472-487` 就是这个范式：跑完看 `returncode`，**不设超时**）；
   * 现有的 `urllib.request.urlopen(req, timeout=5)`（`convert-to-manifest.py:619`）
     是**既有代码**，一期**不新增**同类超时；如要动，属于"删减"而不是"增加"。
   * 理由（本工程已固化的判断）：终态必须由事件产生，时间只允许用于观测
     （对齐 `docs/PLAN-SEEK-FAST-LANDING-CROSSPLATFORM.md:199`、`:242`、
     `docs/SEEK-PRECISION-TODO.md:9`）。
4. **不静默降级**：任何"以为加密了其实没加密"的路径都必须是**硬错误**（§6）。

---

## 6. 明确不能做的事

> 这一节是**红线清单**，欢迎在 Code Review 里按条打勾。

1. **不要偷偷降级成明文。**
   * 加密阶段失败（密钥读不到、工具报错、清单没写上 KEY）⇒ **必须非零退出**，
     **绝不能**"那就输出明文吧"。
   * 清单声明与产物必须**同真同假**：不能出现"清单写了 `#EXT-X-KEY` 但分片是明文"，
     也不能出现"分片是密文但清单没写 KEY"（后者等于交付一堆废文件）。
   * **`convert-to-manifest.py` 连不上 License Server 时"只打警告并继续"**
     （`convert-to-manifest.py:636-638`）是**既有行为**；
     一期要保证"**用了静态 key 文件时**不会走到这条分支"，
     并且如果将来启用 License Server，**这条警告必须在加密模式下升级为失败**。
2. **不要在失败时静默产出未加密产物。**
   * 不允许"清理/回滚时把明文留下、当成成功产物"（回滚见 §4.7：
     要么完整回滚到**全明文**，要么**全密文 + 全声明**，不允许半半）；
   * 不允许把临时名（如 `<final>.enc.tmp`）留在输出目录里充当交付物 ——
     现有脚本已有"临时名不许残留"的断言风格（`verify_segmentbase.py:331-337`），
     一期把它扩到加密产物上。
3. **不要引入运行时开关。**
   * 不加 `--enable-drm` / `--encryption on|off` / `"encrypted": true` 这类布尔开关（§5.5.1）；
   * 不加"环境变量开关"（`DRM_ENABLED=1`）；
   * 不在内核里加 `setOption("enableDrm", …)`；
   * 加密与否只由**参数是否存在**决定，且**缺参数是错误、不是默认值**。
4. **（补充红线，同样不许做）**
   * **不许**在日志/stdout/清单里打印密钥内容（只允许 KID 或密钥指纹）；
   * **不许**新增 DRM 专属错误码枚举值（一期复用既有错误类，§4.5.3）；
   * **不许**为了加密而改动主转码命令与 SegmentBase 再封装命令的参数
     （沿用 `transcode_all.py:431-440` 的既有约定）；
   * **不许**在没有同时改清单的情况下改产物（或反之）—— 两者必须**同一次提交**内一致。

---

## 7. 附录：证据索引

### 7.1 内核（`D:\hilihili\CicadaPlayerNext`）

| 主题 | 位置 |
|---|---|
| `SegmentEncryption` 定义（方法枚举/字段） | `framework/demuxer/play_list/segment_decrypt/SegmentEncryption.h:13-33` |
| `SegmentEncryption` 默认 NONE | `.../SegmentEncryption.cpp:7-9` |
| `segment::encryptions` / `setEncryption` | `framework/demuxer/play_list/segment.h:38,61`；`segment.cpp:45-48` |
| `EXTXKEY` tag 枚举 / 工厂 | `framework/demuxer/play_list/HlsTags.h:78-91`；`HlsTags.cpp:290,336` |
| `Attribute::hexSequence/quotedString` | `framework/demuxer/play_list/HlsTags.h:15-35` |
| `#EXT-X-KEY` 解析 | `framework/demuxer/play_list/HlsParser.cpp:294-355` |
| 加密数组挂到分片 | `framework/demuxer/play_list/HlsParser.cpp:267-270`；`:296-299` |
| 选一条加密记录（keyFormat 判定） | `framework/demuxer/play_list/HLSStream.cpp:545-550` |
| 拉密钥（16 字节 / URL 缓存） | `framework/demuxer/play_list/HLSStream.cpp:789-833` |
| IV 处理（静态/序号） | `framework/demuxer/play_list/HLSStream.cpp:835-853` |
| 解密路径选择 | `framework/demuxer/play_list/HLSStream.cpp:1080-1102` |
| 整片解密器装配 | `framework/demuxer/play_list/HLSStream.cpp:855-902` |
| 样本级解密器装配 | `framework/demuxer/play_list/HLSStream.cpp:905-929` |
| 解密器交给 demuxer / 形态提示 | `framework/demuxer/play_list/HLSStream.cpp:573`；`:597` |
| 解密路径禁止字节 seek | `framework/demuxer/play_list/HLSStream.cpp:206-215` |
| `Stream_meta` 出口（keyUrl/keyFormat/pssh/keyId） | `framework/demuxer/play_list/HLSStream.cpp:1517-1520` |
| AES-128-CBC 实现 | `framework/demuxer/decrypto/avAESDecrypt.cpp:23-31` |
| 按块解密 + 去 PKCS7 | `framework/demuxer/play_list/segment_decrypt/AES_128Decrypter.cpp:27-101`；`.../AES_128Decrypter.h:23-30` |
| 样本级只支持 H264/AAC + 密钥无效静默 | `framework/demuxer/sample_decrypt/HLSSampleAesDecrypter.cpp:54-77`；`:56-59` |
| clear-lead 常量 | `framework/demuxer/sample_decrypt/HLSSampleAesDecrypter.h:34-35`；`.cpp:178-179,229,266-268` |
| `DrmUtils::isSupport`（仅 Android Widevine） | `framework/utils/DrmUtils.cpp:7-12`；调用 `HLSStream.cpp:546` |
| DRM handler 只在 Android/OHOS 编 | `framework/drm/CMakeLists.txt:11-24`；`WideVineDrmHandler.cpp:110-113`；`OHOS/OhosDrmHandler.cpp:15` |
| DRM prototype 队列 | `framework/drm/DrmHandlerPrototype.cpp:14-36` |
| `DrmInfo` 组装 / 回调 | `mediaPlayer/SMPAVDeviceManager.cpp:47-59,96-97` |
| `STATUS_DRM_ERROR` | `framework/codec/IDecoder.h:46`；`framework/codec/ActiveDecoder.cpp:267` |
| 错误类/码枚举 | `framework/utils/errors/framework_error.h:19,50-53` |
| MPD **不**解析 ContentProtection | `framework/demuxer/dash/MPDParser.cpp:343-433`；`dash/DashStream.cpp:394-406` |
| 对象清单模型（ContentProtection/LicenseServer/Aes128Encryption/rep/顶层） | `framework/demuxer/manifest/MediaManifest.h:33-60,151-152,246-250,260-263` |
| 对象清单 parser | `framework/demuxer/manifest/MediaManifestParser.cpp:39-63,194-200,372-384` |
| `buildSegmentEncryptions`（AES-128 / CENC → SegmentEncryption） | `framework/demuxer/manifest/ManifestDemuxer.cpp:138-187`；挂载 `:432-450,522-546` |
| 对象片源一律走 HLS 管线 | `framework/demuxer/manifest/ManifestDemuxer.cpp:579-583` |
| Qt 清单 builder 的 encryption/ContentProtection | `platform/QtPlayer/src/CicadaManifestBuilder.h:163-202,315` |
| 硬约束口径（不新增开关/无看门狗） | `docs/ANDROID-NDK-ASYNC-DECODER.md:114,383-384`；`docs/DASH-SEGMENTBASE.md:81`；`docs/SINGLE-DECODER-REFACTOR.md:171`；`docs/PLAN-SEEK-FAST-LANDING-CROSSPLATFORM.md:199,242` |

### 7.2 web（`D:\hilihili\front\hili-player`）

| 主题 | 位置 |
|---|---|
| `ContentProtection` / `LicenseServer` / `Aes128Encryption` 类型 | `packages/plugins/src/vendor/types/manifest.ts:64-105,115-138,150-180`；rep 级 `:1009`、顶层 `:622` |
| `applyEncryption()` | `packages/plugins/src/vendor/manifest-to-hls.ts:90-108`；调用 `:226,278,398-415` |
| `ManifestPlaylistDetails.encryption` / `ManifestEncryption` | `packages/plugins/src/hls/vendor/hls.d.ts:3483-3511` |
| **缺口**：`_buildLevelDetails` 不消费 encryption | `packages/plugins/src/hls/vendor/hls.mjs:38934-38993`；`loadManifest` 调用 `:38860,38901` |
| `Key` 类 / `getDecryptData` / `isSupported` | `packages/plugins/src/hls/vendor/hls.mjs:7431-7484` |
| `parseKey`（文本路径） | `packages/plugins/src/hls/vendor/hls.mjs:8317`；调用 `:8055`（EXT-X-KEY）、`:7686`（SESSION-KEY） |
| 密钥响应按裸字节处理 | `packages/plugins/src/hls/vendor/hls.mjs:36838` |
| 软件解密（AES-128-CBC + PKCS7） | `packages/plugins/src/hls/vendor/hls.mjs:5985-6020` |
| DASH ClearKey ContentProtection 生成 | `packages/plugins/src/vendor/manifest-to-dash.ts:414-433`；挂载 `:449-497` |
| DASH 侧忽略 rep.encryption | `packages/plugins/src/vendor/manifest-to-dash.ts:328-330` |
| 校验器（encryption 必填字段） | `packages/plugins/src/vendor/utils/validate.ts:57-59,130-132,243-253` |
| 插件入口 | `packages/plugins/src/hls/HlsPlugin.ts:381,498`；`packages/plugins/src/dash/DashPlugin.ts:327,369` |

### 7.3 转码脚本（`D:\hilihili\转码脚本`）

> ⚠ **行号分两套**：下表中标了 **`[施工前]`** 的是设计初稿写下的行号（一期施工后已经偏移，
> 只能当"哪个函数"的线索用）；标了 **`[施工后]`** 的是**一期施工完成后的真实行号**，
> 按它去读代码即可。施工后的完整改动点清单见 §4.8.1。

| 主题 | 位置 |
|---|---|
| 主转码命令（DASH+HLS 一次产出） | `transcode_all.py [施工后] :526-673`；DASH/HLS 输出段 `:655-672` · `[施工前] :254-401`、`:384-400` |
| 主转码执行 + 失败语义 | `transcode_all.py [施工后] :675-712` · `[施工前] :403-429` |
| SegmentBase 再封装（-c copy，命令不可动） | `transcode_all.py [施工后] :714-786` · `[施工前] :431-493`、`:442-462` |
| SegmentBase 产物准备/改写 | `transcode_all.py [施工后] :899,1339,1503-1555,1558,1608` · `[施工前] :997,1067,1231-1283,1286,1336` |
| 重命名与清单收尾（**不再生成 JSON**） | `transcode_all.py [施工后] :2244-2401` · `[施工前] :1813-1953`；补 codecs `:1738-1759`；补 endNumber `:2305-2333` |
| HLS 属性读写工具（可复用） | `transcode_all.py [施工后] :1718-1736` · `[施工前] :1446-1463` |
| 内置 JSON（**【施工后】已带 encryption**） | `transcode_all.py [施工后] :1827-1956`（`_with_encryption` `:1906`、`_json_encryption` `:1891`）；写出 `:2075-2119` |
| 优先调用 convert-to-manifest.py | `transcode_all.py [施工后] :2121-2242` |
| 段内 IDR 校验 | `transcode_all.py [施工后] :2716-2804` · `[施工前] :1955-2043` |
| `main()` 流程 | `transcode_all.py [施工后] :2840-2949` · `[施工前] :2045-2101` |
| ~~**转码侧零加密**~~（**【施工后】已作废**） | 【施工后】加密阶段 `transcode_all.py:2404-2710`（`run_encryption` `:2582`）；参数解析 `:2807-2837` |
| **【施工后】加密原语与密钥装载** | `transcode_all.py:287-502`（`_aes128_encrypt_block` `:314`、`_iv_for_index` `:324`、`aes128_cbc_encrypt` `:335`、`load_hls_key_info` `:429`） |
| **【施工后】SegmentBase 不加密的双向断言** | `transcode_all.py:1920-1955`（`_assert_segmentbase_plain` / `_assert_segmentbase_not_encrypted_on_disk`） |
| `#EXT-X-KEY` → encryption | `convert-to-manifest.py:143-162`；写出 `:738-770` |
| License Server 客户端（假定形状） | `convert-to-manifest.py:602-638`；开关变量 `:35-37`；触发 `:990-995`；CLI `:954-964` |
| MPD `xmlns:cenc` 补全（已有） | `convert-to-manifest.py:183-191` |
| SegmentBase 清单**不含** encryption | `convert-to-manifest.py:561-567`；HLS 版 `:570-595` |
| 验收风格与工具 | `verify_segmentbase.py [施工后] :3-72,91-304,633-1145,1147-1400` · `[施工前] :3-68,71-284,610-663` |
| **【施工后】一期 DRM 验收断言** | `verify_segmentbase.py:741-1030`（`check_drm_encryption`）、`:1040-1091`（`check_drm_not_encrypted`）、`:1092-1145`（`check_no_enc_tmp` / `check_drm_declaration_consistent`） |
| 临时名不许残留 | `verify_segmentbase.py [施工后] :348-354`（原有）+ `:1092-1099`（加密的 `*.enc.tmp`）；`_sb_name_selfcheck.py` |

### 7.4 本文件里标注为【推测/待验证】的项（施工前必须实测）

> **【施工后状态】** 本节的 1~5 在设计初稿里都是"施工前必须实测"；一期施工后：
> **1 已确认（不可用）**、2~3 一期不用（二期 CENC 才需要）、4 一期绕过（web 只走 HLS）、
> **5 与一期无关**。**一期真正剩下的待验证项已挪到 §4.8.7**（10 条，含一条命令的验法）。

1. ~~`-hls_key_info_file` 能否用于本工程的 `-f dash` 输出~~ ——
   **【施工后已确认：不可用】**。主转码是 `-f dash` + `-hls_playlist 1`
   （`transcode_all.py` 的 `build_ffmpeg_cmd()`），dashenc 的内嵌 HLS 播放列表不吃
   hls muxer 的私有选项；而且即便另起一趟 `-f hls`，它也只能给整条播放列表**一个 IV**，
   与"每分片 IV 唯一"冲突。故一期改成**脚本自己做字节级加密**（§3.1 第 3 条 / §4.8.3）；
2. ffmpeg 8.x 的 `-encryption_scheme cenc-aes-ctr -encryption_key -encryption_kid`
   具体可用性与**能否与 `-single_file 1` 并存**（§3.2）—— 二期再测；
3. shaka-packager / Bento4（`mp4fragment`/`mp4encrypt`）的**版本与参数名** —— 二期再测；
4. AES-128 在 **DASH/MPD** 里的标准表达（web dash.js 是否认）—— 一期保守只走 HLS
   （`output.mpd` 里那条 `ContentProtection` 只作为"语义完备性"补充，内核不读它）；
5. 内核 `Ffmpeg` 版本对 `AV_PKT_DATA_ENCRYPTION_INFO` 的行为（与一期无关，CENC 二期才会遇到）。

---

**文档结束。**

---

## 附：一期施工涉及的文件清单（一句话索引）

| 文件 | 状态 |
|---|---|
| `D:\hilihili\转码脚本\aes128.py` | **新增**：纯 Python AES-128（FIPS-197）原语 + 两条官方向量自检；**加密端与验收端共用这一份** |
| `D:\hilihili\转码脚本\transcode_all.py` | **已改**：`--hls-key-info` 参数、AES-128-CBC 加密（原语来自 `aes128.py`）、`run_encryption()`、`#EXT-X-KEY`/`#EXT-X-SESSION-KEY`/`ContentProtection`、内置 JSON 的 `encryption`、SegmentBase 明文断言（改动点清单见 §4.8.1） |
| `D:\hilihili\转码脚本\verify_segmentbase.py` | **已改**：一期 DRM 验收（A1~A13）+ `--key-file` / `--clear-dir` / `--expect-encrypted` |
| `D:\hilihili\转码脚本\convert-to-manifest.py` | **未改**（本来就认识 `#EXT-X-KEY`，并且本来就写全 6 个 JSON —— 本次施工时重新核对了 `:1042-1066`，它确实写 `test-dash-*` 与 `test-hls-*-segmentbase.json` 两版） |
| `D:\hilihili\front\hili-player\packages\plugins\src\hls\vendor\hls.mjs` | **已改**：`_buildLevelDetails()` 消费 `playlist.encryption`（Object 注入路径补上 AES-128） |
| `D:\hilihili\front\hili-player\packages\plugins\src\hls\vendor\hls.d.ts` | **已改**：`ManifestEncryption` 文档 |
| `D:\hilihili\CicadaPlayerNext\**`（C++） | **未改**（一期内核零改动；§4.5.3 的可观测性改动留给单独施工，理由见 §4.8.5） |
| `D:\hilihili\CicadaPlayerNext\docs\DRM-ENCRYPTION-DESIGN.md` | **已改**：回写实现差异 + §4.8 施工记录 |
（本文件是唯一新增内容；`CicadaPlayerNext` 下未修改任何其他文件。）
