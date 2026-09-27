//
// Created by moqi on 2018/4/28.
//
#define LOG_TAG "HLSStream"

#include "HLSStream.h"
#include "Helper.h"
#include "data_source/dataSourcePrototype.h"
#include "demuxer/DemuxerMeta.h"
#include "demuxer/IDemuxer.h"
#include "demuxer/play_list/segment_decrypt/AES_128Decrypter.h"
#include "segment.h"
#include "segment_decrypt/SegDecryptorFactory.h"
#include "utils/DrmUtils.h"
#include "utils/af_string.h"
#include "utils/errors/framework_error.h"
#include "utils/frame_work_log.h"
#include "utils/mediaFrame.h"
#include "utils/timer.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>

// TODO support active and no active mode

namespace Cicada {

    static const int defaultInitSegSize = 1024 * 1024;

    const char *HLSStream::hls_id3 = "id3v2_priv.com.apple.streaming.transportStreamTimestamp";

    HLSStream::HLSStream(SegmentTracker *pTracker, int id)
        : mPTracker(pTracker),
          mId(id)
    {
    }

    HLSStream::~HLSStream()
    {
        close();
        delete mThreadPtr;
        delete mPTracker;
        mStreamStartTimeMap.clear();

        if (mInitSegBuffer) {
            free(mInitSegBuffer);
        }
    }

    int HLSStream::getStreamType() const
    {
        return mPTracker->getStreamType();
    }

    int HLSStream::GetRemainSegmentCount()
    {
        return mPTracker->GetRemainSegmentCount();
    }

    int HLSStream::Decrypter_read_callback(void *arg, uint8_t *buffer, int size)
    {
        auto *pHandle = static_cast<HLSStream *>(arg);

        if (pHandle->mExtDataSource) {
            return pHandle->mExtDataSource->Read(buffer, (size_t) size);
        }

        return pHandle->mPdataSource->Read(buffer, (size_t) size);
    }

    int HLSStream::read_callback(void *arg, uint8_t *buffer, int size)
    {
        auto *pHandle = static_cast<HLSStream *>(arg);
        int ret;

        if (pHandle->mInterrupted) {
            return -EIO;
        }

        if (pHandle->mIsDataEOS) {
            AF_LOGE("%s : %d stream(%d),EOS", __func__, __LINE__,
                    pHandle->mPTracker->getStreamType());
            return 0;
        }

        if (pHandle->mInitSegBuffer) {
            int initSegSize = pHandle->mInitSegSize - pHandle->mInitSegPtr;

            if (initSegSize > 0) {
                int readSize = std::min(initSegSize, size);
                memcpy(buffer, pHandle->mInitSegBuffer + pHandle->mInitSegPtr, readSize);
                pHandle->mInitSegPtr += readSize;
                if (readSize < 0) {
                    AF_LOGE("HLSStream::read_callback 1 ret=%d, size=%d", readSize, size);
                }
                return readSize;
            }
        }

        ret = pHandle->readSegment(buffer, size);
        if (ret == 0 && !pHandle->mReopen) {
            MoveToNextPart move_ret = pHandle->moveToNextPartialSegment();
            if (move_ret == MoveToNextPart::moveSuccess) {
                ret = pHandle->readSegment(buffer, size);
                if (ret < 0) {
                    AF_LOGE("HLSStream::read_callback 2 ret=%d, size=%d", ret, size);
                }
                return ret;
            } else if (move_ret == MoveToNextPart::tryAgain) {
                int tryTimes = 150;
                while (tryTimes > 0 && !pHandle->mInterrupted) {
                    af_msleep(20);
                    pHandle->mPTracker->reLoadPlayList();
                    MoveToNextPart move_ret = pHandle->moveToNextPartialSegment();
                    if (move_ret == MoveToNextPart::moveSuccess) {
                        ret = pHandle->readSegment(buffer, size);
                        if (ret < 0) {
                            AF_LOGE("HLSStream::read_callback 3 ret=%d, size=%d", ret, size);
                        }
                        return ret;
                    } else if (move_ret == MoveToNextPart::segmentEnd) {
                        return 0;
                    }
                    --tryTimes;
                };
                return 0;
            } else {
                return move_ret;
            }
        }

        if (pHandle->getStreamType() == STREAM_TYPE_SUB && pHandle->mVttPtsOffSet == INT64_MIN && ret > 0) {
            pHandle->mVttPtsOffSet = pHandle->mWVTTParser.addBuffer(buffer, ret);

            if (pHandle->mVttPtsOffSet != INT64_MIN) {
                AF_LOGD("WVTTParser pts is %lld\n", pHandle->mVttPtsOffSet);
            }
        }
        if (ret < 0) {
            AF_LOGE("HLSStream::read_callback 4 ret=%d, size=%d", ret, size);
        }

        return ret;
    }

    int HLSStream::readSegment(const uint8_t *buffer, int size)
    {
        int ret = 0;

        if (mSegDecrypter == nullptr) {
            if (mExtDataSource) {
                ret = mExtDataSource->Read((void *) buffer, size);
            } else if (mPdataSource) {
                ret = mPdataSource->Read((void *) buffer, (size_t) size);
            } else {
                AF_LOGE("HLSStream::readSegment, no dataSource");
            }
        } else {
            ret = mSegDecrypter->Read(const_cast<uint8_t *>(buffer), size);
        }

        return ret;
    }

    MoveToNextPart HLSStream::moveToNextPartialSegment()
    {
        auto curSeg = mPTracker->getCurSegment(false);
        if (curSeg && curSeg->mSegType == SEG_LHLS) {
            bool bHasUnusedParts = false;
            bool downloadComplete = curSeg->isDownloadComplete(bHasUnusedParts);
            if (bHasUnusedParts) {
                curSeg->moveToNextPart();
                AF_LOGD("[hls][lhls] moveToNextPart, uri=%s", curSeg->getDownloadUrl().c_str());
                int ret = tryOpenSegment(curSeg);
                if (ret < 0) {
                    return MoveToNextPart::segmentEnd;
                }
                return MoveToNextPart::moveSuccess;
            } else {
                if (downloadComplete) {
                    return MoveToNextPart::segmentEnd;
                } else if (mPTracker->hasPreloadSegment()) {
                    std::string segmentUri;
                    int64_t rangeStart, rangeEnd;
                    mPTracker->usePreloadSegment(segmentUri, rangeStart, rangeEnd);
                    AF_LOGD("[lhls] use partial segment of preload hint, uri=%s", segmentUri.c_str());
                    std::string uri = Helper::combinePaths(mPTracker->getBaseUri(), segmentUri);
                    int ret = tryOpenSegment(uri, rangeStart, rangeEnd);
                    AF_LOGD("[lhls] use partial segment of preload hint, ret=%d", ret);
                    if (ret < 0) {
                        return MoveToNextPart::tryAgain;
                    }
                    return MoveToNextPart::moveSuccess;
                } else {
                    return MoveToNextPart::tryAgain;
                }
            }
        }
        return MoveToNextPart::segmentEnd;
    }

