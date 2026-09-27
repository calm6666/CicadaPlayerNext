# Android 解码数据面迁到 NDK `AMediaCodec`（**异步**，NDK r25 / minSdk 24）

> 状态：**设计已定，待施工**。本文是这一迁移的设计基准（design of record），
> 与 `ARCH-REDESIGN.md` §6 的"Android 轻量红线"、`SINGLE-DECODER-REFACTOR.md`
> 的单解码器/落点不变量配套。
>
> 已定的三个决策（用户 2026-09-27 指定）：
> 1. **只用 NDK r25（r25c，`25.2.9519653`）**，不升级到 r28；
> 2. **数据面只用异步**，**不保留同步轮询**；
> 3. **必须保兼容性**（minSdk 24 不抬）。

---

## 一、为什么要做（根因，而不是"性能优化"）

`ARCH-REDESIGN.md:120` 已经写死红线：

> Android 轻量红线：**内核不得依赖 Java 侧回调做节拍**；JNI 调用只用于"创建/配置/写入"这类必须项。

而现状恰好违反它：每帧的数据面是

```
mediaCodecDecoder.cpp (C++)
  → MediaCodec_Decoder.cpp (JNI 桥，15 个方法)
    → MediaCodecDecoder.java (持有 android.media.MediaCodec，异步回调在这条 Java 路上)
```

后果有三条，都是实测过的：

1. **每帧 4~5 次 JNI**（`dequeueInputBufferIndex` / `queueInputBuffer` /
   `dequeueOutputBufferIndex` / `getOutput` / `releaseOutputBuffer`）。
2. **每帧 2 次拷贝 + 1 次 Java 堆分配**：
   `MediaCodec_Decoder.cpp:456-459` 每帧 `NewByteArray(size)` + `SetByteArrayRegion`
   把压缩帧拷进 Java 堆；Java 侧 `queueInputBufferInner` 再 `inputBuffer.put(...)` 拷第二次。
   GC 抖动即由此而来。
3. **节拍依赖 Java 回调**：`onInputBufferAvailable` 不来，解码器就永久没有输入缓冲。
   2026-09-27 真机日志（本地文件与 DASH 各一次）就是这个形态：
   `async input path looks dead (criterion=no-input-callback-after-flush,
   waitedMs≈1000, mInputWaitCount≈30)` → 无帧上屏 → 画面永久卡死（KPI total fps 1.0）。
   根因已定位并修复（见 §六.1）：异步模式下 `flush()` 之后**必须**再 `start()`。

本文的做法把 1)、2) 消掉（数据面全部在 C++ 内完成），3) 的"依赖 Java 回调"也一并消掉
（回调改为 NDK 自己的 `AMediaCodecOnAsyncNotifyCallback`，不再跨语言）。

---

## 二、事实基线（全部从本机 NDK r25c 头文件核实，不是记忆）

核实对象：
`D:\Android\Sdk\ndk\25.2.9519653\toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\include\media\NdkMediaCodec.h`

### 2.1 API 级别（`__INTRODUCED_IN` 原文）

| 函数 | 级别 | 用途 |
|---|---|---|
| `AMediaCodec_createCodecByName` | **21** | **按名字精确创建**（Java 选中哪颗就用哪颗）——注意：**不是 28**，此前文档/口头把它记成 28 是错的 |
| `AMediaCodec_createDecoderByType` | 21 | 按 MIME 让平台选 |
| `AMediaCodec_delete` | 21 | 释放 |
| `AMediaCodec_configure` / `_start` / `_stop` / `_flush` | 21 | 生命周期 |
| `AMediaCodec_getInputBuffer` / `_getOutputBuffer` | 21 | 取缓冲区**直接指针** |
| `AMediaCodec_dequeueInputBuffer` / `_dequeueOutputBuffer` | 21 | 同步取（**本设计不使用**） |
| `AMediaCodec_queueInputBuffer` / `_queueSecureInputBuffer` | 21 | 喂码流 |
| `AMediaCodec_releaseOutputBuffer` | 21 | 输出（surface 模式 `render=true` 直出） |
| `AMediaCodec_setOutputSurface` | 21 | 换输出面 |
| `AMediaCodecCryptoInfo_*` | 21 | DRM 子样本（keyId / IV / clear+encrypted bytes） |
| `AMediaCodec_createInputSurface` / `_getInputSurface` / `_setParameters` / `_signalEndOfInputStream` | 26 | 编码器方向，解码不用 |
| `AMediaCodec_getName` / `_releaseName` | 28 | 读实际选中的 codec 名（**用于核对**） |
| `AMediaCodec_getBufferFormat` / `_getInputFormat` / `_releaseCrypto` | 28 | 逐 buffer 格式 / 释放 crypto |
| `AMediaCodecActionCode_isRecoverable` / `_isTransient` | 28 | 错误分类 |
| **`AMediaCodec_setAsyncNotifyCallback`** | **28** | **本设计的核心：异步模式** |
| `AMediaCodec_setOnFrameRenderedCallback` | **33**（Android T） | 上屏确认（见 §七） |

