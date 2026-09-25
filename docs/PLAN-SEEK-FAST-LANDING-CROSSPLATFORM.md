# 方案：seek 首帧"落点即上屏" + 音频时间锚/缓冲（内核通用，各端只打补丁）

> 修订说明：上一版拿"某个平台播放器的行为"当设计依据，这是错的，本版全部删除。
> 本版只按**通用不变量**论证内核改动；平台特有的东西一律不进内核，只在
> **各端壳 / 各后端实现**里打补丁。本文不含块注释起止符。

---

## 0. 归属原则：什么进内核，什么进各端/各后端补丁

内核只做**通用语义**，判据是"换任何平台、任何后端，这条结论都成立"：

| 通用不变量 | 含义 |
|---|---|
| I1 单一时间锚 | 一次 seek 只允许**一个**权威锚点事件决定 A/V 的时间基准；不得由 A/V 各自异步改主时钟 |
| I2 位置单调 | 对外上报的位置不得回退；但这只管**上报**，不得连带约束渲染 |
| I3 落点即上屏 | 解码从参考帧起，第一张可解码帧就该上屏；"必须精确到目标帧"是可选策略，不是默认 |
| I4 音频缓冲下限 | 起播 / flush 之后手里必须握有 ≥X ms 的已解码音频；这个下限是内核职责，不能用"平台最小缓冲"顶替 |
| I5 修正渐进 | 时钟修正不得阶跃；要么限幅内插，要么同一时刻通知视频路 |
| I6 零平台依赖 | 内核只认 option 与抽象接口 `IAudioRender` / `IVideoRender` / `IDecoder`；平台怪癖在各后端实现里兜住 |

| 待修问题 | 归属 | 理由 |
|---|---|---|
| seek 渲染地板身兼两职（见 1.1） | **内核** `SuperMediaPlayer` | 纯语义 |
| 一次 seek 出现 3 个主时钟写者（见 2.3） | **内核** | 纯语义，违反 I1 |
| 音频已解码队列只留 2 帧（见 2.1） | **内核** `SuperMediaPlayer.cpp:3927` | 违反 I4 |
| 时钟累积到 100ms 才一次性并入（见 2.2） | **内核** | 违反 I5 |
| 渲染队列 flush 后复位为 2 帧 | 策略在**内核**，计数在**后端**（`mMaxQueSize` 在后端类） | 跨层 |
| 设备缓冲取平台最小值 | **各后端**（Android/OHOS/Apple 各自实现） | 平台 API |
| 写设备返回值未检查（短写丢样） | **各后端** | 平台 API |
| 播放位置在 flush 后不归零 | **Android 后端**（已有 workaround） | 平台怪癖，I6 的正确落点 |
| **seek 在途的墙钟终态**（日志里的 `PlayerBase` + `[HSM]` + `baseTimeout()`，545ms 即 `stop()`） | **我们自己写的状态机**：**必须删除**（W0）。**不是**任何"外层预编译/平台约束" | 违反 I1/I3：终态必须由事件产生 |
| 仓库注释里"外层预编译 PlayerBase 的 HSM"这句陈述 | **错误陈述，要改**：它把我们的设计错误写成了平台约束，会误导后续每一次排查 | 见 3.2 |
| Qt 端 6s seek 看门狗、UI 位置地板 | **Qt 端**：**必须删除**（不是保留、不是调大），见第 3 节看门狗清零清单 | 红线：禁止看门狗 |
| Apple 优先走 `AppleAVPlayer` | **Apple 端** | 端侧 |
| HarmonyOS 欠载补静音 | **OHOS 后端** | 平台 API |

同一份内核被 6 个壳编译（只用于说明"内核改一处、各端都生效"）：
`platform/QtPlayer/CMakeLists.txt:235`、`platform/JetpackComposePlayer/cicadaplayer/src/main/cpp/CMakeLists.txt:58`、
`platform/Android/source/premierlibrary/CMakeLists.txt:28`、`platform/HarmonyOS/entry/src/main/cpp/CMakeLists.txt:27`、
`platform/Apple/source/CMakeLists.txt:40`、`cmdline/CMakeLists.txt:66`；
后端按 `framework/{Android,iOS,macOSX,windows,Linux,HarmonyOS,Emscripten}.cmake` 分叉。

各端 seek 入口（**只用于定位该端要打/要验什么，不作为内核设计依据**）：
`platform/QtPlayer/src/CicadaPlayerItem.cpp:2518`、
`platform/JetpackComposePlayer/.../player/CicadaPlayerController.kt:220-226`、
`platform/Android/source/paasApp/.../view/CicadaVodPlayerView.java:217,1224,2128`、
`platform/HarmonyOS/entry/src/main/cpp/napi_player.cpp:504`、
`platform/Apple/source/CicadaPlayer.mm:484-498`、
`cmdline/cicadaEventListener.cpp:38,42`；它们最后都落到同一个 `SuperMediaPlayer::SeekTo`（`:580`）。

---

## 1. 4K seek 慢"卡半天"（内核根因，违反 I2 与 I3 的混用）

### 1.1 根因：一条地板身兼两职，且没有"非精确"出口

`SeekTo()` 在 `:595` **无条件**设位置地板；清它的地方只有 `:5419` / `:5421` / `:7507`，
**没有任何一处因为 `bAccurate == false` 而清**。渲染侧 `:5413-5426` 又用**同一条**地板挡帧：

```cpp
if (render && mSeekPositionFloorUs != INT64_MIN) {
    const int64_t frameTimePos = videoFrame->getInfo().timePosition;
    if (frameTimePos >= 0) {
        if (frameTimePos + SEEK_TARGET_DROP_AHEAD_US >= mSeekPositionFloorUs) {
            mSeekPositionFloorUs = INT64_MIN;
        } else if (frameTimePos + SEEK_FLOOR_GIVEUP_US < mSeekPositionFloorUs) {
            mSeekPositionFloorUs = INT64_MIN;
        } else {
            render = false;      // 目标点之前的帧一律不上屏
        }
    }
}
```

⇒ 从"参考帧 → 目标点"整段 GOP 前缀必须**解完并丢掉**，才可能出现第一帧。
GOP 长度：同文件 `:149-150` 记录 DASH 片内 IDR 间隔 2~4s，另有片源为分片边界
（`platform/QtPlayer/docs/PLAN-QUALITY-SWITCH-FIX.md:71` 记 8.3s）→ 30fps 下 60~250 帧。
**4K 的像素是 1080p 的 4 倍**，这一项被放大 4 倍：慢的根源是"被要求解码的工作量"，
不是设备算力不足。

副产物：`mSeekFlag` 只在"有帧真的上屏"时清（`:3130`），所以首帧出不来 →
`renderer joining window started (seek finished)`（`:3000` / `:3137`）迟发 → 各端 SeekEnd 迟发。

### 1.2 修法（内核）

- 新增"渲染闸门"与"落点已接受"两个成员。按本仓库约定 **追加在 `SuperMediaPlayer.h` 成员列表末尾**
  （见 `.h:1134`、`.h:1144-1151`：中间插入会移动偏移、破坏增量构建）。
- `SeekTo()`：置闸门、复位"落点已接受"。
- `RenderVideo()`：**位置地板语义一个字节不改**（它只服务 `getCurrentPosition()`，`:1048-1049`）；
  渲染改用闸门，且本次 seek 的**第一帧直接接受并上屏**，同时把主时钟重锚到该帧 PTS；
  是否要求"必须精确到目标帧"由新 option `seekExactLanding` 决定，默认不要求。

效果：首帧 ≈ 一个参考帧的解码时间（4K 上是几十 ms 量级，而非 60~250 帧）；SeekEnd 提前。
代价：落点最多早一个 GOP（不丢内容，最多重放刚看过的 ≤2s），进度条由位置地板钉住不回退。

### 1.3 二期（仍在内核）

落点取**更靠近目标的那一个**参考帧（前后都算，用现成的
`mBufferController->GetKeyTimePositionBefore(...)`，用法见 `SMPMessageControllerListener.cpp:700`），
避免落到刚看过的内容上。

---

## 2. 音频"播放本身就感觉有问题"（内核根因 + 后端补丁）

### 2.1 缓冲下限太低（违反 I4）——起播与 seek 后最容易听出来

| 环节 | 现状 | 证据 |
|---|---|---|
| 内核已解码 PCM 队列 | **只保留 2 帧** | `SuperMediaPlayer.cpp:3927` |
| 后端渲染队列深度 | 初始 **2**，且**每次 flush 都复位成 2** | `framework/render/audio/Android/AudioTrackRender.h:116`；`.cpp:275` |
| 自适应增长 | **只在写线程空转时**每 20ms +1，上限 16 | `.cpp:369-374`、`.cpp:22-23,30` |
| 设备缓冲 | 取平台最小缓冲 | `.cpp:145-165` |

