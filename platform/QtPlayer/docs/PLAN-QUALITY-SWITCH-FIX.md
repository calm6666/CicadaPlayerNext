# 修改计划：双路解码（无感切换清晰度）HLS/DASH 卡死 —— 分阶段修复

> 依据：`ANALYSIS-QUALITY-SWITCH-FREEZE.md`（DASH 日志）+ `ANALYSIS-HLS-VS-DASH.md`（HLS 日志）
> 目标读者：直接动手改代码的人（含我自己）
>
> **本文只写「怎么改」。** 每条都带：改动位置、改法、为什么不会破坏现有行为、**一条能判定成败的日志**、回滚点。
> 行号是当前工作区的锚点，改动后会有漂移，以函数名为准。

---

## 实施进度

### ✅ C1（阶段 0 埋点）+ C2（W1.1 切换收尾解耦）—— 2026-09-21 已完成，Debug 动态构建通过

改动文件与内容：

| 文件 | 改动 |
|---|---|
| `platform/QtPlayer/main.cpp` | **W0.1**：新增进程内唯一的日志文件出口（`log_set_back` → `cicadaLogFileBack`），路径优先级 `--log-file <path>` > `CICADA_LOG_FILE` > `<用户数据目录>/appQtPlayer.log`，写 `-` 关闭；`setvbuf(stdout, _IOLBF)`；Qt 消息处理器不再在已有单文件出口时重复写 stderr（那是日志里大量重复行的来源） |
| `mediaPlayer/SuperMediaPlayer.h` | 新增 `finishQualitySwitch()` / `checkQualitySwitchDeadline()` / `logQualitySwitchState()` / `floodLogAllowed()` 声明；`FlushVideoPath()` 增加 `const char *from` 形参；**所有新成员一律追加在成员列表末尾**（该文件顶部有硬约束：插在中间会让别的 TU 的偏移对不上 → 0xC0000005） |
| `mediaPlayer/SuperMediaPlayer.cpp` | **W0.2** `[switch]` 每秒一行；**W0.3** `FlushVideoPath` 打印调用者与全部相关状态；**W0.4** 六类洪水日志限频（每秒 2 条 + 汇总）；**C2/W1.1** `finishQualitySwitch()` 成为 READY/关旧流/退 retired/清状态的唯一出口，`RenderVideo` 的 READY 分支改为调它，`FlushVideoPath` 里在途切换也走它，并新增 500ms 死线兜底（只在 `PLAYING && !buffering` 时计时） |
| `mediaPlayer/SMPMessageControllerListener.cpp` | 两处 `FlushVideoPath` 传 `__func__` |
| `framework/demuxer/play_list/HLSStream.cpp`、`framework/demuxer/dash/DashStream.cpp` | **W0.5** 新增 `[seek] hls/dash …: reqUs=… -> segNum=… curSegNum=… live=…` 与 `tracker positioned at segNum=…` 两行，把「seek 请求值」和「tracker 真正的落点」分开（原来的 `usSeeked` 打的是请求值，容易误判） |

**验证程度**：只做了 **动态 Debug 构建**（`[100%] Built target appQtPlayer`，无新增 warning）。
**没有做运行时验证** —— 这个 harness 起 GUI 进程会立刻退出（本工程 `HANDOFF`/`HANDOVER-TODO.md` 第五节记录过同一条限制），
需要用户在 Qt Creator 里跑一次并回传日志。下一节列了「跑一次应该看到什么」。

**新增日志的验收点**（下次日志里逐条对）：

| 现象 | 判据 |
|---|---|
| 单文件出口生效 | 开头有 `[log] framework log file (single writer): <路径>`；**这份文件本身没有重复行** |
| 切换状态可见 | 切换期间每秒一行 `[switch] state=… willChange=… master=… activeFrontPts=… frameQ=… deadlineLeft=…` |
| 谁 flush 了视频路 | `FlushVideoPath from <函数名> (…) commitPending=… retiredStream=…` |
| 洪水消失 | `[flood] <类别>: N more lines suppressed in the previous second` 代替几万行逐帧日志 |
| **HLS 的旧流被关掉** | 出现 `HLSManager: CloseStream 3` 与 `closed retired video stream after quality switch (ready=…)`；`unknown stream` 归零 |
| **必须有终态** | 每次 `quality switch status=0` 之后必有 `=1/2/3`；若走兜底会有 `quality switch deadline hit: … finishing as FAILED` |
| 每次收尾一行 | `finishQualitySwitch ready=… stream=… pts=… master=… reason=…` |

### ✅ 第三轮：按 20:20 那份日志修掉三个 bug（FIX-A/C/D）—— 已编译，等实测

C1/C2 的埋点把剩下的问题全部钉死了。证据 → 修复：

| # | 日志证据（`log.txt` 2026-09-21 20:20~20:21） | 结论 | 修复 |
|---|---|---|---|
| **A** | `20:21:05.498 committed … pts=283617 master=2277277` → `20:21:05.528 FlushVideoPath from skipVideoForwardToMasterClock (…): commitPending=1` → `20:21:05.544 quality switch status=2 … video path flushed`（20:21:10 第二次一模一样）。**三次切换三次都是这个形状** | 提交后帧队列里还躺着**旧路**的帧，它们相对主时钟落后 >500ms → `RenderVideo()` 的“落后追帧”分支先命中 → `skipVideoForwardToMasterClock()` → `FlushVideoPath()` → 刚提交的切换被自己人按“取消”收尾 | **FIX-A**：追帧分支加 `!qualitySwitchWarming`（提交期间一律不追帧）。`SuperMediaPlayer.cpp` `RenderVideo()` |
| **D** | `20:21:37.193 finishQualitySwitch ready=1 stream=0`（HLS 切换**成功**）之后，`20:21:38.2~44.2` 的 `[flood] drop stale video packet after quality switch: 82 more lines suppressed` 连续 7 秒；期间 `video fps is 0` | 退役 Representation 在公共视频队列里的残留包只能由解码循环“一个主循环丢一个”（≈82 个/秒），几百上千个包要丢 7 秒；排在他们后面的**新**流的包一个都轮不到解码 | **FIX-D**：`MediaPacketQueue::DropPacketsByStream()` + `BufferController::DropPacketsByStream()`，在**提交时**和**收尾时**各原子清一次；stale 分支补 `mVideoFlowMs`（免得看门狗把“正在清理”误判成“视频路死了”） |
| **C** | `[seek] dash video: reqUs=16894000 -> segNum=2 curSegNum=6`（播放 16.9s，读取游标已在 60s）；3.7 秒后 `curSegNum=8`（80s）；`[switch] … master=5329077 activeFrontPts=15331983`（**超前 +10 秒**，`activeQ=5342` 个包）；HLS 切成功后 `next key pts 75075000` vs `master 66085454`（+9 秒）→ `skip video forward refused` 永久拒绝 | 读前量只按**内容总量**限流（`cur_buffer_duration > mSet->maxBufferDuration`，默认 **50 秒**，见 `MediaPlayerConfig.cpp:16`），没有按**播放时钟**限流 → 队列头合法地停在时钟前面 10 秒 → `RenderVideo()` 认为“帧太早”不上屏 → 帧队列堵死 → 队列不消费（`frameQ=0` 而包还在被消费）→ 读得更多 | **FIX-C**：`doReadPacket()` 新增**读前闸门**：`队首 pts − master > 2 秒` 就停止读取（`MAX_VIDEO_READ_AHEAD_US`）。护栏：只在 PLAYING 且非缓冲/非 seek/非 pending、且音频缓冲 ≥1 秒时生效，避免连音频一起停 |

**同轮的小诊断改进**：`[switch]` 行补 `willChange=`（上一份日志里 `state=warming target=-1` 看着像“没在切换”，其实 `mWillChangedVideoStreamIndex` 已经置上了，只是没打出来）；新增 `FLOOD_READ_AHEAD` 限频类别给闸门用。

**这一轮回答掉的悬案**：
* **目标 Representation 的落点是准的**：HLS 这次 `open_internal:374 usSeeked is 62562498 seek num is 8`（62.56s）与 `${master}` 62.82s 基本吻合；上一份日志里“落在 70.9s”就是**读飞了**（FIX-C 处理），不是 seek 没生效。W1.3 的“带 seek 打开”因此降级为“消除竞态”的可选项。
* **切换终态链路是通的**：HLS 这次拿到了 `quality switch status=1`（有史以来第一次），DASH 三次都是 `status=2`（有明确原因，不再静默）。

**仍然悬着、需要下一份日志的**：
1. **提交锚点仍可能落后 1~2 秒**（`pts=283617 vs master=2277277`）。4K 目标路预热比实时慢，收紧提交窗口会让它永远追不上时钟。现在的策略是“先按粗略锚点接上画面（FIX-A 保证接得上），READY 之后由正常追帧把 1~2 秒追回来”。要判断这个代价能不能接受，看 `finishQualitySwitch ready=1` 之后 `video fps` 恢复得怎么样。
2. **`frameQ=0` 却 `activeQ` 在增长**（包在消耗但不出帧）这条路径还没有解释。下一份日志的 `[switch]` 行现在同时打 `frameQ` / `activeFrontPts` / `activeQ`，三者一起看就能区分“解码器不出帧”还是“帧被丢掉了”。

### ✅ 第四轮：按 20:41 那份日志修「追帧跑飞 + 状态机单向闩锁」+ 按开源实现重写 ABR

证据来源：`C:\Users\calm\AppData\Local\appQtPlayer\appQtPlayer.log`（单写日志，935 行，20:41:49~20:44:24）。
C1/C2 的埋点全部生效（`[switch]` 行、`FlushVideoPath from <函数>`、`[flood] … suppressed`、`[seek] dash video: reqUs=… -> segNum=…`），
这一轮把日志里剩下的问题逐条钉死。

#### 4.1 根因与修复

| # | 日志证据 | 结论（根因） | 修复 |
|---|---|---|---|
| **RC-A** | `PFR: seek posUs=33620000`（33.62s）→ `read-ahead gate: front 37053683 is 3419 ms ahead of master 33634305`；随后每秒一次 `video is late, dropped 28/247/246… packets … key pts 37537500 → 41708333 → 45879167 → 50050000 → 54220833 → 58391667`，而 master 每秒只走 1 秒，gap 从 3.4s 涨到 16.7s；最后 `skip video forward refused: 2 catch-up jumps without a single rendered frame` | **追帧跳到了时钟「之后」的关键帧**。旧的取点逻辑无条件取「当前点之后的下一个关键帧」，而 DASH 的关键帧间隔是分片边界（8.3 秒），跳过去就把画面**停在未来**；`RenderVideo()` 对「太早」的帧一律不上屏，于是没有帧上屏 → 追帧计数满 → 永久拒绝 → 0 FPS。这是「seek 都卡死」的直接原因 | `skipVideoForwardToMasterClock()`（`SuperMediaPlayer.cpp`）：目标选择改成「贴着时钟的关键帧」——只有当**下一个**关键帧比时钟超前超过 `CATCH_UP_MAX_AHEAD_US`(200ms) 时，才改用 `GetLastKeyPTSAtOrBefore(BUFFER_TYPE_VIDEO, master)` 取「时钟之前最近的关键帧」；否则仍用下一个。配套新增 `MediaPacketQueue::GetLastKeyPTSAtOrBefore()` / `BufferController::GetLastKeyPTSAtOrBefore()`。日志格式改为 `video is late, dropped N packets and restarted the video path at keyframe (master …, next key pts …, chosen …, …)` |
| **RC-B** | `[switch] state=decoderSwitch … target=-1 current=7 … decoderSwitch=1` 连续 ~20 秒不变（637~656 行），中间一次用户 seek 才解开；用户再切一次后又出现（20:44:12.497 之后） | **`mPendingVideoDecoderSwitch` 是单向闩锁**：`FlushVideoPath()`（seek / 追帧 / 切换取消都会走它）没有清它。它为真时 `RenderVideo()` 把 `qualitySwitchWarming` 判为真 → **绕过所有时钟节拍**（`render = true`）→ `AFActiveVideoRender::mInputQueue` 每次溢出后 `dropFrame()` 并把自己的渲染时钟重锚到最新帧 → 实测 3 秒内 77 行 `drop a frame pts = 77177100…81314567`，表现就是「画面忽快忽慢 + 进度条跳 + 帧率掉到 1 帧」 | `FlushVideoPath()` 里清掉 `mPendingVideoDecoderSwitch` 与 `mVideoSkipWithoutRenderCount`（路径已经 flush 成空，闩锁没有任何理由继续存在），并在 flush 日志里打出清了什么 |
| **RC-C** | 一次切换停在 `decoderSwitch`，`pendingPktQ=120 pendingFrameQ=4` 卡了 20 秒 | **提交前的超时条件漏了一种**：旧条件要求「帧落后切换点 2 秒以上」才超时；而这次帧并不落后（`pendingFrameQ=4`），只是永远凑不出满足提交条件的帧 → 状态机被永久钉住 | `TryCommitPendingVideoSwitch()`：把「落后 8 秒」的超时改成一个**无条件**的绝对上限 `QUALITY_SWITCH_TOTAL_TIMEOUT_MS`(10s)（与 ExoPlayer 的 pending-period 超时同量级），并把超时日志补全为 `elapsed=… pendingPktQ=… pendingFrameQ=… master=…` |
| **RC-D** | `committed seamless video decoder switch at pts=1201200 master=3192806`（锚点比时钟旧 2 秒）→ 紧接着 `video is late … key pts 4170833` → 每次切换成功之后都有约 1 秒冻结 + 音画偏移 | 提交时把 pending 队列里**所有**已解码帧（最多 4 帧）推入 active 队列，播放锚点取的是**最旧**的一帧 | `TryCommitPendingVideoSwitch()`：只把 **最新** 一帧带进 active 队列，更旧的 `setDiscard(true)`；锚点贴着时钟，剩下的差值交给 `beginRendererJoining()` 的追赶窗口（对应 ExoPlayer 的 `setJoiningDeadlineMs()` / `VideoFrameReleaseControl.join()`） |
| **RC-E** | 每次切换成功之后 `dropLateVideoFrames` 从没生效过（它只被读、只被置 false，没有任何地方置 true） | 这套「交接窗口内丢迟到帧」的机制是**死的**，切换后的 1~2 秒落后只能靠 `video is late → 追帧 → flush` 这条会伤到切换的路 | 新增 `beginRendererJoining(const char*)`：置 `dropLateVideoFrames = true` + 窗口截止时间 `JOINING_DROP_LATE_WINDOW_MS`(3s)，在**提交时**和 **seek 结束时**调用；窗口到期或遇到准时帧自动关闭。对应 ExoPlayer 的 joining 语义：交接窗口内「迟到帧按 skip 处理」，窗口外才正常渲染 |
| **RC-F** | `PFR: seek posUs=33620000` 之后包队列队首已经在 37.05s（超前 3.4 秒）；seek 期间 `force_render=true` 把**每一帧**都推给渲染器 | seek 期间 `force_render` 绕过所有节拍判断，于是「目标点之前」的几十上百帧也被推给 Qt 渲染器（4K 纹理上传 + 交换链），渲染器输入队列一满就丢帧并重锚时钟 → seek 之后画面快进、进度条跳 | `RenderVideo()`：`render && mSeekFlag && mSeekPos != INT64_MIN && frameTimePos + SEEK_TARGET_DROP_AHEAD_US(200ms) < mSeekPos` → 不渲染（丢弃）。还没到目标点的帧对用户没有意义（ExoPlayer 对解码前缀帧用的是 `FRAME_RELEASE_SKIP`） |
| **RC-G** | 视频路超前时看门狗也会走「追上主时钟」出口 | 旧看门狗只有一个出口：往前跳。**超前**时往前跳只会更超前 | `checkVideoPathWatchdog()`：新增「视频超前 → 等，不追」分支（`videoPathLeadUs() > VIDEO_AHEAD_NO_CATCHUP_US`(300ms)），并计入限频类别 `FLOOD_VIDEO_AHEAD` |
| **RC-H** | `[switch]` 行看不到 pending 路到底卡在哪 | `pendingFrameQ=4` 分不清「还在解码」和「解出帧但落在时间线之外」 | `logQualitySwitchState()` 增加 `pendingFrontPts=` / `pendingFrontPos=` / `joiningLeft=` |
| **RC-I** | 每次 seek 后 2 秒必然出现 `no SeekEnd from the player within 2000 ms` | 看门狗太紧：DASH 精确 seek 会落在**前一个分片**（首包 12.5s），框架要从那里的关键帧解码到目标点，5.7 秒 4K H.264 ≈ 2.6 秒真实时间 → **正常情况就会响**，一响就把在途状态清掉，用户后续拖动被当成新 seek 重发，和框架里没结束的那次重叠 | `CicadaPlayerItem`：`m_seekWatchdog` 2000 → **6000 ms**（依据：框架自己的视频路救援窗口 `VIDEO_PATH_STALL_FLUSH_MS=3000`，正常 seek 不会超过两个窗口） |
| **RC-J** | `zhttp://…` 被原样丢给播放器，只留下一句打不开的 URL | `HomeWindow.qml` 用「像不像 scheme」的正则（`^[a-zA-Z][a-zA-Z0-9+.\-]*:`）判协议，任何 `word:` 都算；副作用是 `D:\x.m3u8` 里的 `D:` 也被当成协议，盘符路径连 `file:///` 都补不上 | `openPlayerUrl()` 改成**白名单**判协议（http/https/file/ftp/qrc/content/rtsp/rtmp/rtp/udp/srt/tcp/hls/data），先认盘符再判协议；白名单外的协议去掉后按域名补 `https://` 并打一条 `console.warn` |
| **RC-K** | `committed seamless video decoder switch at pts=1201200 master=3192806`（锚点比时钟旧 2 秒）→ 紧跟 `video is late` + 约 1 秒冻结；用户反馈「进度条会往回跳一下」 | 提交门限允许新路首帧**落后主时钟 2 秒**。于是 active 帧队列里出现「旧帧在时钟上、新帧在时钟之前」的顺序反转（33.60 → 33.62 → **31.9**），画面往回倒 + 音画错位。开源实现里没有这个动作：ExoPlayer 对 `presentationTimeUs < positionUs` 的输出帧是 `FRAME_RELEASE_SKIP`（解码不显示），换流只把读取游标移回当前位置；hls.js/dash.js 是把新 Codec 的分片**追加**在当前播放位置之后 | 提交门限从「落后 2 秒以内」收紧到 `PENDING_ALIGN_TOLERANCE_US`(100ms)= 一个帧间隔。比播放位置更旧的帧继续丢弃、继续等目标路解到播放位置；旧路在等待期间照常播放，所以用户看不到冻结，只是换档时刻晚一点。追不上时由 `QUALITY_SWITCH_TOTAL_TIMEOUT_MS` 兜底 |
| **RC-L** | `drop a frame pts = 77177100 … 81314567` 在 3 秒里连刷 77 条（`AFActiveVideoRender` 输入队列溢出 + 渲染时钟被重锚）；同期 `[switch] state=decoderSwitch`、`video fps is 0~1` | **预热阶段（提交之前）每一帧都绕过时钟节拍**：`RenderVideo()` 的 `qualitySwitchWarming` 把「提交**前**的预热」也算进来（`mPendingVideoStreamIndex >= 0`），于是 `render = true`。预热阶段旧路本来完全正常（decoder 没换、帧队列没换），绕过节拍只会把「未来的帧」提前推进渲染器 → 输入队列顶爆 → 丢帧 + 时钟重锚 | `RenderVideo()`：无条件出画只保留给**已经提交**的状态（`mQualitySwitchCommitPending`）；预热阶段回到正常节拍。那个 `qualitySwitchWarming` 局部变量由此**没有任何读者**，2026-09-24 已连同例外一起删除（`mPendingVideoStreamIndex` / `mPendingVideoDecoderSwitch` 在别处仍用于流切换与解码调度，但**渲染的早/迟帧判据与追赶窗口不再看这两个标志**，只剩 `dropLateVideoFrames` 与 `mQualitySwitchCommitPending`） |
| **RC-M** | 追赶窗口内可能把整整 3 秒的帧全丢掉（4K 硬解每帧都迟到 >10ms 时），表现为「切换后黑一下/冻一下」 | 原 `dropLateVideoFrames` 分支有两个问题：on-time 的那条分支只清标志、`render` 仍是 false（**准时帧也被丢了**）；且没有任何「多久没出画就必须渲染」的护栏 | 对着 ExoPlayer `shouldForceRenderOutputBuffer()`（`elapsedSinceLastRenderUs > 100_000`）补 `JOINING_FORCE_RENDER_MS`(100ms)：追赶窗口内迟到帧照丢，但距上次真的上屏超过 100ms 就强制渲染一帧；on-time 分支改成 `render = true` |
| **RC-N** | `quality switch deadline hit` 有可能在切换**正常推进**时误报失败 | 死线只看「有没有帧上屏」，而追赶窗口本来就是「丢迟到帧、暂时不上屏」；于是正常追赶会被判 FAILED，界面收到失败、高亮退回旧档 | `checkQualitySwitchDeadline()`：死线衡量「有没有进展」——只要视频路还在流动（上屏或按窗口丢帧，`mVideoFlowMs` 在刷新）就把死线往后推；真的什么都不发生时 `mVideoFlowMs` 变陈旧，兜底照旧生效 |

#### 4.2 ABR：按开源实现重写（不是自己想的规则）

> 完整的取材记录（每个常量在哪个文件的哪一段、原文引用、以及没能拿到的部分）见
> [`RESEARCH-ABR-AND-SEAMLESS-SWITCH.md`](RESEARCH-ABR-AND-SEAMLESS-SWITCH.md)。

旧 `AbrBufferAlgoStrategy` **完全没有带宽估计**，只有「最近 10 次缓冲在涨还是在跌」的投票（上切要 `buffer>=30s` 且 8/10 在涨；
下切要 `buffer<15s` 且 8/10 在跌）。两个后果：缓冲掉到 15 秒才降档（已经晚了）；更严重的是
`mSwitching` **只在 `SetCurrentBitrate()` 里清零** —— 只要有一次 ABR 发起的切换没走到 `streamChangedSuc`（超时/失败/被 flush 打断），
`ProcessAbrAlgo()` 第一句就 return，ABR 被永久锁死。日志里从头到尾没有一条 `BA switch to bitrate`，只有手点之后才出现的
`BA already change to bitrate`，就是这条路径。

重写依据（全部读源码得到，逐条对应代码里的常量与注释）：

