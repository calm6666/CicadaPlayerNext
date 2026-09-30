
#define LOG_TAG "AFVTBDecoder"

#include "AppleVideoToolBox.h"
#include "codec/utils_ios.h"
#include "utils/errors/framework_error.h"
#include "utils/timer.h"
#include <cinttypes>
#include <utils/frame_work_log.h>
#include <utils/pixelBufferConvertor.h>
extern "C" {
#include <libavcodec/avcodec.h>
};
#include <base/media/AVAFPacket.h>

#include "video_tool_box_utils.h"

using namespace std;
namespace Cicada {
    static const int IOS_OUTPUT_CACHE_FOR_B_FRAMES = 8;
    AFVTBDecoder AFVTBDecoder::se(0);

    AFVTBDecoder::AFVTBDecoder()
    {
#if TARGET_OS_IPHONE
        RegisterIOSNotificationObserver(this, (int) (IOSResignActive | IOSBecomeActive));
        mActive = IOSNotificationManager::Instance()->GetActiveStatus() != 0;
        //FIX ME: uv22 not supported by render, set to 420v or support in render
#if TARGET_OS_SIMULATOR
        mVTOutFmt = AF_PIX_FMT_YUV420P;
#endif
#endif
        mName = "VideoToolBox";
        mFlags = DECFLAG_PASSTHROUGH_INFO | DECFLAG_HW;
    }

    AFVTBDecoder::~AFVTBDecoder()
    {
        if (mIsDummy) {
            return;
        }

        close();
#if TARGET_OS_IPHONE
        RemoveIOSNotificationObserver(this);
#endif
    }

