#include <MediaPlayer.h>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
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
#if defined(_WIN32) && defined(ENABLE_SDL)
#include "D3d11DirectPresenter.h"
#endif
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
           "  -nv12                 let the hardware decoder hand out NV12 and upload it\n"
           "                        with SDL_UpdateNVTexture() instead of converting every\n"
           "                        frame to YUV420P (saves ~24MB/frame at 4K). Off by\n"
           "                        default: it needs a working SDL D3D11 NV12 texture.\n"
           "  -direct               true zero copy: the demo presents the decoded D3D11\n"
           "                        texture itself, with its own swap chain and video\n"
           "                        processor. No download, no upload, no CPU copy, and\n"
           "                        the picture keeps playing while the window is dragged\n"
           "                        or resized. [default; probed at startup]\n"
           "  -nodirect             fall back to the copy-back + SDL path (the picture\n"
           "                        freezes while a window drag/resize is in progress)\n"
           "\n"
           "misc:\n"
           "  -h, --help            show this help\n"
           "\n"
           "keyboard: SPACE pause/resume   LEFT/RIGHT step seek   UP/DOWN volume\n"
           "          F7/F9 speed down/up  F8 reset speed   0-9 seek to percent\n"
           "          p re-prepare         r reconnect         ESC quit\n"
           "mouse   : click the play/pause button, drag the progress bar to seek, and\n"
           "          the seek happens once, when the drag is released\n",
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
 * NOTE: do NOT enable protected.audio.render.change_format here.
 *
 * It looks like the obvious answer to "the audio render only accepts S16/S16P/
 * FLT/FLTP" (SdlAFAudioRender2::device_require_format, framework/render/audio/
 * SdlAFAudioRender2.cpp:31), and it was tried: setting
 *   protected.audio.render.change_format     = ON
 *   protected.audio.render.change_format.fmt = s16
 * made the audio path unusable for EVERY file, not just the ones whose sample
 * format was unsupported, which is what the log showed:
 *
 *   D/afThread: stop() AudioRender          \
 *   D/afThread: stop() AudioRender          |  repeated every ~60 ms, forever
 *   SetUpAudioPath start                    /
 *   W/drop frame,master played time is ...  (frames dropped)
 *   KPI test total fps:12.7  9 dropped of 14 video frames
 *
 * The mechanism is in filterAudioRender::init (framework/render/audio/
 * filterAudioRender.cpp:78): with change_format on, requireSetting() raises
 * needFilter, init() then creates an ffmpeg audio filter, and if
 * mFilter->init() fails init() returns the error. The audio path is torn down
 * and rebuilt on every failure, so the retry loop never settles: no sound at
 * all, plus the CPU spent retrying starves the video path down to ~10 fps.
 *
 * The right place to fix the unsupported format is the audio render itself -
 * teach SdlAFAudioRender2 to map S32 (and whatever else) onto an SDL format -
 * not this shared setting.
 */

#if defined(_WIN32) && defined(ENABLE_SDL)
/*
 * Presentation is taken over from SdlAFVideoRender so the control bar can be
 * drawn into the same SDL_Renderer, after the video and before the present.
 * IVideoRender's callback is called before the framework draws and returning
 * true skips its drawing, so everything below is the demo's responsibility.
 *
 * 分工：
 *
 *   框架的 VSync 线程  ->  onVideoRendering()：
 *                          - 直通（默认）：D3D11 呈现不含任何 SDL 调用，整帧
 *                            就在这里做完——窗口拖动/缩放期间画面照常出（见
 *                            D3d11DirectPresenter.h 的说明）。
 *                          - 其它：只把最新一帧交给事件线程，一个 SDL 调用都不做。
 *   主/事件线程        ->  presentPendingFrame()：上传纹理、画视频、画控件条、
 *                          Present，SDL 的工作都在这里。
 *
 * 为什么 SDL 的画必须留在拥有窗口的线程：SDL 的窗口/渲染器 API 不是线程安全的。
 * 窗口尺寸变化时 SDL 的 D3D11 渲染器会在主线程上 ResizeBuffers（释放并重建后台
 * 缓冲）；此刻另一个线程还在同一个 renderer 上 Present，就会出现"画面停在最后
 * 一帧"甚至直接崩——这正是 SDL 官方 issue #3600
 * "Crash on window resize when rendering on another thread" 和论坛帖子
 * "rendering display freezes on last displayed frame when resizing app" 描述的
 * 问题。把绘制收回窗口所属线程，这类问题从根上没有了（也顺手不再需要
 * 停手握手）。
 */
