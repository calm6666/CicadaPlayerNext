//
//  AppleAVPlayerLayerProcessor.h
//  CicadaPlayerSDK
//
//  Created by zhou on 2020/7/26.
//

#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import "native_cicada_player_def.h"

NS_ASSUME_NONNULL_BEGIN

@interface AppleAVPlayerLayerProcessor : NSObject

@property (nonatomic, strong) CALayer *parentLayer;
// playerLayer 会被 removePlayerLayer 置空(表示当前不挂任何 layer), 所以它是
// nullable; NS_ASSUME_NONNULL_BEGIN 下的默认 nonnull 会让置空产生 -Wnonnull。
@property (nonatomic, strong, nullable) AVPlayerLayer *playerLayer;

@property (nonatomic, assign) ScaleMode scaleMode;
@property (nonatomic, assign) MirrorMode mirrorMode;
@property (nonatomic, assign) RotateMode rotateMode;

- (void)setVideoSize:(CGSize)videoSize;

@end

NS_ASSUME_NONNULL_END
