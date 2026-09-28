//
//  MediaPlayer.cpp
//
//  Created by shiping.csp on 2018/11/12.
//

#include "MediaPlayer.h"
#include "abr/AbrBufferAlgoStrategy.h"
#include "abr/AbrManager.h"
#include "media_player_api.h"
#include <muxer/ffmpegMuxer/FfmpegMuxer.h>
#include <utils/af_string.h>
#include <utils/file/FileUtils.h>
#include <utils/frame_work_log.h>
#include <utils/timer.h>
#include <utils/uuid.h>

#include "analytics/AnalyticsCollectorFactory.h"
#include "analytics/AnalyticsQueryListener.h"
#include "media_player_error_def.h"
#include "PlayerCacheDataSource.h"
#include "QueryListener.h"

using namespace Cicada;

namespace Cicada {

#define GET_PLAYER_HANDLE  playerHandle* handle = (playerHandle*)mPlayerHandle;
#define GET_MEDIA_PLAYER MediaPlayer* player = (MediaPlayer*)userData;
    MediaPlayer::MediaPlayer(const char *opt) : MediaPlayer((AnalyticsCollectorFactory::Instance()), opt)
    {
    }

    MediaPlayer::MediaPlayer(IAnalyticsCollectorFactory &factory, const char *opt) : mCollectorFactory(factory)
    {
        playerHandle *handle = CicadaCreatePlayer(opt);
        mPlayerHandle = (void *) handle;
        playerListener listener{nullptr};
        listener.userData = this;
        listener.EventCallback = eventCallback;
        listener.ErrorCallback = errorFrameCallback;
        listener.LoopingStart = loopingStartCallback;
        listener.Prepared = preparedCallback;
        listener.Completion = completionCallback;
        listener.FirstFrameShow = firstFrameCallback;
        listener.VideoSizeChanged = videoSizeChangedCallback;
        listener.VideoRendered = videoRenderedCallback;
        listener.AudioRendered = audioRenderedCallback;
        listener.PositionUpdate = currentPositionCallback;
        listener.UtcTimeUpdate = currentUtcTimeCallback;
        listener.BufferPositionUpdate = bufferPositionCallback;
        listener.LoadingStart = loadingStartCallback;
        listener.LoadingEnd = loadingEndCallback;
        listener.LoadingProgress = loadingProgressCallback;
        listener.CurrentDownLoadSpeed = currentDownLoadSpeed;
        listener.Seeking = PlayerSeeking;
        listener.SeekEnd = PlayerSeekEnd;
        listener.SubtitleShow = subtitleShowCallback;
        listener.SubtitleHide = subtitleHideCallback;
        listener.SubtitleExtAdd = subtitleExtAddedCallback;
        listener.SubtitleHeader = subtitleHeaderCallback;
        listener.MediaInfoGet = mediaInfoGetCallback;
        listener.StreamSwitchSuc = streamChangedSucCallback;
        listener.VideoQualitySwitch = videoQualitySwitchCallback;
        listener.StatusChanged = PlayerStatusChanged;
        listener.CaptureScreen = captureScreenResult;
        listener.AutoPlayStart = autoPlayStart;
        CicadaSetListener(handle, listener);
        CicadaSetMediaFrameCb(handle, onMediaFrameCallback, this);
        mConfig = new MediaPlayerConfig();
        configPlayer(mConfig);
        mQueryListener = new QueryListener(this);
        mCollector = mCollectorFactory.createAnalyticsCollector(mQueryListener);
        bExternalCollector = false;
        mAbrManager = new AbrManager();
        std::function<void(int)> fun = [this](int stream) -> void {
            return this->abrChanged(stream);
        };
        mAbrAlgo = new AbrBufferAlgoStrategy(fun);
        mAbrRefData = new AbrBufferRefererData(handle);
        mAbrAlgo->SetRefererData(mAbrRefData);
        mAbrManager->SetAbrAlgoStrategy(mAbrAlgo);
        mAbrAlgo->SetSwitchStatusCallback([this](AbrAlgoStrategy::Status status) -> void {
            if (mCollector != nullptr) {
                mCollector->ReportAbrSwitchStatus((int) status);
            }
        });

        refreshPlayerSessionId();
    }

    void MediaPlayer::refreshPlayerSessionId() {
        char signatureStr[100] = {0};
        uuid id{};
        uuid4_generate( &id );
        uuid_to_string( &id, signatureStr);
        mPlayerSessionId = string(signatureStr);
        if(mCollector != nullptr) {
            mCollector->ReportUpdatePlaySession(mPlayerSessionId);
        }
        
        GET_PLAYER_HANDLE;
        CicadaSetOption(handle, "sessionId" , mPlayerSessionId.c_str());
    }

    string MediaPlayer::GetPlayerSessionId() {
        return mPlayerSessionId;
    }

    void MediaPlayer::SetAnalyticsCollector(IAnalyticsCollector * collector) {
        if (mCollector && !bExternalCollector) {
            mCollectorFactory.destroyAnalyticsCollector(mCollector);
            // avoid be used in derivative class
            mCollector = nullptr;
        }

        bExternalCollector = true;
        mCollector = collector;
        if(mCollector != nullptr) {
            mCollector->ReportUpdatePlaySession(mPlayerSessionId);
        }
    }

    void MediaPlayer::dummyFunction(bool dummy)
    {
#ifdef ENABLE_MUXER
        if (dummy) {
            FfmpegMuxer ffmpegMuxer("", "");
        }
#endif
    }

    MediaPlayer::~MediaPlayer()
    {
        auto *handle = (playerHandle *) mPlayerHandle;
        CicadaReleasePlayer(&handle);
        delete mQueryListener;
        delete mAbrManager;
        delete mAbrAlgo;
        delete mAbrRefData;
        delete mConfig;

        if (mCollector && !bExternalCollector) {
            mCollectorFactory.destroyAnalyticsCollector(mCollector);
            // avoid be used in derivative class
            mCollector = nullptr;
        }
#ifdef ENABLE_CACHE_MODULE
        delete mCacheManager;

#endif
    }

