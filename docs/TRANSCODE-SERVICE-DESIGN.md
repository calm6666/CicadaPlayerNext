# 转码服务详细设计（Go 服务 + ffmpeg Worker，可横向扩展）

> 目标读者：要实现这个服务的人（以及要评审"为什么不用 C++ / 为什么不用库方式"的人）。
>
> 本文的每一节都标明证据等级：
> **【实测】**= 本机跑出来的；**【读码】**= 读了现有实现的源码/文档得到的；**【推断】**= 由前两者推出的设计判断。
>
> 参考实现（都在 `D:\hilihili` 下）：
> * `转码脚本\`（Python，**只作本地测试与事实规格的权威来源**）
> * `server\hvc`（Go，**有真实转码能力的成熟服务**）
> * `server\vod_live_transcoding`（C++，**libav\* 进程内转码的真实服务**）
> * `server\hili_video_cloud`（C++，**不是服务，是一份设计规格 + 一层编译不过的接缝**）
> * `CicadaPlayerNext\server\drm-keyserver`（Go，**已经实现并通过 27+3 个单测的密钥/许可证服务**）
> * `CicadaPlayerNext\docs\{DRM-ENCRYPTION-DESIGN,DASH-DRM-SCHEMES,DRM-COMPLETENESS-AUDIT}.md`（DRM 侧的设计与证据）

---

## 0. 结论摘要（先看这一节）

1. **语言与形态：Go 写服务，ffmpeg 当子进程。**
   理由不是"Go 更时髦"，而是 **【实测】转码的全部算力都在编解码器里，进程边界不构成成本**，
   而"一个进程一次解码、N 路编码"这件事 `ffmpeg -filter_complex split=N` 已经做完了。
   本机实测：一条命令同时出 3 档 DASH 分片，退出码 0、MPD 与分片齐全（§3.3）。
   反过来，C++ 走 libav\* 的代价已经有人在付：`vod_live_transcoding\src\common\ffmpeg\FfmpegExecutor.cpp`
   **3,980 行手写 FFmpeg 内存管理、零测试**，其自审计报告列出 `av_frame_alloc 10 / av_frame_free 8`、
   `av_packet_alloc 51 / av_packet_free 43`、`sws_getContext 5 / sws_freeContext 2` 的**分配/释放不配对**
   【读码】。这条路上"多出来的性能"远小于"多出来的可维护性风险"。
2. **"哪一个写法更好"的答案：写服务看 `hvc`（Go），写转码内核看 `vod_live_transcoding`，但两者都别整体继承。**
   * `hvc` 的**并发语义是真的**：条件 `UPDATE` 抢单(乐观 CAS) + 租约 + `leaseGeneration` 防旧实例回写 +
     Outbox 三通道投递 + 分片上传失败队列。这四块可以直接抄语义。
   * `vod_live_transcoding` 的**转码细节更专业**：fMP4 muxer 的 `movflags`、网络源 stream-copy 本地缓存、
     5 平台编码器参数、雪碧图/`.bin` 索引。这些值得**读**，但不值得**移植它的内存管理**。
   * `hili_video_cloud` **不能当代码基础**：38 个 `.cpp`/1327 行里 **0 个文件能编译**，
     92 处 `#include "refactor_stage/..."` + 53 处 `#include "src/..."` 全部无法解析，
     CMake 里引用了未定义的 `vod_common` 目标（configure 直接失败）。它的价值是那份 **4350 行的
     `docs/refactor/DESIGN.md` + 881 行 DDL（31 张表）**——**当规格书读**。
3. **三者共同的空洞正好是本项目最需要的三件事：**
   **DRM/分片加密（两家都是 0 实现）**、**"完成 = 可播"的契约（hvc 明确不成立）**、
   **能跑真 ffmpeg 的测试（两家都是 0）**。本设计把这三条列为第一优先级。
4. **容量模型不能靠"会话数上限"猜**：【实测】本机 GTX 1650 Ti 上
   6 路与 12 路并发 1080p30 `h264_nvenc` **全部成功**，但 12 路的墙钟正好是 6 路的 2 倍
   ⇒ **聚合吞吐在 ~6 路时饱和（≈25× 实时）**，再多只是排队。
   所以调度要按 **GPU 吞吐预算**分箱，而不是按"最多几个会话"。

---

## 1. 三个现有实现的完成度分析

### 1.1 规模与语言（客观计数）

> **计数口径**（本节数字都是【实测计数】，2026-10-02 本机执行）：
> 递归统计源码文件，**排除** `node_modules`、`.claude`、`build`、`cmake-build*`、`.cxx`、`.gotmp`、`Debug`、`Release`；
> 行数用 PowerShell `Get-Content | Measure-Object -Line`。

| 项目 | 语言 | 真实代码量 | 是否有转码 | 测试 | 构建状态 |
|---|---|---|---|---|---|
| `hvc` | Go 1.25 | 正式树 **285** 个 `.go`（其中 `internal/` **267** 个 / **29,890** 行）；`glob` 报 1083 是因为 `.claude/worktrees/` 下有 6 份完整旧副本 | **有**（ffmpeg 子进程 + 5 平台硬编） | 48 个 `_test.go` / 156 个用例，但**没有一个是真跑 ffmpeg/DB/S3/HTTP 的** | 检入了 `main.exe`(41 MB)；无 CI、无 Makefile【读码】 |
| `vod_live_transcoding` | C++20 | `src/` 下 **24** 个 `.cpp` / **17,383** 行 + **37** 个 `.h` / 6,526 行 | **有**（libav\* 进程内） | **0** | 曾构建成功过（`build\Debug\vod_app.exe` 存在），**最后一次 MSBuild 失败**（`unsuccessfulbuild`）【读码】 |
| `hili_video_cloud` | C++20 | **39** 个 `.cpp` / **1,741** 行 + 47 个 `.h` / 825 行 | **没有**（关键词 `hls/m3u8/mpd/dash/watermark/aes/drm/encrypt/segment` 在代码里 **0 命中**） | **0** | **configure 失败**（CMakeLists 引用未定义的 `vod_common`）【读码】 |
| `转码脚本`（Python） | Python | `transcode_all.py` **4064** 行 + `convert-to-manifest.py` + `verify_segmentbase.py`(1881) + `aes128.py` + `drm_keyserver_tool.py` | **有**（且是本项目**唯一**产出过真实加密产物并逐字节验收过的实现） | 自带验收器 A1–A12 + 3 个离线自检脚本 | 可运行【读码+实测】 |

**几个关键文件的体量**（【实测计数】，用来判断"能不能维护"）：

| 文件 | 行数 | 字节 |
|---|---|---|
| `vod_live_transcoding/src/common/db/MySqlConnector.cpp` | 5,037 | 323,765 |
| `vod_live_transcoding/src/common/ffmpeg/FfmpegExecutor.cpp` | 3,980 | 186,462 |
| `vod_live_transcoding/src/app/roles/WorkerRunner.cpp` | 1,514 | 106,985 |
| `vod_live_transcoding/src/common/service/TranscodeJobService.cpp` | 1,424 | 75,111 |
| `vod_live_transcoding/src/app/roles/SchedulerRunner.cpp` | 647 | 38,833 |

### 1.2 逐能力对照

