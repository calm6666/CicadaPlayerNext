# CicadaPlayerNext 架构重设计（Stage 1：设计稿）

> 本轮**只产出设计**，不写功能代码，不动功能类改动（镜像 / 色觉 / 截图存相册 / 设置持久化 / 清晰度列表）。
> 参考：mpv（demux 线程 + hr-seek + 每解码器队列）、VLC（input→es_out→每 ES decoder 线程 + FIFO）、ijkplayer（线程模型参考）。
> 规则：每个设计决定尽量附**上游源码证据 URL + 原文片段**；查不到的写 **未证实**；不写"将来可能"。

---

## 1. 分层与边界

| 层 | 职责 | 允许依赖 | 禁止 |
|---|---|---|---|
| **L3 端侧**（Qt/Android/iOS/Harmony UI） | 交互、seek 滑条、清晰度菜单、画布/纹理绑定、生命周期 | L2 | 不得自带"位置地板/seek 状态机"第二套实现；不得推断内核时间轴 |
| **L2 控制层**（player 门面 / 消息） | 用户意图 → **请求对象**（`SeekRequest`、`SwitchRequest`）；对外事件（duration/position/EOS/switch 终态） | L1 | 不得在逐帧路径上做阻塞调用；不得直接调解复用器/解码器句柄 |
| **L1 核心引擎**（跨平台，纯 C++，**零平台 `#ifdef`**） | demux 线程与服务、每流队列、seek 执行、**每流解码线程 + 有界 FIFO**、主时钟与 A/V 对齐、渲染调度（把帧/采样交给 L0 输出） | L0 | 不得含平台头/宏；不得改对外时间轴语义；不得用墙钟"猜"收敛 |
| **L0 平台 HAL**（每平台自己实现） | 解码器（MediaCodec / VideoToolbox / OH AVC / FFmpeg SW）、输出（Surface·ANativeWindow / AudioTrack / CoreAudio / Qt RHI·QAudioSink）、单调时钟与线程 | OS | **不得改时间轴语义**（不得改写 pts、不得自造 seek 落点、不得缓存跨 seek 数据） |

依赖方向单向 **L3 → L2 → L1 → L0**。反向引用一律视为架构缺陷。

**mpv 证据**（每条一段原文）：
- demux 独立线程：`if (mp_thread_create(&in->thread, demux_thread, in))` / `mp_thread_set_name("demux");`
  https://github.com/mpv-player/mpv/blob/master/demux/demux.c （经镜像 https://cdn.jsdelivr.net/gh/mpv-player/mpv@master/demux/demux.c 取证）
- 每流独立读游标/队列：`// demux user state (user thread, somewhat similar to reader/decoder state)`；`struct demux_stream` + `struct demux_queue`（`demux_stream` 内有 `reader_head` 与按流 index）
  同上。
- 解码器 ≠ demux：解码包装器是独立 filter，带自己的队列与上限：`struct dec_queue_opts { bool use_queue; int64_t max_bytes; int64_t max_samples; double max_duration; };`，视频默认 `max_bytes = 512*1024*1024, max_samples = 50, max_duration = 2`，音频默认 `max_bytes = 1*1024*1024, max_samples = 48000, max_duration = 1`
  https://github.com/mpv-player/mpv/blob/master/filters/f_decoder_wrapper.c

**VLC 证据**：每个 ES 一个 decoder 线程 + FIFO（`vlc_clone(&p_owner->thread, DecoderThread, p_owner)`、`block_fifo_t *p_fifo`），FIFO 有上限且满则等待（`while (vlc_fifo_GetCount(p_fifo) >= 10) vlc_fifo_WaitCond(p_fifo);`）
https://code.videolan.org/videolan/vlc/-/raw/master/src/input/decoder.c
（以上 VLC 片段取证于本会话较早一轮；`block_fifo_t` 现已是指向 `vlc_fifo_*`/`vlc_frame_*` 的别名宏：`#define block_FifoPut vlc_fifo_Put`、`#define block_ChainRelease vlc_frame_ChainRelease`
https://code.videolan.org/videolan/vlc/-/raw/master/include/vlc_block.h ）

