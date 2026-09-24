//
// Windows 零拷贝呈现（cmdline -direct）。
//
// 解码器把解码好的 D3D11 纹理原样交上来（AF_PIX_FMT_D3D11），这里用 FFmpeg
// 自己的 ID3D11Device 建一条交换链，再用 ID3D11VideoProcessor 把 NV12 纹理
// 直接缩放/转换到后台缓冲上——全程 GPU 到 GPU，一个字节都不经过 CPU。
// 对比 copy-back 路径每帧要搬 12.4MB 下载 + 12.4MB 上传（4K60 约 1.5GB/s）。
//
// 为什么必须自己呈现：SDL2 没有“把外部 ID3D11Texture2D 包成 SDL_Texture”的
// API（SDL3 才有），所以只要还用 SDL 的渲染器画视频，GPU→内存→GPU 这一次
// 往返就省不掉。
//
// 四个关键点：
//   * 设备和锁都来自帧本身：AVFrame::hw_frames_ctx -> AVHWFramesContext::
//     device_ref -> AVD3D11VADeviceContext。FFmpeg 默认会给设备上下文配一把
//     互斥锁（hwcontext_d3d11va.c:513），解码器自己也在用它
//     （libavcodec/dxva2.c: ff_dxva2_lock），所以这里必须在 lock/unlock 之间
//     访问设备和上下文，才能和解码线程安全并发——这也是 mpv 的做法。
//   * 交换链挂在**子窗口**上：SDL 的 D3D11 渲染器已经在这个 HWND 上建了一条
//     flip-model 交换链，而 DXGI 规定同一个 HWND 同时只能有一条 flip-model
//     交换链（第二条会 DXGI_ERROR_INVALID_CALL）。子窗口有自己的 HWND，两边
//     互不干扰；直通模式下 SDL 那条什么都不画，只是占着窗口。
//   * 控件条用 CPU 画成一张小位图，再用 CopySubresourceRegion 贴到后台缓冲
//     底部：不需要着色器，也不需要第二条视频处理流，代价只有几十 KB/次。
//   * 整条路径不含任何 SDL 调用，所以呈现可以放在框架的 VSync 线程上做：Windows
//     拖动/缩放窗口时用的是模态循环（WM_ENTERSIZEMOVE），它阻塞的是拥有窗口的
//     那个线程（事件线程）。SDL 的窗口和渲染器只能在那个线程上用，因此只要还用
//     SDL 画视频，拖动窗口期间画面就必然停住；这条自建交换链的路不受影响，
//     画面照常出（这也是 mpv 的 vo 线程能边拖边放的原因）。
//
#ifndef CICADAMEDIA_D3D11DIRECTPRESENTER_H
#define CICADAMEDIA_D3D11DIRECTPRESENTER_H

#include <SDL2/SDL.h>
#include <atomic>
#include <cstdint>
#include <mutex>

#include "SdlControlBar.h"

struct AVFrame;
class IAFFrame;

class D3d11DirectPresenter {
public:
    /*
     * 启动时探测：这台机器的 D3D11 能不能做 NV12 -> BGRA 的视频处理。
     * 用一个临时设备探，探不到就别开直通（否则解码纹理没法上屏）。
     */
    static bool supported();

    /*
     * 主显示器的刷新率（Hz），拿不到返回 0。
     *
     * 给应用在 Prepare() 之前设置渲染回调频率用
     * （PROPERTY_KEY_VIDEO_RENDER_HZ）：呈现要按显示器刷新率节流，回调也跟着
     * 刷新率走，高刷屏上的 24fps 片源才不会挤在 60Hz 的格子上（3:2 抖动）。
     */
    static int primaryDisplayRefreshHz();

    D3d11DirectPresenter() = default;
    ~D3d11DirectPresenter();

    D3d11DirectPresenter(const D3d11DirectPresenter &) = delete;
    D3d11DirectPresenter &operator=(const D3d11DirectPresenter &) = delete;

    /* 子窗口句柄，由事件线程在窗口建好后传入。 */
    void setWindow(void *hwnd);

    /*
     * 画一帧。frame 必须是 AF_PIX_FMT_D3D11 的硬件帧（解码纹理），
     * 有效图像区域由这里的 getInfo().video 宽高决定（解码纹理按编码尺寸
     * 对齐，例如 3840x2176 里只有上面 2160 行是画面）。等比缩放居中在
     * GPU 上由视频处理器完成。
     * bar 可以为空；barInfo 只在 bar 可见时使用。
     *
     * 这个方法**不碰 SDL**（只用 D3D11 和自己的交换链），所以既可以在框架的
     * VSync 线程上调用（正常播放：这样拖动/缩放窗口时画面不会停），也可以在
     * 事件线程上调用（暂停时控件条变了，需要单独刷一帧）。两条线程之间用
     * mPresentMutex 串行。
     *
     * frame 为 nullptr 表示"只更新控件条，不换视频帧"（暂停时的悬停/拖动）。
     */
    bool present(IAFFrame *frame, SdlControlBar *bar, const SdlControlBar::Info &barInfo);

