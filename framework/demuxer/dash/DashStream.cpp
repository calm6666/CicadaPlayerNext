//
// Created by yuyuan on 2021/03/17.
//
#define LOG_TAG "DashStream"

#include "DashStream.h"
#include "DashSegment.h"
#include "DashSegmentTracker.h"
#include "data_source/dataSourcePrototype.h"
#include "demuxer/DemuxerMeta.h"
#include "demuxer/IDemuxer.h"
#include "demuxer/play_list/Helper.h"
/*
 * 【为什么必须包含】DashStream::seek() 里要取"tracker 真正选中分片的起点"：
 *     mPTracker->getCurrentRepresentation()->getMediaSegmentStartTime(num)
 * 而 DashSegmentTracker.h(:18) 只对 Representation 做了**前向声明** ⇒ 直接这样调用会报
 *   error: member access into incomplete type 'Cicada::Representation'
 * 这里包含它的**定义**（framework/demuxer/play_list/Representation.h:24，继承
 * Dash::SegmentInformation；getMediaSegmentStartTime 实现见
 * demuxer/dash/SegmentInformation.cpp:52）。回退：删掉本 include 即复现该编译错误。
 */
#include "demuxer/play_list/Representation.h"
#include "utils/af_string.h"
#include "utils/errors/framework_error.h"
#include "utils/frame_work_log.h"
#include "utils/mediaFrame.h"
#include "utils/timer.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>

// TODO support active and no active mode

using namespace Cicada;

static const int defaultInitSegSize = 1024 * 1024;

DashStream::DashStream(DashSegmentTracker *pTracker, int id) : mPTracker(pTracker), mId(id)
{}

DashStream::~DashStream()
{
    close();
    if (mInitSegBuffer) {
        free(mInitSegBuffer);
    }
    delete mThreadPtr;
    delete mPTracker;
    mStreamStartTimeMap.clear();
}

int DashStream::getStreamType() const
{
    return mPTracker->getStreamType();
}

int DashStream::GetRemainSegmentCount()
{
    if (mPreloadSucc) {
        //AF_LOGI("GetRemainSegmentCount preload success");
        return 1;
    }
    return mPTracker->GetRemainSegmentCount();
}

int DashStream::read_callback(void *arg, uint8_t *buffer, int size)
{
    auto *pHandle = static_cast<DashStream *>(arg);
    int ret;

    if (pHandle->mInterrupted) {
        return -EIO;
    }

    if (pHandle->mIsDataEOS) {
        AF_LOGE("%s : %d stream(%d),EOS", __func__, __LINE__, pHandle->mPTracker->getStreamType());
        return 0;
    }

    if (pHandle->mInitSegBuffer) {
        int initSegSize = pHandle->mInitSegSize - pHandle->mInitSegPtr;

        if (initSegSize > 0) {
            int readSize = std::min(initSegSize, size);
            memcpy(buffer, pHandle->mInitSegBuffer + pHandle->mInitSegPtr, readSize);
            pHandle->mInitSegPtr += readSize;
            return readSize;
        }
    }

    ret = pHandle->readSegment(buffer, size);

    if (ret == 0) {
        if (pHandle->mStopOnSegEnd) {
            pHandle->mIsEOS = true;
            return 0;
        }
        if (pHandle->mReopen) {
            return 0;
        }
        int updateRet = pHandle->updateSegment();
        if (updateRet == 0) {
            return pHandle->readSegment(buffer, size);
        } else if (updateRet == -EAGAIN) {
            return 0;
        } else {
            return updateRet;
        }
    }

    return ret;
}

int64_t DashStream::seek_callback(void *arg, int64_t offset, int whence)
{
    auto *pHandle = static_cast<DashStream *>(arg);

    return pHandle->seekSegment(offset, whence);
}

int DashStream::readSegment(const uint8_t *buffer, int size)
{
    int ret = 0;

    if (mExtDataSource) {
        ret = mExtDataSource->Read((void *) buffer, size);
    } else if (mPdataSource) {
        ret = mPdataSource->Read((void *) buffer, (size_t) size);
    }

    return ret;
}

int64_t DashStream::seekSegment(off_t offset, int whence)
{
    int64_t ret;
    if (mExtDataSource) {
        ret = mExtDataSource->Seek(offset, whence);
    } else {
        ret = mPdataSource->Seek(offset, whence);
    }

    return ret;
}

int DashStream::open()
{
    return 0;
}

int DashStream::UpdateInitSection()
{
    std::string uri;
    int ret;
    mInitSegPtr = 0;

    Dash::DashSegment *initSeg = mPTracker->getInitSegment();
    if (initSeg == mCurInitSeg || initSeg == nullptr) {
        return 0;
    }

    uri = initSeg->getUrlSegment().toString(mPTracker->getCurSegNum(), mPTracker->getCurrentRepresentation());
    ret = tryOpenSegment(uri, initSeg->startByte, initSeg->endByte);

    if (ret < 0) {
        return ret;
    }

    mCurInitSeg = initSeg;
    mInitSegSize = defaultInitSegSize;
    if (mCurInitSeg->startByte == INT64_MIN && mCurInitSeg->endByte == INT64_MIN) {
        mInitSegSize = seekSegment(0, SEEK_SIZE);
    } else if (mCurInitSeg->startByte != INT64_MIN && mCurInitSeg->endByte == INT64_MIN) {
        mInitSegSize = seekSegment(0, SEEK_SIZE) - mCurInitSeg->startByte;
    } else if (mCurInitSeg->startByte == INT64_MIN && mCurInitSeg->endByte != INT64_MIN) {
        mInitSegSize = mCurInitSeg->endByte + 1;
    } else {
        mInitSegSize = mCurInitSeg->endByte - mCurInitSeg->startByte + 1;
    }

    if (mInitSegSize < 0) {
        mInitSegSize = defaultInitSegSize;
    }

    if (mInitSegBuffer) {
        free(mInitSegBuffer);
    }

    mInitSegBuffer = static_cast<uint8_t *>(malloc(mInitSegSize));
    int size = 0;

    do {
        // TODO: realloc mInitSegBuffer
        ret = readSegment(mInitSegBuffer + size, mInitSegSize - size);

        if (ret > 0) {
            size += ret;
        } else {
            break;
        }
    } while (size < mInitSegSize);

    mInitSegSize = size;
    return 0;
}

int64_t DashStream::getDurationToStartStream()
{
    return mPTracker->getDurationToStartStream();
}

vector<mediaSegmentListEntry> DashStream::getSegmentList()
{
    return mPTracker->getSegmentList();
}

void DashStream::enableCache(bool enalbe)
{
    mEnableCache = enalbe;
}

int64_t DashStream::getBufferDuration() const
{
    if (mExtDataSource) {
        return mExtDataSource->getBufferDuration();
    }
    if (mPdataSource) {
        return mPdataSource->getBufferDuration();
    }
    return 0;
}

