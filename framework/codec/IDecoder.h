//
// Created by moqi on 2018/8/10.
//

#ifndef FRAMEWORK_VIDEO_DECODER_H
#define FRAMEWORK_VIDEO_DECODER_H

#include <vector>
#include <cstdint>
#include <utils/AFMediaType.h>
#include <string>
#include <mutex>
#include <functional>
#include <af_config.h>
#include <base/media/IAFPacket.h>
#include <drm/DrmHandler.h>
#include "IVideoFrame.h"

enum decoder_status {
    got_pic,
    retry_input,
    new_format,
    decoder_eos,
    decoder_have_error,
    vt_session_invalid,
    decoder_background,
    decoder_create_fail,
    decoder_drm_error,
};

typedef enum DECODER_FRAME_STATUS {
    DECODER_FRAME_INPUT_NORMAL,
    DECODER_FRAME_INPUT_EOF,
    DECODER_FRAME_INPUT_FLUSH,
    DECODER_FRAME_OUTPUT_EOF,
} DECODER_FRAME_STATUS;

#define STATUS_GOT_PIC    (1 << got_pic)
#define STATUS_RETRY_IN   1 << retry_input
#define STATUS_NEWFORMAT  1 << new_format
#define STATUS_EOS        1 << decoder_eos
#define STATUS_HAVE_ERROR 1 << decoder_have_error
#define STATUS_VT_SESSION_INVALID  1 << vt_session_invalid
#define STATUS_VT_DECODER_BACKGROUND  1 << decoder_background
#define STATUS_CREATE_FAIL 1 << decoder_create_fail
#define STATUS_DRM_ERROR 1 << decoder_drm_error


namespace Cicada {

    class IDecoder {
    public:
        typedef struct decoder_error_info_t {
            int error;
            int64_t pts;
        } decoder_error_info;
        enum status {
            OK,
            EOS,
            RETRY = EAGAIN
        };

        IDecoder() = default;

        virtual ~IDecoder()
        {
            clean_error();
        }

        virtual int open(const Stream_meta *meta, void *voutObsr, uint64_t flags , const DrmInfo *drmInfo ) = 0;

        virtual void flush() = 0;

        virtual void updateMetaData(const Stream_meta *meta){};

        /**
         * surface 重建（如前后台切换）后热重绑解码器输出 surface。
         * 仅隧道直通（MediaCodec）解码器需要实现，其余解码器默认 no-op。
         */
        virtual int setOutputSurface(void *surface)
        {
            return 0;
        }

        /**
         * 隧道直通：最近一次实际渲染（releaseOutputBuffer(render=true)）的
         * 视频帧 PTS 与帧间隔（us）。用于暂停后 surface 重建时逐帧精确恢复
         * "暂停的那一帧"。默认返回无意义值。
         */
        virtual int64_t getLastRenderedVideoPts()
        {
            return INT64_MIN;
        }

        virtual int64_t getLastRenderedVideoFrameDur()
        {
            return 0;
        }

        /**
         * 渲染门（暂停帧恢复专用）：设置后只放行 PTS 精确等于 gatePts 的
         * 视频帧，其余帧不上屏（避免从关键帧开始闪帧）；INT64_MIN 关闭。
         */
        virtual void setRenderGate(int64_t gatePts)
        {}

        /**
         * 渲染门是否已命中（门帧已实际渲染）。未启用渲染门时恒为 true。
         * 暂停帧恢复期间 seek 必须等门帧渲染后才能宣告完成（否则读包/解码
         * 管线会在门帧解码前停掉）。
         */
        virtual bool isRenderGateHit()
        {
            return true;
        }

        virtual void close() = 0;

        virtual void prePause() = 0;

        virtual void pause(bool pause) = 0;

        virtual void setEOF()
        {

        }

        /*
         * @param packet the packet send to decoder, use null to signals eos
         * @param timeOut the max time us to wait when packet is full in decoder
         *
         * @return
         *       -AGAIN time out was reached
         */

        virtual int send_packet(std::unique_ptr<IAFPacket> &packet, uint64_t timeOut)
        {
            return -ENOTSUP;
        };

        /*
         * @param[out] frame output the frame decoded
         * @param timeOut the max time us to wait when no frame in decoder
         * @return
         *        -AGAIN time out was reached
         */

        virtual int getFrame(std::unique_ptr<IAFFrame> &frame, uint64_t timeOut) = 0;

#ifdef ENABLE_HWDECODER
        virtual void dec_set_hw_device_content(CICADAHWDeviceType type, void *cont)
        {
            mHw_device_cont = cont;
            mHwDevicetype = type;
        }
#endif

