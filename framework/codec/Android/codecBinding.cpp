//
// 绑定工厂 + Java 绑定适配 + 运行期路由。设计见 docs/ANDROID-NDK-ASYNC-DECODER.md。
//
// 一句话：内核只看到一个 IAndroidCodecBinding；具体走 NDK 异步还是 Java 异步，
// 由**平台能力**（libmediandk 是否提供 setAsyncNotifyCallback）与**内容能力要求**
// （secure 内容需要 Java MediaCrypto；占位/空 surface 需要 Java DummySurface）决定。
// 两条路都是异步 —— 本文件里没有任何同步轮询/降级路径。
//

#include "codecBinding.h"

#include <utils/frame_work_log.h>

#if defined(__ANDROID__)
#include "ndkCodecBinding.h"
#endif

using namespace Cicada;

namespace Cicada {
    namespace {

        /* ==================== Java 绑定：1:1 转发到既有 JNI 包装 ====================
         * 行为与迁移前逐字相同（同一个 MediaCodec_Decoder 对象、同一套 Java 异步泵），
         * 因此 API 24~27 的设备与 secure/占位 surface 场景的行为不变。
         */
        class JavaCodecBinding : public IAndroidCodecBinding {
        public:
            JavaCodecBinding() = default;

            ~JavaCodecBinding() override = default;

            void setCodecSpecificData(
                    const std::list<std::unique_ptr<CodecSpecificData>> &csds) override
            {
                mJava.setCodecSpecificData(csds);
            }

            int setDrmInfo(const std::string &uuid, const void *sessionId, int size) override
            {
                return mJava.setDrmInfo(uuid, sessionId, size);
            }

            void setForceInsecureDecoder(bool force) override
            {
                mJava.setForceInsecureDecoder(force);
            }

            int setDecodeBoost(bool boost) override
            {
                return mJava.setDecodeBoost(boost);
            }

            int configureVideo(const std::string &mime, int width, int height, int angle, void *surface,
                               bool usePlaceholderSurface) override
            {
                return mJava.configureVideo(mime, width, height, angle, surface, usePlaceholderSurface);
            }

            int configureAudio(const std::string &mime, int sampleRate, int channelCount,
                               int isADTS) override
            {
                return mJava.configureAudio(mime, sampleRate, channelCount, isADTS);
            }

            int setOutputSurface(void *surface) override
            {
                return mJava.setOutputSurface(surface);
            }

            int start() override
            {
                return mJava.start();
            }

            int flush() override
            {
                return mJava.flush();
            }

            int stop() override
            {
                return mJava.stop();
            }

            int release() override
            {
                return mJava.release();
            }

            int dequeueInputBufferIndex(int64_t timeoutUs) override
            {
                return mJava.dequeueInputBufferIndex(timeoutUs);
            }

            int queueInputBuffer(int index, void *buffer, size_t size, int64_t pts,
                                 bool isConfig) override
            {
                return mJava.queueInputBuffer(index, buffer, size, pts, isConfig);
            }

            int queueSecureInputBuffer(int index, void *buffer, size_t size,
                                       IAFPacket::EncryptionInfo *pEncryptionInfo, int64_t pts,
                                       bool isConfig) override
            {
                return mJava.queueSecureInputBuffer(index, buffer, size, pEncryptionInfo, pts, isConfig);
            }

            int dequeueOutputBufferIndex(int64_t timeoutUs) override
            {
                return mJava.dequeueOutputBufferIndex(timeoutUs);
            }

            int getOutput(int index, mc_out *out, bool readBuffer) override
            {
                return mJava.getOutput(index, out, readBuffer);
            }

            int releaseOutputBuffer(int index, bool render) override
            {
                return mJava.releaseOutputBuffer(index, render);
            }

            const char *bindingName() const override
            {
                return "java-async";
            }

            /* 控制面：把"选哪颗 codec"交给 Java 侧同一套逻辑（见 MediaCodecDecoder.java）。 */
            std::string selectCodecName(bool isVideo, const std::string &mime, int width, int height,
                                        int sampleRate, int channelCount, int isADTS)
            {
                return mJava.selectCodecName(isVideo, mime, width, height, sampleRate, channelCount,
                                             isADTS);
            }

        private:
            MediaCodec_Decoder mJava;
        };

