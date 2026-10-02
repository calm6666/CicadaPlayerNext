//
// Created by SuperMan on 11/27/20.
//

#define  LOG_TAG "WideVineDrmHandler"

#include "WideVineDrmHandler.h"
#include "DrmSchemes.h"
#include <utils/Android/GetStringUTFChars.h>
#include <utils/Android/JniUtils.h>
#include <utils/frame_work_log.h>
#include <utils/Android/FindClass.h>
#include <utils/Android/JniEnv.h>
#include <utils/Android/NewStringUTF.h>
#include <utils/Android/NewByteArray.h>
#include <utils/CicadaUtils.h>
#include <cassert>
#include <utils/CicadaJSON.h>

extern "C" {
#include <utils/errors/framework_error.h>
#include <libavutil/intreadwrite.h>
}

static jclass jMediaDrmSessionClass = nullptr;
static jmethodID jMediaDrmSession_init = nullptr;
static jmethodID jMediaDrmSession_requireSession = nullptr;
static jmethodID jMediaDrmSession_releaseSession = nullptr;
static jmethodID jMediaDrmSession_isForceInsecureDecoder = nullptr;

/*
 * Android MediaDrm 能建会话的三家 DRM 系统（W3C / DASH-IF 登记的 urn:uuid: 形式）。
 *
 * 【为什么只有三家】Android 的 MediaDrm 只提供这三种 scheme 的插件选择：Widevine、
 * PlayReady（设备厂商可选）、ClearKey。清单里其余那二十几个 DRM 系统（Marlin / Nagra /
 * Irdeto / …）在 Android 上**没有**对应的 MediaDrm scheme，所以这里**不能**写成
 * "是个 urn:uuid: 就认"：认了会在 DrmSessionManager.prepare() 里失败，而这个
 * "认不认"还要用来决定"要不要把软解让给平台"（认了就不登记软解密钥）。
 *
 * ★这三个常量必须与 `platform/Android/**/DrmSessionManager.java` 里的
 * WIDEVINE_FORMAT / PLAYREADY_FORMAT / CLEARKEY_FORMAT 逐字一致 ——
 * 那边负责把它们映射成 MediaDrm 的 UUID 并真正建会话。
 */
static const char *const kAndroidMediaDrmWidevine =
    "urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed";
static const char *const kAndroidMediaDrmPlayReady =
    "urn:uuid:9a04f079-9840-4286-ab92-e65be0885f95";
static const char *const kAndroidMediaDrmClearKey =
    "urn:uuid:e2719d58-a985-b3c9-781a-b030af78d30e";

using namespace Cicada;

WideVineDrmHandler WideVineDrmHandler::dummyWideVineHandler(0);

WideVineDrmHandler::WideVineDrmHandler(const DrmInfo &drmInfo)
        : DrmHandler(drmInfo) {
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr) {
        return;
    }

    jobject pJobject = env->NewObject(jMediaDrmSessionClass, jMediaDrmSession_init, (jlong) this);
    mJDrmSessionManger = env->NewGlobalRef(pJobject);
    env->DeleteLocalRef(pJobject);
}

WideVineDrmHandler::~WideVineDrmHandler() {
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr) {
        return;
    }

    if (mJDrmSessionManger != nullptr) {
        env->CallVoidMethod(mJDrmSessionManger, jMediaDrmSession_releaseSession);
        env->DeleteGlobalRef(mJDrmSessionManger);
    }

    if (mSessionId != nullptr) {
        free(mSessionId);
        mSessionId = nullptr;
    }

}

void WideVineDrmHandler::open() {
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mDrmMutex);
        if (bSessionRequested) {
            return;
        } else {
            bSessionRequested = true;
        }
    }

    NewStringUTF jUrl(env, drmInfo.uri.c_str());
    NewStringUTF jFormat(env, drmInfo.format.c_str());
    NewStringUTF jPssh(env, drmInfo.pssh.c_str());
    NewStringUTF jKeyId(env, drmInfo.keyId.c_str());
    jstring pJurl = jUrl.getString();
    jstring pJformat = jFormat.getString();
    jstring pJpssh = jPssh.getString();
    jstring pJkeyId = jKeyId.getString();

    /*
     * ★参数顺序与 DrmSessionManager.java 的 requireSession 逐字对应：
     *   (String keyFormat, String licenseUrl, String pssh, String keyId, String mime)
     * 第 5 个 mime 传 nullptr：MediaDrm 的 getKeyRequest 对三家 scheme 都不需要它
     * （清单一路上也没有"流 mime"这个字符串可传，凭空造一个属于发明）。
     *
     * 为什么必须把 pssh / keyId 传过去：ClearKey 的 initData 就是 pssh（或者由 KID
     * 按 W3C Common PSSH box 构造），只传 url 的话 Java 侧根本拿不到 initData。
     */
    env->CallVoidMethod(mJDrmSessionManger,
                        jMediaDrmSession_requireSession,
                        pJformat, pJurl,
                        pJpssh, pJkeyId,
                        nullptr);
}


bool WideVineDrmHandler::isErrorState() {
    std::lock_guard<std::mutex> lock(mDrmMutex);
    return mState == SESSION_STATE_ERROR;
}

DrmHandler *
WideVineDrmHandler::clone(const DrmInfo &drmInfo) {
    return new WideVineDrmHandler(drmInfo);
}

bool WideVineDrmHandler::is_supported(const DrmInfo &drmInfo) {
    /*
     * 清单里的 DRM 系统标识写法五花八门（大写 UUID、裸 UUID、com.widevine.alpha …），
     * 先归一化成 urn:uuid:<小写>，再与 MediaDrm 真正支持的三种 scheme 比。
     *
     * 原来这里只比一个 Widevine 字面量：Android 上声明 PlayReady 或 ClearKey 的
     * DASH 清单会被判成"没有平台 DRM"，于是内核会去走软解 —— 而这两家在 Android 上
     * 本来是可以硬解的（Java 的 DrmSessionManager 已经有它们的 MediaDrm UUID）。
     */
    const std::string scheme = DrmSchemes::canonical(drmInfo.format);

    return scheme == kAndroidMediaDrmWidevine
           || scheme == kAndroidMediaDrmPlayReady
           || scheme == kAndroidMediaDrmClearKey;
}

static JNINativeMethod mediaCodec_method_table[] = {
        {"native_requestProvision", "(JLjava/lang/String;[B)[B", (void *) WideVineDrmHandler::requestProvision},
        {"native_requestKey",       "(JLjava/lang/String;[B)[B", (void *) WideVineDrmHandler::requestKey},
        {"native_changeState",      "(JII)V",                    (void *) WideVineDrmHandler::changeState},
        {"native_updateSessionId",  "(J[B)V",                    (void *) WideVineDrmHandler::updateSessionId},
};

int WideVineDrmHandler::registerMethod(JNIEnv *pEnv) {
    if (jMediaDrmSessionClass == nullptr) {
        return JNI_FALSE;
    }

    if (pEnv->RegisterNatives(jMediaDrmSessionClass, mediaCodec_method_table,
                              sizeof(mediaCodec_method_table) / sizeof(JNINativeMethod)) < 0) {
        return JNI_FALSE;
    }

    return JNI_TRUE;
}

void WideVineDrmHandler::init(JNIEnv *env) {
    if (env == nullptr) {
        return;
    }

    if (jMediaDrmSessionClass == nullptr) {
        FindClass jClass(env, "com/cicada/player/utils/media/DrmSessionManager");
        jMediaDrmSessionClass = static_cast<jclass>(env->NewGlobalRef(jClass.getClass()));
        jMediaDrmSession_init = env->GetMethodID(jMediaDrmSessionClass, "<init>", "(J)V");
        jMediaDrmSession_requireSession = env->GetMethodID(jMediaDrmSessionClass,
                                                           "requireSession",
                                                           "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
        jMediaDrmSession_releaseSession = env->GetMethodID(jMediaDrmSessionClass, "releaseSession",
                                                           "()V");
        jMediaDrmSession_isForceInsecureDecoder = env->GetMethodID(jMediaDrmSessionClass,
                                                                   "isForceInsecureDecoder",
                                                                   "()Z");
    }
}

void WideVineDrmHandler::unInit(JNIEnv *env) {
    if (env == nullptr) {
        return;
    }

    if (jMediaDrmSessionClass != nullptr) {
        env->DeleteGlobalRef(jMediaDrmSessionClass);
        jMediaDrmSessionClass = nullptr;
    }
}

bool WideVineDrmHandler::isForceInsecureDecoder() {
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr) {
        return false;
    }

    jboolean ret = env->CallBooleanMethod(mJDrmSessionManger,
                                          jMediaDrmSession_isForceInsecureDecoder);
    return (bool) ret;
}

void WideVineDrmHandler::updateSessionId(JNIEnv *env, jobject instance, jlong nativeInstance,
                                         jbyteArray jSessionId) {

    if (jSessionId == nullptr) {
        return;
    }

    auto *drmSessionManager = (WideVineDrmHandler *) (int64_t) nativeInstance;
    if (drmSessionManager == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(drmSessionManager->mDrmMutex);
        drmSessionManager->mSessionSize = env->GetArrayLength(jSessionId);;
        drmSessionManager->mSessionId = JniUtils::jByteArrayToChars(env, jSessionId);
    }
}

void WideVineDrmHandler::changeState(JNIEnv *env, jobject instance, jlong nativeIntance,
                                     jint state, jint errorCode) {

    auto *drmSessionManager = (WideVineDrmHandler *) (int64_t) nativeIntance;
    if (drmSessionManager == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(drmSessionManager->mDrmMutex);

        if (state == 0) {
            drmSessionManager->mState = SESSION_STATE_OPENED;
            AF_LOGI("drm prepared OK");
        } else if (state == -1) {
            drmSessionManager->mState = SESSION_STATE_ERROR;
        } else if (state == -2) {
            drmSessionManager->mState = SESSION_STATE_IDLE;
        }

        drmSessionManager->mErrorCode = gen_framework_errno(error_class_drm, errorCode);
    }
}


jbyteArray
WideVineDrmHandler::requestProvision(JNIEnv *env, jobject instance, jlong nativeIntance,
                                     jstring url, jbyteArray data) {

    AF_LOGI("drm requestProvision.,");
    if (nativeIntance == 0) {
        return nullptr;
    }

    auto *drmSessionManager = (WideVineDrmHandler *) (int64_t) nativeIntance;
    if (drmSessionManager->drmCallback == nullptr) {
        return nullptr;
    }

    GetStringUTFChars cUrl(env, url);
    char *cData = JniUtils::jByteArrayToChars(env, data);
    int dataLen = env->GetArrayLength(data);

    DrmRequestParam drmRequestParam{};
    drmRequestParam.mDrmType = "WideVine";

    CicadaJSONItem param{};
    param.addValue("requestType", "provision");
    param.addValue("url", std::string(cUrl.getChars()));
    param.addValue("data", CicadaUtils::base64enc(cData, dataLen));
    drmRequestParam.mParam = &param;

    free(cData);

    DrmResponseData *drmResponseData = drmSessionManager->drmCallback(drmRequestParam);

    if (drmResponseData == nullptr) {
        return nullptr;
    }

    int responseDataSize = 0;
    const char *responseData = drmResponseData->getData(&responseDataSize);

    jbyteArray mResult = nullptr;
    if (responseData != nullptr && responseDataSize > 0) {
        mResult = env->NewByteArray(responseDataSize);
        env->SetByteArrayRegion(mResult, 0, responseDataSize, (jbyte *) (responseData));
    }

    delete drmResponseData;

    return mResult;
}

jbyteArray
WideVineDrmHandler::requestKey(JNIEnv *env, jobject instance, jlong nativeIntance,
                               jstring url,
                               jbyteArray data) {
    AF_LOGI("drm requestKey.,");
    if (nativeIntance == 0) {
        return nullptr;
    }

    auto *drmSessionManager = (WideVineDrmHandler *) (int64_t) nativeIntance;
    if (drmSessionManager->drmCallback == nullptr) {
        return nullptr;
    }

    GetStringUTFChars cUrl(env, url);
    char *cData = JniUtils::jByteArrayToChars(env, data);
    int dataLen = env->GetArrayLength(data);

    DrmRequestParam drmRequestParam{};
    drmRequestParam.mDrmType = "WideVine";

    CicadaJSONItem param{};
    param.addValue("requestType", "key");
    param.addValue("url", std::string(cUrl.getChars()));
    param.addValue("data", CicadaUtils::base64enc(cData, dataLen));
    drmRequestParam.mParam = &param;

    free(cData);

    DrmResponseData *drmResponseData = drmSessionManager->drmCallback(drmRequestParam);

    if (drmResponseData == nullptr) {
        return nullptr;
    }

    int responseDataSize = 0;
    const char *responseData = drmResponseData->getData(&responseDataSize);

    jbyteArray mResult = nullptr;
    if (responseData != nullptr && responseDataSize > 0) {
        mResult = env->NewByteArray(responseDataSize);
        env->SetByteArrayRegion(mResult, 0, responseDataSize, (jbyte *) (responseData));
    }

    delete drmResponseData;

    return mResult;
}

int WideVineDrmHandler::getState() {
    std::lock_guard<std::mutex> lock(mDrmMutex);
    return mState;
}

int WideVineDrmHandler::getSessionId(char **session) {
    std::lock_guard<std::mutex> lock(mDrmMutex);
    *session = mSessionId;
    return mSessionSize;
}

int WideVineDrmHandler::getErrorCode() {
    std::lock_guard<std::mutex> lock(mDrmMutex);
    return mErrorCode;
}

void WideVineDrmHandler::convertData(int naluLengthSize, uint8_t **new_data, int *new_size,
                                     const uint8_t *data,
                                     int size) {
    if (data == nullptr || size == 0) {
        return;
    }

    if (naluLengthSize == 0) {
        return;
    }

    const int nalPrefixLen = 5;
    char nalPrefixData[nalPrefixLen];
    nalPrefixData[0] = 0;
    nalPrefixData[1] = 0;
    nalPrefixData[2] = 0;
    nalPrefixData[3] = 0;
    nalPrefixData[4] = 0;

    int sampleBytesWritten = 0;
    int sampleSize = size;

    int nalUnitPrefixLength = naluLengthSize + 1;
    int nalUnitLengthFieldLengthDiff = 4 - naluLengthSize;
    int sampleCurrentNalBytesRemaining = 0;

    *new_size = size;
    *new_data = static_cast<uint8_t *>(malloc(*new_size));
    uint8_t *new_data_ptr = *new_data;
    uint8_t *ori_data_ptr = const_cast<uint8_t *>(data);

    while (sampleBytesWritten < sampleSize) {

        if (sampleCurrentNalBytesRemaining == 0) {
//new nal
            for (int i = 0; i < nalUnitPrefixLength; i++) {
                nalPrefixData[nalUnitLengthFieldLengthDiff + i] = *ori_data_ptr;
                ori_data_ptr++;
            }

            int nalLengthInt = AV_RB32(nalPrefixData);
            if (nalLengthInt < 1) {
                AF_LOGE("Invalid NAL length");
                return;
            }

            sampleCurrentNalBytesRemaining = nalLengthInt - 1;

            if (sampleBytesWritten + 5 > *new_size) {

                *new_size = sampleBytesWritten + 5;

                *new_data = static_cast<uint8_t *>(realloc(*new_data, *new_size));
                new_data_ptr = *new_data + sampleBytesWritten;
            }

            //put start code
            static uint8_t START_CODE[4] = {0, 0, 0, 1};
            memcpy(new_data_ptr, START_CODE, 4);
            new_data_ptr += 4;
            //nal type
            *new_data_ptr = nalPrefixData[4];
            new_data_ptr++;

            sampleBytesWritten += 5;
            sampleSize += nalUnitLengthFieldLengthDiff;

        } else {
            if (sampleBytesWritten + sampleCurrentNalBytesRemaining > *new_size) {
                *new_size = sampleBytesWritten + sampleCurrentNalBytesRemaining;
                *new_data = static_cast<uint8_t *>(realloc(*new_data, *new_size));

                new_data_ptr = *new_data + sampleBytesWritten;
            }
            memcpy(new_data_ptr, ori_data_ptr, sampleCurrentNalBytesRemaining);
            ori_data_ptr = ori_data_ptr + sampleCurrentNalBytesRemaining;
            new_data_ptr = new_data_ptr + sampleCurrentNalBytesRemaining;

            sampleBytesWritten += sampleCurrentNalBytesRemaining;
            sampleCurrentNalBytesRemaining -= sampleCurrentNalBytesRemaining;
        }
    }

    assert(*new_size == sampleBytesWritten);

    *new_size = sampleBytesWritten;
}
