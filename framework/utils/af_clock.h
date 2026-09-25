//
// Created by moqi on 2018/9/18.
//

#ifndef SOURCE_AF_CLOCK_H
#define SOURCE_AF_CLOCK_H

#include <cstdint>
#include <atomic>

class af_clock {
public:
    af_clock();

    ~af_clock();

    void start();

    void pause();

    void set(int64_t time);

    int64_t get();

    bool isPaused();

    void reset();


private:
    std::atomic<int64_t> mStartUs{0};
    std::atomic_int mStatus{0};
    int64_t mSetUs = 0;
    int64_t mPauseUs = 0;
};

class af_scalable_clock {
public:
    af_scalable_clock();

    virtual ~af_scalable_clock();

    void start();

    void pause();

    void set(int64_t time);

    int64_t get();

    void setSpeed(float speed);

    float getSpeed();

    bool isPaused();

    virtual void reset();

private:
    std::atomic<float> mScale{1.0f};
    af_clock mClock;
    std::atomic<int64_t> mSetTime{0};
    int64_t mScaleStartTime = 0;

};

/*
 * ============ 【B5-5】"这个时间值显然是未初始化/环绕出来的" ============
 *
 * 背景（真机日志，Android + DASH，2026-09-25）：
 *   `[AlivcPlayerClock] :TIMEPOS reSync time 30018965 to  -9223372036824759809`
 *   `[AlivcPlayerClock] :TIMEPOS reSync time -9223372036824753533 to  30016000`
 *   `[ApsaraPlayerService] :drop frame,master played time is 9223372036854775570,video pts is 250250`
 *
 * 成因：一旦有人给时钟 set 了哨兵值（INT64_MIN），`af_scalable_clock::get()` 就是
 * `mSetTime + elapsed`，于是返回"哨兵 + 一小段 elapsed"的**环绕值**；它再被当成真时间
 * 参与比较（甚至被 set 回时钟里去）→ 差值可能溢出、判定全错、日志不可读。
 *
 * 判据只做状态判断，没有任何时间阈值、没有看门狗：真实媒体时间不可能落在
 * INT64 极值的 1 小时之内，所以落进去就是未初始化/已环绕。
 */
inline bool af_clock_value_is_unset(int64_t v)
{
    const int64_t kUnsetBandUs = 3600LL * 1000 * 1000; /* 1 小时 */
    return v >= (INT64_MAX - kUnsetBandUs) || v <= (INT64_MIN + kUnsetBandUs);
}

#endif //SOURCE_AF_CLOCK_H
