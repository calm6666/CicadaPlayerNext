//
// Created by pingkai on 2020/12/28.
//

#include "SMPMessageControllerListener.h"

#include "SuperMediaPlayer.h"
#include "media_player_error_def.h"
#include <cinttypes>
#include <cstdio>
#include <render/video/IVideoRender.h>
#include <cassert>
#include <climits>
#include <data_source/dataSourcePrototype.h>
#include <utils/AFMediaType.h>
#include <utils/CicadaUtils.h>
#include <utils/UrlUtils.h>
#include <utils/af_string.h>
#include <utils/file/FileUtils.h>
#include <utils/timer.h>
#include <vector>
/*
 * 起播档的"硬解优先"判据：设备能不能硬解这个编码。这是 L1 的无平台宏接口，
 * 平台实现关在 framework/codec/decoderFactory.cpp 里（Android 走 MediaCodecList、
 * Apple 走 VTIsHardwareDecodeSupported、桌面走 FFmpeg 的 hwaccel 配置），
 * 结果在那边缓存，这里可以放心直接问。
 */
#include <codec/decoderFactory.h>

#define HAVE_VIDEO (mPlayer.mCurrentVideoIndex >= 0)
#define HAVE_AUDIO (mPlayer.mCurrentAudioIndex >= 0)
#define HAVE_SUBTITLE (mPlayer.mCurrentSubtitleIndex >= 0)
#define PTS_DISCONTINUE_DELTA (20 * 1000 * 1000)
using namespace Cicada;
SMPMessageControllerListener::SMPMessageControllerListener(SuperMediaPlayer &player) : mPlayer(player)
{}
SMPMessageControllerListener::~SMPMessageControllerListener() = default;

bool SMPMessageControllerListener::OnPlayerMsgIsPadding(PlayMsgType msg, MsgParam msgContent)
{
    bool padding = false;

    switch (msg) {
        case MSG_CHANGE_VIDEO_STREAM:
            /*
             * 【为什么视频切档消息**不再**延后派发】
             *
             * 这里原来是 `padding = (mVideoChangedFirstPts != INT64_MIN)` —— 那是**双解码器**
             * 时代的产物：切档要等"目标路第一个包/第一帧"到了才允许处理后面的请求。
             *
             * 单解码器模型下切档是**同步**完成的（SwitchVideo：关旧流→开新流→按流 seek→
             * 原地重建同一块解码器→落点过滤接管），入口 switchVideoStream() 自己就实现了
             * "最新请求覆盖旧请求"，**不需要**任何延后。
             *
             * 而且留着它会**把队列永久堵死**：switchVideoStream() 会把该闩置成 INT64_MAX，
             * 而它的复位判据是 `info.pts >= mVideoChangedFirstPts`（本文件 ProcessRenderedMsg）
             * —— 帧 pts 永远不可能 ≥ INT64_MAX，所以闩一次也复位不了。
             * 真机实测（2026-09-27 Qt，用户连点清晰度）：第一次切档成功，此后每一次点击
             * 都只剩 "switch stream request posted"、没有任何执行日志 —— 消息全被扣在队列里。
             *
             * 因此这里恒为 false：视频切档请求一律立即派发。
             */
            padding = false;
            break;

        case MSG_CHANGE_AUDIO_STREAM:
            padding = mPlayer.mAudioChangedFirstPts != INT64_MIN;
            break;

        case MSG_CHANGE_SUBTITLE_STREAM:
            padding = mPlayer.mSubtitleChangedFirstPts != INT64_MIN;
            break;

        case MSG_SEEKTO:
            if (mPlayer.mSeekFlag) {
                padding = true;
            }

            break;

        default:
            padding = false;
    }

    return padding;
}

