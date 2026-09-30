# 安卓侧：进入/退出播放的资源释放审计

> 目标（与桌面同口径，用户已重新定标）：**退出播放器（返回键 / 关闭播放器）之后，进程内存
> 应回到"只开主界面"的水平，且反复进出不得逐轮爬升**。
> 本文只做两件事：把**两条入口**到"资源全部释放"的链路逐跳写清楚（file:line），
> 以及给出**判据**（关一次该看哪些日志行）。
>
> 标注约定同主文档：**【已证实】** = 本会话读到了那段代码；**【推测】** = 由代码结构推出；
> **【未逐行核实】** = 需要真机/进一步读码才能定论。
> 主文档：`docs/ANALYSIS-MEMORY-FOOTPRINT.md`（桌面侧，结论已按 25MB 基线更正）。

---

## 0. 结论摘要

1. **安卓侧的两条入口在代码上是完整的**，而且**最终都汇进同一个 C++ 收口点**
   `CicadaReleasePlayer()`（`mediaPlayer/media_player_api.cpp:40`）——
   也就是说**安卓与桌面共用同一套"销毁播放器 = 释放解码器 + 表面池 + 队列 + 解复用器 + 数据源"的收口**。
   **【已证实】**
2. 与桌面最大的不同：安卓**没有**"播放器窗口对象还活着"这个风险面（没有 QQuickWindow/QML 树），
   所以桌面上那个"每轮 +25 MB 的窗口树"在安卓不存在；安卓要盯的是
   **解码器/Surface、音频流、截图 Bitmap、以及每轮新建的监听器**。**【推测，需真机对照】**
3. 本轮给安卓补了 **3 条 C++ 释放探针**，让"返回键一次 / 关闭一次"各能打出一整套释放日志：
   `CicadaReleasePlayer`（共用收口）、`MediaCodec decoder closed`、`AAudio output stream closed`。
   **【已证实（代码已改）】**

---

## 1. 两条入口的逐跳链路

### 1.1 新播放器（`platform/Android/ComposePlayer`）：返回键与"关闭播放器"是**同一条**退栈路径

| 跳 | 位置 | 做了什么 | 判定 |
|---|---|---|---|
| 1a | **返回键**：`app/.../components/video/CicadaVideoPlayer.kt:379` `BackHandler(enabled = isFullscreen \|\| openPanel != PlayerPanel.None)` | 只在"全屏中/面板打开"时**截获**返回键（用于退出全屏或关面板） | **这是"只是退一层 UI"，不是释放** —— 正确 |
| 1b | 非全屏、无面板时的返回键 | 没有被 `BackHandler` 截获 ⇒ 交给 Navigation ⇒ `MainActivity.kt:64` 的 `popBackStack()` | 进入退栈路径 |
| 1c | **关闭播放器按钮**：`VideoPlayerScreen.kt:353`（全屏→退全屏；非全屏→返回上一页）→ `onClose` → `MainActivity.kt:64` `popBackStack()` | 与返回键**同一条**路径 | ✓ 两条入口合一 |
| 2 | `popBackStack()` → 播放页 composable 离开组合 | Compose 的 `DisposableEffect` 触发 `onDispose` | 关键一跳 |
| 3 | `app/.../components/video/CicadaVideoPlayer.kt:577-578` `DisposableEffect(controller) { onDispose { controller.release() } }` | 调 `CicadaPlayerController.release()` | **同步调用** ✓ |
| 4 | `app/.../player/CicadaPlayerController.kt:485-490` `release()` | ① `releaseSnapshot()`；② `player.setSurface(null)`；③ `player.release()` | ✓ 顺序正确（先回收截图、再摘 Surface、最后 release 播放器） |
| 5 | `app/.../player/CicadaPlayerController.kt:865-868` `releaseSnapshot()` | `snapshotBitmap?.recycle(); snapshotBitmap = null` | **截图 Bitmap 显式 recycle** —— 比桌面原来那个"整幅 QImage 常驻"处理得更干净 ✓ |
| 6 | Java `CicadaPlayer.release()` → JNI `nRelease`：`ComposePlayer/cicadaplayer/src/main/jni/player/NativeBase.cpp:1219` → `NativeBase::java_Release` `:350` | `delete player;`（`:357`） | 进入 C++ |
| 7 | `~MediaPlayer`（`mediaPlayer/MediaPlayer.cpp:133`）→ `CicadaReleasePlayer`（`mediaPlayer/media_player_api.cpp:40`） | 删除门面 + `delete pPlayer` → `~SuperMediaPlayer` | **探针：`[mem] CicadaReleasePlayer: destroying the player object ...`** |
| 8 | `SuperMediaPlayer::Stop()`（`mediaPlayer/SuperMediaPlayer.cpp:963-1078`）+ `~SuperMediaPlayer:518 mAVDeviceManager = nullptr` | 包/帧队列、解复用器、数据源（含 http 连接）同步释放；**解码器与它的表面池在 `mAVDeviceManager = nullptr` 时失去最后一个引用** | ✓ 同步 |
| 9 | 视频解码器：`framework/codec/Android/mediaCodecDecoder.cpp:425 close_decoder()` | `mDecoder->stop(); releaseDecoder(); mbInit = false;` ⇒ **MediaCodec 实例 + 它的输出 Surface 引用一起放** | **探针：`[mem] MediaCodec decoder closed: ...`** ✓ 同步 |
| 10 | 音频：`framework/render/audio/Android/AaudioRender.cpp:565 closeStream()` | `streamRequestStop` + `streamClose` | **探针：`[mem] AAudio output stream closed`** ✓ 同步 |
| 11 | 音频（另一条实现）：`framework/render/audio/Android/AudioTrackRender.cpp:44 ~AudioTrackRender()` | 析构存在，随 `mAVDeviceManager` 一起走 | **【未逐行核实】** 其内部是否显式 `AudioTrack.release()`；建议真机确认 |

