//
// Windows 零拷贝呈现的实现，设计说明见 D3d11DirectPresenter.h。
//

#include "D3d11DirectPresenter.h"

#if defined(_WIN32) && defined(ENABLE_SDL)

/*
 * d3d11.h 先以 C++ 链接进来，随后 FFmpeg 的头放在 extern "C" 里：
 * libavutil/hwcontext_d3d11va.h 会包含 d3d11.h，若整块都在 extern "C" 中，
 * d3d11.h 为 D3D11_VIEWPORT / D3D11_RECT / D3D11_BOX 定义的 C++ 比较运算符会
 * 拿到 C 链接，MSVC 报 C2733。有 include guard，第二次包含是空操作。
 */
#include <d3d11_1.h>
#include <dxgi1_6.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixfmt.h>
}

#include <utils/frame_work_log.h>
#include "base/media/AVAFPacket.h"

#include <new>
#include <cstddef>
#include <cmath>
#include <vector>

namespace {
    /*
     * 控件条位图的 BGRA 像素格式，和后台缓冲一致，CopySubresourceRegion 才能直接用。
     * HDR 直通时后台缓冲是 R10G10B10A2，控件条位图会在上传前转成同样的格式。
     */
    const DXGI_FORMAT BACK_BUFFER_FORMAT_SDR = DXGI_FORMAT_B8G8R8A8_UNORM;
    const DXGI_FORMAT BACK_BUFFER_FORMAT_HDR = DXGI_FORMAT_R10G10B10A2_UNORM;

    /*
     * 问一块输出（显示器）当前的刷新率，拿不到返回 0。
     *
     * DXGI_OUTPUT_DESC1 里**没有**刷新率字段（它只有 HDR 的亮度/原色信息），
     * 所以只能绕道：
     *   1. GDI 的 ENUM_CURRENT_SETTINGS 问的就是"这块显示器现在跑的是哪个模式"，
     *      从 desc.DeviceName 拿设备名，所以多显示器也不会问错。整数 Hz 足够用
     *      （这个值只决定渲染回调的格子有多细，不决定实际出帧节奏）。
     *   2. GDI 不灵时退到 DXGI 的显示模式列表：同一分辨率会有好几条
     *      （60/100/120/144Hz），分不出哪条是当前用的，就取**最高**的那条——
     *      格子细一点只是多几次空转，挑低了却会把回调卡在 60Hz（24fps 片源在
     *      高刷屏上就会出现 3:2 抖动的那个问题）。
     */
    int refreshHzFromOutput(IDXGIOutput *output)
    {
        DXGI_OUTPUT_DESC desc = {};

        if (output == nullptr || FAILED(output->GetDesc(&desc))) {
            return 0;
        }

        DEVMODEW gdiMode = {};
        gdiMode.dmSize = sizeof(gdiMode);

        if (desc.DeviceName[0] != L'\0' &&
                EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &gdiMode) &&
                gdiMode.dmDisplayFrequency > 1) {
            return static_cast<int>(gdiMode.dmDisplayFrequency);
        }

        const int width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
        const int height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
        UINT count = 0;
        int best = 0;

        if (SUCCEEDED(output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &count, nullptr)) &&
                count > 0) {
            std::vector<DXGI_MODE_DESC> modes(count);

            if (SUCCEEDED(output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &count,
                           modes.data()))) {
                for (UINT i = 0; i < count; ++i) {
                    const DXGI_MODE_DESC &mode = modes[i];

                    if (static_cast<int>(mode.Width) != width ||
                            static_cast<int>(mode.Height) != height ||
                            mode.RefreshRate.Denominator == 0) {
                        continue;
                    }

                    const int hz = static_cast<int>(
                                       static_cast<double>(mode.RefreshRate.Numerator) /
                                       mode.RefreshRate.Denominator + 0.5);

                    if (hz > best) {
                        best = hz;
                    }
                }
            }
        }

        return best;
    }

    /*
     * 控件条位图的分配高度：实际只用 bar->barHeight()（56），留点余量。
     */
    const int BAR_BITMAP_MAX_HEIGHT = 64;

    /* SDR 白在 HDR10 里通常按 203nit 映射（Windows 桌面就是这么定的）。 */
    const double SDR_WHITE_NITS = 203.0;

    /*
     * 每隔多少帧重新问一次显示器 HDR 状态：用户在 Windows 显示设置里开关 HDR
     * 时交换链要重建（后台缓冲格式是创建时固定的）。60fps 下约两秒一次，
     * 一次 GetDesc1 的代价可以忽略。
     */
    const int HDR_RECHECK_FRAMES = 120;

    /* 连续失败多少帧之后停止刷屏，只留一条"去掉 -direct"的提示。 */
    const int kFailureReportLimit = 60;

    /*
     * 控件条的“签名”：只有这些值变了才需要重画位图并重新上传。
     *
     * 必须覆盖**所有**影响外观的东西（SdlControlBar::drawBar 读的就是这些）：
     * 可见性、暂停/全屏、进度、音量，以及正在拖哪个控件、拖到哪儿——拖动时滑块
     * 跟的是 dragFraction 而不是播放位置，漏掉它拖动就没有反馈。
     * 播放位置量化到 1/2048：滑块只有几百像素宽，这个精度足够，也不会每帧重画。
     */
    uint64_t overlaySignature(SdlControlBar &bar, const SdlControlBar::Info &info, int width)
    {
        uint64_t value = bar.visible() ? 1u : 0u;
        value = value * 131 + static_cast<uint64_t>(info.paused ? 1 : 0);
        value = value * 131 + static_cast<uint64_t>(info.fullScreen ? 1 : 0);
        value = value * 131 + static_cast<uint64_t>(info.durationMs > 0
                ? (info.positionMs * 2048 / info.durationMs) : 0);
        value = value * 131 + static_cast<uint64_t>(info.volume * 2048.0f);
        value = value * 131 + static_cast<uint64_t>(static_cast<int>(bar.dragControl()));
        value = value * 131 + static_cast<uint64_t>(bar.dragFraction() * 4096.0f);
        value = value * 131 + static_cast<uint64_t>(width & 0xffff);
        return value;
    }

    /* 等比缩放并居中，和 SDL 那条路的 aspectFit() 是同一套算法。 */
    SDL_Rect aspectFit(int videoWidth, int videoHeight, int windowWidth, int windowHeight)
    {
        SDL_Rect result{0, 0, 0, 0};

        if (videoWidth <= 0 || videoHeight <= 0 || windowWidth <= 0 || windowHeight <= 0) {
            return result;
        }

        const float videoRatio = static_cast<float>(videoWidth) / static_cast<float>(videoHeight);
        const float windowRatio = static_cast<float>(windowWidth) / static_cast<float>(windowHeight);

        if (videoRatio <= windowRatio) {
            result.h = windowHeight;
            result.w = static_cast<int>(windowHeight * videoRatio);
        } else {
            result.w = windowWidth;
            result.h = static_cast<int>(windowWidth / videoRatio);
        }

        result.x = (windowWidth - result.w) / 2;
        result.y = (windowHeight - result.h) / 2;
        return result;
    }

    /*
     * 这一帧是不是 HDR 内容。
     *
     * 元数据来自解码器（FFmpeg 把容器的 color_trc / color_primaries 带在帧上）。
     * 判定标准：
     *   * 传输曲线是 PQ(HDR10) 或 HLG —— 硬指标，最可靠；
     *   * 或者纹理是 10bit(P010) 且原色是 BT.2020 —— 这种片源元数据常常不全
     *     （只写了原色没写曲线），而 10bit BT.2020 实际上就是 HDR。
     * 只看 BT.2020 是不够的：BT.2020 的 SDR(G22) 片源也存在，把它当 HDR 直通到
     * PQ 输出会偏暗。
     */
    bool isHdrFrame(const AVFrame *frame, int swFormat)
    {
        if (frame == nullptr) {
            return false;
        }

        if (frame->color_trc == AVCOL_TRC_SMPTE2084 || frame->color_trc == AVCOL_TRC_ARIB_STD_B67) {
            return true;
        }

        return frame->color_primaries == AVCOL_PRI_BT2020 && swFormat == AV_PIX_FMT_P010;
    }

    /*
     * 输入色彩空间：视频处理器要知道解码数据是 BT.709 还是 PQ/BT.2020，才能把
     * HDR 正确映射到 SDR 输出（这就是色调映射，不需要我们自己写 shader）。
     */
    DXGI_COLOR_SPACE_TYPE inputColorSpaceFor(const AVFrame *frame)
    {
        if (frame != nullptr && frame->color_trc == AVCOL_TRC_SMPTE2084) {
            return DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020;
        }

        if (frame != nullptr && frame->color_trc == AVCOL_TRC_ARIB_STD_B67) {
            return DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020;
        }

        if (frame != nullptr && frame->color_primaries == AVCOL_PRI_BT2020) {
            return DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_TOPLEFT_P2020;
        }

        return DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    }

    /*
     * 8bit sRGB 值 -> HDR10(PQ) 的 10bit 编码。控件条在 HDR 直通模式下要自己
     * 转一遍，否则那些按 SDR 定的灰色贴到 PQ 后台缓冲上会暗得看不见。
     */
    uint32_t sdrToPq10(double srgb)
    {
        if (srgb <= 0.0) {
            return 0;
        }

        /* sRGB -> 线性 */
        const double linear = (srgb <= 0.04045) ? (srgb / 12.92)
                                                : pow((srgb + 0.055) / 1.055, 2.4);
        /* 线性 -> 绝对亮度（nits），再归一化到 10000nit */
        const double nits = linear * SDR_WHITE_NITS;
        const double y = nits / 10000.0;
        const double m1 = 2610.0 / 16384.0;
        const double m2 = 2523.0 / 4096.0 * 128.0;
        const double c1 = 3424.0 / 4096.0;
        const double c2 = 2413.0 / 4096.0 * 32.0;
        const double c3 = 2392.0 / 4096.0 * 32.0;
        const double p = y <= 0.0 ? 0.0
                                  : pow((y * (c2 + c3 * pow(y, m1)) ) / (1.0 + c1 * pow(y, m1)), m2);
        const double code = p * 1023.0 + 0.5;

        return static_cast<uint32_t>(code < 0.0 ? 0.0 : (code > 1023.0 ? 1023.0 : code));
    }

    /*
     * 8bit -> PQ 10bit 的查表（sdrToPq10 只吃 8bit 输入，256 个值算一次就够）。
     * 函数内静态量的初始化在 C++11 起是线程安全的，两条线程都会用它。
     */
    struct PqLut {
        uint32_t value[256];
        PqLut()
        {
            for (int i = 0; i < 256; ++i) {
                value[i] = sdrToPq10(static_cast<double>(i) / 255.0);
            }
        }
    };

    const PqLut &pqLut()
    {
        static const PqLut lut;
        return lut;
    }

    /* BGRA 位图 -> R10G10B10A2 位图，供 HDR 后台缓冲直接拷贝。 */
    void convertBarToRgb10(const uint32_t *bgra, uint32_t *rgb10, int pixels, bool pq)
    {
        const PqLut &lut = pqLut();

        for (int i = 0; i < pixels; ++i) {
            const uint32_t value = bgra[i];
            const uint8_t b = static_cast<uint8_t>((value >> 0) & 0xff);
            const uint8_t g = static_cast<uint8_t>((value >> 8) & 0xff);
            const uint8_t r = static_cast<uint8_t>((value >> 16) & 0xff);
            uint32_t red;
            uint32_t green;
            uint32_t blue;

            if (pq) {
                /*
                 * 后台缓冲是 PQ 编码（HDR 直通）：每条通道查一次表。
                 * 4K 宽的控件条有十几万像素，按公式现算（每像素十几个 pow）
                 * 要几十毫秒，查表只要零点几毫秒。
                 */
                red = lut.value[r];
                green = lut.value[g];
                blue = lut.value[b];
            } else {
                /*
                 * 后台缓冲是 10bit 但按 SDR(G22/sRGB) 解释（驱动不支持 HDR
                 * 直通时）：8bit 精确扩到 10bit，(v << 2) | (v >> 6) 就是
                 * 标准做法，不需要任何转换。
                 */
                red = static_cast<uint32_t>((r << 2) | (r >> 6));
                green = static_cast<uint32_t>((g << 2) | (g >> 6));
                blue = static_cast<uint32_t>((b << 2) | (b >> 6));
            }

            /*
             * DXGI_FORMAT_R10G10B10A2_UNORM：低 10 位是 R，往上依次 G、B，
             * 最高 2 位是 A（alpha 对后台缓冲没意义，给满值 3）。
             */
            rgb10[i] = red | (green << 10) | (blue << 20) | (3u << 30);
        }
    }
}

