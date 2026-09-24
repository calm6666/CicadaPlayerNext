//
// Created by moqi on 2019/12/11.
//

#ifndef CICADAMEDIA_SDLEVENTRECEIVER_H
#define CICADAMEDIA_SDLEVENTRECEIVER_H
#ifdef ENABLE_SDL
#include <SDL2/SDL.h>
#endif
#include "IEventReceiver.h"
#ifdef _WIN32
#include "SdlControlBar.h"
#endif
#include <atomic>
#define USE_NATIVE_WINDOW 0

enum CicadaSDLViewType { CicadaSDLViewType_SDL_WINDOW, CicadaSDLViewType_NATIVE_WINDOW };

typedef struct CicadaSDLView_t {
    void *view;
    CicadaSDLViewType type;
} CicadaSDLView;

class SDLEventReceiver : public IEventReceiver {

public:
    explicit SDLEventReceiver(Listener &listener);

    ~SDLEventReceiver();

    void poll(bool &exit) override;

#ifdef ENABLE_SDL
    /*
     * The SDL_Window the video is displayed in. The cmdline demo takes over
     * presentation (see the video rendering callback in cicadaPlayer.cpp) and
     * needs it to fetch the SDL_Renderer with SDL_GetRenderer().
     */
    void *getWindow() const
    {
        return mView.view;
    }
#endif

#ifdef _WIN32
    /*
     * The on-screen control bar. The demo draws it from its rendering callback
     * once the video blit is done, and this class feeds it the mouse events and
     * turns them into listener calls.
     *
     * Windows only: the demo only takes over presentation on Windows, so the
     * other SDL platforms keep the framework's own renderer.
     */
    SdlControlBar &getControlBar()
    {
        return mControlBar;
    }
#endif

#if defined(_WIN32) && defined(ENABLE_SDL)
    /*
     * 零拷贝（-direct）用的视频子窗口。
     *
     * 直通模式要自己建一条 D3D11 交换链，而 SDL 的 D3D11 渲染器已经在 SDL 窗口
     * 这个 HWND 上建了一条 flip-model 交换链，DXGI 规定同一个 HWND 同时只能有
     * 一条，第二条 CreateSwapChainForHwnd 会直接失败。子窗口有自己的 HWND，
     * 于是两边互不干扰：直通模式下 SDL 那条什么都不画，只负责窗口和输入。
     *
     * 子窗口的鼠标消息由它自己的窗口过程转回这里，走的是和 SDL 事件完全相同的
     * 处理路径，所以控件条的点击/拖动行为不变。
     */
    void setDirectMode(bool enable);

    /*
     * 子窗口是懒创建的（拿到窗口句柄之后才建）。万一它跟着父窗口一起没了，
     * 这里会补建一个——调用方不用关心它当时在不在。
     */
    void ensureDirectWindow();

    void *getDirectWindow() const
    {
        return mDirectWindow;
    }

    /*
     * 子窗口客户区尺寸（物理像素）。控件条要按它排版：直通模式下控件条画成
     * 位图贴到后台缓冲底部，位图宽度必须和后台缓冲宽度一致，行距才对得上。
     * 拿不到（子窗口还没建好）返回 false。
     */
    bool getDirectClientSize(int &width, int &height) const;

    /* 子窗口过程转发过来的鼠标事件，坐标就是客户区坐标。 */
    void forwardMouseButton(bool down, int x, int y);
    void forwardMouseMotion(int x, int y);
#endif

#ifdef ENABLE_SDL
    /*
     * 全屏切换时和"绘制方"的停手握手。
     *
     * 切全屏会让 SDL 拆掉并重建渲染器的设备/交换链；如果这时候另一个线程正在
     * Present，画面会卡住甚至崩（SDL 官方 issue #3600 "Crash on window resize
     * when rendering on another thread" 说的就是这件事）。所以 toggleFullScreen()
     * 先举起 mRenderSuspended，等绘制方离开 SDL 调用区再动窗口。
     *
     * 注意：绘制现在已经搬回**主/事件线程**（cicadaPlayer.cpp 的
     * presentPendingFrame，照 ffplay 的分工），而 toggleFullScreen() 本身也在
     * 事件线程上——两边天然串行，这套握手因此成了兜底：mInRenderCall 不会再被
     * 置起来，等待循环立刻通过。留着是为了万一日后又把绘制挪到别的线程。
     *
     * These live under ENABLE_SDL, not under _WIN32: SDLEventReceiver.cpp
     * defines them for every SDL platform, and guarding the declarations more
     * tightly than the definitions is what broke the Linux build with
     * "no declaration matches 'bool SDLEventReceiver::isRenderSuspended() const'".
     */
    bool isRenderSuspended() const;
    void enterRenderCall();
    void leaveRenderCall();

    /*
     * True once after the window geometry changed (full screen enter/leave, or a
     * plain resize). The presentation code recreates its texture and re-fetches
     * the renderer when it sees this, so a renderer side resource rebuild cannot
     * leave a stale SDL_Texture behind.
     */
    bool consumeWindowGeometryChanged();
#endif

private:
#ifdef ENABLE_SDL
    void handleMouseButtonDown(const SDL_Event &event);
    void handleMouseButtonUp(const SDL_Event &event);
    void handleMouseMotion(const SDL_Event &event);

    /* Toggles the window and notifies the listener, shared by the double click
     * shortcut and the control bar's full screen button. */
    void toggleFullScreen();

    SDL_Event event{};
    CicadaSDLView mView{};

    /* Set while the window is being switched to or from full screen. */
    std::atomic<bool> mRenderSuspended{false};

    /* True while the render thread is inside its SDL calls. */
    std::atomic<bool> mInRenderCall{false};

    /* Raised on a full screen switch, cleared by the render callback. */
    std::atomic<bool> mWindowGeometryChanged{false};

    /* Window geometry saved when entering full screen, restored on the way out. */
    int mSavedX = 0;
    int mSavedY = 0;
    int mSavedW = 1280;
    int mSavedH = 720;
#endif

#ifdef _WIN32
    SdlControlBar mControlBar{};

    /*
     * Last volume pushed to the player during a drag. SDL delivers a motion
     * event for every pixel, so the bar only calls SetVolume() once the value
     * moved by at least 1%.
     *
     * The progress bar has no such field: it is only committed once, on mouse
     * up (see handleMouseButtonUp), because each seek flushes the video path.
     */
    float mLastVolumeApplied = -1.0f;

    /* 零拷贝直通：开关，以及承载 D3D11 交换链的子窗口。 */
    bool mDirectMode = false;
    void *mDirectWindow = nullptr;

    void createDirectWindow();
    void resizeDirectWindow();
    void destroyDirectWindow();

    /*
     * 拖动窗口边缘/最大化期间让渲染线程停手。
     *
     * 注意：自从绘制搬回事件线程（见 cicadaPlayer.cpp 的 presentPendingFrame），
     * 这套停手其实已经不需要了——窗口缩放和绘制现在天然在同一个线程上串行。
     * 这两个字段先留着，作为"万一日后又把绘制挪回别的线程"的兜底，默认不生效。
     */
    std::atomic<bool> mResizeSuspended{false};
    int64_t mResizeSettleMs{0};
#endif
};


#endif //CICADAMEDIA_SDLEVENTRECEIVER_H
