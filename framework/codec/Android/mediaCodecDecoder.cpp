#define LOG_TAG "mediaCodecDecoder"

#include "mediaCodecDecoder.h"
#include <utils/frame_work_log.h>
#include <utils/timer.h>
#include <utils/Android/systemUtils.h>
#include <cassert>
#include <map>
#include <utils/ffmpeg_utils.h>
#include <drm/DrmHandlerPrototype.h>
#include <drm/WideVineDrmHandler.h>

extern "C" {
#include <libavutil/intreadwrite.h>
#include <utils/errors/framework_error.h>
}

#define  MAX_INPUT_SIZE 4
using namespace std;
namespace Cicada {

    typedef struct blackModelDevice {
        AFCodecID codec;
        string model;
    } blackModelDevice;
    blackModelDevice blackList[] = {
            {AF_CODEC_ID_H264, "2014501"},
            {AF_CODEC_ID_HEVC, "OPPO R9tm"},
            {AF_CODEC_ID_HEVC, "OPPO A59s"},
    };

    mediaCodecDecoder mediaCodecDecoder::se(0);

    mediaCodecDecoder::mediaCodecDecoder() {
        AF_LOGD("android decoder use jni");
        mName = "VD.mediaCodec";
        mFlags |= DECFLAG_HW;
        /*
         * 平台绑定由工厂按**平台能力**选择（不是配置开关）：
         *   · API >= 28 且 libmediandk 提供 setAsyncNotifyCallback ⇒ ndk-async
         *     （数据面全部在 C++：每帧 0 次 JNI、0 次 Java 堆分配、0 次多余拷贝）；
         *   · 否则（API 24~27，或 secure/占位 surface 这类必须 Java 能力的场景）⇒ java-async
         *     （现有实现，同样是异步回调）。
         * 两者都实现 IAndroidCodecBinding，本文件其余代码与状态机语义不变。
         */
        mDecoder = createAndroidCodecBinding();
        AF_LOGI("[ndk-codec] mediaCodecDecoder created: binding=%s\n",
                (mDecoder != nullptr) ? mDecoder->bindingName() : "none");
        /* B5-4：帧释放回调的共享状态（见头文件里的完整说明）。 */
        mReleaseState = std::make_shared<FrameReleaseState>();
        mReleaseState->decoder = (mDecoder != nullptr) ? mDecoder.get() : nullptr;
    }

    mediaCodecDecoder::~mediaCodecDecoder() {
        mCSDList.clear();
        /*
         * B5-4：**在同一把锁内**置"已销毁"并 delete JNI 包装 —— 与帧释放回调的临界区
         * 互斥：要么回调先跑完（那时对象还活着），要么回调看到 alive==false 直接短路。
         * 不持锁就 delete 的话，"旧代帧等下一次 VSync 才析构"就是真实的 use-after-free
         * （真机路径：切档提交把旧解码器移入 retired → 播放线程同步销毁它 →
         *  渲染线程下一次 VSync 才析构它解出的帧）。
         */
        if (mReleaseState != nullptr) {
            std::lock_guard<std::mutex> lock(mReleaseState->mutex);
            mReleaseState->alive = false;
            mReleaseState->decoder = nullptr;
            mDecoder.reset();
        } else {
            mDecoder.reset();
        }
    }

    void mediaCodecDecoder::invalidateFrameReleases() {
        if (mReleaseState == nullptr) {
            return;
        }

        std::lock_guard<std::mutex> lock(mReleaseState->mutex);
        ++mReleaseState->flushGen;
    }

    bool mediaCodecDecoder::checkSupport(const Stream_meta &meta, uint64_t flags, int maxSize) {
        AFCodecID codec = meta.codec;
        // 硬解白名单扩展：主流视频格式只要芯片支持就走 MediaCodec 硬解，
        // 不支持时 configure 失败，播放器会自动回退软解（SuperMediaPlayer 的
        // CreateVideoDecoder(false) 重试链路）
        if (codec != AF_CODEC_ID_H264 && codec != AF_CODEC_ID_HEVC
            && codec != AF_CODEC_ID_MPEG4 && codec != AF_CODEC_ID_VP8
            && codec != AF_CODEC_ID_VP9 && codec != AF_CODEC_ID_AV1
            && codec != AF_CODEC_ID_AAC) {
            return false;
        }

        string version = get_android_property("ro.build.version.sdk");
        int sdkVersion = atoi(version.c_str());

        if (sdkVersion < 16) {
            return false;
        }

        if (sdkVersion < 21) {
            if (flags & DECFLAG_ADAPTIVE || codec == AF_CODEC_ID_HEVC
                //maxSize will be judged by the codec. Tianmao box supports large width/height.
                /*|| maxSize > 1920*/
            ) {
                return false;
            }
        }

        // VP9 硬解自 Android 7.0(API 24) 提供；AV1 硬解自 Android 10(API 29) 提供，
        // 版本不满足直接交给软解，避免无效的 MediaCodec 创建尝试
        if ((codec == AF_CODEC_ID_VP9 && sdkVersion < 24) ||
            (codec == AF_CODEC_ID_AV1 && sdkVersion < 29)) {
            return false;
        }

        string model = get_android_property("ro.product.model");
        for (auto device : blackList) {
            if (device.codec == codec && device.model == model) {
                AF_LOGI("device %d@%s is in black list\n", device.codec, device.model.c_str());
                return false;
            }
        }

        return true;
    }

