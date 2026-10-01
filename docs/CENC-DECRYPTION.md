# CENC（ISO/IEC 23001-7）软件解密实现说明

本文说明 `framework/demuxer/sample_decrypt/CENCDecrypter.{h,cpp}` 里究竟实现了什么、
依据是什么、哪些地方是判断取舍，以及测试证明了什么、没证明什么。

目标是：**没有平台 CDM 的情况下也能解开 CENC 加密的样本**。实现只用内核已有的 AES 原语
（`framework/demuxer/decrypto/avAESDecrypt.{h,cpp}`，封装 FFmpeg `libavutil/aes.c`），
不含任何平台宏、任何 `#ifdef`，可同时编译在 Windows / Linux / macOS / Android / iOS / OHOS。

---

## 1. 支持的 scheme 与不支持的 scheme

| scheme | 分段模式 | pattern | 本实现 | 说明 |
| --- | --- | --- | --- | --- |
| `cenc` | AES-CTR | 无 | ✅ 支持 | 计数器模式，见 §3 |
| `cens` | AES-CTR | 名义上有 | ✅ 按 CTR 处理 | 见下方「判断取舍 1」 |
| `cbc1` | AES-CBC | 无 | ✅ 支持（含尾部 CTS） | 见 §4 |
| `cbcs` | AES-CBC | 有（crypt:skip） | ✅ 支持（含尾部 CTS） | 见 §5 |

明确**不支持**的：`cbc1`/`cbcs` 之外再出现新 scheme（返回 `-EINVAL`，不会静默透传）。
本实现不含密钥获取（KID → key 的下发/请求），调用方要自己把 key 通过 `setKey()` 注册进来。

---

## 2. 与 `IAFPacket::EncryptionInfo` 的对接

`framework/base/media/AVAFPacket.cpp::getEncryptionInfo()` 已经从 FFmpeg 的
`AV_PKT_DATA_ENCRYPTION_INFO` side data 里取出了：`scheme`、`key_id`、`iv`、
`crypt_byte_block`、`skip_byte_block`、`subsamples`。这一步**本实现不重复做**。

集成时只需要把字段搬过来：

```cpp
std::vector<SubsampleInfo> subs;
for (const auto &s : info.subsamples) {
    subs.push_back({s.bytes_of_clear_data, s.bytes_of_protected_data});
}
int64_t ret = mCenc->decrypt(kidHex, info.scheme, info.iv, info.iv_size, subs,
                             info.crypt_byte_block, info.skip_byte_block,
                             pkt->getData(), pkt->getSize());
```

`SubsampleInfo` 故意定义在 `Cicada` 命名空间作用域（不是嵌套类），所以调用方构造
`std::vector<SubsampleInfo>` 时不需要包含任何包/解复用头文件。

> ⚠️ 注意 `EncryptionInfo::iv` / `key_id` 指向的是 FFmpeg side data 的**借用内存**，
> 生命周期跟着 `AVPacket`；本实现只在 `decrypt()` 调用期间读它。

---

## 3. `cenc` / `cens`：AES-CTR 的 counter block 构造（精确描述）

对每个 protected 区间（subsample）：

1. 取 16 字节 counter block：**前 `ivSize` 字节 = per-sample IV，其余字节置 0**。
   - `ivSize` 通常是 8（`cenc`），也可以是 16；本实现接受 1..16，越界返回 `-EINVAL`。
2. **后 8 字节当作大端 64bit 计数器，初值 0，每处理一个 16 字节分组 +1**（进位只发生在
   这 8 字节内）。
3. `keystream = AES-ECB-Encrypt(counter block)`，`plaintext = ciphertext XOR keystream`。
4. 计数器**在每个 subsample 起点重新从 0 开始**；IV 本身在一个样本内不变
   （`senc` 一个样本只给一个 IV）。
5. 最后一个不满 16 字节的分组只 XOR 前 `n` 字节。

依据：
- ISO/IEC 23001-7 中 `cenc` 使用 AES-CTR、counter 高低位划分的规定（约 §9.2 及
  「AES-CTR」相关小节）；
