# HLS 实测日志分析 + DASH/HLS 对照（双路解码切换）

> 对象：`hls_log.txt`（2026-09-21 19:24:54 → 19:25:21，`http://127.0.0.1:9000/video/master.m3u8`，
> 用户点了一次 2160p，11 秒后放弃关窗；**进程正常退出，没有崩溃**）
> 配套：`ANALYSIS-QUALITY-SWITCH-FREEZE.md`（DASH 那份日志的分析，本文简称「DASH 报告」）
>
> 结论先行：**HLS 这次是真的播放起来了（前 6 秒 44~60fps），切换卡死与 DASH 是同一个内核 bug；
> 但 HLS 又额外暴露了两条 DASH 那份日志里没有的、更严重的协议侧缺陷。**

---

## 1. HLS 这次到底发生了什么（一句话版）

解析成功 → 正常播了 6 秒 → 用户点 2160p → 提交时选了一个**落后主时钟 1.57 秒**的帧当锚点 →
**新开的 4K 流不是从切换点开始读，而是直接从 ~70.9 秒的分段开始读**（播放点只有 ~35 秒）→
帧队列里全是「未来 35 秒」的帧，`RenderVideo()` 的「早于时钟 10ms 不渲染」把它们全挡住 →
视频路看门狗跳两次没上屏后**拒绝再跳**（`next key pts 50.05s` vs `master 37.03s`）→ FPS 永久 0。
同时：**切换的 READY 回调从未发出**，于是 `CloseStream` / `releaseRetiredVideoDecoder` 这条收尾链整条没跑，
被退役的 480p 流一直读到 PTS 84~88 秒，每个包都在框架里刷 `unknown stream 3`（2000+ 行）。

---

## 2. HLS 日志时间线（关键行）

```
19:25:01.948 HlsParser: MasterPlaylist ; codec is "avc1.4d4034,mp4a.40.2" ×4 + ",mp4a.40.2" ×4
19:25:01.950 Demuxer service get nubmer streams is 9          ← 4×H264 + 4×HEVC(codec 为空) + 1 音频
19:25:01.951 HLSManager: OpenStream 3   (854x480 H264) / OpenStream 8 (audio)
19:25:01.978 mPtsDiscontinueDelta = 10000000                  ← 仍是 10s（与 DASH 那份一样）
19:25:02.0~02.45 起步：HLSStream 连续读 -1..-6.m4s（seqno 2..7）→ 0.45 秒读完约 7 个分段
19:25:02.256 first frame handed over to the Qt item (pts 0)
19:25:02.270 first zero-copy frame: 854x480 NV12 -> RGBA
19:25:02.991 ~ 19:25:07.990   video fps is 44 / 46 / 29 / 60 / 60 / 60 / 47   ← 播放正常
19:25:03.769 PFR: seek posUs=31780000 inCache=0 status=5      ← 用户拖到 31.78s（普通 seek 成功）
19:25:03.774 HLSManager: first seeked time is 31780000 --> 20854166
             HLSManager: second seeked time is 20854166 --> 19989333
19:25:07.549 [CicadaPlayerItem] selectQuality: index=0 (streamIndex=0)      ← 用户点 2160p
19:25:07.556 HLSManager: OpenStream 0 ; quality switch status=0: started
19:25:07.605 video stream switched to incompatible parameters (codec 1 -> 1, 854x480 -> 3840x2160)
19:25:07.667 committed seamless video decoder switch at pts=33366667 master=34941672
             audio=35328000 offset=-9223372036854775808                 ← ★ 锚点比 master 落后 1.57s
19:25:07.787 起 drop stale video packet after quality switch: stream=3 current=0 pts=37537500 …   ← 退役流包洪水
19:25:07.990 video fps is 47                                            ← 最后一帧上屏
19:25:08.025~08.037 新 4K 流打开：HLSStream: stream (0) mark startTime 70904164 / pFrame->pts 70904167
             tryOpenSegment: ...-video-h264-3840_2160-9.m4s seqno is 10  ← ★★ 直接从 70.9s 的分段开始读！
19:25:08.139 HLSStream: stream (0) mark startTime 83416664 / pFrame->pts 83416667  ← 83.4s
19:25:08.146 起 unknown stream 3, read packet pts is 84517767 …  → 一直刷到 88s+
             currentVideo=0 pendingVideo=-1 willChangeVideo=-1 retiredVideo=-1 currentAudio=8
19:25:08.991 video fps is 0                                             ← 从此永远 0
19:25:09.756 skip video forward refused: 2 catch-up jumps without a single rendered frame
             (master 37031313, next key pts 50050000)                   ← ★★ 队列比时钟超前 13s
19:25:10.760 …(master 38035477, next key 50050000)
19:25:11.767 …(master 39041780, next key 50050000)
19:25:12.758 video path stalled 4003 ms but packets are still being consumed, keep waiting
19:25:12.770 …(master 40045501, next key 54220833)
19:25:13.774 …(master 41049062, next key 54220833)
19:25:14.779 …(master 42054669, next key 54220833)
19:25:15.766 video path stalled 7011 ms but packets are still being consumed, keep waiting
19:25:15.790 …(master 43065438, next key 58391667)
19:25:16.801 …(master 44075832, next key 58391667)
19:25:17.804 …(master 45079742, next key 58391667)
19:25:18.775 video path stalled 10019 ms but packets are still being consumed, keep waiting
19:25:19.996 KPI test finish: total fps:16.2                          ← 用户放弃，正常关窗
```

