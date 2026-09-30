//
// Created by lifujun on 2019/7/23.
//

#ifndef FRAMEWORK_SDLAFVIDEORENDER_H
#define FRAMEWORK_SDLAFVIDEORENDER_H

#include <SDL2/SDL.h>
#include <condition_variable>
#include <mutex>
#include <render/video/AFActiveVideoRender.h>

enum CicadaSDLViewType { CicadaSDLViewType_SDL_WINDOW, CicadaSDLViewType_NATIVE_WINDOW };

typedef struct CicadaSDLView_t {
    void *view;
    CicadaSDLViewType type;
} CicadaSDLView;

class SdlAFVideoRender : public IVideoRender, private IVSync::Listener {
public:
    SdlAFVideoRender();

    ~SdlAFVideoRender() override;

    int init() override;

    int clearScreen() override;

    /*
     * 【同步释放】放掉停在这里的两帧（mLastVideoFrame / mBackFrame）。
     *
     * 与 clearScreen() 的区别：**不清屏、不碰 SDL、不动 VSync**，只把对解码帧的引用
     * 放下 —— 因为调用它的场合是"解码器马上要被关掉/重建"，此刻清屏会黑一下，而
     * 硬解帧的引用必须在解码器之前放掉（整池 surface 被一帧钉住，见
     * IVideoRender::releaseFrames() 的说明）。
     */
    void releaseFrames() override;

    int renderFrame(std::unique_ptr<IAFFrame> &frame) override;

    int setRotate(Rotate rotate) override;

    int setFlip(Flip flip) override;

    int setScale(Scale scale) override;

    int setDisPlay(void *view) override;

    void captureScreen(std::function<void(uint8_t *data, int width, int height)> func) override;

    void setSpeed(float speed) override
    {

    }

    float getRenderFPS() override
    {
        return 0;
    };

    void onWindowSizeChange(SDL_Window *window);

    int refreshScreen();

    void delayRefreshScreen();

    uint64_t getFlags() override
    {
        return 0;
    }
    void setBackgroundColor(uint32_t color) override
    {}

private:
    int VSyncOnInit() override
    {
        return 0;
    };

    void VSyncOnDestroy() override
    {

    }

    int onVSync(int64_t tick) override;

    int onVSyncInner(int64_t tick);

    SDL_Rect getDestRet();

    SDL_Rect getSnapRect();

    SDL_RendererFlip convertFlip();

    void recreateTextureIfNeed(int videoWidth, int videoHeight);


private:
    bool mInited = false;
    SDL_Window *mVideoWindow = nullptr;
    SDL_Texture *mVideoTexture = nullptr;
    /*
     * 硬解 copy-back 直通 NV12 时用的两平面纹理（见 onVSyncInner）。只有
     * 解码器确实交来 AF_PIX_FMT_NV12 时才会创建；默认关闭，因为默认路径会
     * 先转成 YUV420P。
     */
    SDL_Texture *mVideoNv12Texture = nullptr;
    int mVideoNv12Width = 0;
    int mVideoNv12Height = 0;
    SDL_Renderer *mVideoRender = nullptr;
    bool mWindowNeedRelease{false};
    bool mRenderNeedRelease{false};
    void* mCurrentView = nullptr;

    std::atomic_bool mVideoSubSystemInited{false};

    std::unique_ptr<IAFFrame> mLastVideoFrame{};
    std::unique_ptr<IAFFrame> mBackFrame{};

    Rotate mVideoRotate = Rotate::Rotate_None;
    Rotate mRotate = Rotate::Rotate_None;
    Flip mFlip = Flip::Flip_None;
    Scale mScale = Scale::Scale_AspectFit;

    int mVideoWidth = 0;
    int mVideoHeight = 0;

    int mWindowWidth = 720;
    int mWindowHeight = 360;

    std::mutex mRenderMutex;
    std::unique_ptr<IVSync> mVSync{nullptr};

    /*
     * 渲染回调频率只从 property 里读一次（第一次回调时读，那时属性一定已经由
     * 应用设好了），见 onVSyncInner 和 PROPERTY_KEY_VIDEO_RENDER_HZ。
     */
    bool mRenderHzApplied = false;

#ifdef __WINDOWS__
    std::mutex mWindowSizeChangeMutex{};
    std::condition_variable mWindowSizeChangeCon{};
#endif
};


#endif //FRAMEWORK_SDLAFVIDEORENDER_H
