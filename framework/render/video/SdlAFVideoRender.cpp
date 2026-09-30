//
// Created by lifujun on 2019/7/23.
//

#define  LOG_TAG "SdlAFVideoRender"

#include "SdlAFVideoRender.h"
#include <base/media/AVAFPacket.h>
#include <render/video/vsync/VSyncFactory.h>
#include <thread>
#include <utils/frame_work_log.h>
#include <utils/globalSettings.h>
#include <utils/property.h>
#include <cstdlib>
#include <string>
#ifdef __APPLE__
#include <base/media/PBAFFrame.h>
#endif

static int SDLCALL SdlWindowSizeEventWatch(void *userdata, SDL_Event *event);

static void sdlLogCb(void *userdata, int category, SDL_LogPriority priority, const char *message)
{
    AF_LOGI("sdl log: %d %d %s", category, priority, message);
}

static float getRequestedRenderHz()
{
    /* 属性是浮点字符串（例如 "119.88"）；没设、解析失败、不是正数都当作"不改"。 */
    const std::string value =
        Cicada::globalSettings::getSetting().getProperty(PROPERTY_KEY_VIDEO_RENDER_HZ);

    if (value.empty()) {
        return 0;
    }

    const float hz = static_cast<float>(atof(value.c_str()));
    return (hz > 1.0f && hz < 1000.0f) ? hz : 0;
}

SdlAFVideoRender::SdlAFVideoRender()
{
    mVSync = VSyncFactory::create(*this, 60);
//   mHz = 0;
    SDL_LogSetAllPriority(SDL_LOG_PRIORITY_VERBOSE);
    SDL_LogSetOutputFunction(sdlLogCb, nullptr);
    mVSync->start();
};

SdlAFVideoRender::~SdlAFVideoRender()
{
    if (mVideoTexture != nullptr) {
        SDL_DestroyTexture(mVideoTexture);
        mVideoTexture = nullptr;
        mInited = false;
    }
    if (mVideoNv12Texture != nullptr) {
        SDL_DestroyTexture(mVideoNv12Texture);
        mVideoNv12Texture = nullptr;
        mVideoNv12Width = 0;
        mVideoNv12Height = 0;
    }
    if (mRenderNeedRelease) {
        SDL_DelEventWatch(SdlWindowSizeEventWatch, this);
        SDL_DestroyRenderer(mVideoRender);
        mVideoRender = nullptr;
        mRenderNeedRelease = false;
    }
    if (mWindowNeedRelease && mVideoWindow) {
        SDL_DestroyWindow(mVideoWindow);
        mVideoWindow = nullptr;
        mWindowNeedRelease = false;
    }

    if (mVideoSubSystemInited) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
}


int SdlAFVideoRender::init()
{
    int initRet = 0;

    if (initRet < 0) {
        AF_LOGE("SdlAFVideoRender could not initialize! Error: %s\n", SDL_GetError());
        return initRet;
    }

    if (mVideoWindow == nullptr) {
        AF_LOGE("Window could not be created! SDL_Error: %s\n", SDL_GetError());
        return -1;
    }

    if (mVideoRender == nullptr) {
        AF_LOGE("Render could not be created! SDL_Error: %s\n", SDL_GetError());
        return -1;
    }

    int renderWidth = 0;
    int renderHeight = 0;
    SDL_GL_GetDrawableSize(mVideoWindow, &mWindowWidth, &mWindowHeight);
    SDL_GetRendererOutputSize(mVideoRender, &renderWidth, &renderHeight);
    float DISPLAY_WIDTH_RATIO = renderWidth * 1.0f / mWindowWidth;
    float DISPLAY_HEIGHT_RATIO = renderHeight * 1.0f / mWindowHeight;
    SDL_RenderSetScale(mVideoRender, DISPLAY_WIDTH_RATIO, DISPLAY_HEIGHT_RATIO);
    SDL_setenv(SDL_HINT_RENDER_SCALE_QUALITY, "linear", 0);
    return 0;
}


int SdlAFVideoRender::refreshScreen()
{
    bool needClearScreen = false;
    {
        std::unique_lock<std::mutex> lock(mRenderMutex);

        if (mLastVideoFrame == nullptr && mBackFrame != nullptr) {
            mLastVideoFrame = mBackFrame->clone();
        }
        if (mLastVideoFrame == nullptr) {
            needClearScreen = true;
        }
    }
    if (needClearScreen) {
        clearScreen();
    } else {
        onVSync(-1);
    }
    return 0;
}

