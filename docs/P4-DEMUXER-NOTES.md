# P4 前置事实：解复用器"落点延迟线"该留还是该删（父代理静态勘察）

> 用途：P4 施工前必读。本文只记录**代码事实**与**结论**，不代替
> `docs/SINGLE-DECODER-REFACTOR.md` 的四条规则。P4 完成后请把本文并入该文档的 P4 节。

## 一、它到底在解决什么（代码自述，`HLSStream.cpp:2034-2079`）

`HLSStream` / `DashStream` 各有一份**同形**的"落点延迟线"（`seekLandingArm` /
`seekLandingFilter` / `seekLandingFlush` / `seekLandingReset` / `seekLandingDropStage` +
`mSeekLandingStage` 延迟队列）。原注释写得很清楚：

- **要解决的问题**：实测（10s 分片、片内每 2.5s 一个 IDR）seek 的落点关键帧比目标早 **3~9 秒**，
  落点落在"整 10 秒的分片片首"；播放器必须从落点一路解到目标，**这就是 seek 后 1~2.4 秒卡顿的全部来源**。
  分片内部的 IDR **无法用更小的字节范围落上去**（分片里只有一个 moof、没有可从内部进入的 box 边界，
  内层 demuxer 也不可 seek），清单里也只有分片级信息。
- **做法**：包是顺序出的，关键帧标记（`AF_PKT_FLAG_KEY`）就在包上 ⇒ 把解码起点从"分片片首"挪到
  **"不晚于目标的最后一个关键帧"**；它之前的包整体丢掉，从它到目标以及目标之后的包一个不丢、按序交出。
  落点误差从"分片长度(≤10s)"降到"关键帧间隔(本片源 2.5s)"。
- 只对**点播视频路**生效（音频没有 GOP 语义；混合流里丢前缀会连声音一起丢）。
- 状态迁移全部由"包上时间戳 + 关键帧标记"驱动，**没有计时器**（唯一与时间有关的是"攒前缀时不要每包白等
  10ms"，判据是"上一轮有没有真取到包"这种进度事实）。

## 二、结论：**保留它，但收敛成一份** —— 绝不要裸删

**为什么不能删**：它降低的是"必须解码前推的帧数"，也就是 seek 的**延迟**。
`docs/SINGLE-DECODER-REFACTOR.md` §4 里 P4 曾写"`[seekLanding]` 行消失"，那条**必须改**：
删掉它以后，分片源 seek 会退回"从分片片首起解码"，4K/HEVC 下等于多解 100~300 帧
（用户反复抱怨的"seek 后和加载还是很慢"会回来）。它与"精度换流畅"相反：它是**用更少的解码换同样的精度**。

**它也不是"第二个精度权威"**：它只丢"落点关键帧之前的包"，且**失败安全**（拿不到 timePosition 就整批按序交出）。
有了 P1 的 renderer 单一落点过滤之后，即使它交接得过早（多给了前缀），`shouldDropForDiscontinuity()`
也会把这些前缀帧丢掉、并把落点精确到"包含目标的那一帧"。**精度的唯一权威仍然只有 renderer 一个**
（P4 要在代码注释与文档里把这句话写死，避免后来者误以为 demuxer 也在判精度）。

**为什么要收敛**：`AbstractStream.h` 是**纯接口**（没有共享包队列，`:19-117`），
所以 HLSStream 与 DashStream **各写了一份**（约 50 行代码 + 60 行注释 × 2，实测残留里
`seekLanding`/`mSeekLandingStage` 合计约 190 处）。P4 应把它**提取成一份共享实现**：
在 `framework/demuxer/play_list/` 放一个小的 helper（一个状态结构体 + 三个函数：
`arm(target)` / `filter(packet)` / `flush()`），两个 demuxer 各自持有该状态结构体并调用同一实现，
删掉 DashStream 的那份拷贝。这样"两份同形实现"的漂移风险归零，也不动类继承关系。

## 三、P4 的验收口径（据此修订 §3/§4/§5）

- **§3 零容忍清单**：`seekLanding` / `mSeekLandingStage` **从"必须为 0"改为"必须只有一份实现"**
  （在两个 demuxer 类里仍会出现名字，不再是残留项）。其余 P4 项不变：
  `SeekInCache`（缓存内 seek 的旧路）、`DECODE_STALL_REBUILD_ROUNDS`（死线常量）按各自的性质处理
  （前者要按"有界前向缓冲单一规则"重做，后者直接删）。
- **行为验收**（用户构建后对照）：
  1. `[seekLanding]` 每轮最多 `arm/ENGAGE/RELEASE/ABANDON` 各一条（既有性质，不新增日志）；
  2. seek 后的解码前推距离 = 关键帧间隔量级，而不是分片长度量级（这是它存在的理由，
     删掉就会退化）；
  3. 不出现 `activeQ` 上千、两轴差 > 1s（§4 里 P4 的既有标记，属"有界前向缓冲"范围）。
- **代码验收**：`seekLanding*` 的实现只有一处（grep 应只有一个文件的函数体 + 两个 demuxer 的调用点）。

## 四、P4 还要处理的"有界前向缓冲单一规则"

用户要求"有界前向缓冲单一规则"，现状里与之相关的读点（**P4 施工时逐个确认，不要凭本文改动**）：
- `SuperMediaPlayer` 的 read-ahead 门与 `maxBufferDuration` 相关判据（P1 已把切档豁免改过一次，见 §8.4）；
- `DoCheckBufferPass()`（`SuperMediaPlayer.cpp` 约 `:2063` 起）与缓冲位置发布；
- `SeekInCache`（缓存内 seek 的旧路径，§3 列在残留清单里）；
- 直播路径的 UTC 迟到丢弃（`SuperMediaPlayer.cpp:2950-2975`，live 专用，**不属于**本项）。