    /*
     * 【设备硬解能力查询】编码 -> MediaCodec MIME。
     *
     * 与 init_decoder() 里那份 mMime 映射**必须一致**（video/avc、video/hevc、
     * video/mp4v-es、video/x-vnd.on2.vp8、video/x-vnd.on2.vp9、video/av01）；
     * 这里单独抽一个小函数，是因为能力查询要在**没有 Stream_meta**的情况下按编码问。
     * 认不出来的编码返回 nullptr = "MediaCodec 不管这个编码"。
     */
    static const char *mimeForHardwareProbe(enum AFCodecID codec) {
        switch (codec) {
            case AF_CODEC_ID_H264:
                return "video/avc";
            case AF_CODEC_ID_HEVC:
                return "video/hevc";
            case AF_CODEC_ID_MPEG4:
                return "video/mp4v-es";
            case AF_CODEC_ID_VP8:
                return "video/x-vnd.on2.vp8";
            case AF_CODEC_ID_VP9:
                return "video/x-vnd.on2.vp9";
            case AF_CODEC_ID_AV1:
                return "video/av01";
            default:
                return nullptr;
        }
    }

    /*
     * 【设备硬解能力查询】问 MediaCodecList：设备上有没有**硬件**解码器能解这个编码。
     *
     * 真正的探测在 Java 侧（com.cicada.player.utils.media.MediaCodecUtils
     * .isHardwareDecodeSupported(String mime)）：它复用已经缓存的
     * getDeviceDecodecs() 列表，只认硬件解码器 —— API 29+ 用
     * MediaCodecInfo.isHardwareAccelerated()，更老的 API 用编解码器名前缀排除
     * OMX.google. / c2.android. / OMX.android. 这些纯软解实现。
     *
     * JNI 那一层放在 MediaCodec_Decoder::isHardwareDecodeSupported()（类句柄和方法 ID
     * 在 JNI_OnLoad 里缓存好，见该函数的说明；内核线程上现查类会失败）。
     *
     * 语义（与 decoderFactory::isHardwareDecodeSupported 的约定一致）：
     *   true  = Java 侧明确回答"有硬件解码器"，**或**查询本身失败/不可用（查不到就当支持）；
     *   false = Java 侧明确回答"没有"。
     * 编码不在 MediaCodec 的 MIME 表里时也返回 true（查不到）。
     *
     * 探测结果由 decoderFactory::isHardwareDecodeSupported() 缓存，本函数每次调用
     * 都会真的跨一次 JNI，不要直接高频调用它。
     */
    bool mediaCodecDecoder::isHardwareDecodeSupported(enum AFCodecID codec) {
        const char *mime = mimeForHardwareProbe(codec);

        if (mime == nullptr) {
            return true;
        }

        return MediaCodec_Decoder::isHardwareDecodeSupported(mime);
    }

