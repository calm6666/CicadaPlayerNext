#ifndef CICADA_PLAYER_SERVICE_H
#define CICADA_PLAYER_SERVICE_H

#include <string>
#include <atomic>

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
         * 静音窗口起止日志（只在状态变化时各一条；不改动任何播放行为，
         * 编码见 AUDIO_SILENCE_* 与成员 mAudioSilenceReason）。
         */
        void logAudioSilence(int reason, const char *detail, int64_t audioPts, int64_t clockUs);

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
         * 把**目标（pending）Representation 的 codec 参数集**（SPS/PPS/VPS，
         * 即 demuxer meta 里的 extradata）贴到即将送入 pending 解码器的包上。
         *
         * 为什么需要它（马赛克根因，2026-09-24）：
         *   预滚跳过（见 ProcessVideoPacket 里那段）会把目标流开头的一批包整批
         *   丢掉，而"携带 extradata 的那一条"往往就在里面 —— 框架是在目标流
         *   第一个包上 setExtraData 的。丢过之后，新解码器拿到的第一个关键帧
         *   没有参数集，解不出来（`Error while decoding frame -1094995529 :
         *   Invalid data found when processing input`），P 帧参考不上参考帧，
         *   画面就是"人物糊成马赛克块"。
         *
         * 原来只有"等到参考点之后的关键帧"那一条分支贴了参数集，**兜底回退分支
         * （等太久、退而接受更早的关键帧）没贴** —— 而日志里两处马赛克簇
         * （22:51:00.790 / 22:52:09.646）正好紧跟在那条回退日志之后。所以这里
         * 抽成公共函数，两条分支共用，语义上不可能再漏。
         *
         * 返回 true 表示本次确实贴上了参数集（供调用处判断要不要 log）。
         */
        bool attachPendingVideoCodecParams();

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
         * ============ seek 期间主时钟锚点规则的**唯一**实现（两处锚点共用）============
         *
         * 起因（实测）：一次 seek 里主时钟被锚到"seek 之后第一张上屏的视频帧"，
         * 而那张帧是**旧时间轴**上的（目标 2.844s，锚到的帧 0.083s）。锚点一错，
         * 时钟就钉在 0.083s 上按 1x 往前走，位置上报与追赶窗口全部按错的时间轴判，
         * 这一整段 seek 的落点判据（mSeekPositionFloorUs）也一起失效。
         *
         * 规则（一次 seek 只允许锚一次，且只允许下面两类量）：
         *   · 目标时刻 mSeekPositionFloorUs（由 seek 入口写入，等于用户目标）
         *     —— "位置必须等于目标"的直接实现；
         *   · 或者**时间位置 >= 目标（容差内）**的落点帧（视频落点帧 / 音频落点首帧）。
         * **绝不**允许用早于目标的帧或包去锚：那正是上面 0.083s vs 2.844s 的来源。
         * 落点帧比目标早（稀疏 IDR 的正常情况）时锚到目标，画面的精确性由落点帧
         * 上屏事件负责，不靠把时钟拉回去。
         *
         * 返回：这一次可以采用的锚点值（>= 目标）；返回 INT64_MIN = 这一帧/包不符合
         * 规则，锚点事件必须保留给后面的帧（两处调用都不许消费 mSeekAnchorPending）。
         *
         * 实现见 SuperMediaPlayer.cpp 末尾。声明追加在方法列表末尾（本文件有
         * "新成员/新声明一律追加在最后"的约定）。
         */
        int64_t fetchSeekClockAnchorUs(int64_t frameUs, const char *why);

        /*
         * ================= 暂停态无感切档（2026-09-24，跨平台）=================
         *
         * 用户要求"暂停中 / 播放中都要能无感换清晰度"，而且"切档在途时暂停、
         * 暂停时切档、seek 与切档叠加"等组合都要收敛。播放中那条路本来就能走
         * （旧路继续出画、新路预热完 promote）；**卡死的是暂停态**，因为它有
         * 四道门全部以"主时钟在走"为前提：
         *
         *   门1 渲染 doRender() 只在 PLAYER_PLAYING 调 render()；
         *   门2 提交 TryCommitPendingVideoSwitch() 要求"帧时间 <= master + 150ms"；
         *   门3 预滚只接受"参考点之后的关键帧"，且参考点取自主时钟；
         *   门4 终态 checkQualitySwitchDeadline() 非 PLAYING 时无限延期。
         * 主时钟在暂停时是冻结的，于是 1/2/3 一起把状态机钉在 decoderSwitch：
         * 实测 [switch] 关键量 28.6 秒逐字节不变、18s 超时被拖到 33.3s，
         * 最后是外层预编译 PlayerBase 的 baseTimeout() 把播放器 stop() 掉。
         *
         * 解法：切档发起时**快照**一个冻结的参考时刻（mPausedSwitchPivotUs），
         * 所有"和 master 比"的判据在暂停态改用它；预滚起点改成"暂停点之前
         * 最近的关键帧"（不必读空队列去够后面的关键帧）；提交后由 doRender()
         * 在暂停态渲染**恰好一帧**到达终态。全程不动主时钟、不发位置回调。
         */

        /*
         * 本次切档是否发起于暂停态。true 时上面那四条改走"冻结参考点"路径。
         * 只在发起时写一次，切档终态（成功/失败/被 seek 中止）时清零。
         * 【成员变量本身定义在类成员列表的末尾，见文件下方同名声明处的说明 ——
         *   本工程增量构建不做头文件依赖，新成员必须追加在成员列表最后，
         *   不能夹在方法声明中间。】
         */

        /*
         * 暂停态切档的**冻结参考时刻**（微秒，媒体时间轴）。
         * 取发起切档那一刻的 getCurrentPosition()，之后不再推进 —— 这正是
         * "暂停"的语义：时间不走了，所以判据要拿一个不动的值去比。
         */

        /*
         * 提交之后、暂停态还欠"恰好一帧上屏"。doRender() 看到它为真就反复调
         * RenderVideo(true)，直到有一帧真的送出（或撞上墙钟死线）。
         * 不是周期渲染：一旦有一帧上屏（或切换终态）立刻清假。
         */

        /*
         * 提交前墙钟死线（steady ms）：**只要有任何在途切换**就必须有到点的出口。
         *
         * 为什么必须有：Android 侧预编译的 PlayerBase 会在 ~545ms 内就
         * baseTimeout()+stop()，而切换在途的判据一旦依赖主时钟（暂停时不动），
         * 就等于没有出口 —— 用户看到的是"点了清晰度之后整个播放器卡死"。
         */

        /*
         * 是否有在途的清晰度切换（提交前或提交后）。给墙钟死线、复位点、
         * 以及"seek 要不要中止切档"共用同一个判据，避免三处各写一份而漏形态。
         */
        bool isQualitySwitchInFlight() const
        {
            return mPendingVideoStreamIndex >= 0 || mWillChangedVideoStreamIndex >= 0 ||
                   mQualitySwitchCommitPending;
        }

        /*
         * 提交前的墙钟死线。由 ProcessVideoLoop 每轮调用（紧挨着原有的
         * checkQualitySwitchDeadline），覆盖每一种"在途"状态：暂停切档在途、
         * 播放中切档在途、seek 在途、以及它们的叠加。到点即
         * finishQualitySwitch(false, ...) —— 必有终态。
         */
        void checkQualitySwitchPrerollDeadline();

        /*
         * 切档发起时的统一快照（暂停/播放都用它，保证两条路状态初始化一致）。
         * 只记录，不动主时钟、不动位置回调。
         */
        void beginQualitySwitchTracking();

        /*
         * 清掉上面三个新状态。所有"切档终态"的出口都必须调它：
         * finishQualitySwitch()、切档超时失败、FlushVideoPath()、Reset()。
         */
        void resetPausedSwitchState();

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
            FLOOD_DECODE_STALL,       /* "还在等 seek 的第一个关键帧"导致的停滞判据跳过 */
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
         * pending 解码器这一轮是否已经贴过目标流的参数集（SPS/PPS）。
         * 由 attachPendingVideoCodecParams() 维护：只在"还没贴过"时贴一次，
         * 避免每个包都重复 setExtraData（Java 侧每帧一次 JNI 拷贝，不划算）。
         * 每次 CreatePendingVideoDecoder / FlushVideoPath 时复位。
         * 同样**追加在成员列表末尾**（见上面那段增量构建的说明）。
         */
        bool mPendingVideoCodecParamsAttached{false};

        /*
         * 暂停态切档的状态（声明与完整说明见上面那一段）。**追加在成员列表末尾**
         * （见本文件里"增量构建不做头文件依赖、新成员一律追加在最后"那段约定）：
         * 插在中间会移动其后成员的偏移，让旧的 friend TU 目标文件按错偏移访问。
         */
        bool mSwitchStartedWhilePaused{false};
        int64_t mPausedSwitchPivotUs{INT64_MIN};
        bool mPausedSwitchRenderPending{false};
        int64_t mQualitySwitchPrerollDeadlineMs{0};

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
         * ============ seek 落点与"只锚一次"（见 docs/PLAN-SEEK-FAST-LANDING-CROSSPLATFORM.md K1/K2）============
         *
         * 三者分工必须分清，这是本轮修 seek 的核心：
         *   mSeekPositionFloorUs    只管"对外上报的位置"（进度条不回退），语义不变；
         *   mSeekRenderGateUs       只管"渲染闸门"：本次 seek 的落点帧还没上屏时用它挡帧，
         *                           落点帧一被接受立刻撤掉（之后一帧都不再挡）；
         *   mSeekLandingFrameAccepted  本次 seek 是否已接受落点帧（纯闩锁，SeekTo 复位）。
         *
         * mSeekClockAnchored 是"一次 seek 只允许锚定一次"的闩：音频首帧与视频落点帧谁先到谁锚，
         * 后到的那条不再改写主时钟（否则后到的帧会被新时钟判成迟到帧丢掉）。
         *
         * mSeekExactLanding 保留旧语义（必须精确到目标帧、目标点之前的帧一律不上屏）的开关，
         * 默认 0 = 落点即上屏；需要旧行为时由各端 setOption("seekExactLanding", "1") 打开。
         *
         * 全部追加在成员列表末尾（本文件顶部有约定：中间插入会移动偏移、破坏增量构建）。
         */
        int64_t mSeekRenderGateUs{INT64_MIN};
        bool mSeekLandingFrameAccepted{false};
        bool mSeekClockAnchored{false};
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
         * A 方案（seek 落点语义）：本次 seek 的视频落点帧是否已经**采纳并锚定主时钟**。
         * 为假时：音频不许把时钟接走（见 render 里"音频等时钟"的门与音频锚定分支），
         * 这样时钟保持在落点关键帧的 PTS 上按 1× 前进，视频不背"目标点 − 落点"那笔债。
         */
        bool mSeekVideoAnchorDone{false};
        /*
         * C 方案：本次 seek 是否已经把音频**起点对齐到视频落点**（一次性闩）。
         * 事件：读到本次 seek 的第一个关键帧视频包（= 落点 PTS 已知）。
         * 动作：把音频包队列裁到落点并 flush 音频路，让音频解码从落点重新开始 ——
         * 这样 A 的"音频等时钟"不再需要（无 ≤1 个 GOP 静音），A/V 内容也对齐。
         * 失败/不支持（音频队列里没有落点及之后的数据）就什么都不做，回落到 A。
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
         * C 方案：音频"落点地板"。落点 PTS 已知后，PTS 早于它的音频帧不再推给设备，
         * 使音频与视频从同一落点开始（不留债务、不出静音）。首帧音频渲染后清零。
         */
        int64_t mSeekAudioFloorUs{INT64_MIN};
        /*
         * 【第 4 条】落点对齐期间的"音频连续性高水位"（微秒，INT64_MIN = 未启用）。
         *
         * 只靠"低于落点就丢"挡不住旧时间轴残留：seek 之后音频解码器**内部**还压着旧
         * 时间轴的数据（ActiveDecoder 输入队列 ≤16 包 + 输出队列 ≤10 帧，约 0.5s 内容），
         * 它们的 PTS 位于 seek 之前的位置（实测高出落点 8.8~11.9s：落点 20.833s 却报出
         * 29.674s → reSync + 约 230 帧丢帧）。而新时间轴的音频从落点开始**连续**到达
         * （实测包队首 = 落点 + ≤21ms）。所以对齐期间再加一条连续性判据：
         *   PTS 高出本高水位超过 SEEK_AUDIO_CONTINUITY_TOLERANCE_US 的帧 = 旧时间轴残留，丢；
         *   其余帧接受，并把高水位推进到该帧。
         *
         * 与 mSeekAudioFloorUs 同生共死：C 对齐事件置位、首帧音频上屏即撤除 ——
         * 对齐窗口一结束就不再干预正常音频（不会误伤真正的时间戳跳跃或缺包）。
         */
        int64_t mSeekAudioContinuityUs{INT64_MIN};
        /*
         * 【第 4 条的安全上界】对齐期间因为"高出高水位"被丢掉的帧数。
         * 旧时间轴残留是可数的（解码器输入 ≤16 包 + 输出 ≤10 帧），所以最多丢
         * SEEK_AUDIO_STALE_DROP_MAX 帧就必须放行 —— 万一新时间轴本身在这段窗口里
         * 有一个 >200ms 的正常空隙（编辑列表/丢包），也绝不会把音频饿死到"窗口永不关闭"。
         * 纯计数，不是时间判据。
         */
        int mSeekAudioStaleDrops{0};
        /*
         * 音频静音窗口的**状态**（0 有声 / 1 seek 窗口内等主时钟 / 2 设备写失败 /
         * 3 设备被 flush 重建）。只用于"起止各一条"的日志，不参与任何管线决策：
         * 判据全是状态变化，没有时间阈值，也没有周期性动作。
         */
        int mAudioSilenceReason{0};
        /*
         * 【第 2 项：有界精确落点】本次 seek 是否"值得精确到目标帧"。
         * 事件：读到本次 seek 的第一个关键帧视频包时，若它离目标点不超过
         *       SEEK_EXACT_LANDING_BUDGET_US，就置真（否则保持假 = 照旧立刻上屏落点帧）。
         * 作用：RenderVideo 的落点采纳分支在该闩为真时，会把"仍比目标早超过预算"的帧
         *       挡在门外（render=false），直到解码器走到目标附近才采纳/锚定 ——
         *       帧 PTS 单调前进，所以必然终止，不需要任何计时器。
         * 与用户选项 mSeekExactLanding 相互独立：那是全局策略（同一套旧语义，只在采纳
         * 之后挡帧），这里只是"这一次 seek 的目标点离落点够近"的预算判断。
         * 每次 SeekTo 与 Reset 都清零，绝不让上一次 seek 的判断影响下一次。
         */
        bool mSeekExactLandingByBudget{false};
        /*
         * 【锚点事件闩，修"seek 没反应"】"本次 seek 之后第一帧真的上屏"这个事件还没被消费。
         * SeekTo 置真；doRender 里一旦 rendered 就消费它，把主时钟锚到 mPlayedVideoPts
         * （刚上屏那一帧自己的 PTS），只锚一次。
         * 独立成事件的理由：落点采纳依赖 frameTimePosition >= 0，部分容器/流该字段缺失时
         * 它永远不发生 —— 锚点与 seek 结束都不能绑在它身上（上一轮绑上去，导致安卓 seek
         * 永久结束不了、一直转圈）。
         */
        bool mSeekAnchorPending{false};
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
         * 【A 方案：音频流重新定位到落点】事件闩与数值。
         * 置闩处：renderAudioFrame() 里"音频时间轴在一个 seek 之后前跳 > 1s"那一条 ——
         *   这是"容器把音频放到了别处（不是视频落点）"唯一可观测的信号（安卓实测：
         *   视频落点 110.110s，而音频的下一帧是 112.512s，前跳 2026 ms）。
         * 消费处：主循环 ProcessVideoLoop() → DoCheckBufferPass() 之前（消息线程）。
         * 消费结论：解复用层目前**没有**按流重定位的通路 ⇒ 记一条限频日志，并保持既有
         *   防线（1b 清已解码帧队列 + 重基时间轴、双侧地板 + 32 帧上限）。证据见消费处注释。
         * 读包/渲染路径只置闩，**绝不**调解复用器（禁止重入与跨线程占用）。
         */
        bool mSeekAudioRepositionPending{false};
        int64_t mSeekAudioRepositionUs{INT64_MIN};
        int64_t mSeekAudioRepositionJumpUs{INT64_MIN};
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
         * B2：当前 pending（切档目标）解码器是不是用"1x1 占位 Surface"配置的
         * （= tunnel 渲染器带 FLAG_DUMMY 且 view 非空时，由 CreatePendingVideoDecoder
         * 连同 DECFLAG_PLACEHOLDER_SURFACE 一起置真）。提交
         * （TryCommitPendingVideoSwitch）据此走 placeholder 交接：先让旧解码器交出
         * 真 Surface，确认新解码器接上之后才 promote 并释放旧解码器；接不上就原地回滚。
         *
         * SeekTo / Reset / FlushVideoPath 三处必须随 pending 状态一起复位 ——
         * 漏一处就会跨 seek / 跨片源残留，让下一次提交误判"pending 是 surface 模式"。
         *
         * 追加在成员列表末尾：本文件有"增量构建不做头文件依赖、新成员一律追加在最后"
         * 的约定，插在中间会移动其后成员的偏移，让旧的 friend TU 目标文件按错偏移访问。
         */
        bool mPendingDecoderUsesPlaceholderSurface{false};

        /*
         * 【纯诊断，两个值都不参与任何判定；同样追加在成员列表末尾】
         *
         * mPendingVideoPrerollKeyPts：预滚实际开始解码的那个关键帧的 raw pts
         *   （在两个接受分支里记录：正常"参考点之后的第一个关键帧"与"等太久回退"）。
         * mQualitySwitchCarriedFramePts：提交（promote）时从 pending 帧队列带进
         *   active 队列的那一帧的 pts（每次提交先置 INT64_MIN，避免跨次残留）。
         *
         * 用途只有一个：RenderVideo 里交接后第一帧真的进真面时打
         *   `post-handover first frame: pts=… prerollKeyPts=… carriedPendingPts=…`
         * 一眼判定"交接后上屏的是 preroll 那一代（交接前就解好的帧，~百 ms 出画）
         * 还是交接后新解出来的帧"，以及从提交到上屏的真实毫秒数。
         * 坐标轴：mQualitySwitchCarriedFramePts 与上屏帧的 pts 同轴（都是流水线
         * 归一化后的），可以直接比较；mPendingVideoPrerollKeyPts 是**包的原始 pts**，
         * 与它们相差一个 mActiveVideoPtsOffset，只作"离预滚关键帧多远"的参考。
         * 之所以要这一行：上一版在这里加了 flush，逼出 6.4s 等关键帧 + 4.2s 等时钟
         * （实测 10.6s 静止）。有了这三个 pts，同一份日志就能当场判定走了哪条路。
         */
        int64_t mPendingVideoPrerollKeyPts{INT64_MIN};
        int64_t mQualitySwitchCarriedFramePts{INT64_MIN};

        /*
         * ============ B4（两条渲染路）：active 视频解码器**实际绑定的输出面** ============
         *
         * 就是 `CreateVideoDecoder()` 里算出来的那个 view：
         *   · 隧道（FLAG_DUMMY，解码器直出）：= App 的 Surface（`mSet->mView`）；
         *   · GL（GLRender）：= 渲染器 SurfaceTexture 的那块 Surface（`GLRender::getSurface()`）。
         *
         * 唯一用途：切清晰度的"占位 Surface 交接"（B2）必须把**真面**从旧解码器交给
         * pending 解码器。GL 路的"真面"不是 App 的 view —— 拿 `mSet->mView` 去
         * setOutputSurface 一定失败（那块面属于 EGL，不是 codec 的输出面），
         * 这正是"两条路要各自记住自己的真面"的原因。
         *
         * 生命周期：`CreateVideoDecoder()` 一开始置 null（失败即保持 null ⇒ 交接退回
         * 纯 promote），成功后写入；解码器被销毁的路径（Reset / closeVideo）也置 null。
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
         * ============ 【B12】切换预滚"两路代价取小"的状态（2026-09-26）============
         *
         * 预滚起点有两条路（见 SuperMediaPlayer.cpp 里那段决策）：
         *   · 等参考点**之后**第一个关键帧：不解码，等待 = nextKey - ref（≤ 一个关键帧间隔）；
         *   · 从参考点**之前**最近的关键帧起解、丢前缀追上时钟：代价 = (ref - lastKey) / (rate - 1)。
         * 在预滚入口比较两者、选小的那条。
         *
         * mPendingVideoDecodeRateMilli 是"目标路解码速度 / 实时"的千分比，每次提交后用它这一次
         * 真正解掉的媒体长度 ÷ 墙钟实测更新；墙钟从**切换请求**起算（含 open/seek），因此只会
         * 低估速率 ⇒ 偏向"等关键帧"（今天的行为），不会因为高估而选错路。
         *
         * mPendingVideoPrerollPathChosen 保证选路**只在预滚入口做一次**（每次切换请求处复位）：
         * 否则包队列一前进就改主意，会和"已经丢掉的前缀"打架。
         * 两个成员一律追加在类末尾（本文件顶部有硬约束：中间插入会让别的 TU 的偏移对不上）。
         */
        int mPendingVideoDecodeRateMilli{1900};
        bool mPendingVideoPrerollPathChosen{false};

        /*
         * ============ 【B14】音频时钟"设备位置真的前进过"才交出（事件驱动，无计时器）============
         *
         * 背景（安卓真机，本仓库两处注释都记过这个形态）：
         *   · AudioTrack 在 seek 窗口内会经历 PAUSED → FLUSHED/STOPPED → PLAYING，
         *     而 `AudioTrackRender::device_get_position()` 在**非 PLAYING/PAUSED** 时
         *     `getDevicePlayedSimples()` 返回 0（framework/render/audio/Android/AudioTrackRender.cpp
         *     的 `if (state == PLAYSTATE_PLAYING || state == PLAYSTATE_PAUSED)` 那一支），
         *     于是上报的位置会**塌回 0 / 向后跳**；
         *   · 这个值一旦被 SystemReferClock::GetTime() 拿去做**双向** reSync
         *     （mediaPlayer/system_refer_clock.cpp 的 `mClock.set(referTime)`），主时钟会被
         *     向后拉，视频随即被判"未来"继续干等 —— 就是 SuperMediaPlayer.cpp 里
         *     "第一次 seek 画面不动、半天才动"那条注释描述的机制。
         *
         * 处置（全是事件判据，没有计时器、没有时间阈值）：
         *   · seek 后第一帧音频完成重锚之后，第一次拿到的设备位置记为**基准**
         *     （mAudioClockProgressBaseUs），并**先不交出**音频时钟；
         *   · 直到观察到设备位置**严格超过**基准（= 设备真的在产数据/前进），才把音频参考
         *     放行（mAudioClockProgressSeen）；
         *   · 放行之后仍然拒绝"低于基准"的值（向后跳），一律按"音频时钟暂不可用"返回
         *     INT64_MIN —— 语义与既有的 mSeekFlag / mAudioClockReanchorPending 两条 return 完全一致。
         * 复位点与 mAudioClockReanchorPending 一起（FlushAudioPath / Reset），所以每次 seek 重新观察。
         * mAudioClockProgressLogCount 只用来给这条观测限频（不新增 FloodLogId：那会移动
         * mFloodLog[] 及其后成员的偏移）。
         * 三个成员一律追加在类末尾（本文件顶部硬约束：中间插入会让别的 TU 的偏移对不上）。
         */
        int64_t mAudioClockProgressBaseUs{INT64_MIN};
        bool mAudioClockProgressSeen{false};
        int mAudioClockProgressLogCount{0};

        /*
         * ============ 【B15】seek 之后"先出画、再精确"的事件闩 ============
         *
         * 语义：一次 seek 里，**第一张可解码的帧**（= 解码从关键帧起步之后解出的第一帧，
         * 判据就是既有的 mSeekDecodeStartIsKey）在 RenderVideo 的落点块里被直接放上屏，
         * 用来**替换掉 seek 之前那张旧画面**；此后直到"包含目标的那一帧"到达之前，
         * 仍然按原来的规则不显示（精度判据一个字都没改）。
         *
         * 为什么必须有这个闩：没有它，落点块里那条"早于目标 ⇒ render=false"会**每一帧**生效，
         * 于是整段前缀解码期间屏幕停在 seek 前的旧画面上（用户看到的"seek 后画面不动"）。
         * 有了它，最多只提前显示**一张**来自关键帧的干净帧，之后的前缀帧照旧丢掉。
         *
         * 复位点与 mSeekDecodeStartIsKey 完全一致（SeekTo / Reset），保证"每次 seek 只先出一次"。
         * 只做状态判断：不引入计时器、不与时钟锚定交互（锚定仍由 fetchSeekClockAnchorUs
         * 拒绝早于目标的帧，见那里的说明）。
         * 新成员一律追加在类末尾（本文件顶部硬约束：中间插入会让别的 TU 的偏移对不上）。
         */
        bool mSeekFirstDecodableFrameShown{false};

        /*
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
         * 复位口径与 mSeekFirstDecodableFrameShown 一致（SeekTo / Reset）。
         * 无计时器（纯事件计数）。新成员一律追加在类末尾（本文件顶部硬约束：
         * 中间插入会让别的 TU 的偏移对不上）。
         */
        int mVideoDiscardStreak{0};
        int64_t mVideoDiscardGapAbsUs{INT64_MIN};

        /*
         * 【B16-b】追赶期（dropLateVideoFrames）内的**连续被拒帧数**（不做"偏移是否变小"
         * 的过滤）。追赶期偏移可能只是在缓慢收敛（实测 ~2~3ms/帧），上面那条判据不会触发，
         * 于是还要这条：连续被拒达到 VIDEO_CATCHUP_DISCARD_STREAK_MAX 就强制放行一帧。
         * 只在"非 seek 落点窗口"（mSeekRenderGateUs == INT64_MIN）累计，避免与 B15
         * 的"seek 前缀只先出一张"打架；上屏即清零（见 RenderVideo）。
         * 纯事件计数，无计时器。新成员追加在类末尾（本文件顶部硬约束）。
         */
        int mCatchUpDiscardStreak{0};

        /*
         * ============ 【①②B17】切档被 seek 取消后"重新装弹" / 落点窗口"归属" ============
         *
         * 背景（安卓日志，2026-09-26 20:24~20:25）：
         *   · 7/7 次手动切档都是 `status=0 started` → 紧跟一条
         *     `FlushVideoPath from ProcessSeekToMsg (… cancelPendingSwitch=1 … pendingStream=0)`
         *     → `status=3 canceled`，没有一次 status=1 —— 用户点过的档位意图被一次
         *     插进来的用户 seek 整条作废；
         *   · 同时出现 `seek landing frame accepted: pts=99933167, -97006 ms before the seek target`
         *     这种荒谬读数（floor=2.927s 是用户 seek 目标，被采纳的帧在 99.93s），
         *     说明"落点窗口"在被切档路径挪动过时间轴之后仍然被当成有效窗口消费。
         *
         * mSwitchReArmStreamIndex / mSwitchReArmPending：
         *   seek 拆掉在途切档时（SMPMessageControllerListener 的 seek 处理里）记下用户要的档位，
         *   并置闩；seek 结束事件（ResetSeekStatus）里**只重新发起一次** SwitchStream(该档)，
         *   发起即清闩。区分判据见 arm 处注释：只有 MSG_SEEKTO（= 用户 SeekTo）会走到那里，
         *   切档自己的 demuxer 级 seek 从不经过 MSG_SEEKTO ⇒ 不会 ping-pong。
         *   显式取消（换片源/停止/Reset、用户又选别的档、错误终态）一律清闩不重装：
         *   Reset() 与 SwitchStream() 入口都会清。
         *
         * mSeekLandingFloorOwnerUs：与 mSeekPositionFloorUs **同点**写入的"归属"标记
         *   （SeekTo 写地板处，两处都是同一个值）。落点采纳前要求两者一致（或归属为
         *   INT64_MIN = 未知），避免"地板被别的路径改写后，落点块还拿它当本次 seek 的目标"；
         *   另外拒绝"比目标晚出整个精确 seek 容差（mSet->maxASeekDelta）"的帧被当成落点。
         *   seek 结束时由 ResetSeekStatus() 把闸门/地板/归属一起关闭 ⇒ 不会留悬挂态。
         *
         * 三个成员一律追加在类末尾（本文件顶部硬约束：中间插入会让别的 TU 的偏移对不上）。
         */
        int64_t mSeekLandingFloorOwnerUs{INT64_MIN};
        int mSwitchReArmStreamIndex{-1};
        bool mSwitchReArmPending{false};

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
         *   · 切档在途（mPendingVideoStreamIndex >= 0 || mWillChangedVideoStreamIndex >= 0
         *     || mSwitchReArmPending）时不发 seek，只置闩并返回；
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
         * 【B19-b】"切档在途"的唯一判据（PFR 让路 / 用户 seek 推迟都以它为准）。
         * 取切档状态机自己的未完成标志的**并集**：两个 pending/willChange 索引、
         * mPendingVideoDecoderSwitch（`state=decoderSwitch`）、mQualitySwitchCommitPending
         * 与 mQualitySwitchCommittedStreamIndex（`state=committed`）、
         * mQualitySwitchOldFramesPending、mRetiredVideoStreamIndex、
         * mSwitchStartedWhilePaused / mPausedSwitchRenderPending、以及 B17 的 mSwitchReArmPending。
         * 定义与逐项理由见 SuperMediaPlayer.cpp 里那个函数的注释。
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
         * 【追帧加速（seek 落点前缀 / 切档预滚）】
         *
         * 语义：只是把"跑在更高性能点"的要求下发给解码器 —— **不丢帧、不改时间轴、
         * 不动任何精度判据**。seek 之后必须从目标之前的关键帧起解，那段前缀本来就要
         * 全部解出来（现在也全部上屏，见 RenderVideo 的落点前缀分支），本开关只是让
         * platform codec 更快地把它们吐出来（安卓 = MediaCodec operating-rate）。
         *
         * setVideoDecodeBoost      → 当前活动视频解码器（seek 窗口）
         * setPendingVideoDecodeBoost → 切档的 pending 解码器（预滚窗口）
         *
         * 两个都是状态判据驱动：seek 真正开始置位、seek 结束/落点上屏/切档终态收回；
         * 没有任何计时器。平台不支持时是空操作，所以核心层不需要任何平台宏。
         * 追加在类末尾，保证增量 ABI 安全。
         */
        void setVideoDecodeBoost(bool boost);
        void setPendingVideoDecodeBoost(bool boost);

        /*
         * 【切档进度判据】待提交帧的"有效位置"上次采样值，以及"连续多少次没有前进"的计数。
         *
         * 用途：切换是否判失败不再看墙钟，而看**目标路有没有在前进** ——
         * 真机日志里那次切换 lag 从 5248ms 收敛到 96ms（差一瞬间就能提交）却被时间上限
         * 判成失败（界面"切换失败"）。纯状态、无计时器。
         * 追加在类末尾，保证增量 ABI 安全。
         */
        int64_t mPendingVideoProgressUs{INT64_MIN};
        int mPendingVideoStallChecks{0};

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
         * 【ABR 让路】"当前有切档在途"的唯一对外查询（复用 qualitySwitchInFlight() 的并集判据）。
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
         * 不连续点本体。**追加在成员列表最末尾**：本工程增量构建不记录头文件
         * 依赖，插在中间会移动其后成员的偏移，让旧的 friend TU 目标文件按错
         * 偏移访问（见本文件里"以下这些成员必须留在成员列表的最末尾"那段）。
         */
        Discontinuity mDiscontinuity{};
    };
}// namespace Cicada
#endif// CICADA_PLAYER_SERVICE_H
