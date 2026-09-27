# P0–P3 交接单：现在该构建什么、该看哪些日志

> 本文件面向**构建者与验收者**（用户）。架构细节见 `docs/SINGLE-DECODER-REFACTOR.md`；
> P3 的前置事实见 `docs/P3-BOUNDARY-SWITCH-NOTES.md`；P4 的前置事实见 `docs/P4-DEMUXER-NOTES.md`。

## 一、已完成的架构（P0 → P3，全部已提交）

三条"唯一"，取代了原来那一整套闩与状态机：

| 唯一权威 | 实现 | 位置 |
|---|---|---|
| **唯一内容时间轴** | `mMasterClock.GetTime()`：有音频时 = 目标点基准 + **设备已消费量**；无音频时 = 暂停感知自走时钟。`getCurrentPosition()` 只读它 | `SuperMediaPlayer.cpp` `getCurrentPosition()` / `getAudioPlayTimeStamp()` |
| **唯一落点过滤（精度权威）** | `shouldDropForDiscontinuity(framePos, frameDur)`：完全在目标之前的帧不上屏；包含/越过目标的帧上屏并**结束过滤**；EOF 结构性终止。结束过滤**只**能由它或下一次 `beginDiscontinuity()` —— `SeekEnd` 没有能力关它 | `SuperMediaPlayer.cpp` `RenderVideo()` 落点链 + 文件末尾 |
| **单解码器切档** | `SwitchVideo(switchPos)` = `CloseStream(旧) → OpenStream(新) → Seek(switchPos, 0, 新) → 只 flush 视频 → 同 meta 不匹配才原地重建**同一块**解码器 → beginDiscontinuity(switchPos)`；落点帧被采纳即 `NotifyVideoQualitySwitch(READY)` | `SuperMediaPlayer.cpp` `SwitchVideo()`；入口 `SMPMessageControllerListener::switchVideoStream()` |

**已彻底删除**（残留验收命令输出 `0`）：pending/retired 解码器与队列、占位面 B2/B2'、预热与追播放时间线、
`mQualitySwitch*`/`mSwitchReArm*`/`mPausedSwitch*`/`mWillChangedVideoStreamIndex`、全部切档死线常量
（`QUALITY_SWITCH_TOTAL_TIMEOUT_MS`、`PENDING_PREROLL_WAIT_MAX_MS`、`DECODE_STALL_REBUILD_ROUNDS` 等）、
`seek` 闩群（位置地板/落点闸门/归属标记/预算判据）、音频地板与锚点闩、`fetchSeekClockAnchorUs()`、
"扣住 PCM 等时钟"那道门。切档的 `FAILED` 现在**只**由确定性错误发出（开流/定位/meta/重建失败），
不再有"等太久判失败"。

## 二、构建（**必须全量重编**）

P1/P2/P2.1/P3 都在成员列表中间删过成员、并新增过成员 ⇒ 旧目标文件按旧偏移编译会错位。
**Qt / msvc 目标**（父代理已用它做过验证）：

```powershell
# 先删掉陈旧的增量产物（至少删 media_player.dir 下的 .obj）
Remove-Item -Recurse -Force platform\QtPlayer\build\msvc-static\mediaPlayer.out\media_player.dir
cmake --build platform\QtPlayer\build\msvc-static --target media_player --config Release
```

**父代理实测结果**：`media_player.lib` 生成成功，**0 error / 0 warning**（MSVC 14.39）。
若你只想快速复核，同一命令即可；Android/HarmonyOS/iOS 侧同样要全量重编。

## 三、真机验收标记

### seek（本地 / DASH / HLS）

**应看到**（每次 seek）：
- `seek landing frame accepted: pts=…, offsetFromTarget=… ms, generation=…, afterSeekMs=…, reason=…`
  —— `offsetFromTarget` 应满足 `-1 帧 ≤ offset ≤ 0`（落点不晚于目标），**这是精度的唯一判据**；