    int64_t MediaPlayer::GetMasterClockPts()
    {
        GET_PLAYER_HANDLE
        return CicadaGetMasterClockPts(handle);
    }
    void MediaPlayer::SetClockRefer(clockRefer cb, void *arg)
    {
        GET_PLAYER_HANDLE
        return CicadaSetClockRefer(handle,cb,arg);
    }

    void MediaPlayer::abrChanged(int stream)
    {
        std::lock_guard<std::mutex> lock(mMutexAbr);

        // return the selection if disable by selectTrack
        if (!mAbrManager->IsEnableAbr()) {
            return;
        }
        std::string value{};
        mAbrManager->GetOption("switchInfo", value);

        /*
         * 【修：切档在途时 ABR 让过这一 tick】
         *
         * 用户症状：点一次手动清晰度，界面先弹"已取消切换"再弹结果 —— 因为 ABR 这一 tick
         * 的请求会被内核按"新请求取代旧请求"判成 CANCELED（就是那次在途的手动切档）。
         * ABR 是**周期性**决策（每秒一次），让过这一 tick 不丢任何意图：下一次 tick 会按
         * 新的缓冲/带宽重新算一遍；而手动请求的优先级本来就高于自动档。
         *
         * 判据复用内核唯一的"切档在途"并集（ICicadaPlayer::IsStreamSwitchInFlight），
         * 不引入任何新状态、不引入计时器。
         */
        GET_PLAYER_HANDLE

        if (handle != nullptr && CicadaIsStreamSwitchInFlight(handle)) {
            AF_LOGI("abr: a quality switch is still in flight, skipping this ABR tick so the in-flight "
                    "switch is not reported as canceled (the next tick will re-evaluate)\n");
            return;
        }

        int64_t toTime = af_gettime_relative();
        int64_t fromTime = toTime - 10 * 1000000;
        CicadaJSONItem params{};
        params.addValue("from", (long)fromTime);
        params.addValue("to", (long)toTime);
        std::string playerBuffer = GetPropertyString(PropertyKey::PROPERTY_KEY_BUFFER_INFO, params);

        if (mCollector != nullptr) {
            mCollector->ReportAutoSwitchBitrateStart(value, playerBuffer);
        }

        /* handle 已在上面为"切档在途"判据取过（同一作用域，不能重复声明）。 */
        CicadaSwitchStreamIndex(handle, stream);
    }

    void MediaPlayer::SetListener(const playerListener &Listener)
    {
        mListener = Listener;
    }

    void MediaPlayer::EnableHardwareDecoder(bool bEnabled)
    {
        GET_PLAYER_HANDLE
        DecoderType type = DT_SOFTWARE;

        if (bEnabled) {
            type = DT_HARDWARE;
        }

        CicadaSetDecoderType(handle, type);
    }

    void MediaPlayer::SetView(void *view)
    {
        GET_PLAYER_HANDLE
        CicadaSetView(handle, view);
    }

    void MediaPlayer::ClearScreen()
    {
        GET_PLAYER_HANDLE;
        CicadaClearScreen(handle);
    }

    void MediaPlayer::SetDataSource(const char *url)
    {
        GET_PLAYER_HANDLE
        string playUrl;
#ifdef ENABLE_CACHE_MODULE
        if (mCacheConfig.mEnable) {
            if (mCacheManager != nullptr) {
                delete mCacheManager;
                mCacheManager = nullptr;
            }

            mCacheManager = new CacheManager();
            mCacheManager->setCacheConfig(mCacheConfig);
            mCacheManager->setSourceUrl(url);
            char descriptionLen[MAX_OPT_VALUE_LENGTH] = {0};
            CicadaGetOption(handle, "descriptionLen", descriptionLen);
            int len = atoi(descriptionLen);
            char *value = static_cast<char *>(malloc(len + 1));
            memset(value, 0, len + 1);
            CicadaGetOption(handle, "description", value);
            mCacheManager->setDescription(value);
            free(value);
            mCacheManager->setCacheFailCallback([this](int code, string msg) -> void {
                AF_LOGE("Cache fail : code = %d , msg = %s", code, msg.c_str());
                Cicada::MediaPlayer::eventCallback(MEDIA_PLAYER_EVENT_CACHE_ERROR, msg.c_str(), this);
            });
            mCacheManager->setCacheSuccessCallback([this]() -> void {
                mCacheSuccess = true;
                if (IsLoop()) {
                    //if cache success and want play loop,
                    // we set loop false to let onCompletion callback deal loop.
                    CicadaSetLoop(static_cast<playerHandle *>(mPlayerHandle), false);
                }

                Cicada::MediaPlayer::eventCallback(MEDIA_PLAYER_EVENT_CACHE_SUCCESS, nullptr, this);
            });
            ICacheDataSource *cacheDataSource = new PlayerCacheDataSource(mPlayerHandle);
            mCacheManager->setDataSource(cacheDataSource);
            playUrl = mCacheManager->init();
        }else
#endif
        {
            playUrl = url;
        }

        if (playUrl != string(url) && mPlayUrlChangedCallback != nullptr) {
            mPlayUrlChangedCallback(playUrl);
        }

        mPlayUrl = playUrl;
        CicadaSetDataSourceWithUrl(handle, playUrl.c_str());
    }

    void MediaPlayer::SetDataSource(const Manifest::MediaManifest &manifest)
    {
        GET_PLAYER_HANDLE
        mPlayUrl = "";
        CicadaSetDataSourceWithManifestObject(handle, manifest);
    }

    void MediaPlayer::SetDataSource(const std::string &jsonManifest)
    {
        GET_PLAYER_HANDLE
        mPlayUrl = "";
        CicadaSetDataSourceWithManifest(handle, jsonManifest.c_str());
    }