**与桌面链的对照（差异只有两处，都有利于安卓）**：
* 桌面有"窗口对象 + QML 树 + RHI"这一层（每轮 +25MB 的风险点），**安卓没有**；
* 桌面的退出是"隐藏窗口 + 清片源"，**安卓是退栈即 `release()`**（更接近"关闭就释放"）。

### 1.2 旧示例（`platform/Android/source/paasApp`）

| 跳 | 位置 | 判定 |
|---|---|---|
| 返回键 | `CicadaPlayerActivity.java:765-777`：`onKeyDown` → `releasePlayer(); finish();` | ✓ **显式释放后结束** |
| 生命周期 | `:610-615 onStop` → `mCicadaVodPlayerView.onStop()`（**只是暂停，不释放**）；`:747-754 onDestroy` → `mCicadaVodPlayerView.onDestroy()` | ✓ 与安卓常规一致：`onStop` 暂停、`onDestroy` 释放；**注意：只按 Home 键不释放是预期行为** |
| 另一处 | `SourceChooseActivity.java:510-517 onBackPressed` | 与播放器无关（选源页） |

---

## 2. 大块资源逐项核对

| 项 | 谁持有 | 释放点（file:line） | 是否同步 | 判据（日志 / 现象） |
|---|---|---|---|---|
| **MediaCodec 解码器 + 输出 Surface** | `SMPAVDeviceManager::mVideoDecoder.decoder`（`mediaPlayer/SMPAVDeviceManager.h:24-25,159-160`）→ `mediaCodecDecoder` | `~SuperMediaPlayer:518` → `~mediaCodecDecoder`(`mediaCodecDecoder.cpp:54`) → `close_decoder()`(`:425`)：`stop()` + `releaseDecoder()` | **同步** | **`[mem] MediaCodec decoder closed: codec stopped and released ...`**（本轮新增） |
| **Surface 本身** | 应用层（UI）传给播放器 | Java 侧在 release 前 `player.setSurface(null)`（`CicadaPlayerController.kt:488`） | 同步 | 无崩溃/无"Surface 已释放"报错即可；**【未逐行核实】** 各 ROM 行为 |
| 包队列 / 帧队列 | `BufferController` / `SuperMediaPlayer` 成员 | `Stop()`：`ClearPacket(BUFFER_TYPE_AV)`(`:1026`)、`FlushAudioPath`(`:1029`)、`FlushVideoPath`(`:1034`) | **同步** | `~SuperMediaPlayer` / 探针 |
| demuxer + 数据源（含 http 连接） | `mDemuxerService` / `mDataSource` | `Stop()`：`stop()/close()`(`:1047-1048`)、`CloseStream(...)`、`mDataSource->Close()+delete`(`:1073-1078`) | **同步** | 同上 |
| **解码器 + 硬件表面池（MediaCodec 的 output buffers）** | 见上 | 见上（`mAVDeviceManager = nullptr`） | **同步** | `[mem] CicadaReleasePlayer: ...` + `[mem] MediaCodec decoder closed ...` |
| **音频输出流（AAudio）** | `AaudioRender::mStream` | `closeStream()`(`AaudioRender.cpp:565`)：`streamRequestStop` + `streamClose` | **同步** | **`[mem] AAudio output stream closed`**（本轮新增） |
| 音频输出（AudioTrack 老实现） | `AudioTrackRender` | `~AudioTrackRender()`(`AudioTrackRender.cpp:44`) | 推测同步 | **【未逐行核实】**；若要判据，可在其析构里加一行 AF_LOGI |
| 截图 Bitmap | `CicadaPlayerController.snapshotBitmap` | `releaseSnapshot()`(`:865-868`)：`recycle()` + 置空；在 `release()` 里**先**调用(`:487`) | **同步** | 无内存增长即可；可加 `Log.i` 计数 |
| 弹幕引擎 / 图集 / 缩略图 / `SnapshotImageProvider` | — | **安卓侧不存在这些对象**（它们是 Qt/QML 侧的东西：`DanmakuAtlas`/`ProgressRow`/`SnapshotImageProvider` 都在 `platform/QtPlayer`） | — | 这条是**澄清**：安卓的"弹幕"由播放器 UI 自己画，不在本仓 C++ 侧 |
| 每轮新建的监听器 / `DisposableEffect` | Compose | `DisposableEffect` 的 `onDispose` 会执行（`:577-578`、`VideoPlayerScreen.kt:230-231` 恢复系统栏/方向） | 同步 | `onDispose` 里 `removeObserver`（`CicadaVideoPlayer.kt:423`）✓ |
| `CacheModule` / `ISliceManager` | 框架 | 默认关闭 / 生产路径死代码 | — | 见主文档 §1.6/§1.7 → **0 MB** |

