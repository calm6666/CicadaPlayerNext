# 全平台同形审计：渲染侧握着"最后一帧"导致解码输出池迟迟不放

> 起因：Qt（Windows）侧已定案一个缺陷形状 —— **渲染侧在关闭/切走之后仍握着最后一帧
> （或它的纹理/输入视图），于是解码器虽然析构了，解码输出池（D3D11VA 的 20 片表面数组
> ≈62MB@1080p）还被那一帧钉着；释放被推迟到"下一次播放"或干脆到进程结束。**
> Qt 侧证据：`surface pool after decoder teardown (codec hevc): externally pinned=1` 每次关窗都成立，
> 而 `render thread released the previous frame and the D3D11 input view` **只在开窗后出现、关窗时刻没有**。
>
> 本文按**同一形状**扫其它平台，给出结论、修复点与验收判据。
> 主文档：`docs/ANALYSIS-MEMORY-FOOTPRINT.md`（Qt/Windows 侧）；安卓释放链：`docs/ANALYSIS-ANDROID-RESOURCE-RELEASE.md`。
> 标注：【已证实】= 读到了那段代码；【待确认】= 证据不足，给出测法，**没有瞎改**。

---

## 0. 形状与引用方向（判定的唯一标准）

```
          渲染侧持有者（"最后一帧"/纹理/layer/surface）
                    │  持有一帧（或帧的 buf[]）
                    ▼
            解码帧 AVFrame::buf[]
                    │  引用
                    ▼
        解码输出池（hw frames ctx / MediaCodec output buffers / CVPixelBufferPool）
                    │  引用
                    ▼
            解码设备 / 驱动侧分配
```
**同形缺陷的判据只有一条**：在"关闭 / 换源 / 清屏"这条路径上，
**释放顺序是"解码器先死、渲染侧后放"**（或渲染侧根本不在关闭路径上放），
于是池的寿命由**渲染侧那一帧**决定，而不是由解码器决定。

反过来，"不同形"的证明也必须落到顺序或释放点上：
**渲染侧在关闭路径上（同一跳、且早于解码器析构）把自己那一帧放掉**。

---

## 1. 审计表

| 端 | 渲染侧持有者（file:line） | 关闭路径上的释放点 | 是否同形 | 修复点 / 状态 |
|---|---|---|---|---|
| **Qt / Windows**（本次定案） | `CicadaPlayerItem::m_currentFrame`（渲染线程那一帧）+ `CicadaTextureD3D11::m_inputView`（视频处理器输入视图） | 原来只有 `updatePaintNode()` 的 `m_releaseRenderState` 分支（需要"再画一帧"）；`onSceneGraphInvalidated()` 不放帧 | **同形** | **已修**：`platform/QtPlayer/src/CicadaPlayerItem.cpp` 的 `onSceneGraphInvalidated()`（关窗时必然在渲染线程跑一次的那一跳）里补上 `m_pendingFrame/m_currentFrame` 释放 + `releaseInputState()`；析构里另有第二道（帧先死、`destroyPlayer()` 后死） |
| **桌面非 Qt（SDL：cmdline / demo）** | `SdlAFVideoRender::mLastVideoFrame`（`SdlAFVideoRender.h:108`）、`mBackFrame`（`:109`） | `clearScreen()`（`SdlAFVideoRender.cpp:148`）原来**只** `mBackFrame = nullptr`；`mLastVideoFrame` 全仓无清零点（grep 无 reset），只能等下一次 `renderFrame` 换掉或渲染器析构 | **同形**（已证实） | **已修**：`clearScreen()` 里一并放掉 `mLastVideoFrame`（`setDiscard(true)` + 置空），与 `mBackFrame` 同一把 `mRenderMutex` 保护 |
| **桌面非 Qt（headless / "Cheater"）** | `CheaterVideoRender::mLastVideoFrame`（`CheaterVideoRender.h:68`） | `onVSync()` 里 `mLastVideoFrame = nullptr`（`CheaterVideoRender.cpp:78`） | **形状相同但释放点存在**（每次 VSync 放） | **未改**。注意该 .cpp 疑似**未完成/未编入**：`:76` 引用了未定义的 `frame`，文件仅 80 行 —— 【待确认】它是否参与构建；若不参与，本条不构成风险 |
| **Android（Compose / premierlibrary）** | `AFActiveVideoRender::mRendingFrame`（`AFActiveVideoRender.h:75`）；MediaCodec 输出缓冲由解码器自己按帧 `releaseOutputBuffer` | `mRendingFrame` 每次渲染后即置空（`AFActiveVideoRender.cpp:149-152`），flush 路径也置空（`:99-101`）；`CicadaPlayerController.release()` 先 `player.setSurface(null)` 再 `player.release()`（`CicadaPlayerController.kt:488-489`） | **不同形**（已证实：渲染侧放手在解码器销毁之前，且 `mRendingFrame` 不是长期持有者） | 不改。判据见 §2.1 |
| **Android：MediaCodec 侧"旧代帧"** | 解码器内部的渲染门（`mRenderHold`/`mRenderGatePts`/`mRenderGateHit`）+ 渲染器队列里可能压着的旧代帧 | `invalidateFrameReleases()` 作废旧代（`mediaCodecDecoder.cpp:428-438`），`close_decoder()` 里 `stop()+releaseDecoder()`（`:442-458`） | **不同形**（已证实：作废旧代是显式动作，且发生在 stop 之前） | 不改 |
| **OHOS** | OHOS 视频渲染（`OH_VideoDecoder` 输出到 surface；渲染侧持有 surface/输出缓冲） | — | **【待确认】**：本次没有读到 OHOS 视频渲染侧"持有最后一帧"的成员（`OhosAVCodecDecoder` 走的是 `AVAFFrame` 平面拷贝路径，见 `OhosAVCodecDecoder.cpp:499/518`） | **未改**。测法见 §2.2 |
| **Apple（VideoToolbox / AVSampleBufferDisplayLayer）** | `SampleDisplayLayerRender.mm` 里的 `AVSampleBufferDisplayLayer`（Apple 语义：**已入队的 sample buffer 由 layer 保留，直到 flush/替换**）+ `CVPixelBufferPool` | — | **【待确认】**：需要确认 stop/close 时是否 `flush` 图层、是否把 pool 置空 | **未改**。测法见 §2.3 |

