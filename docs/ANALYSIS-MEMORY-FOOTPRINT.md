# 内存占用分析：173 / 158 / 141 / 105 MB 是怎么来的

> 现象（用户实测，Windows，任务管理器「内存」列 = **私有工作集 / private working set**）：
>
> | 时刻 | 内存 |
> |---|---|
> | 只开主窗口（首页，无播放） | 25 MB |
> | 打开 DASH 视频后（峰值） | 173 MB |
> | 播一会儿稳定 | ~158 MB |
> | 暂停（弹幕 + 分段进度条 + 预览图都开着） | ~141 MB |
> | **关闭播放器窗口之后** | **~105 MB** |
>
> 本文的回答顺序：现象 → 逐项归因（字节算术 + file:line）→ 关窗后 105MB 的去向 →
> 主流播放器参照 → 降内存方案（收益/风险）→ 需要补的测量。
>
> 标注约定：
> * **【已证实】** = 本会话真的读到了那段代码/那行日志，给出 file:line；
> * **【推测】** = 由代码结构 + 公开行为推出来的，没有直接证据；
> * **【未核实】** = 需要实测才能定论（本文最后列出该怎么测）。
>
> 本会话 shell 不可用（sandbox 拒绝命令），所以**没有任何新的实测数据**；
> 下面所有数字要么是代码里的常量，要么是按「字节 = 宽 × 高 × 每像素字节」直接算出来的算术。
> 仓库里已有一份**实测**记录（`platform/QtPlayer/docs/MEMORY.md`），本文引用它的地方会写明。

---

## 0. 先说结论

1. **【本轮重大更正】"~105/107 MB 是进程地板"这个结论是错的。**
   用户给出关键事实：**只开主窗口、什么都不做 = ~25 MB**。⇒ 关掉播放器窗口后应该回到
   **≈25 MB**，而实测停在 107 MB，即**有 ~82 MB 没释放**；并且每开/关一轮再涨 ~25 MB
   （107 → 132 → 156）。据此把目标与结论改成：
   * **目标：关窗后回到 ≈25 MB 量级**（允许小幅偏差），且**每轮开关不得逐轮爬升**；
   * 结论拆成两类：**每轮累积（≈25 MB/轮）** 与 **一次性残留（≈50–60 MB）**，
     逐项清单见 §2.6，判据见 §2.7。
   * **【最新实测（带本轮修复+探针的版本）】**：第 1 轮关窗后 **~84 MB**（比修复前的 107 少 ~23 MB，
     说明有一块确实放掉了），第 2 轮 **~114 MB**（**+30 MB/轮**）。对应关系与一条命令定性见 **§2.6.4**：
     **59 MB ≈ 1080p D3D11VA 表面池 62.2 MB**（优先查"解码器对象到底死没死"），
     **+30 MB/轮 ≈ 一个播放器窗口树 + RHI**（用析构行的 `top-level windows alive=N` 判定是它还是别的）。
2. **为什么上一版会得出"无泄漏"**（三处错误，都要记住）：
   * 我引用的 `platform/QtPlayer/docs/MEMORY.md` §6 那份实测，**脚本是自己 `createObject` +
     `deleteLater()` 销毁窗口的**，因此**从来没有走过 `HomeWindow` 的销毁那一跳**；
     它证明的是"播放器资源（表面池/队列）每轮不累积"（今天仍然成立），
     **不是**"窗口对象每轮被销毁"。而且它自己的地板数字是 **118 → 127 → 130 MB**（在涨），
     当时被总结成了"回到同一水平"。
   * 我把"代码里存在 `reapPlayerWindow()`"当成了"它会被调用"。**存在 ≠ 被执行**：
     它的触发条件依赖 `visible`，而那是个平台相关的读数。
   * **最要命的一条：我加的两条窗口探针用的是 QML `console.log`，它在 Release 构建里
     进不了日志** —— 所以"两条窗口探针都没打"**不能**当作"窗口树没销毁"的判据；
     同理，"播放器侧 6 条齐全 ⇒ 无泄漏"也不能当结论。完整推导见 §2.6 开头。
3. **逐项大头（1080p，按代码里的容量常量算）**：
   | 排名 | 项 | 量级 | 性质 |
   |---|---|---|---|
   | 1 | FFmpeg D3D11VA 解码表面池（20 片一个 Texture2D Array） | **62.2 MB** | **一次性残留的头号嫌疑**（§2.6） |
   | 2 | Qt Quick 播放器窗口树 + 场景图/RHI（D3D11 字形图集、交换链、管线缓存） | **~25 MB** | **每轮累积的头号嫌疑**（与实测 +25 MB/轮 吻合） |
   | 3 | 预览图（`previewFrames` + 悬停后进 Qt 全局 pixmap 缓存的解码位图） | 4–29 MB | **按用户要求保持默认**；但其**进程级**增长要如实计数（§2.6） |
   | 4 | D3D11 零拷贝输出 RGBA 纹理 | 8.29 MB（4K 33.18 MB） | 独占一张，关窗时 Release（§2.1 跳 8） |
   | 5 | CPU 回退 swscale 目标位图 `m_bgraImage` | 8.29 MB（4K 33.18 MB） | 只在 CPU 路径，已改为场景图失效即释放（§4.2） |
   | 6 | 15 秒包缓冲（1080p ~3 Mbps） | 4.7–6.6 MB | 已从 40s 收到 15s（§1.1） |
   | 7 | 弹幕（条目 + 1024×1024 图集 4.19 MB QImage + 同尺寸 GPU 纹理） | ~8–15 MB | 有界；**随窗口树销毁** → 窗口树不销毁时它跟着泄漏 |
   | 8 | 整幅截屏快照 `m_lastSnapshot` | 8.29 MB（4K 33.18 MB） | **已改成"按需生成、用完即还"**（§4.1） |
   | 9 | 解码音频 PCM 队列 / curl 环形缓冲 / AVIO 缓冲 | < 1 MB | 可忽略 |
   | 10 | `CacheModule` 播放缓存 / `ISliceManager` 100 MB 切片池 | **0 MB** | 前者默认关闭、后者生产路径死代码（§1.6/§1.7） |
4. **本轮改动**（桌面）：把"关窗即销毁本轮窗口对象及其 QML 树"做成**确定性**的
   （触发点从 `visibleChanged` 改成 `closing` 驱动的 `closeCommitted`，并**删掉 `visible` 闸**）；
   把窗口销毁探针搬到 **C++ 侧**（`QQuickWindow::destroyed` + `AF_LOGI`，必定进同一份日志）；
   新增**每轮内存读数探针**（窗口树建立/销毁各一行，直接给 MB 数）与
   `CicadaReleasePlayer` / MediaCodec / AAudio 三处释放探针。
   逐项与 file:line 见 §2.6 与附录 A；**安卓**两条入口（返回键 / 关闭播放器）的审计见
   `docs/ANALYSIS-ANDROID-RESOURCE-RELEASE.md`。

---

## 1. 逐项内存归因

### 1.1 读前后缓冲（packet buffer）

**容量常量（file:line）**

| 项 | 值 | 位置 |
|---|---|---|
| `START_BUFFER_DURATION_DEFAULT` | 1 s | `mediaPlayer/player_types.cpp:16` |
| `HIGH_BUFFERING_LEVEL_DEFAULT` | 5 s | `mediaPlayer/player_types.cpp:17` |
| `MAX_BUFFER_DURATION_DEFAULT` | **40 s** | `mediaPlayer/player_types.cpp:18`（`reset()` 里赋给 `maxBufferDuration`，`:31`） |
| C 门面 config 的另一份默认 | 50000 ms | `mediaPlayer/MediaPlayerConfig.cpp:16`（注意和上面那份 40s **不是同一个结构体**，见下方"两个默认值"） |
| `MAX_VIDEO_READ_AHEAD_US` | **2 s**（微秒） | `mediaPlayer/SuperMediaPlayer.cpp:110` |
| `MAX_VIDEO_DECODER...` 无用 | — | — |
| `AUDIO_FRAME_QUEUE_FLOOR` | 9 帧（PCM） | `mediaPlayer/SuperMediaPlayer.cpp:117` |
| demuxer 内部 FIFO `MAX_QUEUE_SIZE` | 60 个包（注释：约 500 ms 音视频包） | `framework/demuxer/avFormatDemuxer.h:166`，阻塞点 `avFormatDemuxer.cpp:855-856` |
| `VIDEO_PICTURE_MAX_CACHE_SIZE` | 2 帧 | `mediaPlayer/SuperMediaPlayer.cpp:39` |
| `CICADA_HW_SW_FRAME_POOL` | 6（copy-back 下载槽） | `framework/codec/avcodecDecoder.h:76` |

**Qt 侧对 DASH/HLS 的实际覆盖（这就是日志里那句 `buffer capped at 15s`）**
`platform/QtPlayer/src/CicadaPlayerItem.cpp:3145-3154`：

```
const bool segmentStream = !m_manifestJson.isEmpty() || isSegmentPlaylistUrl(m_source);
if (segmentStream) {
    SetOption("maxBufferDuration",      "15000");   // 15 s
    SetOption("highLevelBufferDuration","5000");    //  5 s
    SetOption("startBufferDuration",    "1000");    //  1 s
    AF_LOGI("segment stream (DASH/HLS): buffer capped at 15s ...");
}
```
另外两条相邻策略：本地文件 5 s / 3 s / 0.5 s（`:3173-3179`），连续流直播（HTTP-FLV/RTMP）
4 s / 2 s / 0.5 s（`:3223-3229`）。

**单位换算是真的**：`SuperMediaPlayer.cpp:1196-1202` 把 `maxBufferDuration` 当**毫秒**收下
（`mSet->maxBufferDuration = int64_t(duration) * 1000`），所以 15000 → 15,000,000 µs = **15 秒**。
（`highLevelBufferDuration` 同一写法，`:1185-1193`。）

**这两个默认值是哪一份在起作用？** 【推测，倾向 `player_types.cpp`】
`SuperMediaPlayer` 用的是 `player_type_set mSet`（`SuperMediaPlayer.h`），它的构造函数走
`reset()`（`player_types.cpp:27-31`）→ 40 s。`MediaPlayerConfig`（50 s）是 C 门面
`MediaPlayer` 那条路用的配置结构（`MediaPlayer.cpp:556-572` 会做 clamp）。
两者都被 Qt 侧的 `SetOption("maxBufferDuration","15000")` 覆盖，所以 **15 s 是确定生效的**；
但文档/注释里"框架默认 40 秒"和"50 秒"两个说法并存，如果以后要写文档请以 40 s（内核实际用的那份）为准。

**存的是什么、存在哪**
* `BufferController` 里三条队列（视频/音频/字幕），成员就是三个 `MediaPacketQueue`
  （`mediaPlayer/buffer_controller.h:96-98`）。
* `MediaPacketQueue` 内部是 `std::deque<std::shared_ptr<IAFPacket>>`（见
  `media_packet_queue.h` 与 `media_packet_queue.cpp:21-29,71`）。
  **视频里存的是压缩包（AVPacket），不是解码后的帧** —— 所以它的大小 = 码率 × 秒数。

**上限是怎么执行的**
* 主上限：`SuperMediaPlayer.cpp:2382-2397`，`cur_buffer_duration > mSet->maxBufferDuration`
  就置 `mBufferIsFull = true` 并 `break`（停止读包），注释明确写了"不设任何例外"。
* 读前闸门（日志里 `read-ahead gate … queued=…` 的来源）：`SuperMediaPlayer.cpp:2282-2369`。
  三个条件同时成立才停读：① 不在切档途中；② 队列里已经 ≥ 2×2 s = 4 s（`:2324`）；
  ③ 音频队列 ≥ 2 s（`:2325-2326`）；④ 队首归一化后比主时钟超前 > `MAX_VIDEO_READ_AHEAD_US`
  = 2 s（`:2351-2357`）。日志行就是 `:2358-2365` 那条 `AF_LOGW`。
* 历史上那个"48 s / activeQ=2923"是**上限被执行之前**的状态：同一份代码里留了两条同源事故记录 ——
  切档在途时上限被豁免，队列被读到 **6978 个包 ≈ 233 秒的 4K 内容**
  （`SuperMediaPlayer.cpp:2372-2381` 的注释），以及 `activeQ=120 queued=2001 ms`（`:2312-2313`）、
  `activeQ=96 queued=4003 ms`（`:2337-2338`）。现在这两条路都被堵掉了。

**为什么 DASH/分段流要被 cap 到 15 s**（`CicadaPlayerItem.cpp:3118-3144` 的注释是权威依据）
1. **换档作废**：DASH/HLS 换 Representation 后旧档读进来的包按旧码率/旧分片号全要丢
   （日志里的 `dropped 2761 stale video packets of the retired stream at commit`），
   缓冲越厚这一下白扔越多；
2. 15 s ≈ 1.5 个 10 s 分片（或 3.6 个 4.17 s 分片），够吃一次网络抖动；
3. 15 s 是**和 ABR 阈值配套**的：上切要求缓冲 ≥ 8 s、连续低于 7 s 才降档
   （`mediaPlayer/abr/AbrBufferAlgoStrategy.cpp:115` 引用这个 15 s，`:437-446` 读它当 ABR 输入）。
   改这个值必须同时看 ABR，否则会出现"永远够不到上切门槛"或"降档太迟钝"。

**字节算术（1080p，15 s 上限）**

| 码率 | 15 s 压缩缓冲 | 备注 |
|---|---|---|
| 1.5 Mbps（HEVC 1080p 典型） | **2.8 MB** | B 站 HEVC 1080p 常见档 |
| 3 Mbps（AVC 1080p 典型） | **5.6 MB** | 最常见 |
| 6 Mbps（1080p60 高码率） | **11.3 MB** | |
| 15 Mbps（本地高码率） | **28.1 MB** | 但本地文件走的是 5 s 策略 → 9.4 MB |

音频（128–320 kbps）15 s = 0.24–0.6 MB。
**所以视频包缓冲在用户这个场景里只有约 3–7 MB，不是 173→158 的大头。** 【算术已证实，码率是推测】

补充：`READ-AHEAD 4 s` 是闸门的下限（不是上限），所以稳态队列长度在 4–15 s 之间抖动 ——
这解释了 173(峰值,缓冲填满/刚起播) → 158(稳态) 那 15 MB 的一部分。

### 1.2 解码 / 帧队列

* **视频帧队列上限 = 1 或 2 帧**。`doDeCode()`（`SuperMediaPlayer.cpp:3677-3689`）：
  `max_cache_size = VIDEO_PICTURE_MAX_CACHE_SIZE`(2)，而 `mPictureCacheType == picture_cache_type_cannot`
  时是 **1**；赋值在 `:7598`
  （`mPictureCacheType = IsVideoDecoderHardware() ? picture_cache_type_cannot : picture_cache_type_soft`）。
  → **硬解 1 帧、软解 2 帧**。这条很重要：**播放器层没有"帧堆积"这个内存项**。
* `mVideoFrameQue` 是 `std::queue<unique_ptr<IAFFrame>>`（`SuperMediaPlayer.h:779`），
  `mAudioFrameQue` 是 `std::deque`（`:789`），音频下限 9 帧（`SuperMediaPlayer.cpp:3781`）。
* **帧是引用计数不是深拷贝**：`avcodecDecoder.cpp:966` 走的是
  `new AVAFFrame(outFrame)` → 命中 `AVAFPacket.cpp:208-212`
  的 `av_frame_clone(frame)`（= `av_frame_ref`，只加引用）。
  唯一真正 `memcpy` 整帧像素的是 `AVAFPacket.cpp:170-197`（`(info, data, lineSize, lineNums, type)`
  这个构造），**只有 Android MediaCodec / OHOS 路径在用**（`mediaCodecDecoder.cpp:847`、
  `OhosAVCodecDecoder.cpp:499,518`），Windows 不走。所以"软解把整帧 RGBA/NV12 常驻"这件事
  **在本仓的 Windows 路径上不成立**。**【已证实】**
* 软解路径真正的大头是 **FFmpeg 自己的 DPB（参考帧缓冲）**，它在 codec context 里，由
  `max_dec_frame_buffering` 决定，应用侧不参与。1080p H.264 按 4–8 个参考帧估：
  4–8 × 1920×1080×1.5 = **12–25 MB**；4K 按 4–8 帧 = **50–100 MB**。**【推测，量级来自 FFmpeg 常识】**
* **copy-back 下载池**：`retrieveHwFrame()`（`avcodecDecoder.cpp:594-672`）轮转使用
  `swFrames[6]`：
  1080p NV12 = 1920×1080×1.5 = 3,110,400 B × 6 = **17.8 MiB**；
  4K NV12 = 12,441,600 B × 6 = **71.2 MiB**。
  **零拷贝直通时这条池子不填**：`avcodecDecoder.cpp:926-937`，`format == hwPixFmt && directOutput`
  就直接把 GPU 帧交出去（"一个字节都不下载"）。所以它只在
  `video.render.hw.direct_texture` 为 OFF 时才付出。
