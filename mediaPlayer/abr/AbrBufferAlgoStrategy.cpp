//
//  AbrAlgoStrategy.cpp
//  apsara_player
//
//  Created by shiping.csp on 2018/11/1.
//
/*
 *  ABR（自适应码率）策略 —— 按开源实现重写，不是自己想的规则。
 *
 * 【为什么重写】
 * 旧实现完全没有带宽估计，只有"最近 10 次缓冲在涨还是在跌"的投票：
 *   - 上切：buffer >= 30s 且 10 次里 8 次在涨；
 *   - 下切：buffer < 15s 且 10 次里 8 次在跌。
 * 两个问题：
 *   1) 不看吞吐量。缓冲掉到 15s 才降档，而 15s 缓冲掉到 0 只需要几秒，
 *      网络一变差就是一次可见的卡顿之后才降档；
 *   2) `mSwitching` 只在 SetCurrentBitrate() 里清零。只要有一次 ABR 发起的
 *      切换没有走到 streamChangedSuc（超时、失败、被 FlushVideoPath 打断），
 *      ProcessAbrAlgo() 第一件事就是 return，ABR 被永久锁死 —— 日志里
 *      再也看不到 "BA switch to bitrate"。
 *
 * 【依据的开源实现】（全部读源码得到，逐条对应下面的常量）
 *   - hls.js `src/utils/ewma.ts` + `src/utils/ewma-bandwidth-estimator.ts`：
 *     快/慢双半衰期 EWMA（VOD 默认 3 / 9 个样本），估计值取 min(快, 慢)，
 *     样本权重 = 下载时长（秒）、值 = 8*bytes/时长；样本不足时用默认估计值
 *     （abrEwmaDefaultEstimate = 500kbps）。
 *   - hls.js `src/controller/abr-controller.ts` `findBestLevel()`：
 *     下切判定用 `adjustedbw = abrBandWidthFactor(0.95) * 估计`，
 *     上切用 `adjustedbw = abrBandWidthUpFactor(0.70) * 估计`（上切更保守），
 *     并要求这次下载能在缓冲耗尽前完成（`maxStarvationDelay = 4s`）。
 *   - dash.js `InsufficientBufferRule` / `BolaRule`：缓冲越浅，允许的码率越低；
 *     缓冲见底时按"这段时间最多能下多少"强制降档。
 *   - dash.js `AbrController`：`abandonLoadTimeout = 10000` —— 放弃一次下载之后
 *     10 秒内禁止上切（降档不受限）。
 *   - ExoPlayer `AdaptiveTrackSelection`：
 *     `DEFAULT_MIN_DURATION_FOR_QUALITY_INCREASE_MS = 10_000`（缓冲不到 10s 不上切）；
 *     `DEFAULT_MAX_DURATION_FOR_QUALITY_DECREASE_MS = 25_000`（缓冲还有 25s 以上先不降）。
 *   - Shaka `SimpleAbrManager`：`abr.switchInterval = 8s`，两次切换之间至少隔 8 秒
 *     （Shaka 的这个默认值就是 ExoPlayer `DEFAULT_MAX_DURATION_FOR_QUALITY_DECREASE_MS`
 *     的 25s 缓冲在 10s 片段上的自然结果，量级一致）。
 *
 * 【与开源实现的差异，明确记录】
 *   1) 片段时长：hls.js/ExoPlayer 的"这次下载来不来得及"用片段时长算，
 *      而 AbrRefererData 没有暴露片段时长。这里用 ExoPlayer 的缓冲门限
 *      （上切 >= 10s、降档 <= 25s）覆盖同一个问题 —— 它本身就是
 *      "预测下载时长 vs 剩余缓冲"的等价形式，且不依赖片段时长这个未知量。
 *   2) 用默认估计值（500kbps）去改**已经播起来**的档位没有依据（hls.js 只拿
 *      它选起始档位），所以样本不足前一律不动作，门槛用 Shaka 的
 *      `advanced.minTotalBytes = 128KB`。
 */
#define LOG_TAG "AbrBufferAlgoStrategy"
#include "AbrBufferAlgoStrategy.h"
#include "AbrBufferRefererData.h"
#include <algorithm>
#include <cmath>
#include <utility>
#include <utils/CicadaJSON.h>
#include <utils/frame_work_log.h>
#include <utils/timer.h>

/* ---- hls.js ewma-bandwidth-estimator.ts：VOD 默认半衰期 3 / 9 个样本 ---- */
#define ABR_FAST_HALF_LIFE_S (3.0)
#define ABR_SLOW_HALF_LIFE_S (9.0)
/* 数据源每秒滚动一次统计窗口（MediaPlayerUtil 里 timeS > 1.0 才更新），
 * 所以每个样本代表 1 秒的吞吐。 */