void SdlAFVideoRender::delayRefreshScreen()
{
    std::thread thread([=]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        {
            std::unique_lock<std::mutex> lock(mRenderMutex);
            if (mLastVideoFrame == nullptr && mBackFrame != nullptr) {
                mLastVideoFrame = mBackFrame->clone();
            }
        }
    });
    thread.detach();
}


int SdlAFVideoRender::clearScreen()
{
    std::unique_lock<std::mutex> lock(mRenderMutex);

    if (mVideoRender != nullptr && mVideoTexture != nullptr) {
        SDL_SetRenderDrawColor(mVideoRender, 0, 0, 0, 255);
        SDL_RenderClear(mVideoRender);
        SDL_RenderPresent(mVideoRender);
    }

    /*
     * 【本轮修：这里原来只放 mBackFrame，不放 mLastVideoFrame —— 与 Qt 侧同一个形状】
     *
     * mLastVideoFrame 是"停在屏上的最后一帧"（见 renderFrame 里那句
     * `mLastVideoFrame = std::move(frame)`）。它一直握着那一帧的 buf[]，而硬解帧的
     * buf[] 里握着**解码器的输出缓冲/表面池**。原来的写法只有 `mBackFrame = nullptr`，
     * 于是"调用方要求清屏（停播 / 换源 / 关闭）"之后，最后一帧连同它钉着的解码输出
     * 仍然活着，一直等到**下一次播放**换掉它、或者渲染器析构才放 —— 这就是 Qt 侧
     * `externally pinned=1`、释放被推迟到下一轮那个缺陷的同一形状。
     *
     * 清屏的语义本来就是"屏上不要留东西"，留着最后一帧是自相矛盾的，所以在这里放掉它
     * 既符合语义、又让释放点变成**调用方一要求就放**（确定性）。
     *
     * 为什么不会 use-after-free：mLastVideoFrame 的所有读者/写者
     * （renderFrame / onVSync / 本函数）都在 mRenderMutex 之下，本函数已经持锁，
     * 所以不存在与渲染/VSync 线程的竞争。
     */
    if (mLastVideoFrame) {
        mLastVideoFrame->setDiscard(true);
        mLastVideoFrame = nullptr;
    }

    mBackFrame = nullptr;

    return 0;
}

void SdlAFVideoRender::releaseFrames()
{
    /*
     * 只做一件事：把对解码帧的两处引用放下（mLastVideoFrame = 还没上屏的那一帧，
     * mBackFrame = 上过屏、留着给 refreshScreen() 复用的那一帧）。
     *
     * 为什么不直接调 clearScreen()：本函数被调用的时刻是"解码器马上要关/重建"，
     * 那时清屏（SDL_RenderClear + Present）会在切档 / seek 上闪一下黑，而这一帧的
     * 引用是**必须**在解码器之前放掉的（硬解帧钉着整池 surface，见
     * IVideoRender::releaseFrames()）。两件事分开，调用方各取所需。
     *
     * 线程安全：与 renderFrame / onVSync / clearScreen 同一把 mRenderMutex；
     * 这里不需要暂停 VSync —— 只是把两处 unique_ptr 置空，任何时刻做都安全
     * （onVSync 取帧时会发现 mLastVideoFrame 为空，直接返回不渲染）。
     */
    std::unique_lock<std::mutex> lock(mRenderMutex);

    unsigned long long dropped = 0;

    if (mLastVideoFrame) {
        mLastVideoFrame->setDiscard(true);
        mLastVideoFrame = nullptr;
        dropped++;
    }

    if (mBackFrame) {
        mBackFrame->setDiscard(true);
        mBackFrame = nullptr;
        dropped++;
    }

    AF_LOGI("[mem] releaseFrames: dropped %llu decoded frame(s) held by the renderer "
            "(last + back frame)\n", dropped);
}

