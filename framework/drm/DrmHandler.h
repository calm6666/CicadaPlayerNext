//
// Created by SuperMan on 11/27/20.
//

#ifndef SOURCE_DRMHANDLER_H
#define SOURCE_DRMHANDLER_H

#include <cstdint>
#include <string>
#include <cstring>
#include <functional>
#include <map>
#include "DrmInfo.h"

namespace Cicada {

    class DrmRequestParam {
    public:
        DrmRequestParam() = default;

        ~DrmRequestParam() = default;

        std::string mDrmType{};
        void *mParam{nullptr};

        /**
         * Optional: the DRM licence challenge produced by the platform DRM
         * plugin, so an app-side callback can POST it to a licence server.
         * Empty when the platform's DRM stack hands the challenge to the
         * codec out-of-band (e.g. the Android MediaDrm path).
         */
        std::string mKeyRequest{};

        /** Optional: the licence server URL reported by the DRM plugin. */
        std::string mLicenseUrl{};
    };

    class DrmResponseData {
    public:
        DrmResponseData(char *data, int size) {
            if (data != nullptr && size > 0) {
                mData = static_cast<char *>(malloc(size));
                memcpy(mData, data, size);
                mSize = size;
            }
        };

        ~DrmResponseData() {
            if (mData != nullptr) {
                free(mData);
            }
        };

        const char* getData(int* size){
            *size = mSize;
            return mData;
        }

    private:
        int mSize{0};
        char *mData{nullptr};
    };

    class DrmHandler {

    public:

        DrmHandler(const DrmInfo &drmInfo);

        virtual ~DrmHandler() = default;

        void setDrmCallback(const std::function<DrmResponseData *(
                const DrmRequestParam &drmRequestParam)> &callback) {
            drmCallback = callback;
        }

        virtual bool isErrorState() {
            return false;
        }

    protected:
        DrmInfo drmInfo;

        std::function<DrmResponseData *(const DrmRequestParam &drmRequestParam)> drmCallback{
                nullptr};

    };
}


#endif //SOURCE_DRMHANDLER_H
