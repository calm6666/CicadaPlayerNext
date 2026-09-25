//
// Created by lifujun on 2019/8/12.
//
#define  LOG_TAG "GLRender"


#include "render_system/EGL/gl_context_factory.h"
#include "GLRender.h"
#include <utils/timer.h>
#include <utils/AFMediaType.h>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <render/video/glRender/base/utils.h>

using namespace std;

static const int MAX_IN_SIZE = 3;

#ifdef __ANDROID__

    #include "OESProgramContext.h"

#elif TARGET_OS_IPHONE

    #include "CV420PProgramContext.h"
    #include <base/media/PBAFFrame.h>

#endif

#include "render/video/vsync/VSyncFactory.h"
#include "YUVProgramContext.h"

using namespace Cicada;

GLRender::GLRender(float Hz)
{
    /*
     * 【色觉辅助滤镜 / 回退点 R3-b】颜色矩阵的初值（单位矩阵 = 关闭）现在写在
     * GLRender.h 的成员声明处（普通 float[9] 可以直接聚合初始化）。
     * 原来这里那 9 次逐元素 .store() 是 std::atomic<float>[9] 时代的写法，已删除。
     */
    mVSync = VSyncFactory::create(*this, Hz);
    mHz = 0;
    mVSyncPeriod = static_cast<int64_t>(1000000 / Hz);
#if TARGET_OS_IPHONE
    IOSNotificationManager::Instance()->RegisterObserver(this, 0);
    mInBackground = IOSNotificationManager::Instance()->GetActiveStatus() == 0;
    setenv("METAL_DEVICE_WRAPPER_TYPE", "0", 1);
//   setenv("CG_CONTEXT_SHOW_BACKTRACE", "1", 1);
#endif
}

GLRender::~GLRender()
{
#if TARGET_OS_IPHONE
    IOSNotificationManager::Instance()->RemoveObserver(this);
#endif
    AF_LOGE("~GLRender");
    // MUST delete Vsync here,because it has callback
    mVSync = nullptr;
}

int GLRender::init()
{
    AF_LOGD("-----> init .");
    // don't auto start in background
    std::unique_lock<std::mutex> locker(mInitMutex);

    if (!mInBackground) {
        mVSync->start();
    }

    return 0;
}

int GLRender::clearScreen()
{
    AF_LOGD("-----> clearScreen");
    mClearScreenOn = true;
    bFlushAsync = true;
    return 0;
}

int GLRender::renderFrame(std::unique_ptr<IAFFrame> &frame)
{
//    AF_LOGD("-----> renderFrame");
    if (mInitRet != INT32_MIN && mInitRet != 0) {
        return -EINVAL;
    }

    if (frame == nullptr) {
        bFlushAsync = true;
        return 0;
    }

    std::unique_lock<std::mutex> locker(mFrameMutex);
    mInputQueue.push(move(frame));
    return 0;
}

void GLRender::dropFrame()
{
    int64_t framePts = mInputQueue.front()->getInfo().pts;
    AF_LOGI("drop a frame pts = %lld ", framePts);
    mInputQueue.front()->setDiscard(true);
    mInputQueue.pop();
    if (mListener) {
        mListener->onFrameInfoUpdate(mVideoInfo, false);
    }
}

int GLRender::setRotate(IVideoRender::Rotate rotate)
{
    AF_LOGD("-----> setRotate");
    mRotate = rotate;
    return 0;
}

int GLRender::setFlip(IVideoRender::Flip flip)
{
    AF_LOGD("-----> setFlip");
    mFlip = flip;
    return 0;
}

/*
 * 【色觉辅助滤镜 / 回退点 R3】
 * 只存矩阵，不下发：真正的 GL 调用集中在 renderActually()（持 EGL context 的渲染线程），
 * 这里碰 GL 会踩到"非渲染线程操作 GL"的老坑。存下来后，renderActually 每帧都会带上
 * ——所以"切格式（硬解 OES ↔ 软解 YUV）之后滤镜仍然生效"是自然成立的，不需要额外补下发。
 * 单位矩阵 = 关闭（着色器里乘上去等于没乘），不做"是否设置过"的判断。
 */
