# 直播播放器 / FLV 点播 —— 实现与取舍分析

本文对应这一轮的工作：**FLV 点播打通 + 新建直播播放器 UI + 本地直播测试服务**，
以及两项分析（mock 服务用什么语言、要不要做 HLS 直播）。

---

## 1. 这一轮做了什么（结论先行）

| # | 需求 | 结论 | 落在哪 |
|---|---|---|---|
| 1 | 现有播放器支持 **FLV 点播** | 核心本来就支持（ffmpeg 的 `flv` demuxer 已编进 `libffmpeg.dll`），缺的只是**入口** | `HomeWindow.qml`「FLV 视频」按钮 + `openFlvFile()/playLocalFile()` |
| 2 | 首页新增 **FLV 直播地址** 入口 | 已加（地址弹窗 + 校验 + 交给直播窗口） | `HomeWindow.qml` 的 `flvLiveDialog` / `openLiveUrl()` |
| 3 | **新建直播播放器 UI**（不用现有点播那套） | 已新建 5 个文件，独立窗口 + 独立控制条 | `LiveMain.qml`、`LivePlayerView.qml`、`controls/LiveControlBar.qml`、`controls/LiveIconButton.qml`、`controls/LiveMenu.qml` |
| 4 | 本地 **mock 直播服务**（ffmpeg 循环 + 多清晰度 + **GPU 硬编**） | 已实现并实测通过 | `front/player/mock-live/server.js`（零依赖 Node ESM） |
| 5 | 直播缓冲策略 | 新增 `liveMode` 属性：4 秒上限 / 2 秒高水位 / 0.5 秒起播 | `CicadaPlayerItem.h/.cpp`（不设它跑的就是框架默认 40 秒 → "延时 40 秒的直播"） |
| 6 | 要不要做 HLS 直播 | **已做，但 mock 里默认关**（生产一般只推 FLV）：`--hls` 打开、`--hls-encode` 让它 1 秒一片；播放器侧按后缀自动分流（填单档地址会自动升级到同目录 master） | 见第 5 节 |

---

## 2. mock 直播服务：为什么用 Node，够不够用

### 2.1 这个服务到底在干什么

它**不做转码**。转码全部交给 ffmpeg + 显卡：

```
一个视频文件（无限循环）
   │  -re -stream_loop -1
   ▼
ffmpeg（每档清晰度一条进程）
   NVDEC 解码 ──► scale_cuda 缩放 ──► h264_nvenc 编码 ──► FLV
   （显存里）      （显存里）           （显卡固定功能单元）      │
                                                              ▼ stdout
Node 服务：切 FLV tag（找关键帧/存序列头）→ 扇出给所有 HTTP 客户端
```

所以 Node 承担的只有两种活，而且都很轻：

1. **字节流切片 + 扇出**：把 ffmpeg 写进管道的 FLV 字节切成 tag，缓存"FLV 头 + onMetaData + AVC/AAC 序列头 + 最近一个 GOP"，新客户端连上来先补发这一串（否则晚进来的客户端**永远拿不到 SPS/PPS，必然花屏或解不出来**），之后实时转发；
2. **几个 JSON/文件接口**：清晰度清单、运行状态、FLV 点播文件（带 Range）。

### 2.2 实测数字（本机：GTX 1650 Ti 4GB / 驱动 596.36 / ffmpeg 8.1）

| 项目 | 实测 |
|---|---|
| 全 GPU 管线是否可用 | `nvenc=true cuda解码=true scale_cuda=true` |
| 编码吞吐（4 档，源 1080p30，5 秒内容） | 1080p **1.29s**、720p **0.93s**、480p **0.84s**、360p **0.74s** == 1080p 约 **3.9× 实时**、360p 约 **6.8× 实时** |
| 一起跑 1× 实时时占 NVENC 的比例 | 1/3.9 + 1/5.4 + 1/6.0 + 1/6.8 ≈ **0.77** → 还有约 23% 余量 |
| ffmpeg CPU 占用（1080p 那一路） | 35 秒墙钟用了 **5.2 秒 CPU**（约单核 15%）→ 解码/缩放/编码确实都在显卡上 |
| GPU 占用（3 档 + 几个客户端） | GPU 39%、**编码器 26%**、解码器 43%、显存 1.9GB |
| Node 服务内存 | **49~53 MB**（3 档 + 3 个客户端） |
| 并发扇出 | 同一路 3 个客户端同时拉，`clients=3`、`dropped=0`、无积压 |
| 晚接入能不能秒开 | ffmpeg 读完 720p 直播流**3 秒内容只花 3.28 秒**、**零解码错误零警告** → 序列头 + GOP 补发是对的 |

