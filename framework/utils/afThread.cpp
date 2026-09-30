#include <pthread.h>
#include <utility>
#include <atomic>

//
// Created by moqi on 2018/9/3.
//

#define LOG_TAG "afThread"
#include "afThread.h"
#include "frame_work_log.h"
#include "timer.h"
#include <cassert>

/*
 * 【释放探针】afThread 对象的生命周期计数（只按**构造/析构**对称计，不按"启停"计）。
 *
 * 为什么必须是构造/析构：上一版把 +1 放在 start()、-1 放在 stop()/forceStop()/析构，
 * 结果 stop() 只是"把线程停下来"、对象还活着，于是**递减次数远多于递增**，
 * live 一路跌成负数（实测跌到 -50），读数完全无意义 —— 这是一次教训：
 * **计数必须绑在对象的生命周期上，不能绑在"状态的迁移"上。**
 *
 * 语义：
 *   * g_createdAfThreads —— 累计构造了几个 afThread 对象；
 *   * g_destroyedAfThreads —— 累计析构了几个；
 *   * g_liveAfThreads —— 当前存活对象数，恒等于 created - destroyed，**永远不会为负**。
 * 自检（跑完一轮看最后一行即可）：`created - destroyed == live`，且 live >= 0。
 */
static std::atomic<int> g_createdAfThreads{0};
static std::atomic<int> g_destroyedAfThreads{0};
static std::atomic<int> g_liveAfThreads{0};

#ifdef ANDROID

    #include <sys/prctl.h>
    #include <cassert>


    #define MAX_TASK_NAME_LEN (16)
#endif

#ifdef ANDROID

static void get_name(char *name)
{
    if (prctl(PR_GET_NAME, (unsigned long) name, 0, 0, 0) != 0) {
        strcpy(name, "<name unknown>");
    } else {
        // short names are null terminated by prctl, but the man page
        // implies that 16 byte names are not.
        name[MAX_TASK_NAME_LEN] = 0;
    }

    AF_LOGD("thread_name is %s\n", name);
}

static void set_name(char *name)
{
    char threadName[MAX_TASK_NAME_LEN + 1];
    size_t size = strlen(name) > MAX_TASK_NAME_LEN ? MAX_TASK_NAME_LEN : strlen(name);
    strncpy(threadName, name, size);
    threadName[size] = 0;
    prctl(PR_SET_NAME, (unsigned long) threadName, 0, 0, 0);
}

#endif

static void thread_set_self_name(char *name)
{
#if defined __APPLE__
    pthread_setname_np(name);
#elif defined(ANDROID)
    set_name(name);
#endif
}
afThread::afThread(std::function<int()> func, const char *name)
    : mFunc(std::move(func)),
      mName(name)
{
    /*
     * 【释放探针·创建点】计数绑在**构造**上（见文件头那三条计数器的说明）。
     * 带 this 指针是为了配对：同名线程（avFormatDemuxer / HLSStream / DashStream 会有多个）
     * 只看名字没法区分，必须靠指针。
     */
    const int created = g_createdAfThreads.fetch_add(1) + 1;
    const int live = g_liveAfThreads.fetch_add(1) + 1;
    AF_LOGI("[mem] afThread '%s' @%p created (live=%d, created=%d)\n", mName.c_str(),
            static_cast<void *>(this), live, created);
}

int afThread::start()
{
    std::lock_guard<std::mutex> guard(mMutex);
    mTryPaused = false;

    if (nullptr == mThreadPtr) {
        mThreadStatus = THREAD_STATUS_RUNNING;
        mThreadPtr = new std::thread(threadRun, this);

        /* 【释放探针】只记"线程真的起来了"，**不改任何计数**（计数只归构造/析构管）。 */
        AF_LOGI("[mem] afThread '%s' @%p thread started\n", mName.c_str(),
                static_cast<void *>(this));
    } else {
        std::unique_lock<std::mutex> sleepMutex(mSleepMutex);
        mThreadStatus = THREAD_STATUS_RUNNING;
        mSleepCondition.notify_one();
    }

    return 0;
}

