//
// Created by yuyuan on 2021/03/17.
//

#include "DashManager.h"
#include "DashSegmentTracker.h"
#include "DashStream.h"
#include "demuxer/play_list/AdaptationSet.h"
#include "demuxer/play_list/Period.h"
#include "demuxer/play_list/Representation.h"
#include "demuxer/play_list/playList_demuxer.h"
#include "utils/errors/framework_error.h"
#include "utils/frame_work_log.h"
#include "utils/mediaFrame.h"
#include <cassert>
#include <cerrno>
#include <utils/timer.h>

#undef LOG_TAG
#define LOG_TAG "DashManager"

using namespace Cicada;

DashManager::DashManager(playList *pList) : PlaylistManager(pList)
{}

DashManager::~DashManager()
{
    for (auto i = mStreamInfoList.begin(); i != mStreamInfoList.end(); i++) {
        AF_TRACE;
        (*i)->mPFrame = nullptr;
        (*i)->mPStream->close();
        delete (*i)->mPStream;
        delete (*i);
    }

    mStreamInfoList.clear();
}

int DashManager::init()
{
    int ret;
    std::list<Period *> &periodList = mPList->GetPeriods();
    int id = 0;
    int videoStreamCount = 0;
    uint64_t lowestBandwidth = std::numeric_limits<uint64_t>::max();

    for (auto &pit : periodList) {
        std::list<AdaptationSet *> adaptSetList = FindSuitableAdaptationSets(pit);

        for (auto &ait : adaptSetList) {
            auto representList = ait->getRepresentations();

            for (auto &rit : representList) {
                rit->mPlayListType = playList_demuxer::playList_type_dash;
                auto *pTracker = new DashSegmentTracker(ait, rit, mSourceConfig);
                if (pTracker->getStreamType() == STREAM_TYPE_VIDEO) {
                    videoStreamCount++;
                    uint64_t bandwidth = 0;
                    std::string lang;
                    pTracker->getStreamInfo(nullptr, nullptr, &bandwidth, lang);
                    if (lowestBandwidth > bandwidth) {
                        lowestBandwidth = bandwidth;
                        mLowestBandwidthVideoId = id;
                    }
                }
                pTracker->setOptions(mOpts);
                auto *info = new DashStreamInfo();
                info->mPStream = new DashStream(pTracker, id++);
                info->mPStream->setOptions(mOpts);
                info->mPStream->setDataSourceConfig(mSourceConfig);
                info->mPStream->setBitStreamFormat(mMergeVideoHeader, mMergerAudioHeader);
                info->mPStream->setUrlToUniqueIdCallback(mUrlHashCb, mUrlHashCbUserData);
                mStreamInfoList.push_back(info);
            }
        }
    }

    bool bEnableCache = videoStreamCount > 1 ? false : true;
    if (mPList->isLive()) {
        bEnableCache = false;
    }
    for (auto &i : mStreamInfoList) {
        i->mPStream->enableCache(bEnableCache);
    }

    /* 冷路径一行：清单里的码率档数（清晰度菜单就是从这里出来的） */
    AF_LOGI("[dash] 清单里有 %d 条视频档、共 %d 条流（最低档 id=%d）\n",
            videoStreamCount, (int) mStreamInfoList.size(), mLowestBandwidthVideoId);

    if (mStreamInfoList.size() == 1) {
        ret = (*mStreamInfoList.begin())->mPStream->open();
        if (ret >= 0) {
            mMuxedStream = (*mStreamInfoList.begin())->mPStream;
            mMuxedStream->setExtDataSource(mExtDataSource);
        }
    }

    return 0;
}

void DashManager::preStop()
{
    for (auto &i : mStreamInfoList) {
        if (i->mPStream->isOpened()) {
            i->mPStream->preStop();
        }
    }
}

void DashManager::stop()
{
    for (auto &i : mStreamInfoList) {
        if (i->mPStream->isOpened()) {
            i->mPStream->stop();
        }
    }

    mStarted = false;
}

int DashManager::GetNbStreams() const
{
    if (mMuxedStream) {
        return mMuxedStream->GetNbStreams();
    } else {
        return static_cast<int>(mStreamInfoList.size());
    }
}