按带宽算更直观：1080p 一档 5 Mbps ≈ 0.62 MB/s；3 档合计入 ~1.9 MB/s，
5 个客户端看同一档再出 ~3 MB/s。**总共几个 MB/s 的内存拷贝**，对任何运行时都算不上负载。

### 2.3 Node / Python / Go 取舍

| | 适不适合 | 说明 |
|---|---|---|
| **Node.js**（本项目其余 mock 服务已经在用：零依赖 ESM、Node ≥18、端口 9101） | ✅ **够用，就用它** | 活是"读管道 → 切 tag → 写 socket"，典型的 I/O 扇出；实测整机 CPU < 5%，内存 50MB 级。而且 FLV tag 解析（11 字节头 + dataSize + PreviousTagSize、关键帧判定、序列头识别）用 JS 写一遍只有几十行。**和现有 mock 服务同一套约定**，不用再教一遍怎么跑。 |
| Python | ⚠️ 够用但没好处 | 同样的活 `asyncio` 完全能干（甚至更省内存），但：要么引 `aiohttp/uvicorn`（破坏"零依赖"这一条），要么用 `http.server` + 线程（扇出模型反而更别扭）。FLV 解析得重写一遍。**没有任何一项比 Node 强**，所以不换。 |
| Go | ❌ 这一轮不需要 | Go 的强项是"几十万并发连接"和"CPU 密集解析"。这里真实的天花板**不是 Node，是 NVENC**（见下），并发客户端数量在测试场景里是个位数到几十。上 Go 只会多一套工具链（工程里其它 mock 服务都是 JS）。**如果**以后要做"几千观众同时看的压测服务"，再考虑 Go 重写扇出那一层。 |

### 2.4 真正的瓶颈与限制（比语言重要）

* **NVENC 会话数**：消费级显卡老驱动限制同时 3 个 NVENC 会话，驱动 550+ 放宽到 8（本机 596 ✓）。
  超了 ffmpeg 会直接失败（`OpenEncodeSessionEx failed`），服务里表现为"这一档一直重启"，
  日志会写清楚是哪一档。**建议这台 1650 Ti 上最多 4 档**。
* **4K 源**：1650 Ti 的 NVENC 能编 4K，但 4K 实时编一路就吃掉大半，再加 3 档缩小的会顶到上限。
  所以默认档位是 `1080p,720p,480p,360p`，源是 4K 时会**先缩到 1080p 再编**（`--qualities` 可调）。
* **延迟**：`-tune ll` + CBR + **1 秒关键帧** + `-flush_packets 1`，实测端到端 1~2 秒量级；
  关键帧间隔同时决定了"新观众多久出画面"和"切清晰度多快恢复"。

---

## 3. FLV 点播：核心本来就支持，缺的只是入口

（这一节是代码审计的结论，避免以后再翻一遍。）

* 协议识别：`CicadaPlayerItem.cpp:1874` 已经 `.flv → "FLV"`，`protocolDetailFor()` 也有 FLV 文案；
* 解封装：走 ffmpeg 的 `av_probe_input_format2()`（`framework/demuxer/avFormatDemuxer.cpp:816`），
  `CONFIG_FLV_DEMUXER 1` / `CONFIG_LIVE_FLV_DEMUXER 1` 都在打包进去的 `libffmpeg.dll` 里；
* 文件过滤器：`AppFileDialogs.cpp:84`、`VideoLibrary.cpp:34-39` 里本来就有 `flv`；
* **所以点播不需要改核心**，这一轮补的是"首页能选到它"：
  * 「FLV 视频」按钮 → C++ 原生文件框（过滤器第一项就是 `*.flv`）→ 现有**点播**窗口播放；
  * 本地文件、`http(s)://…flv` 点播地址两条路都支持（`playLocalFile()`，非盘符路径走统一的 `normalizeAddress()`）。