    bool AFVTBDecoder::is_supported(enum AFCodecID codec)
    {
        if (codec != AF_CODEC_ID_H264 && codec != AF_CODEC_ID_HEVC) {
            return false;
        }

#ifndef NDEBUG

        if (codec == AF_CODEC_ID_HEVC) {
#if TARGET_OS_IPHONE

            if (__builtin_available(iOS 11.0, *))
#else
            if (__builtin_available(macOS 10.13, *))
#endif
            {
                return VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC);
            } else {
                return false;
            }
        }

#endif
        return true;
    }

    bool AFVTBDecoder::is_hardware_decode_supported(enum AFCodecID codec)
    {
        /*
         * VideoToolbox 侧的能力问题用 VTIsHardwareDecodeSupported() 回答
         * （iOS 11.0+ / macOS 10.13+，与 is_supported() 用的是同一个 API；
         * 这里**不**像 is_supported() 那样只在 NDEBUG 未定义时检查 —— 那个
         * #ifndef NDEBUG 是历史遗留，真实能力查询不能只在 debug 构建里生效）。
         */
        if (codec == AF_CODEC_ID_H264) {
            /* H.264 硬解是 VideoToolbox 的基础能力，系统版本再低也有。 */
            return true;
        }

        if (codec == AF_CODEC_ID_HEVC) {
#if TARGET_OS_IPHONE
            if (__builtin_available(iOS 11.0, *))
#else
            if (__builtin_available(macOS 10.13, *))
#endif
            {
                return VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC) != 0;
            } else {
                return false;
            }
        }

        if (codec == AF_CODEC_ID_MPEG4) {
#if TARGET_OS_IPHONE
            if (__builtin_available(iOS 11.0, *))
#else
            if (__builtin_available(macOS 10.13, *))
#endif
            {
                return VTIsHardwareDecodeSupported(kCMVideoCodecType_MPEG4Video) != 0;
            } else {
                return false;
            }
        }

        /*
         * 其余编码（AV1 / VP9 / ...）：VideoToolbox 没有对应的 CMVideoCodecType，
         * 内核侧查不到 —— 按约定返回 true（"查不到 = 视为支持"）：这时只按压缩效率
         * 序排，绝不因为一次查不到就把整条流降级。
         */
        return true;
    }

    int AFVTBDecoder::init_decoder_internal()
    {
        mInputCount = 0;
        Stream_meta *meta = ((Stream_meta *) (*(mPInMeta)));
        int ret = gen_framework_errno(error_class_codec, codec_error_video_not_support);

        if (meta->codec == AF_CODEC_ID_H264) {
            mVideoCodecType = kCMVideoCodecType_H264;
        } else if (meta->codec == AF_CODEC_ID_MPEG4) {
            mVideoCodecType = kCMVideoCodecType_MPEG4Video;
        } else if (meta->codec == AF_CODEC_ID_HEVC) {
            mVideoCodecType = kCMVideoCodecType_HEVC;
        } else {
            return ret;
        }

        if (mVideoCodecType == kCMVideoCodecType_HEVC) {
#if TARGET_OS_IPHONE

            if (Cicada::GetIosVersion() < 11.0) {
                AF_LOGE("version below 11.0, not support hevc ios harddecoder.");
                return ret;
            }

#endif
        }

        //        if (!VTIsHardwareDecodeSupported(mVideoCodecType))
        //            return ret;
        //        mGotFirstFrame = false;

        if (meta->extradata && meta->extradata_size > 0 && mActive) {
            ret = createDecompressionSession(meta->extradata, meta->extradata_size, meta->width, meta->height);

            if (ret < 0) {
                AF_LOGE("createDecompressionSession error\n");
                return ret;
            }
        }

        resetPocInfo();
        return 0;
    }

    int AFVTBDecoder::init_decoder(const Stream_meta *meta, void *voutObsr, uint64_t flags, const DrmInfo* drmInfo)
    {
        if (meta->pixel_fmt == AF_PIX_FMT_YUV422P || meta->pixel_fmt == AF_PIX_FMT_YUVJ422P || meta->interlaced == InterlacedType_YES) {
            return -ENOTSUP;
        }

        mPInMeta = unique_ptr<streamMeta>(new streamMeta(meta));
        Stream_meta *pInmeta = (Stream_meta *) (*(mPInMeta));
        /*
         * extradata 的所有权归 streamMeta: streamMeta 析构时 releaseMeta() 用的是
         * free()(framework/utils/mediaFrame.c:26), 所以这里必须用 av_malloc 配自己的一份,
         * 不能用 new[](不配对的分配器, 释放时是 UB)。先把浅拷贝来的指针清掉,
         * 免得分配失败时 streamMeta 去 free 调用方的内存。
         */
        pInmeta->extradata = nullptr;
        pInmeta->extradata_size = 0;

        if (meta->extradata != nullptr && meta->extradata_size > 0) {
            pInmeta->extradata = static_cast<uint8_t *>(av_malloc(meta->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));

            if (pInmeta->extradata == nullptr) {
                return -ENOMEM;
            }

            memcpy(pInmeta->extradata, meta->extradata, meta->extradata_size);
            pInmeta->extradata_size = meta->extradata_size;
        }
        pInmeta->lang = nullptr;
        pInmeta->description = nullptr;
        pInmeta->meta = nullptr;
        pInmeta->keyUrl = nullptr;
        pInmeta->keyFormat = nullptr;

        parserInfo info{0};
        if (parser_extradata(pInmeta->extradata, pInmeta->extradata_size, &info, pInmeta->codec) >= 0) {
            pInmeta->width = info.width;
            pInmeta->height = info.height;
        }

        mInputCount = 0;

        if (meta->codec == AF_CODEC_ID_H264 || meta->codec == AF_CODEC_ID_HEVC) {
            mParser = unique_ptr<bitStreamParser>(new bitStreamParser());
            int ret = mParser->init(pInmeta);

            if (ret < 0) {
                mParser = nullptr;
            } else {
                mBUsePoc = true;
            }
        }

        int ret = init_decoder_internal();

        if (ret < 0) {
            return ret;
        }
        return 0;
    }

    void AFVTBDecoder::close_decoder()
    {
        if (!mVTDecompressSessionRef) {
            return;
        }

        VTDecompressionSessionInvalidate(mVTDecompressSessionRef);
        CFRelease(mVTDecompressSessionRef);
        mVTDecompressSessionRef = nullptr;
        CFRelease(mVideoFormatDesRef);
        mVideoFormatDesRef = nullptr;

        if (mDecoder_spec) {
            CFRelease(mDecoder_spec);
            mDecoder_spec = nullptr;
        }
        mPocErrorCount = 0;
    };

    int AFVTBDecoder::createDecompressionSession(uint8_t *pData, int size, int width, int height)
    {
        int rv = 0;

        if (!mVideoFormatDesRef) {
            rv = createVideoFormatDesc(pData, size, width, height, mDecoder_spec, mVideoFormatDesRef);
        }

        if (rv != 0) {
            AF_LOGE("createVideoFormatDescFromPacket failed rv %d", rv);
            return -EINVAL;
        }

        CFDictionaryRef buf_attr = videotoolbox_buffer_attributes_create(width, height, outPutFormat);
        VTDecompressionOutputCallbackRecord outputCallbackRecord;
        outputCallbackRecord.decompressionOutputCallback = Cicada::AFVTBDecoder::decompressionOutputCallback;
        outputCallbackRecord.decompressionOutputRefCon = this;
        rv = VTDecompressionSessionCreate(kCFAllocatorDefault, mVideoFormatDesRef, mDecoder_spec, buf_attr, &outputCallbackRecord,
                                          &mVTDecompressSessionRef);

        if (rv != 0) {
            AF_LOGE("create seesion failed rv %d", rv);
            CFRelease(buf_attr);

            if (mVideoFormatDesRef) {
                CFRelease(mVideoFormatDesRef);
                mVideoFormatDesRef = nullptr;
            }

            if (mDecoder_spec) {
                CFRelease(mDecoder_spec);
                mDecoder_spec = nullptr;
            }

            return -EINVAL;
        }

        CFRelease(buf_attr);
        return 0;
    }

    static void dict_set_string(CFMutableDictionaryRef dict, CFStringRef key, const char *value)
    {
        CFStringRef string;
        string = CFStringCreateWithCString(NULL, value, kCFStringEncodingASCII);
        CFDictionarySetValue(dict, key, string);
        CFRelease(string);
    }

    static void dict_set_boolean(CFMutableDictionaryRef dict, CFStringRef key, bool value)
    {
        CFDictionarySetValue(dict, key, value ? kCFBooleanTrue : kCFBooleanFalse);
    }

    static void dict_set_data(CFMutableDictionaryRef dict, CFStringRef key, uint8_t *value, uint64_t length)
    {
        CFDataRef data;
        data = CFDataCreate(NULL, value, (CFIndex) length);
        CFDictionarySetValue(dict, key, data);
        CFRelease(data);
    }

    static void dict_set_object(CFMutableDictionaryRef dict, CFStringRef key, CFTypeRef *value)
    {
        CFDictionarySetValue(dict, key, value);
    }

    static void dict_set_i32(CFMutableDictionaryRef dict, CFStringRef key, int32_t value)
    {
        CFNumberRef number;
        number = CFNumberCreate(nullptr, kCFNumberSInt32Type, &value);
        CFDictionarySetValue(dict, key, number);
        CFRelease(number);
    }
    static void dict_set_f32(CFMutableDictionaryRef dict, CFStringRef key, float value)
    {
        CFNumberRef number;
        number = CFNumberCreate(nullptr, kCFNumberFloat32Type, &value);
        CFDictionarySetValue(dict, key, number);
        CFRelease(number);
    }

    int AFVTBDecoder::createVideoFormatDesc(const uint8_t *pData, int size, int width, int height, CFDictionaryRef &decoder_spec,
                                            CMFormatDescriptionRef &cm_fmt_desc)
    {
        OSStatus status;
        int ret;

        if (pData == nullptr || size == 0) {
            return -EINVAL;
        }

        CFMutableDictionaryRef par =
                CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFMutableDictionaryRef atoms =
                CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFMutableDictionaryRef extensions =
                CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        /* CVPixelAspectRatio dict */
        Stream_meta *meta = ((Stream_meta *) (*(mPInMeta)));


        float HSpacing = 1.0;
        float VSpacing = 1.0;

        if (meta->displayHeight && meta->displayWidth) {
            VSpacing = (float) meta->displayHeight / height;
            HSpacing = (float) meta->displayWidth / width;
        }
        dict_set_f32(par, CFSTR("HorizontalSpacing"), HSpacing);
        dict_set_f32(par, CFSTR("VerticalSpacing"), VSpacing);


        /* SampleDescriptionExtensionAtoms dict */
        switch (mVideoCodecType) {
            case kCMVideoCodecType_H264:
                dict_set_data(atoms, CFSTR("avcC"), (uint8_t *) pData, size);
                break;

            case kCMVideoCodecType_HEVC:
                dict_set_data(atoms, CFSTR("hvcC"), (uint8_t *) pData, size);
                break;

            case kCMVideoCodecType_MPEG4Video:
                dict_set_data(atoms, CFSTR("esds"), (uint8_t *) pData, size);
                break;

            default:
                break;
        }

        /* Extensions dict */
        dict_set_string(extensions, CFSTR("CVImageBufferChromaLocationBottomField"), "left");
        dict_set_string(extensions, CFSTR("CVImageBufferChromaLocationTopField"), "left");
        dict_set_boolean(extensions, CFSTR("FullRangeVideo"), FALSE);
        dict_set_object(extensions, CFSTR("CVPixelAspectRatio"), (CFTypeRef *) par);
        dict_set_object(extensions, CFSTR("SampleDescriptionExtensionAtoms"), (CFTypeRef *) atoms);

        if (width <= 0 || height <= 0) {
            parserInfo info{0};
            Stream_meta *meta = ((Stream_meta *) (*(mPInMeta)));
            ret = parser_extradata(pData, size, &info, meta->codec);

            if (ret < 0) {
                CFRelease(extensions);
                CFRelease(atoms);
                CFRelease(par);
                return -EINVAL;
            }

            width = info.width;
            height = info.height;
        }

        status = CMVideoFormatDescriptionCreate(nullptr, mVideoCodecType, width, height, extensions, &cm_fmt_desc);
        CFRelease(extensions);
        CFRelease(atoms);
        CFRelease(par);

        if (status == 0) {
            return 0;
        } else {
            return -EINVAL;
        }
    }

