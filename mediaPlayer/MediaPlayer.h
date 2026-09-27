//
//  MediaPlayer.h
//  apsara_player
//
//  Created by shiping.csp on 2018/11/12.
//

#ifndef CicadaPlayer_h
#define CicadaPlayer_h

#include "MediaPlayerConfig.h"
#include "abr/AbrBufferRefererData.h"
#include "native_cicada_player_def.h"
#include <cstdio>

class AbrManager;

class AbrAlgoStrategy;

#include <cacheModule/CacheManager.h>
#include <demuxer/manifest/MediaManifest.h>
#include <drm/DrmHandler.h>
#include <mutex>
#include <render/video/IVideoRender.h>
#include <sstream>
#include <string>
#include <utils/AFMediaType.h>
#include <vector>

#ifdef ANDROID

namespace std {
    template<typename T>
    std::string to_string(T value)
    {
        std::ostringstream os;
        os << value;
        return os.str();
    }
}// namespace std
#endif

namespace Cicada {
    class AnalyticsQueryListener;

    class IAnalyticsCollector;

    class IAnalyticsCollectorFactory;

    typedef void (*CicadaLogCallback)(void *userData, int prio, const char *buf);


    class MediaPlayer {
    public:
        explicit MediaPlayer(const char *opt = nullptr);

        explicit MediaPlayer(IAnalyticsCollectorFactory &factory, const char *opt = nullptr);

        ~MediaPlayer();

        static string GetSdkVersion()
        {
            return "paas 0.9";//TODO version
        }

        std::string getName();

    public:

        void SetAnalyticsCollector(IAnalyticsCollector * collector);

        void EnableVideoRenderedCallback(bool enable);

        void SetOnRenderFrameCallback(onRenderFrame cb, void *userData);

        void SetAudioRenderingCallback(onRenderFrame cb, void *userData);

        void SetVideoRenderingCallback(videoRenderingFrameCB cb, void *userData);

        void SetUpdateViewCallback(UpdateViewCB cb, void *userData);

        void SetUrlHashCallback(UrlHashCB cb, void *userData);

        void SetStreamTypeFlags(uint64_t flags);

        void SetIPResolveType(IpResolveType type);

        void SetFastStart(bool mode);

        /*
         * set player listener
         */
        void SetListener(const playerListener &Listener);

        /*
         * enable hardware decoder or not
         */
        void EnableHardwareDecoder(bool bEnabled);

        /*
         * set player view
         */
        void SetView(void *view);

        void ClearScreen();

        /*
         * set player url source
         */
        void SetDataSource(const char *url);

        /*
         * 对象模式播放：传入统一的 MediaManifest 清单对象
         * （对应 hili-player 的 MediaManifest JSON 结构），无需 m3u8/mpd 文本。
         * 支持 AES-128 分段加密与 DRM（Widevine/FairPlay/ClearKey）。
         */
        void SetDataSource(const Manifest::MediaManifest &manifest);

        /*
         * 对象模式播放：传入 MediaManifest 结构的 JSON 文本。
         */
        void SetDataSource(const std::string &jsonManifest);

        /*
        * when play url not equals SetDataSource , will callback.
        */
        void SetDataSourceChangedCallback(function<void(const string &)> urlChangedCallbak);

        void setDrmRequestCallback(const std::function<DrmResponseData*(const DrmRequestParam& drmRequestParam)> & drmCallback);

        /*
         *select specific track info
         */
        void SelectTrack(int index);

        /*
         * prepare the url
         */
        void Prepare();

        /*
         * start to play
         */
        void Start();

        /*
         * Pause the player
         */
        void Pause();

        /*
         * set the volume
         */
        void SetVolume(float volume);

        /**
         * get current volume of the player
         */
        float GetVolume();

        /*
         * seekt to new Postion by seekmode
         */
        void SeekTo(int64_t seekPos, SeekMode mode);

        /*
         * capture screen, buffer will get by callback
         */
        void CaptureScreen();

        /*
         * stop playing
         */
        void Stop();

        /*
         * get the total duration of playing url
         */
        int64_t GetDuration();

        /*
        * get master clock pts
        */
        int64_t GetMasterClockPts();//TODO ??这是什么？


        void SetClockRefer(clockRefer cb, void *arg);

        /*
         * get current stream index
         */
        int GetCurrentStreamIndex(StreamType type);//TODO ??这是什么？

        /*
         * get current stream info
         */
        StreamInfo *GetCurrentStreamInfo(StreamType type);

        /*
         * 【当前视频解码器实际在用硬解还是软解】
         *
         * 事实读数（活动解码器实例眼下走的那条路，含解码途中退回软解），不是
         * "配置想用哪种"、也不是"设备支持不支持"。没有视频解码器时是 false。
         * 界面上的"解码方式"显示用它，见 ICicadaPlayer::IsVideoDecoderHardware()。
         */
        bool IsVideoDecoderHardware();

