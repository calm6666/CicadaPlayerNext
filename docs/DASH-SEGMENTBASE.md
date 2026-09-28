# DASH SegmentBase（单文件 + sidx + 字节范围）支持矩阵与改造方案

本文回答两个问题：**播放器现在支持 DASH SegmentBase 吗**、**转码脚本要怎么改成一次同时产出
分段式与 SegmentBase 式清单（含 SegmentBase JSON）**。所有结论都给了代码位置，便于逐条核对。

---

## 一、结论速览

| 清单类型 | SegmentBase 单文件模式 | 结论 | 依据 |
|---|---|---|---|
| **DASH MPD**（`<SegmentBase indexRange=...><Initialization range=.../>`） | 支持，端到端可用 | ✅ 已支持 | 见 §二 证据链 |
| **MediaManifest JSON**（`mode:"single"` + `indexRange`） | 解析了，但原本**建不出任何媒体段** | ✅ **本轮已实现**（见 §三） | `ManifestDemuxer` 的 Single 分支 |
| MediaManifest JSON（显式列表 + 每段 `byteRange`） | 支持（不依赖 sidx） | ✅ 已支持 | `ManifestDemuxer.cpp:279-284` |

也就是说：**MPD 侧的 SegmentBase 不用改**；**JSON 侧的 SegmentBase 是缺口，本轮已补齐**（实现与验证见 §三、§七）。

---

## 二、MPD SegmentBase 为什么说"已经支持"（证据链，逐环可查）

1. **解析**：`framework/demuxer/dash/MPDParser.cpp:316-341`
   - `parseSegmentBase()` 建 `SegmentBase` 属性；只有 indexRange、没有显式 Initialization 时，
     会**合成 init 段** `[0, indexRangeEnd]`（`:330-336`）。
   - `parseCommonSegmentBase()`（`:573-598`）解析 `indexRange`：建 index 段并
     `setByteRange(start,end)`，同时把单文件自身的 byteRange 设成 `[indexEnd+1, ∞)`
     （注释原话："index must be before data, so data starts at index end"）；还解析 `timescale`。
2. **拉 index 并解析 sidx**：`framework/demuxer/dash/DashSegmentTracker.cpp:267-307`
   - 只在 `mRep->needsIndex()` 时做：取 `getIndexSegment()` 的 URL 与字节范围，
     用数据源 `Seek(startByte)` + 按范围 Read，然后 `Dash::SidxParser::ParseSidx()`，
     再 `parseIndex()`。
   - `parseIndex()`（`:74-105`）把 sidx 的每个 reference 变成一个子段：
     `point.offset = sidx.first_offset + endByte + 1`，然后逐条累加 `referenced_size` 得
     每段的 `[offset, offset+size-1]`，并用 `sidx.timescale` 覆盖 Representation 的 timescale。
3. **取段**：`framework/demuxer/dash/SegmentBase.cpp:37-47`（`getMediaSegment` 返回 `subsegments[pos]`）、
   `:54-63`（`getSegmentNumberByTime` 用 timescale 把时间换算成段号）。
4. **字节范围真的下发到数据源**：`framework/demuxer/dash/DashStream.cpp:163 / 315 / 732`
   都是 `tryOpenSegment(uri, seg->startByte, seg->endByte)`，`tryOpenSegment` 里
   `mPdataSource->setRange(start, fixEnd)`（`:477-488`）；`IDataSource::setRange`（`data_source/IDataSource.h:115`）
   落到 curl 的 Range 头（`data_source/curl/CURLConnection.h:80 sendRange`）。

**结论**：只要 MPD 里写的是 `SegmentBase indexRange`（单 m4s 含 sidx），这条链是通的，不需要改动。
需要真机/桌面实测的是"命中/不命中"这类行为细节（本机没有鸿蒙设备）。

---

## 三、JSON（MediaManifest）侧的缺口与实现方案

### 3.1 现状（缺口）

- 数据结构**已经为 SegmentBase 设计好了**：`framework/demuxer/manifest/MediaManifest.h:80-84`
  有 `enum class SegmentMode { Single /* one file + byte ranges (SegmentBase) */, Template, List }`，
  `:93-94` 有 `initialization`（注释："single mode: byte range"）与 `indexRange`（注释："single mode: sidx range"）。