---

## 2. 精确 seek 设计（目标：落到用户点的帧附近，与分片多长无关）

**决定 S1 — seek 归属：控制侧只发请求，demux 线程执行。**
mpv 原文：`static bool queue_seek(struct demux_internal *in, double seek_pts, int flags, ...)`，注释 `// called locked, from user thread only`，消费侧在 demux 线程 `if (in->seeking) { execute_seek(in); ... }`，且 `execute_seek()` **解锁后**才调 `desc->seek()`。
证据：https://github.com/mpv-player/mpv/blob/master/demux/demux.c
→ 我们的 `SMPMessageControllerListener` 里同步 `mDemuxerService->Seek(...)`（`SMPMessageControllerListener.cpp:733`）是**违反项**；改为置 `SeekRequest`，由 demux/服务线程执行，消费侧只**查状态**推进。

**决定 S2 — hr-seek 语义：hr 是"弱提示"，真正的"到帧"由解码侧完成。**
mpv 原文：`#define SEEK_HR (1 << 5) // hr-seek (this is a weak hint only)`、`#define SEEK_FORWARD (1 << 2) // prefer later time if not exact (if unset, prefer earlier time)`、`#define SEEK_BLOCK (1 << 6) // upon successfully queued seek, block readers`；demux 侧确有 `queue_seek(in, target, SEEK_SATAN | SEEK_HR, false);`
证据：https://github.com/mpv-player/mpv/blob/master/demux/demux.h 、demux.c（同上）
→ 设计：demux 只保证**关键帧/分片起点**；"精确到目标帧"= 解码线程从起点**继续解码并丢弃**直到 `pts >= target`（或 `SEEK_FORWARD` 时取第一帧 ≥ target），再交给输出层。这样"落点距离"由**解码推进**决定，与分片长度、GOP 长度解耦。
**未证实**：mpv `--hr-seek` 的用户级默认值与文档原文（镜像 `DOCS/man/options.rst` 返回 `application/octet-stream` 或不可达，本轮未取得）。

**决定 S3 — DASH 侧定位：不假设索引精度。**
`SegmentTemplate@duration`/`SegmentTimeline`/`SegmentList` 只给**分片起点**；要更细只有两条：① 分片内 sample 索引（`sidx`/`tfdt`+`trun`）② **解码丢弃**逼近。设计：**② 为必选、① 为加速项**（有索引就跳过无谓解码，没有也绝不牺牲精度）。
→ 与"分片 20s 就只能落 20s 之前"彻底脱钩；当前实现落在分片起点即上屏，是**违反 S2** 的行为（日志：`reqUs=198919000 -> segNum=10` / `tracker positioned at segNum=9` / `pts=179879700, 19039 ms before the seek target`）。

**决定 S4 — 落点与位置上报一致：唯一权威 = 已上屏帧。**
规则：seek 期间对外报"最后确认的真实位置"（保持单调，不回退、**也不跳到目标**）；落点帧（hr-seek 后的目标帧）上屏那一刻起，位置从该帧时间轴单调前进。
→ 现状违反：内核先把上报地板钉到目标（`SuperMediaPlayer.cpp:698-705`），当落点与目标差 > `SEEK_FLOOR_GIVEUP_US(15s)`（`:251`）时放弃地板并回落到落点（`:1172-1177`），端侧又"乐观上报目标"（`platform/QtPlayer/src/CicadaPlayerItem.cpp:2521-2524`）⇒ 进度条"跳到目标再弹回"。新设计里这三处**合并为一条规则**（见 S4）。

**决定 S5 — seek 失败退路（必须确定性）**：无索引且解不到目标 → 停在**已确认的最近可行帧**（关键帧落点）并**只上报该真实位置**；对外 seek 完成事件照常发出（终态唯一）；绝不为了"到达目标"反复重试或阻塞数据供给。