static inline uint64_t getSize(const uint8_t *data, unsigned int len, unsigned int shift)
{
    uint64_t size(0);
    const uint8_t *dataE(data + len);

    for (; data < dataE; ++data) {
        size = size << shift | *data;
    }

    return size;
};

int DashStream::open_internal()
{
    int ret;
    AF_LOGD("mPTracker type is %d\n", mPTracker->getStreamType());
    ret = mPTracker->init();
    if (mPTracker->isLive()) {
        mSuggestedPresentationDelay = mPTracker->getLiveDelay();
        mStreamStartTime = mPTracker->getStreamStartTime();
    } else {
        mSuggestedPresentationDelay = 0;
        mStreamStartTime = -1;
    }
    AF_LOGD("mSuggestedPresentationDelay=%lld, mStreamStartTime=%lld \n", mSuggestedPresentationDelay, mStreamStartTime);

    if (ret < 0) {
        AF_TRACE;
        return ret;
    }

    if (mSeekPendingUs >= 0) {
        uint64_t num = 0;
        auto usSeeked = (uint64_t) mSeekPendingUs;
        bool b_ret = mPTracker->getSegmentNumberByTime(usSeeked, num);

        if (!b_ret) {
            AF_LOGE("getSegmentNumberByTime error us is %lld\n", mSeekPendingUs);
        } else {
            AF_LOGI("%s:%d stream (%d) usSeeked is %lld seek num is %d\n", __func__, __LINE__, mPTracker->getStreamType(), usSeeked, num);
            mPTracker->setCurSegNum(num);
        }

        /*
         * 【seek 落点延迟线：装弹（延后型 seek）】tracker 还没 init 时，seek() 只把目标存进
         * mSeekPendingUs（那一刻读线程可能正跑在 open_internal 里，不能去动延迟线的 stage）。
         * 这里才是"真正定位到分片"的那一刻，而且本函数跑在读线程上：装弹之后紧接着才会产出
         * 第一个包，不存在与读线程并发写延迟线的问题。
         */
        mSeekLanding.arm(mSeekPendingUs, "DASH", mPTracker);
        mSeekPendingUs = -1;
    }

    //   AF_TRACE;
    //   AF_LOGD("mPTracker->getCurSegNum is %llu", mPTracker->getCurSegNum());

    mStopOnSegEnd = false;
    mCurSeg = nullptr;
    mIsStartSegment = true;
    mCurSeg = mPTracker->getStartSegment();
    int trySegmentTimes = 0;

    do {
        trySegmentTimes++;

        if (mCurSeg == nullptr) {
            if (mPTracker->isLive()) {
                AF_TRACE;
                return -EAGAIN;
            } else {
                AF_LOGE("can't find seg %llu\n", mPTracker->getCurSegNum());
                return gen_framework_errno(error_class_format, 0);
            }
        }

        ret = UpdateInitSection();
        if (ret < 0) {
            return ret;
        }

        std::string uri = mCurSeg->getUrlSegment().toString(mPTracker->getCurSegNum(), mPTracker->getCurrentRepresentation());
        AF_LOGD("open uri is %s seq is %llu\n", uri.c_str(), mCurSeg->sequence);
        ret = tryOpenSegment(uri, mCurSeg->startByte, mCurSeg->endByte);

        if (isHttpError(ret)) {
            resetSource();
            if (!mPTracker->bufferingAvailable()) {
                return -EAGAIN;
            }
            mCurSeg = mPTracker->getNextSegment();

            if (trySegmentTimes < 5 && !mInterrupted) {
                af_msleep(20);
                continue;
            }
        }

        if (ret < 0) {
            AF_TRACE;
            mDataSourceError = ret;
            resetSource();
            return ret;
        }
    } while (ret < 0);

    if (mInterrupted) {
        return FRAMEWORK_ERR_EXIT;
    }

    ret = createDemuxer();

    if (ret >= 0) {
        mIsOpened_internal = true;
    } else {
        AF_LOGE("open demuxer error %d\n", ret);
        return ret;
    }

    //     mStatus = status_inited;

    if (mPdataSource) {
        string info = mPdataSource->GetOption("connectInfo");
        openInfoArray.addJSON(CicadaJSONItem(info));
    }

    return ret;
}

int64_t DashStream::getPackedStreamPTS()
{
    return INT64_MIN;
}

int DashStream::createDemuxer()
{
    int ret;
    mError = 0;
    mDataSourceError = 0;

    if (mDemuxerMeta && mDemuxerMeta->id != mCurSeg->discontinuity) {
        mDemuxerMeta = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(mHLSMutex);
        mPDemuxer = std::unique_ptr<demuxer_service>(new demuxer_service(nullptr));
    }

    mPDemuxer->setOptions(this->mOpts);
    unique_ptr<DemuxerMeta> demuxerMeta = unique_ptr<DemuxerMeta>(new DemuxerMeta());
    demuxerMeta->ownerUrl = mPTracker->getPlayListUri();
    mPDemuxer->setDemuxerMeta(demuxerMeta);
    mPDemuxer->SetDataCallBack(read_callback, this, nullptr, nullptr, nullptr);
    ret = mPDemuxer->createDemuxer(demuxer_type_unknown);

    if (ret < 0) {
        return ret;
    }

    if (mPDemuxer->getDemuxerHandle()) {
        mPDemuxer->getDemuxerHandle()->setBitStreamFormat(this->mMergeVideoHeader, this->mMergerAudioHeader);
    }

    //        if (mDemuxerMeta) {
    //            mPDemuxer->getDemuxerHandle()->setMeta(mDemuxerMeta.get());
    //        }

    ret = mPDemuxer->initOpen();

    if (ret >= 0) {
        int nbStream = mPDemuxer->GetNbStreams();
        AF_LOGI("file have %d streams\n", nbStream);
        bool needUpdateMeta = false;

        if (!mDemuxerMeta) {
            mDemuxerMeta = std::unique_ptr<DemuxerMetaInfo>(new DemuxerMetaInfo());
            mDemuxerMeta->id = mCurSeg ? mCurSeg->discontinuity : 0;
            needUpdateMeta = true;
        }

        unique_ptr<streamMeta> meta = nullptr;

        // open all stream in demuxer
        for (int i = 0; i < nbStream; ++i) {
            mPDemuxer->GetStreamMeta(meta, i, false);
            Stream_type subType = ((Stream_meta *) (*meta))->type;
            Stream_type trackerType = (Stream_type) mPTracker->getStreamType();
            AF_LOGD("sub type is %d\n", subType);
            AF_LOGD("trackerType type is %d\n", trackerType);

            if ((trackerType == STREAM_TYPE_MIXED && subType != STREAM_TYPE_UNKNOWN) || subType == trackerType) {
                AF_LOGW("open stream  index is %d\n", i);
                mPDemuxer->OpenStream(i);
                OpenedStreamIndex = i;
            }

            if (needUpdateMeta) {
                mDemuxerMeta->meta.push_back(move(meta));
            }

            mDemuxerMeta->bContinue = true;
        }

        mPacketFirstPts = getPackedStreamPTS();
    }

    return ret;
}

