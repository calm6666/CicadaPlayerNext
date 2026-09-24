# 无感切换清晰度（双路解码）卡死 / FPS→0 / seek 失效 / HLS 打不开 —— 根因分析

> 分析对象：`CicadaPlayerNext/framework`（跨平台核心）+ `CicadaPlayerNext/mediaPlayer`（SuperMediaPlayer）
> + `CicadaPlayerNext/platform/QtPlayer`（Qt 前端）
> 证据：用户提供的 `log.txt`（2026-09-21 19:08:11 → 19:08:59，片源 `http://127.0.0.1:9000/video/output.mpd`，
> 8 条视频 Representation = 4 档 × {H.264, H.265}，60fps，`SegmentTemplate duration=10s`）
>
> **验证程度（先说清楚）**：本文每条结论都标注了「日志证据 / 代码位置」。
> 凡是只有推断没有证据的，写在最后一节「不能从这份日志确定的部分」里，没有混进结论。
>
> **后续补充（重要）**：之后又拿到了 HLS 的实测日志（合法 URL，`master.m3u8` 播放成功），
> 分析见同目录 **`ANALYSIS-HLS-VS-DASH.md`**。它给出了两份日志的逐条对照，
> 并修正/新增了三条结论：
> 1. 本文 §RC-2（`timePosition` 与 PTS 轴混用）**只在 DASH 上发作** —— HLS 的 `SegmentTracker`
>    用分段首个包的真实 PTS 当段起点，`timePosition ≡ pts`；本文 §3 RC-2 的影响面请按此收窄；
> 2. 本文 §RC-4「demuxer 读得太快」在 HLS 上表现为**目标 4K 流一打开就落在 70.9 秒的分段**
>    （比 DASH 更极端），而不是「读得快」；
> 3. 新增 **RC-3b**：READY / `CloseStream(retired)` / `releaseRetiredVideoDecoder()` 全绑在
>    「有一帧上屏」上，**一帧没上屏就整条收尾链不跑**（HLS 日志里 `unknown stream 3` 刷了 2000+ 行）。
>    修复时这条优先于本文的 P0-1。

---

## 0. 结论速览

| 用户看到的现象 | 真正的原因 | 证据强度 |
|---|---|---|
| HLS「直接无法播放、播放错误」 | **URL 本身就是非法的**：`zhttp://…/master.m3u8`，多了一个 `z`。前端的地址规范化把 `zhttp:` 当成合法 scheme 原样放过去，FFmpeg 于是报 `Unsupported protocol`。**不是播放器的 HLS 实现坏了。** | 日志铁证（唯一的 HLS 尝试就是这条 URL） |
| 点切换后画面冻住、半天切不过来、FPS→1 甚至 0 | 切换状态机把 **两套时间轴混用**（`mMasterClock.GetTime()` 是 PTS 轴，`timePosition` 是呈现轴，本片源差 2.5s 且每个分段还不一样）→ 切换窗口/提交门限全部错位；提交后**新码流的 demuxer 游标没有回到切换点**，共享包队列里堆了 **15~18 秒的未来视频**；`RenderVideo()` 对「早于时钟 10ms 以上」的帧一律不渲染 → 画面停死 | 日志铁证（`next key pts 41.7s/45.9s` vs `master 24~28s`）+ 代码 |
| 「现在连 seek 都不行了」 | **同一个根因**。日志里 19:08:21.63 那次暂停帧 seek（`PFR: seek posUs=12840000`）之后视频就已经死了：19:08:25.178 出现 `skip video forward refused … (master 15853165, next key pts 25025000)` —— 队列里的下一个关键帧在 25.0s，时钟才 15.85s，**缓冲区比时钟超前 9 秒**。清晰度切换只是把这个状态变成永久的。 | 日志铁证 |
| 画面再也回不来（等到用户关程序都是 0 FPS） | 追帧自检 `MAX_VIDEO_SKIP_WITHOUT_RENDER=2`：跳两次没上屏就**拒绝再跳**；而「上不了屏」恰恰是因为队列超前 —— 两个保护互相锁死，形成永久死锁。日志里 33.4→37.5s 每秒刷一次 refusal | 日志铁证 |
| 日志一大坨 `drop stale video packet after quality switch` | 旧 Representation 的包**留在共享视频包队列里**，只能由解码循环一个一个丢（4.4 秒 / ~370 个包），而且这种「丢包」**不更新看门狗的 mVideoFlowMs**，于是看门狗把这段当成「整条路死了」 | 日志铁证 |

