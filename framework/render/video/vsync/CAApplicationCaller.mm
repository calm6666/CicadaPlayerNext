
#import <Foundation/Foundation.h>
#import "CAApplicationCaller.h"
#import <QuartzCore/QuartzCore.h>

@interface NSObject (CADisplayLink)
+ (id)displayLinkWithTarget:(id)arg1 selector:(SEL)arg2;

- (void)addToRunLoop:(id)arg1 forMode:(id)arg2;

- (void)setFrameInterval:(int)interval;

- (void)invalidate;
@end

@implementation CAApplicationCaller

@synthesize interval;

- (instancetype)init:(IVSync::Listener *)syncLisener
{
    if (self = [super init])
    {
        interval = 1;
        lisener = syncLisener;
    }
    return self;
}

- (void)startMainLoop {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (lisener) {
            lisener->VSyncOnInit();
        }
    });

    // CCDirector::setAnimationInterval() is called, we should invalidate it first
    [displayLink invalidate];
    displayLink = nil;
    displayLink = [NSClassFromString(@"CADisplayLink") displayLinkWithTarget:self selector:@selector(displayLinkCallback:)];
  //  [displayLink setFrameInterval:self.interval];
    [displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSDefaultRunLoopMode];
}

- (void) stop {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (lisener) {
            lisener->VSyncOnDestroy();
        }
    });
    // ARC 下不再手动 release; 但只把引用置空是不够的: run loop 还持有这个
    // display link, 它会继续以 self 为目标回调(dealloc 之后就是野指针),
    // 所以必须先 invalidate 摘掉注册, 再让 ARC 释放。
    [displayLink invalidate];
    displayLink = nil;
}

- (void)displayLinkCallback:(CADisplayLink *)sender {
    if (lisener) {
        lisener->onVSync([sender timestamp] + [sender duration]);
    }
}

@end