void SMPMessageControllerListener::ProcessPrepareMsg()
{
    AF_LOGD("ProcessPrepareMsg start");
    int ret;

    bool manifestMode = mPlayer.mSet->manifest != nullptr;

    if (mPlayer.mSet->url.empty() && mPlayer.mBSReadCb == nullptr && !manifestMode) {
        AF_LOGD("ProcessPrepareMsg url is empty");
        mPlayer.ChangePlayerStatus(PLAYER_ERROR);
        mPlayer.mPNotifier->NotifyError(MEDIA_PLAYER_ERROR_DATASOURCE_EMPTYURL, "Prepare url is empty");
        return;
    }

    if (mPlayer.mPlayStatus != PLAYER_INITIALZED && mPlayer.mPlayStatus != PLAYER_STOPPED) {
        AF_LOGD("ProcessPrepareMsg status is %d", mPlayer.mPlayStatus.load());
        return;
    }
    mPlayer.ChangePlayerStatus(PLAYER_PREPARINIT);
    //   mPlayer.mPlayStatus = PLAYER_PREPARINIT;
    bool noFile = false;

    if (!(mPlayer.mBSReadCb != nullptr && mPlayer.mBSSeekCb != nullptr && mPlayer.mBSCbArg != nullptr)) {
        if (manifestMode) {
            // Object-based playback: segments are fetched through the playlist
            // pipeline which opens the data source lazily per segment URL.
            IDataSource::SourceConfig config{};
            config.low_speed_time_ms = mPlayer.mSet->timeout_ms;
            config.low_speed_limit = 1;
            config.connect_time_out_ms = mPlayer.mSet->timeout_ms;
            config.http_proxy = mPlayer.mSet->http_proxy;
            config.refer = mPlayer.mSet->refer;
            config.userAgent = mPlayer.mSet->userAgent;
            config.customHeaders = mPlayer.mSet->customHeaders;
            config.listener = mPlayer.mSourceListener.get();

            std::lock_guard<std::mutex> locker(mPlayer.mCreateMutex);
            mPlayer.mDataSource = dataSourcePrototype::create("https://", &(mPlayer.mSet->mOptions), DS_NEED_CACHE);
            if (mPlayer.mDataSource != nullptr) {
                mPlayer.mDataSource->Set_config(config);
                mPlayer.mDataSource->setUrlToUniqueIdCallback(mPlayer.mUrlHashCb, mPlayer.mUrlHashCbUserData);
            }
        } else if (!mPlayer.mSet->url.empty()) {
            ret = openUrl();

            if (ret < 0) {
                AF_LOGD("%s mDataSource open failed,url is %s %s", __FUNCTION__, mPlayer.mSet->url.c_str(), framework_err2_string(ret));

                if (ret == FRAMEWORK_ERR_EXIT) {
                    // stop by user.
                    //ChangePlayerStatus(PLAYER_STOPPED);
                    return;
                } else if (ret == FRAMEWORK_ERR_PROTOCOL_NOT_SUPPORT) {
                    noFile = true;
                } else {
                    mPlayer.NotifyError(ret);
                    return;
                }
            } else {
                if (mPlayer.mDataSource->getFlags() & IDataSource::flag_report_speed) {
                    mPlayer.mCalculateSpeedUsePacket = false;
                }
            }
        }
    }

    if (mPlayer.mCanceled) {
        return;
    }

    {
        std::lock_guard<std::mutex> locker(mPlayer.mCreateMutex);
        mPlayer.mDemuxerService = static_cast<unique_ptr<demuxer_service>>(new demuxer_service(mPlayer.mDataSource));
        mPlayer.mDemuxerService->setOptions(&mPlayer.mSet->mOptions);
    }

    std::function<void(std::string, std::string)> demuxerCB = [this](const std::string &key, const std::string &value) -> void {
        this->mPlayer.OnDemuxerCallback(key, value);
    };
    mPlayer.mDemuxerService->setDemuxerCb(demuxerCB);
    mPlayer.mDemuxerService->setNoFile(noFile);

    if (!noFile) {
        mPlayer.mDemuxerService->SetDataCallBack(mPlayer.mBSReadCb, mPlayer.mBSCbArg, mPlayer.mBSSeekCb, mPlayer.mBSCbArg, nullptr);
    }


    /* 【B19 硬要求】换片源/重新 Prepare 不允许跨片源补做 PFR。 */
    mPlayer.mPauseFrameRestorePending = false;
    /* 【B20 硬要求 B】Prepare（换片源）⇒ 在途被推迟的用户 seek 一并作废，不跨片源重放。 */
    mPlayer.mDeferredUserSeekPending = false;
    mPlayer.mDeferredUserSeekUs = INT64_MIN;
    mPlayer.mDeferredUserSeekAccurate = false;

    //prepare之前seek
    if (mPlayer.mSeekPos > 0) {
        mPlayer.mPNotifier->NotifySeeking(false);
        mPlayer.mDemuxerService->Seek(mPlayer.mSeekPos, 0, -1);
        mPlayer.mSeekFlag = true;
    } else {
        mPlayer.ResetSeekStatus();
    }

#ifdef ENABLE_VIDEO_FILTER
    if (mPlayer.mFilterManager) {
        mPlayer.mFilterManager->clearBuffer();
    }
#endif

    AF_LOGD("initOpen start");
    demuxer_type demuxerType = (mPlayer.mBSReadCb || noFile) ? demuxer_type_bit_stream : demuxer_type_unknown;

    if (manifestMode) {
        mPlayer.mDemuxerService->setManifestSource(std::move(mPlayer.mSet->manifest));
        demuxerType = demuxer_type_manifest;
    }

    ret = mPlayer.mDemuxerService->createDemuxer(demuxerType);

    // TODO: video tool box HW decoder not merge the header
    if (mPlayer.mDemuxerService->getDemuxerHandle()) {
#ifdef __APPLE__
        mPlayer.mDemuxerService->getDemuxerHandle()->setBitStreamFormat(header_type::header_type_extract, header_type::header_type_extract);
#else
        mPlayer.mDemuxerService->getDemuxerHandle()->setBitStreamFormat(header_type::header_type_merge, header_type::header_type_merge);
#endif
        mPlayer.mDemuxerService->getDemuxerHandle()->setUrlToUniqueIdCallback(mPlayer.mUrlHashCb, mPlayer.mUrlHashCbUserData);
        if (noFile) {
            IDataSource::SourceConfig config;
            mPlayer.mDataSource->Get_config(config);
            mPlayer.mDemuxerService->getDemuxerHandle()->setDataSourceConfig(config);
        }

        mPlayer.mDemuxerService->getDemuxerHandle()->SetOption("sessionId", mPlayer.mSet->sessionId);

        mPlayer.mDcaManager->createObservers();
        mPlayer.sendDCAMessage();
    }


    //step2: Demuxer init and getstream index
    ret = mPlayer.mDemuxerService->initOpen(demuxerType);

    if (ret < 0) {
        if (ret != FRAMEWORK_ERR_EXIT && !mPlayer.mCanceled) {
            mPlayer.NotifyError(ret);
        }

        return;
    }

    int nbStream = mPlayer.mDemuxerService->GetNbStreams();
    AF_LOGD("Demuxer service get nubmer streams is %d", nbStream);
    unique_ptr<streamMeta> pMeta;
    int bandWidthNearStreamIndex = -1;
    int minBandWidthDelta = INT_MAX;
    int mDefaultBandWidth = mPlayer.mSet->mDefaultBandWidth;
    int videoStreamCount = 0;

    /*
     * 视频档位快照，只用于下面那次"起播档同分辨率编码偏好"的收尾。
     * codecRank 取的是 decoderFactory::getEffectiveCodecEfficiencyRank()，与 ABR 侧
     * **同一份来源**：应用层传过 preference 就是应用层的位置序，否则是内核默认序
     * （AV1 5 / H.265 4 / VP9 3 / H.264 2 / MPEG-4、MPEG-2 1 / 未知 0）；
     * hwDecodeSupported 是"这台设备能不能硬解这一路"（同一个 decoderFactory 接口，
     * 应用层传过硬解编码集合时直接用应用层的集合，不再探测；未知时视为支持）。
     */
    struct StartUpVideoVariant {
        int index;
        int width;
        int height;
        int bandwidth;
        int codecRank;
        std::string codec;
        bool hwDecodeSupported;
    };
    std::vector<StartUpVideoVariant> videoVariants;

    for (int i = 0; i < nbStream; ++i) {
        mPlayer.mDemuxerService->GetStreamMeta(pMeta, i, false);
        auto *meta = (Stream_meta *) (pMeta.get());

        if (mPlayer.mDuration < 0) {
            mPlayer.mDuration = meta->duration;
        }

        if (meta->type == STREAM_TYPE_MIXED) {
            mPlayer.mMixMode = true;
        }

        if (meta->type == STREAM_TYPE_MIXED || meta->type == STREAM_TYPE_VIDEO) {
            videoStreamCount++;
            int metaBandWidth = (int) meta->bandwidth;

            if (abs(mDefaultBandWidth - metaBandWidth) < minBandWidthDelta) {
                bandWidthNearStreamIndex = i;
                minBandWidthDelta = abs(mDefaultBandWidth - metaBandWidth);
            }

            StartUpVideoVariant variant;
            variant.index = i;
            variant.width = meta->width;
            variant.height = meta->height;
            variant.bandwidth = metaBandWidth;
            variant.codec = afCodecShortName(meta->codec, nullptr);
            /*
             * 效率序与 ABR 侧**同一份来源、同一个函数**：应用层传过 preference 就用
             * 应用层的位置序，否则用内核默认序。这里不再自己挑顺序，也不把两套混用。
             */
            variant.codecRank = decoderFactory::getEffectiveCodecEfficiencyRank(variant.codec.c_str());
            /*
             * 硬解能力直接拿 Stream_meta.codec（这里就是 AFCodecID）去问，不用绕短名。
             * 应用层传过"设备能硬解的编码集合"时那个函数直接用应用层的集合（不探测）；
             * AF_CODEC_ID_NONE（清单里没有编码信息）会得到 true —— "编码未知 =
             * 能力未知 = 视为支持"，不会因为认不出编码就把这一路排到最后。
             */
            variant.hwDecodeSupported = decoderFactory::isHardwareDecodeSupported(meta->codec);
            videoVariants.push_back(variant);
        }
    }

    /*
     * 【起播档的同分辨率编码偏好：手动指定最高，硬解第一约束，压缩效率第二偏好】
     *
     * 上面那轮选起播档只看"离 mDefaultBandWidth 最近的码率"。当同一清晰度同时有
     * H.264 / H.265 / AV1 多路时（用户片单里的实际情况），它可能落到**白花带宽**、
     * 甚至**设备根本解不了**的那一路上。这里做一次收尾，规则与 ABR 侧**完全一致**
     * （AbrAlgoStrategy::FindSameResolutionEfficientCodec）：
     *
     *   0) 应用层手动指定了编码（preferred，见 decoderFactory::getEffectivePreferredCodec）
     *      且这一清晰度下有该编码的流、且内核解得了它 => **就用它**，并且**不再做下面
     *      的自动收尾** —— 手动压过硬解偏好与效率序（用户的目的就是手动更改编码格式，
     *      哪怕设备能硬解 H.265、哪怕 H.265 更省带宽，指定 H.264 就给 H.264），
     *      允许软解（日志里用 decodedBy=hw|sw 记录实际走的那条路）。
     *      这一清晰度下没有该编码的流、或内核根本解不了它 => 回退到下面的自动规则，
     *      并打一条说明回退原因的日志；
     *   1) 候选集 = 与已选档**宽高完全相同**的流（宽高不是都 > 0 就不做收尾）；
     *   2) 候选里只要存在能硬解的变体，就只在能硬解的变体里挑；一个能硬解的都没有
     *      才允许软解变体 —— 绝不选一条"硬解不了、又不比别的更优"的编码；
     *   3) 在筛出来的集合里按压缩效率序取最高者（AV1 > H.265 > VP9 > H.264 >
     *      MPEG-4/MPEG-2），同级取带宽最低者，仍相同则取下标小者（结果确定，
     *      不依赖遍历顺序）；
     *   4) 收益判据：候选要么能带来**硬解**（已选档解不了、它解得了，此时不要求
     *      更省带宽），要么**效率等级更高且带宽严格更低**；既不更能解、也不更省的
     *      一律不动（不会因为"另一路带宽更低"就换到压缩效率更差的编码上）。
     *
     * 绝不跨分辨率替换（不为了省带宽降低起播清晰度）。这里没有任何计时器/开关，
     * 只是把"选哪一路"的判据补全；读 preferred 的是**同一个函数、同一份快照**
     * （decoderFactory::getEffectivePreferredCodec），ABR 侧也读它，两处不各自判断。
     * 起播这一档是"下一次分片请求"的一种：设置 preferred 本身不切，Prepare 时
     * （也就是第一次选流时）才按它选。
     */
    if (bandWidthNearStreamIndex >= 0) {
        const StartUpVideoVariant *chosen = nullptr;

        for (size_t v = 0; v < videoVariants.size(); v++) {
            if (videoVariants[v].index == bandWidthNearStreamIndex) {
                chosen = &videoVariants[v];
                break;
            }
        }

        /* 手动指定是否已经把起播档定死（true = 不再走下面的自动收尾）。 */
        bool manualPreferredDecided = false;

        if (chosen != nullptr && chosen->width > 0 && chosen->height > 0) {
            /*
             * ---- 优先级 1：应用层手动指定的编码（preferred） ----
             *
             * 与 ABR 侧同一套判据、同一份快照，只做"起播这一档选哪一路"：
             *   · 内核根本解不了它（既不能硬解、也没有软解路径）=> 回退 + 日志；
             *   · 这一清晰度下没有该编码的流 => 回退 + 日志；
             *   · 有 => 选它（同一编码同一清晰度有多路时取带宽最低者，
             *     带宽相同取下标小者，结果与遍历顺序无关），并结束自动收尾。
             */
            const std::string preferred = decoderFactory::getEffectivePreferredCodec();

            if (!preferred.empty()) {
                if (!decoderFactory::isCodecDecodable(preferred.c_str())) {
                    AF_LOGW("startup video codec preference: preferred=%s (manual) cannot be decoded by the "
                            "kernel (neither hardware nor software path), falling back to the automatic rules\n",
                            preferred.c_str());
                } else {
                    const StartUpVideoVariant *manual = nullptr;

                    for (size_t v = 0; v < videoVariants.size(); v++) {
                        const StartUpVideoVariant &cand = videoVariants[v];

                        if (cand.width != chosen->width || cand.height != chosen->height) {
                            continue;
                        }

                        if (cand.codec != preferred) {
                            continue;
                        }

                        if (manual == nullptr || cand.bandwidth < manual->bandwidth ||
                            (cand.bandwidth == manual->bandwidth && cand.index < manual->index)) {
                            manual = &cand;
                        }
                    }

                    if (manual != nullptr) {
                        /*
                         * 手动指定已经把起播档定死：**不**再做下面的自动收尾 ——
                         * 否则自动规则会以"效率收益"为名把它换成 H.265/AV1，
                         * 用户的手动指定等于失效。
                         */
                        manualPreferredDecided = true;

                        if (manual->index != chosen->index) {
                            AF_LOGI("startup video stream %d -> %d: preferred=%s (manual) at %dx%d, "
                                    "decodedBy=%s, bandwidth %d -> %d (automatic rules bypassed)\n",
                                    chosen->index, manual->index, preferred.c_str(),
                                    chosen->width, chosen->height,
                                    manual->hwDecodeSupported ? "hw" : "sw",
                                    chosen->bandwidth, manual->bandwidth);
                            bandWidthNearStreamIndex = manual->index;
                        } else {
                            /* 已选档就是 preferred：不换，但也明确记一条，便于真机核对。 */
                            AF_LOGI("startup video stream %d: preferred=%s (manual) is already the chosen "
                                    "stream at %dx%d, decodedBy=%s (automatic rules bypassed)\n",
                                    chosen->index, preferred.c_str(), chosen->width, chosen->height,
                                    manual->hwDecodeSupported ? "hw" : "sw");
                        }
                    } else {
                        AF_LOGW("startup video codec preference: preferred=%s (manual) but there is no such "
                                "stream at %dx%d, falling back to the automatic rules "
                                "(hardware decode first, then efficiency)\n",
                                preferred.c_str(), chosen->width, chosen->height);
                    }
                }
            }
        }

        if (!manualPreferredDecided && chosen != nullptr && chosen->width > 0 && chosen->height > 0) {
            /*
             * 这一清晰度上有没有能硬解的变体（chosen 自己也在候选集里，所以它解不了
             * 时这里只要为 true，下面就一定能找到同一清晰度上能硬解的那一路）。
             * 硬解能力是第一约束：有能硬解的变体时，候选集收缩到能硬解的那些。
             */
            bool hasHardwareVariant = false;

            for (size_t v = 0; v < videoVariants.size(); v++) {
                const StartUpVideoVariant &cand = videoVariants[v];

                if (cand.width == chosen->width && cand.height == chosen->height && cand.hwDecodeSupported) {
                    hasHardwareVariant = true;
                    break;
                }
            }

            /*
             * 排序键依次是：编码效率等级（高者胜）-> 带宽（低者胜）-> 下标（小者胜）。
             * 最后一个键让结果与遍历顺序无关。
             */
            const StartUpVideoVariant *best = nullptr;

            for (size_t v = 0; v < videoVariants.size(); v++) {
                const StartUpVideoVariant &cand = videoVariants[v];

                if (cand.width != chosen->width || cand.height != chosen->height) {
                    continue;
                }

                /* 硬解优先：有能硬解的变体时，软解变体直接出局。 */
                if (hasHardwareVariant && !cand.hwDecodeSupported) {
                    continue;
                }

                if (best == nullptr) {
                    best = &cand;
                    continue;
                }

                if (cand.codecRank > best->codecRank) {
                    best = &cand;
                } else if (cand.codecRank == best->codecRank) {
                    if (cand.bandwidth < best->bandwidth ||
                        (cand.bandwidth == best->bandwidth && cand.index < best->index)) {
                        best = &cand;
                    }
                }
            }

            if (best != nullptr && best->index != chosen->index) {
                /*
                 * 收益判据：候选要么能带来**硬解**（已选档解不了、它解得了 —— 这时
                 * 不要求更省带宽），要么**效率等级更高且带宽严格更低**。既不更能解、
                 * 也不更省的一律不动 —— 特别地，不会因为"另一路带宽更低"就换到压缩
                 * 效率更差的编码上（那是降画质，不是省带宽）。
                 */
                const bool hardwareGain = best->hwDecodeSupported && !chosen->hwDecodeSupported;
                const bool efficiencyGain = (best->codecRank > chosen->codecRank) &&
                                            (best->bandwidth < chosen->bandwidth);

                if (hardwareGain || efficiencyGain) {
                    AF_LOGI("startup video stream %d -> %d: same %dx%d resolution, codec %s -> %s, "
                            "hw %d -> %d, bandwidth %d -> %d (%s)\n",
                            chosen->index, best->index, chosen->width, chosen->height,
                            chosen->codec.empty() ? "(unknown)" : chosen->codec.c_str(),
                            best->codec.empty() ? "(unknown)" : best->codec.c_str(),
                            (int) chosen->hwDecodeSupported, (int) best->hwDecodeSupported,
                            chosen->bandwidth, best->bandwidth,
                            hardwareGain ? "hardware decode" : "more efficient codec, less bandwidth");
                    bandWidthNearStreamIndex = best->index;
                }
            }
        }
    }

    if (mPlayer.mDuration == 0) {
        //live should not be cached
        if (mPlayer.mDataSource) {
            mPlayer.mDataSource->enableCache(mPlayer.mSet->url, false);
        }
    } else {
        if (mPlayer.mDemuxerService->isPlayList() && videoStreamCount > 1) {
            if (mPlayer.mDataSource) {
                mPlayer.mDataSource->enableCache(mPlayer.mSet->url, false);
            }
        } else {
            if (mPlayer.mDataSource) {
                mPlayer.mDataSource->enableCache(mPlayer.mSet->url, true);
            }
        }
    }

    for (int i = 0; i < nbStream; ++i) {
        int openStreamRet = 0;
        mPlayer.mDemuxerService->GetStreamMeta(pMeta, i, false);
        auto *meta = (Stream_meta *) (pMeta.get());

        mPlayer.mSuggestedPresentationDelay = meta->suggestedPresentationDelay;
        AF_LOGD("mSuggestedPresentationDelay %lld\n", meta->suggestedPresentationDelay);

        auto *info = new StreamInfo();
        info->streamIndex = i;
        info->subtitleLang = nullptr;
        info->audioLang = nullptr;
        info->description = nullptr;
        info->bitrate = meta->bitrate;

        AF_LOGD("get a stream %d\n", meta->type);

        if (!mPlayer.mSet->bDisableVideo && meta->type == STREAM_TYPE_VIDEO) {
            info->type = ST_TYPE_VIDEO;
            info->videoWidth = meta->width;
            info->videoHeight = meta->height;
            info->videoBandwidth = (int) meta->bandwidth;
            /*
             * 编码短名：清单/容器给出的编码经**同一个**归一化函数转成应用层可显示的
             * 短名（"H.264"/"H.265"/"AV1"/"VP9"/"MPEG-4"/"MPEG-2"）。拿不到编码
             * 信息时是空串，界面据此不显示徽标 —— 这里不许猜。
             */
            snprintf(info->videoCodec, sizeof(info->videoCodec), "%s",
                     afCodecShortName(meta->codec, nullptr));
            info->HDRType = VideoHDRType_SDR;
            if (meta->pixel_fmt == AF_PIX_FMT_YUV420P10BE || meta->pixel_fmt == AF_PIX_FMT_YUV420P10LE) {
                info->HDRType = VideoHDRType_HDR10;
            }

            if (meta->description) {
                info->description = strdup((const char *) meta->description);
            }

            mPlayer.mMediaInfo.mStreamInfoQueue.push_back(info);
            mPlayer.mVideoInterlaced = meta->interlaced;

            if (mPlayer.mCurrentVideoIndex < 0 && !mPlayer.mMixMode && meta->attached_pic == 0) {
                if (bandWidthNearStreamIndex == i) {
                    AF_LOGD("get a video stream\n");
                    openStreamRet = mPlayer.mDemuxerService->OpenStream(i);
                    mPlayer.mCurrentVideoIndex = i;
                    mPlayer.updateVideoMeta();
                }
            }
        } else if (!mPlayer.mSet->bDisableAudio && meta->type == STREAM_TYPE_AUDIO) {
            info->type = ST_TYPE_AUDIO;

            if (meta->lang) {
                info->audioLang = strdup((const char *) meta->lang);
            }

            if (meta->description) {
                info->description = strdup((const char *) meta->description);
            }

            info->nChannels = meta->channels;
            info->sampleFormat = meta->sample_fmt;
            info->sampleRate = meta->samplerate;
            mPlayer.mMediaInfo.mStreamInfoQueue.push_back(info);

            if (mPlayer.mCurrentAudioIndex < 0 && !mPlayer.mMixMode) {
                AF_LOGD("get a audio stream\n");
                openStreamRet = mPlayer.mDemuxerService->OpenStream(i);
                mPlayer.mCurrentAudioIndex = i;
                mPlayer.mCATimeBase = meta->ptsTimeBase;
            }
        } else if (meta->type == STREAM_TYPE_SUB) {
            info->type = ST_TYPE_SUB;

            if (meta->lang) {
                info->subtitleLang = strdup((const char *) meta->lang);
            }

            if (meta->description) {
                info->description = strdup((const char *) meta->description);
            }

            mPlayer.mMediaInfo.mStreamInfoQueue.push_back(info);

            if (mPlayer.mCurrentSubtitleIndex < 0 &&
                /*
                 * The codec of the subtitle stream can't be detected in HLS master play list
                 */
                (meta->codec != AF_CODEC_ID_NONE || mPlayer.mDemuxerService->isPlayList())) {
                AF_LOGD("get a subtitle stream\n");
                openStreamRet = mPlayer.mDemuxerService->OpenStream(i);
                mPlayer.mCurrentSubtitleIndex = i;
                if (meta->extradata && meta->extradata_size > 0) {
                    meta->extradata[meta->extradata_size] = 0;
                    mPlayer.mPNotifier->NotifySubtitleHeader(mPlayer.mCurrentSubtitleIndex, (const char *) meta->extradata);
                }
            }
        } else if (meta->type == STREAM_TYPE_MIXED) {
            info->type = ST_TYPE_VIDEO;
            info->streamIndex = i;
            info->videoBandwidth = (int) meta->bandwidth;
            info->videoWidth = meta->width;
            info->videoHeight = meta->height;
            /* 与上面的视频分支同一份归一化实现（muxed 流的编码同样来自
             * 清单/容器，拿不到就是空串）。 */
            snprintf(info->videoCodec, sizeof(info->videoCodec), "%s",
                     afCodecShortName(meta->codec, nullptr));
            AF_LOGD("STREAM_TYPE_MIXED bandwidth is %llu", meta->bandwidth);

            if (mPlayer.mMainStreamId >= 0) {
                AF_LOGD("already readed stream");
            } else if (bandWidthNearStreamIndex == i) {
                mPlayer.mMixMode = true;
                openStreamRet = mPlayer.mDemuxerService->OpenStream(i);
                mPlayer.mMainStreamId = i;
            }

            mPlayer.mMediaInfo.mStreamInfoQueue.push_back(info);
        } else {
            delete info;
        }

        if (openStreamRet < 0) {
            mPlayer.ChangePlayerStatus(PLAYER_ERROR);
            mPlayer.mPNotifier->NotifyError(MEDIA_PLAYER_ERROR_DEMUXER_OPENSTREAM, "open stream failed");
            return;
        }
    }

    Media_meta pMediaMeta{};
    mPlayer.mDemuxerService->GetMediaMeta(&pMediaMeta);
    mPlayer.mMediaInfo.totalBitrate = pMediaMeta.totalBitrate;

    buildContainerInfo();

    // TODO: why ?
    /*
    if (!HAVE_VIDEO) {
        mPlayer.mSeekNeedCatch = false;
    }
*/

    AF_LOGD("initOpen end");
    mPlayer.mDemuxerService->start();
    mPlayer.ChangePlayerStatus(PLAYER_PREPARING);
    mPlayer.mTimeoutStartTime = INT64_MIN;
}