struct renderContext {
    Cicada::MediaPlayer *player = nullptr;
    cicadaEventListener *listener = nullptr;
    SDLEventReceiver *receiver = nullptr;
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    /*
     * Two textures, one per input layout the decoder can hand us:
     *
     *   IYUV (three planes) - software decoding, and the hardware copy-back
     *                         path when it converts NV12 to YUV420P.
     *   NV12 (two planes)   - 硬解 copy-back 直通模式：打开全局设置
     *                         "video.render.hw.copyback_nv12" 后解码器不再做
     *                         那次 4K 全画幅转换，改由这里直接
     *                         SDL_UpdateNVTexture() 上传，每帧省 24MB 内存搬运。
     */
    SDL_Texture *texture = nullptr;
    int textureWidth = 0;
    int textureHeight = 0;
    SDL_Texture *nv12Texture = nullptr;
    int nv12TextureWidth = 0;
    int nv12TextureHeight = 0;
    /*
     * 零拷贝直通（-direct，默认开）：解码纹理直接交给 D3D11 呈现器，这条路上完全
     * 不用 SDL 的渲染器，所以下面的 renderer / texture 字段都不参与。
     */
    D3d11DirectPresenter *directPresenter = nullptr;
    int lastOutputWidth = 0;
    int lastOutputHeight = 0;

    /*
     * VSync 线程 -> 事件线程 的交接：只放“最新一帧”（旧帧直接丢掉，视频不需要
     * 排队）。clone() 只是加一次引用计数，复制很便宜；拿走的帧保证在画完之前
     * 不会被解码器回收（帧缓冲/GPU 纹理都由它钉住）。
     */
    std::mutex pendingMutex;
    std::unique_ptr<IAFFrame> pendingFrame;
    bool pendingValid = false;

    /* 最近一次真正画出去的帧：暂停时控件条还要能重画（悬停/拖动），靠它。 */
    std::unique_ptr<IAFFrame> currentFrame;
    bool lastBarVisible = false;
    /* 直通模式下控件条内容上一次的签名，见 controlBarSignature()。 */
    uint64_t lastBarSignature = 0;