int DashManager::GetStreamMeta(Stream_meta *meta, int index, bool sub) const
{
    memset(meta, 0, sizeof(Stream_meta));
    meta->type = STREAM_TYPE_UNKNOWN;
    int subIndex = GEN_SUB_STREAM_ID(index);
    int streamIndex = GEN_STREAM_INDEX(index);

    if (mMuxedStream) {
        if (mMuxedStream->getId() == streamIndex) {
            meta->type = STREAM_TYPE_MIXED;
            return mMuxedStream->GetStreamMeta(meta, subIndex, sub);
        } else {
            return -1;
        }
    }

    int i = 0;
    DashStream *stream = nullptr;

    for (const auto &item : mStreamInfoList) {
        if (i == streamIndex) {
            stream = item->mPStream;
            break;
        }

        i++;
    }

    if (stream) {
        stream->GetStreamMeta(meta, subIndex, sub);

        if (meta->type == STREAM_TYPE_UNKNOWN) {
            meta->type = static_cast<Stream_type>(stream->getStreamType());
        }

        return 0;
    }

    AF_LOGE("no such stream %d\n", index);
    return -1;
}

static const char *getStreamTypeName(DashStream *pStream)
{
    char *name = const_cast<char *>("unknown");

    switch (pStream->getStreamType()) {
        case STREAM_TYPE_VIDEO:
            name = const_cast<char *>("Video");
            break;

        case STREAM_TYPE_AUDIO:
            name = const_cast<char *>("Audio");
            break;

        case STREAM_TYPE_SUB:
            name = const_cast<char *>("Subtitle");
            break;

        default:
            break;
    }

    return name;
}

int DashManager::ReadPacket(unique_ptr<IAFPacket> &packet, int index)
{
    IAFPacket *pFrameOut{};

    if (mMuxedStream) {//mediaPlayList
        int ret = mMuxedStream->read(packet);

        if (packet != nullptr) {
            packet->getInfo().streamIndex = GEN_STREAM_ID(mMuxedStream->getId(), packet->getInfo().streamIndex);
        }

        return ret;
    }

    int ret;

    for (auto &i : mStreamInfoList) {
        if (i->mPStream->isOpened() && i->selected && i->mPFrame == nullptr && !i->eos) {
            ret = i->mPStream->read(i->mPFrame);
            if (ret > 0) {
                i->mPFrame->getInfo().streamIndex = GEN_STREAM_ID(i->mPStream->getId(), i->mPFrame->getInfo().streamIndex);
            } else if (ret == 0) {
                AF_LOGD("EOF %d\n", i->mPStream->getId());

                if (i->stopOnSegEnd) {
                    i->mPStream->stop();
                    i->selected = false;

                    for (auto &j : mStreamInfoList) {
                        if (j->mPStream->getId() == i->toStreamId) {
                            j->selected = true;
                            j->stopOnSegEnd = false;
                            j->toStreamId = -1;

                            if (i->mPStream->isLive()) {
                                uint64_t targetPosition = i->mPStream->getCurSegPosition() + 1;
                                AF_LOGE("set SegPosition to %llu\n", targetPosition);
                                j->mPStream->setCurSegPosition(targetPosition);
                            } else {
                                AF_LOGE("set SegNum to %llu\n", i->mPStream->getCurSegNum() + 1);
                                j->mPStream->SetCurSegNum(i->mPStream->getCurSegNum() + 1);
                            }

                            break;
                        }
                    }

                    i->stopOnSegEnd = false;
                    i->mPStream->stopOnSegEnd(false);
                    OpenStream(i->toStreamId);
                    AF_LOGD("change stream %d -> %d", i->mPStream->getId(), i->toStreamId);
                    i->toStreamId = -1;
                    return -EAGAIN;
                } else {
                    i->eos = true;
                }
            } else {
                if (ret == FRAMEWORK_ERR_FORMAT_NOT_SUPPORT) {
                    AF_LOGE("read error %s\n", framework_err2_string(ret));
                    i->eos = true;
                    i->mPStream->stop();
                } else if (ret != -EAGAIN) {
                    AF_LOGE("read error %d\n", ret);
                }

                return ret;
            }
        }

        if (i->mPFrame) {
            if (pFrameOut == nullptr) {
                pFrameOut = i->mPFrame.get();
            } else if (i->mPFrame->getInfo().dts < pFrameOut->getInfo().dts) {
                pFrameOut = i->mPFrame.get();
            }
        }
    }

    if (index == -1 && mPList->isLive() && mPreferAudioEnabled && mBufferLevel == client_buffer_level_low) {
        bool lowestVideo = false;
        for (auto &i : mStreamInfoList) {
            if (i->mPStream->isOpened() && i->selected && i->mPStream->getStreamType() == STREAM_TYPE_AUDIO && i->mPFrame) {
                index = i->mPFrame->getInfo().streamIndex;
                break;
            } else if (i->mPStream->isOpened() && i->selected && i->mPStream->getStreamType() == STREAM_TYPE_VIDEO) {
                lowestVideo = (i->mPStream->getId() == mLowestBandwidthVideoId);
            }
        }
        if (!lowestVideo) {
            index = -1;
        }
    }

    if (index != -1) {
        pFrameOut = nullptr;

        for (auto &i : mStreamInfoList) {
            if (i->mPStream->isOpened() && i->selected && i->mPFrame != nullptr && !i->eos) {
                if (i->mPFrame->getInfo().streamIndex == index) {
                    pFrameOut = i->mPFrame.get();
                    packet = std::move(i->mPFrame);
                    break;
                }
            }
        }
    } else {
        for (auto &i : mStreamInfoList) {
            if (pFrameOut == i->mPFrame.get()) {
                packet = std::move(i->mPFrame);
                break;
            }
        }
    }

    if (pFrameOut == nullptr || pFrameOut->getSize() == 0) {
        AF_LOGD("EOS");
        return 0;
    }

    return packet->getSize();
}

