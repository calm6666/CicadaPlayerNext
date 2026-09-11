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

SDLEventReceiver::SDLEventReceiver(IEventReceiver::Listener &listener) : IEventReceiver(listener) {
}

SDLEventReceiver::~SDLEventReceiver() = default;

#ifdef ENABLE_SDL
/*
 * Mouse handling.
 *
 * The control bar gets first refusal on every event: when it consumes a click
 * (or owns a drag in progress) the double-click-fullscreen shortcut and the
 * rest of the demo must not also react.
 */
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
                mListener.onPercentageSeek(static_cast<int>(mControlBar.dragFraction() * 100));
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
    (void) sdlEvent;
#ifdef _WIN32
    mControlBar.mouseUp();

    // Arm the next drag to push its first value straight through.
    mLastSeekPercent = -1;
    mLastVolumeApplied = -1.0f;
#endif
}

void SDLEventReceiver::handleMouseMotion(const SDL_Event &sdlEvent)
{
#ifdef _WIN32
    mControlBar.mouseMotion(sdlEvent.motion.x, sdlEvent.motion.y);

    /*
     * Apply the drag live. The control bar only knows pixels; turning that into
     * a seek or a volume change is the demo's job.
     */
    const SdlControlBar::Hit dragging = mControlBar.dragControl();

    if (dragging == SdlControlBar::Hit::Progress) {
        const int percent = static_cast<int>(mControlBar.dragFraction() * 100.0f);

        if (percent != mLastSeekPercent) {
            mLastSeekPercent = percent;
            mListener.onPercentageSeek(percent);
        }
    } else if (dragging == SdlControlBar::Hit::Volume) {
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
#endif
                    SDL_Renderer *renderer;
                    SDL_CreateWindowAndRenderer(1280, 720, flags, reinterpret_cast<SDL_Window **>(&mView.view), &renderer);
                    mView.type = CicadaSDLViewType_SDL_WINDOW;
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
