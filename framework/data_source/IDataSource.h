//
// Created by moqi on 2018/1/23.
//

#ifndef FRAMEWORK_DATASOUTCE_H
#define FRAMEWORK_DATASOUTCE_H

#include <atomic>
#include <base/OptionOwner.h>
#include <string>
#include <utils/CicadaJSON.h>
#include <utils/CicadaType.h>
#include <vector>

namespace Cicada {

    enum {
        SEEK_SIZE = 0x10000,
    };

    struct mediaSegmentListEntry {
        std::string url;
        int64_t duration = 0;

        mediaSegmentListEntry(const std::string &_url, int64_t _duration) : url(_url), duration(_duration)
        {}
    };

    class IMediaInfoProvider {
    public:
        virtual int64_t estimateExclusiveEndPositionBytes(const std::string &url, int64_t timeMicSec, int64_t totalLength) = 0;
        virtual int64_t estimatePlayTimeMicSec(const std::string &url, int64_t filePosition, int64_t totalLength) = 0;
        virtual std::pair<int64_t, int64_t> estimatePlayTimeMicSecRange(const std::pair<int64_t, int64_t> &fileRange) = 0;
        virtual bool isStreamsEncodedSeparately(const std::string &url) = 0;
    };

    typedef std::string (*UrlHashCB)(const char *, void *userData);

    class IDataSource : public OptionOwner {
    public:
        class Listener {
        public:
            enum NetWorkRetryStatus {
                NetWorkRetryStatusPending,
                NetWorkRetryStatusRetry,
//                NetWorkRetryStatusAbort,
            };

            enum bitStreamType {
                bitStreamTypeMedia,
            };


            enum NetworkEvent {
                networkEvent_startConnect,
                networkEvent_connected,
                networkEvent_disconnect,
                networkEvent_error,
                networkEvent_eos,
                networkEvent_retry,
                networkEvent_resume,
                networkEvent_exit,
            };

            virtual void onNetworkEvent(const std::string &url, const CicadaJSONItem &eventParams)
            {}

            virtual NetWorkRetryStatus onNetWorkRetry(int error) = 0;

            virtual void onNetWorkConnected()
            {
            }
            /*
             * return, whether stop report to other
             */
            virtual bool onNetWorkInPut(uint64_t size, bitStreamType type)
            {
                return false;
            }
        };

        enum speedLevel {
            speedLevel_mem,
            speedLevel_local,
            speedLevel_remote,
        };

        class SourceConfig {
        public:
            enum IpResolveType { IpResolveWhatEver, IpResolveV4, IpResolveV6 };

        public:
            int low_speed_limit{1};
            int low_speed_time_ms{15000};
//        int64_t max_time_ms{0};
            int connect_time_out_ms{15000};
            int so_rcv_size{0};
            std::string http_proxy = "";
            std::string refer = "";
            std::string userAgent = "";
            std::vector<std::string> customHeaders;
            Listener *listener = nullptr;
            IpResolveType resolveType{IpResolveWhatEver};
            bool enableLog{true};

            std::string toString();
        };

        const static uint64_t flag_report_speed = 1 << 0;

        explicit IDataSource(std::string url);

        virtual ~IDataSource() = default;

        virtual int setRange(int64_t start, int64_t end);

        virtual void setMediaInfoProvider(IMediaInfoProvider *provider);

        virtual void setPost(bool post, int64_t size, const uint8_t *data);

        virtual int Open(int flags) = 0;

        virtual int Open(const std::string &url);

        virtual void Close() = 0;

        virtual int64_t Seek(int64_t offset, int whence) = 0;

        virtual int Read(void *buf, size_t nbyte) = 0;

        virtual void Interrupt(bool interrupt);

        virtual std::string Get_error_info(int error);

        virtual void Set_config(SourceConfig &config);

        virtual void Get_config(SourceConfig &config);

        virtual std::string GetOption(const std::string &key);

        virtual const options *GetOption();

        virtual std::string GetUri();

        virtual std::string GetOriginUri();

        virtual speedLevel getSpeedLevel();

        virtual void setSegmentList(const std::vector<mediaSegmentListEntry> &segments)
        {}

        virtual void enableCache(const std::string &originUrl, bool enable)
        {}
        virtual uint64_t getFlags()
        {
            return 0;
        }

        virtual int64_t getBufferDuration()
        {
            return 0;
        }

        virtual void setUrlToUniqueIdCallback(UrlHashCB onUrlHash, void *userData)
        {}

        virtual void clearCache()
        {}

        /*
         * 【已读入的字节范围】本地/单文件源用这两个接口回答"读到哪了、总长多少"。
         *
         * 为什么需要它们：对外上报的"缓冲位置"目前只能拿 getBufferDuration()，而它的实现
         * 依赖缓存模块（本地不走缓存时默认返回 0）⇒ 缓冲值退化成读前窗口里的包队列深度，
         * 既不等于"已读入多少"，又会随队列锯齿抖动。有了已读偏移与总长，播放器就能把
         * "已读入的字节范围"按线性比例映射到时间轴，得到一个既真实又单调的缓冲位置
         * （往回 seek 后文件游标也回到落点 ⇒ 缓冲位置同步变小）。
         *
         * 默认 -1 表示"该源不提供"（分片流/网络流的"已缓冲"由分片缓存表达，不用这两个值）：
         * 此时播放器退回原有算法，行为与以前一致。
         *
         * 本虚函数留在虚函数列表末尾（理由同 IDemuxer 里 SeekStream / setStreamEncrypted 的约定），
         * 以后新增同类接口继续往这里追加。
         */
        virtual int64_t getReadPosition()
        {
            return -1;
        }

        virtual int64_t getTotalLength()
        {
            return -1;
        }

    protected:
        std::atomic_bool mInterrupt{false};
        SourceConfig mConfig{};
        /**
         * the main/first url
         */
        std::string mUri{};
        int64_t rangeStart{INT64_MIN};
        int64_t rangeEnd{INT64_MIN};

        bool mBPost{false};
        const uint8_t *mPostData{nullptr};
        int64_t mPostSize{0};

        IMediaInfoProvider *mMediaInfoProvider{nullptr};
    };
}


#endif //FRAMEWORK_DATASOUTCE_H
