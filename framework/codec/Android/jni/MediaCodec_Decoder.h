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

    private:
        jobject mMediaCodec{nullptr};

        int mCodecCategory{CATEGORY_VIDEO};
    };

}

#endif //SOURCE_MEDIACODEC_DECODER_H