⇒ 起播 / **每次 seek 之后**，手里只有约 `2 帧 × ~21ms + 最小设备缓冲 ~20~40ms ≈ 60~80ms`。
4K 解码突发、网络抖动、写线程被抢占，都能把它吃干 → 欠载。
各后端对欠载的表现不同（OHOS 直接补静音：`OhosAudioRender.cpp:201-203`；Android 会把设备饿着）。

修法（内核）：把"2 帧"改成**时间下限**（"至少 X ms 的已解码音频在手"，X 取 100~200ms 量级），
并让后端渲染队列的**初始深度**由同一时间下限推导，而不是写死 2；`flush` 后复位到该下限而不是最低。

### 2.2 时钟修正是一次 >100ms 的阶跃（违反 I5）

证据 `SuperMediaPlayer.cpp:5177-5196`：

```cpp
int64_t offset = pts - (mPlayedAudioPts + mLastAudioFrameDuration);
if (llabs(offset)) {
    mAudioTime.deltaTimeTmp += offset;
    mPlayedAudioPts += offset;
}
if (llabs(mAudioTime.deltaTimeTmp) > 100000) {          // 100ms
    mAudioTime.deltaTime += mAudioTime.deltaTimeTmp;    // 一次性并入 = 阶跃
    mAudioTime.deltaTimeTmp = 0;
}
```

⇒ 稳态允许 A/V 差到 ~100ms 才纠，纠的时候是一次跳变（主时钟一跳，视频路丢帧/重排帧，
画面顿一下）。修法：**限幅内插**（每个音频帧最多并入一小步），或并入的同时通知视频路平滑跟随。

### 2.3 一次 seek 出现 3 个主时钟写者（违反 I1）——seek 后音画错位/跳变的直接原因

| 写者 | 位置 | 写入值 |
|---|---|---|
| ① seek 一发起 | `SMPMessageControllerListener.cpp:565`、`:735` | 用户目标点 |
| ② seek 完成时的"发现超前再拉回" | `SuperMediaPlayer.cpp:3119-3128` | **删除项 W13**：补救式写时钟，K2 之后不需要 |
| ③ 音频首帧重锚 | `SuperMediaPlayer.cpp:5162-5168`（`setTime(pts)`） | **第一张 seek 后音频帧的 PTS** |

③ 之前音频时钟整体不可用（`:6854-6856` 返回 `INT64_MIN`）。
⇒ 时钟被写 2~3 次，每次都是阶跃；若音频段起点与视频参考帧起点不同（分片流常见），
③ 会把基准从"目标点"改到"音频实际起点"，画面跟着跳。视频侧还有自己的落点问题（1.1）。

修法（内核，I1 与 I3 合用）：**seek 后只锚一次** —— 取"本次 seek 的第一张视频帧 / 第一张音频帧
中先到的那个"作为唯一落点，A/V 都锚到同一点；**②③ 两处 setTime 全部删除**（不是"保留为安全网"：
留任何一处，就等于保留一个"事后纠正"的补救路径，跟看门狗是同一类东西）。

### 2.4 写设备不检查返回值（后端补丁）

`framework/render/audio/Android/AudioTrackRender.cpp:439-443`：

```cpp
if (audio_track && method_write) {
    handle->SetByteArrayRegion(...);
    handle->CallIntMethod(audio_track, method_write, jbuffer, 0, len);   // 返回值未使用
    mSendSimples += audioInfo->nb_samples;                               // 无条件按全量记账
}
```

⇒ flush / pause 竞态下若发生**短写**，会静默少送 PCM，而 `mSendSimples` 仍按全量推进：
"时钟认为已播出的"与"实际发声的"不再一致 → A/V 缓慢漂移、偶发咔哒。
各后端统一约定：**短写必须补齐或回退记账**（不进内核）。

### 2.5 另两处（内核/后端边界）

- 渲染队列溢出时**直接删帧**：`AudioTrackRender.cpp:382-390` → 静默丢音频。
- 长播后播放位置计数：`AudioTrackRender.cpp:396-411`（`OVERFLOW_SAMPLES = 0x7F000000`，48k 下约 12.4 小时），
  且注释自认"部分设备 flush 后位置不归零"（`:277-280` 的 `mAudioFlushPosition` 就是为它打的补丁）——
  这是"平台怪癖 → 后端补丁"的样例。

### 2.6 症状 → 原因对照（请指认是哪一种，好把范围收到 1~2 条）

| 现象 | 最可能的原因 |
|---|---|
| 起播头几百毫秒不稳、偶发咔哒/爆音 | 2.1 缓冲下限 + 2.4 短写 |
| 偶尔断一下，尤其 4K / 高码率时 | 2.1（4K 解码突发把音频写线程挤掉） |
| 音画越播越偏 | 2.2 阶跃修正 + 2.4 记账不符 + 2.5 位置计数 |
| seek 后声音晚出 / 音画错位 / 跳一下 | 2.3 三个时钟写者 + 2.1（seek 后缓冲被压到最低） |
| 倍速下变调或节奏不对 | **待查**：`IAudioRender::setSpeed` 与各后端重采样/播放参数组合尚未核对 |

---

## 3. 看门狗清零清单（本方案红线：**禁止看门狗 / 周期动作**）

**更正**：上一版把 Qt 的 seek 看门狗写成"保留 / 可下调"，违反红线，本版更正为**删除**。两条原则：

1. **终态必须由内核事件保证**：seek 的终态（SeekEnd）只能由"落点帧上屏"这一事件产生；
   端侧不得用超时去猜"大概完事了"。
2. **超时只允许出现在纯观测**（打日志、打线程栈），**不允许参与状态迁移**（清在途状态、放锁、发终态、重建）。
   凡是"等 N 毫秒还没发生就动手"的，都是在掩盖根因。

### 3.1 必须删除（它们存在的理由就是 1.1 / 2.3 的根因）

**第 0 项 W0 在最前面、见 3.2**（seek 在途的墙钟终态），下面 W1~W14 都是它的下游补偿。

| # | 位置 | 现在干什么 | 为什么能删 |
|---|---|---|---|
| W1 | `SuperMediaPlayer.cpp:142` `SEEK_INPUT_STARVED_REBUILD_MS=900` | seek 期间输入饿死 900ms → 重建解码器 | K1 之后首帧 = 一个参考帧，不再有"等前缀"的长窗口 |
| W2 | `SuperMediaPlayer.cpp:127` `SEEK_NO_FRAME_REBUILD_MS=3000` | seek 期间 3s 无帧 → 重建解码器 | 同上；这也是 4K 下 250MB 缓冲池重配的来源 |
| W3 | `SuperMediaPlayer.cpp:112` `SEEK_CATCH_AUDIO_UNBLOCK_MS=400` | 400ms 后强行解封音频 | K2 之后音频与"视频出没出帧"完全解耦，无需解封 |
| W4 | `SuperMediaPlayer.cpp:74` `VIDEO_RECOVER_COOLDOWN_MS=30000` 与 `:88` `VIDEO_RECOVER_STUCK_SEEK_MS=15000` | 视频路"停滞多久算死"→ 追帧 / 重建 | 判据改成事件（包队列有没有被消费、有没有帧上屏），不再需要"停滞多久" |
| W5 | `SuperMediaPlayer.cpp:204` `JOINING_DROP_LATE_WINDOW_MS=3000` | seek 后 3s 内丢迟到帧 | 改成事件驱动：只丢"确实早于落点"的帧，由对齐事件结束，不用时间窗 |
| W6 | `SuperMediaPlayer.cpp:173` `QUALITY_SWITCH_DEADLINE_MS=500`、`:157` `QUALITY_SWITCH_PREROLL_DEADLINE_MS=6000`、`abr/AbrBufferAlgoStrategy.cpp:148` `ABR_SWITCH_WATCHDOG_MS=20000` | 切档"多久没收敛就判失败" | 仓库文档记录它们被一再调大（8s→12s→20s、10s→18s），是"用超时掩盖根因"的典型；状态机应按事件收敛 |
| W7 | `platform/QtPlayer/src/CicadaPlayerItem.cpp:655-658`、`:2517`、`:2524`、`:2552-2576` `m_seekWatchdog`(6s) | SeekEnd 不来就自己清在途状态 | 文档 RC-I 把它 2000→6000ms，正是"调看门狗"的反面教材；K1/K2 修完后 SeekEnd 必然及时 |
| W8 | `platform/QtPlayer/src/CicadaPlayerItem.cpp:3478-3487` `m_seekUiFloorMs` | 端侧第二套位置地板 | 与内核 `mSeekPositionFloorUs` 职责重复；通用职责放内核，端侧删 |
| W9 | `platform/QtPlayer/controls/ProgressRow.qml:319-323` Timer 1500ms | `pendingSeekMs` 超时清掉 | `:312-314` 已按位置事件收敛，超时是多余兜底 |
| W10 | `platform/JetpackComposePlayer/.../components/video/PlayerControls.kt:102,191-196` `SEEK_SETTLE_TIMEOUT_MS=2500` | 同上 | 同上；`:187-190` 已按位置事件收敛 |
| W11 | `platform/JetpackComposePlayer/.../VideoPlayerScreen.kt:147-155` `delay(700)` 方向兜底 | **UI 层**的"用时钟猜事件"（改的是显示模式，不是播放状态） | **本轮已删**（同类反模式）；若按"UI 不归本方案管"的口径，可还原这一处 —— 由用户定 |
| W12 | `platform/QtPlayer/DanmakuView.qml:709-741` 2 秒一次可见性巡检 | **UI 层**（弹幕层可见性） | **不动**（见 3.3 边界） |
| W13 | `mediaPlayer/SuperMediaPlayer.cpp:3119-3128` seek 完成时"时钟超前就拉回目标点" | 事后纠正主时钟 | 补救式写时钟 = 看门狗同类物；K2 之后只由落点事件锚一次 |
| W14 | `mediaPlayer/SuperMediaPlayer.cpp:5051-5062`（`mSeekCatchStartMs` 相关）与 `SuperMediaPlayer.h:1136-1137` | seek 等待的墙钟计时状态 | W0/W1/W3 删除后这两个时间戳没有任何消费者，一并删 |

