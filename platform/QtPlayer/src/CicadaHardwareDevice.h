//
// CicadaHardwareDevice —— 把 Qt 场景图正在用的 GPU 设备"借"给 FFmpeg 解码。
//
// 边界（很重要，别越界）
// --------------------
// **图形设备的选择完全归 Qt**：用哪块 GPU、哪个 RHI 后端、混合显卡怎么挑、没有硬件
// 加速时怎么退回软件渲染、设备丢了怎么重建、调试层开不开 —— 这些 Qt 都处理得比我们好，
// 本工程不创建、不指定、不替换任何设备（不碰 QQuickWindow::setGraphicsDevice）。
//
// 我们只做两件事：
//   * 在渲染线程上**读**一下 Qt 当前用的设备（QSGRendererInterface::DeviceResource）；
//   * 问它一句"带不带 D3D11 的视频接口"：带，就把这个设备借给 FFmpeg 解码（这样解码
//     纹理和 Qt 采样在同一个设备上 → 零拷贝）；不带，就什么都不借（FFmpeg 建自己的
//     设备，硬解照旧，只是帧要 copy-back 回内存）。
//
// 为什么"借设备"这个动作必须做
// ---------------------------
// GPU 纹理属于具体的设备对象。D3D11 里两个不同 ID3D11Device 的纹理不能互相采样
// （要跨设备共享必须显式创建 shared handle），所以：
//
//   * 框架默认 av_hwdevice_ctx_create() 自己造一个 D3D11 设备 —— 解码纹理在**那个**
//     设备上，Qt 场景图在**它自己**的设备上，两者没关系，零拷贝不可能成立；
//   * 所以：让 FFmpeg 的 D3D11VA 解码器用 Qt 正在用的那个设备。这样解码纹理、视频
//     处理器输出纹理、Qt 采样用的纹理全在同一个设备上，才能真正做到"一个字节都
//     不经过 CPU"。
//
// 怎么把设备交给 FFmpeg（FFmpeg 源码依据）
// ---------------------------------------
//   * av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA) 建一个空的设备上下文；
//   * 填 AVD3D11VADeviceContext::device / device_context；
//   * av_hwdevice_ctx_init() —— 它只调用 device_init
//     （libavutil/hwcontext.c:229），也就是 d3d11va_device_init()
//     （hwcontext_d3d11va.c:508）：建锁、没有立即上下文就取一个、QueryInterface 出
//     ID3D11VideoDevice/VideoContext。**不会再创建设备**，所以我们的设备会被采用；
//     但这个 QueryInterface 失败就整条硬解起不来 —— 所以用之前必须先确认它能不能过，
//     过不了就别借（见上面"只读"的探测）。
//   * 注意所有权：d3d11va_device_uninit() 会对 device/device_context 调 Release
//     （hwcontext_d3d11va.c:546），所以交出去之前必须自己 AddRef。
//
// 顺带要做的两件事
// ----------------
//   1. SetMultithreadProtected(TRUE)：解码线程（FFmpeg）和 Qt 的渲染线程会用同一个
//      立即上下文。FFmpeg 自己创建设备时会打开这个开关（d3d11va_device_create），
//      借用外部设备时不会，得我们自己开，否则两个线程同时往立即上下文提交命令是
//      未定义行为。
//   2. 注册到 framework/utils/hwDeviceBridge.h，框架在初始化解码器时会来取。
//
// 线程约定
// --------
// Qt 的设备句柄只能从 QSGRendererInterface 拿，而那是**渲染线程**的接口，所以
// captureFromSceneGraph() 必须在渲染线程上调用（本工程在
// QQuickWindow::sceneGraphInitialized 里做，那个信号就在渲染线程发出）。
//
#ifndef CICADA_QT_CICADAHARDWAREDEVICE_H
#define CICADA_QT_CICADAHARDWAREDEVICE_H

#include <QtCore/QString>

#include <atomic>

class QQuickWindow;

