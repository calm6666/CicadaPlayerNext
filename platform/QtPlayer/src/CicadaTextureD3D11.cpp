//
// Windows D3D11 零拷贝后端的实现，设计说明见 CicadaTextureD3D11.h。
//

#include "CicadaTextureD3D11.h"
#include "CicadaQtTextureWrap.h"

#if defined(Q_OS_WIN)

/*
 * 顺序很重要：d3d11.h 先以 **C++** 链接进来，之后 FFmpeg 的
 * libavutil/hwcontext_d3d11va.h 也会 #include <d3d11.h>，但它在 include guard
 * 之后是空操作，于是 D3D11_VIEWPORT/D3D11_RECT/D3D11_BOX 的 C++ 比较运算符不会
 * 拿到 C 链接（否则 MSVC 会报一片 C2733）。
 */
#include <d3d11_1.h>
/* IDXGIDevice3::Trim：逼 D3D 立刻销毁延迟销毁的对象（见 trimVideoMemory 的说明）。 */
#include <dxgi1_3.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixfmt.h>
}

#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>
#include <QtCore/QSize>
#include <QtCore/QDebug>

#include <utils/frame_work_log.h>
#include "base/media/AVAFPacket.h"

/*
 * 这里不需要 using namespace Cicada：IAFFrame / AVAFPacket / AF_PIX_FMT_* 都在
 * 全局命名空间里（framework/base/media/IAFPacket.h、framework/utils/AFMediaType.h）。
 */

namespace cicadaqt {

    /*
     * 视频处理器的输出格式。
     *
     * **必须是 R8G8B8A8_UNORM，不能写成 B8G8R8A8_UNORM。**
     *
     * 原因在 Qt 那一侧：QSGTexture 根本没有"像素格式"这个参数，所以
     * QNativeInterface::QSGD3D11Texture::fromNative() 只能按 Qt 场景图自己的约定去
     * 包装这张纹理 —— 也就是 RGBA8（D3D11 后端把 QRhiTexture::RGBA8 映射成
     * DXGI_FORMAT_R8G8B8A8_UNORM，见 qrhid3d11.cpp 的格式表；QRhiTexture 的
     * NativeTexture 结构里只有 object/layout，没有格式字段，所以 Qt 也用不上
     * "读出我们纹理的真实格式"这条路）。
     *
     * 而 D3D11 规定：**有类型的（非 typeless）**纹理只能用完全相同的格式建视图。
     * 给它一张 B8G8R8A8_UNORM 的纹理，Qt 去建 R8G8B8A8_UNORM 的 SRV 会直接
     * E_INVALIDARG 失败 —— 就是实测日志里那句
     *     Failed to create srv: COM error 0x80070057
     * 材质拿不到可采样的纹理，于是**解码正常、帧也交出去了，画面却是全黑的**。
     *
     * 颜色通道不会因此乱掉：视频处理器按**纹理声明的格式**写数据（声明 RGBA 就按
     * R,G,B,A 排），Qt 又按同一格式采样，两边一致。
     *
     * 这个格式会先过 CheckVideoProcessorFormat() 确认驱动支持，不支持就自动退到
     * CPU 路径（见 ensureProcessor）。
     */
#define CICADA_QT_D3D11_OUTPUT_FORMAT DXGI_FORMAT_R8G8B8A8_UNORM

    /* 解码纹理的 DXGI 格式：8bit 是 NV12，10bit HDR 是 P010。 */
    static int dxgiFormatForSwFormat(int swFormat)
    {
        return (swFormat == AV_PIX_FMT_P010) ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    }

    /*
     * 由帧的色彩元数据推出视频处理器的**输入色彩空间**。
     *
     * 元数据来自解码器（FFmpeg 把容器的 color_trc / color_primaries 放在帧上）：
     *   * PQ(HDR10)            -> YCBCR_STUDIO_G2084_TOPLEFT_P2020
     *   * HLG                  -> YCBCR_STUDIO_GHLG_TOPLEFT_P2020
     *   * BT.2020（非 PQ/HLG） -> YCBCR_STUDIO_G22_TOPLEFT_P2020
     *   * 其它（含未标注）      -> YCBCR_STUDIO_G22_LEFT_P709
     *
     * 这个值决定驱动怎么做 YUV->RGB：输入标 HDR、输出是 SDR(BT.709 G22) 时，
     * 驱动会顺手做**色调映射**，HDR 片源因此不会发灰；标错了（例如把 HDR 当 709）
     * 画面会又暗又灰。
     */
    static int inputColorSpaceForFrame(const AVFrame *frame)
    {
        if (frame == nullptr) {
            return DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
        }

        if (frame->color_trc == AVCOL_TRC_SMPTE2084) {
            return DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020;
        }

        if (frame->color_trc == AVCOL_TRC_ARIB_STD_B67) {
            return DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020;
        }

        if (frame->color_primaries == AVCOL_PRI_BT2020) {
            return DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_TOPLEFT_P2020;
        }

        return DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    }