- NIST SP 800-38A §F.5（CTR-AES128 测试向量），本仓库测试直接使用 F.5.1/F.5.2 的向量；
- FFmpeg `libavutil/aes_ctr.c` 的实现与该构造一致：`av_aes_ctr_set_full_iv()` 把 16 字节
  IV 整块写入 counter，`av_aes_ctr_crypt()` 里 `av_aes_ctr_increment_be64(a->counter + 8)`
  只自增低 8 字节。见 `external/external/ffmpeg/libavutil/aes_ctr.c:89-135`。

---

## 4. `cbc1`：AES-CBC（无 pattern）+ 尾部 CBC-CS3（ciphertext stealing）

- IV = per-sample IV 铺到前 `ivSize` 字节、其余补 0（通常是 16 字节 IV）。
- **IV 每个 subsample 重置**；subsample 内按标准 CBC 链解密：
  `P_i = AES-ECB-Decrypt(C_i) XOR C_{i-1}`，`C_0` 的前一段是 IV。
- 尾部不满 16 字节时用 **CTS（ciphertext stealing）**，取 NIST SP 800-38A Addendum 的
  **CBC-CS3**：末两段密文块交换次序。

即对最后完整块 `C_{n-1}` 和截断块 `C_n`（长度 `r ∈ [1,16)`）：

```text
X     = AES-Dec(C_n) XOR C_{n-1}          // 前 r 字节是 P_n，后 16-r 字节是 C_{n-1} 残余
P_n   = X[0..r)                            // 只保留 r 字节
P_{n-1} = AES-Dec(C_{n-1}) XOR X
```

`cbc1` 没有 pattern（`crypt_byte_block`/`skip_byte_block` 恒为 0，与
`AVAFPacket::getEncryptionInfo()` 给出的元数据一致，FFmpeg `mov.c` 对 `cbc1` 也是整段 CBC）。

**判断取舍**：ISO/IEC 23001-7 的原文无法在本环境内取得，我在**文档里无法引用 100% 确认的
小节号**。选择 CBC-CS3 的理由是：CENC 的 `cbcs`/`cbc1` 的尾部处理在生态实现里按
「CBC-CS3」被引用，而 `cbc1`/`cbcs` 的 CTS 是同一个机制；CS3 与 CS1 的区别只在末两段密文
是否交换，若某个实际内容用的是 CS1（CBC-CS1 交换、CBC-CS2 不交换），表现会是**最后两段的
明文块错位**。这一点已写进 §8「未验证事项」。

---

## 5. `cbcs`：AES-CBC + pattern + 尾部 CTS

- IV 16 字节（同样按 `ivSize` 铺前段、补 0）；**每个 protected 区间（subsample）起点重置 IV**。
- pattern：**先加密 `cryptByteBlock` 个 16 字节块，再跳过 `skipByteBlock` 个 16 字节块**，
  如此重复；每个 protected 区间的起点一定从「加密」段开始（也就是第一段
  `16*cryptByteBlock` 字节一定被加密）。
- skip 段**不打断 CBC 链**：整段 protected 数据（含被跳过的明文段前后）用一个连续的
  CBC 链，链值在每个 run 的「最后一段密文」上推进。
- 每段被加密的 run 内部，若长度 ≥ 32 字节且不是 16 的整数倍，尾部走 §4 的 CBC-CS3 CTS。
- 约束校验（不满足直接返回 `-EINVAL`）：`1 <= cryptByteBlock <= 9`、`0 <= skipByteBlock <= 9`，
  且 `skipByteBlock != 0` 时 `cryptByteBlock + skipByteBlock <= 10`。

依据：
- ISO/IEC 23001-7 中 `cbcs` 的 CBC pattern encryption 规定（约 §9.6，及
  「Pattern encryption」/`crypt_byte_block`/`skip_byte_block` 小节；pattern 语义在
  `senc`/`saiz`/`saio` 与 `schm`/`tenc` box 中由 `crypt/skip` 表达）；
- 约束上限 9/9、和不超过 10 与 `tenc` box 的 pattern 字段定义一致；
- FFmpeg `libavformat/mov.c` 的 `cbcs_scheme_decrypt()` 同样是「每个 subsample 重置 IV +
  crypt 段 / skip 段交替」，可交叉对照（`external/external/ffmpeg/libavformat/mov.c:8560-8620`）。

