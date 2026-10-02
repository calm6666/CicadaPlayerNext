//
// OhosDrmHandler.cpp
//
// See OhosDrmHandler.h for the design.
//
// NOTE: every DRM Kit call below was checked against the API 22 SDK headers.
// The previous revision of this file was written against an API that does not
// exist in the NDK (OH_MediaKeySystem_Create with one argument,
// OH_MediaKeySession_Create, OH_DRM_ErrCode, OH_MediaKeyRequestInfo.mediaKeyType,
// ...), so it had never compiled.
//

#define LOG_TAG "OhosDrmHandler"

#ifdef __OHOS__

#include "OhosDrmHandler.h"
#include "../DrmSchemes.h"
#include <utils/frame_work_log.h>

#include <multimedia/drm_framework/native_mediakeysystem.h>
#include <multimedia/drm_framework/native_mediakeysession.h>
#include <multimedia/drm_framework/native_drm_common.h>
#include <multimedia/drm_framework/native_drm_err.h>

#include <cstring>

namespace Cicada {

    /*
     * 这四个常量原来是**裸 UUID**（不带 urn:uuid: 前缀），而清单/内核对 keyFormat 的
     * 口径是**规范形式 `urn:uuid:<uuid>`**（mediaCodecDecoder / SuperMediaPlayer /
     * DashStream 都按带前缀的字符串比）。于是 `is_supported()` 永远为假 ——
     * OhosDrmHandler 整套 DRM Kit 的代码在 OHOS 上一次都没被触发过（真 bug）。
     *
     * 现在统一用规范形式，并且经 DrmSchemes::canonical 归一化后再比，
     * 大小写/花括号/裸 UUID/别名（com.widevine.alpha 等）都能认出来。
     */
    static const char *kWidevineUuid = "urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed";
    static const char *kPlayReadyUuid = "urn:uuid:9a04f079-9840-4286-ab92-e65be0885f95";
    static const char *kFairPlayUuid = "urn:uuid:94ce86fb-07ff-4f43-adb8-93d2fa968ca2";
    static const char *kClearKeyUuid = "urn:uuid:e2719d58-a985-b3c9-781a-b030af78d30e";

    /** Buffer for the offline key id returned by ProcessMediaKeyResponse. */
    static const int32_t kOfflineKeyIdCapacity = 512;

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
        // 归一化后再比：清单里的写法可能是大写 UUID / 裸 UUID / EME 别名。
        const std::string scheme = DrmSchemes::canonical(drmInfo.format);

        return scheme == kWidevineUuid
               || scheme == kPlayReadyUuid
               || scheme == kFairPlayUuid
               || scheme == kClearKeyUuid;
    }

    int OhosDrmHandler::open()
    {
        const std::string &uuid = drmInfo.format;

        // OH_MediaKeySystem_Create(const char *name, MediaKeySystem **system)
        // native_mediakeysystem.h:153
        Drm_ErrCode err = OH_MediaKeySystem_Create(uuid.c_str(), &mSystem);
        if (err != DRM_ERR_OK || mSystem == nullptr) {
            AF_LOGE("OH_MediaKeySystem_Create failed %d for %s (plugin not present?)\n",
                    static_cast<int>(err), uuid.c_str());
            mError = true;
            return -1;
        }

        // OH_MediaKeySystem_CreateMediaKeySession(MediaKeySystem *,
        //     DRM_ContentProtectionLevel *, MediaKeySession **)
        // native_mediakeysystem.h:272
        // SW_CRYPTO is the level ClearKey and Widevine L3 use; L1 content would
        // need CONTENT_PROTECTION_LEVEL_HW_CRYPTO plus a secure surface.
        DRM_ContentProtectionLevel level = CONTENT_PROTECTION_LEVEL_SW_CRYPTO;
        err = OH_MediaKeySystem_CreateMediaKeySession(mSystem, &level, &mSession);
        if (err != DRM_ERR_OK || mSession == nullptr) {
            AF_LOGE("OH_MediaKeySystem_CreateMediaKeySession failed %d\n", static_cast<int>(err));
            mSession = nullptr;
            mError = true;
            return -1;
        }

        if (drmCallback == nullptr) {
            // No license callback configured: the session is created but no key
            // request is issued (the caller may drive it later).
            return 0;
        }

        // OH_MediaKeySession_GenerateMediaKeyRequest(MediaKeySession *,
        //     DRM_MediaKeyRequestInfo *, DRM_MediaKeyRequest *)
        // native_mediakeysession.h:145
        DRM_MediaKeyRequestInfo requestInfo{};
        requestInfo.type = MEDIA_KEY_TYPE_ONLINE;
        // mimeType is a fixed char[MAX_MIMETYPE_LEN] buffer, not a pointer.
        strncpy(requestInfo.mimeType, "video/mp4", sizeof(requestInfo.mimeType) - 1);
        // initData (PSSH) is a fixed uint8_t[MAX_INIT_DATA_LEN] buffer. The
        // bring-up path leaves it empty and relies on the sample key ids; a
        // base64-decoded drmInfo.pssh should be memcpy'd here once available.
        requestInfo.initDataLen = 0;

        DRM_MediaKeyRequest request{};
        err = OH_MediaKeySession_GenerateMediaKeyRequest(mSession, &requestInfo, &request);
        if (err != DRM_ERR_OK) {
            AF_LOGW("GenerateMediaKeyRequest err %d (license deferred)\n", static_cast<int>(err));
        }

        // Deliver the challenge through the app-side DRM callback. The key
        // request blob and the licence server URL come from the DRM Kit and are
        // exposed on DrmRequestParam; an app callback POSTs the blob to the
        // server and returns the licence response.
        DrmRequestParam param{};
        param.mDrmType = "OHOS";
        param.mParam = const_cast<char *>(drmInfo.uri.c_str());
        if (request.dataLen > 0) {
            param.mKeyRequest.assign(reinterpret_cast<const char *>(request.data),
                                     static_cast<size_t>(request.dataLen));
        }
        param.mLicenseUrl = request.defaultUrl;
        AF_LOGI("DRM challenge: %d bytes, licence url %s\n", request.dataLen, request.defaultUrl);
        DrmResponseData *response = drmCallback(param);
        if (response != nullptr) {
            int size = 0;
            const char *data = response->getData(&size);
            if (data != nullptr && size > 0) {
                // OH_MediaKeySession_ProcessMediaKeyResponse(MediaKeySession *,
                //     uint8_t *response, int32_t responseLen,
                //     uint8_t *offlineMediaKeyId, int32_t *offlineMediaKeyIdLen)
                // native_mediakeysession.h:163
                uint8_t offlineKeyId[kOfflineKeyIdCapacity] = {0};
                int32_t offlineKeyIdLen = kOfflineKeyIdCapacity;
                err = OH_MediaKeySession_ProcessMediaKeyResponse(mSession,
                        reinterpret_cast<uint8_t *>(const_cast<char *>(data)), size,
                        offlineKeyId, &offlineKeyIdLen);
                if (err != DRM_ERR_OK) {
                    AF_LOGE("ProcessMediaKeyResponse failed %d\n", static_cast<int>(err));
                    mError = true;
                }
            }
            delete response;
        }

        return 0;
    }

} // namespace Cicada

#endif // __OHOS__