int SdlAFVideoRender::renderFrame(std::unique_ptr<IAFFrame> &frame)
{
    {

        bool paused = false;
        bool flushRequested = false;
        if (frame == nullptr) {
            mVSync->pause();
            paused = true;
            flushRequested = true;
        }
        {
            std::unique_lock<std::mutex> lock(mRenderMutex);
            if (mLastVideoFrame) {
                mLastVideoFrame->setDiscard(true);
                if (mListener) {
                    mListener->onFrameInfoUpdate(mLastVideoFrame->getInfo(), false);
                }
            }
            mLastVideoFrame = std::move(frame);

            /*
             * 【flush 必须把手上的帧放干净】frame == nullptr 就是"清空视频路"这个
             * 既有语义（flushVideoRender() 用的就是它），而 mBackFrame 是**比
             * mLastVideoFrame 活得久**的那一份（onVSyncInner 末尾
             * `mBackFrame = move(frame)`）—— 原来那次 flush 只放 mLastVideoFrame，
             * 于是"放完"之后渲染器手里还留着一帧旧解码器的输出：切档时它钉着旧池，
             * 关窗时它钉到渲染器析构为止。既然是 flush，就把两帧一起放掉。
             *
             * 副作用（刻意）：flush 之后 refreshScreen()/delayRefreshScreen() 不再
             * "把最后一帧重新贴回屏上" —— flush 的语义本来就是屏上不留旧内容
             * （clearScreen() 早就同时放这两帧了）。
             */
            if (flushRequested && mBackFrame) {
                mBackFrame->setDiscard(true);
                mBackFrame = nullptr;
            }

            if (mLastVideoFrame && mVideoRotate != getRotate(mLastVideoFrame->getInfo().video.rotate)) {
                mVideoRotate = getRotate(mLastVideoFrame->getInfo().video.rotate);
            }
        }
        if (paused) {
            mVSync->start();
        }
    }
//    int width = frame->getInfo().video.width;
//    int height = frame->getInfo().video.height;
//    AF_LOGD("video width = %d , height = %d", width, height);
    return 0;
}


void SdlAFVideoRender::recreateTextureIfNeed(int videoWidth, int videoHeight)
{
    if (mInited && videoHeight == mVideoHeight && videoWidth == mVideoWidth) {
//        AF_LOGE("SdlAFVideoRender has initialize!");
        return;
    }

    {
        std::unique_lock<std::mutex> lock(mRenderMutex);
        if (mVideoRender == nullptr) {
            return;
        }

        mInited = true;

        if (mVideoTexture != nullptr) {
            SDL_DestroyTexture(mVideoTexture);
            mVideoTexture = nullptr;
        }

        if (mVideoTexture == nullptr) {
            mVideoTexture = SDL_CreateTexture(mVideoRender, SDL_PIXELFORMAT_IYUV,
                                              SDL_TEXTUREACCESS_STREAMING,
                                              videoWidth, videoHeight);
        }

        if (mVideoTexture == nullptr) {
            AF_LOGE("Texture could not be created! SDL_Error: %s\n", SDL_GetError());
            return;
        }

        mVideoWidth = videoWidth;
        mVideoHeight = videoHeight;
    }
}

int SdlAFVideoRender::onVSync(int64_t tick)
{
    int ret = onVSyncInner(tick);
#ifdef __WINDOWS__
    {
        std::unique_lock<std::mutex> lock(mWindowSizeChangeMutex);
        mWindowSizeChangeCon.notify_one();
    }
#endif
    return ret;
}