- 位置上报等于目标且**单调不回退**（`NotifyPosition` 在 seek 后不再回退）；
- 暂停时位置**恒定**（不前进）；seek 后若发生缓冲停顿，位置也**不再前进**（P2.1 修的正是这一类）。

**不应看到**：
- `seek landing refused` / `landing window is still OPEN` / `seek first decodable frame shown … fast-forwarding`
  （旧落点窗口语义已删）；
- `seek 后进度条回弹`、`画面往回倒`（前缀帧现在一律不上屏，画面停在上一张直到"包含目标的那一帧"渲染）；
- 花屏/马赛克（不是从关键帧起步的帧不上屏）。

### 音频

**应看到**：`audio first frame after seek … afterSeekMs=` **≤ 200**；
`audio landing drop: dropped=… pos=… target=… generation=…`（一个 seek 至多一条，用来对账
"音频被正确丢到目标点"vs"音频根本没来"，它的 `target` 应与 `seek landing frame accepted` 的 `target=` **相等**）。

**不应看到**：`audio silence starts (reason=1 …)` 长窗口、`audio keep-alive: reached the resource cap`。
（`reason=2/3/4` 属设备侧既有机制，该出现时仍应出现。）

### 切档（手动 + ABR）

**应看到**：`quality switch: closed the old video stream … (single-decoder switch)` →
`target stream … opened in … ms` → `target stream … seeked to … us in … ms` →
（需要时）`the target stream needs an in-place decoder rebuild … the SINGLE decoder is rebuilt, never a second one` →
落点帧上屏后 `NotifyVideoQualitySwitch(READY)`，即 `status=0 → status=1`。
位置/声音在切档期间**连续**（音频流全程不被 flush、不被重定位）。

**不应看到**（这些字符串随 P3 一起从源码里消失了）：
`[switch] state=decoderSwitch`、`pendingPktQ`/`pendingFrameQ`、`pending preroll`、
`placeholder surface handover`、`quality switch timed out`、`wait a key frame` 风暴、
`video decoder accepts no input … rebuilding it once`、两块 4K 实例 `-1010`、
`quality switch deadline hit` / `preroll deadline hit`。切档**不应**再出现 `FAILED`/`CANCELED`
（除非真的有开流/定位/重建错误）。

## 四、两处工程裁决（已定，供你知情）

1. **"首帧立即出画"的口径**：它的准确含义是**不等时钟**（落点帧走 `force_render`），
   **不是**允许把早于目标的帧推上屏。依据：要求里"首个**最终**上屏帧必须是包含目标时刻的那一帧"
   + "不能错误"；这也与 ExoPlayer 一致（`bufferTimeUs < pendingPlaybackPositionUs` 一律 skip，
   画面停在上一张等目标帧）。已实测 `clearScreen()` 只在 `ClearScreen()` 与 stop 且
   `mSet->clearShowWhenStop` 时调用 ⇒ seek 路径不会清屏、不会黑。
2. **`mSwitchStateLogMs` / 少量 pending 宏**：作为无引用残留保留了一小部分（不影响任何活跃行）；
   `PENDING_CAP_AUDIO_MIN_US` **仍在使用**（读前闸门），不得删。P5 会做最后一次清理。

## 五、剩余工作

- **P4**：解复用器"落点延迟线"（`HLSStream` 与 `DashStream` 各一份、同形）**收敛成一份共享实现**。
  注意：这条线**不是残留，必须保留** —— 它把解码起点从"分片片首 ≤10s"挪到"不晚于目标的最后一个关键帧"，
  是**降低 seek 延迟**的优化；删掉会让分片源 seek 变慢。P4 的验收是"只有一份实现"。
- **P5**：注释/命名终检、`-CaseSensitive` 残留命令复核、平台宏与 vtable/成员追加等工程约束的静态检查、
  以及本条日志验收的最终回归。
