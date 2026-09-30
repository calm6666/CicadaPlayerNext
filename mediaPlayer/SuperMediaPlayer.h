#ifndef CICADA_PLAYER_SERVICE_H
#define CICADA_PLAYER_SERVICE_H

#include <string>
#include <atomic>
#include <memory>
#include <mutex>

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
/*
 * 播放缓存的管理器类型（CacheManager 的**完整定义**，不是 CacheModule 的前向声明）。
 * 本类末尾那几个缓存成员是指针，本来只靠前向声明也能声明；但 SetCacheConfig /
 * GetCachePathByURL 的 override 与 .cpp 里对 CacheManager 成员函数的调用都需要
 * 完整类型，照 MediaPlayer.h 的既有写法在这里一并包含（同一个头，同一份来源）。
 */
#ifdef ENABLE_CACHE_MODULE
#include <cacheModule/CacheManager.h>
#endif
#include <codec/IDecoder.h>
/*
 * 应用层传入的"设备硬解能力 + 编码偏好"覆盖值的类型
 * （decoderFactory::AppCodecSupport）：本类持有它的一份 shared_ptr，并把同一份
 * 发布给 decoderFactory 供 ABR / 起播回调读取。见成员列表末尾。
 */
#include <codec/decoderFactory.h>
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

    /*
     * ============ 【不连续点模型（本次重构的时间轴唯一权威）】============
     *
     * 旧实现把一个"不连续点"拆成了十几个互不相识的闩（落点闸门、位置地板、
     * 锚点事件、音频地板、连续性高水位、追赶窗口……），每个闩各自有置位点、
     * 消费点和"忘了清"的悬挂形态，于是出现了"落点窗口被 seek 结束提前关掉"
     * "位置回弹""音频扣住 PCM 等时钟"这一整类问题。根因是：**没有一个东西
     * 能回答"这条包/帧/时钟事件属于哪一次 seek"**，只能靠手工清零来防陈旧写入。
     *
     * 现在改成主流的做法：一个不连续点 = 一个三元组，代际（generation）是
     * 唯一的归属判据。
     *
     *   targetUs   用户请求点。只服务两件事：对外位置上报、渲染器输出过滤。
     *   startUs    实际起播点（demuxer 落点，可早可晚，通常是关键帧）。
     *   generation 每次 seek / 换档 +1。任何异步事件（包、帧、时钟基准、
     *              待处理请求）都带着它去比对；代际不符 = 陈旧 = 当场作废，
     *              不再需要"归属标记/闩"这种手工防线。
     *
     * generation 用 atomic：写入点在 API 线程（SeekTo / SwitchStream），
     * 读取点在内核工作线程（读包 / 解码 / 渲染）。字段名与三元组的形状
     * 与设计一致，只是把并发语义显式化。
     */
    struct Discontinuity {
        int64_t targetUs{INT64_MIN};
        int64_t startUs{INT64_MIN};
        std::atomic<int> generation{0};

        /*
         * ---- P1/P2.1：renderer 单一过滤 + 唯一的"内容时间"来源 ----
         *
         * filterActive     本次不连续点的"落点过滤"是否仍然生效。它**只**由
         *                  "包含目标的那一帧已上屏"（shouldDropForDiscontinuity 的结束分支，
         *                  含 EOF 兜底那条）或"下一次 seek / Reset / Prepare"
         *                  （beginDiscontinuity）关闭。**SeekEnd / ResetSeekStatus 一律不得
         *                  关它** —— 那是分片源"seek 永远差一个落点前缀"的根因。
         * acceptedFramePos  真正被采纳为落点的那一帧的位置。只服务诊断（回答"落点到底
         *                  采纳了哪一帧、离目标多远"），不参与任何判定。
         *
         * 【P2.1 删除：P1-b 那对"墙钟位置基准"字段】这里原来还有一对字段（一个媒体值基准 +
         * 一个单调毫秒），实现的是"位置 = 不连续点基准 + **墙钟**增量"。于是上报位置与
         * 渲染/音频参考各走一根轴：seek（或切档）之后只要发生一次缓冲停顿，墙钟照走而设备
         * 没消费 ⇒ 进度条跑到画面与声音前面，而当时的"回收"分支只能处理墙钟**落后**。
         * 现在只有一个来源 —— mMasterClock.GetTime()（有音频 = 目标点 + 设备已消费量；
         * 无音频 = 暂停感知的自走时钟，单位微秒、与 targetUs 同轴），见 getCurrentPosition()
         * 的长注释。字段减少 = 机制减少，不再需要冻结哨兵与回收分支。
         *
         * 并发：filterActive / acceptedFramePos 是**跨线程发布的值** —— 写侧是 SeekTo /
         * SwitchStream 所在的 API 线程（或 Reset 所在的消息线程），读侧是内核工作线程的
         * 渲染路径与上报路径，所以取 atomic。targetUs / startUs 保持普通字段，因为 P0
         * 已经约定了访问顺序（任何读侧**先** generation.load() 建立与写入侧的配对关系，
         * 再读它们；shouldDropForDiscontinuity 与 getCurrentPosition 都是按这个顺序写的）。
         */
        std::atomic<bool> filterActive{false};
        std::atomic<int64_t> acceptedFramePos{INT64_MIN};

        /*
         * ---- 音频落点地板：音频路自己的"目标点闸门"（不共用视频的 filterActive）----
         *
         * 为什么必须与 filterActive 分开：filterActive 是一个**全局**闩，而它的关闭点是
         * "**视频**落点帧被采纳"（RenderVideo -> shouldDropForDiscontinuity ->
         * acceptDiscontinuityLandingFrame）。视频与音频跑在**内核工作线程的同一个
         * render() 之内、但是两次独立调用**（render() 先 RenderAudio() 再 RenderVideo()），
         * 于是存在一个真实的一帧级竞态：某次 render() 里 RenderVideo() 采纳了视频落点帧
         * 并把 filterActive 清成假，而**下一次** render() 的 RenderAudio() 才去处理
         * 那一帧音频 —— 此时闸门已经不在了，落在目标点**之前**的音频帧就被直接推给了
         * 音频设备。
         *
         * 真机日志（本地文件 "新建文本文档.txt"，Android，2026-09-29 12:14:51 那次 seek）：
         *   seek 目标 557028000，视频落点 pts=557000000（偏移 -28 ms，正确）；
         *   同一次 seek 紧接着的音频首帧是
         *   `audio first frame after seek: pts=555885714 target=557028000 ... consumed=340000`
         *   —— 音频首帧落在目标点**之前 1.14 秒**。
         *
         * 危害不是"声音晚一会儿响"，而是它把音频时钟带偏：音频时钟模型的唯一不变量是
         *   audioPosition = audioBaseUs(target) + 设备已消费量
         * 一旦设备里被塞进 [落点, 目标) 这段陈旧音频，设备消费的每一微秒都被算成
         * "走在目标点之后"，音频参考就**超前内容**，RenderVideo 随即判视频"迟到"、
         * 丢帧甚至跳到关键帧 —— 这就是 seek 之后长期存在的音画错位。
         * 代码里这条危害本来就写在 RenderAudio 的落点丢弃判据上方（"若把这些帧推给设备，
         * 时钟就会比内容超前"），只是它依赖的那个闩会被视频提前关掉。
         *
         * 语义：audioLandingPending 为真 = 音频路尚未走到本次不连续点的目标点，
         * 落在目标点之前的音频帧一律不上设备。**只有 seek 会重新锚定音频时间轴**
         * （FlushAudioPath -> pinAudioClockBase(targetUs)），所以它只由 seek 臂上；
         * 换档（switchVideo）不 flush 音频、时间轴连续，绝不能臂上它（否则音频会被
         * 永久闸住）。关闭只有两个出口，都是纯状态判据，没有计时器：
         *   · 音频队首真的到达/越过目标点（RenderAudio 的落点丢弃判据里就地清假）；
         *   · 音频解码器已 EOS（此后不会再有更晚的帧，否则会把音频闷死）。
         * 下一次 beginDiscontinuity（新的不连续点）会无条件重新判定它，这也是一条出口。
         *
         * 并发：与 filterActive 同族 —— 写侧是 seek 所在的消息线程，读侧是内核工作线程的
         * 音频渲染路径，所以取 atomic；targetUs 的读取仍沿用本结构体既有的约定
         * （读侧先 load generation 建立配对关系，再读非原子字段）。
         */
        std::atomic<bool> audioLandingPending{false};

        /*
         * ---- P2：音频时钟基准（唯一来源 = 设备已消费量，ijkplayer / ExoPlayer 模型）----
         *
         * audioBaseUs         音频时钟的**内容位置**基准。不连续点上由 FlushAudioPath()
         *                     钉在 targetUs（与视频**同一个目标点**）；非 seek 的 flush
         *                     保持内容位置连续（旧基准 + 本次消费增量外推）。
         * audioBaseConsumedUs 写 audioBaseUs 那一刻设备**已消费**的媒体时间快照（微秒）。
         *                     之后 audioPosition = audioBaseUs + (已消费 - 该快照)。
         * 读侧只有一个：getAudioPlayTimeStamp()（主时钟的音频参考）。
         * INT64_MIN = 音频时钟不可用（主时钟退回自走）；暂停时设备不消费 ⇒ 位置自动冻结。
         * 设备已消费量取自既有接口 getAudioRenderPosition()（Android 为
         * AudioTrack.getPlaybackHeadPosition() 按采样率折算），不新增虚函数、不引平台宏。
         */
        std::atomic<int64_t> audioBaseUs{INT64_MIN};
        std::atomic<int64_t> audioBaseConsumedUs{INT64_MIN};
    };

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

        /* 【色觉辅助滤镜 / 回退点 C7】与 SetMirrorMode 同一形态 */
        void SetColorMatrix(const float matrix[9]) override;

        void SetVideoBackgroundColor(uint32_t color) override;

        MirrorMode GetMirrorMode() override;

        int GetCurrentStreamIndex(StreamType type) override;

        StreamInfo *GetCurrentStreamInfo(StreamType type) override;

        float GetVolume() const override;

        void CaptureScreen() override;

        void SetDecoderType(DecoderType type) override;

        DecoderType GetDecoderType() override;

        /*
         * 当前视频解码器**实际在用**硬解还是软解（见 ICicadaPlayer 里的完整说明：
         * 事实读数，含运行期退回软解；没有视频解码器时是 false）。
         */
        bool IsVideoDecoderHardware() override;

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

        /*
         * 【应用层视频编码"硬解能力 + 效率偏好"】见 ICicadaPlayer.h 里那一段契约说明。
         *
         * get：返回**当前实际生效**的能力/偏好 JSON。应用层没传过时是内核探测结果
         *      （source = "kernel"），传过时是应用层那份（source = "app"，
         *      探测完全不再发生）。
         * set：json 为空串 = 清除应用层覆盖（恢复内核探测），返回 0；
         *      畸形/非法 JSON 返回非 0（-EINVAL）且**不改变**当前状态。
         *
         * 状态本体与线程安全见成员列表末尾的 mAppCodecSupport。
         */
        std::string GetVideoCodecSupportJson() override;

        int SetVideoCodecSupportJson(const std::string &json) override;

        /*
         * ==================== 【播放缓存 play-and-cache】====================
         *
         * 见 ICicadaPlayer.h 里那一段契约说明。这里只补本类的实现要点：
         *
         * SetCacheConfig()：只有配置真的变了（CacheConfig::isSame 为假）才动状态，
         *   并且先把已在跑的那个缓存管理器 stop 掉 —— 理由与 C++ 门面
         *   MediaPlayer::SetCacheConfig 逐字一致（那份缓存是按旧配置写的，接着写会
         *   混进旧目录/旧文件名）。stop 只置状态并停掉内部 remuxer 线程，不删管理器
         *   本体（下一次 SetDataSource 才重建）。
         *
         * GetCachePathByURL()：纯计算，转发到 CacheManager::getCachePath(url, config)
         *   （静态函数，不需要管理器实例存在）。
         *
         * 缓存真正"接上"的位置在 SetDataSource(const char *)：那里算出代理 URL 并交给
         * 既有的 MSG_SETDATASOURCE 通路（见 .cpp 里的长注释）。那个"把播放器当数据源"
         * 的 ICacheDataSource 适配器由 CacheManager 拥有并在它析构时 delete。
         */
        void SetCacheConfig(const CacheConfig &config) override;

        std::string GetCachePathByURL(const std::string &url) override;

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
         * 静音窗口起止日志（只在状态变化时各一条；不改动任何播放行为，
         * 编码见 AUDIO_SILENCE_* 与成员 mAudioSilenceReason）。
         */
        void logAudioSilence(int reason, const char *detail, int64_t audioPts, int64_t clockUs);

        /*
         * 清空视频解码/渲染路径。
         * 切档是**单解码器原地换档**（见 SuperMediaPlayer::SwitchVideo）：换流、按流定位、
         * 清解码器与解码帧队列、按新档 meta 决定是否重建同一块解码器，任何时刻只有一块。
         *
         * from：调用者名字（传 __func__）。每次调用都留一行日志，方便定位"是谁把视频路 flush 了"。
         */
        void FlushVideoPath(bool flushRender = true, bool cancelPendingSwitch = true, const char *from = nullptr);

        /*
         * 清晰度切换的**唯一终态出口**（READY / FAILED）。
         *
         * 前置条件只有一条：`mVideoSwitchInFlight`（有挂着的切档请求）。终态时清零它
         * 与 `mVideoSwitchTargetIndex`，所以本函数天然幂等。
         */
        void finishQualitySwitch(bool ready, const char *reason);

        /*
         * 开始“渲染器追赶窗口”：在窗口内丢掉迟到的帧（并把还没解码的过时包也丢掉），
         * 让画面尽快回到主时钟上。用于清晰度切换提交后、以及 seek 完成之后。
         * 对应 ExoPlayer 的 setJoiningDeadlineMs()。窗口上限见
         * JOINING_DROP_LATE_WINDOW_MS。
         */
        void beginRendererJoining(const char *why);

        /*
         * seek 卡死排查用的**状态翻转日志**（只在翻转时各打一行，不刷屏）。
         *
         * 起因（2026-09-23 安卓 pause.log，22:58:03.880 那次 seek）：
         * seek 之后整份日志只剩三条 INFO —— PFR: seek / FlushVideoPath /
         * clearCache，然后 545ms 后 PlayerBase 的 HSM 就 baseTimeout() + stop()。
         * 而"seek 为什么没完成"这件事在日志里**一个判据都没有**：
         *   * mSeekFlag   只由 doRender() 里"有帧真的上屏"那一处清掉，它是不是还挂着；
         *   * mBufferingFlag 只在 DoCheckBufferPass() 里置位/清除，它是不是把
         *     doRender() 挡在 return false（:2103）之外；
         *   * 帧队列是不是空的、解码器是不是被标记成 EOS；
         * 这三件事全都无日志，于是"没帧上屏"到底是缓冲没起来、解码器死了、
         * 还是 seek 状态没清，只能靠猜 —— 这次就是猜不出来。
         * 这里把它们的翻转补上：一行说清"谁在什么时候变成了什么"。
         * 每次翻转一行，稳态下一行都不打。
         */
        void logSeekPipelineState(const char *why);

        /*
         * 是否有在途的清晰度切换。单解码器模型下只有**一个**判据：
         * `mVideoSwitchInFlight`（由 switchVideoStream() 置位，由 SwitchVideo() 的失败
         * 出口与 finishQualitySwitch() 清零）。ABR 让路、read-ahead 门、诊断行
         * 全部读它，避免三处各写一份而漏形态。
         *
         * 保留这个函数名：IsStreamSwitchInFlight() 与 ABR 让路都调用它。
         */
        bool isQualitySwitchInFlight() const;

        /*
         * 洪水日志限频（W0.4）。
         *
         * 逐帧/逐包打日志的那几个点（unknown stream / read-ahead gate）在一次切换里
         * 能刷出 4000+ 行，把真正重要的
         * 那几行淹掉，也把日志文件撑到几 MB。这里统一限成“每个窗口内最多放行
         * 若干条，其余丢弃并在窗口切换时补一行汇总”。
         */
        enum FloodLogId {
            FLOOD_UNKNOWN_STREAM = 0, /* unknown stream */
            FLOOD_READ_AHEAD,         /* 读前闸门触发的说明行 */
            FLOOD_DECODE_STALL,       /* "还在等 seek 的第一个关键帧"导致的停滞判据跳过 */
            FLOOD_STALE_DROP,         /* 切档后把旧流残留包挡在解码器之外（限频，避免刷屏） */
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
        /*
         * 视频原始 pts 轴 → 主时钟（timePosition）轴 的映射偏移。
         *
         * 两个建立点：seek 落点（DecodeVideoPacket 里 B3 那段，由落点包自己的
         * timePosition − pts 算出）、以及 FlushVideoPath / Reset 的清零点。
         * 两个消费点：DecodeVideoPacket 给"不带 timePosition"的帧做 pts 归一化，
         * doReadPacket 的读前闸门把包 pts 折算到主时钟同一把尺子上。
         */
        int64_t mActiveVideoPtsOffset{INT64_MIN};
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
        /*
         * 【第七项：死代码清理 —— 这四个成员**故意保留**，不要删】
         *
         * 它们原来是"多久没上屏就判死并重建解码器"那套墙钟巡检的状态，机制已按红线
         * 整体删除（见 .cpp 的 doRender() 历史说明），现在**已无任何读取点**
         * （mLastVideoFrameRenderedMs 还被"上屏成功"那一刻写入，属于无害的残留写入）。
         *
         * 为什么保留而不是删掉：本文件顶部的**增量构建偏移契约** —— 本工程不记录头文件
         * 依赖，friend 监听类按成员偏移直接访问；删成员会移动其后所有成员的偏移，
         * 让未重编的旧 TU 目标文件按错偏移访问（这是真实存在过的坑，见头文件里那段说明）。
         * 因此"零行为变化的清理"在这里的正确做法是：**留成员、删宏、写清楚**。
         * 真要删，必须连同其后所有 TU 一起全量重编，不能只改这一处。
         *
         * 下面 mLastVideoFrameRenderedMs / mVideoRecoverCooldownMs / mRecoverSampleMs /
         * mRecoverSamplePackets 都属于这一类。
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
         * mSwitchStateLogMs        [switch] 每秒一行的限频时间戳。
         * mFloodLog[]              FloodLogId 里每一项的窗口计数（W0.4）。
         */
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
         * 【P1-b 删除：seek 位置"地板"】
         *
         * 这里原来有一个 int64_t 成员记"seek 目标点"，getCurrentPosition() 在管道走到它之前
         * 一直上报它（防进度条回退）。它已随 P1-b 删除，P2.1 起位置上报读的是**唯一那根内容
         * 时间轴**（mMasterClock.GetTime()：有音频 = 目标点 + 设备已消费量，无音频 = 自走
         * 且暂停感知，见 getCurrentPosition()）—— 设备不消费就不前进 ⇒ 数学上不回弹、
         * 停顿与暂停都自然停住 ⇒ 不需要任何"下界/兜底链"，也不需要冻结哨兵与回收分支。
         * 与它一起删除的还有它的归属标记与渲染闸门（见本文件里 Discontinuity 的说明）。
         */

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

        /*
         * logSeekPipelineState() 的"上一次值"。纯粹为了"只在翻转时打一行"，
         * 不参与任何判定。同样**追加在成员列表末尾**（增量构建不做头文件依赖，
         * 插在中间会移动其后成员的偏移，见上面那段说明）。
         */
        bool mSeekDiagLastSeekFlag{false};
        bool mSeekDiagLastBufferingFlag{false};
        bool mSeekDiagLastFirstBufferFlag{true};
        bool mSeekDiagLastVideoDecoderEOS{false};
        int mSeekDiagLastVideoFrameQueSize{-1};

        /*
         * ============ seek 的墙钟出口（2026-09-24）============
         *
         * 为什么必须有：播放中 seek 之后，"精确 seek"这个标志会把**音频**也一起
         * 掐死 —— render() 里 `if ((mCurrentAudioIndex >= 0) && !mSeekNeedCatch)`
         * 一帧音频都不推，而 mSeekNeedCatch 只在 FillVideoFrame() 真的从解码器
         * 取到一帧时才清。于是"视频解码器没吐出目标帧"这件事会连带把音频饿死，
         * AudioTrack 约 500ms 后 BUFFER TIMEOUT → 预编译 PlayerBase 的 HSM
         * baseTimeout()+stop()（2026-09-23 pause.log: 03.880 seek → 04.425
         * baseTimeout，中间一次音频都没推）。这条链跟"视频能不能出帧"绑在一起
         * 是设计缺陷：seek 的进度回调可以等，音频缓冲不能等。
         *
         * 这里只记两个时间戳，不引入任何周期性动作：
         *   mSeekCatchStartMs    —— mSeekNeedCatch 变成 true 的墙钟时刻；
         *   mSeekNoFrameSinceMs  —— "seek 在途且帧队列为空"这段已经持续多久。
         * 用 af_getsteady_ms()（单调钟），不用主时钟 —— 主时钟在 seek 期间是被
         * 钉住/暂停的，拿它计时会永远等不到超时。
         *
         * 同样**追加在成员列表末尾**（见上面那段"增量构建不做头文件依赖"）。
         */
        int64_t mSeekCatchStartMs{0};
        /*
         * 【第七项：死代码清理】mSeekNoFrameSinceMs 原是"seek 在途且帧队列为空持续多久"
         * 的墙钟计时；产生它的墙钟块已按红线删除，现在只剩 Reset() 里的一次写、无人读。
         * **故意保留**：删它会移动其后成员的偏移，违反本文件的增量构建偏移契约。
         */
        int64_t mSeekNoFrameSinceMs{0};

        /*
         * seek 墙钟死线触发那一刻，"输出缓冲被谁占住"的诊断只打一次用的闩。
         * 纯诊断，不参与任何判定；同样追加在末尾。
         *
         * 【第七项：死代码清理】产生它的墙钟机制已删除，本闩现在既不被读也不被写
         * （连同 mSeekNoFrameSinceMs 一起成为历史成员）。**故意保留**：删它会移动其后
         * 成员的偏移，违反本文件的增量构建偏移契约（见上面那段说明）。
         */
        bool mSeekStallDiagLogged{false};

        /*
         * ============ 【P1-b 删除：seek 落点闸门 / 落点采纳闩 / 落点归属标记】============
         *
         * 这里原来有四个成员：一个 int64_t 的"渲染闸门"、一个"本次 seek 是否已接受落点帧"
         * 的闩、一个与位置地板同点写入的"归属"标记（在下面另一处）、以及一个"落点是否够近
         * 才精确"的预算闩（也在下面另一处）。它们与位置地板一起构成"十余个闩"里最核心的
         * 一组，现已全部删除。职责改由一处承担：
         *   · mDiscontinuity.filterActive  —— 落点过滤是否生效（唯一写点见 .cpp）；
         *   · mDiscontinuity.targetUs      —— 唯一的"用户目标点"载体；
         *   · shouldDropForDiscontinuity() —— 唯一的落点判据（与目标点本身比较）。
         *
         * mSeekExactLanding 保留旧语义（必须精确到目标帧、目标点之前的帧一律不上屏）的开关，
         * 默认 0 = 落点即上屏；需要旧行为时由各端 setOption("seekExactLanding", "1") 打开。
         *
         * 【P2】"一次 seek 只允许锚定一次"那个闩（音频首帧与视频落点帧谁先到谁锚）已随
         * 音频基准重设整体删除：主时钟现在只有一个来源 —— 有音频时是
         * "目标点 + 设备已消费量"（Discontinuity::audioBase*），无音频时是落点采纳时
         * 钉在 targetUs 的时钟。既然只有一个真相，就不需要"谁先锚、锚没锚过"的闩。
         *
         * 全部追加在成员列表末尾（本文件顶部有约定：中间插入会移动偏移、破坏增量构建）。
         */
        bool mSeekExactLanding{false};
        /*
         * 本次 seek 是否"从关键帧起步解码"（由 DecodeVideoPacket 在关键帧包上置真）。
         * 花屏（马赛克）就是"非关键帧起步解出来的帧被显示出去"：解码器缺参考帧，
         * 输出是脏的。渲染侧必须等这个闩为真才允许接受落点帧。
         */
        bool mSeekDecodeStartIsKey{false};
        /*
         * S2：视频路"手里什么都没有"连续成立的轮数（**状态计数，不是时间**）。
         * 达到 VIDEO_STARVE_ITERS 时按"缓冲空"处理：暂停主时钟 + 暂停音频渲染 +
         * 通知界面，避免"画面冻住、声音继续"。数据一回来立刻清零。
         */
        int mVideoStarveIters{0};
        /*
         * C 方案：本次 seek 是否已经把音频**起点对齐到目标点**（一次性闩）。
         * 事件：读到本次 seek 的第一个关键帧视频包（= 落点 PTS 已知）。
         * 动作：把音频包队列裁到**目标点**并做本进程内的时间轴重基 —— 音频与视频
         * 从同一个目标点起步（视频只是因为必须从关键帧起解才落得早一些）。
         * 失败/不支持就什么都不做，按既有路径继续。
         */
        bool mSeekAudioAlignDone{false};
        /*
         * 解码器"有输入却零输出"的一次性救援（纯状态驱动，与时间无关）。
         * 判据**不再限定在 seek 窗口内**：安卓实测卡死发生在 seek **结束之后**
         * （seekFlag 只活 16~23ms，而 codec 的"输出缓冲被占住"信号 0.5~1s 后才出现，
         * 于是"只在 seek 窗口内判定"永远等不到），所以这三个状态由"应当有帧在流的
         * 播放态"驱动：
         *   mVideoDecodeRetrySeen   —— 观察到"解码器不收输入"这个**状态签名**
         *                              （sendPacket 返回 STATUS_RETRY_IN：解码器自己的输入队列满，
         *                              即 codec 拿着缓冲不放手）；
         *   mDecodeStallIters       —— 该签名成立、且**一帧都没产出**（帧队列为空）时，
         *                              管线自身工作循环走过的轮数（不是时间，也不是周期动作）；
         *   mDecodeStallRebuildDone —— 本次卡死是否已重建过一次；**出帧即自愈**
         *                              （FillVideoFrame 收到帧时清掉，与重建预算一起复位），
         *                              所以它既是"每次卡死只重建一次"的闩，也是不用计时器的冷却。
         * 三者同时成立即走**错误驱动**那条现成路径 rebuildVideoDecoder(false)。
         */
        bool mVideoDecodeRetrySeen{false};
        int mDecodeStallIters{0};
        bool mDecodeStallRebuildDone{false};
        /*
         * 音频静音窗口的**状态**（0 有声 / 1 seek 窗口内等主时钟 / 2 设备写失败 /
         * 3 设备被 flush 重建）。只用于"起止各一条"的日志，不参与任何管线决策：
         * 判据全是状态变化，没有时间阈值，也没有周期性动作。
         */
        int mAudioSilenceReason{0};
        /*
         * 【P1-b 删除：有界精确落点的"预算闩"】
         *
         * 这里原来有一个"本次 seek 是否值得精确到目标帧"的闩：它靠"落点离目标不超过某预算"
         * 来决定要不要把早于目标的帧挡在门外。那是**精度换等待** —— 精度取决于预算给不给，
         * 而不是取决于目标本身（稀疏 IDR 片源的落点常常早 1~11 秒，于是永远拿不到精确落点）。
         * 现在判据与目标本身比较（shouldDropForDiscontinuity），与落点距离无关，预算闩删除。
         */
        /*
         * 【本轮修"seek 之后一直转圈 + 日志刷满"】PTS_REVERTING 分支的日志闩：
         *   0 = 既不在等待也不在强制渲染；1 = 正在"强制渲染旧视频帧"；2 = 正在"等音频倒回来"。
         * 只在进入/离开该状态时各打一条日志（原来每轮 2~3 行，每秒数百~上千行）。
         * 纯日志用途，不参与任何决策。
         */
        int mPtsRevertWaitLogged{0};
        /* 同上限频用：上一次已经打过日志的音频位置（值没变就不再打）。 */
        int64_t mPtsRevertLastLoggedAudio{INT64_MIN};
        /*
         * [seekdiag] 的**语义**变化检测（本轮修刷屏）：
         * 原来把 frameQueSize 也算进"变化"，而 frameQ 在 0/1 之间每轮都跳，
         * 于是每轮主循环都打一条 —— 2 秒 190+ 行，自己吃掉 CPU 与日志带宽。
         * 现在只看语义字段：seekFlag / seekNeedCatch / buffering / videoEOS /
         * playStatus / vDecValid（队列长度只作为输出，不参与判定）。
         */
        bool mSeekDiagLastSeekNeedCatch{false};
        int mSeekDiagLastPlayStatus{-1};
        bool mSeekDiagLastVideoDecoderValid{false};

        /*
         * ============ B4（两条渲染路）：active 视频解码器**实际绑定的输出面** ============
         *
         * 就是 `CreateVideoDecoder()` 里算出来的那个 view：
         *   · 隧道（FLAG_DUMMY，解码器直出）：= App 的 Surface（`mSet->mView`）；
         *   · GL（GLRender）：= 渲染器 SurfaceTexture 的那块 Surface（`GLRender::getSurface()`）。
         *
         * 生命周期：`CreateVideoDecoder()` 一开始置 null（失败即保持 null），成功后写入；
         * 解码器被销毁的路径（Reset / closeVideo）也置 null。
         * 不额外持有 JNI 引用（与 mSet->mView 同样的用法：调用方保证它活得比播放器长）。
         */
        void *mActiveVideoSurface{nullptr};

        /*
         * ============ 【B6】后端能力探测：输出帧带不带节目时间轴（timePosition）============
         *
         * `FillVideoFrame()` 里只要见到一张 `timePosition >= 0` 的帧就置真（单调、粘性）。
         * Android 的 MediaCodec 后端**不带**（`mediaCodecDecoder.cpp:692` 仍是
         * `INT64_MIN`，那句 TODO 还在），Qt / macOS / iOS 的解码器带。
         *
         * 切档预滚用它决定对齐策略 —— 注意这是**能力探测**，不是平台分支：
         *   · 带 ⇒ 沿用既有逻辑（帧自带 timePosition，逐字不变）；
         *   · 不带 ⇒ 目标档的帧没法自报位置，只能"按构造对齐"：预滚从参考点**之前**
         *     最近的关键帧起步（解掉前缀、由切换窗丢帧丢掉它），提交时手上就有贴着
         *     主时钟的帧。否则就会走主时钟相对偏移把帧**重命名**到时钟轴上，把
         *     "预滚起点领先参考点的量（lead，本片源 4.5~9 s）"变成恒定的音画错位
         *     ——真机日志里 `committed … offset=-9473284` 就是它。
         */
        bool mActiveDecoderFramesCarryTimePosition{false};

        /*
         * 【B8】音频时钟纠偏的诊断水位（纯诊断，不参与任何判定）。
         * 纠偏把"每帧 pts 与预测位置的差"累加进 mAudioTime.deltaTime（见 RenderAudio 里
         * kGradualStepUs 那段）。如果这个累加**单向持续**，主时钟就会相对真实时间被拉偏，
         * 表现为"越播音画偏差越大、最后完全对不上"。这里记住上次打印时的水位，
         * 每累计变化 100 ms 打一行（事件驱动，无计时器），一眼看出是不是 deltaTime 在爬。
         */
        int64_t mAudioClockDriftLoggedUs{0};

        /*
         * ============ 【B10】"视频包 pts → 节目时间轴(timePosition)"的逐帧配对表 ============
         *
         * DASH 的每一档都会在**每个分片边界**重算 `time2ptsDelta`（`DashStream.cpp:880`、
         * `HLSStream.cpp:1295/1315` 同理），于是"原始 pts 轴 → 节目轴"的映射**不是常数**：
         * 真机（2026-09-25 14:00，`output.mpd`）实测每 10 s 分片要修 427 ms（4.27%）。
         *
         * 用一个全局偏移去平移帧（哪怕每个包都跟着刷新）在分片边界上总有一步台阶。
         * 这里改成**逐帧精确配对**：`DecodeVideoPacket()` 把 active 流每个包的
         * `(pts → timePosition)` 记进这两个并行的双端队列，`FillVideoFrame()` 用帧自己的
         * pts 精确查表，直接把帧的 pts 设成它自己那一个包的 timePosition ——
         * 每一帧都落在自己的真实节目位置，于是**没有漂移、没有台阶、没有跳变**。
         *
         * 只在 active 流、且没有切档在途时记录（切档时流进来的是另一档的包，delta 不同）；
         * 容量上限 64，按顺序消费/淘汰（帧与包同序），查不到就退回 mActiveVideoPtsOffset
         * 那条全局偏移（B9-2）—— 所以这是**纯增量**：任何异常路径行为与今天一致。
         * 全部在内核里，没有平台分支 ⇒ Qt 与安卓同一份。
         */
        std::deque<int64_t> mVideoAxisPts;
        std::deque<int64_t> mVideoAxisTimePos;

        /*
         * ============ 【P2 删除：音频时钟"设备位置真的前进过"的第二套观察基准】============
         *
         * 这里原有三个成员（一个"设备位置基准"、一个"是否见过前进"的闩、一个限频计数），
         * 与本轮删除的"音频重锚"状态同生共死。它们的职责已经被
         * Discontinuity::audioBaseUs / audioBaseConsumedUs 完整吸收，而且是**同一个量**：
         *   · 旧方案的基准 = "seek 重锚之后第一次读到的设备位置"，语义是"设备从哪儿开始算"；
         *   · 新方案的基准 = 写内容位置那一刻的设备已消费量快照，语义完全相同，
         *     但它与**内容位置**（targetUs 或首帧 pts）写在同一个原子字段对里 ⇒
         *     不存在"基准在这儿、内容位置在那儿"的两条真相。
         * 于是"主时钟只允许有一个来源"这条不变量成立，本组成员删除。
         * 旧方案要治的坏值（哨兵/负值/向后跳）在新判据里仍然逐个拒绝：
         * 见 getAudioPlayTimeStamp() 里的 `consumedUs < 0` 与 `delta <= 0` 两条。
         */

        /*
         * ============ 【P1-b 删除：B15"先出画"的独立事件闩】============
         *
         * 这里原来有一个"seek 之后是否已经先出过一张干净帧"的闩（配合"只先出一张、其余
         * 前缀帧一律丢"的旧语义）。P1 起改为**单一过滤规则**：
         *   · 完全落在目标之前的帧**一律不上屏**（画面停在上一张，不会黑屏）；
         *   · 包含目标、或已经越过目标的那一帧强制上屏，并结束本次过滤。
         * 所以"只先出一张"这个语义已经不存在了，本闩删除。
         *
         * ============ 【B16】"追赶不收敛 ⇒ 一帧都不送"的连续事件计数 ============
         *
         * mVideoDiscardStreak：播放态下渲染判定**连续**给出"不上屏"的帧数，任何一次
         *   真的上屏都把它清零（见 RenderVideo 的两个分支）。
         * mVideoDiscardGapAbsUs：上一次被拒帧与主时钟的距离绝对值（|master - videoPts|）。
         *   新的一帧如果这个距离**变小了**，说明追赶在收敛（例如 seek 里时钟钉在目标上、
         *   解码从落点往目标走），计数立刻回到 1，**不会**去强制放行；
         *   只有"距离一直不变小"（两条轴恒速同向、偏移恒定 = 追赶不收敛）才累计到阈值，
         *   由 RenderVideo 强制放行一帧，避免画面永久冻住（安卓日志形态：3.09s / 246 条
         *   drop、偏移恒定 ~3s、期间 0 次上屏、0 条落点采纳 ⇒ 画面冻死）。
         *
         * 两个成员都只是**观测/出口**用途：不改主时钟、不改落点判据、不改位置上报；
         * 复位口径与不连续点过滤闩（mDiscontinuity.filterActive）一致（SeekTo / Reset）。
         * 无计时器（纯事件计数）。新成员一律追加在类末尾（本文件顶部硬约束：
         * 中间插入会让别的 TU 的偏移对不上）。
         */
        int mVideoDiscardStreak{0};
        int64_t mVideoDiscardGapAbsUs{INT64_MIN};

        /*
         * 【B16-b】追赶期（dropLateVideoFrames）内的**连续被拒帧数**（不做"偏移是否变小"
         * 的过滤）。追赶期偏移可能只是在缓慢收敛（实测 ~2~3ms/帧），上面那条判据不会触发，
         * 于是还要这条：连续被拒达到 VIDEO_CATCHUP_DISCARD_STREAK_MAX 就强制放行一帧。
         * 只在"落点过滤未激活"（mDiscontinuity.filterActive 为假）时累计，避免与 B15
         * 的"seek 前缀只先出一张"打架；上屏即清零（见 RenderVideo）。
         * 纯事件计数，无计时器。新成员追加在类末尾（本文件顶部硬约束）。
         */
        int mCatchUpDiscardStreak{0};

        /*
         * ============ 【B19】切档在途时不发 PFR seek，改为"推迟到切档终态" ============
         *
         * PFR（暂停帧恢复）的发起点在**内核内部**：SuperMediaPlayer::RestorePausedVideoFrame()
         * 在暂停态读到"解码器最后渲染帧 pts"后直接 SeekTo(...)（日志里的
         * `PFR: seek posUs=…` 就是它）。上层（Compose/QML）反复 setView 时这个函数会被
         * 反复调到，于是切档在途期间一次次插进 seek —— `FlushVideoPath(cancelPendingSwitch=1)`
         * 把在途切档打成 `status=3 canceled`，这就是"手动切档 100% 失败"的直接推手。
         *
         * mPauseFrameRestorePending：一次"有待补做的 PFR"的事件闩。
         *   · 切档在途（质量切换的唯一在途判据，见 qualitySwitchInFlight()）时不发 seek，
         *     只置闩并返回；
         *   · 在切档终态出口补做一次（READY/FAILED 走 finishQualitySwitch 那处，
         *     CANCELED 走 FlushVideoPath 那处），且**只补做一次**：先清闩再补发；
         *   · 补做时仍在暂停态才真的发（PFR 只在暂停态有意义），否则丢弃闩；
         *   · 补做时若又检测到新的切档在途，只重新置闩（推迟到它自己的终态），
         *     **不递归、不立即重试** ⇒ 不会 ping-pong；
         *   · 换片源/停止/Reset()/Prepare 一律清闩，不跨片源补做。
         *
         * 不记目标位置：补做时重新调用 RestorePausedVideoFrame()，它自己会从**当前**
         * 解码器读"最后渲染帧 pts" ⇒ 天然取到切档之后的新时间轴，不会用取消前的旧时间点。
         *
         * runDeferredPauseFrameRestore()：三个终态出口共用的唯一补做入口（无计时器）。
         */
        bool mPauseFrameRestorePending{false};
        void runDeferredPauseFrameRestore();
        /*
         * "切档在途"的唯一判据（PFR 让路 / 用户 seek 推迟都以它为准）。
         * 单解码器模型下它只有一项：`mVideoSwitchInFlight`。
         * 定义见 SuperMediaPlayer.cpp 里那个函数的注释。
         */
        bool qualitySwitchInFlight() const;

        /*
         * 【B20】切档在途时被推迟的用户 seek（"串行化"）
         *
         * 为什么必须串行而不是就地执行：切档自己的定位是 demuxer 级 seek（目标 = "当前播放位置"），
         * 用户 seek 重定位同一条活动读链路 —— **谁后执行谁生效**。安卓日志实证：
         * 用户 seek 到 135164000 us，随后落点窗口里出现的帧在 169886383 us，而紧跟着切档
         * `target stream 0 seeked to 169886383` —— 差 34722 ms，落点判据必然对不上，
         * 于是要么拒绝（画面冻住）、要么采纳错帧，切档本身也被这次 seek 打断成 canceled。
         *
         * 语义：**推迟而不是丢弃**。切档在途时只记下"最后一次"用户 seek 目标（后到覆盖先到），
         * 在切档三个终态（READY / CANCELED / FAILED）各补做一次（先清闩再 SeekTo）；
         * 补做走正常 SeekTo 流程，由既有 SeekEnd / NotifySeeking(false) 收尾。
         * 不递归：补做前先清闩；补做期间若又有切档在途，只重新置闩，等那一次终态再补。
         * 全部是状态判据，无计时器。
         */
        int64_t mDeferredUserSeekUs{INT64_MIN};
        bool mDeferredUserSeekAccurate{false};
        bool mDeferredUserSeekPending{false};
        void replayDeferredUserSeek();

        /*
         * 【追帧加速（seek 落点前缀）】
         *
         * 语义：只是把"跑在更高性能点"的要求下发给解码器 —— **不丢帧、不改时间轴、
         * 不动任何精度判据**。seek 之后必须从目标之前的关键帧起解，那段前缀本来就要
         * 全部解出来（现在也全部上屏，见 RenderVideo 的落点前缀分支），本开关只是让
         * platform codec 更快地把它们吐出来（安卓 = MediaCodec operating-rate）。
         *
         * setVideoDecodeBoost → 当前活动视频解码器
         *
         * 状态判据驱动：seek 真正开始置位、seek 结束/落点上屏收回；
         * 没有任何计时器。平台不支持时是空操作，所以核心层不需要任何平台宏。
         * 追加在类末尾，保证增量 ABI 安全。
         */
        void setVideoDecodeBoost(bool boost);

        /*
         * 【seek 延迟量化】用户那一刻（SeekTo 被调用的瞬间）的单调毫秒。
         *
         * 用途：把"这次 seek 到底慢在哪一段"变成日志里可比较的数字 ——
         *   · 画面第一帧上屏 / 落点帧被采纳 / 音频到锚点，各自打印"距 seek 请求多少毫秒"。
         * 真机实测（2026-09-27）：画面 95~220 ms，而音频要 0.19~6.6 s ⇒ "先没声后有声"。
         * 只服务诊断，不参与任何判据。追加在类末尾，保证增量 ABI 安全。
         */
        int64_t mSeekRequestMs{0};

        /*
         * 【ABR 让路】"当前有切档在途"的唯一对外查询（复用 qualitySwitchInFlight() 的单一判据）。
         * 追加在类末尾，保证增量 ABI 安全；语义见 ICicadaPlayer::IsStreamSwitchInFlight。
         */
        bool IsStreamSwitchInFlight() const override;

        /*
         * ============ 【P0：不连续点与代际机制（只写入，暂不改行为）】============
         *
         * beginDiscontinuity() —— 开启一个新代际：generation +1，targetUs = 参数，
         *   startUs 复位为"未知"。所有"新的一次 seek / 换档"都必须经它，
         *   这样后面每一处异步事件都能拿一个单调递增的代际号判断归属。
         * markDiscontinuityStartUs() —— 记下 demuxer 的真实落点（seek 后第一个包的位置）。
         *   INT64_MIN 表示尚未观测到落点。
         * discontinuityGeneration() —— 当前代际，只读。
         *
         * 声明追加在方法列表末尾（本文件约定），定义见 .cpp 末尾。
         */
        void beginDiscontinuity(int64_t targetUs);
        void markDiscontinuityStartUs(int64_t startUs);
        int discontinuityGeneration() const;

        /*
         * ============ 【P1-a：renderer 单一过滤规则（唯一实现）】============
         *
         * 判据只与"目标点"本身比较，与落点远近、GOP 长度、分片长度全都无关：
         *
         *   framePos + frameDur <= targetUs
         *       ⇒ 这一帧完全落在目标之前、不包含目标 ⇒ 返回 true（丢弃），
         *         过滤继续；
         *   否则（framePos <= targetUs < framePos + frameDur，即**包含目标**；
         *         或 framePos > targetUs，即目标落在本段之前、没有更早的帧可选）
         *       ⇒ 返回 false，并且**结束本次过滤**：记下 acceptedFramePos、
         *         把内容时间轴（mMasterClock）钉在 targetUs、filterActive 置假。
         *
         * 不变量：
         *   · 帧 PTS 单调前进 ⇒ 必然在有限帧内走到"包含目标"的那一帧，
         *     所以既不需要预算、也不需要任何超时 / 看门狗兜底；
         *   · 结束过滤**只**发生在本函数的结束分支，或下一次
         *     beginDiscontinuity（seek / Reset / Prepare）。SeekEnd 不算 ——
         *     这正是分片源"seek 永远差一个落点前缀"的根因所在。
         *
         * 调用约定：本函数是**纯判据**，但调用方（RenderVideo）必须**无条件**先调它一次 ——
         * 因为"某一帧恰好包含目标"时结束过滤的副作用就发生在这一次调用里；若因为别的分支
         * 而跳过调用，过滤就永远不会结束（P1-c 之前正是这个形状）。
         *
         * **没有"第一帧无条件出画"这条例外**（P1-c 删除）：早于目标的帧绝不上屏，
         * 否则首个上屏帧就不是"包含目标的那一帧"。删掉它不会黑屏 ——
         *   · clearScreen() 在全文件只有两处调用（SuperMediaPlayer::ClearScreen() 与
         *     stop 且 mSet->clearShowWhenStop 为真时），seek / FlushVideoPath 路径**不清屏**；
         *   · FlushVideoPath() 走 flushDevice() / decoder->flush()，其 flushRender=false
         *     分支的既有注释本来就写明"渲染器里缓存的旧帧下一帧会被覆盖"。
         * 所以"目标帧到达前不上屏"只会让画面**停在上一张**。seek 的结束改由落点帧的渲染
         * 触发（doRender 里 videoDecoder->isRenderGateHit() 命中），而这必然发生：
         * 目标钳位保证"包含/越过目标"的帧存在，另有 RenderVideo 的 EOF 采纳，
         * 以及 playCompleted() 自己清 mSeekFlag。
         *
         * 过滤未激活（targetUs 未知，或本次已经结束）⇒ 直接返回 false 且无副作用，
         * 也就是正常播放路径上一行行为都不变。
         *
         * 声明追加在方法列表末尾（本文件约定），定义见 .cpp 末尾。
         */
        bool shouldDropForDiscontinuity(int64_t framePos, int64_t frameDur);

        /*
         * ============ 【P1-b：采纳落点帧（唯一的收尾动作）】============
         *
         * 两个调用者：shouldDropForDiscontinuity() 的"包含目标 / 越过目标"分支，
         * 以及 RenderVideo() 的 **EOF 结构性终止**分支（"过滤仍激活但不会再有帧了"）。
         *
         * 动作（每个都是"把不连续点收敛掉"所必需的）：
         *   · 记下 acceptedFramePos（诊断）；
         *   · **把内容时间轴钉在 targetUs**（不是落点帧的位置）—— 用户要的是
         *     "位置 == target 且之后单调"：mMasterClock.GetTime() 就是那根轴（有音频时由
         *     设备已消费量驱动，无音频时自走），位置上报与渲染节拍共用它，见
         *     getCurrentPosition() 的说明；seek 一发起 ProcessSeekToMsg 已经把主时钟钉在
         *     seekPos，所以这一步在正常路径上是幂等的；
         *   · filterActive = false —— 这就是"结束本次过滤"的**唯一**写点。
         *
         * reason 只进日志（例如 "no frame can reach the target (eof)"）。
         * generation 由调用方**先** load 一次并传进来：它是"这一帧属于哪一次不连续"的
         * 归属证据，同时保证调用方在读非原子的 targetUs 之前已经建立了与写入侧的配对。
         * 声明追加在方法列表末尾（本文件约定），定义见 .cpp 末尾。
         */
        void acceptDiscontinuityLandingFrame(int64_t framePos, int generation, const char *reason);

        /*
         * ============ 【P2：音频时钟基准的唯一写点】============
         *
         * 把"音频时钟的内容位置基准"钉在 baseUs，同时记下此刻"设备已消费量"的快照；
         * 之后 getAudioPlayTimeStamp() 给出 baseUs + (设备已消费 - 快照)，单位微秒、
         * 与 targetUs 同一根轴。baseUs == INT64_MIN 表示作废（音频时钟暂不可用，
         * 主时钟退回自走）。快照取不到有效值时写 INT64_MIN = "待惰性补锚"。
         *
         * 调用点：FlushAudioPath()（设备 flush 之后，唯一的主写点）、RenderAudio() 的
         * 首帧兜底、getAudioPlayTimeStamp() 的 delta<0 自愈。
         * 声明追加在方法列表末尾（本文件约定），定义见 .cpp 的 FlushAudioPath 之后。
         */
        void pinAudioClockBase(int64_t baseUs);

        /*
         * 【音频落点地板诊断限频】"落点地板丢掉一帧音频"的日志已经为哪个代际打过。
         *
         * RenderAudio 在音频落点地板激活时（audioLandingPending 为真，或视频的落点过滤
         * filterActive 仍为真）会丢掉完全落在目标点之前的音频帧；那条日志按**代际**限频
         * （纯状态比较，不是计时器），于是用户能把"音频被正确地丢到目标点"与
         * "音频根本没来"区分开，又不会刷屏（本地文件一次 seek 实测前缀音频约 49 个
         * AAC 帧 = 1.14 s，逐帧记会淹掉真正重要的行）。每次新的不连续点在
         * beginDiscontinuity() 里复位成 -1。
         * 追加在成员列表末尾（本文件约定：只有追加才是增量 ABI 安全的）。
         */
        int mAudioLandingDropLoggedGen{-1};

        /*
         * ============ 【P3：单解码器切档的"挂着的切换请求"（新模型的唯一在途判据）】============
         *
         * mVideoSwitchInFlight   —— 是否有一次切档请求正在生效。由
         *   `SMPMessageControllerListener::switchVideoStream()` 在发出 STARTED 前置真，
         *   由 `finishQualitySwitch()` 在终态（READY / FAILED / CANCELED）清零。
         *   本工程"切档在途"只此一个判据：`qualitySwitchInFlight()` 与
         *   `ICicadaPlayer::IsStreamSwitchInFlight()`（ABR 让路）都返回它。
         * mVideoSwitchTargetIndex —— 目标档索引；READY 通知与日志用它（终态时清 -1）。
         *
         * 双解码器那套"切档在途"曾经由 9 个成员拼出来；单解码器切档是**立即切换**
         * （关旧流 → 开新流 → 按流 seek → 只 flush 视频 → 重建同一块解码器 → 落点过滤
         *  → 落点帧上屏即 READY），所以只需要这两个字段。
         * 追加在成员列表末尾（本文件约定：只有追加才是增量 ABI 安全的）。
         */
        bool mVideoSwitchInFlight{false};
        int mVideoSwitchTargetIndex{-1};

        /*
         * 不连续点本体。**追加在成员列表最末尾**：本工程增量构建不记录头文件
         * 依赖，插在中间会移动其后成员的偏移，让旧的 friend TU 目标文件按错
         * 偏移访问（见本文件里"以下这些成员必须留在成员列表的最末尾"那段）。
         */
        Discontinuity mDiscontinuity{};

        /*
         * ============ 【应用层视频编码"硬解能力 + 效率偏好"的状态】============
         *
         * 这是本播放器实例"应用层传进来的那一份"（CicadaSetVideoCodecSupport 的
         * 参数解析结果）。对象**发布后不再改动**（改 = 造一份新的再发布），所以：
         *   · ABR 线程 / 起播回调线程只用 decoderFactory 里那把锁取到的 shared_ptr
         *     快照，不读本成员 ⇒ 它们与 API 线程之间没有共享可变状态；
         *   · 本成员自己由 mAppCodecSupportMutex 保护，只在 set（API 线程）与
         *     析构（清覆盖值）时读写。
         * 没有计时器、没有看门狗：清除只由 set("") / 析构这两个事件触发。
         *
         * 追加在成员列表**最末尾**（本工程硬规则：只有追加才是增量 ABI 安全的，
         * 见文件顶部那段说明）。
         */
        std::mutex mAppCodecSupportMutex;
        std::shared_ptr<const decoderFactory::AppCodecSupport> mAppCodecSupport;

        /*
         * 【落点过滤诊断限频】"落点过滤丢掉一帧"的日志已经为哪个代际打过。
         *
         * RenderVideo 的单一落点过滤（shouldDropForDiscontinuity）在"完全落在目标之前"时
         * 会丢掉这一帧；前缀帧可能有几百上千张，逐帧记只会淹掉真正重要的那几行。
         * 这里按**代际**限频（纯状态比较，不是计数器、不是计时器），一次不连续点至多一行 ——
         * 而这一行正好回答了上次真机死锁里唯一缺的那个问题：
         *   **判据用的那个"帧位置"与目标点是不是同一根轴**。
         * （真机 2026-09-27 那次暂停态 seek 就是因为两者不同轴，过滤永不结束，
         *  日志里只有 812 条 drop frame、0 条 accepted，看不出原因。）
         * 追加在成员列表**最末尾**（本工程硬规则：只有追加才是增量 ABI 安全的）。
         */
        int mVideoLandingDropLoggedGen{-1};

        /*
         * 【本次 seek 是否需要重新臂上音频落点地板】—— SeekTo() 置真，
         * beginDiscontinuity() 消费一次即清。
         *
         * 为什么不直接在 beginDiscontinuity() 里按 targetUs 判断：beginDiscontinuity()
         * 也被**换档**（switchVideo -> beginDiscontinuity(switchPos)）和 Reset 调用，
         * 而换档不 flush 音频、音频时间轴连续，臂上地板会把音频永久闸住。
         * 只有 SeekTo() 这条路（以及经由它派发到 ProcessSeekToMsg 的同一次 seek）
         * 会调 FlushAudioPath() -> pinAudioClockBase(targetUs) 重新锚定音频时间轴，
         * 所以"要不要臂地板"这件事只能由 seek 入口自己声明。
         *
         * 并发：写侧是 API 线程（SeekTo），读侧是消息线程（beginDiscontinuity），
         * 与 mSeekAudioAlignDone / mSeekDecodeStartIsKey 同一组、同一个访问节奏
         * （seek 派发前后各一次），沿用本工程既有的写法，不新增同步原语。
         * 追加在成员列表**最末尾**（本工程硬规则：只有追加才是增量 ABI 安全的）。
         */
        bool mSeekAudioLandingReset{false};

        /*
         * ==================== 【播放缓存 play-and-cache 的状态】====================
         *
         * 三者都**追加在成员列表最末尾**（本文件硬规则：只有追加才是增量 ABI 安全的，
         * 见文件顶部那段说明）。
         *
         * 【必须用 ENABLE_CACHE_MODULE 包起来】缓存模块整体由 CMake 的
         * ENABLE_CACHE_MODULE 控制（mediaPlayer/CMakeLists.txt:54-55，值来自
         * framework/module_config.cmake），关掉它的构建里连 CacheManager 类型都不存在，
         * 不留条件地把成员写在这里会直接编不过（MediaPlayer.cpp 里那些 #ifdef
         * ENABLE_CACHE_MODULE 就是同一件事在实现层的写法）。
         * 上面两个虚函数的**声明**不受影响：它们由 ICicadaPlayer 无条件声明、默认实现是
         * 空操作，所以关掉缓存模块时它们照样存在（实现里的 #ifdef 只让函数体变空）。
         *
         * 关于成员布局：SuperMediaPlayer.h 只被 mediaPlayer 目录下的 TU 包含
         * （SuperMediaPlayer.cpp / CicadaPlayerPrototype.cpp /
         * SuperMediaPlayerDataSourceListener.cpp / SMPMessageControllerListener.cpp /
         * SMP_DCAManager.cpp，都在同一个 media_player 目标里），它们看到的
         * ENABLE_CACHE_MODULE 一定一致，所以这里的条件成员不会造成"不同 TU 看到不同
         * 布局"。反过来，如果以后有别的 CMake 目标直接包含本头文件，必须保证那个目标
         * 上的 ENABLE_CACHE_MODULE 取值与本目标一致。
         *
         * mCacheConfig   应用层通过 SetCacheConfig 传进来的那一份（字段见 CacheConfig.h）。
         *                默认 mEnable = false ⇒ 不开缓存时下面两行完全不参与任何行为。
         * mCacheManager  本实例的缓存管理器；**只在 SetDataSource(const char *) 里创建**，
         *                在 Stop() / 析构里 stop + 释放。它同时持有那个把播放器当数据源
         *                的 ICacheDataSource 对象（CacheManager 析构时 delete），所以它的
         *                生命周期必须覆盖整个播放期 —— 见 .cpp 里
         *                SuperMediaPlayerCacheDataSource 的说明。
         *
         *                用 shared_ptr 而不是裸指针是有理由的：喂包发生在**播放线程**
         *                （读包路径每一包一次），而缓存管理器是**API 线程**在
         *                SetDataSource / Stop 里换掉并释放的。播放线程只取一份
         *                shared_ptr 快照（cacheManagerOrNull()），拿着这份快照喂完这一包
         *                才可能让管理器析构 —— 于是"拿到指针之后、用之前被换掉"这条
         *                use-after-free 在类型上就不可能发生（换成裸指针时它要靠调用
         *                时序去保证）。取快照的临界区只有一次拷贝，不持锁做任何 I/O，
         *                因此与 mCreateMutex 之间不存在锁序问题。
         * mCacheSuccess  缓存文件已经写成（CacheManager 的成功回调触发过）。语义是
         *                "这一份缓存已经完整"：成功回调之后 remuxer 线程已经退出
         *                （CacheModule::streamEnd 让 mFrameEof 置真 → mux 线程收尾并
         *                退出），而 CacheModule::addFrame 仍会把包 clone 进它那条只进
         *                不出的队列 —— 所以本类必须靠这个标志停止喂包
         *                （见 .cpp 的 sendMediaFrameToCache）。SetDataSource / Stop 复位。
         *                它**不**参与循环语义：本类的循环是 playCompleted 里原地 seek 回 0，
         *                没有"播完重设源"那条路径，改 bLooping 只会让播放停住。
         *
         * mCacheMutex    只保护 mCacheManager 这一个 shared_ptr 的读写（换入/换出/取
         *                快照），不保护管理器内部状态（它自己有一套锁）。
         */
#ifdef ENABLE_CACHE_MODULE
        CacheConfig mCacheConfig{};
        std::shared_ptr<CacheManager> mCacheManager{};
        std::atomic<bool> mCacheSuccess{false};
        std::mutex mCacheMutex{};
        std::shared_ptr<CacheManager> cacheManagerOrNull();
        void sendMediaFrameToCache(const IAFPacket *frame, StreamType type);
        void ReleaseCacheManager();
#endif

        /*
         * 【切档时 meta 还不完整 ⇒ 把"原地重建解码器"推迟到新档第一个包】
         *
         * 真机现象（2026-09-30 22:02，DASH output.mpd，h265-854_480 → h265-1920_1080）：
         * 切档那一刻 DashStream::GetStreamMeta() 里 mPDemuxer 还是空的（新档的 init 段要等
         * 读线程 ~200 ms 后才解析出来），拿到的 meta 只有清单信息、**extradata 为空**；
         * 此时重建 VideoToolbox 会话只能沿用旧档的参数集 ⇒ 新档每个关键帧都回
         * kVTVideoDecoderBadDataErr(-12909) ⇒ 解码器在 ~3 s 里丢掉 1000+ 帧 ⇒ 读线程一路
         * 冲到第 8 片（42 s）⇒ 主时钟才 3.7 s，read-ahead gate 判定"视频超前 38 s"把视频
         * 掐死 ⇒ 画面卡住、随后闪退。内容本身没问题（已核对分段 tfdt：第 1 片 0 s、
         * 第 8 片 42.042 s；MPD 六个档共用同一条 SegmentTimeline）。
         *
         * 处置：meta 缺 extradata 时**不当场重建**，只置这一位；新档第一个视频包到达时
         * （那时 init 段早已解析完、meta 完整）再重建。**纯事件驱动**：没有计时器、
         * 没有阈值、没有重试。追加在成员列表最末尾（本工程硬规则：只有追加才是增量
         * ABI 安全的）。
         */
        bool mVideoDecoderRebuildPending{false};
    };
}// namespace Cicada
#endif// CICADA_PLAYER_SERVICE_H
