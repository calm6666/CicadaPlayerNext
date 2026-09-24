//
// Created by pingkai on 2020/11/2.
//

#ifndef CICADAMEDIA_SMPAVDEVICEMANAGER_H
#define CICADAMEDIA_SMPAVDEVICEMANAGER_H

#include <codec/IDecoder.h>
#include <drm/DrmManager.h>
#include <render/audio/IAudioRender.h>
#include <render/video/IVideoRender.h>

#ifdef __APPLE__
#include <codec/Apple/AppleVideoToolBox.h>
#endif


// TODO: add create lock
namespace Cicada {
    class SMPAVDeviceManager {
    public:
        enum deviceType { DEVICE_TYPE_VIDEO = 1 << 0, DEVICE_TYPE_AUDIO = 1 << 1 };

        struct DecoderHandle {
            std::unique_ptr<IDecoder> decoder{nullptr};
            Stream_meta meta{};
            bool valid{false};
            uint64_t decFlag;
            void *device;
            uint32_t mDstFormat{0};
            DrmInfo mDrmInfo{};

            bool match(const Stream_meta *pMeta, uint64_t flag, void *pDevice, uint32_t dstFormat, const DrmInfo &info)
            {
#ifdef __APPLE__
                auto *vtbDecoder = dynamic_cast<AFVTBDecoder *>(decoder.get());
                if (vtbDecoder) {
                    if (pMeta->interlaced) {
                        return false;
                    }
                }
#endif
                return (pDevice == device) && (flag == decFlag) && (pMeta->codec == meta.codec) && (dstFormat == mDstFormat) &&
                       (mDrmInfo == info);
            }
        };

    public:
        SMPAVDeviceManager();
        ~SMPAVDeviceManager();

        IDecoder *getDecoder(deviceType type) const
        {
            if (type == DEVICE_TYPE_VIDEO) {
                return mVideoDecoder.decoder.get();
            } else if (type == DEVICE_TYPE_AUDIO) {
                return mAudioDecoder.decoder.get();
            }
            return nullptr;
        }

        /**
         * 待切换视频解码器。
         *
         * 清晰度切换期间，旧解码器仍然负责输出当前画面；新码流只送入这
         * 个 pending decoder，等它成功解出第一帧后再 promote。该接口位于
         * 公共核心层，因此软解、MediaCodec、VideoToolbox、FFmpeg 等平台
         * 都遵循同一套切换语义，渲染层无需知道切换细节。
         */
        IDecoder *getPendingVideoDecoder() const
        {
            return mPendingVideoDecoder.decoder.get();
        }
        bool isPendingVideoDecoderValid() const
        {
            return mPendingVideoDecoder.valid;
        }
        // 创建 pending decoder；创建失败时 active decoder 不受影响。
        int setUpPendingVideoDecoder(uint64_t decFlag, const Stream_meta *meta, void *device, uint32_t dstFormat);
        // 从 pending decoder 取出已解码帧，送入 pending 帧队列。
        int getPendingVideoFrame(std::unique_ptr<IAFFrame> &frame, uint64_t timeOut);
        // 只向 pending decoder 投递目标清晰度的数据包，禁止混入 active decoder。
        int sendPendingVideoPacket(std::unique_ptr<IAFPacket> &packet, uint64_t timeOut);
        // 仅标记 pending 无效，供异常恢复路径使用。
        void invalidatePendingVideoDecoder();
        // 取消切换并释放 pending decoder 及其内部资源。
        void discardPendingVideoDecoder();
        // 新 decoder 首帧就绪后，把 pending 提升为 active。
        void promotePendingVideoDecoder();
        // 将旧 active 解码器保留到新帧真正交给渲染器之后，避免 Qt/D3D11 仍引用旧 surface。
        void releaseRetiredVideoDecoder();

        bool isDecoderValid(deviceType type) const
        {
            if (type == DEVICE_TYPE_VIDEO) {
                return mVideoDecoder.valid;
            } else if (type == DEVICE_TYPE_AUDIO) {
                return mAudioDecoder.valid;
            }
            return false;
        }