void SMPMessageControllerListener::buildContainerInfo()
{

    CicadaJSONItem containerInfo{};

    URLComponents urlComponents{};
    UrlUtils::parseUrl(urlComponents, mPlayer.mSet->url);

    std::string finalProto = urlComponents.proto;
    if (urlComponents.proto.empty()) {
        if (FileUtils::isFileExist(mPlayer.mSet->url.c_str())) {
            finalProto = "file";
        } else {
            finalProto = "N/A";
        }
    }
    containerInfo.addValue("protocol", finalProto);

    int videoStreamCount = 0;
    for (StreamInfo *item : mPlayer.mMediaInfo.mStreamInfoQueue) {
        if (item->type == StreamType::ST_TYPE_VIDEO) {
            videoStreamCount++;
        }
    }
    containerInfo.addValue("isMultiBitrate", videoStreamCount > 1 ? "1" : "0");

    IDemuxer *demuxer = mPlayer.mDemuxerService->getDemuxerHandle();
    std::string containerName = demuxer->GetProperty(-1, "containerName");
    containerInfo.addValue("containerName", containerName);

    mPlayer.mContainerInfo = containerInfo.printJSON();
}

void SMPMessageControllerListener::ProcessStartMsg()
{
    if (mPlayer.mPlayStatus == PLAYER_PAUSED || mPlayer.mPlayStatus == PLAYER_PREPARED || mPlayer.mPlayStatus == PLAYER_COMPLETION) {
        mPlayer.mUtil->reset();

        if (mPlayer.mPlayStatus != PLAYER_PAUSED) {
            if (HAVE_AUDIO) {
                mPlayer.mMasterClock.setTime(mPlayer.mFirstAudioPts);
            } else {
                mPlayer.mMasterClock.setTime(mPlayer.mFirstVideoPts);
            }
        }

        mPlayer.ChangePlayerStatus(PLAYER_PLAYING);

        // 恢复播放：解除暂停帧恢复的渲染门（恢复正常渲染）
        IDecoder *decoder = mPlayer.mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);
        if (decoder != nullptr) {
            decoder->setRenderGate(INT64_MIN);
        }
    }
}

void SMPMessageControllerListener::ProcessPauseMsg()
{
    if (mPlayer.mPlayStatus != PLAYER_PLAYING) {
        return;
    }

    mPlayer.ChangePlayerStatus(PLAYER_PAUSED);
    mPlayer.startRendering(false);
}

// TODO: set layout when init videoRender?
void SMPMessageControllerListener::ProcessSetDisplayMode()
{
    if (mPlayer.mAVDeviceManager->isVideoRenderValid()) {
        mPlayer.mAVDeviceManager->getVideoRender()->setScale(SuperMediaPlayer::convertScaleMode(mPlayer.mSet->scaleMode));
    }
}

void SMPMessageControllerListener::ProcessSetRotationMode()
{
    if (mPlayer.mAVDeviceManager->isVideoRenderValid()) {
        mPlayer.mAVDeviceManager->getVideoRender()->setRotate(SuperMediaPlayer::convertRotateMode(mPlayer.mSet->rotateMode));
    }
}