* **D3D11VA 解码表面池**（最确定的一笔大数）：20 个 surface 在**同一个**
  `ID3D11Texture2D` 的 ArraySize=20 里（`MEMORY.md` §2 引 FFmpeg
  `libavcodec/dxva2.c:619-634` + `decode.c:1108` + `libavutil/hwcontext_d3d11va.c:299-330`）：
  * 1080p NV12：1920×1080×1.5 × 20 = 62,208,000 B = **59.3 MiB ≈ 62 MB**
  * 4K NV12：3840×2160×1.5 × 20 = 248,832,000 B = **237.3 MiB ≈ 249 MB**
  * 4K P010(10bit)：再翻倍 ≈ 498 MB
  关键性质（代码注释也写了）：**每一片 surface 的 AVBufferRef 都对整张数组 AddRef**，
  所以"还有一帧活着"= "整池还活着"。仓里为此专门修过一次：`CicadaPlayerItem` 释放
  `m_currentFrame` + `CicadaTextureD3D11::m_inputView`（`CicadaPlayerItem.h:1114-1125`、
  `CicadaPlayerItem.cpp:3293-3311`、`CicadaTextureD3D11.cpp:207-223`）。
  **它算不算"任务管理器内存"**：这是 WDDM 记账问题 —— 表面上它属于显存（任务管理器
  「专用 GPU 内存」），但 WDDM 会把 D3D 分配也计入进程提交。**【未核实】**：
  用户看到的 158 MB 里到底含不含这 62 MB，决定了后面所有结论的基线，**这是最该先量的一件事**（§5）。

### 1.3 零拷贝纹理（D3D11）

* 后端：`platform/QtPlayer/src/CicadaTextureD3D11.{h,cpp}`，由
  `CicadaVideoTexture::prepare()`（`CicadaVideoTexture.cpp:104-114`）探测、`m_zeroCopyActive` 开关。
* **只有一张输出纹理**：`CicadaTextureD3D11.h:158 m_outputTexture`，尺寸 = 视频显示尺寸
  （`ensureOutputTexture`）：1920×1080×4 = **8.29 MB**，3840×2160×4 = **33.18 MB**。
  不是 3 张、不是池 —— **1 张**。**【已证实】**
* 输入侧不额外占内存：`m_inputView` 是 `ID3D11VideoProcessorInputView`，只是**引用**解码纹理
  （所以它才是"钉住整池"的那个东西，见 `CicadaTextureD3D11.h:91-104`）。
* 谁持有：`CicadaPlayerItem::m_textureBackend`（`CicadaPlayerItem.h:1053`）→ `CicadaVideoTexture`
  → `m_d3d11`。由场景图失效释放。
* **关闭时是否释放：会。**
  `CicadaPlayerItem::onSceneGraphInvalidated()`（`CicadaPlayerItem.cpp:3434-3455`）→
  `m_textureBackend->releaseResources()`（`:3439-3441`）→
  `CicadaVideoTexture::releaseResources()`（`CicadaVideoTexture.cpp:330-381`）→
  `CicadaTextureD3D11::releaseResources()`（`CicadaTextureD3D11.cpp:225-271`，逐个 Release
  输出视图/输入视图/处理器/枚举器/输出纹理/VideoContext/VideoDevice）。
  另外 `~CicadaPlayerItem`（`:722-738`）再兜一遍。
* **关于 `~CicadaTextureD3D11()`（`CicadaTextureD3D11.cpp:108-115`）那个"故意什么都不 Release"
  的空析构**：注释写"真漏了也只是进程退出时的一次引用计数残留，不会崩"。
  本次审计**证明了它在真实关窗路径上不会导致纹理不回收**（三条代码事实见 §2.1 末段）：
  `m_textureBackend` 永远不会被重新赋值，而 `~CicadaPlayerItem` 一定**先**调
  `releaseResources()` 再析构成员，所以 `~CicadaTextureD3D11()` 跑起来时 8 个 COM 指针全是 nullptr。
  结论：**不改它**（改它会把"漏而不崩"变成"渲染线程还在用就 Release"）。**【已证实】**

### 1.4 预览图 / 分段进度条（用户明确提到的"预览图"）

有**两条**互斥的路，都在 `controls/ProgressRow.qml`：

**(a) 接口位置帧（在线，优先，`previewFrames`）—— 这是常驻的那一份**
* 取数：`Main.qml:775-786` 拉 `videoshot/preview.bin`，按 `\u001F` 切分 →
  `playerView.previewFrames = frames`（一个**全片每 5 秒一帧的 data URL 数组**）。
* 透传链：`Main.qml` → `PlayerView.qml:90` → `PlayerControlBar.qml:93` → `ProgressRow.qml:60`。
* 展示：`ProgressRow.qml:699-720`（下标 = `floor(悬停秒/5)+1`）+ `:755-763` 那个 `Image`：
  `sourceSize: Qt.size(160,90)`、`asynchronous: false`、**没有 `cache: false`**
  → 每张被悬停过的帧都会被 Qt 解码并放进**全局 pixmap 缓存**（key = data URL）。
* **所以内存 = ① 全部 data URL 字符串本身（常驻，跟片长线性）+ ② 被悬停过的解码位图（有上限）**：
  * ① 一帧 160×90 的 JPEG 大致 2–6 KB，base64 后 ×1.37 ≈ 3–8 KB；QV4 的字符串是 UTF-16
    （2 字节/字符）→ **6–16 KB/帧**。30 分钟片（360 帧）= **2.2–5.8 MB**；
    2 小时片（1440 帧）= **8.6–23 MB**。**【推测：帧大小来自本地 mock（注释写明 mock 是 160×90）】**
  * ② 每张解码后 160×90×4 = 57,600 B；全部 360 帧都悬停过 = 20.7 MB，
    但受 Qt 全局缓存上限约束（本工程**没有**任何地方调 `QPixmapCache::setCacheLimit`
    —— 全仓 grep 无命中，所以走 Qt 默认值；QQuickPixmapCache 的具体默认上限本会话
    网络受限、**未能从 Qt 源码核实**）。**【推测】**
* 重要：这一份**不随"暂停/停止"释放**，只随播放器窗口销毁释放（因为它是 PlayerView 的属性）。
* **【按用户要求：这一项保持默认、不做任何改动】**——不加 `cache: false`、不改成按需取档。
  它随窗口销毁释放（§2.1 跳 5/10），不构成"关窗不释放"。

**(b) 本地兜底帧（接口不可用时）= 框架 `CaptureScreen` 快照**
* `ProgressRow.qml:731-733, 769-778`：URL = `image://snapshot/<snapshotRevision>`，
  `cache: false`（revision 一直在变，缓存旧的纯浪费）。
* C++ 侧：`CicadaPlayerItem::requestSnapshot()`（`CicadaPlayerItem.cpp:1235-1255`）→
  框架 `CaptureScreen()` → `onCaptureScreenCb`（`:217-258`，**在框架线程把整幅 RGBA 拷成 QImage**：
  1080p 8.29 MB / 4K 33.18 MB）→ queued 投回 GUI 线程 → `notifySnapshot()`
  （`:3976-4012`）。
* `notifySnapshot()` 里做了两件事：
  1. `m_lastSnapshot = image;` —— 把整幅放进那个成员，**当次交接用**。
     原来是"生成一次就挂到停播"（全仓 grep 只有赋值、没有任何读取者），
     **已改成"用完即还"**：同一个函数末尾就放掉（§2.4 / §4.1）。
     `destroyPlayer()` 里再放一次作停播兜底。
  2. `SnapshotImageProvider::publish(preview)`，preview 先缩到宽 320：
     320×180×4 = **230 KB**，存在函数内 static 里（`SnapshotImageProvider.h:105-128`），有界。
     **这一份才是 QML 气泡实际看的**，它保持进程级、不动。
* 分段进度条本身（`viewPoints` / `pbpData`，`ProgressRow.qml:52-54`）是接口给的小数组
  （pbp 的 `data` 按 step_sec 一点一个数，30 分钟 1 s 步长 = 1800 个数）→ **< 1 MB**。**【推测】**
* 顺带：首页卡片的缩略图**是落盘的**（`VideoLibrary.cpp:43 thumbCacheDir()`、
  `:391-460 thumbnailFor()` 抽帧 → JPEG 写盘），**不常驻内存**。**【已证实】**

### 1.5 弹幕

* 逻辑在独立纯 C++ 引擎 `danmaku/`，Qt 侧是薄适配层 `DanmakuController`（`DanmakuController.h`）。
* 全部弹幕存在引擎的 `_items` 里（`danmaku/src/DanmakuEngine.cpp`，`load/append` 填、
  `clear/reset` 清，`:169-205`）。没有"只留窗口内"的裁剪 → **全片弹幕常驻**。
  量级：B 站一条弹幕的 `DanmakuItem`（时间 + 类型 + 字号 + 颜色 + 文本 + uid）大约
  100–200 B（含 `std::string` 堆开销）→ 3000 条 ≈ **0.3–0.6 MB**，10000 条 ≈ **1–2 MB**。**【推测】**
* 文字宽度缓存 `_measuredWidths`（`DanmakuEngine.cpp:362` 处 clear；`DanmakuController.h:437`
  说"量一次就被引擎缓存"）→ 键是弹幕文本，条数 = 不同文案数 → 与上面同量级，**< 1 MB**。**【推测】**
* **图集（真正的大件，但有界）**：`DanmakuAtlas`，`DanmakuAtlas.cpp:19 kAtlasInitialSize = 1024`
  → 一张 1024×1024 `Format_ARGB32_Premultiplied` QImage = **4.19 MB**
  + 同尺寸 GPU 纹理 ≈ 4.19 MB（`DanmakuAtlas.cpp:74-78`、`texture()`）。
  缓存是 `QHash<QString, DanmakuAtlasEntry>`（`DanmakuAtlas.h:176`），
  **放不下就整体清空重烤**（`resetPacking()`，`DanmakuAtlas.cpp:98-111`，会打一行
  `DanmakuAtlas: atlas is full ... clearing and re-baking`）。→ **有界，不会长。**
* 渲染侧 `DanmakuRendererItem`：顶点数据按"这一帧在屏条数"生成，量级是几十到几百条。**【推测】**
* QML 侧：`DanmakuView.qml` 只有 `_slotKeys` / `_echoKeys` 两个小数组（`:1116`、`:1370`）；
  `activeItems` 那张 `QVariantList` 在 GPU 路径下**不再构建**（`DanmakuController.h:143-158`
  的 `gpuLayer` 机制，`refreshActiveItems(false)` 只更新统计）—— 这一条已经是省过的。
* **合计：弹幕 ≈ 8–15 MB，其中系统内存约 4.19 MB（QImage）+ 引擎条目 1–3 MB。**

### 1.6 已排除项：`ISliceManager` 的 100 MB 切片池（**生产路径是死代码**）

`framework/data_source/cache/ISliceManager.cpp:19,29-48` 有一个进程级单例：
切片 32 KB、**容量上限 100 MB**（`mCapacity = 100*1024*1024`）、
`fixSizePool` **惰性分配、只回收进 free list、直到进程退出才释放**
（`memPool.cpp:26-50`：`getBuffer()` 里 `new uint8_t[mBufferSize]`，`releaseBuffer()` 只是 push 回队列）。

看着像"关窗后仍有 100 MB"的头号嫌疑人，**但它在本工程里根本不会被构造**：
* `ISliceManager::getManager()` 只在 `cachedSource::Open()`（`cachedSource.cpp:96`）里被调用；
* `cachedSource` **只在测试里被 new 过**：`mediaPlayer/tests/cache/mediaPlayerCacheTest.cpp:63`、
  `framework/data_source/cache/cachedSourceTest.cpp:19`、`framework/cachedDemuxerTest.cpp:40`。
* `DashStream::recreateSource()` / `HLSStream` 传的 `DS_NEED_CACHE`（`DashStream.cpp:529`、
  `HLSStream.cpp:751`）这个 flag **全仓没有任何消费点**（`dataSourcePrototype.cpp:24-58`
  只是把它透传给 `probeScore`，而 `probeScore` 是 `dataSourcePrototype.h:33` 的空默认实现）。
**【已证实】** → 所以这一项 **0 MB**，可以彻底排除。

### 1.7 其它

* **`CacheModule`（边播边缓存）**：`CacheConfig.h:38 mEnable = false` 是默认值；
  `CacheManager::Open` 里 `if (!mCacheConfig.mEnable) return;`（`cacheModule/CacheManager.cpp:48`）。
  Qt 侧**从来不调 `SetCacheConfig`**（`platform/QtPlayer` 全目录 grep 无命中）
  → **关闭，0 MB**。**【已证实】**
* **curl / 网络缓冲**：`CURLConnection2.cpp:21,25`
  `RINGBUFFER_SIZE = 256 KB` + `CURLOPT_BUFFERSIZE = 64 KB`（每条连接）
  → 视频 + 音频两条连接 ≤ **~0.6 MB**。`dataSourceIO.cpp:17` 的 AVIO 缓冲 32 KB。**【已证实】**
* **ffmpeg 解复用上下文**：`avformat` 的 context 是几百 KB 量级（probe 缓冲 + 索引）；
  DASH 每个分片会 reopen 内层 demuxer（`DashStream::openSegment` `:476-507`），
  旧的在 `mPDemuxer->close()`（`:566-570`）里释放。**【推测】**
* **日志缓冲**：`AF_LOGI/W/E` 走 `framework/utils/frame_work_log.h`，
  `framework/utils/ringBuffer.{h,cpp}` 只被 curl 用（grep 确认），
  **没有内存日志环形缓冲**，日志直接写 stdout/文件 → **0 MB**。**【推测（grep 已证没有 ring buffer 消费者）】**
* **QML/JS/场景图**：本工程**没有**设 `QPixmapCache::setCacheLimit`、没有 `qmlRegisterSingletonInstance`
  缓存大对象（grep 确认）。剩下的都是 Qt 运行时自己的：QML 类型注册表、V4 JS 堆、
  `QFontDatabase` 字体缓存（中文回退字体是这里的大头）、D3D11 RHI 的着色器/管线缓存、ICU。
  → 量级 **20–40 MB**，且**进程级、关窗不还**。**【推测：需要 VMMap 才能量（§5）】**

---

## 2. 关窗后 105 MB 的去向

### 2.1 关闭路径释放审计表（每一跳 / 释放了什么 / 是否同步 / file:line）

> **行号口径**：本表行号是**本次改动之后**的当前行号（锚点是函数名，行号会随编辑漂移，以函数名为准）。
> 「同步」= 在**同一个调用栈上**当场释放（不是等 GC、不是等分配器还页、不是等后台线程）。

