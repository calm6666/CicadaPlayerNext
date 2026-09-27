package com.cicada.player.utils;

import android.os.Build;

import com.cicada.player.utils.media.MediaCodecUtils;

import java.util.HashMap;
import java.util.Map;

/**
 * 【「硬解 HEVC（硬件名）」括号里那个硬件名从哪来】
 *
 * <p>安卓上没有"显卡型号"这种直读接口：真正跑硬解的是设备的 MediaCodec（SoC 里的
 * 视频编解码单元），既不挂在 GPU 上，也没有一个跨厂商的"适配器名"API。所以这里按
 * <b>可靠性从高到低</b>取一个**真实存在**的描述，全拿不到就返回空串 —— 空串的含义是
 * "别写括号"，界面此时只显示 {@code 硬解 HEVC}，<b>绝不许编造型号</b>。</p>
 *
 * <p>分级（判据与 API 级别要求）：</p>
 * <ol>
 *   <li><b>GL_RENDERER</b>（{@code glGetString(GL_RENDERER)}，如 {@code Adreno (TM) 640}、
 *       {@code Mali-G715}）—— 最接近"显卡型号"。但它<b>要求调用线程上有 current 的
 *       EGL/GL 上下文</b>，否则读到 null；本工程唯一的那份 GL 上下文在内核的渲染线程里
 *       （framework/render/video/glRender），Java 侧没有、也不许为了显示一行字去新建
 *       上下文或线程。所以这一级由**调用方**在"本来就有上下文可读"的地方读好再传进来
 *       （{@link #resolve(String, boolean, String)} 的第一个参数）；本工程当前的调用点
 *       都传 {@code null}，即这一级暂时取不到值。</li>
 *   <li><b>{@code Build.SOC_MODEL}</b>（Android 12 / API 31 起才有，例如 {@code SM8650}）
 *       → <b>{@code Build.HARDWARE}</b>（例如 {@code qcom}、{@code mt6989}）→
 *       <b>{@code Build.BOARD}</b>（例如 {@code kalama}）。取第一个"不是空串、也不是
 *       {@link Build#UNKNOWN}"的。{@code Build.SOC_MODEL} 的读取**必须**由
 *       {@code Build.VERSION.SDK_INT >= Build.VERSION_CODES.S} 守卫（本模块 compileSdk
 *       34/36，编译得过；不守卫在 API &lt; 31 的设备上会抛 NoSuchFieldError）。
 *       见 {@link #fromBuild()}。</li>
 *   <li><b>硬件解码器名</b>（{@code MediaCodecInfo.getName()}，如 {@code c2.qti.hevc.decoder}）
 *       —— 用 {@link MediaCodecUtils#findHardwareDecoderName(String)} 按**当前编码的 mime**
 *       查本机的硬件解码器（判据与缓存跟内核的 {@code isHardwareDecodeSupported} 完全同一份）。
 *       <b>只在硬解时才有意义</b>：软解根本没在编解码单元上跑，把它的名字显示成"硬件名"
 *       等于骗人 —— 所以它被 {@code hardwareDecoderInUse} 这个开关**结构性地**挡住
 *       （见 {@link #resolve(String, boolean, String)}）。见 {@link #hardwareDecoderNameFor(String)}。</li>
 *   <li>都没有 ⇒ 返回空串（界面写 {@code 硬解 HEVC}，不写括号）。</li>
 * </ol>
 *
 * <p><b>关于第 ③ 级的诚实说明</b>：{@code MediaCodecList} 查到的是"**这台设备上有**哪个硬解
 * 组件能解这个编码"（按枚举顺序取第一个），**不一定**就是内核此刻真正选中的那一个组件 ——
 * 内核选中的那个名字只活在 SDK 自己的 JNI 解码线程里（{@code utils/media/MediaCodecDecoder}），
 * 应用层没有任何 API 能读到它。所以它排在 SoC / 板级型号之后，只在两者都拿不到时才出现，
 * 而且只在确认真的是硬解时。</p>
 *
 * <p>参考：本工程那台老示例（{@code source/paasApp} 的 {@code PerfMonitorUtils}）做过同一类
 * {@code MediaCodecList} 查询，区别是它按**固定顺序**先找 avc，因此显示的名字甚至可能不是
 * 当前编码的解码器；这里改成按当前编码查。</p>
 *
 * <p>本类在两套 SDK（{@code platform/Android/source/premierlibrary} 与
 * {@code platform/Android/ComposePlayer/cicadaplayer}）下**各一份、内容逐字相同**。</p>
 */
public final class HardwareNameUtil {

    /**
     * 第 ② 级的结果缓存。{@code Build.SOC_MODEL} / {@code HARDWARE} / {@code BOARD}
     * 在一次进程里是常量，没必要每次刷新统计都重读一遍；
     * {@code null} = 还没算过，{@code ""} = 算过了、这台设备什么都没有。
     */
    private static volatile String sBuildName;

    /**
     * 第 ③ 级的按编码缓存（mime → 解码器名；{@code ""} = 查过、这台设备没有）。
     *
     * <p>枚举 MediaCodecList 是设备级调用，绝不能每次刷新统计都跑（面板会按内核的
     * CurrentPosition 反复重组），所以**每种编码一次进程只查一次**。</p>
     */
    private static final Map<String, String> sDecoderNames = new HashMap<>();

    private HardwareNameUtil() {
    }