int DashManager::OpenStream(int index)
{
    int ret = 0;
    AF_LOGD("OpenStream %d\n", index);

    // select stream
    if (mStreamInfoList.size() == 1) {
        // mediaPlayList
        if (!(*mStreamInfoList.begin())->mPStream->isOpened()) {
            ret = (*mStreamInfoList.begin())->mPStream->open();

            if (ret >= 0) {
                mMuxedStream = (*mStreamInfoList.begin())->mPStream;
            }

            mMuxedStream->start();
        }
    } else {
        for (auto &i : mStreamInfoList) {
            if (i->mPStream->getId() == index) {
                if (!i->mPStream->isOpened()) {
                    if (mFirstSeekPos != INT64_MIN) {
                        i->mPStream->seek(mFirstSeekPos, 0);
                    }

                    ret = i->mPStream->open();
                }
                i->selected = true;
                i->mPStream->start();
                if (index == mLowestBandwidthVideoId) {
                    i->mPStream->setPreferAudio(mBufferLevel == client_buffer_level_low);
                }
                if (i->mPStream->getStreamType() == STREAM_TYPE_AUDIO) {
                    mOpenAudioStreamCount++;
                }
                break;
            }
        }
    }

    return ret;
}

const std::string DashManager::GetProperty(int index, const string &key)
{
    for (auto &i : mStreamInfoList) {
        if (i->mPStream->getId() == index) {
            return i->mPStream->GetProperty(key);
        }
    }

    return "";
}

int DashManager::GetRemainSegmentCount(int index)
{
    for (auto &i : mStreamInfoList) {
        if (i->mPStream->getId() == index) {
            return i->mPStream->GetRemainSegmentCount();
        }
    }

    return -1;
}

void DashManager::CloseStream(int id)
{
    AF_LOGD("CloseStream %d\n", id);

    if (mMuxedStream) {
        return;
    }

    for (auto &i : mStreamInfoList) {
        if (i->mPStream->getId() == id) {
            if (!i->selected) {
                AF_LOGW("CloseStream not opened\n");
            }

            i->selected = false;
            i->mPStream->stop();
            i->mPFrame = nullptr;
            if (i->mPStream->getStreamType() == STREAM_TYPE_AUDIO) {
                mOpenAudioStreamCount--;
            }
            break;
        }
    }
}

