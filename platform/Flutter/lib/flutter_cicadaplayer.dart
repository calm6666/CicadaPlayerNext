import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'flutter_avpdef.dart';

export 'flutter_avpdef.dart';

typedef OnPrepared = void Function();
typedef OnRenderingStart = void Function();
typedef OnVideoSizeChanged = void Function(int width, int height);
typedef OnSnapShot = void Function(String path);

typedef OnSeekComplete = void Function();
typedef OnSeiData = void Function(); //TODO

typedef OnLoadingBegin = void Function();
typedef OnLoadingProgress = void Function(int percent, double netSpeed);
typedef OnLoadingEnd = void Function();

typedef OnStateChanged = void Function(int newState);

typedef OnSubtitleExtAdded = void Function(int trackIndex, String url);
typedef OnSubtitleShow = void Function(
    int trackIndex, int subtitleID, String subtitle);
typedef OnSubtitleHide = void Function(int trackIndex, int subtitleID);
typedef OnTrackReady = void Function();

typedef OnInfo = void Function(int infoCode, int extraValue, String extraMsg);
typedef OnError = void Function(
    int errorCode, String errorExtra, String errorMsg);
typedef OnCompletion = void Function();

typedef OnTrackChanged = void Function(dynamic value);

typedef OnThumbnailPreparedSuccess = void Function();
typedef OnThumbnailPreparedFail = void Function();

typedef OnThumbnailGetSuccess = void Function(
    Uint8List bitmap, Int64List range);
typedef OnThumbnailGetFail = void Function();

/// 只在 [FlutterCicadaPlayer.init] 的"建纹理失败"那条路上用到：失败不在这里抛，
/// 真正的错误由 CicadaTextureView 的 FutureBuilder 处理。
void _swallowTextureError(Object error, StackTrace stackTrace) {}

/// 播放器实例。一个实例 = 原生侧一个 CicadaPlayer + 一张零拷贝视频纹理。
///
/// 纹理 id 不再由本类的构造参数决定，而是原生侧 `createTexture` 的返回值：
/// 谁创建谁释放（见 [ensureTexture] / [disposeTexture]）。
class FlutterCicadaPlayer {
  OnLoadingBegin? onLoadingBegin;
  OnLoadingProgress? onLoadingProgress;
  OnLoadingEnd? onLoadingEnd;
  OnPrepared? onPrepared;
  OnRenderingStart? onRenderingStart;
  OnVideoSizeChanged? onVideoSizeChanged;
  OnSeekComplete? onSeekComplete;
  OnStateChanged? onStateChanged;
  OnInfo? onInfo;
  OnCompletion? onCompletion;
  OnTrackReady? onTrackReady;
  OnError? onError;
  OnSnapShot? onSnapShot;

  OnTrackChanged? onTrackChanged;
  OnThumbnailPreparedSuccess? onThumbnailPreparedSuccess;
  OnThumbnailPreparedFail? onThumbnailPreparedFail;

  OnThumbnailGetSuccess? onThumbnailGetSuccess;
  OnThumbnailGetFail? onThumbnailGetFail;

  //外挂字幕
  OnSubtitleExtAdded? onSubtitleExtAdded;
  OnSubtitleHide? onSubtitleHide;
  OnSubtitleShow? onSubtitleShow;

  /// 播放器/事件通道。名字保持不变，原生两侧（Java/ObjC）用的是同一组名字。
  final MethodChannel channel = const MethodChannel('flutter_cicadaplayer');
  final EventChannel eventChannel =
      const EventChannel('flutter_cicadaplayer_event');

  /// 工厂通道：只承担"和某个播放器无关的"操作——建/销毁纹理。
  /// 为什么放在这条通道上：原生两侧都是先有工厂（FlutterPlugin /
  /// CicadaPlayerFactory）才有播放器，纹理必须在播放器之前建好并把 Surface
  /// 交给它，否则第一帧会落在没有消费者的 BufferQueue 上。
  final MethodChannel factoryChannel =
      const MethodChannel('plugins.flutter_cicadaplayer_factory');

  /// 原生侧那张视频纹理的 id，null 表示还没建。渲染用 `Texture(textureId:)`。
  int? textureId;