---

## 4. 直播播放器：参考能给什么、不能给什么

**参考实现 `front/hilihili-live`**（B 站直播房间页的抓取版）能给：

* 播放器盒：**1130×630、圆角 4、底色 rgba(0,0,0,.7)**（响应式 928×522 / 832×468）；
* 底部 **56px 渐变条** `.web-player-controller-bg`：`linear-gradient(rgba(0,0,0,0), rgba(0,0,0,.7))`；
* **弹幕层契约** `.web-player-danmaku`（`z-index:7`、`pointer-events:none`）+ `.danmaku-x-dm-rotate`（**15° 斜向轨道**）
  + `.danmaku-x-dm` 的 `--fontSize:25px / --color:#fff / --textShadow 四向 1px 黑描边 / --duration:7.55s`；
* 关注按钮 `.player-follow`：**89×32、圆角 26、rgba(0,0,0,.4)**，头像 24×24 圆形；
* 设计令牌：`#00a1d6`（主色）、`#00aeec`（功能色）、`#FB7299`/`#ff6699`（粉）、
  `hsla(0,0%,8%,.9)`（浮层底）、清晰度菜单项 **36px 高 / hover 白 10%**。

**参考给不了的**（工作区里确实没有，只能按 B 站直播的实际观感设计并写清楚）：

* **真正的直播控制栏是个空 div**（`.web-player-controller-wrap` 里什么都没有，真身是运行时才注入的）；
* 所以"直播中"状态、**清晰度档位表**、人气值这些东西在参考里**一个都没有**；
* 右侧那块是**推荐房间列表**，不是弹幕列表。

**本项目的处理原则**：凡是能拿真数据的地方一律真数据 ——

* 顶部那一行状态是 **真实读数**：`1920x1080 · 5000kbps · 12.3 MB/s · 30 fps`
  （来自 `player.stats` / `player.downloadSpeedText`，不是编的"人气 12345"）；
* **不编人气/粉丝数**：没有账号体系就不显示假数字（关注按钮点了会说明"未接入"）；
* 清晰度档位来自服务端 `streams.json`（本地服务真实提供），拿不到就说"这个地址没有清晰度清单"，
  而不是摆几个假档位；
* 弹幕：`mock-live` 不带弹幕服务，所以直播界面用**本地示例弹幕**跑通"上屏 + 输入框 + 开关"这条链，
  并在日志/提示里写明；真要实时弹幕，把 WebSocket 地址填进 `DanmakuView.liveUrl` 即可（接口已经留好）。

### 4.1 直播特有、这一轮真做了的功能

| 功能 | 做法 |
|---|---|
| **没有进度条/时长** | 直播控制栏里根本没有进度条（B 站直播也是这样）；时长恒为 0 |
| **切清晰度 = 换流重连** | HTTP-FLV 没有清单，档位来自 `streams.json`；换档 = `source = 另一条 .flv`（先置空再设，因为 `setSource` 对**同一地址**会直接返回） |
| **自动重连** | 断流/出错触发，指数退避（1→2→4→8 秒，最多 5 次），界面上给提示；连上后自动清零并提示"已重新连上直播" |
| **回到直播** | 暂停后点它 = 重连一次（HTTP-FLV 不能 seek，重连拿到的就是最新直播位置） |
| **缓冲收小** | `liveMode=true` → 4 秒上限 / 2 秒高水位 / 0.5 秒起播（否则是框架默认 40 秒 = 延时 40 秒） |
| **统计信息面板** | **直接复用点播那个 `PlayerInfoPanel`**（同一份数据链路 + 曲线），没必要再写一个 |

---

## 5. HLS 直播（本轮已实现）

### 5.1 mock 侧：一条 ffmpeg 出两路（FLV + HLS）

**HLS 直播默认关闭**（用户要求）：生产环境一般只推 FLV，所以本地默认也按生产来 ——
只出 FLV 直播 + FLV 点播；要验证 HLS（多码率清单 / 清单内换档 / iOS 那条路）才加 `--hls`。

