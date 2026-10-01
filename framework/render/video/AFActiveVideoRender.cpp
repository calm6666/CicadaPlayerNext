//
// Created by moqi on 2019-08-02.
//

#define LOG_TAG "AFActiveVideoRender"
#include "AFActiveVideoRender.h"
#include "render/video/vsync/timedVSync.h"
#include <utils/frame_work_log.h>

#define MAX_FRAME_QUEUE_SIZE 100
#define MAX_IN_SIZE 3
using namespace std;

AFActiveVideoRender::AFActiveVideoRender(float Hz)
    : mInputQueue(MAX_FRAME_QUEUE_SIZE)
{
    mVSync = std::unique_ptr<IVSync>(new timedVSync(*this, Hz));
    mVSync->start();
}

AFActiveVideoRender::~AFActiveVideoRender()
{
    if (mRendingFrame) {
        mRendingFrame->setDiscard(true);
    }
    mVSync->pause();
    while (mInputQueue.size() > 0) {
        dropFrame();
    }
    mVSync = nullptr;
}

int AFActiveVideoRender::setHz(float Hz)
{
    return mVSync->setHz(Hz);
}
void AFActiveVideoRender::setSpeed(float speed)
{
    mRenderClock.setSpeed(speed);
}
int AFActiveVideoRender::renderFrame(std::unique_ptr<IAFFrame> &frame)
{
    if (frame == nullptr) {
        // flush the mRendingFrame, when mInputQueue is empty
        mNeedFlushSize = std::max(mInputQueue.size(), (size_t) 1);
        return 0;
    }

    if (mNeedFlushSize > 0) {
        // FIXME: can't drop this frame
        AF_LOGW("renderFrame before flush finish\n");
    }

    if (mInputQueue.size() >= MAX_FRAME_QUEUE_SIZE) {
        AF_LOGE("too many frames...\n");
        return 0;
    }
    //    std::unique_lock<std::mutex> locker(mFrameMutex);
    /*
     * 【临时诊断，定位完就删】把每一次"帧所有权转移/释放"的指针打进日志。
     *
     * 目的：真机崩溃栈是 onVSync → malloc 的错误路径 → abort（堆被破坏，延迟几秒才被发现），
     * 而 backtrace_symbols 在系统库里会解析错位，看不出是哪块内存出问题。这里把 push /
     * free / drop 的裸指针都留下来，下次复现时同一指针出现两次释放就是凶手。
     */
    IAFFrame *pushedFrame = frame.get();
    AF_LOGI("[frame] push ptr=%p pts=%lld\n", static_cast<void *>(pushedFrame),
            (long long) pushedFrame->getInfo().pts);

    mInputQueue.push(frame.release());
    return 0;
}

void AFActiveVideoRender::releaseFrames()
{
    /*
     * 顺序与理由（改这里之前先读 IVideoRender::releaseFrames() 的契约）：
     *
     *   1. 先 mVSync->pause()：afThread::pause() 会等到 VSync 线程走到安全点
     *      （条件变量等待，没有超时）才返回。这之后 mInputQueue 没有消费者，
     *      播放线程是唯一的操作者 —— 这是能**同步**放帧、且不破坏单生产单消费
     *      队列不变量的前提。析构函数（:21-31）用的就是同一套办法。
     *   2. 再放 mRendingFrame（VSync 线程已经取走、但还没交给 deviceRenderFrame 的那一帧）
     *      和队列里所有排队的帧。这两处就是本渲染器对解码帧的**全部**引用。
     *   3. 最后 start() 恢复节拍：放帧是"清空手上存货"，不是"停播"，节拍必须继续。
     *
     * 为什么不能只依赖 renderFrame(nullptr)：它把 flush 登记成 mNeedFlushSize，
     * 真正的丢帧在下一次 VSync 回调里发生 —— 而调用方（关解码器）要的是"现在就放完"。
     */
    mVSync->pause();

    unsigned long long dropped = 0;

    if (mRendingFrame) {
        /*
         * 【只标记丢弃，绝不在这里 delete —— 这是真机崩溃的根因】
         *
         * mVSync->pause() 只保证 VSync 线程停在**安全点**，并不保证它没停在 onVSync
         * 回调内部：那一帧（"已从队列取走、还没交给 deviceRenderFrame"）此刻可能正被
         * 回调使用。原来这里 `mRendingFrame = nullptr`（= 立刻释放）就会让 onVSync
         * 拿着已释放的帧继续跑 —— 真机崩溃栈正是
         *     AFActiveVideoRender::onVSync → abort（libsystem_malloc，堆错误），
         * 而且是延迟十几秒才被 malloc 发现。
         *
         * 析构函数（本文件 :23-25）用的就是"只 setDiscard(true)"，这里与它对齐：
         * 真正的释放交给 VSync 线程自己按 discard 完成。
         */
        mRendingFrame->setDiscard(true);
        dropped++;
    }

    while (mInputQueue.size() > 0) {
        dropFrame();
        dropped++;
    }

    /* 队列已空，之前登记的 flush 已经没有意义了，一起清掉。 */
    mNeedFlushSize = 0;

    mVSync->start();

    AF_LOGI("[mem] releaseFrames: dropped %llu decoded frame(s) held by the renderer "
            "(queued + rendering); the hardware surface pool is nobody's business but the "
            "decoder's from here on\n", dropped);
}