#define ABR_SAMPLE_WEIGHT_S (1.0)
/* 样本不足时的兜底估计值：hls.js abrEwmaDefaultEstimate = 500000 bps */
#define ABR_DEFAULT_BANDWIDTH_BPS (500000.0)
/* hls.js abrEwmaDefaultEstimateMax = 5000000：用"起播档码率"给估计值播种时
 * 的上限（见 SetCurrentBitrate）。 */
#define ABR_START_ESTIMATE_MAX_BPS (5000000.0)
/* Shaka advanced.minTotalBytes = 128 * 1000 */
#define ABR_MIN_TOTAL_BYTES (128 * 1000)
#define MAX_DOWNLOAD_SPEED_HISTORY 30

/* ---- hls.js abr-controller.ts ---- */
/* 上切用更保守的折扣：adjustedbw = 0.70 * 估计 */
#define ABR_UP_BANDWIDTH_FACTOR (0.70)
/* 下切/保持用 0.95 */
#define ABR_DOWN_BANDWIDTH_FACTOR (0.95)
/* 缓冲低于这个值视为"即将断流"，无条件降档（hls.js maxStarvationDelay = 4s
 * 描述的就是这个量级；dash.js InsufficientBufferRule 也是同一件事） */
#define ABR_PANIC_BUFFER_MS (4000)

/*
 * 【2026-09-21 修：改成"缓冲优先"（dash.js BOLA 那一派）】
 *
 * 用户实测反馈 + 日志证据：
 *   `ABR switch up: … buffer=50365 ms` → 1.5 秒后 `ABR switch down: … buffer=2001 ms
 *   reason=buffer about to run out` → 又一次 up … 一直来回切，每次切换本身就冻几秒。
 *
 * 原因：**每次换档都会把新 Representation 的缓冲从 0 重新攒**（旧档那 50 秒是按
 * 旧档读的，必须丢），所以换档之后的一两秒里"缓冲"必然是个很小的数。旧规则一看到
 * 缓冲小就紧急降档，于是"升到最高 → 立刻降回来"，永远到不了稳态。
 *
 * 主流怎么做（都是读源码来的）：
 *   * dash.js 默认的 **BolaRule** 就是**缓冲优先**：码率由缓冲水位决定，
 *     吞吐量只作为上限（`representationForThroughput`）；缓冲高就待着不动。
 *   * dash.js `InsufficientBufferRule` 有 `segmentIgnoreCount = 2` —— 换档后
 *     **前两个分片不做判断**，正是为了躲开这段"缓冲重新攒"的窗口。
 *   * ExoPlayer `AdaptiveTrackSelection`：缓冲 >= 25s 时**推迟降档**
 *     （`DEFAULT_MAX_DURATION_FOR_QUALITY_DECREASE_MS`），只有缓冲掉下来才降。
 *
 * 所以这里的规则改成：
 *   1) 缓冲充足（>= ABR_HIGH_BUFFER_MS）→ 只考虑**上切**（带宽有余量才上）；
 *   2) 缓冲偏低（< ABR_LOW_BUFFER_MS）且**连续若干秒都低** → 才降档，
 *      降到"这段时间的吞吐撑得住"的那一档；
 *   3) 缓冲见底（< ABR_PANIC_BUFFER_MS）→ 立刻降档（不等连续计数）；
 *   4) 中间地带、以及换档后的宽限期（ABR_POST_SWITCH_GRACE_MS）里 → **什么都不做**。
 * 这样"网络有波动"不会导致降档，只有"缓冲真的不够了"才降 —— 就是用户要的行为。
 */
/*
 * ⚠ 这几个缓冲阈值是**和播放器那边的缓冲上限配套**的，改一个要一起看另一个：
 *   应用层（platform/QtPlayer/src/CicadaPlayerItem.cpp，createPlayer）把 DASH/HLS
 *   的 `maxBufferDuration` 收成了 **15 秒**（用户要求：分片流别缓太多，否则每次换档
 *   旧档的包全白下）。所以这里的"高/低水位"必须落在 15 秒以内，否则会出现
 *   "永远够不到上切门槛"（缓冲上限 < 上切门槛）或者"降档太迟钝"。
 *   现在的取值：上切 ≥ 8 秒、连续 3 秒低于 7 秒才降档、低于 4 秒立刻降档 ——
 *   在 15 秒上限下，健康网络稳定在 12~15 秒（可上切），被限速时会掉到 7 秒以下
 *   （降档），两个方向都有明确的触发区间。
 */
