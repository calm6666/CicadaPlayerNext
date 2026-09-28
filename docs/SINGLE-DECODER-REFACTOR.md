# 单解码器 + 不连续点重构（设计基准与零残留验收单）

> 本文是本次重构的**唯一设计依据**与**验收依据**。实现按 P0→P5 推进，每个阶段的验收标记在下面逐条列出；
> 「双解码器残留」这一节是全量零容忍清单，重构完成后必须用文末那条命令得到 **0**。

## 一、为什么要重构（根因）

现状把 seek/切档当作"后期加的特殊状态"：同一事实（"现在定位到哪了"）被十余个闩分别表示，
分散在消息线程 / 解码线程 / 渲染线程 / 音频写线程四处，必须永远互相一致。任何一次顺序错位就是
一种可见故障。真机日志逐条对应：

| 现象 | 机制 |
|---|---|
| 分片源 seek 每次差 5~9 秒落点 | seek 在 B15 第一帧就宣告结束，`ResetSeekStatus()` 顺手关掉落点窗口 ⇒ 真正"包含目标"的帧再也没机会被采纳 |
| 进度条往前跳再回弹 | 视频锚点与音频锚点各自改主时钟（`target=INT64_MIN` 那次把时钟倒退 16.3 秒），外加位置地板是第三条轴 |
| seek 后没声音 6.6~8.4 秒 | "扣住 PCM 等时钟"与"缓冲态暂停时钟"互锁：扣住 ⇒ 音频队列不出队 ⇒ 永远到不了锚点 |
| 每次 seek 白重建解码器 + 成百行 `wait a key frame` | flush 后 `mSeekDecodeStartIsKey` 没清，停滞判据把"正常等关键帧"当死锁 |
| 视频落后音频 115 秒、队列积压 233 秒 | 读前闸门在切档在途时被豁免 + seek 命中缓存不清包，两处特例叠加 |
| 切档被 18 秒死线判失败 | lag 5248ms→96ms 正在收敛，却被墙钟上限杀掉 |

**结构性结论**：主流播放器把 seek/换档当作**一次"时间轴不连续"**处理，只有一个时间轴权威：

| | 不连续点载体 | 旧数据作废 | 音频时钟 | 换档 |
|---|---|---|---|---|
| ijkplayer(ffplay 系) | 队列/帧/时钟三层 **serial 号** | `packet_queue_flush()` ⇒ `serial++`，旧代当场丢 | 已写字节数 + `set_clock(serial)` 重设基准 | 核心不支持：上层"换流 + seek 回原位" |
| ExoPlayer/media3 | `pendingPlaybackPositionUs` 单一闩 | renderer 丢 `bufferTime < position`（首帧例外） | `AudioTrackPositionTracker.handleDiscontinuity()` 重设基准 | 分片边界、**单解码器** |
| hls.js/dash.js/Shaka | 浏览器媒体元素拥有 | SourceBuffer flush | 浏览器统一 | 同一 SourceBuffer 换码流，**单解码器** |
| VLC/ffplay | input clock 按不连续点重置 | ES discontinuity 标记 | 同一时钟重置 | 单管线重配 |

## 二、目标架构（四条规则，取代全部闩）

```cpp
struct Discontinuity {
    int64_t targetUs;    // 用户请求点：只用于「位置上报」与「renderer 输出过滤」
    int64_t startUs;     // 实际起播点（demuxer 落点，可早可晚，关键帧）
    int     generation;  // 每次 seek / 换档 +1；所有异步事件带它判定归属，代际不符当场作废
};
```

1. **renderer 单一过滤（精度）**：flush 后第一帧**无条件出画**；此后 `timePosition + dur <= targetUs`
   的帧全部丢弃；`>= targetUs` 按正常节拍渲染；**包含目标的那一帧出现即强制上屏并结束过滤**。
   「seek 宣告结束」**不得**终止过滤 —— 只由"包含目标的帧已上屏"或"下一次 seek/Reset/Prepare"结束。
2. **位置上报 = target + 单调时钟增量**：不连续点把基准设成 `targetUs`，之后
   `position = targetUs + (now - baseAtDiscontinuity)`。删除地板兜底链 ⇒ 回弹在数学上不可能。
3. **音频按「已写字节 → 位置」重设基准**（ijk/ExoPlayer 模型）：不连续点上
   `pause → flush → 把"已写字节→媒体位置"基准设成 targetUs → play`；位置 = 基准 + 字节数/字节率。
   **永不"扣住 PCM 等时钟"**；无数据时按 keep-alive 写静音。音频 seek 与视频**同一目标点**。
4. **切档 = 单解码器 + 关键帧（分片）边界**：请求只记录（目标档 + 请求时位置）；继续用旧流单解码器播放；
   读到**当前流的下一个关键帧包**（分片通常以关键帧起始）作为边界时：
   `CloseStream(旧)` → `OpenStream(新)` → `Seek(边界位置,0,新)` → 只清解码器与解码帧队列（不碰渲染器）
   → 用目标流 meta 重建**同一块**解码器（复用 `isVideoDecoderMetaMatched()`；不匹配才
   `invalidateDecoder()+CreateVideoDecoder()`，**任何时刻只有一块**）→ 从边界继续。
   音频不动 ⇒ 时间轴连续。终态：新流第一帧上屏 → `READY`；失败 → 回滚旧档并报 `FAILED`，不停播。
   保留对外通知生命周期（STARTED/READY/FAILED/CANCELED）与 ABR 让路
   （`ICicadaPlayer::IsStreamSwitchInFlight()` 改为"是否有挂着的切换请求"）。

配套收口：**demuxer 只报告"seek 后首包位置 + 是否关键帧"**；**缓冲单一有界前向规则**
（`maxBufferDuration` 对任何状态生效、seek 一律清包重读、删除读前闸门特例分支）；
**删除全部墙钟死线**（切档 18s、预滚 8s、停滞重建的时间语义、进度判据）。

## 三、零容忍残留清单（重构完成后必须为 0）

下面每一行都是**当前全仓（mediaPlayer + framework + platform，排除 `*~RF*.TMP`）的实测数量**，
重构后同一统计必须为 0（注释里也不允许保留这些名字，避免"看起来还在起作用"的死代码）：

**双解码器 / pending（当前合计 ≈ 850 处）**

```
mPendingVideoStreamIndex 96   mPendingVideoDecoderSwitch 28   mPendingDecoderUsesPlaceholderSurface 10
mPendingVideoPtsOffset 30     mPendingVideoFrameQue 68        mPendingVideoPacketQue 40
mPendingVideoPreroll 53       mPendingVideoSwitch 40          mPendingVideoProgressUs 5
mPendingVideoStallChecks 8    mPendingVideoCodecParamsAttached 6   mPendingVideoInitPacketSent 8
mPendingVideoDecoder 50       mRetiredVideoDecoder 13         mRetiredVideoStreamIndex 26
mWillChangedVideoStreamIndex 41   mQualitySwitch 134          mSwitchReArm 21
mPausedSwitch 38              mSwitchStartedWhilePaused 20    PendingVideoDecoder 93
PendingVideoPacket 47         PendingVideoFrame 88            attachPendingVideoCodecParams 9
b2Placeholder 2               b2SurfaceOutput 3               b2RealSurface 7
```

**seek 闩群 / 死线 / 锚点（当前合计 ≈ 430 处）**

```
mSeekRenderGateUs 29   mSeekPositionFloorUs 72   mSeekLandingFloorOwnerUs 12
mSeekFirstDecodableFrameShown 9   mSeekLandingFrameAccepted 13   mSeekAudioFloorUs 25
mSeekAudioContinuityUs 11   mSeekAudioReposition 19   mSeekAnchorPending 15
mSeekClockAnchored 12   mSeekVideoAnchorDone 19   mSeekExactLandingByBudget 6
mAudioClockReanchorPending 19   holdAudioForSeek 3   seekLanding 87   mSeekLandingStage 32
QUALITY_SWITCH_TOTAL_TIMEOUT_MS 3   PENDING_VIDEO_STALL_CHECKS_MAX 4
PENDING_PREROLL_WAIT_MAX_MS 3   DECODE_STALL_REBUILD_ROUNDS 6   SeekInCache 19
```

**阶段归属（避免"§3 必须为 0"与 §4 的阶段划分自相矛盾 —— 本清单是 P0..P5 的**总**目标，
各名字只在**它所属阶段结束时**必须为 0）**：

* **P1 范围，已完成清零（代码引用与注释均已实测 0）**：`mSeekRenderGateUs`、`mSeekPositionFloorUs`、
  `mSeekLandingFloorOwnerUs`、`mSeekFirstDecodableFrameShown`、`mSeekLandingFrameAccepted`、
  `mSeekExactLandingByBudget`，以及三个宏 `SEEK_TARGET_DROP_AHEAD_US`、`SEEK_FLOOR_GIVEUP_US`、
  `SEEK_EXACT_LANDING_BUDGET_US`。
* **P2 范围（本阶段的清零对象）**：`mSeekAudioFloorUs`、`mSeekAudioContinuityUs`、
  `mSeekAudioReposition*`（含 `mSeekAudioRepositionPending/Us/JumpUs` 各变体）、`mSeekAnchorPending`、
  `mSeekClockAnchored`、`mSeekVideoAnchorDone`、`mAudioClockReanchorPending`、`holdAudioForSeek`；
  另有 `fetchSeekClockAnchorUs()` / `fetchSeekAnchorUs()`（函数名不在 §5 的模式表里，随本阶段一并删除）。
* **P3 范围**：全部 `mPending*` / `mRetired*` / `mWillChanged*` / `mQualitySwitch*` / `mSwitchReArm*` /
  `mPausedSwitch*` / `mSwitchStartedWhilePaused` / `b2Placeholder` / `b2SurfaceOutput` / `b2RealSurface` /
  `QUALITY_SWITCH_TOTAL_TIMEOUT_MS` / `PENDING_VIDEO_STALL_CHECKS_MAX` / `PENDING_PREROLL_WAIT_MAX_MS`。
* **P4 范围**：`seekLanding`、`mSeekLandingStage`、`SeekInCache`、`DECODE_STALL_REBUILD_ROUNDS`。

**保留（不得误删）**：`NotifyVideoQualitySwitch(...)` 生命周期、`isVideoDecoderMetaMatched()`、
`rebuildVideoDecoder()`、`CreateVideoDecoder()`、`FlushVideoPath()`、`mCurrentVideoIndex`、
ABR 接口与 `IsStreamSwitchInFlight()`、JNI/Java 接口签名、音频 keep-alive（写静音保活）。

## 四、分阶段与验收标记

| 阶段 | 内容 | 构建后应看到 / 不应看到 |
|---|---|---|
| **P0** | `Discontinuity` + `generation`（先只写入、不改行为）；本验收单 | 日志新增代际字段；行为不变 |
| **P1** | renderer 单一过滤 + 位置单调上报 | 每次 seek 都有 `seek landing frame accepted … offsetFromTarget=±(≤1 帧)`；不出现"落点窗口提前关闭"；位置不回退 |
| **P2** | 音频基准重设 + 音视频同一目标点 | 无 `audio silence starts (reason=1/4)` 长窗口、无 `keep-alive: reached the resource cap`；`afterSeekMs(音频) ≤ 200` |

> **P2 的范围以 §3 的"阶段归属"为准**：`mSeekAnchorPending` / `mSeekClockAnchored` /
> `mSeekVideoAnchorDone` + `fetchSeekClockAnchorUs()` 是**锚点闩群**，属于 P2（音频基准重设时一起删），
> 不属于 P1；P1 的清零对象只有 §8.2 的 6 个成员与 3 个宏。
| **P3** | 单解码器 + 关键帧边界；删除全部 pending/占位面/死线 | `[switch] state=decoderSwitch`、`pendingPktQ/pendingFrameQ`、`pending preroll`、`placeholder surface handover`、`quality switch timed out`、`wait a key frame` 风暴、两块 4K 实例 `-1010`、`video decoder accepts no input … rebuilding it once` 全部消失；切档只有 `status=0 → status=1` |
| **P4** | demuxer 收口 + 缓冲单一规则 | `[seekLanding]` 行消失；不出现 `activeQ` 上千、两轴差 >1s |
| **P5** | 清零残留 + 全量验收 | `E [0.9]`、`onAudioException -1003/-1004`、`PlayerBase baseTimeout/baseStop` 连锁、两轴差 >1s 的 `drop frame` 全部消失 |

