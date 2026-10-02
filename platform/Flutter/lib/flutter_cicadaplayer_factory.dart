import 'flutter_cicadaplayer.dart';

/// 播放器工厂。
///
/// 零拷贝要的纹理必须在播放器之前建好（原生侧要先拿到 Surface / 注册
/// FlutterTexture），所以 `createTexture` 走的是工厂通道，而不是播放器通道：
/// 两侧的原生实现都是"先有工厂，才有播放器"。
///
/// 原来这里还会额外发一次 `plugins.flutter_cicadaplayer_factory` 上的
/// `createCicadaPlayer`（仅 Android，且没有 await）。那条调用现在是多余的：
/// 原生播放器由 `createTexture` 顺带建出来，而 Flutter 保证同一个
/// BinaryMessenger 上的平台消息按发出顺序投递，所以"先 createTexture、
/// 后 setUrl"这个顺序是确定的，不会出现 setUrl 打到不存在的播放器上。
class FlutterCicadaPlayerFactory {
  FlutterCicadaPlayer createCicadaPlayer() {
    // 保留老的同步签名：调用方（example、既有业务代码）拿到实例后直接
    // 用它的 setOnXxx / setUrl。纹理由实例自己按需创建，见
    // FlutterCicadaPlayer.ensureTexture()。
    return FlutterCicadaPlayer.init(0);
  }
}