**判断取舍（与 FFmpeg 的一处差异）**：FFmpeg `mov.c` 里
`av_aes_crypt(sc->cenc.aes_ctx, data, data, sample->crypt_byte_block, iv, 1)` 依赖
`av_aes_crypt` 的 CBC 链，而该函数（`libavutil/aes.c:155-169`）实现的 iv 是「每个分组就地
回写」而不是逐块推进的链，跨 run 也不会续链；同时它遇到 `rem_bytes < 16*crypt_byte_block`
就直接 `break`，没有 CTS。本实现**按标准做真正的 CBC 链 + CTS**，不跟随 FFmpeg 的这个行为。
如果后续实测发现某类内容需要与 FFmpeg 行为逐字节对齐，这是第一个要复查的地方。

---

## 6. 校验与错误语义（“绝不假装成功”）

`decrypt()` 的所有失败路径**都不写 buffer**，返回值一律为负：

| 情况 | 返回值 |
| --- | --- |
| `buffer == nullptr` / `size < 0` | `-EINVAL` |
| `iv == nullptr` / `ivSize == 0` / `ivSize > 16` | `-EINVAL` |
| scheme 不是四种之一 | `-EINVAL` |
| `cbcs` 的 pattern 违反 §5 约束 | `-EINVAL` |
| 该 key id 没有注册 key | `-ENOENT` |
| subsample 长度之和超过样本长度 | `-EINVAL` |
| `size == 0` | 返回 0（空样本不是错误） |

成功返回写入的字节数（恒等于 `size`）。

日志：缺 key、输入非法各有一条 `AF_LOGE`，且**每种只打一次**（成员标志
`mMissingKeyLogged` / `mMalformedLogged`，注册新 key 或 `clearKeys()` 后重新武装），
做法与 `HLSStream::mKeyFetchFailedLogged` 一致，避免每包刷屏。

---

## 7. 测试证明了什么

测试文件：`framework/tests/demuxer/cencDecrypterTest.cpp`（gtest）。

### 7.1 证明依据是外部已知答案，不是自证

1. **`cencAesPrimitive.ecbPathIsUnchained`**
   先钉死所用的 AES 原语语义：`avAESEncrypt::encrypt(..., iv = nullptr)` 是**无链接的单分组
   ECB 加密**（同一明文块两次同密文、不同明文块密文不同）。这是后面所有「测试内独立参考
   实现」的地基——如果内核 AES 的 `iv == nullptr` 通路不是 ECB，后面全部参考实现都错。
2. **`cencAesPrimitive.nistSp80038aCbcAes128Vectors`**
   用 **NIST SP 800-38A §F.2.1 CBC-AES128** 的四个已知答案向量（key
   `2b7e1516…4f3c`、IV `000102…0e0f`、四组明文/密文）证明：内核 AES-128 是标准 AES-128，
   且「ECB 原语 + 手工 XOR 链」搭出来的 CBC 与标准逐字节一致；解密方向同样验证。
3. **`cencDecrypter.nistSp80038aCtrVectorsThroughDecrypter`**
   用 **NIST SP 800-38A §F.5.1/§F.5.2 CTR-AES128** 的向量驱动 `CENCDecrypter` 本身：
   - 四个密文块各自单独解密 → 得到对应的四个明文块；
   - 四块拼接成 64 字节样本一次解密 → 证明计数器按 16 字节分组递增；
   - IV 分别用 8 字节（`f0f1…f6f7`）和 16 字节（`f0f1…feff`）→ 两种 counter block 布局
     得到同一组 keystream。
   只要 counter block 不是「前 8 字节 IV + 后 8 字节大端计数器从 0 起」，这些断言就会失败。

### 7.2 证明 subsample / pattern / CTS 的语义

4. **`cencDecrypter.multiSubsampleCtrRecoversPlaintext`**
   5 个 subsample（含一个 40 字节多块 protected、一个 1 字节 protected、一个空 subsample），
   用**测试文件内独立写的** CTR 参考实现（只用 ECB 原语 + 手工 XOR + 手工大端计数器）
   造密文，再让 `CENCDecrypter` 解密，断言逐字节还原；另外逐字节断言所有 clear 段没被动过。
   这条覆盖了 subsample 展开和 **per-subsample counter 重置**。
