//
// Created by moqi on 2018/4/24.
//

#ifndef FRAMEWORK_DATA_SOURCE_UTILS_H
#define FRAMEWORK_DATA_SOURCE_UTILS_H
extern "C" {
#include <libavformat/avio.h>
};

#include "base/media/framework_type.h"
#include "IDataSource.h"

namespace Cicada{ ;

    class dataSourceIO {
    public:
        dataSourceIO(IDataSource *pDataSource);

        dataSourceIO(demuxer_callback_read read, demuxer_callback_seek seek, void *arg);

        ~dataSourceIO();

        int get_line(char *buf, int maxlen);

        int64_t seek(int64_t offset, int whence);

        char readChar();

        bool isEOF();


    private:
        int init();

        static int read_callback(void *arg, uint8_t *buffer, int size);

        static int64_t seek_callback(void *arg, int64_t offset, int whence);

        /*
         * 【回调版专用】把宿主的 read/seek 回调包一层，见 .cpp 里 read_wrapper 的说明：
         * 宿主的 read 回调用 **0** 表示"读完了"（干净的 EOF），而 ffmpeg 的 avio 只认
         * AVERROR_EOF —— 不翻译的话 avio_r8() 那条路（逐字节读清单）会死循环。
         */
        static int read_wrapper(void *arg, uint8_t *buffer, int size);

        static int64_t seek_wrapper(void *arg, int64_t offset, int whence);

        IDataSource *mPDataSource{nullptr};
        AVIOContext *mPb = nullptr;

        /* 回调版：宿主给的回调（read_wrapper/seek_wrapper 转发到它们） */
        demuxer_callback_read mUserRead{nullptr};
        demuxer_callback_seek mUserSeek{nullptr};
        void *mUserArg{nullptr};
    };
}


#endif //FRAMEWORK_DATA_SOURCE_UTILS_H