- 解析也读到了：`MediaManifestParser.cpp:115` `out.indexRange = item.getString("indexRange","")`。
- **但建段逻辑没有 Single 分支**：`framework/demuxer/manifest/ManifestDemuxer.cpp:245-255`
  只对 `List`/`Template` 建 `segments`；`:262` 还显式跳过 Single 的 init 段
  （`segInfo.mode != SegmentMode::Single`）；全文**没有一处使用 `indexRange`**。
  ⇒ `mode:"single"` 的 JSON 会让 Representation 拿到一个**空段表**：既没有 init，也没有媒体段，
  表现为"清单能打开、一直没有数据"。

### 3.2 实现方案（内核，`ManifestDemuxer`）

在 `ManifestDemuxer` 里补齐 `SegmentMode::Single`，语义与 MPD 路径**逐条对齐**（避免两套口径）：

1. **单文件 URL**：`rep.baseUrl`（JSON 里该 representation 的 `baseUrl`/`url`）即那个唯一 m4s。
2. **init 段**：`initialization` 是 `"start-end"` 字节范围 → 建 init segment 并
   `setByteRange(start,end)`，挂到 `segmentList->addInitSegment()`（现在就漏了这一步）。
3. **靠 sidx 推媒体段**（`indexRange` 非空时）：
   - 复用与 `DashSegmentTracker::parseIndex` **同一套**偏移基准
     （`first_offset + indexRangeEnd + 1`，见上面 `DashSegmentTracker.cpp:78-85`），
     用 `Dash::SidxParser` 解析拉到的 index 范围；
   - 每个 reference 生成一段：`url` = 单文件 URL、`byteRange = "off-(off+size-1)"`、
     `duration = subsegment_duration / timescale`（秒，供 `segment::duration` 用）、
     `startTime` 逐段累加（与现有 JSON 路径 `:271-277` 的算法一致）；
   - timescale 从 sidx 取（`SidxBox.timescale`），不要用 JSON 里的 `timescale` 去猜。
4. **显式列表兜底**（`indexRange` 为空、但 `segments[]` 里给了 `byteRange`）：
   走今天已经可用的那条路（`:279-284`），**不改**。
5. **失败不静默**：拿不到 index 范围、或 sidx 解析失败时，返回明确的框架错误码并按现有风格打日志，
   不要"清单打开成功但永远没数据"。

约束遵守：不新增配置开关、不写按时间判定的分支、不加平台宏（这是 L1 核心）、
新成员追加在类末尾、新虚函数（如必须）追加在 vtable 末尾。

---

## 四、转码脚本改造方案（`D:\hilihili\转码脚本`）

现状：`transcode_all.py`（49 KB）一次 ffmpeg 调用产出 **分段式 DASH**（`output.mpd` + `chunk-stream*`）
与 **HLS**（`master.m3u8`/`media_*.m3u8`），再调 `convert-to-manifest.py`（23 KB）生成
`test-hls-{version}.json` / `test-dash-{version}.json`（模板）与两者 `-explicit`（显式分片）共 4 个 JSON。

本机 ffmpeg 8.1（`D:\ffmpeg-8.1-full_build\bin\ffmpeg.exe`）已确认支持我们需要的三个参数：
`-single_file`（"Store all segments in one file, accessed using byte ranges"）、
`-single_file_name`、`-global_sidx`（"Write global SIDX atom. Applicable only for single file, mp4 output, non-streaming mode"）。

### 4.1 目标产物清单（✅ = 新增）

| 产物 | 说明 |
|---|---|
| `output.mpd` | 现状：分段式（SegmentTemplate + SegmentTimeline） |
| ✅ `output-segmentbase.mpd` | SegmentBase 版：每个 Representation 一个 m4s，`<BaseURL>` + `<SegmentBase indexRange=…><Initialization range=…/></SegmentBase>` |
| ✅ `sb-{video,audio}-<id>.m4s` | SegmentBase 用的**单一 m4s**（init + media + 全局 sidx 都在里面） |
| `chunk-stream*-*.m4s` | 现状：分片文件 |
| `master.m3u8` / `media_*.m3u8` | 现状：HLS |
| `test-hls-{version}.json` / `test-dash-{version}.json`（+ `-explicit`） | 现状：4 个 JSON |
| ✅ `test-dash-{version}-segmentbase.json` | SegmentBase 模式 JSON（字段与现有一致，见 4.3） |

### 4.2 落地做法：主命令不动 + 一条 `-c copy` 再封装（**已实现的方案**）