### 2.2 异步回调的原文语义（头文件引用）

```c
typedef struct AMediaCodecOnAsyncNotifyCallback {
      AMediaCodecOnAsyncInputAvailable  onAsyncInputAvailable;
      AMediaCodecOnAsyncOutputAvailable onAsyncOutputAvailable;
      AMediaCodecOnAsyncFormatChanged   onAsyncFormatChanged;
      AMediaCodecOnAsyncError           onAsyncError;
} AMediaCodecOnAsyncNotifyCallback;
```

- `onAsyncOutputAvailable(codec, userdata, index, bufferInfo)`
  —— "Called when an output buffer becomes available"，`bufferInfo` 带该 buffer 的信息。
- `onAsyncFormatChanged(codec, userdata, format)`
  —— "Called when the output format has changed"，新格式就是 `format`。
- `onAsyncError(codec, userdata, error, actionCode, detail)`
  —— 原文：`actionCode` 用 `AMediaCodecActionCode_isRecoverable()` /
  `AMediaCodecActionCode_isTransient()` 判定；**两者都为 false ⇒ 致命，必须删除该 codec**。
- 线程约束（原文）：`AMediaCodec_setAsyncNotifyCallback`
  **"should not be called on the callback thread"**；回调线程上不得做重活。

### 2.3 平台契约（决定本设计的一条硬不变量）

AOSP `MediaCodec.flush()` 原文（Java 与 NDK 是**同一个 ACodec 状态机**）：

> Upon return, all indices previously returned ... become invalid, and all buffers are owned
> by the codec. **If the codec is configured in asynchronous mode, call start after flush has
> returned to resume codec operations. The codec will not request input buffers until this
> has happened.**

⇒ **`AMediaCodec_flush()` 之后必须 `AMediaCodec_start()`**，否则异步回调永不恢复。
这条在 Java 侧已经按它修过（commit `99cfd107`），C 侧必须原样保留。

---

## 三、决策与兼容性策略

| 决策 | 内容 |
|---|---|
| **异步优先** | 数据面一律用 `AMediaCodec_setAsyncNotifyCallback`；**代码里不出现任何同步 `dequeue*` 调用** |
| **不保留同步轮询** | 不写"拿不到回调就轮询"的降级路；也不保留 Java 侧 `sAsyncBroken` → 下个实例走轮询那套（连同它一起删） |
| **兼容性** | 按**平台能力**绑定实现，不是配置开关：<br>· API **≥ 28**：`NdkAsyncBinding`（`AMediaCodec` + 异步回调）<br>· API **24–27**：`JavaAsyncBinding`（**现有的** Java 异步实现，保持不动）<br>两条都是**异步**，所以"无同步轮询"在所有受支持系统上都成立 |
| **NDK 版本** | 只认 r25c；不引入 r28 的 16 KB 对齐默认值等变化（那条与本迁移分开做） |
| **弱符号** | `AMediaCodec_setAsyncNotifyCallback` 用 `__attribute__((weak_import))` 声明，运行期判空决定绑定 |

弱符号用法（正式的可用性判据，不是兜底）：