  /// 播放器与原生实例的对应关系。当前原生实现是"一个引擎一个播放器"，
  /// 所以这里固定 0；留着它是为了让事件回调能带上身份，方便将来扩多实例。
  int playerId = 0;

  /// 当前视频尺寸（像素），来自原生 onVideoSizeChanged 事件。
  ///
  /// 为什么要暴露成 Listenable：以前的平台视图是原生按像素尺寸自己铺的，
  /// 换成 Texture 之后"按真实宽高比布局"这件事只能由 Dart 做，widget 需要能收到变化。
  final ValueNotifier<Offset?> videoSize = ValueNotifier<Offset?>(null);

  Future<int>? _textureFuture;

  FlutterCicadaPlayer.init(int id) {
    playerId = id;

    /*
     * 【顺序很关键：先让原生侧把播放器建出来，**再**订阅事件通道】
     *
     * 原生侧三条通道的 handler 不是一起注册的：
     *   * 插件挂载时（onAttachedToEngine）只注册**工厂**通道
     *     plugins.flutter_cicadaplayer_factory；
     *   * 播放器通道 flutter_cicadaplayer 与事件通道 flutter_cicadaplayer_event
     *     的 handler，是在**原生播放器对象构造时**注册的
     *     （FlutterCicadaPlayer 构造函数里那两行 setMethodCallHandler / setStreamHandler），
     *     而那个对象只在工厂通道的 createCicadaPlayer / createTexture 里被建出来。
     *
     * 所以原来"先 listen、后建纹理"的顺序是错的，会抛：
     *     MissingPluginException(No implementation found for method listen
     *                            on channel flutter_cicadaplayer_event)
     * 而且**不会自愈**：receiveBroadcastStream 的 listen 只往原生发一次，抛掉之后
     * 这条流不会再发第二次，于是 onPrepared / onRenderingStart 等事件一个都收不到
     *（表现是画面一直黑着、日志一片安静）。
     * 另外这条异常不会走 onError：EventChannel 内部是用 FlutterError.reportError 报的，
     * 所以它会进全局错误处理器，界面上就是顶部那条红条。
     *
     * 现在改成 ensureTexture() 成功之后再订阅：createTexture 走的是**工厂**通道，
     * 它返回时原生播放器一定已经构造完，两条通道的 handler 都已就绪。
     *
     * 失败**不**在这里抛：真正的错误由 CicadaTextureView 的 FutureBuilder 接住
     *（那里能把画面降级成黑屏并保留一份可诊断的状态）；这里吞掉是为了避免
     * 一个没人 await 的 Future 把异常抛到 zone 里变成未捕获异常。
     */
    ensureTexture().then(_listenEventChannel, onError: _swallowTextureError);
  }

  /// 原生播放器就绪之后才订阅事件通道（顺序原因见 [init] 里的注释）。
  /// 参数就是纹理 id，这里用不到 —— 只是为了接住 ensureTexture 的结果。
  void _listenEventChannel(int textureId) {
    eventChannel.receiveBroadcastStream().listen(_onEvent, onError: _onError);
  }

  /// 建纹理（幂等）。同一个实例重复调用只会发一次原生调用。
  ///
  /// 零拷贝的起点就在这里：原生侧 `createTexture` 返回的 id 背后是
  /// * Android: TextureRegistry.SurfaceProducer 的 Surface
  /// * iOS: FlutterTexture + 解码器直出的 CVPixelBuffer
  Future<int> ensureTexture() {
    final cached = _textureFuture;
    if (cached != null) {
      return cached;
    }

    final future = _createTexture();
    _textureFuture = future;
    return future;
  }

  Future<int> _createTexture() async {
    final id = await factoryChannel.invokeMethod<int>('createTexture');
    if (id == null) {
      throw StateError('createTexture returned null');
    }
    textureId = id;
    return id;
  }

  /// 释放纹理。谁建谁放：原生侧注销纹理并断开播放器的 Surface。
  Future<void> disposeTexture() async {
    final id = textureId;
    _textureFuture = null;
    textureId = null;
    if (id == null) {
      return;
    }
    await factoryChannel.invokeMethod<void>('disposeTexture');
  }

