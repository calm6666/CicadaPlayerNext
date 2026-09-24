#ifndef CICADA_PLAYER_SERVICE_H
#define CICADA_PLAYER_SERVICE_H

#include <string>

using namespace std;

#include "native_cicada_player_def.h"
#include "demuxer/demuxer_service.h"

#include "MediaPlayerUtil.h"

#include "player_msg_control.h"
#include "buffer_controller.h"

#include <deque>
#include "system_refer_clock.h"

#include "SMPAVDeviceManager.h"
#include "SMPMessageControllerListener.h"
#include "SMP_DCAManager.h"
#include "SuperMediaPlayerDataSourceListener.h"
#include "player_notifier.h"
#include "player_types.h"
#include "render/video/IVideoRender.h"
#include <filter/IAudioFilter.h>
#include <queue>
#include <render/audio/IAudioRender.h>
#include <utils/bitStreamParser.h>

#include "CicadaPlayerPrototype.h"
#include <cacheModule/CacheModule.h>
#include <cacheModule/cache/CacheConfig.h>
#include <codec/IDecoder.h>
#ifdef ENABLE_VIDEO_FILTER
#include <filter/FilterManager.h>
#endif

#ifdef __APPLE__

#include <TargetConditionals.h>

#endif

#include "MediaPlayerAnalyticsUtil.h"
#include "SMPRecorderSet.h"
#include "mediaPlayerSubTitleListener.h"

namespace Cicada {
    typedef struct streamTime_t {
        int64_t startTime;
        int64_t deltaTime;
        int64_t deltaTimeTmp;

    } streamTime;

    typedef enum RENDER_RESULT {
        RENDER_NONE,
        RENDER_PARTLY,
        RENDER_FULL,
    } RENDER_RESULT;

    typedef enum APP_STATUS {
        APP_FOREGROUND,
        APP_BACKGROUND,
    } APP_STATUS;

    enum class ViewUpdateStatus {
        Unknown,
        No,
        Yes,
    };

    enum class LiveTimeSyncType {
        LiveTimeSyncNormal,
        LiveTimeSyncCatchUp,
        LiveTimeSyncSlowDown
    };

    const static float MIN_SPEED = 0.5;
    const static float MAX_SPEED = 5;

    class SuperMediaPlayer : public ICicadaPlayer, private CicadaPlayerPrototype {

        friend class SuperMediaPlayerDataSourceListener;
        friend class SMP_DCAManager;
        friend class SMPMessageControllerListener;

        /*
         * ⚠ 动这个类的**成员声明**之前必读（友元 TU 按偏移访问 + 本工程增量构建不记录
         *   头文件依赖，改 .h 不会触发依赖它的 TU 重编）：
         *     * 只在成员列表**末尾追加**是安全的；
         *     * **删除/收窄/插入**任何成员都会移动其后成员的偏移，必须让内核全量重编
         *       （删掉 platform/QtPlayer/build 下各 media_player.dir 目录里的 .obj，或整个
         *       build 目录），否则旧的 SuperMediaPlayer*Listener / SMP_DCAManager 等 .obj 会按
         *       旧偏移访问新布局 → 随机 0xC0000005。
         *   完整说明见成员列表里 “以下这些成员必须留在成员列表的最末尾” 那段。
         */

    public:

        SuperMediaPlayer();

        ~SuperMediaPlayer() override;

        string getName() override
        {
            return "SuperMediaPlayer";
        }

        int SetListener(const playerListener &Listener) override;

        void SetOnRenderCallBack(onRenderFrame cb, void *userData) override;

        void SetAudioRenderingCallBack(onRenderFrame cb, void *userData) override;

        void SetVideoRenderingCallBack(videoRenderingFrameCB cb, void *userData) override;

        void SetUpdateViewCB(UpdateViewCB cb, void *userData) override;

        void SetUrlHashCB(UrlHashCB cb, void *userData) override;

        // TODO: use setParameters and setOpt to set
        void SetRefer(const char *refer) override;

        void SetUserAgent(const char *userAgent) override;

        void SetTimeout(int timeout) override;

        void SetDropBufferThreshold(int dropValue) override;

        void SetLooping(bool looping) override;

        bool isLooping() override;

        int SetOption(const char *key, const char *value) override;

        void GetOption(const char *key, char *value) override;

        int64_t GetPlayingPosition() override
        {
            return getCurrentPosition() / 1000;
        };

        int64_t GetBufferPosition() override;

        int64_t GetDuration() const override;

        PlayerStatus GetPlayerStatus() const override;

        void SetScaleMode(ScaleMode mode) override;

        ScaleMode GetScaleMode() override;

        void SetRotateMode(RotateMode mode) override;

        RotateMode GetRotateMode() override;

        void SetMirrorMode(MirrorMode mode) override;

        void SetVideoBackgroundColor(uint32_t color) override;

        MirrorMode GetMirrorMode() override;

        int GetCurrentStreamIndex(StreamType type) override;

        StreamInfo *GetCurrentStreamInfo(StreamType type) override;

        float GetVolume() const override;

        void CaptureScreen() override;

        void SetDecoderType(DecoderType type) override;

        DecoderType GetDecoderType() override;

        bool IsMute() const override;

        float GetVideoRenderFps() override;

        float GetVideoDecodeFps() override;

        void GetVideoResolution(int &width, int &height) override;

        void GetVideoRotation(int &rotation) override;

        void SetView(void *view) override;

        void ClearScreen() override;

        void SetDataSource(const char *url) override;

