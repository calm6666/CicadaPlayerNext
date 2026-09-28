//
// OHOSVSync.cpp
//
// 见 OHOSVSync.h。
//

#define LOG_TAG "OHOSVSync"

#ifdef __OHOS__

#include "OHOSVSync.h"

#include <utils/frame_work_log.h>

#include <cstring>

OHOSVSync::OHOSVSync(Listener &listener) : IVSync(listener)
{
    static const char *kName = "cicadaVSync";
    mNativeVsync = OH_NativeVSync_Create(kName, static_cast<unsigned int>(strlen(kName)));
    if (mNativeVsync == nullptr) {
        AF_LOGW("OH_NativeVSync_Create failed, the caller should fall back to the timer VSync\n");
    }
    mClock.start();
}

OHOSVSync::~OHOSVSync()
{
    // 先停掉续挂：已经在途的那次回调看到 mRunning=false 会直接返回，不再碰本对象。
    mRunning = false;
    if (mNativeVsync != nullptr) {
        OH_NativeVSync_Destroy(mNativeVsync);
        mNativeVsync = nullptr;
    }
    if (mInited.exchange(false)) {
        mListener.VSyncOnDestroy();
    }
}

void OHOSVSync::start()
{
    if (mNativeVsync == nullptr) {
        return;
    }
    if (!mInited.exchange(true)) {
        mListener.VSyncOnInit();
    }
    // 已经在跑就不重复挂帧（同一帧里重复 RequestFrame 只会留下最后一次回调）。
    if (mRunning.exchange(true)) {
        return;
    }
    requestNext();
}

void OHOSVSync::pause()
{
    /*
     * 不去取消已经在途的那次请求（没有对应接口），只是不再续挂；
     * 那一次回调进来后看到 mRunning=false 会立刻返回，等于停了。
     */
    mRunning = false;
}

void OHOSVSync::requestNext()
{
    if (!mRunning.load() || mNativeVsync == nullptr) {
        return;
    }
    if (OH_NativeVSync_RequestFrame(mNativeVsync, &OHOSVSync::onFrame, this) != 0) {
        AF_LOGW("OH_NativeVSync_RequestFrame failed, stop the vsync loop\n");
        mRunning = false;
    }
}

void OHOSVSync::onFrame(long long timestamp, void *data)
{
    (void) timestamp;

    auto *self = static_cast<OHOSVSync *>(data);
    if (self == nullptr || !self->mRunning.load()) {
        return;
    }

    /*
     * 周期由系统给（纳秒）：第一次回调之后就能取到，因此不必像 Android 侧那样靠
     * 两次时间戳相减去猜刷新率。
     */
    long long periodNs = 0;
    if (OH_NativeVSync_GetPeriod(self->mNativeVsync, &periodNs) == 0 && periodNs > 0) {
        const int periodUs = static_cast<int>(periodNs / 1000);
        if (periodUs > 0 && periodUs != self->mPeriod.load()) {
            self->mPeriod.store(periodUs);
            self->mHz.store(1000000.0f / static_cast<float>(periodUs));
        }
    }

    const int period = self->mPeriod.load();
    const int64_t tick = period > 0 ? self->mClock.get() / period : 0;
    if (self->mListener.onVSync(tick) >= 0) {
        self->requestNext();
    } else {
        // 上层要求停下（与 timedVSync::timedThread 的返回语义一致）。
        self->mRunning = false;
    }
}

#endif // __OHOS__
