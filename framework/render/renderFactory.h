//
// Created by moqi on 2019-08-20.
//

#ifndef CICADA_PLAYER_RENDERFACTORY_H
#define CICADA_PLAYER_RENDERFACTORY_H

#include "render/audio/IAudioRender.h"
#include "render/video/IVideoRender.h"
namespace Cicada {
    class AudioRenderFactory {
    public:
        static std::unique_ptr<IAudioRender> create();
    };

    class videoRenderFactory {
    public:
        /*
         * 平台集成的视频渲染器工厂函数类型。
         *
         * 正常情况下由框架按编译宏挑渲染器（GLRENDER / ENABLE_SDL / Dummy）。
         * 但像 Qt 这种"窗口和绘制全归 UI 框架管"的集成方，需要自己拿到解码帧：
         * 框架的渲染器要么会自己建窗口（SdlAFVideoRender），要么会把帧直接丢掉
         * （DummyVideoRender），都不合适。
         *
         * 所以留一个注入点：平台在启动时注册一个工厂函数，返回自己的
         * IVideoRender 实现（典型做法是继承 AFActiveVideoRender，只实现
         * deviceRenderFrame()，在里面把帧交给 UI 线程）。
         *
         * 返回 nullptr 表示这次不接管，框架会继续走编译期选出来的渲染器。
         */
        using RenderCreator = std::unique_ptr<IVideoRender> (*)();

        /* 注册/清除（传 nullptr 即清除）平台自己的视频渲染器工厂。 */
        static void setRenderCreator(RenderCreator creator);

        static std::unique_ptr<IVideoRender> create(uint64_t flags = 0);
    };
}// namespace Cicada

#endif//CICADA_PLAYER_RENDERFACTORY_H