namespace cicadaqt {

    class CicadaHardwareDevice {
    public:
        /* 单例：一个进程里我们只借一套场景图设备（谁先初始化场景图就借谁的）。 */
        static CicadaHardwareDevice &instance();

        /*
         * 渲染线程：从场景图拿设备并注册给框架。
         * 返回 true 表示"框架接下来的硬解会用 Qt 的设备"，零拷贝有戏。
         *
         * **可以反复调用**：每个窗口的场景图初始化时都会来一次（见
         * CicadaPlayerItem::onSceneGraphInitialized）。如果这次拿到的设备跟手里缓存的是
         * 同一个，直接返回；**不是同一个就换过去**（置空旧的、重新探测、重新交给框架）。
         *
         * 【为什么必须能换】这不是理论问题：本工程的播放器窗口是"关掉就销毁、下次再建"
         * 的，新窗口的场景图很可能拿到**另一个** ID3D11Device。以前这里第一行就是
         * `if (m_ready) return true;`，于是第二个窗口仍然把**上一个窗口的设备**交给
         * FFmpeg —— 解码在设备 A、显示在设备 B，首帧永远出不来（界面卡在"正在加载"）；
         * 如果设备 A 已经被 Qt 释放，那就是在已释放的对象上 AddRef：**直接闪退**。
         *
         * 注意这里**只读** Qt 选的设备，不改它：用哪块卡、哪个图形后端、怎么回退
         * （混合显卡、WARP、设备丢失恢复、调试层……）全部是 Qt 的决定，那是它做得
         * 最稳的地方，我们不插手。我们只多问一句"这个设备能不能做视频处理"
         * （D3D11 的 ID3D11VideoDevice）：能，就把设备借给 FFmpeg 解码（解码纹理和
         * Qt 采样在同一个设备上 → 零拷贝）；不能，就什么都不做，让 FFmpeg 自己建
         * 一个能解码的设备（硬解照旧，只是帧要走 copy-back）。
         */
        bool captureFromSceneGraph(QQuickWindow *window);

        /*
         * 渲染线程：某个窗口的场景图没了（窗口被销毁 / RHI 重建）——如果手里那份设备
         * 就是从它借的，就放掉，别让下一个窗口继续用。
         *
         * 只认"借它的那个窗口"：这样首页窗口的场景图被回收时，不会把播放器窗口正在用的
         * 设备顺手放掉。
         */
        void releaseFromSceneGraph(QQuickWindow *window);

        /* 是否已经把设备交给框架。 */
        bool isReady() const
        {
            return m_ready.load();
        }

        /* 诊断用：设备来源。 */
        QString description() const
        {
            return m_description;
        }

        /*
         * 界面显示用：当前交给框架解码的那块**显卡的名字**（Windows 上就是 DXGI 的
         * adapter 名，例如 "NVIDIA GeForce RTX 4060"）。
         *
         * 专门一个访问器，**不是**从 description() 那段文本里抠出来的 —— description()
         * 是排查用的实现细节文案（"Qt D3D11 device (...)"），界面不许解析它。
         *
         * 取不到时是**空串**（非 Windows 平台、场景图还没初始化、DXGI 查不出名字），
         * 调用方据此只写"硬解 <编码>"、不写型号，不许拿 "unknown adapter" 之类的
         * 占位文本当显卡名显示。
         */
        QString deviceName() const
        {
            return m_deviceName;
        }

    private:
        CicadaHardwareDevice() = default;
        ~CicadaHardwareDevice() = default;

        std::atomic<bool> m_ready{false};
        QString m_description;
        /*
         * 显卡名（**追加在成员末尾**）：与 m_description 同一套线程约定 —— 渲染线程在
         * captureFromSceneGraph() 里写，界面线程读（Qt 组装"解码方式"那一栏）。
         */
        QString m_deviceName;
    };

}// namespace cicadaqt

#endif// CICADA_QT_CICADAHARDWAREDEVICE_H