int DashStream::tryOpenSegment(const string &uri, int64_t start, int64_t end)
{
    AF_LOGD("tryOpenSegment: %s, [%lld,%lld]\n", uri.c_str(), start, end);
    int retryTimes = 0;
    int ret;

    do {
        resetSource();
        ret = openSegment(uri, start, end);
        retryTimes++;

        if (ret >= 0 || retryTimes > 2) {
            break;
        }

        af_msleep(20);
    } while (isHttpError(ret) && !mInterrupted);

    return ret;
}

int DashStream::openSegment(const string &uri, int64_t start, int64_t end)
{
    int ret;
    int64_t fixEnd = end;
    if (fixEnd != INT64_MIN) {
        fixEnd++;
    }
    if (mExtDataSource) {
        if (mIsFirstOpen) {
            mIsFirstOpen = false;
            if (!mPTracker->isLive()) {
                mExtDataSource->setSegmentList(getSegmentList());
            }
        }
        mExtDataSource->setRange(start, fixEnd);
        int ret = mExtDataSource->Open(uri);
        mExtDataSource->enableCache(uri, mEnableCache);
        return ret;
    }

    if (mPdataSource == nullptr) {
        recreateSource(uri);
        mPdataSource->setRange(start, fixEnd);
        ret = mPdataSource->Open(0);
    } else {
        mPdataSource->setRange(start, fixEnd);
        ret = mPdataSource->Open(uri);
        mPdataSource->enableCache(uri, mEnableCache);
    }

    return ret;
}

void DashStream::resetSource()
{
    std::lock_guard<std::mutex> lock(mHLSMutex);

    if (mPdataSource) {
        if (!mIsOpened_internal) {
            string info = mPdataSource->GetOption("connectInfo");
            openInfoArray.addJSON(CicadaJSONItem(info));
        }

        //           std::lock_guard<std::mutex> lock(mHLSMutex);
        //            delete mPdataSource;
        //            mPdataSource = nullptr;
    }
}

void DashStream::recreateSource(const string &url)
{
    resetSource();
    std::lock_guard<std::mutex> lock(mHLSMutex);
    mPdataSource = dataSourcePrototype::create(url, mOpts, DS_NEED_CACHE);
    mPdataSource->Set_config(mSourceConfig);
    mPdataSource->Interrupt(mInterrupted);
    if (!mPTracker->isLive()) {
        mPdataSource->setSegmentList(getSegmentList());
    }
    mPdataSource->setUrlToUniqueIdCallback(mUrlHashCb, mUrlHashCbUserData);
    mPdataSource->enableCache(url, mEnableCache);
}

void DashStream::clearDataFrames()
{
    std::unique_lock<std::mutex> locker(mDataMutex);

    while (0 < mQueue.size()) {
        mQueue.pop_front();
    }

    /*
     * 【seek 落点延迟线】延迟线里攒着的包同样是"已经收下、还没交出去"的队列：位置一旦作废
     * （seek / 换分片 reopen / stop），它必须跟着一起丢掉并复位，否则新的位置会带着旧的
     * 落点状态继续跑。这个函数的三个调用点都在读线程 pause() 或 join 之后，
     * 所以这里清 stage 不存在与读线程并发写的问题。
     *
     * 用 DropStage 而不是 Reset：**seek 目标要留着**。stop()（含 DashManager::CloseStream）
     * 之后重新 start() 会在同一个分片上重开（mCurrentSegNumber 没动），目标依然有效；旧的
     * 写法把目标一起清掉，于是"装了弹又被 stop 抹掉"的那些 seek 就悄悄退回分片首。
     * 目标过期的情况由 SeekLandingStage::filter 的"第一包 pos > 目标 就放弃"兜住，不会误丢数据。
     * 真正需要连目标一起清的只有 reopenSegment（位置是按分片号重设的，见那里的 Reset）。
     */
    mSeekLanding.dropStage();
}

void DashStream::close()
{
    stop();

    if (mPDemuxer) {
        mPDemuxer->close();
        std::lock_guard<std::mutex> lock(mHLSMutex);
        mPDemuxer = nullptr;
    }

    mIsOpened = false;
    mIsOpened_internal = false;
    openInfoArray.reset();
}

int DashStream::read_thread()
{
    if (mExited) {
        return -1;
    }

    int ret;

    if (mIsOpened && !mIsOpened_internal) {
        ret = open_internal();

        if (ret == -EAGAIN) {
            AF_LOGI("open_internal again\n");
            af_usleep(10000);
            ret = mPTracker->reLoadPlayList();

            if (ret == gen_framework_http_errno(403)) {
                mError = ret;
            }

            return 0;
        } else if (ret < 0) {
            mError = ret;
            af_msleep(10);
            return 0;// continue retry
        }
    }

    {
        std::unique_lock<std::mutex> waitLock(mDataMutex);
        bool waitResult = mWaitCond.wait_for(waitLock, std::chrono::milliseconds(10),
                                             [this]() { return mQueue.size() <= 1 || mInterrupted || mSwitchNeedBreak || mExited; });

        if (!waitResult || mInterrupted || mSwitchNeedBreak || mExited) {
            return 0;
        }
    }

    unique_ptr<IAFPacket> tmp{};
    int packet_size = read_internal(tmp);

    if ((nullptr != tmp) && (nullptr != tmp->getData()) && (0 < tmp->getSize())) {
        std::unique_lock<std::mutex> waitLock(mDataMutex);
        mQueue.push_back(move(tmp));
    } else if (nullptr != tmp) {
        AF_LOGE("read_thread frame size be set as 0");
        return 0;
    }

    mWaitCond.notify_one();

    if (packet_size == 0) {
        mIsEOS = true;
        return -1;
    } else if (packet_size < 0) {
        if (packet_size == -EAGAIN) {
            //     AF_LOGD("read timed out");
            /*
             * 【seek 落点延迟线】攒前缀时不能每个包都停 10ms。
             *
             * "不晚于目标的最后一个关键帧"可能要几十个包之后才出现（本片源一个 GOP ≈ 60 个包），
             * 每包停 10ms 就是 0.6 秒，正好把这次优化的收益吃光。
             * 判据是**进度**、不是时间：只有上一轮确实从 demuxer 取到了一个包
             * （SeekLandingStage::filter 把它收进延迟线时置的进度），才跳过这一次停顿；
             * demuxer 没数据（等网络）时进度为假，照旧 af_msleep(10) —— 不会空转、不是计时器。
             * 延迟线一放行或一复位，这个标志就被清掉，节奏立刻回到原样。
             */
            if (!mSeekLanding.consumeProgress()) {
                af_msleep(10);
            }
            return 0;
        }

        if (packet_size != FRAMEWORK_ERR_EXIT) {
            AF_LOGD("read error 0x%4x %s\n", -packet_size, framework_err2_string(packet_size));
            mError = packet_size;
            af_msleep(10);
            return 0;// continue retry
        }
    }

    return 0;
}