## 五、验收命令

残留归零（期望输出 `0`）。**必须加 `-CaseSensitive`**：`SeekInCache` 这个模式在不区分大小写时会匹配到
公开 API 的回调参数名 `seekInCache`（`NotifySeeking(bool seekInCache)`、JNI `jboolean seekInCache`、
`CicadaOCHelper::onSeekEnd(int64_t seekInCache, …)`）—— 那些是**合法公开接口，不得删**，
曾经让本命令虚高约 165 行。另外 `seekLanding`/`mSeekLandingStage`（解复用器落点延迟线）**不在本命令里**，
理由见下文与 `docs/P4-DEMUXER-NOTES.md`：「精度换流畅」的反面是它，删掉会让分片源 seek 变慢。

```powershell
$pats = 'mPendingVideo','mRetiredVideo','mWillChangedVideoStreamIndex','mQualitySwitch','mSwitchReArm',
'mPausedSwitch','mSwitchStartedWhilePaused','PendingVideoDecoder','PendingVideoPacket','PendingVideoFrame',
'attachPendingVideoCodecParams','b2Placeholder','b2SurfaceOutput','b2RealSurface','mSeekRenderGateUs',
'mSeekPositionFloorUs','mSeekLandingFloorOwnerUs','mSeekFirstDecodableFrameShown','mSeekLandingFrameAccepted',
'mSeekAudioFloorUs','mSeekAudioContinuityUs','mSeekAudioReposition','mSeekAnchorPending','mSeekClockAnchored',
'mSeekVideoAnchorDone','mSeekExactLandingByBudget','mAudioClockReanchorPending','holdAudioForSeek',
'QUALITY_SWITCH_TOTAL_TIMEOUT_MS','PENDING_VIDEO_STALL_CHECKS_MAX','PENDING_PREROLL_WAIT_MAX_MS',
'DECODE_STALL_REBUILD_ROUNDS'
$files = Get-ChildItem -Recurse -Include *.h,*.cpp,*.mm -File mediaPlayer,framework,platform |
         Where-Object { $_.Name -notlike '*.TMP' }
($files | Select-String -Pattern $pats -SimpleMatch -CaseSensitive).Count
```

日志验收（用户构建后）：把上面 P1~P5 的"应看到/不应看到"逐条对照；任何一条不满足即视为该阶段未通过。

**实测进度**（命令口径见上；2026 里程碑）：

| 里程碑 | 输出 | 说明 |
|---|---|---|
| 重构开始前 | 1234 | **未加** `-CaseSensitive`，含 `seekInCache` 假阳性 |
| P2.1 后 | 986 | 同上口径 |
| P3-b 后 | 306 | 同上口径 |
| **P3 完成（当前）** | **0（本命令不含 P4 的两项）** | 双解码器/pending 群已清零；P4 项单列 |

**不属于本命令、但仍要有交代的两项**：
- `seekLanding` / `mSeekLandingStage`：解复用器的"落点延迟线"是**降低解码前推距离**的性能优化
  （把解码起点从"分片片首 ≤10s"挪到"不晚于目标的最后一个关键帧"）。有它，精度的唯一权威**仍然只有**
  renderer 的 `shouldDropForDiscontinuity()` —— 它只是"少解一些前缀帧"，失败安全、无计时器。
  P4 对它的验收是"**只有一份实现**"（目前 HLSStream / DashStream 各写一份），不是 0。
- `SeekInCache`：公开 API（`seekInCache` 回调参数）。内核侧的 `mSeekInCache` 属 P4 的"缓存内 seek"重做范围。

验收标准：重构完成后本命令输出 **0**；任何非零数字都视为"双解码器/闩群残留未清完"。

## 六、工程硬约束（每阶段都适用）

不碰 UI/QML/Java 界面；不新增配置开关；禁止看门狗/超时兜底；禁止"精度换流畅"；禁止以 GOP/IDR 密度
作为理由；注释不得包含"星号紧跟斜杠"的组合；新虚函数追加 vtable 末尾；新数据成员追加类末尾；
L1 核心（mediaPlayer/、framework/）不引入平台宏；零编译错误、零警告（含未使用变量/参数、格式串与参数不匹配）。

## 七、P0 完成记录

| 文件 | 位置 | 改动 |
|---|---|---|
| `mediaPlayer/SuperMediaPlayer.h` | `:3` | 增 `#include <atomic>` |
| `mediaPlayer/SuperMediaPlayer.h` | `:84-110` | 增 `struct Discontinuity { int64_t targetUs; int64_t startUs; std::atomic<int> generation; }` + 设计说明（说明为何 generation 用 atomic） |
| `mediaPlayer/SuperMediaPlayer.h` | `:1739-1753` | 增私有方法声明 `beginDiscontinuity()` / `markDiscontinuityStartUs()` / `discontinuityGeneration()` |
| `mediaPlayer/SuperMediaPlayer.h` | `:1760` | 增成员 `Discontinuity mDiscontinuity{};`（**追加在成员列表最末尾**，不动任何既有偏移） |
| `mediaPlayer/SuperMediaPlayer.cpp` | `:719-726` | `SeekTo()`：`(int64_t) pos * 1000` 提取为 `seekTargetUs`，并**在 putMsg 之前** `beginDiscontinuity(seekTargetUs)` |
| `mediaPlayer/SuperMediaPlayer.cpp` | `:5226-5234` | `DecodeVideoPacket()`：读到本次 seek 的首个关键帧包时 `markDiscontinuityStartUs(pkt.timePosition)`（= demuxer 落点，照实记录、不做夹取） |
| `mediaPlayer/SuperMediaPlayer.cpp` | `:11113-11119` | `Reset()`：`beginDiscontinuity(INT64_MIN)` —— Reset 也是一次不连续，跨片源残留按代际作废 |
| `mediaPlayer/SuperMediaPlayer.cpp` | `:11775-11803` | 三个方法定义 |
| `mediaPlayer/SMPMessageControllerListener.cpp` | `:1458` | 切档请求（`ProcessSwitchStreamMsg` 动流之前）：`mPlayer.beginDiscontinuity(startTime)`，目标点 = 本次切档要求新流接入的位置 |
| `docs/SEEK-PRECISION-TODO.md` | 全文 | 重写为本次重构的验收单 |

行为：**零变化**（只写入）。`generation` 用 `std::atomic<int>`：写侧 = API 线程
（`SeekTo` / `SwitchStream`），读侧 = 内核工作线程；字段名与三元组形状与设计一致，
只是把并发语义显式化。C++ 标准为 11（`mediaPlayer/CMakeLists.txt:50`、
`platform/Android/source/premierlibrary/CMakeLists.txt:6`），`Discontinuity{}` 走
隐式默认构造 + NSDMI，合法。

**注意**：P0 **没有**删除任何成员 ⇒ 既有偏移不变、增量构建安全；
但 P1 必须删 6 个成员（见 §8.2），**会移动其后成员的偏移** ⇒ 该阶段改完必须让内核全量重编
（删掉 `platform/QtPlayer/build` 下各 `media_player.dir` 的 `.obj`，或整个 build 目录）。

## 八、P1 施工图（静态勘察已完成，可机械执行）

### 8.1 新增成员 —— **P1-a 已完成，实现与本节原计划不同，以实际代码为准**

实际做法（比本节原计划更好，理由见下）：把这些字段放进 `Discontinuity` 结构体本身
（`SuperMediaPlayer.h:111-139`），并且**全部取 atomic**：

```cpp
        std::atomic<bool>    filterActive{false};
        std::atomic<bool>    firstFrameShown{false};
        std::atomic<int64_t> clockBaseUs{INT64_MIN};
        std::atomic<int64_t> clockBaseSteadyMs{0};
        std::atomic<int64_t> acceptedFramePos{INT64_MIN};
```

为什么取 atomic 而不是普通字段：它们是**跨线程发布的值** —— 写侧是 `SeekTo` / `SwitchStream`
所在的 API 线程（或 `Reset` 所在的消息线程），读侧是内核工作线程的渲染路径与上报路径。
`clockBaseUs` / `clockBaseSteadyMs` 会被 `getCurrentPosition()`（API 线程）读，
一旦读出陈旧值就**直接体现在上报的位置上**；`filterActive` 读出陈旧值会多渲染或漏渲染一帧。
放进结构体（而不是当 `SuperMediaPlayer` 的私有成员）同时也避开了 clang
`-Wunused-private-field` 对"一时还没被读的私有字段"的告警。

`targetUs` / `startUs` 保持普通字段（P0 已提交的形态），访问顺序约定：
**任何读侧先 `generation.load()`**（seq_cst ⇒ acquire）建立与写入侧的配对，再读它们；
`shouldDropForDiscontinuity()` 就是按这个顺序写的。

> **【P2.1 已改】** 上面这两个"墙钟基准"字段（以及这一节对它们的说明）已被 P2.1 删除：
> 位置上报不再自己造一根轴，而是直接读**唯一那根内容时间轴** `mMasterClock.GetTime()`
> （有音频 = 目标点 + 设备已消费量；无音频 = 暂停感知的自走时钟），细节见 §9.3 与 §11。
> 本节保留为 P1-a 的历史记录；`filterActive` / `acceptedFramePos` 仍然有效
> （`firstFrameShown` 已在 P1-c 删除）。

### 8.2 要删的 6 个成员（`SuperMediaPlayer.h`）

> **行号已按 P1-a 之后校正**：P1-a 把 `Discontinuity` 结构体从 5 行扩到 34 行，
> 所以 `.h` 里 110 行之后的声明整体下移 **+30**。`.cpp` 里 §8.4 的行号**不受影响**，
> 因为 P1-a 的改动全部落在 11774 行之后。

`mSeekRenderGateUs`(`:1299`)、`mSeekLandingFrameAccepted`(`:1300`)、
`mSeekLandingFloorOwnerUs`(`:1663`)、`mSeekFirstDecodableFrameShown`(`:1603`)、
`mSeekExactLandingByBudget`(`:1393`)、`mSeekPositionFloorUs`(`:1156`)。
**保留** `mSeekExactLanding`(`:1302`，用户选项，不在本次删除范围)。

### 8.3 renderer 单一过滤（替换 `RenderVideo()` 的整块落点逻辑）

删掉 `SuperMediaPlayer.cpp:8011-8358`（从"落点帧即上屏（K1）"注释块起，到
`} else if (mSeekExactLanding) { render = false; }` 那段结束），以单一规则取代。

**注意**：判据本体已经在 **P1-a 里实现完毕**，就是
`SuperMediaPlayer::shouldDropForDiscontinuity(framePos, frameDur)`
（声明 `SuperMediaPlayer.h:1815`，定义 `SuperMediaPlayer.cpp:11831-11910`）。
所以 P1-b 的 `RenderVideo()` 只需要**调用**它，不要再把判据内联一遍：

