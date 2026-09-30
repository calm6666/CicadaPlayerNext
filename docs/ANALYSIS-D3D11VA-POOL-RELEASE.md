# D3D11VA 表面池的释放：顺序、官方契约与验收

本文记录"关窗/切档之后 D3D11VA 那 20 片表面池（1080p ≈62MB、4K ≈249MB）为什么回收不掉、
这一轮按什么顺序改、以及怎么只看日志就能判定它到底有没有回收"。

配套阅读：`docs/ANALYSIS-MEMORY-FOOTPRINT.md`（内存账）、
`platform/QtPlayer/docs/MEMORY.md`（Qt 侧生命周期）、
`docs/ANALYSIS-PLATFORM-RENDERER-FRAME-HOLD-AUDIT.md`（"渲染器握着最后一帧"的同形问题）。

## 1. 官方契约（这一轮所有改动的依据，不是猜的）

三条都来自微软官方文档：

| 文档 | 原文要点 | 对我们的意义 |
| --- | --- | --- |
| [`ID3D11DeviceContext::Flush`](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-flush) | "Direct3D 11 **defers the destruction of objects**. Therefore, an application can't rely upon objects immediately being destroyed. By calling **Flush**, you destroy any objects whose destruction was deferred." 同页给出同步销毁配方："release all its references, call `ClearState`, and then call `Flush`" | **Release 到 0 ≠ 已经回收**。不 Flush，池可能被运行时"延迟销毁"拖着不还 |
| [`ID3D11DeviceContext::ClearState`](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-clearstate) | "sets all input/output resource slots, shaders, input layouts, predications, scissor rectangles, depth-stencil state, rasterizer state, blend state, sampler state, and viewports to NULL" | 这条**不能用**：`m_context` 是 Qt 场景图的立即上下文，清了会和 Qt RHI 自己的状态跟踪对不上（Qt 的 D3D11 后端自己就在跟踪管线，见其 "reset pipeline tracking between passes" 一类修复） |
| [`IDXGIDevice3::Trim`](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgidevice3-trim) | "graphics drivers periodically allocate internal memory buffers... **These memory allocations count against the app's memory usage**... **Calling Trim forces Direct3D to destroy objects immediately**... apps should only call Trim when going idle" | 关窗后不降的那几十 MB 里，有驱动内部缓冲的一份；Trim 是官方给的"空闲时全还"手段 |

另外 [`ID3D11VideoDevice::CreateVideoDecoderOutputView`](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11videodevice-createvideodecoderoutputview)
明确 "The caller must release the interface" —— 我们自己创建的那些 view 必须自己放。

## 2. 为什么"池会被钉住"（D3D11VA 的特殊形状）

FFmpeg 的 d3d11va 把池里 20 片 surface 放进**同一张** `ID3D11Texture2D`（`ArraySize=20`），
每片 surface 的 `AVBufferRef` 都对整张数组 AddRef。所以：

> **只要还有一帧活着（或还有一个视图钉着这张纹理），整张纹理数组就回收不掉。**

"一帧"可能停在这些地方（这一轮之前，切档路径上**没有任何同步释放点**）：

| 持有者 | 什么时候放 | 切档路径上原来有没有释放点 |
| --- | --- | --- |
| `SuperMediaPlayer::mVideoFrameQue` | `FlushVideoPath()` 清 | 有 |
| `AFActiveVideoRender::mInputQueue` + `mRenderingFrame` | `renderFrame(nullptr)` 只是**登记** `mNeedFlushSize`，真正丢帧要等**下一次 VSync 回调** | **没有**（切档只 `decoder->flush()`，连登记都没有） |
| `SdlAFVideoRender::mLastVideoFrame` / `mBackFrame` | `clearScreen()`；原来 `flushVideoRender()`（`renderFrame(nullptr)`）只放 `mLastVideoFrame`，**`mBackFrame` 留着** | **没有** |
| `CicadaVideoRender::m_lastFrame`（截屏缓存） | `clearScreen()` | **没有** |
| QML 组件 `m_currentFrame` / `m_pendingFrame` + D3D11 输入视图 | 下一帧到达时被顶掉，或场景图失效 | 隐式（靠"下一帧"） |

