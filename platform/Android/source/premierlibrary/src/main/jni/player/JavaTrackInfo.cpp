//
// Created by lifujun on 2018/12/7.
//

#include "JavaTrackInfo.h"
#include <utils/Android/NewStringUTF.h>
#include <utils/Android/FindClass.h>
#include <utils/Android/GetStringUTFChars.h>

static const char *trackInfoPath = "com/cicada/player/nativeclass/TrackInfo";

//STREAM_INFO
jclass gj_TrackInfoClass = nullptr;
jmethodID gj_TrackInfo_init = nullptr;
jmethodID gj_TrackInfo_setType = nullptr;
jmethodID gj_TrackInfo_setVideoHDRType = nullptr;
jmethodID gj_TrackInfo_nGetType = nullptr;

jfieldID gj_TrackInfo_Index = nullptr;
jfieldID gj_TrackInfo_Description = nullptr;

jfieldID gj_TrackInfo_VideoBitrate = nullptr;
jfieldID gj_TrackInfo_VideoWidth = nullptr;
jfieldID gj_TrackInfo_VideoHeight = nullptr;
/* 视频编码短名（Java String，本轮新增；只给清晰度菜单的编码徽标与"同分辨率分列"用） */
jfieldID gj_TrackInfo_VideoCodec = nullptr;

jfieldID gj_TrackInfo_AudioLang = nullptr;
jfieldID gj_TrackInfo_AudioChannels = nullptr;
jfieldID gj_TrackInfo_AudioSampleRate = nullptr;
jfieldID gj_TrackInfo_AudioSampleFormat = nullptr;

jfieldID gj_TrackInfo_SubtitleLang = nullptr;


void JavaTrackInfo::init(JNIEnv *env) {
    if (gj_TrackInfoClass == nullptr) {
        FindClass cls(env, trackInfoPath);
        gj_TrackInfoClass = (jclass) env->NewGlobalRef(cls.getClass());
        gj_TrackInfo_init = env->GetMethodID(gj_TrackInfoClass,
                                             "<init>",
                                             "()V");
        gj_TrackInfo_setType = env->GetMethodID(gj_TrackInfoClass,
                                                "setType",
                                                "(I)V");

        gj_TrackInfo_setVideoHDRType = env->GetMethodID(gj_TrackInfoClass, "setVideoHDRType", "(I)V");
        gj_TrackInfo_nGetType = env->GetMethodID(gj_TrackInfoClass,
                                                 "nGetType",
                                                 "()I");
        gj_TrackInfo_Index = env->GetFieldID(gj_TrackInfoClass, "index", "I");
        gj_TrackInfo_Description = env->GetFieldID(gj_TrackInfoClass, "description",
                                                   "Ljava/lang/String;");
        gj_TrackInfo_VideoBitrate = env->GetFieldID(gj_TrackInfoClass, "videoBitrate", "I");
        gj_TrackInfo_VideoWidth = env->GetFieldID(gj_TrackInfoClass, "videoWidth", "I");
        gj_TrackInfo_VideoHeight = env->GetFieldID(gj_TrackInfoClass, "videoHeight", "I");
        gj_TrackInfo_VideoCodec = env->GetFieldID(gj_TrackInfoClass, "videoCodec",
                                                  "Ljava/lang/String;");
        gj_TrackInfo_AudioLang = env->GetFieldID(gj_TrackInfoClass, "audioLang",
                                                 "Ljava/lang/String;");
        gj_TrackInfo_AudioChannels = env->GetFieldID(gj_TrackInfoClass, "audioChannels", "I");
        gj_TrackInfo_AudioSampleRate = env->GetFieldID(gj_TrackInfoClass, "audioSampleRate", "I");
        gj_TrackInfo_AudioSampleFormat = env->GetFieldID(gj_TrackInfoClass, "audioSampleFormat",
                                                         "I");
        gj_TrackInfo_SubtitleLang = env->GetFieldID(gj_TrackInfoClass, "subtitleLang",
                                                    "Ljava/lang/String;");
    }
}

void JavaTrackInfo::unInit(JNIEnv *pEnv) {
    if (gj_TrackInfoClass != nullptr) {
        pEnv->DeleteGlobalRef(gj_TrackInfoClass);
        gj_TrackInfoClass = nullptr;
    }
}