```cpp
    /*
     * renderer 单一过滤：判据本体在 shouldDropForDiscontinuity() 里（P1-a 已实现）。
     * 这里只做三件事：把帧的"位置 + 帧长"喂给它、处理"第一帧无条件出画"这条例外、
     * 以及在它结束过滤时收掉"追帧加速"并复位两个 revert 闩。
     */
    if (mDiscontinuity.filterActive.load()) {
        int64_t frameTimePos = videoFrame->getInfo().timePosition;
        if (frameTimePos < 0) {
            frameTimePos = videoFrame->getInfo().pts;   /* 容器不填 timePosition 的退回 */
        }
        int64_t frameDurUs = videoFrame->getInfo().duration;
        if (frameDurUs <= 0) {
            const int fps = (mCurrentVideoMeta != nullptr)
                            ? std::max(1, (int) (mCurrentVideoMeta->operator Stream_meta *()->avg_fps)) : 25;
            frameDurUs = 1000000 / fps;
        }

        /*
         * 【顺序不能反】必须**无条件**先调一次：判据在"包含目标"时结束过滤，
         * 而"第一帧就包含目标"是完全正常的情形 —— 若因为第一帧例外而跳过调用，
         * 过滤就永远不会结束。所以先拿判据结果，再用第一帧例外覆盖 render 决定。
         */
        const bool dropForLanding = shouldDropForDiscontinuity(frameTimePos, frameDurUs);

        if (!mDiscontinuity.firstFrameShown.load()) {
            /* flush 之后的第一帧：无条件出画（不等时钟、不丢）。 */
            mDiscontinuity.firstFrameShown = true;
            force_render = true;
            render = true;
            AF_LOGI("seek first frame shown: pts=%lld afterSeekMs=%lld\n", ...);
        } else if (dropForLanding) {
            render = false;   /* 完全在目标之前 ⇒ 不上屏，继续解 */
        } else {
            force_render = true;
            render = true;    /* 包含目标（或第一帧就晚于目标）⇒ 强制上屏、过滤已结束 */
        }

        if (!mDiscontinuity.filterActive.load()) {
            /* 判据刚刚结束了本次过滤（或本来就没激活）：收尾一次，幂等。 */
            setVideoDecodeBoost(false);
            mVideoPtsRevert = false;
            mAudioPtsRevert = false;
            mPtsRevertWaitLogged = 0;
        }
    } else if (!mSeekDecodeStartIsKey && mSeekFlag) {
        /*
         * 解码不是从关键帧起步（demuxer 落点在 GOP 中间）⇒ 这一帧缺参考帧、是脏帧：
         * 不上屏，保持上一张好画面。这条与 seek 无关，是**保留**的花屏保护。
         */
        render = false;
    }
```

`beginDiscontinuity()` 已经在 P1-a 里补齐（激活/复位过滤与基准，`SuperMediaPlayer.cpp:11781-11809`），
P1-b 不需要再动它。P1-b 仍要做的两件事：删掉 `mSeekDecodeStartIsKey` 相关的**旧**落点分支
（上面那段 `else if` 是新写的等价保护），以及把 `mSeekExactLanding` 的旧语义分支一并去掉。

### 8.4 P1 必须逐个改掉的代码点（行号为 P0 之后的当前值，已 grep 确认）

| 文件:行 | 现在写的是 | 改成 |
|---|---|---|
| `SuperMediaPlayer.cpp:720` | `mSeekRenderGateUs = seekTargetUs;` | 删（由 `beginDiscontinuity` 置过滤闩） |
| `SuperMediaPlayer.cpp:729` | `mSeekLandingFrameAccepted = false;` | 删（并入 `beginDiscontinuity`） |
| `SuperMediaPlayer.cpp:733` | `mSeekFirstDecodableFrameShown = false;` | 删（同上） |
| `SuperMediaPlayer.cpp:765` | `mSeekExactLandingByBudget = false;` | 删 |
| `SuperMediaPlayer.cpp:786-788` | 地板 + 归属写入 | 删 |
| `SuperMediaPlayer.cpp:1253-1270` | `getCurrentPosition()` 的地板兜底链 | 换成 `targetUs + (now - mDiscontinuityBaseMs)`；管道实际位置超过它时把基准回收过去（保持单调）；`mCurrentPos` 仅在 `targetUs == INT64_MIN`（不连续点未激活）时使用 |
| `SuperMediaPlayer.cpp:1858` | `inSeekWindow = (mSeekFlag \|\| mSeekRenderGateUs != INT64_MIN)` | `(mSeekFlag \|\| mDiscontinuityFilterActive)` |
| `SuperMediaPlayer.cpp:3827` | 同上的三元或 | `(mPausedSwitchRenderPending \|\| mSeekFlag \|\| mDiscontinuityFilterActive)` |
| `SuperMediaPlayer.cpp:4177` | `(mSeekFlag \|\| mSeekRenderGateUs != INT64_MIN) && !mSeekDecodeStartIsKey` | `(mSeekFlag \|\| mDiscontinuityFilterActive) && !mSeekDecodeStartIsKey` |
| `SuperMediaPlayer.cpp:5227` | `!mSeekLandingFrameAccepted && flags != 0` | 按 §8.3（该处 `markDiscontinuityStartUs` 保留） |
| `SuperMediaPlayer.cpp:5304-5305` | 音频锚点 `mSeekPositionFloorUs` 优先 | `mDiscontinuity.targetUs`（P2 会连同 C 方案一起删） |
| `SuperMediaPlayer.cpp:5321-5325`、`:5423` | 日志用 `mSeekPositionFloorUs` | `mDiscontinuity.targetUs` |
| `SuperMediaPlayer.cpp:7662` | `: mSeekPositionFloorUs` | `: mDiscontinuity.targetUs` |
| `SuperMediaPlayer.cpp:7798` | `seekLandingPending = (gate && !accepted)` | `mDiscontinuityFilterActive` |
| `SuperMediaPlayer.cpp:8011-8358` | 整块落点逻辑 | 按 §8.3 替换 |
| `SuperMediaPlayer.cpp:8473-8483`、`:8647` | 追赶阀门以 `mSeekRenderGateUs == INT64_MIN` 为界 | `!mDiscontinuityFilterActive` |
| `SuperMediaPlayer.cpp:11111-11113` | Reset 清地板/归属 | 删（`beginDiscontinuity(INT64_MIN)` 已覆盖） |
| `SuperMediaPlayer.cpp:11137-11142`、`:11157` | Reset 清闸门/落点闩 | 删 |
| `SuperMediaPlayer.cpp:11334-11407` | `ResetSeekStatus()` 里"落点窗口出口"整段 + `mSeekAnchorPending` 作废判据 | **整段删**：seek 结束不再碰过滤（本条最关键）；保留 `setVideoDecodeBoost(false)` 与 B17 重装 |
| `SuperMediaPlayer.cpp:11469-11470` | `fetchSeekClockAnchorUs()` 取 `mSeekPositionFloorUs` | `mDiscontinuity.targetUs`（P2 会连该函数一起删） |
| `SMPMessageControllerListener.cpp:720-721` | `mSeekPositionFloorUs = seekPos; mSeekRenderGateUs = seekPos;` | 删（`SeekTo` 已开新代际），该处注释同步改述 |
| `SMPMessageControllerListener.cpp:698`、`:711-713`、`:906`、`:1002` | 注释提到被删的名字 | 改述 |
| `QtPlayer/src/CicadaPlayerItem.cpp:2517`、`:3459` | 注释提到 `mSeekPositionFloorUs` | 改述（**只改注释，不动任何 UI 代码**） |
| `framework/demuxer/dash/DashStream.cpp:1205` | 注释提到 `mSeekPositionFloorUs` | 改述 |
| `SuperMediaPlayer.h:501-504`、`:1126`、`:1256-1270`、`:1359-1363`、`:1573`、`:1588`、`:1599`、`:1625-1633` | 声明与说明 | 随 6 个成员一起删 / 改述 |
| `SuperMediaPlayer.cpp:226-241`、`:1263`、`:1368`、`:3459`、`:4053`、`:7086`、`:7432`、`:7453`、`:7529`、`:8004-8040`、`:8100`、`:8389`、`:9762-9776`、`:11337-11381`、`:11442` | 提到被删名字的注释 | 改述（P5 的零残留 grep 会检查） |

`SEEK_TARGET_DROP_AHEAD_US` / `SEEK_FLOOR_GIVEUP_US` / `SEEK_EXACT_LANDING_BUDGET_US`
三个宏在 §8.3 替换后**再无读者**，一并删除（定义在 `SuperMediaPlayer.cpp:218/226/241`，
另有多处注释引用）。

### 8.5 P1 验收标记

* 每次 seek 都有 `seek landing frame accepted … offsetFromTarget=±(≤1 帧)`；
* 不再出现"落点窗口被提前关闭"（§8.4 里 `ResetSeekStatus()` 那一段被删掉就是这一条）；
* 位置不再回退（数学单调）。

### 8.6 P1-a 完成记录（纯增量，零行为变化）

| 文件:行 | 改动 |
|---|---|
| `SuperMediaPlayer.h:111-139` | `Discontinuity` 结构体扩 5 个字段：`filterActive` / `firstFrameShown` / `clockBaseUs` / `clockBaseSteadyMs` / `acceptedFramePos`（全 atomic，语义与并发理由写在结构体内注释里）**——其中 `firstFrameShown` 已在 P1-c 删除，后两个"墙钟"字段已在 P2.1 删除（见 §11）** |
| `SuperMediaPlayer.h:1785-1815` | 新增方法声明 `bool shouldDropForDiscontinuity(int64_t framePos, int64_t frameDur);`（追加在方法列表末尾） |
| `SuperMediaPlayer.cpp:11781-11809` | `beginDiscontinuity()`：新增"复位 acceptedFramePos / firstFrameShown / clockBase*，并 `filterActive = (targetUs != INT64_MIN)`"（**`firstFrameShown` 与 `clockBase*` 的复位已分别随 P1-c / P2.1 删除**） |
| `SuperMediaPlayer.cpp:11823-11910` | 新增 `shouldDropForDiscontinuity()` 定义（含两条 `seek landing frame accepted …` 日志） |
| `docs/SINGLE-DECODER-REFACTOR.md` | §8.1/§8.2/§8.3 按实际实现校正 + 本节 |

**删除的符号：无。** `shouldDropForDiscontinuity()` 本阶段**还没有调用点**（P1-b 才接上），
这是设计上的"纯增量"——它不影响任何既有判据，所以构建后行为与 P0 逐字一致。

自检要点：

* `shouldDropForDiscontinuity` 声明（`.h:1815`）/ 定义（`.cpp:11831`）齐全；调用点 = 0（P1-a 预期）。
* 5 个新字段全部有写入点（`beginDiscontinuity` + `shouldDropForDiscontinuity`），无"只声明未使用"。
* 无未使用变量：`generation` 在两条日志里都被使用；`frameDur` 形参被就地修正使用。
* 格式串与实参逐一核对：`"pts=%lld, offsetFromTarget=%+lld ms, generation=%d, afterSeekMs=%lld … target=%lld"`
  对应 `(long long)framePos, (long long)(offset/1000), generation(int), (long long)(afterSeekMs), (long long)targetUs`。
* C++11 合法性：`std::atomic<bool>` / `std::atomic<int64_t>` 的 NSDMI 走转换构造；`Discontinuity{}` 仍走隐式默认构造。
* 注释内无"星号紧跟斜杠"组合。
* 新字段都在 `Discontinuity` 结构体内部 ⇒ `SuperMediaPlayer` 的成员布局**不变**（结构体仍只有一个成员 `mDiscontinuity`），
  所以**增量构建依然安全**；真正需要全量重编的是 P1-b（删 6 个成员）。
* 残留计数：**不变**（仍是 1235 行 / 26 文件）。P1-a 新增的标识符
  （`shouldDropForDiscontinuity` / `filterActive` / `firstFrameShown` / `clockBase*` / `acceptedFramePos`）
  与残留清单里的 35 个模式**无一匹配**，且新增日志文本 `seek landing frame accepted`
  是"seek landing"（带空格），不匹配模式 `seekLanding`。

## 九、P2 施工图（音频基准重设 + 删除"扣住 PCM"与锚点闩）

目标（用户原话）：**seek 后必须有声音、音视频用同一个目标点、位置不回弹、不卡顿**。
红线不变：无墙钟死线/超时/看门狗、不"精度换流畅"、不新增配置开关、不碰 UI/QML、
L1 核心不引平台宏、新数据成员追加类末尾、新虚函数追加 vtable 末尾、注释不含"星号紧跟斜杠"。

### 9.1 新判据的完整形态 —— 音频时钟 = 目标点基准 + 设备已消费量

**用的既有接口**（无需新增虚函数）：`SMPAVDeviceManager::getAudioRenderPosition()`
→ `IAudioRender::getPosition()` → `filterAudioRender::getPosition()`
→ `device_get_position()`，各平台实现：

| 平台 | 实现 | 语义 |
|---|---|---|
| Android | `framework/render/audio/Android/AudioTrackRender.cpp:434` | `mOverFlowPlayedSimples + getDevicePlayedSimples() - mAudioFlushPosition`，按 `sample_rate` 折算成微秒 —— **AudioTrack.getPlaybackHeadPosition() = 设备已消费样本数**；flush 后该差值重新起算 |
| Apple | `Apple/AFAudioUnitRender.cpp:468`、`Apple/AFAudioQueueRender.cpp:344` | 同一 `device_get_position()` 契约（设备已消费 → 微秒） |
| SDL / OHOS / Cheater | `SdlAFAudioRender.cpp:200`、`SdlAFAudioRender2.cpp:139`、`OHOS/OhosAudioRender.cpp:117`、`CheaterAudioRender.cpp:48` | 同上 |

