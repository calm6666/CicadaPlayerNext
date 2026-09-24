#define LOG_TAG "ApsaraPlayerService"

#include "SuperMediaPlayer.h"
#include "media_player_error_def.h"
#include "media_player_error_map.h"
#include "utils/CicadaJSON.h"
#include "utils/CicadaUtils.h"
#include "utils/UrlUtils.h"
#include <cassert>
#include <cinttypes>
#include <codec/avcodecDecoder.h>
#include <codec/decoderFactory.h>
#include <data_source/dataSourcePrototype.h>
#include <demuxer/IDemuxer.h>
#include <demuxer/manifest/MediaManifestParser.h>
#include <render/renderFactory.h>
#include <utils/AFMediaType.h>
#include <utils/af_string.h>
#include <utils/err.h>
#include <utils/errors/framework_error.h>
#include <utils/ffmpeg_utils.h>
#include <utils/file/FileUtils.h>
#include <utils/frame_work_log.h>
#include <utils/oscl/oscl_utils.h>
#include <utils/property.h>
#include <utils/timer.h>


#ifdef __APPLE__

#include <TargetConditionals.h>
#include <codec/Apple/AppleVideoToolBox.h>
#include <render/audio/Apple/AFAudioSessionWrapper.h>

#endif

#define PTS_DISCONTINUE_DELTA (20 * 1000 * 1000)
#define VIDEO_PICTURE_MAX_CACHE_SIZE 2

/*
 * 管线真死时“一次性恢复”的三个阈值（毫秒）。
 *
 * STALL   ：PLAYING 状态下超过这么久**一帧都没真的上屏**，才开始怀疑管线死了。
 *           取 4000 —— 正常播放一秒几十帧；清晰度切换/seek 的渲染器追赶窗口最长
 *           3 秒（JOINING_DROP_LATE_WINDOW_MS），4 秒能完整覆盖它们而不会误报。
 * SAMPLE  ：对比“视频包队列还在不在减少”的采样间隔。队列长度用
 *           mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO) 取（队首 PTS 不
 *           适合做这个判断：解码器不消费时队首 PTS 本来就是不动的）。
 * COOLDOWN：恢复动作的冷却时间。重建解码器是重动作（硬解要重新申请 surface、
 *           从关键帧重新建立参考帧链），绝不允许变成周期性巡检，所以冷却期内
 *           无论如何都不再触发第二次。
 *
 * 【这一段的定位要说清楚，别再拿“主流做法”给它背书】
 *   * 主流播放器是**错误驱动**的：ExoPlayer 的 MediaCodecRenderer 在 codec 报错
 *     （onCodecError()/CodecException）之后才 releaseCodec() + maybeInitCodecOrBypass()，
 *     并且**没有**“停摆/超时 N 秒就重启 codec”这种机制；ffplay 更是只逐帧丢迟到帧，
 *     从不中途重建解码器。
 *   * 所以下面这套“无帧上屏 + 队列不消费 → 重建解码器”是**本内核自己的兜底**，
 *     不是任何主流播放器的做法，阈值（4000/15000/30000ms）也是我们自己的策略。
 *   * 为什么还需要它：各平台后端**不保证**把“静默停摆”报成错误 —— MediaCodec
 *     不返回任何错误、桌面 avcodec 解不出帧也不一定给负值，此时
 *     get_error_frame_no()/STATUS_HAVE_ERROR 一直是干净的，错误驱动的恢复
 *     （见 DecodeVideoPacket 里那段）不会被唤起，画面就永久冻住。
 *     错误驱动的那条路是**首选**，这条只是它兜不住的静默故障的最后一道网。
 *   * 它同样只用可移植接口（af_getsteady_ms() + GetPacketSize() + rebuildVideoDecoder()），
 *     不依赖任何单一平台。
 *   * 一条硬约束：**绝不在播放中 flush 整条管线**。“整条管线 flush”指 FlushVideoPath()
 *     那种（清包 + 取消在途切换 + 清闩锁）；重建解码器时渲染器输入队列会被内核正常
 *     重置（CreateVideoDecoder() 内部的 flushVideoRender()），两者不是一回事 ——
 *     见 rebuildVideoDecoder() 的注释。
 */
#define VIDEO_RECOVER_STALL_MS (4000)
#define VIDEO_RECOVER_SAMPLE_MS (1000)
#define VIDEO_RECOVER_COOLDOWN_MS (30000)

/*
 * seek 在途时的“停滞”宽限（毫秒）。只在包队列**完全不消费**时才可能命中。
 *
 * 为什么需要它：mSeekFlag 只由 doRender() 里“有帧真的上屏”那一处清掉。如果 seek
 * 之后解码器死了（永远解不出目标帧），mSeekFlag 就是**永久真**，而恢复探测的正常
 * 判据要求 seek 不在途 —— 于是谁都不会来救，表现就是用户实测的“seek 完直接卡死”。
 * 所以给 seek 一个更长的宽限：不是“seek 期间不救”，而是“seek 期间多等一会儿再救”。
 *
 * 取 15000 的依据：日志里正常 seek（含 4K DASH、一个分片一个 IDR 的大 GOP）从未超过
 * ~8 秒；框架对同类“等一帧”的判断用 QUALITY_SWITCH_TOTAL_TIMEOUT_MS=18s。15s 加上
 * “包队列一个采样周期内一个包都没被消费”这个条件，只可能是真的死了。
 */
#define VIDEO_RECOVER_STUCK_SEEK_MS (15000)

/*
 * 清晰度切换提交之后的收尾死线（毫秒）。
 *
 * 提交（TryCommitPendingVideoSwitch）只是把 pending decoder 提升为 active，
 * 并不代表新画面已经上屏。真正的收尾（发 READY、关旧 Representation、
 * 释放 retired decoder）都挂在“新帧真的送进渲染器”这一刻上。
 *
 * 问题是这件事**不保证发生**：目标解码器可能一帧都出不来（HLS 2026-09-21
 * 那份日志：提交后 FPS 一直是 0，旧流读到 PTS 88s，unknown stream 刷了 2000+ 行）。
 * 所以给一个上限，到点无论如何都收尾，宁可报 FAILED 也不能把状态机钉死。
 *
 * 500ms 的依据：正常路径实测只要 8ms（DASH 2026-09-21），留 60 倍余量；
 * 再长就会让用户明显感觉到“点了没反应”。
 */
#define QUALITY_SWITCH_DEADLINE_MS (500)

/*
 * [switch] 诊断行 + 各类洪水日志的窗口长度（毫秒）。
 */
#define SWITCH_STATE_LOG_INTERVAL_MS (1000)
#define FLOOD_LOG_WINDOW_MS (1000)

/*
 * 读前闸门阈值（微秒）：视频包队列的**队首**比主时钟超前超过它就不再读。
 *
 * 2 秒的依据：正常播放时队首就贴在播放点上（超前 0~几百毫秒，取决于解码
 * 快慢）；只有“解码器被堵住、读取还在继续”时才会累积到秒级。留 2 秒既不会
 * 误伤正常的抖动/慢解码，又能把实测出现的 +9~+10 秒那种状态挡在门外。
 */
#define MAX_VIDEO_READ_AHEAD_US (2 * 1000 * 1000)

/*
 * “渲染器追赶窗口”（毫秒）—— 对应 ExoPlayer 的 setJoiningDeadlineMs()/joining 语义。
 *
 * 场景：清晰度切换提交时，新路的首帧可能比主时钟旧 1~2 秒（4K 目标路预热比实时慢，
 * 提交门限为此留了 2 秒窗口），seek 之后同理（解码器要从分片起点解到目标点）。
 * 这时画面若按“每帧都渲染”，视频会**永久**落后 1~2 秒（一帧不快进就追不回来）；
 * 若按“跳到下一个关键帧”，画面会被扔到时钟前面几秒然后冻住。
 *
 * 正确做法（ExoPlayer/AVPlayer 都这么做）：给渲染器一个**有限的追赶窗口**，在这段
 * 时间内**丢掉迟到的帧**，直到出现一帧落在时钟上，之后恢复正常节拍。
 * 本工程里这条路径由 beginRendererJoining() 打开（RenderVideo 的
 * `else if (dropLateVideoFrames)` 决定哪些迟到帧被丢掉、DecodeVideoPacket 按它
 * 丢掉过时的包），窗口到点或被一帧准时帧命中就自动关掉。
 */
#define JOINING_DROP_LATE_WINDOW_MS (3000)

/*
 * seek 期间丢弃“离目标点还有这么远”的帧（微秒）：只保留目标点前 200ms 之内的帧，
 * 其余直接丢，别推给渲染器（原因见 RenderVideo 里那段注释）。
 */
#define SEEK_TARGET_DROP_AHEAD_US (200 * 1000)

/*
 * seek 目标地板的“放弃距离”（微秒）：帧位置离 seek 目标还差这么远时，就不再为了
 * 对齐目标而丢帧（见 RenderVideo 里那段说明）。取 15 秒 —— 分片式 DASH/HLS 最多
 * 也就一个分片的预滚（实测 8.3~12.5 秒），超过它说明目标点本身有问题，宁可倒一下
 * 也不能把画面永久冻住。
 */
#define SEEK_FLOOR_GIVEUP_US (15 * 1000 * 1000)

/*
 * seek 完成时判定"主时钟是否还在目标点附近"的容忍值（微秒）。
 * 超过它就把时钟重新钉回 seek 目标（见 RenderVideo 之后的 seek 完成分支）：
 * 追赶窗口是按主时钟决定丢哪些帧的，时钟跑偏就会把目标帧丢掉。
 */
#define SEEK_CLOCK_TOLERANCE_US (300 * 1000)

/*
 * 切换提交后**先挡掉几帧**再交给渲染器（见提交处那段说明）：
 * 防的是"解码器刚接手时的一瞬间马赛克/花屏"。3 帧 ≈ 50ms（60fps），用户感觉不到，
 * 但足够让参考帧链和解码器内部状态稳定下来。
 */
#define QUALITY_SWITCH_HOLD_FRAMES 3

/*
 * 目标（pending）Representation 预热队列的上限，单位是"包"。
 *   PENDING_VIDEO_QUEUE_CAP —— 软上限：到这儿就优先让解码线程消费，正常情况下
 *                              读循环会在当前路还够吃的时候停读。
 *   PENDING_VIDEO_QUEUE_HARD_CAP —— 硬上限：当前路快见底时读循环会继续读
 *                              （否则当前路和音频一起饿死，画面整体卡死），
 *                              多出来的目标包到硬上限就丢掉，保证内存有界。
 *   PENDING_CAP_ACTIVE_MIN_US / AUDIO_MIN_US —— 判定"当前路还吃得住"的门槛。
 */
#define PENDING_VIDEO_QUEUE_CAP 120
#define PENDING_VIDEO_QUEUE_HARD_CAP 320
#define PENDING_CAP_ACTIVE_MIN_US (3 * 1000 * 1000)
#define PENDING_CAP_AUDIO_MIN_US (2 * 1000 * 1000)

/*
 * 每轮 doDeCode 里最多连续解码多少个目标（pending）包。
 * 目标路追赶速度的硬上限 = PENDING_DECODE_BURST × 主循环轮数/秒；
 * 原来是 4（≈108 包/秒），DASH 一个 10 秒分片的预滚要 5~10 秒才追得上。
 * pending 解码器输出队列满时会 RETRY_IN 让位给 active 路，所以调大是安全的。
 */
#define PENDING_DECODE_BURST 64

/*
 * 清晰度切换的**总**时长上限（毫秒）。提交前（warming / decoderSwitch）也算：
 * 实测出现过目标路一直追不上、状态机在原地挂 20 秒的情况（只靠用户 seek 才取消），
 * 期间 qualitySwitchWarming 一直为真，渲染节拍被完全绕过。
 *
 * 【2026-09-21 修：10 秒 → 18 秒】
 * 现在切换点是"参考点之后的第一个关键帧"（见 PENDING_PREROLL_KEEP_US），
 * 等待时间由**关键帧间隔**决定：本片源 4.17 秒，但分片式 DASH 里一个分片只放
 * 一个 IDR 的情况也不少（10~20 秒）。10 秒上限会把这种"正常但慢一点"的切换
 * 判成失败（日志里 `target rendition did not reach playback timeline` 就是它），
 * 而用户的要求是"不会出现切换失败"。18 秒 = 一个较长的分片 + 余量。
 */
#define QUALITY_SWITCH_TOTAL_TIMEOUT_MS (18000)


/*
 * 追赶窗口内「多久没上屏就强制渲染一帧」（毫秒）。
 * 对应 ExoPlayer `MediaCodecVideoRenderer.shouldForceRenderOutputBuffer()` 里的
 * `elapsedSinceLastRenderUs > 100_000`：为了追赶时钟丢掉迟到帧可以，但不能长时间不出画。
 */
#define JOINING_FORCE_RENDER_MS (100)

/*
 * 提交时允许新路首帧比主时钟（= 当前播放位置）旧多少（微秒）。
 *
 * 这是「无感」的关键：新路的帧如果明显早于**当前播放位置**，把它推进 active
 * 帧队列就等于把画面往回倒 —— 用户看到进度条往回跳一下、音画错位
 * （实测 `committed … pts=1201200 master=3192806`，锚点比时钟旧 2 秒，
 * 紧接着就是约 1 秒的画面冻住）。
 *
 * 开源实现里都没有「把已经过去的时间再放一遍」这个动作：ExoPlayer 对
 * `presentationTimeUs < positionUs` 的输出缓冲直接 `FRAME_RELEASE_SKIP`
 * （解码但不显示），换流只把读取游标移回当前位置；hls.js / dash.js 把新
 * Codec 的分片**追加**在当前播放位置之后，旧缓冲放完正好接上。所以这里只留
 * 一个帧间隔的容差（25fps 一帧 40ms，100ms 兼顾 30/60fps 与时间戳抖动）。
 */
#define PENDING_ALIGN_TOLERANCE_US (100 * 1000)

/*
 * 提交时允许新路首帧比主时钟**新**多少（微秒）。
 *
 * 超过这个值就等主时钟走过来（旧路在等的时候照常播放，不花代价），而不是把
 * 一张"未来"的帧提交上去 —— 提交未来帧 = 画面当场往前跳，就是音画不同步。
 * 取 150ms：一帧（25fps 40ms / 60fps 16.7ms）的若干倍，足够吸收时间戳抖动。
 */
#define PENDING_FUTURE_TOLERANCE_US (150 * 1000)

/*
 * FillPendingVideoFrame 单次调用最多排空的预滚帧数。
 *
 * 目标路从"分片边界"开始解码时，落在切换窗口之前的帧可能有几百张
 * （DASH 分片 10 秒 × 60fps = 600 张）。丢弃本身不花时间，真正的成本是解码，
 * 所以这个上限要足够大，不能让"每轮只丢 8 张"成为切换时长的瓶颈。
 */
#define PENDING_PREROLL_DRAIN_MAX 2048

/*
 * 预滚窗口（微秒）：目标路只保留"切换参考点**之后**最近的一个关键帧"及其之后的
 * 数据，更早的包在**喂给解码器之前**就丢掉（见 ProcessVideoPacket 里的预滚跳过）。
 *
 * 取 0 是这一轮最关键的决定：参考点 = 进入预滚那一刻的 max(切换点, 主时钟)，于是
 *   "保留 >= 参考点的第一个**关键帧**"
 * 就等于**在下一个关键帧边界上换档** —— 这正是 hls.js / dash.js 切换的做法
 * （把新 Representation 的分片追加在当前位置之后），它一次性消掉了三个问题：
 *   1) **不用再解预滚**：原来是"从分片起点开始解，一直解到播放位置，解出来的帧
 *      全部丢掉"。DASH 分片 10 秒，4K 目标路解码只有 ~110 帧/秒，10 秒预滚 = 600
 *      帧 = 5.5 秒 —— 这就是"dash 切换时间明显比 hls 长"和"有时切换失败（超时）"
 *      的主因；18.5 秒预滚那一次（从 4K 切 480P）还让两路解码器抢了 9 秒 CPU，
 *      用户看到的就是"从高分辨率切低分辨率会卡一下"；
 *   2) **A/V 绝对同步**：提交点就是关键帧本身，主时钟走到那里才提交，误差只有
 *      抖动量级（原来是"播到哪算哪"，实测提交时视频比主时钟落后 50~99ms、
 *      极端情况超前 481ms）；
 *   3) **没有卡顿**：等待期间旧路照常播放（不是黑屏/冻结），切换在关键帧上无缝接上。
 *
 * 代价是"点击到生效"最多等一个关键帧间隔（本片源 DASH 分片 10 秒、HLS 4.17 秒），
 * 但因为画面一直在动，用户感知到的是"过一会儿清晰度变了"，而不是"卡住了"。
 */
#define PENDING_PREROLL_KEEP_US (0)

/*
 * 等"参考点之后的关键帧"最多等多久（毫秒）。超时后退化：接受下一个关键帧，
 * 哪怕它在参考点之前（多解一个 GOP，但保证切换能完成）。这是给"关键帧间隔
 * 特别长"的片源兜底的。播放器自己的切换上限是 18 秒，这里留足余量。
 */
#define PENDING_PREROLL_WAIT_MAX_MS (6000)


/*
 * 预滚跳过的参考点最多允许比"切换请求时刻"往前推这么多。
 *
 * 参考点取 max(切换点, 当前主时钟)：目标流打开的这段时间里主时钟还在走，按它
 * 丢预滚能让解码起点更贴近真正需要的时刻（少解一点）。但如果目标路解码比实时还慢，
 * 参考点无限往前推就等于永远不开始解码 —— 所以封顶。
 */
#define PENDING_PREROLL_MAX_SKIP_US (8 * 1000 * 1000)
/* 单次 TryCommitPendingVideoSwitch 调用最多丢弃多少张"已落在播放位置之前"的
 * pending 帧（剩下的下一轮继续），避免目标路一直比主时钟慢时占死主循环。 */
#define PENDING_STALE_DRAIN_MAX 512

static int MAX_DECODE_ERROR_FRAME = 1000;

/*
 * 解码器“release + 重新 init”这种恢复最多尝试几次。
 *
 * 主流播放器都是**错误驱动**的：ExoPlayer 的 MediaCodecRenderer 在 onCodecError() 之后走
 * releaseCodec() + maybeInitCodecOrBypass()，失败时按构造参数 enableDecoderFallback
 * 回退到低优先级解码器，回退链走完还不行就用 setPendingPlaybackException() 报致命错误
 * —— 它没有任何“停摆/超时 N 秒就重启 codec”的机制。
 *
 * 本内核的可回退档数就是硬解 + 软解两档（CreateVideoDecoder 里硬解失败会自动落软解），
 * 所以这里取 2：与“回退链走完就报错”一致，绝不无限重试。中间只要成功出过一帧
 * （见 FillVideoFrame 里紧挨 clean_error() 的清零），计数就归零，等价于“codec 恢复正常”。
 */
#define MAX_VIDEO_DECODER_REBUILDS (2)

#define PTS_REVERTING (mVideoPtsRevert != mAudioPtsRevert)

using namespace Cicada;
SuperMediaPlayer SuperMediaPlayer::se(1);

static MsgParam dummyMsg{{nullptr}};

const int64_t SuperMediaPlayer::SEEK_ACCURATE_MAX = 11 * 1000 * 1000;

#define HAVE_VIDEO (mCurrentVideoIndex >= 0)
#define HAVE_AUDIO (mCurrentAudioIndex >= 0)
#define HAVE_SUBTITLE (mCurrentSubtitleIndex >= 0)

SuperMediaPlayer::SuperMediaPlayer()
{
    AF_LOGD("SuperMediaPlayer()");

    mSet = static_cast<unique_ptr<player_type_set>>(new player_type_set());
    mBufferController = static_cast<unique_ptr<BufferController>>(new BufferController());
    mUtil = static_cast<unique_ptr<MediaPlayerUtil>>(new MediaPlayerUtil());
    mMPAUtil = static_cast<unique_ptr<MediaPlayerAnalyticsUtil>>(new MediaPlayerAnalyticsUtil());
    mMsgCtrlListener = static_cast<unique_ptr<SMPMessageControllerListener>>(new SMPMessageControllerListener(*this));
    mMessageControl = static_cast<unique_ptr<PlayerMessageControl>>(new PlayerMessageControl(*mMsgCtrlListener));
    mAudioRenderCB = static_cast<unique_ptr<ApsaraAudioRenderCallback>>(new ApsaraAudioRenderCallback(*this));
    mVideoRenderListener = static_cast<unique_ptr<ApsaraVideoRenderListener>>(new ApsaraVideoRenderListener(*this));
    mVideoProcessCb = static_cast<unique_ptr<ApsaraVideoProcessTextureCallback>>(new ApsaraVideoProcessTextureCallback(*this));
    mApsaraThread = static_cast<unique_ptr<afThread>>(new afThread([this]() -> int { return this->mainService(); }, LOG_TAG));

    /*
     * 纯加固（不是 16:34 那次闪退的原因）：把 mPNotifier 提前到 listener 之前创建。
     *
     * listener 一旦被建立，它的回调 onNetWorkRetry() / onNetWorkConnected()
     * （SuperMediaPlayerDataSourceListener.cpp:43 / :79）就会解引用 mPlayer.mPNotifier，
     * 而它原本要等到本函数末尾（原 :97）才被 new 出来，构造窗口内它是 nullptr。
     * 这和 enableRetry_l()（SuperMediaPlayerDataSourceListener.cpp:67）在构造期就读
     * mPlayer.mSet 属于同一类问题：listener 在构造期就会去碰 SuperMediaPlayer 的成员，
     * 所以它用到的成员都应该先于它初始化。PlayerNotifier 自身只创建一个通知线程对象，
     * 不依赖 SuperMediaPlayer 的其它成员，提前创建是安全的。
     */
    mPNotifier = new PlayerNotifier();

    mSourceListener = static_cast<unique_ptr<SuperMediaPlayerDataSourceListener>>(new SuperMediaPlayerDataSourceListener(*this));
    mDcaManager = static_cast<unique_ptr<SMP_DCAManager>>(new SMP_DCAManager(*this));
    mAVDeviceManager = static_cast<unique_ptr<SMPAVDeviceManager>>(new SMPAVDeviceManager());
    mRecorderSet = static_cast<unique_ptr<SMPRecorderSet>>(new SMPRecorderSet());

    Reset();
    mTimerInterval = 500;
}

SuperMediaPlayer::~SuperMediaPlayer()
{
    if (mIsDummy) {
        return;
    }
    Stop();
    AF_LOGD("~SuperMediaPlayer");
    mCanceled = true;
    mPlayerCondition.notify_one();
    mApsaraThread->stop();
    mSubPlayer = nullptr;
    mSubListener = nullptr;
    // delete mPNotifier after mPMainThread, to avoid be using
    delete mPNotifier;
    mPNotifier = nullptr;
    mMessageControl = nullptr;
    mAVDeviceManager = nullptr;
#ifdef ENABLE_VIDEO_FILTER
    mFilterManager = nullptr;
#endif
}

void SuperMediaPlayer::putMsg(PlayMsgType type, const MsgParam &param, bool trigger)
{
    mMessageControl->putMsg(type, param);

    if (trigger) {
        mPlayerCondition.notify_one();
    }
}

void SuperMediaPlayer::SetView(void *view)
{
    mMsgCtrlListener->ProcessSetViewMsg(view);
}

void SuperMediaPlayer::ClearScreen()
{
    if (mAVDeviceManager->getVideoRender()) {
        mAVDeviceManager->getVideoRender()->clearScreen();
    }
}

int64_t SuperMediaPlayer::GetMasterClockPts()
{
    return mMasterClock.GetTime();
}

void SuperMediaPlayer::setBitStreamCb(readCB read, seekCB seek, void *arg)
{
    MsgParam param;
    MsgBitStreamParam msgBitStreamParam = {nullptr};
    msgBitStreamParam.read = read;
    msgBitStreamParam.seek = seek;
    msgBitStreamParam.arg = arg;
    param.msgBitStreamParam = msgBitStreamParam;
    putMsg(MSG_SET_BITSTREAM, param);
}

void SuperMediaPlayer::SetDataSource(const char *url)
{
    MsgParam param;
    MsgDataSourceParam dataSourceParam = {nullptr};
    dataSourceParam.url = new string(url ? url : "");
    param.dataSourceParam = dataSourceParam;
    putMsg(MSG_SETDATASOURCE, param);
}

void SuperMediaPlayer::SetDataSource(const Manifest::MediaManifest &manifest)
{
    MsgParam param;
    MsgManifestParam manifestParam{};
    manifestParam.manifest = new Manifest::MediaManifest(manifest);
    param.msgManifestParam = manifestParam;
    putMsg(MSG_SETMANIFESTSOURCE, param);
}

void SuperMediaPlayer::SetDataSource(const std::string &jsonManifest)
{
    std::string error;
    auto *manifest = new Manifest::MediaManifest();
    if (!Manifest::MediaManifestParser::parse(jsonManifest, *manifest, error)) {
        AF_LOGE("SetDataSource(json): manifest parse error: %s\n", error.c_str());
        delete manifest;
        return;
    }
    MsgParam param;
    MsgManifestParam manifestParam{};
    manifestParam.manifest = manifest;
    param.msgManifestParam = manifestParam;
    putMsg(MSG_SETMANIFESTSOURCE, param);
}

void SuperMediaPlayer::Prepare()
{

    if (mPlayStatus != PLAYER_INITIALZED && mPlayStatus != PLAYER_STOPPED) {
        Stop();
    }

    mMPAUtil->reset();

#if TARGET_OS_IPHONE
    AFAudioSessionWrapper::activeAudio();
#endif
    mPrepareStartTime = af_gettime_relative();
    std::unique_lock<std::mutex> uMutex(mPlayerMutex);
    putMsg(MSG_PREPARE, dummyMsg);
    mApsaraThread->start();
}

void SuperMediaPlayer::CaptureScreen()
{
    std::lock_guard<std::mutex> uMutex(mCreateMutex);

    if (mAVDeviceManager->getVideoRender()) {
        mAVDeviceManager->getVideoRender()->captureScreen([this](uint8_t *data, int width, int height) {
            if (this->mPNotifier) {
                this->mPNotifier->NotifyCaptureScreen(data, width, height);
            }
        });
    } else {
        if (this->mPNotifier) {
            this->mPNotifier->NotifyCaptureScreen(nullptr, 0, 0);
        }
    }
}

void SuperMediaPlayer::SetVolume(float volume)
{
    //TODO:put message to
    mSet->mVolume = volume;

    if (mSet->mVolume < 0) {
        mSet->mVolume = 0;
    } else if (mSet->mVolume > 1.0) {
        AF_LOGW("volume >1.0");
    }

    mAVDeviceManager->setVolume(mSet->mVolume);
}

void SuperMediaPlayer::Start()
{
    if ((PLAYER_INITIALZED == mPlayStatus) || (PLAYER_PREPARING == mPlayStatus) || PLAYER_PREPARINIT == mPlayStatus) {
        waitingForStart = true;
    }

    this->putMsg(MSG_START, dummyMsg);
}


void SuperMediaPlayer::Pause()
{
    waitingForStart = false;
    this->putMsg(MSG_PAUSE, dummyMsg);
}


void SuperMediaPlayer::SeekTo(int64_t pos, bool bAccurate)
{
    MsgParam param;
    MsgSeekParam seekParam;
    seekParam.seekPos = (int64_t) pos * 1000;
    seekParam.bAccurate = bAccurate;
    param.seekParam = seekParam;
    this->putMsg(MSG_SEEKTO, param);
    mSeekPos = pos * 1000;
    mSeekNeedCatch = bAccurate;
    /*
     * seek 目标地板（见 SuperMediaPlayer.h 里 mSeekPositionFloorUs 的说明）：
     * 精确 seek 的重启点在目标点之前的关键帧上，管道要走一会儿才到目标，
     * 这段时间里位置回调会把进度条往回拽。地板保证它只往前走。
     */
    mSeekPositionFloorUs = mSeekPos;
}

void SuperMediaPlayer::Mute(bool bMute)
{
    if (bMute == mSet->bMute) {
        return;
    }

    mSet->bMute = bMute;
    this->putMsg(MSG_MUTE, dummyMsg);
}

void SuperMediaPlayer::EnterBackGround(bool back)
{
    // lock mAppStatusMutex before mCreateMutex
    std::lock_guard<std::mutex> lock(mAppStatusMutex);
    MsgParam param;
    MsgHoldOnVideoParam holdParam;

    if (back) {
        AF_LOGI("EnterBackGround");
        mAppStatus = APP_BACKGROUND;
        holdParam.hold = true;
        param.msgHoldOnVideoParam = holdParam;
        putMsg(MSG_INTERNAL_VIDEO_HOLD_ON, param);

        if (mPlayStatus == PLAYER_PLAYING) {
            putMsg(MSG_INTERNAL_VIDEO_CLEAN_FRAME, dummyMsg);
        }
    } else {
        AF_LOGI("EnterBackGround APP_FOREGROUND");
        mAppStatus = APP_FOREGROUND;
        holdParam.hold = false;
        param.msgHoldOnVideoParam = holdParam;
        putMsg(MSG_INTERNAL_VIDEO_HOLD_ON, param);
    }
}

StreamType SuperMediaPlayer::SwitchStream(int streamIndex)
{
    MsgParam param;
    MsgChangeStreamParam streamParam;
    streamParam.index = streamIndex;
    param.streamParam = streamParam;
    StreamType streamType = ST_TYPE_UNKNOWN;
    PlayMsgType type = MSG_INVALID;
    std::deque<StreamInfo *> &streamInfoQueue = mMediaInfo.mStreamInfoQueue;
    for (auto &it : streamInfoQueue) {
        if (it->streamIndex == streamIndex) {
            switch (it->type) {
                case ST_TYPE_VIDEO:
                    streamType = ST_TYPE_VIDEO;
                    type = MSG_CHANGE_VIDEO_STREAM;
                    break;

                case ST_TYPE_AUDIO:
                    streamType = ST_TYPE_AUDIO;
                    type = MSG_CHANGE_AUDIO_STREAM;
                    break;

                case ST_TYPE_SUB:
                    streamType = ST_TYPE_SUB;
                    type = MSG_CHANGE_SUBTITLE_STREAM;
                    break;

                default:
                    AF_LOGE("unknown stream Type");
                    return streamType;
            }

            break;
        }
    }

    if (type != MSG_INVALID) {
        this->putMsg(type, param);
    }

    return streamType;
}

void SuperMediaPlayer::Interrupt(bool inter)
{
    AF_TRACE;
    std::lock_guard<std::mutex> locker(mCreateMutex);

    if (mDataSource) {
        mDataSource->Interrupt(inter);
    } else {
        AF_TRACE;
    }

    if (mDemuxerService) {
        mDemuxerService->interrupt(inter);
        mDemuxerService->preStop();
    } else {
        AF_TRACE;
    }
}


int SuperMediaPlayer::Stop()
{
    if ((afThread::THREAD_STATUS_RUNNING != mApsaraThread->getStatus()) &&
        ((mPlayStatus == PLAYER_IDLE) || (mPlayStatus == PLAYER_STOPPED))) {
        return 0;
    }

    /* 显式持有锁直到 Stop() 返回，避免 MSVC 将临时 lock 判定为未保护作用域。 */
    std::unique_lock<std::mutex> playerLock(mPlayerMutex);
    AF_LOGI("Player ReadPacket Stop");
    int64_t t1 = af_getsteady_ms();
    AF_TRACE;
    waitingForStart = false;
    mCanceled = true;
    mPNotifier->Clean();
    mPNotifier->Enable(false);

    // video render use a dispatch_sync to main thread, to avoid dead lock,release the thread to deal dispatch_sync job
    // FIXME: create render in setView api in main thread on apple platform
    //    #ifdef __APPLE__
    //        if (strcmp(dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL), dispatch_queue_get_label(dispatch_get_main_queue())) == 0) {
    //            if (!mVideoRenderInited) {
    //                while (!mMainServiceCanceled) {
    //                    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, 1);
    //                    AF_LOGI("Waiting for main service canceled\n");
    //                }
    //            }
    //        }
    //    #endif
    Interrupt(true);
    mPlayerCondition.notify_one();
    mApsaraThread->pause();
    mAVDeviceManager->invalidDevices(SMPAVDeviceManager::DEVICE_TYPE_AUDIO | SMPAVDeviceManager::DEVICE_TYPE_VIDEO);
    mPlayStatus = PLAYER_STOPPED;
    //        ChangePlayerStatus(PLAYER_STOPPED);
    mBufferController->ClearPacket(BUFFER_TYPE_AV);

    AF_TRACE;
    FlushAudioPath();

    AF_TRACE;
    mBRendingStart = false;
    AF_TRACE;
    FlushVideoPath(true, true, __func__);
    // clear the message queue after flash video render
    mMessageControl->clear();
    AF_TRACE;

    if (mDemuxerService) {
        mDemuxerService->interrupt(1);

        if (mDataSource) {
            mDataSource->Interrupt(true);
        }

        std::lock_guard<std::mutex> uMutex(mCreateMutex);
        mDemuxerService->stop();
        mDemuxerService->close();

        if (mMixMode) {
            if (mMainStreamId != -1) {
                mDemuxerService->CloseStream(mMainStreamId);
            }

            if (mCurrentSubtitleIndex >= 0) {
                mDemuxerService->CloseStream(mCurrentSubtitleIndex);
            }
        } else {
            if (mCurrentAudioIndex >= 0) {
                mDemuxerService->CloseStream(mCurrentAudioIndex);
            }

            if (mCurrentVideoIndex >= 0) {
                mDemuxerService->CloseStream(mCurrentVideoIndex);
            }

            if (mCurrentSubtitleIndex >= 0) {
                mDemuxerService->CloseStream(mCurrentSubtitleIndex);
            }
        }
    }

    if (mDataSource) {
        mDataSource->Close();
        std::lock_guard<std::mutex> uMutex(mCreateMutex);
        delete mDataSource;
        mDataSource = nullptr;
    }

    if (mAVDeviceManager->getVideoRender()) {
        // lock mAppStatusMutex before mCreateMutex
        std::lock_guard<std::mutex> lock(mAppStatusMutex);

        // for iOS, don't delete render in background, and we should reuse it later.
        if (APP_BACKGROUND != mAppStatus) {
            std::lock_guard<std::mutex> uMutex(mCreateMutex);

            if (mSet->clearShowWhenStop) {
                mAVDeviceManager->getVideoRender()->clearScreen();
            }
        }
    }

    delete mVideoParser;
    mVideoParser = nullptr;
    {
        std::lock_guard<std::mutex> uMutex(mCreateMutex);
        std::deque<StreamInfo *> &streamInfoQueue = mMediaInfo.mStreamInfoQueue;
        for (StreamInfo *info : streamInfoQueue) {
            releaseStreamInfo(info);
        }
        streamInfoQueue.clear();
        mMediaInfo.totalBitrate = 0;
    }
    mBufferController->ClearPacket(BUFFER_TYPE_SUBTITLE);
    Reset();

    mRecorderSet->reset();

    AF_LOGD("stop spend time is %lld", af_getsteady_ms() - t1);
    return 0;
}

void SuperMediaPlayer::releaseStreamInfo(const StreamInfo *info) const
{
    if (info->subtitleLang) {
        free(info->subtitleLang);
    }

    if (info->audioLang) {
        free(info->audioLang);
    }

    if (info->description) {
        free(info->description);
    }

    delete info;
}

void SuperMediaPlayer::SetRefer(const char *referer)
{
    if (referer) {
        mSet->refer = referer;
    }
}

void SuperMediaPlayer::SetUserAgent(const char *userAgent)
{
    if (userAgent) {
        mSet->userAgent = userAgent;
    }
}

void SuperMediaPlayer::SetTimeout(int timeout)
{
    mSet->timeout_ms = timeout;
}

void SuperMediaPlayer::SetDropBufferThreshold(int dropValue)
{
    mSet->RTMaxDelayTime = dropValue * 1000;
}

void SuperMediaPlayer::SetLooping(bool looping)
{
    mSet->bLooping = looping;
}

bool SuperMediaPlayer::isLooping()
{
    return mSet->bLooping;
}

int SuperMediaPlayer::SetOption(const char *key, const char *value)
{
    if (key == nullptr) {
        return -1;
    }

    int duration;
    string theKey = key;

    if (theKey == "startBufferDuration") {
        duration = atoi(value);

        if (duration > 0) {
            mSet->startBufferDuration = duration * 1000;
        }
    } else if (theKey == "RTMaxDelayTime") {
        duration = atoi(value);

        if (duration > 0) {
            mSet->RTMaxDelayTime = duration * 1000;
            mSet->mOptions.set(theKey, std::to_string(mSet->RTMaxDelayTime), options::REPLACE);
        }
    } else if (theKey == "highLevelBufferDuration") {
        duration = atoi(value);

        if (duration > 0) {
            mSet->highLevelBufferDuration = duration * 1000;
            mSet->mOptions.set(theKey, std::to_string(mSet->highLevelBufferDuration), options::REPLACE);
        }
    } else if (theKey == "http_proxy") {
        mSet->http_proxy = value;
    } else if (theKey == "maxBufferDuration") {
        duration = atoi(value);

        if (duration > 0) {
            mSet->maxBufferDuration = int64_t(duration) * 1000;
            mSet->mOptions.set(theKey, std::to_string(mSet->maxBufferDuration), options::REPLACE);
        }
    } else if (theKey == "LowLatency") {
        mSet->bLowLatency = (bool) atoi(value);
    } else if (theKey == "ClearShowWhenStop") {
        int clearShowWhenStop = atoi(value);
        mSet->clearShowWhenStop = (bool) clearShowWhenStop;
    } else if (theKey == "enableVideoTunnelRender") {
        mSet->bEnableTunnelRender = (atoi(value) != 0);
    } else if (theKey == "disableAudio") {
        mSet->bDisableAudio = (atoi(value) != 0);
    } else if (theKey == "disableVideo") {
        mSet->bDisableVideo = (atoi(value) != 0);
    } else if (theKey == "timerInterval") {
        mTimerInterval = atoi(value);
    } else if (theKey == "Analytics.ReportID") {
        if (nullptr == value) {
            return -1;
        }

        int64_t eventReportID = atoll(value);
        mSet->AnalyticsID = eventReportID;
    } else if (theKey == "bandWidth") {
        mSet->mDefaultBandWidth = atoi(value);
    } else if (theKey == "description") {
        mSet->mOptions.set(theKey, value, options::REPLACE);
        return 0;
    } else if (theKey == "enableVRC") {
        mSet->bEnableVRC = (atoi(value) != 0);
    } else if (theKey == "maxAccurateSeekDelta") {
        mSet->maxASeekDelta = atoi(value) * 1000;
    } else if (theKey == "maxVideoRecoverSize") {
        mSet->maxVideoRecoverSize = atoi(value);
    } else if (theKey == "surfaceChanged") {
        std::lock_guard<std::mutex> uMutex(mCreateMutex);

        if (mAVDeviceManager->isVideoRenderValid()) {
            mAVDeviceManager->getVideoRender()->surfaceChanged();
        }
    } else if (theKey == "streamTypes") {
        uint64_t flags = atoll(value);
        mSet->bDisableAudio = mSet->bDisableVideo = true;
        if (flags & VIDEO_FLAG) {
            mSet->bDisableVideo = false;
        }
        if (flags & AUDIO_FLAG) {
            mSet->bDisableAudio = false;
        }
    } else if (theKey == "IPResolveType") {
        uint64_t type = atoll(value);
        mSet->mIpType = static_cast<IpResolveType>(type);
    } else if (theKey == "fastStart") {
        mSet->mFastStart = atol(value) != 0;
    } else if (theKey == "pixelBufferOutputFormat") {
        mSet->pixelBufferOutputFormat = atol(value);
    } else if (theKey == "liveStartIndex") {
        mSet->mOptions.set(theKey, value, options::REPLACE);
    } else if (theKey == "DRMMagicKey") {
        mSet->drmMagicKey = value;
    } else if (theKey == "sessionId") {
        mSet->sessionId = value;

        std::lock_guard<std::mutex> uMutex(mCreateMutex);
        if (mDemuxerService != nullptr && mDemuxerService->getDemuxerHandle()) {
            mDemuxerService->getDemuxerHandle()->SetOption("sessionId", mSet->sessionId);
        }
    } else if (theKey == "networkRetryCount") {
        mSet->netWorkRetryCount = (int) atol(value);
    } else if (theKey == "maxBackwardBufferDuration") {
        mBufferController->SetMaxBackwardDuration(BUFFER_TYPE_ALL, atoll(value) * 1000);
    } else if (theKey == "preferAudio") {
        mSet->preferAudio = (atoi(value) != 0);
        AF_LOGI("preferAudio %d\n", mSet->preferAudio);
        std::lock_guard<std::mutex> uMutex(mCreateMutex);
        if (mDemuxerService && mDemuxerService->getDemuxerHandle()) {
            mDemuxerService->getDemuxerHandle()->SetOption("preferAudio", mSet->preferAudio);
        }
    }

    return 0;
}

void SuperMediaPlayer::GetOption(const char *key, char *value)
{
    if (key == nullptr) {
        return;
    }

    string theKey = key;

    if (theKey == "maxBufferDuration") {
        snprintf(value, MAX_OPT_VALUE_LENGTH, "%" PRId64 "", mSet->maxBufferDuration);
    } else if (theKey == "mediaStreamSize") {
        int64_t size = -1;
        std::unique_lock<std::mutex> uMutex(mCreateMutex);

        if (mDataSource && mDemuxerService) {
            if (!mDemuxerService->isPlayList()) {
                size = mDataSource->Seek(0, SEEK_SIZE);
            }
        } else if (mBSSeekCb) {
            size = mBSSeekCb(mBSCbArg, 0, SEEK_SIZE);
        }

        snprintf(value, MAX_OPT_VALUE_LENGTH, "%" PRId64 "", size);
    } else if (theKey == "description") {
        sprintf(value, "%s", mSet->mOptions.get("description").c_str());
    } else if (theKey == "descriptionLen") {
        snprintf(value, MAX_OPT_VALUE_LENGTH, "%lu", static_cast<unsigned long>(mSet->mOptions.get("description").length()));
    } else if (theKey == "renderFps") {
        float renderFps = GetVideoRenderFps();
        snprintf(value, MAX_OPT_VALUE_LENGTH, "%f", renderFps);
    } else if (theKey == "videoDroppedInfo") {
        uint64_t total, dropped;
        mUtil->getVideoDroppedInfo(total, dropped);
        snprintf(value, MAX_OPT_VALUE_LENGTH, "%" PRIu64 "/%" PRIu64, dropped, total);
    }
}

void SuperMediaPlayer::NotifyPosition(int64_t position)
{
    mPNotifier->NotifyPosition(position / 1000);
}

int64_t SuperMediaPlayer::getCurrentPosition()
{
    if (isSeeking()) {
        return mSeekPos;
    }

    mCurrentPos = mCurrentPos.load() < 0 ? 0 : mCurrentPos.load();

    if (mDuration > 0) {
        mCurrentPos = mCurrentPos.load() <= mDuration ? mCurrentPos.load() : mDuration;
    }

    /*
     * seek 目标地板：管道还没走到 seek 目标之前，位置一律按目标点报。
     * 不加这一条，进度条会“跳到目标 → 被回到关键帧位置的旧回调拽回去 → 再往前走”，
     * 用户看到的就是来回跳（见 mSeekPositionFloorUs 的说明）。
     */
    if (mSeekPositionFloorUs != INT64_MIN && mCurrentPos.load() < mSeekPositionFloorUs) {
        return mSeekPositionFloorUs;
    }

    return mCurrentPos;
}

void SuperMediaPlayer::NotifyUtcTime()
{
    if (mCurrentFrameUtcTime < 0) {
        return;
    }
    mPNotifier->NotifyUtcTime(mCurrentFrameUtcTime / 1000);
}

void SuperMediaPlayer::SetScaleMode(ScaleMode mode)
{
    if (mode == mSet->scaleMode) {
        return;
    }

    mSet->scaleMode = static_cast<ScaleMode>(mode);
    this->putMsg(MSG_SET_DISPLAY_MODE, dummyMsg);
}

void SuperMediaPlayer::SetRotateMode(RotateMode mode)
{
    if (mode == mSet->rotateMode) {
        return;
    }

    mSet->rotateMode = static_cast<RotateMode>(mode);
    this->putMsg(MSG_SET_ROTATE_MODE, dummyMsg);
}

RotateMode SuperMediaPlayer::GetRotateMode()
{
    return mSet->rotateMode;
}

void SuperMediaPlayer::SetMirrorMode(MirrorMode mode)
{
    if (mode == mSet->mirrorMode) {
        return;
    }

    mSet->mirrorMode = static_cast<MirrorMode>(mode);
    this->putMsg(MSG_SET_MIRROR_MODE, dummyMsg);
}

void SuperMediaPlayer::SetVideoBackgroundColor(uint32_t color)
{
    if (color == mSet->mVideoBackgroundColor) {
        return;
    }

    mSet->mVideoBackgroundColor = color;
    this->putMsg(MSG_SET_VIDEO_BACKGROUND_COLOR, dummyMsg);
}

MirrorMode SuperMediaPlayer::GetMirrorMode()
{
    return mSet->mirrorMode;
}

ScaleMode SuperMediaPlayer::GetScaleMode()
{
    return mSet->scaleMode;
}

int64_t SuperMediaPlayer::GetBufferPosition()
{
    return mBufferPosition / 1000;
}

int64_t SuperMediaPlayer::GetDuration() const
{
    if (mDuration != INT64_MIN) {
        return mDuration / 1000;
    }
    return -1;
}

// TODO: change name to EnableHwDecode
void SuperMediaPlayer::SetDecoderType(DecoderType type)
{
    mSet->bEnableHwVideoDecode = (type == DT_HARDWARE);
}

void SuperMediaPlayer::AddCustomHttpHeader(const char *header)
{
    for (auto &item : mSet->customHeaders) {
        if (item == header) {
            return;
        }
    }

    mSet->customHeaders.emplace_back(header);
}

void SuperMediaPlayer::RemoveAllCustomHttpHeader()
{
    mSet->customHeaders.clear();
}

// TODO: move to mainService thread
void SuperMediaPlayer::setSpeed(float speed)
{
    speed = max(min(speed, MAX_SPEED), MIN_SPEED);
    MsgParam param;
    MsgSpeedParam speedParam;
    speedParam.speed = speed;
    param.msgSpeedParam = speedParam;
    putMsg(MSG_SET_SPEED, param);
}

float SuperMediaPlayer::getSpeed()
{
    return mSet->rate;
}

DecoderType SuperMediaPlayer::GetDecoderType()
{
    std::lock_guard<std::mutex> uMutex(mCreateMutex);

    if (mAVDeviceManager->getVideoDecoderFlags() & DECFLAG_HW) {
        return DT_HARDWARE;
    }

    return DT_SOFTWARE;
}

PlayerStatus SuperMediaPlayer::GetPlayerStatus() const
{
    return mPlayStatus;
}

float SuperMediaPlayer::GetVolume() const
{
    return mSet->mVolume;
}

int64_t SuperMediaPlayer::GetPropertyInt(PropertyKey key)
{
    switch (key) {
        case PROPERTY_KEY_VIDEO_BUFFER_LEN: {
            int64_t duration = mBufferController->GetPacketDuration(BUFFER_TYPE_VIDEO);

            if (duration < 0) {
                duration = mBufferController->GetPacketLastPTS(BUFFER_TYPE_VIDEO) - mBufferController->GetPacketPts(BUFFER_TYPE_VIDEO);
            }

            return duration;
        }

        case PROPERTY_KEY_REMAIN_LIVE_SEG:
            return mRemainLiveSegment;

        case PROPERTY_KEY_NETWORK_IS_CONNECTED:
            return mSourceListener->isConnected();

        case PROPERTY_KEY_RE_BUFFERING:
            return mBufferingFlag;

        case PROPERTY_KEY_DOWNLOAD_COMPLETED:
            return mEof;

        default:
            break;
    }

    return 0;
}

std::string SuperMediaPlayer::GetPropertyString(PropertyKey key, const CicadaJSONItem &param)
{
    switch (key) {
        case PROPERTY_KEY_RESPONSE_INFO: {
            CicadaJSONArray array;
            std::lock_guard<std::mutex> uMutex(mCreateMutex);
            MediaPlayerAnalyticsUtil::addURLProperty("responseInfo", array, mDataSource);
            //if (mDemuxerService->isPlayList())
            {
                std::deque<StreamInfo *> &streamInfoQueue = mMediaInfo.mStreamInfoQueue;
                MediaPlayerAnalyticsUtil::getPropertyJSONStr("responseInfo", array, false, streamInfoQueue, mDemuxerService.get());
            }
            return array.printJSON();
        }

        case PROPERTY_KEY_CONNECT_INFO: {
            std::lock_guard<std::mutex> uMutex(mCreateMutex);

            if (mDataSource) {
                return mDataSource->GetOption("connectInfo");
            }

            return "";
        }

        case PROPERTY_KEY_OPEN_TIME_STR: {
            CicadaJSONArray array;
            CicadaJSONItem item;
            item.addValue("readpacketMS", (double) mFirstReadPacketSucMS);
            array.addJSON(item);
            std::lock_guard<std::mutex> uMutex(mCreateMutex);
            MediaPlayerAnalyticsUtil::addURLProperty("connectInfo", array, mDataSource);
            //if (mDemuxerService->isPlayList())
            {
                std::deque<StreamInfo *> &streamInfoQueue = mMediaInfo.mStreamInfoQueue;
                MediaPlayerAnalyticsUtil::getPropertyJSONStr("openJsonInfo", array, true, streamInfoQueue, mDemuxerService.get());
            }
            return array.printJSON();
        }

        case PROPERTY_KEY_PROBE_STR: {
            CicadaJSONArray array;
            std::lock_guard<std::mutex> uMutex(mCreateMutex);
            MediaPlayerAnalyticsUtil::addURLProperty("probeInfo", array, mDataSource);

            if (nullptr == mDemuxerService) {
                return array.printJSON();
            } else if (mDemuxerService->isPlayList()) {
                std::deque<StreamInfo *> &streamInfoQueue = mMediaInfo.mStreamInfoQueue;
                MediaPlayerAnalyticsUtil::getPropertyJSONStr("probeInfo", array, false, streamInfoQueue, mDemuxerService.get());
            } else {
                CicadaJSONItem item(mDemuxerService->GetProperty(0, "probeInfo"));
                item.addValue("type", "video");
                array.addJSON(item);
            }

            return array.printJSON();
        }

        case PROPERTY_KEY_DELAY_INFO: {
            std::lock_guard<std::mutex> uMutex(mCreateMutex);
            if (nullptr != mDemuxerService) {
                string ret = mDemuxerService->GetProperty(0, "delayInfo");
                return ret;
            }

            return "";
        }
        case PROPERTY_KEY_PLAY_CONFIG: {
            CicadaJSONItem item{};
            item.addValue("http_proxy", mSet->http_proxy);
            item.addValue("refer", mSet->refer);
            item.addValue("timeout_ms", (int) mSet->timeout_ms);
            item.addValue("RTMaxDelayTime", (int) mSet->RTMaxDelayTime);
            item.addValue("startBufferDuration", (int) mSet->startBufferDuration);
            item.addValue("highLevelBufferDuration", (int) mSet->highLevelBufferDuration);
            item.addValue("maxBufferDuration", (int) mSet->maxBufferDuration);
            return item.printJSON();
        }
        case PROPERTY_KEY_DECODE_INFO: {
            CicadaJSONArray decodeInfos{};
            if (HAVE_AUDIO) {
                CicadaJSONItem audioDecodeInfo{};
                audioDecodeInfo.addValue("type", "audio");
                audioDecodeInfo.addValue("createDecodeCost", (int) mRecorderSet->createAudioDecoderCostMs);
                audioDecodeInfo.addValue("decodeFirstCost", (int) mRecorderSet->decodeFirstAudioFrameInfo.getDecodeFirstFrameCost());
                audioDecodeInfo.addValue("firstSize", (int) mRecorderSet->decodeFirstAudioFrameInfo.firstPacketSize);
                audioDecodeInfo.addValue("firstPts", (double) mRecorderSet->decodeFirstAudioFrameInfo.firstPacketPts);
                decodeInfos.addJSON(audioDecodeInfo);
            }
            if (HAVE_VIDEO) {
                CicadaJSONItem videoDecodeInfo{};
                videoDecodeInfo.addValue("type", "video");
                videoDecodeInfo.addValue("createDecodeCost", (int) mRecorderSet->createVideoDecoderCostMs);
                videoDecodeInfo.addValue("decodeFirstCost", (int) mRecorderSet->decodeFirstVideoFrameInfo.getDecodeFirstFrameCost());
                videoDecodeInfo.addValue("firstSize", (int) mRecorderSet->decodeFirstVideoFrameInfo.firstPacketSize);
                videoDecodeInfo.addValue("firstPts", (double) mRecorderSet->decodeFirstVideoFrameInfo.firstPacketPts);
                decodeInfos.addJSON(videoDecodeInfo);
            }

            return decodeInfos.printJSON();
        }
        case PROPERTY_KEY_HLS_KEY_URL: {
            std::lock_guard<std::mutex> uMutex(mCreateMutex);
            if (nullptr != mDemuxerService) {
                return mDemuxerService->GetProperty(0, "keyUrl");
            }

            return "";
        }
        case PROPERTY_KEY_VIDEO_DROPPED_INFO: {
            char dropInfo[MAX_OPT_VALUE_LENGTH] = {0};
            GetOption("videoDroppedInfo", dropInfo);
            return dropInfo;
        }
        case PROPERTY_KEY_NETWORK_SPEED: {
            int64_t from = param.getInt64("from", -1);
            int64_t to = param.getInt64("to", -1);

            std::map<int64_t, int64_t> speeds = mMPAUtil->getNetworkSpeed(from, to);
            CicadaJSONItem value{};
            for (auto &item : speeds) {
                value.addValue(AfString::to_string(item.first), AfString::to_string((int) (item.second / 1024)));
            }

            return value.printJSON();
        }
        case PROPERTY_KEY_BUFFER_INFO: {
            int64_t from = param.getInt64("from", -1);
            int64_t to = param.getInt64("to", -1);
            std::map<int64_t, std::string> bufferInfo = mMPAUtil->getBufferInfo(from, to);
            CicadaJSONItem value{};
            for (auto &item : bufferInfo) {
                value.addValue(AfString::to_string(item.first), item.second);
            }

            return value.printJSON();
        }
        case PROPERTY_KEY_NETWORK_REQUEST_LIST: {
            int64_t from = param.getInt64("from", -1);
            int64_t to = param.getInt64("to", -1);
            return mMPAUtil->getNetworkRequestInfos(from, to);
        }
        case PROPERTY_KEY_RENDER_INFO: {
            return mMPAUtil->getRenderInfoAndReset();
        }
        case PROPERTY_KEY_CONTAINER_INFO: {
            return mContainerInfo;
        }
        default:
            break;
    }

    return "";
}

int SuperMediaPlayer::getCurrentStreamMeta(Stream_meta *meta, StreamType type)
{
    int streamIndex = -1;

    switch (type) {
        case ST_TYPE_VIDEO:
            streamIndex = mCurrentVideoIndex;
            break;

        case ST_TYPE_AUDIO:
            streamIndex = mCurrentAudioIndex;
            break;

        case ST_TYPE_SUB:
            streamIndex = mCurrentSubtitleIndex;
            break;

        default:
            return -EINVAL;
    }

    std::unique_lock<std::mutex> uMutex(mCreateMutex);

    if (streamIndex < 0 || mDemuxerService == nullptr) {
        return -EINVAL;
    }

    return mDemuxerService->GetStreamMeta(meta, streamIndex, false);
}

void SuperMediaPlayer::reLoad()
{
    mSourceListener->enableRetry();
    std::lock_guard<std::mutex> uMutex(mCreateMutex);
    if (mDemuxerService && mDemuxerService->getDemuxerHandle()) {
        mDemuxerService->getDemuxerHandle()->Reload();
    }
}

IVideoRender::Scale SuperMediaPlayer::convertScaleMode(ScaleMode mode)
{
    if (mode == ScaleMode::SM_CROP) {
        return IVideoRender::Scale::Scale_AspectFill;
    } else if (mode == ScaleMode::SM_FIT) {
        return IVideoRender::Scale::Scale_AspectFit;
    } else {
        return IVideoRender::Scale::Scale_Fill;
    }
}

IVideoRender::Rotate SuperMediaPlayer::convertRotateMode(RotateMode mode)
{
    if (mode == RotateMode::ROTATE_MODE_0) {
        return IVideoRender::Rotate::Rotate_None;
    } else if (mode == RotateMode::ROTATE_MODE_90) {
        return IVideoRender::Rotate::Rotate_90;
    } else if (mode == RotateMode::ROTATE_MODE_180) {
        return IVideoRender::Rotate::Rotate_180;
    } else if (mode == RotateMode::ROTATE_MODE_270) {
        return IVideoRender::Rotate::Rotate_270;
    } else {
        return IVideoRender::Rotate::Rotate_None;
    }
}


IVideoRender::Flip SuperMediaPlayer::convertMirrorMode(MirrorMode mode)
{
    switch (mode) {
        case MirrorMode::MIRROR_MODE_HORIZONTAL:
            return IVideoRender::Flip::Flip_Horizontal;
        case MirrorMode::MIRROR_MODE_VERTICAL:
            return IVideoRender::Flip::Flip_Vertical;
        default:
            return IVideoRender::Flip::Flip_None;
    }
}

float SuperMediaPlayer::GetVideoRenderFps()
{
    if (mAVDeviceManager->isVideoRenderValid()) {
        return mAVDeviceManager->getVideoRender()->getRenderFPS();
    }

    return mUtil->getVideoRenderFps();
}


float SuperMediaPlayer::GetVideoDecodeFps()
{
    return 0.0f;
}

void SuperMediaPlayer::NotifyError(int code)
{
    ChangePlayerStatus(PLAYER_ERROR);

    if (mErrorConverter) {
        int newErrorCode;
        std::string outStr;
        int processed = mErrorConverter->ConvertErrorCode(code, newErrorCode, outStr);

        if (processed) {
            AF_LOGE("Player ConvertErrorCode 0x%08x :%s\n", newErrorCode, outStr.c_str());
            mPNotifier->NotifyError(newErrorCode, outStr.c_str());
            return;
        }
    }

    int newErrorCode = framework_error2_code(code);
    char errbuf[128] = {0};
    int isFfmpegError = -1;

    if (newErrorCode == MEDIA_PLAYER_ERROR_UNKNOWN) {
        //maybe ffmpeg error code,,, try get ffmpeg error msg.
        isFfmpegError = get_ffmpeg_error_message(code, errbuf, 128);
    }

    if (isFfmpegError == 0) {
        AF_LOGE("Player ReadPacket ffmpeg error ?? 0x%04x :%s\n", -code, errbuf);
        mPNotifier->NotifyError(MEDIA_PLAYER_ERROR_DEMUXER_OPENSTREAM, errbuf);
    } else {
        char *desc = const_cast<char *>(framework_err2_string(code));
        mPNotifier->NotifyError(newErrorCode, desc);
    }
}

int SuperMediaPlayer::updateLoopGap()
{
    switch (mPlayStatus.load()) {
        case PLAYER_PREPARINIT:
        case PLAYER_PREPARING:
        case PLAYER_PREPARED:
            return 3;

        case PLAYER_PLAYING:
            if (!mFirstRendered) {
                return 3;
            } else if (HAVE_VIDEO) {
                if (mCurrentVideoMeta) {
                    // the loop gap can't too low
                    int fps = std::max(25, (int) (mCurrentVideoMeta->operator Stream_meta *()->avg_fps));
                    if (mVideoInterlaced == InterlacedType_YES) {
                        fps *= 2;
                    }
                    {
#ifdef ENABLE_VIDEO_FILTER
                        std::lock_guard<std::mutex> filterLock(mFilterManagerMutex);
                        if (mFilterManager != nullptr && !mFilterManager->isInvalid(IVideoFilter::Feature::Buffer, "vfi")) {
                            fps *= 2;
                        }
#endif
                    }
                    return 1000 / int((float) fps * mSet->rate * 1.5);
                }
            }
            return 1000 / int(50 * mSet->rate);

        default:
            return 40;
    }
}

int SuperMediaPlayer::mainService()
{

    if (mCanceled) {
        mMainServiceCanceled = true;
        return 0;
    } else {
        mMainServiceCanceled = false;
    }
    int64_t curTime = af_gettime_relative();
    mUtil->notifyPlayerLoop(curTime);
    sendDCAMessage();

    if (mMessageControl->empty() || (0 == mMessageControl->processMsg())) {
        ProcessVideoLoop();
        int loopGap = updateLoopGap();
        int64_t use = (af_gettime_relative() - curTime) / 1000;
        int64_t needWait = loopGap - use;
        // AF_LOGD("use :%lld, needWait:%lld", use, needWait);
        // FIXME : refactor it after made sure the goal of this logic
        if (needWait <= 0) {
            if (loopGap < 5) {
                needWait = 2;
            } else {
                return 0;
            }
        }
        if ((mVideoCatchingUp || mSeekFlag) && getPlayerBufferDuration(false, false) > 0) {
            /*
             * 追赶/seek 期间要尽快再跑一轮，但必须留一点时间片：原来这里
             * 直接 return 0，一次 wait 都不做，主循环变成 0 延时自旋。
             * 4K 硬解 copy-back 一旦追不上实时就会长期停在这个分支，日志里
             * loop index 从 80/s 涨到 177 万/s，一个核被打满，解码线程反而
             * 抢不到 CPU，画面永远追不回来也无法恢复。2ms 足够让出时间片，
             * 又比正常帧间隔（loopGap）小得多，不影响追赶的响应速度。
             */
            if (needWait > 2) {
                needWait = 2;
            }
        }

        std::unique_lock<std::mutex> uMutex(mSleepMutex);
        mPlayerCondition.wait_for(uMutex, std::chrono::milliseconds(needWait), [this]() { return this->mCanceled.load(); });
    }

    return 0;
}

void SuperMediaPlayer::sendDCAMessage()
{
    string event = mDcaManager->getEvent();
    while (!event.empty()) {
        mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_DIRECT_COMPONENT_MSG, event.c_str());
        event = mDcaManager->getEvent();
    }
}

void SuperMediaPlayer::ProcessVideoLoop()
{
    int64_t curTime = af_gettime_relative() / 1000;
    {
#ifndef NDEBUG
        int streamIds[] = {mCurrentVideoIndex,           mCurrentAudioIndex,           mCurrentSubtitleIndex,
                           mWillChangedVideoStreamIndex, mWillChangedAudioStreamIndex, mWillChangedSubtitleStreamIndex};
        int size = sizeof(streamIds) / sizeof(streamIds[0]);

        for (int i = 0; i < size; ++i) {
            for (int j = 0; j < size; ++j) {
                if (i != j && streamIds[i] >= 0) {
                    assert(streamIds[i] != streamIds[j]);
                }
            }
        }

#endif
    }

    if (mSubPlayer) {
        mSubPlayer->onNoop();
    }

    if ((PLAYER_COMPLETION != mPlayStatus && (mPlayStatus < PLAYER_PREPARING || mPlayStatus > PLAYER_PAUSED)) ||
        nullptr == mDemuxerService) {// not working
        if (curTime - mTimerLatestTime > mTimerInterval) {
            OnTimer(curTime);
            mTimerLatestTime = curTime;
        }

        return;
    }
    /*
     * 切换状态机的诊断 + 死线兜底，放在 doReadPacket()/doDeCode() **之前**。
     *
     * 必须在这里的原因：
     *   * 这条死线管的是“提交了但一帧都出不来”的状态（旧流没关、unknown stream
     *     洪水、永久 0 FPS），它不属于别的任何模块的职责；
     *   * ProcessVideoLoop() 在下面还有几处提前 return（buffer 不满足等），
     *     放在这里能保证每轮主循环都跑到。
     */
    logQualitySwitchState();
    checkQualitySwitchDeadline();

    doReadPacket();
    doDeCode();

    // audio render will create after get a frame from decoder

    if (mCanceled) {
        return;
    }
    setUpAVPath();

    /*
     * 【这里什么都不做，原因如下 —— 2026-09-23】
     *
     * 主流播放器（ExoPlayer/ffplay）都不会在播放中途 flush 视频路、也不会周期性地
     * 往前跳关键帧：晚到的帧由 RenderVideo() 逐帧丢弃就够了；管线真的死了走
     * doRender() 里的**一次性**解码器重建（见 VIDEO_RECOVER_STALL_MS 的说明）。
     * 之前那个周期性“巡检 + FlushVideoPath(flushRender=1)”会把渲染器一起 flush，
     * 之后 doReadPacket() 的 read-ahead gate 永久堵住 → 时钟不动、解码器不再消费
     * → 永久卡死（2026-09-23 安卓日志：pause 后 12 秒 flush，然后 read-ahead gate
     * 每秒刷 2 分钟）。这条路径已被彻底删除，不要加回来。
     */

    const bool bufferPass = DoCheckBufferPass();
    if (!bufferPass) {
        /*
         * seek 的完成条件是“目标视频帧已经解码并送入渲染器”，不是重新填满
         * 正常播放所需的整段缓冲。网络较慢或 DASH 关键帧间隔较大时，
         * DoCheckBufferPass() 会暂时返回 false；如果这里直接 return，下面的
         * doRender() 永远拿不到机会，SeekEnd 就会一直等不到（Qt 侧日志中的
         * 2 秒超时）。只要队列里已有 seek 后的有效视频帧，就允许本轮继续到
         * doRender()；音频仍由 render() 按正常缓冲规则控制，不会破坏播放稳定性。
         */
        if (!(mSeekFlag && HAVE_VIDEO && !mVideoFrameQue.empty())) {
            return;
        }
    }

    if (!mBRendingStart && mPlayStatus == PLAYER_PLAYING && !mBufferingFlag) {
        if ((mEof && (!HAVE_AUDIO || mAVDeviceManager->isAudioRenderValid()) && (!HAVE_VIDEO || mAVDeviceManager->isVideoRenderValid())) ||
            // render out the cache frame in renders
            ((!HAVE_VIDEO || !mVideoFrameQue.empty() || (APP_BACKGROUND == mAppStatus)) && (!HAVE_AUDIO || !mAudioFrameQue.empty()))) {
            startRendering(true);
        }
    }

    doRender();
    if (mUtcTimer) {
        if (mFirstRendered) {
            if (mCurrentFrameUtcTime <= 0) {
                AF_LOGW("wrong current frame utc time");
            }
        }

        if (mCurrentFrameUtcTime > 0 && mPlayStatus == PLAYER_PLAYING) {
            //      AF_LOGD("delayTime is utc timer is %lld mCurrentFrameUtcTime us %lld\n",mUtcTimer->get(),mCurrentFrameUtcTime.load());
            LiveTimeSync(mUtcTimer->get() - mCurrentFrameUtcTime);
        }
    } else if (mDuration == 0) {
        int64_t lastAudio = mBufferController->GetPacketLastPTS(BUFFER_TYPE_AUDIO);
        if ((lastAudio != INT64_MIN) && (mPlayedAudioPts != INT64_MIN)) {
            int64_t delayTime = lastAudio - mPlayedAudioPts;
            static int64_t lastT = af_getsteady_ms();

            if (af_getsteady_ms() - lastT > 1000) {
                lastT = af_getsteady_ms();
                AF_LOGD("lastAudio:%lld mPlayedAudioPts:%lld, delayTime:%lld", lastAudio, mPlayedAudioPts, delayTime);
            }
        }
    }
    checkEOS();
    updateBufferInfo(false);

    curTime = af_gettime_relative() / 1000;

    if (curTime - mTimerLatestTime > mTimerInterval) {
        OnTimer(curTime);
        mTimerLatestTime = curTime;
    }
}

void SuperMediaPlayer::updateBufferInfo(bool force)
{
    int64_t videoBufferDuration = INT64_MIN;
    int64_t audioBufferDuration = INT64_MIN;

    if (HAVE_VIDEO) {
        videoBufferDuration = mBufferController->GetPacketDuration(BUFFER_TYPE_VIDEO);
        if (videoBufferDuration < 0) {
            videoBufferDuration =
                    mBufferController->GetPacketLastPTS(BUFFER_TYPE_VIDEO) - mBufferController->GetPacketPts(BUFFER_TYPE_VIDEO);
        }
    }

    if (HAVE_AUDIO) {
        audioBufferDuration = mBufferController->GetPacketDuration(BUFFER_TYPE_AUDIO);
    }

    mMPAUtil->updateBufferInfo(force, videoBufferDuration, audioBufferDuration);
}

void SuperMediaPlayer::doReadPacket()
{
    //check packet queue full
    int64_t cur_buffer_duration = getPlayerBufferDuration(false, false);
    //100s
    mUtil->notifyRead(MediaPlayerUtil::readEvent_Loop, 0);

    if (mEof) {
        return;
    }

    //demuxer read
    int64_t read_start_time = af_gettime_relative();
    int timeout = 10000;
    mem_info info{};
    int checkStep = 0;
    /*
     * 清晰度切换期间，旧 representation 的公共缓冲可能已经达到上限。
     * 如果仍然完全遵守公共缓冲上限，ReadPacket() 会在目标路拿到 init/GOP
     * 之前直接退出，pending decoder 只能消费一小段旧数据，最终永远追不上
     * 主时钟。这里给目标路一个有限的“预热读取额度”，只允许多读少量包，
     * 不会无限扩大内存，也不会让本地文件播放路径产生额外行为。
     */
    int pendingReadAllowance = 16;

    while (true) {
        /*
         * pending 路是预热缓存，不参与公共 buffer duration 统计；没有上限
         * 时 ReadPacket() 会在 active 路仍有大量缓存时持续下载目标流，读取
         * 线程占满 CPU/网络，反过来拖慢旧 decoder，正是“点击后旧帧率降到
         * 1 帧”的另一条路径。保留约两个 GOP，达到上限就让解码线程先消费。
         *
         * 【2026-09-21 修：满不能"整个读循环停掉"，只能"目标路先停"】
         *
         * 读循环是按 demuxer 走的：一次 ReadPacket 拿到的可能是目标流的包，
         * 也可能是当前路/音频的包。原来只要目标队列满就直接 break，于是
         * **当前路和音频一起断粮**。实测（22:10 那份日志，一次 ABR 自动降档）：
         *     activeQ 从 120 一路掉到 0、master 冻结在 4781124 不再前进、
         *     `[switch] … pendingPktQ=120 pendingFrameQ=0` 卡 45 秒以上，
         *     画面 100% 卡住（音频也跟着停 → 又反过来让切换永远提交不了：
         *     主时钟不动，目标路"追上主时钟"这个条件就没法用时间推进来满足）。
         *
         * 所以改成：**只有在当前路确实还吃得住的时候才停读**；当前路（或音频）
         * 快见底时必须继续读。多读进来的目标包由路由处按硬上限兜住（见
         * ProcessVideoPacket 里的 PENDING_VIDEO_QUEUE_HARD_CAP），内存仍然有界。
         */
        if (mPendingVideoStreamIndex >= 0 || mWillChangedVideoStreamIndex >= 0) {
            if (mPendingVideoPacketQue.size() >= PENDING_VIDEO_QUEUE_CAP) {
                const int64_t videoQueuedUs = mBufferController->GetPacketDuration(BUFFER_TYPE_VIDEO);
                const int64_t audioQueuedUs = HAVE_AUDIO
                                                  ? mBufferController->GetPacketDuration(BUFFER_TYPE_AUDIO)
                                                  : INT64_MAX;
                const bool activeComfortable = videoQueuedUs > PENDING_CAP_ACTIVE_MIN_US &&
                                               audioQueuedUs > PENDING_CAP_AUDIO_MIN_US;

                if (activeComfortable) {
                    break;
                }
            }
        }

        // once buffer is full, we will try to read again if buffer consume more then BufferGap
        const bool warmingPending = (mPendingVideoStreamIndex >= 0 ||
                                     mWillChangedVideoStreamIndex >= 0) &&
                                    mPendingVideoPacketQue.size() < 120 &&
                                    pendingReadAllowance > 0;

        /*
         * 【读前闸门：按**播放时钟**限流，而不是按“一共缓冲了多久”】
         *
         * 下面那两个 mBufferIsFull / cur_buffer_duration 判断用的是“队列里一共
         * 存了多少秒”。对一个 VOD 播放器，缓冲 30~90 秒是合理的；但**帧队列只有
         * 2 帧**，而 RenderVideo() 对“比主时钟早 10ms 以上”的帧既不渲染也不丢弃，
         * 所以“合法的深缓冲”等价于“画面停住等时钟”。
         *
         * 实测（2026-09-21 20:20~20:21 那份 Debug 日志）：
         *   * seek 到 16.89s 之后，DASH 的 tracker 游标一路跑到 segNum=6（60s），
         *     3.7 秒后又到 segNum=8（80s）；
         *   * [switch] 行里 activeFrontPts − master 稳定在 **+10 秒**（activeQ=5342 个包）；
         *   * HLS 切换成功（READY）之后，next key pts 75.07s 而 master 66.09s（+9 秒），
         *     随后画面一直停在最后一帧、直到播放位置自己追上去。
         * 三次都是同一个形状：解码器前方压着十几秒未来数据 → 头帧“太早” →
         * 不上屏 → 队列不消费 → 读得更多。
         *
         * 所以这里补一条：**队首（下一个要解码的包）比主时钟超前超过阈值就停读**。
         * 队首是“解码进度”，正常播放时它就贴在播放点上，不该超前几秒。
         * 用队首而不是队尾，是为了不影响“seek 之后把缓冲填满”这条正常行为
         * （那时队首≈seek 目标≈主时钟，填的是它后面的包）。
         *
         * 只在“真的在播”时生效：暂停/缓冲中主时钟不动，读了也不算超前；
         * seek/定位期间更是必须放开读，否则 seek 结束条件都满足不了；
         * pending 预热阶段（warmingPending）也不管 —— 那一段读的是目标
         * Representation 自己的队列，不受公共队列约束。
         *
         * 另外要求音频缓冲至少还有 1 秒：读循环是按 demuxer 走的，停读会连音频
         * 一起停；只有音频这边够吃才敢停，否则为了修画面把声音搞出爆音不值当。
         */
        if (HAVE_VIDEO && !warmingPending && mPlayStatus == PLAYER_PLAYING && !mBufferingFlag &&
            !mSeekFlag && !mSeekNeedCatch && !mPendingVideoDecoderSwitch &&
            (!HAVE_AUDIO || mBufferController->GetPacketDuration(BUFFER_TYPE_AUDIO) > 1000 * 1000)) {
            const int64_t frontPts = mBufferController->GetPacketPts(BUFFER_TYPE_VIDEO);
            const int64_t masterPts = mMasterClock.GetTime();
            /*
             * 【2026-09-21 修：闸门必须再加一条"队列里已经有足够数据"】
             *
             * 只看"队首比时钟超前"是不够的。切清晰度提交之后队列会被清到只剩
             * **一个**包（实测 `activeQ=1`），而那个包按分片边界落在时钟前面
             * 2~3 秒 —— 于是闸门立刻停止读包：解码器没有数据 → 画面停在最后一帧 →
             * 主时钟继续走 → 视频越来越“迟到” → 画面冻得更久。切一次冻一次。
             *
             * 加上"队列里已经有 MAX_VIDEO_READ_AHEAD_US 以上的数据"这一条，
             * 含义就清楚了：**先保证手里有货（至少一个闸门时长的压缩数据），
             * 再按播放时钟限流**。队首在时钟前面 2 秒、后面还压着 2 秒数据 = 4 秒
             * 的余量，这时候停读才是安全的。
             */
            const int64_t queuedDurUs = mBufferController->GetPacketDuration(BUFFER_TYPE_VIDEO);
            /*
             * 【2026-09-22 修：闸门要更保守，而且**切换期间一律不闸**】
             *
             * 22:06 那份日志里出现过一个更难查的连锁：
             *   `read-ahead gate: video packet queue front 35902533 is 6017 ms ahead of
             *    master … (activeQ=120 queued=2001 ms)` 之后，**音频也跟着断粮**
             *   （读循环是按 demuxer 走的，停读 = 音频也停），音频渲染饿死 → 主时钟
             *   不再前进 → 视频被判成"超前 954 ms"→ 画面冻住等一个不动的时钟。
             * 也就是说闸门本身会把"视频超前"变成"整机冻死"。
             *
             * 所以三条一起加：
             *   1) 切换在途（有 pending 目标）时**绝不**停读 —— 目标路要喂、音频也不能饿；
             *   2) 手里要真有货：视频队列 >= 2 个闸门时长（4 秒），而不是刚好 2 秒；
             *   3) 音频队列也要 >= 2 秒才敢停（原来只要求 1 秒）。
             */
            const bool switchInFlight = mPendingVideoStreamIndex >= 0 || mWillChangedVideoStreamIndex >= 0;
            const bool queuedEnough = queuedDurUs >= 2 * MAX_VIDEO_READ_AHEAD_US;
            const bool audioEnough = !HAVE_AUDIO ||
                                     mBufferController->GetPacketDuration(BUFFER_TYPE_AUDIO) > PENDING_CAP_AUDIO_MIN_US;

            if (!switchInFlight && queuedEnough && audioEnough &&
                frontPts != INT64_MIN && masterPts > 0 &&
                frontPts - masterPts > MAX_VIDEO_READ_AHEAD_US) {
                if (floodLogAllowed(FLOOD_READ_AHEAD, 1, "read-ahead gate: video packet queue front is ahead of the clock")) {
                    AF_LOGW("read-ahead gate: video packet queue front %lld is %lld ms ahead of master %lld "
                            "(activeQ=%d queued=%lld ms) — stop reading until the decoder catches up\n",
                            (long long) frontPts, (long long) ((frontPts - masterPts) / 1000),
                            (long long) masterPts, (int) mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO),
                            (long long) (queuedDurUs / 1000));
                }

                break;
            }
        }

        if (mBufferIsFull && !warmingPending) {
            static const int BufferGap = 1000 * 1000;

            if ((mSet->maxBufferDuration > 2 * BufferGap) && (cur_buffer_duration > mSet->maxBufferDuration - BufferGap) &&
                getPlayerBufferDuration(false, true) > mSet->startBufferDuration) {
                break;
            }
        }

        if (cur_buffer_duration > mSet->maxBufferDuration && !warmingPending &&
            getPlayerBufferDuration(false, true) > mSet->startBufferDuration
            // we need readout the buffer in demuxer when no buffer in player, player keep at least start buffer duration
        ) {
            mBufferIsFull = true;
            break;
        }

        mBufferIsFull = false;

        if ((0 >= checkStep--) && (cur_buffer_duration > 1000 * 1000) && (AFGetSystemMemInfo(&info) >= 0)) {
            //AF_LOGD("system_availableram is %" PRIu64 "",info.system_availableram);
            if (info.system_availableram > 2 * mSet->lowMemSize) {
                checkStep = (int) (info.system_availableram / (5 * 1024 * 1024));
            } else if (info.system_availableram < mSet->lowMemSize) {
                AF_LOGW("low memery...");

                if (!mLowMem) {
                    mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_SYSTEM_LOW_MEMORY, "App Low memory");
                }

                mLowMem = true;

                if (mSet->highLevelBufferDuration > 800 * 1000) {
                    mSet->highLevelBufferDuration = 800 * 1000;
                }

                if (mSet->startBufferDuration > 800 * 1000) {
                    mSet->startBufferDuration = 800 * 1000;
                }

                break;
            } else {
                checkStep = 5;
                mLowMem = false;
            }
        }

        int ret = ReadPacket();

        if (warmingPending && pendingReadAllowance > 0) {
            --pendingReadAllowance;
        }

        if (ret == -EAGAIN) {
            if (0 == mDuration) {
                mRemainLiveSegment = mDemuxerService->GetRemainSegmentCount(mCurrentVideoIndex);
            }

            mUtil->notifyRead(MediaPlayerUtil::readEvent_Again, 0);
            break;
        } else if (ret == 0) {
            AF_LOGE("Player ReadPacket EOF");

            if (!mEof) {
                mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_DEMUXER_EOF, "Demuxer End of File");
            }

            if (mPlayStatus == PLAYER_PREPARING) {
                if (HAVE_VIDEO && !mHaveVideoPkt) {
                    closeVideo();
                }
                if (HAVE_AUDIO && !mHaveAudioPkt) {
                    closeAudio();
                }
            }

            mEof = true;
            break;
        } else if (ret == FRAMEWORK_ERR_EXIT) {
            AF_LOGE("Player ReadPacket error 0x%04x :%s\n", -ret, framework_err2_string(ret));
            break;
        } else if (ret == FRAMEWORK_ERR_FORMAT_NOT_SUPPORT) {
            AF_LOGE("read error %s\n", framework_err2_string(ret));
            NotifyError(ret);
            break;
        } else if (ret < 0) {
            if (!mBufferingFlag && mPlayStatus >= PLAYER_PREPARED) {
                //AF_LOGI("Player ReadPacket ret < 0 with data");
            } else {
                AF_LOGE("Player ReadPacket error 0x%04x :%s\n", -ret, framework_err2_string(ret));

                if (ret != FRAMEWORK_ERR_EXIT && !mCanceled) {
                    NotifyError(ret);
                }
            }

            break;
        }

        //AF_LOGI("Player ReadPacket have data");
        if (0 >= mFirstReadPacketSucMS) {
            mFirstReadPacketSucMS = af_getsteady_ms();
        }

        if (af_gettime_relative() - read_start_time > timeout) {
            AF_LOGD("Player ReadPacket time out\n");
            mUtil->notifyRead(MediaPlayerUtil::readEvent_timeOut, 0);
            //                    mMsgProcessTime = 0;
            break;
        }

        cur_buffer_duration = getPlayerBufferDuration(false, false);
        //                if(getPlayerBufferDuration(true) > mSet->maxBufferDuration * 2){
        //                    AF_LOGE("buffer stuffed\n");
        //                    mPNotifier->NotifyError(MEDIA_PLAYER_ERROR_BUFFER_STUFFED,"buffer stuffed");
        //                    break;
        //                }
    }
}

void SuperMediaPlayer::OnDemuxerCallback(const std::string &key, const std::string &value)
{}

bool SuperMediaPlayer::DoCheckBufferPass()
{
    int64_t cur_buffer_duration = getPlayerBufferDuration(false, false);
    int64_t HighBufferDur = mSet->highLevelBufferDuration;

    if (mEof) {
        mDemuxerService->getDemuxerHandle()->setClientBufferLevel(client_buffer_level_normal);
    } else {
        if (cur_buffer_duration < mSet->highLevelBufferDuration) {
            if (mPlayStatus == PLAYER_PLAYING) {
                mDemuxerService->getDemuxerHandle()->setClientBufferLevel(client_buffer_level_low);
            } else {
                mDemuxerService->getDemuxerHandle()->setClientBufferLevel(client_buffer_level_normal);
            }
        } else if (cur_buffer_duration >= mSet->maxBufferDuration - 3 * 1000 * 1000) {
            mDemuxerService->getDemuxerHandle()->setClientBufferLevel(client_buffer_level_low_full);
        } else if (cur_buffer_duration > 2 * mSet->highLevelBufferDuration) {
            mDemuxerService->getDemuxerHandle()->setClientBufferLevel(client_buffer_level_normal);
        } else {
            // TODO:
        }
    }

    if (mFirstBufferFlag && !mEof) {
        HighBufferDur = mSet->startBufferDuration;

        //clean late audio data
        if (cur_buffer_duration > HighBufferDur && HAVE_VIDEO && HAVE_AUDIO) {
            if (mSoughtVideoPos > 0) {
                int64_t count = mBufferController->ClearPacketBeforeTimePos(BUFFER_TYPE_AUDIO, mSoughtVideoPos);
                if (count > 0) {
                    AF_LOGW("clean late audio data %lld before %lld", count, mSoughtVideoPos);
                }
                int64_t pts = mBufferController->GetPacketPts(BUFFER_TYPE_AUDIO);

                if (mRemovedFirstAudioPts == INT64_MIN) {
                    mRemovedFirstAudioPts = pts;

                    if (mFirstAudioPts == INT64_MIN) {
                        mFirstAudioPts = pts - mFirstSeekStartTime;
                    }
                }

                cur_buffer_duration = getPlayerBufferDuration(false, false);

                if (cur_buffer_duration < HighBufferDur) {
                    return false;
                }
            }
        }
    }

    bool isTimeSync = false;
    if (mDemuxerService != nullptr) {
        isTimeSync = mDemuxerService->isWallclockTimeSyncStream(mCurrentVideoIndex);
    }

    if (mPlayStatus == PLAYER_PREPARING) {

        if (isTimeSync) {
            int currentStreamIndex = mCurrentAudioIndex;
            if (!HAVE_AUDIO) {
                currentStreamIndex = mCurrentVideoIndex;
            }
            int64_t durationToStart = mDemuxerService->getDurationToStartStream(currentStreamIndex);
            if (durationToStart < 0) {
                mPNotifier->NotifyPosition(durationToStart / 1000);
                return false;
            }
        }

        if ((cur_buffer_duration >= HighBufferDur &&
             (!HAVE_VIDEO || !mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO) || videoDecoderFull ||
              APP_BACKGROUND == mAppStatus || !mSet->mFastStart)) ||
            (mEof)) {
            if (mEof && getPlayerBufferDuration(true, false) <= 0) {
                // If player don`t get any packets when read eof
                if (mSeekPos > 0) {
                    updateBufferInfo(true);
                    //If caused by before prepare seeked, treat as play completed.
                    mPNotifier->NotifyLoading(loading_event_end, 0);
                    playCompleted();
                } else {
                    ChangePlayerStatus(PLAYER_ERROR);
                    mPNotifier->NotifyError(MEDIA_PLAYER_ERROR_DEMUXER_OPENSTREAM, "open stream failed");
                }
            } else {
                ChangePlayerStatus(PLAYER_PREPARED);
                if (mDemuxerService->isWallclockTimeSyncStream(mCurrentVideoIndex)) {
                    mUtcTimer = mDemuxerService->getDemuxerHandle()->getUTCTimer();
                }
                mDemuxerService->getDemuxerHandle()->SetOption("preferAudio", mSet->preferAudio);
                AF_LOGD("PLAYER_PREPARED");
                AF_LOGD("prepare use %lld ms\n", (af_gettime_relative() - mPrepareStartTime) / 1000);
                notifyPreparedCallback();

                if (mFirstBufferFlag) {
                    mFirstBufferFlag = false;
                }
            }
        } else {

            int64_t duration_v = -1;
            int64_t duration_a = -1;

            if (HAVE_VIDEO) {
                duration_v = mBufferController->GetPacketDuration(BUFFER_TYPE_VIDEO);
            }
            if (HAVE_AUDIO) {
                duration_a = mBufferController->GetPacketDuration(BUFFER_TYPE_AUDIO);
            }

            /**
             * if meta has audio and video stream infos , but after read 2 minutes duration ,
             * one of streams still has no buffer duration , close it to avoid read all packets.
             */
            if (std::min(duration_v, duration_a) == 0 && std::max(duration_v, duration_a) > 2 * 60 * 1000000) {
                if (duration_v > duration_a) {
                    if (mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_AUDIO)) {
                        closeAudio();
                    }
                } else {
                    if (mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)) {
                        closeVideo();
                    }
                }
            }
        }
    }

    //check buffering empty
    if (!mEof && cur_buffer_duration <= 0 && !mBufferingFlag && (mPlayStatus == PLAYER_PLAYING || mPlayStatus == PLAYER_PAUSED)) {
        mBufferingFlag = true;
        /*
         * seek 期间缓存被 ClearPacket 清空，这里必然成立；但这不是"网络卡了"，
         * 而是 seek 自己的等待，界面已经收到 NotifySeeking/NotifySeekEnd，
         * 再发一次 LoadingStart 会让"缓冲中"提示和 seek 提示重复打架
         * （用户看到的就是"seek 为什么要缓冲"）。
         * 内部状态照旧：暂停主时钟、暂停音频渲染，seek 完成时由下面的
         * 缓冲出口统一 start()/pauseAudioRender(false)，所以不通知 UI 也不会
         * 让时钟永远停住。非 seek 的真实卡顿仍然正常通知。
         */
        if (!mSeekFlag) {
            mPNotifier->NotifyLoading(loading_event_start, 0);
            mBufferingNotified = true;
            AF_LOGD("loading start");
        } else {
            mBufferingNotified = false;
            AF_LOGD("seek in progress, parking clock at %lld without showing the buffering UI\n",
                    (long long) mMasterClock.GetTime());
        }
        mLoadingProcess = 0;
        mTimeoutStartTime = INT64_MIN;
        mMasterClock.pause();
        mAVDeviceManager->pauseAudioRender(true);
        return false;
    }

    //AF_LOGD("current duration is %lld,video duration is %lld,audio duration is %lld", cur_buffer_duration
    //	,mBufferController->GetPacketDuration(BUFFER_TYPE_VIDEO), mBufferController->GetPacketDuration(BUFFER_TYPE_AUDIO));
    bool isRealTime = false;
    if (mDemuxerService != nullptr) {
        isRealTime = mDemuxerService->isRealTimeStream(mCurrentVideoIndex);
    }

    while (isRealTime && mSet->RTMaxDelayTime > 0) {
        if (!HAVE_AUDIO) {
            int64_t maxBufferDuration = getPlayerBufferDuration(true, false);

            if (maxBufferDuration > mSet->RTMaxDelayTime + 1000 * 1000 * 5) {
                int64_t lastKeyPos = mBufferController->GetPacketLastKeyTimePos(BUFFER_TYPE_VIDEO);
                mBufferController->ClearPacketBeforeTimePos(BUFFER_TYPE_VIDEO, lastKeyPos);
                break;
            }

            LiveCatchUp(maxBufferDuration);
            break;
        }

        int64_t maxBufferDuration = getPlayerBufferDuration(true, false);

        if (maxBufferDuration > mSet->RTMaxDelayTime + 1000 * 1000 * 5) {
            //drop frame
            int64_t lastVideoPos = mBufferController->GetPacketLastTimePos(BUFFER_TYPE_VIDEO);
            int64_t lastAudioPos = mBufferController->GetPacketLastTimePos(BUFFER_TYPE_AUDIO);
            int64_t lastAudioPts = mBufferController->GetPacketLastPTS(BUFFER_TYPE_AUDIO);
            int64_t lastPos;

            if (lastVideoPos == INT64_MIN) {
                lastPos = lastAudioPos;
            } else if (lastAudioPos == INT64_MIN) {
                lastPos = lastVideoPos;
            } else {
                lastPos = lastAudioPos < lastVideoPos ? lastAudioPos : lastVideoPos;
            }

            lastPos -= min(mSet->RTMaxDelayTime, 500 * 1000);
            int64_t lastVideoKeyTimePos = mBufferController->GetKeyTimePositionBefore(BUFFER_TYPE_VIDEO, lastPos);
            int videoPacketCount = mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO);

            if (videoPacketCount == 0 || lastVideoKeyTimePos != INT64_MIN) {
                AF_LOGD("drop left lastPts %lld, lastVideoKeyPts %lld", lastPos, lastVideoKeyTimePos);
                mMsgCtrlListener->ProcessSetSpeed(1.0);

                int64_t clearPos = (lastVideoKeyTimePos != INT64_MIN) ? lastVideoKeyTimePos : lastPos;
                int64_t dropVideoCount = mBufferController->ClearPacketBeforeTimePos(BUFFER_TYPE_VIDEO, clearPos);
                int64_t dropAudioCount = mBufferController->ClearPacketBeforeTimePos(BUFFER_TYPE_AUDIO, clearPos);

                if (dropVideoCount > 0) {
                    FlushVideoPath(true, true, __func__);
                    AF_LOGD("drop left video duration is %lld,left video size is %d",
                            mBufferController->GetPacketDuration(BUFFER_TYPE_VIDEO), mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO));
                }

                if (dropAudioCount > 0) {
                    FlushAudioPath();
                    AF_LOGD("drop left aduio duration is %lld,left aduio size is %d",
                            mBufferController->GetPacketDuration(BUFFER_TYPE_AUDIO), mBufferController->GetPacketSize(BUFFER_TYPE_AUDIO));
                    mMasterClock.setTime(lastAudioPts);
                }
            }
        }

        int64_t lastAudio = mBufferController->GetPacketLastPTS(BUFFER_TYPE_AUDIO);

        if ((lastAudio != INT64_MIN) && (mPlayedAudioPts != INT64_MIN)) {
            int64_t delayTime = lastAudio - mPlayedAudioPts;
            static int64_t lastT = af_getsteady_ms();

            if (af_getsteady_ms() - lastT > 1000) {
                lastT = af_getsteady_ms();
                AF_LOGD("lastAudio:%lld mPlayedAudioPts:%lld, delayTime:%lld", lastAudio, mPlayedAudioPts, delayTime);
            }

            LiveCatchUp(delayTime);
        }

        break;
    }

    //check buffering status
    if ((mBufferingFlag || mFirstBufferFlag)) {
        if (((cur_buffer_duration > HighBufferDur || (HighBufferDur >= mSet->maxBufferDuration && mBufferIsFull)) &&
             (!HAVE_VIDEO || videoDecoderFull || APP_BACKGROUND == mAppStatus)) ||
            mEof) {
            // if still in seek, wait for seek status be changed.
            if (!mSeekFlag || mEof) {
                if (mBufferingFlag) {
                    mLoadingProcess = -1;
                    updateBufferInfo(true);

                    /* 只有真的给 UI 发过 LoadingStart 才配对发 LoadingEnd；
                     * seek 引起的缓冲是静默的（见上面的入口），发一个没有 start
                     * 的 end 会让界面上的"缓冲中"状态错乱。*/
                    if (mBufferingNotified) {
                        mPNotifier->NotifyLoading(loading_event_end, 0);
                        AF_LOGD("loading end");
                    } else {
                        AF_LOGD("seek buffering finished without a UI notification");
                    }

                    mBufferingNotified = false;

                    if (mPlayStatus == PLAYER_PLAYING) {
                        mMasterClock.start();
                        mAVDeviceManager->pauseAudioRender(false);
                    }
                }

                mTimeoutStartTime = INT64_MIN;
                mFirstBufferFlag = false;
                mBufferingFlag = false;
            }
        } else if (cur_buffer_duration >= 0) {
            // TODO: Notify when change
            int prg = MIN(static_cast<int>(cur_buffer_duration * 100 / HighBufferDur), 100);

            if (prg > mLoadingProcess && mBufferingFlag) {
                mLoadingProcess = prg;
                mPNotifier->NotifyLoading(loading_event_progress, prg);
                mTimeoutStartTime = INT64_MIN;
            } else if (mLoadingProcess == prg) {//15seconds loading progress not changed
                int64_t curTime = af_gettime_relative() / 1000;

                if ((mTimeoutStartTime == INT64_MIN) || (PLAYER_PLAYING != mPlayStatus)) {
                    mTimeoutStartTime = curTime;
                } else if (curTime - mTimeoutStartTime >= mSet->timeout_ms) {
                    //TODO: demuxer still read crazy
                    if (mLowMem) {
                        mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_SYSTEM_LOW_MEMORY, "App Low memory");
                        // if it's a network error or waiting for network to retry, don't notify error
                    } else if (mSourceListener->isConnected() && !mSourceListener->isPending()) {
                        //    ChangePlayerStatus(PLAYER_ERROR);
                        //   notifyErrorCallback(MEDIA_PLAYER_ERROR_LOADING_TIMEOUT, "Loading timeout");
                    }

                    mTimeoutStartTime = curTime;
                }
            }
        }
    }

    return true;
}

void SuperMediaPlayer::closeAudio()
{
    AF_LOGW("close audio stream");
    mDemuxerService->CloseStream(mCurrentAudioIndex);
    mCurrentAudioIndex = -1;
    mMasterClock.setReferenceClock(nullptr, nullptr);
    //    mAudioFrameQue.clear();
    mBufferController->ClearPacket(BUFFER_TYPE_AUDIO);
    FlushAudioPath();
}

void SuperMediaPlayer::closeVideo()
{
    AF_LOGW("close video stream");
    mDemuxerService->CloseStream(mCurrentVideoIndex);
    mCurrentVideoIndex = -1;
    //  mVideoFrameQue.clear();
    mBufferController->ClearPacket(BUFFER_TYPE_VIDEO);
    FlushVideoPath(true, true, __func__);
}

void SuperMediaPlayer::LiveCatchUp(int64_t delayTime)
{
    int recoverGap = 50 * 1000;

    if (mSet->RTMaxDelayTime >= 1000 * 1000) {
        recoverGap = 500 * 1000;
    } else if (mSet->RTMaxDelayTime >= 200 * 1000) {
        recoverGap = 100 * 1000;
    }

    if ((delayTime > mSet->RTMaxDelayTime) && (150 * 1000 < delayTime)) {
        mMsgCtrlListener->ProcessSetSpeed(1.2);
    } else if ((delayTime < mSet->RTMaxDelayTime - recoverGap) || (100 * 1000 > delayTime)) {
        mMsgCtrlListener->ProcessSetSpeed(1.0);
    }
}

void SuperMediaPlayer::LiveTimeSync(int64_t delayTime)
{
    static int64_t lastT = af_getsteady_ms();
    if (af_getsteady_ms() - lastT > 1000) {
        lastT = af_getsteady_ms();
        AF_LOGD("delayTime is %lld rate is %f  buffer duration is %lld\n", delayTime, mBufferingFlag ? 0 : mSet->rate.load(),
                getPlayerBufferDuration(false, false));
    }
    int64_t maxGopTime = mDemuxerService->getDemuxerHandle()->getMaxGopTimeUs();
    if (maxGopTime <= 0) {
        maxGopTime = 2 * 1000 * 1000;
    }
    if (maxGopTime > mSuggestedPresentationDelay) {
        maxGopTime = mSuggestedPresentationDelay;
    }
    assert(mSuggestedPresentationDelay > 0);
    if (mSuggestedPresentationDelay <= 0) {
        return;
    }

    int64_t catchUpBufferDelta = std::max(mSet->highLevelBufferDuration, maxGopTime);

    if (delayTime > (mSuggestedPresentationDelay + 1000 * 1000 * 5) &&
        getPlayerBufferDuration(true, false) > (mSuggestedPresentationDelay + 1000 * 1000 * 5 + catchUpBufferDelta)) {
        //drop frame
        int64_t lateUTCTime = mUtcTimer->get() - (mSuggestedPresentationDelay + 1000 * 1000 * 5);
        int64_t lastKeyTimePos = mBufferController->GetKeyTimePositionBeforeUtcTime(BUFFER_TYPE_VIDEO, lateUTCTime);
        if (lastKeyTimePos == INT64_MIN) {
            lastKeyTimePos = mBufferController->GetKeyTimePositionBeforeUtcTime(BUFFER_TYPE_AUDIO, lateUTCTime);
        }

        // TODO: get the buffer duration after lateUTCTime, if buffer duration is too small, do not clear the buffers
        if (lastKeyTimePos != INT64_MIN) {
            AF_LOGD("drop left lateUTCTime %lld, lastVideoKeyPts %lld", lateUTCTime, lastKeyTimePos);
            int64_t dropVideoCount = mBufferController->ClearPacketBeforeTimePos(BUFFER_TYPE_VIDEO, lastKeyTimePos);
            int64_t dropAudioCount = mBufferController->ClearPacketBeforeTimePos(BUFFER_TYPE_AUDIO, lastKeyTimePos);

            if (dropVideoCount > 0) {
                FlushVideoPath(true, true, __func__);
                AF_LOGD("drop left video duration is %lld,left video size is %d", mBufferController->GetPacketDuration(BUFFER_TYPE_VIDEO),
                        mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO));
            }

            if (dropAudioCount > 0) {
                FlushAudioPath();
                AF_LOGD("drop left audio duration is %lld,left audio size is %d", mBufferController->GetPacketDuration(BUFFER_TYPE_AUDIO),
                        mBufferController->GetPacketSize(BUFFER_TYPE_AUDIO));
                mMasterClock.setTime(mBufferController->GetPacketPts(BUFFER_TYPE_AUDIO));
            }
        }
    }
    int64_t bufferDelay = mSuggestedPresentationDelay;

    switch (mLiveTimeSyncType) {

        case LiveTimeSyncType::LiveTimeSyncNormal:
            assert(mSet->rate == 1.0f);
            if ((delayTime > bufferDelay + maxGopTime / 2) && getPlayerBufferDuration(false, false) > catchUpBufferDelta) {
                mMsgCtrlListener->ProcessSetSpeed(1.2);
                mLiveTimeSyncType = LiveTimeSyncType::LiveTimeSyncCatchUp;
            } else if (delayTime < bufferDelay - maxGopTime / 2) {
                mMsgCtrlListener->ProcessSetSpeed(0.9);
                mLiveTimeSyncType = LiveTimeSyncType::LiveTimeSyncSlowDown;
                AF_LOGD("LiveTimeSync, delayTime=%lld, set speed 0.9", delayTime);
            }
            break;

        case LiveTimeSyncType::LiveTimeSyncCatchUp:
            //   AF_LOGD("CatchUp speed is %f\n", mSet->rate.load());
            assert(mSet->rate == 1.2f);
            if ((delayTime < bufferDelay) || (getPlayerBufferDuration(false, false) < catchUpBufferDelta)) {
                mMsgCtrlListener->ProcessSetSpeed(1.0);
                mLiveTimeSyncType = LiveTimeSyncType::LiveTimeSyncNormal;
                AF_LOGD("LiveTimeSync, delayTime=%lld, recover from catch up", delayTime);
            }
            break;
        case LiveTimeSyncType::LiveTimeSyncSlowDown:
            assert(mSet->rate == 0.9f);
            if (delayTime > bufferDelay) {
                mMsgCtrlListener->ProcessSetSpeed(1.0);
                mLiveTimeSyncType = LiveTimeSyncType::LiveTimeSyncNormal;
                AF_LOGD("LiveTimeSync, delayTime=%lld, recover from slow down", delayTime);
            }
            break;
    }
}

void SuperMediaPlayer::notifyPreparedCallback()
{
    if (waitingForStart && mSet->bLooping) {
        //when loop play don`t send prepare msg.
    } else {
        mPNotifier->NotifyPrepared();
    }

    if (waitingForStart || mAutoPlay) {
        Start();
        waitingForStart = false;
    }

    if (mAutoPlay) {
        mPNotifier->NotifyAutoPlayStart();
    }
}

/*
 * 洪水日志限频（见 SuperMediaPlayer.h 的 FloodLogId 注释）。
 *
 * 语义：每 FLOOD_LOG_WINDOW_MS 一个窗口，窗口内最多原样放行 perWindow 条；
 * 超出的丢进 dropped；窗口切换时（下一条日志进来时）先补一行汇总说明上一窗口
 * 压掉了多少条。这样“密度”和“总量”都还在，只是不再逐帧刷屏。
 */
bool SuperMediaPlayer::floodLogAllowed(FloodLogId id, int perWindow, const char *what)
{
    const int64_t now = af_getsteady_ms();
    FloodLogState &f = mFloodLog[id];

    if (f.windowStartMs == 0 || now - f.windowStartMs >= FLOOD_LOG_WINDOW_MS) {
        if (f.dropped > 0) {
            AF_LOGW("[flood] %s: %lld more lines suppressed in the previous second\n",
                    f.what != nullptr ? f.what : what, (long long) f.dropped);
        }

        f.windowStartMs = now;
        f.emitted = 0;
        f.dropped = 0;
        f.what = what;
    }

    if (f.emitted < perWindow) {
        ++f.emitted;
        return true;
    }

    ++f.dropped;
    return false;
}

/*
 * [switch] 每秒一行：清晰度切换状态机的全部关键量。
 *
 * 出问题时不用再去 grep 几千行洪水日志，这一行就能回答：
 *   - 状态机停在哪一步（warming / committed / none）
 *   - 目标流读到哪了（targetStream / targetFrontPts）
 *   - active 路的包队列头、帧队列深度（判断是“没帧”还是“帧太早被挡”）
 */
void SuperMediaPlayer::logQualitySwitchState()
{
    const bool inSwitch = mPendingVideoStreamIndex >= 0 || mWillChangedVideoStreamIndex >= 0 ||
                          mPendingVideoDecoderSwitch || mQualitySwitchCommitPending;

    if (!inSwitch) {
        return;
    }

    const int64_t now = af_getsteady_ms();

    if (mSwitchStateLogMs != 0 && now - mSwitchStateLogMs < SWITCH_STATE_LOG_INTERVAL_MS) {
        return;
    }

    mSwitchStateLogMs = now;

    const char *state = "warming";

    if (mQualitySwitchCommitPending) {
        state = "committed";
    } else if (mPendingVideoDecoderSwitch) {
        state = "decoderSwitch";
    }

    /*
     * pendingFrontPts/pendingFrontPos 是诊断"切换卡在预热"的关键：只看
     * pendingFrameQ 的深度分不清"目标路还在解码"和"目标路已经解出帧、但帧落在
     * 时间线之外"。日志里那次卡 20 秒就是后者（pendingFrameQ=4，
     * pendingFrontPos 一直停在切换点之前），加上这两个值一眼可判。
     */
    const int64_t pendingFrontPts = mPendingVideoFrameQue.empty()
                                        ? INT64_MIN
                                        : mPendingVideoFrameQue.front()->getInfo().pts;
    const int64_t pendingFrontPos = mPendingVideoFrameQue.empty()
                                        ? INT64_MIN
                                        : mPendingVideoFrameQue.front()->getInfo().timePosition;
    const int64_t joiningLeftMs = (dropLateVideoFrames && mDropLateVideoFramesUntilMs > 0)
                                      ? mDropLateVideoFramesUntilMs - now : -1LL;

    AF_LOGI("[switch] state=%s willChange=%d target=%d current=%d retired=%d master=%lld "
            "activeFrontPts=%lld activeQ=%d frameQ=%d pendingPktQ=%d pendingFrameQ=%d "
            "pendingFrontPts=%lld pendingFrontPos=%lld decoderSwitch=%d commitPending=%d "
            "oldFramesPending=%d joiningLeft=%lld deadlineLeft=%lld\n",
            state, mWillChangedVideoStreamIndex, mPendingVideoStreamIndex, mCurrentVideoIndex,
            mRetiredVideoStreamIndex,
            (long long) mMasterClock.GetTime(),
            (long long) mBufferController->GetPacketPts(BUFFER_TYPE_VIDEO),
            (int) mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO),
            (int) mVideoFrameQue.size(), (int) mPendingVideoPacketQue.size(),
            (int) mPendingVideoFrameQue.size(),
            (long long) pendingFrontPts, (long long) pendingFrontPos,
            (int) mPendingVideoDecoderSwitch, (int) mQualitySwitchCommitPending,
            (int) mQualitySwitchOldFramesPending, (long long) joiningLeftMs,
            mQualitySwitchDeadlineMs > 0 ? (long long) (mQualitySwitchDeadlineMs - now) : -1LL);
}

/*
 * 清晰度切换的唯一终态出口（见 SuperMediaPlayer.h 的说明）。
 *
 * 做四件事，顺序不能变：
 *   1. 通知上层终态（READY / FAILED）—— 界面据此收掉“切换中”；
 *   2. 关掉退役的旧 Representation —— 晚关会导致它继续被读到几十秒之后
 *      （HLS 日志里旧 480p 流被读到 88s），产出的包全部落进 unknown stream；
 *   3. 释放 retired 解码器（含它的硬解 surface 池）；
 *   4. 清干净切换状态机的所有字段。
 *
 * 幂等：没有在途切换时直接返回（会被 RenderVideo / checkQualitySwitchDeadline /
 * FlushVideoPath 三条路调用，必须能重复进）。
 */
void SuperMediaPlayer::finishQualitySwitch(bool ready, const char *reason)
{
    const bool hadCommit = mQualitySwitchCommitPending || mQualitySwitchCommittedStreamIndex >= 0;

    if (!hadCommit) {
        return;
    }

    const int committedStream = mQualitySwitchCommittedStreamIndex;
    const int retiredStream = mRetiredVideoStreamIndex;
    const int64_t committedPts = mPlayedVideoPts;

    if (ready && committedStream >= 0) {
        if (StreamInfo *info = GetCurrentStreamInfo(ST_TYPE_VIDEO)) {
            mPNotifier->NotifyStreamChanged(info, ST_TYPE_VIDEO);
        }
    }

    mPNotifier->NotifyVideoQualitySwitch(ready ? PLAYER_QUALITY_SWITCH_READY : PLAYER_QUALITY_SWITCH_FAILED,
                                         committedStream,
                                         reason != nullptr ? reason : (ready ? "quality switch rendered"
                                                                            : "quality switch finished"));

    /*
     * 关闭退役流的时机：ready 时说明新画面已经上屏，旧 Representation 不再承担
     * 输出；不 ready 时必须**立即**关，否则它会一直被读到很远的位置（实测 88s），
     * 而且每个包都会在 ProcessVideoPacket() 里打 unknown stream。
     */
    if (retiredStream >= 0 && mDemuxerService != nullptr) {
        /*
         * 关流之前先把它在公共队列里的残留包一次清掉：CloseStream 只让 demuxer
         * 不再往这条流上读，已经进队列的那些包还得有人处理 —— 留给解码循环逐个丢
         * 就是上面说的“82 个/秒饿死新路”。（提交时已经清过一次，这里兜第二次，
         * 覆盖“提交到真正关流”之间新到的那批。）
         */
        const int droppedStale = mBufferController->DropPacketsByStream(BUFFER_TYPE_VIDEO, retiredStream);

        if (droppedStale > 0) {
            AF_LOGI("dropped %d stale video packets of the retired stream=%d at finish\n",
                    droppedStale, retiredStream);
        }

        if (retiredStream != mCurrentVideoIndex) {
            mDemuxerService->CloseStream(retiredStream);
            AF_LOGI("closed retired video stream after quality switch (ready=%d): stream=%d\n",
                    (int) ready, retiredStream);
        }
    }

    mAVDeviceManager->releaseRetiredVideoDecoder();

    AF_LOGI("finishQualitySwitch ready=%d stream=%d pts=%lld master=%lld reason=%s\n",
            (int) ready, committedStream, (long long) committedPts,
            (long long) mMasterClock.GetTime(), reason != nullptr ? reason : "-");

    mQualitySwitchCommitPending = false;
    mQualitySwitchCommittedStreamIndex = -1;
    mQualitySwitchOldFramesPending = 0;
    mRetiredVideoStreamIndex = -1;
    mActiveVideoPtsOffset = INT64_MIN;
    mQualitySwitchCommitMs = 0;
    mQualitySwitchDeadlineMs = 0;
}

void SuperMediaPlayer::checkQualitySwitchDeadline()
{
    if (!mQualitySwitchCommitPending || mQualitySwitchDeadlineMs <= 0) {
        return;
    }

    const int64_t now = af_getsteady_ms();

    /*
     * 只有"本来该出帧"的状态才计这条死线。
     *
     * PAUSED 时 doRender() 根本不会调 render()（见 doRender 的 mPlayStatus 判断），
     * 缓冲中（mBufferingFlag）框架也不渲染 —— 这两种情况"没有帧上屏"是正常的，
     * 到点就判 FAILED 会把已经成功的切换误报成失败（界面会把高亮退回旧档）。
     * 所以这两种状态下只把死线往后推，等真正开始播再计时。
     */
    if (mPlayStatus != PLAYER_PLAYING || mBufferingFlag) {
        mQualitySwitchDeadlineMs = now + QUALITY_SWITCH_DEADLINE_MS;
        return;
    }

    /*
     * 【2026-09-21 修：死线衡量的是「没有进展」，不是「没有上屏」】
     *
     * 提交锚点本来就允许比主时钟旧 1~2 秒（4K 目标路预热比实时慢），所以
     * beginRendererJoining() 会开一个追赶窗口，把刚接上的几帧当作迟到帧丢掉 ——
     * 这段时间里**一帧都没上屏是正常的**，但视频路并没有死：窗口本身就是
     * “故意在丢帧等时钟”，并且窗口到点会自己关掉。
     * 只按「有没有上屏」计时，会在切换正常推进的时候判 FAILED：界面收到失败、
     * 高亮退回旧档，而实际上画面已经在切了。
     *
     * 所以留两个“还在进展”的出口：
     *   * 追赶窗口还开着（dropLateVideoFrames 且没到 mDropLateVideoFramesUntilMs）；
     *   * 最近 QUALITY_SWITCH_DEADLINE_MS 内有帧真的上屏（mLastVideoFrameRenderedMs）。
     * 两者都不成立时死线照样命中 —— 兜底能力不变。
     */
    const bool joiningWindowActive = dropLateVideoFrames && mDropLateVideoFramesUntilMs > now;
    const bool frameRenderedRecently = mLastVideoFrameRenderedMs > 0 &&
                                       (now - mLastVideoFrameRenderedMs) < QUALITY_SWITCH_DEADLINE_MS;

    if (joiningWindowActive || frameRenderedRecently) {
        mQualitySwitchDeadlineMs = now + QUALITY_SWITCH_DEADLINE_MS;
        return;
    }

    if (now < mQualitySwitchDeadlineMs) {
        return;
    }

    if (mQualitySwitchWarnMs == 0 || now - mQualitySwitchWarnMs >= SWITCH_STATE_LOG_INTERVAL_MS) {
        mQualitySwitchWarnMs = now;
        AF_LOGW("quality switch deadline hit: committed at stream=%d but no frame reached the "
                "renderer within %d ms (elapsed=%lld ms master=%lld frameQ=%d activeFrontPts=%lld) — "
                "finishing as FAILED so the old stream/decoder are released\n",
                mQualitySwitchCommittedStreamIndex, (int) QUALITY_SWITCH_DEADLINE_MS,
                (long long) (mQualitySwitchCommitMs > 0 ? now - mQualitySwitchCommitMs : -1),
                (long long) mMasterClock.GetTime(), (int) mVideoFrameQue.size(),
                (long long) mBufferController->GetPacketPts(BUFFER_TYPE_VIDEO));
    }

    finishQualitySwitch(false, "quality switch deadline hit (no frame rendered in time)");
}

/*
 * 开始“渲染器追赶窗口”（见 JOINING_DROP_LATE_WINDOW_MS 的说明）。
 *
 * 只置两个东西：dropLateVideoFrames（RenderVideo 和 DecodeVideoPacket 都会读它）
 * 和窗口截止时间。真到了窗口内，迟到帧会被丢掉、过时的包也不会再送去解码，
 * 视频会在几百毫秒到几秒内回到主时钟上，然后自己停掉（出现准时帧时
 * RenderVideo 会把 dropLateVideoFrames 清回 false）。
 */
void SuperMediaPlayer::beginRendererJoining(const char *why)
{
    dropLateVideoFrames = true;
    mDropLateVideoFramesUntilMs = af_getsteady_ms() + JOINING_DROP_LATE_WINDOW_MS;
    AF_LOGI("renderer joining window started (%s): late frames will be dropped for up to %d ms "
            "to catch up with the master clock (master=%lld)\n",
            why != nullptr ? why : "-", (int) JOINING_DROP_LATE_WINDOW_MS,
            (long long) mMasterClock.GetTime());
}

void SuperMediaPlayer::doRender()
{
    bool rendered = false;

    if (mSeekFlag && mPlayStatus != PLAYER_PREPARING) {
        if (HAVE_VIDEO && mAppStatus != APP_BACKGROUND) {
            rendered = RenderVideo(true);

            if (rendered) {
                AF_LOGD("TIMEPOS RenderVideo :%lld", mPlayedVideoPts / 1000);

                /*
                 *  set the position to video position tmp, audio will update the position when it rendered,
                 *  otherwise the position will update to the old audio position when audio not reach on time after seek.
                 *
                 */
                if (mSoughtVideoPos != INT64_MIN) {
                    mCurrentPos = mSoughtVideoPos;
                } else {
                    mCurrentPos = mPlayedVideoPts;
                }
                NotifyPosition(getCurrentPosition());

                // seek preview can't render audio,but set the audio clock to here pts
                if (HAVE_AUDIO && (mAudioTime.startTime <= 0)) {
                    if (!mAudioFrameQue.empty()) {
                        mAudioTime.startTime = mAudioFrameQue.front()->getInfo().pts;
                    } else {
                        mAudioTime.startTime = mPlayedVideoPts;
                    }
                }
            }
        } else {// audio only
            if (!mAudioFrameQue.empty()) {
                NotifyPosition(mAudioFrameQue.front()->getInfo().timePosition);
                rendered = true;
                mCurrentPos = mAudioFrameQue.front()->getInfo().timePosition;
                mAudioTime.startTime = mAudioFrameQue.front()->getInfo().pts;
            }
        }
    }

    if (mPlayStatus == PLAYER_PLAYING) {
        if (!mBufferingFlag) {
            rendered |= render();
        }
    }

    if (rendered) {
        mFirstBufferFlag = false;

        //may audio already played over
        if (mEof && mAudioFrameQue.empty() && mBufferController->GetPacketSize(BUFFER_TYPE_AUDIO) == 0) {
            mMasterClock.setReferenceClock(nullptr, nullptr);
        }

        if (mSeekFlag) {
            // 暂停帧恢复期间：渲染门命中前不能结束 seek —— 门会把目标帧
            // 之前的帧全部挡掉，若按第一帧就宣告 seek 完成，暂停态的
            // 读包/解码管线会停，目标帧永远解码不出来（黑屏根因）
            IDecoder *videoDecoder = mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);
            if (videoDecoder == nullptr || videoDecoder->isRenderGateHit()) {
                /*
                 * 安全网：宣告 seek 完成之前确认主时钟没有跑到 seek 目标**前面**去。
                 * 只要视频还没真的从新位置播过（mPlayedVideoPts == INT64_MIN），
                 * 时钟超前目标点时一律拉回目标点 —— 追赶窗口是按这个时钟决定丢哪些帧的，
                 * 时钟超前就会把目标帧当"迟到帧"丢掉，画面只能等时钟追回来。
                 *
                 * 只处理"超前"：时钟**落后**于目标是无害的（目标帧等时钟到点再上屏，
                 * 这正是正常行为），把它往前拽反而会跳过内容。
                 */
                if (mPlayedVideoPts == INT64_MIN && mSeekPos > 0) {
                    const int64_t clockNow = mMasterClock.GetTime();

                    if (clockNow == INT64_MIN || (clockNow - mSeekPos) > SEEK_CLOCK_TOLERANCE_US) {
                        AF_LOGW("seek completion: master clock %lld is %lld ms AHEAD of seek target %lld, re-park it\n",
                                (long long) clockNow, (long long) ((clockNow == INT64_MIN ? 0 : clockNow - mSeekPos) / 1000),
                                (long long) mSeekPos);
                        mMasterClock.setTime(mSeekPos);
                    }
                }

                mSeekFlag = false;
                /*
                 * seek 完成（目标帧已经上屏）：解码器是从目标点之前的关键帧解过来的，
                 * 队列里可能还压着一段“早于 seek 目标/主时钟”的帧。开一个追赶窗口，
                 * 让这些迟到帧被丢掉、画面尽快和主时钟对齐 —— 否则 seek 之后视频会
                 * 永久落后一段（实测 seek 到 33.6s 后视频停在 37s 且再也不动）。
                 */
                beginRendererJoining("seek finished");

                if (!mMessageControl->findMsgByType(MSG_SEEKTO)) {
                    // update position when seek end. in case of when paused.
                    // update position before reset seek status, so getCurrentPosition return mSeekPos instead of mCurrentPos
                    // fix bug the mCurrentPos not accuracy
                    NotifyPosition(getCurrentPosition());
                    ResetSeekStatus();
                    mPNotifier->NotifySeekEnd(mSeekInCache);
                    mSeekInCache = false;
                }
            }
        }
    }

    /*
     * ================= 管线真死时的一次性恢复（取代原来那个周期性视频路巡检）=================
     *
     * 放在 doRender() 里、每轮主循环判断一次，但**只有下面这些状态同时成立**才会
     * 往下走 —— 也就是“本该有画面在动”的正常播放状态：
     *
     * A. 探测（最小化：一个“最后上屏时间戳” + 包队列采样）
     *    1. 距上一帧**真的上屏**超过一个停滞阈值（正常播放是 VIDEO_RECOVER_STALL_MS；
     *       seek 在途时是宽限大得多的 VIDEO_RECOVER_STUCK_SEEK_MS，见下面的 E）；且
     *    2. 视频包队列**没有在减少**（每 VIDEO_RECOVER_SAMPLE_MS 采一次样，
     *       和上一次的 GetPacketSize() 比：解码器真的死了就不会再消费包）；且
     *    3. 队列里**有包可解**（packets > 0）—— “有输入却长时间不产出”才是 codec
     *       故障（ExoPlayer 的判据）；队列本来就是空的时候属于读取/网络问题，
     *       解码器没活干，重建它治不了病还会平白重置一次渲染器输入队列。
     *    三条一起才成立，才算“管线真死”——只看“没上屏”会把正常的追赶窗口
     *    误判成死亡，只看“队列不动”会把暂停/缓冲/正在下载误判成死亡。
     *
     * B. 动作（一次性 + 冷却）：只做一次受控重建，走 rebuildVideoDecoder(false)
     *    —— invalidateDecoder() + CreateVideoDecoder()，也就是起播时就在用的
     *    那条现成路径（硬解失败自动落软解）。这是**动作**上照主流做
     *    （ExoPlayer 的 releaseCodec() + maybeInitCodecOrBypass()，失败按
     *    enableDecoderFallback 回退）；但**触发**上不是主流 —— ExoPlayer 只由
     *    codec 报错触发，本段是本内核自己的超时兜底（原因见文件头那段说明）。
     *    音频、解复用、主时钟都不动。
     *
     * C. 明确**不做**的事（做过就出过事故）：
     *    * FlushVideoPath()：会把渲染器一起 flush，之后 doReadPacket() 的
     *      read-ahead gate 永久堵住 → 时钟不动、解码器不再消费 → 永久卡死
     *      （2026-09-23 安卓日志）；
     *    * 跳关键帧 / 丢包追赶：播放中做只会把画面丢到时钟前面；seek 的定位
     *      完全由 seek 机制自己负责（SeekTo + seek floor + 追赶窗口），这里不补；
     *    * 周期性重复动作：靠 VIDEO_RECOVER_COOLDOWN_MS 冷却；触发一次后连
     *      “最后上屏时间戳”一起重置，不存在任何形式的周期巡检。
     *
     * D. 另外三个“不是死了”的护栏（在这三种状态下没有新帧是设计如此，
     *    触发只会白重建一次解码器，所以排除掉）：
     *    mBufferingFlag  —— 缓冲中框架本来就不渲染（见上面 render() 的门）；
     *    mEof            —— 播完了当然没有新帧；
     *    APP_BACKGROUND  —— 后台不读包/不解码。
     *
     * E. **seek 在途时的唯一放宽**：正常 seek 期间“暂时没有帧上屏”是对的
     *    （解码器要从目标点之前的关键帧解过来，而且 mSeekPositionFloorUs 会把目标点
     *    之前的帧全丢掉），所以 4s 那个正常阈值不适用于 seek。但 seek 也可能
     *    **永远结束不了**：mSeekFlag 只由 doRender() 里“有帧上屏”那一处清掉，解码器
     *    一旦死了它就是永久真的，于是谁都救不回来 —— 正是用户实测的“seek 完直接卡死”。
     *    所以这里不是“seek 期间不救”，而是“seek 期间多等一会儿再救”：仍要求包队列
     *    完全不消费，只是把停滞阈值放宽到 VIDEO_RECOVER_STUCK_SEEK_MS。
     */
    const bool seekInFlight = mSeekFlag || mSeekNeedCatch;
    const bool videoShouldRender = mPlayStatus == PLAYER_PLAYING && !mBufferingFlag && !mEof &&
                                   mAppStatus != APP_BACKGROUND && HAVE_VIDEO && mAVDeviceManager != nullptr &&
                                   mPendingVideoStreamIndex < 0 && !mPendingVideoDecoderSwitch &&
                                   !mQualitySwitchCommitPending;

    if (videoShouldRender) {
        const int64_t nowMs = af_getsteady_ms();
        const int64_t packets = mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO);
        /* seek 在途 → 宽限阈值；其它时候 → 4s。两者都必须同时满足“包队列不消费”。 */
        const int64_t stallLimitMs = seekInFlight ? VIDEO_RECOVER_STUCK_SEEK_MS : VIDEO_RECOVER_STALL_MS;

        if (mRecoverSampleMs == 0) {
            /* 第一条样本只建立基线，不做判断。 */
            mRecoverSampleMs = nowMs;
            mRecoverSamplePackets = packets;

            if (mLastVideoFrameRenderedMs == 0) {
                /*
                 * 起播以来**一帧都还没上过屏**（Reset() 之后没有任何成功上屏）。
                 * 这时上面“距上次上屏多久”没有基准，探测会永远不成立 —— 而
                 * “解码器从第一帧起就是死的”恰恰是最该救的一种（用户看到的是
                 * 起播黑屏/首帧永远不来）。所以用“进入本该出画状态”的时刻当基准，
                 * 等价于从这里开始计时；一旦有帧真的上屏，它就会被真实时间戳覆盖。
                 *
                 * 只在这里写（主循环线程），不引入跨线程写这个 int64_t。
                 * 暂停/缓冲/切档会走到下面的 else 分支把基线丢掉，重新进入时重新计。
                 */
                mLastVideoFrameRenderedMs = nowMs;
            }
        } else if (nowMs - mRecoverSampleMs >= VIDEO_RECOVER_SAMPLE_MS) {
            /*
             * “队列没有在减少” = 相隔一个采样周期的两次读数里，队列没变浅。
             * 基线是 0 不会永久压制：后来队列涨起来（packets>0）并且不再下降时，
             * `packets >= mRecoverSamplePackets`（0）照样成立，再配合下面的
             * “有包可解”就能正常触发。
             */
            const bool packetsNotDraining = packets >= mRecoverSamplePackets;
            /*
             * “有活可干”：队列里确实有包等着被消费。
             *
             * 这一条不能省：只有“有输入喂不出去”才是解码器死了；队列**本来就是 0**
             * （网络/分片还没读进来、seek 到新位置正在下载）时 `0 >= 0` 也会成立，
             * 但那属于读取/网络问题 —— 解码器根本没活干，重建它治不了病，还会平白
             * 打断一次管线（CreateVideoDecoder() 内部会重置渲染器输入队列）。
             * ExoPlayer 判 codec 故障同样是“有输入却长时间没输出”。
             */
            const bool hasPacketsToDecode = packets > 0;

            mRecoverSampleMs = nowMs;
            mRecoverSamplePackets = packets;

            if (packetsNotDraining && hasPacketsToDecode && mLastVideoFrameRenderedMs > 0 &&
                nowMs - mLastVideoFrameRenderedMs >= stallLimitMs &&
                nowMs >= mVideoRecoverCooldownMs) {
                AF_LOGW("video pipeline looks dead: no frame rendered for %lld ms AND the video packet "
                        "queue is not draining (packets=%lld waiting to be decoded but not consumed, "
                        "master=%lld, seekInFlight=%d) — rebuilding the video decoder once "
                        "(invalidateDecoder + CreateVideoDecoder; audio/demuxer/flush untouched), "
                        "no further attempt within %d ms\n",
                        (long long) (nowMs - mLastVideoFrameRenderedMs), (long long) packets,
                        (long long) mMasterClock.GetTime(), (int) seekInFlight,
                        (int) VIDEO_RECOVER_COOLDOWN_MS);

                mVideoRecoverCooldownMs = nowMs + VIDEO_RECOVER_COOLDOWN_MS;
                /* 除了冷却，再把“最后上屏”时间戳重置一次：即使重建后首帧来得慢，
                 * 也不会在下一轮立刻又满足条件。 */
                mLastVideoFrameRenderedMs = nowMs;
                rebuildVideoDecoder(false);
            }
        }
    } else {
        /* 不在“本该出画面”的状态：丢掉采样基线，回到 PLAYING 时重新建立，
         * 避免拿暂停/切换/缓冲之前的旧样本做判断。 */
        mRecoverSampleMs = 0;
        mRecoverSamplePackets = -1;
    }
}

void SuperMediaPlayer::doDeCode()
{
    //get video packet to decode
    if (HAVE_VIDEO && !videoDecoderEOS && mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)) {
        int max_cache_size = VIDEO_PICTURE_MAX_CACHE_SIZE;

        if (mPictureCacheType == picture_cache_type_cannot) {
            max_cache_size = 1;
        }

        unsigned long videoFrameSize = mVideoFrameQue.size();
        /* pending decoder 必须独立运行，不能因为旧 decoder 的画面队列已满而停止
         * 取包；否则 HLS/DASH 切换永远等不到 pending 的首帧。 */
        const bool hasPendingSwitch = mPendingVideoStreamIndex >= 0;
        /* pending 路必须获得稳定的解码时间片。原实现只有在 active 队列为空
         * 或已满时才取 pending 包；直播流的 active 队列通常永远不为空，导致
         * 目标 decoder 实际上每秒只解出一两帧，日志就会出现“持续丢 pending
         * 帧、FPS=0”。
         *
         * 【2026-09-21 修：每轮 4 个 → PENDING_DECODE_BURST 个】
         *
         * 4 个/轮 × 主循环 27 轮/秒 = 108 包/秒，这就是目标路追赶速度的**硬上限**
         * （实测日志里 pending 帧正好以 108 帧/秒推进），也是 DASH 切换要 7~10 秒
         * 的直接原因：10 秒分片的关键帧要解 600 帧。上限 50ms 的时间片本来就兜住了
         * CPU，轮内多解几个包不会影响旧路；而 pending 解码器一旦输出队列满
         * （RETRY_IN）就会 pendingBlocked 让位给 active 路（见下面的处理），
         * 所以不需要靠"每轮只给 4 个"来保护旧路。 */
        int pendingBurst = 0;
        /*
         * 目标解码器输出队列满（RETRY_IN）时，本轮不要再抢目标队列的包，
         * 把剩下的时间片让给 active 路（见下面 pendingPacket 分支里的说明）。
         */
        bool pendingBlocked = false;

        if (videoFrameSize < max_cache_size || hasPendingSwitch) {
            int64_t startDecodeTime = af_getsteady_ms();
            int64_t videoEarlyUs = 0;

            do {
                if (mCanceled) {
                    break;
                }
                // if still in seeking, don't send data to decoder in background
                // due to the playback position could be changed later.
                if ((APP_BACKGROUND == mAppStatus) && isSeeking()) {
                    break;
                }

                if (mVideoPacket == nullptr) {
                    /* active 路优先消费，pending 路只从独立队列取包。这样
                     * 目标 representation 的 init/GOP 预热不会抢空 active
                     * 路的公共视频队列，旧画面可以持续播放。 */
                    const bool activeQueueFull = mVideoFrameQue.size() >= max_cache_size;
                    /*
                     * 提交之后 mPendingVideoStreamIndex 已经归 -1，但目标队列里
                     * 还剩一段"已读过、没解码"的数据（上限 120 个包 ≈ 2 秒，
                     * 见 TryCommitPendingVideoSwitch 里那段说明）。这段数据是
                     * **当前这条流**时间轴的延续，必须接着解，不能因为
                     * mPendingVideoStreamIndex 归 -1 就绕开它去读公共队列
                     * （那会在时间轴上挖一个 2 秒的洞，画面冻住等时钟）。
                     */
                    const bool leftoverAfterCommit = mPendingVideoStreamIndex < 0 &&
                                                    !mPendingVideoPacketQue.empty();
                    const bool canRunPending = (mPendingVideoStreamIndex >= 0 || leftoverAfterCommit) &&
                                               !pendingBlocked &&
                                               !mPendingVideoPacketQue.empty() &&
                                               (activeQueueFull || leftoverAfterCommit ||
                                                pendingBurst < PENDING_DECODE_BURST);
                    if (canRunPending) {
                        mVideoPacket = std::move(mPendingVideoPacketQue.front());
                        mPendingVideoPacketQue.pop_front();
                        ++pendingBurst;
                    } else {
                        mVideoPacket = mBufferController->getPacket(BUFFER_TYPE_VIDEO);
                        pendingBurst = 0;
                        if (!mVideoPacket && mPendingVideoStreamIndex >= 0 &&
                            !mPendingVideoPacketQue.empty()) {
                            mVideoPacket = std::move(mPendingVideoPacketQue.front());
                            mPendingVideoPacketQue.pop_front();
                            pendingBurst = 1;
                        }
                    }
                }

                /* active 队列已满时仍允许 pending decoder 排空，但不能继续向
                 * active decoder 送包，否则会无限堆积解码输出，反过来拖死 pending。 */
                const bool pendingPacketBeforeDecode = mPendingVideoStreamIndex >= 0 && mVideoPacket &&
                                                        mVideoPacket->getInfo().streamIndex == mPendingVideoStreamIndex;
                if (!pendingPacketBeforeDecode && mVideoFrameQue.size() >= max_cache_size) {
                    break;
                }

                /*
                 * 【注意：下面这一整段是**禁用**的历史代码（条件写死 `false &&`），
                 *  它描述的是“旧码流放完、队首换成新码流时在这里 flush + 换解码器”
                 *  的老做法。当前真正换解码器的地方是 TryCommitPendingVideoSwitch()
                 *  （同一个 doDeCode 循环里调用，走 promotePendingVideoDecoder() 的
                 *  槽位交换，而不是在这里 flush）。看这段代码时不要把它当成现行行为。
                 *
                 * 保留它只是为了记住当时的取舍：清晰度切换的“交叉点”是旧码流的包
                 * 已经放完、队首这一包是新码流的，而且它的 pts 已经追上主时钟
                 * （不是切换瞬间漏出来的那条 pts=0 残留包）。
                 *
                 * 老做法：flush 视频路丢掉旧解码器/旧帧，把解码器置为无效，然后
                 * break 出本轮；同一轮 mainService() 里紧接着的 setUpAVPath()
                 * -> SetUpVideoPath() 会用新流的 meta 重新 updateVideoMeta() +
                 * CreateVideoDecoder()，也就是起播时那条现成的重建路径（含软解兜底），
                 * 并且会 NotifyVideoSizeChanged 把新分辨率告诉 Qt 侧。
                 *
                 * 现行做法（TryCommitPendingVideoSwitch）与它最大的区别：音频路、主时钟、
                 * demuxer 一样都不动，但**不 flush 视频路**（旧帧要继续放完，避免画面断档）。
                 */
                if (false && mPendingVideoDecoderSwitch && mVideoPacket != nullptr &&
                    mVideoPacket->getInfo().streamIndex == mCurrentVideoIndex) {
                    /*
                     * 500ms 容差与 RenderVideo() 里“迟到 500ms 以上另作处理”
                     * 用的是同一个尺度：新码流第一个关键帧可能正好压在时钟上，
                     * 也可能是切流 seek 落在它之前一点点。
                     */
                    const int64_t masterTimeForSwitch = mMasterClock.GetTime();

                    if (masterTimeForSwitch > 0 &&
                        mVideoPacket->getInfo().pts + 500 * 1000 >= masterTimeForSwitch) {
                        /*
                         * FlushVideoPath() 会把 mVideoPacket 置空
                         * （SuperMediaPlayer.cpp:3586），而这一包很可能就是新码流的
                         * 第一个关键帧，丢了它就要再等一个 GOP。先摘下来，重建完放回去。
                         */
                        unique_ptr<IAFPacket> keptPacket = std::move(mVideoPacket);
                        mPendingVideoDecoderSwitch = false;
                        FlushVideoPath(false, false, __func__);
                        mAVDeviceManager->invalidateDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);
                        mVideoPacket = std::move(keptPacket);
                        break;
                    }
                }

                videoEarlyUs = mVideoPacket ? mVideoPacket->getInfo().dts - mMasterClock.GetTime() : 0;

                // don't send too much data when in background
                if (mVideoPacket && APP_BACKGROUND == mAppStatus && videoEarlyUs > 0) {
                    break;
                }

                FillVideoFrame();

                if (nullptr == mVideoPacket && !mEof) {
                    break;
                }

                if (mVideoPacket && (!HAVE_AUDIO || audioDecoderEOS)) {
                    if (!(mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)->getFlags() & DECFLAG_PASSTHROUGH_INFO) &&
                        mDemuxerService->getDemuxerHandle()->isTSDiscontinue()) {
                        if (mVideoPacket->getInfo().timePosition >= 0) {
                            mCurrentPos = mVideoPacket->getInfo().timePosition;
                        }
                        if (mVideoPacket->getInfo().utcTime >= 0) {
                            mCurrentFrameUtcTime = mVideoPacket->getInfo().utcTime;
                        }
                        //printTimePosition(mCurrentPos);
                    }
                }

                int ret = 0;
                // 切流时必须按 streamIndex 将数据分流：旧清晰度继续进入 active
                // decoder，新清晰度只进入 pending decoder。若把两种 codec/分辨率
                // 混在同一个 decoder 中，就会再次出现 PPS/NALU 错误和画面冻结。
                const bool pendingPacket = mPendingVideoStreamIndex >= 0 && mVideoPacket &&
                                            mVideoPacket->getInfo().streamIndex == mPendingVideoStreamIndex;
                if (pendingPacket) {
                    /* DASH/HLS 切流常把新 representation 从 segment 0 的初始化包
                     * 开始送出。若当前播放已经超过 1 秒，这些 pts=0 或明显早于主时钟
                     * 的包必须在解码前丢弃；只在“解出 frame 后”丢弃仍会让解码器先
                     * 花大量时间重建旧时间轴，表现为低帧率甚至 seek 卡死。 */
                    const int64_t packetPts = mVideoPacket->getInfo().pts;
                    const int64_t masterPts = mMasterClock.GetTime();
                    const int64_t switchTolerance = std::max<int64_t>(500 * 1000,
                                                                        mPtsDiscontinueDelta == INT64_MIN ? 0 : mPtsDiscontinueDelta);
                    const bool invalidPts = packetPts == INT64_MIN;
                    /*
                     * 【2026-09-21 新增：预滚包跳过 —— DASH 切换慢的主因】
                     *
                     * 目标路是从"包含切换点的那个分片"的**起点**开始喂数据的，而这个
                     * 起点可能比切换点早十几秒（日志实测：切换点 166.2s，目标流从
                     * 150.0s 开始；DASH 分片 10~20 秒，HLS 只有 4 秒 —— 这就是
                     * "dash 切换时间明显比 hls 长的多"的根源）。这段数据全部解码出来
                     * 又全部丢掉，纯粹白干：4K 目标路解码只有 ~110 帧/秒，10 秒预滚
                     * = 600 帧 = 5.5 秒，切换自然要 7~10 秒甚至超时失败。
                     *
                     * 解法：在**喂给解码器之前**就把预滚丢掉。解码必须从关键帧开始，
                     * 但不必从分片起点开始 —— 保留"切换窗口之前最近的那个关键帧"
                     * 及其之后的数据即可（包里的 flags 就是关键帧标记）。分片内部
                     * 通常每 2~4 秒有一个 IDR，预滚因此从一个分片缩短到一个 GOP。
                     */
                    const int64_t packetTimePos = mVideoPacket->getInfo().timePosition;
                    const bool isKeyFrame = mVideoPacket->getInfo().flags != 0;

                    if (!mPendingVideoPrerollDone && mPendingVideoSwitchTimePosition >= 0 &&
                        packetTimePos >= 0) {
                        /*
                         * 【2026-09-21 第三次修：起点必须是"参考点**之后**的第一个关键帧"】
                         *
                         * 前两版都没做到这一点，所以预滚一直很大（实测
                         * `pending preroll starts at key frame: pts=41708333 timePosition=40000000
                         *  target=58514893 (preroll=18515 ms)` —— 18.5 秒预滚！）。
                         * 原因：代码在遇到**第一个**关键帧时就停下来了，而目标路是从
                         * "包含切换点的分片"的起点开始喂数据的，那个起点可能比切换点早
                         * 十几秒（DASH 分片 10 秒）。18.5 秒预滚要在 9 秒里解完，
                         * 4K 旧路和 480P 新路同时抢解码器 → 用户看到的"从高分辨率切
                         * 低分辨率会卡一下"。
                         *
                         * 现在：参考点之前的包**全部**丢（关键帧也丢），只在遇到
                         * "参考点之后（含）的第一个关键帧"时才开始解码。这样
                         *   * 解码量 ≈ 0（只解主时钟马上要用的那几帧）；
                         *   * 提交点正好落在关键帧边界上 → 音画绝对同步；
                         *   * 新旧两路不会同时长时间解码 → 不卡。
                         * 代价是"点击到生效"最多等一个分片/关键帧间隔（本片源 10 秒分片，
                         * HLS 4.17 秒），等待期间旧路照常播放。
                         *
                         * 参考点取**进入预滚那一刻**的主时钟（只算一次，见
                         * mPendingVideoPrerollRefUs）：只有这样"之后第一个关键帧"才稳定，
                         * 否则主时钟一直在走，已经到达的关键帧会被反复判成"还不够靠后"
                         * 而永远等不到（下一个关键帧可能是一个分片之后）。
                         *
                         * 兜底：等超过 PENDING_PREROLL_WAIT_MAX_MS 还没等到（关键帧间隔
                         * 特别长时），退化成"接受下一个关键帧，哪怕它在参考点之前" ——
                         * 宁可多解一段，也不能让切换永远不完成。
                         */
                        if (mPendingVideoPrerollRefUs == INT64_MIN) {
                            mPendingVideoPrerollRefUs = std::max(mPendingVideoSwitchTimePosition, masterPts);
                        }

                        const bool waitedTooLong = mPendingVideoSwitchStartMs > 0 &&
                                                   (af_getsteady_ms() - mPendingVideoSwitchStartMs) >
                                                       PENDING_PREROLL_WAIT_MAX_MS;

                        if (packetTimePos < mPendingVideoPrerollRefUs) {
                            if (!isKeyFrame || !waitedTooLong) {
                                if (floodLogAllowed(FLOOD_STALE_PENDING, 2, "skip pending preroll packet")) {
                                    AF_LOGD("skip pending preroll packet pts=%lld timePosition=%lld ref=%lld\n",
                                            (long long) packetPts, (long long) packetTimePos,
                                            (long long) mPendingVideoPrerollRefUs);
                                }

                                mVideoPacket.reset();
                                continue;
                            }

                            /* 等太久了：接受这个更早的关键帧，代价是多解一个 GOP。 */
                            mPendingVideoPrerollDone = true;
                            AF_LOGW("pending preroll waited %lld ms without a key frame after the reference, "
                                    "falling back to an earlier key frame: timePosition=%lld ref=%lld\n",
                                    (long long) (af_getsteady_ms() - mPendingVideoSwitchStartMs),
                                    (long long) packetTimePos, (long long) mPendingVideoPrerollRefUs);
                        } else if (isKeyFrame) {
                            mPendingVideoPrerollDone = true;
                            AF_LOGI("pending preroll starts at key frame: pts=%lld timePosition=%lld "
                                    "ref=%lld master=%lld (lead=%lld ms)\n",
                                    (long long) packetPts, (long long) packetTimePos,
                                    (long long) mPendingVideoPrerollRefUs, (long long) masterPts,
                                    (long long) ((packetTimePos - mPendingVideoPrerollRefUs) / 1000));

                            /*
                             * 【关键帧必须自带 codec 参数（SPS/PPS）】
                             *
                             * 预滚跳过会把目标流开头的一大批包丢掉，而"携带 extradata 的
                             * 那一条"往往就在里面（框架是在目标流第一个包上
                             * `setExtraData(meta->extradata, …)` 的）。丢过之后解码器拿到的
                             * 第一帧没有参数集，实测会连刷
                             *     `avcodecDecoder: Error while decoding frame -1094995529
                             *      :Invalid data found when processing input`
                             * 并让切换开头几帧解不出来。这里从 stream meta 重新取一次参数集
                             * 贴到"第一个真正送进解码器的包"上，跳没跳过都不影响。
                             */
                            if (mDemuxerService != nullptr && mPendingVideoStreamIndex >= 0) {
                                unique_ptr<streamMeta> pPrerollMeta;
                                mDemuxerService->GetStreamMeta(pPrerollMeta, mPendingVideoStreamIndex, false);

                                if (pPrerollMeta && mVideoPacket) {
                                    auto *prerollMeta = (Stream_meta *) (*pPrerollMeta);

                                    if (prerollMeta->extradata != nullptr && prerollMeta->extradata_size > 0) {
                                        mVideoPacket->setExtraData(prerollMeta->extradata,
                                                                   prerollMeta->extradata_size);
                                    }
                                }
                            }
                        } else {
                            /* 已经过了参考点但还不是关键帧：继续等下一个关键帧。 */
                            mVideoPacket.reset();
                            continue;
                        }
                    }
                    /* pts=0 的关键帧通常携带 codec extradata，必须保留给 pending
                     * decoder；真正提交前会在 TryCommitPendingVideoSwitch() 丢弃其
                     * 输出。只有非关键的旧零时间包才直接过滤。 */
                    const bool staleZeroPts = packetPts < 0 ||
                                              (packetPts == 0 && !mVideoPacket->getInfo().flags) &&
                                              mPendingVideoPtsOffset == INT64_MIN &&
                                              (mPlayedVideoPts > 1000 * 1000 || masterPts > 1000 * 1000);
                    /* 偏移建立后必须用归一化时间轴比较；否则新 representation
                     * 的原始 PTS（例如 0、12ms）会被误当成远早于主时钟，目标流
                     * 的所有包都会在送入 pending decoder 前被丢掉。 */
                    const int64_t normalizedPacketPts =
                            (!invalidPts && mPendingVideoPtsOffset != INT64_MIN)
                                    ? packetPts + mPendingVideoPtsOffset : packetPts;
                    /* 在 pending decoder 还没有输出首帧之前，偏移尚未建立，
                     * 不能根据原始 PTS 丢包；否则关键帧后面的预测帧会在首帧
                     * 出来前被全部丢掉，decoder 永远无法完成时间轴对齐。 */
                    const bool staleTimelinePts = mPendingVideoPtsOffset != INT64_MIN &&
                                                  !invalidPts && masterPts > 1000 * 1000 &&
                                                  normalizedPacketPts < masterPts &&
                                                  normalizedPacketPts + switchTolerance < masterPts;
                    if (invalidPts || staleZeroPts || staleTimelinePts) {
                        if (floodLogAllowed(FLOOD_STALE_PENDING, 2, "drop stale pending video packet")) {
                            AF_LOGD("drop stale pending video packet pts=%lld master=%lld stream=%d\n",
                                    (long long) packetPts, (long long) masterPts, mPendingVideoStreamIndex);
                        }

                        mVideoPacket.reset();
                        continue;
                    }
                    // pending decoder 延迟创建，避免用户切换清晰度但目标流尚未真正
                    // 产生首包时就破坏当前播放路径。
                    if (!mAVDeviceManager->isPendingVideoDecoderValid()) {
                        Stream_meta pendingMeta{};
                        if (mDemuxerService->GetStreamMeta(&pendingMeta, mPendingVideoStreamIndex, false) >= 0) {
                            CreatePendingVideoDecoder(pendingMeta);
                        }
                    }
                    if (mAVDeviceManager->isPendingVideoDecoderValid()) {
                        // 只解码目标 Representation；active decoder 仍可继续输出旧帧。
                        ret = DecodePendingVideoPacket(mVideoPacket);
                        // pending 队列独立于 active 队列 drain。旧画面即使已经
                        // 缓存满，也不能阻止目标 decoder 继续解码，否则切流会
                        // 永远等不到首帧（HLS 日志中的“点击后无变化”就是此类问题）。
                        DrainPendingVideoFrames();
                        /*
                         * 【2026-09-21 修：这一句必须在这里，不能只在循环尾】
                         *
                         * 目标解码器输出队列满时 DecodePendingVideoPacket 返回
                         * RETRY_IN，而原来的代码在循环尾 `if (ret & STATUS_RETRY_IN)
                         * break;` —— 于是 TryCommitPendingVideoSwitch() **一次都进不去**。
                         * 而把 app 侧那一小撮 pending 帧拿走（提交、或按对齐窗口丢弃）
                         * 的正是它：没人拿 → `mPendingVideoFrameQue` 永远停在 4 →
                         * DrainPendingVideoFrames() 的 `size() < 4` 不成立 → 目标
                         * 解码器输出队列永远是满的 → 双方互相等。
                         *
                         * 实测就是这个形状：`pendingPktQ=120 pendingFrameQ=4` 连续
                         * 56 秒一个字节都不动（21:21:06 → 21:22:02），期间
                         * activeFrontPts 冻结、frameQ=0、画面 100% 卡住、连超时都
                         * 不会触发（超时也在 TryCommit 里面）。
                         */
                        TryCommitPendingVideoSwitch();
                    } else {
                        mPNotifier->NotifyVideoQualitySwitch(PLAYER_QUALITY_SWITCH_FAILED,
                                                             mPendingVideoStreamIndex,
                                                             "pending video decoder is not available");
                        // 目标资源/初始化信息暂时不可用时，立即放弃 pending，
                        // 保留 active decoder 继续播；否则每轮都会重复报错并阻塞解码循环。
                        mAVDeviceManager->discardPendingVideoDecoder();
                        while (!mPendingVideoPacketQue.empty()) {
                            mPendingVideoPacketQue.front()->setDiscard(true);
                            mPendingVideoPacketQue.pop_front();
                        }
                        while (!mPendingVideoFrameQue.empty()) {
                            mPendingVideoFrameQue.front()->setDiscard(true);
                            mPendingVideoFrameQue.pop();
                        }
                        if (mPendingVideoStreamIndex >= 0 && mDemuxerService != nullptr) {
                            mDemuxerService->CloseStream(mPendingVideoStreamIndex);
                            AF_LOGI("closed pending video stream after decoder creation failure: stream=%d\n",
                                    mPendingVideoStreamIndex);
                        }
                        mPendingVideoStreamIndex = -1;
                        mPendingVideoDecoderSwitch = false;
                        mPendingVideoSwitchTimePosition = INT64_MIN;
                        mPendingVideoSwitchStartMs = 0;
                        mPendingVideoInitPacketSent = false;
                        mPendingVideoPrerollDone = false;
                        mPendingVideoPrerollRefUs = INT64_MIN;
                        mRetiredVideoStreamIndex = -1;
                        break;
                    }
                } else {
                    /*
                     * 旧 representation 在双路切换提交前仍然保持 selected，
                     * 因此它可能已经有少量 packet 排在公共视频队列中。提交后
                     * mCurrentVideoIndex 已指向新路，这些旧 packet 不能再喂给
                     * 新 decoder；尤其是 codec/分辨率不同的 DASH/HLS 流，混入
                     * 一个旧包就会触发错误帧、PTS 回退和低帧率。这里只丢弃
                     * 已经失效的旧 stream，正常本地文件和未切换路径不受影响。
                     */
                    if (mVideoPacket && mCurrentVideoIndex >= 0 &&
                        mVideoPacket->getInfo().streamIndex != mCurrentVideoIndex) {
                        if (floodLogAllowed(FLOOD_STALE_DROP, 2, "drop stale video packet after quality switch")) {
                            AF_LOGD("drop stale video packet after quality switch: stream=%d current=%d pts=%lld\n",
                                    mVideoPacket->getInfo().streamIndex, mCurrentVideoIndex,
                                    (long long) mVideoPacket->getInfo().pts);
                        }

                        mVideoPacket.reset();
                        continue;
                    }
                    ret = DecodeVideoPacket(mVideoPacket);
                }

                if (ret & STATUS_RETRY_IN) {
                    /*
                     * 【2026-09-21 修：目标路吃不下时让位给 active 路，而不是整轮结束】
                     *
                     * RETRY_IN 的含义是"解码器输出队列满了，先把输出取走"。对 active
                     * 路直接 break 没问题（下一轮 doDeCode 开头的 FillVideoFrame 会
                     * 排空输出）；但目标路 break 会把整个循环结束掉，**旧画面那一包
                     * 都拿不到** —— 实测目标解码器一被堵住，activeFrontPts 和 frameQ
                     * 就冻结 20~56 秒，画面 100% 卡住而声音正常。
                     *
                     * 所以：目标包退回队列、本轮不再抢目标队列（pendingBlocked），
                     * continue 让 active 路拿一包继续出画。active 路若同样 RETRY_IN
                     * 才真的 break。
                     */
                    if (pendingPacketBeforeDecode) {
                        if (mVideoPacket != nullptr) {
                            mPendingVideoPacketQue.push_front(std::move(mVideoPacket));
                        }

                        pendingBlocked = true;
                        continue;
                    }

                    break;
                }

                /* 目标 decoder 需要持续获得解码机会，但不能在播放器主循环里
                 * 连续占用几百毫秒；否则渲染线程没有机会送出旧帧，用户看到的
                 * 仍然是“点击切换后 FPS=0”。pending 与 active 共用本解码线程，
                 * 因此统一保持短时间片，靠每轮的 pending burst 逐步追赶。 */
                if (af_getsteady_ms() - startDecodeTime > 50) {
                    break;
                }
                TryCommitPendingVideoSwitch();
            /* 预热 pending decoder 时必须持续解码，而不能复用普通播放的
             * “只在 seek 追赶状态循环”的条件。目标流通常从最近关键帧开始，距离
             * 当前时钟可能有数秒；每轮只解一个包会以 1x 实时速度追赶，永远
             * 到不了切换点，于是 FillPendingVideoFrame() 会无限丢帧。限制每轮
             * 50ms CPU 时间即可保护旧路，同时允许 pending 以更高于实时的
             * 速度完成 init/GOP 预热。 */
            } while (mPendingVideoStreamIndex >= 0 ||
                     ((mSeekNeedCatch || dropLateVideoFrames) && (videoEarlyUs < 200 * 1000)));
        }
    }

    //get audio packet to decode
    if (HAVE_AUDIO && mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_AUDIO)) {

        while (mAudioFrameQue.size() < 2 && !audioDecoderEOS && !mCanceled) {

            if (mAudioPacket == nullptr) {
                mAudioPacket = mBufferController->getPacket(BUFFER_TYPE_AUDIO);
            }

            if (mAudioPacket) {
                int64_t timePosition = mAudioPacket->getInfo().timePosition;
                int64_t utcTime = mAudioPacket->getInfo().utcTime;
                int ret = DecodeAudio(mAudioPacket);
                if (mAudioPacket == nullptr &&
                    !(mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_AUDIO)->getFlags() & DECFLAG_PASSTHROUGH_INFO)) {
                    if (timePosition >= 0) {
                        mCurrentPos = timePosition;
                    }
                    if (utcTime >= 0) {
                        mCurrentFrameUtcTime = utcTime;
                    }
                    //printTimePosition(mCurrentPos);
                }
                if (ret == -EAGAIN) {
                    break;
                }
            } else if (mEof) {
                unique_ptr<IAFPacket> packet{};
                DecodeAudio(packet);
            } else
                break;
        }

        //            AF_LOGD("mAudioFrameQue.size is %d\n", mAudioFrameQue.size());
    }
}

bool SuperMediaPlayer::checkEOSAudio()
{

    if (!HAVE_AUDIO) {
        return true;
    }
    if (!audioDecoderEOS) {
        return false;
    }
    int packetSize = mBufferController->GetPacketSize(BUFFER_TYPE_AUDIO);
    int frameSize = static_cast<int>(mAudioFrameQue.size());
    if (frameSize > 0 || packetSize > 0) {
        AF_TRACE;
        return false;
    }

    uint64_t audioQueDuration = mAVDeviceManager->getAudioRenderQueDuration();

    if (audioQueDuration != 0) {
        AF_TRACE;
        //work around: xiaomi 5X 7.1.2 audioTrack getPosition always is 0 when seek to end
        int64_t now = af_getsteady_ms();

        if (mCheckAudioQueEOSTime == INT64_MIN || mAudioQueDuration != audioQueDuration) {
            mCheckAudioQueEOSTime = now;
            mAudioQueDuration = audioQueDuration;
        }

        if ((now - mCheckAudioQueEOSTime) * 1000 <= audioQueDuration) {
            return false;
        }
    }
    return true;
}

bool SuperMediaPlayer::checkEOSVideo()
{
    if (!HAVE_VIDEO) {
        return true;
    }
    if (mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO) && !videoDecoderEOS && (APP_BACKGROUND != mAppStatus)) {
        return false;
    }
    int packetSize = 0;
    int frameSize = 0;
    if ((APP_BACKGROUND != mAppStatus) && mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)) {
        frameSize += mVideoFrameQue.size();
        packetSize += mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO);
    }

    if (frameSize > 0 || packetSize > 0) {
        AF_TRACE;
        return false;
    }
    return true;
}

void SuperMediaPlayer::checkEOS()
{
    if (!mEof || PLAYER_COMPLETION == mPlayStatus) {
        return;
    }
    if (!mVideoEOS) {
        mVideoEOS = checkEOSVideo();
    }
    if (!mAudioEOS) {
        mAudioEOS = checkEOSAudio();
    }
    if (!(mVideoEOS && mAudioEOS)) {
        return;
    }
    NotifyPosition(mDuration);
    playCompleted();
}

void SuperMediaPlayer::playCompleted()
{
    //notify seek completion if seek to the end directly.
    if (mSeekFlag) {
        mSeekFlag = false;

        if (!mMessageControl->findMsgByType(MSG_SEEKTO)) {
            ResetSeekStatus();
            mPNotifier->NotifySeekEnd(mSeekInCache);
            mSeekInCache = false;
        }
    }

    if (mSet->bLooping && mDuration > 0) {
        mSeekPos = 0;//19644161: need reset seek position
        mMsgCtrlListener->ProcessSeekToMsg(0, false);
        mPNotifier->NotifyLoopStart();
        NotifyPosition(0);
    } else {
        if (mPlayStatus != PLAYER_COMPLETION) {
            if (APP_BACKGROUND == mAppStatus) {
                FlushVideoPath(true, true, __func__);
                mBufferController->ClearPacket(BUFFER_TYPE_VIDEO);
            }

            mPNotifier->NotifyCompletion();
            ChangePlayerStatus(PLAYER_COMPLETION);
            mUtil->reset();
        }
    }
}

int SuperMediaPlayer::DecodeVideoPacket(unique_ptr<IAFPacket> &pVideoPacket)
{
    int ret = 0;

    if (videoDecoderEOS) {
        return ret;
    }

    int64_t pos = getCurrentPosition();

    if (pVideoPacket != nullptr) {
        // for cache video, or seeking accurate, check whether drop output frame
        if (mSeekNeedCatch || dropLateVideoFrames) {
            int64_t checkPos = mSeekNeedCatch ? mSeekPos.load() : pos;

            // only decode and don't need output to render if too old
            if ((pVideoPacket->getInfo().timePosition < checkPos) && (pVideoPacket->getInfo().timePosition < mDuration - 200 * 1000)) {
                pVideoPacket->setDiscard(true);
            }
        }

        if (!mRecorderSet->decodeFirstVideoFrameInfo.isFirstPacketSendToDecoder) {
            DecodeFirstFrameInfo &info = mRecorderSet->decodeFirstVideoFrameInfo;
            info.isFirstPacketSendToDecoder = true;
            info.firstPacketSize = pVideoPacket->getSize();
            info.firstPacketPts = pVideoPacket->getInfo().pts;
            info.waitFirstFrame = true;
            info.sendFirstPacketTimeMs = af_getsteady_ms();
        }

        ret = mAVDeviceManager->sendPacket(pVideoPacket, SMPAVDeviceManager::DEVICE_TYPE_VIDEO, 0);
        // don't need pop if need retry later
        if (!(ret & STATUS_RETRY_IN)) {
            //  mBufferController->PopFrontPacket(BUFFER_TYPE_VIDEO);
            assert(pVideoPacket == nullptr);
        }
    } else if (mEof) {
        //     mVideoDecoder->setEOF();
        mAVDeviceManager->sendPacket(pVideoPacket, SMPAVDeviceManager::DEVICE_TYPE_VIDEO, 0);
        ret = 0;
    }

    if (ret > 0) {
        bool haveError = false;
        bool drmError = false;

        if (ret & STATUS_HAVE_ERROR) {
            if (mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)->get_error_frame_no() > MAX_DECODE_ERROR_FRAME) {
                haveError = true;
            }
        }

        if (ret & STATUS_DRM_ERROR) {
            haveError = true;
            drmError = true;
        }

        //            if (ret & STATUS_CREATE_FAIL) {
        //                haveError = true;
        //            }

        if (haveError) {
            /*
             * 【2026-09-24：先按主流做法“release + 重新 init”，建不起来才报致命错误】
             *
             * 主流播放器是**错误驱动**的：ExoPlayer 的 MediaCodecRenderer 在
             * onCodecError()/CodecException 之后走 releaseCodec() + maybeInitCodecOrBypass()
             * （构造参数 enableDecoderFallback 决定失败时是否回退到低优先级解码器），
             * 回退链走完还不行才用 setPendingPlaybackException() 把异常抛给上层。
             * 它**没有**“停摆 N 秒就重启 codec”这种东西 —— 不要照抄那种思路。
             *
             * 本内核的等价信号就是这里：STATUS_HAVE_ERROR 由各平台的
             * send_packet()/getFrame() 负值经 ActiveDecoder::enqueueError() 汇总，
             * 而 get_error_frame_no() 只在**一帧都没出**时才会累积超过阈值
             * （FillVideoFrame 每成功出一帧就 clean_error()），所以它正是
             * “有输入喂进去却长时间没有输出”这个判据，不依赖任何单一平台。
             *
             * 恢复动作走 rebuildVideoDecoder()：invalidateDecoder() + CreateVideoDecoder()，
             * 硬解失败自动落软解 —— 与上面那套 ExoPlayer 语义一一对应，并且只用
             * IDecoder / SMPAVDeviceManager 这层可移植接口（Android MediaCodec、
             * 桌面 FFmpeg + D3D11/DXVA/VAAPI、Apple VideoToolbox 都走同一条路）。
             * 次数上限 MAX_VIDEO_DECODER_REBUILDS = 可用解码器档数（硬解 + 软解），
             * 与“回退链走完就报错”一致，绝不无限重试。
             *
             * DRM 错误不在可恢复之列：重装解码器解决不了密钥/DRM 会话问题，
             * 直接按致命错误上报。
             */
            const bool canRecover = !drmError && mVideoDecodeRebuildCount < MAX_VIDEO_DECODER_REBUILDS;
            bool recovered = false;

            if (canRecover) {
                ++mVideoDecodeRebuildCount;
                AF_LOGW("video decode error: errorFrames=%d without a single output frame, rebuilding the "
                        "video decoder (attempt %d/%d, release + init with hw->sw fallback)\n",
                        (int) mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)->get_error_frame_no(),
                        (int) mVideoDecodeRebuildCount, (int) MAX_VIDEO_DECODER_REBUILDS);

                recovered = (rebuildVideoDecoder(false) >= 0);

                if (recovered) {
                    /* 新解码器从干净状态开始计数（重建成功≠已经有帧，计数由出帧处清零）。 */
                    IDecoder *videoDecoder = mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);
                    if (videoDecoder != nullptr) {
                        videoDecoder->clean_error();
                    }
                }
            }

            if (!recovered) {
                AF_LOGW("video decode error is not recoverable (attempts=%d/%d, drm=%d): reporting a fatal error\n",
                        (int) mVideoDecodeRebuildCount, (int) MAX_VIDEO_DECODER_REBUILDS, (int) drmError);
                ChangePlayerStatus(PLAYER_ERROR);
                mPNotifier->NotifyError(MEDIA_PLAYER_ERROR_DECODE_VIDEO, "video decode error");
            }
        }
    }

    return ret;
}

int SuperMediaPlayer::FillVideoFrame()
{
    int64_t pos = getCurrentPosition();
    unique_ptr<IAFFrame> pFrame{};
    int ret = mAVDeviceManager->getFrame(pFrame, SMPAVDeviceManager::DEVICE_TYPE_VIDEO, 0);

    if (ret == STATUS_EOS) {
        videoDecoderEOS = true;

        if (mSeekFlag && mSeekNeedCatch) {
            mSeekNeedCatch = false;
        }
    }

    if (pFrame != nullptr) {

        if (mSeekFlag && mPlayStatus == PLAYER_PAUSED) {
            AF_LOGI("PFR: frame pulled pts=%" PRId64 " seekFlag=%d\n", pFrame->getInfo().pts, (int) mSeekFlag);
        }

        if (mRecorderSet->decodeFirstVideoFrameInfo.waitFirstFrame) {
            DecodeFirstFrameInfo &info = mRecorderSet->decodeFirstVideoFrameInfo;
            info.getFirstFrameTimeMs = af_getsteady_ms();
            info.waitFirstFrame = false;
        }

        mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)->clean_error();
        /* 解码器又能出帧了 = “codec 恢复正常”：把错误驱动的重建次数清零，
         * 下次再出问题仍然允许重新尝试 release + init（语义见 DecodeVideoPacket）。 */
        mVideoDecodeRebuildCount = 0;

        /* 切换提交后，目标 decoder 的每一帧都必须使用与首帧相同的时间轴偏移。
         * 只修正首帧会导致后续帧突然跳回原始 rendition PTS，RenderVideo() 便会
         * 把它们当成严重落后帧持续丢弃，日志表现为 0/1 FPS。 */
        if (mActiveVideoPtsOffset != INT64_MIN && pFrame->getInfo().pts != INT64_MIN) {
            pFrame->getInfo().pts += mActiveVideoPtsOffset;
        }

        if (mSecretPlayBack) {
            pFrame->setProtect(true);
        }
        int64_t pts = pFrame->getInfo().pts;

        /* 切换完成后的首帧必须和音频主时钟处在同一时间轴。这里保留一条
         * 轻量日志，便于确认实际送入渲染器的归一化视频 PTS 与音频 PTS
         * 是否仍有明显偏差；不改变本地视频的时钟行为。 */
        if (mQualitySwitchCommitPending) {
            AF_LOGD("quality switch video frame pts=%lld master=%lld audio=%lld\n",
                    (long long) pts, (long long) mMasterClock.GetTime(),
                    (long long) mPlayedAudioPts);
        }

        if (mSeekFlag && mSeekNeedCatch) {
            mSeekNeedCatch = false;
        }

        auto *meta = (Stream_meta *) (mCurrentVideoMeta.get());

        if (!mAdaptiveVideo && mVideoWidth > 0 && (pFrame->getInfo().video.width != mVideoWidth || pFrame->getInfo().video.height != mVideoHeight)) {

            // displayWidth and displayHeight is changed, but we can't know the sar for now

            // TODO: get the sar from frame or update current video meta
            meta->displayWidth = meta->displayHeight = 0;
        }

        if (meta->displayWidth > 0 && meta->displayHeight > 0) {
            pFrame->getInfo().video.dar = 1.0 * meta->displayWidth / meta->displayHeight;
        } else {
            //if not get displaywidth/height , set dar with width/height
            pFrame->getInfo().video.dar = 1.0 * pFrame->getInfo().video.width / pFrame->getInfo().video.height;
        }

        mDemuxerService->SetOption("V_FRAME_DECODED", pts);
        //            AF_LOGI("DecodeVideoPacket p_dec_delay frame :%lld pos:%lld, mPlayedAudioPts:%lld, posdiff:%lld audiodiff:%lld videodiff:%lld",
        //                    pFrame->GetPts()/1000, pos/1000, mPlayedAudioPts/1000, (pos - pFrame->GetPts())/1000,
        //                    (mLastInputAudio - mPlayedAudioPts)/1000, (mLastInputVideo - pFrame->GetPts())/1000);


        int format = pFrame->getInfo().video.format;
        bool success = push(pFrame);
        if (success) {
            while (true) {
                std::unique_ptr<IAFFrame> frame = nullptr;
                success = pull(format, frame);
                if (success) {
                    mVideoFrameQue.push(std::move(frame));
                } else {
                    break;
                }
            }
        } else {
            mVideoFrameQue.push(std::move(pFrame));
        }

        videoDecoderFull = true;
    }

    return ret;
}

int SuperMediaPlayer::DecodePendingVideoPacket(unique_ptr<IAFPacket> &pVideoPacket)
{
    if (!pVideoPacket) return 0;
    int ret = mAVDeviceManager->sendPendingVideoPacket(pVideoPacket, 0);
    if (!(ret & STATUS_RETRY_IN)) assert(pVideoPacket == nullptr);
    return ret;
}

int SuperMediaPlayer::FillPendingVideoFrame()
{
    /* 一个 representation 的 init 分片可能连续产生多张“PTS=0/无媒体位置”的
     * 解码输出。这里最多排空 8 张初始化/旧时间轴帧，直到拿到真正接近主时钟的
     * 媒体帧；不能只取一张就返回，否则 TryCommitPendingVideoSwitch() 会把 init
     * 帧当成切换完成。 */
    /*
     * 【2026-09-21 修：单次调用最多排空 PENDING_PREROLL_DRAIN_MAX 帧】
     *
     * 原来是死循环 8 次。目标路从"分片边界"开始解码时，要丢弃的预滚帧可能**几百张**
     * （DASH 分片 10 秒 × 60fps = 600 张），而每调用一次只丢 8 张 —— 600 张要 75 次
     * 调用，1024 级别的量级直接决定了切换要几秒（实测 DASH 7~10 秒、HLS 0.2~1.3 秒，
     * 差别就在分片长度）。丢帧本身只是 setDiscard+析构，不花时间，所以把上限提上去。
     */
    for (int i = 0; i < PENDING_PREROLL_DRAIN_MAX; ++i) {
        unique_ptr<IAFFrame> frame;
        int ret = mAVDeviceManager->getPendingVideoFrame(frame, 0);
        if (ret != 0 && !frame) {
            return ret;
        }
        if (!frame) {
            return ret;
        }

        const int64_t framePts = frame->getInfo().pts;
        const int64_t frameTimePosition = frame->getInfo().timePosition;
        const int64_t masterPts = mMasterClock.GetTime();

        /* 目标 representation 从其最近关键帧预热，但只保留切换点附近的帧。
         * 例如切换点 41.7s、目标流关键帧 33.3s 时，33.3s 这一整段不能成为
         * 新路的首帧；继续解码到 41.7s 即可，旧路在此期间仍负责显示。 */
        if (mPendingVideoSwitchTimePosition >= 0 && frameTimePosition >= 0 &&
            frameTimePosition + 2 * 1000 * 1000 < mPendingVideoSwitchTimePosition) {
            if (floodLogAllowed(FLOOD_PENDING_DROP, 2, "drop pending frame before switch window")) {
                /*
                 * 用 AF_LOGI 而不是 AF_LOGD：这条以前只在 Debug 级别，日志里只剩
                 * 一句 "[flood] … N more lines suppressed"，看不到**数值**，
                 * 卡住的时候完全没法判断"目标流到底解到哪儿了"。现在每次限频
                 * 输出里带上 pts/timePosition/目标点。
                 */
                AF_LOGI("drop pending frame before switch window: pts=%lld timePosition=%lld "
                        "target=%lld (lag=%lld ms)\n",
                        (long long) framePts, (long long) frameTimePosition,
                        (long long) mPendingVideoSwitchTimePosition,
                        (long long) ((mPendingVideoSwitchTimePosition - frameTimePosition) / 1000));
            }

            frame->setDiscard(true);
            continue;
        }

        /* 不能用不断前进的 masterPts 过滤 pending 帧。目标 representation
         * 通常从切换点之前最近的关键帧开始，网络/解码速度又可能暂时落后主
         * 时钟数秒；若这里按 masterPts 每帧丢弃，目标路永远追不上，日志会
         * 反复出现“drop pending init/old frame”并最终 FPS=0。真正需要丢弃的
         * 只有固定切换目标之前的旧 segment，上面的 switch-window 判断已经
         * 完成；主时钟仅在 TryCommitPendingVideoSwitch() 中作为提交门限。 */

        /* 某些硬解路径不会把 packet metadata 复制到输出帧。此时只能用 PTS
         * 识别 init 帧：主时钟已经运行后，0~5ms 的输出不能作为切换锚点。 */
        /* PTS=0 也不能单独视为旧帧：这是每个 representation 的 init 时间轴，
         * 后续媒体帧仍必须经过 pending decoder 才能建立正确的 codec 状态。若
         * timePosition 在切换窗口内，保留该帧并让提交阶段决定是否可显示。 */

        /* DASH/HLS 的 representation 可能把解码器 PTS 从 0 重新开始，
         * 但 packet metadata 中的 timePosition 仍是全局媒体时间。
         *
         * 【2026-09-21 修：只要有 timePosition 就**一律**用它当 pts】
         *
         * 原来只在 |pts − timePosition| > 2 秒时才归一化。实测 DASH 各档的
         * pts 与 timePosition 恒定差 0.9~3.4 秒（例如
         * `pts=70904167 timePosition=70000000`），差值小于 2 秒的那些档**不做**
         * 归一化，于是 promote 到 active 队列的帧带着"差 0.9 秒"的 pts 进入
         * RenderVideo，和主时钟（全局时间轴）一比就是音画错位 —— 用户说的
         * "切换之后音画不太同步"有一部分就是它。主时钟、公共队列、timePosition
         * 全在全局时间轴上，所以这里没有理由保留另一条轴上的值。
         */
        if (frameTimePosition >= 0) {
            /*
             * 记录"解码器原始 PTS → 全局 timePosition"的偏移，并且**留给提交后的
             * active 路径继续用**（见 mActiveVideoPtsOffset 的说明）。
             *
             * DASH 各档的原始 PTS 与全局 timePosition 会差一个固定量（实测同一档
             * 恒定差 0.9~3.4 秒，例如 `pts=33366667 timePosition=30000000`）。
             * pending 帧在这里逐帧归一化没问题，但**提交之后**同一解码器的后续帧是
             * 走 FillVideoFrame() 的，那里没有 packet metadata、只能用
             * mActiveVideoPtsOffset 平移 —— 而这个偏移以前只在"完全没有 media time"
             * 的罕见情况下才建立。于是提交后视频 PTS 直接跳回原始时间轴（比主时钟
             * 超前 0.9~3.4 秒）：RenderVideo() 认为"帧太早"不上屏 → 画面卡住等时钟
             * （实测 `video path stalled 1003 ms but the video is 2545 ms AHEAD of the
             * master clock`），之后画面内容还永久落后音频同样的时长。
             *
             * **DASH 必现、HLS 不出现**（HLS 的原始 pts == timePosition，偏移恒为 0），
             * 这就是"dash 切换 99% 会卡一下、hls 很流畅"的根因。
             */
            if (framePts != INT64_MIN && framePts != frameTimePosition) {
                mPendingVideoPtsOffset = frameTimePosition - framePts;
            }

            if (framePts != frameTimePosition &&
                floodLogAllowed(FLOOD_PENDING_NORMALIZE, 2, "normalize pending frame pts")) {
                AF_LOGD("normalize pending frame pts=%lld to timePosition=%lld stream=%d\n",
                        (long long) framePts, (long long) frameTimePosition,
                        mPendingVideoStreamIndex);
            }

            frame->getInfo().pts = frameTimePosition;
        }

        /* 某些 HLS/DASH rendition 的 PTS 基于各自 segment 时间轴，切换时可能
         * 与当前主时钟相差一个或多个 segment。把差值记录一次并应用到后续
         * pending 帧，音频和主时钟保持不变，画面即可在当前播放点接上。 */
        /*
         * 目标 representation 的首个可解码帧可能来自新的 segment，DASH/HLS
         * 实现经常把它的 PTS 从 0 或几毫秒重新开始。之前用“framePts 必须大于
         * 5ms”过滤了这种情况，导致 pending decoder 提交后视频时间轴跳回开头，
         * RenderVideo() 随后不断丢帧，而音频主时钟仍继续向前，最终出现严重
         * 音画不同步。
         *
         * 这里用主时钟作为唯一锚点：只要目标首帧与当前主时钟相差超过 1 秒，
         * 无论首帧 PTS 是 0、几毫秒还是完整媒体时间，都建立同一个偏移，并
         * 应用到该 decoder 后续输出。相差在 1 秒内的帧保持原始 PTS，避免对
         * 正常连续时间轴做不必要的修正。
         */
        /* 只有 demuxer 没有提供媒体位置时，才用 PTS 建立备用时间轴偏移。
         * 正常 DASH/HLS rendition 的 frameTimePosition 已经是绝对媒体时间，
         * 即使首个关键帧比主时钟早/晚一个 segment，也不能把它强行平移；
         * 否则会制造新的音画时钟跳变。 */
        if (mPendingVideoPtsOffset == INT64_MIN && frameTimePosition < 0 &&
            framePts != INT64_MIN && masterPts > 1000 * 1000 &&
            llabs(framePts - masterPts) > 1000 * 1000) {
            mPendingVideoPtsOffset = masterPts - framePts;
            AF_LOGI("align pending video timeline: framePts=%lld masterPts=%lld offset=%lld\n",
                    (long long) framePts, (long long) masterPts,
                    (long long) mPendingVideoPtsOffset);
        }
        mPendingVideoFrameQue.push(std::move(frame));
        return ret;
    }

    return 0;
}

void SuperMediaPlayer::DrainPendingVideoFrames()
{
    if (!mAVDeviceManager->isPendingVideoDecoderValid()) {
        return;
    }

    // 一次最多保留一个 GOP 左右的目标帧，防止用户连续切换时 pending
    // 队列无限增长；但至少要拿到第一帧，提交条件才可能满足。
    while (mPendingVideoFrameQue.size() < VIDEO_PICTURE_MAX_CACHE_SIZE * 2) {
        if (FillPendingVideoFrame() != 0 || mPendingVideoFrameQue.empty()) {
            break;
        }
    }
}

int SuperMediaPlayer::CreatePendingVideoDecoder(const Stream_meta &meta)
{
    bool bHW = mSet->bEnableHwVideoDecode;
    uint64_t flags = bHW ? DECFLAG_HW : DECFLAG_SW;
    if (mAdaptiveVideo) flags |= DECFLAG_ADAPTIVE;
    if (mSet->bLowLatency) flags |= DECFLAG_OUTPUT_FRAME_ASAP;
    // pending decoder 不能绑定 active decoder 正在使用的同一个 surface。
    // 对 FFmpeg/D3D11、VideoToolbox 等帧输出解码器，nullptr 表示输出解码帧；
    // 对要求独占 surface 的平台硬解则走软解兜底，避免两个 decoder 争用同一
    // surface 导致死锁、黑屏或极低帧率。
    int ret = mAVDeviceManager->setUpPendingVideoDecoder(flags, &meta, nullptr, 0);
    if (ret < 0 && bHW) {
        ret = mAVDeviceManager->setUpPendingVideoDecoder(DECFLAG_SW, &meta, nullptr, 0);
    }
    if (ret < 0) {
        mPNotifier->NotifyVideoQualitySwitch(PLAYER_QUALITY_SWITCH_FAILED,
                                             mPendingVideoStreamIndex,
                                             "pending video decoder creation failed");
        mAVDeviceManager->discardPendingVideoDecoder();
        while (!mPendingVideoPacketQue.empty()) {
            mPendingVideoPacketQue.front()->setDiscard(true);
            mPendingVideoPacketQue.pop_front();
        }
    }
    return ret;
}

bool SuperMediaPlayer::TryCommitPendingVideoSwitch()
{
    // 提交条件是 pending 至少解出一帧。此时切换只替换帧队列和 decoder 指针，
    // 音频、主时钟和 demuxer 都不重置，因此用户感知到的是连续播放。
    if (mPendingVideoStreamIndex < 0 || mPendingVideoFrameQue.empty()) return false;
    const int64_t master = mMasterClock.GetTime();
    const int64_t frameTimePosition = mPendingVideoFrameQue.front()->getInfo().timePosition;

    /*
     * 目标路允许从切换点之前最近的关键帧开始解码，但不能把远早于切换点
     * 的连续帧逐张丢到几十秒后。这里设置一个有限的预热窗口：
     *   - 关键帧/首个可解码帧最多早于目标 2 秒；
     *   - 早于窗口的帧直接丢弃，等待解码器继续向前；
     *   - 目标路若超过 8 秒仍没有追上，认为本次切换失败，恢复 active 路。
     * 这与 ExoPlayer/AVPlayer 的“pending period 超时回退”原则一致，避免
     * 网络慢或不连续清单让切换状态永远卡住。
     */
    if (mPendingVideoSwitchTimePosition >= 0 && frameTimePosition >= 0 &&
        frameTimePosition + 2 * 1000 * 1000 < mPendingVideoSwitchTimePosition) {
        mPendingVideoFrameQue.front()->setDiscard(true);
        mPendingVideoFrameQue.pop();
        return false;
    }
    const int64_t switchElapsedMs =
        mPendingVideoSwitchStartMs > 0 ? af_getsteady_ms() - mPendingVideoSwitchStartMs : 0;
    /*
     * 预热超时必须覆盖两种截然不同的失败：
     *
     *   1) 目标路一直追不上播放时间线（frameTimePosition 落后切换点 2 秒以上）。
     *      这条用 8 秒，和 ExoPlayer 的 pending period 超时同量级。
     *   2) 目标路解码正常、帧也不落后，但就是凑不出满足提交条件的帧。日志里实测
     *      出现过 `pendingPktQ=120 pendingFrameQ=4` 卡住 20 秒不动的情况。
     *
     * 第 2 种必须是**无条件**的绝对上限，否则这次之前那条带 `frameTimePosition`
     * 判据的超时永远不会命中：状态机被永久钉在 decoderSwitch —— 旧流关不掉、
     * retired 解码器不还、上层永远收不到终态；更糟的是 mPendingVideoDecoderSwitch
     * 一直为 true，RenderVideo() 会绕过全部节流（这就是帧率掉到 1 FPS 的直接原因）。
     */
    const bool switchTimedOut =
        switchElapsedMs > 0 &&
        (switchElapsedMs > QUALITY_SWITCH_TOTAL_TIMEOUT_MS ||
         (switchElapsedMs > 8000 &&
          (frameTimePosition < 0 ||
           (mPendingVideoSwitchTimePosition >= 0 &&
            frameTimePosition + 2 * 1000 * 1000 < mPendingVideoSwitchTimePosition))));
    if (switchTimedOut) {
        AF_LOGW("quality switch timed out before commit: stream=%d target=%lld frame=%lld "
                "elapsed=%lld pendingPktQ=%d pendingFrameQ=%d master=%lld\n",
                mPendingVideoStreamIndex,
                (long long) mPendingVideoSwitchTimePosition,
                (long long) frameTimePosition, (long long) switchElapsedMs,
                (int) mPendingVideoPacketQue.size(), (int) mPendingVideoFrameQue.size(),
                (long long) master);
        mPNotifier->NotifyVideoQualitySwitch(PLAYER_QUALITY_SWITCH_FAILED,
                                             mPendingVideoStreamIndex,
                                             "target rendition did not reach playback timeline");
        mAVDeviceManager->discardPendingVideoDecoder();
        while (!mPendingVideoPacketQue.empty()) {
            mPendingVideoPacketQue.front()->setDiscard(true);
            mPendingVideoPacketQue.pop_front();
        }
        while (!mPendingVideoFrameQue.empty()) {
            mPendingVideoFrameQue.front()->setDiscard(true);
            mPendingVideoFrameQue.pop();
        }
        if (mPendingVideoStreamIndex >= 0 && mDemuxerService != nullptr) {
            mDemuxerService->CloseStream(mPendingVideoStreamIndex);
            AF_LOGI("closed pending video stream after quality switch timeout: stream=%d\n",
                    mPendingVideoStreamIndex);
        }
        mPendingVideoStreamIndex = -1;
        mPendingVideoDecoderSwitch = false;
        mPendingVideoPtsOffset = INT64_MIN;
        mPendingVideoSwitchTimePosition = INT64_MIN;
        mPendingVideoSwitchStartMs = 0;
        mPendingVideoInitPacketSent = false;
        mPendingVideoPrerollDone = false;
                        mPendingVideoPrerollRefUs = INT64_MIN;
        return false;
    }

    /*
     * 每个 DASH/HLS representation 都有自己的 init 分片。pending decoder 在
     * 读完 init 后，可能先输出一组来自该 representation 起点的帧：它们的
     * PTS 看起来是有效值，但 timePosition 仍然落在 0 或很早的位置。此时
     * 不能仅凭 PTS 与主时钟的差值提交，否则会把“新 decoder 已初始化”误当成
     * “新 decoder 已追到当前播放点”，导致切换后音频继续前进而视频从开头播。
     *
     * timePosition 是 demuxer 给每个 packet 写入的媒体时间位置，decoder 会
     * 通过 packet metadata 原样带到输出帧。只有输出帧已经到达当前主时钟附近，
     * 才允许进行 PTS 对齐和 decoder promote；较早的帧全部留在 pending decoder
     * 内部继续排空，直到对应的媒体位置追上来。
     *
     * 【2026-09-21 修：这里是 2 秒窗口 → 一个帧间隔的容差】
     *
     * 原来落后主时钟 2 秒以内就提交。后果是把「已经过去的时间」推进 active
     * 帧队列：队里原来的旧帧在时钟上（33.60、33.62），新帧却在时钟之前
     * （31.9）—— 播放顺序反了，画面往回倒、音画错位。见 PENDING_ALIGN_TOLERANCE_US
     * 的说明（ExoPlayer 对 position 之前的输出帧是 FRAME_RELEASE_SKIP，
     * hls.js/dash.js 是把新分片追加在当前播放位置之后）。
     *
     * 代价：目标路要一直解到播放位置才提交，切换完成时刻会晚一些；但**旧路在
     * 这期间照常播放**（提交前不再绕过节拍），所以用户看不到冻结，只是换档的
     * 时刻晚一点。目标路真的追不上时由上面的 QUALITY_SWITCH_TOTAL_TIMEOUT_MS 兜底。
     */
    /*
     * 【2026-09-21 修：一次调用把"早于播放位置"的 pending 帧**全部**丢干净】
     *
     * 原来这里一次只丢一张就 return。目标路从分片边界开始预热时，落后主时钟的帧
     * 可能是几百张（DASH 10 秒分片 × 60fps = 600 张），而这条路径的调用频率就是
     * 主循环 + 每次解码迭代 —— 实测追赶速率被卡在 **108 帧/秒**（= 每轮 4 个
     * pending 包 × 27 轮/秒），于是一个 DASH 切换要 7~10 秒（HLS 分片 4 秒，
     * 所以只要 0.2~1.3 秒）。用户看到的"dash 切换时间明显比 hls 长很多"就是它。
     *
     * 丢帧本身只是 setDiscard + 析构，不花时间；真正的成本在解码。所以要一次性
     * 把已经解出来、但已经落在播放位置之前的帧全部丢掉，并顺手把解码器输出队列里
     * 的下一批拉进来（DrainPendingVideoFrames），让"解码 → 丢"这条路一次走完，
     * 而不是一个主循环周期才走一张。
     */
    int64_t droppedStaleFrames = 0;

    if (master > 0) {
        while (!mPendingVideoFrameQue.empty()) {
            const int64_t headPos = mPendingVideoFrameQue.front()->getInfo().timePosition;

            if (headPos < 0 || headPos + PENDING_ALIGN_TOLERANCE_US >= master) {
                /* 这一张（或者没有 media time 的 init 帧）交给下面的正常判断。 */
                break;
            }

            mPendingVideoFrameQue.front()->setDiscard(true);
            mPendingVideoFrameQue.pop();
            ++droppedStaleFrames;

            /* 一次调用最多丢这么多：避免目标解码器一直比主时钟慢时，
             * 这个循环把主循环占死（剩下的下一轮继续）。 */
            if (droppedStaleFrames >= PENDING_STALE_DRAIN_MAX) {
                break;
            }

            /* 队列空了就立刻去解码器里再拉一批，别等下一轮主循环。 */
            if (mPendingVideoFrameQue.empty()) {
                DrainPendingVideoFrames();
            }
        }

        if (droppedStaleFrames > 0 && mPendingVideoFrameQue.empty()) {
            /* 追平了但还没有可提交的帧：下轮继续解码。 */
            return false;
        }
    }

    if (mPendingVideoFrameQue.empty()) {
        return false;
    }

    const int64_t frameTimePositionNow = mPendingVideoFrameQue.front()->getInfo().timePosition;

    if (master > 0 && frameTimePositionNow >= 0 &&
        frameTimePositionNow + PENDING_ALIGN_TOLERANCE_US < master) {
        if (floodLogAllowed(FLOOD_PENDING_HOLD, 2, "hold pending video before playback position")) {
            /* 同 FLOOD_PENDING_DROP：带数值输出，否则卡住时只有一句计数。 */
            AF_LOGI("hold pending video before playback position: framePts=%lld timePosition=%lld "
                    "master=%lld lag=%lld ms stream=%d droppedStale=%lld\n",
                    (long long) mPendingVideoFrameQue.front()->getInfo().pts,
                    (long long) frameTimePositionNow, (long long) master,
                    (long long) ((master - frameTimePositionNow) / 1000),
                    mPendingVideoStreamIndex, (long long) droppedStaleFrames);
        }

        mPendingVideoFrameQue.front()->setDiscard(true);
        mPendingVideoFrameQue.pop();
        return false;
    }

    /*
     * 目标流已经读到主时钟之后：**等**主时钟追上来，而不是丢弃。
     *
     * 【2026-09-21 修：未来窗口从 2 秒收紧到 PENDING_FUTURE_TOLERANCE_US】
     *
     * 放宽到 2 秒/500ms 会把一张"未来 481ms"的帧直接提交（实测
     * `committed … pts=200200000 master=199718939`），画面当场往前跳半秒 ——
     * 就是用户说的"dash 切换之后音画不太同步"。等待是不花代价的：旧路在等的时候
     * 照常播放，主时钟走到那一点再提交，误差就只有抖动级别。
     */
    if (master > 0 && frameTimePositionNow >= 0 &&
        frameTimePositionNow > master + PENDING_FUTURE_TOLERANCE_US) {
        return false;
    }

    const int64_t rawPts = mPendingVideoFrameQue.front()->getInfo().pts;
    /*
     * 【2026-09-21 修：偏移只对"没有 media time"的帧补加】
     *
     * FillPendingVideoFrame() 在 frameTimePosition >= 0 时**已经逐帧把 pts 归一化到
     * 全局时间轴**了；mPendingVideoPtsOffset 现在还会被记录成"原始 PTS → timePosition"
     * 的固定偏移（提交后 active 路径要用它），所以这里**不能再加一次**，否则归一化过的
     * 帧会被平移两次。只有 frameTimePosition < 0（没有 media time，靠主时钟反推偏移）
     * 那条老路还需要在这里补加。
     */
    const bool framePtsAlreadyGlobal = frameTimePosition >= 0;
    const int64_t pts = (!framePtsAlreadyGlobal && mPendingVideoPtsOffset != INT64_MIN && rawPts != INT64_MIN)
                            ? rawPts + mPendingVideoPtsOffset : rawPts;
    // Representation 切换时，DASH/HLS 常会先送一包初始化帧 pts=0。
    // 这不是可显示的媒体时间，不能拿它提交 decoder，否则会把视频时钟
    // 回拨到 0，随后触发 PTS_REVERTING 和大段画面“严重迟到”。
    if (pts <= 0 && frameTimePosition < 0 && mPlayedVideoPts > 1000 * 1000) {
        mPendingVideoFrameQue.front()->setDiscard(true);
        mPendingVideoFrameQue.pop();
        return false;
    }
    const int64_t ptsDiscontinueDelta = mPtsDiscontinueDelta == INT64_MIN ? PTS_DISCONTINUE_DELTA : mPtsDiscontinueDelta;
    if (mPlayedVideoPts != INT64_MIN && pts + ptsDiscontinueDelta < mPlayedVideoPts) {
        mPendingVideoFrameQue.front()->setDiscard(true);
        mPendingVideoFrameQue.pop();
        return false;
    }
    if (master > 0 && pts > master + PENDING_FUTURE_TOLERANCE_US) return false;

    /*
     * 这里不能清空 active 视频帧队列。队列中的帧是旧 decoder 已经完成解码、
     * 还没有送入 Qt 渲染器的最后安全缓冲；如果在 promote 时直接丢掉它们，
     * 新 decoder 虽然已经创建成功，但 pending 路还没有形成连续输出，画面就
     * 会出现数秒黑住/停在最后一帧。主流播放器的交接顺序是：保留旧帧 -> 把
     * 新路已对齐的帧追加到队尾 -> 渲染线程自然消费旧帧并接上新帧 -> 新帧
     * 真正上屏后再释放 retired decoder 和旧 stream。
     */
    const size_t oldVideoFramesPending = mVideoFrameQue.size();
    mAVDeviceManager->promotePendingVideoDecoder();
    /*
     * 【2026-09-21 修：只把 pending 队列里**最新**的一帧带进 active 队列】
     *
     * pending 队列里最多压 4 帧（DrainPendingVideoFrames 的上限），每帧之间只差
     * 一个帧间隔。把这几帧全推进 active 队列，播放锚点就变成**最旧**的那一帧 ——
     * 新画面一上来就落后好几帧，而且它比队里的旧帧还旧，播放顺序会反转
     * （旧帧在时钟上、新帧在时钟之前）。只带最新的一帧，锚点贴着提交时的对齐点。
     */
    std::unique_ptr<IAFFrame> newestPendingFrame{nullptr};

    while (!mPendingVideoFrameQue.empty()) {
        auto frame = std::move(mPendingVideoFrameQue.front());
        mPendingVideoFrameQue.pop();

        if (frame == nullptr) {
            continue;
        }

        if (newestPendingFrame != nullptr) {
            /* 更旧的那一帧直接作废：它只在 pending 队列里待了一个帧间隔，
             * 丢掉不会有任何可见影响，但能让提交锚点贴近时钟。 */
            newestPendingFrame->setDiscard(true);
        }

        newestPendingFrame = std::move(frame);
    }

    if (newestPendingFrame != nullptr) {
        /* 同上面的 pts：有 media time 的帧已经在 FillPendingVideoFrame 里归一化过，
         * 这里只补加"没有 media time"那条老路的偏移。 */
        if (mPendingVideoPtsOffset != INT64_MIN &&
            newestPendingFrame->getInfo().timePosition < 0 &&
            newestPendingFrame->getInfo().pts != INT64_MIN) {
            newestPendingFrame->getInfo().pts += mPendingVideoPtsOffset;
        }

        mVideoFrameQue.push(std::move(newestPendingFrame));
    }
    mCurrentVideoIndex = mPendingVideoStreamIndex;
    mQualitySwitchCommittedStreamIndex = mCurrentVideoIndex;
    mQualitySwitchCommitPending = true;
    /*
     * 提交只是换了槽位，还没有一帧真的上屏。给收尾设一个死线：到点还没等到
     * 上屏就走 finishQualitySwitch(false, ...)，否则状态机会被永久钉在这里
     * （旧流关不掉、retired 解码器不还、上层收不到终态）。见
     * QUALITY_SWITCH_DEADLINE_MS 的说明。
     */
    mQualitySwitchCommitMs = af_getsteady_ms();
    mQualitySwitchDeadlineMs = mQualitySwitchCommitMs + QUALITY_SWITCH_DEADLINE_MS;

    /*
     * 退役 Representation 在公共视频队列里的残留包**一次性**清掉。
     *
     * 不清的话解码循环只能“发现一个是旧的、丢一个”，速率就是主循环频率
     * （实测约 82 个/秒，日志里的 `[flood] drop stale video packet after quality
     * switch: 82 more lines suppressed`），而旧路可能已经堆了几百上千个包：
     * 那几秒里排在他们后面的**新** Representation 的包一个都轮不到解码，
     * 画面就是 0 FPS —— HLS 那次切换明明已经 READY 成功，还是冻了 6 秒。
     * 旧路的解码帧在 mVideoFrameQue 里（那是要保留着连续播放的），这里清的只是包。
     */
    if (mRetiredVideoStreamIndex >= 0 && mRetiredVideoStreamIndex != mCurrentVideoIndex) {
        const int droppedStale = mBufferController->DropPacketsByStream(BUFFER_TYPE_VIDEO, mRetiredVideoStreamIndex);

        if (droppedStale > 0) {
            AF_LOGI("dropped %d stale video packets of the retired stream=%d at commit\n",
                    droppedStale, mRetiredVideoStreamIndex);
        }
    }

    /*
     * 提交的锚点可能比主时钟旧 1~2 秒（4K 目标路预热慢，提交门限为此留了窗口）。
     * 开一个追赶窗口：新路接管画面之后把迟到的帧丢掉，几百毫秒内追上时钟 —— 既不
     * 像“每帧都渲染”那样永久落后 2 秒，也不像“跳下一个关键帧”那样把画面扔到
     * 时钟前面冻住。对应 ExoPlayer 的 setJoiningDeadlineMs()。
     */
    beginRendererJoining("quality switch committed");
    /* 记录 promote 前 active 队列中的旧帧。它们必须继续送出，不能在切换
     * 瞬间被新路覆盖；计数在 RenderVideo() 消费旧帧时递减。 */
    mQualitySwitchOldFramesPending = oldVideoFramesPending;
    mActiveVideoPtsOffset = mPendingVideoPtsOffset;
    updateVideoMeta();
    /* 注意：这里仅完成 decoder 槽位交换，不能马上通知 READY。
     * 新帧还没有真正送入渲染器，Qt/D3D11 纹理也可能仍是旧宽高；
     * RenderVideo() 在新帧成功送出后再统一发尺寸、清晰度和 READY 回调。 */
    mPendingVideoStreamIndex = -1;
    /*
     * 【2026-09-21 修：提交时**不能**清空 mPendingVideoPacketQue】
     *
     * 这个队列里装的是目标 Representation "已经读进来、还没解码"的那一段 ——
     * 它有一个上限（doReadPacket 的 120 个包），60fps 的内容就是**约 2 秒**。
     * 原来在这里把它们全部 discard，等于在新旧两条时间轴之间挖一个 2 秒的洞：
     *   实测（两个片源各一次，数字完全对得上）
     *     DASH：`committed … pts=6172833 master=6265649`（只差 93ms，对齐是准的）
     *           下一秒 `read-ahead gate: video packet queue front 8458450 is 2177 ms
     *           ahead of master 6280637 (activeQ=1)` —— activeQ 只剩 1，因为
     *           6.17s~8.45s 那 2.28 秒全被丢掉了；
     *     HLS ：`committed … pts=77577500 master=77665039` 之后
     *           `video packet queue front 79863117 is 2170 ms ahead of master`。
     * 画面只能停在洞的这一侧，等主时钟走到洞的另一侧（2.2 秒），期间
     * 视频一路“迟到”、画面冻住（HLS 实测 5.8 秒 0 FPS）。
     *
     * 正确做法：把这段数据留给**已经提升为 active 的新 decoder** 继续解。
     * 它们的 streamIndex 就是新的 mCurrentVideoIndex，在 doDeCode 里会自然走
     * active 分支（旧流的残留包另有 streamIndex 检查挡掉）。
     */
    mPendingVideoPtsOffset = INT64_MIN;
    mPendingVideoSwitchTimePosition = INT64_MIN;
    mPendingVideoSwitchStartMs = 0;
    mPendingVideoInitPacketSent = false;
    mPendingVideoPrerollDone = false;
                        mPendingVideoPrerollRefUs = INT64_MIN;
    mPendingVideoDecoderSwitch = false;
    mVideoPtsRevert = false;
    /* 不要把已播放 PTS 清零。pending 帧已经被对齐到当前主时钟，保留旧
     * mPlayedVideoPts 才能让 RenderVideo() 连续计算，不会因切换瞬间重新从
     * 时间 0 起步而把整段画面判成“严重迟到”。 */
    mVideoChangedFirstPts = INT64_MIN;
    /*
     * 【切换落点上的"花屏/马赛克"防线】
     *
     * 新解码器刚接手的头几帧有个已知风险：它是从新 Representation 的关键帧重新建的，
     * 而那一刻的参考帧链、codec 参数集（SPS/PPS）、以及 D3D11 视频处理器的输入格式
     * 都只在"换过一遍"之后才齐 —— 实测日志里紧接着提交会出现一簇
     * `avcodecDecoder: Error while decoding frame -1094995529`（AVERROR_INVALIDDATA），
     * 用户在画面上看到的就是**一瞬间的马赛克/花屏**。
     *
     * 这里不丢帧、也不清队列，只是把新流的**前几帧**挡在渲染器外面（画面保持上一帧，
     * 约 3 帧 ≈ 50ms）：解码照常进行，参考帧链在解码器内部继续建立，等这几帧过去，
     * 交给渲染器的就是干净的帧了。代价是切换落点晚 50ms（对"无感"毫无影响），
     * 换来的是画面不会闪一下花屏。
     */
    mQualitySwitchHoldFrames = QUALITY_SWITCH_HOLD_FRAMES;
    AF_LOGI("committed seamless video decoder switch at pts=%lld master=%lld audio=%lld offset=%lld "
            "(holding %d new frames to avoid a corrupt first frame)\n",
            (long long) pts, (long long) master, (long long) mPlayedAudioPts,
            (long long) mActiveVideoPtsOffset, mQualitySwitchHoldFrames);
    return true;
}

bool SuperMediaPlayer::push(unique_ptr<IAFFrame> &frame)
{
#ifdef ENABLE_VIDEO_FILTER
    std::lock_guard<std::mutex> filterLock(mFilterManagerMutex);
    if (mFilterManager != nullptr) {
        bool success = mFilterManager->push(frame);
        return success;
    }
#endif
    return false;
}

bool SuperMediaPlayer::pull(int format, unique_ptr<IAFFrame> &frame)
{
#ifdef ENABLE_VIDEO_FILTER
    std::lock_guard<std::mutex> filterLock(mFilterManagerMutex);
    if (mFilterManager != nullptr) {
        bool success = mFilterManager->pull(format, frame);
        return success;
    }
#endif
    return false;
}

bool SuperMediaPlayer::render()
{
    //send to audio render
    bool audioRendered = false;
    bool videoRendered = false;

    if ((mCurrentAudioIndex >= 0) && !mSeekNeedCatch) {
        int ret;
        do {
            ret = RenderAudio();
            if (RENDER_NONE != ret) {
                audioRendered = true;
            }
        } while (ret == RENDER_FULL);
    }

    if (HAVE_VIDEO) {
        videoRendered = RenderVideo(!mFirstRendered);
    }

    if (HAVE_VIDEO && HAVE_AUDIO) {
        // TODO: do it in reset()
        if (!PTS_REVERTING) {
            if (videoRendered && audioRendered) {
                mVideoPtsRevert = mAudioPtsRevert = false;
            }
        } else {
            AF_LOGW("PTS_REVERTING audio pts is %lld ,video pts is %lld\n", mPlayedAudioPts, mPlayedVideoPts);
        }
    } else {
        mVideoPtsRevert = mAudioPtsRevert = false;
    }

    if ((HAVE_SUBTITLE || mSubPlayer) && !mSeekFlag) {
        RenderSubtitle(mCurVideoPts);
    }

    return audioRendered || videoRendered;
}

RENDER_RESULT SuperMediaPlayer::RenderAudio()
{
    RENDER_RESULT ret = RENDER_NONE;
    int64_t pts = INT64_MIN;
    int64_t duration = INT64_MIN;
    int64_t position = INT64_MIN;
    int render_ret;
//#define DUMP_PCM
#ifdef DUMP_PCM
    static int fd = open("out.pcm", O_CREAT | O_RDWR, 0666);
#endif

    if (mAudioFrameQue.empty()) {
        if (audioDecoderEOS && mAVDeviceManager->getAudioRenderQueDuration() == 0) {
            mMasterClock.setReferenceClock(nullptr, nullptr);
        }
        return ret;
    }

    pts = mAudioFrameQue.front()->getInfo().pts;
    position = mAudioFrameQue.front()->getInfo().timePosition;

    if (pts == INT64_MIN) {
        mAudioFrameQue.pop_front();
        return ret;
    }

    auto *avafFrame = dynamic_cast<AVAFFrame *>(mAudioFrameQue.front().get());

    if (avafFrame) {
        duration = getPCMFrameDuration(avafFrame->ToAVFrame());
    }

    if (mFrameCb && (!mSecretPlayBack || mDrmKeyValid)) {
        mFrameCb(mFrameCbUserData, avafFrame);
    }

    render_ret = mAVDeviceManager->renderAudioFrame(mAudioFrameQue.front(), 0);

    if (render_ret == IAudioRender::FORMAT_NOT_SUPPORT) {
        if (mAVDeviceManager->getAudioRenderQueDuration() == 0) {
            std::lock_guard<std::mutex> uMutex(mCreateMutex);
            mAudioTime.startTime = mAudioFrameQue.front()->getInfo().pts;
            mAudioTime.deltaTimeTmp = 0;
            mAudioTime.deltaTime = 0;
            mLastAudioFrameDuration = -1;
            setUpAudioRender(mAudioFrameQue.front()->getInfo().audio);
            if (mBRendingStart) {
                mAVDeviceManager->pauseAudioRender(false);
            }
            mAVDeviceManager->renderAudioFrame(mAudioFrameQue.front(), 0);
        }
    } else if (render_ret == IAudioRender::OPEN_AUDIO_DEVICE_FAILED) {
        AF_LOGE("render audio failed due to can not open device, close audio stream");
        mOpenAudioDeviceFailed = true;
        closeAudio();
        if (HAVE_VIDEO) {
            mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_OPEN_AUDIO_DEVICE_FAILED, "open audio device failed");
        } else {
            ChangePlayerStatus(PLAYER_ERROR);
            mPNotifier->NotifyError(MEDIA_PLAYER_ERROR_RENDER_AUDIO_OPEN_DEVICE_FAILED, "open audio device failed");
            return ret;
        }
    }

    if (!mAudioFrameQue.empty() && mAudioFrameQue.front() == nullptr) {
        mAudioFrameQue.pop_front();
        ret = RENDER_FULL;
    } else {
        return ret;
    }

#ifdef DUMP_PCM
    write(fd, buffer + mWriteAudioLen, len);
#endif

    if (!HAVE_VIDEO) {
        checkFirstRender();
    }

    if (mPlayedAudioPts == INT64_MIN) {
        mAudioTime.startTime = pts;
        mAudioTime.deltaTime = 0;
        mAudioTime.deltaTimeTmp = 0;
        mMasterClock.setReferenceClock(getAudioPlayTimeStampCB, this);
        if (mAudioClockReanchorPending) {
            /* seek 后音频解码器重新输出第一帧时，把主时钟重新锚定到这张帧，
             * 防止 DashStream 关键帧回退/音频 segment 起点差异继续扩大 A/V 偏移。 */
            mMasterClock.setTime(pts);
            mAudioClockReanchorPending = false;
            AF_LOGI("audio clock re-anchored after seek at pts=%lld\n", (long long) pts);
        }
    } else {
        if (mLastAudioFrameDuration > 0) {
            if (!mAudioPtsRevert) {
                mAudioPtsRevert = pts < mPlayedAudioPts - mPtsDiscontinueDelta;
                if (mAudioPtsRevert) {
                    AF_LOGI("PTS_REVERTING audio start\n");
                }
            }
            int64_t offset = pts - (mPlayedAudioPts + mLastAudioFrameDuration);

            /*
             * the mLastAudioFrameDuration and the pts are all not the accurate value,
             * the mLastAudioFrameDuration accurate for 1/1000000 s,
             * the pts maybe accurate for 1/1000 s (eg. flv file), so can't increase the deltaTimeTmp when
             * offset little than 1ms.
             */
            if (llabs(offset)) {
                //    AF_LOGW("offset is %lld,pts is %lld", offset, pts);
                mAudioTime.deltaTimeTmp += offset;
                mPlayedAudioPts += offset;
            }

            if (llabs(mAudioTime.deltaTimeTmp) > 100000) {
                AF_LOGW("correct audio and master clock offset is %lld, frameDuration :%lld", mAudioTime.deltaTimeTmp,
                        mLastAudioFrameDuration);
                mAudioTime.deltaTime += mAudioTime.deltaTimeTmp;
                mAudioTime.deltaTimeTmp = 0;
            }
        }
    }

    if (mPlayedAudioPts == INT64_MIN && isSeeking()) {
        // update after send first frame in seeking, because audio render callback is async.
        // sometimes notify position before audio rendered callback , will cause position not right.
        mCurrentPos = position;
    }

    if (mPlayedAudioPts != INT64_MIN) {
        mPlayedAudioPts += duration;
    } else
        mPlayedAudioPts = pts;
    mLastAudioFrameDuration = duration;

    if (mAudioChangedFirstPts == pts && !mMixMode) {
        StreamInfo *info = GetCurrentStreamInfo(ST_TYPE_AUDIO);
        mPNotifier->NotifyStreamChanged(info, ST_TYPE_AUDIO);
        AF_LOGD("audio changed\n");
        mAudioChangedFirstPts = INT64_MIN;
    }

    return ret;
}

bool SuperMediaPlayer::RenderVideo(bool force_render)
{

    if (!mAVDeviceManager->isVideoRenderValid()) {
        return false;
    }
    //send to video render
    if (mVideoFrameQue.empty()) {
        return false;
    }

    unique_ptr<IAFFrame> &videoFrame = mVideoFrameQue.front();

    if (videoFrame == nullptr) {
        return false;
    }

    int64_t videoPts = videoFrame->getInfo().pts;

    // work around for huaweiP20 pro hardware decode get pts = INT64_MIN when change resolution.
    if (videoPts == INT64_MIN && videoPts < mPlayedVideoPts) {
        videoPts = mPlayedVideoPts + 1;
    }

    int frameWidth;
    int frameHeight = videoFrame->getInfo().video.height;
    if (videoFrame->getInfo().video.dar != 0) {
        frameWidth = videoFrame->getInfo().video.dar * videoFrame->getInfo().video.height;
    } else {
        frameWidth = videoFrame->getInfo().video.width;
    }
    videoFrame->getInfo().video.rotate = mVideoRotation;

    if (!mVideoPtsRevert) {
        mVideoPtsRevert = mPlayedVideoPts != INT64_MIN && videoPts < mPlayedVideoPts - mPtsDiscontinueDelta;

        if (mVideoPtsRevert) {
            AF_LOGI("PTS_REVERTING video start\n");
        }
    }

    // audio pts first revert to small, force render the old video frame
    if (PTS_REVERTING && mAudioPtsRevert && videoPts - mPtsDiscontinueDelta > mPlayedAudioPts) {
        AF_LOGI("PTS_REVERTING force render the old video frame");
        force_render = true;
        // video pts first revert to small,the new video data wait audio pts to revert
    } else if (PTS_REVERTING && mVideoPtsRevert && videoPts + mPtsDiscontinueDelta < mPlayedAudioPts) {
        AF_LOGI("PTS_REVERTING wait audio to revert");
        return false;
    }

    int64_t masterPlayedTime = mMasterClock.GetTime();
    int64_t videoLateUs = masterPlayedTime - videoPts;// > 0 表示视频落后于主时钟
    videoLateUs -= mVideoDelayTime;

    /*
     * 【切换提交后的头几帧：挡在渲染器外面，避免一瞬间马赛克/花屏】
     * 见提交处（committed seamless video decoder switch）那段说明：解码照常，
     * 只是这几帧不交给渲染器 —— 画面保持上一帧，等它们过去画面就是干净的。
     * 注意这里**不影响时钟**：帧照常从队列里出，主时钟按音频走，
     * 后面那一帧会被正常渲染（最多晚 3 帧 ≈ 50ms）。
     */
    if (mQualitySwitchHoldFrames > 0) {
        --mQualitySwitchHoldFrames;
        videoFrame->setDiscard(true);
        mUtil->videoRendered(false);
        mMPAUtil->videoRendered(false);
        mPlayedVideoPts = videoPts;
        mVideoFrameQue.pop();

        if (mQualitySwitchHoldFrames == 0) {
            AF_LOGI("quality switch: hold window over, resuming rendering from pts=%lld\n",
                    (long long) videoPts);
        }

        /* 返回 false = 这一轮没有帧上屏（画面保持上一帧，不是"卡"） */
        return false;
    }

    /*
         *  if stc is free, video rectify it
         */
    if ((llabs(videoLateUs) > 1000 * 1000) || (llabs(videoLateUs) > mSet->maxBufferDuration)) {
        /*
         * don't have master, or master not in valid status
         *
         * 【seek 期间禁止这条兜底 —— 这是"seek 之后画面冻住 1~3 秒"的直接原因】
         *
         * seek 一开始 ProcessSeekToMsg() 就把主时钟钉在 seek 目标上，而且刻意让音频
         * 参考时钟在 seek 结束前一律返回 INT64_MIN（见 getAudioPlayTimeStamp 的说明），
         * 于是这里 isMasterValid() 为假 —— 兜底就把主时钟改成**视频帧的 pts**。
         * 可 seek 期间解码器是从"目标点之前的关键帧"开始猛解的（没有任何节流），
         * 它早就跑到目标点后面 2~3 秒了：时钟于是被推到那里（日志实测
         * master=37.72s vs 目标 34.34s），随后追赶窗口按这个假时钟把
         * 34.3s~37.7s 的帧（**含 seek 目标那一帧**）当"迟到帧"全丢光；
         * 等第一张音频帧把时钟拉回 34.35s，画面只剩 37.7s 之后的帧 ——
         * 只能干等时钟追上 3 秒（声音正常、进度条先跳到 37.7s 再弹回 34.3s）。
         *
         * seek 期间时钟由 ProcessSeekToMsg 权威地钉住，不需要也不允许这里再改写：
         * 目标帧等时钟到点再上屏就是正确行为（那时 videoLateUs 会回到正常范围）。
         */
        if ((!mMasterClock.haveMaster() || !mMasterClock.isMasterValid()) && !mSeekFlag) {
            mMasterClock.setTime(videoPts);
            masterPlayedTime = videoPts;
        }
    }

    /* video early more than 10ms, don't (render||drop) it, deal it next time
         * if the pts is not continue drop it

         ---------------------------------------early----------------------------|------------------------late--------------

         ----------------------------------|-------------------------------|-----|-------------|--------------------------------------->
         drop if pts discontinue    disDelta s       render next time     10ms   0            500ms     drop
            && audio reverting
                                                                          |--------render-----|


         */
    bool render = force_render;

    /*
     * 追赶窗口到点就关掉（见 beginRendererJoining）：避免解码器本身就跟不上实时
     * （4K 软解之类）时无限丢帧、把画面丢成空白。
     *
     * 窗口用完时如果画面仍然明显落后，说明“丢掉迟到帧”这条路没能在窗口内把画面
     * 拉回时钟 —— 这是“seek/切档之后画面慢速追时钟”的唯一直接信号（以前这里一行
     * 日志都不打，只能从 KPI 猜）。窗口每个 beginRendererJoining() 只会到点一次
     * （到点即清标志），所以这条日志天然低频，不会刷屏。
     */
    if (dropLateVideoFrames && mDropLateVideoFramesUntilMs > 0 &&
        af_getsteady_ms() > mDropLateVideoFramesUntilMs) {
        dropLateVideoFrames = false;
        mDropLateVideoFramesUntilMs = 0;

        if (videoLateUs > 200 * 1000) {
            AF_LOGW("renderer joining window expired while still late: %lld ms behind the master clock "
                    "(master=%lld videoPts=%lld seekFlag=%d frameQ=%d activeQ=%d) — late frames will now "
                    "be rendered instead of dropped\n",
                    (long long) (videoLateUs / 1000), (long long) masterPlayedTime, (long long) videoPts,
                    (int) mSeekFlag, (int) mVideoFrameQue.size(),
                    (int) mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO));
        }
    }

    /*
     * 【2026-09-21 修：seek 前后都不要把“还没到目标点”的帧推给渲染器】
     *
     * seek 期间 doRender() 用 force_render=true 调进来（为了拿到目标帧、结束 seek），
     * 而 force_render 会绕过下面**所有**时钟节拍判断，于是从“目标点之前的关键帧”
     * 到目标点之间的每一帧都被推给了渲染器。渲染器的输入队列（AFActiveVideoRender::
     * mInputQueue）一满就 dropFrame() 并把自己的渲染时钟重锚到最新帧 ——
     * 现象是 seek 之后画面快进一段、进度条跳、而且**视频跑到主时钟前面**
     * （实测 seek 到 33.62s 之后包队列队首已经在 37.05s，超前 3.4 秒；
     * 随后就是“每秒往前跳一个分片”、gap 涨到 16.7 秒的永久冻死）。
     *
     * 原来这里只在 `mSeekFlag` 为真时生效。不够：seek 一结束 mSeekFlag 就清了，
     * 而管道离目标点还差好几秒（HLS 实测 seek 到 26.708s、tracker 落在 20.854s，
     * 差 5.9 秒），这段“目标点之前的帧”照样上屏 —— 画面往回倒，位置回调跟着
     * 往回跳（用户反馈的“进度跳切过去又跳回来再跳回去”）。
     *
     * 所以改用 mSeekPositionFloorUs（seek 一发起就记下的目标点，直到管道真的
     * 走到它为止）。ExoPlayer 的做法完全一样：`positionUs < 目标点` 的输出缓冲
     * 一律 `FRAME_RELEASE_SKIP`（解码但不显示）。
     *
     * 兜底：目标点如果离谱到差 15 秒以上，就撤掉地板照常渲染 —— 宁可倒一下，
     * 也不能因为一个坏目标点把画面永久冻住。
     */
    if (render && mSeekPositionFloorUs != INT64_MIN) {
        const int64_t frameTimePos = videoFrame->getInfo().timePosition;

        if (frameTimePos >= 0) {
            if (frameTimePos + SEEK_TARGET_DROP_AHEAD_US >= mSeekPositionFloorUs) {
                /* 到目标点了：撤掉地板，位置交回正常管道。 */
                mSeekPositionFloorUs = INT64_MIN;
            } else if (frameTimePos + SEEK_FLOOR_GIVEUP_US < mSeekPositionFloorUs) {
                mSeekPositionFloorUs = INT64_MIN;
            } else {
                render = false;
            }
        }
    }

    if (!force_render) {
        const bool qualitySwitchWarming = mPendingVideoStreamIndex >= 0 ||
                                           mPendingVideoDecoderSwitch ||
                                           mQualitySwitchCommitPending;
        /*
         * 【2026-09-22 修：早帧一律等时钟 —— 预热阶段**不再**例外】
         *
         * 这里原来带 `!qualitySwitchWarming`：切换预热（提交之前）期间，"比主时钟早
         * 10ms 以上"的帧**不等待**，继续往下走去渲染。这一条的初衷是"预热阶段旧路是
         * 唯一能显示的路，不能让画面停住"，但它管的是**迟到**帧，而这条判断管的是
         * **提前**帧 —— 提前帧本来就应该等时钟，跟切换没有关系。
         *
         * 去掉例外之后的实测差别（22:06 那份日志，正是用户说的"dash 切换卡一下"）：
         *   * 预热期间每一帧（可能早几百毫秒）都被推给 Qt 渲染器 →
         *     `AFActiveVideoRender: drop a frame pts=…` 每秒刷几十上百条、连着 3 秒
         *     （01:06:04.3~07.2 / 01:07:31.6~33.0 / 01:07:39.9~43.0 三段），
         *     渲染器把队列里多余的帧丢掉、画面跳着走 = 卡顿；
         *   * 迟到帧照样走下面的正常分支（videoLateUs < 500ms → render）渲染出去，
         *     所以旧画面不会因此停住，"预热期间必须继续出画"这个目标仍然满足。
         */
        if (videoLateUs < -10 * 1000 &&
            (!mDemuxerService->getDemuxerHandle()->isTSDiscontinue() || videoLateUs > -mPtsDiscontinueDelta || !mAudioPtsRevert)) {
            return false;
        }

        /*
         * 这里**什么也不做**：seek 之后的“定位”由 seek 机制自己负责
         * （SeekTo() + 主时钟钉在 seek 目标 + mSeekPositionFloorUs 的地板 +
         * beginRendererJoining() 的追赶窗口 + 下面 RenderVideo() 的逐帧丢帧），
         * 在追赶窗口内（最多 JOINING_DROP_LATE_WINDOW_MS）收敛到主时钟上。
         *
         * 这里原来还有一次“seek 后往前跳关键帧 + flush 视频路”的定位，它和周期性
         * 视频路巡检一起被物理删除了：播放中 flush 整条视频路正是 2026-09-23 卡死
         * 的原因（清包 + 取消在途切换 + 之后 read-ahead gate 永久堵住）。
         * 不要再在这里加任何 seek 后的 flush 式定位。
         */

        /* 双路切换尚未完成时，旧 decoder 是唯一已经可显示的路。即使它相对
         * 音频主时钟暂时落后，也必须继续把旧帧送到渲染器，不能被当成“该丢的
         * 迟到帧”丢空；
         * 否则 pending decoder 还在缓冲时画面会完全停止。新路首帧真正加入
         * mVideoFrameQue 后，再由时间戳自然接管输出。
         *
         * 【2026-09-21 修：只有**已经提交**才无条件出画】
         *
         * 原来这里判的是 qualitySwitchWarming，它把提交**之前**的预热状态
         * （mPendingVideoStreamIndex / mPendingVideoDecoderSwitch）也算进来，
         * 于是整个预热阶段每一帧都绕过节拍直接推给渲染器。后果实测：
         * AFActiveVideoRender 的输入队列被顶爆 → `drop a frame pts=…` 连刷
         * 77 条 + 渲染时钟被重锚到最新帧 → 画面快进、跳帧、进度条跳、帧率掉到 1。
         *
         * 预热阶段旧路是完全正常的（decoder 没换、帧队列没换），必须继续按节拍走；
         * 真正需要「无条件出画」的只有提交之后那一小段 —— 那时帧队列里躺着旧路的
         * 帧，它们相对新时钟可能落后，不放行就没有画面。 */
        if (mQualitySwitchCommitPending) {
            render = true;
        } else if (dropLateVideoFrames) {
            /*
             * 追赶窗口：迟到的帧按「跳过」处理，直接跳到主时钟上。
             *
             * 【2026-09-21 修两处，都对着 ExoPlayer 的 VideoFrameReleaseControl】
             *
             * 1) 原来 on-time 的那条分支只把 dropLateVideoFrames 清掉，`render`
             *    仍然是 false —— 于是**第一帧准时的帧也被丢掉**，要等下一帧才上屏。
             *    准时帧必须渲染，标志才有意义。
             *
             * 2) 加「强制渲染」护栏，对应 ExoPlayer 的
             *    shouldForceRenderOutputBuffer()：迟到帧本该丢掉，但如果距上次
             *    真的上屏已经超过 100ms（`elapsedSinceLastRenderUs > 100_000`），
             *    就必须强制把它渲染出去 —— **不能为了追赶时钟把画面冻住**。
             *    没有这条，4K 硬解只要每帧都迟到 10ms 以上，追赶窗口就会把整整
             *    3 秒的帧全丢掉，用户看到的就是"切换后黑一下/冻一下"。
             */
            const bool renderedRecently = mLastVideoFrameRenderedMs > 0 &&
                                          (af_getsteady_ms() - mLastVideoFrameRenderedMs) < JOINING_FORCE_RENDER_MS;

            if (videoLateUs > 10 * 1000 && renderedRecently) {
                render = false;
            } else {
                dropLateVideoFrames = false;
                render = true;
            }
        } else if (videoLateUs < 500 * 1000) {// video early litter than 10ms or late litter than 500ms,render it
            render = true;
        }
        // Try to render if already haven't render more then 60ms
        else if (INT64_MIN == mPlayedVideoPts || (videoPts - mPlayedVideoPts) > 60 * 1000) {
            render = true;
        }
    }

    if (render) {
        /* 画面又在动了：清掉“追赶中”，主循环恢复正常的等待间隔
         * （读取点在 mainService()，见 mVideoCatchingUp 的说明）。 */
        mVideoCatchingUp = false;
        /*
         * 帧真的上屏了 —— 这是“视频路还活着”的**唯一**权威证据（丢帧不算，
         * 丢帧风暴里画面其实冻着）。管线真死时的一次性恢复就看这个时间戳：
         * 超过 VIDEO_RECOVER_STALL_MS（seek 在途时是 VIDEO_RECOVER_STUCK_SEEK_MS）
         * 没有刷新，并且视频包队列也不消费，才做动作（见 doRender() 的探测段）。
         */
        mLastVideoFrameRenderedMs = af_getsteady_ms();
        SendVideoFrameToRender(std::move(videoFrame));

        if (frameWidth != mVideoWidth || frameHeight != mVideoHeight) {
            mVideoWidth = frameWidth;
            mVideoHeight = frameHeight;
            mPNotifier->NotifyVideoSizeChanged(mVideoWidth, mVideoHeight);
        }

        /*
         * 只有这一帧已经走过 SendVideoFrameToRender，才认为清晰度切换真正完成。
         * 这样 Qt 端收到 READY 时，解码器、帧队列和实际输出尺寸已经一致；
         * retired decoder 也要在此之后释放，避免 D3D11 输入视图仍引用旧 surface。
         *
         * 具体的收尾动作（READY / 关旧流 / 释放 retired / 清状态）全部在
         * finishQualitySwitch() 里，这里只负责“判定条件成立”。
         */
        if (mQualitySwitchCommitPending && mQualitySwitchCommittedStreamIndex >= 0 &&
            mQualitySwitchOldFramesPending == 0) {
            AF_LOGI("quality switch rendered: stream=%d size=%dx%d\n",
                    mQualitySwitchCommittedStreamIndex, frameWidth, frameHeight);
            finishQualitySwitch(true, "quality switch rendered");
        }

        if (!HAVE_AUDIO) {
            /* 纯视频片源：第一张真正上屏的帧就是时钟锚点。
             * 但 **seek 期间不能锚** —— 理由和上面 RenderVideo 里那段一样：
             * seek 目标点的时钟已经由 ProcessSeekToMsg 钉住，而这里的第一张帧可能
             * 是解码器冲到目标点后面 2~3 秒才解出来的，锚上去等于把时钟推飞。 */
            if (mPlayedVideoPts == INT64_MIN && !mSeekFlag) {
                mMasterClock.setTime(videoPts);
                mMasterClock.setReferenceClock(mClockRef, mCRArg);
            }
        }
    } else {
        AF_LOGW("drop frame,master played time is %lld,video pts is %lld\n", masterPlayedTime, videoPts);
        videoFrame->setDiscard(true);
        mUtil->videoRendered(false);
        mMPAUtil->videoRendered(false);
        mVideoCatchingUp = true;
        /* 又丢了一帧：置“追赶中”，让主循环以最多 2ms 的间隔尽快再跑一轮
         * （mainService() 里读它；这就是这个标志现在唯一的用途）。 */

        if (mFrameCb && (!mSecretPlayBack || mDrmKeyValid)) {
            mFrameCb(mFrameCbUserData, videoFrame.get());
        }
        RenderCallback(ST_TYPE_VIDEO, false, videoFrame->getInfo());
    }

    mPlayedVideoPts = videoPts;
    mVideoFrameQue.pop();
    if (mQualitySwitchCommitPending && mQualitySwitchOldFramesPending > 0) {
        --mQualitySwitchOldFramesPending;
    }
    return render;
}

void SuperMediaPlayer::RenderSubtitle(int64_t pts)
{
    if (mSubPlayer) {
        mSubPlayer->update(getCurrentPosition());

        if (mSubPlayer->isActive()) {
            return;
        }
    }

    auto iter = mSubtitleShowedQueue.begin();

    while (iter != mSubtitleShowedQueue.end()) {
        if (*iter) {
            if (((*iter)->getInfo().pts + (*iter)->getInfo().duration) <= pts) {
                mPNotifier->NotifySubtitleEvent(subTitle_event_hide, (*iter).release(), 0, nullptr);
                iter = mSubtitleShowedQueue.erase(iter);
                continue;
            }
        }

        iter++;
    }

    int64_t subTitlePts = mBufferController->GetPacketPts(BUFFER_TYPE_SUBTITLE);

    if (subTitlePts == INT64_MIN || subTitlePts > pts) {
        return;
    }

    unique_ptr<IAFPacket> pFrame = mBufferController->getPacket(BUFFER_TYPE_SUBTITLE);

    if (pFrame == nullptr) {
        return;
    }

    bool changed = false;

    if (pFrame->getInfo().pts + pFrame->getInfo().duration < pts) {
        AF_LOGD("read subtitle pts is long before pts is %lld,subtitle pts is %lld", pts, subTitlePts);
        mBufferController->ClearPacketBeforePts(BUFFER_TYPE_SUBTITLE, subTitlePts);

        if (subTitlePts <= mSubtitleChangedFirstPts && pts > mSubtitleChangedFirstPts) {
            changed = true;
        }
    } else {
        if (subTitlePts == mSubtitleChangedFirstPts) {
            changed = true;
        }

        AF_LOGD("read subtitle pts show pts is %lld,subtitle pts is %lld", pts, subTitlePts);
        pFrame->getInfo().dts = mSubtitleShowIndex++;
        //          pFrame->pBuffer[pFrame->size] = 0;
        mPNotifier->NotifySubtitleEvent(subTitle_event_show, pFrame.get(), 0, nullptr);
        mSubtitleShowedQueue.push_back(std::move(pFrame));
    }

    if (changed) {
        AF_LOGD("subtitle changed");
        StreamInfo *info = GetCurrentStreamInfo(ST_TYPE_SUB);
        mPNotifier->NotifyStreamChanged(info, ST_TYPE_SUB);
        mSubtitleChangedFirstPts = INT64_MIN;
    }
}

void SuperMediaPlayer::OnTimer(int64_t curTime)
{
    if (mPlayedAudioPts != INT64_MIN || mPlayedVideoPts != INT64_MIN) {
        /*
             * if have seek not completed,DO NOT update the position,it will lead process bar
             * jumping
             */
        if ((mPlayStatus == PLAYER_PLAYING) && !isSeeking()) {
            //AF_LOGD("TIMEPOS OnTimer :%lld", getCurrentPosition());
            NotifyPosition(getCurrentPosition());
            NotifyUtcTime();
        }

        PostBufferPositionMsg();
    }

    mPNotifier->NotifyCurrentDownloadSpeed(mUtil->getCurrentDownloadSpeed());
}

void SuperMediaPlayer::SendVideoFrameToRender(unique_ptr<IAFFrame> frame, bool valid)
{
    if (mFrameCb && (!mSecretPlayBack || mDrmKeyValid)) {
        bool rendered = mFrameCb(mFrameCbUserData, frame.get());
        if (rendered) {
            RenderCallback(ST_TYPE_VIDEO, true, frame->getInfo());
            return;
        }
    }
    if (mAVDeviceManager->isVideoRenderValid()) {
        int ret = mAVDeviceManager->renderVideoFrame(frame);

        if (ret < 0) {
            AF_LOGE("renderFrame error \n");
            // for windows init failed, which may need change render type in future.
            mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_VIDEO_RENDER_INIT_ERROR, "init video render failed");
        }
    } else {
        assert(0);
        //render directly
        RenderCallback(ST_TYPE_VIDEO, !frame->getDiscard(), frame->getInfo());
    }
}

int SuperMediaPlayer::DecodeAudio(unique_ptr<IAFPacket> &pPacket)
{
    if (audioDecoderEOS) {
        return 0;
    }

    unique_ptr<IAFFrame> frame{};
    int ret;

    do {
        ret = mAVDeviceManager->getFrame(frame, SMPAVDeviceManager::DEVICE_TYPE_AUDIO, 0);
        if (ret == STATUS_EOS) {
            audioDecoderEOS = true;
            break;
        }

        if (frame != nullptr) {

            int64_t duration = mBufferController->GetPacketDuration(BUFFER_TYPE_AUDIO);
            if (duration < 0) {
                //If audio duration is unknow when demux , update duration after decode one frame.
                IAFFrame::AFFrameInfo frameInfo = frame->getInfo();
                int64_t packetDuration = (int64_t) frameInfo.audio.nb_samples * 1000000 / frameInfo.audio.sample_rate;
                mBufferController->SetOnePacketDuration(BUFFER_TYPE_AUDIO, packetDuration);
            }

            if (mRecorderSet->decodeFirstAudioFrameInfo.waitFirstFrame) {
                DecodeFirstFrameInfo &info = mRecorderSet->decodeFirstAudioFrameInfo;
                info.getFirstFrameTimeMs = af_getsteady_ms();
                info.waitFirstFrame = false;
            }

            if (mSecretPlayBack) {
                frame->setProtect(true);
            }
            if (frame->getInfo().pts == INT64_MIN) {
                // TODO: why mAudioFrameQue.back()->getInfo().pts is INT64_MIN
                if (!mAudioFrameQue.empty() && mAudioFrameQue.back()->getInfo().pts != INT64_MIN) {
                    duration = ((double) frame->getInfo().audio.nb_samples) / frame->getInfo().audio.sample_rate;
                    frame->getInfo().pts = mAudioFrameQue.back()->getInfo().pts + duration * 1000000;
                } else {
                    //                       assert(0);
                }
            }
            mDemuxerService->SetOption("A_FRAME_DECODED", frame->getInfo().pts);
            mAudioFrameQue.push_back(std::move(frame));
        }
    } while (ret != -EAGAIN && ret != -EINVAL);

    if (!mRecorderSet->decodeFirstAudioFrameInfo.isFirstPacketSendToDecoder) {
        DecodeFirstFrameInfo &info = mRecorderSet->decodeFirstAudioFrameInfo;
        info.isFirstPacketSendToDecoder = true;
        info.waitFirstFrame = true;
        info.firstPacketSize = pPacket->getSize();
        info.firstPacketPts = pPacket->getInfo().pts;
        info.sendFirstPacketTimeMs = af_getsteady_ms();
    }

    ret = mAVDeviceManager->sendPacket(pPacket, SMPAVDeviceManager::DEVICE_TYPE_AUDIO, 0);

    if (ret > 0) {
        bool haveError = false;

        if (ret & STATUS_HAVE_ERROR) {
            if (mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_AUDIO)->get_error_frame_no() > MAX_DECODE_ERROR_FRAME) {
                haveError = true;
            }
        }

        if (ret & STATUS_DRM_ERROR) {
            haveError = true;
        }

        if (ret & STATUS_CREATE_FAIL) {
            haveError = true;
        }

        if (ret &= STATUS_RETRY_IN) {
            ret = -EAGAIN;
        }

        if (haveError) {
            ChangePlayerStatus(PLAYER_ERROR);
            mPNotifier->NotifyError(MEDIA_PLAYER_ERROR_DECODE_AUDIO, "audio decode error");
        }
    }

    return ret;
}

void SuperMediaPlayer::ProcessOpenStreamInit(int streamIndex)
{
    AF_LOGD("ProcessOpenStreamInit ProcessOpenStreamInit start");
    std::deque<StreamInfo *> &streamInfoQueue = mMediaInfo.mStreamInfoQueue;
    int streamCount = (int) streamInfoQueue.size();
    int videoStreams = 0;

    for (int i = 0; i < streamCount; i++) {
        StreamInfo *info = streamInfoQueue[i];

        if (info->type == ST_TYPE_VIDEO) {
            videoStreams++;
        }
    }

    if (videoStreams > 1) {
        mAdaptiveVideo = true;
    } else {
        mAdaptiveVideo = false;
    }

    if (mMixMode) {
        if (mCurrentVideoIndex < 0 && mCurrentAudioIndex < 0) {
            unique_ptr<streamMeta> pMeta;
            Stream_meta *meta{};
            int nbSubStream = mDemuxerService->GetNbSubStream(mMainStreamId);

            for (int j = 0; j < nbSubStream; j++) {
                mDemuxerService->GetStreamMeta(pMeta, GEN_STREAM_ID(mMainStreamId, j), true);
                meta = (Stream_meta *) (pMeta.get());
                AF_LOGD("get a stream %d\n", meta->type);

                if (!mSet->bDisableVideo && meta->type == STREAM_TYPE_VIDEO && mCurrentVideoIndex < 0 && meta->height > 0 &&
                    meta->attached_pic == 0) {
                    AF_LOGD("get a video stream\n");
                    mCurrentVideoIndex = GEN_STREAM_ID(mMainStreamId, j);
                    mVideoInterlaced = meta->interlaced;
                    updateVideoMeta();
                } else if (!mSet->bDisableAudio && !mOpenAudioDeviceFailed && meta->type == STREAM_TYPE_AUDIO && mCurrentAudioIndex < 0 && meta->channels > 0) {
                    AF_LOGD("get a audio stream\n");
                    mCurrentAudioIndex = GEN_STREAM_ID(mMainStreamId, j);
                    mCATimeBase = meta->ptsTimeBase;
                } else if (meta->type == STREAM_TYPE_SUB && mCurrentSubtitleIndex < 0) {
                    AF_LOGD("get a subtitle stream\n");
                    mCurrentSubtitleIndex = GEN_STREAM_ID(mMainStreamId, j);
                    if (meta->extradata && meta->extradata_size > 0) {
                        mPNotifier->NotifySubtitleHeader(mCurrentSubtitleIndex, (const char *) meta->extradata);
                    }
                }
            }
        }
    }

    mPNotifier->CancelNotifyMediaInfo();
    //post before PostMsg(PreparedReq). sdk will get these infos on Prepared callback.
    mPNotifier->NotifyMediaInfo(&mMediaInfo);
}

void SuperMediaPlayer::setUpAVPath()
{
    if (!mInited) {
        return;
    }

    if (!mSet->mFastStart && mPlayStatus < PLAYER_PLAYING) {
        AF_LOGI("not fast start mode\n");
        return;
    }

    if (mCurrentAudioIndex >= 0 &&
        (!mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_AUDIO) || !mAVDeviceManager->isAudioRenderValid())) {
        AF_LOGD("SetUpAudioPath start");
        int ret = SetUpAudioPath();

        if (ret < 0) {
            AF_LOGE("%s SetUpAudioPath failed,url is %s %s", __FUNCTION__, mSet->url.c_str(), framework_err2_string(ret));
            mOpenAudioDeviceFailed = true;
            closeAudio();
            mCATimeBase = 0;
        } else {
        }
    }

    if (mCurrentVideoIndex >= 0) {
        int ret = SetUpVideoPath();

        if (ret < 0) {
            AF_LOGE("%s SetUpVideoPath failed,url is %s %s", __FUNCTION__, mSet->url.c_str(), framework_err2_string(ret));
            closeVideo();
        }
    }

    if (mCurrentVideoIndex < 0 && mCurrentAudioIndex < 0) {
        ChangePlayerStatus(PLAYER_ERROR);
        mPNotifier->NotifyError(MEDIA_PLAYER_ERROR_DEMUXER_NO_VALID_STREAM, "No valid stream");
    }
}

bool SuperMediaPlayer::NeedDrop(int64_t pts, int64_t refer)
{
    return (pts < refer) && (pts < mDuration - 200 * 1000);
}

int SuperMediaPlayer::ReadPacket()
{
    IAFPacket *pFrame = nullptr;
    std::unique_ptr<IAFPacket> pMedia_Frame{};

    if (mDemuxerService == nullptr) {
        assert(0);
    }

    int index = -1;

    if (HAVE_SUBTITLE && !mSubtitleEOS) {
        if (mBufferController->GetPacketDuration(BUFFER_TYPE_SUBTITLE) <= 0) {
            if (mSubtitleChangedFirstPts != INT64_MIN || 1) {
                index = mCurrentSubtitleIndex;
            }
        }
    }

    int ret = mDemuxerService->readPacket(pMedia_Frame, index);

    if (pMedia_Frame == nullptr) {
        //  AF_LOGD("Can't read packet %d\n", ret);
        if (ret == 0) {
            mSubtitleEOS = true;

            if (index != -1) {
                ret = -EAGAIN;
            }
        }

        return ret;
    }

    if (mPtsDiscontinueDelta == INT64_MIN) {
        int64_t maxGopTimeUs = mDemuxerService->getDemuxerHandle()->getMaxGopTimeUs();
        if (maxGopTimeUs > 0) {
            mPtsDiscontinueDelta = maxGopTimeUs;
        } else {
            mPtsDiscontinueDelta = PTS_DISCONTINUE_DELTA;
        }
        AF_LOGI("mPtsDiscontinueDelta = %lld", mPtsDiscontinueDelta);
    }

    // FIXME: transfer to frame
    if (pMedia_Frame->isProtected() && !mSecretPlayBack) {
        AF_LOGI("SecretPlayBack\n");
        mSecretPlayBack = true;

        if (!pMedia_Frame->getMagicKey().empty() && pMedia_Frame->getMagicKey() == mSet->drmMagicKey) {
            mDrmKeyValid = true;
        }
    }

    pFrame = pMedia_Frame.get();
    uint64_t size = mCalculateSpeedUsePacket ? pFrame->getSize() : 0;
    mUtil->notifyRead(MediaPlayerUtil::readEvent_Got, size);
    mMPAUtil->updateNetworkReadSize(size);

    // TODO: get the min first stream pts
    if (pFrame->getInfo().timePosition >= 0 && mMediaStartPts == INT64_MIN && pFrame->getInfo().streamIndex != mCurrentSubtitleIndex &&
        pFrame->getInfo().streamIndex != mWillChangedSubtitleStreamIndex) {
        mMediaStartPts = pFrame->getInfo().pts - pFrame->getInfo().timePosition;
    }

    if (mSeekFlag && mSeekNeedCatch) {
        if (pFrame->getInfo().timePosition < (mSeekPos - mSet->maxASeekDelta)) {
            // first frame is far away from seek position, don't suppport accurate seek
            mSeekNeedCatch = false;
        }
    }

    int id = GEN_STREAM_INDEX(pFrame->getInfo().streamIndex);

    if (mDuration < 0) {
        unique_ptr<streamMeta> pMeta;
        mDemuxerService->GetStreamMeta(pMeta, pFrame->getInfo().streamIndex, false);
        mDuration = ((Stream_meta *) (pMeta.get()))->duration;
    }

    std::deque<StreamInfo *> &streamInfoQueue = mMediaInfo.mStreamInfoQueue;
    if (id < streamInfoQueue.size() && streamInfoQueue[id]->type == ST_TYPE_VIDEO && mMainStreamId != -1 && id != mMainStreamId) {
        unique_ptr<streamMeta> pMeta;
        Stream_meta *meta{};
        int count = mDemuxerService->GetNbSubStream(id);

        for (int i = 0; i < count; i++) {
            mDemuxerService->GetStreamMeta(pMeta, GEN_STREAM_ID(id, i), true);
            meta = (Stream_meta *) (pMeta.get());
            int streamId = GEN_STREAM_ID(id, i);

            if (meta->type == STREAM_TYPE_VIDEO && meta->height > 0 && streamId != mCurrentVideoIndex) {
                mWillChangedVideoStreamIndex = streamId;
            } else if (meta->type == STREAM_TYPE_AUDIO && meta->channels > 0 && streamId != mCurrentAudioIndex) {
                mWillChangedAudioStreamIndex = streamId;
                mWATimeBase = meta->ptsTimeBase;
            } else if (meta->type == STREAM_TYPE_SUB && streamId != mCurrentSubtitleIndex) {
                mWillChangedSubtitleStreamIndex = streamId;
            }
        }

        mMainStreamId = id;
    }

    if (!mInited) {
        ProcessOpenStreamInit(pFrame->getInfo().streamIndex);
        ProcessUpdateView();
        mInited = true;
    }

    //        AF_LOGD("read packet pts is %lld,streamIndex is %d duration is %d\n", pFrame->getInfo().pts, pFrame->getInfo().streamIndex,
    //                pFrame->getInfo().duration);

    if (pFrame->getInfo().streamIndex == mCurrentVideoIndex ||
        pFrame->getInfo().streamIndex == mWillChangedVideoStreamIndex ||
        pFrame->getInfo().streamIndex == mPendingVideoStreamIndex) {
        mHaveVideoPkt = true;
        if (mMediaFrameCb && (!pMedia_Frame->isProtected() || mDrmKeyValid)) {
            mMediaFrameCb(mMediaFrameCbArg, pMedia_Frame.get(), ST_TYPE_VIDEO);
        }

        /* pending representation 不能进入 active 的公共视频队列。主流
         * 播放器会为新 period 保留独立的 demux/decode 缓冲，只有目标帧
         * 达到主时钟并 promote 后才切换输出。 */
        if ((mPendingVideoStreamIndex >= 0 || mWillChangedVideoStreamIndex >= 0) &&
            (pFrame->getInfo().streamIndex == mPendingVideoStreamIndex ||
             pFrame->getInfo().streamIndex == mWillChangedVideoStreamIndex) &&
            pFrame->getInfo().streamIndex != mCurrentVideoIndex) {
            /*
             * 硬上限：当前路快见底时读循环会继续读（见 doReadPacket），目标队列
             * 因此可能越过软上限。到这里就丢掉最新的目标包，保证内存有界。
             * 这只在"目标解码器明显跟不上、当前路又要断粮"时才会发生，属于两害
             * 相权取其轻：宁可目标路重来一次，也不能把整路画面冻死。
             */
            if (mPendingVideoPacketQue.size() >= PENDING_VIDEO_QUEUE_HARD_CAP) {
                if (floodLogAllowed(FLOOD_STALE_PENDING, 1, "drop pending video packet over the hard cap")) {
                    AF_LOGW("drop pending video packet over the hard cap (%d): stream=%d pts=%lld\n",
                            (int) PENDING_VIDEO_QUEUE_HARD_CAP, pFrame->getInfo().streamIndex,
                            (long long) pFrame->getInfo().pts);
                }

                pMedia_Frame->setDiscard(true);
                return ret;
            }

            mPendingVideoPacketQue.emplace_back(std::move(pMedia_Frame));
        } else {
            mBufferController->AddPacket(std::move(pMedia_Frame), BUFFER_TYPE_VIDEO);
        }
        mDemuxerService->SetOption("V_FRAME_RECEIVE", pFrame->getInfo().pts);

        if (mVideoInterlaced == InterlacedType_UNKNOWN) {
            if (mVideoParser == nullptr) {
                //FIXME mCurrentVideoMeta can not be update here.
                mDemuxerService->GetStreamMeta(mCurrentVideoMeta, pFrame->getInfo().streamIndex, false);
                auto *meta = (Stream_meta *) (mCurrentVideoMeta.get());
                mVideoParser = new bitStreamParser();
                mVideoParser->init(meta);

                {
#ifdef ENABLE_VIDEO_FILTER
                    std::lock_guard<std::mutex> filterLock(mFilterManagerMutex);
                    if (mFilterManager != nullptr) {
                        mFilterManager->setStreamMeta(meta);
                    }
#endif
                }
            }

            mVideoParser->parser(pFrame->getData(), static_cast<int>(pFrame->getSize()));
            mVideoInterlaced = mVideoParser->getInterlaced();

            if (mVideoInterlaced != InterlacedType_UNKNOWN) {
                delete mVideoParser;
                mVideoParser = nullptr;
            } else {
                mVideoParserTimes++;

                if (mVideoParserTimes > 10) {
                    mVideoInterlaced = InterlacedType_NO;
                    delete mVideoParser;
                    mVideoParser = nullptr;
                    mVideoParserTimes = 0;
                }
            }
        }

        if (mFirstVideoPts == INT64_MIN) {
            mFirstVideoPts = pFrame->getInfo().pts - pFrame->getInfo().timePosition;

            if (!HAVE_AUDIO) {
                mFirstSeekStartTime = pFrame->getInfo().timePosition;
            }
        }

        if (mSoughtVideoPos == INT64_MIN) {
            if (mSeekNeedCatch) {
                mSoughtVideoPos = mSeekPos;
            } else {
                mSoughtVideoPos = pFrame->getInfo().timePosition;
            }

            /*
                 * seek would clean the packet which have ExtraData in decoder queue,
                 * so add the ExtraData after seek on key frame
                 */
            if (/*mAdaptiveVideo &&*/ pFrame->getInfo().flags) {
                unique_ptr<streamMeta> pMeta;
                mDemuxerService->GetStreamMeta(pMeta, pFrame->getInfo().streamIndex, false);
                pFrame->setExtraData(((Stream_meta *) (*pMeta))->extradata, ((Stream_meta *) (*pMeta))->extradata_size);
            }
        }

        //this time video bitrate changed
        if (mVideoChangedFirstPts == INT64_MAX && pFrame->getInfo().streamIndex == mWillChangedVideoStreamIndex) {
            AF_LOGD("video stream first pts is %lld", pFrame->getInfo().pts);
            /* 只允许首个目标包进入切换状态机；不再把该 PTS 作为“已提交”
             * 的通知条件，真正的 stream changed 会在 pending decoder 提升后发送。 */
            mVideoChangedFirstPts = INT64_MIN;
            unique_ptr<streamMeta> pMeta;
            mDemuxerService->GetStreamMeta(pMeta, mWillChangedVideoStreamIndex, false);
            pFrame->setExtraData(((Stream_meta *) (*pMeta))->extradata, ((Stream_meta *) (*pMeta))->extradata_size);

            /*
             * 这是“清晰度切换”的开始（新码流的第一包到了，注意它可能只是
             * open 阶段漏出来的一条 pts=0 残留包，见下面 doDeCode() 的交叉点判断）。
             *
             * 如果新流的 codec id 或分辨率跟正在用的解码器对不上，就必须重建视频
             * 解码器，否则解码器一帧都出不来 —— 旧缓冲播完后画面就冻在最后一帧，
             * 而音频路（另一个解码器 + 主时钟）完全没被动，所以声音一直正常。
             * 这正是“切清晰度后画面卡死、声音正常”。
             *
             * 但这里**不能马上换**：切换是“无缝”设计的 —— 新码流被 seek 到旧缓冲
             * 结束的时间点，旧码流已解码/缓存的包会先把这段路播完（日志里切换后
             * 画面还能正常播 6~10 秒就是这个原因）。此刻换解码器会让还在缓冲里的
             * 旧码流包全部变成解不出来的垃圾，等于把画面立刻冻住。
             *
             * 所以只记一个标志，等 doDeCode() 里真正要解新码流的包时再换
             * （那时主时钟也已经走到同一个位置，画面无断点）。
             */
            if (!isVideoDecoderMetaMatched((const Stream_meta *) (*pMeta))) {
                const auto *changedMeta = (const Stream_meta *) (*pMeta);
                const auto *oldMeta = mAVDeviceManager->getDecoderMeta(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);

                AF_LOGW("video stream switched to incompatible parameters "
                        "(codec %d -> %d, %dx%d -> %dx%d), the video decoder will be rebuilt "
                        "when the new stream reaches the master clock\n",
                        oldMeta == nullptr ? -1 : (int) oldMeta->codec, (int) changedMeta->codec,
                        oldMeta == nullptr ? 0 : oldMeta->width, oldMeta == nullptr ? 0 : oldMeta->height,
                        changedMeta->width, changedMeta->height);
                mPendingVideoDecoderSwitch = true;
            } else {
                // Even when the codec is reusable, keep the target on the
                // pending path. This preserves one switch state machine for
                // all platforms and prevents packets from two representations
                // being interleaved in one decoder.
                mPendingVideoDecoderSwitch = true;
            }

            // 连续点击多个清晰度时，旧目标尚未完成的切换必须明确取消；
            // 旧 active decoder 和当前画面完全不受影响，新目标从头建立 pending。
            if (mPendingVideoStreamIndex >= 0 && mPendingVideoStreamIndex != mWillChangedVideoStreamIndex) {
                mPNotifier->NotifyVideoQualitySwitch(PLAYER_QUALITY_SWITCH_CANCELED,
                                                     mPendingVideoStreamIndex,
                                                     "quality switch superseded by a newer request");
                if (mDemuxerService != nullptr) {
                    mDemuxerService->CloseStream(mPendingVideoStreamIndex);
                    AF_LOGI("closed superseded pending video stream: stream=%d\n",
                            mPendingVideoStreamIndex);
                }
                mAVDeviceManager->discardPendingVideoDecoder();
                while (!mPendingVideoFrameQue.empty()) {
                    mPendingVideoFrameQue.front()->setDiscard(true);
                    mPendingVideoFrameQue.pop();
                }
            }

            // Keep the active stream/decoder alive until the pending decoder
            // has produced its first frame.  The old implementation changed
            // mCurrentVideoIndex here, which made the common packet queue feed
            // the new codec into the old decoder.
            /* 用户可能在上一轮目标帧真正上屏前再次点击清晰度。此时上一轮
             * 的 retired stream 已经不再是当前 active 路，不能被下面的新
             * mRetiredVideoStreamIndex 覆盖，否则它永远不会 CloseStream，
             * 后续 ReadPacket() 会持续注入旧 representation。先回收上一轮
             * 的 retired 资源，再建立本轮双路切换。 */
            if (mQualitySwitchCommitPending && mRetiredVideoStreamIndex >= 0 &&
                mRetiredVideoStreamIndex != mCurrentVideoIndex) {
                if (mDemuxerService != nullptr) {
                    mDemuxerService->CloseStream(mRetiredVideoStreamIndex);
                    AF_LOGI("closed previous retired stream before a new quality switch: stream=%d\n",
                            mRetiredVideoStreamIndex);
                }
                mAVDeviceManager->releaseRetiredVideoDecoder();
                mRetiredVideoStreamIndex = -1;
                mQualitySwitchCommitPending = false;
                mQualitySwitchCommittedStreamIndex = -1;
                mQualitySwitchOldFramesPending = 0;
            }
            mRetiredVideoStreamIndex = mCurrentVideoIndex;
            mPendingVideoStreamIndex = mWillChangedVideoStreamIndex;
            /*
             * 这是备用解码器的“目标时间”。切换请求发生在旧画面仍可播放时，
             * 不能使用首次读到的目标包时间（它往往是 init/segment 0），而要以
             * 切换时的主时钟为准。目标流的 packet timePosition 到达这个窗口后
             * 才允许进入 pending decoder；网络慢时旧 decoder 继续正常输出。
             */
            mPendingVideoSwitchTimePosition = mMasterClock.GetTime();
            if (mPendingVideoSwitchTimePosition < 0) {
                mPendingVideoSwitchTimePosition = mCurrentPos;
            }
            mPendingVideoSwitchStartMs = af_getsteady_ms();
            mPendingVideoInitPacketSent = false;
            mPendingVideoPtsOffset = INT64_MIN;
            /* 新的一次切换：预滚（丢弃窗口之前的数据）重新开始。 */
            mPendingVideoPrerollDone = false;
                        mPendingVideoPrerollRefUs = INT64_MIN;
            // 清晰度切换建立新的 pending 时间轴；旧 pending 帧不能带入本次
            // 切换，否则连续点击多个档位时会把上一目标的首帧误提交。
            while (!mPendingVideoFrameQue.empty()) {
                mPendingVideoFrameQue.front()->setDiscard(true);
                mPendingVideoFrameQue.pop();
            }
            mAVDeviceManager->discardPendingVideoDecoder();
            /* 当前 ReadPacket() 已经把触发状态机的首个目标 packet 放入
             * mPendingVideoPacketQue。这里不能再次清空，否则每次切换都会
             * 丢掉 init/关键帧，pending decoder 永远只能等下一次 GOP。旧
             * pending 队列已在“被新请求替换”分支和 seek/失败路径清理。 */
            mWillChangedVideoStreamIndex = -1;
        }
    } else if (pFrame->getInfo().streamIndex == mCurrentAudioIndex || pFrame->getInfo().streamIndex == mWillChangedAudioStreamIndex) {
        // printTimePosition(pFrame->getInfo().timePosition);
        mHaveAudioPkt = true;

        if (mFirstAudioPts == INT64_MIN) {
            mFirstAudioPts = pFrame->getInfo().pts - pFrame->getInfo().timePosition;
            mFirstSeekStartTime = pFrame->getInfo().timePosition;
        }

        if (mSeekFlag && mSeekNeedCatch && NeedDrop(pFrame->getInfo().timePosition, mSeekPos)) {
            return ret;
        }

        if (pFrame->getInfo().streamIndex == mWillChangedAudioStreamIndex) {
            mCurrentAudioIndex = mWillChangedAudioStreamIndex;
            mCATimeBase = mWATimeBase;
            mWillChangedAudioStreamIndex = -1;
        }

        if (mAudioChangedFirstPts == INT64_MAX) {
            int64_t playedTime = mMasterClock.GetTime();

            if (pFrame->getInfo().pts < playedTime) {
                return ret;
            } else {
                //recodr 64MAX for audio stream changed for first frame
                mAudioChangedFirstPts = pFrame->getInfo().pts;
            }
        }

        if (mMediaFrameCb && (!pMedia_Frame->isProtected() || mDrmKeyValid)) {
            mMediaFrameCb(mMediaFrameCbArg, pMedia_Frame.get(), ST_TYPE_AUDIO);
        }

        mBufferController->AddPacket(std::move(pMedia_Frame), BUFFER_TYPE_AUDIO);
        mDemuxerService->SetOption("A_FRAME_RECEIVE", pFrame->getInfo().pts);
    } else if (pFrame->getInfo().streamIndex == mCurrentSubtitleIndex || pFrame->getInfo().streamIndex == mWillChangedSubtitleStreamIndex) {
        if (mMediaFrameCb && (!pMedia_Frame->isProtected() || mDrmKeyValid)) {
            mMediaFrameCb(mMediaFrameCbArg, pMedia_Frame.get(), ST_TYPE_SUB);
        }

        mBufferController->AddPacket(std::move(pMedia_Frame), BUFFER_TYPE_SUBTITLE);
        AF_LOGD("read subtitle pts is %lld", pFrame->getInfo().pts);

        if (pFrame->getInfo().streamIndex == mWillChangedSubtitleStreamIndex) {
            mCurrentSubtitleIndex = mWillChangedSubtitleStreamIndex;
            mWillChangedSubtitleStreamIndex = -1;
        }
        if (mSubtitleChangedFirstPts == INT64_MAX) {
            mSubtitleChangedFirstPts = pFrame->getInfo().pts;
        }
    } else {
        if (floodLogAllowed(FLOOD_UNKNOWN_STREAM, 2, "unknown stream, read packet dropped")) {
            AF_LOGW("unknown stream %x, read packet pts is %lld; currentVideo=%d pendingVideo=%d willChangeVideo=%d retiredVideo=%d currentAudio=%d\n",
                    pFrame->getInfo().streamIndex, pFrame->getInfo().pts,
                    mCurrentVideoIndex, mPendingVideoStreamIndex,
                    mWillChangedVideoStreamIndex, mRetiredVideoStreamIndex,
                    mCurrentAudioIndex);
        }

        if (mCurrentAudioIndex < 0 || mCurrentVideoIndex < 0) {
            std::unique_ptr<streamMeta> meta;
            int ret1 = mDemuxerService->GetStreamMeta(meta, pFrame->getInfo().streamIndex, true);
            if (ret1 >= 0) {
                switch (((Stream_meta *) (*meta))->type) {
                    case STREAM_TYPE_VIDEO: {
                        if (!mSet->bDisableVideo && mCurrentVideoIndex < 0 && ((Stream_meta *) (*meta))->width > 0) {
                            mCurrentVideoIndex = pFrame->getInfo().streamIndex;
                            updateVideoMeta();
                        }
                        break;
                    }
                    case STREAM_TYPE_AUDIO: {
                        if (!mSet->bDisableAudio && !mOpenAudioDeviceFailed && mCurrentAudioIndex < 0 && ((Stream_meta *) (*meta))->channels > 0) {
                            mCurrentAudioIndex = pFrame->getInfo().streamIndex;
                            mCATimeBase = ((Stream_meta *) (*meta))->ptsTimeBase;
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    }

    if (mWillSwitchVideo) {
        int videoCount = 0;
        int64_t startTime = mBufferController->FindSeamlessPointTimePosition(BUFFER_TYPE_VIDEO, videoCount);

        if (startTime == 0 || videoCount < 40) {
            return ret;
        }

        if (mMixMode) {
            int64_t startTimeA = mBufferController->FindSeamlessPointTimePosition(BUFFER_TYPE_AUDIO, videoCount);

            if (startTimeA == 0 || videoCount < 40) {
                return ret;
            }

            startTime = std::max(startTime, startTimeA);
        }

        SwitchVideo(startTime);
        mWillSwitchVideo = false;
    }

    return ret;
}

void SuperMediaPlayer::printTimePosition(int64_t time) const
{
    if (time > 0) {
        time /= 1000;
        int timeM = static_cast<int>((time / 1000) / 60);
        int timeS = static_cast<int>((time / 1000) % 60);
        int timeMS = static_cast<int>(time - timeM * 60000 - timeS * 1000);
        AF_LOGD("timePosition is %d:%d:%d\n", timeM, timeS, timeMS);
    }
}

void SuperMediaPlayer::FlushAudioPath()
{
    /* 只有 seek 需要把主时钟重新锚到下一张音频帧。普通的缓冲追赶、停止或
     * 内部重建也会调用本函数，但这些场景不能无条件改写正在运行的主时钟，
     * 否则会把一次短暂的音频 flush 变成新的 A/V 跳变。 */
    const bool reanchorAfterFlush = mSeekFlag;
    mAVDeviceManager->flushDevice(SMPAVDeviceManager::DEVICE_TYPE_AUDIO);

    audioDecoderEOS = false;

    //flush frame queue
    while (!mAudioFrameQue.empty()) {
        mAudioFrameQue.pop_front();
    }

    mPlayedAudioPts = INT64_MIN;
    mAudioClockReanchorPending = false;
    mAudioPtsRevert = false;
    mAudioTime.startTime = 0;
    mAudioTime.deltaTime = 0;
    mAudioTime.deltaTimeTmp = 0;
    mAudioPacket = nullptr;
    mAudioEOS = false;
    // seek 后第一张音频帧重新建立音频时钟；不能沿用旧流的 PTS 修正状态。
    mFirstAudioPts = INT64_MIN;
    mFirstSeekStartTime = 0;
    mRemovedFirstAudioPts = INT64_MIN;
    mAudioClockReanchorPending = reanchorAfterFlush;
}

void SuperMediaPlayer::FlushVideoPath(bool flushRender, bool cancelPendingSwitch, const char *from)
{
    /*
     * 每次调用都留一行（限频）：以前这里一行日志都不打，而它会顺手清掉
     * mQualitySwitchCommitPending / mRetiredVideoStreamIndex —— 于是“提交后
     * 0.1 秒到底是谁把切换状态清了、READY 为什么永远没发”只能靠猜。
     */
    AF_LOGW("FlushVideoPath from %s (flushRender=%d cancelPendingSwitch=%d): "
            "commitPending=%d pendingStream=%d willChangeStream=%d retiredStream=%d current=%d "
            "seekFlag=%d seekNeedCatch=%d\n",
            from != nullptr ? from : "?", (int) flushRender, (int) cancelPendingSwitch,
            (int) mQualitySwitchCommitPending, mPendingVideoStreamIndex,
            mWillChangedVideoStreamIndex, mRetiredVideoStreamIndex, mCurrentVideoIndex,
            (int) mSeekFlag, (int) mSeekNeedCatch);

    /*
     * seek/stop 之前必须把 pending representation 从 demuxer manager 中撤掉。
     * 仅丢弃 pending decoder 不够：HLS/DASH manager 仍会把该流标记为 selected，
     * 下一次 Seek() 会同时给旧流和已取消的新流发包，随后 ReadPacket() 把它们
     * 识别成 unknown stream，最终表现为 seek 卡死或切换后无画面。
     */
    if (cancelPendingSwitch && mPendingVideoStreamIndex >= 0 &&
        mPendingVideoStreamIndex != mCurrentVideoIndex && mDemuxerService != nullptr) {
        mDemuxerService->CloseStream(mPendingVideoStreamIndex);
        AF_LOGI("closed pending video stream while canceling switch: stream=%d\n",
                mPendingVideoStreamIndex);
    }
    if (cancelPendingSwitch && mPendingVideoStreamIndex >= 0) {
        mPNotifier->NotifyVideoQualitySwitch(PLAYER_QUALITY_SWITCH_CANCELED,
                                             mPendingVideoStreamIndex,
                                             "quality switch canceled");
    }
    if (flushRender) {
        mAVDeviceManager->flushDevice(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);
    } else {
        /*
         * 只清解码器。flushDevice() 最后会 flushVideoRender()，那会
         * pause/start VSync 线程；seek 这种一次性“原地重启视频”没必要每次都去
         * 碰 afThread 的 pause/start 状态机——渲染器里缓存的旧帧下一帧本来
         * 就会被覆盖。
         */
        IDecoder *videoDecoder = mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);

        if (videoDecoder != nullptr) {
            videoDecoder->flush();
        }
    }

    videoDecoderEOS = false;
    //flush frame queue

    while (!mVideoFrameQue.empty()) {
        mVideoFrameQue.front()->setDiscard(true);
        mMsgCtrlListener->ProcessRenderedMsg(ST_TYPE_VIDEO, mVideoFrameQue.front()->getInfo(), af_getsteady_ms(), false, nullptr);
        mVideoFrameQue.pop();
    }
    while (!mPendingVideoFrameQue.empty()) {
        mPendingVideoFrameQue.front()->setDiscard(true);
        mPendingVideoFrameQue.pop();
    }
    /* seek/原地重定位时保留目标 stream，但必须丢掉旧 pending decoder 的内部状态，
     * 下一批目标包会按新的 seek 边界重新建立解码器。stop/reset 则彻底取消。 */
    mAVDeviceManager->discardPendingVideoDecoder();
    while (!mPendingVideoPacketQue.empty()) {
        mPendingVideoPacketQue.front()->setDiscard(true);
        mPendingVideoPacketQue.pop_front();
    }
    /*
     * 在途的切换必须走终态出口：光把 mQualitySwitchCommitPending 置 false 会让
     * mRetiredVideoStreamIndex 一起丢掉（下面那句），旧 Representation 就再也
     * 关不掉了 —— HLS 实测里旧 480p 流因此被一直读到 PTS 88s。
     * finishQualitySwitch() 会先关流再清状态，所以必须排在清 retired 之前。
     * 注意它只在**已提交**时才动（warming 阶段的取消由上面那句 CANCELED 负责）。
     */
    if (mQualitySwitchCommitPending || mQualitySwitchCommittedStreamIndex >= 0) {
        finishQualitySwitch(false, "video path flushed (seek/stop/catch-up)");
    }

    // seek/stop 时新旧切换都失去渲染条件，retired decoder 也必须立即释放，
    // 防止下一次 seek 长时间占用旧的硬解 surface。
    mAVDeviceManager->releaseRetiredVideoDecoder();

    mPendingVideoPtsOffset = INT64_MIN;
    mActiveVideoPtsOffset = INT64_MIN;
    mQualitySwitchCommitPending = false;
    mQualitySwitchCommittedStreamIndex = -1;
    mQualitySwitchOldFramesPending = 0;
    mQualitySwitchCommitMs = 0;
    mQualitySwitchDeadlineMs = 0;
    /*
     * 【2026-09-21 修：mPendingVideoDecoderSwitch 是个单向闩锁，必须在这里清】
     *
     * 它只由 ProcessVideoPacket 置 true，只有三条路会清：提交成功、切换超时失败、
     * reset()。**取消切换的路径（seek、FlushVideoPath）不清它** —— 于是它一旦置上就
     * 永久为真，而它是 qualitySwitchWarming 的一项：
     *
     *   1. RenderVideo() 里 `if (qualitySwitchWarming) render = true;` 会**绕过所有
     *      时钟节拍**：每一帧都往渲染器推。渲染器的输入队列（AFActiveVideoRender::
     *      mInputQueue，MAX_IN_SIZE）一满就把最旧的帧 dropFrame() 并把自己的
     *      渲染时钟重锚到最新帧 —— 现象就是画面快进、跳帧、进度条跳、A/V 不同步；
     *   2. `videoLateUs < -10ms` 的过滤也被跳过，早于时钟的帧照样上屏 —— 视频会
     *      一路跑到时钟前面。
     *
     * 实测（2026-09-21 20:44 那份日志）：用户 seek 取消了一次切换之后
     * `[switch] … target=-1 current=7 … decoderSwitch=1` 一直挂着，
     * 紧接着就是 `AFActiveVideoRender: drop a frame pts = 77.1s … 81.3s` 连续 77 条
     * （3 秒里把 4.1 秒的内容全丢掉）+ `PFR: seek` 之后再也回不到正常节拍。
     *
     * FlushVideoPath 本来就已经把 pending decoder 丢掉了（discardPendingVideoDecoder），
     * 留这个标志为真没有任何意义。
     */
    mPendingVideoDecoderSwitch = false;
    if (cancelPendingSwitch) {
        mPendingVideoStreamIndex = -1;
    }
    mPendingVideoSwitchTimePosition = INT64_MIN;
    mPendingVideoSwitchStartMs = 0;
    mPendingVideoInitPacketSent = false;
    mPendingVideoPrerollDone = false;
                        mPendingVideoPrerollRefUs = INT64_MIN;
    mRetiredVideoStreamIndex = -1;

    mPlayedVideoPts = INT64_MIN;
    mCurVideoPts = INT64_MIN;
    videoDecoderFull = false;
    mVideoPtsRevert = false;
    // seek 会建立全新的音视频时间轴；切换清晰度时保留音频时钟，但 seek 时必须
    // 清掉上一段流的音频滤波器累计偏移和输出队列，避免 DASH seek 后出现几十秒级
    // mDeltaPts，导致音频和视频再次脱节。
    mVideoPacket = nullptr;
    dropLateVideoFrames = false;
    mVideoCatchingUp = false;
    mVideoEOS = false;
}

void SuperMediaPlayer::FlushSubtitleInfo()
{
    while (!mSubtitleShowedQueue.empty()) {
        if (mSubtitleShowedQueue.front()) {
            mPNotifier->NotifySubtitleEvent(subTitle_event_hide, mSubtitleShowedQueue.front().release(), 0, nullptr);
        }

        mSubtitleShowedQueue.pop_front();
    }

    mSubtitleShowedQueue.clear();
    mSubtitleShowIndex = 0;
    mSubtitleEOS = false;
    if (mSubPlayer) {
        mSubPlayer->flush();
    }
}

void SuperMediaPlayer::PostBufferPositionMsg()
{
    if (mPlayStatus == PLAYER_PAUSED || mPlayStatus == PLAYER_PLAYING) {
        /*
         * 【2026-09-21 修：seek 期间不要发布"塌下来"的缓冲位置】
         *
         * 原来 seek 在途时强制 `duration = 0`，于是 `mBufferPosition = 当前位置 + 0`
         * —— 缓冲条当场塌到播放点，等 seek 结束缓冲值回来再"跳"出去。
         * 用户的原话是"每次 seek，后缓冲都是先归零再跳"，说的就是这里。
         *
         * seek 期间缓冲正在被清空/重建，报哪个值都是错的。干脆不发布：缓冲条保持
         * seek 之前的值，等 seek 真正结束（数据已经从新位置读进来）再一次性更新，
         * 和进度条自己的那一次跳变同步。
         */
        if (isSeeking()) {
            return;
        }

        int64_t duration = getPlayerBufferDuration(false, false);

        if (duration >= 0) {
            mBufferPosition = getCurrentPosition() + duration;

            if (mEof) {
                mBufferPosition = mDuration;
            }

            mPNotifier->NotifyBufferPosition((mBufferPosition <= mDuration ? mBufferPosition : mDuration) / 1000);
        }
    }
}

int64_t SuperMediaPlayer::getPlayerBufferDuration(bool gotMax, bool internal)
{
    int64_t durations[3] = {-1, -1, -1};
    int i = 0;
    int64_t duration = -1;

    if (HAVE_AUDIO) {
        int64_t &duration_c = durations[i++];
        duration_c = mBufferController->GetPacketDuration(BUFFER_TYPE_AUDIO);
        //            AF_LOGD("audioDuration is %lld\n",audioDuration);
        if (!internal && mDemuxerService && mDemuxerService->getDemuxerHandle()) {
            duration_c += mDemuxerService->getDemuxerHandle()->getBufferDuration(mCurrentAudioIndex);
        }

        if (mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_AUDIO)) {
            int64_t audioPacketDuration = mBufferController->GetOnePacketDuration(BUFFER_TYPE_AUDIO);
            if (audioPacketDuration <= 0) {
                audioPacketDuration = 23 * 1000;
            }
            duration_c += mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_AUDIO)->getInputPaddingSize() * audioPacketDuration;
        }

        if (mSet->preferAudio) {
            return duration_c;
        }
    }

    if (HAVE_VIDEO) {
        int64_t &duration_c = durations[i++];
        duration_c = mBufferController->GetPacketDuration(BUFFER_TYPE_VIDEO);

        //            AF_LOGD("videoDuration is %lld\n",videoDuration);
        if (duration_c < 0 && !HAVE_AUDIO) {
            duration_c = mBufferController->GetPacketLastPTS(BUFFER_TYPE_VIDEO) - mBufferController->GetPacketPts(BUFFER_TYPE_VIDEO);

            if (duration_c <= 0) {
                duration_c = (int64_t) mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO) * 40 * 1000;
            }
        }
        if (!internal && mDemuxerService && mDemuxerService->getDemuxerHandle()) {
            duration_c += mDemuxerService->getDemuxerHandle()->getBufferDuration(mCurrentVideoIndex);
        }

        if (mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)) {
            // FIXME: get the accurate duration
            duration_c += mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)->getInputPaddingSize() * 40 * 1000;
        }
    }

    /*
         *  Do not let player loading when switching subtitle, we'll read subtitle first
         *  in ReadPacket()
         */
    if (HAVE_SUBTITLE && !mSubtitleEOS && mSubtitleChangedFirstPts == INT64_MIN) {
        int64_t &duration_c = durations[i++];
        duration_c = mBufferController->GetPacketDuration(BUFFER_TYPE_SUBTITLE);
        if (!internal && mDemuxerService && mDemuxerService->getDemuxerHandle()) {
            duration_c += mDemuxerService->getDemuxerHandle()->getBufferDuration(mCurrentSubtitleIndex);
        }
    }

    int num = i;

    for (i = 0; i < num; i++) {
        if (durations[i] < 0) {
            continue;
        }
        if (duration < 0) {
            duration = durations[i];
        } else {
            duration = gotMax ? std::max(duration, durations[i]) : std::min(duration, durations[i]);
        }
    }

    return duration;
}

bool SuperMediaPlayer::SeekInCache(int64_t pos)
{
    int64_t audioLastPos = mBufferController->GetPacketLastTimePos(BUFFER_TYPE_AUDIO);
    int64_t videoLastPos = mBufferController->GetPacketLastTimePos(BUFFER_TYPE_VIDEO);
    int64_t minLastPos = -1;

    if (HAVE_VIDEO && HAVE_AUDIO) {
        minLastPos = audioLastPos < videoLastPos ? audioLastPos : videoLastPos;
    } else if (HAVE_VIDEO) {
        minLastPos = videoLastPos;
    } else if (HAVE_AUDIO) {
        minLastPos = audioLastPos;
    } else {
        // no video and audio ?
    }

    //seek bigger than last frame
    if (minLastPos > 0 && pos > minLastPos) {
        return false;
    }

    //seek back
    if (pos <= mCurrentPos) {
        mBufferController->Rewind(BUFFER_TYPE_ALL);
        int64_t audioFirstPos = mBufferController->GetPacketFirstTimePos(BUFFER_TYPE_AUDIO);
        int64_t videoFirstPos = mBufferController->GetPacketFirstTimePos(BUFFER_TYPE_VIDEO);
        int64_t maxFirstPos = -1;
        if (HAVE_VIDEO && HAVE_AUDIO) {
            maxFirstPos = std::max(audioFirstPos, videoFirstPos);
        } else if (HAVE_VIDEO) {
            maxFirstPos = videoFirstPos;
        } else if (HAVE_AUDIO) {
            maxFirstPos = audioFirstPos;
        } else {
            // no video and audio ?
        }

        if (maxFirstPos > 0 && pos < maxFirstPos) {
            return false;
        }
    }

    int64_t keyPosBefore = INT64_MIN;

    //can not find last key video pts,return
    if (HAVE_VIDEO) {
        keyPosBefore = mBufferController->GetKeyTimePositionBefore(BUFFER_TYPE_VIDEO, pos);
    } else if (HAVE_AUDIO) {
        keyPosBefore = mBufferController->GetKeyTimePositionBefore(BUFFER_TYPE_AUDIO, pos);
    }

    if (keyPosBefore == INT64_MIN) {
        return false;
    }

    mBufferController->ClearPacketBeforeTimePos(BUFFER_TYPE_ALL, keyPosBefore);
    mSoughtVideoPos = keyPosBefore;
    return true;
}

/*
 * 切换清晰度时，新码流不一定是“老解码器能解的那条流”：
 *
 *   * codec id 变了。DASH 的同一个清晰度在 MPD 里是 h264 / h265 两条独立的
 *     Representation（日志 15:50:30 那次就是 h265-854_480 -> h264-1920_1080）。
 *     FFmpeg 解码器在 avcodecDecoder::init_decoder() 里用
 *     avcodec_find_decoder(codecId) 定死了 codec（avcodecDecoder.cpp:258），
 *     之后再喂另一种标准的码流只会一路 “Skipping invalid undecodable NALU”，
 *     一帧都出不来；
 *   * 分辨率变了。HLS 的 master.m3u8 各档就是同一个 h264 的不同尺寸
 *     （日志 15:51:07 那次是 h264-854_480 -> h264-3840_2160）。D3D11VA 的
 *     表面池是第一帧时按当时的分辨率分配的（avcodecDecoder.cpp:343 的注释），
 *     尺寸换了之后同样解不出帧（日志里零拷贝转换器也一直停在
 *     “D3D11 video processor ready: 854x480”，没有为 4K 重建过）。
 *
 * 这两种情况 pFrame->setExtraData() 都救不了：它只是把新码流的参数集作为
 * AV_PKT_DATA_NEW_EXTRADATA 交给解码器（avcodecDecoder.cpp:864-873），
 * 只在“同一个 codec、同一个分辨率、只是 SPS/PPS 变了”时才有意义。
 *
 * 尺寸拿不到（0）时不据此判为不匹配，避免重建风暴。
 */
bool SuperMediaPlayer::isVideoDecoderMetaMatched(const Stream_meta *newMeta) const
{
    if (newMeta == nullptr) {
        return true;
    }

    /*
     * 用解码器自己的 meta（setUpDecoder 建解码器时存下来的那份）来比，
     * 不用 mCurrentVideoMeta：后者会被 ProcessUpdateView() 等路径按
     * mCurrentVideoIndex 刷新，切换途中有可能已经被刷成新码流的 meta，
     * 那样就会误判成“能解”。
     */
    const Stream_meta *cur = mAVDeviceManager->getDecoderMeta(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);

    if (cur == nullptr || cur->codec == 0) {
        return true;
    }

    if (cur->codec != newMeta->codec) {
        return false;
    }

    if (cur->width > 0 && cur->height > 0 && newMeta->width > 0 && newMeta->height > 0) {
        return cur->width == newMeta->width && cur->height == newMeta->height;
    }

    return true;
}

void SuperMediaPlayer::SwitchVideo(int64_t startTime)
{
    AF_LOGD("video change find start time is %lld", startTime);
    const int targetStreamIndex = mWillChangedVideoStreamIndex;
    int ret = mDemuxerService->OpenStream(targetStreamIndex);

    if (ret < 0) {
        AF_LOGW("switch video open stream failed, target stream index %d\n", targetStreamIndex);
        mPNotifier->NotifyVideoQualitySwitch(PLAYER_QUALITY_SWITCH_FAILED,
                                             targetStreamIndex,
                                             "target video stream open failed");
        mWillChangedVideoStreamIndex = -1;
        return;
    }

    /*
     * 不要在这里关闭旧流。SwitchVideo() 只是启动目标路，旧流要一直保留到
     * TryCommitPendingVideoSwitch() 完成并且新帧真正上屏；否则 pending 路还在
     * 追关键帧时，active decoder 会立刻断粮，音画时钟自然分离。旧流的关闭由
     * 提交后的安全点执行。
     */
    ret = mDemuxerService->Seek(startTime / 1000 * 1000, 0, targetStreamIndex);
    if (ret < 0) {
        AF_LOGW("switch video seek failed, target stream index %d ret=%d\n", targetStreamIndex, ret);
        mDemuxerService->CloseStream(targetStreamIndex);
        mPNotifier->NotifyVideoQualitySwitch(PLAYER_QUALITY_SWITCH_FAILED,
                                             targetStreamIndex,
                                             "target video stream seek failed");
        mWillChangedVideoStreamIndex = -1;
        return;
    }

    /*
     * 这里绝对不能清空 active 路在 startTime 之后的缓冲。
     *
     * 旧实现为了“把切换点对齐”调用 ClearPacketAfterTimePosition()，但
     * BufferController 只有一条公共视频队列，里面保存的是当前 active
     * representation 的数据。目标 representation 此时还没有进入队列，
     * 因而这句实际上删掉了旧 decoder 继续播放所需的全部后续 GOP。目标
     * 流又必须先下载 init + 关键帧、解码并追到主时钟后才能 promote，于是
     * 两条路之间出现真空，日志表现为 video fps=0/1、声音继续播放。
     *
     * 主流播放器的切换顺序是：保留 active 缓冲 -> 单独预热目标 period/
     * decoder -> 在目标帧可显示时原子替换。目标流的 seek 已在上面按
     * startTime 完成，不需要再修改 active 公共队列。
     */

    mWillSwitchVideo = false;
    mVideoChangedFirstPts = INT64_MAX;
    mEof = false;
}

int64_t SuperMediaPlayer::getAudioPlayTimeStampCB(void *arg)
{
    auto *pHandle = static_cast<SuperMediaPlayer *>(arg);
    return pHandle->getAudioPlayTimeStamp();
}

int64_t SuperMediaPlayer::getAudioPlayTimeStamp()
{
    if (!mAVDeviceManager->isAudioRenderValid()) {
        return INT64_MIN;
    }

    if (mSeekFlag) {
        return INT64_MIN;
    }

    /*
     * seek 之后音频路被 flush，mAudioTime.startTime 归零、而音频渲染器的位置
     * 回调还会返回上一段播放累积的旧位置（实测残留 ~2.5s）。此时如果让
     * SystemReferClock::GetTime() 拿这个值去 reSync，主时钟会瞬间跳到
     * "seek 目标 + 2.5s"，追赶窗口随之丢掉目标帧（见 ProcessSeekToMsg 里的说明）。
     * 第一张 seek 后的音频帧上屏时（renderAudioFrame 里 mAudioClockReanchorPending
     * 分支）才会把时钟锚到真实位置，在那之前一律视为"音频时钟不可用"。
     */
    if (mAudioClockReanchorPending) {
        return INT64_MIN;
    }

    int64_t aoutPos;
    aoutPos = mAVDeviceManager->getAudioRenderPosition();
    return mAudioTime.startTime + mAudioTime.deltaTime + aoutPos;
}

void SuperMediaPlayer::GetVideoResolution(int &width, int &height)
{
    width = mVideoWidth;
    height = mVideoHeight;
}

void SuperMediaPlayer::GetVideoRotation(int &rotation)
{
    rotation = mVideoRotation;
}

int SuperMediaPlayer::setUpAudioDecoder(const Stream_meta *meta)
{
    int ret = 0;

    if (meta->samplerate <= 0) {// meta.frame_size maybe 0 when playing artp
        ret = 0;
        return 0;
    }

    if (meta->duration > mDuration) {
        mDuration = meta->duration;
    }

    //setVolume to current setting after create new.
    SetVolume(mSet->mVolume);

    if (mSet->bMute) {
        mMsgCtrlListener->ProcessMuteMsg();
    }

    uint64_t flags = DECFLAG_SW;

#ifdef ANDROID
    bool isWideVineVideo = (meta->keyFormat != nullptr && strcmp(meta->keyFormat, "urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed") == 0);
    if (isWideVineVideo) {
        flags |= DECFLAG_HW;
    }
    // 音频硬解（AAC→MediaCodec）在这台机器上实测是净亏损：MediaCodec 以
    // 进程内 codec2 模式运行，每多一个实例就多一个厂商 looper 线程
    // （MediaCodec_loop + CodecLooper）加一个 10ms 轮询的输入循环
    // （AFActiveDecoder），合计 ≈9% CPU；而 AAC 软解只需 ~1-2%。
    // ExoPlayer 的 MediaCodecAudioRenderer 靠异步回调（setCallback）省掉
    // 轮询，但异步模式在本机回调不送达会起播失败（ASYNC_ENABLED 默认关）。
    // 结论：默认走 FFmpeg 软解，硬件音频解码等异步回调在目标机验证后再开。
    bool tryHwAudio = false && (meta->codec == AF_CODEC_ID_AAC)
            && (meta->extradata != nullptr && meta->extradata_size >= 2)
            && mSet->bEnableHwVideoDecode;
    if (tryHwAudio) {
        flags |= DECFLAG_HW;
    }
#endif

    ret = mAVDeviceManager->setUpDecoder(flags, meta, nullptr, SMPAVDeviceManager::DEVICE_TYPE_AUDIO, 0);

#ifdef ANDROID
    if (ret < 0 && tryHwAudio) {
        // MediaCodec AAC 创建失败 → 回退纯软解，保证可播
        AF_LOGW("audio hw decode failed, fallback to software decoder");
        ret = mAVDeviceManager->setUpDecoder(DECFLAG_SW, meta, nullptr, SMPAVDeviceManager::DEVICE_TYPE_AUDIO, 0);
    }
#endif

    if (ret < 0) {
        MediaPlayerEventType type = MEDIA_PLAYER_EVENT_AUDIO_DECODER_DEVICE_ERROR;
        if (ret == gen_framework_errno(error_class_codec, codec_error_audio_not_support)) {
            type = MEDIA_PLAYER_EVENT_AUDIO_CODEC_NOT_SUPPORT;
        }
        AF_LOGE("setUpAudioDecoder error %d\n", ret);
        mPNotifier->NotifyEvent(type, framework_err2_string(ret));
        return ret;
    }

    return ret;
}

int SuperMediaPlayer::SetUpAudioPath()
{
    int ret = 0;
    if (!mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_AUDIO)) {

        /*
         * make sure the audio stream is opened before get stream meta,
         * otherwise, will crash in hls stream
         */
        if (mBufferController->IsPacketEmtpy(BUFFER_TYPE_AUDIO)) {
            return 0;
        }

        unique_ptr<streamMeta> pMeta{};
        mDemuxerService->GetStreamMeta(pMeta, mCurrentAudioIndex, false);
        auto *meta = (Stream_meta *) (pMeta.get());

        int64_t startTimeMs = af_getsteady_ms();

        ret = setUpAudioDecoder(meta);

        int64_t costTimeMs = af_getsteady_ms() - startTimeMs;
        mRecorderSet->createAudioDecoderCostMs = costTimeMs;

        if (ret < 0) {
            return ret;
        }
    }

    if (mAudioFrameQue.empty() || mAVDeviceManager->isAudioRenderValid()) {
        return 0;
    }

    //        IAFFrame::audioInfo info = ;
    //        info.channels = meta->channels;
    //        info.sample_rate = meta->samplerate;
    //        info.format = meta->sample_fmt;
    //        info.nb_samples = meta->frame_size;
    //        info.channel_layout = meta->channel_layout;
    setUpAudioRender(mAudioFrameQue.front()->getInfo().audio);
    return ret;
}

int SuperMediaPlayer::setUpAudioRender(const IAFFrame::audioInfo &info)
{
    int ret = mAVDeviceManager->setUpAudioRender(info);

    if (ret < 0) {
        AF_LOGE("AudioOutHandle Init Error is %d", ret);
        // don't release audio handle because we only new it in constructor
        // PS: we should try to recover it later, or notify error
        //                    mAudioOutHandle = 0;
        mCurrentAudioIndex = -1;
        mCATimeBase = 0;
        return -1;
    }
    mAVDeviceManager->setAudioRenderListener(mAudioRenderCB.get());
    mAVDeviceManager->setSpeed(mSet->rate);
    mAVDeviceManager->setMute(mSet->bMute);
    mAVDeviceManager->setVolume(mSet->mVolume);

    if (!mSecretPlayBack) {
        mAVDeviceManager->setAudioRenderingCb(mAudioRenderingCb, mAudioRenderingCbUserData);
    }
    return 0;
}

int SuperMediaPlayer::setUpVideoRender(uint64_t flags)
{
    if (mAppStatus == APP_BACKGROUND) {
        AF_LOGW("create video render in background");
    }

    AF_LOGD("SetUpVideoRender start");
    CreateVideoRender(flags);
    if (!mAVDeviceManager->isVideoRenderValid()) {
        AF_LOGE("can't create video render\n");
        mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_VIDEO_RENDER_INIT_ERROR, "init video render failed");
        return -EINVAL;
    }

    if (!mSecretPlayBack) {
        mAVDeviceManager->setVideoRenderingCb(mVideoRenderingCb, mVideoRenderingCbUserData);
    }

    //re set view in case for not set view before
    if (mSet->mView) {
        if (mAVDeviceManager->isVideoRenderValid()) {
            mAVDeviceManager->getVideoRender()->setDisPlay(mSet->mView);
        }
    }

    return 0;
}


int SuperMediaPlayer::SetUpVideoPath()
{
    if (mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO) && (mAVDeviceManager->isVideoRenderValid())) {
        return 0;
    }

    if (mBufferController->IsPacketEmtpy(BUFFER_TYPE_VIDEO)) {
        return 0;
    }

    if (mVideoInterlaced == InterlacedType_UNKNOWN) {
        AF_LOGW("Wait for parser video interlaced Type");
        return 0;
    }
    /*
     * update the video meta after the first video packet was reached,
     * otherwise the video meta is incomplete when playing a master hls playList.
     */
    updateVideoMeta();
    auto *meta = (Stream_meta *) (mCurrentVideoMeta.get());

    bool bHW = false;
    if (mSet->bEnableHwVideoDecode) {
        switch (meta->codec) {
            case AF_CODEC_ID_H264: {
                string value = getProperty("ro.video.dec.h264");
                bHW = !(value == "OFF");
                break;
            }
            case AF_CODEC_ID_HEVC: {
                string value = getProperty("ro.video.dec.hevc");
                bHW = !(value == "OFF");
                break;
            }

            default:
                bHW = true;
                break;
        }
    }

    bool tunnelRender = mSet->bEnableTunnelRender;
    if (!mSet->bEnableHwVideoDecode || !bHW) {
        //soft decoder not support tunnel Render
        tunnelRender = false;
    }

    uint64_t flags = 0;


    if (isHDRVideo(meta)) {
        /*
         * HDR video must use mediaCodec to render direct on Android,
         * we use a dummy render to release the frame simply
         */
#ifdef ANDROID
        flags |= IVideoRender::FLAG_DUMMY;
#else
        flags |= IVideoRender::FLAG_HDR;
#endif
    }
#ifdef ANDROID
    bool isWideVine = isWideVineVideo(meta);
#endif

    if (tunnelRender
#ifdef ANDROID
        || isWideVine
#endif
    ) {
        flags |= IVideoRender::FLAG_DUMMY;
    }

    AF_LOGI("SetUpVideoPath tunnelRender=%d hw=%d renderFlags=%" PRIx64 " view=%p\n",
            (int) tunnelRender, (int) bHW, flags, mSet->mView.load());

    int ret = setUpVideoRender(flags);
    if (ret < 0) {
        return ret;
    }

    if (mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)) {
        return 0;
    }

    AF_LOGD("SetUpVideoDecoder start");

    if (meta->interlaced == InterlacedType_UNKNOWN) {
        meta->interlaced = mVideoInterlaced;
    }

#ifdef ANDROID
    if (bHW && mAVDeviceManager->isVideoRenderValid() && mAVDeviceManager->getVideoRender()->getFlags() & IVideoRender::FLAG_DUMMY &&
        mSet->mView == nullptr) {
        return 0;
    }
#endif

    int64_t startTimeMs = af_getsteady_ms();
    ret = CreateVideoDecoder(bHW, *meta);

    if (ret < 0) {
        if (bHW) {

            if (flags & IVideoRender::FLAG_DUMMY) {
                flags &= ~IVideoRender::FLAG_DUMMY;
                ret = setUpVideoRender(flags);
                if (ret < 0) {
                    return ret;
                }
            }

            // Hardware decoding is the default, but a failure here must not cost
            // the user the video: drop to the software decoder and keep playing.
            // On Windows that is the D3D11VA path in avcodecDecoder failing
            // (no hwaccel in the linked FFmpeg, or the D3D11 device could not be
            // created); avcodec_get_hw_config() decides, so an unavailable
            // hwaccel is detected before any frame is decoded.
            ret = CreateVideoDecoder(false, *meta);
        }
    }

    int64_t costTimeMs = af_getsteady_ms() - startTimeMs;
    mRecorderSet->createVideoDecoderCostMs = costTimeMs;

    if (ret < 0) {
        AF_LOGE("%s CreateVideoDecoder failed, error msg is  %s", __FUNCTION__, framework_err2_string(ret));

        if (ret == gen_framework_errno(error_class_codec, codec_error_video_not_support)) {
            mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_VIDEO_CODEC_NOT_SUPPORT, framework_err2_string(ret));
        } else if (ret == gen_framework_errno(error_class_codec, codec_error_video_device_error)) {
            mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_VIDEO_DECODER_DEVICE_ERROR, framework_err2_string(ret));
        }

        return ret;
    }

    if (mAVDeviceManager->getVideoDecoderFlags() & DECFLAG_HW) {
    } else {
        if (mSet->bEnableHwVideoDecode) {
            mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_SW_VIDEO_DECODER, "Switch to software video decoder");
        }
    }

    if (meta->duration > mDuration) {
        mDuration = meta->duration;
    }

    return ret;
}

void SuperMediaPlayer::updateVideoMeta()
{
    mDemuxerService->GetStreamMeta(mCurrentVideoMeta, mCurrentVideoIndex, false);
    auto *meta = (Stream_meta *) (mCurrentVideoMeta.get());
    {
#ifdef ENABLE_VIDEO_FILTER
        std::lock_guard<std::mutex> filterLock(mFilterManagerMutex);
        if (mFilterManager != nullptr) {
            mFilterManager->setStreamMeta(meta);
        }
#endif
    }

    int with = meta->displayWidth == 0 ? meta->width : meta->displayWidth;
    int height = meta->displayHeight == 0 ? meta->height : meta->displayHeight;
    // TunnelRender（dummy render）直通时旋转由 MediaCodec 完成，
    // 对外上报的尺寸需与旋转后的显示方向一致（90/270 交换宽高）
    if (mAVDeviceManager->isVideoRenderValid() &&
        (mAVDeviceManager->getVideoRender()->getFlags() & IVideoRender::FLAG_DUMMY) &&
        (meta->rotate == 90 || meta->rotate == 270)) {
        int tmp = with;
        with = height;
        height = tmp;
    }
    if (mVideoWidth != with || mVideoHeight != height || mVideoRotation != meta->rotate) {
        mVideoWidth = with;
        mVideoHeight = height;
        mVideoRotation = meta->rotate;
        mPNotifier->NotifyVideoSizeChanged(mVideoWidth, mVideoHeight);
    }
}

bool SuperMediaPlayer::CreateVideoRender(uint64_t flags)
{
    if (mAVDeviceManager->isVideoRenderValid() && mAVDeviceManager->getVideoRender()->getFlags() == flags) {
        return true;
    }
    mAVDeviceManager->createVideoRender(flags);
    if (!mAVDeviceManager->getVideoRender()) {
        return false;
    }
    mAVDeviceManager->getVideoRender()->setScale(convertScaleMode(mSet->scaleMode));
    mAVDeviceManager->getVideoRender()->setRotate(convertRotateMode(mSet->rotateMode));
    mAVDeviceManager->getVideoRender()->setBackgroundColor(mSet->mVideoBackgroundColor);
    mAVDeviceManager->getVideoRender()->setFlip(convertMirrorMode(mSet->mirrorMode));
    mAVDeviceManager->getVideoRender()->setDisPlay(mSet->mView);
    mAVDeviceManager->setVideoRenderListener(mVideoRenderListener.get());
    mAVDeviceManager->getVideoRender()->setVideoProcessTextureCb(mVideoProcessCb.get());

    int renderRet = mAVDeviceManager->getVideoRender()->init();

    if (renderRet != 0) {
        // for windows init failed, which may need change render type in future.
        mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_VIDEO_RENDER_INIT_ERROR, "init video render failed");
    }
    mAVDeviceManager->setSpeed(mSet->rate);
    mVideoRenderInited = true;
    return true;
}

int SuperMediaPlayer::CreateVideoDecoder(bool bHW, Stream_meta &meta)
{
    int ret;
    uint64_t decFlag = 0;

    if (bHW) {
        decFlag |= DECFLAG_HW;
    } else {
        decFlag |= DECFLAG_SW;
    }

    if (mAdaptiveVideo) {
        decFlag |= DECFLAG_ADAPTIVE;
    }
    if (!mSet->bLowLatency) {
        mSet->bLowLatency = mDemuxerService->getDemuxerHandle()->isLowLatency();
    }

    if (mSet->bLowLatency) {
        decFlag |= DECFLAG_OUTPUT_FRAME_ASAP;
    }

    void *view = nullptr;
    mAVDeviceManager->flushVideoRender();

    if (bHW) {
        if (mAVDeviceManager->isVideoRenderValid() && mAVDeviceManager->getVideoRender()->getFlags() & IVideoRender::FLAG_DUMMY) {
            view = mSet->mView;
            decFlag |= DECFLAG_DIRECT;
        } else {
            if (mAVDeviceManager->isVideoRenderValid()) {
                /*
                 * Get a cached surface, then the mediaCodec video decoder can be reuse,
                 * otherwise the mediaCodec video decoder will be recreate on setUpDecoder.
                 */
                view = mAVDeviceManager->getVideoRender()->getSurface(false);
            }
        }
    }

    if (!mSet->bLowLatency) {
        mSet->bLowLatency = mDemuxerService->getDemuxerHandle()->isLowLatency();
    }

    if (mSet->bLowLatency) {
        decFlag |= DECFLAG_OUTPUT_FRAME_ASAP;
    }
    uint32_t dstFormat = 0;
#ifdef __APPLE__
    dstFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    if ((mFrameCb || mVideoRenderingCb) && mSet->pixelBufferOutputFormat) {
        dstFormat = mSet->pixelBufferOutputFormat;
    } else {
        // TODO: move to VTB decoder
        /*
         *  must set hdr video to output x420, otherwise vtb will output a p420 format
         *  if iOS version little than 14 that can't be displayed by AVSampleBufferDisplayLayer
         */
        if (meta.pixel_fmt == AF_PIX_FMT_YUV420P10BE || meta.pixel_fmt == AF_PIX_FMT_YUV420P10LE) {
            dstFormat = kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
        }
    }
#endif
    ret = mAVDeviceManager->setUpDecoder(decFlag, (const Stream_meta *) (&meta), view, SMPAVDeviceManager::DEVICE_TYPE_VIDEO, dstFormat);
    AF_LOGI("CreateVideoDecoder bHW=%d decFlag=%" PRIx64 " view=%p ret=%d\n",
            (int) bHW, decFlag, view, ret);
    if (ret < 0) {
        return ret;
    }

    /*
     * 刚建好的解码器不可能已经在 EOS 状态：videoDecoderEOS 只描述"当前这个解码器
     * 报过 EOS"。以前重建路径不清它，而 DecodeVideoPacket() 开头见到它就整个
     * return —— 于是 Android 上解码器自愈关闭（mediaCodecDecoder 的 codec input
     * stuck、或输入侧硬错误）留下的 videoDecoderEOS=true 会一直生效：错误驱动
     * 重建和 doRender() 的超时兜底都白重建，新解码器一包都收不到，画面永久冻住
     * （2026-09-23 22:37 日志：22:37:59.690 关闭解码器后，整份日志再没有出现过
     * MediaCodec init）。与 ActiveDecoder::flush() 里复位 bDecoderEOS 同一个道理。
     */
    videoDecoderEOS = false;

    {
        std::lock_guard<std::mutex> lock(mAppStatusMutex);
        mMsgCtrlListener->ProcessVideoHoldMsg(mAppStatus == APP_BACKGROUND);
    }
    return ret;
}

/*
 * 视频解码器的“release + 重新 init”路径。两个调用者共用同一份实现：
 *
 *   RestartVideoDecoder()        —— requireDummyRender=true。由 ProcessSetViewMsg
 *                                   在持有 mCreateMutex 时调用：setOutputSurface
 *                                   热切换失败（codec 已失效，如后台期间 surface
 *                                   被系统销毁 → ACodec 报错）时，只重建视频解码器，
 *                                   音频/解复用不动，新解码器绑定新 surface。
 *   DecodeVideoPacket() 的错误恢复 —— requireDummyRender=false。codec 报错
 *                                   （STATUS_HAVE_ERROR / STATUS_DRM_ERROR 见那里）。
 *   doRender() 的一次性兜底        —— requireDummyRender=false。后端连错误都不报的
 *                                   静默停摆（本内核自己的兜底，不是主流做法）。
 *
 * 这套**动作**是照主流做的、而且只用可移植接口：
 *   ExoPlayer 的 MediaCodecRenderer 在 onCodecError()/CodecException 之后走
 *   releaseCodec() + maybeInitCodecOrBypass()，失败时按构造参数 enableDecoderFallback
 *   回退到低优先级解码器；回退链走完才 setPendingPlaybackException() 报错。
 *   这里一一对应：invalidateDecoder()（= releaseCodec）、CreateVideoDecoder()
 *   （= maybeInitCodec）、硬解失败自动落软解（= enableDecoderFallback）、
 *   建不起来就上报警告/致命错误（= setPendingPlaybackException）。
 *   差别只有一处：ExoPlayer 只由 codec 报错触发，本函数多了一个超时兜底调用者。
 *
 * 它“不留后患”的原因：
 *   * 只走 invalidateDecoder() + CreateVideoDecoder()（= setUpDecoder()）这条
 *     起播时就在用的现成路径，硬解失败还有软解兜底；
 *   * **不** FlushVideoPath()、**不**跳关键帧、**不**动音频与主时钟 —— 播放中
 *     flush 整条管线正是 2026-09-23 卡死的原因。
 *     注意别把两件事混为一谈：CreateVideoDecoder() 内部确实会调
 *     mAVDeviceManager->flushVideoRender()，那是内核**每次建解码器/切档都会走**
 *     的正常初始化（只重置渲染器输入队列/VSync），不是 FlushVideoPath() 那种
 *     “清包 + 取消在途切换 + 清 mPendingVideoDecoderSwitch 闩锁”的整条管线 flush；
 *   * 与 setUpVideoPath()/CreateVideoDecoder() 同线程（主循环），不引入新的并发；
 *   * 各平台同一份代码：Android MediaCodec、桌面 FFmpeg + D3D11/DXVA/VAAPI、
 *     Apple VideoToolbox 都经 IDecoder/SMPAVDeviceManager 这层进来，没有平台分支。
 * 代价：新解码器要从下一个关键帧重新建立参考帧链，画面会短暂停一下 —— 这是
 * “解码器已经坏了”场景下可接受的代价，主流播放器重建 codec 时同样如此。
 */
int SuperMediaPlayer::rebuildVideoDecoder(bool requireDummyRender)
{
    /*
     * 解码器槽位可以是 valid=false（“僵尸”）状态 —— 那正是需要重建的时候，
     * 所以这里**不**要求解码器当前有效，只要求视频路存在且渲染器可用。
     */
    if (mAVDeviceManager == nullptr || !HAVE_VIDEO ||
        !mAVDeviceManager->isVideoRenderValid()) {
        return -EINVAL;
    }

    if (requireDummyRender &&
        (mAVDeviceManager->getVideoRender()->getFlags() & IVideoRender::FLAG_DUMMY) == 0) {
        // RestartVideoDecoder() 的调用场景（surface 热切换）只对 dummy/tunnel 有意义。
        return -EINVAL;
    }

    // 用栈上副本，避免与主循环线程并发读写共享的 mCurrentVideoMeta
    Stream_meta meta{};
    if (mDemuxerService == nullptr ||
        mDemuxerService->GetStreamMeta(&meta, mCurrentVideoIndex, false) < 0) {
        return -EINVAL;
    }

    // 沿用上一次的解码方式（硬解/软解）
    bool bHW = (mAVDeviceManager->getVideoDecoderFlags() & DECFLAG_HW) != 0;

    // 置无效后 setUpDecoder 才会关掉旧（僵尸）解码器并新建
    mAVDeviceManager->invalidateDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);
    int ret = CreateVideoDecoder(bHW, meta);
    if (ret < 0 && bHW) {
        // 软解兜底（罕见路径：硬解重建失败时保音频不断播）
        ret = CreateVideoDecoder(false, meta);
    }

    if (ret < 0) {
        AF_LOGW("%s rebuild video decoder failed %s\n", __FUNCTION__, framework_err2_string(ret));
    } else {
        AF_LOGI("rebuildVideoDecoder dummyOnly=%d bHW=%d ret=%d\n", (int) requireDummyRender, (int) bHW, ret);
    }
    return ret;
}

int SuperMediaPlayer::RestartVideoDecoder()
{
    // 兜底路径，由 ProcessSetViewMsg 在持有 mCreateMutex 时调用。
    // 语义与参数说明见 rebuildVideoDecoder()：这里只处理 dummy/tunnel 渲染器
    // （surface 热切换失败的场景），且不得改变既有行为。
    return rebuildVideoDecoder(true);
}

int SuperMediaPlayer::RestorePausedVideoFrame()
{
    // 由 ProcessSetViewMsg 在持有 mCreateMutex 时调用（仅暂停状态）。
    // surface 重建后 ACodec 不会重绘最后一帧（ExoPlayer 2.9.6 的
    // setOutputSurface 热切换同样不重绘）。以解码器记录的"最后渲染帧
    // PTS"为渲染门（只放行这一帧，避免从关键帧开始闪帧），原地 seek
    // 解码，逐帧精确恢复暂停画面。
    if (mPlayStatus != PLAYER_PAUSED) {
        AF_LOGI("PFR: skip, status=%d\n", (int) mPlayStatus.load());
        return 0;
    }
    IDecoder *decoder = mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);
    if (decoder == nullptr) {
        AF_LOGI("PFR: skip, no video decoder\n");
        return 0;
    }
    int64_t lastPts = decoder->getLastRenderedVideoPts();
    if (lastPts == INT64_MIN) {
        AF_LOGI("PFR: skip, lastRenderedPts unknown\n");
        return 0;
    }
    int64_t frameDur = decoder->getLastRenderedVideoFrameDur();
    if (frameDur <= 0) {
        // 未知帧间隔时按 50ms 兜底：只影响 seek 目标（多读少量包），
        // 渲染门会丢弃目标帧之外的所有帧
        frameDur = 50000;
    }

    decoder->setRenderGate(lastPts);
    mRestoringPausedFrame = true;
    // seek 到"暂停帧之后一帧以内"：保证暂停帧所在包被读到，
    // 且不会多解多少包（向上取整避免毫秒截断到暂停帧之前）
    SeekTo((lastPts + frameDur + 999) / 1000, false);
    AF_LOGI("PFR: restore start, lastPts=%" PRId64 " frameDur=%" PRId64 "\n", lastPts, frameDur);
    return 0;
}


void SuperMediaPlayer::Reset()
{
    mCurrentVideoIndex = -1;
    mCurrentAudioIndex = -1;
    mCurrentSubtitleIndex = -1;
    mVideoWidth = 0;
    mVideoHeight = 0;
    mVideoRotation = 0;
    mDuration = INT64_MIN;
    mBufferPosition = 0;
    mSeekPos = INT64_MIN;
    mPlayedVideoPts = INT64_MIN;
    mPlayedAudioPts = INT64_MIN;
    mAudioClockReanchorPending = false;
    mSeekFlag = false;
    mFirstAudioPts = INT64_MIN;
    mFirstVideoPts = INT64_MIN;
    mMediaStartPts = INT64_MIN;
    mEof = false;
    mFirstBufferFlag = true;
    mBufferingFlag = false;
    mBufferingNotified = false;
    mCurVideoPts = INT64_MIN;
    mLastAudioFrameDuration = INT64_MIN;
    mTimeoutStartTime = INT64_MIN;
    mSubtitleShowIndex = 0;
    mWillChangedVideoStreamIndex = -1;
    mPendingVideoStreamIndex = -1;
    mPendingVideoPtsOffset = INT64_MIN;
    mActiveVideoPtsOffset = INT64_MIN;
    mQualitySwitchCommitPending = false;
    mQualitySwitchCommittedStreamIndex = -1;
    mQualitySwitchOldFramesPending = 0;
    mWillChangedAudioStreamIndex = -1;
    mWillChangedSubtitleStreamIndex = -1;
    mBufferIsFull = false;
    mWillSwitchVideo = false;
    mMixMode = false;
    mFirstRendered = false;
    mInited = false;
    mSeekNeedCatch = false;
    mSeekPositionFloorUs = INT64_MIN;
    mMainStreamId = -1;
    mRemovedFirstAudioPts = INT64_MIN;
    mFirstSeekStartTime = 0;
    mAudioChangedFirstPts = INT64_MIN;
    mVideoChangedFirstPts = INT64_MIN;
    mPendingVideoDecoderSwitch = false;
    mSubtitleChangedFirstPts = INT64_MIN;
    mSoughtVideoPos = INT64_MIN;
    mFirstReadPacketSucMS = 0;
    // 管线恢复的状态跟着播放器一起复位，否则上一次播放留下的时间戳/冷却
    // 会影响下一次播放的判定。
    mLastVideoFrameRenderedMs = 0;
    mVideoRecoverCooldownMs = 0;
    mRecoverSampleMs = 0;
    mRecoverSamplePackets = -1;
    mVideoDecodeRebuildCount = 0;
    mCanceled = false;
    mPNotifier->Enable(true);
    FlushSubtitleInfo();
    mSubtitleShowedQueue.clear();
    mSubPlayer = nullptr;
    mBSReadCb = nullptr;
    mBSCbArg = nullptr;
    mBSSeekCb = nullptr;
    mBSCbArg = nullptr;
    mUtil->reset();
    mDcaManager->reset();
    mVideoInterlaced = InterlacedType_UNKNOWN;
    mVideoParserTimes = 0;
    mVideoPtsRevert = mAudioPtsRevert = false;
    mHaveVideoPkt = mHaveAudioPkt = false;
    mLowMem = false;
    mCurrentVideoMeta = nullptr;
    mAdaptiveVideo = false;
    dropLateVideoFrames = false;
    mVideoCatchingUp = false;
    mBRendingStart = false;
    mSubtitleEOS = false;
    mSecretPlayBack = false;
    mDrmKeyValid = false;
    mPtsDiscontinueDelta = INT64_MIN;
    mCurrentPos = 0;
    mCurrentFrameUtcTime = -1;
    mCATimeBase = 0;
    mWATimeBase = 0;
    mSuggestedPresentationDelay = 0;
    mLiveTimeSyncType = LiveTimeSyncType::LiveTimeSyncNormal;
    mCalculateSpeedUsePacket = true;
    mUtcTimer = nullptr;
    mContainerInfo = {};
    mOpenAudioDeviceFailed = false;
}

int SuperMediaPlayer::GetCurrentStreamIndex(StreamType type)
{
    int streamIndex = -1;

    if (mMixMode && type != ST_TYPE_SUB) {
        if (HAVE_VIDEO) {
            streamIndex = GEN_STREAM_INDEX(mCurrentVideoIndex);
        } else if (HAVE_AUDIO) {
            streamIndex = GEN_STREAM_INDEX(mCurrentAudioIndex);
        }
    } else {
        switch (type) {
            case ST_TYPE_AUDIO:
                streamIndex = mCurrentAudioIndex;
                break;

            case ST_TYPE_VIDEO:
                streamIndex = mCurrentVideoIndex;
                break;

            case ST_TYPE_SUB:
                streamIndex = mCurrentSubtitleIndex;
                break;

            default:
                break;
        }
    }

    return streamIndex;
}

StreamInfo *SuperMediaPlayer::GetCurrentStreamInfo(StreamType type)
{
    int streamIndex = GetCurrentStreamIndex(type);

    if (streamIndex != -1) {
        std::deque<StreamInfo *> &streamInfoQueue = mMediaInfo.mStreamInfoQueue;
        for (StreamInfo *info : streamInfoQueue) {
            if (info->streamIndex == streamIndex) {
                return info;
            }
        }
    }

    return nullptr;
}

void SuperMediaPlayer::RenderCallback(StreamType type, bool rendered, IAFFrame::AFFrameInfo &info)
{
#if 0
    static int64_t audioPts = INT64_MIN;
    if (type == ST_TYPE_AUDIO){
        audioPts = info.pts;
    } else if (type == ST_TYPE_VIDEO){
        if (audioPts != INT64_MIN){
            AF_LOGD("avsync video late %lld (%lld - %lld) clock is %lld\n",audioPts - info.pts,audioPts, info.pts,mMasterClock.GetTime());
        }
    }
#endif
    //   AF_LOGD("video stream render pts is %lld", pts);
    if (mCanceled) {
        return;
    }

    if ((PLAYER_PREPARED != mPlayStatus) && (PLAYER_PAUSED != mPlayStatus) && (PLAYER_PLAYING != mPlayStatus)) {
        return;
    }

    MsgParam param;
    param.renderedParam.info = info;
    param.renderedParam.rendered = rendered;
    param.renderedParam.type = type;
    param.renderedParam.timeMs = af_getsteady_ms();
    putMsg(MSG_INTERNAL_RENDERED, param, false);
}

void SuperMediaPlayer::checkFirstRender()
{
    if (!mFirstRendered) {
        mFirstRendered = true;
        AF_LOGI("Player NotifyFirstFrame");
        mPNotifier->NotifyFirstFrame();
    }
}

void SuperMediaPlayer::ChangePlayerStatus(PlayerStatus newStatus)
{
    mOldPlayStatus = mPlayStatus;

    if (mPlayStatus != newStatus) {
        if (newStatus == PLAYER_ERROR) {
            if (mDataSource) {
                mDataSource->clearCache();
            }
        }
        mPNotifier->NotifyPlayerStatusChanged(mPlayStatus, newStatus);
        mPlayStatus = newStatus;
    }
}

void SuperMediaPlayer::ResetSeekStatus()
{
    mSeekPos = INT64_MIN;
    mSeekNeedCatch = false;
    /* seek 完成后重置“上屏时间戳”与采样状态：seek 本来就要重新建立解码器/时间轴，
     * 期间没有帧上屏是正常的，不能让上一个播放周期的陈旧时间戳立刻触发恢复。
     * 采样从头开始，由 doRender() 在真正播放时重新建立。 */
    mVideoCatchingUp = false;
    mLastVideoFrameRenderedMs = af_getsteady_ms();
    mRecoverSampleMs = 0;
    mRecoverSamplePackets = -1;
}

void SuperMediaPlayer::notifySeekEndCallback()
{
    mPNotifier->NotifySeekEnd(mSeekInCache);
}

bool SuperMediaPlayer::IsMute() const
{
    return mSet->bMute;
}

int SuperMediaPlayer::SetListener(const playerListener &Listener)
{
    mSet->mPlayerListener = Listener;

    if (mPNotifier) {
        mPNotifier->setListener(Listener);
    }

    return 0;
}

void SuperMediaPlayer::SetAutoPlay(bool bAutoPlay)
{
    mAutoPlay = bAutoPlay;
}

bool SuperMediaPlayer::IsAutoPlay()
{
    return mAutoPlay;
}

void SuperMediaPlayer::SetFilterConfig(const std::string &filterConfig)
{
#ifdef ENABLE_VIDEO_FILTER
    std::lock_guard<std::mutex> filterLock(mFilterManagerMutex);
    if (mFilterManager != nullptr) {
        AF_LOGW("not support change filterConfig after be set");
        return;
    }

    mFilterConfig = std::unique_ptr<CicadaJSONArray>(new CicadaJSONArray(filterConfig));
    if (mFilterConfig == nullptr || !mFilterConfig->isValid()) {
        AF_LOGD("filterConfig not JSON Array");
        return;
    }

    //TODO videoInfo is useless now.
    mFilterManager = std::unique_ptr<FilterManager>(new FilterManager(IAFFrame::videoInfo(), *mFilterConfig.get()));
    mDcaManager->createObservers();
#endif
}

void SuperMediaPlayer::UpdateFilterConfig(const std::string &target, const std::string &options)
{
#ifdef ENABLE_VIDEO_FILTER
    std::lock_guard<std::mutex> filterLock(mFilterManagerMutex);
    if (mFilterManager != nullptr) {
        mFilterManager->updateFilter(target, options);
    }
#endif
}

void SuperMediaPlayer::SetFilterInvalid(const std::string &target, bool invalid)
{
#ifdef ENABLE_VIDEO_FILTER
    std::lock_guard<std::mutex> filterLock(mFilterManagerMutex);
    if (mFilterManager != nullptr) {
        mFilterManager->setInvalid(target, invalid);
    }
#endif
}

void SuperMediaPlayer::addExtSubtitle(const char *uri)
{
    MsgParam param;
    MsgDataSourceParam dataSourceParam = {nullptr};
    dataSourceParam.url = new string(uri ? uri : "");
    param.dataSourceParam = dataSourceParam;
    putMsg(MSG_ADD_EXT_SUBTITLE, param);
}

int SuperMediaPlayer::selectExtSubtitle(int index, bool bSelect)
{
    if (!(index & EXT_STREAM_BASE)) {
        AF_LOGE("select ext subtitle error\n");
        mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_SUBTITLE_SELECT_ERROR, "Not a ext stream");
        return -1;
    }

    MsgParam param;
    MsgSelectExtSubtitleParam track = {index, bSelect};
    param.msgSelectExtSubtitleParam = track;
    putMsg(MSG_SELECT_EXT_SUBTITLE, param);
    return 0;
}

int SuperMediaPlayer::setStreamDelay(int index, int64_t time)
{
    // FIXME : delay all video here now
    if ((HAVE_VIDEO && (index == mCurrentVideoIndex)) || index == -1) {
        mVideoDelayTime = time * 1000;
        return 0;
    }
    if (!(index & EXT_STREAM_BASE) || !mSubPlayer) {
        AF_LOGE("setStreamDelay support ext subtitle only for now\n");
        return -ENOSYS;
    }
    if (mSubPlayer) {
        mSubPlayer->setDelayTime(index, time * 1000);
        mSubPlayer->seek(std::max(getCurrentPosition() + time * 1000, (int64_t) 0));
    }
    return 0;
}

void SuperMediaPlayer::startRendering(bool start)
{
    if (start == mBRendingStart) {
        return;
    }

    mBRendingStart = start;

    if (start) {
        mMasterClock.start();
    } else {
        mMasterClock.pause();
    }
    mAVDeviceManager->pauseAudioRender(!start);
}

void SuperMediaPlayer::SetOnRenderCallBack(onRenderFrame cb, void *userData)
{
    mFrameCb = cb;
    mFrameCbUserData = userData;
}

void SuperMediaPlayer::SetAudioRenderingCallBack(onRenderFrame cb, void *userData)
{
    mAudioRenderingCb = cb;
    mAudioRenderingCbUserData = userData;
}

void SuperMediaPlayer::SetVideoRenderingCallBack(videoRenderingFrameCB cb, void *userData)
{
    mVideoRenderingCb = cb;
    mVideoRenderingCbUserData = userData;
}

void SuperMediaPlayer::SetUpdateViewCB(UpdateViewCB cb, void *userData)
{
    mUpdateViewCB = cb;
    mUpdateViewCBUserData = userData;
}

void SuperMediaPlayer::SetUrlHashCB(UrlHashCB cb, void *userData)
{
    mUrlHashCb = cb;
    mUrlHashCbUserData = userData;
}

int SuperMediaPlayer::invokeComponent(std::string content)
{
    return mDcaManager->invoke(content);
}

void SuperMediaPlayer::setDrmRequestCallback(const std::function<DrmResponseData *(const DrmRequestParam &drmRequestParam)> &drmCallback)
{
    mAVDeviceManager->setDrmRequestCallback(drmCallback);
}

void SuperMediaPlayer::ProcessUpdateView()
{

    if (mCurrentVideoIndex < 0) {
        return;
    }

    int videoTag = VideoTag::VIDEO_TAG_NONE;

    updateVideoMeta();
    auto *meta = (Stream_meta *) (mCurrentVideoMeta.get());

    bool isHDR = isHDRVideo(meta);
    if (isHDR) {
        videoTag |= VideoTag::VIDEO_TAG_HDR10;
    }

#ifdef ANDROID
    bool isWideVine = isWideVineVideo(meta);
    if (isWideVine) {
        //TODO set widevine level  by user
        videoTag |= VideoTag::VIDEO_TAG_WIDEVINE_L1;
    }
    if (mUpdateViewCB != nullptr) {
        bool update = mUpdateViewCB(videoTag, mUpdateViewCBUserData);
        if (update) {
            //do not use old view to create decoder
            mSet->mView = nullptr;
        }
    }
#endif
}

bool SuperMediaPlayer::isWideVineVideo(const Stream_meta *meta)
{
    bool isWideVineVideo = (meta->keyFormat != nullptr && strcmp(meta->keyFormat, "urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed") == 0);
    return isWideVineVideo;
}

bool SuperMediaPlayer::isHDRVideo(const Stream_meta *meta)
{
    bool isHDRVideo = false;

    if (meta->pixel_fmt == AF_PIX_FMT_YUV420P10BE || meta->pixel_fmt == AF_PIX_FMT_YUV420P10LE) {
        AF_LOGD("HDR video\n");
        isHDRVideo = true;
    }
    return isHDRVideo;
}

float SuperMediaPlayer::getCurrentDownloadSpeed()
{
    return mUtil->getCurrentDownloadSpeed();
}

void SuperMediaPlayer::ApsaraAudioRenderCallback::onFrameInfoUpdate(IAFFrame::AFFrameInfo &info, bool rendered)
{
    mPlayer.RenderCallback(ST_TYPE_AUDIO, rendered, info);
}

void SuperMediaPlayer::ApsaraVideoRenderListener::onFrameInfoUpdate(IAFFrame::AFFrameInfo &info, bool rendered)
{
    mPlayer.RenderCallback(ST_TYPE_VIDEO, rendered, info);
}

bool SuperMediaPlayer::ApsaraVideoProcessTextureCallback::init(int type)
{
#ifdef ENABLE_VIDEO_FILTER
    std::lock_guard<std::mutex> filterLock(mPlayer.mFilterManagerMutex);
    if (mPlayer.mFilterManager) {
        return mPlayer.mFilterManager->initFilter(IVideoFilter::Texture, type);
    }
#endif
        return false;

}

bool SuperMediaPlayer::ApsaraVideoProcessTextureCallback::needProcess()
{
#ifdef ENABLE_VIDEO_FILTER
    std::lock_guard<std::mutex> filterLock(mPlayer.mFilterManagerMutex);
    if (mPlayer.mFilterManager) {
        return mPlayer.mFilterManager->hasFilter(IVideoFilter::Texture, "");
    }
#endif
        return false;

}

bool SuperMediaPlayer::ApsaraVideoProcessTextureCallback::push(std::unique_ptr<IAFFrame> &textureFrame)
{
    return mPlayer.push(textureFrame);
}

bool SuperMediaPlayer::ApsaraVideoProcessTextureCallback::pull(std::unique_ptr<IAFFrame> &textureFrame)
{
    return mPlayer.pull(AF_PIX_FMT_CICADA_TEXTURE, textureFrame);
}
