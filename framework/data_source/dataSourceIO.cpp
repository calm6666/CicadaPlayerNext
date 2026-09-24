//
// Created by moqi on 2018/4/24.
//

#include <utils/af_string.h>
#include "dataSourceIO.h"
/* AVERROR(ENOSYS)：seek 回调缺失时按 ffmpeg 的惯例报"不支持" */
#include <cerrno>
// FFmpeg 9.0：avio.h 不再间接引入 mem.h/error.h，需显式包含；
// mem.h 自身没有 extern "C" 包裹，必须手动包一层——否则 C++ 会 mangle
// av_malloc/av_freep 等符号，与 libffmpeg.so 里的 C 符号对不上（链接失败）
extern "C" {
#include <libavutil/mem.h>
#include <libavutil/error.h>
}

#define INITIAL_BUFFER_SIZE 32768
namespace Cicada {
    dataSourceIO::dataSourceIO(IDataSource *pDataSource) : mPDataSource(pDataSource)
    {
        init();
    }

    dataSourceIO::dataSourceIO(demuxer_callback_read read, demuxer_callback_seek seek, void *arg)
        : mUserRead(read), mUserSeek(seek), mUserArg(arg)
    {
        auto *read_buffer = static_cast<uint8_t *>(av_malloc(INITIAL_BUFFER_SIZE));
        mPb = avio_alloc_context(read_buffer, INITIAL_BUFFER_SIZE, 0, this, read_wrapper, nullptr, seek_wrapper);
    }

    dataSourceIO::~dataSourceIO()
    {
        if (mPb) {
            av_freep(&mPb->buffer);
            av_freep(&mPb);
        }
    }

    int dataSourceIO::read_callback(void *arg, uint8_t *buffer, int size)
    {
        auto *pHandle = static_cast<dataSourceIO *>(arg);
        //     AF_LOGE("read_callback", "%s %d \n", __func__, size);
        int ret = pHandle->mPDataSource->Read(buffer, size);
        return ret ? ret : AVERROR_EOF;
    }

    /*
     * 【回调版的 read 包装：把"读完"翻译成 AVERROR_EOF】
     *
     * 宿主的 read 回调用 **0** 表示读到结尾（`demuxer_service::read_callback` 就是这么写的：
     * 底层数据源 Read 返回 0 = EOF）。可 ffmpeg 的 avio 只认 **AVERROR_EOF**：
     *
     *   * `avio_read()`（ffmpeg 解复用器走的那条，见 aviobuf.c:645-650）遇到 0 会自己
     *     break，所以本地文件/普通流一直没问题 —— 这也是为什么这个坑藏了这么久；
     *   * 而 `avio_r8()`（逐字节读，aviobuf.c:606-613 + fill_buffer:551-564）遇到 0
     *     既不置 `eof_reached` 也不报错，`avio_feof()` 于是**永远是假**。
     *
     * 于是 `MPDParser::parse()`（dash/MPDParser.cpp:42 `while (!mDataSourceIO->isEOF())`）
     * 会**死循环**：没有日志、没有报错、也没有阻塞 IO —— 表现就是"选中频道后一直正在加载"，
     * 但进程还活着（点停止还有反应）。DASH 的 .mpd 和 HLS 的 .m3u8 都走这段代码。
     *
     * 上面那个 IDataSource 版的 read_callback 本来就是这么翻译的（`ret ? ret : AVERROR_EOF`），
     * 这个回调版漏了 —— 这里补上，让两条构造路径口径一致。
     */
    int dataSourceIO::read_wrapper(void *arg, uint8_t *buffer, int size)
    {
        auto *pHandle = static_cast<dataSourceIO *>(arg);

        if (pHandle == nullptr || pHandle->mUserRead == nullptr) {
            return AVERROR_EOF;
        }

        const int ret = pHandle->mUserRead(pHandle->mUserArg, buffer, size);

        /* 0 = 宿主说的"读完了"；负数原样透出去（那是真的错误，交给 avio 记 error） */
        return ret ? ret : AVERROR_EOF;
    }

    int64_t dataSourceIO::seek_wrapper(void *arg, int64_t offset, int whence)
    {
        auto *pHandle = static_cast<dataSourceIO *>(arg);

        if (pHandle == nullptr || pHandle->mUserSeek == nullptr) {
            return AVERROR(ENOSYS);
        }

        return pHandle->mUserSeek(pHandle->mUserArg, offset, whence);
    }

    int64_t dataSourceIO::seek_callback(void *arg, int64_t offset, int whence)
    {
        auto *pHandle = static_cast<dataSourceIO *>(arg);
        //      AF_LOGE("seek_callback", "%s %lld \n", __func__, offset);
        return pHandle->mPDataSource->Seek(offset, whence);
    }

    int dataSourceIO::init()
    {
        auto *read_buffer = static_cast<uint8_t *>(av_malloc(INITIAL_BUFFER_SIZE));
        mPb = avio_alloc_context(read_buffer, INITIAL_BUFFER_SIZE, 0, this, read_callback, nullptr, seek_callback);
        return 0;
    }

    static int cicada_get_line(AVIOContext *s, char *buf, int maxlen)
    {
        int i = 0;
        char c;

        do {
            c = (char) avio_r8(s);

            if (c && i < maxlen - 1) {
                buf[i++] = c;
            }
        } while (c != '\n' && c != '\r' && c);

        if (c == '\r' && avio_r8(s) != '\n' && !avio_feof(s)) {
            avio_skip(s, -1);
        }

        buf[i] = 0;
        return i;
    }

    int dataSourceIO::get_line(char *buf, int maxlen)
    {
        int len = cicada_get_line(mPb, buf, maxlen);

        while (len > 0 && AfString::isSpace(buf[len - 1])) {
            buf[--len] = '\0';
        }

        return len;
    }

    int64_t dataSourceIO::seek(int64_t offset, int whence)
    {
        return avio_seek(mPb, offset, whence);
    }

    bool dataSourceIO::isEOF()
    {
        return static_cast<bool>(avio_feof(mPb));
    }

    char dataSourceIO::readChar()
    {
        return avio_r8(mPb);
    }
}