int DashManager::start()
{
    if (mMuxedStream) {
        if (mMuxedStream->isOpened()) {
            return mMuxedStream->start();
        }
    }

    for (auto &i : mStreamInfoList) {
        if (i->mPStream->isOpened() && i->selected) {
            i->mPStream->start();
        }
    }

    mStarted = true;
    return 0;
}

int64_t DashManager::seek(int64_t us, int flags, int index)
{
    bool hasOpened = false;

    for (auto &i : mStreamInfoList) {
        if (i->selected) {
            hasOpened = true;
            break;
        }
    }

    if (hasOpened) {
        mFirstSeekPos = INT64_MIN;
    } else {
        mFirstSeekPos = us;
    }

    if (mMuxedStream) {
        return mMuxedStream->seek(us, flags);
    }

    int type = STREAM_TYPE_UNKNOWN;

    if (index != -1) {
        for (auto &i : mStreamInfoList) {
            if (i->mPStream->getId() == index) {
                type = i->mPStream->getStreamType();

                if (type == STREAM_TYPE_SUB) {
                    return i->mPStream->seek(us, flags);
                }

                break;
            }
        }
    }

    // seek all the stream
    if (index == -1) {
        // 1. finish all Aligned switch
        for (auto &i : mStreamInfoList) {
            if (i->stopOnSegEnd) {
                CloseStream(i->mPStream->getId());

                // TODO: open at seek finish
                if (i->toStreamId >= 0) {
                    OpenStream(i->toStreamId);
                }

                i->toStreamId = -1;
                i->stopOnSegEnd = false;
            }
        }

        // 2. seek video first ,get the seekedUs
        type = STREAM_TYPE_VIDEO;
        /*
         * 【P0-B：其余每一路的 seek 目标必须是"用户请求值"，不能是视频那一侧的落点】
         *
         * 本函数原来在第 3 步给其余每一路喂的是 `us`，而 `us` 在这里被改写成**视频 seek()
         * 的返回值**。DASH 的返回值是"视频分片起点"（DashStream.cpp 的 landingUs），HLS 的
         * 返回值同样是"自己那一片的分片起点"（SegmentList::getSegmentNumberByTime 会把入参
         * time 就地改写成该片的 startTime）。于是音频的**读位置**由另一路（视频）的分片网格
         * 决定：请求落在视频分片尾部时，音频被拽到比请求值早将近一整个视频分片的位置
         * （实测 output.mpd：视频片 19.9866 s、音频片 9.984 s；日志里视频与音频的 reqUs 差
         * -369 / -760 / -1932 / -2997 / -5176 ms，方向恒为音频更早）。
         *
         * 为什么这样是错的：每一路的分片网格是各自的，"包含请求值的那一片"才是它自己的正确
         * 读起点。用视频的分片起点当音频的目标，等于要求音频按**别的流**的网格对齐，音频
         * 自己的分片起点于是可能再往前落一整片 —— 这段前缀必须被解码后丢掉，既拉长 seek
         * 的收敛时间，也让"音频与视频拿到同一个目标点"这条前提失效。
         *
         * 为什么这样改不会让落点精度变差：落点精度由播放器侧的单一判据负责
         * （SuperMediaPlayer 的 shouldDropForDiscontinuity 与音频落点地板都与 targetUs 比较，
         * 与"落点"无关），解复用层只决定"从哪里开始读、开始解"。这里把**同一个请求值**交给
         * 每一路，每一路各自落到自己那一片的片首，播放器再按同一个 targetUs 把各自的前缀裁掉。
         *
         * 视频侧行为一字不变：第 2 步仍然用请求值 seek 视频、仍然取回它的返回值，返回值照旧
         * 只用于诊断日志与"视频 seek 失败 ⇒ 整次 seek 失败"这条既有终态（失败时负值继续留在
         * us 里传给第 3 步，与改动前逐字相同）；视频的落点对齐与落点延迟线都在
         * DashStream::seek() 内部，本函数既不参与也不影响它们。
         */
        const int64_t requestedUs = us;

        for (auto &i : mStreamInfoList) {
            if (i->selected) {
                if (i->mPStream->getStreamType() == STREAM_TYPE_VIDEO || i->mPStream->getStreamType() == STREAM_TYPE_MIXED) {
                    type = i->mPStream->getStreamType();
                    int64_t seekedUs = i->mPStream->seek(us, flags);
                    AF_LOGD("first seeked time is %lld --> %lld", us, seekedUs);

                    if (seekedUs < 0) {
                        /* 视频 seek 失败：把这个负值留在 us 里，第 3 步拿它 seek 其余流并返回 -1。 */
                        us = seekedUs;
                    } else {
                        /* 视频 seek 成功：第 3 步的目标回到请求值，不吃"视频分片起点"。 */
                        us = requestedUs;
                    }

                    /*
                     * 本次改动的验收读数：video_return 仍是视频的分片起点（视频侧一字未变），
                     * 而 other_seek_target 与 user 相同 ⇒ 音频及其它每一路不再被视频的网格拽走。
                     * 一次 seek 一条，天然有界，不需要限频。
                     */
                    AF_LOGI("[dashseek] user=%lld video_return=%lld -> other_seek_target=%lld (stream=%d type=%d): "
                            "every stream is positioned by the SAME request value and lands on its own segment "
                            "grid; only the VOD video path stages its landing\n",
                            (long long) requestedUs, (long long) seekedUs, (long long) us,
                            i->mPStream->getId(), (int) type);
                    i->eos = false;
                    break;
                }
            }
        }

        // 3. use the request value to seek all other streams opened (each on its own segment grid)

        for (auto &i : mStreamInfoList) {
            if (i->selected && i->mPStream->getStreamType() != type) {
                i->eos = false;
                int64_t seekedUs = i->mPStream->seek(us, flags);

                if (seekedUs < 0) {
                    return -1;
                }

                if (i->mPStream->getId() == index) {
                    break;
                }

                AF_LOGD("second seeked time is %lld --> %lld", us, seekedUs);
            }

            i->mPFrame = nullptr;
        }

        return 0;
    }

    if (type == STREAM_TYPE_UNKNOWN) {
        AF_LOGE("unknown stream type, can't seek");
        return -EINVAL;
    }

    //     STREAM_TYPE_VIDEO, STREAM_TYPE_AUDIO, STREAM_TYPE_MIXED
    // 1. finish Aligned switch  TO ME

    for (auto &i : mStreamInfoList) {
        if (i->stopOnSegEnd && i->toStreamId == index) {
            CloseStream(i->mPStream->getId());

            // TODO: open at seek finish
            if (i->toStreamId >= 0) {
                OpenStream(i->toStreamId);
            }

            i->toStreamId = -1;
            i->stopOnSegEnd = false;
            break;
        }
    }

    // 2. do seek
    for (auto &i : mStreamInfoList) {
        if (i->mPStream->getId() == index) {
            i->eos = false;
            return i->mPStream->seek(us, flags);
        }
    }

    assert(0);
    AF_LOGE("unknown stream type, can't seek");
    return -EINVAL;
}

