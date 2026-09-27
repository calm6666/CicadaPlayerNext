//
// Created by SuperMan on 2020/10/13.
//

#include <utils/Android/FindClass.h>
#include <utils/Android/JniEnv.h>
#include <utils/Android/JniException.h>
#include <utils/Android/NewStringUTF.h>
#include <utils/Android/GetStringUTFChars.h>
#include <utils/Android/JniUtils.h>
#include <utils/Android/NewByteArray.h>
#include <utils/frame_work_log.h>
#include <utils/Android/NewHashMap.h>
#include "MediaCodec_Decoder.h"
#include "OutputBufferInfo.h"
#include "JEncryptionInfo.h"

using namespace Cicada;

static jclass jMediaCodecClass = nullptr;
static jmethodID jMediaCodec_init = nullptr;
static jmethodID jMediaCodec_setCodecSpecificData = nullptr;
static jmethodID jMediaCodec_setDrmInfo = nullptr;
static jmethodID jMediaCodec_setForceInsecureDecoder = nullptr;
static jmethodID jMediaCodec_setDecodeBoost = nullptr;
static jmethodID jMediaCodec_configureVideo = nullptr;
static jmethodID jMediaCodec_configureAudio = nullptr;
static jmethodID jMediaCodec_setOutputSurface = nullptr;
static jmethodID jMediaCodec_start = nullptr;
static jmethodID jMediaCodec_flush = nullptr;
static jmethodID jMediaCodec_stop = nullptr;
static jmethodID jMediaCodec_release = nullptr;
static jmethodID jMediaCodec_releaseOutputBuffer = nullptr;
static jmethodID jMediaCodec_dequeueInputBufferIndex = nullptr;
static jmethodID jMediaCodec_queueInputBuffer = nullptr;
static jmethodID jMediaCodec_queueSecureInputBuffer = nullptr;
static jmethodID jMediaCodec_dequeueOutputBufferIndex = nullptr;
static jmethodID jMediaCodec_getOutputBufferInfo = nullptr;
static jmethodID jMediaCodec_getOutBuffer = nullptr;
/* 【控制面】NDK 数据面用它取得"Java 侧选中的 codec 名"（见 MediaCodec_Decoder.h 的说明）。 */
static jmethodID jMediaCodec_selectCodecName = nullptr;

/*
 * 【设备硬解能力查询】MediaCodecUtils 的类句柄与方法 ID。
 * 与上面那批一样，在 init()（JNI_OnLoad，Java 线程）里缓存；查询发生在内核线程上，
 * 那时再 FindClass 是找不到应用侧的类的。方法不存在（旧版 AAR）时保持 nullptr。
 */
static jclass jMediaCodecUtilsClass = nullptr;
static jmethodID jMediaCodecUtils_isHardwareDecodeSupported = nullptr;