    int mediaCodecDecoder::init_decoder(const Stream_meta *meta, void *voutObsr, uint64_t flags,
                                        const DrmInfo *drmInfo) {
        if (meta->pixel_fmt == AF_PIX_FMT_YUV422P || meta->pixel_fmt == AF_PIX_FMT_YUVJ422P) {
            return -ENOSPC;
        }

        if (!checkSupport(*meta, flags, max(meta->height, meta->width)) ||
                (drmInfo != nullptr && !is_drmSupport(drmInfo))) {
            return -ENOSPC;
        }

        if (flags & DECFLAG_DIRECT) {
            mFlags |= DECFLAG_OUT;
        }
        /*
         * B2 的前提条件（**缺了它整个占位 Surface 方案不会生效**）：
         * mFlags（IDecoder.h 里的 int mFlags）不是 open 的入参，它只会被本处
         * 与构造函数显式 OR 进来 —— 之前只把 DECFLAG_DIRECT 转成 DECFLAG_OUT，
         * DECFLAG_PLACEHOLDER_SURFACE 从来没有进过 mFlags。
         *
         * 真机证据（2026-09-24 那份 323KB 日志）：pending 解码器明明是带
         * DECFLAG_PLACEHOLDER_SURFACE 建的（`pending video decoder is up … flag=0x12a`
         * 含 0x100），但 configDecoder() 里 `(mFlags & DECFLAG_PLACEHOLDER_SURFACE)`
         * 恒为 0 ⇒ Java 收到 usePlaceholderSurface=false ⇒ configure(null) 退成
         * ByteBuffer 模式 ⇒ 提交时 setOutputSurface 抛
         * "codec was not configured for an output surface"（ret=-2），
         * 新解码器永远接不上真 Surface。
         */
        if (flags & DECFLAG_PLACEHOLDER_SURFACE) {
            mFlags |= DECFLAG_PLACEHOLDER_SURFACE;
        }

        if (meta->codec == AF_CODEC_ID_H264) {
            codecType = CODEC_VIDEO;
            mMime = "video/avc";
        } else if (meta->codec == AF_CODEC_ID_HEVC) {
            codecType = CODEC_VIDEO;
            mMime = "video/hevc";
        } else if (meta->codec == AF_CODEC_ID_MPEG4) {
            codecType = CODEC_VIDEO;
            mMime = "video/mp4v-es";
        } else if (meta->codec == AF_CODEC_ID_VP8) {
            codecType = CODEC_VIDEO;
            mMime = "video/x-vnd.on2.vp8";
        } else if (meta->codec == AF_CODEC_ID_VP9) {
            codecType = CODEC_VIDEO;
            mMime = "video/x-vnd.on2.vp9";
        } else if (meta->codec == AF_CODEC_ID_AV1) {
            codecType = CODEC_VIDEO;
            mMime = "video/av01";
        } else if (meta->codec == AF_CODEC_ID_AAC) {
            codecType = CODEC_AUDIO;
            mMime = "audio/mp4a-latm";
        } else {
            AF_LOGE("codec is %d, not support", meta->codec);
            return -ENOSPC;
        }

        mMeta = *meta;
        mVideoOutObser = voutObsr;
        updateCSD(meta, meta->extradata, meta->extradata_size);

        // CSD 必须在 configure 之前下发给 Java 层（addCsdInfo 把 csd-0 写入
        // MediaFormat）。此前只在 enqueue_decoder 里下发（configure 之后），
        // AAC 缺 AudioSpecificConfig 永远不出帧，导致音视频同步卡死；
        // H264/HEVC 走 AnnexB 裸流不依赖 CSD 才未暴露
        if (!mCSDList.empty()) {
            mDecoder->setCodecSpecificData(mCSDList);
            mCSDList.clear();
        }

        lock_guard<recursive_mutex> func_entry_lock(mFuncEntryMutex);

        if (drmInfo != nullptr) {
            if (mRequireDrmHandlerCallback != nullptr) {
                mDrmHandler = std::dynamic_pointer_cast<WideVineDrmHandler>(mRequireDrmHandlerCallback(*drmInfo));
                assert(mDrmHandler != nullptr);
            }

            int ret = initDrmHandler();
            if (ret == -EAGAIN) {
                return 0;
            } else if (ret < 0) {
                return ret;
            }
        }

        return configDecoder();
    }

