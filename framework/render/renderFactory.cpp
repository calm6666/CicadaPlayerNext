//
// Created by moqi on 2019-08-20.
//
#include "renderFactory.h"

// Needed for std::unique_ptr below; some platform header sets do not pull it in
// transitively (observed on the OHOS build).
#include <memory>

#ifdef ANDROID
    #include "audio/Android/AudioTrackRender.h"
#endif

#ifdef __APPLE__
#include <render/audio/Apple/AFAudioQueueRender.h>
#include <render/video/AVFoundation/AVFoundationVideoRender.h>
#endif

#ifdef __OHOS__
    #include "audio/OHOS/OhosAudioRender.h"
#endif


#if TARGET_OS_OSX

#endif
#ifdef ENABLE_SDL

#include "audio/SdlAFAudioRender2.h"
#include "video/SdlAFVideoRender.h"

#endif

#ifdef GLRENDER

#include "video/glRender/GLRender.h"

#endif

#include "audio/audioRenderPrototype.h"

#ifdef ENABLE_CHEAT_RENDER

#include "audio/CheaterAudioRender.h"

#endif

#include "video/DummyVideoRender.h"
#include <utils/frame_work_log.h>
/* PRIx64：日志里打 flags 用。 */
#include <cinttypes>

using namespace Cicada;

/*
 * 平台注入的视频渲染器工厂（见 renderFactory.h）。
 *
 * 用裸函数指针而不是 std::function：注入发生在程序启动阶段（UI 线程），读取发生
 * 在播放器内部线程，写一次、读多次，指针赋值本身是原子的，不需要额外加锁，也
 * 不会在每次创建渲染器时产生堆分配。
 */
static videoRenderFactory::RenderCreator g_videoRenderCreator = nullptr;

void videoRenderFactory::setRenderCreator(RenderCreator creator)
{
    g_videoRenderCreator = creator;
    AF_LOGI("video render creator %s\n", creator != nullptr ? "registered" : "cleared");
}

bool videoRenderFactory::preferDirectSurfaceForHardwareDecode()
{
#if defined(__OHOS__) && defined(GLRENDER)
    /*
     * 鸿蒙：GLRender 现在也编进来了（软解 CPU 帧走它上屏），但**硬解仍然必须走
     * OH_AVCodec 的 surface 直出** —— 解码器直接把画面写进 XComponent 窗口，零拷贝、
     * 帧不进框架，与开 GLRender 之前完全一致。所以这里告诉上层"硬解时请带上
     * FLAG_DUMMY"：带上之后渲染器是 DummyVideoRender，SuperMediaPlayer 走
     * DECFLAG_DIRECT + mSet->mView；软解不加这个标志，渲染器是 GLRender。
     *
     * 如果硬解创建失败，SuperMediaPlayer::SetUpVideoPath 本来就会去掉 FLAG_DUMMY
     * 重建一次渲染器再退软解，那条既有路径会自动把渲染器切成 GLRender。
     */
    return true;
#else
    return false;
#endif
}

std::unique_ptr<IAudioRender> AudioRenderFactory::create()
{
    std::unique_ptr<IAudioRender> render = audioRenderPrototype::create(AF_CODEC_ID_NONE);

    if (render) {
        return render;
    }
#ifdef ENABLE_CHEAT_RENDER
    return std::unique_ptr<IAudioRender>(new CheaterAudioRender());
#endif

#ifdef ANDROID
    return std::unique_ptr<IAudioRender>(new AudioTrackRender());
#endif
#ifdef __APPLE__
    return std::unique_ptr<IAudioRender>(new AFAudioQueueRender());
#elif defined(__OHOS__)
    return std::unique_ptr<IAudioRender>(new OhosAudioRender());
#elif defined(ENABLE_SDL)
    return std::unique_ptr<IAudioRender>(new SdlAFAudioRender2());
#endif
    return nullptr;
}

std::unique_ptr<IVideoRender> videoRenderFactory::create(uint64_t flags)
{
    /*
     * 平台注入优先。
     *
     * 唯一的例外是 FLAG_DUMMY：那是播放器主动要求的"别渲染，把帧丢掉就行"
     * （Android 上 tunnel 模式 / DRM 直出 / WideVine 那些场景，解码器直接把画面
     * 送到 surface，框架不应该再插手）。这种语义下必须让 DummyVideoRender 接手，
     * 否则注入的渲染器会让本该被丢掉的帧继续流下去。
     */
    if (g_videoRenderCreator != nullptr && (flags & IVideoRender::FLAG_DUMMY) == 0) {
        std::unique_ptr<IVideoRender> render = g_videoRenderCreator();

        if (render != nullptr) {
            AF_LOGI("using the video render registered by the application (flags %" PRIx64 ")\n",
                    flags);
            return render;
        }

        AF_LOGW("the registered video render creator returned nothing, "
                "falling back to the built-in render\n");
    }

    if (flags & IVideoRender::FLAG_DUMMY) {
        return std::unique_ptr<IVideoRender>(new DummyVideoRender());
    }

    if (flags & IVideoRender::FLAG_HDR) {
#ifdef ANDROID// no render support hdr on Android, use FLAG_DUMMY to render use mediacodec
        return nullptr;
#endif
    }
#if defined(__OHOS__) && !defined(ENABLE_GLRENDER)
    // Surface-mode OH_AVCodec hardware decode renders straight into the
    // XComponent window; the pipeline uses a dummy video render.
    return std::unique_ptr<IVideoRender>(new DummyVideoRender());
#endif

#if defined(__APPLE__) && !defined(ENABLE_SDL) && !defined(ENABLE_CHEAT_RENDER)
    return std::unique_ptr<IVideoRender>(new AVFoundationVideoRender());
#endif

#if defined(GLRENDER)
    return std::unique_ptr<IVideoRender>(new GLRender());
#elif defined(ENABLE_SDL)
    return std::unique_ptr<IVideoRender>(new SdlAFVideoRender());
#elif defined(ENABLE_CHEAT_RENDER)
    return std::unique_ptr<IVideoRender>(new DummyVideoRender());
#endif
    return nullptr;
}
