//
// macOS 零拷贝后端的实现，设计说明见 CicadaTextureMetal.h。
//

#include "CicadaTextureMetal.h"
#include "CicadaQtTextureWrap.h"

#if defined(Q_OS_MACOS)

#include <CoreVideo/CoreVideo.h>
#include <CoreVideo/CVMetalTextureCache.h>
#include <Metal/Metal.h>
/* 色彩调整：CIContext / CIFilter(CIColorControls) / CIImage。 */
#import <CoreImage/CoreImage.h>

#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>
#include <QtCore/QSize>

#include <cmath>

#include <utils/frame_work_log.h>
#include "base/media/PBAFFrame.h"

namespace cicadaqt {

    /* CIColorControls 的三个输入名：setValue:forKey: 与 attributes 用的是同一批名字。 */
    static NSString *const kCicadaColorBrightness = @"inputBrightness";
    static NSString *const kCicadaColorContrast = @"inputContrast";
    static NSString *const kCicadaColorSaturation = @"inputSaturation";

    /*
     * 从 attributes 里读一对 (min, max)，并校验它是不是一个能拿来做线性映射的范围：
     * 必须是有限值、max > min，而且不是那种"无边界"的写法（Core Image 里有些滤镜把
     * Min/Max 写成 ±FLT_MAX，那种范围做线性映射没有意义 —— 滑块动一点点参数就飞了）。
     */
    static bool readColorRangePair(NSDictionary *attributes, NSString *minKey, NSString *maxKey,
                                   float *minValue, float *maxValue)
    {
        NSNumber *minNumber = attributes[minKey];
        NSNumber *maxNumber = attributes[maxKey];

        if (minNumber == nil || maxNumber == nil) {
            return false;
        }

        const float minRead = minNumber.floatValue;
        const float maxRead = maxNumber.floatValue;

        if (!std::isfinite(minRead) || !std::isfinite(maxRead) || maxRead <= minRead) {
            return false;
        }

        if (std::fabs(minRead) > 10000.0f || std::fabs(maxRead) > 10000.0f) {
            return false;
        }

        *minValue = minRead;
        *maxValue = maxRead;
        return true;
    }

    /*
     * 读 CIColorControls 某个输入参数的 (min, default, max)。
     *
     * 【为什么不写死数字】D3D11 那边是问驱动要范围
     * （ID3D11VideoProcessorEnumerator::GetVideoProcessorFilterRange 给出
     * Minimum / Maximum / Default），Core Image 把同样的信息放在 filter.attributes 里。
     * 这里照同一个思路：优先用完整范围 kCIAttributeMin/Max（语义和 D3D11 的
     * Minimum/Maximum 一一对应），没有再退回 kCIAttributeSliderMin/Max（Apple 建议的滑块范围）。
     * 两个都读不到就返回 false —— **绝不自己发明一个范围**；那时整条色彩调整不生效（有日志），
     * 而不是给用户一个猜出来的、和 Windows 对不上的效果。
     */
    static bool readColorAdjustRange(CIFilter *filter, NSString *inputKey, float *minValue,
                                     float *defaultValue, float *maxValue)
    {
        NSDictionary *attributes = [filter attributes][inputKey];

        if (attributes == nil) {
            return false;
        }

        NSNumber *defaultNumber = attributes[kCIAttributeDefault];

        if (defaultNumber == nil) {
            defaultNumber = attributes[kCIAttributeIdentity];
        }

        if (defaultNumber == nil || !std::isfinite(defaultNumber.floatValue)) {
            return false;
        }

        if (!readColorRangePair(attributes, kCIAttributeMin, kCIAttributeMax, minValue, maxValue) &&
                !readColorRangePair(attributes, kCIAttributeSliderMin, kCIAttributeSliderMax,
                                    minValue, maxValue)) {
            return false;
        }

        *defaultValue = defaultNumber.floatValue;
        return true;
    }