void afThread::threadRun(void *arg)
{
    auto *pThread = static_cast<afThread *>(arg);
    pThread->onRun();
}

void afThread::onRun()
{
    if (mThreadBeginCallback != nullptr) {
        mThreadBeginCallback();
    }

    assert(mFunc != nullptr);
    int ret;

    if (mName.length() > 0) {
        thread_set_self_name(const_cast<char *>(mName.c_str()));
    }

    while (THREAD_STATUS_STOPPED < mThreadStatus) {
        if (mWaitPaused) {
            mThreadStatus = THREAD_STATUS_PAUSED;
            std::unique_lock<std::mutex> sleepMutex(mSleepMutex);
            mWaitPaused = false;
            mSleepCondition.notify_one();
        }

        if (THREAD_STATUS_PAUSED == mThreadStatus) {
            std::unique_lock<std::mutex> sleepMutex(mSleepMutex);
            // make sure notify on time, reduce loop cpu usage.
            mSleepCondition.wait(sleepMutex, [this]() {
                return THREAD_STATUS_PAUSED != mThreadStatus;
            });
        } else if (THREAD_STATUS_RUNNING == mThreadStatus) {
            ret = mFunc();

            if (ret < 0) {
                mTryPaused = true;
            }
        }

        if (mTryPaused) {
            if (mMutex.try_lock()) {
                mThreadStatus = THREAD_STATUS_PAUSED;
                mMutex.unlock();
            }

            mTryPaused = false;
        }
    }

    if (mThreadEndCallback != nullptr) {
        mThreadEndCallback();
    }
}

void afThread::prePause()
{
    if (mMutex.try_lock()) {
        if (THREAD_STATUS_RUNNING == mThreadStatus) {
            mTryPaused = true;
        }

        mMutex.unlock();
    }
}

void afThread::pause()
{
    std::lock_guard<std::mutex> guard(mMutex);

    if (THREAD_STATUS_RUNNING == mThreadStatus) {
        std::unique_lock<std::mutex> sleepMutex(mSleepMutex);
        mWaitPaused = true;
        /*
         * 【计时日志：静默卡死的第二种形态】
         * pause() 是"等读线程走到安全点"，而读线程可能正卡在阻塞 IO 上 ——
         * 这期间两边都不打日志，用户看到的就是"卡住且没有多余的日志"。
         * 只把等待时长打出来（>200ms 才打），下次日志就能点名。
         */
        const int64_t waitBeginMs = af_getsteady_ms();
        mSleepCondition.wait(sleepMutex, [this]() {
            return !mWaitPaused;
        });
        const int64_t waitedMs = af_getsteady_ms() - waitBeginMs;

        if (waitedMs > 200) {
            AF_LOGW("%s: pause() waited %lld ms for the thread to reach a safe point\n",
                    mName.c_str(), (long long) waitedMs);
        }
    }
}