| 跳 | 触发 / 位置 | 释放了什么 | 是否同步 | 判据（日志/代码） |
|---|---|---|---|---|
| 1 | `Main.qml:124-127 onClosing` | 只负责发两个调用：`player.stop()` + `playerView.source = ""`；本身不释放内存 | 同步（同一次事件处理） | `Main.qml:124-127` |
| 2 | `CicadaPlayerItem::stop()` `CicadaPlayerItem.cpp:1236` | 只调 `m_player->Stop()`（不销毁对象） | 同步 | `:1236-1244` |
| 3 | `setSource(QUrl())` → `destroyPlayer()` `CicadaPlayerItem.cpp:850` | 关窗这一跳是**同步**的：`setSource` 里第 850 行直接调 `destroyPlayer()`，不是排队 | 同步 | `setSource` 定义 `:789`，`destroyPlayer()` 调用点 `:850` |
| 4 | `destroyPlayer()` `CicadaPlayerItem.cpp:3262`（整体） | 见下面 4a–4f | — | 函数体 `:3262-3390` |
| 4a | `:3320-3324` | **整幅截屏快照**（`m_lastSnapshot`，1080p 8.29MB / 4K 33.18MB） | **同步**（QImage 引用计数归零即释放像素） | 日志 `[mem] destroyPlayer: ... snapshot N bytes released` `:3375` |
| 4b | `:3339` `m_releaseRenderState = true; update();` | **不是同步释放**：给渲染线程留标记，由它在自己的线程上放 `m_currentFrame` + D3D11 输入视图（钉住整池表面纹理的那两个引用） | **异步（渲染线程）** | 日志 `[mem] render thread released the previous frame and the D3D11 input view` `:3721` |
| 4c | `:3351 m_player->Stop()` → `SuperMediaPlayer::Stop()` `SuperMediaPlayer.cpp:963` | **包队列**（`mBufferController->ClearPacket(BUFFER_TYPE_AV)` `:1026`）、**帧队列**（`FlushAudioPath` `:1029` / `FlushVideoPath` `:1034`）、音频/视频设备标失效（`:1023`）、**解复用器**（`mDemuxerService->stop()/close()` + 各 `CloseStream` `:1039-1071`）、**数据源**（`mDataSource->Close()` + `delete` `:1073-1078`） | **同步** | `SuperMediaPlayer.cpp:963-1078` |
| 4d | `:3352 m_player.reset()` → `~MediaPlayer` `MediaPlayer.cpp:133` → `CicadaReleasePlayer` `media_player_api.cpp:40-45`（`delete pPlayer`）→ `~SuperMediaPlayer` `SuperMediaPlayer.cpp:480` | `~SuperMediaPlayer` 里 `:511 mApsaraThread->stop()`、`:518 mAVDeviceManager = nullptr` → **解码器析构**（`~avcodecDecoder` `avcodecDecoder.cpp:826` → `close()` `ActiveDecoder.cpp:48-77` → `close_decoder()` `avcodecDecoder.cpp:261`）→ `avcodec_free_context` + 归还 hw device + 释放 6 槽 copy-back 池 ⟹ **D3D11VA 的 20 片表面池（1080p 62.2MB / 4K 248.8MB）在这里失去最后一个引用** | **同步** | 日志 `decoder closed: codec context freed, hardware device reference released, ...` `avcodecDecoder.cpp:317` |
| 4e | `:3356-3358` | **挂起帧** `m_pendingFrame.reset()` | 同步 | `:3356-3358` |
| 4f | `:3375` | 一行汇总探针（关窗后核对该行是否打印） | 同步 | `AF_LOGI("[mem] destroyPlayer: ...")` |
| 5 | 窗口 `visible=false` → `HomeWindow.qml:1267-1270` `visibleChanged` → `Qt.callLater(reapPlayerWindow)` → `reapPlayerWindow()` `HomeWindow.qml:1544` → `w.destroy()` `:1563` | **整棵播放器窗口的 QML 对象树 + `CicadaPlayerItem`**。`parent` 是 `home.contentItem`（有 QObject 父对象）→ `destroy()` 走 `deleteLater()`（**下一轮事件循环**，不是 GC，是确定的删除） | **异步（下一轮事件循环）** | 日志 `[mem] reapPlayerWindow: destroying the player window object` `HomeWindow.qml:1562`；反例分支 `[mem] reapPlayerWindow: window still visible, destroy skipped` `:1556` |
| 6 | `CicadaPlayerItem::onSceneGraphInvalidated()` `CicadaPlayerItem.cpp:3487`（渲染线程） | `invalidateFrameTextureCache()`（放掉 `m_cachedTexture`/`m_cachedFrame` 的**引用**，不 delete —— 那张 QSGTexture 归场景图节点所有，见该函数的注释）→ `m_textureBackend->releaseResources()` → `CicadaHardwareDevice::releaseFromSceneGraph(window())`（归还借给 FFmpeg 的 Qt D3D11 设备） | **同步**（渲染线程上） | 日志 `[mem] scene graph invalidated: ...` `:3514` |
| 7 | `CicadaVideoTexture::releaseResources()` `CicadaVideoTexture.cpp:330` | 平台后端 + **CPU 回退的 `m_bgraImage`（整幅 BGRA，1080p 8.29MB / 4K 33.18MB）与 `m_sws` 上下文**（本次新增） | **同步** | 日志 `[mem] video texture backend released: CPU fallback bitmap N bytes ...` `:391` |
| 8 | `CicadaTextureD3D11::releaseResources()` `CicadaTextureD3D11.cpp:225` | **8 个 COM 对象**：输出视图、输入视图、视频处理器、枚举器、**输出 RGBA 纹理（1080p 8.29MB / 4K 33.18MB VRAM）**、VideoDevice/VideoContext/VideoContext1 | **同步** | 日志 `[mem] D3D11 zero-copy resources released: RGBA output texture WxH ...` `:300` |
| 9 | `~CicadaPlayerItem()` `CicadaPlayerItem.cpp:722` | 再兜一遍：`destroyPlayer()`（`:731`）+ `invalidateFrameTextureCache()` + `releaseResources()`（都幂等） | **同步** | 日志 `[mem] ~CicadaPlayerItem: player item destroyed, no player-held resource left` `:747` |
| 10 | `Main.qml` QML 树析构 | QML 对象树（含弹幕 `DanmakuRendererItem` → `~DanmakuAtlas` 4.19MB QImage + GPU 纹理、`DanmakuController` 引擎里全部弹幕条目） | **同步**（随之析构；场景图纹理由 Qt 回收） | 日志 `[mem] Main.qml: player window QML tree destroyed` `Main.qml:139` |
| 11 | `CurlMulti` 网络线程（**全局**，进程级） | curl 连接对象 → `~CURLConnection2` `CURLConnection2.cpp:135`（`curl_easy_cleanup` + `RingBufferDestroy`）。`deleteHandle` 只是入队 `CurlMulti.cpp:245-256`，真正 `delete` 在网络线程下一轮 `:210-213` | **异步（网络线程），量级 < 1 MB** | `SuperMediaPlayer.cpp:1073-1078` 关数据源；`CurlMulti.cpp:202-213` |

**审计结论（逐项核对的结果）**：

| 大块资源 | 关窗时是否释放 | 释放点 | 是否"同一栈立刻" |
|---|---|---|---|
| 解码器 + **D3D11VA 表面池（1080p 62.2MB / 4K 248.8MB）** | 是 | `m_player.reset()` → `avcodecDecoder::close_decoder()` | **是**（跳 4d） |
| 帧队列（`m_pendingFrame` / `mVideoFrameQue` / `mAudioFrameQue`） | 是 | `destroyPlayer` 4e + `Stop()` 的 Flush*Path 4c | 是 |
| 包队列（视频/音频/字幕三条 `MediaPacketQueue`） | 是 | `Stop()` 的 `ClearPacket(BUFFER_TYPE_AV)` 4c | 是 |
| `m_currentFrame` + D3D11 输入视图（钉住整池的两个引用） | 是 | 跳 4b（渲染线程）或跳 9（item 析构） | **两者之一**：4b 是异步（渲染线程下一帧），9 是同步（`w.destroy()` 之后）。**关窗后这两条 [mem] 日志至少有一条必须打印** |
| D3D11 输出 RGBA 纹理 + 其余 7 个 COM 对象 | 是 | 跳 8 | 是 |
| `m_cachedTexture`（QSGTexture 缓存引用） | 是（只放引用，纹理由场景图节点回收） | 跳 6/9 的 `invalidateFrameTextureCache()` | 是 |
| **整幅截屏快照** `m_lastSnapshot` | 是 | 跳 4a（+ 正常路径上 `notifySnapshot` 已经"用完即还"，见 §2.4） | 是 |
| CPU 回退 `m_bgraImage` + `m_sws` | 是 | 跳 7（本次新增；原来只在 `~CicadaVideoTexture`） | 是 |
| curl 连接与环形缓冲 | 是 | 跳 11（网络线程下一轮） | 异步但 < 1 MB |
| 弹幕引擎 / 图集 / 对象池 | 是 | 跳 10（随 QML 树析构） | 是 |
| 日志缓冲 | 无此对象（无内存日志环形缓冲，见 §1.7） | — | — |
| `CacheModule` 播放缓存 | 无此对象（默认关闭，见 §1.7） | — | — |
| `ISliceManager` 100MB 切片池 | 无此对象（生产路径死代码，见 §1.6） | — | — |

**所以：关窗之后，没有任何"播放器持有的大块资源"存活。** 唯一"不是同一栈"的两项是
① 被渲染线程持有的 `m_currentFrame` + 输入视图（由渲染线程下一帧或 item 析构释放，二者必有一）
和 ② curl 连接（网络线程下一轮，< 1MB）—— 两者都不是"忘了释放"，而是**线程归属决定的释放点**。

**关于 `~CicadaTextureD3D11()` 故意不 Release（`CicadaTextureD3D11.cpp:108-115`）**：
**在真实关窗路径上它不会导致"纹理不回收"**，证明如下（三条都是代码事实）：
1. `CicadaPlayerItem::m_textureBackend` 在构造函数里 `new`（`CicadaPlayerItem.cpp:690`），
   **全仓没有任何 `reset()`/重新赋值**（它只在 `~CicadaPlayerItem`、`onSceneGraphInitialized`、
   `onSceneGraphInvalidated`、`updatePaintNode` 这几处被解引用调用），所以它一定比
   `~CicadaPlayerItem` 活得久；
2. `~CicadaPlayerItem` 函数体里**先**调 `m_textureBackend->releaseResources()`（`:735-737`），
   然后成员才按声明逆序析构 → `~CicadaVideoTexture` → `m_d3d11` 的 `unique_ptr` 析构；
   也就是说 **`~CicadaTextureD3D11()` 被调用时，8 个 COM 指针全都是 nullptr**；
3. 即使 `~CicadaPlayerItem` 没跑到（窗口树没销毁），`~CicadaTextureD3D11()` 也不会跑
   （它由 item 拥有）—— 那种情况的问题是"窗口树还活着"，不是"析构没 Release"。
因此**不需要改它**；改它反而会把当前"最坏情况只是多留一份引用"变成
"在渲染线程可能还在用的时候把对象 Release 掉"（`Release` 掉最后一个引用会当场销毁
D3D11 对象，渲染线程再 `Blt` 就是 use-after-free）。这一条是**故意选择"漏而不崩"**，
保留原样，只用跳 8 的日志证明它每次都先被调用过。

### 2.2 那 80 MB 到底是什么（按可能性排序）

| # | 持有者 | 量级 | 证据 / 判据 | 结论 |
|---|---|---|---|---|
| 1 | **Windows 分配器不把释放的页还给系统**（NT/segment heap 的 free list + 工作集未 trim） | 数十 MB | 仓库实测：关窗后地板 WS ~125–140 MB、**4 轮开关不抬升**（`MEMORY.md` §6 表） | **主因（推测但吻合实测）**；不是泄漏 |
| 2 | **Qt/QML 运行时**：QML 类型注册表 + V4 JS 堆 + `QFontDatabase`（中文字体）+ D3D11 RHI 着色器/管线缓存 + ICU | 20–40 MB | 进程级，随首页窗口一起常驻；无代码级释放点 | 正常开销 |
| 3 | **首页窗口自己**（HomeWindow 的 QML 树 + 场景图 + 交换链） | 10–25 MB | 用户自己测的"只开主窗口 = 25 MB" | 正常开销 |
| 4 | `m_lastSnapshot` 整幅快照 | 8.29（4K 33.18）MB | 原来是"生成一次挂到停播"的写死成员 | **真 bug → 已修**：改成"按需生成、用完即还"，见 §2.4 / §4.1 |
| 5 | CPU 回退的 `m_bgraImage`（只在零拷贝失败的会话里） | 8.29（4K 33.18）MB | 原来只在 `~CicadaVideoTexture()` 释放 | **真缺口 → 已修（§4.2）** |
| 6 | `CicadaTextureD3D11` 的 COM 引用（**仅当 `releaseResources()` 没被调用过**） | 8.29–33.18 MB（VRAM）+ 若干 MB 驱动侧 | `~CicadaTextureD3D11()` 故意不 Release（`.cpp:108-115`） | **已证明在真实关窗路径上不会发生**（`~CicadaPlayerItem` 一定先调 releaseResources，见 §2.1 末段） |
| 7 | `SnapshotImageProvider::publish()` 的 320 宽预览（进程 static） | 0.23 MB | `SnapshotImageProvider.h:119-128` | 可忽略 |
| 8 | Qt 全局 pixmap 缓存里那些**悬停过的预览帧**（解码后位图） | 最多 ~20 MB | `ProgressRow.qml:755-763` 的 Image **没有 `cache: false`**；工程没设缓存上限 | 有界于 Qt 默认上限；**唯一"随着使用长大"的项** |

### 2.3 结论（回答用户的问题②）

> ⚠ **【本节已被 §2.6 更正，保留原文只为留痕】**
> 下面这段把 105 MB 解释成"进程地板（首页窗口 25MB + Qt 运行时 + 分配器未 trim）"，
> **在"只开主窗口就是 25MB"这个已知事实下不成立**：同一进程既然能到 25MB，
> 关掉播放器窗口后就该回到 25MB 附近，而实测停在 107MB ⇒ **有 ~82 MB 没释放**。
> 而且"`MEMORY.md` §6 证明无泄漏"这条引用也不成立（那份脚本自己销毁窗口、
> 从未走过 `HomeWindow` 的销毁那一跳，而且它自己的地板 118→127→130 是在涨的）。
> 正确结论、逐项清单与判据见 **§2.6 / §2.7**。

* **不是"关闭后资源没销毁"**：关窗链路（§2.1）在代码上是完整的，而且 `MEMORY.md` §6 那份
  **实测**已经证明"连开 4 个窗口、每轮峰值和地板都不抬升" → **没有泄漏**。
* **关窗之后没有任何"播放器持有的大块资源"存活**（逐项核对见 §2.1 的两张表）。
  唯一两项"不是同一栈立刻放"的是**渲染线程手里的 `m_currentFrame` + D3D11 输入视图**
  （渲染线程下一帧 **或** item 析构，二者必有一）和 **curl 连接**（网络线程下一轮，< 1MB）——
  两者都是线程归属决定的释放点，不是遗漏。
* **【105 MB 地板】与【播放器资源】必须分开看**（这就是用户问的那件事）：
  * **播放器资源**（解码器 + 表面池 + 纹理 + 帧/包队列 + 快照 + 数据源）：
    在关窗这一跳**全部释放**，其量级在 1080p 零拷贝下是 **70–90 MB**
    （表面池 62.2MB + 输出纹理 8.29MB + 包缓冲 5–7MB + 帧/队列若干）
    —— 也就是说 173 → 105 掉下来的那 ~68MB 里，主要就是它们（吻合）。
  * **105 MB 地板**：首页窗口（用户自测 25 MB）+ Qt/QML 运行时（类型注册表 / V4 堆 /
    中文字体缓存 / D3D11 RHI 着色器与管线缓存 / ICU，20–40 MB）
    + **分配器已经把内存还给系统、但进程的工作集没有立即 trim** 的那部分。
    这三样都**不属于播放器**，关不关播放窗口都一样。
* **修掉的两处"该释放没释放"**（§4.1、§4.2）各 8.29MB（4K 33.18MB），
  效果是"停播/暂停时这两笔立刻返还"，而不是"解释掉全部 80MB"。

### 2.4 截图成员的生命周期（`m_lastSnapshot`，按用户口径改造）

用户口径：**截图能力要保留、绝不能有内存泄漏、不要"生成一次就永久挂着一整幅"**。
按这个口径，`m_lastSnapshot` 的生命周期现在是三个明确的点（`CicadaPlayerItem.h` 里那个成员的注释
与 `notifySnapshot()` 里的代码逐字对应）：

| 阶段 | 行为 | 位置 |
|---|---|---|
| **生成（按需）** | 进度条悬停/拖动 → `requestSnapshot()` → 框架 `CaptureScreen()` → `onCaptureScreenCb` 在框架线程现做一张整幅 RGBA（逐行 memcpy 进 `QImage`） | `CicadaPlayerItem.cpp:1245`（请求）、`:217-258`（回调） |
| **用完即还** | `notifySnapshot()` 把**缩小版**（320 宽，约 230KB）交给 `SnapshotImageProvider` 之后，**在同一个函数里** `m_lastSnapshot = QImage();` —— QImage 是引用计数，最后一份引用归零就**当场**释放像素缓冲 | `CicadaPlayerItem.cpp:4050`（函数）、`:4109`（还） |
| **停播兜底** | `destroyPlayer()`（停播 / 换片源 / 关窗 / 析构）再放一次，并打一行带字节数的日志 | `CicadaPlayerItem.cpp:3320-3324`、日志 `:3375` |

要点：
* **稳态（不悬停）时这个成员是空的** —— 播放器不持有任何整幅位图；
  整幅只在 `notifySnapshot()` 那一次调用期间存在（峰值 8.29/33.18MB，随即归还）。
* **能力没有丢**：`requestSnapshot()` / `CaptureScreen()` / provider 这条链一字未改，
  进度条悬停气泡（`ProgressRow.qml:731-733, 769-778`，`cache: false`）行为完全不变。
* **将来做"保存截图"的正确接入方式**（已写在 `.h` 与 `.cpp` 注释里）：
  在用户点存的那一刻现调 `requestSnapshot()` 取一张（暂停时也能截），
  在 §2.4 表格第二行那个位置把 `image` 交给消费者，由消费者用完释放 ——
  **不要**让它跨越整场播放常驻，也**不要**把整幅塞进 QML 属性。
* **同口径检查过的其它"整幅帧"成员**（用户要求"一并按同一口径处理"）：
  | 成员 | 是不是整幅帧缓存 | 现状 |
  |---|---|---|
  | `CicadaVideoTexture::m_bgraImage`（QImage*，整幅 BGRA） | 是 | 已改为在 `releaseResources()` 里释放（§4.2） |
  | `CicadaVideoItem::m_cachedTexture`（QSGTexture*） | 否（只是渲染线程的"同一帧→同一纹理"缓存引用，像素归场景图节点所有） | 由 `invalidateFrameTextureCache()` 放引用；`~CicadaPlayerItem` 与场景图失效两条路都覆盖（`CicadaPlayerItem.cpp:3538-3549`） |
  | `CicadaVideoRender` 截屏缓冲（`malloc(width*height*4)`） | 是，但**本来就是按次分配/按次释放** | 每次截屏现 malloc，回调返回后立刻 `free`（`CicadaVideoRender.cpp:475-510`）——**这正是本次给 `m_lastSnapshot` 采用的口径** |
  | `SnapshotImageProvider` 的进程 static（`QImage`） | 否（已缩到 ≤320 宽，约 230KB） | 有意进程级（多窗口"后发布者赢"）；**不能在停播时置空**（空图会让 QML `Image` 打 `Failed to get image from provider` 警告，见 `ProgressRow.qml:725-729`） |
  | `DanmakuAtlas::m_image`（1024×1024 ARGB32 = 4.19MB） | 否（弹幕文字图集，不是视频帧） | 有界（满了整体重烤）；随 `DanmakuRendererItem` 析构释放，且已处理场景图失效 |
  | `VideoLibrary::grabFrameAt()` 的 QImage | 是，但是**局部量** | 抽帧后写盘，函数返回即释放（`VideoLibrary.cpp:277-460`） |
  | macOS `CicadaVideoTexture::textureForFrameCpu()` 的 `downloaded` 帧 | 是，但是**局部 unique_ptr** | 函数返回即释放（`CicadaVideoTexture.cpp:235-254`） |

### 2.5 判据（怎么证明上面每一条 —— 探针已经加好了）

