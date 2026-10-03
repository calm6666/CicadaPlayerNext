package com.fplayer.flutter_cicadaplayer;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import androidx.annotation.NonNull;
import io.flutter.embedding.engine.plugins.FlutterPlugin;
import io.flutter.embedding.engine.plugins.activity.ActivityAware;
import io.flutter.embedding.engine.plugins.activity.ActivityPluginBinding;
import io.flutter.plugin.common.MethodCall;
import io.flutter.plugin.common.MethodChannel;
import io.flutter.plugin.common.MethodChannel.MethodCallHandler;
import io.flutter.plugin.common.MethodChannel.Result;
import io.flutter.plugin.common.PluginRegistry;
import io.flutter.view.TextureRegistry;

/**
 * FlutterCicadaPlayerPlugin
 *
 * <p>两条通道，职责分开：
 *
 * <ul>
 *   <li>{@code plugins.flutter_cicadaplayer_factory}：本类处理。放"和某个播放器实例无关"的
 *       操作：零拷贝纹理的建/销（createTexture / disposeTexture）、历史遗留的
 *       createCicadaPlayer，以及"挑一个本地视频"（pickLocalVideo）。
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
 *
 * <p>【本轮新增 pickLocalVideo】本类现在同时实现 {@link ActivityAware} 与两个
 * {@link PluginRegistry} 回调接口，用来"申请系统媒体权限 + 打开系统文件选择器"。
 * 为什么要 Activity：请求权限和启动选择器都要一个 Activity，而 {@link FlutterPlugin}
 * 只给引擎；ActivityAware 是 v2 embedding 拿 Activity 的标准方式。
 */