```cpp
/* 仅 API 28+ 存在；weak_import 让 r25 编出的 .so 也能在 API 24 上加载 */
extern "C" media_status_t AMediaCodec_setAsyncNotifyCallback(
        AMediaCodec *, AMediaCodecOnAsyncNotifyCallback, void *) __attribute__((weak_import));

static bool ndkAsyncDecoderAvailable() {
    return AMediaCodec_setAsyncNotifyCallback != nullptr;   /* 24~27 → nullptr → 用 Java 绑定 */
}
```

`AMediaCodec_createCodecByName` / `configure` / `start` / `stop` / `flush` /
`getInputBuffer` / `getOutputBuffer` / `queueInputBuffer` / `releaseOutputBuffer` /
`CryptoInfo_*` 都是 **21**，minSdk 24 下**无需**弱符号、可直接调。

---

## 四、目标架构

### 4.1 分层（只有"绑定"换，内核一行不改）

```
SuperMediaPlayer / IDecoder（不变）
   └── framework/codec/Android/mediaCodecDecoder.{h,cpp}   ← 保留：flush 状态机、渲染门、CSD 组装、诊断
         └── AndroidDecoderBinding（新增抽象，仅本平台内部）
               ├── NdkAsyncBinding   ← 新写：AMediaCodec + setAsyncNotifyCallback（API≥28）
               └── JavaAsyncBinding  ← 现有：JNI + MediaCodecDecoder.java 异步（API 24~27）
```

- 同一时刻**只有一个**解码器实例（单解码器模型不变；不是双解码器）。
- `IDecoder` 接口布局不变；若必须加虚函数，只能追加在 vtable **末尾**。
- 控制面（每解码器一次）仍走 JNI：能力查询/选 codec、OEM 钩子、Surface 获取。
- 数据面（每帧）**不再有 JNI**。

### 4.2 数据流（每帧，零拷贝 / 零 JNI）

```
输入：内核解码线程 AMediaCodec_getInputBuffer(idx) → 直接从 IAFPacket 内存拷贝/分段写入
      → AMediaCodec_queueInputBuffer(idx, 0, size, ptsUs, flags)
输出：dequeue 由回调驱动 → 内核解码线程 AMediaCodec_getOutputBuffer(idx)（surface 模式不必取）
      → 构造 IAFFrame（带 pts/flags）→ AMediaCodec_releaseOutputBuffer(idx, render)
```

### 4.3 线程模型（回调只入队，绝不做活）

平台在**一个**内部线程上回调。规则：

1. 回调体**只做**三件事：把 index（输出还要带 `AMediaCodecBufferInfo` 副本）放进受锁保护的队列、
   `notify` 条件变量、返回。**不拷贝帧数据、不构造 IAFFrame、不调 release、不打重日志。**
2. 所有实际工作（写输入缓冲、queue、取输出、构造帧、release）都在**现有的内核解码线程**上做
   —— 与今天 JNI 调用来自同一个线程，架构不变。
3. 绝不在回调线程上调用 `AMediaCodec_setAsyncNotifyCallback`（头文件明文）。
4. **索引失效**：`flush()` 之后旧 index 全部作废，回调队列必须整体丢弃；随后 `start()` 才会来新回调。
5. **背压/防饿死**：内部永远至少留 1 个输入 index 不占用（不囤满），否则 codec 无输入可用
   —— 这正是历史上出现过的一类死锁（与 flush 后不 start 是两回事，必须一起防）。

### 4.4 生命周期与不变量

```c
/* 建（控制面，JNI 只在这里） */
codec = AMediaCodec_createCodecByName(nameFromJava);        /* API 21，精确锁定 Java 选中的那颗 */
AMediaCodec_configure(codec, fmt, windowOrNull, NULL, 0);   /* fmt: AMediaFormat，含 csd-0 */
AMediaCodec_setAsyncNotifyCallback(codec, cb, this);        /* 必须在 start 之前 */
AMediaCodec_start(codec);

/* seek / 切档 flush（唯一写点，写死顺序） */
AMediaCodec_flush(codec);          /* 1) 旧 index 全部作废 */
dropAllQueuedIndices();            /* 2) 本地队列同步作废 */
AMediaCodec_start(codec);          /* 3) 不变量：不调它，异步回调永不恢复 */

/* 错误 */
onAsyncError: isRecoverable/isTransient 任一为真 → 交给既有的错误驱动重建；
              两者都假 → 致命：delete codec 并由控制面重建（不尝试"降级轮询"，本设计没有轮询）
```