    /**
     * 取"硬件名"，按可靠性从高到低（分级判据见类注释）。
     *
     * <p><b>软解的红线就写在签名里</b>：{@code hardwareDecoderInUse} 为 false 时，第 ③ 级
     * （解码器名）一定不会被采用 —— "软解绝不写型号"这件事不靠调用方自觉。</p>
     *
     * @param glRenderer            调用线程上**已有** GL 上下文中读到的 GL_RENDERER 字符串；
     *                              没有（或不方便读）就传 null / 空串 —— 本方法不会为此新建上下文
     * @param hardwareDecoderInUse  活动解码器是否真的在硬解（内核
     *                              {@code CicadaPlayer.isVideoDecoderHardware()}）；软解传 false
     * @param codecShortName        当前视频流的**内核编码短名**（"H.264" / "H.265" / "AV1" /
     *                              "VP9" / "MPEG-4" / "MPEG-2"；空串 = 内核认不出）。
     *                              **只用来在第 ③ 级按编码查本机的硬解组件名，不参与显示**
     * @return 一个真实的硬件描述；一个都取不到时返回**空串**（界面据此不写括号）
     */
    public static String resolve(String glRenderer, boolean hardwareDecoderInUse, String codecShortName) {
        String name = clean(glRenderer);
        if (name != null) {
            return name;
        }

        name = fromBuild();
        if (name.length() > 0) {
            return name;
        }

        /* 软解到此为止：绝不写型号（解码没在编解码单元上跑，写上会让人以为硬解生效了） */
        if (!hardwareDecoderInUse) {
            return "";
        }

        return hardwareDecoderNameFor(codecShortName);
    }

    /**
     * 第 ② 级：{@code Build.SOC_MODEL}（API 31+）→ {@code Build.HARDWARE} → {@code Build.BOARD}，
     * 取第一个"非空且不是 {@code unknown}"的；都没有返回空串。
     *
     * <p>结果按进程缓存（见 {@link #sBuildName}）。读取 {@code Build.SOC_MODEL} 的版本守卫
     * 与它写在同一个三元表达式里，lint 的 NewApi 检查认得这种写法。</p>
     *
     * @return SoC / 硬件 / 板级型号；取不到时空串
     */
    public static String fromBuild() {
        String cached = sBuildName;
        if (cached != null) {
            return cached;
        }

        String name = firstReal(
                Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ? Build.SOC_MODEL : null,
                Build.HARDWARE,
                Build.BOARD);

        String result = name == null ? "" : name;
        sBuildName = result;
        return result;
    }

    /**
     * 第 ③ 级：按当前编码的 mime 查**本机硬件解码器名**
     * （{@link MediaCodecUtils#findHardwareDecoderName(String)}，返回值就是
     * {@code MediaCodecInfo.getName()}，例如 {@code c2.qti.hevc.decoder}）。
     *
     * <p>内核编码短名 → MediaCodec mime 的对应表就是本机的 {@code mimeOfCodec()}：
     * H.264 → video/avc、H.265 → video/hevc、VP9 → video/x-vnd.on2.vp9、
     * AV1 → video/av01、MPEG-4 → video/mp4v-es、MPEG-2 → video/mpeg2
     * （mime 字面量与 {@code android.media.MediaFormat} 上那几个常量等值；这里写字面量是为了
     * 不踩 {@code MIMETYPE_VIDEO_AV1} 等常量"API 29 才有"的坑）。认不出的编码直接返回空串。</p>
     *
     * <p>**显示用的探测**：查询本身失败（个别设备上 MediaCodecList 会抛异常）就当作"没有"，
     * 绝不让一行界面文案影响播放链路。结果按 mime 缓存（见 {@link #sDecoderNames}）。</p>
     *
     * @param codecShortName 内核编码短名；空串 / 认不出时返回空串
     * @return 硬件解码器名；取不到时空串
     */
    public static String hardwareDecoderNameFor(String codecShortName) {
        String mime = mimeOfCodec(codecShortName);
        if (mime == null) {
            return "";
        }

        synchronized (sDecoderNames) {
            String cached = sDecoderNames.get(mime);
            if (cached != null) {
                return cached;
            }

            String name;
            try {
                name = MediaCodecUtils.findHardwareDecoderName(mime);
            } catch (Exception e) {
                /* 只影响这一行显示的括号：探测失败 = 没有这个名字，照旧不写括号 */
                name = null;
            }

            String result = name == null ? "" : name;
            sDecoderNames.put(mime, result);
            return result;
        }
    }

    /** 内核编码短名 → MediaCodec mime；认不出来返回 null（**不做任何猜测**）。 */
    private static String mimeOfCodec(String codecShortName) {
        if (codecShortName == null) {
            return null;
        }

        switch (codecShortName.trim()) {
            case "H.264":
                return "video/avc";
            case "H.265":
                return "video/hevc";
            case "VP9":
                return "video/x-vnd.on2.vp9";
            case "AV1":
                return "video/av01";
            case "MPEG-4":
                return "video/mp4v-es";
            case "MPEG-2":
                return "video/mpeg2";
            default:
                return null;
        }
    }

    /** 取第一个"真实"的值（见 {@link #clean(String)}）；都没有返回 null。 */
    private static String firstReal(String... values) {
        if (values == null) {
            return null;
        }

        for (String value : values) {
            String name = clean(value);
            if (name != null) {
                return name;
            }
        }

        return null;
    }

    /**
     * 规整一个候选值：去首尾空白；空串与 {@link Build#UNKNOWN}（字面量 "unknown"）都算
     * "没有这个信息"，返回 null。**只做这一件事，不做任何映射/缩写/猜测。**
     */
    private static String clean(String value) {
        if (value == null) {
            return null;
        }

        String trimmed = value.trim();
        if (trimmed.length() == 0) {
            return null;
        }

        if (Build.UNKNOWN.equalsIgnoreCase(trimmed)) {
            return null;
        }

        return trimmed;
    }
}