int DashStream::read(unique_ptr<IAFPacket> &packet)
{
    int ret;
    packet = nullptr;

    if (mThreadPtr) {
        std::unique_lock<std::mutex> waitLock(mDataMutex);

        if (mLastReadSuccess && (mQueue.empty())) {
            mWaitCond.wait_for(waitLock, std::chrono::milliseconds(1), [this]() { return !mQueue.empty(); });
        }

        if (mQueue.empty()) {
            mLastReadSuccess = false;

            if (mIsEOS) {
                AF_LOGI("return eos\n");
                return 0;
            } else if (mError < 0) {
                ret = mError;
                mError = 0;
                AF_LOGE("return error %d\n", ret);
                return ret;
            } else {
                return -EAGAIN;
            }
        }

        packet = move(mQueue.front());
        mQueue.pop_front();
        ret = static_cast<int>(packet->getSize());
        mWaitCond.notify_one();
        mLastReadSuccess = true;
        return ret;
    } else {
        return read_internal(packet);
    }
}

int DashStream::updateSegment()
{
    mIsStartSegment = false;
    Dash::DashSegment *seg = nullptr;
    AF_LOGD("getCurSegNum is %llu\n", mPTracker->getCurSegNum());
    if (mIsPreload) {
        seg = mCurSeg;
        mIsPreload = false;
    } else {
        seg = mPTracker->getNextSegment();
    }
    if (seg == nullptr) {
        seg = mPTracker->getNextSegment();
    }

    // if current segment time > live delay, discard it
    if (isLive()) {
        int64_t liveDelay = mPTracker->getLiveDelay();
        int64_t segmentDuration = mPTracker->getSegmentDuration();
        int64_t discardBuffer = liveDelay;
        if (mPreferAudio) {
            discardBuffer -= segmentDuration;
        }
        if (discardBuffer < 0) {
            discardBuffer = 0;
        }
        
        int64_t utcTime = af_get_utc_time();
        while (seg && ((utcTime - (seg->fixedStartTime + mStreamStartTime)) > discardBuffer + segmentDuration)) {
            AF_LOGD("DashStream %d, discard segment %llu because it is too late", mId, mPTracker->getCurSegNum());
            seg = mPTracker->getNextSegment();
        }
    }

    int ret = 0;
    mCurSeg = nullptr;

    if (seg) {
        do {
            mCurSeg = seg;
            std::string uri = mCurSeg->getUrlSegment().toString(mPTracker->getCurSegNum(), mPTracker->getCurrentRepresentation());

            AF_LOGD("open segment %lld %lld, %lld", (af_get_utc_time() - (mCurSeg->fixedStartTime + mStreamStartTime)) / 1000,
                    af_get_utc_time(), mCurSeg->fixedStartTime + mStreamStartTime);

            ret = tryOpenSegment(uri, seg->startByte, seg->endByte);

            if (isHttpError(ret) || isLocalFileError(ret)) {
                resetSource();
                if (!mPTracker->bufferingAvailable()) {
                    mIsPreload = true;
                    mPreloadSucc = false;
                    return -EAGAIN;
                }
                seg = mPTracker->getNextSegment();

                if (seg) {
                    af_msleep(20);
                    continue;
                } else if (mPTracker->isLive()) {
                    return -EAGAIN;
                } else {
                    //no more seg
                    break;
                }
            }
        } while (isHttpError(ret) || isLocalFileError(ret));

        if (ret < 0) {
            mDataSourceError = ret;

            if (ret != FRAMEWORK_ERR_EXIT) {
                mError = ret;
            }

            resetSource();
            return ret;
        } else {
            if (!mPTracker->bufferingAvailable()) {
                mPreloadSucc = true;
            }
        }
        return 0;
    } else {
        AF_LOGE("EOS");
        mIsDataEOS = true;
        return -EAGAIN;
    }

    return -EAGAIN;
}

