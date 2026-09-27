//
//  AbrAlgoStrategy.cpp
//  apsara_player
//
//  Created by shiping.csp on 2018/11/1.
//

#include "AbrAlgoStrategy.h"
#include "AbrRefererData.h"
#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include <utils/AFMediaType.h>
/*
 * 硬解能力查询（decoderFactory::isHardwareDecodeSupported）。它是 L1 的无平台宏接口
 * —— 平台实现关在 framework/codec/decoderFactory.cpp 里，这里看不到任何平台差异。
 */
#include <codec/decoderFactory.h>
#include <utils/frame_work_log.h>

namespace {

    /*
     * ==================== 手动指定编码（preferred）相关日志的去重 ====================
     *
     * 为什么需要它：起播默认档那边一次 Prepare 只判一次，直接打日志没问题；
     * 而 ABR 每秒一个 tick、每个 tick 又要对 2~3 个候选档做同分辨率收敛，
     * "应用层指定的编码在这个片源里根本没有"这种回退是**常态**，不去重就是每秒
     * 刷 2~3 条同样的 WARN，把真正有用的切换日志淹掉（日志本身也是排查手段，
     * 不能被自己刷废）。
     *
     * 去重键由调用方给出（preferred + 事件 + 分辨率/被选中的流），**同一个键只打一条**，
     * 值一变就再打一条。这是个纯日志去重：不参与任何选择判定，没有计时器、
     * 没有看门狗、没有失效逻辑；键是进程级静态量，多播放器实例最坏也就是少打一条
     * 重复的日志。带锁是因为不同 MediaPlayer 实例的 ABR 线程可能同时进来。
     */
    bool shouldLogPreferredEvent(const std::string &key)
    {
        static std::mutex mutex;
        static std::string lastKey;

        std::lock_guard<std::mutex> lock(mutex);

        if (key == lastKey) {
            return false;
        }

        lastKey = key;
        return true;
    }

    void logPreferredFallbackOnce(const std::string &preferred, const std::string &reason,
                                  int width, int height)
    {
        if (!shouldLogPreferredEvent("fallback|" + preferred + "|" + reason + "|" +
                                     std::to_string(width) + "x" + std::to_string(height))) {
            return;
        }

        AF_LOGW("ABR codec preference: preferred=%s (manual) is not usable at %dx%d: %s; "
                "falling back to the automatic rules (hardware decode first, then efficiency)\n",
                preferred.c_str(), width, height, reason.c_str());
    }

}// namespace

AbrAlgoStrategy::AbrAlgoStrategy(std::function<void(int)> func)
{
    mFunc = std::move(func);
    mCurrentBitrate = -1;
}

AbrAlgoStrategy::~AbrAlgoStrategy()
{
    //  delete mRefererData;
}

void AbrAlgoStrategy::Clear()
{
    mBitRates.clear();
    mStreamIndexBitrateMap.clear();
    mLevelInfos.clear();
    mCurrentBitrate = -1;
}

void AbrAlgoStrategy::AddStreamInfo(int streamIndex, int bitrate)
{
    mStreamIndexBitrateMap.insert(pair<int, int>(bitrate, streamIndex));
    mBitRates.push_back(bitrate);

    /*
     * 扩展信息与 mBitRates **同长度、同顺序**。排序键必须能把 bitrate 相同、
     * streamIndex 不同的两档排成确定顺序（std::sort 不保证稳定），所以这里
     * 用 (bitrate, streamIndex) 的严格序，两个容器各自排完仍然一一对应。
     */
    AbrLevelInfo info;
    info.streamIndex = streamIndex;
    info.bitrate = bitrate;
    mLevelInfos.push_back(info);

    sort(mBitRates.begin(), mBitRates.end(), std::less<int>());
    sort(mLevelInfos.begin(), mLevelInfos.end(),
         [](const AbrLevelInfo &a, const AbrLevelInfo &b) -> bool {
             if (a.bitrate != b.bitrate) {
                 return a.bitrate < b.bitrate;
             }

             return a.streamIndex < b.streamIndex;
         });
}

