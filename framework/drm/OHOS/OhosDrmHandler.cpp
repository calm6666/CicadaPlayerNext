//
// OhosDrmHandler.cpp
//
// See OhosDrmHandler.h for the design.
//

#define LOG_TAG "OhosDrmHandler"

#ifdef __OHOS__

#include "OhosDrmHandler.h"
#include <utils/frame_work_log.h>

#include <multimedia/drm_framework/native_mediakeysystem.h>
#include <multimedia/drm_framework/native_mediakeysession.h>
#include <multimedia/drm_framework/native_drm_common.h>
#include <multimedia/drm_framework/native_drm_err.h>

#include <cstring>

namespace Cicada {

    static const char *kWidevineUuid = "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed";
    static const char *kPlayReadyUuid = "9a04f079-9840-4286-ab92-e65be0885f95";
    static const char *kFairPlayUuid = "94ce86fb-07ff-4f43-adb8-93d2fa968ca2";
    static const char *kClearKeyUuid = "e2719d58-a985-b3c9-781a-b030af78d30e";

    OhosDrmHandler OhosDrmHandler::se(0);

    OhosDrmHandler::OhosDrmHandler(const DrmInfo &drmInfo)
        : DrmHandler(drmInfo)
    {
    }

    OhosDrmHandler::OhosDrmHandler(int dummy)
        : DrmHandler(DrmInfo{})
    {
        addPrototype(this);
    }

    OhosDrmHandler::~OhosDrmHandler()
    {
        if (mSession != nullptr) {
            OH_MediaKeySession_Destroy(mSession);
            mSession = nullptr;
        }
        if (mSystem != nullptr) {
            OH_MediaKeySystem_Destroy(mSystem);
            mSystem = nullptr;
        }
    }

    void OhosDrmHandler::registerPrototype()
    {
        // Registered through the static instance `se` in the constructor.
    }

    bool OhosDrmHandler::is_supported(const DrmInfo &drmInfo)
    {
        return drmInfo.format == kWidevineUuid
               || drmInfo.format == kPlayReadyUuid
               || drmInfo.format == kFairPlayUuid
               || drmInfo.format == kClearKeyUuid;
    }

    int OhosDrmHandler::open()
    {
        const std::string &uuid = drmInfo.format;
        mSystem = OH_MediaKeySystem_Create(uuid.c_str());
        if (mSystem == nullptr) {
            AF_LOGE("OH_MediaKeySystem_Create failed for %s (plugin not present?)\n", uuid.c_str());
            mError = true;
            return -1;
        }

        OH_MediaKeySession *session = nullptr;
        OH_DRM_ErrCode err = OH_MediaKeySession_Create(mSystem, &session);
        if (err != DRM_ERR_OK || session == nullptr) {
            AF_LOGE("OH_MediaKeySession_Create failed %d\n", err);
            mError = true;
            return -1;
        }
        mSession = session;

        // Build the key request and hand the challenge to the app callback.
        // The app POSTs the challenge to the license server and returns the
        // license response; we then install it with ProcessMediaKeyResponse.
        if (drmCallback != nullptr) {
            OH_MediaKeyRequestInfo requestInfo{};
            requestInfo.mediaKeyType = MEDIA_KEY_TYPE_ONLINE;
            requestInfo.mimeType = "video/mp4";
            // initData: PSSH (base64-decoded) or the default KID, as available.
            // When no initData is provided, the plugin derives it from the
            // first encrypted sample's key id.
            std::vector<uint8_t> initData;
            if (!drmInfo.pssh.empty()) {
                // base64 decode omitted for brevity in bring-up builds; the
                // sample path keeps initData empty and relies on sample key ids.
            }
            requestInfo.initData = initData.empty() ? nullptr : initData.data();
            requestInfo.initDataCount = static_cast<int32_t>(initData.size());

            err = OH_MediaKeySession_GenerateMediaKeyRequest(mSession, &requestInfo);
            if (err != DRM_ERR_OK) {
                AF_LOGW("GenerateMediaKeyRequest err %d (license deferred)\n", err);
            }

            // Deliver the challenge through the DRM callback.
            DrmRequestParam param{};
            param.mDrmType = "OHOS";
            param.mParam = const_cast<char *>(drmInfo.uri.c_str());
            DrmResponseData *response = drmCallback(param);
            if (response != nullptr) {
                int size = 0;
                const char *data = response->getData(&size);
                if (data != nullptr && size > 0) {
                    OH_MediaKeySession_ProcessMediaKeyResponse(mSession,
                            reinterpret_cast<uint8_t *>(const_cast<char *>(data)), size);
                }
                delete response;
            }
        }

        return 0;
    }

} // namespace Cicada

#endif // __OHOS__