    /*
     * 面板给的 0~200（100 = 中性）→ Core Image 参数值。
     *
     * 用的是和 CicadaTextureD3D11::applyColorAdjust() **同一个公式**：
     *     t = (value - 100) / 100            （-1 ~ +1）
     *     t >= 0： default + (max - default) * t
     *     t <  0： default - (default - min) * (-t)
     * 即"从中性值出发按支持范围线性插值"，两端正好落在 max / min 上；方向与 Windows 一致：
     * 亮度变大更亮、对比度变大对比更强、饱和度变大更艳（降到 0 就是灰度）。
     */
    static float mapColorAdjustValue(float value, float minValue, float defaultValue, float maxValue)
    {
        const float t = (value - 100.0f) / 100.0f;

        if (t >= 0.0f) {
            return defaultValue + (maxValue - defaultValue) * t;
        }

        return defaultValue - (defaultValue - minValue) * (-t);
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
        CIContext *ciContext = nil;
        /*
         * 复用的 CIColorControls 实例。CIFilter 不是线程安全的，所以只允许在渲染线程用；
         * 每帧只改它的三个输入值和输入图，绝不重新 filterWithName:（那会每帧新建对象）。
         */
        CIFilter *ciColorControls = nil;
        /*
         * 工作色彩空间（我们自己持有，releaseResources 里放）。为什么是线性 RGB、
         * 为什么输入输出都用它，见 ensureColorAdjustContext() 的长注释。
         */
        CGColorSpaceRef workingColorSpace = nullptr;
        /* 从 CIColorControls 的 attributes 里读出来的 (min, default, max)。 */
        bool rangesReady = false;
        float brightnessMin = 0.0f;
        float brightnessDefault = 0.0f;
        float brightnessMax = 0.0f;
        float contrastMin = 0.0f;
        float contrastDefault = 1.0f;
        float contrastMax = 0.0f;
        float saturationMin = 0.0f;
        float saturationDefault = 1.0f;
        float saturationMax = 0.0f;
        /*
         * 输出纹理环（3 槽）：和 refs[3] 一个思路 —— Qt 的场景图是异步的，上一两帧的命令
         * 缓冲可能还在采样这张纹理，所以不能只用一张。有界（最多 3 张），尺寸变了整体重建。
         */
        struct OutputSlot {
            id<MTLTexture> texture = nil;
        };
        OutputSlot outputs[3];
        int outputWidth = 0;
        int outputHeight = 0;
        int outputNext = 0;
        /*
         * 场景图的 Metal 命令队列（从 getResource 借的，我们不持有、也不 release）。
         * 拿到它就能把"我们这次 pass"排在 Qt 那一帧的采样之前，见 colorAdjustedTexture()。
         */
        id<MTLCommandQueue> queue = nil;
        bool queueResolved = false;

        /* 下面三个只在渲染线程调用。 */
        bool ensureColorAdjustContext();
        bool ensureOutputTextures(int width, int height);
        id<MTLTexture> colorAdjustedTexture(QQuickWindow *window, id<MTLTexture> input, int width,
                                            int height, float brightness, float contrast,
                                            float saturation);
    };

