//
// Created by moqi on 2019-08-20.
//
#define LOG_TAG "decoderFactory"

#include <drm/DrmInfo.h>
#include "decoderFactory.h"
#include <mutex>
#include <utils/frame_work_log.h>

#if defined(__APPLE__)
/* 判断要不要走 FFmpeg 的 videotoolbox 硬解（零拷贝用），见 property.h。 */
#include <utils/globalSettings.h>
#include <utils/property.h>
#endif

#ifdef ANDROID

    #include "Android/mediaCodecDecoder.h"

#endif

#ifdef __APPLE__

    #include "Apple/AppleVideoToolBox.h"

#endif

#ifdef __OHOS__

    #include "OHOS/OhosAVCodecDecoder.h"

#endif

#include "avcodecDecoder.h"

using namespace Cicada;
using namespace std;

/*
 * ==================== 设备硬解能力：平台分派与缓存 ====================
 *
 * 这一块是**唯一**知道"本平台怎么问硬解能力"的地方，所有平台宏都关在这个文件里
 * （decoderFactory.cpp 本来就是解码器的平台分派点，createBuildIn 里已经有同样的
 * ANDROID / __APPLE__ / __OHOS__ / _WIN32 分支）。上层（mediaPlayer/abr）只看到
 * decoderFactory::isHardwareDecodeSupported(enum AFCodecID) 这一个无平台宏的接口。
 *
 * 三态在内部收敛：1 = 明确有，0 = 明确没有，-1 = 未知；对外 -1 按"视为支持"返回 true。
 */
namespace {

    /*
     * 平台分派。返回 1 / 0 / -1，语义见上。
     *
     * 注意各分支的条件与下面的 createBuildIn() 保持一一对应 —— "能交给哪个平台解码器"
     * 就是"哪个平台解码器能回答硬解能力"。
     */
    int probeHardwareDecodeSupport(enum AFCodecID codec)
    {
#if defined(ANDROID) && defined(ENABLE_MEDIA_CODEC_DECODER)
        /*
         * Android：问 MediaCodecList —— mediaCodecDecoder 里的实现在 Java 侧只认
         * 硬件解码器（MediaCodecInfo.isHardwareAccelerated()，老 API 用编解码器名
         * 前缀判断），MIME 映射 video/avc->H.264、video/hevc->H.265、
         * video/av01->AV1、video/x-vnd.on2.vp9->VP9、video/mp4v-es->MPEG-4。
         */
        return mediaCodecDecoder::isHardwareDecodeSupported(codec) ? 1 : 0;
#elif defined(__APPLE__) && defined(ENABLE_VTB_DECODER)
        /*
         * Apple：VideoToolbox 的 VTIsHardwareDecodeSupported（iOS 11+ / macOS 10.13+）。
         * 即使应用打开了 "video.decoder.ffmpeg_videotoolbox"（只是换一层包装，解码仍然
         * 由 VideoToolbox 做），可硬解的编码集合不变，所以这里统一问 AFVTBDecoder。
         * 它答不上来的编码（VideoToolbox 没有对应 CMVideoCodecType 的，例如 AV1）返回
         * "未知" -> 对外视为支持，不会误降。
         */
        return AFVTBDecoder::is_hardware_decode_supported(codec) ? 1 : 0;
#elif defined(__OHOS__) && defined(ENABLE_OHOS_AVCODEC_DECODER)
        /*
         * OpenHarmony：硬解后端是系统 OH_AVCodec，createBuildIn() 里能交给
         * OhosAVCodecDecoder 的视频编码就是下面这几个 —— 这里必须是**同一份列表**，
         * 将来加编码两处一起改。列表之外的编码返回"未知"（OH_AVCodec 的能力无法在
         * 内核侧静态枚举），按"视为支持"处理。
         */
        return (codec == AF_CODEC_ID_H264 || codec == AF_CODEC_ID_HEVC
                || codec == AF_CODEC_ID_MPEG4 || codec == AF_CODEC_ID_VP9
                || codec == AF_CODEC_ID_AV1) ? 1 : -1;
#elif defined(ENABLE_AVCODEC_DECODER) && defined(CICADA_HW_DEVICE_TYPE)
        /*
         * 桌面（Windows D3D11VA / Linux VAAPI，以及 Apple 上没编 VTB 时的
         * FFmpeg VideoToolbox）：查这一份 FFmpeg 构建里有没有该编码的硬解配置
         * （avcodec_get_hw_config），**不创建设备**。
         *
         * 不创建设备是刻意的：创建 D3D11/VAAPI 设备有真实成本，而且会为一个不一定
         * 真正解码的编码白占 GPU 资源；"FFmpeg 有硬解配置、但这块显卡/驱动解不了"
         * 这种情况本来就有兜底 —— 解码器创建失败时 CreateVideoDecoder 会 release +
         * 用软解重建（SuperMediaPlayer.cpp 的 hw->sw 回退），与本次改动无关。
         */
        return avcodecDecoder::is_hardware_decode_supported(codec) ? 1 : 0;
#else
        /* 没有可问的平台后端：未知，对外按"视为支持"处理（只用效率序）。 */
        (void) codec;
        return -1;
#endif
    }