void MediaCodec_Decoder::init(JNIEnv *env) {
    if (env == nullptr) {
        return;
    }

    if (jMediaCodecClass == nullptr) {
        FindClass jClass(env, "com/cicada/player/utils/media/MediaCodecDecoder");
        jMediaCodecClass = static_cast<jclass>(env->NewGlobalRef(jClass.getClass()));
        jMediaCodec_init = env->GetMethodID(jMediaCodecClass, "<init>", "()V");
        jMediaCodec_setCodecSpecificData = env->GetMethodID(jMediaCodecClass,
                                                            "setCodecSpecificData", "(Ljava/lang/Object;)V");
        jMediaCodec_setDrmInfo = env->GetMethodID(jMediaCodecClass, "setDrmInfo",
                                                  "(Ljava/lang/String;[B)Z");
        jMediaCodec_setForceInsecureDecoder = env->GetMethodID(jMediaCodecClass, "setForceInsecureDecoder",
                                                  "(Z)V");
        jMediaCodec_setDecodeBoost = env->GetMethodID(jMediaCodecClass, "setDecodeBoost", "(Z)V");

        /*
         * 新旧 Java/native 混装（例如 APK 里是旧 AAR、.so 是新的）时，这个成员可能不存在。
         * GetMethodID 失败会挂起一个 pending 异常，不清掉会污染后面**所有** JNI 调用。
         * 所以这里主动清掉并把句柄置空 —— 追帧加速退化成空操作，其余功能一字不受影响。
         */
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            jMediaCodec_setDecodeBoost = nullptr;
        }
        jMediaCodec_configureVideo = env->GetMethodID(jMediaCodecClass, "configureVideo",
                                                      "(Ljava/lang/String;IIILandroid/view/Surface;Z)I");
        jMediaCodec_configureAudio = env->GetMethodID(jMediaCodecClass, "configureAudio",
                                                      "(Ljava/lang/String;III)I");
        jMediaCodec_setOutputSurface = env->GetMethodID(jMediaCodecClass, "setOutputSurface",
                                                         "(Ljava/lang/Object;)I");
        jMediaCodec_start = env->GetMethodID(jMediaCodecClass, "start", "()I");
        jMediaCodec_flush = env->GetMethodID(jMediaCodecClass, "flush", "()I");
        jMediaCodec_stop = env->GetMethodID(jMediaCodecClass, "stop", "()I");
        jMediaCodec_release = env->GetMethodID(jMediaCodecClass, "release", "()I");
        jMediaCodec_releaseOutputBuffer = env->GetMethodID(jMediaCodecClass, "releaseOutputBuffer",
                                                           "(IZ)I");
        jMediaCodec_dequeueInputBufferIndex = env->GetMethodID(jMediaCodecClass,
                                                               "dequeueInputBufferIndex", "(J)I");
        jMediaCodec_queueInputBuffer = env->GetMethodID(jMediaCodecClass, "queueInputBuffer",
                                                        "(I[BJZ)I");
        jMediaCodec_queueSecureInputBuffer = env->GetMethodID(jMediaCodecClass,
                                                              "queueSecureInputBuffer",
                                                              "(I[BLjava/lang/Object;JZ)I");
        jMediaCodec_dequeueOutputBufferIndex = env->GetMethodID(jMediaCodecClass,
                                                                "dequeueOutputBufferIndex", "(J)I");
        jMediaCodec_getOutputBufferInfo = env->GetMethodID(jMediaCodecClass, "getOutputBufferInfo",
                                                           "(I)Ljava/lang/Object;");
        jMediaCodec_getOutBuffer = env->GetMethodID(jMediaCodecClass, "getOutBuffer",
                                                    "(I)Ljava/lang/Object;");
        /*
         * 控制面：选 codec 名（NDK 数据面用）。失败时清掉 pending 异常，
         * 理由见本文件 setDecodeBoost 那段注释：GetMethodID 失败会挂起异常，
         * 不清掉会污染后面**所有** JNI 调用。
         */
        jMediaCodec_selectCodecName = env->GetMethodID(jMediaCodecClass, "selectCodecName",
                                                       "(ZLjava/lang/String;IIIII)Ljava/lang/String;");

        if (JniException::clearException(env)) {
            jMediaCodec_selectCodecName = nullptr;
            AF_LOGW("MediaCodecDecoder.selectCodecName not found (老版本 Java 类): "
                    "NDK 绑定将回落 Java 绑定\n");
        }
    }

    /*
     * 【设备硬解能力查询】提前把 MediaCodecUtils 缓存下来。
     *
     * 放在 init() 里的理由：init() 是 JNI_OnLoad 从 Java 线程调用的，FindClass 那时
     * 才找得到应用侧的类；真正查询硬解能力的是内核线程（mediaPlayer / ABR），那里
     * 的 FindClass 走系统类加载器，找不到 AAR 里的类。
     *
     * 找不到类或方法（极老的 AAR）就保持 nullptr：查询方按"未知 = 视为支持"处理，
     * 只是一次优化退化成"只用效率序"，不影响播放。
     */
    if (jMediaCodecUtilsClass == nullptr) {
        FindClass jUtilsClass(env, "com/cicada/player/utils/media/MediaCodecUtils");
        jclass utilsClass = jUtilsClass.getClass();

        if (utilsClass != nullptr) {
            jMediaCodecUtilsClass = static_cast<jclass>(env->NewGlobalRef(utilsClass));
            jMediaCodecUtils_isHardwareDecodeSupported =
                    env->GetStaticMethodID(jMediaCodecUtilsClass, "isHardwareDecodeSupported",
                                           "(Ljava/lang/String;)Z");

            if (env->ExceptionCheck()) {
                /* GetStaticMethodID 失败会挂起 pending 异常，不清掉会污染后面所有 JNI 调用。 */
                env->ExceptionClear();
                jMediaCodecUtils_isHardwareDecodeSupported = nullptr;
            }
        }
    }


}

