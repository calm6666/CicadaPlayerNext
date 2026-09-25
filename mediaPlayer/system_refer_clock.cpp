
#define LOG_TAG "AlivcPlayerClock"

#include <utils/timer.h>
#include "system_refer_clock.h"
#include "utils/frame_work_log.h"

#define CLOCK_DEVIATION_TIME_US (100 * 1000)

namespace Cicada {

    int64_t SystemReferClock::GetTime()
    {
        int64_t timeRet = mClock.get();

        if (mGetClock && !mClock.isPaused()) {
            int64_t referTime = mGetClock(mClockArg);

            /*
             * ============ 【B5-5：哨兵/环绕值不得进入算术，也不得写回时钟】============
             *
             * 两个来源都会给出"未初始化"的值（判据见 af_clock.h 里
             * af_clock_value_is_unset()，纯状态判断、无时间阈值）：
             *   · referTime：参考时钟（音频时间戳）内部还带着 INT64_MIN 哨兵时返回的是
             *     "哨兵 + elapsed"的环绕值 —— 真机日志
             *     `TIMEPOS reSync time 30018965 to  -9223372036824759809`；
             *   · timeRet：本地时钟被 set 过哨兵值时同理 ——
             *     `TIMEPOS reSync time -9223372036824753533 to  30016000`。
             *
             * 旧写法的两个后果（都在真机日志里能看到）：
             *   ① `llabs(referTime - timeRet)` 在两者都是环绕值时**可能溢出**，于是
             *      "偏差很小"的假象会让该做的 resync 被跳过；
             *   ② 更糟的是它会把环绕值 `mClock.set(referTime)` **写回主时钟** ——
             *      主时钟从此返回环绕值，`RenderVideo` 的早/晚判定全错，起播阶段连丢
             *      约 10 帧（`drop frame,master played time is 9223372036854775570`）。
             *
             * 处理：拿不到有效的参考值就**不动时钟**（保持原值，等下一次回调）；
             * 本地值未初始化时不做差值比较，直接采纳有效的参考值。两者都是纯状态判断，
             * 不引入任何计时器。
             */
            const bool referValid = (INT64_MIN != referTime) && !af_clock_value_is_unset(referTime);
            const bool selfValid = !af_clock_value_is_unset(timeRet);

            if (referValid && (!selfValid || llabs(referTime - timeRet) > CLOCK_DEVIATION_TIME_US)) {
                mClock.set(referTime);

                if (selfValid) {
                    AF_LOGW("TIMEPOS reSync time %lld to  %lld\n", timeRet, referTime);
                } else {
                    /* 本地值还没建立：这不是"重同步"，是首次赋值，不要打印环绕值。 */
                    AF_LOGD("TIMEPOS first valid reference time %lld (local clock was unset)\n",
                            (long long) referTime);
                }

                timeRet = referTime;
            }

            //AF_LOGD("TIMEPOS timeRet: %lld", timeRet);
        }

        return timeRet;
    }


    void SystemReferClock::reset()
    {
        //AF_LOGD("TIMEPOS reset reset");
        mClock.reset();
        mGetClock = nullptr;
        mClockArg = nullptr;
    }

    void SystemReferClock::SetScale(float scale)
    {
        //AF_LOGD("TIMEPOS SetScale :%f", scale);
        mClock.setSpeed(scale);
    }

    void SystemReferClock::pause()
    {
        //AF_LOGD("TIMEPOS pause");
        mClock.pause();
    }

    void SystemReferClock::start()
    {
        //AF_LOGD("TIMEPOS start");
        mClock.start();
    }

    void SystemReferClock::setTime(int64_t time)
    {
        //AF_LOGD("TIMEPOS setTime:%lld", time);
        mClock.set(time);
    }

    void SystemReferClock::setReferenceClock(get_clock getClock, void *arg)
    {
        //AF_LOGD("TIMEPOS setReferenceClock");
        mGetClock = getClock;
        mClockArg = arg;
    }

    bool SystemReferClock::haveMaster()
    {
        return mGetClock != nullptr;
    }

    bool SystemReferClock::isMasterValid()
    {
        if (mGetClock) {
            if (INT64_MIN != mGetClock(mClockArg)) {
                return true;
            }
        }

        return false;
    }

    float SystemReferClock::GetScale()
    {
        return mClock.getSpeed();
    }
};
