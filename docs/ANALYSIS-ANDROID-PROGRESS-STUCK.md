# Android（Compose）「起播后进度条不动，手动 seek 一次才开始走」—— 位置更新链路 + 根因 + 最小修复

> 现象（用户原话）：**播放视频后进度条默认不动，必须手动 seek 一次才开始走**；
> 补充："好像只要归零就不自动动了"（位置停在 / 回到 0 时更新链不启动）。
>
> 目标工程：`platform/Android/ComposePlayer`（Compose UI + `:cicadaplayer` 内核）。
> **验证程度**：本文每条结论都标了【已证实（读码）】或【推测·待验证】。
> **本会话 shell 不可用**：没有编译、没有真机日志，全部结论来自**逐行读码**；
> 编译与真机验收由用户按 §8 做。
>
> **范围（第二轮已扩到三端）**：根因是"音频渲染器漏发'这一帧已交给设备'"这类缺陷。
> 第一轮修的是 Android（`AaudioRender`，§5）；第二轮按同一口径把 **OHOS（`OhosAudioRender`）**
> 与 **Apple 未被编译的 `AFAudioUnitRender`** 补齐，并把仓库里**所有**音频渲染实现排查了一遍
> —— 见 **§6 三端同步**。Apple 实际选中的 `AFAudioQueueRender` 本来就有这条上报，
> 所以 Apple 当前配置**没有**这个缺口。

---

## 1. 结论速览

| # | 结论 | 强度 |
|---|---|---|
| 1 | **根因不在 Compose 侧，也不在判据写法上**：整条链里**没有任何** `if (pos == 0) return` / `pos > 0` / 阈值过滤（逐跳都读过，见 §3.1）。Compose 侧 `if (p >= 0) positionMs = p`（`CicadaPlayerController.kt:281`）**正确地把 0 当合法位置**放行 | 【已证实】§3.1 |
| 2 | **根因（内核 + 安卓平台渲染器）：`AaudioRender` 从不发"这一帧音频已经交给设备"这条事件**。`AudioTrackRender` 在整帧被设备接收后会调 `mListener->onFrameInfoUpdate(info, true)`（`AudioTrackRender.cpp:691-693`）；同族的 `AaudioRender::device_write` 只调了**应用自定义渲染回调** `mRenderingCb`（`AaudioRender.cpp:956-962`），而本工程/任何没注册它的应用拿到的都是 `nullptr` —— 等于什么都不做 | 【已证实】§4.1 |
| 3 | 这条事件是**"有音轨的普通播放"里唯一会推进内容位置 `mCurrentPos` 的写点**（`SMPMessageControllerListener.cpp:1393-1406`；视频那一路被 `mCurrentAudioIndex < 0 \|\| mAudioEOS` 挡在门外，`:1416`）。少了它 ⇒ `mCurrentPos` 一直是它的初值 **0**（`SuperMediaPlayer.h:677` `std::atomic<int64_t> mCurrentPos{}`） | 【已证实】§3.2 / §4.1 |
| 4 | 而 `getCurrentPosition()` 在**"还没建立过不连续点"**（起播后没 seek 过，或 `Reset()` 之后）时**逐字返回 `mCurrentPos`**（`SuperMediaPlayer.cpp:1383` 判据、`:1401` 返回）⇒ `OnTimer` 每 500ms 推给界面的 `NotifyPosition` 都是 **0**（`SuperMediaPlayer.cpp:5732-5749`）⇒ 进度条不动 | 【已证实】§3.3 / §4.2 |
| 5 | 一旦用户 seek 一次：`SeekTo()` → `beginDiscontinuity(seekTargetUs)`（`SuperMediaPlayer.cpp:799`）把 `targetUs` 置成非 `INT64_MIN` ⇒ `getCurrentPosition()` **改从 `mMasterClock.GetTime()` 读**（`:1383-1398`），而那根轴由设备 **presented 帧数**驱动（`getAudioPlayTimeStamp()`，`:6992-7097`），与上面那条缺失的回调**无关** ⇒ 进度条立刻开始走。**这就是"seek 一次就活了"的机制** | 【已证实】§4.3 |
| 6 | 反向也成立：每一次"新一轮播放"（换源 / `stop` 后重新起播 / 重进播放页重建播放器）都会 `Prepare()` → `Stop()` → `Reset()` → `beginDiscontinuity(INT64_MIN)`（`SuperMediaPlayer.cpp:696-698` → `:1106` → `:8026`）⇒ 位置来源退回 `mCurrentPos` ⇒ **又停在 0 不动**。这就是用户那句"只要归零就不自动动了"。注意：**拖动到 0 的那次 seek 不算**（`targetUs = 0 != INT64_MIN`，仍走主时钟），所以复现口径要写清 | 【已证实】§4.3 / §4.4 |
| 7 | 该缺陷是 **AAudio 落地引入的回归**：安卓默认走 AAudio（API ≥ 27，`AaudioRender::isAvailable()`，`AaudioRender.cpp:279-316`；`AudioTrackRender::is_supported()` 反过来 `return !AaudioRender::isAvailable()`，`AudioTrackRender.h:79-83`），而 AAudio 之前的 AudioTrack 路**是有这条上报的**。所以"以前好好的、现在不动了"完全对得上 | 【已证实】§4.1 尾注 |
| 8 | 修复：在 `AaudioRender::device_write()` 里 ring 写入成功之后**补上这条上报**（与 `AudioTrackRender` 同一位置、同一口径）。**一行调用 + 注释**，不新增成员 / 不新增虚函数 / 不加配置开关 / 不加计时器 / 不引平台宏到非平台代码 | 【已证实】§5 |
| 9 | **OHOS 是同一缺口，而且更重**：`OhosAudioRender` 是 OHOS 唯一的音频实现（`renderFactory.cpp:105-106`），它既不上报"整帧已交给设备"，**也不按契约交出帧**（核心靠"unique_ptr 变空"判断出队，`SuperMediaPlayer.cpp:4729-4734`）⇒ 同一帧被反复灌进设备队列、音频记账路径从不执行。已一并补齐（上报 + 交帧） | 【已证实】§6.3 |
| 10 | **Apple 当前配置下没有这个缺口**：真正被选中的是 `AFAudioQueueRender`（**本来就在报**，`copyAudioData:102-105`）；`AFAudioUnitRender` **不在任何编译列表里**（全仓库只命中它自己的源码与 docs）——仍按同一口径补齐，供将来重新启用（§6.2） | 【已证实】§6.2 |
| 11 | 全量排查了仓库里 **8 个**音频渲染实现：需要修的只有 #1 `AaudioRender`（已修）、#5 `OhosAudioRender`（已修）、#4 未编译的 `AFAudioUnitRender`（已补）；其余要么本来就有上报，要么根本没被实例化（§6.4） | 【已证实】§6.4 |

---

## 2. 首要假设的裁决：**"0 被当成哨兵"只对了一半**

任务给的首要假设是"位置更新链里某个判据把 0 当哨兵/未开始"。逐跳核对结果：

| 链路跳 | 与 0 有关的判据 | 裁决 |
|---|---|---|
| `SuperMediaPlayer::getCurrentPosition()` | `mCurrentPos = mCurrentPos < 0 ? 0 : mCurrentPos`（`:1337`）；`contentUs < 0 \|\| af_clock_value_is_unset(contentUs)` 才退回（`:1390`）。`af_clock_value_is_unset(0)` = **false**（`framework/utils/af_clock.h:82-86`，判据是 INT64 极值 ±1 小时带） | **放行 0**，不是哨兵判据 |
| `SuperMediaPlayer::NotifyPosition()` | `mPNotifier->NotifyPosition(position / 1000)`（`:1326-1329`），无过滤 | 放行 0 |
| `PlayerNotifier::NotifyPosition()` | 只查 `mEnable` 与监听器非空（`player_notifier.cpp:205-215`），无值判据 | 放行 0 |
| JNI `NativeBase::jni_onCurrentPositionUpdate` | 只查 `userData` / `env`（`NativeBase.cpp:1389-1405`） | 放行 0 |
| Java `NativePlayerBase` | `obtainMessage(UPDATE_CURRENT_POSITION, (int) position, 0)` → `setExtraValue(msg.arg1)`（`:1037-1042`、`:65-74`）。**这里是 int 承载**（ms 量级远小于 2^31，够用），且 0 不被特殊对待 | 放行 0 |
| Compose `CicadaPlayerController` | `if (p >= 0) positionMs = p`（`:279-282`） | **正确**：0 是合法位置 |
| Compose 进度条 | `playedRatio = (positionMs / safeDuration).coerceIn(0f,1f)`（`PlayerControls.kt:195-201`，细条 `:115`） | 0 就是"已播 0%"，没问题 |

