# seek / 清晰度切档 整体重构 —— 验收单

> 本文件是本次重构（P0..P5）的**验收单**。执行方案与红线见任务书。
> 设计基准、零残留清单、基线统计与 **P1 施工图** 见 `docs/SINGLE-DECODER-REFACTOR.md`。
> 目标：把 seek 与清晰度切档从"十余个闩 + 双解码器 + 追播放时间线 + 墙钟死线"
> 整体重构为主流架构（不连续点 + 代际 + 单一渲染过滤 + 单解码器 + 状态判据），
> 并**删除全部双解码器代码**。
>
> 硬红线：不看门狗 / 不超时兜底 / 不新增配置开关 / 不动 UI/QML/Java 界面 /
> 不拿 GOP·IDR 密度当理由 / seek 必须 100% 帧级精准且立刻出画。

## 一、目标架构（六条，逐条验收）

### 1. 不连续点模型（唯一的时间轴权威）

`SuperMediaPlayer::Discontinuity { targetUs, startUs, generation }`。

* `targetUs`：用户请求点 —— **只**服务"位置上报"与"renderer 输出过滤"。
* `startUs`：demuxer 实际落点（可早可晚，关键帧）。
* `generation`：每次 seek / 换档 +1；**所有**异步事件（包、帧、时钟基准、
  待处理请求）带它判定归属，**代际不符一律作废**。

取代：位置地板、锚点事件闩、落点闸门、落点归属标记、音频地板、音频连续性
高水位、reposition 事件闩、追赶窗口……这一整组闩。

- [x] **P0** 结构与代际机制落地（只写入、与旧代码并存，零行为变化）
- [ ] **P1** 渲染过滤与位置上报改为读它
- [ ] **P2** 音频基准与 seek 目标改为读它
- [ ] **P3** 切档边界与在途请求改为读它

### 2. renderer 单一过滤规则（落点精度）

* seek / 换档 flush 之后的**第一帧立即出画**（不等时钟、不丢）。
* 之后：`frame.timePosition + frameDur <= targetUs` 的帧**全部丢弃**；
  `frameTimePos <= target < frameTimePos + dur` 的那一帧（**包含目标**）
  一旦出现 → **强制上屏并终止本次过滤**。
* **seek 宣告结束（UI 收到 SeekEnd）不得终止过滤** —— 过滤只由"包含目标的帧
  已上屏"或"下一次 seek / Reset / Prepare"结束。
  这是"分片源 seek 永远差一个落点前缀"的根因，必须在本阶段消灭。

删掉：`mSeekRenderGateUs`、`mSeekPositionFloorUs`、`mSeekLandingFloorOwnerUs`、
`mSeekFirstDecodableFrameShown`、`mSeekLandingFrameAccepted`、`mSeekExactLandingByBudget`。

- [ ] **P1**

### 3. 位置上报 = target + 单调时钟增量（不可能回退）

* 不连续点上把主时钟 / 位置基准设为 `targetUs`；
  之后 `position = targetUs + (now - baseAtDiscontinuity)`。
* `getCurrentPosition()` 里的"地板"兜底链删除；位置在数学上单调，回弹不可能发生。

- [ ] **P1**

### 4. 音频按"已写字节 → 位置"重设基准（对齐 ijkplayer / ExoPlayer）

* 不连续点上：`pause → flush → 把"已写出 PCM 字节数 → 媒体位置"的基准设成
  targetUs → play`；`audioPosition = base + bytesWritten / bytesPerSecond`。
* **永远不允许"扣住 PCM 等时钟"**（循环依赖：扣住 ⇒ 队列不出队 ⇒ 永远到不了锚点；
  真机造成 6.6~8.4s 静音并把上层看门狗触发停播）。没数据时按现有 keep-alive 写静音。
* 音频 seek 请求与视频**同一个目标点**（取消"音频定位到视频落点关键帧"的 C 方案）。

删掉：`mSeekAudioFloorUs`、`mSeekAudioContinuityUs`、
`mSeekAudioRepositionPending/Us/JumpUs`、`mSeekAnchorPending`、`mSeekClockAnchored`、
`mSeekVideoAnchorDone`、`mAudioClockReanchorPending`、`holdAudioForSeek` 那道门、
`fetchSeekAnchorUs` / `fetchSeekClockAnchorUs` 的多锚点判据（音频改基准重设；
视频侧删除）。