        void SetDataSource(const Manifest::MediaManifest &manifest) override;

        void SetDataSource(const std::string &jsonManifest) override;

        void setBitStreamCb(readCB read, seekCB seek, void *arg) override;

        void Prepare() override;

        void SetVolume(float volume) override;

        void Start() override;

        void Pause() override;

        void SeekTo(int64_t pos, bool bAccurate) override;

        void Mute(bool bMute) override;

        void EnterBackGround(bool back) override;

        StreamType SwitchStream(int streamIndex) override;

        int Stop() final;

        void setSpeed(float speed) override;

        void AddCustomHttpHeader(const char *httpHeader) override;

        void RemoveAllCustomHttpHeader() override;

        float getSpeed() override;

        void Interrupt(bool inter);

        std::string GetPropertyString(PropertyKey key, const CicadaJSONItem &param) override;

        int64_t GetPropertyInt(PropertyKey key) override;

        int64_t GetMasterClockPts() override;

        int getCurrentStreamMeta(Stream_meta *meta, StreamType type) override;

        void reLoad() override;

        /**
         * 视频解码器重建兜底：仅在 setOutputSurface 热切换失败（codec 已
         * 失效）时使用，等价于 ExoPlayer 2.9.6 的
         * releaseCodec()+maybeInitCodec() 分支 —— 仅重建视频解码器，
         * 音频与解复用保持运行，新解码器绑定新 surface 从下一关键帧续播。
         * 必须在持有 mCreateMutex 时调用（由 ProcessSetViewMsg 调用）。
         *
         * 实现见 rebuildVideoDecoder(true)：它只接受 dummy/tunnel 渲染器。
         */
        int RestartVideoDecoder();

        /**
         * 上面那条“release + 重新 init 解码器”的实现本体（ExoPlayer
         * MediaCodecVideoRenderer 的 codec 自愈路径）。
         *
         * requireDummyRender=true  —— RestartVideoDecoder() 的语义：只处理
         *                             dummy/tunnel（Android 硬解直出）渲染器。
         * requireDummyRender=false —— 管线真死时的一次性恢复用（探测与调用点都在
         *                             SuperMediaPlayer.cpp 的 doRender() 主循环里）。
         *                             Qt/D3D11 的渲染器不是 dummy，但同样需要这条
         *                             恢复路径；这里不 FlushVideoPath、不跳关键帧。
         *
         * 由主循环线程调用（doRender()），与 setUpVideoPath()/CreateVideoDecoder()
         * 同线程，不引入新的并发访问。
         */
        int rebuildVideoDecoder(bool requireDummyRender);

        /**
         * 暂停状态下 surface 重建后，逐帧精确恢复"暂停的那一帧"：
         * 用解码器记录的最后渲染帧 PTS 设置渲染门（只放行这一帧），
         * 再以该帧位置原地 seek 解码上屏。必须在持有 mCreateMutex 时调用
         * （由 ProcessSetViewMsg 调用）。
         */
        int RestorePausedVideoFrame();

        void SetAutoPlay(bool bAutoPlay) override;

        bool IsAutoPlay() override;

        void SetFilterConfig(const std::string &filterConfig) override;

        void UpdateFilterConfig(const std::string &target, const std::string &options) override;

        void SetFilterInvalid(const std::string &target, bool invalid) override;

        void addExtSubtitle(const char *uri) override;

        int selectExtSubtitle(int index, bool bSelect) override;

        int setStreamDelay(int index, int64_t time) override;

        int invokeComponent(std::string content) override;

        void setDrmRequestCallback(const std::function<DrmResponseData*(const DrmRequestParam& drmRequestParam)>  &drmCallback) override;

        float getCurrentDownloadSpeed() override;

    private:
        void NotifyPosition(int64_t position);

        void NotifyUtcTime();

        void OnTimer(int64_t curTime);

        int updateLoopGap();

        int mainService();

        bool NeedDrop(int64_t pts, int64_t refer);

        void NotifyError(int code);

        void putMsg(PlayMsgType type, const MsgParam &param, bool trigger = true);

        void ProcessVideoLoop();

        void OnDemuxerCallback(const std::string &key, const std::string &value);

        bool DoCheckBufferPass();

        int DecodeVideoPacket(unique_ptr<IAFPacket> &pVideoPacket);

        void LiveCatchUp(int64_t delayTime);

        void LiveTimeSync(int64_t delayTime);

        int FillVideoFrame();
        int FillPendingVideoFrame();
        void DrainPendingVideoFrames();
        int DecodePendingVideoPacket(std::unique_ptr<IAFPacket> &pVideoPacket);
        int CreatePendingVideoDecoder(const Stream_meta &meta);
        bool TryCommitPendingVideoSwitch();

    private:
        int SetUpAudioPath();

        int setUpAudioDecoder(const Stream_meta *meta);

        int SetUpVideoPath();

        void SendVideoFrameToRender(unique_ptr<IAFFrame> frame, bool valid = true);

        // RENDER_NONE: doesn't render
        // RENDER_PARTLY: render part of data
        // RENDER_FULL: render a frame fully
        RENDER_RESULT RenderAudio();

        int DecodeAudio(unique_ptr<IAFPacket> &pPacket);

        int ReadPacket();

        void PostBufferPositionMsg();

        void ChangePlayerStatus(PlayerStatus newStatus);

        void ResetSeekStatus();

        void RenderCallback(StreamType type, bool rendered, IAFFrame::AFFrameInfo &info);

