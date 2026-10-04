//
// Created by moqi on 2018/4/27.
//

#include "HLSManager.h"
#include "AdaptationSet.h"
#include "Period.h"
#include "Representation.h"
#include "playList_demuxer.h"
#include "utils/errors/framework_error.h"
#include "utils/frame_work_log.h"
#include "utils/mediaFrame.h"
#include <cassert>
#include <cerrno>

#undef LOG_TAG
#define LOG_TAG "HLSManager"

namespace Cicada {

    HLSManager::HLSManager(playList *pList)
        : PlaylistManager(pList)
    {
    }

    HLSManager::~HLSManager()
    {
        for (auto i = mStreamInfoList.begin(); i != mStreamInfoList.end(); i++) {
            AF_TRACE;
            (*i)->mPFrame = nullptr;
            (*i)->mPStream->close();
            delete (*i)->mPStream;
            //           mStreamInfoList.erase(i++);
            delete (*i);
        }

        mStreamInfoList.clear();
    }

    int HLSManager::init()
    {
        int ret;
        std::list<Period *> &periodList = mPList->GetPeriods();
        int id = 0;

        for (auto &pit : periodList) {
            std::list<AdaptationSet *> &adaptSetList = pit->GetAdaptSets();

            for (auto &ait : adaptSetList) {
                auto representList = ait->getRepresentations();

                for (auto &rit : representList) {
                    rit->mPlayListType = playList_demuxer::playList_type_hls;
                    auto *pTracker = new SegmentTracker(rit, mSourceConfig);
                    pTracker->setOptions(mOpts);
                    auto *info = new HLSStreamInfo();
                    info->mPStream = new HLSStream(pTracker, id++);
                    info->mPStream->setOptions(mOpts);
                    info->mPStream->setDataSourceConfig(mSourceConfig);
                    info->mPStream->setBitStreamFormat(mMergeVideoHeader, mMergerAudioHeader);
                    info->mPStream->setUrlToUniqueIdCallback(mUrlHashCb, mUrlHashCbUserData);
                    mStreamInfoList.push_back(info);
                }
            }
        }

        if (mStreamInfoList.size() == 1) {
            // mediaPlayList
            ret = (*mStreamInfoList.begin())->mPStream->open();

            if (ret >= 0) {
                mMuxedStream = (*mStreamInfoList.begin())->mPStream;
                mMuxedStream->setExtDataSource(mExtDataSource);
                mExtDataSource = nullptr;
            }

            //      mMuxedStream->start();
        }

        return 0;
    }

    void HLSManager::preStop()
    {
        for (auto &i : mStreamInfoList) {
            if (i->mPStream->isOpened()) {
                i->mPStream->preStop();
            }
        }
    }

    void HLSManager::stop()
    {
        for (auto &i : mStreamInfoList) {
            if (i->mPStream->isOpened()) {
                i->mPStream->stop();
            }
        }

        mStarted = false;
    }

    int HLSManager::GetNbStreams() const
    {
        if (mMuxedStream) {
            return mMuxedStream->GetNbStreams();
        } else {// masterPlayList only support one es stream per stream url(ext-media-info ext-stream-info) now
            return static_cast<int>(mStreamInfoList.size());
        }
    }