void SMPMessageControllerListener::ProcessSetMirrorMode()
{
    if (mPlayer.mAVDeviceManager->isVideoRenderValid()) {
        mPlayer.mAVDeviceManager->getVideoRender()->setFlip(SuperMediaPlayer::convertMirrorMode(mPlayer.mSet->mirrorMode));
    }
}

void SMPMessageControllerListener::ProcessSetVideoBackgroundColor()
{
    if (mPlayer.mAVDeviceManager->isVideoRenderValid()) {
        mPlayer.mAVDeviceManager->getVideoRender()->setBackgroundColor(mPlayer.mSet->mVideoBackgroundColor);
    }
}

/*
 * 【色觉辅助滤镜 / 回退点 C6】照 ProcessSetMirrorMode 的写法：对**当前**渲染器下发。
 * 矩阵本体直接读 mSet->colorMatrix（与 mirrorMode 同一套写法），消息只作触发信号。
 * 空默认实现：非 GL 渲染器（Dummy / Qt / Apple）什么都不做，且**不需要改一行**。
 * 这里是"事件驱动"的那一行日志，方便真机确认通道打通（不是计时器）。
 */
void SMPMessageControllerListener::ProcessSetColorMatrix()
{
    const float *matrix = mPlayer.mSet->colorMatrix;
    AF_LOGI("color matrix applied: %f %f %f %f %f %f %f %f %f\n",
            matrix[0], matrix[1], matrix[2], matrix[3], matrix[4],
            matrix[5], matrix[6], matrix[7], matrix[8]);

    if (mPlayer.mAVDeviceManager->isVideoRenderValid()) {
        mPlayer.mAVDeviceManager->getVideoRender()->setColorMatrix(matrix);
    }
}

void SMPMessageControllerListener::ProcessSetViewMsg(void *view)
{
    mPlayer.mSet->mView = view;
    std::unique_lock<std::mutex> uMutex(mPlayer.mCreateMutex);

    if (mPlayer.mAVDeviceManager->getVideoRender() != nullptr) {
        mPlayer.mAVDeviceManager->getVideoRender()->setDisPlay(view);
    }

    // 隧道直通（dummy render）模式：surface 变化（含销毁，view==null）时
    // 调用 MediaCodec.setOutputSurface —— 对齐 ExoPlayer 2.9.6：
    // null 时 Java 侧换成内部 DummySurface（codec 保持运行，帧静默丢弃），
    // 回前台切回真实 surface；只有 codec 已失效（返回负值）才走
    // releaseCodec+maybeInitCodec 式的重建兜底（RestartVideoDecoder）
    if (mPlayer.mAVDeviceManager->getVideoRender() != nullptr
        && (mPlayer.mAVDeviceManager->getVideoRender()->getFlags() & IVideoRender::FLAG_DUMMY)) {
        /*
         * 【2026-09-24 修：切档在途时，这里不许换 surface、更不许重建解码器】
         *
         * 真机 native 崩溃（Fatal signal 11 SIGSEGV, fault addr 0x4,
         * tid=ApsaraPlayerService）的成因：A 方案下 SurfaceView 尺寸变化会再次
         * setSurface ⇒ 本函数在主线程走 setOutputSurface，失败后
         * RestartVideoDecoder()（invalidateDecoder + CreateVideoDecoder），而**播放线程
         * 正在为切档重建解码器** —— 两边同时动解码器槽位/mAVDeviceManager，
         * 且都落到 tunnel 下无意义的软解兜底，最终崩在 native。
         *
         * 判据是**既有状态**（不新增成员、不看时间、没有定时器）：切档在途
         * ⇒ 播放线程正在接管视频槽位，这里必须让路，只记一条日志；
         * 切档自己用既有的 finishQualitySwitch / 错误路径收尾。
         *
         * 【为什么 Qt 不受影响】整条判据包在 FLAG_DUMMY 分支内，而 Qt 永远没有
         * FLAG_DUMMY 渲染器（bEnableTunnelRender 每次播放前 reset 为 false、
         * Qt 壳不下发该选项、CicadaVideoRender 明确返回 0 而不是 FLAG_DUMMY）
         * ⇒ 恒不进入，Qt 走的还是原来那条 setOutputSurface/RestartVideoDecoder 分支。
         */
        const bool qualitySwitchInFlight = mPlayer.mVideoSwitchInFlight;
        if (qualitySwitchInFlight) {
            AF_LOGW("ProcessSetViewMsg: quality switch in flight — skipping the dummy surface swap and "
                    "the decoder rebuild; the switch owns the video slot\n");
        } else {
            IDecoder *decoder = mPlayer.mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);
            if (decoder != nullptr) {
                int ret = decoder->setOutputSurface(view);
                if (ret < 0) {
                    AF_LOGI("video output surface swap failed ret=%d, fallback: restart video decoder\n", ret);
                    mPlayer.RestartVideoDecoder();
                } else if (view != nullptr && mPlayer.mPlayStatus == PLAYER_PAUSED) {
                    // 暂停状态恢复 surface：ACodec 不会重绘最后一帧，用渲染门 +
                    // 原地 seek 精确恢复"暂停的那一帧"（只放行最后渲染帧 PTS）
                    mPlayer.RestorePausedVideoFrame();
                }
            }
        }
    }
}

void SMPMessageControllerListener::ProcessSetDataSourceMsg(const std::string &url)
{
    if (mPlayer.mPlayStatus == PLAYER_IDLE || mPlayer.mPlayStatus == PLAYER_STOPPED) {
        mPlayer.mSet->url = url;
        mPlayer.mSet->manifest.reset();
        mPlayer.ChangePlayerStatus(PLAYER_INITIALZED);
    }
}

void SMPMessageControllerListener::ProcessSetManifestDataSourceMsg(std::unique_ptr<Manifest::MediaManifest> manifest)
{
    if (mPlayer.mPlayStatus == PLAYER_IDLE || mPlayer.mPlayStatus == PLAYER_STOPPED) {
        mPlayer.mSet->manifest = std::move(manifest);
        mPlayer.mSet->url = "";
        mPlayer.ChangePlayerStatus(PLAYER_INITIALZED);
    }
}

void SMPMessageControllerListener::ProcessSetBitStreamMsg(readCB read, seekCB seek, void *arg)
{
    if (mPlayer.mPlayStatus == PLAYER_IDLE || mPlayer.mPlayStatus == PLAYER_STOPPED) {
        mPlayer.mBSReadCb = read;
        mPlayer.mBSSeekCb = seek;
        mPlayer.mBSCbArg = arg;

        if (read != nullptr) {
            mPlayer.ChangePlayerStatus(PLAYER_INITIALZED);
        }
    }
}