⚠️ **为什么不是"在主命令里追加第二个 `-f dash` 输出"**：ffmpeg 官方文档（Stream selection →
"Complex filtergraphs"）规定 *"Complex filtergraph output streams with labeled pads must be mapped
once and exactly once"* —— 把同一个 `[outN]` 映射给两个输出文件会**整条命令致命失败**
（"None of the output files shall be processed"），连现有产物都一起没了。要合法就必须给每条流加
`split=2`、并把 `-map/-c:v/-b:v/-g/-tag:v` 等**每个输出各重复一遍**（非全局选项只作用于紧邻的
下一个输出文件），于是 6~8 路 NVENC 会变 12~16 路并发编码会话，很容易撞上消费级显卡的上限而
整条失败 —— 那反而破坏了"现有产物零影响"这条硬约束。

**所以落地方案是**：主转码命令**一个参数都不改**，跑完再加一条 `-c copy` 的再封装命令：

```text
ffmpeg -y -i <OUTPUT_DIR>/output.mpd -map 0 -c copy -f dash \
  -single_file 1 -single_file_name sb-$RepresentationID$.m4s -global_sidx 1 \
  -use_template 1 -use_timeline 0 -hls_playlist 0 -seg_duration 6 \
  <OUTPUT_DIR>/output-segmentbase.mpd
```

- `-hls_playlist 0` **必须**：否则单文件版会覆盖现有 `master.m3u8` / `media_*.m3u8`；
- `-use_timeline 0`：SegmentBase 的段时长来自 sidx 的 `subsegment_duration`，不来自 SegmentTimeline；
- 只编码一次、不额外占编码器，`output.mpd` / `chunk-stream*` / HLS **零影响**；
- 失败只影响新产物，脚本明确报错并以非零退出。

⚠️ ffmpeg 的 `-single_file 1 -global_sidx 1` **不会**输出 `<SegmentBase indexRange>`，它输出
**`<BaseURL>` + `<SegmentList>` + 每段 `<SegmentURL mediaRange="a-b"/>`**（§七 实测）。所以脚本还要：

1. **重写 MPD**（→ `output-segmentbase.mpd`）：把段定位元素整段替换为
   ```xml
   <SegmentBase indexRange="{sidxStart}-{sidxEnd}" timescale="{sidx timescale}">
     <Initialization range="0-{sidxStart-1}" />
   </SegmentBase>
   ```
   保留 `<BaseURL>` 与 AdaptationSet/Representation 的其它属性（`codecs` 仍由 `fill_mpd_codecs()` 补）。
   ⚠️ ffmpeg 原本的 `<Initialization range="0-907"/>` **把 sidx（820-907）包在里面**，重写时必须只到
   `sidxStart-1` —— 既符合 DASH 语义，也与内核"只有 indexRange 时合成 `[0, indexRangeStart-1]`"一致
   （`MPDParser.cpp:330-336`）。
2. **单文件改名（用户口径：不要序号）**：ffmpeg 模板只有 `$RepresentationID$` 可用，所以先出临时名
   `sb-<id>.m4s`，再按**类型-编码-分辨率**重命名，并把新名写回 MPD 的 `<BaseURL>` 与 JSON 的
   `baseUrl` / `segments[].url`：
   - 视频：`sb-video-<编码短名>-<高度>p.m4s`（如 `sb-video-h264-1080p.m4s`；同编码同高度重复时追加
     `-<kbps>k` 消歧）
   - 音频：`sb-audio-<编码短名>.m4s`（如 `sb-audio-aac.m4s`；同编码多路时追加采样率消歧）
   - 临时名必须从输出目录消失；目标名已存在就报错退出（不静默覆盖）。
3. **校验**：脚本改动后必须跑 `verify_segmentbase.py`（§五）。

**备用方案（若某台机器上 `-i output.mpd -c copy` 这条再封装路径不成立）**：给每条流加
`split=2` 并在两个输出上各重复一遍全部每输出选项（合法但编码器翻倍，注意 NVENC 会话上限）；
或"逐路 concat 已编码分片 + 再封装"。脚本实现里保留了明确报错，不会产出半成品清单。

### 4.3 SegmentBase JSON 设计（**字段沿用现有，不新造**）

字段全部来自现有 schema（`MediaManifest.h` 的 `SegmentInfo` / `Segment`）：

