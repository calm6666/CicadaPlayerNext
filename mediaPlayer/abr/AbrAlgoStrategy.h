//
//  AbrAlgoStrategy.h
//  apsara_player
//
//  Created by shiping.csp on 2018/11/1.
//

#ifndef AbrAlgoStrategy_h
#define AbrAlgoStrategy_h

#include <cstdint>
#include <cstdio>
#include <string>
#include <functional>
#include <map>
#include <vector>

using namespace std;

class AbrRefererData;

class AbrAlgoStrategy {
public:
    explicit AbrAlgoStrategy(std::function<void(int)> func);

    virtual ~AbrAlgoStrategy();

public:
    //set referer data source
    virtual void SetRefererData(AbrRefererData *refererData);

    //add stream index and bitrate info
    virtual void AddStreamInfo(int streamIndex, int bitrate);

    //set current stream
    virtual void SetCurrentBitrate(int bitrate);

    enum Status {
        Switch,
        Lowest_Already,
        Highest_Already,
    };

    virtual void SetSwitchStatusCallback(const std::function<void(Status)> &statusCallback)
    {
        mStatusCallback = statusCallback;
    }

    virtual void Clear();
    virtual void SetDuration(int64_t ms) {mDurationMS = ms;}

    /*
     * ABR 被重新打开时回调（用户从手动档切回"自动"）。
     * 默认什么都不做；策略可以用它清掉"上次在途切换"的残留状态/限流计时，
     * 保证"切到自动就立刻是自动的"，而不是先白等一个看门狗周期。
     */
    virtual void OnAbrEnabled()
    {}

    //reset abr algo
    virtual void Reset() = 0;

    virtual void ProcessAbrAlgo() = 0;

    virtual void GetOption(const std::string &key, std::string &value)
    {}

    virtual uint32_t GetBitRateCount()
    {
        return mBitRates.size();
    }

protected:
    AbrRefererData *mRefererData = nullptr;
    map<int, int> mStreamIndexBitrateMap;
    vector<int> mBitRates;
    int mCurrentBitrate = 0;
    int mPreBitrate = 0;
    int64_t mDurationMS = -1;
    std::function<void(int)> mFunc;
    std::function<void(Status)> mStatusCallback;
    void *mUserData = nullptr;
};

#endif /* AbrAlgoStrategy_h */
