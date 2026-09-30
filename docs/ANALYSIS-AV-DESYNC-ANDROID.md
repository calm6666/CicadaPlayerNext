# Android「seek 之后 / 播放途中音视频不同步」—— 日志 + 代码定位与最小修复

> 对象日志：`新建文本文档.txt`（496 KB，**Android logcat**，不是播放器单写日志 ——
> 判据见 §2.0）。进程 `com.cicada.player.compose`（pid 32443），内核库 `libCicadaPlayer.so`
> `external version is 20260906_18:31:42_359c1cce`，日志时间 `2026-09-29 12:14:42 → 12:19:14`。
> 期间测了两条片源：
>
> | 段 | 时间 | 片源 | 证据 |
> |---|---|---|---|
> | 第 1 段 | 12:14:47 → 12:16:31 | **本地视频文件**（无分片、无协议日志） | 全文没有 `[seek] dash/hls`、没有 `DashManager/HLSManager`；`mPtsDiscontinueDelta = 20000000`（:90）；音频 44100/2ch（:154/:191）；视频 DAR 1920:1080、帧间隔 40 ms（25 fps） |
> | 第 2 段 | 12:18:23 → 12:19:14 | **DASH**（MinIO `output.mpd`，6 档视频、共 7 条流） | `[dash] 清单里有 6 条视频档、共 7 条流（最低档 id=5）`（:1554）、`[seek] dash video/audio` ×10 |
>
> 与既有分析的关系：`platform/QtPlayer/docs/` 下那三份（`ANALYSIS-HLS-VS-DASH.md`、
> `PLAN-QUALITY-SWITCH-FIX.md`、`ANALYSIS-QUALITY-SWITCH-FREEZE.md`）讲的是 **Qt/桌面端
> 的清晰度切换卡死**，以及 `mActiveVideoPtsOffset` / `timePosition` 两轴问题。
> 本文是**另一件事**：Android 上的 **A/V 同步相位**，根因在**音频落点**而不是视频/切换。
> 两份结论不冲突，`mPtsDiscontinueDelta` 那条本文也在 §4.3 里按本文证据重新判定。
>
> **验证程度**：本文每一条都标了【已证实】/【推测】。代码改动见 §7；
> **没有运行时验证**（本会话 shell 不可用），编译与实测由用户做。

---

## 1. 结论速览

| # | 结论 | 强度 |
|---|---|---|
| 1 | 这条日志里 **A/V 不同步的直接证据是"seek 之后第一帧音频落在 seek 目标点之前"**：本地文件 8 次 seek 的音频首帧 − 目标点 = **8 / −1142 / −2198 / −1420 / −3102 / −770 / −1043 / −4024 ms**（7 次为负，最大 −4024 ms；唯一的正值 +8 ms 是"音频先于视频落点到达、闸门还在"的那一次） | 【已证实】日志 + 数值 |
| 2 | 本地文件那次最严重：`audio first frame after seek: pts=555885714 target=557028000`，紧接着 `correct audio and master clock offset is 766275`（应为 0）→ `audio clock drift: correction deltaTime=300000 us (capped at +-300000)`（顶到限幅）→ 若干条 `audio timeline re-anchored (offset 1811173 > 1s)` | 【已证实】日志 :328/:331/:332/:489 |
| 3 | **根因 1（内核，通用）**：音频的"落点闸门"和视频共用同一个 `mDiscontinuity.filterActive`，而它的关闭点是"**视频**落点帧被采纳"。音频与视频在 `render()` 里是两次独立调用，于是存在**一帧级竞态**：视频先采纳落点并关闸，下一轮才轮到那帧落在目标点**之前**的音频 —— 闸门已经不在，它就被直接推进了音频设备 | 【已证实】代码 `SuperMediaPlayer.cpp:4617` + 日志时序（§4.1） |
| 4 | **根因 2（内核，通用）**：音频**时间轴的建立点**取的是"第一帧自己的 pts"（`mAudioTime.startTime = pts`），而不是 seek 目标点。首帧一旦在窗口外，整条音频记账（`mPlayedAudioPts` / `mAudioTime`）就从窗口外起算，offset 恒偏、re-anchor 反复触发 | 【已证实】代码 + 日志 :328/:331/:332 |
| 5 | 危害链条**与代码里自己的注释完全一致**：音频时钟 = `目标点 + 设备已消费量`；设备里一旦混进 [落点, 目标) 的音频，"设备消费"就被算成"走在目标点之后" ⇒ **音频参考超前内容** ⇒ `RenderVideo` 判视频"迟到"、丢帧/跳关键帧。日志算术在本地文件段**逐微秒验证**了 `master = audioBase + consumed`（§3.3 第 1~2 组） | 【已证实】日志算术 |
| 6 | **DASH 这一条另有独立缺陷**：`DashStream::seek()` 返回的是**分片起点**（`DashStream.cpp:1246-1256/1381`），`DashManager::seek()` 又把它当作音频的 seek 目标（`DashManager.cpp` 改前 `:488/:490/:502`），于是**音频的 seek 目标比视频的早 369 / 760 / 1932 / 2997 / 5176 ms**（5 次 seek 全是这个方向）。**【已修（代码级），见 §7.4】**DASH 与 HLS 的 manager 都改成"每一路都按**用户请求值**定位"；另外两处修正：① **HLS 也有同一条缺陷**（原来说"HLS 返回请求值"是错的，见 §4.4 尾注）；② 原表"音频首帧比视频落点超前 371 667 ~ 1 938 000 µs"是**两种不同量的比较**，不是相位差的直接证据（§4.4 尾注） | 【已证实】日志 + 代码；修复效果待运行时验证 |
| 7 | **本次 Android 音频输出走的是 C 接口 AAudio，不是 Java AudioTrack** | 【已证实】§6 |
| 8 | 「播放途中慢慢漂」：本会话没有测到净漂移（内容时间/墙钟比 0.99~1.0），但日志里有 `TIMEPOS reSync` 以 1 行/109 ms 的频率、每次把主时钟**向前拽 ~200 ms**（12:18:33→12:18:48、12:18:53→12:19:03 两大簇）。这段是 DASH 段，且**没有**对应的音频 pts 日志，只能定到"主时钟在追音频参考"这一层 | 【已证实】现象 / 【推测】成因 |

---

## 2. 日志本身：它是什么、怎么看

### 2.0 【已证实】它是 Android logcat

- 行首是 `2026-09-29 12:14:42.798 32443-32443 <Tag> com.cicada.player.compose <级别> <正文>`
  —— 标准 logcat `threadtime` 格式，带 pid/tid/包名；
- 第 1~3 行 `--------- beginning of crash / system / main`、第 4 行
  `---------------------------- PROCESS STARTED (32443) for package com.cicada.player.compose ---------`
  是 Android 运行时（`logcat -b` 拼接）写的分隔行，播放器自己不会写这种行；
- 播放器自己的日志全部带 `[0.9] [模块名] :` 前缀（`AliFrameWork` tag），
  例如 `[0.9] [ApsaraPlayerService] :…`、`[0.9] [AaudioRender] :…`；
- 大量平台侧 tag 混在中间（`MediaCodec`、`ACodec`、`AAudio`、`AudioTrack`、`GLRender`、
  `OpenGLRenderer`、`Hwaps`…），这是 logcat 的特征；
- 因此 **`AF_LOGD` 级别的框架日志不在里面**（默认 log 级别过滤掉了）。
  这直接决定了两件事：① 逐帧/逐包级别的诊断（例如 `TIMEPOS RenderVideo :…`）看不到；
  ② 要定位"音频帧 vs 视频滤波器的调用顺序"这类一帧级竞态，只能靠**已有的 INFO/WARN 行**做时序推理（见 §4.1）。

### 2.1 第 1 段（本地文件）时间线

```
12:14:47.155 [seekdiag] loop: seekFlag=0 … master=0 us vDecValid=0                 (:89)
12:14:47.158 mPtsDiscontinueDelta = 20000000                                       (:90)
12:14:47.154 AudioStreamBuilder  rate = 44100, channels = 2, format = 1            (:154)
12:14:47.191 [aaudio] stream opened: rate=44100 channels=2 bufferCapacityFrames=2205 ringBytes=65536 (:191)
12:14:47.242 video pts axis refreshed from the packet: stream=0 rawPts=0 timePosition=0 offset=0   (:148)
12:14:47.373 Player NotifyFirstFrame                                               (:207)
12:14:47.592 TIMEPOS reSync time 264146 to  40000                                  (:218)  ← 起播第一次对齐
────────── 起播正常播放，12:14:47.6 → 12:14:51.4 无异常日志 ──────────
12:14:51.460 PFR: seek posUs=557028000 inCache=0 status=5                          (:232)
12:14:51.463 FlushVideoPath from ProcessSeekToMsg (… seekFlag=1 seekNeedCatch=1)   (:233)
12:14:51.492 video pts axis refreshed … rawPts=555000000 timePosition=555000000 offset=0  (:254)
12:14:51.492 seek decode starts at a keyframe: pts=555000000 flags=1               (:255)
12:14:51.492 seek: the landing keyframe is 2028 ms before the target …             (:256)
12:14:51.492 seek audio aligned to the TARGET point: landing=555000000 target=557028000 dropped=44 (:257)
12:14:51.521 drop frame,master played time is 557028460,video pts is 555000000    (:266)  ← 目标之前的帧被丢（正确）
   … 约 50 条 drop frame，pts 从 555000000 一步步涨到 556960000 …
12:14:51.815 seek landing frame accepted: pts=557000000, offsetFromTarget=-28 ms …  (:319)  ← 视频落点：-28 ms，很准
12:14:51.843 TIMEPOS reSync time 557028001 to  557368000                           (:326)  ★ 主时钟被拽到目标+340 ms
12:14:51.843 audio first frame after seek: pts=555885714 target=557028000
             afterSeekMs=406 master=557368128 audioBase=557028000 consumed=340000  (:328)  ★★ 音频首帧比目标早 1142 ms
12:14:51.870 correct audio and master clock offset is 766275, frameDuration :23219 (:331)  ★ offset 应为 0
12:14:51.870 audio clock drift: correction deltaTime=300000 us (capped at +-300000),
             startTime=555885714, aoutPos=380000, lastFrameDuration=23219, audio pts=557046712 (:332) ★ 顶到限幅
12:14:52.011~52.561 [GLRender] drop a frame pts = 557200000 … 558000000           (:333-338) ← 视频被判"迟到"
```

