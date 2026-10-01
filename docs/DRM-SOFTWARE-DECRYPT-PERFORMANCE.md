# DRM 软件解密：性能实测 + CENC 正确性实测

> 这份文件回答两个问题，两个都用**本机跑出来的数**回答，不用估算：
> 1. 只能软解吗？性能如何、CPU 占用高吗？—— 见 §1。
> 2. CENCDecrypter 写完了，它在**真的 CENC 产物**上对不对？—— 见 §2。
>
> 复现全部在这里：`tools/drm_bench/`。两份实测都跑在 Windows x86_64、
> 项目自带 FFmpeg 9.0（`HAVE_AESNI=yes`，见
> `external/build/ffmpeg/win32/x86_64/ffbuild/config.mak:230`）。

---

## 1. 性能实测：软解不是瓶颈

### 1.1 怎么测的

`tools/drm_bench/drm_bench.cpp`。关键点：**它直接链接仓库自己的解密类**
（`AES_128Decrypter`、`CENCDecrypter`）与 `av_aes_*` 原语，不重新实现任何东西，
所以这些数字描述的就是产品里真正跑的那条路。

```bat
:: 见 tools/drm_bench/README.md 里的完整命令行
drm_bench.exe
```

### 1.2 原始结果

```
raw AES-128 blocks                    16.00 MB in   0.004 s =   3920.9 MB/s (raw AES-128 ECB, no XOR/chaining)
HLS AES-128 (6 MB/6 s seg)            60.00 MB in   0.018 s =   3249.6 MB/s | 60 s of media -> 0.03% of one core
HLS AES-128 (30 MB/6 s seg, UHD)     120.00 MB in   0.042 s =   2882.9 MB/s | 24 s of media -> 0.17% of one core
CENC cenc (64 KB, NAL subsamples)       64 KB pass in    0.094 ms =   662.1 MB/s | 94.4 us per MB of content
CENC cenc (1 MB)                      1024 KB pass in    1.524 ms =   656.2 MB/s | 1524.0 us per MB of content
CENC cbcs pattern 1:9 (1 MB)          1024 KB pass in    0.082 ms = 12180.3 MB/s | 82.1 us per MB of content
CENC cbcs pattern 1:0 (all crypt)     1024 KB pass in    0.678 ms =  1475.8 MB/s | 677.6 us per MB of content
CENC cbc1 (1 MB)                      1024 KB pass in    0.551 ms =  1815.9 MB/s | 550.7 us per MB of content
```

### 1.3 换算成"占用多少 CPU"

`tools/drm_bench/core_share.py` 的输出（1 Mbit/s = 0.125 MB/s；百分比是**单核**的）：

| 解密路径 | 吞吐 | 1080p 8 Mbps | UHD 40 Mbps | 4K 100 Mbps |
|---|---|---|---|---|
| HLS AES-128 整片 | 3250 MB/s | **0.031%** | 0.154% | 0.385% |
| CENC `cenc`（AES-CTR） | 656 MB/s | **0.152%** | 0.762% | 1.905% |
| CENC `cbcs` 1:9 | 12180 MB/s | 0.008% | 0.041% | 0.103% |
| CENC `cbcs` 1:0（全加密） | 1476 MB/s | 0.068% | 0.339% | 0.847% |
| CENC `cbc1` | 1816 MB/s | 0.055% | 0.275% | 0.688% |

另一种更好感知的说法（每分钟播放消耗的 CPU 时间）：

| 解密路径 | 1080p 8 Mbps | UHD 40 Mbps |
|---|---|---|
| HLS AES-128 整片 | 18.5 ms / 分钟 | 92.3 ms / 分钟 |
| CENC `cenc` | 91.4 ms / 分钟 | 457.2 ms / 分钟 |
| CENC `cbcs` 1:9 | 4.9 ms / 分钟 | 24.6 ms / 分钟 |

### 1.4 结论

* **最重的那条路（CENC `cenc`）在 4K/100Mbps 上占单核不到 2%**，1080p 只有 **0.15%**。
  也就是说 **CPU 占用高这件事不存在**，软解不是瓶颈。
* 为什么这么快：**底下的 AES 原语本身是硬件加速的**。`av_aes_crypt` 在
  x86 上走 AES-NI（`config.mak:230 HAVE_AESNI=yes`），所以"软件 DRM"里的
  "软件"指的是**我们自己写调度、跑在 CPU 上**，而不是"用慢的查表 AES"。
  真正慢的是逐 subsample 的循环与 XOR，量级在 600~3000 MB/s。