#define ABR_LOW_BUFFER_MS (7 * 1000)
#define ABR_HIGH_BUFFER_MS (15 * 1000)
/* 换档后的宽限期：这段时间缓冲正在重新攒，不做任何降档判断。 */
#define ABR_POST_SWITCH_GRACE_MS (5 * 1000)
/* 缓冲偏低要连续这么多秒才真的降档（去抖，避免一次抖动就降）。 */
#define ABR_LOW_BUFFER_TICKS (3)

/* ---- ExoPlayer AdaptiveTrackSelection ---- */
/*
 * 上切要求的最低缓冲。ExoPlayer 的默认值是 10 秒；这里取 **8 秒** ——
 * 因为播放器的 DASH/HLS 缓冲上限是 15 秒（见上面那段说明），
 * 10 秒在"刚攒到 12 秒就被读循环暂停"的锯齿里偶尔会误判成"缓冲不够"，
 * 8 秒既保留了 ExoPlayer 的保守性，又不会把正常状态卡在上切门外。
 */
#define ABR_MIN_BUFFER_FOR_UP_MS (8 * 1000)
/* ExoPlayer 的 DEFAULT_MAX_DURATION_FOR_QUALITY_DECREASE_MS（25s）在 15 秒上限下
 * 永远达不到，所以这里跟着上限走：缓冲到 15 秒（= 上限）就明确"不该降档"。 */
#define ABR_MAX_BUFFER_FOR_DOWN_MS (15 * 1000)

/* ---- Shaka abr.switchInterval / dash.js abandonLoadTimeout ---- */
#define ABR_MIN_SWITCH_INTERVAL_MS (8 * 1000)
#define ABR_UP_BAN_AFTER_DOWN_MS (10 * 1000)
/* 请求出去之后等播放器回终态的上限。必须**大于**播放器自己的切换上限
 * （SuperMediaPlayer 侧那个 18s 的切档上限），否则
 * 一次正常的、需要等到关键帧边界的切换还没结束就被判"没完成"，
 * 放锁之后又立刻重发同一个请求 —— 实测会变成每 8 秒一次的请求风暴。 */
#define ABR_SWITCH_WATCHDOG_MS (20 * 1000)
/*
 * 【第七项：死代码清理 —— 这一条**保留**，并在此标注】
 * 它是一条"请求发出后等播放器回终态"的上限（:407 处活跃使用），属于"切换永不回报"
 * 的兜底。按约定的规则：要删它必须先给出"切换一定会回报"的事件出口，而当前并不存在
 * 这样一个出口（切档终态的收敛依赖上屏/失败上报，暂停、后台、seek 交织时无法保证
 * 一定回报），因此**给不出就不删**，只在此注明它是本轮唯一保留的 ABR 侧时间兜底。
 */

double AbrBufferAlgoStrategy::Ewma::Alpha() const
{
    return exp(log(0.5) / halfLifeS);
}

void AbrBufferAlgoStrategy::Ewma::Reset()
{
    estimateBps = 0.0;
    totalWeightS = 0.0;
}

void AbrBufferAlgoStrategy::Ewma::Sample(double weightS, double valueBps)
{
    if (weightS <= 0.0 || valueBps <= 0.0) {
        return;
    }

    const double adjAlpha = pow(Alpha(), weightS);
    estimateBps = valueBps * (1.0 - adjAlpha) + adjAlpha * estimateBps;
    totalWeightS += weightS;
}

double AbrBufferAlgoStrategy::Ewma::Get() const
{
    /* 抵消"估计值从 0 起步"造成的低估（hls.js 的 zeroFactor）。 */
    const double zeroFactor = 1.0 - pow(Alpha(), totalWeightS);

    if (zeroFactor <= 0.0) {
        return 0.0;
    }

    return estimateBps / zeroFactor;
}

AbrBufferAlgoStrategy::AbrBufferAlgoStrategy(std::function<void(int)> func) : AbrAlgoStrategy(std::move(func))
{
    Reset();
}

AbrBufferAlgoStrategy::~AbrBufferAlgoStrategy() = default;

