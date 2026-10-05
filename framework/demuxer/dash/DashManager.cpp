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

            bool boundaryNow = false;

            if (i->stopOnSegEnd && i->toStreamId >= 0 && mAlignedSwitchBoundaryStream >= 0) {
                const int64_t playheadForBoundary = getAlignedSwitchPlayheadUs();
                uint64_t playheadSegForBoundary = 0;


                boundaryNow = (playheadForBoundary > 0 &&
                               i->mPStream->getSegmentNumByTime(playheadForBoundary, playheadSegForBoundary) &&
                               (int64_t) playheadSegForBoundary > (int64_t) mAlignedSwitchBoundaryStream);
                if (!boundaryNow && (playheadForBoundary <= 0 || mAlignedSwitchBoundaryStream < 0)) {
                    boundaryNow = true;
                    AF_LOGW("aligned switch: cannot evaluate the playhead boundary (playhead=%lld, armed segment=%d) "
                            "— allowing the switch to proceed at the old stream's segment end rather than getting "
                            "stuck\n", (long long) playheadForBoundary, mAlignedSwitchBoundaryStream);
                }

                if (boundaryNow) {
                    AF_LOGI("aligned switch: the playhead has left the segment it was in when the switch was "
                            "requested (playhead=%lld us -> segment %llu > armed segment %d) — this is the "
                            "switch point\n", (long long) playheadForBoundary,
                            (unsigned long long) playheadSegForBoundary, mAlignedSwitchBoundaryStream);
                }
            }

            if (boundaryNow) {
                /* 不读旧流的下一包：直接走下面既有的边界块（ret == 0 那条分支）。 */
                i->mPFrame = nullptr;
                ret = 0;
            } else {
                ret = i->mPStream->read(i->mPFrame);
            }

            if (ret > 0) {

                {

                    bool armedTargetIsThis = false;

                    for (auto &k : mStreamInfoList) {
                        if (k->stopOnSegEnd && k->toStreamId == i->mPStream->getId()) {
                            armedTargetIsThis = true;
                            break;
                        }
                    }

                    if (armedTargetIsThis && mAlignedSwitchBoundaryStream >= 0) {
                        const uint64_t expectedStartSeg = (uint64_t) (mAlignedSwitchBoundaryStream + 1);


                        (void) expectedStartSeg;
                    }
                }

                i->mPFrame->getInfo().streamIndex = GEN_STREAM_ID(i->mPStream->getId(), i->mPFrame->getInfo().streamIndex);

            } else if (ret == 0) {
                AF_LOGD("EOF %d\n", i->mPStream->getId());


                if (i->stopOnSegEnd) {

                    const int toStreamId = i->toStreamId;
                    const bool oldIsLive = i->mPStream->isLive();



                    if (oldIsLive) {
                        for (auto &j : mStreamInfoList) {
                            if (j->mPStream->getId() == toStreamId) {
                                j->selected = true;
                                j->stopOnSegEnd = false;
                                j->toStreamId = -1;
                                uint64_t targetPosition = i->mPStream->getCurSegPosition() + 1;
                                AF_LOGE("set SegPosition to %llu\n", targetPosition);
                                j->mPStream->setCurSegPosition(targetPosition);
                                break;
                            }
                        }
                    }

                    i->stopOnSegEnd = false;
                    i->mPStream->stopOnSegEnd(false);

                    AF_LOGD("change stream %d -> %d", i->mPStream->getId(), toStreamId);


                    /* 不做"首个包到达再定位一次"：那会用更晚的播放头把目标档再往后挪一片 ⇒ 切换后快进。目标档起点只在本块里定一次。 */
                    mPendingAlignedSwitchStream = -1;


                    mAlignedSwitchBoundaryStream = -1;

                    if (!oldIsLive) {

                        for (auto &j : mStreamInfoList) {
                            if (j->mPStream->getId() != toStreamId) {
                                continue;
                            }

                            j->selected = true;
                            j->stopOnSegEnd = false;
                            j->toStreamId = -1;

                            j->eos = false;
                            j->mPFrame = nullptr;

                            j->mPStream->start();

                            const int64_t playheadUs = getAlignedSwitchPlayheadUs();
                            uint64_t playheadSegNum = 0;
                            const bool byPlayhead = (playheadUs > 0) &&
                                                    j->mPStream->getSegmentNumByTime(playheadUs, playheadSegNum);
                            int setSegRet = -1;
                            uint64_t targetSegNum = 0;

                            if (byPlayhead) {
                                targetSegNum = playheadSegNum;
                                AF_LOGI("aligned switch %d -> %d: the target stream starts at the segment the "
                                        "playhead is in right now (playhead=%lld us -> its segment %llu; target "
                                        "segment %llu)\n",
                                        i->mPStream->getId(), toStreamId, (long long) playheadUs,
                                        (unsigned long long) playheadSegNum, (unsigned long long) targetSegNum);
                                setSegRet = j->mPStream->SetCurSegNum(targetSegNum);
                            } else {
                                if (playheadUs <= 0) {
                                    AF_LOGW("aligned switch %d -> %d: NO PLAYHEAD was delivered to this manager "
                                            "(alignedSwitchPlayheadUs=%lld) — FALLING BACK to the old stream's "
                                            "segment + 1; DELIVERY problem, not a timeline problem\n",
                                            i->mPStream->getId(), toStreamId, (long long) playheadUs);
                                } else {
                                    AF_LOGW("aligned switch %d -> %d: the playhead %lld us is NOT covered by the "
                                            "target stream's segment table — FALLING BACK to the old stream's "
                                            "segment + 1; TIMELINE problem (delivery was fine)\n",
                                            i->mPStream->getId(), toStreamId, (long long) playheadUs);
                                }


                                targetSegNum = (mAlignedSwitchBoundaryStream >= 0)
                                               ? (uint64_t) (mAlignedSwitchBoundaryStream + 1)
                                               : (i->mPStream->getCurSegNum() + 1);
                                AF_LOGE("set SegNum to %llu\n", targetSegNum);
                                setSegRet = j->mPStream->SetCurSegNum(targetSegNum);
                            }

                            if (setSegRet < 0) {

                                setAlignedSwitchResult(-1);
                                j->selected = false;
                                i->selected = true;

                                i->stopOnSegEnd = false;
                                AF_LOGW("aligned switch %d -> %d FAILED: SetCurSegNum(%llu) returned %d — that "
                                        "segment does not exist (e.g. the end of a VOD list); the old stream %d "
                                        "keeps playing and the player will report PLAYER_QUALITY_SWITCH_FAILED\n",
                                        i->mPStream->getId(), toStreamId,
                                        (unsigned long long) targetSegNum, setSegRet, i->mPStream->getId());
                                i->toStreamId = -1;
                                return -EAGAIN;
                            }

                            break;
                        }
                    }

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

    if (mPendingPrefetchStream >= 0) {

        const int prefetchStream = mPendingPrefetchStream;
        mPendingPrefetchStream = -1;
        OpenStream(prefetchStream);


        for (auto &j : mStreamInfoList) {
            if (j->mPStream->getId() == prefetchStream) {
                j->selected = false;
                break;
            }
        }
        AF_LOGI("aligned switch: the target stream %d is opened NOW on the demuxer's read thread so it can "
                "prefetch its segment before the boundary; it stays unselected (bypass) and becomes the active "
                "stream only when the boundary block selects and positions it\n", prefetchStream);


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

    if (key == "alignedSwitchResult") {
        return std::to_string(getAlignedSwitchResult());
    }


    if (key == "alignedSwitchBoundaryReached") {

        for (auto &i : mStreamInfoList) {
            if (!i->stopOnSegEnd) {
                continue;
            }

            const int64_t playheadUs = getAlignedSwitchPlayheadUs();
            uint64_t segNum = 0;

            return (mAlignedSwitchBoundaryStream >= 0 && playheadUs > 0 &&
                    i->mPStream->getSegmentNumByTime(playheadUs, segNum) &&
                    (int64_t) segNum > (int64_t) mAlignedSwitchBoundaryStream) ? "1" : "0";
        }

        return (mAlignedSwitchBoundaryStream >= 0) ? "1" : "0";
    }

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

            i->mPStream->close();
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
             * 没有播放头就定不出"切换点"（切换点 = 播放头所在分片的下一分片起点）。
             * 这时既不能立刻换（没有落点），也不能挂着（切换永远不完成、seek 被永久挂起）⇒
             * 返回负值，让播放器走既有的失败出口 finishQualitySwitch(false)：旧档继续播、报 FAILED。
             * 分片表暂时查不到不算失败（边界块会用目标档自己的表再查一次），所以只在播放头缺失时拒绝。
             */
            const int64_t armPlayhead = getAlignedSwitchPlayheadUs();
            uint64_t armSegNum = 0;

            if (armPlayhead <= 0) {
                AF_LOGW("aligned switch %d -> %d refused: no playhead was delivered to this manager "
                        "(alignedSwitchPlayheadUs=%lld), so the switch point cannot be defined\n",
                        from, to, (long long) armPlayhead);
                return -EINVAL;
            }

            i->stopOnSegEnd = true;
            i->toStreamId = to;

            if (!i->mPStream->isLive()) {
                mPendingPrefetchStream = to;
            }

            mAlignedSwitchBoundaryStream = i->mPStream->getSegmentNumByTime(armPlayhead, armSegNum)
                                           ? (int) armSegNum : -1;

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
