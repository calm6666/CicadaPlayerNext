//
// HLSStream / DashStream 共用的 seek "落点延迟线"（唯一一份实现）。
//
#define LOG_TAG "SeekLandingStage"

#include "SeekLandingStage.h"
#include "utils/AFMediaType.h"
#include "utils/frame_work_log.h"
#include <utility>

namespace Cicada {

    SeekLandingStage::SeekLandingStage(std::deque<std::unique_ptr<IAFPacket>> &queue,
                                       std::mutex &queueMutex,
                                       std::condition_variable &queueCond)
        : mQueue(queue),
          mQueueMutex(queueMutex),
          mQueueCond(queueCond)
    {
    }

    void SeekLandingStage::armInternal(int64_t targetUs, const char *what, int streamType, bool live)
    {
        if (what != nullptr) {
            mWhat = what;
        }

        /* 装弹前的目标：还能看到"同一条 seek 里第二路把目标改掉"这种最值得警惕的情况。 */
        const int64_t prevTarget = mTargetUs;

        /*
         * 先清掉上一次的一次性标记：arm() 被拒绝（负值 / 非视频 / 直播）时不能把这个标记留给
         * 后面某次**与本 seek 无关**的宿主重开收口，否则那次收口会保留一个陈旧目标。
         * 标记只在下面走到"真的装弹成功"时重新置真。
         */
        mReopenBelongsToSeek = false;

        reset();

        if (targetUs < 0) {
            AF_LOGI("[seekLanding] %s not armed: reqUs=%lld is negative\n", mWhat, (long long) targetUs);
            return;
        }

        if (streamType != STREAM_TYPE_VIDEO) {
            AF_LOGI("[seekLanding] %s not armed: reqUs=%lld stream=%d (only the VOD video path stages the landing; "
                    "audio has no GOP semantics, MIXED would drop audio together with the prefix)\n",
                    mWhat, (long long) targetUs, streamType);
            return;
        }

        /*
         * 只对点播生效：直播的目标轴是"当前可用窗"，落点距离本来就由分片位置决定，
         * 而且直播另有它自己的过期丢弃逻辑；这一轮不动它（只慢不错）。
         */
        if (live) {
            AF_LOGI("[seekLanding] %s not armed: reqUs=%lld (live; only VOD is staged)\n",
                    mWhat, (long long) targetUs);
            return;
        }

        mTargetUs = targetUs;
        mStreamType = streamType;

        if (prevTarget != INT64_MIN) {
            /*
             * 同一路视频流在一轮 seek 里被**第二次**装弹，而且上一轮还没接管：目标一旦被换成更早的
             * 位置（例如按 stream index 的单流 seek：切档预热 / 解码器定位），延迟线就会按
             * 那个位置找落点，从玩家视角看就是"这次 seek 又落回分片首"。这一条日志就是用来抓它的。
             */
            AF_LOGW("[seekLanding] %s arm OVERWRITES a still-pending target: stream=%d prev=%lld new=%lld "
                    "(previous arm had not engaged yet)\n",
                    mWhat, streamType, (long long) prevTarget, (long long) targetUs);
        }

        AF_LOGI("[seekLanding] %s armed: stream=%d reqUs=%lld (landing will be the last keyframe <= reqUs inside "
                "the segment that is opened next; the prefix before it is dropped)\n",
                mWhat, streamType, (long long) targetUs);

        /*
         * 【关键】装弹成功 ⇒ 接下来那次宿主"重开分片"的收口属于本次 seek：目标必须活到
         * "包含目标的那个分片"的第一个包上，否则延迟线装了弹却从不接管（真机实测形态）。
         * 语义、为什么不能无条件保留（切档交接 / SetCurSegNum 必须清）、以及"标记始终没被消费"
         * 时为什么安全，都写在头文件那两个入口的说明里。
         */
        mReopenBelongsToSeek = true;
    }