**本次已经在代码里加好 9 处释放探针**（一行一个，只在停播/关窗/释放时打印，不会刷屏）。
其中 8 处在 Qt 播放器侧、统一带 `[mem]` 前缀；内核那一条（解码器关闭）沿用内核日志风格、
不带前缀（见下表第 2 行）。关窗后应当能在日志/控制台里看到**下面这几行**（同一轮关窗的全部或大部分）：

| 顺序 | 探针日志（关键片段） | 位置 | 它证明什么 |
|---|---|---|---|
| 1 | `[mem] destroyPlayer: player stopped and destroyed (decoder + D3D11VA surface pool, packet/frame queues, demuxer and data source all released on this stack); snapshot N bytes released` | `CicadaPlayerItem.cpp:3375` | 播放器侧全部大块资源在**同一个调用栈**上放完；N = 整幅快照字节数（正常是 0，见 §2.4） |
| 2 | `decoder closed: codec context freed, hardware device reference released, copy-back frame pool freed (...)` | `framework/codec/avcodecDecoder.cpp:317` | **最大的那一笔（表面池 62.2MB@1080p / 248.8MB@4K）真的还回去了** |
| 3 | `[mem] scene graph invalidated: video texture backend released, borrowed D3D11 device returned` | `CicadaPlayerItem.cpp:3514` | 场景图这一跳把纹理后端与**借来的 Qt D3D11 设备**都还了 |
| 4 | `[mem] D3D11 zero-copy resources released: RGBA output texture WxH (N bytes), ...` | `CicadaTextureD3D11.cpp:300` | 零拷贝的 8 个 COM 对象 + 输出纹理（8.29/33.18MB）已 Release |
| 5 | `[mem] video texture backend released: CPU fallback bitmap N bytes, swscale context freed` | `CicadaVideoTexture.cpp:391` | CPU 回退时那块整幅 BGRA 已放（零拷贝会话里 N=0 是正常的） |
| 6 | `[mem] render thread released the previous frame and the D3D11 input view (this is what unpins the decoder surface pool)` **或** 第 8 行 | `CicadaPlayerItem.cpp:3721` | 钉住表面池的帧引用被放掉（**这一条与第 8 条至少有一条必须出现**） |
| 7 | `[mem] reapPlayerWindow: destroying the player window object` | `HomeWindow.qml:1562` | 窗口对象确实被销毁（不是"藏起来"） |
| 8 | `[mem] ~CicadaPlayerItem: player item destroyed, no player-held resource left` | `CicadaPlayerItem.cpp:747` | 播放器侧已彻底结束 |
| 9 | `[mem] Main.qml: player window QML tree destroyed` | `Main.qml:139` | 整棵 QML 树（含弹幕引擎/图集）已随窗口销毁 |
| ⚠ 反例 | `[mem] reapPlayerWindow: window still visible, destroy skipped` | `HomeWindow.qml:1556` | **如果看到这一行**：窗口没被销毁（第 7/8/9 行不会出现）→ "关窗后内存降不下来"的原因是窗口树还活着，与 Qt/QML 常驻无关 |

怎么用这条链回答"是地板还是播放器没放"：
* 第 1–6 行**都出现** → 播放器侧资源已全部释放 → 剩下的就是 **§2.3 说的 105MB 地板**；
* 第 1–6 行里缺了某一行 → 那一类的资源没放（例如缺第 2 行 = 表面池还挂着），按缺失的那一行定位；
* 第 7/8/9 行缺了、反而出现 ⚠ 反例行 → 窗口对象没销毁，先修那一跳。

其它（不需要改代码就能做的）验证：

| 要证明的事 | 怎么做 |
|---|---|
| 是不是泄漏（而非地板） | 开关播放窗口 **4 次**，每次记 WS/Private/Handles。**地板不抬升 = 不是泄漏**（`MEMORY.md` §6 就是这个方法）。 |
| 表面池算不算系统内存 | 播放时切到任务管理器「性能 → GPU → 专用/共享 GPU 内存」，对照进程「内存」列；或用 VMMap 看 Private Data 的增长量是不是 ≈ 62 MB。 |
| 分配器/堆占比（地板拆分） | Sysinternals **VMMap** → 播放中 / 暂停 / 关窗后 各存一份快照 → 看 Type 汇总里的 `Private Data` / `Heap` / `Image` / `Mapped File` 占比。关窗后仍高的是分配器与 Qt 运行时，不是播放器。 |
| 弹幕 | `DanmakuAtlas` 已有 `cachedCount()` / `atlasWidth()` / `atlasHeight()`（`DanmakuAtlas.h:152-154`），1024×1024 = 4.19MB 是常数。 |
| 预览图（**按用户要求保持默认，不改**） | 想看规模就在 `Main.qml` 的 `preview.bin` 回调里临时打一行 `frames.length` 与 `frames[1].length`；**不要**改 `cache: false`、**不要**改成按需取。 |

---

### 2.6 ✕ 路径的真相、本轮修复，以及每开/关一轮的累积清单

#### 2.6.1 先钉死"日志能不能当判据"（这条推翻了上一轮的判决）

我在上一轮加的两条"窗口探针"用的是 QML `console.log`。它的实际流向是：

| 环节 | 代码事实 | 后果 |
|---|---|---|
| QML 的 console 输出 | 走 Qt 消息处理器：`main.cpp:842 qInstallMessageHandler(cicadaQtMessageHandler)` | 进了处理器，但**类型决定去哪个宏** |
| `console.log` → `QtDebugMsg` | `main.cpp:402-403`：`case QtDebugMsg: AF_LOGD(...)` | 变成 **D 级**日志 |
| 框架的 D 级开关 | `frame_work_log.c:268-272`：Release（`NDEBUG`）下 `logCtrl.log_level = AF_LOG_LEVEL_INFO`；`:279` `if (prio > log_ctrl_level) return 0;` | **D 级被直接丢弃** ⇒ `console.log` **进不了那份日志** |
| `console.warn` → `QtWarningMsg` | `main.cpp:410-411`：`AF_LOGW(...)`（24 ≤ INFO） | **必然进日志** ✓ |

**结论：用"QML `console.log` 探针没打"来断言"窗口树没销毁"是不成立的**
（用户实测的"每轮 +25 MB"是真的，但那条推论不成立）。**本轮已经把这几点改掉**：
* QML 侧探针改 `console.warn`（`Main.qml:139` 一带、`HomeWindow.qml` 的 `reapPlayerWindow`/两个触发点）；
* 关键判据搬到 **C++ 侧走 `AF_LOGI`**：
  * `[mem] the player window object was destroyed (QQuickWindow::destroyed)` ——
    在 `CicadaPlayerItem::onSceneGraphInitialized()` 里接窗口自己的 `destroyed` 信号
    （`CicadaPlayerItem.cpp`，`onSceneGraphInitialized` 内；成员 `m_windowDestroyedProbeAttached`
    追加在 `CicadaPlayerItem.h` 类末尾）。**这条一发就说明窗口对象真的被删了**；
  * `[mem] CicadaReleasePlayer: destroying the player object ...` ——
    `mediaPlayer/media_player_api.cpp:40`（桌面与安卓共用的唯一收口点）；
  * MediaCodec / AAudio 两条安卓探针（见安卓文档）。
* 新增**每轮内存读数**：`CicadaPlayerItem` 构造与析构各打一行
  `[mem] ... working set %.1f MB, private commit %.1f MB`（Windows 用
  `K32GetProcessMemoryInfo`，`GetProcAddress` 解析、**不加 psapi.lib**）。
  ⇒ "这一轮开关留下多少"在**同一份日志里**就能读出来。

#### 2.6.2 ✕ 的真实链路（逐跳，file:line）

| 跳 | 位置 | 做了什么 | 对"释放"的意义 |
|---|---|---|---|
| 1 | `Main.qml` 的 `onClosing`（`Main.qml:124` 起） | `player.stop()` + `playerView.source = ""`；**没有**动 `close.accepted`（默认接受关闭） | `source = ""` → `CicadaPlayerItem::setSource` → **第 850 行同步调 `destroyPlayer()`** ⇒ 播放器资源在这一跳全放 |
| 2 | 同上，新增 | `root.closeCommitted()`（新信号） | 把"窗口对象也该销毁了"这件事**确定性地**告诉创建方（原来只有 `visibleChanged`） |
| 3 | `HomeWindow.qml` `ensurePlayerWindow()`（`HomeWindow.qml:1249` 起） | 连 `closeCommitted` → `Qt.callLater(home.reapPlayerWindow)`；**兜底**再连 `visibleChanged` → 同一个函数 | 两个触发点汇进**一个幂等函数**，不再有"两个都不动手"的空档 |
| 4 | `HomeWindow.qml` `reapPlayerWindow()`（`HomeWindow.qml:1590` 附近） | **删掉了原来的 `if (playerWindow.visible) return` 闸**；先置空引用再 `w.destroy()` | 这是本轮**唯一的行为修复**：销毁不再依赖 `visible` 这个平台读数 |
| 5 | `w.destroy()`（parent = `home.contentItem`） | 有 QObject 父对象 ⇒ 走 `deleteLater()`；`Qt.callLater` 已经把它推到"本次事件处理之后" | 时序保证从"读一个布尔量"改成"事件循环保证"：close 已走完、场景图已收完 |
| 6 | `CicadaPlayerItem::onSceneGraphInvalidated()` / `~CicadaPlayerItem()` | 纹理后端、借来的 D3D11 设备、8 个 COM 对象、输出纹理、CPU 回退位图+sws | 已在 §2.1 逐个核对，日志 6 条齐全 |
| 7 | 进程不退出 | 只销毁播放器窗口；首页窗口、QML 引擎、进程都留着 | 符合用户要求 |

**真相结论**：
* ✕ 走到跳 1 是**必然**的（`closing` 在关闭事件里必发），所以**播放器那 6 条探针必然齐全** ——
  与用户日志一致；
* 原来"窗口对象销毁"只挂在 `visibleChanged` 上，而 `visible` 在 ✕（原生关闭）路径上
  **不保证**变成 false（同一个坑本仓文件对话框那段注释里已记过一次："它的 `visible` 可能一直停在 true"）。
  一旦如此，`reapPlayerWindow` **根本不会被调用**（不只是走跳过分支，而是整个函数不会进），
  窗口对象 + 整棵 QML 树活到进程退出 ⇒ **与"每轮 +25 MB"完全吻合**；
  这也解释了为什么两条 QML 探针**一条都没打**（含"跳过"那条）。
* 跳 3–4 的改动让这条闸消失：**无论 `visible` 怎么取值，`closeCommitted` 一到就销毁**。

#### 2.6.3 每开/关一轮的累积清单（谁持有 / 能否关窗释放 / 怎么释放 / 判据）

| 项 | 量级 | 每轮还是首次 | 谁持有 | 关窗能否释放 | 怎么释放 / 判据 |
|---|---|---|---|---|---|
| **播放器窗口对象 + QML 树 + 它的场景图/RHI**（D3D11 字形图集 ~2048²、交换链、管线/着色器缓存） | **~25 MB** | **每轮** | `HomeWindow.playerWindow` → QML 树 | **能，且必须** | **本轮修**：`closeCommitted` → `reapPlayerWindow()`（无 `visible` 闸）→ `destroy()`。判据：`[mem] the player window object was destroyed` + `[mem] Main.qml: player window QML tree destroyed` 每轮各一次 |
| 弹幕：`DanmakuEngine`（全片弹幕）+ `DanmakuAtlas`（1024² QImage 4.19MB + 同尺寸 GPU 纹理）+ `DanmakuRendererItem` 顶点缓冲 | ~8–15 MB（其中系统内存 ~4.19MB + 引擎条目） | 每轮 | 窗口 QML 树内（`DanmakuView`） | **能（随窗口树）** | 无独立持有者；**窗口树不销毁时它跟着泄漏** ⇒ 修好上一行即可。判据：`DanmakuAtlas: atlas is full` 不是每轮出现；`~DanmakuAtlas` 随 `Main.qml ... destroyed` |
| `CicadaPlayerItem` + `CicadaVideoTexture`（D3D11 输出纹理 8.29MB + 7 个 COM） | 8.29 MB（VRAM）| 每轮 | 窗口 QML 树内 | **能** | `~CicadaPlayerItem` → `releaseResources()`（日志：`[mem] D3D11 zero-copy resources released: RGBA output texture 1920x1080 (8294400 bytes)`） |
| **FFmpeg D3D11VA 表面池**（20 片一个 Texture2D Array） | **1080p 62.2MB / 4K 248.8MB** | **首次之后不回落 = 一次性残留头号嫌疑** | 解码器（`SMPAVDeviceManager` → `DecoderHandle::decoder`） | **能**（解码器析构即释放） | 释放点：`m_player.reset()` → `~SuperMediaPlayer:518 mAVDeviceManager = nullptr` → `~avcodecDecoder` → `close_decoder()`。判据：日志必须有 **`decoder closed: codec context freed, hardware device reference released...`**（`avcodecDecoder.cpp`）。**这条若缺失，62MB 就是它** |
| 渲染线程手里的 `m_currentFrame` + D3D11 输入视图（钉住整池的两个引用） | —（决定上一条能否真放） | 每轮 | `CicadaPlayerItem`（渲染线程） | **能** | `m_releaseRenderState` 分支（日志 `[mem] render thread released the previous frame and the D3D11 input view`）**或** item 析构；两条至少一条要出现 |
| Qt 全局 pixmap 缓存（悬停过的预览帧，data URL 作 key） | 0（不悬停）～ 数十 MB（Qt 上限内） | 取决于是否悬停 | **Qt 进程级** | **本工程没有清理入口，且用户要求预览功能不动** | 如实列出：这是**唯一**一个"用得多就大、且关窗不回落"的项。测量：关窗前后对比任务管理器；要验证是否它 → 整个会话不悬停进度条复测一次，看地板是否变小 |
| `SnapshotImageProvider` 的进程 static（≤320 宽） | 0.23 MB | 每次截屏覆盖 | Qt 引擎持有 | **不需要**（单槽、覆盖式） | 不能置空（会触发 QML 取图警告），保持 |
| `Qt.createComponent(Main.qml)` 的 `QQmlComponent` | KB~MB | **每轮（原来）** | JS 所有权（等 GC） | **本轮改成只建一次** | `HomeWindow.qml` 新增 `property var playerComponent` 缓存 |
| `CicadaHardwareDevice::g_qtDevice`（借给 FFmpeg 的设备） | 一个设备（不是每轮一个） | 首次/每次 capture 覆盖 | 进程级单例 | **已核对记账平衡**：每次 `captureFromSceneGraph` 恰好 +1 引用，`releaseFromSceneGraph` 恰好 -1（`CicadaHardwareDevice.cpp:322-348` / `:427-447`） | 日志已有：`captured Qt's D3D11 device ...` / `released the captured D3D11 device ...` 每轮各一次 |
| `Main.qml` / `PlayerView.qml` 里每轮新建的信号连接 | 无（连接随被连对象一起销毁） | — | 各窗口内对象 | **能（随窗口树）** | 已逐条核对 7 处 `.connect(`：发送者都在窗口内，闭包只引用 `home`，不反向持有窗口 |
| V4 JS 堆 / QML 类型注册 / 中文字体缓存 / ICU | 几 MB~十几 MB | **首次之后不回落** | Qt 进程级 | **不能**（设计如此） | 只能给证据：关窗后 `Private Data` 里若有这么一块且**不随轮次增长**，就是它；要"清"的话只能退出进程 |
| `CacheModule` / `ISliceManager` | 0 | — | — | — | 默认关闭 / 生产路径死代码（§1.6/§1.7） |

#### 2.6.4 【新实测】84 MB / 114 MB 这组数字怎么读（含一条命令定性）

用户跑上有修复+探针的一版之后的数据：

| 时刻 | 内存 | 与基线的差 |
|---|---|---|
| 只开主窗口（基线 B） | ~25 MB | — |
| 第 1 轮关窗后 | **~84 MB** | **+59 MB（一次性残留）** |
| 第 2 轮关窗后 | **~114 MB** | 再 **+30 MB/轮（每轮累积）** |
| （对比：修复前的同一测法） | 107 MB | 修复后首轮 −23 MB |

**两个数字各自指向什么（按量级对应，并给出验证方法）**：

* **+59 MB ≈ 62.2 MB = 1080p 的 D3D11VA 表面池**（20 片 NV12 在一个 Texture2D Array：
  1920×1080×1.5×20 = 62,208,000 B = 59.3 MiB）。⇒ **"为什么关窗后表面池没放"是第一优先**。
  两条互斥的判据（见下面那条 grep）：
  1. **`avcodecDecoder object destroyed` 缺失** ⇒ 解码器**对象**还活着
     （谁在持有 `IDecoder`/`AVCodecContext`）⇒ 表面池跟着活；
  2. **该行出现、且 `hw device ref exclusively owned=1`** ⇒ 设备/表面池这一侧干净，
     59 MB 不是它，要改查"Qt 设备/驱动侧分配"或"每轮累积项"。
* **+30 MB/轮 ≈ 一个播放器窗口树 + 它的 RHI**（D3D11 字形图集 ~16.7MB + 交换链 + 管线/着色器缓存，
  合计 20–30 MB）。⇒ 每轮仍有东西留下。**首选判别法不用外部工具**：
  看**析构行的 `top-level windows alive=N`** ——
  * 关窗后 N 没有回到基线值（首页窗口那个数）**且逐轮递增** ⇒ **窗口对象没被销毁**，
    问题在 ✕ 销毁这一跳（下一段给了逐项排查）；
  * 关窗后 N 正确回落、但析构行的 `working set` 仍逐轮 +30 MB ⇒ **窗口树确实回收了**，
    累积发生在窗口之外（Qt 侧缓存/设备/驱动按轮累积）——那时按 §2.6.3 表里
    "Qt 进程级"几行逐项排除（最快的一刀：整个会话**不悬停进度条、不开弹幕**再测一次）。

