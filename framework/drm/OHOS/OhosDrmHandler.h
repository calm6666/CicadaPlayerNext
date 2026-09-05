//
// OhosDrmHandler.h
//
// DRM support for HarmonyOS / OpenHarmony via the DRM Kit native C API
// (libnative_drm.so):
//
//   OH_MediaKeySystem  - a DRM plugin instance (selected by system UUID)
//   OH_MediaKeySession - per-stream key/license state
//
// License flow: create system + session -> OH_MediaKeySession_GenerateMediaKeyRequest
// (challenge) -> the app-side license callback (setDrmCallback) delivers the
// license response -> OH_MediaKeySession_ProcessMediaKeyResponse installs it.
// The session is then attached to the OH_AVCodec decoder with
// OH_AVCodec_SetMediakeySessionConfig() so the codec service decrypts CENC
// samples in-pipeline.
//
// Supported systems (plugin availability is device/vendor-dependent):
//   - "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed" Widevine (Huawei-proprietary, L3 on
//     commercial HarmonyOS NEXT devices)
//   - "e2719d58-a985-b3c9-781a-b030af78d30e" ClearKey (open OpenHarmony plugin)
//   - "94ce86fb-07ff-4f43-adb8-93d2fa968ca2" FairPlay (vendor-dependent)
//   - "9a04f079-9840-4286-ab92-e65be0885f95" PlayReady (vendor-dependent)
//

#ifndef FRAMEWORK_DRM_OHOS_OHOSDRMHANDLER_H
#define FRAMEWORK_DRM_OHOS_OHOSDRMHANDLER_H

#ifdef __OHOS__

#include <drm/DrmHandler.h>
#include <drm/DrmHandlerPrototype.h>

typedef struct OH_MediaKeySystemNative OH_MediaKeySystem;
typedef struct OH_MediaKeySessionNative OH_MediaKeySession;

namespace Cicada {

    class OhosDrmHandler : public DrmHandler, private DrmHandlerPrototype {
    public:
        explicit OhosDrmHandler(const DrmInfo &drmInfo);

        ~OhosDrmHandler() override;

        /**
         * Create the media key system + session and kick off the license
         * request. Returns 0 on success (the session may still be pending
         * license installation).
         */
        int open();

        /** Attach this session to an OH_AVCodec decoder (before Configure). */
        OH_MediaKeySession *getMediaKeySession()
        {
            return mSession;
        }

        bool isErrorState() override
        {
            return mError;
        }

        /** Register this handler with the DRM prototype registry. */
        static void registerPrototype();

    private:
        explicit OhosDrmHandler(int dummy);

        DrmHandler *clone(const DrmInfo &drmInfo) override
        {
            return new OhosDrmHandler(drmInfo);
        }

        bool is_supported(const DrmInfo &drmInfo) override;

        static OhosDrmHandler se;

    private:
        OH_MediaKeySystem *mSystem{nullptr};
        OH_MediaKeySession *mSession{nullptr};
        bool mError{false};
    };
} // namespace Cicada

#endif // __OHOS__
#endif // FRAMEWORK_DRM_OHOS_OHOSDRMHANDLER_H