    CicadaTextureD3D11::CicadaTextureD3D11() = default;

    CicadaTextureD3D11::~CicadaTextureD3D11()
    {
        /*
         * 析构时不一定还在渲染线程上（item 销毁可能发生在 UI 线程），所以这里
         * 不主动释放 D3D11 对象：正常路径下 releaseResources() 已经在场景图失效
         * 时被调用过了。真漏了也只是进程退出时的一次引用计数残留，不会崩。
         */
    }

    bool CicadaTextureD3D11::prepare(QQuickWindow *window)
    {
        if (m_device != nullptr) {
            return true;
        }

        if (window == nullptr) {
            return false;
        }

        auto *rif = window->rendererInterface();

        if (rif == nullptr) {
            AF_LOGE("Qt renderer interface is null, cannot use the D3D11 zero-copy path");
            m_failed = true;
            return false;
        }

        if (rif->graphicsApi() != QSGRendererInterface::Direct3D11) {
            /*
             * 场景图不是跑在 D3D11 上（比如用 QSG_RHI_BACKEND=opengl 强制过）。
             * 那不是错误，只是这条零拷贝路走不通，交给 CPU 回退路径。
             */
            AF_LOGW("Qt scene graph is not using D3D11 (api=%d), "
                    "the D3D11 zero-copy path is unavailable\n", (int) rif->graphicsApi());
            m_failed = true;
            return false;
        }

        /*
         * QSGRendererInterface::DeviceResource / DeviceContextResource 就是场景图
         * 正在用的 D3D11 设备和立即上下文，只能在渲染线程上问（我们现在就在）。
         */
        m_device = static_cast<ID3D11Device *>(
                       rif->getResource(window, QSGRendererInterface::DeviceResource));
        m_context = static_cast<ID3D11DeviceContext *>(
                        rif->getResource(window, QSGRendererInterface::DeviceContextResource));

        if (m_device == nullptr || m_context == nullptr) {
            AF_LOGE("Qt gave no D3D11 device/context, the D3D11 zero-copy path is unavailable");
            m_failed = true;
            return false;
        }

        /* 记下渲染线程：Flush / Trim 只能在同一条线程上做（立即上下文不是自由线程的）。 */
        m_renderThreadId = std::this_thread::get_id();

        /*
         * 视频处理需要 ID3D11VideoDevice / ID3D11VideoContext。
         *
         * 这里有可能是**拿不到**的：D3D11 设备要带 D3D11_CREATE_DEVICE_VIDEO_SUPPORT
         * 创建才保证支持视频接口，而设备是 Qt 建的（用哪块卡、怎么回退都归 Qt 管，
         * 我们不去改）。拿不到不是崩溃性的问题 —— 自动退到 CPU 路径（下载 + 上传），
         * 硬解本身还在（FFmpeg 会用自己的设备解码），画面照常播，只是没有零拷贝。
         */
        HRESULT hr = m_device->QueryInterface(IID_PPV_ARGS(&m_videoDevice));

        if (FAILED(hr)) {
            AF_LOGW("Qt's D3D11 device has no ID3D11VideoDevice (hr=0x%08lx): "
                    "zero-copy needs it, falling back to the CPU path (the picture is "
                    "downloaded from the GPU and uploaded as a texture; hardware decoding "
                    "is unaffected)\n", (long) hr);
            releaseResources();
            m_failed = true;
            return false;
        }

        hr = m_context->QueryInterface(IID_PPV_ARGS(&m_videoContext));

        if (FAILED(hr)) {
            AF_LOGW("Qt's D3D11 device context has no ID3D11VideoContext (hr=0x%08lx), "
                    "falling back to the CPU path\n", (long) hr);
            releaseResources();
            m_failed = true;
            return false;
        }

        /*
         * 色彩空间接口（ID3D11VideoContext1，DXGI 1.6）。拿不到也能跑，只是 HDR 内容
         * 会由驱动按默认假设处理（可能发灰），所以这里只警告、不失败。
         */
        if (FAILED(m_context->QueryInterface(IID_PPV_ARGS(&m_videoContext1)))) {
            m_videoContext1 = nullptr;
            AF_LOGW("ID3D11VideoContext1 is not available: HDR content may look washed out "
                    "(no explicit colour space / tone mapping)\n");
        }

        AF_LOGI("D3D11 zero-copy is ready: decoded NV12/P010 textures are converted to "
                "RGBA by the D3D11 video processor and handed to the Qt scene graph "
                "(no CPU copy at all)\n");
        return true;
    }