即"设备已消费字节/采样数"这个量**本工程已有唯一接口**，P2 不再追加虚函数。

**新字段**（放进 `Discontinuity`，与 P1 的 `clockBase*` 同一处；结构体是单一成员 ⇒ 私有成员偏移不变）：

```cpp
        std::atomic<int64_t> audioBaseUs{INT64_MIN};          // 音频时钟的内容位置基准
        std::atomic<int64_t> audioBaseConsumedUs{INT64_MIN};  // 写基准那一刻"设备已消费"的快照（微秒）
```

**唯一读点** `getAudioPlayTimeStamp()`（主时钟的音频参考）：

```cpp
    if (!mAVDeviceManager->isAudioRenderValid()) return INT64_MIN;
    const int64_t baseUs = mDiscontinuity.audioBaseUs.load();
    if (baseUs == INT64_MIN) return INT64_MIN;               /* 本次不连续点还没有音频基准 */
    const int64_t consumedUs = mAVDeviceManager->getAudioRenderPosition();
    if (consumedUs < 0 || af_clock_value_is_unset(consumedUs)) return INT64_MIN;
    int64_t baseConsumedUs = mDiscontinuity.audioBaseConsumedUs.load();
    if (baseConsumedUs == INT64_MIN) {                        /* 建立基准时设备位置还不可用：惰性补锚 */
        mDiscontinuity.audioBaseConsumedUs = consumedUs;
        return INT64_MIN;
    }
    const int64_t delta = consumedUs - baseConsumedUs;
    if (delta <= 0) return INT64_MIN;                         /* 设备还没消费/向后跳 ⇒ 音频时钟暂不可用 */
    return baseUs + delta;                                    /* 单位：微秒，与 targetUs 同一根轴 */
```

**两个写点**：

1. `FlushAudioPath()`（设备 flush 之后、`mPlayedAudioPts` 被清零之前）：不连续点上把基准
   钉在 `mDiscontinuity.targetUs`（= 用户目标点，与视频同一目标点）；非 seek 的 flush 保持
   "内容位置连续"（用旧基准 + 本次消费增量外推）。快照取 `getAudioRenderPosition()`。
2. `getAudioPlayTimeStamp()` 里的惰性补锚（上表 `baseConsumedUs == INT64_MIN` 那支）。

**暂停行为**：设备暂停 ⇒ `getPlaybackHeadPosition()` 不再增长 ⇒ `delta` 不变 ⇒ 位置恒定，
**不需要任何额外冻结判据**（ExoPlayer 的 `AudioTrackPositionTracker` 同形）。
主时钟只此一个来源：有音频 = 上表；无音频 = `acceptDiscontinuityLandingFrame()` 把
`mMasterClock` 钉在 `targetUs`（P1 已有）。

### 9.2 要删 / 要改的文件:行表（施工时的行号，实测后以 §10 为准）

| 文件:行（施工前） | 现在写的是 | 改成 |
|---|---|---|
| `SuperMediaPlayer.h:523-545` | `fetchSeekClockAnchorUs()` 声明 + 锚点规则注释 | **整段删** |
| `SuperMediaPlayer.h:897` | `mAudioClockReanchorPending` | 删 |
| `SuperMediaPlayer.h:1282-1290` | `mSeekClockAnchored` | 删 |
| `SuperMediaPlayer.h:1304-1309` | `mSeekVideoAnchorDone` | 删 |
| `SuperMediaPlayer.h:1337-1341` | `mSeekAudioFloorUs` | 删 |
| `SuperMediaPlayer.h:1342-1356` | `mSeekAudioContinuityUs` | 删 |
| `SuperMediaPlayer.h:1357-1364` | `mSeekAudioStaleDrops` | 删（随判据一起失去全部读点） |
| `SuperMediaPlayer.h:1379-1387` | `mSeekAnchorPending` | 删 |
| `SuperMediaPlayer.h:1397-1409` | `mSeekAudioRepositionPending/Us/JumpUs` | 删 |
| `SuperMediaPlayer.h:1541-1569` | `mAudioClockProgress*` | 删（新判据自带"设备已消费"基准快照，不需要第二条"进度观察"真相） |
| `SuperMediaPlayer.h` | `Discontinuity` 末尾 | 增 `audioBaseUs` / `audioBaseConsumedUs` |
| `SuperMediaPlayer.cpp:218-255` | `SEEK_AUDIO_CONTINUITY_TOLERANCE_US` / `SEEK_AUDIO_STALE_DROP_MAX` | 删（唯一使用者在 9.2 被删代码里） |
| `SuperMediaPlayer.cpp:724-757` | `SeekTo()` 里被删成员的复位 | 删（`beginDiscontinuity` 覆盖）；`mSeekAudioAlignDone` 保留复位 |
| `SuperMediaPlayer.cpp:2038-2101` | `DoCheckBufferPass` 的 reposition 消费点（`SeekStream`） | 整段删 |
| `SuperMediaPlayer.cpp:3903-4003` | `doRender` 的视频锚点块（`mSeekAnchorPending`/`fetchSeekClockAnchorUs`） | 整段删（主时钟已由 `ProcessSeekToMsg` 与落点采纳钉在目标点） |
| `SuperMediaPlayer.cpp:5295-5410` | C 方案音频对齐（`audioAnchorUs` / `mSeekAudioFloorUs` / 高水位 / 裁剪） | 只留 B3（`mActiveVideoPtsOffset`）与 1b（清帧队列 + 重基 `mAudioTime`），锚点改成 `mDiscontinuity.targetUs` |
| `SuperMediaPlayer.cpp:7144-7311` | `holdAudioForSeek` / `inSeekAudioWindow` / 静音门日志 | 整段删；音频无条件推（`render()` 直接循环 `RenderAudio()`） |
| `SuperMediaPlayer.cpp:7377-7460` | `RenderAudio` 的 `mSeekAudioFloorUs` 双侧地板 | 换成一条：落点过滤仍激活时丢掉"完全在目标点之前"的音频帧 |
| `SuperMediaPlayer.cpp:7462-7512` | `RenderAudio` 的音频锚点块 | 整段删 |
| `SuperMediaPlayer.cpp:7582-7646` | 首帧音频的 `mSeekVideoAnchorDone`/`mAudioClockReanchorPending` 交接 | 换成"把音频基准钉在目标点（无目标点则钉在这一帧的内容位置）+ 交参考时钟" |
| `SuperMediaPlayer.cpp:7709-7718` | `mSeekAudioReposition*` 置闩 | 删 |
| `SuperMediaPlayer.cpp:9301-9336` | `FlushAudioPath()` | 增"音频基准重设"（唯一写点 1） |
| `SuperMediaPlayer.cpp:9894-10026` | `getAudioPlayTimeStamp()` | 按 9.1 重写（唯一读点） |
| `SuperMediaPlayer.cpp:10898-10902`、`:10967-10987` | `Reset()` 里被删成员的复位 | 删（`beginDiscontinuity(INT64_MIN)` 已作废音频基准） |
| `SuperMediaPlayer.cpp:11188-11196` | `ResetSeekStatus()` 提到 `mSeekAnchorPending` 的注释 | 改述：锚点闩群随 P2 删除 |
| `SuperMediaPlayer.cpp:11225-11292` | `fetchSeekClockAnchorUs()` 定义 | 整段删 |
| `SMPMessageControllerListener.cpp:770-777` | `mSeekAnchorPending = true;` | 删（不连续点已由 `beginDiscontinuity` 建立） |
| `SMPMessageControllerListener.cpp:963-977` | 提到 `mSeekAudioAlignDone` 的 C 方案注释 | 改述为"音频与视频同一目标点" |

**保留（不得误删）**：`NotifyVideoQualitySwitch` 生命周期、`isVideoDecoderMetaMatched()`、
`rebuildVideoDecoder()`、`CreateVideoDecoder()`、`FlushVideoPath()`、`mCurrentVideoIndex`、
ABR 接口、JNI/Java 签名、音频 keep-alive 写静音保活、`mSeekExactLanding`、`mActiveVideoPtsOffset`（B3）、
`logAudioSilence`/`mAudioSilenceReason`（设备侧 reason=2/3/4 仍在使用）、`SEEK_AUDIO_*` 之外的宏。

### 9.3 只有一根内容时间轴（P2.1 起：位置上报**读的就是**它）

**写死这条不变量**：本内核只有**一根**内容时间轴 —— 单位微秒，轴就是 `targetUs` 所在的
`timePosition` 轴；它的**唯一取值口**是 `mMasterClock.GetTime()`
（`SystemReferClock::GetTime()`）。P2 时位置上报还在用它自己的一根墙钟轴（上表 #1），
P2.1 已把两者合并 —— 现在**没有第二个产生"内容时间"的地方**：

| # | 消费者 | 形态 | 内容时间从哪来 |
|---|---|---|---|
| 1 | 位置上报（`getCurrentPosition()`） | `mMasterClock.GetTime()`（`targetUs != INT64_MIN` 时；否则逐字用 `mCurrentPos`） | 见下表的"谁写" |
| 2 | 渲染节拍 / 音频参考 | `mMasterClock.GetTime()`（同一个口） | 同上 |
| 3 | 音频参考本身（`getAudioPlayTimeStamp()`） | `audioBaseUs + (设备已消费 - audioBaseConsumedUs)` | `FlushAudioPath()` 在 seek 的 flush 之后把它钉在 **targetUs** |

**这根轴谁写（全部是 pin，不产生第二条轴）**：

| 写点 | 值 | 位置 |
|---|---|---|
| `ProcessSeekToMsg()` | `seekPos`（钳位后的目标点） | `SMPMessageControllerListener.cpp:690`、`:1066` |
| `acceptDiscontinuityLandingFrame()` | `targetUs`（落点被采纳时再钉一次，幂等） | `SuperMediaPlayer.cpp:11210` |
| `SystemReferClock` 的 reSync | 音频参考（`getAudioPlayTimeStamp()`），**只有偏差 > 100ms 才采纳** | `system_refer_clock.cpp:44-45` |
| 纯视频片源首帧 / 无有效参考时 | 该帧 pts（既有 guard：`!mSeekFlag && !mPausedSwitchRenderPending`） | `SuperMediaPlayer.cpp:7522`、`:7926` |

**单位是微秒，代码证据**（P2.1 核对）：
`SystemReferClock::GetTime()` = `af_scalable_clock::get()`
= `mSetTime + af_clock::get() * scale`（`framework/utils/af_clock.cpp:133-136`），
而 `af_clock::get()` 的两个来源都是微秒 —— `af_gettime_relative()` 是 `steady_clock` 的
**微秒**值（`framework/utils/timer.cpp:25-28`），暂停时返回冻结的 `mPauseUs`
（`af_clock.cpp:60-75`）。`mSetTime` 由 `setTime()` 写入，写入的就是 `seekPos` / `targetUs`
（微秒，与 `timePosition` 同轴）。

**暂停 / 缓冲停顿行为**（P2.1 的核心收益）：这根轴不吃墙钟 ——
· **暂停**：`startRendering(false)` → `mMasterClock.pause()` → `af_clock::get()` 返回冻结值；
  即使音频参考可用，`SystemReferClock::GetTime()` 也只在 `!mClock.isPaused()` 时才去取它
  （`system_refer_clock.cpp:16`）⇒ 位置恒定。
· **缓冲停顿**（时钟被 pause、设备没有消费）：有音频时内容时间由设备已消费量驱动 ⇒ 不前进；
  时钟被 pause 时同理。⇒ `NotifyPosition` **不再跑到画面/声音前面**（这正是 P2.1 要修的
  用户可见问题：seek 或切档后一次缓冲停顿，旧实现的墙钟照走，而"回收"只处理墙钟落后）。
· **恢复播放**：时钟 `start()` 后按 1×（或 `scale`）继续前进，位置随之继续前进。
· **坏值**：`GetTime()` 返回负值/哨兵（`af_clock_value_is_unset`）时退回 `mCurrentPos`。