    void mediaCodecDecoder::updateCSD(const Stream_meta *meta, const uint8_t *extradata, int extradata_size)
    {
        if (meta->codec == AF_CODEC_ID_HEVC) {

            if (extradata == nullptr || extradata_size == 0) {
                return;
            }

            uint8_t *vps_data = nullptr;
            uint8_t *sps_data = nullptr;
            uint8_t *pps_data = nullptr;
            int vps_data_size = 0;
            int sps_data_size = 0;
            int pps_data_size = 0;

            int ret = parse_h265_extraData(CodecID2AVCodecID(AF_CODEC_ID_HEVC), extradata, extradata_size, &vps_data, &vps_data_size,
                                           &sps_data, &sps_data_size, &pps_data, &pps_data_size, &naluLengthSize);
            if (ret >= 0) {
                mCSDList.clear();
                std::unique_ptr<CodecSpecificData> csd0 = std::unique_ptr<CodecSpecificData>(new CodecSpecificData());

                const int data_size =
                        vps_data_size + sps_data_size + pps_data_size;
                char data[data_size];

                memcpy(data, vps_data, vps_data_size);
                memcpy(data + vps_data_size, sps_data, sps_data_size);
                memcpy(data + vps_data_size + sps_data_size, pps_data, pps_data_size);

                csd0->setScd("csd-0", data, data_size);
                mCSDList.push_back(move(csd0));

                av_free(vps_data);
                av_free(sps_data);
                av_free(pps_data);
            }
        } else if (meta->codec == AF_CODEC_ID_H264) {

            if (extradata == nullptr || extradata_size == 0) {
                return;
            }
            uint8_t *sps_data = nullptr;
            uint8_t *pps_data = nullptr;
            int sps_data_size = 0;
            int pps_data_size = 0;

            int ret = parse_h264_extraData(CodecID2AVCodecID(AF_CODEC_ID_H264), extradata, extradata_size, &sps_data, &sps_data_size,
                                           &pps_data, &pps_data_size, &naluLengthSize);

            if (ret >= 0) {
                mCSDList.clear();
                std::unique_ptr<CodecSpecificData> csd0 = std::unique_ptr<CodecSpecificData>(new CodecSpecificData());
                csd0->setScd("csd-0", sps_data, sps_data_size);
                mCSDList.push_back(move(csd0));
                std::unique_ptr<CodecSpecificData> csd1 = std::unique_ptr<CodecSpecificData>(new CodecSpecificData());
                csd1->setScd("csd-1", pps_data, pps_data_size);
                mCSDList.push_back(move(csd1));

                av_free(sps_data);
                av_free(pps_data);
            }

        } else if (meta->codec == AF_CODEC_ID_AAC) {
            if (extradata == nullptr || extradata_size == 0) {
                isADTS = true;
                //ADTS, has no extra data . MediaCodec MUST set csd when decode aac

                int samplingFreq[] = {
                        96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
                        16000, 12000, 11025, 8000
                };

                // Search the Sampling Frequencies
                int sampleIndex = -1;
                for (int i = 0; i < 12; ++i) {
                    if (samplingFreq[i] == mMeta.samplerate) {
                        sampleIndex = i;
                        break;
                    }
                }
                if (sampleIndex < 0) {
                    return;
                }

                const size_t kCsdLength = 2;
                char csd[kCsdLength] = {0};
                csd[0] = (meta->profile + 1) << 3 | sampleIndex >> 1;
                csd[1] = (sampleIndex & 0x01) << 7 | meta->channels << 3;

                mCSDList.clear();
                std::unique_ptr<CodecSpecificData> csd0 = std::unique_ptr<CodecSpecificData>(new CodecSpecificData());
                csd0->setScd("csd-0", csd, kCsdLength);
                mCSDList.push_back(move(csd0));
            } else {
                isADTS = false;
                mCSDList.clear();
                std::unique_ptr<CodecSpecificData> csd0 = std::unique_ptr<CodecSpecificData>(new CodecSpecificData());
                csd0->setScd("csd-0", (void *) extradata, extradata_size);
                mCSDList.push_back(move(csd0));
            }
            return;
        } else if (meta->codec == AF_CODEC_ID_VP8) {
            // VP8 无 CSD（in-band），无需处理
            return;
        } else if (meta->codec == AF_CODEC_ID_VP9 || meta->codec == AF_CODEC_ID_AV1
                   || meta->codec == AF_CODEC_ID_MPEG4) {
            // mp4 封装下 FFmpeg 把 vpcC/av1C/ESDS 盒子内容放进 extradata，
            // MediaCodec 需要它们作为 csd-0；mkv/webm 无 extradata（in-band）直接跳过
            if (extradata == nullptr || extradata_size == 0) {
                return;
            }
            mCSDList.clear();
            std::unique_ptr<CodecSpecificData> csd0 = std::unique_ptr<CodecSpecificData>(new CodecSpecificData());
            csd0->setScd("csd-0", (void *) extradata, extradata_size);
            mCSDList.push_back(move(csd0));
        } else {
            return;
        }
    }

    void mediaCodecDecoder::flush_decoder() {
        lock_guard<recursive_mutex> func_entry_lock(mFuncEntryMutex);
        mOutputFrameCount = 0;

        if (!mbInit) {
            return;
        }

        if (mInputFrameCount <= 0) {
            return;
        }

        {
            std::lock_guard<std::mutex> l(mFlushInterruptMuex);
            mFlushState = 1;
            /*
             * B5-4：平台 flush 之后所有"已出队未释放"的 buffer index 都会失效，
             * 而它们的帧还压在渲染器队列里（下一次 VSync 才析构）。先在锁内把这一代
             * 作废，回调就不会拿着失效 index 去 release（消掉
             * `releaseOutputBuffer fail Error 0xfffffff3`）。
             */
            invalidateFrameReleases();
            int ret = mDecoder->flush();
            AF_LOGI("clearCache. ret %d, flush state %d", ret, mFlushState);
        }

        mDiscardPTSSet.clear();
        mInputFrameCount = 0;
    }

    void mediaCodecDecoder::close_decoder() {
        lock_guard<recursive_mutex> func_entry_lock(mFuncEntryMutex);

        // 关闭渲染门：解码器即将释放，暂停帧恢复状态失效
        mRenderHold = false;
        mRenderGatePts = INT64_MIN;
        mRenderGateHit = false;

        /*
         * B5-4：stop + release 之后旧 index 同样失效（而且 stop/换源路径上必然还有
         * 旧代帧压在渲染器队列里）—— 一并作废这一代，避免它们去 release 刷 E。
         * 注意它**不**释放/销毁 anything：只是让回调短路。
         */
        invalidateFrameReleases();

        // stop decoder.
        // must before destructor producer because inner thread will use surface.
        if (mbInit) {
            mFlushState = 0;
            mDecoder->stop();
            releaseDecoder();
            mbInit = false;
        }

        mInputFrameCount = 0;
    }