    void SeekLandingStage::flushOnHostReopen()
    {
        const bool seekOwned = mReopenBelongsToSeek;
        mReopenBelongsToSeek = false;

        if (!seekOwned) {
            /* 非 seek 的重开（切档交接 / SetCurSegNum / 普通换片）：维持旧语义，连目标一起清。 */
            flush();
            return;
        }

        /*
         * 本次 seek 自己的重开收口：**保留目标**。
         * flush() 会把 stage 按原序交回宿主队列（此刻它通常是空的：还没读到任何包），
         * 但它同时会 reset()（连目标清），所以这里把目标与流类型装回去 —— 装回去之后的状态
         * 正好是"已装弹、还没见过任何包"，接着 updateSegment() 打开的那个含目标分片的第一包
         * 就能被 filter() 正常接管。
         */
        const int64_t keepTarget = mTargetUs;
        const int keepStreamType = mStreamType;
        flush();
        mTargetUs = keepTarget;
        mStreamType = keepStreamType;

        AF_LOGI("[seekLanding] %s host reopen of the SEEK at the segment boundary: target=%lld KEPT "
                "(the segment opened next is the one that contains the target; clearing it here is exactly "
                "why the stage used to stay un-engaged and the decoder restarted at the segment head)\n",
                mWhat, (long long) keepTarget);
    }

    void SeekLandingStage::dropStageOnHostReopen()
    {
        const bool seekOwned = mReopenBelongsToSeek;
        mReopenBelongsToSeek = false;

        if (seekOwned) {
            /* 本次 seek 自己的重开：只丢 stage，保留目标（数据语义与 reset() 的"丢弃"一致）。 */
            dropStage();
            AF_LOGI("[seekLanding] %s host reopen of the SEEK: staged packets dropped, target=%lld KEPT\n",
                    mWhat, (long long) mTargetUs);
        } else {
            /* 非 seek 的重开：没有时间目标，连目标一起清（防止陈旧目标吃掉新分片的前缀）。 */
            reset();
        }
    }

    void SeekLandingStage::dropStage()
    {
        mStage.clear();
        mStarted = false;
        mHaveKey = false;
        mProgress = false;
        mExtraData.clear();
        mDropped = 0;
        mLogCount = 0;
        /*
         * mTargetUs **故意保留**：stop() 之后重新 start() 会在同一个分片上重开，
         * 目标依然有效；万一目标过期（播放点跑过了它），延迟线在"第一包 pos > 目标"处会立刻
         * 自行放弃（见 filter），不会误丢数据。
         */
    }

    void SeekLandingStage::reset()
    {
        dropStage();
        mTargetUs = INT64_MIN;
    }

    void SeekLandingStage::flush()
    {
        if (!mStage.empty()) {
            {
                std::unique_lock<std::mutex> waitLock(mQueueMutex);

                while (!mStage.empty()) {
                    mQueue.push_back(std::move(mStage.front()));
                    mStage.pop_front();
                }
            }
            /* 消费端 read() 在队列空时会等这个条件（和 read_thread 推包后的动作一致）。 */
            mQueueCond.notify_one();
        }

        reset();
    }

    bool SeekLandingStage::consumeProgress()
    {
        const bool progress = mProgress;
        mProgress = false;
        return progress;
    }

    void SeekLandingStage::collectExtraData()
    {
        for (auto &staged : mStage) {
            if (staged != nullptr && staged->getInfo().extra_data != nullptr &&
                staged->getInfo().extra_data_size > 0) {
                mExtraData.assign(staged->getInfo().extra_data,
                                  staged->getInfo().extra_data + staged->getInfo().extra_data_size);
            }
        }
    }

