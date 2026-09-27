//
// CicadaHardwareDevice 的实现，设计说明见 CicadaHardwareDevice.h。
//
// 三个平台的情况不一样，所以这个文件里只有 Windows 真的有活干：
//   * Windows：FFmpeg 的 D3D11VA 解码器 + Qt 的 D3D11 场景图，必须共用同一个
//     ID3D11Device 才能零拷贝 —— 需要把 **Qt 正在用的**设备借给 FFmpeg（只读地取，
//     不自己建设备、不改 Qt 的设备选择；设备带不带视频接口要先用 QueryInterface 探一下，
//     详情见头文件）；
//   * macOS：框架用自研的 AFVTBDecoder，解码结果是 CVPixelBuffer，和 MTLDevice
//     没有"设备共享"这回事（纹理缓存用 Qt 的 MTLDevice 创建即可，见
//     CicadaTextureMetal），所以这里什么都不用做；
//   * Linux：VAAPI 有自己的 DRM 设备，导出的 dmabuf 由纹理后端用 EGL 导入，
//     Qt 的 GL 上下文只在纹理后端里用，同样不需要注入。
//
// 线程约定：captureFromSceneGraph() 必须在**渲染线程**上调用（Qt 的
// QSGRendererInterface 只在那个线程有效）。
//

#include "CicadaHardwareDevice.h"

#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QPointer>
#include <QtQuick/QQuickWindow>
#include <utils/frame_work_log.h>

#if defined(Q_OS_WIN)
/* d3d11.h 必须先用 C++ 链接进来，之后 FFmpeg 的 hwcontext_d3d11va.h 才是空操作。 */
#include <d3d11_1.h>
#include <dxgi.h>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include <QtQuick/QSGRendererInterface>
#include <utils/hwDeviceBridge.h>

using namespace Cicada;
#endif

namespace cicadaqt {

    CicadaHardwareDevice &CicadaHardwareDevice::instance()
    {
        static CicadaHardwareDevice s_instance;
        return s_instance;
    }

#if defined(Q_OS_WIN)

    /*
     * Qt 场景图的设备句柄。
     *
     * 【所有权：我们自己持有一份引用】这里**不是**裸指针借用 —— 拿到之后立刻 AddRef，
     * 放掉的时候 Release。理由：窗口一销毁 Qt 就会放掉它那一份，而播放器窗口在本工程里
     * 是"关掉就销毁、下次再建"的；只借不持有的话，缓存里的指针随时可能指向已释放的对象，
     * 而解码线程（FFmpeg）还会拿着它去 AddRef / 解码 —— 那就是"关掉播放窗口再开一个新视频
     * 就闪退 / 卡在正在加载"的根因。
     *
     * 【为什么要允许"换设备"】新窗口的场景图可能给出**另一个** ID3D11Device。交给 FFmpeg
     * 的设备必须和 Qt 正在渲染的那个是同一个，否则解码出来的纹理 Qt 采样不了（首帧永远
     * 出不来）。所以每次场景图初始化都重新看一遍，不一样就换过去。
     */
    static ID3D11Device *g_qtDevice = nullptr;
    static ID3D11DeviceContext *g_qtContext = nullptr;
    /* 这套设备是从哪个窗口借的：场景图回收时只放"自己那一份"。 */
    static QPointer<QQuickWindow> g_capturedWindow;

    /*
     * 解码线程（provideHwDevice）和渲染线程（capture / release）都会碰上面三个变量，
     * 必须串起来：解码器初始化可能正好撞上窗口销毁。
     */
    static QMutex g_deviceMutex;

    /* 调用方必须已经持锁。 */
    static void releaseDeviceLocked(const char *why)
    {
        if (g_qtDevice != nullptr) {
            g_qtDevice->Release();
            g_qtDevice = nullptr;
        }

        if (g_qtContext != nullptr) {
            g_qtContext->Release();
            g_qtContext = nullptr;
        }

        if (!g_capturedWindow.isNull()) {
            AF_LOGI("released the captured D3D11 device (%s)\n", why);
        }

        g_capturedWindow = nullptr;
    }