        /*
         * return the number of error packet in decoding progress
         * you can use this to decide stop or continue the decoding
         * it will be reset to zero when flush or close
         */
        virtual int get_error_frame_no()
        {
#if AF_HAVE_PTHREAD
            std::lock_guard<std::mutex> lock(mVideoMutex);
#endif
            return static_cast<int>(mErrorQueue.size());
        }

        /*
         *  get the error info of one packet
         */
        virtual decoder_error_info get_error_frame_info(int index)
        {
#if AF_HAVE_PTHREAD
            std::lock_guard<std::mutex> lock(mVideoMutex);
#endif
            return mErrorQueue[index];
        }

        /*
         * clean the errors
         *
         */
        virtual void clean_error()
        {
#if AF_HAVE_PTHREAD
            std::lock_guard<std::mutex> lock(mVideoMutex);
#endif
            mErrorQueue.clear();
        }

        /*
         * get the decoder flags after open, not equal to the flags passed by open
         */

        virtual int getFlags()
        {
            return mFlags;
        }

        // return true if could recover
        virtual bool enterBackground(bool back)
        {
            mInBackground = back;
            return true;
        }

        std::string getName()
        {
            return mName;
        }

        virtual int holdOn(bool hold) = 0;

        virtual int getRecoverQueueSize() = 0;

        virtual uint32_t getInputPaddingSize() = 0;

        virtual bool supportReuse()
        {
            return true;
        }

        /*
         * 【追帧加速开关（seek / 切档预滚窗口）】
         *
         * 语义：只是给"把 codec 跑在哪个性能点"提个要求 —— **不丢帧、不改时间轴、不动精度**。
         * seek 之后必须从目标之前的关键帧起解，这段前缀本来就要全部解出来，本开关只是让
         * codec 更快地把它们吐出来（安卓侧对应 MediaCodec.setParameters("operating-rate")，
         * 与 ExoPlayer 用 MediaFormat.KEY_OPERATING_RATE 的做法同源）。
         *
         * 默认实现是空操作：只有能真正下发给平台的解码器（安卓 MediaCodec）才覆盖它，
         * 软解/其它平台调用它完全无副作用。**追加在 vtable 末尾**，保证增量 ABI 安全。
         */
        virtual int setDecodeBoost(bool boost)
        {
            (void) boost;
            return 0;
        }

        /*
         * 【当前解码方式的事实读数：这个解码器实例实际在用硬解还是软解】
         *
         * 与"设备/构建支持不支持硬解"的能力查询（decoderFactory::isHardwareDecodeSupported）
         * 是两件事：这里回答的是**活动解码器**眼下真正走的那条路，所以
         *   * 建解码器时硬解没能起来（构建里没有该编码的硬解配置、GPU 设备建不出来）
         *     ⇒ false；
         *   * 解码途中硬解掉了、退回软解（avcodecDecoder 的 getHwFormat 会把状态复位）
         *     ⇒ false。
         * 界面（Qt 的"解码方式"那一栏）据此显示"硬解/软解"，不许读"配置想用什么"。
         *
         * 默认实现按解码器打开后留下的标志位回答（Android 的 mediaCodecDecoder、
         * Apple 的 AFVTBDecoder 在 open 时就置了 DECFLAG_HW，语义正确）；桌面 FFmpeg
         * 那条路（avcodecDecoder）有运行期状态，由它自己覆盖。
         *
         * **追加在 vtable 末尾**，保证增量 ABI 安全。
         */
        virtual bool isHardwareDecoderInUse()
        {
            return (mFlags & DECFLAG_HW) != 0;
        }

        void setRequireDrmHandlerCallback(std::function<std::shared_ptr<DrmHandler>(const DrmInfo &drmInfo)> callback)
        {
            mRequireDrmHandlerCallback  = callback;
        }

    protected:
        std::string mName;
        int mFlags = 0; // VFLAG_HW,VFLAG_OUT
#if AF_HAVE_PTHREAD
        std::mutex mVideoMutex;
#endif
        std::vector<decoder_error_info> mErrorQueue;
        bool mInBackground = false;
        bool bNeedKeyFrame{true};
        int64_t keyPts = INT64_MIN;

        std::function<std::shared_ptr<DrmHandler>(const DrmInfo &drmInfo)> mRequireDrmHandlerCallback{nullptr};
    };
}


#endif //FRAMEWORK_VIDEO_DECODER_H
