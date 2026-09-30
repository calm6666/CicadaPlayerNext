//
// Windows 零拷贝后端：FFmpeg D3D11VA 的解码纹理 -> D3D11 视频处理器（NV12/P010
// -> RGBA）-> 包成 QSGTexture 交给 Qt 场景图。
//
// 为什么是"GPU 内部转换 + Qt 采样"，而不是直接把 NV12 丢给 Qt
// -----------------------------------------------------------
// Qt 的场景图（QSGTexture / 默认材质）只会把纹理当成 RGBA 采样，它没有"这是 NV12，
// 帮我做 YUV->RGB"的概念（Qt 自己的视频管线是靠私有的 QVideoTextureHelper + 自定义
// 材质做的，不适合第三方依赖）。所以要在 GPU 上先把 NV12 转成 RGBA，再用
// QNativeInterface::QSGD3D11Texture::fromNative() 包成 QSGTexture（Qt 6.8+；
// 6.5~6.7 走 QRhiTexture 的兜底路线，见 CicadaQtTextureWrap）。
//
// **输出必须是 DXGI_FORMAT_R8G8B8A8_UNORM**：QSGTexture 没有像素格式参数，Qt 只能按
// 场景图自己的约定（RGBA8）去建 SRV，而有类型的 D3D11 纹理格式对不上就会
// E_INVALIDARG（画面全黑）。原因写在 CicadaTextureD3D11.cpp 里的
// CICADA_QT_D3D11_OUTPUT_FORMAT 上。
//
// 关键在于**转换全程在 GPU 上**：解码纹理 -> 视频处理器（GPU）-> RGBA 纹理 ->
// Qt 采样（GPU），一个字节都不回内存，也没有 GPU->CPU->GPU 的往返。这跟 copy-back
// 那条路（4K 每帧下载 12.4MB + 上传 12.4MB）差了一个数量级。
//
// 前提（很重要）
// --------------
// 解码纹理和 RGBA 纹理必须属于**同一个** ID3D11Device，而那个设备还必须就是 Qt
// 场景图正在用的设备 —— 否则纹理没法直接给 Qt 采样。所以：
//   1. CicadaHardwareDevice 在场景图初始化时把 Qt 的 ID3D11Device 交给 FFmpeg
//      （framework/utils/hwDeviceBridge.h，只支持 D3D11 这条路）；
//   2. 这里再通过 QSGRendererInterface::DeviceResource 拿到同一个设备，
//      QueryInterface 出 ID3D11VideoDevice/VideoContext 做视频处理。
//
// 线程约定：本文件所有函数都只能在 Qt 的**渲染线程**上调用
// （QQuickItem::updatePaintNode 里），因为要碰场景图的 RHI 设备。
//
#ifndef CICADA_QT_CICADATEXTURED3D11_H
#define CICADA_QT_CICADATEXTURED3D11_H

#include <QtCore/QSize>
#include <QtQuick/QSGTexture>
/* 色彩调整的三个值 + 脏标记：GUI 线程写、渲染线程读，所以用原子量。 */
#include <atomic>
#include <thread>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11VideoDevice;
struct ID3D11VideoContext;
struct ID3D11VideoContext1;
struct ID3D11VideoProcessorEnumerator;
struct ID3D11VideoProcessor;
struct ID3D11VideoProcessorInputView;
struct ID3D11VideoProcessorOutputView;
class QQuickWindow;
class IAFFrame;

namespace cicadaqt {

    /*
     * D3D11 零拷贝纹理生产者。
     *
     * 生命周期：跟着 QQuickItem 的渲染资源走 —— prepare() 在第一次渲染时调用，
     * releaseResources() 在场景图失效（QQuickWindow::sceneGraphInvalidated）或者
     * item 销毁时调用。所有 D3D11 对象都在这个类里自持有、自释放。
     */
    class CicadaTextureD3D11 {
    public:
        CicadaTextureD3D11();
        ~CicadaTextureD3D11();

        CicadaTextureD3D11(const CicadaTextureD3D11 &) = delete;
        CicadaTextureD3D11 &operator=(const CicadaTextureD3D11 &) = delete;