    int mediaCodecDecoder::setOutputSurface(void *surface) {
        if (codecType != CODEC_VIDEO) {
            return 0;
        }
        if (!mbInit) {
            // 解码器已被自愈关闭（后台 surface 被系统销毁 → ACodec 报错）。
            // 返回负值，让上层（ProcessSetViewMsg）走重建兜底
            AF_LOGI("setOutputSurface: decoder closed, need rebuild\n");
            return -1;
        }
        // 对齐 ExoPlayer 2.9.6：surface 变化（含 null，Java 侧换成内部
        // DummySurface）时调用 MediaCodec.setOutputSurface 热重绑，codec 保持
        // 运行，无需重建解码器
        lock_guard<recursive_mutex> func_entry_lock(mFuncEntryMutex);
        int ret = mDecoder->setOutputSurface(surface);
        AF_LOGI("mediaCodecDecoder setOutputSurface surface=%p ret=%d\n", surface, ret);
        return ret;
    }

    int64_t mediaCodecDecoder::getLastRenderedVideoPts() {
        return mLastRenderedVideoPts.load();
    }

    /*
     * 【追帧加速】只对视频、且解码器已初始化时下发；失败一路返回 0 —— 这是"性能点提示"，
     * 任何平台拒绝都不允许影响解码/精度。详见 IDecoder::setDecodeBoost 的说明。
     */
    int mediaCodecDecoder::setDecodeBoost(bool boost) {
        if (codecType != CODEC_VIDEO || !mbInit || mDecoder == nullptr) {
            return 0;
        }

        int ret = mDecoder->setDecodeBoost(boost);
        AF_LOGI("mediaCodecDecoder setDecodeBoost boost=%d ret=%d\n", (int) boost, ret);
        return ret;
    }

    int64_t mediaCodecDecoder::getLastRenderedVideoFrameDur() {
        int64_t last = mLastRenderedVideoPts.load();
        int64_t prev = mPrevRenderedVideoPts.load();
        if (last == INT64_MIN || prev == INT64_MIN || last <= prev) {
            return 0;
        }
        return last - prev;
    }

    void mediaCodecDecoder::setRenderGate(int64_t gatePts) {
        if (gatePts == INT64_MIN) {
            // 关闭渲染门（恢复正常渲染）
            mRenderHold = false;
            mRenderGatePts = INT64_MIN;
            mRenderGateHit = false;
        } else {
            mRenderGatePts = gatePts;
            mRenderGateHit = false;
            mRenderHold = true;
        }
    }

    bool mediaCodecDecoder::isRenderGateHit() {
        // 未启用渲染门时视为"命中"（不影响普通 seek 的完成判定）；
        // 启用期间必须等门帧实际渲染后才算命中
        return !mRenderHold.load() || mRenderGateHit.load();
    }

    void mediaCodecDecoder::releaseDecoder() {
        if (mDecoder != nullptr) {
            mDecoder->release();
        }
    }

