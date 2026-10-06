//
// Created by moqi on 2018/1/23.
//

#include "file_data_source.h"
#include <fcntl.h>
#include <unistd.h>
#include <log.h>
#include <cerrno>

#define TAG "fileDataSource"

namespace Cicada {
    fileDataSource::fileDataSource()
    {
        mFd = 0;
    }

    fileDataSource::~fileDataSource()
    {
        Close();
    }

    int fileDataSource::Open(const string &path)
    {
        mPath = path;

        if (mPath.c_str() == nullptr) {
            return -1;
        }

        mFd = ::open(mPath.c_str(), O_RDONLY);

        if (mFd < 0) {
            AF_LOGE(TAG, "open %s error %d(%s)\n", mPath.c_str(), errno, strerror(errno));
            return mFd;
        }

        return 0;
    }

    void fileDataSource::Close()
    {
        if (mFd > 0) {
            ::close(mFd);
            mFd = 0;
        }
    }

    int64_t fileDataSource::Seek(int64_t offset, int whence)
    {
        if (mFd <= 0) {
            return -1;
        }

        return ::lseek(mFd, offset, whence);
    }

    ssize_t fileDataSource::Read(void *buf, size_t nbyte)
    {
        return ::read(mFd, buf, nbyte);
    }

    int64_t fileDataSource::getReadPosition()
    {
        if (mFd <= 0) {
            return -1;
        }

        const off_t position = ::lseek(mFd, 0, SEEK_CUR);

        if (position < 0) {
            return -1;
        }

        return static_cast<int64_t>(position);
    }

    int64_t fileDataSource::getTotalLength()
    {
        if (mFd <= 0) {
            return -1;
        }

        /* 必须先记住当前位置：SEEK_END 会改动读取游标，查完要还原，否则会把读取位置打乱。 */
        const off_t current = ::lseek(mFd, 0, SEEK_CUR);

        if (current < 0) {
            return -1;
        }

        const off_t end = ::lseek(mFd, 0, SEEK_END);
        ::lseek(mFd, current, SEEK_SET);

        if (end < 0) {
            return -1;
        }

        return static_cast<int64_t>(end);
    }
}