        /*
         * 渲染线程：从 Qt 拿 D3D11 设备/上下文，QueryInterface 出视频处理接口。
         * 成功返回 true，之后 textureForFrame() 才可用。
         */
        bool prepare(QQuickWindow *window);

        /*
         * 渲染线程：把一帧解码纹理转成 RGBA 并包成 QSGTexture。
         *
         * 返回的 QSGTexture 归调用方所有（通常交给 QSGSimpleTextureNode 的
         * setOwnsTexture(true)）；失败返回 nullptr，调用方应该退回到 CPU 路径。
         * 调用方还需要知道采样时要不要上下翻转：D3D11 纹理原点在左上，Qt 的
         * D3D11 后端也是，所以 *flipVertically = false。
         */
        QSGTexture *textureForFrame(QQuickWindow *window, IAFFrame *frame, bool *flipVertically);

        /* 释放所有 D3D11 资源（渲染线程或场景图析构时调用）。 */
        void releaseResources();

        /*
         * 渲染线程：只放掉**输入视图**（连它缓存的那张解码纹理信息），设备、处理器、
         * 枚举器、输出视图和输出纹理全部保留。
         *
         * 为什么单独需要它：输入视图对**解码纹理**持有 D3D11 引用，而解码纹理是
         * FFmpeg d3d11va 的整张表面数组（池子里 20 个 surface 在同一个
         * ID3D11Texture2D 里）。换片源/停播之后播放器会被销毁，但只要这个视图还留着，
         * 上一个解码器的整池显存就释放不掉（实测 1080p 约 60MB、4K 约 250MB）。
         *
         * 不顺手释放处理器/枚举器是有意的：它们和输入尺寸绑定、跟"是哪一条片源"无关，
         * 留着才能在下一条片源上直接复用（重建一次要 CreateVideoProcessor +
         * CreateVideoProcessorEnumerator，纯属浪费）。
         */
        void releaseInputView();

        /*
         * 渲染线程：忘掉"输出纹理那张 QSGTexture 包装"的记录（**只丢指针，不 delete**）。
         *
         * 调用时机只有一个要求：**场景图节点被删掉/重建的那一刻**。那张 QSGTexture 归节点
         * 所有（setOwnsTexture(true)），节点一没它就没了，再拿着旧指针去 setTexture() 就是
         * use-after-free。节点重建的四个点在 CicadaPlayerItem::updatePaintNode 里，
         * 那里会调本函数。
         */
        void forgetOutputTextureWrapper();

        /*
         * 渲染线程：逼 D3D11 把"延迟销毁"的对象真正销毁掉。
         *
         * 为什么需要它（微软官方文档，不是猜的）：
         *   ID3D11DeviceContext::Flush 的 Remarks 原文 —— "Direct3D 11 defers the
         *   destruction of objects. Therefore, an application can't rely upon objects
         *   immediately being destroyed. By calling Flush, you destroy any objects whose
         *   destruction was deferred."
         *
         * 换成这套代码里的话：**我们把最后一帧/输入视图/纹理的引用全 Release 掉了，
         * 也只是把引用计数减到 0；那张 20 片 surface 的纹理数组（1080p 约 62MB、
         * 4K 约 249MB）仍然可能被运行时"延迟销毁"拖着**。所以"关解码器 + 放帧"
         * 之后必须再 Flush 一次，否则显存/驱动分配会一直挂在那里 —— 这正是
         * "pool 释放不掉"最直接、也最被忽略的一环。
         *
         * 为什么**不**顺手调 ID3D11DeviceContext::ClearState()：
         *   官方在 Flush 那一页给的"同步销毁"配方是 "release all references →
         *   ClearState → Flush"，其中 ClearState 的作用是清掉**管线绑定**，让 D3D
         *   不再持有那些对象的引用。但这里的 m_context 是 **Qt 场景图的立即上下文**
         *   （QSGRendererInterface::DeviceContextResource），而 ClearState 会把所有
         *   资源槽/shaders/输入布局/光栅化与混合状态/viewport 全部置 NULL
         *   （ClearState 文档原文："sets all input/output resource slots, shaders,
         *   input layouts, ... and viewports to NULL"）—— Qt 的 RHI 自己也在跟踪
         *   管线状态，我们一清就会和它记账对不上，画面上会出现随机的错绑定。
         *   所以这条路我们只用 Flush（它只"把命令推给 GPU"，不改任何状态），
         *   而"清绑定"这件事交给下一条：Trim 之前的场景图失效路径本来就把渲染
         *   资源全放了（见 releaseResources）。
         */
        void flushDeferredDestruction();