    int64_t HLSStream::seekSegment(off_t offset, int whence)
    {
        int64_t ret;

        if (mSegDecrypter == nullptr) {
            if (mExtDataSource) {
                ret = mExtDataSource->Seek(offset, whence);
            } else {
                ret = mPdataSource->Seek(offset, whence);
            }
        } else {
            // ret = mSegDecrypter->Read(const_cast<uint8_t *>(buffer), size);
            return -EINVAL;
        }

        return ret;
    }

//    off_t HLSStream::seek_callback(void *arg, off_t offset, int whence) {
////        HLSStream *pHandle = static_cast<HLSStream *>(arg);
////        AF_LOGE("test", "%s %lld \n", __func__, offset);
////        if (pHandle->mSeekCb)
////            return pHandle->mSeekCb(pHandle->mSeekArg, offset, whence);
////        return mPDataSource->Seek(offset, whence);
//        return 0;
//    }

    int HLSStream::open()
    {
//        mIsOpened = true;
//        mStatus = status_paused;
//        mIsEOS = false;
//        mIsDataEOS = false;
//        mStopOnSegEnd = false;
//        pthread_create(&read_thread_id, NULL, read_thread, this);
        return 0;
    }

    int HLSStream::upDateInitSection()
    {
        int ret;
        mInitSegPtr = 0;

        if (!mCurSeg || !mCurSeg->init_section || (mCurInitSeg != nullptr && mCurInitSeg->mUri == mCurSeg->init_section->mUri)) {
            return 0;
        }

        ret = tryOpenSegment(mCurSeg->init_section);

        if (ret < 0) {
            return ret;
        }

        mCurInitSeg = mCurSeg->init_section;
        mInitSegSize = defaultInitSegSize;
        mInitSegSize = seekSegment(0, SEEK_SIZE);

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
        return size;
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

    int HLSStream::open_internal()
    {
        int ret;
        AF_LOGD("mPTracker type is %d\n", mPTracker->getStreamType());
        //  mPTracker->setCurSegNum(0);
        uint64_t targetPosition = mPTracker->getCurSegPosition();
        uint64_t targetSegNum = mPTracker->getCurSegNum();
        bool useExt = false;
        if (mExtDataSource && !mPTracker->isInited()) {
            mPTracker->setExtDataSource(mExtDataSource);
            useExt = true;
        }
        ret = mPTracker->init();
        if (useExt) {
            mPTracker->setExtDataSource(nullptr);
        }
        if (useExt) {
            if (mPTracker->isLive()) {
                mExtDataSource->enableCache(mExtDataSource->GetOriginUri(), false);
            } else {
                mExtDataSource->enableCache(mExtDataSource->GetOriginUri(), true);
            }
        }

        if (ret < 0) {
            AF_TRACE;
            return ret;
        }

        if (mPTracker->isLive() && mPTracker->isSeeked()) {
            uint64_t firstSegNum = mPTracker->getFirstSegNum();
            uint64_t lastSegNum = mPTracker->getLastSegNum();
            uint64_t segSize = mPTracker->getSegSize();
            AF_LOGD("targetNum = %llu , firstSegNum = %llu , lastSegNum = %lld", targetSegNum, firstSegNum, lastSegNum);

            uint64_t useSegNumMin = 0;
            if (firstSegNum > (segSize + 1) / 2 - 1) {
                // we always request the next fragment, so enlarge the max segnum by minus 1
                useSegNumMin = firstSegNum - (segSize + 1) / 2 - 1;
            }
            uint64_t useSegNumMax = lastSegNum + (segSize + 1) / 2;

            if (targetSegNum >= useSegNumMin && targetSegNum <= useSegNumMax) {

                if (targetSegNum >= firstSegNum && targetSegNum <= lastSegNum) {
                    AF_LOGD("match seg no , curSegNum  = %llu", targetSegNum);
                } else if (targetSegNum >= useSegNumMin && targetSegNum < firstSegNum) {
                    AF_LOGW("tracker newer than target num");
                } else if (targetSegNum > lastSegNum && targetSegNum <= useSegNumMax) {
                    AF_LOGW("tracker older than target num, refresh playlist may catch up , curSegNum  = %llu", targetSegNum);
                }

                mPTracker->setCurSegPosition(0);
                mPTracker->setCurSegNum(targetSegNum);
            } else {
                // segNo may not be aligned, try use position to open
                mPTracker->setCurSegPosition(targetPosition);
                AF_LOGW("segNo may not be aligned, try use position to open, setCurSegPosition = %llu", targetPosition);
            }
        }

        //      mPTracker->print();
//        if (mPTracker->getStreamType() == STREAM_TYPE_SUB && mPTracker->getSegSize() == 1) {
//            AF_LOGW("only one  subtitle seg");
//            //   mPTracker->setCurSegNum(0);
//            mSeekPendingUs = -1;
//        }

        if (mSeekPendingUs >= 0) {
            uint64_t num = 0;
            auto usSeeked = (uint64_t) mSeekPendingUs;
            bool b_ret = mPTracker->getSegmentNumberByTime(usSeeked, num);

            if (!b_ret) {
                AF_LOGE("getSegmentNumberByTime error us is %lld\n", mSeekPendingUs);
            } else {
                AF_LOGI("%s:%d stream (%d) usSeeked is %lld seek num is %d\n", __func__, __LINE__,
                        mPTracker->getStreamType(), usSeeked, num);
                mPTracker->setCurSegNum(num);
            }

            /*
             * 【seek 落点延迟线：装弹（延后型 seek）】tracker 还没 init 时，seek() 只把目标存进
             * mSeekPendingUs（那一刻读线程可能正跑在 open_internal 里，不能去动延迟线的 stage）。
             * 这里才是"真正定位到分片"的那一刻，而且本函数跑在读线程上：装弹之后紧接着才会产出
             * 第一个包，不存在与读线程并发写延迟线的问题。
             */
            mSeekLanding.arm(mSeekPendingUs, "HLS", mPTracker);
            mSeekPendingUs = -1;
        }

        //        AF_TRACE;
        //        AF_LOGD("mPTracker->getCurSegNum is %llu", mPTracker->getCurSegNum());
        //        AF_LOGD("getFirstSegNum is %llu\n", mPTracker->getFirstSegNum());
        //        AF_LOGD("getSegSize is %llu\n", mPTracker->getSegSize());

        if (mPTracker->getCurSegNum() < mPTracker->getFirstSegNum() && mPTracker->isLive()) {
            AF_LOGW("skip seg %llu -->%llu", mPTracker->getCurSegNum(),
                    mPTracker->getFirstSegNum());
            mPTracker->setCurSegNum(mPTracker->getFirstSegNum());
        }

        if (mPTracker->isLive() && !mPTracker->isSeeked()) {
            if (mOpts) {
                string value = mOpts->get("liveStartIndex");
                if (!value.empty()) {
                    mLiveStartIndex = atoll(value.c_str());
                    AF_LOGI("set liveStartIndex to %lld\n", mLiveStartIndex);
                }
            }
            mPTracker->MoveToLiveStartSegment(mLiveStartIndex);
        }

        mStopOnSegEnd = false;
        mCurSeg = nullptr;
        mCurSeg = mPTracker->getCurSegment(true);
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

            ret = upDateInitSection();

            if (ret < 0) {
                return ret;
            }

            AF_LOGD("open uri is %s seq is %llu\n", mCurSeg->getDownloadUrl().c_str(), mCurSeg->sequence);
            ret = tryOpenSegment(mCurSeg);

            if (isHttpError(ret)) {
                resetSource();
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

    int64_t HLSStream::getPackedStreamPTS()
    {
        Source_meta *meta = nullptr;
        mPDemuxer->GetSourceMeta(&meta);
        int64_t pts = INT64_MIN;
        Source_meta *meta1 = meta;

        while (meta1 != nullptr) {
            if (meta1->key && meta1->value) {
//                AF_LOGD("%s:[%s]", meta1->key, meta1->value);
                int ptr = 0;

                if (strcmp(meta1->key, hls_id3) == 0) {
                    uint8_t buf[8];
                    int v;

                    for (unsigned char &i : buf) {
                        if (sscanf(meta1->value + ptr, "\\x%02x", &v) == 1) {
                            ptr += 4;
                            i = v;
                        } else {
                            i = *(meta1->value + ptr);
                            ptr++;
                        }
                    }

                    uint64_t ps = getSize(buf, 8, 8);
                    pts = ps * 1000 / 90;
                    //                   AF_LOGD("ps is %u\n", ps);
                }
            }

            meta1 = meta1->next;
        }

        releaseSourceMeta(meta);
        return pts;
    }

    int HLSStream::createDemuxer()
    {
        int ret;
        mError = 0;
        mDataSourceError = 0;

        if (mDemuxerMeta && mDemuxerMeta->id != mCurSeg->discontinuityNum) {
            mDemuxerMeta = nullptr;
        }

        for (SegmentEncryption &item: mCurSeg->encryptions) {
            if (item.keyFormat.empty() || DrmUtils::isSupport(item.keyFormat)) {
                mCurrentEncryption = item;
                break;
            }
        }

        ret = updateDecrypter();

        if (ret < 0) {
            return ret;
        }

        if (getStreamType() == STREAM_TYPE_SUB) {
            mWVTTParser.rest();
            mVttPtsOffSet = INT64_MIN;
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
        mPDemuxer->setSampleDecryptor(this->mSampeAesDecrypter.get());
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
                mDemuxerMeta->id = mCurSeg->discontinuityNum;
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

                if ((trackerType == STREAM_TYPE_MIXED && subType != STREAM_TYPE_UNKNOWN &&
                     mClosedSubStreams.find(i) == mClosedSubStreams.end()) ||
                    subType == trackerType) {
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

    int HLSStream::tryOpenSegment(const string &uri, int64_t start, int64_t end)
    {
        AF_LOGD("tryOpenSegment: %s(%lld,%lld)\n", uri.c_str(), start, end);
        mSegmentOpened = false;
        int retryTimes = 0;
        int ret;

        do {
            resetSource();
            ret = openSegment(uri, start, end);
            retryTimes++;
            AF_LOGD("openSegment ret=%d retryTimes=%d \n", ret, retryTimes);

            if (ret >= 0 || retryTimes > 2) {
                break;
            }

            af_msleep(20);
        } while (isHttpError(ret) && !mInterrupted);

        if (ret >= 0) {
            mSegmentOpened = true;
        }
        return ret;
    }

    int HLSStream::tryOpenSegment(std::shared_ptr<segment> seg)
    {
        std::string uri = Helper::combinePaths(mPTracker->getBaseUri(), seg->getDownloadUrl());
        int64_t rangeStart, rangeEnd;
        seg->getDownloadRange(rangeStart, rangeEnd);
        return tryOpenSegment(uri, rangeStart, rangeEnd);
    }

    int HLSStream::openSegment(const string &uri, int64_t start, int64_t end)
    {
        int ret;

        if (mExtDataSource) {
            if (mIsFirstOpen) {
                mIsFirstOpen = false;
                if (!mPTracker->isLive()) {
                    mExtDataSource->setSegmentList(getSegmentList());
                }
            }
            mExtDataSource->setRange(start, end);
            int ret = mExtDataSource->Open(uri);
            if (mPTracker->getStreamType() == STREAM_TYPE_MIXED && !mPTracker->isLive()) {
                mExtDataSource->enableCache(uri, true);
            } else {
                mExtDataSource->enableCache(uri, false);
            }
            return ret;
        }

        if (mPdataSource == nullptr) {
            recreateSource(uri);
            mPdataSource->setRange(start, end);
            ret = mPdataSource->Open(0);
        } else {
            mPdataSource->setRange(start, end);
            ret = mPdataSource->Open(uri);
            if (mPTracker->getStreamType() == STREAM_TYPE_MIXED && !mPTracker->isLive()) {
                mPdataSource->enableCache(uri, true);
            } else {
                mPdataSource->enableCache(uri, false);
            }
        }

        return ret;
    }

    void HLSStream::resetSource()
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

    void HLSStream::recreateSource(const string &url)
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
        if (mPTracker->getStreamType() == STREAM_TYPE_MIXED && !mPTracker->isLive()) {
            mPdataSource->enableCache(url, true);
        } else {
            mPdataSource->enableCache(url, false);
        }
    }

    void HLSStream::clearDataFrames()
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
         * 用 DropStage 而不是 Reset：**seek 目标要留着**。stop()（含 HLSManager 的关流/切档
         * 交接）之后重新 start() 会在同一个分片上重开，目标依然有效；旧的写法把目标一起清掉，
         * "装了弹又被 stop 抹掉"的那些 seek 就悄悄退回分片首。目标过期的情况由 SeekLandingStage::filter
         * 的"第一包 pos > 目标 就放弃"兜住，不会误丢数据。真正需要连目标一起清的只有
         * reopenSegment（位置按分片号重设，见那里的 Reset）。
         */
        mSeekLanding.dropStage();
    }


    bool HLSStream::updateKey()
    {
        string keyUrl = Helper::combinePaths(mPTracker->getBaseUri(),
                                             mCurrentEncryption.keyUrl);

        if (mKeyUrl == keyUrl) {
            return false;
        }

        mKeyUrl = keyUrl;
        {
            std::lock_guard<std::mutex> lock(mHLSMutex);
            delete mSegKeySource;
            mSegKeySource = dataSourcePrototype::create(keyUrl, mOpts);
            mSegKeySource->Set_config(mSourceConfig);
        }
        int ret = mSegKeySource->Open(0);

        if (ret < 0) {
            AF_LOGE("open key file error\n");
            return ret;
        }

        int64_t size = 0;

        while (size < 16) {
            int len = mSegKeySource->Read(mKey + size, (size_t) (16 - size));

            if (len > 0) {
                size += len;
            } else {
                break;
            }
        }

        if (size != 16) {
            AF_LOGE("key size is %d not 16\n", size);
//                    delete mSegKeySource;
            mSegKeySource->Close();
            return -1;
        }

        mSegKeySource->Close();
        return true;
    }

    bool HLSStream::updateIV()
    {
        if (!mCurrentEncryption.ivStatic) {
            mCurrentEncryption.iv.clear();
            mCurrentEncryption.iv.resize(16);
            int number = (int) mCurSeg->getSequenceNumber();
            mCurrentEncryption.iv[15] = static_cast<unsigned char>(
                                             (number /* - segment::SEQUENCE_FIRST*/) & 0xff);
            mCurrentEncryption.iv[14] = static_cast<unsigned char>(
                                             ((number /* - segment::SEQUENCE_FIRST*/) >> 8) & 0xff);
            mCurrentEncryption.iv[13] = static_cast<unsigned char>(
                                             ((number/* - segment::SEQUENCE_FIRST*/) >> 16) & 0xff);
            mCurrentEncryption.iv[12] = static_cast<unsigned char>(
                                             ((number /* - segment::SEQUENCE_FIRST*/) >> 24) & 0xff);
            return true;
        }

        return false;
    }

    int HLSStream::updateSegDecrypter()
    {
        if (mCurrentEncryption.method == SegmentEncryption::AES_128) {
            if (updateKey()) {
                if (mSegDecrypter == nullptr)
                    mSegDecrypter = unique_ptr<ISegDecrypter>(
                                        SegDecryptorFactory::create(mCurrentEncryption.method, Decrypter_read_callback, this));

                mSegDecrypter->SetOption("decryption key", mKey, 16);
            }

            if (updateIV()) {
                mSegDecrypter->SetOption("decryption IV", &mCurrentEncryption.iv[0], 16);
            }

            mSegDecrypter->flush();

            if (mDRMMagicKey.empty() && mSegKeySource){
                mDRMMagicKey = mSegKeySource->GetOption("drmMagicKey");
            }
        } else if (mCurrentEncryption.method == SegmentEncryption::AES_PRIVATE) {
            memset(mKey, 0, 16);
            long length = mCurrentEncryption.keyUrl.length();

            if (length > 16) {
                length = 16;
            }

            memcpy(mKey, mCurrentEncryption.keyUrl.c_str(), length);

            if (mSegDecrypter == nullptr) {
                mSegDecrypter = unique_ptr<ISegDecrypter>(
                                    SegDecryptorFactory::create(mCurrentEncryption.method,
                                            Decrypter_read_callback, this));
            }

            mCurrentEncryption.iv.clear();
            mCurrentEncryption.iv.resize(16);
            mSegDecrypter->SetOption("decryption key", mKey, 16);
            mSegDecrypter->SetOption("decryption IV", &mCurrentEncryption.iv[0], 16);
            mSegDecrypter->flush();
            if (mDRMMagicKey.empty() && mSegKeySource){
                mDRMMagicKey = mSegDecrypter->GetOption("drmMagicKey");
            }
        }

        return 0;
    }


    int HLSStream::updateSampleAesDecrypter()
    {
        if (updateKey()) {
            if (mSampeAesDecrypter == nullptr) {
                mSampeAesDecrypter = unique_ptr<HLSSampleAesDecrypter>(new HLSSampleAesDecrypter());
            }

            mSampeAesDecrypter->SetOption("decryption key", mKey, 16);
        }

        if (updateIV()) {
            assert(mSampeAesDecrypter != nullptr);

            if (mSampeAesDecrypter) {
                mSampeAesDecrypter->SetOption("decryption IV", &mCurrentEncryption.iv[0], 16);
//                mSampeAesDecrypter->SetOption("decryption KEYFORMAT", (uint8_t *) mCurSeg->encryption.keyFormat.c_str(),
//                                              (int) mCurSeg->encryption.keyFormat.length());
            }
        }
        if (mDRMMagicKey.empty() && mSegKeySource) {
            mDRMMagicKey = mSegKeySource->GetOption("drmMagicKey");
        }

        return 0;
    }

    void HLSStream::close()
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

    int HLSStream::read_thread()
    {
        if (mExited) {
            return -1;
        }

        int ret;
        // first seek is deal in open_internal

//        if (mIsOpened_internal &&
//            mPTracker->getCurSegNum() >
//            mPTracker->getFirstSegNum()) {  // reOpened and seek to, the next seg should be cur seg
//
//            mPTracker->setCurSegNum(mPTracker->getCurSegNum() - 1);
//        }
//        if (mDataSourceStatus != dataSource_status_valid)
//            mIsOpened_internal = false;

        if (mIsOpened && !mIsOpened_internal) {
            ret = open_internal();

            if (ret == -EAGAIN) {
                AF_LOGI("open_internal again\n");
                af_usleep(10000);
                return 0;
            } else if (ret < 0) {
                if (ret == gen_framework_errno(error_class_format, 0) && !mPTracker->isLive() &&
                    mPTracker->getCurSegNum() > mPTracker->getLastSegNum()) {
                    mIsEOS = true;
                    AF_LOGW("read packet lager than last segment , return EOS");
                    return -1;
                }

                mError = ret;
                af_msleep(10);
                return 0; // continue retry
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
                return 0; // continue retry
            }
        }

        return 0;
    }

    int HLSStream::read(unique_ptr<IAFPacket> &packet)
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

    int HLSStream::updateDecrypter()
    {
        int ret = 0;
        mProtectedBuffer = mCurrentEncryption.method != SegmentEncryption::NONE;

        if (mCurrentEncryption.method == SegmentEncryption::AES_128 ||
                mCurrentEncryption.method == SegmentEncryption::AES_PRIVATE) {
            ret = updateSegDecrypter();

            if (ret < 0) {
                return ret;
            }
        } else if (mCurrentEncryption.method == SegmentEncryption::AES_SAMPLE
                && mCurrentEncryption.keyFormat.empty()) {
            ret = updateSampleAesDecrypter();

            if (ret < 0) {
                return ret;
            }
        }

        return ret;
    }

    FILE *file = NULL;


    int HLSStream::updateSegment()
    {
        shared_ptr<segment> seg = nullptr;
        //        AF_LOGD("getCurSegNum is %lld\n", mPTracker->getCurSegNum());
        seg = mPTracker->getNextSegment();
        int ret;
        mCurSeg = nullptr;

        if (seg) {
            do {
                mCurSeg = seg;
                ret = tryOpenSegment(seg);

                if (isHttpError(ret) || isLocalFileError(ret)) {
                    resetSource();
                    seg = mPTracker->getNextSegment();

                    if (seg) {
                        if (seg->mSegType == SEG_LHLS) {
                            af_msleep(5);
                        } else {
                            af_msleep(20);
                        }
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
            }

            AF_LOGD("[hls][lhls] updateSegment");
            AF_LOGD("stream(%p) read seg %s seqno is %llu\n", this, seg->getDownloadUrl().c_str(), seg->getSequenceNumber());
            ret = updateDecrypter();

            if (ret < 0) {
                return ret;
            }

            return 0;
        } else if (mPTracker->getDuration() > 0) {
            AF_LOGE("EOS");
            mIsDataEOS = true;
            return -EAGAIN;
        } else if (mPTracker->hasPreloadSegment()) {
            auto curSeg = mPTracker->getCurSegment(false);
            bool hasUnuse = false;
            if (curSeg && curSeg->isDownloadComplete(hasUnuse)) {
                mCurSeg = mPTracker->usePreloadSegment();
                AF_LOGD("[lhls] use virtual segment of preload hint, uri=%s", mCurSeg->getDownloadUrl().c_str());
                int ret = tryOpenSegment(mCurSeg);
                AF_LOGD("[lhls] use virtual segment of preload hint, ret=%d", ret);
                if (ret < 0) {
                    return -EAGAIN;
                }
                return 0;
            }
            return -EAGAIN;
        }

        return -EAGAIN;
    }

    int HLSStream::read_internal(std::unique_ptr<IAFPacket> &packet)
    {
        //TODO: move read synMsgRst to class member
        int ret = mPTracker->reLoadPlayList();

        if (ret == gen_framework_http_errno(403) || ret == -EIO) {
            return ret;
        }

        packet = nullptr;
        ret = 0;
        if (mSegmentOpened) {
            ret = mPDemuxer->readPacket(packet);
            if (ret < 0) {
                AF_LOGD("mPDemuxer->readPacket ret=%d, packet=%p", ret, packet.get());
            }
        }
        //AF_LOGD("mPDemuxer->readPacket ret is %d,pFrame is %p", ret, *pFrame);

        if (ret == -EAGAIN) {
            if (mInterrupted) {
                return ret;
            }
        } else if (ret == FRAMEWORK_ERR_EXIT) {
            AF_LOGW("mPDemuxer->readPacket FRAMEWORK_ERR_EXIT\n");
        }

        if (ret == 0 && mStopOnSegEnd) {
            /*
             * 【这条不是错误，别每 10ms 打一次】
             *
             * "切清晰度"时管理器会让旧流**在本分片边界停下**（HLSStream::stopOnSegEnd(true)），
             * 这个分支就是"这一片读完了、上面要求停"，属于正常状态。
             *
             * 但 read_thread 对 -EAGAIN 的处理是 `af_msleep(10)` 后重试（见上面 read_thread），
             * 而这里原来每轮都打一条 **AF_LOGE** —— 切一次清晰度要等旧分片读完（几十毫秒到
             * 十几秒），日志里就是刷屏的红色 "mStopOnSegEnd"：用户实测"切换不过去还卡死"，
             * 一半是这些日志、一半是等待本身。改成**每次要求停只打一行**，而且降到 D 级。
             */
            if (!mStopOnSegEndLogged) {
                mStopOnSegEndLogged = true;
                AF_LOGD("stop on segment end（切清晰度：本片读完就停在片界，等管理器换流）\n");
            }

            /* 【seek 落点延迟线】本片到此为止，先把延迟线里攒着的（本片末尾那一段）整体交出，再报停。 */
            mSeekLanding.flush();

            mIsEOS = true;
            return -EAGAIN;
        }

        if (ret == gen_framework_errno(error_class_network, network_errno_http_range)) {
            ret = 0;
        }

        if (ret == 0 || mReopen) {
            if (mReopen) {
                AF_LOGD("reopen");
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
                ret = upDateInitSection();

                if (ret > 0) {
                    //got new initSection, need reopen curSeg.
                    //because use same data source pointer for read init section and segment.
                    tryOpenSegment(mCurSeg);
                }

                ret = createDemuxer();

                if (ret >= 0) {
                    int nbStream = mPDemuxer->GetNbStreams();
                    AF_LOGI("file have %d streams\n", nbStream);
                    // open all stream in demuxer
                    Stream_meta meta{};

                    for (int i = 0; i < nbStream; ++i) {
                        mPDemuxer->GetStreamMeta(&meta, i, false);

                        if (meta.type == mPTracker->getStreamType() ||
                            (mPTracker->getStreamType() == STREAM_TYPE_MIXED && meta.type != STREAM_TYPE_UNKNOWN &&
                             mClosedSubStreams.find(i) == mClosedSubStreams.end())) {
                            mPDemuxer->OpenStream(i);
                        }

                        releaseMeta(&meta);
                    }

                    mPacketFirstPts = getPackedStreamPTS();
                }
                mReopen = false;
            }

            packet = nullptr;
            return -EAGAIN;
        }

        if (ret == -EAGAIN && mPTracker->getDuration() == 0) {

            MoveToNextPart move_ret = moveToNextPartialSegment();
            if (move_ret == MoveToNextPart::segmentEnd) {
                ret = updateSegment();
            } else {
                return -EAGAIN;
            }

            if (ret < 0) {
                return ret;
            }

            return -EAGAIN;
        }

        if (packet != nullptr) {
            //  AF_LOGD("read a frame \n");

            /*
             * 【这块只服务"对齐切档"】mDiscardPts 的唯一设置点是 HLSManager 的对齐切档
             * （目标流接过旧流的最后 pts），语义是"这次切档要从哪儿开始"。
             * ⚠ 铁律：**绝不允许把 seek 目标塞进 mDiscardPts**。下面第二条判据会整片跳过
             * （mReopen），拿 seek 目标当它就会把落点推到目标之后，直接破坏"首帧必须包含目标、
             * 不晚于目标"。HLSStream::seek 开头已经把地板清成 INT64_MIN 做硬化。
             */
            if (mDiscardPts != INT64_MIN) {
                if (packet->getInfo().pts < mDiscardPts) {
                    if (mDiscardPts - packet->getInfo().pts > mPTracker->getTargetDuration() / 2) {
                        AF_LOGW("skip segment , dis - pts = %lld , mCurSeg->duration /2 = %lld ", mDiscardPts - packet->getInfo().pts,
                                mPTracker->getTargetDuration() / 2);
                        //skip this segment, to void decode cost too long time
                        mReopen = true;
                        packet = nullptr;
                        // only skip one segment, to avoid the case segment not aligned
                        mDiscardPts = INT64_MIN;
                        return -EAGAIN;
                    } else {
                        packet->setDiscard(true);
                        discardCount++;
                    }
                } else {
                    mDiscardPts = INT64_MIN;
                    AF_LOGW("discard pkt count = %d", discardCount);
                }
            }

            mLastPts = std::max(mLastPts, packet->getInfo().pts);

            if (mProtectedBuffer && !mDRMMagicKey.empty()) {
                packet->setProtected();
                packet->setMagicKey(mDRMMagicKey);
            }
            if (mPTracker->getStreamType() != STREAM_TYPE_MIXED) {
                packet->getInfo().streamIndex = 0;
            }

            if (mPacketFirstPts != INT64_MIN && packet->getInfo().pts != INT64_MIN) {
                packet->getInfo().pts += mPacketFirstPts;
                packet->getInfo().dts += mPacketFirstPts;
            }

            if (mVttPtsOffSet != INT64_MIN && packet->getInfo().pts != INT64_MIN) {
                packet->getInfo().pts += mVttPtsOffSet;
                packet->getInfo().dts += mVttPtsOffSet;
            }

            if (mCurSeg) {
                // mark the seg start time to first seg frame
                AF_LOGD("stream (%d) mark startTime %llu\n", mPTracker->getStreamType(), mCurSeg->startTime);
                AF_LOGD("stream (%d)pFrame->pts is %lld pos is %lld flags is %d streamIndex is %d\n", mPTracker->getStreamType(),
                        packet->getInfo().pts, packet->getInfo().pos, packet->getInfo().flags, packet->getInfo().streamIndex);

                if (packet->getInfo().flags == 0) {
                    AF_LOGE("not a key frame\n");
                    _hex_dump(packet->getData(), 16);
                }

                int nbStreams = GetNbStreams();

                for (int i = 0; i < nbStreams; i++) {
                    mStreamStartTimeMap[i].timePosition = mCurSeg->startTime;
                    if (mCurSeg->utcTime >= 0) {
                        mStreamStartTimeMap[i].utcTime = mCurSeg->utcTime;
                    }
                    mStreamStartTimeMap[i].seamlessPoint = true;
                }

                mCurSeg = nullptr;
            }

            int streamIndex = packet->getInfo().streamIndex;
            packet->getInfo().seamlessPoint = mStreamStartTimeMap[streamIndex].seamlessPoint;

            if (mStreamStartTimeMap[streamIndex].seamlessPoint) {
                if (packet->getInfo().pts != INT64_MIN) {
                    mStreamStartTimeMap[streamIndex].time2ptsDelta = mStreamStartTimeMap[streamIndex].timePosition - packet->getInfo().pts;
                    if ( mStreamStartTimeMap[streamIndex].utcTime >= 0) {
                        mStreamStartTimeMap[streamIndex].utc2ptsDelta = mStreamStartTimeMap[streamIndex].utcTime - packet->getInfo().pts;
                    }
                }

                mStreamStartTimeMap[streamIndex].seamlessPoint = false;
            }

            if (packet->getInfo().duration > 0) {
                mStreamStartTimeMap[streamIndex].frameDuration = packet->getInfo().duration;
            }

            if (packet->getInfo().pts == INT64_MIN
                    && mStreamStartTimeMap[streamIndex].lastFramePts != INT64_MIN
                    && mStreamStartTimeMap[streamIndex].frameDuration != INT64_MIN) {
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

//          AF_LOGE("pFrame->pts is %lld index is %d\n", (*pFrame)->pts, (*pFrame)->streamIndex);
        }

        return ret;
    }


    int HLSStream::GetNbStreams() const
    {
        if (mPTracker->getStreamType() == STREAM_TYPE_MIXED) { //mediaPlayList
            if (mPDemuxer) {
                return mPDemuxer->GetNbStreams();
            }
        }

        return 1; // masterPlayList only support one es stream per stream url(ext-media-info ext-stream-info) now
    }

    int HLSStream::GetStreamMeta(Stream_meta *meta, int index, bool sub) const
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

        if (meta->height == 0) {
            meta->height = height;
            meta->width = width;
        }

        /*
         * 【这一路变体的编码：master playlist 的 CODECS -> AFCodecID】
         *
         * HlsParser::createRepresentation() 已经把 EXT-X-STREAM-INF / EXT-X-MEDIA 的
         * CODECS 属性存进 Representation::codecs（见 HlsParser.cpp 里 addCodecs 那次调用）。
         * 这里把它交给同一个归一化函数转成 AFCodecID。
         *
         * 只在底层没给出编码时才填：已开流时 mPDemuxer 的编码来自真实容器
         * （TS 的 PMT / fmp4 的 stsd），是权威值，不覆盖。清单里没有 CODECS
         * 属性时 getCodecsString() 返回空串，归一化返回 NONE —— 也就是"这一路
         * 没有编码信息"，应用层拿到空短名、不显示徽标，不允许猜。
         */
        if (meta->codec == AF_CODEC_ID_NONE) {
            meta->codec = afCodecIDFromManifestCodecs(mPTracker->getCodecsString().c_str());
        }

        meta->lang = strdup(lang.c_str());
        meta->bandwidth = bandwidth;
        meta->duration = mPTracker->getDuration();

        if (!mPTracker->getDescriptionInfo().empty()) {
            meta->description = strdup(mPTracker->getDescriptionInfo().c_str());
        }

        meta->keyUrl = mCurrentEncryption.keyUrl.empty() ? nullptr : strdup(mCurrentEncryption.keyUrl.c_str());
        meta->keyFormat = mCurrentEncryption.keyFormat.empty() ? nullptr : strdup(mCurrentEncryption.keyFormat.c_str());
        meta->drmPssh = mCurrentEncryption.pssh.empty() ? nullptr : strdup(mCurrentEncryption.pssh.c_str());
        meta->drmKeyId = mCurrentEncryption.keyId.empty() ? nullptr : strdup(mCurrentEncryption.keyId.c_str());

        return 0;
    }

    bool HLSStream::CloseSubStream(int index)
    {
        mClosedSubStreams.insert(index);
        if (mPDemuxer) {
            mPDemuxer->CloseStream(index);
        }
        return true;
    }

    bool HLSStream::isOpened()
    {
        return mIsOpened;
    }

    int64_t HLSStream::getTargetDuration()
    {
        if (mPTracker) {
            return mPTracker->getTargetDuration();
        }
        return INT64_MIN;
    }

    vector<mediaSegmentListEntry> HLSStream::getSegmentList()
    {
        if (mPTracker) {
            return mPTracker->getSegmentList();
        }
        return {};
    }

    int HLSStream::start()
    {
//        demuxer_msg::StartReq start;
//        mPProxyService->SendMsg(start, mPDemuxer->GetAddr(), false);
        mIsOpened = true;
        mIsEOS = false;
        mIsDataEOS = false;
        mStopOnSegEnd = false;
        mClosedSubStreams.clear();
        mError = 0;

        if (mThreadPtr == nullptr) {
            mThreadPtr = NEW_AF_THREAD(read_thread);
        }

        mThreadPtr->start();
        return 0;
    }

    int HLSStream::preStop()
    {
        std::unique_lock<std::mutex> waitLock(mDataMutex);
        mExited = true;
        mWaitCond.notify_one();
        return 0;
    }

    int HLSStream::stop()
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
    int64_t HLSStream::seek(int64_t us, int flags)
    {
        (void) flags;
        uint64_t num = 0;
        auto usSought = (uint64_t) us;
        bool reqReOpen = true;
        AF_LOGD("%s:%d stream (%d) seek us is %lld\n", __func__, __LINE__,
                mPTracker->getStreamType(), us);

        /*
         * ============ mDiscardPts 的硬边界：它只属于"对齐切档"，绝不允许装 seek 目标 ============
         *
         * mDiscardPts 是"这次清晰度对齐要从哪儿开始"的地板，唯一的设置点是 HLSManager 的对齐切档
         * （目标流接过旧流的最后 pts）。read_internal 对它的用法有两条：
         *   · 低于地板：丢包（setDiscard）；
         *   · 低得超过半片：**整片跳过**（mReopen）。
         * 所以一旦有人把**seek 目标**塞进 mDiscardPts，"低于地板"的判据会变成"晚于目标的帧才算数"，
         * 整片跳过那条更是会把落点推到目标**之后** —— 直接破坏"首帧必须是包含目标、且不晚于目标的
         * 那一帧"这条铁律（精准 seek 的全部意义就在这一条）。
         * 因此这里做硬化：seek 一开始就把地板清掉，让 seek 落点只由"包含目标的那一片 + 延迟线"决定；
         * 正在途中的对齐切档也已经由 HLSManager::seek 的第 1 步收尾了，不需要这个地板。
         */
        mDiscardPts = INT64_MIN;
        discardCount = 0;

        if (!mPTracker->isInited()) {
            mSeekPendingUs = us;
            AF_LOGI("pending seek\n");
            return us;
        }

        //   int ret = mPTracker->init();
        bool b_ret = mPTracker->getSegmentNumberByTime(usSought, num);

        if (!b_ret) {
            AF_LOGE("(%d)getSegmentNumberByTime error us is %lld\n", mPTracker->getStreamType(),
                    us);
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

        AF_LOGD("%s:%d stream (%d) usSeeked is %lld seek num is %d\n", __func__, __LINE__,
                mPTracker->getStreamType(), usSought, num);

        /*
         * 【落点诊断】这一行是"目标 Representation 到底被定位到哪"的唯一权威读数。
         *
         * 2026-09-21 的 HLS 实测：点 2160p 时请求 34.831s，这里拿到 segNum=5
         * （= 33.366s，正确），但 0.5 秒后读取线程已经把 -9.m4s（70.9s）读进队列，
         * 播放点却还在 34.9s —— 于是 RenderVideo() 认为"帧太早"一直不渲染，
         * FPS 永久 0。上一条 usSeeked 打印的是**请求值**不是结果，容易误判，
         * 所以在这里补上 tracker 真正选中的段号/段位置。
         */
        AF_LOGI("[seek] hls %s: reqUs=%lld -> segNum=%llu curSegNum=%llu lastSegNum=%llu "
                "duration=%lld live=%d initialized=%d\n",
                mPTracker->getStreamType() == STREAM_TYPE_VIDEO ? "video" : "audio/other",
                (long long) usSought, (unsigned long long) num,
                (unsigned long long) mPTracker->getCurSegNum(),
                (unsigned long long) mPTracker->getLastSegNum(),
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
                num--;
                mReopen = true;
            }

            mPTracker->setCurSegNum(num);
            AF_LOGI("[seek] hls %s: tracker positioned at segNum=%llu (reqUs=%lld, reopened)\n",
                    mPTracker->getStreamType() == STREAM_TYPE_VIDEO ? "video" : "audio/other",
                    (unsigned long long) mPTracker->getCurSegNum(), (long long) usSought);
        }

        /*
         * 【seek 落点延迟线：装弹】位置很关键：
         *   · 必须在上面 clearDataFrames() 之后 —— 那一刻读线程已经 pause()，不会再写延迟线的 stage；
         *   · 必须在下面 mThreadPtr->start() 之前 —— 线程一启动就可能立刻产出第一个包。
         * 目标必须用入参 us，**不能用 usSought**：SegmentList::getSegmentNumberByTime 会把 usSought
         * 改写成"该分片起点"（这是它给 HLS 提供的落点语义），拿它当目标的话，落点会退回到
         * "最后一个 ≤ 分片起点"的关键帧，等于白做。
         */
        mSeekLanding.arm(us, "HLS", mPTracker);

        mIsEOS = false;
        mIsDataEOS = false;
        mError = 0;
        if (mDemuxerMeta) {
            mDemuxerMeta->bContinue = false;
        }

        if (mThreadPtr) {
            mThreadPtr->start();
        }

        return usSought;
    }

    int64_t HLSStream::seek_internal(uint64_t segNum, int64_t us)
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

    uint64_t HLSStream::getCurSegNum()
    {
        return mPTracker->getCurSegNum();
    }

    int HLSStream::stopOnSegEnd(bool stop)
    {
        mStopOnSegEnd = stop;

        /* 每次"要求停"只允许打一行日志（见 read_internal 里那个分支的说明） */
        if (stop) {
            mStopOnSegEndLogged = false;
        }

        return 0;
    }

    int HLSStream::SetCurSegNum(uint64_t num)
    {
        std::map<OpenType, uint64_t> params{};
        params[OpenType::SegNum] = num;
        return reopenSegment(params);
    }

    int HLSStream::reopenSegment(std::map<OpenType, uint64_t> &params)
    {
        {
            std::unique_lock<std::mutex> waitLock(mDataMutex);
            mSwitchNeedBreak = true;
        }

        mWaitCond.notify_one();

        bool threadRunning = false;
        if (mThreadPtr && mThreadPtr->getStatus() == afThread::THREAD_STATUS_RUNNING) {
            threadRunning = true;
        }

        if (mThreadPtr && threadRunning) {
            mThreadPtr->pause();
        }

        mSwitchNeedBreak = false;
        clearDataFrames();
        /*
         * 【seek 落点延迟线】reopenSegment 是"按分片号/分片位置重设读位置"（切档交接、SetCurSegNum
         * 等），它**没有时间目标**：上一次 seek 留下的目标到这里已经不能代表现在要读的位置，必须连
         * 目标一起清。否则一个陈旧的、恰好落在前方的目标会让延迟线丢掉这个分片的前缀。
         */
        mSeekLanding.reset();
        resetSource();

        if (mIsOpened_internal) {
            mReopen = true;
        }

        if (params.count(OpenType::SegPosition) > 0) {
            uint64_t value = params[OpenType::SegPosition];
            if (mIsOpened_internal && value > 0) {
                value--;
            }
            AF_LOGD("setCurSegPosition %llu\n", value);
            mPTracker->setCurSegPosition(value);
        }

        if (params.count(OpenType::SegNum) > 0) {
            uint64_t value = params[OpenType::SegNum];
            if (mIsOpened_internal && value > 0) {
                value--;
            }
            AF_LOGD("setCurSegNum %llu\n", value);
            mPTracker->setCurSegNum(value);
        }

        seek_internal(0, 0);
        mIsEOS = false;
        mIsDataEOS = false;
        mError = 0;

        if (mThreadPtr && threadRunning) {
            mThreadPtr->start();
        }

        return 0;
    }

    uint64_t HLSStream::getCurSegPosition()
    {
        return mPTracker->getCurSegPosition();
    };

    int HLSStream::setCurSegPosition(uint64_t position)
    {
        std::map<OpenType, uint64_t> params{};
        params[OpenType::SegPosition] = position;
        return reopenSegment(params);
    }

    int HLSStream::setCurSegInfo(CurSegInfo &curSegInfo)
    {
        std::map<OpenType, uint64_t> params{};
        params[OpenType::SegPosition] = curSegInfo.position;
        params[OpenType::SegNum] = curSegInfo.segNum;
        return reopenSegment(params);
    }

    void HLSStream::setCurRenditionInfo(const std::vector<RenditionReport> &renditions)
    {
        if (mPTracker) {
            mPTracker->setRenditionInfo(renditions);
        }
    }

    std::vector<RenditionReport> HLSStream::getCurRenditionInfo()
    {
        if (mPTracker) {
            return mPTracker->getRenditionInfo();
        }
        return {};
    }

    bool HLSStream::isLive()
    {
        return mPTracker->isLive();
    }

    int64_t HLSStream::getDuration()
    {
        return mPTracker->getDuration();
    }

    int HLSStream::getNBStream() const
    {
        if (mPDemuxer) {
            return mPDemuxer->GetNbStreams();
        }

        return 1;
    }

    void HLSStream::interrupt(int inter)
    {
        {
            std::unique_lock<std::mutex> waitLock(mDataMutex);
            mInterrupted = static_cast<bool>(inter);
        }
        interrupt_internal(inter);
    }

    void HLSStream::interrupt_internal(int inter)
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

    std::string HLSStream::GetProperty(const string &key)
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
        } else if ("keyUrl" == key) {
            return mCurrentEncryption.keyUrl;
        }

        return "";
    }

    bool HLSStream::isRealTimeStream()
    {
        if (mPTracker != nullptr) {
            return mPTracker->isRealTimeStream();
        } else {
            return false;
        }
    }

    int64_t HLSStream::getBufferDuration() const
    {
        if (mExtDataSource) {
            return mExtDataSource->getBufferDuration();
        }
        if (mPdataSource) {
            return mPdataSource->getBufferDuration();
        }
        return 0;
    }

    HLSStream::WebVttParser::WebVttParser() = default;

    HLSStream::WebVttParser::~WebVttParser()
    {
        if (mBuffer) {
            free(mBuffer);
        }
    }

    static int64_t read_ts(const char *s)
    {
        int hh, mm, ss, ms;

        if (sscanf(s, "%u:%u:%u.%u", &hh, &mm, &ss, &ms) == 4) {
            return (hh * 3600LL + mm * 60LL + ss) * 1000LL + ms;
        }

        if (sscanf(s, "%u:%u.%u", &mm, &ss, &ms) == 3) {
            return (mm * 60LL + ss) * 1000LL + ms;
        }

        return INT64_MIN;
    }

    int64_t HLSStream::WebVttParser::addBuffer(uint8_t *buffer, int size)
    {
        if (bFinished) {
            return mMapPTS;
        }

        char *lend;

        if ((lend = strnstr((const char *) buffer, "\n\n", size)) != nullptr
                || (lend = strnstr((const char *) buffer, "\r\n\r\n", size)) != nullptr) {
            bFinished = true;
        }

        size_t append_size = size;

        if (lend != nullptr) {
            append_size = lend - (const char *) buffer;
        }

        uint8_t *new_data = static_cast<uint8_t *>(realloc(mBuffer, mSize + append_size));

        if (new_data) {
            mBuffer = new_data;
        } else {
            return mMapPTS;
        }

        memcpy(mBuffer + mSize, buffer, append_size);
        mSize += append_size;

        if (!bFinished) {
            return INT64_MIN;
        }

        mBuffer[mSize - 1] = 0;
        /*
        * WebVTT files in HLS streams contain a timestamp offset for
        * syncing with the main stream:
        *
        * X-TIMESTAMP-MAP=LOCAL:00:00:00.000,MPEGTS:900000
        * (LOCAL and MPEGTS can be reversed even though HLS spec
        *  does not say so)
        */
        char *hls_timestamp_map = strstr(reinterpret_cast<char *>(mBuffer), "\nX-TIMESTAMP-MAP=");

        if (hls_timestamp_map == nullptr) {
            return mMapPTS;
        }

        char *native_str = strstr(hls_timestamp_map, "LOCAL:");
        char *mpegts_str = strstr(hls_timestamp_map, "MPEGTS:");

        if (native_str && mpegts_str) {
            int64_t native_ts = read_ts(native_str + 6);
            int64_t mpegts_ts = strtoll(mpegts_str + 7, nullptr, 10);

            if (native_ts != INT64_MIN) {
                mMapPTS = mpegts_ts * 100 / 9 - native_ts * 1000;
            }
        }

        return mMapPTS;
    }

    void HLSStream::WebVttParser::rest()
    {
        mSize = 0;
        mMapPTS = INT64_MIN;
        bFinished = false;
    }

}// namespace Cicada