| 参数 | 作用 |
|---|---|
| `--hls` | **打开 HLS 直播**（默认关；关着时 `/live/hls/**` 直接 404、`streams.json` 里 `hls` 段为 null） |
| `--no-hls` | 显式关闭（默认就是关，写出来只是让脚本更明确） |
| `--hls-encode` | HLS 那一路**也重编**（同 `--hls-encode` 一节；会顺带打开 `--hls`） |
| `--hls-time <sec>` | 分片时长（默认 1 秒；**copy 时受源关键帧限制**，`--hls-encode` 时才是真的 1 秒） |
| `--hls-list <n>` | 播放列表窗口长度（默认 6 → 约 6 秒滑动窗口） |

地址（`--hls` 打开后）：

```
http://127.0.0.1:9200/live/hls/master.m3u8      ← 一个地址带全部档位（播放器填这个）
http://127.0.0.1:9200/live/hls/720p/index.m3u8  ← 只测一档时用（播放器会自动升级到同目录 master）
```

**关键实现点（三个都是踩出来的）**：

1. **同一个 ffmpeg 进程同时出 FLV 和 HLS 时，HLS 那路必须显式写编码参数。**
   ffmpeg 的编码器/滤镜参数是**按输出**生效的；第二个输出不写编码器就会退回默认的 **libx264**，
   而我们开了 `-hwaccel_output_format cuda`（帧在显存里），CPU 编码器吃不下 →
   `Impossible to convert between the formats supported by the filter …`（src: cuda → dst: yuv420p）
   → 该档 ffmpeg 每秒崩一次、无限重启。默认用 `-c copy`（同一条已编好的 H.264/AAC 直接封进 TS）。
   实测：4 档全程 `restarts=0`。
2. **master 清单一律自己写**（不用 `-master_pl_name`）：它是按"每档一个子目录"组织的，
   官方的自动 master 只会写在媒体清单同一个目录里。自己写还能把 `BANDWIDTH/RESOLUTION/NAME` 写准。
3. **`-c copy` 的代价（"HLS 直播没有 FLV 流畅"的真因）**：`-c copy` 时 ffmpeg
   **直接从输入取包、绕过编码器**，所以 HLS 分片里装的是**源视频的原始码流** ——
   源片 5 秒一个关键帧，分片就只能 5 秒一片（`-hls_time 1` 只是目标值），码率也不平稳；
   而播放器是"整片下载完才能播 + 每 `TARGETDURATION/2` 才刷新一次清单"，
   片越长、抖动越被放大成停顿。FLV 那一路是重编的（`-tune ll -rc cbr`、关键帧 1 秒），所以又稳又跟手。
   → 想让 HLS 也接近 FLV：加 **`--hls-encode`**（同参数重编，分片真的 1 秒一片）。
   代价是**每档多占 1 个 NVENC 会话**（消费卡上限 8：3 档 ×2=6 安全，4 档 ×2=8 顶到上限）。
   注意：即便如此，HLS 的**延迟**仍会比 FLV 高一个量级（分片协议天性，改不了）。

直播 HLS 的清单规矩：**不写 `#EXT-X-ENDLIST`**（写了播放器就认为直播结束）、
`delete_segments`（滚动窗口，旧分片删掉）、`independent_segments`、`program_date_time`。
服务端送 `.m3u8` 用 `application/vnd.apple.mpegurl`、`.ts` 用 `video/mp2t`，并且**禁止缓存**
（播放器每隔一个分片就来拉同一个 index.m3u8，缓存住就会"播几秒卡住"）。

实测证据（本轮）：

```
master.m3u8        : 3 档（原画/720P/480P），BANDWIDTH/RESOLUTION/FRAME-RATE 都在
720p/index.m3u8    : 没有 #EXT-X-ENDLIST；MEDIA-SEQUENCE 8、6 个分片、带 PROGRAM-DATE-TIME
响应头              : application/vnd.apple.mpegurl + Cache-Control: no-cache, no-store
磁盘                : 滚动窗口只留 6 个 ts（旧的被删）
ffmpeg 直接解码      : 读 http://…/live/hls/720p/index.m3u8 解 3 秒 → 0.53s、**零警告**
命令行播放器         : cicadaPlayer.exe 播 master.m3u8 连续 16 秒不崩、CPU 正常
```

### 5.2 播放器侧：FLV 与 HLS 的**分流规则**（这是 HLS 直播相对 FLV 的最大优势）