**全程没有出现的三行**（这三条的缺失比出现的行更重要）：

| 缺失的日志 | 说明 |
|---|---|
| `quality switch rendered: stream=0 size=3840x2160` | 框架侧 READY 从未打印 |
| `stream switched: video index=0 3840x2160 …` / `quality switch status=1` | UI 侧也从未收到 READY ⇒ 界面一直停在「切换中」 |
| `HLSManager: CloseStream 3` / `closed retired video stream after rendered switch` | **退役的 480p 流从头到尾没有被关掉** |

---

## 3. HLS 与 DASH 的相同点（同一个内核 bug，两份日志互证）

| # | 现象 | DASH 日志 | HLS 日志 |
|---|---|---|---|
| 1 | 提交锚点是错的，且 `mActiveVideoPtsOffset` 从未建立 | `pts=20854167 master=20606253 offset=INT64_MIN`（提前 0.25s） | `pts=33366667 master=34941672 offset=INT64_MIN`（**落后 1.57s**） |
| 2 | 退役 Representation 的包留在共享视频包队列里被一个个丢 | `drop stale video packet … stream=7 current=0` ×≈370，4.4 秒 | `drop stale video packet … stream=3 current=0` ×≈500+，1.4 秒 |
| 3 | 追帧自检锁死：跳两次没上屏就拒绝再跳 | `refused … (master 24.1→28.1s, next key 41.7→45.9s)` 每秒一次 | `refused … (master 37.0→45.1s, next key 50.05→58.39s)` 每秒一次 |
| 4 | 看门狗出口只能「等包继续流动」 | `stalled 4018 ms but packets are still being consumed` | `stalled 4003 / 7011 / 10019 ms but packets are still being consumed` |
| 5 | 结果 | 画面冻死到用户关程序（FPS 0 持续 6s，总 FPS 10.5） | 画面冻死到用户关程序（FPS 0 持续 11s，总 FPS 16.2） |
| 6 | 根本原因同一处 | 解码器/队列比播放时钟**超前十几秒**，而 `RenderVideo()`「早于时钟 10ms」的帧既不渲染也不丢弃 | 同上 |

也就是说：**DASH 报告里的 RC-4（队列堆未来视频）、RC-5（两个保护互相锁死）、RC-6（追帧 flush 把关键帧也丢了）
在 HLS 上一条不差地复现了**，这两份日志互为独立证据。

---

## 4. HLS 与 DASH 的不同点（这部分是这次新挖出来的）

### 差异 1 ★`timePosition` 与 `pts` 的关系完全相反 —— 决定了两边「错位」的表现形式★

| | 分段起点是怎么来的 | 日志证据 | 后果 |
|---|---|---|---|
| **HLS** | `SegmentTracker` 用**分段里第一个包的真实 PTS** 当段起点，`time2ptsDelta = 0` | `mark startTime 20854166` ↔ `pFrame->pts 20854167`（差 1µs）；`mark startTime 70904164` ↔ `pts 70904167` | `timePosition ≡ pts`，**没有两套时间轴** |
| **DASH** | `DOMParser` 用 MPD 的**名义栅格** `(N-1)*SegmentTemplate.duration` | `open segment …, 29999999` ↔ `pFrame->pts 33366667`（差 **3.37s**）；`…, 59999999` ↔ `62562500`（差 **2.56s**） | `timePosition = pts − 2.5s~3.4s`，**且每跨一个分段跳一次** |

⇒ **DASH 报告里的 RC-2（切换窗口/提交门限把 `timePosition` 和 PTS 轴的 master 混着比）只在 DASH 上发作**；
HLS 因为两个轴一致，`TryCommitPendingVideoSwitch()` 的门限都算「对」，于是它**换了个方式出错**（见差异 2）。

