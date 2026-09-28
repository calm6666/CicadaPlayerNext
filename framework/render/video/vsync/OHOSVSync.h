//
// OHOSVSync.h
//
// 鸿蒙原生 VSync：直接用系统显示节拍（OH_NativeVSync，libnative_vsync.so），
// 不再用 af_usleep 定时器去模拟刷屏节拍。
//
// 对外语义与 timedVSync 完全一致，所以上层（GLRender / SdlAFVideoRender /
// CheaterVideoRender 这些经 VSyncFactory 取 VSync 的渲染器）不需要任何改动：
//   · start()/pause() 起停；
//   · onVSync(tick) 里的 tick = "自本对象创建以来的微秒数 / 一帧的微秒数"，
//     因此 tick / getHz() 仍然是秒（AFActiveVideoRender:81-82 就是这么用的）；
//   · 起停回调 VSyncOnInit / VSyncOnDestroy 与 timedVSync 的线程起止回调对齐。
//

#ifndef FRAMEWORK_OHOSVSYNC_H
#define FRAMEWORK_OHOSVSYNC_H

#ifdef __OHOS__

#include <render/video/vsync/IVSync.h>
#include <utils/timer.h>

#include <native_vsync/native_vsync.h>

#include <atomic>

class OHOSVSync : public IVSync {
public:
    explicit OHOSVSync(Listener &listener);

    ~OHOSVSync() override;

    /*
     * OH_NativeVSync_Create 失败（例如没有显示设备）时为 false，调用方应退回
     * timedVSync。这是能力探测：拿不到系统节拍就换回定时器实现。
     */
    bool isValid() const
    {
        return mNativeVsync != nullptr;
    }

    void start() override;

    void pause() override;

    float getHz() override
    {
        return mHz.load();
    }

    int getPeriod() override
    {
        return mPeriod.load();
    }

private:
    static void onFrame(long long timestamp, void *data);

    void requestNext();

private:
    OH_NativeVSync *mNativeVsync{nullptr};
    std::atomic<bool> mRunning{false};
    std::atomic<bool> mInited{false};
    /* 周期先给一个 60Hz 的名义值，第一次回调后用系统给的周期覆盖。 */
    std::atomic<float> mHz{60.0f};
    std::atomic<int> mPeriod{16667};
    af_clock mClock;
};

#endif // __OHOS__
#endif // FRAMEWORK_OHOSVSYNC_H
