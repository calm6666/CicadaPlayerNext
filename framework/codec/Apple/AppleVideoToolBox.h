#ifndef AFVTB_DECODER_H
#define AFVTB_DECODER_H

#include "codec/ActiveDecoder.h"
#include <map>
#include <VideoToolbox/VideoToolbox.h>
#include <CoreMedia/CoreMedia.h>
#include <queue>
#include <base/media/PBAFFrame.h>
#include <codec/utils_ios.h>
#include <utils/AFMediaType.h>
#include <utils/mediaTypeInternal.h>
#include <utils/bitStreamParser.h>
#include "../codecPrototype.h"

namespace Cicada{

    class CICADA_CPLUS_EXTERN AFVTBDecoder : public ActiveDecoder, private codecPrototype, private IOSNotificationObserver {
    public:
        AFVTBDecoder();

        ~AFVTBDecoder() override;

        static bool is_supported(enum AFCodecID codec);

        /*
         * 【设备硬解能力查询（Apple 平台实现）】
         *
         * VideoToolbox 的能力问题，用 VTIsHardwareDecodeSupported()（iOS 11+ /
         * macOS 10.13+）回答：H.264 / H.265 / MPEG-4 三个能映射到
         * CMVideoCodecType 的问系统，其余编码（VideoToolbox 没有对应类型，例如 AV1）
         * 一律返回 true —— 语义是"查不到 = 视为支持"，宁可只用效率序，也不许把
         * 一条其实解得了的流误判成解不了。
         *
         * 只声明，不新增虚函数：类布局与 vtable 不变。
         */
        static bool is_hardware_decode_supported(enum AFCodecID codec);

        void setEOF() override
        {
        }

        int setPixelBufferFormat(OSType format);

    private:
        /*
         * 可空性标注：macOS 15 SDK 打开了 -Wnullability-completeness，而本头文件里
         * 凡是**真的允许为空**的指针参数都显式标注（本文件既有惯例就是 CM_NULLABLE，
         * 见下面的 decompressionOutputCallback 与 mVTDecompressSessionRef）。
         *
         *   · wnd —— 本工程自己有两处直接传 nullptr 调用它
         *     （dequeue_decoder 与 flush_decoder 里的 `init_decoder(..., nullptr, ...)`），
         *     所以它是 nullable，不能笼统地按 nonnull 处理；
         *   · drmInfo —— 本来就是 `const DrmInfo*` 且调用方会传 nullptr。
         *
         * 为什么不用 NS_ASSUME_NONNULL_BEGIN 整段包起来：那会把本头文件里**所有**
         * 未标注的指针一次性判成 nonnull，而这个类的私有成员里本来就有可空句柄。
         * 逐参数标注更诚实，也不会把"实际可能为空"的地方悄悄改成 nonnull 语义。
         */
        int init_decoder(const Stream_meta *meta, void *CM_NULLABLE wnd, uint64_t flags,
                         const DrmInfo *CM_NULLABLE drmInfo) override;

        void close_decoder() override;

        int enqueue_decoder(std::unique_ptr<IAFPacket> &pPacket) override;

        int dequeue_decoder(std::unique_ptr<IAFFrame> &pFrame) override;

        void flush_decoder() override;

        int get_decoder_recover_size() override;

        void decoder_updateMetaData(const Stream_meta *meta) override;

        void flushReorderQueue();

    private:
        explicit AFVTBDecoder(int dummy)
        {
            addPrototype(this);
            mIsDummy = true;
        };

        AFVTBDecoder *clone() override
        {
            return new AFVTBDecoder();
        };

        bool is_supported(const Stream_meta &meta, uint64_t flags, int maxSize) override
        {
            if (!(flags & DECFLAG_HW))
                return false;
            return is_supported(meta.codec);
        };
        
        bool is_drmSupport(const DrmInfo* drmInfo) override
        {
            return false;
        }
        
        static AFVTBDecoder se;

    private:

        int init_decoder_internal();

        void push_to_recovery_queue(std::unique_ptr<IAFPacket> pPacket);

        void gen_recovering_queue();

        int enqueue_decoder_internal(std::unique_ptr<IAFPacket> &pPacket);

        int process_extra_data(IAFPacket *pPacket);

        int createDecompressionSession(uint8_t *pData, int size, int width, int height);

        int createVideoFormatDesc(const uint8_t *pData, int size, int width, int height, CFDictionaryRef &decoder_spec,
                                  CMFormatDescriptionRef &cm_fmt_desc);

        static void decompressionOutputCallback(void *CM_NULLABLE decompressionOutputRefCon,
                                                void *CM_NULLABLE sourceFrameRefCon,
                                                OSStatus status,
                                                VTDecodeInfoFlags infoFlags,
                                                CM_NULLABLE CVImageBufferRef imageBuffer,
                                                CMTime presentationTimeStamp,
                                                CMTime presentationDuration);

        // packet 是**可空**的：decompressionOutputCallback 在
        // kVTVideoDecoderMalfunctionErr 路径上会把 sourceFrameRefCon（可能为空）
        // 直接转成 IAFPacket* 传进来，onDecoded 开头的 `packet == nullptr` 就是为此。
        void onDecoded(IAFPacket *CM_NULLABLE packet, std::unique_ptr<PBAFFrame> frame, OSStatus status);


        void AppWillResignActive() override;

        void AppDidBecomeActive() override;

        inline bool isErrorPoc(int poc, bool keyFrame) const;
        inline void resetPocInfo();

#if 0
        void outputByPoc(VideoFrame *CM_NULLABLE beCachedFrame, int poc);
#endif
    private:
        CM_NULLABLE VTDecompressionSessionRef mVTDecompressSessionRef{nullptr};
        CM_NULLABLE CMVideoFormatDescriptionRef mVideoFormatDesRef{nullptr};
        CM_NULLABLE CFDictionaryRef mDecoder_spec{nullptr};
        const static int MAX_POC_ERROR = 3;
        CMVideoCodecType mVideoCodecType{0};
        std::map<int64_t, std::unique_ptr<PBAFFrame>> mReorderFrameMap;
        int mInputCount{0};
        bool mThrowPacket{false};
        std::mutex mActiveStatusMutex;
        std::atomic_bool mActive{true};
        AFPixelFormat mVTOutFmt = AF_PIX_FMT_NONE;
        std::unique_ptr<streamMeta> mPInMeta{nullptr};
        std::queue<std::unique_ptr<IAFPacket>> mRecoveryQueue{};
        std::queue<std::unique_ptr<IAFPacket>> mRecoveringQueue{};
        std::unique_ptr<bitStreamParser> mParser{nullptr};
        uint8_t mPocDelta = 2;
        std::atomic_int64_t mOutputPoc {0};
        bool mBUsePoc = false;
        std::queue<std::unique_ptr<IAFFrame>> mReorderedQueue{};
        std::mutex mReorderMutex{};

        bool mResignActive{false};
        bool mIsDummy = false;
        int mPocErrorCount{0};
        OSType outPutFormat{0};
    };

}

#endif
