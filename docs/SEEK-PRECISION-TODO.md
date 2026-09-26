# 精准 seek（100% 帧级）—— 现状与剩余工作

> 目标：seek 后**首个上屏帧**必须是"包含目标时刻"的那一帧（目标落在两帧之间时取**不晚于**
> 目标的那一帧），位置上报等于目标时刻；本地文件 / DASH / HLS 三条来源一致；
> **不得**用看门狗、超时、预算（"精度换等待"）实现。

## 已完成（提交 00257f10，基于 57ef726a）

文件：`mediaPlayer/SuperMediaPlayer.cpp`，落点采纳块（`RenderVideo()` 内，`mSeekRenderGateUs` 那一段）。

1. **采纳判据 = 包含目标**（原为预算判据 `SEEK_EXACT_LANDING_BUDGET_US`）：
   ```cpp
   const int64_t seekFrameDurUs = (videoFrame->getInfo().duration > 0)
                                  ? videoFrame->getInfo().duration : 40000;
   ...
   } else if (frameTimePos != INT64_MIN && mSeekPositionFloorUs != INT64_MIN &&
              frameTimePos + seekFrameDurUs <= mSeekPositionFloorUs) {
       render = false;      // 完全在目标之前 ⇒ 不包含目标 ⇒ 继续解
   } else {
       ...采纳 + 锚主时钟 + 撤销闸门...
   }
   ```
   - 帧 PTS 单调 ⇒ 必然在有限帧内到达"包含目标"的那一帧；无计时器、无预算。
   - 目标落在落点之前（稀疏 IDR：第一帧就晚于目标）⇒ 条件不成立 ⇒ 直接采纳它。
2. **`timePosition` 缺失的容器/流**：`frameTimePos < 0` 时退回帧自己的 `pts`。
   原来整块落点判定被 `if (frameTimePos >= 0)` 跳过 ⇒ 那类来源**永不采纳、永不锚定**。

## 剩余工作（按优先级）

1. ~~**帧长兜底的 off-by-one**~~ **已完成（提交 `6e05686a`）**：`duration` 缺失时按**本流实际帧率**
   （`mCurrentVideoMeta->operator Stream_meta *()->avg_fps`，取法与 `SuperMediaPlayer.cpp:1669` 一致）
   推算帧长，不再写死 25fps/40ms。
2. **把丢弃点搬进 seek 状态机（去掉渲染路径上的 seek 条件）** ← 当前这一项
   目标形状 —— 渲染路径不认识 seek；"解到目标"由 seek 自己完成（丢弃发生在进入渲染之前）。
   步骤：
   a. 读 `ProcessSeekToMsg`（seek 真正开始处，`mSeekFlag` 置真）、`FlushVideoPath`、
      `DecodeVideoPacket`，确认"解码 → 帧入 `mVideoFrameQue`"这一步的唯一入口；
   b. 在 **seek 阶段**加"包含目标"丢弃：帧区间 `[timePosition, timePosition+帧长)` 不含目标
      且目标在其之后 ⇒ seek 自己丢掉该帧、继续解（与现已提交的判据同一份数学，只是位置换到
      seek 侧）；seek 完成条件 = 出现包含目标的帧（或第一帧就晚于目标）；
   c. 渲染路径只保留"脏帧不上屏"（`mSeekDecodeStartIsKey`）这一条**与 seek 无关的**保护；
   d. 删除这组闩及其全部引用：`mSeekRenderGateUs`、`mSeekLandingFrameAccepted`、
      `mSeekExactLandingByBudget`、`SEEK_EXACT_LANDING_BUDGET_US`、`mSeekPositionFloorUs`；
      引用点（`grep` 已确认）：`SeekTo()`（`:646` 附近复位）、`RenderVideo()` 采纳块、
      `DecodeVideoPacket`（授予预算那段）、`getCurrentPosition()`（地板语义）、`Reset()`；
   e. 位置上报：seek 完成时直接置为目标时刻，不再靠地板兜。

   **删除清单（grep 已逐处确认，逐条删即可）**：

   | 要删 | 引用点 |
   |---|---|
   | `SEEK_EXACT_LANDING_BUDGET_US` | 定义 `SuperMediaPlayer.cpp:240`；使用 `:4853`、`:4858`（`DecodeVideoPacket` 授予预算那整块）；注释 `:4911`、`:7328` |
   | `mSeekExactLandingByBudget` | `SeekTo()` `:684`；授予 `:4854`；渲染采纳块内的 `= false`；`Reset()` `:9645`；声明在 `SuperMediaPlayer.h` |
   | `mSeekRenderGateUs` | `SeekTo()` `:653`；渲染采纳块 `:7272`/`:7299` 一带；`Reset()` `:9631`；声明 `SuperMediaPlayer.h:1215` |
   | `mSeekLandingFrameAccepted` | `SeekTo()` `:654`；渲染采纳块多处；`Reset()` `:9632` |
   | `mSeekPositionFloorUs` | `SeekTo()` `:705`；`DecodeVideoPacket` `:4853`；`getCurrentPosition()` `:1147` 一带；`Reset()`；声明 `SuperMediaPlayer.h:1072` |

   替代位置：discard 放到 `DecodeVideoPacket`（`:4683`）里"帧入 `mVideoFrameQue`"的唯一入口，
   判据与已提交的那份数学相同（`timePosition + 帧长 <= 目标 ⇒ 丢`）；渲染路径只留
   `mSeekDecodeStartIsKey`（脏帧不上屏，与 seek 无关）。
3. **验收（必须由人工执行，AI 侧无法编译/运行）**：本地 / DASH / HLS 各 seek 一次，记录
   - seek 后首帧上屏的 pts 与目标的差（方向应为"不晚于"，幅度 < 1 帧长）；
   - `NotifyPosition()` 的值（应等于目标时刻）。

## 相关位置速查

| 内容 | 位置 |
|---|---|
| `SeekTo()`（复位所有 seek 闩） | `mediaPlayer/SuperMediaPlayer.cpp:646` |
| 落点采纳块 | `mediaPlayer/SuperMediaPlayer.cpp`（`if (mSeekRenderGateUs != INT64_MIN && (render \|\| mSeekFlag))`） |
| 帧结构（`duration` / `timePosition`） | `framework/base/media/IAFPacket.h:205-216` |
| 视频帧队列 | `mediaPlayer/SuperMediaPlayer.h:736`（当前是 `std::queue`；**本方案不需要改容器**，因为包含判据只用帧自身 `duration`） |