int SdlAFVideoRender::onVSyncInner(int64_t tick)
{
    /*
     * 回调频率：默认 60Hz（配合 SDL 自己的呈现）。应用把
     * PROPERTY_KEY_VIDEO_RENDER_HZ 设成显示器刷新率后跟着它走——零拷贝直通
     * 时呈现由呈现方按刷新率节流，回调格子细一点，高刷屏上的 24fps 片源才不
     * 会因为落在 60Hz 的格子上而出现 3:2 抖动。只读一次（第一次回调时属性
     * 一定已经设好了）。
     */
    if (!mRenderHzApplied) {
        mRenderHzApplied = true;
        const float hz = getRequestedRenderHz();

        if (hz > 0) {
            mVSync->setHz(hz);
            AF_LOGI("render callback rate set to %.2f Hz\n", hz);
        }
    }

    std::unique_ptr<IAFFrame> frame;
    {
        std::unique_lock<std::mutex> lock(mRenderMutex);
        if (mLastVideoFrame == nullptr) {
            return 0;
        }

        frame = move(mLastVideoFrame);
    }
#ifdef __APPLE__
    auto *pBFrame = dynamic_cast<PBAFFrame *>(frame.get());
    if (pBFrame) {
        auto *avafFrame = static_cast<AVAFFrame *>(*pBFrame);
        if (avafFrame) {
            frame = std::unique_ptr<IAFFrame>(avafFrame);
        } else {
            return 0;
        }
    }
#endif
    bool rendered = false;
    if (mRenderingCb) {
        std::unique_lock<std::mutex> lock(mRenderMutex);
        CicadaJSONItem params{};
        rendered = mRenderingCb(mRenderingCbUserData, frame.get(), params);
    }
    if (!rendered) {
        IAFFrame::videoInfo &videoInfo = frame->getInfo().video;
        /*
         * mVideoWidth/mVideoHeight 只由 recreateTextureIfNeed() 维护，而 NV12
         * 直通根本不用那张 IYUV 纹理，所以那条路径下必须用帧自己的尺寸，
         * 否则 srcRect 会是 0x0，更新等于没做（同样是整屏绿色）。
         */
        const bool nv12Frame = (videoInfo.format == AF_PIX_FMT_NV12);

        if (!nv12Frame) {
            recreateTextureIfNeed(videoInfo.width, videoInfo.height);
        }

        SDL_Rect srcRect{};
        srcRect.x = 0;
        srcRect.y = 0;
        srcRect.w = nv12Frame ? videoInfo.width : mVideoWidth;
        srcRect.h = nv12Frame ? videoInfo.height : mVideoHeight;
        int angle = (mRotate + mVideoRotate) % 360;
        SDL_RendererFlip flip = convertFlip();
        SDL_Rect dstRect = getDestRet();
        {
            std::unique_lock<std::mutex> lock(mRenderMutex);
            uint8_t **data = frame->getData();
            int *lineSize = frame->getLineSize();

            if (mVideoRender != nullptr && data != nullptr && lineSize != nullptr && data[0] != nullptr) {
                /*
                 * 硬解 copy-back 直通时解码器交来的是 NV12：两块平面（Y 一块、
                 * UV 交错一块），用 SDL_UpdateYUVTexture() 传三个平面会直接
                 * 失败（"Parameter 'Vplane' is invalid"），纹理保持全零，
                 * 而 Y=U=V=0 恰好就是整屏绿色。所以这里按格式分开处理。
                 */
                if (videoInfo.format == AF_PIX_FMT_NV12 && data[1] != nullptr) {
                    if (mVideoNv12Texture == nullptr ||
                        mVideoNv12Width != videoInfo.width || mVideoNv12Height != videoInfo.height) {
                        if (mVideoNv12Texture != nullptr) {
                            SDL_DestroyTexture(mVideoNv12Texture);
                            mVideoNv12Texture = nullptr;
                        }

                        /* SDL 的 D3D11 后端用 UpdateSubresource 更新 NV12，
                         * 需要 DEFAULT 用法，所以先按 STATIC 建，不行再退。 */
                        mVideoNv12Texture = SDL_CreateTexture(mVideoRender, SDL_PIXELFORMAT_NV12,
                                                              SDL_TEXTUREACCESS_STATIC,
                                                              videoInfo.width, videoInfo.height);

                        if (mVideoNv12Texture == nullptr) {
                            AF_LOGW("SDL_PIXELFORMAT_NV12 with STATIC access failed (%s), trying STREAMING\n",
                                    SDL_GetError());
                            mVideoNv12Texture = SDL_CreateTexture(mVideoRender, SDL_PIXELFORMAT_NV12,
                                                                  SDL_TEXTUREACCESS_STREAMING,
                                                                  videoInfo.width, videoInfo.height);
                        }

                        mVideoNv12Width = videoInfo.width;
                        mVideoNv12Height = videoInfo.height;

                        if (mVideoNv12Texture == nullptr) {
                            AF_LOGE("NV12 texture could not be created! SDL_Error: %s\n", SDL_GetError());
                        }
                    }

                    if (mVideoNv12Texture != nullptr) {
                        if (SDL_UpdateNVTexture(mVideoNv12Texture, &srcRect,
                                                data[0], lineSize[0], data[1], lineSize[1]) != 0) {
                            AF_LOGE("SDL_UpdateNVTexture failed: %s\n", SDL_GetError());
                        }

                        SDL_RenderClear(mVideoRender);
                        SDL_RenderCopyEx(mVideoRender, mVideoNv12Texture, &srcRect, &dstRect, angle, nullptr, flip);
                        SDL_RenderPresent(mVideoRender);
                    }
                } else if (mVideoTexture != nullptr) {
                    SDL_UpdateYUVTexture(mVideoTexture, &srcRect, data[0], lineSize[0], data[1], lineSize[1], data[2], lineSize[2]);
                    SDL_RenderClear(mVideoRender);
                    SDL_RenderCopyEx(mVideoRender, //SDL_Renderer*          renderer,
                                     mVideoTexture,//SDL_Texture*           texture,
                                     &srcRect,     //const SDL_Rect*        srcrect,
                                     &dstRect,     //const SDL_Rect*        dstrect,
                                     angle,        //const double           angle,
                                     nullptr,      //const SDL_Point*       center,
                                     flip          //const SDL_RendererFlip flip
                    );
                    SDL_RenderPresent(mVideoRender);
                }
            }
        }
    }
    {
        std::unique_lock<std::mutex> lock(mRenderMutex);
        if (mListener && frame) {
            mListener->onFrameInfoUpdate(frame->getInfo(), true);
        }
        mBackFrame = move(frame);
    }
    return 0;
}