**8 次普通 seek 的音频首帧 vs 目标点**（`audio first frame after seek`，按时间顺序，原样摘录关键数值）：

| 日志行 | 时间 | 目标 `target` | 音频首帧 `pts` | **`pts − target`** | `audioBase` | `consumed` | `master` | `master-base` |
|---|---|---|---|---|---|---|---|---|
| :328 | 12:14:51 | 557 028 000 | 555 885 714 | **−1 142 286 µs** | 557 028 000 | 340 000 | 557 368 128 | +340 ms |
| :485 | 12:14:55 | 928 930 000 | 926 731 610 | **−2 198 390 µs** | 928 930 000 | 660 000 | 929 590 096 | +660 ms |
| :533 | 12:15:00 | 1 170 254 000 | 1 170 262 494 | **+8 494 µs** | 1 170 254 000 | 20 000 | 1 170 273 074 | +20 ms |
| :662 | 12:15:12 | 1 327 279 000 | 1 325 859 410 | **−1 419 590 µs** | 1 327 279 000 | 320 000 | 1 327 599 142 | +320 ms |
| :868 | 12:15:37 | 1 474 388 000 | 1 471 285 986 | **−3 102 014 µs** | 1 474 388 000 | 620 000 | 1 475 008 112 | +620 ms |
| :971 | 12:15:40 | 816 533 000 | 815 763 447 | **−769 553 µs** | 816 533 000 | 320 000 | 816 853 104 | +320 ms |
| :1083 | 12:15:49 | 581 821 000 | 580 777 506 | **−1 043 494 µs** | 581 821 000 | 360 000 | 582 181 138 | +360 ms |
| :1278 | 12:16:04 | 414 878 000 | 410 853 878 | **−4 024 122 µs** | 414 878 000 | 700 000 | 415 578 102 | +700 ms |

**同一段里「视频落点」是准的**（对比项，说明问题只在音频）：

| 日志行 | `seek landing frame accepted` 的 `offsetFromTarget` |
|---|---|
| :319 | -28 ms |
| :476 | -10 ms |
| :546 | -14 ms |
| :653 | -39 ms |

结论：**视频落点误差 10~39 ms（帧间隔 40 ms，本来就是"包含目标的那一帧"能做到的极限），
音频落点误差 -177 ms ~ -4024 ms，且 8 次里 7 次是音频"落在目标之前"。**

### 2.2 `master == audioBase + consumed` 的算术前缀（后面反复要用）

日志 :328 一次把三个量打在同一行上：`audioBase=557028000`、`consumed=340000`、`master=557368128`，
而 `557028000 + 340000 = 557368000`（与 `master` 差 128 µs，是三次读的间隙）。§3.3 会用 4 组
独立读数验证这条恒等式。

### 2.3 第 2 段（DASH）时间线要点

```
12:18:23.657 [dash] 清单里有 6 条视频档、共 7 条流（最低档 id=5）                    (:1554)
12:18:23.753 video pts axis refreshed … stream=5 rawPts=21000 timePosition=0 offset=-21000 (:1635)
12:18:24.858 ABR switch up: bitrate 943688 -> 2444958 (index 4, stream 3)           (:1690)
12:18:25.082 finishQualitySwitch ready=1 stream=3 pts=INT64_MIN master=1014748      (:1795)  ← 切档成功
12:18:27.737 [seek] dash video: reqUs=48808000 -> segNum=8 curSegNum=9 …            (:1840)
12:18:27.737 [seek] dash audio/other: reqUs=48048000 -> segNum=8 curSegNum=9 …      (:1843)  ★ 音频请求比视频早 760 ms
12:18:27.798 [seekLanding] DASH first packet after the seek: pos=48048000 key=1 target=48808000 (:1870)
12:18:27.800 [seekLanding] DASH RELEASE: landing=48048000 target=48808000 (760 ms before the target) (:1872)
12:18:28.004 video pts axis refreshed … stream=3 rawPts=48069000 timePosition=48048000 offset=-21000 (:1885)
12:18:28.014 audio first frame after seek: pts=48810667 target=48808000
             afterSeekMs=285 master=48825008 audioBase=48808000 consumed=3755000  (:1890)  ★ 音频首帧 +3 ms
```
后面 5 次 seek 的 `reqUs` 对（`:2005/:2008`、`:2125/:2128`、`:2389/:2392`、`:2473/:2476`、
`:2473…:2523`）**方向一律是音频更早**，量值各不相同（-369 / -760 / -1 932 / -2 997 / -5 176 ms），
逐条列在 §4.4。

---

## 3. 判定用的读数：主时钟是什么、音频参考是什么

### 3.1 【已证实】主时钟 = `SystemReferClock`，音频参考就是它的参考值

- `SuperMediaPlayer.cpp:4834`：`mMasterClock.setReferenceClock(getAudioPlayTimeStampCB, this);`
  （在 `RenderAudio()` 的"每 seek 后第一帧音频"分支里，`mPlayedAudioPts == INT64_MIN`）；
- `SuperMediaPlayer.cpp:547`：`return mMasterClock.GetTime();`（`getCurrentPosition()`）；
- `framework` 侧：`mediaPlayer/system_refer_clock.cpp:12-62` 的 `SystemReferClock::GetTime()`
  —— 只要 `mGetClock` 存在且时钟未暂停，就拿参考时间和本地时钟比，
  **差值 > `CLOCK_DEVIATION_TIME_US`(100 ms) 就把本地时钟 set 成参考时间并打一行
  `TIMEPOS reSync time <旧> to  <参考>`**（:44-48）。

所以：**日志里每一条 `TIMEPOS reSync` 都是"主时钟被音频参考拽了一次"的记录**，
`to` 那个值就是那一刻音频参考的读数。

### 3.2 【已证实】音频参考 = 目标点 + 设备已消费量

`SuperMediaPlayer::getAudioPlayTimeStamp()`（`SuperMediaPlayer.cpp:6974` 起，本地文件里
是 `:6933-6990` 那一段）：

```cpp
const int64_t baseUs     = mDiscontinuity.audioBaseUs.load();          // seek 时 = targetUs
const int64_t consumedUs = mAVDeviceManager->getAudioRenderPosition();  // 设备"已消费"
const int64_t delta      = consumedUs - mDiscontinuity.audioBaseConsumedUs.load();
return baseUs + delta;                                                  // ← 这就是主时钟的参考值
```
`FLUSH`/`SEEK` 时基准的重钉点在 `FlushAudioPath()`（`SuperMediaPlayer.cpp:6290`：
`pinAudioClockBase(mDiscontinuity.targetUs)`），真正写字段的是 `pinAudioClockBase()`
（`:6380` 一带，`audioBaseUs = baseUs; audioBaseConsumedUs = 当时读到的 consumed`）。

**这条模型的前提被代码自己写在 `RenderAudio()` 的落点丢弃判据上方**（`SuperMediaPlayer.cpp:4568-4575`）：

> 「若把这些帧推给设备，时钟就会比内容**超前**（目标点 − 帧 pts），
>  视频随即被判"迟到"而全丢 —— 那正是以前"音频对齐到视频落点、时钟钉在目标点"造成的错位。」

**本日志的失败模式就是这条前提被打破**：音频首帧落在目标点之前，而闸门已经不在。

### 3.3 【已证实】4 组独立读数验证 `master = audioBase + consumed`

| 日志行 | 段 | `audioBase` | `consumed` | 相加 | 日志 `master` | `master − (base+consumed)` |
|---|---|---|---|---|---|---|
| :328 | 本地文件 | 557 028 000 | 340 000 | 557 368 000 | 557 368 128 | **同一次读，恒等式成立** |
| :533 | 本地文件 | 1 170 254 000 | 20 000 | 1 170 274 000 | 1 170 273 074 | **-926 µs**（= 两次读之间时钟的正常推进量） |
| :1890 | DASH | 48 808 000 | 3 755 000 | 52 563 000 | 48 825 008 | **-3 737 992 = −consumed + 17 008** |
| :2056 | DASH | 93 087 000 | 15 730 000 | 108 817 000 | 93 107 809 | **-15 709 191 = −consumed + 20 809** |
| :2175 | DASH | 128 058 000 | 23 725 000 | 151 783 000 | 128 076 119 | **-23 706 881 = −consumed + 18 119** |
| :2441 | DASH | 173 344 000 | 42 640 000 | 215 984 000 | 173 369 160 | **-42 614 840 = −consumed + 25 160** |

**结论（重要）**：
- 第 1~2 组（**本地文件**）**逐微秒成立**，证明 §3.2 的公式就是当前实现，
  而且 `audioBase + consumed` 就是主时钟的参考值；
- 第 3~6 组（**DASH 段**）**不成立**，差值恰好是 `−consumed` 本身 ⇒ 说明那段里
  `audioBaseConsumedUs` 是**此刻才被惰性补锚**成当前 `consumed` 的
  （`getAudioPlayTimeStamp()` 的 `baseConsumedUs == INT64_MIN → 补锚并返回 INT64_MIN` 那条，
  `SuperMediaPlayer.cpp:7005-7011`），所以**那一刻参考还没交出去**，主时钟读的是
  `ProcessSeekToMsg()` / 落点采纳时钉的值（`mMasterClock.setTime(targetUs)`）；
- 这也顺带证明：**DASH 段里 `consumed` 是单调累加的绝对值（3.755 s → 3.755/15.73/23.725/42.64/45.20 s），
  设备 flush 之后它并没有回到 0** —— 与 AAudio 侧"flush 时若拿不到时间戳就**保留旧基线**"的写法
  一致（`AaudioRender.cpp:691-703` 的注释与实现）。
  ⇒ 这条是 §6.3 那个"seek 后漂移"待验证项的判据来源。

---

## 4. 根因

### 4.1 【已证实】根因 1：音频的落点闸门与视频**共用**，且被视频提前关掉

代码（`mediaPlayer/SuperMediaPlayer.cpp`，改动前行号）：

- 闸门的唯一定义：`SuperMediaPlayer.h:153` `std::atomic<bool> filterActive{false};`
- 臂上：`beginDiscontinuity()` → `:8495` `mDiscontinuity.filterActive = (targetUs != INT64_MIN);`
- 关闭（**唯一写点**）：`acceptDiscontinuityLandingFrame()` → `:8598` `mDiscontinuity.filterActive = false;`
  —— 而它的调用者是 `shouldDropForDiscontinuity()`（`:8580`），只在 **`RenderVideo()`** 里被调用；