void MediaCodec_Decoder::unInit(JNIEnv *env) {
    if (env == nullptr) {
        return;
    }
    if (jMediaCodecClass != nullptr) {
        env->DeleteGlobalRef(jMediaCodecClass);
        jMediaCodecClass = nullptr;
    }

    if (jMediaCodecUtilsClass != nullptr) {
        env->DeleteGlobalRef(jMediaCodecUtilsClass);
        jMediaCodecUtilsClass = nullptr;
        jMediaCodecUtils_isHardwareDecodeSupported = nullptr;
    }
}

/*
 * 【设备硬解能力查询】把 mime 交给 Java 侧只认**硬件**解码器的枚举
 * （MediaCodecUtils.isHardwareDecodeSupported）。
 *
 * Java 侧明确回答 true 才是 true；以下"查不到"的情形统统返回 true（未知 = 视为支持）：
 *   * init() 时没缓存到类或方法（旧版 AAR）；
 *   * 拿不到 JNIEnv（线程没挂上）；
 *   * Java 侧抛了异常。
 * 这样即使新 .so 配了旧 AAR，也只会退化成"只用压缩效率序"，不会把能解的编码误判成解不了。
 */
bool MediaCodec_Decoder::isHardwareDecodeSupported(const char *mime) {
    if (mime == nullptr) {
        return true;
    }

    if (jMediaCodecUtilsClass == nullptr || jMediaCodecUtils_isHardwareDecodeSupported == nullptr) {
        return true;
    }

    JniEnv jniEnv{};
    JNIEnv *env = jniEnv.getEnv();

    if (env == nullptr) {
        return true;
    }

    NewStringUTF jMime(env, mime);
    jstring mimeString = jMime.getString();

    if (mimeString == nullptr) {
        return true;
    }

    jboolean supported = env->CallStaticBooleanMethod(jMediaCodecUtilsClass,
                                                      jMediaCodecUtils_isHardwareDecodeSupported,
                                                      mimeString);

    if (JniException::clearException(env)) {
        return true;
    }

    return supported == JNI_TRUE;
}


MediaCodec_Decoder::MediaCodec_Decoder() {
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr) {
        return;
    }

    jobject pJobject = env->NewObject(jMediaCodecClass, jMediaCodec_init);
    mMediaCodec = env->NewGlobalRef(pJobject);
    env->DeleteLocalRef(pJobject);
}

MediaCodec_Decoder::~MediaCodec_Decoder() {
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr) {
        return;
    }

    if (mMediaCodec != nullptr) {
        env->DeleteGlobalRef(mMediaCodec);
    }
}

void MediaCodec_Decoder::setCodecSpecificData(const std::list<std::unique_ptr<CodecSpecificData>> &csds)
{
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr) {
        return;
    }

    if(csds.empty()){
        return;
    }

    NewHashMap csdMap(env);

    for (auto &data : csds) {
        std::string key = data->key;
        NewStringUTF keyStr(env, key.c_str());
        NewByteArray csdData(env, data->buffer, data->len);
        csdMap.put(keyStr.getString(), csdData.getArray());
    }

    env->CallVoidMethod(mMediaCodec, jMediaCodec_setCodecSpecificData, csdMap.getMap());
}