### 4.5 与既有机制的对接

| 既有机制 | 迁移后 |
|---|---|
| `mFlushState`（0 stop / 1 flushed / 2 running / 3 eos） | 保留，语义不变；只在 flush 与 start 两处与 NDK 调用配对 |
| 渲染门（`mRenderGatePts` / `mRenderGateHit` / `mRenderHold`） | 保留（内核侧判据，与谁提供帧无关） |
| `invalidateFrameReleases()`（flush/stop 后旧 index 作废） | 保留；NDK 侧由"丢弃回调队列"实现，语义一一对应 |
| `getVideoDecoderFlags()` 的 `DECFLAG_HW` 维护 | 保留（重建后仍必须是硬解标志） |
| `sAsyncBroken` + "下个实例走轮询" | **删除**（本设计无轮询；根因已修） |
| DRM（`setDrmInfo` → `JEncryptionInfo`） | 第一阶段保留 JNI；后续可用 `AMediaCodecCryptoInfo_*`（API 21）+ `queueSecureInputBuffer` 迁走 |

---

## 五、分阶段施工与验收

### 阶段 A：可验证性 + 基线（不改行为）
- A1 先跑通 Android 构建（`gradlew :cicadaplayer:assembleDebug`），确认 `libmediandk` 可链接。
- A2 `git tag pre-ndk-mediacodec`（回滚锚点）。
- A3 `simpleperf record -g -p <pid>`：视频解码线程上 `art::JNI*`、`NewByteArray`、
  `SetByteArrayRegion`、`memcpy`、`art::gc::*` 占比；记 KPI fps、连续 seek 延迟、CPU。
- 验收：基线表格入库（本文档追加一节），否则后面无法证明"性能更好"。

### 阶段 B：控制面固化（仍 Java，一次性）
- B1 Java 侧一次性给出：选中的 codec 名、mime、宽高、CSD、Surface 引用、是否 secure、OEM 钩子结果。
- B2 C++ 侧 `AMediaCodec_createCodecByName` + `AMediaCodec_getName` 核对（28+ 才有 getName；
  24–27 走 Java 绑定，天然一致）。
- 验收日志：`[ndk-codec] selected=<Java 选中> created=<NDK 实际>`，同机同源两者必须相等。

### 阶段 C：NDK 异步数据面（核心）
- C1 `AMediaFormat` 逐字段对照（把 Java `MediaFormat` 全字段打印，再 1:1 建 `AMediaFormat`；
  csd-0 尤其要核）。
- C2 异步回调注册 + 队列 + 条件变量；输入侧直接写 `getInputBuffer` 指针。
- C3 输出侧 surface 模式 `releaseOutputBuffer(idx, true)`；dummy/隧道模式保留 Java 侧 Surface。
- C4 `flush → start` 不变量（§4.4），并把 `mFlushState` 复位语义对齐。
- C5 错误映射（`onAsyncError` + actionCode）。
- 验收：同一片源逐项对比旧实现（起播/跨段 seek/连续拖拽/暂停态 seek/切档/音画同步/字幕/EOS），
  且必须出现：每次 seek 有 `seek landing frame accepted`、**无** `async input path looks dead`、
  **无** `onAudioException -1003`。

### 阶段 D：切换（按平台能力路由；**保留** Java 数据面作为 24–27 的绑定）
- 调度器（`codecBinding.cpp` 的 `AndroidCodecBinding`）在**第一个会碰 codec 的调用**上路由：
  - `setDrmInfo` 成功（secure 内容，需要 Java MediaCrypto 全链路）⇒ 整实例 `java-async`；
  - 视频 surface 为空、或 `usePlaceholderSurface`（B2 占位面是 Java 侧技巧）⇒ 整实例 `java-async`；
  - 其余且 API ≥ 28 ⇒ `ndk-async`；NDK configure 失败 ⇒ 进程内一次性熔断并整体回落 `java-async`。