| 能力 | `hvc`(Go) | `vod_live_transcoding`(C++) | 本设计要做什么 |
|---|---|---|---|
| 任务三入口（HTTP/gRPC/MQ） | ✅ 生产可用（约 96 条路由） | ✅ 生产可用（公开 4 + 后台 50 端点） | 照抄语义，用 go-zero 生成 |
| 排队/调度 | ✅ 条件 UPDATE 抢单 + 租约 + 代次 | ⚠️ 有租约，但**未找到任何原子抢占语句**（无 `FOR UPDATE`/`SKIP LOCKED`/`SETNX`） | 用 `hvc` 的方案，并把抢占写进一条 `UPDATE ... WHERE status=queued` + 校验 `RowsAffected` |
| 故障接管 | ✅ `ListExpiredRunning` + `ForceTakeover` | ⚠️ 心跳摘除 + 自动恢复有；接管细节未查完 | 照抄 + 明确"接手后从 staging 断点续跑" |
| 三通道回调 | ✅ **Outbox 状态机**（CAS 抢占 + 重试 + 死信表） | ✅ Outbox + priority + legacy 回退 | **抄 `hvc` 的 outbox 语义**（更完整） |
| 分片上传 | ✅ 上传队列 + 重试 + 失败表 | ✅ S3 multipart | 抄，但**加流式上传**（见 §10.3） |
| 清单 | ⚠️ **动态从 DB 拼**，磁盘 MPD 被删（CDN 无法缓存） | ⚠️ 有 m3u8/MPD 写出，但 **DASH 只入库不产出**（`support_dash` 是静默失效开关） | **产出静态清单文件**（HLS+DASH+JSON 对象清单），CDN 可直出 |
| 阶梯 | ⚠️ 固定 1080/720/480/360，无自适配 | ⚠️ 有 profile 表，但 preset/tune 硬编码 | 按源分辨率裁剪 + profile 可配（值承载） |
| 水印 | ⚠️ **只有 CPU overlay**，硬件 overlay 只探测不使用 | ⚠️ CPU 逐像素 + GPU filter 两条路，未验证 | 先做**已验证的 CPU overlay**，`overlay_cuda` 走能力探测后启用 |
| 缩略图/雪碧图 | ⚠️ 有代码但**主链路从未调用**（死代码） | ✅ 雪碧图 + `.bin` 索引 + SHA-256 | 明确接线（可选产物） |
| 直播转码 | ❌ 未实现（只有频道/鉴权控制面） | ⚠️ 有 `LiveRunner` 转码，但**产物写在本地临时目录，退出时被删掉**，不上传、无滑动窗口、无多码率 | 本期只做 VOD；直播单独立项（否则会重演"转完就删"） |
| **加密 / DRM** | ❌ **全树 0 命中**（连 `crypto/aes` 都没引） | ❌ **全树 0 命中** | **本设计的重点**（§9），复用 `drm-keyserver` |
| 断点续跑 | ❌ 失败即 `ResetToQueued` 从零重跑 | ⚠️ 有 `retry_mode`，但自认"不是 ffmpeg 编码断点续转" | **按步骤幂等 + staging 续跑**（§10） |
| 安全基线 | ⚠️ 有内部 token，未见 RBAC 细节 | ⚠️ `api_key: ""` 默认**直接放行**；gRPC `InsecureServerCredentials`；SQL 100+ 处字符串拼接；配置里明文口令 | 见 §14 |
| 测试 | ⚠️ 156 个用例，**E2E 覆盖 0** | ❌ 0 | **以 Python 验收器为门禁**（§15.1） |

### 1.3 各自的"致命一条"

* **`hvc`：`completed` 不等于"可播"。** 代码顺序是 *扫本地分片 → 逐条落库 → `MarkCompleted`*，
  而**分片上传是之后由 DB 待传队列异步推进的**；回调却在完成时就发出去了，
  清单又是按请求从 DB 分片实时拼的 ⇒ **回调到达后立刻拉清单可能拿到不完整清单**【读码】。
  这是下游契约的模糊点，本设计必须堵掉（§13.1）。
* **`vod_live_transcoding`：手写 libav\* 的资源不配对 + 0 测试。** 3,980 行的单文件里同时管
  解码/滤镜/编码/封装/雪碧图，且分配/释放计数不配对【读码】。这类缺陷在压力下才暴露，
  而它没有测试。
* **`hili_video_cloud`：它不是代码。** 连 configure 都过不去，唯一入口 `main.cpp` 不属于任何构建目标，
  `RpcCreateJobRuntimeFacade` / `MessageQueueJobRuntimeFacade` 是 **start 后立刻 stop** 的空壳【读码】。

### 1.4 "哪一种写法更好"——分场景回答

| 问题 | 答案 | 依据 |
|---|---|---|
| **服务层（接单/调度/租约/回调/配置/权限）用哪种写法好？** | **Go（`hvc` 的写法）** | 依赖面 1 个 `go.mod`；并发原语是 SQL 条件更新 + Redis，可验证；无生成代码洪水（`hvc` 生成代码只占 ~2%）；同样的功能在 C++ 侧要拖进 oatpp + gRPC + rabbitmq-c + libmariadb + redis++ + AWS SDK + yaml-cpp + spdlog，且 `hili_video_cloud` 的实测结果就是"配不齐、编不过" |
| **转码内核用哪种写法好？** | **本期用 ffmpeg 子进程（`hvc` 的写法）；只有拿到"必须进程内"的硬需求才上 libav\*** | 见 §2 的三条反驳 |
| **谁的工程完成度高？** | **`hvc` 明显更接近可运营**（并发语义、outbox、错误通道、鉴权中间件都成链）；`vod_live_transcoding` **功能面更宽**（雪碧图、GPU 滤镜、网络源缓存、直播）但工程化差一档；`hili_video_cloud` 不计 | §1.1/§1.2/§1.3 |
| **该复用谁？** | 语义抄 `hvc`，细节读 `vod_live_transcoding`，规格读 `hili_video_cloud/docs/refactor/DESIGN.md` 与它的 31 张表 DDL，**事实规格以 Python 为准** | §16 |

---

## 2. 为什么是"Go + ffmpeg 子进程"（对 `DESIGN.md §7.2.1` 的反驳）

`hili_video_cloud/docs/refactor/DESIGN.md:662-686` 主张"优先用 FFmpeg 库方式而不是命令行进程"，
理由是：额外的上下文切换、内存复制、更差的缓冲区复用、更高的进程管理开销、**更差的实时状态观测粒度**。

**前四条：量级判断错了。** 转码的时间花在**编解码器内部**（硬件编码器或 x264/x265 的像素运算），
不在进程边界上。一次 `read/write` 的拷贝相对一次 1080p 帧的编码是万分之一量级【推断，基于编码耗时的常识量级】。
真正省下来的是"同进程缓冲区复用"，而 ffmpeg 自己的 `filter_complex` 已经把
**一次解码 → N 路编码** 的帧复用做完了——这正是 Python 参考实现用的方式【读码】。

**第五条（状态观测粒度）：被实测反驳。**
本机 **【实测】** `ffmpeg 8.1 (gyan build)`：

```
ffmpeg -nostats -loglevel error -f lavfi -i testsrc2=...:duration=2 \
       -c:v libx264 -preset ultrafast -f null - -progress pipe:1
→  frame=60
    fps=0.00
    stream_0_0_q=-1.0
    bitrate=N/A
    total_size=N/A
    out_time_us=2000000
    out_time_ms=2000000
    out_time=00:00:02.000000
    dup_frames=0
    drop_frames=0
    speed=46.9x
    progress=end
```

这是**机器可读、按固定间隔刷新的结构化进度流**（帧数/时间/速度/丢帧），
粒度到"帧"而不是"百分比"。再加上分片文件按 `xxx.m4s.tmp → xxx.m4s` 原子落盘的时间戳，
就能得到**分片级**进度。也就是说：子进程方案的观测粒度**不比库方式差**。

**库方式真正的独到价值只有两条**，而且都不是本期需求：
1. 在封装器内部**拦截每个分片字节**（例如"分片不落盘、直接加密后推对象存储"）；
2. 在同一个进程里让多个编码器共享同一个 `AVFrame`，而**不经过 ffmpeg 的 filter graph**
   （只有在你要自己实现 ffmpeg 不支持的分流策略时才有意义）。

**因此本设计的选择**：
* 引擎接口做窄：`Transcoder{ Probe(ctx, src) ; Plan(req) → Plan ; Run(ctx, plan, sink) }`，
  Go 侧只实现 `ffmpeg-cli` 引擎；
* 资源调度、并发、重试、续跑**全部在服务层**，与引擎无关；
* 如果将来真需要"边转边加密封装"，**再加一个 `libav` 引擎实现同一个接口**，
  而不是把服务整体搬到 C++。这条路径保留了"以后能换"，今天的成本是 0。

---

## 3. 事实规格：Python 参考实现到底做了什么

> 这一节是"把 Python 当规格书"的结果。Go 服务必须逐条对齐，否则内核播放行为会变。

### 3.1 流水线（8 阶段，顺序有语义）

```
0. AES 原语自检（FIPS-197 C.1 + NIST SP 800-38A F.2.1）—— 失败就一个产物都不产
1. 解析并校验所有加密/DRM 参数（缺值=错误；互斥关系在此判死）—— 早于任何落盘
2. ffprobe 源 → 按源分辨率裁剪阶梯（只出 ≤ 源分辨率的档）→ 初始化 Representation
3. 主转码：一条 ffmpeg 命令出全部 video 档 + 1 路 audio，同时写 DASH(MPD) 与 HLS(master+media)
4. SegmentBase 再封装：-c copy 出"单文件 + 全局 sidx + 字节范围"版本（明文）
5. 收尾重命名（雪花 ID + 档位后缀）+ 补 codecs
6. CENC 那一趟（可选）：-i output.mpd -c copy -encryption_scheme cenc-aes-ctr → 另一个目录
7. 关键帧抽查（ffprobe 数段内 IDR 间隔）—— 必须在加密前
8. HLS AES-128 整片加密（密文 + #EXT-X-KEY + SESSION-KEY + MPD ContentProtection）
9. JSON 对象清单生成（6 份）—— 必须在加密后（要读到 #EXT-X-KEY 的 IV）
```