int DashStream::read_internal(std::unique_ptr<IAFPacket> &packet)
{
    //TODO: move read synMsgRst to class member
    int ret = mPTracker->reLoadPlayList();

    if (ret == gen_framework_http_errno(403)) {
        return ret;
    }

    packet = nullptr;

    ret = mPDemuxer->readPacket(packet);
    //AF_LOGD("mPDemuxer->readPacket ret is %d,pFrame is %p", ret, *pFrame);

    if (ret == -EAGAIN) {
        if (mInterrupted) {
            return ret;
        }
    } else if (ret == FRAMEWORK_ERR_EXIT) {
        AF_LOGW("mPDemuxer->readPacket FRAMEWORK_ERR_EXIT\n");
    }

    if (ret == 0 && mStopOnSegEnd) {
        /* 【seek 落点延迟线】本片到此为止，先把延迟线里攒着的（本片末尾那一段）整体交出，再报停。 */
        mSeekLanding.flush();
        mIsEOS = true;
        AF_LOGE("mStopOnSegEnd");
        return 0;
    }

    if (ret == gen_framework_errno(error_class_network, network_errno_http_range) || ret == -EIO) {
        ret = 0;
    }

    if (ret == 0 || mReopen) {
        if (mReopen) {
            AF_LOGD("reopen");
            mReopen = false;
        }

        /*
         * 【seek 落点延迟线】本分片读完（把 demuxer 读到 EOS）时，延迟线里攒着的正是本片最后
         * 那一段：这属于"目标落在本片最后一个 GOP"的情形，等不到"timePosition > 目标"的包，
         * 判据只能在这里收口。整体按序交出，绝不能跟着分片一起丢掉。
         */
        mSeekLanding.flush();

        ret = updateSegment();

        if (mIsDataEOS) {
            return 0;
        }

        if (ret >= 0) {
            UpdateInitSection();
            ret = createDemuxer();

            if (ret >= 0) {
                int nbStream = mPDemuxer->GetNbStreams();
                AF_LOGI("file have %d streams\n", nbStream);
                // open all stream in demuxer
                Stream_meta meta{};

                for (int i = 0; i < nbStream; ++i) {
                    mPDemuxer->GetStreamMeta(&meta, i, false);

                    if (meta.type == mPTracker->getStreamType() ||
                        (mPTracker->getStreamType() == STREAM_TYPE_MIXED && meta.type != STREAM_TYPE_UNKNOWN)) {
                        mPDemuxer->OpenStream(i);
                    }

                    releaseMeta(&meta);
                }

                mPacketFirstPts = getPackedStreamPTS();
            }
        }

        packet = nullptr;
        return -EAGAIN;
    }

    if (ret == -EAGAIN && mPTracker->isLive()) {

        ret = updateSegment();

        if (ret < 0) {
            return ret;
        }

        return -EAGAIN;
    }

    if (packet != nullptr) {
        //  AF_LOGD("read a frame \n");

        if (mPTracker->getStreamType() != STREAM_TYPE_MIXED) {
            packet->getInfo().streamIndex = 0;
        }

        if (mPacketFirstPts != INT64_MIN && packet->getInfo().pts != INT64_MIN) {
            packet->getInfo().pts += mPacketFirstPts;
            packet->getInfo().dts += mPacketFirstPts;
        }

        if (mCurSeg) {
            // mark the seg start time to first seg frame
            AF_LOGD("stream (%d) mark startTime %llu\n", mPTracker->getStreamType(), mCurSeg->fixedStartTime);
            AF_LOGD("stream (%d)pFrame->pts is %lld pos is %lld flags is %d streamIndex is %d\n", mPTracker->getStreamType(),
                    packet->getInfo().pts, packet->getInfo().pos, packet->getInfo().flags, packet->getInfo().streamIndex);

            if (packet->getInfo().flags == 0) {
                AF_LOGE("not a key frame\n");
                _hex_dump(packet->getData(), 16);
            }

            int nbStreams = GetNbStreams();

            for (int i = 0; i < nbStreams; i++) {
                mStreamStartTimeMap[i].timePosition = mCurSeg->fixedStartTime;
                if (mStreamStartTime >= 0) {
                    mStreamStartTimeMap[i].utcTime = mStreamStartTime + mCurSeg->fixedStartTime;
                }
                mStreamStartTimeMap[i].seamlessPoint = true;
            }

            mCurSeg = nullptr;
        }

        int streamIndex = packet->getInfo().streamIndex;
        packet->getInfo().seamlessPoint = mStreamStartTimeMap[streamIndex].seamlessPoint;

        if (mStreamStartTimeMap[streamIndex].seamlessPoint) {
            if (packet->getInfo().pts != INT64_MIN) {
                const int64_t manifestDelta = mStreamStartTimeMap[streamIndex].timePosition - packet->getInfo().pts;
                const int64_t lastPts = mStreamStartTimeMap[streamIndex].lastFramePts;

                /*
                 * ============ 按 DASH 规范：PTO 是**每个 Representation 的常量** ============
                 *
                 * ISO/IEC 23009-1 / DASH-IF IOP 定义三条时间轴，换算关系是
                 *     presentationTime = PeriodStart + (mediaTime − presentationTimeOffset)
                 * 其中 `@presentationTimeOffset`（PTO）**每路 Representation 只有一个常量**、
                 * 缺省 0；MPD 里 SegmentTimeline/@t 或 SegmentTemplate 累积出来的分片起始时间
                 * 属于 Period/Presentation 轴，作用是**索引/寻址**（选段与 seek），
                 * 不是"每个分片重新定义一次媒体↔节目的换算"。
                 *
                 * 我们这里原来的写法是**每段**用 mCurSeg->fixedStartTime 重算 time2ptsDelta，
                 * 等于"每段重新推一次 PTO" —— 规范里没有这种事。它的实际效果是把
                 * "MPD 声明的时间"与"媒体自己的时间戳"之间的不一致一段段吸收掉：
                 *   · 真机实测（同一份 m4s，mpd/m3u8 两种清单，Android+Qt 都复现）：
                 *     音频这一路两轴差恒 ≈ +10 ms（与 MPD 一致），而**视频**这一路
                 *     每个分片差 427 ms（40/80/110/140/170 s 落点 = −1708/−3416/−4697/
                 *     −5979/−7260 ms）⇒ 每段重算就把视频的漂移"隐藏"进节目轴，
                 *     于是音频与视频被拆成两条速率不同的轴：**声音越来越慢、越播越偏**。
                 *   · 按规范（PTO 恒定）处理时，两路都跟着**自己的媒体时间戳**推进，
                 *     音画关系与媒体本身一致；HLS 那侧（HLSStream.cpp:1295 用播放列表累计
                 *     时长，本片源与媒体一致、差恒 0）本来就是这个行为 ⇒ 同一份 m4s 走 HLS 没事。
                 *
                 * 处置：**只在媒体时间戳真的重置时**（换 Period / 编码器重启 —— 那才是
                 * PTO 允许重新定义的时刻）才接受清单给的新锚点；连续时保持原 delta。
                 * ⚠「跳段 / seek」不算重置：seek 前最后一个包与 seek 后第一个包的 pts
                 * 必然差很远，所以旧判据（与"上一帧 pts + 帧长"差出 ±100ms 就算不连续）
                 * **每 seek 一次就重锚一次**，等于把 MPD 的"名义段长 vs 真实段长"误差
                 * 当偏移量灌进演示轴（真机实测：30s 落点 −1281250µs = 3×(−427083)、
                 * 130s 落点 −5552083µs = 13×(−427083)，而 −427083µs 正是 MPD 声明
                 * 10.000000s 与媒体真实 10.427083s 之差）。这正是"还是一样的"的原因。
                 * 判据是纯状态判断（新包 pts 相对上一包**回退** > 5s），没有计时器；
                 * DashStream::seek() 会把 lastFramePts 清成 INT64_MIN，保证 seek
                 * （前进或后退）都不会被误判成 Period 切换。
                 * timePosition 的算法仍是 pts + delta，下游（seek 落点、位置地板、
                 * 读前闸门、渲染归一化）一行都不用改。
                 */
                const bool firstAnchor = (mStreamStartTimeMap[streamIndex].time2ptsDelta == INT64_MIN);
                // mSeekSuppressResetOnce：seek 后第一个分片点不参与"媒体重置"判断
                // （seek 前最后一个包与 seek 后第一个包的 pts 必然差很远，那不是 Period 切换）
                const bool mediaReset = (!mSeekSuppressResetOnce && lastPts != INT64_MIN &&
                                         packet->getInfo().pts < lastPts - 5 * 1000 * 1000);

                if (firstAnchor || mediaReset) {
                    mStreamStartTimeMap[streamIndex].time2ptsDelta = manifestDelta;

                    if (mStreamStartTimeMap[streamIndex].utcTime >= 0) {
                        mStreamStartTimeMap[streamIndex].utc2ptsDelta = mStreamStartTimeMap[streamIndex].utcTime - packet->getInfo().pts;
                    }
                }
            }

            mStreamStartTimeMap[streamIndex].seamlessPoint = false;
            mSeekSuppressResetOnce = false;      // 只对 seek 后的第一个分片点生效
        }

        if (packet->getInfo().duration > 0) {
            mStreamStartTimeMap[streamIndex].frameDuration = packet->getInfo().duration;
        }

        if (packet->getInfo().pts == INT64_MIN && mStreamStartTimeMap[streamIndex].lastFramePts != INT64_MIN &&
            mStreamStartTimeMap[streamIndex].frameDuration != INT64_MIN) {
            packet->getInfo().pts = mStreamStartTimeMap[streamIndex].lastFramePts + mStreamStartTimeMap[streamIndex].frameDuration;
        }

        if (packet->getInfo().pts != INT64_MIN && mStreamStartTimeMap[streamIndex].time2ptsDelta != INT64_MIN) {
            packet->getInfo().timePosition = packet->getInfo().pts + mStreamStartTimeMap[streamIndex].time2ptsDelta;
        } else {
            packet->getInfo().timePosition = INT64_MIN;
        }
        if (packet->getInfo().pts != INT64_MIN && mStreamStartTimeMap[streamIndex].utc2ptsDelta != INT64_MIN) {
            packet->getInfo().utcTime = packet->getInfo().pts + mStreamStartTimeMap[streamIndex].utc2ptsDelta;
        } else {
            packet->getInfo().utcTime = INT64_MIN;
        }

        if (packet->getInfo().pts != INT64_MIN) {
            mStreamStartTimeMap[streamIndex].lastFramePts = packet->getInfo().pts;
        }

        if (mIsStartSegment) {
            int64_t timePos = packet->getInfo().timePosition;
            if (timePos == INT64_MIN) {
                timePos = packet->getInfo().pts;
            }
            if (timePos >= 0 && mStreamStartTime >= 0 && mSuggestedPresentationDelay > 0) {
                if (timePos < af_get_utc_time() - mStreamStartTime - mSuggestedPresentationDelay) {
                    // AF_LOGD("setDiscard timePos = %lld", timePos);
                    packet->setDiscard(true);
                }
            }
        }

        /*
         * 【seek 落点延迟线】包的时间轴（timePosition）、关键帧标记都已经算完了，落点判定就放在
         * 这里 —— 在进入 mQueue 之前、也在上面那些既有处理之后，所以被延迟线收下的包不会打乱
         * time2ptsDelta / lastFramePts / seamlessPoint 的既有状态机。
         * 返回 false = 这一包被延迟线收下（本轮不产出，读线程立刻再来一轮，见 read_thread 的
         * EAGAIN 分支）；返回 true = 这一包照原样交出去。
         * 判据与边界见 demuxer/SeekLandingStage.h 的类注释（HLS 与 DASH 共用那一份实现）。
         */
        if (!mSeekLanding.filter(packet)) {
            packet = nullptr;
            return -EAGAIN;
        }

    }

    return ret;
}


