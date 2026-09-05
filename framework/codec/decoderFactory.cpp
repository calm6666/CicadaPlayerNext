//
// Created by moqi on 2019-08-20.
//

#include <drm/DrmInfo.h>
#include "decoderFactory.h"

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
    }

    if (flags & DECFLAG_SW) {
#ifdef ENABLE_AVCODEC_DECODER
        return unique_ptr<IDecoder>(new avcodecDecoder());
#endif
    }

    return nullptr;
}