    /*
     * 建 Core Image 的上下文（一次性，惰性）。
     *
     * 【色彩空间：为什么这么设，为什么输入和输出用同一个】
     *
     * 直通那条路（没调色彩时）是把 VideoToolbox 解出来的 BGRA 纹理**原值**交给 Qt 采样，
     * 中间没有任何色彩转换。色彩调整这条路必须保持同样的口径，否则就是"一拖滑块颜色就偏"：
     *
     *   * 输入：CIImage 用 kCIImageColorSpace = [NSNull null] 创建。Apple 的文档写得明确
     *     （Core Image 的 CIImageOption.colorSpace 条目）："To request that Core Image
     *     perform no color management, specify the NSNull object as the value for this key."
     *     于是纹理里的像素值被原样当成"已经在工作色彩空间里"，滤镜直接在这个值上运算；
     *   * 工作色彩空间：线性 RGB（kCGColorSpaceGenericRGBLinear）。Core Image 的滤镜内核
     *     就是按线性光设计的，工作空间必须是线性的；
     *   * 输出：render(...) 的 colorSpace 参数传**同一个**色彩空间。工作空间 → 目标空间
     *     是恒等变换，所以除滤镜那一步运算之外，颜色值不会被任何 transfer function 动过。
     *
     * 反过来做（例如输入声明成 sRGB、输出按线性写回）就是一次真实的 gamma 转换 ——
     * 那正是"颜色偏了"的来源，所以这里刻意两边都用同一个空间。
     *
     * 【为什么不用 kCIContextCacheIntermediates 之类的选项】本工程 macOS 的部署目标是 10.11
     * （见 framework/macOSX.cmake 的 MACOSX_DEPLOYMENT_TARGET），那些 10.12+ 才有的 option key
     * 在 10.11 上拿到的是空符号，不能碰。这里只用 10.4/10.11 起就有的 API。
     *
     * 【内存】CIContext 自带中间结果缓存，但它是有界的 LRU，而且我们全程复用同一个 context
     * （Apple 对视频处理的推荐做法），不会随帧数增长。
     */
    bool CicadaTextureMetal::Private::ensureColorAdjustContext()
    {
        if (ciContext != nil) {
            return true;
        }

        if (device == nil) {
            return false;
        }

        /*
         * 这段会创建自释放对象（options / attributes 字典），所以自己开一个自动释放池：
         * 调用方是 Qt 的渲染线程，不能指望它头上有池。ciContext / ciColorControls 是我们
         * 显式 alloc/retain 持有的，不受池影响。
         */
        @autoreleasepool {
            workingColorSpace = CGColorSpaceCreateWithName(kCGColorSpaceGenericRGBLinear);

            if (workingColorSpace == nullptr) {
                AF_LOGE("cannot create the linear RGB colour space, video colour adjust is off\n");
                return false;
            }

            NSDictionary *options = @{ kCIContextWorkingColorSpace: (__bridge id) workingColorSpace };
            ciContext = [[CIContext alloc] initWithMTLDevice:device options:options];

            if (ciContext == nil) {
                AF_LOGE("cannot create the Metal backed CIContext, video colour adjust is off\n");
                CGColorSpaceRelease(workingColorSpace);
                workingColorSpace = nullptr;
                return false;
            }

            ciColorControls = [[CIFilter filterWithName:@"CIColorControls"] retain];

            if (ciColorControls == nil) {
                AF_LOGE("Core Image has no CIColorControls filter, video colour adjust is off\n");
                [ciContext release];
                ciContext = nil;
                CGColorSpaceRelease(workingColorSpace);
                workingColorSpace = nullptr;
                return false;
            }

            [ciColorControls setDefaults];

            rangesReady = readColorAdjustRange(ciColorControls, kCicadaColorBrightness,
                                               &brightnessMin, &brightnessDefault, &brightnessMax) &&
                          readColorAdjustRange(ciColorControls, kCicadaColorContrast,
                                               &contrastMin, &contrastDefault, &contrastMax) &&
                          readColorAdjustRange(ciColorControls, kCicadaColorSaturation,
                                               &saturationMin, &saturationDefault, &saturationMax);

            if (!rangesReady) {
                /* 读不到范围就不做映射（宁可如实不生效，也不给一个猜出来的效果）。 */
                AF_LOGE("CIColorControls does not report its parameter ranges, "
                        "video colour adjust is off (the picture keeps the direct path)\n");
                [ciColorControls release];
                ciColorControls = nil;
                [ciContext release];
                ciContext = nil;
                CGColorSpaceRelease(workingColorSpace);
                workingColorSpace = nullptr;
                return false;
            }

            AF_LOGI("video colour adjust is ready: CIColorControls on the GPU, ranges "
                    "brightness [%.2f..%.2f] default %.2f, contrast [%.2f..%.2f] default %.2f, "
                    "saturation [%.2f..%.2f] default %.2f (read from the filter attributes)\n",
                    (double) brightnessMin, (double) brightnessMax, (double) brightnessDefault,
                    (double) contrastMin, (double) contrastMax, (double) contrastDefault,
                    (double) saturationMin, (double) saturationMax, (double) saturationDefault);
        }

        return true;
    }