    int mediaCodecDecoder::enqueue_decoder(unique_ptr<IAFPacket> &pPacket) {

        if (!mbInit) {
            if (mDrmHandler == nullptr) {
                // 解码器已被自愈关闭（surface 失效）：结束解码线程，
                // 避免对已释放 codec 反复 dequeue/enqueue 刷错误日志，
                // 由上层（ProcessSetViewMsg → RestartVideoDecoder）重建
                return STATUS_EOS;
            }
            int ret = initDrmHandler();
            if (ret == -EAGAIN) {
                return -EAGAIN;
            } else if (ret < 0) {
                return ret;
            } else if (ret == 0) {
                ret = configDecoder();

                if (ret < 0) {
                    return ret;
                }
            }
        }

        if (pPacket != nullptr && pPacket->getInfo().extra_data != nullptr) {
            updateCSD(&mMeta, pPacket->getInfo().extra_data, pPacket->getInfo().extra_data_size);
        }

        if (!mCSDList.empty()) {
            mDecoder->setCodecSpecificData(mCSDList);
            mCSDList.clear();
        }

        int index = mDecoder->dequeueInputBufferIndex(10000);

        if (index == MC_ERROR) {
            AF_LOGE("dequeue_in error.");
            /*
             * 输入侧硬错误说明这个 codec 已经不能用了。这里不能只返回一个负值：
             * ActiveDecoder::decode_func() 对任何非 -EAGAIN 的返回值都会把当前包
             * 丢掉并继续取下一包，于是一次故障会以每秒数百包的速度把视频包队列
             * 吃空 —— 队列队首在音频时钟不动（暂停中）时凭空前进几十秒，恢复后
             * 每一帧都被判成"太早"而不上屏（2026-09-23 22:32 日志：errorFrames
             * 恰好 1001，25fps 即 40.0 秒，正对应实测的超前 43996 ms）。
             * 按本文件已有的约定优雅关闭并结束解码线程：最多只影响当前这一包，
             * 剩下的队列保持原样，交给上层重建解码器后从正确位置继续。
             * 音频路不这样做：音频没有重建通路，关掉它只会让声音彻底消失。
             */
            if (codecType == CODEC_VIDEO) {
                close_decoder();
                return STATUS_EOS;
            }

            return -ENOSPC;
        } else if (index == MC_INFO_TRYAGAIN) {
            /*
             * 这里以前连续约 1 秒拿不到输入缓冲就 close_decoder() 结束解码线程。
             * 那个判据站不住：拿不到输入缓冲最常见的原因恰恰是正常背压 —— codec
             * 的输出缓冲还没被释放时它当然不会再给输入缓冲，而暂停期间帧队列
             * 不再消费，输出缓冲必然一直满着。于是"暂停超过 1 秒"必然把视频
             * 解码器拆掉（2026-09-23 22:37:56.581 按下暂停，22:37:59.690 就打了
             * codec input stuck，此后没有任何路径重建它，恢复后画面永久冻住、
             * 声音正常、位置照走）。
             * 主流播放器不用计时器判 codec 死活（ExoPlayer 只由 codec 报错触发
             * releaseCodec 加 maybeInitCodec）；真正僵死的 codec 由上层
             * doRender() 里那条"有包却不消费、且长时间没有帧真的上屏"的探测
             * 重建 —— 那条判据不丢包，也不会把一次暂停误判成故障。
             * 所以这里只回报 -EAGAIN：包不丢、解码器不拆，等缓冲被释放后继续。
             */
            mInputTryAgainCount++;

            if (mInputTryAgainCount == 100) {
                AF_LOGW("codec has had no input buffer for about 1 s (output buffers still held: "
                        "paused or back-pressured) — keep waiting, no teardown\n");
            }

            return -EAGAIN;
        }

        mInputTryAgainCount = 0;

        int ret = 0;

        if (index >= 0) {
//            if (pPacket != nullptr)
//                AF_LOGD("mediacodec in pts %" PRId64 " size %d", pPacket->getInfo().pts, pPacket->getSize());
            uint8_t *data = nullptr;
            int size = 0;
            int64_t pts = 0;

            if (pPacket != nullptr) {
                data = pPacket->getData();
                size = static_cast<int>(pPacket->getSize());
                pts = pPacket->getInfo().pts;

                if (pPacket->getDiscard()) {
                    mDiscardPTSSet.insert(pts);
                }
            } else {
                AF_LOGD("queue eos codecType = %d\n", codecType);
            }

            if (mDrmHandler != nullptr) {
                IAFPacket::EncryptionInfo encryptionInfo{};
                if (pPacket != nullptr) {
                    pPacket->getEncryptionInfo(&encryptionInfo);
                }

                uint8_t *new_data = nullptr;
                int new_size = 0;

                mDrmHandler->convertData(naluLengthSize, &new_data, &new_size, data, size);

                if (new_data != nullptr) {
                    data = new_data;
                    size = new_size;
                }

                ret = mDecoder->queueSecureInputBuffer(index, data, static_cast<size_t>(size),
                                                       &encryptionInfo, pts,
                                                       false);

                if (new_data != nullptr) {
                    free(new_data);
                }

            } else {
                ret = mDecoder->queueInputBuffer(index, data, static_cast<size_t>(size), pts,
                                                 false);
            }

            if (ret < 0) {
                AF_LOGE(" mDecoder->queue_in error codecType = %d\n", codecType);
            }

            mInputFrameCount++;
        }

        if (mFlushState == 1) {
            std::lock_guard<std::mutex> l(mFlushInterruptMuex);

            if (pPacket != nullptr) {
                AF_LOGI("send Frame mFlushState = 2. pts %"
                                PRId64, pPacket->getInfo().pts);
            }

            mFlushState = 2;
        }

        if (ret == 0) {
            return 0;
        } else {
            AF_LOGE("queue_in error. ret %d", ret);
            return -ENOSPC;
        }

        return ret;
    }