- 音频侧读这个闸门：`RenderAudio()` → `:4589` `if (mDiscontinuity.filterActive.load()) { … }`
  里面就是"落在目标点之前的音频帧一律不推给设备"（`:4596-4609`）。

调用顺序（`SuperMediaPlayer::render()`，`SuperMediaPlayer.cpp:4473` 与 `:4502`）：

```cpp
if (mCurrentAudioIndex >= 0) { … RenderAudio(); … }   // 先音频
if (HAVE_VIDEO) { videoRendered = RenderVideo(!mFirstRendered); }   // 后视频
```

于是存在两种都在本日志里成立的失序：

1. **同一次 `render()` 之内**：视频落点帧在这一轮的 `RenderVideo()` 里被采纳 ⇒ `filterActive=false`；
   而那一帧音频要等到**下一轮** `render()` 的 `RenderAudio()` 才被处理 —— 闸门已经没了。
2. **跨线程**：音频帧队列的填充（解码）与 `RenderAudio()` 的消费都在内核工作线程，
   但**设备位置/时钟的读取来自渲染线程与音频回调线程**（日志里 `[GLRender] drop a frame`
   的 tid 是 *1817 / *32726，与 `ApsaraPlayerService` 的 *1809 / *32723 不同）。

**本日志怎么证实的**：用"视频落点被采纳"和"音频首帧"两条 INFO 行的**时间差**。
本地文件 8 次 seek，视频落点采纳（`seek landing frame accepted`）与音频首帧
（`audio first frame after seek`）的间隔：

| seek | `seek landing frame accepted` | `audio first frame after seek` | 间隔 |
|---|---|---|---|
| 12:14:51 | .815 | .843 | **28 ms** |
| 12:14:55 | .946 | .974 | **28 ms** |
| 12:15:00 | .872 | .780（音频先） | -92 ms |
| 12:15:12 | .865 | .893 | **28 ms** |
| 12:15:37 | （无 accepted 日志） | .767 | — |
| 12:15:41 | （无 accepted 日志） | .018 | — |

间隔只有 **28 ms**（几乎是同一次 `render()` 的前后两轮）时，音频首帧却是
**-1142 ms / -2198 ms / -1420 ms**。这与"闸门被视频关掉后音频帧才被处理"完全吻合；
反过来，唯一那条 `+8 ms`（12:15:00，`afterSeekMs` 只有 39 ms，音频**先于**视频落点 92 ms 到达）
说明：**只要音频先到，闸门还在，它就会被正确地丢到目标点**（`+8 ms` 是 AAC 帧量化）。

⇒ 这就解释了"为什么同一台机器上有时差 1.1 s、有时 2.2 s、有时只差 8 ms"：
**它是一场 race，输赢取决于这一轮 `render()` 里谁先跑完。**

### 4.2 【已证实】根因 2：音频时间轴的建立点取的是"首帧自己的 pts"

`RenderAudio()` 的 `mPlayedAudioPts == INT64_MIN` 分支（原 `SuperMediaPlayer.cpp:4688-4691`）：

```cpp
if (mPlayedAudioPts == INT64_MIN) {
    mAudioTime.startTime   = pts;      // ← 音频时间轴的起点 = 这一帧自己的 pts
    mAudioTime.deltaTime   = 0;
    mAudioTime.deltaTimeTmp = 0;
    …
    if (mDiscontinuity.audioBaseUs.load() == INT64_MIN) { pinAudioClockBase(pts); }   // 这里用的是 targetUs，不是 pts
```

这个分支每个 seek 只走一次（因为 `FlushAudioPath()` 把 `mPlayedAudioPts` 清成 `INT64_MIN`，
`SuperMediaPlayer.cpp:6313`）。**只要首帧不是落点帧，整条音频记账就从窗口外起算**，
后果在日志里逐条可见：

```
:328 audio first frame after seek: pts=555885714 target=557028000 … audioBase=557028000
:331 correct audio and master clock offset is 766275, frameDuration :23219     ← 下一帧算出的 offset
:332 audio clock drift: correction deltaTime=300000 us (capped at +-300000)    ← 一次性顶到限幅
:489 audio timeline re-anchored (offset 1811173 > 1s): audio pts=928937506 …
:665 audio timeline re-anchored (offset 1044915 > 1s): audio pts=1327299048 …
:874 audio timeline re-anchored (offset 2716751 > 1s): audio pts=1474397460 …
:1283 audio timeline re-anchored (offset 3645549 > 1s): audio pts=414894150 …
:974 correct audio and master clock offset is 394755, frameDuration :23219
:1086 correct audio and master clock offset is 650175, frameDuration :23219
```

offset 的正常值应该是 **0（或一个帧间隔的抖动）**：`23219 µs` 是 44100 Hz 下 1024 采样的
AAC 帧长，而 `pts` 步进也正是 23219 µs（`555885714 / 23219 = 23941.0` 整除）。
日志里的 766275 / 1811173 / 1044915 / 2716751 / 3645549 全部是"起点错了"留下的固定偏移。

**两条相关代码事实（都读过，写在这里防止误改）**：
- `mAudioTime.deltaTime` / `deltaTimeTmp` 这套纠偏累加器**当前没有任何读者**：
  `getAudioPlayTimeStamp()` 用的是 `audioBaseUs + (consumed - baseConsumed)`，
  不是注释里（`:4827`、`SuperMediaPlayer.h:1231`）写的
  `mAudioTime.startTime + mAudioTime.deltaTime + aoutPos`。⇒ **纠偏累加器现在是死代码**，
  §4.2 的危害不是"deltaTime 把时钟拉偏"，而是"`mPlayedAudioPts` / `mAudioTime.startTime`
  这个参照点从窗口外起算"（日志 :489/:665/:874/:1283 的 re-anchor 分支只 `mPlayedAudioPts = pts`、
  不动时钟，所以这个错误起点会一直留着）。
- `FlushAudioPath()` 已经会把 `mLastAudioFrameDuration` 之外的那套（`mPlayedAudioPts`、
  `mAudioTime.*`）全清掉（`:6313-6317`），所以**"首帧 offset 应为 0"是完全可实现的**。

### 4.3 【已证实】`mPtsDiscontinueDelta` 不是本次的原因（按本文证据重新判定）

- 本地文件：`mPtsDiscontinueDelta = 20000000`（:90，= 20 s）；
  DASH：`mPtsDiscontinueDelta = 6000000`（:1575，= 6 s）。
- 它的算法在 `SuperMediaPlayer.cpp:5896-5903` 一带（`mPtsDiscontinueDelta == INT64_MIN` 时
  取 `getMaxGopTimeUs()`，否则退回 `PTS_DISCONTINUE_DELTA`）。
- 它只有 3 个消费者：`:4739`（音频 pts 倒回判定）、`:4945`（视频 pts 倒回判定）、
  `:4978/:4987`（`PTS_REVERTING` 那一组等待判据）。
- **本日志里一次 `PTS_REVERTING` 都没有**（全文搜索 0 条），所以**这 20 s / 6 s 在本次完全没有参与**
  —— 既有分析里"本地文件为什么会有 20 s"这个疑问对本次现象**不是**根因。
  （仍建议单独复查 `getMaxGopTimeUs()` 对本地文件的返回值，但那不是本次 desync 的原因。）

### 4.4 【已证实 · 已修（代码级）】根因 3（DASH 与 HLS 都有）：音频和视频的 seek **目标点本身**就不一样

- `DashManager::seek()` 的设计是"2. 先 seek 视频、取回 `seekedUs`；3. 用 `seekedUs` seek 其余流"
  （`framework/demuxer/dash/DashManager.cpp` 改前 `:481-512`，代码原文；`HLSManager::seek()` 改前
  `:497-535` 与之同形）；
- `DashStream::seek()` 的返回值已经被改成**分片起点**（`framework/demuxer/dash/DashStream.cpp:1246-1256`
  `landingUs = mPTracker->getCurrentRepresentation()->getMediaSegmentStartTime(num);`，
  `:1381 return landingUs;`）；
- 日志逐条对上（视频 `reqUs` 与音频 `reqUs` 的差**方向恒为音频更早，量值各不相同**）：

| 视频请求 | 音频请求 | 差（音频 − 视频） |
|---|---|---|
| :1840 `reqUs=48808000` | :1843 `reqUs=48048000` | **-760 000 µs** |
| :2005 `reqUs=93087000` | :2008 `reqUs=90090000` | **-2 997 000 µs** |
| :2125 `reqUs=128058000` | :2128 `reqUs=126126000` | **-1 932 000 µs** |
| :2389 `reqUs=173344000` | :2392 `reqUs=168168000` | **-5 176 000 µs** |
| :2473 `reqUs=210579000` | :2476 `reqUs=210210000` | **-369 000 µs** |

  ⇒ 差不是常数，而是"视频分片起点相对**用户请求点**的回退量"：请求刚过分片头时回退很小
  （-369 ms / -760 ms），请求接近分片尾时回退很大（-2 997 ms / -5 176 ms）。
  也就是说 **`getMediaSegmentStartTime(num)` 把音频的读位置拽回了视频所在分片的**起点**，
  而这个起点可能比用户目标早大半个分片。**方向恒为音频更早**，这一点是结构性的、
  与具体数值无关。
  （**措辞勘误**：这里说的是音频的**读起点更早**，不要读成"音频内容超前" —— 读起点更早意味着
   音频要**多解一段前缀**、seek 收敛更慢；真正的相位由播放器侧的落点地板按 `targetUs` 决定，
   见本节尾注。）

- 而 `SeekLandingStage` 只给**视频**装了延迟线，音频明确不装：
  `[seekLanding] DASH not armed: reqUs=… stream=1 (only the VOD video path stages the landing;
   audio has no GOP semantics, MIXED would drop audio together with the prefix)`（:1848/:2011/:2131/:2394）。
- 结果（5 次全都同向）：**音频首帧（真正上屏的那一帧）比视频落点晚 371 ~ 1 938 ms**：