        /**
         * 现在的解码器是拿哪份 stream meta 建起来的（没有解码器时返回 nullptr）。
         * 切流时需要用它判断新码流还能不能交给现有解码器解：codec id / 分辨率
         * 一旦不同，旧解码器一帧都出不来（见 SuperMediaPlayer::isVideoDecoderMetaMatched）。
         */
        const Stream_meta *getDecoderMeta(deviceType type) const
        {
            if (type == DEVICE_TYPE_VIDEO) {
                return mVideoDecoder.decoder ? &mVideoDecoder.meta : nullptr;
            } else if (type == DEVICE_TYPE_AUDIO) {
                return mAudioDecoder.decoder ? &mAudioDecoder.meta : nullptr;
            }
            return nullptr;
        }
        bool isAudioRenderValid() const
        {
            return mAudioRenderValid;
        }

        void invalidDevices(uint64_t deviceTypes);

        /**
         * 仅把解码器标记为无效（不触碰 audio/render 资源）。
         * surface 重建热重绑失败后，调用 setUpDecoder 会关闭旧解码器并新建。
         */
        void invalidateDecoder(uint64_t deviceTypes);

        void flushDevice(uint64_t deviceTypes);

        int getFrame(std::unique_ptr<IAFFrame> &frame, deviceType type, uint64_t timeOut);

        int sendPacket(std::unique_ptr<IAFPacket> &packet, deviceType type, uint64_t timeOut);

        int setVolume(float volume);

        void setMute(bool mute);

        uint64_t getAudioRenderQueDuration();

        int renderAudioFrame(std::unique_ptr<IAFFrame> &frame, int timeOut);

        void pauseAudioRender(bool pause);

        int setUpAudioRender(const IAFFrame::audioInfo &info);

        int setSpeed(float speed);

        int64_t getAudioRenderPosition();

        void setAudioRenderListener(IAudioRenderListener *listener);

        void setVideoRenderListener(IVideoRender::IVideoRenderListener *listener);

        void setAudioRenderingCb(renderingFrameCB cb, void *userData);

        void setVideoRenderingCb(videoRenderingFrameCB cb, void *userData);

        int setUpDecoder(uint64_t decFlag, const Stream_meta *meta, void *device, deviceType type, uint32_t dstFormat);

        uint64_t getVideoDecoderFlags();

        int createVideoRender(uint64_t flags);

        void destroyVideoRender();

        bool isVideoRenderValid()
        {
            return mVideoRenderValid;
        }
        IVideoRender *getVideoRender()
        {
            if (mVideoRender) {
                return mVideoRender.get();
            }
            return nullptr;
        }
        void flushVideoRender();

        int renderVideoFrame(std::unique_ptr<IAFFrame> &frame);

        void setDrmRequestCallback(const std::function<DrmResponseData *(const DrmRequestParam &drmRequestParam)> &drmCallback);

    private:
        DecoderHandle *getDecoderHandle(const deviceType &type);

    private:
        std::mutex mMutex{};
        DecoderHandle mAudioDecoder;
        DecoderHandle mVideoDecoder;
        DecoderHandle mPendingVideoDecoder;
        // 切换提交后暂存旧解码器；渲染线程确认新帧上屏前绝不能销毁。
        DecoderHandle mRetiredVideoDecoder;
        std::unique_ptr<IAudioRender> mAudioRender{nullptr};
        IAFFrame::audioInfo mAudioRenderInfo{};
        bool mAudioRenderValid{false};
        bool mMute{false};
        std::unique_ptr<IVideoRender> mVideoRender{nullptr};
        bool mVideoRenderValid{false};
        uint64_t mVideoRenderFlags{0};

        std::unique_ptr<DrmManager> mDrmManager{};
    };
}// namespace Cicada


#endif//CICADAMEDIA_SMPAVDEVICEMANAGER_H