    int mediaCodecDecoder::dequeue_decoder(unique_ptr<IAFFrame> &pFrame) {

        if (!mbInit) {
            return -EAGAIN;
        }

        int ret;
        int index;
        // 输出轮询超时 1ms→10ms：无帧可出时线程每毫秒唤醒一次做 JNI 调用，
        // 是 MediaCodec_loop 线程 ~2-3% CPU 的来源；视频帧间隔 16~33ms，
        // 10ms 超时不影响播放节奏，但把空轮询次数降低 10 倍
        index = mDecoder->dequeueOutputBufferIndex(10000);

        if (index == MC_ERROR) {
            AF_LOGE("dequeue_out occur error. flush state %d", mFlushState);
            // codec 已失效（典型场景：surface 销毁后 ACodec 报错、或 codec
            // 内部错误）。对齐 ExoPlayer 的 releaseCodec() 自愈：优雅关闭本
            // 解码器并结束解码线程，避免死循环报错刷屏；回前台由
            // ProcessSetViewMsg → RestartVideoDecoder 用新 surface 重建。
            // mFlushState==1（flush 进行中，如 seek）时跳过，避免打断正常 flush
            if (mFlushState == 0 || mFlushState == 2) {
                close_decoder();
                return STATUS_EOS;
            }
            return MC_ERROR;
        } else if (index == MC_INFO_TRYAGAIN || index == MC_INFO_OUTPUT_BUFFERS_CHANGED) {
            return -EAGAIN;
        } else if (index == MC_INFO_OUTPUT_FORMAT_CHANGED) {
            mc_out out{};
            mDecoder->getOutput(index, &out, false);

            if (codecType == CODEC_VIDEO) {
                height = out.conf.video.height;

                if (out.conf.video.crop_bottom != MC_ERROR && out.conf.video.crop_top != MC_ERROR) {
                    height = out.conf.video.crop_bottom + 1 - out.conf.video.crop_top;
                }

                width = out.conf.video.width;

                if (out.conf.video.crop_right != MC_ERROR && out.conf.video.crop_left != MC_ERROR) {
                    width = out.conf.video.crop_right + 1 - out.conf.video.crop_left;
                }
            } else if (codecType == CODEC_AUDIO) {
                channel_count = out.conf.audio.channel_count;
                sample_rate = out.conf.audio.sample_rate;
                format = out.conf.audio.format;
            }

            return -EAGAIN;
        } else if (index >= 0) {
            mc_out out{};
            ret = mDecoder->getOutput(index, &out, codecType != CODEC_VIDEO);
            auto item = mDiscardPTSSet.find(out.buf.pts);

            if (item != mDiscardPTSSet.end()) {
                mDecoder->releaseOutputBuffer(index, false);
                mDiscardPTSSet.erase(item);
                return -EAGAIN;
            }

            if (out.b_eos) {
                return STATUS_EOS;
            }

            // AF_LOGD("mediacodec out pts %" PRId64, out.buf.pts);
            if (codecType == CODEC_VIDEO) {
                const int64_t framePts = out.buf.pts;
                /*
                 * B5-4：帧**按值持有**释放状态（shared_ptr）+ 自己所属的代。
                 * 这样即使 mediaCodecDecoder 已经析构（切档提交后旧解码器是同步销毁的），
                 * 回调也不会在已释放的对象上读成员 —— 它先在同一把锁内校验：
                 *   · 对象已销毁 / JNI 包装已销毁 → 短路；
                 *   · 平台已 flush 或已 close（index 失效）→ 短路（不进 Java）。
                 */
                std::shared_ptr<FrameReleaseState> releaseState = mReleaseState;
                const uint64_t frameGen =
                        (releaseState != nullptr) ? releaseState->flushGen.load() : 0;

                pFrame = unique_ptr<AFMediaCodecFrame>(
                        new AFMediaCodecFrame(IAFFrame::FrameTypeVideo, index,
                                              [this, framePts, releaseState, frameGen](int index, bool render) {
                                                  if (releaseState == nullptr) {
                                                      return;
                                                  }

                                                  std::lock_guard<std::mutex> lock(releaseState->mutex);

                                                  if (!releaseState->alive || releaseState->decoder == nullptr ||
                                                      frameGen != releaseState->flushGen.load()) {
                                                      /*
                                                       * 解码器已销毁，或平台已经 flush/close 过（buffer index 已失效）：
                                                       * 这次释放注定失败，直接短路 —— 不进 Java（消掉
                                                       * `releaseOutputBuffer fail Error 0xfffffff3`），也不碰
                                                       * 任何可能已释放的成员。buffer 已由平台收回，不会泄漏。
                                                       */
                                                      return;
                                                  }

                                                  // 暂停帧恢复：只放行 PTS 精确等于门值的
                                                  // 那一帧，其余帧不上屏（避免 GOP 闪帧）
                                                  if (mRenderHold.load()) {
                                                      bool gateHit = (framePts == mRenderGatePts.load());
                                                      AF_LOGI("PFR: release idx=%d pts=%" PRId64 " gate=%" PRId64 " render=%d gateHit=%d\n",
                                                              index, framePts, mRenderGatePts.load(), (int) render, (int) gateHit);
                                                      if (gateHit && render) {
                                                          mRenderGateHit = true;
                                                      }
                                                      render = render && gateHit;
                                                  }
                                                  if (render) {
                                                      mPrevRenderedVideoPts = mLastRenderedVideoPts.load();
                                                      mLastRenderedVideoPts = framePts;
                                                  }
                                                  releaseState->decoder->releaseOutputBuffer(index, render);
                                              }));
                pFrame->getInfo().video.width = width;
                pFrame->getInfo().video.height = height;
            } else if (codecType == CODEC_AUDIO) {

                assert(out.buf.p_ptr != nullptr);

                if (out.buf.p_ptr == nullptr) {
                    return -EAGAIN;
                }

                AFSampleFormat afFormat = AFSampleFormat::AF_SAMPLE_FMT_NONE;
                if (format < 0 || format == 2) {
                    afFormat = AF_SAMPLE_FMT_S16;
                } else if (format == 3) {
                    afFormat = AF_SAMPLE_FMT_U8;
                } else if (format == 4) {
                    afFormat = AF_SAMPLE_FMT_S32;
                }

                assert(afFormat != AFSampleFormat::AF_SAMPLE_FMT_NONE);

                IAFFrame::AFFrameInfo frameInfo{};
                frameInfo.audio.format = afFormat;
                frameInfo.audio.sample_rate = sample_rate;
                frameInfo.audio.channels = channel_count;

                uint8_t *data[1] = {nullptr};
                data[0] = const_cast<uint8_t *>(out.buf.p_ptr);
                int lineSize[1] = {0};
                lineSize[0] = out.buf.size;

                pFrame = unique_ptr<AVAFFrame>(
                        new AVAFFrame(frameInfo, (const uint8_t **) data, (const int *) lineSize, 1,
                                      IAFFrame::FrameTypeAudio));
                mDecoder->releaseOutputBuffer(index, false);

                pFrame->getInfo().audio.sample_rate = sample_rate;
                pFrame->getInfo().audio.channels = channel_count;
                pFrame->getInfo().audio.format = afFormat;
            }

            pFrame->getInfo().pts = out.buf.pts != -1 ? out.buf.pts : INT64_MIN;

            // TODO: get the timePosition form input packet
            pFrame->getInfo().timePosition = INT64_MIN;
            pFrame->getInfo().utcTime = INT64_MIN;
            return 0;
        } else {
            AF_LOGE("unknown error %d\n", index);
            return index;
        }
    }