void GLRender::setColorMatrix(const float matrix[9])
{
    if (matrix == nullptr) {
        return;
    }

    AF_LOGI("-----> setColorMatrix");
    /*
     * 【回退点 R3-b】锁只护这 9 个 float 的拷贝：本函数不碰 GL、也没有别的判断，
     * 所以锁的范围到此为止（AF_LOGI 放在锁外，不持锁打日志）。
     */
    std::lock_guard<std::mutex> locker(mColorMatrixMutex);

    for (int i = 0; i < 9; i++) {
        mColorMatrix[i] = matrix[i];
    }
}

int GLRender::setScale(IVideoRender::Scale scale)
{
    AF_LOGD("-----> setScale");
    mScale = scale;
    return 0;
}


void GLRender::setBackgroundColor(uint32_t color)
{
    mBackgroundColor = color;
};

int GLRender::onVSync(int64_t tick)
{
    int ret = onVsyncInner(tick);
#ifdef __ANDROID__
    {
        unique_lock<mutex> lock(mRenderCallbackMutex);
        mRenderCallbackCon.notify_one();
    }
#endif
    return ret;
}

int GLRender::onVsyncInner(int64_t tick)
{
    if (mInitRet == INT32_MIN) {
        VSyncOnInit();

        if (mInitRet == INT32_MIN) {
            return 0;
        } else if (mInitRet != 0) {
            AF_LOGE("VSyncOnInit error");
            return -EINVAL;
        }
    }

    if (mHz == 0) {
        mHz = mVSync->getHz();

        if (mHz == 0) {
            mHz = 60;
        }

        mVSyncPeriod = static_cast<int64_t>(1000000 / mHz);
    }

    {
        std::unique_lock<std::mutex> locker(mFrameMutex);

        if (bFlushAsync) {
            while (!mInputQueue.empty()) {
                dropFrame();
            }
            bFlushAsync = false;
        }

        if (!mInputQueue.empty()) {
            if (mInputQueue.size() >= MAX_IN_SIZE) {
                while (mInputQueue.size() >= MAX_IN_SIZE) {
                    dropFrame();
                }

                mRenderClock.set(mInputQueue.front()->getInfo().pts);
                mRenderClock.start();
            } else {
                assert(mInputQueue.front() != nullptr);

                if (mRenderClock.get() == 0) {
                    mRenderClock.set(mInputQueue.front()->getInfo().pts);
                    mRenderClock.start();
                }

                int64_t late = mInputQueue.front()->getInfo().pts - mRenderClock.get();

                if (llabs(late) > 100000) {
                    mRenderClock.set(mInputQueue.front()->getInfo().pts);
                } else if (late - mVSyncPeriod * mRenderClock.getSpeed() > 0) {
//                    AF_LOGD("mVSyncPeriod is %lld\n", mVSyncPeriod);
//                    AF_LOGD("mRenderClock.get() is %lld\n", mRenderClock.get());
//                    AF_LOGD("mInputQueue.front()->getInfo().pts is %lld\n", mInputQueue.front()->getInfo().pts);
                    calculateFPS(tick);
                    return 0;
                }
            }
        }
    }

    if (renderActually()) {
        mRenderCount++;
    }

    calculateFPS(tick);
    return 0;
}

void GLRender::calculateFPS(int64_t tick)
{
    if ((tick / uint64_t(mHz)) != mRendertimeS) {
        if (mRendertimeS == 0 || 1) {
            mRendertimeS = tick / uint64_t(mHz);
        } else {
            mRendertimeS++;
        }

        AF_LOGD("video fps is %llu\n", mRenderCount);
        mFps = mRenderCount;
        mRenderCount = 0;
    }
}

int GLRender::VSyncOnInit()
{
    if (mInBackground) {
        return 0;
    }

    mContext = GLContextFactory::NewInstance();
    mInitRet = mContext->Init(nullptr);
    mInitCondition.notify_all();

    if (mInitRet != 0) {
        AF_LOGE("GLContext init failed. ret = %d ", mInitRet.load());
        return -EINVAL;
    }

    return 0;
}

void GLRender::VSyncOnDestroy()
{
    mPrograms.clear();
    /* B5：program/纹理随 GL context 一起销毁，"就绪"闩必须复位，
     * 否则下一次 getSurface() 会拿到一个已经失效的面。 */
    mOutTextureReady = false;

    if (mContext == nullptr) {
        return;
    }
    if (mClearScreenOn) {
        glClearScreen();
    }

    mContext->DestroyView();
    mContext->DestroySurface(mGLSurface);
    mGLSurface = nullptr;
    mContext->Destroy();
    delete mContext;
    mContext = nullptr;
}

