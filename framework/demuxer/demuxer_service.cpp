//
// Created by moqi on 2018/2/1.
//
#define LOG_TAG "demuxer_service"

#include <algorithm>
#include <utils/frame_work_log.h>
#include <cassert>
#include <utils/errors/framework_error.h>
#include <demuxer/sample_decrypt/SampleDecryptDemuxer.h>
#include <demuxer/manifest/ManifestDemuxer.h>
#include "demuxer_service.h"

#define  MAX_PROBE_SIZE 1024

namespace Cicada {

    demuxer_service::demuxer_service(IDataSource *pDataSource)
        : mPDataSource(pDataSource)
    {
    }

    demuxer_service::~demuxer_service()
    {
        delete[] mPProbBuffer;
    }


    void *demuxer_service::getCodecPar(int streamIndex)
    {
        AF_TRACE;

        if (mDemuxerPtr) {
            return mDemuxerPtr->getCodecPar(streamIndex);
        }

        return nullptr;
    }

    void demuxer_service::SetDataCallBack(demuxer_callback_read read, void *read_arg,
                                          demuxer_callback_seek seek,
                                          void *seek_arg, const char *uri)
    {
        AF_TRACE;
        mReadCb = read;
        mReadArg = read_arg;
        mSeekCb = seek;
        mSeekArg = seek_arg;
    }

    void demuxer_service::setDemuxerCb(const std::function<void(std::string, std::string)> &func)
    {
        if (mDemuxerPtr) {
            return mDemuxerPtr->setDemuxerCb(func);
        }

        mDemuxerCbfunc = func;
    }

#define CHECK_DEMUXER do{if (mDemuxerPtr == nullptr) return -1;}while(false);
#define CHECK_DEMUXER_V do{if (mDemuxerPtr == nullptr) return;}while(false);

    int demuxer_service::createDemuxer(demuxer_type type)
    {
        if (mSDec) {
            SampleDecryptDemuxer *demuxer = new SampleDecryptDemuxer();
            demuxer->setDecryptor(mSDec);
            mDemuxerPtr = std::unique_ptr<IDemuxer>(demuxer);
        }

        if (mDemuxerPtr == nullptr && type == demuxer_type_manifest && mManifestSource != nullptr) {
            // Object-based playback: the manifest is converted in-process, no
            // URL probing or manifest network I/O is performed.
            mDemuxerPtr = std::unique_ptr<IDemuxer>(new ManifestDemuxer(std::move(mManifestSource)));
        }

        if (mDemuxerPtr == nullptr) {
            if (mPProbBuffer == nullptr) {
                mPProbBuffer = new uint8_t[MAX_PROBE_SIZE];
                memset(mPProbBuffer, 0, MAX_PROBE_SIZE);
                mProbBufferSize = 0;
            }

            if (!mNoFile) {
                int probSize = 256;
                while (mProbBufferSize < probSize) {
                    int ret = 0;

                    if (mPDataSource) {
                        ret = mPDataSource->Read(mPProbBuffer + mProbBufferSize, MAX_PROBE_SIZE - mProbBufferSize - 1);
                    } else if (mReadCb) {
                        ret = mReadCb(mReadArg, mPProbBuffer + mProbBufferSize, MAX_PROBE_SIZE - mProbBufferSize - 1);
                    } else {
                        return -EINVAL;
                    }

                    if (ret > 0) {
                        if (mProbBufferSize == 0) {
                            if (strstr((char *) mPProbBuffer, "<MPD") || strstr((char *) mPProbBuffer, "<mpd")) {
                                probSize = MAX_PROBE_SIZE;
                            }
                        }
                        mProbBufferSize += ret;
                    } else {
                        break;
                    }
                }

                if (mProbBufferSize <= 0) {
                    return -EIO;
                }
            }

            string url;

            if (mPDataSource) {
                url = mPDataSource->GetUri();
            }

            mPProbBuffer[mProbBufferSize] = 0;
            mDemuxerPtr = std::unique_ptr<IDemuxer>(
                              demuxerPrototype::create(url, mPProbBuffer, mProbBufferSize, std::move(mDemuxerMeta), mOpts));

            if (!mDemuxerPtr) {
                AF_LOGE("create demuxer error\n");
                return FRAMEWORK_ERR_FORMAT_NOT_SUPPORT;
            }
        }

        mDemuxerPtr->setDemuxerCb(mDemuxerCbfunc);

        if (mDemuxerPtr->isPlayList()) {
            IDataSource::SourceConfig config;

            if (mPDataSource) {
                mPDataSource->Get_config(config);
            }

            mDemuxerPtr->setDataSourceConfig(config);
        }

        return 0;
    }

