#include <MediaPlayer.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utils/timer.h>
#include <vector>

#ifdef ENABLE_SDL
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_main.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

#include <utils/CicadaJSON.h>
using namespace Cicada;
using namespace std;

#include "NetWorkEventReceiver.h"
#include "SDLEventReceiver.h"
#include "cicadaEventListener.h"
#include <utils/frame_work_log.h>

#include <utils/property.h>

#include <media_player_error_def.h>

using IEvent = IEventReceiver::IEvent;
struct cicadaCont {
    MediaPlayer *player;
    IEventReceiver *receiver;
    bool error;
};

static const char *DEFAULT_URL = "https://player.alicdn.com/video/aliyunmedia.mp4";

#ifdef _WIN32
/*
 * Opts the process into per-monitor DPI awareness.
 *
 * Without this Windows treats the process as DPI-unaware and bitmap-stretches
 * everything it draws, which is why the file dialog came out blurry on a scaled
 * display. It has to run before any window or dialog exists: the file dialog is
 * opened before SDL_Init(), so calling this first in main() covers both the
 * dialog and the video window.
 *
 * SetProcessDpiAwarenessContext() exists only on Windows 10 1703+, so it is
 * resolved dynamically and the Vista-era SetProcessDPIAware() is the fallback.
 * Doing it via GetProcAddress also avoids linking Shcore.lib. If neither works
 * the picture is simply system-scaled as before - no failure.
 */
static void enableDpiAwareness()
{
    typedef BOOL (WINAPI * SetProcessDpiAwarenessContextFn)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");

    if (user32 != nullptr) {
        auto setContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
                GetProcAddress(user32, "SetProcessDpiAwarenessContext"));

        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
        if (setContext != nullptr &&
                setContext(reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-4))) != FALSE) {
            return;
        }
    }

    SetProcessDPIAware();
}

/*
 * The player logs UTF-8 (the source path of a locally picked file is UTF-8),
 * but a Chinese Windows console defaults to cp936, so every non-ASCII path came
 * out as mojibake such as "D:\瑙嗛\鎵嬫満瑙嗛\...". Switching the console's output
 * code page to UTF-8 makes the log readable. If it fails the log still works,
 * it is just displayed in the old code page.
 */
static void enableUtf8Console()
{
    SetConsoleOutputCP(CP_UTF8);
}
#endif

static void usage(const char *exe)
{
    printf("CicadaPlayer cmdline demo\n"
           "\n"
           "usage: %s [options] [url]\n"
           "\n"
           "source (mutually exclusive, last one wins):\n"
           "  <url>                 play a network or local URL directly\n"
#ifdef _WIN32
           "  -f, --file            open a file dialog to pick a local video\n"
           "  (no argument)         same as -f, falling back to the built-in demo URL\n"
#endif
           "  -m, --manifest <file> play an object-based MediaManifest JSON (DRM)\n"
           "\n"
           "decode:\n"
           "  -hw, --hardware       hardware (GPU) video decoding [default]\n"
           "                        On Windows this is D3D11VA; if it cannot be set\n"
           "                        up, the player falls back to the CPU decoder.\n"
           "  -sw, --software       force the software (FFmpeg) video decoder\n"
           "\n"
           "misc:\n"
           "  -h, --help            show this help\n"
           "\n"
           "keyboard: SPACE pause/resume   LEFT/RIGHT step seek   UP/DOWN volume\n"
           "          F7/F9 speed down/up  F8 reset speed   0-9 seek to percent\n"
           "          p re-prepare         r reconnect         ESC quit\n"
           "mouse   : click the play/pause button, drag the progress bar to seek\n",
           exe);
}

#ifdef _WIN32
/*
 * Win32 open-file dialog. Returns the picked path as UTF-8 (FFmpeg's file
 * protocol converts UTF-8 to a wide path on Windows, so non-ASCII names work),
 * or an empty string when the user cancels.
 *
 * COM is initialised around the call so the shell can resolve virtual folders,
 * network locations and long paths. The stack buffer avoids MAX_PATH limits.
 */