void GLRender::glClearScreen()
{
    glViewport(0, 0, mWindowWidth, mWindowHeight);
    unsigned int backgroundColor = mBackgroundColor;
    float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    cicada::convertToGLColor(backgroundColor, color);
    glClearColor(color[0], color[1], color[2], color[3]);
    glClear(GL_COLOR_BUFFER_BIT);
    mContext->Present(mGLSurface);
}

bool GLRender::renderActually()
{
    if (mContext == nullptr) {
        return false;
    }

    if (mInBackground) {
        //        AF_LOGD("renderActurally  .. InBackground ..");
        return false;
    }

    //  AF_LOGD("renderActually .");
    bool rendered  = true;
    int64_t renderStartTime = af_getsteady_ms();
#ifdef __ANDROID__

    if (needCreateOutTexture) {
        /*
         * program / OES 纹理 / DecoderSurface 都只能在**持有 EGL context 的渲染线程**
         * 上创建（context 在 onVsyncInner → VSyncOnInit 里 makeCurrent）。
         * 这里创建成功后置 mOutTextureReady，getSurface() 的等待者据此判断"面真的有了"。
         * 建不出来（program==nullptr，例如 EGL 初始化失败）时**也要通知**，否则等待者
         * 只能靠超时返回 —— 通知前不置 ready，等待者拿到 nullptr，上层按"没有输出面"处理。
         */
        IProgramContext *programContext = getProgram(AF_PIX_FMT_CICADA_MEDIA_CODEC);

        if (programContext != nullptr) {
            programContext->createSurface();
            mOutTextureReady = (programContext->getSurface() != nullptr);
        } else {
            AF_LOGE("GLRender: could not create the media-codec program on the render thread; the "
                    "video render path will report 'no decoder surface'\n");
        }

        /*
         * ============ 【预热】软解用的 YUV 程序也在这里先建好 ============
         *
         * 为什么必须现在建：GL 程序**第一次** initProgram() 要真编译/链接着色器，Android 驱动
         * 只有在编译过一次之后才会把它写进 App 私有 shader cache。而 getProgram() 平时是
         * **懒创建**（第一次真正用到该格式时才编译）—— 实测表现就是：
         *   · 首次安装后打开第一个视频，**第一次 seek** 卡住一段时间后自己恢复；
         *   · 之后（包括杀进程重开）都不卡，只有卸载/清数据才会重新出现；
         *   · Qt 侧走 D3D11VA，着色器路径完全不同 ⇒ 从来不受影响。
         * 放在这里而不是等按需触发，有三个好处：
         *   ① 位置正确：已在渲染线程、EGL context 已 makeCurrent（program/纹理只能在
         *      这个线程建，见上面 needCreateOutTexture 的说明）；
         *   ② 成本被藏起来：此刻正处在"起播/首帧"这个用户本来就在等加载的窗口里；
         *   ③ 一次覆盖全部后续路径：seek、切档、软硬解切换都不再触发冷编译，
         *      首帧渲染本身也会更快。
         * 失败不改变任何行为：返回 nullptr 就当作没预热，后续仍走原来的懒创建路径。
         * 回退：删掉下面这个 if 块即可恢复"按需懒创建"的旧行为。
         */
        if (getProgram(AF_PIX_FMT_YUV420P) == nullptr) {
            AF_LOGW("GLRender: YUV program pre-warm failed, it will be created lazily as before\n");
        }

        std::unique_lock<std::mutex> locker(mCreateOutTextureMutex);
        needCreateOutTexture = false;
        mCreateOutTextureCondition.notify_all();
    }

#endif
    if (mInvalid) {
        return false;
    }
    bool displayViewChanged  = false;
    bool viewSizeChanged = false;
    {
        unique_lock<mutex> viewLock(mViewMutex);
        displayViewChanged = mContext->SetView(mDisplayView);
        viewSizeChanged = mContext->IsViewSizeChanged();

        if (viewSizeChanged || displayViewChanged
                || (mGLSurface == nullptr && mDisplayView != nullptr)) {
            createGLSurface();
        } else {
            mContext->MakeCurrent(mGLSurface);
        }
    }
    mWindowWidth = mContext->GetWidth();
    mWindowHeight = mContext->GetHeight();

    if (mGLSurface == nullptr || mInvalid) {

        std::unique_lock<std::mutex> locker(mFrameMutex);
        if (!mInputQueue.empty()) {
            dropFrame();
        }

        return false;
    }

    std::unique_ptr<IAFFrame> frame = nullptr;
    {
        std::unique_lock<std::mutex> locker(mFrameMutex);

        if (!mInputQueue.empty()) {
            frame = move(mInputQueue.front());
            mInputQueue.pop();
        } else {
            rendered = false;
        }
    }

    if (frame != nullptr) {
        mProgramFormat = frame->getInfo().video.format;
        mProgramContext = getProgram(mProgramFormat, frame.get());
    }

    if (mProgramContext == nullptr) {
        mProgramFormat = -1;
        return false;
    }

    int64_t framePts = INT64_MIN;

    if (frame != nullptr) {
        framePts = frame->getInfo().pts;
        mVideoInfo = frame->getInfo();
        mVideoRotate = getRotate(frame->getInfo().video.rotate);
    }

    Rotate finalRotate = Rotate_None;
    int tmpRotate = (mRotate + mVideoRotate) % 360;

    if (tmpRotate == 0) {
        finalRotate = Rotate_None;
    } else if (tmpRotate == 90) {
        finalRotate = Rotate_90;
    } else if (tmpRotate == 180) {
        finalRotate = Rotate_180;
    } else if (tmpRotate == 270) {
        finalRotate = Rotate_270;
    }

    mProgramContext->updateScale(mScale);
    mProgramContext->updateRotate(finalRotate);
    mProgramContext->updateWindowSize(mWindowWidth, mWindowHeight, displayViewChanged);
    mProgramContext->updateFlip(mFlip);
    /*
     * 【色觉辅助滤镜 / 回退点 R3】与上一行 updateFlip 并排：**每帧**都下发（和 mFlip 一样）。
     * 好处是程序在 getProgram() 里被缓存/复用时（同格式复用旧 program，不会重新初始化）
     * 也一定带上当前矩阵；「硬解 OES ↔ 软解 YUV」切格式后的第一帧同样带上。
     * 注意 captureScreen 抓的是**加滤镜之前**的画面（它读的是这里的 framebuffer 结果，
     * 而这个矩阵是画上去之前乘的）——截图不带滤镜是预期行为，和 Qt 截图链路同源。
     */
    {
        /*
         * 【回退点 R3-b】锁内只做一次 9 float 拷贝（与 setColorMatrix 成对：
         * 保证这里拿到的是**同一代**写入的整组系数，不会半新半旧）；
         * updateColorMatrix 在锁外调用 —— GL 调用绝不进锁。
         */
        float colorMatrix[9];
        {
            std::lock_guard<std::mutex> locker(mColorMatrixMutex);

            for (int i = 0; i < 9; i++) {
                colorMatrix[i] = mColorMatrix[i];
            }
        }
        mProgramContext->updateColorMatrix(colorMatrix);
    }
    mProgramContext->updateBackgroundColor(mBackgroundColor);
    int ret = -1;
    if (mScreenCleared && frame == nullptr) {
        //do not draw last frame when need clear screen.
        if (viewSizeChanged || displayViewChanged) {
            glClearScreen();
        }
    } else {
        mScreenCleared = false;
        ret = mProgramContext->updateFrame(frame);
    }
    //work around for glReadPixels is upside-down.
    {
        std::unique_lock<std::mutex> locker(mCaptureMutex);

        if (mCaptureOn && mCaptureFunc != nullptr) {
            //if need capture , update flip and other
            if (mFlip == Flip_None ) {
                mProgramContext->updateFlip(Flip_Vertical);
            } else if (mFlip == Flip_Vertical) {
                mProgramContext->updateFlip(Flip_None);
            } else if ( mFlip == Flip_Horizontal) {
                mProgramContext->updateFlip(Flip_Both);
            }

            if (finalRotate == Rotate_90) {
                mProgramContext->updateRotate(Rotate_270);
            } else if (finalRotate == Rotate_270) {
                mProgramContext->updateRotate(Rotate_90);
            }

            std::unique_ptr<IAFFrame> dummyFrame = nullptr;
            mProgramContext->updateFrame(dummyFrame);
            captureScreen();
            //reset flip and other
            mProgramContext->updateFlip(mFlip);
            mProgramContext->updateRotate(finalRotate);
            mProgramContext->updateFrame(dummyFrame);
        }
    }

    if (ret == 0) {
        //if frame not change, don`t need present surface
        mContext->Present(mGLSurface);
        if (mListener) {
            mListener->onFrameInfoUpdate(mVideoInfo, true);
        }
    }

    if (mClearScreenOn) {
        glClearScreen();
        mScreenCleared = true;
        mClearScreenOn = false;
    }

    int64_t end = af_getsteady_ms();

    if (end - renderStartTime > 100) {
        AF_LOGD("renderActually use:%lld", end - renderStartTime);
    }

//   AF_LOGD(" cost time : render = %d ms", (af_getsteady_ms() - renderStartTime));
    return rendered;
}