void SMPMessageControllerListener::ProcessSeekToMsg(int64_t seekPos, bool bAccurate)
{
    /*
     * 【③ B20：切档在途时**不执行**这次 seek，推迟到切档终态再重放】
     *
     * 为什么必须在最前面拦：本函数后面会（a）把内容时间轴（mMasterClock）钉到目标、
     * （b）开启新的不连续点（落点过滤 + 代际）、（c）用 `FlushVideoPath(true, true, __func__)`
     * 把在途切档拆掉 —— 一旦走到那里，切档就已经被判 CANCELED 了，之后再"补"也来不及。
     *
     * 为什么两套 seek 会互相覆盖（本条的动机）：切档自己的定位是 demuxer 级 seek，目标是
     * "当前播放位置"；用户 seek 也重定位同一条读链路，谁后执行谁生效。日志实证：
     * 用户 seek 到 135164000 us，随后落点窗口里出现的帧却在 169886383 us，而紧接着切档
     * `target stream 0 seeked to 169886383` —— 差 34722 ms。串行化之后，用户 seek 一定在
     * 切档终态之后、在新流的同一时间轴上执行，落点判据不会再对不上。
     *
     * 不丢用户意图（硬要求 C）：
     *   · 多次点/多次 seek ⇒ 后面的覆盖前面的（只保留最后一次目标）；
     *   · 推迟期间通知一次 NotifySeeking(true)，让界面进入"定位中"；
     *   · 补做走的是正常的 SeekTo 流程 ⇒ 由既有的 SeekEnd / NotifySeeking(false) 收尾。
     * 判据直接复用 SuperMediaPlayer::qualitySwitchInFlight()（B19-b 的唯一判据）；
     * 切档自己的 demuxer 级 seek 不进本函数（全仓 putMsg(MSG_SEEKTO) 只有 SeekTo() 一处），
     * 所以不会自锁、也不会 ping-pong。
     */
    /*
     * 【修：切档在途时推迟这次 seek，等切档终态再重放一次】
     *
     * 真机日志（2026-09-27 10:18，在线视频）把"推迟"的代价写得很清楚：
     *   user seek: DEFERRED … target=85791000            （10:18:10.081）
     *   user seek: replaying the DEFERRED seek …         （10:18:23.372）
     * —— 用户的 seek 干等了十几秒，界面一直在"定位中"，用户描述就是"seek 后卡死"。
     *
     * 单解码器立即切换之后只有一种在途形态（请求生效 → 落点帧上屏的终态），
     * 所以判据就是"有没有切档在途"；推迟的那次 seek 由切档终态（finishQualitySwitch
     * 出口里的 replayDeferredUserSeek）**只补做一次**，后到的请求覆盖先到的，用户意图不丢。
     */
    if (mPlayer.mVideoSwitchInFlight) {
        mPlayer.mDeferredUserSeekUs = seekPos;
        mPlayer.mDeferredUserSeekAccurate = bAccurate;
        mPlayer.mDeferredUserSeekPending = true;

        mPlayer.mPNotifier->NotifySeeking(true);

        AF_LOGW("user seek: DEFERRED (NOT executed now) — a quality switch is in flight; target=%lld us "
                "accurate=%d. It will be replayed ONCE at the switch's terminal state (READY / FAILED); "
                "a newer seek request overwrites this one.\n",
                (long long) seekPos, (int) bAccurate);
        return;
    }

    mPlayer.mSeekNeedCatch = bAccurate;
    mPlayer.mSeekPos = seekPos;

    /*
     * 立刻把主时钟钉到 seek 目标上（而不是等下面 demuxer->Seek() 返回后再钉，
     * 函数末尾还有一次 setTime(seekPos)）。
     *
     * 为什么要提前：DASH/HLS 的 demuxer->Seek() 要等新 segment 下载完（实测 1s+），
     * 而 seek 开始时第 622 行的 ClearPacket(BUFFER_TYPE_ALL) 已经把缓存清空，
     * 主循环于是先进入缓冲态并 pause() 主时钟 —— 此时时钟还停在 **seek 之前**的位置。
     * 新 segment 一到，解码器立刻解出目标帧并上屏，第 3039 行 beginRendererJoining()
     * 用的是这个旧时钟（日志实测 master=14.85s，目标是 12.33s），追赶窗口就把
     * 12.33s~14.85s 的目标帧全部当成"迟到帧"丢掉；随后时钟又被拉回 12.33s，
     * 于是丢掉的区间变成了再也拿不回来的未来 —— 画面静止约 2.5 秒，这就是
     * "seek 之后卡一下"的根因。提前钉住时钟后，无论 seek 的哪个线程先跑，
     * 时间轴都和目标一致，追赶窗口丢掉的只有真正的 seek 之前帧。
     */
    mPlayer.mMasterClock.setTime(seekPos);

    /*
     * ============ 【P1-b：所有 seek 入口都在这里开启不连续点】============
     *
     * 实测（用户日志）：
     *   seek anchor: master clock anchored to the first frame rendered after the seek
     *                (pts=83438, target=2844000, landingFloor=-9223372036854775808)
     * 目标 2.844s 却锚到 0.083s，而且那一次 seek 的"目标载体"是空的。
     *
     * 根因：目标点原来只有一个来源 —— SuperMediaPlayer::SeekTo()（API 线程）。而 seek
     * 不止那一条入口，至少还有两条**绕过 SeekTo()** 直接派发 MSG_SEEKTO / 直接调用本函数的路径：
     *   · SuperMediaPlayer::playCompleted()（循环播放重开：`mSeekPos = 0` 之后直接调用
     *     本函数）；
     *   · SMPMessageControllerListener::ProcessPrepareMsg() 的"prepare 之前 seek"分支。
     * 那些 seek 里目标载体恒为空 ⇒ 落点判据拿不到目标、锚点判据也没有目标可比。
     *
     * 现在统一改成**开启一个新的不连续点**（本函数是"seek 真正开始、mSeekFlag 置真"的
     * 唯一收口处）：它一次性完成原来那两行补丁的全部职责，而且用的是同一份架构机制 ——
     *   · generation +1 ⇒ 所有旧代际的包/帧/待处理事件当场作废；
     *   · targetUs = seekPos ⇒ 唯一的"用户目标点"载体（落点过滤与位置上报共用）；
     *   · filterActive = true ⇒ 落点过滤开始生效（只由"包含目标的帧上屏"或
     *     下一次 seek/Reset 结束，SeekEnd 关不掉它）；
     *   · 内容时间轴（mMasterClock）在下面 setTime(seekPos) 钉在目标点 ⇒ 位置上报与渲染
     *     节拍共用同一根轴，等落点帧上屏时再被 acceptDiscontinuityLandingFrame() 钉一次。
     *
     * 与 SeekTo() 里那次调用是同一语义的重复保险（SeekTo 走的消息最终也到这里），
     * 值完全一致；代际多推一格没有副作用（它只是归属判据）。
     * 目标为 0（循环重开）时 filterActive 同样为真，行为与旧地板对 0 的语义一致。
     */
    /*
     * 【音频落点地板】这是"seek 真正开始"的收口处，所以在这里再声明一次：
     * 本次 seek 会（在下面的 FlushAudioPath 里）把音频时间轴重新锚到 seekPos，
     * 于是 beginDiscontinuity() 会臂上音频自己的落点地板 —— 落在目标点之前的音频帧
     * 一律不上设备（理由见 Discontinuity::audioLandingPending 的说明）。
     * SeekTo() 那次 beginDiscontinuity() 已经臂过一次，这里用同一个 seekPos 重算，
     * 结果相同；显式再声明是为了覆盖"绕过 SeekTo、直接派发 MSG_SEEKTO"的两条入口
     * （playCompleted() 的循环重开、ProcessPrepareMsg() 的 prepare 前 seek）。
     */
    mPlayer.mSeekAudioLandingReset = true;
    mPlayer.beginDiscontinuity(seekPos);

    // 暂停帧恢复的渲染门只在"恢复专用 seek"期间保持；用户自己发起的
    // seek（如暂停时拖动进度条）要关闭渲染门，恢复正常渲染
    if (!mPlayer.mRestoringPausedFrame) {
        IDecoder *decoder = mPlayer.mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);
        if (decoder != nullptr) {
            decoder->setRenderGate(INT64_MIN);
        }
    }
    mPlayer.mRestoringPausedFrame = false;

    // seek before prepare, should keep mSeekPos
    // 注意这里**不包含 PLAYER_STOPPED**：停住/出错的状态下这个 seek 是不会执行的，
    // 那种情况必须把 seek 状态清掉（交给下面那个分支的 ResetSeekStatus）。
    // 原来 STOPPED 写在这个条件里，于是永远走不到下面、mSeekPos 和 mSeekNeedCatch
    // 就一直留着 —— isSeeking() 永远为真，位置读出来是那个没生效的目标值，
    // 而且之后所有"早于目标位置"的包都会被丢掉：重新播放时画面出不来（卡死）。
    if (mPlayer.mPlayStatus < PLAYER_PREPARING) {
        return;
    }

    //can seek when finished
    if ((0 >= mPlayer.mDuration) || (mPlayer.mPlayStatus >= PLAYER_STOPPED && mPlayer.mPlayStatus != PLAYER_COMPLETION)) {
        mPlayer.ResetSeekStatus();
        return;
    }
    //checkPosInPackQueue cache in seek
    //TODO: seek sync
    mPlayer.mSeekFlag = true;
    /*
     * 【追帧加速】seek 真正开始（mSeekFlag 置真的同一处）⇒ 让视频解码器跑更高的性能点。
     *
     * seek 落点必须从**目标之前的关键帧**起解，这段前缀帧现在会全部上屏（快进到目标），
     * 所以这段解码时间直接等于用户看到的"追上去要多久"。这里只是把本就要解的内容更快
     * 吐出来：不丢帧、不改时间轴、不动落点判据。
     * 收回点成对且都是状态判据：落点帧被采纳时、以及 ResetSeekStatus()（seek 结束）。
     */
    mPlayer.setVideoDecodeBoost(true);
    mPlayer.mPlayedVideoPts = INT64_MIN;
    mPlayer.mPlayedAudioPts = INT64_MIN;
    mPlayer.mSoughtVideoPos = INT64_MIN;
    mPlayer.mCurVideoPts = INT64_MIN;
    /* 用户 seek 开始时丢弃上一次清晰度切换留下的时间轴偏移；
     * seek 目标是新的 A/V 同步锚点，不能继续把 rendition 偏移应用到新帧。 */
    mPlayer.mActiveVideoPtsOffset = INT64_MIN;
    mPlayer.mVideoPtsRevert = false;
    mPlayer.mAudioPtsRevert = false;
    /*
     * 【P2】这里原来置一个"seek 之后第一帧上屏"的锚点事件闩（供 doRender 在 seek 后第一帧
     * 上屏时把主时钟锚到那一帧）。锚点闩群已随 P2 的音频基准重设整体删除：
     *   · 有音频时主时钟的参考是"目标点 + 设备已消费量"（Discontinuity::audioBase*），
     *     它在不连续点上就等于目标点并只随设备消费单调前进；
     *   · 无音频时主时钟由本函数上面的 setTime(seekPos) 与落点采纳各自钉住。
     * 也就是说"seek 真正开始"这件事现在只需要 beginDiscontinuity()（本函数上面那处），
     * 不需要再置任何"事后要把时钟拉回来"的事件闩。
     */
    /* 必须在调用 DASH/HLS demuxer->Seek() 之前拆掉在途切档（关掉它的目标流）。
     * 否则网络慢时 Seek 会等待已经被用户放弃的目标流，表现为 seek 直接卡死。
     * 没有在途切档时保持原有 SeekInCache 行为，不额外 flush。 */
    if (mPlayer.mVideoSwitchInFlight) {
        /*
         * flushRender 从 true 改成 false：这里只需要"拆掉在途切档"（cancelPendingSwitch=1），
         * 不需要连渲染器一起 flush。flushRender=true 会把渲染器里的最后一帧也清掉 ⇒ 画面
         * 黑一下再出画（而本函数后面 :1000 一带的 seek 自身 flush 早就改成 flushRender=false
         * 以避免碰渲染器）。改成 false 之后：画面停在旧帧上，凑齐落点帧再替换。
         */
        mPlayer.FlushVideoPath(false, true, __func__);

        mPlayer.mVideoChangedFirstPts = INT64_MIN;
    }

    /*
     * ============ 矩阵第 3 格：seek 在途时必须**干净中止**在途切档（2026-09-24）============
     *
     * 上面那个 if 只做"拆掉视频路"（CancelPendingSwitch）。这里保证任何在途切档都走
     * **唯一终态出口** —— 复用现成的 finishQualitySwitch(false, …)：它会关掉目标流、
     * 释放退役解码器、发 FAILED 给 UI（高亮退回旧档）并清干净在途状态。否则状态机
     * 会停在"在途但永远不会上屏"，用户看到的就是画面卡住。
     *
     * 为什么用 finishQualitySwitch 而不是 FlushVideoPath：这里如果再来一次
     * FlushVideoPath(flushRender=1) 会把渲染器一起 flush 掉，正是历史上"seek 后
     * read-ahead gate 永久堵住"那条事故路径。所以只做状态收尾，终态通知由
     * finishQualitySwitch 统一发给 UI。
     */
    if (mPlayer.mVideoSwitchInFlight) {
        AF_LOGW("seek is taking over while a quality switch was in flight — finishing the switch cleanly "
                "before repositioning, otherwise the two in-flight state machines would wait for each other\n");

        mPlayer.finishQualitySwitch(false, "superseded by a seek that took over the video timeline");
    }
    //flush packet queue
    mPlayer.mSeekInCache = mPlayer.SeekInCache(seekPos);
    /* DASH/HLS 的 segment reader 有独立的当前 segment 游标，单纯在
     * BufferController 中回退 packet 并不能把 demuxer 游标回退；日志里
     * seek 到 18.7s 后 demuxer 仍从 60s segment 读取，随后视频追帧进入
     * FPS=0/1 死循环。对 playlist/manifest 强制走 demuxer seek，只有本地
     * 文件等可安全回放公共缓存的路径才使用 SeekInCache。 */
    if (mPlayer.mDemuxerService->isPlayList()) {
        mPlayer.mSeekInCache = false;
    }

    /*
     * ============ 【A：seek 一律"flush + 重读"，取消"命中缓存"捷径】============
     *
     * 这是主流播放器的做法（ExoPlayer 每次 seek 都 flush 掉 loader/decoder 缓冲，从目标
     * 分片重读），也是"分片边界切换"能成立的前提。
     *
     * 为什么必须取消这条捷径（真机 21:42 那份日志）：
     *   `sought in cache` 分支**一个视频包都不清**（只处理音频对齐），于是主时钟被本次
     *   seek 钉在目标点（本函数末尾 setTime(seekPos)），而解码器仍从**队首**重新起步：
     *     [switch] … master=214890741 activeFrontPts=95545450 activeQ=6978
     *     drop frame,master played time is 234128251, video pts is 118668550
     *   视频活动路落后音频 **115 秒**、队列积压 ≈233 秒 4K 内容，解码器只能一包一包啃
     *   （~1.2× 实时）⇒ 画面靠防冻阀门每 8 帧放 1 帧，切档也永远追不上时间线。
     *
     * 强制走"清包 + demuxer seek"这支之后：目标流从目标分片重新读，配合下面 seek 收尾处
     * 的"裁掉目标点之前的陈旧视频包"，两条轴在每次 seek 后都从同一个落点重新对齐。
     * 代价只有一个 —— seek 后必然重新缓冲（分片流本来就如此），这正是主流的取舍。
     */
    mPlayer.mSeekInCache = false;
    AF_LOGI("PFR: seek posUs=%" PRId64 " inCache=%d status=%d\n",
            seekPos, (int) mPlayer.mSeekInCache, (int) mPlayer.mPlayStatus.load());

    mPlayer.mPNotifier->NotifySeeking(mPlayer.mSeekInCache);

    /*
     * 【本轮 B15：进度条与缓冲条在"seek 受理"这一刻就到位】
     *
     * 位置：seek 目标点**就是**用户要求的播放位置，报它即"到位"，不必等管道走过去。
     * 之后每一次位置上报都由 getCurrentPosition() 从**同一根内容时间轴**给出
     * （P2.1：那根轴就是 mMasterClock，seek 时已被钉在目标点，之后由设备已消费量推进），
     * 所以这一跳只可能向前，不会被落点帧（目标之前的那个关键帧）的旧位置再拽回去。
     * 这是纯状态语义：目标点来自本次 seek 的入参，不依赖任何计时器/预测。
     *
     * 缓冲条：这里报的是**下限**语义 —— "到目标点为止的数据正在按本次 seek 重建"。
     * 它不会再像以前那样"先窜到目标、随后框架报出几乎为 0 的真实值又塌回来"：
     * 真正的下限由 SuperMediaPlayer 侧保证（GetBufferPosition() 与 PostBufferPositionMsg()
     * 都加了"缓冲条末端不可能早于播放头"的下限，而 seek 窗口内播放头正是目标点），
     * 也就是说框架在这之后报出的任何值都 >= 这里报出去的值，界面只会看到它继续往后长。
     *
     * 这条与下面 !mSeekInCache 分支里那段"以前把已缓冲位置直接报成 seek 目标点"的历史说明
     * 并不矛盾：那次的问题是**没有下限**（报完还会被更小的真实值拽回去），现在是先立下限再报。
     */
    mPlayer.mPNotifier->NotifyPosition(seekPos / 1000);
    mPlayer.mPNotifier->NotifyBufferPosition(seekPos / 1000);

    // TODO: why add this?
    /*
    if (mPlayer.mSeekNeedCatch && !HAVE_VIDEO) {
        mPlayer.mSeekNeedCatch = false;
    }
*/
    if (!mPlayer.mSeekInCache) {
        mPlayer.mBufferController->ClearPacket(BUFFER_TYPE_ALL);
        int64_t ret = mPlayer.mDemuxerService->Seek(seekPos, 0, -1);

        if (ret < 0) {
            mPlayer.NotifyError(ret);
        }
        /*
         * 【这里以前会把"已缓冲位置"直接报成 seek 目标点】
         *     NotifyBufferPosition(seekPos / 1000);
         * 但上面那句 ClearPacket(BUFFER_TYPE_ALL) 已经把整段缓存清空了，
         * 新 segment 还在下载 —— 等于告诉界面"一直到 seek 目标都有缓冲"。
         * 界面上看到的就是：灰色缓冲条先窜到 seek 点，随后框架报出真实的
         * 缓冲位置（几乎为 0）又塌回来，用户描述成"每次 seek 缓冲条都在重新加载"。
         * 现在不再伪造：让界面按框架真实的 BufferPositionUpdate 走，
         * 缓冲条只会随真实数据增长（seek 后本来就是从 0 重新缓冲，这是分片流的固有行为）。
         *
         * 【B15 补充】上面这段说的是"**没有下限**地伪造会塌回来"。本轮改成"先立下限再报"：
         * 受理时把目标点报出去（见本函数上方那处 NotifyPosition / NotifyBufferPosition），
         * 而框架侧之后报出的值**不可能低于播放头**（seek 窗口内播放头就是目标点），
         * 因此不会被更小的真实值拽回去 —— 先立下限、再报目标，两者配合才是"不回缩、不弹回"。
         */
        mPlayer.mEof = false;

        if ((mPlayer.mVideoChangedFirstPts != INT64_MAX) && (INT64_MIN != mPlayer.mVideoChangedFirstPts)) {
            mPlayer.mVideoChangedFirstPts = seekPos;
        }
    } else {
        AF_LOGI("sought in cache");

        if (mPlayer.mSeekNeedCatch) {
            int64_t videoPos = mPlayer.mBufferController->GetKeyTimePositionBefore(BUFFER_TYPE_VIDEO, mPlayer.mSeekPos);

            if (videoPos < (mPlayer.mSeekPos - mPlayer.mSet->maxASeekDelta)) {
                // first frame is far away from seek position, don't suppport accurate seek
                mPlayer.mSeekNeedCatch = false;
            } else {
                /*
                 * 【P2：音频与视频用**同一个目标点**】
                 *
                 * 这里原来会把"目标点之前的音频包"直接清掉
                 * （ClearPacketBeforeTimePos(AUDIO, mSeekPos)），随后又改成"保留落点→目标点
                 * 这段音频、由落点事件把音频对齐到视频落点"（C 方案）。P2 之后两者都不需要：
                 *   · 音频的对齐动作统一由 SuperMediaPlayer::DecodeVideoPacket 里那个
                 *     一次性事件完成，阈值就是**目标点**（mDiscontinuity.targetUs），
                 *     与 RenderAudio 的丢弃判据、以及音频时钟基准完全同一个值；
                 *   · "音频从目标点开始"是音频时钟模型（目标点 + 设备已消费量）的前提，
                 *     所以这里不再"保留到落点为止的音频"。
                 * 于是本处**不动音频包**：让读包路径照常把数据发下去，由上面那条判据处理。
                 */
                AF_LOGI("seek in cache: audio packets are left alone — the audio path is aligned to the "
                        "target point by the landing event (the same point the video landing filter and the "
                        "audio clock base use)\n");
            }
        }

        if ((mPlayer.mVideoChangedFirstPts != INT64_MAX) && (INT64_MIN != mPlayer.mVideoChangedFirstPts) &&
            (seekPos > mPlayer.mVideoChangedFirstPts)) {
            mPlayer.mVideoChangedFirstPts = seekPos;
        }
    }

    /*
     * demuxer 已经完成定位后再 flush active decoder、清解码帧队列。这样不会让
     * DASH/HLS 的 Seek() 等待已经取消的 pending 流，同时保证旧位置的解码帧
     * 不会混入新 seek 时间轴。
     *
     * 【B14-A：flushRender 由 true 改成 false —— seek 是"原地重启视频"，不必碰渲染器】
     *
     * 依据是本仓库自己的结论（SuperMediaPlayer.cpp 的 FlushVideoPath 里那段说明）：
     *   "只清解码器。flushDevice() 最后会 flushVideoRender()，那会 pause/start VSync
     *    线程；seek 这种一次性'原地重启视频'没必要每次都去碰 afThread 的
     *    pause/start 状态机 —— 渲染器里缓存的旧帧下一帧本来就会被覆盖。"
     * 安卓上 flushVideoRender() 的 pause/start 是真实开销（VSync 线程握手），
     * 而 seek 每次都要走这里 ⇒ 每次 seek 省一次渲染器 flush。
     *
     * 精度不受影响：落点帧仍由内核的落点过滤判据挡着（只有"包含目标、不晚于
     * 目标"的帧会被采纳，见 RenderVideo 的采纳块），本处只是不再主动清渲染器里
     * 已提交的那 1~2 帧 —— 而 seek 期间画面本来就停在上一帧，观感一致。
     * 解码帧队列（mVideoFrameQue）在 FlushVideoPath 内部无条件清空，与 flushRender 无关。
     */
    mPlayer.FlushVideoPath(false, false, __func__);

    /*
     * ============ 【修：seek 后必须裁掉"目标点之前的陈旧视频包"】============
     *
     * 症状（真机 21:42:17 → 21:42:37）：
     *   [switch] … master=214890741 activeFrontPts=95545450 activeQ=6978
     *   drop frame,master played time is 234128251,video pts is 118668550
     * 视频活动路落后主时钟（音频）**115 秒**，队列里压着约 7000 个包（≈233 秒 4K 内容），
     * 解码器只能从**队首**一包一包往前啃（实测只有 ~1.2× 实时），画面于是只剩"每丢 8 帧
     * 由防冻阀门放 1 帧"——用户看到的就是"整个卡死"。清晰度切换也一起受害：它要追的
     * 参考点跟这堆陈旧积压完全错位，切档目标追了 37 秒（lag 5248ms→96ms）仍被判
     * `target rendition did not reach playback timeline`（界面"切换失败"）。
     *
     * 成因：seek 的 `ClearPacket(BUFFER_TYPE_ALL)` 只在 `!mSeekInCache` 那一支执行
     * （见上面 :876），而"命中缓存"的捷径（sought in cache）**一个视频包都不清**。
     * 于是：主时钟被本次 seek 钉在目标点上（本函数末尾的 setTime(seekPos)），
     * 解码器却从落后的队首重新起步 —— 两条轴从此永久错开，而解码器要追上目标点
     * 需要按"落后多少秒"把这段内容全解一遍（上百秒 = 十几分钟）。
     *
     * 处置：seek 收尾处（解码器已 flush、解码帧队列已清）按"**不晚于目标点的最近关键帧**"
     * 裁剪视频包队列。语义与落点判据完全一致：落点必须从关键帧起解，更早的包永远不可能
     * 上屏；目标点及其之后的包一个不动，音频/字幕包一个不动（音频的对齐走上面那条
     * "同一个目标点"的判据）。
     * 纯状态判断，无计时器、不改任何精度判据；非缓存支路此时队列本来就是空的 ⇒ 幂等空操作。
     */
    {
        const int64_t videoKeyBeforeSeek =
            mPlayer.mBufferController->GetKeyTimePositionBefore(BUFFER_TYPE_VIDEO, seekPos);
        const int64_t videoTrimUs = (videoKeyBeforeSeek != INT64_MIN) ? videoKeyBeforeSeek : seekPos;
        const int64_t videoTrimmed =
            mPlayer.mBufferController->ClearPacketBeforeTimePos(BUFFER_TYPE_VIDEO, videoTrimUs);

        if (videoTrimmed > 0) {
            AF_LOGW("seek: trimmed %lld stale video packets before the landing key frame (keyPos=%lld "
                    "target=%lld) — the video decoder now starts at the target instead of grinding through "
                    "the stale backlog, which is exactly what kept the picture frozen while the audio "
                    "kept playing and made the quality switch unable to reach the timeline\n",
                    (long long) videoTrimmed, (long long) videoKeyBeforeSeek, (long long) seekPos);
        }
    }

    mPlayer.FlushAudioPath();
    mPlayer.FlushSubtitleInfo();