static string pickVideoFile()
{
    vector<wchar_t> name(32768, L'\0');

    // Filter pairs: "label\0pattern\0" ... terminated by an extra empty string.
    // The source is UTF-8 and the build passes /utf-8, so these wide literals
    // are converted to UTF-16 correctly.
    static const wchar_t filter[] =
            L"视频文件 (*.mp4;*.mkv;*.mov;*.flv;*.ts;*.m4v;*.webm;*.avi;*.mpg)\0"
            L"*.mp4;*.mkv;*.mov;*.flv;*.ts;*.m4v;*.webm;*.avi;*.mpg;*.m2ts\0"
            L"音频文件 (*.mp3;*.aac;*.flac;*.wav;*.m4a)\0"
            L"*.mp3;*.aac;*.flac;*.wav;*.m4a\0"
            L"播放列表 (*.m3u8;*.mpd)\0"
            L"*.m3u8;*.mpd\0"
            L"所有文件 (*.*)\0"
            L"*.*\0"
            L"\0";

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = name.data();
    ofn.nMaxFile = (DWORD) name.size();
    ofn.lpstrTitle = L"选择视频文件";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool comInited = SUCCEEDED(hr);

    if (GetOpenFileNameW(&ofn) == FALSE) {
        if (comInited) {
            CoUninitialize();
        }
        return string();
    }

    if (comInited) {
        CoUninitialize();
    }

    int need = WideCharToMultiByte(CP_UTF8, 0, name.data(), -1, nullptr, 0, nullptr, nullptr);

    if (need <= 1) {
        return string();
    }

    string utf8((size_t) need - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, name.data(), -1, &utf8[0], need, nullptr, nullptr);
    return utf8;
}
#endif

static void onVideoSize(int64_t width, int64_t height, void *userData)
{
    using IEvent = IEventReceiver::IEvent;
    auto *cont = static_cast<cicadaCont *>(userData);

    if (cont->receiver) {
        cont->receiver->push(std::unique_ptr<IEvent>(new IEvent(IEvent::TYPE_SET_VIEW)));
    }
}

static void onAudioRendered(int64_t time, int64_t pts, void *userData)
{
    //    AF_LOGD("audio pts %lld rendered\n",pts);
}

static void onEOS(void *userData)
{
    auto *cont = static_cast<cicadaCont *>(userData);

    if (cont->receiver) {
        cont->receiver->push(std::unique_ptr<IEvent>(new IEvent(IEvent::TYPE_EXIT)));
    }
}
static void onPrepared(void *userData)
{
    auto *cont = static_cast<cicadaCont *>(userData);
    //   af_msleep(10000);
    cont->player->Start();
}

static void currentDownLoadSpeed(int64_t speed, void *userData)
{
    AF_LOGD("current speed is %f kbps\n", (float) speed / 1024);
}

static void onEvent(int64_t errorCode, const void *errorMsg, void *userData)
{
    auto *cont = static_cast<cicadaCont *>(userData);

    if (errorMsg == nullptr) {
        return;
    }

    AF_LOGD("%s\n", errorMsg);

    switch (errorCode) {
        case MediaPlayerEventType::MEDIA_PLAYER_EVENT_NETWORK_RETRY:
            cont->player->Reload();
            break;

        case MediaPlayerEventType::MEDIA_PLAYER_EVENT_DIRECT_COMPONENT_MSG: {
            AF_LOGI("get a dca message %s\n", errorMsg);
            CicadaJSONItem msg((char *) errorMsg);
            if (msg.getString("content", "") == "hello") {
                msg.deleteItem("content");
                msg.addValue("content", "hi");
                msg.addValue("cmd", 0);
                cont->player->InvokeComponent(msg.printJSON().c_str());
            }
            break;
        }

        default:
            break;
    }
}

static void onError(int64_t errorCode, const void *errorMsg, void *userData)
{
    auto *cont = static_cast<cicadaCont *>(userData);

    if (errorMsg) {
        AF_LOGE("%s\n", errorMsg);
    }

    if (cont->receiver) {
        auto *event = new IEvent(IEvent::TYPE_EXIT);
        cont->receiver->push(std::unique_ptr<IEvent>(event));
    } else {
        cont->error = true;
    }
}

static bool CicadaOnRenderFrame(void *userData, IAFFrame *frame)
{
    if (frame == nullptr) return false;
    if (frame->getType() != IAFFrame::FrameTypeVideo) {
        return false;
    }
    //    AF_LOGD("render a video frame %lld\n", frame->getInfo().pts);
    return false;
}
static void onSeekEnd(int64_t position, void *userData)
{
    //  AF_LOGD("seek end\n");
}