**关键顺序约束（每一条都有理由，摘自源码注释）【读码】**：
* 5 必须在 6 之前：CENC 的输入是**明文** `output.mpd`，AES-128 会先把分片换成密文。
* 8 必须在 9 之前：JSON 的 `encryption` 字段从带 `#EXT-X-KEY` 的 m3u8 里读。
* 7 必须在 8 之前：密文喂 ffprobe 只会得到垃圾。

### 3.2 主转码命令（**这是本服务必须复现的模板**）【读码】

```
ffmpeg -y -hwaccel cuda  [无水印时再加: -hwaccel_output_format cuda]
       -i <源>            [有水印时再加: -i <水印图>]
       -filter_complex "…"
       # 每档视频：
       -map [outN] -c:v:N <h264_nvenc|hevc_nvenc>
       -tag:v:N <avc1|hvc1>            # HEVC 必须 hvc1（Apple 只认它，且 dashenc 按它写 codecs=）
       -b:v:N <kbps> -maxrate:v:N <kbps> -bufsize:v:N <kbps>
       -forced-idr:v:N 1               # 关键帧必须是 IDR（不是 open-GOP 的 I 帧）
       -no-scenecut:v:N 1              # 关掉 NVENC 的场景切换插帧，否则 IDR 位置随机
       -strict_gop:v:N 1 -sc_threshold:v:N 0
       -g:v:N <keyint> -keyint_min:v:N <keyint>     # keyint = IDR_INTERVAL × 源帧率（不是段长！）
       # 音频：
       -map 0:a:0 -c:a aac -b:a 128k
       # 输出：
       -f dash -init_seg_name "$RepresentationID$-0.m4s"
              -media_seg_name "$RepresentationID$-$Number$.m4s"
              -hls_playlist 1 -hls_master_name master.m3u8
              -use_template 1 -use_timeline 1          # ★ timeline 必须为 1
              -seg_duration 6
              output.mpd
```

**为什么 `-use_timeline 1` 不能改回 0**【读码，源码注释给了实测数据】：
`use_timeline=0` 时 MPD 只能写一个统一的 `@duration`，ffmpeg 写的是 `-seg_duration` 的**名义值**，
而真实切段点由关键帧决定。两者不等时 MPD 就在撒谎（实测 23.976fps + GOP 250 帧时
真实每段 10.427083s、声明 10.0s，差 4.27%），播放器按声明网格建时间轴会出现**声音越来越慢于画面**。

**为什么 GOP = IDR 间隔而不是段长**【读码，真机日志证据】：
GOP 取段长时 IDR 只落在段边界、段内没有 IDR，一次 seek 要从片首白解 6.36 秒（146 帧）、耗时 2159ms。
取 `IDR_INTERVAL × fps` 后段内还有 IDR，seek 最坏前推 ≈ IDR_INTERVAL 秒。
硬约束：**段长必须是 IDR 间隔的整数倍**（6 = 3 × 2），否则段边界落不到 IDR 上、`startWithSAP` 不成立。

### 3.3 【实测】本机把这条命令跑通了，并且发现一个必须写进设计里的坑

**实测 A（一条命令出多档 + 同时出 DASH/HLS）**：`ffmpeg 8.1` + `GTX 1650 Ti` 上，
`-filter_complex split` + N 路编码 + `-f dash -use_timeline 1 -hls_playlist 1` **退出码 0**，
产出 `out.mpd` + `init-0/1.m4s` + `seg-0-1/seg-1-1.m4s` + `media_0/1.m3u8` + `master.m3u8`。

**实测 B（坑：dash muxer 的分片路径是相对"子进程 CWD"的）**：
把 `-f dash <绝对路径>\out.mpd` 与 `-media_seg_name "seg-$RepresentationID$-$Number$.m4s"` 一起给时，
**MPD 落在指定目录，分片却落在 ffmpeg 子进程的 CWD**：

```
[dash] Opening 'C:\...\tprobe4\dash\out.mpd.tmp' for writing
[dash] Opening 'seg-0-1.m4s.tmp' for writing      ← 相对名
→ 实际文件出现在 CWD（D:\hilihili\seg-0-1.m4s），不在 out.mpd 所在目录
```

**设计要求（硬）**：Worker 起 ffmpeg 时 **必须把子进程的 `cwd` 设成该任务的 staging 目录**，
或者把 `-init_seg_name/-media_seg_name` 写成绝对路径。
否则会得到"MPD 声明了分片、分片在别处"这种**看起来成功、实际不可播**的产物。

### 3.4 验收判据 A1–A12（**服务要把它当门禁**）【读码】

| 判据 | 内容 | 判"什么坏了" |
|---|---|---|
| A1 | 解密后前 16 字节是合法 MP4 box 头（长度 + 可打印 type） | 密钥/IV 配错，或其实没加密 |
| A2 | 密文长度是 16 的整数倍 | CBC 没补齐 |
| A3 | 清单分片行 == 目录里的分片文件 | 清单与产物不符 |
| A4 | `#EXT-X-KEY` 不许带非 identity 的 `KEYFORMAT` | 非空会让内核跳过这条加密=静默降级 |
| A5 | 每片 IV 唯一；前 12 字节同一基、尾 4 字节按序号递增 | CBC 复用 IV（致命）；IV 推导口径错 |
| A6 | **逐片**：KEY 条数 == 分片数；第 k 条覆盖第 k 片；第 k 条 IV == `_iv_for_index(基, 序号+k)`；解密后与明文对照**逐字节一致** | 最强端到端证据 |
| A7 | PKCS7 补齐量落在 1..16 | 补齐实现错 |
| A8 | 解密后 mp4 box 完整性（init 段有 ftyp/moov，媒体段 moof/mdat 配对） | 产物结构坏 |
| A9 | 分段式 JSON 的 `encryption.keyUrl/iv` 与 `#EXT-X-KEY` 逐字一致 | 清单与产物不同源 |
| A10 | SegmentBase 单文件清单**不得**含 encryption；其播放列表**不得**有 `#EXT-X-KEY` | 单文件模式被顺手加密 |
| A11 | 临时名（`*.enc.tmp`）不许残留；明文对照目录与分片一一对应 | 原子写没做完 / 交付残留 |
| A12 | master 的 `#EXT-X-SESSION-KEY` 与媒体播放列表一致 | 主/子清单不一致 |
| CENC | init 段的样本入口是 `encv`/`enca`；`tenc.default_KID == 旁挂 JSON == 期望 KID`；媒体段有 `senc`+`saiz`+`saio`；MPD 的 `ContentProtection` 与 tenc 同源；**文件仍以 `ftyp` 开头**（证明不是整片密文） | CENC 真伪 |

> 注：`verify_segmentbase.py` 明确声明**不做**的事是"用密钥把 CENC 样本 AES-CTR 解回码流再验"，
> 理由写在其注释里（要正确做需解析 per-sample IV 并重建 NAL/ADTS 判据，成本远超收益）。
> **本服务沿用这个边界**，不假装验过。

### 3.5 产物清单（一次任务的全部输出）

| 类别 | 文件 | 备注 |
|---|---|---|
| 分段式 DASH | `output.mpd` + `<ID>-<档位>-<序号>.m4s`（含 init 段 `-0.m4s`） | 明文或 AES-128 密文 |
| 分段式 HLS | `master.m3u8` + `media_N.m3u8`（每片一条 `#EXT-X-KEY`） | 同上 |
| SegmentBase 单文件 | `output-segmentbase.mpd` + 每档单文件 `.m4s` + 每档 `.m3u8` + `master-segmentbase.m3u8` | **始终明文** |
| CENC（可选，另一目录） | `output-cenc.mpd` + `cenc-*-init.mp4` + `cenc-*-<序号>.m4s` + `cenc_key_info.json`（旁挂，**不含密钥**） | 样本级加密 |
| 对象清单 JSON ×6 | `test-{dash,hls}-<版本>.json`、`-explicit.json`、`-segmentbase.json` | 内核与 web 的播放入口 |
| 明文对照（可选） | `_clear/<分片>` | 验收用；**上 CDN 必须排除** |