| 来源 | 取用的东西 |
|---|---|
| [hls.js `src/utils/ewma.ts`](https://github.com/video-dev/hls.js/blob/master/src/utils/ewma.ts) + [`ewma-bandwidth-estimator.ts`](https://github.com/video-dev/hls.js/blob/master/src/utils/ewma-bandwidth-estimator.ts) | 快/慢双半衰期 EWMA（VOD 默认 3 / 9 个样本）、`getEstimate() = min(快, 慢)`、`alpha = exp(ln0.5/halfLife)`、样本权重=下载秒数、值=8×bytes/时长、兜底估计 `abrEwmaDefaultEstimate = 500kbps` |
| [hls.js `src/controller/abr-controller.ts`](https://github.com/video-dev/hls.js/blob/master/src/controller/abr-controller.ts) `findBestLevel()` | 上切 `adjustedbw = abrBandWidthUpFactor(0.70) × 估计`（比下切保守）、下切/保持 `abrBandWidthFactor(0.95) × 估计`、`maxStarvationDelay = 4s` |
| [dash.js `InsufficientBufferRule.js`](https://github.com/Dash-Industry-Forum/dash.js) | 缓冲越浅允许的码率越低；缓冲见底时按「这段时间最多能下多少」强制降档 |
| dash.js `AbrController` (`abandonLoadTimeout = 10000`) | 放弃一次下载后 10 秒内禁止**上切**（降档不受限） |
| [ExoPlayer `AdaptiveTrackSelection`](https://github.com/androidx/media/blob/main/libraries/exoplayer/src/main/java/androidx/media3/exoplayer/trackselection/AdaptiveTrackSelection.java) | `DEFAULT_MIN_DURATION_FOR_QUALITY_INCREASE_MS = 10_000`（缓冲不到 10s 不上切）、`DEFAULT_MAX_DURATION_FOR_QUALITY_DECREASE_MS = 25_000`（缓冲还有 25s 以上先不降）、`canSelectFormat(…, effectiveBitrate)` = 「码率不超过预算的最高档」 |
| [Shaka `SimpleAbrManager`](https://shaka-project.github.io/shaka-player/docs/api/lib_abr_simple_abr_manager.js.html) | `abr.switchInterval = 8s`（两次切换之间的最小间隔）、`advanced.minTotalBytes = 128KB`（估计值可信门槛） |

与开源实现的**差异**（已写进代码注释，不藏）：`AbrRefererData` 没有暴露片段时长，所以 hls.js/ExoPlayer 里
「预测下载时长 vs 剩余缓冲」这条判断用 ExoPlayer 的缓冲门限（10s / 25s）等价覆盖；另外**样本不足前一律不动作**
（hls.js 只用默认估计值选**起始**档位，拿它去改已经播起来的档位没有依据）。

新增日志（下次日志里对）：

| 现象 | 判据 |
|---|---|
| ABR 在跑 | `ABR waiting for samples: N/128000 bytes, buffer=… ms` → 之后出现 `ABR switch up/down: bitrate A -> B … throughput=… fast=… slow=… buffer=… ms reason=…` |
| 上切被缓冲挡住 | `ABR hold up-switch: buffer=… ms < 10000 ms` |
| 降档被缓冲挡住 | `ABR hold down-switch: buffer=… ms >= 25000 ms` |
| **闩锁不再出现** | 不再出现「切了一次之后就永远没有 `ABR switch …`」；若真超时会打 `ABR: switch to bitrate=… did not complete in … ms, releasing the in-flight flag and rolling back to bitrate=…` |

#### 4.3 这一轮同步对齐的 ExoPlayer 阈值（研究结论，暂不改行为）

对开源实现的核对结果，记在这里备查（**这一轮不改**，因为现有值更保守且不产生新风险）：

| 量 | 本工程 | ExoPlayer（media3 `release` = 2.x 的字面量提升为常量） |
|---|---|---|
| 判定「迟到」 | `dropLateVideoFrames` 窗口内 `late > 10ms` 就丢 | `earlyUs < -30_000us`（`MIN_EARLY_US_LATE_THRESHOLD`） |
| 判定「非常迟」→ 丢包跳到关键帧 | `late >= 500ms` 走 `skipVideoForwardToMasterClock()` | `earlyUs < -500_000us`（`MIN_EARLY_US_VERY_LATE_THRESHOLD`）→ `skipSource()` + flush 解码器 |
| 「太早」的帧 | 不渲染、不丢，留在队列里下次再看 | `earlyUs > earlySchedulingThresholdUs(50_000us)` → `FRAME_RELEASE_TRY_AGAIN_LATER`，缓冲区保留 |
| 长时间没上屏就强制渲染 | `(videoPts - mPlayedVideoPts) > 60ms` | `elapsedSinceLastRenderUs > 100_000` |
| 交接（joining）窗口 | `JOINING_DROP_LATE_WINDOW_MS = 3s` | `setAllowedJoiningTimeMs()` / `join()` → `joiningDeadlineMs` |

**验收点（本轮新增）**：A10 追帧跳点必须落在时钟**之前**（`chosen <= master`）；A11 一次切换从 `status=0` 到终态 ≤ 10 秒；
A12 `decoderSwitch=1` 不得跨过一次 `FlushVideoPath`（flush 后必须为 0）；A13 seek 后不得出现连续两次
`skipVideoForwardToMasterClock` 而没有 `quality switch rendered`/`RenderCallback`；A14 提交锚点与主时钟之差必须
≤ 100ms（`committed seamless video decoder switch at pts=… master=…`，两者差值即 A14）；A15 切换过程中
`drop a frame pts=…`（`AFActiveVideoRender` 溢出）不得出现。


---

### ✅ 第五轮：21:20 那份日志 —— 追帧跳到未来 + 提交时挖掉 2 秒数据 + 预热死锁 + ABR 从来不跑

证据来源：`appQtPlayer.log`（853 行，21:20:17~21:24:04，DASH 与 HLS 各测了一轮）。
第四轮的埋点全部生效，而且第一次拿到了"对齐提交"的证据：
`committed seamless video decoder switch at pts=6172833 master=6265649`（只差 **93ms**）和
`committed … pts=77577500 master=77665039`（差 **87ms**）—— **提交对齐是对的**。但提交之后立刻出问题。

#### 5.1 根因（都有精确数字）

| # | 日志证据 | 结论（根因） | 修复 |
|---|---|---|---|
| **RC-O** ★ | DASH：`committed … pts=6172833 master=6265649` → 下一秒 `read-ahead gate: video packet queue front 8458450 is 2177 ms ahead of master 6280637 (**activeQ=1**)`；HLS：`committed … pts=77577500` → `queue front 79863117 is 2170 ms ahead of master` | 提交时把 `mPendingVideoPacketQue` **整段 discard** 了。这个队列装的是目标流"已经读进来、还没解码"的那一段，上限 120 个包 —— 60fps 的内容正好是**约 2.2 秒**。丢掉它等于在新旧时间轴之间挖一个 2.2 秒的洞：画面停在洞的这一侧、主时钟继续走 → 2 秒后 `video is late` → 追帧 → 冻得更久（HLS 实测跳到 83.4s，**5.8 秒 0 FPS**）。两次片源的数字（2177ms / 2170ms）与 120 包 60fps 完全吻合 | 提交时**不清**这个队列，把这段数据留给已经提升为 active 的新 decoder 继续解（`doDeCode` 里新增 `leftoverAfterCommit`：`mPendingVideoStreamIndex` 归 -1 之后仍从这个队列取包） |
| **RC-P** ★ | `video is late, dropped 193 packets and restarted the video path at keyframe (master 79874301, **next key pts 83416667, chosen 83416667**)`；DASH 那次 `master 8488474 → chosen 12512500`（差 **4.0 秒**） | 第四轮改成"优先跳到时钟之前的关键帧"，但**在没有这种关键帧时回退到 nextKeyPts** —— 而分片式 DASH/HLS 的下一个关键帧就在下一个分片边界上（8.3~12.5 秒一跳），于是画面被停到时钟**前面** 4~5.8 秒。更糟的是它把队列头推到时钟前面，读前闸门随即停读，越等越死 | `skipVideoForwardToMasterClock()`：只有"不晚于主时钟的最近关键帧"才是合法落点；连它都没有（队列里只剩时钟之后的数据）就**直接返回 false**，让 RenderVideo 走"迟到帧照样上屏"，靠解码速度追上。对应 ExoPlayer 的 `skipSource(positionUs)` —— 永远跳到**当前位置**，不跳未来 |
| **RC-Q** | `read-ahead gate: … (activeQ=1)` —— 队列里只剩 **1 个包**时闸门还在停读 | 读前闸门只看"队首比时钟超前"，没看"手里还有多少货"。提交后队列被清空的那一瞬间它必然触发，把唯一的救命数据也掐断 | 闸门加一条前置条件：`GetPacketDuration(VIDEO) >= MAX_VIDEO_READ_AHEAD_US`（手里已经有 2 秒压缩数据）才允许停读 —— 先保证有货，再按时钟限流 |
| **RC-R** ★ | `[switch] state=decoderSwitch … pendingPktQ=120 **pendingFrameQ=4** … decoderSwitch=1` 从 21:21:06 一动不动到 21:22:02（**56 秒**），期间 `activeFrontPts` 冻结、`frameQ=0`、画面 100% 卡住、10 秒超时也不触发 | 目标解码器输出队列满 → `DecodePendingVideoPacket` 返回 `STATUS_RETRY_IN` → 循环尾 `if (ret & STATUS_RETRY_IN) break;` 在 `TryCommitPendingVideoSwitch()` **之前**就 break 了。于是：没人从 `mPendingVideoFrameQue` 取帧（取帧/丢弃/超时全在 TryCommit 里）→ app 侧队列永远停在 4 → `DrainPendingVideoFrames()` 的 `size() < 4` 不成立 → 输出队列永远满 → 互相等；同时 active 路一包都拿不到 | ① 目标分支里在 RETRY_IN 之前就调 `TryCommitPendingVideoSwitch()`；② 目标包 RETRY_IN 时把包退回队列、置 `pendingBlocked`、`continue`（让 active 路拿包继续出画），active 路 RETRY_IN 才真的 break |
| **RC-S** | 用户反馈：每次 seek 后"**后缓冲先归零再跳**"、"**进度跳切过去又跳回来再跳过去**" | ① `PostBufferPositionMsg()` 在 seek 在途时强制 `duration = 0` → 缓冲条塌到播放点，seek 结束再跳出去；② 精确 seek 的重启点在目标点**之前**的关键帧上（HLS 实测 seek 到 26.708s、tracker 落在 20.854s，音频却按 26.709s 重新锚定），而"目标点之前的帧"只在 `mSeekFlag` 期间才被丢弃 —— seek 一结束它们就上屏，位置回调把进度条拽回 20.8s，随后再往前走 | ① `PostBufferPositionMsg()`：seek 在途时**不发布**缓冲位置（保持旧值，等 seek 结束一次性更新）；② 新增 **seek 目标地板** `mSeekPositionFloorUs`：`SeekTo()` 记下目标点，`RenderVideo()` 对"目标点之前的帧"一律不渲染（对应 ExoPlayer 的 `FRAME_RELEASE_SKIP`），`getCurrentPosition()` 在管道走到它之前一直返回它；差 15 秒以上就放弃地板（免得坏目标点把画面永久冻住） |
| **RC-T** ★ | 用户反馈：自动清晰度模式永远从最低档起播、而且一直不升 | ① 组件默认 `m_autoQuality = true`（界面显示"自动"），但框架的 ABR 只有 `SelectTrack(SELECT_TRACK_VIDEO_AUTO)` 之后才 `EnableAbr(true)` —— 不调的话框架按 `mDefaultBandWidth = 0` 挑"离 0 最近"的档，就是**最低档**，而且 ABR 线程根本不会跑；② 即便跑起来，我的 ABR 在 `bufferFull` 时直接 return 不采样 —— 本地/内网服务器几秒就把 50 秒缓冲填满，于是**一个样本都收不到**，`mSampledBytes` 永远到不了门槛，一次决策都不做 | ① `CicadaPlayerItem::notifyQualities()`：清单就绪时如果还是自动档就补一次 `SelectTrack(SELECT_TRACK_VIDEO_AUTO)`（界面状态与框架状态对齐）；② ABR 采样去掉 `bufferFull` 早退（hls.js 的样本来自分片下载完成，与缓冲深度无关），只在速度值变化时累加字节；③ `SetCurrentBitrate` 用当前档码率给 EWMA 播种（hls.js `level-controller.ts` 的 "Update default bwe to first variant bitrate"，上限 `abrEwmaDefaultEstimateMax = 5Mbps`），并且不再重置切换间隔 |

#### 5.2 补修：切回"自动"必须真的自动（RC-U）

用户要求：默认自动档，中途手动选了某一档之后，再切回"自动"必须重新走 ABR。

查下来是**两处断点**：

| 断点 | 原因 | 修复 |
|---|---|---|
| `EnableAbr(true)` 只置标志，**没有把 ABR 线程拉起来** | ABR 线程是可以被暂停的：`MediaPlayer::Pause()`（暂停播放）和 `MediaPlayer::SeekTo()`（**每次 seek**）都会 `mAbrManager->Pause()`，只有 `MediaPlayer::Start()`（播放）和 seek 结束回调才会 `Start()`。只要有一次 seek 没走到结束回调（被新 seek 覆盖、中途 stop、失败），线程就一直停着 —— 切回自动后界面显示"自动"、行为完全不动 | `MediaPlayer::SelectTrack(AUTO)` 在打开 ABR 的同时 `mAbrManager->Start()`；调用放在 `mMutexAbr` **外面**（ABR 线程回调 `abrChanged()` 是在 `AbrManager::mMutex` 里持 `mMutexAbr` 的，反过来加锁会 ABBA 死锁）；暂停状态不硬拉线程，等用户按播放时 `MediaPlayer::Start()` 起来 |
| 策略里残留"在途切换/上切禁令/切换间隔" | 手动切档时 ABR 被关掉，没人回调 `SetCurrentBitrate()`，`mSwitching` 一直挂着（看门狗要等 8 秒）；`mUpSwitchBannedUntilMs`（降档后 10 秒禁上切）和 `mLastSwitchTimeMS`（切换间隔）也都还在 | `AbrAlgoStrategy` 新增虚函数 `OnAbrEnabled()`，`AbrManager::EnableAbr(false→true)` 时回调一次；`AbrBufferAlgoStrategy` 用它清掉这三样并打一行 `ABR enabled again: …re-evaluating from the next tick`，所以切回自动后**下一个 tick（≤1 秒）**就按当前吞吐重新评估 |

配套（同一件事的另一半，来自 RC-T）：自动档在清单就绪时就会 `SelectTrack(AUTO)`（`CicadaPlayerItem::notifyQualities`），
所以"起播就是自动"和"切回自动"走的是同一条路。


#### 5.3 验收点（本轮新增）

| 判据 | 期望 |
|---|---|
| 提交后**不得**再出现 `read-ahead gate … (activeQ=1)` | 提交后 `activeQ` 应保持在两位数以上（那段 120 包的目标数据被继续消费） |
| 提交后 **2 秒内**不得出现 `video is late` | 提交对齐（±100ms）之后不应该再判"落后" |
| `chosen` 必须 `<= master` | 新的拒绝日志 `skip video forward refused: no key frame at or before the clock…` 允许出现；`chosen > master` 一律不许 |
| `[switch] … pendingFrameQ=4` 的**停滞时间** | 不得超过 1 秒（TryCommit 现在每轮都会跑） |
| 起播清晰度 | 自动档下 `BA seeded the bandwidth estimate from the current bitrate: …`，随后 ≤10 秒内出现 `ABR switch up: bitrate … -> …` |
| 切回自动 | 手点某一档后再点"自动"：`ABR enabled again: …re-evaluating from the next tick`，随后 ≤2 秒内出现 `ABR switch up/down …` |
| seek 后的位置回调 | 不得小于 seek 目标（日志看 `PFR: seek posUs=X` 之后的 `PositionUpdate`，X 之后不应回退） |


---

### ✅ 第六轮：22:10 那份日志 —— ABR 换档后的"缓冲假摔"、读循环饿死 active 路、缓冲没有任何回调

证据来源：`appQtPlayer.log`（1239 行，22:10:13~22:13:15）。第五轮的对齐/闩锁/死锁修复都生效了
（`committed … pts=1801800 master=1889178` 差 87ms，第一次切换干净利落），
但 ABR 自动切换之后立刻连环出问题。

#### 6.1 根因

| # | 日志证据 | 结论（根因） | 修复 |
|---|---|---|---|
| **RC-V** ★ | `22:10:15.055 ABR switch up: … buffer=50365 ms` → 成功提交 → `22:10:16.056 ABR switch down: 3500000 -> 3000000 … buffer=2001 ms reason=buffer about to run out`；HLS 那边 `22:12:36.194 up … buffer=49615 ms` → `22:12:37.195 down … buffer=283 ms` | ABR 的"缓冲"取自 `PROPERTY_KEY_VIDEO_BUFFER_LEN` = **视频包队列的时长**。而每次换档都会把新 Representation 的缓冲从 0 重新攒（旧档那 50 秒是按旧档读的，必须丢），所以换档后一两秒里它必然只有 ~2 秒。旧规则一看到缓冲小就**紧急降档** → "升到最高立刻又降回来"，然后每次降档又开始一轮新的换档……永远到不了稳态，用户看到的就是一直"正在切换中" + 画面卡 | ① 换档后加 `ABR_POST_SWITCH_GRACE_MS = 5s` 宽限期（dash.js `InsufficientBufferRule.segmentIgnoreCount = 2` 就是干这个的），窗口内不做任何降档；② 降档改成**缓冲优先**：只有缓冲连续 `ABR_LOW_BUFFER_TICKS = 3` 个 tick 都低于 `ABR_LOW_BUFFER_MS = 10s` 才降，见 6.2 |
| **RC-W** ★ | `22:11:20.121~22:11:45.133` 连续 6 条 `ABR waiting for samples: 0/128000 bytes, buffer=1951/50000 ms, speed=0 bps` | **高速网络测不到下载速度**。`MediaPlayerUtil` 只在"距上次结算 > 1 秒"时才写一次 `mCurrentDownloadSpeed`；本地/内网服务器把 50 秒缓冲一口气读完只要几百毫秒，窗口永远不闭合 → 值一直是初始的 0 → ABR 一个决策都做不了 | `MediaPlayerUtil::notifyRead`：结算窗口 1s → **200ms**，并要求窗口内至少 64KB（或满 1 秒）。高速网络的几百毫秒突发也能切出有效样本；低速网络行为与以前一致；几 KB 的清单下载不会被当成吞吐样本 |
| **RC-X** ★ | `22:10:19.083` 起 `activeQ=0`、`master=4781124` **冻结不动**、`pendingPktQ=120 pendingFrameQ=0`，`[switch] state=decoderSwitch` 连续刷 45 秒，`total fps:15.0` | 读循环里"目标队列满了就 `break`"把**整个读循环**停掉了。读循环是按 demuxer 走的：一次 `ReadPacket` 拿到的可能是目标流的包、也可能是当前路/音频的包，一 break 就是**当前路和音频一起断粮** → active 队列掉到 0、主时钟冻结 → 而主时钟冻结又让"目标路追上主时钟"这个提交条件永远不成立 → 双向死锁，画面 100% 卡住、声音也停 | `doReadPacket`：目标队列满时**只有在当前路还吃得住**（视频 >3s 且音频 >2s）才停读；否则继续读，多出来的目标包由硬上限 `PENDING_VIDEO_QUEUE_HARD_CAP = 320` 兜住（内存有界） |
| **RC-Y** | ABR 每 8 秒重发一次同一个降档请求（`22:10:24 / 32 / 40` 三条 `did not complete in 8004 ms, releasing the in-flight flag`） | ABR 侧看门狗 8 秒 < 播放器自己的切换上限 10 秒（对齐提交需要目标路解码追上时钟，本来就可能 5~10 秒），于是正常切换被判"没完成"→ 放锁 → 立刻重发 | `ABR_SWITCH_WATCHDOG_MS` 8s → **12s**（大于播放器的 10s 上限） |
| **RC-Z** ★ | 用户：*"就算卡住缓冲也要有回调事件…不要出现视频缓冲声音还在播放弹幕还在播放"* | 框架**本来就有**完整的缓冲状态机（`mBufferingFlag=true` → `NotifyLoading(loading_event_start)` + `mMasterClock.pause()` + `pauseAudioRender(true)`，缓冲够了再 end + 恢复），但 Qt 侧的 `playerListener` **从来没订阅** `LoadingStart/LoadingEnd/LoadingProgress/CurrentDownLoadSpeed`（memset 之后没人赋值）→ 界面只知道"打开中/就绪"，不知道"播放中卡住了"；弹幕只能靠"位置 600ms 没动"去**猜** | 见 6.3 |

#### 6.2 ABR 新策略：缓冲优先（dash.js BOLA 那一派）

> 用户问"主流的 abr 是这样实现的吗"——**是**。主流有两大派：
> * **吞吐优先**（hls.js、ExoPlayer 的 AdaptiveTrackSelection）：按带宽估计选档，缓冲只作为门限；
> * **缓冲优先**（dash.js 默认的 **BolaRule**）：码率由**缓冲水位**决定，吞吐量只当上限；
>   dash.js 的 `InsufficientBufferRule` 还专门 `segmentIgnoreCount = 2`（换档后头两个分片不判断），
>   ExoPlayer 也有 `DEFAULT_MAX_DURATION_FOR_QUALITY_DECREASE_MS = 25s`（缓冲够厚就推迟降档）。
>
> 用户要的"网络有波动不要降、只有缓冲不够了才降"就是**缓冲优先**这一派，所以按它改。

| 条件 | 动作 |
|---|---|
| 缓冲 < 4s 且过了宽限期 | 立刻降档（保不断流） |
| 缓冲 < 10s **连续 3 个 tick** 且过了宽限期 | 降到"当前吞吐撑得住"的档（`0.7 × 估计`） |
| 缓冲 ≥ 10s（ExoPlayer 的上切门限）且吞吐有余量 | 上切到 `0.7 × 估计` 撑得住的最高档 |
| 其它情况（含换档后 5 秒宽限期、缓冲在中间地带） | **什么都不做**（这是"不因为网络波动降档"的关键：吞吐掉一下不再直接触发降档） |

#### 6.3 缓冲事件接到界面（RC-Z）

| 位置 | 改动 |
|---|---|
| `CicadaPlayerItem` | 订阅 `LoadingStart/LoadingEnd/LoadingProgress/CurrentDownLoadSpeed`；新增属性 `buffering`、`downloadSpeed`(bit/s)、`downloadSpeedText`（**>1MB/s 用 MB/s，否则 KB/s**，按用户要求） |
| `PlayerView.qml` | 画面中央新增"缓冲中 X MB/s"的指示（半透明胶囊 + `BusyIndicator`），只在 `player.buffering` 时显示 |
| `DanmakuView.qml` | `_mediaStalled` 除了"位置 600ms 没动"这条**推断**，再加一条明确信号：`player.buffering` 为真立刻冻结弹幕时钟（用户要求"只要缓冲弹幕就停"） |
| 声音 | 框架侧本来就会 `pauseAudioRender(true)` + `mMasterClock.pause()`，缓冲够了自动恢复并继续播放，这一轮不用改 |
| 诊断 | `hold pending video` / `drop pending frame before switch window` 两条洪水日志从 `AF_LOGD` 提到 `AF_LOGI` 并带上 `pts/timePosition/master/target/lag` **数值** —— 之前日志里只有一句"suppressed"，卡住时完全看不出目标流解到哪儿 |

#### 6.4 验收点（本轮新增）

| 判据 | 期望 |
|---|---|
| 换档后不再立刻降档 | 不再出现 `ABR switch down … buffer=<2s` 紧跟一次 `ABR switch up` 的成对记录；降档理由必须是 `buffer too low` 或 `buffer about to run out` |
| 降档理由 | 不允许再出现 `throughput dropped below bitrate`（那条规则已删除） |
| 自动档能升到最高 | 出现 `ABR switch up: … -> <最高档>` 之后**不得**在 5 秒内出现降档 |
| 换档期间不卡死 | 换档期间 `[switch]` 行里的 `master` 必须持续增长（不能冻结）；`activeQ` 不得长时间为 0 |
| 下载速度可测 | 不再出现 `ABR waiting for samples: 0/128000 bytes … speed=0 bps` 的长时间刷屏；`CurrentDownLoadSpeed` 有非 0 值 |
| 缓冲提示 | 缓冲时界面出现"缓冲中 X MB/s"；`buffering start` / `buffering end` 成对出现在日志里；缓冲期间弹幕停住 |


---

### ✅ 第七轮：DASH 切换慢/失败/音画不同步 —— 改成"关键帧边界换档"

证据来源：`appQtPlayer.log`（4872 行，23:11:53~23:16:51，DASH 与 HLS 各测一轮）。
第六轮的修复生效（ABR 不再假摔、缓冲回调到位），剩下的是**切换本身的机制**问题。

#### 7.1 先把测得的数据摆出来

| 片源 | 切换起止 | 耗时 | 目标路预滚 |
|---|---|---|---|
| DASH #1（→4K） | 23:12:01.34 → 23:12:09.24 | **7.9s** | 帧从 7.0s 开始，切换点 9.0s |
| DASH #2（→480P） | 23:12:09.43 → **23:12:19.50 失败** | 10.1s | 帧从 30.0s 开始，切换点 47.3s（**17s 预滚**） |
| DASH #3（重试） | 23:12:41.57 → 23:12:49.81 | 8.2s | 帧从 30.0s 开始 |
| DASH #4 | 23:14:52.50 → 23:15:02.58 失败 → 重试 → 23:15:14.03 | 21.5s | 180s 那一段 |
| HLS #1 | 23:16:02.28 → 23:16:02.47 | **0.19s** | HLS 分片 4.17s |
| HLS #2 | 23:16:24.05 → 23:16:24.99 | 0.94s | |
| HLS #3 | 23:16:33.15 → 23:16:34.51 | 1.35s | |

**DASH 分片 10 秒、HLS 分片 4.17 秒**（`[seek] … -> segNum=N` 与 `duration=268200000` 反推：
178.192s → segNum=18、190.195s → segNum=20，即 10 秒一片）。差的就是这个。

#### 7.2 根因

| # | 证据 | 结论 | 修复 |
|---|---|---|---|
| **RC-AA** ★ | 切换点 47.3s，而 `drop pending frame before switch window: timePosition=30000000 target=47290339`、`timePosition=150000000 target=166236043` | 目标路是**从"包含切换点的那个分片的起点"开始解码**的，而这个起点可能比切换点早 **10~20 秒**（DASH 分片 10 秒；日志里最大 17 秒）。这段数据全部解码出来又全部丢掉 —— 纯白干。4K 目标路解码只有 ~110 帧/秒，10 秒预滚 = 600 帧 = 5.5 秒，于是切换要 7~10 秒，超 10 秒就直接判失败（`target rendition did not reach playback timeline` ×2）。**HLS 分片短，所以只要 0.2~1.3 秒** | 见 7.3：**在喂给解码器之前**就把预滚丢掉，并且把"解码起点"定在**参考点之后的第一个关键帧**上 |
| **RC-AB** ★ | pending 帧正好以 **108 帧/秒**推进（`hold pending video … framePts=7007000 → 9442767 → 11761750`，每 1 秒才 +2.3 秒内容），而 480p 的目标路甚至只有 1x | 追赶速度被两处"每轮只做一点"卡死：① `doDeCode` 里 `pendingBurst < 4` → 每轮最多 4 个包，× 主循环 ~27 轮/秒 = **108 包/秒**，这就是 108 帧/秒的来源；② `TryCommitPendingVideoSwitch()` 的"丢弃落后帧"分支**一次只丢一张**，落后 600 张就要 600 次调用。真正的成本在解码，不在丢帧 | ① `PENDING_DECODE_BURST = 64`；② 丢落后帧改成**一次丢干净**（`PENDING_STALE_DRAIN_MAX = 512`，并在循环里顺手 `DrainPendingVideoFrames()` 补货）；③ `FillPendingVideoFrame` 单次排空上限 8 → `PENDING_PREROLL_DRAIN_MAX = 2048` |
| **RC-AC** ★ | `committed … pts=200200000 master=199718939`（视频比主时钟**超前 481ms**）；另外 `drop pending frame … pts=70904167 timePosition=70000000`（两条时间轴差 0.9s） | ① 提交门限允许"未来 500ms 以内"的帧 → 提交未来帧 = 画面当场往前跳半秒 = **音画不同步**；② pending 帧的 pts 只在"与 timePosition 差 > 2 秒"时才归一化，差 0.9~1.9 秒的档位（实测 DASH 每档都差 0.9~3.4 秒）**不归一化**，promote 之后带着另一条时间轴的 pts 进 RenderVideo → 音画错位 | ① 未来窗口 500ms → `PENDING_FUTURE_TOLERANCE_US = 150ms`（更远的未来帧就"等主时钟走过来"，等待不花代价）；② `timePosition >= 0` 时**一律**用它当 pts |
| **RC-AD** | 两次 `status=2 target rendition did not reach playback timeline` | 总超时 10 秒对"要等一个关键帧边界"的切换太紧（分片式 DASH 一个分片一个 IDR 时就是 10~20 秒） | `QUALITY_SWITCH_TOTAL_TIMEOUT_MS` 10s → **18s**，ABR 看门狗 12s → **20s** |

#### 7.3 新机制：**关键帧边界换档**（这一轮的核心）

`PENDING_PREROLL_KEEP_US = 0` + 参考点 `max(切换请求时刻, 当前主时钟)`，于是
目标路只保留"**参考点之后的第一个关键帧**及其之后的包"，其余在 `ProcessVideoPacket`
里就 `setDiscard`（**不解码**）。

这就是 hls.js / dash.js 的切换方式（把新 Representation 的分片追加在当前位置之后），
一次消掉三个问题：

| 原来 | 现在 |
|---|---|
| 从分片起点解预滚，解完丢掉；10 秒预滚 = 5.5 秒（4K） | **零预滚**：解码从关键帧开始，只解到主时钟追上来的这几帧 |
| 提交点"播到哪算哪"，实测视频落后主时钟 50~99ms、极端超前 481ms | **提交点就是关键帧本身**，主时钟走到那里才提交，误差只有抖动量级 → A/V 同步 |
| 追赶太慢 → 10 秒超时 → 切换失败 | 不再需要追赶 → 不可能因为"追不上"失败 |

代价只有一个：**点击到生效最多等一个关键帧间隔**（本片源 4.17 秒，平均 2 秒）。
但等待期间**旧路照常播放**（不是黑屏/冻结），用户感知是"过一会儿清晰度变了"，
而不是"卡住了"。日志里现在会打一行
```
pending preroll starts at key frame: pts=… timePosition=… target=… master=… (preroll=N ms)
```
`preroll` 就是"解码起点距离参考点有多远"，正常情况下应该是 0~关键帧间隔。

#### 7.4 验收点（本轮新增）

| 判据 | 期望 |
|---|---|
| 切换耗时 | 每次 `quality switch started` → `status=1` ≤ 一个关键帧间隔 + 0.5 秒（本片源 ≤4.7s；HLS 应 ≤4.2s） |
| 不再失败 | 不出现 `target rendition did not reach playback timeline` |
| A/V 同步 | `committed … pts=X master=Y` 的 `\|X − Y\|` ≤ 150ms（现在还会打 `audio=`，注意 `audio − master` 约 +350ms 是音频设备缓冲，属于正常） |
| 预滚 | `pending preroll starts at key frame … (preroll=N ms)`，N 必须 ≤ 关键帧间隔（4.17s）；不允许出现"从分片起点开始解" |
| 无卡顿 | 切换期间 `[switch]` 行里 `master` 持续增长、`frameQ` 不为 0、不出现 `AFActiveVideoRender: drop a frame` 连刷 |


---

### ✅ 第八轮：DASH 最后一个分片丢失 + 高→低切换的预滚卡顿

证据来源：`appQtPlayer.log`（1657 行，23:59:28~00:01:39）+ **直接抓取的片源清单**
（`http://127.0.0.1:9000/video/output.mpd`，MinIO）。

#### 8.1 DASH 清单的真实结构（抓下来的）

```xml
mediaPresentationDuration="PT4M28.2S"        ← 268.2 秒
<SegmentTemplate timescale="1000000" duration="10000000" endNumber="27"
                 startNumber="1" media="$RepresentationID$-$Number$.m4s">
```
MinIO 实测对象：`…-24.m4s`✓ `…-25.m4s`✓ `…-26.m4s`✓ `…-27.m4s`✓（1.12MB ≈ 8.2 秒）；
`…-28.m4s` = **404**。

也就是：**分片编号 1~27，每片 10 秒，最后一片（27）是 [260s, 268.2s) 只有 8.2 秒的
"余量分片"** —— 和用户判断的完全一致。

#### 8.2 RC-AE ★ "seek 到最后少一段、直接跳结尾"

| 证据 | 结论 |
|---|---|
| `seek posUs=256796000` → `[seek] dash video: reqUs=256796000 -> segNum=26` → 播到 257.06s 之后立刻 `DashStream: EOS` → `read_callback : 66 stream(0),EOS` → `Player ReadPacket EOF` → 播放器跳到结尾 | `SegmentTemplate::getNextMediaSegment()` 里是 `if (i_pos >= endnum - 1) return nullptr;`，而 `endnum` 是**排他**上界（= startNumber + ceil(时长/分片时长) = 1 + 27 = 28）→ 最后一个合法编号 `endnum - 1 = 27` 被当成 EOS。**最后那 8.2 秒永远读不到** |
| 清单里的 `endNumber="27"`（还有 `endNumber` 这个字段）在框架里**从来没被解析过** | `duration` 只能算分片边界、算不出"一共几片"。余量分片不足一个 duration 时，按 duration 反推会多算出一片（27 片却算出 28），再叠加上面那个 `>=` 就正好把真正的最后一片吃掉 |

**修复**：
1. `MPDParser::parseCommonMultiSegmentBase()` 解析 `endNumber`（新增 `EndnumberAttr` /
   `inheritEndNumber()`，见 `InheritablesAttrs.{h,cpp}`）；
2. `getNextMediaSegment()`：`if (i_pos >= endnum)`（去掉 off-by-one），并且
   `endNumber` 存在时取 `min(duration 推算, endNumber + 1)` 当排他上界；
3. `getSegmentNumberByTime()`：结果夹到 `endNumber`，这样"seek 到超出最后一片的位置"
   会落在**真正的最后一片**上，而不是算出一个不存在的编号再去吃 404 → EOS。

#### 8.3 RC-AF ★ 从高分辨率切低分辨率"卡一下"

| 证据 | 结论 |
|---|---|
| `pending preroll starts at key frame: pts=41708333 timePosition=40000000 target=58514893 (preroll=18515 ms)`；这次切换 00:00:08.747 → 00:00:17.986 = **9.2 秒** | 上一轮的"关键帧边界换档"没做到位：代码在遇到**第一个**关键帧时就停下，而目标路是从"包含切换点的分片**起点**"开始喂数据的（这里 40.0s，切换点 58.5s），于是预滚 = **18.5 秒**。这 18.5 秒要在 9 秒内解完：4K 旧路和 480P 新路同时抢解码器/主循环 → 用户看到的"高切低卡一下"；切换慢也是它 |

**修复**：参考点（`mPendingVideoPrerollRefUs`，进入预滚那一刻取一次
`max(切换点, 主时钟)`，之后不变）**之前的所有包一律丢（关键帧也丢）**，只在遇到
"参考点之后的第一个关键帧"时才开始解码：
- 解码量 ≈ 0 → 新旧两路不会同时长时间解码 → 不卡；
- 解码起点 = 提交点 = 关键帧边界 → 音画绝对同步；
- 等待时间 = 到下一个关键帧的距离（本片源 DASH 10 秒分片、HLS 4.17 秒），
  期间**旧路照常播放**；
- 兜底：等超过 `PENDING_PREROLL_WAIT_MAX_MS = 6s` 还没等到（关键帧间隔特别长的片源），
  退化成"接受下一个关键帧，哪怕在参考点之前"，保证切换一定能完成。

#### 8.4 验收点（本轮新增）

| 判据 | 期望 |
|---|---|
| DASH 播到最后 | 不再出现 `DashStream: EOS` + `Player ReadPacket EOF`，最后一个余量分片正常播放到 268.2s |
| seek 到末尾附近 | `[seek] dash video: … -> segNum=27`（最后一片）而不是 26 之后 EOS；不得出现 404 之后跳结尾 |
| 预滚 | `pending preroll starts at key frame … (lead=N ms)`，`lead` 应 ≤ 一个关键帧间隔（DASH 10s / HLS 4.2s）；**不允许**再出现 `preroll=18515 ms` 这种十几秒的预滚 |
| 高→低切换 | 切换期间不出现 `AFActiveVideoRender: drop a frame` 连刷、`total fps` 不塌；等待期间旧画面持续播放 |
| 音画 | `committed … pts=X master=Y` 的 `\|X−Y\|` ≤ 150ms |


---

### ✅ 第九轮：首页新增"清单对象"播放入口（对象直接传入）+ macOS 零拷贝/DASH/HLS 能力核对

需求：*"对象传入播放的首页入口，就是直接将 manifest 对象传入实现播放；考虑能否实现 macOS 的
零拷贝和解码以及 dash 和 hls 的全部功能。"*

#### 9.1 加了什么

| 位置 | 内容 |
|---|---|
| `HomeWindow.qml` 工具条 | 新增 **"清单对象"** 按钮（在 DASH 地址 / HLS 地址 旁边）→ 打开 JSON 输入弹窗 |
| `HomeWindow.qml` `ManifestDialog`（新组件） | 多行 `TextArea`（等宽字体）+ 校验（必须合法 JSON、最外层是对象；没有 `video[]` 只告警）+ **"从 .json 文件载入"**（`FileDialog`） |
| `HomeWindow.qml` | 新 API：`openPlayerManifest(jsonText, name)`、`playManifestObject(obj, name)`、`openPlayerManifestFile(path)`（都复用同一个播放器窗口：`ensurePlayerWindow()` + `showPlayerWindow()`） |
| `Main.qml`（播放器窗口） | 新 API：`playManifestObject(obj, name)` / `playManifestJson(text, name)` / `playManifestFile(path)`；都先 `library = null`（和 `playUrl` 同一条理由：网络/对象片源不在播放列表里） |
| `CicadaPlayerItem` | 新 `Q_INVOKABLE bool loadManifestFile(const QString &path)`：**在 C++ 里读本地 .json**。原因：Qt 默认禁止 `qrc:` 文档对 `file://` 发 XHR（`QML_XHR_ALLOW_FILE_READ` 未开），QML 里读本地文件必然失败；读文件的活放在 C++，和命令行 `appQtPlayer.exe xxx.json` 那条路一致 |

**对象路径本来就是通的**（`setManifest` / `setManifestJson` → `MediaPlayer::SetDataSource(std::string json)`
→ `MediaManifestParser` → `ManifestDemuxer::buildPlayList()`），这一轮补的是**首页入口**和**文件载入**。

用法三种，等价：
```js
home.playManifestObject({ duration: 268.2, video: [ … ], audio: [ … ] })   // JS 对象
home.openPlayerManifest(jsonText, "output-mock.json")                     // 粘贴文本
home.openPlayerManifestFile("D:/…/test-dash-v5.json")                     // 本地文件
```

> **第十二轮更新**：上表里的 `ManifestDialog` / 三个入口函数都**扩展了协议参数**（见第十二轮 12.3 的第 6~8 条）：
> 按钮拆成"**DASH 对象** / **HLS 对象**"两颗，`openPlayerManifest(jsonText, name, protocol)`、
> `openPlayerManifestFile(path, protocol)` 多了可选的 `protocol`（`"dash"`/`"hls"`）；
> 组件侧新增 `setManifestForProtocol()` / `loadManifestFileForProtocol()`（按协议**补全**清单对象）；
> 文件选择框改从 C++ 调（`src/AppFileDialogs.*` + `AppDialogs` 单例），老的 QML `FileDialog` 只作兜底。

#### 9.2 macOS 零拷贝 + 硬解：**完全一致**（代码路径核对）

`CicadaPlayerItem::createPlayer()` 里，**解码器/渲染器/零拷贝的配置全在 `SetDataSource` 之前**，
和片源类型无关：

| 步骤 | 代码 | 和片源类型有关吗 |
|---|---|---|
| 注入应用自己的渲染器（零拷贝入口） | `m_player->SetVideoRenderingCallback(...)` → `CicadaVideoRender` → `CicadaVideoTexture::reset(window, allowZeroCopy)` | 无关 |
| 硬解开关 | `m_player->EnableHardwareDecoder(m_hardwareDecoding)` | 无关 |
| macOS 要求 VideoToolbox 输出 **32BGRA** | `SetOption("pixelBufferOutputFormat", kCVPixelFormatType_32BGRA)`（`Q_OS_MACOS` 分支） | 无关（在 Prepare 之前） |
| 片源 | `SetDataSource(std::string(json))` **或** `SetDataSource(path)` | ← **唯一的区别** |

所以对象片源在 macOS 上照样走：VideoToolbox 硬解 → `CVPixelBuffer(32BGRA)` → IOSurface →
`MTLTexture` → `QSGTexture`（`CicadaTextureMetal.h`），`zeroCopy` 属性也是同一条判据
（`m_hardwareDecoding && m_textureBackend->isZeroCopy()`）。

#### 9.3 DASH / HLS 功能核对（对象路径）

对象模型（`framework/demuxer/manifest/MediaManifest.h`）覆盖得很全：
`video[]/audio[]/subtitle[]` 三组 Representation、三种分片寻址
（`single` 字节区间 / `template` + `segmentTimeline` / `list` 显式列表，含
`startNumber`/`totalCount`/`suffix`/`mediaSequence`/`byteRange`）、直播配置
（`timeShiftBufferDepth`/`minimumUpdatePeriod`/`partTargetDuration`/hold-back）、
AES-128（`encryption.keyUrl/iv`）、DRM（`contentProtection[]`：Widevine/PlayReady/FairPlay/ClearKey
+ `licenseServer`）、多 Period、内容导向、UTCTiming。

**因此这些能力对对象片源全部有效**（因为它们都在 `MediaPlayer` 这一层之上，和"片源从哪来"无关）：
多码率清晰度列表（`MediaInfoGet`）、控制栏清晰度菜单、双路解码无感切换、ABR、seek、
音轨/字幕轨、AES-128/DRM、缓冲状态回调。

**一条要知道的差异**（代码事实，不是猜测）：`ManifestDemuxer::Open()` 里
```cpp
if (mManifest->mediaSourceType == "dash") {
    AF_LOGW("mediaSourceType dash: falling back to the HLS segment pipeline (DRM-capable)\n");
}
mPPlaylistManager = new HLSManager(mPPlaylist);
```
—— **对象片源一律走 HLS 分片管线**（`mediaSourceType` 只影响这一行日志），只有 **URL 片源**
（`.mpd` 文本）才走 `DashManager`/`DashStream`。这是有意为之（HLS 管线是带 DRM 的那条），
对静态 VOD 等价；但要注意两点：
1. 第八轮修的"最后一个余量分片"是 **DASH URL 路径**的 bug，对象路径不受影响、也不继承该修复；
2. 对象里的 `segmentInfo.mode = "template"` 由 HLS 管线按 `totalCount`/`segmentTimeline` 展开，
   **余量分片（最后一片时长不足）请用 `mode:"list"` 显式列出，或把 `totalCount` 写准** ——
   这样就不会出现"少一段"。

#### 9.4 验收点

| 判据 | 期望 |
|---|---|
| 首页入口 | 工具条出现"清单对象"；粘贴 JSON / 从 .json 载入 / `home.playManifestObject({...})` 三条路都能起播 |
| 日志 | `opening manifest: N 个视频档, M 字节 JSON` → `qualities: N 档`（清晰度菜单有内容） |
| macOS | 设置页诊断里 `zeroCopy=true`（对象片源与 URL 片源表现一致） |
| 功能 | 清晰度菜单可切、ABR 可升档、seek 正常、音轨/字幕轨可选（对象里给了就有） |
| 文件载入 | `manifest file: <路径> (N bytes)`（说明走的是 C++ 读文件，而不是 QML XHR） |


---

### ✅ 第十轮：DASH 切换后"卡一下"的根因 —— 提交后视频 PTS 跳回原始时间轴

证据来源：`appQtPlayer.log`（1736 行，00:35:25~00:39:19）。

#### 10.1 这一轮先确认了上一轮是有效的

切换**落点已经精确在关键帧边界上**，而且和主时钟只差 100ms 左右：
```
committed … pts=4170833  master=4024914
committed … pts=20000000 master=19854993
committed … pts=30000000 master=29871203
committed … pts=40000000 master=39870991
committed … pts=50000000 master=49866467
committed … pts=78341666 master=78201061
pending preroll starts at key frame: … (lead=635 / 1489 / 1843 / 1931 / 3144 / 3459 ms)
```
（`lead` 最多一个分片，不再是十几秒的预滚。）

#### 10.2 RC-AG ★ 但**每一次**提交之后都卡一下

```
00:36:37.030 committed … pts=30000000 master=29871203
00:36:38.037 read-ahead gate: video packet queue front 35902533 is 6017 ms ahead of master 30887487
00:36:38.037 video path stalled 1003 ms but the video is 2545 ms AHEAD of the master clock (frameQ=1)
00:36:39.046 video path stalled 1010 ms but the video is 1536 ms AHEAD of the master clock
00:36:40.058 video path stalled 1011 ms but the video is  524 ms AHEAD of the master clock
```
另几处是 `video path stalled 1002 ms, force rendered the newest frame`（看门狗兜底强出一帧）。

**提交后视频比主时钟超前 0.5~2.5 秒** → `RenderVideo()` 对"太早"的帧既不渲染也不丢弃
（只 `return false` 等时钟）→ **画面冻住等主时钟追上来** → 用户看到的"卡一下"。
而且等追平之后，画面**内容**比音频永久晚同样长的时间（这就是之前报的"音画不太同步"）。

#### 10.3 根因

DASH 的每个 Representation 都带一个**固定的 PTS ↔ timePosition 偏移**，日志里能直接量到：
```
pending preroll starts at key frame: pts=33366667 timePosition=30000000   → +3.37s
                                  pts=20854167 timePosition=20000000     → +0.85s
                                  pts=79245833 timePosition=78341666     → +0.90s
                                  pts=87587500 timePosition=84170833     → +3.42s
```

- **提交之前**：pending 帧在 `FillPendingVideoFrame()` 里**逐帧**用 `timePosition` 归一化
  （上一轮加的），所以提交的那一帧是对的（`pts=30000000` ✓）。
- **提交之后**：同一个解码器的后续帧改由 `FillVideoFrame()`（active 路径）交付，那里
  **没有 packet metadata**，只能靠 `mActiveVideoPtsOffset` 平移 —— 而这个偏移以前只在
  "压根没有 media time（timePosition < 0）"那种罕见情况下才建立，DASH 上它一直是
  `INT64_MIN`（不平移）→ **后续帧的 PTS 直接跳回原始时间轴，比主时钟超前 0.85~3.4 秒**。
- **HLS 为什么没事**：HLS 分片的原始 pts == timePosition，偏移恒为 0，跳不跳都一样。
  这就是"dash 切换 99% 卡一下、hls 很流畅"的全部原因。

#### 10.4 修复

| # | 改动 |
|---|---|
| 1 | `FillPendingVideoFrame()`：归一化的同时**记下这个偏移** `mPendingVideoPtsOffset = timePosition − rawPts`（同一档恒定，只记一次） |
| 2 | `TryCommitPendingVideoSwitch()`：把这个偏移带进 `mActiveVideoPtsOffset`（本来就有这一行），并且**不再对已经归一化过的帧重复平移**（加了 `frameTimePosition >= 0` 的判断，老路"没有 media time"仍然补加） |
| 3 | 于是 `FillVideoFrame()` 里那句 `pFrame->pts += mActiveVideoPtsOffset` 就能把**提交之后的所有帧**都拉到全局时间轴上 → 不再"超前"、不再冻帧、画面内容和音频同一时间轴 → 音画同步 |
| 4 | 预滚跳过的第一个包上**重新贴一次 codec 参数集**（SPS/PPS，从 stream meta 取）：跳过的包里本来就有"携带 extradata 的那一条"，丢过之后解码器开头会连刷 `Error while decoding frame -1094995529 :Invalid data found when processing input`（日志里实测 7 条） |

#### 10.5 验收点

| 判据 | 期望 |
|---|---|
| 提交后不得再超前 | 不再出现 `video path stalled … but the video is N ms AHEAD of the master clock`（N>200ms） |
| 提交后不得冻帧 | 不再出现 `video path stalled 1000 ms, force rendered the newest frame` |
| 音画 | 切换后 `committed … pts=X master=Y` 的 \|X−Y\| ≤ 150ms，且**下一帧**（走 active 路径）的 pts 也应 ≈ master（不再跳 +0.85~3.4s） |
| 解码器错误 | 切换开头不再连刷 `enqueue_decoder error -1094995529` |
| 体感 | DASH 低→高、高→低两个方向都不再"卡一下"，与 HLS 一致 |


---

### ✅ 第十一轮：DASH 切换"卡一下"的真身 —— 预热期间早帧不等时钟 → 渲染器被灌爆

证据来源：`appQtPlayer.log`（130KB，01:06:04~01:18:06，4K↔480P 来回切）。

#### 11.1 现象（三段一模一样）

```
01:06:04.163 quality switch started（4K → stream 0）
01:06:04.356 ~ 01:06:07.206  AFActiveVideoRender: drop a frame pts = 1167833 … 4938267
                             （3 秒内几十上百条，间隔 33~50ms）
01:06:05.166 [switch] pendingPktQ=120 pendingFrameQ=4 pendingFrontPos=4170833 master=1984796
01:06:06.170 … pendingFrontPos=4170833 master=2988956      ← 目标帧停在关键帧 4.170833s 等时钟
01:06:07.229 committed … pts=4170833 master=4043559        ← 落点精确，误差 127ms ✓
```
另外两段：`01:07:31.6~33.0`、`01:07:39.9~43.0`，同样是**预热期间**渲染器连续丢帧。

**关键**：丢帧发生在**提交之前**（预热等待关键帧边界的那 1.5~3 秒里），丢的是**旧流**的帧
（`pts=1.17s…4.93s`，正是当时正在播的内容）。

#### 11.2 根因（RC-AH）：预热阶段"早帧"也被直接推给渲染器

`RenderVideo()` 里那条"比主时钟早 10ms 以上就先不上屏，等下一轮"的判断，原本带着一个例外：

```cpp
if (videoLateUs < -10 * 1000 && !qualitySwitchWarming && …) return false;   // 早帧 → 等时钟
```

`qualitySwitchWarming`（= 有 pending 目标 / 待切解码器 / 已提交）在**预热期间就是真**，
于是这个例外把整段预热期的节拍**全部关掉**：凡是早于主时钟的帧（解码器跑在前面时可能早
几百毫秒）都直接往下走 → `render = true` → 推给 Qt 渲染器。

Qt 渲染器（`AFActiveVideoRender`）有自己的输入队列和呈现时钟，被灌进远早于呈现时刻的帧
只能丢掉多余的（`drop a frame`），并把自己的时钟重锚到最新帧 —— 画面于是**跳着走**，
用户看到的就是"切换的时候卡一下"。

这一条例外的初衷是"预热阶段旧路是唯一能显示的路，不能让它停住"，但它管错了方向：
那条 `return false` 只对**提前**帧生效；**迟到**帧本来就会走下面的正常分支
（`videoLateUs < 500ms → render`）渲染出去，所以去掉例外不会让旧画面停住。

#### 11.3 修复

| # | 改动 | 效果 |
|---|---|---|
| 1 | `RenderVideo()`：早帧判断**去掉 `!qualitySwitchWarming` 例外**，预热期间同样"早于时钟 10ms 就先不上屏" | 预热期间恢复按播放时钟节拍出帧 → 渲染器不再被灌爆 → `drop a frame` 连刷消失 → 切换过程**平滑**（旧画面正常播到关键帧边界，然后无缝换档） |
| 2 | `doReadPacket()` 读前闸门加两条限制：**切换在途一律不闸**、视频队列要有 **4 秒**（原来 2 秒）且**音频队列 ≥2 秒**（原来 1 秒） | 闸门原来会在"视频队列超前 6 秒"时把**整个读循环**停掉，读循环是按 demuxer 走的 → 音频一起断粮 → 音频渲染饿死 → 主时钟不再前进 → 视频被判成"超前 954ms"画面冻住等一个不动的时钟（01:07:34.053 `video path stalled 1000 ms but the video is 954 ms AHEAD …`）。现在闸门不会再制造这种连锁 |

#### 11.4 现状与取舍

* 切换**落点**没问题：`pts=4170833 master=4043559`（127ms）、`pts=90000000 master=89856683`（143ms），
  偏移也正确建立了（`offset=-1758333` / `offset=-100000` —— 第十轮的 PTS 归一化修复生效）。
* 切换**等待时间** = 到下一个关键帧边界的距离（DASH 10 秒分片平均 5 秒、HLS 4.17 秒平均 2 秒），
  这是"零预滚解码"的代价：等待期间旧画面正常播放、**不卡**，只是清晰度晚一点变。
  想更快就要允许解码"上一个关键帧之后的预滚"（`PENDING_PREROLL_KEEP_US` 调大），
  代价是切换瞬间多一小段解码（480P 约 0.2 秒 CPU、4K 约 1~2 秒）可能带来轻微竞争 ——
  这一轮选择"绝对不卡"。

#### 11.5 验收点

| 判据 | 期望 |
|---|---|
| **不再有渲染器丢帧** | 切换前后不再出现 `AFActiveVideoRender: drop a frame pts = …` 连刷 |
| 不再有"超前等时钟" | 不再出现 `video path stalled … the video is N ms AHEAD of the master clock` |
| 不再有主时钟冻结 | 切换期间 `master` 持续增长；不出现音频断粮导致的 `buffering start` |
| 落点 | `committed … pts=X master=Y`，`\|X−Y\| ≤ 150ms`，且 `X` 是关键帧边界 |
| 体感 | 4K→480P / 480P→4K 两个方向切换过程**画面连续、无跳动** |


---

### ✅ 第十二轮：seek 之后"要缓冲 + 卡一下"的真身（RC-AI）+ 协议徽标 + 清单对象按协议拆分 + 文件对话框改从 C++ 调

证据来源：`appQtPlayer.log`（69KB，10:06:26~10:07:51，DASH 上 seek）。

#### 12.1 现象

```
10:06:57.043 PFR: seek posUs=12333000 inCache=0 status=6
10:06:57.055 FlushVideoPath from ProcessSeekToMsg … seekFlag=1 seekNeedCatch=1
10:06:57.092 buffering start                        ← ①每次都弹"缓冲中"
10:06:58.196 renderer joining window started (seek finished) master=14848167   ← ②时钟在 14.85s
10:06:58.240 buffering end
10:06:58.850 audio clock re-anchored after seek at pts=12330667
10:06:58.863 read-ahead gate: video queue front 15165150 is 2821 ms ahead of master 12343544
10:06:59.815 video path stalled 1006 ms but the video is 1569 ms AHEAD   ← ③画面冻 ~2.5 秒
```

#### 12.2 根因（RC-AI）：seek 期间主时钟还停在"seek 之前"，追赶窗口把目标帧当迟到帧丢了

时序（三个线程交错，这就是"概率性卡一下"的来源）：

1. `ProcessSeekToMsg` 先 `ClearPacket(BUFFER_TYPE_ALL)`（缓存瞬间清空）；
2. 紧接着 `DemuxerService::Seek()` **阻塞**去取新 segment（DASH 实测 1 秒以上）；
3. 主循环此时看到"缓存空了" → 进入缓冲态 → `mMasterClock.pause()`。**注意此刻时钟还停在 seek 之前的位置（14.85s），
   因为把时钟钉到目标点的那句 `setTime(seekPos)` 写在函数末尾、要等第 2 步返回才执行**；
4. 新 segment 一到，解码器立刻解出目标帧并上屏 → 第 3039 行 `beginRendererJoining()` 用的是**旧时钟 14.85s**
   → 追赶窗口判定 12.33s~14.85s 的帧"迟到"，**包括 seek 目标那一帧**，全部丢掉；
5. 第 2 步返回，函数末尾 `setTime(12.333)` 把时钟拉回来，最后音频第一帧再锚一次 →
   但已经被丢掉的那 2.5 秒**再也拿不回来**了：解码器只可能给出 ≥14.85s 的帧（前面的包已被消费/丢弃），
   于是画面静止等时钟追上来 —— 用户看到的就是"seek 完之后卡一下"。

至于"seek 要缓冲"：`cur_buffer_duration <= 0` 在 seek 期间必然成立（刚被 `ClearPacket` 清空），
那条分支会 `NotifyLoading(loading_event_start)` → 界面弹"缓冲中"。但这不是网络卡，是 seek 自己的等待
（界面已经收到了 `NotifySeeking`），属于**重复提示**。

#### 12.3 修复

| # | 文件 / 位置 | 改动 |
|---|---|---|
| 1 | `SMPMessageControllerListener.cpp` `ProcessSeekToMsg()` | **一进函数就把主时钟钉到 seek 目标**（`mMasterClock.setTime(seekPos)`，紧跟 `mSeekPos = seekPos`）。这样无论第 2 步阻塞多久、seek 出来的第一帧多早到达，时间轴都已经是对的，追赶窗口只会丢真正属于"seek 之前"的帧 |
| 2 | `SuperMediaPlayer.cpp` seek 完成分支（`mSeekFlag` 置假之前） | **安全网**：宣告 seek 完成前若 `mPlayedVideoPts == INT64_MIN`（还没真的从新位置播过）且时钟**超前** `mSeekPos` 超过 `SEEK_CLOCK_TOLERANCE_US (300ms)`，就重新钉一次并打 `seek completion: master clock … AHEAD of seek target …, re-park it`。防止别处（音频参考时钟重同步、缓冲出口）再把时钟改写。只处理"超前"：时钟落后于目标是无害的（目标帧等时钟到点再上屏），往前拽反而会跳过内容 |
| 3 | `SuperMediaPlayer.cpp` `getAudioPlayTimeStamp()` | seek 之后音频路被 flush，`mAudioTime.startTime` 归零而**音频渲染器位置回调仍返回上一段播放的旧位置**（实测残留 ~2.5s）。此时 `SystemReferClock::GetTime()` 一旦拿它 reSync，主时钟会瞬间跳到"目标+2.5s"，同样导致丢目标帧。现在 `mSeekFlag` **或 `mAudioClockReanchorPending`** 为真时一律返回 `INT64_MIN`（音频时钟不可用），直到 seek 后第一张音频帧上屏时重新锚定 |
| 4 | `SuperMediaPlayer.{h,cpp}` 缓冲入口/出口 | seek 引起的缓冲**不再通知 UI**（新增 `mBufferingNotified` 记录"这次到底通知过没有"，出口按它配对发 `loading_event_end`），但**内部状态照旧**：暂停主时钟 + 暂停音频渲染，seek 完成时由缓冲出口统一 `start()`/恢复音频渲染。界面因此不再在 seek 时弹"缓冲中" |
| 5 | `PlayerView.qml` | 左上角新增**协议徽标**（DASH / HLS / FLV / MP4 / RTMP…，多码率协议还会标"自动/手动"），悬停显示 `protocolDetail`。排查时"这条到底是 DASH 还是 HLS"不再靠猜 |
| 6 | `CicadaPlayerItem.{h,cpp}` | 新增只读属性 `protocol` / `protocolDetail`（判断顺序：清单对象 → URL 后缀 → URL scheme）；新增 `setManifestForProtocol()` / `loadManifestFileForProtocol()`：**按协议补全清单对象**（推断 `mode`、从 `initialization` 推 `media` 模板、由 `segmentTimeline` 算 `totalCount`、补 `startIndex/targetDuration`、HLS 补 `mediaSequence`、写死 `mediaSourceType`），缺关键字段时把"缺什么"写进 `errorString()`。**只补不改**，已有字段一律不动 |
| 7 | `HomeWindow.qml` | "清单对象"一颗按钮拆成 **"DASH 对象" / "HLS 对象"** 两颗：各自带协议化的标题、占位示例、按协议的校验（`mediaSourceType` 不符 / 缺 `duration` / 缺 `video[]` / 缺 `segmentInfo` / template 缺 `initialization` / list 缺 `segments[]` 都会当场指出），新增**"填入示例"**一键填入能直接播放的完整对象（本机 MinIO 片源参数） |
| 8 | `AppFileDialogs.{h,cpp}`（新）+ `main.cpp` + `CMakeLists.txt` | 文件/目录选择框**改从 C++ 调**：`QPlatformTheme::createPlatformDialogHelper(FileDialog)` 造系统原生对话框，选完 `deleteLater()` 掉对象 —— **句柄当场释放**，不再依赖 "QML 动态创建 + destroy + 隔一拍" 那套。QML 侧 `import CicadaPlayer` 后用 `AppDialogs.openVideoFile/openJsonFile/openDirectory`；平台不支持（无 QtGui 私有头）时 `available()` 为 false，自动退回原来的 QML `FileDialog`/`FolderDialog` 路径 |

> 关于第 8 条的依据：附带的 `分析.md` 结论是"QML 的 `close()` 只隐藏窗口，组件对象和它底下的原生句柄
> （Windows 的 IFileDialog/COM、文件系统监视器）都还活着，连开几次就弹不出来 / 目录被占用；
> 要真正释放必须**销毁对象**"。QML 侧的"每次 createObject + accepted/rejected 里 `Qt.callLater(destroy)`"
> 是文档给的方案一/方案二；这里采用的是**同一思路的 C++ 版**（现建 → 阻塞等结果 → 立刻销毁），
> 少一层 QML 生命周期的状态交错。

#### 12.4 验收点（跑一次 DASH seek，逐条对日志）

| 判据 | 期望 |
|---|---|
| seek 不弹缓冲 | 不再出现 `buffering start`（`notifyBuffering` 那条）；改成 `seek in progress, parking clock at … without showing the buffering UI` |
| 时钟不跑偏 | `renderer joining window started (seek finished) master=…` 里的 master **≈ seek 目标**（差 < 300ms），不再是 seek 前的旧值 |
| 不丢目标帧 | 不再出现 `video path stalled … but the video is N ms AHEAD of the master clock`（N>200ms） |
| 画面 | seek 完成后**立刻**从目标点继续播，没有 1~3 秒静止 |
| 音画 | `audio clock re-anchored after seek at pts=…` 与 seek 目标差 < 100ms |
| 协议徽标 | 播放器左上角显示 `DASH`/`HLS`/`MP4`…；DASH/HLS 时旁边有"自动/手动" |
| 清单对象 | "DASH 对象"里点"填入示例"→ 播放 → 直接出画面；故意删掉 `segmentInfo` 会在对话框里看到"缺少 segmentInfo：…" |
| 文件对话框 | 连续开关 5 次以上都能正常弹出（句柄不再滞留）；日志里每次都有 `AppFileDialogs: 打开原生对话框 …` 与 `选中 …`，且**没有** QML FileDialog 的 `chooseFile: creating a fresh file dialog` |

---

### ✅ 第十三轮：第十二轮的回归修复 + seek 冻帧真因（RC-AJ）+ 清单对象两种格式

证据来源：`appQtPlayer.log`（142KB，12:32:37~12:42:xx，DASH 与 HLS 各一遍，多次 seek + 清晰度切换）。

#### 13.1 用户报告的现象

1. 不管 DASH 还是 HLS，**seek 时进度条先往后（向前）弹一下再跳回 seek 点**，而且 seek 之后画面冻 1~3 秒；
2. 每次 seek **缓冲条都重新加载**；
3. DASH 切清晰度"还是会卡"，有声音没画面；
4. **文件选择框完全打不开了**（点"打开"没反应）；
5. **DASH / HLS 传入 manifest 对象都没反应**；
6. 要求两种清单对象格式（`test-{dash,hls}-v4.json` 的模板分片 + `test-{dash,hls}-v4-explicit.json` 的显式分片）都必须支持。

#### 13.2 逐条根因与修复

| # | 现象 | 根因（日志证据） | 修复 |
|---|---|---|---|
| **RC-AJ** | ②③ 进度条先弹再回、seek 后冻 1~3 秒 | 第十二轮我给音频时钟加了 seek 期间的"不可用"守卫（`getAudioPlayTimeStamp()` 返回 `INT64_MIN`），于是 `RenderVideo()` 里那句"主时钟无效就拿视频帧 pts 兜底"被激活：`if (!haveMaster() \|\| !isMasterValid()) mMasterClock.setTime(videoPts)`。**seek 期间解码器是从目标点之前的关键帧开始无节流猛解的**（实测早跑到目标点后面 3.4 秒），时钟于是被钉到视频帧上 —— 日志：`renderer joining window started (seek finished) master=37721017`（目标 34.341s，差 +3.38s），12 ms 后音频把时钟拉回 `34346667`，追赶窗口已经把 34.3~37.7s 的帧（含目标帧）当迟到帧丢光 → `read-ahead gate: video queue front … 3662 ms ahead`、`video path stalled … the video is 2398 ms AHEAD`，画面干等时钟追 2~3 秒。进度条读的就是这个时钟，所以"先跳到 37.7s 再弹回 34.3s" | `RenderVideo()` 的兜底加 `&& !mSeekFlag`；纯视频（`!HAVE_AUDIO`）那条"第一帧锚时钟"同样加 `&& !mSeekFlag`。seek 期间主时钟**只**由 `ProcessSeekToMsg` 钉住，任何人不得改写 |
| 2 | seek 时缓冲条重新加载 | `ProcessSeekToMsg` 里 `NotifyBufferPosition(seekPos/1000)` 把"已缓冲位置"直接报成 seek 目标，而缓存刚被 `ClearPacket(BUFFER_TYPE_ALL)` 清空 → 界面先画到目标点，随后真实更新（≈0）又塌回来 | 删掉这句伪造，缓冲条只跟框架真实的 `BufferPositionUpdate` 走 |
| 3 | DASH 切清晰度卡 | 本轮日志里 DASH 切换本身是正常的（`committed … offset=-904167` ✓、READY ✓、退役流关掉 ✓）；"卡"的时段与 **seek 重叠**——那一分钟的 `KPI fps 40.6` 正好等于"6 次 seek × 每次冻 ~3 秒"的缺口。"切换后旧流不出帧"是 RC-AJ 的连带现象（时钟被推飞→追帧窗口丢帧） | 由 RC-AJ 修复覆盖；另外本轮日志确认切流终态、offset 归一化、退役流回收都正确 |
| 4 | 文件选择框点不动 | 日志原文：`AppFileDialogs: 打开原生对话框 …（shown=1）` → `Native file dialog: unable to get dialog's window.` → 4ms 后就 rejected。逐行核对 Qt 源码（`qwindowsdialoghelpers.cpp`）后的结论：`QWindowsDialogHelperBase::show()` 对**模态**对话框只做三件事 —— 建 IFileDialog 对象（在 GUI 线程上 `CoCreateInstance`，是 STA 对象）、记 owner 窗口、起一个 **0ms 定时器**；对话框真正 `Show()` 是在**那个定时器触发后由 Qt 起的工作线程**里跑（`QWindowsDialogThread::run` → `QComHelper(COINIT_APARTMENTTHREADED)` → `doExec`）。也就是说：如果调用方在 `show()` 之后自己跑事件循环，STA 的 COM 对象就被另一个线程直接用了 —— 实测就是 4ms 内被 rejected。上一版我只调了 `requestActivate()` 就 `show()` 并跑事件循环，正好踩在这条路上 | `AppFileDialogs`：① **第一优先策略改成 GUI 线程 `exec()`**：`show()` 之后**不跑事件循环**、直接 `helper->exec()` —— 它会停掉那个 0ms 定时器并在**当前线程**里跑 `IFileDialog::Show()` 模态循环（COM 对象和调用线程同一公寓）；② 剩下的 2 次尝试用"show + 嵌套事件循环"（上一版的路子）兜底；③ 每次重试前 `bringToFront()` = `raise + requestActivate` 并**等焦点窗口真的变成它**（最多 500ms，用嵌套事件循环而不是 sleep）；④ 判据"活过 700ms 才算用户取消，秒回 rejected 判为平台瞬时拒绝"→ 重试；⑤ 日志补 `策略=GUI线程exec/嵌套事件循环 / visible / focused / hwnd / shown`；⑥ `available`/`lastError` 改成**属性**（上一版是 Q_INVOKABLE，QML 里 `AppDialogs.lastError` 拿到的是函数对象 —— 日志里那句"不可用：function() { [native code] }"就是它）；⑦ main.cpp 打开 `qt.qpa.dialogs` 调试日志（那行 `doExec returns 0x…` 是判断平台为什么拒绝的唯一权威读数），并让卡死看门狗在对话框开着时跳过（`AppFileDialogs::isDialogOpen()`，否则用户在文件框里翻十几秒会被误判成"界面卡死"） |
| 5 | 清单对象没反应 | `CicadaPlayerItem::createPlayer()` 第一句 `if (m_player != nullptr \|\| m_source.isEmpty()) return;` —— 而**清单对象播放时 `m_source` 必然是空的**（`setManifestJson` 会主动清掉 source，两种片源互斥）。于是播放器**根本没建**：日志里 `setManifestForProtocol(dash): 校验通过` + `opening manifest: 8 个视频档` 之后就再没有任何播放器日志（没有 `preferAudio`、没有 Prepared） | 判据改成 `(m_source.isEmpty() && m_manifestJson.isEmpty())`；`componentComplete()` 里同一处判断也一起改 |
| 6 | 两种清单对象格式 | 参考实现的两套对象：`mode=template`（`segmentTimeline` + `totalCount`）与 `mode=list`（`segments[]` 显式 duration+url） | 归一化/校验本来就两条都认（template 推 `media`/算 `totalCount`，list 校验 `segments[]` 非空）；界面新增**两个示例按钮**（"填入示例（模板分片）"/"填入示例（显式分片）"），说明文字里写明两种格式都支持 |

#### 13.3 验收点

| 判据 | 期望 |
|---|---|
| seek 不再冻帧 | `renderer joining window started (seek finished) master=…` 的 master **≈ seek 目标**（差 < 300ms，允许"落后"）；不再出现 `video path stalled … AHEAD`（N>200ms） |
| 进度条不来回弹 | seek 期间位置/缓冲条不再先跳到目标之后的值再回来 |
| 缓冲条 | 不再"先窜到 seek 点再塌回" |
| 清单对象（两种格式都能播） | 点"填入示例（模板分片）"或"（显式分片）"→ 播放 → 出现 `opening manifest` 之后紧跟 `preferAudio 0`、`[dash]/[hls] 清单 … 解析完成`、`Prepared`、首帧 |
| 文件对话框 | 日志出现 `AppFileDialogs: 打开原生对话框 …第 1 次（策略=GUI线程exec … shown=1）`；开了 `qt.qpa.dialogs` 之后还能看到 `doExec returns 0x0`；连开多次都正常 |
| 文件对话框（兜底） | 若 3 次都失败：`连续 3 次都没能弹出原生对话框`，QML 侧自动改用 QtQuick.Dialogs 的 `FileDialog`；日志里 `qt.qpa.dialogs` 那行 `doExec returns 0x…` 会告诉我们确切的 HRESULT |

---

### ✅ 第十四轮：播放器窗口/标题栏 + 右键菜单 + 统计信息等面板（对照参考 DOM/CSS）

需求（用户原话）：播放器窗口缩小一点；自定义标题栏要"鼠标移上去就带背景地显示、停在上面就一直显示、移出再消失"（否则拖窗口时会点到播放器）；加播放器右键菜单和对应功能，参考 `front/hili-player/player（仅参考不要动里面的代码）`，能对接的实际功能都对接、没有的先写静态 UI，**包括 info 统计信息面板（里面的实际数据也要实现）**；对照 DOM/CSS 让 UI 完全一样。

#### 14.1 窗口与标题栏（`Main.qml` / `QtPlayerTheme.qml`）

| # | 改动 | 说明 |
|---|---|---|
| 1 | 默认窗口 `1280x720` → **`1080x608`** | 仍是 16:9，面积小约 29% |
| 2 | 标题栏**不再透明**：底色 `QtPlayerTheme.captionBarBg`（深 `#E61B1E24` / 浅 `#E6F2F3F5`，90% 不透明）+ 底部 1px `captionBarBorder` | 用户报的直接原因："透明 → 看不出哪一条能拖 → 按下去那一下被当成点画面（暂停）、拖拽时灵时不灵"。因为底色不再是被视频画面穿透，标题/图标颜色改为固定用 `titleText`/`captionGlyph`（原来要按 `hasSource` 在黑画面/窗口底色之间换色） |
| 3 | 顶栏加一层 `titleDragArea`（MouseArea，`hoverEnabled`） | ① `surfaceHovered` 进"要不要显示"的判据（**鼠标停在顶栏上就一直显示**）；② `onPressed` 里 `root.startSystemMove()` —— 任何平台都能拖（不只依赖 QWindowKit 的 HTCAPTION），并且这次"按下"被它吃掉，**不会再漏到画面区变成暂停**；③ `onDoubleClicked` 最大化/还原 |
| 4 | 收起仍是"高度归零" | 这是 QWindowKit 判定标题栏存在与否的依据，保持原样（见文件里那段说明） |

#### 14.2 右键菜单（新 `PlayerContextMenu.qml`，复刻 `component/context/index.ts` + `.player-contextmenu`）

菜单项文案与顺序**一字不差**：`复制视频地址（精准空降）` / `视频色彩调整` / `快捷键说明` / `播放器版本 1.0.0` / `视频统计信息`。
CSS 对应：整块 `rgba(33,33,33,.9)`(=#E6212121)、`border-radius 4`、`min-width 80`、每项 `font-size 12 / 行高 30 / 内边距 4px 20px`（= 38 高）、悬停 `hsla(0,0%,100%,.12)`、`opacity .1s cubic-bezier(0,0,.2,1)` 淡入、`.player-context-area` 的 `z-index:1000`。

| 菜单项 | 实际功能（本轮对接） |
|---|---|
| 复制视频地址（精准空降） | `CicadaPlayerItem::landingLink()`（普通地址 → `<url>?t=<秒>`；清单对象 → 写明"清单对象 + 位置"，不伪造 URL）+ `copyTextToClipboard()`（QML 没有剪贴板 API，在 C++ 里做）→ 弹 toast "已复制：…" |
| 视频色彩调整 | 打开色彩面板（见 14.3；**核心无此接口**，滑块仅 UI + 面板内一行说明） |
| 快捷键说明 | 打开快捷键面板（列表 = 本轮真正实现的快捷键，见 14.4） |
| 播放器版本 1.0.0 | 参考里点了没反应；这里弹 toast 显示同一句文案（不做假功能，也不留"点了没反应"的死项） |
| 视频统计信息 | 打开统计面板（**全部真实数据**，见 14.3） |

#### 14.3 三个面板 + 提示条（新 4 个 QML 文件；数据来自新 `CicadaPlayerItem::stats`）

`stats` 是只读 `QVariantMap`，2Hz 刷新（值没变不发信号），来源全部是框架真实读数：
分辨率/码率 → `GetCurrentStreamInfo`；源帧率/编解码器 → `GetCurrentStreamMeta`（**用完 `releaseMeta`**，否则每次刷新泄漏一份 strdup 的字符串）；
渲染帧率 → `GetOption("renderFps")`；丢帧/总帧 → `GetOption("videoDroppedInfo")`；缓存速度 → `CurrentDownLoadSpeed` 回调（bps）；
协议/容器 → `GetPropertyString(PROPERTY_KEY_CONTAINER_INFO)`；连接地址 → `GetPropertyString(PROPERTY_KEY_CONNECT_INFO)`；解码方式 → 本组件的 `backend()`（零拷贝/CPU 回拷，真实生效的那条路）；
编解码器字符串/mimeType → 核心只有 `AFCodecID` 枚举（没有任何字符串映射函数），所以自己翻成 `H.264/AAC`…，**清单对象那条路直接取清单 JSON 里的 `codecs`/`mimeType`/`baseUrl`**（唯一带这些字符串的地方）。

| 面板 | 复刻的参考 | 数据来源 / 诚实标注 |
|---|---|---|
| `PlayerInfoPanel.qml`（统计信息） | `videoinfo/index.ts` + `index.scss`：`left:10 top:10`、`bg rgba(33,33,33,.9)`、标题 40 行高 16px + 底部 1px 分隔线、内容 `padding:15px 26px 0`、标签宽 100 右对齐、值 `#999` 12px 最宽 470 省略 | 参考那 9 行 + 我们多给 7 行（渲染帧率/丢帧/缓冲时长/位置/解码方式/容器/HDR）。**两处如实标注**：视频/音频单独缓存速度（核心只给一个合计值）、媒体类型里的 codecs（普通 URL 片源只能给枚举翻译）。**有意不照抄**参考 CSS 里 `.info-title{text-indent:-99em}`（那是把标签推到屏幕外、只剩值的 bug，抄了面板就没法用） |
| `PlayerColorPanel.qml`（色彩调整） | `colorpanel/index.ts` + `index.scss` + `.ui-slider.ui-dark`：居中 400 宽、标题同款、三行 `饱和度/亮度/对比度`（名称 50 + 滑块 + 数值 40）、重置按钮底 `hsla(0,0%,100%,.3)`/hover `.4`、滑块轨道 2px `#505050` + 已填充 `#00a1d6` + 12px 圆点、取值 0~255 默认 100（参考 `scaleX(0.392157)=100/255`） | **核心没有色彩调整接口**（详查：`MediaPlayer` 无此类方法；唯一的视频滤镜框架 `SetFilterConfig` 是死代码 —— `ffmpegVideoFilter::is_supported()` 恒 false、`setOption()` 恒 false）。用户允许"没有的先写静态 UI"，所以滑块只动 UI，面板底部有一行"当前播放核心未提供色彩调整接口，以上数值仅作界面预览"，拖滑块时日志也打一行 warning。真实实现路径（后续可做）：Windows 在 `CicadaTextureD3D11` 的 D3D11 视频处理器上 `SetProcAmpValues`（亮度/对比度/饱和度/色相，零拷贝不受影响）；macOS 用 CIFilter |
| `PlayerHotkeyPanel.qml`（快捷键说明） | `hotkeypanel/index.ts` + `index.scss`：居中 400 宽、标题同款、列表区 `margin 10px 0 / max-height 268 / 隐藏滚动条`、每行 24 高、名称列 120、说明列 190 `#999` | 列表 = **本轮真正接上的**快捷键（见 14.4）；参考那份里的 `E 收藏`/`Enter 发弹幕`/`[ ] 多P` 本项目没有对应功能，**没有列**（列了就是按了没反应的假快捷键） |
| `PlayerToast.qml`（提示条） | `toast/index.scss` 的 `.player-toast-wrap`：`bottom:70 left:10`、`bg rgba(0,0,0,.8)`、`radius 4`、`line-height 36`、`font-size 14`、`padding 0 10px`、`transition all .3s`、`pointer-events:none` | 复制链接 / 版本号 / 静音与弹幕开关 / 倍速状态 |

#### 14.4 快捷键（本轮新实现，`Main.qml`；面板里列的就是这一组）

用窗口级 `Shortcut`（不需要焦点元素）：

| 键 | 动作 | 备注 |
|---|---|---|
| `Space` / `K` | 播放-暂停 | 输入框聚焦时不会误触发（Qt 先把按键给焦点项） |
| `→` | 快进 5 秒；**按住 → 2 倍速** | 键盘没有"抬起"事件，用两次触发的间隔判按住（>900ms 算单击，≤900ms 算按住），停手 500ms 恢复 1 倍速 |
| `←` | 快退 5 秒 | |
| `↑` / `↓` | 音量 ±10% | |
| `M` | 静音开关 | + toast |
| `D` | 弹幕开关 | 直接切 `controlBar.danmakuEnabled`（它驱动 `danmakuLayer`） |
| `F` | 全屏 / 退出全屏 | |
| `Esc` | 退出全屏；**有面板时先关面板** | 两颗 Esc 互斥（同时 enabled 会 "Ambiguous shortcut overload"） |
| 媒体键 play/pause | 播放-暂停 | `StandardKey.MediaPlay / MediaPause / MediaTogglePlayPause` |
| `Ctrl+O` / `Ctrl+D` | 打开文件 / 灌示例弹幕 | 原有 |

#### 14.5 验收点

| 判据 | 期望 |
|---|---|
| 窗口 | 默认 1080x608（比原来小一圈） |
| 标题栏 | 鼠标移到窗口最上面：出现一条**有底色**的标题栏；鼠标停在上面一直显示；移出约 300ms 后消失 |
| 拖窗口 | 在标题栏上按住拖动 → 窗口跟着走，**并且画面不会暂停/播放**（不会再"点到播放器"） |
| 右键菜单 | 画面区右键 → 菜单出现在光标处；点外面关掉；`复制视频地址` 后剪贴板里有 `…?t=<秒>`，界面弹 "已复制：…" |
| 统计信息 | 面板里：分辨率/码率/Host/缓存速度 都是当前片源的真实值；渲染帧率/丢帧随后台播放变化（`renderFps` 空闲是 0）；换片源后数字换新 |
| 色彩调整 | 面板与参考一致；拖滑块数值变、出现"仅界面预览"提示；日志有 `[color] … = N（播放核心没有色彩调整接口…）` |
| 快捷键 | 面板列出的每一个键按下去都有反应（可逐条试） |
| 编译 | `[100%] Built target appQtPlayer`，无新增 warning |

---

### ✅ 第十五轮：文件对话框不再堵 UI + 鼠标指针/闪烁/提示文字 + seek 进度条地板 + 切换花屏防线 + 统计曲线

证据来源：`appQtPlayer.log`（81KB，16:02:16~16:05:xx，DASH/HLS 清单对象播放 + 多次 seek + 清晰度切换 + 两次文件选择）。

#### 15.1 先把上一轮**已经修好**的确认下来（日志实证）

| 项 | 日志证据 |
|---|---|
| seek 不再冻帧 | `16:02:39.127 PFR: seek posUs=229073000` → `16:02:41.618 renderer joining window started (seek finished) master=229077396`（目标 229.073s，**差 4ms**）；`209705000` → `master=209706815`（差 2ms）。**没有** `video path stalled … AHEAD`、**没有** `buffering start` |
| 文件对话框能弹出 | `AppFileDialogs: 打开原生对话框 “选择清单 JSON 文件” 第 1 次（策略=GUI线程exec … shown=1）` → 10 秒后 `选中 D:/…/test-hls-v4-explicit.json` ✓ |

#### 15.2 本轮修的问题

| # | 现象（用户报告） | 根因 | 修复 |
|---|---|---|---|
| 1 | 点"打开文件选择框"有时**未响应**、播放窗口出问题 | 上一轮为了绕开"跨公寓用 COM 对象导致 Show() 失败"，改用了 **GUI 线程 `exec()`** —— 对话框能弹出来（上面日志实证），但它**阻塞 GUI 线程**：我们的视频帧回调是 `update()` **队列投递到 GUI 线程**的，GUI 一堵画面就不再刷新，Windows 也会把窗口标成"(未响应)" | `AppFileDialogs` 改成**在工作线程里跑原生对话框**：`QThread::create` → 线程内 `CoInitializeEx(APARTMENTTHREADED)` + **建 helper + show + exec 全在这条线程**（COM 对象和调用线程同一公寓 ✓）；GUI 线程只跑一个**嵌套事件循环**等它结束（视频、QML、定时器、鼠标指针全都照常）。第 1、2 次尝试走工作线程；都失败才退回"GUI 线程 exec"兜底（并在日志里写明会短暂阻塞界面） |
| 2 | 鼠标指针在原来那些按钮上不再是小手、一直是箭头 | 上一轮加的右键层是一个**铺满画面的 MouseArea**（`anchors.fill: player; z:999`）。Qt 决定光标形状时取"最上面设置了光标的元素"，而 MouseArea 即使只接右键、即使 `hoverEnabled:false`，它的 `cursorShape` 默认也是**箭头** → 把控制栏那些小手全压住了 | 换成 **`TapHandler`（`acceptedButtons: Qt.RightButton`）**：手势处理器不设置光标、也不独占 hover，只认右键那一下 |
| 3 | 顶部标题栏鼠标移上去**一直闪烁** | 上一轮把"顶栏自己的 hover"（`titleDragArea.containsMouse`）加进了判据，但热区（topHotZone）在标题栏**下面**：鼠标移到顶部 → 热区拿到 hover → 标题栏露出（高 36）→ 热区被盖住、hover 被转走；而 MouseArea 的 `containsMouse` 在"鼠标没动、元素刚出现"这一帧不一定立刻为真 → 三个来源同时为假 → 300ms 后收起 → 热区又拿到 hover…… 如此循环 | 把热区提到标题栏**之上**（`z: 5`）：它用的是**被动** HoverHandler（不独占 hover、不参与按键分发），所以只要光标在那 36px 里 `topHotHover.hovered` 就恒为真 → 顶栏稳定不闪；下面的拖拽区/按钮照样能收 hover 和点击 |
| 4 | 播放视频时"按 Ctrl+O 选择本地视频…"那行字**不消失** | 那行字的判据是 `player.source === ""`，而**清单对象播放时 source 是空的**（setManifestJson 会清掉它）→ 清单对象播放时提示一直挂着 | 新增 `CicadaPlayerItem.hasMedia`（= 有地址 **或** 有清单对象）+ `hasMediaChanged` 信号，提示改用 `visible: !player.hasMedia` |
| 5 | seek 时进度条"点了跳过去、又跳回原处、最后才跳到 seek 点" | C++ 侧的时间轴已经修好（15.1），剩下的是**UI 层**：seek 一发起我们就乐观地把目标报给界面，但框架在 seek 走完前仍可能推来几条**旧位置**的 PositionUpdate（渲染线程那帧还没换 / 消息队列里排着 seek 之前那条）→ 进度条往回跳 | `CicadaPlayerItem`：`issueSeek()` 立刻把目标写进 `m_position` 并发 `positionChanged`，同时记下 `m_seekUiFloorMs`；`notifyPosition()` **丢掉所有比地板小的位置更新**；SeekEnd / 看门狗超时后撤掉地板（`finishSeek`） |
| 6 | 清晰度切换**有时出现马赛克/花屏** | 日志实证：提交后紧跟一簇 `avcodecDecoder: Error while decoding frame -1094995529`（AVERROR_INVALIDDATA，见 16:04:54.946 起）。新解码器刚接手那几帧，参考帧链/参数集/视频处理器输入格式都还在建立中，个别坏帧会送到屏幕上 | 提交时置 `mQualitySwitchHoldFrames = 3`：**新流的前 3 帧不上屏**（画面保持上一帧，约 50ms），解码照常进行、参考帧链在解码器内部继续建立；`RenderVideo()` 里那三帧按 discard 出队。代价 50ms 落点，换来不闪花屏 |
| 7 | 统计信息面板要有**曲线图**，且"只有卡片打开才显示、关闭就停止监听" | 新需求 | `CicadaPlayerItem` 新增 `statsHistory` / `statsSampling` + `startStatsSampling()` / `stopStatsSampling()`：500ms 采一个点（`{t, speed(B/s), fps, buffer(s)}`），上限 60 点（30 秒窗口）；`PlayerInfoPanel` 用 **Canvas** 画三条折线（缓存速度/渲染帧率/缓冲时长，含当前值与峰值）；`PlayerView` 在面板 `onOpenedChanged` 里 start/stop —— 关掉即停定时器并清空历史（省 CPU/内存） |
| 8 | 对话框选完/关闭后要**确认**资源与句柄都释放 | 复查 | 工作线程路径：helper 与它内部的原生对话框都在**同一条线程**创建，`exec()` 返回后 `hide()` → `delete helper` → `sendPostedEvents(DeferredDelete)`（Qt 内部对 `m_nativeDialog` 用的就是 deleteLater，那条线程没有事件循环，不主动派发会挂到进程结束）→ `CoUninitialize()` 配对；日志新增 `对话框对象已销毁（原生句柄已释放，worker 线程）` 供核对。GUI 兜底路径用 `deleteLater()`（GUI 线程有事件循环） |

#### 15.3 验收点

| 判据 | 期望 |
|---|---|
| 文件对话框 | 日志 `策略=工作线程exec` 且 `shown=1`；**打开期间视频继续播、窗口不"(未响应)"、鼠标指针正常**；选完/取消后紧跟 `对话框对象已销毁（原生句柄已释放，worker 线程）` |
| 鼠标指针 | 控制栏按钮/进度条上仍是小手；画面区右键仍能开菜单 |
| 标题栏 | 鼠标移到顶部：出现且**不闪**；停在上面一直显示；移出后约 300ms 消失；拖动不会点到画面 |
| 提示文字 | 清单对象播放时那行"按 Ctrl+O…"消失 |
| seek 进度条 | 点/拖进度条：只前进不来回弹，最终停在 seek 点 |
| 切换花屏 | 切换提交时日志出现 `(holding 3 new frames to avoid a corrupt first frame)`，画面不闪马赛克 |
| 统计曲线 | 打开统计面板：三条曲线 0.5 秒一个点长出来（有当前值/峰值）；关闭面板后日志 `stats curves: sampling stopped`，再打开重新开始 |

---

### ✅ 第十六轮：DASH/HLS 缓冲从 40 秒收到 15 秒（省网络与内存，换档不再白下）

用户要求："dash 和 hls 的缓冲不要缓冲太多，不然切换清晰度后缓冲的都直接没用了，节省网络资源和内存"。

#### 16.1 现状（日志实证）

* 框架默认 `MAX_BUFFER_DURATION_DEFAULT = 40 秒`（`mediaPlayer/player_types.cpp`）。实测日志里读到过
  `read-ahead gate: video packet queue front … (activeQ=2923 queued=48764 ms)` —— 包队列攒了 **48 秒**。
* 每次换档都会作废：`dropped 2761 stale video packets of the retired stream at commit`。
  缓冲越厚，这一下白扔的越多：**4K 10Mbps 40 秒 ≈ 50MB 白下**（1080p 5Mbps ≈ 25MB）。
* 分片流（DASH/HLS）和连续流不一样：分片是**离散、可按需取**的单元，留 40 秒没有任何收益，
  真断粮了框架本来就会进缓冲态等分片。

#### 16.2 改动

| 片源类型 | `maxBufferDuration` | `highLevelBufferDuration` | `startBufferDuration` | 依据 |
|---|---|---|---|---|
| **DASH / HLS**（清单对象、`.mpd`、`.m3u8`） | **15000**（15 秒） | 5000 | 1000 | 15 秒 = 1.5 个 10 秒分片（或 ~3.6 个 4.17 秒分片），足够吃掉一次网络抖动；内存/白下都降到原来的 ~1/3 |
| 本地文件 | 5000 | 3000 | 500 | 原有策略不变（磁盘随手可读） |
| 其它网络源（http mp4、rtmp…） | 框架默认 40 秒 | 默认 | 默认 | **连续流**没有"换档作废"问题，缓冲厚才抗抖动 |

落点：`platform/QtPlayer/src/CicadaPlayerItem.cpp` 的 `createPlayer()`（必须在 `Prepare()` 之前，
和 `pixelBufferOutputFormat` 同一个道理），新增 `isSegmentPlaylistUrl()`（只看路径后缀、忽略
query，认不出来按连续流处理）。Prepare 时会打一行
`segment stream (DASH/HLS): buffer capped at 15s (maxBuffer=15000ms high=5000ms start=1000ms) …`。

**ABR 阈值必须跟着上限走**（`mediaPlayer/abr/AbrBufferAlgoStrategy.cpp`）—— 这是本轮的另一半改动：

| 常量 | 旧值 | 新值 | 为什么 |
|---|---|---|---|
| `ABR_LOW_BUFFER_MS` | 10 s | **7 s** | 15 秒上限下，"缓冲不够"要能真的被触发；连续 3 秒低于 7 秒才降档 |
| `ABR_HIGH_BUFFER_MS` | 25 s | **15 s** | 25 秒在上限 15 秒下永远到不了（只用于一行调试日志，但数值要自洽） |
| `ABR_MIN_BUFFER_FOR_UP_MS` | 10 s | **8 s** | ExoPlayer 默认 10 秒；在上限 15 秒、读循环"填满即停"的锯齿里，10 秒偶尔会误判成"缓冲不够"而卡住上切 |
| `ABR_MAX_BUFFER_FOR_DOWN_MS` | 25 s | **15 s** | 同上（跟上限对齐） |

用户要的语义没变：**到最高档且缓冲充足就不降**；只有缓冲被吃薄（连续 3 秒 < 7 秒）或见底
（< 4 秒）才降。健康网络下稳态缓冲在 12~15 秒 → 既不降档、也够得到上切门槛（≥8 秒）。

#### 16.3 验收点

| 判据 | 期望 |
|---|---|
| 上限生效 | Prepare 后有一行 `segment stream (DASH/HLS): buffer capped at 15s …` |
| 缓冲区变小 | `read-ahead gate: … queued=` 从 48000ms 量级降到 **15000~25000ms**（含正在下的那一个分片） |
| 换档浪费变小 | `dropped N stale video packets of the retired stream at commit` 的 N 明显变小（约为原来的 1/3） |
| ABR 仍然正常 | 稳态日志 `ABR switch up: … buffer=12000~15000 ms`（能上切）；被限速时才出现 `ABR down-switch: buffer stayed below 7000 ms for 3 ticks` |
| 播放体验 | 起播更快（start 1 秒）、播放不因为缓冲变小而频繁进缓冲态（`buffering start` 不应变多） |

---

### ✅ 第十七轮：五个界面细节修复（文字居中 / 菜单 hover 底色 / 曲线与面板高度）

用户逐条报的观感问题，全部是**布局**问题（CSS 与 QML 的排版模型差异），逐条定位：

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | 高能进度条右边那颗 pin 的气泡（"打开/关闭《高能进度条》常驻"）：**文字垂直不居中、黑底有点高、文字偏上** | 两处：① 高度用了 `Text.implicitHeight`，而 QML 的 implicitHeight 走**字体度量**（12px 字体自然行高 ~14-16px × 1.5 = 21-24px），算出来比参考的 CSS 公式（1.5×12 + 2×4 = 26px）高 3~6px；② CSS 的 `line-height: 1.5` 对单行来说等于"把字形摆在这 18px 行盒中间"，而 QML 的 `lineHeight` 只把多出来的行距**加在字形下面** → 字偏上 | 高度改成按参考公式算 `round(fontSize × lineHeight) + 2×paddingV`；文字块高度 = 外框高 - 上下 padding，`verticalAlignment: AlignVCenter`，并**去掉 lineHeight**（单行不需要，居中的活交给对齐） |
| 2 | 清晰度切换提示框：**文字不居中，后面的 × 却是居中的** | `Row` 只按 y=0 从左往右摆、**不做垂直对齐**：13px 的提示文字和 20px 的 "×" 高度不同，各自贴顶 → 大字号的 × 看起来居中、小字号的提示偏上 | 两个子项都加 `anchors.verticalCenter: parent.verticalCenter` |
| 3 | 右键菜单：**子项鼠标移上去背景不是宽度撑满的** | 上一版每个 `li` 用自己的文字宽（`文字宽 + 40`），而菜单背景按 Column 的 implicitWidth（= 最宽一项）撑开：窄的那几项 hover 底色只到自己那一小段。CSS 里 `li` 是块级元素，天然占满整个 `ul` | `Column` 宽度 = 最长一项，所有 delegate `width: menuColumn.width`（每一项都撑满菜单宽度） |
| 4 | 统计面板里的**曲线图没显示出来** | 曲线用的 `Canvas` 在面板**隐藏**时就被创建了，那时的绘制结果在第一次真正可见时不保证会重新提交；另外重绘只挂在"数据变化"上 | Canvas 的重绘触发点补全为三个：`series` 变（新采样点）、`tick` 变（面板打开时 `repaintTick++`）、`Component.onCompleted` / `onVisibleChanged`；并把数据/上限都绑成 Canvas 自己的属性（不再用 Connections） |
| 5 | 统计面板**高度太低、底部文字溢出** | 面板高度是"标题 + 明细 + 曲线"三块**相加**算的，漏掉了两处外边距（title 下 15px、曲线块上 15px）以及底部那行采样提示的高度 | 内容收成**一棵树**（`contentColumn`：标题 → 15 → 明细 → 分隔线 → 补充项 → 分隔线 → 曲线 → 采样提示），面板高度 = `contentColumn.height + 15`（参考的 padding-bottom），并加 `clip: true` 兜底；行的左右内缩改用**子元素 x** 表达（positioner 只管 Y，不依赖 Column 对 X 的处理） |

验收：① pin 气泡文字垂直居中、黑底高度回到 26px；② 清晰度提示文字与 × 同高居中；③ 右键菜单每一项 hover 底色都横贯整个菜单；④ 打开统计面板立刻看到三条曲线（0.5 秒一个点）；⑤ 面板底部不再有文字溢出（`clip` 之后结构上不可能溢出）。

---

### ✅ 第十八轮：清晰度「自动」角标 + 面板顺序 + 菜单宽度 + 曲线改 Shape + 真实色彩调整 + 快捷键滚动条

| # | 需求/问题 | 根因与修复 |
|---|---|---|
| 1 | 控制栏清晰度按钮要显示**当前清晰度**，并加一个**只有自动模式才显示**的「自动」角标 | 按钮文字改成**永远显示当前在播那一档**（自动模式下就是 ABR 选中的那档，取 `qualities[qualityIndex].label`）；角标按用户给的 DOM/CSS 复刻：`.bpx-player-ctrl-quality-result-auto-badge` → `position:absolute; left:100%; margin-left:-12px; margin-top:-10px; height:11px; font-size:7px; line-height:1; padding:0 2px; border-radius:3px; background:hsla(0,0%,100%,.12); border:.5px solid hsla(0,0%,100%,.3); color:hsla(0,0%,100%,.5)`，全屏档 `height:13px / font-size:8px / margin-left:-14px`。QML 里 `border.width` 只能是整数，用 1px + 40% 白近似（CSS 的 0.5px 做不到），已在注释里写明 |
| 2 | 清晰度面板：**「自动」放到最下面**；未选自动时显示"自动"，选中后这一项里显示**当前真实清晰度** | `contentItem` 里手动档 `Repeater` 在前、自动档单独一项在最后；`hoverIndex` 语义同步改成 `0..n-1 = qualities[i]，n = 自动`。自动项选中时右侧多一段灰字 = `qualityButton.currentLabel`（当前真实在播的那一档） |
| 3 | 右键菜单**变得很窄、文字显示不全** | 上一版写成"Column 宽 = max(80, implicitWidth)，delegate 宽 = menuColumn.width" —— **循环依赖**（Column 的 implicitWidth 本来就由子项宽度决定），Qt 解析不出来，宽度塌成 80。改成用 **TextMetrics 量最长文案**独立算出 `menuWidth`，Column 与所有 delegate 都用它（无任何自引用），既不会循环、每项也撑满整条菜单 |
| 4 | 统计面板曲线**还是不显示**（本地文件 / DASH / HLS 都一样） | Canvas 是**命令式**绘制（只在 `requestPaint()` 时更新，而"父项从隐藏变可见"不一定触发到它）。改用 **`Shape` + `ShapePath` + `PathPolyline`**：折线点 `polyline` 是普通属性绑定（依赖 series / width / height），数据或尺寸一变场景图自动重画 —— 不存在"什么时候重画"的问题。同时 C++ 每 10 个采样点打一行 `stats curves: N points (last: …)`，用来区分"没采到数据"和"没画出来" |
| 5 | **把视频色彩调整真正实现出来** | 落在渲染后端：Windows 的 **D3D11 零拷贝路径**本来每帧就要用视频处理器把 NV12/P010 转 RGBA，而视频处理器自带 `BRIGHTNESS / CONTRAST / SATURATION / HUE` 四个过滤器（`ID3D11VideoProcessorEnumerator::GetVideoProcessorFilterRange` + `ID3D11VideoContext::VideoProcessorSetStreamFilter`，`d3d11.h:10473-10476`）——在那里应用**零额外代价**（不额外拷贝、不退出零拷贝）。取值 0~200（100 = 中性）按驱动的 min/max 线性映射，中性时直接 `Enable=FALSE`；GUI 线程拖滑块 → `CicadaPlayerItem::setColorAdjust()` → `CicadaVideoTexture`（原子量 + 脏标记）→ 渲染线程 Blt 前 `applyColorAdjust()`，每帧零代价。macOS(Metal) / Linux(VAAPI) / CPU 回退返回 `colorAdjustSupported() == false`，面板如实提示"仅界面预览" |
| 6 | 快捷键面板**要把滚动条显示出来**、内部内容溢出 | ① 面板高度少算了列表区下面那 10px（`margin:10px 0`），最后一行被切 → 高度改成 `标题 + 10 + 列表 + 10`；② 滚动条从 `AlwaysOff`（参考的行为）改成 **`AsNeeded` + 细样式**（用户明确要求显示）；③ 列表内容宽/高按滚动条宽度让位，行内"名称 120 + 说明 190"这一对**居中**（参考是 text-align:center） |

编译：`[100%] Built target appQtPlayer`，无新增 warning。

---

### ✅ 第十九轮：清晰度「自动（实际清晰度）」+ 曲线改矩形折线 + 色彩调整「重置」失效

| # | 需求/问题 | 根因与修复 |
|---|---|---|
| 1 | 清晰度面板的自动项要显示成 **自动（实际清晰度）**，且**整段用和"自动"一样的激活色** | 之前是"自动" + 另一段灰色清晰度文字。改成**一个** Text：选中自动时文案拼成 `自动（<当前在播那一档>）`，颜色统一用 `qualityActiveText`（和手动档选中同一个高亮色）。档位还不知道时（`qualityIndex < 0`，例如刚起播）只显示"自动"，避免出现"自动（自动）" |
| 2 | info 面板曲线**还是没渲染**（第三次反馈） | 这块前后换过三种画法，前两种都败在"绘制上下文"上：Canvas 是命令式的（面板从隐藏变可见时不一定重画），Shape/PathPolyline 在这台机器上也是空白（而且 **Shape 只渲染自己的 `ShapePath` 子项** —— 当时塞在里面的两条 hairline Rectangle 根本没被画出来）。现在改成**一段一段的细 Rectangle + rotation**：折线本来就是若干线段，每段一个 1.6px 粗的矩形旋转到位，只用最基础的矩形几何，不依赖任何绘制上下文/材质/后端。**另外**面板底部那行自检文字现在直接显示 `N 个采样点｜速度 …｜帧率 …｜缓冲 …`，曲线画不出来时一眼能区分"没采到数据"还是"没画出来"（同时 C++ 每 10 个点打一行 `stats curves: N points`） |
| 3 | 色彩调整**重置后滑块不归位** | `ColorSlider` 自己声明了 `value`，而拖动时代码里写了 `slider.value = …` —— **QML 的赋值会打断绑定**（那个滑块本来就绑着 `value: colorPanel.saturate`），于是滑块从此只认自己的值：`reset()` 把面板属性改回 100，滑块照样停在老位置。改成**单向数据流**：拖动只 `moved(v)` → 父级写面板属性 → 属性经绑定流回滑块 —— 重置自然生效（画面也一起恢复：`reset()` 里照样调 `apply()` 把 100/100/100 推给播放器） |

---

### ✅ 第二十轮：曲线与三项读数一直没数据 —— 根因是 QML **同一个信号写了两个处理器**（静默顶掉）

用户反馈（原话）："不仅曲线没画出来，**缓冲速度**、**渲染帧率**、**缓冲时长**这些都没变化，我觉得是数据未送过来，不是没画出来。"

判断是对的：**数据根本没送到 QML**，跟画法无关（前两轮换 Canvas/Shape 都白改了）。

| # | 问题 | 根因与修复 |
|---|---|---|
| 1 | 曲线空白 + 曲线区三个"当前/峰值"永远空 | **根因（RC-AK）**：`PlayerInfoPanel.qml` 的根对象上出现了**两个** `onOpenedChanged`。QML 对"同一个对象的同一个信号处理器再赋值一次"**不报错**，后写的会**静默顶掉**前写的 —— 被顶掉的正好是那句 `player.startStatsSampling()`，于是 `m_statsSampling` 永远是 `false` → `refreshStats()` 里那句 `if (m_statsSampling) appendStatsSample(...)` 永远不执行 → `statsHistory` 恒为空 → 曲线没点可画、三个读数也是空。**代码看着完全正常，日志里一行错都没有。** 修复：两件事（重画 tick + 采样开关）合并进**同一个** `onOpenedChanged`；采样开关本身也从"外面的实例上挂处理器"搬进**声明 `opened` 的这个组件里**（实例上的处理器和组件里的处理器属于同一类冲突，跨文件时更没法查）。 |
| 2 | 同类隐患：以后可能再写出来 | 新写了 `build/find-dup-handlers.js`（**花括号栈**跟踪"当前对象"，按对象分组找同名处理器；先抹掉注释/字符串，避免花括号干扰）。自测用例能稳定报出重复，全工程 24 个 qml 文件现在是 **0 处**重复。 |
| 3 | 不想再靠"某一次 QML 调用"活着 | C++ 侧兜底：`m_statsSampling` **默认 `true`**（定时器本来就在 `Prepare` 之后一直跑，多存 60 个点没有任何可测量开销）。于是"面板打开 → 开始采样"这条路径**即使整条断掉，曲线照样有数据**；QML 那两次调用退化成"关面板时停掉定时器"的省电优化。另外换片源时清空历史（`notifyPrepared`），`stopStatsSampling()` **不再清历史**（重开面板立刻就有完整 30 秒趋势，不用等）。 |
| 4 | 出问题时必须一眼定位（用户看不到日志细节） | 三层自检：① 面板底部自检行在数据不通时显示 `［自检 sampling=? 点数=? player=? stats=?］`（分别对应"开关没开 / 历史没送到 / 播放器引用没拿到 / 连 stats 都到不了 QML"）；② QML `console.warn` → `[info] stats panel opened=… playerItem=ok`、`[info] stats history in QML: N points`；③ C++ `stats curves: N points (last: speed=… fps=… buffer=…)`（**前 3 个点每个都打**，之后每 10 个点）。三处对着看，"没采"还是"采了没送到 QML"一目了然。 |
| 5 | 顺手：文件对话框的句柄审计（用户要求详细检查） | `runOnWorkerThread()` 里 `CoInitializeEx` 之后有几条**提前 return** 的路（helper 造不出来 / show 失败），原来只在正常路径上配了 `CoUninitialize()` → 这条线程的 COM 公寓会漏。改成 RAII 守卫 `ComApartment`（进过函数就一定配对）；helper 改 `std::unique_ptr` 持有，析构 + 两次 `sendPostedEvents(DeferredDelete)`（`hide()`/析构过程中新排进来的也一起清），保证原生 `IFileDialog`/COM 句柄在这一轮就释放干净。 |
| 6 | 顺手：面板两行速度值一样容易被当成假数据 | 核心只有**一个**合计下载速度（`MediaPlayerUtil::mReadGotSize` 是所有流一起累加的，`CurrentDownLoadSpeed` 回调就这一个数，ABR 也只用它），没有按流分开的吞吐量。参考 DOM 那两行是网页播放器（每个分片单独统计）。保留行名与顺序，但值后面挂 `（合计）`，不让人误以为音频那行是单独测出来的。 |

验收点（本轮）：

| # | 怎么验 | 期望 |
|---|---|---|
| B1 | 右键 →「视频统计信息」，看曲线区 | 三条曲线**在动**；标题右侧 `当前 …（峰值 …）` 每 0.5 秒跟着变 |
| B2 | 同一个面板底部那行 | 正常时显示 `N 个采样点｜速度 …｜帧率 …｜缓冲 …`（不出现 `［自检 …］`）；若出现自检，按 `sampling/点数/player/stats` 四个数直接定位 |
| B3 | 日志 | `stats curves: 1 points`、`2 points`、`3 points` 之后每 10 个点一行；同时 `[info] stats history in QML: 1 points`（QML 侧收到了） |
| B4 | 面板关了再开 | 一打开就有最近 30 秒的完整曲线（历史不再被清空），日志出现 `stats curves: sampling stopped (history kept: N points)` |
| B5 | 文件对话框：选完 / 取消 / 连点两次 | 日志 `对话框对象已销毁（原生句柄已释放，worker 线程）`；重复点出现 `已经有一个原生对话框在显示，忽略这一次`；界面全程不"(未响应)" |

编译：`[100%] Built target appQtPlayer`（`build-round24.log`），无新增 warning。

---

### ✅ 第二十一轮：曲线压字 + 曲线改白色 + 高能进度条贴底（去缝 / 藏图钉）

用户反馈（原话）："曲线怎么和那几个文字重叠了，还有就是曲线颜色要说白色才行，再就是高能进度条在控制栏隐藏后吸附到底部的那个进度条之间不是有距离吗？直接不要距离了不要间隔了，在就是右边的开关高能进度读条常驻的[图标]在吸附底部后也不显示。"

| # | 问题/需求 | 根因与修复 |
|---|---|---|
| 1 | 曲线压到文字上 | **根因（RC-AL）**：`StatsChart` 的高度是**写死**的 `16 + 30 = 46`，而真实内容是"标题行高 + 2 + 曲线 30"。12px 字的 `implicitHeight` 在本机是 15~17px（跟字体/字重/DPI 有关），于是每条曲线都比自己的组件高 1~3px；`Column` 是按**声明高度**摆下一个子项的，多出来的那几像素正好压在下一条曲线的标题文字上。修复：高度改成 `titleHeight + titleGap + plotHeight`，其中 `titleHeight = Math.max(16, ceil(chartTitle.implicitHeight))`；曲线区的 `topMargin`/`height` 也改成读那两个常量（不再各写一份数字，避免以后又对不上）。 |
| 2 | 曲线颜色要白色 | 三条曲线原来分别是蓝 `rgb(0,161,214)` / 绿 `rgb(126,211,33)` / 黄 `rgb(224,192,96)`，全部改成 **`#ffffff`**（组件默认值也改成白色）。三条曲线各自一行、各有标题，同色不会认错。 |
| 3 | 高能进度条贴到底部时，与下面那条进度条之间的缝要去掉 | `pbpArea.y` 只在 **docked**（常驻 + 控制栏收起）那一支去掉 `pbpDockedGap(3px)`：`y = shadowProgress.y - height`，pbp 的底边直接压在 2px 影子进度条的顶边上，**零缝**。跟随控制栏那一支**保持原样**（仍是进度行顶边再往上 3px —— 用户之前明确要求"高能进度条在进度条上方要有一定距离"），所以进度行自己的 44 / 60 / 10 / 4 / 6 一个数都没动。 |
| 4 | 高能进度条吸附到底部后要**隐藏那颗图钉（pin）**（用户说明：这是功能修改，不是 bug） | `pbpPin.visible: !pbpArea.docked` —— 只在"常驻 + 控制栏收起（贴到底部）"时隐藏；控制栏一浮出来（`docked` 变假）立刻回来，**其它情况一律不变**。配套改 `pbpArea.pointerOnPin()`：docked 时直接返回 `false`，否则那块**看不见的**命中盒仍然会吃掉点击、把常驻关掉（表现就是"贴到底部后点一下曲线右边，常驻莫名没了"）；`pinHovered` 跟着变假，手型光标和提示气泡也一起收掉。 |

验收点（本轮）：

| # | 怎么验 | 期望 |
|---|---|---|
| C1 | 右键 →「视频统计信息」 | 三条曲线**不再压**任何文字；每条曲线的上沿与下一行标题之间有 2px 空隙 |
| C2 | 同上 | 三条曲线都是**白色** |
| C3 | 进度条最右边的 pin 点一下（常驻）→ 让控制栏自动收起 | 曲线贴到最底部，**与底下那条 2px 进度条零缝**；贴底期间**看不到 pin** |
| C4 | 贴底状态下移动鼠标让控制栏浮出 | 曲线回到进度条上方（3px 缝）、**pin 立刻重新出现**、可点（关掉常驻） |

编译：`[100%] Built target appQtPlayer`（`build-round25.log`），无新增 warning。

---

### ✅ 第二十二轮：曲线压字的真根因（坐标系）+ FLV 点播/直播 + 直播播放器 UI

**先说曲线**：上一轮我以为是"行高写死" —— **猜错了**。真根因是**坐标系用错**（RC-AM）：
`StatsChart` 里算折线顶点的 `polyline` 用的是**整个组件**的 `width/height`（整行 ≈54px），
而线段是画在 30px 高的 `plot` 里的 → 曲线按 54px 算却落在 30px 的框里，
底部整条溢出到下一行标题上、基线正好压在下一行文字底边（用户看到的"渲染帧率/缓冲时长/
60 个采样点 和曲线重叠、底部齐平"）。改成 `plot.width/plot.height` 之后彻底对齐
（上一轮那个"按字体量高度"只是把溢出量从 16px 变成 24px，越改越糟）。

| # | 需求 | 落地 |
|---|---|---|
| 1 | **FLV 点播**（现有播放器） | 核心本来就支持（ffmpeg `flv` demuxer 已编进 `libffmpeg.dll`，协议识别 `CicadaPlayerItem.cpp:1874` 已有 FLV）。缺的是入口 → 首页新按钮「FLV 视频」：C++ 原生文件框（第一项就是 `*.flv`）+ 本地/HTTP 两条路（`openFlvFile()/playLocalFile()`）。实测 mock 的 `/vod/sample.flv`：`format_name=flv`、1080p h264+aac、20.07s、**Range 返回 206**（seek 可用） |
| 2 | **FLV 直播**入口 | 首页新按钮「直播 FLV 地址」（地址弹窗 + 校验）→ 打开**直播窗口** |
| 3 | **直播播放器 UI**（另写一套） | 新文件：`LiveMain.qml`（窗口/QWindowKit/顶部热区标题栏）、`LivePlayerView.qml`（画面+弹幕层+房间信息+换流+自动重连+"回到直播"）、`controls/LiveControlBar.qml`（**没有进度条**，多了"直播中"状态/弹幕输入/清晰度）、`controls/LiveIconButton.qml`、`controls/LiveMenu.qml`。参考能给什么/给不了什么见 `docs/LIVE-PLAYER.md` 第 4 节（**直播控制栏在参考里是个空 div**，只能按实际观感设计） |
| 4 | 直播缓冲 | 新增 `liveMode` 属性（QML 在设 source **之前**置真）→ 4 秒上限/2 秒高水位/0.5 秒起播。**不设它跑的是框架默认 40 秒 = "延时 40 秒的直播"**。故意**不**打开 `RTMaxDelayTime`（那是给 HLS/DASH 写的，会进 `LiveCatchUp/LiveTimeSync`，而 FLV 直播没有 `mUtcTimer`，没实测不开） |
| 5 | 本地 **mock 直播服务**（ffmpeg 循环 + 多清晰度 + **GPU 硬编**） | `front/player/mock-live/server.js`（零依赖 Node ESM）。全 GPU 管线：NVDEC 解码 + `scale_cuda` 缩放 + `h264_nvenc` 编码；`-re -stream_loop -1` 无限循环；1 秒关键帧；HTTP-FLV 扇出（**preamble + 最近 GOP 补发**，晚来的客户端才能解码）。实测：4 档同时推、`restarts=0`、Node **55MB**、GPU 编码器 **15%**、晚接入客户端解码 3 秒内容 **2.85s 零警告** |
| 6 | 分析：node/python/go 够不够 | **Node 够用**（服务只做"切 tag + 扇出"，转码在显卡上；实测整机 CPU<5%）。瓶颈是 **NVENC 会话数**不是语言。详见 `docs/LIVE-PLAYER.md` 第 2 节 |
| 7 | 分析：要不要做 HLS 直播 | **这轮不做**：用户目标（FLV 点播+直播、测切换）已达成；核心的直播追赶/UTC 同步是 HLS/DASH 专属且从没被 FLV 跑过，打开它风险大于收益。详见同文档第 5 节 |

新增文档：`docs/LIVE-PLAYER.md`（实现说明 + 两项分析 + 服务端自检命令 + L1~L7 验收点）。

编译：`[100%] Built target appQtPlayer`（`build-round26.log`），无新增 warning；
5 个新 QML 均已进 qrc（日志里 `Generating .rcc/qmlcache/appQtPlayer_Live*.cpp`）。

---

### ✅ 第二十三轮：直播"填了地址却一直连接中"—— 内层播放器**从没拿到地址**（RC-AN）

用户实测（日志 `appQtPlayer.log`，22:31~22:38）：直播窗口打开了、界面一直"连接中"，
**点了切换清晰度反而能播**。日志里证据链非常干净：

```
22:31:01  liveMode = true
22:31:01  [live] 清晰度清单：4 档（原画 / 720P / 480P / 360P）     ← 地址确实传到了本组件
（此后 7 分钟里没有任何 "opening source:"）                        ← 但播放器从没收到地址
22:38:05  [live] 切换清晰度 → 720p
22:38:05  opening source: http://127.0.0.1:9200/live/720p.flv      ← 只有换档那条路能通
22:38:07  first frame handed over to the Qt item（约 1.8 秒出画）
```

**根因（RC-AN）**：`LivePlayerView.qml` 对外只暴露 `source` 属性（LiveMain 写它），
但里层的 `CicadaPlayerItem` **没有把 `source` 绑上去** —— `onSourceChanged` 只去拉了清晰度清单，
地址就停在本组件里，从没交给播放器。而 `switchQuality()` 是**直接给 `player.source` 赋值**的，
所以"换清晰度"那条路绕过了这个缺失，能播。

**修复**：

1. `CicadaPlayerItem { source: liveView.source }` —— 用**绑定**接上（这是唯一正确写法）；
2. 因为"给已绑定的属性赋值会打断绑定"，**重连**（`reconnectNow`）和**换档**（`switchQuality`）
   里那两处 `player.source = …` 全部改成 `liveView.source = …`，让绑定把地址传下去
   （不改的话：重连/换档一次之后，绑定就死了，以后再填地址又传不进去 —— 同样的坑换个地方再踩一遍）。

**顺带修掉一个 QML ReferenceError**（同一条日志第 8 行）：
`LiveMain.qml:146 ReferenceError: buttonsRow is not defined` —— 那是上一轮我把三颗窗口按钮
从 `Repeater` 改成有 id 的实例时漏掉的引用（标题的右边界还锚在那个已删掉的 id 上，
后果是标题会一直顶到窗口右边、压在窗口按钮下面）。现在锚到 `minimizeButton.left`。

**这次日志另外证实了两件事**（都是前几轮的成果，可以安心了）：

* 统计曲线的数据链路是通的：`stats curves: 60 points (last: speed=810280 B/s fps=25.0 buffer=0.0s)`
  与 `[info] stats history in QML: 60 points` 成对出现；
* 直播延迟正常：`opening source` → `first frame handed over to the Qt item` 相隔 **1.8 秒**
  （0.5 秒起播 + 1 秒关键帧 + 网络，符合"直播 1~2 秒"的预期）。

编译：`[100%] Built target appQtPlayer`（`build-round28.log`），无 error/warning。

---

### ✅ 第二十四轮：HLS 直播 + 按**真实直播页**校 UI 数值

用户要求："再添加 hls 协议的直播支持，并在 mock 中加上对应的参数启用或者不启用 hls 直播，
直播播放器 ui 不要自己乱写参考，[按] live.bilibili.com/8178490 这个链接的 ui 实现。"

**HLS 直播（FLV + HLS 两条都支持）**

| 侧 | 做了什么 |
|---|---|
| mock（`front/player/mock-live/server.js`） | 新参数 `--no-hls` / `--hls-time <sec>`（默认 1）/ `--hls-list <n>`（默认 6）；**HLS 默认开**。地址 `…/live/hls/master.m3u8`（多码率）与 `…/live/hls/<档>/index.m3u8`（单档）。master 由 Node 自己写（BANDWIDTH/RESOLUTION/NAME 准确），媒体清单由 ffmpeg 出（滚动窗口、无 ENDLIST）。 |
| 坑 1（★关键） | **同一个 ffmpeg 出两路时，HLS 那路必须写 `-c copy`**：编码器参数是**按输出**生效的，第二个输出不写就退回 **libx264**，而帧在显存里（`-hwaccel_output_format cuda`）→ `Impossible to convert between the formats supported by the filter…` → 该档每秒崩一次、无限重启（实测到）。`-c copy` 后不额外占 NVENC 会话、不二次编码、分片边界天然对齐 → 4 档全程 `restarts=0`。 |
| 坑 2 | `const base = …` 声明顺序（TDZ）：`writeHlsMaster(renditions, base)` 写在 `const base` 之前 → 启动即 `ReferenceError: Cannot access 'base' before initialization`。 |
| 坑 3 | 我把 `res.writeHead` 写成了 `resp.writeHead` → 一访问 HLS 路由 Node 就崩。 |
| 播放器 | `LivePlayerView.qml` 按后缀分流：`.m3u8` → 档位取自 `player.qualities`（master 解析来的）、切档走 `player.selectQuality(streamIndex)`（**清单内换流、不重连不黑屏**）；`.flv` → 档位取自 streams.json、切档换流重连。控制栏信号改成回传整项（HLS 需要 streamIndex、FLV 需要 url）。首页那颗按钮从「直播 FLV 地址」改成「**直播地址**」（FLV/HLS 都能填）。 |
| 实测 | master 3 档、720p 清单无 ENDLIST（滑动窗口 6 片）、MIME `application/vnd.apple.mpegurl` + 禁缓存、ffmpeg 直接解码 3 秒 0.53s **零警告**、命令行播放器播 master.m3u8 16 秒不崩。 |

**直播 UI：把"猜的"换成"查到的"**（详见 `docs/LIVE-PLAYER.md` 5b 节）

查到了就照抄：菜单面板 `rgba(28,28,28,.9)/圆角 3/padding 3`、**条目 200×28**（`padding:3px 20px; line-height:22px`）、
hover `hsla(0,0%,100%,.1)`；控制条底色 `linear-gradient(to bottom, transparent 20%, rgba(0,0,0,.9))`；
全屏底栏 48px；弹幕输入框 placeholder 原文 **`发个弹幕呗~`**、**maxlength 30**、`border-bottom:2px solid #fff8`、高 24；
发送按钮=设计系统 primary（实心品牌蓝）；回到直播 **22px/圆角 12/`rgba(51,51,51,.8)`/6px `#ff5243` 红点**；
顶部头像 32×32、房间名 max-width 150 量级；品牌粉从旧的 `#FB7299` 换成当前主题的 **`--Pi5:#FF6699`**、
直播 chrome 用 **`#23ade5`**、菜单当前档/进度类用 **`#00a1d6`**。

查不到的（**写明是近似，不装成事实**）：直播控制条总高度/按钮尺寸/图标尺寸、菜单"当前档"标记、
弹幕层的倾斜角与字号描边、关注按钮几何、以及人气/关注数（**不编假数字**，那一行放真实分辨率/码率/速度/帧率）。
原因：直播控制条是运行时注入的，样式在 `room-player.min.js`（CSS-in-JS）里，`web_fetch` 拒收 JS，
Aliyun OSS 禁止匿名改 content-type，Common Crawl 对该域名只收录了 robots.txt。
——要 100% 一致，得从 DevTools 里把那段 HTML+生效 CSS 复制出来。

编译：`[100%] Built target appQtPlayer`（`build-round30.log` / `build-round31.log`），无 error/warning。

---

### ✅ 第二十四轮（续）：用户给了官方 bundle —— 控制条改成**真值**

用户把官方 `room-player.min.js`（748 KB）下载下来发我了 —— 直播控制条的样式就在里面的 CSS-in-JS 里，
**之前"公网抓不到"的那些数，现在全能读到**。写了个抽取器 `build/extract-live-css.js`
（找 `选择器{...}`、按关键词归类、抓上下文），报告 `build/live-css-report.txt`
（4974 条规则；控制条 15、清晰度菜单 64、弹幕层 63）。

**换掉"猜的"、改成真值的部分**：

| 元素 | 之前（近似） | 现在（官方真值） |
|---|---|---|
| 控制条底衬 | `linear-gradient(transparent 20%, rgba(0,0,0,.9))` 铺满 | `.web-player-controller-bg`：**56px 高**、`linear-gradient(180deg, rgba(0,0,0,0), rgba(0,0,0,.7))` |
| 控制条内容行 | 高 46/48、垂直居中、spacing 10 | `.control-area`：**bottom:3px; height:36px; padding:0 12px; font-size:12px** |
| 按钮 | 36×22 | `.icon`：**36×36**，`svg fill:rgba(255,255,255,.9)` → hover `#fff` |
| 文字项（清晰度） | 自定 padding | `.text`：`line-height:36px; padding:0 6px; color:rgba(255,255,255,.9)` → hover `#fff` |
| 控制条布局 | 单行左对齐 | `.left-area` + `.right-area`（**`flex:1; row-reverse` = 右组贴右边**） |
| 浮层壳 | `rgba(28,28,28,.9)`、圆角 3 | `.panel`：**`rgba(33,33,33,.9)`、圆角 4、opacity .9、z-index 100、居中对齐按钮** |
| 清晰度面板 | 条目 200×28（那是**右键菜单**的值，用错了） | `.quality-wrap .panel{padding:9px 0; min-height:94px}` + `.quality-it{line-height:22px; padding:0 20px}` |
| 当前档标记 | `#00a1d6` + 小圆点 | **`.quality-it.selected{color:#23ade5}`**（就是文字变色，没有圆点） |
| 音量默认 | 100% | 官方 video `volume=.9`、设置默认 `{value:90}` → 起播 0.9 |
| 直播状态红点 | `#FA5A57` | `--Re5` 的 **`#F85A54`** |

**还没做（真值已记录在 `docs/LIVE-PLAYER.md` 5b.1，随时可补）**：清晰度面板上方的**线路选择器**
（`.line-wrap` 66px 宽那组数值；本项目只有一条线路，没有数据就不摆假控件）、
**弹幕设置面板**（`298px` 宽、`padding:20px`、贴右 12px，四行：不透明度/字号缩放/同屏密度/显示区域；
其中前两项与"显示区域"能直连 `DanmakuView`，同屏密度缺对应 API）、以及图标形状本身。

**仍属"近似"的**：直播间页面自己的那几块（顶部房间信息、关注按钮、回到直播、弹幕输入框）——
它们不在播放器 bundle 里；要真值需要房间页自己的 CSS（或 DevTools 里那一段）。

编译：`[100%] Built target appQtPlayer`（`build-round32.log`），无 error/warning。

---

### ✅ 第二十五轮：直播窗口"点播放没反应" —— QML 的 color 不认 CSS 函数写法（RC-AO）

用户日志只有一行，但足够定位：

```
[home] 直播窗口创建失败：qrc:/qt/qml/QtPlayer/LiveMain.qml:65:5: Type LivePlayerView unavailable
qrc:/qt/qml/QtPlayer/LivePlayerView.qml:425:5: Type LiveControlBar unavailable
qrc:/qt/qml/QtPlayer/controls/LiveControlBar.qml:101:41: Invalid property assignment: color expected
```

**根因（RC-AO）**：上一轮我为了"贴官方 CSS 原文"，把两行写成了

```qml
readonly property color barText:    "rgba(255,255,255,0.9)"   // ✗ QML 的 color 不认 CSS 函数写法
readonly property color barTextDim: "rgba(255,255,255,0.6)"   // ✗
```

QML 的 `color` 只接受 `#RGB` / `#RRGGBB` / `#AARRGGBB` 和内置颜色名，**不接受 `rgba(...)`/`hsla(...)`**。
更麻烦的是这是**运行期**错误：qmlcachegen 不做属性类型检查，**编译一路绿灯**；
而这一行让 `LiveControlBar` 整个组件加载失败 → `LivePlayerView` 不可用 → `LiveMain` 建不出来 →
用户在首页填了直播地址点播放**什么都不会发生**（连窗口都建不出来）。

**修复**：`Qt.rgba(1, 1, 1, 0.9)` / `Qt.rgba(1, 1, 1, 0.6)`（等价于那两句 CSS）。

**顺带补一道关卡**：`build/check-qml-shapes.js` —— 专门盯"编译能过、运行才炸"的写法：
① `color:` 被赋 CSS 函数串；② 十六进制颜色位数不对（`#fff8` 这种 CSS 写法）；
③ `Qt.rgba()/Qt.hsla()` 参数不足 4 个；④ `color:` 被赋数字。会正确处理 `//` 与 `/* */` 注释
（自测：故意塞 3 个坏写法全抓到；注释里的示例不误报）。
加上之前那个 `build/find-dup-handlers.js`（重复信号处理器），两个检查器现在全工程都是干净的：
**29 个 qml 文件，0 处问题**。

编译：`[100%] Built target appQtPlayer`（`build-round33.log`），无 error/warning。

---

### ✅ 第二十六轮：主题色统一成品牌粉（只改两个变量）+ 修好 HLS 直播的清晰度菜单

**1. 主题色统一（用户要求：所有蓝色都改成主题变量，深 `#D44E7D` / 浅 `#FF6699`）**

先写了个**按色相**扫描的脚本 `build/find-blue-colors.js`（不靠人肉记忆，把
`#RRGGBB`/`#AARRGGBB`/`Qt.rgba(...)` 三种写法都算进去，色相 170°~275° 即"偏蓝"），
扫出全项目 **42 处、15 种**蓝。然后：

* 在 `QtPlayerTheme.qml` 顶部定义**唯一来源**：
  `accentDark ""#D44E7D"` / `accentLight "#FF6699"` → `playerAccent`，
  外加 `playerAccentHover`（亮一档，取代 `#00aeec`）、`playerAccentActive`（按下档）、
  `playerAccentFaint`（18% 底，取代 `#cfe0f5`）、`playerAccentSoft`（主色 20%，高能进度条已播层）。
* 主题里 7 个键改指向它：`progressPlayedBg`（进度条已播）、`qualityActiveText`（清晰度选中）、
  `volumeThumbBg`（音量滑块）、`pbpCurvePlayedBg`（高能进度条）、`accent`、`buttonPressedBg`、`choiceActiveBg`。
* 其它文件 **43 处**字面量替换成 `QtPlayerTheme.playerAccent` / `playerAccentHover`：
  弹幕面板 14、设置面板 9、色彩面板 5、字幕面板 4、直播控制条 4、清晰度菜单 4、弹幕设置 2、首页边框 1。
* **明确不改的两处**（并在代码里加了注释）：`DanmakuBar` 的**弹幕可选颜色**调色板、
  `SubtitlePanel` 的**字幕颜色**选项 —— 那是数据不是主题色；另外首页那张深灰卡片底也保留。
* 文档：`docs/THEME-COLORS.md`（定义在哪、已改哪些、**不许改哪些**、怎么换色、自查命令）。

**2. HLS 直播的清晰度菜单没有选项（用户实测）**

根因两句话：直播界面的控制条实例**绑的是 FLV 专用的 `renditions`**（HLS 下恒为空），
而 HLS 的档位本来该从 `player.qualities`（核心解析 master 后上报）来 —— 这条**不保证有**。
修法：两条路都接上，菜单永远不空

* ① **首选**：`player.qualities`（核心上报）→ 切档 `player.selectQuality(streamIndex)` = 清单内换流；
* ② **兜底**：QML 自己拉 `master.m3u8` 解析 `#EXT-X-STREAM-INF`（BANDWIDTH/RESOLUTION/NAME + 下一行 URI
  拼成绝对地址）→ 切档走"换一条 variant 重连"（复用 FLV 那条逻辑）。
* 控制条实例改成绑 `currentQualityList`（内部按协议二选一）。

**3. 这轮的验证手段（都是能自己跑的，不靠 GUI）**

* `build/test-hls-parser.js` —— 把 QML 里的 `parseHlsAttributes`/`parseHlsMaster`
  **按花括号配对原样抠出来**在 Node 里跑，喂真实 `master.m3u8`：
  实测解析出 **4 档**（原画 1920x1080 5128000bps / 720P / 480P / 360P），
  绝对地址、分辨率、带宽、显示名全部正确 ✓；
* 核心侧：`cicadaPlayer.exe` 播 `http://127.0.0.1:9200/live/hls/master.m3u8` 连续 14 秒不崩、
  HLS 分片序号持续推进（MEDIA-SEQUENCE:77）→ HLS 直播这条链路在核心侧是通的 ✓；
* `find-blue-colors.js` 复查：只剩第 3 节那两处"数据调色板"；
  `check-qml-shapes.js` / `find-dup-handlers.js`：29 个 qml 文件全干净 ✓。

编译：`[100%] Built target appQtPlayer`（`build-round34.log`），无 error/warning。

---

### ✅ 第二十七轮：直播缓冲只对连续流收薄 + 单档 HLS 地址自动升级 + HLS 默认关闭

**1. RC-AP：我把"直播薄缓冲"错套到 HLS 上了（用户实测：HLS 直播加载不出来）**

上一版给直播加的"4 秒上限 / 2 秒高水位 / 0.5 秒起播"是**按 FLV（连续流）设计的**，却对
HLS/DASH（分片流）也生效了。而 **HLS 的分片长度由源视频关键帧间隔决定**（`-c copy` 只能在
关键帧处切）：用户的素材 5 秒一个关键帧 → 分片 5 秒一片 ⇒ **4 秒的缓冲上限比一个分片还短**，
分片流永远攒不出可播的一段数据，核心里就是刷屏的 `HLSStream: open_internal again`（每 77ms 一次）、
一片都放不出来。

修：`createPlayer()` 里条件收紧成 `mLiveMode && !segmentStream` ——
**薄缓冲只给连续流（HTTP-FLV/RTMP）**；HLS/DASH 直播保持原来那套 15 秒分片策略并单独打一行日志：
`live segment stream (HLS/DASH): keeping the 15s segment buffer (a thinner buffer than one segment would starve playback)`。
（同时确认：**master.m3u8 是能播的**，用户实测。）

**2. 单档 HLS 地址（`…/1080p/index.m3u8`）"无法解析" —— 自动升级到同目录 master**

原来的清晰度菜单只认**多码率 master**（`#EXT-X-STREAM-INF`）；单档媒体清单里没有这些标签，
解析出 0 档 → 菜单显示"没有清晰度清单"，而裸单档直播清单核心也不一定能起播。
修：填单档地址时自动探测同目录的 **`master.m3u8`**（先父目录 `…/live/hls/master.m3u8`，
再同目录），**确认含 `#EXT-X-STREAM-INF` 才升级**过去（防抖：只在地址没被改过时升），
于是两种地址随便填。找不到 master 时：菜单显示"当前这一档"（名字从 URL 目录或真实分辨率来），
并且**切档只走 `selectQuality`**、绝不加载 variant 地址（避免把正在播的流换坏）。
`build/` 里那个 `test-hls-parser.js` 可跑；`siblingMasterCandidates` 也做了单测
（`1080p/index.m3u8 → …/hls/master.m3u8`；`master.m3u8 → []` 不递归）。

**3. HLS 直播在 mock 里改成默认关闭（用户要求："生产一般只推 FLV"）**

`opts.hls` 默认 **false**；`--hls` 打开、`--no-hls` 显式关、`--hls-encode` 顺带打开 HLS。
关闭时 `/live/hls/**` 404、`streams.json` 的 `hls` 段为 null、ffmpeg 不挂 HLS 输出 —— 
**默认行为与生产一致**。

**4. "HLS 直播没有 FLV 流畅"的原因与开关**

实测：用用户视频时 HLS 分片是 **5 秒**（`TARGETDURATION:5`），用我 2 秒关键帧的测试源时正好 **2 秒** ——
**分片时长 == 源关键帧间隔** ⇒ HLS 那路是 `-c copy`、**直接取输入包、绕过编码器**，
所以 FLV 的"1 秒关键帧 + CBR + tune ll"完全没作用到 HLS 上；再叠加"整片下载完才能播、
清单每 TARGETDURATION/2 才刷新、每片交界重开 demuxer"，抖动就被放大成停顿。
新增 `--hls-encode`（HLS 也按同一套低延迟参数重编 → 真的 1 秒一片；代价每档多 1 个 NVENC 会话，
3 档=6 安全 / 4 档=8 顶上限）。**默认关闭**，默认行为不变。
HLS 的**延迟**始终高于 FLV（分片协议天性，改不了）。

编译：`[100%] Built target appQtPlayer`（`build-round35.log` / `build-round36.log`），无 error/warning。

---

### ✅ 第二十九轮：色彩调整面板删绿字 + 统计/色彩/快捷键面板点击不再穿透到画面区

**1. 删掉色彩调整面板底部那行绿字（用户明确要求）**

`PlayerColorPanel.qml` 底部那个 `Text` 是二选一的（后端支持=绿、不支持=琥珀）。现在：

* **绿字那支整支删掉** —— 「色彩调整已即时生效（在 D3D11 视频处理器上完成，不额外拷贝、不退出零拷贝）。」不再出现；
* 琥珀色那支**保留**：`visible: colorPanel.touched && !colorPanel.supported`。
  理由：macOS Metal / Linux VAAPI / CPU 回退上拖滑块真的不动画面，这句话必须如实说，
  删掉就成了骗用户；本机（Windows/D3D11）`supported` 为真 → 整块 Text 不显示，
  `Column` 会跳过 invisible 子项，面板底部是干净的、不留空档。

**2. RC-AQ：点统计信息面板/色彩调整面板，视频却被暂停了**

根因：QML 里**只有 MouseArea / TapHandler 这类"接收者"才消费鼠标事件**，
`Rectangle`/`Column`/`Text` 一律不消费。三个面板的根都是 `Rectangle`，于是点在面板**空白处**时，
事件继续往下找，落到铺满整个播放区的 `activityArea`（`PlayerView.qml:644`）→ 它的 `onClicked`
起 400ms 定时器 → `player.togglePause()`；右键还会弹出播放器右键菜单。

修法：三个面板（`PlayerInfoPanel` / `PlayerColorPanel` / `PlayerHotkeyPanel`）根部各加一层
**什么都不画**的 MouseArea（`id: panelBackdrop`）：

| 设置 | 为什么 |
|---|---|
| **必须是第一个子元素**（z 序最低） | 上面的滑块 / ✕ / 重置按钮自己接走事件，只有空白处的点击落到它身上 |
| `acceptedButtons: Qt.AllButtons` | 右键也要吃掉，否则在面板上右键会弹播放器右键菜单 |
| `hoverEnabled: false` | **故意不抢 hover**：`activityArea` 的鼠标移动（显示控制栏 + 重置自动隐藏定时器）和光标形状照常工作，鼠标划过面板不会把控制栏"冻住" |
| `cursorShape: Qt.ArrowCursor` | 面板上是箭头，不跟着画面区变成小手/空白光标 |

**3. 把「QML 静态检查」正式补进流程**（承接第 28 轮 `timerType` 那次事故 RC-AP）

改完 QML 跑一遍 `qmllint`（Qt 自带，29 个 QtPlayer QML 秒级）：**只看 `Error:` 行，必须 0 条** ——
`timerType` 那种会报成 `Member "timerType" not found on type "Timer" [missing-property]`。
`QtPlayerTheme` 单例属性、弹幕池动态 `createComponent` 出来的对象、`QKeySequence.MediaPlay`
这几类是误报（判读方法见 `DANMAKU-ANALYSIS.md` §7.4）。

验证：`qmllint` 三个文件 0 error；`build-round40.log` → `[100%] Built target appQtPlayer`，无 error。

**4. 同一轮的用户需求（安卓侧）**

同时要求把 Qt 播放器的功能与 UI 移植到 `platform/JetpackComposePlayer`（Android + Jetpack Compose）：
UI 按两张参考截图（非全屏/全屏）、主题色深色 `#D44E7D` / 浅色 `#FF6699`、播放暂停用 Lottie、
竖屏/全屏用用户给的 SVG。进度、设计取舍与"还没做什么"见
**`platform/JetpackComposePlayer/docs/PLAYER-PORT.md`**。

---

## 0. 目标与验收标准





### 0.1 一句话目标
让「点清晰度」在 DASH 与 HLS 上都能在 **≤1 秒**内完成切换，且**任何失败路径都有终态**（不留下关不掉的旧流、不留下永久的 0 FPS）。

### 0.2 客观验收标准（全部以日志为准，不看主观感受）

| # | 判据（日志断言） | 现状（DASH / HLS） |
|---|---|---|
| A1 | 切换后 3 秒内 `video fps is` 回到 ≥55 | DASH 0 / HLS 0 |
| A2 | 一次切换里 `skip video forward refused` **出现 0 次** | DASH 5 次 / HLS 9 次 |
| A3 | 一次切换里 `drop stale video packet after quality switch` **出现 0 次** | DASH ≈370 / HLS ≈500 |
| A4 | 一次切换里 `unknown stream` **出现 0 次** | DASH 0 / HLS 2000+ |
| A5 | `committed seamless video decoder switch` 里 `offset` **不是** `-9223372036854775808`；且 `\|pts − master\| ≤ 1 帧` | 两边都违反（0.25s 前 / 1.57s 后） |
| A6 | 切换必有一条终态：`quality switch status=1（READY）` 或 `=2/3（FAILED/CANCELED）`；**不允许只有 `=0`** | HLS 只有 =0 |
| A7 | 切换结束后目标流落点 ≈ 切换点：`HLSStream: mark startTime` / `DashStream: getCurSegNum` 的 pts 与 master 差 ≤1 个分段 | HLS 落 70.9s（点 34.9s）/ DASH 跑到 70s |
| A8 | 切换后 `pendingVideo=-1 willChangeVideo=-1 retiredVideo=-1`（状态机已归零） | HLS `retiredVideo=-1` 但旧流仍在产出 |
| A9 | 切换结束后 5 秒内不再有 `drop late` / `refused` 之外的 FPS 波动；`mQualitySwitchCommitPending` 不再为真（可用一行日志判） | 未知 |

### 0.3 不能破坏的既有行为（回归红线）
1. **普通 seek**（`PFR: seek`）、暂停/恢复、本地文件播放、`format` 为 0 的老路径全部照旧。
2. **同一 codec 只换分辨率**（HLS 那份的 `codec 1 -> 1`）不能再触发「重建解码器但 decoder 不匹配」的老坑。
3. **跨 codec 切换**（H.265 480p → H.264 2160p，DASH 那份的 `codec 6 -> 1`）必须仍然重建解码器。
4. `AUDIO`、`SUBTITLE`、`DRM`、`MIXED` 路径不动。
5. 切换期间**音频与主时钟绝对不动**（现有承诺，继续保持）。

---

## 1. 阶段总览与依赖

```
阶段 0（埋点，1 小时）─── 必须最先做，否则后面无法判定
   │
   ├─> 阶段 1（P0：解死锁）      ← 收益最大，3 项可独立提交
   │     1A 切换收尾解耦（RC-3b）
   │     1B 「太早」判为游标错位 + 追帧计数拆分（RC-4/RC-5）
   │     1C 目标流定位确定化（RC-4b）
   │     1D 退役流包原子清除 + drop 计入 flow（RC-3）
   │
   ├─> 阶段 2（P1：统一时间轴）  ← 依赖 1B/1C 之后才好验证
   │     2A DASH 段起点改用真实 PTS
   │     2B 切换状态机统一到一根轴 + offset 无条件建立
   │     2C 提交门限收紧到 1 帧
   │     2D mPtsDiscontinueDelta 来源核查
   │
   └─> 阶段 3（P2：设备/锁/日志/UI）
         3A pending 与 active 共用 hw device
         3B SMPAVDeviceManager 锁粒度
         3C 日志限频
         3D UI 切换看门狗 + 高亮回退 + URL scheme 白名单
```

**提交粒度**：阶段 0、1A、1B、1C、1D、2A…每个都是一次可独立编译 + 独立验证的提交，任何一条出问题都能单独 revert，不影响其它。

---

## 2. 阶段 0：先让日志说人话（不改行为）

> 现状：三类 `AF_LOGD` 逐帧/逐包打印，一份日志里 ~4000 行是噪声；同时**日志文件被两个写入者交错覆盖**（整段重复 + 错位拼接），关键行还会被吃掉。不先解决这个，后面每一项都没法判定。

| 编号 | 改动 | 位置 | 判据 |
|---|---|---|---|
| W0.1 | 日志出口收成一个（只留一个 `fopen`/`freopen`，或内部 logger 与 shell 重定向二选一） | `platform/QtPlayer/main.cpp` 的日志初始化 + `framework/utils/frame_work_log.h` 实现 | 新日志里不再出现「一整段重复、且被截断拼接」的行 |
| W0.2 | 新增 `[switch]` **每秒一行**汇总：`state=warming/committed/ready/failed target=%d master=%lld activeFront=%lld targetKeyPts=%lld activeFrameQ=%zu targetFrameQ=%zu cmdPending=%d` | `SuperMediaPlayer::checkVideoPath()` 开头（或 `OnTimer`） | 复现时一行就能看出「目标流在哪、队列头在哪、状态机停在哪」 |
| W0.3 | `FlushVideoPath()` 打一行带**调用者与原因**：`AF_LOGW("FlushVideoPath from %s (flushRender=%d cancelPending=%d) seekFlag=%d commitPending=%d retired=%d")`，调用点传 `__func__`/枚举 | `SuperMediaPlayer.cpp:4393` + 6 个调用点（`:428/:1760/:1864/:1922/:2241/:2727` 与 `SMPMessageControllerListener.cpp:596/:658`） | 钉死「提交后 0.1 秒内是谁 flush 了视频路」这个悬案（HLS 报告 §7.1） |
| W0.4 | 三类洪水改**每秒汇总**：`drop stale video packet` / `drop pending frame before switch window` / `hold pending video before playback position` / `unknown stream` | `SuperMovePlayer.cpp:2503/:2937/:3120/:4296` | 日志从 ~7000 行降到 ~300 行 |
| W0.5 | `HLSStream::seek` / `DashStream::seek` 打印「请求 us → 选定 segment index → segment startTime/pts」 | `framework/demuxer/play_list/HLSStream.cpp`、`dash/DashStream.cpp` | 直接判定 W1.3 是否生效 |

**回滚**：纯日志，无风险。W0.1 若涉及构建脚本/部署脚本，单独一个提交。

---

## 3. 阶段 1（P0）：解掉三类死锁

### W1.1 ★切换收尾解耦：让状态机一定有终态★（对应 RC-3b，最高优先）

**问题**：`RenderVideo()` 的 READY 分支（`SuperMediaPlayer.cpp:3571-3602`）同时承担三件事：发 READY、`CloseStream(retired)`、`releaseRetiredVideoDecoder()`；而它的门槛是「**必须有一帧真的上屏**」。HLS 那份日志里一帧都没上屏 → 三件事全没做 → 旧流读到 84s、`unknown stream` 刷 2000+ 行、旧解码器不退、界面永远「切换中」。

**改法**：
1. 抽出单一出口函数：
   ```cpp
   // SuperMediaPlayer.h
   void finishQualitySwitch(bool ready, const char *reason);
   ```
   里面按顺序做：`NotifyVideoQualitySwitch(ready ? READY : FAILED, mQualitySwitchCommittedStreamIndex, reason)`
   → `CloseStream(mRetiredVideoStreamIndex)` → `releaseRetiredVideoDecoder()` →
   清 `mQualitySwitchCommitPending / mQualitySwitchCommittedStreamIndex / mQualitySwitchOldFramesPending / mRetiredVideoStreamIndex / mActiveVideoPtsOffset`。
2. `RenderVideo()` 里原来那段改成 `finishQualitySwitch(true, "quality switch rendered")`（保留 `NotifyStreamChanged` 与 `NotifyVideoSizeChanged`）。
3. **新增超时兜底**：`TryCommitPendingVideoSwitch()` 成功时记 `mQualitySwitchDeadlineMs = af_getsteady_ms() + kQualitySwitchDeadlineMs(500)`；
   在 `mainService()`（或 `checkVideoPath()` 里、早于那三个 early-return）每秒检查一次：
   ```
   if (mQualitySwitchCommitPending && now > mQualitySwitchDeadlineMs) finishQualitySwitch(false, "quality switch not rendered in time");
   ```
   注意 **必须放在 `checkVideoPath()` 的 early-return 之前**，否则 `mQualitySwitchCommitPending` 本身就把看门狗挡住了（这正是 HLS 那份日志里最诡异的点）。
4. `FlushVideoPath()` 里那句 `mQualitySwitchCommitPending = false`（`:4452`）**不能直接删**（seek/stop 需要它），但要改成走 `finishQualitySwitch(false, "flush canceled the switch")`，保证 `CloseStream(retired)` 一定被执行、`retired` 一定不丢。

**风险**：过早关旧流 → 黑帧/花屏。缓解：超时阈值取 500ms（实测 DASH 的 READY 用了 8ms，HLS 那种失败路径本来就没有可用帧）；且只在 `mQualitySwitchCommitPending` 为真时生效。
**判据**：A6（必有终态）+ A8；HLS 复现时 `HLSManager: CloseStream 3` 必须出现一次，`unknown stream` 归零。

---

### W1.2 ★把「太早」判为游标错位，并把两种追帧计数拆开★（对应 RC-4/RC-5）

**问题**：`RenderVideo():3499` 对「比时钟早 10ms 以上」的帧只 `return false`，不弹不丢；而 `skipVideoForwardToMasterClock():2034` 的 `MAX_VIDEO_SKIP_WITHOUT_RENDER=2` 会在「跳了两次都没上屏」后**永久拒绝再跳**。于是「队列超前 → 不上屏 → 拒绝跳 → 永远不上屏」闭环，两份日志都是这样死的。

**改法**：
1. `RenderVideo()` 里在原来 3499 行的 early-return **之前**加一档：
   ```cpp
   const int64_t kEarlyRealignUs = 500 * 1000;   // 1 个量级：远早于"单帧提前"
   if (videoLateUs < -kEarlyRealignUs && !qualitySwitchWarming) {
       if (realignVideoToMasterClock()) { mVideoCatchingUp = true; return false; }
       // 对齐失败也不原地等：走"丢掉这个过分的未来帧"的出口，避免队列被 2 帧上限堵死
       videoFrame->setDiscard(true); ... 走 drop 分支 ...
   }
   ```
2. 新增 `bool SuperMediaPlayer::realignVideoToMasterClock()`：
   - 取 `master = mMasterClock.GetTime()`；
   - `nextKeyPts = mBufferController->GetFirstKeyPTSAfter(BUFFER_TYPE_VIDEO, master)`；
   - **若 `nextKeyPts` 本身也超前太多（> 1 个分段）**，说明「包队列整体错位」，此时正确的动作不是丢包，而是**把该 representation 的 demuxer 拉回 master**：
     `mDemuxerService->Seek(master, 0, mCurrentVideoIndex)` + `FlushVideoPath(false,false)` + `mBufferController->ClearPacket(BUFFER_TYPE_VIDEO)`；
   - 否则退回现有 `ClearPacketBeforePTS(nextKeyPts) + FlushVideoPath(false,false)` 语义；
   - **不计入** `mVideoSkipWithoutRenderCount`。
3. 拆计数：`mVideoLateSkipCount`（落后，用于现有的 `MAX_VIDEO_SKIP_WITHOUT_RENDER` 保护）与 `mVideoAheadRealignCount`（超前，只用于限频日志，不抑制重试）。`RenderVideo()` 的 `mVideoSkipWithoutRenderCount = 0` 清零点照旧（上屏即清零）。

**风险**：`Seek` 在当前视频路上是重活（占读线程）——所以要限制频率（例如超前对齐 1 秒最多一次）；且只在 `mSeekFlag==false` 时做。
**判据**：A2（`refused` 归零）；A7；A1。

---

### W1.3 ★目标 Representation 必须落在切换点★（对应 RC-4b）
> **2026-09-21 修正（C1 埋点阶段就查清了，原判断有偏差）**：把 HLS 日志按 `mark startTime` /
> `open uri` / `read seg` 逐条对齐之后，**seek 本身是对的**：
> ```
> 07.556 HLSStream: seek:1498 stream (0) seek us is 34831000   （请求 34.831s）
> 07.556 HLSStream: pending seek                                 （tracker 还没 init）
> 07.564 HLSStream: open_internal:374 stream (0) usSeeked is 33366666 seek num is 5
> 07.566 HLSStream: open uri is ...-3840_2160-4.m4s seq is 5
> 07.604 HLSStream: stream (0) mark startTime 33366666            ← 精确定位到 33.366s ✅
> ```
> 真正的问题在后面 0.5 秒：读取线程**一路跑了 5 个分段**
> （07.709 → 41.7s，07.820 → 50.05s，07.909 → 62.56s，07.986 → 70.90s，08.139 → 83.42s），
> 把 33.4s→83.4s 全塞进了包队列，而播放点只有 34.9s。
> 根因是**读前量只按「缓冲时长」限流、没有按「播放时钟」限流**：
> `doReadPacket()` 的停止条件是 `cur_buffer_duration > mSet->maxBufferDuration`
> （外加 `mBufferIsFull` + gap 判断，`SuperMediaPlayer.cpp:1439-1457`），而
> `maxBufferDuration` 是几十秒量级 —— 对一个 VOD 播放器这是合理的缓冲目标，
> 但**帧队列只有 2 帧、`RenderVideo()` 又拒绝渲染「早于时钟 10ms」的帧**，
> 于是「缓冲里合法地存着 50 秒未来视频」直接等价于「画面停 50 秒」。
>
> **所以 W1.3 的正确做法是加一条「按播放时钟的读前闸门」**（而不是重新 seek）：
> - 在 `doReadPacket()` 的停止条件里补一条：`frontPts − mMasterClock.GetTime() > kMaxReadAheadUs`
>   （建议 2~3 秒，且**只在清晰度切换的 warm-up 阶段和提交后的一小段窗口内**生效，
>   避免影响普通 seek 后的快速填充）；
> - 提交（`TryCommitPendingVideoSwitch`）之后，对**新的 active 流**也要有这条闸门，
>   不能只限制 pending 阶段（HLS 那 5 个分段正是在提交之后读进去的）；
> - 落点校验（下面的第 4 条）仍然保留：它能把「seek 没生效」和「seek 生效但被读飞了」
>   这两件事永久区分开。
>
> 下面原始的 W1.3 描述保留，作为「如果是 seek 没生效」那条分支的处理方案。

**问题**：`SwitchVideo()`（`SuperMediaPlayer.cpp:4704`）的顺序是 `OpenStream(target)` → `Seek(target, startTime)`。
但 `HLSManager::OpenStream()`（`HLSManager.cpp:327`）在 `mFirstSeekPos == INT64_MIN` 时**不 seek 就直接 `open() + start()`**（起读线程），
而 `mFirstSeekPos` 在播放中永远是 `INT64_MIN`（见 `HLSManager::seek():453-457`）。
→ 目标流用 **tracker 默认游标**起读（HLS 那份日志一度表现为落在 `-9.m4s` 70.9 秒 —— 但按 `open uri` 逐条核对，
那其实是**读飞了**而不是 seek 没生效，原因见上面的修正）。

**改法**：
1. `HLSManager` 增加带 seek 的打开：
   ```cpp
   int OpenStream(int index, int64_t seekUs = INT64_MIN);   // 打开后、start() 之前先 seek
   ```
   实现：`open()` 之后、`i->mPStream->start()` **之前**，若 `seekUs >= 0` 则 `i->mPStream->seek(seekUs, 0)`；
   并在 HLSStream 内部保证「seek 结果确定后再启动读线程」（若 `HLSStream::seek` 是异步/pending 的，就在 `start()` 前同步等待它落定一次）。
   *（注：实测 `pending seek` 会被 `open_internal` 消费掉，所以这条的收益主要是「消除竞态」而不是「修落点」。）*
2. `demuxer_service` / `IDemuxer` 增加 `OpenStreamAt(int64_t us, int index)`（默认实现 = `OpenStream` + `Seek`，供 DASH 与其它 demuxer 复用）。
3. `SuperMediaPlayer::SwitchVideo()` 改成：
   ```cpp
   int ret = mDemuxerService->OpenStreamAt(startTime, targetStreamIndex);
   // 成功后校验落点（见 W0.5 的那行 [seek] 日志）：|streamStart − startTime| > 1 个分段就判失败
   ```
   保留现有的「失败 → `NotifyVideoQualitySwitch(FAILED)` + 关流 + `mWillChangedVideoStreamIndex=-1`」路径。
4. 落点校验：用 W0.5 新加的 `[seek] hls/dash …: tracker positioned at segNum=…` 这条日志判定
   （比原来的 `mark startTime` 更早、更直接 —— 它在 seek 返回前就打出来了）；
   偏差 > 一个分段就视为定位失败，直接 FAILED 而不是硬着头皮预热。

**风险**：HLS 的 `seek` 在 manager 里会走「seek all selected streams」分支（`index==-1`）。必须用 `index != -1` 的路径（`HLSManager.cpp:526-567`），不能碰旧流。
**判据**：A7（`HLSStream: stream (0) mark startTime` ≈ 切换点；DASH `getCurSegNum` 不再一路跑到 7）。

---

### W1.4 退役流的包原子清除 + `drop stale` 计入 flow（对应 RC-3）

**问题**：旧 representation 已进入共享 `BUFFER_TYPE_VIDEO` 队列的包，只能由解码循环一个一个丢（DASH 370 个 / 4.4 秒，HLS 500+ 个），且这条路径**不更新 `mVideoFlowMs`**（`doDeCode():2560`），于是看门狗把「一直在丢包」误判成「整条路死了」，反向触发追帧。

**改法**：
1. `MediaPacketQueue` 增加 `int DropPacketsByStream(int streamIndex)`（内部按 `mCurrent..end` 重建 deque、同步 `mDuration/mTotalDuration/mCurrent`，**不要**逐个 `getPacket()`）；`BufferController` 转发。
2. `finishQualitySwitch()` / 提交前一次性调用：`mBufferController->DropPacketsByStream(mRetiredVideoStreamIndex)`。
3. `doDeCode()` 的 `drop stale video packet` 分支加 `mVideoFlowMs = af_getsteady_ms();`。
4. （可选、更强）把 `mPendingVideoPacketQue` 推广成 **按 streamIndex 分队列**，让 `GetFirstKeyPTSAfter()` / `ClearPacketBeforePTS()` 重新只在单条时间轴上工作（同时修掉 `next key pts` 在两路交错时乱跳的问题）。

**风险**：逐个丢 → 一次丢，语义更硬；要确认 `mDuration` 记账正确（`assert(mMAXBackwardDuration != 0 || (mTotalDuration == mDuration && mCurrent == mQueue.begin()))` 会抓出来）。
**判据**：A3（`drop stale` 归零）。

---

## 4. 阶段 2（P1）：把时间轴统一

### W2.1 DASH 的分段起点照 HLS 改（根因 RC-2 的一半）
- 位置：`framework/demuxer/dash/DashStream.cpp:846`（`mStreamStartTimeMap[i].timePosition = mCurSeg->fixedStartTime;`）
- 现状：`fixedStartTime` 来自 MPD 的**名义栅格** `(N-1)*SegmentTemplate.duration`；
  实测 `open segment …, 29999999` ↔ `pFrame->pts 33366667`，差 **3.37 s**，且**每跨一个分段都跳一次**。
- HLS 的做法是对的（`HLSStream` 用分段首个包的真实 PTS 当段起点，`mark startTime 20854166` ↔ `pts 20854167`）。
- 改法：`fixedStartTime` 只作初始估计；每个新分段打开后用「首个 packet 的 pts」校正一次段起点，并把偏移累加进 `time2ptsDelta`。
- 判据：DASH 日志里 `timePosition ≡ pts`（不再出现同一帧差 2.5~3.4 s）；同时 DASH 那次「seek 之后 `next key pts 25025000` vs `master 15853165`」应当消失。

### W2.2 切换状态机统一到一根轴 + `offset` 无条件建立
- `SuperMediaPlayer.cpp:4224`：`mPendingVideoSwitchTimePosition = mMasterClock.GetTime();` → 改成用**当前播放帧的 pts**（`mPlayedVideoPts`，缺失时退回 `mCurrentPos`）。
- `FillPendingVideoFrame():2935` 与 `TryCommitPendingVideoSwitch():3065/:3118/:3129`：判据里比较的两个量必须在同一根轴上（统一用帧 pts）。
- `mPendingVideoPtsOffset` 的建立条件去掉 `frameTimePosition < 0`（`:2991`），改成「只要 `|pendingFirstPts − masterPts| > 阈值` 就建立一次，并固定用于该解码器整个生命周期」。
- 判据：A5（`offset` 不是 `INT64_MIN`）。

### W2.3 提交门限收紧
- `TryCommitPendingVideoSwitch():3065` 的 ±2 s 窗口 → 收紧到 **≤ 1 帧**（`frameDuration` 或 40ms 取大者）。
- 宁可在目标流上多等一个分段，也不要把「落后 1.57 s 的帧」当锚点（HLS 那份的 `pts=33366667 master=34941672`）。

### W2.4 `mPtsDiscontinueDelta` 来源核查
- 两份日志都是 `10000000`（10 s）。来源 `getMaxGopTimeUs()`（`:3968-3975`）。
- HLS 分段实测 4.2~12.5 s，DASH 名义 10 s。10 s 的「PTS 不连续阈值」让 `mVideoPtsRevert` 与 `TryCommit` 里的 `pts + delta < mPlayedVideoPts` 形同虚设。
- 改法：`getMaxGopTimeUs()` 对 HLS 应返回**最大分段时长**而不是 10 s 常量；并把该值 clamp 到 `[1s, 5s]`。
- 判据：日志里 `mPtsDiscontinueDelta` 在 HLS 上应是 ~4.2~12.5 s 中的合理值。

---

## 5. 阶段 3（P2）：设备、锁、日志、UI

| 编号 | 改动 | 位置 | 判据/风险 |
|---|---|---|---|
| W3.1 | pending 解码器**使用与 active 相同的 hw device / frames context**（现在传 `nullptr`）；`finishQualitySwitch` 里先 `releaseInputState()` 再 `releaseRetiredVideoDecoder()`，新帧到达后再重建输入视图 | `SuperMediaPlayer.cpp:3031`（`setUpPendingVideoDecoder(..., nullptr, 0)`）、`SMPAVDeviceManager.{h,cpp}`、`CicadaVideoTexture::releaseInputState()` | 4K↔4K 切换不再有左上角残片/黑帧；需真机实测 |
| W3.2 | `SMPAVDeviceManager::mMutex` 里不再做「4K 解码器 close」：`releaseRetiredVideoDecoder()` 的 `flush+close` 移到锁外（或延迟回收队列） | `SMPAVDeviceManager.cpp:198/215` | 切换时主循环不再出现整帧停顿（`mReadLoopIndex` 不再掉到 81） |
| W3.3 | 三类洪水日志限频（已在 W0.4 做）+ `[switch]` 保留为常驻诊断 | 同上 | 日志 < 300 行/次切换 |
| W3.4 | UI：`CicadaPlayerItem` 加**切换看门狗**（仿已有的 `m_seekWatchdog`/`onSeekWatchdog`，`CicadaPlayerItem.cpp:1184-1200`）；收到 `STARTED` 起 3 秒仍无终态就发 `FAILED` 并回退高亮 | `platform/QtPlayer/src/CicadaPlayerItem.{h,cpp}`（`notifyQualitySwitchStatus():1166` 已处理 FAILED/CANCELED，只差超时） | A6；界面不再停在「切换中」 |
| W3.5 | UI：URL scheme **白名单**（`http/https/file/rtsp/rtmp/data`），不合法直接给明确提示 | `platform/QtPlayer/HomeWindow.qml:779` | 上次 `zhttp://` 那类错误不再以 `Unsupported protocol` 出现 |

---

## 6. 验证方案

### 6.1 每阶段的机器可判定检查
所有检查都是**在日志上 grep**，不需要人眼看画面：

```powershell
# A2/A3/A4：三个必须为 0
Select-String -Path log.txt -Pattern 'skip video forward refused|drop stale video packet|unknown stream' | Measure-Object     # Count 必须 0
# A5：offset 不是 INT64_MIN，且 pts 与 master 差 <= 1 帧
Select-String -Path log.txt -Pattern 'committed seamless video decoder switch'
# A6：每次 'quality switch started' 后面必须有一条 status=1/2/3
Select-String -Path log.txt -Pattern 'quality switch status='
# A7：目标流落点
Select-String -Path log.txt -Pattern 'HLSStream: stream \(0\) mark startTime|getCurSegNum is'
# A1：切换后 fps 恢复
Select-String -Path log.txt -Pattern 'video fps is'
```

### 6.2 测试矩阵（同一份 mock 片源，`127.0.0.1:9000`）

| 用例 | 片源 | 操作 | 期望 |
|---|---|---|---|
| T1 | `output.mpd`（8 表示） | 480p HEVC → 2160p H.264（跨 codec，必现路径） | A1~A9 全过 |
| T2 | `output.mpd` | 2160p → 480p（降档） | 同上；且不出现 `Failed to parse header of NALU` / `PPS id out of range` |
| T3 | `master.m3u8` | 480p → 2160p（同 codec 换分辨率） | 同上 |
| T4 | `master.m3u8` | 连续快速点 3 档（触发 superseded 分支） | 只有最后一档 READY，前两档 CANCELED，且旧流都关掉 |
| T5 | 两份片源 | 切换前先 seek 到 12.8s / 31.8s，再切 | 不出现 `next key pts` 远超 master |
| T6 | 本地 mp4 | 播放 + seek + 暂停/恢复 | 无回归 |
| T7 | `master.m3u8` | 切换中途关窗 | 正常退出，无泄漏（`~SuperMediaPlayer` 后无 `unreleased resources`） |

### 6.3 需要人工确认的
- 切换瞬间画面**不黑、不闪、不跳帧**（这是「无感」的本体，日志判不了）。
- 切换后音画同步（口型）。

---

## 7. 工作量与提交顺序（建议）

| 提交 | 内容 | 预估 | 依赖 |
|---|---|---|---|
| C1 | 阶段 0（W0.1~W0.5） | 1~2 h | — |
| C2 | W1.1 切换收尾解耦 + 超时兜底 | 3~4 h | C1 |
| C3 | W1.4 退役流原子清除 + drop 计入 flow | 2 h | C1 |
| C4 | W1.2 「太早」判游标错位 + 追帧计数拆分 | 3~4 h | C2/C3（否则判定被洪水污染） |
| C5 | W1.3 目标流定位确定化（HLSManager/OpenStreamAt） | 4~6 h | C1；与 C4 可并行 |
| C6 | W2.1 DASH 段起点改用真实 PTS | 3 h | C5 |
| C7 | W2.2/W2.3 状态机统一 + 门限收紧 | 3 h | C6 |
| C8 | W2.4 + W3.3 日志 | 1 h | — |
| C9 | W3.4/W3.5 UI | 2 h | C2 |
| C10 | W3.1/W3.2 GPU 与锁 | 4~8 h（要真机反复试） | C2~C5 |

**建议按 C1 → C2 → C3 → C5 → C4 → C6 → C7 的顺序走**：
先让状态机有终态（C2）和旧流能被清掉（C3），再修目标流定位（C5），最后才动「超前对齐」（C4）——
否则 C4 的追帧策略会在「目标流落点本来就错」的前提下产生新的误判。

---

## 8. 本次明确**不做**的事

1. **不重写双路解码架构**。保留「旧路继续出画 → 目标路预热 → 原子提交」的模型（它与 ExoPlayer/AVPlayer 一致），
   只把上面的定位、门限、终态、清理补上。真要换成一解码器 + 分段边界 flush 的方案，是另一个量级的改动，等这轮验证完再说。
2. **不动音频/字幕/DRM 的切换路径**。
3. **不改 `CicadaHardwareDevice` 的单例语义**（那是 4c 已修好的东西）。
4. **不做 ABR 策略调整**（`AbrManager` 不在本次范围）。
5. **不生成测试视频**（用户既有要求），用现成的 `output.mpd` / `master.m3u8`。

---

## 9. 上线前必须回答的三个悬案（阶段 0 就是为它们准备的）

| 悬案 | 需要的日志 | 影响 |
|---|---|---|
| 提交后 0.1 s 内是谁 `FlushVideoPath()`？ | W0.3 | 决定 W1.1 里那个 `finishQualitySwitch(false, "flush …")` 要放在哪 |
| 新 4K 流为什么落在第 10 段（70.9s）？ | W0.5 + W1.3 的落点校验 | 决定 W1.3 是「seek 没生效」还是「seek 生效后被 reader 覆盖」 |
| promote 之后 4K 解码器到底出没出帧？ | W0.2 的 `[switch] targetFrameQ=` | 决定 W3.1（GPU 上下文）的优先级 |