void GLRender::captureScreen()
{
    int64_t captureStartTime = af_getsteady_ms();
    GLint pView[4];
    glGetIntegerv(GL_VIEWPORT, pView);
    int width = pView[2];
    int height = pView[3];
    GLsizei bufferSize = width * height * sizeof(GLubyte) * 4; //RGBA
    GLubyte *bufferData = (GLubyte *) malloc(bufferSize);
    memset(bufferData, 0, bufferSize);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glReadPixels(pView[0], pView[1], pView[2], pView[3], GL_RGBA, GL_UNSIGNED_BYTE,
                 bufferData);
    int64_t captureEndTime = af_getsteady_ms();
    AF_LOGD("capture cost time : capture = %d ms", (captureEndTime - captureStartTime));
    mCaptureFunc(bufferData, width, height);
    free(bufferData);
    mCaptureOn = false;
}

int GLRender::setDisPlay(void *view)
{
    AF_LOGD("-----> setDisPlay view = %p", view);

    if (mDisplayView != view) {
        mVSync->pause();
        {
            unique_lock<mutex> viewLock(mViewMutex);
            mDisplayView = view;
        }
        std::unique_lock<std::mutex> locker(mInitMutex);

        if (!mInBackground) {
            mVSync->start();
        }
    }

    return 0;
}

