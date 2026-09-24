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
    if (mPendingVideoDecoder.decoder) {
        mPendingVideoDecoder.decoder->flush();
        mPendingVideoDecoder.decoder->close();
    }
    if (mRetiredVideoDecoder.decoder) {
        mRetiredVideoDecoder.decoder->flush();
        mRetiredVideoDecoder.decoder->close();
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

int SMPAVDeviceManager::setUpPendingVideoDecoder(uint64_t decFlag, const Stream_meta *meta, void *device, uint32_t dstFormat)
{
    // 该函数只操作 pending slot，不会关闭或 flush 当前正在显示画面的 active
    // decoder，所以创建失败也不会影响旧清晰度继续播放。
    std::lock_guard<std::mutex> uMutex(mMutex);
    DecoderHandle &h = mPendingVideoDecoder;
    if (h.valid) return 0;

    DrmInfo drmInfo{};
    if (meta->keyFormat != nullptr) {
        drmInfo.format = meta->keyFormat;
        drmInfo.uri = meta->keyUrl == nullptr ? "" : meta->keyUrl;
    }
    if (meta->drmPssh != nullptr) drmInfo.pssh = meta->drmPssh;
    if (meta->drmKeyId != nullptr) drmInfo.keyId = meta->drmKeyId;

    if (h.decoder) {
        h.decoder->flush();
        h.decoder->close();
        h.decoder.reset();
    }
    h.meta = *meta;
    h.decFlag = decFlag;
    h.device = device;
    h.mDrmInfo = drmInfo;
    h.mDstFormat = dstFormat;
    h.decoder = decoderFactory::create(*meta, decFlag, std::max(meta->height, meta->width), drmInfo.empty() ? nullptr : &drmInfo);
    if (!h.decoder) return gen_framework_errno(error_class_codec, codec_error_video_not_support);
    h.decoder->setRequireDrmHandlerCallback(
        [this](const DrmInfo &info) -> std::shared_ptr<DrmHandler> { return move(mDrmManager->require(info)); });
    int ret = h.decoder->open(meta, device, decFlag, drmInfo.empty() ? nullptr : &drmInfo);
    if (ret < 0) {
        h.decoder.reset();
        return gen_framework_errno(error_class_codec, codec_error_video_device_error);
    }
    h.valid = true;
    return 0;
}

int SMPAVDeviceManager::getPendingVideoFrame(std::unique_ptr<IAFFrame> &frame, uint64_t timeOut)
{
    // 解码器管理器统一加锁，避免后台解码线程和切换提交线程同时操作 decoder。
    std::lock_guard<std::mutex> uMutex(mMutex);
    if (!mPendingVideoDecoder.valid || !mPendingVideoDecoder.decoder) return -EINVAL;
    return mPendingVideoDecoder.decoder->getFrame(frame, timeOut);
}

int SMPAVDeviceManager::sendPendingVideoPacket(std::unique_ptr<IAFPacket> &packet, uint64_t timeOut)
{
    std::lock_guard<std::mutex> uMutex(mMutex);
    if (!mPendingVideoDecoder.valid || !mPendingVideoDecoder.decoder) return -EINVAL;
    return mPendingVideoDecoder.decoder->send_packet(packet, timeOut);
}

void SMPAVDeviceManager::invalidatePendingVideoDecoder()
{
    std::lock_guard<std::mutex> uMutex(mMutex);
    mPendingVideoDecoder.valid = false;
}

void SMPAVDeviceManager::discardPendingVideoDecoder()
{
    std::lock_guard<std::mutex> uMutex(mMutex);
    if (mPendingVideoDecoder.decoder) {
        mPendingVideoDecoder.decoder->flush();
        mPendingVideoDecoder.decoder->close();
    }
    mPendingVideoDecoder = DecoderHandle{};
}

void SMPAVDeviceManager::promotePendingVideoDecoder()
{
    // 调用方已经确认 pending 有首帧。旧 decoder 不能在这里 close：Qt 场景图/D3D11
    // 可能仍持有旧 surface 的输入视图。先把旧 decoder 移到 retired，等新帧真正
    // 送入渲染器后再释放，避免切高清时出现左上角残片、黑帧和 GPU 卡死。
    std::lock_guard<std::mutex> uMutex(mMutex);
    if (!mPendingVideoDecoder.valid || !mPendingVideoDecoder.decoder) return;
    if (mRetiredVideoDecoder.decoder) {
        mRetiredVideoDecoder.decoder->flush();
        mRetiredVideoDecoder.decoder->close();
        mRetiredVideoDecoder = DecoderHandle{};
    }
    mRetiredVideoDecoder = std::move(mVideoDecoder);
    std::swap(mVideoDecoder, mPendingVideoDecoder);
    mPendingVideoDecoder = DecoderHandle{};
}

void SMPAVDeviceManager::releaseRetiredVideoDecoder()
{
    std::lock_guard<std::mutex> uMutex(mMutex);
    if (!mRetiredVideoDecoder.decoder) return;
    mRetiredVideoDecoder.decoder->flush();
    mRetiredVideoDecoder.decoder->close();
    mRetiredVideoDecoder = DecoderHandle{};
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
    if (mAudioRender == nullptr) {
        mAudioRender = AudioRenderFactory::create();
    }

    assert(mAudioRender);
    int audioInitRet = mAudioRender->init(&info);

    if (audioInitRet < 0) {
        AF_LOGE("AudioOutHandle Init Error is %d", audioInitRet);
        return -1;
    } else {
        mAudioRenderInfo = info;
        mAudioRenderValid = true;
        return 0;
    }
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