    bool SeekLandingStage::filter(std::unique_ptr<IAFPacket> &packet)
    {
        if (mTargetUs == INT64_MIN || packet == nullptr) {
            return true;
        }

        /*
         * 装弹时就确认过是视频流，这里再挡一次；判据与装弹时同源，不是第二个精度权威。
         * 流类型在 tracker 生命周期内不变（SegmentTracker 没有 setStreamType），所以用装弹时
         * 记下的值与"每包现取"等价。
         */
        if (mStreamType != STREAM_TYPE_VIDEO) {
            reset();
            return true;
        }

        /*
         * 和 read_thread 的判据保持一致：这种包本来就不该进队列（read_thread 会把它丢掉），
         * 不能让它占住延迟线的队首。
         */
        if (packet->getData() == nullptr || packet->getSize() <= 0) {
            return true;
        }

        const int64_t target = mTargetUs;
        const int64_t pos = packet->getInfo().timePosition;

        if (pos == INT64_MIN) {
            /*
             * 拿不到 timePosition 就判不了"不晚于目标"：放弃延迟线。已收下的包**按原序交出去**
             * （flush 会把 stage 搬进 mQueue），退化成旧行为 —— 只慢、不错。
             */
            AF_LOGW("[seekLanding] %s ABANDON: this packet has no timePosition (stream=%d target=%lld) — cannot tell "
                    "'not later than the target', so this seek falls back to the segment head\n",
                    mWhat, mStreamType, (long long) target);
            flush();
            return true;
        }

        const bool isKey = (packet->getInfo().flags & AF_PKT_FLAG_KEY) != 0;

        if (!mStarted) {
            mStarted = true;
            /* 这一条就是"这次 seek 为什么没被接管"的答案：engage 需要 key=1 且 pos<=target。 */
            AF_LOGI("[seekLanding] %s first packet after the seek: pos=%lld key=%d target=%lld stream=%d "
                    "(engage requires key=1 and pos<=target)\n",
                    mWhat, (long long) pos, (int) isKey, (long long) target, mStreamType);
        }

        if (pos > target) {
            /*
             * "最后一个 <= 目标的关键帧"到此唯一确定（包按 timePosition 单调，之后不可能再有
             * <= 目标 的关键帧）。stage 非空 ⇒ 延迟线接管过这次 seek；stage 为空 ⇒ 这一片里没有
             * 可用候选。两种情况下都整批按序交出，当前这一包走正常路径返回。
             */
            if (mStage.empty()) {
                AF_LOGI("[seekLanding] %s RELEASE without candidate: first packet past the target is pos=%lld "
                        "target=%lld (droppedPrefix=%d) — landing stays at the segment head\n",
                        mWhat, (long long) pos, (long long) target, mDropped);
            } else {
                const int64_t landing = mStage.front()->getInfo().timePosition;
                AF_LOGI("[seekLanding] %s RELEASE: landing=%lld target=%lld (%lld ms before the target), "
                        "droppedPrefix=%d packets, landingIsKey=%d — decoder starts at the landing "
                        "keyframe instead of the segment head\n",
                        mWhat, (long long) landing, (long long) target,
                        (long long) ((target - landing) / 1000), mDropped, (int) mHaveKey);
            }

            flush();
            return true;
        }

        if (isKey) {
            if (!mHaveKey) {
                /* 首个候选（正常情形就是分片第一个包）：延迟线正式接管这次 seek。 */
                collectExtraData();
                mDropped += static_cast<int>(mStage.size());
                mStage.clear();
                mHaveKey = true;

                AF_LOGI("[seekLanding] %s ENGAGE: stream=%d target=%lld firstKeyPos=%lld (segment head is "
                        "%lld ms before the target)\n",
                        mWhat, mStreamType, (long long) target, (long long) pos,
                        (long long) ((target - pos) / 1000));
            } else {
                /*
                 * 又看到一个仍然 <= 目标的关键帧 ⇒ 它更贴近目标，是更好的落点：把 stage（一定从
                 * 上一个候选关键帧开始）整批丢掉，换成从这一包开始。丢之前把参数集抄出来。
                 */
                collectExtraData();
                const int droppedNow = static_cast<int>(mStage.size());
                mDropped += droppedNow;
                mStage.clear();

                if (mLogCount < 4) {
                    ++mLogCount;
                    AF_LOGI("[seekLanding] %s closer keyframe found: pos=%lld target=%lld (dropped %d more "
                            "packets, total droppedPrefix=%d)\n",
                            mWhat, (long long) pos, (long long) target, droppedNow, mDropped);
                }
            }
        } else if (!mHaveKey) {
            /*
             * 候选之前就来了非关键帧（异常包序：上一片的尾巴、或分片首不是 IDR）：**不放弃**，
             * 先攒着。若随后出现 <= 目标 的关键帧，这一批会被整批丢掉；若直到 pos > 目标 都没等到
             * 候选，就按原序整批交出去。这样单包异常不会让整条 seek 悄悄退回分片首。
             */
            if (mLogCount < 4) {
                ++mLogCount;
                AF_LOGW("[seekLanding] %s non-key packet before any candidate (pos=%lld target=%lld) — held, "
                        "waiting for the first keyframe <= target\n",
                        mWhat, (long long) pos, (long long) target);
            }
        }

        /*
         * 【extradata / SPS-PPS 必须贴到新的第一个包上】stage 为空时的这一包就是新的首包；
         * 第二个条件（它自己没带参数集）与 ActiveDecoder 里"从 holding 队列补参数集"的做法同形。
         */
        if (mStage.empty() && !mExtraData.empty() && packet->getInfo().extra_data_size <= 0) {
            packet->setExtraData(mExtraData.data(), static_cast<int>(mExtraData.size()));
        }

        mStage.push_back(std::move(packet));
        mProgress = true;
        return false;
    }

}// namespace Cicada