int DashStream::GetNbStreams() const
{
    if (mPTracker->getStreamType() == STREAM_TYPE_MIXED) {//mediaPlayList
        if (mPDemuxer) {
            return mPDemuxer->GetNbStreams();
        }
    }

    return 1;// masterPlayList only support one es stream per stream url(ext-media-info ext-stream-info) now
}

int DashStream::GetStreamMeta(Stream_meta *meta, int index, bool sub) const
{
    // if not muxed stream ,get the 0 stream default index
    meta->type = (Stream_type) mPTracker->getStreamType();

    if (meta->type != STREAM_TYPE_MIXED) {
        index = OpenedStreamIndex;
    }

    uint64_t bandwidth;
    std::string lang;
    int width;
    int height;
    mPTracker->getStreamInfo(&width, &height, &bandwidth, lang);
    {
        std::lock_guard<std::mutex> lock(mHLSMutex);

        if (mPDemuxer) {
            mPDemuxer->GetStreamMeta(meta, index, sub);
        }
    }

    // meta->type would be override, recover it
    if (!sub) {
        meta->type = (Stream_type) mPTracker->getStreamType();
    }

    /*
     * 【宽高必须**各自**补齐，不能只看 height】
     *
     * 这里 meta 是"已开流（mPDemuxer 存在）"时才可能被底层填过：宽高来自解码器对
     * init 段/SPS 的解析。而**换档切换的窗口期**（新流 OpenStream 之后、第一个关键帧
     * 解出来之前）底层很可能只填了 height 没填 width（或者反过来）—— 老代码的条件是
     * `if (meta->height == 0)`：height 有值时整段跳过，于是 width 保持 0，上层拿到
     * `0 x 2160` 这种残值。后果有两个，用户都遇到过：
     *   * Qt 侧清晰度菜单按"宽x高"分档去重（platform/QtPlayer/src/CicadaPlayerItem.cpp
     *     的 onMediaInfoGetCb），`0x2160` 和 `3840x2160` 分不进同一档 → **同一清晰度出现两个**；
     *   * 标签/画幅比例跟着一起错。
     * 所以两个字段分别判断：缺哪个补哪个（MPD 里的值本来就来自清单属性，见
     * MPDParser.cpp:400-410 → Representation::getStreamInfo）。
     */
    if (meta->width <= 0) {
        meta->width = width;
    }

    if (meta->height <= 0) {
        meta->height = height;
    }

    /*
     * 【这一路 Representation 的编码：清单 @codecs -> AFCodecID】
     *
     * MPD 里 `@codecs`（MPDParser.cpp:417-418 → Representation::addCodecs）是
     * **每一路 rendition 各自**的编码声明，而 meta 在"这条流还没 OpenStream"时
     * 只有清单信息（mPDemuxer 为空）：不给它填这一项，应用层拿到的每一路流
     * 编码就都是空的，同分辨率不同编码根本没法区分。
     *
     * 只在底层没给出编码时才用清单的值填 —— 已开流时 mPDemuxer 的编码来自
     * init 段/SPS 解析，是容器里的**真值**，绝不能覆盖（弱一点说：换档窗口期
     * 底层可能只填了宽高，此时清单值正好补上）。
     */
    if (meta->codec == AF_CODEC_ID_NONE) {
        meta->codec = afCodecIDFromManifestCodecs(mPTracker->getCodecsString().c_str());
    }

    meta->lang = strdup(lang.c_str());
    meta->bandwidth = bandwidth;
    meta->duration = mPTracker->isLive() ? 0 : mPTracker->getDuration();

    if (!mPTracker->getDescriptionInfo().empty()) {
        meta->description = strdup(mPTracker->getDescriptionInfo().c_str());
    }

    meta->suggestedPresentationDelay = mPTracker->getLiveDelay();

    return 0;
}

bool DashStream::isOpened()
{
    return mIsOpened;
}

int DashStream::start()
{
    //        demuxer_msg::StartReq start;
    //        mPProxyService->SendMsg(start, mPDemuxer->GetAddr(), false);
    mIsOpened = true;
    mIsEOS = false;
    mIsDataEOS = false;
    mStopOnSegEnd = false;
    mError = 0;

    if (mThreadPtr == nullptr) {
        mThreadPtr = NEW_AF_THREAD(read_thread);
    }

    mThreadPtr->start();
    return 0;
}

int DashStream::preStop()
{
    std::unique_lock<std::mutex> waitLock(mDataMutex);
    mExited = true;
    mWaitCond.notify_one();
    return 0;
}

int DashStream::stop()
{
    AF_TRACE;

    if (mThreadPtr) {
        AF_TRACE;
        interrupt_internal(1);
        mWaitCond.notify_one();
        AF_TRACE;
        mThreadPtr->stop();
        AF_TRACE;
        interrupt_internal(mInterrupted);
        AF_TRACE;
    }

    resetSource();
    {
        std::lock_guard<std::mutex> lock(mHLSMutex);

        if (mPdataSource) {
            mPdataSource->Close();
            delete mPdataSource;
            mPdataSource = nullptr;
        }

        if (mSegKeySource) {
            mSegKeySource->Close();
            delete mSegKeySource;
            mSegKeySource = nullptr;
        }

        mIsOpened_internal = false;
    }
    clearDataFrames();
    AF_LOGD("%s\n", __func__);
    return 0;
}

