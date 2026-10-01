//
// Created by moqi on 2018/2/1.
//

#ifndef FRAMEWORK_DEMUXER_SERVICE_H
#define FRAMEWORK_DEMUXER_SERVICE_H
#define    MIN(a, b) (((a)<(b))?(a):(b))

#include "base/media/framework_type.h"
#include <data_source/IDataSource.h>
#include <functional>
#include <base/media/IAFPacket.h>
#include "utils/mediaFrame.h"
#include "utils/mediaTypeInternal.h"
#include "DemuxerMeta.h"
#include "IDemuxer.h"
#include <base/OptionOwner.h>
#include <memory>
#include <utils/CicadaType.h>
#include "sample_decrypt/ISampleDecryptor.h"
#include "sample_decrypt/CENCDecrypter.h"
#include <string>
#include <vector>
#include <demuxer/manifest/MediaManifest.h>
#define GEN_STREAM_ID(index, subId) (((subId) << 16) + (index))
#define GEN_SUB_STREAM_ID(index) (index >> 16)
#define GEN_STREAM_INDEX(index) (index & 0xFF)

namespace Cicada {

    class CICADA_CPLUS_EXTERN demuxer_service : public OptionOwner {

    public:
        explicit demuxer_service(IDataSource *pDataSource);

        void setNoFile(bool noFile)
        {
            mNoFile = noFile;
        }

        //     demuxer_service(string path);

        ~demuxer_service();

        void SetDataCallBack(demuxer_callback_read read, void *read_arg, demuxer_callback_seek seek,
                             void *seek_arg,
                             const char *uri);

        void setSampleDecryptor(ISampleDecryptor *decryptor)
        {
            mSDec = decryptor;
        };

        /*
         * ============ CENC 内容密钥注册（软件解密用，**可选的兜底**）============
         *
         * 【为什么需要它】样本级加密（CENC）的解密有两个可能的执行者：
         *   1. **平台解码器**（Android MediaCodec + MediaCrypto、OHOS DRM Kit）——
         *      密钥在安全世界里，CPU 看不到明文，**这是首选**；
         *   2. **内核自己**（CENCDecrypter，纯 CPU）—— 给**没有 CDM 的平台**兜底
         *      （桌面 Qt、以及任何没有硬件 DRM 的设备）。
         *
         * 【关键：注册了才解密】这两条路是互斥的，而"谁来解"必须由**上层**决定，
         * 不能由内核猜（内核不知道本机有没有可用 CDM）。所以这里的语义是：
         *   · **注册了**该 KID 的密钥 ⇒ 内核用 CENCDecrypter 就地解密，
         *     包交给解码器时已经是明文；
         *   · **没注册** ⇒ 内核**原样放行**（包上仍然带 AV_PKT_DATA_ENCRYPTION_INFO），
         *     由平台解码器拿 queueSecureInputBuffer 去解。
         * 于是"有 CDM 的平台走硬解、没有的走软解"这件事变成一条**数据判据**，
         * 不需要新开关，也不会出现"两边都解一遍"（解两遍等于把明文当密文再解一次）。
         *
         * 【尺寸】CENC 是就地加密：密文长度 == 明文长度，所以解密后包长不变，
         * 只有内容变。因此这里不需要处理 realloc。
         *
         * 返回 0 表示已登记（同一 KID 重复登记会替换）；参数非法返回 -EINVAL。
         */
        int setCencKey(const std::string &keyIdHex, const uint8_t *key, int keySize);

        // 该 KID 是否已登记软件密钥（用于上层判断"这条流内核会不会自己解"）。
        bool hasCencKey(const std::string &keyIdHex) const;

        // 清空已登记的 CENC 密钥（关流/切档时调用，避免上一档的密钥影响下一档）。
        void clearCencKeys();

        int createDemuxer(demuxer_type type);

        /**
         * Object-based playback: hand a unified MediaManifest object to the
         * demuxer service. After this call, createDemuxer(demuxer_type_manifest)
         * instantiates the manifest demuxer (no probing, no URL required).
         */
        void setManifestSource(std::unique_ptr<Manifest::MediaManifest> manifest);

        int initOpen(demuxer_type type = demuxer_type_unknown);