JSON 清单的字段集合以 `CicadaPlayerNext\framework\demuxer\manifest\MediaManifestParser.cpp` 为准
（`mediaSourceType` / `duration` / `minBufferTime` / `video[]` / `audio[]` / `subtitle[]` /
`segmentInfo{mode,initialization,indexRange,targetDuration,media,segments[],segmentTimeline[]}` /
`encryption{keyUrl,iv,keyFormat,keyFormatVersions}` / `licenseServer{url,contentId,keyType}` /
`contentProtection[]` / `liveConfig` / `contentSteering`）【读码】。**Go 服务必须产出与它逐字段可解的 JSON。**

---

## 4. 目标架构

```
                    ┌──────────────────────── 接入层（go-zero）────────────────────────┐
   HTTP  ────────►  │  api 服务：POST /v1/transcode/jobs  (也做查询/取消/重试/管理后台)  │
   RPC   ────────►  │  zrpc 服务：TranscodeJob.CreateJob / QueryJob / CancelJob         │
   MQ    ────────►  │  consumer：transcode.job.create.<env>                            │
                    └───────────────┬──────────────────────────────────────────────────┘
                                    │ 三入口 → 同一个 CreateJobUseCase（统一 DTO + request_id 幂等）
                    ┌───────────────▼────────────────┐
                    │ MySQL：唯一事实来源（job/steps/ │   Redis：热路径（租约心跳、进度、配置快照）
                    │  renditions/artifacts/outbox）  │
                    └───────────────┬────────────────┘
                                    │ 调度器（可多实例）：筛选 → 打分 → 条件 UPDATE 抢单
                    ┌───────────────▼──────────────────────────────────────────────────┐
                    │ Worker 节点（N 台，无状态）                                       │
                    │  step 1 probe → step 2 plan → step 3 encode(ffmpeg 子进程)        │
                    │  → step 4 package → step 5 encrypt → step 6 verify(A1–A12)        │
                    │  → step 7 upload → step 8 publish(原子) → step 9 callback        │
                    │  每步：staging 目录 + 幂等 + 断点续跑                              │
                    └───────┬──────────────────────────────┬───────────────────────────┘
                            │                              │
                  对象存储（S3/MinIO/OSS）          DRM 密钥服务（已有，Go）
                  产物 + 清单 + 旁挂 JSON          /admin/keys /key/{kid} /license /admin/sign
```

**三条不变量（贯穿全文）**：
1. **MySQL 是唯一事实来源；MQ 只是"有新活的"提示。** 消息丢了只会慢，不会错。
2. **任何对外可见的状态变化都发生在"原子发布"之后**。发布之前只有 staging 与 DB 内部状态。
3. **内容密钥只存在于密钥服务与加密那一刻的内存里**，任何清单/日志/DB/旁挂 JSON 都不得出现。

---

## 5. 领域模型与数据库

### 5.1 表（在 `hili_video_cloud` 的 31 张表基础上裁剪+修正）

| 表 | 作用 | 相对参考实现的修正 |
|---|---|---|
| `t_transcode_job` | 任务主表：`request_id`(幂等键) / `biz_key` / `source` / `profile_id` / `priority` / `status` / `lease_owner` / `lease_expire_at` / `lease_generation` / `step` / `params_hash` | 加 `step`（断点位置）与 `params_hash`（判断 staging 是否可复用） |
| `t_transcode_job_step` | **每步一行**：`job_id` / `step` / `status` / `attempt` / `input_hash` / `output_hash` / `started_at` / `finished_at` / `error_code` | `hvc` 没有这张表，所以无法续跑 |
| `t_transcode_rendition` | 每档：codec / 宽高 / 码率 / 最大码率 / bufsize / fps | 加 `encoder_preset/tune/rc`（`vod_live_transcoding` 这几项是硬编码） |
| `t_transcode_artifact` | 产物：类型（init/media/playlist/mpd/json/sprite/bin）/ 相对路径 / 大小 / sha256 / 对象键 / 上传状态 | 取代"把每个分片都塞进 MySQL"（见 5.3） |
| `t_transcode_segment` | **只在需要逐片查询时使用**（直播/边播边出） | VOD 默认**不写逐片行** |
| `t_event_outbox` | 回调事件：`event_type` / `payload` / `status` / `attempt` / `next_attempt_at` / `last_error` | 抄 `hvc` 的 CAS 状态机 |
| `t_delivery_failure_queue` | 死信：投递彻底失败的事件 | 抄 `hvc` |
| `t_worker_node` / `t_worker_capability` | 节点与能力快照：硬件类型 / 编解码器集合 / **每 GPU 会话上限** / **实测吞吐** / 心跳时间 | 加"实测吞吐"字段（见 §11.3） |
| `t_profile` / `t_profile_rendition` | 阶梯模板（可按业务覆写） | |
| `t_callback_config` | 回调目标：type(http/rpc/mq) / url-or-endpoint-or-routingkey / priority / enabled | 抄 |
| `t_runtime_config` | 热配置版本化 | 抄 `vod_live_transcoding` 的 runtime config + 发布快照 |
| `t_transcode_job_request_override` | 按任务覆写模板 | 保留 |

### 5.2 状态机（**这是修 `hvc` 那个洞的地方**）

```
CREATED ──► QUEUED ──► LEASED ──► RUNNING ──► VERIFYING ──► PUBLISHED ──► CALLBACK_SENT
               ▲           │           │           │              │
               │           │           │           └── 验收不过 ──► FAILED(VERIFY)
               │           └── 租约过期 ──────────┘（接管后从 t_transcode_job_step 的断点续跑）
               └── 显式重试（retry_mode=continue|restart）
FAILED(*)：终态或可重试态，可人工/自动重入队
```

**核心修正**：`PUBLISHED` 的定义是 **"所有分片与清单已上传到对象存储、且已通过 A1–A12 验收"**，
而不是"ffmpeg 退出码 0"。回调事件只在进入 `PUBLISHED` 后落 outbox。
`RUNNING → VERIFYING` 之间的产物只存在于 staging 与对象存储的 `_staging/` 前缀下，**不可被 CDN 读到**。

### 5.3 为什么 VOD 不把每个分片写进 MySQL

`hili_video_cloud` 的设计要求"分片表必须具备关键字段"，`hvc` 的实现是"扫描目录 → 逐条 `INSERT`"。
一个 2 小时 4 档 6 秒分片的片子 ≈ **4,800 个分片 × 4 档 = 19,200 行/任务**。
十万级任务就是十亿级行，只为了"能按请求动态拼 m3u8"（`hvc` 的做法）→ 得不偿失【推断，基于行数量级】。

**本设计**：
* VOD 的权威分片表是**对象存储里的一个 JSON sidecar**（`<job>/index.json`，含每档的分片名/时长/字节范围/sha256），
  由 Worker 在打包阶段一次写出；
* 同时把**同一份 JSON 内联进对象清单**（内核直接读的就是它）；
* MySQL 只存 `t_transcode_artifact` 级别的行（每档几行），以及**可选的**逐片行（`t_transcode_segment`），
  仅在需要"按分片查询/直播"时写。

---

## 6. 接口契约

### 6.1 统一请求对象（三入口同一份语义）

```json
{
  "request_id": "biz-20261002-0001",          // 幂等键（唯一索引）
  "biz_key": "movie/12345",
  "input": {
    "type": "local|http|s3",                   // 显式声明，不靠字符串猜
    "url": "s3://vod-in/movie/12345/source.mp4",
    "headers": { "Authorization": "..." },     // http 源用；签名 URL 也走这里
    "expected_sha256": "…"                     // 可选：源完整性校验
  },
  "output": {
    "formats": ["hls", "dash", "json"],        // 值承载，不是开关
    "segment_duration_sec": 6,
    "idr_interval_sec": 2,
    "base_url": "https://cdn.example.com/vod/12345/",
    "backup_urls": ["https://cdn2.example.com/vod/12345/"],
    "segmentbase_single_file": true,           // 是否额外产出单文件+sidx 版本
    "object_store": { "config_id": 3, "prefix": "vod/12345/" }
  },
  "video": {                                   // 省略则按源分辨率自动裁剪
    "renditions": [
      { "codec": "h264", "width": 1920, "height": 1080, "bitrate_kbps": 5000,
        "maxrate_kbps": 5500, "bufsize_kbps": 10000, "fps": null,
        "encoder": "h264_nvenc", "preset": "p4", "tune": "hq" },
      { "codec": "h265", "width": 1920, "height": 1080, "bitrate_kbps": 3500, "…": "…" }
    ]
  },
  "audio": { "codec": "aac", "bitrate_kbps": 128, "tracks": [0] },
  "watermark": {
    "enabled": true,
    "image_url": "s3://vod-in/assets/wm.png",
    "anchor": "bottom-right",                  // 9 宫格
    "x_ratio": 0.02, "y_ratio": 0.02,          // 相对边距
    "height_ratio_1080p": 0.0583,              // 相对 1080p 的高度比例
    "opacity": 0.8,
    "safe_margin_px": 16
  },
  "encryption": {
    "mode": "none|hls_aes128|cenc",
    "key_service": { "base_url": "https://drm.internal", "key_id": null },
    "cenc_scheme": "cenc",                     // cenc|cens|cbc1|cbcs
    "kid": null                                // 省略则由密钥服务铸
  },
  "callback": { "http_url": "…", "rpc_endpoint": "…", "mq_routing_key": "…" },
  "priority": 5,
  "idempotency_scope": "request_id"
}
```

