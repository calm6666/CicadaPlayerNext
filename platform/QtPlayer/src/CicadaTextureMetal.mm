//
// macOS 零拷贝后端的实现，设计说明见 CicadaTextureMetal.h。
//

#include "CicadaTextureMetal.h"
#include "CicadaQtTextureWrap.h"

#if defined(Q_OS_MACOS)

#include <CoreVideo/CoreVideo.h>
#include <CoreVideo/CVMetalTextureCache.h>
#include <Metal/Metal.h>

#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>
#include <QtCore/QSize>

#include <utils/frame_work_log.h>
#include "base/media/PBAFFrame.h"

namespace cicadaqt {

    /*
     * 放掉一个**我们自己 new/alloc/retain 出来**的 Objective-C 对象。
     *
     * 本文件在 ARC 与非 ARC（MRR）两种编译方式下都必须能编能跑：
     *   * platform/QtPlayer/CMakeLists.txt **没有**给本目录设 -fobjc-arc（设了的是
     *     platform/Apple/source 与 mediaPlayer 那两个**别的** target，它们的变量不会
     *     往上传到本目录），所以本目标实际编译方式是 MRR —— 自己 new 出来的对象要显式 release；
     *   * 但不写死这个前提：万一以后给本目录加上 -fobjc-arc，显式 release 就是编译错误
     *     （ARC 下该方法不可用），所以用 __has_feature 把差异封在这一个函数里。
     *     同样的写法在本仓库有先例：framework/codec/utils_ios.mm 的 CFBridging_Release。
     *
     * 用模板是因为调用点上拿到的是 id<MTLTexture> / id<MTLComputePipelineState> 这类
     * **协议限定指针**：它们和裸 id 是不同的类型，形参写成 `id &` 会绑不上（要转临时量）。
     */
    template <typename T>
    static void releaseOwnedObject(T &object)
    {
        if (object == nil) {
            return;
        }

#if !__has_feature(objc_arc)
        [object release];
#endif

        object = nil;
    }

    /* 色彩调整的内核参数（与下面 Metal 源码里的同名结构体**逐字段对应**；只用 POD）。 */
    struct ColorAdjustParams {
        float brightness;      /* 加在 RGB 上的偏移，0 = 中性 */
        float contrast;        /* 围绕 0.5 中灰的缩放，1 = 中性 */
        float saturation;      /* 围绕亮度的缩放，1 = 中性 */
        float lumaR;
        float lumaG;
        float lumaB;
        unsigned int width;
        unsigned int height;
    };

    /*
     * 色彩调整的 Metal 计算内核源码（运行时编译一次，见 ensureColorAdjustPipeline）。
     *
     * 【为什么把 Core Image 整条路换掉】这条路已经两次"一拖滑块整屏红"，两次都出在
     * Core Image 那些**我们控制不到的隐含约定**上：
     *   * `CIImage imageWithMTLTexture:` 会把纹理内容按**预乘 alpha**解释。VideoToolbox 直出的
     *     32BGRA 是**不透明视频**，alpha 通道本来没有约定（可能是 0）；alpha = 0 时
     *     反预乘会算出 0/0，颜色内核随后算出的非法值写进 8bit 纹理就可能变成"某几个通道饱和"
     *     —— 真机看到的"整屏红"正是这一类（单通道/饱和）的形态；
     *   * 色彩管理还有一串旋钮（输入图的 colorSpace、CIContext 的 workingColorSpace /
     *     workingFormat、render 时的目标 colorSpace），任何一个不对都是整体变色。
     * 计算内核没有这些隐含约定：读进来就是 (R,G,B,A) 四个通道，算完写回去，
     * 每一步都在我们手里；**alpha 一律写 1**，预乘那类问题从根上不存在。
     *
     * 通道顺序：内核里用 `texture2d<float>` 读 bgra8unorm 纹理时，Metal 按**像素格式**把通道
     * 映射好再交给我们 —— sample.r 就是红、sample.b 就是蓝，不需要自己换字节序（写回去同理）。
     * 这正是"用类型化纹理"比"把像素当一块裸 buffer 处理"更不容易出错的地方。
     */
    static const char *kColorAdjustKernelSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct ColorAdjustParams {
    float brightness;
    float contrast;
    float saturation;
    float lumaR;
    float lumaG;
    float lumaB;
    uint  width;
    uint  height;
};

kernel void cicadaColorAdjust(texture2d<float, access::read>  src [[texture(0)]],
                              texture2d<float, access::write> dst [[texture(1)]],
                              constant ColorAdjustParams &params [[buffer(0)]],
                              uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= params.width || gid.y >= params.height) {
        return;
    }

    const float4 sample = src.read(gid);

    float3 rgb = sample.rgb + params.brightness;
    rgb = (rgb - 0.5f) * params.contrast + 0.5f;