int MediaCodec_Decoder::setDrmInfo(const std::string &uuid, const void *sessionId, int size) {
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr) {
        return MC_ERROR;
    }

    NewStringUTF jKeyUrl(env, uuid.c_str());
    NewByteArray jSessionId(env, sessionId, size);
    bool ret = env->CallBooleanMethod(mMediaCodec, jMediaCodec_setDrmInfo, jKeyUrl.getString(),
                                      jSessionId.getArray());

    return ret? 0 : MC_ERROR;
}

void MediaCodec_Decoder::setForceInsecureDecoder(bool force)
{
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr) {
        return ;
    }

    env->CallVoidMethod(mMediaCodec, jMediaCodec_setForceInsecureDecoder,  (jboolean)force);

}

int MediaCodec_Decoder::setDecodeBoost(bool boost)
{
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr || mMediaCodec == nullptr || jMediaCodec_setDecodeBoost == nullptr) {
        return MC_ERROR;
    }

    env->CallVoidMethod(mMediaCodec, jMediaCodec_setDecodeBoost, (jboolean) boost);

    /*
     * Java 侧已经用 try/catch 把"个别 codec 不认这个 key"兜住了；这里再清一次
     * 异常，保证即使真的抛出来也不会污染后续 JNI 调用（返回值只用于日志）。
     */
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return MC_ERROR;
    }

    return 0;
}

int MediaCodec_Decoder::configureVideo(const std::string &mime, int width, int height, int angle,
                                       void *surface, bool usePlaceholderSurface) {
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr) {
        return MC_ERROR;
    }

    if (mMediaCodec == nullptr) {
        return MC_ERROR;
    }

    mCodecCategory = CATEGORY_VIDEO;
    NewStringUTF jMime(env, mime.c_str());
    int ret = env->CallIntMethod(mMediaCodec, jMediaCodec_configureVideo, jMime.getString(),
                                 (jint) width, (jint) height, (jint) angle, (jobject) surface,
                                 (jboolean) usePlaceholderSurface);

    return ret;
}

int MediaCodec_Decoder::configureAudio(const std::string &mime, int sampleRate, int channelCount,
                                       int isADTS) {
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr) {
        return MC_ERROR;
    }

    if (mMediaCodec == nullptr) {
        return MC_ERROR;
    }

    mCodecCategory = CATEGORY_AUDIO;
    NewStringUTF jMime(env, mime.c_str());
    int ret = env->CallIntMethod(mMediaCodec, jMediaCodec_configureAudio, jMime.getString(),
                                 (jint) sampleRate, (jint) channelCount, (jint) isADTS);

    return ret;
}

std::string MediaCodec_Decoder::selectCodecName(bool isVideo, const std::string &mime, int width,
                                                int height, int sampleRate, int channelCount,
                                                int isADTS) {
    std::string result;
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();

    if (env == nullptr || mMediaCodec == nullptr || jMediaCodec_selectCodecName == nullptr) {
        /*
         * 拿不到就返回空串：调用方（NDK 绑定）会把整个实例回落给 Java 绑定，
         * 功能不缺失，只是一次调用没走成 —— 与"能力查询未知即视为支持"的既有口径一致。
         */
        return result;
    }

    NewStringUTF jMime(env, mime.c_str());
    jstring name = (jstring) env->CallObjectMethod(mMediaCodec, jMediaCodec_selectCodecName,
                                                   (jboolean) isVideo, jMime.getString(),
                                                   (jint) width, (jint) height, (jint) sampleRate,
                                                   (jint) channelCount, (jint) isADTS);

    if (JniException::clearException(env)) {
        AF_LOGE("selectCodecName exception\n");
        return result;
    }

    if (name != nullptr) {
        const char *chars = env->GetStringUTFChars(name, nullptr);

        if (chars != nullptr) {
            result.assign(chars);
            env->ReleaseStringUTFChars(name, chars);
        }

        env->DeleteLocalRef(name);
    }

    return result;
}