#ifdef ENABLE_VIDEO_FILTER
    if (mPlayer.mFilterManager) {
        mPlayer.mFilterManager->clearBuffer();
    }
#endif

    if (mPlayer.mSubPlayer) {
        mPlayer.mSubPlayer->seek(seekPos);
    }

    mPlayer.mFirstBufferFlag = true;
    mPlayer.mMasterClock.setTime(seekPos);
}

void SMPMessageControllerListener::ProcessMuteMsg()
{
    mPlayer.mAVDeviceManager->setMute(mPlayer.mSet->bMute);
}

void SMPMessageControllerListener::ProcessSwitchStreamMsg(int index)
{
    if (mPlayer.mDemuxerService == nullptr) {
        return;
    }

    Stream_type type = STREAM_TYPE_UNKNOWN;
    int i;
    int number = mPlayer.mDemuxerService->GetNbStreams();

    for (i = 0; i < number; i++) {
        if (index == i) {
            unique_ptr<streamMeta> pMeta;
            mPlayer.mDemuxerService->GetStreamMeta(pMeta, i, false);
            auto *meta = (Stream_meta *) (pMeta.get());
            type = meta->type;
            break;
        }
    }

    if (i >= number) {
        AF_LOGW("no such stream\n");
        return;
    }

    if (mPlayer.mDuration == 0) {
        /* 直播清单：VIDEO / MIXED 统一走 switchVideoStream()（单解码器立即切换）。
         * 旧实现这里直接调用 SwitchStreamAligned()，而 manager 的 stopOnSegEnd 语义
         * 已取消，结果就是点击 HLS/DASH 清晰度后只记录了目标却没有真正换流。
         * 音频/字幕仍走原有路径。 */
        if (type == STREAM_TYPE_VIDEO || type == STREAM_TYPE_MIXED) {
            /* 混合流的 mediaInfo 使用复合 stream id，必须和有限时长路径
             * 一样先编码主流 id。 */
            switchVideoStream(type == STREAM_TYPE_MIXED ? GEN_STREAM_INDEX(index) : index, type);
            return;
        }
        int toIndex = index;
        int fromIndex = -1;

        if (type == STREAM_TYPE_MIXED) {
            if (mPlayer.mMainStreamId == -1 || mPlayer.mMainStreamId == toIndex) {
                AF_LOGD("current stream index is the same");
                return;
            }
            fromIndex = mPlayer.mMainStreamId;
            toIndex = GEN_STREAM_INDEX(index);
            mPlayer.mAudioChangedFirstPts = INT64_MAX;
            mPlayer.mEof = false;
        } else if (type == STREAM_TYPE_AUDIO && mPlayer.mCurrentAudioIndex >= 0 && mPlayer.mCurrentAudioIndex != index) {
            fromIndex = mPlayer.mCurrentAudioIndex;
            mPlayer.mWillChangedAudioStreamIndex = index;
        } else if (type == STREAM_TYPE_SUB && mPlayer.mCurrentSubtitleIndex >= 0 && mPlayer.mCurrentSubtitleIndex != index) {
            fromIndex = mPlayer.mCurrentSubtitleIndex;
            mPlayer.mWillChangedSubtitleStreamIndex = index;
        }
        if (fromIndex < 0) {
            AF_LOGE("invalid switch stream %d\n", index);
            return;
        }
        mPlayer.mVideoChangedFirstPts = INT64_MAX;
        mPlayer.mDemuxerService->SwitchStreamAligned(fromIndex, toIndex);
        return;
    }

    if (type == STREAM_TYPE_MIXED) {
        int id = GEN_STREAM_INDEX(index);

        if (mPlayer.mMainStreamId == -1 || mPlayer.mMainStreamId == id) {
            AF_LOGD("current stream index is the same");
            return;
        }

        mPlayer.mVideoChangedFirstPts = INT64_MAX;
        mPlayer.mAudioChangedFirstPts = INT64_MAX;
        mPlayer.mEof = false;
        switchVideoStream(id, type);
        return;
    }

    if (type == STREAM_TYPE_SUB && mPlayer.mCurrentSubtitleIndex >= 0 && mPlayer.mCurrentSubtitleIndex != index) {
        return switchSubTitle(index);
    } else if (type == STREAM_TYPE_AUDIO && mPlayer.mCurrentAudioIndex >= 0 && mPlayer.mCurrentAudioIndex != index) {
        return switchAudio(index);
    } else if (type == STREAM_TYPE_VIDEO) {
        /*
         * 【修：当前视频下标未知时不再丢弃用户的切档请求】
         *
         * 原来这一支是 `type == STREAM_TYPE_VIDEO && mPlayer.mCurrentVideoIndex >= 0 &&
         * mPlayer.mCurrentVideoIndex != index`。只要 mCurrentVideoIndex 是 -1
         * （换源 / 切档 / seek 交叉时会被置过），用户点清晰度就在这一行**无声返回**：
         * 没有日志、没有 STARTED 事件、画面不动，界面上就是"点了没反应"。
         *
         * "当前档位"这个判据真正有用的只有一件事 —— 点的是正在播的那一档就没必要
         * 白切一次；而"未知"（<0）绝不等于"不能切"：下面 switchVideoStream() 用的
         * 是**目标档位的元数据** + **主时钟位置** 作为切换起点，
         * 整条路都不依赖当前档位下标。
         */
        if (mPlayer.mCurrentVideoIndex == index) {
            AF_LOGI("quality switch ignored: stream %d is already the playing video stream\n", index);
            return;
        }

        if (mPlayer.mCurrentVideoIndex < 0) {
            AF_LOGW("quality switch: current video index is unknown (%d) — accepting the request for stream %d "
                    "instead of dropping it (the switch position comes from the master clock, not from the "
                    "current index)\n", mPlayer.mCurrentVideoIndex, index);
        }

        return switchVideoStream(index, type);
    }

    /*
     * 【修：留一条"请求没被服务"的明确日志】
     * 走到这里说明 index 指向的不是可切的对象（例如点了正在播的音频/字幕档，
     * 或流表里没有这一路）。原来这里是函数末尾静默落地，日志上完全看不出来
     * 有过一次切档请求 —— 排查时无法区分"没收到请求"和"收到了但被丢掉"。
     */
    AF_LOGW("switch stream request NOT served: index=%d type=%d duration=%lld video=%d audio=%d sub=%d\n",
            index, (int) type, (long long) mPlayer.mDuration,
            mPlayer.mCurrentVideoIndex, mPlayer.mCurrentAudioIndex, mPlayer.mCurrentSubtitleIndex);
}