- **要删的**：`sAsyncBroken` 及其"下个实例走轮询"分支（本设计不存在轮询路径）；
  `mediaCodecDecoder.h` 里从未被使用的 `mUseNdk`。
- **要保留的**：`MediaCodec_Decoder`（JNI 数据面）与 Java 数据面 —— 它们是 API 24~27 的
  **异步绑定**，不是"待删的双实现"；`OutputBufferInfo.cpp` / `JEncryptionInfo.cpp` 同理由它使用。
  > 本节此前写"删除 JNI 数据面方法"是**错的**：那会砍掉老系统上唯一的异步路径，与"保兼容性"冲突。
- 验收：编解码路径内**同步 `dequeue*` 轮询**引用为 0、`sAsyncBroken` 引用为 0；
  `ndk-async` 生效时每帧 0 次 JNI（simpleperf 中解码线程 `art::JNI*` ≈ 0）；
  `SINGLE-DECODER-REFACTOR.md` §五 残余扫描 0 命中；Android 构建 0 警告。

### 阶段 E：性能复测 + 回退判定
- 同一套 simpleperf 指标对比 A3；文档追加"实测对比"。
- 不达标（解码线程 JNI/拷贝占比没有显著下降，或出现新卡顿）⇒ `git revert` / 回 tag，不留半成品。

---

## 六、错误与坑（都要在代码注释里留证据）

1. **flush 后不 start**：异步回调永久停摆（已在 Java 侧修，C 侧照抄不变量）。
2. **回调线程做重活**：平台明文禁止；也是延迟来源。
3. **囤满输入 index**：codec 无输入可用 → 与"没有回调"表现一样，难区分；靠"永远留 1 个"防。
4. **CSD/format 差异**：HEVC 的 hvcC、AAC 的 ADTS 标志位，必须逐字段对照，不能照抄 Java 语义。
5. **API 28 以下没有 NDK 异步**：这不是"坑"而是平台事实，用 Java 异步绑定覆盖（§三）。
6. **`AMediaCodec_getName` 是 28**：24–27 无法核对 codec 名——因为那条路本来就还是 Java 实现，
   不存在"Java 选 A、NDK 建 B"的可能。

---

## 七、相关但与本次迁移无关的两个事实（避免再记错）

- `AMediaCodec_setOnFrameRenderedCallback` / `onFrameRendered(codec, userdata, mediaTimeUs, systemNano)`
  原文："Called when an output frame has rendered on the output surface"，
  **`Available since Android T`（API 33）**。它能给出"这一帧真的上屏 + 系统时间"的权威事实，
  正好匹配"首个最终上屏帧必须是包含目标时刻的那一帧"这条硬要求；
  但 minSdk 24 下只能在 **33+** 设备上启用（弱符号判空），24~32 继续用现有渲染器日志取证。
- `AMediaCodec` 自 **API 21** 就是公共 NDK API（不是"升级 r28 才有"）。
  升级 NDK r28 与本次迁移是**两件独立的事**；本设计按用户要求固定在 r25c。

---

## 八、决策已定（2026-09-27，用户拍板）

1. **数据面只用异步**，**不保留同步轮询**；NDK 异步绑定（`setAsyncNotifyCallback`）为 API ≥ 28 的主路径。
2. **只用 NDK r25c**，不升级 r28；minSdk **24 不抬**（24~27 走 Java 异步绑定）。
3. **DRM/secure 本期不迁**：`setDrmInfo` 成功即把整实例交给 Java 绑定（MediaCrypto 全链路在 Java 侧）。
4. `onFrameRendered`（API 33）**本期不纳入**（覆盖设备太少），继续用现有渲染器日志取证。
5. **不使用 FFmpeg 自带的 mediacodec**（理由见 §十）。

## 九、实现记录（阶段 A/B/C/D 已完成编码，编译验证见下）

