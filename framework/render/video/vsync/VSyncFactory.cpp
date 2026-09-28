//
// Created by lifujun on 2019/9/6.
//
#include <cstdlib>
#include "VSyncFactory.h"
#include "timedVSync.h"
#include "CADisplayLinkVSync.h"
#include <string>

#ifdef ANDROID
    #include "AndroidVSync.h"
    #include <utils/Android/systemUtils.h>

#endif

#ifdef __OHOS__
    #include "OHOSVSync.h"
#endif

std::unique_ptr<IVSync> VSyncFactory::create(IVSync::Listener &listener, float HZ)
{
    //TODO use android vsync: compatibility issue
//#if defined(ANDROID)
//    std::string version = get_android_property("ro.build.version.sdk");
//
//    if (atoi(version.c_str()) >= 16)
//        return std::unique_ptr<IVSync>(new AndroidVSync(listener));
//
//#endif
#ifdef __OHOS__
    /*
     * 鸿蒙：优先用系统原生 VSync（显示刷新节拍），拿不到才退回定时器。
     * OH_NativeVSync_Create 失败（例如没有显示设备）时 isValid() 为 false，这里
     * 自然落到下面的 timedVSync —— 这是能力探测式选择，不依赖任何按时间判定的分支。
     */
    std::unique_ptr<OHOSVSync> ohosVSync(new OHOSVSync(listener));

    if (ohosVSync->isValid()) {
        return std::unique_ptr<IVSync>(ohosVSync.release());
    }
#endif

    return std::unique_ptr<IVSync>(new timedVSync(listener, HZ));
    //  return std::unique_ptr<IVSync>(new CADisplayLinkVSync(listener));
}