一句话总结：**这不是一个 bug，是双路切换的三处结构性问题叠在一起** ——
(1) 两套时间轴混用；(2) 新旧 Representation 共用一个 FIFO 包队列 + demuxer 游标不回到切换点；
(3) `RenderVideo` 的「太早不渲染」与看门狗的「跳两次就不再跳」互相锁死。
HLS 那条是**前端 URL 校验缺一个白名单**，跟框架无关。

---

## 1. 日志时间线（每行都是原文摘录）

```
19:08:18.0  打开 output.mpd；选 stream 7 = H.265 854x480（最低档 1050kbps）；D3D11VA 硬解 hevc
19:08:19.4-21.4  正常：video fps 45 / 59 / 60，KPI Current FPS 58.9
19:08:21.627 PFR: seek posUs=12840000 inCache=0 status=5          ← 暂停帧 seek 到 12.84s
19:08:21.633 DashStream: seek:1062 stream (0) seek us is 12840000 / pending seek
19:08:22.167 audio clock re-anchored after seek at pts=12842667
19:08:22.375 video fps is 16
19:08:23.377 video fps is 0        KPI Current FPS:1.3 → 1.0      ← ★ 视频路已经死了
19:08:25.178 skip video forward refused: 2 catch-up jumps without a single rendered frame,
             the video path is not merely late (master 15853165, next key pts 25025000)   ← ★★ 队列比时钟超前 9s
19:08:25.178 video path stalled 1009 ms, force rendered the newest frame
19:08:26.193 getCurSegNum is 7 / open segment …, 69999999         ← 播放位置 ~17s，读的是 70s 的分段
19:08:27.149 [CicadaPlayerItem] selectQuality: index=0 (streamIndex=0)       ← 用户点 2160p
19:08:27.160 DashManager: OpenStream 0 ; DashStream: pending seek (us is 17835000)
19:08:27.212 video stream switched to incompatible parameters (codec 6 -> 1, 854x480 -> 3840x2160)
19:08:27.218 avcodecDecoder: D3D11VA: hardware decoding enabled for h264       ← pending 解码器
19:08:27.247→29.4  drop pending frame before switch window pts=12512500 timePosition=10000000 target=17887678
                    （≈200 行，timePosition 从 10.0s 一路丢到 12.1s，目标 17.888s）
19:08:29.281→29.5  normalize pending frame pts=18401717 to timePosition=15889217 stream=0
                    hold pending video before playback position: framePts=15889217 … master=19956582
                    （≈600 行，一直丢到 master 追上）
19:08:29.932 committed seamless video decoder switch at pts=20854167 master=20606253 audio=20970667
             quality switch rendered: stream=0 size=3840x2160
19:08:30.2→34.6   drop stale video packet after quality switch: stream=7 current=0 pts=29312617 … 44494450
                    （4.4 秒 × ≳83/s ≈ 370 个包，pts 从 29.3s 一直丢到 44.5s）
19:08:32.468 stream 0: getCurSegNum 3 / open segment …, 29999999 / pFrame->pts 33366667
19:08:34.025/34.082/34.183/34.229  getCurSegNum 4/5/6/7（40s/50s/60s/70s）  ← ★ 1.8 秒读完 5 个分段
19:08:32.375 起  video fps is 0 …… 一直到 38.376 每秒都是 0
19:08:33.462 skip video forward refused: … (master 24138226, next key pts 41708333)   ← ★★ 队列超前 17s
19:08:34.466 skip video forward refused: … (master 25141734, next key pts 41708333)
19:08:35.469 skip video forward refused: … (master 26144353, next key pts 41708333)
19:08:36.469 skip video forward refused: … (master 27144642, next key pts 45879167)
19:08:36.469 video path stalled 4018 ms but packets are still being consumed, keep waiting
19:08:37.473 skip video forward refused: … (master 28149019, next key pts 45879167)   ← 1 秒内 next key 没动
19:08:38.384 KPI test finish: total fps:10.5                                        ← 用户放弃，关播放器
19:08:49.160 [qt] Main.qml:1609 playUrl: zhttp://127.0.0.1:9000/video/master.m3u8    ← ★ HLS：URL 多了个 z
19:08:49.353 ffmpegDataSource: open error
             mDataSource open failed,url is zhttp://… Unsupported protocol
19:08:49.354 avformat_open_input error -1330794744,Protocol not found
19:08:49.361 [CicadaPlayerItem] player error 537198593: Unsupported protocol
19:08:59.930 opening source:  (previous: zhttp://…/master.m3u8)                      ← 用户放弃
```

