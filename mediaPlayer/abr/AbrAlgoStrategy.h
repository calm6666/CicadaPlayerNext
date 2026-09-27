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

    /*
     * 【同一分辨率优先更省带宽的编码 + 硬解优先】所需的补充信息。
     *
     * AddStreamInfo() 只带 index + bitrate，判不了"同分辨率"和"哪个编码更高效"，
     * 所以在它之后（同一个 mediaInfoGet 回调里）把这一路流的编码短名与分辨率喂进来。
     *
     * 刻意**不**改 AddStreamInfo 的签名：它是虚函数，动签名就是动 vtable 槽位的
     * 函数类型；这里用一个新增的非虚方法，vtable 布局完全不变。
     *
     * codecShortName 用 afCodecShortName() 的规范短名（空串 = 没有编码信息）；
     * 编码效率等级在实现里用 afCodecEfficiencyRankByShortName() 算，等级表只有那一份；
     * "这台设备能不能硬解"在实现里问 decoderFactory::isHardwareDecodeSupported()
     * （AFCodecID 由短名经 afCodecIDFromShortName() 反查，还是同一张表）。
     * 调用方**不需要**自己判断硬解能力，也不允许传自己算的结论进来。
     */
    void SetStreamExtraInfo(int streamIndex, const char *codecShortName, int width, int height);

    /*
     * 在同一分辨率的档位里，找"更该选"的那一档，返回它在 mBitRates / mLevelInfos 里的
     * 下标；没有更优的变体（或不允许替换）时返回 -1。
     *
     * 选择规则 —— **硬解能力是第一约束，压缩效率序是第二偏好**，两者都只作用于
     * "同一分辨率"内部，绝不跨分辨率：
     *   1) 候选集 = 宽高与 index 这一档**完全相同**的流。宽高不是都 > 0 时直接返回 -1：
     *      判不了"同分辨率"就不动（宁可不换，也不许猜）；
     *   2) 候选集里只要**存在**能硬解的变体，就只在能硬解的变体里挑；一个能硬解的
     *      都没有时才允许软解变体（全都解不了，那软解就是唯一选择）。这一条就是
     *      "绝不选一个硬解不了、又不比别的更优的编码"；
     *   3) 在筛出来的集合里按压缩效率取最高的那个
     *      （afCodecEfficiencyRankByShortName：AV1 > H.265 > VP9 > H.264 > MPEG-4/MPEG-2），
     *      同级取码率最低者，码率仍相同则取 streamIndex 小者（结果确定，不依赖遍历顺序）；
     *   4) maxBitrate > 0 时只考虑码率不超过它的候选。"降清晰度"那条路径用它把换编码
     *      限制在本来就负担得起的范围内（缓冲见底时换到一个更贵的编码等于把降档白做）；
     *      maxBitrate <= 0 表示不设上限，"升清晰度/稳态"那条路径用它，允许为了硬解或
     *      效率在同一分辨率内换到码率更高的那一档 —— 分辨率不变，画质不变。
     *
     * 除上面的规则外还要求"换了确实有收益"：
     *   * 候选能带来**硬解**（当前档解不了、候选档能解）—— 能力收益，不要求更省带宽，
     *     这是"同一分辨率内换到一个软解变体"唯一被允许的情形；
     *   * 否则必须是**效率收益**：候选的编码效率等级更高，**并且**码率严格更低
     *     （"压缩率更高 ⇒ 同一画质下码率更低"）。
     * 既不更能解、也不更省带宽的替换一律不做 —— 特别地，**不会**因为"另一路码率更低"
     * 就换到压缩效率更差的编码上（那是降画质，不是省带宽）。
     *
     * 返回 -1 的情形：index 非法、分辨率未知、候选集里没有更优的档、更优的档被
     * maxBitrate 挡住，或替换没有收益。
     */
    int FindSameResolutionEfficientCodec(int index, int maxBitrate) const;

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

    /*
     * 档位的扩展信息。**与 mBitRates 严格一一对应**（同长度、同顺序，
     * AddStreamInfo 里两个容器一起 push、一起排序），所以它的下标可以直接
     * 当作 mBitRates 的下标用。
     *
     * 新成员一律追加在结构体末尾（本工程既定规则）。
     */
    struct AbrLevelInfo {
        int streamIndex = -1;
        int bitrate = 0;
        int width = 0;
        int height = 0;
        int codecRank = 0;
        std::string codec;
        /*
         * 这台设备能不能**硬解**这一路的编码。默认 true = "未知，视为支持"：
         * SetStreamExtraInfo() 没被调用过（没有编码信息）、或者平台答不上来时，
         * 都不许把它当成"解不了"而影响选择。
         */
        bool hwDecodeSupported = true;
    };

    /* 新成员一律追加在类末尾（本工程既定规则）。 */
    vector<AbrLevelInfo> mLevelInfos;
};

#endif /* AbrAlgoStrategy_h */
