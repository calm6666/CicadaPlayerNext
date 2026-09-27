//
//  AbrAlgoStrategy.cpp
//  apsara_player
//
//  Created by shiping.csp on 2018/11/1.
//

#include "AbrAlgoStrategy.h"
#include "AbrRefererData.h"
#include <algorithm>
#include <utility>
#include <utils/AFMediaType.h>
/*
 * 硬解能力查询（decoderFactory::isHardwareDecodeSupported）。它是 L1 的无平台宏接口
 * —— 平台实现关在 framework/codec/decoderFactory.cpp 里，这里看不到任何平台差异。
 */
#include <codec/decoderFactory.h>

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
        mLevelInfos[i].codecRank = afCodecEfficiencyRankByShortName(mLevelInfos[i].codec.c_str());
        /*
         * 硬解能力：短名 -> AFCodecID -> 问平台（两者的编码表只有一份，
         * 见 afCodecIDFromShortName / decoderFactory::isHardwareDecodeSupported）。
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