**一条命令定性**（在 `appQtPlayer.log` 上跑，字符串越少越好）：

```
grep -nE "avcodecDecoder object destroyed|hw device ref exclusively owned|decoder closed|CicadaReleasePlayer|player window object was destroyed|player window tree (created|destroyed)|top-level windows alive|reapPlayerWindow|captured D3D11 device|released the captured D3D11 device" appQtPlayer.log
```

**读法**（每轮开关应当各命中一次）：

| 命中情况 | 结论 | 下一步 |
|---|---|---|
| `avcodecDecoder object destroyed` **缺失** | **解码器对象没死 ⇒ 62 MB 表面池没放**（本轮 59 MB 就是它） | 查谁持有 `IDecoder`：`SMPAVDeviceManager` 是否被销毁（`~SuperMediaPlayer` 是否真的跑到 `mAVDeviceManager = nullptr`）、`CicadaReleasePlayer` 有没有命中；再查 `m_currentFrame`（渲染线程那一步） |
| 该行在，但 `hw device ref exclusively owned=0` | 设备引用**不是独占**：还有别人拿着（平台设备桥/缓存） | 查 `CicadaHardwareDevice`（本轮已修"覆盖持有者"的记账 bug）、以及是否有人缓存了 `provideHwDevice()` 的返回值 |
| 该行在、`exclusively owned=1` | 表面池这一侧**干净**，59 MB 不是它 | 转向 §2.6.3 的 Qt 进程级项与"每轮累积"；用 `top-level windows alive` 先分清是窗口树还是别的 |
| `player window object was destroyed` 缺失 | 窗口对象没被删（✕ 那一跳没生效或又被打回） | 看 `reapPlayerWindow` 那行在不在、`top-level windows alive` 是否递增 |
| `top-level windows alive` 关窗后不回落到基线 | **窗口树每轮留一个**（+30 MB/轮就是它） | 逐项查：`closeCommitted` 是否连上（`player window reported closeCommitted` 在不在）、`Qt.callLater` 是否执行（`reapPlayerWindow: destroying` 在不在）、`w.destroy()` 是否落到 `deleteLater`（有 QObject 父对象 ⇒ 会）、是否还有 QML/JS 或 C++ 侧持有该窗口 |
| `captured D3D11 device` 次数 > `released the captured D3D11 device` 次数 | 借了没还（设备及其名下分配不回收） | 本轮已修"持有者被覆盖导致跳过释放"的记账 bug；对照 `(window %p)` 是否成对 |
| 以上全部正常、内存仍停在 59 MB | 59 MB 属于**Qt/驱动进程级**（V4 堆/字体/RHI 缓存/驱动 device 分配） | 用 VMMap 的 Type 汇总（关窗前后各一份）区分 Heap / Private Data / Mapped；要"清"只能退进程 —— 这种情况要在报告里**明确说是进程级、不是泄漏** |

#### 2.6.5 【三轮回读】窗口树已排除；现在盯"表面池 + 设备"

用户提供的 3 轮开关日志（探针读数，行号为该文件）：

| 轮 | 建立时 WS / commit / 顶层窗口 | 关窗后 WS / commit / 顶层窗口 |
|---|---|---|
| 1 | 79.5 / 96.8 / **2** | 177.1 / 224.4 / **1** |
| 2 | 143.8 / 193.7 / **2** | 202.0 / 309.2 / **1** |
| 3 | 184.6 / 288.5 / **2** | 237.3 / 391.6 / **1** |

**① 窗口树这条彻底排除（下一轮不要再往这里查）**：每轮关窗后 `top-level windows alive` 都正确回到 **1**，
三条探针齐全（`reapPlayerWindow: destroying` / `the player window object was destroyed` /
`Main.qml: ... tree destroyed`）⇒ ✕ 修复生效，**+25~30 MB/轮 不是窗口树**。

**② 我上一轮那条 `hw device ref exclusively owned=0` 是误导，已改**：它在每轮关窗都是 0，
但**不代表"还有别人持有设备"**。代码依据（`avcodecDecoder.cpp`）：`initHwDecoder()` 里
`codecCont->hw_device_ctx = av_buffer_ref(deviceRef)`（设备句柄被**两处**引用），而
`close_decoder()` 里 `unref(hwDeviceRef)` 发生在 `avcodec_free_context()` **之前** ⇒
那一刻 refcount 必然为 2 ⇒ writable 必然 0，**这是正常状态**。本轮改成测**表面池本体**：
* **`[mem] hw frames ctx (the surface pool) exclusively owned=%d`**（`avcodec_free_context` 之前打）：
  `hw_frames_ctx` 就是那张表面数组（1080p 20 片 ≈62MB）。**`=0` ⇒ 还有一帧在外部活着钉着整池**
  （Qt 侧第一嫌疑是 `m_currentFrame` / 渲染线程那一帧）；`=1` ⇒ 池随 codec context 正常释放。
* 原那条的措辞也改成 `hw device ref refcount>1 is expected here (=N)`，不再误导。

**③ capture / release 其实是配对的（"no window recorded" 不是漏还）**：每轮开头那条
`released the captured D3D11 device (a different scene-graph device is taking over) (no window recorded)`
是**换设备分支里的空操作**（上一轮已正常还过 ⇒ `g_qtDevice` 本来就是空的）。真正的借/还每轮各一次：
* 借：`captured Qt's D3D11 device (window %p): our device refs now held=1`（本轮新增计数）；
* 还：`released ... (its scene graph went away) (window %p) (actually released=1, our device refs now held=0)`。
两条日志现在都打 `actually released=N` / `our device refs now held=N` ⇒ **空操作与真释放一眼可分**。
⇒ 设备桥（我们这一侧）借还平衡，且结构上不可能逐轮累积（只有一份 `g_qtDevice`）。
> 本轮顺带修了它的一个真 bug：原来"同一设备、另一个窗口"分支无条件 `g_capturedWindow = window`，
> 把"持有者"记到最后来的窗口名下 ⇒ 先前窗口回收时会因"不是记录里那个"而**跳过释放**。

**④ +85 MB/轮 commit（224→309→392）的性质**：窗口树已排除、设备桥借还平衡 ⇒ 嫌疑集中在
**每一轮那个 D3D11 设备名下没死的东西**：`hw frames 上下文`（表面池 62MB）+ 设备侧驱动/RHI 分配（~20MB）≈85MB。
用本轮新增两条探针就能拆开：
* `hw frames ctx ... exclusively owned=0` ⇒ **有一帧还在** ⇒ 池（62MB）被钉住
  （查 `m_currentFrame` / 渲染线程那一帧 / `m_pendingFrame`）；
* `=1` 且 `our device refs now held=0` 而 commit 仍每轮涨 ⇒ 池已放，涨的是**设备侧**
  （Qt RHI/驱动 per-device 分配）⇒ 查"新设备接管"时旧设备的 COM 引用是否真归零
  （FFmpeg 的 hw device 上下文是唯一不由我们记账的持有者）。

**⑤ 下一轮只需一条 grep**（预期每轮各命中一次）：
```
grep -nE "hw frames ctx|refcount>1 is expected|avcodecDecoder object destroyed|CicadaReleasePlayer|our device refs now held|actually released|top-level windows alive|player window object was destroyed" appQtPlayer.log
```

| 命中内容 | 期望 | 不对时说明什么 |
|---|---|---|
| `captured ... our device refs now held=1` | 每轮 1 次 | >1 ⇒ 我们没还上一轮那份 |
| `released ... actually released=1, ... held=0` | 每轮 1 次 | 没有 held=0 ⇒ 我们少还一份 |
| `actually released=0 ... (no window recorded)` | 每轮 1 次 | 正常（换设备空操作），**不是**漏还 |
| `hw frames ctx (the surface pool) exclusively owned=1` | 每轮 1 次 | **=0 ⇒ 表面池被外部帧钉住（62MB 就是它）** |
| `avcodecDecoder object destroyed` | 每轮 ≥1 次（视频+音频） | 0 次 ⇒ 解码器对象没死 |
| `top-level windows alive` 关窗后 =1 | 每轮 | >1 或逐轮递增 ⇒ 窗口树又漏了 |
| 以上全对、commit 仍每轮 +85MB | — | 池与设备引用都已归零 ⇒ 属 Qt/驱动 per-device 分配，用 VMMap 的 Type 汇总定性 |

#### 2.6.6 【定案】`exclusively owned=0` 的真相、顺序修复与新的决定性判据

三轮回读的关键三行（用户 `findstr` 结果，每轮各一次）：
`hw frames ctx (the surface pool) exclusively owned=0` →
`avcodecDecoder object destroyed` →
`released the captured D3D11 device ... actually released=1, our device refs now held=0`，
且关窗后 `top-level windows alive=1`。

**先把三件事钉死（避免下一轮重复排查）**：
1. **窗口树：已结案**（每轮 `alive` 回到 1，三条窗口探针齐全）。
2. **设备桥：借还平衡**（`our device refs now held=1 → 0`、`actually released=1`）。
   那条 `(no window recorded)` 是换设备分支的空操作，**不是漏还**。
3. **`hw frames ctx … =0` 这一条当时并不能证明"有外部帧"** —— 它测的是被我们自己钉住的状态。
   代码依据（`avcodecDecoder.cpp` 原来的顺序）：
   * `dequeue_decoder()` 里 `avcodec_receive_frame(codecCont, mPDecoder->avFrame)` 会把
     **最近解出的那一帧留在 `mPDecoder->avFrame`**（FFmpeg 只先 unref 再填，不替我们放），
     而零拷贝帧的 buf[] 握着的正是 hw frames 上下文（=那张 20 片表面数组）；
   * 而 `close_decoder()` 原来是 **先 `avcodec_free_context()`、后
     `av_frame_free(&mPDecoder->avFrame)`** ⇒ 探针所在那一刻 refcount 至少是 2
     （codec context 自己 + 我们那只复用帧）⇒ **无条件 `=0`**，与有没有外部帧无关。
   ⇒ 所以那条读数**不足以定罪**，本轮给出了能定罪的判据。

**顺序修复（两处，都不加开关/不加计时）**：
* **F1 `framework/codec/avcodecDecoder.cpp`（`close_decoder()`）**：把
  `av_frame_free(&mPDecoder->avFrame)` **提到 `avcodec_free_context()` 之前**。
  这样紧随其后的 `hw frames ctx … exclusively owned` 第一次变成**有信息量**的读数：
  报 0 才真意味着"codec context 之外还有帧"。
* **F2 `platform/QtPlayer/src/CicadaPlayerItem.cpp`（`~CicadaPlayerItem()`）**：
  在 `destroyPlayer()` **之前**先放掉本类持有的两帧（`m_pendingFrame` 上锁、
  `m_currentFrame` 直接 reset）。原来它们在 `destroyPlayer()` 之后、成员析构阶段才放，
  顺序恰好是"解码器先死、帧后放"，池的最后一份引用要等到成员析构才没 ——
  "关窗时表面池随解码器一起走"因此落空。
  **安全性**：与下面既有的 `invalidateFrameTextureCache()` / `releaseResources()` **同一前提**
  （析构时窗口/场景图已回收，Qt 不会再调度 `updatePaintNode()`，渲染线程没有读者）；
  `m_pendingFrame` 用 `m_frameMutex`（与 `destroyPlayer()` 里同一把锁）。
  **回归面**：播放期间不放帧，`m_releaseRenderState` 那条异步通道保持原样；
  只有"item 正在析构"这个已经不再渲染的时刻才动 `m_currentFrame`。

**新的决定性判据（本轮新增）**：
```
[mem] surface pool after decoder teardown (codec h264): externally pinned=0|1
```
做法：在 `close_decoder()` 里**先取一份临时引用**，等"我们自己的复用帧 + codec context
两边都放完"之后再问这张池外面还有没有人拿着（`av_buffer_is_writable` 判 refcount==1）。
* **`externally pinned=0`** ⇒ 池随解码器一起走了 —— **关窗后 62MB 不该出现在残留里**；
* **`=1`** ⇒ 解码器之外**确实还有帧活着**，62MB 记在它头上，下一步按 frames ctx 的引用链
  找那一帧（Qt 侧 `m_currentFrame` / 渲染线程那一帧 / 框架侧队列里没放掉的帧）。

**"两个解码器实例"的解释**：一轮关窗会打印**两套** `hw frames ctx` / `avcodecDecoder object destroyed`
—— **视频一个、音频一个**；音频那个显示 `hw frames ctx: none`（它没有表面池），
带池的是**视频**这个。本轮已把 **codec 名字**打进两条探针
（`... exclusively owned=%d (codec %s)` / `... (codec %s; ... none ...)`），
以后不必靠顺序猜；若某一轮出现三套，说明视频解码器被重建过（重建点有既有日志）。

**期望的下一轮读数**：关窗后 `externally pinned=0`、`~CicadaPlayerItem` 的析构读数
三轮持平并回到 ≈25 MB 量级（判据沿用 §2.7，那里已把读数要求写成"每轮报析构行"）。

#### 2.6.7 【定论 + 修复】渲染线程那一帧钉住表面池；释放点从"下次渲染"改到"关窗这一跳"

用户两轮日志（99 KB）的定论性证据：
* `surface pool after decoder teardown (codec hevc): externally pinned=1` —— **每次关窗都是 1**；
* `render thread released the previous frame and the D3D11 input view` **只在开窗时刻**出现
  （19:20:23.769 / 19:21:10.008），**关窗时刻（19:20:40.8 / 19:21:20.8）一次都没有**；
* 窗口树正常（`alive=1`）、设备桥平衡（`actually released=1, held=0`）。

**引用链（厘清）**：
```
D3D11VA 表面数组（hw frames ctx，20 片 ≈62MB）
   └─ 被任何一帧的 buf[] 引用
        ├─ ① 渲染线程手里的当前帧 m_currentFrame（Qt 侧唯一"长期"持有者）
        ├─ ② D3D11 输入视图 CicadaTextureD3D11::m_inputView（视频处理器输入视图）
        └─ ③ 短命引用：m_pendingFrame、ActiveDecoder 的输入/输出队列、
              mPDecoder->avFrame（已由 F1 改成"先放帧再 free 上下文"）
```
① 的释放原来**只**挂在 `updatePaintNode()` 的 `m_releaseRenderState` 分支上 ——
那条分支要等"渲染线程再画一帧"才执行，窗口一关就不再渲染 ⇒ **这一帧被握到下一轮开窗**
（正是日志里"释放行只在开窗出现"的原因）。② 由 `releaseResources()` 释放，而它是在
`onSceneGraphInvalidated()` 里调的 —— 那一跳**关窗时一定跑**（同一份日志里
`released the captured D3D11 device (its scene graph went away)` 就是它打的）。

**本轮修复（最后一刀）**：`platform/QtPlayer/src/CicadaPlayerItem.cpp` 的
**`onSceneGraphInvalidated()`** 里补上"放帧"（原来它一个帧都不放）：
```cpp
{ QMutexLocker locker(&m_frameMutex); m_pendingFrame.reset(); }
m_currentFrame.reset();                                        // ← 关键：渲染线程那一帧
if (m_textureBackend) m_textureBackend->releaseInputState();   // ← 输入视图（幂等）
if (m_textureBackend) m_textureBackend->releaseResources();
```
**为什么确定性**：本函数是**关窗时一定会在渲染线程上执行一次**的那一跳（事件驱动，
不是计时器、不是重试、不是等待）⇒"帧什么时候放"从"下次渲染"变成"关窗这一跳"。
**为什么不会 use-after-free**：本函数在**渲染线程**，`m_currentFrame` 的所有读写者
（`takeFrameForRendering()` / `updatePaintNode()`）都在同一线程；场景图此刻已失效，
Qt 不会再为该 item 调度 `updatePaintNode()`；GUI 线程同期的 `destroyPlayer()` /
`~CicadaPlayerItem` 只碰播放器与后端，不读 `m_currentFrame`。
上一轮的 **F2**（`~CicadaPlayerItem()` 里"先放帧、再 destroyPlayer()"）保留，作为第二道保证。

> ⚠ **量测口径的诚实说明**：`externally pinned` 是在**解码器内部**测的，而 ✕ 路径上解码器
> 在 `onClosing` 的 `source=""` 里就销毁了 —— 那一刻帧**按设计还活着**（要等场景图失效那一跳）。
> 所以**同一轮仍可能读到 `externally pinned=1`**，它不再等于"关窗后池没放"；
> 真正的终态判据是 **关窗后内存回到 ≈25 MB 量级且三轮持平**。
> 若要连"解码器销毁那一刻"也是 0（帧先死、解码器后死），需要把 `destroyPlayer()` 里的
> `m_player.reset()` 在"没有新片源"时**推迟到 `~CicadaPlayerItem()`**（那时 F2 已放帧）——
> 这会动到换源路径的顺序，属**下一步**且需单独回归；本轮没做，理由是不拿未编译验证的顺序
> 改动去碰换源路径。

#### 2.6.8 【句柄差分】三份 Process Explorer 导出说明了什么（含"哪一类没漏"）

