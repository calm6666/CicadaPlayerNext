//
// Created by moqi on 2019-08-28.
//
#include "PBAFFrame.h"
#include "AVAFPacket.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

PBAFFrame::PBAFFrame(CVPixelBufferRef pixelBuffer, int64_t pts, int64_t duration, const VideoColorInfo &info)
    : mPBuffer(CVPixelBufferRetain(pixelBuffer))
{

    mInfo.pts = pts;
    mInfo.duration = duration;
    mInfo.video.format = AF_PIX_FMT_APPLE_PIXEL_BUFFER;
    mInfo.video.width = (int) CVPixelBufferGetWidth(mPBuffer);
    mInfo.video.height = (int) CVPixelBufferGetHeight(mPBuffer);
    mInfo.video.colorInfo = info;

    OSType pixel_format = CVPixelBufferGetPixelFormatType(pixelBuffer);
    if (pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
        mInfo.video.colorRange = COLOR_RANGE_FULL;
    } else if (pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
        mInfo.video.colorRange = COLOR_RANGE_LIMITIED;
    } else if (pixel_format == kCVPixelFormatType_32BGRA) {
        /*
         * 【2026-09-30 · macOS 对齐】32BGRA 是 **RGB（打包）**，没有"有限/全范围"那一说，
         * 一律按 FULL 记：Qt 播放器会强制 VideoToolbox 直出 32BGRA（零拷贝那条路要求
         * 单平面纹理），CPU 回退/截图都从这一帧来；这里报 UNSPECIFIED 会让下游
         * （swscale / 统计面板）按"未知"处理，截图与画面颜色就会对不上。
         */
        mInfo.video.colorRange = COLOR_RANGE_FULL;
    } else {
        mInfo.video.colorRange = COLOR_RANGE_UNSPECIFIED;
    }

    CFTypeRef colorAttachments = CVBufferGetAttachment((CVPixelBufferRef) pixelBuffer, kCVImageBufferYCbCrMatrixKey, NULL);
    if (colorAttachments != nullptr) {
        if (CFStringCompare((CFStringRef) colorAttachments, kCVImageBufferYCbCrMatrix_ITU_R_601_4, 0) == kCFCompareEqualTo) {
            mInfo.video.colorSpace = COLOR_SPACE_BT601;
        } else if (CFStringCompare((CFStringRef) colorAttachments, kCVImageBufferYCbCrMatrix_ITU_R_709_2, 0) == kCFCompareEqualTo) {
            mInfo.video.colorSpace = COLOR_SPACE_BT709;
        } else if (CFStringCompare((CFStringRef) colorAttachments, kCVImageBufferYCbCrMatrix_ITU_R_2020, 0) == kCFCompareEqualTo) {
            mInfo.video.colorSpace = COLOR_SPACE_BT2020;
        } else {
            mInfo.video.colorSpace = COLOR_SPACE_UNSPECIFIED;
        }
    } else {
        mInfo.video.colorSpace = COLOR_SPACE_UNSPECIFIED;
    }
}
static AVFrame *nv122yuv420p(AVFrame *frame)
{

    int x, y;
    AVFrame *outFrame = av_frame_alloc();
    outFrame->height = frame->height;
    outFrame->width = frame->width;
    outFrame->format = AV_PIX_FMT_YUV420P;
    int ret = av_frame_get_buffer(outFrame, 32);
    ret = av_frame_make_writable(outFrame);

    if (frame->linesize[0] == frame->width) {
        memcpy(outFrame->data[0], frame->data[0], frame->width * frame->height);
    } else {
        for (y = 0; y < outFrame->height; ++y) {
            for (x = 0; x < outFrame->width; ++x) {
                outFrame->data[0][y * outFrame->linesize[0] + x] = frame->data[0][y * frame->linesize[0] + x];
            }
        }
    }

    for (y = 0; y < outFrame->height / 2; ++y) {
        for (x = 0; x < outFrame->width / 2; ++x) {
            outFrame->data[1][y * outFrame->linesize[1] + x] = frame->data[1][y * frame->linesize[1] + 2 * x];
            outFrame->data[2][y * outFrame->linesize[2] + x] = frame->data[1][y * frame->linesize[1] + 2 * x + 1];
        }
    }
    return outFrame;
}

