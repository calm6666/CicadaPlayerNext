package com.fplayer.flutter_cicadaplayer;

import android.view.Surface;
import com.cicada.player.CicadaPlayer;
import io.flutter.view.TextureRegistry;

/**
 * 一个播放器对应的一张零拷贝视频纹理。
 *
 * <p>为什么是这条路（而不是原来的 TextureView 平台视图）：
 *
 * <ul>
 *   <li>TextureRegistry.SurfaceProducer 给出的 Surface 后面就是 Flutter 合成器自己用的那张
 *       SurfaceTexture，它的消费者在 Flutter 的 GL 上下文里。播放器把解码结果直接写进这张
 *       Surface —— 全程 GPU 到 GPU，没有 Bitmap、没有 copyPixelsFromBuffer、没有任何 byte[]
 *       中转。
 *   <li>旧实现（FlutterCicadaPlayerView 里塞一个 android.view.TextureView）虽然也让播放器写
 *       SurfaceTexture，但它是**平台视图**：多一层平台视图合成，还要靠 Flutter 的平台视图尺寸
 *       同步，和 Flutter 自己的帧调度对不上。纹理注册表这条路没有平台视图，Dart 侧直接
 *       Texture(textureId:)。
 *   <li>SurfaceProducer 而不是已废弃的 createSurfaceTexture：后者每次换帧都要自己挂
 *       SurfaceTexture.OnFrameAvailableListener 加 markTextureFrameAvailable，而且拿不到
 *       producer 自己管的表面生命周期；SurfaceProducer 会把"GL 上下文重建导致表面失效"这类
 *       事件通过 callback 告诉我们，是 Flutter 3.10+ 的正式做法。
 * </ul>
 *
 * <p>线程约定：{@link #attachPlayer} / {@link #updateVideoSize} / {@link #release} 只从平台线程
 * （UI 线程）调用；SurfaceProducer.Callback 的回调由 Flutter 在它自己的 GL 线程上调用，
 * 所以本类里对播放器的操作全部同步在一个方法里做，并只在"Surface 实例真的换了"时才落到
 * 播放器上。
 *
 * <p>【本轮修：三处按真实 API 改正，此前从未编译过】
 * 本文件是随"Flutter 零拷贝"一起写的，而 Flutter Android 那条路一直卡在构建上（缺 aar、
 * Gradle 环境坏），所以它**一次都没被编译过**。真正编起来暴露了三处：
 *
 * <ol>
 *   <li>{@code mProducer.getId()} —— TextureRegistry.TextureEntry 上的方法是
 *       {@code long id()}，没有 getId()。编译报"找不到符号 方法 getId()"。
 *   <li>{@code @Override public void onSurfaceCreated(Surface)} —— 回调接口里的
 *       onSurfaceCreated() **不带参数**，而带 Surface 参数的那个重载根本不存在，于是
 *       {@code @Override} 报"方法不会覆盖或实现超类型的方法"。
 *   <li>更要紧的是**没人注册这个 callback**：类实现了 Callback，但创建它的地方
 *       （FlutterCicadaPlayerPlugin.createTexture）从来没调过 producer.setCallback(this)，
 *       于是"GL 上下文重建"这类事件永远不会通知到这里 —— 退到后台再回来画面就可能是死的。
 *       本轮在构造函数里补上 setCallback(this)。
 * </ol>
 *
 * <p>回调名按**当前** API 取：SurfaceProducer.Callback 里有四个 default 方法，
 * onSurfaceCreated 与 onSurfaceDestroyed 已被标记为待删除（编译会出 [removal] 警告），
 * 现在应当用 onSurfaceAvailable 与 onSurfaceCleanup。本工程要求零警告，所以只覆写后者两个。
 */
final class PlayerSurface implements TextureRegistry.SurfaceProducer.Callback {

    private final TextureRegistry.SurfaceProducer mProducer;

    private CicadaPlayer mPlayer;
    /** 已经交给播放器的那个 Surface 实例，用来避免同一个实例被重复设置。 */
    private Surface mAttachedSurface;

    PlayerSurface(TextureRegistry.SurfaceProducer producer) {
        mProducer = producer;
        // 必须注册，否则下面两个回调永远不会被调用（见类注释第 3 条）。
        mProducer.setCallback(this);
    }

    long getTextureId() {
        // TextureEntry.id()（不是 getId()）。
        return mProducer.id();
    }

    /**
     * 把 producer 的 Surface 交给播放器。
     *
     * <p>为什么可以在这里同步取 Surface：producer 在 createSurfaceProducer 时就已经把底层
     * Surface 建好了（getSurface 的约定是"创建后立刻可用"），不需要等回调。这一点很重要 ——
     * 必须在用户 prepare() 之前就把 Surface 给到播放器，否则第一帧解码出来时 BufferQueue
     * 还没有消费者，那一帧直接丢掉，表现为"起播黑一下"。
     */
    void attachPlayer(CicadaPlayer player) {
        mPlayer = player;
        attachProducerSurface();
    }

    /**
     * 取 producer 当前的 Surface 并交给播放器；producer 还没给出 Surface 时什么都不做
     * （onSurfaceAvailable 会再来一次）。
     */
    private void attachProducerSurface() {
        Surface surface = mProducer.getSurface();
        if (surface != null) {
            setPlayerSurface(surface);
        }
    }

    /**
     * 视频尺寸变化时告诉 producer。为什么必须报：producer 默认缓冲尺寸是 0×0，Flutter 会按它
     * 建纹理；不给尺寸的话纹理尺寸和实际帧对不上。这里是解码器给出的**像素**尺寸，正是
     * SurfaceProducer.setSize 期望的量纲（Dart 侧的 width/height 是逻辑像素，不能用）。
     */
    void updateVideoSize(int width, int height) {
        if (width <= 0 || height <= 0) {
            return;
        }
        mProducer.setSize(width, height);
    }

    /**
     * producer 又有一张可用的 Surface 了（首次创建、或 GL 上下文重建之后）。
     * 这时必须重新交给播放器，否则画面停在旧表面上。
     */
    @Override
    public void onSurfaceAvailable() {
        attachProducerSurface();
    }

    /**
     * 当前 Surface 即将被清理。刻意**不**在这里调 mPlayer.setSurface(null)：那会触发播放器
     * 重启解码器，而 producer 很快会给出新 Surface，中间的等待窗口对用户不可见。
     * 这里只是不再引用旧的，等 onSurfaceAvailable 拿新的重设。
     */
    @Override
    public void onSurfaceCleanup() {
        mAttachedSurface = null;
    }

    void release() {
        if (mPlayer != null) {
            mPlayer.setSurface(null);
            mPlayer = null;
        }
        mAttachedSurface = null;
        mProducer.release();
    }

    /**
     * 只在"Surface 实例真的换了"时才 setSurface。
     *
     * <p>为什么加这个判断：播放器收到 setSurface 会走
     * ProcessSetViewMsg，进而 RestartVideoDecoder，可能与切档/建解码器并发。平台侧现有代码
     * （platform/Android/ComposePlayer 下的 CicadaVideoPlayer.kt）就是因为这个在
     * surfaceChanged 里刻意不重设 Surface 的。
     */
    private void setPlayerSurface(Surface surface) {
        if (mPlayer == null || surface == null || surface == mAttachedSurface) {
            return;
        }
        mAttachedSurface = surface;
        mPlayer.setSurface(surface);
        mPlayer.redraw();
    }
}