| seek | 视频落点（`seekLanding RELEASE`） | 音频首帧 | 音频 − 视频落点 |
|---|---|---|---|
| :1872 / :1890 | 48 048 000 | 48 810 667 | **+762 667 µs** |
| :2038 / :2056 | 92 092 000 | 93 098 667 | **+1 006 667 µs** |
| :2157 / :2175 | 126 126 000 | 128 064 000 | **+1 938 000 µs** |
| :2423 / :2441 | 172 172 000 | 173 354 667 | **+1 182 667 µs** |
| :2505 / :2523 | 210 210 000 | 210 581 333 | **+371 333 µs** |

  （**口径勘误（重要）**：这两列不是同一种量 —— 左列是**视频的落点关键帧**（解码起点，
  `seekLanding RELEASE`），右列是**音频的第一个上屏帧**（`audio first frame after seek`，已经被
  播放器侧的音频落点地板按 `targetUs` 裁过）。于是"落点差"≈（目标 − 视频落点）+ 音频帧量化，
  它**不是** A/V 相位差的直接证据：表里 +762.667 ms ≈ 760 ms（请求差）+ 2.667 ms（AAC 帧量化）
  正好对上这一点。相位差的直接读数是 `audio first frame after seek` 里的 `pts − target`，
  它在 DASH 段是 **+3 ~ +11 ms**（§5.2 自己也写了），不是几百毫秒。
  换句话说：本节确证的是"**音频被喂了另一个流（视频）的分片起点**"这个结构缺陷，
  它按结构让音频的读起点偏早（多解前缀、seek 更慢、并把"两路同一个目标点"这条前提破坏掉）；
  而"音频落点比视频落点晚若干百毫秒"只是"音频上屏帧 vs 视频解码起点"这两种读数之差。）

⇒ **DASH 上"音频与视频的 seek 目标点不一致"是实锤**：音频被喂的是**另一个流（视频）的分片起点**，
   方向恒为音频更早。

⇒ **HLS 也有同一条缺陷（本节原来的对照结论是错的）**：本文原来说"HLS 侧 `HLSStream::seek()` 末尾
   返回的是**请求值**（`:1763 return usSought;`），所以没有这条缺陷"—— 事实是 `usSought` 已经被
   `SegmentTracker::getSegmentNumberByTime` 一路传到
   `play_list/SegmentList.cpp:114-120`，在那里被**就地改写**成该片的 `startTime`
   （`time = i->startTime;`，形参是 `uint64_t &time`）。所以 HLS 的返回值同样是"HLS 分片起点"，
   `HLSManager::seek()` 改前的 `us = seekedUs`（`:505-507`）与 DASH 完全同形 ——
   DASH 与 HLS 是**同一个缺陷的两个实例**，"DASH 特有"不成立。
   一个可用的旁证：HLS 的 `[seek] hls …: reqUs=%lld` 打的就是这个被改写过的 `usSought`
   （`HLSStream.cpp:1697-1704`），所以**在 HLS 上不能拿 `reqUs` 两行相等当验收判据**
   （见 §8.1 里 P0-B 的那四条判据）。

这与既有 `ANALYSIS-HLS-VS-DASH.md` 里"HLS 两轴一致、DASH 两轴错位"的结论**不同层**：
那一条讲的是 `timePosition` 与 pts 轴；这一条讲的是 **manager 喂给音频的 seek 目标本身**。

**【已修（代码级），改动与理由见 §7.4】**`DashManager::seek()` 与 `HLSManager::seek()` 的第 3 步
不再拿视频 `seek()` 的返回值当其余每一路的目标，改成用**用户请求值**；两个 `Stream::seek()` 的
返回值语义（分片起点）**一字未改**（它另有消费者，审计见 §7.4）。修复后仍待运行时验证的判据
集中在 §8.1。

### 4.5 【推测·待验证】「播放途中慢慢漂」与 `TIMEPOS reSync` 高频簇

日志里两段 `TIMEPOS reSync` 是按 1 行/109 ms 的频率连续出现的（各约 60 行）：

```
12:18:33.129→12:18:33.784   reSync … 53803000 → 54558000   …  （约 7 行/700 ms）
12:18:45.561→12:18:48.375   reSync … 98042000 → 100627000  …  （约 25 行/2.8 s）
12:18:53.751→12:19:03.980   reSync … 133058000 → 143148000 …  （约 120 行/10 s）
```
- 行间距固定 ≈ **109 ms**，`to` 每次步进 ≈ **196~200 ms**，`from` 步进 ≈ 102 ms；
- 也就是说：**主时钟的本地值是"每 100 ms 走 100 ms"的正常速率，而音频参考是"每 100 ms 跳 ~200 ms"**，
  于是差值反复越过 100 ms 门限、反复 reSync；
- 这一段是 **DASH 段**，而且**同一时间窗里没有一条 `audio first frame after seek` / `correct audio …` /
  `audio clock drift …`**（§2.1 那些诊断行只在"每 seek 后第一帧"打一次），所以**看不出**
  是 `consumed` 在跳还是参考在跳；
- 净漂移：取 12:18:45.561 → 12:18:48.375（2.814 s 墙钟），目标时间 98 042 000 → 100 627 000
  = 2.585 s ⇒ 比值 **0.92**（没有单向净漂移）。⇒ **本会话这段不能证明"越播越偏"**，
  只能证明**主时钟在这段时间里被音频参考反复小幅拽动（±100~200 ms）**。

**判据（下次日志一眼判定）**：在 `TIMEPOS reSync` 簇期间每秒打一行
`audio ref: base=… consumed=… ref=… local=… videoPts=…`（`consumed = getAudioRenderPosition()`）
—— 如果 `consumed` 的**增量**明显大于墙钟增量，就是设备位置（AAudio `getTimestamp` 的
`presented`）在跳；如果 `consumed` 正常而 `base` 在变，就是 `pinAudioClockBase` 被反复调用。

---

## 5. DASH 与 HLS 分别判定

### 5.1 判定表

| 维度 | 本地文件（第 1 段，实测） | **DASH**（第 2 段，实测 + 代码） | **HLS**（代码判定，本次日志无 HLS） |
|---|---|---|---|
| seek 请求给谁 | 一次 `mDemuxerService->Seek(seekPos,0,-1)`，单容器内所有流同一个请求（`SMPMessageControllerListener.cpp:1115`） | 同一个入口，但 `DashManager::seek()` 会**视频先 seek，再拿视频的返回值去 seek 其余每一路**（改前 `DashManager.cpp:481-512`；**【已修，见 §7.4】**） | 同 DASH 结构（`HLSManager::seek()` 改前 `:497-535` 同形；**【已按同一口径一起修，见 §7.4】**） |
| 音频/视频的**目标点**是否一致 | **一致**（同一个 `seekPos`） | 改前**不一致**：音频目标 = 视频的**分片起点**，实测差 369~5176 ms，方向恒为音频更早（§4.4）；**已修 ⇒ 现在两路都是同一个请求值** | 代码：`HLSStream::seek()` 末尾是 `:1763 return usSought;`，但 `usSought` 已被 `SegmentList::getSegmentNumberByTime` **就地改写成该片 startTime** ⇒ 改前音频拿到的同样是**分片起点**（与 DASH 同一个缺陷，见 §4.4 尾注）；**已修** |
| 每路自己的落点语义 | 视频落到"目标前的关键帧"（`seek audio aligned` 日志给的是同一个关键帧；实测 2028/3930/2279/4388 ms 之前） | 视频由 `SeekLandingStage` 收敛到"分片内最后一个 ≤ 目标的（更近的）关键帧"（`:2037/:2421/:2422`）；音频不装延迟线，落到音频分片内第一个包，再由播放器侧地板按 `targetUs` 裁到目标点 | HLS 每路各自 `SegmentTracker` 按自己的分片网格落点；两轴一致（`timePosition ≡ pts`，见既有分析差异 1） |
| **本次是否复现 A/V 不同步** | **复现，且最严重**（音频首帧 −1142 ~ −4024 ms，7/8 次在目标之前） | **复现**：音频首帧与**目标点**只差 +3~11 ms（相位是对的），但**音频被喂的目标是视频的分片起点** ⇒ 读起点结构性地比请求值早 369~5176 ms（多解前缀、seek 收敛更慢，并破坏"两路同一个目标点"这条前提）。原表"音频落点比视频落点超前 371 667 ~ 1 938 000 µs"是"音频上屏帧 vs 视频解码起点"两种读数之差，见 §4.4 口径勘误 | **【推测】结构上同样有根因 1/2 的缺陷**（音频闸门共用、时间轴建立点取首帧 pts，两处都是协议无关的内核代码） |
| 该条管线**独有**的修复 | 无（本地文件只是请求值一致） | **【已修】**`DashManager::seek()` 不再把视频的分片起点当其余每一路的目标（§7.4） | **【已修，同一口径】**`HLSManager::seek()` 同样不再把视频的分片起点当其余每一路的目标（§7.4）；"音频读位置落在自己的分片网格"两边都靠"传请求值"解决，落点精度仍由 §7 的地板兜住 |

### 5.2 逐条回答"两条管线目前有没有这个问题"

- **本地文件：有，而且最严重。** 直接证据 §2.1（7/8 次音频首帧在目标之前，最大 -4024 ms），
  根因是 §4.1 + §4.2（两处都在内核通用代码里，与协议无关）。
- **DASH：有。** 直接证据 §4.4（`reqUs` 差恒为负 ⇒ 音频被喂的是视频的分片起点）。
  它**多出一条**协议侧缺陷（`DashManager` 拿视频分片起点当音频目标），已按同一口径修正（§7.4）。
  **相位正确性并不靠这一条**：DASH 段里 `audio first frame after seek` 的 `pts` 与 `target`
  只差 **+3~11 ms**，因为主时钟钉在"用户目标点"、音频落点地板也按同一个 `targetUs` 裁前缀
  （`mSeekLanding` 用的是请求值 `us`，`DashStream.cpp:1356` 的注释明确写了"目标用请求值 us
  （不是 landingUs）"）。所以这次修的是**读位置**（前缀长度 / seek 收敛 / "两路拿到同一个目标"
  这条前提），不是"把几百毫秒的相位差抹掉" —— 那几百毫秒是两种读数的差，见 §4.4 口径勘误。