#if 0
    int AFVTBDecoder::createVideoFormatDesc(const uint8_t *pData, int size, int width, int height, CFDictionaryRef &decoder_spec,
                                            CMFormatDescriptionRef &cm_fmt_desc)
    {
        OSStatus status = -1;

        if (pData == nullptr || size == 0) {
            return -EINVAL;
        }

        parserInfo info{};
        decoder_spec = videotoolbox_decoder_config_create(mVideoCodecType, pData, size, &info);

        if (!width || !height) {
            width = info.width;
            height = info.height;
        }

        // TODO: add width and height
        status = CMVideoFormatDescriptionCreate(kCFAllocatorDefault,
                                                mVideoCodecType,
                                                width,
                                                height,
                                                decoder_spec, // Dictionary of extension
                                                &cm_fmt_desc);

        if (!cm_fmt_desc) {
            if (decoder_spec) {
                CFRelease(decoder_spec);
            }

            return -1;
        }

        return 0;
    }
#endif

    int AFVTBDecoder::enqueue_decoder(unique_ptr<IAFPacket> &pPacket)
    {
        if (mResignActive) {
            close_decoder();
            std::lock_guard<std::mutex> lock(mActiveStatusMutex);
            mResignActive = false;
        }

        if (!mActive) {
            return -EAGAIN;
        }

        if (mVTDecompressSessionRef == nullptr) {
            gen_recovering_queue();
            init_decoder_internal();
        }

        int64_t startDecodeTime = af_getsteady_ms();

        while (!mRecoveringQueue.empty()) {
            int ret = enqueue_decoder_internal(mRecoveringQueue.front());

            if (ret != -EAGAIN) {
                mRecoveringQueue.pop();
            } else {
                return -EAGAIN;
            }

            if (!mRunning || !mActive) {
                return -EAGAIN;
            }

            if (af_getsteady_ms() - startDecodeTime > 200) {
                return -EAGAIN;
            }
        }

        if (pPacket == nullptr) {
            VTDecompressionSessionWaitForAsynchronousFrames(mVTDecompressSessionRef);
            flushReorderQueue();
            return STATUS_EOS;
        }

        return enqueue_decoder_internal(pPacket);
    }

    int AFVTBDecoder::process_extra_data(IAFPacket *pPacket)
    {
        if (pPacket == nullptr || pPacket->getInfo().extra_data == nullptr) {
            return 0;
        }

        Stream_meta *meta = ((Stream_meta *) (*(mPInMeta)));

        if (meta->extradata_size == pPacket->getInfo().extra_data_size) {
            if (memcmp(pPacket->getInfo().extra_data, meta->extradata, meta->extradata_size) == 0) {
                return 0;
            }
        }

        if (meta->extradata) {
            free(meta->extradata);
        }

        meta->extradata = static_cast<uint8_t *>(av_malloc(pPacket->getInfo().extra_data_size + AV_INPUT_BUFFER_PADDING_SIZE));
        meta->extradata_size = pPacket->getInfo().extra_data_size;
        memcpy(meta->extradata, pPacket->getInfo().extra_data, pPacket->getInfo().extra_data_size);
        unique_ptr<bitStreamParser> parser = unique_ptr<bitStreamParser>(new bitStreamParser());
        int ret = parser->init(meta);
        parser->parser(pPacket->getData(), pPacket->getSize());
        parser->getPictureSize(meta->width, meta->height);

        if (mParser) {
            mParser = std::move(parser);
        }

        CM_NULLABLE CMVideoFormatDescriptionRef videoFormatDesRef{nullptr};
        CM_NULLABLE CFDictionaryRef decoder_spec{nullptr};
        int rv = createVideoFormatDesc(pPacket->getInfo().extra_data, pPacket->getInfo().extra_data_size, meta->width, meta->height,
                                       decoder_spec, videoFormatDesRef);
        /*
         there are some bugs when reuse on iOS 14.x,eg h264 main profile to high profile
         */
        bool canReuse = true;
#if TARGET_OS_IPHONE
        if (Cicada::GetIosVersion() >= 14.0 /* && Cicada::GetIosVersion() < 15.0*/) {
            if (meta->codec == AF_CODEC_ID_H264 || meta->codec == AF_CODEC_ID_HEVC) {
                canReuse = false;
            }
        }
#endif

        if (rv == 0) {
            if (!canReuse || !VTDecompressionSessionCanAcceptFormatDescription(mVTDecompressSessionRef, videoFormatDesRef)) {
                flushReorderQueue();
                close_decoder();
                mVideoFormatDesRef = videoFormatDesRef;
                mDecoder_spec = decoder_spec;
                rv = init_decoder_internal();

                if (rv < 0) {
                    return -EINVAL;
                }
            } else {
                if (videoFormatDesRef) {
                    CFRelease(videoFormatDesRef);
                }
            }

            resetPocInfo();
        } else {
            if (videoFormatDesRef) {
                CFRelease(videoFormatDesRef);
            }

            if (decoder_spec) {
                CFRelease(decoder_spec);
            }

            return -EINVAL;
        }

        return 0;
    }

    int AFVTBDecoder::enqueue_decoder_internal(unique_ptr<IAFPacket> &pPacket)
    {
        //   AF_LOGD("mInputQueue size is %d\n", mInputQueue.size());
        //        if (mOutputQueue.size() > maxOutQueueSize) {
        //            //       AF_TRACE;
        //            return -EAGAIN;
        //        }
        if (pPacket->getInfo().flags & AF_PKT_FLAG_KEY) {
            mThrowPacket = false;
        }

        if (mThrowPacket) {
            AF_LOGE("IOS8VT: throw frame");
            enqueueError(-1, pPacket->getInfo().pts);
            return 0;
        }

        if (!mActive) {
            AF_LOGD("ios bg decoder inactive return resign active");
            return -EAGAIN;
        }

        if (pPacket->getInfo().extra_data) {
            int ret = process_extra_data(pPacket.get());

            if (ret < 0) {
                return ret;
            }
        }

        CMBlockBufferRef newBuffer = nullptr;

        if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, pPacket->getData(), pPacket->getSize(), kCFAllocatorNull, nullptr, 0,
                                               pPacket->getSize(), 0, &newBuffer) != 0) {
            AF_LOGE("failed to create mblockbuffer");
            return -EINVAL;
        }

        CMSampleTimingInfo timeInfo;
        timeInfo.presentationTimeStamp = CMTimeMake(pPacket->getInfo().pts, 1000000);
        timeInfo.duration = CMTimeMake(pPacket->getInfo().duration, 1000000);
        timeInfo.decodeTimeStamp = CMTimeMake(pPacket->getInfo().dts, 1000000);
        CMSampleBufferRef sampleBuffer = nullptr;

        if (0 != CMSampleBufferCreate(kCFAllocatorDefault, newBuffer, TRUE, nullptr, nullptr, mVideoFormatDesRef, 1, 1, &timeInfo, 0,
                                      nullptr, &sampleBuffer)) {
            CFRelease(newBuffer);// add by yager
            AF_LOGE("failed to CMSampleBufferCreate");
            return -EINVAL;
        }

        VTDecodeInfoFlags infoFlags;
        VTDecodeFrameFlags flags = 0;

        if (pPacket->getDiscard()) {
            flags |= kVTDecodeFrame_DoNotOutputFrame;
        }

        IAFPacket *packet = pPacket.release();
        int rv = VTDecompressionSessionDecodeFrame(mVTDecompressSessionRef, sampleBuffer, flags, packet, &infoFlags);
        CFRelease(newBuffer);
        CFRelease(sampleBuffer);

        if (rv == kVTInvalidSessionErr || rv == kVTVideoDecoderMalfunctionErr) {
            AF_LOGW("kVTInvalidSessionErr\n");
            pPacket.reset(packet);
            close_decoder();
            return -EAGAIN;
        }

        if (rv != noErr) {
            // TODO: enqueue error
            /*
             * VTDecompressionSessionDecodeFrame WILL HAVE callback when return error,
             * so can't push it to queue
             */
            //     push_to_recovery_queue(unique_ptr<IAFPacket>(packet));
        }

        if (rv != noErr && !(infoFlags & kVTDecodeInfo_FrameDropped)) {
            if (rv == kVTVideoDecoderBadDataErr) {
                mThrowPacket = true;
                AF_LOGE("IOSVT: decode failed kVTVideoDecoderBadDataErr");
                // bad data error, return warning.
                return -EINVAL;
            } else {
                AF_LOGE("IOSVT: decode failed status=%d", (int) rv);
                return -EINVAL;
            }
        }

        return 0;
    }

    bool AFVTBDecoder::isErrorPoc(int poc, bool keyFrame) const
    {
        if (mVideoCodecType == kCMVideoCodecType_HEVC) {
            return false;
        }
        // for h264
        return poc < 0 || (poc == 0 && !keyFrame);
    }
    void AFVTBDecoder::resetPocInfo()
    {
        if (mVideoCodecType == kCMVideoCodecType_HEVC) {
            mPocDelta = 1;
            mOutputPoc = INT64_MIN;
        } else {
            mPocDelta = 2;
            mOutputPoc = 0;
        }
    }

    void AFVTBDecoder::onDecoded(IAFPacket *packet, std::unique_ptr<PBAFFrame> frame, OSStatus status)
    {
        if (packet == nullptr) {
            return;
        }

        if (!packet->getDiscard() && (status != noErr || !frame)) {
            AF_LOGW("AFVTBDecoder decoder error %d\n", status);
            if (status == kVTVideoDecoderBadDataErr) {
                mThrowPacket = true;
                AF_LOGE("IOSVT: decode failed kVTVideoDecoderBadDataErr");
            }
            enqueueError(status, packet->getInfo().pts);
        }

        bool keyFrame = (packet->getInfo().flags & AF_PKT_FLAG_KEY);
        int64_t mapKey = packet->getInfo().pts;

        bool bIDRFrame = keyFrame;
        // must parser poc
        if (mBUsePoc && mPocErrorCount < MAX_POC_ERROR) {
            assert(mParser != nullptr);
            mParser->parser(packet->getData(), packet->getSize());
            int poc = mParser->getPOC();

            // for h265, set mOutputPoc to poc -1, to let out put this frame immediately
            if (mOutputPoc == INT64_MIN && keyFrame) {
                mOutputPoc = poc - 1;
            }
            // 265 IDR poc is 0
            bIDRFrame = poc == 0;
            if (isErrorPoc(poc, keyFrame)) {
                AF_LOGI("error poc is %d\n", poc);
                if (++mPocErrorCount >= MAX_POC_ERROR) {
                    AF_LOGE("too much poc error, disable reorder use poc\n");
                }
                push_to_recovery_queue(unique_ptr<IAFPacket>(packet));
                return;
            }
            // for h264
            if (poc == 1) {
                mPocDelta = 1;
            }

            mapKey = poc;
        }

        if (frame == nullptr) {
            push_to_recovery_queue(unique_ptr<IAFPacket>(packet));

            if (mBUsePoc) {
                mOutputPoc = mapKey;
            }

            return;
        }
//        assert(!mBUsePoc || bIDRFrame || mOutputPoc < mapKey);
        frame->getInfo().timePosition = packet->getInfo().timePosition;
        frame->getInfo().utcTime = packet->getInfo().utcTime;

        if (bIDRFrame && mPocErrorCount < MAX_POC_ERROR) {
            flushReorderQueue();
            if (mVTOutFmt == AF_PIX_FMT_YUV420P) {
                auto *avframe = (AVAFFrame *) (*frame);
                avframe->getInfo().timePosition = packet->getInfo().timePosition;
                avframe->getInfo().utcTime = packet->getInfo().utcTime;
                std::unique_lock<std::mutex> uMutex(mReorderMutex);
                mReorderedQueue.push(unique_ptr<IAFFrame>(avframe));
            } else {
                std::unique_lock<std::mutex> uMutex(mReorderMutex);
                mReorderedQueue.push(std::move(frame));
            }
            if (mBUsePoc) {
                mOutputPoc = mapKey;
            }
        } else {
            std::unique_lock<std::mutex> uMutex(mReorderMutex);
            mReorderFrameMap[mapKey] = std::move(frame);
        }

        push_to_recovery_queue(unique_ptr<IAFPacket>(packet));
    }

    void AFVTBDecoder::decompressionOutputCallback(void *CM_NULLABLE decompressionOutputRefCon, void *CM_NULLABLE sourceFrameRefCon,
                                                   OSStatus status, VTDecodeInfoFlags infoFlags, CM_NULLABLE CVImageBufferRef imageBuffer,
                                                   CMTime presentationTimeStamp, CMTime presentationDuration)
    {
        auto *decoder = static_cast<AFVTBDecoder *>(decompressionOutputRefCon);
        IAFPacket *packet = static_cast<IAFPacket *>(sourceFrameRefCon);
        unique_ptr<PBAFFrame> pbafFrame{};

        if (infoFlags & kVTDecodeInfo_FrameDropped) {
            AF_LOGI("kVTDecodeInfo_FrameDropped");
        }
        if (status == kVTInvalidSessionErr || status == kVTVideoDecoderMalfunctionErr) {
            // the packet will be send to decoder again on those error, so do nothing here
            return;
        }

        // TODO: why status is 1 when recovering

        if (status != noErr || imageBuffer == nullptr || packet->getDiscard()) {
            // TODO: enqueue error
            return decoder->onDecoded(packet, nullptr, status);
        }
        // Strip the attachments added by VT. They are likely wrong.
        //           CVBufferRemoveAllAttachments(imageBuffer);
        pixelBufferConvertor::UpdateColorInfo(((Stream_meta *) (*(decoder->mPInMeta)))->color_info, imageBuffer);

        int64_t duration = presentationDuration.value * (1000000.0 / presentationDuration.timescale);
        int64_t pts = presentationTimeStamp.value * (1000000.0 / presentationTimeStamp.timescale);
        pbafFrame = unique_ptr<PBAFFrame>(new PBAFFrame(imageBuffer, pts, duration, ((Stream_meta *) (*(decoder->mPInMeta)))->color_info));
        return decoder->onDecoded(packet, std::move(pbafFrame), status);
    }

    int AFVTBDecoder::dequeue_decoder(unique_ptr<IAFFrame> &pFrame)
    {
        //   AF_LOGD("mOutputQueue size is %d\n", mOutputQueue.size());
        std::unique_lock<std::mutex> uMutex(mReorderMutex);

        if (!mReorderedQueue.empty()) {
            pFrame = std::move(mReorderedQueue.front());
            mReorderedQueue.pop();
            return 0;
        }

        if (mBUsePoc) {
            if (mReorderFrameMap.empty()) {
                return -EAGAIN;
            }

            int64_t poc = (*mReorderFrameMap.begin()).first;

            if (isErrorPoc((int) poc, true) || isErrorPoc((int) mOutputPoc, true) ||
                (mReorderFrameMap.size() >= IOS_OUTPUT_CACHE_FOR_B_FRAMES) || (poc - mOutputPoc == mPocDelta)) {
                if (mReorderFrameMap.size() >= IOS_OUTPUT_CACHE_FOR_B_FRAMES) {
                    AF_LOGW("mReorderFrameMap.size() is %lld\n", mReorderFrameMap.size());
                }
                mOutputPoc = poc;
                //AF_LOGD("out put poc is %lld -- %lld\n", mOutputPoc.load(), mReorderFrameMap.size());
            } else {
                return -EAGAIN;
            }
        } else if (mReorderFrameMap.size() < IOS_OUTPUT_CACHE_FOR_B_FRAMES) {
            return -EAGAIN;
        }

        if (mVTOutFmt == AF_PIX_FMT_YUV420P) {
            PBAFFrame *pbafFrame = (*(mReorderFrameMap.begin())).second.get();
            auto *frame = (AVAFFrame *) (*pbafFrame);
            pFrame = unique_ptr<IAFFrame>(frame);
        } else {
            pFrame = std::move(*(mReorderFrameMap.begin())).second;
        }

        mReorderFrameMap.erase(mReorderFrameMap.begin());
        return 0;
    }

    void AFVTBDecoder::gen_recovering_queue()
    {
        // push the RecoveringQueue to RecoveryQueue
        while (!mRecoveringQueue.empty()) {
            mRecoveryQueue.push(std::move(mRecoveringQueue.front()));
            mRecoveringQueue.pop();
        }

        assert(mRecoveringQueue.empty());

        while (!mRecoveryQueue.empty()) {
            mRecoveringQueue.push(std::move(mRecoveryQueue.front()));
            mRecoveryQueue.pop();
        }
    }

    void AFVTBDecoder::push_to_recovery_queue(std::unique_ptr<IAFPacket> pPacket)
    {
        pPacket->setDiscard(true);
        //      std::lock_guard<std::mutex> lock(mActiveStatusMutex);

        if (pPacket->getInfo().flags & AF_PKT_FLAG_KEY) {
            while (!mRecoveryQueue.empty()) {
                mRecoveryQueue.pop();
            }
        }

        mRecoveryQueue.push(std::move(pPacket));
    }

    void AFVTBDecoder::flushReorderQueue()
    {
        std::unique_lock<std::mutex> uMutex(mReorderMutex);
        while (!mReorderFrameMap.empty()) {
            if (mVTOutFmt == AF_PIX_FMT_YUV420P) {
                PBAFFrame *pbafFrame = (*(mReorderFrameMap.begin())).second.get();
                auto *frame = (AVAFFrame *) (*pbafFrame);
                frame->getInfo().timePosition = pbafFrame->getInfo().timePosition;
                mReorderedQueue.push(unique_ptr<IAFFrame>(frame));
            } else {
                mReorderedQueue.push(std::move(*(mReorderFrameMap.begin())).second);
            }

            mReorderFrameMap.erase(mReorderFrameMap.begin());
        }
    }

    void AFVTBDecoder::flush_decoder()
    {
        if (mVTDecompressSessionRef && mInputCount > 0 && mActive) {
            VTDecompressionSessionWaitForAsynchronousFrames(mVTDecompressSessionRef);
            mInputCount = 0;
        }

        {
            std::unique_lock<std::mutex> uMutex(mReorderMutex);
            mReorderFrameMap.clear();
        }

        std::lock_guard<std::mutex> lock(mActiveStatusMutex);

        while (!mRecoveringQueue.empty()) {
            mRecoveringQueue.pop();
        }

        while (!mRecoveryQueue.empty()) {
            mRecoveryQueue.pop();
        }

        while (!mReorderedQueue.empty()) {
            mReorderedQueue.pop();
        }
        mPocErrorCount = 0;
        resetPocInfo();
    }

    void AFVTBDecoder::AppWillResignActive()
    {
        std::lock_guard<std::mutex> lock(mActiveStatusMutex);
        AF_LOGD("ios bg decoder appWillResignActive");
        mActive = false;
        mResignActive = true;
        //        if (mDecodedHandler) {
        //            mDecodedHandler->OnDecodedMsgHandle(CICADA_VDEC_WARNING_IOS_RESIGN_ACTIVE);
        //        }
    }

    void AFVTBDecoder::AppDidBecomeActive()
    {
        std::lock_guard<std::mutex> lock(mActiveStatusMutex);
        AF_LOGD("ios bg decoder appDidBecomeActive");
        if (mResignActive) {
            mThrowPacket = true;
        }
        mActive = true;
    }

    int AFVTBDecoder::get_decoder_recover_size()
    {
        std::lock_guard<std::mutex> lock(mActiveStatusMutex);
        return mRecoveryQueue.size();
    }
    int AFVTBDecoder::setPixelBufferFormat(OSType format)
    {
        switch (format) {
            case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
            case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
            case kCVPixelFormatType_420YpCbCr8Planar:
            case kCVPixelFormatType_32BGRA:
            case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
                outPutFormat = format;
                return 0;
            default:
                AF_LOGE("not support pixel format %u\n", format);
                return -1;
        }
    }
    void AFVTBDecoder::decoder_updateMetaData(const Stream_meta *meta)
    {
        Stream_meta *pMeta = ((Stream_meta *) (*(mPInMeta)));

        /*
         * 【画质切换必须重建 VT 会话 —— macOS 上"切档之后一帧都解不出来"的根因】
         *
         * SMPAVDeviceManager 复用解码器时(SMPAVDeviceManager.cpp 的 "reuse decoder" 分支)
         * 只做 flush() + updateMetaData() + pause(false), **不会**再走 open()/init_decoder();
         * 而复用判据 DecoderHandle::match() 只看 codec/设备/flags/dstFormat/drm —— 同一条
         * 视频的两个码率档 codec 都是 HEVC, 于是 480p 切 1080p 也算"匹配", 走到这里。
         *
         * 但 VideoToolbox 的会话把格式描述(CMVideoFormatDescription, 由 extradata 的
         * VPS/SPS/PPS + 宽高建出来)在创建那一刻**钉死**了: 拿旧会话去解新档的包, 每个
         * 关键帧都回 kVTVideoDecoderBadDataErr(-12909), 接着 mThrowPacket 又把中间所有帧
         * 丢掉, 下一个关键帧照样失败 —— 日志里 1000+ 错误帧、零输出、两次重建后报致命错误,
         * 就是这条链。
         *
         * 所以这里必须判"格式变了没有"(extradata 或宽高), 变了就更新 meta 并关掉旧会话,
         * 立刻按新格式描述重建。
         */
        bool formatChanged = false;

        if (meta->extradata != nullptr && meta->extradata_size > 0) {
            const bool sameExtra = pMeta->extradata != nullptr &&
                                   pMeta->extradata_size == meta->extradata_size &&
                                   memcmp(pMeta->extradata, meta->extradata, meta->extradata_size) == 0;
            formatChanged = !sameExtra;
        }

        if (pMeta->width != meta->width || pMeta->height != meta->height) {
            formatChanged = true;
        }

        /* 这三项与格式描述无关, 但也要跟着新档走(显示宽高比 / HDR 信息) */
        pMeta->displayHeight = meta->displayHeight;
        pMeta->displayWidth = meta->displayWidth;
        pMeta->color_info = meta->color_info;

        if (!formatChanged) {
            return;
        }

        pMeta->width = meta->width;
        pMeta->height = meta->height;

        if (meta->extradata != nullptr && meta->extradata_size > 0) {
            void *newExtra = av_malloc(meta->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);

            if (newExtra != nullptr) {
                memcpy(newExtra, meta->extradata, meta->extradata_size);
                free(pMeta->extradata);
                pMeta->extradata = static_cast<uint8_t *>(newExtra);
                pMeta->extradata_size = meta->extradata_size;
            }
        }

        AF_LOGI("video format changed (%dx%d, extradata %d bytes): rebuilding the VideoToolbox session\n",
                meta->width, meta->height, meta->extradata_size);

        flushReorderQueue();
        close_decoder();
        resetPocInfo();
        /*
         * 立刻按新格式描述建会话; 万一失败也不要紧 —— enqueue_decoder() 里那句
         * "mVTDecompressSessionRef == nullptr 就 init_decoder_internal()" 还会再补一次。
         */
        const int ret = init_decoder_internal();

        if (ret < 0) {
            AF_LOGE("rebuilding the VideoToolbox session after the format change failed: %d\n", ret);
        }
    }

}// namespace Cicada
