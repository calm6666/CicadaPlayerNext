#ifndef CICADA_PLAYER_CLOCK_H_
#define CICADA_PLAYER_CLOCK_H_

#include <cstdint>
#include <cstdlib>
#include <utils/af_clock.h>

typedef int64_t (*get_clock)(void *arg);


namespace Cicada {
    class SystemReferClock {
    public:
        SystemReferClock() = default;

        ~SystemReferClock() = default;

        void start();

        void pause();

        void setTime(int64_t time);

        int64_t GetTime();

        void SetScale(float scale);

        float GetScale();

        void setReferenceClock(get_clock getClock, void *arg);

        void reset();

        bool haveMaster();

        bool isMasterValid();

        /*
         * 【状态查询】主时钟当前是否处于"暂停"（不前进）状态。
         *
         * 用途：seek 窗口内"先按住 PCM、等主时钟追上来再推"这道门**前提是时钟在走**；
         * 而缓冲态会把时钟 pause 掉（doReadPacket 的缓冲分支 mMasterClock.pause()），
         * 那时"追上"永远不会发生 —— 真机实测就是 PCM 被按住 8.4 秒（日志
         * `audio silence starts (reason=1 …) holding pcm until the clock catches up`
         * 之后一直不恢复，直到设备侧 keep-alive 用尽）。
         * 有了这个查询，调用方就能用"时钟到底在不在走"这个**事实**做判据，而不是靠
         * 猜某个播放状态。纯查询、无副作用。
         */
        bool isPaused()
        {
            return mClock.isPaused();
        }

    private:
        af_scalable_clock mClock;
        get_clock mGetClock{nullptr};
        void *mClockArg{nullptr};
    };

};// namespace Cicada


#endif // CICADA_PLAYER_CLOCK_H_