- **HLS：结构上有同一根因，而且和 DASH 是同一个缺陷（本节原来的推论是错的）。**
  本日志没有 HLS 数据，不能给"实测"结论；但代码上 `HLSStream::seek()` 的返回值同样是**分片起点**
  （`usSought` 被 `SegmentList::getSegmentNumberByTime` 就地改写成该片 `startTime`，见 §4.4 尾注），
  `HLSManager::seek()` 改前的 `us = seekedUs` 与 DASH 完全同形 ⇒ HLS 也把视频的分片起点喂给了音频。
  本次已按同一口径一起修（§7.4），根因 1/2 由 §7 修掉。
  ⇒ **DASH 与 HLS 现在处在同一条口径上**；两边都还要靠 §8.1 的运行时判据收口（HLS 另需 §8 的 P1 实测）。

---

## 6. 【用户补充要求】本轮 Android 音频输出走的是哪条路：AAudio 还是 AudioTrack

### 6.1 【已证实】本日志：走的是 **AAudio（C 接口）**，不是 Java AudioTrack

**正面证据（必须是播放器自己打的日志，不能只看系统 tag）：**

| 证据 | 日志行 | 说明 |
|---|---|---|
| 渲染器实体打日志 | `[0.9] [AaudioRender] :[aaudio] output=aaudio stream opened: performanceMode=12 rate=44100 channels=2 bufferCapacityFrames=2205 ringBytes=65536 (S16, shared, target ~50ms buffer)` | 第 1 段 :191 |
| 同上（第 2 段） | `[0.9] [AaudioRender] :[aaudio] output=aaudio stream opened: performanceMode=12 rate=48000 channels=2 bufferCapacityFrames=2400 ringBytes=65536 …` | 第 2 段 :1667 |
| 只属于 AAudio 路径的日志 | `[0.9] [AaudioRender] :[aaudio] requestFlush returned -895 (state transition is asynchronous; the position base is re-pinned below anyway)` | :245 / :354 / :519 / :2160（**每次 seek 一条**） |
| 只属于 AAudio 路径的日志 | `[0.9] [AaudioRender] :[aaudio] data callback underrun: the ring had 0 of 2940/588/1176 bytes — filling the rest with silence. … AudioTrack keep-alive machinery is not needed on this path` | :200 / :251 / :359 / :2167（**这句话本身就是在对比 AudioTrack**） |
| 平台侧印证 | `AudioStreamBuilder … rate = 44100, channels = 2, format = 1, sharing = SH, dir = OUTPUT`（:154）、`AAudioStream setState(s#2) …`、`AAudioStream_requestPause/Flush/Start(s#2)`、`AudioSystem reportHalLatency sessionId 30377` | AAudio 流的原生日志 |

**反证（另一条没被走）：**

| 应该出现的行 | 本日志 |
|---|---|
| tag `AudioTrackRender`（`framework/render/audio/Android/AudioTrackRender.cpp:4` `#define LOG_TAG "AudioTrackRender"`） | **0 条**（全文搜索无 `AudioTrackRender`） |
| `[aaudio] not used on this device (API level=… < 27) ⇒ fall back to AudioTrack`（`AaudioRender.cpp:296`） | **0 条** |
| `[aaudio] libaaudio unavailable ⇒ fall back to AudioTrack`（`AaudioRender.cpp:194/251`） | **0 条** |
| `[aaudio] not used any more in this process (…)`（sticky 自降级，`AaudioRender.cpp:308`） | **0 条** |
| Java 侧 `AudioTrack` 的用法特征（`getPlaybackHeadPosition` 的 JNI 调用、`AudioTrackRender::write_loop` 的保活静音） | 日志里出现的 `AudioTrack pause(7908)/start(7908)` 是 **AAudio 内部实现**在 AudioFlinger 上打的 tag（AAudio 在共享模式下就是建一条 AudioTrack），不能作为"走了 Java AudioTrack 渲染器"的证据 |

⇒ **结论：本日志走 AAudio。** 并且它是"能力判定"选出来的，没有开关可以切。

### 6.2 【已证实】代码里的选择规则（file:line）

```
framework/render/renderFactory.cpp:89-111   AudioRenderFactory::create()
        └─ audioRenderPrototype::create(AF_CODEC_ID_NONE)
             framework/render/audio/audioRenderPrototype.cpp:15-24
             取"注册顺序里第一个 is_supported() 为真的实现"，clone() 出来

注册（两个静态对象，各自 .cpp 构造时 addPrototype）：
        framework/render/audio/Android/AaudioRender.cpp:321   AaudioRender  AaudioRender::se(0);
        framework/render/audio/Android/AudioTrackRender.cpp:38 AudioTrackRender AudioTrackRender::se(0);

"AAudio 优先"靠**能力互斥**表达，与注册顺序无关：
        framework/render/audio/Android/AudioTrackRender.h:79-83
            bool is_supported(AFCodecID) override { return !AaudioRender::isAvailable(); }
        framework/render/audio/Android/AaudioRender.h:126
            bool is_supported(AFCodecID) override { return isAvailable(); }
```

`AaudioRender::isAvailable()`（`AaudioRender.cpp:279-316`）的**三个必需条件**，
任意一条不成立就回退 AudioTrack：

1. `libaaudio.so` 能 `dlopen` 且必需符号全部解析（`loadApi()`，:184-266；失败打
   `[aaudio] libaaudio unavailable ⇒ fall back to AudioTrack`）；
2. `android_get_device_api_level() >= AAUDIO_MIN_RECOMMENDED_API`，该常量 = **27**
   （`AaudioRender.cpp:60`，理由在文件头 :41-58：Google 不推荐 8.0 及以前用 AAudio）；
3. 本进程没有"AAudio 流真的打不开"的 sticky 记录
   （`s_streamOpenFailed`，`AaudioRender.cpp:73`；置位点在 :425/:476/:494，开流失败后打
   `… the AudioTrack path will be used from the next audio setup on`）。

**"什么条件下会切到另一条"**（一句话版）：
① API < 27；② 设备没有 AAudio 符号；③ 本进程曾经开 AAudio 流失败（此后本进程一直用 AudioTrack）。
本机三条都不成立 ⇒ AAudio。**没有系统属性 / 配置开关参与**（代码注释明确写了
"这不是配置开关，判据只是这台设备有没有这个能力"）。

### 6.3 两条路的**时间戳/时钟语义差异**，以及本次 desync 与所选路的关系

| 维度 | **AAudio**（本次走的路） | **AudioTrack（Java, JNI）** |
|---|---|---|
| 位置来源 | `AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &frames, &nanoTime)`（`AaudioRender.cpp:747-770` 的 `queryPresentedFrames()`），失败**返回 INT64_MIN，绝不用墙钟造时间** | `AudioTrack.getPlaybackHeadPosition()`（`AudioTrackRender.cpp:504-506`），再由 `device_get_position()` 换算（:484-486） |
| 单位/换算 | `(presented - mPresentedBaseFrames) * 1e6 / mOutSampleRate`（:803） | `(mOverFlowPlayedSimples + playedSimples - mAudioFlushPosition) / (sample_rate/1e6)`（:484-485） |
| **回绕** | 载体是 `int64_t`，无 32 位回绕问题 | `getPlaybackHeadPosition()` 返回 Java `int`（32 位**有符号**），日志里有 `mOverFlowPlayedSimples` 这样的累加字段来处理回绕 —— **这条差异是 AudioTrack 独有的风险点** |
| flush 后"从零起算" | `flush_device()` 里 `mPresentedBaseFrames = queryPresentedFrames()`；**拿不到就保留旧基线**（`AaudioRender.cpp:691-703`，注释写明这个选择），另有 `presented < base 就地重钉` 的自愈（:786-796） | `flush_device_inner()` 里用 `mAudioFlushPosition` 记账（`AudioTrackRender.cpp:349` 起） |
| 设备停止时 | 位置**照旧**按 `presented` 走（AAudio 回调在跑就一直在消费；欠载时回调补静音，`presented` 继续涨） | 位置在 `STOPPED/FLUSHED` 时返回 0（`getDevicePlayedSimples()` 只在 `PLAYING/PAUSED` 时读，`AudioTrackRender.cpp:503`）——代码注释 `:469-483` 明确记录了"曾改成返回哨兵导致第一次 seek 半天不动"的回退 |
| 单次采样粒度 | `getTimestamp` 的 `presented` 是**离散更新**的（本日志实测相邻两条 reSync 之间参考步进 ~200 ms，见 §4.5） | `getPlaybackHeadPosition` 也离散，但按 AAudio"SHARED + 回调周期"这条路的缓冲是 50 ms×N |

**本次 desync 与"走哪条路"的关系（明确回答）**：

- **根因 1/2 是内核代码，与音频渲染器实现无关**（`SuperMediaPlayer::RenderAudio()` /
  `beginDiscontinuity()` / `acceptDiscontinuityLandingFrame()` 没有平台分支，
  `mediaPlayer/` 与 `framework/` 非平台代码里也没有平台宏）⇒ **换成 AudioTrack 一样会中**。
- 但**所选这条路决定了"泄漏到设备里的陈旧音频能造成多大的相位差"**：
  AAudio 的"已消费量"是**硬件时间戳里的 presented 帧数**，设备一旦开始跑（哪怕在欠载补静音，
  日志 :251 `underrun … filling the rest with silence`），它就**不会停**；
  而 AudioTrack 那条路在 `STOPPED` 时返回 0。所以**同一条陈旧音频，在 AAudio 上更容易被
  算成"走在目标点之后"**。
- **判据（哪条更可能造成 seek 后漂移）**：看 `consumed`（`getAudioRenderPosition()`）在
  **seek 窗口内**有没有涨。本日志里它是涨的（本地文件那次 340 ms / seek 后 406 ms，
  见 §2.1 表格最后一列 `master-base`：340/660/20/320/620/320/360/700 ms，**8 次 seek
  有 7 次在 seek 窗口内就涨了 320~700 ms**）。**下一个要钉死的问题就是这 320~700 ms 是
  "真实播放"还是"欠载补静音"** —— 见 §8 的 P0 调查项。
- **本会话无法从日志区分**两种可能：① 设备真的在这段里播了 320~700 ms 音频（说明
  陈旧音频进了设备）；② 设备在欠载补静音、`presented` 照涨（说明是"消音也算消费"）。
  两种都指向同一个修法方向（音频参考不能把"没有内容的消费"算成内容推进），
  但**证据不同、修法不同**，所以要按 §8 补一行日志。