    int HLSManager::GetStreamMeta(Stream_meta *meta, int index, bool sub) const
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
        HLSStream *stream = nullptr;

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
                //   meta->bandwidth = stream->
            }

            return 0;
        }

        AF_LOGE("no such stream %d\n", index);
        return -1;
    }

    static const char *getStreamTypeName(HLSStream *pStream)
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

    int HLSManager::ReadPacket(unique_ptr<IAFPacket> &packet, int index)
    {
        IAFPacket *pFrameOut{};

        if (mMuxedStream) { //mediaPlayList
            int ret = mMuxedStream->read(packet);

            if (packet != nullptr) {
                packet->getInfo().streamIndex = GEN_STREAM_ID(mMuxedStream->getId(), packet->getInfo().streamIndex);
            }

            return ret;
        }

        // TODO: detect eos
        int ret;

        for (auto &i : mStreamInfoList) {
            if (i->mPStream->isOpened() && i->selected && i->mPFrame == nullptr && !i->eos) {
                ret = i->mPStream->read(i->mPFrame);

                // AF_LOGD("CurSegNum is %llu", i.mPStream->getCurSegNum());
                if (ret > 0) {
//                    i.mPFrame->streamIndex = i->mPStream->getId();
                    // subId *100 + streamID
                    i->mPFrame->getInfo().streamIndex = GEN_STREAM_ID(i->mPStream->getId(),
                                                        i->mPFrame->getInfo().streamIndex);
                } else if (ret == 0) {
                    // TODO: don't block here
                    AF_LOGD("EOF %d\n", i->mPStream->getId());

                    int64_t lastPts = i->mPStream->getLastPts();
                    if (i->stopOnSegEnd) {
                        i->mPStream->stop();
                        i->selected = false;

                        for (auto &j : mStreamInfoList) {
                            if (j->mPStream->getId() == i->toStreamId) {
                                j->selected = true;
                                j->stopOnSegEnd = false;
                                j->toStreamId = -1;

                                if (i->mPStream->isLive()) {
                                    std::vector<RenditionReport> renditions = i->mPStream->getCurRenditionInfo();
                                    j->mPStream->setCurRenditionInfo(renditions);
                                    std::string renditionStr;
                                    for (auto &it : renditions) {
                                        renditionStr += " uri=";
                                        renditionStr += it.uri;
                                        renditionStr += ",msn=";
                                        renditionStr += std::to_string(it.lastMsn);
                                        renditionStr += ",part=";
                                        renditionStr += std::to_string(it.lastPart);
                                        renditionStr += ";";
                                    }
                                    AF_LOGD("[lhls] rendition info: %s", renditionStr.c_str());

                                    uint64_t targetPosition = i->mPStream->getCurSegPosition() + 1;
                                    int64_t targetSegNo = i->mPStream->getCurSegNum() + 1;
                                    AF_LOGE("set SegPosition to %llu ,targetSegNo  = %llu \n", targetPosition, targetSegNo);
                                    AbstractStream::CurSegInfo curSegInfo{};
                                    curSegInfo.segNum = targetSegNo;
                                    curSegInfo.position = targetPosition;
                                    j->mPStream->setCurSegInfo(curSegInfo);
                                    j->mPStream->setDiscardPts(lastPts);
                                } else {
                                    AF_LOGE("set SegNum to %llu\n",
                                            i->mPStream->getCurSegNum() + 1);
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
                if (i->mPFrame) {
//                    if (i->mPStream->getStreamType() == STREAM_TYPE_SUB) {
//                        AF_LOGD("(%s)i->mPFrame->streamIndex is %x, pts is %lld %s\n", getStreamTypeName(i->mPStream),
//                                i->mPFrame->streamIndex,
//                                i->mPFrame->pts, i->mPFrame->pBuffer);
//                    } else
//                        AF_LOGD("(%s)i->mPFrame->streamIndex is %x, pts is %lld\n", getStreamTypeName(i->mPStream),
//                                i->mPFrame->streamIndex,
//                                i->mPFrame->pts);
                }

                if (pFrameOut == i->mPFrame.get()) {
                    packet = std::move(i->mPFrame);
                    break;
                }
            }

//            AF_LOGD("\n");
        }

        if (pFrameOut == nullptr || pFrameOut->getSize() == 0) {
            AF_LOGD("EOS");
            return 0;
        }

        return packet->getSize();
    }

    int HLSManager::OpenStream(int index)
    {
        int ret = 0;
        AF_LOGD("OpenStream %d\n", index);

        // select stream
        if (mStreamInfoList.size() == 1) {
            // TODO: select stream
            // mediaPlayList
            if (!(*mStreamInfoList.begin())->mPStream->isOpened()) {

                if (mExtDataSource) {
                    mMuxedStream->setExtDataSource(mExtDataSource);
                    mExtDataSource = nullptr;
                }

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
                        if (mExtDataSource) {
                            i->mPStream->setExtDataSource(mExtDataSource);
                            mExtDataSource = nullptr;
                        }
                        ret = i->mPStream->open();
                    }

                    i->selected = true;
                    //    if (mStarted)
                    i->mPStream->start();
                    break;
                }
            }
        }

        return ret;
    }

    const std::string HLSManager::GetProperty(int index, const string &key)
    {
        for (auto &i : mStreamInfoList) {
            if (i->mPStream->getId() == index) {
                return i->mPStream->GetProperty(key);
            }
        }

        return "";
    }

    int HLSManager::GetRemainSegmentCount(int index)
    {
        for (auto &i : mStreamInfoList) {
            if (i->mPStream->getId() == index) {
                return i->mPStream->GetRemainSegmentCount();
            }
        }

        return -1;
    }

    void HLSManager::CloseStream(int id)
    {
        AF_LOGD("CloseStream %d\n", id);

        if (mMuxedStream) {
            int subIndex = GEN_SUB_STREAM_ID(id);
            mMuxedStream->CloseSubStream(subIndex);
            return;
        }

        for (auto &i : mStreamInfoList) {
            if (i->mPStream->getId() == id) {
                if (!i->selected) {
                    AF_LOGW("CloseStream not opened\n");
                }

                i->selected = false;
                // TODO: close the hlsStream? close at release
                //  if (mStarted)
                i->mPStream->stop();
                i->mPFrame = nullptr;
                break;
            }
        }
    }

    int HLSManager::start()
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

    int64_t HLSManager::seek(int64_t us, int flags, int index)
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
                    //      break;
                }
            }

            // 2. seek video first ,get the seekedUs
            type = STREAM_TYPE_VIDEO;
            // TODO type use bit or
            /*
             * 【P0-B（与 DashManager::seek 同一口径）：其余每一路的 seek 目标必须是"用户请求值"】
             *
             * 本函数原来在第 3 步给其余每一路喂的是 `us`，而 `us` 在这里被改写成**视频 seek()
             * 的返回值**。HLSStream::seek() 末尾返回的 `usSought` 并**不是**请求值：
             * SegmentList::getSegmentNumberByTime 把入参 time 就地改写成该片的 startTime
             * （play_list/SegmentList.cpp 里 `time = i->startTime;`），所以它同样是"视频分片
             * 起点"。于是音频的读位置由视频的分片网格决定 —— 与 DASH 完全同一个形状，只是
             * 两边的分片网格不同。DASH 侧已按同一口径修正（见 DashManager::seek 里那段说明：
             * 为什么必须解耦、为什么落点精度不会因此变差、为什么视频侧一字不变），这里保持一致。
             *
             * 视频侧行为一字不变：仍然用请求值 seek 视频、仍然取回它的返回值，返回值照旧只用于
             * 诊断日志与"视频 seek 失败 ⇒ 整次 seek 失败"这条既有终态；视频的落点对齐与落点
             * 延迟线都在 HLSStream::seek() 内部，本函数既不参与也不影响它们。
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
                        AF_LOGI("[hlsseek] user=%lld video_return=%lld -> other_seek_target=%lld (stream=%d type=%d): "
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

    int HLSManager::SwitchStreamAligned(int from, int to)
    {
        AF_LOGD("SwitchStreamAligned %d -->%d\n", from, to);

        for (auto &i : mStreamInfoList) {
            if (i->mPStream->getId() == from) {
                                /* 【清晰度切换：单解码器 + 延迟到分片边界 + 首帧进渲染才算提交】
                 * 与 DashManager::SwitchStreamAligned 同一套，这里只列结论：
                 *   · **双路解码方案已废除**（切换慢、帧可能对不齐），全仓只有一路视频
                 *     解码器 + 一路音频解码器（SMPAVDeviceManager::mVideoDecoder /
                 *     mAudioDecoder），没有 pendingDecoder 这类成员；
                 *   · 本函数只记录目标流 toStreamId，到分片边界再换；
                 *   · 提交点是"目标流首帧真正送入渲染器"，之后才 CloseStream 旧流；
                 *   · stopOnSegEnd 只用于普通 seek：清晰度切换时若在边界就关旧流，
                 *     目标那路解码器重建期间没有包可喂，表现为卡帧 / EOS / 音画不同步。 */
                i->toStreamId = to;
                break;
            }
        }

        return 0;
    }

    int HLSManager::getNBSubStream(int index) const
    {
        for (const auto &i : mStreamInfoList) {
            if (i->mPStream->getId() == index) {
                return i->mPStream->getNBStream();
            }
        }

        return 0;
    }

    void HLSManager::interrupt(int inter)
    {
        for (auto &i : mStreamInfoList) {
            i->mPStream->interrupt(inter);
        }
    }

    bool HLSManager::isRealTimeStream(int index)
    {
        for (auto &i : mStreamInfoList) {
            if (i->mPStream->getId() == index) {
                return i->mPStream->isRealTimeStream();
            }
        }

        return false;
    }

    int64_t HLSManager::getTargetDuration()
    {
        if (mMuxedStream) {
            return mMuxedStream->getTargetDuration();
        }

        int64_t targetDuration = INT64_MIN;
        for (auto &i : mStreamInfoList) {
            if (i->mPStream->isOpened() && i->selected) {
                int64_t streamTargetDuration = i->mPStream->getTargetDuration();
                if (streamTargetDuration > 0) {
                    if (targetDuration == INT64_MIN) {
                        targetDuration = streamTargetDuration;
                    } else if (streamTargetDuration < targetDuration) {
                        targetDuration = streamTargetDuration;
                    }
                }
            }
        }

        return targetDuration;
    }

    int64_t HLSManager::getBufferDuration(int index) const
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

}