        void Reset();

        void FlushSubtitleInfo();

        void FlushAudioPath();

        /*
         * flushRender=false 只清解码器、不动渲染器。
         *
         * 默认的 true 会走 flushDevice()，而那里面最后会 flushVideoRender()，
         * 也就是 pause/start VSync 线程——afThread 的 pause/start 状态机本身
         * 就有竞态（有机会把线程留在 PAUSED），seek 时用它是对的，但“原地重启
         * 视频”这种不需要动渲染器的路径没必要冒这个险：下一帧本来就会覆盖渲染器
         * 里缓存的旧帧。
         */
        /*
         * 清空视频解码/渲染路径。
         * cancelPendingSwitch=false 用于 seek 等“原地重启视频”的场景：
         * 目标清晰度仍然有效，只重建 pending decoder，不能把切换状态误取消。
         *
         * from：调用者名字（传 __func__）。以前找不到“提交后 0.1 秒是谁把视频路
         * flush 了”，因为 FlushVideoPath() 自己一行日志都不打，而它会顺手把
         * mQualitySwitchCommitPending / mRetiredVideoStreamIndex 清掉 ——
         * 清了以后 READY 永远不会发、旧 Representation 也就永远关不掉。
         * 现在每次调用都留下一行（见 docs/PLAN-QUALITY-SWITCH-FIX.md W0.3）。
         */
        void FlushVideoPath(bool flushRender = true, bool cancelPendingSwitch = true, const char *from = nullptr);

        /*
         * 清晰度切换的**唯一终态出口**。
         *
         * 以前“发 READY + 关旧 stream + 释放 retired decoder + 清切换状态”这四件事
         * 挤在 RenderVideo() 的一个分支里，门槛是“必须有一帧真的上屏”。只要目标
         * 解码器一帧都出不来（HLS 实测就是），这四件事一件都不会做：旧流一直读到
         * PTS 88s、每个包在 ProcessVideoPacket 里打 unknown stream、retired 硬解
         * surface 不还、上层永远收不到终态（界面一直停在“切换中”）。
         *
         * 现在统一从这里出去，ready / 超时 / 被 flush 取消 三条路都走它。
         */
        void finishQualitySwitch(bool ready, const char *reason);

        /*
         * 切换死线兜底：提交后超过 QUALITY_SWITCH_DEADLINE_MS 还没等到“新帧上屏”，
         * 就以 FAILED 收尾。每轮主循环调一次（见 ProcessVideoLoop）。
         */
        void checkQualitySwitchDeadline();

        /*
         * 开始“渲染器追赶窗口”：在窗口内丢掉迟到的帧（并把还没解码的过时包也丢掉），
         * 让画面尽快回到主时钟上。用于清晰度切换提交后、以及 seek 完成之后。
         * 对应 ExoPlayer 的 setJoiningDeadlineMs()。窗口上限见
         * JOINING_DROP_LATE_WINDOW_MS。
         */
        void beginRendererJoining(const char *why);

        /*
         * 清晰度切换状态机的每秒一行诊断（[switch] ...）。
         * 出问题时这一行就能回答：目标流读到哪里了、active 队列头在哪、
         * 帧队列堵了几个、状态机卡在哪个标志上。
         */
        void logQualitySwitchState();

        /*
         * 洪水日志限频（W0.4）。
         *
         * 逐帧/逐包打日志的那几个点（drop pending / hold pending / normalize pending /
         * drop stale / stale pending / unknown stream / read-ahead gate）在一次切换里
         * 能刷出 4000+ 行，把真正重要的
         * 那几行淹掉，也把日志文件撑到几 MB。这里统一限成“每个窗口内最多放行
         * 若干条，其余丢弃并在窗口切换时补一行汇总”。
         */
        enum FloodLogId {
            FLOOD_PENDING_DROP = 0,   /* drop pending frame before switch window */
            FLOOD_PENDING_HOLD,       /* hold pending video before playback position */
            FLOOD_PENDING_NORMALIZE,  /* normalize pending frame pts */
            FLOOD_STALE_DROP,         /* drop stale video packet after quality switch */
            FLOOD_STALE_PENDING,      /* drop stale pending video packet */
            FLOOD_UNKNOWN_STREAM,     /* unknown stream */
            FLOOD_READ_AHEAD,         /* 读前闸门触发的说明行 */
            FLOOD_COUNT
        };

        bool floodLogAllowed(FloodLogId id, int perWindow, const char *what);

        bool SeekInCache(int64_t pos);

        void SwitchVideo(int64_t startTime);

        int64_t getPlayerBufferDuration(bool gotMax, bool internal);

        void ProcessOpenStreamInit(int streamIndex);

        static int64_t getAudioPlayTimeStampCB(void *arg);

        int64_t getAudioPlayTimeStamp();

        bool push(std::unique_ptr<IAFFrame> &frame);

        bool pull(int format, std::unique_ptr<IAFFrame> &frame);

        bool render();

        void RenderSubtitle(int64_t pts);

        bool RenderVideo(bool force_render);

        void releaseStreamInfo(const StreamInfo *info) const;

        // mClearFlag will be set when processing (after remove from mMessageControl), it have gap
        bool isSeeking()
        {
            return INT64_MIN != mSeekPos;
        }//{return mClearFlag || mMessageControl.findMsgByType(MSG_SEEKTO);}

//        void setRotationMode(RotateMode rotateMode, MirrorMode mirrorMode) const;

        bool CreateVideoRender(uint64_t flags);