int MediaCodec_Decoder::start() {
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr) {
        return MC_ERROR;
    }

    if (mMediaCodec == nullptr) {
        return MC_ERROR;
    }

    int ret = env->CallIntMethod(mMediaCodec, jMediaCodec_start);

    return ret;
}

int MediaCodec_Decoder::flush() {
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr) {
        return MC_ERROR;
    }

    if (mMediaCodec == nullptr) {
        return MC_ERROR;
    }

    int ret = env->CallIntMethod(mMediaCodec, jMediaCodec_flush);

    return ret;
}

int MediaCodec_Decoder::stop() {
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr) {
        return MC_ERROR;
    }

    if (mMediaCodec == nullptr) {
        return MC_ERROR;
    }

    int ret = env->CallIntMethod(mMediaCodec, jMediaCodec_stop);

    return ret;
}

int MediaCodec_Decoder::release() {
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr) {
        return MC_ERROR;
    }

    if (mMediaCodec == nullptr) {
        return MC_ERROR;
    }

    int ret = env->CallIntMethod(mMediaCodec, jMediaCodec_release);

    return ret;
}

int MediaCodec_Decoder::dequeueInputBufferIndex(int64_t timeoutUs) {
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr) {
        return MC_ERROR;
    }

    if (mMediaCodec == nullptr) {
        return MC_ERROR;
    }

    int ret = env->CallIntMethod(mMediaCodec, jMediaCodec_dequeueInputBufferIndex,
                                 (jlong) timeoutUs);
    return ret;
}

int MediaCodec_Decoder::dequeueOutputBufferIndex(int64_t timeoutUs) {
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr) {
        return MC_ERROR;
    }

    if (mMediaCodec == nullptr) {
        return MC_ERROR;
    }

    int ret = env->CallIntMethod(mMediaCodec, jMediaCodec_dequeueOutputBufferIndex,
                                 (jlong) timeoutUs);

    return ret;
}

int MediaCodec_Decoder::queueInputBuffer(int index, void *buffer, size_t size, int64_t pts,
                                         bool isConfig) {
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr) {
        return MC_ERROR;
    }

    if (mMediaCodec == nullptr) {
        return MC_ERROR;
    }

    jbyteArray jBuffer = nullptr;
    if (buffer != nullptr) {
        jBuffer = env->NewByteArray(size);
        env->SetByteArrayRegion(jBuffer, 0, size, (jbyte *) (buffer));
    }


    int ret = env->CallIntMethod(mMediaCodec, jMediaCodec_queueInputBuffer, (jint) index, jBuffer,
                                 (jlong) pts, (jboolean) isConfig);
    if (jBuffer != nullptr) {
        env->DeleteLocalRef(jBuffer);
    }
    return ret;
}

int MediaCodec_Decoder::queueSecureInputBuffer(int index, void *buffer, size_t size,
                                               IAFPacket::EncryptionInfo *pEncryptionInfo,
                                               int64_t pts, bool isConfig) {
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr) {
        return MC_ERROR;
    }

    if (mMediaCodec == nullptr) {
        return MC_ERROR;
    }

    jbyteArray jBuffer = nullptr;
    if (buffer != nullptr) {
        jBuffer = env->NewByteArray(size);
        env->SetByteArrayRegion(jBuffer, 0, size, (jbyte *) (buffer));
    }

    jobject encryptionInfo = nullptr;
    if (pEncryptionInfo != nullptr) {
        encryptionInfo = JEncryptionInfo::convert(env, pEncryptionInfo);
    }
    int ret = env->CallIntMethod(mMediaCodec, jMediaCodec_queueSecureInputBuffer, (jint) index,
                                 jBuffer,
                                 encryptionInfo, (jlong) pts, (jboolean) isConfig);
    if (jBuffer != nullptr) {
        env->DeleteLocalRef(jBuffer);
    }

    if(encryptionInfo != nullptr){
        env->DeleteLocalRef(encryptionInfo);
    }

    return ret;
}