```json
{
  "video": [{
    "id": 0,
    "baseUrl": "http://127.0.0.1:9000/video/dash2/sb-video-0.m4s",
    "backupUrls": ["…", "…"],
    "bandwidth": 3000000,
    "codecs": "avc1.64001f",
    "width": 1920, "height": 1080, "frameRate": "30000/1001",
    "segmentInfo": {
      "mode": "single",
      "timescale": 90000,
      "initialization": "0-1479",
      "indexRange": "1480-2048",
      "segments": [
        { "duration": 6.006, "url": "sb-video-0.m4s", "byteRange": "2049-560000" },
        { "duration": 6.006, "url": "sb-video-0.m4s", "byteRange": "560001-1110000" }
      ]
    }
  }]
}
```

- `mode: "single"` + `indexRange` = 让内核**用 sidx 自己推段**（本次要实现的路径）；
- 同时给出 `segments[]`（每条 `byteRange` + `duration`）= 兼容"只认显式列表"的实现，
  两份数据由脚本从同一个 sidx 解析出来，保证一致；
- `initialization` 为**字节范围**（single 模式语义，见 `MediaManifest.h:93`），不是 URL；
- `baseUrl`/`backupUrls`/`bandwidth`/`codecs`/`frameRate` 等与现有 JSON 完全同名字段。

### 4.4 脚本结构改动

- `transcode_all.py`
  - `build_ffmpeg_cmd()`：追加输出 B 与它的参数；
  - 新增 `parse_segmentbase_mpd()`：解析 `output-segmentbase.mpd` 取
    `indexRange` / `Initialization@range` / `BaseURL`（复用现有 `parse_mpd_manifest()` 的 XML 容错处理）；
  - 新增 `parse_m4s_sidx()`：直接读那个单 m4s 的 `sidx` box（版本/第一条 offset/timescale/reference
    列表：`referenced_size` + `subsegment_duration`），作为 `indexRange`/`byteRange` 的**权威来源**
    （MPD 只用来交叉校验）；参考内核 `dash/SidxParser.cpp:109-166` 的字段顺序；
  - 新增 `generate_segmentbase_json()`：产出 `test-dash-{version}-segmentbase.json`；
  - 既有校验风格延续：断言 sidx 的 reference 数与分段数一致、每段 `byteRange` 连续无重叠、
    `initialization` 与 `indexRange` 不重叠。
- `convert-to-manifest.py`（权威 JSON 实现）
  - `parse_mpd()` 增加对 `SegmentBase` 的识别（当前只认 SegmentTemplate/SegmentList）；
  - `build_segment_info()` 增加 `is_single` 分支，输出上述 `mode:"single"` 结构；
  - `main()` 增加第 5 个产物 `test-dash-{version}-segmentbase.json`。
- 两个脚本都保持"无第三方依赖"（只用标准库 + ffmpeg/ffprobe），并保证 `python -m py_compile` 通过。

---

## 五、验证方案

| 项 | 手段 | 现状 |
|---|---|---|
| 内核改动编译 | 鸿蒙 `assembleHar` + `assembleHap`（零错误零告警）；桌面 MSVC 侧同源编译 | 待本轮执行 |
| JSON 契约 | 用改造后的脚本产出的 SegmentBase JSON 走 `MediaManifestParser` 解析，逐字段比对 | 待本轮执行 |
| SegmentBase MPD 解析 | 断言每个 Representation 有 `indexRange` + `Initialization@range`，且单文件确有 sidx | 待本轮执行 |
| 脚本自检 | `python -m py_compile` 两个脚本；有输入视频则真跑一遍并核对产物 | 待本轮执行 |
| 产物结构 | `D:\hilihili\转码脚本\verify_segmentbase.py`（新写的验收脚本，只用标准库）：断言 MPD 有
`BaseURL`/`SegmentBase@indexRange`/`Initialization@range`、init 与 sidx 紧邻不重叠、文件里真有 sidx 且
范围等于 `indexRange`、段范围连续无缝、JSON 的 `mode/initialization/indexRange/segments[]` 与 sidx **同源一致**、
没有带序号的临时名残留；任一不符退出码 1 | 待本轮执行 |
| 端到端播放 | 桌面/真机：`output-segmentbase.mpd` 与 `test-dash-*-segmentbase.json` 两条清单各播一遍（起播、seek、切档） | **需要用户侧设备/桌面实测**（本机无鸿蒙设备） |

---

## 六、风险与边界（如实记录）

1. **ffmpeg 的 `-single_file` 与 `-use_timeline 0` 组合**：SegmentBase 版的 MPD 只有 `indexRange`，
   时长信息来自 sidx 而非 SegmentTimeline——这正是 SegmentBase 的定义，但**必须**确保
   `-global_sidx 1` 生效，否则单文件里没有 sidx，播放器拿不到段表（脚本里会断言 sidx 存在，
   缺了就报错退出，不产出"看着像对、实际播不了"的清单）。