        /* ==================== 调度器：内核看到的唯一绑定 ==================== */
        class AndroidCodecBinding : public IAndroidCodecBinding {
        public:
            AndroidCodecBinding() : mJava(new JavaCodecBinding())
            {
                /*
                 * Java 绑定**总是**构造：它是控制面（按名字选 codec）与安全/占位场景的回退，
                 * 构造时机与迁移前一致（仍在 mediaCodecDecoder 构造期）。
                 */
#if defined(__ANDROID__)

                if (NdkCodecBinding::isAvailable()) {
                    mNdk.reset(new NdkCodecBinding());
                    mNdk->setCodecNameProvider([this](bool isVideo, const std::string &mime, int width,
                                                      int height, int sampleRate, int channelCount,
                                                      int isADTS) -> std::string {
                        return mJava->selectCodecName(isVideo, mime, width, height, sampleRate,
                                                      channelCount, isADTS);
                    });
                    mUseNdk = true;
                    AF_LOGI("[ndk-codec] binding=ndk-async (AMediaCodec + setAsyncNotifyCallback)\n");
                } else {
                    AF_LOGI("[ndk-codec] binding=java-async (%s)\n",
                            NdkCodecBinding::isMarkedUnavailable()
                            ? NdkCodecBinding::unavailableReason()
                            : "API<28: libmediandk has no setAsyncNotifyCallback");
                }

#else
                AF_LOGI("[ndk-codec] binding=java-async (not building for Android)\n");
#endif
            }

            ~AndroidCodecBinding() override = default;

            void setCodecSpecificData(
                    const std::list<std::unique_ptr<CodecSpecificData>> &csds) override
            {
                /* 两边各自持有一份：NDK 绑定需要自己拷贝（configure 时才用），
                 * Java 绑定按既有方式立刻建 MediaFormat。 */
                mJava->setCodecSpecificData(csds);

                if (mNdk) {
                    mNdk->setCodecSpecificData(csds);
                }
            }

            int setDrmInfo(const std::string &uuid, const void *sessionId, int size) override
            {
                int ret = mJava->setDrmInfo(uuid, sessionId, size);

                if (ret == 0 && mUseNdk) {
                    /*
                     * secure 内容：必须走 Java（MediaCrypto 会话全链路在 Java 侧）。
                     * 若 NDK 已经 configure 过（调用顺序异常的兜底），把同一份配置在 Java 侧重放一次。
                     */
                    AF_LOGW("[ndk-codec] secure content ⇒ switch this decoder instance to java-async\n");
                    switchToJava();
                }

                return ret;
            }

            void setForceInsecureDecoder(bool force) override
            {
                /* 只影响"选哪颗 codec"（Java 控制面），NDK 侧不需要。 */
                mJava->setForceInsecureDecoder(force);

                if (mNdk) {
                    mNdk->setForceInsecureDecoder(force);
                }
            }

            int setDecodeBoost(bool boost) override
            {
                return active()->setDecodeBoost(boost);
            }

            int configureVideo(const std::string &mime, int width, int height, int angle, void *surface,
                               bool usePlaceholderSurface) override
            {
                mConfigured = true;
                mVideoConfigured = true;
                mMime = mime;
                mWidth = width;
                mHeight = height;
                mAngle = angle;
                mSurface = surface;
                mPlaceholder = usePlaceholderSurface;

#if defined(__ANDROID__)

                if (mUseNdk) {
                    int ret = mNdk->configureVideo(mime, width, height, angle, surface,
                                                   usePlaceholderSurface);

                    if (ret == 0) {
                        mNdkConfigured = true;
                        return 0;
                    }

                    /*
                     * 本设备/本参数组合下 NDK 路不可用（占位 surface、创建/配置失败…）。
                     * 这是"平台能力"回落，不是行为开关：整实例改用 Java 异步绑定。
                     */
                    AF_LOGW("[ndk-codec] ndk configureVideo failed ⇒ fall back to java-async\n");
                    mUseNdk = false;
                }

#endif
                return mJava->configureVideo(mime, width, height, angle, surface,
                                             usePlaceholderSurface);
            }

            int configureAudio(const std::string &mime, int sampleRate, int channelCount,
                               int isADTS) override
            {
                mConfigured = true;
                mVideoConfigured = false;
                mMime = mime;
                mSampleRate = sampleRate;
                mChannelCount = channelCount;
                mIsADTS = isADTS;

#if defined(__ANDROID__)

                if (mUseNdk) {
                    int ret = mNdk->configureAudio(mime, sampleRate, channelCount, isADTS);

                    if (ret == 0) {
                        mNdkConfigured = true;
                        return 0;
                    }

                    AF_LOGW("[ndk-codec] ndk configureAudio failed ⇒ fall back to java-async\n");
                    mUseNdk = false;
                }

#endif
                return mJava->configureAudio(mime, sampleRate, channelCount, isADTS);
            }

