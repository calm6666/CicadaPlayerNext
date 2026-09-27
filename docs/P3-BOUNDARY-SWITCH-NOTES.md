# P3 前置事实：切档的既有原语（父代理静态勘察，行号为 P1-c 之后）

> 用途：P3（单解码器切档）施工前必读。这里只记录**代码事实**与**两条可选路径**，
> 不代替 `docs/SINGLE-DECODER-REFACTOR.md` §2 的四条规则。P3 完成后请把本文并入该文档的 §10/P3 节。

## 一、接口事实

| 接口 | 位置 | 语义 |
|---|---|---|
| `demuxer_service::OpenStream(int index)` | `framework/demuxer/demuxer_service.h:115` | 打开/选中某路流 |
| `demuxer_service::CloseStream(int index)` | `demuxer_service.h:117` | 关闭某路流 |
| `demuxer_service::Seek(int64_t us, int flags, int index)` | `demuxer_service.h:75` | **按流定位**（带 index），可对"新开的那一路"单独定位 |
| `demuxer_service::SwitchStreamAligned(int from, int to)` | `demuxer_service.h:129` | 见下（现在被双解码器改了语义） |
| `IDemuxer::SwitchStreamAligned` | `framework/demuxer/IDemuxer.h:152` | 虚接口 |
| `AbstractStream::stopOnSegEnd(bool)` | `framework/demuxer/play_list/AbstractStream.h:52` | 让某个流"在本分片边界停下" |

## 二、`SwitchStreamAligned` 的原始语义仍完整保留在 manager 里

**原始（单流、边界交接）语义**：给旧流置 `stopOnSegEnd = true` + `toStreamId = to`；
manager 的读循环在旧流**到达分片边界**时执行交接：
- 清 `stopOnSegEnd` / `toStreamId`；
- `i->mPStream->stopOnSegEnd(false)`；
- `OpenStream(toStreamId)`（新流从它自己的边界关键帧开始）。

这些代码今天**一行都没少**，只是因为 `stopOnSegEnd` 永远是 false 而从不执行：
- `framework/demuxer/play_list/HLSManager.cpp:211-219`（边界检测）、`:254-258`（交接动作）、
  `:483-492`、`:547-556`（另两条等价出口）
- `framework/demuxer/dash/DashManager.cpp:221-229`、`:244-248`、`:468-477`、`:530-539`

**被改成什么样（双解码器的痕迹）**：
- `HLSManager.cpp:574-590` 的注释明说「双 decoder 切换期间旧 HLS 流不能在分片边界提前关闭……
  stopOnSegEnd 仅用于普通 seek/非双路切换流程」，函数体**只写 `toStreamId`，不置 `stopOnSegEnd`**。
- `DashManager.cpp:557-578` 同形，注释明说「清晰度切换由 SuperMediaPlayer 的双路 decoder 状态机完成……
  实际关闭由 SuperMediaPlayer::RenderVideo() 在提交后执行」。

⇒ 也就是说：**双解码器那条路是把 demuxer 自带的边界切换能力关掉之后，在上层重造了一套**
（pending/retired 解码器、占位面、追播放时间线、死线仲裁）。P3 的方向就是把上层那套删掉、
把 demuxer 的边界切换打开 —— 这是"删代码 + 恢复既有能力"，不是新写机制。

## 三、两条可选路径（P3 决策点）

**路径 A：立即切换（推荐，满足"切换时间必须短"）**
```
记录目标 index → CloseStream(old) → OpenStream(new) → Seek(当前位置, flags, new)
→ 只 flush 解码器 + 帧队列（FlushVideoPath 的既有形态）→ 用 isVideoDecoderMetaMatched()/rebuildVideoDecoder()
   重建**同一个**解码器 → beginDiscontinuity(当前位置)
```
- 用 P1 的单一落点过滤保证"新流首个上屏帧是包含当前位置的那一帧"；
- 不需要任何 pending 队列/占位面/死线；
- `Seek(us, flags, index)` 已存在，seek 路径的落点处理是现成的。

**路径 B：边界交接（最省事，但切换延迟 = 到下一个分片边界的剩余时间）**
```
SwitchStreamAligned(from,to) 恢复置 stopOnSegEnd=true → manager 在边界自己 OpenStream(new)
→ 上层只负责"新流的包到了以后重建同一解码器 + beginDiscontinuity(边界位置)"
```
- 切换期间旧流持续播放，不中断；延迟上界 = 当前分片剩余时长（HLS/DASH 常见 2~6s）；
- 代码量最小（上层几乎只剩"重建同一个解码器"）。

**判定标准**（用户要求）："100% 成功" + "切换时间必须要短" + "手动与 ABR 都要成功"。
若路径 B 的边界等待被判定为"不够短"，就走路径 A；两者的落点精度都由 P1 的
`Discontinuity + shouldDropForDiscontinuity` 承担，不需要为精度另加机制。

## 四、P3 完成时必须一起消失的东西（`docs/SINGLE-DECODER-REFACTOR.md` §3 清单）

`mPendingVideo*`（解码器/队列/预热/死线/占位面）、`mRetiredVideo*`、`mWillChangedVideoStreamIndex`、
`mQualitySwitch*`、`mSwitchReArm*`、`mPausedSwitch*`、`SMPAVDeviceManager` 里的
pending/retired 槽位与 `promotePendingVideoDecoder()` 之类的槽位交换、`b2Placeholder/b2SurfaceOutput/b2RealSurface`
（B2/B2' 占位面交接）、以及 `QUALITY_SWITCH_TOTAL_TIMEOUT_MS` / `DECODE_STALL_REBUILD_ROUNDS` /
`PENDING_VIDEO_STALL_CHECKS_MAX` / `PENDING_PREROLL_WAIT_MAX_MS` 这些死线常量。

**必须保留**：`NotifyVideoQualitySwitch(...)` 生命周期（`status=0 → status=1`，失败才 `2/3`）、
`isVideoDecoderMetaMatched()`、`rebuildVideoDecoder()`、`CreateVideoDecoder()`、`FlushVideoPath()`、
`mCurrentVideoIndex`、ABR 接口与 `ICicadaPlayer::IsStreamSwitchInFlight()`、JNI/Java 签名。
