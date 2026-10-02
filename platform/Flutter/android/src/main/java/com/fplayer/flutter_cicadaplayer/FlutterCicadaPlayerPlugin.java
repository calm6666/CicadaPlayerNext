package com.fplayer.flutter_cicadaplayer;

import androidx.annotation.NonNull;
import io.flutter.embedding.engine.plugins.FlutterPlugin;
import io.flutter.plugin.common.MethodCall;
import io.flutter.plugin.common.MethodChannel;
import io.flutter.plugin.common.MethodChannel.MethodCallHandler;
import io.flutter.plugin.common.MethodChannel.Result;
import io.flutter.plugin.common.PluginRegistry.Registrar;
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
 */
public class FlutterCicadaPlayerPlugin implements FlutterPlugin, MethodCallHandler {

    private FlutterPluginBinding flutterPluginBinding;
    private FlutterCicadaPlayer mFlutterCicadaPlayer;
    private PlayerSurface mPlayerSurface;
    /** 只有 pre-1.12 的 registerWith 注册路径会用到这两个（那时候拿不到 binding）。 */
    private TextureRegistry mTextureRegistry;
    private Registrar mRegistrar;

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

    //   This static function is optional and equivalent to onAttachedToEngine. It supports the old
    //   pre-Flutter-1.12 Android projects. You are encouraged to continue supporting
    //   plugin registration via this function while apps migrate to use the new Android APIs
    //   post-flutter-1.12 via https://flutter.dev/go/android-project-migration.
    //
    //   It is encouraged to share logic between onAttachedToEngine and registerWith to keep
    //   them functionally equivalent. Only one of onAttachedToEngine or registerWith will be called
    //   depending on the user's project. onAttachedToEngine or registerWith must both be defined
    //   in the same class.
    public static void registerWith(Registrar registrar) {
        registrar.platformViewRegistry().registerViewFactory(
                "flutter_cicadaplayer_render_view", new FlutterCicadaPlayerView(registrar));
        MethodChannel mCicadaPlayerFactoryMethodChannel =
                new MethodChannel(registrar.messenger(), "plugins.flutter_cicadaplayer_factory");
        FlutterCicadaPlayerPlugin plugin = new FlutterCicadaPlayerPlugin();
        plugin.mTextureRegistry = registrar.textures();
        plugin.mRegistrar = registrar;
        mCicadaPlayerFactoryMethodChannel.setMethodCallHandler(plugin);
    }

    @Override
    public void onMethodCall(@NonNull MethodCall call, @NonNull Result result) {
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

        TextureRegistry textures = mTextureRegistry;
        if (textures == null && flutterPluginBinding != null) {
            textures = flutterPluginBinding.getTextureRegistry();
        }
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
            if (flutterPluginBinding != null) {
                mFlutterCicadaPlayer = new FlutterCicadaPlayer(flutterPluginBinding);
            } else if (mRegistrar != null) {
                mFlutterCicadaPlayer = new FlutterCicadaPlayer(mRegistrar);
            }
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
