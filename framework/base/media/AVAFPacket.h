//
// Created by moqi on 2019-07-05.
//

#ifndef FRAMEWORK_AVPACKET_H
#define FRAMEWORK_AVPACKET_H

#include "base/media/IAFPacket.h"
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/encryption_info.h>
}

#ifdef __APPLE__
class PBAFFrame;
#endif

/*
 * 【存活计数探针·2026-09-30】框架里帧/包包装类的存活数（created - destroyed）。
 *
 * 为什么需要：D3D11VA 表面池已经被证明会随解码器释放（见
 * docs/ANALYSIS-D3D11VA-POOL-RELEASE.md 的探针修正），关窗后内存仍然每轮上涨这件事
 * 就必须逐类归因。AVAFFrame / AVAFPacket 是框架里**唯一**的帧/包包装 ——
 * 硬解帧的 buf[] 里握着解码器的输出缓冲与整池表面，所以"它们的存活数会不会随播放时长
 * 增长"是最直接的判据：
 *   · 增长 ⇒ 某一条路径把帧/包留住了（顺着日志找是谁留的）；
 *   · 不增长 ⇒ 帧/包这一侧是干净的，那几十 MB 在别处（呈现方 / Qt / QML / 驱动）。
 *
 * 只读计数：打印点在 avFormatDemuxer 的 open 成功与 Close（分片边界，天然的时间轴）。
 */
long long afLiveAvafFrames();

long long afLiveAvafPackets();

class AVAFPacket : public IAFPacket {
public:
    attribute_deprecated explicit AVAFPacket(AVPacket &pkt, bool isProtected = false);

    explicit AVAFPacket(AVPacket *pkt, bool isProtected = false);

    explicit AVAFPacket(AVPacket **pkt, bool isProtected = false);

    AVAFPacket(const AVAFPacket &pkt);

    ~AVAFPacket() override;

    void setDiscard(bool discard) override;

    uint8_t *getData() override;

    bool isProtected() override
    {
        return mIsProtected;
    }

    void setProtected() override
    {
        mIsProtected = true;
    }

    std::unique_ptr<IAFPacket> clone() const override;

    int64_t getSize() override;

    AVPacket *ToAVPacket();

    explicit operator AVPacket *();

    void setMagicKey(const std::string &key) override
    {
        if (mMagicKey.empty()) {
            mMagicKey = key;
        }
    }

    std::string getMagicKey() override
    {
        return mMagicKey;
    }

    bool getEncryptionInfo(EncryptionInfo* dst) override;

private:
    AVPacket *mpkt{nullptr};
    bool mIsProtected;
    std::string mMagicKey{};
    AVEncryptionInfo *mAVEncryptionInfo{nullptr};

    void copyInfo();
};


class CICADA_CPLUS_EXTERN AVAFFrame : public IAFFrame {
public:

    explicit AVAFFrame(const AFFrameInfo &info, const uint8_t **data, const int *lineSize, int lineNums,
                       IAFFrame::FrameType type = FrameTypeUnknown);

    explicit AVAFFrame(AVFrame *frame, FrameType type = FrameTypeUnknown);

    explicit AVAFFrame(AVFrame **frame, FrameType type = FrameTypeUnknown);

    ~AVAFFrame() override;

    std::unique_ptr<IAFFrame> clone() override;

    uint8_t **getData() override;

    int *getLineSize() override;

    FrameType getType() override;

    AVFrame *ToAVFrame();

    explicit operator AVFrame *() const;

    void updateInfo();


private:
    AVFrame *mAvFrame;
    FrameType mType = FrameTypeUnknown;

    void copyInfo();
};

static inline AVFrame *getAVFrame(IAFFrame *frame)
{
    auto *avafFrame = dynamic_cast<AVAFFrame *>(frame);
    if (avafFrame) {
        return static_cast<AVFrame *>(*(avafFrame));
    }
    return nullptr;
}

static inline AVPacket *getAVPacket(IAFPacket *packet)
{
    auto *avafPacket = dynamic_cast<AVAFPacket *>(packet);
    if (avafPacket) {
        return static_cast<AVPacket *>(*(avafPacket));
    }
    return nullptr;
}


#endif//FRAMEWORK_AVPACKET_H
