package com.cicada.player.utils.media;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCrypto;
import android.media.MediaFormat;
import android.graphics.SurfaceTexture;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.text.TextUtils;
import android.util.SparseArray;
import android.view.Surface;

import com.cicada.player.utils.Logger;
import com.cicada.player.utils.NativeUsed;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;

import static android.media.MediaCodec.BUFFER_FLAG_CODEC_CONFIG;
import static android.media.MediaCodec.BUFFER_FLAG_END_OF_STREAM;
import static android.media.MediaCodec.INFO_OUTPUT_BUFFERS_CHANGED;
import static android.media.MediaCodec.INFO_OUTPUT_FORMAT_CHANGED;
import static android.media.MediaCodec.INFO_TRY_AGAIN_LATER;

@NativeUsed
public class MediaCodecDecoder {
    private static final String TAG = MediaCodecDecoder.class.getSimpleName();

    private static final Object queLock = new Object();

    private static final int ERROR = -1;
    private static final int TRY_AGAIN = -11;

    private static int CODEC_CATEGORY_VIDEO = 0;
    private static int CODEC_CATEGORY_AUDIO = 1;

    private Map<String, byte[]> mCodecSpecificDataMap = new LinkedHashMap<>();

    private String mMime;
    private int mCodecCateGory = CODEC_CATEGORY_VIDEO;

    private MediaCodec mMediaCodec = null;

    // 对齐 ExoPlayer 2.9.6 DummySurface（SurfaceTexture 支撑的 1x1 dummy
    // surface）：surface 被销毁（切后台）时把 codec 输出切到这里，codec 保持
    // 运行、帧被静默丢弃；回前台再用 setOutputSurface 切回真实 surface，
    // 无需重建解码器（参考 MediaCodecVideoRenderer.setSurface / DummySurface）
    private Surface mDummySurface = null;
    private SurfaceTexture mDummySurfaceTexture = null;
    private MediaCrypto mediaCrypto = null;

    // 异步回调模式（对齐 ExoPlayer AsynchronousMediaCodecAdapter）：
    // 缓冲区就绪事件由回调投递到队列，dequeue 轮询改为条件变量等待，
    // 消除定时轮询对厂商 codec looper 的反复唤醒
    private final ArrayList<Integer> mInputIndices = new ArrayList<>();
    private final ArrayList<Integer> mOutputIndices = new ArrayList<>();
    private final SparseArray<MediaCodec.BufferInfo> mOutputBufferInfos = new SparseArray<>();
    private final Object mInputLock = new Object();
    private final Object mOutputLock = new Object();
    private HandlerThread mCallbackThread;
    private boolean mAsyncFirstInputLogged = false;

    // 异步回调模式（对齐 ExoPlayer AsynchronousMediaCodecAdapter）：注册必须
    // 在 MediaCodec.start() 之前（见 start()）。个别 codec 回调不送达时运行
    // 时检测到会自动回退轮询（sAsyncBroken）
    private static final boolean ASYNC_ENABLED = true;
    private static boolean sAsyncBroken = false;
    private boolean mAsyncMode = false;
    private int mInputWaitCount = 0;

    /*
     * ============ post-flush 输入回调缺失的检测（2026-09-24）============
     *
     * 背景（真机 pause.log）：seek 会走 flush 路径。flush() 里 mInputIndices
     * 被清空，而**唯一**的重填路径是平台的 onInputBufferAvailable 回调。
     * 一旦这个回调在 flush 之后不再送达，输入侧就永久拿不到缓冲：
     * 日志表现为 `codec has had no input buffer for about 1 s` 之后画面永不恢复，
     * 而旧代码里没有任何东西接住它 —— 见下面 dequeueInputBufferIndex() 里
     * 原来那道被 !mAsyncFirstInputLogged 永久关闭的保险。
     *
     * mFlushCompletedMs：最近一次 flush() 返回的时刻（单调毫秒）。0 表示还没 flush 过。
     * mInputCallbackAfterFlushMs：flush 之后**第一个**输入回调到达的时刻；0 表示还没到。
     *   这两个值一起就能回答"flush 之后过 M 毫秒还没有任何回调"。
     *
     * 【单调钟】用 SystemClock.uptimeMillis() 而不是 System.currentTimeMillis()：
     * 后者会被系统时间调整影响（本机日志里就出现过 `Time changed in` 一条），
     * 拿它算时间差可能得到负数，让判据永远不成立。
     *
     * 【语义说明，必须写下来】平台在 flush 之后**通常**会把全部输入缓冲重新
     * 回调一遍（ACodec 在 flush 完成时把 owned buffers 交回），但这属于平台
     * 行为、不是 API 契约：极端情况下（厂商 codec 行为差异）它不保证一定发生。
     * 所以这里不是"确认它一定会回调"，而是"如果它没回调，我们必须能发现并降级"。
     */
    private long mFlushCompletedMs = 0;
    private long mInputCallbackAfterFlushMs = 0;

    /*
     * flush 之后判定"输入回调没来"的宽限（毫秒）。
     * 1000ms 的依据与 C++ 侧 ActiveDecoder 的观测量一致：解码器在拿不到输入缓冲时
     * 约 1 秒会打一条 `codec has had no input buffer about 1 s`，而正常 seek 后
     * 首个输入回调是**毫秒级**（codec 一完成 flush 就把缓冲交回来）。所以 1 秒
     * 已经远宽于正常情况，不会误判。
     */
    private static final long FLUSH_INPUT_CALLBACK_GRACE_MS = 1000;