/*
 * Force the audio render onto 16 bit samples.
 *
 * SdlAFAudioRender2::device_require_format() (framework/render/audio/
 * SdlAFAudioRender2.cpp:31) accepts only S16, S16P, FLT and FLTP, and
 * init_device() maps exactly those onto AUDIO_S16SYS / AUDIO_F32SYS. Anything
 * else - most visibly the S32 that a 24 bit FLAC decodes to - matches neither
 * branch, so the SDL_AudioSpec it hands to SDL_OpenAudioDevice() keeps
 * format == 0, the device fails to open and the file plays with no sound while
 * the video and the position timer carry on perfectly normally.
 *
 * protected.audio.render.change_format is the framework's own answer: it makes
 * filterAudioRender (the base of SdlAFAudioRender2, see renderFactory.cpp:71)
 * convert the decoded frames to the requested layout before they reach the
 * device.
 *
 * Only fmt is set. The framework also understands change_format.channels and
 * change_format.sample_rate, but setting those would resample and downmix every
 * file - including the ones that were already fine - for no benefit. Set them
 * only if a particular audio device turns out to reject the source rate.
 *
 * Windows only for the moment, to keep this change inside the platform being
 * worked on; the same gap exists in the Linux and macOS SDL builds.
 */
static void forceSupportedAudioFormat()
{
    setProperty("protected.audio.render.change_format", "ON");
    setProperty("protected.audio.render.change_format.fmt", "s16");
}

#if defined(_WIN32) && defined(ENABLE_SDL)
/*
 * Presentation is taken over from SdlAFVideoRender so the control bar can be
 * drawn into the same SDL_Renderer, after the video and before the present.
 * IVideoRender's callback is called before the framework draws and returning
 * true skips its drawing, so everything below is the demo's responsibility.
 */
struct renderContext {
    Cicada::MediaPlayer *player = nullptr;
    cicadaEventListener *listener = nullptr;
    SDLEventReceiver *receiver = nullptr;
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;
    int textureWidth = 0;
    int textureHeight = 0;
    int lastOutputWidth = 0;
    int lastOutputHeight = 0;
};

/* Aspect-fit rectangle, centred - the default Scale_AspectFit behaviour. */
static SDL_Rect aspectFit(int videoWidth, int videoHeight, int windowWidth, int windowHeight)
{
    SDL_Rect result{0, 0, 0, 0};

    if (videoWidth <= 0 || videoHeight <= 0 || windowWidth <= 0 || windowHeight <= 0) {
        return result;
    }

    const float videoRatio = static_cast<float>(videoWidth) / static_cast<float>(videoHeight);
    const float windowRatio = static_cast<float>(windowWidth) / static_cast<float>(windowHeight);

    if (videoRatio <= windowRatio) {
        result.h = windowHeight;
        result.w = static_cast<int>(windowHeight * videoRatio);
    } else {
        result.w = windowWidth;
        result.h = static_cast<int>(windowWidth / videoRatio);
    }

    result.x = (windowWidth - result.w) / 2;
    result.y = (windowHeight - result.h) / 2;
    return result;
}

/*
 * Marks the span during which the render thread is inside SDL. A full screen
 * switch waits for this to clear before touching the window, so the guard has to
 * release on every exit path, including the early returns.
 */
namespace {
    struct RenderCallGuard {
        explicit RenderCallGuard(SDLEventReceiver *receiver) : mReceiver(receiver)
        {
            mReceiver->enterRenderCall();
        }

        ~RenderCallGuard()
        {
            mReceiver->leaveRenderCall();
        }

        SDLEventReceiver *mReceiver;
    };
}