void afThread::stop()
{
    AF_LOGD("%s:%d(%s) %s \n", __FILE__, __LINE__, __func__, mName.c_str());
    std::lock_guard<std::mutex> guard(mMutex);
    mTryPaused = false;
    {
        std::unique_lock<std::mutex> sleepMutex(mSleepMutex);
        mThreadStatus = THREAD_STATUS_STOPPED;
    }
    mSleepCondition.notify_one();

    if (mThreadPtr && mThreadPtr->joinable()) {
        if (mThreadPtr->get_id() != std::this_thread::get_id()) {
            /*
             * 【★ 这里是最典型的"静默卡死"点 ★】
             *
             * 用户实测（2026-09-20）："有时候卡住没有多余的日志"。
             * 这个 join() **没有任何超时**：读线程要是正卡在阻塞 socket 读上
             * （curl 的 low_speed_time = 15000ms，见日志里 CURLConnection 那几行），
             * 调用方（切档时是消息/关闭路径）就在这里干等，最长能等满 15 秒 ——
             * 期间两条线程都不产生日志，表现就是"卡住 + 没有日志 + 过一会儿又好了"。
             *
             * 这里不做危险的"超时就 detach"（线程还活着的话会碰已经释放的对象），
             * 只**测量并打出来**：>200ms 就一行 warn，带线程名和毫秒数。
             * 下次再卡，日志里会有
             *     afThread HLSStream: stop() waited 14980 ms for the thread to exit (blocked IO?)
             * —— 是谁、卡了多久，一眼就能看到。
             */
            const int64_t waitBeginMs = af_getsteady_ms();
            mThreadPtr->join();
            const int64_t waitedMs = af_getsteady_ms() - waitBeginMs;

            if (waitedMs > 200) {
                AF_LOGW("%s: stop() waited %lld ms for the thread to exit (blocked IO?)\n",
                        mName.c_str(), (long long) waitedMs);
            }
        } else {
            mThreadPtr->detach();
        }
    }

    delete mThreadPtr;
    mThreadPtr = nullptr;

    /* 【释放探针】只记"线程被停了"，**不改计数**：对象这时还活着（计数归析构管）。 */
    AF_LOGI("[mem] afThread '%s' @%p thread stopped (object still alive)\n", mName.c_str(),
            static_cast<void *>(this));
    AF_LOGD("%s:%d(%s) %s \n", __FILE__, __LINE__, __func__, mName.c_str());
}

void afThread::forceStop()
{
    if (mThreadPtr) {
        mThreadPtr->detach();
        delete mThreadPtr;
        mThreadPtr = nullptr;

        /* 【释放探针】同 stop()：只记事件，**不改计数**（对象还活着）。 */
        AF_LOGI("[mem] afThread '%s' @%p thread force-stopped (object still alive)\n",
                mName.c_str(), static_cast<void *>(this));
    }
}

void afThread::detach()
{
    std::unique_lock<std::mutex> uMutex(mMutex);
    if (mThreadPtr && mThreadPtr->joinable()) {
        mThreadPtr->detach();
    }
}

afThread::~afThread()
{
    /* 【释放探针】记下"析构时是否还握着一个线程对象"，只用于下面那行日志判读。 */
    const bool hadThread = (mThreadPtr != nullptr);

    if (mThreadPtr) {
        std::lock_guard<std::mutex> guard(mMutex);
        mTryPaused = false;
        {
            std::unique_lock<std::mutex> sleepMutex(mSleepMutex);
            mThreadStatus = THREAD_STATUS_IDLE;
        }
        mSleepCondition.notify_one();

        if (mThreadPtr && mThreadPtr->joinable()) {
            mThreadPtr->join();
        }

        delete mThreadPtr;
        mThreadPtr = nullptr;

    }

    /*
     * 【释放探针·销毁点】live **只在这里**递减，且**无条件**（对象没起过线程也要减），
     * 因为计数语义是"存活对象数"、不是"存活线程数"。
     * 带指针便于与创建行配对；hadThread=1 = 析构时还握着线程对象
     * （它的句柄刚在上面 join/detach 那几句里关掉）。
     * 自检：created - destroyed == live，且 live >= 0（永不为负）。
     */
    const int live = g_liveAfThreads.fetch_sub(1) - 1;
    const int destroyed = g_destroyedAfThreads.fetch_add(1) + 1;
    AF_LOGI("[mem] afThread '%s' @%p destroyed (live=%d, destroyed=%d, hadThread=%d)\n",
            mName.c_str(), static_cast<void *>(this), live, destroyed, hadThread ? 1 : 0);
}

void afThread::setBeginCallback(const thread_beginCallback &callback)
{
    mThreadBeginCallback = callback;
}

void afThread::setEndCallback(const thread_endCallback &callback)
{
    mThreadEndCallback = callback;
}

std::thread::id afThread::getId()
{
    if (mThreadPtr) {
        return mThreadPtr->get_id();
    }
    return {};
}