### 3.2 W0：seek 在途的墙钟终态 —— 先说清它到底是什么（更正）

**更正**：本方案早先写过"Android 外层预编译 `PlayerBase` 的 545ms HSM，源码不在本仓库、删不掉"。
这句话是**照抄 `mediaPlayer/SuperMediaPlayer.h` 与 `.cpp` 里的注释**得出的，**没有独立核实，现予撤回**。

本次核查的事实：

| 核查项 | 结果 |
|---|---|
| 全仓库搜 `class PlayerBase` / `baseStart` / `basePause` / `baseStop` / `baseRelease` / `baseTimeout` / `[HSM]` / `piid` | **一处实现都没有**（`CicadaPlayerNext` 全树、`android`、`nplayer` 都搜过；命中的只有 `SuperMediaPlayer.h/.cpp` 的**注释**和 `NativePlayerBase.java` 这个名字相似的类，后者没有任何 HSM/超时方法） |
| 当前构建产物（`app-arm64-v8a-debug.apk`）里搜 `baseTimeout` 字符串 | **没有** |
| `:cicadaplayer` 模块的依赖（`cicadaplayer/build.gradle.kts`） | 只有一个预编译 `.so`（libffmpeg），**没有任何预编译播放器 AAR/JAR** |

结论：`[HSM] PlayerBase` + `basePause/baseStart/baseTimeout` 这套东西
**不是第三方预编译件、也不是平台行为**，而是**我们自己写出来的一个状态机**（在 2026-09-23 那份日志的运行版本里存在）。
它现在要么已被删掉、要么在某个我们自己的构建产物/分支里 —— 无论哪种，它都**必须进删除清单，而且是第 0 项**：

> **W0**：任何"seek 在途超过 N 毫秒就把播放器 `stop()` / 判死"的状态机。
> 主流播放器没有这种东西：seek 的终态是**事件**（seek 处理完成 / 首帧上屏 / 状态机回 READY），
> 时间只允许用在 **I/O 超时**与**诊断**上，绝不允许用来**终止播放**。
> 一个正常的 seek（数据还在下、解码还在跑）不应该有任何墙钟能把播放器停掉。

拆除顺序（不能反）：K1（落点即上屏，seek 终态由"落点帧上屏"事件产生）→ K2（单一时间锚）→
**再删 W0** → 然后才谈 W1~W14。删 W0 之后，W1/W2/W3（400ms 解封、900/3000ms 重建）也一并消失 ——
它们本来就是为躲 W0 而加的第二、第三层定时器。

另外：`mediaPlayer/SuperMediaPlayer.h`（`:444`、`:471`、`:1123`）与 `SuperMediaPlayer.cpp`（`:96`、`:100-108`、
`:119`、`:125`、`:151`、`:1690`、`:2923`、`:3286`、`:3299`、`:5001-5007`）
里那些"外层预编译 PlayerBase 的 HSM"注释必须改掉：它们把**我们的设计错误**描述成了**平台约束**，
是这次误判的直接来源（我自己就被它误导了两轮）。

### 3.3 边界：**不属于**本方案的（不要动）

**更正**：本节上一版把"控制栏 3 秒自动隐藏、150ms 重绘"也列进了清理范围，这是错的 ——
那是 **UI 交互**，跟播放器内核无关，跟"看门狗"也无关。已剔除。

先说清"看门狗"的定义，用它划边界：

> **看门狗 = 用时间推断"出问题了"，并据此改动播放状态或终止播放。**
> 只要不判断"出问题"、不改播放状态，就**不是**看门狗，**不属于本方案范围**。

| 物件 | 性质 | 处置 |
|---|---|---|
| `CicadaVideoPlayer.kt:235` 控制栏 3 秒自动隐藏 | **UI 交互**（用户不动就收起控制栏） | **不动**。与内核无关，不是看门狗 |
| `ProgressRow.qml:393-397` 150ms 重绘 Timer | **UI 重绘节流** | **不动** |
| `SuperMediaPlayer.cpp:418` `mTimerInterval=500` | 内核主循环节拍（驱动解码/渲染循环） | **不动**。它不是"判死"逻辑 |
| `platform/QtPlayer/main.cpp:500-683,859-871` 卡死看门狗（1 秒心跳 + 打线程栈） | **诊断工具**（只打日志，不改播放状态） | **不动**（诊断允许用时间；若你要求移除，单独提） |
| `platform/Android/.../util/NetWatchdog.java`、`platform/HarmonyOS/.../util/NetWatchdog.ets` | 网络变化**事件**监听（注册/反注册） | **不动**（事件源，不是超时兜底） |

反过来说，**必须删**的是**替内核兜底**的那一类 —— 典型是
`PlayerControls.kt` 原来的 `SEEK_SETTLE_TIMEOUT_MS`（UI 用超时猜"seek 完事了没有"，
掩盖的是内核 SeekEnd 事件缺失）。本轮已删；它属于"UI 替内核兜底"，不是 UI 交互。

### 3.4 删除顺序（不许先删代码后修根因）

先 K1（落点即上屏）→ 再 K2（单一时间锚）→ 再 K3（音频缓冲下限）→ **再一次性删 W1~W14**，
然后复跑"seek 后必出 SeekEnd、无停滞、无重建"的回归。
若删掉某条后出现回归，说明对应根因没修干净 —— **回去修根因，不许把看门狗加回来**。

---

## 4. 落地顺序（内核为主，各端只做验证与端侧补丁）

| 阶段 | 内容 | 归属 | 预期 |
|---|---|---|---|
| K1 | 渲染地板拆职责 + 落点帧即上屏（1.2） | 内核 | 首帧 ≈ 一个参考帧；SeekEnd 提前 |
| K2 | seek 只锚一次（2.3） | 内核 | seek 后不再音画跳变 |
| K3 | 音频缓冲下限按时间（2.1）：内核队列 + 后端初始深度 | 内核 + 各后端 | 起播 / seek 后不欠载 |
| K4 | 时钟限幅内插（2.2） | 内核 | 不再有 >100ms 阶跃 |
| K5 | 短写补齐 + 队列溢出不静默丢帧 | 各后端 | 不丢样、时钟与实际一致 |
| K6 | 删看门狗/定时兜底：**先 W0**，再 W1~W14 一次性清（第 3 节，含"时钟超前再拉回"W13 与墙钟计时状态 W14）；同时改掉 `SuperMediaPlayer.h/.cpp` 里"外层预编译 HSM"的错误注释 | 内核 + 各端 + 各后端 | 终态只由事件产生；超时只用于观测 |

统一指标：`PFR: seek posUs=…` → `renderer joining window started (seek finished)`
（`SuperMediaPlayer.cpp:663` / `:3000`）；音频侧看 `correct audio and master clock offset is X`（`:5192`）、
`audio clock re-anchored after seek at pts=`（`:5167`）、`PTS_REVERTING audio start`（`:5174`）。

## 5. 风险 / 不可静态论证

0. **红线**：本方案不得引入任何新看门狗/周期动作；已有的一律按第 3 节删除。
   删 W1~W14 的前提是 K1~K3 先落地并回归通过；顺序反了会把"掩盖"变成"暴露"。

1. 本会话无法编译（pwsh 起不来），以上为补丁级改动，**未编译、未运行**。
2. K1 改变"seek 落点"的可感知语义（最多早一个 GOP），可用 `seekExactLanding` 切回。
3. K3 提高缓冲下限会**增加起播音频延迟**（换抗抖动），取值必须可配置，且各端低延迟场景要能调。
4. K2 把"先到的那个落点"作为唯一锚，牵涉暂停态 seek / 切档在途 seek 的组合，必须先补回归用例再改。
5. 落点判据要用**新成员**，不要复用 `mPlayedVideoPts == INT64_MIN`：`FlushVideoPath`（`:6528`）
   与 `Reset()`（`:7471`）也会重置它，复用会在其它路径误触发。