    void demuxer_service::setManifestSource(std::unique_ptr<Manifest::MediaManifest> manifest)
    {
        mManifestSource = std::move(manifest);
    }

    int demuxer_service::initOpen(demuxer_type type)
    {
        AF_TRACE;

        if (mDemuxerPtr == nullptr) {
            createDemuxer(type);
        }

        if (!mDemuxerPtr) {
//            AF_LOGE("create demuxer error\n");
            return FRAMEWORK_ERR_FORMAT_NOT_SUPPORT;
        }

        if (!mNoFile) {
            /*
             * demuxer will occurred an error when set seek callback but can't seek
             */
            if ((mPDataSource == nullptr || mPDataSource->Seek(0, SEEK_SIZE) <= 0) && (mSeekCb == nullptr)) {
                AF_LOGD("not support seek\n");
                mDemuxerPtr->SetDataCallBack(read_callback, nullptr, open_callback, interrupt_callback, setSegmentList_callback,
                                             getBufferDuration_callback, enableCache_callback, this);
            } else {
                mDemuxerPtr->SetDataCallBack(read_callback, seek_callback, open_callback, interrupt_callback, setSegmentList_callback,
                                             getBufferDuration_callback, enableCache_callback, this);
            }
        }

        if (mFirstSeekUs > 0) {
            mDemuxerPtr->Seek(mFirstSeekUs, 0, -1);
        }

        int openRet = mDemuxerPtr->Open();
        if (openRet >= 0 && mPDataSource != nullptr) {
            mPDataSource->setMediaInfoProvider(mDemuxerPtr.get());
        }
        return openRet;
    }

    int demuxer_service::readPacket(std::unique_ptr<IAFPacket> &packet, int index)
    {
        int ret;
        CHECK_DEMUXER;
        ret = mDemuxerPtr->ReadPacket(packet, index);

        /*
         * ============ 样本级加密（CENC）的**兜底**就地解密 ============
         *
         * 【判据全部来自数据，没有任何平台宏、没有开关】
         *   · mCencDecrypter 为空 ⇒ 说明本次播放**没有任何 KID 注册过软件密钥**
         *     （见 setCencKey）。有 CDM 的平台就属于这一种：包原样放行，由
         *     平台解码器 queueSecureInputBuffer 去解。这里立刻返回，零开销。
         *   · 非空 ⇒ 逐个包看**包自己带的** AV_PKT_DATA_ENCRYPTION_INFO：
         *     有 KID 且该 KID 注册过密钥就解；没注册就**原样放行**。
         *
         * 为什么"没注册就原样放行"是对的而不是漏解：CENCDecrypter 对未知 KID
         * 返回 -ENOENT（拒绝把密文当明文），而这条路径的语义是"注册了才解"，
         * 所以未注册本来就不该由内核动它。
         *
         * 为什么放在 readPacket 而不是 avFormatDemuxer 内部：这是**所有**解复用
         * 路径（HLS 分片管线、DASH 分片管线、普通文件）的唯一收口，放在这里
         * 一处即可覆盖，且天然在 bsf **之后**（解密后的包才该参与 head 合并），
         * 与 avFormatDemuxer::createBsf 里"看到加密 side data 就不建 bsf"的
         * 既有判据不会互相打架。
         */
        if (ret > 0 && mCencDecrypter != nullptr && packet != nullptr) {
            applyCencDecryption(*packet);
        }

        return ret;
    }