    void MediaPlayer::SelectTrack(int index)
    {
        /*
         * 【2026-09-21 修：切到"自动"必须**真的**变成自动】
         *
         * 原来这里只 `EnableAbr(true)`。但 ABR 线程是**可以被暂停**的：
         * `MediaPlayer::Pause()`（暂停播放）和 `MediaPlayer::SeekTo()`（每次 seek！）
         * 都会 `mAbrManager->Pause()`，只有 `Start()`（播放）和 seek 结束回调里才会
         * 重新 `Start()`。于是只要有一次 seek 没有走到结束回调（被新 seek 覆盖、
         * 中途 stop、失败），ABR 线程就一直停着：用户从手动档切回"自动"，
         * `EnableAbr(true)` 只置了个标志，线程根本没醒 —— 界面显示自动、行为却完全
         * 不动，跟"没实现"一模一样。
         *
         * 所以这里在打开 ABR 的同时把线程也拉起来。注意两条：
         *   1) `mAbrManager->Start()` 要放在 mMutexAbr **外面**调用：ABR 线程回调
         *      `abrChanged()`（持 mMutexAbr）是发生在 `AbrManager::mMutex` 里面的，
         *      在持 mMutexAbr 时再去拿 AbrManager::mMutex 就是反向加锁，会 ABBA 死锁；
         *   2) 暂停状态下不硬拉线程（ABR 不该在暂停时自己切档）；那种情况只
         *      EnableAbr(true)，用户按播放时 `MediaPlayer::Start()` 会把它起来。
         */
        bool enableAbr = false;
        {
            std::lock_guard<std::mutex> lock(mMutexAbr);
            GET_PLAYER_HANDLE

            if (SELECT_TRACK_VIDEO_AUTO == index) {
                mAbrManager->EnableAbr(true);
                enableAbr = true;
            } else if (index < SELECT_TRACK_VIDEO_AUTO) {
                return;
            } else {
                StreamType type = CicadaSwitchStreamIndex(handle, index);

                if (ST_TYPE_VIDEO == type) {
                    mAbrManager->EnableAbr(false);
                }
            }
        }

        /*
         * 【2026-09-27 修：这里必须看**当前**状态，不能看 mOldPlayStatus】
         *
         * 原判据是 `mOldPlayStatus == PLAYER_PLAYING`，而 mOldPlayStatus 存的是
         * "上一次状态迁移的**旧**状态"（见 PlayerStatusChanged）：例如"暂停→播放"之后
         * 它是 PLAYER_PAUSED。于是用户"手动选一档 → 再切回自动"时，这个条件常常不成立
         * ⇒ 不调用 mAbrManager->Start()，ABR 线程继续停着（AbrAdjustFun 要求 mRunning）
         * ⇒ 界面显示"自动"、行为完全不动 —— 正是用户报的"切不回自动"。
         *
         * 改为看 mPlayStatus（本次新增的"当前状态"）。暂停态仍然不硬拉线程（ABR 不该在
         * 暂停时自己切档），等用户按播放时 MediaPlayer::Start() 把它起来 —— 语义不变。
         */
        if (enableAbr && mPlayStatus == PLAYER_PLAYING) {
            mAbrManager->Start();
        }
    }

    bool MediaPlayer::IsEnableAbr()
    {
        return mAbrManager->IsEnableAbr();
    }

    void MediaPlayer::Prepare()
    {
        if(mFirstPrepared) {
            refreshPlayerSessionId();
        } else {
            mFirstPrepared = true;
        }

        if (mCollector) {
            mCollector->ReportBlackInfo();
            mCollector->ReportPrepare();
        }

        GET_PLAYER_HANDLE
        CicadaPreparePlayer(handle);
    }

    void MediaPlayer::Start()
    {
        if (mCollector) {
            mCollector->ReportPlay();
        }

        mAbrManager->Start();
        GET_PLAYER_HANDLE
        CicadaStartPlayer(handle);
    }

    void MediaPlayer::Pause()
    {
        if (mCollector) {
            mCollector->ReportPause();
        }

        mAbrManager->Pause();
        GET_PLAYER_HANDLE
        CicadaPausePlayer(handle);
    }

    void MediaPlayer::SetVolume(float volume)
    {
        GET_PLAYER_HANDLE
        CicadaSetVolume(handle, volume);
    }

    float MediaPlayer::GetVolume()
    {
        GET_PLAYER_HANDLE
        return CicadaGetVolume(handle);
    }

    void MediaPlayer::SeekTo(int64_t seekPos, SeekMode mode)
    {
        if (mCollector) {
            mCollector->ReportSeekStart(seekPos, mode);
        }

        //TODO: use mode to seek
        GET_PLAYER_HANDLE
        bool bAccurate = false;

        if (mode & SEEK_MODE_ACCURATE) {
            bAccurate = true;
        }

        CicadaSeekToTime(handle, seekPos, bAccurate);
        //when seek, close abrmanager
        mAbrManager->Pause();
    }

    void MediaPlayer::CaptureScreen()
    {
        GET_PLAYER_HANDLE
        CicadaCaptureScreen(handle);

        if (mCollector) {
            mCollector->ReportSnapshot();
        }
    }

    void MediaPlayer::Stop()
    {
        if (mCollector) {
            mCollector->ReportStop();
        }

        mAbrManager->Stop();
        mAbrManager->Reset();
        mAbrManager->EnableAbr(false);
        mAbrAlgo->Clear();
#ifdef ENABLE_CACHE_MODULE
        if (IsLoop() && mCacheSuccess) {
            GET_PLAYER_HANDLE
            CicadaSetLoop(handle, true);
        }
        mCacheSuccess = false;
        if (mCacheManager != nullptr) {
            mCacheManager->stop("cache stopped by stop");
        }
#endif
        waitingForLoop = false;
        waitingForStart = false;
        GET_PLAYER_HANDLE
        CicadaStopPlayer(handle);
    }

    int64_t MediaPlayer::GetDuration()
    {
        GET_PLAYER_HANDLE
        return CicadaGetDuration(handle);
    }

    int MediaPlayer::GetCurrentStreamIndex(StreamType type)
    {
        GET_PLAYER_HANDLE
        return CicadaGetCurrentStreamIndex(handle, type);
    }

    StreamInfo *MediaPlayer::GetCurrentStreamInfo(StreamType type)
    {
        GET_PLAYER_HANDLE
        return CicadaGetCurrentStreamInfo(handle, type);
    }

    bool MediaPlayer::IsVideoDecoderHardware()
    {
        GET_PLAYER_HANDLE
        return CicadaIsVideoDecoderHardware(handle);
    }

