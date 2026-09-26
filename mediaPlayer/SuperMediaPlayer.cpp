#define LOG_TAG "ApsaraPlayerService"

#include "SuperMediaPlayer.h"
#include "media_player_error_def.h"
#include "media_player_error_map.h"
#include "utils/CicadaJSON.h"
#include "utils/CicadaUtils.h"
#include "utils/UrlUtils.h"
#include <cassert>
#include <cinttypes>
#include <cstdlib>
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
/*
 * 【第七项：死代码清理】原 VIDEO_RECOVER_STALL_MS / VIDEO_RECOVER_SAMPLE_MS /
 * VIDEO_RECOVER_COOLDOWN_MS 三个宏已删除：它们是"多久没上屏就判死并重建解码器"
 * 那套墙钟巡检的参数，而那套机制已按红线整体删除（见 doRender() 里那段历史说明），
 * 四个宏在代码里已无任何引用。当前"有输入却零输出"的救援是纯状态判据，
 * 见 DECODE_STALL_REBUILD_ROUNDS。
 */

/*
 * 【第七项：死代码清理】原 VIDEO_RECOVER_STUCK_SEEK_MS 已删除：它是上面那套墙钟
 * 巡检在 seek 期间的宽限值，随机制一起删除，代码里已无引用。
 * seek 期间解码器卡死现在由 DECODE_STALL_REBUILD_ROUNDS 那条状态判据负责恢复
 * （判据里显式包含"seek 在途"这一支），不再需要任何"多等一会儿"的时间宽限。
 */

/*
 * 本轮已删除"seek 期间掐住音频"的墙钟上限（原 SEEK_CATCH_AUDIO_UNBLOCK_MS）。
 * 音频不再被 mSeekNeedCatch 掐住（见 render() 里的说明），这条上限失去存在理由。
 * 红线：禁止看门狗 / 超时兜底 —— 终态只能由事件产生，超时只允许用于观测与日志。
 */

/*
 * 本轮已删除两条 seek 的墙钟阈值（原 SEEK_NO_FRAME_REBUILD_MS=3000 / SEEK_INPUT_STARVED_REBUILD_MS=900）
 * 以及它们驱动的"没出帧就重建解码器"整块逻辑。
 * 理由：根因（渲染地板要求解完整段 GOP 前缀）已由 K1 落点帧即上屏修掉；4K 下一次重建要重配
 * 约 250MB 缓冲池；主流实现（mpv/ffplay）的解码器恢复是错误驱动，不是时间驱动。
 * 红线：禁止看门狗 / 超时兜底 —— 超时只能用于日志与诊断。
 */

/*
 * 提交前墙钟死线（毫秒）：只要有任何在途清晰度切换，到这个点必须有终态。
 *
 * 6000ms 的依据：
 *   * 正常路径根本用不到它 —— 播放中切档实测 200~700ms 完成；
 *   * 暂停态切档要等一个 GOP 的预滚（本片源 DASH 10s 分片内部的 IDR 间隔
 *     2~4s），6000ms 覆盖两个 IDR；
 *   * 而外层预编译 PlayerBase 的 HSM 只给 ~545ms（它由 seek 期间的
 *     baseStart() 武装）——注意这条死线管的是**切档**，切档本身不会像 seek
 *     那样触发 baseStart()，所以 6s 是安全的；真正危险的是"状态机永不收敛"
 *     导致后续任何操作都撞在同一个卡死状态上。
 * 到点即 finishQualitySwitch(false, ...)，UI 高亮退回旧档，且能立刻再切一次。
 */
#define QUALITY_SWITCH_PREROLL_DEADLINE_MS (6000)

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
/*
 * 【第七项：死代码清理 —— 这两个 define **故意保留**】
 * 消费它们的两个函数 checkQualitySwitchPrerollDeadline() / checkQualitySwitchDeadline()
 * 已确认无调用点，并已用 `#if 0` 整体停用（见那两处）。但宏本身**不能删**，因为它们
 * 仍被活跃代码引用：
 *   · QUALITY_SWITCH_PREROLL_DEADLINE_MS —— beginQualitySwitchTracking() 里给
 *     mQualitySwitchPrerollDeadlineMs 赋初值时引用（:2877 一带的活跃代码）；
 *   · QUALITY_SWITCH_DEADLINE_MS —— 切档提交处 mQualitySwitchDeadlineMs = commit + 该值
 *     （活跃代码）。
 * 这两个字段在函数停用后已无人读取，属于"只写不读"，但写入本身是既有行为，
 * 删宏会连带改动活跃行 —— 不属于"零行为变化"，故保留并在此注明。
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
 * 音频**已解码 PCM 帧**队列的下限（帧数）。
 * 取 9 与主流一致（ffplay 的 SAMPLE_QUEUE_SIZE 就是 9）：约 190ms 的抗抖动余量。
 * 低于这个量，4K 解码突发 / 网络抖动会让 AudioTrack 断供（听感是"断一下/咔哒"）。
 */
#define AUDIO_FRAME_QUEUE_FLOOR (9)

/*
 * S2：「视频路手里什么都没有」需要连续多少轮主循环才认定为"卡住"。
 * 判据是**状态 + 迭代计数**（帧队列空 + 视频包队列空 + 解码器输入队列空），
 * 不是墙钟 —— 连续几轮只用来滤掉"两帧之间瞬时为空"，不是看门狗。
 */
#define VIDEO_STARVE_ITERS (3)
/*
 * 解码器"拒收输入 + 零输出"这个状态签名需要连续成立多少轮管线工作循环，才认定 codec
 * 是真的咬住了缓冲（而不是刚起解码器、首帧还没解出来）。
 *   · 计价单位是**轮数**，不是时间：计数器只在上面那个状态签名成立时才 +1，
 *     签名一破立刻归零，所以它不随时间流逝而增长，也没有任何周期性动作；
 *   · 每次卡死最多触发一次重建（mDecodeStallRebuildDone 闩住），代价有上限；
 *   · 出帧即自愈：FillVideoFrame() 收到帧时把闩、轮数、签名与重建预算一起复位，
 *     所以它是可用且不重复的（这就是不用计时器的"冷却"）；
 *   · 4K 下一次重建要释放并重配约 250MB 输出缓冲池，所以轮数不能太小：
 *     取 100 —— 远大于"刚起解码器需要几轮"的量级，又能在用户察觉前收敛。
 */
#define DECODE_STALL_REBUILD_ROUNDS (100)
/*
 * "音频静音窗口"的**状态**编码，只用于起止各一条的日志（不参与任何管线决策）。
 * 与 framework/render/audio/Android/AudioTrackRender.cpp 里同名编码保持一致，
 * 这样一份日志就能直接判定静音是"内核在等时钟"还是"设备侧欠载/被重建"。
 *   0 = 有声；1 = seek 窗口内等主时钟追上（设计内）；2 = 设备写失败/短写；
 *   3 = 音频设备被 flush / 重建，等数据重新灌满。
 */
#define AUDIO_SILENCE_NONE (0)
#define AUDIO_SILENCE_SEEK_CLOCK (1)
#define AUDIO_SILENCE_DEVICE_WRITE (2)
#define AUDIO_SILENCE_DEVICE_RESTART (3)
/*
 * 4 = 缓冲态（空缓存）：主时钟与音频渲染被一起暂停，声音因此停止；缓冲出口恢复。
 * 加上这一档是为了让下一份日志能直接区分"偶发静音"到底来自 1（等落点）、
 * 2/3（设备侧欠载/被重建）还是 4（缓冲态）—— 这正是上一轮缺的那条证据。
 */
#define AUDIO_SILENCE_BUFFERING (4)

/*
 * “渲染器追赶窗口”已改为**事件驱动**（本轮）：不再有"窗口多久"这个时间上限。
 *
 * 场景：清晰度切换提交时、或 seek 之后，新路的首帧可能比主时钟旧 1~2 秒。
 * 这时若"每帧都渲染"，视频会永久落后；若"跳到下一个关键帧"，画面会被扔到时钟前面。
 *
 * 做法（与主流一致）：丢掉迟到的帧，**直到出现一帧落在时钟上为止** ——
 * 由 RenderVideo 在"有准时帧上屏"这一事件上把 dropLateVideoFrames 清回 false。
 * 不再用"3 秒到点就撤"的墙钟（那是看门狗同类物）。
 */

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
 * 【第 2 项：有界精确落点】落点关键帧离 seek 目标不超过这个距离时，才要求"精确到目标帧"
 * （即落点帧必须是离目标 <= 该预算的那一帧），超出预算就照旧立刻上屏落点关键帧。
 *
 * 为什么必须是**有界**的：精确落点本身不增加解码量（解码器无论如何都要从落点关键帧
 * 解到目标），代价全在"首帧上屏被推迟到解码器走到目标附近"。Qt 实测该文件 IDR 极稀疏
 * （落点比目标早 1.0~11.2 秒），若无条件精确，用户要黑屏等几秒甚至十几秒 —— 那会
 * 直接毁掉已经达标的 seek→落点 79~103ms 基线。取 200ms —— 与 RenderVideo 里既有的
 * "目标点前 200ms 之内即视为到位"（SEEK_TARGET_DROP_AHEAD_US）同值，语义统一：
 * 进入目标前 200ms 就算"精确到目标"。代价上界只有约 200ms 的内容（25fps 下 ≤5 帧）。
 * 实测那 8 次 seek 的落点距离是 1.0/1.1/1.4/2.3/5.9/7.2/11.2/11.2 秒，全部大于该预算，
 * 所以这条策略对已测基线**零影响**（可静态论证）。
 */
#define SEEK_EXACT_LANDING_BUDGET_US (200 * 1000)

/*
 * 【第 4 项】追赶窗口打开时，"已经注定要被丢掉"的帧在我们自己队列里的判定距离（微秒）。
 * 与 RenderVideo 里既有的迟到阈值（videoLateUs >= 500ms 不渲染）保持一致，
 * 这样被提前清掉的帧与原本会被丢掉的帧是同一批 —— 显示结果不变。
 */
#define JOINING_STALE_FRAME_US (500 * 1000)

/*
 * 【第 4 条】落点对齐期间的"音频连续性"容差（微秒）。
 *
 * 为什么需要：1b 清掉的是**上层**已解码帧队列（mAudioFrameQue），但音频解码器
 * **内部**还压着旧时间轴的数据（ActiveDecoder 输入队列 ≤16 包 + 输出队列 ≤10 帧，
 * 约 0.5s 内容），它们会在对齐之后才被解出来、并按旧 PTS 到达 —— 只判"低于落点就丢"
 * 挡不住它们（它们的 PTS 比落点**高** 8.8~11.9s；实测落点 20.833s 却报出 29.674s，
 * 随即 TIMEPOS reSync 与约 230 帧连续丢帧；安卓上表现为音频把主时钟拽走）。
 *
 * 判据：新时间轴的音频从落点开始**连续**到达（实测包队首 = 落点 + ≤21ms），
 * 所以"高出高水位超过本容差"的帧必是旧时间轴残留。
 * 取 200ms：正常音频帧间隔（AAC 1024@48k≈21ms，2048@44.1k≈46ms）远小于它，
 * 旧时间轴的跳变（秒级）远大于它 —— 两者不可能混淆。
 * 只在对齐窗口内生效（首帧落点音频上屏即撤除），纯状态判断，无时间阈值。
 */
#define SEEK_AUDIO_CONTINUITY_TOLERANCE_US (200 * 1000)

/*
 * 【第 4 条的安全上界】对齐期间"高出高水位"的帧最多丢多少帧就必须放行。
 * 旧时间轴残留是可数的（解码器输入队列 ≤16 包 + 输出队列 ≤10 帧，约 26 帧），
 * 所以取 32 —— 足够清掉全部残留，又保证万一新时间轴在这段窗口里本身有一个
 * >200ms 的正常空隙（编辑列表/丢包），也绝不会把音频饿死成"对齐窗口永不关闭"。
 * 纯计数，不是时间判据，也不是周期性动作。
 */
#define SEEK_AUDIO_STALE_DROP_MAX (32)

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
 * 切换在途时，doReadPacket() **一次调用**最多为目标路读多少个包。
 *
 * 原来是写死的 16：额度一用完 warmingPending 立刻翻假，读循环马上重新受
 * "当前路缓冲已满"那两条判断的约束而停读 —— 而稳态播放时当前路的缓冲本来就是满的，
 * 于是目标档的数据只能跟着**播放节奏**一点点进来：实测预热正好 1× 实时
 * （切换 1.09 s，其间 pending 只前进了约 1.0 s 的内容）。
 *
 * 现在放开到"一次调用足够填满目标队列"（= 它的硬上限）。内存仍然有界：
 * 路由处按 PENDING_VIDEO_QUEUE_HARD_CAP 丢弃、循环顶部按 PENDING_VIDEO_QUEUE_CAP
 * 在当前路吃得住时停读（见 doReadPacket 里那两处），所以这里放开的是
 * **每次调用的读取工作量**，不是队列长度。当前路快见底时那两处会让路，
 * 不会把旧画面饿死；读满一屏目标包后本函数就返回，主循环照样去解码/渲染。
 */
#define PENDING_READ_BURST_PER_CALL PENDING_VIDEO_QUEUE_HARD_CAP

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
 * 【B16】"追赶不收敛 ⇒ 一帧都不送"的事件计数出口（无计时器）。
 *
 * 判据（全是状态）：播放态下，渲染判定**连续**给出"不上屏"，而且这些被拒帧与主时钟的
 * 距离 |master - videoPts| **一直没有变小**（说明两条轴在恒速同向前进、偏移恒定 ——
 * 也就是"追赶不收敛"：解码在前进，差距不收敛，于是按原规则每一帧都会被拒）。
 * 一旦连续达到这个帧数，就**强制把当前帧放上屏一次**，让画面随解码前进，而不是
 * 永远停在 seek 之前那张旧画面上（安卓日志里的形态：3.09s/246 条 drop、偏移恒定 ~3s、
 * 期间 0 次上屏、0 条落点采纳 ⇒ 画面冻死）。
 *
 * 为什么用"帧数"而不是时间：帧数是**事件计数**（连续被拒了几帧），不引入计时器、不看墙钟；
 * 它与 ExoPlayer 里 shouldForceRenderOutputBuffer 的 100ms 护栏同义（那边用时间，
 * 这里用连续事件数），量级也刻意对齐本文件既有的 JOINING_FORCE_RENDER_MS = 100ms。
 *
 * 为什么不会破坏精度：这条只在"判定已经说不上屏"之后把 render 翻成 true，
 *   · 不影响 seek 落点判据（落点采纳在同一帧里已经先跑完；采纳了就轮不到这里）；
 *   · 不动主时钟、不动位置上报、不动任何锚点闩；
 *   · 一帧上屏后计数清零 ⇒ 不会是"每帧都放行"，节拍/追赶逻辑照旧。
 */
#define VIDEO_STUCK_DISCARD_STREAK_MAX 8
/*
 * 【B16-b】追赶（dropLateVideoFrames）期内的"永不冻死"阀门。
 *
 * 为什么单独要一条：安卓日志（`drop frame,master played time is 240649463,video pts is
 * 239889650` 等）里追赶期连续丢了几百帧，而偏移只在**缓慢**收敛（~2~3 ms/帧）——
 * 上面那条"偏移不再变小"的判据因此**不会**触发（偏移确实一直在变小，只是太慢），
 * 用户仍然看到数秒冻住。所以追赶期内改成只看**连续被拒帧数**：连续被拒达到这个帧数
 * 就强制放行一帧（宁可极短暂的慢放，也不冻住）。
 *
 * 为什么"帧数"不是计时器：它是**事件计数**（连续被拒了几帧），不读墙钟、不设时间阈值。
 * 20 帧 ≈ 0.33s@60fps ≈ 0.8s@25fps，量级与 ExoPlayer 的 100ms 强制出画护栏同义。
 *
 * 为什么只在"非 seek 落点窗口"生效（mSeekRenderGateUs == INT64_MIN）：
 *   · seek 前缀（落点尚未采纳）期间由 B15 负责"先出画一张"，落点帧到达后照旧替换，
 *     那条路**必须**保持"前缀帧基本不上屏"，否则就成了"快进扫一遍"；
 *   · 追赶期（本阀门）与 seek 落点窗口是**互斥**状态，分开口径互不干扰。
 */
