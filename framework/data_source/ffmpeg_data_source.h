//
// Created by moqi on 2018/1/24.
//

#ifndef FRAMEWORK_FFMPEGDATASOURCE_H
#define FRAMEWORK_FFMPEGDATASOURCE_H
extern "C" {
#include <libavformat/url.h>
#include <libavformat/avio.h>
}

#include "IDataSource.h"
#include "dataSourcePrototype.h"
#if defined(WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace Cicada {
    class ffmpegDataSource : public IDataSource, private dataSourcePrototype {
    public:
        static bool probe(const std::string &path)
        {
            return path.compare(0, 7, "rtmp://") == 0 || (access(path.c_str(), 0) == 0);
        };

        explicit ffmpegDataSource(const std::string &url);

        ~ffmpegDataSource() override;

        int Open(int flags) override;

        void Close() override;

        int64_t Seek(int64_t offset, int whence) override;

        int Read(void *buf, size_t nbyte) override;

        void Interrupt(bool interrupt) override;

        std::string Get_error_info(int error) override;

        std::string GetOption(const std::string &key) override;
        uint64_t getFlags() override
        {
            return flag_report_speed;
        }

        /*
         * 已读入范围：本地文件（probe 就是"路径能 access 到"）与直连网络单文件都由本类提供，
         * 用 ffmpeg 自己的 AVIO 回答这两个值：
         *   avio_size() = 整个输入的总长（本地就是文件大小，容器/协议层已经算好）；
         *   avio_tell() = 当前读取游标，也就是"已读到的字节偏移"。
         * 都是只读查询，不动读取位置。基类默认返回 -1（分片流的"已缓冲"由分片缓存表达），
         * 这里给真值之后，播放器才能按"已读入的字节范围"上报缓冲位置
         * （往回 seek 时文件游标回退 ⇒ 缓冲位置同步变小）。
         */
        int64_t getReadPosition() override
        {
            if (mPuc == nullptr) {
                return -1;
            }

            return avio_tell(mPuc);
        }

        int64_t getTotalLength() override
        {
            if (mPuc == nullptr) {
                return -1;
            }

            return avio_size(mPuc);
        }

    private:
        static int check_interrupt(void *pHandle);

    private:
        explicit ffmpegDataSource(int dummy) : IDataSource("")
        {
            addPrototype(this);
        }

        Cicada::IDataSource *clone(const std::string &uri) override
        {
            return new ffmpegDataSource(uri);
        };

        bool is_supported(const std::string &uri, int flags) override
        {
            if (flags != 0) {
                return false;
            }
            return probe(uri);
        };

        static ffmpegDataSource se;

    private:
        AVIOContext *mPuc{};
        AVIOInterruptCB mInterruptCB{};
        int mInterrupted{};
        char mErrorMsg[64]{};   // AV_ERROR_MAX_STRING_SIZE removed in FFmpeg 6.0（原值 64）
        bool isNetWork{true};
    };
}


#endif //FRAMEWORK_FFMPEGDATASOURCE_H