    /*
     * 控件条要画的内容（播放位置/时长/音量/暂停状态/全屏），存成一份快照。
     *
     * 取值用的那些 player getter 只在事件线程上调用（和 SDL 那条路一样），
     * 快照本身给 VSync 线程读：直通模式的呈现是在 VSync 线程上做的，它需要这
     * 份数据来画控件条。SdlControlBar 自己的状态由它内部的锁保护，这里只管这
     * 份 POD。
     */
    std::mutex barMutex;
    SdlControlBar::Info barInfo;
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
 * 框架 VSync 线程调用。**这里不许出现任何 SDL 调用**（见 renderContext 的说明）。
 *
 * 两条路分工：
 *   - 零拷贝直通：D3D11 呈现不碰 SDL，所以整帧就在这里做完——这是"拖动/缩放
 *     窗口画面不停"的关键：Windows 拖动窗口走的是模态循环（WM_ENTERSIZEMOVE），
 *     被阻塞的是**拥有窗口的那个线程**（事件线程），任何要从事件线程出画的路径
 *     在拖动期间都只能停住；不依赖那个线程的呈现路径则照常出画。
 *   - 其它情况（软解 / copy-back）：只把最新一帧交给事件线程，SDL 的活全在
 *     那边做（SDL 的窗口和渲染器只能由拥有它们的线程碰）。
 *
 * 返回 true = 这一帧由我们负责呈现，框架不要再自己画。
 */
static bool onVideoRendering(void *userData, IAFFrame *frame, const CicadaJSONItem &params)
{
    (void) params;
    auto *ctx = static_cast<renderContext *>(userData);

    if (ctx == nullptr || frame == nullptr || frame->getType() != IAFFrame::FrameTypeVideo) {
        return false;
    }

    if (ctx->directPresenter != nullptr && frame->getInfo().video.format == AF_PIX_FMT_D3D11) {
        SdlControlBar::Info info;

        {
            std::lock_guard<std::mutex> lock(ctx->barMutex);
            info = ctx->barInfo;
        }

        /*
         * 无论成不成都不再走 SDL：D3D11 纹理没有 CPU 副本，SDL 那条路根本画不
         * 了（会把它当普通像素指针用）。失败时 present() 会把原因打进日志，画面
         * 停在上一帧，音频继续走。
         */
        ctx->directPresenter->present(frame, &ctx->receiver->getControlBar(), info);
        return true;
    }

    std::lock_guard<std::mutex> lock(ctx->pendingMutex);

    /* 只保留最新一帧：旧的直接丢掉，视频不需要排队。 */
    ctx->pendingFrame = frame->clone();
    ctx->pendingValid = true;
    return true;
}

/*
 * 控件条"看起来有没有变"的签名。
 *
 * 直通模式下事件线程没有新帧可选，唯一需要它补一帧的情况是控件条本身变了
 * （暂停时的悬停/拖动、自动隐藏、进度推进），所以先在这里算一个便宜的签名，
 * 没变就什么都不做——不然拖动时每一轮（2ms）都会去刷一帧。
 *
 * 量化的步长和呈现器里那份（D3d11DirectPresenter.cpp 的 overlaySignature）
 * 必须完全一致——同样的项、同样的顺序，两边结论一样才不会做无用功（多刷一帧
 * 浪费 GPU，少刷一帧就是画面不更新）。
 */
static uint64_t controlBarSignature(SdlControlBar &bar, const SdlControlBar::Info &info)
{
    uint64_t value = bar.visible() ? 1u : 0u;
    value = value * 131 + static_cast<uint64_t>(info.paused ? 1 : 0);
    value = value * 131 + static_cast<uint64_t>(info.fullScreen ? 1 : 0);
    value = value * 131 + static_cast<uint64_t>(info.durationMs > 0
            ? (info.positionMs * 2048 / info.durationMs) : 0);
    value = value * 131 + static_cast<uint64_t>(info.volume * 2048.0f);
    value = value * 131 + static_cast<uint64_t>(static_cast<int>(bar.dragControl()));
    value = value * 131 + static_cast<uint64_t>(bar.dragFraction() * 4096.0f);
    value = value * 131 + static_cast<uint64_t>(bar.barWidth() & 0xffff);
    return value;
}

/*
 * 事件线程调用（拥有窗口的那个线程），所有 SDL 工作都在这里。
 *
 * 有几种情况需要画：
 *   - 有新帧（正常播放，约 60/s）；
 *   - 控件条可见状态变了、或者正在被拖动（暂停时也要能看见/操作控件条）；
 *   - 窗口可绘制区尺寸变了（下面检测到就重建纹理）。
 * 其它时候什么都不做，所以暂停时空转成本可以忽略。
 */
static void presentPendingFrame(renderContext *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    std::unique_ptr<IAFFrame> frame;

    {
        std::lock_guard<std::mutex> lock(ctx->pendingMutex);

        if (ctx->pendingValid) {
            frame = std::move(ctx->pendingFrame);
            ctx->pendingValid = false;
        }
    }

    SdlControlBar &bar = ctx->receiver->getControlBar();
    const bool barVisible = bar.visible();
    /* 控件条自己有变化也要重画一帧：悬停/拖动/自动隐藏。 */
    const bool barChanged = (barVisible != ctx->lastBarVisible) || bar.isDragging();

    /*
     * 更新控件条快照：直通模式的呈现发生在 VSync 线程上，它读的就是这份数据。
     * player 的 getter 只在事件线程上取（和 SDL 那条路一致），每轮一次的开销
     * 可以忽略。
     */
    {
        SdlControlBar::Info info;
        info.positionMs = ctx->player->GetCurrentPosition();
        info.durationMs = ctx->player->GetDuration();
        info.volume = ctx->player->GetVolume();
        info.paused = ctx->listener->isPaused();
        info.fullScreen = ctx->listener->isFullScreen();

        std::lock_guard<std::mutex> lock(ctx->barMutex);
        ctx->barInfo = info;
    }

    if (ctx->directPresenter != nullptr) {
        /*
         * 零拷贝直通：视频帧在 VSync 线程上就呈现掉了（见 onVideoRendering），
         * 这里只做两件事——
         *   1. 把视频子窗口交给呈现器（子窗口是事件线程建的，句柄只在事件线程
         *      上读；这个调用是幂等的）；
         *   2. 控件条内容变了、或者窗口尺寸变了的时候补刷一帧：暂停时没有新帧，
         *      不补的话滑块不动、缩放后的画面也停在旧尺寸上。
         */
        ctx->receiver->ensureDirectWindow();
        ctx->directPresenter->setWindow(ctx->receiver->getDirectWindow());

        /*
         * 控件条排版：SDL 那条路每次画之前都会 layout()，直通这条路也得做，而且
         * 必须按**子窗口客户区**（也就是后台缓冲）的尺寸排——位图宽度和后台上
         * 缓冲宽度一致，UpdateSubresource 的行距才对得上。
         */
        int barAreaWidth = 0;
        int barAreaHeight = 0;

        if (ctx->receiver->getDirectClientSize(barAreaWidth, barAreaHeight)) {
            bar.layout(barAreaWidth, barAreaHeight);
        }

        SdlControlBar::Info info;

        {
            std::lock_guard<std::mutex> lock(ctx->barMutex);
            info = ctx->barInfo;
        }

        const uint64_t signature = controlBarSignature(bar, info);
        /* consume：尺寸变化这个标志也是 SDL 那条路要用的，取走一次。 */
        const bool geometryChanged = ctx->receiver->consumeWindowGeometryChanged();

        if (frame == nullptr && (geometryChanged || signature != ctx->lastBarSignature)) {
            ctx->directPresenter->present(nullptr, &bar, info);
        }

        ctx->lastBarSignature = signature;
        ctx->lastBarVisible = barVisible;
        return;
    }

    if (frame == nullptr) {
        if (!barChanged) {
            return;
        }

        /*
         * 没有新帧：拿上一次画过的那一帧重画控件条。
         * currentFrame 为空说明还没画过任何一帧（刚起来），那就没什么可重画的。
         */
        if (ctx->currentFrame == nullptr) {
            return;
        }

        frame = ctx->currentFrame->clone();
    }

    ctx->lastBarVisible = barVisible;

    /*
     * 解码器退回软解了（交上来的不再是 D3D11 纹理）：子窗口留着会盖住 SDL 画的
     * 画面，把它交还回去（close() 会把子窗口藏起来并放掉交换链）。
     */
    if (ctx->directPresenter->ready()) {
        ctx->directPresenter->close();
    }

    SDL_Window *window = static_cast<SDL_Window *>(ctx->receiver->getWindow());

    if (window == nullptr) {
        /* 窗口还没建好（视图是在第一次视频尺寸通知时才建的）。 */
        return;
    }

    /*
     * 窗口刚从全屏切回来、或者刚才尺寸变过：渲染器的后台缓冲被重建过，
     * 之前按旧尺寸缓存的纹理必须丢掉，下面会按新尺寸重建。
     */
    if (ctx->receiver->consumeWindowGeometryChanged()) {
        if (ctx->texture != nullptr) {
            SDL_DestroyTexture(ctx->texture);
            ctx->texture = nullptr;
        }

        if (ctx->nv12Texture != nullptr) {
            SDL_DestroyTexture(ctx->nv12Texture);
            ctx->nv12Texture = nullptr;
        }

        ctx->textureWidth = 0;
        ctx->textureHeight = 0;
        ctx->nv12TextureWidth = 0;
        ctx->nv12TextureHeight = 0;
    }

    if (ctx->renderer == nullptr || ctx->window != window) {
        ctx->renderer = SDL_GetRenderer(window);
        ctx->window = window;

        if (ctx->renderer == nullptr) {
            AF_LOGE("SDL_GetRenderer returned null: %s\n", SDL_GetError());
            return;
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

        if (ctx->nv12Texture != nullptr) {
            SDL_DestroyTexture(ctx->nv12Texture);
            ctx->nv12Texture = nullptr;
        }

        ctx->textureWidth = 0;
        ctx->textureHeight = 0;
        ctx->nv12TextureWidth = 0;
        ctx->nv12TextureHeight = 0;
        AF_LOGI("window drawable size is now %dx%d, rebuilding the video texture\n",
                outWidth, outHeight);
    }

    const int videoWidth = frame->getInfo().video.width;
    const int videoHeight = frame->getInfo().video.height;

    if (videoWidth <= 0 || videoHeight <= 0) {
        return;
    }

    uint8_t **data = frame->getData();
    int *lineSize = frame->getLineSize();

    if (data == nullptr || lineSize == nullptr || data[0] == nullptr) {
        return;
    }

    /*
     * NV12 straight from the hardware decoder: Y in data[0] and one interleaved
     * UV plane in data[1], which is exactly SDL_PIXELFORMAT_NV12. Taking this
     * path means the decoder skipped its swscale conversion, so the frame must
     * not be handed to the framework's YUV blit either - every early return
     * below returns true (consumed) for the same reason.
     *
     * SDL_UpdateNVTexture() arrived in SDL 2.0.22; without it the property is
     * never set below, the decoder converts to YUV420P, and this branch simply
     * never runs.
     */
#if SDL_VERSION_ATLEAST(2, 0, 22)
    if (frame->getInfo().video.format == AF_PIX_FMT_NV12) {
        if (ctx->nv12Texture == nullptr ||
            ctx->nv12TextureWidth != videoWidth || ctx->nv12TextureHeight != videoHeight) {
            if (ctx->nv12Texture != nullptr) {
                SDL_DestroyTexture(ctx->nv12Texture);
                ctx->nv12Texture = nullptr;
            }

            /*
             * NV12 在 SDL 的 D3D11 后端是有讲究的：更新走的是 UpdateSubresource，
             * 那条路要求纹理是 D3D11_USAGE_DEFAULT，而 SDL 会把 STREAMING 映射成
             * DYNAMIC + Map/Unmap。所以先按 STATIC 建，建不出来再退到 STREAMING，
             * 并把两次的原因都写进日志——上次整屏绿色就是因为这条更新没生效。
             */
            ctx->nv12Texture = SDL_CreateTexture(ctx->renderer, SDL_PIXELFORMAT_NV12,
                                                 SDL_TEXTUREACCESS_STATIC,
                                                 videoWidth, videoHeight);

            if (ctx->nv12Texture == nullptr) {
                AF_LOGW("SDL_PIXELFORMAT_NV12 with STATIC access failed (%s), trying STREAMING\n",
                        SDL_GetError());
                ctx->nv12Texture = SDL_CreateTexture(ctx->renderer, SDL_PIXELFORMAT_NV12,
                                                     SDL_TEXTUREACCESS_STREAMING,
                                                     videoWidth, videoHeight);
            }

            ctx->nv12TextureWidth = videoWidth;
            ctx->nv12TextureHeight = videoHeight;

            if (ctx->nv12Texture == nullptr) {
                AF_LOGE("cannot create the NV12 video texture: %s\n", SDL_GetError());
                return;
            }
        }

        const SDL_Rect nv12SrcRect{0, 0, videoWidth, videoHeight};

        if (SDL_UpdateNVTexture(ctx->nv12Texture, &nv12SrcRect,
                                data[0], lineSize[0], data[1], lineSize[1]) != 0) {
            AF_LOGE("SDL_UpdateNVTexture failed: %s\n", SDL_GetError());
        }

        const SDL_Rect nv12DstRect = aspectFit(videoWidth, videoHeight, outWidth, outHeight);

        SDL_SetRenderDrawColor(ctx->renderer, 0, 0, 0, 255);
        SDL_RenderClear(ctx->renderer);

        if (SDL_RenderCopy(ctx->renderer, ctx->nv12Texture, &nv12SrcRect, &nv12DstRect) != 0) {
            AF_LOGE("SDL_RenderCopy failed: %s\n", SDL_GetError());
        }

        bar.layout(outWidth, outHeight);

        if (bar.visible()) {
            /* 复用上面那份快照：同一个事件线程刚取过，不必再问一遍 player。 */
            SdlControlBar::Info info;

            {
                std::lock_guard<std::mutex> lock(ctx->barMutex);
                info = ctx->barInfo;
            }

            bar.draw(ctx->renderer, info);
        }

        SDL_RenderPresent(ctx->renderer);
        ctx->currentFrame = std::move(frame);
        return;
    }
#endif // SDL_VERSION_ATLEAST(2, 0, 22)

    if (ctx->texture == nullptr || ctx->textureWidth != videoWidth || ctx->textureHeight != videoHeight) {
        if (ctx->texture != nullptr) {
            SDL_DestroyTexture(ctx->texture);
            ctx->texture = nullptr;
        }

        /* Three separate planes: FFmpeg software decoding hands out YUV420P, and
         * the D3D11VA copy-back converts its NV12 download to YUV420P unless
         * "video.render.hw.copyback_nv12" sent it straight to the branch above. */
        ctx->texture = SDL_CreateTexture(ctx->renderer, SDL_PIXELFORMAT_IYUV,
                                         SDL_TEXTUREACCESS_STREAMING, videoWidth, videoHeight);
        ctx->textureWidth = videoWidth;
        ctx->textureHeight = videoHeight;

        if (ctx->texture == nullptr) {
            AF_LOGE("cannot create the video texture: %s\n", SDL_GetError());
            return;
        }
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

    bar.layout(outWidth, outHeight);

    if (bar.visible()) {
        /* 复用上面那份快照：同一个事件线程刚取过，不必再问一遍 player。 */
        SdlControlBar::Info info;

        {
            std::lock_guard<std::mutex> lock(ctx->barMutex);
            info = ctx->barInfo;
        }

        bar.draw(ctx->renderer, info);
    }

    SDL_RenderPresent(ctx->renderer);
    ctx->currentFrame = std::move(frame);
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
        /*
         * Opt-in: let the hardware decoder hand out NV12 and upload it with
         * SDL_UpdateNVTexture() instead of converting it to YUV420P first.
         * Saves 24MB of memory traffic per 4K frame, but SDL's D3D11 NV12
         * texture path has been seen to leave the texture zeroed (green
         * screen), so it is off unless -nv12 is passed.
         */
        bool nv12Passthrough = false;
        /*
         * Zero copy, on by default. The decoder hands out the decoded D3D11
         * texture and the demo presents it with its own D3D11 swap chain and
         * video processor - no download, no upload, no CPU touch at all.
         *
         * This is also what makes "keep playing while the window is dragged or
         * resized" possible: Windows owns the mouse in a modal loop
         * (WM_ENTERSIZEMOVE) and blocks the thread that owns the window, which
         * is the only thread SDL may be drawn from. A swap chain of our own is
         * not affected, so the picture keeps flowing.
         *
         * Requires a GPU/driver that can do NV12 video processing; probed at
         * startup (D3d11DirectPresenter::supported()), and the copy-back + SDL
         * path is used when the probe fails. -nodirect forces that path.
         */
        bool direct = true;
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
        } else if (arg == "-nv12") {
            opt.nv12Passthrough = true;
        } else if (arg == "-direct") {
            opt.direct = true;
        } else if (arg == "-nodirect" || arg == "--no-direct") {
            opt.direct = false;
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
    // protected.audio.render.change_format is deliberately left off - see the
    // long note above for what it broke when it was enabled.

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
    /*
     * 零拷贝呈现器。和 renderCtx 一样必须声明在 player 之前：局部对象按声明
     * 逆序销毁，这样它是等播放器停掉渲染/VSync 线程之后才析构的，回调不会
     * 碰到已经释放的呈现器。
     */
    D3d11DirectPresenter directPresenter;
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
    /*
     * Hardware copy-back hands out YUV420P unless the presenter claims it can
     * draw NV12. Claiming it means SDL_PIXELFORMAT_NV12 + SDL_UpdateNVTexture(),
     * which replaces the per-frame swscale conversion (24MB of traffic at 4K).
     *
     * It stays opt-in because that SDL path has not been verified end to end
     * here: the first attempt produced a green screen, which turned out to be a
     * missing NV12 row in framework/utils/ffmpeg_utils.c's pix_fmt_pair_table
     * (so the demo never even reached its NV12 branch), and the fix has not been
     * exercised yet. -direct below is the better answer anyway: it removes the
     * download as well. Must be set before Prepare(): the decoder reads it when
     * it opens.
     */
#if SDL_VERSION_ATLEAST(2, 0, 22)
    if (opt.nv12Passthrough) {
        setProperty(PROPERTY_KEY_HW_COPYBACK_NV12, "ON");
        AF_LOGW("-nv12: asking the hardware decoder to hand out NV12 without conversion\n");
    }
#else
    if (opt.nv12Passthrough) {
        AF_LOGW("-nv12 ignored: SDL %d.%d.%d has no SDL_UpdateNVTexture()\n",
                SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL);
    }
#endif

    /*
     * 零拷贝直通（默认开）：先探这台机器能不能做 NV12 -> BGRA 的 D3D11 视频处理。
     * 探不到就不能开——解码纹理没有 CPU 副本，上不了屏就是全黑，不如老实
     * 走 copy-back + SDL。
     *
     * 打开后解码器不再下载任何东西（property.h 里的
     * "video.render.hw.direct_texture"），呈现全部由 directPresenter 负责：
     * 它拿 FFmpeg 自己的 D3D11 设备建交换链，视频处理器直接把纹理画到后台
     * 缓冲，控件条用 CPU 位图贴上去。
     *
     * 这也是"拖动/缩放窗口画面不停"的前提，原因见 D3d11DirectPresenter.h。
     */
    bool directActive = false;

    if (opt.direct && !opt.hardwareDecode) {
        /* 软解没有 D3D11 纹理可交，直通无从谈起。 */
        AF_LOGW("-sw: software decoding hands out CPU frames, zero copy is off\n");
        opt.direct = false;
    }

    if (opt.direct) {
        if (!D3d11DirectPresenter::supported()) {
            AF_LOGW("-direct: this GPU/driver cannot do NV12 video processing, "
                    "staying on the copy-back + SDL path\n");
        } else {
            setProperty(PROPERTY_KEY_HW_DIRECT_TEXTURE, "ON");
            receiver.setDirectMode(true);
            renderCtx.directPresenter = &directPresenter;

            if (opt.nv12Passthrough) {
                /* 直通时根本没有下载这一步，-nv12 就没有意义了。 */
                AF_LOGW("-direct overrides -nv12: there is no copy-back to keep in NV12\n");
            }

            /*
             * 帧率：呈现按显示器刷新率节流（交换链的可等待对象），渲染回调也跟
             * 着刷新率走，这样原片是多少帧就出多少帧，而且落点不被 60Hz 的格子
             * 卡住——24fps 片源在 120Hz 屏上因此不会出现 3:2 抖动。
             * 属性必须在 Prepare() 之前设好：渲染器只在第一次回调时读它。
             */
            const int refreshHz = D3d11DirectPresenter::primaryDisplayRefreshHz();

            if (refreshHz > 0) {
                char hzText[16] = {};
                snprintf(hzText, sizeof(hzText), "%d", refreshHz);
                setProperty(PROPERTY_KEY_VIDEO_RENDER_HZ, hzText);
                AF_LOGI("-direct: display refresh %d Hz, the render callback follows it\n", refreshHz);
            }

            AF_LOGI("-direct: zero copy enabled, the decoder will hand out D3D11 textures\n");
            directActive = true;
        }
    }

    if (!directActive) {
        AF_LOGI("presentation: copy-back + SDL (the picture freezes while a window drag "
                "or resize is in progress; -direct avoids that)\n");
    }

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

#if defined(_WIN32) && defined(ENABLE_SDL)
        /*
         * 视频绘制在这里——主（事件）线程。VSync 线程只负责把最新一帧放进
         * renderCtx，所有 SDL 调用都留在拥有窗口的这个线程上（ffplay 也是这么
         * 分的）。这样窗口缩放/全屏切换和绘制天然串行，不会再出现"另一个线程
         * 正在 Present，主线程同时 ResizeBuffers"那种卡住/闪退。
         */
        if (!quite) {
            presentPendingFrame(&renderCtx);
        }
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
            /*
             * 2ms 而不是 10ms：新帧到达之后最多等这么久就上屏，帧间抖动小；
             * 一轮只做一次 SDL_PollEvent 和一次取帧判断，空转成本可以忽略。
             */
            af_msleep(2);
        }
    }

    return 0;
}