  /// 让用户挑一个本地视频：**申请系统媒体权限 → 打开系统文件选择器**，返回选中的地址。
  ///
  /// 返回的是系统给的 `content://...` Uri 字符串；用户取消时返回 null。
  /// 拿到之后直接 `setUrl(uri)` 就能播 —— 内核自己按 Uri 读（SDK 里有
  /// `com.cicada.player.utils.ContentDataSource`），**不需要先拷贝一份文件**。
  ///
  /// 权限按系统版本申请（Android 13+ 是 READ_MEDIA_VIDEO，14+ 再加
  /// READ_MEDIA_VISUAL_USER_SELECTED，12- 是 READ_EXTERNAL_STORAGE）；
  /// 用户拒绝时抛 PlatformException("PERMISSION_DENIED")，调用方如实提示即可，别假装播了。
  ///
  /// 这个方法与播放器实例无关，只是复用插件那条工厂通道，所以放在这里，
  /// 省得再暴露一条通道。
  ///
  /// 注意：目前只有 Android 侧实现了（iOS 侧要另写 PHPicker/UIImagePickerController）。
  Future<String?> pickLocalVideo() {
    return factoryChannel.invokeMethod<String>('pickLocalVideo');
  }

  void setOnPrepared(OnPrepared prepared) {
    this.onPrepared = prepared;
  }

  void setOnRenderingStart(OnRenderingStart renderingStart) {
    this.onRenderingStart = renderingStart;
  }

  void setOnVideoSizeChanged(OnVideoSizeChanged videoSizeChanged) {
    this.onVideoSizeChanged = videoSizeChanged;
  }

  void setOnSnapShot(OnSnapShot snapShot) {
    this.onSnapShot = snapShot;
  }

  void setOnSeekComplete(OnSeekComplete seekComplete) {
    this.onSeekComplete = seekComplete;
  }

  void setOnError(OnError onError) {
    this.onError = onError;
  }

  void setOnLoadingStatusListener(
      {OnLoadingBegin? loadingBegin,
      OnLoadingProgress? loadingProgress,
      OnLoadingEnd? loadingEnd}) {
    this.onLoadingBegin = loadingBegin;
    this.onLoadingProgress = loadingProgress;
    this.onLoadingEnd = loadingEnd;
  }

  void setOnStateChanged(OnStateChanged stateChanged) {
    this.onStateChanged = stateChanged;
  }

  void setOnInfo(OnInfo info) {
    this.onInfo = info;
  }

  void setOnCompletion(OnCompletion completion) {
    this.onCompletion = completion;
  }

  void setOnTrackReady(OnTrackReady onTrackReady) {
    this.onTrackReady = onTrackReady;
  }

  void setOnTrackChanged(OnTrackChanged onTrackChanged) {
    this.onTrackChanged = onTrackChanged;
  }

  void setOnThumbnailPreparedListener(
      {OnThumbnailPreparedSuccess? preparedSuccess,
      OnThumbnailPreparedFail? preparedFail}) {
    this.onThumbnailPreparedSuccess = preparedSuccess;
    this.onThumbnailPreparedFail = preparedFail;
  }

  void setOnThumbnailGetListener(
      {OnThumbnailGetSuccess? onThumbnailGetSuccess,
      OnThumbnailGetFail? onThumbnailGetFail}) {
    this.onThumbnailGetSuccess = onThumbnailGetSuccess;
    this.onThumbnailGetFail = onThumbnailGetFail;
  }

  void setOnSubtitleShow(OnSubtitleShow onSubtitleShow) {
    this.onSubtitleShow = onSubtitleShow;
  }

  void setOnSubtitleHide(OnSubtitleHide onSubtitleHide) {
    this.onSubtitleHide = onSubtitleHide;
  }

  void setOnSubtitleExtAdded(OnSubtitleExtAdded onSubtitleExtAdded) {
    this.onSubtitleExtAdded = onSubtitleExtAdded;
  }

  Future<void> createCicadaPlayer() async {
    return channel.invokeMethod('createCicadaPlayer');
  }

  Future<void> setUrl(String url) async {
    return channel.invokeMethod('setUrl', url);
  }

