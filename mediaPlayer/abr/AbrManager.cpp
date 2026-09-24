//
//  AbrManager.cpp
//  apsara_player
//
//  Created by shiping.csp on 2018/11/1.
//

#define LOG_TAG "AbrManager"

#include "AbrManager.h"
#include "AbrAlgoStrategy.h"
#include "utils/afThread.h"
#include "utils/timer.h"

AbrManager::AbrManager()
{
    mPMainThread = NEW_AF_THREAD(AbrAdjustFun);
    mMsgProcessTime = 1000;
}

AbrManager::~AbrManager()
{
    Stop();
    delete mPMainThread;
}

void AbrManager::Reset()
{
    std::unique_lock<std::mutex> uMutex(mMutex);

    if (mAlgoStrategy) {
        mAlgoStrategy->Reset();
    }
}

void AbrManager::Start()
{
    {
        std::unique_lock<std::mutex> uMutex(mMutex);
        mRunning = true;
    }
    if (mAlgoStrategy && mAlgoStrategy->GetBitRateCount() > 1) {
        mPMainThread->start();
    }
}

void AbrManager::Pause()
{
    {
        std::unique_lock<std::mutex> uMutex(mMutex);
        mRunning = false;
    }
    mCondition.notify_one();
    mPMainThread->pause();
}

void AbrManager::Stop()
{
    {
        std::unique_lock<std::mutex> uMutex(mMutex);
        mRunning = false;
    }
    mCondition.notify_one();
    mPMainThread->stop();
}

void AbrManager::EnableAbr(bool bEnabled)
{
    /*
     * 从"关"变"开"时通知策略清一次状态：用户手动选了某一档、再切回"自动"时，
     * 策略里可能还留着上一次 ABR 请求的在途标记/上切禁令，不清掉的话切回自动
     * 之后还要白等一个看门狗周期才会重新评估。
     */
    if (bEnabled && !mEnableAbr && mAlgoStrategy != nullptr) {
        mAlgoStrategy->OnAbrEnabled();
    }

    mEnableAbr = bEnabled;
}

void AbrManager::SetAbrAlgoStrategy(AbrAlgoStrategy *abrAlgo)
{
    mAlgoStrategy = abrAlgo;
}

int AbrManager::AbrAdjustFun()
{
    std::unique_lock<std::mutex> uMutex(mMutex);
    mCondition.wait_for(uMutex, std::chrono::milliseconds(mMsgProcessTime), [this]() { return !mRunning; });

    if (mAlgoStrategy && mEnableAbr && mRunning) {
        mAlgoStrategy->ProcessAbrAlgo();
    }

    return 0;
}

void AbrManager::GetOption(const std::string &key, std::string &value)
{
    if (mAlgoStrategy) {
        mAlgoStrategy->GetOption(key, value);
    }
}
