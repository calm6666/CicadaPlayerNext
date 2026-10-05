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
/*
 * DRM：DrmHandlerPrototype 提供"本平台认不认这个 DRM scheme"的平台无关查询
 * （认不认由各平台在 framework/drm/ 下注册的 handler 决定 —— Android 的
 * WideVineDrmHandler、OHOS 的 OhosDrmHandler）。CENCDecrypter 的密钥由
 * ContentKeyFetcher 按清单声明的地址在**读到第一个加密包时**按需取，见
 * HLSStream::fetchCencKey 与 demuxer_service::setCencKeyResolver。
 */
#include "drm/DrmHandlerPrototype.h"
#include "drm/DrmInfo.h"
#include "demuxer/sample_decrypt/ContentKeyFetcher.h"
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
                /*
                 * 重试仍取不到（网络还没回来）⇒ 报"暂时没数据"，不能报 0(=EOF)：
                 * 报 EOF 会让读线程置 mIsEOS 退出，播放器随后闩上 mEof，网络恢复也不会继续。
                 * 用框架自己的 -EAGAIN（POSIX 上等于 AVERROR(EAGAIN)）。
                 */
                return -EAGAIN;
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
        /*
         * 【判据必须是"本片声明的加密方法"，不能只看 mSegDecrypter 在不在】
         *
         * createDemuxer() 现在保证"本片不加密就把解密器交还"（clearDecrypterState），
         * 但这里的判据仍然按**权威来源**（mCurrentEncryption.method）再确认一次：
         * 只要本片声明的不是整片加密（NONE / AES_SAMPLE），就**绝不**让 mSegDecrypter
         * 参与读字节。这样"残留解密器把明文解坏"这一整类问题在结构上不可能发生 ——
         * 而不是依赖两个地方的成员状态恰好同步。
         */
        const bool useSegDecrypter = hasActiveDecrypter();

        if (mSegDecrypter != nullptr && !useSegDecrypter) {
            // 样本级路径的 mSegDecrypter 恒为 null，所以走到这里只能是"状态不同步"，
            // 打一条 ERROR 让现场能看见，而不是静默换路径。
            AF_LOGE("a segment decrypter is present but the current segment declares method=%d "
                    "(not AES_128): ignoring the leftover decrypter and reading the segment as-is\n",
                    (int) mCurrentEncryption.method);
        }

        if (!useSegDecrypter) {
            /*
             * ============ 【★ 这里绝不能把密文当明文读出去（本轮修）★】============
             *
             * 走到这个分支有两种情况，必须分清楚：
             *
             *   1. **本来就不加密**（mCurrentEncryption.method == NONE）：
             *      正常路径，直接读分片字节 —— 行为与以前完全一致。
             *
             *   2. **清单声明了加密，但解密器没建出来**（key 拉取失败 / key 长度不对）：
             *      以前这里也是**直接读密文**交给内层 demuxer ⇒ 表现是"HLS 分片解密失败"
             *      但日志里一句"解密失败"都没有，只有下游一条通用 demux/解码错误
             *      （花屏 / `not a key frame` / moov 解析失败），现场无法区分
             *      "密钥错"和"产物坏"。
             *
             * 现在第 2 种情况**明确报错**：返回 -EIO 并只打一条 ERROR（用 updateKey() 里
             * 已经置好的 mKeyFetchFailedLogged 去重，避免 read_thread 每 10ms 刷一条）。
             * 为什么是 -EIO 而不是继续读：读出去必然是垃圾，让上层拿到一个明确的 IO 错误
             * 比拿一堆垃圾去喂解码器干净得多 —— 这也是"不许假成功"的直接落点。
             *
             * 【CENC 必须排除在这条判据之外（本轮新增）】CENC 的"解密器没建出来"是**正常
             * 状态**而不是失败：它的每样本 IV 与 subsample 表在容器里，解密只能逐样本做，
             * 由 demuxer_service::readPacket 在读到包时就地解（软解），或者交给平台 CDM
             * （硬解）。在**这一层**根本不该有解密器，所以"把分片字节原样交给内层 demuxer"
             * 正是它要的行为。把它一起判成 -EIO 会让对象化清单（CMAF/CENC）这条路的
             * 分片一个字节都读不出来 —— 那不是"拒绝假成功"，那是直接播不了。
             */
            if (mProtectedBuffer && mCurrentEncryption.method != SegmentEncryption::CENC) {
                if (!mKeyFetchFailedLogged) {
                    mKeyFetchFailedLogged = true;
                    AF_LOGE("segment decryption is not applied: the manifest declares encryption "
                            "but no decrypter could be built (key url=%s). Refusing to hand "
                            "ciphertext to the demuxer as if it were plaintext\n",
                            mKeyUrl.empty() ? mCurrentEncryption.keyUrl.c_str() : mKeyUrl.c_str());
                }

                return -EIO;
            }

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
        if (!hasActiveDecrypter()) {
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
        int64_t initStart = INT64_MIN;
        int64_t initEnd = INT64_MIN;
        mCurInitSeg->getDownloadRange(initStart, initEnd);
        mInitSegSize = defaultInitSegSize;
        if (initStart == INT64_MIN && initEnd == INT64_MIN) {
            mInitSegSize = seekSegment(0, SEEK_SIZE);
        } else if (initStart != INT64_MIN && initEnd == INT64_MIN) {
            mInitSegSize = seekSegment(0, SEEK_SIZE) - initStart;
        } else if (initStart == INT64_MIN && initEnd != INT64_MIN) {
            mInitSegSize = initEnd + 1;
        } else {
            mInitSegSize = initEnd - initStart + 1;
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

    bool HLSStream::hasActiveDecrypter() const
    {
        return mSegDecrypter != nullptr && mCurrentEncryption.method == SegmentEncryption::AES_128;
    }

    void HLSStream::releaseSampleAesDecrypter()
    {
        if (mSampeAesDecrypter != nullptr) {
            AF_LOGW("releasing the sample-level (SAMPLE-AES) decrypter: this segment's encryption "
                    "method changed (now method=%d)\n", (int) mCurrentEncryption.method);
            mSampeAesDecrypter.reset();
        }
    }

    void HLSStream::clearDecrypterState()
    {
        if (mSegDecrypter == nullptr && mSampeAesDecrypter == nullptr && mKeyUrl.empty()) {
            return; // 纯明文片源的常态：本来就是空的，不做任何事也不打日志
        }

        mSegDecrypter.reset();
        mSampeAesDecrypter.reset();
        mKeyUrl.clear();
        mKeyFetchFailedLogged = false;
    }

    int HLSStream::createDemuxer()
    {
        int ret;
        mError = 0;
        mDataSourceError = 0;

        if (mDemuxerMeta && mDemuxerMeta->id != mCurSeg->discontinuityNum) {
            mDemuxerMeta = nullptr;
        }
        mCurrentEncryption = SegmentEncryption();
        bool encryptionAccepted = false;
        std::string skippedKeyFormat{};

        for (SegmentEncryption &item: mCurSeg->encryptions) {
            if (item.keyFormat.empty()) {
                mCurrentEncryption = item;
                encryptionAccepted = true;
                break;
            }
            DrmInfo probe{};
            probe.format = item.keyFormat;
            probe.uri = item.keyUrl;
            probe.pssh = item.pssh;
            probe.keyId = item.keyId;

            if (DrmHandlerPrototype::isSupport(&probe)) {
                mCurrentEncryption = item;
                encryptionAccepted = true;
                break;
            }

            if (skippedKeyFormat.empty()) {
                skippedKeyFormat = item.keyFormat;
            }
        }

        if (!encryptionAccepted && !skippedKeyFormat.empty()) {
            /*
             * 只打一条（这是"清单声明了加密、但当前平台一条都不支持"的唯一现场证据）。
             * 绝不打 key 内容：keyFormat 是"DRM 系统标识"（如 Widevine 的 urn:uuid:…），不是密钥。
             */
            AF_LOGE("this segment declares encryption but no declared key format is supported by "
                    "the current platform (keyFormat=%s): the segment will be treated as if it "
                    "carried no encryption record at all\n", skippedKeyFormat.c_str());
        }
        if (!encryptionAccepted) {
            clearDecrypterState();
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
        if (mCurrentEncryption.method == SegmentEncryption::CENC && !mCurrentEncryption.keyUrl.empty()) {
            const std::string cencKeyUrl = Helper::combinePaths(mPTracker->getBaseUri(), mCurrentEncryption.keyUrl);

            mPDemuxer->setCencKeyResolver(
            [this, cencKeyUrl](const std::string &kidHex, uint8_t *key, int *keySize) -> bool {
                return fetchCencKey(cencKeyUrl, kidHex, key, keySize);
            });
        }

        ret = mPDemuxer->createDemuxer(demuxer_type_unknown);

        if (ret < 0) {
            return ret;
        }

        if (mPDemuxer->getDemuxerHandle()) {
            mPDemuxer->getDemuxerHandle()->setBitStreamFormat(this->mMergeVideoHeader, this->mMergerAudioHeader);
            const bool sampleLevelEncrypted = mCurrentEncryption.method == SegmentEncryption::AES_SAMPLE ||
                                              mCurrentEncryption.method == SegmentEncryption::CENC;
            mPDemuxer->getDemuxerHandle()->setStreamEncrypted(sampleLevelEncrypted);
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
                    mDemuxerMeta->meta.push_back(std::move(meta));
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
            if (mPTracker->getStreamType() == STREAM_TYPE_MIXED && !mPTracker->isLive()) {
                mExtDataSource->enableCache(uri, true);
            } else {
                mExtDataSource->enableCache(uri, false);
            }
            return ret;
        }

        if (mPdataSource == nullptr) {
            recreateSource(uri);
            mPdataSource->setRange(start, fixEnd);
            ret = mPdataSource->Open(0);
        } else {
            mPdataSource->setRange(start, fixEnd);
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
        {
            std::lock_guard<std::mutex> lock(mHLSMutex);
            delete mSegKeySource;
            mSegKeySource = dataSourcePrototype::create(keyUrl, mOpts);
            mSegKeySource->Set_config(mSourceConfig);
        }
        int ret = mSegKeySource->Open(0);

        if (ret < 0) {
            AF_LOGE("open key file error: %s (ret=%d) —— key 拉取失败，本片不建解密器；"
                    "URL 未被记住，下一次开流/换分片会再试\n", keyUrl.c_str(), ret);

            if (!mKeyFetchFailedLogged) {
                mKeyFetchFailedLogged = true;
                AF_LOGE("the stream declares %s encryption but its key cannot be fetched: %s "
                        "(decryption is NOT applied; playback of this segment will fail)\n",
                        mCurrentEncryption.method == SegmentEncryption::AES_SAMPLE ? "SAMPLE-AES" : "AES-128",
                        keyUrl.c_str());
            }

            std::lock_guard<std::mutex> lock(mHLSMutex);
            delete mSegKeySource;
            mSegKeySource = nullptr;
            return false;
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
            AF_LOGE("key size is %lld not 16 (url=%s) —— key 内容不对，本片不建解密器；"
                    "URL 未被记住，下一次开流/换分片会再试\n", (long long) size, keyUrl.c_str());
            mSegKeySource->Close();

            if (!mKeyFetchFailedLogged) {
                mKeyFetchFailedLogged = true;
                AF_LOGE("the key fetched from %s is %lld bytes, expected exactly 16; "
                        "decryption is NOT applied\n", keyUrl.c_str(), (long long) size);
            }

            std::lock_guard<std::mutex> lock(mHLSMutex);
            delete mSegKeySource;
            mSegKeySource = nullptr;
            return false;
        }

        mSegKeySource->Close();
        /*
         * **到这里才记住 URL**：同 URL 下一次直接复用（不重复拉），换 URL 才重拉 ——
         * 多 key 轮换就是靠这条"URL 变化"来触发的（见函数头那句判据）。
         */
        mKeyUrl = keyUrl;
        mKeyFetchFailedLogged = false;
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

            /*
             * 这两处都要判空: updateKey() 现在会真的返回 false(key 打不开 / 长度不是 16),
             * 那时 mSegDecrypter 还没建出来 —— 原来正是因为"失败也返回 true"才没走到这里。
             */
            if (updateIV() && mSegDecrypter) {
                mSegDecrypter->SetOption("decryption IV", &mCurrentEncryption.iv[0], 16);
            }

            if (mSegDecrypter) {
                mSegDecrypter->flush();
            }

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

        /*
         * 与 updateSegDecrypter 同理要判空: updateKey() 失败时 decrypter 还没建出来,
         * 只靠 assert 不够 —— Release 下 assert 会被优化掉。
         */
        if (updateIV() && mSampeAesDecrypter) {
            mSampeAesDecrypter->SetOption("decryption IV", &mCurrentEncryption.iv[0], 16);
//                mSampeAesDecrypter->SetOption("decryption KEYFORMAT", (uint8_t *) mCurSeg->encryption.keyFormat.c_str(),
//                                              (int) mCurSeg->encryption.keyFormat.length());
        }
        if (mDRMMagicKey.empty() && mSegKeySource) {
            mDRMMagicKey = mSegKeySource->GetOption("drmMagicKey");
        }

        return 0;
    }
    bool HLSStream::fetchCencKey(const std::string &keyUrl, const std::string &kidHex, uint8_t *key, int *keySize)
    {
        const ContentKeyFetcher::Result result = ContentKeyFetcher::fetch(keyUrl, mOpts, mSourceConfig);

        if (!result.ok()) {
            if (!mCencKeyLogged) {
                mCencKeyLogged = true;
                AF_LOGE("cannot set up software CENC decryption for key id %s from %s: %s. Playback of "
                        "this protected stream will fail on this platform\n",
                        kidHex.c_str(), keyUrl.c_str(), result.detail.c_str());
            }

            return false;
        }

        if (!mCencKeyLogged) {
            mCencKeyLogged = true;
            AF_LOGI("CENC software decryption is set up (key id %s, key url %s): no platform CDM handles "
                    "this scheme here, so decryption runs on the CPU\n", kidHex.c_str(), keyUrl.c_str());
        }

        if (key != nullptr) {
            memcpy(key, result.key, 16);
        }

        if (keySize != nullptr) {
            *keySize = result.keySize;
        }

        return true;
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
            mQueue.push_back(std::move(tmp));
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

            packet = std::move(mQueue.front());
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
        if (mSampeAesDecrypter != nullptr &&
                mCurrentEncryption.method != SegmentEncryption::AES_SAMPLE) {
            releaseSampleAesDecrypter();
        }

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
            if (!mStopOnSegEndLogged) {
                mStopOnSegEndLogged = true;
                AF_LOGD("stop on segment end（切清晰度：本片读完就停在片界，等管理器换流）\n");
            }

            /* 【seek 落点延迟线】本片到此为止，先把延迟线里攒着的（本片末尾那一段）整体交出，再报停。 */
            mSeekLanding.flush();

            mIsEOS = true;
            return 0;
        }

        if (ret == gen_framework_errno(error_class_network, network_errno_http_range)) {
            ret = 0;
        }

        if (ret == 0 || mReopen) {
            if (mReopen) {
                AF_LOGD("reopen");
            }
            mSeekLanding.flushOnHostReopen();

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
            mKeyUrl.clear();

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
        mSeekLanding.arm(us, "HLS", mPTracker);

        mIsEOS = false;
        mIsDataEOS = false;
        mError = 0;
        if (mDemuxerMeta) {
            mDemuxerMeta->bContinue = false;
        }
        for (auto &streamStartTime : mStreamStartTimeMap) {
            streamStartTime.second.lastFramePts = INT64_MIN;
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

    bool HLSStream::getSegmentNumByTime(int64_t timeUs, uint64_t &num)
    {
        if (timeUs <= 0) {
            return false;
        }

        /*
         * 转发即可：这个 tracker 的实现会把 timeUs 就地改写成该片的 startTime，
         * 但 num 始终是**分片号**（包含该时刻的那一片）⇒ +1 由调用方做。
         */
        uint64_t time = (uint64_t) timeUs;
        return mPTracker->getSegmentNumberByTime(time, num);
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
        mSeekLanding.dropStageOnHostReopen();
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