int DashManager::SwitchStreamAligned(int from, int to)
{
    AF_LOGD("SwitchStreamAligned %d -->%d\n", from, to);

    for (auto &i : mStreamInfoList) {
        if (i->mPStream->getId() == from) {
            /*
             * 清晰度切换由 SuperMediaPlayer 的双路 decoder 状态机完成：
             * 旧流必须继续 selected 并持续产包，直到目标 decoder 的首帧
             * 真正送入渲染器。这里若设置 stopOnSegEnd，Seek() 会在旧流
             * 到达分片边界时立即 CloseStream，pending decoder 就会失去
             * 时钟对齐期间所需的旧路保护，表现为卡帧、EOS 或音画不同步。
             * 普通 seek 仍然保留原有 stopOnSegEnd 路径；这里只记录目标，
             * 实际关闭由 SuperMediaPlayer::RenderVideo() 在提交后执行。
             */
            i->toStreamId = to;
            break;
        }
    }

    return 0;
}

int DashManager::getNBSubStream(int index) const
{
    for (const auto &i : mStreamInfoList) {
        if (i->mPStream->getId() == index) {
            return i->mPStream->getNBStream();
        }
    }

    return 0;
}

void DashManager::interrupt(int inter)
{
    for (auto &i : mStreamInfoList) {
        i->mPStream->interrupt(inter);
    }
}