        /*
         * 渲染线程（只在**空闲**时刻调：场景图失效 / 关窗 / 播放器销毁）：
         * 让 DXGI 立刻销毁延迟销毁的对象，并把驱动内部缓存的内存还给系统。
         *
         * IDXGIDevice3::Trim 文档原文："Trims the graphics memory allocated by the
         * IDXGIDevice3 DXGI device on the app's behalf... graphics drivers periodically
         * allocate internal memory buffers in order to speed up subsequent rendering
         * requests. **These memory allocations count against the app's memory usage**...
         * Direct3D will normally defer the destruction of D3D objects. Calling Trim,
         * however, forces Direct3D to destroy objects immediately... apps should only
         * call Trim when going idle for a period of time."
         *
         * 两点正是我们要的：
         *   1. "驱动内部缓冲算在应用的内存账上" —— 关窗后那几十 MB 不降，这一条是
         *      官方点名的来源之一（不只是我们自己的纹理）；
         *   2. "只在空闲时调" —— 关窗/销毁播放器就是这个时刻，切档那种每几秒一次的
         *      路径**不能**调（文档明说会有性能损失，驱动要重新分配内部缓冲）。
         */
        void trimVideoMemory();

        /* 诊断用：当前走的是哪条路。 */
        const char *backendName() const
        {
            return "D3D11 zero-copy (video processor NV12->RGBA on the GPU)";
        }

        /*
         * 视频色彩调整（右键菜单 →「视频色彩调整」）。
         * 取值 0~200，**100 = 中性**。可以**从 GUI 线程调**（拖滑块时），
         * 所以三个值 + 脏标记都是原子量；真正应用到处理器在渲染线程的 Blt 之前
         * （见 applyColorAdjust）。
         */
        void setColorAdjust(int brightness, int contrast, int saturation);

    private:
        /*
         * 渲染线程（Blt 之前调用）：把色彩调整应用到视频处理器的过滤器上。
         * 只在脏的时候做一次 —— 之后每帧零代价。实现见 .cpp 里的说明。
         */
        void applyColorAdjust();

        std::atomic<int> m_brightness{100};
        std::atomic<int> m_contrast{100};
        std::atomic<int> m_saturation{100};
        std::atomic<bool> m_colorDirty{true};

    private:
        /* 确保输入视图存在（解码纹理换了就重建）。 */
        bool ensureInputView(ID3D11Texture2D *texture, int arraySlice, int width, int height,
                             int dxgiFormat);
        /*
         * 确保处理器/枚举器存在。枚举器带的是"输入纹理尺寸 -> 输出显示尺寸"，
         * 所以这两组尺寸里任何一个变了都得重建。
         *
         * inputColorSpace 是 DXGI_COLOR_SPACE_TYPE：告诉视频处理器输入是 BT.709 还是
         * PQ/HLG-P2020。输入是 HDR 而输出是 SDR 时驱动会在这条路径上做色调映射，
         * 这就是 HDR 片源在 SDR 输出下不再发灰的原因。
         */
        bool ensureProcessor(int textureWidth, int textureHeight, int displayWidth,
                             int displayHeight, int dxgiFormat, int inputColorSpace);
        bool ensureOutputTexture(int width, int height);

        ID3D11Device *m_device = nullptr;
        ID3D11DeviceContext *m_context = nullptr;
        ID3D11VideoDevice *m_videoDevice = nullptr;
        ID3D11VideoContext *m_videoContext = nullptr;
        /* 色彩空间（HDR 色调映射）用的是 DXGI 1.6 那一层接口，拿不到也能跑。 */
        ID3D11VideoContext1 *m_videoContext1 = nullptr;
        ID3D11VideoProcessorEnumerator *m_enumerator = nullptr;
        ID3D11VideoProcessor *m_processor = nullptr;
        ID3D11VideoProcessorInputView *m_inputView = nullptr;
        ID3D11VideoProcessorOutputView *m_outputView = nullptr;
        ID3D11Texture2D *m_outputTexture = nullptr;