        int CreateVideoDecoder(bool bHW, Stream_meta &meta);

        int64_t getCurrentPosition();

        void checkEOS();

        bool checkEOSAudio();

        bool checkEOSVideo();

        void playCompleted();

        void notifySeekEndCallback();

        void notifyPreparedCallback();

        void updateVideoMeta();

        void doDeCode();

        void doRender();

        /*
         * 切流（清晰度切换）后判断正在用的视频解码器还能不能解新码流
         * （codec id 是否相同、分辨率是否相同）。实现与理由见 .cpp。
         */
        bool isVideoDecoderMetaMatched(const Stream_meta *newMeta) const;

        void doReadPacket();

        int setUpAudioRender(const IAFFrame::audioInfo &info);

        std::atomic<int64_t> mCurrentPos{};
        std::atomic<int64_t> mCurrentFrameUtcTime{};

        void printTimePosition(int64_t time) const;

        void setUpAVPath();

        void startRendering(bool start);

        void sendDCAMessage();

        void ProcessUpdateView();

        static bool isHDRVideo(const Stream_meta *meta);

        static bool isWideVineVideo(const Stream_meta *meta);

        void closeAudio();

        void closeVideo();

        void checkFirstRender();

        int setUpVideoRender(uint64_t renderFlags);

        void updateBufferInfo(bool force);

        class ApsaraAudioRenderCallback : public IAudioRenderListener {
        public:
            explicit ApsaraAudioRenderCallback(SuperMediaPlayer &player) : mPlayer(player)
            {}

            void onEOS() override
            {}
            void onFrameInfoUpdate(IAFFrame::AFFrameInfo &info, bool rendered) override;

        private:
            SuperMediaPlayer &mPlayer;
        };

        class ApsaraVideoRenderListener : public IVideoRender::IVideoRenderListener {

        public:
            explicit ApsaraVideoRenderListener(SuperMediaPlayer &player) : mPlayer(player)
            {}
            void onFrameInfoUpdate(IAFFrame::AFFrameInfo &info, bool rendered) override;

        private:
            SuperMediaPlayer &mPlayer;
        };

        class ApsaraVideoProcessTextureCallback : public IVideoRender::videoProcessTextureCb {
        public:
            explicit ApsaraVideoProcessTextureCallback(SuperMediaPlayer &player) : mPlayer(player)
            {}

            bool init(int type) override;

            bool needProcess() override;

            bool push(std::unique_ptr<IAFFrame> &textureFrame) override;

            bool pull(std::unique_ptr<IAFFrame> &textureFrame) override;

        private:
            SuperMediaPlayer &mPlayer;
        };

    private:
        static IVideoRender::Scale convertScaleMode(ScaleMode mode);

        static IVideoRender::Rotate convertRotateMode(RotateMode mode);

        static IVideoRender::Flip convertMirrorMode(MirrorMode mode);


    public:
        static bool is_supported(const options *opts)
        {
            return true;
        }

    private:
        explicit SuperMediaPlayer(int dummy)
        {
            mIsDummy = true;
            addPrototype(this);
        }
        ICicadaPlayer *clone() override
        {
            return new SuperMediaPlayer();
        };

        static SuperMediaPlayer se;