#define VIDEO_CATCHUP_DISCARD_STREAK_MAX 20

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
        /*
         * 【错误回调】原来这里只打一条 AF_LOGE 就静默 return，App 完全不知道
         * setDataSource(JSON) 失败（表现是"点了没反应"）。
         * 现在复用既有 error 通知通道上报一条明确错误：
         *   码   = MEDIA_PLAYER_ERROR_DEMUXER_MANIFEST_PARSE（ErrorCodeMap 映射 0x20030007）
         *   文案 = "MediaManifest JSON parse failed: <解析器给出的原因>"
         * 解析失败时不进入任何播放状态、不改动任何既有字段，对既有流程零影响；
         * 调用方可据该码/文案提示"清单 JSON 不合法"（见 docs/MANIFEST-OBJECT-GUIDE.md）。
         */
        const std::string notifyMsg = "MediaManifest JSON parse failed: " + error;
        mPNotifier->NotifyError(MEDIA_PLAYER_ERROR_DEMUXER_MANIFEST_PARSE, notifyMsg.c_str());
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
    /*
     * 渲染闸门与"只锚一次"的闩在 putMsg 之前就置好：消息处理线程可能立刻开始跑
     * ProcessSeekToMsg，晚置会出现"闸门还是上一次 seek 的值"的竞态。
     * 目标点直接用入参算（不要读 mSeekPos，它在 putMsg 之后才赋值）。
     */
    mSeekRenderGateUs = (int64_t) pos * 1000;
    mSeekLandingFrameAccepted = false;
    mSeekClockAnchored = false;
    mSeekDecodeStartIsKey = false;
    /* B15：本次 seek 的"先出画"闩，与 mSeekDecodeStartIsKey 同点复位（每次 seek 只先出一帧）。 */
    mSeekFirstDecodableFrameShown = false;
    /* B16：本帧连续拒帧计数与偏移采样随 seek 一起重新起算。 */
    mVideoDiscardStreak = 0;
    mVideoDiscardGapAbsUs = INT64_MIN;
    mCatchUpDiscardStreak = 0;
    mSeekVideoAnchorDone = false;
    mSeekAudioAlignDone = false;
    /*
     * 【修法 2】锚点事件**不在这里置位**，只在这里清零。
     *
     * 原来在 SeekTo()（= API 线程、putMsg 之前）就置真，于是 SeekTo 到
     * ProcessSeekToMsg 之间渲染出来的**旧时间轴帧**会把事件消费掉：
     * 实测安卓 18:52:01.053 `seek anchor: … (pts=1751750, target=52085000)` 比
     * 18:52:01.079 的 `PFR: seek posUs=52085000` 早 26ms —— 锚到了旧帧的 1.75s。
     * 那一次是音频交接 + TIMEPOS reSync 把时钟救了回来；在**没有音频轨 / 音频不交接**
     * 的场景下时钟就会停在那个旧值上（1.75s vs 目标 52s），seek 后内容被
     * 判成"迟到"全丢。
     *
     * 现在置位点移到 ProcessSeekToMsg()（seek 真正开始、mSeekFlag 置真的同一处），
     * 因此只有"seek 之后渲染的帧"才可能消费这个事件。
     */
    mSeekAnchorPending = false;
    /* A 方案的事件闩也必须在 putMsg 前清干净：不让上一次 seek 的定位事件影响这一次。 */
    mSeekAudioRepositionPending = false;
    mSeekAudioRepositionUs = INT64_MIN;
    mSeekAudioRepositionJumpUs = INT64_MIN;
    mVideoDecodeRetrySeen = false;
    mDecodeStallIters = 0;
    mDecodeStallRebuildDone = false;
    mSeekAudioFloorUs = INT64_MIN;
    mSeekAudioContinuityUs = INT64_MIN;
    mSeekAudioStaleDrops = 0;
    mSeekExactLandingByBudget = false;
    /*
     * B2：pending 解码器的"占位 Surface"身份是 pending 路由的一部分，seek 会把
     * pending 路整体丢掉重建（见 FlushVideoPath 里的 discardPendingVideoDecoder），
     * 所以这里必须随其它 pending/seek 状态一起复位。
     */
    mPendingDecoderUsesPlaceholderSurface = false;

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
    /* 【② B18】归属标记与地板**同点、同值**写入（见 SuperMediaPlayer.h 的说明）。 */
    mSeekLandingFloorOwnerUs = mSeekPositionFloorUs;
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
    /*
     * 【① B17 硬要求 B】用户又发起了新的切档请求 ⇒ 之前那个"等 seek 结束再重新装弹"的
     * 意图立即作废：新的请求本身就是用户的最新意图，直接走正常流程即可（不会重装旧档）。
     * 本函数的两个调用者（公开 API / 重装路径）都走这里，所以这里清一次就够。
     */
    mSwitchReArmPending = false;
    mSwitchReArmStreamIndex = -1;

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
    } else if (theKey == "seekExactLanding") {
        /*
         * seek 落点策略：1 = 旧语义（必须精确到目标帧，目标点之前的帧一律不上屏）；
         * 0（默认）= 落点同步帧即上屏（K1）。各端都用既有 setOption 切换。
         */
        mSeekExactLanding = (atoi(value) != 0);
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
     *
     * 【地板必须会自己作废 —— 循环播放/重开源的归零修复】上面那条只在“seek 在飞”期间成立。
     * 一旦播放从**更前面**重新开始（循环播放重开缓存源、Stop+Prepare、大跨度回退 seek），
     * 管道位置会明显低于地板，此时地板已经过期：旧代码只在落点分支里做“放弃”判断
     * （:7317 那条），重开源的路径根本走不到那里 ⇒ getCurrentPosition() 被永久钉在旧目标点上，
     * 表现就是“循环播放从头开始，进度条却停在上一次 seek 的点（末尾）不归零”。
     * 这里补一条**纯状态判断**的自愈：管道位置比地板低出一个分片量级（SEEK_FLOOR_GIVEUP_US，
     * 远大于任何落点回看距离）就作废地板，按真实位置报。没有计时器，不影响正常 seek 的防回退。
     */
    if (mSeekPositionFloorUs != INT64_MIN && mCurrentPos.load() < mSeekPositionFloorUs) {
        if (mCurrentPos.load() + SEEK_FLOOR_GIVEUP_US < mSeekPositionFloorUs) {
            mSeekPositionFloorUs = INT64_MIN;
            return mCurrentPos;
        }
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

/*
 * 【色觉辅助滤镜 / 回退点 C7】照 SetMirrorMode 的写法：存配置 + putMsg 通知渲染器线程。
 * 与 SetMirrorMode 的唯一区别：矩阵逐元素比较（镜像是个枚举，可以直接 ==）。
 * 单位矩阵 = 关闭（见 player_types.h 里 colorMatrix 的默认值）。
 */
void SuperMediaPlayer::SetColorMatrix(const float matrix[9])
{
    if (matrix == nullptr) {
        return;
    }

    for (int i = 0; i < 9; i++) {
        if (mSet->colorMatrix[i] != matrix[i]) {
            for (int j = 0; j < 9; j++) {
                mSet->colorMatrix[j] = matrix[j];
            }
            this->putMsg(MSG_SET_COLOR_MATRIX, dummyMsg);
            return;
        }
    }
}

ScaleMode SuperMediaPlayer::GetScaleMode()
{
    return mSet->scaleMode;
}

int64_t SuperMediaPlayer::GetBufferPosition()
{
    /*
     * 【2026-09-26 修：seek 之后缓冲条"先往回缩、再弹回来"】
     *
     * mBufferPosition 只是一份**上报缓存**，它只在 PostBufferPositionMsg() 里被写。
     * 而那个函数在 seek 在途时（isSeeking()）是**直接 return 不发布**的（见那里的说明），
     * 于是整个 seek 期间这里返回的都是**上一个播放周期**写下的旧值：
     *   · 向前 seek：旧值（例如 30s）远小于 seek 目标（例如 90s），
     *     而播放头这时已经被 mSeekPositionFloorUs 钉在 90s ——
     *     界面拿到的是"缓冲条末端落在播放头后面"，
     *   · 分片流 seek 会把整段缓存清掉再从目标重下（见 SMPMessageControllerListener
     *     里 seek 分支的说明），旧值本来也不再代表任何真实缓冲。
     * 用户描述成"seek 后 buffer 往后缩再弹回来"，说的就是这段窗口：
     * 先拿到旧的小值（缩），等 seek 结束、真实缓冲时长能测出来以后再跳出去（弹）。
     *
     * 这里给上报值加一条**纯状态**下限：缓冲条末端不可能早于播放头。
     * seek 期间 getCurrentPosition() 返回的就是 seek 目标（地板钉住的），
     * 所以这条下限正好等价于"seek 窗口内缓冲位置以目标为下限"。
     * getCurrentPosition() 自己带地板过期自愈（循环播放/重开源的场景），
     * 所以这里不会用一个已经过期的目标点把缓冲条顶到旧位置上去。
     * 无计时器、不新增成员、不在 seek 之外改变任何取值（非 seek 时
     * mBufferPosition 恒不小于当前位置，这一条是空操作）。
     */
    int64_t bufferPosition = mBufferPosition;
    const int64_t currentPosition = getCurrentPosition();

    if (bufferPosition < currentPosition) {
        bufferPosition = currentPosition;
    }

    if (mDuration > 0 && bufferPosition > mDuration) {
        bufferPosition = mDuration;
    }

    return bufferPosition / 1000;
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
        /*
         * ============ 【★ seek 窗口内必须全速前推 —— "seek 后立马继续播放"的来源 ★】============
         *
         * 原判据是 `(mVideoCatchingUp || mSeekFlag) && getPlayerBufferDuration(...) > 0`。
         * 而 seek 刚发起时缓冲**正是被清空的**（就是 0），于是这条"快跑"在最需要它的时刻
         * 不成立：主循环退回 loopGap（25fps 时约 26ms 一拍），而"解码前推到目标"是**一帧一拍**
         * 地往前挪的 —— 落点比目标早 11 秒（≈300 帧）时，光前推就要 6~7 秒，
         * 用户看到的就是"seek 之后卡一下才动"。精确落点判据（包含目标）是对的，卡在前推速度上。
         *
         * seek 窗口期间（`mSeekFlag` 在途，或落点帧尚未采纳即 mSeekRenderGateUs 仍置位）
         * 不再看缓冲：持续 2ms 一拍把包喂给解码器，直到"包含目标的那一帧"进入帧队列并被采纳。
         * 这是**状态判据**（seek 还在不在进行），不引入计时器；窗口一结束
         * （采纳时 mSeekRenderGateUs 归 INT64_MIN）自动回到正常节拍，不会长期快跑。
         * 追赶（mVideoCatchingUp）那条保留原样，仍要求"缓冲 > 0"。
         * ==================================================================================
         */
        const bool inSeekWindow = (mSeekFlag || mSeekRenderGateUs != INT64_MIN);

        /*
         * ============ 【★ seek 之后"刚开始一直卡着"的来源之一：追赶期被"缓冲>0"挡住 ★】============
         *
         * 原判据 `(mVideoCatchingUp || mSeekFlag) && getPlayerBufferDuration(...) > 0`：
         * **seek 一完成、缓冲还空着**的时候（缓冲=0）快跑不成立，主循环退回 loopGap（约 26ms 一拍），
         * 而这个阶段恰恰是"解码器刚被 flush、帧队列空、要把重新解码出来的帧尽快送上去"的时候 ——
         * 一拍一帧地挪 ⇒ 用户看到的就是"seek 之后刚开始一直卡着，过一会儿才动"。
         *
         * 而 getPlayerBufferDuration() 在**帧队列空**时会返回 0，正是它把最需要快跑的阶段排除掉了。
         * 所以这里对"追赶中"（mVideoCatchingUp：每丢一帧置真、一旦真有帧上屏立刻清零）**不再要求缓冲 > 0**。
         * 这不是自旋：needWait 仍有 2ms 下限（下面那段），而且 mVideoCatchingUp 一旦有帧上屏就归假，
         * 快跑窗口是**状态**定义的、有界的，不引入任何计时器。
         * ==================================================================================
         */
        if (inSeekWindow || mVideoCatchingUp) {
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
    /*
     * 这里原来每轮主循环都调两条"墙钟死线"：
     *   checkQualitySwitchDeadline()        —— 提交后 500ms 内没等到新帧上屏就判 FAILED；
     *   checkQualitySwitchPrerollDeadline() —— 提交前 6000ms 还没提交就判 FAILED。
     * 本轮按红线（禁止看门狗 / 超时兜底）**删除调用**：
     * 切档的终态一律由事件产生，而事件出口本来就是齐全的 ——
     *   · 成功：目标路的帧真的上屏（RenderVideo 里 finishQualitySwitch(true)）；
     *   · 失败/取消：seek、stop、换源、flush 视频路（FlushVideoPath 里
     *     finishQualitySwitch(false)）以及解码器错误路径。
     * 两条死线只会在"事件永远不来"时救场，而那种情况本身就该由错误路径收掉，
     * 不是靠时间猜。
     */
    logSeekPipelineState("loop");

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

    /*
     * ============ A 方案消费点：音频流重新定位到落点 —— 结论：接口不支持，安全回退 ============
     *
     * 这里是**消息线程**（ProcessVideoLoop），也是唯一允许调解复用器的地方。
     * 事件由 renderAudioFrame() 置闩（"音频时间轴在 seek 之后前跳 > 1s"）。
     *
     * 逐文件核对后的 API 事实（这是"能不能按流重定位"的结论，别再猜）：
     *   1. 单文件容器 avFormatDemuxer::Seek (framework/demuxer/avFormatDemuxer.cpp:577-588)
     *      **完全忽略 index**：走 avformat_seek_file(mCtx, **-1**, …) —— 定位整个容器、
     *      清空解复用包队列、暂停并重启读线程。传音频下标 = 对整个容器再 seek 一次，
     *      视频包会被重新投递（PTS 倒退/重复）⇒ **会破坏视频读取**，红线不允许。
     *   2. HLS/DASH 只在 index 命中**字幕流**时按流 seek
     *      （play_list/HLSManager.cpp:465-477）；音频下标会落到 "seek all" 分支，
     *      而那个分支 (HLSManager.cpp:497-535) 本身就是"**先 seek 视频，再用视频落地
     *      位置去 seek 其余流**" —— 也就是说 HLS/DASH 早已把音频对齐到视频落点，
     *      根本不需要我们再定位一次（再调一次等于又做一次整体 seek，有害无益）。
     *   3. 因此"按流重定位"在当前接口下不可实现。要真正消除单文件容器上
     *      "视频从关键帧起步、音频从目标附近起步"的差，必须扩展解复用 API
     *      （新增按流 re-position），那是单独一轮的接口改动，不在本次范围。
     *
     * 所以这里只做一件事：把事实记成一条限频日志（每次事件一条，不是周期日志），
     * 然后**保持既有防线**（1b 清已解码帧队列 + 重基时间轴；双侧地板 + 32 帧上限）——
     * 它们在定位不可用时仍然把旧时间轴残留挡在设备之外。
     */
    if (mSeekAudioRepositionPending) {
        const int64_t repositionLandingUs = mSeekAudioRepositionUs;
        const int64_t repositionJumpUs = mSeekAudioRepositionJumpUs;
        const int audioIndex = mCurrentAudioIndex;

        mSeekAudioRepositionPending = false;
        mSeekAudioRepositionUs = INT64_MIN;
        mSeekAudioRepositionJumpUs = INT64_MIN;

        /*
         * 消息线程是唯一允许调解复用器的地方（读包/渲染路径只置闩）。
         * 接口语义见 framework/demuxer/IDemuxer.h 的 SeekStream()：只允许"只改 index
         * 指定流的投递位置，不动共享容器位置、不动其它流队列、不重启读线程"。
         * 任何做不到该语义的实现都必须返回负值 —— 默认实现就是 -1，所以当前所有
         * 解复用器（含单文件容器）都会走下面的回退分支，行为与本次改动前完全一致。
         * flags 与既有 seek 路径一致（全部传 0，见 SMPMessageControllerListener 里的
         * mDemuxerService->Seek(pos, 0, -1)）。
         */
        int repositionRet = -1;

        if (repositionLandingUs > 0 && audioIndex >= 0 && mDemuxerService != nullptr &&
            mDemuxerService->getDemuxerHandle() != nullptr) {
            repositionRet = mDemuxerService->getDemuxerHandle()->SeekStream(repositionLandingUs, 0, audioIndex);
        }

        if (repositionRet >= 0) {
            AF_LOGW("audio stream repositioned to landing=%lld index=%d ret=%d (the audio timeline had jumped "
                    "%lld ms) — the audio read position now matches the video landing point\n",
                    (long long) repositionLandingUs, audioIndex, repositionRet,
                    (long long) (repositionJumpUs / 1000));
        } else {
            AF_LOGW("audio stream reposition unsupported (the demuxer interface has no per-stream "
                    "re-position for audio: a single-file container ignores the stream index and moves the "
                    "whole container, while playlist containers already seek the video first and put the "
                    "other streams at that landing point) — keeping the queue-trim path "
                    "(landing=%lld audioJumpedBy=%lld ms audioIndex=%d ret=%d)\n",
                    (long long) repositionLandingUs, (long long) (repositionJumpUs / 1000),
                    audioIndex, repositionRet);
        }
    }

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
        /*
         * 【2026-09-24 追加：seek 在途时**即使帧队列为空**也不能整段跳掉 doRender()】
         *
         * 上面那条只覆盖“队列里有帧”。可本次事故恰好是**队列一直为空**：
         * 缓冲被 seek 清空后 DoCheckBufferPass() 立刻置 mBufferingFlag=true 并
         * 返回 false（见它的 :2079-2103），于是每一轮都在这里 return，
         * doRender() 连同它末尾那条“管线真死”的一次性恢复探测**一次都不执行** ——
         * 这就是为什么整份日志里连一条 `video pipeline looks dead` 都没有，
         * 也是为什么 seek 没有任何自救路径（外层 HSM 545ms 就把播放器停了）。
         *
         * 所以 seek 在途时放行到 doRender()。doRender() 自己会处理“缓冲中不渲染”
         * （render() 的门在 mPlayStatus/mBufferingFlag 上，且这一轮帧队列是空的，
         * RenderVideo() 本来就会 return false），所以**不会**因为放行而多渲染任何帧；
         * 这里放行的唯一目的就是让那条探测在 seek 期间仍然被求值。
         *
         * 只加 mSeekFlag 这一个前置：非 seek 的普通缓冲行为一个字节都没变。
         */
        if (!(mSeekFlag && HAVE_VIDEO)) {
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

    /*
     * 本轮主循环**真的走到底**了（doReadPacket/doDeCode/doRender 都跑过）：
     * 这是状态翻转日志最有价值的一个采样点 —— 它能回答"缓冲有没有起来、
     * seek 有没有被清掉"。放在上面那个 bufferPass 提前 return 之后，
     * 所以"缓冲把本轮挡掉了"这件事会由下一轮的 loop 采样体现为 buffering=1。
     */
    logSeekPipelineState("loop-end");

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
     * 主时钟。这里给目标路一个“预热读取额度”：额度内不受"当前路缓冲已满"
     * 的限制，只按目标队列自己的上限（见循环顶部那处）收敛。
     *
     * 【2026-09-25 修：16 → PENDING_READ_BURST_PER_CALL】
     *
     * 16 太小：一轮读循环只肯为目标路多读 16 个包，其余时间读循环被当前路的
     * 缓冲上限挡住，目标档的数据于是按播放节奏到达（实测 pending 预热 1× 实时，
     * 切换 1.09 s）。现在一次调用就足以把目标队列填到它的上限，让预滚按解码器
     * 速度跑；队列长度仍由 PENDING_VIDEO_QUEUE_CAP / 硬上限兜住。
     * 无切换在途时这个变量不参与任何判断（warmingPending 恒假），行为不变。
     */
    int pendingReadAllowance = PENDING_READ_BURST_PER_CALL;

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

            /*
             * ============ 【2026-09-24 修：闸门必须和主时钟在**同一条时间轴**上比较】============
             *
             * GetPacketPts() 给的是**该流自己的原始 PTS 轴**，而 mMasterClock 是全局时间轴。
             * 切清晰度时 pending 路会建立一条固定偏移
             * （mPendingVideoPtsOffset，提交后成为 mActiveVideoPtsOffset），
             * 这个偏移**只加在帧上**（见下面 DecodeVideoPacket 里那句
             * `pFrame->getInfo().pts += mActiveVideoPtsOffset`），包 PTS 一直是原始值。
             *
             * 真机日志（2026-09-24 那份 249KB）：交接成功后
             *   `read-ahead gate: video packet queue front 16433083 is 14405 ms ahead of
             *    master 2027219 (activeQ=96 queued=4003 ms)`
             * 而这一次的 offset = -8529558（同一份日志 `committed seamless … offset=-8529558`）
             * ⇒ 真实超前只有 (16433083 - 8529558) - 2027219 = 5876306 us ≈ **5.9 秒**，
             * 闸门却按 14.4 秒判：解除要等主时钟爬到 14.4s - 2s，约 **12 秒**，
             * 而按真实值只要 ~3.9 秒。多出来的 8.5 秒纯粹是"两把尺子不在一条轴上"。
             * 这段多等的冻结恰好与实测吻合：队首 16433083 连续 6.1 秒不变、
             * 用户等到 7 秒后手动 seek。
             *
             * 处理：只做归一化，**不动任何阈值**（MAX_VIDEO_READ_AHEAD_US 仍是 2 秒，
             * "手里要有 2 个闸门时长"仍是 4 秒）。mActiveVideoPtsOffset == INT64_MIN
             * （没有偏移，绝大多数会话）时 frontNormalized == frontPts ⇒ 判据逐字不变。
             * 这是通用修正、不是平台分叉：Qt 侧只要发生过切档建立偏移，同样受益。
             */
            const int64_t frontNormalized =
                    (mActiveVideoPtsOffset != INT64_MIN && frontPts != INT64_MIN)
                    ? frontPts + mActiveVideoPtsOffset : frontPts;

            if (!switchInFlight && queuedEnough && audioEnough &&
                frontNormalized != INT64_MIN && masterPts > 0 &&
                frontNormalized - masterPts > MAX_VIDEO_READ_AHEAD_US) {
                if (floodLogAllowed(FLOOD_READ_AHEAD, 1, "read-ahead gate: video packet queue front is ahead of the clock")) {
                    AF_LOGW("read-ahead gate: video packet queue front %lld is %lld ms ahead of master %lld "
                            "(activeQ=%d queued=%lld ms, offset=%lld us → normalized front=%lld)\n",
                            (long long) frontPts, (long long) ((frontNormalized - masterPts) / 1000),
                            (long long) masterPts, (int) mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO),
                            (long long) (queuedDurUs / 1000),
                            (long long) (mActiveVideoPtsOffset == INT64_MIN ? 0 : mActiveVideoPtsOffset),
                            (long long) frontNormalized);
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
            /*
             * 【本轮修"音频被挖掉 1.75s"】清理阈值改成"落点优先"。
             *
             * 本块的目的：丢掉"seek 之前留在缓冲里的迟到音频"。但原来用的阈值是
             * mSoughtVideoPos = **用户目标点**，而一次 seek 之后真正的**内容位置是视频落点**
             * （关键帧）：安卓实测 landing=65.065s、target=69.75s，于是 [65.4, 67.18] 这段
             * **合法音频**被当成迟到数据丢掉 —— 日志
             *   `clean late audio data 8 / 37 / 37 before 69750000`（共 82 包 ≈1.72s）
             * 与紧随其后的
             *   `audio timeline re-anchored (offset 1749339 > 1s)`（前跳 1.749s）
             * 严格吻合，用户听到的就是"少了一段"。
             *
             * 现在：
             *   · seek 窗口内（C 对齐事件已经把落点记进 mSeekAudioFloorUs）用**落点**作阈值 ——
             *     它只会丢掉落点之前的包，与 C 的裁包（ClearPacketBeforeTimePos(AUDIO, landing)）
             *     和双侧地板完全是**同一个阈值**，不会再出现"同一段数据被两个不同阈值处理"；
             *   · 窗口外（mSeekAudioFloorUs == INT64_MIN）**完全回退**到原来的 mSoughtVideoPos，
             *     正常播放的"清迟到音频"行为一字不变。
             *
             * mSeekAudioFloorUs 的生命周期（置位/撤除/复位三处都已核对）：
             *   置位：DecodeVideoPacket() 的 C 对齐事件（落点已知）；
             *   撤除：RenderAudio() 里首帧落点音频真正上设备时（mPlayedAudioPts == INT64_MIN 分支）；
             *   复位：SeekTo() 与 Reset() 都置 INT64_MIN。
             */
            const int64_t cleanBeforeUs = (mSeekAudioFloorUs != INT64_MIN) ? mSeekAudioFloorUs : mSoughtVideoPos;

            if (cleanBeforeUs > 0) {
                int64_t count = mBufferController->ClearPacketBeforeTimePos(BUFFER_TYPE_AUDIO, cleanBeforeUs);
                if (count > 0) {
                    AF_LOGW("clean late audio data %lld before %lld (seek-window landing-priority threshold)\n",
                            count, (long long) cleanBeforeUs);
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

    /*
     * ============ S2：视频路"手里什么都没有" = 缓冲空？？—— 本轮修"偶发静音"============
     *
     * 原来这里只要**视频路**三个队列全空就按"缓冲空"处理（下面统一：暂停主时钟 +
     * 暂停音频渲染 + 不通知界面），目的是避免"画面冻住、声音继续"。
     *
     * 但它的代价正是用户报的这个 bug：**音频还是满的，却因为视频路一时没数据而把声音停掉** ——
     * 主时钟是音频驱动的，音频侧一点问题都没有，却被 `pauseAudioRender(true)` 暂停、
     * 并且 `doRender()` 在 mBufferingFlag 期间根本不调 render() ⇒ 声音断掉；
     * 视频数据一到，缓冲出口又 `pauseAudioRender(false)` ⇒ 声音自动恢复。
     * 表现就是"中途一段时间没声音，之后自己好了"，而且只在**视频侧**短暂断供时出现
     * （4K 流、读包突发、分片边界都常见），所以是"偶发"。
     *
     * 修正：只有当**音频路也真的干了**（内核帧队列 / 音频包队列 / 渲染器里已排队的 PCM /
     * 音频解码器在途包，四处全空）时，才认定"缓存空"并把整条管线停下来 —— 那种情况下
     * 静音是不可避免的，暂停才是如实上报。音频侧还有任何可播内容时，**不动时钟、不动声音**，
     * 画面等视频数据回来自己接上（这也是主流做法：音频是主时钟，视频落后只丢/等视频，
     * 不停音频）。
     *
     * 判据仍是**状态 + 迭代计数**，没有墙钟：三者连续 VIDEO_STARVE_ITERS 轮成立才认，
     * 数据一回来立刻清零。
     */
    if (HAVE_VIDEO && !videoDecoderEOS && !mEof && mPlayStatus == PLAYER_PLAYING &&
        mAVDeviceManager != nullptr) {
        const bool videoNothingToShow =
            mVideoFrameQue.empty() &&
            mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO) == 0 &&
            (!mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO) ||
             mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)->getInputPaddingSize() == 0);

        if (videoNothingToShow) {
            mVideoStarveIters++;
        } else {
            mVideoStarveIters = 0;
        }

        if (mVideoStarveIters >= VIDEO_STARVE_ITERS && cur_buffer_duration > 0) {
            /*
             * 音频路是否也干了：四处只要有一处还有内容，就说明声音还能继续播，
             * 此时**不允许**把整条管线按"缓存空"停下（那就是凭视频侧断供掐掉声音）。
             */
            bool audioAlsoDry = true;

            if (HAVE_AUDIO) {
                const bool audioDecoderDrained =
                    !mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_AUDIO) ||
                    mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_AUDIO)->getInputPaddingSize() == 0;

                audioAlsoDry = mAudioFrameQue.empty() &&
                               mBufferController->GetPacketSize(BUFFER_TYPE_AUDIO) == 0 &&
                               mAVDeviceManager->getAudioRenderQueDuration() == 0 &&
                               audioDecoderDrained;
            }

            if (!audioAlsoDry) {
                if (mVideoStarveIters == VIDEO_STARVE_ITERS) {
                    AF_LOGW("video path has nothing to show but the audio path still has data — NOT pausing the "
                            "clock/render (a video-only gap must never cut the sound); audio frameQ=%d "
                            "packetQ=%d renderQueDuration=%lld\n",
                            (int) mAudioFrameQue.size(),
                            (int) mBufferController->GetPacketSize(BUFFER_TYPE_AUDIO),
                            (long long) mAVDeviceManager->getAudioRenderQueDuration());
                }
            } else {
                if (mVideoStarveIters == VIDEO_STARVE_ITERS) {
                    AF_LOGW("video path has nothing to show and the audio path is dry too (frameQ=0, packetQ(v)=0, "
                            "inputPadding=0 for %d loops) — treating it as an empty cache: pausing the clock and "
                            "the audio render\n",
                            (int) VIDEO_STARVE_ITERS);
                }
                cur_buffer_duration = 0;
            }
        }
    } else {
        mVideoStarveIters = 0;
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
        /*
         * 从这一刻起声音是停的（时钟与音频设备都停了，且缓冲态里 doRender 不调 render()）。
         * 记一条静音开始（reason=4=缓冲态）；缓冲出口恢复时记"结束"。
         * 只在状态变化时各一条，不是周期日志。
         */
        logAudioSilence(AUDIO_SILENCE_BUFFERING,
                        "buffering: the clock and the audio render are paused (empty cache)",
                        mPlayedAudioPts, mMasterClock.GetTime());
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
        /*
         * 【本轮修"seek 之后一段时间没声音"】退出条件原本要求 `videoDecoderFull`
         * （视频解码器输出队列满）——**这是让声音去等视频**：
         *   · 音频是主时钟，seek 之后音频路往往几十毫秒就有数据可播；
         *   · 而凑满视频解码队列在 4K 上要几百毫秒（要真解出 VIDEO_PICTURE_MAX_CACHE_SIZE 帧）；
         *   · 这段时间缓冲态没退出 ⇒ mMasterClock 仍停、pauseAudioRender(true) 仍生效、
         *     且 doRender() 在缓冲态不调 render() ⇒ 用户听到的就是"seek 完一段时间没声音"。
         * （再加上"首个缓冲"用的 startBufferDuration，默认 500ms，两者叠加就是那一段静音。）
         *
         * 现在增加一个**音频就绪**的出口（纯状态，无时间阈值）：
         *   音频帧队列里有已解码帧，或音频包队列里有包可解 —— 任一成立即说明声音立刻能流动。
         * 此时允许退出缓冲态、恢复时钟与音频设备；画面若还没跟上，那是视频自己的事
         * （丢/等视频），不再拖住声音。视频侧的 videoDecoderFull 仍然保留给"没有音频"的片源。
         */
        if (((cur_buffer_duration > HighBufferDur || (HighBufferDur >= mSet->maxBufferDuration && mBufferIsFull)) &&
             (!HAVE_VIDEO || videoDecoderFull || APP_BACKGROUND == mAppStatus ||
              (HAVE_AUDIO && (!mAudioFrameQue.empty() ||
                              mBufferController->GetPacketSize(BUFFER_TYPE_AUDIO) > 0)))) ||
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
                        /*
                         * 缓冲出口：时钟与音频设备都恢复了 = 声音重新流动。
                         * 只在真的从"缓冲态静音"（reason=4）退出时才会打这一条
                         * （logAudioSilence 只在状态变化时输出）。
                         */
                        logAudioSilence(AUDIO_SILENCE_NONE,
                                        "buffering finished: the clock and the audio render are running again",
                                        mPlayedAudioPts, mMasterClock.GetTime());
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

    /*
     * 【B19 终态出口 1/2：READY（status=1）与 FAILED】本函数同时是这两个终态的唯一出口
     * （上面 :3156 那条 NotifyVideoQualitySwitch(ready ? READY : FAILED)）。
     * 切档已经走完，把在途期间被推迟的那一次 PFR 补做一次（内部先清闩、且只在暂停态补做；
     * 若此时又有新切档在途，它只会重新置闩，等那一次的终态 —— 不递归、不立即重试）。
     */
    runDeferredPauseFrameRestore();
    /* B20：切档走到终态，把在途时被推迟的用户 seek 补做一次（先清闩，不递归）。 */
    replayDeferredUserSeek();
    /*
     * ============ 【2026-09-24 修：这里**不能**清 mActiveVideoPtsOffset】============
     *
     * 原代码在这里写 `mActiveVideoPtsOffset = INT64_MIN;`（= 切档一收尾就把偏移扔掉），
     * 这与本文件 DecodeVideoPacket 里那段约束**直接冲突**：
     *   "切换提交后，目标 decoder 的每一帧都必须使用与首帧相同的时间轴偏移。
     *    只修正首帧会导致后续帧突然跳回原始 rendition PTS，RenderVideo() 便会
     *    把它们当成严重落后帧持续丢弃，日志表现为 0/1 FPS。"
     * 而 finishQualitySwitch() 是在 READY 那一刻被调用的 —— 真机实测：提交后仅
     * **167ms** 就走到这里，于是"新流"的后续帧立刻失去归一化。
     *
     * 【真机证据（2026-09-24 那两份日志，同一片源：目标流没有 media time，
     *   提交时 offset=-8587802）】
     *   · READY 前 1 秒的读前闸门：
     *       `read-ahead gate: video packet queue front 16433083 is 5921 ms ahead of
     *        master 1923319 (… offset=-8587802 us → normalized front=7845281)`
     *   · READY 后 1 秒（本行已经执行过）：
     *       `read-ahead gate: video packet queue front 16433083 is 13495 ms ahead of
     *        master 2937512 (… offset=0 us → normalized front=16433083)`
     *   同一个队首，判据从 5.9s 变成 13.5s —— 闸门于是比必要多等 6.5 秒以上，
     *   实测队首连续 11 秒不动、用户等 12 秒后手动 seek。
     *   · 同一时刻的第二个后果：提交后新解出的帧不再被平移（DecodeVideoPacket 里
     *     那句加了 `mActiveVideoPtsOffset != INT64_MIN` 的条件），它们的 pts 落在
     *     全局时钟前 ~8.6 秒 ⇒ 判成"太早"不可渲染 ⇒ 帧在 app 侧累积、输出缓冲
     *     不还给 codec ⇒ codec 停止消费输入（日志 `async input path looks dead
     *     (criterion=starved-without-backpressure, noOutputHeld=true)` +
     *     `codec has had no input buffer for about 1 s`，正好在 READY 后 3.2 秒）
     *     ⇒ 一路拖到错误驱动重建（`accepts no input … rebuilding it once`，
     *     READY 后 13.05 秒）才恢复。
     *
     * 正确做法：偏移的生命周期 = **这条新流作为 active 流的整个生命期**，而不是
     * "切档过程"。它只在这两处被读：
     *   · DecodeVideoPacket 给"没有 media time"的帧做 pts 归一化；
     *   · doReadPacket 的读前闸门把包 pts 折算到主时钟同一把尺子上。
     * 换流时一定会被重新建立或清掉，所以留在这里不会跨流残留：
     *   · 下一次切档提交：TryCommitPendingVideoSwitch 里
     *     `mActiveVideoPtsOffset = mPendingVideoPtsOffset;` 直接覆盖；
     *   · seek / stop / 换源：FlushVideoPath() 与 Reset() 各自清零（那两处是对的，
     *     因为时间轴确实重建了）。
     */
    mQualitySwitchCommitMs = 0;
    mQualitySwitchDeadlineMs = 0;
    /*
     * 暂停态切档的三个新状态必须跟着终态一起清（矩阵第 3、8 格：seek 中止切档、
     * EOS/Stop/换源时不能留下悬空状态）。放在这里而不是每个调用点，是因为
     * finishQualitySwitch() 是所有"切档终态"的唯一收口 —— 只要挂在这里，
     * 成功、失败、超时、被 seek 中止四条路都不可能漏。
     */
    resetPausedSwitchState();
}

/*
 * 给"即将送进 pending 解码器的那个包"贴上目标流的参数集（SPS/PPS/VPS）。
 * 完整原因见 SuperMediaPlayer.h 里的声明；一句话：预滚跳过会把"携带 extradata
 * 的那一条包"整批丢掉，新解码器缺参数集就会解出马赛克。
 */
bool SuperMediaPlayer::attachPendingVideoCodecParams()
{
    if (mPendingVideoCodecParamsAttached) {
        /* 本轮 pending 解码器已经贴过了，不重复贴（每个包都贴等于每帧一次
         * JNI 拷贝，白花 CPU）。 */
        return false;
    }

    if (mDemuxerService == nullptr || mPendingVideoStreamIndex < 0 || mVideoPacket == nullptr) {
        return false;
    }

    unique_ptr<streamMeta> pMeta;
    if (mDemuxerService->GetStreamMeta(pMeta, mPendingVideoStreamIndex, false) < 0 || !pMeta) {
        return false;
    }

    auto *meta = (Stream_meta *) (*pMeta);

    if (meta->extradata == nullptr || meta->extradata_size <= 0) {
        return false;
    }

    /*
     * 参数集（H.264 的 SPS/PPS、H.265 的 VPS/SPS/PPS）挂在**即将真正送进解码器的
     * 那个包**上。只有第一个包需要它：解码器收到后会把 CSD 存进自己的参数表，
     * 后续包照旧走正常路径。
     */
    mVideoPacket->setExtraData(meta->extradata, meta->extradata_size);
    mPendingVideoCodecParamsAttached = true;
    return true;
}

/*
 * seek 卡死排查用的状态翻转日志（声明与原因见 SuperMediaPlayer.h）。
 *
 * 为什么必须是"翻转时打一行"而不是每轮打：
 *   * 主循环是 ~2ms/轮（mVideoCatchingUp 时），逐轮打日志本身就会拖慢它，
 *     而且会把日志淹掉 —— 排查 seek 卡死时最需要的就是"安静的这段时间里
 *     状态到底变过没有"，刷屏反而把答案冲走；
 *   * 翻转日志的语义正好是"谁在什么时候变成了什么"，一次 seek 最多几行。
 *
 * 打出来的量刻意包含"帧队列长度"（不是布尔）：seek 之后"队列一直是 0"
 * 和"队列有过帧但没上屏"是两种完全不同的病（前者是没数据/解码器不出帧，
 * 后者是渲染判定把帧丢了），只看长度变化就能分开。
 */
void SuperMediaPlayer::logSeekPipelineState(const char *why)
{
    const int frameQueSize = (int) mVideoFrameQue.size();
    const bool vDecValid = (mAVDeviceManager != nullptr &&
                            mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO));
    const int playStatus = (int) mPlayStatus.load();

    /*
     * 只在**语义字段**变化时打（本轮修刷屏）：
     * 原来把 frameQueSize 也当成"变化"的判据，而 frameQ 在 0/1 之间每轮都跳，
     * 于是每轮主循环都打一条 —— 实测 2 秒 190+ 行，直接用 CPU 和日志带宽把 seek
     * 拖慢。帧队列长度只作为输出字段，不再参与判定。
     */
    if (mSeekFlag == mSeekDiagLastSeekFlag &&
        mSeekNeedCatch == mSeekDiagLastSeekNeedCatch &&
        mBufferingFlag == mSeekDiagLastBufferingFlag &&
        videoDecoderEOS == mSeekDiagLastVideoDecoderEOS &&
        playStatus == mSeekDiagLastPlayStatus &&
        vDecValid == mSeekDiagLastVideoDecoderValid) {
        return;
    }

    mSeekDiagLastSeekFlag = mSeekFlag;
    mSeekDiagLastSeekNeedCatch = mSeekNeedCatch;
    mSeekDiagLastBufferingFlag = mBufferingFlag;
    mSeekDiagLastVideoDecoderEOS = videoDecoderEOS;
    mSeekDiagLastPlayStatus = playStatus;
    mSeekDiagLastVideoDecoderValid = vDecValid;

    /*
     * 用 AF_LOGI：本次事故里能看到的日志下限就是 INFO（AF_LOGD 全被过滤了），
     * 这条日志的存在意义就是在"下一次只剩三条 INFO"时不至于又抓瞎。
     * 注意这里**只读**状态，不改任何标志，因此对播放语义零影响。
     */
    /*
     * 【B5-5】主时钟还没被赋过有效值时，`GetTime()` 会给出"哨兵 + elapsed"的环绕值
     * （`[seekdiag] … master=-9223372036824759729` 就是这么来的，见 af_clock.h 里
     * af_clock_value_is_unset() 的说明）。诊断行不该把这种值当时间打出去 ——
     * 读日志的人会以为时钟真的在那里，于是把"起播/seek 早期的噪声"误读成故障。
     * 判据纯状态、无阈值语义（那个 1 小时的带宽只是"不可能是真时间"的边界）。
     */
    char masterText[32] = {0};
    const int64_t masterForDiag = mMasterClock.GetTime();

    if (af_clock_value_is_unset(masterForDiag)) {
        snprintf(masterText, sizeof(masterText), "unset");
    } else {
        snprintf(masterText, sizeof(masterText), "%lld us", (long long) masterForDiag);
    }

    AF_LOGI("[seekdiag] %s: seekFlag=%d seekNeedCatch=%d buffering=%d firstBuf=%d "
            "videoEOS=%d playStatus=%d frameQ=%d packetQ(v)=%d master=%s vDecValid=%d\n",
            why != nullptr ? why : "?",
            (int) mSeekFlag, (int) mSeekNeedCatch, (int) mBufferingFlag, (int) mFirstBufferFlag,
            (int) videoDecoderEOS, (int) mPlayStatus.load(), frameQueSize,
            mBufferController != nullptr ? (int) mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO) : -1,
            masterText,
            (int) (mAVDeviceManager != nullptr &&
                   mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)));
}

void SuperMediaPlayer::resetPausedSwitchState()
{
    mSwitchStartedWhilePaused = false;
    mPausedSwitchPivotUs = INT64_MIN;
    mPausedSwitchRenderPending = false;
    mQualitySwitchPrerollDeadlineMs = 0;
}

void SuperMediaPlayer::beginQualitySwitchTracking()
{
    /*
     * 每一次新的切档请求都从这里重新开始。先把上一轮可能残留的状态清掉
     * （连续点按、ABR 自动切都会走到这里），再判断本轮是不是暂停态发起的。
     */
    resetPausedSwitchState();

    mSwitchStartedWhilePaused = (mPlayStatus == PLAYER_PAUSED);

    if (mSwitchStartedWhilePaused) {
        /*
         * 冻结参考时刻 = 暂停时用户看到的位置。为什么不用 mMasterClock：
         * 主时钟在暂停态是 pause() 的，但缓冲/seek 等路径都可能把它挪动，
         * 而"暂停切档"的语义要求参考点**绝对不动**。getCurrentPosition() 读的
         * 就是用户可见的位置（并且被 mSeekPositionFloorUs 钉住），最贴近意图。
         */
        mPausedSwitchPivotUs = getCurrentPosition();

        if (mPausedSwitchPivotUs < 0) {
            /* 位置还不可用（极少见）：退化成用主时钟读一次，仍只读一次。 */
            mPausedSwitchPivotUs = mMasterClock.GetTime();
        }

        AF_LOGI("quality switch started while paused: pivot=%lld us (frozen), current=%d pending=%d "
                "— pending preroll will target the last key frame at or before the pivot and the "
                "commit anchor will be the first frame at or after it\n",
                (long long) mPausedSwitchPivotUs, mCurrentVideoIndex, mPendingVideoStreamIndex);
    }

    /*
     * 提交前的墙钟死线：**只要有任何在途切换就必须有出口**。这里无条件武装，
     * 播放态也一样 —— 播放态正常几百毫秒就提交并转入 mQualitySwitchDeadlineMs，
     * 这条只是"永远不收敛"的最终兜底。
     */
    mQualitySwitchPrerollDeadlineMs = af_getsteady_ms() + QUALITY_SWITCH_PREROLL_DEADLINE_MS;
}

/*
 * 【第 7 项：死代码清理（零行为变化）】
 * 下面两个"切档死线兜底"函数已经**没有任何调用点**（切档终态现在由事件驱动：
 * 提交后首帧上屏 / 失败即报，见 RenderVideo 与 finishQualitySwitch 那条路径）。
 * 用编译期开关整体停用而不是直接删掉函数体，便于需要时追溯与恢复
 * （把 0 改 1 即可）。停用后它们的头文件声明仍然存在但不再被调用，链接无影响。
 */
#if 0
void SuperMediaPlayer::checkQualitySwitchPrerollDeadline()
{
    /*
     * S9 的提交前一半：覆盖矩阵里每一种"在途"状态 —— 暂停切档在途、播放中
     * 切档在途、seek 在途、以及三者叠加。
     *
     * 判据刻意只用**墙钟 + 是否有在途切换**，不看 mPlayStatus、不看
     * mBufferingFlag、不看 seek 状态：任何一条"看状态"的判据在暂停/缓冲/seek
     * 期间都可能永远不成立，那正是原实现把状态机钉死 28.6 秒的形状。
     *
     * 与 checkQualitySwitchDeadline() 的分工：
     *   * 本函数管**提交前**（pending 预热还没 promote）—— 18s 那条写在解码循环
     *     里的超时到不了暂停态，这里补上；
     *   * checkQualitySwitchDeadline() 管**提交后**（已 promote，等首帧上屏）。
     * 两条都以墙钟计时，合起来保证"任何在途切换都有到点的出口"。
     */
    if (mQualitySwitchPrerollDeadlineMs <= 0) {
        return;
    }

    /* 已经提交 → 交给 checkQualitySwitchDeadline()，本条退出。 */
    if (mQualitySwitchCommitPending) {
        mQualitySwitchPrerollDeadlineMs = 0;
        return;
    }

    /* 没有在途切换（被取消/已完成）→ 撤掉死线。 */
    if (mPendingVideoStreamIndex < 0 && mWillChangedVideoStreamIndex < 0) {
        mQualitySwitchPrerollDeadlineMs = 0;
        return;
    }

    const int64_t now = af_getsteady_ms();

    if (now < mQualitySwitchPrerollDeadlineMs) {
        return;
    }

    AF_LOGW("quality switch preroll deadline hit before commit: pending=%d target=%lld pivot=%lld "
            "pausedSwitch=%d seekFlag=%d buffering=%d frameQ=%d pendingPktQ=%d pendingFrameQ=%d "
            "elapsed=%lld ms — finishing as FAILED so no in-flight switch can hang forever\n",
            mPendingVideoStreamIndex, (long long) mPendingVideoSwitchTimePosition,
            (long long) mPausedSwitchPivotUs, (int) mSwitchStartedWhilePaused,
            (int) mSeekFlag, (int) mBufferingFlag, (int) mVideoFrameQue.size(),
            (int) mPendingVideoPacketQue.size(), (int) mPendingVideoFrameQue.size(),
            (long long) (mPendingVideoSwitchStartMs > 0 ? now - mPendingVideoSwitchStartMs : -1));

    /*
     * 统一走 finishQualitySwitch(false, ...) 的既有收尾：它会关 pending/retired
     * 流、释放 retired 解码器、清 commitPending/offset/各计数器，并且发 FAILED
     * 给 UI（高亮退回旧档）。这里额外把 pending 路自己的状态也清干净，
     * 保证"立刻再切一次"可用（矩阵第 10 格）。
     */
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
        AF_LOGI("closed pending video stream after preroll deadline: stream=%d\n", mPendingVideoStreamIndex);
    }

    mPendingVideoStreamIndex = -1;
    mWillChangedVideoStreamIndex = -1;
    mPendingVideoDecoderSwitch = false;
    mPendingVideoPtsOffset = INT64_MIN;
    mPendingVideoSwitchTimePosition = INT64_MIN;
    mPendingVideoSwitchStartMs = 0;
    mPendingVideoInitPacketSent = false;
    mPendingVideoPrerollDone = false;
    mPendingVideoPrerollRefUs = INT64_MIN;
    mPendingVideoCodecParamsAttached = false;

    mPNotifier->NotifyVideoQualitySwitch(PLAYER_QUALITY_SWITCH_FAILED,
                                         mQualitySwitchCommittedStreamIndex,
                                         "quality switch preroll did not complete in time");

    finishQualitySwitch(false, "quality switch preroll deadline hit (never committed)");
    resetPausedSwitchState();
}

#endif

