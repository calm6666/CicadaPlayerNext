//
// Created by moqi on 2019/12/11.
//

#include "SDLEventReceiver.h"
#if USE_NATIVE_WINDOW
extern "C" {
#include "nativeWindow/nativeWindow.h"
}
#endif
#include <SDL2/SDL_syswm.h>
#include <utils/frame_work_log.h>
#include <utils/timer.h>

#ifdef _WIN32
#include <windowsx.h>
#endif

SDLEventReceiver::SDLEventReceiver(IEventReceiver::Listener &listener) : IEventReceiver(listener) {
}

SDLEventReceiver::~SDLEventReceiver()
{
#if defined(_WIN32) && defined(ENABLE_SDL)
    destroyDirectWindow();
#endif
}

#if defined(_WIN32) && defined(ENABLE_SDL)
namespace {
    const wchar_t *DIRECT_WINDOW_CLASS = L"CicadaDirectVideoWindow";

    /*
     * 视频子窗口的窗口过程：只做两件事——把鼠标消息转回 SDLEventReceiver
     * （走和 SDL 事件一样的处理路径），以及吞掉背景擦除（交换链自己负责这一块，
     * 让系统再刷一遍会闪）。
     */
    LRESULT CALLBACK directWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        auto *self = reinterpret_cast<SDLEventReceiver *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg) {
            case WM_NCCREATE: {
                auto *create = reinterpret_cast<CREATESTRUCTW *>(lParam);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
                return TRUE;
            }

            case WM_ERASEBKGND:
                return 1;

            case WM_MOUSEMOVE:
                if (self != nullptr) {
                    self->forwardMouseMotion(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                }

                return 0;

            case WM_LBUTTONDOWN:
                if (self != nullptr) {
                    self->forwardMouseButton(true, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                }

                return 0;

            case WM_LBUTTONUP:
                if (self != nullptr) {
                    self->forwardMouseButton(false, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                }

                return 0;

            case WM_MOUSEWHEEL:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
                return 0;

            default:
                break;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
#endif

#ifdef ENABLE_SDL
/*
 * Mouse handling.
 *
 * The control bar gets first refusal on every event: when it consumes a click
 * (or owns a drag in progress) the double-click-fullscreen shortcut and the
 * rest of the demo must not also react.
 */
#if defined(_WIN32) && defined(ENABLE_SDL)
void SDLEventReceiver::setDirectMode(bool enable)
{
    mDirectMode = enable;

    if (enable && mView.view != nullptr) {
        createDirectWindow();
    }
}

void SDLEventReceiver::ensureDirectWindow()
{
    if (mDirectMode) {
        createDirectWindow();
    }
}

void SDLEventReceiver::createDirectWindow()
{
    /*
     * 子窗口是父窗口的子窗口，父窗口一销毁它也跟着没了（SDL 在某些情况下会重建
     * 自己的窗口）。这时候句柄就是野的，IsWindow 一眼看得出来：重建它，不能对着
     * 已经消失的窗口 SetWindowPos。
     */
    if (mDirectWindow != nullptr && !IsWindow(static_cast<HWND>(mDirectWindow))) {
        AF_LOGW("direct mode: the video child window is gone, recreating it\n");
        mDirectWindow = nullptr;
    }

    /*
     * 只有直通模式才建这个子窗口。默认路径下它会把 SDL 渲染出来的画面整个
     * 盖住，而它自己什么都不画——那就是一整屏白色（WM_ERASEBKGND 返回 1，
     * 内容未定义）。
     */
    if (!mDirectMode || mDirectWindow != nullptr || mView.view == nullptr) {
        return;
    }

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);

    if (SDL_GetWindowWMInfo(static_cast<SDL_Window *>(mView.view), &info) != SDL_TRUE) {
        AF_LOGE("direct mode: cannot get the native window handle: %s\n", SDL_GetError());
        return;
    }

    HWND parent = info.info.win.window;

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = directWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = DIRECT_WINDOW_CLASS;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);

    /* 第二次调用会因为类已存在而失败，那不算错误。 */
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        AF_LOGE("direct mode: RegisterClassEx failed (%lu)\n", GetLastError());
        return;
    }

    RECT client = {};
    GetClientRect(parent, &client);

    /*
     * 先建好但**不显示**：交换链要等第一帧才知道用哪个设备，在那之前这个
     * 子窗口画不出任何东西，显示出来就是一片没有内容的白色。呈现器在第一帧
     * 成功上屏时才 ShowWindow() 它。
     */
    HWND child = CreateWindowExW(0, DIRECT_WINDOW_CLASS, L"",
                                 WS_CHILD,
                                 0, 0, client.right - client.left, client.bottom - client.top,
                                 parent, nullptr, windowClass.hInstance, this);

    if (child == nullptr) {
        AF_LOGE("direct mode: CreateWindowEx failed (%lu)\n", GetLastError());
        return;
    }

    mDirectWindow = child;
    AF_LOGI("direct mode: video child window created (%ldx%ld)\n",
            client.right - client.left, client.bottom - client.top);
}

bool SDLEventReceiver::getDirectClientSize(int &width, int &height) const
{
    width = 0;
    height = 0;

    if (mDirectWindow == nullptr || !IsWindow(static_cast<HWND>(mDirectWindow))) {
        return false;
    }

    RECT client = {};
    GetClientRect(static_cast<HWND>(mDirectWindow), &client);
    width = client.right - client.left;
    height = client.bottom - client.top;
    return width > 0 && height > 0;
}

void SDLEventReceiver::resizeDirectWindow()
{
    if (mDirectWindow == nullptr || mView.view == nullptr ||
            !IsWindow(static_cast<HWND>(mDirectWindow))) {
        return;
    }

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);

    if (SDL_GetWindowWMInfo(static_cast<SDL_Window *>(mView.view), &info) != SDL_TRUE) {
        return;
    }

    RECT client = {};
    GetClientRect(info.info.win.window, &client);

    SetWindowPos(static_cast<HWND>(mDirectWindow), nullptr, 0, 0,
                 client.right - client.left, client.bottom - client.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void SDLEventReceiver::destroyDirectWindow()
{
    if (mDirectWindow == nullptr) {
        return;
    }

    DestroyWindow(static_cast<HWND>(mDirectWindow));
    mDirectWindow = nullptr;
}

void SDLEventReceiver::forwardMouseMotion(int x, int y)
{
    /* 子窗口覆盖的正是客户区，所以坐标可以直接用。 */
    SDL_Event event = {};
    event.type = SDL_MOUSEMOTION;
    event.motion.x = x;
    event.motion.y = y;
    handleMouseMotion(event);
}

void SDLEventReceiver::forwardMouseButton(bool down, int x, int y)
{
    SDL_Event event = {};
    event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = x;
    event.button.y = y;

    if (down) {
        handleMouseButtonDown(event);
    } else {
        handleMouseButtonUp(event);
    }
}
#endif // defined(_WIN32) && defined(ENABLE_SDL)

void SDLEventReceiver::handleMouseButtonDown(const SDL_Event &sdlEvent)
{
    if (sdlEvent.button.button != SDL_BUTTON_LEFT) {
        return;
    }

#ifdef _WIN32
    {
        const SdlControlBar::Hit hit = mControlBar.mouseDown(sdlEvent.button.x, sdlEvent.button.y);

        switch (hit) {
            case SdlControlBar::Hit::PlayPause:
                // onPausePlay() flips the listener's own paused flag, so the bar
                // and the SPACE key stay in agreement automatically.
                mListener.onPausePlay();
                return;

            case SdlControlBar::Hit::Progress:
                /*
                 * Only grab the drag. The seek itself happens once, on mouse
                 * up, so that clicking the track and then dragging it still
                 * costs exactly one flush of the video path.
                 */
                return;

            case SdlControlBar::Hit::Volume:
                mListener.onSetVolume(mControlBar.dragFraction());
                return;

            case SdlControlBar::Hit::FullScreen:
                toggleFullScreen();
                return;

            case SdlControlBar::Hit::None:
                break;
        }

        // A click anywhere on the bar must not reach the fullscreen shortcut
        // below, otherwise clicking the bar's background toggles full screen.
        if (mControlBar.contains(sdlEvent.button.x, sdlEvent.button.y)) {
            return;
        }
    }
#endif

    if (mView.type != CicadaSDLViewType_SDL_WINDOW) {
        return;
    }

    // Double click on the video toggles full screen.
    static int64_t last_mouse_left_click = 0;

    if (af_gettime_relative() - last_mouse_left_click <= 500000) {
        toggleFullScreen();
        last_mouse_left_click = 0;
    } else {
        last_mouse_left_click = af_gettime_relative();
    }
}

void SDLEventReceiver::handleMouseButtonUp(const SDL_Event &sdlEvent)
{
#ifdef _WIN32
    /* Mouse up now has a side effect (the seek), so only the left button ends a
     * drag; a right button release must not jump the position. */
    if (sdlEvent.button.button != SDL_BUTTON_LEFT) {
        return;
    }

    /*
     * Seek on release, not while dragging.
     *
     * Every seek tears the video path down and rebuilds it (clear the packets,
     * flush the decoder, hunt the next keyframe), and the progress bar only has
     * 100 steps, so a live drag fired dozens of seeks in a couple of seconds.
     * On a 4K60 hardware decode the video never wins that race back to real
     * time afterwards, and staying permanently late is what locks the player up
     * (see the watchdog in SuperMediaPlayer::checkVideoPath). One seek per drag
     * avoids the storm; the slider knob already follows the mouse while
     * dragging through SdlControlBar::dragFraction().
     */
    const SdlControlBar::Hit dragging = mControlBar.dragControl();

    if (dragging == SdlControlBar::Hit::Progress) {
        mListener.onPercentageSeek(static_cast<int>(mControlBar.dragFraction() * 100.0f));
    } else if (dragging == SdlControlBar::Hit::Volume) {
        mListener.onSetVolume(mControlBar.dragFraction());
    }

    mControlBar.mouseUp();

    // Arm the next drag to push its first volume value straight through.
    mLastVolumeApplied = -1.0f;
#else
    (void) sdlEvent;
#endif
}

void SDLEventReceiver::handleMouseMotion(const SDL_Event &sdlEvent)
{
#ifdef _WIN32
    mControlBar.mouseMotion(sdlEvent.motion.x, sdlEvent.motion.y);

    /*
     * Volume is applied live - it is a cheap mixer change and the user expects
     * to hear it while dragging. The progress bar is only previewed here (the
     * knob follows the mouse) and committed in handleMouseButtonUp().
     */
    if (mControlBar.dragControl() == SdlControlBar::Hit::Volume) {
        const float volume = mControlBar.dragFraction();

        float diff = volume - mLastVolumeApplied;

        if (diff < 0.0f) {
            diff = -diff;
        }

        if (diff >= 0.01f) {
            mLastVolumeApplied = volume;
            mListener.onSetVolume(volume);
        }
    }
#else
    (void) sdlEvent;
#endif
}

void SDLEventReceiver::toggleFullScreen()
{
    static bool is_full_screen = false;

    is_full_screen = !is_full_screen;

    if (mView.view == nullptr) {
        mListener.onFullScreen(is_full_screen);
        return;
    }

    SDL_Window *window = static_cast<SDL_Window *>(mView.view);

    /*
     * Full screen is done with a borderless window covering the display, NOT
     * with SDL_WINDOW_FULLSCREEN_DESKTOP.
     *
     * SDL's own full screen path switches the display mode and rebuilds the
     * renderer's swap chain, which on the Direct3D renderer this project ends up
     * with (the log says "Created renderer: direct3d") means resetting the D3D
     * device. A Present() racing that reset blocks, and every default pool
     * resource - including the video texture - is invalidated, which is what
     * froze the picture. Resizing a borderless window changes no display mode and
     * resets no device, so there is nothing to race.
     *
     * The render thread is still asked to stand down across the geometry change,
     * because SDL does rebuild the swap chain on a resize.
     */
    mRenderSuspended.store(true);

    for (int waited = 0; waited < 200 && mInRenderCall.load(); waited += 5) {
        SDL_Delay(5);
    }

    if (is_full_screen) {
        SDL_GetWindowPosition(window, &mSavedX, &mSavedY);
        SDL_GetWindowSize(window, &mSavedW, &mSavedH);

        SDL_Rect bounds{mSavedX, mSavedY, mSavedW, mSavedH};
        const int display = SDL_GetWindowDisplayIndex(window);

        if (display >= 0) {
            SDL_GetDisplayBounds(display, &bounds);
        }

        SDL_SetWindowBordered(window, SDL_FALSE);
        SDL_SetWindowPosition(window, bounds.x, bounds.y);
        SDL_SetWindowSize(window, bounds.w, bounds.h);
    } else {
        SDL_SetWindowBordered(window, SDL_TRUE);
        SDL_SetWindowSize(window, mSavedW, mSavedH);
        SDL_SetWindowPosition(window, mSavedX, mSavedY);
    }

    /* Let the resize settle before the render thread touches SDL again. */
    SDL_Delay(100);

    mWindowGeometryChanged.store(true);
    mRenderSuspended.store(false);

    mListener.onFullScreen(is_full_screen);
}

bool SDLEventReceiver::isRenderSuspended() const
{
    return mRenderSuspended.load();
}

void SDLEventReceiver::enterRenderCall()
{
    mInRenderCall.store(true);
}

void SDLEventReceiver::leaveRenderCall()
{
    mInRenderCall.store(false);
}

bool SDLEventReceiver::consumeWindowGeometryChanged()
{
    return mWindowGeometryChanged.exchange(false);
}
#endif

void SDLEventReceiver::poll(bool &exit) {
    std::unique_ptr<IEvent> UserEvent = pop();

    if (UserEvent) {
        switch (UserEvent->getType()) {
        case IEvent::TYPE_SET_VIEW: {
            if (SDL_WasInit(SDL_INIT_VIDEO) != SDL_INIT_VIDEO) {
                SDL_Init(SDL_INIT_VIDEO);
            }
            if (mView.view == nullptr) {
#if USE_NATIVE_WINDOW
                if (getNativeFactor() != nullptr) {
                    mView.view = (getNativeFactor()->CreateNativeWindow(1280, 720));
                    if (mView.view) {
                        mView.type = CicadaSDLViewType_NATIVE_WINDOW;
                    }
                }

#endif
                if (mView.view == nullptr) {
                    Uint32 flags = 0;
                    flags |= SDL_WINDOW_ALLOW_HIGHDPI;
                    flags |= SDL_WINDOW_RESIZABLE;
#ifdef _WIN32
                    /*
                     * This is where the renderer is actually created, which makes
                     * this the only place these hints can still have an effect.
                     * SdlAFVideoRender::setView() sets SDL_HINT_RENDER_DRIVER to
                     * direct3d11 as well, but it runs after SDL_GetRenderer() has
                     * already found the renderer built here, so it never applied
                     * and Windows ended up on the Direct3D 9 driver.
                     *
                     * D3D9 is what breaks on a resize. Any size change - a full
                     * screen switch, a maximise, even dragging the window edge -
                     * makes SDL's D3D9 renderer reset its device, and here that
                     * reset fails permanently with D3DERR_INVALIDCALL:
                     *     SdlAFVideoRender: sdl log: 1 2 Reset(): INVALIDCALL
                     * repeated once per frame forever, after which Present no
                     * longer updates the window and the picture freezes while
                     * audio and the position timer carry on.
                     *
                     * The D3D11 driver resizes through ResizeBuffers and does not
                     * have that failure mode.
                     */
                    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d11");

                    /*
                     * Keep the Direct3D device usable from the render thread too.
                     * The renderer is built on this thread while the framework
                     * presents from its VSync thread, and the D3D9 device is only
                     * created with D3DCREATE_MULTITHREADED when this hint is set.
                     * Harmless under D3D11, load bearing if SDL falls back to D3D9.
                     */
                    SDL_SetHint(SDL_HINT_RENDER_DIRECT3D_THREADSAFE, "1");
#elif defined(__linux__)
                    /*
                     * Leave SDL to its own choice and it picks the OpenGL
                     * driver, which then cannot get a context in every
                     * environment:
                     *   SdlAFVideoRender: sdl log: 6 3 Created renderer: opengl
                     *   SdlAFVideoRender: sdl log: 1 2 Could not make GL context
                     *       current: BadAccess (attempt to access private resource denied)
                     *   SdlAFVideoRender: sdl log: 1 2 The specified window has
                     *       not been made current
                     * repeated once per frame: decoding and the position timer
                     * run normally but nothing is ever drawn, so the window
                     * stays black. Typical of a VM / WSL / remote session whose
                     * GL stack is not usable.
                     *
                     * The software renderer has no such requirement, and it is
                     * what SdlAFVideoRender itself asks for on Windows
                     * (SDL_RENDERER_SOFTWARE in SdlAFVideoRender.cpp:553). Use
                     * "opengl" here instead on a machine whose GL actually works.
                     */
                    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
#endif
                    SDL_Renderer *renderer;
                    SDL_CreateWindowAndRenderer(1280, 720, flags, reinterpret_cast<SDL_Window **>(&mView.view), &renderer);
                    mView.type = CicadaSDLViewType_SDL_WINDOW;
#if defined(_WIN32) && defined(ENABLE_SDL)
                    /*
                     * 直通模式的视频子窗口必须在渲染回调第一次跑之前就存在。
                     * createDirectWindow() 自己会判断 mDirectMode，非直通模式
                     * 下直接返回（默认路径绝不能有这个子窗口，它会盖住画面）。
                     */
                    createDirectWindow();
#endif
                }
            }
            mListener.onSetView(&mView);
            break;
        }
        case IEvent::TYPE_EXIT:
          mListener.onExit();
          exit = true;
          return;

        default:
          AF_LOGW("unknown type event %d", UserEvent->getType());
          break;
        }
    }

    if (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                mListener.onExit();
                exit = true;
                break;

            case SDL_WINDOWEVENT:
#ifdef _WIN32
                if (event.window.event == SDL_WINDOWEVENT_LEAVE) {
                    /* Pointer left the window: let the bar hide itself again and
                     * stop answering clicks in the bottom strip of the picture. */
                    mControlBar.mouseLeave();
                } else if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                           event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    /*
                     * 尺寸变了：让绘制侧丢掉按旧尺寸缓存的纹理。
                     * 绘制本身已经在这个线程上（presentPendingFrame），所以
                     * 不需要再"让渲染线程停手"。
                     */
                    mWindowGeometryChanged.store(true);

                    /* 零拷贝模式的视频子窗口要跟着父窗口一起改大小。 */
                    resizeDirectWindow();
                }
#endif
                break;

            case SDL_KEYDOWN: {
                switch (event.key.keysym.sym) {
                    case SDLK_SPACE:
                        mListener.onPausePlay();
                        break;

                    case SDLK_ESCAPE:
                        mListener.onExit();
                        exit = true;
                        break;

                    case SDLK_RIGHT:
                    case SDLK_LEFT:
                        mListener.onStepSeek(event.key.keysym.sym == SDLK_RIGHT);
                        break;

                    case SDLK_UP:
                    case SDLK_DOWN:
                        mListener.onChangeVolume(event.key.keysym.sym == SDLK_UP);
                        break;

                    case SDLK_F7:
                    case SDLK_F9:
                        mListener.onSpeedUp(event.key.keysym.sym == SDLK_F9);
                        break;

                    case SDLK_F8:
                        mListener.onSpeedReset();
                        break;

                    case SDLK_p:
                        mListener.onPrePare();
                        break;
                    case SDLK_r:
                        mListener.onReconnect();
                        break;

                    default:
                        if (event.key.keysym.sym >= SDLK_0 && event.key.keysym.sym <= SDLK_9) {
                            mListener.onPercentageSeek((event.key.keysym.sym - SDLK_0) * 10);
                        } else
                            AF_LOGW("unknown key %c", event.key.keysym.sym);
                        break;
                }
                break;
            }

            case SDL_MOUSEBUTTONDOWN: {
                handleMouseButtonDown(event);
            }
                break;

            case SDL_MOUSEBUTTONUP: {
                handleMouseButtonUp(event);
            }
                break;

            case SDL_MOUSEMOTION: {
                handleMouseMotion(event);
            }
                break;

            default:
                break;
        }
        if (exit){
            if (mView.view != nullptr) {
#if USE_NATIVE_WINDOW
                if (mView.type == CicadaSDLViewType_NATIVE_WINDOW) {
                    getNativeFactor()->DestroyNativeWindow(mView.view);
                } else
#endif
                    SDL_DestroyWindow((SDL_Window *) mView.view);
                mView.view = nullptr;
            }
        }
    }
}
