package com.cicada.player.utils.media;

import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Build;
import android.util.Range;

import java.util.ArrayList;
import java.util.List;

public class MediaCodecUtils {

    private static final String TAG = MediaCodecUtils.class.getSimpleName();


    /**
     * @param mime
     * @param secure 是否需要安全解码
     * @return
     */
    public static synchronized List<MediaCodecInfo> getCodecInfos(String mime, boolean secure, MediaFormat format) {

        if (allDecoders == null) {
            allDecoders = getDeviceDecodecs();
        }

        List<MediaCodecInfo> mediaCodecInfos = new ArrayList<>();

        for (MediaCodecInfo mediaCodecInfo : allDecoders) {

            String codecMimeType = getCodecMimeType(mediaCodecInfo, mime);
            if (codecMimeType == null) {
//                Logger.v(TAG, mediaCodecInfo.getName() + " not support mime : " + mime);
                continue;
            }

            MediaCodecInfo.CodecCapabilities capabilities = mediaCodecInfo.getCapabilitiesForType(codecMimeType);
            boolean secureMatch = isSecureSupport(secure, capabilities, codecMimeType);
            if (!secureMatch) {
//                Logger.v(TAG, mediaCodecInfo.getName() + " not support secure : " + secure);
                continue;
            }

            boolean formatSupport = isFormatSupport(format, capabilities, codecMimeType);
            if (!formatSupport) {
//                Logger.v(TAG, mediaCodecInfo.getName() + " not support format : " + format);
                continue;
            }

            mediaCodecInfos.add(mediaCodecInfo);
        }

        return mediaCodecInfos;
    }