**约定**（照抄参考实现已经验证过的口径）：
* `enabled` 之类布尔只出现在**请求对象内部**表示"这一条子配置要不要生效"，
  **服务自身的行为切换一律用带值的参数表达**（`formats[]`/`mode`/`input.type`），不引入
  `--enable-drm` 这种"给了开关却没有值"的形态——Python 参考实现明确拒绝这类参数【读码】。
* 参数非法 ⇒ **400，且什么都不做**（不"降级为不加密"、不"降级为不加水印"）。
  Python 的对应红线是"缺值即错误，绝不静默产出未加密产物"。

### 6.2 HTTP（go-zero `.api`）

```
POST   /v1/transcode/jobs                 创建（幂等：同 request_id 返回既有任务）
GET    /v1/transcode/jobs/:id             详情（状态/步骤/进度/产物清单/失败原因）
GET    /v1/transcode/jobs                 列表（filter: biz_key/status/时间段）
POST   /v1/transcode/jobs/:id/cancel      取消（幂等）
POST   /v1/transcode/jobs/:id/retry       重试（retry_mode=continue|restart）
GET    /v1/transcode/jobs/:id/artifacts   产物（对象键 + 预签名 URL）
GET    /v1/transcode/jobs/:id/progress    进度快照（也用于 WS）
POST   /v1/transcode/probe                只探测源（不转码）→ 返回真实阶梯建议
GET    /v1/transcode/nodes                节点与能力（运维）
POST   /v1/transcode/nodes/:id/quarantine 隔离/恢复
POST   /v1/transcode/config               热配置更新 + 发布
```

### 6.3 RPC（proto）

```proto
service TranscodeJobService {
  rpc CreateJob (CreateJobRequest)        returns (CreateJobResponse);
  rpc GetJob    (GetJobRequest)           returns (JobDetail);
  rpc CancelJob (CancelJobRequest)        returns (CancelJobResponse);
  rpc Probe     (ProbeRequest)            returns (PlanSuggestion);
  rpc ReportProgress (ProgressReport)     returns (Ack);      // 节点 → 控制面
  rpc RenewLease     (LeaseRequest)       returns (LeaseResponse);
}
// 回调（控制面 → 业务方）
service TranscodeCallback {
  rpc OnJobFinished (JobFinishedEvent) returns (Ack);
}
message JobFinishedEvent {
  string request_id = 1;
  string biz_key = 2;
  string status = 3;                    // published | failed
  repeated Artifact artifacts = 4;      // 类型/对象键/大小/sha256
  string manifest_hls_url = 5;
  string manifest_dash_url = 6;
  string manifest_json_url = 7;
  string error_code = 8;
  string error_message = 9;
  int64  finished_at = 10;
}
```

### 6.4 MQ

* 入站：`exchange=transcode.direct`，`routing_key=transcode.job.create.<env>`，
  body 与 §6.1 同构（三入口共用 DTO 是 `vod_live_transcoding` 已经验证过的做法【读码】）。
* 出站（回调）：`routing_key=transcode.job.finished.<env>.` + `biz_key` 前缀，便于按业务分流。
* 消费语义：**手动 ack**；处理成功才 ack。消费失败不依赖"MQ 重投"来保证正确性——
  任务已经在 MySQL 里（`CREATED/QUEUED`），重投只是加速。

### 6.5 三通道回调 + Outbox 状态机（抄 `hvc` 的语义）

```
事件落库 (t_event_outbox, status=PENDING)
  → 投递器取一批（CAS: UPDATE … SET status=SENDING WHERE id=? AND status=PENDING）
    → 按 t_callback_config.priority 依次尝试 HTTP / RPC / MQ
      → 任一成功 ⇒ status=DELIVERED
      → 全失败   ⇒ status=RETRYABLE + next_attempt_at（退避在 DB 里，不用内存计时器）
      → 超过上限 ⇒ status=FINAL_FAILED + 写 t_delivery_failure_queue（人工/补偿可重放）
  → 清理：DELIVERED 超期归档；SENDING 超期（投递器崩溃）由 ResetStaleSending 收回
```

**为什么退避放在 DB 的 `next_attempt_at` 而不是内存定时器**：投递器是多实例的，
内存计时器在实例重启/扩容时会丢；DB 字段是唯一的真相。
（这一条同时满足"服务里不出现看门狗/内存定时兜底"的工程偏好。）

---

## 7. 输入源与输出格式

### 7.1 输入（**显式类型，不猜**）

| 类型 | 处理方式 | 注意 |
|---|---|---|
| `local` | 直接给 ffmpeg 路径 | 只允许白名单根目录（防目录穿越） |
| `http` | 直接给 ffmpeg URL + `-headers` | 签名 URL 会过期 ⇒ **在 plan 阶段就把源拉成本地 staging 副本**，避免长任务中途失效 |
| `s3` | 用对象存储 SDK 下到 staging（或流式管道） | `vod_live_transcoding` 的做法是"网络源先用 stream-copy 存成本地 mkv，多档只下一次"【读码】，值得照做 |
| 直播拉流 | 本期不做 | 见 §16.3 |

**网络源的强约束**：长任务（转码）**不允许**直接吃一个会在中途过期的 URL。
`vod_live_transcoding` 的做法（先本地缓存）是对的，但它是用 ffmpeg 再解一次；
更省的做法是**对象存储 `GetObject` 直接落 staging**（不重新封装），
只有在源是 `http` 且不能直接下载时才退回"ffmpeg stream-copy 缓存"。

### 7.2 输出格式（值承载的多选）

| 值 | 产出 |
|---|---|
| `hls` | `master.m3u8` + `media_N.m3u8` + fMP4 分片 |
| `dash` | `output.mpd`（`SegmentTimeline` 真实时长）+ 同一批分片 |
| `json` | 对象清单 JSON（内核/播放器直读） |
| `segmentbase` | 单文件 + 全局 sidx + 字节范围 + 对应 m3u8/JSON（**始终明文**） |
| `cenc` | 另一目录的样本级加密产物（走独立一趟 ffmpeg） |
| `sprite`/`bin` | 缩略图雪碧图 + `.bin` 索引（可选） |

---

## 8. 水印设计

### 8.1 已验证的软件路径【读码，且是 Python 实际在用的】

```
# 水印高度按 1080p 基准等比缩放；宽度必须用 -2（自动取偶数），用 -1 可能算出奇数宽
#   → overlay 到 yuv420p 会直接报 "width not divisible by 2" 而整条命令失败
[1:v]scale=-2:<h>[wm];
[0:v][wm]overlay=x='0.02*main_w':y='0.02*main_h'[vmarked];
[vmarked]hwupload_cuda[vup];
[vup]split=N[v0][v1]…;
[v0]scale_cuda=<w0>:<h0>[out0]; …
```

**必须记住的坑**：**有水印时不要加 `-hwaccel_output_format cuda`**。
加了之后帧只在 GPU 显存里，CPU 的 `overlay` 拿不到，会失败；
参考实现的做法是"让 FFmpeg 自动下载到 CPU 内存 → CPU overlay 一次 → `hwupload_cuda` → GPU split/scale"，
CPU 占用极低【读码】。

### 8.2 锚点模型（值承载，来自 `hili_video_cloud` DESIGN.md §7.6，本设计采纳）