> 这条也顺带解释了 DASH 那份日志里 `drop pending frame before switch window` ≈200 行 + `hold pending` ≈600 行
> 而 HLS 一份都没有：DASH 是在和一个错的窗口较劲。

### 差异 2 ★★目标 4K 流不是从切换点开始读，而是直接从 ~70.9 秒的分段开始★★（HLS 特有）

```
19:25:08.025 HLSStream: file have 1 streams / DAR 3840:2160 / open stream index is 0
19:25:08.037 HLSStream: stream (0) mark startTime 70904164 / pFrame->pts is 70904167 pos is 10048 flags is 1
19:25:08.087 HLSStream: tryOpenSegment: ...-video-h264-3840_2160-9.m4s
19:25:08.095 HLSStream: stream(...) read seg ...-3840_2160-9.m4s seqno is 10
19:25:08.139 HLSStream: stream (0) mark startTime 83416664 / pFrame->pts is 83416667
```

- 打开新 Representation 的那一刻（`OpenStream 0` @ 07.556 → 读包 @ 08.037）**只用了 ~0.5 秒就落到 `-9.m4s`（段起点 70.9s）**；
  它**没有**像 DASH 那样老老实实从 20.8s 的分段一路解过来（DASH 那份日志里新流是从 `getCurSegNum 2` = 20s 段开始的）。
- 而此刻播放点只有 ~35 秒。⇒ 队列里塞进去的全是 **35 秒之后的未来视频**。
- 这比 DASH 更狠：DASH 那份是 demuxer「读得太快」把队列读到 70s；HLS 这份是**一打开就落在 70s**。

这条与「同一份媒体、同一批 `…-3840_2160-N.m4s` 文件」（DASH 与 HLS 指的其实是同一批分片）对照起来看，
说明 **HLS 侧 `OpenStream`/`SegmentTracker` 的「当前分段游标」没有按切换点重新定位**。
（精确到「为什么是第 10 段」还需要再看一眼 `HLSStream::open` → `SegmentTracker::getCurSegPosition` 的初始化，
这份日志只能证明结果：落点在 70.9s。）

### 差异 3 ★★退役流从头到尾没有被关掉 → `unknown stream` 洪水 + 资源不回收★★（HLS 特有）

```
19:25:08.146 W: unknown stream 3, read packet pts is 84517767; currentVideo=0 pendingVideo=-1
                willChangeVideo=-1 retiredVideo=-1 currentAudio=8     ← ×2000+ 行，PTS 84.5s→88s+
```

- DASH 那份日志里 **一条 `unknown stream` 都没有**：因为 DASH 的 READY 在提交后 8ms 就发出了，
  于是 `CloseStream(7)` 执行了（`DashManager: CloseStream 7`），旧流不再产包。
- HLS 这份日志里 **`HLSManager: CloseStream 3` 一次都没出现**，`retiredVideo=-1`。
  根因是收尾链被绑在 READY 上：
  ```cpp
  // SuperMediaPlayer::RenderVideo()  ~:3571
  if (mQualitySwitchCommitPending && mQualitySwitchCommittedStreamIndex >= 0 &&
      mQualitySwitchOldFramesPending == 0) {      // ← 需要"有一帧上屏"
      ... NotifyVideoQualitySwitch(READY) ...
      if (mRetiredVideoStreamIndex >= 0 && ...) mDemuxerService->CloseStream(mRetiredVideoStreamIndex);
      mAVDeviceManager->releaseRetiredVideoDecoder();
  }
  ```
  而这次 **一帧都没上屏**，READY 没发，于是：
  ① 旧 480p 的 `HLSStream` 保持 `selected=true`，继续被读到 PTS 84~88 秒（播放点才 35 秒）；
  ② 它产出的每个包都掉进 `ProcessVideoPacket()` 最后的 `else` 分支，打 `unknown stream` 并空转；
  ③ `releaseRetiredVideoDecoder()` 不跑 ⇒ 旧解码器 + 它的 D3D11 表面池一直占着（`docs/MEMORY.md` 里量化的那笔开销）；
  ④ 上层永远收不到 READY，界面停在「切换中」。
  > 这里还有一个自相矛盾的细节：看门狗在 19:25:09.756 就已经在跑了（说明 `mQualitySwitchCommitPending` 那时**已经是 false**），
  > 但 READY 又确实没发。代码里能在不发 READY 的情况下把它清掉的只有 `FlushVideoPath()`（`:4452`）
  > —— 而 `FlushVideoPath()` 同时会把 `mRetiredVideoStreamIndex` 清成 -1（`:4461`），
  > 这与上面 `retiredVideo=-1`、「CloseStream 3 从未出现」完全自洽。
  > **是「谁在提交后 0.1 秒内就 flush 了视频路」需要下一条日志来钉死（见第 6 节）。**