// TODO: deal seek in read_thread
int64_t DashStream::seek(int64_t us, int flags)
{
    (void) flags;
    uint64_t num = 0;
    auto usSought = (uint64_t) us;
    bool reqReOpen = true;
    AF_LOGD("%s:%d stream (%d) seek us is %lld\n", __func__, __LINE__, mPTracker->getStreamType(), us);

    if (!mPTracker->isInited()) {
        mSeekPendingUs = us;
        AF_LOGI("pending seek\n");
        return us;
    }

    //   int ret = mPTracker->init();
    bool b_ret = mPTracker->getSegmentNumberByTime(usSought, num);

    /*
     * 【seek 落点修复 / 回退点 A】把返回值从"请求值"改成"tracker 真正选中的分片起点"。
     *
     * 为什么：DashManager::seek() 的既有设计是"2. 先 seek 视频、取回 seekedUs；
     * 3. 用 seekedUs 去 seek 其余流"（DashManager.cpp:481-512），它**完全依赖本函数的返回值**。
     * 而本函数原来返回 usSought = 请求值（下面 :1153 一带的注释自己就写明"usSeeked 打的是请求值"），
     * 于是第 3 步退化成"音频也 seek 到同一个请求目标" ⇒ 音频落在**音频自己的分片网格**、
     * 视频落在**视频自己的分片网格**。实测 output.mpd（视频片 19.9866s、音频片 9.984s）：
     * 目标 56.141s 时视频落点 39.973s、音频落点 49.92s ⇒ 音频首帧超前主时钟 10.03s，
     * SuperMediaPlayer 的"音频超前就 hold"门（reason=1）静音整整 10s（用户报的"声音断一下再回来"）。
     *
     * 改成返回分片起点后：视频返回 39.973s ⇒ 第 3 步把音频 seek 到 39.973s ⇒ 音频落到
     * 包含它的音频分片起点 39.936s（略**早于**视频落点，正是安全方向：音频不超前、
     * 那道门不会被触发）。时间轴与视频包 timePosition / 播放器侧 landing 同一把尺子
     * （见本文件 :846 `timePosition = mCurSeg->fixedStartTime`）。
     *
     * 只改这一条语义：不动请求目标、不动落点/位置上报（内核上报位置由不连续点基准给出，不再有地板）；
     * 消费方安全性已核：Seek() 的返回值只被用作 `ret < 0` 错误判断
     * （SMPMessageControllerListener.cpp:733-737、SuperMediaPlayer.cpp:8837-8847，
     * 另两处直接丢弃返回值），且 DASH 多流路径的 manager 返回值恒为 0（DashManager.cpp:518）。
     * 回退：删掉 landingUs 的计算、把末尾 `return landingUs;` 换回 `return usSought;`。
     */
    int64_t landingUs = us;

    if (b_ret && mPTracker->getCurrentRepresentation() != nullptr) {
        const int64_t segStartUs = mPTracker->getCurrentRepresentation()->getMediaSegmentStartTime(num);

        /* 取不到（<0）就退回请求值：绝不让本节返回值变成负数 ——
         * DashManager 第 3 步把负值当"seek 失败"（DashManager.cpp:504-506）。 */
        if (segStartUs >= 0) {
            landingUs = segStartUs;
        }
    }

    if (!b_ret) {
        AF_LOGE("(%d)getSegmentNumberByTime error us is %lld\n", mPTracker->getStreamType(), us);
        // us's accuracy is ms, so change duration's accuracy to ms
        bool seekOnLast = false;
        if (us >= (mPTracker->getDuration() / 1000 * 1000)) {
            num = mPTracker->getLastSegNum();

            // reopen will -- it
            if (mIsOpened_internal) {
                num++;
            }
            usSought = us;
            seekOnLast = true;
        }

        if (!seekOnLast) {
            if (mPTracker->getStreamType() == STREAM_TYPE_SUB) {
                mIsEOS = false;
                mError = 0;

                if (mThreadPtr) {
                    mThreadPtr->start();
                }
            } else {
                return -1;
            }
        }
    }

    AF_LOGD("%s:%d stream (%d) usSeeked is %lld seek num is %d\n", __func__, __LINE__, mPTracker->getStreamType(), usSought, num);

    /*
     * 【落点诊断】同 HLSStream::seek 里的说明：usSeeked 打的是**请求值**，
     * tracker 真正选中的段号在这里才是权威读数。DASH 实测里
     * "seek us is 17835000 / pending seek" 之后读取线程一路跑到 getCurSegNum=7，
     * 需要这一行来区分"seek 没生效"和"seek 生效后又被读取线程跑远"。
     */
    AF_LOGI("[seek] dash %s: reqUs=%lld -> segNum=%llu curSegNum=%llu duration=%lld live=%d initialized=%d\n",
            mPTracker->getStreamType() == STREAM_TYPE_VIDEO ? "video" : "audio/other",
            (long long) usSought, (unsigned long long) num,
            (unsigned long long) mPTracker->getCurSegNum(),
            (long long) mPTracker->getDuration(), (int) mPTracker->isLive(),
            (int) mPTracker->isInited());

    if (mPTracker->getStreamType() == STREAM_TYPE_SUB && num == mPTracker->getCurSegNum()) {
        AF_LOGW("only one  subtitle seg");
        reqReOpen = false;
        seek_internal(num, us);
    }

    {
        std::unique_lock<std::mutex> waitLock(mDataMutex);
        mSwitchNeedBreak = true;
    }

    mWaitCond.notify_one();
    interrupt_internal(1);

    if (mThreadPtr) {
        mThreadPtr->pause();
    }

    interrupt_internal(mInterrupted);
    mSwitchNeedBreak = false;
    clearDataFrames();

    if (reqReOpen) {
        resetSource();

        if (mIsOpened_internal) {
            mReopen = true;
        }

        /*
         * 【日志勘误，不改行为】这里打出的 segNum 是 tracker 的"游标"，而读取线程取下一段时
         * 走的是 DashSegmentTracker::getNextSegment()，它**先 ++mCurrentSegNumber 再取段**
         * （DashSegmentTracker.cpp:149）。于是 `setCurSegNum(num - 1)` 配上那次 ++ 正好读
         * 第 num 段 —— 也就是**包含请求时刻的那一段**，这是对的。
         * 但旧日志只打游标值，看上去就像"请求第 5 段却定位到第 4 段"（真机排查时被误读成
         * 落点偏了一个分片）。所以这里把"第一段实际会读哪一段"一并打出来，避免下一次再被误读。
         * 判据仍是纯状态（游标值 + 1 = num），不引入任何计时器 / 阈值。
         */
        mPTracker->setCurSegNum(num - 1);
        AF_LOGI("[seek] dash %s: tracker cursor set to segNum=%llu (reqUs=%lld, reopened) — the read thread "
                "advances with ++, so the FIRST segment read is segNum=%llu (this must be the segment that "
                "CONTAINS the request; landing at its head IDR is intended)\n",
                mPTracker->getStreamType() == STREAM_TYPE_VIDEO ? "video" : "audio/other",
                (unsigned long long) mPTracker->getCurSegNum(), (long long) usSought,
                (unsigned long long) num);
    }

    /*
     * 【seek 落点延迟线：装弹】位置很关键：
     *   · 必须在上面 clearDataFrames() 之后 —— 那一刻读线程已经 pause()，不会再写延迟线的 stage；
     *   · 必须在下面 mThreadPtr->start() 之前 —— 线程一启动就可能立刻产出第一个包。
     * 目标用请求值 us（不是 landingUs）：延迟线要找的是"玩家要的那一帧"所在的 GOP，
     * 分片起点只是解码起点，不能拿它当目标（否则落点会退回到"最后一个 ≤ 分片起点"的关键帧）。
     */
    mSeekLanding.arm(us, "DASH", mPTracker);

    mIsEOS = false;
    mIsDataEOS = false;
    mError = 0;
    if (mDemuxerMeta) {
        mDemuxerMeta->bContinue = false;
    }

    /*
     * 【跨 seek 保持演示轴】seek 不是"媒体时间戳不连续"，只是换了个读取位置：
     * 让 seek 后的第一个分片点不参与 readPacket 里"媒体重置"的判断，于是
     * time2ptsDelta（等价于 −PTO 的那个常量）跨 seek 保持不变 —— 否则 MPD 的
     * 名义段长误差会在每次 seek 时被当成新锚点灌进来（音画越播越偏的根因）。
     * 这里只置一个开关，不动 lastFramePts/frameDuration，避免影响"pts 缺失时用
     * lastFramePts + frameDuration 补值"的兜底分支。
     * 回退：删掉下面这一行赋值即可恢复"每次 seek 重锚"的老行为。
     */
    mSeekSuppressResetOnce = true;

    if (mThreadPtr) {
        mThreadPtr->start();
    }

    /* 【seek 落点修复 / 回退点 A】原来是 `return usSought;`（请求值）—— 见本函数开头那段说明 */
    return landingUs;
}