void AbrBufferAlgoStrategy::SetCurrentBitrate(int bitrate)
{
    AF_LOGI("BA already change to bitrate:%d", bitrate);
    AbrAlgoStrategy::SetCurrentBitrate(bitrate);
    /*
     * 播放器回话了：这次切换（不管是用户点的还是 ABR 发的）已经落地，
     * 放掉在途标记。
     *
     * 【2026-09-21 修两点】
     *
     * 1) 不要在这里重置 mLastSwitchTimeMS。切换间隔（Shaka abr.switchInterval）
     *    计的是"两次 ABR 决策之间"，Shaka 只在 chooseVariant() 里更新它。
     *    以前在每次 SetCurrentBitrate 都重置，而 mediaInfoGet / seekEnd /
     *    streamChangedSuc 都会调它 —— 于是每 seek 一次、每手点一次，ABR 就又被
     *    压 8 秒，起播之后也要白等 8 秒才允许第一次上切。
     *
     * 2) 用当前档位的码率给 EWMA 播一个种子，这是 hls.js 的
     *    "Update default bwe to first variant bitrate"（level-controller.ts）：
     *    没拿到真实样本之前，把估计值设成起播档的码率（被
     *    abrEwmaDefaultEstimateMax = 5Mbps 夹住），而不是那个与片源无关的
     *    500kbps 兜底值。否则一条 1M~10M 的码率阶梯上，ABR 的起点永远是
     *    "最低档"，上切要爬很久。
     */
    mSwitching = false;
    mSwitchingSinceMs = 0;

    /*
     * 一次换档真的落地了（不管是 ABR 发的还是用户手点的）。记下时间：
     * 接下来的 ABR_POST_SWITCH_GRACE_MS 里，新 Representation 的缓冲正在重新攒，
     * "缓冲很小"是正常现象，不做降档判断（dash.js 的 segmentIgnoreCount 同理）。
     */
    mLastSwitchDoneMs = af_getsteady_ms();
    mLowBufferTicks = 0;

    if (mSampledBytes == 0 && bitrate > 0) {
        const double seed = std::min((double) bitrate, ABR_START_ESTIMATE_MAX_BPS);
        mFast.Reset();
        mSlow.Reset();
        mFast.Sample(ABR_SAMPLE_WEIGHT_S, seed);
        mSlow.Sample(ABR_SAMPLE_WEIGHT_S, seed);
        AF_LOGI("BA seeded the bandwidth estimate from the current bitrate: %.0f kbps\n", seed / 1024.0);
    }
}

void AbrBufferAlgoStrategy::Reset()
{
    mFast.halfLifeS = ABR_FAST_HALF_LIFE_S;
    mSlow.halfLifeS = ABR_SLOW_HALF_LIFE_S;
    mFast.Reset();
    mSlow.Reset();
    mSampledBytes = 0;
    mSwitching = false;
    mSwitchingSinceMs = 0;
    mLastSwitchTimeMS = INT64_MIN;
    mUpSwitchBannedUntilMs = 0;
    mLastWaitingLogMs = 0;
    mLastSampledSpeed = 0;
    mLowBufferTicks = 0;
    mLastSwitchDoneMs = 0;
    mDownloadSpeed.clear();
    mBufferStatics.clear();
}

/*
 * 采样一次吞吐。
 *
 * 【2026-09-21 修：缓冲满了也要采样】
 *
 * 旧实现（和它抄来的旧代码）在 `bufferFull` 时直接 return，理由是"下载都停了，
 * 这时候的速度不代表网络能力"。这个理由在**网络慢**的时候成立，但在**网络快**
 * 的时候会直接把 ABR 废掉：本地/内网服务器几秒钟就把 maxBufferDuration(50s)
 * 填满，之后一直处于 bufferFull，于是采样永远是 0 个，`mSampledBytes` 永远
 * 到不了门槛 → ABR 一次决策都不做 → 用户看到的就是"自动档永远停在起播那档
 * （默认最低档）"。用户实测反馈正是这个。
 *
 * 正确的做法看 hls.js：它的吞吐样本来自**每个分片下载完成**（bytes/耗时），
 * 与缓冲深度无关。我们这里的数据源每秒滚动一次窗口（MediaPlayerUtil 里
 * timeS > 1.0 才更新），所以：
 *   * 值 > 0 就当成有效样本喂给 EWMA —— 读包暂停时数据源保留的是**上一次真正
 *     测到的速度**，那正是"最近一次观测到的网络能力"，比丢弃它更接近事实；
 *   * 但只有值发生变化时才累加字节数，避免同一个陈旧值被反复计成新流量。
 */
void AbrBufferAlgoStrategy::UpdateThroughput()
{
    const int64_t speed = mRefererData->GetCurrentDownloadSpeed();

    if (speed <= 0) {
        return;
    }

    if (speed == mLastSampledSpeed) {
        return;
    }

    mLastSampledSpeed = speed;
    mDownloadSpeed.push_back(speed);

    while (mDownloadSpeed.size() > MAX_DOWNLOAD_SPEED_HISTORY) {
        mDownloadSpeed.pop_front();
    }

    mFast.Sample(ABR_SAMPLE_WEIGHT_S, (double) speed);
    mSlow.Sample(ABR_SAMPLE_WEIGHT_S, (double) speed);
    mSampledBytes += (int64_t) ((double) speed / 8.0 * ABR_SAMPLE_WEIGHT_S);
}