---

## 2. 现在这套「双路解码」是怎么写的（代码地图）

| 机制 | 位置 |
|---|---|
| 请求切换（上层 → 框架） | `SMPMessageControllerListener.cpp` `ProcessSwitchStreamMsg` / `switchVideo`（:929 附近）；`SuperMediaPlayer.cpp:4131` 起 `mWillChangedVideoStreamIndex` 命中首个目标包时建立切换 |
| 三槽解码器 | `SMPAVDeviceManager`：`mVideoDecoder`（active）/ `mPendingVideoDecoder` / `mRetiredVideoDecoder`；`setUpPendingVideoDecoder` / `promotePendingVideoDecoder` / `releaseRetiredVideoDecoder`（`.cpp:128/198/215`） |
| 目标包的独立队列 | `SuperMediaPlayer::mPendingVideoPacketQue`（`ProcessVideoPacket` :4058 分流） |
| 目标帧的预热 / 丢弃 | `FillPendingVideoFrame()`（:2912，含 `drop pending frame before switch window`、`normalize pending frame pts`、`hold pending video before playback position`）、`DrainPendingVideoFrames()`（:3006） |
| 提交（原子换槽） | `TryCommitPendingVideoSwitch()`（:3048，`committed seamless video decoder switch`） |
| 提交后发 READY / 关旧流 / 退旧解码器 | `RenderVideo()` :3571-3602（`quality switch rendered`） |
| 追帧 | `skipVideoForwardToMasterClock()`（:2007） |
| 看门狗 | `checkVideoPath()`（:2087 起，`video path stalled …`） |
| Qt 侧 | `platform/QtPlayer/src/CicadaPlayerItem.cpp:1044` `selectQuality()`（先改高亮再等框架）、`notifyQualities` / `notifyStreamSwitched` |

设计意图（代码注释里写得很清楚）：**旧解码器继续出画 → 新码流只进 pending 解码器预热 → 追到切换点后 promote → 新帧真的上屏了再关旧流/退旧解码器**。
方向是对的（ExoPlayer/AVPlayer 的「保留旧路直到新路就绪」就是这个思路），**但下面 9 条把它做坏了**。

---

## 3. 根因

### RC-1 「HLS 打不开」= URL 非法（前端缺陷，与框架无关）

- 日志：`playUrl: zhttp://127.0.0.1:9000/video/master.m3u8` → `Unsupported protocol`。
- 代码：`HomeWindow.qml:771 openPlayerUrl()`：
  ```qml
  if (!/^[a-zA-Z][a-zA-Z0-9+.\-]*:/.test(u)) { ... 补 https:// ... }
  ```
  `zhttp:` 完全符合这个正则（`zhttp` + `:`），于是被当成「已有 scheme」原样交给播放器。
- 结论：**这条不是播放器 bug**；`z` 是被输进输入框的（很可能手误/输入法）。
  但**校验本身是个真缺陷**：任何 `word:` 前缀都算合法 scheme，用户看不到「地址不合法」，只看到框架层的 `Unsupported protocol`。
- 附带证据：`platform/QtPlayer/main.cpp:541` 记录过「HLS 才走到 `initOpen`（解析 master.m3u8）就 terminated abnormally」，
  也就是说 **HLS 路径本身还没有被这份日志验证过**（这份日志里 HLS 一次都没有真正进到 HlsParser）。

### RC-2 ★两套时间轴混用（切换状态机最核心的错）★

下面这 4 处把 **PTS 轴**（`mMasterClock` / 音频 pts / 视频原始 pts）和 **呈现轴**（`packet->getInfo().timePosition`）当成同一个量在比：