  /// 对象模式播放：传入统一的 MediaManifest 清单 JSON（与 hili-player Web 端
  /// MediaManifest 结构一致），无需 m3u8/mpd URL。支持 AES-128 与
  /// Widevine/FairPlay/ClearKey DRM 内容保护。
  Future<void> setDataSourceManifest(String mediaManifestJson) async {
    return channel.invokeMethod('setDataSourceManifest', mediaManifestJson);
  }

  Future<void> prepare() async {
    return channel.invokeMethod('prepare');
  }

  Future<void> play() async {
    return channel.invokeMethod('play');
  }

  Future<void> pause() async {
    return channel.invokeMethod('pause');
  }

  Future<dynamic> snapshot(String path) async {
    return channel.invokeMethod('snapshot', path);
  }

  Future<void> stop() async {
    return channel.invokeMethod('stop');
  }

  Future<void> destroy() async {
    return channel.invokeMethod('destroy');
  }

  Future<void> seekTo(int position, int seekMode) async {
    var map = {"position": position, "seekMode": seekMode};
    return channel.invokeMethod("seekTo", map);
  }

  Future<bool> isLoop() async {
    return await channel.invokeMethod<bool>('isLoop') ?? false;
  }

  Future<void> setLoop(bool isloop) async {
    return channel.invokeMethod('setLoop', isloop);
  }

  Future<bool> isAutoPlay() async {
    return await channel.invokeMethod<bool>('isAutoPlay') ?? false;
  }

  Future<void> setAutoPlay(bool isAutoPlay) async {
    return channel.invokeMethod('setAutoPlay', isAutoPlay);
  }

  Future<bool> isMuted() async {
    return await channel.invokeMethod<bool>('isMuted') ?? false;
  }

  Future<void> setMuted(bool isMuted) async {
    return channel.invokeMethod('setMuted', isMuted);
  }

  Future<bool> enableHardwareDecoder() async {
    return await channel.invokeMethod<bool>('enableHardwareDecoder') ?? false;
  }

  Future<void> setEnableHardwareDecoder(bool isHardWare) async {
    return channel.invokeMethod('setEnableHardwareDecoder', isHardWare);
  }

  Future<int> getRotateMode() async {
    return await channel.invokeMethod<int>('getRotateMode') ?? 0;
  }

  Future<int> getDuration() async {
    return await channel.invokeMethod<int>('getDuration') ?? 0;
  }

  Future<void> setRotateMode(int mode) async {
    return channel.invokeMethod('setRotateMode', mode);
  }

  Future<int> getScalingMode() async {
    return await channel.invokeMethod<int>('getScalingMode') ?? 0;
  }

  Future<void> setScalingMode(int mode) async {
    return channel.invokeMethod('setScalingMode', mode);
  }

  Future<int> getMirrorMode() async {
    return await channel.invokeMethod<int>('getMirrorMode') ?? 0;
  }

  Future<void> setMirrorMode(int mode) async {
    return channel.invokeMethod('setMirrorMode', mode);
  }

  Future<double> getRate() async {
    return await channel.invokeMethod<double>('getRate') ?? 1.0;
  }

  Future<void> setRate(double mode) async {
    return channel.invokeMethod('setRate', mode);
  }

  Future<void> setVideoBackgroundColor(int color) async {
    return channel.invokeMethod('setVideoBackgroundColor', color);
  }

  Future<void> setVolume(double volume) async {
    return channel.invokeMethod('setVolume', volume);
  }

  Future<double> getVolume() async {
    return await channel.invokeMethod<double>('getVolume') ?? 1.0;
  }

  Future<dynamic> getConfig() async {
    return channel.invokeMethod("getConfig");
  }

  Future<void> setConfig(Map<dynamic, dynamic> map) async {
    return channel.invokeMethod("setConfig", map);
  }

  Future<dynamic> getCacheConfig() async {
    return channel.invokeMethod("getCacheConfig");
  }

  Future<void> setCacheConfig(Map<dynamic, dynamic> map) async {
    return channel.invokeMethod("setCacheConfig", map);
  }

  ///return deviceInfo
  Future<String> createDeviceInfo() async {
    return await channel.invokeMethod<String>("createDeviceInfo") ?? "";
  }