void GLRender::createGLSurface()
{
    if (mContext == nullptr) {
        return;
    }

    GLContext *pGLContext = mContext;
    pGLContext->DestroySurface(mGLSurface);
    pGLContext->MakeCurrent(nullptr);
    mGLSurface = pGLContext->CreateSurface();

    if (mGLSurface == nullptr) {
        AF_LOGE("createGLSurface fail ");
    }

    pGLContext->MakeCurrent(mGLSurface);
}

void GLRender::captureScreen(std::function<void(uint8_t *, int, int)> func)
{
    {
        std::unique_lock<std::mutex> locker(mCaptureMutex);
        mCaptureFunc = func;
        mCaptureOn = true;
    }
}

void *GLRender::getSurface(bool cached)
{
#ifdef __ANDROID__
    /*
     * ============ 【B5 修：program / OES 纹理只能由渲染线程创建】============
     *
     * 旧写法第一句就是 `getProgram(AF_PIX_FMT_CICADA_MEDIA_CODEC)` —— 那会在**调用线程**
     * （播放线程：SuperMediaPlayer::CreateVideoDecoder 里的 `getSurface()`）上
     * new OESProgramContext + initProgram()，而此刻 EGL context 只由渲染线程
     * makeCurrent（onVsyncInner → VSyncOnInit）⇒ glCreateProgram / glCompileShader
     * 一律失败，每次冷启动都刷一条
     *   `[GLRender_OESContext] compileShader mVertShader failed. ret = -1`
     * 的 E 级日志（真机 2026-09-25 11:02:16.412 那条就是它），而且白做一次无用功。
     *
     * 现在：调用线程只"向渲染线程要"，创建一律发生在渲染线程（context 一定 current）；
     * 渲染线程建好后置 mOutTextureReady。调用线程拿到的必然是已建好并缓存的那个
     * program，因此这里**不再调用 getProgram()**（它会在 cache miss 时触发创建）。
     *
     * 等待有上限（2s）：渲染线程若因 GL 初始化失败而永远建不出来，这里返回 nullptr，
     * 由上层按"没有解码器输出面"处理，而不是把播放线程无限挂住（旧写法是无超时 wait）。
     */
    if (!mOutTextureReady.load() || !cached) {
        std::unique_lock<std::mutex> locker(mCreateOutTextureMutex);
        needCreateOutTexture = true;

        if (!mCreateOutTextureCondition.wait_for(locker, std::chrono::seconds(2),
                                                 [this]() { return !needCreateOutTexture; })) {
            needCreateOutTexture = false;
            AF_LOGE("GLRender::getSurface timed out after 2s waiting for the render thread to create the "
                    "decoder surface — returning nullptr instead of blocking the caller forever\n");
            return nullptr;
        }
    }

    if (!mOutTextureReady.load()) {
        return nullptr;
    }

    /* 只读缓存（不创建）：见上面说明，创建只发生在渲染线程。 */
    auto it = mPrograms.find(AF_PIX_FMT_CICADA_MEDIA_CODEC);

    if (it != mPrograms.end() && it->second != nullptr) {
        return it->second->getSurface();
    }

#endif
    return nullptr;
}

