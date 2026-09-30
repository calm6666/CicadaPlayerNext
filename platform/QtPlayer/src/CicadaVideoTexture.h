//
// CicadaVideoTexture —— 把一帧解码结果变成 Qt 场景图能用的 QSGTexture。
//
// 分层
// ----
//   CicadaPlayerItem（QML 组件）
//        |
//        v
//   CicadaTextureBackend（本文件）：按平台/RHI 后端选一条路
//        |-- CicadaTextureD3D11   Windows：D3D11 视频处理器 NV12->RGBA（GPU 零拷贝）
//        |-- CicadaTextureMetal   macOS  ：CVMetalTextureCache（GPU 零拷贝，见下一轮）
//        |-- CicadaTextureVAAPI   Linux  ：VAAPI -> dmabuf -> EGLImage（GPU 零拷贝）
//        '-- CPU 回退（本文件内）：swscale 转 BGRA -> QImage -> QSGTexture
//
// 为什么要 CPU 回退
// ----------------
// 零拷贝对"解码格式 + RHI 后端 + 驱动"都有要求（例如 Qt 的场景图被强制成 OpenGL、
// 或者 Qt 的 D3D11 设备没开视频支持）。任何一环不满足时，我们不希望用户看到黑屏或
// 崩溃，而是安静地退回"下载到内存再上传"的老路 —— 慢，但一定能播。回退发生时会打
// 一条明确的日志，方便判断当前到底走的哪条路。
//
#ifndef CICADA_QT_CICADAVIDEOTEXTURE_H
#define CICADA_QT_CICADAVIDEOTEXTURE_H

#include <QtCore/QSize>
#include <QtCore/QString>
#include <atomic>
#include <memory>

class QQuickWindow;
class QSGTexture;
class QImage;
struct SwsContext;
class IAFFrame;

namespace cicadaqt {

#if defined(Q_OS_WIN)
    class CicadaTextureD3D11;
#elif defined(Q_OS_MACOS)
    class CicadaTextureMetal;
#elif defined(Q_OS_LINUX)
    class CicadaTextureVAAPI;
#endif

    /*
     * 纹理后端。所有方法都只能在 Qt 的渲染线程上调用。
     */
    class CicadaVideoTexture {
    public:
        CicadaVideoTexture();
        ~CicadaVideoTexture();

        CicadaVideoTexture(const CicadaVideoTexture &) = delete;
        CicadaVideoTexture &operator=(const CicadaVideoTexture &) = delete;

        /*
         * （重新）决定走哪条路：零拷贝，还是 CPU 回退。**只能在渲染线程上调用** ——
         * 探测要拿 Qt 的 RHI 设备，还可能释放/重建 GPU 资源。
         *
         * allowZeroCopy = false 表示应用层把硬解关了：解码器交出来的是内存里的 CPU 帧，
         * 零拷贝从根上不成立，这时连后端都不进（免得第一帧触发一次"零拷贝运行中失败"
         * 的误报日志）。
         *
         * 调用时机：场景图初始化时一次；设置页里改了硬解开关时再由 CicadaPlayerItem
         * 在 updatePaintNode() 里调一次（会先释放上一次的后端，再重新探测）。
         * 返回值同 prepare()：true = 零拷贝可用。
         */
        bool reset(QQuickWindow *window, bool allowZeroCopy);

        /*
         * 把一帧变成 QSGTexture。
         *
         * 返回值归调用方所有（交给 QSGSimpleTextureNode::setOwnsTexture(true) 最省事）；
         * 失败返回 nullptr。*flipVertically 告诉调用方采样时要不要上下翻转
         * （D3D11/Metal 的纹理原点在左上，OpenGL 在左下）。
         */
        QSGTexture *textureForFrame(QQuickWindow *window, IAFFrame *frame, bool *flipVertically);

        /* 场景图失效 / item 销毁时释放渲染资源。 */
        void releaseResources();

        /*
         * 渲染线程：放掉"跟着某一条片源走"的输入侧状态，但保留设备、处理器、枚举器和
         * 输出纹理（那些可以跨片源复用，重建一次很贵）。
         *
         * 目前只有 Windows 的 D3D11 后端有这种东西：视频处理器的**输入视图**对解码纹理
         * 持有引用，而那张解码纹理是解码器整池 surface 所在的数组纹理 —— 一个视图就够
         * 把上一条片源的整池显存钉住（实测 1080p 约 60MB、4K 约 250MB）。
         * macOS / Linux 的后端这里是空实现，见 .cpp 里的说明。
         */
        void releaseInputState();

        /*
         * 渲染线程：忘掉"输出纹理那张 QSGTexture 包装"的记录（只丢指针，不 delete）。
         *
         * 只在**场景图节点被删掉/重建**的那一刻调用：那张 QSGTexture 归节点所有，
         * 节点一没它就没了（见 CicadaTextureD3D11.h 里 m_outputQsTexture 那段）。
         * 非 D3D11 后端空实现。
         */
        void forgetOutputTextureWrapper();