三份文件其实是 **Process Explorer 的"系统进程列表 + 该进程句柄"合并导出**：
前约 380 行是进程列表，`Process: appQtPlayer.exe Pid: 20240` 之后才是该进程的句柄清单。
**关键限制**：清单里只有**具名句柄**（每行形如 `Type<TAB>Name`）——
三份分别 759 / 757 / 757 行、具名句柄 **151 / 128 / 127** 条，
而用户计数显示当时进程有 **1397 / 1268 / 1411** 个句柄
⇒ **导出只覆盖"有名字的那一部分"，无名句柄不在其中。**

按 Type 统计（三次快照）：

| 时刻 | Thread | Section | Semaphore | Mutant | Key | ALPC | Desktop/Directory/WindowStation | 具名合计 |
|---|---|---|---|---|---|---|---|---|
| S2 播放中 | **88** | 12 | 4 | 8 | 29 | 2 | 各 1~2 | 151 |
| S4 第一轮关窗后 | **67** | 10 | 4 | 8 | 29 | 1 | 各 1~2 | 128 |
| 第二轮关窗后 | **67** | 10 | 4 | 8 | 30 | 1 | 各 1~2 | 127 |

**结论（【已证实·读文件】）**：
1. 关窗确实回收了一批：**线程句柄 88 → 67（−21）**、Section 12 → 10（−2，正是
   `4f10HWNDInterface:*` 这类**窗口相关**段对象）、ALPC 2 → 1（CoreUI 端口）；
   其余（Mutant/Semaphore/Key/Desktop/Directory/WindowStation）三次完全持平。
2. **第一轮关窗 → 第二轮关窗，具名句柄一条都没涨（128 → 127）** ⇒
   **"每轮累积"不在具名句柄里**；线程、事件、段、注册表键、互斥体、信号量都可排除。
   这同时**排除了"线程/线程句柄泄漏"**（Thread 稳定 67）；代码侧也吻合：
   全仓只有 `platform/QtPlayer/main.cpp:595` 一处 `OpenThread`（崩溃/挂起诊断里，不在播放路径）。
3. 同一 TID 重复出现（`27512`×7、`28944`×2、`19032`×2、`23116`×2 …）**三份里次数完全相同**
   ⇒ 是稳定的重复打开（Qt/COM 打开主线程句柄），**不是**增长项。
4. 所以用户计数里"关窗只回收 129 个句柄、第二轮又涨回 1411、提交 +86MB/轮"的增量
   落在**无名句柄**上。与代码最吻合的映射：**每轮那个 D3D11 设备名下的 WDDM 分配
   （视频内存 section）及它派生的内核对象** —— 设备一死就一起还，设备不死就按轮累积；
   而"设备为什么不死"的上游正是 §2.6.6/§2.6.7 那条链：
   `帧 → hw frames ctx（表面池）→ device_ref → D3D11 设备 → WDDM 分配`。
   ⇒ 这条链不只解释 62MB，也解释 **+143 句柄/轮 与 +86MB 提交/轮**。
   【推测·待验证：用下面那次观测确认无名句柄/设备的释放时刻】

**下一次观测（不依赖 VS、不需要我们跑命令）**——二选一：
* `handle.exe -a -p appQtPlayer.exe`（Sysinternals，`-a` 会列出**无名**句柄）：
  在"播放中 / 关窗后 / 再开关一轮后"各跑一次，diff 输出；
* 或 Process Explorer 勾上 **Show Unnamed Handles and Mappings** 后重复同样三次导出。
**判据**：关窗后句柄总数回到 **≈S1 的 511±少量**、第二轮不再涨、提交同步回落；
若关窗后无名 `Section` 仍逐轮 +100 以上 ⇒ 印证"上一轮 D3D11 设备没死"，
下一步只需确认设备引用归零（已有探针 `our device refs now held=N`、
`released the captured D3D11 device ... (window %p) (actually released=1, ...)`）。

**已排除（不要再查）**：窗口对象/GDI（GDI 只多 3~4；窗口树 `alive=1`）、
具名的 Section/Event/Mutant/Semaphore/Key/ALPC、设备桥借还。
**（"线程"这一条已被下面的新数据推翻，见 2.6.8.1 —— 它正是本轮定点的那一类。）**

##### 2.6.8.1 【三份新导出】逐类差分：**逐轮涨的是 `Thread`（+27/轮）**

新一轮三份文件仍是 **Process Explorer 的"系统进程列表 + 该进程具名句柄"导出**
（不是 `handle64 -a` 的含无名格式）：749 / 757 / 749 行，句柄区依旧是 `Type<TAB>Name`。
**同一 PID（34640）** 内可比的是 S4（第一轮关窗后）与 S5（第二轮关窗后）：

| Type | S4 第一轮关窗后 | S5 第二轮关窗后 | **逐轮增量** |
|---|---|---|---|
| **Thread** | **59** | **86** | **+27** |
| Section | 11 | 12 | +1 |
| Key | 29 | 30 | +1 |
| Mutant | 8 | 9 | +1 |
| Semaphore | 4 | 4 | 0 |
| Event | （未计） | 4 | — |
| WindowStation / Desktop / Directory | 2 / 1 / 2 | 同 | 0 |
| **具名合计** | ~118 | ~145 | **~+27** |

**结论【已证实·读文件】**：
1. **具名句柄的逐轮增量几乎全部是 `Thread`（+27/轮）**；Section/Mutant/Key 各只 +1，
   Semaphore 与 WindowStation/Desktop/Directory 完全持平。
2. 用户计数总数 **1268 → 1411（+143）**，具名只 +27 ⇒ 其余 **~+116 仍是无名句柄**
   （这三份依旧只含具名部分：S4 里没有任何"类型 + 空名字"的行，已用 `\t$` 验证为 0 命中）。
3. 所以下一个定点是 **"每轮多出约 27 个线程（连同句柄）"**：这 27 个线程若还活着，
   默认 1 MB 栈 ⇒ 约 **+27 MB**，与 **+86 MB/轮** 的提交增长方向一致（其余仍指向设备/WDDM 侧）。

**代码侧核对【已证实·读码】**：
* `afThread`（框架里线程的主要来源）**句柄是关的**：`stop()` 里 `join()`（`afThread.cpp:223`）
  或 `detach()`（`:231`）之后 `delete mThreadPtr`（`:235`）；`forceStop()` 也是
  `detach()+delete`（`:243-244`）；`~afThread()` 同样 `join/detach + delete`。
* 全仓生产代码 `pthread_create` 只有一处：`framework/utils/TimedEventQueue/TimedEventQueue.cpp:60`，
  且 `:80` 有配对 `pthread_join` ⇒ 不是它。
* ⇒ +27/轮只可能是 **(a) 某个 `afThread`（或别的线程宿主对象）根本没被析构**（线程与句柄一直活着），
  或 **(b) 线程不是 afThread 开的**（Qt 线程池 / ffmpeg 解码内部线程 / 网络侧）。【推测·待验证】

**新增判据（本轮已修正探针：Release 可见、纯日志、无新成员、无平台宏）**：
`framework/utils/afThread.cpp` 里有三个文件级计数原子量
（`g_createdAfThreads` / `g_destroyedAfThreads` / `g_liveAfThreads`），**计数只绑对象生命周期**：
* **构造**打一行（带 `this` 指针，同名线程才配得上对）：
  `[mem] afThread '<名字>' @0x… created (live=N, created=M)`
* **析构**打一行（**无条件**递减：对象没起过线程也要减），带状态：
  `[mem] afThread '<名字>' @0x… destroyed (live=N, destroyed=M, hadThread=0|1)`
* `start()` / `stop()` / `forceStop()` **只记事件、不改计数**（它们只是"起/停线程"，对象还活着）：
  `… thread started` / `… thread stopped (object still alive)` / `… thread force-stopped (object still alive)`

> ⚠ **修正记录（重要教训）**：上一版把 +1 放在 `start()`、−1 放在
> `stop()/forceStop()/析构` 三处，而 `stop()` 并不销毁对象 ⇒
> **递减远多于递增，`live` 一路跌到 −50，读数完全无意义**。
> 计数必须绑在**对象生命周期**上，不能绑在"状态迁移"上。已改掉。

**自检（跑完一轮看最后一行就够）**：`created − destroyed == live`，且 **`live >= 0` 永不为负**。
（本类构造即建对象；`std::thread` 才是延迟到 `start()` 建的 —— 这正是"计数绑构造/析构、
而'线程起来/停下'只记事件"的原因。若以后有别的类是延迟创建对象，也要以"对象创建点"为准。）

**下一轮只需 grep**：`grep -n "\[mem\] afThread" appQtPlayer.log`
**判读**：
* 关窗后 `live` 回落到常驻水平、第二轮**不涨** ⇒ **afThread 干净**，
  线程来自 afThread 之外（Qt 线程池 / ffmpeg 解码内部线程 / 网络侧）⇒ 转查那些创建点；
* 关窗后不回落、逐轮 **+20 以上** ⇒ **有 afThread 对象的析构没走到**；此时
  **"只有 created、没有对应 destroyed 的 名字+指针"**就是直接点名（同名多个靠 `@0x…` 配对），
  不需要任何外部工具；
* 顺带核对线索：关窗瞬间成批出现的 `avFormatDemuxer / HLSStream / DashStream` 事件日志，
  配合 created/destroyed 两行即可算出**每轮这些内部 demuxer/stream 各建几个、析构几个**
  （直接关系到 +27 线程/轮的来源）。

##### 2.6.8.2 【逐名对账】结论：**afThread 干净**；+27 Thread/轮来自 afThread 之外

用户对称计数版日志（211 行 `findstr` 输出，**最后一行被截断**）逐名对账：

| 名字 | 轮1 created | 轮1 destroyed | 轮2 created | 轮2 destroyed | 结束时 live |
|---|---|---|---|---|---|
| `ApsaraPlayerService` | 1 | 1（L127） | 1（L130） | 截断区（同形应在末段） | ≈0 |
| `PlayerNotifier` | 1 | 1（L121） | 1（L131） | 1（L210） | 0 |
| `AbrManager` | 1 | 1（L129） | 1（L133） | 截断区 | ≈0 |
| `UTCTime` | 1 | 1（L11） | 1 | 1（L137） | 0 |
| **`DashSegmentTracker`** | **7** | **7**（L94/95/96/103/104/111/118） | **7** | **7**（L183/184/185/192/193/200/207） | 0 |
| **`DashStream`** | **3** | **3**（L102/110/117） | **3** | **3**（L191/199/206） | 0 |
| **`avFormatDemuxer`** | ~13（含每分片重开） | ~13（L43–L89 成对交替） | ~13 | ~13（L168–L204） | 0 |
| `AFActiveDecoder` | 2 | 2（L125/126） | 2 | 截断前 1（L164） | ≈0 |
| `timedVSync` / `AudioRender` | 各 1 | 各 1（L122/124） | 各 1 | 截断区 | 0 |
| **`CurlEasyManager`** | 1（L7） | **0** | 0 | 0 | **1（进程级常驻）** |
| **`AsyncJob`** | 窗口之前创建 | **0** | 0 | 0 | **1（进程级常驻）** |

**关键读数**：
* **自检通过**：`created − destroyed == live`，且 **`live` 全程非负**（上一版绑错生命周期的问题已修掉）。
* **第一轮关窗后 `live = 2`**（L129 `AbrManager destroyed (live=2)`）—— 正是**进程级常驻**的
  `CurlEasyManager` + `AsyncJob`（它们从不出现在 destroyed 里）。
* **第二轮关窗后**可见最后一行 `live=7`（L210/211，**日志截断**）。按第一轮同一段的形状
  （L121 → L129 之间还有 6 次 destroyed），第二轮剩余约 6 次销毁**就在截断区** ⇒ **预计同样回到 ≈1~2**。
  **截断对结论的影响**：不能把"最后可见 live=7"当成"第二轮多了 5 个"。
* **除进程级那 2 个之外，没有任何"只有 created、没有 destroyed"的对象**
  （`DashSegmentTracker` 7/7、`DashStream` 3/3、`avFormatDemuxer` 成对、`AFActiveDecoder` 2/2、
  其余全部配对）。

**结论【已证实·读日志+读码】**：
1. **afThread 干净**：两轮都回落到"进程级 2 个"，无泄漏对象、无计数漂移 ⇒
   §2.6.8.1 判据的第二个分支成立：**每轮 +27 个 `Thread` 句柄不是 afThread 对象泄漏来的**，
   也**没有需要修的对象**（本项无最小修复可做）。
2. 下一步查 **afThread 之外的线程创建点**（按可能性）：
   * **`avFormatDemuxer` 的高频重建**（本轮新事实）：播放中约 **每 1~2 秒一对 create/destroy**
     （L43→L89 共 ~13 对），每次都起一条新 `std::thread`。句柄在 `stop()` 的 join/detach 里关了，
     但**每秒一对线程创建/销毁**本身就是句柄/提交的抖动源，值得单独优化
     （DASH 每个分片重开一次内层 demuxer：`framework/demuxer/dash/DashStream.cpp` 的
     `reopenSegment`/`openSegment`）。【推测·待验证】
   * **Qt 线程池**：`platform/QtPlayer/src/DanmakuController.cpp` 的异步弹幕加载
     （`loadXmlFileAsync` → 线程池）——池线程复用，正常不逐轮增长，但要确认没有每轮新建
     `QThreadPool`/未回收的 `QRunnable`。【待确认】
   * **ffmpeg 解码内部线程**：`avcodec_open2` 按 `thread_count` 起线程、close 时 join；
     若框架设了较大的 `thread_count`，每轮会起一批（`framework/codec/avcodecDecoder.cpp` 的
     `init_decoder` 附近）。【待确认】
3. 每轮"建几个"已量化（后续对照用）：**`DashSegmentTracker` 7、`DashStream` 3、
   `avFormatDemuxer` ~13~20、`AFActiveDecoder` 2、`timedVSync`/`AudioRender`/`UTCTime` 各 1、
   `ApsaraPlayerService`/`PlayerNotifier`/`AbrManager` 各 1**；其中只有 `avFormatDemuxer` 是
   "播放中反复建销"的。

**下一次观测（若还要继续钉 +27）**：在关窗探针旁**再加一行"当前进程线程数"**
（`CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD)` 统计本进程 TID；`main.cpp:578` 已有同样用法），
关窗后读这一个数：若它逐轮 +27 而 afThread 的 `live` 恒为 2 ⇒ 线程确实来自外部创建点，
再按上面三个候选逐个加"创建/销毁成对"的日志定点。

#### 2.7 判据重写（用户照这个复测）

**桌面：连续开关播放器窗口 3 轮**，每轮关掉后在同一份 `appQtPlayer.log` 里核对：

| # | 该看的行 | 期望 | 它证明什么 |
|---|---|---|---|
| ① | `[mem] destroyPlayer: ...` / `decoder closed: ...` / `[mem] D3D11 zero-copy resources released ...` / `[mem] video texture backend released ...` / `[mem] scene graph invalidated ...` / `[mem] ~CicadaPlayerItem ...` | **每轮各一次** | 播放器侧六项全放（含**表面池**） |
| ② | `[mem] the player window object was destroyed (QQuickWindow::destroyed)` | **每轮一次** | **窗口对象真的被删了**（这条走 AF_LOGI，Release 也必定可见） |
| ③ | `[mem] reapPlayerWindow: destroying the player window object`（`console.warn`） | 每轮一次 | 销毁动作被触发；**不应再看到任何 `destroy skipped`** |
| ④ | `[mem] Main.qml: player window QML tree destroyed`（`console.warn`） | 每轮一次 | 整棵 QML 树（含弹幕引擎/图集）已随窗口销毁 |
| ⑤ | `[mem] player window tree created (CicadaPlayerItem ctor): working set X MB` 与下一行 `[mem] player window tree destroyed (~CicadaPlayerItem): working set Y MB` | **Y 逐轮持平**，且 Y 回到"只开主窗口 ≈25 MB"+小偏差 | **每轮净残留 ≈ 0**；Y 每轮 +25 MB ⇒ ②③④ 没做到；Y 停在 60~90 MB ⇒ **一次性残留**，先查 ① 里 `decoder closed` 是否真的出现过（表面池 62MB） |
| ⑥ | 任务管理器「内存」列（辅助） | 关窗后 ≈25 MB 量级、三轮持平 | 最终验收 |

**若停在 X MB，嫌疑对照**：
* X ≈ 25 MB → 达标；
* X ≈ 84 MB（本轮实测）→ **59 MB 残留**，优先按 §2.6.4 的两条判据分：`avcodecDecoder object destroyed`
  缺失 ⇒ 表面池（62.2MB）；在且 `exclusively owned=1` ⇒ 不是它，转向 Qt/驱动进程级；
* X ≈ 60–90 MB 且 `decoder closed:` **缺失** → **D3D11VA 表面池（62MB）没放**（解码器没走到析构，或帧引用还钉着）；
* X ≈ 50 MB 左右且 ⑤ 的 Y 逐轮 +25~30 MB → **窗口树还在泄漏**（②③④ 缺哪条就查那一跳）；
* **+30 MB/轮（本轮实测 84 → 114）**：先看 **析构行的 `top-level windows alive=N`** ——
  * 关窗后 N 不回落且逐轮 +1 ⇒ **窗口对象没销毁**（窗口树就是那 30 MB）；查
    `closeCommitted` 是否连上（`player window reported closeCommitted`）、
    `reapPlayerWindow: destroying` 是否打印、`player window object was destroyed` 是否打印；
  * N 正确回落但 `working set` 仍逐轮 +30 MB ⇒ 窗口树已回收，累积在窗口之外：
    按 §2.6.3 里 Qt 进程级几行逐项排除，最快的一刀是"整个会话不悬停进度条、不开弹幕"再测；