    /*
     * 输入侧"持续拿不到缓冲"的确认阈值（次）。每次 dequeueInputBufferIndex()
     * 的超时是 10ms（C++ 侧传 10000us），所以 100 次 ≈ 1 秒 —— 与现有的
     * `codec has had no input buffer about 1 s` 观测点同量级。
     */
    private static final int INPUT_STARVATION_CONFIRM_COUNT = 100;

    public MediaCodecDecoder() {
    }

    @NativeUsed
    public void setCodecSpecificData(Object datas) {
        Logger.d(TAG, "--> setCodecSpecificData datas " + datas);
        mCodecSpecificDataMap.clear();
        if (datas == null) {
            return;
        }

        mCodecSpecificDataMap.putAll((LinkedHashMap<? extends String, ? extends byte[]>) datas);
    }

    @NativeUsed
    public boolean setDrmInfo(String uuid, byte[] sessionId) {
        Logger.d(TAG, "--> setDrmInfo uuid " + uuid);
        try {
            UUID drmUUID = UUID.fromString(uuid);
            mediaCrypto = new MediaCrypto(drmUUID, sessionId);
        } catch (Exception e) {
            Logger.e(TAG, "createMediaCrypto failed: " + e.getMessage());
            return false;
        }
        return true;
    }

    private boolean forceInsecureDecoder = false;

    @NativeUsed
    public void setForceInsecureDecoder(boolean force) {
        Logger.d(TAG, "--> setForceInsecureDecoder  " + force);
        forceInsecureDecoder = force;
    }

    /*
     * 追帧窗口的性能点：240 帧/秒量级 = "尽快解"（seek 落点前缀 / 切档预滚期间用）。
     * 退出时回 -1，语义等同 MediaCodecInfo.CodecCapabilities.OPERATING_RATE_UNSPECIFIED
     * —— ExoPlayer 的 getCodecOperatingRateV23 用的就是 -1（"不指定"）。
     */
    private static final int OPERATING_RATE_BOOST = 240;
    private static final int OPERATING_RATE_UNSPECIFIED = -1;

    /*
     * 【追帧加速】seek / 切档预滚窗口里把 codec 提到更高的性能点。
     *
     * 只下发一个"性能点"提示：key 就是 MediaFormat.KEY_OPERATING_RATE 的字符串
     * （这里写字面量而不是引用那个常量，是为了不引入 API 23 的 InlinedApi 提示）。
     *
     * 关键性质：
     *   · **不丢帧、不改时间轴** —— seek 之后"从关键帧解到目标"的那段前缀帧照常
     *     全部解出来，精度不会因为这里少一帧或多一帧（丢帧与否由内核的落点判据决定）；
     *   · 它只是让 codec 跑在更高的性能点，把本就要解的那段内容更快吐出来；
     *   · 个别厂商 codec 不认这个 key 会抛异常 —— 全部吞掉只记一条日志，绝不影响解码。
     */
    @NativeUsed
    public void setDecodeBoost(boolean boost) {
        MediaCodec codec = mMediaCodec;

        if (codec == null) {
            return;
        }

        try {
            Bundle params = new Bundle();
            params.putInt("operating-rate", boost ? OPERATING_RATE_BOOST : OPERATING_RATE_UNSPECIFIED);
            codec.setParameters(params);
            Logger.d(TAG, "--> setDecodeBoost " + boost);
        } catch (Throwable t) {
            Logger.w(TAG, "setDecodeBoost(" + boost + ") is not supported by this codec: " + t);
        }
    }

    @NativeUsed
    public int configureVideo(String mime, int width, int height, int angle, Surface surface,
                              boolean usePlaceholderSurface) {
        Logger.d(TAG, "--> configureVideo start " + mime + ", " + width + ", " + height + ", " + surface
                + ", usePlaceholderSurface = " + usePlaceholderSurface);
        mCodecCateGory = CODEC_CATEGORY_VIDEO;
        mMime = mime;

        MediaFormat videoFormat = MediaFormat.createVideoFormat(mime, width, height);
        addCsdInfo(videoFormat);
        if (angle != 0) {
            videoFormat.setInteger("rotation-degrees", angle);
        }

        String codecName = findDecoderName(videoFormat);
        if (TextUtils.isEmpty(codecName)) {
            Logger.e(TAG, "not found video codec : " + mime);
            return -12;
        }

        try {
            mMediaCodec = MediaCodec.createByCodecName(codecName);
        } catch (IOException e) {
        }

        if (mMediaCodec == null) {
            Logger.e(TAG, "createByCodecName fail : " + codecName);
            return -13;
        }

        /*
         * B2（placeholder / 占位 Surface 交接）：tunnel（渲染器带 FLAG_DUMMY）下
         * pending（切档目标）解码器不能绑真 Surface —— 一个 Surface 同时只允许
         * 一个 MediaCodec 连接（平台日志：already connected / err -22），而 active
         * 解码器正连着它。于是 pending 用一块 1x1 的占位 Surface 配置：它从此以
         * surface 模式运行，提交时由内核先让旧 codec 让出真 Surface，再把这块
         * codec 接上去（失败可原地回滚）。
         *
         * usePlaceholderSurface 为假（或占位 Surface 创建失败）时保持老行为：
         * 没有 surface 就 configure(null)，即 ByteBuffer 模式。
         */
        Surface target = (surface instanceof Surface) ? (Surface) surface : null;
        if (target == null && usePlaceholderSurface) {
            target = ensureDummySurface();
            if (target == null) {
                Logger.w(TAG, "[codec-placeholder] dummy surface unavailable — falling back to ByteBuffer mode");
            }
        }
        /*
         * 打一条"占位分支到底走没走"的证据（INFO 级：默认日志等级下 DEBUG 不出现）。
         */
        if (usePlaceholderSurface) {
            Logger.i(TAG, "[codec-placeholder] configureVideo placeholder=" + (target != null)
                    + " (dummy surface used for surface-mode configure)");
        }
        try {
            mMediaCodec.configure(videoFormat, target, mediaCrypto, 0);
        } catch (Exception e) {
            Logger.e(TAG, "configure fail : " + e.getMessage());
            return -14;
        }

        return 0;
    }