---

## 3. 本轮改动（安卓相关）

| 文件 | 位置 | 改动 | 为什么必须 |
|---|---|---|---|
| `mediaPlayer/media_player_api.cpp` | `:8` 附近 + `CicadaReleasePlayer()`（`:45` 起） | 补 `LOG_TAG`；加一条 **`[mem] CicadaReleasePlayer: destroying the player object ...`** | 这是桌面与安卓**共用的唯一收口点**：一行日志就能判定"播放器对象到底有没有被销毁"。C++ 侧 AF_LOGI，Release 也必定进日志（与桌面那条 QML `console.log` 的坑相反） |
| `framework/codec/Android/mediaCodecDecoder.cpp` | `close_decoder()` `:442-458` | 在 `if (mbInit)` 里、`stop()+releaseDecoder()` 之后加 **`[mem] MediaCodec decoder closed: ...`** | 安卓视频解码器/Surface 释放的唯一直接证据；放在 `if (mbInit)` 内保证"一轮退出只出现一次" |
| `framework/render/audio/Android/AaudioRender.cpp` | `closeStream()` `:585-587` | 在 `streamClose` 调用成功之后加 **`[mem] AAudio output stream closed`** | 音频侧"不留一个打开的输出流"的直接证据；没有该符号时不打，日志不说谎 |

