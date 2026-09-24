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
    };

}// namespace cicadaqt

#endif// CICADA_QT_CICADATEXTURED3D11_H
