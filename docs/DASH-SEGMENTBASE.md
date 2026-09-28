# DASH SegmentBase（单文件 + sidx + 字节范围）支持矩阵与改造方案

本文回答两个问题：**播放器现在支持 DASH SegmentBase 吗**、**转码脚本要怎么改成一次同时产出
分段式与 SegmentBase 式清单（含 SegmentBase JSON）**。所有结论都给了代码位置，便于逐条核对。

---

## 一、结论速览

| 清单类型 | SegmentBase 单文件模式 | 结论 | 依据 |
|---|---|---|---|
| **DASH MPD**（`<SegmentBase indexRange=...><Initialization range=.../>`） | 支持，端到端可用 | ✅ 已支持 | 见 §二 证据链 |
| **MediaManifest JSON**（`mode:"single"` + `indexRange`） | 解析了，但**建不出任何媒体段** | ❌ **本次要修** | 见 §三 |
| MediaManifest JSON（显式列表 + 每段 `byteRange`） | 支持（不依赖 sidx） | ✅ 已支持 | `ManifestDemuxer.cpp:279-284` |

也就是说：**MPD 侧的 SegmentBase 不用改**；**JSON 侧的 SegmentBase 是缺口**，本次补齐。

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

### 4.2 ffmpeg 侧做法（编码一次、两个 dash 输出）

在同一次 ffmpeg 调用里给**两个输出**，各自 `-f dash`，互不影响：

```text
# 输出 A（保持现状）
-f dash -seg_duration 6 -use_timeline 1 -use_template 1 ... output.mpd

# 输出 B（新增，SegmentBase）
-f dash -single_file 1 -single_file_name "sb-$RepresentationID$.m4s" -global_sidx 1 \
       -use_timeline 0 -use_template 1 -seg_duration 6 ... output-segmentbase.mpd
```

要点：
- `-single_file 1` + `-global_sidx 1` 才会写出"单文件 + 全局 sidx"，MPD 才会带 `indexRange`；
  `-global_sidx` 官方限制是"single file + mp4 + 非流式"，正好符合 VOD 场景。
- `-single_file_name` 用模板名（`$RepresentationID$`），保证音视频各自一个文件、名字可预测。
- 分组（AdaptationSet）与 codecs 补全沿用现有逻辑：`fill_mpd_codecs()` 要对**两个** MPD 都跑一遍
  （SegmentBase 版同样没有 `codecs` 声明）。
- 段边界/IDR 约束不变（`SEGMENT_DURATION=6`、`IDR_INTERVAL=2`），SegmentBase 的 sidx 分片同样按 6s。

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
