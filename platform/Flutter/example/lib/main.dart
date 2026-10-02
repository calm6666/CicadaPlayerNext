// Flutter 侧的最小测试 App —— 专门用来验「零拷贝播放」这条主线。
//
// 【为什么新写一个入口，而不是修旧的 example/lib/page/*】
// 旧的 demo 是 Dart 2 / Flutter 2 时代写的，`flutter analyze` 报
// **323 个 error，分布在 31 个文件里的 23 个**，主要是三类：
//   1. 空安全迁移：`SomeType x` 当可选具名参数用（Dart 3 要求 required 或可空）；
//   2. Flutter 3 删掉的 API：RaisedButton / FlatButton 等；
//   3. class_used_as_mixin 之类的新规则。
// 那是**一整轮迁移**的工作量，而且与被测主线（零拷贝纹理）无关。
// 所以这个入口只做一件事：把播放器跑起来，并把回调/尺寸/错误显示出来。
//
// 旧的入口与页面**一个文件都没删**：
//   · 旧入口留档在 `lib/legacy_main.dart`；
//   · 旧的 `lib/page/*`、`lib/model/*`、`lib/mytest/*` 原样保留。
// Dart 只编译从入口可达的库，所以 APK 能正常构建；旧页面要恢复成可用，
// 需要单独做一轮 Dart 3 迁移（见文末 TODO）。

import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_cicadaplayer/flutter_cicadaplayer.dart';
import 'package:flutter_cicadaplayer/flutter_cicadaplayer_factory.dart';

/// 启动期/运行期"没人接住的错误"都会记到这里，然后显示在界面顶部的红条上。
///
/// 为什么需要：release 模式下 Flutter 默认只把异常写进 logcat，界面上什么都不显示 ——
/// 看起来就是"黑屏/闪退"。这个 App 的用途是测播放器，把原因直接摆到屏幕上比让人去查
/// logcat 快得多（尤其是在手上没有 adb 的时候）。
final ValueNotifier<String?> gFatalError = ValueNotifier<String?>(null);

void main() {
  final WidgetsBinding binding = WidgetsFlutterBinding.ensureInitialized();

  // 1) Flutter 框架层面的异常（build / layout / paint 里抛出来的）。
  FlutterError.onError = (FlutterErrorDetails details) {
    FlutterError.presentError(details);
    gFatalError.value = 'FlutterError: ${details.exception}';
  };

  // 2) 异步里没人接的异常，以及平台侧回过来的错误。
  binding.platformDispatcher.onError = (Object error, StackTrace stack) {
    gFatalError.value = '未捕获异常: $error';
    return true;
  };

  runApp(const CicadaTestApp());
}

/// 默认源：改成你自己的地址即可。用 http:// 的话注意清单里的
/// usesCleartextTraffic（本工程已经打开，见 AndroidManifest.xml）。
const String kDefaultUrl = 'https://player.alicdn.com/video/aliyunmedia.mp4';

const String kDefaultLocalPath = '/sdcard/Download/test.mp4';

class CicadaTestApp extends StatelessWidget {
  const CicadaTestApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'CicadaPlayer 零拷贝测试',
      debugShowCheckedModeBanner: false,
      home: const TestHomePage(),
    );
  }
}

class TestHomePage extends StatefulWidget {
  const TestHomePage({super.key});

  @override
  State<TestHomePage> createState() => _TestHomePageState();
}

class _TestHomePageState extends State<TestHomePage> {
  final FlutterCicadaPlayerFactory _factory = FlutterCicadaPlayerFactory();
  late final FlutterCicadaPlayer _player = _factory.createCicadaPlayer();

  final TextEditingController _urlCtrl =
      TextEditingController(text: kDefaultUrl);
  final TextEditingController _pathCtrl =
      TextEditingController(text: kDefaultLocalPath);

  final ScrollController _logScroll = ScrollController();
  final List<String> _log = <String>[];

  bool _hwDecode = true;
  bool _loop = false;
  bool _created = false; // onCreated 是否已回来（纹理已就绪）
  String _state = '未创建';
  String _size = '-';
  String _lastError = '-';
  int _durationMs = 0;
  int _positionMs = 0;
  /// 进度估算的两个状态：上次同步的毫秒数 + 从那一刻起的墙钟起点。
  int _seekBaseMs = 0;
  DateTime? _playingSince;
  double _rate = 1.0;
  Timer? _ticker;