static bool onVideoRendering(void *userData, IAFFrame *frame, const CicadaJSONItem &params)
{
    (void) params;
    auto *ctx = static_cast<renderContext *>(userData);

    if (ctx == nullptr || frame == nullptr || frame->getType() != IAFFrame::FrameTypeVideo) {
        return false;
    }

    /*
     * A full screen switch is in progress. Return without touching SDL at all -
     * the window is mid-rebuild and any call here could block behind it. The
     * last presented frame simply stays on screen for the few frames this lasts.
     */
    if (ctx->receiver->isRenderSuspended()) {
        return true;
    }

    RenderCallGuard renderCall(ctx->receiver);

    SDL_Window *window = static_cast<SDL_Window *>(ctx->receiver->getWindow());

    if (window == nullptr) {
        /* The window does not exist yet (the view is created on the first video
         * size notification). Let the framework have the frame. */
        return false;
    }

    /*
     * The window was just switched to or from full screen. Anything the render
     * thread cached from the old window state has to go: the renderer's swap
     * chain has been rebuilt, and a D3D device reset invalidates every default
     * pool resource, the video texture included. Dropping both makes the code
     * below re-fetch the renderer and allocate a fresh texture.
     */
    if (ctx->receiver->consumeWindowGeometryChanged()) {
        if (ctx->texture != nullptr) {
            SDL_DestroyTexture(ctx->texture);
            ctx->texture = nullptr;
        }

        ctx->textureWidth = 0;
        ctx->textureHeight = 0;
    }

    if (ctx->renderer == nullptr || ctx->window != window) {
        ctx->renderer = SDL_GetRenderer(window);
        ctx->window = window;

        if (ctx->renderer == nullptr) {
            AF_LOGE("SDL_GetRenderer returned null: %s\n", SDL_GetError());
            return false;
        }

        SDL_RendererInfo info;

        if (SDL_GetRendererInfo(ctx->renderer, &info) == 0) {
            AF_LOGI("video renderer: '%s'\n", info.name ? info.name : "?");
        }
    }

    /*
     * SdlAFVideoRender::init() installed a render scale to compensate for DPI,
     * which would also scale every coordinate below. Work in drawable pixels
     * with the scale neutralised instead.
     */
    SDL_RenderSetScale(ctx->renderer, 1.0f, 1.0f);

    int outWidth = 0;
    int outHeight = 0;
    SDL_GetRendererOutputSize(ctx->renderer, &outWidth, &outHeight);

    /*
     * Rebuild the texture whenever the drawable size changes, not only on a full
     * screen switch. A maximise, or the user dragging the window edge, resizes
     * the window without going through toggleFullScreen(), and the renderer's
     * back buffer is rebuilt underneath us in all of those cases.
     */
    if (outWidth != ctx->lastOutputWidth || outHeight != ctx->lastOutputHeight) {
        ctx->lastOutputWidth = outWidth;
        ctx->lastOutputHeight = outHeight;

        if (ctx->texture != nullptr) {
            SDL_DestroyTexture(ctx->texture);
            ctx->texture = nullptr;
        }

        ctx->textureWidth = 0;
        ctx->textureHeight = 0;
        AF_LOGI("window drawable size is now %dx%d, rebuilding the video texture\n",
                outWidth, outHeight);
    }

    const int videoWidth = frame->getInfo().video.width;
    const int videoHeight = frame->getInfo().video.height;

    if (videoWidth <= 0 || videoHeight <= 0) {
        return false;
    }

    if (ctx->texture == nullptr || ctx->textureWidth != videoWidth || ctx->textureHeight != videoHeight) {
        if (ctx->texture != nullptr) {
            SDL_DestroyTexture(ctx->texture);
            ctx->texture = nullptr;
        }

        /* The decoder hands out 8 bit 4:2:0 here: FFmpeg software decoding gives
         * YUV420P, and the D3D11VA copy-back converts its NV12 download to
         * YUV420P precisely because this renderer uploads Y/U/V planes. */
        ctx->texture = SDL_CreateTexture(ctx->renderer, SDL_PIXELFORMAT_IYUV,
                                         SDL_TEXTUREACCESS_STREAMING, videoWidth, videoHeight);
        ctx->textureWidth = videoWidth;
        ctx->textureHeight = videoHeight;

        if (ctx->texture == nullptr) {
            AF_LOGE("cannot create the video texture: %s\n", SDL_GetError());
            return false;
        }
    }

    uint8_t **data = frame->getData();
    int *lineSize = frame->getLineSize();

    if (data == nullptr || lineSize == nullptr || data[0] == nullptr) {
        return false;
    }

    SDL_Rect srcRect{0, 0, videoWidth, videoHeight};

    if (SDL_UpdateYUVTexture(ctx->texture, &srcRect,
                             data[0], lineSize[0], data[1], lineSize[1], data[2], lineSize[2]) != 0) {
        AF_LOGE("SDL_UpdateYUVTexture failed: %s\n", SDL_GetError());
    }

    const SDL_Rect dstRect = aspectFit(videoWidth, videoHeight, outWidth, outHeight);

    SDL_SetRenderDrawColor(ctx->renderer, 0, 0, 0, 255);
    SDL_RenderClear(ctx->renderer);

    if (SDL_RenderCopy(ctx->renderer, ctx->texture, &srcRect, &dstRect) != 0) {
        AF_LOGE("SDL_RenderCopy failed: %s\n", SDL_GetError());
    }

    SdlControlBar &bar = ctx->receiver->getControlBar();
    bar.layout(outWidth, outHeight);

    if (bar.visible()) {
        SdlControlBar::Info info;
        info.positionMs = ctx->player->GetCurrentPosition();
        info.durationMs = ctx->player->GetDuration();
        info.volume = ctx->player->GetVolume();
        info.paused = ctx->listener->isPaused();
        info.fullScreen = ctx->listener->isFullScreen();
        bar.draw(ctx->renderer, info);
    }

    SDL_RenderPresent(ctx->renderer);
    return true;
}
#endif