| | FLV（HTTP-FLV） | HLS（HTTP-HLS） |
|---|---|---|
| 清晰度从哪来 | **没有清单**，靠服务端另外给的 `streams.json` | **master.m3u8 自带**，核心解析后经 MediaInfoGet → `player.qualities` |
| 切档怎么做 | 换一条 `.flv` → **断开重连**（黑一下、等一个 GOP，约 1 秒） | `player.selectQuality(streamIndex)` → **清单内换流，不重连、不黑屏** |
| 单位 | 一档一个地址 | 一个地址带全部档位 |

UI 侧（`LivePlayerView.qml`）只按后缀分流一次，其余共用：
`readonly property bool isHls: source.toLowerCase().indexOf(".m3u8") >= 0`，
档位表 `isHls ? hlsQualityList（来自 player.qualities） : renditions（来自 streams.json）`，
切档 `switchQuality(item)` 里判 `item.streamIndex` 是否存在。
**没有为 HLS 另写一套界面** —— 直播窗口、控制栏、弹幕、统计面板全都复用。

### 5.3 仍然**没有**打开的那一项：核心的直播追赶

`RTMaxDelayTime` 依旧是 0（关闭）。原因没变：那段（缓冲修剪 + `LiveCatchUp` + `LiveTimeSync`）
是给 HLS/DASH 写的，`LiveTimeSync` 里用 `mUtcTimer`，而 **FLV 直播不会创建 UTC 定时器**
（`SuperMediaPlayer.cpp:1590-1601` 有保护，但那条路显然不是为 FLV 准备的）。
HLS 直播理论上能走到 UTC 同步，但"第一次打开"必须专门验一轮（延迟、卡顿、音画同步都要看），
所以这一轮仍然按"小缓冲（4s/2s/0.5s）"来控延迟，不碰追赶逻辑。

### 5.4 结论（对上一轮那份分析的修正）

上一轮我判断"HLS 直播不必做"，理由是"用户目标已达成 + 风险大于收益"。现在用户明确要求做，
并且做下来发现：**收益是实的**（多码率清单 + 清单内无感换档），**成本主要在 mock 一侧**
（播放器侧只多了一次协议分流），**唯一仍然悬着的是核心的直播追赶**（5.3）。
所以结论修正为：**FLV 与 HLS 两条直播都保留**（FLV 是 B 站直播的主力传输、延迟更低，
生产一般也只推 FLV；HLS 用来验证多码率清单与无感换档）。
按用户要求，**mock 里 HLS 默认关闭**（`--hls` 打开、`--no-hls` 显式关），
这样默认行为就与生产一致；要测 HLS 再加参数，播放器侧不用改。

---

## 5b. 直播 UI 的数值来源（哪些是"查到的"，哪些是"近似的"）

用户要求按 `https://live.bilibili.com/8178490…` 的真实页面实现。**能查到的都换了官方值**，
查不到的地方明确标出来 —— 不把记忆当事实。

### 5b.1 已核实（有出处，已落到代码）

**第二批（用户提供了官方 bundle `room-player.min.js`，配置条的真实 CSS-in-JS 直接从里面抽出来）**

抽取方式：`platform/QtPlayer/build/extract-live-css.js`（找 `选择器{...}` 片段 + 关键词归类 + 抓上下文），
报告落在 `platform/QtPlayer/build/live-css-report.txt`（748 KB 的 bundle 里抽出 4974 条规则，
其中控制条容器 15 条、清晰度菜单 64 条、弹幕层 63 条）。**下面这些都是真值**：