* `anchor` ∈ 9 宫格：`top-left|top-center|top-right|middle-left|center|middle-right|bottom-left|bottom-center|bottom-right`
* 相对边距 `x_ratio`/`y_ratio`（0..1，相对主画面宽/高）+ `safe_margin_px`（按输出分辨率换算的像素级安全边距）
* 高度比例 `height_ratio_1080p`，实际高度 = `ratio × 输出高度 / 1080`，**并向上取偶**
* `opacity` 用 `format=rgba,colorchannelmixer=aa=<opacity>` 实现

### 8.3 硬件路径（后置）

`hvc` 只探测 `overlay_cuda/scale_cuda/overlay_qsv` 是否存在却从不使用；
`vod_live_transcoding` 有 `overlay_cuda` 的滤镜图（`:675`）但无测试【读码】。
**本期先不做硬件叠加**：软件 overlay 已被 Python 验证；硬件路径等有了可对比的成片后再上，
并且**必须靠能力探测决定**（`ffmpeg -filters` 里有没有对应滤镜 + 该 GPU 是否可用），
不允许"探测到了就假设一定更快"。

---

## 9. 加密与 DRM（**两家参考实现都没有的那一块**）

### 9.1 两种加密，两种做法

| 模式 | 谁做 | 关键点 |
|---|---|---|
| `hls_aes128`（整片加密） | **我们自己的代码**（Go `crypto/aes` 或 C++ OpenSSL） | 逐分片 AES-128-CBC + PKCS7；IV = `基IV[0:12] + (基IV尾 + HLS序号)` 大端；**逐片写一条 `#EXT-X-KEY`** |
| `cenc`（样本级加密） | **ffmpeg 做**（`-encryption_scheme cenc-aes-ctr -encryption_key <32hex> -encryption_kid <32hex>`） | KID 必须**先定**（要同时写进 tenc / MPD / 旁挂 JSON 三处）；ffmpeg 的 scheme 名是 `cenc-aes-ctr` 不是 `cenc` |

**为什么不整片加密交给 ffmpeg**：ffmpeg 没有"把每个 `.m4s` 整文件当明文加密"的 muxer 选项；
而整片加密必须发生在**封装完成之后**（否则 box 结构会被破坏）。Python 参考实现就是自己做的，
并且已经逐字节验收过（密文解回明文与 `_clear/` 对照一致）【读码】。

**为什么逐片写 `#EXT-X-KEY`**：一条带 IV 的 `#EXT-X-KEY` 覆盖它之后的**所有**分片（RFC 8216 §4.3.2.4）。
只写一条就只能在"基 IV 恒为全 0、靠播放器按序号推导"和"所有片共用一把 IV"之间二选一，
而后者是 CBC 的致命错误。逐片写让清单与产物**同真同假**。落点规则也要照抄：
第 1 条排在 `MEDIA-SEQUENCE` 之后、`MAP` 之前；其余每条紧贴各自 `#EXTINF`/`#EXT-X-PART` 之前。

### 9.2 密钥管理与"密钥永不落盘"

* 密钥的唯一来源是 **`CicadaPlayerNext/server/drm-keyserver`**（Go，已实现且 27+3 单测通过）：
  `/admin/keys` 铸钥、`/key/{kid}` 裸 16 字节、`/license/{kid}?format=clearkey` W3C ClearKey、
  `/admin/sign` 出带有效期的签名 URL。
* Worker 只在"加密那一步"持有密钥明文，**用完立即清零**；任何日志只打**密钥指纹**（SHA-256 前 8 字节），
  绝不打密钥内容——这条是 Python 参考实现已经执行的纪律【读码】。
* 清单里写的是**取密钥的 URL**（可以是签名 URL），不是密钥。
* 任务参数里**不允许**出现密钥明文（`key_service.key_id` 是引用，不是密钥）。
  （`vod_live_transcoding` 的 `appsettings.yaml` 里 `minioadmin`/`123456` 明文口令是反例【读码】。）

### 9.3 与内核/播放器的契约（**必须与已实现的内核对齐**）

内核侧已经完成的（见 `docs/DASH-DRM-SCHEMES.md`）：
* DASH-IF 登记表全量 31 条 scheme 的识别与选优；
* DASH 的 DRM 方案选优：**平台 CDM 认的优先硬解，否则软解 CENC**，且 `Stream_meta.keyFormat`
  **只在平台 CDM 认这个 scheme 时才填**（否则会把所有软解解码器排除）；
* 按需取密钥：读到第一个带 `AV_PKT_DATA_ENCRYPTION_INFO` 的包时用包上的 KID 去取密钥；
* 接受三种许可证形状：裸 16 字节 / W3C ClearKey JSON / hex JSON。

**因此转码服务要产出的声明形式**（这是"能不能播"的接口）：
```xml
<!-- DASH：CENC -->
<ContentProtection schemeIdUri="urn:mpeg:dash:mp4protection:2011" value="cenc"
                   cenc:default_KID="<小写带连字符>"/>
<ContentProtection schemeIdUri="urn:uuid:e2719d58-a985-b3c9-781a-b030af78d30e">
  <cenc:pssh>…base64…</cenc:pssh>
</ContentProtection>
```
```json
// 对象清单
"encryption": { "keyUrl": "https://drm.internal/key/<kid>?exp=…&sig=…", "iv": "<首片 IV hex>" },
"licenseServer": { "url": "https://drm.internal/license/<kid>", "keyType": "clearkey" }
```
**禁止**写的：`#EXT-X-KEY` 里的非 identity `KEYFORMAT`（会让内核跳过这条加密 = 静默降级，A4 判据）。
**一期 HLS 的 `output.mpd` 声明**用登记表里真实的
`urn:uuid:3ea8778f-7742-4bf9-b18b-e834b2acbd47`（Clear Key AES-128 / HLS CBC），
不要用不在登记表里的示例 UUID（这是本轮修掉的一处真错，见 `docs/DRM-ENCRYPTION-DESIGN.md` §4.3.1 的订正块）。

### 9.4 验收门禁（**把 A1–A12 变成服务的自动门禁**）

`VERIFYING` 步骤直接跑移植后的验收器（Go 重写 Python 的判据）：
不通过 ⇒ 任务 `FAILED(VERIFY)`，**staging 整体丢弃、不发布、不发回调**。
这条门禁同时解决了两家参考实现的"0 真测试"问题：**每次真实任务都在跑验收**。

---

## 10. Worker 执行模型

### 10.1 步骤化 + 幂等 + 断点续跑

```
staging/<job_id>/
  src/            # 源副本（网络源先落地）
  plan.json       # 本次实际使用的 argv、阶梯、参数 hash
  out/            # ffmpeg 直接输出（cwd = 这里）
    init-*.m4s  seg-*-*.m4s  output.mpd  master.m3u8  media_*.m3u8
  clear/          # 可选：加密前的明文对照（验收用）
  cenc/           # 可选：CENC 那一趟
  index.json      # 分片索引（对象存储 sidecar 的来源）
  manifest/*.json # 6 份对象清单
  verify/report.json
```

* 每个 step 在 `t_transcode_job_step` 里一行，带 `input_hash`（源+参数）与 `output_hash`。
* 接管/重启后：`input_hash` 未变且上一步 `status=SUCCEEDED` ⇒ **跳过**（这就是断点续跑，
  不需要任何"重试次数"机制；`hvc` 缺的正是这一步）。
* 最后一步 `publish` 是**原子**的：先 `CopyObject` 到 `_staging/` 前缀下，全部成功后再
  批量移动到正式前缀（或对象存储的 `CopyObject` + 删除），最后写 `PUBLISHED`。
  CDN 只可能看到"全有"或"全无"。

### 10.2 进度

* ffmpeg 用 `-progress pipe:1 -nostats`，解析 `out_time_us/frame/fps/speed/drop_frames`（**【实测】**格式见 §2）。
* 叠加分片落盘时间（`xxx.m4s.tmp → xxx.m4s` 的 rename 事件）得到分片级进度。
* 进度写 Redis（TTL），不写 MySQL；任务终态时才落库，避免热路径打 DB。

### 10.3 边转边传（可选增强）

ffmpeg 的 dash muxer 会在分片完成时把 `.tmp` 改名。监听该目录的改名事件即可**流式上传**已完成的
分片（`vod_live_transcoding` 与 `hvc` 都是"转完再扫目录上传"，首帧可播时间因此等于整个任务时长）。
本期先做"转完再传"（简单、可验收），把流式上传列为增强，因为它的正确性依赖
"哪些文件已经完整"的判据（改名事件 / 大小稳定），需要单独验证。