D3d11DirectPresenter::~D3d11DirectPresenter()
{
    /*
     * 窗口不归这个对象管，而且走到析构时它多半已经被销毁了（子窗口活在那个
     * 接收器里）：先把句柄放掉，免得 close() 里对着一块已经不是窗口的内存
     * 调用 ShowWindow。
     */
    mHwnd = nullptr;
    close();
}

bool D3d11DirectPresenter::fail(const char *what, long hr)
{
    mLastError = what;

    /*
     * 失败会一帧一条地刷屏（呈现是每帧一次的），所以到一定次数就闭嘴，改成
     * 一条能照着做的提示：去掉 -direct 回到 copy-back + SDL 那条路。
     */
    if (mFailureCount++ < kFailureReportLimit) {
        AF_LOGE("-direct: %s (hr=0x%08lx)\n", what, hr);
    }

    if (mFailureCount == kFailureReportLimit) {
        AF_LOGE("-direct: zero-copy presentation keeps failing (%d frames), the picture "
                "is frozen on the last good frame; restart without -direct to go back to "
                "the copy-back + SDL path\n", kFailureReportLimit);
    }

    return false;
}

bool D3d11DirectPresenter::supported()
{
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    ID3D11VideoDevice *videoDevice = nullptr;
    ID3D11VideoProcessorEnumerator *enumerator = nullptr;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    bool ok = false;

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &device, &level, &context);

    if (FAILED(hr)) {
        AF_LOGW("-direct: D3D11CreateDevice failed (hr=0x%08lx), staying on the SDL path\n", hr);
        return false;
    }

    hr = device->QueryInterface(IID_PPV_ARGS(&videoDevice));

    if (FAILED(hr)) {
        AF_LOGW("-direct: this D3D11 device has no video processing (hr=0x%08lx)\n", hr);
    } else {
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputWidth = 1920;
        content.InputHeight = 1080;
        content.OutputWidth = 1920;
        content.OutputHeight = 1080;
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        hr = videoDevice->CreateVideoProcessorEnumerator(&content, &enumerator);

        if (FAILED(hr)) {
            AF_LOGW("-direct: CreateVideoProcessorEnumerator failed (hr=0x%08lx)\n", hr);
        } else {
            UINT flags = 0;
            bool inputOk = SUCCEEDED(enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &flags)) &&
                           (flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT);
            flags = 0;
            bool outputOk = SUCCEEDED(enumerator->CheckVideoProcessorFormat(BACK_BUFFER_FORMAT_SDR, &flags)) &&
                            (flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT);
            flags = 0;
            /* P010 只有 10bit HDR 片源会用到，缺了不影响 SDR 播放，单独报一声。 */
            bool p010Ok = SUCCEEDED(enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_P010, &flags)) &&
                          (flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT);

            ok = inputOk && outputOk;

            if (!ok) {
                AF_LOGW("-direct: NV12 input %s, BGRA output %s, staying on the SDL path\n",
                        inputOk ? "ok" : "unsupported", outputOk ? "ok" : "unsupported");
            } else if (!p010Ok) {
                AF_LOGW("-direct: this GPU cannot feed P010 textures, 10bit HDR sources "
                        "will not be playable in zero copy mode\n");
            }
        }
    }

    if (enumerator != nullptr) {
        enumerator->Release();
    }

    if (videoDevice != nullptr) {
        videoDevice->Release();
    }

    if (context != nullptr) {
        context->Release();
    }

    if (device != nullptr) {
        device->Release();
    }

    return ok;
}

int D3d11DirectPresenter::primaryDisplayRefreshHz()
{
    int hz = 0;
    IDXGIFactory1 *factory = nullptr;

    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        IDXGIAdapter1 *adapter = nullptr;

        if (factory->EnumAdapters1(0, &adapter) != DXGI_ERROR_NOT_FOUND) {
            IDXGIOutput *output = nullptr;

            if (adapter->EnumOutputs(0, &output) != DXGI_ERROR_NOT_FOUND) {
                hz = refreshHzFromOutput(output);
                output->Release();
            }

            adapter->Release();
        }

        factory->Release();
    }

    /* 连输出都枚举不到（老系统/远程会话）就退到主显示器的 GDI 模式查询。 */
    if (hz <= 0) {
        DEVMODEW mode = {};
        mode.dmSize = sizeof(mode);

        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1) {
            hz = static_cast<int>(mode.dmDisplayFrequency);
        }
    }

    return hz;
}