## 3. 这一轮改了什么（按顺序）

### 3.1 内核：新增"关解码器之前同步放帧"的契约

* `framework/render/video/IVideoRender.h`：**在 vtable 末尾**新增
  `virtual void releaseFrames()`，默认空实现（dummy / tunnel / 不用帧的渲染器一行都不用改）。
* `framework/render/video/AFActiveVideoRender.cpp`：实现为
  `mVSync->pause()`（`afThread::pause()` 会**等到安全点**，无超时）→ 放 `mRenderingFrame`
  → 按队列逐帧 `dropFrame()` → `mVSync->start()`。**同步**是这条契约的全部意义。
* `framework/render/video/SdlAFVideoRender.cpp`：实现为在 `mRenderMutex` 下放
  `mLastVideoFrame` + `mBackFrame`；并且 `renderFrame(nullptr)`（既有的 flush 语义）
  也改成**两帧一起放** —— 原来只放前者，`mBackFrame` 会一直留到下一次渲染或析构。
* `framework/render/video/glRender/GLRender.cpp`、`framework/render/video/CheaterVideoRender.cpp`：
  各自覆写（前者同步清 `mInputQueue`，后者放 `mLastVideoFrame`）。
* `mediaPlayer/SMPAVDeviceManager.{h,cpp}`：新增 `releaseVideoRenderFrames()` 转发到渲染器。
* `mediaPlayer/SuperMediaPlayer.cpp`：三个"解码器要死/要重建"的入口都在**动解码器之前**调用它 ——
  `FlushVideoPath()` 最前面、`rebuildVideoDecoder()` 的 `invalidateDecoder()` 之前、
  `CreateVideoDecoder()` 的 `setUpDecoder()` 之前（`setUpDecoder()` 里才是
  `decoder->flush(); decoder->close();`）。

### 3.2 Qt 侧：D3D11 那一层按官方契约补最后两步

* `platform/QtPlayer/src/CicadaTextureD3D11.{h,cpp}`：
  * `flushDeferredDestruction()` = `ID3D11DeviceContext::Flush()`；
  * `trimVideoMemory()` = `IDXGIDevice3::Trim()`；
  * `releaseResources()` 末尾（在把 `m_device`/`m_context` 置空**之前**）按文档顺序执行
    **Release → Flush → Trim**。
  * 刻意不调 `ClearState()`（理由见 §1 表格第三列）。
* `platform/QtPlayer/src/CicadaVideoTexture.{h,cpp}`：转发 `flushDeferredDestruction()`
  （Metal/VAAPI/CPU 三个后端有意为空 —— 它们没有 D3D11 延迟销毁这件事）。
* `platform/QtPlayer/src/CicadaVideoRender.{h,cpp}`：覆写 `releaseFrames()` ——
  先让基类放队列帧，再放掉自己的截屏缓存 `m_lastFrame`，最后通过
  `setDecoderGenerationEndedHook()` 通知 QML 组件"这一代解码器结束了"。
* `platform/QtPlayer/src/CicadaPlayerItem.{h,cpp}`：收到通知只置原子量
  `m_decoderGenerationEnded` + 请求重绘；真正的动作在**渲染线程**的 `updatePaintNode()` 里：
  `releaseInputState()`（放掉对解码纹理持引用的输入视图）+ `flushDeferredDestruction()`。
  **刻意不放 `m_currentFrame`** —— 放了会走"没有帧就删节点"那条分支，切档瞬间画面会空一下。

### 3.3 内核：把"先放池的引用、再销毁解码器"写成显式的

`framework/codec/avcodecDecoder.cpp`（`close_decoder()`）：

1. 打印池的几何/大小（新增）：`pool geometry: 1920x1080 nv12 x 20 slice(s) -> about 62 MB`；
2. 打印引用计数，然后**显式** `av_buffer_unref(&codecCont->hw_frames_ctx)`（池本体那一份引用）
   —— 这一步就是用户要的"先释放 d3d11va，再销毁解码器"；
   安全性：`avcodec_free_context()` 内部（`libavcodec/decode.c:1226` `ff_hwaccel_uninit`）
   本来也会 unref 这一份，`av_buffer_unref` 对 nullptr 是空操作；本仓库自带的
   `libavcodec/d3d11va.c` 全程不读 `hw_frames_ctx`。
