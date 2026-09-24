//
// Created by moqi on 2018/11/27.
//

#ifndef CICADA_PLAYER_PROPERTY_H
#define CICADA_PLAYER_PROPERTY_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 硬解 copy-back 的输出格式开关（实现见 framework/codec/avcodecDecoder.cpp）。
 *
 * "ON"：下载出来的 NV12 直接交给呈现方，跳过每帧一次全画幅 swscale 转换。
 *       4K60 一帧省 12.4MB 读 + 12.4MB 写，是 4K 追不上实时的主要开销。
 *       只有真正能吃 NV12 的呈现方（例如 cmdline 用 SDL_PIXELFORMAT_NV12
 *       纹理的 onVideoRendering()）才能打开这个开关。
 * 其它值/未设置：保持原行为，转成 YUV420P 再交出去。
 */
#define PROPERTY_KEY_HW_COPYBACK_NV12 "video.render.hw.copyback_nv12"

/*
 * 硬解零拷贝直通开关（实现见 framework/codec/avcodecDecoder.cpp）。
 *
 * "ON"：解码出来的 GPU 纹理**不下载**，直接交给上层（AF_PIX_FMT_D3D11，
 *       data[0] = ID3D11Texture2D*，data[1] = 数组切片索引）。上层必须能用
 *       D3D11 自己把这张纹理画到窗口上，否则什么都看不到——SDL2 没有把外部
 *       纹理包成 SDL_Texture 的 API，所以 cmdline 在打开这个开关时同时切到
 *       自己的 D3D11 呈现路径（-direct）。
 * 其它值/未设置：保持 copy-back（下载到内存再渲染）。
 */
#define PROPERTY_KEY_HW_DIRECT_TEXTURE "video.render.hw.direct_texture"

/*
 * 渲染回调的频率（Hz，浮点字符串，例如 "120"）。实现见
 * framework/render/video/SdlAFVideoRender.cpp。
 *
 * 未设置时渲染器按 60Hz 起回调（原来的行为）。零拷贝直通
 * （PROPERTY_KEY_HW_DIRECT_TEXTURE）时呈现完全由呈现方按显示器刷新率节流，
 * 这个值就该设成当前显示器的刷新率：回调格子越细，24fps 这类片源在高刷屏上
 * 越不容易因为只能落在 60Hz 的格子上而出现 3:2 抖动。
 * 设成 0 或者负数表示不改（仍然 60Hz）。
 */
#define PROPERTY_KEY_VIDEO_RENDER_HZ "video.render.hz"

/*
 * macOS 上是否启用 FFmpeg 的 VideoToolbox 硬件解码路径（实现见
 * framework/codec/avcodecDecoder.cpp 的 initHwDecoder）。
 *
 * "ON"：Apple 平台走 FFmpeg 的 videotoolbox hwaccel，解码结果是 CVPixelBuffer
 *       （AV_PIX_FMT_VIDEOTOOLBOX，data[3]），macOS 上可以用 CVMetalTextureCache
 *       包成 MTLTexture，交给 Qt 的 Metal 场景图——硬解 + 零拷贝。
 *       同时 framework/codec/decoderFactory.cpp 会在 Apple 平台优先选
 *       avcodecDecoder 而不是自研的 AFVTBDecoder。
 * 其它值/未设置：保持原行为（Apple 用自己的 AFVTBDecoder），不影响 iOS/macOS
 *       上已有的应用。
 */
#define PROPERTY_KEY_DECODER_FFMPEG_VT "video.decoder.ffmpeg_videotoolbox"

const char* getProperty(const char *key);

/*
 * @key
 * ro.* : can't be modified after be set
 * protected.* : only can be modified by the thread first set
 * return 0 if ok, otherwise return -1
 */
int setProperty(const char *key, const char *value);

#ifdef __cplusplus
}
#endif

#endif //CICADA_PLAYER_PROPERTY_H