    private:
        IDataSource *mDataSource{nullptr};
        std::atomic_bool mCanceled{false};
        std::atomic_bool mMainServiceCanceled{true};
        std::atomic_bool mVideoRenderInited{false};
        std::unique_ptr<demuxer_service> mDemuxerService{nullptr};
        std::queue<unique_ptr<IAFFrame>> mVideoFrameQue{};
        std::queue<unique_ptr<IAFFrame>> mPendingVideoFrameQue{};
        /* 目标 representation 的 packet 独立缓存，避免与 active 路混队列。 */
        std::deque<unique_ptr<IAFPacket>> mPendingVideoPacketQue{};
        /* 目标码流与当前主时钟存在容器时间轴偏移时，在 pending 帧提交前
         * 统一重定位 PTS，避免 HLS 不同 rendition 以绝对时间戳输出而导致
         * 提交条件永远等不到（例如目标首帧 133s、当前播放 82s）。 */
        int64_t mPendingVideoPtsOffset{INT64_MIN};
        /*
         * 本次清晰度切换要求目标 representation 从哪个媒体时间点接入。
         * 双路播放器不会让备用解码器从 representation 的 0 点一路解码到当前
         * 播放点，而是只保留用于初始化 codec 的首个关键包，然后直接等待目标
         * 时间点对应的关键帧。这样可以避免切换时把几十秒的旧帧送进 pending
         * decoder，造成“wait a key frame”、大量丢包和主循环卡死。
         */
        int64_t mPendingVideoSwitchTimePosition{INT64_MIN};
        int64_t mPendingVideoSwitchStartMs{0};
        bool mPendingVideoInitPacketSent{false};
        /* pending 提升为 active 后继续作用于该清晰度 decoder 的所有输出帧，
         * 防止只有首帧被对齐、后续帧又回到 rendition 原始时间轴。 */
        int64_t mActiveVideoPtsOffset{INT64_MIN};
        // decoder 槽位已经切换，但还未确认新帧完成一次实际渲染；在此期间不发 READY，
        // 也不释放 retired decoder，保证上层清晰度与画面同步。
        bool mQualitySwitchCommitPending{false};
        int mQualitySwitchCommittedStreamIndex{-1};
        /* promote 时 active 队列中尚未输出的旧帧数量。只有这些旧帧全部
         * 消费后，RenderVideo() 才能把当前帧认定为新清晰度并发送 READY。 */
        size_t mQualitySwitchOldFramesPending{0};
        std::deque<unique_ptr<IAFFrame>> mAudioFrameQue{};
        unique_ptr<streamMeta> mCurrentVideoMeta{};
        bool videoDecoderEOS = false;
        bool audioDecoderEOS = false;
        picture_cache_type mPictureCacheType = picture_cache_type_cannot;
        bool videoDecoderFull = false;
        std::unique_ptr<SMPMessageControllerListener> mMsgCtrlListener{nullptr};
        std::unique_ptr<PlayerMessageControl> mMessageControl{nullptr};
        std::unique_ptr<ApsaraAudioRenderCallback> mAudioRenderCB{nullptr};
        std::unique_ptr<ApsaraVideoRenderListener> mVideoRenderListener{nullptr};
        std::unique_ptr<ApsaraVideoProcessTextureCallback> mVideoProcessCb{nullptr};
        std::unique_ptr<BufferController> mBufferController{nullptr};
        std::mutex mAppStatusMutex;
        std::atomic<APP_STATUS> mAppStatus{APP_FOREGROUND};
        int mVideoWidth{0};
        int mVideoHeight{0};
        int mVideoRotation{0};
        int64_t mDuration{INT64_MIN};
        int64_t mBufferPosition{0};
        PlayerStatus mOldPlayStatus{PLAYER_IDLE};
        atomic <PlayerStatus> mPlayStatus{PLAYER_IDLE};
        std::deque<std::unique_ptr<IAFPacket>> mSubtitleShowedQueue;
        MediaInfo mMediaInfo{};
        //        ResolutionPolicy mResolutionPolicy{kShowAll};
        int mCurrentVideoIndex{-1};
        int mCurrentAudioIndex{-1};
        int mCurrentSubtitleIndex{-1};
        int mWillChangedVideoStreamIndex{-1};
        int mPendingVideoStreamIndex{-1};
        /* 切换提交前仍在输出旧画面的 stream。目标真正上屏后才关闭它。 */
        int mRetiredVideoStreamIndex{-1};
        int mWillChangedAudioStreamIndex{-1};
        int mWillChangedSubtitleStreamIndex{-1};
        float mCATimeBase{};      // current audio stream origin pts time base
        float mWATimeBase{};      // willChange audio stream origin pts time base
        int mRemainLiveSegment{0};// To avoid access demuxer multi-thread
        bool mInited{false};
        atomic_bool mSeekNeedCatch{false};
        const static int64_t SEEK_ACCURATE_MAX;
        atomic <int64_t> mSeekPos{INT64_MIN};
        // 标记当前 seek 是否为"暂停帧恢复"专用（用户 seek 要关闭渲染门）
        atomic_bool mRestoringPausedFrame{false};
        SystemReferClock mMasterClock;
        streamTime mAudioTime{INT64_MIN, 0};
        int64_t mPlayedVideoPts{INT64_MIN}; // sync pts
        bool mVideoPtsRevert{false};
        bool mAudioPtsRevert{false};
        bool mHaveVideoPkt{false};
        bool mHaveAudioPkt{false};
        int64_t mPlayedAudioPts{INT64_MIN};
        // seek/重建音频路径后，第一张实际送入音频设备的帧作为新的主时钟锚点。
        bool mAudioClockReanchorPending{false};
        int64_t mFirstVideoPts{INT64_MIN};
        int64_t mCurVideoPts{INT64_MIN};  // update from render cb
        int64_t mFirstAudioPts{INT64_MIN};
        int64_t mMediaStartPts{INT64_MIN}; // the first small frame pts in the media stream
        int64_t mVideoChangedFirstPts{INT64_MIN};
        int64_t mAudioChangedFirstPts{INT64_MIN};
        int64_t mSubtitleChangedFirstPts{INT64_MIN};
        int64_t mFirstReadPacketSucMS{0};
        int mMainStreamId{-1};
        int64_t mRemovedFirstAudioPts{INT64_MIN};;
        int64_t mFirstSeekStartTime{0};
        bool mEof{false};
        bool mSubtitleEOS{false};
        bool mLowMem{false};
        bool mSeekFlag{false};
        bool mSeekInCache{false};
        bool mFirstBufferFlag{true}; // first play and after seek play
        bool mBufferingFlag{false};
        bool mMixMode{false};
        bool mAdaptiveVideo{false};
        bool mFirstRendered{false};
        int mWriteAudioLen{0};
        int64_t mLastAudioFrameDuration{INT64_MIN};
        int64_t mTimeoutStartTime{INT64_MIN};
        int64_t mSubtitleShowIndex{0};
        bool mBufferIsFull{false};
        bool mWillSwitchVideo{false};

