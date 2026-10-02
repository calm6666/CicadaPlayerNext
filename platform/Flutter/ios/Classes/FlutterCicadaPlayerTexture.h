//
//  FlutterCicadaPlayerTexture.h
//  flutter_cicadaplayer
//
//  iOS 侧的视频零拷贝出口。
//

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Flutter/Flutter.h>

NS_ASSUME_NONNULL_BEGIN

/**
 * 注册给 Flutter 纹理表的一张视频纹理。
 *
 * 为什么这条路是零拷贝：解码器（framework/codec/Apple 的 AFVTBDecoder）硬解之后交出来的
 * 就是一个 **CVPixelBuffer**，底层是 IOSurface。Flutter 的 FlutterTexture 契约要求
 * copyPixelBuffer 返回一个 CVPixelBufferRef，引擎拿到之后会按 IOSurface 直接建纹理采样
 * （shell/platform/darwin/ios 的 ios_external_texture_gl / _metal），中间没有
 * CVPixelBufferGetBaseAddress、没有 NSData、没有 UIImage。所以本类要做的只有一件事：
 * 把解码器那一帧原样转交给 Flutter，并且把引用计数规则守对。
 *
 * 引用计数（Flutter 的约定，必须严格照做）：copyPixelBuffer 返回的 buffer 由**调用方**
 * 负责释放，因此本类在返回前必须 CVPixelBufferRetain，交出去的那一份引用就归 Flutter 了。
 * 本类自己持有"最新一帧"的那份引用，在下一帧到达或 unregister 时释放。
 */
@interface FlutterCicadaPlayerTexture : NSObject <FlutterTexture>

/**
 * 由纹理表返回的 id，Dart 侧就是拿它去 `Texture(textureId:)`。
 *
 * 可写是因为注册顺序：必须先有对象才能 registerTexture:，而 id 是 registerTexture: 的
 * 返回值，所以只能建好之后再回填。
 */
@property(nonatomic, assign) int64_t textureId;

/**
 * 纹理表。只读属性（assign）而不是 strong：Flutter 那边持有本对象，
 * 本对象反过来强引用纹理表就成环了。
 */
@property(nonatomic, assign, readonly) NSObject<FlutterTextureRegistry> *registry;

/** 纹理表已经注销过本纹理，之后不再 textureFrameAvailable。 */
@property(nonatomic, assign, readonly) BOOL unregistered;

- (instancetype)initWithTextureRegistry:(NSObject<FlutterTextureRegistry> *)registry
                                textureId:(int64_t)textureId;

/**
 * 解码线程（CicadaRenderCBWrapper::OnRenderFrame）每帧调一次。
 *
 * 调用方传进来的 pixelBuffer 的生命周期只在这个函数里有效，本类需要时就自己 retain，
 * 不假定它一直活着。
 */
- (void)onVideoPixelBuffer:(CVPixelBufferRef)pixelBuffer;

/**
 * 当前手上那一帧的尺寸（没有帧时是 CGSizeZero）。只用来做诊断和尺寸上报。
 */
- (CGSize)latestPixelBufferSize;

/** 注销：把本类手上的帧引用放掉并标记不再上报。 */
- (void)unregister;

@end

NS_ASSUME_NONNULL_END
