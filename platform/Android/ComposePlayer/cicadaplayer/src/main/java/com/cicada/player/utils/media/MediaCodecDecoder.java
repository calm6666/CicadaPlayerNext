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
import android.util.Range;
import android.util.SparseArray;
import android.view.Surface;

import com.cicada.player.utils.Logger;
import com.cicada.player.utils.NativeUsed;

import java.io.IOException;
import java.lang.reflect.Method;
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
    // 在 MediaCodec.start() 之前（见 start()）。本工程**只用异步**，没有轮询回退。
    private static final boolean ASYNC_ENABLED = true;

    /*
     * 【2026-09-27 阶段 D：删除"降级轮询"的静态闩 sAsyncBroken】
     *
     * 它原来的作用：一旦检测到 flush 之后输入回调不再送达，就标记本进程"异步不可用"，
     * 之后新建的解码器实例改用**同步轮询**（dequeueInputBuffer/dequeueOutputBuffer）。
     *
     * 现在删除，理由两条：
     *   1) 根因已修：异步模式下 flush() 之后必须再调 start()（AOSP 契约原文），
     *      Java 侧与 NDK 侧都按这条不变量实现（commit 99cfd107 / ndkCodecBinding::flush）；
     *   2) 设计约束（docs/ANDROID-NDK-ASYNC-DECODER.md）：**不保留同步轮询路径**。
     *      轮询会引入"轮询周期"延迟，并与 NDK 异步数据面的模型相互矛盾。
     * 因此这里只保留诊断日志；真出问题时由内核既有的"错误 → 重建解码器"路径处理。
     */
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

    /*
     * ============ B：优先厂商硬解 Codec2 组件（2026-09-24，只作用于视频）============
     *
     * 背景（真机 115KB 日志）：视频组件选中了 OMX 老实现
     *     In onAllocateComponent create component, codec name: OMX.hisi.video.decoder.hevc
     * 而它在 configure 时拒绝了 dynamic-ANW 输出端口模式：
     *     setPortMode on output to DynamicANWBuffer failed w/ err -2147483648
     * ACodec 于是退回把 Surface 的 buffer 按**裸 YUV** 几何重配：
     *     Add ALIGN32BIT usage
     *     Allocating 6/11 buffers from a native window of size 737280 / 645120
     * 这正是默认 GL 路（SurfaceTexture 采样）出现额外转换与带宽压力的来源。
     *
     * Codec2 组件（c2. 前缀）走 handle / opaque buffer 的现代通路，不经过 ACodec 的
     * setPortMode 协商，所以**优先**选它。三条安全约束（缺一不可）：
     *   1) 只对视频生效 —— 音频没有零拷贝诉求，换组件只会引入 A/V 时序风险；
     *   2) 只偏爱**硬解** c2 —— c2.android. 是 AOSP 软解，选它等于偷偷降级成软解；
     *   3) 找不到任何合格 c2 时**逐字回退**到原来的"第一个非黑名单组件"逻辑
     *      （见 getNotBlackCodecName），所以没有 c2 的机器行为与今天完全一致。
     *
     * A/B 对比只需把 PREFER_HARDWARE_C2 改成 false。
     */
    private static final boolean PREFER_HARDWARE_C2 = true;
    private static final String CODEC2_PREFIX = "c2.";
    private static final String CODEC2_SOFTWARE_PREFIX_ANDROID = "c2.android.";
    private static final String CODEC2_SOFTWARE_PREFIX_GOOGLE = "c2.google.";

    /*
     * D：协商结果每个解码器实例只打一条。
     *
     * 组件选择那一刻只能知道"这个组件**声明**支持 COLOR_FormatSurface"；真正的输出
     * 端口模式是 configure 之后由 ACodec/OMX 协商的，只有在拿到
     * INFO_OUTPUT_FORMAT_CHANGED 的 output format 时才看得到（见
     * fillFormatOutputBufferInfo 里那条 negotiated 行）。
     */
    private boolean mReportedNegotiatedFormat = false;

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

    /**
     * 【控制面：NDK 数据面专用】只做"选择"、不做"创建"。
     *
     * 与 configureVideo/configureAudio 用**完全相同**的依据（同一份 MediaCodecList 顺序、
     * 同一份黑名单、同一份 secure 判定）返回将被 {@code MediaCodec.createByCodecName()}
     * 使用的 codec 名；NDK 异步绑定（ndkCodecBinding）用它 +
     * {@code AMediaCodec_createCodecByName()} 精确锁定同一颗 codec（按名字创建自 API 21 就有），
     * 从而保证"Java 能力面选中的"与"NDK 实际创建的"是同一颗。
     *
     * 纯查询：不创建 MediaCodec、不持有资源、不改本实例的播放状态（只按入参临时组一个
     * MediaFormat 供能力筛选）。返回 null/空串 = 选择失败，调用方回落 Java 绑定。
     * 设计见 docs/ANDROID-NDK-ASYNC-DECODER.md。
     */
    @NativeUsed
    public String selectCodecName(boolean isVideo, String mime, int width, int height,
                                  int sampleRate, int channelCount, int isADTS) {
        if (TextUtils.isEmpty(mime)) {
            return null;
        }

        mMime = mime;
        mCodecCateGory = isVideo ? CODEC_CATEGORY_VIDEO : CODEC_CATEGORY_AUDIO;

        MediaFormat format;
        if (isVideo) {
            format = MediaFormat.createVideoFormat(mime, width, height);
        } else {
            format = MediaFormat.createAudioFormat(mime, sampleRate, channelCount);
            if (isADTS != 0) {
                format.setInteger(MediaFormat.KEY_IS_ADTS, 1);
            }
        }

        String codecName = findDecoderName(format);
        Logger.i(TAG, "[codec-select] ndk control plane: isVideo=" + isVideo + " mime=" + mime
                + " -> " + codecName);
        return codecName;
    }

    @NativeUsed
    public int configureVideo(String mime, int width, int height, int angle, Surface surface,
                              boolean usePlaceholderSurface) {
        Logger.d(TAG, "--> configureVideo start " + mime + ", " + width + ", " + height + ", " + surface
                + ", usePlaceholderSurface = " + usePlaceholderSurface);
        mCodecCateGory = CODEC_CATEGORY_VIDEO;
        mMime = mime;

        /* 新一次配置 = 新一次协商，允许再报一条（见 mReportedNegotiatedFormat）。 */
        mReportedNegotiatedFormat = false;

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
         * 打一条"占位分支到底走没走"的证据（INFO 级：默认日志等级下 DEBUG 不出现，
         * 上一次排查就是靠猜 usePlaceholderSurface 的真假，浪费了一整份真机日志）。
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

        /*
         * D：一次性诊断（每次建解码器一条，天然限频）。
         * 必须用 INFO 级别：默认日志等级就是 INFO（Logger 里 mCurrentLogLevel 初值），
         * DEBUG 行在真机日志里根本不出现 —— 已核，本文件原有的 DEBUG 行（例如
         * configureVideo 的 "--> configureVideo start"）在用户那份日志里一条都没有。
         */
        Logger.i(TAG, describeCodecSelect(mediaCodecInfoList, codecName));

        /*
         * 复用判定诊断（**纯日志、零行为改动**，只对视频打）。见 describeCodecReuse 的说明：
         * 它回答"这次的档位切换，ExoPlayer 的 DecoderReuseEvaluation 会判成哪一类"，
         * 用来决定下一轮走 B1（可复用 ⇒ 原地换流）还是 B2（不可复用 ⇒ DummySurface 交接）。
         * 每个解码器实例一条，天然限频。
         */
        if (mCodecCateGory == CODEC_CATEGORY_VIDEO) {
            Logger.i(TAG, describeCodecReuse(mediaCodecInfoList, codecName, videoFormat));
        }

        return codecName;
    }

    /*
     * ============ 复用判定诊断（ExoPlayer DecoderReuseEvaluation 的思想，2026-09-24）============
     *
     * 目的：一份日志就能回答"这次切档 ExoPlayer 会判成可复用还是必须换 codec"，从而决定
     * 下一轮实现 B1（可复用 ⇒ 把新 CSD 贴给 active 解码器、Surface 全程不动、真无缝）
     * 还是 B2（不可复用 ⇒ pending 用已有 mDummySurface + 提交时有序交接）。
     *
     * 【为什么这里只能给出"本 codec 能不能承载新格式"，而不是 ExoPlayer 的
     *   yes_without_reconfig / yes_with_reconfig 三档原词】
     * ExoPlayer 的三档判据需要**同时**拿到旧格式与新格式（媒体3 的
     * MediaCodecInfo.canReuseCodec(oldFormat, newFormat)）。本函数只有新格式：
     *   · 旧格式的 MIME/编码/分辨率由**内核已有的那行**给出
     *         `video stream switched to incompatible parameters (codec X -> Y, WxH -> WxH)`
     *      （codec id 变了 = MIME 变化 ⇒ ExoPlayer 直接判 no；那行**不存在** = 参数完全一致
     *      ⇒ 判 yes_without_reconfig）。为了不新增行为/成员，这里不重复实现它。
     *   · **故意不引入"记住上一次格式"的静态状态**：同一进程里可能同时存在多个播放器、多路
     *     解码器（本工程就有 pending/active/retired 三类），静态的"上一次"会把不同播放器的
     *     信息混在一起；给出错的结论比没有结论更糟（红线：不要猜）。
     *
     * 判据（照 ExoPlayer 的规则，能用平台 API 就用，取不到就写 unknown，绝不猜）：
     *   · 分辨率 / 帧率 / 码率是否落在本 codec 的 codecMax 之内 —— VideoCapabilities 的
     *     isSizeSupported / areSizeAndRateSupported / getBitrateRange（ExoPlayer 的
     *     codecMaxValues 就是这三项）；
     *   · format 是否被该组件的 capabilities 接受 —— CodecCapabilities.isFormatSupported；
     *   · 平台的无缝适配结论 —— API 29 的 isSeamlessAdaptationSupported，**走反射**：
     *     这个 API 在部分 SDK/机型上取不到，而这条日志绝不能因此让编译或运行出问题，
     *     取不到就写 unknown（"不要猜"）；
     *   · MIME 变化 / 旋转 / 色彩 / DRM 会话变化 —— 前三项由内核那行 + 上面 ranges 覆盖，
     *     DRM 会话在 configureVideo 之前设置、这里看不到，**不报**（不猜）。
     */
    private String describeCodecReuse(List<MediaCodecInfo> candidates, String chosen, MediaFormat format) {
        final int width = getFormatInteger(format, MediaFormat.KEY_WIDTH);
        final int height = getFormatInteger(format, MediaFormat.KEY_HEIGHT);

        StringBuilder sb = new StringBuilder(256);
        sb.append("[codec-reuse] to=").append(mMime).append(' ')
                .append(width).append('x').append(height)
                .append(" profile=").append(getFormatInteger(format, MediaFormat.KEY_PROFILE))
                .append(" level=").append(getFormatInteger(format, MediaFormat.KEY_LEVEL))
                .append(" codec=").append(chosen == null ? "none" : chosen);

        MediaCodecInfo info = null;
        for (MediaCodecInfo candidate : candidates) {
            if (chosen != null && chosen.equals(candidate.getName())) {
                info = candidate;
                break;
            }
        }

        if (info == null) {
            return sb.append(" reuse=unknown reasons=candidate_not_found").toString();
        }

        MediaCodecInfo.CodecCapabilities capabilities = null;
        try {
            capabilities = info.getCapabilitiesForType(mMime);
        } catch (Throwable ignored) {
            /* 诊断信息：取不到就当 unknown，绝不影响解码器创建 */
        }

        if (capabilities == null) {
            return sb.append(" reuse=unknown reasons=capabilities_unavailable").toString();
        }

        MediaCodecInfo.VideoCapabilities video = capabilities.getVideoCapabilities();
        sb.append(" codecMax=");
        if (video != null) {
            Range<Integer> widths = video.getSupportedWidths();
            Range<Integer> heights = video.getSupportedHeights();
            /* getSupportedFrameRates() 的返回类型是 Range<Integer>（不是 Double）。 */
            Range<Integer> rates = video.getSupportedFrameRates();
            Range<Integer> bitrates = video.getBitrateRange();
            sb.append(widths == null ? "?" : String.valueOf(widths.getUpper())).append('x')
                    .append(heights == null ? "?" : String.valueOf(heights.getUpper())).append('@')
                    .append(rates == null ? "?" : String.valueOf(rates.getUpper()))
                    .append(" bitrate=").append(bitrates == null ? "?" : bitrates.toString());
        } else {
            sb.append("?");
        }

        sb.append(" seamlessAdaptationSupported=")
                .append(querySeamlessAdaptation(info, capabilities, format));

        /* 逐条列"为什么不能在本 codec 上继续解"（ExoPlayer 的 discardReasons 口径） */
        StringBuilder reasons = new StringBuilder();
        if (!capabilities.isFormatSupported(format)) {
            appendReason(reasons, "format_unsupported");
        }
        if (video != null) {
            if (width > 0 && height > 0 && !video.isSizeSupported(width, height)) {
                appendReason(reasons, "size_out_of_codecMax");
            }
            final int frameRate = getFormatInteger(format, MediaFormat.KEY_FRAME_RATE);
            if (frameRate > 0 && width > 0 && height > 0 &&
                    !video.areSizeAndRateSupported(width, height, frameRate)) {
                appendReason(reasons, "fps_out_of_codecMax");
            }
            final int bitrate = getFormatInteger(format, MediaFormat.KEY_BIT_RATE);
            if (bitrate > 0 && !video.getBitrateRange().contains(bitrate)) {
                appendReason(reasons, "bitrate_out_of_codecMax");
            }
        }

        /*
         * verdict 的读法（与内核那行 `video stream switched to incompatible parameters` 组合）：
         *
         *   ① codec_ok 且**本行的 codec= 与上一次（active 解码器那条）[codec-reuse] 相同**
         *      → 同一个组件就能承载新格式。再看内核那行：
         *        · 那行**不存在**（参数完全一致）→ ExoPlayer 判 YES_WITHOUT_RECONFIGURATION
         *          → B1 最理想：连重配都不用，直接把新 CSD 喂给 active。
         *        · 那行存在（分辨率/档位变了）→ ExoPlayer 判 YES_WITH_RECONFIGURATION
         *          → B1：在**同一个 codec** 上 flush+重配+贴新 CSD，Surface 全程不动。
         *   ② codec_ok 但 codec= **变了**（换了组件）→ 组件能力不等价，ExoPlayer 判 NO → B2。
         *   ③ codec_no（reasons 里逐条说明越界项）→ ExoPlayer 判 NO → B2。
         *   ④ unknown → 能力/无缝 API 取不到，只能靠平台日志（ACodec/CCodec 那几行）人工判断。
         *
         * 注意：codecMax / seamlessAdaptationSupported 是**组件级**属性，所以当①②里的
         * 组件名相同时，本行给出的就是"老 codec 能不能吃下新格式"的答案（这正是 B1 的问法）。
         */
        sb.append(" reuse=").append(reasons.length() == 0 ? "codec_ok" : "codec_no");
        sb.append(" reasons=").append(reasons.length() == 0 ? "none" : reasons.toString());
        return sb.toString();
    }

    private static void appendReason(StringBuilder reasons, String reason) {
        if (reasons.length() > 0) {
            reasons.append(',');
        }
        reasons.append(reason);
    }

    /*
     * 平台的无缝适配结论：API 29 的 isSeamlessAdaptationSupported 走反射调用 ——
     * 该 API 在部分 SDK/机型上不存在，而这条诊断绝不能因此影响编译或运行。
     * 先试 MediaCodecInfo 上的重载，再试 CodecCapabilities 上的，都没有就 unknown。
     */
    private static String querySeamlessAdaptation(MediaCodecInfo info,
                                                  MediaCodecInfo.CodecCapabilities capabilities,
                                                  MediaFormat format) {
        try {
            Method method = MediaCodecInfo.class.getMethod("isSeamlessAdaptationSupported", MediaFormat.class);
            Object result = method.invoke(info, format);
            if (result instanceof Boolean) {
                return String.valueOf((Boolean) result);
            }
        } catch (Throwable ignored) {
            /* 该重载不存在或不可调用：继续试下一个 */
        }

        try {
            Method method = MediaCodecInfo.CodecCapabilities.class
                    .getMethod("isSeamlessAdaptationSupported", MediaFormat.class);
            Object result = method.invoke(capabilities, format);
            if (result instanceof Boolean) {
                return String.valueOf((Boolean) result);
            }
        } catch (Throwable ignored) {
            /* 同上 */
        }

        return "unknown";
    }

    private String getNotBlackCodecName(List<MediaCodecInfo> mediaCodecInfoList) {
        String codecName = null;
        if (!mediaCodecInfoList.isEmpty()) {
            /*
             * B：视频优先硬解 c2 组件（门控见 PREFER_HARDWARE_C2）。一个都找不到就
             * 落到下面那段老逻辑 —— 那一段一个字都没改，所以没有 c2 的机器行为与今天
             * 逐字一致（同样的黑名单、同样的 MediaCodecList 顺序、同样的第一个命中者）。
             */
            if (PREFER_HARDWARE_C2 && mCodecCateGory == CODEC_CATEGORY_VIDEO) {
                for (MediaCodecInfo info : mediaCodecInfoList) {
                    if (!isBlackCodec(info) && isHardwareC2Codec(info)) {
                        codecName = info.getName();
                        break;
                    }
                }
            }

            if (codecName == null) {
                for (MediaCodecInfo info : mediaCodecInfoList) {
                    if (!isBlackCodec(info)) {
                        codecName = info.getName();
                        break;
                    }
                }
            }
        }
        return codecName;
    }

    /*
     * 厂商硬解 Codec2 组件判定：
     *   · 名字必须 c2. 开头（OMX 老组件不算）；
     *   · 排除 AOSP 软解 c2.android. / c2.google.（选它们等于静默降级软解）；
     *   · API 29+ 再用平台标志 isSoftwareOnly() 兜一层（个别厂商软解不按前缀命名）。
     */
    private static boolean isHardwareC2Codec(MediaCodecInfo info) {
        String name = info.getName();
        if (name == null || !name.startsWith(CODEC2_PREFIX)) {
            return false;
        }
        if (name.startsWith(CODEC2_SOFTWARE_PREFIX_ANDROID) || name.startsWith(CODEC2_SOFTWARE_PREFIX_GOOGLE)) {
            return false;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            try {
                if (info.isSoftwareOnly()) {
                    return false;
                }
            } catch (Throwable ignored) {
                /* 平台标志不可用/抛异常时，按前缀结论处理 */
            }
        }
        return true;
    }

    /* 软解组件名判定（只用于日志里如实标明 hwByName，不参与选择）。 */
    private static boolean isSoftwareCodecName(String name) {
        if (name == null) {
            return false;
        }
        return name.startsWith("OMX.google.") || name.startsWith("OMX.PV.")
                || name.startsWith(CODEC2_SOFTWARE_PREFIX_ANDROID)
                || name.startsWith(CODEC2_SOFTWARE_PREFIX_GOOGLE)
                || name.endsWith(".sw.dec") || name.endsWith(".hevcswvdec");
    }

    /*
     * D：一条日志回答"这台机器有哪些候选、最终选了谁、是不是 c2、声明支持不支持
     * COLOR_FormatSurface、c2 到底有没有找到"。
     *
     * opaquePortMode 在这里只能是 unknown —— 端口模式是 configure 之后由
     * ACodec/OMX 协商的，选择阶段拿不到；真实值见 fillFormatOutputBufferInfo 里
     * 那条 negotiated 行（由协商出来的输出 color-format 推断）。
     */
    private String describeCodecSelect(List<MediaCodecInfo> candidates, String chosen) {
        StringBuilder sb = new StringBuilder(256);
        sb.append("[codec-select] category=")
                .append(mCodecCateGory == CODEC_CATEGORY_VIDEO ? "video" : "audio")
                .append(" candidates=");

        boolean first = true;
        boolean c2Candidate = false;
        for (MediaCodecInfo info : candidates) {
            if (!first) {
                sb.append(',');
            }
            first = false;
            sb.append(info.getName());
            if (!isBlackCodec(info) && isHardwareC2Codec(info)) {
                c2Candidate = true;
            }
        }

        sb.append(" chosen=").append(chosen == null ? "none" : chosen);
        sb.append(" isC2=").append(chosen != null && chosen.startsWith(CODEC2_PREFIX));
        sb.append(" hwByName=").append(chosen == null ? "unknown" : String.valueOf(!isSoftwareCodecName(chosen)));
        sb.append(" surfaceColorFormat=").append(declaresSurfaceColorFormat(candidates, chosen));
        sb.append(" preferC2=").append(PREFER_HARDWARE_C2
                ? (c2Candidate ? "found" : "notfound") : "off");
        sb.append(" opaquePortMode=unknown");
        return sb.toString();
    }

    /* 被选中的组件是否在 capabilities 里声明支持 COLOR_FormatSurface（可否 surface 输出）。 */
    private boolean declaresSurfaceColorFormat(List<MediaCodecInfo> candidates, String chosen) {
        if (chosen == null) {
            return false;
        }
        for (MediaCodecInfo info : candidates) {
            if (!chosen.equals(info.getName())) {
                continue;
            }
            try {
                MediaCodecInfo.CodecCapabilities capabilities = info.getCapabilitiesForType(mMime);
                if (capabilities == null || capabilities.colorFormats == null) {
                    return false;
                }
                for (int format : capabilities.colorFormats) {
                    if (format == MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface) {
                        return true;
                    }
                }
            } catch (Throwable ignored) {
                /* 诊断信息，取不到就当 false，绝不影响解码器创建 */
            }
            return false;
        }
        return false;
    }

    /*
     * 由协商出来的输出 color-format 推断输出端口模式：
     *   COLOR_FormatSurface(2130708361) = 解码器直接写 Surface 的 gralloc buffer
     *     （opaque，消费端可建 EGLImage，即零拷贝端口模式）；
     *   其它值（19 = YUV420Planar、21 = YUV420SemiPlanar 等）= 裸 YUV 几何，
     *     也就是日志里 Add ALIGN32BIT usage 那条老路；
     *   -1 = output format 里没有这个键，别猜。
     */
    private static String describeOpaquePortMode(int colorFormat) {
        if (colorFormat < 0) {
            return "unknown";
        }
        return colorFormat == MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface ? "true" : "false";
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
    /**
     * 【控制面：NDK 数据面专用】把本实例的 1x1 DummySurface 交给 native。
     *
     * NDK 异步绑定在"没有真 surface"或"占位面（切档 B2）"这两种情形下，需要一块 surface
     * 才能维持 surface 模式 —— 这正是 Java 侧 configureVideo 的既有做法
     * （`target = ensureDummySurface()`）。让 NDK 复用**同一块** dummy，语义与 Java 路径
     * 逐字一致，也避免"NDK 退化成 ByteBuffer 模式后视频帧无人消费"这种分叉。
     *
     * 纯查询：懒创建并缓存（与 configureVideo 用的就是同一个 mDummySurface）；
     * 失败返回 null，调用方回落 Java 绑定（那里也会再试同一块 dummy）。
     * 见 docs/ANDROID-NDK-ASYNC-DECODER.md 的路由规则。
     */
    @NativeUsed
    public Object getDummySurface() {
        return ensureDummySurface();
    }

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
         * 【2026-09-27 阶段 D】原来这里还有一句"static 的 sAsyncBroken 一旦被标记，
         * 之后新建的实例直接走轮询"。该闩已删除：本设计**没有**同步轮询路径
         * （见本文件顶部 ASYNC_ENABLED 处的说明与 docs/ANDROID-NDK-ASYNC-DECODER.md）。
         */
        mInputWaitCount = 0;
        mAsyncFirstInputLogged = false;
        mFlushCompletedMs = 0;
        mInputCallbackAfterFlushMs = 0;

        // 异步回调模式：必须在 MediaCodec.start() 之前注册 ——
        // start() 之后 codec 立刻回调 onInputBufferAvailable，注册晚了会丢失
        // 首批回调，输入队列永远为空（此前误判为"设备不支持异步"）。
        if (ASYNC_ENABLED) {
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
            /* ASYNC_ENABLED 关闭时才是同步轮询；本工程恒为 true（异步唯一路径）。 */
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
                 * 时间戳放在 flush() **返回之后**记（见下面的赋值位置说明）：
                 * 这一句在 mMediaCodec.flush() 已经返回之后执行，所以它标记的是
                 * "flush 完成时刻"，宽限期从这里开始算。
                 */
                mFlushCompletedMs = SystemClock.uptimeMillis();
            }
            synchronized (mOutputLock) {
                /*
                 * 清空输出缓冲跟踪。语义上这也是**正确**的：flush 会把 codec
                 * 持有的输出缓冲全部收回（ACodec 在 flush 完成时把 owned buffers
                 * 交回），所以"此刻没有任何输出缓冲被播放器占住"。
                 * 正因如此，下面判据 (b) 里"输出侧没被占住 + 输入侧持续拿不到"
                 * 在 flush 之后才是一个有意义的组合。
                 */
                mOutputIndices.clear();
                mOutputBufferInfos.clear();
            }

            /*
             * ============ 【2026-09-27 修：flush 之后必须 start()，否则输入侧永久空转】============
             *
             * 平台契约（AOSP MediaCodec.flush() 的原文，见 developer.android.com/reference/
             * android/media/MediaCodec#flush() ）：
             *
             *   "Upon return, all indices previously returned in calls to dequeueInputBuffer
             *    and dequeueOutputBuffer — or obtained via onInputBufferAvailable or
             *    onOutputBufferAvailable callbacks — become invalid, and all buffers are
             *    owned by the codec.
             *    If the codec is configured in asynchronous mode, call start after flush has
             *    returned to resume codec operations. **The codec will not request input
             *    buffers until this has happened.**"
             *
             * 也就是说：异步模式下 flush() 之后**不调 start() 就永远不会再有
             * onInputBufferAvailable**。而上面 mInputIndices.clear() 把输入侧重填的
             * **唯一**来源也清空了 ⇒ 输入侧永久空转、解码器永远拿不到落点关键帧、
             * 一帧都不上屏。真机日志（2026-09-27 21:54:27 本地文件 / 21:55:28 DASH，
             * 两次症状完全相同）：
             *
             *   PFR: seek → FlushVideoPath → kWhatFlushCompleted → clearCache ret 0（flush state 1）
             *   → MediaCodecDecoder: async input path looks dead
             *       (criterion=no-input-callback-after-flush, waitedMs=1003/1029,
             *        mInputWaitCount=31/30, noOutputHeld=true)
             *   → 无解码帧 ⇒ 画面永久卡死（同一窗口 KPI total fps 0.9 / 1.0）
             *
             * 三个注意点：
             *   1) 这里调的是**平台**的 mMediaCodec.start()，**不是**本类的 start() 方法：
             *      后者会重建 HandlerThread 并再次 setCallback（setCallback 必须在本实例
             *      start 之前且只注册一次），在 flush 之后调它会破坏异步回调注册。
             *   2) 只对异步模式调。同步模式按同一份契约会自行恢复（配置了 input surface 的
             *      自动恢复，其余在 dequeueInputBuffer 时恢复），多调一次可能抛
             *      "Cannot call start() twice"。
             *   3) 顺序不能反：必须**先** clear()（旧 index 全部作废）**再** start()，
             *      否则 start() 之后到达的回调会把新 index 又清掉，等于没修。
             *   4) 这不是兜底、也没有任何计时器：它补齐的是平台要求的**状态迁移**
             *      （Executing → Flushed → start() → Executing）。
             */
            try {
                mMediaCodec.start();
            } catch (Exception e) {
                Logger.e(TAG, "start after flush fail " + e.getMessage());
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
                // 所以这里【只】回报 TRY_AGAIN，绝不改 mAsyncMode、绝不调用同步 API。
                // 阶段 D 起连"降级给下一个实例"也没有了：本设计不存在同步轮询路径。
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
                //       —— 这一条只在 flush 之后 mOutputIndices 被清空、且输入
                //       回调也没来时才可能成立，与 (a) 互相印证。
                //
                // 【阶段 D】这里原来还会标记 sAsyncBroken 让下一个实例走轮询；该闩已删除。
                // 命中本判据时只留一条 WARN，兜底交给内核既有的"错误 → 重建解码器"路径。
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

                if (flushCallbackMissing || starvedWithoutBackpressure) {
                    /*
                     * 【阶段 D：这里不再标记"下个实例走轮询"】
                     * 只打一条 WARN 说明命中了哪个判据。本设计**没有**同步轮询回退：
                     *   · 根因（异步模式 flush 后不 start）已按平台契约修好；
                     *   · 兜底动作交给内核既有的"错误 → 重建解码器"路径，而不是换成轮询
                     *     （轮询会引入轮询周期延迟，并与 NDK 异步数据面模型矛盾）。
                     * 见 docs/ANDROID-NDK-ASYNC-DECODER.md §五 阶段 D。
                     */
                    Logger.w(TAG, "async input path looks dead (criterion="
                            + (flushCallbackMissing ? "no-input-callback-after-flush" : "starved-without-backpressure")
                            + ", waitedMs=" + (mFlushCompletedMs > 0 ? (nowMs - mFlushCompletedMs) : -1)
                            + ", mInputWaitCount=" + mInputWaitCount
                            + ", noOutputHeld=" + noOutputHeld
                            + "): 没有轮询回退；若真出现，请按 flush->start 不变量与 codec 状态排查");
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

            /*
             * D：每个实例一条 —— 这才是"端口模式到底拿到没拿到"的实测值。
             * 组件选择阶段的 surfaceColorFormat 只是 capabilities 里的**声明**，
             * 真正协商结果只有 output format 上的 color-format 能回答：
             * opaque（COLOR_FormatSurface）→ 解码器写 opaque gralloc，消费端可建
             * EGLImage（零拷贝）；其它值 → 裸 YUV，即 OMX 拒绝 dynamic-ANW 之后那条路。
             * stride / slice-height 一起打出来，便于和裸 YUV 的几何对账。
             */
            if (!mReportedNegotiatedFormat) {
                mReportedNegotiatedFormat = true;
                Logger.i(TAG, "[codec-select] negotiated output color-format=" + info.videoPixelFormat
                        + " stride=" + info.videoStride + " slice-height=" + info.videoSliceHeight
                        + " opaquePortMode=" + describeOpaquePortMode(info.videoPixelFormat));
            }
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