        std::unique_ptr<player_type_set> mSet{};
        int64_t mSoughtVideoPos{INT64_MIN};
        int mTimerInterval = 0;
        int64_t mTimerLatestTime = 0;
        std::mutex mCreateMutex{}; // need lock if access pointer outside of loop thread
        std::mutex mPlayerMutex{};
        std::mutex mSleepMutex{};
        std::condition_variable mPlayerCondition;
        PlayerNotifier *mPNotifier = nullptr;
        std::unique_ptr<afThread> mApsaraThread{};
        int mLoadingProcess{0};
        int64_t mPrepareStartTime = 0;
        int mVideoParserTimes = 0;
        InterlacedType mVideoInterlaced = InterlacedType_UNKNOWN;
        bitStreamParser *mVideoParser = nullptr;
        int64_t mPtsDiscontinueDelta{INT64_MIN};
        std::unique_ptr<MediaPlayerUtil> mUtil{};
        std::unique_ptr<MediaPlayerAnalyticsUtil> mMPAUtil{};
        std::unique_ptr<SuperMediaPlayerDataSourceListener> mSourceListener{nullptr};
        std::unique_ptr<SMP_DCAManager> mDcaManager{nullptr};
        std::unique_ptr<SMPAVDeviceManager> mAVDeviceManager{nullptr};
        std::unique_ptr<IAFPacket> mVideoPacket{};
        std::unique_ptr<IAFPacket> mAudioPacket{};
        std::unique_ptr<mediaPlayerSubTitleListener> mSubListener;
        std::unique_ptr<subTitlePlayer> mSubPlayer;
#ifdef ENABLE_VIDEO_FILTER
        std::mutex mFilterManagerMutex{};
        std::unique_ptr<FilterManager> mFilterManager;
#endif
        bool dropLateVideoFrames = false;
        bool waitingForStart = false;
        bool mBRendingStart {false};
        bool mSecretPlayBack{false};
        bool mDrmKeyValid{false};
        std::unique_ptr<SMPRecorderSet> mRecorderSet{nullptr};
        bool mAutoPlay = false;
        int64_t mCheckAudioQueEOSTime{INT64_MIN};
        uint64_t mAudioQueDuration{UINT64_MAX};
        onRenderFrame mFrameCb{nullptr};
        void *mFrameCbUserData{nullptr};
        UpdateViewCB mUpdateViewCB{nullptr};
        void *mUpdateViewCBUserData{nullptr};
        onRenderFrame mAudioRenderingCb{nullptr};
        void *mAudioRenderingCbUserData{nullptr};

        videoRenderingFrameCB mVideoRenderingCb{nullptr};
        void *mVideoRenderingCbUserData{nullptr};

        UrlHashCB mUrlHashCb{nullptr};
        void *mUrlHashCbUserData{nullptr};

        bool mIsDummy{false};
        readCB mBSReadCb = nullptr;
        seekCB mBSSeekCb = nullptr;
        void *mBSCbArg = nullptr;
        int64_t mSuggestedPresentationDelay = 0;
        LiveTimeSyncType mLiveTimeSyncType = LiveTimeSyncType::LiveTimeSyncNormal;
        /*
         * “视频路刚丢过帧、正在追赶”的标志。现在**唯一**的用途是让 mainService()
         * 把本轮等待时间压到最多 2ms（读取点：SuperMediaPlayer.cpp 的
         * `(mVideoCatchingUp || mSeekFlag)` 那一行），好尽快再跑一轮；丢帧时置真、
         * 有一帧上屏时清假（都在 RenderVideo() 里）。seek 期间另有 mSeekFlag 覆盖，
         * 两者取或。它不再参与任何“跳关键帧/flush”类的动作。
         */
        bool mVideoCatchingUp{false};
        bool mAudioEOS{false};
        bool mVideoEOS{false};
        int64_t mVideoDelayTime{0};

        /*
         * 【管线真死时的一次性恢复】（取代原来那个周期性视频路巡检）
         *
         * mLastVideoFrameRenderedMs —— “画面还在动”的节拍基准（单调毫秒）。
         *        真正记下“一帧上了屏”的只有 RenderVideo() 成功送帧那一刻；另外两处
         *        **不是上屏**，而是把这个基准重置（用来把下一次探测/判定的窗口往后推，
         *        并且各自在代码里写明了理由）：ResetSeekStatus() 之后、以及恢复动作
         *        触发之后。它有两个用途：
         *        (1) 管线恢复的探测：“这么久没有一帧上屏”才值得怀疑（正常播放是
         *            VIDEO_RECOVER_STALL_MS；seek 在途时放宽到 VIDEO_RECOVER_STUCK_SEEK_MS，
         *            否则一个死掉的 seek 永远等不到救援 —— 见 .cpp 的 doRender() E 段）；
         *        (2) 清晰度切换死线兜底的“还在进展”判据（见 checkQualitySwitchDeadline）。
         * mVideoRecoverCooldownMs  —— 恢复动作的冷却截止时间（单调毫秒，0 = 还没
         *        触发过）。冷却期内绝不再重建解码器，避免把一次性恢复变成周期性动作。
         * mRecoverSampleMs         —— 上一次采样视频包队列长度的时间；0 = 当前没有采样。
         *        队列长度用 mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO) 取。
         * mRecoverSamplePackets    —— 上一次采样的队列长度，用来判断“队列还在不在
         *        减少”；-1 = 还没有有效样本。注意触发还要求“队列里有包可解”
         *        （packets > 0）—— 空队列是读取/网络问题，不属于本探头的职责，
         *        见 SuperMediaPlayer.cpp 的探测段。
         *
         * 实现说明：这四个成员是**原位**替换原来那四个 int64_t 时间戳的
         * （总大小仍是 32 字节，8 字节对齐），所以就**这一处替换本身**而言，
         * 这一段之后的成员偏移不变 —— 本工程增量构建不记录头文件依赖，friend
         * 监听类会按偏移直接访问成员，偏移一变就可能拿错指针（见下面“必须留在
         * 最末尾”那段；本次另外删除的两个成员确实移动了偏移，那段里写清了）。
         */
        int64_t mLastVideoFrameRenderedMs{0};
        int64_t mVideoRecoverCooldownMs{0};
        int64_t mRecoverSampleMs{0};
        int64_t mRecoverSamplePackets{-1};