**旧的第三条基准（位置地板 / 音频地板 / 锚点值）与两个墙钟字段已全部删除，不允许再出现。**

**音频基准的失效点只有两处**，且都对应"音频路真的被重建"：

* **seek 在途**：`getAudioPlayTimeStamp()` 用 `mSeekFlag` 判"参考暂不可用"（避免拿上一段播放的
  残留位置去 reSync）；随后本次 seek 的 `FlushAudioPath()` 把基准重钉到 `targetUs`。
* **`Reset()`**（换片源 / 停止 / Prepare）：显式把基准作废，由首次起播的音频帧兜底重钉。

**切档不是失效点**：`switchVideo()` 也调 `beginDiscontinuity()`，但音频**不动**（不 OpenStream、
不 flush、时间轴连续）。若把切档也算成"音频基准失效"，音频参考时钟就会在每次切档后
**永久不可用**（一直返回 `INT64_MIN`），主时钟退回自走、音画基准漂移 —— 所以
`beginDiscontinuity()` 刻意**不碰** `audioBase*`。

**音频侧的两个对齐点**（必须同时成立，否则会回弹或漂移）：

* **(a) 轴与单位**：音频的"已消费量"由 `getAudioRenderPosition()` 按采样率折算成微秒返回
  （媒体时间，不是墙钟时间），加上同样微秒的基准 ⇒ 与 `targetUs` 同一根轴。
* **(b) 快照与 flush 配对**：`FlushAudioPath()` 的基准快照必须在 **`flushDevice()` 之后**取
  （顺序反了快照就是旧的大值，`delta` 立刻为负）；非 seek 的 flush（切档 / stop / 后台）
  必须保持**内容位置连续**（旧基准 + 本次消费增量外推），**不要**把它也钉到某个目标点上。

### 9.4 `delta < 0` 的结构性自愈（最终代码）

设备已消费量向后跳 = "设备被 flush 过但基准快照没跟着重设"（例如某条 flush 路径没走到
`FlushAudioPath` 的写点）。只 `return INT64_MIN` 会让快照**永远**大于真实已消费量 ⇒ `delta`
永远为负 ⇒ 音频时钟永久不可用，所以必须就地重新取快照：

```cpp
    const int64_t delta = consumedUs - baseConsumedUs;

    if (delta < 0) {
        /* 设备侧被重设过 ⇒ 重新取快照，本次不可用。这不是兜底，是"基准快照必须与设备
         * flush 配对"这条不变量缺少写点时的自愈；判据是纯值比较，无计时器、无阈值。 */
        mDiscontinuity.audioBaseConsumedUs = consumedUs;
        return INT64_MIN;
    }

    if (delta == 0) {
        /* 刚锚定、设备还没消费：位置与基准相同，交出去没有信息量。 */
        return INT64_MIN;
    }

    return baseUs + delta;
```

### 9.5 P2 验收标记

* 不再出现 `audio silence starts (reason=1 …)`（那道"扣住 PCM 等时钟"的门已删除）；
  **设备侧的 reason=2/3/4 照旧**（`logAudioSilence` 与 `mAudioSilenceReason` 保留，
  reason=1 这个编号也保留，因为它与 `AudioTrackRender.cpp` 共用一套编号）；
  P2 的验收就是"reason=1 不再出现、reason=2/3/4 该出现时仍出现"。
* 不再出现 `audio keep-alive: reached the resource cap`；
* `audio first frame after seek … afterSeekMs=` ≤ 200（seek 后立刻有声：音频的推进与
  视频是否出帧无关，且不再被任何门扣住）；
* 新增可对账日志（按**代际**限频，一个 seek 至多一条）：
  `audio landing drop: dropped=… pos=… target=… generation=…`
  —— 用来区分"音频被正确地丢到目标点"与"音频根本没来"；
* 位置不回弹（P2.1 起位置上报与渲染节拍读的是同一根内容时间轴；音频基准与它同轴同基准）。

### 9.6 P2.1 验收标记（位置上报与音频时钟合并成同一来源）

* seek 之后（以及每次切档之后）**若发生缓冲停顿**，`NotifyPosition` **不得**继续前进
  （与画面/声音一致）；缓冲出口恢复后继续前进 —— 这是旧墙钟模型做不到、P2.1 要修的那一条。
* 暂停时位置恒定（沿用 P1-c 已确立的性质）。
* 位置单调不回退（除用户主动 seek 造成的跳变）。
* seek 后位置 == 目标点（`ProcessSeekToMsg()` 钉 `seekPos` + 落点采纳再钉 `targetUs`）。
* `getCurrentPosition()` 与渲染节拍**不存在第二根轴**：代码里不再有"基准 + 墙钟增量"的位置
  计算（§5 之外的自查 grep：`grep clockBase` 在 `mediaPlayer/framework/platform` 的
  `*.h/*.cpp/*.mm` 里为 0）。

---

## 十、P2 完成记录（已完成实现；构建与真机验收待用户执行）

### 10.1 删除的符号（全仓代码引用 = 0，仅剩"删除说明"注释）

`mSeekAudioFloorUs`、`mSeekAudioContinuityUs`、`mSeekAudioStaleDrops`、
`mSeekAudioRepositionPending` / `mSeekAudioRepositionUs` / `mSeekAudioRepositionJumpUs`、
`mSeekAnchorPending`、`mSeekClockAnchored`、`mSeekVideoAnchorDone`、
`mAudioClockReanchorPending`、`mAudioClockProgressBaseUs` / `mAudioClockProgressSeen` /
`mAudioClockProgressLogCount`、局部量 `holdAudioForSeek` / `inSeekAudioWindow` /
`seekHoldDisabled` / `clockNotAdvancing`、函数 `fetchSeekClockAnchorUs()`、
宏 `SEEK_AUDIO_CONTINUITY_TOLERANCE_US` / `SEEK_AUDIO_STALE_DROP_MAX`。

### 10.2 新增的符号

| 符号 | 位置 | 形态 |
|---|---|---|
| `Discontinuity::audioBaseUs` | `SuperMediaPlayer.h:152` | `std::atomic<int64_t>{INT64_MIN}` |
| `Discontinuity::audioBaseConsumedUs` | `SuperMediaPlayer.h:153` | `std::atomic<int64_t>{INT64_MIN}` |
| `pinAudioClockBase(int64_t)` | 声明 `SuperMediaPlayer.h:1753`，定义 `SuperMediaPlayer.cpp:8897` | 唯一写点封装 |
| `mAudioLandingDropLoggedGen` | `SuperMediaPlayer.h:1763` | 落点丢弃日志的代际限频（纯状态） |

**没有新增虚函数**：设备已消费量用既有接口 `SMPAVDeviceManager::getAudioRenderPosition()`
→ `IAudioRender::getPosition()` → 各平台 `device_get_position()`。

### 10.3 实际改动表（行号为 P2 完成后的实测值）

| 文件:行 | 改动 |
|---|---|
| `SuperMediaPlayer.h:111-153` | `Discontinuity` 末尾增 `audioBaseUs` / `audioBaseConsumedUs` + 完整语义/并发说明 |
| `SuperMediaPlayer.h:523-545`（删） | `fetchSeekClockAnchorUs()` 声明与锚点规则注释整段删除 |
| `SuperMediaPlayer.h:897`、`1282-1290`、`1304-1309`、`1322-1349`、`1364-1372`、`1382-1394`、`1541-1570` | 被删成员及其说明整段删除（含 `mAudioClockProgress*` 那套"第二套观察基准"） |
| `SuperMediaPlayer.h:1753`、`1763` | 新增 `pinAudioClockBase()` 声明与 `mAudioLandingDropLoggedGen` |
| `SuperMediaPlayer.cpp:231-241` | 两个音频宏删除，留"为什么删"的说明 |
| `SuperMediaPlayer.cpp:708-730`（`SeekTo()`） | 被删成员的复位删除；只留 `mSeekAudioAlignDone` 复位 + P2 说明 |
| `SuperMediaPlayer.cpp:2011-2035`（`DoCheckBufferPass` 前） | "音频按流重定位"消费点（`SeekStream`）整段删除 |
| `SuperMediaPlayer.cpp:3815-3840`（`doRender`） | 视频锚点块（事件闩 + 只锚一次闩 + 共用判据）整段删除 |
| `SuperMediaPlayer.cpp:5129-5255`（`DecodeVideoPacket`） | 一次性对齐事件的锚点改为 `mDiscontinuity.targetUs`；保留 B3（`mActiveVideoPtsOffset`）与 1b 重基 |
| `SuperMediaPlayer.cpp:2515-2530` | "清迟到音频"阈值改为"落点过滤激活时用 `targetUs`，否则 `mSoughtVideoPos`" |
| `SuperMediaPlayer.cpp:6925-6945`（`render()`） | `holdAudioForSeek` / `inSeekAudioWindow` / 静音门日志整段删除；音频无条件推 |
| `SuperMediaPlayer.cpp:7016-7093`（`RenderAudio`） | 双侧地板换成"同一目标点"判据 + 按代际限频的 `audio landing drop` 日志 |
| `SuperMediaPlayer.cpp:7152-7205`（`RenderAudio`） | 音频锚点块删除；新增"基准兜底钉住 + 交参考时钟" |
| `SuperMediaPlayer.cpp:7214-7250` | `>1s` 前跳分支：不再置重定位事件、不再重锚时间轴（只记日志） |
| `SuperMediaPlayer.cpp:8834-8910` | `FlushAudioPath()` 增基准重设（快照在 flush 之后）+ 新增 `pinAudioClockBase()` 定义 |
| `SuperMediaPlayer.cpp:9478-9580` | `getAudioPlayTimeStamp()` 按 9.1/9.4 重写（含 `delta<0` 自愈与 `mSeekFlag` 退回） |
| `SuperMediaPlayer.cpp:10430-10440`（`Reset()`） | 音频基准显式作废；被删成员的复位删除 |
| `SuperMediaPlayer.cpp:10690-10705`（`ResetSeekStatus()`） | 注释改述：seek 结束不碰任何时间轴权威 |
| `SuperMediaPlayer.cpp:11078-11105`（`beginDiscontinuity()`） | 明确**不碰**音频基准（切档也要走它），写明两个失效点 |
| `SuperMediaPlayer.cpp:10862-10752`（`fetchSeekClockAnchorUs` 定义） | 整段删除，留删除说明 |
| `SMPMessageControllerListener.cpp:771-782` | `mSeekAnchorPending = true;` 删除 + P2 说明 |
| `SMPMessageControllerListener.cpp:965-982` | "seek in cache"里的 C 方案注释改述为"同一个目标点" |

### 10.4 音频时钟的新数据流（谁写 / 谁读 / 单位 / 暂停）

```
写（唯一主写点）  FlushAudioPath()                —— flushDevice() 之后取快照
                  · seek 且有目标点 ⇒ baseUs = mDiscontinuity.targetUs
                  · 其它 flush      ⇒ baseUs = 旧 baseUs + 本次消费增量（内容位置连续）
                  · 信息不足        ⇒ baseUs = 最后一次已渲染音频帧的内容位置 / INT64_MIN
                  同时写 audioBaseConsumedUs = getAudioRenderPosition()（无效则 INT64_MIN）
写（兜底）        RenderAudio() 首帧分支：baseUs == INT64_MIN 时 pinAudioClockBase(pts)
写（自愈）        getAudioPlayTimeStamp()：delta < 0 时就地重取快照
作废              Reset()（换片源/停止/Prepare）。
                  seek 在途由 getAudioPlayTimeStamp() 的 mSeekFlag 退回覆盖（见 9.3）。
读（唯一）        getAudioPlayTimeStamp() → SystemReferClock 的参考时钟回调
                  → 供 getCurrentPosition()/渲染节拍/落点判据使用
单位              微秒；轴 = targetUs 所在的 timePosition 轴
暂停行为          设备暂停 ⇒ 已消费量不再增长 ⇒ delta 不变 ⇒ 位置恒定；无需额外冻结判据
```

### 10.5 自检