/* 同上：提交后的 500ms 死线兜底同样已无调用点（终态由"新帧上屏/失败上报"驱动）。 */
#if 0
void SuperMediaPlayer::checkQualitySwitchDeadline()
{
    if (!mQualitySwitchCommitPending || mQualitySwitchDeadlineMs <= 0) {
        return;
    }

    const int64_t now = af_getsteady_ms();

    /*
     * ============ 提交之前可以"等状态"，提交之后**只认墙钟**（2026-09-24）============
     *
     * 原来这里是**无条件**延期：
     *
     *     if (mPlayStatus != PLAYER_PLAYING || mBufferingFlag) {
     *         mQualitySwitchDeadlineMs = now + QUALITY_SWITCH_DEADLINE_MS;
     *         return;
     *     }
     *
     * 它有两个问题，正好对应两组验收格：
     *
     * 1) **暂停/缓冲态没有墙钟出口**。在途切换如果一直停在"提交了但没帧上屏"，
     *    这里每轮都把它往后推 → deadline 永远不会到 → `commitPending` 无限悬空，
     *    界面既收不到 READY 也收不到 FAILED。实测（用户日志）就是
     *    `[switch]` 关键量 28.6 秒逐字不变、18s 超时因为管线不动被拖到 33.3s；
     *    而 Android 侧预编译的 PlayerBase 会在 545ms 就 baseTimeout() 把播放器
     *    stop() 掉 —— 一条**永远不结束**的在途状态就是让外层 HSM 开火的直接原因。
     *
     * 2) 它用的是 `af_getsteady_ms()`（单调墙钟）而不是主时钟 —— 但主时钟在
     *    暂停/缓冲时是**冻结**的，两者混用会让"延期"和"判定"的可比性丢失。
     *    这里统一到墙钟：暂停、缓冲、后台、seek 在途……任何状态下，只要
     *    切换已经**提交**，就必须在一个 QUALITY_SWITCH_DEADLINE_MS 之内出终态。
     *
     * 保留的语义（不能丢）：**提交之前**暂停/缓冲确实不该计时。暂停中用户点了
     * 切档、预滚还没走完时，判 FAILED 会把一次本来能成功的切换误报成失败
     * （界面高亮退回旧档）。所以延期只保留给"尚未提交"这一段，而且同样用墙钟 ——
     * 于是它也从"无限延期"变成"提交前可以等，提交后必然收敛"。
     *
     * 对播放中路径零影响：提交后若能正常出帧，frameRenderedRecently / 追赶窗口
     * 那两个出口（见下面）本来就会先把 deadline 推到 now + QUALITY_SWITCH_DEADLINE_MS，
     * 走不到这里；真正会走到这一条的只有"提交了却一帧都没上屏"——
     * 那本来就是要判 FAILED 的情形，只是现在**在暂停/缓冲/后台态也判得出来**了。
     */
    if (!mQualitySwitchCommitPending && (mPlayStatus != PLAYER_PLAYING || mBufferingFlag)) {
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
    const bool joiningWindowActive = dropLateVideoFrames;
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
 * 开始“渲染器追赶窗口”（事件驱动，见上面 JOINING 的说明）。
 *
 * 只置 dropLateVideoFrames（RenderVideo 与 DecodeVideoPacket 都读它）：
 * 迟到帧会被丢掉、过时的包不再送去解码，直到出现一帧准时帧上屏 —— 那一刻
 * RenderVideo 会把它清回 false。**没有时间上限**。
 */
#endif

void SuperMediaPlayer::beginRendererJoining(const char *why)
{
    dropLateVideoFrames = true;
    mDropLateVideoFramesUntilMs = 0;

    /*
     * 【第 4 项：开窗时先把"必然要被丢掉"的帧从我们自己的画面队列里收敛掉】
     *
     * 现象（Qt 日志）：切档提交后 `renderer joining started (quality switch committed)`
     * 紧接着就是成组的 `AFActiveVideoRender: drop a frame`（每 150ms 三连丢，例如
     * 12:17:48.648 / 48.802 / 48.951）。原因是开窗之后 RenderVideo 会逐帧丢掉迟到帧，
     * 但**丢弃发生在把帧推给渲染器之后**：渲染器的输入队列先被这批注定要丢的帧填满，
     * 于是它自己再逐帧 drop，并顺手把自己的渲染时钟重锚到最新帧（等于让渲染器替我们
     * 做了一次追赶，画面会跳）。
     *
     * 处置：开窗这一刻就把"按既有迟到判据本来就会被丢"的帧（落后主时钟超过
     * JOINING_STALE_FRAME_US，与 RenderVideo 里 500ms 的迟到阈值同值）从
     * mVideoFrameQue **队首连续**清掉。显示结果完全不变（这些帧在原路径下也会被丢），
     * 但渲染器再也收不到注定被丢的帧，那串 drop 风暴随之消失。
     * 只清队首连续过期帧：能关闭追赶窗口的那一帧（落在主时钟附近）绝不会被清到。
     * 纯状态驱动，没有计时器，也没有周期性动作。
     */
    if (!mVideoFrameQue.empty()) {
        const int64_t masterUs = mMasterClock.GetTime();
        int purged = 0;

        if (masterUs != INT64_MIN) {
            while (!mVideoFrameQue.empty() && mVideoFrameQue.front() != nullptr) {
                const int64_t framePts = mVideoFrameQue.front()->getInfo().pts;

                if (framePts != INT64_MIN && framePts + JOINING_STALE_FRAME_US < masterUs) {
                    /* mVideoFrameQue 是 std::queue：只有 pop()（弹队首），没有 pop_front()。 */
                    mVideoFrameQue.pop();
                    purged++;
                    continue;
                }

                break;
            }
        }

        if (purged > 0) {
            AF_LOGI("renderer joining (%s): convergent drop of %d already-late frame(s) before handing "
                    "anything to the renderer (master=%lld)\n",
                    why != nullptr ? why : "-", purged, (long long) masterUs);
        }
    }

    AF_LOGI("renderer joining started (%s): late frames will be dropped until a frame lands on the "
            "master clock (master=%lld)\n",
            why != nullptr ? why : "-", (long long) mMasterClock.GetTime());
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

    /*
     * ============ S5：暂停态切档 —— 渲染**恰好一帧**到达终态（2026-09-24）============
     *
     * 这是暂停态无感切档的最后一环（门1）。上面 S1-S4 已经把"冻结参考点"打通，
     * pending 路能预热、能提交；但提交之后还有一道门：新帧要真的**上屏**才算
     * 切档完成（RenderVideo() 里 mQualitySwitchCommitPending + finishQualitySwitch
     * 的判定挂在"帧真的走进渲染器"那一刻）。而 doRender() 只在 PLAYER_PLAYING
     * 调 render() —— 暂停态一帧都不会上屏，于是状态机停在"已提交但没上屏"，
     * 由 QUALITY_SWITCH_DEADLINE_MS 兜底判 FAILED：用户点了切档却看到高亮弹回去。
     *
     * 所以这里加一个与上面 seek 分支**同级**的分支：暂停态切档提交之后，
     * 在暂停态渲染**恰好一帧**。要点：
     *   * 用 RenderVideo(true)（force_render），与 seek 分支同一个用法 ——
     *     暂停时主时钟不动，按节拍判定会永远不满足；
     *   * **恰好一帧**：成功一帧立刻清 mPausedSwitchRenderPending，
     *     不会变成"暂停期间一直在渲染"（那不是暂停）；
     *   * 只渲染、**不动主时钟、不发 NotifyPosition**（S6）—— 用户看到的位置
     *     必须还是暂停时那个位置；
     *   * 显式排除 APP_BACKGROUND（矩阵第 7 格）：后台不渲染，否则会和
     *     video hold / DummySurface 那条路打架；后台期间死线照走（S9），
     *     所以后台也不会把切换挂住 —— 它会在到点时以 FAILED 收敛。
     *
     * 对播放中路径零影响：前置条件是 PLAYER_PAUSED 且 mPausedSwitchRenderPending，
     * 而后者只在"暂停态切档提交"那一刻置真；PLAYING 时恒为假，整块不进入。
     */
    /*
     * ============ 【★ 暂停态 seek：目标帧到了却没人交给渲染器 ⇒ 画面/进度条都不动 ★】============
     *
     * 实测（用户日志）：
     *   14:05:50.132  renderer joining started (seek finished) … master=83445
     *   14:05:53.145  [seekdiag] loop: seekFlag=0 playStatus=6(PAUSED) frameQ=1 master=3092596 us
     *   之后 fps=0.0、画面停在原地、进度条也不动（直到十几秒后有别的事件发生）。
     *
     * 机制：`render()` 那道门在 `mPlayStatus == PLAYER_PLAYING` 上；暂停态唯一会交帧的入口
     * 原来是"切档待渲染"。于是**暂停态 seek** 一旦 `mSeekFlag` 清掉（SeekEnd），
     * 之后才解出来的目标帧 / 落点帧就**没有任何分支把它送进渲染器** —— 帧队列里明明有帧
     * （frameQ=1），画面却永远停在上一次上屏的那张，位置回调也不再更新。
     *
     * 处理：把"暂停 + seek 相关"并进同一个分支（纯状态判据，无计时器）：
     *   `mSeekFlag`（seek 在途）、`mSeekRenderGateUs != INT64_MIN`（落点帧尚未采纳）都算在内；
     *   每次只交一帧，靠帧自己的 pts 去重；语义与切档那条一致 —— **不动主时钟、不发位置通知**，
     *   用户看到的位置仍是暂停时那个位置，只有画面换成落点帧。
     *   切档那条的老行为一字未变（清 mPausedSwitchRenderPending 与那条日志仍只属于它）。
     * ==========================================================================================
     */
    if ((mPausedSwitchRenderPending || mSeekFlag || mSeekRenderGateUs != INT64_MIN) &&
        mPlayStatus == PLAYER_PAUSED &&
        mAppStatus != APP_BACKGROUND && HAVE_VIDEO) {
        const bool pausedSwitchFrameRendered = RenderVideo(true);

        if (pausedSwitchFrameRendered && mPausedSwitchRenderPending) {
            /*
             * 恰好一帧送达渲染器：清掉欠账，后面的终态判定由 RenderVideo() 里
             * "提交后首帧上屏"那条既有逻辑负责（它会走 finishQualitySwitch(true)）。
             * **不**在这里造第二个终态出口，避免两处同时收尾。
             */
            mPausedSwitchRenderPending = false;
            AF_LOGI("paused quality switch rendered one frame at pts=%lld (master untouched at %lld, "
                    "position callback suppressed) — the switch can now reach READY\n",
                    (long long) mPlayedVideoPts, (long long) mMasterClock.GetTime());
        }
    }

    if (rendered) {
        mFirstBufferFlag = false;

        /*
         * ============ 锚点事件（路径无关，本轮修"seek 没反应"）============
         *
         * 事件定义：**本次 seek 之后第一帧真的上屏的视频帧**。
         * 为什么放在这里而不是 RenderVideo 的落点块里：那个块有前置条件
         * （frameTimePosition >= 0 且渲染门判定），而部分容器/流的 timePosition 缺失，
         * 于是"落点采纳"永远不发生。上一轮我把 seek 结束**强制绑定**在这个事件上，
         * 结果就是：落点采纳不来 → mSeekFlag 永远清不掉 → seek 完全没反应、一直转圈。
         * 这是回归，本轮改掉：锚点与 seek 结束解绑，两者都只依赖"有帧上屏"这个
         * 在任何渲染路径上都必然发生的事件。
         *
         * 只锚一次（mSeekAnchorPending 事件闩 + mSeekVideoAnchorDone），锚定值取
         * mPlayedVideoPts —— 渲染路径刚刚把上屏帧的 PTS 写进去，正是落点/目标帧自己的 PTS。
         *
         * ============ 消费不变式（本轮修"master 停在目标点、内容在落点"）============
         *   · 这个事件**只允许被"锚定成功"消费**：真的把主时钟锚到 mPlayedVideoPts 之后，
         *     立刻清 mSeekAnchorPending；
         *   · 没有锚定就**必须留着**，等下一帧视频上屏（或下一次 SeekTo() 复位）——
         *     不允许任何其它路径清它。
         *
         * 为什么必须这样：这里的前置是 `rendered`，而 rendered 为真也可能是**纯音频渲染**
         * （render() 返回的 audioRendered 为真，见 doRender 里 `rendered |= render()`）。
         * 那一刻 mPlayedVideoPts 还是 INT64_MIN（seek 的 FlushVideoPath() 把它清了），
         * 旧写法无条件清闩 ⇒ 锚点被白白消费、永不发生 ⇒ 主时钟停在 seek **目标点**，
         * 而视频落点与音频内容都在**落点**上：安卓实测 master≈69.75s、内容 65.07s（差 4.7s），
         * 视频被判"迟到"全丢、随后还触发一次解码器停摆重建（`video decoder accepts no input
         * and produced no frame … rebuilding it once`），用户听到的"少一段"就是这个错位的后果。
         *
         * 锚定值只用 mPlayedVideoPts（渲染路径、节拍判定、丢弃判定用的都是这条时间轴），
         * 不用 frameTimePosition 那条轴。
         * `mPlayedVideoPts != INT64_MIN` 必然是"seek 之后新渲染的视频帧"：
         * FlushVideoPath() 无条件把它置 INT64_MIN（SuperMediaPlayer.cpp:7087 一带）。
         */
        if (mSeekAnchorPending && !mSeekVideoAnchorDone && mPlayedVideoPts != INT64_MIN) {
            /*
             * ============ 【锚点规则收口】只允许"目标"或"不早于目标的落点帧" ============
             *
             * 原来这里**无条件**把主时钟设成 mPlayedVideoPts（seek 之后第一张上屏的帧）。
             * 实测那条锚点日志：
             *   seek anchor: master clock anchored to the first frame rendered after the seek
             *                (pts=83438, target=2844000, landingFloor=-9223372036854775808)
             * 帧 0.083s、目标 2.844s、地板未设 —— 时钟被钉在旧时间轴的 0.083s 上按 1x
             * 往前走，之后整段时间轴错位（位置上报、追赶窗口、落点判据全按错轴判）。
             *
             * 现在判据由 fetchSeekClockAnchorUs() 统一给出（它是两处锚点共用的唯一实现）：
             *   · 这一帧早于目标超过一个容差 ⇒ **不锚**，事件留在闩上（不清 mSeekAnchorPending、
             *     不置 mSeekVideoAnchorDone），等 >= 目标的那一帧（含目标的那一帧）上屏再来锚；
             *   · 这一帧在目标附近或之后 ⇒ 锚到它（落点在目标之前则锚到目标本身）。
             * 位置上报的地板与落点判据读的都是目标，所以时间轴不会再被拉回到 seek 之前。
             * ==========================================================================
             */
            const int64_t seekAnchorUs = fetchSeekClockAnchorUs(mPlayedVideoPts, "first frame rendered after the seek");

            if (seekAnchorUs != INT64_MIN) {
                /*
                 * ============ 【修：锚点绝不允许把主时钟往回拉】============
                 *
                 * 真机日志（安卓 2026-09-26 20:55:38，用户 seek 到 116.273s）逐行可对：
                 *   38.825  seek anchor refused: frame=99933167 earlier than target=116273000
                 *   38.851  seek anchor: pts=99966533, target=-9223372036854775808,
                 *           landingFloor=116273000, anchor=99966533
                 *   39.033  seek anchored by the video landing frame, audio pts=116288000
                 * 第二行里 `target=-9223372036854775808`（= INT64_MIN）说明：锚定发生时
                 * `mSeekPositionFloorUs` 与 `mSeekPos` **都已经被清掉**（seek 结束走的
                 * ResetSeekStatus()，见本文件 10912 / 10929），于是 fetchSeekClockAnchorUs()
                 * 走"目标未知"兜底分支返回了这一帧自己的 pts —— 主时钟从 116.273s 被
                 * **倒退**到 99.966s（-16.3 秒），36ms 之后又被落点帧拉回 116.288s。
                 * 用户看到的就是"进度条先往回弹一下、然后又弹回去"；而时钟倒退的那段时间里
                 * 所有正常帧都成了"迟到帧"被丢掉 —— 也就是"seek 后卡死半天不动"。
                 *
                 * 判据（纯状态、无阈值、无计时器）：**只有向前才改写主时钟**。
                 * 目标已知时这不会改变任何行为 —— fetchSeekClockAnchorUs() 的返回值是
                 * max(落点帧, 目标)，本来就不可能小于 seek 目标；唯一会小于当前时钟的情形，
                 * 正是上面那种"目标已被清掉、这一帧是旧位置"的坏状态，也就是要拦的这一种。
                 */
                const int64_t masterBeforeUs = mMasterClock.GetTime();

                if (seekAnchorUs >= masterBeforeUs) {
                    mMasterClock.setTime(seekAnchorUs);
                    AF_LOGW("seek anchor: master clock anchored to the first frame rendered after the seek "
                            "(pts=%lld, target=%lld, landingFloor=%lld, anchor=%lld)\n",
                            (long long) mPlayedVideoPts, (long long) mSeekPositionFloorUs,
                            (long long) mSeekAudioFloorUs, (long long) seekAnchorUs);
                } else {
                    AF_LOGW("seek anchor SKIPPED: anchoring on pts=%lld would move the master clock BACKWARD "
                            "from %lld (by %lld ms) — the frame is older than the position the clock is "
                            "already on (this is the 'progress bar bounces back' state: the seek target has "
                            "already been cleared, so this frame carries no usable anchor)\n",
                            (long long) seekAnchorUs, (long long) masterBeforeUs,
                            (long long) ((masterBeforeUs - seekAnchorUs) / 1000));
                }

                /*
                 * 事件在两种出口都消费掉：目标已经被清掉的这一帧之后，再等下一帧只会得到
                 * 一个更晚的旧位置，锚定已经没有正确值可用 或 时钟本身就在正确位置上。
                 */
                mSeekClockAnchored = true;
                mSeekVideoAnchorDone = true;
                mSeekAnchorPending = false;
            }
        }

        //may audio already played over
        if (mEof && mAudioFrameQue.empty() && mBufferController->GetPacketSize(BUFFER_TYPE_AUDIO) == 0) {
            mMasterClock.setReferenceClock(nullptr, nullptr);
        }

        if (mSeekFlag) {
            // 暂停帧恢复期间：渲染门命中前不能结束 seek —— 门会把目标帧
            // 之前的帧全部挡掉，若按第一帧就宣告 seek 完成，暂停态的
            // 读包/解码管线会停，目标帧永远解码不出来（黑屏根因）
            IDecoder *videoDecoder = mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);

            /*
             * 【让 seek 结束回到"有帧上屏"这个充分条件（本轮修回归）】
             *
             * 上一轮这里额外要求 mSeekLandingFrameAccepted（"落点帧被采纳"），
             * 而落点采纳块本身要求 timePosition >= 0 且能通过渲染门 —— 只要该字段缺失
             * （部分容器/流就是不给），采纳永远不发生，seek 就永远结束不了：
             * 用户看到的就是"seek 没反应、一直转圈、进度条像摆设"。
             *
             * 现在恢复为原来的充分条件（isRenderGateHit：暂停帧恢复期间由 codec 渲染门
             * 兜住），主时钟的锚定则交给上面那个**路径无关**的"第一帧上屏"事件。
             * 这样"锚点唯一"与"seek 一定能结束"两件事各自成立、互不绑架。
             */
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
                /*
                 * 这里原来有一段"seek 完成前发现主时钟超前目标点就把它拉回"的补救式写时钟。
                 * 本轮删除（W13）：一次 seek 的主时钟只允许被"落点事件"锚定一次
                 * （见 RenderVideo 的落点帧分支与 renderAudioFrame 的音频锚定），
                 * 事后纠正属于补救路径，与看门狗同类，不允许保留。
                 */

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
    /*
     * ============ 视频路"停滞就判死并重建解码器"的探测已整体删除（本轮）============
     *
     * 上面那一大段注释描述的是被删掉的机制（原 VIDEO_RECOVER_SAMPLE_MS /
     * VIDEO_RECOVER_STALL_MS / VIDEO_RECOVER_STUCK_SEEK_MS / VIDEO_RECOVER_COOLDOWN_MS），
     * 保留作为历史记录。删除理由：
     *   · 它用"多久没上屏 + 队列没变浅"推断"解码器死了"，然后 rebuildVideoDecoder() ——
     *     属于红线禁止的看门狗 / 超时兜底；
     *   · 主流实现（mpv/ffplay）不靠时间推断解码器死亡：解码器故障一律走**错误驱动**
     *     的重建（本文件的 mVideoDecodeRebuildCount + rebuildVideoDecoder 调用），
     *     管线停顿由"缓冲/停顿"状态如实上报给界面，而不是偷偷改管线；
     *   · seek 场景的根因（渲染地板要求解完整段 GOP 前缀）已由 K1 落点帧即上屏修掉。
     * 需要观测时保留日志与状态，不允许再据时间改动管线。
     */

    /*
     * ============ seek 期间"多久没出帧就重建解码器" 已删除（本轮）============
     *
     * 这里原来是两条**墙钟**判据（输入饿死 900ms / 无帧 3000ms）→ rebuildVideoDecoder()。
     * 按红线（禁止看门狗 / 超时兜底）删除，理由：
     *   · 它要治的"seek 老不出帧"，根因是渲染地板要求把目标点之前的整段 GOP 解完
     *     （见 RenderVideo 里 K1 的"落点帧即上屏"），K1 落地后这个窗口不存在了；
     *   · 4K 下一次重建要释放并重配约 250MB 的编解码输出缓冲池，代价远大于收益；
     *   · 主流实现（mpv / ffplay）没有"seek 超时就重建解码器"这种东西 —— 解码器不可用
     *     走的是**错误驱动**的 release + init（本文件 MAX_VIDEO_DECODER_REBUILDS 那条路径），
     *     不是时间驱动。
     * 需要观测"seek 卡住"时，保留日志与状态上报即可，不允许再据时间改动管线。
     */
    /*
     * 上面那个"seek 期间 900ms/3000ms 没出帧就重建解码器"的墙钟块已整体删除（本轮）。
     * 该块是唯一消费者，删除后 mSeekNoFrameSinceMs / mSeekStallDiagLogged 不再被读写，
     * 仅作为历史状态留在头文件里；解码器不可用一律走错误驱动的重建路径。
     */

    /*
     * ============ 解码器"有输入却零输出"时的一次性重建（纯状态，事件驱动）============
     *
     * 为什么需要它（这是"seek 完一直卡在加载中"的最后一环）：
     *   Android 的 MediaCodec 可能**拿着输出缓冲不放手**：
     *   framework/codec/Android/mediaCodecDecoder.cpp 的 enqueue_decoder() 拿不到输入
     *   缓冲时返回 -EAGAIN（那里明确写了"只回报 -EAGAIN：包不丢、解码器不拆"），
     *   而 ActiveDecoder::extract_decoder() 又把 -EAGAIN 吞掉 —— 于是**内核看不到任何
     *   错误**：get_error_frame_no() 恒为 0，DecodeVideoPacket 里那条错误驱动的重建
     *   （STATUS_HAVE_ERROR / STATUS_DRM_ERROR）永远不会触发。原来唯一会发现它的是
     *   doRender() 里"多久没上屏"的墙钟探测，本轮已按红线删除，所以现在必须补一条
     *   **不依赖时间**的判据，否则这就是一个真正的死结：mSeekFlag 只由"有帧上屏"清掉。
     *
     * 【第 2 项：窗口从 seek 移到"应当有帧在流的播放态"】
     *   安卓日志实测该卡死发生在 seek **结束之后**：
     *     · 每次 seek 的 seekFlag 只活 16~23ms（12:05:29.638 → 29.709 即 seekFlag=0）；
     *     · codec 的饥饿信号 0.5~1s 后才出现
     *       （12:05:32.818 "codec has had no input buffer for about 1 s
     *        (output buffers still held …)"）；
     *     · 卡死时刻的采样是 seekFlag=0、frameQ=0、packetQ(v)=3226、vDecValid=1、
     *       playStatus=5(播放)、buffering=0。
     *   两条时间线永远不重叠，所以旧窗口下这条判据一次都没成立过（实测 0 条日志）。
     *   现在窗口是"解码器本该产出帧"的播放态：播放中、非缓冲、非 EOF、非后台，
     *   或者 seek 在途（seek 期间也要能救）。判据本身（下面 5 条）一条没改。
     *
     * 判据（全部是状态，没有时间、没有周期动作）：
     *   1. 解码器本该产出帧（见上：播放态或 seek 在途）；
     *   2. mVideoDecodeRetrySeen：解码器连自己的输入队列都满了（sendPacket 返回
     *      STATUS_RETRY_IN）。包明明在这里，却塞不进去 —— "不收输入"；
     *   3. mVideoFrameQue.empty()：**一帧都没产出**（不是"慢"，是"无"）；
     *   4. 解码器有效、且渲染门没有在拦截（isRenderGateHit：暂停帧恢复那条路会
     *      故意把帧挡在 codec 里，那时"没帧"是设计如此，必须排除）；
     *   5. 上面四条连续成立 DECODE_STALL_REBUILD_ROUNDS 轮。
     *
     * 动作：走**已存在**的错误驱动路径 rebuildVideoDecoder(false)
     *   （invalidateDecoder + CreateVideoDecoder，硬解失败自动落软解，主流做法；
     *   预算与错误路径共用 mVideoDecodeRebuildCount，上限 MAX_VIDEO_DECODER_REBUILDS，
     *   出帧后自动清零 —— 绝不无限重试）。
     *   冷却不是计时器，而是两个状态闩：mDecodeStallRebuildDone（这次卡死只重建一次）
     *   与 mVideoDecodeRebuildCount（重建预算），两者都在 FillVideoFrame() 收到帧时
     *   一起复位 —— 也就是"出帧即自愈"，没有任何周期性动作。
     *   重建成功后把 mSeekDecodeStartIsKey 归假：
     *   新解码器必须重新从关键帧起步，否则输出缺参考帧就是花屏（与 seek 起点的处理一致）。
     *
     * 明确不做的事：不 FlushVideoPath（红线）、不动音频、不动主时钟、不丢包追赶。
     */
    /*
     * 解码器本该在产出帧的状态：播放中且没在缓冲（缓冲态本来就整条管线停着），
     * 或者 seek 在途（seek 期间解码器同样可能卡死，那时更要救）。
     */
    const bool decodeShouldProduce = mAppStatus != APP_BACKGROUND && !mEof &&
        (mSeekFlag || (mPlayStatus == PLAYER_PLAYING && !mBufferingFlag));

    if (HAVE_VIDEO && decodeShouldProduce && !mDecodeStallRebuildDone &&
        mPlayStatus != PLAYER_PREPARING) {
        IDecoder *videoDecoder = mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);
        const bool stalledWithoutOutput =
            mVideoDecodeRetrySeen && videoDecoder != nullptr &&
            mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO) &&
            mVideoFrameQue.empty() && videoDecoder->isRenderGateHit() &&
            mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO) > 0;

        if (stalledWithoutOutput) {
            if (++mDecodeStallIters >= DECODE_STALL_REBUILD_ROUNDS) {
                /* 闩住：这次卡死只做一次，无论下面的重建成功与否，都不再重复进入
                 *（出帧时 FillVideoFrame 会把这个闩与重建预算一起复位）。 */
                mDecodeStallRebuildDone = true;
                mDecodeStallIters = 0;
                mVideoDecodeRetrySeen = false;

                if (mVideoDecodeRebuildCount < MAX_VIDEO_DECODER_REBUILDS) {
                    ++mVideoDecodeRebuildCount;
                    AF_LOGW("video decoder accepts no input and produced no frame "
                            "(retry rounds=%d, frameQ empty, packetQ=%d, playStatus=%d, seekFlag=%d) "
                            "— rebuilding it once (attempt %d/%d, release + init with hw->sw fallback)\n",
                            (int) DECODE_STALL_REBUILD_ROUNDS,
                            (int) mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO),
                            (int) mPlayStatus, (int) mSeekFlag,
                            (int) mVideoDecodeRebuildCount, (int) MAX_VIDEO_DECODER_REBUILDS);

                    if (rebuildVideoDecoder(false) >= 0) {
                        /*
                         * 新解码器是干净状态，必须重新从关键帧起步：闩归假后
                         * DecodeVideoPacket 会重新闩关键帧，渲染侧在闩为真之前不接受落点帧
                         * —— 最多再等一个 GOP，绝不把缺参考帧的脏帧显示出去。
                         */
                        mSeekDecodeStartIsKey = false;

                        videoDecoder = mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);
                        if (videoDecoder != nullptr) {
                            videoDecoder->clean_error();
                        }
                    }
                } else {
                    AF_LOGW("video decoder accepts no input and produced no frame, but the "
                            "rebuild budget is exhausted (attempts=%d/%d) — not rebuilding\n",
                            (int) mVideoDecodeRebuildCount, (int) MAX_VIDEO_DECODER_REBUILDS);
                }
            }
        } else {
            mDecodeStallIters = 0;
        }
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

                        /*
                         * ============ S3：暂停态预滚起点改成"暂停点之前最近的关键帧"============
                         *
                         * 门3 的死结在这里：上面这个参考点是**主时钟**（或切换请求时的
                         * 主时钟），而暂停态主时钟冻结，预滚只会接受"参考点**之后**的第一个
                         * 关键帧"。暂停时用户就停在参考点上，后面那个关键帧要等 demuxer 再读
                         * 一个 GOP 才到；可暂停态没人消化队列，读前闸门（doReadPacket 里
                         * 那一大段）会把读取停住 —— 于是"等关键帧"和"没人读包"互相等，
                         * 实测 pendingPktQ=120 钉死、`[switch]` 28.6 秒不变。
                         *
                         * 暂停态没有"必须贴着时钟"这个约束：用户已经停住了，起点早一点
                         * （多解一个 GOP）完全无害，画面换完还是停在同一个位置。所以把参考点
                         * 降到"暂停点之前（含）最近的那个关键帧"——那个关键帧通常**已经在
                         * pending 队列里**，下一个包就能满足 `packetTimePos >= ref`，预滚
                         * 立刻完成，不需要再读一个 GOP。
                         *
                         * 查不到（目标 representation 的关键帧还没读进来，返回 INT64_MIN）
                         * 就保持原参考点不动：下一次包到来时再查 —— 这时**不会死锁**，
                         * 因为上面的 waitedTooLong 兜底仍然有效，而 S9 的提交前墙钟死线
                         * （6s）保证最坏情况也有终态。
                         *
                         * 播放态：只有在本后端"帧不带 timePosition"（无法自报节目时间轴）时
                         * 才按同一规则降参考点，见下面 B6 那段；带 timePosition 的后端
                         * （Qt / macOS / iOS）完全不受影响。
                         */
                        /*
                         * 【B6：播放态也要降参考点 —— 仅当后端"帧不带 timePosition"】
                         *
                         * 原来只有暂停态降参考点（下面保留的注释解释暂停态的死结）。播放态等
                         * "参考点之后的关键帧"的代价是：新档内容整体领先主时钟 lead 秒
                         * （本片源 4.5~9 s）。带 timePosition 的后端由提交门把它挡住
                         * （等价于等时钟）；**不带**的后端会走"重命名到主时钟"的备用偏移
                         * ⇒ 恒定的音画错位（用户："切完清晰度就错位"）。
                         *
                         * 所以：只在后端**无法用帧自报位置**时，把播放态的参考点也降到
                         * "参考点之前最近的关键帧" —— pending 解码器从前缀解起，由相邻的
                         * "切换窗丢帧"把参考点之前的帧丢掉，提交时手上就有贴着主时钟的帧
                         * （按构造对齐）。代价是最多一个 GOP 的前缀解码。
                         *
                         * 带 timePosition 的后端（Qt / macOS / iOS）**逐字不变**：这个条件恒假，
                         * 走的还是原来那条"等参考点之后的关键帧"的路。
                         */
                        const bool needAlignmentByConstruction = !mActiveDecoderFramesCarryTimePosition;
                        int64_t prerollLowerPivotUs =
                                (mSwitchStartedWhilePaused && mPausedSwitchPivotUs > 0)
                                ? mPausedSwitchPivotUs
                                : (needAlignmentByConstruction ? mPendingVideoPrerollRefUs : INT64_MIN);

                        /*
                         * ============ 【B12：两路代价取小（只在预滚入口选一次）】============
                         *
                         * 预滚起点有两条路，各有代价：
                         *   · 等参考点**之后**第一个关键帧：不解码，代价 = 墙钟 nextKey - ref
                         *     （≤ 一个关键帧间隔）；
                         *   · 从参考点**之前**最近的关键帧起解、丢前缀追上时钟：代价 = 解码
                         *     (ref - lastKey) 这么长的一段，按 (rate - 1) 倍实时追赶。
                         * 换档点刚过关键帧时（gap 很小）追帧远快于等下一个关键帧；反之等关键帧更省。
                         *
                         * 只在**播放态且帧带 timePosition**（prerollLowerPivotUs == INT64_MIN，
                         * 即今天固定走"等关键帧"的那条路）时判一次：
                         *   · 暂停态与"帧不带 timePosition"的后端本来就固定走追帧（S3 / B6），
                         *     没有可选项；
                         *   · mPendingVideoPrerollPathChosen 保证只判一次（每次切换请求处复位），
                         *     否则包队列一前进就改主意，会和"已经丢掉的前缀"打架。
                         *
                         * nextKey / lastKey 都从**当前视频包队列**查：DASH 的 segmentAlignment 与
                         * HLS 的 dashenc 让各档 IDR 落在同一网格上，所以这个估计对目标档成立；
                         * 万一不成立也只是"选得不最优"—— 提交门"早于目标就丢、晚于目标就等"仍然
                         * 兜住正确性，**不会降低精度**。
                         */
                        if (prerollLowerPivotUs == INT64_MIN && !mPendingVideoPrerollPathChosen &&
                            !mPendingVideoPrerollDone && mPendingVideoPrerollRefUs != INT64_MIN) {
                            const int64_t refForChoiceUs = mPendingVideoPrerollRefUs;
                            const int64_t nextKeyUs =
                                    mBufferController->GetFirstKeyPTSAfter(BUFFER_TYPE_VIDEO, refForChoiceUs);
                            const int64_t lastKeyUs =
                                    mBufferController->GetKeyTimePositionBefore(BUFFER_TYPE_VIDEO, refForChoiceUs);

                            if (nextKeyUs != INT64_MIN && nextKeyUs > refForChoiceUs &&
                                lastKeyUs != INT64_MIN && lastKeyUs < refForChoiceUs) {
                                const int64_t waitNextKeyUs = nextKeyUs - refForChoiceUs;
                                const int64_t gapFromLastKeyUs = refForChoiceUs - lastKeyUs;
                                /* rate 是千分比；净追赶速度 = (rate - 1) 倍实时，至少按一点点算。 */
                                const int64_t excessRateMilli = std::max(1, mPendingVideoDecodeRateMilli - 1000);
                                const int64_t catchUpUs = gapFromLastKeyUs * 1000 / excessRateMilli;
                                const bool chooseCatchUp = catchUpUs < waitNextKeyUs;

                                if (chooseCatchUp) {
                                    prerollLowerPivotUs = lastKeyUs;
                                }

                                mPendingVideoPrerollPathChosen = true;

                                AF_LOGI("quality switch preroll: two candidate paths — wait for the next key frame "
                                        "%lld ms (nextKey=%lld ref=%lld) vs catch up from the previous key frame "
                                        "%lld ms (lastKey=%lld gap=%lld ms decodeRate=%d/1000) → choosing %s\n",
                                        (long long) (waitNextKeyUs / 1000),
                                        (long long) nextKeyUs, (long long) refForChoiceUs,
                                        (long long) (catchUpUs / 1000),
                                        (long long) lastKeyUs, (long long) (gapFromLastKeyUs / 1000),
                                        mPendingVideoDecodeRateMilli,
                                        chooseCatchUp
                                        ? "catch-up (decode the previous key frame's GOP, drop the prefix)"
                                        : "wait for the next key frame (no prefix decode)");
                            }
                        }

                        if (prerollLowerPivotUs != INT64_MIN && !mPendingVideoPrerollDone) {
                            const int64_t lastKeyBeforePivot =
                                    mBufferController->GetKeyTimePositionBefore(BUFFER_TYPE_VIDEO, prerollLowerPivotUs);

                            if (lastKeyBeforePivot != INT64_MIN &&
                                lastKeyBeforePivot < mPendingVideoPrerollRefUs) {
                                AF_LOGI("pre-roll: reference lowered from %lld to the last key frame at or before "
                                        "%lld (timePosition=%lld) — %s; the pending decoder will reach the switch "
                                        "point by decoding (and dropping) the prefix, instead of its frames being "
                                        "renamed onto the master clock (which is what caused the A/V offset)\n",
                                        (long long) mPendingVideoPrerollRefUs,
                                        (long long) prerollLowerPivotUs,
                                        (long long) lastKeyBeforePivot,
                                        mSwitchStartedWhilePaused
                                        ? "paused switch: the frozen pivot can never get a later key frame"
                                        : (needAlignmentByConstruction
                                           ? "this backend's output frames carry no timePosition"
                                           : "adaptive: catching up from the previous key frame is cheaper than "
                                             "waiting for the next one"));
                                mPendingVideoPrerollRefUs = lastKeyBeforePivot;
                            }
                        }

                        const bool waitedTooLong = mPendingVideoSwitchStartMs > 0 &&
                                                   (af_getsteady_ms() - mPendingVideoSwitchStartMs) >
                                                       PENDING_PREROLL_WAIT_MAX_MS;

                        /*
                         * 【B6】先记下"这一档的 raw pts → 全局 timePosition"的恒定差。
                         *
                         * 目标档每个包都带 timePosition（DASH/HLS 的 time2ptsDelta），而
                         * Android 硬解的输出帧**不带**（见 FillPendingVideoFrame 里的说明），
                         * 所以对那个后端来说，这个差是后面唯一能把帧放回节目时间轴的依据。
                         * 与 seek 落点（B3）用的是同一个式子：timePosition - pts。
                         *
                         * 只在本后端"帧不带 timePosition"时记录：Qt/macOS/iOS 的帧自带
                         * timePosition，那里的归一化（FillPendingVideoFrame 按帧算偏移）
                         * 逐字不变，两种来源不打架。
                         */
                        if (!mActiveDecoderFramesCarryTimePosition &&
                            packetPts != INT64_MIN && packetTimePos >= 0) {
                            mPendingVideoPtsOffset = packetTimePos - packetPts;
                        }

                        /*
                         * ============ 【B7】参考点**之前**的关键帧：直接从这里起解 ============
                         *
                         * 真机（2026-09-25 13:00，DASH 分片 10 s、IDR 在分片起点）：
                         * 切换参考点 ref=1.018 s，而"参考点之后的第一个关键帧"在 10.0 s
                         * ⇒ lead=8.98 s。旧行为把 0.0 s 那个关键帧（离参考点只有 1 s）当
                         * "预滚跳过"丢掉，于是 pending 帧全部落在参考点**之后**：
                         *   · 做了轴修正（B6）⇒ 帧落在正确的节目轴上，提交门的"未来帧"判据
                         *     （:5605 `pts > master + PENDING_FUTURE_TOLERANCE_US`）把提交挡住，
                         *     一直等时钟爬到 10.0 s（8.98 s）⇒ 超过切档死线 ⇒
                         *     `target rendition did not reach playback timeline`（切换失败）；
                         *   · 不做轴修正 ⇒ 走"重命名到主时钟"的备用偏移 ⇒ 恒定音画错位。
                         * 两条路都不对，根因是"从参考点之后的 IDR 起解"这个选择本身。
                         *
                         * 正确做法：从参考点**之前**最近的关键帧起解（暂停态早就是这么做的，
                         * 见上面 S3 那段）。解掉 [关键帧, 参考点] 这段前缀，前缀帧由切换窗 /
                         * 渲染器追赶窗丢掉，提交时手上就有贴着主时钟的帧 ⇒ 既不错位也不等时钟。
                         * 代价被一个 IDR 间隔封顶（本例只有 1 s），而且与下载重叠。
                         *
                         * 触发条件只是"这是个参考点之前的关键帧"（纯状态判断，无计时器）；
                         * 一旦被接受，mPendingVideoPrerollDone 置真，本块不会再跑。
                         */
                        if (isKeyFrame && packetTimePos < mPendingVideoPrerollRefUs) {
                            AF_LOGI("pending preroll: starting at the key frame BEFORE the reference "
                                    "(keyTimePosition=%lld ref=%lld prefix=%lld ms) instead of waiting for the "
                                    "next one — waiting would push every pending frame past the clock and either "
                                    "delay the commit past its deadline or force the clock-relative rename that "
                                    "caused the A/V offset\n",
                                    (long long) packetTimePos, (long long) mPendingVideoPrerollRefUs,
                                    (long long) ((mPendingVideoPrerollRefUs - packetTimePos) / 1000));
                            mPendingVideoPrerollRefUs = packetTimePos;
                        }

                        if (packetTimePos < mPendingVideoPrerollRefUs) {
                            if (!isKeyFrame || !waitedTooLong) {
                                if (floodLogAllowed(FLOOD_STALE_PENDING, 2, "skip pending preroll packet")) {
                                    AF_LOGD("skip pending preroll packet pts=%lld timePosition=%lld ref=%lld\n",
                                            (long long) packetPts, (long long) packetTimePos,
                                            (long long) mPendingVideoPrerollRefUs);
                                }

                                /*
                                 * 【马赛克排查，2026-09-24】
                                 *
                                 * 这里丢掉的包**可能正带着 codec 参数集**（框架是在目标流
                                 * 第一个包上 setExtraData 的）。丢它本身没问题 —— 参数集
                                 * 会在真正送进解码器的那个包上重新贴（见下面两条接受分支
                                 * 里的 attachPendingVideoCodecParams()）；但万一哪天又有
                                 * 一条新分支忘了贴，这条 warning 就能直接指出"参数集是
                                 * 在预滚跳过这一步丢的"，不必从马赛克反推。
                                 */
                                if (mVideoPacket->getInfo().extra_data != nullptr &&
                                    mVideoPacket->getInfo().extra_data_size > 0) {
                                    AF_LOGW("preroll skip dropped a packet that carried the codec params "
                                            "(timePosition=%lld) — the parameter set is re-attached to the "
                                            "first packet actually sent to the pending decoder; if the switch "
                                            "output looks like mosaic, check that this re-attachment happened\n",
                                            (long long) packetTimePos);
                                }

                                mVideoPacket.reset();
                                continue;
                            }

                            /* 等太久了：接受这个更早的关键帧，代价是多解一个 GOP。 */
                            mPendingVideoPrerollDone = true;
                            /* 诊断：记住预滚实际起解的那个关键帧（raw pts），交接后用
                             * `post-handover first frame` 一行判定"上屏的是不是这一代"。 */
                            mPendingVideoPrerollKeyPts = packetPts;
                            AF_LOGW("pending preroll waited %lld ms without a key frame after the reference, "
                                    "falling back to an earlier key frame: timePosition=%lld ref=%lld\n",
                                    (long long) (af_getsteady_ms() - mPendingVideoSwitchStartMs),
                                    (long long) packetTimePos, (long long) mPendingVideoPrerollRefUs);

                            /*
                             * 【马赛克根因修复，2026-09-24】
                             *
                             * 这条回退分支以前**不贴参数集**，而它是"等不到参考点
                             * 之后的关键帧"时才走的路径 —— 恰恰是最可能已经把
                             * "携带 extradata 的那一条包"丢掉的情形。日志里两处
                             * 坏包簇（-1094995529 AVERROR_INVALIDDATA，
                             * 22:51:00.790 / 22:52:09.646）就紧跟在这条日志之后：
                             * 新解码器缺 SPS/PPS → P 帧参考不上 → 人物糊成马赛克块。
                             *
                             * 现在与"正常关键帧"分支共用同一个函数，两条路都不可能漏。
                             */
                            if (attachPendingVideoCodecParams()) {
                                AF_LOGI("pending preroll fallback attached codec params to the first packet "
                                        "(timePosition=%lld) to avoid a parameter-set-less first frame\n",
                                        (long long) packetTimePos);
                            }
                        } else if (isKeyFrame) {
                            mPendingVideoPrerollDone = true;
                            /* 诊断：记住预滚实际起解的那个关键帧（raw pts）—— 见上面
                             * 回退分支同样的说明，以及 RenderVideo 里
                             * `post-handover first frame` 那条日志。 */
                            mPendingVideoPrerollKeyPts = packetPts;
                            /*
                             * 【B5-3：这一行的坐标轴必须标清楚】
                             * `keyRawPts` 是**包的原始 pts 轴**；`keyTimePosition` / `ref` /
                             * `master` 是**位置轴（主时钟轴）**；`lead = keyTimePosition - ref`
                             * 也只有这两个值同轴。
                             * 以前写成 `pts=… timePosition=… ref=… master=… (lead=…)`，读日志的人
                             * 很容易把 pts 与 lead 当成同一把尺子 —— 真机 2026-09-25 那份
                             * `pts=145979167 timePosition=140000000 ref=135470098 (lead=4529 ms)`
                             * 就是这么被误读的。
                             * lead 大只说明"参考点之后最近的关键帧离参考点很远"（本片源 IDR
                             * 间隔 5~11 s，是内容属性，不是选择逻辑出错）；再补一个"参考点之前
                             * 最近的关键帧"，一眼就能看出另一个候选离参考点有多远。
                             */
                            const int64_t keyBeforeRef =
                                    mBufferController->GetKeyTimePositionBefore(BUFFER_TYPE_VIDEO,
                                                                               mPendingVideoPrerollRefUs);
                            char keyBeforeText[64] = {0};

                            if (keyBeforeRef == INT64_MIN) {
                                snprintf(keyBeforeText, sizeof(keyBeforeText),
                                         "none (not among the buffered packets)");
                            } else {
                                snprintf(keyBeforeText, sizeof(keyBeforeText), "%lld us",
                                         (long long) keyBeforeRef);
                            }

                            AF_LOGI("pending preroll starts at key frame: keyRawPts=%lld (raw pts axis) "
                                    "keyTimePosition=%lld ref=%lld master=%lld (position/master axis) "
                                    "lead=%lld ms (= keyTimePosition - ref) prevKeyTimePosition=%s\n",
                                    (long long) packetPts, (long long) packetTimePos,
                                    (long long) mPendingVideoPrerollRefUs, (long long) masterPts,
                                    (long long) ((packetTimePos - mPendingVideoPrerollRefUs) / 1000),
                                    keyBeforeText);

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
                             *
                             * 【2026-09-24】这段已抽成 attachPendingVideoCodecParams()，
                             * 与上面那条"等太久回退"分支共用同一份实现 —— 两条路本来
                             * 只有一条贴了参数集，这正是马赛克（-1094995529）的来源。
                             */
                            attachPendingVideoCodecParams();
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
                        /*
                         * 【观测（纯日志，零行为改动）：pending 路的背压】
                         *
                         * pending 解码器回 RETRY_IN 就是"目标路吃不下"的唯一信号
                         * （ActiveDecoder 在输入队列 >= 16 或输出队列 >= 10 时才回它）。
                         * 这一行与每秒一行的 `[switch]` 行、以及
                         * `drop pending frame already behind the playback position`
                         * 那行配合，一次实测就能分清三种"每轮只前进一个包"：
                         *   · 这里频繁出现 + pendingFrameQ 停在 4 ⇒ 背压（本文件已按
                         *     "落后播放位置的帧在抽取处就丢"修掉了一半，剩下的就是
                         *     "帧没落后、但主时钟还没走到"这份正常等待）；
                         *   · 这里不出现 + pendingPktQ 为 0 ⇒ 包没到（读闸门/网络）；
                         *   · 这里不出现 + 队列都不空 ⇒ 解码/渲染本身是瓶颈。
                         *
                         * 用 FLOOD_PENDING_HOLD 这个既有 id（"pending 路在等"是同一
                         * 族语义）：这样 FloodLogId 枚举与 mFloodLog[] 的大小都不变，
                         * 不会挪动类里任何成员的位置（增量构建 ABI 安全）。
                         */
                        if (floodLogAllowed(FLOOD_PENDING_HOLD, 2, "pending decoder back-pressure (RETRY_IN)")) {
                            AF_LOGI("pending decoder back-pressure (RETRY_IN): pts=%lld timePosition=%lld "
                                    "pendingPktQ=%d pendingFrameQ=%d frameQ=%d pendingBurst=%d prerollDone=%d "
                                    "elapsed=%lld ms\n",
                                    (long long) (mVideoPacket != nullptr ? mVideoPacket->getInfo().pts : INT64_MIN),
                                    (long long) (mVideoPacket != nullptr ? mVideoPacket->getInfo().timePosition : INT64_MIN),
                                    (int) mPendingVideoPacketQue.size(), (int) mPendingVideoFrameQue.size(),
                                    (int) mVideoFrameQue.size(), pendingBurst, (int) mPendingVideoPrerollDone,
                                    (long long) (mPendingVideoSwitchStartMs > 0
                                                     ? af_getsteady_ms() - mPendingVideoSwitchStartMs : -1));
                        }

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

        /*
         * 音频解码队列下限按"帧数"给足抗抖动余量（主流做法：ffplay 的音频 PCM 队列是 9 帧）。
         * 原来只留 2 帧（约 40ms），4K 解码突发或网络抖动一来就欠载 —— 这也是
         * "seek 之后声音不稳 / 起播咔哒"的通用原因（见方案 2.1）。
         */
        while (mAudioFrameQue.size() < AUDIO_FRAME_QUEUE_FLOOR && !audioDecoderEOS && !mCanceled) {

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
        /*
         * ============ 【B9】"视频原始 pts → 节目轴"的偏移必须**跟着分段走** ============
         *
         * 真机证据（2026-09-25 13:47，DASH `output.mpd`，8 档）：同一个档每次 seek 落点报出来的
         * 两轴差（`seek landing: … -N ms away …`）分别是
         *     −1708 / −3416 / −4697 / −5979 / −7260 ms（落点 40/80/110/140/170 s）
         * ⇒ **每 10 s 分片差 ~1.7 s**，也就是说这一档的"原始 pts 轴"与节目轴的**速率都不一样**
         * （≈17%）。而 HLS/顺序片源那几行是 `0 ms away`（差恒为 0）⇒ 天生免疫。
         *
         * 危害：偏移只在两个时刻被建立（seek 落点=B3、切档提交），之后**冻结**。于是
         * 跨过一个分片后，帧被平移到错误的节目位置，视频相对音频每秒偏 ~170 ms ——
         * "seek 之后音画偏差越来越大、最后完全对不上"，DASH 必现、HLS 正常。
         *
         * 处置：**每个视频包**都按它自己的 `timePosition − pts` 刷新这个偏移（包级元数据本来就有）。
         * 只看**当前 active 流**的包（切档在途时流进来的包属于另一档，delta 不同，不能用来改 active 的轴）；
         * 只在变化超过 100 ms 时才刷新并打一行日志（分片边界一次，事件驱动、不刷屏）。
         *
         * 关于"跨边界那一批帧"：偏移是在 FillVideoFrame（帧出解码器时）应用的，所以刷新前
         * 已出队的那几张帧仍带旧轴，最多几帧（<0.1~0.2 s）会偏一次 —— 相比"永远偏下去"
         * 这是可接受的折中。要做到逐帧精确，需要把每个帧自己的 timePosition 从 Android
         * 硬解后端带出来（mediaCodecDecoder 里那个 TODO），属于下一步。
         */
        if (mPendingVideoStreamIndex < 0 && mWillChangedVideoStreamIndex < 0 &&
            pVideoPacket->getInfo().streamIndex == mCurrentVideoIndex) {
            const int64_t packetPtsForAxis = pVideoPacket->getInfo().pts;
            const int64_t packetPosForAxis = pVideoPacket->getInfo().timePosition;

            if (packetPtsForAxis != INT64_MIN && packetPosForAxis >= 0) {
                const int64_t freshOffset = packetPosForAxis - packetPtsForAxis;
                const int64_t previousOffset = mActiveVideoPtsOffset;
                const bool logThisRefresh = (previousOffset == INT64_MIN ||
                                             llabs(freshOffset - previousOffset) > 500 * 1000);

                /*
                 * ============ 【B9-2：偏移**逐包跟随**，不再用 100 ms 阈值】============
                 *
                 * 真机日志（2026-09-25 14:00，DASH `output.mpd`）：
                 *   `video pts axis refreshed … rawPts=83416667 timePosition=80000000
                 *    offset=-3416667 (was -2989583, corrected by -427 ms)`
                 * ⇒ 这一档每 10 s 分片的两轴差要修 **427 ms**（原始 pts 轴比节目轴快 4.27%），
                 * 而 HLS/顺序片源同一行是 `offset=-2`（差恒为 0）—— 这就是"DASH 才有、
                 * HLS 没有"的全部原因。
                 *
                 * 上一版用"变化超过 100 ms 才刷新"来省日志，代价是修正被攒成台阶：
                 * 归一化后的帧 pts 相对节目轴呈锯齿，残余误差可达 ±100 ms —— 用户看到的
                 * "DASH 还是有点不同步"就是这一截。
                 *
                 * 现在**每个包都赋值**（这是逐帧正确性的关键）：帧出解码器（FillVideoFrame）
                 * 时用的是"离它最近的那个包的偏移"，误差 = 解码滞后 × 4.27% ≈ 几毫秒，
                 * 于是 DASH 的紧密度与 HLS（差值恒 0）完全一致。
                 * 只有**日志**保留 500 ms 阈值（大约每分片一行），不会刷屏。
                 */
                mActiveVideoPtsOffset = freshOffset;

                /*
                 * B10：把这一包的 (pts → timePosition) 也记进逐帧配对表（见 .h 里的说明）。
                 * 帧与包同序、只为 active 流记录、上限 64 ⇒ 内存与查找成本都可以忽略。
                 */
                mVideoAxisPts.push_back(packetPtsForAxis);
                mVideoAxisTimePos.push_back(packetPosForAxis);

                while (mVideoAxisPts.size() > 64) {
                    mVideoAxisPts.pop_front();
                    mVideoAxisTimePos.pop_front();
                }

                if (logThisRefresh) {
                    AF_LOGW("video pts axis refreshed from the packet: stream=%d rawPts=%lld timePosition=%lld "
                            "offset=%lld (was %lld, corrected by %lld ms) — a DASH rendition whose raw pts axis "
                            "does not advance at the program rate would otherwise drift out of sync after every "
                            "segment boundary\n",
                            pVideoPacket->getInfo().streamIndex, (long long) packetPtsForAxis,
                            (long long) packetPosForAxis, (long long) freshOffset,
                            (long long) (previousOffset == INT64_MIN ? 0 : previousOffset),
                            (long long) ((previousOffset == INT64_MIN ? 0 : (freshOffset - previousOffset)) / 1000));
                }
            }
        }

        // for cache video, or seeking accurate, check whether drop output frame
        /*
         * 落点帧即上屏（K1）之后，默认路径不再把"目标点之前的包"标成丢弃 —— 否则
         * 落点帧本身（它就在目标点之前）会被丢掉，画面出不来。
         * 只有 mSeekExactLanding（旧语义）才继续按目标点丢包；dropLateVideoFrames
         * 是"已上屏之后的迟到帧"策略，与 seek 无关，保持不变。
         */
        if ((mSeekExactLanding && mSeekNeedCatch) || dropLateVideoFrames) {
            int64_t checkPos = (mSeekExactLanding && mSeekNeedCatch) ? mSeekPos.load() : pos;

            // only decode and don't need output to render if too old
            if ((pVideoPacket->getInfo().timePosition < checkPos) && (pVideoPacket->getInfo().timePosition < mDuration - 200 * 1000)) {
                pVideoPacket->setDiscard(true);
            }
        }

        /*
         * 花屏（马赛克）修复：记下"这一轮 seek 有没有从关键帧起步解码"。
         *   解码必须从关键帧（IDR）起步，否则输出缺参考帧 → 画面是大块脏数据；
         *   旧代码靠"渲染地板"恰好把这类帧挡住，本轮改成"落点帧即上屏"后必须显式判断。
         * AF_PKT_FLAG_KEY(0x0001) 的判据与本文件 :3406 的 isKeyFrame 一致（flags != 0）。
         * 关键帧包一送到解码器就置真；渲染侧只有在这个闩为真时才允许接受落点帧，
         * 否则继续等下一个关键帧 —— 最多等一个 GOP，绝不把脏帧显示出去。
         */
        if (!mSeekLandingFrameAccepted && pVideoPacket->getInfo().flags != 0) {
            if (!mSeekDecodeStartIsKey) {
                AF_LOGI("seek decode starts at a keyframe: pts=%lld flags=%d\n",
                        (long long) pVideoPacket->getInfo().timePosition, pVideoPacket->getInfo().flags);
            }

            /*
             * ============ C 方案：把音频也对齐到同一个落点（一次性，事件驱动）============
             *
             * 事件就是"读到了本次 seek 的第一个关键帧视频包"——此刻落点 PTS 已知
             * （= 这一包的 timePosition）。此时做三件事，全部只做一次：
             *   1. 丢掉落点之前的音频包（保留落点及之后的）；
             *   2. 记下"音频落点地板"mSeekAudioFloorUs：按旧起点已经解出来、还压在
             *      mAudioFrameQue 里的音频帧，在推送给音频设备那一刻被丢掉（见 RenderAudio），
             *      首帧落点音频真正上设备后地板清零；
             *   3. **把整条音频时间轴对齐到落点**（1b）：清空已解码帧队列，并把
             *      mPlayedAudioPts / mAudioTime 重置到落点。
             *
             * 第 3 条是必须的，而且只靠"地板"不够 —— 实测（Qt 日志）：
             *   `seek audio aligned … landing=20833333 dropped=40 audioFront=20842667`
             *   （音频包队列**确实**已经对齐到落点 20.843s）
             *   186ms 后却出现
             *   `correct audio and master clock offset is 8789339` →
             *   `TIMEPOS reSync time 20888010 to  29674671` → 约 230 帧连续丢帧。
             * 原因：地板只丢"**低于**落点"的帧，而 seek 前按旧时间轴解出来、**高于**落点
             * 的那批帧（本例≈29.6s，即用户按下 seek 时的播放位置）留在了 mAudioFrameQue
             * 里，被推给设备 → 音频参考时钟立刻报 29.674s → "音频是主"就把主时钟从落点
             * 一步拽到 29.674s → 视频永久欠 8.8s 的债 → 丢帧风暴（安卓上同一根因表现为
             * 落点帧被绕过、时钟停在目标点、画面冻死）。
             * 所以这里连**已解码帧队列**一起清掉，并把音频时间轴重新落到落点。
             *
             * 为什么**不**在这里 FlushAudioPath()：那会在 seek 窗口内对音频设备做第二次
             * flush（第一次是 seek 开始时的那次），实测紧随其后 0.5s 就出现
             * onAudioException -2103466313 —— 设备被清空后没有数据可播，外层状态机
             * 判定异常。这里的做法达到同样的时间轴对齐效果，却完全不碰设备、不重置
             * 音频解码器（音频解码是有状态的，不 flush 反而避免了解码断点杂音）。
             * 清帧队列与重置时间轴都是本进程内的状态操作，不产生任何设备调用。
             *
             * 失败安全：音频队列里没有落点及之后的数据（front 为空/异常）时**什么都不做**，
             * 直接回落 A（音频等时钟，最多一个 GOP 静音）——绝不因此让音频永不开始或让播放停住。
             * 只碰音频路，不碰视频路（红线里的"禁播放中 FlushVideoPath"不受影响）。
             */
            if (!mSeekAudioAlignDone && HAVE_AUDIO) {
                const int64_t landingUs = pVideoPacket->getInfo().timePosition;
                const int64_t audioFront = mBufferController->GetPacketPts(BUFFER_TYPE_AUDIO);

                mSeekAudioAlignDone = true;

                /*
                 * ============ 【B13：音频的对齐锚点必须与时钟锚点是同一个点（目标）】============
                 *
                 * 实测（用户日志，暂停态 seek）：时钟被钉在**目标**（mSeekPositionFloorUs），
                 * 而音频的裁剪/地板/时间轴重基都按**视频落点**（landingUs）来做 —— 两者相差
                 * "落点到目标"这一段，于是出现：
                 *   · 音频队列里保留着 [落点, 目标) 这一段的帧；
                 *   · render() 里"pcm 队首比时钟超前就按住"那道门把它们按住；
                 *   · 暂停态时钟是 pause 的（ProcessPauseMsg → startRendering(false) →
                 *     mMasterClock.pause()），它**不可能"追上来"**，门就永远不开 —— 音频不流动
                 *     ⇒ 以音频为参考的时钟也不动 ⇒ 视频落点帧的渲染门永远不满足 ⇒
                 *     画面与进度条都不动（fps=0，实测 frameQ=1 而 master 已越过落点）。
                 *
                 * 这里把音频的对齐锚点统一到**时钟锚点**上：目标已知且不早于落点时，按目标
                 * 裁剪/重基（丢掉落在目标之前的音频，包括 [落点, 目标) 那一段）。第一帧音频于是
                 * 落在目标附近（一个音频帧的量级），上面那道门自然不成立，不需要"等时钟追上"。
                 *
                 * 通用性：这里只比较两个**位置值本身**，与 GOP、关键帧间距、分片长度都无关。
                 * 精度不变：视频首帧仍由"包含目标、不晚于目标"的采纳块决定，本处只动音频。
                 * 目标未知（INT64_MIN）或目标早于落点时，锚点退回落点 —— 与今天逐字一致。
                 */
                const int64_t audioAnchorUs =
                        (mSeekPositionFloorUs != INT64_MIN && mSeekPositionFloorUs > landingUs)
                        ? mSeekPositionFloorUs : landingUs;

                /*
                 * 【预算判据已删除（精准 seek 第 2 项）】
                 *
                 * 这里原来按"落点距目标是否在 SEEK_EXACT_LANDING_BUDGET_US 内"来**授予**
                 * 精确落点（mSeekExactLandingByBudget = true）。那是"精度换等待"：精度取决于
                 * 预算给不给，而不是取决于目标本身 —— 落点比目标早超过预算时就直接把落点帧
                 * 放上屏（实测该文件早 1.0~11.2 秒），这正是 DASH/HLS/稀疏 IDR 上"seek 不精准"。
                 *
                 * 现在落点判据是**包含目标**（RenderVideo 的落点采纳块：帧区间
                 * [timePosition, timePosition + 帧长) 不含目标就继续解到下一帧），与目标本身
                 * 比较、与落点距离无关，所以不再需要"授予"这一步：
                 * mSeekExactLandingByBudget 从此恒为假 —— 预算这条路径被彻底关掉。
                 * 这里只留一条"落点离目标多远"的日志，便于现场核对，与精度无关。
                 */
                if (mSeekPositionFloorUs != INT64_MIN && landingUs > 0 &&
                    mSeekPositionFloorUs > landingUs) {
                    AF_LOGI("seek: the landing keyframe is %lld ms before the target — decoding forward "
                            "to the frame that contains the target (exact landing, no budget)\n",
                            (long long) ((mSeekPositionFloorUs - landingUs) / 1000));
                }

                /*
                 * 【第 4 条补完】对齐动作只依赖"落点已知"，**不依赖音频包队列当时有没有数据**：
                 * 清已解码帧队列、重基时间轴、记落点地板与连续性高水位，全都不需要读包队列。
                 * 原来整块挂在 `audioFront != INT64_MIN` 之下 —— 一旦那一刻音频包队列还是空的
                 * （seek 刚开始、读包还没到），对齐就整块跳过，旧时间轴的已解码帧与解码器
                 * 内部残留就没有任何东西挡得住，等于第 4 条在最需要它的路径上失效。
                 * 现在只有"裁包"这一步需要 audioFront（日志/丢包统计），其余照做。
                 */
                if (audioAnchorUs > 0) {
                    int64_t dropped = 0;

                    if (audioFront != INT64_MIN) {
                        dropped = mBufferController->ClearPacketBeforeTimePos(BUFFER_TYPE_AUDIO, audioAnchorUs);
                    }

                    mSeekAudioFloorUs = audioAnchorUs;
                    /* 第 4 条：连续性高水位从**同一个锚点**起算（详见 SEEK_AUDIO_CONTINUITY_TOLERANCE_US）。 */
                    mSeekAudioContinuityUs = audioAnchorUs;
                    mSeekAudioStaleDrops = 0;

                    /*
                     * ============ B3：seek 落点必须重建"视频原始 pts → 主时钟轴"的映射 ============
                     *
                     * 背景：这条片源的视频**原始 pts 轴**与全局 timePosition 轴差一个固定量
                     * （HLS/DASH 每一路各自算 time2ptsDelta：`timePosition = pts + time2ptsDelta`，
                     * 见 HLSStream 与 DashStream 的包出口）。主时钟与音频走的是 timePosition 轴
                     * （下面就把它重基到 landingUs），而视频的渲染节拍 / 丢弃 / 锚定用的是帧的 pts
                     * （RenderVideo 的 videoLateUs、mPlayedVideoPts）。两条轴之间的映射就是
                     * mActiveVideoPtsOffset；seek 之前它由切档预滚提供，而 FlushVideoPath 为了
                     * 重建时间轴必须把它清零（本文件 FlushVideoPath 里那句
                     * `mActiveVideoPtsOffset = INT64_MIN`）—— 落地之后**没有任何地方把它建回来**。
                     *
                     * 后果（真机日志，两次 seek 的算术各自独立吻合）：落点关键帧的
                     * timePosition=40.0s，而 seek 后第一张上屏的帧 pts=52.135s
                     * ⇒ RenderVideo 里 videoLateUs = 主时钟 40.0 - 帧 52.135 ≈ -12.1s，
                     * 命中"早于主时钟 10ms 以上一律不渲染"那条直接 return
                     * ⇒ 帧全部堆在 mVideoFrameQue 里出不去，codec 的输出缓冲不回收、
                     * 随即不再交付输入缓冲（日志 `codec has had no input buffer` 出现在
                     * 队首冻结之后 ⇒ 它是结果不是原因），读前闸门也按两把不同的尺子比较
                     * （同一时刻报 front 53636917 比 master 40020000 超前 13616 ms，
                     * 真实只有 1.5s）⇒ 整机冻结，直到时钟自己爬过那 12.1 秒。
                     * 同一时刻的第二个后果：位置锚点被钉在 52.135s（mPlayedVideoPts），
                     * 位置回调往前跳一次；归一化之后锚点就是落点本身，不再跳。
                     *
                     * 修法：落点包自己同时带着两个值 —— timePosition（全局时间轴）与 pts（原始轴），
                     * 两者之差正是这一路流的 time2ptsDelta，与切档预滚建立偏移的式子
                     * （mPendingVideoPtsOffset = frameTimePosition - framePts）完全同源。
                     * 复用**既有**成员与**既有**两个消费者：DecodeVideoPacket 给帧做 pts 归一化、
                     * doReadPacket 的读前闸门把包 pts 折算到主时钟同一把尺子上。
                     * 不新增任何状态，不动落点 / 目标语义（mSeekPositionFloorUs、
                     * mSeekRenderGateUs、SEEK_EXACT_LANDING_BUDGET_US 全部不变），
                     * 位置地板照旧保证进度条不回退。
                     *
                     * 保真：pts 与 timePosition 本来就一致的片源算出来是 0，这一句只在两者真的
                     * 不一样时才置位 ⇒ 那些片源的判据逐字不变（mActiveVideoPtsOffset 保持
                     * INT64_MIN，两个消费者都是恒等变换）；解码器原样带出 timePosition 的平台
                     * 也不受影响（只动帧的 pts，不动 timePosition）。所有硬件解码器路径共用
                     * 这一句，没有任何平台分支 ⇒ Qt 走同一条 seek 路径、同向收益。
                     *
                     * 生命周期与切档建立的那条偏移完全一致（见 mActiveVideoPtsOffset 的说明）：
                     * 下一次切档提交由 `mActiveVideoPtsOffset = mPendingVideoPtsOffset` 直接覆盖，
                     * 下一次 seek / stop / 换源由 FlushVideoPath 与 Reset 各自清零。
                     */
                    if (pVideoPacket->getInfo().pts != INT64_MIN && pVideoPacket->getInfo().pts != landingUs) {
                        mActiveVideoPtsOffset = landingUs - pVideoPacket->getInfo().pts;
                        AF_LOGI("seek landing: this stream's raw pts axis is %lld ms away from the timePosition "
                                "axis (landing packet pts=%lld timePosition=%lld) - video frames are normalized "
                                "onto the master clock axis so they are no longer judged too early\n",
                                (long long) (mActiveVideoPtsOffset / 1000),
                                (long long) pVideoPacket->getInfo().pts, (long long) landingUs);
                    }

                    /*
                     * 1b：整条音频时间轴对齐到**上面的锚点**（目标优先，见 B13）。清的是
                     * **本进程内**已解码帧队列，不动设备、不动音频解码器：下一帧音频会按锚点
                     * 重新建立时间轴（mPlayedAudioPts == INT64_MIN 那条既有交接路径），
                     * 因此"设备位置按旧时间轴报数"再也不可能把主时钟拽走。
                     */
                    while (!mAudioFrameQue.empty()) {
                        mAudioFrameQue.pop_front();
                    }

                    mPlayedAudioPts = INT64_MIN;
                    mAudioTime.startTime = audioAnchorUs;
                    mAudioTime.deltaTime = 0;
                    mAudioTime.deltaTimeTmp = 0;
                    mLastAudioFrameDuration = -1;
                    /* 音频包指针也必须丢：它属于 seek 前的旧时间轴。 */
                    mAudioPacket = nullptr;

                    AF_LOGI("seek audio aligned to the CLOCK anchor: landing=%lld anchor=%lld target=%lld "
                            "dropped=%lld audioFront=%lld (audio before the anchor is dropped so the first "
                            "audio frame sits at the target; no device flush, the decoded frames from the "
                            "old timeline are discarded and the audio clock is rebased to the anchor)\n",
                            (long long) landingUs, (long long) audioAnchorUs,
                            (long long) mSeekPositionFloorUs, (long long) dropped,
                            (long long) mBufferController->GetPacketPts(BUFFER_TYPE_AUDIO));
                } else {
                    AF_LOGI("seek audio align skipped (landing=%lld audioFront=%lld) — falling back to "
                            "waiting for the clock\n",
                            (long long) landingUs, (long long) audioFront);
                }
            }

            mSeekDecodeStartIsKey = true;
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
        } else {
            /*
             * 状态签名：解码器连自己的输入队列都满了（包塞不进去）。这不是"没有输入"
             * （包就在这里），是 codec 拿着缓冲不放手 —— 后续由 doRender() 里那条
             * 纯状态判据决定是否走一次错误驱动的重建。
             * 这里**不再**要求 mSeekFlag：安卓实测该签名出现在 seek 结束之后
             * （seekFlag 只活 16~23ms），限定在 seek 窗口内等于永远看不到它。
             * 只记闩，不在解码线程里做任何判断或等待。
             */
            mVideoDecodeRetrySeen = true;
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

        /*
         * "有输入却零输出"那条状态判据的**自愈**（第 2 项）：只要有帧真的出来，
         * 卡死闩、轮数与输入饥饿签名一起复位 —— 下一次真卡死仍然允许重建一次。
         * 这就是它全部意义上的"冷却"：由出帧事件驱动，不用任何计时器。
         */
        mDecodeStallRebuildDone = false;
        mDecodeStallIters = 0;
        mVideoDecodeRetrySeen = false;

        /* 切换提交后，目标 decoder 的每一帧都必须使用与首帧相同的时间轴偏移。
         * 只修正首帧会导致后续帧突然跳回原始 rendition PTS，RenderVideo() 便会
         * 把它们当成严重落后帧持续丢弃，日志表现为 0/1 FPS。 */
        /*
         * ============ 【B10】逐帧精确：优先用"帧自己那一个包"的 timePosition ============
         *
         * 帧与包同序 ⇒ 帧的 pts 应能在 mVideoAxisPts 的前端找到（顺便把更旧的条目淘汰掉）。
         * 命中就直接把帧的 pts 设成**它自己**的节目位置：分片边界不再有任何台阶，
         * 也不需要对全局偏移做任何刷新（B9-2 退化为"查不到时的兜底"）。
         * 查不到（解码器改写 pts、或该路没有包级 timePosition）⇒ 与今天完全一致的全局平移。
         */
        bool framePtsTakenFromPacketAxis = false;

        if (pFrame->getInfo().pts != INT64_MIN) {
            while (!mVideoAxisPts.empty() && mVideoAxisPts.front() < pFrame->getInfo().pts) {
                mVideoAxisPts.pop_front();
                mVideoAxisTimePos.pop_front();
            }

            if (!mVideoAxisPts.empty() && mVideoAxisPts.front() == pFrame->getInfo().pts) {
                pFrame->getInfo().pts = mVideoAxisTimePos.front();
                mVideoAxisPts.pop_front();
                mVideoAxisTimePos.pop_front();
                framePtsTakenFromPacketAxis = true;
            }
        }

        if (!framePtsTakenFromPacketAxis &&
            mActiveVideoPtsOffset != INT64_MIN && pFrame->getInfo().pts != INT64_MIN) {
            pFrame->getInfo().pts += mActiveVideoPtsOffset;
        }

        if (mSecretPlayBack) {
            pFrame->setProtect(true);
        }
        int64_t pts = pFrame->getInfo().pts;

        /*
         * 【B6】能力探测：这个后端的输出帧带不带节目时间轴（timePosition）。
         * 只置真、不置假（粘性）：Android MediaCodec 后端永远不给，Qt/macOS/iOS 会给。
         * 用途见头文件里 mActiveDecoderFramesCarryTimePosition 的说明 ——
         * 切档预滚据此决定"能不能用帧自带的 timePosition 对齐"。
         */
        if (pFrame->getInfo().timePosition >= 0) {
            mActiveDecoderFramesCarryTimePosition = true;
        }

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
        /*
         * ============ 【B6】帧的节目时间轴：后端不给就自己算 ============
         *
         * Android 硬解（mediaCodecDecoder）不把 packet metadata 复制到输出帧
         * （`mediaCodecDecoder.cpp:692` 仍是 `timePosition = INT64_MIN`），于是
         * `frameTimePosition < 0` 会让下面三件事全部失效：
         *   · "切换窗丢帧"整段被跳过（预滚前缀帧一张都不丢）；
         *   · 逐帧归一化整段被跳过；
         *   · 落到 :5131 的**备用偏移** `masterPts - framePts`。
         * 备用偏移的语义是"把目标档帧重命名到主时钟轴上"，而预滚起点是参考点**之后**
         * 的第一个关键帧（本片源 IDR 5~11 s ⇒ 领先 lead）⇒ 新档内容被整体**提前**
         * lead 秒显示：用户看到的就是"切完清晰度音画错位"。真机 5 次切换的 lead =
         * 4.529 / 6.601 / 6.872 / 8.282 / 8.997 s，与 `committed … offset=` 的
         * 主时钟相对值逐一对得上。
         *
         * 处置：包级元数据是**有的**（DecodeVideoPacket 在预滚处按
         * `timePosition - pts` 记进 mPendingVideoPtsOffset），所以帧缺 timePosition 时
         * 用同一个恒定差把它算出来。只用**局部变量**、不写回帧的字段 —— 渲染侧的
         * K1 落点采纳读的就是 frame 的 timePosition，不能连带改变它的行为。
         * 这样 Android 的预滚判断与 Qt 完全同构（Qt 的帧本来就带 timePosition）。
         */
        int64_t frameTimePosition = frame->getInfo().timePosition;

        if (frameTimePosition < 0 && framePts != INT64_MIN && mPendingVideoPtsOffset != INT64_MIN) {
            frameTimePosition = framePts + mPendingVideoPtsOffset;
        }

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

        /*
         * ============ 【预热背压修复：已经落后播放位置的帧在**抽取处**就丢掉】============
         *
         * 症状（实测）：DASH 切换 7.35 秒，而 pending 路正好以 1× 实时前进
         * （7.26 秒内容 / 218 帧 / 约 38 包每秒），硬解本应几百帧每秒。
         *
         * 机制：DrainPendingVideoFrames() 只要 app 侧队列到了
         * VIDEO_PICTURE_MAX_CACHE_SIZE * 2（= 4）帧就**不再抽**解码器输出；而预热期
         * 抽到的"已经落在播放位置之前"的帧，在这里既不会被丢（本判据以前只在
         * TryCommitPendingVideoSwitch 的提交门里做），又永远满足不了提交门
         * （提交门要求帧贴着主时钟）。于是那 4 个格位被一批**注定要丢**的帧钉住，
         * 解码器输出队列随之一直是满的（ActiveDecoder 的 maxOutQueueSize），
         * send_pending_packet 于是每次都返回 STATUS_RETRY_IN ⇒ pending 路每轮只能
         * 前进一个包，速度被压到主时钟的节拍上 —— 这就是"1× 实时"的全部来源。
         *
         * 处置：在抽取处（还在 mPendingVideoFrameQue.push 之前）就把这类帧
         * setDiscard 掉，不占那 4 个格位，然后 continue 继续抽。丢帧本身只是
         * setDiscard + 析构，不花时间；真正的成本在解码，所以这一步只增不减速度。
         *
         * 【为什么用"播放位置"而不是"预滚参考点"】
         * mPendingVideoPrerollRefUs 会被 B7 那段**下调到真正的解码起点关键帧**
         * （见上面 `pending preroll: starting at the key frame BEFORE the reference`
         * 那一分支），于是"早于参考点"在"从分片起点起解"这种最常见的慢场景里是
         * **空集**，起不到拆背压的作用；而"早于播放位置（减对齐容差）"正是提交门
         * 原本会丢掉的那一批。
         *
         * 【静态自证：不可能丢掉会被提交的帧】
         * 判据与 TryCommitPendingVideoSwitch 里"丢弃落后帧"的循环、以及它后面那条
         * 单帧丢弃逐字相同（frameEffectivePos + PENDING_ALIGN_TOLERANCE_US < master）；
         * 那里的 master 是在**更晚**的时刻读的（本函数这里是更早、更小的值，播放态
         * 下主时钟单调不减），所以这里丢掉的一定是提交点也会丢掉的那一批，
         * 提交锚点的选择逐字不变。暂停态两边用的都是冻结的 mPausedSwitchPivotUs，
         * 完全同参。外层 for 的上限是 PENDING_PREROLL_DRAIN_MAX，不会无界循环。
         */
        if (frameTimePosition >= 0) {
            const int64_t videoAlignMasterUs = (mSwitchStartedWhilePaused && mPausedSwitchPivotUs > 0)
                                                   ? mPausedSwitchPivotUs : masterPts;

            if (videoAlignMasterUs > 0 &&
                frameTimePosition + PENDING_ALIGN_TOLERANCE_US < videoAlignMasterUs) {
                if (floodLogAllowed(FLOOD_PENDING_DROP, 2, "drop pending frame already behind the playback position")) {
                    AF_LOGI("drop pending frame already behind the playback position: pts=%lld timePosition=%lld "
                            "master=%lld lag=%lld ms stream=%d — dropping it here instead of at the commit gate "
                            "keeps the 4-slot pending queue free, so the target decoder is no longer throttled to "
                            "the clock rate by back-pressure\n",
                            (long long) framePts, (long long) frameTimePosition,
                            (long long) videoAlignMasterUs,
                            (long long) ((videoAlignMasterUs - frameTimePosition) / 1000),
                            mPendingVideoStreamIndex);
                }

                frame->setDiscard(true);
                continue;
            }
        }

        /* 不能用不断前进的 masterPts 过滤 pending 帧。目标 representation
         * 通常从切换点之前最近的关键帧开始，网络/解码速度又可能暂时落后主
         * 时钟数秒；若这里按 masterPts 每帧丢弃，目标路永远追不上，日志会
         * 反复出现“drop pending init/old frame”并最终 FPS=0。真正需要丢弃的
         * 只有固定切换目标之前的旧 segment，上面的 switch-window 判断已经
         * 完成；主时钟仅在 TryCommitPendingVideoSwitch() 中作为提交门限。
         *
         * 【2026-09-25 补充：紧挨上面的新判据不是这里说的那种"过滤"】
         * 那段只是把"已经落后播放位置超过 PENDING_ALIGN_TOLERANCE_US"的帧提前
         * 丢掉 —— 判据与提交门里的落后帧循环、以及它后面那条单帧丢弃逐字相同，
         * 也就是提交点**本来就会丢掉的那一批**，提交锚点的选择一字未改；
         * 区别只是不再让这些注定要丢的帧占住 app 侧那 4 个格位（占住的后果
         * 见上面"预热背压修复"整段：解码器输出队列恒满 ⇒ RETRY_IN ⇒ 1× 实时）。
         * 这里记录的历史坑（pending 解码比实时还慢时"永远追不上"）依旧成立：
         * 那种情况下同一批帧在提交点也一样会被丢，切换仍由既有超时收尾，
         * 新判据既不会让它变好，也不会让它变坏。 */

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
    /*
     * 新解码器是干净状态：参数集必须重新贴一次（见 attachPendingVideoCodecParams）。
     * 这个标志管的是"当前这个 pending 解码器收没收到过 CSD"，换了实例就得复位，
     * 否则新实例会一帧参数集都拿不到 —— 那正是马赛克（-1094995529）的形状。
     */
    mPendingVideoCodecParamsAttached = false;
    /*
     * ============ B2：pending 解码器用"占位 Surface"配置（2026-09-24）============
     *
     * tunnel（渲染器带 FLAG_DUMMY）下 pending 解码器**不能**绑真 Surface ——
     * 一个 Surface 同时只允许一个 MediaCodec 连接（平台日志 already connected /
     * err -22，见下面那段实测），而 active 解码器正连着它。
     *
     * 但"退成 ByteBuffer 模式"（下面传 nullptr）也不行：promote 之后它已经变成
     * active，帧里却没有像素可写，画面永远停在旧 codec 留在 Surface 里的最后一帧 ——
     * 这正是"切清晰度后画面停在旧帧"。
     *
     * 处置：pending 用一块 1x1 的占位 Surface 配置（surface 模式：帧被丢弃，但解码
     * 正常推进），提交时由 TryCommitPendingVideoSwitch 做有序交接：
     *   ① 旧解码器 setOutputSurface(null) 交出真 Surface（它仍活着，是回滚的底牌）
     *   ② pending 解码器 setOutputSurface(真 view)
     *   ③ 只有 ② 成功才 promote + 释放旧解码器；失败就把真 Surface 还给旧解码器并
     *      按失败收尾（不 promote、不释放）—— 画面因此不受影响。
     * 注意顺序与 2026-09-24 第一版相反：**绝不"先释放旧 codec 再交接"**，那会让
     * SurfaceView 失去 producer，一旦新 codec 接不上画面必冻。
     *
     * 判据（B4：两条渲染路都要成立）：
     *   · 隧道（FLAG_DUMMY）：真面 = App 的 view ⇒ view 非空才做（与今天逐字一致）；
     *   · GL（GLRender）：真面 = 渲染器 SurfaceTexture 的那块面（mActiveVideoSurface，
     *     由 CreateVideoDecoder 记下）⇒ 只要硬解、且渲染器已经给过一块面就做。
     *     GL 路**不需要** App 的 view（这正是"没有 Surface 也能解、后台不黑屏"的性质），
     *     所以这里不能再用 mSet->mView 判空 —— 那会让 GL 路整条 B2 恒假，
     *     退回"promote 一个绑在占位面上的解码器"，画面永远停在旧帧。
     * 下面 setUpPendingVideoDecoder 仍然传 device = nullptr：占位 Surface 由 Java 侧
     * （MediaCodecDecoder.ensureDummySurface）创建，内核不持有它。
     */
    const bool renderIsDummy =
        mAVDeviceManager->isVideoRenderValid() &&
        (mAVDeviceManager->getVideoRender()->getFlags() & IVideoRender::FLAG_DUMMY) != 0;
    const bool renderIsSurfaceConsumingGL =
        mAVDeviceManager->isVideoRenderValid() && !renderIsDummy;
    const bool pendingPlaceholderSurface =
        renderIsDummy ? (mSet->mView.load() != nullptr)
                      : (renderIsSurfaceConsumingGL && bHW && mActiveVideoSurface != nullptr);
    if (pendingPlaceholderSurface) {
        flags |= DECFLAG_PLACEHOLDER_SURFACE;
        /*
         * 旋转归属必须与 **active 解码器**一致（占位面同样是 surface 模式）：
         *   · 隧道的 active 解码器带 DECFLAG_DIRECT ⇒ mediaCodecDecoder 里转成
         *     DECFLAG_OUT ⇒ Java 写 rotation-degrees，旋转由 MediaCodec 做；
         *   · GL 的 active 解码器**不带** DECFLAG_OUT（旋转由 OES shader 做，
         *     见 GLRender 里的 getRotate(frame->video.rotate)）⇒ 占位面也必须不带，
         *     否则交接后方向与 active 不一致（rotate != 0 的片源会转错）。
         * 判据直接读 active 解码器的 flags（getVideoDecoderFlags = 解码器自己 getFlags），
         * 所以与"现在走哪条路"无关，也不需要写两份。
         */
        if (mAVDeviceManager->getVideoDecoderFlags() & DECFLAG_OUT) {
            flags |= DECFLAG_OUT;
        }
    }
    mPendingDecoderUsesPlaceholderSurface = pendingPlaceholderSurface;
    // pending decoder 不能绑定 active decoder 正在使用的同一个 surface。
    // 对 FFmpeg/D3D11、VideoToolbox 等帧输出解码器，nullptr 表示输出解码帧；
    // 对要求独占 surface 的平台硬解则走软解兜底，避免两个 decoder 争用同一
    // surface 导致死锁、黑屏或极低帧率。
    //
    // 【2026-09-24 实测：在 Android tunnel 下这条是**平台硬约束**，不是保守选择】
    // 曾尝试把 mSet->mView 交给 pending（想让 promote 之后能出画），结果
    // MediaCodec.configure(surface) 当场失败，平台日志原文：
    //   BufferQueueProducer: connect: already connected (cur=3 req=3)
    //   SurfaceUtils:        Failed to connect to surface 0x…, err -22
    //   MediaCodec:          nativeWindowConnect returned an error: Invalid argument (-22)
    //   MediaCodecDecoder:   configure fail / failed to config mDecoder rv -14
    // ⇒ 一个 Surface（SurfaceView 的 BLAST producer）**同时只允许一个 MediaCodec 连接**，
    //   所以 pending 不能直接绑真 Surface；上面 B2 的"占位 Surface + 提交时有序交接"
    //   就是"在别处解决"的那条路。
    /*
     * HDR rendition 不能走 GL（B4）：HDR 依赖"解码器直出 + 显示链路拿到 HDR 元数据"
     * （SetUpVideoPath 里 `isHDRVideo(meta) ⇒ FLAG_DUMMY`，那里有说明）；GL 路要把
     * buffer 采成普通 OES 纹理，没有 tone mapping ⇒ 画面发灰发暗。
     * 起播时内核已按内容选好了路，但"切到 HDR 档"是**运行中的换流**，而运行中换渲染模式
     * 需要重建渲染器 + 重建解码器 = 必然一次关键帧重解（正是本方案要消灭的卡顿）。
     * 所以宁可**明确失败**并说清原因，也不给用户一个颜色错误的画面。
     * 要切 HDR 档：让 App 打开 tunnel 开关（那条路渲染 HDR 是对的）。
     *
     * 【判据必须收窄到"安卓 GLRender 那一条路"，否则会动到别的平台】
     * 全仓只有 `GLRender` 覆盖了 `IVideoRender::getSurface()`（而且它内部是
     * `#ifdef __ANDROID__`，非安卓直接返回 nullptr）——Qt 自己的渲染器、
     * SdlAFVideoRender、AVFoundationVideoRender 都用默认实现返回 nullptr，
     * 于是它们的 `mActiveVideoSurface` 恒为 null。用"mActiveVideoSurface 非空"当判据，
     * 这条拒绝**只可能在安卓 GL 路上生效**：Qt 的 FLAG_HDR 路（CicadaVideoRender 返回
     * FLAG_HDR 表示"我自己渲染 HDR"）不受任何影响。
     */
    const bool glPathOwnsDecoderSurface = !renderIsDummy && mActiveVideoSurface != nullptr;
    const char *pendingFailReason = "pending video decoder creation failed";
    int ret = 0;
    if (glPathOwnsDecoderSurface && isHDRVideo(&meta)) {
        ret = -ENOSPC;
        pendingFailReason = "switching to an HDR rendition is not supported on the GL render path "
                            "(HDR needs the tunnel/direct render path)";
        AF_LOGE("quality switch refused: the target rendition is HDR while the active render path is "
                "GL (renderFlags=0). The render path is chosen per content in SetUpVideoPath, and "
                "changing it mid-playback would cost a key-frame re-decode. Enable the tunnel option "
                "for HDR content instead.\n");
    } else {
        ret = mAVDeviceManager->setUpPendingVideoDecoder(flags, &meta, nullptr, 0);
        if (ret < 0 && bHW) {
            ret = mAVDeviceManager->setUpPendingVideoDecoder(DECFLAG_SW, &meta, nullptr, 0);
        }
    }
    if (ret < 0) {
        mPNotifier->NotifyVideoQualitySwitch(PLAYER_QUALITY_SWITCH_FAILED,
                                             mPendingVideoStreamIndex,
                                             pendingFailReason);
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
    /*
     * ============ S2：暂停态切档用**冻结参考点**代替主时钟（2026-09-24）============
     *
     * 本函数下面全部"提交门"都写成 `frameTimePosition <= master + 容差` 的形状，
     * 而主时钟在暂停态是**不动**的（mMasterClock.pause()）。于是：
     *   * 被暂停点之前的关键帧喂进来的帧永远满足不了 `帧 >= master - 容差`；
     *   * 用户暂停时的时间点也永远等不到"主时钟追上来"。
     * 两条合起来就是"暂停切档永远不提交"，实测 [switch] 关键量 28.6 秒不变。
     *
     * 处理：暂停态把参与比较的参考量换成切换发起时快照的 mPausedSwitchPivotUs。
     * **只换参考量，不换任何比较结构**，所以两条路径的门限语义完全一致，
     * 播放态（mSwitchStartedWhilePaused == false）行为逐字不变。
     *
     * 注意这里仍然**不动主时钟、不发位置回调** —— S6 的约束在这里同样成立：
     * 暂停态切档只影响"视频槽位"，不改变用户看到的时间。
     */
    const bool pausedSwitch = mSwitchStartedWhilePaused && mPausedSwitchPivotUs > 0;
    const int64_t master = pausedSwitch ? mPausedSwitchPivotUs : mMasterClock.GetTime();
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
        (switchElapsedMs > 0 &&
         (switchElapsedMs > QUALITY_SWITCH_TOTAL_TIMEOUT_MS ||
          (switchElapsedMs > 8000 &&
           (frameTimePosition < 0 ||
            (mPendingVideoSwitchTimePosition >= 0 &&
             frameTimePosition + 2 * 1000 * 1000 < mPendingVideoSwitchTimePosition)))));
    if (switchTimedOut) {
        AF_LOGW("quality switch timed out before commit: stream=%d target=%lld frame=%lld "
                "elapsed=%lld pendingPktQ=%d pendingFrameQ=%d master=%lld\n",
                mPendingVideoStreamIndex,
                (long long) mPendingVideoSwitchTimePosition,
                (long long) frameTimePosition, (long long) switchElapsedMs,
                (int) mPendingVideoPacketQue.size(), (int) mPendingVideoFrameQue.size(),
                (long long) master);
        mPNotifier->NotifyVideoQualitySwitch(
            PLAYER_QUALITY_SWITCH_FAILED, mPendingVideoStreamIndex,
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

    /*
     * ============ 【B11】pending 帧的"有效节目位置" ============
     *
     * 提交门的三处判定原来都读 `frame->getInfo().timePosition`，而 Android 硬解的
     * 输出帧恒为 `INT64_MIN`（`mediaCodecDecoder.cpp:692` 那个 TODO）⇒ 这三处**在安卓上
     * 全部失效**：
     *   · "落后主时钟就丢"的循环直接 break（前缀帧一张都不丢）；
     *   · "落后就等/超前就等"两条判据都不生效 ⇒ **只要有帧就提交**。
     * 于是 B7 让 pending 从参考点之前起解时，提交会发生在**落后主时钟约 1 s**的位置
     * （真机 `committed … pts=41708 master=1459472`），随后靠渲染器**逐帧丢弃**去追
     * —— 日志里那串 `drop frame` 就是它，用户看到的是"启动/切档时的小跳动"。
     *
     * 修法：用"有效位置"= 帧自带 timePosition；缺了就用它**已经被 B6 归一化过的 pts**
     * （B6 之后安卓上这个 pts 就是节目位置）。纯状态判断，无计时器；对带 timePosition
     * 的后端（Qt/macOS/iOS）取值与今天**逐字相同**。
     */
    auto pendingEffectivePos = [](const std::unique_ptr<IAFFrame> &f) -> int64_t {
        if (f == nullptr) {
            return INT64_MIN;
        }

        const int64_t tp = f->getInfo().timePosition;
        return (tp >= 0) ? tp : f->getInfo().pts;
    };

    if (master > 0) {
        while (!mPendingVideoFrameQue.empty()) {
            const int64_t headPos = pendingEffectivePos(mPendingVideoFrameQue.front());

            if (headPos == INT64_MIN || headPos + PENDING_ALIGN_TOLERANCE_US >= master) {
                /* 这一张（或者连位置都推不出来的 init 帧）交给下面的正常判断。 */
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
    /* B11：判定用"有效位置"（见上面 pendingEffectivePos 的说明） */
    const int64_t frameEffectiveNow = pendingEffectivePos(mPendingVideoFrameQue.front());

    if (master > 0 && frameEffectiveNow != INT64_MIN &&
        frameEffectiveNow + PENDING_ALIGN_TOLERANCE_US < master) {
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
    if (master > 0 && frameEffectiveNow != INT64_MIN &&
        frameEffectiveNow > master + PENDING_FUTURE_TOLERANCE_US) {
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
    /*
     * ============ B2'：tunnel 下的"单连接有序交接"（2026-09-24）============
     *
     * 问题（真机实测，缺陷 ②）：tunnel 模式下 pending 解码器只能传 nullptr 配置
     * （一个 Surface 只允许一个 MediaCodec 连接，传 view 会 configure 失败 -22），
     * 于是它是 ByteBuffer 模式、帧里没有像素；promote 之后画面永远停在旧帧、音频照常。
     *
     * 处置：在**提交点**先把旧 codec 关掉（释放真 Surface 连接），再按目标流 meta 新建
     * 一个解码器 —— CreateVideoDecoder 的 tunnel 分支自带 view = mSet->mView +
     * DECFLAG_DIRECT，所以新解码器是**唯一**连接、帧是真像素。全程不调用 FlushVideoPath。
     *
     * 顺序（不可换）：
     *   ① 旧档已解出的帧 discard（它们随旧 codec 关闭失效，不能再用；画面停在 Surface
     *      里最后一张 buffer 上，不黑屏）
     *   ② 丢弃 ByteBuffer 模式的 pending 解码器及其包/帧队列
     *   ③ invalidateDecoder 关旧 codec ⇒ 释放真 Surface 连接
     *   ④ 非 const 栈副本取目标流 meta（照 rebuildVideoDecoder 的写法）+ CreateVideoDecoder
     *   ⑤ 下面既有的 mQualitySwitchHoldFrames = QUALITY_SWITCH_HOLD_FRAMES 重新 armed hold
     *   ⑥ 下面既有的 finishQualitySwitch(true, …) 在新档首帧送出后收 READY
     *
     * 【安全护栏】"位置不回退"是硬要求：只在**目标帧带 media time**（timePosition >= 0）
     * 时才走 B2'。没有 media time 时 pending 预热算出的 mPendingVideoPtsOffset 是唯一
     * 归一化依据，丢掉它会让 pts 从 0 起步 ⇒ 退回今天的 pending 路径（行为与现在一致）。
     *
     * 【一键回退】条件里含 FLAG_DUMMY ⇒ Compose 侧 TUNNEL_RENDER_ENABLED=false 时恒假，
     * 行为与今天逐字一致。Qt 侧永远没有 FLAG_DUMMY 渲染器（bEnableTunnelRender 每次播放前
     * reset 为 false、Qt 壳不下发该选项、Qt 渲染器返回 0 而不是 FLAG_DUMMY）⇒ 死代码。
     *
     * 【纯增量】本块不 promote、不 assign 提交状态：下面的既有序列在 pending 已失效时
     * 全部退化为无操作（promotePendingVideoDecoder 自身有 valid 判空、pending 帧队列已空、
     * oldVideoFramesPending 因为队列已清空而是 0），所以 B2' 走通时与既有提交语义一致。
     *
     * 【与 B2（占位 Surface 交接）的分工 —— 2026-09-24】B2' 是"pending 是 ByteBuffer
     * 模式（帧里没有像素）"的兜底：它只能把 pending 丢掉、按目标 meta 重建一个真绑
     * view 的解码器（会丢掉 pending 解出的帧与 pts 对齐）。而 B2 让 pending 一开始就用
     * 占位 Surface 配置（surface 模式），提交时保留 pending 解出的帧与 pts 对齐，只做
     * "让旧解码器交出真 Surface、把新解码器接上去"。
     * 两者对同一个 pending 是互斥的两种处置，因此本块加了
     * !mPendingDecoderUsesPlaceholderSurface 这道门：pending 带占位 Surface 时一律交给
     * 下面的 B2（含"有 media time"的片源 —— B2 保留 pts 偏移，不需要 B2' 的那条
     * timePosition >= 0 护栏），本块只在占位 Surface 真的用不上时（Java 侧创建失败、
     * 退 ByteBuffer 模式）才执行；两道门都不成立时行为与今天逐字一致。
     */
    const bool b2TunnelRender =
        mAVDeviceManager->isVideoRenderValid() &&
        (mAVDeviceManager->getVideoRender()->getFlags() & IVideoRender::FLAG_DUMMY);

    /*
     * B4：不管走哪条路，交接/重建都要用"当前 active 解码器绑的那块**真面**"——
     *   · 隧道：App 的 view（mSet->mView）；
     *   · GL  ：渲染器 SurfaceTexture 的那块面（CreateVideoDecoder 记下的 mActiveVideoSurface）。
     * 绝不能互换：拿 App 的 view 去给 GL 的解码器 setOutputSurface 一定失败
     * （那块面归 EGL 消费，不是 codec 的输出面），反过来隧道也接不上 SurfaceTexture。
     * 判据只有"这块面存在"一条，所以两条路共用同一段交接/重建代码。
     */
    void *b2RealSurface = nullptr;

    if (mAVDeviceManager->isVideoRenderValid()) {
        b2RealSurface = b2TunnelRender ? mSet->mView.load() : mActiveVideoSurface;
    }

    /* "解码器输出到一块真面"这条能力（B2 与 B2' 的共同前提）。 */
    const bool b2SurfaceOutput = mAVDeviceManager->isVideoRenderValid() && b2RealSurface != nullptr;

    if (b2SurfaceOutput && !mPendingDecoderUsesPlaceholderSurface &&
        mPendingVideoStreamIndex >= 0 &&
        !mPendingVideoFrameQue.empty() &&
        mPendingVideoFrameQue.front()->getInfo().timePosition >= 0) {
        Stream_meta targetMeta{};
        const bool targetMetaOk =
            (mDemuxerService != nullptr) &&
            (mDemuxerService->GetStreamMeta(&targetMeta, mPendingVideoStreamIndex, false) >= 0);

        if (targetMetaOk) {
            const int b2RetiredStream = mCurrentVideoIndex;
            const int b2TargetStream = mPendingVideoStreamIndex;

            /* ① 旧档已解出的帧全部作废（旧 codec 马上要被关掉，这些帧已无法上屏） */
            while (!mVideoFrameQue.empty()) {
                mVideoFrameQue.front()->setDiscard(true);
                mVideoFrameQue.pop();
            }

            /* ② pending（ByteBuffer 模式、没有可用像素）连同它的队列一起丢掉 */
            mAVDeviceManager->discardPendingVideoDecoder();
            while (!mPendingVideoFrameQue.empty()) {
                mPendingVideoFrameQue.front()->setDiscard(true);
                mPendingVideoFrameQue.pop();
            }
            while (!mPendingVideoPacketQue.empty()) {
                mPendingVideoPacketQue.front()->setDiscard(true);
                mPendingVideoPacketQue.pop_front();
            }

            /* ③ 关旧 codec：真 Surface 的连接在这里释放（此后新 codec 才能独占它） */
            mAVDeviceManager->invalidateDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);

            /* ④ 按目标流 meta 新建；隧道自带 view = mSet->mView + DECFLAG_DIRECT，
             *    GL 自带渲染器 getSurface() 给的新面（CreateVideoDecoder 内部选，并记进
             *    mActiveVideoSurface） */
            const bool b2HW = (mAVDeviceManager->getVideoDecoderFlags() & DECFLAG_HW) != 0;
            int b2Ret = CreateVideoDecoder(b2HW, targetMeta);

            if (b2Ret < 0 && b2HW && !b2TunnelRender) {
                /* GL 路与起播路径同规矩：硬解建不起来就落软解（GL 能画 CPU 帧）。
                 * 隧道不能这么做 —— 软解帧走不了 DummyVideoRender，只会黑屏。 */
                b2Ret = CreateVideoDecoder(false, targetMeta);
                AF_LOGI("dummy-surface handover (B2'): hardware decoder failed, fell back to "
                        "software decode (GL path), ret=%d\n", b2Ret);
            }

            if (b2Ret >= 0) {
                /* 目标流就是当前流：让 updateVideoMeta() 立刻按新档刷新宽高/编码
                 *（下面既有那句赋值会再写一次同样的值） */
                mCurrentVideoIndex = b2TargetStream;
                updateVideoMeta();
            }

            AF_LOGI("dummy-surface handover (B2'): retiredStream=%d -> stream=%d %dx%d bHW=%d ret=%d "
                    "(ByteBuffer pending 已丢弃；真面全程只有一个连接；未调用 FlushVideoPath)\n",
                    b2RetiredStream, b2TargetStream, targetMeta.width, targetMeta.height,
                    (int) b2HW, b2Ret);

            if (b2Ret < 0) {
                AF_LOGE("dummy-surface handover (B2'): CreateVideoDecoder failed %d with the %s "
                        "fallback policy\n", b2Ret,
                        b2TunnelRender ? "no-software (tunnel: soft frames can not be shown by "
                                         "DummyVideoRender)" : "software-allowed (GL)");
            }
        } else {
            AF_LOGW("dummy-surface handover (B2') skipped: target stream meta unavailable "
                    "(stream=%d), keeping the pending path\n", mPendingVideoStreamIndex);
        }
    } else if (b2SurfaceOutput && !mPendingDecoderUsesPlaceholderSurface &&
               !mPendingVideoFrameQue.empty() &&
               mPendingVideoFrameQue.front()->getInfo().timePosition < 0) {
        AF_LOGW("dummy-surface handover (B2') skipped: target frames have no media time "
                "(timePosition<0) — keeping the pending path so pts/position semantics stay unchanged\n");
    }

    /*
     * ============ B2：placeholder（占位 Surface）交接（2026-09-24，第二版）============
     *
     * 前提（由 CreatePendingVideoDecoder 建立）：pending 解码器是用一块 1x1 占位 Surface
     * 配置的 —— 它从第一帧起就是 **surface 模式**（帧有像素目标，被占位面静默丢弃），
     * 而不是 ByteBuffer 模式。于是它随时可以被"接到"真面上。**两条路都成立**（B4）：
     * 隧道下真面是 App 的 SurfaceView，GL 下真面是渲染器 SurfaceTexture 的那块面。
     *
     * 【为什么顺序必须是这样（第一版的教训，真机日志为证）】
     * 第一版是"先 releaseRetiredVideoDecoder() 把旧 codec 关掉，再 setOutputSurface(真 view)"。
     * 真机日志里那次交接：
     *   SurfaceUtils: disconnecting from surface …, reason disconnectFromSurface   ← 旧 codec 被关，producer 没了
     *   setOutputSurface fail codec was not configured for an output surface      ← 新 codec 是 ByteBuffer 模式，接不上
     *   dummy-surface placeholder handover (B2): … setOutputSurface ret=-2
     * ⇒ SurfaceView 从此既没有 producer 也没有新画面 ⇒ 画面必冻，而且状态机还照报
     *   "committed seamless" + "quality switch rendered" + App status=1（明明没成还算成功）。
     *
     * 第二版把它翻过来，**任何时刻都至少有一个解码器连着真面**：
     *   ① 旧（= 即将 retired 的 active）解码器 setOutputSurface(null)：它自己的 Java
     *      实现会把输出切到内部 1x1 占位面，从而**交出**真面的唯一连接。
     *      注意这一步**不释放**旧解码器 —— 它是回滚的底牌。
     *   ② pending 解码器（**promote 之前**就能通过 getPendingVideoDecoder() 拿到）
     *      setOutputSurface(真面 = b2RealSurface)：把真面接过来。
     *   ③ 记录两个 ret：
     *      · ② 成功 ⇒ 这时才 promote（槽位交换）并 releaseRetiredVideoDecoder()
     *        （真面已由新 codec 独占，释放旧 codec 不会让画面失去 producer）；
     *      · ② 失败 ⇒ 立刻把真面还给旧解码器（回滚），打 ERROR 日志，并且
     *        **不 promote、不释放**，按既有"切档失败"出口收尾（与 switchTimedOut 的
     *        收尾逐条同构：NotifyVideoQualitySwitch(FAILED) + 丢 pending + 关目标流 +
     *        清 pending 状态 + return false）。此时解码器槽位仍是旧流、旧解码器仍连着
     *        真面且仍在收包 ⇒ 画面继续按旧清晰度播放，只是这次切档失败。
     *
     * 【为什么 GL 路也能用同一套（B4，也是"两条路都要无缝切档"的关键）】
     * GL 下旧解码器写的是渲染器的 SurfaceTexture：同样"一个 BufferQueue 只允许一个
     * codec 连接"，所以 pending 也只能先绑占位面；交接时把真面（SurfaceTexture 的面）
     * 从旧 codec 转给 pending codec，渲染器那边一行都不用改 —— 它照旧 updateTexImage
     * 拿"当前 producer 写进来的最新一帧"。而且 GL 不依赖 App 的 view，交接在后台/
     * 无 Surface 时同样成立。
     *
     * 【一键回退】b2Placeholder 依赖 mPendingDecoderUsesPlaceholderSurface（由
     * CreatePendingVideoDecoder 置位）；把它连同 B4 的那两处判据一起改回 tunnel-only，
     * 整块就退化成"照旧 promote"，与改动前逐字一致。
     */
    const bool b2Tunnel = b2TunnelRender;
    const bool b2Placeholder = mPendingDecoderUsesPlaceholderSurface &&
        b2RealSurface != nullptr && mPendingVideoStreamIndex >= 0;
    int b2RetiredStream = -1;
    IDecoder *b2OldDecoder = nullptr;
    IDecoder *b2NewDecoder = nullptr;
    int b2RetDetach = -1;
    int b2RetAttach = -1;
    int b2RetRollback = -1;
    bool b2HavePair = false;

    if (b2Placeholder) {
        b2RetiredStream = mCurrentVideoIndex;
        b2OldDecoder = mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);
        b2NewDecoder = mAVDeviceManager->getPendingVideoDecoder();

        if (b2OldDecoder == nullptr || b2NewDecoder == nullptr) {
            /*
             * 拿不到成对的解码器就**不做**占位交接：宁可退回既有 promote 路径，
             * 也绝不先把真 Surface 断开再发现没人能接（那正是第一版的死法）。
             */
            AF_LOGW("dummy-surface placeholder handover (B2) unavailable: oldDecoder=%p pendingDecoder=%p "
                    "— falling back to the plain promote path\n",
                    (void *) b2OldDecoder, (void *) b2NewDecoder);
        } else {
            b2HavePair = true;
            /* ① 旧解码器交出真面（切到它自己的 1x1 占位面），但**不释放**它 */
            b2RetDetach = b2OldDecoder->setOutputSurface(nullptr);
            /* ② 把真面交给 pending 解码器（promote 之前做 ⇒ 失败可原地回滚） */
            b2RetAttach = b2NewDecoder->setOutputSurface(b2RealSurface);

            if (b2RetAttach != 0) {
                /* ③ 失败：真面立刻还给旧解码器（它仍是 active、仍收包） */
                b2RetRollback = b2OldDecoder->setOutputSurface(b2RealSurface);
                AF_LOGE("dummy-surface placeholder handover (B2) FAILED: retiredStream=%d -> stream=%d "
                        "detach=%d attach=%d rollback=%d — the real surface is back on the old decoder, "
                        "the switch is finished as FAILED without promoting or releasing anything\n",
                        b2RetiredStream, mPendingVideoStreamIndex, b2RetDetach, b2RetAttach, b2RetRollback);
            } else {
                /* 交接成功：旧 codec 马上被释放，它已解出的帧不能再上屏，一次性作废 */
                while (!mVideoFrameQue.empty()) {
                    mVideoFrameQue.front()->setDiscard(true);
                    mVideoFrameQue.pop();
                }
            }
        }
    }

    const size_t oldVideoFramesPending = mVideoFrameQue.size();

    if (b2HavePair && b2RetAttach == 0) {
        /*
         * ④ 交接成功：**不 flush、只 promote**。
         * oldVideoFramesPending 已在上面的清帧循环后取到（= 0），与既有语义一致。
         *
         * ============ 为什么这里**不能** flush（2026-09-24 真机实测）============
         *
         * 上一版在这里加了 `b2NewDecoder->flush()`（本意是丢掉"占位面那一代"的
         * 输出缓冲占用），结果把切换从"交接后 140ms 出画"变成 **10.6 秒静止**，
         * 日志逐项可对：
         *   · preroll 阶段本来就已经从关键帧起解
         *     （`pending preroll starts at key frame: pts=10427083 … lead=9006 ms`），
         *     提交时 pending 解码器**手里已经有可渲染的帧**；
         *   · flush() 会置 bNeedKeyFrame（ActiveDecoder.cpp:370 那句），于是解码器
         *     把之后所有非关键帧全部丢掉 —— 日志里 `wait a key frame` 连打 **6.4 秒**；
         *     而 preroll 用掉的那个关键帧**已经落在读游标后面**，下一个关键帧要等
         *     5.25~10.9 秒（本片源关键帧间隔是 DASH 分片级：preroll 关键帧 raw 10.43s
         *     → 恢复关键帧 raw ≈21.3s）；
         *   · 恢复点还落在**播放位置之后**（实测超前 3.68s），于是再等主时钟追上来
         *     **4.2 秒** —— 6.4s + 4.2s = 10.6s。
         * 结论：flush 把"已经就位的可用帧"扔掉，换来一次注定落后的关键帧重启。
         *
         * 反向证据（同一台机器、不 flush 的那一版）：交接后 **140ms** 就
         * `quality switch rendered`、画面立刻换档 —— 那才是本方案想要的行为。
         * 因此这里**只 promote**，让 pending 解码器继续从它当前的位置解下去
         * （不 flush ⇒ 参考帧/DPB 完好 ⇒ 不需要新关键帧）。
         *
         * 【如果以后又看到"交接后卡住"】判别特征与出路（**不要再回来加 flush**）：
         *   · 日志出现 `MediaCodecDecoder W async input path looks dead
         *     (criterion=starved-without-backpressure …, noOutputHeld=true)` /
         *     `codec has had no input buffer for about 1 s … keep waiting, no teardown`
         *     ⇒ 说明"surface 换面之后本机（Hisi）codec 停止消费输入"是平台硬约束；
         *   · 下一步二选一，都不是加 flush：
         *     a) 把 pending 那一代帧在 **attach 之前**全部释放（先把输出缓冲还给 codec，
         *        再做换面），仍然**不 flush** —— 这是"换面后 codec 因拿不到输出缓冲
         *        而停摆"这个假设的最小验证（z 的下一步候选）；
         *     b) 直接关 tunnel（Compose 侧 TUNNEL_RENDER_ENABLED=false）回到 GL 路。
         */
        mAVDeviceManager->promotePendingVideoDecoder();
        /*
         * 【不在这里 releaseRetiredVideoDecoder()】真 Surface 的连接已经由 ① 的
         * setOutputSurface(nullptr) 交出去了，早释放不是交接必需；而它会在渲染器
         * 可能仍持有旧世代帧的时候就销毁旧解码器对象（AFMediaCodecFrame 的释放回调
         * 捕获了那个对象、并且直接用 mDecoder->releaseOutputBuffer，没有判空），
         * 是一处真实的 UAF 风险。释放交回既有时点：finishQualitySwitch()
         * （首帧真的上屏之后，见 RenderVideo 里 READY 那段说明）。
         */
        AF_LOGI("dummy-surface placeholder handover (B2): retiredStream=%d -> stream=%d detach=%d attach=%d "
                "(no flush on purpose: the preroll already started at a key frame and the pending decoder "
                "holds renderable frames, so decoding continues without a key-frame restart — flushing here "
                "was measured at 10.6s of frozen picture; the pending packet queue and pts offset are kept; "
                "the old decoder is released later by finishQualitySwitch, not here). READY is NOT raised "
                "here: it stays driven by RenderVideo's 'quality switch rendered' -> finishQualitySwitch(true)\n",
                b2RetiredStream, mPendingVideoStreamIndex, b2RetDetach, b2RetAttach);
    } else if (b2HavePair) {
        /*
         * ⑤ 交接失败：收尾成"切档失败"，与 switchTimedOut 的收尾逐条同构。
         * **不 promote、不 releaseRetiredVideoDecoder** ⇒ 解码器槽位还是旧流，
         * 旧解码器仍连着真 Surface、仍在新一轮读包里拿数据，画面不受影响；
         * 目标流被关掉、pending 队列清空，用户可立刻再切一次。
         */
        mPNotifier->NotifyVideoQualitySwitch(PLAYER_QUALITY_SWITCH_FAILED,
                                             mPendingVideoStreamIndex,
                                             "placeholder surface handover failed");
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
            AF_LOGI("closed pending video stream after placeholder handover failure: stream=%d\n",
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
    } else {
        /* 非 B2（或拿不到成对解码器）：行为与今天逐字一致 */
        mAVDeviceManager->promotePendingVideoDecoder();
    }
    /*
     * 【2026-09-21 修：只把 pending 队列里**最新**的一帧带进 active 队列】
     *
     * pending 队列里最多压 4 帧（DrainPendingVideoFrames 的上限），每帧之间只差
     * 一个帧间隔。把这几帧全推进 active 队列，播放锚点就变成**最旧**的那一帧 ——
     * 新画面一上来就落后好几帧，而且它比队里的旧帧还旧，播放顺序会反转
     * （旧帧在时钟上、新帧在时钟之前）。只带最新的一帧，锚点贴着提交时的对齐点。
     */
    std::unique_ptr<IAFFrame> newestPendingFrame{nullptr};

    /*
     * ============ S8：暂停态提交时保留"暂停点之后"的整段帧（2026-09-24）============
     *
     * 上面"只留最新一帧"的取舍对**播放中**是对的：那时主时钟在走，锚点必须贴着
     * 时钟，多带几帧只会让画面落后。
     *
     * 对**暂停态**它正好反了：暂停后用户按播放，主时钟从冻结的 pivot 重新起步，
     * 而被推入 active 队列的只有"暂停点之后最远的那一帧" —— 中间那一段（通常
     * 几十毫秒到几百毫秒，取决于 pending 队列深度）被丢掉，用户按下播放的第一
     * 眼就是"跳了一下"（顿挫）。用户明确要求"暂停中切清晰度后继续播放不卡顿"。
     *
     * 所以在暂停态改成：保留 pending 队列里**从第一张 >= 暂停点 的帧开始**的全部
     * 帧，并把提交帧（最早的那张）放到队首，让恢复播放从暂停点附近连续接上。
     * 早于暂停点的那些仍然作废（它们与"用户暂停时看到的那一帧"重复）。
     *
     * 播放态行为逐字不变：pausedSwitch 为 false 时走的还是原来那段"只留最新一帧"。
     */
    const bool pausedSwitchCommit = mSwitchStartedWhilePaused && mPausedSwitchPivotUs > 0;
    int keptPausedFrames = 0;
    /*
     * 诊断（B2）：本次提交"带进 active 队列的首帧 pts"。先在分配循环**之前**
     * 清成 INT64_MIN（否则上一次切换的值会骗到日志），再在两条路径里各自记录：
     *   · 暂停态：保留段的第一张（就是待上屏的首帧）；
     *   · 播放态：newestPendingFrame（既有逻辑"只带最新一帧"）。
     * RenderVideo 里 `post-handover first frame` 用它判定上屏的是不是 preroll 那一代。
     */
    mQualitySwitchCarriedFramePts = INT64_MIN;

    while (!mPendingVideoFrameQue.empty()) {
        auto frame = std::move(mPendingVideoFrameQue.front());
        mPendingVideoFrameQue.pop();

        if (frame == nullptr) {
            continue;
        }

        if (pausedSwitchCommit) {
            const int64_t framePos = frame->getInfo().timePosition;

            if (framePos >= 0 && framePos + PENDING_ALIGN_TOLERANCE_US < mPausedSwitchPivotUs) {
                /* 暂停点之前：与用户当前看到的那一帧重复，作废。 */
                frame->setDiscard(true);
                continue;
            }

            /*
             * 保留段按 pending 队列原本的顺序（从早到晚）依次追加到 active 队尾：
             * active 队列此刻装着旧路的帧，新路的帧接在它们后面 ——
             * 这正是"新旧交接"该有的顺序，恢复播放时不会出现时间轴反转。
             *
             * 【注意：这里**不能**动 mQualitySwitchOldFramesPending】
             * 那个计数器记的是"promote 之前 active 队列里的旧帧"，只跟旧帧有关；
             * 新路带进来的帧由 RenderVideo 正常消费时递减（见它结尾的递减）。
             * 如果这里也递减，就会把计数减到 0 以下，让 finishQualitySwitch(true)
             * 的 `== 0` 判据提前成立、在新帧其实还没上屏时就发 READY。
             */
            mVideoFrameQue.push(std::move(frame));

            /* 诊断：暂停态提交保留的**第一张**帧就是待上屏的首帧（见下面
             * `post-handover first frame` 日志的判定口径）。 */
            if (mQualitySwitchCarriedFramePts == INT64_MIN) {
                mQualitySwitchCarriedFramePts = mVideoFrameQue.back()->getInfo().pts;
            }

            ++keptPausedFrames;
            continue;
        }

        if (newestPendingFrame != nullptr) {
            /* 更旧的那一帧直接作废：它只在 pending 队列里待了一个帧间隔，
             * 丢掉不会有任何可见影响，但能让提交锚点贴近时钟。 */
            newestPendingFrame->setDiscard(true);
        }

        newestPendingFrame = std::move(frame);
    }

    if (pausedSwitchCommit) {
        AF_LOGI("paused quality switch commit: kept %d pending frame(s) at or after the frozen pivot=%lld us "
                "so playback resumes without a gap (master untouched)\n",
                keptPausedFrames, (long long) mPausedSwitchPivotUs);

        if (keptPausedFrames == 0) {
            /* 一帧都没留下（理论上不该发生，上面已经确认队非空）：退回播放态的
             * 老路径，保证"提交"这个前提一定成立，避免状态机卡在 pending。 */
            AF_LOGW("paused quality switch commit kept no frame — falling back to the normal commit path\n");
        }
    }

    /*
     * 诊断（B2）：播放态提交的首帧就是 newestPendingFrame（见上面在分配循环之前
     * 的清零点与成员说明；暂停态在循环里已经记过了）。
     */
    if (!pausedSwitchCommit && newestPendingFrame != nullptr) {
        /* 同上面的 pts：有 media time 的帧已经在 FillPendingVideoFrame 里归一化过，
         * 这里只补加"没有 media time"那条老路的偏移。 */
        if (mPendingVideoPtsOffset != INT64_MIN &&
            newestPendingFrame->getInfo().timePosition < 0 &&
            newestPendingFrame->getInfo().pts != INT64_MIN) {
            newestPendingFrame->getInfo().pts += mPendingVideoPtsOffset;
        }

        mQualitySwitchCarriedFramePts = newestPendingFrame->getInfo().pts;
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
     * S5：暂停态提交之后，还欠"恰好一帧上屏"才能到终态。这里置上欠账标志，
     * 由 doRender() 的暂停分支负责把它还掉（渲染一帧后清假）。**注意仍然不动
     * 主时钟、不发位置回调**（S6）—— 这次切换只交换视频槽位。
     * 播放态保持 false：那条路由正常的 PLAYING 渲染完成交接。
     */
    mPausedSwitchRenderPending = (mSwitchStartedWhilePaused && mPlayStatus == PLAYER_PAUSED);

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

    /*
     * ============ 暂停态必须把旧帧计数清零（2026-09-24，矩阵第 1 格的关键）============
     *
     * 那个计数器是"promote 前 active 队列里的旧帧还剩几张没送出去"，而
     * finishQualitySwitch(true, ...) 的前置条件里有 `mQualitySwitchOldFramesPending == 0`
     * （RenderVideo 出口处判定）。**播放态**它能自然减到 0：主循环不停出帧，
     * 旧帧一张张被 RenderVideo 消费掉。
     *
     * **暂停态不行**：暂停时渲染只有 S5 那"恰好一帧"，谁去消费 active 队列里
     * 那几张旧帧？没有。于是计数器永远停在 oldVideoFramesPending > 0，
     * `finishQualitySwitch(true)` 永远不成立 —— 状态机最终由 QUALITY_SWITCH_DEADLINE_MS
     * 判 FAILED 收场：用户点了切档、画面其实已经换了，高亮却弹回旧档。
     *
     * 所以暂停态直接把计数清零：那些旧帧在暂停态本来就不会再上屏（画面停在
     * 用户暂停的那一帧，S5 只渲染新路那一帧），"旧帧是否送完"这个条件在暂停态
     * 没有意义。播放态一个字都不改。
     */
    if (mPausedSwitchRenderPending) {
        mQualitySwitchOldFramesPending = 0;
        AF_LOGI("paused quality switch commit: cleared the old-frame counter (was %d) — in PAUSED state "
                "only the single S5 frame is rendered, so waiting for the old frames to drain would "
                "never complete and the switch would be reported FAILED at its deadline\n",
                (int) oldVideoFramesPending);
    }

    /*
     * 【B10 配对表换代：提交时必须清 mVideoAxisPts / mVideoAxisTimePos】
     *
     * 这张表（见 .h 里 B10 那段说明）只给**当前 active 流**记录条目 —— 记录条件
     * 要求 mPendingVideoStreamIndex < 0（见 doDeCode 里的记录处）。所以在提交
     * 这一刻，表里剩下的**全是退役档**的 (raw pts → timePosition) 配对；而
     * FillVideoFrame 的配对命中（`mVideoAxisPts.front() == pFrame->getInfo().pts`）
     * 是按 raw pts **精确相等**匹配的：新路的首帧之后只要有一帧的 raw pts 恰好
     * 等于某个残留条目，它的 pts 就会被换成**旧档**的 timePosition ⇒ 单帧时间戳
     * 错位（错帧）。清空既不影响任何已归一化的帧（pts 在抽取时就已经写死），
     * 也不影响旧档（它的解码器已退役、不会再被 getFrame）；提交后的第一个
     * active 包会重新开始记录本档的配对。
     */
    mVideoAxisPts.clear();
    mVideoAxisTimePos.clear();
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
    /*
     * ============ 【B12：实测目标路的解码速率（千分比），供下一次切换选路用】============
     *
     * 解掉的媒体长度 = 提交帧的位置 − 预滚起解关键帧的位置。两者都要在**节目轴**上：
     * mPendingVideoPrerollKeyPts 是包的原始 pts，加 mActiveVideoPtsOffset 换算 —— 与提交门
     * 自己用的换算（:5987 那一带）同源，所以两条路的算术一致。
     *
     * 墙钟从**切换请求**起算（mPendingVideoSwitchStartMs），因此含着 open/seek/flush 的时间
     * ⇒ 只会**低估**速率 ⇒ 偏向"等关键帧"（今天的行为）；不会因为高估而选错路。
     * 纯测量，不参与任何判定；没有计时器（只读一次墙钟，和本函数其它统计日志同源）。
     */
    if (mPendingVideoPrerollKeyPts != INT64_MIN && mActiveVideoPtsOffset != INT64_MIN &&
        mPendingVideoSwitchStartMs > 0 && pts != INT64_MIN) {
        const int64_t decodedUs = pts - (mPendingVideoPrerollKeyPts + mActiveVideoPtsOffset);
        const int64_t wallUs = (af_getsteady_ms() - mPendingVideoSwitchStartMs) * 1000;

        if (decodedUs > 0 && wallUs > 0) {
            int64_t rateMilli = decodedUs * 1000 / wallUs;

            /* 夹到 [1.0x, 10x]：低于实时不可能（追不上就不会提交），高于 10x 视为时间戳抖动。 */
            if (rateMilli < 1000) {
                rateMilli = 1000;
            } else if (rateMilli > 10000) {
                rateMilli = 10000;
            }

            mPendingVideoDecodeRateMilli = static_cast<int>(rateMilli);
            AF_LOGI("quality switch: measured target decode rate %d/1000 real-time (decoded %lld ms of media "
                    "in %lld ms wall since the switch request) — the next switch's path choice will use it\n",
                    mPendingVideoDecodeRateMilli, (long long) (decodedUs / 1000), (long long) (wallUs / 1000));
        }
    }

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

    /*
     * ============ 音频不参与 seek 的"精确等待"（本轮重写）============
     *
     * 这里原来有两样东西，本轮全部删除：
     *   1. `!mSeekNeedCatch` 门：把整条音频路和"视频有没有吐出目标帧"绑在一起
     *      （视频解码器不吐帧 → 音频一帧不推 → AudioTrack 超时 → 外层状态机停播放器）；
     *   2. 为绕开 (1) 而加的 400ms 墙钟出口（SEEK_CATCH_AUDIO_UNBLOCK_MS）。
     *
     * 现在的规则：**音频按自己的 PTS 与缓冲推进，与视频是否产出无关**；
     * 需要整体停顿时，由"缓冲/停顿"状态统一停时钟与全部渲染（DoCheckBufferPass），
     * 而不是靠"等 N 毫秒还没等到就动手"的兜底。
     * 位置侧不受影响：getCurrentPosition() 仍被 mSeekPositionFloorUs 钉在目标点。
     */

    /*
     * 音频永远推，不再被"视频出没出帧"掐住。
     *
     * 原来这里是 `&& !mSeekNeedCatch`：mSeekNeedCatch 只在从解码器取到一帧时才清，
     * 于是"视频解码器一帧都不吐"会把整条音频路一起饿死，AudioTrack 超时后外层
     * 状态机就把播放器停掉（用户看到的"seek 一下就卡死"）。
     * 音频的时间轴由音频自己的 PTS 与缓冲决定，与视频是否产出无关；
     * 需要整体停顿时由"缓冲/停顿"状态统一停（见 DoCheckBufferPass），而不是靠这条门。
     */
    if (mCurrentAudioIndex >= 0) {
        /*
         * A 方案（落点关键帧语义）：seek 之后若音频第一帧还在主时钟**前面**，先别推它。
         * 否则音频设备位置一接回主时钟就会把时钟往前拽过落点，视频随即永久欠下
         * "目标点 − 落点"这段债；4K 解码只有约 1×，永远还不清 → 每帧都被判迟到丢掉
         * → 画面冻住、声音继续（用户实测的那条）。
         * 判据全是事件：主时钟（此时按系统时间从落点 1× 前进）走到队首音频帧的 PTS
         * 就自动恢复推送。只在 seek 窗口内生效（mSeekVideoAnchorDone / reanchorPending）。
         */
        bool holdAudioForSeek = false;

        /*
         * ============ 判据只在 seek 窗口内生效（本轮修"正常播放偶发静音"）============
         *
         * 原来第二支的条件里带着 `!mSeekVideoAnchorDone`，而这个闩在**任何一次 seek
         * 的落点帧被采纳之前恒为假** —— 于是整段正常播放（以及"某次 seek 的落点帧
         * 始终没上屏"之后的全部时间）里这一支都是开着的：只要音频队首 PTS 比主时钟
         * 超前 100ms 以上，音频就被按住不推。
         * 而带 9 帧缓冲下限的音频队列（≈190ms）**天然**会让队首 PTS 超前设备播放位置
         * 100ms 以上，于是正常播放中会反复出现"声音停一小会儿、然后自己恢复"
         * （设备把已写入的数据播完 → 时钟追上队首 → 门自动开）。
         *
         * 现在的判据只用两个真正属于 seek 窗口的状态：
         *   mSeekFlag                  —— seek 在途（落点帧上屏后清零）；
         *   mAudioClockReanchorPending —— seek 已结束但时钟还没交给音频
         *                                  （只在 FlushAudioPath 且 mSeekFlag 为真时置位）。
         * 两者都为假 = 正常播放：**照推不误**，不存在任何静音门。
         *
         * seek 窗口内也不是无条件按住：只在"音频队首已经跑到主时钟前面"时才按 ——
         * 否则推它等于把时钟往前拽过落点，视频随即欠下"目标点 − 落点"的债。
         * seek 之后主时钟按 1× 自由前进（getPlayingAudioTime() 此时返回 INT64_MIN
         * 的既有语义），所以这道门**自己会开**：时钟走到队首 PTS 就恢复推送，
         * 最多等 A/V 落点之差（≤1 个 GOP），**不会**因为视频一帧不出而永久静音
         * （那正是以前 mSeekNeedCatch 的老毛病，不能再引入）。
         */
        const bool inSeekAudioWindow = (mSeekFlag && !mSeekVideoAnchorDone) || mAudioClockReanchorPending;
        int64_t audioSilencePts = INT64_MIN;
        int64_t audioSilenceClock = INT64_MIN;

        /*
         * ============ 【B13：暂停态绝不按"等时钟追上"按住 PCM】============
         *
         * 下面这道门的前提是"时钟会自己往前走，走到队首 PTS 就开"。暂停态这个前提不成立：
         * ProcessPauseMsg → startRendering(false) → mMasterClock.pause()，时钟冻住，
         * "追上队首"永远不会发生 —— 于是 PCM 一直不推、以音频为参考的时钟也不动、
         * 视频落点帧的渲染门永远不满足（实测：暂停态 seek 之后 fps=0，画面与进度条都不动，
         * frameQ=1 而 master 已经越过落点）。
         *
         * 所以暂停态**不参与**这道门：音频照常走下面的 RenderAudio（它会按锚点丢掉旧时间轴的帧），
         * 恢复播放时从锚点（目标）起算。要等也只等"音频数据到达"，**不等时钟**。
         * 判据是纯状态（mPlayStatus），没有计时器；只影响暂停态，播放/缓冲路径逐字不变。
         */
        const bool seekHoldDisabled = (mPlayStatus == PLAYER_PAUSED);

        if (!seekHoldDisabled && inSeekAudioWindow && !mAudioFrameQue.empty() && mAudioFrameQue.front() != nullptr) {
            audioSilencePts = mAudioFrameQue.front()->getInfo().pts;
            audioSilenceClock = mMasterClock.GetTime();

            if (audioSilencePts > 0 && audioSilenceClock != INT64_MIN &&
                audioSilencePts > audioSilenceClock + 100 * 1000) {
                holdAudioForSeek = true;
            }
        }

        /*
         * 静音窗口起止各一条（只在状态变化时打，不是周期日志）。
         *
         * 【第 3 项：不再在窗口内反复打】
         *   原来每次 render() 都按"此刻有没有按住"上报，而窗口内"按住/放行"会在
         *   20~40ms 内交替（音频设备本身还有约 190ms 已写入的 PCM 在播），于是实测
         *   0.6s 内打了约 40 条 start/end 往返（安卓日志 338~376 行）。
         *   现在只在**真正跨越边界**时各打一条：
         *     · 进入 seek 窗口且确实按住了 → 一条 start；
         *     · 离开 seek 窗口 → 一条 end。
         *   窗口内的开合不逐条上报（那不是"声音没了"，是设计内的短暂让位）。
         *   另外这里只操作 reason==1 这一档：设备侧 reason=2/3 的记录不会被本处覆盖。
         */
        if (!inSeekAudioWindow) {
            if (mAudioSilenceReason == AUDIO_SILENCE_SEEK_CLOCK) {
                logAudioSilence(AUDIO_SILENCE_NONE, "left the seek window, pcm flows again",
                                audioSilencePts, audioSilenceClock);
            }
        } else if (seekHoldDisabled) {
            /*
             * 暂停态：上面那道门被禁用，PCM 照推。若之前已经报过"被按住"，这里把它收尾
             * （logAudioSilence 只在**状态变化**时输出，所以不是逐帧日志）。
             */
            if (mAudioSilenceReason == AUDIO_SILENCE_SEEK_CLOCK) {
                logAudioSilence(AUDIO_SILENCE_NONE,
                                "paused: the frozen clock cannot catch up, so pcm is not held any more",
                                audioSilencePts, audioSilenceClock);
            }
        } else if (holdAudioForSeek && mAudioSilenceReason == AUDIO_SILENCE_NONE) {
            logAudioSilence(AUDIO_SILENCE_SEEK_CLOCK,
                            "seek window: the pcm head is ahead of the master clock, holding pcm "
                            "until the clock catches up",
                            audioSilencePts, audioSilenceClock);
        }

        if (!holdAudioForSeek) {
        int ret;
        do {
            ret = RenderAudio();
            if (RENDER_NONE != ret) {
                audioRendered = true;
            }
        } while (ret == RENDER_FULL);
        }
    }

    if (HAVE_VIDEO) {
        videoRendered = RenderVideo(!mFirstRendered);
    }

    if (HAVE_VIDEO && HAVE_AUDIO) {
        // TODO: do it in reset()
        if (!PTS_REVERTING) {
            /* 不在该状态：把日志限频闩复位，下一次进入时还能各打一条。 */
            mPtsRevertLastLoggedAudio = INT64_MIN;

            if (videoRendered && audioRendered) {
                mVideoPtsRevert = mAudioPtsRevert = false;
            }
        } else {
            /*
             * 限频：只在音频位置**变化**时打一条（原来每轮一条，实测 1MB 日志整份被它打满）。
             * 不引入时间判据 —— 值没变就说明还卡在同一个状态，重复记录只会淹没日志。
             */
            if (mPtsRevertLastLoggedAudio != mPlayedAudioPts) {
                mPtsRevertLastLoggedAudio = mPlayedAudioPts;
                AF_LOGW("PTS_REVERTING audio pts is %lld ,video pts is %lld (logged on change; if this "
                        "stops appearing the pair is stuck in this state)\n",
                        (long long) mPlayedAudioPts, (long long) mPlayedVideoPts);
            }
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

    /*
     * C 方案（音频落点地板）：seek 落点 PTS 已知后，任何**早于落点**的音频帧都不再
     * 推给设备 —— 这些帧是音频解码器按 seek 前的旧起点解出来的（C 只裁了包队列，
     * 没有 flush 设备，所以它们还在 mAudioFrameQue 里）。丢掉它们，音频就和视频
     * 从同一个落点开始：A 的"音频等时钟"不需要（无 ≤1 个 GOP 静音），也不会把
     * 主时钟往前拽过落点。
     *
     * 返回 RENDER_NONE（与上面 pts==INT64_MIN 的丢弃一致），**不能**返回 RENDER_FULL：
     * 后者会让 render() 把本轮当成"渲染成功"（audioRendered=true），而 doRender()
     * 用 rendered 判定 seek 完成 —— 纯丢帧绝不能冒充"落点帧已上屏"。
     * 地板是事件置位、事件清零（首帧落点音频上设备时），不是时间判据。
     */
    if (mSeekAudioFloorUs != INT64_MIN) {
        /*
         * C + 第 4 条：**双侧**落点地板。seek 落点 PTS 已知后：
         *   · 低于落点的帧 —— 按旧起点解出来的（C 的原始判据）；
         *   · 高出"连续性高水位"超过容差的帧 —— 解码器**内部**残留的旧时间轴数据
         *     （输入队列 ≤16 包 + 输出队列 ≤10 帧），它们解出来晚、PTS 却落在 seek 之前
         *     的位置上（实测高出落点 8.8~11.9s）。这一类只判"低于落点"是挡不住的。
         * 两类都不是新时间轴的数据，丢掉即可；新时间轴的帧从落点连续到达（+≤21ms），
         * 会被接受并把高水位推进。
         *
         * 【本轮】改成**一次调用把整段不属于新时间轴的前缀丢完**，不再每轮只丢一帧。
         * 每轮一帧时，解码器内部那 ≤26 帧残留会以"主循环轮次"为单位拖住第一帧落点音频：
         * 主循环 30~100Hz 下就是额外 0.3~0.9 秒静音 —— 这正是"seek 后一段时间没声音"
         * 的第二个来源（第一个是下面出口里"声音等视频解码器"那个条件）。
         * 丢完前缀后**继续用新的队首**走正常推送路径，所以静音只剩"本轮"。
         *
         * 返回 RENDER_NONE（与上面 pts==INT64_MIN 的丢弃一致），**不能**返回 RENDER_FULL：
         * 后者会让 render() 把本轮当成"渲染成功"（audioRendered=true），而 doRender()
         * 用 rendered 判定 seek 完成 —— 纯丢帧绝不能冒充"落点帧已上屏"。
         */
        int drainedOldTimeline = 0;

        while (!mAudioFrameQue.empty() && mAudioFrameQue.front() != nullptr) {
            const int64_t headPts = mAudioFrameQue.front()->getInfo().pts;

            if (headPts == INT64_MIN) {
                break;
            }

            const bool headBelowLanding = (headPts < mSeekAudioFloorUs);
            const bool headFromOldTimeline =
                (mSeekAudioContinuityUs != INT64_MIN &&
                 headPts > mSeekAudioContinuityUs + SEEK_AUDIO_CONTINUITY_TOLERANCE_US &&
                 mSeekAudioStaleDrops < SEEK_AUDIO_STALE_DROP_MAX);

            if (!headBelowLanding && !headFromOldTimeline) {
                break;
            }

            if (headFromOldTimeline) {
                ++mSeekAudioStaleDrops;
                ++drainedOldTimeline;
            }

            mAudioFrameQue.pop_front();
        }

        if (drainedOldTimeline > 0) {
            AF_LOGW("seek audio align: drained %d frame(s) from the old timeline in one pass (watermark=%lld, "
                    "landing=%lld)\n",
                    drainedOldTimeline, (long long) mSeekAudioContinuityUs,
                    (long long) mSeekAudioFloorUs);
        }

        if (mAudioFrameQue.empty()) {
            return ret;
        }

        /* 上面可能刚丢掉了若干帧：队首变了，重新取时间戳（后面的推送与记账都用它）。 */
        pts = mAudioFrameQue.front()->getInfo().pts;
        position = mAudioFrameQue.front()->getInfo().timePosition;

        if (pts == INT64_MIN) {
            mAudioFrameQue.pop_front();
            return ret;
        }

        /* 新时间轴的帧：把连续性高水位推进到它（正常帧间隔由容差覆盖）。 */
        if (mSeekAudioContinuityUs == INT64_MIN || pts > mSeekAudioContinuityUs) {
            mSeekAudioContinuityUs = pts;
        }
    }

    /*
     * ============ 修法 1：落点音频首帧**先锚定主时钟，再让它上设备** ============
     *
     * 事件：本次 seek 的落点音频第一帧即将上设备。
     * 条件（全部是状态，无计时器）：
     *   · mSeekAudioFloorUs 已知（C 对齐事件把落点记下来的窗口内）；
     *   · 该帧就在落点附近：|pts − floor| ≤ SEEK_AUDIO_CONTINUITY_TOLERANCE_US（200ms，
     *     与双侧地板的容差同源，避免把"跳过去"的帧误当落点帧）；
     *   · 本次 seek 还没锚过（mSeekClockAnchored）。
     * 动作：把主时钟设到这一帧的 pts，并置 mSeekVideoAnchorDone（让 doRender 里
     * 那个视频锚点按既有 `!mSeekVideoAnchorDone` 条件自动不重复锚），清掉锚点事件闩。
     *
     * 为什么必须"先锚再推"（因果）：原来的顺序是"先推音频、后由视频锚点回拉时钟"。
     * 安卓实测（18:52:04）：04.531 设备 flush → 04.544 音频首帧已推给设备 →
     * 04.554 `renderer joining … (master=88590294)`（时钟还停在**目标点**）→
     * 04.596 视频锚点才把时钟拉到落点 85.085s → 04.603 **`audio silence starts
     * (reason=1 audioPts=85461333 masterClock=85092200)`**：音频在被回拉前已经
     * 突发推了几帧（85.085→85.46），时钟一回到落点，音频队首就超前 ~369ms，
     * render() 里那道"音频等时钟"的门（inSeekAudioWindow）立刻把后续音频按住，
     * 设备没 PCM → 写静音 → 用户听到 **0.3~0.4 秒的顿挫**，并伴随一串
     * `audio keep-alive: writing silence`。
     * 先锚之后：时钟在音频首帧上就已经等于内容位置，那 369ms 的超前从根上不存在，
     * A 门的第二个条件（mAudioClockReanchorPending）也在同一次调用的交接分支里
     * 立即清零 ⇒ 门不会再被触发 ⇒ 不再有这段静音。
     *
     * 位置语义零改动：本处只写主时钟，不碰 mSeekPositionFloorUs / getCurrentPosition()。
     */
    if (!mSeekClockAnchored && mSeekAudioFloorUs != INT64_MIN && pts != INT64_MIN &&
        llabs(pts - mSeekAudioFloorUs) <= SEEK_AUDIO_CONTINUITY_TOLERANCE_US) {
        /*
         * 这里同样收口到 fetchSeekClockAnchorUs()（与视频锚点共用唯一实现）：
         * 音频首帧只有在**不早于目标**（容差内）时才允许锚主时钟。早于目标的那一帧
         * 同样只上设备、不锚时钟 —— 否则时钟会被拉回落点，位置上报与追赶窗口一起错位
         * （实测 0.083s vs 目标 2.844s 就是这条路径的形状）。
         * 不满足规则时**不清** mSeekAnchorPending / mSeekVideoAnchorDone：锚点事件留给
         * 后面真正 >= 目标的帧（音频下一帧、或视频落点帧上屏事件）。
         */
        const int64_t seekAudioAnchorUs = fetchSeekClockAnchorUs(pts, "audio landing first frame");

        if (seekAudioAnchorUs != INT64_MIN) {
            mMasterClock.setTime(seekAudioAnchorUs);
            mSeekClockAnchored = true;
            mSeekVideoAnchorDone = true;
            mSeekAnchorPending = false;
            AF_LOGW("seek anchor (audio landing first frame): pts=%lld floor=%lld target=%lld anchor=%lld\n",
                    (long long) pts, (long long) mSeekAudioFloorUs, (long long) mSeekPositionFloorUs,
                    (long long) seekAudioAnchorUs);
        }
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

        /*
         * 诊断（每次 seek 后第一帧音频只打一条，天然限频 —— 因为 1b 会把 mPlayedAudioPts
         * 重置为 INT64_MIN，所以这个分支每个 seek 只会走到一次）：
         * 直接回答"1b 对齐之后音频首帧的 PTS 到底是多少"。
         *   pts          = 落点对齐后第一帧音频自己的 PTS；
         *   landingFloor = 1b 记下的视频落点地板（对齐目标）；
         *   target       = 用户 seek 的目标点（位置上报仍钉在它上面）。
         * 三者若明显不一致，就说明音频包的读取位置与视频落点不在同一处
         * （解复用侧把音频定位到了目标点，而视频只能从关键帧起步）。
         */
        if (mSeekAudioFloorUs != INT64_MIN || mSeekFlag || mAudioClockReanchorPending) {
            AF_LOGI("audio first frame after seek: pts=%lld landingFloor=%lld target=%lld\n",
                    (long long) pts, (long long) mSeekAudioFloorUs, (long long) mSeekPositionFloorUs);
        }

        /*
         * 落点音频的第一帧已经交给设备（上面的 renderAudioFrame 已成功）：
         * C + 第 4 条的落点地板/连续性高水位完成使命，一起清零。
         * 之后进来的帧 PTS 单调递增，地板不会再命中；对齐窗口关闭后本判据不再干预
         * （真正的时间戳跳跃/缺包由下游既有机制处理，不被这里误伤）。
         */
        mSeekAudioFloorUs = INT64_MIN;
        mSeekAudioContinuityUs = INT64_MIN;
        mSeekAudioStaleDrops = 0;

        /*
         * A 方案：锚点是**视频落点关键帧的 PTS**，不是音频帧的 PTS。
         * 视频落点帧还没采纳（mSeekVideoAnchorDone=false）之前，这里**不接音频时钟**、
         * 也不清 mAudioClockReanchorPending —— 主时钟此时按系统时间从落点 1× 前进
         * （getPlayingAudioTime() 返回 INT64_MIN 的既有语义），音频帧被 render() 里
         * 那道门挡着等到时钟追上。等落点帧上屏，下面的分支自然接上音频时钟；
         * 交接时两者数值已经一致，不会再把时钟往前拽。
         */
        bool handOverToAudio = true;

        /*
         * ============ 【B13：暂停态不按音频帧改写时钟、也不把时钟交给音频】============
         *
         * 机制（都可静态核对）：
         *   · af_clock::set() 在**暂停**态改的是"暂停值"（framework/utils/af_clock.cpp:55-57
         *     `mPauseUs = us`）⇒ 暂停态调 mMasterClock.setTime(pts) 等于把**冻结的时钟**
         *     挪到音频帧的位置上；
         *   · SystemReferClock::GetTime() 在时钟运行起来之后会按音频参考重同步
         *     （mediaPlayer/system_refer_clock.cpp:16-45）⇒ 一旦交接给音频，恢复播放时
         *     时钟从**音频帧的位置**起算，而不是 seek 的目标点。
         * 两者叠加就是用户看到的"暂停期间 master 从 0.083s 变成 3.09s、恢复后从错误时间点起播"。
         *
         * 处理：暂停态**时钟保持不动**（seek 时已被钉在目标上），并且**不交接**给音频；
         * mAudioClockReanchorPending 留着不清，等恢复播放后的第一帧音频再走同一条既有交接
         * 路径（那时时钟在跑，交接才是正确动作）。判据是纯状态（mPlayStatus），没有计时器；
         * 这里不打日志（本函数每帧都走，暂停态又不再被上面的门挡住，逐帧打会刷屏）。
         */
        if (mPlayStatus == PLAYER_PAUSED && mAudioClockReanchorPending) {
            handOverToAudio = false;
        }

        if (handOverToAudio && mAudioClockReanchorPending) {
            if (!mSeekVideoAnchorDone) {
                handOverToAudio = false;
                AF_LOGI("seek: holding the clock at the video landing point until the frame lands "
                        "(audio pts=%lld)\n", (long long) pts);
            } else if (!mSeekClockAnchored) {
                mMasterClock.setTime(pts);
                mSeekClockAnchored = true;
                AF_LOGI("audio clock re-anchored after seek at pts=%lld\n", (long long) pts);
            } else {
                AF_LOGI("seek anchored by the video landing frame, audio pts=%lld follows it\n",
                        (long long) pts);
            }

            if (handOverToAudio) {
                mAudioClockReanchorPending = false;
            }
        }

        if (handOverToAudio) {
            mMasterClock.setReferenceClock(getAudioPlayTimeStampCB, this);
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

            /*
             * 偏差修正不阶跃（对齐主流做法：小偏差渐进、真跳变才一次性对齐）。
             *   · |偏差| <= 100ms ：每帧最多并入 5ms，几十帧内平滑收敛，画面不会顿一下；
             *   · |偏差| > 100ms  ：一次性对齐（这是时间戳跳变/换段，属于事件，不是定时兜底）；
             *   · |偏差| > 10s    ：判定根本不同步，放弃修正（同 ffplay 的 AV_NOSYNC 阈值）。
             * 原来 >100ms 就把整段累积值一次并入，主时钟一跳、视频路就要丢/重排帧。
             */
            const int64_t kGradualStepUs = 5000;
            const int64_t kDiscontinuityUs = 100 * 1000;
            const int64_t kReanchorUs = 1000 * 1000;
            const int64_t kNoSyncUs = 10 * 1000 * 1000;

            if (llabs(mAudioTime.deltaTimeTmp) > kNoSyncUs) {
                mAudioTime.deltaTimeTmp = 0;
            } else if (llabs(mAudioTime.deltaTimeTmp) > kReanchorUs) {
                /*
                 * 【第 5 项：>1s 的偏差不是"漂移"，是音频时间轴换了参照】
                 *
                 * 原来 >100ms 就"纠偏"（把整段偏差并进 mAudioTime.deltaTime），而音频是主时钟，
                 * 于是主时钟被一步拽走。Qt 实测：
                 *   `correct audio and master clock offset is 8789339` →
                 *   `TIMEPOS reSync time 20888010 to  29674671` → 约 230 帧连续丢帧
                 * （落点 20.833s 的时钟被拽到 29.674s，视频永久欠 8.8s 的债）。
                 *
                 * >1s 只可能来自"参照被换掉"（seek 后音频队列换了一批、解码器/设备重启、
                 * 换段/换码流），这时正确的动作是**重新锚定 A/V 对**：把音频时间轴直接落到
                 * 当前帧自己的 PTS 上，偏差不并入 deltaTime，主时钟因此不会阶跃 ——
                 * 主时钟的锚点仍然只由"落点帧事件"决定（第 1 项）。
                 * 小偏差（<=1s）保持原有渐进策略：每帧最多 5ms，几十帧内收敛。
                 */
                AF_LOGW("audio timeline re-anchored (offset %lld > 1s): audio pts=%lld, frameDuration=%lld "
                        "— not folding it into the clock\n",
                        (long long) mAudioTime.deltaTimeTmp, (long long) pts,
                        (long long) mLastAudioFrameDuration);

                /*
                 * 【A 方案：置闩】音频时间轴在 seek 之后前跳 > 1s = "容器把音频放到了别处"。
                 * 实测（安卓 14:34:51）落点 110.110s 而音频下一帧 112.512s（前跳 2026 ms），
                 * 频道因此写不出 PCM 达 2s，被 Android 判 underrun 停轨 1.45~1.6s。
                 * 这里**只记录事实**（置闩 + 记数值），不调解复用器：本函数在渲染/读包
                 * 路径上，调解复用器会造成重入与跨线程占用。真正的处置在主循环里消费。
                 */
                mSeekAudioRepositionPending = true;
                mSeekAudioRepositionUs = (mSeekAudioFloorUs != INT64_MIN) ? mSeekAudioFloorUs
                                                                          : mSeekPositionFloorUs;
                mSeekAudioRepositionJumpUs = mAudioTime.deltaTimeTmp;

                mPlayedAudioPts = pts;
                mAudioTime.startTime = pts;
                mAudioTime.deltaTimeTmp = 0;
                mAudioTime.deltaTime = 0;
            } else if (llabs(mAudioTime.deltaTimeTmp) > kDiscontinuityUs) {
                AF_LOGW("correct audio and master clock offset is %lld, frameDuration :%lld", mAudioTime.deltaTimeTmp,
                        mLastAudioFrameDuration);
                mAudioTime.deltaTime += mAudioTime.deltaTimeTmp;
                mAudioTime.deltaTimeTmp = 0;
            } else if (mAudioTime.deltaTimeTmp > kGradualStepUs) {
                mAudioTime.deltaTime += kGradualStepUs;
                mAudioTime.deltaTimeTmp -= kGradualStepUs;
            } else if (mAudioTime.deltaTimeTmp < -kGradualStepUs) {
                mAudioTime.deltaTime -= kGradualStepUs;
                mAudioTime.deltaTimeTmp += kGradualStepUs;
            }

            /*
             * ============ 【B8】把"纠偏累加器"限幅，禁止它把主时钟拉偏成单向漂移 ============
             *
             * 主时钟 = `mAudioTime.startTime + mAudioTime.deltaTime + aoutPos`
             * （见 getAudioPlayTimeStamp()）：`aoutPos` 是设备播放头（实时速率），
             * 而 `mAudioTime.deltaTime` 是上面这个**纠偏累加器**。
             *
             * 上面每帧做的是：`offset = pts − (mPlayedAudioPts + mLastAudioFrameDuration)`
             * 累加进 deltaTimeTmp，超过 5 ms 就**每帧往 deltaTime 里搬 5 ms**
             * （kGradualStepUs，上面那两行）。只要"解码器给的帧时长"与"容器 pts 步进"
             * 存在**系统性**不一致（例如 AAC-HE/SBR 的样本率、变长帧、容器 timebase 舍入），
             * 这个搬运就会**持续单向**进行：最坏 5 ms/帧 × 21.5 帧/秒 ≈ **100 ms/秒**，
             * 几分钟就是几秒 —— 用户看到的就是"越播越偏、最后和画面里的字幕完全对不上"。
             *
             * 处置（保守、可回退）：给纠偏量一个**上限**（±300 ms）。真实的小漂移照旧
             * 用渐进搬运收敛；而需要超过 300 ms 才能"修正"的那种情况，本质是**参照被换了**
             * （上面 >1s 那条 re-anchor 分支管的正是它），绝不应该是"把主时钟一路拖走"。
             * 限幅之后偏差最多停在 0.3 s（可用 videoDelay 再精修），而不是无限增长。
             *
             * 同时打一行事件驱动诊断（每累计 100 ms 一行，无计时器）：直接看出
             * `deltaTime` 是否在单向爬、爬多快 —— 这是判定根因（是 deltaTime 还是 aoutPos）
             * 的唯一可靠证据，不再靠猜。
             */
            const int64_t kMaxClockCorrectionUs = 300 * 1000;

            if (mAudioTime.deltaTime > kMaxClockCorrectionUs) {
                mAudioTime.deltaTime = kMaxClockCorrectionUs;
            } else if (mAudioTime.deltaTime < -kMaxClockCorrectionUs) {
                mAudioTime.deltaTime = -kMaxClockCorrectionUs;
            }

            /*
             * 诊断打印的**行数上界是常数级**：`deltaTime` 已经被上面限幅在 ±300 ms，
             * 而水位每 100 ms 才动一次 ⇒ 单方向最多 3~6 行；换向时把水位清零重新起算，
             * 这样两个方向都能看到，但仍然不会刷屏（不需要额外的限流器、也没有计时器）。
             */
            if ((mAudioTime.deltaTime >= 0) != (mAudioClockDriftLoggedUs >= 0)) {
                mAudioClockDriftLoggedUs = 0;
            }

            if (llabs(mAudioTime.deltaTime) >= llabs(mAudioClockDriftLoggedUs) + 100 * 1000) {
                mAudioClockDriftLoggedUs = mAudioTime.deltaTime;
                AF_LOGW("audio clock drift: correction deltaTime=%lld us (capped at +-%lld), startTime=%lld, "
                        "aoutPos=%lld, lastFrameDuration=%lld, audio pts=%lld — if this keeps growing in one "
                        "direction the audio timeline bookkeeping (durations vs pts) is off, not the video\n",
                        (long long) mAudioTime.deltaTime, (long long) kMaxClockCorrectionUs,
                        (long long) mAudioTime.startTime,
                        (long long) mAVDeviceManager->getAudioRenderPosition(),
                        (long long) mLastAudioFrameDuration, (long long) pts);
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

    /*
     * 【死循环根因 1】seek 在途且落点尚未采纳时，**不允许评估/保留**"视频时间戳倒回"判据。
     * 一次 seek 本来就会让视频从目标之前的关键帧重新开始（实测落点 40.09s vs 暂停位置
     * 51.99s，Δ≈一个 GOP≈11.9s，于是 40.09 < 51.99-11.9 恰好成立），把它当成"时间戳倒回"
     * 正是下面那个永久等待分支的起点。这里连置位都不允许，并且把可能残留的闩清掉 ——
     * 这样即使某次渲染发生在 FlushVideoPath 之前，也不会有闩活到 seek 之后。
     * 判据本身不改（TS 不连续时它仍然有效），只是 seek 窗口内不参与。
     */
    const bool seekLandingPending = (mSeekRenderGateUs != INT64_MIN && !mSeekLandingFrameAccepted);

    if (seekLandingPending) {
        mVideoPtsRevert = false;
    } else if (!mVideoPtsRevert) {
        mVideoPtsRevert = mPlayedVideoPts != INT64_MIN && videoPts < mPlayedVideoPts - mPtsDiscontinueDelta;

        if (mVideoPtsRevert) {
            AF_LOGI("PTS_REVERTING video start\n");
        }
    }

    /*
     * ============ 修"seek 之后一直转圈 + 日志刷满"（本轮）============
     *
     * 下面这两个分支是给**普通播放中的时间戳不连续（TS discontinuity）**用的启发式：
     * 视频先"倒回去"、音频还没倒（或反之）时先别渲染，等两路都倒过来。
     *
     * 但**一次 seek 本来就会让视频倒回去**：暂停帧恢复（PFR）把 seek 目标定在暂停帧
     * 之后，而 demuxer 只能落在目标之前的关键帧上（实测落点 40.09s vs 暂停位置 51.99s，
     * 差 11.9s，该文件 IDR 极稀疏）。此刻 mVideoPtsRevert 被置真（40.09 < 51.99），
     * 而暂停中 mPlayedAudioPts 是**暂停前留下的旧值**、mAudioPtsRevert 恒假
     * ⇒ 这个分支永远满足"等音频倒回来"，每轮 return false 把所有帧挡在渲染之前。
     * 而它位于 K1 落点采纳块（下面 mSeekRenderGateUs 那段）**之前**，于是落点帧
     * 永远采纳不了、mSeekFlag 永远清不掉 = "seek 一下就一直转圈加载"。
     * 实测日志：12:35:43.019 `seekFlag=1 playStatus=6(PAUSED) frameQ=1 packetQ(v)=3901
     * master=56351616`，同毫秒 3 行、每秒数百~上千轮，整份 1MB 日志都是它。
     *
     * 处置（小、可静态论证，无任何超时判死）：
     *   · 有 seek 落点待采纳时（mSeekRenderGateUs 已置位且尚未采纳）**跳过这两个分支**：
     *     seek 是显式重锚，落点采纳必须优先于这条不连续启发式；
     *   · 落点被采纳时（K1 那一段）清掉两个 revert 闩 —— seek 不是时间戳不连续，
     *     启发式从新时间轴重新起算（这就是它的**事件出口**）；
     *   · 日志只在**进入等待/强制渲染状态**时打一条、离开时再打一条
     *     （mPtsRevertWaitLogged），不再每轮打。
     */
    if (!seekLandingPending) {
    // audio pts first revert to small, force render the old video frame
    if (PTS_REVERTING && mAudioPtsRevert && videoPts - mPtsDiscontinueDelta > mPlayedAudioPts) {
        if (mPtsRevertWaitLogged != 1) {
            AF_LOGI("PTS_REVERTING force render the old video frame (state entered; audio pts=%lld video pts=%lld)\n",
                    (long long) mPlayedAudioPts, (long long) videoPts);
            mPtsRevertWaitLogged = 1;
        }
        force_render = true;
        // video pts first revert to small,the new video data wait audio pts to revert
    } else if (PTS_REVERTING && mVideoPtsRevert && mPlayStatus == PLAYER_PLAYING &&
               videoPts + mPtsDiscontinueDelta < mPlayedAudioPts) {
        /*
         * 【死循环根因 2】必须要求"正在播放"：这一支在等的是"音频也倒回来"
         * （mAudioPtsRevert 置真），而**暂停时不会有任何音频帧被渲染** ⇒ mAudioPtsRevert
         * 永远不可能变真 ⇒ 这是"等一个逻辑上不可能发生的事件"，必然永久阻塞。
         * 实测 1MB 日志正是这个状态：playStatus=6(PAUSED)、seekFlag=1、每秒数百~上千轮
         * 重复 2~3 行，视频帧一直停在 mVideoFrameQue 队首不动。
         * 暂停态下视频的"时间戳倒回"由 seek 落点事件负责重锚（见 K1 采纳块的清零），
         * 不需要也不可能靠音频倒回来收尾。
         */
        if (mPtsRevertWaitLogged != 2) {
            AF_LOGI("PTS_REVERTING wait audio to revert (state entered; audio pts=%lld video pts=%lld; "
                    "logged once per episode)\n",
                    (long long) mPlayedAudioPts, (long long) videoPts);
            mPtsRevertWaitLogged = 2;
        }
        return false;
    } else if (mPtsRevertWaitLogged != 0) {
        AF_LOGI("PTS_REVERTING wait/force-render state left (audio pts=%lld video pts=%lld)\n",
                (long long) mPlayedAudioPts, (long long) videoPts);
        mPtsRevertWaitLogged = 0;
    }
    }

    int64_t masterPlayedTime = mMasterClock.GetTime();

    /*
     * 【B5-5】主时钟还没建立起来时（哨兵值 / "哨兵 + elapsed"的环绕值），任何"早/晚"
     * 判定都没有意义：拿它去丢帧只会把起播的头几帧丢掉 —— 真机日志里
     *   `drop frame,master played time is 9223372036854775570,video pts is 250250`
     * 连刷约 10 条，就是主时钟被环绕值污染之后的结果。
     * 判据是纯状态判断（见 af_clock.h 里 af_clock_value_is_unset()），没有时间阈值、
     * 没有看门狗；命中时一律**放行这一帧**，等主时钟有了真值再按节拍走。
     * 正常播放/seek/切档路径下这个值为 false，判定与今天逐字相同。
     */
    const bool masterClockUnset = af_clock_value_is_unset(masterPlayedTime);
    /*
     * 主时钟未建立时不能做这个减法：masterPlayedTime 是"哨兵 + elapsed"的环绕值，
     * 与 pts 相减可能**溢出**（有符号溢出是 UB），得到的结果也无意义。
     * 取 0 = "正好准时"，后续所有分支都会选择**放行这一帧**（见下面的 masterClockUnset 护栏）。
     */
    int64_t videoLateUs = masterClockUnset ? 0 : (masterPlayedTime - videoPts);// > 0 表示视频落后于主时钟
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

        /*
         * S7：这张旧帧也算"被消费掉了"，必须递减 mQualitySwitchOldFramesPending。
         *
         * 那个计数器记的是"promote 之前 active 队列里的旧帧还剩几张没送出去"，
         * 而 finishQualitySwitch(true, ...) 的前置条件里有 `mQualitySwitchOldFramesPending == 0`。
         * 原来的递减只写在函数末尾那条正常路径上（见本函数结尾），**被这里提前
         * return 跳过了** —— 在没有启用 hold 窗口的平台上无所谓（hold 只在提交后
         * 短暂生效），但 hold 窗口与"旧帧还没送完"重叠时，计数器会停在 1，
         * 于是 READY 被推迟；极端情况（旧帧刚好只剩这一张）下永远发不出 READY，
         * 最后只能靠死线判 FAILED —— 用户看到的就是"切成功了但高亮弹回去"。
         */
        if (mQualitySwitchCommitPending && mQualitySwitchOldFramesPending > 0) {
            --mQualitySwitchOldFramesPending;
        }

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
        if ((!mMasterClock.haveMaster() || !mMasterClock.isMasterValid()) && !mSeekFlag &&
            !mPausedSwitchRenderPending) {
            /*
             * S6：暂停态切档期间**不锚时钟**。这里的第一张帧是"暂停点之前的关键帧
             * 到暂停点之间"解出来的，它的 pts 比用户暂停的位置更早或更晚都可能；
             * 一旦锚上去，用户暂停时看到的位置就被改写了（而且它还会通过
             * getCurrentPosition 推给界面）。暂停语义要求时间**绝对不动**。
             * 停用条件只在"暂停切档欠一帧"期间成立，播放态与普通 seek 不受影响。
             */
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
     * 追赶（dropLateVideoFrames）**没有时间上限**了（本轮改成事件驱动）：
     * 它由 beginRendererJoining() 打开，只由"有一帧准时落到主时钟上"这一事件关闭
     * （见下面 RenderVideo 里清 dropLateVideoFrames 的那一处）。
     * 原来这里有一个"3 秒窗口到点就撤"的墙钟出口，属于看门狗同类物，已删除。
     * 若解码器本身长期跟不上实时（4K 软解之类），表现就是持续丢帧 —— 这是**如实**
     * 反映管线能力，由"缓冲/停顿"状态上报给界面，而不是靠时间偷偷改回"每帧都渲染"
     * 把画面变成慢动作。
     */

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
    /*
     * ============ 落点帧即上屏（K1，本轮新增）============
     *
     * 位置地板 mSeekPositionFloorUs 的语义一个字节都不改（它只服务 getCurrentPosition()，
     * 保证进度条不回退）。渲染改用独立的 mSeekRenderGateUs：
     *   · 本次 seek 的第一张视频帧（落点同步帧）直接接受并上屏，同时把主时钟锚到它
     *     （一次 seek 只锚一次：mSeekClockAnchored）；
     *   · 接受之后闸门立刻撤销，后面所有帧都不再受"目标点"约束，按正常节拍播；
     *   · 只有在 mSeekExactLanding 打开（旧语义）时才继续把目标点之前的帧挡掉。
     *
     * 为什么必须重锚主时钟：seek 一发起主时钟被钉在"用户目标点"，而落点帧的 PTS 可能
     * 比它早最多一个 GOP；不重锚的话落点帧会被后面的迟到帧判据直接丢掉，画面照样出不来。
     */
    /*
     * 【1a：落点判定必须与 force_render 解耦】
     *
     * 原来这里的条件是 `render && …`，而这一行之前 `render` 恰好就等于 force_render
     * （节拍判定在下面才跑），于是**只有 force_render 为真时**才会走落点采纳/锚定。
     * 而 doRender() 一轮主循环里会调**两次**渲染：
     *   1. seek 分支 `RenderVideo(true)`（force_render=true）；
     *   2. 紧随其后的 `render()` → `RenderVideo(!mFirstRendered)`（:5138）；
     * 首帧上屏之后 mFirstRendered 恒真，于是第 2 次调用 force_render=false 就把整块
     * 落点逻辑跳过了 —— 只要"结束 seek 的那一帧"是第 2 次调用渲染的（安卓实测：
     * `seekFlag` 在 16~23ms 内被清、且全文件 0 条 `seek landing frame accepted`），
     * 主时钟就永远停在 ProcessSeekToMsg 写的**目标点**上，而视频是从目标点之前的
     * 关键帧解过来的（实测落点落后 1.6~4.6s）—— 于是帧帧"迟到"、画面冻住。
     *
     * 改成"只要还在 seek 窗口（mSeekRenderGateUs 已被 SeekTo 置位）就判定"：
     * 谁渲染这一帧都要先过落点采纳与锚定，两条调用路径都不可能绕过。
     * 采纳后 mSeekRenderGateUs 立刻归 INT64_MIN，所以每次 seek 仍然只采纳一次、
     * 只锚一次（锚定另有 mSeekVideoAnchorDone 闩）。
     */
    if (mSeekRenderGateUs != INT64_MIN && (render || mSeekFlag)) {
        /*
         * 【timePosition 缺失的容器/流也必须能精确落点】
         *
         * 有些容器/流不填 timePosition（全局时间轴位置）。原来整块落点判定被
         * `if (frameTimePos >= 0)` 挡在门外：那条来源的 seek **永远不采纳、不锚定** ——
         * 于是既没有"落点帧"概念，也没有精度可言（这正是"某些片源 seek 不跳转"的形状）。
         * 这里在缺失（< 0）时退回帧自己的 pts：两者在同一时间轴、单位相同，
         * 判据与采纳/锚定逻辑一字不改。
         */
        int64_t frameTimePos = videoFrame->getInfo().timePosition;

        if (frameTimePos < 0) {
            frameTimePos = videoFrame->getInfo().pts;
        }

        if (frameTimePos >= 0) {
            /*
             * 【100% 帧级精准 seek 的判据基础】
             *
             * 这一帧在时间轴上覆盖 [frameTimePos, frameTimePos + 帧长)。
             * "包含目标时刻的那一帧"就是 frameTimePos <= target < frameTimePos + 帧长 ——
             * 也就是目标要求的"目标落在两帧之间时取不晚于目标的那一帧"。
             * 帧长优先用帧自己的 duration（AFFrameInfo.duration，见
             * framework/base/media/IAFPacket.h:208）；缺失时按 25fps 兜底，
             * 兜底只影响"目标离帧首不到一帧"的边界判定，绝不会把早很多的帧放上屏。
             */
            /*
             * 帧长优先取帧自己的 duration（AFFrameInfo.duration，IAFPacket.h:208）；
             * 缺失时**按本流实际帧率推算**，不再写死 25fps —— 写死会让 60fps 片源
             * "早一帧"就被判成"包含目标"而采纳（一帧 16.7ms 却被当成 40ms）。
             * avg_fps 的取法与本文件 :1669 的既有惯用法完全一致。
             */
            int64_t seekFrameDurUs = videoFrame->getInfo().duration;

            if (seekFrameDurUs <= 0) {
                const int seekFps = (mCurrentVideoMeta != nullptr)
                                    ? std::max(1, (int) (mCurrentVideoMeta->operator Stream_meta *()->avg_fps))
                                    : 25;
                seekFrameDurUs = 1000000 / seekFps;
            }

            if (!mSeekLandingFrameAccepted) {
                if (!mSeekDecodeStartIsKey) {
                    /*
                     * 花屏（马赛克）修复：解码不是从关键帧起步（demuxer 落点在 GOP 中间）时，
                     * 这一帧缺参考帧、是脏数据 —— **不显示**，保持上一张好画面，
                     * 直到关键帧解出来（DecodeVideoPacket 里把 mSeekDecodeStartIsKey 置真）。
                     * 最多等一个 GOP；绝不把脏帧显示出去。
                     */
                    render = false;
                } else if (frameTimePos != INT64_MIN && mSeekPositionFloorUs != INT64_MIN &&
                           frameTimePos + seekFrameDurUs <= mSeekPositionFloorUs) {
                    /*
                     * ============ 【★ 还没到目标：不采纳、不上屏，一直解到"包含目标"那一帧 ★】====
                     *
                     * 原来这里是一条**预算**判据（SEEK_EXACT_LANDING_BUDGET_US / 只在
                     * mSeekExactLandingByBudget 被授予时才生效）：比目标早不超过预算就放上屏。
                     * 那等于"精度换等待"—— 精度取决于预算给不给，而不是取决于目标本身；实测表现
                     * 就是落点帧可能比目标早一整个 GOP，暂停态 seek 尤其明显。
                     *
                     * 现在改成与**目标本身**比较的确定性判据（无预算、无计时器）：
                     *   · 本帧 + 帧长 <= 目标 ⇒ 这一帧完全在目标之前，不包含目标 ⇒ 不上屏，继续解；
                     *   · 否则（本帧 <= 目标 < 本帧 + 帧长）⇒ 它**就是**包含目标的那一帧 ⇒ 走下面的
                     *     采纳分支上屏；
                     *   · 帧 PTS 单调前进 ⇒ 必然在有限帧内到达包含目标的那一帧，不需要任何超时兜底；
                     *   · 目标落在 seek 落点之前（稀疏 IDR，第一帧就晚于目标）⇒ 此时
                     *     frameTimePos > 目标 ⇒ 条件不成立 ⇒ 直接采纳它（没有更早的帧可选）。
                     *
                     * 这一处是**来源无关**的：本地文件、DASH、HLS 的帧都经由同一个
                     * mVideoFrameQue 走到这里，所以三条来源同时获得同样的精度。
                     * ==========================================================================
                     */
                    /*
                     * 【B15：先出画、再精确 —— 前缀解码期间不再冻着旧画面】
                     *
                     * 走到这里说明三件事同时成立：
                     *   · 本次 seek 的落点帧还没被采纳（mSeekLandingFrameAccepted 为假）；
                     *   · 解码**是从关键帧起步**的（上面 !mSeekDecodeStartIsKey 那一支已经把
                     *     "落点在 GOP 中间、缺参考帧的脏帧"挡在门外）—— 手里这一帧是干净的、
                     *     可以显示的；
                     *   · 它完全落在目标之前（本帧 + 帧长 <= 目标），**不包含目标**。
                     *
                     * 原来这里无条件 render = false。那个精度判据本身是对的（不包含目标的帧
                     * 不能当落点），但副作用是**整段前缀解码期间屏幕一直停在 seek 之前那张
                     * 旧画面**上：从落点解到目标要多久，用户就看到画面冻多久 —— 这就是
                     * "seek 之后画面不动 / 卡住"的直接观感来源（与解码快慢无关的那部分）。
                     *
                     * 现在改成"**只把第一张干净的帧先放上屏**，其余前缀帧照旧丢"：
                     *   · 放上屏 ⇒ 旧画面立刻被替换成时间轴上离目标最近的已知帧，
                     *     和主流播放器一致（先到附近、再精确落位）；
                     *   · 只放一张 ⇒ mSeekFirstDecodableFrameShown 是事件闩（SeekTo / Reset
                     *     复位），后面的前缀帧仍然走原来的 render = false，一帧都不多显示；
                     *   · **不碰任何精度状态**：不置 mSeekLandingFrameAccepted、不动
                     *     mSeekRenderGateUs、不动 mSeekPositionFloorUs —— 所以下面那个
                     *     "包含目标 ⇒ 采纳 + 强制上屏"的分支一个字都没改，精确帧到达时
                     *     **替换**这一帧，最终落点仍然是 100% 帧级精确；
                     *   · 也**不会动时间轴**：主时钟锚点由 fetchSeekClockAnchorUs 把关，
                     *     它明确**拒绝**早于目标的帧（"seek anchor refused"），本次 seek 的
                     *     锚点事件（mSeekAnchorPending / mSeekVideoAnchorDone）保持未消费，
                     *     等真正包含目标的那一帧再来锚。
                     */
                    if (!mSeekFirstDecodableFrameShown) {
                        mSeekFirstDecodableFrameShown = true;
                        force_render = true;
                        render = true;

                        AF_LOGI("seek first decodable frame shown: pts=%lld is %lld ms before the seek "
                                "target=%lld — replacing the previous picture now (the frame that CONTAINS "
                                "the target will replace this one as soon as it is decoded; the landing "
                                "judge and the clock anchor are untouched)\n",
                                (long long) frameTimePos,
                                (long long) ((mSeekPositionFloorUs - frameTimePos) / 1000),
                                (long long) mSeekPositionFloorUs);
                    } else {
                        render = false;
                    }
                } else if (mSeekPositionFloorUs != INT64_MIN &&
                           mSeekLandingFloorOwnerUs != INT64_MIN &&
                           mSeekPositionFloorUs != mSeekLandingFloorOwnerUs) {
                    /*
                     * 【② B18：落点窗口的"归属"判据】
                     *
                     * 地板 mSeekPositionFloorUs 是"本次 seek 的目标"的载体；归属标记
                     * mSeekLandingFloorOwnerUs 与它**同点写入**。两者不一致 = 这次 seek 之后
                     * 地板被别的路径改写过了，它已经不代表本次落点的目标 ⇒ 这一帧不能算落点。
                     *
                     * 处置：不采纳、不上屏这一帧，并立刻把整个落点窗口关掉（闸门/地板/归属）。
                     * 关窗口有两个作用：
                     *   · 不会每帧重复判断（避免刷日志、避免长期占着闸门）；
                     *   · 满足"落点窗口必须有明确出口"：即使这一帧没被采纳，seek 结束时
                     *     ResetSeekStatus() 还会再关一次（幂等）。
                     * 之后的位置上报由真实位置承担（getCurrentPosition() 地板已撤），
                     * 后续帧交给正常节拍逻辑 ⇒ 不会冻住。
                     */
                    AF_LOGW("seek landing refused: the position floor %lld does not belong to this "
                            "seek any more (owner=%lld, frame pts=%lld) — not accepting it as the "
                            "landing frame and closing the landing window\n",
                            (long long) mSeekPositionFloorUs, (long long) mSeekLandingFloorOwnerUs,
                            (long long) frameTimePos);

                    /*
                     * 【② B18-b：拒绝之后必须**立刻出画**，不能把窗口关掉让画面留在旧帧上】
                     *
                     * 现场：`seek landing refused … 34722 ms LATER than the seek target …` 之后
                     * 窗口被关掉，B15（先出画）与 B16（追赶阀门）都跟着失效 —— 下一帧既没有
                     * "落点采纳"可以强制上屏、也不在追赶窗口里，用户看到的就是"seek 后卡死半天不动"。
                     *
                     * 处置：不关窗，而是把落点窗口**重开在当前实际读位置**上，并让这一帧立刻上屏：
                     *   · 能走到这里说明 mSeekDecodeStartIsKey 已经为真 ⇒ 这一帧是"从关键帧起步
                     *     解出的干净帧"，不是脏帧（脏帧在更上面那一支就被挡住了）；
                     *   · 地板与归属一起写成这一帧的位置 ⇒ 下一帧位置必然 >= 新地板 ⇒ 走正常
                     *     采纳分支（采纳 + 强制上屏），位置上报从此钉在**真实读位置**上，
                     *     不再出现"进度条停在旧目标、画面在另一个位置"；
                     *   · 出口照旧齐全（不会悬挂）：采纳时关闸门、ResetSeekStatus() 兜底关窗、
                     *     getCurrentPosition() 的地板过期自愈三条都还在，且这里只剩一次拒绝
                     *     （重开后的地板与真实读位置同轴，不会再触发"晚出容差"那条）；
                     *   · 精度判据未改：正常 seek（帧落在目标附近）仍然由原来的采纳分支处理，
                     *     只有"目标已经不可达"的这种异常状态才改判落点。
                     */
                    mSeekPositionFloorUs = frameTimePos;
                    mSeekLandingFloorOwnerUs = frameTimePos;
                    mSeekFirstDecodableFrameShown = true;
                    force_render = true;
                    render = true;
                } else if (mSet->maxASeekDelta > 0 && frameTimePos != INT64_MIN &&
                           mSeekPositionFloorUs != INT64_MIN &&
                           frameTimePos - mSeekPositionFloorUs > mSet->maxASeekDelta) {
                    /*
                     * 【② B18：比目标晚出"精确 seek 容差"以上的帧不算落点】
                     *
                     * 安卓日志里的荒谬读数就是这一支漏掉的：
                     *   `seek landing frame accepted: pts=99933167, -97006 ms before the seek target`
                     * —— 地板是用户 seek 目标 2.927s，而帧在 99.93s（晚了 97s），却被当成
                     * "目标落在落点之前、没有更早的帧可选"而采纳，同时把闸门一起消费掉。
                     *
                     * 判据用**既有的精确 seek 容差** mSet->maxASeekDelta（默认 21s，语义就是
                     * "第一次读到的帧离目标多远还算精确 seek"），不引入任何新阈值/计时器：
                     * 超过它就不认这是本次 seek 的落点。
                     *
                     * 处置：不采纳、不强制上屏，并关掉落点窗口（同上一支）。这一帧之后交给
                     * 正常节拍逻辑 ⇒ 画面继续走（不冻），位置上报回到真实位置；用户再 seek
                     * 会重新写入地板与归属 ⇒ 正常落点照样采纳（这条只影响"目标已经不可达"的
                     * 异常状态）。
                     */
                    AF_LOGW("seek landing refused: the frame pts=%lld is %lld ms LATER than the seek "
                            "target %lld (beyond the accurate-seek tolerance %lld ms) — not accepting "
                            "it as the landing frame (the read position was moved by another path) and "
                            "closing the landing window\n",
                            (long long) frameTimePos,
                            (long long) ((frameTimePos - mSeekPositionFloorUs) / 1000),
                            (long long) mSeekPositionFloorUs,
                            (long long) (mSet->maxASeekDelta / 1000));

                    /*
                     * 【② B18-b：同一处置 —— 拒绝但**不关窗**，把窗口重开在当前实际读位置，
                     * 并让这一帧立刻上屏】理由与上面"归属不一致"那一支逐字相同：
                     * 走到这里的帧是"从关键帧起步的干净帧"；重开后下一帧必然被正常采纳；
                     * 出口（采纳关闸门 / ResetSeekStatus 兜底 / 地板过期自愈）一条不少，
                     * 而且地板与真实读位置同轴之后不会再复发"晚出容差"这条拒绝。
                     */
                    mSeekPositionFloorUs = frameTimePos;
                    mSeekLandingFloorOwnerUs = frameTimePos;
                    mSeekFirstDecodableFrameShown = true;
                    force_render = true;
                    render = true;
                } else {
                mSeekLandingFrameAccepted = true;
                mSeekRenderGateUs = INT64_MIN;
                mSeekExactLandingByBudget = false;

                /*
                 * 【事件出口】落点帧被采纳 = 本次 seek 的时间轴已经重新锚定。
                 * 两个 revert 闩必须在这里清零：seek 不是"时间戳不连续"，而暂停态 seek
                 * （PFR）会让 mVideoPtsRevert 置真、mAudioPtsRevert 因暂停而恒假，
                 * 若不清零，下面的 PTS_REVERTING 等待分支会永久挡住渲染
                 * （实测：seekFlag 永远清不掉 = 一直转圈加载）。清掉之后由新时间轴重新起算。
                 */
                mVideoPtsRevert = false;
                mAudioPtsRevert = false;
                mPtsRevertWaitLogged = 0;
                /*
                 * 落点帧**强制上屏**（本轮修"4K 一直加载"）：
                 * 主时钟是音频驱动的，而音频锚在 seek 目标点上；落点关键帧的 PTS 通常
                 * 比目标点早半个到两个 GOP，于是下面这段节拍逻辑会把落点帧判成
                 * "迟到 > 500ms"直接丢掉（videoLateUs > 500ms → 不渲染）。
                 * 一旦落点帧被丢，mSeekFlag 就永远清不掉（它只在"有帧真的上屏"时清），
                 * 于是 SeekEnd 不来、界面一直转圈；而音频继续 1× 前进，后面的帧只会
                 * "越来越迟"，全部被丢 —— 直到 1s 的 resync 兜底才勉强恢复。
                 * 所以这里必须同时置 render 与 force_render：跳过整段节拍判定，只放这一帧，
                 * 之后由事件驱动的追赶逻辑（dropLateVideoFrames 直到有帧落回时钟）收敛。
                 */
                force_render = true;
                render = true;

                if (!mSeekVideoAnchorDone) {
                    const int64_t beforeUs = (mSeekPositionFloorUs == INT64_MIN)
                                             ? 0
                                             : (mSeekPositionFloorUs - frameTimePos);

                    /*
                     * 【本轮：锚点只留一处】这里原来还顺手写主时钟
                     * （mMasterClock.setTime(frameTimePos) + mSeekClockAnchored + mSeekVideoAnchorDone）。
                     * 那是**第二处锚点**，而且用的是 frameTimePosition 这条时间轴，与渲染路径全程
                     * 使用的 pts 时间轴并非同一来源 —— 两处锚点、两条时间轴，谁先到就按谁锚，
                     * 正是"越改越乱"的来源。
                     * 现在锚点统一由 doRender() 里"seek 后第一帧真的上屏"的事件承担（只用
                     * mPlayedVideoPts，与节拍/丢弃判定同一条时间轴）。本处只负责三件事：
                     *   · 接受落点帧（mSeekLandingFrameAccepted：音频等待窗口与 seek 完成判定要读它）；
                     *   · 撤掉渲染闸门（mSeekRenderGateUs = INT64_MIN，之后一帧都不再挡）；
                     *   · 强制这一帧上屏（force_render + render，避免被"迟到"判据丢掉）。
                     */
                    AF_LOGW("seek landing frame accepted: pts=%lld, %lld ms before the seek target — "
                            "rendering it now (the master clock is anchored by the first-frame-rendered "
                            "event, not here)\n",
                            (long long) frameTimePos, (long long) (beforeUs / 1000));
                }
                }
            } else if (frameTimePos + SEEK_TARGET_DROP_AHEAD_US >= mSeekPositionFloorUs) {
                mSeekPositionFloorUs = INT64_MIN;
            } else if (frameTimePos + SEEK_FLOOR_GIVEUP_US < mSeekPositionFloorUs) {
                mSeekPositionFloorUs = INT64_MIN;
            } else if (mSeekExactLanding) {
                render = false;
            }
        }
    }

    if (!force_render) {
        /*
         * 【2026-09-22 修：早帧一律等时钟 —— 预热阶段**不再**例外】
         *
         * （这里原来还有一个局部变量 qualitySwitchWarming，由 mPendingVideoStreamIndex /
         * mPendingVideoDecoderSwitch / mQualitySwitchCommitPending 三项组成；例外删掉之后
         * 它已经没有任何读者，为避免留一个"看起来还在起作用"的死变量，一并删掉。
         * 提交之后的"无条件出画"由下面单独判 mQualitySwitchCommitPending，与本段无关。）
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
        if (!masterClockUnset && videoLateUs < -10 * 1000 &&
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

            if (!masterClockUnset && videoLateUs > JOINING_STALE_FRAME_US && renderedRecently) {
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

    /*
     * 【B16：追赶不收敛 ⇒ 一帧都不送 —— 出口】
     *
     * 判据（纯状态、无计时器）：播放态下渲染判定已经连续给出"不上屏"共
     * mVideoDiscardStreak 帧，而且这些帧与主时钟的距离 |master - videoPts| **始终没有变小**
     * （见下面丢弃分支里的累计规则）。这正是安卓日志里的形态：
     *   3.09s 内 246 条 `drop frame`（≈79 帧/秒，100% 的帧被拒）、
     *   主时钟与视频帧两条轴**同速同向前进**、偏移恒定（~3s）、期间 0 次上屏、
     *   0 条 `seek landing frame accepted` ⇒ 解码在前进、差距不收敛 ⇒ 画面永久冻住。
     *
     * 处置：把**这一帧**强制放上屏（只此一帧，计数在上屏分支立刻清零）。这样
     *   · 画面至少随解码前进，不再"永不收敛 ⇒ 永远冻住"；
     *   · 节拍/追赶语义不变：上屏后计数归零，下一次仍从 0 开始累计，
     *     所以不会退化成"每帧都放行"；
     *   · **精度零影响**：本行位于落点判据（上面的 mSeekRenderGateUs 块）之后，
     *     落点帧若已被采纳，render 早就是 true，根本走不到这里；本行也不改主时钟、
     *     不改位置上报、不动任何锚点闩 —— 被放行的帧是解码器正常输出的干净帧，
     *     不是"缺参考帧的脏帧"（脏帧在落点块里就被 mSeekDecodeStartIsKey 挡住了）。
     * 暂停态一律不适用（mPlayStatus != PLAYER_PLAYING）：暂停语义要求时间绝对不动。
     */
    const int64_t videoGapAbsUs = masterClockUnset ? INT64_MIN : llabs(videoLateUs);

    /*
     * 【B16-b 补充】追赶期（dropLateVideoFrames）里的另一条出口：偏移可能只是**缓慢**收敛
     * （安卓实测 ~2~3ms/帧），"偏移不再变小"永远不成立，可画面已经冻了好几秒
     * （`drop frame` 连续数百条，偏移从 0.76s 慢慢往下爬）。所以在**非 seek 落点窗口**
     * （mSeekRenderGateUs == INT64_MIN）内，连续被拒帧数达到
     * VIDEO_CATCHUP_DISCARD_STREAK_MAX 也必须放行一帧 —— 宁可极短暂慢放，不冻住。
     * 与 seek 前缀互不干扰：前缀期间 mSeekRenderGateUs != INT64_MIN，这条不参与。
     */
    const bool catchUpValveTripped = (mSeekRenderGateUs == INT64_MIN && dropLateVideoFrames &&
                                      mCatchUpDiscardStreak >= VIDEO_CATCHUP_DISCARD_STREAK_MAX);

    if (!render && mPlayStatus == PLAYER_PLAYING && !masterClockUnset &&
        (mVideoDiscardStreak >= VIDEO_STUCK_DISCARD_STREAK_MAX || catchUpValveTripped)) {
        render = true;

        AF_LOGW("video frames were rejected without the picture being updated (consecutive=%d, "
                "catchUpConsecutive=%d, gap=%lld ms, master=%lld, video pts=%lld, playStatus=%d, "
                "seekFlag=%d, dropLate=%d) — forcing this frame to the screen so the picture keeps "
                "moving instead of freezing; the pacing/landing rules are untouched and the counters "
                "reset here\n",
                mVideoDiscardStreak, mCatchUpDiscardStreak,
                (long long) (videoGapAbsUs / 1000),
                (long long) masterPlayedTime, (long long) videoPts,
                (int) mPlayStatus, (int) mSeekFlag, (int) dropLateVideoFrames);
    }

    if (render) {
        /* 画面又在动了：清掉“追赶中”，主循环恢复正常的等待间隔
         * （读取点在 mainService()，见 mVideoCatchingUp 的说明）。 */
        mVideoCatchingUp = false;
        /* B16：有帧真的上屏 ⇒ 连续拒帧计数与偏移采样一起归零（下一次从 0 开始累计）。 */
        mVideoDiscardStreak = 0;
        mVideoDiscardGapAbsUs = INT64_MIN;
        /* B16-b：追赶期阀门同样在上屏后清零（下一次仍要连续 20 帧才放行）。 */
        mCatchUpDiscardStreak = 0;
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
         *
         * 【READY 到底挂在哪一步 —— B2 之后必须把这条说清】
         * 判据就是下面这两条同时成立：
         *   (1) mQualitySwitchOldFramesPending == 0 —— 提交前 active 队列里的旧帧已经
         *       全部被 RenderVideo 消费掉；
         *   (2) 代码走到这里 —— 这一帧已经经过 SendVideoFrameToRender（真的交给渲染
         *       路径，tunnel 直通下即 releaseOutputBuffer(render=true) 交给真 Surface）。
         * 也就是说 READY 由"帧真的被送出去"驱动，而不是"帧被解出来"。
         * B2 交接（TryCommitPendingVideoSwitch 里的 B2 块）**刻意不 flush**：
         * preroll 已经从关键帧起解、提交时 pending 手里就有可渲染的帧，
         * 所以这里上屏的通常是"preroll 那一代"的帧 —— 交接后 ~140ms 就能到 READY
         * （flush 会把这一代扔掉，逼出 6.4s 等关键帧 + 4.2s 等时钟，实测 10.6s 静止，
         * 那条路已被真机否定）。
         */
        if (mQualitySwitchCommitPending && mQualitySwitchCommittedStreamIndex >= 0 &&
            mQualitySwitchOldFramesPending == 0) {
            /*
             * 诊断（B2，每次切换一行）：交接后**第一帧真的进真面**时打印它的 pts、
             * 预滚起解的关键帧 pts、提交时从 pending 带过来的帧 pts，以及从提交
             * 到这一帧上屏的真实延迟。
             * 坐标轴说明（重要，别误读）：
             *   · pts / carriedPendingPts 都是**流水线归一化后**的 pts（同一把尺子）；
             *   · prerollKeyPts 是**包的原始 pts**（与上面两个差一个
             *     mActiveVideoPtsOffset），只作"这帧离预滚关键帧多远"的参考。
             * 判定口径：
             *   carriedPendingPts == pts ⇒ 上屏的就是"preroll 那一代、交接前
             *   已经解好的帧"（本方案期望的路径，预期百 ms 级出画）；
             *   carriedPendingPts == INT64_MIN 或不等 ⇒ 这一帧是交接后新解出来的
             *   （也正常：解码器没有 flush，继续往下解即可，不需要关键帧）。
             */
            const int64_t b2AttachToFrameMsFinal =
                    (mQualitySwitchCommitMs > 0) ? (af_getsteady_ms() - mQualitySwitchCommitMs) : -1;
            const bool b2FirstIsPrerollGeneration =
                    (mQualitySwitchCarriedFramePts != INT64_MIN && videoPts != INT64_MIN &&
                     mQualitySwitchCarriedFramePts == videoPts);
            AF_LOGI("post-handover first frame: pts=%lld prerollKeyPts=%lld carriedPendingPts=%lld "
                    "attachToFirstFrameMs=%lld source=%s\n",
                    (long long) videoPts, (long long) mPendingVideoPrerollKeyPts,
                    (long long) mQualitySwitchCarriedFramePts, (long long) b2AttachToFrameMsFinal,
                    b2FirstIsPrerollGeneration ? "preroll-generation" : "decoded-after-attach");
            AF_LOGI("quality switch rendered: stream=%d size=%dx%d (READY driver: oldFramesPending==0 and this "
                    "frame went through SendVideoFrameToRender; B2 hands the real surface over without flushing, "
                    "so the preroll generation can go to the real surface right away)\n",
                    mQualitySwitchCommittedStreamIndex, frameWidth, frameHeight);
            finishQualitySwitch(true, "quality switch rendered");
        }

        if (!HAVE_AUDIO) {
            /* 纯视频片源：第一张真正上屏的帧就是时钟锚点。
             * 但 **seek 期间不能锚** —— 理由和上面 RenderVideo 里那段一样：
             * seek 目标点的时钟已经由 ProcessSeekToMsg 钉住，而这里的第一张帧可能
             * 是解码器冲到目标点后面 2~3 秒才解出来的，锚上去等于把时钟推飞。 */
            if (mPlayedVideoPts == INT64_MIN && !mSeekFlag && !mPausedSwitchRenderPending) {
                /*
                 * S6：暂停态切档期间不锚时钟（理由同上一条 guard）。暂停切档可能
                 * 是"纯视频片源 + 暂停中切档"这种组合，此时 mPlayedVideoPts 恰好
                 * 可能还是 INT64_MIN，是最容易误锚的一条路。
                 */
                mMasterClock.setTime(videoPts);
                mMasterClock.setReferenceClock(mClockRef, mCRArg);
            }
        }
    } else {
        /*
         * 【B5-5】主时钟未建立时不再打印环绕值（`master played time is
         * 9223372036854775570` 这种），直接标成 unset —— 否则读日志的人会以为
         * 时钟真的跑到了 INT64_MAX 附近。
         */
        if (masterClockUnset) {
            AF_LOGW("drop frame,master played time is unset (clock not established yet),video pts is %lld\n",
                    (long long) videoPts);
        } else {
            AF_LOGW("drop frame,master played time is %lld,video pts is %lld\n",
                    (long long) masterPlayedTime, (long long) videoPts);
        }

        videoFrame->setDiscard(true);
        mUtil->videoRendered(false);
        mMPAUtil->videoRendered(false);
        mVideoCatchingUp = true;
        /*
         * 【B16】连续拒帧的累计规则：只有"与主时钟的距离**没有变小**"才计入
         * （= 两条轴恒速同向、偏移恒定 ⇒ 追赶不收敛）；距离在收敛（例如 seek 期间
         * 时钟钉在目标上、解码从落点朝目标推进）就回到 1，正常追赶完全不受打扰。
         * 纯事件计数：没有计时器、没有墙钟阈值。
         */
        /*
         * 【修：连续拒帧计数不再被"偏移抖动"清零 ⇒ 防冻阀门真的能用】
         *
         * 原判据是"|master - videoPts| 没有变小才计入"，而它把**任何**一次减小
         * （哪怕 1us）都当成"在收敛"并把计数清回 1。主时钟与视频帧是两条各自
         * 16.7ms 步进的轴，差值本身就在 ±1 帧内抖动 —— 于是计数永远攒不到
         * VIDEO_STUCK_DISCARD_STREAK_MAX(8)，阀门形同不存在。真机日志（安卓
         * 2026-09-26 20:55:54）就是这个形态：`drop frame,master played time is
         * 163268733,video pts is 153570083` 连续刷了 10 秒以上，而
         * `forcing this frame to the screen` 在整份日志里出现 **0 次** ⇒ 画面真的
         * 冻死（用户看到的"有时候完全卡死""有声音没画面"）。
         *
         * 现在改成纯粹的"连续被拒帧数"：只有真的上屏（下面 render == true 那一段）
         * 才清零，与差值的抖动无关。偏移采样仍然保留，只用于日志/诊断。
         * 精度不受影响：这条只在"渲染判定已经连续 8 帧给出不上屏"之后放行**这一帧**，
         * 位于落点判据之后，不碰落点、不碰主时钟、不碰位置上报。
         */
        if (videoGapAbsUs != INT64_MIN) {
            mVideoDiscardGapAbsUs = videoGapAbsUs;

            if (mVideoDiscardStreak < VIDEO_STUCK_DISCARD_STREAK_MAX + 1) {
                ++mVideoDiscardStreak;
            }
        }
        /*
         * 【B16-b】追赶期的连续被拒计数：只看**帧数**（"偏移是否变小"不作为条件，
         * 因为缓慢收敛时它一直在变小却仍然冻屏）；但只在不处于 seek 落点窗口时累计
         * —— 那条窗口由 B15 单独负责，两套口径互斥。
         */
        if (dropLateVideoFrames && mSeekRenderGateUs == INT64_MIN) {
            if (mCatchUpDiscardStreak < VIDEO_CATCHUP_DISCARD_STREAK_MAX + 1) {
                ++mCatchUpDiscardStreak;
            }
        } else {
            mCatchUpDiscardStreak = 0;
        }
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
            /*
             * 【B12】新的一次切换：允许"两路代价取小"在预滚入口重新判一次。
             * 不在这里复位的话，第二次切换会沿用上一次的选择（旧参考点已经作废）。
             */
            mPendingVideoPrerollPathChosen = false;
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

            /*
             * S1（暂停态无感切档）：切换请求已经落到 pending 路上 —— 在这里
             * 一次性快照"本次切档是不是发起于暂停态"以及冻结参考时刻。
             *
             * 位置很关键：必须在上面所有"取消上一个 pending / 关流 / 清队列"
             * 的收尾**之后**（这样快照的是本轮状态，不会被上一轮覆盖），
             * 又必须在首个目标包进入解码循环**之前**（后面所有判据都要读它）。
             *
             * 对播放中路径零影响：beginQualitySwitchTracking() 里唯一的条件就是
             * mPlayStatus == PLAYER_PAUSED，PLAYING 时 mSwitchStartedWhilePaused
             * 恒为 false，后面新增的所有分支都以该标志为前置，因此播放路径的
             * 行为与改动前逐字等价。
             */
            beginQualitySwitchTracking();
        }
    } else if (pFrame->getInfo().streamIndex == mCurrentAudioIndex || pFrame->getInfo().streamIndex == mWillChangedAudioStreamIndex) {
        // printTimePosition(pFrame->getInfo().timePosition);
        mHaveAudioPkt = true;

        if (mFirstAudioPts == INT64_MIN) {
            mFirstAudioPts = pFrame->getInfo().pts - pFrame->getInfo().timePosition;
            mFirstSeekStartTime = pFrame->getInfo().timePosition;
        }

        if (mSeekFlag && mSeekExactLanding && mSeekNeedCatch &&
            NeedDrop(pFrame->getInfo().timePosition, mSeekPos)) {
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

void SuperMediaPlayer::logAudioSilence(int reason, const char *detail, int64_t audioPts, int64_t clockUs)
{
    /*
     * 静音窗口**只在状态变化时**各打一条（开始一条、结束一条）。
     *   · 不是周期日志：reason 与上次相同就直接返回，所以不会随时间刷屏；
     *   · 也不是判据：本函数不改变任何播放行为，只把"现在有没有声音、为什么"写进日志，
     *     让下一份日志能直接区分是 1（内核等时钟）还是 2/3（设备侧欠载/被重建）。
     * 编码定义见文件头部 AUDIO_SILENCE_*；设备侧同名编码在 AudioTrackRender.cpp。
     */
    if (reason == mAudioSilenceReason) {
        return;
    }

    if (reason == AUDIO_SILENCE_NONE) {
        AF_LOGW("audio silence ends (was reason=%d): pcm flowing again, audioPts=%lld masterClock=%lld\n",
                (int) mAudioSilenceReason, (long long) audioPts, (long long) clockUs);
    } else {
        AF_LOGW("audio silence starts (reason=%d audioPts=%lld masterClock=%lld): %s\n",
                (int) reason, (long long) audioPts, (long long) clockUs,
                detail != nullptr ? detail : "");
    }

    mAudioSilenceReason = reason;
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
    /*
     * 【B14】音频设备刚被 flush（seek / 内部重建）：重新开始观察"设备位置有没有真的前进"。
     * 与 mAudioClockReanchorPending 同生共死，语义绑定在一起（见 getAudioPlayTimeStamp）。
     */
    mAudioClockProgressBaseUs = INT64_MIN;
    mAudioClockProgressSeen = false;
    mAudioClockProgressLogCount = 0;
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
     * 【B19】入口先记下"这一次 flush 会不会发出 CANCELED 终态"：真正要补做的 PFR 必须等
     * 函数末尾（pending 索引清成 -1 之后）才能做，否则补做入口会把还没清掉的
     * mPendingVideoStreamIndex 读成"切档仍在途"而再次推迟。纯状态快照，不做任何动作。
     */
    const bool switchCancelNotifiedOnEntry = (cancelPendingSwitch && mPendingVideoStreamIndex >= 0);

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
        /*
         * 【B19 终态出口 3/3：CANCELED（status=3）】未提交就被取消的切档不走
         * finishQualitySwitch（那里 hadCommit 为假会直接 return），所以"补做被推迟的 PFR"
         * 必须在**本函数末尾**（pending 状态全部拆干净之后）做一次 —— 见函数结尾处
         * `switchCancelNotifiedOnEntry` 那一段。放在这里会把 mPendingVideoStreamIndex
         * 仍 >= 0 的状态读成"切档还在途"，于是又推迟，永远补不上。
         */
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
    /* B10：时间轴重建（seek/stop/换源）时逐帧配对表必须一起丢掉，否则旧代的 pts 可能撞上新帧 */
    mVideoAxisPts.clear();
    mVideoAxisTimePos.clear();
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

    /*
     * pending 解码器状态被清掉之后，下一次进目标流必须重新贴参数集（SPS/PPS）。
     * 见 attachPendingVideoCodecParams()：这个标志是"当前 pending 解码器收没收到
     * 过 CSD"，随着 pending 路被 flush/discard 一起失效，留着会让新解码器缺参数集
     * ——马赛克（-1094995529）就是这么来的。
     */
    mPendingVideoCodecParamsAttached = false;
    /*
     * B2：pending 解码器（连同它"用占位 Surface 配置"的身份）已经被上面的
     * discardPendingVideoDecoder() 丢掉了，这个标志必须跟着清 —— 留着会让下一次
     * 提交误判"pending 是 surface 模式"。
     */
    mPendingDecoderUsesPlaceholderSurface = false;

    /*
     * 暂停态切档的三个新状态也在这里清零（矩阵第 3、8 格）。
     * FlushVideoPath() 是"seek 中止在途切档 / stop / 换源 / 后台 flush"的公共
     * 出口 —— finishQualitySwitch() 走的是"切档自己收敛"那条路，本函数走的是
     * "外力中止"那条路，两条都必须清干净，否则 mSwitchStartedWhilePaused /
     * mPausedSwitchRenderPending 会跨片源残留，让下一次操作误走暂停切档分支。
     */
    resetPausedSwitchState();

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

    /*
     * 【B19 终态出口 3/3：CANCELED（status=3），放在**函数末尾**】
     *
     * 到这里 pending 路的一切状态（mPendingVideoStreamIndex / mWillChangedVideoStreamIndex /
     * preroll 闩 / retired 索引）都已经拆干净，所以"切档在途"判据一定为假，补做入口这次
     * 真的会把被推迟的 PFR 发出去（且只在暂停态发、先清闩、不递归）。
     * 条件用入口快照：只有"这次 flush 确实取消了在途切档"才补做 ——
     *   · Stop/换源/后台 flush 的 FlushVideoPath 也带 cancelPendingSwitch=1，但那时
     *     mPlayStatus 不是 PLAYER_PAUSED，补做入口会丢弃这一次；Reset()/Prepare 还会
     *     直接清闩；
     *   · 普通 seek（cancelPendingSwitch=0）不在这里补做 —— 它的 PFR 由"seek 结束 →
     *     重新装弹的切档 → 那个切档的终态"这条链补，避免 seek 期间插 seek。
     */
    if (switchCancelNotifiedOnEntry) {
        runDeferredPauseFrameRestore();
        /*
         * B20：切档被这次 flush 打成 CANCELED 时，finishQualitySwitch() 这条终态出口
         * 不会再到，所以在**这里**补做被推迟的用户 seek（入口内先清闩，不递归；
         * 且它自己会用播放状态把 stop / 换源 / 后台 flush 这三种情况丢掉）。
         * 普通 seek 的 flush（cancelPendingSwitch=0）不进这个分支。
         */
        replayDeferredUserSeek();
    }
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
        /*
         * 当前位置：seek 在途时它等于 mSeekPos；seek 已宣告结束、管道还在从落点往目标
         * 追赶时它被 mSeekPositionFloorUs 钉在**目标点**上（见 getCurrentPosition()）。
         */
        const int64_t position = getCurrentPosition();

        if (duration >= 0) {
            mBufferPosition = position + duration;
        } else if (mSeekPositionFloorUs == INT64_MIN) {
            /* 非 seek 窗口：真实缓冲时长测不出来（包队列暂时空）时不发布，维持原行为。 */
            return;
        } else {
            /*
             * 【2026-09-26 修：seek 之后"缓冲条先往回缩、再弹回来"】
             *
             * 这个分支只在"seek 已经宣告完成（isSeeking() 为假，所以上面那道闸门放行）、
             * 但管道还没走到目标点（mSeekPositionFloorUs 还在）"这段窗口里进得来。
             *
             * 分片流的 seek 会把整段包缓存清空再从目标重下（见 SMPMessageControllerListener
             * 里 seek 分支的说明），此刻 getPlayerBufferDuration() 常常返回 -1（音频/视频
             * 队列都还没数据）。原代码此时**什么都不发布**，于是：
             *   · 播放头那一路刚刚上报了 seek 目标（OnTimer 里 NotifyPosition），
             *   · 缓冲条那一路还停在**上一个播放周期**的旧值上（向前 seek 时它小于目标，
             *     也就是落在播放头后面）。
             * 界面看到的就是"缓冲条缩到播放头后面，等真实缓冲时长测出来再弹回去"。
             *
             * 修正：这段窗口里把缓冲条末端**报成当前上报位置**（即 seek 目标）。
             * 两层理由：一，PFR 已经承诺把目标点之前的数据对齐好，界面上的播放头本来就在
             * 目标点，缓冲条末端早于播放头是不可能的取值；二，报"目标点"不会像以前那样
             * 把缓冲条先窜到很远再塌回来 —— 它就是播放头当前位置，等真实时长测出来只会
             * 从这里**往后长**。无计时器、不新增成员；seek 窗口之外一个字节都不改。
             */
            mBufferPosition = position;
        }

        if (mEof) {
            mBufferPosition = mDuration;
        }

        mPNotifier->NotifyBufferPosition((mBufferPosition <= mDuration ? mBufferPosition : mDuration) / 1000);
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
    /*
     * 【B5-2：把"切档耗时"拆成可判定的两段】
     *
     * 真机实测（2026-09-25，DASH + 4K，GL 路）：从 UI 的
     * `PLAYER_QUALITY_SWITCH_STARTED` 到新档首帧上屏共 0.86~1.41 s，其中
     * "真面交接"只占 240 ms（`post-handover first frame … attachToFirstFrameMs=240`），
     * 其余全在**目标档的准备**上 —— 而准备的头两步（下面 OpenStream / Seek）是
     * **同步、且要等网络**的，它们发生在 `mPendingVideoSwitchStartMs` 计时零点**之前**，
     * 所以在既有日志里是看不见的一段。这里把这两步各自计时打出来，
     * 让"到底是 OpenStream、Seek、还是预滚等分片"一眼可判 —— 只加日志，不改任何行为。
     * 结论性证据（同一份日志）：预滚起点那条行里 `master-ref = 0.776 s`，而要跨越的
     * 媒体长度 `lead = 4.529 s` ⇒ 0.78 s 墙钟内交付 4.53 s 目标档媒体，且该窗口内
     * 一帧都没解（跳过发生在喂解码器之前）⇒ 耗时在网络/分片粒度，不在解码或选择逻辑。
     */
    const int64_t switchVideoStartMs = af_getsteady_ms();
    int ret = mDemuxerService->OpenStream(targetStreamIndex);

    if (ret < 0) {
        AF_LOGW("switch video open stream failed, target stream index %d\n", targetStreamIndex);
        mPNotifier->NotifyVideoQualitySwitch(PLAYER_QUALITY_SWITCH_FAILED,
                                             targetStreamIndex,
                                             "target video stream open failed");
        mWillChangedVideoStreamIndex = -1;
        return;
    }

    AF_LOGI("quality switch: target stream %d opened in %lld ms (synchronous demuxer open)\n",
            targetStreamIndex, (long long) (af_getsteady_ms() - switchVideoStartMs));

    /*
     * 不要在这里关闭旧流。SwitchVideo() 只是启动目标路，旧流要一直保留到
     * TryCommitPendingVideoSwitch() 完成并且新帧真正上屏；否则 pending 路还在
     * 追关键帧时，active decoder 会立刻断粮，音画时钟自然分离。旧流的关闭由
     * 提交后的安全点执行。
     */
    const int64_t seekStartMs = af_getsteady_ms();
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

    AF_LOGI("quality switch: target stream %d seeked to %lld us in %lld ms (synchronous demuxer seek)\n",
            targetStreamIndex, (long long) startTime,
            (long long) (af_getsteady_ms() - seekStartMs));

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

    /*
     * 【B5-5】`mAudioTime.startTime` 还没被赋过值时它是哨兵值，而
     * `startTime + deltaTime + aoutPos` 会算出一个**环绕值** —— 真机日志里
     * `[AlivcPlayerClock] TIMEPOS reSync time 30018965 to -9223372036824759809`
     * 的那个 referTime 就是它。
     * 本函数的契约本来就是"音频时钟不可用时返回 INT64_MIN"（上面两条 return 同理），
     * 所以这里补上同一条判断：让参考时钟看到"不可用"，而不是一个假时间。
     * 这是 system_refer_clock.cpp 里那道护栏的**源头**修复，两者互为保险。
     */
    if (af_clock_value_is_unset(mAudioTime.startTime)) {
        return INT64_MIN;
    }

    int64_t aoutPos;
    aoutPos = mAVDeviceManager->getAudioRenderPosition();

    /*
     * ============ 【★ 坏值不许当基准 —— "没声音 / 卡死"的直接来源（Android 实测）★】============
     *
     * 真机日志（20:24:49 / 20:25:00 等多次出现）：
     *   `audio clock: recorded the post-seek device-position base 9223372036854775807 us`
     *   `audio clock: the device position advanced to 9223372036854775807 us (base=…, delta=0)`
     * 也就是 **INT64_MAX 这个哨兵值被记成了"基准"**。此后任何真实位置都不大于它 ⇒
     * 每一帧都走下面那条 `rejected a stale/backward` ⇒ **音频时钟永远交不出去** ⇒
     * 主时钟失去参考、音频保持静音、画面被判"未来"而不上屏 —— 用户看到的就是
     * "有时候没声音 / 画面不动但有声音 / 直接卡死"。
     *
     * 处置（比 B2 那次"整段不交时钟"窄得多，只针对**坏值**）：
     *   哨兵值（`af_clock_value_is_unset` 判定的那类）与负值 **既不记基准、也不当进度**，
     *   直接按"音频时钟暂不可用"返回；真实设备位置一到，走原有逻辑照常放行。
     * 纯值判据、无计时器。
     * ======================================================================================
     */
    if (aoutPos < 0 || af_clock_value_is_unset(aoutPos)) {
        if (mAudioClockProgressLogCount < 8) {
            ++mAudioClockProgressLogCount;
            AF_LOGW("audio clock: ignored an invalid device position %lld us (sentinel/negative) — it must not "
                    "become the progress base, otherwise every real position would be rejected as stale\n",
                    (long long) aoutPos);
        }

        return INT64_MIN;
    }

    /* 【② 已整体回退】这里曾加过 `af_clock_value_is_unset(aoutPos) ⇒ return INT64_MIN`
     * 的哨兵护栏（回退点 B2）：真机出现"第一次 seek 画面不动、半天才动"——设备在 seek
     * 窗口内本来就会瞬时经过 PAUSED→STOPPED→PLAYING，框架 baseStop 的 STOPPED 还会持续一段；
     * 期间时钟改按墙上时间自走，音频回来时 SystemReferClock::GetTime()（:44-45）又**双向**
     * reSync 把它拉回音频位置（可能向后跳）⇒ 视频被判"未来"继续干等。失败模式比它要治的
     * "参考值冻结"更糟，故整体回退；详见 AudioTrackRender::device_get_position() 的说明。 */

    /*
     * ============ 【B14】"设备位置真的前进过"才交出音频时钟 ============
     *
     * B2 那次回退是因为"整段时间都不交时钟"这条路本身更糟（回头又会被向后 reSync 拉一次）。
     * 这里换一个**更窄、只针对坏值**的判据，语义与上面两条 return 完全一致（音频时钟暂不可用）：
     *   1. seek 后第一帧音频重锚之后，第一次拿到的设备位置只记**基准**，先不交；
     *   2. 只有观察到设备位置**严格超过**基准（设备真的在产数据/前进）才放行；
     *   3. 放行之后，仍然拒绝"低于基准"的值（向后跳）—— 这正是会被 GetTime() 双向 reSync
     *      拿去做"向后拉"的那一类值（AudioTrack 在 seek 窗口内非 PLAYING/PAUSED 时
     *      getDevicePlayedSimples() 返回 0，位置会塌回 0）。
     * 期间主时钟按**本地墙钟**自走（与既有 mSeekFlag 窗口内的行为一致），所以不会"卡死"；
     * 一旦设备真的前进就立刻把音频参考交出去，A/V 仍以音频为主时钟。
     * 全是事件判据：没有计时器、没有时间阈值；复位点与 mAudioClockReanchorPending 同步
     * （FlushAudioPath / Reset），因此每次 seek 重新观察一遍。
     */
    if (mAudioClockProgressBaseUs == INT64_MIN) {
        mAudioClockProgressBaseUs = aoutPos;

        if (mAudioClockProgressLogCount < 8) {
            ++mAudioClockProgressLogCount;
            AF_LOGI("audio clock: recorded the post-seek device-position base %lld us — the audio reference "
                    "stays unavailable until the device position is seen to advance\n",
                    (long long) aoutPos);
        }

        return INT64_MIN;
    }

    if (aoutPos <= mAudioClockProgressBaseUs) {
        if (mAudioClockProgressLogCount < 8) {
            ++mAudioClockProgressLogCount;
            AF_LOGW("audio clock: rejected a stale/backward device position %lld us (base=%lld, delta=%lld us) — "
                    "a backward reSync here is what froze the picture right after a seek\n",
                    (long long) aoutPos, (long long) mAudioClockProgressBaseUs,
                    (long long) (aoutPos - mAudioClockProgressBaseUs));
        }

        return INT64_MIN;
    }

    if (!mAudioClockProgressSeen) {
        mAudioClockProgressSeen = true;

        if (mAudioClockProgressLogCount < 8) {
            ++mAudioClockProgressLogCount;
            AF_LOGI("audio clock: the device position advanced to %lld us (base=%lld, +%lld us) — the audio "
                    "reference is released from here on\n",
                    (long long) aoutPos, (long long) mAudioClockProgressBaseUs,
                    (long long) (aoutPos - mAudioClockProgressBaseUs));
        }
    }

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
    /* 【色觉辅助滤镜 / 回退点 C8】照上一行 setFlip 的写法：建渲染器时把**启动配置**里的矩阵
     * 也下发一次。否则"先 setColorMatrix 再 prepare/播放"时首帧不带滤镜
     * （渲染器是后建的，错过那条消息）。空默认实现下非 GL 渲染器无副作用。 */
    mAVDeviceManager->getVideoRender()->setColorMatrix(mSet->colorMatrix);
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
    /* B4：重建解码器期间先清掉"active 真面"，失败时保持 null ⇒ 切档的占位交接会
     * 退回纯 promote（宁可少一次无缝，也不拿一块过期/已失效的面去 setOutputSurface）。 */
    mActiveVideoSurface = nullptr;
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

    /* B4：记住这块解码器真正绑定的输出面（隧道 = App 的 view；GL = 渲染器 SurfaceTexture 的面）。
     * 切档的占位 Surface 交接要靠它把真面从旧解码器交给 pending 解码器。 */
    mActiveVideoSurface = view;

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
        /*
         * 【tunnel（dummy render）下不做软解兜底 —— 2026-09-24】
         *
         * DummyVideoRender 只负责把 MediaCodec 的 buffer 交给 Surface；软解帧是
         * AVAFFrame 的 YUV 数据，走不了这条路 —— 兜底"成功"也只会得到"有声音、没画面"，
         * 而且会把真正的失败原因从日志里抹掉（真机日志里就出现过
         * `CreateVideoDecoder bHW=0 … ret=0` + `rebuildVideoDecoder … ret=0` 这种假成功）。
         *
         * 判据是渲染器带 FLAG_DUMMY（既有状态，不新增成员）；Qt/GL 路恒不进入 ⇒
         * 那边的"硬解失败落软解"行为逐字不变。
         */
        const bool dummyRender =
            mAVDeviceManager->isVideoRenderValid() &&
            (mAVDeviceManager->getVideoRender()->getFlags() & IVideoRender::FLAG_DUMMY);
        if (dummyRender) {
            AF_LOGW("rebuildVideoDecoder: hardware rebuild failed (%d) and the tunnel render cannot "
                    "present software frames — skipping the software fallback\n", ret);
        } else {
            // 软解兜底（罕见路径：硬解重建失败时保音频不断播）
            ret = CreateVideoDecoder(false, meta);
        }
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

/*
 * 【B19-b】"切档在途"的唯一判据（PFR 是否可以让路、用户 seek 是否该推迟，都以它为准）。
 *
 * 为什么不能只看 pending/willChange 两个索引（B19 第一版就是这么写的，结果**从未生效**）：
 * 日志里 `[switch] state=decoderSwitch willChange=-1 target=0 current=7 retired=7 …
 * decoderSwitch=1 commitPending=0 oldFramesPending=0` 这个阶段，两个索引**都不是 >= 0**
 * （pending 路已经被 promote 成 active、willChange 已清），可是切档明明还没完 ——
 * 于是 PFR 照旧插 seek，把切档打成 CANCELED（7/7 次点击仍然全是 status=0 → 3）。
 *
 * 现状取"切档状态机自己的未完成标志"**并集**（每一项都是一个未完成的终态义务）：
 *   · mPendingVideoStreamIndex >= 0        —— 预滚/待提交的 pending 路还在
 *   · mWillChangedVideoStreamIndex >= 0    —— 已经受理、还没落到 pending 索引上
 *   · mPendingVideoDecoderSwitch           —— `state=decoderSwitch` / `decoderSwitch=1`
 *   · mQualitySwitchCommitPending          —— `state=committed`，还没上屏/收尾
 *   · mQualitySwitchCommittedStreamIndex >= 0 —— 已提交、终态还没发
 *   · mQualitySwitchOldFramesPending != 0  —— 提交前 active 队列的旧帧还没送完（READY 前置条件）
 *   · mRetiredVideoStreamIndex >= 0        —— 退役流还没关、retired 解码器还没释放
 *   · mSwitchStartedWhilePaused / mPausedSwitchRenderPending —— 暂停态切档的 S5/S9 欠账
 *   · mSwitchReArmPending                  —— B17：马上要在 seek 结束重发一次切档
 * 取值域：以上任一为"未完成"即算在途；全部落回 0/-1/false 才算终态。
 *
 * 覆盖 `state=decoderSwitch`：靠 mPendingVideoDecoderSwitch（=1）与 mRetiredVideoStreamIndex（=7 >= 0）
 * 两项 —— 日志里那个阶段这两项都成立，所以新判据一定为真。
 * 本函数是 logQualitySwitchState() 里那个局部 inSwitch 的**超集**：诊断行只在 warming /
 * decoderSwitch 有意义（那边不动），而"能不能插 seek"必须一直覆盖到 retired 释放与
 * oldFramesPending 收尾，所以这里更严。
 */
bool SuperMediaPlayer::qualitySwitchInFlight() const
{
    return mPendingVideoStreamIndex >= 0 || mWillChangedVideoStreamIndex >= 0 ||
           mPendingVideoDecoderSwitch || mQualitySwitchCommitPending ||
           mQualitySwitchCommittedStreamIndex >= 0 || mQualitySwitchOldFramesPending != 0 ||
           mRetiredVideoStreamIndex >= 0 || mSwitchStartedWhilePaused ||
           mPausedSwitchRenderPending || mSwitchReArmPending;
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

    /*
     * 【B19】切档在途时**不发起** PFR seek —— 只记下"有一次 PFR 待做"，等切档终态补做。
     *
     * 为什么要在这里拦：本函数是 PFR 的唯一发起点（下面那句 SeekTo），而它会被上层反复
     * setView 反复调到；只要切档在途，它插进来的 seek 就会走
     * `FlushVideoPath(cancelPendingSwitch=1)` 把在途切档打成 status=3 canceled
     * （安卓日志 7/7 次手动切档都是这么没的）。推迟而不是丢弃：暂停帧恢复本身是必要的，
     * 只是必须等切档走完。
     *
     * 判据（纯状态）：pending 路存在、或已有"将要切到某档"的请求、或 B17 的"等 seek 结束
     * 重新装弹"闩为真（后者意味着马上会再发起一次切档，此时插 seek 会把重装也打断）。
     * 无计时器、无阈值。
     */
    if (qualitySwitchInFlight()) {
        if (!mPauseFrameRestorePending) {
            AF_LOGI("PFR: deferred — a quality switch is still in flight (pendingStream=%d willChangeStream=%d "
                    "decoderSwitch=%d commitPending=%d committedStream=%d oldFramesPending=%d retired=%d "
                    "pausedSwitch=%d pausedRenderPending=%d reArmPending=%d), so no seek is issued now; the "
                    "restore will run ONCE at the switch's terminal state (READY / CANCELED / FAILED)\n",
                    mPendingVideoStreamIndex, mWillChangedVideoStreamIndex,
                    (int) mPendingVideoDecoderSwitch, (int) mQualitySwitchCommitPending,
                    mQualitySwitchCommittedStreamIndex, (int) mQualitySwitchOldFramesPending,
                    mRetiredVideoStreamIndex, (int) mSwitchStartedWhilePaused,
                    (int) mPausedSwitchRenderPending, (int) mSwitchReArmPending);
        }

        mPauseFrameRestorePending = true;
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

    /*
     * B3：这里有两个不同的时间轴，不能混用（同 seek 落点那条修正）。
     *
     *   · 解码器记录的是**原始 pts 轴**的值（隧道直通：mediaCodecDecoder 里
     *     mLastRenderedVideoPts = codec 输出帧的 pts，渲染门比较的也是这个值）
     *     ⇒ setRenderGate() 必须继续喂原始值；
     *   · SeekTo() 的入参是**全局时间轴上的位置**（它只用来定位媒体内容）
     *     ⇒ 必须先把 lastPts 折算到主时钟轴。
     *
     * 两轴不等时（切档预滚建立的偏移，或 seek 落点重建的偏移，见
     * mActiveVideoPtsOffset 的说明）原来直接把原始 pts 当位置用，会 seek 到
     * 相差一个偏移量的地方 —— 恢复出来的不是暂停的那一帧，甚至落在地板之外。
     * 偏移为 INT64_MIN（绝大多数会话）时表达式逐字不变。
     *
     * 只有实现 getLastRenderedVideoPts() 的后端才会走到这里（Android 隧道直通）；
     * Qt / 其它后端的 IDecoder 默认返回 INT64_MIN，上面那句 lastPts == INT64_MIN
     * 已经直接 return 0 ⇒ 本修正对 Qt 不可达。
     */
    const int64_t lastPtsOnClockAxis =
            (mActiveVideoPtsOffset != INT64_MIN) ? lastPts + mActiveVideoPtsOffset : lastPts;

    decoder->setRenderGate(lastPts);
    mRestoringPausedFrame = true;
    // seek 到"暂停帧之后一帧以内"：保证暂停帧所在包被读到，
    // 且不会多解多少包（向上取整避免毫秒截断到暂停帧之前）
    SeekTo((lastPtsOnClockAxis + frameDur + 999) / 1000, false);
    AF_LOGI("PFR: restore start, lastPts=%" PRId64 " frameDur=%" PRId64
            " offset=%" PRId64 " seekToPos=%" PRId64 "\n",
            lastPts, frameDur,
            (int64_t) (mActiveVideoPtsOffset == INT64_MIN ? 0 : mActiveVideoPtsOffset),
            (int64_t) (lastPtsOnClockAxis / 1000));
    return 0;
}

/*
 * 【B20】补做被推迟的用户 seek（唯一入口）。
 * 先清闩再 SeekTo ⇒ 补做出去的那次 seek 不会再触发"再补做"（不递归）；
 * 若此刻又有切档在途（例如刚重发的那次切档），只**重新置闩**，等那一次终态再补；
 * 目标取最后一次请求（后到覆盖先到）。全程状态判据，无计时器。
 * 出口：补做后走正常 SeekTo 流程，由既有 SeekEnd / NotifySeeking(false) 收尾。
 */
void SuperMediaPlayer::replayDeferredUserSeek()
{
    if (!mDeferredUserSeekPending) {
        return;
    }

    const int64_t targetUs = mDeferredUserSeekUs;
    const bool accurate = mDeferredUserSeekAccurate;

    /* 先清闩：防止下面这次 SeekTo 又触发一次补做（不递归）。 */
    mDeferredUserSeekPending = false;
    mDeferredUserSeekUs = INT64_MIN;
    mDeferredUserSeekAccurate = false;

    if (targetUs == INT64_MIN) {
        return;
    }

    /*
     * 只在这个播放器"seek 还有意义"的状态下补做。本入口除了切档终态，也被
     * FlushVideoPath() 末尾那条"外力中止切档"的分支调用，而 stop / 换源 / 后台 flush
     * 走的正是同一个分支（它们的 cancelPendingSwitch 也是 1）。那时玩家已经在
     * PLAYER_STOPPED / PREPARING 上，补做一次 seek 只会把用户早已放弃的定位需求
     * 打进新片源的准备流程里。与 PFR 补做入口同一种写法：状态不符合就丢弃这一次。
     */
    const PlayerStatus statusNow = mPlayStatus.load();

    if (statusNow != PLAYER_PREPARED && statusNow != PLAYER_PLAYING && statusNow != PLAYER_PAUSED &&
        statusNow != PLAYER_COMPLETION) {
        AF_LOGI("user seek: deferred seek DROPPED — the player is no longer in a seekable state "
                "(status=%d), target=%lld us\n",
                (int) statusNow, (long long) targetUs);
        return;
    }

    if (qualitySwitchInFlight()) {
        /* 又有一次切档在途：把目标重新挂上，等那一次的终态再补（不立即重试）。 */
        mDeferredUserSeekUs = targetUs;
        mDeferredUserSeekAccurate = accurate;
        mDeferredUserSeekPending = true;
        return;
    }

    AF_LOGW("user seek: replaying the DEFERRED seek now that the quality switch reached its terminal "
            "state — target=%lld us accurate=%d (the landing window is built on the post-switch timeline, "
            "so the switch's demuxer-level seek can no longer overwrite it)\n",
            (long long) targetUs, (int) accurate);

    SeekTo(targetUs / 1000, accurate);
}

/*
 * 【B19】切档终态出口的唯一补做入口（READY/FAILED/CANCELED 三处都会调到）。
 *
 * 语义：把"切档在途时被推迟的那一次 PFR"补做一次，然后清闩。
 *   · 先清闩再调用 RestorePausedVideoFrame() —— 补发出去的 seek 即使又被打断（又来一次
 *     切档），也只会让 RestorePausedVideoFrame() 重新置闩，由**那一次**切档的终态再补；
 *     本函数不会在这里立即重试、也不会递归调用自己 ⇒ 没有 ping-pong（硬要求 3）。
 *   · 播放器已不在暂停态 ⇒ 丢弃这一次待做（PFR 只对暂停画面有意义），清闩即可。
 *   · 又检测到切档在途（例如刚被"更新的请求"取代、重装闩还在）⇒ 重新置闩，等它的终态。
 *   · 不记目标位置：RestorePausedVideoFrame() 自己从当前解码器读最后渲染帧 pts，
 *     所以补做用的是切档之后的当前时间轴（不是取消前的旧点）。
 */
void SuperMediaPlayer::runDeferredPauseFrameRestore()
{
    if (!mPauseFrameRestorePending) {
        return;
    }

    mPauseFrameRestorePending = false;

    if (mPlayStatus != PLAYER_PAUSED) {
        AF_LOGI("PFR: deferred restore dropped — the player is no longer paused (status=%d)\n",
                (int) mPlayStatus.load());
        return;
    }

    if (qualitySwitchInFlight()) {
        /* 终态出口处又有新的切档在途：推迟到它自己的终态，绝不在这里立即重试。 */
        mPauseFrameRestorePending = true;
        return;
    }

    AF_LOGI("PFR: running the deferred pause-frame restore now that the quality switch reached its "
            "terminal state (no seek was issued while the switch was in flight)\n");

    RestorePausedVideoFrame();
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
    /* 【B14】Reset 与 FlushAudioPath 一样清掉"设备位置前进观察"的状态。 */
    mAudioClockProgressBaseUs = INT64_MIN;
    mAudioClockProgressSeen = false;
    mAudioClockProgressLogCount = 0;
    mSeekFlag = false;
    /* 墙钟计时跟 seek 状态一起复位，否则上一次播放留下的时间戳会让新一次
     * seek 的"音频解锁"立刻命中（见 render() 里 SEEK_CATCH_AUDIO_UNBLOCK_MS）。 */
    mSeekCatchStartMs = 0;
    mSeekNoFrameSinceMs = 0;
    /* 暂停态切档的状态也必须跨片源清零（否则下一次播放会误走暂停切档分支）。 */
    resetPausedSwitchState();
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
    /* B10：时间轴重建（seek/stop/换源）时逐帧配对表必须一起丢掉，否则旧代的 pts 可能撞上新帧 */
    mVideoAxisPts.clear();
    mVideoAxisTimePos.clear();
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
    /* 【② B18】地板作废时归属一起作废，避免"地板 = INT64_MIN、归属还是旧目标"的组合。 */
    mSeekLandingFloorOwnerUs = INT64_MIN;
    /*
     * 【① B17 硬要求 B】Reset = 换片源/停止/Prepare 的终态 ⇒ 之前记下的"等 seek 结束
     * 重新装弹"的意图必须作废（不重装）。
     */
    mSwitchReArmPending = false;
    mSwitchReArmStreamIndex = -1;
    /* 【B19 硬要求】换片源/停止/Reset ⇒ 不跨片源补做 PFR：闩一起清掉。 */
    mPauseFrameRestorePending = false;
    /* 【B20 硬要求 B】换片源/停止/Reset ⇒ 在途被推迟的用户 seek 也必须作废（不跨片源重放）。 */
    mDeferredUserSeekPending = false;
    mDeferredUserSeekUs = INT64_MIN;
    mDeferredUserSeekAccurate = false;
    // 复位时必须把"落点闸门/闩"一起清掉：否则 Reset 之后残留的闸门会让
    // 下一个与 seek 无关的帧被当成落点帧（并带着一个无效的目标点去算日志）。
    mSeekRenderGateUs = INT64_MIN;
    mSeekLandingFrameAccepted = false;
    mSeekClockAnchored = false;
    mSeekDecodeStartIsKey = false;
    /* B15：与 SeekTo 同一个复位口径，保证 Reset 之后不会残留"已经先出过画"的闩。 */
    mSeekFirstDecodableFrameShown = false;
    /* B16：与 SeekTo 同一个复位口径。 */
    mVideoDiscardStreak = 0;
    mVideoDiscardGapAbsUs = INT64_MIN;
    mCatchUpDiscardStreak = 0;
    mSeekCatchStartMs = 0;
    mVideoStarveIters = 0;
    mSeekVideoAnchorDone = false;
    mSeekAudioAlignDone = false;
    mVideoDecodeRetrySeen = false;
    mDecodeStallIters = 0;
    mDecodeStallRebuildDone = false;
    mSeekAudioFloorUs = INT64_MIN;
    mSeekAudioContinuityUs = INT64_MIN;
    mSeekAudioStaleDrops = 0;
    mSeekExactLandingByBudget = false;
    mSeekAnchorPending = false;
    /* A 方案的事件闩：Reset 必须清干净，避免跨片源残留。 */
    mSeekAudioRepositionPending = false;
    mSeekAudioRepositionUs = INT64_MIN;
    mSeekAudioRepositionJumpUs = INT64_MIN;
    mMainStreamId = -1;
    mRemovedFirstAudioPts = INT64_MIN;
    mFirstSeekStartTime = 0;
    mAudioChangedFirstPts = INT64_MIN;
    mVideoChangedFirstPts = INT64_MIN;
    mPendingVideoDecoderSwitch = false;
    /*
     * B2：Reset 是换片源/Stop 的公共出口，pending 路的一切判断都必须在这里清干净；
     * 这个 bool 漏清就会跨片源残留。
     */
    mPendingDecoderUsesPlaceholderSurface = false;
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

    /*
     * 【② B18 硬要求 C】seek 结束 = 落点窗口的**明确出口**：把闸门、地板、归属、
     * "先出画"闩一起关掉。正常路径下采纳那一刻已经关过闸门，这里是幂等兜底 ——
     * 保证 mSeekRenderGateUs / mSeekLandingFrameAccepted 不会留下"无法撤销"的悬挂态
     * （例如落点被归属判据拒绝、或落点帧始终没来）。
     */
    mSeekRenderGateUs = INT64_MIN;
    mSeekPositionFloorUs = INT64_MIN;
    mSeekLandingFloorOwnerUs = INT64_MIN;
    mSeekFirstDecodableFrameShown = false;

    /*
     * 【修：seek 结束 ⇒ 未消费的锚点事件一并作废（"进度条往回弹"的根因）】
     *
     * 本函数刚刚把 mSeekPos 与 mSeekPositionFloorUs（锚点的**两个目标载体**）清零，
     * 却把 mSeekAnchorPending 留着。于是 seek 宣告结束之后第一帧视频上屏时，锚点事件
     * 拿着"目标未知"去锚定：fetchSeekClockAnchorUs() 走兜底分支返回那一帧自己的 pts，
     * 主时钟被**倒退**到落点帧的位置。真机日志（安卓 2026-09-26 20:55:38，用户 seek
     * 到 116.273s）三行可对：
     *   38.825  seek anchor refused: frame=99933167 earlier than target=116273000
     *   38.851  seek anchor: pts=99966533, target=-9223372036854775808,
     *           landingFloor=116273000, anchor=99966533        ← 时钟 116.273s → 99.966s
     *   39.033  seek anchored by the video landing frame, audio pts=116288000  ← 又拉回 116.288s
     * 用户看到的就是"进度条先往回弹一下、然后又弹回去"；而时钟倒退的那 16.3 秒里，
     * 所有正常帧都被判成"迟到帧"丢掉 —— 也就是"seek 后卡死画面半天不动"。
     *
     * 语义上这次锚定本来就没有可用的正确值：目标已经被清掉，"锚到目标"无从谈起，
     * 而 seek 已经结束、时钟本来就停在正确位置上（ProcessSeekToMsg 一开始就把它钉在
     * 目标上），再去锚一个旧帧只会破坏它。所以在**唯一的目标载体被清掉的这一处**
     * 把事件消费掉：不再等"下一帧旧位置"，也不会悬挂（上面两条注释担心的
     * "纯音频渲染把事件白白消费掉"发生在 seek 期间，与这里不是同一个时点）。
     * 应用侧的护栏见 RenderVideo 的锚点出口：即使还有别的时序绕过这里，
     * "锚点不许把主时钟往回拉"那条判据也不会让时钟倒退。
     */
    mSeekAnchorPending = false;

    /*
     * 【① B17】seek 结束事件：如果这次 seek 曾把"用户点过的切档"拆掉，在这里
     * **只重新发起一次**（先清闩再发起，所以不会重复触发）。
     *
     * 目标位置：这里只发档位索引，位置由切档消息处理按**当前**播放位置重新计算
     * （日志里的 `quality switch: target stream N seeked to <当前播放位置>` 就是它），
     * 因此不会用"取消前那个已经过期的时间点"去预热。
     *
     * 为什么不会 ping-pong（硬要求 A）：本函数只在 seek 结束时被调用，而置闩只发生在
     * SMPMessageControllerListener 的 MSG_SEEKTO 处理里；切档自己的定位是 demuxer 级
     * （不产生 MSG_SEEKTO，见那里的注释与全仓唯一的 putMsg(MSG_SEEKTO)），
     * 所以"重装 → 切档内部 seek → 再取消 → 再重装"这条环不存在。
     */
    if (mSwitchReArmPending) {
        const int reArmIndex = mSwitchReArmStreamIndex;
        mSwitchReArmPending = false;
        mSwitchReArmStreamIndex = -1;

        if (reArmIndex >= 0) {
            AF_LOGI("quality switch: re-issuing the user's switch request (stream=%d) now that the "
                    "seek that superseded it has finished — the preroll position will be recomputed "
                    "from the CURRENT playback position, not the pre-seek one\n", reArmIndex);
            SwitchStream(reArmIndex);
        }
    }
}

/*
 * ============ seek 期间主时钟锚点规则的唯一实现（声明与理由见 SuperMediaPlayer.h）============
 *
 * 只在"一次 seek 只锚一次"（mSeekClockAnchored 为假）时才会被调用处调用；这里再
 * 复核一遍，因为它同时是"这一帧能不能当锚点"的唯一判据。
 *
 * 目标点取 **mSeekPositionFloorUs 优先、mSeekPos 兜底**：前者是 seek 入口写下的
 * 位置地板（本次 seek 的目标），兜底那条覆盖"尚未设地板的旧路径 / 管道已经走完
 * 目标点把地板撤掉"这两种状态 —— 两种情况下 mSeekPos 都还是本次 seek 的目标。
 *
 * 为什么"早于目标的帧直接返回 INT64_MIN"而不是照样采纳：早于目标的帧只可能来自
 * 旧时间轴（或本次 seek 的落点关键帧，那是设计上允许**上屏**、但绝不允许**锚时钟**
 * 的那一类）。返回 INT64_MIN 让锚点事件留在闩上，等真正 >= 目标的那一帧再来锚，
 * 时间轴因此不会在 seek 期间被拉回旧位置。
 *
 * 日志里的 why 由调用处给出（视频落点帧 / 音频落点首帧），不新增限频状态：
 * 一次 seek 最多锚成功一次，所以本函数最多打一行。
 */
int64_t SuperMediaPlayer::fetchSeekClockAnchorUs(int64_t frameUs, const char *why)
{
    if (mSeekClockAnchored) {
        /* 本次 seek 已经锚过：主时钟不再被任何后来的帧改写。 */
        return INT64_MIN;
    }

    if (frameUs == INT64_MIN) {
        return INT64_MIN;
    }

    /*
     * 目标点：位置地板优先，其次本次 seek 的 mSeekPos。
     * 两者都是"位置必须等于目标"这条要求的载体，且都在微秒轴上（与帧的 pts 同轴）。
     */
    const int64_t effectiveTargetUs = (mSeekPositionFloorUs != INT64_MIN)
                                      ? mSeekPositionFloorUs
                                      : mSeekPos.load();

    if (effectiveTargetUs != INT64_MIN &&
        frameUs < effectiveTargetUs - SEEK_AUDIO_CONTINUITY_TOLERANCE_US) {
        AF_LOGW("seek anchor refused (%s): frame=%lld is earlier than the target=%lld — keeping the "
                "clock on the target instead of pulling the timeline back to a frame before the seek "
                "(the position floor stays in charge until the landing frame reaches the target)\n",
                why != nullptr ? why : "?", (long long) frameUs, (long long) effectiveTargetUs);
        return INT64_MIN;
    }

    if (effectiveTargetUs == INT64_MIN) {
        /* 极端兜底：目标点未知时，这一帧的时间位置就是唯一可用锚点。 */
        return frameUs;
    }

    /*
     * 【锚到目标】落点帧早于目标（稀疏 IDR / GOP 前缀的正常情况）时，时钟钉在目标上按
     * 1x 前进、位置上报钉在目标上，用户看到的位置从松手那一刻就是目标；落点帧与它
     * 之后的那一帧仍会被照常采纳上屏（RenderVideo 的落点判据与追赶窗口都不读时钟锚点），
     * 所以精确性不受影响 —— 被去掉的只是"把时间轴拉回落点"这个错误动作。
     */
    const int64_t anchorUs = (frameUs > effectiveTargetUs) ? frameUs : effectiveTargetUs;

    if (anchorUs != frameUs) {
        AF_LOGI("seek anchor (%s): the first frame/audio packet of this seek is %lld us earlier than "
                "the target %lld — anchoring the master clock on the TARGET (position reports and the "
                "landing judge both use the target; the frame itself still goes to the screen)\n",
                why != nullptr ? why : "?", (long long) frameUs, (long long) effectiveTargetUs);
    }

    return anchorUs;
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