    int64_t MediaPlayer::GetCurrentPosition()
    {
        GET_PLAYER_HANDLE
        return CicadaGetCurrentPosition(handle);
    }

    int64_t MediaPlayer::GetBufferedPosition()
    {
        GET_PLAYER_HANDLE
        return CicadaGetCurrentBufferedPosition(handle);
    }

    void MediaPlayer::EnterBackGround(bool back)
    {
        GET_PLAYER_HANDLE
        CicadaEnterBackGround(handle, back);
    }

    void MediaPlayer::SetMute(bool bMute)
    {
        GET_PLAYER_HANDLE
        CicadaSetMute(handle, bMute);
    }

    bool MediaPlayer::IsMuted()
    {
        GET_PLAYER_HANDLE
        return CicadaIsMute(handle);
    }

    void MediaPlayer::SetConfig(const MediaPlayerConfig *config)
    {
        //TODO:SetConfig
        *mConfig = *config;
        configPlayer(mConfig);
    }

    void MediaPlayer::configPlayer(const MediaPlayerConfig *config) const
    {
        GET_PLAYER_HANDLE
        //100
        MediaPlayerConfig playerConfig = *config;

        if (playerConfig.maxDelayTime < 0) {
            playerConfig.maxDelayTime = 0;
        }

        if (playerConfig.networkTimeout < 0) {
            playerConfig.networkTimeout = 0;
        }

        if (playerConfig.startBufferDuration < 0) {
            playerConfig.startBufferDuration = 0;
        }

        if (playerConfig.highBufferDuration < 0) {
            playerConfig.highBufferDuration = 0;
        }

        if (playerConfig.maxDelayTime > playerConfig.maxBufferDuration) {
            playerConfig.maxDelayTime = playerConfig.maxBufferDuration;
        }

        //must
        if ((0 < playerConfig.maxDelayTime)
                && (playerConfig.maxDelayTime < playerConfig.highBufferDuration)) {
            playerConfig.highBufferDuration = playerConfig.maxDelayTime;
        }

        if (playerConfig.startBufferDuration > playerConfig.maxBufferDuration) {
            playerConfig.startBufferDuration = playerConfig.maxBufferDuration;
        }

        //must
        if (playerConfig.highBufferDuration > playerConfig.maxBufferDuration) {
            playerConfig.maxBufferDuration = playerConfig.highBufferDuration;
        }

        CicadaSetDropBufferThreshold(handle, playerConfig.maxDelayTime);
        CicadaSetRefer(handle, playerConfig.referer.c_str());
        CicadaSetTimeout(handle, playerConfig.networkTimeout);
        CicadaSetUserAgent(handle, playerConfig.userAgent.c_str());
        string chStartBufferDur = to_string(playerConfig.startBufferDuration);
        CicadaSetOption(handle, "startBufferDuration", chStartBufferDur.c_str());
        string chMaxDelayTime = to_string(playerConfig.maxDelayTime);
        CicadaSetOption(handle, "RTMaxDelayTime", chMaxDelayTime.c_str());
        string chHighBufDur = to_string(playerConfig.highBufferDuration);
        CicadaSetOption(handle, "highLevelBufferDuration", chHighBufDur.c_str());
        string chMaxBufDur = to_string(playerConfig.maxBufferDuration);
        CicadaSetOption(handle, "maxBufferDuration", chMaxBufDur.c_str());
        CicadaSetOption(handle, "liveStartIndex", to_string(playerConfig.liveStartIndex).c_str());
        CicadaSetOption(handle, "http_proxy", playerConfig.httpProxy.c_str());
        CicadaSetOption(handle, "ClearShowWhenStop", playerConfig.bClearShowWhenStop ? "1" : "0");
        CicadaSetOption(handle, "enableVideoTunnelRender", playerConfig.bEnableTunnelRender ? "1" : "0");
        CicadaSetOption(handle, "disableAudio", playerConfig.mDisableAudio ? "1" : "0");
        CicadaSetOption(handle, "disableVideo", playerConfig.mDisableVideo ? "1" : "0");
        CicadaSetOption(handle, "timerInterval", to_string(playerConfig.mPositionTimerIntervalMs).c_str());
        CicadaSetOption(handle, "networkRetryCount", to_string(playerConfig.networkRetryCount).c_str());
        CicadaSetOption(handle, "maxBackwardBufferDuration", to_string(playerConfig.mMaxBackwardBufferDuration).c_str());
        CicadaSetOption(handle, "preferAudio", playerConfig.preferAudio ? "1" : "0");
        if (playerConfig.pixelBufferOutputFormat != 0) {
            CicadaSetOption(handle, "pixelBufferOutputFormat", to_string(playerConfig.pixelBufferOutputFormat).c_str());
        }
        CicadaRemoveAllCustomHttpHeader(handle);

        //add custom http header
        for (auto &customHeader : playerConfig.customHeaders) {
            CicadaAddCustomHttpHeader(handle, customHeader.c_str());
        }

        *mConfig = playerConfig;
    }

    const MediaPlayerConfig *MediaPlayer::GetConfig()
    {
        return mConfig;
    }

    void MediaPlayer::SetScaleMode(ScaleMode mode)
    {
        GET_PLAYER_HANDLE
        CicadaSetScaleMode(handle, mode);
    }

    ScaleMode MediaPlayer::GetScaleMode()
    {
        GET_PLAYER_HANDLE
        return CicadaGetScaleMode(handle);
    }

    void MediaPlayer::SetLoop(bool bLoop)
    {
        mLoop = bLoop;

        if (mCollector) {
            mCollector->ReportLooping(bLoop);
        }

        GET_PLAYER_HANDLE
        CicadaSetLoop(handle, bLoop);
#ifdef ENABLE_CACHE_MODULE
        //if cache successed before setLoop.
        if (mCacheManager != nullptr) {
            if (mCacheSuccess && IsLoop()) {
                //if cache success and want play loop,
                // we set loop false to let onCompletion callback deal loop.
                CicadaSetLoop(handle, false);
            }
        }
#endif
    }

    bool MediaPlayer::IsLoop()
    {
        return mLoop;//CicadaGetLoop(handle);
    }