void D3d11DirectPresenter::setWindow(void *hwnd)
{
    if (mHwnd == hwnd) {
        return;
    }

    mHwnd = hwnd;
    /* 换了窗口就得重建交换链（子窗口会随父窗口重建）。 */
    releaseSwapChain();
    /* 新窗口是隐藏创建的，得重新走一次“第一帧上屏之后再显示”。 */
    mShownWindow = false;
}

void D3d11DirectPresenter::releaseSwapChainTargets()
{
    for (int i = 0; i < 3; ++i) {
        if (mOutputViews[i] != nullptr) {
            static_cast<ID3D11VideoProcessorOutputView *>(mOutputViews[i])->Release();
            mOutputViews[i] = nullptr;
        }

        if (mBackBuffers[i] != nullptr) {
            static_cast<ID3D11Texture2D *>(mBackBuffers[i])->Release();
            mBackBuffers[i] = nullptr;
        }
    }

    mOutputView = nullptr;
    mBackBuffer = nullptr;
    mWidth = 0;
    mHeight = 0;
}

void D3d11DirectPresenter::releaseSwapChain()
{
    releaseSwapChainTargets();

    /*
     * 等待句柄属于调用方（GetFrameLatencyWaitableObject 的约定），交换链释放之
     * 前先关掉：句柄是内核对象，不关就是泄漏。
     */
    if (mWaitable != nullptr) {
        CloseHandle(static_cast<HANDLE>(mWaitable));
        mWaitable = nullptr;
    }

    if (mSwapChain2 != nullptr) {
        static_cast<IDXGISwapChain2 *>(mSwapChain2)->Release();
        mSwapChain2 = nullptr;
    }

    if (mSwapChain3 != nullptr) {
        static_cast<IDXGISwapChain3 *>(mSwapChain3)->Release();
        mSwapChain3 = nullptr;
    }

    mBackBufferIndex = 0;

    if (mSwapChain != nullptr) {
        static_cast<IDXGISwapChain1 *>(mSwapChain)->Release();
        mSwapChain = nullptr;
    }
}

void D3d11DirectPresenter::releaseAll()
{
    releaseSwapChain();

    if (mInputView != nullptr) {
        static_cast<ID3D11VideoProcessorInputView *>(mInputView)->Release();
        mInputView = nullptr;
    }

    if (mStagingTexture != nullptr) {
        static_cast<ID3D11Texture2D *>(mStagingTexture)->Release();
        mStagingTexture = nullptr;
    }

    mStagingInput = false;
    mInputTexture = nullptr;
    mInputIndex = -1;

    if (mProcessor != nullptr) {
        static_cast<ID3D11VideoProcessor *>(mProcessor)->Release();
        mProcessor = nullptr;
    }

    if (mEnumerator != nullptr) {
        static_cast<ID3D11VideoProcessorEnumerator *>(mEnumerator)->Release();
        mEnumerator = nullptr;
    }

    if (mBarTexture != nullptr) {
        static_cast<ID3D11Texture2D *>(mBarTexture)->Release();
        mBarTexture = nullptr;
        mBarTextureWidth = 0;
    }

    if (mVideoContext1 != nullptr) {
        static_cast<ID3D11VideoContext1 *>(mVideoContext1)->Release();
        mVideoContext1 = nullptr;
    }

    if (mVideoContext != nullptr) {
        static_cast<ID3D11VideoContext *>(mVideoContext)->Release();
        mVideoContext = nullptr;
    }

    if (mVideoDevice != nullptr) {
        static_cast<ID3D11VideoDevice *>(mVideoDevice)->Release();
        mVideoDevice = nullptr;
    }

    if (mContext != nullptr) {
        static_cast<ID3D11DeviceContext *>(mContext)->Release();
        mContext = nullptr;
    }

    if (mDevice != nullptr) {
        static_cast<ID3D11Device *>(mDevice)->Release();
        mDevice = nullptr;
    }

    delete[] mBarPixels;
    mBarPixels = nullptr;
    delete[] mBarPixels10;
    mBarPixels10 = nullptr;
    mBarSignature = 0;
    mLock = nullptr;
    mUnlock = nullptr;
    mLockCtx = nullptr;
    mInputWidth = 0;
    mInputHeight = 0;
    mVideoWidth = 0;
    mVideoHeight = 0;
    /* 色彩空间是跟着交换链一起定下来的，链没了就得重新判定。 */
    mHdrOutput = false;
    mHdrPassThroughBlocked = false;
    mDisplayHdr = false;
    mDisplayRefreshHz = 0;

    /*
     * 视频子窗口是盖在 SDL 窗口上的：直通停了（或者解码器退回软解了）就得把它
     * 藏回去，否则 SDL 那条路画出来的画面永远被挡着。窗口句柄归事件线程，这里
     * 只读，而且只在真的显示过之后才动它。
     */
    if (mShownWindow && mReady && mHwnd != nullptr) {
        ShowWindow(static_cast<HWND>(mHwnd), SW_HIDE);
    }

    mShownWindow = false;
    mReady = false;
    mLoggedFirstFrame = false;
}

void D3d11DirectPresenter::close()
{
    releaseAll();
}

