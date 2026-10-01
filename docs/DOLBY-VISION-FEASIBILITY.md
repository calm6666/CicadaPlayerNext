# Dolby Vision（DV）支持可行性研究 —— CicadaPlayerNext

> 目标：回答用户那条需求 —— "设计/找一个开源的 Dolby Vision C/C++ 库；有的话把它编译到所有平台；
> 通过 FFmpeg 实现 DV 支持；检测各平台是否支持，不支持自动回退，支持就直接播；
> 并在播放器 UI 的清晰度列表里显示 Dolby Vision 徽标。"
>
> 本文只做**研究与设计**，不改任何代码（唯一产出就是本文件）。
>
> 所有关于本仓库的论断都标 `文件:行号`，并区分：
> * **【已证实】** —— 本次真的读过该文件该行（或真的抓取到该网页）；
> * **【推测/待验证】** —— 从上下文推断、或取决于外部版本/环境，施工前必须先验证。
>
> 所有关于外部项目的论断都附**本次真的抓取过**的 URL。抓不到的一律写明"未抓取"，
> 并在 §7.1 汇总成"待验证清单"。
>
> ⚠ **本会话有一条硬约束，必须先声明**（它直接决定了 §2 的呈现方式）：
> **shell 在本会话不可用**。任何 `pwsh` 调用都在沙箱初始化阶段就失败，命令根本没执行，
> 原始错误逐字如下：
>
> ```
> SetNamedSecurityInfoW failed (Win32 5): grantWrite(D:\hilihili)
> ```
>
> 这不是"策略拒绝了某条命令"（那种会返回 `[sandbox: file access denied ...]`），
> 而是沙箱**给会话工作区授写权限这一步就失败**（Win32 5 = ACCESS_DENIED），
> 带 `workdir` 换目录、换后台任务、以及委托子代理重试，全部是同一条错误。
> 本会话审批提示被禁用、子代理权限不可从内部放宽，因此**无法**通过提权绕过。
> 后果：**用户要求的 `ffmpeg -decoders / -h decoder=hevc / -h full / -filters` 这几条命令
> 本次一条都没能跑**。§2 用**构建期证据 + 源码证据**替代（这些是二进制能力的直接来源，
> 且比 `-h` 输出更精确），并把该跑的命令原样列出来供你在 shell 正常的会话里补跑。

---

## 目录