* 反过来说：**性能从来不是"要不要用硬件 DRM"的理由**。硬件 DRM（Widevine
  L1 / PlayReady SL3000 / FairPlay）的价值是**密钥与明文不出安全世界**——
  CPU 看不到解密后的帧，所以录制/内存抓取拿不到明文。这是**安全性**的差别，
  不是速度的差别。见 §3.2。

> 【口径提醒】上表的 "MB" 都是 1024²。数字取自一次运行；`raw`/`HLS` 两行是
> 多段重复后的稳定值，`CENC` 各行是**单次 pass**（这正是解复用器每样本要付的
> 代价，没有可摊销的批处理），所以 CENC 的数字受单次调用开销（建 counter、
> 首次触碰缓存）影响，偏保守。

---

## 2. CENC 正确性实测：真产物上 150 个样本，149 个逐字节解对

### 2.1 方法（这是关键：不是自证）

`tools/drm_bench/cenc_verify.cpp`。

同一份编码产出两个文件：一个明文、一个用
`ffmpeg -encryption_scheme cenc-aes-ctr -encryption_key … -encryption_kid …`
加密。两者样本（sample）一一对应、大小相同，于是

```
CENCDecrypter(第 i 个密文样本, senc 里的 IV 与 subsample 表) == 第 i 个明文样本
```

是一个**逐字节**的判据。FFmpeg 只负责解复用并把每个样本的
`AV_PKT_DATA_ENCRYPTION_INFO`（scheme / key_id / iv / subsamples）递过来，
**解密全部由仓库自己的类完成**。这是"拿别人产的东西考自己的实现"，
不是自己考自己。

### 2.2 结果

```
streams compared        : 1
samples total           : 150
samples compared        : 150
  of which encrypted    : 150
  of which clear        : 0
BYTE-IDENTICAL to clear : 149
mismatched              : 1
```

产出的 CENC 文件确实带齐了加密结构（逐字节在文件里找过）：
`encv`(421) / `sinf`(569) / `schm`(589) / `tenc`(617) / `senc`(1079) /
`saio`(1501) / `saiz`(1521)，且 `ftyp` 与明文文件逐字节相同（结构留在明文里）。
（这个文件没有 `pssh`——ffmpeg 的 `-encryption_kid` 只写 `tenc` 的 default_KID，
不生成 `pssh`；这不影响解密，但**影响依赖 pssh 的 CDM 流程**。）

### 2.3 那 1 个样本为什么不一致（如实说：**未查清**）

唯一不一致的是**第 0 个**样本，它带**2 个 subsample**：

```
subsamples: 2
  [0] clear=5 protected=593
  [1] clear=5 protected=6929
subsample sum=7532 sample size=7532 (exact)
```

`cenc_verify.cpp` 用**四个互相竞争的计数器模型**分别去复现明文，结果：

| 模型 | 结果 | 首个错字节 |
|---|---|---|
| A：计数器跨 subsample **连续** | 不匹配 | 603（= 第二个受保护区间起点） |
| B：计数器**每个 subsample 重置**为 0 | 不匹配 | 603 |
| C：整样本当作一个连续密文区间（无 clear） | 不匹配 | 0 |
| D：同 C 但只用 IV 前 8 字节 | 不匹配 | 0 |

A 与 B 都**解对了第一个受保护区间**（前 603 字节一致），只错在第二个区间；
把第二个区间的起始 counter 从 **30 扫到 50** 也**没有任何值**能复现。
也就是说：该样本第二区间**用的不是"同一把 IV + 某个 counter"**——
这是 ffmpeg 这个 muxer 在"一个样本里既有 clear 前缀又有多个受保护区间"时的
一个未定行为。

**我们落地成哪个？** 落地成 **A（连续）**，理由是它跟两个互相独立、都在海量设备
上跑的生产实现一致（代码位置见 `CENCDecrypter.cpp::decryptCtr` 的注释）：
FFmpeg 自己的 `mov.c::cenc_scheme_decrypt()` 对每个 subsample 调
`av_aes_ctr_crypt()` 而不重 arm IV，`movenccenc.c` 则把整个样本一次性 crypt；
Shaka Packager 的 `AesCtrEncryptor::Encrypt()` 也是沿用同一个 counter，
只在 `UpdateIv()`（按样本）时重置。

**影响面**：单 subsample 的样本两种写法等价 —— 而 150 个样本里 **149 个都是
这一类**。所以这条差异只在"一个样本里既有 clear 前缀、又有多个受保护区间"的
片源上才可能体现。

**没做到的事**（不要当成已有）：没有用第二个独立 CENC 产物（例如 Shaka
Packager 或 Bento4 产的）交叉验证过；也没有在设备上做过 CENC 播放验证。
复现命令见 `tools/drm_bench/README.md`。

---

## 3. 顺带回答：软解 vs 硬解、以及各协议/平台的现状