bool D3d11DirectPresenter::initFromFrame(AVFrame *frame)
{
    if (frame == nullptr || frame->hw_frames_ctx == nullptr) {
        return fail("the decoded frame carries no hw_frames_ctx", 0);
    }

    /* 函数作用域：下面取设备、取 DXGI 工厂、建交换链共用它。 */
    HRESULT hr = S_OK;

    /*
     * 设备只取一次，而且必须在碰任何引用计数之前先看窗口有没有准备好：
     * 这个方法会被每一帧重试，重复 AddRef/QueryInterface 会漏。
     */
    if (mDevice == nullptr) {
        auto *framesCtx0 = reinterpret_cast<AVHWFramesContext *>(frame->hw_frames_ctx->data);

        if (framesCtx0 == nullptr || framesCtx0->device_ref == nullptr) {
            return fail("the frame pool has no device", 0);
        }

        /* AVBufferRef::data 是 uint8_t*，转结构体指针只能用 reinterpret_cast。 */
        auto *deviceCtx = reinterpret_cast<AVHWDeviceContext *>(framesCtx0->device_ref->data);
        auto *d3d11 = static_cast<AVD3D11VADeviceContext *>(deviceCtx->hwctx);

        if (d3d11 == nullptr || d3d11->device == nullptr || d3d11->device_context == nullptr) {
            return fail("the D3D11 device from FFmpeg is incomplete", 0);
        }

        /*
         * 同一个设备、同一把锁：锁是 FFmpeg 给设备上下文配的（没有的话
         * av_hwdevice_ctx_create 会自己建一把递归互斥锁），解码线程也在用它，
         * 所以下面所有设备访问都必须包在这对回调之间。
         */
        mDevice = d3d11->device;
        mContext = d3d11->device_context;
        mLock = d3d11->lock;
        mUnlock = d3d11->unlock;
        mLockCtx = d3d11->lock_ctx;

        static_cast<ID3D11Device *>(mDevice)->AddRef();
        static_cast<ID3D11DeviceContext *>(mContext)->AddRef();

        hr = static_cast<ID3D11Device *>(mDevice)->QueryInterface(IID_PPV_ARGS(
                 reinterpret_cast<ID3D11VideoDevice **>(&mVideoDevice)));

        if (FAILED(hr)) {
            return fail("ID3D11VideoDevice is not available on the decoder device", hr);
        }

        hr = static_cast<ID3D11DeviceContext *>(mContext)->QueryInterface(IID_PPV_ARGS(
                 reinterpret_cast<ID3D11VideoContext **>(&mVideoContext)));

        if (FAILED(hr)) {
            return fail("ID3D11VideoContext is not available on the decoder context", hr);
        }
    }

    if (mSwapChain != nullptr) {
        return true;
    }

    if (mHwnd == nullptr) {
        /* 子窗口还没建好，下一帧再来。 */
        mLastError = "waiting for the presentation window";
        return false;
    }

    IDXGIDevice *dxgiDevice = nullptr;
    IDXGIAdapter *adapter = nullptr;
    IDXGIFactory2 *factory = nullptr;

    hr = static_cast<ID3D11Device *>(mDevice)->QueryInterface(IID_PPV_ARGS(&dxgiDevice));

    if (SUCCEEDED(hr)) {
        hr = dxgiDevice->GetAdapter(&adapter);
    }

    if (SUCCEEDED(hr)) {
        hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    }

    if (FAILED(hr)) {
        if (factory != nullptr) factory->Release();
        if (adapter != nullptr) adapter->Release();
        if (dxgiDevice != nullptr) dxgiDevice->Release();
        return fail("cannot reach the DXGI factory for the decoder device", hr);
    }

    RECT client = {};
    GetClientRect(static_cast<HWND>(mHwnd), &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;

    if (width <= 0 || height <= 0) {
        width = 1280;
        height = 720;
    }

    /*
     * HDR 决策：视频是 HDR，且显示器当前开着 HDR，就走直通（10bit PQ/P2020 交换链，
     * 画面原样输出）；否则走 SDR 交换链，交给视频处理器做色调映射。
     * 这个选择必须在这里定，因为后台缓冲格式是创建交换链时就固定的。
     */
    mBackFormat = mHdrOutput ? BACK_BUFFER_FORMAT_HDR : BACK_BUFFER_FORMAT_SDR;
    mOutputColorSpace = mHdrOutput ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                   : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.Format = static_cast<DXGI_FORMAT>(mBackFormat);
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Scaling = DXGI_SCALING_STRETCH;
    /*
     * 可等待对象：每帧先等它一次，交换链就按显示器刷新率放行——产出快于显示时
     * 在这里被节流，而不是把帧堆在队列里越积越晚。等待放在解码锁外面。
     */
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    IDXGISwapChain1 *swapChain = nullptr;
    hr = factory->CreateSwapChainForHwnd(static_cast<ID3D11Device *>(mDevice),
                                         static_cast<HWND>(mHwnd), &desc, nullptr, nullptr, &swapChain);

    factory->Release();
    adapter->Release();
    dxgiDevice->Release();

    if (FAILED(hr)) {
        return fail("CreateSwapChainForHwnd failed", hr);
    }

    mSwapChain = swapChain;

    /* 队列深度 1：延迟最低，同时仍然由显示器刷新率节流。 */
    IDXGISwapChain2 *swapChain2 = nullptr;

    if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain2)))) {
        swapChain2->SetMaximumFrameLatency(1);
        mSwapChain2 = swapChain2;
        /*
         * 等待句柄由调用方负责关闭（不是交换链的引用计数管），
         * releaseSwapChain 里 CloseHandle。
         */
        mWaitable = swapChain2->GetFrameLatencyWaitableObject();
    }

    /*
     * 后台缓冲索引要问 IDXGISwapChain3（GetCurrentBackBufferIndex 在 DXGI 1.4
     * 那一层，IDXGISwapChain2 里没有这个方法）。拿不到就自己轮转。
     */
    IDXGISwapChain3 *swapChain3 = nullptr;

    if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3)))) {
        mSwapChain3 = swapChain3;
    } else {
        AF_LOGW("-direct: IDXGISwapChain3 is missing, tracking the back buffer index manually\n");
    }

    /* HDR 直通：交换链要显式声明成 PQ/P2020，否则系统会把它当 SDR 合成。 */
    if (mHdrOutput) {
        if (mSwapChain3 == nullptr) {
            /* 没有 IDXGISwapChain3（DXGI 1.4 之前）就声明不了色彩空间。 */
            AF_LOGW("-direct: no IDXGISwapChain3, cannot declare a PQ swap chain; "
                    "tone mapping HDR content to SDR instead\n");
            mHdrPassThroughBlocked = true;
            mHdrOutput = false;
            mOutputColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        } else {
            HRESULT colorHr = static_cast<IDXGISwapChain3 *>(mSwapChain3)->SetColorSpace1(
                                  static_cast<DXGI_COLOR_SPACE_TYPE>(mOutputColorSpace));

            if (FAILED(colorHr)) {
                /*
                 * 驱动给不了 PQ 输出：仍然用 10bit 后台缓冲，但按 SDR 解释，画面
                 * 走视频处理器的色调映射。记下这一条，省得每两秒重建一次交换链
                 * 又失败一次。
                 */
                AF_LOGW("-direct: SetColorSpace1(HDR) failed (hr=0x%08lx), "
                        "tone mapping HDR content to SDR instead\n", colorHr);
                mHdrPassThroughBlocked = true;
                mHdrOutput = false;
                mOutputColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
            }
        }
    }

    AF_LOGI("-direct: %s output, back buffer %s, display %d Hz%s\n",
            mHdrOutput ? "HDR (PQ/BT.2020 pass-through)" : "SDR (tone mapped)",
            mHdrOutput ? "R10G10B10A2" : "B8G8R8A8",
            mDisplayRefreshHz, mDisplayHdr ? ", HDR display" : "");
    mWidth = width;
    mHeight = height;
    mReady = true;
    AF_LOGI("-direct: D3D11 zero-copy presenter ready, swap chain %dx%d on the child window\n",
            width, height);
    return true;
}

bool D3d11DirectPresenter::queryDisplayHdr()
{
    HMONITOR monitor = MonitorFromWindow(static_cast<HWND>(mHwnd), MONITOR_DEFAULTTOPRIMARY);
    IDXGIFactory1 *factory = nullptr;
    bool hdr = false;

    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
    }

    for (UINT a = 0; !hdr; ++a) {
        IDXGIAdapter1 *adapter = nullptr;

        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }

        for (UINT o = 0; !hdr; ++o) {
            IDXGIOutput *output = nullptr;

            if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) {
                break;
            }

            DXGI_OUTPUT_DESC desc = {};

            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
                IDXGIOutput6 *output6 = nullptr;

                /*
                 * 顺手把刷新率也问出来（只问一次）：窗口在哪块显示器上就按哪块
                 * 的刷新率节流。DXGI_OUTPUT_DESC1 里没有刷新率，见
                 * refreshHzFromOutput()。
                 */
                if (mDisplayRefreshHz == 0) {
                    mDisplayRefreshHz = refreshHzFromOutput(output);
                }

                if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6)))) {
                    DXGI_OUTPUT_DESC1 desc1 = {};

                    if (SUCCEEDED(output6->GetDesc1(&desc1))) {
                        /*
                         * 显示器确实在 HDR 模式（Windows 显示设置里打开了）：
                         * 输出色彩空间是 PQ/P2020，且峰值亮度明显高于 SDR。
                         */
                        hdr = (desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) &&
                              (desc1.MaxLuminance > 250.0f);
                    }

                    output6->Release();
                }

                output->Release();
                break;
            }

            output->Release();
        }

        adapter->Release();
    }

    factory->Release();
    return hdr;
}

int D3d11DirectPresenter::queryDisplayRefreshHz()
{
    if (mDisplayRefreshHz > 0) {
        return mDisplayRefreshHz;
    }

    /*
     * 正常情况下 queryDisplayHdr() 已经在窗口所在的那块显示器上问到了（见
     * refreshHzFromOutput）。走到这里说明连显示器都没匹配上（窗口还没建好、
     * 远程会话之类），那就问一次主显示器，拿不到就保持 0：节流退回固定超时。
     */
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);

    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1) {
        mDisplayRefreshHz = static_cast<int>(mode.dmDisplayFrequency);
    }

    return mDisplayRefreshHz;
}

