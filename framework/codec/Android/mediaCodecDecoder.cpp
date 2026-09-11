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
        mDecoder = new MediaCodec_Decoder();
    }

    mediaCodecDecoder::~mediaCodecDecoder() {
        mCSDList.clear();
        delete mDecoder;
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
            // TODO: value
            return -ENOSPC;
        } else if (index == MC_INFO_TRYAGAIN) {
            // 对齐 ExoPlayer：codec 失效（典型：surface 销毁后 flush 导致华为
            // codec 进入僵尸态，dequeueInput 永远 TRY_AGAIN）时，检测到连续
            // 拿不到输入缓冲即优雅关闭解码器并结束解码线程，由上层
            // （demo 的 reload/重建）恢复；避免在同一包上死循环刷屏
            mInputTryAgainCount++;
            if (mInputTryAgainCount > 100) {   // 100 × ~10ms ≈ 1 秒
                AF_LOGE("codec input stuck, close decoder for recreation");
                close_decoder();
                return STATUS_EOS;
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
                pFrame = unique_ptr<AFMediaCodecFrame>(
                        new AFMediaCodecFrame(IAFFrame::FrameTypeVideo, index,
                                              [this, framePts](int index, bool render) {
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
                                                  mDecoder->releaseOutputBuffer(index, render);
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
                                           static_cast<jobject>(mVideoOutObser));
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
