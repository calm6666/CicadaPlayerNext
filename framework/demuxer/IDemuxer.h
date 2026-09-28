//
// Created by moqi on 2018/2/1.
//

#ifndef FRAMEWORK_DEMUXER_H
#define FRAMEWORK_DEMUXER_H

#include "base/media/framework_type.h"
#include "play_list/playList.h"
#include "utils/AFMediaType.h"

#include "DemuxerMetaInfo.h"
#include <base/IDCA.h>
#include <base/OptionOwner.h>
#include <base/media/IAFPacket.h>
#include <data_source/IDataSource.h>
#include <functional>
#include <string>
#include <utility>
#include <utils/CicadaType.h>
#include <utils/mediaTypeInternal.h>

using namespace std;

namespace Cicada {
    typedef enum demuxer_type {
        demuxer_type_unknown = 0,
        demuxer_type_playlist,
        demuxer_type_bit_stream,
        demuxer_type_webvtt,
        demuxer_type_manifest,  // object-based playback (MediaManifest JSON/struct)
    } demuxer_type;

    typedef enum header_type {
        header_type_no_touch,
        header_type_merge,
        header_type_extract,
    } header_type;

    typedef enum client_buffer_level {
        client_buffer_level_unknown,
        client_buffer_level_low,
        client_buffer_level_normal,
        client_buffer_level_low_full,
    } client_buffer_level;

    typedef void (*demuxer_callback_setSegmentList)(void *arg, const std::vector<Cicada::mediaSegmentListEntry> &segments);
    typedef int64_t (*demuxer_callback_getBufferDuration)(void *arg, int index);
    typedef void (*demuxer_callback_enableCache)(void *arg, const std::string &originUrl, bool enable);

    class CICADA_CPLUS_EXTERN IDemuxer : public OptionOwner, public IDCA, public IMediaInfoProvider {
    public:
        class streamIndexEntryInfo {
        public:
            struct entryInfo {
                int64_t mPos;
                int64_t mTimestamp;
                bool mKey;
                bool mDiscard;
                int32_t mSize;
                entryInfo(int64_t pos, int64_t timestamp, bool key, bool discard, int32_t size)
                    : mPos(pos), mTimestamp(timestamp), mKey(key), mDiscard(discard), mSize(size)
                {}
            };

        public:
            streamIndexEntryInfo() = default;
            ~streamIndexEntryInfo() = default;

        public:
            int64_t mDuration{0};
            Stream_type type{STREAM_TYPE_UNKNOWN};
            std::vector<entryInfo> mEntry;
        };

    public:
        IDemuxer();

        explicit IDemuxer(string path);

        virtual void setDataSourceConfig(const IDataSource::SourceConfig &config)
        {
            sourceConfig = config;
        };

        ~IDemuxer() override;

        void SetDataCallBack(demuxer_callback_read read, demuxer_callback_seek seek, demuxer_callback_open open,
                             demuxer_callback_interrupt_data inter, demuxer_callback_setSegmentList setSegmentList,
                             demuxer_callback_getBufferDuration getBufferDuration, demuxer_callback_enableCache enableCache, void *arg);

        void setMeta(DemuxerMetaInfo *metaInfo)
        {
            mMetaInfo = metaInfo;
        }

        virtual int Open() = 0;


        /**
         *
         * @param packet
         * @param index
         * @return
         *          FRAMEWORK_ERROR_NEW_FORMAT: new header data of the packet index
         */

        virtual int ReadPacket(std::unique_ptr<IAFPacket> &packet, int index) = 0;

        virtual void Close() = 0;

        virtual void Start() = 0;

        virtual void Stop() = 0;

        /**
         * for some udp demuxer to reconnect to server when network changed
         */
        virtual void Reload()
        {}

        virtual void PreStop(){};

        virtual void flush() = 0;

        virtual int64_t Seek(int64_t us, int flags, int index) = 0;

        virtual int GetNbStreams() const = 0;

        virtual int GetNbSubStreams(int index) const
        {
            return -1;
        };

        virtual int GetRemainSegmentCount(int index)
        {
            return -1;
        }

