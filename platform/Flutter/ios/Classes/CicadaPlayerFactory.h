//
//  VideoViewFactory.h
//  flutter_cicadaplayer
//
//  Created by aliyun on 2020/10/9.
//
#import <CicadaPlayerSDK/CicadaPlayerSDK.h>
#import <Flutter/Flutter.h>
#import <Foundation/Foundation.h>

#import "FlutterCicadaPlayerTexture.h"

NS_ASSUME_NONNULL_BEGIN

/**
 * 工厂兼播放器宿主。
 *
 * 除 FlutterPlatformViewFactory（历史遗留，老 Dart 代码用 UiKitView 时会走到）之外，本类还是
 * 播放器的 CicadaDelegate（事件回 Dart）与 **CicadaRenderDelegate**（拿解码后的 CVPixelBuffer）。
 * 后者就是 iOS 零拷贝的入口：`onVideoPixelBuffer:pts:` 里直接把 buffer 交给
 * FlutterCicadaPlayerTexture，再由它按 FlutterTexture 契约转交给引擎。
 */
@interface CicadaPlayerFactory : NSObject <FlutterPlatformViewFactory, CicadaDelegate, CicadaRenderDelegate, CicadaAudioSessionDelegate, FlutterStreamHandler>

@property(nonatomic, strong, nullable) CicadaPlayer *cicadaPlayer;

/** 当前注册在 Flutter 纹理表里的那张视频纹理；没有纹理时为 nil。 */
@property(nonatomic, strong, nullable) FlutterCicadaPlayerTexture *videoTexture;

- (instancetype)initWithMessenger:(NSObject<FlutterBinaryMessenger> *)messenger
                  textureRegistry:(NSObject<FlutterTextureRegistry> *)textureRegistry;

/** 建纹理并把播放器的渲染回调接到它上面，返回纹理 id。 */
- (int64_t)registerZeroCopyTexture;

/** 注销纹理并把播放器的渲染回调摘掉。 */
- (void)unregisterZeroCopyTexture;

@end

NS_ASSUME_NONNULL_END