| 元素 | 官方值 |
|---|---|
| 底衬 `.web-player-controller-bg` | `position:absolute; bottom:0; width:100%; z-index:10; height:56px; background-image:linear-gradient(180deg, rgba(0,0,0,0), rgba(0,0,0,.7))`（**两段渐变、56px**；早前那份"transparent 20% → .9"是第三方覆盖值，不是官方值） |
| 内容行 `.control-area` | `position:absolute; bottom:3px; left:0; right:0; padding:0 12px; color:#fff; height:36px; font-size:12px; display:flex; align-items:center` |
| 左/右组 | `.left-area,.right-area{height:100%;display:flex}`；**`.right-area{flex:1; flex-direction:row-reverse}`**（右组贴右边） |
| 按钮 `.icon` | `display:inline-block; width:36px; height:36px; cursor:pointer`；`svg{fill:rgba(255,255,255,.9)}` → `svg:hover{fill:#fff}` |
| 文字项 `.text` | `line-height:36px; padding:0 6px; color:rgba(255,255,255,.9)` → `:hover{color:#fff}` |
| 浮层壳 `.panel`（清晰度/弹幕设置都用它） | `position:absolute; left:50%; transform:translate(-50%,-100%); opacity:.9; background-color:rgba(33,33,33,.9); color:rgba(255,255,255,.9); border-radius:4px; z-index:100` |
| **清晰度面板** | `.quality-wrap .panel{padding:9px 0; min-height:94px}`；`.quality-it{cursor:pointer; line-height:22px; padding:0 20px; white-space:nowrap}`；`:hover{background-color:rgba(255,255,255,.1)}`；**`.quality-it.selected{color:#23ade5}`（当前档就是"文字变 #23ade5"，没有圆点）** |
| 线路选择器（**暂未做**：本项目只有一条线路） | `.line-wrap{width:66px; margin-bottom:28px; padding:3px 20px 0 20px}`；`.line-selector{width:66px; border:1px solid rgba(255,255,255,.4); border-radius:2px; background:#212121; transition:height .1s}`；`.line-label,.option{height:22px; padding-left:9px; line-height:22px}`；`.arrow{0×0; top:8px; right:8px; border:4px solid transparent; border-top:6px solid #797979}` |
| 弹幕设置面板（**暂未做**，可作下一步） | `.danmaku-control.panel{width:298px; padding:20px; box-sizing:border-box; left:auto; right:12px; transform:translate(0,-100%)}`；四行：**不透明度 / 字号缩放 / 同屏密度 / 显示区域** |
| 弹幕配置默认值 | `{display:true, opacity:100, fontScale:100, density:100, area:4, enableMask:true, showMaskOption:false}`（存在 localStorage 的 `web-player-ui-config`） |
| 弹幕引擎参数 | `{isLive:true, type:"div", opacity:1, fontSize:1, duration:6, danmakuArea:100, isRecycling:true}` → **弹幕飞过时长 6 秒**、DOM(div) 渲染 |
| 音量默认值 | video 元素 `volume=.9`；用户设置默认 `{volume:{value:90}}`（0–100） |
| 全屏底栏 | `.gift-control-section{height:48px}`、`#fullscreen-container`、`.fullscreen-container-paddingbox{height:48px}` |
| 侧栏宽度 | `--live-chat-panel-width:300px` |
| 清晰度档位名 | 原画 10000 / 蓝光 400 / 超清 250 / 高清 150 / 流畅 80（**直播没有"自动"**） |
| 品牌色（v11 主题） | `--Pi5:#FF6699`（粉，旧的 `#FB7299` 已不在主题文件里）、`--Lb5:#00AEEC`、`--Re5:#F85A54`、`--Gr5:#2AC864` |

**第一批（官方样式表 / 文档 / 主题文件）**：右键菜单 `.player-contextmenu` 那套（`rgba(28,28,28,.9)`、
条目 `padding:3px 20px; width:200px; line-height:22px`）—— 注意**那是右键菜单，不是清晰度面板**；
弹幕输入框（placeholder 原文 `发个弹幕呗~`、`maxlength=30`、`border-bottom:2px solid #fff8`、高 24px）；
回到直播（高 22、圆角 12、`rgba(51,51,51,.8)`、6px `#ff5243` 红点）；顶部房间信息类名与 32×32 头像。

### 5b.2 查不到、因此仍是"近似"的（连同原因）

| 元素 | 现状 | 为什么查不到 |
|---|---|---|
| 顶部房间信息 / 关注按钮 / 回到直播 / 弹幕输入框的实际样式 | 用"第一批"里能查到的数值（32×32 头像、89×32 关注按钮、22px 回到直播、24px 下边线输入框） | 这些属于**直播间页面**的代码，不在播放器 bundle 里（bundle 只管播放器本体） |
| 图标形状本身 | 用工程里已有的 SVG/Lottie 素材 | bundle 里图标是内联 SVG 组件，形状要逐个还原；当务之急是布局与交互 |
| 人气值 / 关注数 | **不显示假数字**：那一行放真实的分辨率/码率/缓存速度/帧率 | 我们不是真的接进了某个直播间 |
| 弹幕层的倾斜角/字号/描边/动画时长 | 沿用工作区那份直播参考（15° 轨道、25px、四向 1px 黑描边）；bundle 里只查到引擎参数 `duration:6` | 倾斜角等在 `live-danmaku-engine-v2` 的样式里，这份 bundle 只到引擎接口层 |