---

## 3. 无缝切清晰度设计

**现状为什么"大概率失败"**（本仓库内证据）：切档提交依赖"pending 帧追上主时钟"这类**墙钟判据**（8s/18s 两级超时）与单一消息线程上的同步 flush；一旦 pending 流落后，就走"超时失败"收尾。
**决定 S6 — 切档 = 旁路新流，不触碰现渲染路径。**
- 新 rendition 在**自己的解码线程 + 自己的有界 FIFO**里预热（VLC 模型，见 §1 证据）；
- **旧流继续按原节拍供帧**，直到提交点成立；
- **提交点（唯一、状态判据）**：新流已产出 `pts >= 主时钟` 的第一帧，且该帧已解码完成 → 原子替换输出源的帧队列归属；**不用任何计时器**；
- 提交前新流的所有帧一律丢弃（不允许"新旧混帧"）；
- **失败回退**：只要上述条件在"旧流仍可继续供帧"的前提下不成立，就**放弃新流**（关流、回收解码器）、旧流不受影响 ⇒ 切档失败**不可能**导致播放中断或音画错位。
- **flush 只动本流且异步**：VLC 原文 `block_ChainRelease(vlc_fifo_DequeueAllUnlocked(...))` 与 "Flushing is fully asynchronous"（decoder.c，同上 URL）。
**未证实**：mpv/VLC 是否有与我们"提交点"完全等价的判据（VLC 的 vout/aout 切换在 `vout_FlushAll`/`vlc_aout_stream_Flush` 层面，细节本轮未取证）。

---

## 4. 不卡断 / 不断声 / 音画同步：硬性规则（铁律）

| # | 铁律 | 依据 |
|---|---|---|
| R1 | **任何一流的 decode / flush / 等待，不得阻塞另一流的数据供给** | VLC 每 ES 独立 decoder 线程 + FIFO（decoder.c）；mpv 每解码器独立队列（f_decoder_wrapper.c） |
| R2 | 所有队列**必须有界**，满则**等待/背压**而不是丢真实数据 | mpv `dec_queue_opts`（512MB/50 样本/2s；1MB/48000/1s）；VLC `>= 10` 包即 `WaitCond` |
| R3 | **音频输出永不被清空到"无数据可播"**；设备侧 flush 只允许清"已过期"数据，且必须立即有后续数据接手 | **工程约定**（本仓库真机教训：设备 pause/flush/start 往返实测 ≈2ms，问题不在耗时而在"清空后没有接手"） |
| R4 | **主时钟归属唯一**；seek / 切档后的重锚规则**唯一**且由事件驱动 | **工程约定**（mpv `--video-sync` 有明确主时钟选项，但其默认值与实现**未证实**：`DOCS/man/options.rst` 本轮取不到） |
| R5 | **禁止计时器 / 看门狗**；所有等待必须是状态判据（"数据到了/帧上屏了/队列空了"） | **工程约定**（本仓库已验证：墙钟超时只带来误判与"卡死半天"） |
| R6 | **对外位置单调不回退**；seek 期间不跳到目标也不弹回 | 见 S4 |
| R7 | 解码器/输出对象的**创建与销毁只发生在其所属流线程**；跨线程只传数据不传句柄语义 | VLC/ mpv 模型（decoder 自持线程与 FIFO）；**工程约定**（HAL 生命周期） |
| R8 | **平台宏只允许出现在平台层与构建开关里**：L0 后端（音频输出、视频渲染、硬解、文件/线程/原子包装、平台能力探测）可自由使用 `__ANDROID__`/`TARGET_OS_IPHONE`/`OHOS`/`_WIN32`/`__APPLE__` 等宏，也允许 `ENABLE_GLRENDER`/`ENABLE_DASH_DEMUXER`/`ENABLE_VIDEO_FILTER` 这类构建开关；**L1 核心**（时间轴、seek、同步/时钟、队列与调度、DASH/HLS 解析与分片逻辑）与 **L2 控制层禁止按平台分支**，只能经接口/HAL 调用平台能力。平台差异的三个落点：① 平台后端类实现同一接口；② 必须特殊处理时在该后端内部消化，或在接口上加"能力查询 + 可选实现"，**不得**写成核心里的 `if (平台)`；③ 确有无法抽象的极小范围时，集中在**一个明确的适配文件**里用宏，并注明"为什么抽象不了"。平台层同样不得改时间轴语义 | 分层前提（§1）；核心零分支是"Qt/Android/OHOS/iOS 同一份核心"的基础 |