    void CicadaTextureD3D11::releaseInputView()
    {
        if (m_inputView != nullptr) {
            m_inputView->Release();
            m_inputView = nullptr;
        }

        /*
         * 缓存的那张解码纹理指针必须一起清掉：它只是个裸指针（我们不对它 AddRef，
         * 纹理的存活靠"输入视图 + 正在显示的帧"这两个引用）。留着的话
         * ensureInputView() 会拿一个已经失效的地址去和 m_inputTexture 比，
         * 判成"同一张纹理"就直接复用那个已经释放的视图。
         * 清掉之后下一次调用必然重建视图（那里的判据就是 m_inputView）。
         */
        m_inputTexture = nullptr;
        m_inputSlice = -1;
    }

    void CicadaTextureD3D11::flushDeferredDestruction()
    {
        if (m_context == nullptr) {
            return;
        }

        /*
         * 立即上下文不是自由线程的：只允许在 prepare() 那条（Qt 渲染）线程上碰它。
         * releaseResources() 有一条路径跑在 GUI 线程（~CicadaPlayerItem），那里只做
         * Release（引用计数本身线程安全），Flush 到下面 trimVideoMemory() 也一样被挡。
         */
        if (std::this_thread::get_id() != m_renderThreadId) {
            AF_LOGW("[mem] skip ID3D11DeviceContext::Flush(): this is not the Qt render thread, "
                    "and the immediate context is not free-threaded (the deferred objects will be "
                    "destroyed by the render thread's own Flush, or by the runtime)\n");
            return;
        }

        /*
         * 只 Flush，不 ClearState。理由（官方文档原文与取舍）全部写在头文件
         * CicadaTextureD3D11.h 的 flushDeferredDestruction() 上：Flush 只"把排队的
         * 命令推给 GPU"，并借此销毁被**延迟销毁**的对象；ClearState 会把 Qt 场景图
         * 正在用的管线绑定全清成 NULL，和 Qt RHI 自己的状态跟踪对不上。
         *
         * 这一步是"pool 释放不掉"的最后一环：引用计数归零只代表我们要放的东西"可以"
         * 销毁了，D3D11 默认还要拖一段时间；Flush 才让它当场消失。
         */
        m_context->Flush();

        AF_LOGI("[mem] ID3D11DeviceContext::Flush() after the decoder frames were dropped: "
                "Direct3D destroys the deferred objects now (the D3D11VA surface array is one "
                "of them)\n");
    }