* X 随"是否悬停进度条"变化 → **Qt 全局 pixmap 缓存**（预览图功能的进程级成本，按用户要求不动）；
* X 稳定在 30–45 MB 且 ②③④ 齐全、`decoder closed` 也在 → 属于 Qt 进程级运行时常驻（V4 堆/字体/RHI 缓存），
  要确认就做一次"整个会话不悬停、不开弹幕"的对照复测；
* 借还计数不对（`captured D3D11 device` > `released the captured D3D11 device`）→ 设备引用没还，
  该设备名下的驱动/Qt 分配不回收（本轮已修持有者覆盖导致的跳过释放，并给两行都加了 `(window %p)` 便于配对）。

**每轮必报的两个读数**（都在日志里，无需任务管理器）：
1. 构造行 `[mem] player window tree created (CicadaPlayerItem ctor): working set X MB, private commit Y MB, top-level windows alive=N`；
2. 析构行 `[mem] player window tree destroyed (~CicadaPlayerItem): ...`。
**要求：第 2、3 轮的析构读数与第 1 轮持平**（这就是把"+30 MB/轮"钉成可测项的方式）；
不平持时按上面那条决策树逐层往下看，直到落到"某一类资源/某一个进程级缓存"。

**安卓**：返回键一次、关闭播放器一次，各自应出现完整一套（详见
`docs/ANALYSIS-ANDROID-RESOURCE-RELEASE.md` 的判据表）。

---

## 3. 主流播放器参照量级

> 来源标注：**[实测/官方]** = 有可点的来源；**[经验区间]** = 业界常见区间，本会话**没有**
> 核实到权威测量，只给范围、不给精确值。**不要把 [经验区间] 当实测引用。**
> 另外必须说清：Windows 任务管理器「内存」列是**私有工作集**，它**不含**被共享的 Qt/系统 DLL
> 页（那些是 file-backed、算 shareable）。所以同一个程序在"私有工作集"下看到的数字
> 会明显小于老文章里的 "RSS/PF 使用量"。