    int MediaPlayer::GetVideoWidth()
    {
        int width = 0;
        int height = 0;
        GET_PLAYER_HANDLE
        CicadaGetVideoResolution(handle, width, height);
        return width;
    }

    int MediaPlayer::GetVideoHeight()
    {
        int width = 0;
        int height = 0;
        GET_PLAYER_HANDLE
        CicadaGetVideoResolution(handle, width, height);
        return height;
    }

    int MediaPlayer::GetVideoRotation()
    {
        int rotation = 0;
        GET_PLAYER_HANDLE
        CicadaGetVideoRotation(handle, rotation);
        return rotation;
    }


    void MediaPlayer::SetRotateMode(RotateMode mode)
    {
        if (mCollector) {
            mCollector->ReportRotate(mode);
        }

        GET_PLAYER_HANDLE
        CicadaSetRotateMode(handle, mode);
    }

    RotateMode MediaPlayer::GetRotateMode()
    {
        GET_PLAYER_HANDLE
        return CicadaGetRotateMode(handle);
    }

    void MediaPlayer::SetMirrorMode(MirrorMode mode)
    {
        if (mCollector) {
            mCollector->ReportRenderMirrorMode(mode);
        }

        GET_PLAYER_HANDLE
        CicadaSetMirrorMode(handle, mode);
    }

    MirrorMode MediaPlayer::GetMirrorMode()
    {
        GET_PLAYER_HANDLE
        return CicadaGetMirrorMode(handle);
    }

    /* 【色觉辅助滤镜 / 回退点 C2】照 SetMirrorMode 的写法转发，无 analytics 上报（新增通道，不动已有上报体系） */
    void MediaPlayer::SetColorMatrix(const float matrix[9])
    {
        if (matrix == nullptr) {
            return;
        }

        GET_PLAYER_HANDLE
        CicadaSetColorMatrix(handle, matrix);
    }

    std::string MediaPlayer::GetVideoCodecSupport()
    {
        GET_PLAYER_HANDLE

        if (handle == nullptr) {
            return std::string();
        }

        const char *json = CicadaGetVideoCodecSupport(handle);

        /*
         * 先拷贝再释放：返回的是 malloc 出来的副本，必须用 CicadaFreeString()（不能 free/delete）。
         * 传空指针给释放函数是安全的。
         */
        std::string result = (json != nullptr) ? std::string(json) : std::string();
        CicadaFreeString(json);
        return result;
    }

    int MediaPlayer::SetVideoCodecSupport(const char *json)
    {
        GET_PLAYER_HANDLE

        if (handle == nullptr) {
            return -1;
        }

        return CicadaSetVideoCodecSupport(handle, json);
    }

    void MediaPlayer::SetVideoBackgroundColor(uint32_t color)
    {
        GET_PLAYER_HANDLE
        CicadaSetVideoBackgroundColor(handle, color);
    }

    void MediaPlayer::SetSpeed(float speed)
    {
        GET_PLAYER_HANDLE
        CicadaPlayerSetSpeed(handle, speed);
    }

    float MediaPlayer::GetSpeed()
    {
        GET_PLAYER_HANDLE
        return CicadaPlayerGetSpeed(handle);
    }

    void MediaPlayer::SetTraceID(const char *traceID)
    {
        //TODO:
    }

    void MediaPlayer::SetComponentCb(player_component_type type, void *factory)
    {
        GET_PLAYER_HANDLE
        CicadaSetComponentCb(handle, type, factory);
    }

    std::string MediaPlayer::GetPropertyString(PropertyKey key)
    {
        GET_PLAYER_HANDLE
        return CicadaGetPropertyString(handle, key, {});
    }

    std::string MediaPlayer::GetPropertyString(PropertyKey key, const CicadaJSONItem &param)
    {
        GET_PLAYER_HANDLE
        return CicadaGetPropertyString(handle, key, param);
    }

    void MediaPlayer::SetOption(const char *key, const char *value)
    {
        GET_PLAYER_HANDLE
        CicadaSetOption(handle, key, value);
    }

    void MediaPlayer::GetOption(const char *key, char *value)
    {
        GET_PLAYER_HANDLE
        CicadaGetOption(handle, key, value);
    }

    void MediaPlayer::SetDefaultBandWidth(int bandWidth)
    {
        GET_PLAYER_HANDLE
        CicadaSetDefaultBandWidth(handle, bandWidth);
    }

    void MediaPlayer::preparedCallback(void *userData)
    {
        GET_MEDIA_PLAYER

        if (player->waitingForStart) {
            player->waitingForStart = false;
            player->Start();
        }

        if (player->mCollector) {
            player->mCollector->ReportPrepared();
        }

        if (player->waitingForLoop) {
        } else {
            if (player->mListener.Prepared) {
                player->mListener.Prepared(player->mListener.userData);
            }
        }
    }

    void MediaPlayer::completionCallback(void *userData)
    {
        GET_MEDIA_PLAYER
#ifdef ENABLE_CACHE_MODULE
        if (player->mCacheManager != nullptr) {
            bool isLoop = player->IsLoop();

            if (isLoop && player->mCacheSuccess) {
                //If cacheSuccess and want to loop, reuse cache file.
                string sourceUrl = player->mCacheManager->getSourceUrl();
                player->Stop();
                player->waitingForStart = true;
                player->waitingForLoop = true;
                player->SetDataSource(sourceUrl.c_str());
                player->SetLoop(true);
                player->Prepare();

                if (player->mListener.LoopingStart) {
                    player->mListener.LoopingStart(player->mListener.userData);
                }

                return;
            }
        }
#endif
        if (player->mCollector) {
            player->mCollector->ReportCompletion();
        }

        if (player->mListener.Completion) {
            player->mListener.Completion(player->mListener.userData);
        }
    }

    void MediaPlayer::firstFrameCallback(void *userData)
    {
        GET_MEDIA_PLAYER

        if (player->mCollector) {
            player->mCollector->ReportFirstRender();
        }

        player->waitingForLoop = false;

        if (player->mListener.FirstFrameShow) {
            player->mListener.FirstFrameShow(player->mListener.userData);
        }
    }