    /*
     * 探测结果缓存。MediaCodecList 枚举要跨 JNI 遍历设备上全部编解码器，
     * VTIsHardwareDecodeSupported 也不是免费的，而编码种类是个很小的固定集合，
     * 所以每个编码只探测一次。
     *
     * 用函数内静态量（C++11 起初始化本身线程安全）+ 一把锁保护 map：
     * mediaInfoGet 回调线程和 ABR 线程都会问，必须是线程安全的。
     * 这里没有任何计时器/看门狗，也没有失效逻辑 —— 设备能力在一次进程生命周期内不变。
     */
    struct HardwareDecodeCapabilityCache {
        std::mutex mutex;
        std::map<int, bool> known;
    };

    HardwareDecodeCapabilityCache &hardwareDecodeCapabilityCache()
    {
        static HardwareDecodeCapabilityCache cache;
        return cache;
    }

}// namespace

bool decoderFactory::isHardwareDecodeSupported(enum AFCodecID codec)
{
    /*
     * 编码未知（AF_CODEC_ID_NONE，例如清单里没给出 codecs 字符串）：能力也未知，
     * 按"视为支持"处理 —— 不能因为认不出编码就把这一路排到最后。
     */
    if (codec == AF_CODEC_ID_NONE) {
        return true;
    }

    HardwareDecodeCapabilityCache &cache = hardwareDecodeCapabilityCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    const int key = (int) codec;
    const auto iter = cache.known.find(key);

    if (iter != cache.known.end()) {
        return iter->second;
    }

    const int probed = probeHardwareDecodeSupport(codec);
    /* -1（未知）对外收敛成 true："查不到就当支持"。 */
    const bool supported = (probed != 0);
    cache.known[key] = supported;
    AF_LOGI("hardware decode capability: codec=%d probe=%d -> supported=%d (cached)\n",
            key, probed, (int) supported);
    return supported;
}

unique_ptr<Cicada::IDecoder> decoderFactory::create(const Stream_meta& meta, uint64_t flags, int maxSize,
                                                    const DrmInfo *drmInfo)
{
    IDecoder *decoder = codecPrototype::create(meta, flags, maxSize, drmInfo);

    if (decoder != nullptr) {
        return unique_ptr<IDecoder>(decoder);
    }

    return createBuildIn(meta.codec, flags, drmInfo);
}

unique_ptr<IDecoder> decoderFactory::createBuildIn(const AFCodecID &codec, uint64_t flags,
                                                   const DrmInfo *drmInfo)
{
    if (flags & DECFLAG_HW) {
#ifdef ANDROID
#ifdef ENABLE_MEDIA_CODEC_DECODER
        return std::unique_ptr<IDecoder>(new mediaCodecDecoder());
#endif
#endif
#ifdef __APPLE__
        /*
         * Apple 默认用自研的 AFVTBDecoder。但如果应用显式打开了
         * "video.decoder.ffmpeg_videotoolbox"（Qt 集成为了零拷贝会打开它），
         * 就走 FFmpeg 的 videotoolbox hwaccel：那条路交出来的是 CVPixelBuffer，
         * macOS 上可以用 CVMetalTextureCache 直接包成 MTLTexture 交给 Metal
         * 场景图，硬解 + 零拷贝。默认不打开，其它 Apple 应用的行为完全不变。
         */
        if (globalSettings::getSetting().getProperty(PROPERTY_KEY_DECODER_FFMPEG_VT) == "ON") {
            return unique_ptr<IDecoder>(new avcodecDecoder());
        }
#endif
#ifdef __APPLE__
#ifdef ENABLE_VTB_DECODER

        if (AFVTBDecoder::is_supported(codec)) {
            return unique_ptr<IDecoder>(new AFVTBDecoder());
        }

#endif
#endif
#ifdef __OHOS__
#ifdef ENABLE_OHOS_AVCODEC_DECODER
        // OH_AVCodec hardware decode (surface or buffer mode); unsupported
        // codecs fall through to the software decoder below.
        if (codec == AF_CODEC_ID_H264 || codec == AF_CODEC_ID_HEVC
                || codec == AF_CODEC_ID_MPEG4 || codec == AF_CODEC_ID_VP9
                || codec == AF_CODEC_ID_AV1 || codec == AF_CODEC_ID_AAC) {
            return std::unique_ptr<IDecoder>(new OhosAVCodecDecoder());
        }
#endif
#endif
#if defined(_WIN32) || defined(__linux__)
        /*
         * Neither Windows nor Linux has a separate hardware decoder class:
         * FFmpeg's hwaccel runs inside avcodecDecoder, with the device type
         * chosen per platform (D3D11VA on Windows, VAAPI on Linux - see
         * CICADA_HW_DEVICE_TYPE in avcodecDecoder.h) and every decoded surface
         * downloaded back to system memory.
         *
         * avcodecDecoder::open() decides at run time, through
         * avcodec_get_hw_config(), whether the hwaccel is actually present in the
         * linked FFmpeg, and otherwise keeps decoding in software. That is why
         * every codec can be handed to it instead of keeping a hardcoded list.
         *
         * Leaving Linux out of this branch was a real bug: a DECFLAG_HW request
         * fell through to the software case below, matched nothing, and returned
         * nullptr, so CreateVideoDecoder reported codec_error_video_not_support
         * (-512) and the framework quietly switched to the software decoder -
         * the VAAPI support in avcodecDecoder was never reached at all.
         */
        return unique_ptr<IDecoder>(new avcodecDecoder());
#endif
    }

    if (flags & DECFLAG_SW) {
#ifdef ENABLE_AVCODEC_DECODER
        return unique_ptr<IDecoder>(new avcodecDecoder());
#endif
    }

    return nullptr;
}