| 文件 | 角色 |
|---|---|
| `framework/codec/Android/codecBinding.h/.cpp` | `IAndroidCodecBinding` 抽象 + `JavaCodecBinding`（1:1 转发既有 JNI）+ `AndroidCodecBinding` 调度器 + `createAndroidCodecBinding()` |
| `framework/codec/Android/ndkCodecBinding.h/.cpp` | NDK 异步绑定：`AMediaCodec` + 回调队列 + 条件变量 + `flush→start` 不变量 + 防饿死 |
| `framework/codec/Android/mediaCodecDecoder.h/.cpp` | 成员由 `MediaCodec_Decoder*` 换成 `std::unique_ptr<IAndroidCodecBinding>`；删除从未使用的 `mUseNdk` |
| `framework/codec/Android/jni/MediaCodec_Decoder.h/.cpp` | 新增**控制面** `selectCodecName(...)`（只选不建，返回 codec 名） |
| 两份 `MediaCodecDecoder.java` | 新增 `selectCodecName(...)`；**删除 `sAsyncBroken` 与"下个实例走轮询"分支** |
| `framework/codec/CMakeLists.txt` + 两个模块 `CMakeLists` | 新增上述源文件；Android 链接 `mediandk` |

**路由规则（`codecBinding.cpp` 的 `AndroidCodecBinding`，按平台能力，不是配置开关）**
- `setDrmInfo` 成功（secure）⇒ 整实例 `java-async`（必要时把已 configure 的配置在 Java 侧重放一次）；
- 其余 + API ≥ 28 ⇒ `ndk-async`；**无真 surface / 占位面（切档 B2）也走 `ndk-async`** ——
  NDK 侧通过控制面取 Java 的**同一块 1x1 DummySurface**（`ensureDummySurface()`，见 `getDummySurface()`）
  维持 surface 模式；连 dummy 都拿不到时才 configure 失败并整体回落 `java-async`；
- NDK configure 失败 ⇒ 进程内一次性熔断（`NdkCodecBinding::markUnavailable`）并整实例回落 `java-async`。
- 两条都是**异步**，所以"无同步轮询"在所有受支持系统上成立。
- `setOutputSurface(nullptr)`（后台/隧道切占位面）同样切到那块 dummy，与 Java 路径语义逐字一致
  （**此前列为已知差异的这条已消除**）。

**一个实证结论（值得记住）**：`AMediaCodec_setAsyncNotifyCallback`(28) / `setParameters`(26) / `ActionCode_is*`(28) 在 `__ANDROID_API__ = 24` 的构建里**不能直接调用**，编译器会报
`'AMediaCodec_setAsyncNotifyCallback' is unavailable: introduced in Android 28`，而自己再写
`__attribute__((weak_import))` 重声明**压不住**头文件已有的 availability 属性（首个声明生效）。
最终采用 **dlopen + dlsym 运行期解析**（句柄进程内保留、不 dlclose）——判据就是"这台设备的
libmediandk 有没有这个符号"，比版本号更准。巧合的是 FFmpeg 上游也是这么做的
（`external/external/ffmpeg/libavcodec/mediacodec_wrapper.c` 的 NDK 后端
`GET_SYMBOL(setAsyncNotifyCallback)`），说明这是标准姿势。

**回滚锚点**：`git tag pre-ndk-mediacodec`（已推送）。整块回退只需回到该 tag。

## 十、为什么不用 FFmpeg 自带的 mediacodec（行号证据，2026-09-27 核实）

用户提出"FFmpeg 9 不是直接支持安卓硬解吗、鸿蒙也一样，自己接是 FFmpeg 4 的老做法"。
核实结论（**证据全部来自本仓库自带的 FFmpeg 源码树 `external/external/ffmpeg/`**）：

