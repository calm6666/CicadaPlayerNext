//
//  AbrBufferAlgoStrategy.h
//  apsara_player
//
//  Created by shiping.csp on 2018/11/1.
//

#ifndef AbrBufferAlgoStrategy_h
#define AbrBufferAlgoStrategy_h

#include "AbrAlgoStrategy.h"
#include <list>

using namespace std;

/*
 * 缓冲/吞吐混合 ABR 策略。
 *
 * 名字里的 "Buffer" 是历史遗留：旧实现只看缓冲涨跌趋势，不看带宽。
 * 现在改成"带宽估计为主、缓冲深度把关"——依据见 .cpp 顶部的出处清单
 * （hls.js 的双半衰期 EWMA、dash.js 的缓冲约束、ExoPlayer 的上下切缓冲门限、
 * Shaka 的切换间隔）。
 */
class AbrBufferAlgoStrategy : public AbrAlgoStrategy {
public:
    explicit AbrBufferAlgoStrategy(std::function<void(int)> func);
    ~AbrBufferAlgoStrategy() override;

public:
    void Reset() final;

    void ProcessAbrAlgo() override;

    void SetCurrentBitrate(int bitrate) override;

    void GetOption(const std::string &key, std::string &value) override;

    /* 从手动档切回"自动"：清掉在途标记/切换间隔/上切禁令，下一个 tick 就重新评估。 */
    void OnAbrEnabled() override;

private:
    /*
     * 双半衰期 EWMA 吞吐估计。
     *
     * 逐字对应当前 hls.js 的实现（src/utils/ewma.ts、ewma-bandwidth-estimator.ts）：
     *   alpha     = exp(ln(0.5) / halfLife)
     *   sample(w) : adj = alpha^w; estimate = value*(1-adj) + adj*estimate; weight += w
     *   get()     : estimate / (1 - alpha^weight)
     * 最后那一步是必需的：估计值从 0 起步，样本很少时会被系统性低估，
     * 除以 (1 - alpha^weight) 正好抵消这段"热机"偏差（hls.js 的 zeroFactor）。
     */
    struct Ewma {
        double halfLifeS{1.0};
        double estimateBps{0.0};
        double totalWeightS{0.0};

        double Alpha() const;
        void Reset();
        void Sample(double weightS, double valueBps);
        double Get() const;
    };

private:
    void UpdateThroughput();
    double GetThroughput() const;
    int FindCurrentIndex() const;
    int BestIndexForBudget(double budgetBps, int maxIndex) const;
    /*
     * 同分辨率编码偏好的落地：把 index 收敛成"同一清晰度下更该选的那一档"
     * （硬解优先，其次压缩效率；判据全部在基类
     * AbrAlgoStrategy::FindSameResolutionEfficientCodec() 里），这里只负责
     * "没有可换的档就原样返回"。
     *
     * maxBitrate 的语义见基类：> 0 是"换过去的码率上限"（降清晰度那条路径用，
     * 保证一次降档不会被换成更贵的编码），<= 0 是不限（升清晰度/稳态那条路径用）。
     */
    int PreferEfficientCodecAtSameResolution(int index, int maxBitrate) const;
    /*
     * banUp：这次切换是否要套上"降档后 10 秒禁上切"。
     * 同一分辨率内换编码不是降清晰度，不该吃这条禁令（abrupt 换编码已经够贵了，
     * 再压 10 秒上切只是白等），所以单独给一个开关。
     */
    void RequestSwitch(int index, bool up, bool banUp, const char *why);
    void updateSwitchStatus(Status newStatus, bool forceCb);

private:
    Ewma mFast;
    Ewma mSlow;
    /* 已采样字节数。Shaka 用 advanced.minTotalBytes = 128KB 决定"估计值可信"，
     * 这里用同一个量纲：样本足够之前不动已经播起来的档位。 */
    int64_t mSampledBytes = 0;

    bool mSwitching = false;
    int64_t mSwitchingSinceMs = 0;
    int64_t mLastSwitchTimeMS = INT64_MIN;
    int64_t mUpSwitchBannedUntilMs = 0;
    int64_t mLastWaitingLogMs = 0;
    /* 上一次采到的下载速度。数据源是"每秒滚动一次窗口"的瞬时值，缓冲满时它
     * 会保持不变；用它去重，避免同一个陈旧值被反复计成新流量。 */
    int64_t mLastSampledSpeed = 0;
    /* 缓冲低水位连续出现的次数（< ABR_LOW_BUFFER_TICKS 不降档）。 */
    int mLowBufferTicks = 0;
    /* 上一次"换档真正落地"的时刻（SetCurrentBitrate 回调）。换档之后缓冲要按
     * 新 Representation 重新攒，这段时间不做降档判断（见 ABR_POST_SWITCH_GRACE_MS）。 */
    int64_t mLastSwitchDoneMs = 0;

    std::list<int64_t> mDownloadSpeed;
    std::list<int> mBufferStatics;
    Status mSwitchStatus{Status::Switch};
};

#endif /* AbrBufferAlgoStrategy_h */