    void MediaPlayer::errorFrameCallback(int64_t errorCode, const void *errorMsg, void *userData)
    {
        GET_MEDIA_PLAYER

        if (player->mOldPlayStatus < PLAYER_PREPARED) {
            //if play cache file , we try prepare the orignal url.
#ifdef ENABLE_CACHE_MODULE
            if (player->mCacheManager != nullptr) {
                string sourceUrl = player->mCacheManager->getSourceUrl();
                player->mCacheManager->stop("cache stopped by error");

                if (sourceUrl != player->mPlayUrl) {
                    //remove wrong cache file, and try play original url.
                    if (!Cicada::FileUtils::rmrf(player->mPlayUrl.c_str())) {
                        if (player->mListener.ErrorCallback) {
                            player->mListener.ErrorCallback(MEDIA_PLAYER_ERROR_DEMUXER_OPEN_CACHEFILE, errorMsg,
                                                            player->mListener.userData);
                        }
                        return;
                    }
                }
            }
#endif
        }

        if (player->mCollector) {
            player->mCollector->ReportError((int) errorCode, (char *) errorMsg, "");
        }

        if (player->mListener.ErrorCallback) {
            player->mListener.ErrorCallback(errorCode, errorMsg, player->mListener.userData);
        }
    }

    void MediaPlayer::eventCallback(int64_t code, const void *msg, void *userData)
    {
        GET_MEDIA_PLAYER

        if (code == MediaPlayerEventType::MEDIA_PLAYER_EVENT_SW_VIDEO_DECODER) {
            if (player->mCollector) {
                player->mCollector->ReportSwitchToSoftDecode();
            }
        } else if (code == MediaPlayerEventType::MEDIA_PLAYER_EVENT_DEMUXER_EOF) {
#ifdef ENABLE_CACHE_MODULE
            if (player->mCacheManager != nullptr) {
                player->mCacheManager->complete();
            }
#endif
        } else if (code == MediaPlayerEventType::MEDIA_PLAYER_EVENT_SYSTEM_LOW_MEMORY) {
            if (player->mCollector) {
                player->mCollector->ReportLowMemory();
            }
        }

        if (player->mListener.EventCallback) {
            player->mListener.EventCallback(code, msg, player->mListener.userData);
        }
    }

    void MediaPlayer::videoSizeChangedCallback(int64_t width, int64_t height, void *userData)
    {
        GET_MEDIA_PLAYER

        if (player->mListener.VideoSizeChanged) {
            player->mListener.VideoSizeChanged(width, height, player->mListener.userData);
        }

        if (player->mCollector) {
            player->mCollector->ReportVideoSizeChanged(static_cast<int>(width), static_cast<int>(height));
        }
    }

    void MediaPlayer::videoRenderedCallback(int64_t timeMs, int64_t pts, void *userData)
    {
        GET_MEDIA_PLAYER

        if (player->mListener.VideoRendered) {
            player->mListener.VideoRendered(timeMs, pts, player->mListener.userData);
        }
    }

    void MediaPlayer::audioRenderedCallback(int64_t timeMs, int64_t pts, void *userData)
    {
        GET_MEDIA_PLAYER

        if (player->mListener.AudioRendered) {
            player->mListener.AudioRendered(timeMs, pts, player->mListener.userData);
        }
    }


    void MediaPlayer::currentPositionCallback(int64_t position, void *userData)
    {
        GET_MEDIA_PLAYER

        if (player->mListener.PositionUpdate) {
            player->mListener.PositionUpdate(position, player->mListener.userData);
        }
    }

    void MediaPlayer::currentUtcTimeCallback(int64_t time, void *userData)
    {
        GET_MEDIA_PLAYER

        if (player->mListener.UtcTimeUpdate) {
            player->mListener.UtcTimeUpdate(time, player->mListener.userData);
        }
    }

    void MediaPlayer::bufferPositionCallback(int64_t position, void *userData)
    {
        GET_MEDIA_PLAYER

        if (player->mListener.BufferPositionUpdate) {
            player->mListener.BufferPositionUpdate(position, player->mListener.userData);
        }
    }

    void MediaPlayer::loopingStartCallback(void *userData)
    {
        GET_MEDIA_PLAYER

        if (player->mListener.LoopingStart) {
            player->mListener.LoopingStart(player->mListener.userData);
        }

        if (player->mCollector) {
            player->mCollector->ReportLoopingStart();
        }
    }

    void MediaPlayer::loadingStartCallback(void *userData)
    {
        GET_MEDIA_PLAYER

        if (player->mListener.LoadingStart) {
            player->mListener.LoadingStart(player->mListener.userData);
        }

        if (player->mCollector) {
            player->mCollector->ReportLoadingStart();
        }
    }

    void MediaPlayer::loadingEndCallback(void *userData)
    {
        GET_MEDIA_PLAYER

        if (player->mListener.LoadingEnd) {
            player->mListener.LoadingEnd(player->mListener.userData);
        }

        if (player->mCollector) {
            player->mCollector->ReportLoadingEnd();
        }
    }

    void MediaPlayer::loadingProgressCallback(int64_t prg, void *userData)
    {
        GET_MEDIA_PLAYER

        if (player->mListener.LoadingProgress) {
            player->mListener.LoadingProgress(prg, player->mListener.userData);
        }
    }

    void MediaPlayer::currentDownLoadSpeed(int64_t speed, void *userData)
    {
        GET_MEDIA_PLAYER

        if (player->mListener.CurrentDownLoadSpeed) {
            player->mListener.CurrentDownLoadSpeed(speed, player->mListener.userData);
        }
        if (player->mAbrRefData) {
            player->mAbrRefData->setCurrentDownloadSpeed(speed);
        }
    }

    void MediaPlayer::subtitleShowCallback(int64_t index, int64_t size, const void *content,
                                           void *userData)
    {
        GET_MEDIA_PLAYER

        if (player->mListener.SubtitleShow) {
            player->mListener.SubtitleShow(index, size, content, player->mListener.userData);
        }
    }

    void MediaPlayer::subtitleHideCallback(int64_t index, int64_t size, const void *content, void *userData)
    {
        GET_MEDIA_PLAYER

        if (player->mListener.SubtitleHide) {
            player->mListener.SubtitleHide(index, size, content, player->mListener.userData);
        }
    }