| 代码 | 比较的两个量 |
|---|---|
| `SuperMediaPlayer.cpp:4224` `mPendingVideoSwitchTimePosition = mMasterClock.GetTime();` | PTS 轴 → 当作 timePosition 用 |
| `FillPendingVideoFrame()` :2935 `frameTimePosition + 2s < mPendingVideoSwitchTimePosition` | timePosition vs PTS 轴 |
| `TryCommitPendingVideoSwitch()` :3118/:3129 `frameTimePosition ± 2s vs master` | timePosition vs PTS 轴 |
| `SuperMediaPlayer.cpp:4058` 之后 `mPendingVideoPtsOffset` 只在 `frameTimePosition < 0` 时才计算 | 于是 DASH/HLS 永远走不到「对齐」这一步 |

日志把差值拍死了：

```
drop pending frame before switch window pts=18468450 timePosition=15955950 target=17887678
                                  ^^^^^^^^^^ 18.468s    ^^^^^^^^^^ 15.956s    ^^^ 17.888s
```
- PTS 轴：18.468s 已经**过了**目标 17.888s（差 +0.58s），按 PTS 判根本不该丢；
- 呈现轴：15.956s **没到**目标（差 −1.93s），按 `time2ptsDelta=−2.5125s` 判才丢。
- 也就是说：代码在做「按呈现轴判定、按 PTS 轴设目标」的混算，**误差恰好是一个容器的起始偏移 2.5125s**。
- 更糟的是这个偏移**每个分段都不一样**。`DashStream.cpp:846-883` 每开一个新分段就重算一次
  `time2ptsDelta = fixedStartTime − 首个包的 pts`，而 `fixedStartTime` 用的是 MPD 的**名义 10s 栅格**，
  真实分段却是**按关键帧对齐**的。从日志里算出来的真实第一个关键帧时刻是
  `12.5125 / 20.854 / 33.367 / 41.708 / 50.05 / 62.5625 / 70.904`，
  与 10s 栅格的差是 `−2.513 / −0.854 / −3.367 / −1.708 / −0.05 / −2.563 / −0.904` ——
  **timePosition 在分段边界上会跳 ±3.4 秒**。任何拿 timePosition 和 master 直接比的逻辑都必然失稳。
- 后果（日志可见）：提交日志里 `offset=-9223372036854775808`（`mActiveVideoPtsOffset` 从未建立）；
  切换窗口判定丢掉了 ~200 帧、`hold pending` 又丢掉了 ~600 帧，**用 2.7 秒的 CPU/GPU 去「等一个本来就错位的窗口」**。

### RC-3 ★新旧 Representation 共用一个 FIFO 包队列 + 游标不回切换点★

- `ProcessVideoPacket()`（:4047）只在「pending 尚未提交」时把目标包分流到 `mPendingVideoPacketQue`；
  提交后**两路都写进同一个** `mBufferController` 的视频包队列（`BUFFER_TYPE_VIDEO`）。
- 提交时只清了 pending 包队列（:3184），**旧 Representation 已进入共享队列的包一个没动**；
  旧流的 `CloseStream` 要等新帧真的上屏（:3588）才做。
- 于是解码循环只能一个一个丢：
  ```
  19:08:30.217 … 19:08:34.601  drop stale video packet after quality switch: stream=7 current=0 pts=29312617 … 40457083
  ```
  4.4 秒、≳370 个包。而且**这条路径不更新 `mVideoFlowMs`**（`doDeCode()` :2560 直接 `mVideoPacket.reset(); continue;`），
  看门狗会把这段「一直在丢包」误判成「整条视频路死了」→ 触发追帧 → flush 掉本来还有用的数据。
- 更根本的：`MediaPacketQueue` 是**按插入顺序**的 FIFO / 单一 `mCurrent` 游标，
  而 `GetFirstKeyPTSAfter()`（:178）返回的是「队列顺序里第一个 pts ≥ 目标的关键帧」**不是最小的那个**。
  两路时间轴混在一条队列里时，这个函数返回的就是随机值 —— 日志里的
  `next key pts 25025000 / 41708333 / 45879167` 就是这么来的。

### RC-4 ★新码流的 demuxer 游标没有回到切换点 → 队列里堆了 15~18 秒未来视频★