6. 2.6 最后一行（倍速）未核对，需单独查证后再给方案。

---

## 6. 架构级根因：判据用"数据量"，而不是"有没有在产出"

### 6.1 现象
**视频卡住时，音频一直在放**（用户实测）。这不是两个独立的 bug，而是同一个设计漏洞的两面。

### 6.2 证据链
1. **主时钟的唯一来源是音频**：`mMasterClock.setReferenceClock(getAudioPlayTimeStampCB, this)`（`SuperMediaPlayer.cpp:5161`）。
   只要音频有数据，时钟就跟着真实时间走。
2. **只有"缓冲"状态能同时停时钟和音频**：`mMasterClock.pause(); mAVDeviceManager->pauseAudioRender(true);`
   （`:2202-2203`，进入缓冲时）。
3. **进入缓冲的唯一判据是"数据量"**：`DoCheckBufferPass()` 里 `cur_buffer_duration <= 0`（`:2180`），
   而 `getPlayerBufferDuration(false, false)`（`:6591-6665`）在 `preferAudio` 为假时取
   **音频/视频/字幕各自"数据时长"的 min**（`:6661`），其中视频那一项还把**解码器输入队列**折算进来
   （`:6636` `getInputPaddingSize() * 40ms`）。
4. ⇒ **"解码器卡住但队列里有货"在缓冲判据眼里是健康的**：min > 0 → 不进缓冲 → 时钟照走、音频照放；
   等视频帧终于出来时，相对主时钟已经"迟到"，被 `videoLateUs` / `dropLateVideoFrames` 丢掉 →
   **画面冻住、声音继续**，并且不会自愈。
5. 而 seek 期间恰好相反：`ClearPacket(BUFFER_TYPE_ALL)` 让 buffer = 0 → 进缓冲 → 暂停音频（`:2202-2203`）
   → AudioTrack 饿死 → 外层那个 HSM 在 545ms 把播放器停掉。
   **同一个判据，一头导致"音频照放"，另一头导致"被停播"。**

### 6.3 这条漏洞解释了"越修问题越多"
下面每一条都是为掩盖 6.2 而加的下游补丁，它们互相耦合，所以每加一条都牵动另一条：

| 补丁 | 位置 | 掩盖的是什么 |
|---|---|---|
| `SEEK_CATCH_AUDIO_UNBLOCK_MS=400` | `SuperMediaPlayer.cpp:112` | 音频被缓冲/精密等待掐死 |
| `SEEK_INPUT_STARVED_REBUILD_MS=900`、`SEEK_NO_FRAME_REBUILD_MS=3000` | `:142`、`:127` | 视频路不产出 |
| `VIDEO_RECOVER_COOLDOWN_MS=30000` | `:74` | 视频路不产出 |
| `JOINING_DROP_LATE_WINDOW_MS=3000` + 丢迟到帧 + `skipVideoForwardToMasterClock` | `:204` | 时钟与画面脱节 |
| `clean late audio data` | `:2076-2098` | seek 后音画起点不一致 |
| 切档死线 500 / 6000 / 20000ms | `:173`、`:157`、`abr/AbrBufferAlgoStrategy.cpp:148` | 切档状态机不收敛 |
| Qt 6s 看门狗、`SEEK_SETTLE_TIMEOUT_MS=2500`、`delay(700)` | 各端 | 终态事件缺失 |
| W0（seek 墙钟终态） | 历史实现 | seek 不产出 |

### 6.4 目标模型（彻底解决要立的契约，而不是逐文件补）
1. **唯一时间所有者**：一个 `TimelineClock`，对外只有 `Play / Pause / Anchor(pts) / ReAnchor(pts)`；
   其它任何模块**不得**直接改时间（现在一次 seek 有 3 个写者，见 2.3）。
2. **每条路一条独立活性状态机**：`Idle → Feeding → Decoding → Rendering`，
   `Stalled` 的判据必须是**事件计数**（喂进解码器的包数 / 出来的帧数 / 上屏的帧数 在若干次主循环迭代内不变），
   **不得**用"停顿了多少毫秒"。
3. **总状态机**：`Playing ⇄ Buffering ⇄ Stalled ⇄ Seeking`；进入 `Buffering/Stalled`
   **必须同时停时钟与所有渲染（含音频）**；退出条件也必须是事件（该路恢复产出 + 缓冲达阈值）。
4. **A/V 收敛规则**：只允许"落后方追、超前方等"；禁止"音频无限超前 + 视频单方面丢帧"。
5. **seek 契约**：`Seek(target)` 是异步请求，终态是事件序列
   `Flushed → LandingFrameRendered → SeekComplete`；**没有任何墙钟可以终止播放**。
6. **位置只有一个权威**：现在内核（`mSeekPositionFloorUs`）+ Qt（`m_seekUiFloorMs`）+ Android（`pendingSeekMs`）
   三处各有一套地板，必须收敛到内核一处。

### 6.5 迁移（不建议整树重写，建议"新协调器 + 分阶段迁移"）
整树重写要同时动 6 个平台壳与全部编解码/渲染后端，且会丢掉这些年修过的坑，风险最高。推荐：

| 阶段 | 做什么 | 完成判据 |
|---|---|---|
| S0 | 新增"活性 / 状态 / 事件"骨架（新文件，旧逻辑并行跑，只观测不改行为） | 新骨架推断出的状态与现状日志一致 |
| S1 | seek 切新路径（K1 落点即上屏 + K2 单一时间锚） | `PFR: seek` → `seek finished` 只剩"数据 + 一个参考帧" |
| S2 | 缓冲/停顿判定切到新状态（替代 6.2 的 min-数据量判据） | **视频不产出时音频必须一起停**，且不再需要任何超时 |
| S3 | 删全部下游补丁（第 3 节 W0~W14） | 删完回归无退化 |
| S4 | 按职责拆文件：`TimelineClock` / `BufferPolicy` / `SeekController` / `VideoPath` / `AudioPath` / `QualitySwitch`；`SuperMediaPlayer.cpp`（现 7942 行）只留薄壳转发 | 每个新文件可单测 |

CI 载体：`cmdline` 播放器（`cmdline/example/syncPlayer.cpp:210,213`）——
把"seek 后首帧耗时"和"视频停顿时音频是否继续推进"做成断言，各平台壳只做端到端验收。

---

## 7. 参考实现的选择规矩，与"跨平台一致"的保证机制

### 7.1 先定规矩：内核是跨平台 C++，只允许引用跨平台实现
- **可以当依据**：`mpv`（C 核心，官方在 Windows / macOS / Linux / Android / BSD / Haiku 都有构建）、
  `ffplay`（FFmpeg 自带播放器，纯 C，随 FFmpeg 在各桌面平台发布）、VLC（C，跨平台）、
  以及我们自己的 `cmdline` / `QtPlayer` 实测数据。
- **不得当依据**：ExoPlayer / Media3（**Android 专用**：Java/Kotlin + MediaCodec）、AVPlayer（Apple 专用）、
  Android 系统 MediaPlayer。它们**最多**作为"同族交叉验证"，**不允许**参与内核契约的推导 ——
  因为内核要同时跑 PC（Qt/Windows/Linux/macOS）、Android、HarmonyOS、Apple。