            int setOutputSurface(void *surface) override
            {
                mSurface = surface;
                return active()->setOutputSurface(surface);
            }

            int start() override
            {
                int ret = active()->start();

                if (ret == 0) {
                    mStarted = true;
                }

                return ret;
            }

            int flush() override
            {
                return active()->flush();
            }

            int stop() override
            {
                mStarted = false;
                return active()->stop();
            }

            int release() override
            {
                int ret = 0;

                if (mNdk) {
                    ret = mNdk->release();
                }

                int javaRet = mJava->release();
                mStarted = false;
                mNdkConfigured = false;
                mConfigured = false;
                return (ret == 0) ? javaRet : ret;
            }

            int dequeueInputBufferIndex(int64_t timeoutUs) override
            {
                return active()->dequeueInputBufferIndex(timeoutUs);
            }

            int queueInputBuffer(int index, void *buffer, size_t size, int64_t pts,
                                 bool isConfig) override
            {
                return active()->queueInputBuffer(index, buffer, size, pts, isConfig);
            }

            int queueSecureInputBuffer(int index, void *buffer, size_t size,
                                       IAFPacket::EncryptionInfo *pEncryptionInfo, int64_t pts,
                                       bool isConfig) override
            {
                return active()->queueSecureInputBuffer(index, buffer, size, pEncryptionInfo, pts,
                                                        isConfig);
            }

            int dequeueOutputBufferIndex(int64_t timeoutUs) override
            {
                return active()->dequeueOutputBufferIndex(timeoutUs);
            }

            int getOutput(int index, mc_out *out, bool readBuffer) override
            {
                return active()->getOutput(index, out, readBuffer);
            }

            int releaseOutputBuffer(int index, bool render) override
            {
                return active()->releaseOutputBuffer(index, render);
            }

            const char *bindingName() const override
            {
                return active()->bindingName();
            }

        private:
            IAndroidCodecBinding *active()
            {
#if defined(__ANDROID__)

                if (mUseNdk && mNdk) {
                    return mNdk.get();
                }

#endif
                return mJava.get();
            }

            const IAndroidCodecBinding *active() const
            {
#if defined(__ANDROID__)

                if (mUseNdk && mNdk) {
                    return mNdk.get();
                }

#endif
                return mJava.get();
            }

            /*
             * 把"整个实例"从 NDK 切到 Java（secure 内容用）。只在 configure 之后发生才需要重放，
             * 正常情况下内核是"setDrmInfo → configure → start"，不会走到重放。
             */
            void switchToJava()
            {
#if defined(__ANDROID__)

                if (!mUseNdk) {
                    return;
                }

                mUseNdk = false;

                if (!mNdkConfigured) {
                    return;
                }

                mNdk->release();
                mNdkConfigured = false;

                if (mConfigured) {
                    int ret = mVideoConfigured
                              ? mJava->configureVideo(mMime, mWidth, mHeight, mAngle, mSurface,
                                                      mPlaceholder)
                              : mJava->configureAudio(mMime, mSampleRate, mChannelCount, mIsADTS);
                    AF_LOGW("[ndk-codec] replay configure on java-async: ret=%d\n", ret);

                    if (ret == 0 && mStarted) {
                        int startRet = mJava->start();
                        AF_LOGW("[ndk-codec] replay start on java-async: ret=%d\n", startRet);
                    }
                }

#endif
            }

            std::unique_ptr<JavaCodecBinding> mJava;
#if defined(__ANDROID__)
            std::unique_ptr<NdkCodecBinding> mNdk;
#endif
            bool mUseNdk{false};
            bool mNdkConfigured{false};
            bool mConfigured{false};
            bool mStarted{false};
            bool mVideoConfigured{false};
            bool mPlaceholder{false};
            std::string mMime{};
            int mWidth{0};
            int mHeight{0};
            int mAngle{0};
            void *mSurface{nullptr};
            int mSampleRate{0};
            int mChannelCount{0};
            int mIsADTS{0};
        };

    } // namespace

    std::unique_ptr<IAndroidCodecBinding> createAndroidCodecBinding()
    {
        return std::unique_ptr<IAndroidCodecBinding>(new AndroidCodecBinding());
    }

} // namespace Cicada