- [ ] **P2**

### 5. 切档：单解码器 + 分片边界（删除双解码器）

状态判据，无计时器：

1. 请求到达：只记"目标档索引 + 请求时的播放位置"（后到覆盖先到；同一目标忽略），
   立即 `NotifyVideoQualitySwitch(STARTED, index, "quality switch started")`。
2. 继续用**旧流单解码器**正常播放，等到切换点：当前流读到**下一个关键帧包**
   （`flags & AF_PKT_FLAG_KEY` 且 `timePosition >= 请求时的播放位置`）作为边界。
3. 到达边界：`CloseStream(old)` → `OpenStream(new)` → `Seek(边界位置, 0, new)`
   → 只清视频解码器与解码帧队列（`FlushVideoPath(false, false, ...)`，不碰渲染器）
   → 用目标流 meta **重建同一块解码器**（复用 `isVideoDecoderMetaMatched()`：
   匹配就只 flush；不匹配就 `invalidateDecoder()` + `CreateVideoDecoder()`）。
   **任何时刻只有一块视频解码器存在。**
4. 新流第一包即段首关键帧 ⇒ 重建后不需要"等关键帧"（不再有 `wait a key frame` 风暴）。
5. 音频不动 ⇒ 时间轴连续，不需要任何锚点 / 地板。
6. 终态：新流第一帧上屏 → `NotifyVideoQualitySwitch(READY, index, "quality switch rendered")`；
   任一步失败 → 回滚（重新 `OpenStream(old)` + seek 回边界）并报 `FAILED`，
   **不得影响继续播放**。seek 打断在途切档：请求继续挂着，seek 完成后在下一个
   关键帧边界执行（**不需要重发机制**）。
7. 保留 `NotifyVideoQualitySwitch` 生命周期与 ABR 让路
   （`IsStreamSwitchInFlight()` = "是否有挂着的切换请求"）。

- [ ] **P3**

### 6. demuxer 收口

demuxer 只报告"seek 后首个包的位置 + 是否关键帧"。删除
`DashStream` / `HLSStream` / `SegmentList` 里的
`seekLandingArm/Filter/Flush/Reset/DropStage`、`mSeekLandingStage`、
ENGAGE/RELEASE 那整套"落点暂存与丢前缀"策略（落点过滤已归 renderer）。

- [ ] **P4**

### 7. 缓冲与队列纪律（单一规则，无例外分支）

* 有界前向缓冲：`maxBufferDuration` 对**任何**状态生效（含切档在途），不保留回看缓冲。
* seek 一律"清包 + demuxer 重读"（`mSeekInCache` 捷径删除）。
* 删除读前闸门里"切档在途免检"之类特例分支（保留"当前路吃得住才停读"这条保护）。

- [ ] **P4**

### 8. 删除全部墙钟死线

切档 18s 上限、预滚 8s、`QUALITY_SWITCH_TOTAL_TIMEOUT_MS`、
`PENDING_VIDEO_STALL_CHECKS_MAX` 进度判据、解码器停滞重建里的时间语义。
`mSeekDecodeStartIsKey` 只能作为"解码器是否已过关键帧"的**纯解码器状态**，
不得再当停滞判据的闸。所有出口必须是状态判据。

- [ ] **P5**

## 二、必须删除的双解码器代码（全仓 grep 后逐一清除）

**SMPAVDeviceManager**：`mPendingVideoDecoder`、`mRetiredVideoDecoder`、
`getPendingVideoDecoder()`、`isPendingVideoDecoderValid()`、`setUpPendingVideoDecoder()`、
`getPendingVideoFrame()`、`sendPendingVideoPacket()`、`invalidatePendingVideoDecoder()`、
`discardPendingVideoDecoder()`、`promotePendingVideoDecoder()`、`releaseRetiredVideoDecoder()`。