int SdlAFVideoRender::setRotate(Rotate rotate)
{
    mRotate = rotate;
    refreshScreen();
    return 0;
}

int SdlAFVideoRender::setFlip(Flip flip)
{
    mFlip = flip;
    refreshScreen();
    return 0;
}

int SdlAFVideoRender::setScale(Scale scale)
{
    mScale = scale;
    refreshScreen();
    return 0;
}

SDL_Rect SdlAFVideoRender::getDestRet()
{
    SDL_Rect dstRect{};
    dstRect.x = 0;
    dstRect.y = 0;
    dstRect.w = 0;
    dstRect.h = 0;
    if (mVideoWindow == nullptr) {
        return dstRect;
    }
    SDL_GL_GetDrawableSize(mVideoWindow, &mWindowWidth, &mWindowHeight);
    if (mWindowWidth == 0 || mWindowHeight == 0 ||
            mVideoWidth == 0 || mVideoHeight == 0) {
        return dstRect;
    }

    float videoWhRatio = 1.0f * mVideoWidth / mVideoHeight;
    float windowWhRatio = 1.0f * mWindowWidth / mWindowHeight;
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

    if (videoWhRatio <= windowWhRatio) {
        if (finalRotate == Rotate_None || finalRotate == Rotate_180) {
            if (mScale == Scale::Scale_AspectFit) {
                dstRect.h = mWindowHeight;
                dstRect.w = static_cast<int>(dstRect.h * videoWhRatio);
            } else if (mScale == Scale_AspectFill) {
                dstRect.w = mWindowWidth;
                dstRect.h = static_cast<int>(dstRect.w / videoWhRatio);
            } else if (mScale == Scale::Scale_Fill) {
                dstRect.h = mWindowHeight;
                dstRect.w = mWindowWidth;
            }
        } else if (finalRotate == Rotate_90 || finalRotate == Rotate_270) {
            if (mScale == Scale::Scale_AspectFit) {
                dstRect.w = mWindowHeight;
                dstRect.h = static_cast<int>( dstRect.w / videoWhRatio);
            } else if (mScale == Scale_AspectFill) {
                dstRect.h = mWindowWidth;
                dstRect.w = static_cast<int>( dstRect.h * videoWhRatio);
            } else if (mScale == Scale::Scale_Fill) {
                dstRect.h = mWindowWidth;
                dstRect.w = mWindowHeight;
            }
        }
    } else {
        if (finalRotate == Rotate_None || finalRotate == Rotate_180) {
            if (mScale == Scale::Scale_AspectFit) {
                dstRect.w = mWindowWidth;
                dstRect.h = static_cast<int>(dstRect.w / videoWhRatio);
            } else if (mScale == Scale_AspectFill) {
                dstRect.h = mWindowHeight;
                dstRect.w = static_cast<int>(dstRect.h * videoWhRatio);
            } else if (mScale == Scale::Scale_Fill) {
                dstRect.h = mWindowHeight;
                dstRect.w = mWindowWidth;
            }
        } else if (finalRotate == Rotate_90 || finalRotate == Rotate_270) {
            if (mScale == Scale::Scale_AspectFit) {
                dstRect.h = mWindowWidth;
                dstRect.w = static_cast<int>(dstRect.h * videoWhRatio);
            } else if (mScale == Scale_AspectFill) {
                dstRect.w = mWindowHeight;
                dstRect.h = static_cast<int>(dstRect.w / videoWhRatio);
            } else if (mScale == Scale::Scale_Fill) {
                dstRect.h = mWindowWidth;
                dstRect.w = mWindowHeight;
            }
        }
    }

    dstRect.y = (mWindowHeight - dstRect.h) / 2;
    dstRect.x = (mWindowWidth - dstRect.w) / 2;
    return dstRect;
}

