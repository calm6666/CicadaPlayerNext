//
// Android 解码器"绑定"抽象：同一套内核解码器（mediaCodecDecoder）之下的两种平台绑定。
//
// 背景与设计见 docs/ANDROID-NDK-ASYNC-DECODER.md。要点：
//   · 内核（mediaCodecDecoder.cpp）只认这个接口，不再直接依赖 JNI 包装类；
//   · 两种实现**都是异步**：
//       - ndkCodecBinding：AMediaCodec + AMediaCodec_setAsyncNotifyCallback（API 28+，弱符号）
//       - JavaCodecBinding：现有 JNI + Java MediaCodec 异步回调（API 24~27，以及 DRM/secure）
//   · 本设计**没有同步轮询路径**：不出现 dequeue* 的轮询降级，也不保留 sAsyncBroken 那套。
//   · 只用 NDK r25c；minSdk 24 不抬。
//

#ifndef CICADA_ANDROID_CODEC_BINDING_H_
#define CICADA_ANDROID_CODEC_BINDING_H_

#include <cstdint>
#include <cstddef>
#include <list>
#include <memory>
#include <string>

#include <base/media/IAFPacket.h>

#include "jni/MediaCodec_Decoder.h"

namespace Cicada {

    /*
     * 与 MediaCodec_Decoder（JNI 绑定）逐字一致的接口 —— 方法名、参数、返回码
     * （MC_ERROR / MC_INFO_* / index>=0）都与既有实现相同，因此 mediaCodecDecoder.cpp
     * 的调用点与状态机不需要改语义，只把成员类型从 MediaCodec_Decoder* 换成
     * std::unique_ptr<IAndroidCodecBinding>。
     */
    class IAndroidCodecBinding {
    public:
        virtual ~IAndroidCodecBinding() = default;

        virtual void setCodecSpecificData(const std::list<std::unique_ptr<CodecSpecificData>> &csds) = 0;

        virtual int setDrmInfo(const std::string &uuid, const void *sessionId, int size) = 0;

        virtual void setForceInsecureDecoder(bool force) = 0;

        /* 追帧加速：下发"更高性能点"要求（Java 走 setParameters("operating-rate")，
         * NDK 走 AMediaCodec_setParameters 的同名 key）。只对视频有效，失败静默。 */
        virtual int setDecodeBoost(bool boost) = 0;

        virtual int configureVideo(const std::string &mime, int width, int height, int angle,
                                   void *surface, bool usePlaceholderSurface) = 0;

        virtual int configureAudio(const std::string &mime, int sampleRate, int channelCount,
                                   int isADTS) = 0;

        /* surface 重建后热重绑输出面（隧道直通模式专用） */
        virtual int setOutputSurface(void *surface) = 0;

        virtual int start() = 0;

        /* 【不变量】flush 之后必须让绑定自己完成"恢复"：
         * Java 侧是 flush() + start()；NDK 侧是 AMediaCodec_flush() + AMediaCodec_start()。
         * 异步模式下 flush 后不恢复 ⇒ 平台永久不再回调输入缓冲（真机卡死的根因）。 */
        virtual int flush() = 0;

        virtual int stop() = 0;

        virtual int release() = 0;

        virtual int dequeueInputBufferIndex(int64_t timeoutUs) = 0;

        virtual int queueInputBuffer(int index, void *buffer, size_t size, int64_t pts,
                                     bool isConfig) = 0;

        virtual int queueSecureInputBuffer(int index, void *buffer, size_t size,
                                           IAFPacket::EncryptionInfo *pEncryptionInfo, int64_t pts,
                                           bool isConfig) = 0;

        virtual int dequeueOutputBufferIndex(int64_t timeoutUs) = 0;

        virtual int getOutput(int index, mc_out *out, bool readBuffer) = 0;

        virtual int releaseOutputBuffer(int index, bool render) = 0;

        /* 诊断用：实际生效的绑定名（"java-async" / "ndk-async"）。 */
        virtual const char *bindingName() const = 0;
    };

    /*
     * 工厂：按**平台能力**选择绑定（不是配置开关）：
     *   · libmediandk 提供 AMediaCodec_setAsyncNotifyCallback（API 28+，弱符号判空）
     *     且尚未被标记为不可用 ⇒ ndk-async；
     *   · 否则 ⇒ java-async（现有实现，API 24~27 唯一的异步路径）。
     * 两者都是异步，所以"无同步轮询"在所有受支持系统上成立。
     */
    std::unique_ptr<IAndroidCodecBinding> createAndroidCodecBinding();

} // namespace Cicada

#endif // CICADA_ANDROID_CODEC_BINDING_H_
