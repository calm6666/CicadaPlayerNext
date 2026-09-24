//
// Created by yuyuan on 2020/12/03.
//

#define LOG_TAG "SdlAFAudioRender2"

#include "SdlAFAudioRender2.h"
#include <assert.h>
#include <base/media/AVAFPacket.h>
#include <utils/ffmpeg_utils.h>
#include <utils/frame_work_log.h>

using namespace Cicada;

SdlAFAudioRender2::SdlAFAudioRender2()
{}

SdlAFAudioRender2::~SdlAFAudioRender2()
{
    if (mDevID > 0) {
        SDL_CloseAudioDevice(mDevID);
        mDevID = 0;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);

    if (mPcmBuffer) {
        free(mPcmBuffer);
    }
}

bool SdlAFAudioRender2::device_require_format(const IAFFrame::audioInfo &info)
{
    int format = info.format;

    /*
     * S32 / S32P are accepted here too.
     *
     * The list used to stop at 16 bit and float, and anything else - most
     * visibly the S32 that a 24 bit FLAC decodes to - fell through, leaving
     * inputSpec.format at 0 in init_device(). SDL then opened the device with
     * that bogus format while device_write() queued real 32 bit samples into it,
     * and the result was loud white noise ("sand") plus an audio clock that
     * never settled, which in turn made the video drop frames continuously:
     *   W/AlivcPlayerClock: TIMEPOS reSync time 8517 to -96000
     *   W/AlivcPlayerClock: TIMEPOS reSync time 111008 to 0
     *   KPI test total fps:27.9   5 dropped of 29 video frames
     *
     * The rest of the path is already format agnostic - getPCMDataLen() sizes
     * the buffer from av_get_bytes_per_sample() and copyPCMData() interleaves
     * planar input - so only the mapping below was missing.
     */
    if (format == AF_SAMPLE_FMT_S16 || format == AF_SAMPLE_FMT_S16P
            || format == AF_SAMPLE_FMT_S32 || format == AF_SAMPLE_FMT_S32P
            || format == AF_SAMPLE_FMT_FLT || format == AF_SAMPLE_FMT_FLTP) {
        mInputInfo = info;
        return true;
    }

    return false;
}

int SdlAFAudioRender2::init_device()
{
    needFilter = true;
    // init sdl audio subsystem
    if (!mSdlAudioInited) {
        int initRet = SDL_Init(SDL_INIT_AUDIO);
        if (initRet < 0) {
            AF_LOGE("SdlAFAudioRender could not initialize! Error: %s\n", SDL_GetError());
            return initRet;
        }
        mSdlAudioInited = true;
    }

    // init sdl audio device
    if (mDevID == 0) {
        SDL_AudioSpec inputSpec{0};
        int format = mInputInfo.format;
        if (format == AF_SAMPLE_FMT_S16 || format == AF_SAMPLE_FMT_S16P) {
            inputSpec.format = AUDIO_S16SYS;
        } else if (format == AF_SAMPLE_FMT_S32 || format == AF_SAMPLE_FMT_S32P) {
            /* 24 bit content decodes to S32; see device_require_format(). */
            inputSpec.format = AUDIO_S32SYS;
        } else if (format == AF_SAMPLE_FMT_FLT || format == AF_SAMPLE_FMT_FLTP) {
            inputSpec.format = AUDIO_F32SYS;
        }
        inputSpec.freq = mInputInfo.sample_rate;
        inputSpec.channels = mInputInfo.channels;
        inputSpec.silence = 0;
        inputSpec.samples = mInputInfo.nb_samples;
        inputSpec.userdata = this;
        inputSpec.callback = nullptr;
        mDevID = SDL_OpenAudioDevice(NULL, false, &inputSpec, &mSpec, 0);
        if (mDevID == 0) {
            AF_LOGE("SdlAFAudioRender could not openAudio! Error: %s\n", SDL_GetError());
            return OPEN_AUDIO_DEVICE_FAILED;
        }
        mOutputInfo.channels = mSpec.channels;
        mOutputInfo.nb_samples = mSpec.samples;
        mOutputInfo.sample_rate = mSpec.freq;
    }

    return 0;
}

int SdlAFAudioRender2::pause_device()
{
    if (mDevID != 0) {
        SDL_PauseAudioDevice(mDevID, 1);
    }
    return 0;
}

int SdlAFAudioRender2::start_device()
{
    if (mDevID != 0) {
        SDL_PauseAudioDevice(mDevID, 0);
    }
    return 0;
}

void SdlAFAudioRender2::flush_device()
{
    if (mRunning) {
        pause_device();
    }
    if (mDevID != 0) {
        SDL_ClearQueuedAudio(mDevID);
    }
    mPlayedDuration = 0;
    if (mRunning) {
        start_device();
    }
}

void SdlAFAudioRender2::device_setVolume(float gain)
{}

int64_t SdlAFAudioRender2::device_get_position()
{
    return mPlayedDuration - device_get_que_duration();
}

int SdlAFAudioRender2::device_write(unique_ptr<IAFFrame> &frame)
{
    if (device_get_que_duration() > 50 * 1000) {
        return -EAGAIN;
    }
    int pcmDataLength = getPCMDataLen(frame->getInfo().audio.channels, (enum AVSampleFormat) frame->getInfo().audio.format,
                                      frame->getInfo().audio.nb_samples);
    if (mPcmBufferSize < pcmDataLength) {
        mPcmBufferSize = pcmDataLength;
        mPcmBuffer = static_cast<uint8_t *>(realloc(mPcmBuffer, mPcmBufferSize));
    }
    bool rendered = false;
    if (mRenderingCb) {
        rendered = mRenderingCb(mRenderingCbUserData, frame.get());
    }
    if (!mMute && !rendered) {
        copyPCMData(getAVFrame(frame.get()), mPcmBuffer);
    } else {
        memset(mPcmBuffer, 0, pcmDataLength);
    }
    SDL_QueueAudio(mDevID, mPcmBuffer, pcmDataLength);

    assert(frame->getInfo().duration > 0);
    if (mListener) {
        mListener->onFrameInfoUpdate(frame->getInfo(), true);
    }
    mPlayedDuration += (uint64_t) frame->getInfo().audio.nb_samples * 1000000 / frame->getInfo().audio.sample_rate;
    //AF_LOGD("queued duration is %llu\n", getQueDuration());
    frame = nullptr;
    return 0;
}

void SdlAFAudioRender2::device_mute(bool bMute)
{
    mMute = bMute;
    // mute all queued audio buffer
    uint32_t queuedAudioSize = SDL_GetQueuedAudioSize(mDevID);
    uint8_t *muteAudioBuffer = (uint8_t *) malloc(queuedAudioSize);
    memset(muteAudioBuffer, 0, queuedAudioSize);
    SDL_ClearQueuedAudio(mDevID);
    SDL_QueueAudio(mDevID, muteAudioBuffer, queuedAudioSize);
    free(muteAudioBuffer);
}

uint64_t SdlAFAudioRender2::device_get_que_duration()
{
    int sampleSize = 4;
    if (mSpec.format == AUDIO_S16SYS) {
        sampleSize = 2;
    }
    double byteRate = mOutputInfo.channels * sampleSize * mOutputInfo.sample_rate;
    return (uint64_t)((SDL_GetQueuedAudioSize(mDevID) / byteRate) * 1000000);
}

int SdlAFAudioRender2::loopChecker()
{
    return 0;
}

void SdlAFAudioRender2::device_preClose()
{}

uint64_t SdlAFAudioRender2::device_get_ability()
{
    return 0;
}