namespace {
    struct cmdlineOptions {
        string url;
        string manifestJson;
        bool wantFilePicker = false;
        bool haveSource = false;
        /*
         * Hardware video decoding is the default, matching the framework
         * (player_types.h: bEnableHwVideoDecode == true). On Windows this drives
         * the D3D11VA path in avcodecDecoder; if that cannot be set up, the
         * framework drops to the software decoder so the video still plays
         * instead of failing outright. Pass -sw to stay on the CPU on purpose.
         */
        bool hardwareDecode = true;
    };
}

int main(int argc, char *argv[])
{
#ifdef _WIN32
    // Must come before the file dialog and before SDL creates the video window,
    // otherwise Windows bitmap-stretches both and everything looks blurry.
    enableDpiAwareness();
    // And make the UTF-8 log readable on a cp936 console.
    enableUtf8Console();
#endif

    cmdlineOptions opt;
    setProperty("protected.network.http.http2", "ON");

    // Usage:
    //   cicadaPlayer <url>             classic URL playback
    //   cicadaPlayer -f                pick a local file with the Win32 dialog
    //   cicadaPlayer -m <file.json>    object-based playback (MediaManifest JSON, DRM)
    //   cicadaPlayer -hw | -sw         hardware / software video decoding
    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else if (arg == "-f" || arg == "--file") {
#ifdef _WIN32
            opt.wantFilePicker = true;
#else
            printf("error: %s is only implemented on Windows, pass the path as an argument\n\n",
                   arg.c_str());
            return 2;
#endif
        } else if (arg == "-hw" || arg == "--hardware") {
            opt.hardwareDecode = true;
        } else if (arg == "-sw" || arg == "--software") {
            opt.hardwareDecode = false;
        } else if (arg == "-m" || arg == "--manifest") {
            if (i + 1 >= argc) {
                printf("error: %s needs a file argument\n\n", arg.c_str());
                usage(argv[0]);
                return 2;
            }
            std::ifstream manifestFile(argv[++i]);
            if (!manifestFile.is_open()) {
                printf("error: cannot open manifest %s\n", argv[i]);
                return 2;
            }
            std::stringstream buffer;
            buffer << manifestFile.rdbuf();
            opt.manifestJson = buffer.str();
            opt.haveSource = true;
        } else if (!arg.empty() && arg[0] == '-' && arg.size() > 1) {
            printf("error: unknown option %s\n\n", arg.c_str());
            usage(argv[0]);
            return 2;
        } else {
            opt.url = arg;
            opt.haveSource = true;
        }
    }

    log_enable_color(1);
    log_set_level(AF_LOG_LEVEL_TRACE, 1);
    setProperty("protected.audio.render.hw.tempo", "OFF");
    //
#ifdef _WIN32
    // Without this, any audio that does not decode to S16 or FLT (24 bit FLAC
    // decodes to S32, for instance) opens no SDL device and plays silently.
    forceSupportedAudioFormat();
#endif

#ifdef _WIN32
    if (!opt.haveSource) {
        // No source given: offer the file dialog, which is the useful default
        // for a desktop demo. Cancelling keeps the old behaviour.
        opt.wantFilePicker = true;
    }

    if (opt.wantFilePicker) {
        string picked = pickVideoFile();

        if (!picked.empty()) {
            opt.url = picked;
            opt.haveSource = true;
        } else if (opt.haveSource) {
            printf("no file selected, keeping the source given on the command line\n");
        } else {
            printf("no file selected, falling back to the built-in demo URL\n");
            opt.url = DEFAULT_URL;
        }
    }