⇒ **不存在"把 0 当无效值丢掉"的判据**。真正发生的是：

> `mCurrentPos` 是"内容位置"的唯一寄存器之一，它的**初值就是 0**（`std::atomic<int64_t> mCurrentPos{}`），
> 而在安卓 AAudio 这条路上**没有任何代码会写它** ⇒ 界面看到的"位置 = 0"不是"播到了开头"，
> 而是"**从来没有被上报过**"。也就是说：**0 在这里确实被当成了"未开始"的哨兵 —— 但它是靠
> "初值 0 + 写点缺失"隐式实现的，而不是靠某个 `if (pos == 0)` 显式判断。**
> 所以修法不是"让判据容忍 0"，而是**把丢失的那个写点补回去**（§5）。

---

## 3. 位置更新链路（完整链路图，含 file:line）

### 3.1 谁在什么时候把位置推给 UI

```
【内核工作线程】SuperMediaPlayer::ProcessVideoLoop（:2060-2225）
      ├─ 缓冲门：DoCheckBufferPass() 为假且非 seek ⇒ 整轮提前 return（:2140-2170）
      │      ⇒ 这一段里 OnTimer 不会被调用（缓冲期间位置不推，是既有设计，不是本次根因）
      └─ 轮末 / 「未工作」分支：
            if (curTime - mTimerLatestTime > mTimerInterval) OnTimer(curTime);   // :2217-2220，:2086-2089
            mTimerInterval = 500 毫秒（:477）

SuperMediaPlayer::OnTimer（:5732-5749）
      if (mPlayedAudioPts != INT64_MIN || mPlayedVideoPts != INT64_MIN) {   // 第一帧渲染之后才成立
          if (mPlayStatus == PLAYER_PLAYING && !isSeeking())
              NotifyPosition(getCurrentPosition());     // ← 位置在这里被"采样"
          NotifyUtcTime();                              // :5742
          PostBufferPositionMsg();
      }

SuperMediaPlayer::NotifyPosition(positionUs)（:1326-1329）→ mPNotifier->NotifyPosition(positionUs/1000)

PlayerNotifier::NotifyPosition（player_notifier.cpp:205-215）→ pushEvent → 通知线程
      → onCurrentPositionUpdate（NativeBase.cpp:1389-1405，回调进 Java）
      → NativePlayerBase.onCurrentPositionUpdate（:1037-1042）
            obtainMessage(UPDATE_CURRENT_POSITION=1000, (int) position, 0) + sendMessage
      → NativePlayerBase.handleMessage（:65-74）→ InfoBean{code=CurrentPosition(2), extraValue=arg1}
      → CicadaPlayerImpl.onInfo（CicadaPlayerImpl.java:75-79，纯透传）
      → CicadaPlayerController.setOnInfoListener（CicadaPlayerController.kt:277-296）
            if (p >= 0) positionMs = p        // :281  ← 0 被正确接受
      → Compose 快照状态 positionMs（mutableLongStateOf，:97）变化 ⇒ 重组
      → PlayerBottomBar(positionMs=…)（CicadaVideoPlayer.kt:895-899）
        / 常驻细条 PlayerThinProgressBar(positionMs=…)（:1046-1054）
      → PlayerControls.kt:195-208 算 playedRatio / dot 偏移；:115 细条算 ratio
```

### 3.2 位置值本身从哪来（`getCurrentPosition()` 的两个来源）

```
SuperMediaPlayer::getCurrentPosition()（:1331-1402）
  ├─ if (isSeeking()) return mSeekPos;                       // :1333-1335
  ├─ 归一化 mCurrentPos（负→0，超过 duration→duration）        // :1337-1341
  ├─ const int64_t discontinuityTargetUs = mDiscontinuity.targetUs;   // :1381
  ├─ if (discontinuityTargetUs != INT64_MIN)  →【来源 A：内容时间轴 / 主时钟】
  │        contentUs = mMasterClock.GetTime();                // :1384
  │        坏值才退回 mCurrentPos（:1390-1392），再钳到 duration（:1394-1396），return
  └─ return mCurrentPos;                                      // :1401 【来源 B：帧驱动的管道位置】
```

* **来源 A（主时钟）**：只在"曾经建立过不连续点"（= 至少 seek 过一次，或切档建立了不连续点）时生效。
  它由 `getAudioPlayTimeStamp()`（`:6992-7097`）= `audioBaseUs + (设备已消费量 - 快照)` 驱动，
  设备已消费量来自 `IAudioRender::getPosition()`（AAudio 是 `queryPresentedFrames()`）。
  **与本节 3.3 那条缺失的回调无关** ⇒ 这就是"seek 之后就正常"的原因。
* **来源 B（`mCurrentPos`）**：从未 seek 过（含每次 `Reset()` 之后）时**唯一**的读数。
  它的写点：`SMPMessageControllerListener.cpp:1401`（音频已渲染）、`:1418/:1420`（视频已渲染，
  **仅当** `mCurrentAudioIndex < 0 || mAudioEOS`）、`SuperMediaPlayer.cpp:3729`（视频包 + TS 不连续）、
  `:3794`（音频包，**要求解码器没有 `DECFLAG_PASSTHROUGH_INFO`**）、`:3387-3406`/`:4986`（都在 seek 窗口内）。

### 3.3 「音频帧已交给设备」这条事件链（本次缺环所在）

```
filterAudioRender::renderLoop（filterAudioRender.cpp:264-305）
      ret = device_write(mRenderFrame);                  // :282
      ⋯ 下一个循环 mRenderFrame = getFrame();            // :296 —— 旧帧在这里析构（所以上报必须在 device_write 内）

AaudioRender::device_write（Android/AaudioRender.cpp:921-1003）
      ⋯ 反压检查（:952-954）
      if (mRenderingCb != nullptr) mRenderingCb(...)     // :956-962  ← 应用自定义渲染回调，本工程没注册 ⇒ nullptr
      written = ringWrite(frame->getData()[0], len);     // :964   ← 真正把 PCM 交给 AAudio 环
      if (written != len) return -EAGAIN;                // :966-970
      【本次修复：在这里补 mListener->onFrameInfoUpdate(frame->getInfo(), true)】   // :972-1000
      return 0;

（对照：AudioTrackRender::write_loop，Android/AudioTrackRender.cpp:681-697）
      ret = device_write_internal(mFrameQueue.front());
      if (mListener) mListener->onFrameInfoUpdate(mFrameQueue.front()->getInfo(), true);   // :691-693  ← 参考实现
      delete mFrameQueue.front(); mFrameQueue.pop();

下游（收到这条事件才发生）：
ApsaraAudioRenderCallback::onFrameInfoUpdate（SuperMediaPlayer.cpp:8515-8518）
      → SuperMediaPlayer::RenderCallback(ST_TYPE_AUDIO, rendered=true, info)（:8157-8184）
      → putMsg(MSG_INTERNAL_RENDERED)（:8183）
      → player_msg_control.cpp:294-296 分发
      → SMPMessageControllerListener::ProcessRenderedMsg(ST_TYPE_AUDIO, …)（:1390-1406）
            若 !rendered 直接 return（:1395-1397）
            if (!isSeeking()) { if (info.timePosition >= 0) mCurrentPos = info.timePosition; }   // :1399-1402 ★
```

