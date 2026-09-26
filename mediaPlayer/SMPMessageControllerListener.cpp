//
// Created by pingkai on 2020/12/28.
//

#include "SMPMessageControllerListener.h"

#include "SuperMediaPlayer.h"
#include "media_player_error_def.h"
#include <cinttypes>
#include <render/video/IVideoRender.h>
#include <cassert>
#include <climits>
#include <data_source/dataSourcePrototype.h>
#include <utils/CicadaUtils.h>
#include <utils/UrlUtils.h>
#include <utils/af_string.h>
#include <utils/file/FileUtils.h>
#include <utils/timer.h>

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
            padding = mPlayer.mVideoChangedFirstPts != INT64_MIN;
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


    /*
     * 【① B17 硬要求 B】换片源 / 重新 Prepare ⇒ 清掉"等 seek 结束重新装弹"的意图：
     * 下面那个 else 分支会调用 ResetSeekStatus()（seek 结束路径），若不清，上一部片源
     * 留下的意图会被 Prepare 当成"seek 结束"而重新发起一次切档。
     */
    mPlayer.mSwitchReArmPending = false;
    mPlayer.mSwitchReArmStreamIndex = -1;
    /* 【B19 硬要求】换片源/重新 Prepare 同样不允许跨片源补做 PFR：闩一起清掉。 */
    mPlayer.mPauseFrameRestorePending = false;

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
         * 【2026-09-24 修：切档在途/提交中时，这里不许换 surface、更不许重建解码器】
         *
         * 真机 native 崩溃（Fatal signal 11 SIGSEGV, fault addr 0x4,
         * tid=ApsaraPlayerService）的成因：A 方案下 SurfaceView 尺寸变化会再次
         * setSurface ⇒ 本函数在主线程走 setOutputSurface，失败后
         * RestartVideoDecoder()（invalidateDecoder + CreateVideoDecoder），而**播放线程
         * 正在为切档创建 pending 解码器** —— 两边同时动解码器槽位/mAVDeviceManager，
         * 且都落到 tunnel 下无意义的软解兜底，最终崩在 native。
         *
         * 判据全部是**既有状态**（不新增成员、不看时间、没有定时器）：切档目标已定、
         * 解码器切换已闩、或提交待收尾 ⇒ 播放线程正在接管视频槽位，这里必须让路，
         * 只记一条日志；切档自己用既有的 finishQualitySwitch / 错误路径收尾。
         *
         * 【为什么 Qt 不受影响】整条判据包在 FLAG_DUMMY 分支内，而 Qt 永远没有
         * FLAG_DUMMY 渲染器（bEnableTunnelRender 每次播放前 reset 为 false、
         * Qt 壳不下发该选项、CicadaVideoRender 明确返回 0 而不是 FLAG_DUMMY）
         * ⇒ 恒不进入，Qt 走的还是原来那条 setOutputSurface/RestartVideoDecoder 分支。
         */
        const bool qualitySwitchInFlight = mPlayer.mPendingVideoStreamIndex >= 0 ||
                                           mPlayer.mPendingVideoDecoderSwitch ||
                                           mPlayer.mQualitySwitchCommitPending;
        if (qualitySwitchInFlight) {
            AF_LOGW("ProcessSetViewMsg: quality switch in flight (pendingStream=%d decoderSwitch=%d "
                    "commitPending=%d) — skipping the dummy surface swap and the decoder rebuild; "
                    "the switch owns the video slot\n",
                    mPlayer.mPendingVideoStreamIndex,
                    (int) mPlayer.mPendingVideoDecoderSwitch,
                    (int) mPlayer.mQualitySwitchCommitPending);
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
     * ============ 【所有 seek 入口都必须在这里设"位置地板"与"渲染闸门"】============
     *
     * 实测（用户日志）：
     *   seek anchor: master clock anchored to the first frame rendered after the seek
     *                (pts=83438, target=2844000, landingFloor=-9223372036854775808)
     * 目标 2.844s 却锚到 0.083s，而且这一条 seek 的 mSeekPositionFloorUs == INT64_MIN。
     *
     * 根因：这两样只有一个来源 —— SuperMediaPlayer::SeekTo()（API 线程、putMsg 之后）。
     * 而 seek 不止那一条入口，至少还有两条**绕过 SeekTo()** 直接派发 MSG_SEEKTO /
     * 直接调用本函数的路径：
     *   · SuperMediaPlayer::playCompleted()（循环播放重开：`mSeekPos = 0` 之后直接调用
     *     本函数，见 SuperMediaPlayer.cpp:4748 一带）；
     *   · SMPMessageControllerListener::ProcessPrepareMsg() 的"prepare 之前 seek"分支
     *     （只置 mSeekFlag，见本文件 :151 一带）。
     * 那些 seek 里地板恒为 INT64_MIN ⇒ RenderVideo 的落点判据拿不到目标、锚点判据
     * 也没有目标可比 ⇒ 旧时间轴的帧可以随时上屏并锚住主时钟。
     *
     * 这里把两样都补上（本函数是"seek 真正开始、mSeekFlag 置真"的唯一收口处）：
     *   mSeekPositionFloorUs = seekPos   位置地板 = 本次 seek 的目标（落点判据与锚点判据
     *                                    读的都是它，见 fetchSeekClockAnchorUs）；
     *   mSeekRenderGateUs    = seekPos   渲染闸门 = 同一目标，落点帧被采纳时由
     *                                    RenderVideo 立刻撤掉（之后一帧都不再挡）。
     * 与 SeekTo() 里那两行是同一语义的重复保险（SeekTo 走的消息最终也到这里），
     * 值完全一致，没有竞态后果。目标为 0 / 负值（循环重开、prepare 前的 seek）时
     * 地板同样按目标写，getCurrentPosition() 的"低于目标就报目标"对 0 无副作用。
     * ============================================================================
     */
    mPlayer.mSeekPositionFloorUs = seekPos;
    mPlayer.mSeekRenderGateUs = seekPos;

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
    mPlayer.mPlayedVideoPts = INT64_MIN;
    mPlayer.mPlayedAudioPts = INT64_MIN;
    mPlayer.mSoughtVideoPos = INT64_MIN;
    mPlayer.mCurVideoPts = INT64_MIN;
    /* 用户 seek 开始时丢弃上一次清晰度切换留下的时间轴偏移和 retired 状态；
     * seek 目标是新的 A/V 同步锚点，不能继续把 rendition 偏移应用到新帧。 */
    mPlayer.mPendingVideoPtsOffset = INT64_MIN;
    mPlayer.mActiveVideoPtsOffset = INT64_MIN;
    mPlayer.mQualitySwitchCommitPending = false;
    mPlayer.mQualitySwitchCommittedStreamIndex = -1;
    mPlayer.mVideoPtsRevert = false;
    mPlayer.mAudioPtsRevert = false;
    /*
     * 【修法 2】锚点事件在这里置位（seek 真正开始时），而不是在 SeekTo() 里。
     * SeekTo() 是 API 线程、只负责 putMsg；从 putMsg 到本函数执行之间，
     * 主循环仍可能渲染出**旧时间轴**的帧，如果那时事件已就绪就会被它消费掉，
     * 把主时钟锚到旧位置（安卓实测锚到了 1.75s 而目标是 52.085s）。
     * 放在这里之后，只有 seek 开始之后渲染的帧才可能消费该事件。
     */
    mPlayer.mSeekAnchorPending = true;
    /* 必须在调用 DASH/HLS demuxer->Seek() 之前取消 pending representation。
     * 旧顺序先对所有 selected stream 做 Seek，再 CloseStream(pending)，网络
     * 慢时 Seek 会等待已经被用户取消的目标流，表现为 seek 直接卡死。普通本地
     * 文件没有 pending 路时保持原有 SeekInCache 行为，不额外 flush。 */
    if (mPlayer.mPendingVideoStreamIndex >= 0 || mPlayer.mWillChangedVideoStreamIndex >= 0) {
        mPlayer.FlushVideoPath(true, true, __func__);

        /*
         * 【① B17：用户点过的档位意图不能就这么丢掉 —— 记下来，等 seek 结束重新装弹】
         *
         * 现场（安卓日志，7/7 次手动切档）：`status=0 started` → 本函数这条
         * `FlushVideoPath(…, cancelPendingSwitch=1)` → `status=3 canceled`，
         * 一次 READY 都没有。用户并没有改主意（他改的是**位置**，不是档位），
         * 所以把档位意图记在很多地方都会用到的两个成员上，由 seek 结束事件
         * （SuperMediaPlayer::ResetSeekStatus）**只重新发起一次**。
         *
         * 为什么不会 ping-pong（硬要求 A：必须区分"用户 seek"与"切档自己的 seek"）：
         *   · 本函数只由 MSG_SEEKTO 驱动，而全仓 `putMsg(MSG_SEEKTO)` **只有一处**
         *     （SuperMediaPlayer::SeekTo()，见那里的 putMsg）+ playCompleted 的循环重开
         *     路径也走同一个入口；
         *   · 切档自己的定位走的是 demuxer 级路径（日志里的
         *     `quality switch: target stream N seeked to …`，由切档消息处理直接调 demuxer），
         *     从不产生 MSG_SEEKTO ⇒ 切档内部 seek 永远进不到这里，也就永远不会置闩。
         *   因此"重装 → 内部 seek → 取消 → 再重装"这条环在状态上不存在。
         *
         * 显式取消优先（硬要求 B）：Reset()（换片源/停止/Prepare）与 SwitchStream()
         * （用户又选了别的档）入口都会清这两个成员；错误终态走 Reset/Stop 时同样清。
         */
        const int reArmIndex = (mPlayer.mWillChangedVideoStreamIndex >= 0)
                               ? mPlayer.mWillChangedVideoStreamIndex
                               : mPlayer.mPendingVideoStreamIndex;

        if (reArmIndex >= 0) {
            mPlayer.mSwitchReArmStreamIndex = reArmIndex;
            mPlayer.mSwitchReArmPending = true;

            AF_LOGI("quality switch: the user's switch request (stream=%d) is superseded by this seek — "
                    "remembering it and re-issuing it ONCE when the seek finishes (the preroll position "
                    "will be recomputed from the post-seek playback position)\n", reArmIndex);
        }

        mPlayer.mWillChangedVideoStreamIndex = -1;
        mPlayer.mVideoChangedFirstPts = INT64_MIN;
    }

    /*
     * ============ 矩阵第 3 格：seek 在途时必须**干净中止**在途切档（2026-09-24）============
     *
     * 上面那个 if 只覆盖"还没提交"的 pending 路（mPendingVideoStreamIndex /
     * mWillChangedVideoStreamIndex >= 0）。**已经提交**的切档（decoder 已经 promote，
     * 两个索引都是 -1、只剩 mQualitySwitchCommitPending 为真）会从这里漏过去 ——
     * 而它恰恰是最容易出事的形态：暂停态切档的 S5 渲染还欠着、S9 的提交后墙钟
     * 死线还在倒计时，而 seek 刚刚把整个视频时间轴换掉了。两者叠加的结果就是
     * 状态机停在"已提交但永远不会上屏"，最后由死线报一次 FAILED，期间用户看到
     * 的是画面卡住。
     *
     * 处理：只要还有任何在途切换（含已提交、含暂停态那三个新状态），seek 就
     * 权威地把它收掉 —— 复用现成的 finishQualitySwitch(false, …)，它会关
     * retired 流、释放 retired 解码器、清 commitPending/offset/计数器，以及
     * 三个暂停态新状态（见 finishQualitySwitch 末尾的 resetPausedSwitchState）。
     *
     * 为什么用 finishQualitySwitch 而不是 FlushVideoPath：提交之后 decoder 已经
     * 是 **active** 路，这里如果再来一次 FlushVideoPath(flushRender=1) 会把渲染器
     * 一起 flush 掉，正是历史上"seek 后 read-ahead gate 永久堵住"那条事故路径。
     * 所以只做状态收尾（不发 CANCELED，避免和真实用户操作的通知语义混淆 ——
     * 该发的终态由 finishQualitySwitch 统一发 FAILED 给 UI，高亮退回旧档）。
     */
    if (mPlayer.mQualitySwitchCommitPending ||
        mPlayer.mSwitchStartedWhilePaused ||
        mPlayer.mPausedSwitchRenderPending) {
        AF_LOGW("seek is taking over while a quality switch was in flight (commitPending=%d "
                "pausedSwitch=%d renderPending=%d) — finishing the switch cleanly before repositioning, "
                "otherwise the two in-flight state machines would wait for each other\n",
                (int) mPlayer.mQualitySwitchCommitPending,
                (int) mPlayer.mSwitchStartedWhilePaused,
                (int) mPlayer.mPausedSwitchRenderPending);

        mPlayer.finishQualitySwitch(false, "superseded by a seek that took over the video timeline");
        mPlayer.resetPausedSwitchState();
        mPlayer.mQualitySwitchPrerollDeadlineMs = 0;
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
    AF_LOGI("PFR: seek posUs=%" PRId64 " inCache=%d status=%d\n",
            seekPos, (int) mPlayer.mSeekInCache, (int) mPlayer.mPlayStatus.load());

    mPlayer.mPNotifier->NotifySeeking(mPlayer.mSeekInCache);

    /*
     * 【本轮 B15：进度条与缓冲条在"seek 受理"这一刻就到位】
     *
     * 位置：seek 目标点**就是**用户要求的播放位置，报它即"到位"，不必等管道走过去。
     * 之后每一次位置上报都被 getCurrentPosition() 的 mSeekPositionFloorUs 地板钉在目标点，
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
                 * C 方案（本轮改）：这里原来会把"目标点之前的音频包"直接清掉
                 * （ClearPacketBeforeTimePos(AUDIO, mSeekPos)），于是音频只能从**目标点**
                 * 开始，而视频从**目标点之前的关键帧**开始 —— 两者相差 1~4 秒，
                 * 视频相对主时钟永久迟到（4K 解码只有约 1×，还不清）→ 画面冻住。
                 *
                 * 现在**不动音频包**：保留缓存里"落点→目标点"这段音频。等本次 seek 的
                 * 落点关键帧被读到（落点 PTS 已知）这个**事件**发生，再由
                 * SuperMediaPlayer::DecodeVideoPacket 一次性把音频裁剪/对齐到落点
                 * （见那里的 mSeekAudioAlignDone）。这样音频与视频从**同一个落点**起步：
                 * 无债务、无静音、A/V 内容对齐。
                 */
                AF_LOGI("seek in cache: keeping the audio packets before the target so the audio can "
                        "start at the video landing keyframe (aligned by the landing event)\n");
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
     * 精度不受影响：落点帧仍由 mSeekRenderGateUs 那道门挡着（只有"包含目标、不晚于
     * 目标"的帧会被采纳，见 RenderVideo 的采纳块），本处只是不再主动清渲染器里
     * 已提交的那 1~2 帧 —— 而 seek 期间画面本来就停在上一帧，观感一致。
     * 解码帧队列（mVideoFrameQue）在 FlushVideoPath 内部无条件清空，与 flushRender 无关。
     */
    mPlayer.FlushVideoPath(false, false, __func__);

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
        /* 直播清单同样走双 decoder 切换状态机。旧实现这里直接调用
        * SwitchStreamAligned()，而我们已取消 manager 的 stopOnSegEnd 语义，
         * 结果就是点击 HLS/DASH 清晰度后只记录了目标却没有启动 pending 路。
         * 视频和混合流统一转到 switchVideoStream()，音频/字幕仍走原有路径。 */
        if (type == STREAM_TYPE_VIDEO || type == STREAM_TYPE_MIXED) {
            /* 混合流的 mediaInfo 使用复合 stream id，必须和有限时长路径
             * 一样先编码主流 id，否则 willChangeInfo 查找不到目标档位。 */
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
        } else if (type == STREAM_TYPE_VIDEO && mPlayer.mCurrentVideoIndex >= 0 && mPlayer.mCurrentVideoIndex != index) {
            fromIndex = mPlayer.mCurrentVideoIndex;
            mPlayer.mWillChangedVideoStreamIndex = index;
            mPlayer.mPNotifier->NotifyVideoQualitySwitch(PLAYER_QUALITY_SWITCH_STARTED, index, "quality switch started");
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
    } else if (type == STREAM_TYPE_VIDEO && mPlayer.mCurrentVideoIndex >= 0 && mPlayer.mCurrentVideoIndex != index) {
        return switchVideoStream(index, type);
    }
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

    if (!willChangeInfo || !currentInfo) {
        return;
    }

    AF_LOGD("video change video bitrate before is %d,after is %d", currentInfo->videoBandwidth, willChangeInfo->videoBandwidth);
    //TODO: different strategy
    mPlayer.mWillChangedVideoStreamIndex = index;
    mPlayer.mVideoChangedFirstPts = INT64_MAX;
    mPlayer.mPNotifier->NotifyVideoQualitySwitch(PLAYER_QUALITY_SWITCH_STARTED, index, "quality switch started");

    /*
     * 双 decoder 对升档、降档采用完全相同的入口：先保留旧路，再打开目标
     * representation 并从当前缓存的安全点预热。旧的“降档走
     * SwitchStreamAligned、升档直接 SwitchVideo”模型依赖 manager 在旧分片
     * 结束时替换流；现在旧流必须一直播放到目标帧真正渲染，若降档仍走旧
     * 分支，目标流根本不会 Open，表现就是 HLS 点击清晰度没有任何变化。
     */
    mPlayer.mMixMode = (type == STREAM_TYPE_MIXED);
    /*
     * 切换目标必须从当前播放时钟附近开始，而不是从公共缓存队列的末端
     * （FindSeamlessPointTimePosition）开始。后者在本地测试中会得到 40s、
     * 但当前播放点只有 20s；目标 decoder 被迫从未来 GOP 追赶，active 路
     * 同时又可能被追帧逻辑判定为落后，最终出现 FPS=0/1。主流播放器是
     * “current media time + 最近关键帧”策略：demuxer 自己选择不晚于该时刻
     * 的 segment，pending decoder 解码到当前时钟后再原子提交。
     */
    int64_t startTime = mPlayer.mMasterClock.GetTime();
    if (startTime <= 0 || startTime == INT64_MIN) {
        startTime = mPlayer.mCurrentPos;
    }
    if (startTime < 0) {
        startTime = 0;
    }
    mPlayer.SwitchVideo(startTime);
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
