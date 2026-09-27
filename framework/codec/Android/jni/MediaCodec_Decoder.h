//
// Created by SuperMan on 2020/10/13.
//

#ifndef SOURCE_MEDIACODEC_DECODER_H
#define SOURCE_MEDIACODEC_DECODER_H

#include <base/media/IAFPacket.h>
#include <cassert>
#include <jni.h>
#include <map>
#include <string>

#define MC_ERROR (-1)
#define MC_INFO_OUTPUT_FORMAT_CHANGED (-2)
#define MC_INFO_OUTPUT_BUFFERS_CHANGED (-3)
#define MC_INFO_TRYAGAIN (-11)

#define CATEGORY_VIDEO (0)
#define CATEGORY_AUDIO (1)

namespace Cicada {
    struct mc_out {
        int type;
        bool b_eos;
        union {
            struct {
                int index;
                int64_t pts;
                const uint8_t *p_ptr;
                size_t size;
            } buf;
            union {
                struct {
                    unsigned int width, height;
                    unsigned int stride;
                    unsigned int slice_height;
                    int pixel_format;
                    int crop_left;
                    int crop_top;
                    int crop_right;
                    int crop_bottom;
                } video;
                struct {
                    int channel_count;
                    int channel_mask;
                    int sample_rate;
                    int format;
                } audio;
            } conf;
        };
    };

    class CodecSpecificData {
    public:
        CodecSpecificData() = default;
        ~CodecSpecificData()
        {
            if (buffer != nullptr) {
                free(buffer);
            }
        }

        void setScd(const std::string& keyStr ,void* data, int size){
            assert(data != nullptr);

            len = size;
            key = keyStr;
            buffer = malloc(size);
            memcpy(buffer, data, size);
        }

        std::string key{};
        void *buffer = nullptr;
        int len = 0;
    };

    class MediaCodec_Decoder {
    public:
        static void init(JNIEnv *env);

        static void unInit(JNIEnv *env);

    public:

        MediaCodec_Decoder();

        ~MediaCodec_Decoder();

        void setCodecSpecificData(const std::list<std::unique_ptr<CodecSpecificData>> &csds);

        int setDrmInfo(const std::string &uuid, const void *sessionId, int size);

        void setForceInsecureDecoder(bool force);

        /*
         * 【追帧加速】把"更高性能点"的要求下发到 MediaCodec（Java 侧
         * MediaCodec.setParameters("operating-rate")）。只对视频有效，失败静默。
         */
        int setDecodeBoost(bool boost);

        int
        configureVideo(const std::string &mime, int width, int height, int angle, void *surface,
                       bool usePlaceholderSurface);

        int configureAudio(const std::string &mime, int sampleRate, int channelCount, int isADTS);

        /*
         * 【控制面：NDK 数据面专用】只做"选择"、不做"创建" —— 用与 configureVideo/
         * configureAudio 完全相同的依据（同一份 MediaCodecList 顺序、同一份黑名单、
         * 同一份 secure 判定）返回**将要被 createByCodecName() 使用**的 codec 名。
         *
         * NDK 异步绑定（ndkCodecBinding）用它 + AMediaCodec_createCodecByName() 精确锁定
         * 同一颗 codec（按名字创建自 API 21 就有，minSdk 24 无需弱符号），从而保证
         * "Java 能力面选中的"与"NDK 实际创建的"是同一颗。
         *
         * 纯查询：不创建 MediaCodec、不改任何 Java 状态（只按入参临时组一个 MediaFormat）。
         * 返回空串 = 选择失败（调用方回落 Java 绑定）。
         */
        std::string selectCodecName(bool isVideo, const std::string &mime, int width, int height,
                                    int sampleRate, int channelCount, int isADTS);

        /**
         * surface 重建后热重绑 MediaCodec 输出 surface（隧道直通模式专用）
         */
        int setOutputSurface(void *surface);

        int start();

        int flush();

        int stop();

        int release();

        int dequeueInputBufferIndex(int64_t timeoutUs);

        int queueInputBuffer(int index, void *buffer, size_t size, int64_t pts, bool isConfig);

        int queueSecureInputBuffer(int index, void *buffer, size_t size,
                                   IAFPacket::EncryptionInfo *pEncryptionInfo, int64_t pts,
                                   bool isConfig);

        int dequeueOutputBufferIndex(int64_t timeoutUs);

        int getOutput(int index, mc_out *out, bool readBuffer);

        int releaseOutputBuffer(int index, bool render);

        /*
         * 【设备硬解能力查询】把"这台设备有没有这个 mime 的**硬件**解码器"问回 Java 侧
         * （com.cicada.player.utils.media.MediaCodecUtils.isHardwareDecodeSupported）。
         *
         * 类句柄与方法 ID 在 init() 里就缓存好 —— init() 是 JNI_OnLoad 从 **Java 线程**
         * 调用的，那时 FindClass 找得到应用侧的类；这是本文件里所有 Java 类的既有做法。
         * 绝不能等到查询时再 FindClass：能力查询发生在内核线程上，那里的 FindClass 走
         * 系统类加载器，找不到应用侧的类，会静默失败（本次改动在 Android 上就等于没生效）。
         *
         * 只回答"有/没有"：Java 侧明确返回 true 才是 true；所有"查不到"的情形
         * （旧版 Java 没有这个方法、拿不到 JNIEnv、Java 侧抛异常、查询结果拿不到）
         * 也返回 true，由调用方统一按"未知 = 视为支持"处理。
         */
        static bool isHardwareDecodeSupported(const char *mime);

    private:
        jobject mMediaCodec{nullptr};

        int mCodecCategory{CATEGORY_VIDEO};
    };

}

#endif //SOURCE_MEDIACODEC_DECODER_H