IProgramContext *GLRender::getProgram(int frameFormat, IAFFrame *frame)
{
    if (mPrograms.count(frameFormat) > 0) {
        IProgramContext *pContext = mPrograms[frameFormat].get();
        pContext->setRenderingCb(mRenderingCb, mRenderingCbUserData);
        pContext->setVideoProcessTextureCb(mProcessTextureCb);
        if (mContext != nullptr) {
            pContext->setGLContext(mContext->GetContext());
        }
        pContext->useProgram();
        return pContext;
    }

    unique_ptr<IProgramContext> targetProgram{nullptr};
#ifdef __ANDROID__

    if (frameFormat == AF_PIX_FMT_CICADA_MEDIA_CODEC) {
        targetProgram = unique_ptr<IProgramContext>(new OESProgramContext());
    } else
#elif TARGET_OS_IPHONE
    if (frameFormat == AF_PIX_FMT_APPLE_PIXEL_BUFFER && frame != nullptr) {
        CVPixelBufferRef pixelBuffer = (dynamic_cast<PBAFFrame *>(frame))->getPixelBuffer();
        OSType pixelFormat = CVPixelBufferGetPixelFormatType(pixelBuffer);

        if (pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                || pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
            targetProgram = unique_ptr<IProgramContext>(new CV420PProgramContext(mContext->GetContext()));
        }
    } else
#endif
        if (frameFormat == AF_PIX_FMT_YUV420P || frameFormat == AF_PIX_FMT_YUVJ420P
                || frameFormat == AF_PIX_FMT_YUV422P || frameFormat == AF_PIX_FMT_YUVJ422P) {
            targetProgram = unique_ptr<IProgramContext>(new YUVProgramContext());
        }

    if (targetProgram == nullptr) {
        return nullptr;
    }

    targetProgram->setVideoProcessTextureCb(mProcessTextureCb);
    int ret = targetProgram->initProgram();

    if (ret == 0) {
        targetProgram->setRenderingCb(mRenderingCb, mRenderingCbUserData);
        if (mContext != nullptr) {
            targetProgram->setGLContext(mContext->GetContext());
        }
        mPrograms[frameFormat] = move(targetProgram);
        return mPrograms[frameFormat].get();
    } else {
        return nullptr;
    }
}



void GLRender::setSpeed(float speed)
{
    mRenderClock.setSpeed(speed);
}




#if TARGET_OS_IPHONE

void GLRender::AppWillResignActive()
{
    std::unique_lock<std::mutex> locker(mInitMutex);
    mInBackground = true;
    AF_LOGE("0919, mInBackground = true");
    mVSync->pause();
};

void GLRender::AppDidBecomeActive()
{
    std::unique_lock<std::mutex> locker(mInitMutex);
    mInBackground = false;
    AF_LOGE("0919, mInBackground = false");
    mVSync->start();
}

#endif

float GLRender::getRenderFPS()
{
    return mFps;
}

void GLRender::surfaceChanged()
{
#ifdef __ANDROID__

    if (mInitRet == INT32_MIN || mInitRet != 0) {
        return ;
    }

    std::unique_lock<mutex> lock(mRenderCallbackMutex);
    mRenderCallbackCon.wait(lock);
#endif
}