        /* 当前输入视图对应的解码纹理信息，用来判断要不要重建视图。 */
        ID3D11Texture2D *m_inputTexture = nullptr;
        int m_inputSlice = -1;
        int m_inputWidth = 0;
        int m_inputHeight = 0;
        /* 输入纹理的 DXGI 格式：NV12(8bit) 或 P010(10bit HDR)。 */
        int m_inputFormat = 0;
        /* 处理器建好时用的输入色彩空间，变了吗就要重建。 */
        int m_inputColorSpace = 0;
        /* 色彩空间是否已经设过（只打一次日志用）。 */
        bool m_loggedColorSpace = false;

        /*
         * 处理器建好时用的输出尺寸（枚举器的 OutputWidth/Height）。
         *
         * 注意这两个字段**只属于处理器**：输出纹理自己有 m_outputTextureWidth/Height。
         * 两者必须分开记 —— 合成一个字段的话，ensureProcessor() 刚把新尺寸写进来，
         * ensureOutputTexture() 就会误判成"尺寸没变"而不重建纹理，窗口改变尺寸之后
         * 处理器和后台纹理尺寸对不上，Blt 会直接失败。
         */
        int m_outputWidth = 0;
        int m_outputHeight = 0;
        /* 实际那张 RGBA 输出纹理的尺寸（等于视频显示尺寸，和窗口大小无关）。 */
        int m_outputTextureWidth = 0;
        int m_outputTextureHeight = 0;

        bool m_failed = false;
        bool m_loggedFirstFrame = false;

        /*
         * 【新增·追加在成员表末尾】输出纹理那张 QSGTexture 包装 + 它属于哪个"输出纹理世代"。
         *
         * 为什么要缓存它：`wrapD3D11Texture()` 每调一次，Qt 就新建一个 QSGTexture 并在
         * D3D11 上建一个 SRV。而这张包装包的是**我们自己的** m_outputTexture —— 尺寸不变
         * 就一直是同一个 D3D11 对象，变的只是它的内容（视频处理器 Blt 写进去的）。
         * 所以包装一次就够，每帧只需要重新 Blt 一次。
         *
         * 实测动机（2026-09-30 真机日志）：弹幕层让场景图按 60Hz 重绘，视频项每次重绘都
         * 会重新走 textureForFrame()（帧对象每帧都是新的 clone，帧指针判据必然落空），
         * 于是 **60 次/秒新建 QSGTexture + SRV**；对应的现象是 `nvwgf2umx.dll`
         * （NVIDIA D3D11 用户态驱动）线程 +18 条/轮、句柄 +~100/轮、私有提交 +~50MB/轮，
         * 而且窗口关掉之后都不还回来。
         *
         * 生命周期：**不持有**（那张 QSGTexture 归场景图节点），节点重建/销毁时必须用
         * forgetOutputTextureWrapper() 把这里的记录清掉。
         */
        QSGTexture *m_outputQsTexture = nullptr;
        int m_outputQsTextureGeneration = -1;
        /* 每重建一次输出纹理 +1；包装的世代对不上就重新包一次 */
        int m_outputTextureGeneration = 0;

        /*
         * 【新增·追加在成员表末尾】prepare() 是在 Qt 的**渲染线程**上被调用的，这里记下
         * 那条线程的 id。
         *
         * 为什么必须记：`ID3D11DeviceContext::Flush()` 用的是**立即上下文**，而 D3D11 的
         * 立即上下文**不是自由线程的**（同一时刻只能有一条线程用它）。但
         * releaseResources() 有一条调用路径跑在 **GUI 线程**上（~CicadaPlayerItem），
         * 所以 Flush / Trim 之前必须核对线程 —— 否则就是在和 Qt 渲染线程抢上下文。
         * （Release 那些 COM 对象不需要核对：引用计数本身是线程安全的。）
         */
        std::thread::id m_renderThreadId{};
    };

}// namespace cicadaqt

#endif// CICADA_QT_CICADATEXTURED3D11_H