  @override
  void initState() {
    super.initState();
    _bindCallbacks();
    // 纹理创建失败会被 CicadaTextureView 降级成一块黑屏（它自己 catch 掉了，不抛），
    // 于是"打不开画面"这件事在界面上完全没有线索。这里额外挂一个监听，把原因写进日志面板。
    // 返回的是同一个缓存 Future（ensureTexture 幂等），所以不会重复建纹理。
    _player.ensureTexture().then<void>(
      (int id) => _addLog('纹理已创建 textureId=$id'),
      onError: (Object e) => _addLog('！！纹理创建失败（很可能是 native 库没加载起来）: $e'),
    );
  }

  @override
  void dispose() {
    _ticker?.cancel();
    _logScroll.dispose();
    _urlCtrl.dispose();
    _pathCtrl.dispose();
    // 释放原生播放器与纹理（谁建谁放）。
    _player.destroy();
    super.dispose();
  }

  void _addLog(String msg) {
    final DateTime now = DateTime.now();
    final String ts = '${now.hour.toString().padLeft(2, '0')}:'
        '${now.minute.toString().padLeft(2, '0')}:'
        '${now.second.toString().padLeft(2, '0')}.'
        '${now.millisecond.toString().padLeft(3, '0')}';
    if (!mounted) {
      return;
    }
    setState(() {
      _log.insert(0, '[$ts] $msg');
      if (_log.length > 200) {
        _log.removeLast();
      }
    });
  }

  void _bindCallbacks() {
    _player.setOnPrepared(() {
      _addLog('onPrepared');
      _player.play();
    });
    _player.setOnRenderingStart(() {
      _addLog('onRenderingStart  ← 首帧上屏（零拷贝纹理真的画出来了）');
    });
    _player.setOnVideoSizeChanged((int w, int h) {
      setState(() => _size = '$w x $h');
      _addLog('onVideoSizeChanged $w x $h');
    });
    _player.setOnStateChanged((int s) {
      const Map<int, String> names = <int, String>{
        0: 'idle',
        1: 'inited',
        2: 'prepared',
        3: 'started',
        4: 'paused',
        5: 'stopped',
        6: 'completed',
        7: 'error',
      };
      setState(() => _state = names[s] ?? 'state=$s');
      _addLog('onStateChanged $s (${names[s] ?? '?'})');
    });
    _player.setOnSeekComplete(() => _addLog('onSeekComplete'));
    _player.setOnCompletion(() {
      _addLog('onCompletion');
      _syncPosition();
    });
    // 插件只暴露 setOnLoadingStatusListener（没有单独的 setOnLoadingProgress）。
    _player.setOnLoadingStatusListener(
      loadingProgress: (int percent, double speed) {
        if (percent % 10 == 0) {
          _addLog('loading $percent%  ${speed.toStringAsFixed(1)} KB/s');
        }
      },
    );
    _player.setOnError((int code, String extra, String msg) {
      setState(() => _lastError = '$code / $extra / $msg');
      _addLog('onError code=$code extra=$extra msg=$msg');
    });
    _player.setOnInfo((int infoCode, int extraValue, String extraMsg) {
      _addLog('onInfo $infoCode $extraValue $extraMsg');
    });
  }

  Future<void> _start(String url) async {
    if (url.isEmpty) {
      _addLog('地址为空，已忽略');
      return;
    }
    _addLog('setUrl: $url');
    await _player.stop();
    _seekBaseMs = 0;
    _playingSince = null;
    await _player.setEnableHardwareDecoder(_hwDecode);
    await _player.setLoop(_loop);
    await _player.setUrl(url);
    await _player.prepare();
  }

