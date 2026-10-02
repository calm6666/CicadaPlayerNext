package com.fplayer.flutter_cicadaplayer;

import androidx.annotation.NonNull;
import io.flutter.embedding.engine.plugins.FlutterPlugin;
import io.flutter.plugin.common.MethodCall;
import io.flutter.plugin.common.MethodChannel;
import io.flutter.plugin.common.MethodChannel.MethodCallHandler;
import io.flutter.plugin.common.MethodChannel.Result;
import io.flutter.view.TextureRegistry;

/**
 * FlutterCicadaPlayerPlugin
 *
 * <p>两条通道，职责分开：
 *
 * <ul>
 *   <li>{@code plugins.flutter_cicadaplayer_factory}：本类处理。放"和某个播放器实例无关"的
 *       操作，也就是零拷贝纹理的建/销（createTexture / disposeTexture）以及历史遗留的
 *       createCicadaPlayer。
 *   <li>{@code flutter_cicadaplayer}：由 {@link FlutterCicadaPlayer} 处理（老的通道契约一个字
 *       都没改）。
 * </ul>
 *
 * <p>顺序很关键：纹理必须比播放器先建好。原因见 {@link PlayerSurface}。这里用
 * {@link #obtainPlayer()} 保证两条通道拿到的是同一个播放器实例，不会因为调用先后而各建一个。
 *
 * <p>【本轮删掉了 Android v1 embedding 的注册路径】本类原来还有：
 *   <ul>
 *     <li>{@code import io.flutter.plugin.common.PluginRegistry.Registrar;}
 *     <li>{@code public static void registerWith(Registrar registrar)}
 *     <li>字段 {@code mTextureRegistry} / {@code mRegistrar} 以及 obtainPlayer() 里的 Registrar 分支
 *   </ul>
 * 这些在 Flutter 3.29 起**已从引擎里移除**（v1 Android embedding 被删），编译必然失败：
 *   <pre>
 *   error: 找不到符号  类 Registrar
 *       public FlutterCicadaPlayer(PluginRegistry.Registrar registrar)
 *   error: 对FlutterCicadaPlayer的引用不明确
 *       new FlutterCicadaPlayer(flutterPluginBinding)
 *         （两个构造器都匹配 —— Registrar 解析不到时编译器就是这种表现）
 *   </pre>
 * 现在只保留 v2（FlutterPlugin + FlutterPluginBinding）这一条路，纹理注册表一律从 binding 取。
 */
public class FlutterCicadaPlayerPlugin implements FlutterPlugin, MethodCallHandler {

    private FlutterPluginBinding flutterPluginBinding;
    private FlutterCicadaPlayer mFlutterCicadaPlayer;
    private PlayerSurface mPlayerSurface;

    @Override
    public void onAttachedToEngine(@NonNull FlutterPluginBinding flutterPluginBinding) {
        this.flutterPluginBinding = flutterPluginBinding;
        flutterPluginBinding.getPlatformViewRegistry().registerViewFactory(
                "flutter_cicadaplayer_render_view", new FlutterCicadaPlayerView(flutterPluginBinding));
        MethodChannel mCicadaPlayerFactoryMethodChannel =
                new MethodChannel(flutterPluginBinding.getBinaryMessenger(),
                        "plugins.flutter_cicadaplayer_factory");
        mCicadaPlayerFactoryMethodChannel.setMethodCallHandler(this);
    }

    @Override
    public void onMethodCall(@NonNull MethodCall call, @NonNull Result result) {
        // 【本轮加这层 try/catch】理由同 FlutterCicadaPlayer.onMethodCall：
        // 加载 native 库失败抛的是 java.lang.UnsatisfiedLinkError（Error，不是 Exception），
        // Flutter 的 MethodChannel 只接 RuntimeException，于是它会直接在平台线程上冒出去
        // 把整个进程干掉 —— 表现就是"装上一打开就闪退"，且不留下可读的 Flutter 日志。
        // 包住之后变成 Dart 能看到的 PlatformException，至少能把原因显示出来。
        try {
            switch (call.method) {
                case "createCicadaPlayer":
                    // 播放器只在"建纹理"那条路上创建：纹理必须先于播放器存在，
                    // 所以这里不能单独建一个播放器出来。
                    obtainPlayer();
                    result.success(null);
                    break;
                case "createTexture":
                    result.success(createTexture());
                    break;
                case "disposeTexture":
                    disposeTexture();
                    result.success(null);
                    break;
                default:
                    result.notImplemented();
                    break;
            }
        } catch (Throwable t) {
            // 固定 tag，方便 adb logcat -s FlutterCicadaPlayerPlugin 直接抓到原因。
            android.util.Log.e("FlutterCicadaPlayerPlugin",
                    "onMethodCall(" + call.method + ") 抛出了 " + t, t);
            // 一个 Result 只能回复一次；上面若已经 success 过，这里再回复会抛 IllegalStateException。
            try {
                result.error("CICADA_PLUGIN_CALL_FAILED", call.method + " 失败: " + t, null);
            } catch (Throwable ignored) {
            }
        }
    }

    /**
     * 建一张零拷贝纹理并把它的 Surface 交给播放器，返回纹理 id。
     *
     * <p>用 TextureRegistry.createSurfaceProducer（Flutter 3.10+ 的正式 API）而不是已废弃的
     * createSurfaceTexture：见 {@link PlayerSurface} 的说明。
     */
    private long createTexture() {
        FlutterCicadaPlayer player = obtainPlayer();
        disposeTexture();

        TextureRegistry textures = flutterPluginBinding.getTextureRegistry();
        PlayerSurface surface = new PlayerSurface(textures.createSurfaceProducer());
        mPlayerSurface = surface;
        player.setPlayerSurface(surface);
        // 顺序：先建纹理，再把 Surface 交给播放器，最后才让 Dart 侧拿到 id
        // （Dart 拿到 id 之后才会 setUrl/prepare）。
        surface.attachPlayer(player.getCicadaPlayer());
        return surface.getTextureId();
    }

    private void disposeTexture() {
        if (mPlayerSurface != null) {
            mPlayerSurface.release();
            mPlayerSurface = null;
        }
    }

    /**
     * 取当前引擎的播放器实例（没有就建）。
     *
     * <p>为什么要有这个：老契约是 Dart 侧先调工厂通道的 createCicadaPlayer、再调播放器通道的
     * createCicadaPlayer。现在纹理那条路会顺带把播放器建出来，这两条路必须落到同一个实例上，
     * 否则 Dart 的事件流会挂在另一个播放器上。
     */
    private FlutterCicadaPlayer obtainPlayer() {
        if (mFlutterCicadaPlayer == null) {
            // 只有 v2 embedding 这一条路（v1 的 Registrar 分支随 registerWith 一起去掉了）。
            mFlutterCicadaPlayer = new FlutterCicadaPlayer(flutterPluginBinding);
        }
        return mFlutterCicadaPlayer;
    }

    @Override
    public void onDetachedFromEngine(@NonNull FlutterPluginBinding binding) {
        disposeTexture();
        mFlutterCicadaPlayer = null;
        flutterPluginBinding = null;
    }
}