---

## 5. DASH / HLS 兼容矩阵

| 形态 | 必须支持 | 如何支持 | 当前风险 |
|---|---|---|---|
| `SegmentTemplate@duration` | 是 | 起点 = `startNumber` + `index*duration`（含 `presentationTimeOffset`） | 分片起点精度限制 → 靠 S2/S3 解码丢弃补足 |
| `SegmentTimeline` | 是 | 逐条 `S@t/@d/@r` 展开成**显式**分片表 | `@r` 展开上限需设界（内存）；**谎报 `@d`** 时只能以实际解码为准 |
| `SegmentList` | 是 | 逐条 URL + `@duration`；缺失则用下一条起点差 | URL 模板拼接与重定向 |
| `SegmentBase`（单文件 + `indexRange`） | 是 | `sidx` → 子分片表（天然满足 S3 的"有索引"快路） | `sidx` 缺失时退化为全文件顺序解码 |
| `presentationTimeOffset` | 是 | 一律只在**解复用层**做轴转换，HAL/端侧不得重复加 | 现状多处轴换算 → 双加会导致 A/V 错位 |
| 多 Period | 是 | Period 边界当**流重启点**（新 demux 会话 + 时钟重锚） | 现状未按 Period 分量队列 |
| `startNumber/endNumber` | 是 | 与 segment 表一致性校验（缺号 = 空洞，不是 EOS） | 缺号被当成 EOS ⇒ 提前结束 |
| 空/缺 `codecs` | 是 | 不依赖 `codecs` 做选择；靠解码器探测（`extradata` 为空也要能开） | 现状可能直接拒绝该 rendition |
| **谎报 `segmentAlignment`** | 是 | **任何**对齐声明都只当"加速提示"，音视频各自按自己的起点/时长工作 | 依赖声明 = 切档/seek 后音画错位 |
| 音视频分片网格不一致（20s vs 10s，实测） | 是 | 每流独立游标 + 每流独立队列；A/V 只在**上屏/出声**处对齐 | 现状用视频落点去定位其余流 ⇒ 音频被拉早/拉晚 |
| 20s 大 GOP | 是 | hr-seek 解码丢弃（S2）+ 队列上限（R2）保证解码追赶有界 | 现状"落到 20s 前"被用户视为 seek 不准 |
| HLS（m3u8 + TS/fMP4） | 是 | 与 DASH 同一抽象：分片表 + 每流游标；EXT-X-BYTERANGE 视作 SegmentBase | 与 DASH 分叉出两套 seek 逻辑 |

---

## 6. 平台最小接口集（L0 必须实现）