  /// 进度是**客户端估算**：Flutter 插件没有暴露"取当前位置"的接口
  /// （事件通道里也没有位置事件），所以这里按"上次同步点 + 已过去的时间 × 倍速"
  /// 推算。它只服务于测试 UI（看是否在走、seek 是否生效），不是精确值。
  void _syncPosition({int? seekTo}) {
    if (seekTo != null) {
      _seekBaseMs = seekTo;
      _playingSince = _state == 'started' ? DateTime.now() : null;
      return;
    }
    if (_state == 'started') {
      if (_playingSince == null) {
        _playingSince = DateTime.now();
      } else {
        final int elapsedMs =
            DateTime.now().difference(_playingSince!).inMilliseconds;
        _seekBaseMs = (_seekBaseMs + (elapsedMs * _rate).round()).clamp(
            0, _durationMs > 0 ? _durationMs : (1 << 31));
      }
      _playingSince = DateTime.now();
    } else {
      _playingSince = null;
    }
  }

  void _startTicker() {
    _ticker?.cancel();
    _ticker = Timer.periodic(const Duration(milliseconds: 500), (_) async {
      if (!mounted) {
        return;
      }
      final int dur = await _player.getDuration();
      if (!mounted) {
        return;
      }
      setState(() {
        _durationMs = dur;
        _syncPosition();
        _positionMs = _seekBaseMs;
      });
    });
  }

  /// 纹理就绪之后才 setUrl/prepare —— 顺序由 CicadaTextureView 保证。
  void _onTextureCreated() {
    setState(() => _created = true);
    _addLog('纹理已创建 textureId=${_player.textureId}');
    _startTicker();
  }

  @override
  Widget build(BuildContext context) {
    final double maxMs = _durationMs > 0 ? _durationMs.toDouble() : 0;
    final int clampedMs =
        _positionMs.clamp(0, _durationMs > 0 ? _durationMs : 0).toInt();
    final double curMs = clampedMs.toDouble();

    return Scaffold(
      appBar: AppBar(
        title: const Text('CicadaPlayer 零拷贝测试'),
        centerTitle: true,
      ),
      body: Column(
        children: <Widget>[
          // --------------------------------------------------- 致命错误红条
          // 只在"有没人接住的错误"时出现。release 下界面默认什么都不显示，
          // 有了它就等于把闪退原因直接摆在屏幕上。
          ValueListenableBuilder<String?>(
            valueListenable: gFatalError,
            builder: (BuildContext context, String? err, Widget? _) {
              if (err == null) {
                return const SizedBox.shrink();
              }
              return Container(
                width: double.infinity,
                color: const Color(0xFFB00020),
                padding: const EdgeInsets.all(8),
                child: Text(
                  err,
                  style: const TextStyle(color: Color(0xFFFFFFFF), fontSize: 12),
                ),
              );
            },
          ),

          // ------------------------------------------------------------- 画面
          SizedBox(
            height: 240,
            width: double.infinity,
            child: ColoredBox(
              color: const Color(0xFF000000),
              child: CicadaTextureView(
                player: _player,
                onCreated: _onTextureCreated,
              ),
            ),
          ),

          // ------------------------------------------------------------- 状态
          Container(
            width: double.infinity,
            color: const Color(0xFFF2F2F2),
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
            child: Text(
              '纹理: ${_created ? '就绪' : '创建中'}   '
              '视频尺寸: $_size   状态: $_state\n'
              '进度(客户端估算): ${_fmt(curMs.toInt())} / ${_fmt(_durationMs)}   '
              '倍速: ${_rate.toStringAsFixed(1)}   '
              '硬解: ${_hwDecode ? '开' : '关'}\n'
              '最近错误: $_lastError',
              style: const TextStyle(fontSize: 12, height: 1.4),
            ),
          ),

          // ------------------------------------------------------------- 控制
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 12),
            child: Column(
              children: <Widget>[
                Row(
                  children: <Widget>[
                    Expanded(
                      child: ElevatedButton(
                        onPressed: () => _start(_urlCtrl.text.trim()),
                        child: const Text('播 URL'),
                      ),
                    ),
                    const SizedBox(width: 8),
                    Expanded(
                      child: ElevatedButton(
                        onPressed: () {
                          final String p = _pathCtrl.text.trim();
                          // 本地文件走同一个入口：内核按 URL 形态自己判协议。
                          _start(p.startsWith('/') ? 'file://$p' : p);
                        },
                        child: const Text('播本地文件'),
                      ),
                    ),
                  ],
                ),
                Row(
                  children: <Widget>[
                    Expanded(
                      child: OutlinedButton(
                        onPressed: () => _player.pause(),
                        child: const Text('暂停'),
                      ),
                    ),
                    const SizedBox(width: 8),
                    Expanded(
                      child: OutlinedButton(
                        onPressed: () => _player.play(),
                        child: const Text('继续'),
                      ),
                    ),
                    const SizedBox(width: 8),
                    Expanded(
                      child: OutlinedButton(
                        onPressed: () => _player.stop(),
                        child: const Text('停止'),
                      ),
                    ),
                  ],
                ),
                Row(
                  children: <Widget>[
                    const Text('硬解'),
                    Switch(
                      value: _hwDecode,
                      onChanged: (bool v) async {
                        setState(() => _hwDecode = v);
                        await _player.setEnableHardwareDecoder(v);
                      },
                    ),
                    const Text('循环'),
                    Switch(
                      value: _loop,
                      onChanged: (bool v) async {
                        setState(() => _loop = v);
                        await _player.setLoop(v);
                      },
                    ),
                    const SizedBox(width: 8),
                    const Text('倍速'),
                    const SizedBox(width: 6),
                    DropdownButton<double>(
                      value: _rate,
                      items: const <double>[0.5, 1.0, 1.5, 2.0]
                          .map((double v) => DropdownMenuItem<double>(
                                value: v,
                                child: Text('${v}x'),
                              ))
                          .toList(),
                      onChanged: (double? v) async {
                        if (v == null) {
                          return;
                        }
                        setState(() => _rate = v);
                        await _player.setRate(v);
                      },
                    ),
                  ],
                ),
              ],
            ),
          ),

          // ------------------------------------------------------------- seek
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 12),
            child: Slider(
              value: maxMs > 0 ? curMs : 0,
              max: maxMs > 0 ? maxMs : 1,
              onChanged: maxMs > 0
                  ? (double v) => setState(() => _positionMs = v.toInt())
                  : null,
              onChangeEnd: (double v) {
                // seekMode 0 = 不精确（快），1 = 精确（帧级）
                _syncPosition(seekTo: v.toInt());
                setState(() => _positionMs = v.toInt());
                _addLog('seekTo ${v.toInt()} ms (精准模式)');
                _player.seekTo(v.toInt(), 1);
              },
            ),
          ),