5. **`cencDecrypter.cbc1RecoversPlaintextIncludingCtsTail`**
   41 字节样本（2 整块 + 9 字节残余）走 CTS 路径，32 字节样本走纯 CBC 链；两条都由
   测试内独立参考实现（含 CBC-CS3 加密）造密文并断言还原。
6. **`cencDecrypter.cbcsPatternRecoversPlaintext`**
   两个 subsample（protected 33 字节与 48 字节），pattern `crypt=1, skip=1`：
   第一条覆盖「run + 尾部 CTS」，第二条覆盖「多个整 run」，并验证 IV 在每个 subsample
   起点重置、pattern 从 subsample 起点重新开始；同时断言非法 pattern（`crypt=0`、`crypt=10`、
   `9:9`、`1:10`）返回负值且不动 buffer。
7. **`cencDecrypter.cbcsPatternSkipNineBlocks`**
   `crypt=1, skip=9`（正好一个 160 字节 run）：断言前 16 字节被解密、**后 144 字节
   （skip 段）一个字节都没被改动**。
8. **`cencDecrypter.cencWholeSampleAndClearOnlyBoundaries`**
   16 字节 IV、无 subsample 信息的便捷重载等价于整样本一个 protected 区间、
   全 clear 的 subsample 不动数据、`size == 0` 返回 0。

### 7.3 证明失败不会返回垃圾

9. **`cencDecrypter.failuresReturnNegativeAndLeaveBufferUntouched`**
   11 种失败路径：未知 key id、非法 key id、未知 scheme、`ivSize == 0`、`ivSize == 17`、
   `iv == nullptr`、subsample 超出样本长度、`clear/protected` 都是 `UINT32_MAX`（验证校验里
   先加后比的溢出）、负 `size`、空 buffer、以及「没有 key 时的合法输入」——
   全部断言返回负值，且除了必然早退的两条外都断言 buffer 与输入**逐字节相同**。
10. **`cencDecrypter.keyIdNormalizationAndReplacement`**
    key id 归一化（大小写、`-`/空格分隔符）、非法参数拒绝、同 key id 重复注册 = 替换、
    `clearKeys()` 后 `hasKey()` 为 false。

---

## 8. 测试**没有**证明什么（诚实清单）

1. **没有证明与 demuxer / 播放链路的集成。** 没有在 `avFormatDemuxer` /
   `demuxer_service` 的读包路径上挂载、没有验证 `IAFPacket::getEncryptionInfo()` 的字段
   与 `decrypt()` 参数的实际对接、没有端到端播放验证。
2. **没有用真实 CENC 加密的 mp4 文件做验证。** 没有真实内容 + 真实 KID/key 的
   端到端解码比对；测试输入全部来自 NIST 向量与测试内参考实现。
3. **`cens` 没有单独测试。** 它在本实现里与 `cenc` 走同一条 CTR 代码路径，因此只由
   `cenc` 的用例间接覆盖；`cens` 语义上是否应该带 pattern（以及真实内容是否给非零
   `crypt_byte_block`）没有验证。
4. **CTS 的变体选择没有外部向量背书。** 本实现按 CBC-CS3 处理尾部（§4/§5）；
   NIST SP 800-38A Addendum 里 CBC-CS1/CS2/CS3 只在末两段交换与否上不同，而 ISO/IEC 23001-7
   的原文在本环境无法取得，**没有引用到 100% 确认的小节号**。测试里的 CTS 是「测试内的
   加密实现 ↔ 被测解密实现」互证，虽然其底层 AES 原语由 F.2.1/F.5.x 向量钉死，但
   **CTS 的整体正确性建立在「CBC-CS3 是正确的变体」这一判断上**。若真实内容表现是
   最后两段明文错位，第一个要复查的就是这里。
5. **`cbc1`/`cbcs` 与 FFmpeg `mov.c` 的解密行为不完全一致**（见 §5 末尾）：FFmpeg 的
   `av_aes_crypt` 不提供真正的跨块 CBC 链、也不做 CTS。本实现刻意按标准做。若需要与
   FFmpeg 产出的「已解密」数据逐字节一致，需要重新评估。
