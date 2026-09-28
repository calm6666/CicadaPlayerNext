//
// Created by pingkai on 2020/11/2.
//

#define LOG_TAG "SMPAVDeviceManager"
#include "SMPAVDeviceManager.h"
#include <algorithm>
#include <cassert>
#include <codec/decoderFactory.h>
#include <render/renderFactory.h>
#include <utils/errors/framework_error.h>
#include <utils/frame_work_log.h>
#ifdef __APPLE__
#include <codec/Apple/AppleVideoToolBox.h>
#endif

using namespace Cicada;
using namespace std;
SMPAVDeviceManager::SMPAVDeviceManager()
{
    mDrmManager = static_cast<std::unique_ptr<DrmManager>>(new DrmManager());
}

SMPAVDeviceManager::~SMPAVDeviceManager()
{
    if (mAudioDecoder.decoder) {
        mAudioDecoder.decoder->close();
    }
    if (mVideoDecoder.decoder) {
        flushVideoRender();
        mVideoRender = nullptr;
        mVideoDecoder.decoder->flush();
        mVideoDecoder.decoder->close();
    }
}
int SMPAVDeviceManager::setUpDecoder(uint64_t decFlag, const Stream_meta *meta, void *device, deviceType type, uint32_t dstFormat)
{
    std::lock_guard<std::mutex> uMutex(mMutex);
    DecoderHandle *decoderHandle = getDecoderHandle(type);
    if (decoderHandle == nullptr) {
        return -EINVAL;
    }
    if (decoderHandle->valid) {
        return 0;
    }

    mDrmManager->clearErrorItems();

    DrmInfo drmInfo{};
    if (meta->keyFormat != nullptr) {
        drmInfo.format = meta->keyFormat;
        drmInfo.uri = meta->keyUrl == nullptr ? "" : meta->keyUrl;
    }
    if (meta->drmPssh != nullptr) {
        drmInfo.pssh = meta->drmPssh;
    }
    if (meta->drmKeyId != nullptr) {
        drmInfo.keyId = meta->drmKeyId;
    }

    if (decoderHandle->decoder) {
        if (decoderHandle->match(meta, decFlag, device, dstFormat, drmInfo) && decoderHandle->decoder->supportReuse()) {// reuse decoder

            AF_LOGI("reuse decoder %s\n", type == DEVICE_TYPE_VIDEO ? "video" : "audio ");
            decoderHandle->valid = true;
            decoderHandle->meta = *meta;
            decoderHandle->mDstFormat = dstFormat;
            decoderHandle->mDrmInfo = drmInfo;
            flushVideoRender();
            decoderHandle->decoder->flush();
            decoderHandle->decoder->updateMetaData(meta);
            decoderHandle->decoder->pause(false);
            return 0;
        }
        /*
         *  must flush decoder before close on android mediacodec decoder
         */
        if (meta->type == STREAM_TYPE_VIDEO && decoderHandle->decoder->getName() == "VD.mediaCodec") {
            flushVideoRender();
        }
        decoderHandle->decoder->flush();
        decoderHandle->decoder->close();
    }

    decoderHandle->meta = *meta;
    decoderHandle->decFlag = decFlag;
    decoderHandle->device = device;
    decoderHandle->decoder =
            decoderFactory::create(*meta, decFlag, std::max(meta->height, meta->width), drmInfo.empty() ? nullptr : &drmInfo);
    decoderHandle->mDrmInfo = drmInfo;
    decoderHandle->mDstFormat = dstFormat;

    if (decoderHandle->decoder == nullptr) {
        return gen_framework_errno(error_class_codec, codec_error_video_not_support);
    }
    decoderHandle->decoder->setRequireDrmHandlerCallback(
            [this](const DrmInfo &info) -> std::shared_ptr<DrmHandler> { return move(mDrmManager->require(info)); });
    int ret;
    if (dstFormat) {
#ifdef __APPLE__
        auto *vtbDecoder = dynamic_cast<AFVTBDecoder *>(decoderHandle->decoder.get());
        if (vtbDecoder) {
            ret = vtbDecoder->setPixelBufferFormat(dstFormat);
            if (ret < 0) {
                AF_LOGW("setPixelBufferFormat error\n");
            }
        }
#endif
    }
    ret = decoderHandle->decoder->open(meta, device, decFlag, drmInfo.empty() ? nullptr : &drmInfo);
    if (ret < 0) {
        AF_LOGE("config decoder error ret= %d \n", ret);
        decoderHandle->decoder = nullptr;
        return gen_framework_errno(error_class_codec, codec_error_video_device_error);
    }
    decoderHandle->valid = true;
    return 0;
}