**监听器是怎么接上的（证明这条线本来是通的）**：
`SuperMediaPlayer::setUpAudioRender()` → `mAVDeviceManager->setAudioRenderListener(mAudioRenderCB.get())`
（`SuperMediaPlayer.cpp:7231`）→ `SMPAVDeviceManager::setAudioRenderListener` → `mAudioRender->setListener(listener)`
（`SMPAVDeviceManager.cpp:364-367`）→ `IAudioRender::setListener`（`IAudioRender.h:172-175`）⇒ `AaudioRender::mListener` 非空。
`IAudioRenderListener::onFrameInfoUpdate` 的默认实现是**空函数体**（`IAudioRender.h:40-41`）——所以少调一次
**既不会编译报错、也不会打任何日志**，这正是它能悄悄漏掉的原因。

### 3.4 为什么"帧上带着 timePosition"这件事是成立的（否则补上报也没用）

* `avcodecDecoder`（安卓音频走的就是它，见下）在构造时置 `DECFLAG_PASSTHROUGH_INFO`（`framework/codec/avcodecDecoder.cpp:854`），
  含义就是"解码器把包的 timePosition/utcTime **透传**到帧上"（`framework/codec/IDecoder.h:194-201` 的注释、
  `framework/utils/AFMediaType.h:565` 的定义，以及 `SuperMediaPlayer.cpp:3726`/`:3792` 两处**正是**在"解码器没有这个位"
  时才退化成用包级时间）。
* 帧级 timePosition 的来源：`enqueue_decoder()` 把包的 timePosition 写进 `AV_PKT_DATA_STRINGS_METADATA`（`avcodecDecoder.cpp:1070`），
  出帧时从 `outFrame->metadata` 取回（`:1025-1039`）；音频滤镜链会**显式保留**它（`framework/filter/ffmpegAudioFilter.cpp:347-348`、`:422-423`）。
* 包级 timePosition：`framework/demuxer/avFormatDemuxer.cpp:459-464`（`= pts - mCtx->start_time`，首包为 0，之后单调增）。
* 对照：安卓 MediaCodec 解码器**没有**这个位，它把帧的 timePosition 明确置成 `INT64_MIN`
  （`framework/codec/Android/mediaCodecDecoder.cpp:866-870` 的 TODO）⇒ 这正是 `ProcessRenderedMsg` 里
  **视频**分支需要 `info.pts` 兜底、而**音频**分支没有兜底的原因。
* 安卓音频解码方式：`setUpAudioDecoder()` 里 `tryHwAudio = false && …`（`SuperMediaPlayer.cpp:7130-7152`，
  注释写明"默认走 FFmpeg 软解"）⇒ 音频 = `avcodecDecoder` ⇒ **帧上确实带 timePosition**。

---

## 4. 根因

### 4.1 【已证实】根因：AAudio 渲染器漏了"这一帧已交给设备"的上报

* 缺环：`framework/render/audio/Android/AaudioRender.cpp` 的 `device_write()`（`:921-1003`）里
  **没有任何 `mListener->onFrameInfoUpdate(...)`**。**改动前**全文里 `mListener` 只出现在 `onError()`
  的 `mListener->onInterrupt(true)`（改前 `:1115`，改后 `:1116-1117`）；整个文件对
  `onFrameInfoUpdate` 是 **0 次**（这是读码核对过的原状，也是本次修复补上的唯一一处）。
* 参考实现（同一个目录、同一族基类）：`AudioTrackRender.cpp:691-693` 在设备接收整帧后上报 `true`。
  在**会通过 `audioRenderPrototype` 注册、因而真有可能被选中**的实现里（Apple `AFAudioQueueRender`、
  Apple `AFAudioUnitRender`、Android `AudioTrackRender`、Android `AaudioRender`，注册点见
  `AFAudioQueueRender.cpp:140`、`AFAudioUnitRender.cpp:821`、`AudioTrackRender.cpp:38`、`AaudioRender.cpp:321`），
  **只有 `AaudioRender` 与 Apple `AFAudioUnitRender` 缺这条上报**；另有 OHOS `OhosAudioRender`
  （直接实现 `IAudioRender`，不经过 `filterAudioRender`）也缺 —— 第二轮已逐行核并补齐，见 §6.3 / §6.4。
* 为什么它能悄悄漏掉：`onFrameInfoUpdate` 的默认实现是空函数体（`IAudioRender.h:40-41`）；
  而 `AaudioRender.cpp:956-962`（改动前 `:956-960`）那段注释写的是"把'这一帧已经交给设备'上报给播放器
  （它据此更新音频时钟/位置）"，但实际调的是 **`mRenderingCb`（应用自定义渲染回调）**，不是听众（listener）接口。
  本工程从不注册它（`platform/Android/ComposePlayer` 全文 0 处 `AudioRenderingCallback`）⇒ `mRenderingCb == nullptr`
  ⇒ 这段什么都不做。
* 为什么"以前是好的"：AAudio 是**新落地**的默认音频路（`AaudioRender::isAvailable()` 在 API ≥ 27 为真，
  `AaudioRender.cpp:279-316`；`AudioTrackRender::is_supported()` = `!AaudioRender::isAvailable()`，
  `AudioTrackRender.h:79-83`；设计依据见 `docs/ANDROID-AAUDIO-RENDER.md` 第一/二/六节）。AAudio 之前的安卓
  默认是 `AudioTrackRender` —— **它是有这条上报的**。⇒ 这是 AAudio 落地带来的回归，而不是位置链本身的老问题。
* 为什么当时没被发现：`docs/ANDROID-AAUDIO-RENDER.md` 的"验收标记"（第三节 / 第六节末）里，
  与位置/时钟有关的判据**全部是 seek 窗口内的**（`audio first frame after seek … afterSeekMs`、
  设备位置读数、underrun 计数），**没有一条覆盖"普通播放（未 seek）时位置是否在推"**；
  而"帧已渲染"这条契约在 `IAudioRenderListener` 里的默认实现是空函数（`IAudioRender.h:40-41`），
  漏调不会报错、不会打日志。两件事叠在一起，就成了一个只在"起播后不 seek"这个最普通的用法里
  才现形的静默缺口。

### 4.2 【已证实】它怎么变成"进度条停在 0"

* 写点缺失 ⇒ `mCurrentPos` 保持初值 0（`SuperMediaPlayer.h:677`）。注意 `SuperMediaPlayer.cpp:3794`
  那条"音频**包**解码时写 mCurrentPos"的备用路径对 FFmpeg 解码器**被禁用**：
  条件里要求 `!(getDecoder(AUDIO)->getFlags() & DECFLAG_PASSTHROUGH_INFO)`（`:3791-3795`），而 `avcodecDecoder`
  正好置了这一位（`avcodecDecoder.cpp:854`）。
* 视频那一路在有音轨时也不写：`if ((mCurrentAudioIndex < 0 || mAudioEOS) && !isSeeking())`（`SMPMessageControllerListener.cpp:1416`）。
* ⇒ `getCurrentPosition()` 走来源 B，逐字返回 0（`:1401`）⇒ `OnTimer` 每 500ms `NotifyPosition(0)`（`:5732-5749`）
  ⇒ Java/Compose 侧收到 `InfoCode.CurrentPosition = 0`（合法值，被正确接受）
  ⇒ `positionMs` 不变 ⇒ 进度条（控制栏那条 + 控制栏自动隐藏后的常驻细条）都不动。
  **时间文字也停在 `00:00 / …`**（`PlayerControls.kt:643`）。

### 4.3 【已证实】为什么"手动 seek 一次"就把链路踢活了

* `CicadaPlayerController.seekTo()`（`CicadaPlayerController.kt:492-498`）→ … → `SuperMediaPlayer::SeekTo()`
  → `beginDiscontinuity(seekTargetUs)`（`SuperMediaPlayer.cpp:799`，实现 `:8566-8574`：`mDiscontinuity.targetUs = targetUs`）。
