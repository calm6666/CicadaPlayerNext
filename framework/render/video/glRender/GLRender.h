//
// Created by lifujun on 2019/8/12.
//

#ifndef SOURCE_GLRENDER_H
#define SOURCE_GLRENDER_H

#include <render/video/AFActiveVideoRender.h>
/* 【并发修复】std::atomic（mVideoRotate/mInitRet 等）与 std::mutex 都显式包含：
 * 原来 <atomic> 是靠 AFActiveVideoRender.h:11 传递进来的，不依赖传递包含更稳。 */
#include <atomic>
#include <mutex>
#include <render/video/vsync/timedVSync.h>
#include <map>
#include <queue>

#include "render_system/EGL/gl_context.h"
#include "platform/platform_gl.h"
#include "IProgramContext.h"

#ifdef __APPLE__

#include <TargetConditionals.h>

#endif

using namespace Cicada;


#if TARGET_OS_IPHONE
#include <codec/utils_ios.h>
class GLRender : public IVideoRender, private IVSync::Listener , private IOSNotificationObserver {
#else
class GLRender : public IVideoRender, private IVSync::Listener {
#endif

public:

    explicit GLRender(float Hz = 60);

    ~GLRender() override;

    int init() override;

    int setDisPlay(void *view) override;

    int clearScreen() override;

    void setBackgroundColor(unsigned int color) override;

    int renderFrame(std::unique_ptr<IAFFrame> &frame) override;

    /*
     * 【同步释放】把 mInputQueue 里还没上屏的帧全部丢掉。
     *
     * 为什么本渲染器要单独覆写：clearScreen() 在这条路上只置了两个标志
     * （mClearScreenOn / bFlushAsync），真正的清空发生在渲染线程下一次循环里 ——
     * 而调用方要的是"关解码器之前帧就已经放掉"（硬解帧钉着解码器输出缓冲 /
     * 表面池，见 IVideoRender::releaseFrames() 的契约）。
     *
     * 队列由 mFrameMutex 保护，渲染线程 pop 时拿的是同一把锁，所以在这里
     * （播放线程）同步清空不会和渲染线程抢队列。
     */
    void releaseFrames() override;

    int setRotate(Rotate rotate) override;

    int setFlip(Flip flip) override;

    /*
     * 【色觉辅助滤镜 / 回退点 R3】覆写 IVideoRender 的空默认实现。
     * 只保存矩阵；真正下发在 renderActually() 里和 updateFlip 并排做（每帧 / 每次格式切换都生效）。
     */
    void setColorMatrix(const float matrix[9]) override;

    int setScale(Scale scale) override;

    void setSpeed(float speed) override;

    void captureScreen(std::function<void(uint8_t *, int, int)> func) override;

    void *getSurface(bool cached) override;

    float getRenderFPS() override;

    void surfaceChanged() override;
    uint64_t getFlags() override
    {
        return 0;
    };

private:

    int VSyncOnInit() override;

    int onVSync(int64_t tick) override;

    void VSyncOnDestroy() override;

#if TARGET_OS_IPHONE

    void AppWillResignActive() override;

    void AppDidBecomeActive() override;

#endif

private:

    void dropFrame();

    void createGLSurface();

    bool renderActually();

    void captureScreen();

    void glClearScreen();

    void calculateFPS(int64_t tick);

    IProgramContext *getProgram(int frameFormat, IAFFrame *frame = nullptr);

    int onVsyncInner(int64_t tick);

protected:

    std::atomic<Rotate> mVideoRotate{Rotate_None};
    std::atomic<Rotate> mRotate{Rotate_None};
    std::atomic<Flip> mFlip{Flip_None};
    std::atomic<Scale> mScale{Scale_AspectFit};
    std::atomic<uint32_t> mBackgroundColor{0xff000000};

    /*
     * 【色觉辅助滤镜 / 回退点 R3】当前颜色矩阵（行主序）。默认单位矩阵 = 关闭。
     * 写入方：播放器线程（setColorMatrix）；读取方：渲染线程（renderActually）。
     *
     * 【并发修复】原来是 `std::atomic<float> mColorMatrix[9]` 逐元素 relaxed 读写：
     * 元素**之间**没有任何配对，渲染线程可能读到"半新半旧的一整组"9 个系数
     * （不是单个 float 撕裂，而是两代写入的值混在一起），严重时出现一帧偏色。
     * 现在是普通 float[9] + 一把锁：锁**只护这 9 个 float 的拷贝**，不扩到任何 GL 调用
     * 或其它逻辑（写极稀疏 = 用户点一下、读每帧一次，代价可忽略；与工程里
     * mViewMutex / mCaptureMutex 同一套写法）。写/读两个点都在 GLRender.cpp。
     */
    float mColorMatrix[9] = {1.0f, 0.0f, 0.0f,
                             0.0f, 1.0f, 0.0f,
                             0.0f, 0.0f, 1.0f};
    /* 【回退点 R3-b】只护 mColorMatrix 的 9 个 float；别把 GL 调用圈进来 */
    std::mutex mColorMatrixMutex;

    int mWindowWidth = 0;
    int mWindowHeight = 0;

private:

    std::atomic_int mInitRet{INT32_MIN};
    std::mutex mInitMutex;
    std::condition_variable mInitCondition;
    std::mutex mFrameMutex;
    std::queue<std::unique_ptr<IAFFrame>> mInputQueue;
//    std::unique_ptr<IAFFrame> mLastRenderFrame = nullptr;
    std::mutex mViewMutex;
    void *mDisplayView = nullptr;
    Cicada::GLContext *mContext = nullptr;
    Cicada::GLSurface *mGLSurface = nullptr;
    std::unique_ptr<IVSync> mVSync = nullptr;
    std::mutex mCaptureMutex;
    bool mCaptureOn = false;
    std::function<void(uint8_t *, int, int)> mCaptureFunc = nullptr;
    std::map<int, std::unique_ptr<IProgramContext>> mPrograms;
    std::mutex mCreateOutTextureMutex;
    std::condition_variable mCreateOutTextureCondition;
    bool needCreateOutTexture = false;
    /*
     * 【B5】"渲染线程已经把 OES 纹理 + DecoderSurface 建好"这个事实的闩。
     * 见 GLRender.cpp 里 getSurface() 的说明：program / 纹理只能在持有 EGL context 的
     * 渲染线程创建，调用线程（播放线程）只等这个闩，不再自己试着建（那会失败并刷 E 日志）。
     * 只在 __ANDROID__ 路径下被读；VSyncOnDestroy 里随 mPrograms.clear() 一起复位。
     */
    std::atomic_bool mOutTextureReady{false};
    bool mInBackground = false;
    uint64_t mRenderCount{};
    uint64_t mRendertimeS{0};
    uint8_t mFps{0};
    float mHz{};
    int64_t mVSyncPeriod;
    af_scalable_clock mRenderClock;

    IProgramContext *mProgramContext = nullptr;
    int mProgramFormat = -1;

    bool mClearScreenOn = false;
    bool mScreenCleared = false;
    IAFFrame::AFFrameInfo mVideoInfo{};

    std::atomic_bool bFlushAsync{false};

#ifdef __ANDROID__
    std::mutex mRenderCallbackMutex{};
    std::condition_variable mRenderCallbackCon{};
#endif

};


#endif //SOURCE_GLRENDER_H