    void MediaPlayer::subtitleExtAddedCallback(int64_t index, const void *url, void *userData)
    {
        GET_MEDIA_PLAYER

        if (player->mListener.SubtitleExtAdd) {
            player->mListener.SubtitleExtAdd(index, url, player->mListener.userData);
        }
    }

    void MediaPlayer::subtitleHeaderCallback(int64_t index, const void *header, void *userData)
    {
        GET_MEDIA_PLAYER

        if (player->mListener.SubtitleHeader) {
            player->mListener.SubtitleHeader(index, header, player->mListener.userData);
        }
    }

    void MediaPlayer::streamChangedSucCallback(int64_t type, const void *Info, void *userData)
    {
        GET_MEDIA_PLAYER
        auto *streamInfo = (StreamInfo *) Info;

        //when stream changed, set abr current video bitrate
        if (type == ST_TYPE_VIDEO) {
            player->mAbrAlgo->SetCurrentBitrate(streamInfo->videoBandwidth);
        }

        if (player->mListener.StreamSwitchSuc) {
            player->mListener.StreamSwitchSuc(type, Info, player->mListener.userData);
        }
    }

    void MediaPlayer::videoQualitySwitchCallback(int64_t status, int64_t streamIndex, const void *desc, void *userData)
    {
        GET_MEDIA_PLAYER
        if (player->mListener.VideoQualitySwitch) {
            player->mListener.VideoQualitySwitch(status, streamIndex, desc, player->mListener.userData);
        }
    }

    void MediaPlayer::PlayerSeeking(int64_t seekInCache, void *userData)
    {
        GET_MEDIA_PLAYER
#ifdef ENABLE_CACHE_MODULE
        if (player->mCacheManager != nullptr && seekInCache == 0) {
                //not seek in cache
            player->mCacheManager->stop("cache stopped by seek");
        }
#endif
    }

    void MediaPlayer::PlayerSeekEnd(int64_t seekInCache, void *userData)
    {
        GET_MEDIA_PLAYER

        if (player->mListener.SeekEnd) {
            player->mListener.SeekEnd(seekInCache, player->mListener.userData);
        }

        //when seek, reset and open abrmanager
        player->mAbrManager->Reset();

        StreamInfo *streamInfo = player->GetCurrentStreamInfo(ST_TYPE_VIDEO);
        if (streamInfo != nullptr && streamInfo->videoBandwidth > 0) {
            player->mAbrAlgo->SetCurrentBitrate(streamInfo->videoBandwidth);
        }

        player->mAbrManager->Start();
        if (player->mCollector) {
            player->mCollector->ReportSeekEnd();
        }
    }

    void MediaPlayer::PlayerStatusChanged(int64_t oldStatus, int64_t newStatus, void *userData)
    {
        GET_MEDIA_PLAYER
        player->mOldPlayStatus = static_cast<PlayerStatus>(oldStatus);
        /*
         * 【当前状态】新成员 mPlayStatus（追加在类末尾）：`SelectTrack(AUTO)` 判断
         * "现在是否正在播放"必须用它，不能用 mOldPlayStatus —— 后者是迁移的**旧**状态。
         */
        player->mPlayStatus = static_cast<PlayerStatus>(newStatus);

        if (player->mListener.StatusChanged) {
            player->mListener.StatusChanged(oldStatus, newStatus, player->mListener.userData);
        }

        if (player->mCollector)
            player->mCollector->ReportPlayerStatueChange(static_cast<PlayerStatus>(oldStatus),
                                                         static_cast<PlayerStatus>(newStatus));
    }

    void MediaPlayer::captureScreenResult(int64_t width, int64_t height, const void *buffer, void *userData)
    {
        GET_MEDIA_PLAYER

        if (player->mListener.CaptureScreen) {
            player->mListener.CaptureScreen(width, height, buffer, player->mListener.userData);
        }
    }

    void MediaPlayer::autoPlayStart(void *userData)
    {
        GET_MEDIA_PLAYER

        if (player->mCollector) {
            player->mCollector->ReportPlay();
        }

        if (player->mListener.AutoPlayStart) {
            player->mListener.AutoPlayStart(player->mListener.userData);
        }

        player->mAbrManager->Start();
    }

    void MediaPlayer::mediaInfoGetCallback(int64_t count, const void *Infos, void *userData)
    {
        GET_MEDIA_PLAYER

        auto *mediaInfo = (MediaInfo *) Infos;

        //add video bitrate to abr manager

        for (auto si : mediaInfo->mStreamInfoQueue) {
            if (si->type == ST_TYPE_VIDEO) {
                player->mAbrAlgo->AddStreamInfo(si->streamIndex, si->videoBandwidth);
                /*
                 * AddStreamInfo() 只带 index + 码率，判不了"同一分辨率 / 哪个编码
                 * 更省带宽"。紧接着把这一路流的编码短名（StreamInfo.videoCodec，
                 * 已由 afCodecShortName() 归一化，拿不到编码时是空串）和分辨率
                 * 一起喂进去 —— 策略里同分辨率优先更省带宽的编码就靠这两项。
                 */
                player->mAbrAlgo->SetStreamExtraInfo(si->streamIndex, si->videoCodec,
                                                     si->videoWidth, si->videoHeight);
            }
        }

        player->mAbrAlgo->SetDuration(player->GetDuration());
        StreamInfo *si = player->GetCurrentStreamInfo(ST_TYPE_VIDEO);

        if (si) {
            player->mAbrAlgo->SetCurrentBitrate(si->videoBandwidth);

            if (player->mCollector) {
                player->mCollector->ReportCurrentBitrate(si->videoBandwidth);
            }
        }

        if (player->mListener.MediaInfoGet) {
            player->mListener.MediaInfoGet(count, Infos, player->mListener.userData);
        }
    }

    void MediaPlayer::setBitStreamCb(readCB read, seekCB seek, void *arg)
    {
        GET_PLAYER_HANDLE;
        return CicadaSetBitStreamCb(handle, read, seek, arg);
    }