  ///type : {FlutterAvpdef.BLACK_DEVICES_H264 / FlutterAvpdef.BLACK_DEVICES_HEVC}
  Future<void> addBlackDevice(String type, String model) async {
    var map = {
      'black_type': type,
      'black_device': model,
    };
    return channel.invokeMethod("addBlackDevice", map);
  }

  Future<String> getSDKVersion() async {
    return await channel.invokeMethod<String>("getSDKVersion") ?? "";
  }

  Future<void> enableMix(bool enable) {
    return channel.invokeMethod("enableMix", enable);
  }

  Future<void> enableConsoleLog(bool enable) {
    return channel.invokeMethod("enableConsoleLog", enable);
  }

  Future<void> setLogLevel(int level) async {
    return channel.invokeMethod("setLogLevel", level);
  }

  Future<int> getLogLevel() async {
    return await channel.invokeMethod<int>("getLogLevel") ?? -1;
  }

  Future<dynamic> getMediaInfo() {
    return channel.invokeMethod("getMediaInfo");
  }

  Future<dynamic> getCurrentTrack(int trackIdx) {
    return channel.invokeMethod("getCurrentTrack", trackIdx);
  }

  Future<dynamic> createThumbnailHelper(String thumbnail) {
    return channel.invokeMethod("createThumbnailHelper", thumbnail);
  }

  Future<dynamic> requestBitmapAtPosition(int position) {
    return channel.invokeMethod("requestBitmapAtPosition", position);
  }

  Future<void> addExtSubtitle(String url) {
    return channel.invokeMethod("addExtSubtitle", url);
  }

  Future<void> selectExtSubtitle(int trackIndex, bool enable) {
    var map = {'trackIndex': trackIndex, 'enable': enable};
    return channel.invokeMethod("selectExtSubtitle", map);
  }

  // accurate 0 为不精确  1 为精确  不填为忽略
  Future<void> selectTrack(int trackIdx, {int accurate = -1}) {
    var map = {
      'trackIdx': trackIdx,
      'accurate': accurate,
    };
    return channel.invokeMethod("selectTrack", map);
  }

  Future<void> setPrivateService(Int8List data) {
    return channel.invokeMethod("setPrivateService", data);
  }

  Future<void> setStreamDelayTime(int trackIdx, int time) {
    var map = {'index': trackIdx, 'time': time};
    return channel.invokeMethod("setStreamDelayTime", map);
  }