int MediaCodec_Decoder::getOutput(int index, mc_out *out, bool readBuffer) {
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr) {
        return MC_ERROR;
    }

    if (mMediaCodec == nullptr) {
        return MC_ERROR;
    }

    OutputBufferInfo outputBufferInfo{};

    jobject outputInfo = env->CallObjectMethod(mMediaCodec, jMediaCodec_getOutputBufferInfo,
                                               (jint) index);
    if (outputInfo != nullptr) {
        OutputBufferInfo::convert(env, &outputBufferInfo, outputInfo);
        env->DeleteLocalRef(outputInfo);
    }

    if (index >= 0) {
        out->type = outputBufferInfo.type;
        out->b_eos = outputBufferInfo.eos;
        out->buf.index = outputBufferInfo.index;
        out->buf.pts = outputBufferInfo.pts;

        if (readBuffer) {
            jobject bufferInfo = env->CallObjectMethod(mMediaCodec, jMediaCodec_getOutBuffer,
                                                       (jint) index);
            if (bufferInfo != nullptr) {
                auto *ptr = (uint8_t *) env->GetDirectBufferAddress(bufferInfo);
                int offset = outputBufferInfo.bufferOffset;
                out->buf.p_ptr = ptr + offset;
                out->buf.size = outputBufferInfo.bufferSize;

                env->DeleteLocalRef(bufferInfo);
            }
        } else {
            out->buf.p_ptr = nullptr;
            out->buf.size = 0;
        }

    } else if (index == MC_INFO_OUTPUT_FORMAT_CHANGED) {
        out->type = outputBufferInfo.type;
        out->b_eos = outputBufferInfo.eos;

        if (mCodecCategory == CATEGORY_VIDEO) {
            out->conf.video.width = outputBufferInfo.videoWidth;
            out->conf.video.height = outputBufferInfo.videoHeight;
            out->conf.video.stride = outputBufferInfo.videoStride;
            out->conf.video.slice_height = outputBufferInfo.videoSliceHeight;
            out->conf.video.pixel_format = outputBufferInfo.videoPixelFormat;
            out->conf.video.crop_left = outputBufferInfo.videoCropLeft;
            out->conf.video.crop_top = outputBufferInfo.videoCropTop;
            out->conf.video.crop_right = outputBufferInfo.videoCropRight;
            out->conf.video.crop_bottom = outputBufferInfo.videoCropBottom;
        } else {
            out->conf.audio.channel_count = outputBufferInfo.audioChannelCount;
            out->conf.audio.channel_mask = outputBufferInfo.audioChannelMask;
            out->conf.audio.sample_rate = outputBufferInfo.audioSampleRate;
            out->conf.audio.format = outputBufferInfo.audioFormat;
        }
    }


    return 0;
}

int MediaCodec_Decoder::releaseOutputBuffer(int index, bool render) {
    JniEnv jniEnv{};

    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr) {
        return MC_ERROR;
    }

    if (mMediaCodec == nullptr) {
        return MC_ERROR;
    }

    int ret = env->CallIntMethod(mMediaCodec, jMediaCodec_releaseOutputBuffer, (jint) index,
                                 (jboolean) render);
    return ret;
}

int MediaCodec_Decoder::setOutputSurface(void *surface) {
    if (mMediaCodec == nullptr) {
        return -1;
    }

    JniEnv jniEnv{};
    JNIEnv *env = jniEnv.getEnv();
    if (env == nullptr) {
        return -1;
    }

    // surface 允许为 null：Java 侧对齐 ExoPlayer 2.9.6 换成内部 DummySurface，
    // codec 保持运行；回前台再切回真实 surface
    return env->CallIntMethod(mMediaCodec, jMediaCodec_setOutputSurface, (jobject) surface);
}