    private String findDecoderName(MediaFormat videoFormat) {
        boolean needSecureDecoder = false;
        if (mediaCrypto != null) {
            needSecureDecoder = forceInsecureDecoder || mediaCrypto.requiresSecureDecoderComponent(mMime);
        }

        String codecName = getDecoderName(videoFormat, needSecureDecoder);

        Logger.d(TAG, "findDecoderName : " + codecName + " , secure = " + needSecureDecoder);
        return codecName;
    }

    private String getDecoderName(MediaFormat videoFormat, boolean needSecureDecoder) {
        List<MediaCodecInfo> mediaCodecInfoList = MediaCodecUtils.getCodecInfos(mMime, needSecureDecoder, videoFormat);
        String codecName = getNotBlackCodecName(mediaCodecInfoList);

        if (needSecureDecoder && TextUtils.isEmpty(codecName) && !mediaCodecInfoList.isEmpty()) {
            codecName = mediaCodecInfoList.get(0).getName();
        }

        return codecName;
    }

    private String getNotBlackCodecName(List<MediaCodecInfo> mediaCodecInfoList) {
        String codecName = null;
        if (!mediaCodecInfoList.isEmpty()) {
            for (MediaCodecInfo info : mediaCodecInfoList) {
                if (!isBlackCodec(info)) {
                    codecName = info.getName();
                    break;
                }
            }
        }
        return codecName;
    }

    @NativeUsed
    public int configureAudio(String mime, int sampleRate, int channelCount, int isADTS) {
        Logger.d(TAG, "--> configureAudio start " + mime + sampleRate + " , " + channelCount);
        mCodecCateGory = CODEC_CATEGORY_AUDIO;
        mMime = mime;

        MediaFormat audioFormat = MediaFormat.createAudioFormat(mime, sampleRate, channelCount);
        audioFormat.setInteger(MediaFormat.KEY_IS_ADTS, isADTS);
        addCsdInfo(audioFormat);

        String codecName = findDecoderName(audioFormat);
        if (TextUtils.isEmpty(codecName)) {
            Logger.e(TAG, "not found codec : " + mime);
            return -12;
        }

        try {
            mMediaCodec = MediaCodec.createByCodecName(codecName);
        } catch (IOException e) {
        }

        if (mMediaCodec == null) {
            Logger.e(TAG, "createByCodecName fail : " + codecName);
            return -13;
        }

        try {
            mMediaCodec.configure(audioFormat, null, mediaCrypto, 0);
        } catch (Exception e) {
            Logger.e(TAG, "configure fail : " + e.getMessage());
            return -14;
        }

        return 0;
    }

    private void addCsdInfo(MediaFormat format) {
        if (mCodecSpecificDataMap.isEmpty()) {
            return;
        }

        for (String key : mCodecSpecificDataMap.keySet()) {
            byte[] data = mCodecSpecificDataMap.get(key);
            if (data == null) {
                continue;
            }

            ByteBuffer csd = ByteBuffer.allocate(data.length);
            csd.put(data);
            csd.flip();
            format.setByteBuffer(key, csd);
        }
    }

    private ByteBuffer[] mInputBuffers = null;
    private ByteBuffer[] mOutputBuffers = null;
    private MediaCodec.BufferInfo mBufferInfo = null;

    private boolean started = false;

    /*
     * 占位（1x1）Surface 的唯一创建入口：configureVideo 的 B2 路径与
     * setOutputSurface 的"surface 被销毁（切后台）"路径共用这一块。
     * 抽出来的理由：一块占位 Surface 只应存在一份 —— 两处各建一份不仅浪费，
     * 而且一旦被两个 codec 同时连接就会撞上平台硬约束 already connected。
     * 内部自己 try/catch，失败返回 null，由调用方决定兜底（configure 退
     * ByteBuffer 模式、setOutputSurface 返回 -1）。
     * 必须与 configure / setOutputSurface 在同一线程语义下调用（现有调用方式不变）。
     */
    private Surface ensureDummySurface() {
        if (mDummySurface == null) {
            try {
                mDummySurfaceTexture = new SurfaceTexture(0);
                mDummySurface = new Surface(mDummySurfaceTexture);
            } catch (Exception e) {
                Logger.e(TAG, "create dummy surface fail " + e.getMessage());
                return null;
            }
        }
        return mDummySurface;
    }