        bool mCalculateSpeedUsePacket{true};
        std::unique_ptr<CicadaJSONArray> mFilterConfig;
        UTCTimer *mUtcTimer{nullptr};
        bool mOpenAudioDeviceFailed{false};

        std::string mContainerInfo{};

        /*
         * ================= 以下这些成员必须留在成员列表的最末尾 =================
         *
         * 它们属于切清晰度/seek 收尾逻辑（doDeCode()、RenderVideo()、finishQualitySwitch()），
         * 看起来应该跟 mWillSwitchVideo 那几个成员放一起，但**只能**
         * 追加在最后，不要再插到类中间去（上一轮就是插在 mSet 前面，导致
         * 2026-09-20 16:34 打开 DASH 链接时 0xC0000005）。
         *
         * 原因：SuperMediaPlayer 的私有成员会被别的翻译单元按“偏移”直接访问
         * （friend：SuperMediaPlayerDataSourceListener / SMPMessageControllerListener /
         * SMP_DCAManager）。最典型的是 mSet：listener 在 SuperMediaPlayer.cpp:92 被
         * 构造，构造函数里立刻调 enableRetry_l()，那里就解引用 mPlayer.mSet
         * （SuperMediaPlayerDataSourceListener.cpp:67 → mSet->timeout_ms）。
         *
         * 只要在 mSet 之前插入成员，mSet 以及它之后所有成员的偏移都会后移。此时
         * 如果有任何一个 .obj 是照旧偏移编译的（本工程增量构建没有记录头文件依赖：
         * platform/QtPlayer/build/.../media_player.dir/ 下的 *.obj.d 是空文件，改 .h 不会
         * 触发依赖它的 TU 重编 —— 崩的时候 SuperMediaPlayerDataSourceListener.cpp.obj
         * 就比 SuperMediaPlayer.h 旧），那个 TU 就会把偏移位置上的**别的成员**当成
         * mSet 指针去解引用。上次插在 mSet 前面时，mSet 的旧偏移正好落在构造期还是
         * 0 的一个 int64_t 成员上，于是拿 0 当指针读 timeout_ms → 立刻
         * 0xC0000005。
         *
         * 按上面的规则**追加**在最后不改变任何既有成员的偏移，新旧 .obj 都能对上。
         * （注意：这条只对“追加”成立；**删除或收窄**成员一定会移动其后成员的偏移，
         * 见下面 2026-09-23 那段。）
         *
         * ----------------------------------------------------------------------
         * 【2026-09-23 删除周期性视频路巡检相关成员：本次 .h 改动会移动偏移，务必全量重编】
         *
         * 这次删掉了两个成员（都原来只服务于周期性巡检/丢包追赶自检：本块之前的一个
         * 计数器 int、以及后面一个限频用的 int64_t），并收窄了 FloodLogId
         * （去掉一个枚举值 → mFloodLog[] 少一格），所以**这些位置之后的所有成员
         * 偏移都变了**。上面那条“增量构建不重编依赖 TU”的坑因此同样适用：如果
         * platform/QtPlayer 下还有照旧偏移编译的 *.obj（SuperMediaPlayer *Listener /
         * *DataSourceListener / SMP_DCAManager 等 friend TU），它们会按旧偏移访问
         * 新布局 → 随机 0xC0000005。
         *
         * 所以本次改完**必须**让内核全量重编（删掉
         * platform/QtPlayer/build 下各 media_player.dir 目录里的 .obj，或整个 build 目录），
         * 不能只做增量构建。
         * ======================================================================
         */

        /*
         * 本次清晰度切换需要重建视频解码器（新码流的 codec id 或分辨率跟当前
         * 解码器不一致），但要等旧码流的缓冲放完、新码流的包真正到点时才换，
         * 见 doDeCode() 里的注释。
         */
        bool mPendingVideoDecoderSwitch{false};

        /*
         * ============ 清晰度切换收尾 / 诊断（2026-09-21，见 PLAN-QUALITY-SWITCH-FIX）============
         *
         * mQualitySwitchCommitMs   推进到提交那一刻的单调毫秒；0 表示没有在途切换。
         * mQualitySwitchDeadlineMs 提交 + QUALITY_SWITCH_DEADLINE_MS；只要还有“进展”
         *                          （追赶窗口开着，或最近一个死线时长内有帧真的上屏）
         *                          就不断往后顺延。顺延不了、到点仍未等到“新帧真的上屏”，
         *                          就走 finishQualitySwitch(false, ...) 收尾 —— 必须有
         *                          这个兜底，否则“提交了但一帧都出不来”会把状态机永久
         *                          钉在 mQualitySwitchCommitPending=true 上，连带旧流不关、
         *                          retired 解码器不还、上层收不到终态。判据见
         *                          SuperMediaPlayer.cpp 的 checkQualitySwitchDeadline()。
         * mQualitySwitchWarnMs     上面那次超时告警的限频时间戳。
         * mSwitchStateLogMs        [switch] 每秒一行的限频时间戳。
         * mFloodLog[]              FloodLogId 里每一项的窗口计数（W0.4）。
         */
        int64_t mQualitySwitchCommitMs{0};
        int64_t mQualitySwitchDeadlineMs{0};
        int64_t mQualitySwitchWarnMs{0};
        int64_t mSwitchStateLogMs{0};