    int mediaCodecDecoder::configDecoder() {

        int ret = -1;
        if (codecType == CODEC_VIDEO) {
            int angle = 0;
            if (mFlags & DECFLAG_OUT) {
                // TunnelRender 直通模式：GL 不参与旋转，交给 MediaCodec 的
                // rotation-degrees（其定义是逆时针；meta.rotate 是顺时针，需换算）
                angle = (360 - mMeta.rotate) % 360;
            }
            ret = mDecoder->configureVideo(mMime, mMeta.width, mMeta.height, angle,
                                           static_cast<jobject>(mVideoOutObser),
                                           (mFlags & DECFLAG_PLACEHOLDER_SURFACE) != 0);
        } else if (codecType == CODEC_AUDIO) {
            ret = mDecoder->configureAudio(mMime, mMeta.samplerate, mMeta.channels,isADTS);
        }

        if (ret >= 0) {
            ret = 0;
        } else {
            AF_LOGE("failed to config mDecoder rv %d", ret);
            releaseDecoder();
            ret = gen_framework_errno(error_class_codec, codec_error_video_device_error);
        }

        if (ret == 0) {
            if (mDecoder->start() == MC_ERROR) {
                AF_LOGE("mediacodec start failed.");
                return gen_framework_errno(error_class_codec, codec_error_video_device_error);
            }

            mbInit = true;
            mFlushState = 1;
        }

        return ret;
    }

    int mediaCodecDecoder::initDrmHandler() {
        mDrmHandler->open();

        int state = mDrmHandler->getState();
        if (state == SESSION_STATE_OPENED) {
            bool insecure = mDrmHandler->isForceInsecureDecoder();
            mDecoder->setForceInsecureDecoder(insecure);

            char *sessionId = nullptr;
            int sessionSize = mDrmHandler->getSessionId(&sessionId);
            mDecoder->setDrmInfo("edef8ba9-79d6-4ace-a3c8-27dcd51d21ed", sessionId,
                                 sessionSize);
            return 0;
        } else if (state == SESSION_STATE_IDLE) {
            return -EAGAIN;
        } else if (state == SESSION_STATE_ERROR) {
            return mDrmHandler->getErrorCode();
        }
        return -EAGAIN;
    }

    bool mediaCodecDecoder::supportReuse()
    {
        return mDrmHandler != nullptr && !mDrmHandler->isErrorState();
    }
}