    @NativeUsed
    public int setOutputSurface(Object surface) {
        if (mMediaCodec == null) {
            return -1;
        }
        Surface target = (surface instanceof Surface) ? (Surface) surface : null;
        if (target == null) {
            // 对齐 ExoPlayer 2.9.6 MediaCodecVideoRenderer.setSurface():
            // surface 为 null（被销毁）时换成内部 DummySurface，codec 不释放，
            // 后台期间输出帧被静默丢弃，回前台再切回真实 surface
            target = ensureDummySurface();
            if (target == null) {
                return -1;
            }
        }
        try {
            // surface 重建（前后台切换）后热重绑输出，避免隧道直通模式黑屏
            mMediaCodec.setOutputSurface(target);
            return 0;
        } catch (Exception e) {
            Logger.e(TAG, "setOutputSurface fail " + e.getMessage());
            return -2;
        }
    }

    @NativeUsed
    public int start() {
        Logger.d(TAG, "--> start ");

        if (mMediaCodec == null) {
            Logger.e(TAG, "mMediaCodec  null ");
            return ERROR;
        }

        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.LOLLIPOP) {
            try {
                mInputBuffers = mMediaCodec.getInputBuffers();
            } catch (Exception e) {
                Logger.e(TAG, "getInputBuffers  fail : " + e.getMessage());
            }

            try {
                mOutputBuffers = mMediaCodec.getOutputBuffers();
            } catch (Exception e) {
                Logger.e(TAG, "getOutputBuffers  fail : " + e.getMessage());
            }
        }

        mBufferInfo = new MediaCodec.BufferInfo();

        /*
         * 【2026-09-24】每次新建解码器都把这些实例级状态清一遍。
         *
         * start() 对每个实例只成功走一次，但"重建解码器"在本工程里是常态
         * （错误驱动重建、清晰度切换、seek 恢复都会重建），而复用的是新的
         * Java 实例 —— 清一遍可以保证判据只依据**本实例**的证据，不会被
         * 上一个实例的残留影响。
         *
         * sAsyncBroken 是 static：一旦被标记，之后新建的实例直接走轮询
         * （下面的 if 会跳过 setCallback），这正是"降级留给下一个实例"的实现。
         */
        mInputWaitCount = 0;
        mAsyncFirstInputLogged = false;
        mFlushCompletedMs = 0;
        mInputCallbackAfterFlushMs = 0;

        // 异步回调模式：必须在 MediaCodec.start() 之前注册 ——
        // start() 之后 codec 立刻回调 onInputBufferAvailable，注册晚了会丢失
        // 首批回调，输入队列永远为空（此前误判为"设备不支持异步"）。
        // 保留 1 秒超时兜底：真遇到回调不送达的 codec 自动回退轮询。
        if (ASYNC_ENABLED && !sAsyncBroken) {
            try {
                mCallbackThread = new HandlerThread("MediaCodecCallback");
                mCallbackThread.start();
                Handler callbackHandler = new Handler(mCallbackThread.getLooper());
                mMediaCodec.setCallback(new MediaCodec.Callback() {
                    @Override
                    public void onInputBufferAvailable(MediaCodec codec, int index) {
                        synchronized (mInputLock) {
                            if (!mAsyncFirstInputLogged) {
                                mAsyncFirstInputLogged = true;
                                Logger.i(TAG, "async first input callback arrived");
                            }
                            /*
                             * 记下"flush 之后的第一个输入回调"。
                             * 这是 flush 恢复能力的唯一证据：只要它到了，就说明
                             * 平台在 flush 后确实会重新交付输入缓冲；如果它一直
                             * 不到，dequeueInputBufferIndex() 会据此判定异步不可用。
                             */
                            if (mInputCallbackAfterFlushMs == 0) {
                                mInputCallbackAfterFlushMs = SystemClock.uptimeMillis();
                            }
                            // flush 竞态下同一 index 可能回调两次，去重防重复投喂
                            if (!mInputIndices.contains(index)) {
                                mInputIndices.add(index);
                            }
                            mInputLock.notifyAll();
                        }
                    }

                    @Override
                    public void onOutputBufferAvailable(MediaCodec codec, int index, MediaCodec.BufferInfo info) {
                        synchronized (mOutputLock) {
                            // 同一 index 未释放前不会收到第二次有效回调；出现即为
                            // flush 竞态的过期回调，去重跳过
                            if (!mOutputIndices.contains(index)) {
                                mOutputBufferInfos.put(index, info);
                                mOutputIndices.add(index);
                            }
                            mOutputLock.notifyAll();
                        }
                    }

                    @Override
                    public void onError(MediaCodec codec, MediaCodec.CodecException e) {
                        Logger.e(TAG, "async onError " + e);
                    }

                    @Override
                    public void onOutputFormatChanged(MediaCodec codec, MediaFormat format) {
                        synchronized (mOutputLock) {
                            mOutputIndices.add(INFO_OUTPUT_FORMAT_CHANGED);
                            mOutputLock.notifyAll();
                        }
                    }
                }, callbackHandler);
                mAsyncMode = true;
                Logger.i(TAG, "async callback mode enabled");
            } catch (Exception e) {
                Logger.e(TAG, "setCallback fail " + e.getMessage());
                mAsyncMode = false;
            }
        } else {
            if (sAsyncBroken) {
                Logger.w(TAG, "async mode disabled (previous input callback timeout), fallback to polling");
            }
            mAsyncMode = false;
        }