        /*
         * get the current playing position of the player
         */
        int64_t GetCurrentPosition();

        /*
         * get the buffered postion of the player
         */
        int64_t GetBufferedPosition();

        /*
         * enter background if back is true, or enter foreground
         *  this function is only used for ios?
         */
        void EnterBackGround(bool back);

        /*
         * set the player mute
         */
        void SetMute(bool bMute);

        /**
         * return ismuted or not
         */
        bool IsMuted();

        /**
         * set player config
         */
        void SetConfig(const MediaPlayerConfig *config);

        /**
         * get playerg config
         */
        const MediaPlayerConfig *GetConfig();

        /*
         * set Scale Mode
         */
        void SetScaleMode(ScaleMode mode);

        /*
         * Get Scale Mode
         */
        ScaleMode GetScaleMode();

        /*
         * set loop
         */
        void SetLoop(bool bLoop);

        /*
         * get loop or not
         */
        bool IsLoop();

        /*
         * get video width of current playing
         */
        int GetVideoWidth();

        /*
         * get video height of current playing
         */
        int GetVideoHeight();

        /*
         * get video rotation angle from video metadata
         */
        int GetVideoRotation();

        /*
         * set rotate mode, refer to RotateMode
         */
        void SetRotateMode(RotateMode mode);

        /*
         * get rotate mode
         */
        RotateMode GetRotateMode();

        /*
         * set mirror mode, refer to MirrorMode
         */
        void SetMirrorMode(MirrorMode mode);

        /*
         * get mirror mode
         */
        MirrorMode GetMirrorMode();

        /*
         * 【色觉辅助滤镜】设置 3x3 颜色矩阵（行主序，9 个 float）。
         * 与 SetMirrorMode 同一层转发语义：转发到 native handle 的 SetColorMatrix。
         *
         * 【回退点 C2】删掉本声明 + MediaPlayer.cpp 里的实现（2 处，可独立回退）。
         */
        void SetColorMatrix(const float matrix[9]);

        /*
         * 【应用层"设备硬解能力 + 编码偏好"：传入 / 取出】
         * 与 SetColorMatrix 同一层转发语义：转发到 native handle 的新接口。
         *
         * 为什么在包装类上再包一层：平台层（Android JNI 与 Qt）只持有 MediaPlayer、
         * 拿不到 playerHandle；而返回字符串的 C API 要求调用方用 CicadaFreeString 释放。
         * 包成 std::string 后平台侧不必关心所有权，也不会漏释放。
         *
         * GetVideoCodecSupport()：返回 JSON（字段契约见 CicadaGetVideoCodecSupport）。
         *   无播放器或失败时返回空串。里面的 "preferred" 就是当前生效的手动指定编码
         *   （空串 = 不指定 = 全自动）。
         * SetVideoCodecSupport(json)：0 = 成功；非 0 = 参数非法（内核状态不变，不半套用）；
         *   json 为空指针或空串 = 清除应用层覆盖，恢复内核自行探测。
         *   同一个 JSON 里的 "preferred" 是应用侧"设置默认视频格式"的入口：指定之后
         *   **下一次**等级选择（起播默认档 / ABR 下一次决策 / ABR 触发的切档）优先用该
         *   编码，该分辨率下没有它的流或内核解不了它时回退到自动规则；设置本身不会
         *   立即触发一次切换（详见 CicadaSetVideoCodecSupport 的契约注释）。
         */
        std::string GetVideoCodecSupport();

        int SetVideoCodecSupport(const char *json);

        /*
         * set clear color
         */
        void SetVideoBackgroundColor(uint32_t color);

        /*
         * set speed of current playing, 0.5-2.0
         */
        void SetSpeed(float speed);

        /*
         * get the speed of current playing
         */
        float GetSpeed();

        /*
         * set traceID for debug log tracing
         */
        void SetTraceID(const char *traceID);

        /**
   * set auto play
   */
        void SetAutoPlay(bool bAutoPlay);

        /**
         * get is auto play or not
         */
        bool IsAutoPlay();

        void Reload();

        /*
         * set Component callback by type
         */
        void SetComponentCb(player_component_type type, void *factory);

        void setBitStreamCb(readCB read, seekCB seek, void *arg);

        void SetMediaFrameCb(playerMediaFrameCb func, void *arg);

        int GetCurrentStreamMeta(Stream_meta *meta, StreamType type);

        void AddExtSubtitle(const char *uri);

        void SelectExtSubtitle(int index, bool select);

        int SetStreamDelayTime(int index, int64_t time);

        void setErrorConverter(ErrorConverter *converter);

        std::string GetPropertyString(PropertyKey key);

        std::string GetPropertyString(PropertyKey key, const CicadaJSONItem &param);

        void SetOption(const char *key, const char *value);

        void GetOption(const char *key, char *value);