- 所以" IINA 的主时钟是什么"要按 **mpv** 看：IINA 官网原话是
  "Powered by the open source media player mpv"（[iina.io](https://iina.io/)）。
  PotPlayer **闭源**，看不到实现，**不猜**。

### 7.2 已核实的事实（可点开核对）
| 事实 | 来源 |
|---|---|
| IINA 的播放核心 = mpv | [iina.io](https://iina.io/) 官网原话 |
| mpv 有显式的"缓冲暂停"状态：`paused-for-cache` 为真 = 正在等数据，缓冲够 `pauseWait` 秒才自动恢复 | mpv 属性语义（[pausedForCache](https://pub.dev/documentation/mpv_audio_kit/latest/mpv_audio_kit/PlayerState/pausedForCache.html)） |
| mpv 在 **Windows / macOS / Linux / Android / BSD / Haiku** 均有构建（跨平台，含 PC） | [mpv.io/installation](https://mpv.io/installation/) |
| ffplay 时钟结构 `Clock{pts, pts_drift, last_updated, speed, serial, paused, *queue_serial}`；`queue_serial` 用于"过期时钟检测" | [ffplay.c](https://ffmpeg.org/doxygen/trunk/ffplay_8c_source.html) 第 140-148 行 |
| ffplay 同步源三选一（audio / video / external），**默认音频为主时钟** | 同上 第 201-205 行、第 347 行 |
| ffplay 阈值：0.04 / 0.1 / 0.1 / 10.0 秒；音频修正上限 ±10%；A/V 差取 20 次滑动平均 | 同上 第 81-98 行 |
| ffplay 队列深度：音频 PCM **9 帧**、视频 **3 帧**；读线程门槛 25 帧 / 15MB | 同上 第 67-68、127-131 行 |
| ffplay 以 `max_frame_duration` 判定"时间戳跳变" | 同上 第 306 行注释 |

### 7.3 照搬的核心思路（对应第 6.4 节的不变量）
| 参考做法 | 对应不变量 | 我们现在违反在哪 |
|---|---|---|
| 时钟 = `pts + (now - last_updated) * speed + drift`，且**自带 paused 位**（暂停即冻结） | I1 单一时间锚 | `mMasterClock` 一次 seek 被 3 处 `setTime`（`SMPMessageControllerListener.cpp:565,735`、`SuperMediaPlayer.cpp:3126,5165`） |
| 同步源是**显式可选**的（默认音频） | I1 | 主时钟来源硬编码在 `:5161`，异常路径靠补丁 |
| **缓冲/停顿是一等状态**，进入即整体暂停（含音频），恢复由"数据够"事件触发 | I4 + 第 6.2 节 | 只有"总数据量 == 0"才进缓冲（`:2180`）→ 视频卡住时音频照放 |
| 偏差**小步渐进**修正（音频 ≤±10%，A/V 差取滑动平均） | I5 | `:5191-5196` 累积 >100ms 才**一次性并入**（阶跃） |
| 队列按帧数给足抗抖动余量（音频 9 帧） | I4 | 音频 PCM 只留 **2 帧**（`:3927`），后端队列初始 2（`AudioTrackRender.h:116`） |
| seek = **请求 + 序列号**（`seek_req`/`serial`/`queue_serial`），旧时钟自动作废 | 终态事件化 | 我们是**同步阻塞** seek，且终态靠超时兜底（**违规**，属 W0/W7 删除项） |

### 7.4 跨平台一致性的保证机制（回答"PC 端怎样保证"）
不是"希望它一致"，而是四层可验证的保证：

1. **语义层：策略只有一份代码。** 时钟 / 缓冲 / seek 策略只写在 `mediaPlayer/` + `framework/`（纯 C++11，
   无 JNI / Qt / N-API / Objective-C）。6 个平台壳编的是同一份（已核实的构建入口见第 0 节）。
   ⇒ PC（Qt / Windows / Linux / macOS）与移动端跑的是**同一段策略代码**，不存在"两端各写一套"。
2. **后端层：平台差异被接口吸收。** 设备相关只体现在 `IAudioRender` / `IVideoRender` / `IDecoder` 的实现里：
   音频后端 = `framework/render/audio/{Android/AudioTrackRender, OHOS/OhosAudioRender, Apple/AFAudioUnitRender,
   Apple/AFAudioQueueRender, SdlAFAudioRender2}`；平台分叉只出现在
   `framework/{windows,Linux,macOSX,Android,iOS,HarmonyOS,Emscripten}.cmake`。
   ⇒ PC 与移动的差别被限制在"设备读写"，**不进入策略层**。
3. **验收层：PC 先立断言，各端只做验收。** 把指标做成**平台无关的日志断言**，先在 PC 上跑通
   （`cmdline` 与 `QtPlayer` 本身就是 PC 端），再让各端壳端到端验收：
   ① `seek → 首帧` 耗时；② **视频不产出时音频必须一起停**；③ 位置单调；④ 不出现任何墙钟终态。
4. **矩阵回归 + 后端一致性用例。** 同一片源（本地 4K MP4 / DASH 4K 未缓存）× 每个平台 × 上面 4 条断言；
   后端另有一致性用例（同一段 PCM 依次过各音频后端，检查时序/欠载行为一致）。

一句话：**策略一份代码、差异收在接口里、断言先在 PC 立、各端壳只做验收** —— 这就是"PC 端怎样保证"的答案。

## 8. B3：seek 落点必须重建"视频原始 pts → 主时钟轴"的映射（2026-09-25 落地）

### 8.1 症状
seek 之后画面冻结约 6~12 秒才恢复，期间音频正常播；`mPlayedVideoPts` 先跳到落点之后的内容
位置，再被音频时钟拽回落点。

### 8.2 根因（不是输入回调，也不是读前量）
这条片源**视频包的原始 pts 轴**与全局 `timePosition` 轴差一个固定量。包出口是这么算的：

* HLS：`timePosition = pts + mStreamStartTimeMap[streamIndex].time2ptsDelta`（`HLSStream.cpp:1295,1315`）
* DASH：`timePosition = pts + mStreamStartTimeMap[streamIndex].time2ptsDelta`（`DashStream.cpp:880`）
* 本地 MP4（Qt 主用）：`timePosition = pts - mCtx->start_time`（`avFormatDemuxer.cpp:430`）⇒ 通常差 0

主时钟与音频走 `timePosition` 轴（seek 落点对齐里 `mAudioTime.startTime = landingUs`，
`SuperMediaPlayer.cpp:4709`），而视频的渲染节拍 / 丢弃 / 锚定用的是**帧的 pts**
（`RenderVideo` 的 `videoLateUs = masterPlayedTime - videoPts`，`:6651`；
`mPlayedVideoPts` 亦然）。两条轴之间的映射就是 `mActiveVideoPtsOffset`：

* 帧侧唯一消费者：`FillVideoFrame()` 里的 `pFrame->getInfo().pts += mActiveVideoPtsOffset`（`:4884`）
* 包侧唯一消费者：读前闸门把队首包 pts 折算到主时钟同一把尺子（`:2122`）

切档有预滚建立这条偏移（`mPendingVideoPtsOffset = frameTimePosition - framePts`，`:5055`），
而 `FlushVideoPath()` 为了重建时间轴必须在 seek 时把它清零（`:8006`，对 seek 本身是必需的）
—— **落地之后内核里没有任何地方把它建回来**。

于是落点关键帧的 `timePosition=40.0s`、而 seek 后第一张上屏的帧 `pts=52.135s`
⇒ `videoLateUs ≈ -12.1s`，命中"早于主时钟 10ms 以上一律不渲染"（`:6917`）直接 return
⇒ 帧全堆在 `mVideoFrameQue` 里出不去 ⇒ codec 的输出缓冲不回收、随即不再交付输入缓冲
（日志 `codec has had no input buffer for about 1 s` **出现在队首冻结之后** ⇒ 它是结果不是原因；
本轮日志里**没有** `criterion=no-input-callback-after-flush`）；读前闸门又按两把不同的尺子比较
（同一时刻报 `front 53636917` 比 `master 40020000` 超前 `13616 ms`，真实只有 `1.5s`）。
两次 seek 的算术各自独立吻合（12.135s / 6.4s），切档与 seek 的差别只是"谁建立这条偏移"。

### 8.3 修法（内核两处，都是既有偏移的复用）
第一处 `mediaPlayer/SuperMediaPlayer.cpp:4634-4696`，在既有的 seek 落点对齐块（`if (landingUs > 0)`）
里用**落点包自己带的两个值**建立偏移：

```cpp
if (pVideoPacket->getInfo().pts != INT64_MIN && pVideoPacket->getInfo().pts != landingUs) {
    mActiveVideoPtsOffset = landingUs - pVideoPacket->getInfo().pts;
    AF_LOGI("seek landing: this stream's raw pts axis is %lld ms away from the timePosition ...");
}
```

第二处 `mediaPlayer/SuperMediaPlayer.cpp:9009-9039`（`RestorePausedVideoFrame()`，暂停态 surface
重建后逐帧恢复暂停画面）：把"解码器记录的原始 pts"与"seek 入参要的位置"分开 ——
`setRenderGate(lastPts)` 继续喂原始值（门在解码器侧、比较的就是原始 pts，
`mediaCodecDecoder.cpp:627-646`），而 `SeekTo()` 用折算到主时钟轴的
`lastPts + mActiveVideoPtsOffset`。原来把原始 pts 当位置用，两轴不等时会 seek 到差一个偏移量的
地方（恢复出来的不是暂停的那一帧），并且新增一条 `PFR: restore start ... offset= seekToPos=` 日志。

两处共同的性质：

* `landingUs - pts` 正是这路流的 `time2ptsDelta`，与切档预滚的式子同源；复用既有成员、
  不新增任何状态、不动落点/目标语义（`mSeekPositionFloorUs`、`mSeekRenderGateUs`、
  `SEEK_EXACT_LANDING_BUDGET_US` 全不变），`getCurrentPosition()` 的位置地板照旧保证进度条不回退。
* 一次 seek 只跑一次（既有 `mSeekAudioAlignDone` 闩）；下一次切档提交由
  `mActiveVideoPtsOffset = mPendingVideoPtsOffset`（`:5930`）覆盖，下一次 seek/stop/换源由
  `FlushVideoPath()`（`:8006`）与 `Reset()`（`:9054`）清零 —— 生命周期与切档建立的偏移完全一致。
* **Qt 零影响**：（1）在共享内核的 seek 路径上，无平台宏；pts 与 timePosition 一致的片源
  （本地 MP4、多数 HLS）算出来是 0，这一句只在真的不一致时才置位 ⇒ 判据逐字不变；
  （2）`RestorePausedVideoFrame()` 依赖 `IDecoder::getLastRenderedVideoPts()`，Qt 等后端用
  默认实现返回 `INT64_MIN` ⇒ 在函数开头就 `return 0`，整段不可达。
* 回退：删掉这两处改动即可（或把 `mActiveVideoPtsOffset` 强制回 `INT64_MIN`）。

### 8.4 落地后要盯的三组数
① `seek landing: ... pts=%lld timePosition=%lld` 的差 = 闸门 `front - master` 的差；
② `seek anchor: ... pts=` 应≈落点（不再是落点 + 偏移量），位置回调不再跳一次；
③ `codec has had no input buffer` 应消失（它本来就是冻结的果，不是因）；
④ 暂停态旋转/恢复画面时 `PFR: restore start ... offset= seekToPos=`：seekToPos 应≈
   `lastPts + offset`（两轴不等时两者必须不同，否则说明这条修正没生效）。

### 8.5 已知残量（本轮**故意不动**，等证据）
1. **HLS 每段会重算 `time2ptsDelta`**（`HLSStream.cpp:1295` 在每个 seamlessPoint 重算）。
   本轮的偏移是落点一次性建立，若某片源跨段时该常量真的变了，会在段边界错一个"变化量"。
   扩展点：在 `DecodeVideoPacket` 里对 `seamlessPoint` 的包按"无切档在途"（`mPendingVideoStreamIndex < 0`）
   刷新一次 —— 属于新增状态/新判据，等出现证据再做。
2. **Java 侧 `no-input-callback-after-flush` 的立即自愈**（`MediaCodecDecoder.java` 现在只置
   `sAsyncBroken`，只对下一个解码器生效）：本轮日志没有该判据 ⇒ 未做。
   健康态 seek 日志若显示"队首正常推进、frameQ 为空、回调停摆是首因"，再按
   "判据命中 → 返回可识别码 → native 映射到既有错误驱动重建"落地。

## 9. B4：两条渲染路 + 按内容自动选（2026-09-25）

### 9.1 两条路各是什么

| | GL 路（**普通内容的默认**） | 隧道路（**HDR / Widevine 强制**） |
|---|---|---|
| codec 绑的 Surface | 内核自建 SurfaceTexture 的面（`GLRender::getSurface()` `GLRender.cpp:515-534` → `SuperMediaPlayer.cpp:8921` → `setUpDecoder`） | App 的 SurfaceView（`SuperMediaPlayer.cpp:8881` + `DECFLAG_DIRECT`） |
| 渲染器 | `GLRender` + `OESProgramContext`（`GLRender.cpp:392` → `updateFrame` → `:426 Present`） | `DummyVideoRender`（帧直接释放，画面由 codec 直出） |
| CPU 拷贝 | 0（buffer → EGLImage → OES 采样） | 0 |
| 代价 | +1 次 GPU 合成、约半帧~1 帧延迟 | 无 GPU 参与（最省电） |
| 等比/黑边/背景色/旋转/截图 | ✅ 内核做（`updateDrawRegion` / `glClearColor` / `glReadPixels`） | ❌ 只能靠 App 布局（`DummyVideoRender.h:37-47` 全空实现） |
| 后台 | codec 不受 App Surface 生命周期影响（只重建 EGL 窗口） | codec 输出面随 SurfaceView 销毁而失效（黑屏来源） |
| HDR / DRM | ❌ 不可能（元数据/secure buffer） | ✅ 唯一可行 |

选择点在 `SetUpVideoPath()`：`isHDRVideo(meta) ⇒ FLAG_DUMMY`、`isWideVine ⇒ FLAG_DUMMY`
（`SuperMediaPlayer.cpp:8703-8724`），其余内容由 App 的 `enableVideoTunnelRender` 决定
（Compose 侧 `TUNNEL_RENDER_ENABLED`，本轮改为 `false` ⇒ 普通内容走 GL）。

### 9.2 本轮改动（内核，全部对其它平台为空操作）

1. `SuperMediaPlayer.h` 末尾新增 `void *mActiveVideoSurface{nullptr};`：记住 active 解码器
   真正绑定的输出面（隧道 = App view；GL = 渲染器的 SurfaceTexture 面）。
2. `SuperMediaPlayer.cpp:8882-8886` / `:8933-8935`：`CreateVideoDecoder` 里清/写这个成员。
3. `SuperMediaPlayer.cpp:5159-5182`：占位 Surface 判据分路 —— 隧道仍是
   `FLAG_DUMMY && view`（逐字不变）；GL 分支要求"硬解 + 渲染器给过面"。旋转标志改为
   **继承 active 解码器的 flags**（GL 的 active 不带 `DECFLAG_OUT`，占位面也必须不带）。
4. `SuperMediaPlayer.cpp:5543-5552`：`b2RealSurface`（隧道 = `mSet->mView`，逐字不变；
   GL = `mActiveVideoSurface`）+ `b2SurfaceOutput`；B2' 门与诊断门改用它。
5. `SuperMediaPlayer.cpp:5587-5617`：B2' 重建时 GL 允许"硬解失败落软解"（与起播同规矩），
   隧道保持不落软解。
6. `SuperMediaPlayer.cpp:5662-5666`：B2 占位交接去掉 `FLAG_DUMMY` 前置，三处
   `setOutputSurface` 用 `b2RealSurface` ⇒ **GL 路的切档也走"占位面 + 无缝交接"**，
   不再退回"promote 一个绑在占位面上的解码器"（那会让画面永久停在旧帧）。
7. `SuperMediaPlayer.cpp:5202-5219`：**窄判据**的 HDR 拒绝 —— 判据是
   `!renderIsDummy && mActiveVideoSurface != nullptr`，即"渲染器自己占着解码器输出面"
   ＝**只有安卓 GLRender 这一条路**。理由见下。

**为什么对 Qt / macOS / Windows / Linux / iOS 为空操作（可核对）**：
全仓只有 `GLRender` 覆盖 `IVideoRender::getSurface()`（`GLRender.h:60`），而它内部是
`#ifdef __ANDROID__`、非安卓直接返回 nullptr；Qt 自己的 `CicadaVideoRender`、`SdlAFVideoRender`、
`AVFoundationVideoRender` 都用默认实现（`IVideoRender.h:180-183`）⇒ 它们的
`mActiveVideoSurface` **恒为 null** ⇒ 上面第 3/4/5/6/7 条的 GL 分支全部恒假，隧道/OHOS
分支逐字不变。Qt 的 HDR 走 `CicadaVideoRender::getFlags() == FLAG_HDR`（`CicadaVideoRender.h:121-124`），
不进入第 7 条。

### 9.3 App 侧（Compose，随本轮一起改）

* `CicadaPlayerController.kt`：`TUNNEL_RENDER_ENABLED = false`（并升为 `internal` 供布局层读），
  注释改为"两条路 + 按内容选"。
* `CicadaVideoPlayer.kt`：`videoSurfaceSize()` 按同一个开关分流 —— GL 路 **fillMaxSize**
  （等比/黑边由内核画），隧道路仍按视频比例定尺寸；`setScaleMode(SCALE_ASPECT_FIT)` 与
  `setVideoBackgroundColor` 保留（GL 路真正生效）。

### 9.4 回退

* 只想回到旧行为：`TUNNEL_RENDER_ENABLED = true`（普通内容也走隧道）。内核不用动。
* 想连内核也回退：把 9.2 的 1~7 逐条删回原样（第 3 条的判据恢复为
  `FLAG_DUMMY && mSet->mView != nullptr`；第 6 条恢复 `b2Tunnel && … && mSet->mView != nullptr`
  且三处 `setOutputSurface` 用 `mSet->mView.load()`）。

### 9.5 已知**未改**的机制（等实测日志判定，都是"卡顿"的候选）

1. **pending 帧在等提交期间无人释放**：内核侧持有上限 = 待提交 4 帧
   （`SuperMediaPlayer.cpp:38` `VIDEO_PICTURE_MAX_CACHE_SIZE 2` ×2）+ `ActiveDecoder`
   输出队列 10 帧（`ActiveDecoder.h:102`），而 surface 模式下 codec 能持有的输出槽位
   只有占位面 BufferQueue 的深度（通常 2~3）⇒ 队列一满，codec 连输入缓冲也拿不到
   （`mediaCodecDecoder.cpp:470-477` 只回 `-EAGAIN`、不报错、不自愈），而提交门又要求
   "帧追上播放位置"（`SuperMediaPlayer.cpp:5237-5240`）⇒ 两者互等，直到切档死线。
   **判据**：日志里 `pendingPktQ`/`pendingFrameQ` 长时间不变 + `codec has had no input
   buffer` + [switch] 关键量不动。
2. **占位面队列无人排空**：占位面是 Java 侧 `SurfaceTexture(0)` + 无监听
   （`MediaCodecDecoder.java:681-692`），画面帧一旦 `render=true` 释放进去就再也拿不回来；
   目前只靠"交接顺序"保证不出问题（交接成功后旧代帧整体作废）。
3. **preroll 起点可能远在播放位置之前/之后**（日志实测 `lead=9006 ms`）：提交门的
   参考量是**队首**帧的 `timePosition`，与 preroll 领先量直接相关；这是"正在切换…"
   停留时长的另一个来源。
4. **`GLRender::getSurface(false)` 是无超时阻塞**（`GLRender.cpp:520-526`，等
   `needCreateOutTexture`，而纹理在渲染线程 `renderActually:297-303` 创建）：EGL 初始化
   失败时会把播放线程挂死。走 GL 前应加超时 + 明确失败。
5. **Android 上 `GLRender::mInBackground` 无人维护**（`GLRender.cpp:40-45` 只有 iOS 设它，
   `renderActually:287` 依赖它）：后台策略目前靠"App `setSurface(null)` → EGL 窗口没了 →
   `renderActually:326-334` 丢帧" + 内核 `MSG_INTERNAL_VIDEO_HOLD_ON`
   （`SMPMessageControllerListener.cpp:974`）兜住，需要实测确认。

### 9.6 验收（先说清"看什么"）

装好后按顺序做：起播播放 → 播放中 seek ×3 → 暂停中 seek → 播放中切清晰度 ×3 →
暂停中切清晰度 → 切档中再 seek → 后台 5s → 回前台 → 全屏/退出全屏 → 4K（若有）。

日志断言（`adb logcat -v threadtime > x.log` 后搜）：
* `SetUpVideoPath tunnelRender=0 hw=1 renderFlags=0` —— 普通内容确实走了 GL；
  HDR/DRM 内容应为 `renderFlags=2`（FLAG_DUMMY）。
* `EGLContext CreateSurface` / `GLRender` 相关行出现 ⇒ GL 路真的在画。
* `dummy-surface placeholder handover (B2): … detach=0 attach=0`（GL 路也要出现这一行，
  说明切档走的是无缝交接而不是 promote 占位面）。
* `codec has had no input buffer`、`async input path looks dead`、`video path stalled`
  应当**不再出现**；`seek landing: … is -N ms away …` 出现且闸门 `normalized front` 与
  `master` 差 < 2s。
* 进度条单调（不回退、不跳 >1s）。

## 10. B5：真机日志（2026-09-25 11:01–11:03）暴露的 5 个问题与处置

这一轮日志（1283 行，Android + HiSilicon + DASH + 4K）证明 B3/B4 都生效了：
切档 5/5 成功（`placeholder handover … attach=0`、`attachToFirstFrameMs=229~240`），
seek 3/3 在 168~375 ms 内锚定出画，全日志**没有**任何停摆判据。随后处理日志里暴露的 5 项：

### 10.1 GL 的 shader E 日志（每次冷启动一条）
* 现象：`[GLRender_OESContext] compileShader mVertShader failed. ret = -1`（11:02:16.412）。
* 根因：`GLRender::getSurface()` 第一句就调 `getProgram()`，那会在**播放线程**上
  `new OESProgramContext + initProgram()`，而 EGL context 只由渲染线程
  `makeCurrent`（`onVsyncInner → VSyncOnInit`）⇒ 所有 GL 调用失败。真实创建由
  `needCreateOutTexture` 握手在渲染线程完成，所以**不是故障，是噪声 + 一次无用功**。
* 修法：`GLRender::getSurface()` 只在调用线程"向渲染线程要"，不再自行创建；新增
  `GLRender::mOutTextureReady` 闩（渲染线程建好后置位）；渲染线程创建失败时空指针
  守卫 + **2 s 超时**（旧写法是无超时 `wait`，GL 初始化失败会把播放线程挂死）；
  `VSyncOnDestroy` 随 `mPrograms.clear()` 复位。文件：`framework/render/video/glRender/GLRender.{h,cpp}`。
* Qt/其它平台：`getSurface()` 的实现体本来就在 `#ifdef __ANDROID__` 里，非安卓直接返回
  nullptr ⇒ 逐字不变。

### 10.2 切档耗时 0.86~1.41 s：**不改算法**（结论 + 证据）
* 拆分（同一份日志）：UI `quality switch started` → `pending preroll starts` 约 0.78 s →
  `committed` 约 0.27 s → `post-handover first frame` 0.24 s。
* 0.78 s 那段的性质：预滚起点行里 `master - ref = 0.776 s`，而要跨越的媒体长度
  `lead = 4.529 s` ⇒ **0.78 s 墙钟内交付 4.53 s 目标档媒体**，且该窗口内**一帧都没解**
  （`packetTimePos < ref` 的包在喂解码器之前就被跳过）⇒ 耗时在**网络/分片粒度**。
* 另一段（`OpenStream` + `Seek`）是同步等网络的，且发生在
  `mPendingVideoSwitchStartMs` 计时零点**之前**，旧日志里看不见 —— 本轮补了两条计时日志：
  `quality switch: target stream %d opened in %lld ms` / `… seeked to %lld us in %lld ms`
  （`SuperMediaPlayer::SwitchVideo`，只加日志、零行为改动）。
* 评估过并被否掉的 4 个"≤10 行改法"：① 把 ref 只用 `mPendingVideoSwitchTimePosition`
  （网络卡顿时选到落后时钟数秒的 IDR，几百帧全丢，4K 上退回 18.5 s 预滚形态）；
  ② 让播放态也降到"前一个 IDR"（把网络等待换成解码等待，4.5 s GOP ≈135 帧更慢且抢解码器）；
  ③ 调小 `PENDING_PREROLL_WAIT_MAX_MS`（DASH 上前一个 IDR 早被丢，回退分支不触发，无效）；
  ④ 抬 pending 队列软上限/burst（不改变下载量，收益≈0）。**因此判定：不动这段逻辑。**
* 真要缩短只能从数据面下手（唯一真实杠杆）：让 demuxer 支持"分片边下边解/字节范围，
  先交出 ref 之后那个 IDR 所在的部分" —— 属 play_list demuxer 的改动，另立项。

### 10.3 preroll 日志坐标轴
* 那行同时打了 raw pts 与位置轴的值却只写 `pts=/timePosition=/ref=/master=`，
  真机里 `pts=145979167 timePosition=140000000 ref=135470098 (lead=4529 ms)` 极易被误读。
* 现在明确标轴 `keyRawPts` / `keyTimePosition`+`ref`+`master`，`lead` 注明
  `= keyTimePosition - ref`，并新增 `prevKeyTimePosition`（参考点之前最近的关键帧；
  查不到时打印 `none`，不再打印 `INT64_MIN`）。

### 10.4 陈旧帧释放：`releaseOutputBuffer fail Error 0xfffffff3` + 真实 UAF 窗口
* 现象：seek（11:02:56.763，+26 ms）后一条 E。机制：`flush_decoder()` 里的平台 `flush()`
  收回了 codec 自持缓冲，而内核手里的 `AFMediaCodecFrame` 的 index 就此失效；这些帧要等
  渲染线程**下一次 VSync** 才析构（`GLRender::mInputQueue` 是异步清的）⇒ 拿失效 index
  去 release，必然失败。
* 更严重：释放回调原来是 `[this, framePts](…)`，而 `~mediaCodecDecoder` 会 `delete mDecoder`；
  已提交切换的 retired 解码器是**播放线程同步销毁**的，旧代帧却还在渲染器队列里
  ⇒ 回调在**已释放对象**上读成员 + 调 JNI = 真实 use-after-free。
* 修法（只动 Android 后端 `framework/codec/Android/mediaCodecDecoder.{h,cpp}`）：
  新增共享状态 `FrameReleaseState{mutex, decoder*, alive, atomic flushGen}`（`shared_ptr`
  成员）；帧的回调**按值持有它**并在锁内先校验 `!alive || decoder==nullptr || gen!=flushGen`
  ⇒ 直接短路（不进 Java、不碰任何成员）；`flush_decoder()` 与 `close_decoder()` 各
  `invalidateFrameReleases()` 一次（+1 代）；析构时**在锁内**置 `alive=false`、`decoder=nullptr`
  并 `delete mDecoder` ⇒ "销毁"与"释放"串行化，UAF 关闭。
* Qt/macOS/Windows/Linux/iOS：本文件是 Android 专用实现，接口/ABI 无变化（只多一个私有成员）
  ⇒ 逐字不变。回退点：删掉该结构与成员、恢复 lambda 捕获裸 `this`、去掉三处调用。

### 10.5 INT64_MIN 哨兵参与算术（环绕值）
* 现象：`TIMEPOS reSync time 30018965 to -9223372036824759809`、
  `drop frame,master played time is 9223372036854775570`、`[seekdiag] master=-9223372036824759729`。
* 根因链：有人给时钟 `set(INT64_MIN)` ⇒ `af_scalable_clock::get() = mSetTime + elapsed`
  返回"哨兵 + elapsed"的环绕值 ⇒ ① 它被当成真时间参与比较（差值可能**溢出**）；
  ② `SystemReferClock::GetTime()` 把它 **set 回主时钟**，主时钟就此被污染，起播连丢约 10 帧。
  音频侧的来源是 `getAudioPlayTimeStamp()` 里 `mAudioTime.startTime + deltaTime + aoutPos`，
  而 `mAudioTime.startTime` 当时还是哨兵值。
* 修法（全部纯状态判断，零时间阈值、零看门狗）：
  1. `framework/utils/af_clock.h` 新增 `af_clock_value_is_unset()`（真时间不可能落在
     INT64 极值 1 小时内）；
  2. `mediaPlayer/system_refer_clock.cpp`：参考值无效就不动时钟；本地值未建立时不做
     差值比较、直接采纳有效参考值；首次赋值不再打印环绕值；
  3. `mediaPlayer/SuperMediaPlayer.cpp`：`getAudioPlayTimeStamp()` 在 `startTime` 未建立时
     返回 `INT64_MIN`（源头修复）；`RenderVideo` 里主时钟未建立时**不做**可能溢出的减法、
     两处丢帧判定加护栏（起播头几帧不再被丢）；`[seekdiag] master=unset`、
     `drop frame,master played time is unset`。
* 正常播放/seek/切档路径：`af_clock_value_is_unset()` 恒为 false ⇒ 判定与今天逐字相同；
  Qt 共用这些文件 ⇒ 同样受益。

### 10.6 状态栏：全屏退出后文字/图标消失（App 层）
* 根因：`Theme.kt:78-79` 每次组合都设 `isAppearanceLightStatusBars = !darkTheme`，
  而播放页状态栏那块**永远是深色**（Inline 自刷 `background(Color.Black)`；全屏是
  `fillMaxSize` + AspectFit 黑边铺到状态栏底下）⇒ 浅色主题下退出全屏，图标被改回
  **深色**、落在黑底上 = 看不见。
* 修法：`VideoPlayerScreen.kt` 在"真的在放视频"这一支里用 `SideEffect`（每次组合都重声明
  `= false`；主题的 `SideEffect` 先注册先执行 ⇒ 本页后写者赢），删掉原来只在
  `displayMode` 变化时写的那行，`onDispose` 把 appearance 交还主题（`= !isDarkMode`）。

## 11. B6：切清晰度后音画恒定位移（内容错位）—— 根因与修法

### 11.1 根因（代码 + 日志算术双重确认）
`mediaCodecDecoder.cpp:692` 至今是 `pFrame->getInfo().timePosition = INT64_MIN;`（原 TODO：
"get the timePosition form input packet"）——**Android 硬解的输出帧不携带节目时间轴**。
于是 `FillPendingVideoFrame()` 里 `frameTimePosition < 0`：
* "切换窗丢帧"整段跳过（预滚前缀帧一张不丢）；
* 逐帧归一化整段跳过（`frame->getInfo().pts = frameTimePosition` 不执行）；
* 落到 `SuperMediaPlayer.cpp:5131` 的**备用偏移** `mPendingVideoPtsOffset = masterPts - framePts`。

这个备用偏移的语义是"把目标档的帧**重命名**到主时钟轴上"，而预滚起点是参考点**之后**的第一个
关键帧（本片源 IDR 间隔 5~11 s）⇒ 新档内容被整体**提前** `lead` 秒显示 = 恒定音画错位。
证据（2026-09-25 那份日志，5 次切换全部自洽）：`committed … offset=` 恰好等于
`master − prerollKeyRawPts`（例：136 506 061 − 145 979 167 ≈ −9 473 284 = 日志值），
而同一档的**节目轴差**是 `keyTimePosition − keyRawPts = 140.0 − 145.979 = −5.979 s`；
两者之差 3.494 s ≈ 该次 `lead`（参考点 135.47 s 与关键帧 140.0 s 的距离）。
Qt / macOS / iOS 的解码器**会**给 timePosition（`avcodecDecoder` 从 tag 取、Apple 侧直接
拷贝），所以只有 Android 出现这个错位 —— 与"Qt 正常、安卓错位"完全一致。

### 11.2 修法（三处内核改动，Qt 按能力探测保持逐字不变）
1. `SuperMediaPlayer.h` 末尾新增 `bool mActiveDecoderFramesCarryTimePosition{false};`
   —— `FillVideoFrame()` 里见到 `timePosition >= 0` 的帧就置真（粘性）。这是**能力探测**，
   不是平台分支：Android 恒为 false，Qt/macOS/iOS 很快为 true。
2. `DecodeVideoPacket()` 的预滚处：当该后端**不带** timePosition 时，按
   `mPendingVideoPtsOffset = packetTimePosition - packetPts` 记录这一档的恒定差
   （与 seek 落点 B3 同一个式子；带 timePosition 的后端不记录，避免两种来源打架）。
3. `FillPendingVideoFrame()`：帧缺 timePosition 时用 `framePts + mPendingVideoPtsOffset`
   把它**算出来**（只作局部变量，不写回帧字段 —— 渲染侧 K1 落点采纳读的是帧字段，
   不能连带改变它的行为）。于是"切换窗丢帧 / 归一化 / 提交门"在 Android 上重新生效。

配套：预滚参考点的下移（原来只有暂停态）扩展到**播放态**，条件就是
`!mActiveDecoderFramesCarryTimePosition`（`SuperMediaPlayer.cpp:4138-4162`）——
让 pending 解码器从参考点**之前**最近的关键帧解起、由切换窗丢掉前缀，提交时手上就有
贴着主时钟的帧（按构造对齐），代价是最多一个 GOP 的前缀解码。

### 11.3 验收断言（这一轮的关键）
* 新增日志 `pre-roll: reference lowered from … to … — this backend's output frames carry no
  timePosition` —— **只应在 Android 出现**；Qt 上不应出现。
* **不变式（最关键）**：`committed … offset=` 必须等于同一档预滚行的
  `keyTimePosition − keyRawPts`（例：`-5979167`），而不再是 `master − keyRawPts`（例：`-9473284`）。
* `post-handover first frame: pts=` 与同一时刻的 `master` 之差应在 ~0.3 s 内
  （旧日志是 250 ms 但那段是**错位的内容**；现在这个差值代表真实对齐）。
* 主观：切完清晰度音画同步（不再"视频比声音快半句台词"）。
* 代价：Android 上切档多出一段前缀解码（4K 目标档估计 0.3~2 s），期间旧档继续播。

## 12. B7：预滚必须从"参考点**之前**的关键帧"起解（2026-09-25 13:00 日志）

**现象**：自动切档（ABR）**失败**（`quality switch status=2 desc=target rendition did not reach
playback timeline`，8.2 s 后超时）；成功的那两次在提交后画面停 **3.36 s / 2.60 s**
（`attachToFirstFrameMs=3362/2598`），期间旧档帧已在 promote 时作废 ⇒ 画面冻、音频继续
⇒ 用户感知"音画不同步"。

**根因**：参考点 ref=1.018 s，而"参考点之后的第一个关键帧"在 10.0 s（该片源 IDR 就在 10 s
分片起点）⇒ 预滚帧全部领先时钟 `lead=8.98 s`。B6 把帧放到正确节目轴之后，提交门的
"未来帧"判据（`SuperMediaPlayer.cpp:5605` `pts > master + PENDING_FUTURE_TOLERANCE_US`）
把提交挡住，直到时钟爬到 10.0 s ⇒ 超过切档死线 ⇒ 失败；不做轴修正（旧行为）则走
"重命名到主时钟"⇒ 恒定错位。两条路都不对 —— 错在"从参考点**之后**的 IDR 起解"这个选择。

**修法**：预滚循环里，遇到参考点**之前**的关键帧就把参考点降到它并接受
（`SuperMediaPlayer.cpp:4209-4218`，纯状态判断、无计时器、接受后 `mPendingVideoPrerollDone`
置真不会反复降）。前缀（本例只有 1.0 s，被一个 IDR 间隔封顶）由切换窗/渲染器追赶窗丢掉，
提交时手上就有贴着主时钟的帧 ⇒ 既不错位、也不等时钟、也不会超时失败。

**验收**：`pending preroll: starting at the key frame BEFORE the reference (… prefix=N ms)` 出现；
`status=2 … did not reach playback timeline` 消失；`committed … offset=` 仍等于该档
`keyTimePosition − keyRawPts`（B6 不变式）；`attachToFirstFrameMs` 应降到几百毫秒以内。
**回退**：在该条件上加 `&& !mActiveDecoderFramesCarryTimePosition`（一行）即可让
Qt/macOS/iOS 回到"等参考点之后的关键帧"的旧路线。