    /*
     * 输出纹理环：惰性建、按尺寸复用（尺寸变了整体重建，和有界的 refs 环一个思路）。
     * Core Image 把滤镜结果渲染进这里，再由 Qt 采样 —— 全程在 GPU 上，不读回 CPU。
     */
    bool CicadaTextureMetal::Private::ensureOutputTextures(int width, int height)
    {
        if (device == nil || width <= 0 || height <= 0) {
            return false;
        }

        if (outputs[0].texture != nil && outputWidth == width && outputHeight == height) {
            return true;
        }

        for (int i = 0; i < 3; ++i) {
            if (outputs[i].texture != nil) {
                [outputs[i].texture release];
                outputs[i].texture = nil;
            }
        }

        outputWidth = 0;
        outputHeight = 0;

        /*
         * 尺寸/格式和**解码纹理**一致（BGRA8；就是显示尺寸，Core Image 这里只做颜色运算、
         * 不缩放），所以交给 Qt 的纹理与直通那条路同尺寸，后面的缩放 / letterbox 逻辑
         * 一个字都不用改。
         *
         * usage 必须带 RenderTarget：Core Image 是往它里面**渲染**；
         * 也必须带 ShaderRead：Qt 的场景图要采样它。
         * storageMode 用 Private：这张纹理只在 GPU 上产生、只在 GPU 上消费，永远不读回 CPU
         *（这正是零拷贝那条硬约束）。
         */
        MTLTextureDescriptor *descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                              width:(NSUInteger) width
                                                             height:(NSUInteger) height
                                                          mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        descriptor.storageMode = MTLStorageModePrivate;

        for (int i = 0; i < 3; ++i) {
            outputs[i].texture = [device newTextureWithDescriptor:descriptor];

            if (outputs[i].texture == nil) {
                for (int j = 0; j < 3; ++j) {
                    if (outputs[j].texture != nil) {
                        [outputs[j].texture release];
                        outputs[j].texture = nil;
                    }
                }

                AF_LOGE("cannot allocate the colour adjust output texture (%dx%d)\n", width, height);
                return false;
            }
        }

        outputWidth = width;
        outputHeight = height;
        outputNext = 0;
        return true;
    }

