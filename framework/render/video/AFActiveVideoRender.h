//
// Created by moqi on 2019-08-02.
//

#ifndef FRAMEWORK_AFACTIVEVIDEORENDER_H
#define FRAMEWORK_AFACTIVEVIDEORENDER_H


#include "IVideoRender.h"
#include "render/video/vsync/IVSync.h"
#include <atomic>
#include <base/media/spsc_queue.h>
#include <mutex>
#include <queue>
#include <utils/af_clock.h>

class AFActiveVideoRender : public IVideoRender, private IVSync::Listener {

public:
    explicit AFActiveVideoRender(float Hz = 60);

    ~AFActiveVideoRender() override;

    void setSpeed(float speed) final;

    int renderFrame(std::unique_ptr<IAFFrame> &frame) final;

    /*
     * 【同步释放】放掉 mRendingFrame 与 mInputQueue 里所有还没上屏的帧。
     *
     * 为什么必须暂停 VSync 线程才能放：mInputQueue 是**单生产单消费**队列
     * （renderFrame() 在播放线程 push，onVSync() 在 VSync 线程 pop），从播放线程
     * 直接去 pop 会和消费者抢同一个队列。所以这里的做法与析构函数完全一致 ——
     * 先 mVSync->pause()（afThread::pause 会**等到**线程走到安全点才返回，无超时），
     * 此时播放线程是队列唯一的操作者，逐帧 delete，然后再 start() 恢复节拍。
     *
     * 调用契约见 IVideoRender::releaseFrames()：必须在关解码器之前同步放完。
     */
    void releaseFrames() override;

    float getRenderFPS() final
    {
        return mFps;
    }
    void captureScreen(std::function<void(uint8_t *, int, int)> func) override;

protected:
    void oNRedraw()
    {
        mNeedReDraw = true;
    }

private:
    int VSyncOnInit() override
    {
        return 0;
    }

    int onVSync(int64_t tick) override;

    void VSyncOnDestroy() override
    {}

    virtual int setHz(float Hz);

    virtual void device_captureScreen(std::function<void(uint8_t *, int, int)> func)
    {}
    virtual bool deviceRenderFrame(IAFFrame *frame) = 0;
    virtual void deviceReDraw(){};

private:
    void dropFrame();
    void calculateFPS(int64_t tick);

protected:
    std::unique_ptr<IVSync> mVSync{};

private:
    Cicada::SpscQueue<IAFFrame *> mInputQueue;
    //    std::mutex mFrameMutex;
    //    std::queue<std::unique_ptr<IAFFrame>> mInputQueue;
    af_scalable_clock mRenderClock;
    IAFFrame::AFFrameInfo mFrameInfo{};
    uint64_t mRenderCount{};
    uint64_t mRendertimeS{0};
    uint8_t mFps{0};
    size_t mNeedFlushSize{0};
    std::unique_ptr<IAFFrame> mRendingFrame{nullptr};
    std::atomic_bool mNeedCaptureScreen{false};
    bool mNeedReDraw{false};
    std::function<void(uint8_t *, int, int)> mCAPFunc{nullptr};
};


#endif//FRAMEWORK_AFACTIVEVIDEORENDER_H