    /*
     * 设备有没有 D3D11 的视频接口。
     *
     * 这是"能不能零拷贝"的总开关：FFmpeg 的 d3d11va_device_init()
     * （libavutil/hwcontext_d3d11va.c:508）在使用外部设备时会 QueryInterface 出
     * ID3D11VideoDevice / ID3D11VideoContext，拿不到就直接返回失败（整条硬解起不来）；
     * 而只有创建设备时带了 D3D11_CREATE_DEVICE_VIDEO_SUPPORT，D3D11 才保证提供它们。
     *
     * 我们**只是问一句**，不改 Qt 的设备选择（用哪块卡、哪个后端都归 Qt 管）。
     */
    static bool hasVideoSupport(ID3D11Device *device)
    {
        if (device == nullptr) {
            return false;
        }

        ID3D11VideoDevice *videoDevice = nullptr;
        const HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&videoDevice));

        if (SUCCEEDED(hr) && videoDevice != nullptr) {
            videoDevice->Release();
            return true;
        }

        return false;
    }

    /*
     * 框架在初始化解码器时会调用这个函数（可能在解码器线程上）。
     * 返回一个**新的引用**，框架用完 av_buffer_unref() 释放。
     */
    static void *provideHwDevice()
    {
        QMutexLocker locker(&g_deviceMutex);

        if (g_qtDevice == nullptr) {
            return nullptr;
        }

        AVBufferRef *deviceRef = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);

        if (deviceRef == nullptr) {
            AF_LOGE("av_hwdevice_ctx_alloc(D3D11VA) failed\n");
            return nullptr;
        }

        auto *deviceCtx = reinterpret_cast<AVHWDeviceContext *>(deviceRef->data);

        if (deviceCtx == nullptr || deviceCtx->hwctx == nullptr) {
            av_buffer_unref(&deviceRef);
            return nullptr;
        }

        auto *d3d11 = static_cast<AVD3D11VADeviceContext *>(deviceCtx->hwctx);

        /*
         * 把 Qt 的设备交出去。
         *
         * AddRef 是必须的：FFmpeg 在自己的 device_uninit 里会 Release 这两个指针
         * （见 hwcontext_d3d11va.c 的 d3d11va_device_uninit）。device_context 可以为
         * 空，那样 FFmpeg 会自己 GetImmediateContext —— 但我们有现成的就一起给它，
         * 保证两边用的是同一个立即上下文。
         *
         * 这个 AddRef 在锁里做：否则"刚判完非空、还没来得及 AddRef"的窗口里，
         * 渲染线程可能已经把设备换掉/放掉了（原来是 UAF）。
         */
        d3d11->device = g_qtDevice;
        g_qtDevice->AddRef();

        if (g_qtContext != nullptr) {
            d3d11->device_context = g_qtContext;
            g_qtContext->AddRef();
        }

        /*
         * 只跑 device_init：建锁、补立即上下文、QueryInterface 出视频接口。
         * **不会创建新设备**（依据：libavutil/hwcontext.c:229 只调 device_init，
         * 而 d3d11va_device_create 只在 av_hwdevice_ctx_create 那条路上跑）。
         */
        const int ret = av_hwdevice_ctx_init(deviceRef);

        if (ret < 0) {
            AF_LOGE("av_hwdevice_ctx_init failed on Qt's device (%d); "
                    "falling back to a device created by FFmpeg\n", ret);
            av_buffer_unref(&deviceRef);
            return nullptr;
        }

        AF_LOGI("FFmpeg will decode on Qt's D3D11 device: zero copy is possible\n");
        return deviceRef;
    }

    bool CicadaHardwareDevice::captureFromSceneGraph(QQuickWindow *window)
    {
        if (window == nullptr) {
            return false;
        }

        auto *rif = window->rendererInterface();

        if (rif == nullptr || rif->graphicsApi() != QSGRendererInterface::Direct3D11) {
            m_description = QStringLiteral("no Qt D3D11 device (scene graph uses another API)");
            /*
             * 场景图不在 D3D11 上：这份设备我们没拿到，"显卡名"也就无从谈起
             * （界面按"拿不到型号"处理，只写"硬解 <编码>"，见 deviceName()）。
             */
            m_deviceName.clear();
            AF_LOGW("Qt scene graph is not on D3D11, zero copy with FFmpeg D3D11VA is not "
                    "possible; the framework will create its own device (copy-back)\n");
            return false;
        }

        auto *device = static_cast<ID3D11Device *>(
                           rif->getResource(window, QSGRendererInterface::DeviceResource));
        auto *context = static_cast<ID3D11DeviceContext *>(
                            rif->getResource(window, QSGRendererInterface::DeviceContextResource));

        if (device == nullptr) {
            AF_LOGW("Qt gave no D3D11 device, zero copy is not possible\n");
            return false;
        }

        {
            QMutexLocker locker(&g_deviceMutex);

            if (device == g_qtDevice) {
                /*
                 * 同一个窗口又初始化了一次场景图（或者 Qt 在多个窗口之间共用同一个设备）：
                 * 手里这套就是对的，什么都不用动。
                 */
                g_capturedWindow = window;
                m_ready = true;
                return true;
            }

            /*
             * 换设备（新窗口 / 场景图重建 / Qt 换了卡）：先把旧的放掉。
             * 先 Release 再 AddRef 的顺序不重要 —— 两个指针同时有效也没关系，
             * 重要的是**绝不留一个我们不再持有的指针在缓存里**。
             */
            releaseDeviceLocked("a different scene-graph device is taking over");

            g_qtDevice = device;
            g_qtDevice->AddRef();

            if (context != nullptr) {
                g_qtContext = context;
                g_qtContext->AddRef();
            }

            g_capturedWindow = window;
        }

        /*
         * 从这里往下用的是我们**自己持有的引用**，渲染线程就算把缓存换掉也不影响
         * （所以我们不持锁做这些 QueryInterface，免得挡住解码线程来取设备）。
         */
        m_ready = false;

        /*
         * 解码线程（FFmpeg）和 Qt 的渲染线程会共用同一个立即上下文：D3D11 的立即
         * 上下文本身不是线程安全的，除非打开多线程保护。FFmpeg 自己创建设备时会
         * 打开它（d3d11va_device_create 里的 SetMultithreadProtected），借用我们的
         * 设备时就得我们自己开。
         */
        ID3D10Multithread *multithread = nullptr;

        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&multithread)))) {
            multithread->SetMultithreadProtected(TRUE);
            multithread->Release();
        }

        /*
         * **关键一步：先确认这个设备有没有 D3D11 的视频接口。**
         *
         * 有，才值得把 Qt 的设备交给 FFmpeg（这样解码纹理和 Qt 采样在同一个设备上，
         * 零拷贝成立）。
         *
         * 没有的话千万别交：d3d11va_device_init() 在外部设备上会 QueryInterface 出
         * 视频接口，拿不到就直接失败 —— 交了反而把硬解整条路堵死（掉软解）。让框架
         * 自己去 av_hwdevice_ctx_create() 建一个带 VIDEO_SUPPORT 的设备，硬解还在，
         * 只是帧要下载回内存（copy-back），Qt 侧走 CPU 上传。
         */
        if (!hasVideoSupport(device)) {
            m_description = QStringLiteral("Qt's D3D11 device has no video support "
                                           "(no zero-copy; hardware decoding via copy-back)");
            AF_LOGW("Qt's D3D11 device has no ID3D11VideoDevice/VideoContext, so FFmpeg cannot "
                    "decode on it. Qt's own device choice is left untouched: hardware decoding "
                    "stays available because FFmpeg creates its own device (the way it always "
                    "does, with D3D11_CREATE_DEVICE_VIDEO_SUPPORT), and the decoded frames are "
                    "downloaded to memory (copy-back). Playback is correct, just without "
                    "zero copy.\n");
            return false;
        }

        /* 把 provider 注册给框架（只需要注册一次，之后每次初始化解码器都会用它）。 */
        HwDeviceBridge::setProvider(&provideHwDevice);

        /* 描述信息里带上设备名，方便确认到底用的哪块卡。 */
        IDXGIDevice *dxgiDevice = nullptr;
        QString adapterName;

        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter *adapter = nullptr;

            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter != nullptr) {
                DXGI_ADAPTER_DESC desc = {};

                if (SUCCEEDED(adapter->GetDesc(&desc))) {
                    adapterName = QString::fromWCharArray(desc.Description);
                }

                adapter->Release();
            }

            dxgiDevice->Release();
        }

        /*
         * 显卡名单独留一份给界面（见 deviceName()）：界面上"硬解 HEVC（<显卡名>）"
         * 显示的就是它（编码那一段是显示名，内核短名 H.265 由
         * CicadaPlayerItem::codecDisplayName() 换过来）。DXGI 查不出来时留**空串** ——
         * 界面按"拿不到型号"处理，只写"硬解 <编码>"，不会把下面的 "unknown adapter"
         * 当成一块卡的名字显示出去。
         */
        m_deviceName = adapterName;

        m_description = QStringLiteral("Qt D3D11 device (%1)")
                                .arg(adapterName.isEmpty() ? QStringLiteral("unknown adapter") : adapterName);
        m_ready = true;
        AF_LOGI("captured Qt's D3D11 device for zero-copy decoding: %s (window %p)\n",
                m_description.toUtf8().constData(), static_cast<void *>(window));
        return true;
    }

    void CicadaHardwareDevice::releaseFromSceneGraph(QQuickWindow *window)
    {
        QMutexLocker locker(&g_deviceMutex);

        /*
         * 只放"从它借的那一份"：window 非空、而且和记录的那个不是同一个 → 什么都不做。
         * （首页窗口的场景图被回收时，不能把播放器窗口正在用的设备顺手放掉 —— 本工程两个
         *  窗口同时活着。）
         *
         * window 为空 = 调用方已经拿不到窗口了（窗口正在析构），这时按"就是它"处理。
         */
        if (window != nullptr && !g_capturedWindow.isNull() && g_capturedWindow != window) {
            return;
        }

        releaseDeviceLocked("its scene graph went away");
        m_ready = false;
        m_description = QStringLiteral("no Qt D3D11 device captured yet");
        /* 设备都还回去了，显卡名跟着清空（下次 capture 会重新填）。 */
        m_deviceName.clear();
    }