    void close();

    bool ready() const
    {
        return mReady;
    }

    const char *lastError() const
    {
        return mLastError;
    }

private:
    bool initFromFrame(AVFrame *frame);
    bool ensureProcessor(int inputWidth, int inputHeight, int inputFormat);
    bool ensureInputView(void *texture, int64_t index);
    /*
     * 驱动不接受"解码表面直接当视频处理器输入"时用的中转纹理：RENDER_TARGET |
     * SHADER_RESOURCE，格式尺寸同解码纹理，每帧 GPU 内部拷一次。
     */
    bool ensureStagingTexture();
    bool copyInputToStaging(void *texture, int64_t index);
    /*
     * 窗口尺寸自检（要重建后台缓冲时顺带 ResizeBuffers）。
     * 必须在 ensureProcessor() 之前调用：枚举器是按输出尺寸建的。
     */
    bool syncOutputSize();
    bool ensureOutputView();
    bool ensureBarTexture(int width);
    bool uploadBar(SdlControlBar *bar, const SdlControlBar::Info &info);
    /* 视频处理器 Blt（锁内调用）。 */
    bool blitVideo(int videoWidth, int videoHeight);
    /* 把控件条位图贴到后台缓冲底部（锁内调用），不需要贴时也返回 true。 */
    bool composeOverlay(SdlControlBar *bar, const SdlControlBar::Info &info);
    /* 后台缓冲已经画好：Present（锁内调用）。 */
    bool presentBackBuffer();
    /*
     * 暂停时只刷控件条：没有新帧，用上一次的输入视图重画后台缓冲再 Present。
     * 事件线程调用（暂停时 VSync 线程不会再来新帧）。
     */
    bool refreshOverlayOnly(SdlControlBar *bar, const SdlControlBar::Info &barInfo);
    void releaseSwapChainTargets();
    /* 连交换链和它的等待句柄一起放掉（换窗口、改格式、析构都用它）。 */
    void releaseSwapChain();
    void releaseAll();
    bool fail(const char *what, long hr);

    /*
     * 显示器是否处在 HDR 模式（Windows 显示设置里打开了 HDR）。用窗口所在的
     * 那个 output 的 GetDesc1() 判断：ColorSpace 是 PQ/P2020 且亮度上限够高。
     */
    bool queryDisplayHdr();

    /* 显示器刷新率（Hz），拿不到就返回 0。 */
    int queryDisplayRefreshHz();

    /* 全部用 void* 存，避免这个头文件把 d3d11.h / dxgi 拖给调用方。 */
    void *mDevice = nullptr;        // ID3D11Device *
    void *mContext = nullptr;       // ID3D11DeviceContext *
    void *mVideoDevice = nullptr;   // ID3D11VideoDevice *
    void *mVideoContext = nullptr;  // ID3D11VideoContext *
    void *mVideoContext1 = nullptr; // ID3D11VideoContext1 *（色彩空间/HDR 用）
    void *mSwapChain = nullptr;     // IDXGISwapChain1 *
    void *mSwapChain2 = nullptr;    // IDXGISwapChain2 *（SetMaximumFrameLatency + 等待句柄）
    /*
     * IDXGISwapChain3 *：GetCurrentBackBufferIndex() 在**这一层**上（DXGI 1.4），
     * IDXGISwapChain2 没有这个方法。取不到它（Windows 8.1 之前没有 DXGI 1.4）就
     * 自己按 mBufferCount 轮转，见 ensureOutputView()。
     */
    void *mSwapChain3 = nullptr;
    int mBackBufferIndex = 0;
    void *mWaitable = nullptr;      // HANDLE，可等待交换链对象（按刷新率节流）
    /*
     * flip-model 交换链有两个后台缓冲，Present 之后当前索引会切换，所以每个
     * 索引各留一份后台缓冲和输出视图；mBackBuffer/mOutputView 指向当前那个。
     */
    void *mBackBuffers[3] = {};     // ID3D11Texture2D *
    void *mOutputViews[3] = {};     // ID3D11VideoProcessorOutputView *
    void *mBackBuffer = nullptr;    // ID3D11Texture2D *，当前索引
    void *mOutputView = nullptr;    // ID3D11VideoProcessorOutputView *，当前索引
    int mBufferCount = 2;
    void *mEnumerator = nullptr;    // ID3D11VideoProcessorEnumerator *
    void *mProcessor = nullptr;     // ID3D11VideoProcessor *
    void *mInputView = nullptr;     // ID3D11VideoProcessorInputView *
    void *mInputTexture = nullptr;  // ID3D11Texture2D * 当前输入视图对应的纹理
    int64_t mInputIndex = -1;
    /* 中转纹理（只在驱动不认解码表面时创建），以及"是否正在用它"。 */
    void *mStagingTexture = nullptr; // ID3D11Texture2D *
    bool mStagingInput = false;
    void *mBarTexture = nullptr;    // ID3D11Texture2D *
    int mBarTextureWidth = 0;
    uint32_t *mBarPixels = nullptr;
    /* HDR 直通时 BGRA 位图转换后的 PQ 10bit 版本（和后台缓冲同格式才能拷贝）。 */
    uint32_t *mBarPixels10 = nullptr;
    /*
     * 控件条的位图只在“看得见的变化”发生时重画并上传：把滑块位置量化到像素，
     * 播放位置每秒变化几次而不是每帧，这样 CPU 侧几乎不花时间。
     */
    uint64_t mBarSignature = 0;