PBAFFrame::operator AVAFFrame *()
{
    CVReturn err;
    uint8_t *data[4] = {nullptr};
    int linesize[4] = {0};
    int planes, ret, i;
    OSType pixel_format = CVPixelBufferGetPixelFormatType(mPBuffer);
    int format;
    if (pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange || pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
        format = AV_PIX_FMT_NV12;
    } else if (pixel_format == kCVPixelFormatType_420YpCbCr8Planar) {
        format = AV_PIX_FMT_YUV420P;
    } else if (pixel_format == kCVPixelFormatType_32BGRA) {
        /*
         * 【2026-09-30 · macOS 对齐 · 这一条是 P0 修复】
         *
         * Qt 播放器为了让 Metal 零拷贝能直接用一张普通纹理采样，**强制** VideoToolbox
         * 输出 32BGRA（见 CicadaPlayerItem.cpp 里设 pixelBufferOutputFormat 的那一段）。
         * 而这里原来只认 NV12 / YUV420P，32BGRA 直接 return nullptr ⇒
         *   · CPU 回退（CicadaVideoTexture 的 textureForFrameCpu）拿不到帧，
         *     配上门面里"运行期零拷贝失败就永久切 CPU"的判定 ⇒ **画面永久冻结**；
         *   · 截图（CicadaVideoRender 的 3c 分支）同样拿不到 CPU 像素 ⇒ 永远空图。
         *
         * 32BGRA 是非平面（打包）格式，所以下面 CVPixelBufferIsPlanar() 走 else，
         * 用 GetBaseAddress/GetBytesPerRow 拿一整块，av_image_copy 按 AV_PIX_FMT_BGRA
         * 拷就行（swscale 那条链本来就吃 BGRA）。
         */
        /*
         * 【2026-10-01 回退：这条路会让 swscale 直接 abort】
         *
         * 32BGRA 在 Cicada 的 AFMediaType 里**没有**对应的 AF_PIX_FMT_* 值。把它声明成
         * AV_PIX_FMT_BGRA 之后，AVAFPacket::copyInfo() → AVPixFmt2Cicada() 只能得到
         * AF_PIX_FMT_NONE（真机日志里那行 "AVPixelFormat 28 not found" 就是它），
         * 下游再把这个未知格式交给 swscale，就命中 FFmpeg 自己的断言：
         *     Assertion desc failed at src/libswscale/swscale_internal.h:778   → SIGABRT
         *
         * 所以这里先回到"拿不到 CPU 帧"的安全行为：32BGRA 只走 GPU 零拷贝那条路
         * （CicadaVideoRender / CicadaTextureMetal 直接吃 CVPixelBuffer，不经过 AVAFFrame）。
         * 要让 CPU 回退与截图也支持 32BGRA，得先在 AFMediaType 里补一个 BGRA 格式、
         * 并让 swscale 那条链认识它 —— 那是独立的一步，不能靠这一行糊过去。
         */
        return nullptr;
    } else {
        return nullptr;
    }
    AVFrame *pFrame = av_frame_alloc();
    pFrame->format = format;
    pFrame->width = (int) CVPixelBufferGetWidth(mPBuffer);
    pFrame->height = (int) CVPixelBufferGetHeight(mPBuffer);
    ret = av_frame_get_buffer(pFrame, 32);

    if (ret < 0) {
        av_frame_free(&pFrame);
        return nullptr;
    }

    err = CVPixelBufferLockBaseAddress(mPBuffer, kCVPixelBufferLock_ReadOnly);

    if (err != kCVReturnSuccess) {
        av_frame_free(&pFrame);
        return nullptr;
    }

    if (CVPixelBufferIsPlanar(mPBuffer)) {
        planes = (int)CVPixelBufferGetPlaneCount(mPBuffer);

        for (i = 0; i < planes; i++) {
            data[i] = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(mPBuffer, i));
            linesize[i] = (int)CVPixelBufferGetBytesPerRowOfPlane(mPBuffer, i);
        }
    } else {
        data[0] = static_cast<uint8_t *>(CVPixelBufferGetBaseAddress(mPBuffer));
        linesize[0] = (int)CVPixelBufferGetBytesPerRow(mPBuffer);
    }

    av_image_copy(pFrame->data, pFrame->linesize,
                  (const uint8_t **) data, linesize, static_cast<AVPixelFormat>(pFrame->format),
                  pFrame->width, pFrame->height);
    CVPixelBufferUnlockBaseAddress(mPBuffer, kCVPixelBufferLock_ReadOnly);
    if (pFrame->format == AV_PIX_FMT_NV12) {
        AVFrame *pyvFrame = nv122yuv420p(pFrame);
        av_frame_free(&pFrame);
        pFrame = pyvFrame;
    }
    pFrame->pts = mInfo.pts;
    // FFmpeg 7.0 删除了 AVFrame::pkt_duration, 统一用 duration(语义相同),
    // 与 AVAFPacket::copyInfo 里的写法保持一致。
    pFrame->duration = mInfo.duration;
    pFrame->chroma_location = static_cast<AVChromaLocation>(mInfo.video.colorInfo.chroma_location);
    pFrame->color_primaries = static_cast<AVColorPrimaries>(mInfo.video.colorInfo.color_primaries);
    pFrame->color_range = static_cast<AVColorRange>(mInfo.video.colorInfo.color_range);
    pFrame->colorspace = static_cast<AVColorSpace>(mInfo.video.colorInfo.color_space);
    pFrame->color_trc = static_cast<AVColorTransferCharacteristic>(mInfo.video.colorInfo.color_trc);

    AVAFFrame *pAvFrame = new AVAFFrame(pFrame, FrameTypeVideo);
    pAvFrame->getInfo().timePosition = mInfo.timePosition;
    av_frame_free(&pFrame);
    return pAvFrame;
}