- **看不出是哪条路的情况不适用**：本节第 1 条已经从 tag + 独有日志 + 反证三个方面钉死了。
  如果以后要在一份**没有** `[AaudioRender]` tag 的日志上判定，需要补的日志是：
  `AudioRenderFactory::create()` 一行 `using audio render <实现名> (aaudioAvailable=%d apiLevel=%d)`
  —— 目前这个函数**只打了一条 `video render creator registered/cleared`**（`renderFactory.cpp:67`），
  **没有为音频渲染器打实现名**；这是唯一"看不到"的地方。

---

## 7. 已改代码（最小、确定性）

### 7.1 改动清单

| 文件 | 位置 | 改动 |
|---|---|---|
| `mediaPlayer/SuperMediaPlayer.h` | `struct Discontinuity` 内、`acceptedFramePos` 之后（新增成员） | 新增 `std::atomic<bool> audioLandingPending{false};` —— 音频**自己的**落点地板闸门，附完整"为什么"注释 |
| 同上 | 类成员列表**最末尾**（`mVideoLandingDropLoggedGen` 之后、`#ifdef ENABLE_CACHE_MODULE` 之前） | 新增 `bool mSeekAudioLandingReset{false};`（只追加，不动既有偏移）。**注意**：诊断限频用的 `mAudioLandingDropLoggedGen` 本来就已经存在（`:1540`），本次**复用它**，没有新增 |
| 同上 | `pinAudioClockBase()` 声明处（`:1519-1540`） | `mAudioLandingDropLoggedGen` 的注释由"音频落点**过滤**"更新为"音频落点**地板**"（同一个成员、同一语义，只是它现在真的会被触发） |
| `mediaPlayer/SuperMediaPlayer.cpp` | `beginDiscontinuity()` 内、`mDiscontinuity.acceptedFramePos = INT64_MIN;` 之后 | 消费 `mSeekAudioLandingReset` 一次，臂上/清掉 `audioLandingPending`，并复位限频闩 |
| 同上 | `SeekTo()` 内、那次 `beginDiscontinuity(seekTargetUs)` **之前**（`:782-799`） | `mSeekAudioLandingReset = true;` —— 声明"本次 seek 会把音频时间轴重锚到 seekTargetUs" |
| `mediaPlayer/SMPMessageControllerListener.cpp` | `ProcessSeekToMsg()` 内、`mPlayer.beginDiscontinuity(seekPos)` **之前**（`:960`） | 同上再声明一次 —— 覆盖"绕过 `SeekTo()` 直接派发 `MSG_SEEKTO`"的两条入口（`playCompleted()` 循环重开、`ProcessPrepareMsg()` 的 prepare 前 seek） |
| 同上 | `RenderAudio()` 的落点丢弃判据 | 条件由 `if (mDiscontinuity.filterActive.load())` 改为「`filterActive` **或** `audioLandingPending`」；并在丢弃循环之后加"地板关闭点"（到达目标 / 音频 EOS），打印 `audio landing reached: …` |
| 同上 | `RenderAudio()` 的"每 seek 后第一帧"分支 | `mAudioTime.startTime = pts;` 改为**先判窗口**：`pts + 帧长 <= targetUs` ⇒ `startTime = targetUs` 并打 `audio first frame after seek is OUTSIDE the seek window: …`；否则维持 `startTime = pts`（行为逐字不变） |

### 7.2 为什么它能消除偏差

1. **闸门不再和视频共用**：`audioLandingPending` 只由 seek 臂上（换档/Reset 不臂），
   只由"音频队首真的到达目标点"或"音频 EOS"关闭。⇒ §4.1 那条 race 消失：
   视频先采纳落点也不会再放行窗口外的音频帧。
   这正是代码自己在 `RenderAudio()` 上方写下的前提（`:4568-4575`）第一次被真正保证。
2. **时间轴的起点不再取窗口外的帧**：首帧若在窗口外（兜底路径），时间轴按 `targetUs` 建立
   —— 与音频时钟基准 `pinAudioClockBase(targetUs)` 是**同一个点**。⇒ 日志 :331/:332/:489/:665/:874/:1283
   那组"offset 恒偏 + re-anchor 反复触发"不会再出现。
3. **不引入新机制**：没有配置开关、没有计时器/看门狗/超时、没有阈值兜底；
   关闭点全是纯状态判据（到达目标 / EOS / 下一次不连续点），与既有
   `shouldDropForDiscontinuity()` 的形状完全一致（`framePos + 帧长 <= target ⇒ 完全在目标之前`）。
4. **不牺牲精度**：地板丢的是"用户没要的那段"，不是"精度换流畅"；
   视频侧本来就这么做（`:266` 起 ~50 条 `drop frame,master played time is 557028460,video pts is …`）。

### 7.3 回归面（哪条路径会被碰到 / 不会被碰到）

| 路径 | 行为变化 | 依据 |
|---|---|---|
| 普通 seek（有音频） | **只有**"落在目标点之前的音频帧"不再送到设备；第一个 ≥ 目标的帧照旧立即送 | 日志 :257 的 `seek audio aligned to the TARGET point` 前提；`RenderAudio()` 的 while 循环 |
| 普通 seek（无音频） | 无变化（`HAVE_AUDIO` 为假时音频路根本不跑；`mCurrentAudioIndex < 0`） | `render()` `:4473` |
| 清晰度切换 | **无变化**：`switchVideo()` 走 `beginDiscontinuity(switchPos)` 时 `mSeekAudioLandingReset` 已是假 ⇒ 地板不臂上 | 新增的 `armAudioLanding` 两个条件；代码 :6869 |
| Reset / Prepare / playCompleted | **无变化**：传 `INT64_MIN` 或（循环重开走 `ProcessSeekToMsg`）与旧 seek 同路径 | `:7920` 传 `INT64_MIN`；`:7925` 起的注释 |
| 起播第一帧音频 | **无变化或更准**：起播时 `targetUs == INT64_MIN` ⇒ 窗口判据不生效，`startTime = pts` 逐字不变 | 新增判据要求 `targetUs != INT64_MIN` |
| 暂停态 seek | **无变化**：音频路本来就不推（`mPlayStatus` 与 `render()` 的门），地板只是多一次状态判断 | `:3401-3405` |
| 音频 EOS 附近 | 地板立即关闭（`audioDecoderEOS` 条件），不会把音频闷死 | 新增的第二个关闭点 |
| ABI / 编译面 | 新成员**只追加**在类末尾与不连续点结构体末尾（未插入任何既有成员之间）；**未新增虚函数**、未加平台宏、未动 vtable | `SuperMediaPlayer.h` 顶部与 `:1519-1524` 的硬约束说明 |

**本轮新增的第 4 处改动（协议侧）**见 §7.4：DASH 与 HLS 的 manager 不再把"视频分片起点"当
其余每一路（音频等）的 seek 目标。

**仍未改（留给下一轮，见 §8）**：
- `AaudioRender::flush_device()` 拿不到 `presented` 时**保留旧基线**（`AaudioRender.cpp:691-703`）
  与 `master == audioBase + consumed` 之间可能存在的窗口内消费（§3.3 / §6.3）；
- `mAudioTime.deltaTime` 这套**没有读者**的纠偏累加器（§4.2 尾注）——删它属于"顺手重构"，
  本文不做。

### 7.4 【本轮新增 · P0-B】DASH 与 HLS 的"其余每一路"不再吃视频的分片起点

#### 7.4.1 改动清单（改前 → 改后，行号为改后）

| 文件 | 位置（改后行号） | 改前 | 改后 |
|---|---|---|---|
| `framework/demuxer/dash/DashManager.cpp` | `seek()` 的 `index == -1` 分支：`:481-563`（新增请求值副本 `:509`；视频分支 `:515-524`；新增诊断行 `:526-535`；第 3 步的 seek `:547`） | 第 2 步 `us = seekedUs;`（`seekedUs` = `DashStream::seek()` 的返回值 = **视频分片起点**），第 3 步就拿它去 seek 其余每一路 | 第 2 步先把**用户请求值**存进 `const int64_t requestedUs`；视频 seek **成功**时把 `us` 换回 `requestedUs`（**失败**时保留那个负值 ⇒ 第 3 步照旧 `return -1`，与改前逐字相同）；第 3 步于是拿到请求值 |
| `framework/demuxer/play_list/HLSManager.cpp` | `seek()` 的 `index == -1` 分支：`:497-563`（`:515` / `:521-530` / `:532-541` / `:553`） | 同上（`HLSStream::seek()` 末尾的 `usSought` 被 `SegmentList::getSegmentNumberByTime` **就地改写**成该片 `startTime`，所以同样是视频分片起点） | 同上口径，一字同形 |
| `framework/demuxer/dash/DashStream.cpp` | **不动** | 返回值 = `landingUs`（分片起点，`:1246-1256` / `:1381`） | **一字未改**。它自己的注释（`:1224-1245`）说明这条语义是给"分片起点"用的，本次不去改它的含义，只是**不再把它当另一路的目标**（避免连锁回归，见 7.4.2 的消费者审计） |
| `framework/demuxer/play_list/HLSStream.cpp` | **不动** | 返回值 = `usSought`（被 tracker 就地改写成该片 `startTime`，`:1763`） | **一字未改**，同上 |

#### 7.4.2 返回值消费者的审计（逐个结论）

**`DashStream::seek()` / `HLSStream::seek()` 的返回值**：

| 消费者（file:line） | 用途 | 改后语义是否正确 |
|---|---|---|
| `DashManager.cpp:515`（视频分支） | 存进 `seekedUs`：① 诊断日志；② 失败传播 | ✅ 正确。它仍然拿到"视频分片起点"；**只有"成功时不再写回 `us`"这一处不同** |
| `DashManager.cpp:547`（其余每一路） | 只用于 `if (seekedUs < 0) return -1;` 与 `AF_LOGD` | ✅ 正确。它现在返回的是**这一路自己**的分片起点（比改前更贴近"这一路自己的落点"），负值语义不变 |
| `DashManager.cpp:456`（`index != -1` 的 SUB 分支）`/ :445`（muxed）`/ :548`（单流） | 直接 `return` 给上游 | ✅ 正确。上游只判 `< 0`（见下表），"分片起点"这个正数本来就不被解释 |
| `DashManager.cpp:341`（`OpenStream` 里延迟 seek）`/ HLSManager.cpp:355`、`playList_demuxer.cpp:83`、`ManifestDemuxer.cpp:592` | `mPStream->seek(mFirstSeekPos, …)` / `mPPlaylistManager->seek(mFirstSeekPos, …)` | ✅ 正确且**与本改动无关**：`mFirstSeekPos` 在 `DashManager.cpp:439-442`（= 第 2 步**之前**）赋值，本来就是请求值，从不经过 `us = seekedUs` 这条改写 |
| `DashManager.cpp:563` / `HLSManager.cpp` 同处（`index == -1` 的总返回） | 返回 **0**，不是流的返回值 | ✅ 正确。这就是"视频分片起点永远到不了播放器"的原因，也说明本次改的是 manager 内部的耦合 |