    /*
     * 对一个包尝试 CENC 解密。返回**实际解了没有**（供日志/自检使用）。
     *
     * 只碰包内字节，不碰包长（CENC 密文与明文等长），所以不需要重建 AVPacket，
     * 也不需要通知任何持有者的尺寸信息。
     */
    bool demuxer_service::applyCencDecryption(IAFPacket &packet)
    {
        if (!packet.isProtected() && !packetHasEncryptionInfo(packet)) {
            return false;
        }

        IAFPacket::EncryptionInfo info{};

        if (!packet.getEncryptionInfo(&info)) {
            return false;
        }

        if (info.key_id == nullptr || info.key_id_size == 0 || info.iv == nullptr || info.iv_size == 0) {
            return false;
        }

        const std::string keyId = CENCDecrypter::toHex(info.key_id, info.key_id_size);

        /*
         * 没注册这把密钥 ⇒ 不是"失败"，而是"不该由内核解"（平台 CDM 负责）。
         * 这里刻意**不打日志**：有 CDM 的平台上每一路流、每一个包都会走到这里，
         * 打日志就是刷屏。
         */
        if (!mCencDecrypter->hasKey(keyId)) {
            return false;
        }

        std::vector<SubsampleInfo> subsamples;
        subsamples.reserve(info.subsamples.size());

        for (const auto &sub : info.subsamples) {
            subsamples.push_back(SubsampleInfo{sub.bytes_of_clear_data, sub.bytes_of_protected_data});
        }

        const int64_t size = packet.getSize();
        const int64_t got = mCencDecrypter->decrypt(keyId, info.scheme, info.iv, info.iv_size,
                              subsamples, info.crypt_byte_block, info.skip_byte_block,
                              packet.getData(), size);

        if (got < 0) {
            /*
             * 注册了密钥却解不开，是**真错误**（密钥不对 / 元数据不对），
             * 绝不能把密文当明文交给解码器（那就是花屏 + 一条看不懂的通用解码错误）。
             * 只打一条，避免每包刷屏；包本身照常交出 —— 上层会因解码失败报错，
             * 而这条日志给出了真正的原因。
             */
            if (!mCencDecryptFailedLogged) {
                mCencDecryptFailedLogged = true;
                AF_LOGE("CENC software decryption failed for key id %s (scheme=%s, iv_size=%u, "
                        "subsamples=%zu): the ciphertext is NOT being passed off as plaintext\n",
                        keyId.c_str(), info.scheme.c_str(), info.iv_size, subsamples.size());
            }

            return false;
        }

        if (!mCencDecryptAppliedLogged) {
            mCencDecryptAppliedLogged = true;
            AF_LOGI("CENC software decryption is active (key id %s, scheme=%s): decrypted on the CPU "
                    "because no platform CDM handled this stream\n",
                    keyId.c_str(), info.scheme.c_str());
        }

        return true;
    }

    bool demuxer_service::packetHasEncryptionInfo(IAFPacket &packet)
    {
        IAFPacket::EncryptionInfo info{};
        return packet.getEncryptionInfo(&info) && info.key_id != nullptr && info.key_id_size > 0;
    }

    int demuxer_service::setCencKey(const std::string &keyIdHex, const uint8_t *key, int keySize)
    {
        if (mCencDecrypter == nullptr) {
            mCencDecrypter = std::unique_ptr<CENCDecrypter>(new CENCDecrypter());
        }

        const int ret = mCencDecrypter->setKey(keyIdHex, key, keySize);

        /*
         * 登记失败且表里一把 key 都没有 ⇒ 把空壳收掉。
         * 为什么在意这个：`applyCencDecryption` 的入口判据是"mCencDecrypter 非空"
         * （那是"本次播放有没有注册过软件密钥"的唯一记号），留一个空壳会让每个包
         * 都白白走一遍 getEncryptionInfo。收掉之后状态与"从没注册过"完全同态。
         */
        if (!mCencDecrypter->hasAnyKey()) {
            mCencDecrypter.reset();
        }

        return ret;
    }

    bool demuxer_service::hasCencKey(const std::string &keyIdHex) const
    {
        return mCencDecrypter != nullptr && mCencDecrypter->hasKey(keyIdHex);
    }

    void demuxer_service::clearCencKeys()
    {
        mCencDecrypter.reset();
        mCencDecryptAppliedLogged = false;
        mCencDecryptFailedLogged = false;
    }

    void demuxer_service::close()
    {
        AF_TRACE;
        CHECK_DEMUXER_V;
        mDemuxerPtr->Close();
    }

    int demuxer_service::start()
    {
        AF_TRACE;
        CHECK_DEMUXER;
        mDemuxerPtr->Start();
        return 0;
    }

    void demuxer_service::stop()
    {
        AF_TRACE;
        CHECK_DEMUXER_V;
        mDemuxerPtr->Stop();
    }