2. **HLS 不受影响**：新增输出只在 DASH 那份，HLS 逻辑一行不动。
3. **JSON 的 `mode:"single"` 需要内核本次改动**才生效；在改动落地前，脚本产出的该 JSON
   属于"声明了但播不了"状态（因此本次内核实现与脚本改造必须一起交付）。
4. **`initialization` 语义**：single 模式下它是字节范围而不是 URL（`MediaManifest.h:93`）；
   生成脚本与内核必须一致，否则会当成 URL 去请求。

---

## 七、实测记录（本机 ffmpeg 8.1 + lavfi 测试源，2026-09-28）

**测试条件**：`ffmpeg -f lavfi -i testsrc2=size=640x360:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000
-t 20 -c:v libx264 -g 50 -keyint_min 50 -sc_threshold 0 -pix_fmt yuv420p -c:a aac -b:a 128k
-f dash -seg_duration 6 -use_timeline 0 -use_template 1 -single_file 1
-single_file_name 'sb-$RepresentationID$.m4s' -global_sidx 1 out-sb.mpd`

**产物**：`sb-0.m4s`（视频 1.93 MB）、`sb-1.m4s`（音频 318 KB）、`out-sb.mpd`；MPD 用的是
**`<SegmentList>` + `<SegmentURL mediaRange=…>`**（不是 SegmentBase）—— 见 §4.2 的更正。

**独立交叉校验**（用 Python 重新实现内核那套偏移算法，与 ffmpeg 自己写的 `mediaRange` 逐条比对）：

| Representation | sidx box | version | timescale | refs | sidx 推出的范围 vs MPD mediaRange |
|---|---|---|---|---|---|
| `sb-0.m4s`（视频） | `820-907` | 1 | 12800 | 4 | `908-588221 / 588222-1184188 / 1184189-1768522 / 1768523-1977452` —— **全部一致**，连续无缝 |
| `sb-1.m4s`（音频） | `769-856` | 1 | 48000 | 4 | `857-96312 / 96313-193686 / 193687-291090 / 291091-325427` —— **全部一致**，连续无缝 |

结论：
1. **偏移基准正确**：`offset = sidx.first_offset + sidxEnd + 1` 后按 `referenced_size` 累加，
   与 ffmpeg 的段划分完全吻合 ⇒ 内核 `ManifestDemuxer::expandSegmentBase` 与
   `DashSegmentTracker::parseIndex` 用的这套算法对 ffmpeg 产物成立。
2. **sidx 是 version=1（64 位字段）**：内核 `SidxParser.cpp:129-135` 按版本取宽（0 → 4 字节，
   1 → 8 字节），与本机产物匹配 —— 这条若不支持，SegmentBase 会整体错位。
3. ffmpeg 的 `Initialization range` **包含 sidx**（视频 `0-907` 覆盖 `820-907`）⇒ 重写 MPD 时
   Initialization 必须只到 `sidxStart-1`（视频 `0-819`、音频 `0-768`）。
4. 因此 MPD 的 `indexRange` 直接取 **sidx box 自身的字节范围**（视频 `820-907`、音频 `769-856`），
   `timescale` 取 sidx 的 timescale（视频 12800、音频 48000 —— 注意与 ffmpeg 在
   `<SegmentList@timescale>` 里写的 `1000000` **不同**，sidx 才是分片时长的权威）。

## 八、内核改动（本轮已完成）

`framework/demuxer/manifest/ManifestDemuxer.cpp`：

1. 新增 `expandSegmentBase(fileUrl, segInfo)`：`segments[]` 显式列表优先；否则按 `indexRange`
   用 `dataSourcePrototype` 把 sidx 按范围拉下来、`Dash::SidxParser` 解析、按 §七 验证过的偏移基准推段。
2. 视频与音频**两处** representation 装配都补了 `SegmentMode::Single` 分支（音频那份是独立拷贝，
   只改一处会漏）。
3. `init` 段：single 模式下 `initialization` 按字节范围处理（`setSourceUrl(baseUrl)` + `setByteRange`），
   写成 URL 的老写法仍然兼容；`setBaseUrl` 不再对 Single 跳过。
4. 一段也拿不到时（无 `segments[]`、`indexRange` 非法、拉不到或解析不出 sidx）**打明确日志并跳过该
   representation**，不再留下"清单能开、永远没数据"的空段表。