**`DashManager::seek()` / `HLSManager::seek()` 的返回值**（即 manager 层出口）：

| 消费者（file:line） | 用途 | 改后语义是否正确 |
|---|---|---|
| `SMPMessageControllerListener.cpp:1125`（`ProcessSeekToMsg` → `mDemuxerService->Seek(seekPos, 0, -1)`） | `int64_t ret = …; if (ret < 0) NotifyError(ret);` | ✅ 正确。`index == -1` 时 manager 恒返回 0（`DashManager.cpp:563`），本次未改这条出口 |
| `SMPMessageControllerListener.cpp:185`（prepare 前 seek） | 返回值被丢弃 | ✅ 正确（无影响） |
| `SMPMessageControllerListener.cpp:1662`（`switchAudio`）、`:1690`（`switchSubTitle`） | 返回值被丢弃，且**带流下标** ⇒ 走 `index != -1` 分支 | ✅ 正确且不受影响 |
| `SuperMediaPlayer.cpp:6900`（`switchVideo` 的按流定位） | `ret < 0` 才报切档失败，同样**带流下标** | ✅ 正确且不受影响（切档的落点靠 `beginDiscontinuity(switchPos)` + 落点延迟线，不靠返回值） |
| `SuperMediaPlayer::mSoughtVideoPos`（`:6171-6176` 写、`:2545-2554` / `:3386-3387` 读）、`SeekTo()`、`mFirstSeekPos`、`mVideoChangedFirstPts`（`SMPMessageControllerListener.cpp:1148`） | 位置类状态 | ✅ 全部来自 `mSeekPos` / 帧自身的 `timePosition`，**没有任何一处吃 demuxer 的 seek 返回值** ⇒ 不受影响 |
| 编译/ABI 面 | — | ✅ 只改了 2 个 `.cpp` 的函数体：**无头文件改动、无新增成员、无新增虚函数、无平台宏** |

#### 7.4.3 为什么它确定性地消除"音频被视频网格拽走"（【已证实】读了代码）

1. 改前音频的读位置 = `segStart_audio( segStart_video(请求) )` —— 另一个流的网格决定了它的落点，
   请求越靠视频片尾，这个值离请求越远（实测 369~5176 ms，§4.4）。
2. 改后音频的读位置 = `segStart_audio(请求)` —— 只由**它自己的网格**决定，且按定义落在
   "包含请求的那一片"里；两路第一次拿到**同一个入参**（`requestedUs`），各自落到自己网格里
   包含它的那一片（两路的片首不一定相同，但它们对齐的是**同一个时间目标**，前缀由播放器侧
   按同一个 `targetUs` 裁掉）。
3. 改后音频读起点**不会晚于**请求（**分片时间轴连续时**，三条分支逐条读过）：
   · HLS：选第一个满足 `startTime + duration > time` 的片（`play_list/SegmentList.cpp:114-120`），
     连续网格下 `startTime <= time`；
   · DASH 的非 SegmentTimeline 分支：选**最后一个 `startTime <= time` 的片**
     （`dash/ISegmentBase.cpp:52-75` 的循环），直接就是 `<= time`；
   · DASH 的 SegmentTimeline 分支：选**包含该时刻**的那个 element
     （`dash/SegmentTimeline.cpp:68-71` 的 `t <= scaled < t + d*r`），同样 `<= time`；
   ⇒ 目标帧一定在读取范围里，播放器侧的落点判据（`shouldDropForDiscontinuity` 与音频落点地板
   都比 `targetUs`）照旧把各自的前缀裁到同一个点。**唯一例外是时间轴有空洞**，见 7.4.6。
4. **前缀只可能变短、不可能变长（单调性，确定性）**：上面那些 `getSegmentNumberByTime` 对入参
   都是**单调不减**的（分片按时间递增排列；遇到空洞只是**往后跳**一片，不会回头），而
   `segStart_video(请求) <= 请求` ⇒
   `segStart_audio(segStart_video(请求)) <= segStart_audio(请求)`。
   也就是说：改后的音频读起点**恒不早于**改前 ⇒ 需要解码并被播放器丢掉的前缀**不会变多**。
   这正是 §8.1 那条"`audio landing drop: dropped=N` 不应变大"判据的依据。
5. 反过来，改前那个值**没有**这个上界保证：它只保证"早于视频片起点"，不保证"早于请求的幅度不超过
   一个音频片" —— 只要音频自己那一片的边界落在[视频片起点, 请求]之间，前缀就多出一整片
   （这就是"必须多解码一段、seek 收敛更慢"的来源；两个网格等长时这种 seek 恰好是空操作）。

⇒ **确定性地消除的是"音频的读位置由视频的分片网格决定"这件事**（方向恒为音频读起点更早）；
**没有**、也不该声称"把 371~1938 ms 的相位差抹掉" —— 那组数是"音频上屏帧 vs 视频解码起点"
两种读数的差（§4.4 口径勘误），相位本身在 DASH 段一直是 +3~11 ms。

#### 7.4.4 视频侧"一字不变"的证据（【已证实】读了代码）

- 视频那次 `seek()` 的**实参**仍是函数入参 `us`：`DashManager.cpp:515` / `HLSManager.cpp:521`
  之前，`us` 在两处都只被**读**过（`mFirstSeekPos = us`），从未被写过；
- 视频 `seek()` 的返回值仍然只流向原来的两个去处：`AF_LOGD("first seeked time is …")` 与
  **失败传播**（负值时第 3 步照旧 `return -1`，因为 `us` 在失败分支被原样保留）；
- 视频的落点对齐（`landingUs`）与落点延迟线（`mSeekLanding.arm(us, "DASH"/"HLS", mPTracker)`，
  `DashStream.cpp:1356` / `HLSStream.cpp:1750`）都在 **Stream::seek() 内部**，本次**没有改这两个文件**；
- 第 3 步的失败判据 `if (seekedUs < 0) { return -1; }`（`DashManager.cpp:549-551`）一字未动；
- 只有**视频流存在**时才会走到"把 `us` 换回 `requestedUs`"这一步；无视频流（纯音频片源）时
  第 2 步的循环不进入，`us` 本来就是请求值 ⇒ 行为与改前相同。

#### 7.4.5 回归面

| 路径 | 行为变化 |
|---|---|
| DASH/HLS 多流 seek（`index == -1`） | **只有**"其余每一路（音频等）的 seek 目标"变了：视频分片起点 → 用户请求值 |
| 视频那一路 | 无变化（见 7.4.4） |
| 单流 seek（切档 `SuperMediaPlayer.cpp:6900`、`switchAudio` `SMPMessageControllerListener.cpp:1662`、`switchSubTitle` `:1690`） | 不走本分支，无变化 |
| 纯音频片源（无视频流） | `us` 本来就是请求值，无变化 |
| `ManifestDemuxer` 那条管线（**它连 `mediaSourceType dash` 也交给 HLSManager**：`ManifestDemuxer.cpp:579-583` 只打一条 WARN 然后 `new HLSManager`，`:592` 用 `mFirstSeekPos` 补 seek） | 同样受益于 HLSManager 的改动；用户日志走的是另一条（`playList_demuxer.cpp:60-64` 按清单类型建 **DashManager**，日志里的 `[dash] 清单里有 6 条视频档` 就是 `DashManager.cpp:88` 打的） |
| 视频 seek 失败的片源 | **无变化**：失败时 `us` 仍是那个负值（`DashManager.cpp:518-520`），第 3 步用负值 seek 其余每一路、由 `:549-551` 返回 -1，与改前同形；只有视频流时 `us` 无人再读 ⇒ 仍返回 0（与改前相同，本次没有新增早退） |
| 已核对仍成立的前提 | `mFirstSeekPos` 仍是请求值（赋值在第 2 步之前）；`SeekLandingStage` 仍是"只给 VOD 视频路装弹"（`SeekLandingStage.cpp:45-60`） |
| 编译面 | 2 个 `.cpp` 的函数体改动；无新增成员/虚函数/头文件改动/平台宏 |

#### 7.4.6 【推测 · 待验证】本改动的两个已知边界

- **分片时间轴有空洞（discontinuity）时**：DASH 的 `SegmentTimeline` 分支**明确处理**这种情况 ——
  `dash/SegmentTimeline.cpp:73-82` 的 `scaled < el->t` 分支返回的是**空洞之后那一个 element**
  （`prevel->number + prevel->r`），它的 start 晚于请求时刻；HLS 的
  `play_list/SegmentList.cpp:114-120` 选的是"片尾在请求之后"的第一片，遇到空洞同样可能落到
  空洞之后。⇒ 这种片源上"请求时刻"没有对应的媒体内容，两路都会落到空洞之后
  （相位差 = 空洞长度）。
  **这不是本次引入的新风险**：改前喂进去的值（视频片起点）同样可能落进空洞；而且播放器侧的
  `shouldDropForDiscontinuity` 本来就有"`framePos > targetUs` ⇒ 采纳"这条分支专门处理它。
  **需要什么证据**：MPD 的 `SegmentTimeline` 里 `t`/`d`/`r` 是否连续（有没有跳变）、HLS 清单的
  分片是否连续。本次日志的取值全部落在规则网格上（视频分片号 8/15/21/28/35 对应
  48048000/90090000/126126000/168168000/210210000 µs，都是 6.006 s 的整数倍），没有空洞迹象。
- **两路分片等长且对齐时**：`segStart_audio(视频片起点)` 恰好等于 `segStart_audio(请求)`
  ⇒ 本次改动在那一次 seek 上是**空操作**（行为逐字相同，不会变差）；它生效的场合是
  "音频分片边界落在[视频片起点, 请求]之间"，也就是视频分片比音频分片**粗**的常见配置
  （`DashStream.cpp:1231` 记的 output.mpd 就是视频片 19.9866 s / 音频片 9.984 s）。