double AbrBufferAlgoStrategy::GetThroughput() const
{
    /* hls.js `EwmaBandWidthEstimator.getEstimate()` 与 Shaka
     * `getBandwidthEstimate()` 都是取快慢两者的较小值：对"带宽突然变差"
     * 反应更快，不会因为慢半衰期还记着旧的高值而切不上去。 */
    if (mSampledBytes < ABR_MIN_TOTAL_BYTES) {
        return ABR_DEFAULT_BANDWIDTH_BPS;
    }

    return std::min(mFast.Get(), mSlow.Get());
}

int AbrBufferAlgoStrategy::FindCurrentIndex() const
{
    int best = -1;

    for (int i = 0; i < (int) mBitRates.size(); i++) {
        if (mBitRates[i] == mCurrentBitrate) {
            return i;
        }

        if (mBitRates[i] <= mCurrentBitrate) {
            best = i;
        }
    }

    return best;
}

/* 在 <= maxIndex 的档位里，选码率不超过 budgetBps 的**最高**档。
 * 对应 ExoPlayer `determineIdealSelectedIndex()` 里
 * `canSelectFormat(format, format.bitrate, effectiveBitrate)` 的语义
 * （trackBitrate <= effectiveBitrate）。都不满足时返回 -1。 */
int AbrBufferAlgoStrategy::BestIndexForBudget(double budgetBps, int maxIndex) const
{
    const int limit = std::min(maxIndex, (int) mBitRates.size() - 1);

    for (int i = limit; i >= 0; i--) {
        if ((double) mBitRates[i] <= budgetBps) {
            return i;
        }
    }

    return -1;
}

int AbrBufferAlgoStrategy::PreferEfficientCodecAtSameResolution(int index, int maxBitrate) const
{
    const int better = FindSameResolutionEfficientCodec(index, maxBitrate);

    return (better >= 0) ? better : index;
}

void AbrBufferAlgoStrategy::RequestSwitch(int index, bool up, bool banUp, const char *why)
{
    if (index < 0 || index >= (int) mBitRates.size()) {
        return;
    }

    const int bitrate = mBitRates[index];

    if (bitrate == mCurrentBitrate) {
        return;
    }

    const auto iter = mStreamIndexBitrateMap.find(bitrate);

    if (iter == mStreamIndexBitrateMap.end()) {
        AF_LOGW("ABR wants bitrate=%d but no stream index is registered for it\n", bitrate);
        return;
    }

    const int64_t nowMs = af_getsteady_ms();
    const int64_t bufferMs = mRefererData->GetCurrentPacketBufferLength() / 1000;

    AF_LOGI("ABR switch %s: bitrate %d -> %d (index %d, stream %d) throughput=%.0f kbps "
            "fast=%.0f kbps slow=%.0f kbps buffer=%lld ms reason=%s\n",
            up ? "up" : "down", mCurrentBitrate, bitrate, index, iter->second,
            GetThroughput() / 1024.0, mFast.Get() / 1024.0, mSlow.Get() / 1024.0,
            (long long) bufferMs, why != nullptr ? why : "-");

    mPreBitrate = mCurrentBitrate;
    mCurrentBitrate = bitrate;
    mSwitching = true;
    mSwitchingSinceMs = nowMs;
    mLastSwitchTimeMS = nowMs;

    if (!up && banUp) {
        /* dash.js 在放弃一次下载后 10 秒内禁止上切。这里无法区分"放弃"和
         * 普通降档，所以对所有降档都套同一条（更保守）。 */
        mUpSwitchBannedUntilMs = nowMs + ABR_UP_BAN_AFTER_DOWN_MS;
    }

    updateSwitchStatus(Status::Switch, true);
    mFunc(iter->second);
}

/*
 * 主决策。每秒被 AbrManager 调一次。
 *
 * 顺序：先看是否还在等上一次切换（含看门狗）→ 采样本 → 紧急降档 →
 * 切换间隔节流 → 上切 → 降档。
 */
