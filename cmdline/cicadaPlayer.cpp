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
           "  -hw, --hardware       request hardware (GPU) video decoding [default]\n"
           "  -sw, --software       force the software (FFmpeg) video decoder\n"
           "\n"
           "misc:\n"
           "  -h, --help            show this help\n"
           "\n"
           "keyboard: SPACE pause/resume   LEFT/RIGHT step seek   UP/DOWN volume\n"
           "          F7/F9 speed down/up  F8 reset speed   0-9 seek to percent\n"
           "          p re-prepare         r reconnect         ESC quit\n",
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

static void changeAudioFormat()
{
    setProperty("protected.audio.render.change_format", "ON");
    setProperty("protected.audio.render.change_format.fmt", "s16");
    setProperty("protected.audio.render.change_format.channels", "2");
    setProperty("protected.audio.render.change_format.sample_rate", "44100");
}

namespace {
    struct cmdlineOptions {
        string url;
        string manifestJson;
        bool wantFilePicker = false;
        bool haveSource = false;
        // The framework enables hardware video decoding by default
        // (player_types.h: bEnableHwVideoDecode), so that is the default here
        // too; -sw forces the FFmpeg software decoder.
        bool hardwareDecode = true;
    };
}

int main(int argc, char *argv[])
{
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
    //    changeAudioFormat();

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