5. 编译：鸿蒙 native 目标链接通过，**0 error / 0 代码告警**（见提交记录）。

## 九、脚本改动落地情况（本轮）

两个脚本已改写（`D:\hilihili\转码脚本\`，**该目录不在 git 仓库里**，只随文件系统交付）：

| 文件 | 规模 | 新增/改动 |
|---|---|---|
| `transcode_all.py` | 49.1 KB → 86.4 KB | 常量 `SEGMENTBASE_MPD_NAME` / `SB_TMP_TEMPLATE` / `SB_REPS` / `SB_META`；`build_segmentbase_cmd()` + `run_segmentbase_remux()`（§4.2 的再封装阶段）；`parse_segmentbase_mpd()`（**三种段定位都认**：SegmentBase / SegmentList(mediaRange) / SegmentTemplate）；`parse_m4s_sidx()` + `sb_segment_ranges()`；`sb_single_file_name()` + `build_sb_rename_map()`（唯一命名口径，无开关）；`rewrite_segmentbase_mpd()`（重写成真 SegmentBase + 换 BaseURL + 补 bandwidth）；`prepare_segmentbase_artifacts()`（解析→sidx→交叉校验→命名冲突检查→**先在内存改写 MPD 并补 codecs**→才重命名文件→写回）；`generate_segmentbase_json()`（第 5 个 JSON）；`write_json_manifests` 内置兜底也产出第 5 个；`main()` 串起"主转码 → 再封装 → 重命名/改写 → 关键帧校验"，失败非零退出 |
| `convert-to-manifest.py` | 23.4 KB → 40.3 KB | `parse_mpd()` 增加 SegmentBase/SegmentList/SegmentTemplate 三种识别与 `index_range`/`init_range`/`media_ranges`/`segment_base_timescale`/`is_audio`；独立的 `parse_m4s_sidx()` / `build_sidx_segments()` / `join_url()`（标准库）；`build_segmentbase_manifest()`（清单里没有 `<SegmentBase>` 就报错，避免改写失败时产出错清单）；`build_segment_info(..., single=...)` 新增 single 分支；`main()` 多写 `test-dash-{version}-segmentbase.json`（文件缺失只警告跳过，存在但 sidx/校验失败则非零退出） |
| `verify_segmentbase.py` | 新增 11.7 KB | 验收工具（§五）：MPD/JSON/单文件 sidx 三方交叉校验 + 临时名残留检查 |

**已完成的验证**：
- 三个脚本 `python -m py_compile` **全部通过**（Python 3.10.9）；
- 内核改动后的**全量**鸿蒙构建：`hvigorw assembleHar`（module=cicadaplayer@default）与
  `hvigorw assembleHap` 均 **BUILD SUCCESSFUL，0 error / 0 warning**；两个 ABI 的
  `libcicadaplayer.so` 都重新链接（arm64-v8a 与 x86_64 时间戳一致），`libdemuxer.a` 两个 ABI 各自重编，
  且两份 `.so` 里都能查到新代码的日志串（`segmentBase: %zu segments from sidx of`、
  `segmentBase: no sidx reference in`）⇒ 改动确实进了两个 ABI，不是只有本机 arm64 探针。

**尚未完成**：改动后的脚本**真跑一次**（我试图在临时工作目录跑全流程时，用户拒绝了该命令的提权，
故未执行；按规则不重试、不绕道；用户已选择**自行在本机跑**并把 `verify_segmentbase.py` 的输出反馈）
→ 因此 §五 表里"脚本自检 / 产物结构"两行仍是**待执行**，
`output-segmentbase.mpd` 与 `test-dash-*-segmentbase.json` 的**真实产物形状尚未经机器核对**。

**风险点（脚本作者自述，按可能性排序）**：① `-i output.mpd -c copy … -single_file 1 -global_sidx 1`
这条再封装路径未在真机验证（最可能是 dash demuxer→dashenc 复制的 DTS 报错，或再封装清单
`bandwidth="0"` —— 已用主清单补齐）；② `-use_template 1 + -single_file 1` 若写的是
`<SegmentTemplate>` 而非实测的 SegmentList，改写与 sidx 仍成立、只把 MPD 交叉校验降级为"以 sidx 为准 + 警告"；
③ `frameRate` 在两个脚本间可能不一致（既有实现本来就有 `round(x,2)` 与 `round(x,6)` 的差异）。