        virtual int GetSourceMeta(Source_meta **meta) const = 0;

        attribute_deprecated virtual int GetStreamMeta(Stream_meta *meta, int index, bool sub) const = 0;

        virtual int GetStreamMeta(unique_ptr<streamMeta> &meta, int index, bool sub) const;

        virtual int GetMediaMeta(Media_meta *mediaMeta) const = 0;

        virtual int OpenStream(int index) = 0;

        virtual void CloseStream(int index) = 0;

        virtual int SwitchStreamAligned(int from, int to)
        {
            return -1;
        }


        virtual void interrupt(int inter) = 0;

        virtual const playList *GetPlayList()
        {
            return nullptr;
        }

        virtual bool isPlayList() const
        {
            return false;
        }

        virtual int64_t getMaxGopTimeUs()
        {
            return INT64_MIN;
        }

        virtual void setDataSourceIO()
        {}

        virtual void *getCodecPar(int streamIndex)
        {
            return nullptr;
        }

        virtual const std::string GetProperty(int index, const string &key) const
        {
            return "";
        }

        virtual int SetOption(const std::string &key, const int64_t value)
        {
            return 0;
        }

        virtual int SetOption(const std::string &key, const std::string &value)
        {
            return 0;
        }

        virtual void setBitStreamFormat(header_type vMergeHeader, header_type aMergeHeader)
        {
            mMergeVideoHeader = vMergeHeader;
            mMergeAudioHeader = aMergeHeader;
        }

        virtual void setDemuxerCb(std::function<void(std::string, std::string)> func)
        {
            mDemuxerCbfunc = func;
        }

        virtual int64_t getBufferDuration(int index) const
        {
            return 0;
        }

        virtual bool isLowLatency() const
        {
            return false;
        }

        int invoke(int cmd, const std::string &content) override
        {
            return 0;
        }

        virtual std::string getName() const
        {
            return mName;
        }

        virtual bool isRealTimeStream(int index)
        {
            return false;
        }

        virtual bool isWallclockTimeSyncStream(int index)
        {
            return false;
        }

        virtual int64_t getDurationToStartStream(int index)
        {
            return 0;
        }
        virtual bool isTSDiscontinue() = 0;

        virtual const vector<streamIndexEntryInfo> &getStreamIndexEntryInfo()
        {
            return mEntryInfos;
        }

        virtual void setUrlToUniqueIdCallback(UrlHashCB cb, void *userData)
        {}

        virtual UTCTimer *getUTCTimer()
        {
            return nullptr;
        }

        virtual void setClientBufferLevel(client_buffer_level level)
        {}

        /*
         * 【按流 re-position】把**单个流**的后续投递位置重新定位到 us（绝对微秒，与 Seek 同一时间轴），
         * 其它流的读取位置与已经排队的数据都不受影响。
         *
         * 注意：本虚函数**必须留在虚函数列表末尾**（与成员偏移契约同理）—— 插在中间会移动
         * 其后所有虚函数的 vtable 槽位，而本工程增量构建不记录头文件依赖，未重编的旧 TU
         * 会按错槽位调用（崩溃）。新增同类接口时也继续往这里追加。
         *
         * 默认实现返回 -1（不支持）—— 与本文件里 GetNbSubStreams / GetRemainSegmentCount
         * 表达"不支持"的方式一致。**当前没有任何解复用器覆写它**，所以这个接口的加入对
         * 现有行为（HLS/DASH/单文件容器）零影响；调用方必须按返回值回退到既有防线。
         *
         * 实现本接口必须先满足下面三条，任何一条做不到就**继续返回负值**：
         *   1. 只改变 index 指定流的投递位置；
         *   2. 不得移动共享容器位置、不得清空其它流的已排队包、不得暂停或重启读线程；
         *   3. 可被消息线程调用（与其它控制接口同一线程模型）。
         *
         * 为什么默认不支持：单文件容器（avFormatDemuxer）的定位原语 avformat_seek_file
         * 移动的是**整个容器上下文**，FFmpeg 没有"只动音频"的能力；用它满足第 2 条是不可能的
         * （会把视频包从新位置重新投递，造成 PTS 倒退与重复）。真要支持，只能为音频流单独
         * 持有一份解复用/读取上下文（另一份 AVFormatContext 与 IO），那是一次独立的大改动。
         */
        virtual int SeekStream(int64_t us, int flags, int index)
        {
            (void) us;
            (void) flags;
            (void) index;
            return -1;
        }