* 9 个 P1 名字（§5 命令的前 9 项）全仓命中 **0**（Task A 已由父代理实测确认）。
* P2 删除的成员/函数/宏代码引用 **0**（`grep` 只剩"删除说明"注释）。
* 每个新增符号都有写点与读点，无"只声明未使用"；新数据成员追加在类末尾；
  **无新增虚函数**；L1 核心（`mediaPlayer/`、`framework/`）未引入平台宏；
  注释内无"星号紧跟斜杠"的组合（`[^ \t]\*/` 的 4 处命中全部是既有的单行注释：
  `MediaPlayerConfig.h:29`、`:36`、`SuperMediaPlayer.cpp:2854`、`:8539`）。
* **残留计数（§5 命令的等价 grep，按匹配行数）**：`mediaPlayer 760 + framework 114 +
  platform 0 = **≈874**`（P1-c 之后父代理实测 1124；Task A 清掉约 30 条注释命中，
  P2 再清掉约 170 条 —— 其中 `mSeekAudioFloorUs` / `mSeekAudioContinuityUs` /
  `mSeekAudioReposition` / `mSeekAnchorPending` / `mSeekClockAnchored` /
  `mSeekVideoAnchorDone` / `mAudioClockReanchorPending` / `holdAudioForSeek` 同时也是
  §5 模式表里的项）。剩余全部是 P3（双解码器/pending/占位面/切档死线）与
  P4（demuxer 落点暂存 `seekLanding*` / `mSeekLandingStage`、
  `SeekInCache`、`DECODE_STALL_REBUILD_ROUNDS`）的范围。**父代理的 §5 命令为准。**
* **构建要求：本阶段必须全量重编**（与 P1-b 同理）。P2 在成员列表**中间**删除了十余个
  成员、并新增了 `mAudioLandingDropLoggedGen` 与 `Discontinuity` 的两个字段
  ⇒ 其后成员的偏移全部改变。请删掉 `platform/QtPlayer/build` 下各 `media_player.dir`
  的 `.obj`（或整个 build 目录）后再构建，否则会按旧偏移访问。

> **上面 §10.5 里的残留计数 ≈874 已被父代理用 §5 命令实测修正为 992（P1-c 后 1124 → 992）。**
> 我的等价 grep 少了一批 `mQualitySwitch*` / `mSwitch*` / `b2*` 等模式，**以后一律以 §5 命令为准，
> 不用简化 pattern 估数**。以下 P2.1 的改动只删除两个字段与一段墙钟逻辑（不含 §5 模式），
> 所以 P2.1 之后的 §5 计数应与 992 基本持平（少 0~1 条）。

---

## 十一、P2.1 完成记录（位置上报与音频时钟合并成同一来源）

### 11.1 为什么必须合并（用户可见的故障）

P1-b 的位置上报是"不连续点基准 + **墙钟**增量"，P2 的音频参考时钟是"目标点 + **设备已消费量**"
—— 两个机制、两根轴。后果：seek（或每次切档）之后只要发生一次缓冲停顿，墙钟照走而设备没有
消费 ⇒ **进度条跑到画面与声音前面**；当时的"回收"分支只处理 `byClock < mCurrentPos`（墙钟落后），
对"墙钟超前"无能为力。暂停冻结那套（冻结哨兵 + 恢复重锚 + 回收）本质是在墙钟模型上补洞。

### 11.2 做法（删机制，不是加机制）

`getCurrentPosition()` 改成从**唯一那根内容时间**读出：

```cpp
    if (isSeeking()) return mSeekPos;                 /* 保留：seek 在途返回钳位后的目标 */
    归一化 mCurrentPos（0..duration）                  /* 保留 */
    if (mDiscontinuity.targetUs != INT64_MIN) {       /* 曾经建立过不连续点 */
        int64_t contentUs = mMasterClock.GetTime();   /* 唯一内容时间：微秒 */
        if (contentUs < 0 || af_clock_value_is_unset(contentUs)) contentUs = mCurrentPos.load();
        if (mDuration > 0 && contentUs > mDuration) contentUs = mDuration;
        return contentUs;
    }
    return mCurrentPos;                               /* 从未 seek 过：逐字不变 */
```

### 11.3 删除的符号（`grep clockBaseUs|clockBaseSteadyMs` 在 `mediaPlayer/framework/platform`
的 `*.h/*.cpp/*.mm` 里 = **0**，连注释也不再有这两个名字）

`Discontinuity::clockBaseUs`、`Discontinuity::clockBaseSteadyMs`
（以及 `getCurrentPosition()` 里那整段"墙钟 + 暂停冻结 + 回收"代码、
`acceptDiscontinuityLandingFrame()` 与 `beginDiscontinuity()` 里对它们的写入）。

### 11.4 唯一内容时间轴：谁写 / 谁读 / 单位 / 暂停与停顿

| 项 | 内容 |
|---|---|
| **载体** | `mMasterClock`（`SystemReferClock` → `af_scalable_clock`），唯一取值口 `mMasterClock.GetTime()` |
| **写** | ① `ProcessSeekToMsg()` → `setTime(seekPos)`（`SMPMessageControllerListener.cpp:690`、`:1066`）；② `acceptDiscontinuityLandingFrame()` → `setTime(targetUs)`（`SuperMediaPlayer.cpp:11210`，幂等）；③ `SystemReferClock` reSync 到音频参考（仅偏差 > 100ms，`system_refer_clock.cpp:44-45`）；④ 纯视频/无有效参考时首帧 pts（`SuperMediaPlayer.cpp:7522`、`:7926`，既有 guard `!mSeekFlag && !mPausedSwitchRenderPending`） |
| **读** | ① `getCurrentPosition()`（位置上报，`SuperMediaPlayer.cpp:1206`）；② `RenderVideo` 的迟到判定等渲染节拍路径；③ 音频参考本身由 `getAudioPlayTimeStamp()` 给出（`audioBaseUs + 设备已消费 - 快照`） |
| **单位** | **微秒**；轴 = `targetUs` 所在的 `timePosition` 轴。证据：`af_scalable_clock::get() = mSetTime + af_clock::get()*scale`（`framework/utils/af_clock.cpp:133-136`），`af_clock::get()` 用 `af_gettime_relative()`（`framework/utils/timer.cpp:25-28`，steady_clock **微秒**），暂停时返回冻结的 `mPauseUs`（`af_clock.cpp:60-75`） |
| **暂停** | `startRendering(false)` → `mMasterClock.pause()` ⇒ `get()` 返回冻结值；且 `SystemReferClock::GetTime()` 只在 `!mClock.isPaused()` 时才取音频参考（`system_refer_clock.cpp:16`）⇒ 位置恒定 |
| **缓冲停顿** | 有音频 ⇒ 内容时间由设备已消费量驱动，设备不消费就不前进；时钟被 pause 时同理 ⇒ `NotifyPosition` 不再前进（恢复播放后继续） |
| **坏值** | `GetTime()` 返回负值/哨兵（`af_clock_value_is_unset`）⇒ 退回 `mCurrentPos`（帧驱动的管道位置） |
| **从未 seek 过** | `targetUs == INT64_MIN`（Reset / Prepare 之后）⇒ 逐字走旧的 `mCurrentPos` 路径，行为不变 |

### 11.5 实际改动表

| 文件:行 | 改动 |
|---|---|
| `SuperMediaPlayer.cpp:1206-1277`（`getCurrentPosition()`） | 整段重写：删墙钟 + 暂停冻结 + 回收分支，改用 `mMasterClock.GetTime()`（含坏值退回与 duration 钳位） |
| `SuperMediaPlayer.h:111-134`（`Discontinuity`） | 删两个墙钟字段及其说明，留"P2.1 删除"记录；`filterActive` / `acceptedFramePos` 保留 |
| `SuperMediaPlayer.h:1719-1739`（`acceptDiscontinuityLandingFrame` 声明注释） | 改述为"把内容时间轴钉在 targetUs" |
| `SuperMediaPlayer.h:1127-1135`（"seek 位置地板"历史说明） | 改述为指向唯一内容时间轴 |
| `SuperMediaPlayer.cpp:11061-11082`（`beginDiscontinuity()`） | 删两个字段的复位，改述；`audioBase*` 仍**不**在这里作废 |
| `SuperMediaPlayer.cpp:11188-11211`（`acceptDiscontinuityLandingFrame()`） | 删两个字段的写入，保留 `mMasterClock.setTime(targetUs)` |
| `SuperMediaPlayer.cpp:750-756`、`:10473-10480`、`:10496-10498`、`:10708-10715`、`:11172-11176` | 位置相关注释全部改述（seek 入口 / Reset / ResetSeekStatus / 落点收尾） |
| `SMPMessageControllerListener.cpp:707-713` | `beginDiscontinuity` 的职责说明改述（不再是 `clockBase*` 复位） |
| `docs/SINGLE-DECODER-REFACTOR.md` §9.3 / §9.6 / §11 | 不变量改写成"位置上报读的就是唯一内容时间轴"；新增 P2.1 验收标记与完成记录 |

### 11.6 自检

* `clockBaseUs|clockBaseSteadyMs`：`mediaPlayer/framework/platform` 的 `*.h/*.cpp/*.mm` 命中 **0**
  （注释也改述完毕）。
* 顺着这条口径，P2 的"删除说明"注释里仍然**点名**的 6 行也一并改成不点名的描述
  （`mSeekAudioFloorUs` / `mSeekAudioContinuityUs` / `mSeekAnchorPending` /
  `mSeekClockAnchored` / `mSeekVideoAnchorDone` / `mAudioClockReanchorPending` —— 这 6 个名字同时
  也在 §5 的模式表里）。现在**P0/P1/P2/P2.1 删除的全部符号在全仓（含注释）命中 0**：
  `grep 'mSeekRenderGateUs|…|clockBaseUs|clockBaseSteadyMs|mAudioClockProgress|mSeekAudioStaleDrops'`
  → No matches。P2.1 之后 §5 计数预计在父代理实测的 992 基础上下调约 6。
* 未新增/删除任何虚函数，未新增数据成员（P2.1 只删字段）；未引平台宏；未加配置开关；
  未引入任何超时/看门狗；注释无"星号紧跟斜杠"。
* `getCurrentPosition()` 的访问顺序沿用 P2 约定：先 `mDiscontinuity.generation.load()`
  （seq_cst，acquire）与 `beginDiscontinuity()` 的 `fetch_add` 配对，再读非原子的 `targetUs`
  —— 与 `shouldDropForDiscontinuity()` 完全一致。
* **构建要求：仍需全量重编**（`Discontinuity` 结构体尺寸再次变化）。

---

## 十二、P3 施工图（单解码器切档：立即切换路径 A + 删尽双解码器）

### 12.1 机制选择：路径 A（立即切换），证据行号

按父代理拍定实现 **路径 A**（不复用 demuxer 分片边界交接、不保留 pending 机制）。可行性已用代码事实核对：

| 事实 | 位置 | 结论 |
|---|---|---|
| `OpenStream(index)` 只把目标流置 `selected = true` 并 start，**不取消旧流的 selected** | `framework/demuxer/play_list/HLSManager.cpp:327-374`（`:365` `i->selected = true`） | ⇒ 路径 A **必须**先 `CloseStream(old)`，否则两路包混进同一条队列（这正是双解码器能工作、也让单解码器不可用的原因） |
| `CloseStream(id)` 置 `selected = false` 并 `stop()` 旧流 | `HLSManager.cpp:398-422`（`:414`/`:417`） | ⇒ 旧 rendition 当场停止投递，单解码器不再收到旧流数据 |
| `seek(us, flags, index)` 对 **video index 只 seek 那一路**（`index == -1` 才走 "seek all"），且**完全不碰音频流** | `HLSManager.cpp:442-567`（`:480` 的 `index == -1` 分支 vs `:562-567` 的按 id 单流 seek） | ⇒ 切档的 `Seek(switchPos, 0, newIndex)` **不会重定位音频** ⇒ 声音连续（"切档后没声音"的一类根因在结构上消失） |
| DashManager 同形 | `framework/demuxer/dash/DashManager.cpp:319`（Open）、`:557-578`（`SwitchStreamAligned` 被改成只写 `toStreamId`） | 同上 |

理由（对应 §2 规则 4）：① 立即生效，不依赖 manager 的分片边界交接语义 ⇒ 满足"切换时间必须短 / 100% 成功"；
② 复用的正是 P1 已经做准的 seek 路径（落点精度由 `shouldDropForDiscontinuity` 承担）；
③ 音频流不动 ⇒ 切档期间声音连续；④ 单解码器 ⇒ 占位面 / 双 surface 那一整套在结构上不再需要。