  void _onEvent(dynamic event) {
    if (event is! Map) {
      return;
    }
    // 用 as String? 而不是直接 as String：原生两侧的事件字段并不完全对称
    // （例如 iOS 的 onLoadingProgress 没有 netSpeed、onInfo 有时没有 extraValue），
    // 一个字段类型不符就抛异常会把整条事件流带断，所以宁可容忍缺字段。
    final String? method = event[EventChanneldef.TYPE_KEY] as String?;
    switch (method) {
      case "onPrepared":
        onPrepared?.call();
        break;
      case "onRenderingStart":
        onRenderingStart?.call();
        break;
      case "onVideoSizeChanged":
        final width = event['width'] as int? ?? 0;
        final height = event['height'] as int? ?? 0;
        videoSize.value = Offset(width.toDouble(), height.toDouble());
        final onVideoSizeChanged = this.onVideoSizeChanged;
        if (onVideoSizeChanged != null) {
          onVideoSizeChanged(width, height);
        }
        break;
      case "onSnapShot":
        final onSnapShot = this.onSnapShot;
        if (onSnapShot != null) {
          onSnapShot(event['snapShotPath'] as String? ?? "");
        }
        break;
      case "onChangedSuccess":
        break;
      case "onChangedFail":
        break;
      case "onSeekComplete":
        onSeekComplete?.call();
        break;
      case "onSeiData":
        break;
      case "onLoadingBegin":
        onLoadingBegin?.call();
        break;
      case "onLoadingProgress":
        final onLoadingProgress = this.onLoadingProgress;
        if (onLoadingProgress != null) {
          onLoadingProgress(event['percent'] as int? ?? 0,
              (event['netSpeed'] as num?)?.toDouble() ?? 0.0);
        }
        break;
      case "onLoadingEnd":
        onLoadingEnd?.call();
        break;
      case "onStateChanged":
        final onStateChanged = this.onStateChanged;
        if (onStateChanged != null) {
          onStateChanged(event['newState'] as int? ?? 0);
        }
        break;
      case "onInfo":
        final onInfo = this.onInfo;
        if (onInfo != null) {
          onInfo(event['infoCode'] as int? ?? 0, event['extraValue'] as int? ?? 0,
              event['extraMsg'] as String? ?? "");
        }
        break;
      case "onError":
        final onError = this.onError;
        if (onError != null) {
          onError(event['errorCode'] as int? ?? 0,
              event['errorExtra'] as String? ?? "",
              event['errorMsg'] as String? ?? "");
        }
        break;
      case "onCompletion":
        onCompletion?.call();
        break;
      case "onTrackReady":
        onTrackReady?.call();
        break;
      case "onTrackChanged":
        final onTrackChanged = this.onTrackChanged;
        if (onTrackChanged != null) {
          onTrackChanged(event['info']);
        }
        break;
      case "thumbnail_onPrepared_Success":
        onThumbnailPreparedSuccess?.call();
        break;
      case "thumbnail_onPrepared_Fail":
        onThumbnailPreparedFail?.call();
        break;
      case "onThumbnailGetSuccess":
        final onThumbnailGetSuccess = this.onThumbnailGetSuccess;
        dynamic bitmap = event['thumbnailbitmap'];
        dynamic range = event['thumbnailRange'];
        if (onThumbnailGetSuccess != null) {
          // iOS 侧 range 走的是 FlutterStandardTypedData 还是普通 List，取决于
          // 原生实现；这里两种都接住，避免一条事件把流打断。
          if (Platform.isIOS && range is List && range is! Int64List) {
            range = Int64List.fromList(range.cast<int>());
          }
          if (bitmap is Uint8List && range is Int64List) {
            onThumbnailGetSuccess(bitmap, range);
          }
        }
        break;
      case "onThumbnailGetFail":
        onThumbnailGetFail?.call();
        break;
      case "onSubtitleExtAdded":
        final onSubtitleExtAdded = this.onSubtitleExtAdded;
        if (onSubtitleExtAdded != null) {
          onSubtitleExtAdded(event['trackIndex'] as int, event['url'] as String);
        }
        break;
      case "onSubtitleShow":
        final onSubtitleShow = this.onSubtitleShow;
        if (onSubtitleShow != null) {
          onSubtitleShow(event['trackIndex'] as int, event['subtitleID'] as int,
              event['subtitle'] as String);
        }
        break;
      case "onSubtitleHide":
        final onSubtitleHide = this.onSubtitleHide;
        if (onSubtitleHide != null) {
          onSubtitleHide(event['trackIndex'] as int, event['subtitleID'] as int);
        }
        break;
    }
  }

  void _onError(dynamic error) {}
}

typedef void CicadaPlayerViewCreatedCallback();

/// 兼容旧名字。等价于 [CicadaTextureView]（新名字更准确地表达了它渲染的是纹理，
/// 而不是平台视图）。
@Deprecated('Use CicadaTextureView instead; it renders a Flutter texture, '
    'not a platform view')
class CicadaPlayerView extends CicadaTextureView {
  CicadaPlayerView({
    Key? key,
    required CicadaPlayerViewCreatedCallback onCreated,
    required FlutterCicadaPlayer player,
    double? x,
    double? y,
    double? width,
    double? height,
  }) : super(
          key: key,
          onCreated: onCreated,
          player: player,
          x: x,
          y: y,
          width: width,
          height: height,
        );
}

/// 真正把视频画到屏幕上的那个 widget。
///
/// 为什么不再是 AndroidView / UiKitView：那两条路都是**平台视图**，视频要经过
/// 一次额外的合成（Android 上还是另一张 SurfaceTexture），而且和 Flutter 自己
/// 的合成器不同步。改成 `Texture(textureId:)` 之后，画面就是 Flutter 自己合成
/// 的一张纹理：Android 侧是 TextureRegistry 的 Surface，iOS 侧是 FlutterTexture
/// 直接交解码器的 CVPixelBuffer，两端都没有 CPU 拷贝。
///
/// 布局行为保持和老实现一致：显式给了 [width] / [height] 就按它撑开，没给就按
/// 视频真实宽高比自适应；给了 [x] / [y] 就相对自身平移（老实现是把 x/y 当创建
/// 参数交给原生视图的）。
class CicadaTextureView extends StatefulWidget {
  final CicadaPlayerViewCreatedCallback? onCreated;
  final FlutterCicadaPlayer player;
  final double? x;
  final double? y;
  final double? width;
  final double? height;