        try {
            mMediaCodec.start();
            started = true;
        } catch (Exception e) {
            Logger.e(TAG, mMediaCodec.getName() + " start fail : " + e.getMessage());
            return ERROR;
        }

        return 0;
    }

    @NativeUsed
    public int flush() {
        Logger.d(TAG, "--> flush start");

        if (mMediaCodec == null) {
            Logger.e(TAG, "mMediaCodec  null ");
            return ERROR;
        }
        try {
            mMediaCodec.flush();
        } catch (Exception e) {
            Logger.e(TAG, "flush  fail " + e.getMessage());
        }

        if (mAsyncMode) {
            synchronized (mInputLock) {
                mInputIndices.clear();
                /*
                 * 【2026-09-24】flush 之后输入侧的等待计数与"回调是否来过"必须重置。
                 *
                 * 不重置会有两个后果：
                 *   1) mInputWaitCount 沿用 flush **之前**的旧计数，于是新的
                 *      "持续拿不到缓冲"判据可能在 flush 后一两轮就立刻成立 —— 那是
                 *      拿旧证据做新判断（flush 前正常播放期间也会短暂堆积计数）；
                 *   2) mInputCallbackAfterFlushMs 若残留上一次的值，就永远无法
                 *      回答"这一次 flush 之后到底有没有回调"。
                 * 重置之后，"flush 后 M 毫秒内没有任何输入回调"才是干净的判据。
                 */
                mInputWaitCount = 0;
                mInputCallbackAfterFlushMs = 0;
                /*
                 * 时间戳放在 flush() **返回之后**记：这一句在 mMediaCodec.flush()
                 * 已经返回之后执行，所以它标记的是"flush 完成时刻"，宽限期从这里算。
                 */
                mFlushCompletedMs = SystemClock.uptimeMillis();
            }
            synchronized (mOutputLock) {
                /*
                 * 清空输出缓冲跟踪。语义上这也是**正确**的：flush 会把 codec
                 * 持有的输出缓冲全部收回（ACodec 在 flush 完成时把 owned buffers
                 * 交回），所以"此刻没有任何输出缓冲被播放器占住"。
                 * 正因如此，判据 (b) 里"输出侧没被占住 + 输入侧持续拿不到"
                 * 在 flush 之后才是一个有意义的组合。
                 */
                mOutputIndices.clear();
                mOutputBufferInfos.clear();
            }
        } else {
            /* 轮询模式没有回调队列，但计数也要清，避免跨 flush 累积。 */
            mInputWaitCount = 0;
        }

        return 0;
    }

    public int stop() {
        Logger.d(TAG, "--> stop start");

        if (mMediaCodec == null) {
            return ERROR;
        }

        if (started) {
            try {
                mMediaCodec.stop();
            } catch (Exception e) {
                Logger.e(TAG, "stop fail " + e.getMessage());
                return ERROR;
            }
        }
        return 0;
    }

    @NativeUsed
    public int release() {
        Logger.d(TAG, "--> release ");

        if (mMediaCodec == null) {
            return ERROR;
        }

        if (mCallbackThread != null) {
            mCallbackThread.quitSafely();
            mCallbackThread = null;
        }
        mAsyncMode = false;

        mMediaCodec.release();
        mMediaCodec = null;

        if (mDummySurface != null) {
            mDummySurface.release();
            mDummySurface = null;
        }
        if (mDummySurfaceTexture != null) {
            mDummySurfaceTexture.release();
            mDummySurfaceTexture = null;
        }

        if (mediaCrypto != null) {
            mediaCrypto.release();
        }

        return 0;
    }

    @NativeUsed
    public int releaseOutputBuffer(int index, boolean render) {

        if (mMediaCodec == null) {
            return ERROR;
        }

        if (mAsyncMode) {
            // release 后该 index 可能被 codec 立即复用并回调新数据，先移除旧 BufferInfo
            synchronized (mOutputLock) {
                mOutputBufferInfos.remove(index);
            }
        }

        try {
            mMediaCodec.releaseOutputBuffer(index, render);
        } catch (Exception e) {
            Logger.e(TAG, "releaseOutputBuffer fail " + e.getMessage());
            return ERROR;
        }
        return 0;
    }

    @NativeUsed
    public int dequeueInputBufferIndex(long timeoutUs) {

        if (mMediaCodec == null) {
            return ERROR;
        }

        if (mAsyncMode) {
            synchronized (mInputLock) {
                if (mInputIndices.isEmpty()) {
                    try {
                        mInputLock.wait(timeoutUs / 1000, (int) ((timeoutUs % 1000) * 1000));
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                    }
                }
                if (!mInputIndices.isEmpty()) {
                    mInputWaitCount = 0;
                    return mInputIndices.remove(0);
                }

                // 本实例绝不把取输入的泵切到同步 API。
                // MediaCodec 的异步模式是单向的：一旦 setCallback()，同步的
                // dequeueInputBuffer 与 dequeueOutputBuffer 一律抛
                // IllegalStateException。以前这里把 mAsyncMode 置 false 当成
                // "回退轮询"，可平台 codec 仍在异步模式，于是此后每一次取输入、
                // 取输出都失败，解码器彻底僵死；而 C++ 解码线程对任何非 -EAGAIN
                // 的返回值都会把当前包丢掉并继续取下一包，一次故障就以每秒数百包
                // 的速度把视频包队列吃空 —— 音频时钟不动（暂停中）时队列队首凭空
                // 前进约 40 秒（2026-09-23 22:32 日志：errorFrames=1001，25fps 即
                // 40.0 秒，与实测超前 43996 ms 对得上），恢复后每一帧都被判成
                // "太早"而不上屏：画面永久冻住、声音正常、位置照走。
                // 所以这里【只】回报 TRY_AGAIN 并做标记，绝不改 mAsyncMode、
                // 绝不调用同步 API —— 降级动作一律留给**下一个**解码器实例
                // （start() 会读 sAsyncBroken 决定是否 setCallback）。
                //
                // 【2026-09-24 修：下面这道保险以前是死的】
                //
                // 原判据是 `!mAsyncFirstInputLogged && mInputWaitCount > 100`。
                // 而 mAsyncFirstInputLogged 在**起播第一个输入回调**时就置真
                // （见 onInputBufferAvailable），所以健康会话里它永远是 true ——
                // 这道保险从起播那一刻起就永久关闭，之后再严重的输入侧停摆
                // （典型：seek/flush 之后回调不再送达）都不会被记录、不会有降级。
                // 真机日志 `pause.log` 里 `async first input callback arrived`
                // 确实出现过，因此那次事故中它一定是关着的。
                //
                // 新判据不再依赖"是否曾经收到过回调"，而是要求**确实等不到回调、
                // 且不是正常背压**，两条独立证据任一成立即可：
                //
                //   (a) flush 之后过了宽限期，输入侧一个回调都没到。
                //       这是最直接的命中：flush() 清空了 mInputIndices，而重填
                //       **唯一**靠 onInputBufferAvailable；只要它不来，输入侧就
                //       永久空转。这条判据不需要任何其他状态，最稳。
                //
                //   (b) 持续 ~1 秒拿不到输入缓冲，**同时输出侧没有任何缓冲被占住**。
                //       正常的"拿不到输入缓冲"几乎都是背压：codec 的输出缓冲
                //       还被播放器握着，它自然不会请求新输入。此时 mOutputIndices
                //       非空。反过来说，输出侧一个都没占（mOutputIndices 为空）
                //       却仍然 1 秒不给输入缓冲，就不是背压，而是异步通路本身坏了。
                //
                // 标记 sAsyncBroken 的效果：**之后新建**的解码器实例直接走轮询
                // （start() 里 `ASYNC_ENABLED && !sAsyncBroken`）。内核侧
                // rebuildVideoDecoder() 重建解码器时走的就是这条 start()。
                mInputWaitCount++;

                final long nowMs = SystemClock.uptimeMillis();
                final boolean flushCallbackMissing =
                        mFlushCompletedMs > 0 && mInputCallbackAfterFlushMs == 0 &&
                        (nowMs - mFlushCompletedMs) >= FLUSH_INPUT_CALLBACK_GRACE_MS;
                /*
                 * (b) 里的"输出侧没有被占住"要在 mOutputLock 下读，避免与回调线程
                 * 竞争。这里可以安全地嵌套加锁：onInputBufferAvailable 只拿
                 * mInputLock，releaseOutputBuffer/onOutputBufferAvailable 只拿
                 * mOutputLock，两把锁之间没有反向获取路径，不存在死锁。
                 */
                boolean noOutputHeld = false;
                synchronized (mOutputLock) {
                    noOutputHeld = mOutputIndices.isEmpty();
                }
                final boolean starvedWithoutBackpressure =
                        mInputWaitCount >= INPUT_STARVATION_CONFIRM_COUNT && noOutputHeld;

                if (!sAsyncBroken && (flushCallbackMissing || starvedWithoutBackpressure)) {
                    /*
                     * 只打一条 WARN 说明"命中了哪个判据 + 下一个解码器走什么模式"。
                     * 不在这里做任何动作 —— 当前实例仍是异步模式，同步 API 会抛
                     * IllegalStateException，那正是历史上把解码器搞僵的路径。
                     */
                    Logger.w(TAG, "async input path looks dead (criterion="
                            + (flushCallbackMissing ? "no-input-callback-after-flush" : "starved-without-backpressure")
                            + ", waitedMs=" + (mFlushCompletedMs > 0 ? (nowMs - mFlushCompletedMs) : -1)
                            + ", mInputWaitCount=" + mInputWaitCount
                            + ", noOutputHeld=" + noOutputHeld
                            + "): keeping this instance asynchronous (never switching the pump to the "
                            + "synchronous API) and marking the async path broken so the NEXT decoder "
                            + "created by the native rebuild will use polling");
                    sAsyncBroken = true;
                    mInputWaitCount = 0;
                }
                return TRY_AGAIN;
            }
        }

        try {
            int index = mMediaCodec.dequeueInputBuffer(timeoutUs);
            if (index >= 0) {
                return index;
            } else {
                return TRY_AGAIN;
            }
        } catch (Exception e) {
            Logger.e(TAG, "dequeueInputBufferIndex fail " + e.getMessage());
            return ERROR;
        }

    }

    @NativeUsed
    public int queueInputBuffer(int index, byte[] buffer, long pts, boolean isConfig) {
        return queueInputBufferInner(index, buffer, pts, isConfig, false, null);
    }

    @NativeUsed
    public int queueSecureInputBuffer(int index, byte[] buffer, Object encryptionInfo, long pts, boolean isConfig) {
        return queueInputBufferInner(index, buffer, pts, isConfig, true, encryptionInfo);
    }

    private int queueInputBufferInner(int index, byte[] buffer, long pts, boolean isConfig, boolean secure, Object encryptionInfo) {

        if (mMediaCodec == null) {
            return ERROR;
        }

        ByteBuffer inputBuffer = null;
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
                inputBuffer = mMediaCodec.getInputBuffer(index);
            } else {
                inputBuffer = mInputBuffers[index];
            }
        }catch (Exception e){
            Logger.e(TAG, "getInputBuffer fail " + e.getMessage());
        }

        if (inputBuffer == null) {
            return ERROR;
        }

        inputBuffer.clear();

        if (buffer != null) {
            inputBuffer.put(buffer, 0, buffer.length);
            inputBuffer.flip();
        }

        int flags = 0;
        if (isConfig) {
            flags |= BUFFER_FLAG_CODEC_CONFIG;
        }
        if (buffer == null) {
            flags |= BUFFER_FLAG_END_OF_STREAM;
        }

        try {
            if (secure && buffer != null) {
                MediaCodec.CryptoInfo crypInfo = createCryptoInfo((EncryptionInfo) encryptionInfo);
                synchronized (queLock) {
                    mMediaCodec.queueSecureInputBuffer(index, 0, crypInfo, pts, flags);
                }
            } else {
                if ((flags & BUFFER_FLAG_END_OF_STREAM) == BUFFER_FLAG_END_OF_STREAM) {
                    mMediaCodec.queueInputBuffer(index, 0, 0, 0, flags);
                } else {
                    mMediaCodec.queueInputBuffer(index, 0, inputBuffer.limit(), pts, flags);
                }
            }
        } catch (Exception e) {
            Logger.e(TAG, "queueInputBufferInner  fail " + e.getLocalizedMessage());
            return ERROR;
        }

        return 0;

    }

    private MediaCodec.CryptoInfo createCryptoInfo(EncryptionInfo info) {
        MediaCodec.CryptoInfo crypInfo = new MediaCodec.CryptoInfo();
        crypInfo.key = info.key_id;
        crypInfo.iv = info.iv;
        if (info.subsamples != null) {
            crypInfo.numSubSamples = info.subsamples.size();
            crypInfo.numBytesOfClearData = new int[crypInfo.numSubSamples];
            crypInfo.numBytesOfEncryptedData = new int[crypInfo.numSubSamples];
            for (int i = 0; i < info.subsamples.size(); i++) {
                SubsampleEncryptionInfo subsampleEncryptionInfo = info.subsamples.get(i);
                crypInfo.numBytesOfClearData[i] = subsampleEncryptionInfo.bytes_of_clear_data;
                crypInfo.numBytesOfEncryptedData[i] = subsampleEncryptionInfo.bytes_of_protected_data;
            }
        }

        if ("cenc".equals(info.scheme) || "cens".equals(info.scheme)) {
            crypInfo.mode = 1;//AES_CTR
        } else if ("cbc1".equals(info.scheme) || "cbcs".equals(info.scheme)) {
            crypInfo.mode = 2;//AES_CBC
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            if ("cens".equals(info.scheme) || "cbcs".equals(info.scheme)) {
                //has pattern
                int blocksToEncrypt = info.crypt_byte_block;
                int blocksToSkip = info.skip_byte_block;
                MediaCodec.CryptoInfo.Pattern pattern = null;
                pattern = new MediaCodec.CryptoInfo.Pattern(blocksToEncrypt, blocksToSkip);
                crypInfo.setPattern(pattern);
            }
        }
        return crypInfo;
    }

    @NativeUsed
    public int dequeueOutputBufferIndex(long timeoutUs) {
        if (mMediaCodec == null) {
            return ERROR;
        }

        if (mAsyncMode) {
            synchronized (mOutputLock) {
                if (mOutputIndices.isEmpty()) {
                    try {
                        mOutputLock.wait(timeoutUs / 1000, (int) ((timeoutUs % 1000) * 1000));
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                    }
                }
                if (!mOutputIndices.isEmpty()) {
                    return mOutputIndices.remove(0);
                }
            }
            return TRY_AGAIN;
        }

        try {
            int index = mMediaCodec.dequeueOutputBuffer(mBufferInfo, timeoutUs);
            if (index >= 0) {
                return index;
            } else if (index == INFO_TRY_AGAIN_LATER) {
                return TRY_AGAIN;
            }else if(index == INFO_OUTPUT_FORMAT_CHANGED) {
                return index;
            } else if (index == INFO_OUTPUT_BUFFERS_CHANGED) {
                if (Build.VERSION.SDK_INT < Build.VERSION_CODES.LOLLIPOP) {
                    mOutputBuffers = mMediaCodec.getOutputBuffers();
                }
                return index;
            } else {
                return ERROR;
            }
        } catch (Exception e) {
            Logger.e(TAG, "dequeueOutputBufferIndex fail " + e.getMessage());
            return ERROR;
        }
    }

    @NativeUsed
    public Object getOutputBufferInfo(int index) {
        if (index == INFO_OUTPUT_FORMAT_CHANGED) {
            OutputBufferInfo outputBufferInfo = fillFormatOutputBufferInfo();
            return outputBufferInfo;
        } else if (index >= 0) {
            OutputBufferInfo outputBufferInfo = fillDecodeBufferInfo(index);
            return outputBufferInfo;
        }
        return null;
    }

    @NativeUsed
    public Object getOutBuffer(int index) {
        if (index >= 0) {
            ByteBuffer outputBuffer = null;
            try {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
                    outputBuffer = mMediaCodec.getOutputBuffer(index);
                } else {
                    outputBuffer = mOutputBuffers[index];
                }
            }catch (Exception e){
                Logger.e(TAG, "getOutBuffer fail " + e.getMessage());
            }

            return outputBuffer;
        }
        return null;
    }

    private OutputBufferInfo fillDecodeBufferInfo(int index) {
        // 异步模式：BufferInfo 来自 onOutputBufferAvailable 回调；轮询模式来自 mBufferInfo
        MediaCodec.BufferInfo bufferInfo = mAsyncMode ? mOutputBufferInfos.get(index) : mBufferInfo;
        if (bufferInfo == null) {
            return null;
        }

        OutputBufferInfo info = new OutputBufferInfo();
        info.type = 0;//buffer
        info.index = index;
        info.pts = bufferInfo.presentationTimeUs;
        info.eos = ((bufferInfo.flags & BUFFER_FLAG_END_OF_STREAM) != 0);
        info.bufferSize = bufferInfo.size;
        info.bufferOffset = bufferInfo.offset;
        return info;
    }

    private OutputBufferInfo fillFormatOutputBufferInfo() {

        MediaFormat format = null;
        try {
            format = mMediaCodec.getOutputFormat();
        } catch (Exception e) {
            return null;
        }

        OutputBufferInfo info = new OutputBufferInfo();
        info.type = 1;//config
        info.eos = false;
        if (mCodecCateGory == CODEC_CATEGORY_VIDEO) {
            info.videoCropBottom = getFormatInteger(format, "crop-bottom");
            info.videoCropLeft = getFormatInteger(format, "crop-left");
            info.videoCropRight = getFormatInteger(format, "crop-right");
            info.videoCropTop = getFormatInteger(format, "crop-top");
            info.videoHeight = getFormatInteger(format, MediaFormat.KEY_WIDTH);
            info.videoWidth = getFormatInteger(format, MediaFormat.KEY_HEIGHT);
            info.videoPixelFormat = getFormatInteger(format, MediaFormat.KEY_COLOR_FORMAT);
            info.videoSliceHeight = getFormatInteger(format, "slice-height");
            info.videoStride = getFormatInteger(format, "stride");
        } else {
            info.audioChannelCount = getFormatInteger(format, MediaFormat.KEY_CHANNEL_COUNT);
            info.audioChannelMask = getFormatInteger(format, MediaFormat.KEY_CHANNEL_MASK);
            info.audioSampleRate = getFormatInteger(format, MediaFormat.KEY_SAMPLE_RATE);
            info.audioFormat = getFormatInteger(format, "pcm-encoding");
        }

        return info;
    }

    private static int getFormatInteger(MediaFormat format, String key) {
        if (format.containsKey(key)) {
            return format.getInteger(key);
        }
        return -1;
    }

    static List<String> blackCodecPrefix = new ArrayList<>();
    static List<String> blackCodecSuffix = new ArrayList<>();

    private static boolean isBlackCodec(MediaCodecInfo info) {
        if (blackCodecPrefix.isEmpty()) {
            blackCodecPrefix.add("OMX.PV.");
            blackCodecPrefix.add("OMX.google.");
            blackCodecPrefix.add("OMX.ARICENT.");
            blackCodecPrefix.add("OMX.SEC.WMV.Decoder");
            blackCodecPrefix.add("OMX.SEC.MP3.Decoder");
            blackCodecPrefix.add("OMX.MTK.VIDEO.DECODER.VC1");
            blackCodecPrefix.add("OMX.SEC.vp8.dec");
        }

        if (blackCodecSuffix.isEmpty()) {
            blackCodecSuffix.add(".sw.dec");
            blackCodecSuffix.add(".hevcswvdec");
        }

        String name = info.getName();
        for (String prefix : blackCodecPrefix) {
            if (name.startsWith(prefix)) {
                return true;
            }
        }

        for (String suffix : blackCodecSuffix) {
            if (name.endsWith(suffix)) {
                return true;
            }
        }

        return false;
    }
}