### 12.2 新模型（file:line 级时序）

```
① 记录目标档 + 切换点
   SMPMessageControllerListener::switchVideoStream()：
       mVideoSwitchInFlight = true;  mVideoSwitchTargetIndex = index;   /* 单一"挂着的切换请求" */
       switchPos = mPlayer.getCurrentPosition()  ← 唯一内容时间轴（P2.1：mMasterClock）
       NotifyVideoQualitySwitch(STARTED, index, "quality switch started")
② 关旧流 → 开新流 → 按流定位到切换点           SuperMediaPlayer::SwitchVideo(switchPos)
       CloseStream(mCurrentVideoIndex)         /* 旧 rendition 当场停止投递 */
       OpenStream(mVideoSwitchTargetIndex)     /* 失败 ⇒ FAILED + 清 in-flight（不动播放） */
       Seek(switchPos / 1000 * 1000, 0, mVideoSwitchTargetIndex)   /* 只 seek 这一路，音频不受影响 */
③ 只 flush 视频：FlushVideoPath(false, false, __func__)
       （视频解码器 + 解码帧队列 + 目标点之前的陈旧视频包；**不碰音频**）
④ 用目标流 meta 重建**同一块**解码器（任何时刻只有一块、无占位面）
       GetStreamMeta(meta, target, true) → isVideoDecoderMetaMatched(meta)
         匹配   ⇒ 什么都不做（③ 的 flush 已经复位解码器）
         不匹配 ⇒ invalidateDecoder(VIDEO) + CreateVideoDecoder(false, meta)
⑤ 提交"当前档"并武装单一过滤（**必须在 ③/④ 之后**，否则旧流的帧会被当成落点帧）
       mCurrentVideoIndex = mVideoSwitchTargetIndex
       mSeekDecodeStartIsKey = false            /* 脏帧保护重新武装：新流首帧必须来自关键帧 */
       beginDiscontinuity(switchPos)            /* 唯一落点过滤接管：新流首个上屏帧 = 包含 switchPos 的那一帧 */
⑥ 落点帧被采纳 ⇒ READY
       acceptDiscontinuityLandingFrame()：if (mVideoSwitchInFlight) finishQualitySwitch(true, "quality switch rendered")
```

**为什么不会花屏 / 不会快进**：新流解码从 demuxer 按流 seek 的落点（关键帧 ≤ switchPos）开始；
`mSeekDecodeStartIsKey = false` 让"非关键帧起步的脏帧不上屏"这条保护覆盖切档路径（P3 要求 3）；
落点过滤保证 switchPos 之前的帧一律不上屏 ⇒ 不会"从头扫一遍"。

**新成员**（追加在类末尾，符合本文件约定）：

| 成员 | 语义 |
|---|---|
| `bool mVideoSwitchInFlight{false}` | 是否有挂着的切档请求（唯一判据，`qualitySwitchInFlight()` / `IsStreamSwitchInFlight()` 都返回它） |
| `int mVideoSwitchTargetIndex{-1}` | 目标档索引（替代 `mQualitySwitchCommittedStreamIndex` 的入口作用） |

### 12.3 兼用语义替换表（逐条落实）

| § | 被删成员 | 兼着的语义 | 替代 |
|---|---|---|---|
| 5.1 | `mPausedSwitchRenderPending` | "暂停态切档不锚主时钟"的两处守卫（`SuperMediaPlayer.cpp:7512-7523`、`:7919-7927`） | 改成 `!mDiscontinuity.filterActive.load() && !mSeekFlag`（更强：同时覆盖 seek 在途与切档） |
| 5.2 | `mQualitySwitchCommittedStreamIndex` | READY 入口 + 日志索引 | `mVideoSwitchTargetIndex`（in-flight 时）/ `mCurrentVideoIndex`（提交后） |
| 5.2 | `mWillChangedVideoStreamIndex` / `mPendingVideoStreamIndex` | "切档在途"判据（ABR 让路、read-ahead 门、日志） | `qualitySwitchInFlight()`（= `mVideoSwitchInFlight`）——ABR 让路本来就是读 `IsStreamSwitchInFlight()` |
| 5.2 | `mSwitchStartedWhilePaused` / `mPausedSwitch*` | 暂停态切档"补做欠帧" | 落点帧渲染本身就是补做：`filterActive` 转假时该帧已被采纳并上屏 |
| 5.2 | `mRetiredVideoStreamIndex` | "旧流何时 CloseStream" | ② 立即 `CloseStream(old)`（单解码器不需要旧流继续供帧） |
| 5.2 | 四个死线常量 | 超时兜底 | **直接删**：单解码器 + 落点过滤没有"等不到"的状态 |

### 12.4 要删的符号（`P3-BOUNDARY-SWITCH-NOTES.md` 第四节 + §3 清单）

`SMPAVDeviceManager.{h,cpp}`：`mPendingVideoDecoder` / `mRetiredVideoDecoder` / `getPendingVideoDecoder()` /
`isPendingVideoDecoderValid()` / `setUpPendingVideoDecoder()` / `getPendingVideoFrame()` /
`sendPendingVideoPacket()` / `invalidatePendingVideoDecoder()` / `discardPendingVideoDecoder()` /
`promotePendingVideoDecoder()`（含槽位交换）/ `releaseRetiredVideoDecoder()`。
`SuperMediaPlayer.{h,cpp}`：`CreatePendingVideoDecoder` / `DecodePendingVideoPacket` / `FillPendingVideoFrame` /
`DrainPendingVideoFrames` / `TryCommitPendingVideoSwitch` / `attachPendingVideoCodecParams`、
`mPendingVideo*`、`mRetiredVideo*`、`mWillChangedVideoStreamIndex`、`mQualitySwitch*`、`mSwitchReArm*`、
`mPausedSwitch*`、`mSwitchStartedWhilePaused`、B2/B2' 占位面（`b2Placeholder`/`b2SurfaceOutput`/`b2RealSurface`）、
`mPendingVideoSwitchTimePosition`、宏 `QUALITY_SWITCH_TOTAL_TIMEOUT_MS` / `DECODE_STALL_REBUILD_ROUNDS` /
`PENDING_VIDEO_STALL_CHECKS_MAX` / `PENDING_PREROLL_WAIT_MAX_MS`。
**保留**：`NotifyVideoQualitySwitch(...)` 生命周期、`isVideoDecoderMetaMatched()`、`rebuildVideoDecoder()`、
`CreateVideoDecoder()`、`FlushVideoPath()`、`mCurrentVideoIndex`、ABR 接口与
`ICicadaPlayer::IsStreamSwitchInFlight()`、JNI/Java 签名。

### 12.5 验收标记（P3）

* 切档只有 `status=0(STARTED) → status=1(READY)`；手动与 ABR 都成功（失败才 `2/3`）；
* 日志里不再出现：`[switch] state=decoderSwitch`、`pendingPktQ`/`pendingFrameQ`、`pending preroll`、
  `placeholder surface handover`、`quality switch timed out`、`wait a key frame` 风暴、
  两块 4K 实例 `-1010`、`video decoder accepts no input … rebuilding it once`；
* 切档期间**声音连续**（音频流未被 seek/flush）；切档后位置不回弹（落点过滤挡掉 switchPos 之前的帧）；
* 切档不花屏（新流从关键帧起解 + 脏帧不上屏）；
* 切档耗时只有 "OpenStream + 按流 Seek + flush + 一次 meta 比较"（无预热等待、无分片边界等待）。

### 12.6 P3 阶段性完成记录（机制已落地；物理清理由后续收尾完成）

**本轮已实现（路径 A 机制，已可运行）**：

| 文件:行 | 改动 |
|---|---|
| `SuperMediaPlayer.h:1766-1787` | 新增新模型的两个成员 `mVideoSwitchInFlight` / `mVideoSwitchTargetIndex`（追加在成员列表末尾，`mDiscontinuity` 仍保持最后） |
| `SuperMediaPlayer.cpp:9391-9520`（`SwitchVideo()`） | **整段重写为路径 A**：`CloseStream(旧)` → `OpenStream(新)` → `Seek(switchPos,0,新)` → 提交 `mCurrentVideoIndex` → `FlushVideoPath(false,false,…)` → 按流 `DropPacketsByStream(VIDEO, 旧)` → `isVideoDecoderMetaMatched()` 不匹配才 `rebuildVideoDecoder(false)`（**同一块**）→ `mSeekDecodeStartIsKey = false` → `beginDiscontinuity(switchPos)`；三个失败出口（Open/Seek/meta/重建）是**唯一**的 FAILED 来源 |
| `SuperMediaPlayer.cpp:3160-3166`、`:3213-3218`（`finishQualitySwitch()`） | 终态前置条件换成 `mVideoSwitchInFlight`；索引改用 `mVideoSwitchTargetIndex`；终态清零两个新成员 |
| `SuperMediaPlayer.cpp:11280-11292`（`acceptDiscontinuityLandingFrame()`） | 新增 READY 出口：落点帧被采纳 + `mVideoSwitchInFlight` ⇒ `finishQualitySwitch(true, "quality switch rendered")` —— 单解码器切档**唯一**的成功出口 |
| `SuperMediaPlayer.cpp:10260-10270`（`qualitySwitchInFlight()`） | 从 9 个 pending 成员的并集改为 `return mVideoSwitchInFlight;`（`IsStreamSwitchInFlight()`/ABR 让路随之只认这一个判据） |
| `SuperMediaPlayer.cpp:7489-7491`、`:7897-7899` | §5.1 的两处 S6 守卫由 `!mPausedSwitchRenderPending` 改为 `!mDiscontinuity.filterActive.load()`（更强：同时覆盖 seek 在途与切档） |
| `SMPMessageControllerListener.cpp:1401-1449`（`switchVideoStream()`） | 换成新模型入口：置 in-flight + 目标档 → `switchPos = getCurrentPosition()`（唯一内容时间轴）→ `NotifyVideoQualitySwitch(STARTED)` → `SwitchVideo(switchPos)`；删掉 `mWillChangedVideoStreamIndex`/pending 判据与旧的 `mMasterClock.GetTime()` 起点计算 |

**本轮未完成的物理清理（按 §12.4，留给后续收尾）**：pending/retired 机器**全部成员与函数仍在源码里**，
但已被新路径**结构性旁置**（视频切档不再写 `mWillChangedVideoStreamIndex`/`mPendingVideoStreamIndex`，
所以 `SuperMediaPlayer.cpp:4680-4689` 的 `CreatePendingVideoDecoder` 分支不再可达）。仍需删除：
`SuperMediaPlayer.{h,cpp}` 的 `CreatePendingVideoDecoder`/`DecodePendingVideoPacket`/`FillPendingVideoFrame`/
`DrainPendingVideoFrames`/`TryCommitPendingVideoSwitch`/`attachPendingVideoCodecParams`、`mPendingVideo*`、
`mRetiredVideo*`、`mQualitySwitch*`、`mSwitchReArm*`、`mPausedSwitch*`、`mSwitchStartedWhilePaused`、
B2/B2' 占位面、四个死线常量，以及 `SMPAVDeviceManager.{h,cpp}` 的 pending/retired 槽位与 promote 交换。

**收尾时必须一并处理的一处纠缠（本轮新发现，必须记录）**：
`SuperMediaPlayer.cpp:8391-8418` 那条 "muxed 流里自动选择视频子流" 的路径仍然会写
`mWillChangedVideoStreamIndex = streamId`（条件：`mDuration < 0 && mMainStreamId != -1 && id != mMainStreamId
&& streamId != mCurrentVideoIndex`）。它是 pending 机器的**第二个激活入口**（起播时 `mCurrentVideoIndex`
可能是 -1 或不同子流），所以"删净 pending 机器"必须先把这条改成单解码器语义（直接
`CloseStream(旧子流)/OpenStream(新子流)` 或复用 `SwitchVideo`），否则删了成员这条路径会编译不过或被
裸删成行为回退 —— 这正是 §12.4 说的"不得裸删"那一类。**先定这条，再删成员。**



## 十三、P4 完成记录（落点延迟线收敛成一份；构建 0/0）

