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
     */
    SdlControlBar &getControlBar()
    {
        return mControlBar;
    }

    /*
     * Full screen handshake with the render thread.
     *
     * The renderer SDL picks on Windows can be Direct3D, and switching that
     * window to full screen makes SDL tear down and recreate its device and swap
     * chain. A Present() issued by the render thread while that is happening can
     * block inside the reset, and then the picture freezes: the main thread is
     * waiting for the device while the render thread sits inside Present.
     *
     * So the demo brackets its SDL work in enterRenderCall()/leaveRenderCall(),
     * and toggleFullScreen() first raises mRenderSuspended and waits for the
     * render thread to leave that region. While suspended the render callback
     * returns without touching SDL at all.
     */
    bool isRenderSuspended() const;
    void enterRenderCall();
    void leaveRenderCall();

    /*
     * True once after the window geometry changed under the render thread's feet
     * (full screen enter/leave). The render callback recreates its texture and
     * re-fetches the renderer when it sees this, so a renderer side resource
     * rebuild cannot leave a stale SDL_Texture behind.
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
#endif
#ifdef _WIN32
    SdlControlBar mControlBar{};

    /*
     * Last values pushed to the player during a drag. SDL delivers a motion
     * event for every pixel, and seeking on each one would flood the demuxer,
     * so the bar only issues a seek when the whole percentage changes.
     */
    int mLastSeekPercent = -1;
    float mLastVolumeApplied = -1.0f;

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
};


#endif //CICADAMEDIA_SDLEVENTRECEIVER_H