jobject JavaTrackInfo::getTrackInfo(JNIEnv *mEnv, const StreamInfo &streamInfo) {
    jobject jStreamInfo = mEnv->NewObject(gj_TrackInfoClass, gj_TrackInfo_init);
    mEnv->SetIntField(jStreamInfo, gj_TrackInfo_Index, streamInfo.streamIndex);
    mEnv->CallVoidMethod(jStreamInfo, gj_TrackInfo_setType, (int) streamInfo.type);
    NewStringUTF tmpdesc(mEnv, streamInfo.description);
    jstring desc = tmpdesc.getString();
    mEnv->SetObjectField(jStreamInfo, gj_TrackInfo_Description, desc);
    switch (streamInfo.type) {
        case ST_TYPE_VIDEO:
            mEnv->SetIntField(jStreamInfo, gj_TrackInfo_VideoBitrate, streamInfo.videoBandwidth);
            mEnv->SetIntField(jStreamInfo, gj_TrackInfo_VideoHeight, streamInfo.videoHeight);
            mEnv->SetIntField(jStreamInfo, gj_TrackInfo_VideoWidth, streamInfo.videoWidth);
            mEnv->CallVoidMethod(jStreamInfo, gj_TrackInfo_setVideoHDRType, (int) streamInfo.HDRType);
            /*
             * 编码短名：**总是送**。内核认不出来时 videoCodec 是空串，Java 侧拿到的就是空串
             * （界面按"空串 = 不显示徽标"处理）—— 不做任何猜测/映射，也让界面不必区分
             * null 与空串这两种"没有编码"的写法。
             */
            {
                NewStringUTF tmpcodec(mEnv, streamInfo.videoCodec);
                jstring codec = tmpcodec.getString();
                mEnv->SetObjectField(jStreamInfo, gj_TrackInfo_VideoCodec, codec);
            }
            break;
        case ST_TYPE_AUDIO:
            mEnv->SetIntField(jStreamInfo, gj_TrackInfo_AudioChannels, streamInfo.nChannels);
            mEnv->SetIntField(jStreamInfo, gj_TrackInfo_AudioSampleFormat, streamInfo.sampleFormat);
            mEnv->SetIntField(jStreamInfo, gj_TrackInfo_AudioSampleRate, streamInfo.sampleRate);

            if (streamInfo.audioLang) {
                NewStringUTF tmpaudioLang(mEnv, streamInfo.audioLang);
                jstring audioLang = tmpaudioLang.getString();
                mEnv->SetObjectField(jStreamInfo, gj_TrackInfo_AudioLang, audioLang);
            }
            break;
        case ST_TYPE_SUB:
            if (streamInfo.subtitleLang) {
                NewStringUTF tmpsubtitleLang(mEnv, streamInfo.subtitleLang);
                jstring subtitleLang = tmpsubtitleLang.getString();
                mEnv->SetObjectField(jStreamInfo, gj_TrackInfo_SubtitleLang, subtitleLang);
            }
            break;
        default:
            break;
    }
    return jStreamInfo;
}

int JavaTrackInfo::getStreamIndexByTrackInfo(JNIEnv *mEnv, jobject trackInfo) {
    if (trackInfo == nullptr) {
        return -1;
    }

    return mEnv->GetIntField(trackInfo, gj_TrackInfo_Index);
}