- **视频分片为空/极短时**：视频返回值可能就是请求值本身，此时两路口径本来就一致（改动是空操作）。

#### 7.4.7 音频为什么**不需要**一条自己的落点延迟线（【已证实】读了代码）

1. **延迟线的职责不是"精度"**：`SeekLandingStage` 的类注释自己写明"精度权威在 renderer 的单一
   落点过滤；这里只是降低解码前推距离的尽力优化，失败只慢不错"（`SeekLandingStage.h:23-26`）。
   它做的是"把解码起点从分片片首挪到**不晚于目标的最后一个关键帧**"，前提是**视频必须从 IDR 起解**
   —— 这是 GOP 语义。
2. **音频没有这个约束，而且"裁前缀"这件事已经在播放器侧做了，且是状态驱动的**：AAC 逐帧可解，
   所以"分片片首到目标点"这段前缀不需要 demuxer 先挑落点，播放器可以**逐帧**丢。
   判据就是 `RenderAudio()` 的音频落点地板：按 `mDiscontinuity.targetUs` 丢掉 `pts < targetUs` 的帧，
   三个关闭点全是状态（队首到达目标 / 音频 EOS / 下一次不连续点），
   没有计时器、没有重试、没有阈值（`SuperMediaPlayer.cpp:4620-4674`）。
3. **在 demuxer 再建一条音频延迟线不会提高落点精度**（精度已被第 2 条钉在"目标点"上，而
   `SeekLandingStage` 自己声明它不是精度权威），它只能把"丢弃"从解码后提前到解码前、省下解码量；
   而同一件事的**大头**已经由 §7.4 的改动省掉了（读起点从视频网格回到音频自己的网格）。
   代价是新增**第二个精度权威**（两条线对"落点"的判定必须永远一致，否则就是新的错位来源），
   与上面的设计声明直接冲突。
4. **混合流（MIXED）更不能装**：延迟线一丢前缀就会把同一路里的音频一起丢掉 ——
   `SeekLandingStage.cpp:45-49` 那条"not armed"日志的原文写的就是这个理由
   （`audio has no GOP semantics, MIXED would drop audio together with the prefix`），
   也就是既有设计**明确**只给 VOD 视频路装弹。

⇒ **结论：音频不需要落点延迟线，本轮也不加。** 它需要的是两件已经具备的事：
   ① 与视频拿到**同一个目标点**（§7.4 的改动）；② 按同一个目标点**逐帧裁前缀**（既有的音频落点地板）。
   【推测·待验证】若以后仍想省掉"最多一个音频分片"的解码量，那属于**性能项**而不是正确性项，
   判据是 §8.1 的 `audio landing drop: dropped=N` 是否仍然偏大；真要做的形状必须与地板同源
   （包上的 `timePosition` 与目标比较、由"读到超过目标的包"这个事件收口），不能引入计时器或重试。

---

## 8. 待验证项 + 需要用户补的日志

### 8.1 用当前改动复现，判据是这几行（按顺序看）

| 期望 | 判据（日志原文形状） |
|---|---|
| 地板生效 | 出现 `audio landing drop: dropped=N pos=… target=… generation=…`（新日志），紧随其后 `audio landing reached: head=… target=… eos=0 droppedBeforeTarget=N` |
| 首帧不再落窗口外 | `audio first frame after seek: pts=… target=…` 里 **`pts >= target`**（旧日志里 7/8 次是 `pts < target`） |
| offset 归零 | **不再出现** `audio first frame after seek is OUTSIDE the seek window:`；`correct audio and master clock offset is …` 的值应在 ±一个帧长（23219 µs）内 |
| 限幅不再被顶 | **不再出现** `audio clock drift: correction deltaTime=300000 us (capped at +-300000)` |
| re-anchor 不再反复 | `audio timeline re-anchored (offset … > 1s)` **0 条**（旧日志 4 条：:489/:665/:874/:1283） |
| 视频不再被误判迟到 | seek 完成后 1 秒内 **不出现** `[GLRender] drop a frame pts = …` 连刷（旧日志 :333-338 六条） |
| **P0-B（DASH）：两路终于同一个目标** | `[seek] dash video: reqUs=X` 与 `[seek] dash audio/other: reqUs=X` **是同一个 X**（旧日志里后者恒小于前者，差 369~5176 ms）。DASH 的这条判定可用 `reqUs` 本身，因为 DASH 的 tracker 链是**按值**取时间（`play_list/Representation.h:76` / `Representation.cpp:238` 的形参是 `int64_t time`，`dash/DashSegmentTracker.cpp:365-371` 只把它转手），不会就地改写 `usSought` —— 这也是改前 DASH 日志能直接看到这个差的原因 |
| **P0-B（DASH）：manager 决策可见** | 每次 seek 多一条 `[dashseek] user=X video_return=Y -> other_seek_target=X (stream=… type=…)`：`video_return == Y` 仍 `<= X`（视频分片起点，视频侧一字未变），`other_seek_target` 必须**等于** `user` |
| **P0-B（HLS）：这条不能用 `reqUs` 判** | HLS 的 `[seek] hls …: reqUs=` 打的是被 tracker **就地改写过的** `usSought`（= 该路自己的分片起点，§4.4 尾注），所以两行**本来就应该不同**。要用新增的 `[hlsseek] user=X video_return=Y -> other_seek_target=X (stream=… type=…)`：`other_seek_target == user` 且 `Y <= X` |
| **P0-B：前缀预算没有变大** | 同一个 seek 位置上，`audio landing drop: dropped=N` 的 `N` **不应大于**改前的同一次（期望随音频读起点靠近请求而变小）；`audio first frame after seek` 的 `afterSeekMs` 也不应变大 |

### 8.2 必须补的日志（P0-A 仍需补；P0-B 已直接写进代码）

**P0-A：seek 窗口内 `consumed` 为什么涨了 320~700 ms（§6.3 的未决项）**
在 `RenderAudio()` 的"每 seek 后第一帧"分支那条 `audio first frame after seek` **之前**，
每 100 ms（或每个音频帧）多打一行（低频、带限频）：

```
[audioref] t=…ms base=%lld baseConsumed=%lld consumed=%lld ref=%lld local=%lld
           ringFilled=%lld underrunFrames=%llu videoPts=%lld
```
（`consumed` 用 `getAudioRenderPosition()`；`underrunFrames` 用 `AaudioRender` 已有的
`mUnderrunFrames` 暴露出来即可。）
判据：① `consumed` 涨而 `underrunFrames` 也涨 ⇒ 是欠载补静音在推参考；
② `underrunFrames` 不涨而 `consumed` 涨 ⇒ 设备真的播了音频（⇒ 陈旧音频确实进了设备）。

**P0-B：`DashManager` 喂给音频的目标点是哪儿来的（§4.4）—— 【已修（代码级），见 §7.4】**
改前的形态就是本文推断的那样：`DashManager.cpp` 改前 `:488` 取回视频返回值、`:490 us = seekedUs;`、
`:502` 把它当其余每一路的目标（`HLSManager.cpp` 改前 `:505/:507/:519` 同形）。修法是把第 3 步的
目标换回**用户请求值**（不是改 `Stream::seek()` 的返回值语义）。
需要补的日志已经**直接写进代码**（AF_LOGI 级，用户现有的 logcat 过滤就能看到，每次 seek 一条）：

```
[dashseek] user=%lld video_return=%lld -> other_seek_target=%lld (stream=%d type=%d)
[hlsseek] user=%lld video_return=%lld -> other_seek_target=%lld (stream=%d type=%d)
```

判据（与 §8.1 最后四条一致）：`other_seek_target == user`；DASH 另可拿
`[seek] dash video/audio: reqUs=` 两行相等做独立佐证；HLS 则必须以 `[hlsseek]` 为准。
**仍待运行时验证的部分**：这些行在真机 DASH/HLS seek 上是否恒成立（尤其"音频那一路的
`reqUs` 是否真的等于视频那一路"、`audio landing drop: dropped=` 是否不再变大）。

**P1：HLS 要实测一次**（本次日志里没有 HLS）。
复现方式：Android 上播 `master.m3u8`（与本地文件同一份媒体），做**完全相同的 8 次拖动**，
回传 logcat。要看的是 §2.1 那张表的三列（`target` / `pts` / `afterSeek*`）**加上 §7.4 新加的那条**：
`[hlsseek] user=X video_return=Y -> other_seek_target=X` 里 `other_seek_target` 必须等于 `user`，
以及 `[seek] hls video: reqUs=…` / `[seek] hls audio/other: reqUs=…` 两行各自是**自己那一片**的片首
（两者不同是正常的，见 §8.1 第 3 条）。HLS 预期是"`pts >= target` 且落点在自己的分片网格内"。

**P2：`TIMEPOS reSync` 高频簇（§4.5）** 需要 8.2 的 P0-A 那行才能判断是
`consumed` 在跳还是 `base` 在变。

### 8.3 复现步骤（最短路径）

1. 装新内核。本轮同时改了 **framework/demuxer** 下两个 `.cpp` 的函数体（`dash/DashManager.cpp`、
   `play_list/HLSManager.cpp`，见 §7.4）与 `mediaPlayer/`（§7.1）⇒ **框架库与播放器库都要重编**；
   `SuperMediaPlayer.h` 也变了 —— 按本仓库的硬规则，**必须删掉 `SuperMediaPlayer*Listener`
   等 friend TU 的 .obj** 或干脆全量重编，否则增量构建会按旧偏移访问。
   （§7.4 那两处**没有**头文件改动，所以纯粹是"库要重编"，不涉及 ABI。）
2. 打开 logcat 级别至少 `I`（WARN 也要），过滤 tag：`AliFrameWork`、`AaudioRender`、`AlivcPlayerClock`；
3. **本地文件**：播同一份本地视频 → 拖动 8 次（每次间隔 >2 s，位置分散）；
4. **DASH**：播 `output.mpd` → 同样 8 次拖动；再**不 seek 连播 60 秒**（给 §4.5 用）；
5. 回传整份 logcat（不要过滤掉 `MediaCodec`/`ACodec`/`AAudio` 行，它们是时序参照）。
