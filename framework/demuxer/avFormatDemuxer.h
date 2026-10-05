//
// Created by moqi on 2019-07-08.
//

#ifndef FRAMEWORK_AVFORMATDEMUXER_H
#define FRAMEWORK_AVFORMATDEMUXER_H

#include <string>
#include <map>
#include <mutex>
#include <atomic>
#include <deque>
#include <vector>
#include "base/media/IAFPacket.h"
#include "AVBSF.h"
#include <demuxer/IDemuxer.h>
#include <af_config.h>
#include "demuxerPrototype.h"

#if AF_HAVE_PTHREAD

#include <utils/afThread.h>

#endif

#include <utils/CicadaType.h>

struct AVFormatContext;
struct AVIOContext;
struct AVDictionary;
typedef struct AVInputFormat AVInputFormat;
namespace Cicada {

    class CICADA_CPLUS_EXTERN avFormatDemuxer : public IDemuxer, protected demuxerPrototype {

    private:
        class AVStreamCtx {
        public:
            std::unique_ptr<IAVBSF> bsf{};
            bool opened{true};
            bool bsfInited{false};
            /*
             * 追加在成员末尾（增量 ABI 安全）：pre-create bsf 之前 codecpar 的
             * extradata 快照。bsf 的 init 会把 codecpar 改写成 Annex B，一旦发现这条流
             * 其实是加密的（包带 AV_PKT_DATA_ENCRYPTION_INFO），必须连 extradata
             * 一起还原成容器原形态，否则包与 meta 又会错位。
             */
            std::vector<uint8_t> preBsfExtraData{};
        };

    public:
        avFormatDemuxer();

        explicit avFormatDemuxer(const string &path);

        ~avFormatDemuxer() override;

        virtual int Open() override;

        int GetNbStreams() const override;

        int GetSourceMeta(Source_meta **meta) const override;

        int GetStreamMeta(Stream_meta *meta, int index, bool sub) const override;

        int GetMediaMeta(Media_meta *mediaMeta) const override;

        void Start() override;

        void PreStop() override;

        void Stop() override;

        void flush() override;

        void interrupt(int inter) override;

        void Close() final;

        int OpenStream(int index) override;

        void CloseStream(int index) override;

        int64_t Seek(int64_t us, int flags, int index) override;

        int ReadPacket(std::unique_ptr<IAFPacket> &packet, int index) override;

        virtual const std::string GetProperty(int index, const string &key) const override;

        bool isRealTimeStream(int index) override;

        bool isTSDiscontinue() override;

        const vector<streamIndexEntryInfo> &getStreamIndexEntryInfo() override;

        int64_t getBufferDuration(int index)  const override;

        /*
         * 覆盖 IDemuxer::setStreamEncrypted（该虚函数定义在 IDemuxer 虚函数列表末尾）。
         * 覆写的位置不影响 vtable 槽位，槽位只由基类声明顺序决定。
         */
        void setStreamEncrypted(bool encrypted) override
        {
            mStreamEncrypted = encrypted;
        }

        /*
         * 覆盖 IDemuxer::isDataExhausted()：只有 AVIO 真的读到范围末尾（av_read_frame 报
         * AVERROR_EOF，且 pb->error 既不是 EAGAIN 也不是负数）时才为真。见 avFormatDemuxer.cpp
         * 读路径里那个 `return 0;// EOS` 的位置：置真在那儿，读到任何包就置假。
         */
        bool isDataExhausted() const override
        {
            return mDataExhausted;
        }

    protected:
        explicit avFormatDemuxer(int dummy);

    private:

        Cicada::IDemuxer *clone(const string &uri, int type, const Cicada::DemuxerMeta *meta) override
        {
            return new avFormatDemuxer(uri);
        }

        bool is_supported(const string &uri, const uint8_t *buffer, int64_t size, int *type, const Cicada::DemuxerMeta *meta,
                          const Cicada::options *opts) override;

        int getType() override
        {
            return demuxer_type_bit_stream;
        }

        static avFormatDemuxer se;

    protected:
        int open(const AVInputFormat *in_fmt);

    private:
        static int interrupt_cb(void *opaque);

        void init();

        int createBsf(AVPacket *pkt, int index);

        /*
         * 撤掉已经建好的 bsf，并把 codecpar 的 extradata 还原成 bsf 改写之前的快照。
         * 只用于"按非加密提前建了 bsf、随后却收到加密包"的防御性回退。
         */
        void dropBsf(int index);

        /*
         * 记录 createBsf 即将改写 codecpar 之前的 extradata 快照（幂等，每次覆盖）。
         */
        void saveCodecParBeforeBsf(int index);

        int ReadPacketInternal(std::unique_ptr<IAFPacket> &packet);

        static inline int avio_callback_read(void *arg, uint8_t *buffer, int size);

        static inline int64_t avio_callback_seek(void *arg, int64_t offset, int whence);

#if AF_HAVE_PTHREAD

        int readLoop();

#endif
        inline int64_t getWorkAroundSeekPos(int64_t pos);


    protected:
        AVDictionary *mInputOpts = nullptr;
        std::string mProbeString{};
        AVFormatContext *mCtx = nullptr;
        int MAX_QUEUE_SIZE = 60; // about 500ms  video and audio packet
        bool mSecretDemxuer{false};
        std::string mDrmMagicKey{};
        std::atomic_bool mInterrupted{false};
        bool bOpened{false};
        int64_t mStartTime = INT64_MIN;

    private:
        std::map<int, std::unique_ptr<AVStreamCtx>> mStreamCtxMap{};
        AVIOContext *mPInPutPb = nullptr;
        std::deque<unique_ptr<IAFPacket>> mPacketQueue{};
        std::atomic_bool bEOS{false};
        std::atomic_bool bPaused{false};
        std::atomic_bool bExited{false};
        bool mNedParserPkt{false};

#if AF_HAVE_PTHREAD
        afThread *mPthread{nullptr};
        std::mutex mMutex{};
        std::mutex mQueLock{};
        std::condition_variable mQueCond{};
        atomic <int64_t> mError{0};
        mutable std::mutex mCtxMutex{};
#endif

        /*
         * 追加在成员末尾（增量 ABI 安全）：清单层在开流前给出的"这一段是否加密"提示。
         * false（默认）= 非加密，OpenStream 时就建 head 合并 bsf，保证 codecpar 从
         * 还没有任何包时起就已经是包的形态。true = 加密，保持第一个包上懒建的原行为。
         */
        bool mStreamEncrypted{false};

        /* 追加在成员末尾：AVIO 是否真的读到了范围末尾（见 isDataExhausted()）。 */
        bool mDataExhausted{false};

    };
}

#endif //FRAMEWORK_AVFORMATDEMUXER_H