#elif defined(Q_OS_MACOS)

    void CicadaHardwareDevice::releaseFromSceneGraph(QQuickWindow *window)
    {
        (void) window;
        /*
         * macOS 没有跨窗口共享的设备缓存（见 captureFromSceneGraph 的说明），
         * 这里只需要把状态复位：下一个窗口会重新 capture 一遍。
         */
        m_ready = false;
        m_description = QStringLiteral("no video device captured yet");
    }

    bool CicadaHardwareDevice::captureFromSceneGraph(QQuickWindow *window)
    {
        (void) window;
        /*
         * macOS 不需要设备注入：
         *   * 框架在 Apple 上用自研的 AFVTBDecoder，解码结果是 CVPixelBuffer，
         *     不需要和 FFmpeg 共享任何设备；
         *   * Qt 的 MTLDevice 只在纹理后端（CicadaTextureMetal）里用，用来创建
         *     CVMetalTextureCache。
         * 这里只是把状态记下来给界面显示。
         */
        m_description = QStringLiteral("VideoToolbox (the framework decoder hands out "
                                       "CVPixelBuffers, no device sharing needed)");
        m_ready = true;
        return true;
    }

#else

    void CicadaHardwareDevice::releaseFromSceneGraph(QQuickWindow *window)
    {
        (void) window;
        /*
         * Linux 没有跨窗口共享的设备缓存（VAAPI 那条路不借 Qt 的设备），
         * 这里只把状态复位，让下一个窗口重新 capture。
         */
        m_ready = false;
        m_description = QStringLiteral("no video device captured yet");
    }

    bool CicadaHardwareDevice::captureFromSceneGraph(QQuickWindow *window)
    {
        (void) window;
        /*
         * Linux 也不需要设备注入：VAAPI 有自己的 DRM 设备，解码结果是 VAAPI surface，
         * 后端的 dmabuf/EGL 导入用的是当前 GL 上下文（Qt 的），两件事互不干扰。
         */
        m_description = QStringLiteral("VAAPI (dmabuf sharing, no device injection needed)");
        m_ready = true;
        return true;
    }

#endif

}// namespace cicadaqt
