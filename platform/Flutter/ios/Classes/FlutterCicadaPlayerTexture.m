//
//  FlutterCicadaPlayerTexture.m
//  flutter_cicadaplayer
//

#import "FlutterCicadaPlayerTexture.h"

#import <CoreVideo/CoreVideo.h>

#import <CicadaPlayerSDK/CicadaPlayerSDK.h>

@implementation FlutterCicadaPlayerTexture {
    /**
     * 保护 mPixelBuffer / mUnregistered。
     *
     * 为什么要锁：写入在解码线程（CicadaRenderCBWrapper::OnRenderFrame → onVideoPixelBuffer），
     * 读取在 Flutter 的 raster 线程（copyPixelBuffer），注销又在平台线程（disposeTexture）。
     * 三边并发，指针和引用计数都必须串起来，否则就是 use-after-free。
     */
    NSLock *mLock;
    CVPixelBufferRef mPixelBuffer;
    BOOL mUnregistered;

    /**
     * "这一帧不是 Flutter 能吃的格式"这类诊断只打一次。
     * 为什么只打一次：不支持是**这台设备这条路**的结论，每帧打会把日志刷爆。
     */
    BOOL mLoggedUnsupportedFormat;
    /** 第一条成功交出去的帧只打一条日志，用来确认真机上走的确实是 IOSurface 直通。 */
    BOOL mLoggedFirstFrame;
}

- (instancetype)initWithTextureRegistry:(NSObject<FlutterTextureRegistry> *)registry
                              textureId:(int64_t)textureId
{
    self = [super init];
    if (self) {
        _registry = registry;
        _textureId = textureId;
        mLock = [[NSLock alloc] init];
        mPixelBuffer = NULL;
        mUnregistered = NO;
    }
    return self;
}

- (void)dealloc
{
    /*
     * 正常路径上 unregister 已经把手上的帧放掉了；这里的释放是兜底
     *（比如 Dart 侧没调 disposeTexture 就退出，我们自己被释放）。
     */
    if (mPixelBuffer != NULL) {
        CVPixelBufferRelease(mPixelBuffer);
        mPixelBuffer = NULL;
    }
}

#pragma mark - 帧入口

- (void)onVideoPixelBuffer:(CVPixelBufferRef)pixelBuffer
{
    if (pixelBuffer == NULL) {
        return;
    }

    [mLock lock];

    if (mUnregistered) {
        [mLock unlock];
        return;
    }

    /*
     * 换帧：先 retain 新的，再放旧的。
     *
     * 为什么必须先 retain 新的：同一帧被连续上报（或者池子复用）时，新旧的底层对象可能
     * 是同一个，先 release 会把它放没了。
     */
    CVPixelBufferRetain(pixelBuffer);

    if (mPixelBuffer != NULL) {
        CVPixelBufferRelease(mPixelBuffer);
    }

    mPixelBuffer = pixelBuffer;

    [mLock unlock];

    /*
     * 通知 Flutter：下一帧合成时会来取。textureFrameAvailable 内部会把这件事排到 raster
     * 线程上，这里（解码线程）调用是官方允许的用法。
     */
    [_registry textureFrameAvailable:_textureId];
}

#pragma mark - FlutterTexture

/**
 * Flutter 在 raster 线程调用，取"当前这一帧"。
 *
 * 引用计数规则（FlutterTexture 的约定，写错就是泄漏或崩溃）：
 *   * 返回出去的那一份引用**归 Flutter**，它用完会自己 CVPixelBufferRelease；
 *   * 所以这里必须先 CVPixelBufferRetain 再返回，retain 出来的那一份就是交出去的那份；
 *   * 本类自己持有的那一份（mPixelBuffer）不动，留给下一帧覆盖或 unregister 时释放。
 * 换句话说：**不能**直接 return mPixelBuffer，那样 Flutter 一释放就把我们的引用也放掉了。
 */
- (CVPixelBufferRef _Nullable)copyPixelBuffer
{
    [mLock lock];

    if (mPixelBuffer == NULL) {
        [mLock unlock];
        return NULL;
    }

    CVPixelBufferRef frame = mPixelBuffer;

    if (!mLoggedUnsupportedFormat) {
        OSType format = CVPixelBufferGetPixelFormatType(frame);

        if (![self isPixelFormatAcceptedByFlutter:format]) {
            mLoggedUnsupportedFormat = YES;
            NSLog(@"[flutter_cicadaplayer] decoded CVPixelBuffer format 0x%08x is not one of the "
                   "formats Flutter's texture registry renders (32BGRA / 420YpCbCr8BiPlanar "
                   "video-or-full-range); the video texture will stay black on this device. "
                   "No CPU conversion is done on purpose - converting here would silently turn "
                   "the zero-copy path into a copy.",
                  (unsigned int) format);
        }
    }

    if (!mLoggedFirstFrame) {
        mLoggedFirstFrame = YES;
        NSLog(@"[flutter_cicadaplayer] first texture frame: %zux%zu CVPixelBuffer (format 0x%08x, "
               "IOSurface %@) handed to Flutter by retain, no CPU copy",
              CVPixelBufferGetWidth(frame), CVPixelBufferGetHeight(frame),
              (unsigned int) CVPixelBufferGetPixelFormatType(frame),
              CVPixelBufferGetIOSurface(frame) != NULL ? @"present" : @"absent");
    }

    CVPixelBufferRetain(frame);

    [mLock unlock];

    return frame;
}

- (CGSize)latestPixelBufferSize
{
    [mLock lock];
    CGSize size = CGSizeZero;

    if (mPixelBuffer != NULL) {
        size = CGSizeMake((CGFloat) CVPixelBufferGetWidth(mPixelBuffer),
                          (CGFloat) CVPixelBufferGetHeight(mPixelBuffer));
    }

    [mLock unlock];
    return size;
}

#pragma mark - 注销

- (void)unregister
{
    [mLock lock];

    if (mUnregistered) {
        [mLock unlock];
        return;
    }

    mUnregistered = YES;

    if (mPixelBuffer != NULL) {
        CVPixelBufferRelease(mPixelBuffer);
        mPixelBuffer = NULL;
    }

    [mLock unlock];
}

#pragma mark - 私有

/**
 * Flutter 的纹理表能直接吃的 CVPixelBuffer 格式。
 *
 * 为什么只认这几个：引擎的 ios_external_texture 两条后端（GL 的
 * CVOpenGLESTextureCacheCreateTextureFromImage、Metal 的 CVMetalTextureCache）都只对
 * 有限几种格式建得出纹理，别的格式传进去是**拿不到纹理**的（画面全黑），而不是自动转换。
 * 既然是黑屏，那就必须在日志里说清楚，而不是偷偷在插件里转一道（那正是本任务禁止的 CPU 拷贝）。
 */
- (BOOL)isPixelFormatAcceptedByFlutter:(OSType)format
{
    switch (format) {
        case kCVPixelFormatType_32BGRA:
        case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
        case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
            return YES;

        default:
            return NO;
    }
}

@end