```
19:08:29.318 getCurSegNum is 2 / open segment …, 19999999      ← 20s 分段
19:08:32.468 getCurSegNum is 3 / open segment …, 29999999      ← 30s
19:08:34.025 getCurSegNum is 4 / open segment …, 39999999      ← 40s
19:08:34.082 getCurSegNum is 5 / open segment …, 49999999      ← 50s
19:08:34.183 getCurSegNum is 6 / open segment …, 59999999      ← 60s
19:08:34.229 getCurSegNum is 7 / open segment …, 69999999      ← 70s
```
**1.8 秒读完了 5 个分段（30s→70s）**，而那一刻播放位置只有 ~25s。
`OpenStream(0)` 触发的 `Seek(17835000)` 之后就再没有把游标拉回切换点；
读取只受「缓冲时长」约束，**不受播放时钟约束**（本地 mock 服务器 11Mbps 都能瞬间灌满）。

后果：
- `RenderVideo()`（:3499）有一条 `if (videoLateUs < -10 * 1000 && !qualitySwitchWarming) return false;`
  —— **「早于时钟 10ms 以上」的帧不弹、不丢、原地等**。队列头是 41.7s 的帧、时钟在 24s → 等 17 秒。
- 帧队列上限 `VIDEO_PICTURE_MAX_CACHE_SIZE = 2`（`SuperMediaPlayer.cpp:38`），
  两个「未来帧」一塞满，解码循环就停；这就是「画面定住、但音频正常、进度条还在走」。

### RC-5 ★追帧的两个保护互相锁死（永久死锁）★

`skipVideoForwardToMasterClock()`（:2034）：
```cpp
if (mVideoSkipWithoutRenderCount >= MAX_VIDEO_SKIP_WITHOUT_RENDER /* = 2 */) { ... refused ...; return false; }
```
- 语义是「跳了两次都没上屏 ⇒ 这条路不是『只是慢』，别再跳了」——**这条判断本身是对的**（它是为了修上一轮的 HEVC 解 H.264 死循环）。
- 但现在「上不了屏」的原因是 **队列超前**（RC-4），不是解码器出不了帧。
  于是：超前 → 不渲染 → 跳两次 → 拒绝再跳 → 永远不渲染。**两个保护互相锁死**。
- 日志里 33.462 / 34.466 / 35.469 / 36.469 / 37.473 每秒一次 `refused`，master 从 24.1s 爬到 28.1s，
  `next key pts` 从 41.7s 到 45.9s —— 用户等 6 秒直接关程序。

### RC-6 追帧用 flush 把唯一能救活的关键帧也丢了

`skipVideoForwardToMasterClock()`：先 `ClearPacketBeforePTS(nextKeyPts)`，再 `FlushVideoPath(false,false)`。
在 RC-3 的混合队列上，`GetFirstKeyPTSAfter()` 可能挑到一个**很靠后**的关键帧（41.7s / 45.9s），
`ClearPacketBeforePTS` 于是把解码器刚预热好的东西全清了。日志里「2 catch-up jumps」正是这两次。

### RC-7 pending 路在解码主循环里做同步重活

- `DrainPendingVideoFrames()`（:3014）一次最多 `VIDEO_PICTURE_MAX_CACHE_SIZE*2` 次 `FillPendingVideoFrame()`，
  每次 `getPendingVideoFrame(frame, 0)` 都要拿 `SMPAVDeviceManager::mMutex`（`.cpp:170`）；
- `promotePendingVideoDecoder()`（`.cpp:198`）**在锁里** `flush()+close()` 上一个 retired 解码器；
  `releaseRetiredVideoDecoder()`（`.cpp:215`）同理 —— 4K D3D11VA 解码器 close 要销毁 20 片表面池（见 `docs/MEMORY.md`），不是微秒级；
- 同一个锁还被 `setUpDecoder()`（active 路）用。这条锁跨了「解码主循环」和「切换提交」两个上下文。
- 日志佐证：切换前后 `mReadLoopIndex` 从 ~83 掉到 ~81、`mReadGotIndex` 出现 `0.000000`/`5366` 这种尖峰
  （19:08:19.245、19:08:25.270、19:08:37.338），主循环被拖住的痕迹很清楚。

### RC-8 双解码器 + 零拷贝共用 GPU 设备这件事没有设计

- `CreatePendingVideoDecoder()`（:3021）调 `setUpPendingVideoDecoder(flags, &meta, nullptr, 0)`
  —— **device 传 nullptr**（注释说「不能和 active 抢同一个 surface」）；