---

## 11. 调度与横向扩展

### 11.1 抢单：条件 `UPDATE` + 租约 + 代次

```sql
UPDATE t_transcode_job
   SET status='LEASED', lease_owner=?, lease_expire_at=DATE_ADD(NOW(), INTERVAL ? SECOND),
       lease_generation=lease_generation+1, updated_at=NOW()
 WHERE id=? AND status='QUEUED';
-- RowsAffected==0 ⇒ 别人抢走了（或状态已变），本实例放弃
```
* 所有后续状态写入都带 `lease_generation` 条件（`hvc` 的做法），防止"旧实例恢复后又回写"。
* 租约 TTL 必须来自配置（`vod_live_transcoding` 把它硬编码成 60 秒、而配置里的
  `jobLeaseTtlSec` 没被那条 SQL 使用，是个隐患【读码】）。
* 租约续期由 Worker 在**每个步骤边界**主动做（不是定时器）：

| 步骤 | 续租 | 理由 |
|---|---|---|
| 开始前 | ✔ | 拿到活先占住 |
| 转码中 | ✔（由 `-progress` 行驱动，每 N 行续一次） | 长任务的唯一硬需求 |
| 上传中 | ✔ | 上传可能比转码更慢 |

### 11.2 节点筛选与打分

* **硬约束**：源编码能否解（硬解优先，软解兜底）、目标编码能否编、所需滤镜是否存在、
  对象存储可达、磁盘余量 ≥ 预估产物体积。
* **打分**：`空闲 GPU 吞吐预算` 权重最高，其次 `近期失败惩罚`，再次 `历史成功率`、`机房亲和`。
  `hili_video_cloud` DESIGN.md §8.3/8.4 的分数模型可用，但它把"GPU 会话数"当硬约束——
  见下一节为什么要改。

### 11.3 【实测】容量模型：按吞吐分箱，不按会话数

本机 **实测**（GTX 1650 Ti + ffmpeg 8.1，`h264_nvenc -preset p1`，源 1920x1080@30，每会话 60 秒内容）：

| 并发会话 | 成功率 | 墙钟 | 聚合吞吐 |
|---|---|---|---|
| 6 | 6/6 | 14.53 s | 6×60/14.53 = **24.8× 实时** |
| 12 | 12/12 | 29.68 s | 12×60/29.68 = **24.3× 实时** |

**结论**：
1. **驱动/GPU 没有给出很低的会话数上限**（12 路并存全部成功），所以"最多 3 路/5 路"这类
   历史经验不能写死；**必须探测**（`ffmpeg -encoders` 有 `h264_nvenc` + 实际试编一次）。
2. **真正的约束是聚合吞吐**：12 路的墙钟正好是 6 路的 2 倍 ⇒ **吞吐在 ~6 路时已饱和**，
   多出来的 6 路只是在排队。所以调度要按"这块 GPU 还能吃多少实时帧"来分箱。
3. 一个任务的 GPU 预算是 **`Σ(档位数 × 每档编码器负载)`**。参考实现的阶梯是
   **4 档 × 2 编码格式 = 8 路编码器在同一条命令里**，在 1650 Ti 上一条命令就已经接近饱和
   ⇒ **单机同时跑几个任务必须由"档位数 × 编码格式数"和实测吞吐共同决定**，不能拍"一台机器 4 个任务"。

**落到设计里**：
* `t_worker_capability` 记录：硬件类型、可用编码器、**每 GPU 实测吞吐（实时倍率）**、显式上限（可配）；
* 调度器做**实时预算分箱**：`Σ 已运行任务的预估负载 + 新任务负载 ≤ 实测吞吐 × 安全系数`；
* 每次任务结束后把真实吞吐回写到能力快照（自校准）。

### 11.4 横向扩展的边界

* 控制面（api/rpc/调度/回调投递）：**无状态可任意扩**，共享 MySQL/Redis/对象存储。
* 执行面（Worker）：**有本地 staging 状态**，但它是可重建的（`input_hash` 变了就重跑）。
  节点下线后任务由租约过期被接管 —— 前提是 §10.1 的断点续跑真的实现了（`hvc` 没实现，
  所以它掉线后只能从零重跑）。
* **不要**把 staging 放在会被两个节点同时挂载的共享盘上做"伪续跑"：那会引入文件锁与一致性新问题。

---

## 12. 可观测性

| 面 | 内容 |
|---|---|
| 指标（Prometheus） | 任务计数（按状态/失败码）、队列深度、租约过期数、**每节点实时吞吐倍率**、每步耗时直方图、上传字节/失败数、回调投递成功率与延迟、密钥服务调用延迟 |
| 日志 | 结构化（zap）；每条日志带 `request_id`/`job_id`/`step`；**禁止**出现密钥、签名 URL 的签名部分、源站凭据 |
| 追踪 | OpenTelemetry 贯穿三入口 → 调度 → Worker → 回调（可选，第一期只做 request_id 串联） |
| 运维面 | 节点列表/隔离/恢复、失败队列查看与重放、热配置发布与版本回滚（`hili_video_cloud` DESIGN.md 第 15/16 节的菜单与权限点可直接复用） |

---

## 13. 失败处理与一致性

### 13.1 "完成 = 可播"（修 `hvc` 的洞）

* 回调事件只在 `PUBLISHED` 之后落 outbox；
* `PUBLISHED` 的前置条件里**包含**"清单文件已上传且其中引用的每个分片都已上传并校验 sha256"；
* 回调载荷里带**清单的绝对 URL + 每个产物的 sha256**，业务方拿到就能校验。

### 13.2 失败分类与动作

| 失败 | 动作 |
|---|---|
| 参数非法 / 源不可读 | 立即 `FAILED(PARAM/SOURCE)`，不发回调之外的任何动作（回调里带错误码） |
| ffmpeg 非零退出 | `FAILED(ENCODE)` + 保留 ffmpeg stderr 尾部到 `t_transcode_job_step.error_detail`（截断、脱敏）；`retry_mode=continue` 可从上一成功步骤续 |
| 验收不过（A1–A12） | `FAILED(VERIFY)` + `verify/report.json` 落库；**绝不发布** |
| 上传失败 | 步骤级重排（DB `next_attempt_at`），不重跑转码 |
| 节点失联 | 租约过期 → 接管 → 按 `t_transcode_job_step` 续跑 |
| 回调失败 | outbox 重试 → 死信表 |

### 13.3 不做的兜底（与仓库既有工程偏好一致）

* 不用"内存计时器/watchdog"实现任何超时；超时的判据全部来自**可持久观察的状态**
  （租约过期时间、MQ `delivery count`、ffmpeg 进程是否仍存在）；
* 不用"多级降级"掩盖失败（例如"加密失败就出明文"）——那是交付事故，不是容错。

---

## 14. 安全

1. **入站鉴权**：API Key + 业务方白名单 + 限流；`api_key` 为空时**拒绝服务**而不是放行
   （`vod_live_transcoding` 的 `api_key:""` 直接 `return true` 是明确的反面教材【读码】）。
2. **SSRF**：`input.url` 只允许 `http/https/s3` + 私网地址黑名单（`vod_live_transcoding` 已有这套校验，
   可直接照抄思路：`isPrivateIpAddress` 覆盖 10./172.16-31./192.168./169.254./fc00/fe80）【读码】。
3. **SQL**：全部参数化（`hvc` 用 GORM、`vod_live_transcoding` 有 100+ 处字符串拼接【读码】，
   后者不要学）。
4. **对象存储**：产物默认私有；对外给预签名 URL；分片清单里可写 CDN 公开 URL（由业务决定）。
5. **密钥**：见 §9.2；服务自身配置里的凭据用文件挂载/密钥管理，不写进仓库。
6. **RPC**：gRPC 必须 TLS（`vod_live_transcoding` 用 `InsecureServerCredentials`【读码】，不要学）。
7. **命令注入**：绝不拼接 shell 字符串执行 ffmpeg；Go 用 `exec.CommandContext` + 参数数组
   （这也顺带解决带空格/引号的路径问题）。

---

## 15. 从 Python 迁移的路线图

### 15.1 阶段 0：把 Python 变成"验收预言机"（1～2 天）

* 固定一份黄金输入（短视频、含中文名/空格路径各一）+ 三套参数（明文 / AES-128 / CENC），
  用 Python 跑出**基线产物**并归档（含 `_clear/` 与验收报告）。
* 把 `verify_segmentbase.py` 的 A1–A12 逐条移植成 Go 包 `internal/verify`
  **并先在 Python 产物上跑通**（此时 Go 还没有转码能力，验收器先可信）。