6. **没有覆盖超长样本 / 计数器回绕。** 低 64bit 计数器在 2^64 个分组后回绕，测试里没有
   构造这种极端样本（现实中不可能出现）。
7. **性能没有测评。** 每包的堆分配只有「subsample 区间列表」一个
   （`mRanges` 复用 capacity），但没有做 benchmark，也没有与平台 CDM 做性能对比。
8. **线程安全只做了约定，没有做并发测试。** 约定：`setKey`/`clearKeys` 只在 demuxer 线程调，
   `decrypt` 在读线程调；同一实例不可重入（内部有临时缓冲成员）。

---

## 9. 构建接入（需要工程侧各加两行）

`CENCDecrypter.cpp` 与测试文件都是新增文件，但**必须挂到现有 CMake 目标上**才会被编译：

1. `framework/demuxer/CMakeLists.txt` 里 `add_library(demuxer ...)` 的 `target_sources(demuxer PRIVATE …)`，
   在 `sample_decrypt/ISampleDecrypt2c.h` 这一行之后追加：

   ```cmake
           sample_decrypt/CENCDecrypter.cpp
           sample_decrypt/CENCDecrypter.h
   ```

   即把这一段（当前内容）：

   ```cmake
           sample_decrypt/ISampleDecrypt2c.cpp
           sample_decrypt/ISampleDecrypt2c.h
           decrypto/IAESDecrypt.h
   ```

   改成：

   ```cmake
           sample_decrypt/ISampleDecrypt2c.cpp
           sample_decrypt/ISampleDecrypt2c.h
           sample_decrypt/CENCDecrypter.cpp
           sample_decrypt/CENCDecrypter.h
           decrypto/IAESDecrypt.h
   ```

2. `framework/tests/demuxer/CMakeLists.txt` 里 `target_sources(demuxerUnitTest PRIVATE …)`，
   在 `demuxerUnitTest.cpp` 之后追加 `cencDecrypterTest.cpp`：

   ```cmake
   target_sources(demuxerUnitTest
           PRIVATE
           demuxerUnitTest.cpp
           cencDecrypterTest.cpp
           )
   ```

   `demuxerUnitTest` 已经链接 `demuxer`、`avutil`、`gtest_main`，不需要新的链接项。

   注意：
   - `framework/windows.cmake` 里有 `set(BUILD_TEST OFF)`，要编测试需要 `-DBUILD_TEST=ON`；
   - `demuxerUnitTest` 里还有若干**联网**用例，本文件无网络依赖，建议只跑本文件：
     `--gtest_filter=cencDecrypter.*:cencAesPrimitive.*`（或设 `GTEST_FILTER` 环境变量）。

> 目录 `framework/tests/demuxer/cencDecrypterTest/` 下另附了一份独立的
> `CMakeLists.txt`，用于「想单独出一个 `cencDecrypterTest` 可执行文件」的场景：
> 那种情况下在 `framework/tests/CMakeLists.txt` 里加 `add_subdirectory(demuxer/cencDecrypterTest)`
> 即可（它自带 gtest 链接，不会重复 `FetchContent`）。
> 若采用上面方案 2，则把这个冗余目录删掉即可，它不会被任何父 `CMakeLists.txt` 引用。


---

## 10. 参考

- NIST SP 800-38A（2001），§F.2.1 CBC-AES128、§F.5.1/§F.5.2 CTR-AES128 测试向量。
- NIST SP 800-38A Addendum（2010），CBC-CS1/CS2/CS3（ciphertext stealing）。
- ISO/IEC 23001-7 *Common encryption in ISO base media file format files*：
  `cenc`/`cens`/`cbc1`/`cbcs` scheme、`senc`/`saiz`/`saio`/`tenc`/`schm` box、
  pattern encryption（`crypt_byte_block`/`skip_byte_block`）。**小节号未在本环境核对，见 §8.4。**
- FFmpeg：`libavutil/aes.c`、`libavutil/aes_ctr.c`、`libavformat/mov.c`
  （`cenc_scheme_decrypt` / `cbcs_scheme_decrypt`）。