#endif

    if (opt.manifestJson.empty()) {
        if (opt.url.empty()) {
            opt.url = DEFAULT_URL;
        }
        AF_LOGI("source: %s\n", opt.url.c_str());
    } else {
        AF_LOGI("source: MediaManifest JSON (%zu bytes)\n", opt.manifestJson.size());
    }
    AF_LOGI("video decoder: %s\n", opt.hardwareDecode ? "hardware (GPU)" : "software (FFmpeg)");

    cicadaCont cicada{};
#if defined(_WIN32) && defined(ENABLE_SDL)
    /*
     * Declared BEFORE the player on purpose.
     *
     * Locals are destroyed in reverse order, so this context is torn down after
     * the player has stopped its renderer and VSync thread. Declaring it later
     * would destroy it first, while the VSync thread could still be calling
     * onVideoRendering() with a dangling pointer.
     *
     * The texture is intentionally not destroyed here: by the time this goes
     * away the SDL_Renderer is already gone, and calling SDL_DestroyTexture on a
     * dead renderer would be worse than letting the process exit release it.
     */
    renderContext renderCtx;
#endif
    unique_ptr<MediaPlayer> player = unique_ptr<MediaPlayer>(new MediaPlayer());
    cicada.player = player.get();
    playerListener pListener{nullptr};
    pListener.userData = &cicada;
    pListener.VideoSizeChanged = onVideoSize;
    pListener.AudioRendered = onAudioRendered;
    pListener.Completion = onEOS;
    pListener.EventCallback = onEvent;
    pListener.ErrorCallback = onError;
    pListener.Prepared = onPrepared;
    pListener.CurrentDownLoadSpeed = currentDownLoadSpeed;
    pListener.SeekEnd = onSeekEnd;
    cicadaEventListener eListener(player.get());
#ifdef ENABLE_SDL
    SDLEventReceiver receiver(eListener);
    cicada.receiver = &receiver;
#else
    int view = 0;
    player->SetView(&view);
#endif
    NetWorkEventReceiver netWorkEventReceiver(eListener);
    player->SetListener(pListener);
    player->SetDefaultBandWidth(1000 * 1000);
    player->EnableHardwareDecoder(opt.hardwareDecode);
#if defined(_WIN32) && defined(ENABLE_SDL)
    /*
     * Take over presentation so the control bar can be drawn on top of the video.
     *
     * IVideoRender's rendering callback runs before the framework blits the
     * frame, and returning true skips the built-in drawing entirely, so the demo
     * has to do the whole job: texture upload, letter box, overlay, present.
     * That is deliberate - the alternative would be a hook inside
     * SdlAFVideoRender, which is shared with the Linux and macOS builds.
     */
    renderCtx.player = player.get();
    renderCtx.listener = &eListener;
    renderCtx.receiver = &receiver;
    player->SetVideoRenderingCallback(onVideoRendering, &renderCtx);
#endif
    if (!opt.manifestJson.empty()) {
        // Object-based playback: unified MediaManifest JSON (DRM-capable).
        player->SetDataSource(opt.manifestJson);
    } else {
        player->SetDataSource(opt.url.c_str());
    }
    player->SetAutoPlay(true);
    player->SetLoop(true);
    player->SetIPResolveType(IpResolveWhatEver);
    player->SetFastStart(true);
    MediaPlayerConfig config = *(player->GetConfig());
    //   config.mMaxBackwardBufferDuration = 20000;
    config.liveStartIndex = -3;
    player->SetConfig(&config);
    player->Prepare();
    player->SelectTrack(-1);
    player->SetOnRenderFrameCallback(CicadaOnRenderFrame, nullptr);
    //    player->SetStreamDelayTime(-1, -200);
    bool quite = false;

    while (!quite && !cicada.error) {
#ifdef ENABLE_SDL
        receiver.poll(quite);
#endif

        if (!quite) {
            netWorkEventReceiver.poll(quite);

            if (quite) {
                auto *event = new IEvent(IEvent::TYPE_EXIT);
#ifdef ENABLE_SDL
                receiver.push(std::unique_ptr<IEvent>(event));
#endif
            }
        }

        if (!quite) {
            af_msleep(10);
        }
    }

    return 0;
}