> 还能再往下挖的两份材料：① `room-player.min.css`（右键菜单那套的真值）；② 直播**房间页**自己的 CSS
> （顶部信息/关注/弹幕输入/回到直播的真值）。把这两份下载下来发我，就能把表里剩下的"近似"也换成真值。

---

## 6. 怎么跑、怎么验

### 6.1 起直播服务（本机已经验证过全流程）

```powershell
# 用你自己的视频（会无限循环；不传 --video 会用 ffmpeg 合成一个 20 秒测试画面）
node D:\hilihili\front\player\mock-live\server.js --video D:\movies\demo.mp4 --port 9200

# 会打印：
#   【FLV 直播地址】
#     原画   1920x1080  5000kbps  http://127.0.0.1:9200/live/1080p.flv
#     720P   1280x720   2500kbps  http://127.0.0.1:9200/live/720p.flv
#     ...
#   【FLV 点播】http://127.0.0.1:9200/vod/sample.flv
#   【清晰度清单】http://127.0.0.1:9200/live/streams.json
#   【运行状态】  http://127.0.0.1:9200/live/status.json
```

服务端会自己检查 `nvenc / cuda 解码 / scale_cuda` 三个能力，缺任何一个都会**明确报错并退出**
（不会偷偷退回 CPU 软编 —— 用户要求必须走显卡；排障时才显式加 `--no-gpu`）。

### 6.2 播放器侧验收点

| # | 操作 | 期望 |
|---|---|---|
| L1 | 首页 →「FLV 视频」→ 选一个 `.flv`（或用 `http://127.0.0.1:9200/vod/sample.flv`） | 用**现有点播窗口**播放，有时长、能拖进度、能暂停 |
| L2 | 首页 →「FLV 直播地址」→ 填 `http://127.0.0.1:9200/live/1080p.flv` | 打开**直播窗口**（另一套 UI）：**没有进度条**，左下角有弹幕输入框，底部控制条有"● 直播中 · 1920x1080 · 5000kbps"，1~2 秒内出画面 |
| L3 | 点控制条上的清晰度 → 选 720P | 提示"正在切换到 720P"，断开重连另一条流，1 个 GOP（约 1 秒）内恢复；顶部状态里的分辨率立刻变成 1280x720 |
| L4 | 暂停 → 出现「回到直播」→ 点它 | 重连后回到最新直播位置（不是从暂停处继续） |
| L5 | 把 mock 服务 Ctrl+C 停掉 | 界面出现"重连中"提示并按 1/2/4/8 秒退避重试；重新起服务后自动连上并提示"已重新连上直播" |
| L6 | 右键菜单 →「视频统计信息」（直播界面里是控制条那颗 info 图标） | 弹出的是**同一个**统计面板：数据在动、曲线在画（本轮同时修掉了曲线压字的坐标系 bug） |
| L7 | 日志 | `liveMode = true`、`live stream (FLV/HTTP): buffer capped at 4s …`、`[live] 清晰度清单：4 档（…）`；服务端 `[live] 客户端 … 接入，补发 …KB（头+序列头+N 个 tag 的 GOP）` |

### 6.3 服务端自检（不依赖播放器，命令行就能验）

```powershell
# 晚接入的客户端能不能直接解码（这一条最能说明补发逻辑对不对）
ffmpeg -hide_banner -v warning -i http://127.0.0.1:9200/live/720p.flv -t 3 -f null -
# 期望：约 3~4 秒跑完，stderr 一行都没有（本轮实测 3.28s、零警告）

# 服务端状态（每路客户端数/进出的字节数/重启次数/是否就绪）
curl http://127.0.0.1:9200/live/status.json

# 调试首页（列出所有地址 + 实时状态）
start http://127.0.0.1:9200/
```