        IAnalyticsCollector *GetAnalyticsCollector()
        {
            return mCollector;
        }
        //
        //        long ApsaraGetPropertyLong(playerHandle *player, int key);

        //        void SetMediaFrameCb(plyerMediaFrameCb func, void* arg);

        /**
         * set cache config.
         * @param config
         */
        void SetCacheConfig(const CacheConfig &config);

        /**
         * Set filter config. call this before prepare. If want update filter config, call updateFilterConfig()
         * @param filterConfig
         */
        void SetFilterConfig(const std::string &filterConfig);

        /**
         * upadate filter config.
         * @param target  Empty or not found, will make no effect. Should not be empty.
         * @param options
         */
        void UpdateFilterConfig(const std::string &target, const std::string &options);

        /**
         * disable/enable filter.
         * @param target  if empty , disable all filters.
         * @param invalid  true: enable(default); false: disable
         */
        void SetFilterInvalid(const std::string &target, bool invalid);

        /**
         * get cache url. Should SetCacheConfig before call this.
         * @param URL orignal url
         * @return cached url.
         */
        string GetCachePathByURL(const string &URL);

        void SetDefaultBandWidth(int bandWidth);

        int InvokeComponent(const char *content);

        string GetPlayerSessionId();

        bool IsEnableAbr();

    private:
        static void preparedCallback(void *userData);

        static void completionCallback(void *userData);

        static void firstFrameCallback(void *userData);

        static void errorFrameCallback(int64_t errorCode, const void *errorMsg, void *userData);

        static void eventCallback(int64_t code, const void *msg, void *userData);

        static void videoSizeChangedCallback(int64_t width, int64_t height, void *userData);

        static void videoRenderedCallback(int64_t timeMs, int64_t pts, void *userData);

        static void audioRenderedCallback(int64_t timeMs, int64_t pts, void *userData);

        static void currentPositionCallback(int64_t position, void *userData);

        static void currentUtcTimeCallback(int64_t time, void *userData);

        static void bufferPositionCallback(int64_t position, void *userData);

        static void loopingStartCallback(void *userData);

        static void loadingStartCallback(void *userData);

        static void loadingEndCallback(void *userData);

        static void loadingProgressCallback(int64_t prg, void *userData);

        static void currentDownLoadSpeed(int64_t speed, void *userData);

        static void subtitleShowCallback(int64_t index, int64_t size, const void *content, void *userData);

        static void subtitleHideCallback(int64_t index, int64_t size, const void *content, void *userData);

        static void subtitleExtAddedCallback(int64_t index, const void *url, void *userData);

        static void subtitleHeaderCallback(int64_t index, const void *header, void *userData);

        static void streamChangedSucCallback(int64_t type, const void *Info, void *userData);

        static void videoQualitySwitchCallback(int64_t status, int64_t streamIndex, const void *desc, void *userData);

        static void PlayerSeeking(int64_t seekInCache, void *userData);

        static void PlayerSeekEnd(int64_t seekInCache, void *userData);

        static void PlayerStatusChanged(int64_t oldStatus, int64_t newStatus, void *userData);

        static void mediaInfoGetCallback(int64_t count, const void *Infos, void *userData);

        static void captureScreenResult(int64_t width, int64_t height, const void *buffer, void *userData);

        static void autoPlayStart(void *userData);

        void abrChanged(int stream);

        static void onMediaFrameCallback(void *arg, const IAFPacket *frame, StreamType type);
        void mediaFrameCallback(const IAFPacket *frame, StreamType type);

        void configPlayer(const MediaPlayerConfig *config) const;

        void refreshPlayerSessionId();

        static void dummyFunction(bool dummy);

    private:
        void *mPlayerHandle = nullptr;
        playerListener mListener{nullptr};
        MediaPlayerConfig *mConfig{};
        AnalyticsQueryListener *mQueryListener{};
        IAnalyticsCollector *mCollector{nullptr};
        bool  bExternalCollector{false};
        IAnalyticsCollectorFactory &mCollectorFactory;
        AbrManager *mAbrManager{};
        AbrAlgoStrategy *mAbrAlgo{};
        AbrBufferRefererData *mAbrRefData{nullptr};
        std::mutex mMutexAbr;
        bool mLoop{false};
        bool waitingForStart{false};
        bool waitingForLoop{false};
        string mPlayUrl;
        CacheManager *mCacheManager{};
        CacheConfig mCacheConfig{};
        std::atomic<bool> mCacheSuccess{false};
        PlayerStatus mOldPlayStatus{PLAYER_IDLE};
        playerMediaFrameCb mMediaFrameFunc{nullptr};
        void *mMediaFrameArg{nullptr};
        function<void(const string &)> mPlayUrlChangedCallback{nullptr};
        std::string mPlayerSessionId{};
        bool mFirstPrepared{false};
    };
}// namespace Cicada


#endif /* CicadaPlayer_h */