### 差异 4 HLS 的 READY 缺失 ⇒ 切换状态机留在半个状态

DASH：`committed → (8ms) → quality switch rendered → stream switched: video index=0 … → status=1`。
HLS：`committed → 什么都没有`。
后果除了差异 3 的收尾链，还有：`mQualitySwitchOldFramesPending` / `mQualitySwitchCommittedStreamIndex`
这一组状态没有正常归零，界面高亮与真实解码器状态可能不一致。

### 差异 5 普通 seek 在 HLS 上是好的（重要对照！）

```
19:25:03.769 PFR: seek posUs=31780000 inCache=0 status=5
19:25:03.774 HLSManager: first seeked time is 31780000 --> 20854166
```
seek 之后播放正常（04.5 之后 FPS 回到 58.8/60）。**DASH 那份日志里 seek 之后视频就死了**
（`fps 16 → 0`，随即 `refused … master 15853165, next key pts 25025000`）。
差别就在差异 1：DASH 的 seek 走的是 MPD 名义栅格，`timePosition` 一错位，`RenderVideo` 就把
「时钟之后 9 秒的关键帧」当成了下一个追帧目标。**这条把 DASH 报告的 RC-4（demuxer 游标不回切换点/seek 点）
精确地钉在了「timePosition 与 pts 不同轴」上**，而不只是「读得快」。

---

## 5. 合并后的根因清单（DASH 报告 + 本文）

| 编号 | 根因 | 只影响 | 证据 |
|---|---|---|---|
| RC-2 | 切换窗口/提交门限混用 `timePosition`（呈现轴）与 `mMasterClock`（PTS 轴）；`mPendingVideoPtsOffset` 在 DASH/HLS 都建不起来 | **DASH 独有**（HLS 两轴一致；但提交门限 ±2s 仍导致差异 2 的锚点偏移） | DASH `pts=18468450 timePosition=15955950 target=17887678`；两次 `offset=INT64_MIN` |
| RC-4 | 提交/seek 后 demuxer 游标没有回到播放点，队列堆未来视频 | 两者都有，形式不同 | DASH 读到 70s；**HLS 一打开就落在 70.9s** |
| RC-4b | **目标 Representation 打开时不在切换点** | **HLS 尤其严重** | `OpenStream 0` 0.5s 后 `mark startTime 70904164` / `-9.m4s` |
| RC-5 | 「太早不渲染」与「跳两次就不再跳」互相锁死 | 两者（完全相同） | 两份日志的 `refused` 逐秒序列 |
| RC-6 | 追帧用 `ClearPacketBeforePTS + FlushVideoPath`，把队列里唯一靠前的关键帧也丢了 | 两者 | `next key pts` 从 41.7→45.9（DASH）/ 50.05→58.39（HLS）单调前移 |
| RC-3 | 退役 Representation 的包留在共享 FIFO 队列里被逐个丢，且不计入 `mVideoFlowMs` | 两者 | `drop stale video packet …` 洪水 |
| **RC-3b** | **「关旧流 + 退旧解码器 + 发 READY」全绑在「有一帧上屏」上；一帧没上屏就整条收尾链不跑** | **HLS 暴露得最清楚** | 无 `CloseStream 3`、`retiredVideo=-1`、`unknown stream 3` ×2000+ |
| RC-8 | 双解码器共用/切换 GPU 设备上下文没有设计 | 两者（未直接观测到，但 HLS 的目标流从未产出可渲染帧与此吻合） | `setUpPendingVideoDecoder(..., nullptr, 0)` |

---

## 6. 修复优先级（在原报告基础上按「这份 HLS 日志新增的证据」重排）

**P0-A（同时修两个协议，收益最大）把「切换收尾」从「等一帧上屏」上解耦**
- READY / `CloseStream(retired)` / `releaseRetiredVideoDecoder()` 不能再以「必须有一帧 render 成功」为唯一条件。
  增加**超时兜底**（例如 commit 后 300~500ms 无论如何都发 `PLAYER_QUALITY_SWITCH_READY` 或 `FAILED`，
  并把 `mRetiredVideoStreamIndex` 交出去关掉）。
- 只要这条不改，任何一次「提交了但帧没上屏」都会变成「旧流不关 + 旧解码器不退 + 界面永远切换中 + 洪水日志」，
  正是 HLS 这份日志（2000+ 行 `unknown stream 3`）的样子。