bool D3d11DirectPresenter::ensureProcessor(int inputWidth, int inputHeight, int inputFormat)
{
    if (mProcessor != nullptr && mInputWidth == inputWidth && mInputHeight == inputHeight &&
            mInputFormat == inputFormat && mOutputWidth == mWidth && mOutputHeight == mHeight) {
        return true;
    }

    if (mProcessor != nullptr) {
        static_cast<ID3D11VideoProcessor *>(mProcessor)->Release();
        mProcessor = nullptr;
    }

    if (mEnumerator != nullptr) {
        static_cast<ID3D11VideoProcessorEnumerator *>(mEnumerator)->Release();
        mEnumerator = nullptr;
    }

    if (mInputView != nullptr) {
        static_cast<ID3D11VideoProcessorInputView *>(mInputView)->Release();
        mInputView = nullptr;
        mInputTexture = nullptr;
        mInputIndex = -1;
    }

    /* 格式/尺寸变了，中转纹理也得跟着重建（它按输入格式建）。 */
    if (mStagingTexture != nullptr) {
        static_cast<ID3D11Texture2D *>(mStagingTexture)->Release();
        mStagingTexture = nullptr;
    }

    mStagingInput = false;

    auto *videoDevice = static_cast<ID3D11VideoDevice *>(mVideoDevice);

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    /*
     * 枚举用的是**纹理**尺寸：解码纹理是编码尺寸对齐后的（例如 3840x2176），
     * 真正的画面区域由每帧的源矩形（帧的显示宽高）裁出来。
     */
    content.InputWidth = static_cast<UINT>(inputWidth);
    content.InputHeight = static_cast<UINT>(inputHeight);
    content.OutputWidth = static_cast<UINT>(mWidth);
    content.OutputHeight = static_cast<UINT>(mHeight);
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    HRESULT hr = videoDevice->CreateVideoProcessorEnumerator(&content,
                   reinterpret_cast<ID3D11VideoProcessorEnumerator **>(&mEnumerator));

    if (FAILED(hr)) {
        return fail("CreateVideoProcessorEnumerator failed", hr);
    }

    auto *enumerator = static_cast<ID3D11VideoProcessorEnumerator *>(mEnumerator);
    UINT flags = 0;

    if (FAILED(enumerator->CheckVideoProcessorFormat(static_cast<DXGI_FORMAT>(inputFormat), &flags)) ||
        (flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0) {
        /* 把格式号打出来：103 = NV12，104 = P010，方便对上"-direct: supported()"的探测结果。 */
        AF_LOGE("-direct: the video processor does not accept input format %d\n", inputFormat);
        return fail("the video processor cannot take this decoded texture format", 0);
    }

    flags = 0;

    if (FAILED(enumerator->CheckVideoProcessorFormat(static_cast<DXGI_FORMAT>(mBackFormat), &flags)) ||
        (flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
        return fail("the video processor cannot write this back buffer format", 0);
    }

    hr = videoDevice->CreateVideoProcessor(enumerator, 0,
                                           reinterpret_cast<ID3D11VideoProcessor **>(&mProcessor));

    if (FAILED(hr)) {
        return fail("CreateVideoProcessor failed", hr);
    }

    /*
     * 色彩空间：告诉视频处理器输入是什么（BT.709 / BT.2020-PQ / BT.2020-HLG），
     * 输出是什么（SDR 709 或 HDR10 PQ/P2020）。输入是 HDR 而输出是 SDR 时，
     * 驱动会在这条路径上做色调映射——不需要我们自己写 shader，这也是 HDR 视频
     * 在 SDR 屏幕上不再发灰的原因。
     */
    ID3D11VideoContext1 *videoContext1 = nullptr;

    if (SUCCEEDED(static_cast<ID3D11VideoContext *>(mVideoContext)->QueryInterface(
                      IID_PPV_ARGS(&videoContext1)))) {
        if (mVideoContext1 != nullptr) {
            static_cast<ID3D11VideoContext1 *>(mVideoContext1)->Release();
        }

        mVideoContext1 = videoContext1;
    }

    if (mVideoContext1 != nullptr) {
        auto *context1 = static_cast<ID3D11VideoContext1 *>(mVideoContext1);
        auto *processor = static_cast<ID3D11VideoProcessor *>(mProcessor);

        /*
         * 每次重建处理器都要重设一遍：色彩空间是挂在处理器对象上的状态，
         * 新对象又回到驱动默认值（重建的一个常见原因就是窗口尺寸变了）。
         */
        context1->VideoProcessorSetStreamColorSpace1(
            processor, 0, static_cast<DXGI_COLOR_SPACE_TYPE>(mInputColorSpace));
        context1->VideoProcessorSetOutputColorSpace1(
            processor, static_cast<DXGI_COLOR_SPACE_TYPE>(mOutputColorSpace));

        AF_LOGI("-direct: color spaces set (input %d, output %d, %s)\n",
                mInputColorSpace, mOutputColorSpace,
                mHdrOutput ? "HDR pass-through" : "tone mapped to SDR");
    } else {
        AF_LOGW("-direct: ID3D11VideoContext1 is missing, colour spaces are left at the "
                "driver default (HDR content may look washed out)\n");
    }

    mInputWidth = inputWidth;
    mInputHeight = inputHeight;
    mInputFormat = inputFormat;
    /* 记下这个处理器是按哪个输出尺寸枚举的，窗口一变就得重建。 */
    mOutputWidth = mWidth;
    mOutputHeight = mHeight;
    AF_LOGI("-direct: video processor for %dx%d textures (input format %d) -> %dx%d output\n",
            inputWidth, inputHeight, inputFormat, mOutputWidth, mOutputHeight);
    return true;
}

/*
 * 输入视图：先拿解码纹理本身去建（Windows 的 D3D11 视频解码示例就是这么做的，
 * 解码表面只带 D3D11_BIND_DECODER）。
 *
 * 有些驱动不认这种资源当视频处理器输入，那就退到一张自建的中转纹理：
 * RENDER_TARGET | SHADER_RESOURCE，格式和尺寸跟解码纹理一样，每帧用
 * CopySubresourceRegion 拷一次——GPU 内部搬运，不经过 CPU，和 copy-back 那条
 * 路（12.4MB 下载 + 12.4MB 上传）完全不是一个量级。哪条路在用会打日志。
 */
bool D3d11DirectPresenter::ensureInputView(void *texture, int64_t index)
{
    if (mInputView != nullptr && mInputTexture == texture && mInputIndex == index) {
        return true;
    }

    if (mInputView != nullptr) {
        static_cast<ID3D11VideoProcessorInputView *>(mInputView)->Release();
        mInputView = nullptr;
    }

    auto *videoDevice = static_cast<ID3D11VideoDevice *>(mVideoDevice);
    auto *enumerator = static_cast<ID3D11VideoProcessorEnumerator *>(mEnumerator);

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc = {};
    desc.FourCC = 0;
    desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipSlice = 0;
    desc.Texture2D.ArraySlice = static_cast<UINT>(index);

    HRESULT hr = videoDevice->CreateVideoProcessorInputView(
                     static_cast<ID3D11Texture2D *>(texture), enumerator, &desc,
                     reinterpret_cast<ID3D11VideoProcessorInputView **>(&mInputView));

    if (SUCCEEDED(hr)) {
        mInputTexture = texture;
        mInputIndex = index;
        mStagingInput = false;
        return true;
    }

    /*
     * 直接用不行：改用中转纹理。注意这里**不报错**，只是换条路——硬解、零拷贝
     * （不下载）都还在，多的只是一次 GPU 内部拷贝。
     */
    AF_LOGW("-direct: CreateVideoProcessorInputView on the decoded texture failed "
            "(hr=0x%08lx), using a %dx%d staging texture instead "
            "(one extra GPU copy per frame, still no CPU copy)\n",
            hr, mInputWidth, mInputHeight);

    if (!ensureStagingTexture()) {
        return false;
    }

    /* 中转纹理是单张的（ArraySize = 1），输入视图永远指向它的第 0 片。 */
    desc.Texture2D.ArraySlice = 0;

    hr = videoDevice->CreateVideoProcessorInputView(
             static_cast<ID3D11Texture2D *>(mStagingTexture), enumerator, &desc,
             reinterpret_cast<ID3D11VideoProcessorInputView **>(&mInputView));

    if (FAILED(hr)) {
        return fail("CreateVideoProcessorInputView failed on the staging texture", hr);
    }

    mStagingInput = true;
    mInputTexture = texture;
    mInputIndex = index;
    /* 新视图的第一帧内容也马上补齐，别等到下一次 present。 */
    return copyInputToStaging(texture, index);
}

bool D3d11DirectPresenter::ensureStagingTexture()
{
    if (mStagingTexture != nullptr) {
        return true;
    }

    if (mInputWidth <= 0 || mInputHeight <= 0 || mInputFormat == 0) {
        return fail("the staging texture has no size or format yet", 0);
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(mInputWidth);
    desc.Height = static_cast<UINT>(mInputHeight);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    /* 必须和解码纹理同格式：CopySubresourceRegion 不做格式转换。 */
    desc.Format = static_cast<DXGI_FORMAT>(mInputFormat);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    /* 视频处理器的输入视图对这张纹理要求的就是这两个绑定。 */
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = static_cast<ID3D11Device *>(mDevice)->CreateTexture2D(&desc, nullptr,
                     reinterpret_cast<ID3D11Texture2D **>(&mStagingTexture));

    if (FAILED(hr)) {
        return fail("cannot create the input staging texture", hr);
    }

    return true;
}

/* 解码纹理的某一层 -> 中转纹理（GPU 内部拷贝，锁内调用）。 */
bool D3d11DirectPresenter::copyInputToStaging(void *texture, int64_t index)
{
    if (mStagingTexture == nullptr || texture == nullptr || mContext == nullptr) {
        return false;
    }

    /* 盒为空 = 整层；尺寸/格式两边一致，所以不需要裁剪。 */
    static_cast<ID3D11DeviceContext *>(mContext)->CopySubresourceRegion(
        static_cast<ID3D11Texture2D *>(mStagingTexture), 0, 0, 0, 0,
        static_cast<ID3D11Texture2D *>(texture), static_cast<UINT>(index), nullptr);
    return true;
}

/*
 * 输出尺寸自检 + 交换链后台缓冲重建。
 *
 * 拖边框、最大化、全屏切换都会让子窗口的客户区变化，通知来得慢，不如直接问一次
 * GetClientRect（代价可以忽略）。
 *
 * **必须在 ensureProcessor() 之前调用**：视频处理器和它的枚举器是带着输出尺寸
 * 枚举出来的，尺寸变了就得先重建缓冲、再让处理器按新尺寸重建。
 */
bool D3d11DirectPresenter::syncOutputSize()
{
    RECT client = {};
    GetClientRect(static_cast<HWND>(mHwnd), &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    if (width <= 0 || height <= 0) {
        return false;
    }

    if (mSwapChain == nullptr || (width == mWidth && height == mHeight)) {
        /* 交换链还没建（initFromFrame 建的时候就是按客户区来的），或者没变。 */
        return true;
    }

    releaseSwapChainTargets();

    auto *swapChain = static_cast<IDXGISwapChain1 *>(mSwapChain);
    /*
     * 先按创建时的标志位改尺寸（可等待对象是创建时定的，带上它最保险）；
     * 万一驱动只认 0，再退一步用 0 试一次——两种情况都不能让画面停住。
     */
    HRESULT hr = swapChain->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height),
                                          DXGI_FORMAT_UNKNOWN,
                                          DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);

    if (FAILED(hr)) {
        hr = swapChain->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height),
                                      DXGI_FORMAT_UNKNOWN, 0);
    }

    if (FAILED(hr)) {
        return fail("ResizeBuffers failed", hr);
    }

    mWidth = width;
    mHeight = height;
    mOutputWidth = 0;
    mOutputHeight = 0;

    /* 视频处理器是按输出尺寸枚举出来的，尺寸变了得重建。 */
    if (mProcessor != nullptr) {
        static_cast<ID3D11VideoProcessor *>(mProcessor)->Release();
        mProcessor = nullptr;
        mInputWidth = 0;
        mInputHeight = 0;
    }

    /* 控件条位图宽度跟着窗口走。 */
    if (mBarTexture != nullptr) {
        static_cast<ID3D11Texture2D *>(mBarTexture)->Release();
        mBarTexture = nullptr;
        mBarTextureWidth = 0;
    }

    return true;
}

