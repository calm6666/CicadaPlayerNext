//
// HLSStream / DashStream 共用的 seek "落点延迟线"（唯一一份实现）。
//

#ifndef FRAMEWORK_DEMUXER_SEEKLANDINGSTAGE_H
#define FRAMEWORK_DEMUXER_SEEKLANDINGSTAGE_H

#include <base/media/IAFPacket.h>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace Cicada {

    /*
     * ============================================================================
     * seek 落点延迟线（唯一一份实现；HLSStream 与 DashStream 各持有一个实例、调用同一份代码）
     * ============================================================================
     *
     * 【精度权威在 renderer 的单一落点过滤；这里只是降低解码前推距离的尽力优化，失败只慢不错。】
     * 即使这里交接早了（多给了"落点关键帧之前"的前缀），播放器侧 shouldDropForDiscontinuity() 的
     * 单一落点过滤也会把这些前缀帧丢掉、并把落点精确到"包含目标的那一帧"。所以本类**不判精度**：
     * 它决定的是"解码器从哪里开始解"，判错的代价只是多解或少解几帧。
     *
     * 【要解决的问题】
     * 实测（HLS/DASH 都是 10 秒分片、片内每 2.5 秒一个 IDR）：seek 三次的落点关键帧分别比目标早
     * 9125 / 3071 / 3744 ms，落点都落在 40.0 / 70.0 / 110.0 这种"整 10 秒的分片片首"上；
     * 播放器必须从落点一路解到目标（"落点距离 = 必须解码前推的帧数"），这就是 seek 后 1~2.4 秒
     * 卡顿的全部来源（日志里那条 "the landing keyframe is N ms before the target"）。
     *
     * 【为什么不能直接落到分片内部】
     * 分片内部的 IDR 无法用一个更小的字节范围落上去：当前工具链（dashenc 默认 frag_type=none）
     * 每个分片只有一个 moof，文件里没有"从内部 IDR 开始"的 box 边界；内层 demuxer 也不可 seek
     * （建 demuxer 时没有传 seek 回调）。清单里同样只有分片级信息。
     *
     * 【所以改成在包这一层挪落点】
     * 包是顺序出的，关键帧标记（AF_PKT_FLAG_KEY）就在包上，于是把解码起点从"分片片首"挪到
     * "不晚于目标的最后一个关键帧"：
     *   · 比这个关键帧更早的包 —— 整体丢掉，不交给播放器（这正是"少解 3~9 秒的帧"的来源）；
     *   · 从这个关键帧到目标、以及目标之后的包 —— 一个不丢、按原顺序交出。
     * 落点误差于是从"分片长度(≤10s)"变成"关键帧间隔(本片源 2.5s)"。
     *
     * 【为什么不会把目标帧丢出去】（静态可自证）
     *   1. 只有 timePosition ≤ 目标 的包才会进延迟线；
     *   2. 包按 timePosition 单调递增，所以每当又看到一个"仍然 ≤ 目标"的关键帧，此前攒下的那一批
     *      全都早于它，都是"落点之前"的帧：丢掉它们不可能丢掉目标帧（目标帧在该关键帧之后，
     *      此刻还没被读出来）；
     *   3. 一旦读到 timePosition > 目标的包，"最后一个 ≤ 目标的关键帧"就唯一确定了
     *      （后面不可能再出现 ≤ 目标的关键帧），于是延迟线整批按序交出，当前这一包也照原样交出，
     *      目标帧必在其中；
     *   4. 目标落在本片最后一个 GOP 时等不到第 3 条的包，由"本片读完 / 停在本片界"这两个分片
     *      切换点收口（整体交出），同样不丢。
     *
     * 【失败安全】拿不到 timePosition 就直接放弃延迟线（把已收下的包照原样交出去），退化成
     * "从分片片首起解"的行为 —— 只慢、不错。只对点播视频路生效：音频没有 GOP 语义，混合流里
     * 丢前缀会连声音一起丢，都不介入。
     *
     * 【没有计时器】所有状态迁移都由"包里带的时间戳 + 关键帧标记"这两个事件驱动；唯一与时间有关
     * 的地方是"攒前缀时不要每包白等 10ms"，它的判据是"上一轮有没有真的取到包"（进度事实），
     * 不是毫秒数。
     *
     * 【首包不是关键帧时不再放弃（2026-09-26 修）】上一版要求"延迟线看到的第一包必须是关键帧"，
     * 否则直接把目标清掉、整条 seek 悄悄退回分片首 —— 实测有一批 seek 就是这样没被接管（落点仍是
     * 8~9.7 秒外的分片首，与生效的那批并存）。现在改成：候选（第一个 <= 目标 的关键帧）**之前**
     * 来的包先攒着，等候选出现就把这一批整批丢掉；若直到 pos > 目标 都没等到候选，就按原序整批
     * 交出去（退化成旧行为）。于是单包异常不再毁掉整条 seek，最坏情况只是多攒"分片首到目标"这一段
     * 的包 —— 与旧行为要交付的数据量相同，不是新增放大。
     *
     * 【日志】装弹 / ENGAGE / RELEASE / ABANDON 各最多一条（每条状态迁移每轮装弹只发生一次，所以
     * 日志量天然有界，不需要按时间限频；SuperMediaPlayer::floodLogAllowed 是播放器层的私有方法，
     * framework/demuxer 取不到），"换更近的落点"另有 mLogCount 的硬上限。
     * 收敛成一份实现之后：前缀仍是原来的 "[seekLanding]"（既有日志筛选命令照旧能命中），来源由
     * 宿主在 arm 时传入的标签紧跟在它后面带上，最终形态
     *     [seekLanding] HLS armed: stream=1 reqUs=...
     * 每个关键字与其余文本逐字不变。日志的 tag 字段变成本文件（原先是宿主的 LOG_TAG）。
     *
     * 【放行去哪里】flush() 把延迟线里攒着的包**按原序**搬回宿主自己的待交付队列，锁与唤醒方式
     * 与宿主读线程推包完全一致，所以宿主只需把队列三件套的引用交给构造函数，不必再写一遍。
     */
    class SeekLandingStage {
    public:
        /*
         * queue / queueMutex / queueCond 是宿主"待交付包队列"的三件套：HLSStream 与 DashStream
         * 都是 mQueue（std::deque<std::unique_ptr<IAFPacket>>）+ mDataMutex + mWaitCond。
         */
        SeekLandingStage(std::deque<std::unique_ptr<IAFPacket>> &queue,
                         std::mutex &queueMutex,
                         std::condition_variable &queueCond);

        /*
         * 装弹。targetUs：本次 seek 的目标（**必须传玩家请求值**，不能传"分片起点"，否则落点会退回
         * "最后一个 ≤ 分片起点"的关键帧，等于白做）；what：来源标签（"HLS" / "DASH"，只进日志）；
         * tracker：宿主的 tracker（可为 nullptr；HLSStream 用 SegmentTracker，DashStream 用
         * DashSegmentTracker，两者都有 getStreamType() / isLive()）。
         *
         * "只有点播视频路才装弹"这条判据（tracker 存在 + 视频流 + 非直播）只写在实现里一处，
         * 两个 demuxer 不再各写一遍 —— 它们原来那两份逐字相同，正是这次收敛要消灭的漂移源。
         */
        template<typename TrackerType>
        void arm(int64_t targetUs, const char *what, TrackerType *tracker)
        {
            armInternal(targetUs, what, tracker != nullptr ? tracker->getStreamType() : -1,
                        tracker != nullptr && tracker->isLive());
        }

        /*
         * 过包。返回 false = 这一包被延迟线收下（调用方本轮不产出，立刻再来一轮）；
         * 返回 true = 这一包照原样交出去。
         */
        bool filter(std::unique_ptr<IAFPacket> &packet);

        /* 把延迟线里攒着的包按原序搬回宿主队列，然后复位整条线（目标一起清）。 */
        void flush();

        /* 复位整条线，目标一起清。 */
        void reset();

        /*
         * 只丢"已收下还没交出"的包，**保留 seek 目标**（stop/start 会在同一个分片上重开，
         * 目标依然有效）。目标过期的情况由 filter 的"第一包 pos > 目标 就放弃"兜住，不会误丢数据。
         */
        void dropStage();

        /*
         * 攒前缀时"上一轮有没有真的取到包"这个进度事实（取一次就清）：宿主读线程用它决定这一轮
         * 要不要 af_msleep(10)。**不是计时器**：它只看进度。
         */
        bool consumeProgress();

    private:
        void armInternal(int64_t targetUs, const char *what, int streamType, bool live);

        /*
         * 把一批即将被丢掉的包身上的 codec 参数集抄出来（后面的覆盖前面的），供新的首包使用：
         * 参数集常常挂在分片第一个包上，而被丢掉的正是它。
         */
        void collectExtraData();

        std::deque<std::unique_ptr<IAFPacket>> &mQueue;
        std::mutex &mQueueMutex;
        std::condition_variable &mQueueCond;
        /* 来源标签（宿主在 arm 时传入）："HLS" / "DASH"，紧跟在 "[seekLanding]" 之后进日志 */
        const char *mWhat{"?"};
        /* 装弹时记下的流类型（tracker 的流类型在生命周期内不变），只用于日志与"非视频就复位" */
        int mStreamType{STREAM_TYPE_UNKNOWN};
        int64_t mTargetUs{INT64_MIN};
        bool mStarted{false};
        bool mHaveKey{false};
        bool mProgress{false};
        std::deque<std::unique_ptr<IAFPacket>> mStage{};
        std::vector<uint8_t> mExtraData{};
        /* 已经丢掉的"落点之前"的包数（只用于日志，证明延迟线真的接管了这次 seek） */
        int mDropped{0};
        /* "换更近的落点"这类日志每轮装弹的硬上限（防极端 GOP 刷屏；其余状态各只打一条） */
        int mLogCount{0};
    };
}// namespace Cicada


#endif //FRAMEWORK_DEMUXER_SEEKLANDINGSTAGE_H