void SMPMessageControllerListener::ProcessRenderedMsg(StreamType type, IAFFrame::AFFrameInfo &info, int64_t timeMs, bool rendered,
                                                      void *picUserData)
{
    if (type == ST_TYPE_AUDIO) {

        if (!rendered) {
            return;
        }
        mPlayer.mDemuxerService->SetOption("A_FRAME_RENDERED", info.pts);
        if (!mPlayer.isSeeking()) {
            if (info.timePosition >= 0) {
                mPlayer.mCurrentPos = info.timePosition;
            }
            if (info.utcTime >= 0) {
                mPlayer.mCurrentFrameUtcTime = info.utcTime;
            }
        }
        if (mPlayer.mSet->bEnableVRC) {
            mPlayer.mPNotifier->NotifyAudioRendered(timeMs, info.pts);
        }
        return;
    }


    if (type == ST_TYPE_VIDEO) {

        if ((mPlayer.mCurrentAudioIndex < 0 || mPlayer.mAudioEOS) && !mPlayer.isSeeking()) {
            if (info.timePosition >= 0) {
                mPlayer.mCurrentPos = info.timePosition;
            } else if (!mPlayer.mDemuxerService->getDemuxerHandle()->isTSDiscontinue() && info.pts >= 0) {
                mPlayer.mCurrentPos = info.pts;
            }
            if (info.utcTime >= 0) {
                mPlayer.mCurrentFrameUtcTime = info.utcTime;
            }
        }
        /*
         * exclude dropped by seeking
         */
        if (!mPlayer.mSeekFlag || rendered) {
            mPlayer.mUtil->videoRendered(rendered);
            mPlayer.mMPAUtil->videoRendered(rendered);
        }
        if (rendered) {
            mPlayer.checkFirstRender();
        }

        if (!mPlayer.mSeekFlag) {
            mPlayer.mCurVideoPts = info.pts;
        }

        //AF_LOGD("video stream render pts is %lld ， mVideoChangedFirstPts = %lld ", pts, mVideoChangedFirstPts);

        if ((INT64_MIN != mPlayer.mVideoChangedFirstPts) && (info.pts >= mPlayer.mVideoChangedFirstPts)) {
            AF_LOGD("video stream changed");
            StreamInfo *pInfo = mPlayer.GetCurrentStreamInfo(ST_TYPE_VIDEO);
            mPlayer.mPNotifier->NotifyStreamChanged(pInfo, ST_TYPE_VIDEO);
            mPlayer.mVideoChangedFirstPts = INT64_MIN;
        }

        assert(mPlayer.mDemuxerService);
        mPlayer.mDemuxerService->SetOption("V_FRAME_RENDERED", info.pts);

        if (mPlayer.mSet->bEnableVRC) {
            mPlayer.mPNotifier->NotifyVideoRendered(timeMs, info.pts);
        }
    }
}

void SMPMessageControllerListener::ProcessVideoCleanFrameMsg()
{
    while (!mPlayer.mVideoFrameQue.empty()) {
        int64_t pts = mPlayer.mVideoFrameQue.front()->getInfo().pts;
        ProcessRenderedMsg(ST_TYPE_VIDEO, mPlayer.mVideoFrameQue.front()->getInfo(), af_getsteady_ms(), false, nullptr);
        mPlayer.mVideoFrameQue.front()->setDiscard(true);
        mPlayer.mVideoFrameQue.pop();
    }

    mPlayer.mAVDeviceManager->flushVideoRender();

    mPlayer.mPlayedVideoPts = INT64_MIN;
    mPlayer.mCurVideoPts = INT64_MIN;
    mPlayer.videoDecoderFull = false;
    mPlayer.mVideoPtsRevert = false;
    mPlayer.dropLateVideoFrames = true;
}

void SMPMessageControllerListener::ProcessVideoHoldMsg(bool hold)
{
    if (mPlayer.mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)) {
        mPlayer.mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)->holdOn(hold);

        if (!hold) {
            int size = mPlayer.mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)->getRecoverQueueSize();

            if (size > mPlayer.mSet->maxVideoRecoverSize) {
                string des = "video decoder recover size too large:" + AfString::to_string(size);
                mPlayer.mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_DECODER_RECOVER_SIZE, des.c_str());
            }
        }
    }
}

void SMPMessageControllerListener::ProcessSetSpeed(float speed)
{
    if (speed < 0.5f) {
        speed = 0.5f;
    } else if (speed > 2.0f) {
        speed = 2.0f;
    }

    if (!CicadaUtils::isEqual(mPlayer.mSet->rate, speed)) {
        mPlayer.mAVDeviceManager->setSpeed(speed);
        mPlayer.mSet->rate = speed;
        mPlayer.mMasterClock.SetScale(speed);
#ifdef ENABLE_VIDEO_FILTER
        if (mPlayer.mFilterManager) {
            mPlayer.mFilterManager->setSpeed(speed);
        }
#endif
    }
}