> 这一步是整个迁移的安全绳：**验收器先信，转码后信**。

### 15.2 阶段 1：单机 Worker（引擎与打包）

* `internal/engine/ffmpegcli`：构造 argv（对齐 §3.2 的模板，**含 cwd 设置**，见 §3.3 实测坑）；
* `internal/pipeline`：8 阶段 + staging + 步骤表 + 幂等；
* 目标：对同一份黄金输入，Go 的产物**通过 A1–A12**，且与 Python 基线**逐字段一致**
  （清单 JSON 去掉 ID/时间戳后 diff；分片按 sha256 对比或按解码后帧对比）。
* 本阶段只跑 CLI（`transcode run --job plan.json`），没有服务、没有 DB。

### 15.3 阶段 2：控制面与三入口

* go-zero api + zrpc + MQ consumer → 统一 `CreateJobUseCase`；
* MySQL 表 + 条件 UPDATE 抢单 + 租约；outbox 三通道回调；
* 验收：HTTP/RPC/MQ 三种提交方式对同一 `request_id` 只产生一个任务；回调三通道各收到一次。

### 15.4 阶段 3：横向扩展与对象存储

* 节点注册/心跳/能力广告；吞吐分箱调度；租约接管 + 断点续跑；
* 对象存储上传（multipart）+ 原子发布 + `_staging/` 前缀；
* 验收：杀掉一个正在转码的 Worker，任务在另一台机器上**从断点**完成且产物通过 A1–A12。

### 15.5 阶段 4：加密与 DRM 全量

* `hls_aes128`（含逐片 `#EXT-X-KEY`）→ `cenc`（ffmpeg + KID 三处同步）→ 接入 `drm-keyserver`；
* 验收：密文解回明文与 `_clear/` 逐字节一致；CENC 的 `tenc.default_KID == 旁挂 JSON == MPD`；
  Android/OHOS 真机 + 桌面软解各播一次（**真机验证必须做，不能被"本机通过了"替代**）。

### 15.6 阶段 5（增强，非本期）

边转边传、硬件水印（`overlay_cuda`）、雪碧图/`.bin`、直播转码、VMAF 质量门禁、
AV1/VP9、多音轨/字幕、成本与抢占式调度。

---

## 16. 与现有实现的关系：抄什么、读什么、不碰什么

### 16.1 直接复用（语义照抄）

| 来源 | 内容 |
|---|---|
| `hvc` | 条件 `UPDATE` 抢单 + 租约 + `leaseGeneration` 防回写；Outbox 的 CAS 状态机与死信表；分片上传失败队列；能力快照 + 心跳摘除；`BuildFFmpegArgs` 的参数组织方式；路由与权限的粒度 |
| `drm-keyserver`（本仓库） | **整个密钥/许可证/签名服务**（已经实现 + 31 个单测），转码服务只做客户端 |
| `CicadaPlayerNext` 内核/文档 | scheme 识别与选优、`Stream_meta` 的 DRM 字段口径、清单 JSON schema（以 `MediaManifestParser.cpp` 为准） |
| `转码脚本` | **事实规格**：8 阶段顺序、精确 ffmpeg 参数与理由、逐片 `#EXT-X-KEY`、A1–A12、加密纪律（密钥只在内存/只打指纹） |

### 16.2 读但不要整体移植

| 来源 | 值得读的 | 不要继承的 |
|---|---|---|
| `vod_live_transcoding` | `FfmpegExecutor` 的 fMP4 muxer `movflags`、5 平台编码器参数、网络源 stream-copy 缓存、雪碧图/`.bin`/SHA-256 格式、per-GPU `max_sessions` 配置位 | 3,980 行单文件手写 libav\* 内存管理（分配/释放不配对）、0 测试、字符串拼接 SQL、`api_key:""` 放行、明文 gRPC、`status IN (3,4,5)` 魔法数字 |
| `hili_video_cloud/docs/refactor/DESIGN.md` + `sql/000_full_project_schema.sql` | 31 张表的字段与注释、9 宫格水印锚点换算、调度打分模型、Redis key 与 MQ 命名规范、权限点清单、后台菜单 | §7.2.1"库方式优于命令行"的结论（§2 已用实测反驳）、"分片全部入库"、以及**它的 C++ 代码**（0 个文件能编译） |

### 16.3 明确不做（本期）

| 项 | 为什么 |
|---|---|
| 直播转码 | 两家的直播都不闭环（`hvc` 完全没做；`vod_live_transcoding` 转完就删临时目录）。直播需要独立的"滑动窗口 + 持续上传 + 低延迟清单"设计，塞进 VOD 流水线会把它拖垮 |
| 进程内 libav\* 引擎 | 见 §2；接口留好，需要时再加 |
| 硬件水印 | 先用已验证的软件 overlay；硬件路径要等有可对比成片 |
| 自己实现 CENC 加密 | ffmpeg 原生支持且参考实现已用；我们只负责 KID 同步与验收 |
| FairPlay / SEA / 离线许可证 | 与内核侧同样的取舍与理由（见 `docs/DASH-DRM-SCHEMES.md` §8）|

---

## 17. 未决问题与风险

| # | 问题 | 现状 | 需要什么才能定 |
|---|---|---|---|
| 1 | **ffmpeg 的 CENC 产物在"多 subsample"样本上的行为有未解之处** | 参考实现里 ffmpeg 产物的 150 个样本中 149 个能逐字节解回，唯一例外是那个多 subsample 样本（Bento4 的 150/150 全对） | 需要第二个独立实现交叉验证，或改用 Bento4 产 CENC 分片 |
| 2 | **真机 DRM 播放未验证** | 桌面软解 CENC 通路已实现（本仓库本轮），Android/OHOS 硬解只能真机验 | 真机 + 授权内容 |
| 3 | **`hvc` / `vod_live_transcoding` 当前能否编译** | 本次分析无 shell，无法执行 `go build` / `cmake --build` | 各跑一次干净构建 |
| 4 | **NVENC 上限的机器相关性** | 本机测到 12 路并存且吞吐在 ~6 路饱和 | 在目标生产机上重复 §11.3 的测量（脚本已给出方法） |
| 5 | **源站签名 URL 的有效期 vs 长任务** | 已定策略（plan 阶段先落地本地副本），但"落地"本身的耗时与失败处理未实测 | 用一个会过期的签名 URL 做一次演练 |
| 6 | **`-filter_complex` 在 8 路编码时的 CPU 开销** | 本机只测到 3 路（libx264）与 nvenc 单路 | 在目标机上按真实阶梯（4 档 × 2 编码）跑一次，量 CPU/显存 |
| 7 | **对象存储的原子发布语义** | 方案是 `_staging/` + 批量 `CopyObject` | 需确认目标存储（MinIO/S3/OSS）的 `CopyObject` 一致性与批量上限 |

---

## 附录 A：本文件里所有【实测】的原始记录

环境：Windows，`ffmpeg 8.1-full_build-www.gyan.dev`（`--enable-nvenc --enable-cuvid --enable-libx264`…），
GPU `NVIDIA GeForce GTX 1650 Ti`。

1. **进度流格式**：`-progress pipe:1 -nostats` 输出 `frame/fps/stream_0_0_q/bitrate/total_size/
   out_time_us/out_time_ms/out_time/dup_frames/drop_frames/speed/progress`（每秒/每段一组）。
2. **一条命令多档 + DASH/HLS 同出**：3 路 `split`+`scale`+`libx264`（或 2 路）到 `-f dash
   -use_timeline 1 -use_template 1 -seg_duration 1 -hls_playlist 1`，退出码 0，
   产出 `out.mpd`、`init-*.m4s`、`seg-*-*.m4s`、`media_*.m3u8`、`master.m3u8`；
   MPD 的 `SegmentTimeline` 写的是真实 `d=30720`（timescale 15360 = 2.0s）。
3. **dash muxer 的相对路径坑**：`-f dash <abs>\out.mpd` 时，
   `-init_seg_name/-media_seg_name` 的相对名会落到**子进程 CWD**（实测文件出现在 `D:\hilihili\`），
   MPD 落在指定目录 ⇒ 产物"看起来成功、实际不可播"。
4. **NVENC 并发**：1080p30、`h264_nvenc -preset p1`、每会话 60 秒内容
   → 6 路 14.53s 全成功、12 路 29.68s 全成功 ⇒ 吞吐在 ~6 路饱和（≈25× 实时）。