---

## 2. 各端验收办法（不依赖我们跑命令）

### 2.1 Android（已判定不同形，做一次确认即可）
1. 进播放页 → 播 10 秒 → 返回（或点关闭播放器）；
2. logcat 过滤本轮加的探针，应各出现一次：
   `[mem] CicadaReleasePlayer: destroying the player object ...`、
   `[mem] MediaCodec decoder closed: codec stopped and released ...`、
   `[mem] AAudio output stream closed`（走 AAudio 时）；
3. `adb shell dumpsys meminfo <包名>` 看 `TOTAL PSS`：**进出 3 轮应持平**；
4. 若逐轮增长，且上述三条齐全 ⇒ 泄漏不在解码输出侧，改查 App 层（UI/弹幕/缩略图）
   —— 详见 `docs/ANALYSIS-ANDROID-RESOURCE-RELEASE.md` 的判据表。

### 2.2 OHOS【待确认】
需要确认两件事，任一为"是"则同形：
1. **渲染侧是否持有解码输出**：在 OHOS 视频渲染实现里查是否有"最后一帧/surface"成员
   （对照 Qt 的 `m_currentFrame`、SDL 的 `mLastVideoFrame`）；
2. **关闭顺序**：`stop()/close()` 时是渲染侧先放 surface/frame，还是解码器先 `release`。
测法：播放 → 停止（不退出应用）→ 用系统内存工具看进程内存是否**立刻**回落；
若只在"下一次播放"或退出时回落 ⇒ 同形。拿到证据后按 Qt 的修法处理
（在"关闭时必然执行的那一跳"上先放渲染侧，再销毁解码器）。

### 2.3 Apple【待确认】
1. **layer 是否保留**：`AVSampleBufferDisplayLayer` 对已入队 sample buffer 有保留语义；
   若 stop 时不 `flush`（`-[AVSampleBufferDisplayLayer flush]`）也不置空，
   则最后一帧（及其 CVPixelBuffer）会活到下一次播放；
2. **pool 的持有者**：`CVPixelBufferPool` 是否被渲染侧或某个单例缓存。
测法：播放 → 停止 → 看 `CVPixelBufferPool` 的分配是否回落（Instruments 的 Allocations
按 `CVPixelBuffer`/`IOSurface` 过滤），或看进程内存是否只在下次播放时回落。
【待确认】之前不改：VideoToolbox 那条路还涉及 `CVPixelBuffer` 与 Metal 纹理的
IOSurface 生命周期，改错会直接黑屏。

---

## 3. 本轮改动点

| 文件 | 位置 | 改动 | 对应形状 |
|---|---|---|---|
| `platform/QtPlayer/src/CicadaPlayerItem.cpp` | `onSceneGraphInvalidated()` | 关窗必然执行的那一跳里放掉"渲染线程那一帧 + 输入视图"（原来一个帧都不放） | Qt 侧定案的形状（已修） |
| `framework/render/video/SdlAFVideoRender.cpp` | `clearScreen()` | 原来只放 `mBackFrame`，现在一并放掉 `mLastVideoFrame`（`setDiscard(true)` + 置空） | 桌面非 Qt（SDL）同形（已修） |

约束遵守：无配置开关、无计时器/看门狗/超时兜底、不"精度换流畅"、
`mediaPlayer/` 与 `framework/` 非平台代码**没有新增平台宏**、
未新增虚函数与类成员、注释不含星号加斜杠。

## 4. 需要编的目标（用于验证本轮改动）

| 目标 | 覆盖的改动 |
|---|---|
| `CicadaPlayerNext` 的 **Qt 播放器**（`platform/QtPlayer`，Windows/MSVC） | `CicadaPlayerItem::onSceneGraphInvalidated()` |
| **framework 库**（`framework/`，被所有平台共用） | `SdlAFVideoRender::clearScreen()`（该文件只在启用 SDL 渲染时编入；若本机未启用 SDL，改动不影响 Qt 播放器） |
| 可选：**cmdline demo**（`cmdline/`） | 验证 SDL 渲染器那条路径（若能开 SDL 窗口） |

> 说明：本轮**没有**改 OHOS 与 Apple —— 那两端的"渲染侧是否持有最后一帧/输出池"
> 缺证据，按约定标【待确认】并给出测法，不瞎改。