    void MediaPlayer::setErrorConverter(ErrorConverter *converter)
    {
        GET_PLAYER_HANDLE;
        return CicadaSetErrorConverter(handle, converter);
    }

    void MediaPlayer::SetMediaFrameCb(playerMediaFrameCb func, void *arg)
    {
        mMediaFrameFunc = func;
        mMediaFrameArg = arg;
    }

    int MediaPlayer::GetCurrentStreamMeta(Stream_meta *meta, StreamType type)
    {
        GET_PLAYER_HANDLE;
        return CicadaGetCurrentStreamMeta(handle, meta, type);
    }

    void MediaPlayer::Reload()
    {
        GET_PLAYER_HANDLE;
        CicadaReload(handle);
    }

    void MediaPlayer::SetAutoPlay(bool bAutoPlay)
    {
        GET_PLAYER_HANDLE;
        CicadaSetAutoPlay(handle, bAutoPlay);
    }

    bool MediaPlayer::IsAutoPlay()
    {
        GET_PLAYER_HANDLE;
        return CicadaIsAutoPLay(handle);
    }

    void MediaPlayer::SetCacheConfig(const CacheConfig &config)
    {
#ifdef ENABLE_CACHE_MODULE
        if (!mCacheConfig.isSame(config)) {
            if (mCacheManager != nullptr) {
                mCacheManager->stop("cache stopped by change config");
            }

            mCacheConfig = config;
        }
#endif
    }

    void MediaPlayer::SetFilterConfig(const string &filterConfig)
    {
        GET_PLAYER_HANDLE;
        CicadaSetFilterConfig(handle, filterConfig);
    }

    void MediaPlayer::UpdateFilterConfig(const std::string &target, const std::string &options)
    {
        GET_PLAYER_HANDLE;
        CicadaUpdateFilterConfig(handle, target, options);
    }

    void MediaPlayer::SetFilterInvalid(const std::string &target, bool invalid)
    {
        GET_PLAYER_HANDLE;
        CicadaSetFilterInvalid(handle, target, invalid);
    }


    string MediaPlayer::GetCachePathByURL(const string &URL)
    {
#ifdef ENABLE_CACHE_MODULE
        return CacheManager::getCachePath(URL, mCacheConfig);
#endif
        return "";
    }

    void MediaPlayer::AddExtSubtitle(const char *uri)
    {
        GET_PLAYER_HANDLE;
        CicadaAddExtSubtitle(handle, uri);
    }

    void MediaPlayer::SelectExtSubtitle(int index, bool select)
    {
        GET_PLAYER_HANDLE;
        CicadaSelectExtSubtitle(handle, index, select);
    }
    int MediaPlayer::SetStreamDelayTime(int index, int64_t time)
    {
        GET_PLAYER_HANDLE;
        return CicadaSetStreamDelayTime(handle, index, time);
    }

    void MediaPlayer::SetDataSourceChangedCallback(function<void(const string &)> urlChangedCallbak)
    {
        mPlayUrlChangedCallback = std::move(urlChangedCallbak);
    }

    void MediaPlayer::setDrmRequestCallback(const std::function<DrmResponseData*(const DrmRequestParam& drmRequestParam)> & drmCallback){
        GET_PLAYER_HANDLE;
        CicadaSetDrmRequestCallback(handle, drmCallback);
    }

    void MediaPlayer::onMediaFrameCallback(void *arg, const IAFPacket *frame, StreamType type)
    {
        auto *player = (MediaPlayer *) arg;

        if (nullptr == player) {
            return;
        }

        player->mediaFrameCallback(frame, type);
    }

    void MediaPlayer::mediaFrameCallback(const IAFPacket *frame, StreamType type)
    {
#ifdef ENABLE_CACHE_MODULE
        if (mCacheManager) {
            mCacheManager->sendMediaFrame(frame, type);
        }
#endif
        if (mMediaFrameFunc) {
            mMediaFrameFunc(mMediaFrameArg, frame, type);
        }
    }


    void MediaPlayer::EnableVideoRenderedCallback(bool enable)
    {
        GET_PLAYER_HANDLE;
        CicadaSetOption(handle, "enableVRC", enable ? "1" : "0");
    }

    void MediaPlayer::SetOnRenderFrameCallback(onRenderFrame cb, void *userData)
    {
        GET_PLAYER_HANDLE;
        CicadaSetOnRenderCallBack(handle, cb, userData);
    }

    void MediaPlayer::SetAudioRenderingCallback(onRenderFrame cb, void *userData)
    {
        GET_PLAYER_HANDLE;
        CicadaSetAudioRenderingCallBack(handle, cb, userData);
    }

    void MediaPlayer::SetVideoRenderingCallback(videoRenderingFrameCB cb, void *userData)
    {
        GET_PLAYER_HANDLE;
        CicadaSetVideoRenderingCallBack(handle, cb, userData);
    }

    void MediaPlayer::SetUpdateViewCallback(UpdateViewCB cb, void *userData)
    {
        GET_PLAYER_HANDLE;
        CicadaSetUpdateViewCallback(handle, cb, userData);
    }

    void MediaPlayer::SetUrlHashCallback(UrlHashCB cb, void *userData)
    {
        GET_PLAYER_HANDLE;
        CicadaSetUrlHashCallback(handle, cb, userData);
    }

    void MediaPlayer::SetStreamTypeFlags(uint64_t flags)
    {
        GET_PLAYER_HANDLE;
        CicadaSetOption(handle, "streamTypes", to_string(flags).c_str());
    }

    void MediaPlayer::SetIPResolveType(IpResolveType type)
    {
        GET_PLAYER_HANDLE;
        CicadaSetOption(handle, "IPResolveType", to_string(type).c_str());
    }
    void MediaPlayer::SetFastStart(bool mode)
    {
        GET_PLAYER_HANDLE;
        int value = mode;
        CicadaSetOption(handle, "fastStart", to_string(value).c_str());
    }
    int MediaPlayer::InvokeComponent(const char *content)
    {
        GET_PLAYER_HANDLE;
        return CicadaInvokeComponent(handle, content);
    }

    std::string MediaPlayer::getName()
    {
        GET_PLAYER_HANDLE;
        return CicadaGetPlayerName(handle);
    }
}