* 从那之后 `getCurrentPosition()` 里 `targetUs != INT64_MIN` 成立 ⇒ **来源 A**：`mMasterClock.GetTime()`（`:1383-1398`）。
  主时钟的音频参考是"目标点 + 设备已消费量"（`getAudioPlayTimeStamp()`，`:6992-7097`），
  设备已消费量是 AAudio 的 presented 帧数 —— **它不依赖 §4.1 那条缺失的回调**。
* `ResetSeekStatus()`（`:8210-8271`）**不会**清 `targetUs`（注释 `:8253-8264` 明确写了"SeekEnd 关不掉落点过滤/时间轴"），
  所以 seek 结束后位置照旧从主时钟读 ⇒ "seek 过一次之后就一直正常"。

### 4.4 【已证实】为什么"只要归零就不自动动了"

* 精确链路（"新一轮播放"是怎么把位置来源打回去的）：
  `Prepare()`（`SuperMediaPlayer.cpp:693-709`，`:696-698`：状态不是 INITIALZED/STOPPED 就先 `Stop()`）
  → `Stop()`（`:963-1112`）→ `Reset()`（`:1106`）→ `beginDiscontinuity(INT64_MIN)`（`:8026`）
  + `mCurrentPos = 0`（`:8097`）。
  重进播放页则是**新建播放器实例**（`CicadaVideoPlayer.kt:155` 的 `remember { CicadaPlayerController(context) }`
  → `CicadaPlayerController.kt:82` 新建 player）→ 构造函数里也走一次 `Reset()`（`:476`）。
  ⇒ `getCurrentPosition()` 退回来源 B ⇒ **只要这一轮还没 seek 过，位置就永远是 0**。
* 口径澄清（避免误判复现条件）：
  * **"归零"= 拖动进度条到 0 / 快退到 0** ⇒ 不算。那是一次 seek，`targetUs = 0 != INT64_MIN`，走来源 A，正常；
  * **"归零"= 新一轮播放（换源 / stop→start / 重进播放页）** ⇒ 算。位置来源被 Reset 打回来源 B，**必然停在 0**。

### 4.5 连带被同一条缺失掩盖的其它症状（同一根因，附带被修）

| 位置 | 影响 |
|---|---|
| `SMPMessageControllerListener.cpp:1617` | 切清晰度时 `switchPos = mPlayer.getCurrentPosition()`（后面 `:1619-1621` 把负值/哨兵当 0，而"恒为 0"的 `mCurrentPos` 本身就是 0，绕不过去）⇒ **在这一轮还没 seek 过时切档，视频会被 seek 回 0（画面跳回开头）**；修复后 `switchPos` 是真实位置 |
| `SMPMessageControllerListener.cpp:1690` | 切字幕轨时 `mDemuxerService->Seek(mPlayer.getCurrentPosition(), 0, index)`：首次 seek 前会 seek 到 0 |
| `SMPMessageControllerListener.cpp:1543` | 字幕延迟 `mSubPlayer->seek(mPlayer.getCurrentPosition())`：同上 |
| `SuperMediaPlayer::RenderSubtitle`（`:5670`） | `mSubPlayer->update(getCurrentPosition())`：外挂字幕的同步基准在"首次 seek 前"是 0，修复后正确 |
| `SuperMediaPlayer.cpp:2181-2191`（`mUtcTimer` 直播路） | `mCurrentFrameUtcTime` 在"有音轨"时不会被渲染回调写（`NotifyUtcTime` 的写点 `:1403-1405` 同属音频分支）⇒ **没有包级写点的那类片源**（DASH/fMP4 直播等）之前直播 UTC 同步从未生效；修复后会生效。完整判据与风险见 §6.5 / §7 回归面 |
| `"A_FRAME_RENDERED"` SetOption（`:1398`） | 仓库内**没有读者**（全库只 2 处写入、0 处读取），不影响本次验收 |

---

## 5. 修复（改动点）

### 5.1 改动清单（三处，第二轮扩到三端）

| # | 文件 | 位置 | 改动 | 运行时影响 |
|---|---|---|---|---|
| 1 | `framework/render/audio/Android/AaudioRender.cpp` | `device_write()` 内、`ringWrite()` 成功之后（改后 **:972-1000**；其中调用在 **:998-1000**。改前该处为空） | 新增：`if (mListener != nullptr) { mListener->onFrameInfoUpdate(frame->getInfo(), true); }`，并写清"为什么必须报、为什么 `mRenderingCb` 顶替不了、为什么放在整帧进环之后"；顺带把上面 `mRenderingCb` 那条**误导性**注释（它写成了"把这一帧已经交给设备上报给播放器"，实际是应用自定义渲染回调）改成如实描述 —— 这条错注释正是当初漏掉上报却没被察觉的原因之一 | Android 默认音频路（AAudio），**修复生效** |
| 2 | `framework/render/audio/OHOS/OhosAudioRender.cpp` | `renderFrame()` 内、整帧 PCM 整段进设备取数队列之后（新增块 **:141-178**；上报 **:173-175**；交帧 **:177**） | 同上那条上报 **＋ `frame = nullptr`**（按 `IAudioRender::renderFrame` 的"成功即接走这一帧"契约交帧；这是第二轮发现的第二个同源缺陷，详见 §6.3） | OHOS 唯一音频路，**修复生效** |
| 3 | `framework/render/audio/Apple/AFAudioUnitRender.cpp` | `renderCallback()` 的 `if (frameClear)` 分支内、`delete frame` 之前（新增块 **:638-671**；调用 **:664-666**） | 同上那条上报（与同族 `AFAudioQueueRender::copyAudioData:102-105` 逐字同形；不加交帧——这里的帧是原始指针、由本函数自己 `delete`） | **当前编不进去**（不在任何 CMake 列表），零运行时影响；防将来重新启用时踩坑 |

三处都**只改平台目录下的平台实现**（Android/OHOS/Apple），没有触碰 `mediaPlayer/` 与 `framework/` 的非平台代码；
`OHOS` 那两行的 listener 通路是继承 `IAudioRender` 已有的 `setListener()`/`mListener`，**没有新增成员、没有新增虚函数**（详见 §6.3）。

下面给出第 1 处（第一轮的 Android 改动）全文；OHOS / Apple 两处的上下文与语义见 §6.2 / §6.3：

```cpp
    const size_t written = ringWrite(frame->getData()[0], (size_t) len);

    if (written != (size_t) len) {
        AF_LOGW("[aaudio] short ring write: %u of %d bytes\n", (unsigned) written, len);
        return -EAGAIN;
    }

    /* ── 本次修复：与 AudioTrackRender 同一约定，整帧真的进了设备才上报 ── */
    if (mListener != nullptr) {
        mListener->onFrameInfoUpdate(frame->getInfo(), true);
    }

    return 0;
```

### 5.2 为什么口径与位置这样选（不是凑出来的）

1. **位置**：`filterAudioRender::renderLoop` 在 `device_write()` 返回后立刻 `mRenderFrame = getFrame()`
   （`filterAudioRender.cpp:282/296`），旧帧随即析构 ⇒ 上报**必须在 `device_write()` 内、帧还活着的时候**。
2. **时机**：放在 `ringWrite` 成功之后，对齐 `AudioTrackRender` 的"设备接收整帧之后才报"（`AudioTrackRender.cpp:681-696`）；
   `-EAGAIN`（环满）时**不报**，与 AudioTrack 短写/未写时不报同一口径。
3. **参数恒为 `true`**：与 `AudioTrackRender` 一致（它也是无条件 `true`）。`rendered=false` 在音频路没有语义，
   传它会让 `ProcessRenderedMsg` 在 `:1395-1397` 直接 return，等于没修。
4. **不碰位置计算的判据**：位置仍然由 `info.timePosition` 决定（`:1400-1402`），FFmpeg 音频帧带着它（§3.4）。
   这保证"位置 = 刚交给设备那一帧的内容位置"这一语义与 AudioTrack 路**逐字相同**。

### 5.3 为什么**不**在别处修