        struct FloodLogState {
            int64_t windowStartMs{0};
            int64_t emitted{0};
            int64_t dropped{0};
            const char *what{nullptr};
        };

        FloodLogState mFloodLog[FLOOD_COUNT];

        /*
         * 渲染器追赶窗口的截止时间（单调毫秒，0 = 没有在追赶）。见 beginRendererJoining()。
         * 到点后即使还没追上也要停掉丢帧，避免“视频本来就慢”时一直丢到空。
         */
        int64_t mDropLateVideoFramesUntilMs{0};

        /*
         * ============ seek 目标“地板”（2026-09-21，用户反馈“进度条跳回去再跳回来”）============
         *
         * 精确 seek 的重启点在目标点**之前**最近的关键帧上：HLS 实测 seek 到 26.708s，
         * tracker 落在 20.854s（一个分片之前），音频却按目标点重新锚定。于是 seek
         * 刚结束时：
         *   * 视频解码位置 / mCurrentPos 还停在 20.8s 附近；
         *   * 界面收到的下一个 PositionUpdate 把进度条从 26.7 拽回 20.8，
         *     随后画面追上来了再往前走 —— 用户看到的就是“跳过去→跳回来→再跳过去”。
         *
         * 这个地板就是那个“不许往回跳”的下界：seek 一发起就记下目标点，
         * getCurrentPosition() 在管道真正走到它之前一直返回它；某帧真正到/过了
         * 这个位置（RenderVideo 判定）就清掉，之后一切照常。
         *
         * ExoPlayer 的等价语义是 seek 之后的 `positionUs`：它是目标点，目标点之前
         * 的输出缓冲一律 `FRAME_RELEASE_SKIP`（解码但不显示），位置从不回退。
         */
        int64_t mSeekPositionFloorUs{INT64_MIN};

        /*
         * 目标路"预滚"是否已经结束（见 PENDING_PREROLL_KEEP_US）。
         * false = 还在丢弃切换窗口之前的数据（只保留遇到的关键帧）；
         * 一旦遇到"切换窗口之前最近的关键帧"或者数据已经进入切换窗口，就置 true。
         * 每次建立新的 pending 切换都要复位。
         */
        bool mPendingVideoPrerollDone{false};

        /*
         * 预滚参考点（微秒，INT64_MIN = 尚未确定）。进入预滚那一刻取
         * max(切换请求时刻, 主时钟)，之后**不再变化**：只有稳定的参考点，
         * "参考点之后的第一个关键帧" 才存在；如果跟着主时钟一直往前推，
         * 刚到达的关键帧会立刻被判成"还不够靠后"，下一个又要等一整个分片。
         */
        int64_t mPendingVideoPrerollRefUs{INT64_MIN};

        /*
         * 本次 mBufferingFlag 期间是否真的给 UI 发过 LoadingStart。
         * seek 引起的"缓冲"（缓存被 ClearPacket 清空、新 segment 还在下载）
         * 不再弹"缓冲中"提示 —— 界面已经有 NotifySeeking/NotifySeekEnd，
         * 重复提示就是用户看到的"seek 为什么要缓冲"。但内部状态（暂停主时钟、
         * 暂停音频渲染）照旧，退出缓冲时也必须保持 start/end 配对，
         * 所以这里记一下有没有通知过，没通知过就不要再发 end。
         */
        bool mBufferingNotified{false};

        /*
         * 清晰度切换刚提交后，还要再挡掉几帧才交给渲染器（见 SuperMediaPlayer.cpp 里
         * 提交处那段说明）。防的是新解码器刚接手时的一瞬间马赛克/花屏：
         * 解码照常进行，只是这几帧不上屏，画面保持上一帧（约 3 帧 ≈ 50ms）。
         */
        int mQualitySwitchHoldFrames{0};

        /*
         * ============ 解码器故障恢复（2026-09-24，错误驱动，跨平台）============
         *
         * 主流播放器（ExoPlayer MediaCodecRenderer）在 codec 报错后是
         * releaseCodec() + maybeInitCodecOrBypass()，失败按 enableDecoderFallback
         * 回退到低优先级解码器，回退链走完才把异常抛给上层；**没有**基于超时的重启。
         *
         * 本内核对应：DecodeVideoPacket() 拿到 STATUS_HAVE_ERROR 且
         * get_error_frame_no() 超过 MAX_DECODE_ERROR_FRAME（说明连续这么多错误帧、
         * 一帧都没出过）时，先 rebuildVideoDecoder()（invalidateDecoder +
         * CreateVideoDecoder，硬解失败落软解），最多 MAX_VIDEO_DECODER_REBUILDS 次；
         * 每次都建不起来才 ChangePlayerStatus(PLAYER_ERROR) + NotifyError。
         *
         * mVideoDecodeRebuildCount 就是已尝试的次数。只要成功出过一帧
         * （FillVideoFrame() 里紧挨 clean_error()）就清零，等价于“codec 恢复正常”。
         * DRM 错误不计入（重装解码器解决不了密钥/DRM 会话问题）。
         *
         * 【必须追加在成员列表末尾】新成员一律追加在最后，不要插进中间：本工程
         * 增量构建不记录头文件依赖，插在中间会移动其后成员偏移，让旧的 friend TU
         * .obj 按错偏移访问（见前面“以下这些成员必须留在成员列表的最末尾”那段）。
         */
        int mVideoDecodeRebuildCount{0};
    };
}// namespace Cicada
#endif// CICADA_PLAYER_SERVICE_H