- 而 active 解码器用的是从 Qt 场景图借来的 D3D11 设备（`HwDeviceBridge` / `CicadaHardwareDevice`）；
- 两条 4K/HEVC 解码器同时开着，各自一套 `hw_frames_ctx` / 表面池；
- 提交时把一个**新设备上下文**解码出来的纹理交给**绑定在旧解码器上的渲染器输入视图**，
  渲染侧不会被重建（`CicadaTextureD3D11::releaseInputState()` 本来就存在，但这条路径没调）。
- 这和项目已经踩过的「`CicadaHardwareDevice` 是进程级单例、窗口重建后设备悬空」（`HANDOVER-TODO.md` 4c）是**同一类问题**。

### RC-9 方向性问题：DASH/HLS 的无缝切换不该靠「预热解码追赶」

主流实现（以及 DASH-IF 互操作规范）都是**在 SAP/分段边界上切换**，而不是让第二个解码器从「切换点之前的关键帧」一路解过来：

- DASH-IF IOP 明确要求切换点要把解码器按新参数**重置在切换点上**；`startWithSAP=1` + `segmentAlignment=true`
  的存在意义就是让客户端能在分片边界无痛切换
  （[DASH-IF IOP v4.2](https://dashif.org/docs/DASH-IF-IOP-v4.2-clean.pdf)、[v4.3](https://dashif.org/docs/DASH-IF-IOP-v4.3.pdf)）。
- 「切换时刻 t 可能需要从更早的随机接入点开始取包」这件事本身是被承认的难点
  （[相关专利文本](https://ptacts.uspto.gov/ptacts/public-informations/petitions/1541802/download-documents?artifactId=9UItgtt3k-uizEdJKQdHLtbEW8ybi0VeSBBPG7aM51OI0PRfa7JzATA)），
  但工程上的解法是**让 demuxer 精确落到那个 SAP**，不是让解码器把中间那一整段解完再丢。
- ExoPlayer/Media3 遇到同类问题（[androidx/media #2299「Decoder freezes on quality switch when playing HLS LL stream」](https://github.com/androidx/media/issues/2299)、
  [ExoPlayer #4394「Stuck in BUFFERING after changing quality」](https://github.com/google/ExoPlayer/issues/4394)）
  的修复都落在 **segment 选择 / 缓冲对齐** 层，不是靠在播放循环里加第二个解码器预热。

本工程现在的写法（pending 解码器从上一个关键帧解到切换点再提交）在 10 秒分段的 4K 片源上，
就意味着**每次切换要在播放循环里多解最多一整个 GOP（60 帧 4K）**，日志里 `drop pending frame` ≈200 行 +
`hold pending` ≈600 行就是这个代价。

---

## 4. 修复方案（按优先级）

### P0-1 统一时间轴：切换状态机一律用 PTS 轴（或一律用 timePosition，但不能混）

- `mPendingVideoSwitchTimePosition` 改成「切换时刻的 **帧 PTS**」，由当前播放帧的 pts 拿，
  而不是 `mMasterClock.GetTime()` 直接当 timePosition 用（`SuperMediaPlayer.cpp:4224`）。
- `FillPendingVideoFrame()` / `TryCommitPendingVideoSwitch()` 里所有 `frameTimePosition` 的判据
  改成用**同一轴的 `frame->getInfo().pts`**（或明确先 `timePosition → pts` 换算一次再比）。
- `mPendingVideoPtsOffset` 的建立条件去掉 `frameTimePosition < 0`（:2991），
  改成「只要 `|pendingFirstPts − masterPts| > 阈值` 就建立一次，并**固定用于该解码器整个生命周期**」。
  提交日志里那个 `offset=-9223372036854775808` 就是这一步没跑的直接证据。
- **顺带修框架的根**：`DashStream.cpp:846` 那句 `mStreamStartTimeMap[i].timePosition = mCurSeg->fixedStartTime;`
  用的是 MPD 名义栅格。分段是按关键帧对齐时，应该用**首个包的实际 timePosition 推断**该分段的起始
  （例如 `fixedStartTime` 只作为初始估计，之后用「上一分段的 timePosition + duration」续推），
  否则 `timePosition` 在每次跨分段时都会跳 ±3 秒。

### P0-2 让 demuxer 精确落在切换点的 SAP，而不是「预热追赶」

1. `selectQuality` → `DashManager/HLSManager` 时，把目标 Representation 直接 **seek 到「第一个 timePosition ≥ 切换点的 SAP」**，
   并把那个 SAP 的 timePosition 回传给 `SuperMediaPlayer` 作为 `mPendingVideoSwitchTimePosition`。
2. pending 侧只接受 `timePosition ≥ 该 SAP` 的包；SAP 之前的一律不进解码器。
3. 这样 pending 只需解 **≤1 个 GOP 里的极少几帧**（甚至 1 帧）就能提交，
   `drop pending frame …` / `hold pending video …` 这两坨日志和那 2.7 秒的预热开销直接消失。

### P0-3 提交后把 demuxer 拉回切换点（禁止「未来视频」堆积）

- 提交时（`TryCommitPendingVideoSwitch()` 尾部 / `RenderVideo()` 发 READY 处）对**新** Representation 调一次
  `MuxerService->Seek(committedPosition)`，或至少 `ClearPacketAfterTimePosition(BUFFER_TYPE_VIDEO, committedPos + maxBuffer)`。
- 给 DASH/HLS 的读取加一条**按播放时钟的流控**：当 `队列尾 timePosition − masterPos > maxBufferDuration` 时，
  暂停该 Representation 的读取（现在只有「缓冲时长」约束，本地服务器会把 15~18 秒未来视频全灌进来）。
- 这条同时修「seek 之后队列超前 9 秒」——日志里 `master 15853165 / next key pts 25025000` 就是它。

### P0-4 把「太早」当成「游标错了」处理，解掉 RC-5 的死锁

- `RenderVideo()` 的 `videoLateUs < -10ms → return false` 只适用于「正常播放中的单帧提前」。
  当头帧早于时钟超过 **一个 GOP / 阈值**（例如 >1s）时，应当**判定为 demuxer/队列错位**，
  走「重新对齐」而不是「原地等」。
- `skipVideoForwardToMasterClock()` 里把两种情形分开：
  * 「视频落后」→ 允许跳（现有的 `mVideoSkipWithoutRenderCount` 保护保留）；
  * 「视频超前」→ **不计入** `mVideoSkipWithoutRenderCount`，走 re-anchor / flush-to-clock 的路径。
  这样 RC-5 的两个保护不再互相锁死。

### P0-5 退役 Representation 的包「原子」清掉，不要靠解码循环一个个丢

- 提交时（或 `CloseStream` 之前）把共享队列里**所有 `streamIndex == mRetiredVideoStreamIndex`** 的包一次性清掉
  （给 `MediaPacketQueue` 加一个 `DropPacketsByStream(int)`，内部改 `mCurrent`/`mDuration` 而不是逐个 `getPacket`）。
- 更彻底：把 `mPendingVideoPacketQue` 推广成**按 streamIndex 分队列**（一路一条队列），
  让 `GetFirstKeyPTSAfter()` / `ClearPacketBeforePTS()` 的语义重新变成「单条时间轴」，
  从根上避免「FIFO 里两套 PTS 交错」。这也顺带让 `GetFirstKeyPTSAfter` 返回的「第一个 key」重新有意义。
- 这些「丢包」也要计入 `mVideoFlowMs`（它现在只认「上屏/丢帧」），否则看门狗会误判。

### P1-1 双解码器的 GPU 设备/渲染器交接

- 提交时先让渲染侧放掉旧解码纹理的引用（已有 `CicadaVideoTexture::releaseInputState()` /
  `CicadaTextureD3D11::releaseInputView()`），再 promote，再重建输入视图；
- 或者更稳：pending 解码器**使用与 active 完全相同的 hw device / frames context**
  （把 `device` 传下去，而不是 `nullptr`），只在帧输出上区分，避免两套表面池 + 两套 hw_frames_ctx。
  这条必须实测（4K HEVC→H.264 切换时的左上角残片/黑帧正是它的症状，`promotePendingVideoDecoder()` 的注释也承认了）。

### P1-2 锁的粒度

- `SMPAVDeviceManager::mMutex` 现在把「取一帧」（`getPendingVideoFrame`，热路径）和
  「close 一个 4K 解码器」（`promotePendingVideoDecoder` / `releaseRetiredVideoDecoder`，冷但很慢）串在一起。
  把 retired 的 `flush()+close()` 移到**锁外**（或单独一个「延迟回收」线程/队列），
  否则每次切换都会在主循环里造成一次可见的停顿。

### P1-3 前端

- `HomeWindow.qml:779`：把「已有 scheme」的判据从「`word:`」收紧成**白名单**
  （`http|https|file|rtsp|rtmp|data`），不在白名单里就当非法地址，给一条明确提示而不是把
  `Unsupported protocol` 丢给用户。这条直接消灭本次的 HLS「打不开」。
- `CicadaPlayerItem::selectQuality()` 目前是**先改高亮再等框架**（代码注释里已写明）。
  建议把 `PLAYER_QUALITY_SWITCH_*` 的状态也喂给 UI（started/ready/failed/canceled），
  失败/超时时把高亮**回退**——「点了半天没反应」的体感一半来自这里。

### P1-4 日志（现在的日志本身在干扰测量）

- `drop pending frame …` / `hold pending video …` / `drop stale video packet …` 全是 `AF_LOGD` **逐帧/逐包**打印，
  这次一份 log 里光这三类就 ~2000 行（还外加日志文件被两个写入者交错覆盖，出现整段重复）。
  改成**每秒一行汇总**（丢掉多少帧/包、当前 master / 队列头 / 队列尾）。
- 真正关键的那行 `quality switch video frame pts=… master=… audio=…`（`FillVideoFrame` :2850）
  只在 `mQualitySwitchCommitPending` 期间打，提交后（也就是出问题的整段）**一行都没有**。
  建议提成每秒一次的 `[switch] active decoder out pts=… timePos=… queueFront=… frameQ=…`。
- 另外这份 log 明显是**两个写入者写同一个文件**（第 36~92 行整段重复第 5~... 行、
  1276~1343 重复 1245~1317，带错位覆盖），排查前先把日志口收成一个。

---

## 5. 建议的验证顺序（每步一行日志就能判定）

1. **HLS**：用 `http://127.0.0.1:9000/video/master.m3u8`（去掉 `z`）重跑。日志应出现
   `HlsParser` 相关行 + `qualities: N`；若仍 `Unsupported protocol`，才需要查 HLS 实现。
2. **seek**：暂停 → 拖到 12.84s → 观察是否还出现
   `skip video forward refused … (master …, next key pts …)`。修复 P0-3 后，`next key pts` 与 `master`
   应该只差一个分段以内，且 `video fps` 不掉到 0。
3. **切换**：在 10s 分段 4K 片源上从 480p 切 2160p，检查：
   - `drop pending frame …` / `hold pending video …` 的总行数应降到个位数（P0-2 生效的判据）；
   - `drop stale video packet after quality switch` 应该消失（P0-5 生效的判据）；
   - 提交行 `committed seamless video decoder switch at pts=… master=… offset=…` 里
     **`offset` 不再是 `-9223372036854775808`**（P0-1 生效的判据）；
   - 切换后 3 秒内 `video fps` 回到 55+（现在 0）。
4. **跨编码切换**（H.265 480p → H.264 2160p，本次的必现路径）：确认没有
   `Failed to parse header of NALU` / `PPS id out of range`（这是上一轮修过的老问题，别回归）。

---

## 6. 不能从这份日志确定的部分（诚实说明）

- **被 promote 之后的 4K H.264 解码器到底「出不出帧」**：日志在提交后就再没有逐帧 PTS 记录，
  而 36.469 那一刻 `RenderVideo(true)` 返回了 false（说明帧队列空）、同时看门狗又说「包还在被消费」，
  两者在日志里是并存的。本文按「队列超前 → 头帧太早 → 帧队列被 2 帧上限堵住」解释（所有可观测数字都吻合），
  但「解码器一帧都不出」这个分支**没有被排除**。补一行每秒的诊断（P1-4）就能一刀切开。
- **`mPictureCacheType` 在这台机器上是 `picture_cache_type_cannot` 还是别的**（决定帧队列上限是 1 还是 2）——
  日志里没有这行，只是从 `VIDEO_PICTURE_MAX_CACHE_SIZE = 2` 推的。
- **HLS 实现本身有没有 bug**：这份日志里 HLS 一次都没进到 `HlsParser`（URL 就非法了），
  所以「HLS 无法播放」在本次唯一证据下**只能归因到 URL**。修 URL 后必须重测才能谈其他。
- **flash/闪退**：本次日志没有崩溃（正常关窗）。`HANDOVER-TODO.md` 4j(3) 记的
  `0xc0000374` 堆损坏、CRT `HEAP CORRUPTION DETECTED` 与这次的现象不是同一件事，本文不覆盖。