bool DashManager::isRealTimeStream(int index)
{
    for (auto &i : mStreamInfoList) {
        if (i->mPStream->getId() == index) {
            return i->mPStream->isRealTimeStream();
        }
    }

    return false;
}

bool DashManager::isWallclockTimeSyncStream(int index)
{
    if (mPList == nullptr) {
        return false;
    }
    return mPList->isLive();
}

int64_t DashManager::getDurationToStartStream(int index)
{
    for (auto &i : mStreamInfoList) {
        if (i->mPStream->getId() == index) {
            return i->mPStream->getDurationToStartStream();
        }
    }
    return 0;
}

int64_t DashManager::getTargetDuration()
{
    if (mPList == nullptr) {
        return 0;
    }
    return mPList->maxSegmentDuration;
}

int64_t DashManager::getBufferDuration(int index) const
{
    if (mMuxedStream) {
        return mMuxedStream->getBufferDuration();
    }
    for (auto &i : mStreamInfoList) {
        if (i->mPStream->getId() == index) {
            return i->mPStream->getBufferDuration();
        }
    }
    return 0;
}

std::list<AdaptationSet *> DashManager::FindSuitableAdaptationSets(Period* period)
{
    /*
     * 【为什么要收下**全部**视频/音频 AdaptationSet，而不是"每种各挑第一条"】
     *
     * 下面 DashManager::init() 的模型是"**每个 Representation 一条流**"（每档清晰度 =
     * 一条可切换的流，播放器那三档清晰度菜单读的就是这个流列表）。
     *
     * 而 MPD 有两种写法：
     *   * 常见写法：一个 AdaptationSet 里挂整条清晰度阶梯（多条 Representation）——
     *     只挑第一条 AdaptationSet 也够用，因为阶梯都在它里面；
     *   * 另一种（参考实现 dash.js 的 manifest-to-dash 就明确这么写："每个 Representation
     *     单独一个 AdaptationSet，与 MPD 原始格式一致"）：**一档一个 AdaptationSet**。
     *
     * 旧代码遇到第一条视频就 `continue`，于是第二种写法下**只剩下第一档**（而且是文件里
     * 排最前的那档）。用户实测：播 DASH 时清晰度菜单里只有"自动 + 2160p"，其余档位全没了。
     *
     * 所以这里把视频/音频的 AdaptationSet 全部收进来 —— 每档都会在 init() 里变成一条流，
     * 清晰度菜单自然就全了。第一种写法行为不变（本来就只有一个视频 AdaptationSet）。
     */
    std::list<AdaptationSet *> &adaptSetList = period->GetAdaptSets();
    std::list<AdaptationSet *> ret;

    for (auto &ait : adaptSetList) {
        auto representList = ait->getRepresentations();
        std::string mimeType = ait->getMimeType();

        if (mimeType.empty()) {
            for (auto &rit : representList) {
                mimeType = rit->getMimeType();
                if (!mimeType.empty()) {
                    break;
                }
            }
        }

        /* 判据和 Representation::updateStreamType() 一致（那边也是比这两个字符串） */
        if (mimeType == "video/mp4" || mimeType == "audio/mp4") {
            ret.push_back(ait);
        }
        // TODO: subtitle
    }

    return ret;
}
UTCTimer *DashManager::getUTCTimer()
{
    return af_get_utc_timer();
}
void DashManager::setClientBufferLevel(client_buffer_level level)
{
    if (mPreferAudioEnabled) {
        if (mBufferLevel == level) {
            return;
        }
        mBufferLevel = level;
        if (mOpenAudioStreamCount > 0) {
            for (auto &i : mStreamInfoList) {
                if (i->mPStream->getId() == mLowestBandwidthVideoId && i->mPStream->isOpened() && i->selected) {
                    i->mPStream->setPreferAudio(level == client_buffer_level_low);
                }
            }
        }
    }
}

void DashManager::preferAudio(bool prefer)
{
    mPreferAudioEnabled = prefer;
}
