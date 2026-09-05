# FFmpeg 9.0 升级说明（依赖与代码迁移）

## 1. 升级内容

| 项 | 升级前 | 升级后 |
|---|---|---|
| FFmpeg 分支 | `n4.3.1` (2020) | **`n9.0`**（FFmpeg 9.0 "Lei"，2025） |
| Android NDK | r14b（gcc 4.9 / android-14/21） | **r25+（clang，API 24）** |
| 编译配置 | `--enable-avresample`（FFmpeg 5.0 已删除，配置必然失败） | 移除；swresample 替代 |
| 私有补丁 | `external/contribute/ffmpeg/0003…0011`（4.3 专用） | **默认关闭**（`FFMPEG_NEED_PATCH=FALSE`），HEVC-in-FLV 等能力已上游合入 |
| 已删除的旧 API | `av_init_packet` / `avcodec_close` / `av_register_all` / `av_lockmgr_register` / `av_compute_pkt_fields` / `AVStream::parser` / `channels`/`channel_layout` 字段 | 全部迁移为 9.0 API |

## 2. 依赖版本矩阵（升级后）

| 依赖 | 版本 | 说明 |
|---|---|---|
| FFmpeg | n9.0 | 解封装 + 软解 + 滤镜 |
| curl | 7.68.0（可升至 8.x） | http/https 数据源 |
| openssl | 1.1.1g（建议升 1.1.1w/3.x） | TLS |
| nghttp2 | 1.41.0 | HTTP/2 |
| libxml2 | 2.9.9 | DASH MPD 解析 |
| fdk-aac（可选） | 需 `--enable-nonfree --enable-gpl` | AAC 编解码 |
| dav1d（可选） | 0.6.0+ | AV1 软解 |
| x264（可选） | 需 `--enable-nonfree --enable-gpl --enable-version3` | 编码 |

## 3. 已迁移的 FFmpeg 旧 API 清单（代码级）

| 旧 API（FFmpeg ≤4.3） | 新 API（FFmpeg 9.0） | 涉及文件 |
|---|---|---|
| `av_init_packet` | `av_packet_alloc()/av_packet_free()` | `AVAFPacket.cpp`、`avFormatDemuxer.cpp`、`avFormatSubtitleDemuxer.cpp`、`AVBSF.cpp`、`bitStreamParser.cpp`、测试 |
| `avcodec_close` | `avcodec_free_context()` | `avcodecDecoder.cpp` |
| `av_register_all / avcodec_register_all / avfilter_register_all / av_lockmgr_register` | 直接删除（5.0 起自注册、内置线程安全） | `ffmpeg_utils.c`、`avcodecDecoder.cpp`、`ffmpegVideoFilter.cpp`、`ffmpegAudioFilter.cpp` |
| `av_register_input_format` | 自定义 demuxer 改为标准 demuxer + `ReadPacket()` 内就地解密（FFmpeg 6.1+ 回调已移入内部 `FFInputFormat`，公开 `AVInputFormat` 不再携带 read_header/read_packet 等回调） | `SampleDecryptDemuxer.cpp`（`sampleDecryptDec.c` 已移出构建） |
| `AVStream::parser / need_parsing` | 删除（7.0 移入私有结构） | `ffmpeg_utils.c`、`sampleDecryptDec.c`（后者已移出构建） |
| `AVFormatContext::correct_ts_overflow` | 删除（7.0 移除，0 为默认） | `avFormatDemuxer.cpp`、`avFormatSubtitleDemuxer.cpp` |
| `AVStream::codec_info_nb_frames` | `mCtx->nb_frames` | `avFormatDemuxer.cpp` |
| vendored `av_compute_pkt_fields` | 删除调用（`av_read_frame` 内部已计算） | `ffmpeg_utils.c/.h`、`avFormatDemuxer.cpp`（以 `#if LIBAVFORMAT_VERSION_MAJOR < 59` 保留兼容分支） |
| `AVCodecContext::refcounted_frames` | 删除（5.0 起默认 refcount） | `avcodecDecoder.cpp` |
| `AVCodecContext::channels / channel_layout`（AVFrame/AVCodecParameters 同） | `AVChannelLayout ch_layout`（`nb_channels` / `u.mask` / `av_channel_layout_default/from_mask/describe`） | `ffmpeg_utils.c`、`AVAFPacket.cpp`、`avcodecDecoder.cpp`、`MetaToCodec.cpp`、`ffmpegAudioFilter.cpp` |
| `av_get_default_channel_layout / av_get_channel_layout_string / av_get_channel_layout_nb_channels` | `av_channel_layout_default()` + `av_channel_layout_describe()` | `ffmpegAudioFilter.cpp`、`MetaToCodec.cpp` |
| `AVInputFormat *`（非 const） | `const AVInputFormat *` | `avFormatDemuxer.*`、`avFormatSubtitleDemuxer.cpp` |
| `avcodec_find_decoder` 返回 `AVCodec *` | `const AVCodec *`（FFmpeg 9 返回值加 const） | `ffmpeg_utils.c`、`video_tool_box_utils.c` |
| `HEVCSEI`（栈上结构） | FFmpeg 7.0 起堆分配：`ff_hevc_sei_alloc()/ff_hevc_sei_free()`，按 `LIBAVCODEC_VERSION_MAJOR` 版本分支 | `ffmpeg_utils.c`、`video_tool_box_utils.c` |

内部 API（`ff_h264_decode_extradata`、`ff_avc_parse_nal_units_buf`、`ff_isom_write_avcc`、`avpriv_set_pts_info` 等）
保留使用 —— 项目以**静态库**方式链接 FFmpeg 且 include 源码树头文件，FFmpeg 9.0 中这些内部符号仍然存在。

## 4. 私有补丁说明

`external/contribute/ffmpeg/` 下的 11 个补丁均为 FFmpeg 4.3 时代产物：

- 0006（FLV HEVC 扩展）、0007（默认编译 avc.c/hevc.c）→ **FFmpeg 5.0 已上游合入**，无需再打；
- 0003/0010（符号导出 libavformat.v）→ FFmpeg 7.0 起版本脚本机制重写，静态链接无需导出；
- 0008/0011（阿里云 FLV 音频扩展 / flv_strict_header）→ 若业务仍依赖阿里云私有 FLV 格式，需要针对 n9.0 重新移植补丁，并将 `FFMPEG_NEED_PATCH` 置回 `TRUE`；
- 0001/0002/0004/0005/0009 为历史构建兼容补丁，n9.0 无需。

## 5. 编译验证要点

```bash
cd external
./build_external.sh Android    # NDK r25c+, API 24
# 或 iOS / macOS / Linux / Windows / OHOS
```

- 所有平台统一走 `player_ffmpeg_config.sh`（组件名已按 9.0 校验，不支持的名字会直接报错退出）；
- `build/ffmpeg/<平台>/<abi>/` 为 FFmpeg 构建目录，`install/ffmpeg/<平台>/<abi>/` 为产物；
- Android/OHOS 会进一步把所有静态库合并为 `libalivcffmpeg.so`（clang/lld 链接）。

## 6. 参考

- [FFmpeg 9.0 "Lei" 发布说明](https://ffmpeg.org/index.html#news)
- [FFmpeg 版本支持周期](https://endoflife.date/ffmpeg)
- [FFmpeg API 变更日志（5.0/6.0/7.0/8.0/9.0）](https://github.com/FFmpeg/FFmpeg/blob/master/doc/APIchanges)
