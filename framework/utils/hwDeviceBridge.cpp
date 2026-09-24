//
// 硬件解码设备桥的实现，设计说明见 hwDeviceBridge.h。
//

#define LOG_TAG "HwDeviceBridge"

#include "hwDeviceBridge.h"
#include <utils/frame_work_log.h>

using namespace Cicada;

/*
 * 注册进来的 provider。写一次读多次（注册发生在 UI 线程启动阶段，读取发生在
 * 解码器初始化线程），裸指针赋值是原子的，这里不再额外加锁。
 */
static HwDeviceBridge::Provider g_hwDeviceProvider = nullptr;

void HwDeviceBridge::setProvider(Provider provider)
{
    g_hwDeviceProvider = provider;
    AF_LOGI("hardware device provider %s\n", provider != nullptr ? "registered" : "cleared");
}

void *HwDeviceBridge::acquireExternalDevice()
{
    if (g_hwDeviceProvider == nullptr) {
        return nullptr;
    }

    void *device = g_hwDeviceProvider();

    if (device == nullptr) {
        /*
         * provider 给不出来（例如 Qt 的场景图当前不是 D3D11 后端）不是错误：
         * 框架会退回到自己创建设备，只是那条路拿不到零拷贝。
         */
        AF_LOGW("the registered hardware device provider gave nothing, "
                "the framework will create its own device (no zero copy)\n");
    } else {
        AF_LOGI("using the hardware device provided by the application (zero copy possible)\n");
    }

    return device;
}