          // ------------------------------------------------------------- 输入
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 12),
            child: Column(
              children: <Widget>[
                TextField(
                  controller: _urlCtrl,
                  decoration: const InputDecoration(
                    labelText: '在线地址（http/https）',
                    isDense: true,
                  ),
                ),
                TextField(
                  controller: _pathCtrl,
                  decoration: const InputDecoration(
                    labelText: '本地文件路径（如 /sdcard/Download/test.mp4）',
                    isDense: true,
                  ),
                ),
              ],
            ),
          ),

          const Divider(height: 8),

          // ------------------------------------------------------------- 日志
          Expanded(
            child: Scrollbar(
              controller: _logScroll,
              child: ListView.builder(
                controller: _logScroll,
                itemCount: _log.length,
                itemBuilder: (BuildContext context, int i) => Padding(
                  padding:
                      const EdgeInsets.symmetric(horizontal: 12, vertical: 2),
                  child: Text(
                    _log[i],
                    style:
                        const TextStyle(fontSize: 11, fontFamily: 'monospace'),
                  ),
                ),
              ),
            ),
          ),
        ],
      ),
    );
  }

  static String _fmt(int ms) {
    if (ms <= 0) {
      return '00:00';
    }
    final int total = ms ~/ 1000;
    final int m = total ~/ 60;
    final int s = total % 60;
    return '${m.toString().padLeft(2, '0')}:${s.toString().padLeft(2, '0')}';
  }
}

// TODO（与被测主线无关，单独一轮）：把 lib/page/*、lib/model/*、lib/mytest/*
// 从 Dart 2 迁到 Dart 3（323 个 error / 23 个文件），并补回 XML 与扫码依赖：
//   · xml_parser（已停止维护）→ package:xml；
//   · qrcode_reader（git 依赖）→ mobile_scanner；
//   · 下拉刷新 → RefreshIndicator；
//   · RaisedButton/FlatButton → ElevatedButton/OutlinedButton。
