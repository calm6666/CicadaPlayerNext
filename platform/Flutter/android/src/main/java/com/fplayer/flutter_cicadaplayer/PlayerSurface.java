package com.fplayer.flutter_cicadaplayer;

import android.view.Surface;
import androidx.annotation.NonNull;
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
 *       Surface —— 全程 GPU→GPU，没有 Bitmap、没有 copyPixelsFromBuffer、没有任何 byte[]
 *       中转。
 *   <li>旧实现（FlutterCicadaPlayerView 里塞一个 android.view.TextureView）虽然也让播放器写
 *       SurfaceTexture，但它是**平台视图**：多一层平台视图合成，还要靠 Flutter 的平台视图尺寸
 *       同步，和 Flutter 自己的帧调度对不上。纹理注册表这条路没有平台视图，Dart 侧直接
 *       `Texture(textureId:)`。
 *   <li>SurfaceProducer 而不是已废弃的 createSurfaceTexture：后者每次换帧都要自己挂
 *       SurfaceTexture.OnFrameAvailableListener + markTextureFrameAvailable，而且拿不到
 *       producer 自己管的表面生命周期；SurfaceProducer 会把"GL 上下文重建导致表面失效"这类
 *       事件通过 callback 告诉我们，是 Flutter 3.10+ 的正式做法。
 * </ul>
 *
 * <p>线程约定：{@link #attachPlayer} / {@link #updateVideoSize} / {@link #release} 只从平台线程
 * （UI 线程）调用；{@link TextureRegistry.SurfaceProducer.Callback} 的两个回调由 Flutter 在它
 * 自己的 GL 线程上调用，所以本类里对播放器的操作全部同步在一个方法里做，并只在"Surface 实例
 * 真的换了"时才落到播放器上。
 */
final class PlayerSurface implements TextureRegistry.SurfaceProducer.Callback {

    private final TextureRegistry.SurfaceProducer mProducer;

    private CicadaPlayer mPlayer;
    /** 已经交给播放器的那个 Surface 实例，用来避免同一个实例被重复设置。 */
    private Surface mAttachedSurface;

    PlayerSurface(TextureRegistry.SurfaceProducer producer) {
        mProducer = producer;
    }

    long getTextureId() {
        return mProducer.getId();
    }

    /**
     * 把 producer 的 Surface 交给播放器。
     *
     * <p>为什么可以在这里同步取 Surface：producer 在 createSurfaceProducer 时就已经把底层
     * Surface 建好了（getSurface 的约定是"创建后立刻可用"），不需要等 onSurfaceCreated。这一点
     * 很重要 —— 必须在用户 prepare() 之前就把 Surface 给到播放器，否则第一帧解码出来时
     * BufferQueue 还没有消费者，那一帧直接丢掉，表现为"起播黑一下"。
     */
    void attachPlayer(CicadaPlayer player) {
        mPlayer = player;
        attachProducerSurface();
    }

    /**
     * 取 producer 当前的 Surface 并交给播放器；producer 还没给出 Surface 时什么都不做
     * （onSurfaceCreated 会再来一次）。
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

    @Override
    public void onSurfaceCreated(@NonNull Surface surface) {
        // GL 上下文重建（例如退到后台再回来）后会有新的 Surface，必须重新交给播放器。
        setPlayerSurface(mProducer.getSurface());
    }

    @Override
    public void onSurfaceDestroyed() {
        // producer 自己会在需要时重建 Surface（再看 getSurface 就是新的），这里只是不再引用
        // 旧的。刻意**不**在这里调 setSurface(null)：那会触发播放器重启解码器，而 producer 很
        // 快会给出新 Surface，中间的等待窗口对用户是不可见的。
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
     * ProcessSetViewMsg → RestartVideoDecoder，可能与切档/建解码器并发。平台侧现有代码
     * （platform/Android/ComposePlayer/.../CicadaVideoPlayer.kt:659-668）就是因为这个在
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