void AbrAlgoStrategy::SetStreamExtraInfo(int streamIndex, const char *codecShortName, int width, int height)
{
    for (size_t i = 0; i < mLevelInfos.size(); i++) {
        if (mLevelInfos[i].streamIndex != streamIndex) {
            continue;
        }

        mLevelInfos[i].codec = (codecShortName != nullptr) ? codecShortName : "";
        /*
         * 效率序取"当前生效"的那一份（decoderFactory::getEffectiveCodecEfficiencyRank）：
         * 应用层传过 preference 就整份用它的位置序，否则整份用内核等级表
         * （afCodecEfficiencyRankByShortName）—— 两套顺序在那边二选一，这里不做第二次选择，
         * 也不允许把它们混起来。起播默认档（SMPMessageControllerListener）用的是同一个函数。
         */
        mLevelInfos[i].codecRank = decoderFactory::getEffectiveCodecEfficiencyRank(mLevelInfos[i].codec.c_str());
        /*
         * 硬解能力：短名 -> AFCodecID -> 问平台（两者的编码表只有一份，
         * 见 afCodecIDFromShortName / decoderFactory::isHardwareDecodeSupported）。
         * 应用层传过"设备能硬解的编码集合"时，那个函数直接用应用层的集合判定，
         * **不再探测**；传过之后这里拿到的就是应用层的结论。
         * 空串/未知短名会得到 AF_CODEC_ID_NONE，而 AF_CODEC_ID_NONE 一律返回 true
         * （"编码未知 = 能力未知 = 视为支持"），不会因为认不出编码就把它降级。
         */
        mLevelInfos[i].hwDecodeSupported =
                decoderFactory::isHardwareDecodeSupported(afCodecIDFromShortName(mLevelInfos[i].codec.c_str()));
        mLevelInfos[i].width = width;
        mLevelInfos[i].height = height;
        return;
    }

    /*
     * 没找到：这一路没有进过 AddStreamInfo（不在清晰度阶梯里），直接丢掉就好 ——
     * 不建"孤儿"条目，保证 mLevelInfos 与 mBitRates 永远等长。
     */
}