void AbrBufferAlgoStrategy::ProcessAbrAlgo()
{
    if (mRefererData == nullptr || mCurrentBitrate <= 0 || mBitRates.size() < 2) {
        return;
    }

    /* 整段都已经下完（本地文件 / 全量缓存），切了也没有意义。 */
    if (mRefererData->IsDownloadCompleted()) {
        return;
    }

    const int64_t nowMs = af_getsteady_ms();

    if (mSwitching) {
        if (mSwitchingSinceMs > 0 && nowMs - mSwitchingSinceMs < ABR_SWITCH_WATCHDOG_MS) {
            return;
        }

        /* 上一次请求的切换迟迟没有落地（超时/失败/被 flush 掉）。
         * 旧实现就是在这里永久锁死的。把乐观改写的当前档位回退到实际档位，
         * 放锁，让下一轮重新评估。 */
        AF_LOGW("ABR: switch to bitrate=%d did not complete in %lld ms, releasing the in-flight "
                "flag and rolling back to bitrate=%d\n",
                mCurrentBitrate, (long long) (nowMs - mSwitchingSinceMs), mPreBitrate);
        mCurrentBitrate = mPreBitrate;
        mSwitching = false;
        mSwitchingSinceMs = 0;
    }

    const int64_t bufferMs = mRefererData->GetCurrentPacketBufferLength() / 1000;
    const int64_t maxBufferMs = mRefererData->GetMaxBufferDurationInConfig() / 1000;

    UpdateThroughput();

    if (mSampledBytes < ABR_MIN_TOTAL_BYTES) {
        if (mLastWaitingLogMs == 0 || nowMs - mLastWaitingLogMs >= 5000) {
            mLastWaitingLogMs = nowMs;
            AF_LOGI("ABR waiting for samples: %lld/%d bytes, buffer=%lld/%lld ms, speed=%lld bps\n",
                    (long long) mSampledBytes, (int) ABR_MIN_TOTAL_BYTES, (long long) bufferMs,
                    (long long) maxBufferMs,
                    (long long) mRefererData->GetCurrentDownloadSpeed());
        }

        return;
    }

    /* 缓冲趋势只用于日志/上报，不再参与决策。 */
    if (mBufferStatics.empty() || bufferMs != mBufferStatics.back()) {
        mBufferStatics.push_back((int) (bufferMs / 1000));

        while (mBufferStatics.size() > 10) {
            mBufferStatics.pop_front();
        }
    }

    const int currentIndex = FindCurrentIndex();

    if (currentIndex < 0) {
        return;
    }

    const double throughput = GetThroughput();
    const int count = (int) mBitRates.size();

    /* 缓冲低水位要连续出现才作数（去抖）。 */
    if (bufferMs < ABR_LOW_BUFFER_MS) {
        if (mLowBufferTicks < 1000) {
            ++mLowBufferTicks;
        }
    } else {
        mLowBufferTicks = 0;
    }

    /*
     * 换档宽限期（dash.js InsufficientBufferRule 的 segmentIgnoreCount = 2）：
     * 刚换完档，缓冲正在按新 Representation 重新攒，这时候的"小缓冲"不代表网络
     * 不行。这段窗口内只允许上切（缓冲厚的时候本来也不会触发），不做任何降档。
     */
    const bool inPostSwitchGrace = mLastSwitchDoneMs > 0 &&
                                   (nowMs - mLastSwitchDoneMs) < ABR_POST_SWITCH_GRACE_MS;

    /* ---- 1) 紧急降档：缓冲快见底，先保住不断流 ---- */
    if (bufferMs < ABR_PANIC_BUFFER_MS && currentIndex > 0 && !inPostSwitchGrace) {
        const int target = BestIndexForBudget(throughput, currentIndex - 1);

        AF_LOGW("ABR panic down-switch: buffer=%lld ms < %d ms, keeping the stream alive\n",
                (long long) bufferMs, (int) ABR_PANIC_BUFFER_MS);
        /* 见底时**不做**同分辨率换编码：换编码要重建解码器，来不及，先降档保命。
         * 这里也**不**为"硬解"做同分辨率替换 —— 同一个理由：缓冲只剩不到 4 秒的时候，
         * 一次解码器重建本身就足以造成可见卡顿，保不断流优先于换编码。
         * （正常降档路径见下面第 4 条，它带着码率上限做同分辨率编码收敛。） */
        RequestSwitch(target >= 0 ? target : 0, false, true, "buffer about to run out");
        return;
    }

    /* ---- 2) 切换间隔（Shaka abr.switchInterval） ---- */
    if (mLastSwitchTimeMS != INT64_MIN && nowMs - mLastSwitchTimeMS < ABR_MIN_SWITCH_INTERVAL_MS) {
        return;
    }

    /*
     * 候选档的同分辨率编码收敛。BestIndexForBudget() 只按码率选档，同一个清晰度上
     * 同时有 H.264 / H.265 / AV1 时它可能落到"白花带宽"的那一路上（也可能落到设备
     * 根本解不了的那一路上）。FindSameResolutionEfficientCodec() 负责在**同一分辨率**
     * 内部把它收敛成该清晰度上更该选的那一档：硬解优先，其次压缩效率（AV1 > H.265 >
     * VP9 > H.264 > MPEG-4/2），分辨率不会变。
     *
     * 两条路径的码率上限刻意不同：
     *   * 上切/稳态（upCandidate）：**不限**码率。硬解是第一约束 —— 当预算挑中的那一档
     *     设备解不了、而同一清晰度上能硬解的那一档略贵时，宁可换成能硬解的那一档
     *     （分辨率/画质不变，只多花一点带宽），也不要留下一条软解的流。万一这次替换
     *     真的超出了吞吐能力，下一轮降档判定会把它收回来。
     *   * 降清晰度（downCandidate）：上限 = 该目标档自己的码率。缓冲已经不够了，
     *     这时换到一个**更贵**的编码等于把这次降档白做（很可能立刻又掉进紧急降档），
     *     所以这一路只接受"同分辨率里更省带宽的编码"（用户第 2 条），不允许抬码率。
     */
    int upCandidate = BestIndexForBudget(ABR_UP_BANDWIDTH_FACTOR * throughput, count - 1);
    int downCandidate = BestIndexForBudget(ABR_DOWN_BANDWIDTH_FACTOR * throughput, currentIndex);

    if (upCandidate >= 0) {
        upCandidate = PreferEfficientCodecAtSameResolution(upCandidate, 0);
    }

    if (downCandidate >= 0) {
        downCandidate = PreferEfficientCodecAtSameResolution(downCandidate, mBitRates[downCandidate]);
    }

    /* ---- 3) 上切：缓冲够厚（ExoPlayer 的 >= 10s，这里用"高水位"更保守）+ 带宽有余量 ---- */
    if (upCandidate > currentIndex) {
        if (bufferMs < ABR_MIN_BUFFER_FOR_UP_MS) {
            AF_LOGD("ABR hold up-switch: buffer=%lld ms < %d ms (bitrate %d -> %d)\n",
                    (long long) bufferMs, (int) ABR_MIN_BUFFER_FOR_UP_MS, mCurrentBitrate,
                    mBitRates[upCandidate]);
            return;
        }

        if (nowMs < mUpSwitchBannedUntilMs) {
            AF_LOGD("ABR hold up-switch: %lld ms left of the up-switch ban\n",
                    (long long) (mUpSwitchBannedUntilMs - nowMs));
            return;
        }

        RequestSwitch(upCandidate, true, false, "throughput headroom");
        return;
    }

    /*
     * ---- 3.5) 同一清晰度内换编码：当前档所在的分辨率上有更该选的那一档 ----
     *
     * 两个诉求都靠这条落地：
     *   * 手动/起播落到了 H.264 那条，而同一清晰度还有更省带宽的 H.265/AV1 时，
     *     自动档主动搬到省带宽的那一路；
     *   * **设备解不了当前这一路**（例如只支持 H.264 却起播在 AV1 上）时，搬到同一
     *     清晰度上设备能硬解的那一路 —— 这时即便那一档码率更高也换（硬解第一约束，
     *     与上面 upCandidate 用"不限码率"是同一个理由）。
     *
     * 门限刻意和"上切"一样保守（缓冲 >= ABR_MIN_BUFFER_FOR_UP_MS、不在换档宽限期）：
     * 换编码要把新 Representation 从 0 重新攒缓冲 + 重建解码器，是**最贵**的一次
     * 切换，缓冲薄的时候绝不能做（那时该走下面的降档）。
     * 用 banUp=false：这不是降清晰度，不该吃"降档后 10 秒禁上切"。
     */
    if (!inPostSwitchGrace && bufferMs >= ABR_MIN_BUFFER_FOR_UP_MS) {
        const int lateral = FindSameResolutionEfficientCodec(currentIndex, 0);

        if (lateral >= 0) {
            RequestSwitch(lateral, false, false, "same resolution, better codec (hw first, then efficiency)");
            return;
        }
    }

    /*
     * ---- 4) 降档：**只有缓冲真的不够了才降** ----
     *
     * 注意这里刻意**不再**用"吞吐量低于当前档位"作为降档条件：网络抖动一下
     * （吞吐瞬间掉到档位之下）不该换档，缓冲还厚就照常播 —— 缓冲被吃薄了自然
     * 会走到下面这条。dash.js 的 BolaRule 就是这么做的（码率由缓冲水位决定，
     * 吞吐只作为上限），ExoPlayer 也有"缓冲 >= 25s 时推迟降档"的同一条。
     *
     * downCandidate 已经在上面的"候选档同分辨率编码收敛"里处理过（带码率上限），
     * 所以这里落地的就是用户第 2 条要的那一档：**目标清晰度内**优先能硬解的编码、
     * 其次压缩效率更高的编码，且永远不会比 BestIndexForBudget() 选出的那一档更贵。
     * 清晰度档位只降不升 —— 换编码绝不跨分辨率。
     */
    if (bufferMs < ABR_LOW_BUFFER_MS && mLowBufferTicks >= ABR_LOW_BUFFER_TICKS && !inPostSwitchGrace) {
        if (downCandidate < currentIndex) {
            AF_LOGI("ABR down-switch: buffer stayed below %d ms for %d ticks "
                    "(buffer=%lld ms, throughput=%.0f kbps)\n",
                    (int) ABR_LOW_BUFFER_MS, (int) mLowBufferTicks,
                    (long long) bufferMs, throughput / 1024.0);
            RequestSwitch(downCandidate >= 0 ? downCandidate : 0, false, true, "buffer too low");
            return;
        }
    }

    if (inPostSwitchGrace || bufferMs >= ABR_HIGH_BUFFER_MS) {
        AF_LOGD("ABR hold: buffer=%lld ms throughput=%.0f kbps grace=%d (keep the current level)\n",
                (long long) bufferMs, throughput / 1024.0, (int) inPostSwitchGrace);
    }
}