SDL_RendererFlip SdlAFVideoRender::convertFlip()
{
    SDL_RendererFlip flip = SDL_FLIP_NONE;

    if (mFlip == Flip_None) {
        flip = SDL_FLIP_NONE;
    } else if (mFlip == Flip_Horizontal) {
        flip = SDL_FLIP_HORIZONTAL;
    } else if (mFlip == Flip_Vertical) {
        flip = SDL_FLIP_VERTICAL;
    }

    return flip;
}

void SdlAFVideoRender::captureScreen(std::function<void(uint8_t *data, int width, int height)> func)
{
    if (func == nullptr) {
        return;
    }
    {
        std::unique_lock<std::mutex> lock(mRenderMutex);

        if (mVideoRender == nullptr) {
            return;
        }
    }
    SDL_Rect finalRect = getSnapRect();
    SDL_Surface *surface = SDL_CreateRGBSurface(0, finalRect.w, finalRect.h, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);

    if (surface == nullptr) {
        AF_LOGE("Texture could not be created! SDL_Error: %s\n", SDL_GetError());
        return;
    }
    refreshScreen();
    Uint32 surfaceFormat = surface->format->format;
    {
        std::unique_lock<std::mutex> lock(mRenderMutex);
        SDL_RenderReadPixels(mVideoRender, &finalRect, surfaceFormat, surface->pixels, surface->pitch);
    }
    if (surfaceFormat == SDL_PIXELFORMAT_ARGB8888) {
        size_t size = 0;
        uint8_t *pixBuf = nullptr;
        size = static_cast<size_t>(4 * finalRect.w * finalRect.h);
        pixBuf = static_cast<uint8_t *>(malloc(size));
        SDL_memcpy(pixBuf, surface->pixels, size);
        func(pixBuf, finalRect.w, finalRect.h);
        free(pixBuf);
        pixBuf = nullptr;
    } else {
        func(nullptr, 0, 0);
    }
    SDL_FreeSurface(surface);

    return ;
}


SDL_Rect SdlAFVideoRender::getSnapRect()
{
    int width = 0;
    int height = 0;
    SDL_GetRendererOutputSize(mVideoRender, &width, &height);
    float DISPLAY_WIDTH_RATIO = width * 1.0f / mWindowWidth;
    float DISPLAY_HEIGHT_RATIO = height * 1.0f / mWindowHeight;
    SDL_Rect rect = getDestRet();
    int finalW, finalH;

    int angle = (mRotate + mVideoRotate) % 360;

    if (angle == Rotate::Rotate_None || angle == Rotate::Rotate_180) {
        finalW = rect.w;
        finalH = rect.h;
    } else if (angle == Rotate::Rotate_90 || angle == Rotate::Rotate_270) {
        finalW = rect.h;
        finalH = rect.w;
    } else {
        finalW = rect.w;
        finalH = rect.h;
    }

    SDL_Rect finalRect{};
    finalRect.w = static_cast<int>(finalW * DISPLAY_WIDTH_RATIO);
    finalRect.h = static_cast<int>(finalH * DISPLAY_HEIGHT_RATIO);
    finalRect.x = static_cast<int>((mWindowWidth - finalW) / 2 * DISPLAY_WIDTH_RATIO);
    finalRect.y = static_cast<int>((mWindowHeight - finalH) / 2 * DISPLAY_HEIGHT_RATIO);
    return finalRect;
}