| 能力 | Android（轻量优先） | Qt（macOS/Linux/Windows） | HarmonyOS | iOS |
|---|---|---|---|---|
| 视频解码 | MediaCodec（硬解优先，`Surface` 直出）；**数据面目标 = NDK `AMediaCodec` + `setAsyncNotifyCallback` 异步回调**（JNI 只留创建/能力/OEM/Surface，见 `ANDROID-NDK-ASYNC-DECODER.md`） | 平台硬解（VideoToolbox / D3D11VA / VAAPI）+ FFmpeg 软解兜底 | OH AVC/HEVC 硬解 + 软解 | VideoToolbox |
| 视频输出 | `Surface`/`ANativeWindow`（零拷贝优先） | Qt RHI 纹理（Metal/D3D11/GL）+ 平台零拷贝路径 | OH NativeWindow | `CVPixelBuffer` + Metal/GL |
| 音频输出 | `AudioTrack`（含低延迟/属性配置） | CoreAudio / WASAPI / ALSA·PulseAudio | OH AudioRenderer | AudioUnit |
| 时钟 | `steady_clock` 单调钟 | 同 | 同 | `mach_absolute_time`/`steady_clock` |
| 线程/锁 | C++ 标准库（**不得引入额外框架**） | 同 | 同 | 同 |
| 必须交给核心 | 时间轴、seek 执行、队列、时钟、A/V 对齐、切档状态机、mpd 解析 | 同 | 同 | 同 |
| 端侧只允许 | UI/事件、Surface 生命周期、音频会话配置、权限 | 同 | 同 | 同 |

> Android 轻量红线：内核不得依赖 Java 侧回调做节拍；JNI 调用只用于"创建/配置/写入"这类必须项。
>
> **现状差距与收敛方案（2026-09-27）**：当前 Android 解码**每帧数据面**仍跨 JNI 到 Java
> `MediaCodec`，并且**节拍确实依赖 Java 的 `onInputBufferAvailable` 回调** —— 违反上面这条红线。
> 收敛路径已定：数据面迁到 NDK `AMediaCodec` 的异步回调（`setAsyncNotifyCallback`，API 28+，
> 用 `__attribute__((weak_import))` 兼容 minSdk 24；API 24–27 走现有 Java 异步实现，**不引入同步轮询**），
> 只用 NDK r25c，不升级 r28。完整设计、API 级别核实表、线程模型与验收标记见
> **`ANDROID-NDK-ASYNC-DECODER.md`**。

**平台宏用在哪一层（对应 R8）**：`__ANDROID__` / `TARGET_OS_IPHONE` / `OHOS` / `_WIN32` / `__APPLE__` 以及 `ENABLE_*` 构建开关，**只允许**出现在上表的 **L0 实现文件**（视频解码、视频输出、音频输出、时钟、线程/原子包装、平台能力探测）与构建系统里。**L1 核心 / L2 控制层不得出现任何平台宏**：核心只调用 L0 的接口（虚函数 + 能力查询）。平台特殊行为要么在该平台后端内部消化，要么在接口上增加"能力查询 + 可选实现"，绝不写成核心里的 `if (平台)`。确有无法抽象的极小范围时，唯一例外是集中在**一个明确的适配文件**里用宏，并在注释里写清抽象不了的原因。

---

## 7. 迁移分期（每期只动一层，功能类冻结）

| 期 | 目标 | 改动层次 | 回退点 | 验收动作 |
|---|---|---|---|---|
| P0 | **本设计定稿** | 文档 | 删除本文档 | 用户确认架构 |
| P1 | demux 层：seek 请求化 + 每流游标/队列显式化 | L1-demux（+ L2 调用点） | 单文件 | seek 期间消息线程不再阻塞；位置单调 |
| P2 | 解码层：每流独立解码线程 + 有界 FIFO + 背压 | L1-decode | 单文件 | 音频供给不受视频解码停顿影响（对拍日志） |
| P3 | hr-seek：解码丢弃到目标帧 + 位置权威统一（S2/S4） | L1-decode + L1-clock | 单文件 | DASH 20s 分片下 seek 到目标 ±1 帧；进度条不弹回 |
| P4 | 切档：旁路预热 + 状态判据提交 + 失败回退（S6） | L1-decode + L1-render | 单文件 | 连续 10 次切档全成功且不中断音频 |
| P5 | 兼容矩阵补全（§5 逐项） | L1-demux | 单文件 | 各 mpd 形态各跑一遍 seek/切档/EOS |
| P6 | 平台 HAL 收敛（4 平台同一接口集） | L0 + L3 | 按平台回退 | 各平台构建 + 冒烟 |