**P0-B 打开目标 Representation 时，必须把它定位到切换点**
- HLS：`HLSManager::OpenStream` 之后（或之前）用**目标流的 timePosition/pts** 显式 `seek()` 到
  「第一个 timePosition ≥ 切换点的分段」，而不是让 `SegmentTracker` 自己决定起点。
- DASH：同样要求在 `OpenStream` 时带上 seek 目标（现在虽然有一句 `pending seek to 17835000`，但之后游标又跑到 70s）。
- 判据（下次日志一行）：`HLSStream: stream (0) mark startTime …` 的值应当 ≈ 切换点，而不是 70.9s。

**P0-C 把「太早」当成「游标错了」处理（修 RC-5 死锁）**
- 头帧早于时钟超过阈值（>1s）时不进入「原地等」，直接判定队列错位 → 重新对齐/清到正确关键帧；
- 且这种「超前」不要计入 `mVideoSkipWithoutRenderCount`（那是给「落后」用的），否则两个保护继续互相锁死。

**P0-D 退役流的包要原子清掉；`drop stale` 要计入 `mVideoFlowMs`**
- 给 `MediaPacketQueue` 加 `DropPacketsByStream(int)`；或把 `mPendingVideoPacketQue` 推广为按 streamIndex 分队列。
- DASH 与 HLS 都需要（两份日志都有这条洪水）。

**P1-A 统一时间轴（仍要做，但优先级低于上面三条）**
- DASH 侧：`DashStream::ReadPacket` 里 `mStreamStartTimeMap[i].timePosition = mCurSeg->fixedStartTime;`（`:846`）
  用的是 MPD 名义栅格。**HLS 已经有正确做法**（用分段首个包的真实 PTS 当段起点，见差异 1），
  DASH 应该照 HLS 的 `SegmentTracker` 对齐；这才是 DASH「seek 之后 9 秒超前」的根。
- 同时把 `mPendingVideoSwitchTimePosition` / `TryCommitPendingVideoSwitch` 的门限统一到一根轴上。

**P1-B 提交锚点的门限收紧**
- 现在允许提交一个**落后主时钟最多 2 秒**的帧（HLS 这次就是 −1.57s）。
  应改成「提交帧与 master 的差 ≤ 一个帧间隔」，否则宁可多等一个分段。

**P1-C 把「目标流打开后跑到哪」打进一行日志**
- 现在判断目标流落点只能靠 `HLSStream: mark startTime` / `DashStream: getCurSegNum` 这些间接行，
  且被 `drop stale` 洪水淹没。建议每秒一行：`[switch] target stream=… curPos=… firstKeyPts=… queueFront=… master=… frameQ=…`。

**P1-D 前端**（沿用原报告）
- URL scheme 白名单（这次 URL 已经合法，`master.m3u8` 解析成功，已验证 RC-1 的判断：上次的 HLS 失败确实是 URL）；
- 切换失败/超时要能回退高亮（HLS 这次界面会一直停在「切换中」）。

---

## 7. 这份日志仍然回答不了 / 需要补的一行

1. **「谁在提交后 0.1 秒内 flush 了视频路」**：看门狗在 09.756 已经能跑 ⇒ `mQualitySwitchCommitPending` 已 false，
   但 READY 没发；能在不发 READY 的情况下清掉它的只有 `FlushVideoPath()`（同时会把 `mRetiredVideoStreamIndex` 清 -1，
   与 `retiredVideo=-1` 自洽）。给 `FlushVideoPath()` 加一行带调用者/原因的日志即可钉死。
2. **新 4K 流为什么落在第 10 段（70.9s）**：需要在 `HLSStream::open` / `SegmentTracker::getCurSegPosition()`
   入口各加一行「进入时的 curSeg 与目标 timePosition」。
3. **promote 之后 4K 解码器到底有没有出帧**：与 DASH 报告同一处缺口（提交后没有逐帧 PTS 日志）。
   两份日志里 `video path stalled … but packets are still being consumed` 与 `RenderVideo(true)` 返回 false 并存，
   说明「帧队列空」与「包在被消费」是同时成立的；补 P1-C 那一行就能一刀切开。
4. HLS 的 `mPtsDiscontinueDelta` 仍然是 **10000000（10s）**，而 HLS 分段其实只有 ~4.2~12.5 秒：
   10 秒的「PTS 不连续阈值」会让 `mVideoPtsRevert` / `TryCommitPendingVideoSwitch` 的 `pts + delta < mPlayedVideoPts`
   判断形同虚设（要落后 10 秒才算 revert）。这条的来源是 `getMaxGopTimeUs()`，值得单独查一下它对 HLS 返回 10s 是否合理。
