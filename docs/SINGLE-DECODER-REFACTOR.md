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

**保留（不得误删）**：`NotifyVideoQualitySwitch(...)` 生命周期、`isVideoDecoderMetaMatched()`、
`rebuildVideoDecoder()`、`CreateVideoDecoder()`、`FlushVideoPath()`、`mCurrentVideoIndex`、
ABR 接口与 `IsStreamSwitchInFlight()`、JNI/Java 接口签名、音频 keep-alive（写静音保活）。

## 四、分阶段与验收标记

| 阶段 | 内容 | 构建后应看到 / 不应看到 |
|---|---|---|
| **P0** | `Discontinuity` + `generation`（先只写入、不改行为）；本验收单 | 日志新增代际字段；行为不变 |
| **P1** | renderer 单一过滤 + 位置单调上报 | 每次 seek 都有 `seek landing frame accepted … offsetFromTarget=±(≤1 帧)`；不出现"落点窗口提前关闭"；位置不回退 |
| **P2** | 音频基准重设 + 音视频同一目标点 | 无 `audio silence starts (reason=1/4)` 长窗口、无 `keep-alive: reached the resource cap`；`afterSeekMs(音频) ≤ 200` |
| **P3** | 单解码器 + 关键帧边界；删除全部 pending/占位面/死线 | `[switch] state=decoderSwitch`、`pendingPktQ/pendingFrameQ`、`pending preroll`、`placeholder surface handover`、`quality switch timed out`、`wait a key frame` 风暴、两块 4K 实例 `-1010`、`video decoder accepts no input … rebuilding it once` 全部消失；切档只有 `status=0 → status=1` |
| **P4** | demuxer 收口 + 缓冲单一规则 | `[seekLanding]` 行消失；不出现 `activeQ` 上千、两轴差 >1s |
| **P5** | 清零残留 + 全量验收 | `E [0.9]`、`onAudioException -1003/-1004`、`PlayerBase baseTimeout/baseStop` 连锁、两轴差 >1s 的 `drop frame` 全部消失 |

## 五、验收命令

残留归零（期望输出 `0`）：

```powershell
$pats = 'mPendingVideo','mRetiredVideo','mWillChangedVideoStreamIndex','mQualitySwitch','mSwitchReArm',
'mPausedSwitch','mSwitchStartedWhilePaused','PendingVideoDecoder','PendingVideoPacket','PendingVideoFrame',
'attachPendingVideoCodecParams','b2Placeholder','b2SurfaceOutput','b2RealSurface','mSeekRenderGateUs',
'mSeekPositionFloorUs','mSeekLandingFloorOwnerUs','mSeekFirstDecodableFrameShown','mSeekLandingFrameAccepted',
'mSeekAudioFloorUs','mSeekAudioContinuityUs','mSeekAudioReposition','mSeekAnchorPending','mSeekClockAnchored',
'mSeekVideoAnchorDone','mSeekExactLandingByBudget','mAudioClockReanchorPending','holdAudioForSeek','seekLanding',
'mSeekLandingStage','QUALITY_SWITCH_TOTAL_TIMEOUT_MS','PENDING_VIDEO_STALL_CHECKS_MAX','PENDING_PREROLL_WAIT_MAX_MS',
'SeekInCache'
$files = Get-ChildItem -Recurse -Include *.h,*.cpp,*.mm -File mediaPlayer,framework,platform |
         Where-Object { $_.Name -notlike '*.TMP' }
($files | Select-String -Pattern $pats -SimpleMatch).Count
```

日志验收（用户构建后）：把上面 P1~P5 的"应看到/不应看到"逐条对照；任何一条不满足即视为该阶段未通过。

**基线实测（重构开始前，本命令输出）**：**1234 行 / 26 个文件**，分布高度集中：

```
SuperMediaPlayer.cpp 812    SuperMediaPlayer.h 91     DashStream.cpp 82     HLSStream.cpp 81
SMPMessageControllerListener.cpp 53   SMPAVDeviceManager.cpp 35
DashStream.h 14             HLSStream.h 14            其余 ~52 行散在 SegmentList / 其它 demuxer 与 mediaPlayer 文件
```

验收标准：重构完成后同一命令输出 **0**；任何非零数字都视为"双解码器/闩群残留未清完"。

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

### 8.1 新增成员（追加在类成员列表末尾，紧接 `mDiscontinuity`）

```cpp
    /* renderer 单一过滤（P1）：只由"包含目标的帧已上屏"或下一次 seek/Reset/Prepare 结束。 */
    bool    mDiscontinuityFilterActive{false};
    bool    mDiscontinuityFirstShown{false};
    /* 位置上报基准（P1）：position = targetUs + (now - baseMs)，数学上单调。 */
    int64_t mDiscontinuityBaseMs{0};
```