每期独立回退；**禁止**为了让"回退"容易而保留两套并行实现（mpv/VLC 都是单一实现）。

### 7.1 P2 主体实施契约（demux 执行上下文，可直接照做）

**唯一无死锁的形状**：`demuxer_service` 自己成为 demux 线程的宿主 —— 线程内跑"读前推 + 命令槽"，
句柄只被该线程触碰（`readPacket`/`Seek`/`OpenStream`/`CloseStream`/`GetStreamMeta` 等**全部**迁进去）。

**为什么不能只加"请求槽 + 阻塞式取包"**：若生产侧（demux 线程）把包发布到单槽后**阻塞等待**消费者取走，
而消费者此刻正等待一条控制命令（`Seek`/`OpenStream`）完成，则双方互等 ⇒ 死锁。
⇒ 生产侧**任何情况下不得阻塞等待消费者**：必须是有界 **readahead 队列 + 队列满即停止读**，
控制命令用非阻塞投递（投完由消费者按原子状态判完成，而不是让生产者等）。

**数据面接口（状态式）**：`TryFetch(kind, out)` → `{PACKET, NO_DATA, EOF, ERROR, CANCELED}`；
等待一律用条件变量，但**锁只覆盖队列与槽的交换，绝不覆盖 demuxer 调用本身**（这是"热路径零锁"的准确含义：
不是没有锁，而是没有"跨 demuxer 调用的锁"）。

**唤醒完整性（缺一即饿死）**：出队、flush（seek/切档）、stop、cancel 四条路径都必须 `notify_all`。

**生命周期**：线程在 `start()` 创建；`stop()`/`preStop()` 投 STOP → `join()`；**句柄销毁必须在线程 join 之后**
（否则线程可能正在调用已析构的 demuxer）。

**迁移必须成组完成**：只迁 `readPacket` 而留下 `Seek`/`OpenStream`/`CloseStream`/`GetStreamMeta`/`getDuration`
在控制线程 ⇒ 线程归属契约不成立（两个线程碰同一句柄），比不迁更危险。

---

## 8. 风险与未证实项（不藏）

1. **未证实**：mpv `--hr-seek` 的用户级默认值与文档原文；`--video-sync` 默认值与实现（`DOCS/man/options.rst` 经镜像返回 `application/octet-stream`/不可达）。R4 因此标为"工程约定"，引入前必须补证。
2. **未证实**：mpv 解码线程的创建点（`filters/f_decoder_wrapper.c` 抓取内容中段被截断，未能引用 `mp_thread_create` 原文）；VLC `vlc_fifo_t` 的声明文件（`include/vlc_fifo.h` 404，`vlc_block.h` 仅剩别名宏）。
3. **未证实**：ijkplayer 的线程模型细节（仅作参考，未取证）；HarmonyOS/iOS 硬解与零拷贝的具体能力边界。
4. **风险**：P2/P3 会把"非原子成员跨线程可见"变成真实问题（EOS、落点、切档状态、时钟锚点），必须逐成员定义 owner 与同步方式；本设计不给实现，但**要求**每期交付时列出该期新增的 owner 表。
5. **风险**：hr-seek 在 4K/20s GOP 下需要连续解码最多一个 GOP；首帧延迟可能从"落点立即上屏"变为"到目标才上屏"。设计取舍：**允许首帧稍晚、不允许位置骗人**（S4）；若要兼顾，只能靠 S3 的索引快路。
6. **风险**：多 Period / 缺号分片的"流重启点"语义会牵动时钟重锚与位置上报；必须与 S4 一起定义。
7. **已知既有缺陷（本轮未处理）**：真机日志出现 `unknown stream 7, read packet ...` 以约 1200 行/秒的速率刷屏（`appQtPlayer.log:6103`、`:6163`），说明存在"播放器未持有的流在持续发包"；其与本次重设计的关系**未证实**，但 P1/P2 必须顺带消除（流生命周期归属）。