| 候选修法 | 为什么否掉 |
|---|---|
| Compose 侧加 ticker / 插值 / 兜底自增 | **红线**（禁止计时器/轮询兜底），而且是"UI 自己造一根位置轴"：与 `positionMs` 打架，seek 后必然回弹；且真实位置本来就是 0（因为没人上报），UI 怎么插值都是假的 |
| JNI / Java 侧改判据 | 那里收到的**全部内容就是 0**；`if (p >= 0)` 本来就是对的，改它只会引入错误 |
| 内核 `getCurrentPosition()` 改成"没有不连续点也用主时钟" | 会改掉 P2.1 明确保留的语义（`docs/SINGLE-DECODER-REFACTOR.md` §9.3 表格："从未 seek 过 ⇒ 逐字走 `mCurrentPos`，行为不变"），把"位置来源"从 1 个变回 2 个互相竞争，风险不可控；而问题本身只是**一个渲染器漏发事件** |
| 在 `ProcessRenderedMsg` 音频分支给 `pts` 兜底 | 变更非平台内核代码的位置语义（DASH 上 `pts` 与 `timePosition` **不是同一根轴**，见 `docs/ANALYSIS-AV-DESYNC-ANDROID.md` §4.4 的 `offset=-21000` 证据）⇒ 可能引入回弹，收益为零（帧上本来就有 timePosition） |

### 5.4 其它音频渲染实现

逐个排查结论、三端（Android / Apple / OHOS）的上报点与 ABI/编译面说明统一放在 **§6 三端同步**。

---

## 6. 三端同步（Android / Apple / OHOS）与全量音频渲染实现排查

> 本节是"同一根因在其它平台是否存在"的收口：**逐端给出上报点 file:line、为什么它等价于
> `AudioTrackRender.cpp:691-693` 的语义、以及该端是否真的会被实例化**。

### 6.0 先确定"哪条路真的在跑"（三端的实际选择）

| 端 | 运行时真的被创建的音频渲染器 | 依据 |
|---|---|---|
| Android（API ≥ 27 / AAudio 可用） | `AaudioRender` | `audioRenderPrototype` 注册（`AaudioRender.h:116-121`）+ `is_supported() = isAvailable()`；`AudioTrackRender::is_supported()` 反向让位（`AudioTrackRender.h:79-83`） |
| Android（AAudio 不可用） | `AudioTrackRender` | 同上；AAudio 落地前一直也是它 |
| Apple（iOS / macOS） | `AFAudioQueueRender` | `framework/render/CMakeLists.txt:80-83`（APPLE 段**只列了它**）+ `addPrototype`（`AFAudioQueueRender.cpp:140`）+ `is_supported()` 恒真（`.h:69-72`）⇒ `audioRenderPrototype::create()` 直接返回它，**先于**工厂的 Cheater/SDL 兜底分支（`renderFactory.cpp:96-108`） |
| OHOS | `OhosAudioRender` | OHOS 侧没有任何 `addPrototype` 注册 ⇒ `renderFactory.cpp:105-106` 的 `#elif defined(__OHOS__)` 就是唯一出口；`HarmonyOS.cmake` 未开 `ENABLE_CHEAT_RENDER` / `ENABLE_SDL`（只有 `ENABLE_GLRENDER` / `ENABLE_OHOS_AVCODEC_DECODER` / `ENABLE_OHOS_AUDIO_RENDER`） |
| Linux（Qt） | `CheaterAudioRender` | `Linux.cmake:138` 开 `ENABLE_CHEAT_RENDER`；SDL 渲染器不注册原型 ⇒ `renderFactory.cpp:96-98` |
| Windows / `ENABLE_SDL` | `SdlAFAudioRender2` | `windows.cmake:118` 开 `ENABLE_SDL`（未开 CHEAT）⇒ `renderFactory.cpp:107-108` |

### 6.1 Android（参照实现，本轮最先修）

见 §5：`AaudioRender::device_write()` 里 `ringWrite` 成功之后上报（改后 **`:998-1000`**）。
语义 = "整帧 PCM 进了设备取数环"，与 `AudioTrackRender` 的"设备接收整帧"（`:691-693`）同一档。

### 6.2 Apple：`AFAudioUnitRender`（同族里唯一缺上报的实现，但当前**编不进去**）

* **当前 Apple 没有这个缺口**：真跑的是 `AFAudioQueueRender`，它在 `copyAudioData()` 的
  `frameClear` 分支里、`delete` 之前上报，并且带了自己的 flush 守卫：
  `if (mListener && !mNeedFlush) mListener->onFrameInfoUpdate(mInPut.front()->getInfo(), true);`
  （`AFAudioQueueRender.cpp:102-105`；该函数由 AudioQueue 的 OutputCallback 调用，`:43`）。
* **`AFAudioUnitRender` 不在任何编译列表里**：全仓库搜 `AFAudioUnitRender` 只命中它自己的
  `.h/.cpp` 与 docs（`framework/render/CMakeLists.txt` 的 `if (APPLE)` 段只列了
  `audio/Apple/AFAudioQueueRender.cpp/.h`，`:82-83`）⇒ 它是被 `AFAudioQueueRender` 取代的旧实现，
  **当前对任何目标都没有运行时影响**。
  这一条同时把 §6.0 的"Apple 实际选中"钉死了：两个 Apple 音频渲染器的 `is_supported()` 都恒真
  （`AFAudioQueueRender.h:69-72`、`AFAudioUnitRender.h:66-69`），注册顺序本会由链接顺序决定 ——
  但**只有一个被编进去**，所以选中结果没有不确定性。
* **仍然补齐**（理由：源码要与同族语义一致，将来若有人把它加回 CMake，不会立刻踩同一个坑）：
  上报点 = `AFAudioUnitRender::renderCallback()` 的 `frameClear` 分支、`delete frame` 之前
  （改后 **`:638-671`**，其中调用在 **`:664-666`**）。
  * 为什么这一点是"整帧真的被设备消费"：`copyPCMDataWithOffset(..., &frameClear)` 给出
    `frameClear == true`，表示这一帧的 PCM 已经**整段**被复制进 AudioUnit 交给本渲染器的输出
    缓冲（`ioData`）—— 与 `AFAudioQueueRender` 的判据同源、同形；
  * **部分写入不报**：`frameClear == false`（`remaining > 0`，这一帧还没读完）时继续循环、不上报；
  * **"写尝试"不报**：`!mRunning || mNeedWaitDateFull` 的提前返回（`renderCallback:597-609`）
    只对 `ioData` 补静音、根本不碰帧 ⇒ 不上报；
  * 与 AudioQueue 那条的唯一差异：本类没有 `mNeedFlush` 成员（flush 走 `pause_device()` + 等
    `mDeviceWorking` 归零 + 清空队列，`flush_device:424-453`），所以少了那半个守卫 ——
    不影响"整帧已消费"这个事实。
* **线程说明（如实记录）**：这条上报发生在 CoreAudio 的渲染回调线程上。这不是新引入的做法 ——
  同族的 `AFAudioQueueRender` 也是在 CoreAudio 回调里上报（`copyAudioData` ← `OutputCallback`），
  而且那个回调本来就在做 `af_msleep(2)` / `delete frame` 这类非实时操作。上报 → `RenderCallback`
  → `putMsg` 的**按值拷贝**（`MsgRenderedParam.info`，`SuperMediaPlayer.cpp:8178-8183`）在
  `delete frame` **之前**同步完成，所以不会出现悬垂引用。

### 6.3 OHOS：`OhosAudioRender`（**真实缺口，已修**）

* **listener 通路本来就通，不需要任何新接口**：`OhosAudioRender : public IAudioRender`
  （`OhosAudioRender.h:33`）⇒ 继承 `IAudioRender::setListener()`（`IAudioRender.h:172-175`）与
  受保护成员 `mListener`（`:186`）；`SuperMediaPlayer::setUpAudioRender()` 在**所有平台**都会执行
  `setAudioRenderListener(mAudioRenderCB.get())`（`SuperMediaPlayer.cpp:7231`）→
  `SMPAVDeviceManager::setAudioRenderListener()`（`SMPAVDeviceManager.cpp:364-367`）→
  `mAudioRender->setListener(listener)`。所以 OHOS 上 `mListener` **非空**，只是从来没有任何代码读它。