/*
 * 用户从手动档切回"自动"（MediaPlayer::SelectTrack(SELECT_TRACK_VIDEO_AUTO) →
 * AbrManager::EnableAbr(true)）。
 *
 * 必须清掉三样东西，否则"切到自动"之后还要白等好几秒才可能动：
 *   * mSwitching / mSwitchingSinceMs —— 上一次 ABR 请求的在途标记；手动切档期间
 *     ABR 被关掉，没人回调 SetCurrentBitrate，它就一直挂着，看门狗要等 8 秒；
 *   * mUpSwitchBannedUntilMs —— 降档后 10 秒禁上切；用户刚刚明确要求"自动"，
 *     不该再吃这条禁令；
 *   * mLastSwitchTimeMS —— 切换间隔。Shaka 的 switchInterval 是"两次 ABR 决策
 *     之间"的间隔，重新打开 ABR 就是一次全新的开始。
 *
 * 真实档位不用在这里修：播放器每次切流成功都会回调 SetCurrentBitrate()，
 * 它才是 mCurrentBitrate 的唯一权威来源。
 */
void AbrBufferAlgoStrategy::OnAbrEnabled()
{
    AF_LOGI("ABR enabled again: dropout in-flight=%d ban=%lld ms dwell=%lld ms — "
            "re-evaluating from the next tick\n",
            (int) mSwitching,
            (long long) (mUpSwitchBannedUntilMs > 0 ? mUpSwitchBannedUntilMs - af_getsteady_ms() : 0),
            (long long) (mLastSwitchTimeMS == INT64_MIN ? -1 : af_getsteady_ms() - mLastSwitchTimeMS));

    mSwitching = false;
    mSwitchingSinceMs = 0;
    mUpSwitchBannedUntilMs = 0;
    mLastSwitchTimeMS = INT64_MIN;
    mLowBufferTicks = 0;
}