`mDiscontinuityFilterActive` / `mDiscontinuityFirstShown` 由内核工作线程读写；
`mDiscontinuityBaseMs` 由 API 线程写、工作线程读 ⇒ 用 `std::atomic<int64_t>`，或者干脆
把它们一起放进 `Discontinuity` 结构体（结构体成员是 public，clang 的
`-Wunused-private-field` 不会对一时还没被读的字段告警，这是更省事、告警风险最低的写法）。

### 8.2 要删的 6 个成员（`SuperMediaPlayer.h`）

`mSeekRenderGateUs`(`:1269`)、`mSeekLandingFrameAccepted`(`:1270`)、
`mSeekLandingFloorOwnerUs`(`:1633`)、`mSeekFirstDecodableFrameShown`(`:1573`)、
`mSeekExactLandingByBudget`(`:1363`)、`mSeekPositionFloorUs`(`:1126`)。
**保留** `mSeekExactLanding`(`:1272`，用户选项，不在本次删除范围)。

### 8.3 renderer 单一过滤（替换 `RenderVideo()` 的整块落点逻辑）

删掉 `SuperMediaPlayer.cpp:8011-8358`（从"落点帧即上屏（K1）"注释块起，到
`} else if (mSeekExactLanding) { render = false; }` 那段结束），以单一规则取代：

```cpp
    /*
     * renderer 单一过滤：seek / 换档 flush 之后第一帧立即出画；此后
     * [timePosition, timePosition + 帧长) 完全落在 targetUs 之前的帧全部丢弃；
     * 一旦出现"包含目标"（或第一帧就晚于目标）的帧，强制上屏并终止本次过滤。
     * 终止**只**由这一事件或下一次 seek / Reset / Prepare 触发 ——
     * SeekEnd / ResetSeekStatus 一律不得关掉它（那是分片源"永远差一个落点前缀"的根因）。
     */
    if (mDiscontinuityFilterActive) {
        int64_t frameTimePos = videoFrame->getInfo().timePosition;
        if (frameTimePos < 0) {
            frameTimePos = videoFrame->getInfo().pts;   /* 容器不填 timePosition 的退回 */
        }
        const int64_t targetUs = mDiscontinuity.targetUs;
        int64_t frameDurUs = videoFrame->getInfo().duration;
        if (frameDurUs <= 0) {
            const int fps = (mCurrentVideoMeta != nullptr)
                            ? std::max(1, (int) (mCurrentVideoMeta->operator Stream_meta *()->avg_fps)) : 25;
            frameDurUs = 1000000 / fps;
        }
        if (!mSeekDecodeStartIsKey) {
            /* 解码不是从关键帧起步 ⇒ 这一帧缺参考帧、是脏帧：不上屏（与 seek 无关的保护，保留）。 */
            render = false;
        } else if (!mDiscontinuityFirstShown) {
            /* 第一帧：无条件出画（不等时钟、不丢）。 */
            mDiscontinuityFirstShown = true;
            force_render = true;
            render = true;
            AF_LOGI("seek first frame shown: pts=%lld target=%lld afterSeekMs=%lld\n", ...);
        } else if (targetUs != INT64_MIN && frameTimePos >= 0 && frameTimePos + frameDurUs <= targetUs) {
            render = false;   /* 完全在目标之前 ⇒ 继续解 */
        } else {
            force_render = true;
            render = true;    /* 包含目标（或第一帧就晚于目标）⇒ 强制上屏 */
        }
        if (mSeekDecodeStartIsKey && targetUs != INT64_MIN && frameTimePos >= 0 &&
            frameTimePos + frameDurUs > targetUs) {
            if (render) {
                AF_LOGW("seek landing frame accepted: pts=%lld, offsetFromTarget=%+lld ms, afterSeekMs=%lld\n", ...);
                setVideoDecodeBoost(false);
            }
            mDiscontinuityFilterActive = false;   /* 唯一的终止点（除下一次 seek / Reset） */
            mVideoPtsRevert = false;
            mAudioPtsRevert = false;
            mPtsRevertWaitLogged = 0;
        }
    }
```

`beginDiscontinuity()`（P0 已有）改为同时置 `mDiscontinuityFilterActive = true`、
清 `mDiscontinuityFirstShown`、写 `mDiscontinuityBaseMs = af_getsteady_ms()`；
`beginDiscontinuity(INT64_MIN)`（Reset 路径）置 `mDiscontinuityFilterActive = false`（Reset 结束过滤）。

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