StreamInfo *JavaTrackInfo::getStreamInfo(JNIEnv *mEnv, jobject trackInfo) {
    if (trackInfo == nullptr) {
        return nullptr;
    }

    auto *info = static_cast<StreamInfo *>(malloc(sizeof(StreamInfo)));
    memset(info, 0, sizeof(StreamInfo));

    info->streamIndex = mEnv->GetIntField(trackInfo, gj_TrackInfo_Index);
    info->type = static_cast<StreamType>(mEnv->CallIntMethod(trackInfo, gj_TrackInfo_nGetType));
    jstring jdescription = static_cast<jstring>(mEnv->GetObjectField(trackInfo,
                                                                     gj_TrackInfo_Description));
    GetStringUTFChars tmpDesc(mEnv, jdescription);
    char *descChars = tmpDesc.getChars();
    if (descChars != nullptr) {
        info->description = static_cast<char *>(malloc(strlen(descChars) + 1));
        strcpy(info->description, descChars);
    } else {
        info->description = nullptr;
    }

    if (info->type == StreamType::ST_TYPE_VIDEO) {
        info->videoBandwidth = mEnv->GetIntField(trackInfo, gj_TrackInfo_VideoBitrate);
        info->videoWidth = mEnv->GetIntField(trackInfo, gj_TrackInfo_VideoWidth);
        info->videoHeight = mEnv->GetIntField(trackInfo, gj_TrackInfo_VideoHeight);
        /*
         * 编码短名（Java → 内核这条回读路）。StreamInfo::videoCodec 是定长 char[16]
         * 且本函数开头已 memset 过整个结构，所以按 C 字符串拷进来、留一个终止符即可；
         * 拷贝长度压到数组容量 - 1，避免越界。
         */
        jstring jVideoCodec = static_cast<jstring>(mEnv->GetObjectField(trackInfo,
                                                                       gj_TrackInfo_VideoCodec));
        GetStringUTFChars tmpCodec(mEnv, jVideoCodec);
        char *codecChars = tmpCodec.getChars();
        if (codecChars != nullptr) {
            strncpy(info->videoCodec, codecChars, sizeof(info->videoCodec) - 1);
        }
    }
    if (info->type == StreamType::ST_TYPE_AUDIO) {
        jstring jAudioLang = static_cast<jstring>(mEnv->GetObjectField(trackInfo,
                                                                       gj_TrackInfo_AudioLang));
        GetStringUTFChars tmpLang(mEnv, jAudioLang);
        char *descChars = tmpLang.getChars();
        if (descChars != nullptr) {
            info->audioLang = static_cast<char *>(malloc(strlen(descChars) + 1));
            strcpy(info->audioLang, descChars);
        } else {
            info->audioLang = nullptr;
        }
        info->nChannels = mEnv->GetIntField(trackInfo, gj_TrackInfo_AudioChannels);
        info->sampleRate = mEnv->GetIntField(trackInfo, gj_TrackInfo_AudioSampleRate);
        info->sampleFormat = mEnv->GetIntField(trackInfo, gj_TrackInfo_AudioSampleFormat);
    }
    if (info->type == StreamType::ST_TYPE_SUB) {
        jstring jSubLang = static_cast<jstring>(mEnv->GetObjectField(trackInfo,
                                                                     gj_TrackInfo_SubtitleLang));
        GetStringUTFChars tmpLang(mEnv, jSubLang);
        char *descChars = tmpLang.getChars();
        if (descChars != nullptr) {
            info->subtitleLang = static_cast<char *>(malloc(strlen(descChars) + 1));
            strcpy(info->subtitleLang, descChars);
        } else {
            info->subtitleLang = nullptr;
        }

    }
    return info;
}


jobjectArray
JavaTrackInfo::getTrackInfoArray(JNIEnv *mEnv, const std::vector<StreamInfo> &streamInfos,
                                 int len) {
    jobjectArray jStreamInfoArray = mEnv->NewObjectArray(len, gj_TrackInfoClass, nullptr);

    for (int i = 0; i < len; i++) {
        jobject jStreamInfo = getTrackInfo(mEnv, streamInfos.at(i));

        if (jStreamInfo != nullptr) {
            mEnv->SetObjectArrayElement(jStreamInfoArray, i, jStreamInfo);
            mEnv->DeleteLocalRef(jStreamInfo);
        }
    }

    return jStreamInfoArray;
}

jobjectArray JavaTrackInfo::getTrackInfoArray(JNIEnv *mEnv, MediaInfo *mediaInfo)
{
    int len = mediaInfo->mStreamInfoQueue.size();
    jobjectArray jStreamInfoArray = mEnv->NewObjectArray(len, gj_TrackInfoClass, nullptr);

    for (int i = 0; i < len; i++) {
        jobject jStreamInfo = getTrackInfo(mEnv, *(mediaInfo->mStreamInfoQueue[i]));

        if (jStreamInfo != nullptr) {
            mEnv->SetObjectArrayElement(jStreamInfoArray, i, jStreamInfo);
            mEnv->DeleteLocalRef(jStreamInfo);
        }
    }

    return jStreamInfoArray;
}