* ⇒ **本次三端同步没有新增任何虚函数、没有新增任何成员、没有 vtable / 布局变化**
  （"新虚函数只追加在 vtable 末尾、新成员只追加在类末尾"这条约束**本次未被触发**）：
  三端都只是在既有函数体里加了一次调用（OHOS 另加一行 `frame = nullptr`）。
* **改动**（`OhosAudioRender.cpp:141-178`，其中上报在 **`:173-175`**、交帧在 **`:177`**）：
  在 `renderFrame()` 里"整帧 PCM 已经整段进设备取数队列"之后上报 + `frame = nullptr`。
  1. **为什么这一点是"整帧被设备消费"**：队列放不下时上面直接 `return -EAGAIN`（一个字节都没写），
     写进去的字节数恒等于整帧 PCM 长度 ⇒ 走到这里只剩"整帧成功"；设备侧 `OnWriteData`
     （改后 `:324`）正是从这个队列取数。
     ⚠ 本渲染器与 `filterAudioRender` 不同：它**没有** `device_write` 那一层（队列是字节队列，
     没有"帧边界"），所以"帧被消费"只能在"整帧入队成功"这一刻判定 —— 这与
     `SdlAFAudioRender2::device_write`（`SDL_QueueAudio` 成功后立刻上报 + `frame = nullptr`，
     `SdlAFAudioRender2.cpp:164-173`）是同一个位置语义。
  2. **为什么必须同时交出帧（第二个发现，与位置同源）**：`IAudioRender::renderFrame()` 的约定是
     "返回成功即接走这一帧" —— 基类用 `mFrameQue.push(std::move(frame))`（`filterAudioRender.cpp:157`），
     另一个直接实现用 `frame = nullptr`（`SdlAFAudioRender.cpp:152`）；播放器就是按
     "回到自己手里时那个 unique_ptr 已经为空"判断出队的（`SuperMediaPlayer.cpp:4729-4734`）。
     改前这里两者都没有 ⇒ ① 每次 `render()` 都用**同一帧**再调一次 `renderFrame`，同一段 PCM
     被反复灌进设备队列（音频实际在循环这一帧）；② 只有出队之后才执行的音频记账
     （`mPlayedAudioPts` / `mAudioTime` 建立、纯音频片源的 `checkFirstRender`、音频切流通知、
     seek 窗口内的 `mCurrentPos` 兜底，`SuperMediaPlayer.cpp:4736-5003`）在这条路上从不执行。
  * **顺序**：先上报（要读 `frame->getInfo()`）再置空；上报在队列锁**之外**（`notify_all()` 之后），
    不持锁回调播放器。
* **残留（未改，如实记录）**：`renderFrame()` 开头两条早退 —— `mRenderer == nullptr → -EINVAL`
  与 `data / lineSize 无效 → return 0`（`:112-120`）—— 都不交帧。它们在"设备已就绪 + 正常 PCM 帧"
  的正常路径上不可达；改了会改变"核心是否把这帧当已渲染"的语义（`return 0` 会让核心把它算成
  `RENDER_FULL`，见 `SuperMediaPlayer.cpp:4596-4598` 的说明），所以本次只记录、不动。

### 6.4 全量排查：仓库里所有音频渲染实现

| # | 实现（文件） | 是否真会被实例化（依据） | `onFrameInfoUpdate` | 上报点 / 结论 |
|---|---|---|---|---|
| 1 | `Android/AaudioRender.cpp` | 是（Android 默认） | **改前无 → 已补**（`:998-1000`） | 见 §5；整帧进设备环之后 |
| 2 | `Android/AudioTrackRender.cpp` | 是（Android 回退 / AAudio 之前的默认） | 有 | `:691-693`（写线程里设备接收整帧之后） |
| 3 | `Apple/AFAudioQueueRender.cpp` | 是（Apple 实际选中的那个） | 有 | `:102-105`（`copyAudioData` 的 `frameClear` 分支、带 `!mNeedFlush` 守卫） |
| 4 | `Apple/AFAudioUnitRender.cpp` | **否**（不在任何 CMake 列表；全仓库只命中源码与 docs） | **改前无 → 已补**（`:664-666`） | `renderCallback` 的 `frameClear` 分支；对当前构建**零运行时影响** |
| 5 | `OHOS/OhosAudioRender.cpp` | 是（OHOS 唯一） | **改前无 → 已补**（`:173-175`） | `renderFrame` 整帧入队成功后（同时补 `frame = nullptr`） |
| 6 | `SdlAFAudioRender2.cpp` | 是（Windows / `ENABLE_SDL`） | 有 | `:167-169`（`SDL_QueueAudio` 之后、`frame = nullptr` 之前） |
| 7 | `CheaterAudioRender.cpp` | 是（Linux；`Linux.cmake:138` 的 `ENABLE_CHEAT_RENDER`）。macOS 的 `macOSX.cmake:94` 也开了它，但 Apple 上原型（`AFAudioQueueRender`）先被返回、根本走不到这个分支，见 §6.0 | 有 | `:54-55`（`device_get_position()` 里"帧的时钟时间已过"= 已消费） |
| 8 | `SdlAFAudioRender.cpp`（旧 SDL1 实现） | **否**：没有 `addPrototype`，工厂只用 `SdlAFAudioRender2`；唯一构造点是 `framework/ffmpeg/sdl_test.cpp:110` 的**注释行** | 无 | **死代码**：不改（改了没有任何运行时效果，也无法验收）。记录在此，避免以后误判"漏了它" |

⇒ 结论：**真正会让用户看到"起播后进度条不动"的只有 #1（Android，已修）与 #5（OHOS，已修）**；
Apple 当前配置本来就正常（#3）；#4 是未编译的旧实现（已补，防将来），#8 是死代码（只记录）。

### 6.5 直播（UTC）影响：三端一致

* **写点**：`ProcessRenderedMsg` 的音频分支 `mCurrentFrameUtcTime = info.utcTime`
  （`SMPMessageControllerListener.cpp:1403-1405`）；视频分支同（`:1422-1424`），但被
  `(mCurrentAudioIndex < 0 || mAudioEOS)`（`:1416`）挡住 ⇒ **有音轨时视频分支不写**。
* **另有两个"包级"写点（在 doDeCode 里，与本次改动无关）**：
  `SuperMediaPlayer.cpp:3725-3733`（视频包：要求 `isTSDiscontinue()` 且视频解码器**没有**
  `DECFLAG_PASSTHROUGH_INFO`）与 `:3791-3798`（音频包：要求音频解码器没有该标志 ⇒ FFmpeg 软解被禁用）。
  ⇒ **HLS-TS + 安卓硬解**这类片源本来就在写 `mCurrentFrameUtcTime`，直播同步本来就在跑；
  本次修复改变的是**没有包级写点**的那类片源（DASH/fMP4 直播等）：它们在此之前
  `mCurrentFrameUtcTime` 恒为 `-1`（`Reset()` 的置位点 `:8098`）⇒ 入口判据
  `mCurrentFrameUtcTime > 0`（`:2188`）不成立 ⇒ `LiveTimeSync()` 从不执行。
* **消费点**：`NotifyUtcTime()`（`:1404-1410`，只要求 `>= 0`）→ 应用层 UTC 回调；
  `mUtcTimer` 非空时的 `LiveTimeSync(mUtcTimer->get() - mCurrentFrameUtcTime)`（`:2181-2191`）。
  `LiveTimeSync`（`:3004-3090`）的动作：偏离阈值时**丢左侧陈旧缓冲**
  （`ClearPacketBeforeTimePos` + `FlushVideoPath` / `FlushAudioPath` + 主时钟重钉），
  或把播放速率在 1.2 / 0.9 / 1.0 之间切换。
