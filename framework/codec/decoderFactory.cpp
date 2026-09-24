//
// Created by moqi on 2019-08-20.
//

#include <drm/DrmInfo.h>
#include "decoderFactory.h"

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

