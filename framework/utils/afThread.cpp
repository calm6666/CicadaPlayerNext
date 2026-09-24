#include <pthread.h>
#include <utility>

//
// Created by moqi on 2018/9/3.
//

#define LOG_TAG "afThread"
#include "afThread.h"
#include "frame_work_log.h"
#include "timer.h"
#include <cassert>

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
}

int afThread::start()
{
    std::lock_guard<std::mutex> guard(mMutex);
    mTryPaused = false;

    if (nullptr == mThreadPtr) {
        mThreadStatus = THREAD_STATUS_RUNNING;
        mThreadPtr = new std::thread(threadRun, this);
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
    AF_LOGD("%s:%d(%s) %s \n", __FILE__, __LINE__, __func__, mName.c_str());
}

void afThread::forceStop()
{
    if (mThreadPtr) {
        mThreadPtr->detach();
        delete mThreadPtr;
        mThreadPtr = nullptr;
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