* **三端是否都会因此改变直播行为**：会 —— 三端的修复都让"音频帧的 utcTime"开始参与。
  判据（真机、DASH 直播、内核日志级别需 DEBUG）：
  1. `LiveTimeSync, delayTime=…`（`:3009` 的 1 秒限频日志、`:3067`）只在 `delayTime` 真的越过
     `mSuggestedPresentationDelay ± maxGopTime/2` 时出现，**不在 1.2/0.9 之间来回抖**
     （抖动意味着音频帧与视频帧的 `utcTime` 不同轴；那时正确的后续动作是**只在视频分支写 utcTime**，
     属内核非平台改动，需要先有这份证据，本次不做）；
  2. `drop left lateUTCTime`（`:3037`）不反复刷：一次丢弃到 live edge 之后应稳定；
  3. 非直播（`mUtcTimer == nullptr`）该分支整体不执行 ⇒ 与本次改动无关（可在同一次验收里对照）。

---

## 7. 回归面

| 路径 | 行为变化 | 依据 |
|---|---|---|
| **Android + AAudio（本次受影响面）** | 每交付一帧 PCM 给 AAudio 环，多一次 `onFrameInfoUpdate(info, true)` ⇒ `ProcessRenderedMsg(ST_TYPE_AUDIO, …, true)` 开始被调用 | `AaudioRender.cpp:921-1003`（调用点改后 `:998-1000`） |
| **Android + AudioTrack（回退路，API < 27 / AAudio 符号缺失 / 曾开流失败）** | **零变化**：该路径本来就在报（`AudioTrackRender.cpp:691-693`），且是不同的类 | 同上 |
| **OHOS（`OhosAudioRender`，唯一实现）** | 两处变化：① 每交付一帧 PCM 多一次上报；② **该帧从此按契约交出**（`frame = nullptr`）⇒ 播放器开始正常出队，`mPlayedAudioPts` / `mAudioTime` / 首帧通知 / 切流通知 / seek 窗口兜底这些"出队后才会跑"的路径开始执行 | `OhosAudioRender.cpp:141-178` |
| **Apple（`AFAudioQueueRender`，实际选中）** | **零变化**：它本来就在报（`:102-105`） | 文件本身未改 |
| **Apple（`AFAudioUnitRender`，当前不在任何编译列表）** | 对当前构建 **零运行时影响**（编不进去）；若将来加回 CMake，行为 = 多一次上报 | `AFAudioUnitRender.cpp:638-671` |
| Linux / Windows（`CheaterAudioRender` / `SdlAFAudioRender2`） | **零变化**：两者本来就在报（`:54-55` / `:167-169`） | 文件未改 |
| 位置上报频率 | **零变化**：`OnTimer` 仍是 500ms（`mTimerInterval`，`:477`），`NotifyPosition` 的节流/去重逻辑一个字没动 | `:5732-5749` |
| 消息负载 | 每音频帧多一条 `putMsg`（AAC 1024@44.1k ≈ 21.5 次/秒）。**同一个设备上视频路本来就在按帧率（24~60 次/秒）发同一条消息**（`GLRender.cpp:522` → `RenderCallback(ST_TYPE_VIDEO…)`），所以没有新的负载量级；OHOS 上"同一帧被反复上报"的隐患随 `frame = nullptr` 一起消失 | `GLRender.cpp:522`、`SuperMediaPlayer.cpp:8157-8184` |
| `mCurrentPos`（来源 B）的读数 | 首次 seek 前不再恒为 0，而是"刚交付设备那一帧的 `timePosition`"。受影响的读者：位置上报（本次目标）、`RenderSubtitle`（`:5670`）、切档起点 `switchPos`（`SMPMessageControllerListener.cpp:1617`）、切字幕的 seek（`SMPMessageControllerListener.cpp:1690`）、`FillVideoFrame` 的 `pos`（`:4292`，实际未参与判定，只用在一处 seek 窗口判据 `:4020`）、`SeekInCache`（`:6750`，但 `mSeekInCache` 在 `SMPMessageControllerListener.cpp:1090` 被强制为 false） | 逐个读过 |
| 暂停 / 恢复 | 暂停时 `OnTimer` 的 `PLAYER_PLAYING` 门挡住 ⇒ 位置不推（不变）；恢复后（`:697-718`）继续走。**修复前**若这一轮还没 seek 过，恢复后位置仍是 0；**修复后**恢复后继续走 —— 这正是期望 | `:5739`、`SMPMessageControllerListener.cpp:697-718` |
| 缓冲 | 缓冲期间（`DoCheckBufferPass()` 假且非 seek）整轮提前 return，`OnTimer` 不跑 ⇒ 位置不推（既有设计，不变） | `:2140-2170` |
| 直播（`mUtcTimer` 非空，三端一致） | 见 §6.5：本次修复让"没有包级 utcTime 写点"的片源（DASH/fMP4 直播等）第一次开始喂 `mCurrentFrameUtcTime` ⇒ `LiveTimeSync()` 可能从"从不执行"变成执行（含 1.2/0.9 变速与丢陈旧缓冲）。判据见 §6.5，风险与回退方向都写在那里 | `:2181-2191`、`:3004-3090` 【推测·待验证】 |
| `bEnableVRC`（`NotifyAudioRendered`） | 默认 `false`（`player_types.h:76`），本工程不设 ⇒ 无变化 | 同上 |
| 精度 | 不涉及任何"精度换流畅"：位置仍是"已交付帧的内容位置"，与 AudioTrack 路逐字同口径 | §5.2 |
| ABI / 编译面 | 三端都**没有新增成员、没有新增虚函数**（OHOS 的 listener 是靠继承 `IAudioRender` 已有的 `setListener`/`mListener`，见 §6.3）；未加配置开关、未加计时器、未在非平台代码里加平台宏 | 三个 diff |

---

## 8. 判据（用户复现 / 验收步骤）

### 8.1 复现（修复前，用于确认现象与根因）

| # | 步骤 | 修复前应当看到 |
|---|---|---|
| P1 | 冷启动 → 播一个**有音轨**的片源（本地 mp4 或网络 mp4/HLS/DASH） | 控制栏进度条与 `00:00 / …` 时间文字**一直不动**；3 秒后控制栏自动隐藏，底部细进度条同样不动 |
| P2 | 同上，**不碰任何控件**，等 5 秒 | 位置始终 0（"00:00"）；没有卡顿、没有报错、画面与声音正常 |
| P3 | 随便拖一下进度条（或点一下进度条任意位置） | 松手后进度条**立刻开始走**，之后一直正常 |
| P4 | 换一个**无音轨**的片源（或用 ffmpeg 去掉音轨）重来 P1 | 预期**正常**（视频路的 `mCurrentAudioIndex < 0 \|\| mAudioEOS` 成立，会写 `mCurrentPos`；硬解 `timePosition=INT64_MIN` 时还有 `info.pts` 兜底）。**这一条是"根因指向音频上报"的最强判据** |
| P5 | **换源起播**（同一个播放页里换一个片源），或退回首页再进播放页（重建播放器） | 位置**又回到 0 不动**（= `Prepare()→Stop()→Reset()` 把 `targetUs` 打回 `INT64_MIN`，位置来源退回 `mCurrentPos`），即用户说的"只要归零就不自动动了"。注意：**"播完再点播放"（不走换源/不进页面）不在这条判据里** —— 那条路上 `ProcessStartMsg`（`SMPMessageControllerListener.cpp:697-718`）会把主时钟重设到首帧，是否复现取决于本轮有没有 seek 过 |

> P4/P5 是【推测·待验证】：由 §4 的代码推导得出，用来把"根因是音频上报"与其它解释区分开。
> 若 P4 也复现（无音轨片源同样不动），说明根因不在这条链上，需要重查（届时请把 `adb logcat` 与
> `getCurrentPosition()` 的返回值一起抓下来）。

### 8.2 验收（修复后，Android）