| 事实 | 证据 |
|---|---|
| 上游**有** Android 硬解，且 wrapper 有**两套后端**（JNI 与 NDK） | `libavcodec/mediacodec_wrapper.c`：`mediacodec_jni_*` 与 `mediacodec_ndk_*` 两套 vtable |
| **解码器是同步轮询**，没有异步 | `mediacodecdec_common.c:923` `ff_AMediaCodec_dequeueInputBuffer`、`:1015` `dequeueOutputBuffer`；解码路径**零** `setAsyncNotifyCallback` |
| **异步只做给了编码器** | `mediacodecenc.c:583-596`（`ff_AMediaCodec_setAsyncNotifyCallback` + 失败自动退回同步的日志）、`:1045` 选项 `ndk_async` |
| **没有 DRM/secure 通路** | `mediacodecdec_common.c:850` `ff_AMediaCodec_configure(codec, format, s->surface, NULL, 0)` —— crypto 传 `NULL` |
| **零拷贝有**（与异步是两件事） | `s->surface` / `ff_mediacodec_surface_ref(...)`、`av_mediacodec_release_buffer[_at_time]` |
| 上游**有** OpenHarmony 解码器 | `libavcodec/ohcodec.c`、`ohdec.c`、`ohenc.c` 就在本仓库的 FFmpeg 树里 |
| **本工程 FFmpeg 构建并未启用它们** | `build_tools/**` 里 `mediacodec|enable-jni|ohcodec|ohdec` **零命中** |

⇒ 结论：开 `--enable-jni --enable-mediacodec` **不会带来异步解码**（解码器只有同步轮询），
也给不了 DRM、占位面/隧道、选 codec 黑名单、OEM 钩子这些本内核依赖的语义。
所以本项目继续走"自己接"的路线；FFmpeg 那条路保留为**将来统一解码栈**的候选，
前提是先给上游 decoder 补 async（模板就是它的 encoder 那份实现）。

> 另外：`docs/Packaging_HarmonyOS.md:109` 原写"FFmpeg 上游**没有** OHOS hwaccel"，
> 与本仓库 FFmpeg 树（`libavcodec/ohdec.c` 存在）**矛盾**，已按本表修正。

## 十一、真机验收与性能对比（用户侧执行；本环境无 Android 设备）

### 11.1 先看"这次走了哪条绑定"
```powershell
adb logcat -c
adb logcat -s AliFrameWork CicadaQuality | Select-String -Pattern "ndk-codec|seek landing|async input path|audio first frame after seek|onAudioException|baseTimeout|Clarity"
```

**期望（API ≥ 28、非 DRM、非占位面）**
- `[ndk-codec] binding=ndk-async (AMediaCodec + setAsyncNotifyCallback)`
- `[codec-select] ndk control plane: … -> X` 与 `[ndk-codec] ndk-async video configured: codec=X` **同名**
- 每次 seek 均有 `seek landing frame accepted`
- **不出现**：`async input path looks dead`、`onAudioException -1003/-1004`、`baseTimeout`、`baseStop`

**预期走 java-async（是设计，不是故障）**
- API 24–27：`binding=java-async (API<28: libmediandk has no setAsyncNotifyCallback)`
- Widevine/secure：`secure content ⇒ switch this decoder instance to java-async`
- NDK configure 失败（罕见）：`ndk configureVideo failed ⇒ fall back to java-async` +
  `NDK async data plane disabled for this process: …`

### 11.2 性能对比（阶段 A3 基线 vs 迁移后）
```powershell
$p = (adb shell pidof com.cicada.player.compose).Trim()
adb shell simpleperf record -g -p $p --duration 20 -o /data/local/tmp/perf.data
adb shell simpleperf report -i /data/local/tmp/perf.data --sort dso,symbol | Select-Object -First 40
```

同一片源、同一动作下看三件事：
- 解码线程上 `art::JNI*` / `NewByteArray` / `SetByteArrayRegion` 占比 —— `ndk-async` 下应 ≈ 0；
- `art::gc::*` 与 GC 次数 —— 应下降（不再每帧分配 Java `byte[]`）；
- 视频线程 CPU 占比、KPI fps、连续 seek 延迟 —— 不应退化。

### 11.3 回退
`git reset --hard pre-ndk-mediacodec`（tag 已推送），或
`git revert 74ec39a5 7c6d1ce0 31a65e7c`（保留文档与设计记录）。