void AbrBufferAlgoStrategy::updateSwitchStatus(Status newStatus, bool forceCb)
{
    AF_LOGD("BA switch status:%d", newStatus);

    Status oldStatus = mSwitchStatus;
    mSwitchStatus = newStatus;

    if (oldStatus != newStatus || forceCb) {
        if (mStatusCallback) {
            mStatusCallback(newStatus);
        }
    }
}

void AbrBufferAlgoStrategy::GetOption(const std::string &key, std::string &value)
{
    if (key == "switchInfo") {
        CicadaJSONItem result{};
        result.addValue("fb", (int) mPreBitrate);
        result.addValue("tb", (int) mCurrentBitrate);
        /* CicadaJSONItem 只有 long/int/double 三个数值重载，int64_t 会歧义，
         * 所以这里显式收敛类型。 */
        result.addValue("fast", (long) mFast.Get());
        result.addValue("slow", (long) mSlow.Get());
        result.addValue("bytes", (double) mSampledBytes);

        CicadaJSONArray speedInfos{};
        for (auto &speedItem : mDownloadSpeed) {
            speedInfos.addInt64(speedItem);
        }
        result.addArray("spd", speedInfos);

        CicadaJSONArray bufferInfos{};
        for (auto &bufferItem : mBufferStatics) {
            bufferInfos.addInt64(bufferItem);
        }
        result.addArray("buf", bufferInfos);

        value = result.printJSON();
    }
}