bool D3d11DirectPresenter::ensureOutputView()
{
    /*
     * 枚举器是输出的必需品，没有它 CreateVideoProcessorOutputView 只会返回
     * E_INVALIDARG（已经踩过一次）。正常情况下 ensureProcessor() 在更前面跑过。
     */
    if (mSwapChain == nullptr || mEnumerator == nullptr) {
        return fail("the video processor enumerator is not ready yet", 0);
    }

    if (mOutputView != nullptr && mBackBuffer != nullptr) {
        return true;
    }

    /*
     * flip-model 交换链有两个后台缓冲，每次 Present 之后"当前索引"会切换，
     * 所以按索引各留一份后台缓冲和输出视图，不能用固定的 0 号。
     *
     * 索引从 IDXGISwapChain3::GetCurrentBackBufferIndex() 来（DXGI 1.4）；
     * 没有它就自己按 mBufferCount 轮转——Present 成功一次换一个，和 flip 的
     * 轮转规律一致。
     */
    auto *swapChain = static_cast<IDXGISwapChain1 *>(mSwapChain);
    UINT index = 0;

    if (mSwapChain3 != nullptr) {
        index = static_cast<IDXGISwapChain3 *>(mSwapChain3)->GetCurrentBackBufferIndex();
    } else {
        index = static_cast<UINT>(mBackBufferIndex);
    }

    if (index >= static_cast<UINT>(mBufferCount) || index >= 3) {
        index = 0;
    }

    if (mBackBuffers[index] == nullptr) {
        HRESULT hr = swapChain->GetBuffer(index, IID_PPV_ARGS(
                         reinterpret_cast<ID3D11Texture2D **>(&mBackBuffers[index])));

        if (FAILED(hr)) {
            return fail("swap chain GetBuffer failed", hr);
        }
    }

    if (mOutputViews[index] == nullptr) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc = {};
        desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipSlice = 0;

        HRESULT hr = static_cast<ID3D11VideoDevice *>(mVideoDevice)->CreateVideoProcessorOutputView(
                         static_cast<ID3D11Texture2D *>(mBackBuffers[index]),
                         static_cast<ID3D11VideoProcessorEnumerator *>(mEnumerator), &desc,
                         reinterpret_cast<ID3D11VideoProcessorOutputView **>(&mOutputViews[index]));

        if (FAILED(hr)) {
            return fail("CreateVideoProcessorOutputView failed", hr);
        }
    }

    mBackBuffer = mBackBuffers[index];
    mOutputView = mOutputViews[index];
    return true;
}

bool D3d11DirectPresenter::ensureBarTexture(int width)
{
    if (mBarTexture != nullptr && mBarTextureWidth == width) {
        return true;
    }

    if (mBarTexture != nullptr) {
        static_cast<ID3D11Texture2D *>(mBarTexture)->Release();
        mBarTexture = nullptr;
    }

    const int height = BAR_BITMAP_MAX_HEIGHT;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    /* 必须和后台缓冲同格式：CopySubresourceRegion 不做格式转换。 */
    desc.Format = static_cast<DXGI_FORMAT>(mBackFormat);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = static_cast<ID3D11Device *>(mDevice)->CreateTexture2D(&desc, nullptr,
                     reinterpret_cast<ID3D11Texture2D **>(&mBarTexture));

    if (FAILED(hr)) {
        return fail("cannot create the control bar texture", hr);
    }

    delete[] mBarPixels;
    mBarPixels = new (std::nothrow) uint32_t[static_cast<std::size_t>(width) *
                                             static_cast<std::size_t>(height)];

    if (mBarPixels == nullptr) {
        return fail("out of memory for the control bar bitmap", 0);
    }

    /* HDR 直通时要先把 BGRA 位图转成 PQ 的 10bit，放在这里分配一次。 */
    delete[] mBarPixels10;
    mBarPixels10 = nullptr;

    if (mBackFormat == BACK_BUFFER_FORMAT_HDR) {
        mBarPixels10 = new (std::nothrow) uint32_t[static_cast<std::size_t>(width) *
                                                   static_cast<std::size_t>(height)];

        if (mBarPixels10 == nullptr) {
            return fail("out of memory for the control bar bitmap", 0);
        }
    }

    mBarTextureWidth = width;
    mBarSignature = 0;
    return true;
}