int AbrAlgoStrategy::FindSameResolutionEfficientCodec(int index, int maxBitrate) const
{
    if (index < 0 || index >= (int) mLevelInfos.size() ||
        index >= (int) mBitRates.size()) {
        return -1;
    }

    const AbrLevelInfo &cur = mLevelInfos[index];

    if (cur.width <= 0 || cur.height <= 0) {
        /* 分辨率未知：判不了"同一分辨率"，保持原档（宁可不换，也不许猜）。 */
        return -1;
    }

    /*
     * ==================== 优先级 1：应用层手动指定的编码（preferred） ====================
     *
     * 用户要的是"一旦设置了该格式，下一次分片请求就优先用该格式，如果没有该格式的流
     * 就按照支持情况"。落到这里就是三条：
     *
     *   1) preferred 非空 + 这一档分辨率下**有**该编码的流 + 内核**解得了**它
     *      => 返回那一路，**直接结束**，不再看下面的硬解优先/效率序 ——
     *      手动压过自动（用户的目的就是手动更改编码格式：哪怕设备能硬解 H.265、
     *      哪怕 H.265 更省带宽，用户指定 H.264 就该给 H.264）；
     *   2) preferred 非空、但这一档分辨率下没有该编码的流
     *      => 回退到自动规则，并打一条带原因的日志；
     *   3) preferred 非空、但内核**根本解不了**它（既不能硬解、也没有软解路径）
     *      => 同样回退 + 打日志。
     *
     * 刻意**不**把"设备只能软解它"算作回退理由：那是手动意图，允许软解；日志里
     * 用 decodedBy=hw|sw 把这次选择实际走的是硬解还是软解写清楚，便于真机核对。
     *
     * 生效时机：这里只回答"该选哪一档"，**不发起任何切换** —— 真正把请求发出去的是
     * 调用方（ABR tick 的上切/降档/同分辨率收敛三处，或起播默认档），所以设置
     * preferred 只是让**下一次**等级选择换一路，不会立刻切一刀。
     *
     * 这里的 preferred 来自 decoderFactory::getEffectivePreferredCodec()，起播默认档
     * （SMPMessageControllerListener）读的是**同一个函数、同一份快照**，两处不允许各自判断。
     */
    const std::string preferred = decoderFactory::getEffectivePreferredCodec();

    if (!preferred.empty()) {
        if (!decoderFactory::isCodecDecodable(preferred.c_str())) {
            /* 回退理由 3：内核既不能硬解、也没有软解路径。 */
            logPreferredFallbackOnce(preferred,
                                     "the kernel cannot decode it (neither hardware nor software path)",
                                     cur.width, cur.height);
        } else if (cur.codec == preferred) {
            /*
             * 这一档本身就是 preferred：保持不动，并且**到此为止** —— 不许下面的自动
             * 规则再以"效率收益/硬解收益"为名把它换走，否则用户的手动指定等于失效。
             */
            return -1;
        } else {
            /*
             * 这一档分辨率下找 preferred 那一路。同一编码同一分辨率仍可能有多路
             * （例如服务端给了两个码率的 H.265），取法要确定：
             *   1) maxBitrate > 0 时优先在**负担得起**（码率不超过上限）的那些里取
             *      码率最低者 —— "降清晰度"那条路径因此不会因为手动指定而把码率抬到
             *      预算之外太远；
             *   2) 没有负担得起的（或本来就不设上限，maxBitrate <= 0）才在全体里取
             *      码率最低者；
             *   3) 码率相同取 streamIndex 小者，结果与遍历顺序无关。
             */
            bool hasPreferredVariant = false;
            int affordableBest = -1;
            int cheapestBest = -1;

            for (size_t i = 0; i < mLevelInfos.size() && i < mBitRates.size(); i++) {
                const AbrLevelInfo &cand = mLevelInfos[i];

                if (cand.width != cur.width || cand.height != cur.height || cand.codec != preferred) {
                    continue;
                }

                hasPreferredVariant = true;

                if (cheapestBest < 0 || mBitRates[i] < mBitRates[cheapestBest] ||
                    (mBitRates[i] == mBitRates[cheapestBest] &&
                     cand.streamIndex < mLevelInfos[cheapestBest].streamIndex)) {
                    cheapestBest = (int) i;
                }

                if (maxBitrate > 0 && mBitRates[i] <= maxBitrate) {
                    if (affordableBest < 0 || mBitRates[i] < mBitRates[affordableBest] ||
                        (mBitRates[i] == mBitRates[affordableBest] &&
                         cand.streamIndex < mLevelInfos[affordableBest].streamIndex)) {
                        affordableBest = (int) i;
                    }
                }
            }

            if (hasPreferredVariant) {
                const int manual = (affordableBest >= 0) ? affordableBest : cheapestBest;

                if (shouldLogPreferredEvent("select|" + preferred + "|" +
                                            std::to_string(mLevelInfos[manual].streamIndex))) {
                    AF_LOGI("ABR codec preference: preferred=%s (manual) -> stream %d at %dx%d, "
                            "decodedBy=%s, bitrate %d -> %d "
                            "(automatic hardware/efficiency rules bypassed)\n",
                            preferred.c_str(), mLevelInfos[manual].streamIndex, cur.width, cur.height,
                            mLevelInfos[manual].hwDecodeSupported ? "hw" : "sw",
                            mBitRates[index], mBitRates[manual]);
                }

                return manual;
            }

            /* 回退理由 2：这一档分辨率下没有该编码的流。 */
            logPreferredFallbackOnce(preferred, "no stream of that codec at this resolution",
                                     cur.width, cur.height);
        }
    }

    /*
     * 第一趟：这一清晰度上有没有**能硬解**的变体。
     *
     * 硬解能力是选择的第一约束：只要有一个能硬解的变体，候选集就收缩到能硬解的那些；
     * 一个都没有（连当前档也解不了）才允许软解变体 —— 那时软解是唯一可行的选择。
     */
    bool hasHardwareVariant = false;

    for (size_t i = 0; i < mLevelInfos.size() && i < mBitRates.size(); i++) {
        const AbrLevelInfo &cand = mLevelInfos[i];

        if (cand.width == cur.width && cand.height == cur.height && cand.hwDecodeSupported) {
            hasHardwareVariant = true;
            break;
        }
    }

    /*
     * 第二趟：在允许的候选里挑"更该选"的那一档。
     *
     * 排序键依次是：编码效率等级（高者胜）-> 码率（低者胜）-> streamIndex（小者胜）。
     * 最后一个键让结果与遍历顺序无关（同等级同码率时仍然确定）。
     */
    int best = -1;

    for (size_t i = 0; i < mLevelInfos.size() && i < mBitRates.size(); i++) {
        const AbrLevelInfo &cand = mLevelInfos[i];

        if (i == (size_t) index || cand.width != cur.width || cand.height != cur.height) {
            continue;
        }

        /* 硬解优先：有能硬解的变体时，软解变体直接出局。 */
        if (hasHardwareVariant && !cand.hwDecodeSupported) {
            continue;
        }

        /*
         * 码率上限（"降清晰度"路径用）：只考虑本来就负担得起的候选。
         * maxBitrate <= 0 表示不设上限（"升清晰度/稳态"路径）。
         */
        if (maxBitrate > 0 && mBitRates[i] > maxBitrate) {
            continue;
        }

        if (best < 0) {
            best = (int) i;
            continue;
        }

        const AbrLevelInfo &bestInfo = mLevelInfos[best];

        if (cand.codecRank > bestInfo.codecRank) {
            best = (int) i;
        } else if (cand.codecRank == bestInfo.codecRank) {
            if (mBitRates[i] < mBitRates[best] ||
                (mBitRates[i] == mBitRates[best] && cand.streamIndex < bestInfo.streamIndex)) {
                best = (int) i;
            }
        }
    }

    if (best < 0) {
        return -1;
    }

    /*
     * 收益判据（换了必须真的更好，否则不动）：
     *   * 当前档解不了、候选档能硬解 —— 这是**能力收益**，不要求更省带宽
     *     （用户第 3 条：宁可换成设备支持的编码，也不要"最省带宽但解不了"的那条）。
     *     这条也是"同一分辨率里换个软解变体"唯一被允许的情形；
     *   * 否则只认**效率收益**：候选的压缩效率等级更高，**并且**码率严格更低
     *     （"压缩率更高 ⇒ 同一画质下码率更低"，用户第 1、2 条要的就是这个）。
     *
     * 特别地，**不会**因为"另一路码率更低"就换到一个压缩效率更差的编码上 —— 那是
     * 降画质，不是省带宽；同分辨率、同效率等级之间也不为了几 kbps 重建一次解码器。
     */
    const bool hardwareGain = mLevelInfos[best].hwDecodeSupported && !cur.hwDecodeSupported;
    const bool efficiencyGain = (mLevelInfos[best].codecRank > cur.codecRank) &&
                                (mBitRates[best] < mBitRates[index]);

    if (!hardwareGain && !efficiencyGain) {
        return -1;
    }

    return best;
}

void AbrAlgoStrategy::SetCurrentBitrate(int bitrate)
{
    mCurrentBitrate = bitrate;
}

void AbrAlgoStrategy::SetRefererData(AbrRefererData *refererData)
{
    mRefererData = refererData;
}
