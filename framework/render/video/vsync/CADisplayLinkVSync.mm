//
// Created by moqi on 2019-09-05.
//
#import <Foundation/Foundation.h>
//#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#include "CADisplayLinkVSync.h"
#import "CAApplicationCaller.h"

CADisplayLinkVSync::CADisplayLinkVSync(IVSync::Listener &listener) : IVSync(listener)
{
    // c 是 void*(C++ 头文件里不能出现 ObjC 类型), 用 __bridge_retained 把所有权
    // 从 ARC 交给这个裸指针, 析构里再用 __bridge_transfer 交还, 保证 +1 与 -1 配对。
    c = (__bridge_retained void *) [[CAApplicationCaller alloc] init:&listener];
}

CADisplayLinkVSync::~CADisplayLinkVSync()
{
    if (c != nullptr) {
        // __bridge_transfer: 所有权转回 ARC, 这个强引用离开作用域时自动释放。
        id caller = (__bridge_transfer id) c;
        c = nullptr;
        (void) caller;
    }
}

void CADisplayLinkVSync::start()
{
    [(__bridge CAApplicationCaller *) c startMainLoop];
}

void CADisplayLinkVSync::pause()
{

}