int64_t DashStream::seek_internal(uint64_t segNum, int64_t us)
{
    (void) segNum;

    // subtitle need us to find frame index to read
    //TODO: use flush to flash all
    if (mPDemuxer) {
        mPDemuxer->Seek(us, 0, -1);
        mPDemuxer->flush();
    }

    return 0;
}

uint64_t DashStream::getCurSegNum()
{
    return mPTracker->getCurSegNum();
}

int DashStream::stopOnSegEnd(bool stop)
{
    mStopOnSegEnd = stop;
    return 0;
}

int DashStream::SetCurSegNum(uint64_t num)
{
    return reopenSegment(num, OpenType::SegNum);
}

int DashStream::reopenSegment(uint64_t num, OpenType openType)
{
    {
        std::unique_lock<std::mutex> waitLock(mDataMutex);
        mSwitchNeedBreak = true;
    }

    mWaitCond.notify_one();

    if (mThreadPtr) {
        mThreadPtr->pause();
    }

    mSwitchNeedBreak = false;
    clearDataFrames();
    /*
     * 【seek 落点延迟线】reopenSegment 是"按分片号/分片位置重设读位置"（切档交接、SetCurSegNum 等），
     * 它**没有时间目标**：上一次 seek 留下的目标到这里已经不能代表现在要读的位置，必须连目标一起清。
     * 否则一个陈旧的、恰好落在前方的目标会让延迟线丢掉这个分片的前缀。
     */
    mSeekLanding.reset();
    resetSource();

    if (mIsOpened_internal) {
        mReopen = true;
        num--;
    }

    if (openType == OpenType::SegNum) {
        mPTracker->setCurSegNum(num);
        AF_LOGD("setCurSegNum %llu\n", num);
    } else if (openType == OpenType::SegPosition) {
        mPTracker->setCurSegPosition(num);
        AF_LOGD("setCurSegPosition %llu\n", num);
    }

    seek_internal(num, 0);
    mIsEOS = false;
    mIsDataEOS = false;
    mError = 0;

    if (mThreadPtr) {
        mThreadPtr->start();
    }

    return 0;
}

uint64_t DashStream::getCurSegPosition()
{
    return mPTracker->getCurSegPosition();
};

int DashStream::setCurSegPosition(uint64_t position)
{
    return reopenSegment(position, OpenType::SegPosition);
}

bool DashStream::isLive()
{
    return mPTracker->isLive();
}

int64_t DashStream::getDuration()
{
    return mPTracker->getDuration();
}

int DashStream::getNBStream() const
{
    if (mPDemuxer) {
        return mPDemuxer->GetNbStreams();
    }

    return 1;
}

void DashStream::interrupt(int inter)
{
    {
        std::unique_lock<std::mutex> waitLock(mDataMutex);
        mInterrupted = static_cast<bool>(inter);
    }
    interrupt_internal(inter);
}

void DashStream::interrupt_internal(int inter)
{
    {
        std::lock_guard<std::mutex> lock(mHLSMutex);

        if (mSegKeySource) {
            mSegKeySource->Interrupt(static_cast<bool>(inter));
        }

        if (mPdataSource) {
            mPdataSource->Interrupt(static_cast<bool>(inter));
        }

        if (mExtDataSource) {
            mExtDataSource->Interrupt(static_cast<bool>(inter));
        }
    }
    {
        std::lock_guard<std::mutex> lock(mHLSMutex);

        if (mPDemuxer) {
            mPDemuxer->interrupt(inter);
        }
    }

    if (mPTracker) {
        mPTracker->interrupt(inter);
    }
}

std::string DashStream::GetProperty(const string &key)
{
    if ("openJsonInfo" == key) {
        return openInfoArray.printJSON();
    } else if ("responseInfo" == key) {
        std::lock_guard<std::mutex> lock(mHLSMutex);

        if (mPdataSource) {
            return mPdataSource->GetOption(key);
        }

        return "";
    } else if ("probeInfo" == key) {
        std::lock_guard<std::mutex> lock(mHLSMutex);

        if (mPDemuxer) {
            return mPDemuxer->GetProperty(-1, key);
        }
    }

    return "";
}

bool DashStream::isRealTimeStream()
{
    if (mPTracker != nullptr) {
        return mPTracker->isRealTimeStream();
    } else {
        return false;
    }
}

void DashStream::setPreferAudio(bool preferAudio)
{
    mPreferAudio = preferAudio;
    AF_LOGI("DashStream %d, setPreferAudio, %ld", mId, (int32_t)preferAudio);
}
