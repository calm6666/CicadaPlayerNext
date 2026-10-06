//
// Created by moqi on 2018/1/25.
//

#ifndef FRAMEWORK_DATASOURCE_CURLSOURCE_H
#define FRAMEWORK_DATASOURCE_CURLSOURCE_H

#include "CURLConnection.h"
#include "data_source/IDataSource.h"
#include "data_source/dataSourcePrototype.h"
#include "utils/ringBuffer.h"
#include <condition_variable>
#include <curl/multi.h>
#include <mutex>
#include <utils/globalNetWorkManager.h>

namespace Cicada {

    class CurlDataSource : public IDataSource, private dataSourcePrototype, private globalNetWorkManager::globalNetWorkManagerListener {
    public:
        static bool probe(const std::string &path);

        explicit CurlDataSource(const std::string &url);

        ~CurlDataSource() override;

        int Open(int flags) override;

        int Open(const std::string &url) override;

        void Close() override;

        int64_t Seek(int64_t offset, int whence) override;

        int Read(void *buf, size_t nbyte) override;

        std::string GetOption(const std::string &key) override;

        void Interrupt(bool interrupt) override;

        std::string GetUri() override;
        uint64_t getFlags() override
        {
            return flag_report_speed;
        }

        /*
         * 已读入范围（本地单文件 / 可随机访问源，走 curl 的 file:// 也一样）：
         *   总长 = 建连时记下的 mFileSize；读取游标 = 连接的 tell()（就是已读到的字节偏移）。
         * 都是只读查询，不动读取位置。基类默认返回 -1，这里给出真值之后，播放器才能按
         * "已读入的字节范围"上报缓冲位置（往回 seek 时游标回退 ⇒ 缓冲位置同步变小）。
         */
        int64_t getReadPosition() override
        {
            if (mPConnection == nullptr) {
                return -1;
            }

            return mPConnection->tell();
        }

        int64_t getTotalLength() override
        {
            return mFileSize;
        }

    private:

        CURLConnection *initConnection();

        int64_t TrySeekByNewConnection(int64_t offset);

        void fillConnectInfo();

        int curl_connect(CURLConnection *pConnection, int64_t filePos);

    private:
        explicit CurlDataSource(int dummy);

        IDataSource *clone(const std::string &uri) override
        {
            return new Cicada::CurlDataSource(uri);
        }

        bool is_supported(const std::string &uri, int flags) override
        {
            if (flags != 0) {
                return false;
            }
            return probe(uri);
        }

        static CurlDataSource se;

        void closeConnections(bool current);

        void OnReconnect() override;

    private:
        const static int max_connection = 1;
        std::string mLocation;
        int64_t mFileSize = -1;
        CURLConnection *mPConnection = nullptr;
        Cicada::IDataSource::SourceConfig *pConfig = nullptr;
        //   pthread_t multi_thread_id;
        int multisession = 0;
        std::string mIpStr;
        int reTryCount = 0;

        //add for custom http headers;
        struct curl_slist *headerList = nullptr;
        std::mutex mSleepMutex;
        std::condition_variable mSleepCondition;
        int64_t mOpenTimeMS = 0;
        std::mutex mMutex;
        std::string mConnectInfo;
        bool mBDummy = false;
        std::vector<CURLConnection *>* mConnections {nullptr};
        bool mEnableLog{true};
        std::atomic<bool> mNeedReconnect{false};
    };
}

#endif //FRAMEWORK_DATASOURCE_CURLSOURCE_H
