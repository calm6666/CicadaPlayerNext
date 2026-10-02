#import "FlutterCicadaPlayerPlugin.h"
#import "CicadaPlayerFactory.h"

@implementation FlutterCicadaPlayerPlugin
//+ (void)registerWithRegistrar:(NSObject<FlutterPluginRegistrar>*)registrar {
//  FlutterMethodChannel* channel = [FlutterMethodChannel
//      methodChannelWithName:@"flutter_aliplayer"
//            binaryMessenger:[registrar messenger]];
//  FlutterAliplayerPlugin* instance = [[FlutterAliplayerPlugin alloc] init];
//  [registrar addMethodCallDelegate:instance channel:channel];
//}

+ (void)registerWithRegistrar:(NSObject<FlutterPluginRegistrar> *)registrar
{
    /*
     * 纹理表从 registrar 拿，交给工厂去做零拷贝视频纹理的注册/注销。
     * 平台视图工厂保留（老 Dart 代码用 UiKitView 时还走得到），但新路径是 Texture widget。
     */
    CicadaPlayerFactory *factory = [[CicadaPlayerFactory alloc] initWithMessenger:registrar.messenger
                                                                  textureRegistry:registrar.textures];
    [registrar registerViewFactory:factory withId:@"plugins.flutter_cicadaplayer"];
}


- (void)handleMethodCall:(FlutterMethodCall *)call result:(FlutterResult)result
{
    if ([@"getPlatformVersion" isEqualToString:call.method]) {
        result([@"iOS " stringByAppendingString:[[UIDevice currentDevice] systemVersion]]);
    } else {
        result(FlutterMethodNotImplemented);
    }
}

@end