    void CicadaTextureD3D11::trimVideoMemory()
    {
        if (m_device == nullptr) {
            return;
        }

        /* 同 flushDeferredDestruction()：Trim 也要驱动去动上下文/设备，只在渲染线程做。 */
        if (std::this_thread::get_id() != m_renderThreadId) {
            AF_LOGW("[mem] skip IDXGIDevice3::Trim(): this is not the Qt render thread "
                    "(the driver-internal buffers stay allocated until the next trim)\n");
            return;
        }

        IDXGIDevice3 *dxgiDevice3 = nullptr;
        IDXGIDevice *dxgiDevice = nullptr;

        HRESULT hr = m_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));

        if (SUCCEEDED(hr) && dxgiDevice != nullptr) {
            hr = dxgiDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice3));
            dxgiDevice->Release();
        }

        if (FAILED(hr) || dxgiDevice3 == nullptr) {
            AF_LOGW("[mem] the D3D11 device exposes no IDXGIDevice3 (hr=0x%08lx): the "
                    "driver-internal graphics memory cannot be trimmed on this system\n", (long) hr);
            return;
        }

        dxgiDevice3->Trim();
        dxgiDevice3->Release();

        AF_LOGI("[mem] IDXGIDevice3::Trim() done: Direct3D destroyed the deferred objects "
                "immediately and the driver handed its internal buffers back (these buffers "
                "count against the app's memory usage)\n");
    }

    void CicadaTextureD3D11::releaseResources()
    {
        /*
         * 记下"放掉了多大一张输出纹理"用于日志（函数末尾会把这两个数清零）。
         * 输出纹理是整幅 RGBA：1080p = 1920*1080*4 = 8.29MB，4K = 3840*2160*4 = 33.18MB。
         */
        const int releasedOutputWidth = m_outputTextureWidth;
        const int releasedOutputHeight = m_outputTextureHeight;

        if (m_outputView != nullptr) {
            m_outputView->Release();
            m_outputView = nullptr;
        }

        if (m_inputView != nullptr) {
            m_inputView->Release();
            m_inputView = nullptr;
        }

        if (m_processor != nullptr) {
            m_processor->Release();
            m_processor = nullptr;
        }

        if (m_enumerator != nullptr) {
            m_enumerator->Release();
            m_enumerator = nullptr;
        }

        if (m_outputTexture != nullptr) {
            m_outputTexture->Release();
            m_outputTexture = nullptr;
        }

        /* 包装那张 QSGTexture 也一起忘掉（它归节点所有，节点马上就会被场景图删掉）。 */
        m_outputQsTexture = nullptr;
        m_outputQsTextureGeneration = -1;

        if (m_videoContext != nullptr) {
            m_videoContext->Release();
            m_videoContext = nullptr;
        }

        if (m_videoContext1 != nullptr) {
            m_videoContext1->Release();
            m_videoContext1 = nullptr;
        }

        if (m_videoDevice != nullptr) {
            m_videoDevice->Release();
            m_videoDevice = nullptr;
        }

        /*
         * ============ 【官方文档给的最后一步：逼 D3D 现在就把对象销毁掉】============
         *
         * 上面那 8 个 COM 对象都已经 Release 了，但"Release 完就回收了"是错的。
         * ID3D11DeviceContext::Flush 的文档原文：
         *   "Direct3D 11 defers the destruction of objects. Therefore, an application
         *    can't rely upon objects immediately being destroyed. By calling Flush, you
         *    destroy any objects whose destruction was deferred."
         * 也就是说**引用计数归零只代表"可以销毁"**，真正回收要等运行时愿意 —— 这正是
         * "关窗/切档之后显存与内存迟迟不降"最容易被忽略的一环。
         *
         * 第二步 Trim 是给**空闲**时刻的（场景图失效 / 关窗 / 销毁播放器都属于）。
         * IDXGIDevice3::Trim 的文档：它让 D3D 立刻销毁延迟销毁的对象，并把**驱动内部
         * 缓存的内存**还给系统，而那些缓冲 "count against the app's memory usage" ——
         * 关窗后不降的那几十 MB 里就有它一份。
         *
         * 顺序按文档来：先 Release（上面全部）→ 再 Flush（逼延迟销毁的对象当场销毁）
         * → 再 Trim（连驱动内部缓冲一起还）。Trim 的文档另外要求
         * "release references on middleware before calling Trim" —— FFmpeg 就是这个
         * middleware，它的表面池在 close_decoder() 里已经放掉了。
         *
         * 为什么**不**在这里调 ClearState：官方那条"同步销毁"配方里的 ClearState 会把
         * 所有资源槽 / shaders / viewport 置 NULL，而 m_context 是 **Qt 场景图的立即
         * 上下文**，清了就和 Qt RHI 自己的状态记账对不上（完整取舍见头文件里
         * flushDeferredDestruction() 的说明）。
         */
        /*
         * 走 flushDeferredDestruction() 而不是直接 m_context->Flush()：本函数有一条调用
         * 路径在 GUI 线程上（~CicadaPlayerItem），而立即上下文不是自由线程的 ——
         * 那条路会在里面被线程检查挡掉并打一条日志（Release 本身不需要挡，
         * 引用计数是线程安全的）。
         */
        flushDeferredDestruction();

        trimVideoMemory();

        /*
         * m_device / m_context 是 Qt（场景图）的，不是我们的：这里只是丢掉指针，
         * 不做 Release。它们的生命周期由 QQuickWindow 保证。
         */
        m_device = nullptr;
        m_context = nullptr;

        m_inputTexture = nullptr;
        m_inputSlice = -1;
        m_inputWidth = 0;
        m_inputHeight = 0;
        m_inputFormat = 0;
        m_outputWidth = 0;
        m_outputHeight = 0;
        m_outputTextureWidth = 0;
        m_outputTextureHeight = 0;
        m_inputColorSpace = 0;

        /*
         * 【释放探针】零拷贝那 8 个 COM 对象（输入/输出视图、视频处理器、枚举器、
         * 输出纹理、VideoDevice/VideoContext/VideoContext1）都已经 Release。
         *
         * 这条日志也是"~CicadaTextureD3D11 那个故意不 Release 的空析构"的判据：
         * 正常关窗路径上本函数一定先被调用（场景图失效，或 ~CicadaPlayerItem 里那一句），
         * 析构时所有成员都已经是 nullptr，所以不存在"析构时还有纹理没回收"。
         */
        AF_LOGI("[mem] D3D11 zero-copy resources released: RGBA output texture %dx%d "
                "(%lld bytes), output/input views + video processor + enumerator + "
                "video device/context released\n",
                releasedOutputWidth, releasedOutputHeight,
                (long long) releasedOutputWidth * releasedOutputHeight * 4);
    }

    bool CicadaTextureD3D11::ensureProcessor(int textureWidth, int textureHeight,
            int displayWidth, int displayHeight, int dxgiFormat, int inputColorSpace)
    {
        if (m_processor != nullptr && m_inputWidth == textureWidth &&
                m_inputHeight == textureHeight && m_inputFormat == dxgiFormat &&
                m_outputWidth == displayWidth && m_outputHeight == displayHeight &&
                m_inputColorSpace == inputColorSpace) {
            return true;
        }

        /* 尺寸/格式变了：输入视图和输出视图都得重建。 */
        if (m_inputView != nullptr) {
            m_inputView->Release();
            m_inputView = nullptr;
            m_inputTexture = nullptr;
            m_inputSlice = -1;
        }

        if (m_outputView != nullptr) {
            m_outputView->Release();
            m_outputView = nullptr;
        }

        if (m_processor != nullptr) {
            m_processor->Release();
            m_processor = nullptr;
        }

        if (m_enumerator != nullptr) {
            m_enumerator->Release();
            m_enumerator = nullptr;
        }

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        /*
         * 输入用**纹理**尺寸：解码纹理按编码尺寸对齐过（例如 3840x2176），
         * 真正的画面只有上面 2160 行，靠 stream source rect 裁掉填充行；
         * 输出用**显示**尺寸：视频处理器直接输出到和画面等大的 RGBA 纹理，
         * 之后由 Qt 场景图做缩放（采样时按 item 矩形缩放，GPU 完成）。
         */
        content.InputWidth = static_cast<UINT>(textureWidth);
        content.InputHeight = static_cast<UINT>(textureHeight);
        content.OutputWidth = static_cast<UINT>(displayWidth);
        content.OutputHeight = static_cast<UINT>(displayHeight);
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        HRESULT hr = m_videoDevice->CreateVideoProcessorEnumerator(&content, &m_enumerator);

        if (FAILED(hr)) {
            AF_LOGE("CreateVideoProcessorEnumerator failed (hr=0x%08lx)\n", (long) hr);
            return false;
        }

        UINT flags = 0;

        if (FAILED(m_enumerator->CheckVideoProcessorFormat(static_cast<DXGI_FORMAT>(dxgiFormat),
                   &flags)) ||
                (flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0) {
            AF_LOGE("the video processor cannot take input format %d\n", dxgiFormat);
            return false;
        }

        flags = 0;

        if (FAILED(m_enumerator->CheckVideoProcessorFormat(CICADA_QT_D3D11_OUTPUT_FORMAT, &flags)) ||
                (flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
            AF_LOGE("the video processor cannot write format %d "
                    "(改 CicadaTextureD3D11.cpp 里的 CICADA_QT_D3D11_OUTPUT_FORMAT 试试)\n",
                    (int) CICADA_QT_D3D11_OUTPUT_FORMAT);
            return false;
        }

        hr = m_videoDevice->CreateVideoProcessor(m_enumerator, 0, &m_processor);

        if (FAILED(hr)) {
            AF_LOGE("CreateVideoProcessor failed (hr=0x%08lx)\n", (long) hr);
            return false;
        }

        /*
         * 色彩空间：告诉视频处理器输入是什么（BT.709 / BT.2020-PQ / BT.2020-HLG），
         * 输出是什么。
         *
         * 输出固定按 **SDR BT.709 G22** 声明：我们交给 Qt 的是一张 8bit RGBA 纹理，
         * Qt 的场景图也按 sRGB 合成它，所以这里必须声明成 SDR；输入是 HDR 时驱动会
         * 在这条路径上做色调映射 —— 这就是 HDR 片源在 SDR 输出下不再发灰的原因。
         *
         * （真正的 HDR 直通需要 10bit + PQ 的输出纹理以及 Qt 6.8+ 的色彩管理，
         * 那是后续工作，见 README。）
         */
        if (m_videoContext1 != nullptr) {
            m_videoContext1->VideoProcessorSetStreamColorSpace1(
                m_processor, 0, static_cast<DXGI_COLOR_SPACE_TYPE>(inputColorSpace));
            m_videoContext1->VideoProcessorSetOutputColorSpace1(
                m_processor, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);

            if (!m_loggedColorSpace || m_inputColorSpace != inputColorSpace) {
                m_loggedColorSpace = true;
                AF_LOGI("video processor colour spaces set: input %d -> output SDR BT.709 "
                        "(HDR content is tone mapped by the driver)\n", inputColorSpace);
            }
        }

        m_inputWidth = textureWidth;
        m_inputHeight = textureHeight;
        m_inputFormat = dxgiFormat;
        m_inputColorSpace = inputColorSpace;
        m_outputWidth = displayWidth;
        m_outputHeight = displayHeight;
        /*
         * 处理器是新的 → 之前设过的色彩调整（亮度/对比度/饱和度）都跟着没了，
         * 标脏让下面 applyColorAdjust() 在第一帧就重新应用一次。
         */
        m_colorDirty.store(true);
        applyColorAdjust();
        AF_LOGI("D3D11 video processor ready: %dx%d texture (format %d) -> %dx%d RGBA\n",
                textureWidth, textureHeight, dxgiFormat, displayWidth, displayHeight);
        return true;
    }

    void CicadaTextureD3D11::setColorAdjust(int brightness, int contrast, int saturation)
    {
        /* 0~200，100 = 中性（和界面那三条滑块同量纲） */
        m_brightness.store(qBound(0, brightness, 200));
        m_contrast.store(qBound(0, contrast, 200));
        m_saturation.store(qBound(0, saturation, 200));
        m_colorDirty.store(true);
    }

    /*
     * 把"视频色彩调整"应用到视频处理器。
     *
     * 用的是 D3D11 视频处理器自带的**过滤器**（不是 ProcAmp —— 那套 API 是另一种封装）：
     *     ID3D11VideoProcessorEnumerator::GetVideoProcessorFilterRange(filter, &range)
     *         range = { Minimum, Maximum, Default, Multiplier }（Level 是整数刻度）
     *     ID3D11VideoContext::VideoProcessorSetStreamFilter(processor, stream, filter, Enable, Level)
     * 四个过滤器：BRIGHTNESS / CONTRAST / HUE / SATURATION（d3d11.h:10473-10476）。
     *
     * 【取值映射】面板给的是 0~200（100 = 中性）。映射方式：
     *   100 → range.Default（驱动给的中性值，通常 0）
     *   >100 → Default + (Maximum - Default) × (value-100)/100
     *   <100 → Default - (Default - Minimum) × (100-value)/100
     * 中性时直接**关掉过滤器**（Enable=FALSE），免得驱动在中性值上还走一遍滤镜。
     *
     * 【代价】这几句是处理器状态设置，只在值变了之后设一次（脏标记），之后每帧的代价为零。
     * 支持情况不定的驱动：GetVideoProcessorFilterRange 失败就跳过这一项（不影响播放）。
     */
    void CicadaTextureD3D11::applyColorAdjust()
    {
        if (!m_colorDirty.exchange(false) || m_processor == nullptr || m_videoContext == nullptr ||
                m_enumerator == nullptr) {
            return;
        }

        const int brightnessValue = m_brightness.load();
        const int contrastValue = m_contrast.load();
        const int saturationValue = m_saturation.load();
        const int values[4] = { brightnessValue, contrastValue, saturationValue, 100 };
        const D3D11_VIDEO_PROCESSOR_FILTER filters[4] = {
            D3D11_VIDEO_PROCESSOR_FILTER_BRIGHTNESS,
            D3D11_VIDEO_PROCESSOR_FILTER_CONTRAST,
            D3D11_VIDEO_PROCESSOR_FILTER_SATURATION,
            D3D11_VIDEO_PROCESSOR_FILTER_HUE
        };

        for (int i = 0; i < 4; ++i) {
            if (values[i] == 100) {
                /* 中性：关掉（Hue 界面没提供滑块，永远走这一支） */
                m_videoContext->VideoProcessorSetStreamFilter(m_processor, 0, filters[i], FALSE, 0);
                continue;
            }

            D3D11_VIDEO_PROCESSOR_FILTER_RANGE range = {};

            if (FAILED(m_enumerator->GetVideoProcessorFilterRange(filters[i], &range))) {
                continue;   /* 这项驱动不支持：跳过，不影响其它项 */
            }

            const float t = (values[i] - 100) / 100.0f;
            INT level = range.Default;

            if (t >= 0.0f) {
                level = range.Default + static_cast<INT>((range.Maximum - range.Default) * t);
            } else {
                level = range.Default - static_cast<INT>((range.Default - range.Minimum) * (-t));
            }

            if (level < range.Minimum) {
                level = range.Minimum;
            }

            if (level > range.Maximum) {
                level = range.Maximum;
            }

            m_videoContext->VideoProcessorSetStreamFilter(m_processor, 0, filters[i], TRUE, level);
        }

        AF_LOGI("colour adjust applied: brightness=%d contrast=%d saturation=%d (0~200, 100 = neutral)\n",
                brightnessValue, contrastValue, saturationValue);
    }

    bool CicadaTextureD3D11::ensureOutputTexture(int width, int height)
    {
        /*
         * 这里比的是**输出纹理自己**记录的尺寸，不能用处理器的 m_outputWidth
         * （处理器的那个字段在 ensureProcessor 里刚被改成新值，会误判成"没变"）。
         */
        if (m_outputTexture != nullptr && m_outputTextureWidth == width &&
                m_outputTextureHeight == height) {
            return true;
        }

        if (m_outputView != nullptr) {
            m_outputView->Release();
            m_outputView = nullptr;
        }

        if (m_outputTexture != nullptr) {
            m_outputTexture->Release();
            m_outputTexture = nullptr;
        }

        /*
         * 输出纹理要换新的了：**旧包装（如果还在）包的是旧纹理**，作废。
         * 只丢指针不 delete —— 那张 QSGTexture 归场景图节点所有。
         * 世代 +1，下面的 textureForFrame() 会重新包一次（每次重建只包一次）。
         */
        m_outputQsTexture = nullptr;
        ++m_outputTextureGeneration;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = CICADA_QT_D3D11_OUTPUT_FORMAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        /*
         * RENDER_TARGET：视频处理器的输出视图要求；
         * SHADER_RESOURCE：Qt 的场景图要拿它当纹理采样。
         */
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_outputTexture);

        if (FAILED(hr)) {
            AF_LOGE("cannot create the %dx%d RGBA texture for zero-copy (hr=0x%08lx)\n",
                    width, height, (long) hr);
            return false;
        }

        m_outputTextureWidth = width;
        m_outputTextureHeight = height;
        return true;
    }

    bool CicadaTextureD3D11::ensureInputView(ID3D11Texture2D *texture, int arraySlice, int width,
            int height, int dxgiFormat)
    {
        if (m_inputView != nullptr && m_inputTexture == texture && m_inputSlice == arraySlice &&
                m_inputWidth == width && m_inputHeight == height && m_inputFormat == dxgiFormat) {
            return true;
        }

        if (m_inputView != nullptr) {
            m_inputView->Release();
            m_inputView = nullptr;
        }

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc = {};
        desc.FourCC = 0;
        desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipSlice = 0;
        desc.Texture2D.ArraySlice = static_cast<UINT>(arraySlice);

        HRESULT hr = m_videoDevice->CreateVideoProcessorInputView(texture, m_enumerator, &desc,
                     &m_inputView);

        if (FAILED(hr)) {
            /*
             * 第一次就失败通常意味着驱动不接受"直接拿解码表面当视频处理器输入"。
             * 这时可以退到"自建一张 RENDER_TARGET 中转纹理、每帧 GPU 内拷一次"的
             * 做法（cmdline 的 D3d11DirectPresenter 就是这么兜底的）。这里先报出来，
             * 交给 CPU 回退保证能播。
             */
            AF_LOGW("CreateVideoProcessorInputView failed on the decoded texture "
                    "(hr=0x%08lx) — the driver may refuse decoder surfaces as video "
                    "processor input\n", (long) hr);
            return false;
        }

        m_inputTexture = texture;
        m_inputSlice = arraySlice;
        /* 同一 D3D11 纹理地址在解码器重建/复用时也可能对应不同尺寸或格式，
         * 这些字段必须同步缓存，否则下一次切换会误复用旧输入视图。 */
        m_inputWidth = width;
        m_inputHeight = height;
        m_inputFormat = dxgiFormat;
        return true;
    }

    QSGTexture *CicadaTextureD3D11::textureForFrame(QQuickWindow *window, IAFFrame *frame,
            bool *flipVertically)
    {
        if (flipVertically != nullptr) {
            /* D3D11 纹理原点在左上，Qt 的 D3D11 后端也是，不需要翻转。 */
            *flipVertically = false;
        }

        if (m_failed || frame == nullptr || window == nullptr || m_device == nullptr) {
            return nullptr;
        }

        auto *avFrameWrapper = dynamic_cast<AVAFFrame *>(frame);

        if (avFrameWrapper == nullptr) {
            return nullptr;
        }

        AVFrame *avFrame = avFrameWrapper->ToAVFrame();

        if (avFrame == nullptr || avFrame->hw_frames_ctx == nullptr) {
            return nullptr;
        }

        /*
         * 解码纹理的取法（libavutil/hwcontext_d3d11va.c: d3d11va_get_buffer）：
         *   data[0] = ID3D11Texture2D*
         *   data[1] = 数组切片索引
         */
        auto *texture = reinterpret_cast<ID3D11Texture2D *>(avFrame->data[0]);
        const int arraySlice = static_cast<int>(reinterpret_cast<intptr_t>(avFrame->data[1]));

        if (texture == nullptr) {
            return nullptr;
        }

        auto *framesCtx = reinterpret_cast<AVHWFramesContext *>(avFrame->hw_frames_ctx->data);

        if (framesCtx == nullptr) {
            return nullptr;
        }

        const int dxgiFormat = dxgiFormatForSwFormat(static_cast<int>(framesCtx->sw_format));
        const int textureWidth = static_cast<int>(framesCtx->width);
        const int textureHeight = static_cast<int>(framesCtx->height);
        /* 输入色彩空间每帧跟着帧的元数据走（同一个文件里也可能 SDR/HDR 混排）。 */
        const int inputColorSpace = inputColorSpaceForFrame(avFrame);

        /*
         * 显示尺寸用帧自己的 getInfo().video：它是裁剪后的真实画面尺寸
         * （例如 3840x2160），而纹理是编码对齐的（3840x2176）。
         */
        const int displayWidth = frame->getInfo().video.width;
        const int displayHeight = frame->getInfo().video.height;

        if (displayWidth <= 0 || displayHeight <= 0) {
            return nullptr;
        }

        if (!ensureProcessor(textureWidth, textureHeight, displayWidth, displayHeight, dxgiFormat,
                             inputColorSpace)) {
            m_failed = true;
            return nullptr;
        }

        if (!ensureOutputTexture(displayWidth, displayHeight)) {
            m_failed = true;
            return nullptr;
        }

        /* ensureProcessor 已经把枚举器/处理器按显示尺寸重建好了。 */
        if (m_processor == nullptr || m_enumerator == nullptr) {
            return nullptr;
        }

        if (!ensureInputView(texture, arraySlice, textureWidth, textureHeight, dxgiFormat)) {
            m_failed = true;
            return nullptr;
        }

        /* 输出视图按输出纹理建一次。 */
        if (m_outputView == nullptr) {
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc = {};
            desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            desc.Texture2D.MipSlice = 0;

            HRESULT hr = m_videoDevice->CreateVideoProcessorOutputView(m_outputTexture, m_enumerator,
                         &desc, &m_outputView);

            if (FAILED(hr)) {
                AF_LOGE("CreateVideoProcessorOutputView failed (hr=0x%08lx)\n", (long) hr);
                m_failed = true;
                return nullptr;
            }
        }

        /*
         * 视频处理器 Blt：NV12/P010 -> RGBA，源矩形裁掉编码对齐的填充行，
         * 目标矩形和输出纹理一样大（1:1）。缩放交给 Qt —— 场景图采样纹理时
         * 按 item 的矩形做 GPU 缩放，比我们自己算一遍再重做视频处理更省。
         *
         * 先（在需要时）把"色彩调整"应用到处理器：拖滑块之后的第一帧生效，
         * 之后走脏标记，每帧零代价（见 applyColorAdjust 的说明）。
         */
        applyColorAdjust();

        D3D11_VIDEO_COLOR black = {};
        black.RGBA.A = 1.0f;
        D3D11_RECT full = {0, 0, displayWidth, displayHeight};
        D3D11_RECT source = {0, 0, displayWidth, displayHeight};

        m_videoContext->VideoProcessorSetStreamFrameFormat(m_processor, 0,
                D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        m_videoContext->VideoProcessorSetStreamOutputRate(m_processor, 0,
                D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, TRUE, nullptr);
        m_videoContext->VideoProcessorSetOutputTargetRect(m_processor, TRUE, &full);
        m_videoContext->VideoProcessorSetOutputBackgroundColor(m_processor, FALSE, &black);
        m_videoContext->VideoProcessorSetStreamSourceRect(m_processor, 0, TRUE, &source);
        m_videoContext->VideoProcessorSetStreamDestRect(m_processor, 0, TRUE, &full);

        D3D11_VIDEO_PROCESSOR_STREAM stream = {};
        stream.Enable = TRUE;
        stream.OutputIndex = 0;
        stream.pInputSurface = m_inputView;

        HRESULT hr = m_videoContext->VideoProcessorBlt(m_processor, m_outputView, 0, 1, &stream);

        if (FAILED(hr)) {
            AF_LOGE("VideoProcessorBlt failed (hr=0x%08lx)\n", (long) hr);
            m_failed = true;
            return nullptr;
        }

        if (!m_loggedFirstFrame) {
            m_loggedFirstFrame = true;
            AF_LOGI("first zero-copy frame: %dx%d %s texture -> RGBA on the GPU -> Qt\n",
                    displayWidth, displayHeight,
                    (dxgiFormat == DXGI_FORMAT_P010) ? "P010" : "NV12");
        }

        /*
         * 把 RGBA 纹理包成 QSGTexture 交给 Qt 场景图。
         *
         * 【2026-09-30 回退：不要跨帧复用这张包装 —— 实测两次都闪退】
         * 曾经为了省掉"每次重绘新建一个 QSGTexture + SRV"而按输出纹理世代缓存过这张包装，
         * 结果**打开播放窗口后不久就闪退**（日志停在渲染线程里，和更早那次"帧指针命中缓存"
         * 的崩法一模一样）。两次的共同点是：**同一张 QSGD3D11Texture 包装被连续多帧复用**。
         * Qt 自己的视频管线也是每帧重新 fromNative 一次，所以这里回到"每次调用都新建"，
         * 那块 churn 只能从"重绘频率"这一侧去解决，不能从这里省。
         *
         * 注意：QSGTexture 的创建必须在渲染线程上（我们现在就在 updatePaintNode 里），
         * 而且返回的对象归调用方所有（纹理本身仍归我们，见 releaseResources）。
         */
        QSGTexture *qsTexture = wrapD3D11Texture(window, m_outputTexture,
                                QSize(displayWidth, displayHeight));

        if (qsTexture == nullptr) {
            m_failed = true;
        }

        return qsTexture;
    }

    void CicadaTextureD3D11::forgetOutputTextureWrapper()
    {
        /*
         * 回退之后不再缓存包装，这个函数只剩"清记录"的作用（保留调用点：
         * 一旦以后再尝试缓存，节点重建那四处必须同步忘掉，否则就是 use-after-free）。
         */
        m_outputQsTexture = nullptr;
        m_outputQsTextureGeneration = -1;
    }

}// namespace cicadaqt

#endif// Q_OS_WIN