void AFActiveVideoRender::dropFrame()
{
    if(mInputQueue.size() <= 0){
        return;
    }
    int64_t framePts = mInputQueue.front()->getInfo().pts;
    /* 【临时诊断，定位完就删】见 renderFrame 里那段说明 */
    AF_LOGI("drop a frame pts = %lld ptr=%p\n", framePts, static_cast<void *>(mInputQueue.front()));
    mInputQueue.front()->setDiscard(true);
    delete mInputQueue.front();
    mInputQueue.pop();

    if (mListener) {
        mListener->onFrameInfoUpdate(mFrameInfo, false);
    }
}

void AFActiveVideoRender::calculateFPS(int64_t tick)
{
    if ((tick / uint64_t(mVSync->getHz())) != mRendertimeS) {
        mRendertimeS = tick / uint64_t(mVSync->getHz());
        AF_LOGD("video fps is %llu\n", mRenderCount);
        mFps = mRenderCount;
        mRenderCount = 0;
    }
}
int AFActiveVideoRender::onVSync(int64_t tick)
{
    if (mNeedCaptureScreen) {
        mNeedCaptureScreen = false;
        device_captureScreen(mCAPFunc);
    }
    if (mNeedReDraw) {
        mNeedReDraw = false;
        deviceReDraw();
    }
    while (mNeedFlushSize > 0) {
        if (mRendingFrame) {
            /* 【临时诊断，定位完就删】见 renderFrame 里那段说明 */
            AF_LOGI("[frame] free(flush) ptr=%p pts=%lld\n", static_cast<void *>(mRendingFrame.get()),
                    (long long) mRendingFrame->getInfo().pts);
            mRendingFrame->setDiscard(true);
            mRendingFrame = nullptr;
        }
        if (mInputQueue.empty()) {
            break;
        }
        dropFrame();
        mNeedFlushSize--;
    }
    mNeedFlushSize = 0;

    if (mInputQueue.size() >= MAX_IN_SIZE) {
        while (mInputQueue.size() >= MAX_IN_SIZE) {
            dropFrame();
        }
        mRendingFrame = static_cast<unique_ptr<IAFFrame>>(mInputQueue.front());
        mInputQueue.pop();
        mRenderClock.set(mRendingFrame->getInfo().pts);
        mRenderClock.start();
    }

    if (mInputQueue.empty() && mRendingFrame == nullptr) {
        calculateFPS(tick);
        return 0;
    }

    if (mRendingFrame == nullptr) {
        mRendingFrame = static_cast<unique_ptr<IAFFrame>>(mInputQueue.front());
        mInputQueue.pop();
    }

    if (mRenderClock.get() == 0) {
        mRenderClock.set(mRendingFrame->getInfo().pts);
        mRenderClock.start();
    }

    int64_t late = mRendingFrame->getInfo().pts - mRenderClock.get();

    if (llabs(late) > 100000) {
        mRenderClock.set(mRendingFrame->getInfo().pts);
    } else if (late - mVSync->getPeriod() * mRenderClock.getSpeed() > 0) {
        //        AF_LOGD("mVSyncPeriod is %lld\n", mVSync->getPeriod());
        //        AF_LOGD("mRenderClock.get() is %lld\n", mRenderClock.get());
        //        AF_LOGD("frame->getInfo().pts is %lld\n", mRendingFrame->getInfo().pts);
        calculateFPS(tick);
        return 0;
    }

    mFrameInfo = mRendingFrame->getInfo();
    if (deviceRenderFrame(mRendingFrame.get())) {
        mRenderCount++;
    }
    /* 【临时诊断，定位完就删】见 renderFrame 里那段说明 */
    AF_LOGI("[frame] free(rendered) ptr=%p pts=%lld\n", static_cast<void *>(mRendingFrame.get()),
            (long long) mFrameInfo.pts);
    mRendingFrame = nullptr;
    calculateFPS(tick);
    if (mListener) {
        mListener->onFrameInfoUpdate(mFrameInfo, true);
    }
    return 0;
}
void AFActiveVideoRender::captureScreen(std::function<void(uint8_t *, int, int)> func)
{
    mCAPFunc = func;
    mNeedCaptureScreen = true;
};