void SMPMessageControllerListener::ProcessAddExtSubtitleMsg(const std::string &url)
{
    lock_guard<mutex> uMutex(mPlayer.mCreateMutex);

    if (mPlayer.mSubPlayer == nullptr) {
        mPlayer.mSubListener = unique_ptr<mediaPlayerSubTitleListener>(new mediaPlayerSubTitleListener(*(mPlayer.mPNotifier)));
        mPlayer.mSubPlayer = unique_ptr<subTitlePlayer>(new subTitlePlayer(*(mPlayer.mSubListener)));
    }

    mPlayer.mSubPlayer->add(url);
}

void SMPMessageControllerListener::ProcessSelectExtSubtitleMsg(int index, bool select)
{
    lock_guard<mutex> uMutex(mPlayer.mCreateMutex);

    if (mPlayer.mSubPlayer == nullptr) {
        AF_LOGE("select ext subtitle error\n");
        mPlayer.mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_SUBTITLE_SELECT_ERROR, "No such subtitle stream");
        return;
    }

    int ret = mPlayer.mSubPlayer->select(index, select);

    if (ret < 0) {
        AF_LOGE("select ext subtitle error\n");
        mPlayer.mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_SUBTITLE_SELECT_ERROR, "No such subtitle stream");
    }

    if (select) {
        mPlayer.mSubPlayer->seek(mPlayer.getCurrentPosition());
        std::string header = mPlayer.mSubPlayer->getHeader(index);
        if (!header.empty()) {
            mPlayer.mPNotifier->NotifySubtitleHeader(index, header.c_str());
        }
    }
}


void SMPMessageControllerListener::switchVideoStream(int index, Stream_type type)
{
    int count = (int) mPlayer.mMediaInfo.mStreamInfoQueue.size();
    StreamInfo *currentInfo = nullptr;
    StreamInfo *willChangeInfo = nullptr;
    int i;
    int currentId = mPlayer.mCurrentVideoIndex;

    if (type == STREAM_TYPE_MIXED) {
        currentId = GEN_STREAM_INDEX(mPlayer.mCurrentVideoIndex);
    }

    for (i = 0; i < count; i++) {
        StreamInfo *info = mPlayer.mMediaInfo.mStreamInfoQueue[i];

        if (info->streamIndex == index) {
            willChangeInfo = info;
        }

        if (currentId == info->streamIndex) {
            currentInfo = info;
        }
    }

    /*
     * 【修：手动切清晰度被"只用于日志的判据"悄悄吃掉】
     *
     * 原来这里是 `if (!willChangeInfo || !currentInfo) { return; }`。其中 currentInfo
     * （= mCurrentVideoIndex 对应的那一档）在下面的全部代码里**只被一条 AF_LOGD 用到**
     * （就是紧跟着这行的 before/after 码率），却成了整个切档的前置条件。于是只要
     * "当前档位下标"和 mMediaInfo 的流表对不上（换源/seek/切档交叉时 mCurrentVideoIndex
     * 会被置成 -1，或下标空间被换过），用户点清晰度就**一声不响地什么都不做**：
     * 没有 PLAYER_QUALITY_SWITCH_STARTED、没有日志、画面不动 —— 表现就是
     * "手动切换清晰度完全不可用，100% 失败"。
     *
     * 修法：目标档位（willChangeInfo）仍然是必须的 —— 找不到它就没有可切的对象，
     * 这种情况要留下明确的 WARN；而"当前档位"只影响那一条日志，取不到就按未知打印，
     * 绝不因此丢弃用户的请求。
     */
    if (willChangeInfo == nullptr) {
        AF_LOGW("quality switch DROPPED: stream %d is not in the stream-info queue (queue=%d currentId=%d) "
                "— nothing can be switched to\n", index, count, currentId);
        return;
    }

    /*
     * 【P3】"同一个目标档的重复请求不再取消再重启"：判据换成新模型的单一闩
     * （请求的目标 == 在途请求的目标 ⇒ 什么都不做，在途那次本来就在朝同一个目标走）。
     */
    if (mPlayer.mVideoSwitchInFlight && mPlayer.mVideoSwitchTargetIndex == index) {
        AF_LOGI("quality switch ignored: stream %d is already the in-flight target\n", index);
        return;
    }

    AF_LOGD("video change video bitrate before is %d,after is %d",
            currentInfo != nullptr ? currentInfo->videoBandwidth : 0, willChangeInfo->videoBandwidth);

    /*
     * ① 记录目标档 + 切换点。切换点取**唯一内容时间轴**（P2.1 起 getCurrentPosition() 读的就是
     *    mMasterClock：有音频 = 目标点 + 设备已消费量，无音频 = 暂停感知的自走时钟）。
     */
    mPlayer.mMixMode = (type == STREAM_TYPE_MIXED);
    mPlayer.mVideoSwitchTargetIndex = index;
    mPlayer.mVideoSwitchInFlight = true;

    int64_t switchPos = mPlayer.getCurrentPosition();

    if (switchPos < 0 || switchPos == INT64_MIN) {
        switchPos = 0;
    }

    /*
     * 【不要在这里设"等目标路首帧"的闩】这里原来有一行
     * `mPlayer.mVideoChangedFirstPts = INT64_MAX;` —— 双解码器时代用它把后续切档请求
     * 延后派发（见 OnPlayerMsgIsPadding）。单解码器切档是同步完成的，这行既是死代码，
     * 又会让那个永远复位不了的闩把切档请求永久扣在消息队列里（真机实测的"点了没反应"）。
     */
    mPlayer.mPNotifier->NotifyVideoQualitySwitch(PLAYER_QUALITY_SWITCH_STARTED, index, "quality switch started");

    /*
     * ② **边界对齐切换**（ABR 与手动共用的唯一出口）：只把"目标档 + 边界触发器"交给解复用层
     *    —— 旧流继续 read/解码/渲染，直到它自己读到本分片末尾；解复用层在分片边界完成换流，
     *    并把目标流的读位置对齐到"旧流分片号 + 1"（DashManager.cpp:221-247）。
     *    目标流的第一个视频包到达时，由 ReadPacket 的提交块坐实当前档，并按 meta 决定
     *    是否 flush / 丢旧档残留包 / 重建同一块解码器。
     *
     *    切过去的时候目标流那一片已经在缓冲里，所以不需要 seek、不需要清包、不会卡顿。
     *    终态仍然只有两种：已提交且首帧交给渲染器 ⇒ READY；真正的错误（arm 失败 / meta 读不到 /
     *    重建失败）⇒ FAILED。没有预热等待、没有任何死线。
     *
     *    ABR 走的也是本函数（AbrBufferAlgoStrategy::RequestSwitch → MediaPlayer::abrChanged →
     *    CicadaSwitchStreamIndex → SuperMediaPlayer::SwitchStream → MSG_CHANGE_VIDEO_STREAM →
     *    ProcessSwitchStreamMsg → 这里），所以这一行同时把 ABR 和手动改成边界对齐。
     */
    mPlayer.SwitchVideoAligned(switchPos);
}

void SMPMessageControllerListener::switchAudio(int index)
{
    // TODO: use position to seek demuxer ,and drop the late packet
    int ret = mPlayer.mDemuxerService->OpenStream(index);

    if (ret < 0) {
        AF_LOGD("subtitle", "switch audio open stream failed,stream index %d\n", index);
        return;
    }
    std::unique_ptr<streamMeta> meta;
    mPlayer.mDemuxerService->GetStreamMeta(meta, index, true);

    mPlayer.mDemuxerService->CloseStream(mPlayer.mCurrentAudioIndex);
    mPlayer.mAudioChangedFirstPts = INT64_MAX;
    mPlayer.mCurrentAudioIndex = index;
    mPlayer.mCATimeBase = ((Stream_meta *) (*meta))->ptsTimeBase;
    int64_t playTime = mPlayer.mMasterClock.GetTime();
    int64_t pts = playTime - mPlayer.mFirstAudioPts;
    mPlayer.mMasterClock.setReferenceClock(nullptr, nullptr);
    mPlayer.mBufferController->ClearPacket(BUFFER_TYPE_AUDIO);
    mPlayer.mEof = false;
    mPlayer.FlushAudioPath();
    mPlayer.mDemuxerService->Seek(pts, 0, index);
    mPlayer.mPlayedAudioPts = INT64_MIN;
}

void SMPMessageControllerListener::switchSubTitle(int index)
{
    int ret = mPlayer.mDemuxerService->OpenStream(index);

    if (ret < 0) {
        AF_LOGD("subtitle", "switch subtitle open stream failed,stream index %d\n", index);
        return;
    }

    mPlayer.mSubtitleChangedFirstPts = INT64_MAX;
    mPlayer.mDemuxerService->CloseStream(mPlayer.mCurrentSubtitleIndex);
    mPlayer.mCurrentSubtitleIndex = index;
    unique_ptr<streamMeta> Meta = nullptr;
    mPlayer.mDemuxerService->GetStreamMeta(Meta, index, true);
    if (Meta) {
        Stream_meta *meta = ((Stream_meta *) (*Meta));
        if (meta->extradata && meta->extradata_size > 0) {
            mPlayer.mPNotifier->NotifySubtitleHeader(mPlayer.mCurrentSubtitleIndex, (const char *) meta->extradata);
        }
    }
    mPlayer.mBufferController->ClearPacket(BUFFER_TYPE_SUBTITLE);
    mPlayer.mEof = false;
    mPlayer.mSubtitleEOS = false;
    mPlayer.FlushSubtitleInfo();
    mPlayer.mDemuxerService->Seek(mPlayer.getCurrentPosition(), 0, index);
}

int SMPMessageControllerListener::openUrl()
{
    IDataSource::SourceConfig config{};
    config.low_speed_time_ms = mPlayer.mSet->timeout_ms;
    config.low_speed_limit = 1;

    switch (mPlayer.mSet->mIpType) {
        case IpResolveWhatEver:
            config.resolveType = IDataSource::SourceConfig::IpResolveWhatEver;
            break;
        case IpResolveV4:
            config.resolveType = IDataSource::SourceConfig::IpResolveV4;
            break;
        case IpResolveV6:
            config.resolveType = IDataSource::SourceConfig::IpResolveV6;
            break;
    }
    //   config.max_time_ms = mSet->timeout;
    config.connect_time_out_ms = mPlayer.mSet->timeout_ms;
    config.http_proxy = mPlayer.mSet->http_proxy;
    config.refer = mPlayer.mSet->refer;
    config.userAgent = mPlayer.mSet->userAgent;
    config.customHeaders = mPlayer.mSet->customHeaders;
    config.listener = mPlayer.mSourceListener.get();
    mPlayer.mSourceListener->enableRetry();

    if (mPlayer.mCanceled) {
        return FRAMEWORK_ERR_EXIT;
    }

    {
        std::lock_guard<std::mutex> locker(mPlayer.mCreateMutex);
        mPlayer.mDataSource = dataSourcePrototype::create(mPlayer.mSet->url, &(mPlayer.mSet->mOptions), DS_NEED_CACHE);
    }

    if (mPlayer.mDataSource) {
        mPlayer.mDataSource->setUrlToUniqueIdCallback(mPlayer.mUrlHashCb, mPlayer.mUrlHashCbUserData);
        mPlayer.mDataSource->Set_config(config);
        int ret = mPlayer.mDataSource->Open(0);
        return ret;
    }

    return -1;
}