int SdlAFVideoRender::setDisPlay(void *view)
{
    if (view == nullptr) {
        return 0;
    }
    auto *display = static_cast<CicadaSDLView *>(view);
    if (mCurrentView == display->view) {
        return 0;
    }
    mCurrentView = display->view;
    if (mCurrentView == nullptr) {
        return 0;
    }

    if (!mVideoSubSystemInited) {
        SDL_InitSubSystem(SDL_INIT_VIDEO);
        mVideoSubSystemInited = true;
    }

    if (mVideoTexture != nullptr) {
        SDL_DestroyTexture(mVideoTexture);
        mVideoTexture = nullptr;
        mInited = false;
    }
    if (mRenderNeedRelease) {
        SDL_DelEventWatch(SdlWindowSizeEventWatch, this);
        SDL_DestroyRenderer(mVideoRender);
        mVideoRender = nullptr;
        mRenderNeedRelease = false;
    }
    if (mWindowNeedRelease && mVideoWindow) {
        SDL_DestroyWindow(mVideoWindow);
        mVideoWindow = nullptr;
        mWindowNeedRelease = false;
    }
    if (display->type == CicadaSDLViewType_NATIVE_WINDOW) {
        mVideoWindow = SDL_CreateWindowFrom(display->view);
        if (mVideoWindow) {
            SDL_ShowWindow(mVideoWindow);
        } else {
            AF_LOGE("SDL_CreateWindowFrom failed! SDL_Error: %s\n", SDL_GetError());
        }
        mWindowNeedRelease = true;
    } else {
        mVideoWindow = static_cast<SDL_Window *>(display->view);
        mWindowNeedRelease = false;
    }

    if (mVideoWindow) {
        mVideoRender = SDL_GetRenderer(mVideoWindow);
        if (mVideoRender == nullptr) {
            // log all render name
            int renderCount = SDL_GetNumRenderDrivers();
            for (int i = 0; i < renderCount; i++) {
                SDL_RendererInfo renderDriverInfo;
                SDL_GetRenderDriverInfo(i, &renderDriverInfo);
                std::string renderDriverName;
                if (renderDriverInfo.name) {
                    renderDriverName = renderDriverInfo.name;
                }
                AF_LOGI("sdl render%d: %s\n", i, renderDriverName.c_str());
            }

            // add before renderer created, so this callback will be called before renderer's window size change callback
            SDL_AddEventWatch(SdlWindowSizeEventWatch, this);
            Uint32 renderFlags = 0;
#ifdef __WINDOWS__
            SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d11");
            renderFlags = SDL_RENDERER_SOFTWARE;
#endif
            mVideoRender = SDL_CreateRenderer(mVideoWindow, -1, renderFlags);

            // log the render name
            SDL_RendererInfo renderInfo;
            SDL_GetRendererInfo(mVideoRender, &renderInfo);
            std::string renderName;
            if (renderInfo.name) {
                renderName = renderInfo.name;
            }
            AF_LOGI("create sdl render: %s\n", renderName.c_str());

            mRenderNeedRelease = true;
        } else {
            mRenderNeedRelease = false;
        }
    }

    return 0;
}

int SDLCALL SdlWindowSizeEventWatch(void *userdata, SDL_Event *event)
{
    if (event->type == SDL_WINDOWEVENT) {
        if (event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            SdlAFVideoRender *pSelf = (SdlAFVideoRender *) userdata;
            if (pSelf) {
                SDL_Window *window = SDL_GetWindowFromID(event->window.windowID);
                pSelf->onWindowSizeChange(window);
            }
        } else if (event->window.event == SDL_WINDOWEVENT_RESIZED) {
            // after SDL_WINDOWEVENT_SIZE_CHANGED event, d3d11 recreate resource, then in this event refresh use new d3d11 resource
            SdlAFVideoRender *pSelf = (SdlAFVideoRender *) userdata;
            if (pSelf) {
                pSelf->delayRefreshScreen();
            }
        }
    }
    return 0;
}

void SdlAFVideoRender::onWindowSizeChange(SDL_Window *window)
{
    if (mVideoWindow == window) {
#ifdef __WINDOWS__
        if (!mInited) {
            return;
        }
        // block the renderer's window size change callback(d3d11 recreate resource) until render complete
        std::unique_lock<std::mutex> lock(mWindowSizeChangeMutex);
        mWindowSizeChangeCon.wait(lock);
#endif
    }
}