    void demuxer_service::preStop()
    {
        AF_TRACE;
        CHECK_DEMUXER_V;
        mDemuxerPtr->PreStop();
    }

    int demuxer_service::GetNbStreams() const
    {
        AF_TRACE;
        CHECK_DEMUXER;
        return mDemuxerPtr->GetNbStreams();
    }

    int demuxer_service::GetSourceMeta(Source_meta **meta)
    {
        AF_TRACE;
        CHECK_DEMUXER;
        return mDemuxerPtr->GetSourceMeta(meta);
    }

    int demuxer_service::GetMediaMeta(Media_meta *mediaMeta)
    {
        AF_TRACE;
        CHECK_DEMUXER;
        return mDemuxerPtr->GetMediaMeta(mediaMeta);
    }

    int demuxer_service::GetStreamMeta(Stream_meta *meta, int index, bool sub) const
    {
//        AF_TRACE;
        CHECK_DEMUXER;
        return mDemuxerPtr->GetStreamMeta(meta, index, sub);
    }

    int demuxer_service::OpenStream(int index)
    {
        AF_TRACE;
        CHECK_DEMUXER;
        return mDemuxerPtr->OpenStream(index);
    }

    void demuxer_service::CloseStream(int index)
    {
        AF_TRACE;
        CHECK_DEMUXER_V;
        mDemuxerPtr->CloseStream(index);
    }

    std::string demuxer_service::GetProperty(int index, const string &key)
    {
        AF_TRACE;

        if (nullptr == mDemuxerPtr) {
            return "";
        }

        return mDemuxerPtr->GetProperty(index, key);
    }

    int demuxer_service::SetOption(const std::string &key, const int64_t value)
    {
        if (nullptr != mDemuxerPtr) {
            return mDemuxerPtr->SetOption(key, value);
        }

        return 0;
    }

    bool demuxer_service::isPlayList()
    {
        if (nullptr == mDemuxerPtr) {
            return false;
        }

        return mDemuxerPtr->isPlayList();
    }

    void demuxer_service::flush()
    {
        AF_TRACE;
        CHECK_DEMUXER_V;
        mDemuxerPtr->flush();
        //  mDemuxerPtr->Seek(0,0,-1);
    }

    int demuxer_service::SwitchStreamAligned(int from, int to)
    {
        AF_TRACE;
        CHECK_DEMUXER;
        return mDemuxerPtr->SwitchStreamAligned(from, to);
    }

    int demuxer_service::GetNbSubStream(int index)
    {
        AF_TRACE;
        CHECK_DEMUXER;
        return mDemuxerPtr->GetNbSubStreams(index);
    }

    int demuxer_service::GetRemainSegmentCount(int index)
    {
        CHECK_DEMUXER;
        return mDemuxerPtr->GetRemainSegmentCount(index);
    }

    bool demuxer_service::isRealTimeStream(int index)
    {
        if (nullptr == mDemuxerPtr) {
            return false;
        }

        return mDemuxerPtr->isRealTimeStream(index);
    }

    bool demuxer_service::isWallclockTimeSyncStream(int index)
    {
        if (nullptr == mDemuxerPtr) {
            return false;
        }

        return mDemuxerPtr->isWallclockTimeSyncStream(index);
    }

    int64_t demuxer_service::getDurationToStartStream(int index)
    {
        if (nullptr == mDemuxerPtr) {
            return 0;
        }

        return mDemuxerPtr->getDurationToStartStream(index);
    }

    void demuxer_service::interrupt(int inter)
    {
        AF_TRACE;
        CHECK_DEMUXER_V;
        return mDemuxerPtr->interrupt(inter);
    }

    int64_t demuxer_service::Seek(int64_t us, int flags, int index)
    {
        AF_TRACE;

        if (mDemuxerPtr == nullptr) {
            mFirstSeekUs = us;
            return 0;
        } else {
            return mDemuxerPtr->Seek(us, flags, index);
        }
    }

    void demuxer_service::setDemuxerMeta(unique_ptr<DemuxerMeta> &meta)
    {
        mDemuxerMeta = std::move(meta);
    }