SMPAVDeviceManager::DecoderHandle *SMPAVDeviceManager::getDecoderHandle(const SMPAVDeviceManager::deviceType &type)
{
    if (type == DEVICE_TYPE_VIDEO) {
        return &mVideoDecoder;
    } else if (type == DEVICE_TYPE_AUDIO) {
        return &mAudioDecoder;
    }
    return nullptr;
}

void SMPAVDeviceManager::invalidDevices(uint64_t deviceTypes)
{
    std::lock_guard<std::mutex> uMutex(mMutex);
    if (deviceTypes & DEVICE_TYPE_AUDIO) {
        if (mAudioDecoder.decoder) {
            mAudioDecoder.decoder->prePause();
        }
        if (mAudioRender) {
            mAudioRender->prePause();
            mAudioRender->mute(true);
        }
        mAudioDecoder.valid = false;
        mAudioRenderValid = false;
    }
    if (deviceTypes & DEVICE_TYPE_VIDEO) {
        if (mVideoDecoder.decoder) {
            mVideoDecoder.decoder->prePause();
        }
        //        if (mVideoRender) {
        //            mVideoRender->invalid(true);
        //        }
        mVideoDecoder.valid = false;
        mVideoRenderValid = false;
    }
}
void SMPAVDeviceManager::invalidateDecoder(uint64_t deviceTypes)
{
    std::lock_guard<std::mutex> uMutex(mMutex);
    if (deviceTypes & DEVICE_TYPE_AUDIO) {
        mAudioDecoder.valid = false;
    }
    if (deviceTypes & DEVICE_TYPE_VIDEO) {
        mVideoDecoder.valid = false;
    }
}
void SMPAVDeviceManager::flushDevice(uint64_t deviceTypes)
{
    /*
     *  flush devices only on valid, otherwise the devices will be flushed on reusing
     */
    if (deviceTypes & DEVICE_TYPE_AUDIO) {
        if (mAudioDecoder.valid) {
            assert(mAudioDecoder.decoder != nullptr);
            mAudioDecoder.decoder->flush();
        }
        if (mAudioRenderValid
        /*
                  flush the Audio render on APPLE platform, otherwise it will output nise on reuse, but I don't know the reason.
                 */
#if __APPLE__
            || mAudioRender
#endif
        ) {
            mAudioRender->flush();
        }
    }
    if (deviceTypes & DEVICE_TYPE_VIDEO) {
        if (mVideoDecoder.valid) {
            mVideoDecoder.decoder->flush();
        }

        flushVideoRender();
    }
}
int SMPAVDeviceManager::getFrame(std::unique_ptr<IAFFrame> &frame, deviceType type, uint64_t timeOut)
{
    DecoderHandle *decoderHandle = getDecoderHandle(type);
    if (decoderHandle == nullptr || !decoderHandle->valid) {
        return -EINVAL;
    }
    assert(decoderHandle->decoder);
    return decoderHandle->decoder->getFrame(frame, timeOut);
}
int SMPAVDeviceManager::sendPacket(std::unique_ptr<IAFPacket> &packet, deviceType type, uint64_t timeOut)
{
    DecoderHandle *decoderHandle = getDecoderHandle(type);
    if (decoderHandle == nullptr || !decoderHandle->valid) {
        return -EINVAL;
    }

    assert(decoderHandle->decoder);
    int ret = decoderHandle->decoder->send_packet(packet, timeOut);

    return ret;
}
int SMPAVDeviceManager::setVolume(float volume)
{
    if (mAudioRender) {
        return mAudioRender->setVolume(volume);
    }
    // TODO: save the value
    return 0;
}
uint64_t SMPAVDeviceManager::getAudioRenderQueDuration()
{
    if (mAudioRender) {
        return mAudioRender->getQueDuration();
    }
    return 0;
}
int SMPAVDeviceManager::renderAudioFrame(std::unique_ptr<IAFFrame> &frame, int timeOut)
{
    if (mAudioRender) {
        int ret = mAudioRender->renderFrame(frame, timeOut);
        if (ret == IAudioRender::FORMAT_NOT_SUPPORT) {
            if (mAudioRender->getQueDuration() == 0) {
                mAudioRender = nullptr;
                mAudioRenderValid = false;
                return ret;
            } else {
                return -EAGAIN;
            }
        }
        return ret;
    }
    return -EINVAL;
}
void SMPAVDeviceManager::pauseAudioRender(bool pause)
{
    if (mAudioRender) {
        mAudioRender->pause(pause);
    }
    // TODO: save the status
}
int SMPAVDeviceManager::setUpAudioRender(const IAFFrame::audioInfo &info)
{
    std::lock_guard<std::mutex> uMutex(mMutex);

    if (mAudioRenderValid) {
        assert(mAudioRender != nullptr);
        return 0;
    }

    if (mAudioRender) {
        mAudioRender->flush();
        mAudioRender->mute(mMute);
        mAudioRender->pause(false);
        mAudioRenderValid = true;
        return 0;
    }

    /*
     * ============ 【init 失败就换一个实现再试一次（最多 1 次重试）】============
     *
     * 背景：Android 上有两套音频输出实现（AaudioRender 优先、AudioTrackRender 兜底），
     * 由 audioRenderPrototype::create() 按"设备能力"选出第一个（见 framework/render/audio/
     * Android/AudioTrackRender.h 里 is_supported() 的说明）。但**能力判定只能在 create() 时
     * 用"平台库/符号/系统版本"完成，真正的设备操作要等到 init(info)**（那时才有采样率/声道）。
     * 于是存在一个窄缝：判定说"AAudio 可用"，可真到建流时设备/ROM 打不开。
     *
     * 那一次 AaudioRender 会把失败记成一个进程级 sticky 标记
     * （framework/render/audio/Android/AaudioRender.cpp 的 s_streamOpenFailed 与 isAvailable()），
     * 于是**下一次 create() 必然落到 AudioTrack**。本函数就是那个"下一次"：失败后销毁实例、
     * 重新 create、再 init 一次 —— 这样"存在但打不开"的设备也能当场回退，而不是让整次播放
     * 直接报 MEDIA_PLAYER_ERROR_RENDER_AUDIO_OPEN_DEVICE_FAILED。
     *
     * 三条工程约束都满足：
     *   · **状态判据**：只看"这一次 init 成不成功"，失败方已经把自己标成不可用；
     *   · **没有计时器/看门狗**：重试次数是编译期常量 2（= 初始 + 1 次重试），不是时间预算；
     *   · **必须换实例**：filterAudioRender::init() 对同一个实例只允许一次
     *     （mInputInfo.sample_rate != 0 就返回 -EINVAL），所以换实现必须换对象。
     */
    int audioInitRet = -1;

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (mAudioRender == nullptr) {
            mAudioRender = AudioRenderFactory::create();
        }

        if (mAudioRender == nullptr) {
            AF_LOGE("no audio render implementation is available (attempt %d/2)", attempt + 1);
            return -1;
        }

        audioInitRet = mAudioRender->init(&info);

        if (audioInitRet >= 0) {
            if (attempt > 0) {
                AF_LOGW("audio output fell back to another render implementation after the first one could not "
                        "open its device (succeeded on attempt %d/2) — playback continues on the fallback\n",
                        attempt + 1);
            }

            mAudioRenderInfo = info;
            mAudioRenderValid = true;
            /*
             * 新建出来的实例不知道播放器当前的静音状态（filterAudioRender::mute 只写它自己的成员）。
             * 复用旧实例的那条分支一直是显式 `mute(mMute)` 的，这里补上同一步，否则
             * "先在静音状态下起播"或"回退到另一个实现"之后会突然出声。
             * 只调一个既有接口，不改任何默认值。
             */
            mAudioRender->mute(mMute);
            return 0;
        }

        AF_LOGE("audio render init failed (attempt %d/2, ret=%d) — destroying that instance and trying the next "
                "implementation (the failed one marks itself unavailable, so create() will not pick it again)\n",
                attempt + 1, audioInitRet);

        /* 销毁失败的实例：析构会停掉它自己的渲染线程并释放设备资源
         * （AaudioRender 的析构会关掉已打开的流）。留着它没有意义 ——
         * 它的 init 已经被调用过，同一个对象再也 init 不起来。 */
        mAudioRender = nullptr;
        mAudioRenderValid = false;
    }

    AF_LOGE("AudioOutHandle Init Error is %d", audioInitRet);
    return -1;
}
int SMPAVDeviceManager::setSpeed(float speed)
{
    if (mAudioRender) {
        mAudioRender->setSpeed(speed);
    }
    if (mVideoRender) {
        mVideoRender->setSpeed(speed);
    }
    // TODO: save
    return 0;
}
int64_t SMPAVDeviceManager::getAudioRenderPosition()
{
    if (mAudioRender) {
        return mAudioRender->getPosition();
    }
    return INT64_MIN;
}
void SMPAVDeviceManager::setAudioRenderListener(IAudioRenderListener *listener)
{
    if (mAudioRender) {
        mAudioRender->setListener(listener);
    }
}
void SMPAVDeviceManager::setVideoRenderListener(IVideoRender::IVideoRenderListener *listener)
{
    if (mVideoRender) {
        mVideoRender->setListener(listener);
    }
}
void SMPAVDeviceManager::setMute(bool mute)
{
    if (mAudioRender) {
        mAudioRender->mute(mute);
    }
    mMute = mute;
}
void SMPAVDeviceManager::setAudioRenderingCb(renderingFrameCB cb, void *userData)
{
    if (mAudioRender) {
        mAudioRender->setRenderingCb(cb, userData);
    }
}
void SMPAVDeviceManager::setVideoRenderingCb(videoRenderingFrameCB cb, void *userData)
{
    if (mVideoRender) {
        mVideoRender->setVideoRenderingCb(cb, userData);
    }
}
uint64_t SMPAVDeviceManager::getVideoDecoderFlags()
{
    if (mVideoDecoder.decoder) {
        return static_cast<uint64_t>(mVideoDecoder.decoder->getFlags());
    }
    return 0;
}
int SMPAVDeviceManager::createVideoRender(uint64_t flags)
{
    if (mVideoRenderValid && mVideoRenderFlags == flags) {
        return 0;
    }
    if (mVideoRender  && mVideoRenderFlags == flags) {
        flushVideoRender();
        mVideoRender->invalid(true);
        mVideoRenderValid = true;
        return 0;
    }

    if (mVideoRender) {
        mVideoRender->clearScreen();
    }

    mVideoRender = videoRenderFactory::create(flags);
//    assert(mVideoRender != nullptr);
    if (mVideoRender){
        mVideoRenderValid = true;
        mVideoRenderFlags = flags;
    }
    return 0;
}
void SMPAVDeviceManager::flushVideoRender()
{
    if (mVideoRender) {
        unique_ptr<IAFFrame> frame{nullptr};
        mVideoRender->renderFrame(frame);
    }
}
int SMPAVDeviceManager::renderVideoFrame(unique_ptr<IAFFrame> &frame)
{
    if (mVideoRender) {
        int ret = mVideoRender->renderFrame(frame);
        mVideoRender->invalid(false);
        return ret;
    }
    return -EINVAL;
}
void SMPAVDeviceManager::destroyVideoRender()
{
    mVideoRender = nullptr;
    mVideoRenderValid = false;
}

void SMPAVDeviceManager::setDrmRequestCallback(const function<DrmResponseData *(const DrmRequestParam &)> &drmCallback)
{
    mDrmManager->setDrmCallback(drmCallback);
}