    const float luma = dot(rgb, float3(params.lumaR, params.lumaG, params.lumaB));
    rgb = mix(float3(luma), rgb, params.saturation);

    rgb = min(max(rgb, float3(0.0f)), float3(1.0f));

    dst.write(float4(rgb, 1.0f), gid);
}
)METAL";

    /*
     * 面板给的 0~200（100 = 中性）→ 内核参数。量纲与方向跟
     * CicadaTextureD3D11::applyColorAdjust() 完全一致：**100 是中性、两端对称、线性**。
     *   * 亮度：加在 RGB 上的偏移，0 → -0.5、100 → 0、200 → +0.5；
     *   * 对比度：围绕 0.5 中灰缩放，0 → 0（全中灰）、100 → 1、200 → 2；
     *   * 饱和度：围绕亮度缩放，0 → 0（灰度）、100 → 1、200 → 2。
     * D3D11 那边"每一格有多强"来自驱动给的 range
     * （ID3D11VideoProcessorEnumerator::GetVideoProcessorFilterRange），这里没有驱动可问，
     * 所以用固定刻度：方向与中性点必须一致，强度就是上面这两个式子（日志里会把实际值打出来）。
     */
    static float mapColorAdjustOffset(float value)
    {
        return (value - 100.0f) / 100.0f * 0.5f;
    }

    static float mapColorAdjustScale(float value)
    {
        return 1.0f + (value - 100.0f) / 100.0f;
    }

    /*
     * 下面三个只服务于诊断日志（真机上排查"整屏红"那类通道/格式问题就靠它）：
     * 全部用 10.0/10.11 起就有的 API，不做任何新分配。
     */
    static const char *cicadaPixelFormatName(MTLPixelFormat format)
    {
        switch (format) {
            case MTLPixelFormatBGRA8Unorm:
                return "BGRA8Unorm";

            case MTLPixelFormatBGRA8Unorm_sRGB:
                return "BGRA8Unorm_sRGB";

            case MTLPixelFormatRGBA8Unorm:
                return "RGBA8Unorm";

            case MTLPixelFormatRGBA8Unorm_sRGB:
                return "RGBA8Unorm_sRGB";

            default:
                return "other";
        }
    }

    /* Objective-C / CoreVideo 类型都藏在这里，头文件保持干净。 */
    struct CicadaTextureMetal::Private {
        id<MTLDevice> device = nil;
        CVMetalTextureCacheRef cache = nullptr;
        /*
         * 近几帧的纹理引用。
         *
         * CVMetalTextureRef 必须活到 Qt 真正采样完 —— Qt 的场景图是异步的，上一两帧
         * 的命令缓冲可能还在跑，提前释放会让 IOSurface 映射失效（花屏或崩溃）。
         * 用一个 3 槽的环：每帧覆盖最老的那一个，既保证有余量，又不会无限增长。
         */
        CVMetalTextureRef refs[3] = {nullptr, nullptr, nullptr};
        int next = 0;

        /*
         * ==================== 色彩调整（GPU-only，全程零 CPU 下载）====================
         *
         * 全部**惰性创建**：没拖过滑块的用户一分钱不花。只在**渲染线程**上访问
         * （textureForFrame 那条线），所以不需要锁 —— 和 refs 一样。
         */
        /*
         * 计算管线：内核源码在第一次真的要用时编译一次，之后一直复用。
         * pipelineReady / pipelineFailed 是一次性决策 —— 失败只打一条明确的日志，
         * 之后**不再重试**（没有重试、没有计时器），画面照常走直通那条路。
         */
        id<MTLComputePipelineState> pipeline = nil;
        bool pipelineReady = false;
        bool pipelineFailed = false;
        /*
         * "这个 pass 这一帧准备失败"的日志只打一次：失败是**每帧都会重试**的（不做计时器、
         * 不做退避），不设这个标志就会变成每帧一条 E 日志。
         */
        bool passFailureLogged = false;
        /*
         * 输出纹理环（3 槽）：和 refs[3] 一个思路 —— Qt 的场景图是异步的，上一两帧的命令
         * 缓冲可能还在采样这张纹理，所以不能只用一张。有界（最多 3 张），尺寸**或格式**变了
         * 整体重建。
         */
        struct OutputSlot {
            id<MTLTexture> texture = nil;
        };
        OutputSlot outputs[3];
        int outputWidth = 0;
        int outputHeight = 0;
        /*
         * 输出纹理的像素格式：**直接取输入纹理的**（VT 直出 32BGRA ⇒ BGRA8Unorm）。
         * 绝不能写死别的格式：格式与输入/取样侧不一致时，轻则红蓝互换，重则只剩一个通道。
         */
        MTLPixelFormat outputFormat = MTLPixelFormatInvalid;
        int outputNext = 0;
        /*
         * 场景图的 Metal 命令队列（从 getResource 借的，我们不持有、也不放）。
         * 拿到它才能把我们这个 pass 排在 Qt 那一帧的采样之前，见 colorAdjustedTexture()。
         */
        id<MTLCommandQueue> queue = nil;
        bool queueResolved = false;

        /* 下面三个只在渲染线程调用。 */
        bool ensureColorAdjustPipeline();
        bool ensureOutputTextures(int width, int height, MTLPixelFormat format);
        /*
         * logDiagnostics = true 时打**一条**诊断日志（首次生效 / 参数变化时由调用方给 true，
         * 绝不每帧打）。字段见实现。
         */
        id<MTLTexture> colorAdjustedTexture(QQuickWindow *window, id<MTLTexture> input, int width,
                                            int height, float brightness, float contrast,
                                            float saturation, bool logDiagnostics);
    };

    /*
     * 编译色彩调整的计算内核并建管线（一次性，惰性）。
     *
     * 【为什么这条路是"可证明正确"的，而 Core Image 那条不是】
     * 内核源码就在本文件里（kColorAdjustKernelSource），输入输出都是**我们自己建的**纹理，
     * 通道顺序由 Metal 按像素格式映射（BGRA 纹理读出来就是 R,G,B,A），
     * alpha 我们**一律写 1**。整条链里没有任何"隐含约定"：
     *   * 不存在预乘/反预乘的歧义（我们根本不读 alpha）；
     *   * 不存在色彩管理（没有 colorSpace、没有 workingColorSpace / workingFormat）；
     *   * 不存在"渲染器要不要真的写这张纹理"的不确定（我们自己 per-pixel 写满每一个像素，
     *     参数里带 width/height，越界像素直接 return，所以不存在未初始化的行）。
     * 编译失败/建管线失败 → 一次性明确日志 + 永久走直通（不重试、不降级到 CPU）。
     */
    bool CicadaTextureMetal::Private::ensureColorAdjustPipeline()
    {
        if (pipelineReady) {
            return true;
        }

        if (pipelineFailed || device == nil) {
            return false;
        }

        /*
         * 编译过程会创建自释放对象（NSError、临时字符串），所以自己开一个自动释放池：
         * 调用方是 Qt 的渲染线程，不能指望它头上有池。
         */
        @autoreleasepool {
            NSError *error = nil;
            NSString *source = [NSString stringWithUTF8String:kColorAdjustKernelSource];
            id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];

            if (library == nil) {
                pipelineFailed = true;
                AF_LOGE("colour adjust: disabled (Metal kernel failed to compile: %s) — "
                        "the picture keeps the direct zero-copy path\n",
                        error != nil ? [[error localizedDescription] UTF8String] : "unknown error");
                return false;
            }

            id<MTLFunction> function = [library newFunctionWithName:@"cicadaColorAdjust"];

            if (function == nil) {
                releaseOwnedObject(library);
                pipelineFailed = true;
                AF_LOGE("colour adjust: disabled (Metal kernel function 'cicadaColorAdjust' not found) — "
                        "the picture keeps the direct zero-copy path\n");
                return false;
            }

            pipeline = [device newComputePipelineStateWithFunction:function error:&error];

            /* 局部这两个只在建管线时用得到：MRR 下必须自己放（ARC 下这行会被编译掉）。 */
            releaseOwnedObject(function);
            releaseOwnedObject(library);

            if (pipeline == nil) {
                pipelineFailed = true;
                AF_LOGE("colour adjust: disabled (cannot create the Metal compute pipeline: %s) — "
                        "the picture keeps the direct zero-copy path\n",
                        error != nil ? [[error localizedDescription] UTF8String] : "unknown error");
                return false;
            }

            pipelineReady = true;
            AF_LOGI("colour adjust: Metal compute kernel is ready (cicadaColorAdjust, "
                    "brightness/contrast/saturation, alpha forced to 1, GPU only)\n");
        }

        return true;
    }

    /*
     * 输出纹理环：惰性建、按尺寸+格式复用（变了就整体重建，和有界的 refs 环一个思路）。
     * 计算内核把结果写进这里，再由 Qt 采样 —— 全程在 GPU 上，不读回 CPU。
     *
     * format **必须**是输入纹理那一份（调用方传进来）：写死别的格式就是"红蓝互换/只剩一个通道"
     * 那类事故。
     */
    bool CicadaTextureMetal::Private::ensureOutputTextures(int width, int height, MTLPixelFormat format)
    {
        if (device == nil || width <= 0 || height <= 0 || format == MTLPixelFormatInvalid) {
            return false;
        }

        if (outputs[0].texture != nil && outputWidth == width && outputHeight == height &&
                outputFormat == format) {
            return true;
        }

        for (int i = 0; i < 3; ++i) {
            releaseOwnedObject(outputs[i].texture);
        }

        outputWidth = 0;
        outputHeight = 0;
        outputFormat = MTLPixelFormatInvalid;

        /*
         * 尺寸与格式都跟**解码纹理**一致（BGRA8；就是显示尺寸，内核这里只做颜色运算、
         * 不缩放），所以交给 Qt 的纹理与直通那条路同尺寸同格式，后面的缩放 / letterbox /
         * wrap 逻辑一个字都不用改。
         *
         * usage：Qt 的场景图要采样它（ShaderRead），计算内核要写它（ShaderWrite）。
         * storageMode 用 Private：这张纹理只在 GPU 上产生、只在 GPU 上消费，永远不读回 CPU
         *（这正是零拷贝那条硬约束）。
         */
        MTLTextureDescriptor *descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                              width:(NSUInteger) width
                                                             height:(NSUInteger) height
                                                          mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        descriptor.storageMode = MTLStorageModePrivate;

        for (int i = 0; i < 3; ++i) {
            outputs[i].texture = [device newTextureWithDescriptor:descriptor];

            if (outputs[i].texture == nil) {
                for (int j = 0; j < 3; ++j) {
                    releaseOwnedObject(outputs[j].texture);
                }

                AF_LOGE("cannot allocate the colour adjust output texture (%dx%d, format %d)\n",
                        width, height, (int) format);
                return false;
            }
        }

        outputWidth = width;
        outputHeight = height;
        outputFormat = format;
        outputNext = 0;
        return true;
    }

    /*
     * 把一帧从解码纹理过一遍计算内核，写进环里的输出纹理，返回那张输出纹理。
     *
     * 【零 CPU 下载】输入是 VideoToolbox 的 MTLTexture（IOSurface 直通），输出是我们自己的
     * MTLTexture，中间只有一个计算 pass：没有 CVPixelBufferGetBaseAddress、没有 QImage、
     * 没有 swscale，退出这条路也不会碰到 textureForFrameCpu。
     *
     * 【顺序：为什么从 Qt 借命令队列】Qt 的场景图是异步的，我们这次写必须排在"Qt 采样那张
     * 输出纹理"之前：命令缓冲从**场景图自己的队列**新建
     * （QSGRendererInterface::CommandQueueResource，Qt 文档明确 Metal 下它就是 MTLCommandQueue *），
     * 同一个队列上的命令缓冲按**提交顺序**开始执行，而 Qt 那一帧的命令缓冲是在
     * updatePaintNode 返回之后才提交的 ⇒ "我们写 → Qt 采样"这个顺序天然成立。
     * 借不到队列时**不做这个 pass**（打一条明确的 disabled 日志，画面照常走直通）：
     * 宁可这次不变色，也不要一条没有顺序保证的写 —— 那会读到写了一半的纹理。
     */
    id<MTLTexture> CicadaTextureMetal::Private::colorAdjustedTexture(QQuickWindow *window,
            id<MTLTexture> input, int width, int height, float brightness, float contrast,
            float saturation, bool logDiagnostics)
    {
        /*
         * 每帧都会创建自释放对象（命令缓冲、编码器），而调用方是 Qt 的渲染线程 ——
         * 不能指望外面每帧都有自动释放池，所以自己开一个。管线与输出纹理是我们自己持有的，
         * 不受这个池影响。
         */
        @autoreleasepool {
            if (window == nullptr || input == nil) {
                return nil;
            }

            /*
             * 队列：惰性问一次（之后记住结论）。拿不到就永久走直通，日志里给一句明确原因。
             */
            if (!queueResolved) {
                queueResolved = true;
                auto *rif = window->rendererInterface();
                queue = (rif != nullptr)
                        ? (__bridge id<MTLCommandQueue>) rif->getResource(
                              window, QSGRendererInterface::CommandQueueResource)
                        : nil;

                if (queue == nil) {
                    AF_LOGE("colour adjust: disabled (the scene graph gave no Metal command queue, "
                            "so this pass cannot be ordered before Qt samples the texture) — "
                            "the picture keeps the direct zero-copy path\n");
                }
            }

            if (queue == nil || !ensureColorAdjustPipeline()) {
                return nil;
            }

            if (!ensureOutputTextures(width, height, input.pixelFormat)) {
                if (!passFailureLogged) {
                    passFailureLogged = true;
                    AF_LOGE("colour adjust: disabled (cannot allocate the %dx%d output texture, "
                            "format %d) — the picture keeps the direct zero-copy path\n",
                            width, height, (int) input.pixelFormat);
                }

                return nil;
            }

            /*
             * 面板的 0~200（100 = 中性）→ 内核参数。中点是 0 / 1 / 1，方向与 D3D11 一致：
             * 亮度变大更亮、对比度变大对比更强、饱和度变大更艳（0 就是灰度）。
             * Rec.709 的亮度权重（0.2126 / 0.7152 / 0.0722）用在饱和度那一步。
             */
            ColorAdjustParams params = {};
            params.brightness = mapColorAdjustOffset(brightness);
            params.contrast = mapColorAdjustScale(contrast);
            params.saturation = mapColorAdjustScale(saturation);
            params.lumaR = 0.2126f;
            params.lumaG = 0.7152f;
            params.lumaB = 0.0722f;
            params.width = (unsigned int) width;
            params.height = (unsigned int) height;

            OutputSlot &slot = outputs[outputNext];
            id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];

            if (commandBuffer == nil) {
                if (!passFailureLogged) {
                    passFailureLogged = true;
                    AF_LOGE("colour adjust: disabled (the scene graph queue refused a command buffer) — "
                            "the picture keeps the direct zero-copy path\n");
                }

                return nil;
            }

            id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];

            if (encoder == nil) {
                if (!passFailureLogged) {
                    passFailureLogged = true;
                    AF_LOGE("colour adjust: disabled (cannot create a Metal compute encoder) — "
                            "the picture keeps the direct zero-copy path\n");
                }

                return nil;
            }

            [encoder setComputePipelineState:pipeline];
            [encoder setTexture:input atIndex:0];
            [encoder setTexture:slot.texture atIndex:1];
            [encoder setBytes:&params length:sizeof(params) atIndex:0];

            /*
             * 线程组固定 16x16，按 ceil(w/16) x ceil(h/16) 铺满：内核里对越界像素 early-return，
             * 所以每一个像素都会被写一次（不存在"没写到的行"）。
             * 刻意**不用** dispatchThreads:（非均匀线程组在老的 Intel Mac 上不支持）。
             */
            const NSUInteger threadGroupSize16 = 16;
            MTLSize threadGroupSize = MTLSizeMake(threadGroupSize16, threadGroupSize16, 1);
            MTLSize threadGroups = MTLSizeMake(((NSUInteger) width + threadGroupSize16 - 1) / threadGroupSize16,
                                               ((NSUInteger) height + threadGroupSize16 - 1) / threadGroupSize16,
                                               1);

            [encoder dispatchThreadgroups:threadGroups threadsPerThreadgroup:threadGroupSize];
            [encoder endEncoding];
            [commandBuffer commit];

            /*
             * 【一次性诊断】只在"首次生效 / 参数变了"时打（由调用方决定，绝不每帧打）。
             * 下一次真机核对就看这一行：它明确写出走的是哪条路（applying），
             * 以及输入/输出的格式、尺寸、usage、storageMode 与实际送进内核的三个值。
             */
            if (logDiagnostics) {
                AF_LOGI("colour adjust: applying via Metal compute kernel (cicadaColorAdjust) | "
                        "in %s(%d) %ux%u usage=%lu storage=%lu -> out %s(%d) %ux%u usage=%lu storage=%lu | "
                        "panel b=%.0f c=%.0f s=%.0f (0~200, 100 = neutral) -> kernel "
                        "brightness=%.3f contrast=%.3f saturation=%.3f luma=Rec709 | "
                        "path=one compute pass on the GPU into the 3 slot MTLTexture ring, "
                        "alpha forced to 1, no colour management, no CPU download\n",
                        cicadaPixelFormatName(input.pixelFormat), (int) input.pixelFormat,
                        (unsigned) input.width, (unsigned) input.height,
                        (unsigned long) input.usage, (unsigned long) input.storageMode,
                        cicadaPixelFormatName(slot.texture.pixelFormat), (int) slot.texture.pixelFormat,
                        (unsigned) slot.texture.width, (unsigned) slot.texture.height,
                        (unsigned long) slot.texture.usage, (unsigned long) slot.texture.storageMode,
                        (double) brightness, (double) contrast, (double) saturation,
                        (double) params.brightness, (double) params.contrast, (double) params.saturation);
            }

            outputNext = (outputNext + 1) % 3;
            /* 这张纹理是我们自己持有的（newTextureWithDescriptor），不受自动释放池影响。 */
            return slot.texture;
        }
    }

    CicadaTextureMetal::CicadaTextureMetal()
        : d(new Private())
    {
    }

    CicadaTextureMetal::~CicadaTextureMetal()
    {
        releaseResources();
        delete d;
        d = nullptr;
    }

    bool CicadaTextureMetal::prepare(QQuickWindow *window)
    {
        if (d->cache != nullptr) {
            return true;
        }

        if (window == nullptr) {
            return false;
        }

        auto *rif = window->rendererInterface();

        if (rif == nullptr || rif->graphicsApi() != QSGRendererInterface::Metal) {
            AF_LOGW("Qt scene graph is not using Metal (api=%d), "
                    "the Metal zero-copy path is unavailable\n",
                    rif != nullptr ? (int) rif->graphicsApi() : -1);
            m_failed = true;
            return false;
        }

        /*
         * 场景图的 Metal 设备。只能在渲染线程问（我们现在就在），
         * 而且必须是**这一个**设备：CVMetalTextureCache 用它建的纹理才能被
         * Qt 的场景图直接采样。
         */
        d->device = (__bridge id<MTLDevice>) rif->getResource(
                        window, QSGRendererInterface::DeviceResource);

        if (d->device == nil) {
            AF_LOGE("Qt gave no Metal device, the Metal zero-copy path is unavailable");
            m_failed = true;
            return false;
        }

        /*
         * 纹理缓存：iOSurface 支撑的 CVPixelBuffer 通过它包成 MTLTexture，
         * 不发生像素拷贝（同一个 IOSurface）。
         */
        const CVReturn err = CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr,
                             d->device, nullptr, &d->cache);

        if (err != kCVReturnSuccess || d->cache == nullptr) {
            AF_LOGE("CVMetalTextureCacheCreate failed (%d), falling back to the CPU path\n",
                    (int) err);
            m_failed = true;
            return false;
        }

        AF_LOGI("Metal zero-copy is ready: VideoToolbox CVPixelBuffers are wrapped into "
                "MTLTextures (IOSurface, no copy) and handed to the Qt scene graph\n");
        return true;
    }

    void CicadaTextureMetal::releaseInputState()
    {
        if (d == nullptr || d->cache == nullptr) {
            return;
        }

        /*
         * ============ 【这里绝不能再去 invalidate 任何东西】============
         *
         * 原来这里做两件事，都是**不安全**的（真机崩溃就是它）：
         *
         *   1) `CVMetalTextureCacheFlush(d->cache, 0)` —— 注释里当时以为"正在被采样的条目
         *      会被留着"。**不是**：这个 API 把缓存里的 CVMetalTexture 全部失效（Apple 头文件
         *      里 options 参数目前就是保留未用），于是我们环里那几张、以及 Qt 场景图此刻
         *      可能仍在采样的那些 CVMetalTexture，底层对象当场失效 ⇒ 再取 `.texture` 或
         *      GPU 采样它就会让 Metal 直接 abort()。崩溃栈正是
         *          AFActiveVideoRender::onVSync → (内联的渲染链) → abort()   （SIGABRT）
         *      而且时机吻合：本地视频播几秒后、以及每次切档落点之后（那条路径会调到本函数）。
         *
         *   2) 把帧环里除"最新一帧"之外的 CVMetalTexture 引用全放掉 —— 可"最新"是按**我们自己
         *      的写入顺序**算的，Qt 的场景图是异步的：它此刻可能还在采样更早的那一两帧，
         *      提前 CFRelease 同样是 use-after-free。
         *
         * 现在的处理：本函数**只做状态复位、不释放任何纹理**。内存是有界的 ——
         * 帧环固定 3 槽（新的覆盖最老的），缓存本身也只服务这几帧；真正的释放放在
         * releaseResources()（关播放器/析构那条路，那时 item 已经先
         * forgetOutputTextureWrapper() + invalidateFrameTextureCache() 把外层引用丢干净了）。
         *
         * 【色彩调整那条路也一样】计算管线 / 输出纹理环都是**我们自己**的资源，
         * 不跟解码代际走：本函数不碰它们（碰了就是同一类"抽掉正在被采样的纹理"的错），
         * 它们只在 releaseResources() 里放。
         */
        d->next = 0;
    }

    void CicadaTextureMetal::releaseResources()
    {
        if (d == nullptr) {
            return;
        }

        for (int i = 0; i < 3; ++i) {
            if (d->refs[i] != nullptr) {
                CFRelease(d->refs[i]);
                d->refs[i] = nullptr;
            }
        }

        d->next = 0;

        if (d->cache != nullptr) {
            /* 先把缓存里没用的条目清掉再释放，避免驱动侧残留。 */
            CVMetalTextureCacheFlush(d->cache, 0);
            CFRelease(d->cache);
            d->cache = nullptr;
        }

        /*
         * 色彩调整那一套（计算管线 / 输出纹理环）**只在这里放**：它们是这条路自己的资源，
         * 不跟着解码代际走 —— releaseInputState() 那边一根手指都不许碰（那条路刚因为
         * "提前释放正在被采样的纹理"崩过，见那个函数的说明）。
         *
         * 自己 new 出来的对象用 releaseOwnedObject 放（ARC/MRR 两种编译方式都对，
         * 理由见那个函数）；queue / device 是从 Qt 借的，不持有、也不放。
         */
        for (int i = 0; i < 3; ++i) {
            releaseOwnedObject(d->outputs[i].texture);
        }

        d->outputWidth = 0;
        d->outputHeight = 0;
        d->outputNext = 0;

        releaseOwnedObject(d->pipeline);
        d->pipelineReady = false;
        /* pipelineFailed 不复位：失败是"这台机器这条路走不通"的结论，不必每关一次播放器再试一遍。 */
        d->passFailureLogged = false;

        d->outputFormat = MTLPixelFormatInvalid;
        d->queue = nil;
        d->queueResolved = false;

        d->device = nil;
    }

    QSGTexture *CicadaTextureMetal::textureForFrame(QQuickWindow *window, IAFFrame *frame,
            bool *flipVertically)
    {
        if (flipVertically != nullptr) {
            /* Metal 的纹理原点在左上，Qt 的 Metal 后端也是，不需要翻转。 */
            *flipVertically = false;
        }

        if (m_failed || frame == nullptr || window == nullptr || d->cache == nullptr) {
            return nullptr;
        }

        /*
         * Apple 平台的解码帧是 PBAFFrame（见 framework/base/media/PBAFFrame.h）：
         * 里面就是 VideoToolbox 的 CVPixelBuffer。不是这种帧（软解、或者走了别的
         * 解码器）就交给 CPU 路径。
         */
        auto *pbFrame = dynamic_cast<PBAFFrame *>(frame);

        if (pbFrame == nullptr) {
            return nullptr;
        }

        CVPixelBufferRef pixelBuffer = pbFrame->getPixelBuffer();

        if (pixelBuffer == nullptr) {
            return nullptr;
        }

        const int width = static_cast<int>(CVPixelBufferGetWidth(pixelBuffer));
        const int height = static_cast<int>(CVPixelBufferGetHeight(pixelBuffer));
        const OSType pixelFormat = CVPixelBufferGetPixelFormatType(pixelBuffer);

        /*
         * 只处理 BGRA。
         *
         * 为什么必须让解码器直接输出 BGRA（见 CicadaPlayerItem 里设的
         * pixelBufferOutputFormat）：双平面 NV12 需要两张纹理 + 自定义着色器才能
         * 采样，Qt 的默认材质做不到；BGRA 是一张普通纹理，Qt 直接可用。
         *
         * 32ARGB / 32ABGR 这些也能映射到 Metal 格式，但字节序和 Qt 期望的 RGBA8
         * 不一致（颜色会错），所以这里只认 32BGRA。
         */
        MTLPixelFormat mtlFormat = MTLPixelFormatInvalid;

        switch (pixelFormat) {
            case kCVPixelFormatType_32BGRA:
                mtlFormat = MTLPixelFormatBGRA8Unorm;
                break;
            default:
                /*
                 * 不是 BGRA：说明 MediaPlayerConfig::pixelBufferOutputFormat 没生效
                 * （或者拿到了 10bit HDR 的 x420）。交给 CPU 路径，画面不会丢。
                 */
                if (!m_loggedFirstFrame) {
                    m_loggedFirstFrame = true;
                    AF_LOGW("the decoded CVPixelBuffer is not 32BGRA (format 0x%08x); "
                            "the Metal zero-copy path needs BGRA, using the CPU path\n",
                            (unsigned) pixelFormat);
                }

                return nullptr;
        }

        CVMetalTextureRef metalTexture = nullptr;

        /*
         * 把 CVPixelBuffer 的第 0 个平面包成 MTLTexture。
         *
         * BGRA 是单平面格式，planeIndex = 0；width/height 必须和平面一致。
         * 这一步是 IOSurface 直通，不拷贝像素。
         */
        const CVReturn err = CVMetalTextureCacheCreateTextureFromImage(
                                 kCFAllocatorDefault, d->cache, pixelBuffer, nullptr,
                                 mtlFormat, static_cast<size_t>(width),
                                 static_cast<size_t>(height), 0, &metalTexture);

        if (err != kCVReturnSuccess || metalTexture == nullptr) {
            AF_LOGW("CVMetalTextureCacheCreateTextureFromImage failed (%d)\n", (int) err);
            return nullptr;
        }

        id<MTLTexture> texture = CVMetalTextureGetTexture(metalTexture);

        if (texture == nil) {
            CFRelease(metalTexture);
            return nullptr;
        }

        /*
         * 换帧：写进环里的下一个槽，顺便放掉 3 帧之前那一份。
         *
         * 为什么至少要留几帧：Qt 的场景图是异步的，前几帧的命令缓冲可能还在执行，
         * 对应的 IOSurface 映射不能提前释放。环状保留既保证有余量，又不会无限增长。
         */
        if (d->refs[d->next] != nullptr) {
            CFRelease(d->refs[d->next]);
        }

        d->refs[d->next] = metalTexture;
        d->next = (d->next + 1) % 3;

        if (!m_loggedFirstFrame) {
            m_loggedFirstFrame = true;
            AF_LOGI("first zero-copy frame: %dx%d BGRA CVPixelBuffer -> MTLTexture "
                    "(IOSurface, no copy) -> Qt\n", width, height);
        }

        /*
         * 色彩调整（右键菜单 →「视频色彩调整」）。
         *
         * 三个值**全中性（100）时完全不进这条路**：仍旧把上面那张解码纹理直接交给 Qt，
         * 零额外 pass、零额外开销，画面与没有这个功能时**逐像素一致**。
         * 非中性时才在 GPU 上过一遍 Metal 计算内核（见 colorAdjustedTexture 的说明），
         * 拿到我们自己复用的那张输出纹理交给 Qt。失败就退回直通，画面不会黑、更不会落 CPU。
         */
        const float brightnessValue = m_brightness.load();
        const float contrastValue = m_contrast.load();
        const float saturationValue = m_saturation.load();
        const bool colorAdjustActive = (brightnessValue != 100.0f || contrastValue != 100.0f ||
                                        saturationValue != 100.0f);
        id<MTLTexture> textureForQt = texture;

        if (colorAdjustActive) {
            /*
             * 诊断日志的开关：**首次生效**，或者三个值**变了**（拖滑块每变一次一行）。
             * 绝不能每帧打 —— 值不变时这里一直是 false。
             */
            const bool logDiagnostics = !m_loggedColorAdjustActive ||
                                        brightnessValue != m_loggedBrightness ||
                                        contrastValue != m_loggedContrast ||
                                        saturationValue != m_loggedSaturation;
            id<MTLTexture> adjusted = d->colorAdjustedTexture(window, texture, width, height,
                                     brightnessValue, contrastValue, saturationValue, logDiagnostics);

            if (adjusted != nil) {
                textureForQt = adjusted;

                if (logDiagnostics) {
                    m_loggedColorAdjustActive = true;
                    m_loggedBrightness = brightnessValue;
                    m_loggedContrast = contrastValue;
                    m_loggedSaturation = saturationValue;
                    AF_LOGI("colour adjust: applying via Metal compute kernel for a %dx%d frame "
                            "(brightness=%.0f contrast=%.0f saturation=%.0f, 0~200, 100 = neutral); "
                            "one GPU pass, no CPU download — the per-pass details are in the "
                            "'colour adjust: applying via' line above\n",
                            width, height, (double) brightnessValue, (double) contrastValue,
                            (double) saturationValue);
                }
            } else if (!m_loggedColorAdjustFailure) {
                m_loggedColorAdjustFailure = true;
                AF_LOGW("colour adjust: disabled (the pass could not be prepared for this frame) — "
                        "the picture keeps the direct zero-copy path\n");
            }
        } else if (m_loggedColorAdjustActive && !m_loggedColorAdjustBypass) {
            m_loggedColorAdjustBypass = true;
            AF_LOGI("colour adjust: passthrough (all three values are neutral again; the decoder "
                    "texture goes straight to Qt, no extra pass)\n");
        }

        /*
         * 交给 Qt 场景图（具体的包装方式按 Qt 版本分两条路，见 CicadaQtTextureWrap）。
         *
         * 注意：CVMetalTextureRef 由我们自己持有（d->refs 那个 3 槽环），
         * 而这里返回的 QSGTexture 是 Qt 自己新建的包装对象，归调用方所有
         * （QSGSimpleTextureNode::setOwnsTexture(true) 会释放它）。
         */
        QSGTexture *qsTexture = wrapMetalTexture(window, (__bridge void *) textureForQt,
                                QSize(width, height));

        if (qsTexture == nullptr) {
            return nullptr;
        }

        return qsTexture;
    }

    void CicadaTextureMetal::setColorAdjust(float brightness, float contrast, float saturation)
    {
        /*
         * 与 CicadaTextureD3D11::setColorAdjust() 完全同量纲：0~200，100 = 中性，越界夹住。
         *
         * 本函数**从 GUI 线程调用**（用户拖滑块），而真正应用到 GPU 在渲染线程的
         * textureForFrame() 里，所以这里只写三个原子量（D3D11 那边是同一个写法）。
         * 三个值都中性时连计算内核都不会编译（管线是惰性建的）。
         */
        m_brightness.store(qBound(0.0f, brightness, 200.0f));
        m_contrast.store(qBound(0.0f, contrast, 200.0f));
        m_saturation.store(qBound(0.0f, saturation, 200.0f));
    }

}// namespace cicadaqt

#endif// Q_OS_MACOS