        int readPacket(std::unique_ptr<IAFPacket> &packet, int index = -1);

        void close();

        void flush();

        int start();

        void stop();

        void preStop();

        int64_t Seek(int64_t us, int flags, int index);

        static int read_callback(void *arg, uint8_t *buffer, int size);

        static int64_t seek_callback(void *arg, int64_t offset, int whence);

        static int open_callback(void *arg, const char *url, int64_t start, int64_t end);

        static void interrupt_callback(void *arg, int inter);

        static void setSegmentList_callback(void *arg, const std::vector<mediaSegmentListEntry> &segments);

        static int64_t getBufferDuration_callback(void *arg, int index);

        static void enableCache_callback(void *arg, const std::string &originUrl, bool enable);

        std::string GetProperty(int index, const std::string &key);

        int SetOption(const std::string &key, int64_t value);

        bool isPlayList();

        void setDemuxerCb(const std::function<void(std::string, std::string)> &func);

        void setDemuxerMeta(std::unique_ptr<DemuxerMeta> &meta);

    public:


        int GetNbStreams() const;

        int GetSourceMeta(Source_meta **meta);

        int GetMediaMeta(Media_meta *mediaMeta);

        attribute_deprecated
        int GetStreamMeta(Stream_meta *meta, int index, bool sub) const;

        int GetStreamMeta(std::unique_ptr<streamMeta> &meta, int index, bool sub);

        int OpenStream(int index);

        void CloseStream(int index);

        int GetNbSubStream(int index);

        int GetRemainSegmentCount(int index);
        
        bool isRealTimeStream(int index);

        bool isWallclockTimeSyncStream(int index);

        int64_t getDurationToStartStream(int index);

        int SwitchStreamAligned(int from, int to);

        void interrupt(int inter);

        void *getCodecPar(int streamIndex);

        IDemuxer *getDemuxerHandle();

    private:
        std::unique_ptr <IDemuxer> mDemuxerPtr {nullptr};
        IDataSource *mPDataSource = nullptr;

        demuxer_callback_read mReadCb = nullptr;
        void *mReadArg{nullptr};

        demuxer_callback_seek mSeekCb = nullptr;
        void *mSeekArg{nullptr};

        std::function<void(std::string, std::string)> mDemuxerCbfunc;

        uint8_t *mPProbBuffer = nullptr;
        int mProbBufferSize = 0;
        uint64_t curPos = 0;
        int64_t mFirstSeekUs = 0;
        bool mNoFile = false;

        ISampleDecryptor *mSDec = nullptr;

        /*
         * CENC 软件解密器。**只在内核确实要自己解时才存在**（见 setCencKey 的说明）：
         * 注册第一把密钥时才创建，clearCencKeys() 时销毁 —— 没有硬件 DRM 的平台
         * 会一直有它；有硬件 DRM 的平台（Android/OHOS 且有 CDM）永远不注册，
         * 于是它恒为 null，读取路径上零开销、零行为变化。
         *
         * 按本仓库惯例，新成员追加在类末尾。
         */
        std::unique_ptr<CENCDecrypter> mCencDecrypter{nullptr};

        // 一次性日志去重（同 HLSStream::mKeyFetchFailedLogged 的做法）：
        // "真的解了一次"与"注册了密钥却解不开"各只打一条，避免每包刷屏。
        bool mCencDecryptAppliedLogged{false};
        bool mCencDecryptFailedLogged{false};

        /*
         * 对一个包尝试 CENC 就地解密；返回**实际解了没有**。
         * 未注册密钥 / 包上没有加密信息 / 解密失败都返回 false（调用方不需要区分，
         * 因为三种情况的正确动作都是"把包原样交出去"）。
         */
        bool applyCencDecryption(IAFPacket &packet);

        // 包上是否带可用于查表的 key id（避免为"没有加密信息的普通包"做无谓的
        // side data 解析——普通包占绝大多数）。
        static bool packetHasEncryptionInfo(IAFPacket &packet);

        std::unique_ptr<Manifest::MediaManifest> mManifestSource{nullptr};

        std::unique_ptr<Cicada::DemuxerMeta> mDemuxerMeta;
    };
}

#endif //FRAMEWORK_DEMUXER_SERVICE_H