        /*
         * 【加密形态提示】在开流之前，把"这条流的包会不会带
         * AV_PKT_DATA_ENCRYPTION_INFO（即样本级加密、DRM）"告诉解复用器。
         *
         * 为什么需要这个接口：head 合并 bsf 必须在**第一个包之前**决定建不建。
         * 因为 bsf 的 init 会把 codecpar 的 extradata 改写成另一种 NAL 形态
         * （AVBSF::init 最后一步是 avcodec_parameters_copy(codecpar, par_out)），
         * 而 GetStreamMeta 给出的 extradata 就是这份 codecpar。要是 bsf 拖到第一个包
         * 才建，那么在"demuxer 刚建好、还没出包"的窗口里取到的 meta 还是容器的
         * hvcC/AVCC，随后真正投递的包却已经是 bsf 转出来的 Annex B，解码器按长度前缀
         * 去拆 Annex B 的包必然失败：日志刷 re-assert、一帧不出、画面冻死。
         *
         * 而"包是否带加密信息"只有清单层知道（HLS 的 EXT-X-KEY、DASH 的
         * ContentProtection），容器本身要等到第一个包才看得出来，所以由清单层在
         * 开流前告知：默认 false 表示非加密，解复用器可以在 OpenStream 时就建 bsf；
         * 传 true 表示加密，保持"第一个包上懒建"的原行为。
         *
         * 这个提示只是提前量，判错不丢正确性：ReadPacketInternal 里还有一道
         * "见到带加密信息的包就撤掉 bsf 并把 codecpar 还原"的防御性回退。
         *
         * 默认实现忽略提示：建 head 合并 bsf 的只有 avFormatDemuxer，它覆写本函数
         * （提示存放在那里，不占基类的成员，基类布局一个字节都不动）。转发型解复用器
         * （playList_demuxer、ManifestDemuxer）由各自的流对象把提示直接设到内层
         * demuxer 上，因此也不需要在这里存。
         *
         * 本虚函数必须留在虚函数列表末尾（理由同上面的 SeekStream），
         * 以后新增同类接口继续往这里追加。
         */
        virtual void setStreamEncrypted(bool encrypted)
        {
            (void) encrypted;
        }

    public:
        int64_t estimateExclusiveEndPositionBytes(const string &url, int64_t timeMicSec, int64_t totalLength) override;

        int64_t estimatePlayTimeMicSec(const string &url, int64_t filePosition, int64_t totalLength) override;

        std::pair<int64_t, int64_t> estimatePlayTimeMicSecRange(const pair<int64_t, int64_t> &fileRange) override;

        bool isStreamsEncodedSeparately(const string &url) override;

    protected:
        demuxer_callback_read mReadCb{nullptr};
        demuxer_callback_seek mSeekCb{nullptr};
        demuxer_callback_open mOpenCb{nullptr};
        demuxer_callback_interrupt_data mInterruptCb{nullptr};
        demuxer_callback_setSegmentList mSetSegmentList{nullptr};
        demuxer_callback_getBufferDuration mGetBufferDuration{nullptr};
        demuxer_callback_enableCache mEnableCache{nullptr};
        void *mUserArg{nullptr};
        std::function<void(std::string, std::string)> mDemuxerCbfunc;
        string mPath{};
        IDataSource::SourceConfig sourceConfig{};

        header_type mMergeVideoHeader = header_type ::header_type_no_touch;
        header_type mMergeAudioHeader = header_type ::header_type_no_touch;

        DemuxerMetaInfo *mMetaInfo = nullptr;
        std::string mName = "IDemuxer";
        std::vector<streamIndexEntryInfo> mEntryInfos;
    };
}// namespace Cicada

#endif//FRAMEWORK_DEMUXER_H