**SuperMediaPlayer**：`CreatePendingVideoDecoder()`、`DecodePendingVideoPacket()`、
`FillPendingVideoFrame()`、`DrainPendingVideoFrames()`、`TryCommitPendingVideoSwitch()`、
`attachPendingVideoCodecParams()`、`mPendingVideoFrameQue`、`mPendingVideoPacketQue`、
`mPendingVideoStreamIndex`、`mPendingVideoDecoderSwitch`、`mPendingDecoderUsesPlaceholderSurface`、
`mPendingVideoPtsOffset`、`mPendingVideoSwitch*`、`mPendingVideoPreroll*`、
`mPendingVideoCodecParamsAttached`、`mPendingVideoProgressUs`、`mPendingVideoStallChecks`、
`mWillChangedVideoStreamIndex`、`mRetiredVideoStreamIndex`、`mQualitySwitch*`、
`mSwitchReArm*`、`mPausedSwitch*` / `mSwitchStartedWhilePaused`、
B2/B2' 占位面交接整块（`b2SurfaceOutput` / `b2Placeholder` / `b2RealSurface` /
`setOutputSurface(null)` 交接 / 回滚 / 仅服务该交接的 `mActiveVideoSurface`）、
`finishQualitySwitch()`、`checkQualitySwitchDeadline()`、`checkQualitySwitchPrerollDeadline()`、
`beginQualitySwitchTracking()`、`logQualitySwitchState()`、`resetPausedSwitchState()`、
`qualitySwitchInFlight()`、`[switch] state=...` 日志。

**SMPMessageControllerListener**：`ProcessSwitchStreamMsg` / `switchVideoStream` 里的
pending 路由、`OnPlayerMsgIsPadding` 里 `mVideoChangedFirstPts` 的切档语义（音频/字幕照旧）、
B17 重装、切档与 seek 的互相取消逻辑。

**ReadPacket / DecodeVideoPacket / doDeCode / FlushVideoPath / RenderVideo**：
所有 pending 分支与"切档提交 / 追赶 / 占位面"分支。

**保留**：`NotifyVideoQualitySwitch(...)` 生命周期、`isVideoDecoderMetaMatched()`、
`rebuildVideoDecoder()`、`CreateVideoDecoder()`、`FlushVideoPath()`、`mCurrentVideoIndex`、
ABR 接口、JNI/Java 接口签名（Java/Kotlin 仅在原生符号消失导致编译不过时最小改动，
**绝不改 UI/QML**）。

- [ ] **P3**（SMPAVDeviceManager / SuperMediaPlayer / SMPMessageControllerListener）
- [ ] **P4**（demuxer）
- [ ] **P5**（零残留 grep）

## 三、验收标记（构建后人工核对）

### P1

* 每次 seek 都有 `seek landing frame accepted ... offsetFromTarget=±(≤1 帧)`；
* 不再出现"落点窗口被提前关闭"；
* 位置不再回退。

### P2

* 不再出现 `audio silence starts (reason=1/4)` 长窗口与
  `audio keep-alive: reached the resource cap`；
* `audio first frame after seek` 与 seek 请求的间隔 ≤ 200ms。

### P3

* `[switch] state=decoderSwitch`、`pendingPktQ/pendingFrameQ`、`pending preroll`、
  `placeholder surface handover`、`quality switch timed out` **全部消失**；
* 切档只有 `status=0(STARTED) → status=1(READY)`；
* `ACodec` 不再出现两块 4K 实例抢缓冲的 `-1010`；
* 不再有成百行 `wait a key frame`；
* `video decoder accepts no input ... rebuilding it once` 消失。

### 全局

* `E [0.9]`、`onAudioException -1003/-1004`、`PlayerBase baseTimeout/baseStop` 连锁、
  `drop frame` 两轴差 > 1s **全部消失**。

## 四、进度

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | `Discontinuity{targetUs,startUs,generation}` + 代际机制（只写入）+ 本验收单 | **完成** |
| P1 | renderer 单一过滤 + 位置上报 | 待做 |
| P2 | 音频基准重设 + 音视频同一目标点 | 待做 |
| P3 | 切档改单解码器 + 关键帧边界 + 删全部双解码器 | 待做 |
| P4 | demuxer 收口 + 缓冲/队列单一规则 | 待做 |
| P5 | 清残留死线/日志 + 零残留 grep + 验收单收尾 | 待做 |