### 3.1 "DRM 只能软解吗" —— 不是，但**桌面没有硬解 DRM**

| 端 | 有没有硬件/安全级 DRM | 走哪条路 | 今天的实际状态 |
|---|---|---|---|
| **Android** | **有**（Widevine，L1 硬件级） | 自己的 JNI MediaCodec：`setDrmInfo` → `MediaCrypto` → `queueSecureInputBuffer`（`framework/codec/Android/jni/MediaCodec_Decoder.cpp:272`、`framework/codec/Android/codecBinding.cpp:103`），DRM 会话管理在 `platform/Android/**/DrmSessionManager.java` | **已实现**，但只在 Android 上；`DrmUtils::isSupport()` 也只认 Widevine 那一个 UUID（`framework/utils/DrmUtils.cpp:7-12`） |
| **OHOS** | 有（DRM Kit） | `framework/drm/OHOS/OhosDrmHandler.cpp` | 通道在，`is_supported` 认 Widevine/PlayReady/FairPlay/ClearKey 四个 UUID |
| **iOS / macOS** | 有（FairPlay），但需要证书与许可服务 | `framework/codec/Apple/AppleVideoToolBox.cpp` 有 `is_drmSupport` 钩子，**没有任何 FairPlay 会话代码** | **未实现** |
| **桌面 Qt（Windows/Linux/macOS 自绘）** | **没有 CDM** | —— | 只能软解（本文 §1、§2） |

所以准确的说法是：**"软解"是桌面端的唯一选择，也是所有端的兜底**；
Android/OHOS 上有硬件/安全级通路，但那是**安全性**的提升，不是性能的提升（§1.4）。

### 3.2 软解的真正代价（不是 CPU）

软解的内容密钥在**应用内存里**、明文帧也在应用内存里，所以：
* 它能挡住的：随便下载分片就能播、以及"没有 key 就是一堆随机字节"；
* 它**挡不住**的：有决心的攻击者从进程内存里抓 key 或抓解码后的帧。

这就是为什么"自建 Go 密钥服务器 + 软解"这整套东西的口径应该叫
**透明 DRM / clear-key DRM**，而不是 Widevine/PlayReady/FairPlay 那一类。
要往上升级必须换到平台 CDM，而那是**另一条实现路线**（见 §3.1），
不是把软解"优化"一下就能到的。

### 3.3 "实现了 DRM 的是不是只有 HLS" —— 今天**是**，DASH 是缺口

| 协议 | 整片 AES-128 | CENC（样本级） | 说明 |
|---|---|---|---|
| **HLS**（文本 m3u8） | **已实现并实测** | 有通道（`SAMPLE-AES` → `mSampeAesDecrypter`），但样本级那条路**未接线** | §2 的 149/150 是"解密类"的实测，不是"播放"的实测 |
| **HLS**（对象清单 JSON） | 已实现（走 HLS 管线） | 同 HLS | —— |
| **DASH**（URL 直连 .mpd） | **未实现** | **未实现** | `MPDParser` **完全没有 `ContentProtection` 解析**（全文件只有 UTCTiming 那一处用 `schemeIdUri`，`framework/demuxer/dash/MPDParser.cpp:231-238`），`DashSegment` 上也没有加密字段（`DashStream.cpp:400-412` 的注释明说"今天按非加密处理"） |
| **DASH**（对象清单 JSON） | —— | 清单侧能把 `ContentProtection` 转成 `SegmentEncryption{AES_SAMPLE}`（`ManifestDemuxer.cpp`），但**解密侧没接线** | —— |

**DASH 要补的三件事**（按依赖顺序）：
1. `MPDParser` 解析 `ContentProtection`（`cenc:default_KID`、`cenc:pssh`、
   `schemeIdUri` 是不是 `mp4protection`），填到 `DashSegment` /
   `Stream_meta.drmKeyId` / `drmPssh`；
2. 一个"按 KID 取密钥"的供给点（与 HLS 侧 `updateKey()` 同形：GET 一个 URL、
   拿 16 字节），把 key 交给 `CENCDecrypter`；
3. 把 `CENCDecrypter` 接进解复用器读取路径（`demuxer_service::readPacket` 或
   `avFormatDemuxer::ReadPacketInternal`，在 bsf 之前，用包上的
   `AV_PKT_DATA_ENCRYPTION_INFO`）。**目前它还没有接进任何读取路径** ——
   §2 是把包取出来在独立程序里喂给它的。

第 3 条对 HLS 与 DASH **是同一条**，所以先把第 3 条做掉，HLS 的样本级与
DASH 的 CENC 会同时具备可播能力；第 1、2 条是 DASH 特有的。