| # | 步骤 | 期望 |
|---|---|---|
| A0 | 编译并装到 **API ≥ 27** 的设备（这样才走 AAudio 路；API ≤ 26 会走 AudioTrack，那是没病的路，验收不到本次修复） | `libCicadaPlayer.so` 重新编出（改的是 `framework/` 下被内核编入的 .cpp，**必须整体重编内核**） |
| A1 | 冷启动 → 播有音轨的片源，**不碰任何控件** | **2 秒内**进度条开始走；`00:00 → 00:0N` 时间文字同步递增（内核位置节拍是 500ms，所以最迟 0.5~1 秒内应看到第一跳） |
| A2 | 同上，观察 10 秒 | 0→N 秒平滑递增（每 ~0.5 秒一跳，符合 `mTimerInterval = 500`），不跳变、不回退 |
| A3 | 拖动进度条到 30% 处松手 | 松手后 dot 停在目标点（不回弹），位置**从目标点继续走**（不跳回 0） |
| A4 | 拖到接近片尾再往回拖 | 同上，无回弹、无卡在目标点不动 |
| A5 | 暂停 → 等 3 秒 → 恢复 | 暂停期间位置冻结；恢复后继续（**修复前**：若这一轮还没 seek 过，恢复后仍不动） |
| A6 | **换源起播**（同一播放页换片源）/ 退回首页再进播放页 | 位置**从 0 立刻开始走**，不再需要 seek 一次 |
| A7 | 切后台（后台播放开 / 关各一次）→ 回前台 | 位置连续；关后台播放时会暂停、回前台自动续播且位置继续走 |
| A8 | 无音轨片源重跑 A1 | 同样正常（修复前后都应正常，用于确认没有把视频路弄坏） |
| A9 | 切清晰度（自动/手动各一次） | 切换后位置继续走；**特别看"首次切档发生在没 seek 过的时候"**：修复前 `switchPos` 会被算成 0（画面跳回开头），修复后不跳 |
| A10 | 直播流（若有） | 位置/UTC 正常，`LiveTimeSync` 不引入异常（判据见 §6.5；这是三处修复共同带来的唯一行为变化面）【推测·待验证】 |

### 8.3 三端各自的验收步骤（怎么在"不 seek"的情况下看到位置开始走）

三端的共同判据只有一条：**起播后不碰任何控件，位置/进度必须在 2 秒内开始单调递增**；
不同端只是"看哪里"和"编什么目标"不同。

| 端 | 要编的目标 | 不 seek 的观察点 |
|---|---|---|
| **Android** | `platform/Android/ComposePlayer`（`:cicadaplayer` 内核重编；NDK 25.2 / CMake） | 本 App 的两个进度条：控制栏里那条（`PlayerProgressBar`）与 3 秒后常驻底部的细条（`PlayerThinProgressBar`）+ `00:00 / …` 时间文字（`PlayerControls.kt:643`）。详见 §8.2 A1/A2 |
| **OHOS** | `platform/HarmonyOS`（OHOS Native SDK + `framework/HarmonyOS.cmake`，arm64-v8a / armeabi-v7a / x86_64；链接 `libohaudio`）：重编 `cicadaplayer` HAR + `entry`。示例工程 `entry` 的 ArkTS 页把 `onPositionUpdate` 直接绑到 UI（`Index.ets:199-200` `self.positionMs = position`），再喂给 **同一套 Compose 播放器** `CicadaComposePlayer`（`Index.ets:744-753`，进度条比例就是 `positionMs/durationMs`：`cicadaplayer/.../compose/PlayerControls.ets:79`），页面下方还有一行 `formatMs(this.positionMs)` 文本（`:794`） | 起播后**不动控件**：`CicadaComposePlayer` 的进度条与该时间文本必须在 2 秒内从 `00:00` 开始递增。**附加判据（OHOS 特有）**：修复前音频实际在循环同一帧（见 §6.3）⇒ 还应听得到音频**连续正常**，而不是一小段循环；并顺带看"换源起播 / 播完重播"是否还需要 seek 一次 |
| **Apple（iOS / macOS）** | `platform/Apple/demo/iOS/CicadaDemo`（`genxcodeproj.sh` 生成工程）或 `platform/Apple/demo/macOS/CicadaDemo`（Xcode） | 这两个 demo 走的是 `AFAudioQueueRender`（**本来就在报**，§6.2）⇒ 预期修复前后都正常：进度滑块（`CicadaSlider`）与 `onCurrentPositionUpdate`（`CicadaPlayerViewController.m:697`）在起播后自动递增。**这一端的验收目的是"确认没有被弄坏"**，不是"看修复生效" |
| （对照）桌面 Qt | `platform/QtPlayer` | 走 `CheaterAudioRender`（Linux）/ `SdlAFAudioRender2`（Windows），两者本来就在报 ⇒ 同样用来确认"没有被弄坏" |
| **只有把 `AFAudioUnitRender` 重新加进 CMake** | `framework/render/CMakeLists.txt` 的 `if (APPLE)` 段加入 `audio/Apple/AFAudioUnitRender.{cpp,h}` | 加进去之后它才会被链接（两个 Apple 渲染器的 `is_supported()` 都恒真，届时选谁由静态注册顺序/链接顺序决定）⇒ 到那时才有必要验证 §6.2 那处补齐：用同一个 demo 看位置是否仍自动递增。**本次不建议加**（它是被取代的旧实现，加它等于换一条未经真机检验的音频路） |

> `AFAudioUnitRender` 当前编不进去 ⇒ **Apple 侧本次没有可验收的行为变化**（§7 表里已写明"零运行时影响"），
> iOS/macOS 两个 demo 只需跑一遍确认"没被弄坏"。

### 8.4 日志判据（可选）

* **先确认走的是哪条音频路**（决定本次验收是否有意义）：
  `adb logcat | grep aaudio` 里应有
  `[aaudio] output=aaudio stream opened: performanceMode=… rate=… channels=… bufferCapacityFrames=… ringBytes=…`
  （`AaudioRender.cpp` 的 openStream；见 `docs/ANDROID-AAUDIO-RENDER.md` 第三节）。
  若出现 `[aaudio] not used on this device (API level=26 < 27) ⇒ fall back to AudioTrack` 或
  `[aaudio] libaaudio unavailable ⇒ fall back to AudioTrack`，说明这台设备走 AudioTrack ——
  **那条路本来就没病**，本次修复在它上面看不出差别（需要换 API ≥ 27 的设备）。
* `PlayerNotifier::NotifyPosition()` 自带一行 `AF_LOGD("NotifyPosition() :%lld", pos)`
  （`player_notifier.cpp:207`）。它默认被日志级别挡住（INFO）；若能把内核日志级别开到 DEBUG
  （框架接口 `log_set_log_level(AF_LOG_LEVEL_DEBUG)`，`framework/utils/frame_work_log.h:39`），
  修复前应看到该值**恒为 0**、修复后单调递增（每 ~500ms 一次）。
* 同机对照（若手边有 API ≤ 26 的设备或能把 AAudio 关掉）：走 AudioTrack 的那台
  **修复前后都正常** —— 这既是回归面的验收，也再次把根因钉在"AAudio 这条渲染路上"。

---

## 9. 一句话总结

进度条不动**不是** UI 的 ticker 没启动（Compose 侧压根没有位置 ticker，位置是纯事件驱动），
**也不是**某个判据把 0 当哨兵 —— 而是**音频渲染器漏发了"这一帧音频已经交给设备"这条事件**，
导致"内容位置寄存器" `mCurrentPos`（初值 0）在**首次 seek 之前永远没有被写过**；
而 `getCurrentPosition()` 恰恰在"还没 seek 过"时逐字返回它。一次 seek 会建立不连续点，
把位置来源切到由设备 presented 帧数驱动的主时钟上，于是"seek 一次就活了"。

三端同步后：**Android 的 `AaudioRender`**（`device_write` 的 ring 写入成功之后）与
**OHOS 的 `OhosAudioRender`**（`renderFrame` 的整帧入队成功之后，并同时按契约交出帧）已补齐；
**Apple 当前配置本来就正常**（实际选中 `AFAudioQueueRender`，它一直在报），未被编译的
`AFAudioUnitRender` 也按同一口径补齐以防将来重新启用；其余实现经全量排查无此缺口
（含一处死代码，只记录不改）。三端都没有新增成员 / 虚函数 / 配置开关 / 计时器。