    void (*mLock)(void *ctx) = nullptr;
    void (*mUnlock)(void *ctx) = nullptr;
    void *mLockCtx = nullptr;

    void *mHwnd = nullptr;
    int mWidth = 0;
    int mHeight = 0;
    int mInputWidth = 0;
    int mInputHeight = 0;
    /*
     * 枚举器/处理器是带着**输出尺寸**建的（D3D11_VIDEO_PROCESSOR_CONTENT_DESC），
     * 窗口尺寸一变就得重建，所以要把建它时用的输出尺寸记下来比对。
     */
    int mOutputWidth = 0;
    int mOutputHeight = 0;
    /*
     * 最近一帧的**显示**尺寸（不是解码纹理尺寸）：暂停时只刷控件条也要重跑
     * 一遍视频 Blt，没有新帧就只能用上次这组数字。
     */
    int mVideoWidth = 0;
    int mVideoHeight = 0;

    /*
     * 格式与色彩空间（存成 int，避免头文件引 dxgi 的枚举）：
     *   mInputFormat  解码纹理的 DXGI 格式：8bit 是 NV12，10bit HDR 是 P010
     *   mBackFormat   交换链后台缓冲格式：SDR 是 B8G8R8A8，HDR 直通是 R10G10B10A2
     *   mInputColorSpace / mOutputColorSpace 交给视频处理器的色彩空间
     * 视频是 HDR 且显示器开着 HDR 时走直通（10bit + PQ/P2020），否则视频处理器
     * 会按输入色彩空间把 HDR 内容色调映射到 SDR——这就是“HDR 视频在 SDR 屏幕上
     * 不再发灰”的原因。
     */
    int mInputFormat = 0;
    int mBackFormat = 0;
    int mInputColorSpace = 0;
    int mOutputColorSpace = 0;
    bool mHdrContent = false;
    bool mHdrOutput = false;
    /*
     * 驱动/系统不支持 PQ 输出（SetColorSpace1 失败）时置上：之后不再尝试 HDR
     * 直通，也不用每隔两秒重建一次交换链。
     */
    bool mHdrPassThroughBlocked = false;
    int mDisplayRefreshHz = 0;
    bool mDisplayHdr = false;
    /* 每隔 HDR_RECHECK_FRAMES 帧复查一次显示器 HDR 状态。 */
    int mFrameCounter = 0;
    /* 连续失败计数（成功一帧就清零），用来给日志限流。 */
    int mFailureCount = 0;

    /*
     * 呈现可能同时来自两条线程（VSync 线程出画、事件线程在暂停时刷控件条），
     * 所有成员状态都由这把锁串起来。注意锁的顺序：mPresentMutex ->
     * (FFmpeg 设备锁)，反过来没有，所以不会死锁。
     */
    std::mutex mPresentMutex;

    /* ready() 会被另一条线程读（事件线程判断要不要交还窗口），用原子量。 */
    std::atomic<bool> mReady{false};
    /*
     * 视频子窗口在第一帧成功上屏之前保持隐藏，见 present()。事件线程在 setWindow()
     * 里会把它清掉（换了窗口），VSync 线程在 Present 成功后会置起来，所以也用原子量。
     */
    std::atomic<bool> mShownWindow{false};
    bool mLoggedFirstFrame = false;
    const char *mLastError = "";
};

#endif // CICADAMEDIA_D3D11DIRECTPRESENTER_H
