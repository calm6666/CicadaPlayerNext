//
// Created by moqi on 2019-08-28.
//

#ifndef CICADA_PLAYER_PBAFFRAME_H
#define CICADA_PLAYER_PBAFFRAME_H

#include "base/media/IAFPacket.h"
#include <CoreMedia/CoreMedia.h>
#include <utils/AFMediaType.h>

class AVAFFrame;

class CICADA_CPLUS_EXTERN PBAFFrame : public IAFFrame {
public:
    PBAFFrame(CVPixelBufferRef pixelBuffer, int64_t pts, int64_t duration, const VideoColorInfo &info);

    ~PBAFFrame() override
    {
        CVPixelBufferRelease(mPBuffer);
    }

    FrameType getType() override
    {
        return FrameTypeVideo;
    };

    uint8_t **getData() override
    {
        mData[0] = reinterpret_cast<uint8_t *>(mPBuffer);
        return mData;
    }

    int *getLineSize() override
    {
        return nullptr;
    }

    CVPixelBufferRef getPixelBuffer()
    {
        return mPBuffer;
    }

    /*
     * 克隆一帧。
     *
     * 以前这里返回 nullptr，导致任何"把帧留到另一个线程再画"的用法在 Apple 上拿到
     * 空指针（Qt 组件就是这样：解码回调在框架 VSync 线程上，绘制在 Qt 渲染线程上，
     * 中间必须留一份副本）。
     *
     * 实现上是**零拷贝**的：构造函数只做 CVPixelBufferRetain（引用计数 +1），
     * 像素数据一个字节都不搬。只要这一帧还活着，解码器的像素缓冲池就不会复用这块
     * IOSurface，所以交给别的线程去采样是安全的。
     */
    std::unique_ptr<IAFFrame> clone() override
    {
        return std::unique_ptr<IAFFrame>(
                   new PBAFFrame(mPBuffer, mInfo.pts, mInfo.duration, mInfo.video.colorInfo));
    }

    explicit operator AVAFFrame *();

private:
    CVPixelBufferRef mPBuffer;
    uint8_t *mData[1];
};


#endif //CICADA_PLAYER_PBAFFRAME_H