  const CicadaTextureView({
    Key? key,
    required this.player,
    this.onCreated,
    this.x,
    this.y,
    this.width,
    this.height,
  }) : super(key: key);

  @override
  State<CicadaTextureView> createState() => _CicadaTextureViewState();
}

class _CicadaTextureViewState extends State<CicadaTextureView> {
  /// 固定住创建这张纹理时用的那个 player。
  ///
  /// 为什么不直接用 widget.player：父级换了 player 之后，dispose 里要释放的是
  /// "当初建纹理的那个"，不是新的那个；用 late final 把它钉住，行为和生命周期
  /// 就对上了（本 widget 不支持中途换 player，换了请重建 widget）。
  late final FlutterCicadaPlayer _player = widget.player;

  /// 纹理创建的结果。没有它就没法把 `onCreated` 放在 build 之后再调
  /// （build 里同步回调会很危险：老代码用 onCreated 去 setUrl，那时还没有纹理）。
  late final Future<int> _textureFuture = _player.ensureTexture();
  /// 本 widget 是否真的建出了纹理。只有建了才由本 widget 释放，避免"A 显示、
  /// B 隐藏"两个 widget 共用一张纹理时，先 dispose 的那个把它拆掉。
  bool _ownsTexture = false;
  /// onCreated 只回调一次。
  bool _notifiedCreated = false;

  @override
  void dispose() {
    if (_ownsTexture) {
      _player.disposeTexture();
    }
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    // 视频尺寸一到就按真实宽高比收敛（老的平台视图是原生自己铺的，没有这层）。
    return FutureBuilder<int>(
      future: _textureFuture,
      builder: (BuildContext context, AsyncSnapshot<int> snapshot) {
        if (snapshot.hasError) {
          // 纹理建不出来时不要抛到渲染树里（那会整页红屏）。给一块黑的，
          // 具体原因由原生侧日志说明。
          return const ColoredBox(color: Color(0xFF000000));
        }
        final id = snapshot.data;
        if (id != null && !_ownsTexture) {
          _ownsTexture = true;
          // 回调放在 build 之后：老代码在 onCreated 里 setUrl/prepare，必须在
          // 原生播放器已经拿到 Surface / 纹理 id 之后才安全。
          if (!_notifiedCreated) {
            _notifiedCreated = true;
            WidgetsBinding.instance.addPostFrameCallback((_) {
              if (mounted) {
                widget.onCreated?.call();
              }
            });
          }
        }
        return ValueListenableBuilder<Offset?>(
          valueListenable: _player.videoSize,
          builder: (BuildContext context, Offset? size, Widget? _) {
            Widget child = id == null
                ? const ColoredBox(color: Color(0xFF000000))
                : Texture(textureId: id);
            final width = widget.width;
            final height = widget.height;
            // 显式给了尺寸就按给的来（和老的 creationParams 语义一致）；
            // 没给才用视频宽高比，免得改掉调用方的布局意图。
            if (width == null && height == null && size != null && size.dx > 0 && size.dy > 0) {
              child = AspectRatio(aspectRatio: size.dx / size.dy, child: child);
            }
            final x = widget.x;
            final y = widget.y;
            // 为什么不用 Positioned：老实现是把 x/y 当创建参数交给原生的，返回的
            // 只是一个普通 widget；Positioned 要求父级必须是 Stack，会悄悄改掉
            // 调用方的布局约束（example 的 Column 会直接报错）。
            if (x != null || y != null) {
              child = Transform.translate(
                  offset: Offset(x ?? 0.0, y ?? 0.0), child: child);
            }
            if (width != null || height != null) {
              child = SizedBox(width: width, height: height, child: child);
            }
            return child;
          },
        );
      },
    );
  }
}