3. 再 `avcodec_free_context()`；
4. 最后用探针引用报 `externally pinned=0|1`（判据见 §4），报 1 时把"已经排除了哪些持有者"
   直接写进日志，免得下一轮又从同一批嫌疑里重新找。

## 4. 验收：只看日志怎么判

一轮"起播 → 切档 → 关窗"里，按时间顺序应该看到：

| 日志（关键字） | 期望 | 说明 |
| --- | --- | --- |
| `FlushVideoPath from quality switch immediate (flushRender=0` | 有 | 切档入口 |
| `[mem] releaseFrames: dropped N decoded frame(s) held by the renderer` | 有，紧跟其后 | 渲染器同步放帧（切档路径以前**没有**这一条） |
| `[mem] pool geometry: … -> about NN MB` | 有 | 池有多大 |
| `[mem] dropping the codec context's own reference to the pool (refcount K before` | 有 | 显式先放池引用。**K 才是判据**：K = 解码器自己(1) + 我们探针(1) + 外部帧(N) |
| `[mem] hw frames ctx (the surface pool) refcount=… holders outside the codec context=` | **0**（期望值） | 切档时通常 >0（呈现方那一帧还活着，下一帧就被顶掉） |
| `[mem] surface pool after decoder teardown (codec hevc): holders outside the decoder=` | **0**（目标） | 0 = 池随解码器一起走 |
| `[mem] ID3D11DeviceContext::Flush()` | 有 | 逼延迟销毁的对象当场销毁 |
| `[mem] IDXGIDevice3::Trim() done` | 关窗时应有 | 连驱动内部缓冲一起还 |

> **【探针修正·2026-09-30】** 本节原先写的是 `externally pinned=0/1`，那两条读数是**废的**：
> FFmpeg 的 `av_hwdevice_ctx_alloc()` / `av_hwframe_ctx_alloc()` 建 buffer 时都带
> `AV_BUFFER_FLAG_READONLY`（`libavutil/hwcontext.c:204-206`、`:286-288`），而
> `av_buffer_is_writable()` 对 READONLY 的 buffer **直接返回 0**（`libavutil/buffer.c:147-153`），
> 与引用计数无关 ⇒ `exclusively owned=0` / `externally pinned=1` 是**常量**。
> 现在一律用 `av_buffer_get_ref_count()` 读数（它才是真引用计数）。

**内存判据**（这是最终判据，日志只是过程证据）：

```
grep "player window tree destroyed (~CicadaPlayerItem)" appQtPlayer.log
```

连续 3 轮"开窗 → 播 40s（含一次自动切档）→ 关窗"，取每轮 `private commit`：

* **平（±几 MB）** ⇒ 修复生效，且**证明**此前每轮那 ~85MB 就是解码表面池（+ 其延迟销毁）
  这一笔 —— 也就是"真的是这个问题"；
* 仍然每轮 +60~85MB ⇒ 池不是主项（此时看 `externally pinned=`：若已是 0，说明池每次都回收了，
  要去查 CPU 侧；若仍是 1，说明呈现方那一帧没放，下一刀切在 `CicadaPlayerItem` 的
  `m_currentFrame` 生命周期上，而不是池）。

## 5. 已知边界（别当成漏掉的）

* **Apple / Android / OHOS**：`AFActiveVideoRender` 那份实现（队列 + 正在渲染那一帧）自动生效；
  `GLRender` 也覆写了。Metal / VAAPI 后端没有"延迟销毁"这件事，`flushDeferredDestruction()`
  是有意空实现 —— 哪天真在那些平台量到"引用放了、内存不降"，再往各自后端里补。
* **Trim 只在空闲路径调**（`releaseResources()`：场景图失效 / 关窗 / item 销毁 / 后端重建）。
  切档那种每几秒一次的路径**只 Flush 不 Trim** —— 官方文档明说 Trim 有性能代价，驱动要重新
  分配内部缓冲。