| 播放器 / 场景 | 1080p 播放时整进程量级（私有工作集） | 来源性质 |
|---|---|---|
| **mpv**（`--hwdec=auto`，Windows） | **60–150 MB**；注意 mpv 默认的解复用缓存很大（`--demuxer-max-bytes` 默认百 MB 量级），有人报到过 GB 级 | 区间**[经验区间]**；"缓存默认过大"有公开 issue 佐证：mpv-player/mpv#5847 |
| **VLC 3.x / 4.x**（Windows） | **100–250 MB** | 区间**[经验区间]**；"VLC 在 mac 上约 130 MB" 有 [HN 讨论](https://news.ycombinator.com/item?id=48612216) 佐证 |
| **PotPlayer** | **80–200 MB** | **[经验区间]** |
| **MPC-BE / MPC-HC**（LAV + madVR 的差异很大） | **60–200 MB**（madVR 可到 GB 级显存） | **[经验区间]** |
| **Chrome / Edge 播 1080p 网页视频** | **300–800 MB**（多进程合计） | **[经验区间]** |
| **一个只有空窗口的 Qt Quick 应用** | **25–60 MB** | **[经验区间]** |
| **纯硬解 1080p 的表面池本身** | 约 **60 MB**（20 片 NV12）；4K 约 **250 MB** | **[已证实（算术）]**：20×1920×1080×1.5 = 62.2 MB / 20×3840×2160×1.5 = 248.8 MB |
| 同一播放器"播放中 − 空载"的**增量** | 常见 **80–200 MB**（含表面池/DPB/缓冲） | **[经验区间]** |

**结论：158 MB 播 1080p + 弹幕 + 预览图，属于正常偏低的水平**，和 mpv/VLC 同量级，
明显低于浏览器。它**不是**"一个简单 QML 程序不该有的占用"——
那份判断（`platform/QtPlayer/docs/MEMORY.md` 开头引用的用户原话）在**解码器表面池**
这个前提下是可以说清的：进程里同时住着"Qt/QML 运行时 + 一个 GPU 解码器 + 一张 20 片的解码池"。

---

## 4. 降内存方案（按 收益/风险 排序）

排序原则（按任务要求）：**"该释放的没释放"（真 bug） > "缓存上限不合理"（默认值过大） >
"对象池/纹理池复用"**。所有方案都遵守硬约束：不新增配置开关、不引入墙钟死线/看门狗、
不用"丢帧/降画质"换内存。

### 4.1 【已改】整幅截屏快照：改成"按需生成、用完即还 + 停播兜底"（截图能力保留）

* **问题**：`CicadaPlayerItem::notifySnapshot()` 原来 `m_lastSnapshot = image;` 把**整幅**
  RGBA 画面挂成常驻成员（1080p 8.29MB / 4K 33.18MB），而且全仓 grep
  `m_lastSnapshot` **只有赋值、没有任何读取者** —— 也就是说这一整幅位图从"第一次悬停截屏"
  开始一直挂到停播，纯粹为"以后可能做的保存截图"占着。
  更要紧的是 `destroyPlayer()` 原来也没清它，所以**关窗后（窗口还活着、等 `deleteLater`
  的这段时间）它还是挂着**。
* **改动（两处，同一口径）**：
  1. `notifySnapshot()`（`CicadaPlayerItem.cpp:4050-4110`）：把缩小版交给 provider 之后，
     **在同一个函数里**还掉整幅 —— `m_lastSnapshot = QImage();`（`:4109`）。
     QImage 是引用计数，最后一份引用归零就**当场**释放像素缓冲。
     于是稳态（不悬停）下播放器**不持有任何整幅位图**，整幅只在那一次调用期间存在。
  2. `destroyPlayer()`（`:3320-3324`）：再放一次作为停播/换片源/关窗的**结构性兜底**，
     并记一条带字节数的日志（`:3375`）。
  3. `CicadaPlayerItem.h` 里那个成员的注释**重写成与实现一致的描述**：
     "按需生成 / 用完即还 / 停播兜底"三个时间点 + "这是整幅快照的唯一交接点，不是缓存"
     + 将来做保存截图的正确接入方式（现调 `requestSnapshot()`，交给消费者用完释放）。
* **收益**：**8.29 MB（1080p）/ 33.18 MB（4K）**，而且不只是"关窗时省"——
  **播放/暂停期间也不再常驻**（原来从第一次悬停一直挂到停播）。
* **风险**：≈0。截图能力一字未改（`requestSnapshot()` → `CaptureScreen()` → provider 这条链
  原样）；给 QML 气泡看的那一份是 320 宽（约 230KB，存在 `SnapshotImageProvider` 里），
  不在这个成员上，所以**画面行为完全不变**；全仓没有第二个读取者，所以没有行为变化。
* **同口径检查过的其它"整幅帧"成员**见 §2.4 的第二张表（结论：`m_bgraImage` 已同样处理；
  `CicadaVideoRender` 的截屏缓冲**本来就是按次 malloc/free**，正是本次采用的口径；
  `m_cachedTexture` 只是引用、`SnapshotImageProvider` 那份已缩到 320 宽、
  `DanmakuAtlas` 是文字图集不是视频帧、`VideoLibrary`/macOS 那两处是局部量）。

### 4.2 【已改】CPU 回退的 swscale 目标位图与上下文只在析构里释放

* **问题**：`CicadaVideoTexture::m_bgraImage`（`CicadaVideoTexture.h:161`，尺寸 = 视频原始画布：
  1080p **8.29 MB**、4K **33.18 MB**）和 `m_sws`（swscale 上下文）原先**只在
  `~CicadaVideoTexture()`**（`CicadaVideoTexture.cpp:50-64`）释放；
  而 `releaseResources()`（`:330-381`）—— 这个函数才是"这一套渲染资源不要了"的既有落点
  （场景图失效 / 后端重建 / item 析构都会走到）—— 只动了平台后端。
* **改动**：`platform/QtPlayer/src/CicadaVideoTexture.cpp:346-376`（新插入一段，在
  `m_prepared = false` 之前）：
  ```cpp
  if (m_sws != nullptr) { sws_freeContext(m_sws); m_sws = nullptr; }
  m_swsSrcWidth = 0; m_swsSrcHeight = 0; m_swsSrcFormat = 0;
  delete m_bgraImage; m_bgraImage = nullptr;
  ```
  纯内存操作、**没有平台宏**、不影响精度（`textureForFrameCpu()` 本来就是
  "分辨率/格式变了或 `m_sws == nullptr` 就重建"的写法，重建出来的转换完全一样）。
* **收益**：**8.29 MB（1080p）/ 33.18 MB（4K）**，**仅在该会话走了 CPU 回退路径时存在**
  （零拷贝成功时这两样根本不分配）。窗口关闭 → item 析构之间的那段时间不再白挂。
* **风险**：低。`releaseResources()` 本来就在渲染线程与析构路径上被调用（同一个函数里
  已经在 Release COM 对象），我只加了内存释放，**没有新增线程约定**。
  唯一理论风险是"渲染线程正在 `textureForFrameCpu()` 而 GUI 线程析构"，但这个竞态在
  改之前就存在（COM Release 同样不安全），且真实路径上 `sceneGraphInvalidated` 先发生。

### 4.3 【按用户要求：不做】悬停预览帧的解码位图会进 Qt 全局缓存

> **用户已明确决定：预览图一律不动。** 这一条以及 §4.4 **保持现状默认**，
> 下面只作为"已知成本"记录，不要再改（不要加 `cache: false`、不要改成按需取）。

* 现状：`ProgressRow.qml:755-763` 的在线预览 `Image` 没有 `cache: false`，
  所以每张悬停过的帧都会被解码并进全局 pixmap 缓存（每张 160×90×4 = 57.6 KB，
  上限由 Qt 默认值兜着）。
* 已知成本：最多 **~20 MB**（全片 360 帧都悬停过），而且**只有用户真的去悬停预览才付**。
* 结论：**不动。**

### 4.4 【按用户要求：不做】接口 `previewFrames` 全量常驻（4–29 MB，随片长线性）

> **用户已明确决定：预览图一律不动。** 保持现状默认。

* 现状：`Main.qml` 里 `preview.bin` 回调一次把**全片**的 data URL 存进 JS 数组并常驻
  （`\u001F` 分隔）。这是"缓存上限不合理"那一类，不是泄漏；而且它是用户明确要的
  "预览图"功能的直接成本 —— 关窗时窗口树被销毁（§2.1 跳 5/10），这一份也随之释放。
* 结论：**不动。**（"关窗就释放"这一条已经由窗口销毁满足，不需要额外清空。）
* 可以做的事（**用户已决定不做，这里只作备查**）：
  1. **关窗时清掉**：`Main.qml` 的 `onClosing` 里顺手 `playerView.previewFrames = []`。
     **收益接近 0**（窗口随后就被销毁，属性自然没了）——除非"窗口被复用/销毁失败"，
     那也只是兜底。**没有加**。
  2. 真要从根上省：改成**按需取档**（悬停到第 k 档才请求/解码那一张），
     而不是一次拿全片。需要接口支持按偏移取，要改 QML 与 mock 两侧。**不做。**
* 结论：**不动。** 它是"预览图"这个已交付功能的直接成本，且关窗后随窗口树释放。

### 4.5 【不要做】缩小 D3D11VA 的 20 片表面池

* 它是**本进程最大的一笔**（62 MB @1080p / 249 MB @4K），但池子大小是 FFmpeg 内部定的
  （`dxva2.c:619-634` + `decode.c:1108`），应用侧没有能把它降下来的选项；
  仓里已经有一条"三个不要动"的既有结论（`MEMORY.md` §3 的"别想缩小解码池"）。
* 也不要用"关硬解换软解"来省内存：1080p 软解的 DPB 是 12–25 MB，但 4K 软解 50–100 MB
  且 CPU 暴涨，属于**用画质/流畅换内存**，违反硬约束。

### 4.6 收益汇总

| 方案 | 省多少（1080p / 4K） | 何时生效 | 风险 | 状态 |
|---|---|---|---|---|
| 4.1 整幅快照"用完即还" + 停播兜底（截图能力保留） | **8.29 MB / 33.18 MB** | **播放期间就不再常驻**（用完即还）+ 停播/换片源/关窗兜底 | ≈0 | **已改** |
| 4.2 `releaseResources()` 放掉 `m_bgraImage` + `m_sws` | **8.29 MB / 33.18 MB**（仅 CPU 回退会话） | 场景图失效 / 后端重建 / 析构 | 低 | **已改** |
| 4.3 预览 Image 加 `cache: false` | ≤ ~20 MB（稳态，靠悬停累积） | 一直 | — | **用户要求不做** |
| 4.4 预览帧按需取 | 4–29 MB | 一直 | — | **用户要求不做** |
| 4.5 缩表面池 | 62 / 249 MB | — | 不可行 | 不要做 |

另外本次**新增了 9 个释放探针日志**（§2.5），用来在关窗后逐项核对"哪一类资源真的放掉了"——
它们不影响内存，但把"是不是地板"这个问题变成可判定的。

---

## 5. 给用户的测量方法（不需要我们来跑命令）

### 5.1 把 105 MB 拆开（Windows 上 vmmap 的等价物）

1. **任务管理器 → 详细信息**：右键列头 → 勾选
   `提交大小`、`工作集（内存）`、`专用工作集`、`句柄`、`线程`。
   四个时刻各记一行：**只开主窗口 / 播放中 / 暂停 / 关掉播放窗口之后**。
   * 只看"内存"列不够 —— 它不含共享的 Qt/系统 DLL 页；`提交大小`能看出地址空间里
     到底申请了多少（分配器 free list 会体现在这里）。
2. **Sysinternals Process Explorer**（免费）：
   进程属性 → **Performance** 页看 `Private Bytes` / `Virtual Size` / `Working Set`；
   再打开 **VMMap**（同一套工具）→ 对 `appQtPlayer.exe` 在四个时刻各存一份快照 →
   看下面的 **Type 汇总**（`Private Data` / `Heap` / `Image` / `Mapped File` / `Shareable`）：
   * 关窗后 `Private Data` 掉不下去 → 分配器/堆未归还（预期，且不是泄漏）；
   * 关窗后 `Private Data` 里有一大块**从不下降** → 那才是要抓的对象，
     再对着 §4 的表逐项排查。
3. **判"泄漏 vs 地板"的决定性实验（5 分钟）**：
   开关播放器窗口 **4 次**（每次播 10 秒再关），记录每次"关掉之后"的 WS/Private/句柄。
   地板**不随轮次抬升 = 不是泄漏**。仓库里 `MEMORY.md` §6 已经这样测过一次
   （WS 关掉后 125–140 MB 反复回到同一水平）。
4. **判"各块资源放没放、窗口真的销毁了吗"**（**探针已经加好了，不用再改代码**）：
   关窗后看日志/控制台里有没有那一组 `[mem]` 行 —— 清单、每行证明什么、
   以及缺哪一行代表哪一类资源没放，全部在 **§2.5 的两张表**里。
   一句话判据：**第 1–6 行都出现 = 播放器侧资源已全放，剩下的就是地板；
   只看得到 `reapPlayerWindow: window still visible, destroy skipped` = 窗口树还活着。**
5. **确认走的是哪条渲染/解码路**（不需要改代码，日志里就有）：
   * `D3D11 zero-copy is ready: decoded NV12/P010 textures are converted to RGBA ...`
     → 零拷贝开着；`first zero-copy frame: 1920x1080 NV12 texture` 会给**真实分辨率**；
   * `Qt's D3D11 device has no video support … (copy-back)` / `zero-copy presentation is not
     available on this setup` → 走 copy-back，那还要付 6 槽下载池（1080p 17.8 MB）；
   * `segment stream (DASH/HLS): buffer capped at 15s (maxBuffer=15000ms high=5000ms start=1000ms)`
     → 15 s 上限真的生效了；
   * `read-ahead gate: ... (activeQ=N queued=M ms)` → 实测队列深度。

### 5.2 我需要你提供什么（用来把 §1 的【推测】变成【已证实】）

1. **这条 DASH 的码率 + 分辨率 + 编码**（界面统计面板 `player.stats` 里有：
   `GetCurrentStreamInfo` 的分辨率/码率，或日志里的 `first zero-copy frame: WxH`）。
   → 用它把"15 s 缓冲 = 码率 × 秒数"算成实数（我这边只能按典型码率推）。
2. **硬解开没开、零拷贝有没有生效**（日志里有没有 `D3D11 zero-copy is ready`）。
   → 决定表面池（62 MB）与下载池（17.8 MB）哪一份在付。
3. **关窗后的一次内存快照**：任务管理器四个时刻的 `内存` + `提交大小` + `句柄`，
   最好再配一份 VMMap 的 Type 汇总（关窗前后各一份）。
4. **预览图的规模**（只为把 §0 表里那个 4–29 MB 区间收窄；**不会据此改预览**）：
   `preview.bin` 的字节数、切片条数、单帧 data URL 的大致长度，以及片长。
5. **片长**（决定预览帧条数与弹幕条数）。
6. **该视频的弹幕条数**（`danmakuLoaded` 信号会带 `count`，日志或界面上能看到）。
7. **【本轮最省事的一条】一条命令的输出**（见 §2.6.4 的 grep），以及每轮的**析构行读数**
   （`[mem] player window tree destroyed (~CicadaPlayerItem): working set X MB, ..., top-level windows alive=N`）。
   有了这两样，59 MB 与 +30 MB/轮 就能分别定性，不需要再猜。

---

## 附录 A：本次改动清单

### A.1 内存修复（释放行为）

| 文件 | 位置（改动后行号） | 改动 |
|---|---|---|
| `platform/QtPlayer/src/CicadaPlayerItem.cpp` | `destroyPlayer()` 内 `:3320-3324` | 停播/换片源/关窗/析构时释放整幅截屏快照 `m_lastSnapshot`（1080p 8.29MB / 4K 33.18MB） |
| `platform/QtPlayer/src/CicadaPlayerItem.cpp` | `notifySnapshot()` 内 `:4050-4110`（核心是 `:4109`） | **改成"按需生成、用完即还"**：把缩小版交给 provider 之后当场还掉整幅 → 播放期间也不再常驻一整幅位图 |
| `platform/QtPlayer/src/CicadaPlayerItem.h` | `m_lastSnapshot` 声明前的注释 | 重写为与实现一致的描述：按需生成 / 用完即还 / 停播兜底；明确"这是整幅快照的唯一交接点，不是缓存"；写清将来做"保存截图"的正确接入方式（保留能力） |
| `platform/QtPlayer/src/CicadaVideoTexture.cpp` | `releaseResources()` 内 `:346-392` | 场景图失效/后端重建/析构时释放 CPU 回退的 `m_bgraImage`（整幅 BGRA）与 `m_sws` 上下文；纯内存操作、无平台宏、不改精度 |

### A.2 释放探针（只加日志，不影响行为）

> 行号为**本轮改动后**的行号；锚点以函数名为准。带 ★ 的是本轮（✕ 路径修复）新增/改动的。

| 文件 | 行号 | 探针 |
|---|---|---|
| `platform/QtPlayer/src/CicadaPlayerItem.cpp` | `destroyPlayer()` 内 | ★ `[mem] destroyPlayer: ...`（含放掉的快照字节数） |
| `platform/QtPlayer/src/CicadaPlayerItem.cpp` | `updatePaintNode()` 的 `m_releaseRenderState` 分支 | `[mem] render thread released the previous frame and the D3D11 input view ...` |
| `platform/QtPlayer/src/CicadaPlayerItem.cpp` | `onSceneGraphInvalidated()` 内 | ★ `[mem] scene graph invalidated: ...` |
| `platform/QtPlayer/src/CicadaPlayerItem.cpp` | `~CicadaPlayerItem()` 内 | ★ `[mem] ~CicadaPlayerItem: ...` + `[mem] player window tree destroyed (~CicadaPlayerItem): working set X MB, private commit Y MB` |
| `platform/QtPlayer/src/CicadaPlayerItem.cpp` | **构造函数**内（本轮新增） | ★ `[mem] player window tree created (CicadaPlayerItem ctor): working set X MB, private commit Y MB` |
| `platform/QtPlayer/src/CicadaPlayerItem.cpp` | `onSceneGraphInitialized()` 内（本轮新增） | ★ **`[mem] the player window object was destroyed (QQuickWindow::destroyed)`** —— **"窗口对象真的被删了"的权威判据（C++/AF_LOGI，Release 必定可见）** |
| `platform/QtPlayer/src/CicadaVideoTexture.cpp` | `releaseResources()` 内 | ★ `[mem] video texture backend released: CPU fallback bitmap N bytes ...` |
| `platform/QtPlayer/src/CicadaTextureD3D11.cpp` | `releaseResources()` 内 | ★ `[mem] D3D11 zero-copy resources released: RGBA output texture WxH ...` |
| `framework/codec/avcodecDecoder.cpp` | `close_decoder()` 末 | ★ `decoder closed: codec context freed, hardware device reference released, ...`（**表面池 62.2MB@1080p 的释放点**；无平台宏） |
| `platform/QtPlayer/Main.qml` | `:139` 一带 | ★ `console.warn("[mem] Main.qml: player window QML tree destroyed")`（**改 warn**：Release 下 console.log 不可见） |
| `platform/QtPlayer/HomeWindow.qml` | `reapPlayerWindow()` 内 | ★ `console.warn("[mem] reapPlayerWindow: destroying the player window object")`（**已删掉原来的 `destroy skipped` 分支**） |
| `platform/QtPlayer/HomeWindow.qml` | `ensurePlayerWindow()` 内 | ★ `console.warn("[mem] player window reported closeCommitted -> scheduling reap")` / `... became invisible -> scheduling reap` |
| **安卓** `mediaPlayer/media_player_api.cpp` | `CicadaReleasePlayer()`（`:45` 起） | ★ `[mem] CicadaReleasePlayer: destroying the player object (decoder, hardware surface pool, packet/frame queues, demuxer and data source)`（桌面+安卓共用收口） |
| **安卓** `framework/codec/Android/mediaCodecDecoder.cpp` | `close_decoder()` 的 `if (mbInit)` 内 | ★ `[mem] MediaCodec decoder closed: codec stopped and released (its output surface reference goes with it)` |
| **安卓** `framework/render/audio/Android/AaudioRender.cpp` | `closeStream()` 内 | ★ `[mem] AAudio output stream closed` |

### A.2b ✕ 关闭路径的行为修复（本轮）

| 文件 | 位置 | 改动 | 为什么 |
|---|---|---|---|
| `platform/QtPlayer/Main.qml` | 新增 `signal closeCommitted()`；`onClosing` 里发出 | `onClosing` 做完 `stop()+source=""` 之后**确定性**地通知创建方"该销毁我了" | 原来销毁只由 `visibleChanged` 触发，而 `visible` 在 ✕（原生关闭）路径上不保证变 false |
| `platform/QtPlayer/HomeWindow.qml` | `ensurePlayerWindow()`：连 `closeCommitted` → `Qt.callLater(home.reapPlayerWindow)`；保留 `visibleChanged` 作兜底 | 两个触发点汇进同一个幂等函数 | 不再有"两个触发点都不动手"的空档 |
| `platform/QtPlayer/HomeWindow.qml` | `reapPlayerWindow()`：**删掉 `if (playerWindow.visible) return` 闸** | 只保留 `playerWindow === null` 幂等判据，然后置空引用 + `destroy()` | 这是本轮**唯一的行为修复点**：销毁不再依赖平台读数；"close 已走完"的时序由 `Qt.callLater`（事件循环）保证 |
| `platform/QtPlayer/HomeWindow.qml` | 新增 `property var playerComponent` | `Qt.createComponent(Main.qml)` **只建一次** | 去掉每轮一个 JS 所有权组件（等 GC）的累积 |

### A.2c 【84/114 这一轮】新增/加强的判据与一处记账修复

| 文件 | 位置 | 改动 | 为什么（对应哪个数字） |
|---|---|---|---|
| `framework/codec/avcodecDecoder.cpp` | `~avcodecDecoder()` | **新增无条件** `[mem] avcodecDecoder object destroyed (...)` | `close_decoder()` 有 `mPDecoder == nullptr` 早退（不打印）⇒ 原来"没有 `decoder closed`"是**歧义**的。这一条让"解码器对象（连同 hw frames 上下文与表面池）到底死没死"变成一次grep可判 → 钉 **59 MB** |
| `framework/codec/avcodecDecoder.cpp` | `close_decoder()` 的 `hwDeviceRef` 分支 | **新增** `[mem] hw device ref exclusively owned=%d`（用 `av_buffer_is_writable`） | 设备引用若不是独占（=0），说明表面池名下的东西还有别人持有 → 决定 59 MB 能不能在解码器这一侧释放 |
| `platform/QtPlayer/src/CicadaPlayerItem.cpp` | `logProcessMemoryProbe()` | 输出追加 **`top-level windows alive=%d`**（`QGuiApplication::topLevelWindows()`） | **不用任何外部工具**判定"窗口对象有没有被销毁"：关窗后不回落且逐轮 +1 ⇒ 每轮留一个窗口，+30 MB/轮 就是窗口树 |
| `platform/QtPlayer/src/CicadaHardwareDevice.cpp` | `captureFromSceneGraph()` 的"同一设备"分支 | **修**：不再无条件 `g_capturedWindow = window`，只在"当前没有持有者"时记名 | 原来把"持有引用"记在最后来的窗口名下 ⇒ 先前窗口回收时 `releaseFromSceneGraph` 因"不是记录里那个"**跳过释放**，我们那份 `AddRef` 再也没人还（设备连同驱动分配不回收）。这属于"能对不上账"的真 bug，与 +30 MB/轮 的设备侧嫌疑直接相关 |
| `platform/QtPlayer/src/CicadaHardwareDevice.cpp` | `releaseDeviceLocked()` | 释放日志追加 **`(window %p)`**，并补了 `no window recorded` 分支 | 与 `captured Qt's D3D11 device ... (window %p)` 配对即可数"借了几次/还了几次"；借 > 还就是要查的漏 |

### A.3 本次**没有**改的东西（按用户口径）

* **预览图一律不动**：`ProgressRow.qml` 没有加 `cache: false`，`previewFrames` 没有改成按需取
  （§4.3/§4.4 已标注"按用户要求不做"）。它造成的 **Qt 全局 pixmap 缓存**进程级增长已如实列进
  §2.6.3 清单（含"怎么验证是不是它"）。
* **没有删** `m_lastSnapshot` 或任何截图能力：`requestSnapshot()` / `CaptureScreen()` /
  `SnapshotImageProvider` / `snapshotRevision` 这条链一字未改（只把整幅改成"用完即还"）。
* **没有**动 `~CicadaTextureD3D11()` 那个"故意不 Release"的空析构：已用代码事实证明
  它在真实关窗路径上不可能留下没回收的纹理（§2.1 末段），改它反而会引入
  "渲染线程还在用就 Release"的 use-after-free 风险，属于"漏而不崩"的既有取舍。
* **没有**改直播窗口（`LiveMain.qml` / `reapLiveWindow`）的那套同形逻辑：本轮范围是点播播放器窗口。

全部改动都**没有**新增配置开关、**没有**引入墙钟死线/看门狗/超时兜底、**没有**任何
"丢帧/降画质换内存"、**没有**在 L1（`mediaPlayer/` 与 `framework/` 非平台代码）加平台宏。
新增的唯一类成员 `m_windowDestroyedProbeAttached` **追加在 `CicadaPlayerItem.h` 类末尾**；
没有新增虚函数。

## 附录 B：本文用到的关键 file:line 索引

> ⚠ 行号是本次改动之后的。**锚点以函数名为准**（新增日志会让后面的行号整体后移）。

```
缓冲与队列
  mediaPlayer/player_types.cpp:16-18,31          START/HIGH/MAX 默认 1s/5s/40s
  mediaPlayer/MediaPlayerConfig.cpp:16            另一份默认 50000ms（C 门面）
  mediaPlayer/SuperMediaPlayer.cpp:110            MAX_VIDEO_READ_AHEAD_US = 2s
  mediaPlayer/SuperMediaPlayer.cpp:1196-1202      maxBufferDuration 按毫秒收下 ×1000
  mediaPlayer/SuperMediaPlayer.cpp:2282-2369      读前闸门（日志 read-ahead gate）
  mediaPlayer/SuperMediaPlayer.cpp:2382-2397      上限无条件生效
  mediaPlayer/SuperMediaPlayer.cpp:2372-2381      历史事故：6978 包 ≈ 233s（4K）
  mediaPlayer/buffer_controller.h:96-98           三条包队列
  framework/demuxer/avFormatDemuxer.h:166         MAX_QUEUE_SIZE = 60 包
  platform/QtPlayer/src/CicadaPlayerItem.cpp:3155           DASH/HLS cap 15s/5s/1s（segmentStream）
  platform/QtPlayer/src/CicadaPlayerItem.cpp:3183-3190     本地文件 5s/3s/0.5s
  platform/QtPlayer/src/CicadaPlayerItem.cpp:3233-3240     连续流直播 4s/2s/0.5s
  mediaPlayer/abr/AbrBufferAlgoStrategy.cpp:115,437-446 ABR 与 15s 配套
解码与帧
  mediaPlayer/SuperMediaPlayer.cpp:39             VIDEO_PICTURE_MAX_CACHE_SIZE = 2
  mediaPlayer/SuperMediaPlayer.cpp:3677-3689      帧队列上限取 1（硬解）或 2（软解）
  mediaPlayer/SuperMediaPlayer.cpp:7598           mPictureCacheType 的赋值点
  mediaPlayer/SuperMediaPlayer.cpp:117,3781       音频 PCM 队列下限 9 帧
  framework/base/media/AVAFPacket.cpp:208-212     av_frame_clone（引用计数，不拷像素）
  framework/base/media/AVAFPacket.cpp:170-197     唯一真 memcpy 整帧的构造（Android/OHOS 用）
  framework/codec/avcodecDecoder.cpp:966          new AVAFFrame(outFrame)
  framework/codec/avcodecDecoder.cpp:594-672      retrieveHwFrame / 6 槽下载池轮转
  framework/codec/avcodecDecoder.h:76             CICADA_HW_SW_FRAME_POOL = 6
  framework/codec/avcodecDecoder.cpp:926-937      零拷贝直通：不下载
零拷贝纹理
  platform/QtPlayer/src/CicadaTextureD3D11.h:158  m_outputTexture（唯一一张，8.29/33.18MB）
  platform/QtPlayer/src/CicadaTextureD3D11.cpp:108-115  析构故意不 Release（已证明关窗路径上不会发生，见 §2.1 末段）
  platform/QtPlayer/src/CicadaTextureD3D11.cpp:207-223  releaseInputView()
  platform/QtPlayer/src/CicadaTextureD3D11.cpp:225-305  releaseResources()（含 :300 释放探针）
  platform/QtPlayer/src/CicadaVideoTexture.cpp:104-114  prepare() 探测
  platform/QtPlayer/src/CicadaVideoTexture.cpp:274-278  CPU 回退 m_bgraImage 分配
  platform/QtPlayer/src/CicadaVideoTexture.cpp:330-392  releaseResources()（含 :391 释放探针）
  platform/QtPlayer/src/CicadaPlayerItem.cpp:3487-3520  onSceneGraphInvalidated（含 :3514 探针）
预览图 / 进度条
  platform/QtPlayer/Main.qml:775-786              preview.bin → previewFrames（按用户要求保持默认）
  platform/QtPlayer/controls/ProgressRow.qml:60    property var previewFrames
  platform/QtPlayer/controls/ProgressRow.qml:699-763  取档 + Image（无 cache:false；按用户要求保持默认）
  platform/QtPlayer/src/CicadaPlayerItem.cpp:217-258   CaptureScreen 回调（整幅 RGBA QImage）
  platform/QtPlayer/src/CicadaPlayerItem.cpp:4050-4110 notifySnapshot（:4109 用完即还整幅 + 320 宽预览）
  platform/QtPlayer/src/SnapshotImageProvider.h:105-128 publish() 的进程 static
  platform/QtPlayer/src/VideoLibrary.cpp:43,391-460    首页缩略图落盘、不常驻
弹幕
  platform/QtPlayer/src/DanmakuAtlas.cpp:19,74-78  1024x1024 ARGB32 = 4.19MB QImage
  platform/QtPlayer/src/DanmakuAtlas.cpp:98-111    图集满了整体重烤（有界）
  platform/QtPlayer/src/DanmakuController.h:143-158 gpuLayer：GPU 路径不建 QVariantList
  danmaku/src/DanmakuEngine.cpp:169-205            _items 的 load/clear/reset
关窗路径（全部行号为本次改动后；§2.1 有逐跳审计表）
  platform/QtPlayer/Main.qml:124-127 onClosing（只藏窗口，用清片源释放）
  platform/QtPlayer/Main.qml:139     Component.onDestruction 探针
  platform/QtPlayer/HomeWindow.qml:1249-1278  ensurePlayerWindow + visibleChanged
  platform/QtPlayer/HomeWindow.qml:1544-1566  reapPlayerWindow → w.destroy()（:1556/:1562 探针）
  platform/QtPlayer/src/CicadaPlayerItem.cpp:789   setSource（:850 同步调 destroyPlayer）
  platform/QtPlayer/src/CicadaPlayerItem.cpp:722-751  ~CicadaPlayerItem（:747 探针）
  platform/QtPlayer/src/CicadaPlayerItem.cpp:3262-3386 destroyPlayer()（:3320-3324 快照、:3356 挂起帧、:3375 探针）
  platform/QtPlayer/src/CicadaPlayerItem.cpp:3702-3730 updatePaintNode 里的 m_releaseRenderState 分支（:3721 探针）
  mediaPlayer/SuperMediaPlayer.cpp:963-1078       Stop()：包队列/帧队列/解复用器/数据源
  mediaPlayer/SuperMediaPlayer.cpp:480-522        ~SuperMediaPlayer：解码器（= 表面池）在这里走
  mediaPlayer/media_player_api.cpp:40-45          CicadaReleasePlayer → delete pPlayer
  framework/codec/avcodecDecoder.cpp:261-319      close_decoder（:317 探针）
  framework/data_source/curl/CurlMulti.cpp:202-213 网络线程真正 delete 连接
已排除
  framework/data_source/cache/ISliceManager.cpp:19,29-48  100MB 切片池（生产路径无消费者）
  framework/codec/avcodecDecoder.h / cacheModule/cache/CacheConfig.h:38  mEnable=false
  platform/QtPlayer/ 全目录无 SetCacheConfig 调用
  framework/data_source/curl/CURLConnection2.cpp:21,25    256KB 环 + 64KB curl 缓冲
```