        /*
         * 渲染线程：逼 D3D11 立刻销毁"被延迟销毁"的对象（内部就是
         * ID3D11DeviceContext::Flush）。
         *
         * 调用时机：**刚放掉解码帧/输入视图之后** —— 引用计数归零只是"可以销毁了"，
         * D3D11 默认还会拖一段时间；Flush 才让它当场消失（官方文档原文见
         * CicadaTextureD3D11.h 里同名函数）。非 D3D11 后端是空实现。
         */
        void flushDeferredDestruction();

        /* 诊断：当前实际走的路径，界面上可以直接显示。 */
        QString backendName() const;

        /* 是否正在用零拷贝（有 GPU 纹理直通）。 */
        bool isZeroCopy() const
        {
            return m_zeroCopyActive;
        }

        /*
         * ---- 色彩调整（右键菜单 →「视频色彩调整」）----
         *
         * 取值范围 **0~200，100 = 中性**（就是面板上那三条滑块的原始值）。
         * GUI 线程写（QML 拖滑块）、渲染线程读（真正应用到 GPU），所以下面三个值都是原子量。
         *
         * 【实现位置】Windows 的 D3D11 零拷贝路径：视频处理器自带 brightness / contrast /
         * saturation / hue 四个「过滤器」（ID3D11VideoContext::VideoProcessorSetStreamFilter，
         * 范围由 GetVideoProcessorFilterRange 给出）—— 在那里应用是**不花额外代价**的
         * （本来就要 Blt 一次做 NV12→RGBA）。
         *
         * 【其它后端】macOS(Metal) / Linux(VAAPI) / CPU 回退这条修改目前**不生效** ——
         * 它们要在各自的转换里另做（CIFilter / VAAPI 的 ProcAmp / CPU 逐像素），
         * 所以 colorAdjustSupported() 会返回 false，界面会如实说明（见 PlayerColorPanel）。
         */
        void setColorAdjust(int brightness, int contrast, int saturation);
        int brightness() const;
        int contrast() const;
        int saturation() const;
        /* 色彩调整在当前这条后端上是否真的生效。 */
        virtual bool colorAdjustSupported() const;
        /*
         * 渲染线程：取走"还没应用过"的标记（取一次清一次）。
         * 后端的处理器重建时也要自己把标记置起来（见 setColorAdjustDirty()）。
         */
        bool takeColorAdjustDirty();
        void setColorAdjustDirty();

    private:
        /* reset() 的实现：探测一次并缓存结论（m_prepared 为真时直接返回上次结论）。 */
        bool prepare(QQuickWindow *window);

        /* CPU 回退：把任意格式的帧转成 BGRA 再交给 Qt 上传。 */
        QSGTexture *textureForFrameCpu(QQuickWindow *window, IAFFrame *frame);

        bool m_prepared = false;
        /*
         * 允不允许走零拷贝（默认允许）。false 来自"硬解被关掉"：这时 prepare() 直接
         * 判 CPU 路径，不碰后端。
         */
        bool m_zeroCopyAllowed = true;
        /*
         * 零拷贝是否真的在用：prepare() 探测通过就置 true（此时零拷贝"可用"），
         * 之后**第一帧真跑通**才算最终成功；中途任何一环失败都会把它打回 false
         * 并永久切到 CPU 路径。
         *
         * 注意：组件必须在 Prepare() 之前问这个值（那时还没有任何一帧），所以判定
         * 依据只能是 prepare() 的结果，见 CicadaPlayerItem::startPlaybackWhenReady()。
         */
        bool m_zeroCopyActive = false;
        /* 已经打过一次"回退到 CPU"的日志了，别每帧刷。 */
        bool m_loggedFallback = false;

        /* CPU 回退用的转换上下文和目标图像。 */
        SwsContext *m_sws = nullptr;
        int m_swsSrcWidth = 0;
        int m_swsSrcHeight = 0;
        int m_swsSrcFormat = 0;
        QImage *m_bgraImage = nullptr;

#if defined(Q_OS_WIN)
        std::unique_ptr<CicadaTextureD3D11> m_d3d11;
#elif defined(Q_OS_MACOS)
        std::unique_ptr<CicadaTextureMetal> m_metal;
#elif defined(Q_OS_LINUX)
        std::unique_ptr<CicadaTextureVAAPI> m_vaapi;
#endif

        /* 色彩调整（0~200，100 = 中性）：GUI 线程写、渲染线程读，所以是原子量。 */
        std::atomic<int> m_brightness{100};
        std::atomic<int> m_contrast{100};
        std::atomic<int> m_saturation{100};
        /* 有还没应用到后端的改动（渲染线程取一次清一次）。 */
        std::atomic<bool> m_colorAdjustDirty{true};
    };

}// namespace cicadaqt

#endif// CICADA_QT_CICADAVIDEOTEXTURE_H