    private static boolean isFormatSupport(MediaFormat format, MediaCodecInfo.CodecCapabilities capabilities, String codecMimeType) {
        boolean isVideo = codecMimeType.startsWith("video");
        boolean isAudio = codecMimeType.startsWith("audio");
        if (isVideo) {
            int width = getFormatInteger(format, MediaFormat.KEY_WIDTH, -1);
            int height = getFormatInteger(format, MediaFormat.KEY_HEIGHT, -1);
            if (width <= 0 || height <= 0) {
                return true;
            }

            boolean isFormatSupported = false;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
                int tmpWidth = Math.max(width, height);
                int tmpHeight = Math.min(width, height);
                MediaCodecInfo.VideoCapabilities videoCapabilities = capabilities.getVideoCapabilities();
                if (videoCapabilities == null) {
                    isFormatSupported = false;
                } else {
                    Range<Integer> supportedWidths = videoCapabilities.getSupportedWidths();
                    Range<Integer> supportedHeights = videoCapabilities.getSupportedHeights();
                    isFormatSupported = supportedWidths.contains(tmpWidth) && supportedHeights.contains(tmpHeight);
                }
            } else {
                isFormatSupported = true;
            }

            return isFormatSupported;

        } else if (isAudio) { // Audio
            boolean isFormatSupported = false;
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.LOLLIPOP) {
                isFormatSupported = true;
            } else {
                MediaCodecInfo.AudioCapabilities audioCapabilities = capabilities.getAudioCapabilities();
                if (audioCapabilities == null) {
                    isFormatSupported = false;
                } else {
                    int sampleRate = getFormatInteger(format, MediaFormat.KEY_SAMPLE_RATE, -1);
                    int channelCount = getFormatInteger(format, MediaFormat.KEY_CHANNEL_COUNT, -1);

                    boolean sampleRateSupported = (sampleRate == -1 || audioCapabilities.isSampleRateSupported(sampleRate));
                    boolean channelCountSupported = (channelCount == -1 || audioCapabilities.getMaxInputChannelCount() >= channelCount);

                    isFormatSupported = sampleRateSupported && channelCountSupported;
                }
            }
            return isFormatSupported;
        }

        return false;

    }

    private static int getFormatInteger(MediaFormat format, String name, int defaultValue) {
        int value = defaultValue;
        if (format.containsKey(name)) {
            value = format.getInteger(name);
        }
        return value;
    }

    private static boolean isSecureSupport(boolean secure, MediaCodecInfo.CodecCapabilities capabilities, String codecMimeType) {

        boolean secureSupported = false;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT) {
            secureSupported = capabilities.isFeatureSupported("secure-playback");
        } else {
            secureSupported = "video/avc".equals(codecMimeType);
        }

        boolean secureRequired = false;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            secureRequired = capabilities.isFeatureRequired("secure-playback");
        } else {
            secureRequired = false;
        }

        if ((!secure && secureRequired) || (secure && !secureSupported)) {
            return false;
        }

        boolean secureDecodersExplicit = Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP;
        if ((secureDecodersExplicit && secure == secureSupported)
                || (!secureDecodersExplicit && !secure)) {
            return true;
        }
        return true;
    }

    /**
     * 设备上有没有<b>硬件</b>解码器能解这个 mime 的编码。
     *
     * <p>只认硬件解码器：API 29+ 用 {@link MediaCodecInfo#isHardwareAccelerated()}；
     * 更老的系统用编解码器名前缀排除纯软解实现（OMX.google. / c2.android. /
     * OMX.android.，与 ExoPlayer 的 MediaCodecUtil 是同一套判据）。软件解码器一律
     * 不算 —— 内核侧的"同分辨率优先硬解"要靠这个答案把解不了的编码让开。
     *
     * <p>复用已经缓存的 allDecoders 列表，不会重复枚举 MediaCodecList。
     *
     * @param mime 例如 "video/avc"、"video/hevc"、"video/av01"、"video/x-vnd.on2.vp9"
     * @return 有硬件解码器 = true；确定没有 = false
     */
    public static synchronized boolean isHardwareDecodeSupported(String mime) {
        if (mime == null) {
            return false;
        }

        if (allDecoders == null) {
            allDecoders = getDeviceDecodecs();
        }

        for (MediaCodecInfo mediaCodecInfo : allDecoders) {
            if (getCodecMimeType(mediaCodecInfo, mime) == null) {
                continue;
            }

            if (isHardwareCodec(mediaCodecInfo)) {
                return true;
            }
        }

        return false;
    }

    /**
     * 【本机上能解这个 mime 的**硬件**解码器名】—— 返回值就是
     * {@link MediaCodecInfo#getName()}（例如 {@code c2.qti.hevc.decoder}、
     * {@code OMX.qcom.video.decoder.hevc}）。
     *
     * <p>与上面的 {@link #isHardwareDecodeSupported(String)} 用**同一份**判据与**同一份**
     * 缓存的解码器列表（{@code isHardwareCodec()} + {@code allDecoders}），所以两边不会
     * 出现"一个说有硬解、一个说不出名字"的矛盾。</p>
     *
     * <p>调用方须知：它回答的是"**设备上有**哪个硬解组件能解这个编码"（取枚举顺序里的第一个），
     * 不一定就是内核此刻真正选中的那一个组件 —— 内核选中的那个名字只在 SDK 自己的 JNI 解码
     * 线程里（{@code utils/media/MediaCodecDecoder}），应用层读不到。所以它只适合放在
     * "SoC / 板级型号也拿不到"之后的最后一级，且**只在确认真的是硬解时**才用。</p>
     *
     * @param mime 例如 "video/avc"、"video/hevc"、"video/av01"、"video/x-vnd.on2.vp9"
     * @return 硬件解码器名；确定没有时 null
     */
    public static synchronized String findHardwareDecoderName(String mime) {
        if (mime == null) {
            return null;
        }

        if (allDecoders == null) {
            allDecoders = getDeviceDecodecs();
        }

        for (MediaCodecInfo mediaCodecInfo : allDecoders) {
            if (getCodecMimeType(mediaCodecInfo, mime) == null) {
                continue;
            }

            if (!isHardwareCodec(mediaCodecInfo)) {
                continue;
            }

            String name = mediaCodecInfo.getName();
            if (name != null && name.length() > 0) {
                return name;
            }
        }

        return null;
    }

    private static boolean isHardwareCodec(MediaCodecInfo info) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            return info.isHardwareAccelerated();
        }

        String name = info.getName();
        if (name == null) {
            return false;
        }

        return !name.startsWith("OMX.google.") && !name.startsWith("c2.android.")
                && !name.startsWith("OMX.android.");
    }


    private static List<MediaCodecInfo> allDecoders;

    private static List<MediaCodecInfo> getDeviceDecodecs() {
        List<MediaCodecInfo> allCodecs = new ArrayList<>();

        MediaCodecInfo[] codecInfos = null;
        int codecNums = 0;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            MediaCodecList mediaCodecList = new MediaCodecList(MediaCodecList.ALL_CODECS);
            codecInfos = mediaCodecList.getCodecInfos();
            codecNums = codecInfos.length;
        } else {
            codecNums = MediaCodecList.getCodecCount();
        }

        for (int i = 0; i < codecNums; i++) {
            MediaCodecInfo info = null;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
                info = codecInfos[i];
            } else {
                info = MediaCodecList.getCodecInfoAt(i);
            }

            if (info.isEncoder()) {
                continue;
            } else {
                allCodecs.add(info);
            }
        }
        return allCodecs;
    }

    private static String getCodecMimeType(
            MediaCodecInfo info,
            String mimeType) {
        String[] supportedTypes = info.getSupportedTypes();
        for (String supportedType : supportedTypes) {
            if (supportedType.equalsIgnoreCase(mimeType)) {
                return supportedType;
            }
        }

        return null;
    }

}