public class FlutterCicadaPlayerPlugin implements FlutterPlugin, MethodCallHandler, ActivityAware,
        PluginRegistry.ActivityResultListener, PluginRegistry.RequestPermissionsResultListener {

    /** 挑本地视频用的 requestCode（权限申请与 startActivityForResult 共用）。 */
    private static final int REQ_PICK_LOCAL_VIDEO = 0x0C1C;

    private FlutterPluginBinding flutterPluginBinding;
    private FlutterCicadaPlayer mFlutterCicadaPlayer;
    private PlayerSurface mPlayerSurface;

    /** 宿主 Activity（ActivityAware 给的），挑视频要用。 */
    private Activity mActivity;
    /** 正在等"挑视频"结果的 Dart 调用；null 表示没有进行中的选择。 */
    private Result mPendingPickResult;

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
                case "pickLocalVideo":
                    // 注意：这一支是**异步**回包的（要等用户授权 / 选完文件），
                    // 所以这里不能顺手 result.success(null) —— 回包在那个流程的末尾。
                    pickLocalVideo(result);
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

    // ======================================================================
    // 挑一个本地视频：申请系统媒体权限 + 打开系统文件选择器
    //
    // 和 Compose 侧（ComposePlayer 的 LocalVideoPicker.kt + LocalMediaAccess）是同一套做法：
    //   * 权限按版本给：Android 14+ 再加 READ_MEDIA_VISUAL_USER_SELECTED（用户在
    //     "仅允许访问选中的照片和视频"那一档也算授权），Android 13 用 READ_MEDIA_VIDEO，
    //     Android 12 及以下用 READ_EXTERNAL_STORAGE；
    //   * 选择器用系统的 ACTION_OPEN_DOCUMENT（MIME 是 video/*），拿回来的是 content:// Uri；
    //   * **不拷贝文件**：content:// 直接交给内核 —— SDK 里有
    //     com.cicada.player.utils.ContentDataSource（native 侧会 FindClass 它）按 Uri 读。
    // ======================================================================

    /** 按系统版本给出"读本地视频"要申请的权限（与 Compose 侧 LocalMediaAccess 一致）。 */
    private static String[] requiredMediaPermissions() {
        if (Build.VERSION.SDK_INT >= 34) {          // Android 14: UPSIDE_DOWN_CAKE
            return new String[] {
                    Manifest.permission.READ_MEDIA_VIDEO,
                    Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED,
            };
        }
        if (Build.VERSION.SDK_INT >= 33) {          // Android 13: TIRAMISU
            return new String[] { Manifest.permission.READ_MEDIA_VIDEO };
        }
        return new String[] { Manifest.permission.READ_EXTERNAL_STORAGE };
    }

    /** 这一组里**任意一个**已授予就算有权限（14+ 的"仅选中"档只给后面那个）。 */
    private boolean hasMediaPermission() {
        Activity activity = mActivity;
        if (activity == null) {
            return false;
        }
        for (String permission : requiredMediaPermissions()) {
            if (activity.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED) {
                return true;
            }
        }
        return false;
    }

    /**
     * Dart 侧 pickLocalVideo 的入口：没权限就先申请，有权限就开选择器。
     * 回包是**异步**的（权限回调 / onActivityResult 里才回），调用方要 await。
     */
    private void pickLocalVideo(Result result) {
        if (mActivity == null) {
            result.error("NO_ACTIVITY",
                    "插件还没拿到 Activity（宿主 Activity 已销毁或尚未挂上），无法打开选择器", null);
            return;
        }
        if (mPendingPickResult != null) {
            result.error("ALREADY_PICKING", "上一次选择还没结束", null);
            return;
        }
        mPendingPickResult = result;
        if (hasMediaPermission()) {
            launchVideoPicker();
        } else {
            // 结果走 onRequestPermissionsResult。
            mActivity.requestPermissions(requiredMediaPermissions(), REQ_PICK_LOCAL_VIDEO);
        }
    }

    private void launchVideoPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("video/*");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try {
            mActivity.startActivityForResult(intent, REQ_PICK_LOCAL_VIDEO);
        } catch (Throwable t) {
            finishPick(null, t);
        }
    }

    /** 结束一次选择：把结果回给 Dart（uri 为 null = 用户取消）。 */
    private void finishPick(Uri uri, Throwable error) {
        Result result = mPendingPickResult;
        mPendingPickResult = null;
        if (result == null) {
            return;
        }
        if (error != null) {
            result.error("PICK_FAILED", "打开系统选择器失败: " + error, null);
        } else {
            result.success(uri != null ? uri.toString() : null);
        }
    }

    @Override
    public boolean onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != REQ_PICK_LOCAL_VIDEO) {
            return false;
        }
        Uri uri = (resultCode == Activity.RESULT_OK && data != null) ? data.getData() : null;
        if (uri != null) {
            // 有的 Provider 支持持久授权（下次进来还能读）；不支持就算了，不当作失败。
            try {
                mActivity.getContentResolver().takePersistableUriPermission(
                        uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (Throwable ignored) {
            }
            android.util.Log.i("FlutterCicadaPlayerPlugin", "挑到本地视频: " + uri);
        }
        finishPick(uri, null);
        return true;
    }

    @Override
    public boolean onRequestPermissionsResult(int requestCode, String[] permissions,
                                             int[] grantResults) {
        if (requestCode != REQ_PICK_LOCAL_VIDEO) {
            return false;
        }
        if (hasMediaPermission()) {
            launchVideoPicker();
        } else {
            Result result = mPendingPickResult;
            mPendingPickResult = null;
            if (result != null) {
                result.error("PERMISSION_DENIED",
                        "没有读取本地视频的权限（请在系统设置里允许本应用读取视频/媒体）", null);
            }
        }
        return true;
    }

    // ------------------------------ ActivityAware ------------------------------

    @Override
    public void onAttachedToActivity(@NonNull ActivityPluginBinding binding) {
        mActivity = binding.getActivity();
        binding.addActivityResultListener(this);
        binding.addRequestPermissionsResultListener(this);
    }

    @Override
    public void onDetachedFromActivityForConfigChanges() {
        detachActivity();
    }

    @Override
    public void onReattachedToActivityForConfigChanges(@NonNull ActivityPluginBinding binding) {
        onAttachedToActivity(binding);
    }

    @Override
    public void onDetachedFromActivity() {
        detachActivity();
    }

    private void detachActivity() {
        mActivity = null;
        // 挂着的 Dart 调用必须如实回掉 —— 否则 await 永远不返回
        // （这个工程刚被"原生不回包"坑过一次，见 FlutterCicadaPlayer.ReplyingResult 的说明）。
        finishPick(null, new IllegalStateException("宿主 Activity 已销毁，选择流程中断"));
    }

    @Override
    public void onDetachedFromEngine(@NonNull FlutterPluginBinding binding) {
        disposeTexture();
        mFlutterCicadaPlayer = null;
        flutterPluginBinding = null;
    }
}