bool D3d11DirectPresenter::uploadBar(SdlControlBar *bar, const SdlControlBar::Info &info)
{
    if (bar == nullptr) {
        return false;
    }

    const int barHeight = bar->barHeight();
    const int barWidth = bar->barWidth();

    if (barHeight <= 0 || mBarPixels == nullptr || barHeight > BAR_BITMAP_MAX_HEIGHT) {
        return false;
    }

    /*
     * 排版宽度必须是后台缓冲宽度：位图的行距就是按它更新上去的，差一列整张图
     * 就歪了。尺寸变化时事件线程会重新 layout()，中间这一两帧先不画控件条。
     */
    if (barWidth != mBarTextureWidth) {
        return false;
    }

    const uint64_t signature = overlaySignature(*bar, info, barWidth);

    if (signature == mBarSignature) {
        return true;
    }

    bar->drawToBitmap(mBarPixels, info);

    /*
     * 后台缓冲是 10bit PQ 时，控件条也得是同一个格式（CopySubresourceRegion
     * 不做格式转换），而且必须走 SDR->PQ 的转换，否则一行白字会暗得看不见：
     * PQ 的 10bit 编码里 SDR 参考白只占 0.58 左右，直接当线性值填进去会暗掉。
     */
    const void *pixels = mBarPixels;
    UINT rowPitch = static_cast<UINT>(mBarTextureWidth) * 4;

    if (mBarPixels10 != nullptr) {
        /* PQ 还是 SDR 由输出色彩空间决定：控件条的编码必须和画面一致。 */
        const bool pq = (mOutputColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        convertBarToRgb10(mBarPixels, mBarPixels10, mBarTextureWidth * barHeight, pq);
        pixels = mBarPixels10;
    }

    D3D11_BOX box = {};
    box.left = 0;
    box.top = 0;
    box.right = static_cast<UINT>(mBarTextureWidth);
    box.bottom = static_cast<UINT>(barHeight);
    box.front = 0;
    box.back = 1;

    static_cast<ID3D11DeviceContext *>(mContext)->UpdateSubresource(
        static_cast<ID3D11Texture2D *>(mBarTexture), 0, &box, pixels, rowPitch, 0);

    mBarSignature = signature;
    return true;
}

bool D3d11DirectPresenter::present(IAFFrame *iaFrame, SdlControlBar *bar,
                                   const SdlControlBar::Info &barInfo)
{
    /*
     * 串行化两条线程的呈现（VSync 线程出画 / 事件线程在暂停时只刷控件条）。
     * 锁序：这把锁 -> FFmpeg 设备锁；没有相反方向的获取，不会死锁。
     */
    std::lock_guard<std::mutex> presentLock(mPresentMutex);

    if (iaFrame == nullptr) {
        /*
         * 只刷控件条：没有新帧，视频部分沿用上一次的输入视图重画一遍
         * （暂停时纹理内容不会变，画出来的还是那一帧）。
         */
        return refreshOverlayOnly(bar, barInfo);
    }

    /*
     * 只认带 AVFrame 的硬件帧：纹理和 hw_frames_ctx 都在里面。
     */
    auto *avafFrame = dynamic_cast<AVAFFrame *>(iaFrame);

    if (avafFrame == nullptr) {
        return fail("the frame is not an AVAFrame, cannot reach the D3D11 texture", 0);
    }

    AVFrame *frame = avafFrame->ToAVFrame();

    if (frame == nullptr) {
        return fail("the frame carries no AVFrame", 0);
    }

    /*
     * 纹理布局（FFmpeg hwcontext_d3d11va.c: d3d11va_get_buffer）：
     *   data[0] = ID3D11Texture2D*，data[1] = 数组切片索引。
     */
    auto *texture = reinterpret_cast<void *>(frame->data[0]);

    if (texture == nullptr) {
        return fail("the decoded frame has no D3D11 texture", 0);
    }

    if (frame->hw_frames_ctx == nullptr) {
        return fail("the decoded frame carries no hw_frames_ctx", 0);
    }

    const int64_t index = static_cast<int64_t>(reinterpret_cast<intptr_t>(frame->data[1]));
    auto *framesCtx = reinterpret_cast<AVHWFramesContext *>(frame->hw_frames_ctx->data);

    /*
     * 解码纹理的实际格式：8bit 流是 NV12，10bit HDR 流是 P010。视频处理器必须
     * 按真实格式枚举，否则 CreateVideoProcessorInputView 会失败。
     */
    const int inputFormat = (framesCtx->sw_format == AV_PIX_FMT_P010) ? DXGI_FORMAT_P010
                                                                     : DXGI_FORMAT_NV12;

    /*
     * HDR / 色彩空间跟着帧的元数据走（同一个文件里也可能变）。内容和显示器状态
     * 一变就要重建交换链——后台缓冲格式是创建时固定的。
     */
    const bool hdrContent = isHdrFrame(frame, framesCtx->sw_format);
    const int inputColorSpace = static_cast<int>(inputColorSpaceFor(frame));
    bool rebuildSwapChain = false;

    if (!mReady) {
        /*
         * 第一帧：先问显示器（HDR 开没开、刷新率多少），再决定 SDR 还是 HDR 直通。
         * 顺序很重要：mBackFormat 是 initFromFrame 里按 mHdrOutput 定的。
         */
        mDisplayHdr = queryDisplayHdr();
        queryDisplayRefreshHz();
        mHdrContent = hdrContent;
        mInputColorSpace = inputColorSpace;
        mHdrOutput = mHdrContent && mDisplayHdr && !mHdrPassThroughBlocked;

        if (!initFromFrame(frame)) {
            return false;
        }
    } else {
        /*
         * 用户随时可能在 Windows 显示设置里开关 HDR，所以每隔两秒左右重新问一次；
         * 内容自己在 SDR/HDR 之间切换（拼接的片源）时立刻重建。
         */
        if (++mFrameCounter >= HDR_RECHECK_FRAMES) {
            mFrameCounter = 0;
            const bool displayHdr = queryDisplayHdr();

            if (displayHdr != mDisplayHdr) {
                AF_LOGI("-direct: the display HDR mode is now %s\n", displayHdr ? "on" : "off");
                mDisplayHdr = displayHdr;
                rebuildSwapChain = true;
            }
        }

        if (hdrContent != mHdrContent || inputColorSpace != mInputColorSpace) {
            rebuildSwapChain = true;
        }

        const bool wantHdrOutput = hdrContent && mDisplayHdr && !mHdrPassThroughBlocked;

        if (wantHdrOutput != mHdrOutput) {
            mHdrOutput = wantHdrOutput;
            rebuildSwapChain = true;
        }

        if (rebuildSwapChain) {
            mHdrContent = hdrContent;
            mInputColorSpace = inputColorSpace;

            /*
             * 后台缓冲格式要跟着换，交换链只能重建。设备和窗口留着：设备是
             * FFmpeg 的（解码线程在用），窗口是事件线程的。
             */
            AF_LOGI("-direct: rebuilding the swap chain for %s content on a %s display"
                    " (refresh %d Hz)\n",
                    mHdrContent ? "HDR" : "SDR", mHdrOutput ? "HDR" : "SDR",
                    mDisplayRefreshHz);

            releaseSwapChain();

            /* 处理器是按后台缓冲格式枚举的，一起重建。 */
            if (mProcessor != nullptr) {
                static_cast<ID3D11VideoProcessor *>(mProcessor)->Release();
                mProcessor = nullptr;
            }

            if (mEnumerator != nullptr) {
                static_cast<ID3D11VideoProcessorEnumerator *>(mEnumerator)->Release();
                mEnumerator = nullptr;
            }

            mInputWidth = 0;
            mInputHeight = 0;

            if (!initFromFrame(frame)) {
                return false;
            }
        }
    }

    /*
     * 按显示器刷新率节流：产出快于显示时在这里等，最多等两个刷新周期（拿不到
     * 刷新率就用 50ms 兜底）。**放在解码锁外面**——锁内不能有等待，否则会把
     * 解码线程一起拖住。
     */
    if (mWaitable != nullptr) {
        const int refresh = mDisplayRefreshHz > 0 ? mDisplayRefreshHz : 0;
        /* 超时给足两个刷新周期：等不到也得往前走，不能把这一帧卡死在这里。 */
        const DWORD timeout = (refresh > 0) ? static_cast<DWORD>((2000 / refresh) + 1) : 50;
        WaitForSingleObject(static_cast<HANDLE>(mWaitable), timeout);
    }

    /*
     * 锁：和解码线程共用 FFmpeg 设备上下文的那把锁（递归互斥）。
     *
     * 下面所有东西都在这把锁里面，包括交换链的 ResizeBuffers/Present：这些
     * 调用都会隐式冲刷立即上下文里的命令，而解码线程正拿着同一个上下文在解码，
     * 两边同时碰它是未定义行为。锁内的操作都很短（命令提交级别），不会有
     * 等垂直同步这种阻塞。
     */
    if (mLock != nullptr) {
        mLock(mLockCtx);
    }

    /*
     * 顺序不能乱：
     *   1. syncOutputSize() 先解决窗口尺寸变化（可能要 ResizeBuffers）；
     *   2. ensureProcessor() 按**新的**输出尺寸建枚举器和处理器；
     *   3. ensureOutputView() 才有枚举器可用。
     *
     * 一开始是先建输出视图的，那时 mEnumerator 还是空的，
     * CreateVideoProcessorOutputView 每次返回 E_INVALIDARG（0x80070057），而且因为
     * "输出视图失败 -> 不建处理器 -> 还是没有枚举器" 死循环，一帧都上不了屏。
     */
    bool ok = syncOutputSize();

    if (ok) {
        ok = ensureProcessor(static_cast<int>(framesCtx->width), static_cast<int>(framesCtx->height),
                             inputFormat);
    }

    if (ok) {
        ok = ensureOutputView();
    }

    if (ok) {
        ok = ensureInputView(texture, index) && ensureBarTexture(mWidth);
    }

    if (ok && mStagingInput) {
        /*
         * 走了中转纹理这条路：每帧都得把解码纹理拷进去。不能靠"纹理指针没变"来
         * 偷懒——解码器的表面池会反复用同一张纹理装新画面。
         */
        ok = copyInputToStaging(texture, index);
    }

    if (ok) {
        /*
         * 纹理是编码尺寸对齐过的（例如 3840x2176），有效画面只有左上角
         * getInfo().video 那么大，所以源矩形要裁掉底部的填充行。
         */
        const int videoWidth = iaFrame->getInfo().video.width;
        const int videoHeight = iaFrame->getInfo().video.height;

        if (videoWidth <= 0 || videoHeight <= 0) {
            if (mUnlock != nullptr) {
                mUnlock(mLockCtx);
            }

            return false;
        }

        mVideoWidth = videoWidth;
        mVideoHeight = videoHeight;
        ok = blitVideo(videoWidth, videoHeight);
    }

    if (ok) {
        ok = composeOverlay(bar, barInfo);
    }

    if (ok) {
        /*
         * Present 必须留在锁内，而且用 sync interval 0。
         *
         * IDXGISwapChain::Present 会隐式提交立即上下文里排队的命令，而解码线程
         * 正在用同一个立即上下文（它同样持这把锁）——两边同时碰上下文是未定义
         * 行为。留在锁内就没有并发。
         *
         * 用 0 而不是 1 是因为 Present(1, 0) 会等垂直同步（最多十几毫秒），那样
         * 等于拿着解码锁睡一整帧，把解码线程一起拖住。flip-model 交换链在窗口
         * 模式下由 DWM 合成，sync 0 不会撕裂；按刷新率节流由上面那个可等待对象
         * 负责（它在锁外等）。
         */
        ok = presentBackBuffer();
    }

    if (mUnlock != nullptr) {
        mUnlock(mLockCtx);
    }

    return ok;
}

/*
 * 视频处理器的 Blt：把当前输入视图（解码纹理）画到当前后台缓冲上。
 * 锁内调用；后台缓冲每次都要重新画满，因为 flip-model 用的是
 * DXGI_SWAP_EFFECT_FLIP_DISCARD——Present 之后后台缓冲的内容是未定义的，
 * 只贴一条控件条就 Present 会看到一片花屏。
 */
bool D3d11DirectPresenter::blitVideo(int videoWidth, int videoHeight)
{
    auto *videoContext = static_cast<ID3D11VideoContext *>(mVideoContext);
    auto *processor = static_cast<ID3D11VideoProcessor *>(mProcessor);

    if (videoContext == nullptr || processor == nullptr || mInputView == nullptr ||
            mOutputView == nullptr) {
        return false;
    }

    D3D11_VIDEO_COLOR black = {};
    black.RGBA.R = 0.0f;
    black.RGBA.G = 0.0f;
    black.RGBA.B = 0.0f;
    black.RGBA.A = 1.0f;

    D3D11_RECT full = {0, 0, mWidth, mHeight};
    D3D11_RECT source = {0, 0, videoWidth, videoHeight};

    /* 等比缩放居中：视频处理器在 GPU 上做缩放和 NV12->BGRA 转换。 */
    const SDL_Rect dstRect = aspectFit(videoWidth, videoHeight, mWidth, mHeight);
    D3D11_RECT dest = {dstRect.x, dstRect.y, dstRect.x + dstRect.w, dstRect.y + dstRect.h};

    videoContext->VideoProcessorSetStreamFrameFormat(processor, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext->VideoProcessorSetStreamOutputRate(processor, 0,
            D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, TRUE, nullptr);
    videoContext->VideoProcessorSetOutputTargetRect(processor, TRUE, &full);
    videoContext->VideoProcessorSetOutputBackgroundColor(processor, FALSE, &black);
    videoContext->VideoProcessorSetStreamSourceRect(processor, 0, TRUE, &source);
    videoContext->VideoProcessorSetStreamDestRect(processor, 0, TRUE, &dest);

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.OutputIndex = 0;
    stream.InputFrameOrField = 0;
    stream.PastFrames = 0;
    stream.FutureFrames = 0;
    stream.pInputSurface = static_cast<ID3D11VideoProcessorInputView *>(mInputView);

    HRESULT hr = videoContext->VideoProcessorBlt(processor,
                     static_cast<ID3D11VideoProcessorOutputView *>(mOutputView), 0, 1, &stream);

    if (FAILED(hr)) {
        return fail("VideoProcessorBlt failed", hr);
    }

    return true;
}

/*
 * 控件条贴图（锁内调用）。控件条不可见、排版还没跟上、或者窗口比控件条还矮时
 * 直接跳过——视频那一层每次都是整幅重画的，不贴就等于没有控件条。
 */
bool D3d11DirectPresenter::composeOverlay(SdlControlBar *bar, const SdlControlBar::Info &info)
{
    if (bar == nullptr || mBarTexture == nullptr || mContext == nullptr || mBackBuffer == nullptr ||
            !bar->visible()) {
        return true;
    }

    const int barHeight = bar->barHeight();

    if (barHeight <= 0 || barHeight > BAR_BITMAP_MAX_HEIGHT || mHeight < barHeight) {
        return true;
    }

    if (!uploadBar(bar, info)) {
        return true;
    }

    /* 控件条那条路没有混合能力：位图已经和黑色预乘好了，直接贴上去。 */
    D3D11_BOX box = {};
    box.left = 0;
    box.top = 0;
    box.right = static_cast<UINT>(mBarTextureWidth);
    box.bottom = static_cast<UINT>(barHeight);
    box.front = 0;
    box.back = 1;

    static_cast<ID3D11DeviceContext *>(mContext)->CopySubresourceRegion(
        static_cast<ID3D11Texture2D *>(mBackBuffer), 0, 0,
        static_cast<UINT>(mHeight - barHeight), 0,
        static_cast<ID3D11Texture2D *>(mBarTexture), 0, &box);
    return true;
}

bool D3d11DirectPresenter::presentBackBuffer()
{
    HRESULT hr = static_cast<IDXGISwapChain1 *>(mSwapChain)->Present(0, 0);

    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            return fail("the D3D11 device was removed", hr);
        }

        return fail("Present failed", hr);
    }

    /* 这一帧真上屏了：失败计数清零，日志限流重新开始。 */
    mFailureCount = 0;

    /*
     * 没有 IDXGISwapChain3 时用的兜底轮转（有它时 ensureOutputView 直接问它，
     * 这里保持同步只是为了让两种来源不打架）。
     */
    if (mBufferCount > 0) {
        mBackBufferIndex = (mBackBufferIndex + 1) % mBufferCount;
    }

    if (!mLoggedFirstFrame) {
        mLoggedFirstFrame = true;
        AF_LOGI("-direct: first zero-copy frame presented, no CPU copy involved\n");

        /*
         * 现在才把视频子窗口显示出来：它创建时是隐藏的，因为交换链要等第一帧
         * 才知道用哪个设备，在那之前显示出来只会是一片没有内容的白色。
         * SW_SHOWNA：不抢激活，键盘仍旧归 SDL 的父窗口。
         */
        if (!mShownWindow && mHwnd != nullptr) {
            mShownWindow = true;
            ShowWindow(static_cast<HWND>(mHwnd), SW_SHOWNA);
        }
    }

    return true;
}