    int demuxer_service::GetStreamMeta(std::unique_ptr<streamMeta> &meta, int index, bool sub)
    {
        /*
         * 【必须值初始化（{}）】streamMeta 是**浅拷贝 + 析构里 releaseMeta()**
         * （framework/utils/mediaTypeInternal.cpp:8-16 → framework/utils/mediaFrame.c:23-64
         * 会 free extradata/lang/description/keyUrl/drmPssh/drmKeyId 并顺着 meta 链表走）。
         * 只要底层 demuxer 有一次"返回 >=0 但没把每个指针字段都写出来"（生产者只写自己
         * 关心的字段是很常见的写法），浅拷贝里剩下的就是栈垃圾，析构时就是对野指针 free
         * —— ntdll 直接报 0xc0000374 堆损坏（用户 2026-09-19 的三次闪退就是这个码）。
         * 零初始化后这些字段是 nullptr，free(nullptr) 是空操作，最坏也只是少释放。
         */
        Stream_meta Meta{};
        int ret = GetStreamMeta(&Meta, index, sub);

        if (ret < 0) {
            return ret;
        }

        meta = std::unique_ptr<streamMeta>(new streamMeta(&Meta));
        return 0;
    }

    IDemuxer *demuxer_service::getDemuxerHandle()
    {
        return mDemuxerPtr.get();
    }

    int demuxer_service::read_callback(void *arg, uint8_t *buffer, int size)
    {
        auto *pHandle = static_cast<demuxer_service *>(arg);

        if (pHandle->mPProbBuffer && pHandle->curPos < pHandle->mProbBufferSize) {
            int readSize = std::min(size, pHandle->mProbBufferSize - (int) pHandle->curPos);
            memcpy(buffer, pHandle->mPProbBuffer + (int) pHandle->curPos, static_cast<size_t>(readSize));
            pHandle->curPos += readSize;

            if (pHandle->curPos >= pHandle->mProbBufferSize) {
                delete[] pHandle->mPProbBuffer;
                pHandle->mPProbBuffer = nullptr;
            }

            return readSize;
        }

        if (pHandle->mReadCb) {
            return pHandle->mReadCb(pHandle->mReadArg, buffer, size);
        }

        return pHandle->mPDataSource->Read(buffer, size);
    }

    int64_t demuxer_service::seek_callback(void *arg, int64_t offset, int whence)
    {
        auto *pHandle = static_cast<demuxer_service *>(arg);

        //   AF_LOGE("test", "%s %lld \n", __func__, offset);
        if (pHandle->mPProbBuffer) {
            delete[] pHandle->mPProbBuffer;
            pHandle->mPProbBuffer = nullptr;
        }

        if (pHandle->mSeekCb) {
            return pHandle->mSeekCb(pHandle->mSeekArg, offset, whence);
        }

        return pHandle->mPDataSource->Seek(offset, whence);
    }

    int demuxer_service::open_callback(void *arg, const char *url, int64_t start, int64_t end)
    {
        auto *pHandle = static_cast<demuxer_service *>(arg);

        if (pHandle->mPProbBuffer) {
            delete[] pHandle->mPProbBuffer;
            pHandle->mPProbBuffer = nullptr;
        }

        if (pHandle->mPDataSource) {
            pHandle->mPDataSource->setRange(start, end);
            return pHandle->mPDataSource->Open(url);
        } else {
            return 0;
        }
    }

    void demuxer_service::interrupt_callback(void *arg, int inter)
    {
        auto *pHandle = static_cast<demuxer_service *>(arg);

        if (pHandle->mPDataSource) {
            return pHandle->mPDataSource->Interrupt(static_cast<bool>(inter));
        }
    }

    void demuxer_service::setSegmentList_callback(void *arg, const std::vector<mediaSegmentListEntry> &segments)
    {
        auto *pHandle = static_cast<demuxer_service *>(arg);

        if (pHandle->mPDataSource) {
            pHandle->mPDataSource->setSegmentList(segments);
        }
    }

    int64_t demuxer_service::getBufferDuration_callback(void *arg, int index)
    {
        auto *pHandle = static_cast<demuxer_service *>(arg);

        if (pHandle->mPDataSource) {
            return pHandle->mPDataSource->getBufferDuration();
        }
        return 0;
    }

    void demuxer_service::enableCache_callback(void *arg, const std::string &originUrl, bool enable)
    {
        auto *pHandle = static_cast<demuxer_service *>(arg);

        if (pHandle->mPDataSource) {
            return pHandle->mPDataSource->enableCache(originUrl, enable);
        }
    }
}