    /*
     * 把一帧从解码纹理过一遍 CIColorControls，写进环里的输出纹理，返回那张输出纹理。
     *
     * 【零 CPU 下载】输入是 VideoToolbox 的 MTLTexture（IOSurface 直通），输出是我们自己的
     * MTLTexture，中间只有一个 Core Image 的 GPU pass：没有 CVPixelBufferGetBaseAddress、
     * 没有 QImage、没有 swscale，退出这条路也不会碰到 textureForFrameCpu。
     *
     * 【顺序：为什么能从 Qt 借命令队列】Qt 的场景图是异步的，我们这次写必须排在"Qt 采样那张
     * 输出纹理"之前：
     *   * 命令缓冲从**场景图自己的队列**新建（QSGRendererInterface::CommandQueueResource，
     *     Qt 文档明确 Metal 下它就是 MTLCommandQueue *）。同一个队列上的命令缓冲按**提交顺序**
     *     开始执行，而 Qt 那一帧的命令缓冲是在 updatePaintNode 返回之后才提交的，所以
     *     "我们写 → Qt 采样"这个顺序天然成立（Apple 对 render:toMTLTexture:commandBuffer:
     *     的说明也是这么建议的：要和别的 Metal 渲染配合就把**同一个**命令缓冲传进去）；
     *   * 借不到队列（老版本 / 别的后端）时传 nil：Core Image 自己建并提交一个，仍是 GPU-only，
     *     只是少了这层顺序保证，所以打一条 W 日志说明（只打一次）。
     */
    id<MTLTexture> CicadaTextureMetal::Private::colorAdjustedTexture(QQuickWindow *window,
            id<MTLTexture> input, int width, int height, float brightness, float contrast,
            float saturation)
    {
        /*
         * 每帧都会创建几个自释放对象（CIImage、命令缓冲、参数 NSNumber、字典……），
         * 而调用方是 Qt 的渲染线程 —— 不能指望外面每帧都有自动释放池，所以自己开一个。
         * 不开的话拖滑块时内存会一直涨（"连续拖 30 秒内存不增长"就是这条判据）。
         */
        @autoreleasepool {
            if (window == nullptr || !ensureColorAdjustContext() || !rangesReady) {
                return nil;
            }

            if (!queueResolved) {
                queueResolved = true;
                auto *rif = window->rendererInterface();
                queue = (rif != nullptr)
                        ? (__bridge id<MTLCommandQueue>) rif->getResource(
                              window, QSGRendererInterface::CommandQueueResource)
                        : nil;

                if (queue == nil) {
                    AF_LOGW("the scene graph gave no Metal command queue, the colour adjust pass will "
                            "use Core Image's own command buffer (still GPU only, no CPU download)\n");
                }
            }

            if (!ensureOutputTextures(width, height)) {
                return nil;
            }

            /*
             * 输入图：不做色彩管理（kCIImageColorSpace = NSNull，见
             * ensureColorAdjustContext 的长注释），像素值原样进滤镜。
             *
             * 方向：imageWithMTLTexture: 与 render:toMTLTexture: 是 Apple 文档里**成对**给出的
             * 用法（输入纹理 → 滤镜 → 输出纹理，见 imageWithMTLTexture 的 Discussion），
             * 两个方向用的是同一套纹理坐标约定，所以输出纹理与输入图同向 ——
             * 不需要翻转，textureForFrame 里那个 flipVertically 保持 false 就行。
             * 真机上万一画面上下颠倒，就在这里给 CIImage 加一次垂直翻转。
             *
             * 说明：Core Image 把纹理内容按**预乘 alpha** 处理。VideoToolbox 的 32BGRA 输出是
             * 不透明的（alpha = 1），预乘与不预乘等价，所以这里不用额外处理。真机上万一
             * 拖了滑块画面变全黑，那说明这条假设不成立（alpha 不是 1），要在进滤镜前把 alpha
             * 置成 1。
             */
            CIImage *image = [CIImage imageWithMTLTexture:input
                                                  options:@{ kCIImageColorSpace: [NSNull null] }];

            if (image == nil) {
                return nil;
            }

            [ciColorControls setValue:image forKey:kCIInputImageKey];
            [ciColorControls setValue:@(mapColorAdjustValue(brightness, brightnessMin,
                                      brightnessDefault, brightnessMax))
                               forKey:kCicadaColorBrightness];
            [ciColorControls setValue:@(mapColorAdjustValue(contrast, contrastMin,
                                      contrastDefault, contrastMax))
                               forKey:kCicadaColorContrast];
            [ciColorControls setValue:@(mapColorAdjustValue(saturation, saturationMin,
                                      saturationDefault, saturationMax))
                               forKey:kCicadaColorSaturation];

            CIImage *filtered = ciColorControls.outputImage;

            if (filtered == nil) {
                return nil;
            }

            OutputSlot &slot = outputs[outputNext];
            id<MTLCommandBuffer> commandBuffer = (queue != nil) ? [queue commandBuffer] : nil;

            /*
             * bounds 用图像自己的尺寸（输出纹理就是这么大），colorSpace 用工作色彩空间
             * （恒等变换，见 ensureColorAdjustContext 的说明）。
             */
            [ciContext render:filtered
                  toMTLTexture:slot.texture
                 commandBuffer:commandBuffer
                        bounds:CGRectMake(0.0, 0.0, (CGFloat) width, (CGFloat) height)
                    colorSpace:workingColorSpace];

            if (commandBuffer != nil) {
                [commandBuffer commit];
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
         * 【色彩调整那条路也一样】Core Image 上下文 / 输出纹理环都是**我们自己**的资源，
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
         * 色彩调整那一套（Core Image 上下文 / 复用滤镜 / 输出纹理环 / 工作色彩空间）
         * **只在这里放**：它们是这条路自己的资源，不跟着解码代际走 ——
         * releaseInputState() 那边一根手指都不许碰（那条路刚因为"提前释放正在被采样的
         * 纹理"崩过，见那个函数的说明）。
         *
         * 本工程的 .mm 没有开 ARC（对照 platform/Apple/source/CMakeLists.txt 里的
         * -fobjc-arc），所以自己 alloc/retain/new 出来的对象要显式 release；
         * queue / device 是从 Qt 借的，不持有、不 release。
         */
        for (int i = 0; i < 3; ++i) {
            if (d->outputs[i].texture != nil) {
                [d->outputs[i].texture release];
                d->outputs[i].texture = nil;
            }
        }

        d->outputWidth = 0;
        d->outputHeight = 0;
        d->outputNext = 0;

        if (d->ciColorControls != nil) {
            [d->ciColorControls release];
            d->ciColorControls = nil;
        }

        if (d->ciContext != nil) {
            [d->ciContext release];
            d->ciContext = nil;
        }

        if (d->workingColorSpace != nullptr) {
            CGColorSpaceRelease(d->workingColorSpace);
            d->workingColorSpace = nullptr;
        }

        d->rangesReady = false;
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
         * 非中性时才在 GPU 上过一遍 CIColorControls（见 colorAdjustedTexture 的说明），
         * 拿到我们自己复用的那张输出纹理交给 Qt。失败就退回直通，画面不会黑、更不会落 CPU。
         */
        const float brightnessValue = m_brightness.load();
        const float contrastValue = m_contrast.load();
        const float saturationValue = m_saturation.load();
        const bool colorAdjustActive = (brightnessValue != 100.0f || contrastValue != 100.0f ||
                                        saturationValue != 100.0f);
        id<MTLTexture> textureForQt = texture;

        if (colorAdjustActive) {
            id<MTLTexture> adjusted = d->colorAdjustedTexture(window, texture, width, height,
                                     brightnessValue, contrastValue, saturationValue);

            if (adjusted != nil) {
                textureForQt = adjusted;

                if (!m_loggedColorAdjustActive) {
                    m_loggedColorAdjustActive = true;
                    AF_LOGI("video colour adjust is active: brightness=%.0f contrast=%.0f saturation=%.0f "
                            "(0~200, 100 = neutral), applied on the GPU with CIColorControls into a "
                            "reused %dx%d MTLTexture (one extra pass, no CPU download)\n",
                            (double) brightnessValue, (double) contrastValue,
                            (double) saturationValue, width, height);
                }
            } else if (!m_loggedColorAdjustFailure) {
                m_loggedColorAdjustFailure = true;
                AF_LOGW("video colour adjust could not be applied on this frame; the picture keeps "
                        "the direct zero-copy path\n");
            }
        } else if (m_loggedColorAdjustActive && !m_loggedColorAdjustBypass) {
            m_loggedColorAdjustBypass = true;
            AF_LOGI("video colour adjust is back to neutral: the direct zero-copy path is used again "
                    "(no extra pass)\n");
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
         * 没走额外 pass 的时候（三个值中性）连 Core Image 上下文都不会建。
         */
        m_brightness.store(qBound(0.0f, brightness, 200.0f));
        m_contrast.store(qBound(0.0f, contrast, 200.0f));
        m_saturation.store(qBound(0.0f, saturation, 200.0f));
    }

}// namespace cicadaqt

#endif// Q_OS_MACOS