1. [结论速览](#0-结论速览tldr)
2. [Dolby Vision 技术要点](#1-dolby-vision-技术要点)
3. [本机 FFmpeg 实测（最重要的一节）](#2-本机-ffmpeg-实测最重要的一节)
4. [开源库调研：有没有、能不能用、能不能编](#3-开源库调研有没有能不能用能不能编)
5. [跨平台可用性矩阵](#4-跨平台可用性矩阵)
6. [针对本播放器的设计](#5-针对本播放器的设计)
7. [诚实的底线](#6-诚实的底线)
8. [附录：证据索引与待验证清单](#7-附录证据索引与待验证清单)

---

## 0. 结论速览（TL;DR）

**一句话：可以在"检测 + 徽标 + 自动回退播基底层 + 暴露 RPU 元数据"这四件事上做出真实可用的功能；
但"真正的 Dolby Vision 输出"在本工程当前的技术栈下做不到，软件路线也做不到完整正确。**

拆成三块：

| 能力 | 能否 | 依据 |
|---|---|---|
| **检测**片源是不是 DV、是哪个 profile | **能，而且很便宜** | FFmpeg 9.0 的 mov/matroska 解复用器已经把 `dvcC`/`dvvC`/`dvwC` 解析成 `AV_PKT_DATA_DOVI_CONF` 流侧数据（§2.4），解码器解析 RPU 后把 `AV_FRAME_DATA_DOVI_METADATA` 挂在帧上（§2.4） |
| **UI 徽标** | **能，而且有现成模式可抄** | 本仓库刚做过"编码徽标"（`platform/QtPlayer/controls/RightControls.qml:2142` 的 `codecBadge`），DV 徽标照抄这一套即可（§5.5） |
| **自动回退** | **能，但回退目标随 profile 而不同** | profile 8.x 的基底层就是标准 HDR10，可照常播；profile 5 的基底层**不是**可用的 HDR10/SDR 画面（§1.2） |
| **真正的 DV 输出**（把 RPU 交给显示管线、由显示器做 DV 呈现） | **做不到（当前栈）** | 需要"授权硬件 + OS DV 通道 + 许可"，且本工程三条桌面/移动渲染路径都没有 DV 元数据通道（§4、§5.7） |
| **软件 DV 处理**（自己按 RPU 做 reshape/色调映射，输出正确画面） | **理论可行，实践不可行** | 唯一现实候选是 libplacebo（LGPL，确实实现了 DV，见 §3.4），但它**不在**本工程自带的 FFmpeg 构建里（`external/build/ffmpeg/win32/x86_64/ffbuild/config.mak:743` `!CONFIG_LIBPLACEBO=yes`），要引它等于把 FFmpeg 重编一遍并新增一个重 GPU 依赖（§5.6） |
| **开源 C/C++ 库"完整处理 DV RPU 并输出正确画面"** | **不存在** | 见 §3.6 的裁定 |

---

## 1. Dolby Vision 技术要点

### 1.1 需要关心的三个 profile

| profile | 分层 | 基底层是什么 | 通用解码器"当普通视频播"的结果 |
|---|---|---|---|
| **5** | 单层（BL 即全部） | HEVC，但色彩是 **IPT（ICtCp 家族）** + 专有 reshaping | ❌ **画面不可用**（颜色错乱：偏绿/偏紫，且动态范围错）。profile 5 **没有任何 HDR10 兼容声明**，这是它和 8.x 最本质的区别 |
| **8.1** | 单层（BL + RPU） | HEVC **标准 HDR10**（PQ / BT.2020） | ✅ **可当 HDR10 播**，画面正常，只是没有 DV 的动态元数据增强 |
| **8.4** | 单层（BL + RPU） | HEVC **标准 HLG**（BT.2020 / ARIB STD-B67） | ✅ 可当 HLG 播 |
| **7**（MEL / FEL） | **双层**：BL + EL（增强层）+ RPU | HEVC HDR10（BL 本身是合规 HDR10） | ⚠ 可当 HDR10 播（丢增强层与 RPU）；FEL 的附加精度/细节会丢，但**不会**像 profile 5 那样颜色错乱 |

**"profile 7 的 EL 藏在哪"这件事有一手文档可引**（本次已抓取）：

> "Profile 7 carries the enhancement-layer HEVC bitstream interleaved inside the base-layer access units,
> wrapped in user-unspecified NAL units of type 63 (UNSPEC63), and the RPU metadata as a sibling
> user-unspecified NAL of type 62 (UNSPEC62)."
> —— <https://ffmpeg.org/ffmpeg-bitstream-filters.html>（`dovi_split` 一节）【已证实】

这一句同时把 **RPU 在 HEVC 里的承载方式（UNSPEC62 = NAL type 62）** 和 **profile 7 双层结构**都钉死了。

### 1.2 profile 5 为什么"不能当普通视频播"——这是整份报告的题眼

**【已证实·来源：libplacebo 头文件】** `pl_dovi_metadata` 的结构本身就说明了问题：
它带的是 **reshape 曲线（`num_pivots` / `poly_coeffs` / `mmr_coeffs`）**、**`nonlinear` / `linear` 矩阵**
（注释明确写着 `nonlinear` = "ycc_to_rgb"、`linear` = "rgb_to_lms"），以及
**NLQ 参数（`nlq_active`，注释：`Used only when an enhancement layer is used. Ignored for MEL and non-FEL
profile 7 streams. When nlq_active is false ... only BL-only reshape is performed.`）**。
来源：<https://code.videolan.org/videolan/libplacebo/-/raw/master/src/include/libplacebo/colorspace.h>【已证实】

同一文件里另有：
* `PL_COLOR_SYSTEM_DOLBYVISION, // Dolby Vision (see pl_dovi_metadata)`【已证实】
* `pl_ipt_rgb2lms` / `pl_ipt_lms2rgb`，注释：`Primary-dependent RGB->LMS matrix for the IPTPQc4 color system.
  This is derived from the HPE XYZ->LMS matrix with 4% crosstalk added.`【已证实】

**结论（证据强度：高，但一手规范未抓）**：DV 的画面正确性依赖"按 RPU 做 reshape + IPT 反变换"这一步；
通用解码器只做 YCbCr→RGB，**没有**这一步。对 profile 8.1 无所谓（基底层本来就是 HDR10，少拿的是"增强"）；
对 profile 5 是致命的（基底层根本不是 YCbCr HDR10，**没有可用的解释方式**）。

> 【推测/待验证】profile 5 基底层"偏绿/偏紫"的具体观感、以及 ICC/播放器业界对它的通用描述，
> 本次**没有抓到 Dolby 官方规范或一手资料**（网络受限，见 §7.1）。上面这段是从
> FFmpeg/libplacebo 的实现反推的，机制层面可信，但**引用时必须标明这是实现反推**。
>
> 旁证（已抓取）：FFmpeg 自己的文档在描述 `libplacebo` 滤镜的 `apply_dolbyvision` 时说
> "Note that Dolby Vision will always output BT.2020+PQ, overriding the usual input frame metadata."
> —— <https://ffmpeg.org/ffmpeg-filters.html>（本会话抓的是该文档的本地同版本副本
> `D:\ffmpeg-8.1-full_build\doc\ffmpeg-filters.html:20849-20854`，与线上同源）【已证实】

### 1.3 RPU 元数据的三条承载路径（本工程都会遇到）

| 承载 | 形态 | 本仓库可验证的证据 |
|---|---|---|
| **容器层（MP4/MOV）** | `dvcC` / `dvvC` / `dvwC` box，内含 **`DOVIDecoderConfigurationRecord`** | 【已证实】FFmpeg 源码：`external/external/ffmpeg/libavformat/mov.c:8831`（`mov_read_dvcc_dvvc`）、`:9845-9847`（注册 `dvcC`/`dvvC`/`dvwC`）、`:8848` → `dovi_isom.c:32 ff_isom_parse_dvcc_dvvc`；日志串在 `dovi_isom.c:76` |
| **容器层（Matroska）** | 同样的 config record | 【已证实】`libavformat/matroskadec.c:2507`（`mkv_parse_dvcc_dvvc`）、`:3351`（`matroska_parse_dovi_streams`） |
| **码流层（HEVC）** | **NAL type 62（UNSPEC62）** 里的 RPU | 【已证实】ffmpeg.org 的 `dovi_split` 文档（上文引文）；FFmpeg 源码 `libavcodec/hevc/hevcdec.c:3753` 调 `ff_dovi_rpu_parse()` |
| **码流层（AV1）** | ITU-T T.35 metadata OBU | 【已证实】`libavcodec/itut35.c:227` 调 `ff_dovi_rpu_parse()`；`libavcodec/av1dec.h:105` 持有 `DOVIContext dovi` |
| **清单层（DASH/HLS）** | `codecs="dvhe.08.06"` / `"dvh1.05.06"` 这类 RFC 6381 token | 【已证实·本仓库】本工程的短名归一化函数**明确把 Dolby Vision 归到"认不出来"**：`framework/utils/AFMediaType.h:479`「其余（含 VP8、**Dolby Vision**、以及各种音频编码）一律返回空串」 |

**最后一格很关键**：它意味着**今天**本工程遇到 `dvhe.08.06` 这条流时，
`StreamInfo::videoCodec` 是空串、清晰度列表上**没有任何徽标**、ABR 的编码效率序也拿它没辙
（`mediaPlayer/SMPMessageControllerListener.cpp:543-544` 写入、
`platform/QtPlayer/src/CicadaPlayerItem.cpp:502-504` 读出）。DV 信息**在链路里被丢掉了**，
这是 §5.1 要接的第一个点。

### 1.4 `DOVIDecoderConfigurationRecord` 的字段（设计要用的就这几个）

【已证实】`external/external/ffmpeg/libavutil/dovi_meta.h:55-65`：

```
dv_version_major, dv_version_minor
dv_profile                    // 5 / 7 / 8 ...
dv_level
rpu_present_flag
el_present_flag
bl_present_flag               // profile 7 的 EL 轨这里是 0（见 mov.c:11197 的判据）
dv_bl_signal_compatibility_id // ★ 基底层能不能当 HDR10 播，就是看它
dv_md_compression             // NONE / LIMITED / RESERVED / EXTENDED（:67-72）
```

> 【推测/待验证】`dv_bl_signal_compatibility_id` 的取值含义（业界习惯：`1` = HDR10 兼容、
> `2` = SDR 兼容、`4` = HLG 兼容；profile 5 为 `0` = 无兼容）本次**未抓到一手规范**。
> 施工前必须用样片实测（§7.1 给了最小验证命令）。

---

## 2. 本机 FFmpeg 实测（最重要的一节）

### 2.1 先交代：哪些命令没跑成

用户要求的命令，**本次一条都没执行**（原因见文首：沙箱初始化失败，命令从未启动）：

```
ffmpeg -hide_banner -decoders | findstr /i "dovi hevc vp9 av1"
ffmpeg -hide_banner -h decoder=hevc | findstr /i "dovi dolby"
ffmpeg -hide_banner -h full | findstr /i "dovi dovi_config dolby"
ffmpeg -hide_banner -filters | findstr /i "libplacebo tonemap"
ffmpeg -hide_banner -version
```

**替代证据的可靠性说明**：`-version` 打印的 configure 行、`-decoders`/`-filters` 打印的组件列表，
在 FFmpeg 里就是构建期生成的 `ffbuild/config.mak`（configure 的产物）与 `config.h` 的投影。
我直接读了**这两个二进制对应的 `config.mak`**（`external/build/ffmpeg/<target>/ffbuild/config.mak`
就是 `make install` 留在构建目录里的同一份 configure 记录），以及
系统那套 gyan.dev 构建**自带的 `README.txt`**（它把 configure 摘要原样印出来了）。
**证据强度：等于 `-version` 的 configure 行 + 等于 `-decoders`/`-filters` 的组件清单**，
但不等于 `-h decoder=hevc` 的选项表 —— 后者我用**源码里的 AVOption 表**替代（见 2.3）。

### 2.2 原始证据（逐条，trimmed to the relevant lines）

**(A) 本工程自带的 FFmpeg 是 9.0** 【已证实】`external/external/ffmpeg/RELEASE:1`：

```
9.0
```

**(B) 自带 Windows x64 构建的 configure 行** 【已证实】
`external/build/ffmpeg/win32/x86_64/ffbuild/config.mak:4`（工具显示截断到 2000 字符，此处照抄可见部分）：

```
FFMPEG_CONFIGURATION=--disable-everything --disable-programs --disable-doc --disable-filters --disable-avdevice --disable-hwaccels --disable-bzlib --disable-videotoolbox --enable-pic --disable-debug --enable-openssl --enable-decoder=aac ... --enable-decoder=h264 --enable-decoder=hevc ... --enable-decoder=vp8 --enable-decoder=vp9 --enable-decoder=av1 ... --enable-demuxer=mov ... --e...
```

要点（都在这一行里）：`--disable-everything`、`--disable-filters`、`--disable-hwaccels`、
`--disable-videotoolbox`、`--enable-decoder=hevc/vp9/av1`、`--enable-openssl`。
Android（`.../Android/arm64-v8a/.../config.mak:4`）与 OHOS 的同名行结构一致（另有 `--enable-libxml2`）【已证实】。

**(C) 自带构建的关键开关** 【已证实】
`external/build/ffmpeg/win32/x86_64/ffbuild/config.mak`：

```
743: !CONFIG_LIBPLACEBO=yes          ← libplacebo 没编
783: !CONFIG_MEDIACODEC=yes          ← FFmpeg 自带的 MediaCodec 壳没编（本工程走自己的 JNI 解码器）
822: CONFIG_D3D11VA=yes              ← D3D11VA 硬解在（与 --disable-hwaccels 并存，说明后面另有 --enable 覆盖）
823: !CONFIG_D3D12VA=yes
824: !CONFIG_DXVA2=yes
831: !CONFIG_VIDEOTOOLBOX=yes
832: !CONFIG_VULKAN=yes
849: CONFIG_AVFILTER=yes
880: CONFIG_BSFS=yes
883: CONFIG_HWACCELS=yes
922: CONFIG_DOVI_RPUDEC=yes          ← ★ 解码侧 DOVI RPU 解析：在
923: !CONFIG_DOVI_RPUENC=yes
1019: !CONFIG_DOVI_RPU_BSF=yes        ← dovi_rpu 这个 bsf：没编
1020: !CONFIG_DOVI_SPLIT_BSF=yes      ← dovi_split 这个 bsf：没编
1152: CONFIG_HEVC_DECODER=yes
1321: CONFIG_VP9_DECODER=yes
1633: CONFIG_AV1_DECODER=yes
2432: !CONFIG_LIBPLACEBO_FILTER=yes
```

`--disable-filters` 的实际效果也能看到：`config.mak:2110-2341` 区间的滤镜全是 `!CONFIG_*_FILTER=yes`，
只有 `aformat`（:2136）、`aresample`（:2164）、`atempo`（:2190）、`volume`（:2248）是 `yes`【已证实】。
**即：自带 FFmpeg 里一个视频滤镜都没有，`tonemap` 和 `libplacebo` 都不存在。**

Android / OHOS 自带构建的同一组值**与 Windows 完全一致**（`:743 !CONFIG_LIBPLACEBO=yes`、
`:922 CONFIG_DOVI_RPUDEC=yes`、`:1019 !CONFIG_DOVI_RPU_BSF=yes`、`:2432 !CONFIG_LIBPLACEBO_FILTER=yes`）【已证实】。

**(D) 系统 ffmpeg 8.1 full build（gyan.dev）自带的 configure 摘要** 【已证实】
`D:\ffmpeg-8.1-full_build\README.txt`：

```
3:  Version: 8.1-full_build-www.gyan.dev
5:  License: GPL v3
7:  Source Code: https://github.com/FFmpeg/FFmpeg/commit/9047fa1b08
71: libplacebo                ← External libraries 列表里有它
82: ...（外部库清单）
128: hevc                     ← Enabled decoders 里有 hevc（另有 :165 libdav1d、:238 vp9、:201 av1）
746: libplacebo               ← Enabled filters 里有 libplacebo
754: tonemap                  ← 也有 tonemap
825: dovi_rpu                 ← Enabled bsfs 里有 dovi_rpu
828: dv_error_marker
871: libplacebo v7.360.0-11-g33b5dfa   ← 外部库版本
```

注意：该构建的 bsfs 清单（`README.txt:816-833`）里**有 `dovi_rpu`、没有 `dovi_split`**；
而本工程自带的 **9.0 源码树里 `libavcodec/bsf/dovi_split.c` 是存在的**
（`external/external/ffmpeg/libavcodec/bsf/dovi_split.c:71` 引用 `AV_PKT_DATA_DOVI_CONF`）【已证实】。
> 【推测/待验证】"`dovi_split` 是 9.0 才有的、8.1 还没有"—— 这是从上面两条事实推断的，未查到 changelog。

**(E) 解码侧 DOVI 能力的源码证据（自带 9.0 树）** 【已证实】

```
libavcodec/dovi_rpu.c                    ← RPU 公共实现（存在）
libavcodec/dovi_rpudec.c:343             int ff_dovi_rpu_parse(...)
libavcodec/dovi_rpuenc.c:644             int ff_dovi_rpu_generate(...)
libavcodec/bsf/dovi_rpu.c:53,71          parse / generate（bsf 形态）
libavcodec/hevc/hevcdec.c:3753           ff_dovi_rpu_parse(...)        ← HEVC 解码器解析 RPU
libavcodec/hevc/hevcdec.c:3132           av_frame_new_side_data_from_buf(out, AV_FRAME_DATA_DOVI_RPU_BUFFER, ...)
libavcodec/hevc/hevcdec.c:3139           ff_dovi_attach_side_data(&s->dovi_ctx, out)   ← 把元数据挂到帧上
libavcodec/hevc/hevcdec.c:3862           av_packet_get_side_data(avpkt, AV_PKT_DATA_DOVI_CONF, &sd_size)
libavcodec/hevc/hevcdec.c:4179           ff_get_coded_side_data(avctx, AV_PKT_DATA_DOVI_CONF)
libavcodec/itut35.c:227                  ff_dovi_rpu_parse(...)        ← AV1 的 T.35 路径
libavutil/frame.h:201                    AV_FRAME_DATA_DOVI_RPU_BUFFER
libavutil/frame.h:208                    AV_FRAME_DATA_DOVI_METADATA
libavcodec/packet.h:280                  AV_PKT_DATA_DOVI_CONF（注释 :273-279 指明存的是 AVDOVIDecoderConfigurationRecord）
libavformat/mov.c:11188-11190            st->codecpar->coded_side_data 上挂 AV_PKT_DATA_DOVI_CONF
libavformat/mov.c:11210                  avformat_stream_group_create(s, AV_STREAM_GROUP_PARAMS_DOLBY_VISION, NULL)
```

**(F) "解码器有没有 `dolby_vision` / `apply_dovi` 选项"—— 全树 grep 的结论** 【已证实】

对 `external/external/ffmpeg` 全树 grep `apply_dovi|"dolby_vision"|dolby_vision`，
**只有 4 处命中，全部在 `libavfilter/vf_libplacebo.c`**：

```
libavfilter/vf_libplacebo.c:192     int apply_dovi;
libavfilter/vf_libplacebo.c:969     if (s->apply_dovi && av_frame_get_side_data(ref, AV_FRAME_DATA_DOVI_METADATA)) {
libavfilter/vf_libplacebo.c:1099    .map_dovi   = s->apply_dovi,
libavfilter/vf_libplacebo.c:1662    { "apply_dolbyvision", "Apply Dolby Vision metadata", OFFSET(apply_dovi), AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, DYNAMIC },
```

**所以：**

* **HEVC 解码器没有 `dolby_vision` 选项，也没有 `apply_dovi` 选项。** DV 解析在解码器里是**无条件的**，
  由 `CONFIG_DOVI_RPUDEC` 编译期决定；应用只能**读** `AV_FRAME_DATA_DOVI_METADATA` / `AV_FRAME_DATA_DOVI_RPU_BUFFER`。
* **`apply_dovi` 不是解码器选项，是 libplacebo 滤镜的内部字段**，对外暴露成 `apply_dolbyvision`
  （**默认开**，见上面 `{.i64 = 1}`；文档见 §1.2 的引文）。**只有 libplacebo 会真正"应用"RPU。**
* 用户问题里提到的 `AVDOVIConfig`：**在 FFmpeg 9.0 全树里不存在**（grep `AVDOVIConfig|DOVIConfig` 无命中）。
  实际类型是 `AVDOVIDecoderConfigurationRecord`，承载在 `AV_PKT_DATA_DOVI_CONF` 上【已证实】。

### 2.3 直接回答用户那几个问题

| 问题 | 结论 |
|---|---|
| 自带 FFmpeg 有 `dovi` / `dolby_vision` **解码侧**能力吗？ | **有解析，没有"应用"**。`CONFIG_DOVI_RPUDEC=yes`（`config.mak:922`，三端一致）、`hevcdec.c:3753/3139` 解析并暴露 `AV_FRAME_DATA_DOVI_METADATA`；但**不存在** `dolby_vision`/`apply_dovi` 解码器选项（§2.2F） |
| 有 `dovi_rpu` / RPU 解析（`libavcodec/dovi_rpu.c`）吗？ | **有**。`libavcodec/dovi_rpu.c`、`dovi_rpudec.c`、`dovi_rpuenc.c`、`dovi_rpu.h` 都在源码树里【已证实】 |
| `dovi_rpu` / `dovi_split` 这两个 **bsf** 能用吗？ | **自带构建都不能**（`config.mak:1019-1020` 两个 `!...=yes`）。系统 8.1 构建**有 `dovi_rpu`、无 `dovi_split`**（`README.txt:825`，清单里没有 dovi_split） |
| `libplacebo` 编进去了吗（找 `--enable-libplacebo`）？ | **自带构建：没有**（`config.mak:743` `!CONFIG_LIBPLACEBO=yes`，且 `:2432` 滤镜也没有）。**系统 8.1：有**（`README.txt:71/746/871`，libplacebo v7.360.0） |
| `-vf libplacebo` / `-vf tonemap` 可用吗？ | **系统 8.1：可用**（`README.txt:746`、`:754`）。**自带构建：不可用**（`--disable-filters`，只有 aformat/aresample/atempo/volume，`config.mak:2136/2164/2190/2248`） |
| configure 行 | 见 §2.2B（自带 Windows）与 §2.2D（系统 8.1 README） |
| 版本 | 自带 **9.0**（`external/external/ffmpeg/RELEASE:1`）；系统 **8.1-full_build**（`README.txt:3`） |

### 2.4 这对本工程意味着什么（本节的三条硬结论）

1. **"通过 FFmpeg 实现 DV 支持"里，FFmpeg 能白送的只有"识别 + 解析 + 暴露元数据"。**
   自带构建已经具备（`CONFIG_DOVI_RPUDEC=yes`），**不需要重编 FFmpeg** 就能做 §5 的检测与徽标。
2. **"把 RPU 应用成正确画面"FFmpeg 自己做不到**：整个 FFmpeg 里唯一的实现是 `libplacebo` 滤镜，
   而它不在自带构建里。要用就得给三端 FFmpeg 加 libplacebo（= 重编 FFmpeg + 新增重 GPU 依赖 + 各平台
   需要 Vulkan/GL 后端），这与"decoder 不许无谓重编"的项目规则直接冲突，**必须单独论证**（§5.6）。
3. **容器的 DV 信息是免费到手的**：`mov.c:11188-11190` 把 config record 挂在
   `st->codecpar->coded_side_data` 上，本工程读流元数据的地方正是同一层（§5.1）。

---

## 3. 开源库调研：有没有、能不能用、能不能编

### 3.1 候选一览

| 候选 | 语言 | 许可 | 能做什么 | 能不能"出正确画面" |
|---|---|---|---|---|
| **libdovi**（`quietvoid/dovi_tool` 的 Rust 库 `dolby_vision`） | Rust，**且提供 C API** | **MIT** | 读写 DV 元数据（RPU 解析/生成、profile 转换） | ❌ 不做渲染 |
| **dovi_tool** | Rust CLI | MIT（同仓） | CLI 工具集 | ❌ 离线工具，不是播放库 |
| **FFmpeg libavcodec DOVI**（`dovi_rpu.c` / `dovi_meta.c`） | C | LGPL/GPL（随 FFmpeg） | **解析并暴露** RPU、bsf 改写/剥离/拆分 | ⚠ 只解析，不应用 |
| **libplacebo** | C | **LGPL-2.1-or-later** | **真正应用 DV**：reshape / IPT / 色调映射 / FEL NLQ | ✅ 最接近"能出画面"的一个 |
| **Dolby 官方 SDK**（`dlb_mp4base`、SPOTT 等） | C/C++ | **非开源，需 Dolby 授权** | 官方编解码/封装工具 | ✅ 但**不是开源**、要许可 |

### 3.2 libdovi（`dolby_vision` crate）

**【已证实】原文（自述）**：
> "Library to read & write Dolby Vision metadata. Comes as a Rust crate and C compatible library."

* 抓取来源：<https://crates.io/api/v1/crates/dolby_vision/3.4.0/readme>（内容托管于
  <https://static.crates.io/readmes/dolby_vision/dolby_vision-3.4.0.html>）
* **许可：MIT** 【已证实】—— 抓取 <https://crates.io/api/v1/crates/dolby_vision>，
  各版本 `"license":"MIT"`；最新 **3.4.0**（2026-08-12），仓库
  `https://github.com/quietvoid/dovi_tool/tree/main/dolby_vision`。
* **C API 存在，但要过 cargo-c** 【已证实】原文：
  `libdovi comes as a C compatible library. To build and install it you can use cargo-c:`
  `cargo install cargo-c` / `cargo cinstall --release`；示例 `gcc capi_rpu_file.c -ldovi -o capi_example.o`。
* **工具链要求** 【已证实】`The minimum Rust version to use dolby_vision is 1.88.0.`

**对"本工程要 C/C++ 库"这个诉求的判定**：
**它不是纯 Rust 封闭的**（有 `capi` feature + cargo-c 产出的 C 库），**但也不是一个 C 库**：
引入它 = 给 Windows / macOS / Linux / Android / iOS / HarmonyOS **六套构建各加一条 Rust + cargo-c 工具链**，
且 iOS/OHOS 的交叉编译要额外验证。
**【推测/待验证】** cargo-c 产物在 iOS/OHOS 上的可用性、静态链接后的体积与 `crate-type`（staticlib/cdylib）
选择，本次未验证（无网络抓取条件，§7.1）。

### 3.3 dovi_tool（CLI）

* 【已证实·仅限身份】crates.io 记录里的仓库地址就是它，且搜索结果的标题自述为
  "dovi_tool is a CLI tool combining multiple utilities for working with Dolby Vision."
  （仅见搜索结果，**该页本次未抓取成功**：`github.com` 与 `raw.githubusercontent.com`
  在本会话不稳定，多次 `fetch failed`）。
* **【推测/待验证】** "能把 profile 7 转成 8.1、能抽/注入 RPU、有各平台预编译二进制" ——
  这是业界普遍描述，**本次没有抓到 README 原文，不能当已证实写入设计前提**。
  但**能力上等价的证据我抓到了一手**：FFmpeg 9.0 自带 `dovi_split` / `dovi_rpu` 两个 bsf
  （`dovi_split` 文档见 §1.1 引文，`mode=bl|bl_rpu|el|el_rpu`），
  离线把 profile 7 拆成 BL / BL+RPU 这件事**可以完全用 FFmpeg 做**，不必引入 dovi_tool。
* 定位结论：**它是离线制作工具，不是播放期库**。对播放器没有运行期价值（除非我们用它在转码侧预处理片源）。

### 3.4 FFmpeg 自带的 libavcodec DOVI 代码

* **存在于自带 9.0**（§2.2E），**API 清单**（全部【已证实】，行号见 §2.2E）：
  * `AVDOVIDecoderConfigurationRecord`（`libavutil/dovi_meta.h:55-65`）—— 容器里的 config record
  * `AVDOVIRpuDataHeader`（`:87`）、`AVDOVIDataMapping`（`:152`）、`AVDOVIDmData`（`:318`，含 L1/L2/L5/L6… 各级动态元数据）
  * `AVDOVIMetadata`（`:345`，配合 `av_dovi_get_header/mapping/color/ext`、`av_dovi_find_level`）
  * `AV_FRAME_DATA_DOVI_RPU_BUFFER`（`libavutil/frame.h:201`）、`AV_FRAME_DATA_DOVI_METADATA`（`:208`）
  * `AV_PKT_DATA_DOVI_CONF`（`libavcodec/packet.h:280`）
  * **`AVDOVIConfig` 这个类型不存在**（全树 grep 无命中）
* **能力边界（最重要的判定）**：**它只能"解析并暴露"，不能"应用"**。
  唯一"应用"的实现是 `libavfilter/vf_libplacebo.c`（§2.2F）。也就是说 FFmpeg 的
  `dovi_rpu.c` 提供的是**元数据层**，不是**渲染层**。

### 3.5 libplacebo

**【已证实】许可：LGPL-2.1-or-later** —— 抓取
<https://code.videolan.org/videolan/libplacebo/-/raw/master/LICENSE>（GNU LGPL v2.1 全文，
头部即 "Version 2.1, February 1999"）。

**【已证实】DV 能力（一手头文件）** —— 抓取
<https://code.videolan.org/videolan/libplacebo/-/raw/master/src/include/libplacebo/colorspace.h>：

* `PL_COLOR_SYSTEM_DOLBYVISION, // Dolby Vision (see pl_dovi_metadata)`
* `struct pl_dovi_metadata`：
  * `nonlinear_offset[3]`（注释：`input offset ("ycc_to_rgb_offset")`）
  * `nonlinear`（`before PQ, also called "ycc_to_rgb"`）、`linear`（`after PQ, also called "rgb_to_lms"`）
  * `comp[3].num_pivots` / `pivots[9]` / `method[8]`（`0 = polynomial, 1 = MMR`）/ `poly_coeffs[8][3]` / `mmr_order` / `mmr_constant` / `mmr_coeffs`
  * `nlq_active` + `nlq[3]`（**FEL 合成的非线性反量化**）
* 矩阵辅助：`pl_ipt_rgb2lms` / `pl_ipt_lms2rgb`（`IPTPQc4`）。

**判定：libplacebo 是唯一"真的算 DV"的开源实现，也是本节的现实路线。**
**但它不是"一个 DV 库"，而是一个 GPU 渲染/色彩管线库**：

1. 它需要 **Vulkan / OpenGL / D3D11 之类的后端**（本工程自带 FFmpeg 把 `VULKAN` 也关了，
   `config.mak:832`，见 §2.2C）；
2. 它**不负责解码**：profile 5 的 BL、profile 7 的 EL 都要调用方自己解好再喂给它；
3. 它接受的是 `pl_frame` + `pl_dovi_metadata`，而 `pl_dovi_metadata` 要从
   FFmpeg 的 `AVDOVIMetadata` 映射过来（FFmpeg 侧就是 `vf_libplacebo.c:969/1099` 干的事）。

### 3.6 Dolby 官方 SDK（`dlb_mp4base` 等）

**【未能抓取】**：`developer.dolby.com` DNS 解析失败（`getaddrinfo ENOTFOUND`），
`github.com/DolbyLaboratories/dlb_mp4base` 在本会话持续 `fetch failed`。
因此**"它不是开源、需要 Dolby 授权/许可"这一条，本次没有拿到可引用的原文**，只能标
**【推测/待验证】**，并附上本次搜索命中但**未抓取**的线索：
`https://github.com/DolbyLaboratories/dlb_mp4base`（搜索结果标题为
"Initial commit · DolbyLaboratories/dlb_mp4base"）。

**行业事实层面的说明（必须谨慎表达）**：Dolby Vision 的**播放**在硬件/OS 层面依赖 Dolby 的
授权与认证链条（授权解码器、授权显示管线、Dolby 的许可协议）。
【推测/待验证】具体条款本次无法核验（§7.1）。
**但本报告的设计不依赖这一点**：我们不需要 Dolby SDK —— 我们只需要"检测 + 回退 + 徽标"，
而这三件事 FFmpeg 已经给全了（§2.4）。

### 3.7 其它可能的候选

* **mpv**：【推测/待验证】mpv 的 DV 支持是建在 libplacebo 之上的（mpv 用 `pl_dovi_metadata`）。
  本次未抓取 mpv 文档，故不作为独立候选，只当"libplacebo 路线的存在性旁证"。
* **doviconvert / 其它第三方转换工具**：搜索命中 `https://docs.doviconvert.com/essentials/conversion`
  （**未抓取**）。属于离线转换工具，与 dovi_tool 同类，对播放期无价值。

### 3.8 本节裁定（不夸大版）

> **有没有一个开源 C/C++ 库，能完整处理 Dolby Vision RPU 并输出正确画面？**
>
> **没有。** 准确的说法是：
> * **播放基底层**（profile 8.x = 标准 HDR10、profile 7 的 BL = HDR10）**能**，
>   而且**今天就能**，代价接近零（§5）。
> * **解析并暴露 RPU 元数据** **能**，FFmpeg 自带构建已具备（`CONFIG_DOVI_RPUDEC=yes`）。
> * **把 RPU 应用成正确画面**只有 **libplacebo** 能做到（LGPL，不是 C++ 里"现成的 DV 库"，
>   而是一整套 GPU 色彩管线），而且它**不在**本工程自带的 FFmpeg 里。
> * **profile 5** 在"不引 libplacebo"的前提下，**任何平台都播不出正确画面**（§1.2）。
> * **profile 7 的 FEL**：即便引了 libplacebo，EL 的解码与合成仍要自己做；
>   libplacebo 只提供 `nlq` 那一步的数学（头文件注释明说 `Used only when an enhancement layer is used`）。

---

## 4. 跨平台可用性矩阵

> **先说清楚三条口径**：
> (a) "OS/显示栈支不支持 DV 输出" —— 这一栏**大部分是【推测/待验证】**：
> 本轮只成功抓到了 libplacebo / FFmpeg / libdovi 的文档，Apple/Windows/HarmonyOS 的官方页
> 在本会话全部 `fetch failed`（§7.1）。**不要把下表的 (a) 列当已证实事实用于对外承诺。**
> (b) "硬件解码器支持" —— 指本工程自己的解码路径（不是 FFmpeg 的 hwaccel 清单）。
> (c) "今天忽略 DV 会怎样" —— 这一栏**是【已证实】的**，因为它由本仓库代码决定。

| 平台 | (a) OS/显示栈原生 DV 输出 | (b) 硬件解码器 & 要向它请求什么 | (c) **今天**忽略 DV、只解基底层会怎样 | (d) 现实可达的最好行为 |
|---|---|---|---|---|
| **Windows**（`platform/QtPlayer`） | 【推测/待验证】需要"DV 认证的 GPU+驱动+显示器"以及 OS 侧 PlayReady/DV 管线；普通 Win32 桌面应用**没有**公开的 DV 提交接口 | 本工程走 **FFmpeg D3D11VA**（`external/build/ffmpeg/win32/x86_64/ffbuild/config.mak:822 CONFIG_D3D11VA=yes`；代码在 `framework/codec/avcodecDecoder.cpp:671-723`、能力探测在 `:1054-1057` `只查配置，不调 av_hwdevice_ctx_create()`）。**要拿到 DV 得让解码器输出带 DV 元数据的表面 —— 当前 D3D11VA 路径没有这个通道** | 正常播（8.x/7 当成 HDR10 播）；渲染侧 `platform/QtPlayer/src/CicadaTextureD3D11.cpp:87-106` 按 `color_trc` 选 DXGI 色彩空间（PQ → `YCBCR_STUDIO_G2084_TOPLEFT_P2020`），**HDR10 观感是对的**；profile 5 则颜色错乱 | **HDR10 降级播放 + DV 徽标 + （可选）显示"本机不支持 DV 呈现"** |
| **macOS**（`platform/QtPlayer` + `CicadaTextureMetal.mm`） | 【推测/待验证】Apple 的 DV 播放走 AVFoundation/VideoToolbox 的特定路径，**且依赖授权设备**；用 Metal 自绘纹理的桌面 Qt 应用**不在**这条路径上 | `config.mak` 里 `VIDEOTOOLBOX=no`（Windows 树如此；Apple 树的 `config.mak` **本次未读**，§7.1）。本工程 Apple 解码器是 `framework/codec/Apple/AppleVideoToolBox.cpp`（`mPInMeta` 在 `:172`，色彩信息在 `:794/1000`），**没有任何 DV 相关字段**（全 framework grep `dvhe|dvh1|Dolby|DOVI` 只命中一条注释：`framework/utils/AFMediaType.h:479`） | 同上：8.x/7 → 正常 HDR10；profile 5 → 错乱 | 同 Windows |
| **Linux** | 【推测/待验证】桌面 Linux 没有 DV 输出的既定通道（DV 认证链路在 PC 上本来就不通用） | 走 VAAPI（`framework/codec/avcodecDecoder.cpp:211` 注释：`same code serves D3D11VA on Windows and VAAPI on Linux`；Qt 侧 `platform/QtPlayer/src/CicadaTextureVAAPI.cpp`） | 同 Windows（无 HDR 元数据通道时按 SDR 播） | 同 Windows，且更保守 |
| **Android**（`platform/Android`） | 【推测/待验证】AOSP 有 DV 相关 MIME（搜索结果里 `source.android.com/docs/core/display/hdr` 提到 "video/dolby-vision"），但**该页本次抓取只拿回导航骨架、正文被截断**，不能作为已证实引用 | 本工程用**自己的 JNI MediaCodec 解码器**（`framework/codec/Android/mediaCodecDecoder.cpp`、`framework/codec/Android/jni/MediaCodec_Decoder.cpp`），**FFmpeg 自带的 mediacodec 壳没编**（`:783 !CONFIG_MEDIACODEC=yes`）。DV 与否取决于机型 SoC/OEM 是否授权 | **最不确定的一栏**：授权机型上 MediaCodec 可能就把 DV 解出来了（**它是否落回 HDR10、还是直接出 DV 画面，取决于 OEM，属于【推测/待验证】**）；非授权机型上按 HDR10 播 profile 8.x，profile 5 仍然错乱 | **检测机型能力 → 有 DV 就直出（若 OEM 提供通道）、没有就 HDR10 降级** |
| **iOS**（`platform/iOS`） | 【推测/待验证】Apple 生态 DV 播放最强，但要求走 AVFoundation 的受支持路径与受支持设备 | 本工程 iOS 复用 `framework/codec/Apple/AppleVideoToolBox.cpp`（同一份 L1 平台代码），**没有 DV 字段** | 同 macOS | 同 macOS（**注意**：这条与"iPhone 原生能播 DV"的直觉不同，因为**我们自己的渲染/输出路径不会请求 DV**） |
| **HarmonyOS**（`platform/HarmonyOS`，ArkTS+native） | 【推测/待验证】未抓到任何一手材料（搜索结果里有 `ost.51cto.com` 的问答，但**未抓取**） | native 解码器是 `framework/codec/OHOS/OhosAVCodecDecoder.cpp`；FFmpeg 自带构建的开关与其它端一致（`external/build/ffmpeg/OHOS/arm64-v8a/ffbuild/config.mak:922 CONFIG_DOVI_RPUDEC=yes`、`:743 !CONFIG_LIBPLACEBO=yes`） | 同 Android（取决于设备） | 同 Android，且**建议按"不支持 DV 输出"实现**（证据最少，保守） |

**三条横贯所有平台的结论**：

1. **(c) 列是"今天就能验收"的行为**：因为 DV 信息在本工程里**根本没被读出来**
   （§1.3 最后一格：`AFMediaType.h:479` 明确把 DV token 归为"认不出来"），
   所以当前行为等价于"把 DV 片源当普通 HDR10/SDR 播" —— profile 8.x 是对的，profile 5 是错的。
2. **(b) 列的关键不是"能不能硬解 HEVC"**（都能），**而是"硬解出来的东西能不能带着 DV 元数据
   一路走到显示"**。本工程三条渲染路径（D3D11 / Metal / VAAPI）都只搬像素 + `color_trc`
   （`CicadaTextureD3D11.cpp:77-105` 就是全部），**没有任何 DV 元数据通道**。
3. **(a) 列普遍是【推测/待验证】**，所以 §5 的设计**不把 (a) 当输入**：
   能力判定只依赖"我们自己能不能把 DV 画面正确送出去"，而不是"OS 声称支持 DV"。

---

## 5. 针对本播放器的设计

> 总原则（对齐项目硬规则）：
> **不新增布尔开关**（只走"带值参数"：一个 profile 字符串 / 一个枚举值）；
> **不引定时器/超时/看门狗/重试兜底**；
> **不无谓重编解码器**；
> **L1 共享核心（`mediaPlayer/`、`framework/` 非平台代码）里不许出现平台 `#ifdef`**；
> **新虚函数追加在 vtable 末尾、新成员追加在类末尾**（本仓库既有惯例，
> 见 `mediaPlayer/native_cicada_player_def.h:80-90` 的注释：`必须**追加在结构体末尾**`）。

### 5.1 检测：读什么、在哪读

**检测点 1：容器层（首选，最权威）—— `framework/utils/ffmpeg_utils.c`**

`get_stream_meta()` 在 `framework/utils/ffmpeg_utils.c:634`，
视频分支把容器信息搬进 `Stream_meta` 的那几行是 `:689-698`：

```
690:  meta->width  = pStream->codecpar->width;
693:  meta->pixel_fmt = pStream->codecpar->format;
694:  meta->color_info.color_range     = (enum AFColorRange) pStream->codecpar->color_range;
695:  meta->color_info.color_primaries = ...
698:  meta->color_info.color_trc       = ...
```

**就在这里加一段（追加在 `:698` 之后）**：从 `pStream->codecpar->coded_side_data` 里
找 `AV_PKT_DATA_DOVI_CONF`，把 `AVDOVIDecoderConfigurationRecord` 的
`dv_profile` / `dv_bl_signal_compatibility_id` / `rpu_present_flag` / `el_present_flag` /
`bl_present_flag` 抄进 `Stream_meta`。
依据【已证实】：FFmpeg 已经在同一个 `codecpar->coded_side_data` 上挂好了
（`libavformat/mov.c:11188-11190`、`libavformat/matroskadec.c:2507`），
本工程已经有读 `codecpar` 的代码，**零新依赖、零重编**。

**检测点 2：清单层（覆盖 DASH/HLS 且不依赖容器解析）—— 编码 token**

DASH 的 `@codecs` 已经进了 `Representation`（`framework/demuxer/play_list/Representation.h:88`
`std::list<std::string> codecs;`，由 `:55 addCodecs()` 写入），
DASH 流在 `framework/demuxer/dash/DashStream.cpp:1115-1127` 用它换算 `AFCodecID`。

**要做的改动只有一处**：给 `framework/utils/AFMediaType.h` 的短名体系**追加一个值承载的 DV 标识**。
注意今天它是被显式排除的（`AFMediaType.h:479`：DV 一律返回空串），
所以要么
* 在 `afCodecShortNameById`（`framework/utils/mediaTypeInternal.cpp:142-163`）里**追加** `AF_CODEC_ID_DOVI_HEVC` → **新短名**（追加在枚举末尾，见 `AFMediaType.h:83-90` 的既定做法），要么
* 让 DV **保持 `videoCodec` 空串不变**，DV 信息**只走独立字段**（§5.2）。

**推荐后者**：`videoCodec` 的语义是"编码"（H.265），DV 是"同一套 H.265 的封装 profile"。
把 DV 塞进 codec 短名会污染 ABR 的效率序（`afCodecEfficiencyRankByShortName`，
`AFMediaType.h:496`）与分组键（`CicadaPlayerItem.cpp:588-589` 的 `h1080|H.265`）。

**检测点 3（可选、最细）：码流层 RPU**
`framework/demuxer/avFormatDemuxer.cpp:405-456` 已经在逐包看 side data
（`av_packet_get_side_data(pkt, AV_PKT_DATA_ENCRYPTION_INFO, ...)`、`..._EXTRADATA`）。
若需要"这个包有没有 RPU"级别的信息，可在此处读 `AV_PKT_DATA_DOVI_CONF`（同 §2.2E 的 hevcdec 做法）。
**但这不是必须的**：容器层已经够了。

### 5.2 承载：一个"带值"字段，不是布尔

**L1 侧（框架内部）**：`Stream_meta` 追加成员（**必须追加在末尾**，
`framework/utils/AFMediaType.h:462` 是 `} Stream_meta;`）。
推荐**复用已有的 `VideoColorInfo`**（`AFMediaType.h:382-388`，`Stream_meta.color_info` 在 `:433`）：
它的语义就是"色彩相关的附加信息"，DV profile 天然属于这里。

值建议用**自描述文本**（而不是布尔/位标志），因为它同时能喂徽标：

```
// AFMediaType.h 的 VideoColorInfo 末尾追加（示意，不是最终代码）
char dvProfile[12];
// "" = 不是 DV；否则是 "dvhe.05.06" / "dvhe.08.06" / "dvhe.07.06" 这类
// 由 dv_profile + dv_bl_signal_compatibility_id 推出来的 RFC 6381 形态
```

**应用层（ABI）**：`StreamInfo` 追加字段。`mediaPlayer/native_cicada_player_def.h:58-91`
的注释已经把规则写死了（`必须追加在结构体末尾`，`videoCodec[16]` 就是这么加的）：

```
// native_cicada_player_def.h，追加在 char videoCodec[16]; 之后
char dolbyVision[16];   // "" = 非 DV；否则 "dvhe.08.06" 等。空串 = 界面不渲染徽标
```

**无布尔开关**这条规则由此满足：整条链路上新增的**只有两个带值的定长字符串**，
没有 `enableDolbyVision` 这种开关，也没有"用户可关的 DV"配置项。

**填充点**：`mediaPlayer/SMPMessageControllerListener.cpp:533-548`
（那里已经在填 `videoCodec` 与 `HDRType`）：

```
543:  snprintf(info->videoCodec, sizeof(info->videoCodec), "%s", afCodecShortName(meta->codec, nullptr));
545:  info->HDRType = VideoHDRType_SDR;
546:  if (meta->pixel_fmt == AF_PIX_FMT_YUV420P10BE || meta->pixel_fmt == AF_PIX_FMT_YUV420P10LE) {
547:      info->HDRType = VideoHDRType_HDR10;
548:  }
```

在这后面追加**一行**：把 `meta->color_info.dvProfile` 拷进 `info->dolbyVision`
（`""` 原样保留 = "没有 DV 信息"，与 `videoCodec` 的空串语义一致，
见 `mediaPlayer/native_cicada_player_def.h:83`）。

> 注意**不要**去动 `VideoHDRType`（`native_cicada_player_def.h:47-50`）去加 `VideoHDRType_DOLBY_VISION`：
> 它能表达"是 DV"但**表达不了是哪个 profile**，而 profile 恰恰是回退决策的唯一依据（§5.4）。
> 用带值字符串，一个字段同时解决"检测/徽标/回退"。

### 5.3 判定"本平台能不能真的出 DV 画面"：问什么、放哪

**原则**：只问"我们自己的输出路径能不能把 DV 正确送出去"，**不问 OS 是否声称支持 DV**
（§4 的 (a) 列证据不足）。因此这个判定天然是**平台代码**，不需要 L1 里出现 `#ifdef`。

推荐的落点（每一处都是**已有文件的已有函数**，只加一个返回带值结果的查询）：

| 平台 | 文件 | 查询什么 |
|---|---|---|
| Windows | `framework/codec/avcodecDecoder.cpp`（能力探测已在 `:1054-1057`，注释明说"只查配置，不调 `av_hwdevice_ctx_create()`"）+ 渲染侧 `platform/QtPlayer/src/CicadaTextureD3D11.cpp`（`inputColorSpaceForFrame()`，`:87-106`） | 解码器能否输出 DV 元数据（**当前不能**）；D3D11 交换链/视频处理器是否被授予 DV 输出（**当前代码里没有这个概念**） |
| macOS/iOS | `framework/codec/Apple/AppleVideoToolBox.cpp`（`mPInMeta` 在 `:172`、色彩信息在 `:794`、`:1000` 拷 `color_info`） | VideoToolbox 输出缓冲上有没有 DV 附件（当前没有读） |
| Android | `framework/codec/Android/mediaCodecDecoder.cpp`、`framework/codec/Android/jni/MediaCodec_Decoder.cpp` | MediaCodec 的 `MediaFormat` 能力位 / 实际输出格式里有没有 DV 信息（**当前没有读**） |
| HarmonyOS | `framework/codec/OHOS/OhosAVCodecDecoder.cpp` | 同上（证据最少，默认按"不支持"） |

**上报通道（重要：复用已有机制，不新增接口）**：
应用层已经有一条**带值的 JSON 能力通道** —— `CicadaGetVideoCodecSupport()` /
应用侧 `setVideoCodecSupport(json)`（`mediaPlayer/media_player_api.h:362-409`），
契约是 `{"source":...,"hwDecode":[...],"preference":[...],"preferred":"..."}`（`:369-373`）。
**做法**：在同一个 JSON 里**追加一个带值字段** `"dvRender":""`|`"true"`… —— 更符合本工程口味的写法是
`"dvOutput":["hdr10"]` / `["dolby-vision"]` 这类**值列表**（表达"本机能把 DV 送到显示哪种程度"），
而不是 `true`/`false`。
**判定结果只用于两件事**：① 徽标文案；② 是否尝试请求 DV 输出。**不用于"要不要播"。**

### 5.4 自动回退：**不能一刀切**（本节是设计的核心）

**关键洞察（用户要求必须说清的那一条）**：
回退**不是**"DV 播不了就降级成 HDR 播"这么简单 —— **回退目标随 profile 不同**，
而且对 profile 5 **根本不存在正确的回退目标**。

| 检测到的 profile | 基底层是什么 | 本平台不支持 DV 输出时该做什么 | 能不能"正确"播放 |
|---|---|---|---|
| **8.1**（`dv_bl_signal_compatibility_id` = HDR10） | 标准 HDR10 | **什么都不用做**：照常解 HEVC，HDR10 元数据（PQ/BT.2020）已随流到达；渲染侧已有处理（`CicadaTextureD3D11.cpp:93-95`） | ✅ 正确（丢的只是 DV 动态增强） |
| **8.4**（HLG） | 标准 HLG | 同上，走 HLG 分支（`CicadaTextureD3D11.cpp:97-99`） | ✅ 正确 |
| **7**（MEL/FEL） | HDR10（BL） | 照常播 BL；**丢掉 EL**（FEL 会丢细节/精度，MEL 基本无感）。**不要**去尝试解 EL（见下） | ⚠ 基本正确（FEL 有损） |
| **5** | **不是 HDR10，也不是 SDR** | **只能明确告知"本机无法正确播放"** + 尽力而为的降级（可选：用 RPU 里的 mapping 做一次静态近似，但那不是 DV 正确画面） | ❌ **做不到**（这是"唯一正确答案是播不了"的那一档） |

**关于 profile 7 的 EL：明确建议"不解析"**。理由（全部【已证实】）：
* EL 是**交错藏在 BL 的 access unit 里**的 `UNSPEC63` NAL（ffmpeg.org `dovi_split` 文档，§1.1 引文）；
  H.265 解码器会把它当"未知 NAL"丢掉 —— 这正是我们今天能"当 HDR10 播"的原因；
* 要解 EL 需要**拆包 + 第二路解码 + 合成**（FFmpeg 只在 `dovi_split` bsf 里提供"拆"，
  合成在 FFmpeg 里**没有**实现）；
* 本工程自带构建连 `dovi_split` 都没编（`config.mak:1020 !CONFIG_DOVI_SPLIT_BSF=yes`）。

**决策逻辑（伪代码，注意没有定时器/重试）**：

```
dv = streamInfo.dolbyVision            // "" / "dvhe.05.06" / "dvhe.08.06" / "dvhe.07.06"
可出DV = 平台能力查询（§5.3）

if (dv 为空)                      -> 老路径，什么都不变
else if (可出DV 且 dv 允许直出)    -> 请求 DV 输出（当前所有平台都不满足 ⇒ 今天走不到这一支）
else if (dv 是 08.x 或 07.x)       -> 正常播（HDR10/HLG 基底层），徽标显示 "Dolby Vision"
else if (dv 是 05.x)               -> 正常播 + **明确标记"本机不支持该片源的 DV，画面不可正确还原"**
                                      （UI 只做提示，不拦截播放、不改分辨率、不重试）
```

**为什么"不拦截"**：拦截需要新增开关/错误码语义；而"照播 + 诚实标注"符合本项目
"不猜、不写未知"的既有口径（`platform/QtPlayer/src/CicadaPlayerItem.cpp:99-100`
与 `CicadaPlayerItem.h:333-334` 的注释就是这条口径的原文）。

### 5.5 UI 徽标：直接复用"编码徽标"

**现有实现（照抄对象）** —— 用户要求找的就是它：

* 清晰度菜单（**DASH/点播**）：`platform/QtPlayer/controls/RightControls.qml`
  * 每行从 `player.qualities[i].codecLabel` 取文本：`:2082-2089`
  * 徽标本体：`:2135-2176`（`id: codecBadge` 在 `:2142`，`codecBadgeText.text: codecBadge.codecLabel` 在 `:2164`）
  * **"空串就不渲染"** 的既有约定：`:2073-2074`（内核认不出编码时给空串，整块不显示）
* 清晰度菜单（**直播**）：`platform/QtPlayer/controls/LiveMenu.qml`
  * `:160-195`（`id: codecBadge` 在 `:167`，文本在 `:195`），
    宽度预算在 `:71-100`（`longestCodec` 用来给徽标留位置）
* 数据来源：`platform/QtPlayer/src/CicadaPlayerItem.cpp:502-504`
  （`entry.insert("codec", ...)` / `entry.insert("codecLabel", codecDisplayName(codec))`），
  函数 `codecDisplayName()` 在 `:107-116`（**只有一处映射**，注释 `:91-106` 明确警告
  "不许拿它当内部标识用"）
* P 值属性声明：`platform/QtPlayer/src/CicadaPlayerItem.h:322-344`（`qualities` 的字段清单在 `:323-327`）
* 另一处**已有的、带值的 HDR 文案**先例：`platform/QtPlayer/src/CicadaPlayerItem.cpp:2086-2087`
  —— `s.insert("hdr", "HDR10"/"SDR")`，属性注释在 `CicadaPlayerItem.h:281`

**DV 徽标的最小实现（4 处，全部追加、不改语义）**：

1. `CicadaPlayerItem.cpp` 的 `onMediaInfoGetCb` 里，紧随 `codecLabel`（`:504`）
   **追加一个键** `"dolbyVision"`，值取 `si->dolbyVision`（§5.2 的新字段），**空串原样保留**。
2. `CicadaPlayerItem.h:323-327` 的 `qualities` 字段说明里**追加一行** `dolbyVision`。
3. `RightControls.qml`：在 `codecBadge` 旁边**再放一颗同款小胶囊**（复用同一套样式：
   半透明白底 + 1px 描边 + 9px 字，见 `:2135-2176` 的注释），
   文本固定为 `"Dolby Vision"`（**不做 profile→文案 的映射在 QML 里做**，理由同
   `CicadaPlayerItem.cpp:102-105` 的警告：显示名只允许一处来源）。
   显示条件：`modelData.dolbyVision !== ""`，与 `codecLabel` 的"空串不渲染"口径一致。
4. `LiveMenu.qml` 的宽度预算（`:91-100`）**追加**这颗徽标的宽度 —— 否则面板宽度会算少（既有注释已说明该处是在给徽标留位置）。

**为什么不新增布尔属性**：徽标的"是否显示"由**空串/非空串**决定，
这正是 `codecLabel` 已经在用的表达方式（`CicadaPlayerItem.h:333-334`）。
新增 `bool hasDolbyVision` 会同时违反"无布尔开关"与"只有一处来源"两条。

### 5.6 分阶段计划

**阶段 0（零新依赖、零 FFmpeg 重编）—— 建议立刻做**
1. `framework/utils/AFMediaType.h`：`VideoColorInfo` 末尾追加 `char dvProfile[12]`（§5.2）。
2. `framework/utils/ffmpeg_utils.c:698` 之后：从 `codecpar->coded_side_data` 读 `AV_PKT_DATA_DOVI_CONF` 填进去（§5.1）。
3. `mediaPlayer/native_cicada_player_def.h`：`StreamInfo` 末尾追加 `char dolbyVision[16]`。
4. `mediaPlayer/SMPMessageControllerListener.cpp:548` 之后：填 `info->dolbyVision`。
5. `platform/QtPlayer/src/CicadaPlayerItem.{h,cpp}`：`qualities` 追加 `dolbyVision` 键（§5.5）。
6. `RightControls.qml` + `LiveMenu.qml`：DV 徽标（§5.5）。
7. **回退**：profile 8.x/7 无操作；profile 5 只加一条"本机不能正确还原"的提示（§5.4）。

**阶段 1（平台能力查询，仍然零新依赖）**
8. 在 §5.3 的四个平台解码器/渲染文件里各加一个**带值**查询；
9. 走 `CicadaGetVideoCodecSupport()` 的既有 JSON 通道上报（`media_player_api.h:362-409`），
   **追加字段而不是改契约**；
10. 徽标文案按能力分流（例如"本机不支持 DV 呈现"）。

**阶段 2（需要新依赖，必须单独立项）—— 软件 DV 处理 / 真 DV 输出**
11. 给自带 FFmpeg 加 `--enable-libplacebo`（三端 × 各架构），并在各平台提供 Vulkan/GL/D3D11 后端；
    这条路要重编 FFmpeg、要在移动端背一个 GPU 色彩管线，**与"decoder 不许无谓重编"的规则冲突，
    必须先证明收益**（例如：真的必须支持 profile 5）。
12. 路线 B：在**离线转码侧**用 FFmpeg 的 `dovi_split`/`dovi_rpu`（9.0 已具备）把 profile 7 预处理成
    profile 8.1，让播放器永远只需要处理"基底层是 HDR10"的片源 ——
    **这是本报告认为性价比最高的"绕开 DV 处理"的方案**，且完全不碰播放器内核。

### 5.7 明确做不到的（写清楚，免得后面返工）

1. **做不到"真正的 DV 输出"**：本工程三条渲染路径都只搬像素 + `color_trc`
   （`platform/QtPlayer/src/CicadaTextureD3D11.cpp:87-106` 是 Windows 侧的全部），
   没有任何把 RPU 交给显示管线的通道。
2. **做不到"profile 5 正确播放"**（不引 libplacebo 的前提下）。
3. **做不到"profile 7 FEL 合成"**：FFmpeg 没有 BL+EL 合成实现，本工程也没有第二路解码/合成。
4. **做不到"根据 OS 声称的 DV 能力做决策"**：§4 的 (a) 列证据不足，
   而且桌面 Qt/自绘纹理本来就不在那些授权通道上。
5. **做不到"用户可开关的 DV"**：项目规则禁止新增布尔开关（本节设计全程遵守）。

---

## 6. 诚实的底线

**我们能交付的（今天、零新依赖、零 FFmpeg 重编）：**

* **检测**：知道片源是不是 DV、是哪个 profile（8.1 / 8.4 / 7 / 5），依据是容器里的
  `dvcC`/`dvvC` config record —— FFmpeg 已经解析好放在 `codecpar->coded_side_data` 上
  （`libavformat/mov.c:11188-11190`），自带构建也**已经编进了 DOVI 解码侧解析**
  （`external/build/ffmpeg/win32/x86_64/ffbuild/config.mak:922 CONFIG_DOVI_RPUDEC=yes`）。
* **徽标**：清晰度列表 / 直播菜单里显示 **Dolby Vision** 徽标，复用刚做过的编码徽标那套
  （`platform/QtPlayer/controls/RightControls.qml:2142`、`controls/LiveMenu.qml:167`），
  由**真实流元数据**驱动，空串不渲染（沿用 `CicadaPlayerItem.h:333-334` 的口径）。
* **自动回退**：profile 8.x / 7 → 照常按 HDR10/HLG 播（本来就是对的）；
  profile 5 → 照常播但**明确标注"本机无法正确还原"**。全程无定时器、无重试、无新开关。
* **暴露 RPU 派生的元数据**：需要时可以从 `AV_FRAME_DATA_DOVI_METADATA`
  （`libavutil/frame.h:208`）读出 L1/L2/L5 等动态元数据用于展示。

**我们不能交付的：**

* **真正的 DV 画面**（把 DV 元数据送进显示管线、由显示端做 DV 呈现）——
  需要**授权硬件 + OS 的 DV 通道 + Dolby 许可**，而且本工程当前的 D3D11/Metal/VAAPI 渲染路径
  里**根本不存在**这个通道。
* **profile 5 的正确画面**（除非按阶段 2 引入 libplacebo 并重编 FFmpeg）。
* **profile 7 FEL 的增强层合成**。

**代价（如果一定要往下走）：**

| 想要的东西 | 代价 |
|---|---|
| 检测 + 徽标 + 回退 | **≈0**：3 个结构体/枚举追加 + 1 处 side data 读取 + 2 个 QML 徽标。**不动 FFmpeg、不引三方库** |
| 软件 DV 处理（含 profile 5） | 给三端 FFmpeg 加 **libplacebo**（LGPL-2.1+，v7.x）⇒ 重编 FFmpeg、各平台加 GPU 后端（Vulkan/GL/D3D11）、移动端体积与功耗上升；**违反"不无谓重编"规则，须单独立项** |
| 真 DV 输出 | **不是工程问题，是授权问题**：DV 认证硬件 + OS 通道 + Dolby 许可。**本报告不建议走这条路** |
| 省掉 profile 5 的麻烦 | **转码侧**用 FFmpeg 9.0 的 `dovi_split` / `dovi_rpu` 预处理成 profile 8.1（或直接出 HDR10），播放器只面对 HDR10 —— **推荐** |

**一句话交代给决策者：**
> 我们能**识别并标注** DV，能**自动降级**到正确的 HDR10/HLG 播放（profile 8.x/7 完全正确），
> 并且能**如实告知** profile 5 在本机无法正确还原；
> 但我们**不能**在这套技术栈上产出真正的 Dolby Vision 画面 ——
> 那需要授权组件，而不是更多的代码。

---

## 7. 附录：证据索引与待验证清单

### 7.1 待验证清单（每条附最小验证命令）

| # | 待验证的事 | 最小验证方式 |
|---|---|---|
| 1 | **本机 ffmpeg 的 DV 相关能力**（本会话完全没跑成，见文首） | 在 shell 正常的会话里跑：`D:\ffmpeg-8.1-full_build\bin\ffmpeg.exe -hide_banner -version`、`-decoders`、`-h decoder=hevc`、`-h full`、`-filters`，各配 `findstr /i "dovi dolby libplacebo tonemap"`；再核对 `-h filter=libplacebo` 里有没有 `apply_dolbyvision` |
| 2 | **`dovi_split` 是否 9.0 才引入** | `D:\ffmpeg-8.1-full_build\bin\ffmpeg.exe -bsfs` 看有没有 `dovi_split`（本会话只能从 `README.txt:816-833` 的清单推断它**不在** 8.1） |
| 3 | **`dv_bl_signal_compatibility_id` 取值含义** | 用 profile 8.1 / 5 / 7 样片各一个，跑 `ffprobe -show_streams`（**需能打印 DOVI side data 的版本**）或读 `dvcC` box 原始字节 |
| 4 | **profile 5 基底层"颜色错乱"的具体观感与正确回退目标** | 拿一个 profile 5 样片在本工程与一个已知正确的播放器（支持 DV 的）上对比；同时抓 Dolby 官方规范页（本会话 `developer.dolby.com` DNS 失败） |
| 5 | **Dolby 官方 SDK（`dlb_mp4base` 等）的许可条款** | 抓 `https://github.com/DolbyLaboratories/dlb_mp4base` 的 LICENSE/DOC（本会话 github 持续 `fetch failed`）与 developer.dolby.com 的许可页 |
| 6 | **dovi_tool 的具体子命令与平台二进制** | 抓 `https://github.com/quietvoid/dovi_tool` 的 README（本会话未抓成） |
| 7 | **libdovi 的 C 库在 iOS / HarmonyOS 上能不能交叉编出来** | 在有 Rust + cargo-c 的机器上试 `cargo cinstall --release --target aarch64-apple-ios`（及 OHOS 目标） |
| 8 | **Apple / Windows / HarmonyOS 各自的 DV 输出前置条件** | 抓 Apple 的 HDR 元数据白皮书、Microsoft 的 DV/PlayReady 文档、HarmonyOS 的媒体能力文档（本会话均未能抓取） |
| 9 | **Android 授权机型上 MediaCodec 拿到 DV 流时的实际行为** | 在授权机型上跑本工程，打印 `MediaFormat` 与输出 `color_trc`；对比同一片源在非授权机型上的表现 |
| 10 | **`external/build/ffmpeg/<target>/ffbuild/config.mak` 之外的构建来源**（是否存在 gyan 式预编译 libffmpeg.dll，因为它与内置构建的 hwaccel 设置不同） | 查 `framework/codec/avcodecDecoder.cpp:672` 注释提到的 "stock prebuilt win32 libffmpeg.dll (--disable-hwaccels)" 与实际链接产物 |

### 7.2 本次真的抓取过的外部 URL

* <https://ffmpeg.org/ffmpeg-bitstream-filters.html> —— `dovi_rpu`（strip / compression=none|limited|extended）、
  `dovi_split`（`mode=bl|bl_rpu|el|el_rpu`，profile 7 的 UNSPEC63/UNSPEC62 结构）【已证实】
* <https://code.videolan.org/videolan/libplacebo/-/raw/master/src/include/libplacebo/colorspace.h> ——
  `PL_COLOR_SYSTEM_DOLBYVISION`、`pl_dovi_metadata`、`pl_ipt_rgb2lms`【已证实】
* <https://code.videolan.org/videolan/libplacebo/-/raw/master/LICENSE> —— LGPL-2.1【已证实】
* <https://crates.io/api/v1/crates/dolby_vision> —— libdovi：MIT、3.4.0、`capi` feature、仓库地址【已证实】
* <https://static.crates.io/readmes/dolby_vision/dolby_vision-3.4.0.html>（经
  <https://crates.io/api/v1/crates/dolby_vision/3.4.0/readme> 指向）—— libdovi 自述与 cargo-c 用法【已证实】
* <https://libplacebo.org/> —— libplacebo 使用入口（API v4+、仓库在 code.videolan.org）【已证实】
* <https://source.android.google.cn/docs/core/display/hdr?hl=en> —— HTTP 200，但**正文被截断为导航骨架**，
  未取到 DV 相关正文，**不作为已证实引用**【仅证明该 URL 存在】
* <https://deepwiki.com/FFmpeg/FFmpeg/5.5-dolby-vision-and-hdr-metadata> —— 被 Vercel 安全检查拦住（HTTP 429），
  **无有效内容**【未证实】

**说明**：线上 `https://ffmpeg.org/ffmpeg-filters.html` 本次未直接抓取成功；
`apply_dolbyvision` 的原文引用来自系统 ffmpeg 8.1 构建**自带的同名文档副本**
（`D:\ffmpeg-8.1-full_build\doc\ffmpeg-filters.html:20849-20854`），内容与线上同源【已证实·本地副本】。

### 7.3 本次真的读过的本仓库关键行（速查）

| 事实 | 位置 |
|---|---|
| 自带 FFmpeg 版本 9.0 | `external/external/ffmpeg/RELEASE:1` |
| 自带构建 configure 行 | `external/build/ffmpeg/win32/x86_64/ffbuild/config.mak:4`（Android/OHOS 同行结构一致） |
| DV 解码侧解析已编入 | 同上 `:922 CONFIG_DOVI_RPUDEC=yes`（三端一致） |
| `dovi_rpu` / `dovi_split` bsf 未编 | 同上 `:1019` / `:1020` |
| libplacebo 未编（库与滤镜都没有） | 同上 `:743` / `:2432` |
| 只有 4 个滤镜可用（无视频滤镜） | 同上 `:2136` / `:2164` / `:2190` / `:2248` |
| D3D11VA 打开、D3D12VA/DXVA2/Vulkan/VideoToolbox 关闭 | 同上 `:822` / `:823` / `:824` / `:832` / `:831` |
| 系统 ffmpeg 8.1 身份与组件清单 | `D:\ffmpeg-8.1-full_build\README.txt:3` / `:71` / `:128` / `:746` / `:754` / `:825` / `:871` |
| FFmpeg 解码器解析 RPU 并挂帧 | `external/external/ffmpeg/libavcodec/hevc/hevcdec.c:3753` / `:3132` / `:3139` |
| 无 `apply_dovi` / `dolby_vision` 解码器选项 | `external/external/ffmpeg/libavfilter/vf_libplacebo.c:192/969/1099/1662`（全树仅此 4 处） |
| DV 帧/包侧数据类型 | `external/external/ffmpeg/libavutil/frame.h:201/208`、`libavcodec/packet.h:280` |
| config record 字段 | `external/external/ffmpeg/libavutil/dovi_meta.h:55-65`（压缩级别 `:67-72`） |
| 容器解析 → `codecpar->coded_side_data` | `external/external/ffmpeg/libavformat/mov.c:8831/9845-9847/11188-11190`；`matroskadec.c:2507/3351` |
| profile 7 EL 识别与 stream group | `libavformat/mov.c:11195-11198` / `:11210` |
| 本工程 DV 信息**今天被丢弃** | `framework/utils/AFMediaType.h:479` |
| 流元数据填写点（改这里） | `framework/utils/ffmpeg_utils.c:634`（`get_stream_meta`），视频分支 `:689-698` |
| `VideoColorInfo` / `Stream_meta` 末尾 | `framework/utils/AFMediaType.h:382-388` / `:433` / `:462` |
| 应用层 `StreamInfo`（追加规则写在注释里） | `mediaPlayer/native_cicada_player_def.h:58-91`（`videoCodec[16]` 在 `:90`） |
| `HDRType` 现有语义 | `mediaPlayer/native_cicada_player_def.h:47-50` / `:69`；填写点 `mediaPlayer/SMPMessageControllerListener.cpp:545-548` |
| DASH 清单 codecs 通路 | `framework/demuxer/play_list/Representation.h:55/88`；`framework/demuxer/dash/DashStream.cpp:1115-1127` |
| 应用层能力 JSON 通道（可追加字段） | `mediaPlayer/media_player_api.h:362-409` |
| 平台解码器文件 | `framework/codec/Apple/AppleVideoToolBox.cpp`、`framework/codec/Android/mediaCodecDecoder.cpp`、`framework/codec/OHOS/OhosAVCodecDecoder.cpp` |
| Windows 硬解与能力探测 | `framework/codec/avcodecDecoder.cpp:671-723` / `:1054-1057` |
| Windows 渲染侧色彩空间（DV 无通道） | `platform/QtPlayer/src/CicadaTextureD3D11.cpp:87-106` |
| 编码徽标（照抄对象） | `platform/QtPlayer/controls/RightControls.qml:2082-2089` / `:2135-2176`（`codecBadge` 在 `:2142`）；`controls/LiveMenu.qml:71-100` / `:160-195` |
| 徽标数据来源与显示名映射 | `platform/QtPlayer/src/CicadaPlayerItem.cpp:502-504` / `:107-116`；`CicadaPlayerItem.h:322-344` |
| 已有"带值"HDR 文案先例 | `platform/QtPlayer/src/CicadaPlayerItem.cpp:2086-2087`；`CicadaPlayerItem.h:281` |

### 7.4 本会话限制（供后续接手者参考）

* **shell 不可用**：`pwsh` 在沙箱初始化阶段失败
  （`SetNamedSecurityInfoW failed (Win32 5): grantWrite(D:\hilihili)`），
  带 `workdir`、后台运行、委托子代理（子代理回报同一条错误）三种方式均失败；
  本会话审批被禁用且子代理权限不可放宽，故**未能执行任何 ffmpeg 命令**（§2.1）。
* **网络受限**：仅 `ffmpeg.org`、`code.videolan.org`、`crates.io`/`static.crates.io`、
  `libplacebo.org` 抓取成功；`github.com`、`raw.githubusercontent.com`、
  `developer.dolby.com`、`developer.android.com`、`learn.microsoft.com` 均失败。
  外部论断因此严格区分"已抓取"（附 URL）与"【推测/待验证】"（§7.1）。