/*
 * 暂停时"只刷控件条"那条路：没有新帧，直接拿上一次的输入视图再画一遍
 * （暂停期间解码器不再动那张纹理，所以内容还是当前这一帧）。
 */
bool D3d11DirectPresenter::refreshOverlayOnly(SdlControlBar *bar,
        const SdlControlBar::Info &barInfo)
{
    if (!mReady || mSwapChain == nullptr || mInputView == nullptr ||
            mVideoWidth <= 0 || mVideoHeight <= 0 || mLock == nullptr || mUnlock == nullptr) {
        return false;
    }

    mLock(mLockCtx);

    /*
     * 也要重新跑一遍视频 Blt：flip-model 的 FLIP_DISCARD 在 Present 之后会丢弃
     * 后台缓冲的内容，光贴一条控件条 Present 出去就是花屏。
     * 顺序和 present() 里一致：先解决尺寸，再处理器，最后才是输出视图。
     */
    bool ok = syncOutputSize() && ensureProcessor(mInputWidth, mInputHeight, mInputFormat) &&
              ensureOutputView() && ensureBarTexture(mWidth) &&
              blitVideo(mVideoWidth, mVideoHeight);

    if (ok) {
        ok = composeOverlay(bar, barInfo);
    }

    if (ok) {
        ok = presentBackBuffer();
    }

    mUnlock(mLockCtx);
    return ok;
}

#endif // defined(_WIN32) && defined(ENABLE_SDL)
