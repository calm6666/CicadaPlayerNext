//
// 硬件解码设备桥（framework/utils/hwDeviceBridge.h）
//
// 为什么需要它
// ------------
// 想做**零拷贝**，解码器解出来的 GPU 纹理必须能被"呈现方"直接使用。而 GPU 纹理
// 属于某个具体的设备对象：D3D11 里是 ID3D11Device，Metal 里是 MTLDevice。两个不同
// 设备之间的纹理不能直接互相采样（D3D11 的跨设备共享纹理要显式创建 shared
// handle，Metal 则完全不行）。
//
// Qt 的场景图有自己的 RHI 设备（Windows 上默认是 D3D11，macOS 是 Metal）。框架里
// 的 avcodecDecoder 默认自己 av_hwdevice_ctx_create() 造一个设备，于是解码纹理和
// Qt 的场景图**不在同一个设备上**，零拷贝就无从谈起（只能下载回内存再上传）。
//
// 所以给平台集成方留一个注入点：由平台创建一个"采用已有设备"的硬件设备上下文
// 交给框架使用。Qt 端就是这么做的（platform/QtPlayer/src/CicadaHardwareDevice.*）。
//
// 用法（Qt 端，Windows 为例）
// --------------------------
//   1. 从 Qt 的 QSGRendererInterface 拿到 ID3D11Device / ID3D11DeviceContext；
//   2. av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA)，把 device/device_context
//      填进 AVD3D11VADeviceContext（**记得 AddRef**，FFmpeg 在 uninit 时会 Release），
//      然后 av_hwdevice_ctx_init()（它只跑 device_init，不会另建设备 —— 见
//      libavutil/hwcontext.c:229 和 hwcontext_d3d11va.c 的 d3d11va_device_init）；
//   3. 把 provider 注册进来。
//
// 所有权约定
// ----------
// provider 每次调用都要返回一个**新的引用**（AVBufferRef*），框架用完会
// av_buffer_unref()。返回 nullptr 表示这次给不了，框架会退回到自己创建设备。
//
#ifndef CICADA_HWDEVICE_BRIDGE_H
#define CICADA_HWDEVICE_BRIDGE_H

namespace Cicada {
    class HwDeviceBridge {
    public:
        /*
         * 返回已经初始化好的 AVBufferRef*（硬件设备上下文），失败返回 nullptr。
         * 头文件里用 void* 是为了不把 FFmpeg 的头拖给每一个包含它的编译单元。
         */
        using Provider = void *(*)();

        /* 注册/清除（传 nullptr 即清除）。 */
        static void setProvider(Provider provider);

        /* 框架内部用：取一个外部设备引用，没有注册或失败时返回 nullptr。 */
        static void *acquireExternalDevice();
    };
}// namespace Cicada

#endif// CICADA_HWDEVICE_BRIDGE_H