三条都：**没有新增开关**、**没有引入计时器/看门狗/超时兜底**、**没有改任何精度或丢帧逻辑**、
L1（`mediaPlayer/`）里**没有平台宏**（`media_player_api.cpp` 那条是通用代码；两条平台探针在
`framework/codec/Android/`、`framework/render/audio/Android/` 这两个本来就是平台专属的文件里）。

---

## 4. 判据（真机照这个看）

前置：安卓侧框架日志只走 logcat（`frame_work_log.c:311-312` 的 `__android_log_print`），
且 **Release 下 D 级被过滤**（`frame_work_log.c:268-272`，和桌面同一个坑）
⇒ **所有判据都用 I 级日志**（下面这些探针都是 `AF_LOGI`）。

### 4.1 返回键一次（新播放器，非全屏、无面板）

`adb logcat -s CicadaPlayer`（或按你们的 tag 过滤）应看到**一整套、各一次**：

| # | 日志行 | 证明 |
|---|---|---|
| ① | `[mem] CicadaReleasePlayer: destroying the player object (decoder, hardware surface pool, packet/frame queues, demuxer and data source)` | 播放器对象被销毁（收口点） |
| ② | `[mem] MediaCodec decoder closed: codec stopped and released (its output surface reference goes with it)` | **视频解码器 + Surface** 已放 |
| ③ | `[mem] AAudio output stream closed` | 音频输出流已关 |
| ④ | `Player ReadPacket Stop`（框架既有，`SuperMediaPlayer.cpp:982`） | `Stop()` 走到了（包/帧队列/解复用器/数据源同栈释放） |

**判定**：
* ①②③④ 齐全 → 安卓这一轮退出**资源全放**；
* 缺 ① → 根本没走到 `delete player`（检查 `CicadaPlayerController.release()` 有没有被调、
  `DisposableEffect` 有没有被移除 —— 例如把 release 写在 `onPause` 之类的分支里）；
* 缺 ② 而 ① 在 → `close_decoder()` 没走到（`mbInit` 已 false 说明解码器**早就关了**；
  若解码器从未初始化，也属正常）；
* 缺 ③ → 这次会话没用到 AAudio（走的是 AudioTrack 老实现），此时看 `~AudioTrackRender`。

### 4.2 关闭播放器一次

与 4.1 **完全相同**（两条入口在 Compose 里是同一跳：`popBackStack()` → `onDispose` → `release()`）。
**这正是本轮要保证的性质**：两条入口不能有一条只暂停/只隐藏。

### 4.3 反复进出（对应用户的"每轮 +25 MB"复测）

连做 **3 轮**"进入播放 → 播放 10 秒 → 返回"，每轮之后：
1. ①②③④ 各出现一次；
2. `adb shell dumpsys meminfo <包名>` 的 `TOTAL PSS`（或系统设置里的内存）**三轮持平**；
   若逐轮 +20~30 MB，桌面那套"窗口树"嫌疑在安卓不成立，要改查：
   **每轮新建的监听器/回调是否解绑**、**Surface 是否真的摘了**、
   **弹幕/UI 层是否按轮累积**（这些在 App 层，不在本仓 C++ 侧）。

---

## 5. 还没核实、需要真机或读码补的

1. **`AudioTrackRender` 老实现**是否显式 `AudioTrack.release()`（只读到有析构函数，
   没读到内部实现）——若真机上"缺 ③ 但内存仍在涨"，这里要加一行探针。
2. **`premierlibrary` 旧 SDK 内部**（`mCicadaVodPlayerView.onDestroy()` 里到底做了什么）——
   本文只核实了 demo 侧的调用点（`CicadaPlayerActivity.java:747-754`）。
3. **各 ROM 上 `setSurface(null)` + MediaCodec release 的时序**（Android 版本相关的
   "releaseOutputBuffer after release"类告警），需要真机 logcat 对照。
4. 安卓侧**没有**本仓的 Qt/QML 对象，所以桌面文档里"窗口树/图集/pixmap 缓存"三节
   **不适用于安卓** —— 这一点请勿照搬排查。