* **切档时 Flush 必须在渲染线程**：立即上下文不是自由线程的，所以通知只置原子量，
  动作留在 `updatePaintNode()` 里做。
* 若某台机器上 `IDXGIDevice3` 拿不到（老驱动），日志里会有一条 `AF_LOGW`，此时只剩 Flush 的
  效果，属于能力缺失而不是回归。

## 6. 实测（2026-09-30，两轮 1080p/512p 混合源）：**池是清白的**

用修正后的引用计数读那一份日志：

| 时机 | 读数 | 含义 |
| --- | --- | --- |
| 第 1 轮切档（切到 896x512 低清，13MB 池） | `refcount 3 before` = 解码器(1) + 探针(1) + **外部 1** | 呈现方（Qt item）手上那一帧还活着 —— 期望如此，下一帧就顶掉 |
| 第 2 轮切档（1920x1152 主池，63MB） | `refcount 3 before` | 同上 |
| 第 1 轮关窗 | `refcount 3 before`（Trim/场景图失效比解码器 teardown **晚 95ms**） | 那一帧 95ms 后随场景图失效放掉，池随后回收 |
| **第 2 轮关窗** | **`refcount 2 before`** = 解码器(1) + 探针(1) ⇒ **外部 0** | 场景图失效发生在解码器 teardown **之前 4ms** ⇒ **池就是随解码器一起释放的（63MB 当场回去）** |

⇒ **D3D11VA 表面池不是"关窗后不降"的那一笔。** 同时这份日志还证明：`afThread` 全部配平
（进程退出时 `live=0`，`CurlEasyManager` + `AsyncJob` 是仅有的两个常驻），`Flush()`/`Trim()`
都按序执行。

而内存账仍然在涨（这正是要继续查的）：

| 时点 | working set | private commit |
| --- | --- | --- |
| 第 1 轮 ctor | 78.9 MB | 95.2 MB |
| 第 1 轮 dtor | 168.7 MB | 185.6 MB |
| 第 2 轮 ctor | 187.3 MB | 189.4 MB |
| 第 2 轮 dtor | 206.6 MB | **239.8 MB** |

第 2 轮只播了约 20s ⇒ **约 2.7MB/s、且随播放时长线性增长**（第 1 轮 56s 约 1.6MB/s）——
这是"按时间/按帧/按分片分配"的形状，不是"一次开关漏一块"的形状。

### 6.1 下一把刀：存活对象快照（已加，等一轮实测）

新增探针（都是只读计数，不改行为）：

* `framework/demuxer/avFormatDemuxer.cpp`：在**分片边界**（open 成功 / Close）打印
  `[mem] live entities @open ok|close (this=… <path>): avFormatDemuxer=… (created/destroyed) AVFormatContext live=… (alloc/close) | AVAFFrame=… AVAFPacket=…`。
  这一轮日志里 HLS 那条路 **15 秒内重建了 30 次内层 demuxer**（`avFormatDemuxer created/destroyed` 成对刷屏），
  所以"每分片漏一点"是当前头号嫌疑；
* `framework/base/media/AVAFPacket.cpp`：`AVAFFrame` / `AVAFPacket` 存活计数（框架里唯一的帧/包包装，
  硬解帧的 `buf[]` 里就握着解码器输出缓冲）；
* `platform/QtPlayer/src/CicadaPlayerItem.cpp`：ctor/dtor 那行内存读数**加上句柄数与线程数**
  （`GetProcessHandleCount` + `CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD)`），
  对应此前一直没归因的"+27 Thread / +116 无名句柄每轮"。

判读方式（下一轮日志里直接对齐）：

* 存活计数**跟着 commit 一起涨** ⇒ 就是这一类对象没放（帧/包/容器上下文/内层 demuxer），
  顺着 `this=` 指针与 `created/destroyed` 的差值定位到具体路径；
* 存活计数**平、而 commit 仍涨** ⇒ 与框架无关，去查呈现方那一侧（弹幕层 / 预览图 / QML 缓存 / 驱动），
  最快的二分是**关掉弹幕跑一轮**（当前日志里 1000 条 mock 弹幕 + GPU item 一直在跑）。