**做了什么**：解复用器里那条"落点延迟线"原先在 `HLSStream` 与 `DashStream` 里**各写了一份**（同形、约 50 行代码 + 60 行注释 ×2）。
现在抽成一份共享实现 `framework/demuxer/SeekLandingStage.{h,cpp}`，两个 demuxer 各持一个 `SeekLandingStage mSeekLanding{mQueue, mDataMutex, mWaitCond};`
（**追加在各自类成员列表末尾**）并调用它；各自那份拷贝的成员、函数与重复注释全部删除
（`HLSStream.cpp` 2308→2032 行、`DashStream.cpp` 1800→1517 行）。`framework/demuxer/CMakeLists.txt` 显式列入新源文件
（该 target 不用 file GLOB —— 静态库链接不会暴露未定义符号，这一步必须做；父代理已确认 `SeekLandingStage.obj` 真的产出）。

**为什么必须保留它（而不是当残留删掉）**：它把解码起点从"分片片首（≤10s 之前）"挪到
**"不晚于目标的最后一个关键帧"**，即把"seek 后必须解码前推的帧数"从分片长度量级降到关键帧间隔量级。
删掉它 = 分片源 seek 变慢（用户抱怨过的"seek 后很慢"会回来），属于"精度换流畅"的反面。
**它不是第二个精度权威**：即使它交接早了（多给了前缀帧），renderer 的 `shouldDropForDiscontinuity()` 也会把这些帧丢掉。
这句话已写进 `SeekLandingStage.h` 的类注释。

**行为逐字等价**（P4 的硬要求，逐条对照）：(a) 只有 `timePosition ≤ 目标` 的包进延迟线；
(b) 读到 `timePosition > 目标` ⇒ 丢掉"最后一个 ≤ 目标的关键帧"之前攒下的整批，从它起（含当前包）按原序交出；
(c) 包没有 `timePosition` ⇒ ABANDON（已收下的按原序交出，只慢不错）；(d) "本片读完 / 停在本片界"两个切换点整批交出；
(e) 任何情况下不丢目标帧，**无任何计时器**（"攒前缀时不要每包白等 10ms"的判据是 `consumeProgress()` 这个进度事实）。
加锁语义也与原实现**逐字相同**（`flush()` 锁宿主 `mDataMutex`，且原实现同样在 `filter()` 的 ABANDON/RELEASE 分支里调它）——
父代理用 `git show` 对比过，没有引入新的死锁面。

**日志**：前缀仍是字面 `[seekLanding]`（既有 grep 照旧命中），来源紧随其后（`[seekLanding] HLS armed: …` / `[seekLanding] DASH armed: …`），
12 条消息文本/级别与"每轮各状态一条 + 限频"不变。

**效果**：`seekLanding`/`mSeekLandingStage` 命中 190 → **18**（12 在共享实现、3 在其头文件、3 是内核注释）；
§5 验收命令仍为 **0**；MSVC 构建 **0 错误 0 警告**。

## 十四、P5 终检记录（静态门 + 构建 + 验收）

**构建**：`cmake --build platform\QtPlayer\build\msvc-static --target media_player --config Release` → **0 error / 0 warning**
（MSVC 14.39.33519）。本阶段消掉的三条警告：`C4005 AV_DISPOSITION_ATTACHED_PIC` 宏重定义（把 `AFMediaType.h` 的写法对齐
ffmpeg 的 `(1 << 10)` —— MSVC 比较的是记号序列而非取值）、`C4244`（`MediaPlayerAnalyticsUtil.cpp` 的 float→int64 显式化）、
`C4305`（`AbrBufferRefererData.cpp` 的 `return -1` → `false`）。

**验收命令**：§5（32 个模式、`-CaseSensitive`）→ **0**。

**工程硬约束静态门**（对本阶段**全部 +3210 新增行**逐条 grep，而不是全仓）：

| 门 | 结果 |
|---|---|
| 平台宏（`__ANDROID__`/`__APPLE__`/`_WIN32`/OHOS/`TARGET_OS_`/`__linux__`/`_MSC_VER`） | **0**（唯一命中在文档表格里） |
| 新虚函数 `virtual` | **0** ⇒ vtable 未动 |
| 新增配置开关（`setOption`/`options::SET|REPLACE`/`addValue`） | **0** |
| 计时器/看门狗（`af_msleep(≥100)`/`usleep`/`watchdog`/`*_TIMEOUT_MS`/`*_DEADLINE_`） | **0**（命中全是文档里"已删除"的记述） |
| 注释内"星号紧跟斜杠" | 修掉最后一处字面违反（`SuperMediaPlayer.cpp` 的 `/*mAdaptiveVideo &&*/`，改成上一行普通注释），现为 0 |
| UI/QML | 未碰逻辑；`platform/QtPlayer/src/CicadaPlayerItem.cpp` 只改了两处注释 |
| 新成员追加类末尾 | P3 的 `mVideoSwitchInFlight`/`mVideoSwitchTargetIndex`、P4 的 `mSeekLanding` 均为追加；`mDiscontinuity` 仍是最后成员 |

**必须全量重编的理由**：P1/P2/P2.1/P3 在成员列表中间删过成员、并追加过成员 ⇒ 旧 `.obj` 按旧偏移编译会错位。
构建步骤见 `docs/HANDOVER-P0-P3.md` 第二节。

**仍需用户执行**：真机（Qt / Android / iOS / HarmonyOS）构建 + 按 `docs/HANDOVER-P0-P3.md` 第三节的日志标记回归
（seek 落点 `offsetFromTarget`、位置单调、暂停/缓冲时位置恒定、音频 `afterSeekMs ≤ 200` 与 `audio landing drop` 的
`target` 与视频落点一致、切档只有 `status=0 → 1` 且不再出现 pending/占位面/死线日志）。**行为结论只能由这些日志判定**，
父代理能给出的只是"编译与静态规则的证据"。

---

## 十五、Android 解码数据面迁移（新任务，独立排期）

本重构（P0–P5）解决的是**时间轴/落点/切档**的语义问题。Android 侧**每帧数据面仍在 Java `MediaCodec` 上跨 JNI**，
且**节拍依赖 Java 的 `onInputBufferAvailable` 回调** —— 这与 `ARCH-REDESIGN.md:120` 的红线
（"内核不得依赖 Java 侧回调做节拍"）冲突，是另一条独立战线。

- **已修（commit `99cfd107`）**：异步模式下 `flush()` 之后**必须再调 `start()`**（AOSP 契约原文：
  flush 后所有 index 作废、缓冲归 codec，异步模式不调 start 则 codec 不会请求输入缓冲）。
  否则回调永久停摆 ⇒ 解码器拿不到落点关键帧 ⇒ **画面永久卡死**
  （真机 `async input path looks dead / no-input-callback-after-flush`，两次命中）。
  两份 SDK 同步修，顺序为"先作废旧 index，再 `start()`"。记录见
  `PLAN-SEEK-FAST-LANDING-CROSSPLATFORM.md` §8.5 第 2 条。
- **待做**：数据面迁到 NDK `AMediaCodec` 异步回调（`setAsyncNotifyCallback`，API 28+；
  `__attribute__((weak_import))` 兼容 minSdk 24；API 24–27 保留现有 Java 异步实现；
  **不引入同步轮询**；固定 NDK r25c，不升级 r28）。设计、API 级别核实表、线程模型、
  分阶段验收与回退判定见 **`ANDROID-NDK-ASYNC-DECODER.md`**。

**对本重构的影响：无。** 迁移只替换 L0 实现与平台绑定：`IDecoder` 接口、单解码器模型、
落点过滤、`Discontinuity` 成员与 §5 验收命令全部不变。迁移完成后，`sAsyncBroken`
（"下个实例降级轮询"）作为轮询路径的一部分一并删除。

---

## 十六、帧队列上限按"实际硬解/软解"赋值（2026-09-27：补一个从未被赋值的成员）

**事实核查**：`mPictureCacheType`（`SuperMediaPlayer.h:721`，声明处默认
`picture_cache_type_cannot`）在全仓**只有声明与 `doDeCode()` 的一处读**
（`SuperMediaPlayer.cpp:3435`），**没有任何赋值点** ⇒ `doDeCode()` 恒取 `max_cache_size = 1`，
连软解也被迫"解一帧→渲染一帧"，丢掉了 `VIDEO_PICTURE_MAX_CACHE_SIZE`(=2) 本意提供的
解码/渲染重叠（既有文档 `platform/QtPlayer/docs/ANALYSIS-QUALITY-SWITCH-FREEZE.md:358`
把它列为待确认问题："决定帧队列上限是 1 还是 2"）。

**修法**（唯一视频解码器创建点 `CreateVideoDecoder()`，紧跟 `setUpDecoder()` 成功之后）：

```cpp
mPictureCacheType = IsVideoDecoderHardware() ? picture_cache_type_cannot : picture_cache_type_soft;
```

- 用 `IsVideoDecoderHardware()`（问**活动解码器对象** `IDecoder::isHardwareDecoderInUse()`，
  含 FFmpeg 运行期降级）而不是请求参数 `bHW`：请求硬解但实际落软解时，也必须按"实际是软解"
  放行流水线；
- **硬解侧行为完全不变**（仍是上限 1）：硬解交出去的每一帧都持有平台 buffer index，必须尽快
  release —— B5-4 的"释放代"机制正是按"至多一帧在飞"设计的。这是**正确性优先**，不是性能妥协；
- 软解侧帧是深拷贝（`AVAFFrame`，`AVAFPacket.cpp:188-193`），多预放 1 帧安全；落点过滤仍逐帧
  判定，**不改变**"首个上屏帧 = 包含目标的那一帧"这条精度契约（属"加流水线"，不是"精度换流畅"）；
- 新增一条 INFO 便于真机核对取值与理由：
  `video frame queue cap: pictureCacheType=… maxCacheSize=… (hardware=…)`。

**为什么现在才动**：Android NDK 数据面迁移的真机验证即将进行，本次只改软解侧，
硬解（被测路径）零变化，避免把两个变量混在一次验证里。

---

## 十七、硬解帧的帧长（落点判据的输入）必须由解码器补上（2026-09-27）

**事实核查**：落点判据是 `framePos + frameDur > target ⇒ 这一帧包含目标`
（`shouldDropForDiscontinuity()`）。帧长的来源按解码器分两类：

- 软解（FFmpeg）：`framework/base/media/AVAFPacket.cpp:225` `mInfo.duration = mAvFrame->duration`
  ⇒ **有真值**；
- 硬解（Android `AFMediaCodecFrame`）：全仓 `.duration =` 的赋值点里**没有它**
  （`framework/base/media/AFMediaCodecFrame.h` 只有 index/pts 类成员）⇒ **帧长恒 0**。

⇒ 硬解路一直走 `SuperMediaPlayer` 的"标称帧率"兜底；而兜底原先把 fps 钳到 ≥1.0，
fps 也拿不到时帧长 = 1 秒，会把目标前近 1 秒的**前缀帧**判成"包含目标"而上屏 ——
直接违反"首个上屏帧必须是包含目标时刻的那一帧"（P1–P4 一直在治的那类症状）。

**两处修**：

1. **帧的出生地补真值**（`framework/codec/Android/mediaCodecDecoder.cpp`，视频分支）：
   `if (duration <= 0 && mMeta.avg_fps > 1.0) duration = 1000000 / avg_fps`
   —— 只填 0 值、**不覆盖**解码器给的真值；CFR 内容下等于真实帧长，判据因此走**精确分支**；
2. **兜底不再猜**（`SuperMediaPlayer.cpp` 落点帧长段）：fps 拿到就换算；两者都拿不到时取 **0** ⇒
   判据退化为"丢掉所有 `framePos <= target` 的帧、接受第一个 `framePos > target` 的帧"，
   **最坏只晚一帧**，绝不会把 1 秒前的前缀帧当落点。这一档会由既有的**一次性**落点日志
   以 `dur=0` 直接暴露（真机一看就知道有没有走到）。

**已知残量（写明以便后面收口）**：帧长与帧率**都**拿不到时，合同要求的
"取不晚于目标的最后一帧"需要**前瞻一帧**（把落点候选按住到下一帧到达再决定）。
本轮**故意不动**落点过滤的状态机（它刚在 P1–P4 定稿、正等真机验证）：
等真机日志确认 `dur=0` 这档是否真会出现，再决定是否实现"落点窗口内按住一帧"。