//
// Created by lifujun on 2019/8/29.
//
#define LOG_TAG "AudioTrackRender"
#include "AudioTrackRender.h"
#include <utils/frame_work_log.h>

#include <cassert>
#include <cerrno>
#include <utils/Android/AndroidJniHandle.h>
#include <utils/Android/JniEnv.h>
#include <utils/Android/JniException.h>
#include <utils/ffmpeg_utils.h>
#include <utils/globalSettings.h>
#include <utils/timer.h>


const int PLAYSTATE_PAUSED = 2;  // matches SL_PLAYSTATE_PAUSED
/** indicates AudioTrack state is playing */
const int PLAYSTATE_PLAYING = 3;  // matches SL_PLAYSTATE_PLAYING

#define MAX_FRAME_QUEUE_SIZE 16
#define MIN_FRAME_QUEUE_SIZE 2

/*
 * 【B 方案：静音保活】的资源上界。
 * 每次写 20ms 静音（sample_rate/50），上限 120 次 ≈ 2.4 秒：到达后停止填充并打一条日志，
 * 交回原有的 underrun 处理。**这是资源保护，不是"判死"** —— 它不推断任何状态是否死亡，
 * 也不做周期性动作；真实 PCM 一回来，计数与闩立刻复位，保活重新可用。
 */
#define AUDIO_KEEP_ALIVE_MAX_WRITES 120
/* 每次静音写入的时长（毫秒），与 write_loop 的 20ms 轮询节拍一致。 */
#define AUDIO_KEEP_ALIVE_WRITE_MS 20


using namespace Cicada;

AudioTrackRender AudioTrackRender::se(0);

AudioTrackRender::AudioTrackRender() : mFrameQueue(MAX_FRAME_QUEUE_SIZE)
{
}

AudioTrackRender::~AudioTrackRender()
{
    JniEnv  jniEnv;
    JNIEnv *handle = jniEnv.getEnv();

    mRunning = false;

    delete mWriteThread;

    while (!mFrameQueue.empty()) {
        delete mFrameQueue.front();
        mFrameQueue.pop();
    }

    if (handle) {
        if (audio_track && method_stop) {
            handle->CallVoidMethod(audio_track, method_stop);

            if (JniException::clearException(handle)) {
                AF_LOGE("AudioTrack stop exception. maybe IllegalStateException.");
            }
        }

        if (audio_track && method_release) {
            handle->CallVoidMethod(audio_track, method_release);
            handle->DeleteGlobalRef(audio_track);
        }

        if (jbuffer != nullptr) {
            handle->DeleteGlobalRef(jbuffer);
        }

        /* B 方案：静音保活用的零填充 JNI 全局引用同样要释放。 */
        if (jSilenceBuffer != nullptr) {
            handle->DeleteGlobalRef(jSilenceBuffer);
            jSilenceBuffer = nullptr;
            jSilenceBufferLen = 0;
        }
    }

    if (mPcmBuffer) {
        free(mPcmBuffer);
        mPcmBuffer = nullptr;
    }
}

bool AudioTrackRender::device_require_format(const IAFFrame::audioInfo &info)
{
    if (info.format != AF_SAMPLE_FMT_S16 || info.sample_rate > 48000 || info.channels > 2) {
        return false;
    }
    mRequireFormat = std::unique_ptr<IAFFrame::audioInfo>(new IAFFrame::audioInfo());
    (*mRequireFormat) = info;
    return true;
}

int AudioTrackRender::init_device()
{
    adjustOutputInfo();
    int ret = init_jni();

    if (ret < 0) {
        return ret;
    }

    mWriteThread = NEW_AF_THREAD(write_loop);
    if (mRunning) {
        mWriteThread->start();
    }
    return 0;
}

void AudioTrackRender::adjustOutputInfo()
{
    if (mRequireFormat) {
        mOutputInfo = *mRequireFormat;
        return;
    }
    if (mInputInfo.format != AF_SAMPLE_FMT_S16) {
        mOutputInfo.format = AF_SAMPLE_FMT_S16;
        needFilter = true;
    }

    if (mInputInfo.sample_rate > 48000) {
        mOutputInfo.sample_rate = 48000;
        needFilter = true;
    }

    if (mInputInfo.channels > 2) {
        mOutputInfo.channels = 2;
        needFilter = true;
    }
}


int AudioTrackRender::init_jni()
{
    const int CHANNEL_CONFIGURATION_MONO = 4;
    const int CHANNEL_CONFIGURATION_STEREO = 12;
    int channelType =
        mOutputInfo.channels == 2 ? CHANNEL_CONFIGURATION_STEREO : CHANNEL_CONFIGURATION_MONO;
    JniEnv  jniEnv;
    JNIEnv *handle = jniEnv.getEnv();

    if (!handle) {
        AF_LOGE("init jni error \n");
        return -ENOTSUP;
    }

    AndroidJniHandle<jclass> audio_track_cls(handle->FindClass("android/media/AudioTrack"));
    jmethodID min_buff_size_id = handle->GetStaticMethodID(
                                     audio_track_cls,
                                     "getMinBufferSize",
                                     "(III)I");
    jclass audio_track_impl;
    jmethodID constructor_id;
    audio_track_impl = audio_track_cls;
    AF_LOGI("choose internal audio track.");
    int buffer_size = handle->CallStaticIntMethod(audio_track_impl, min_buff_size_id,
                      mOutputInfo.sample_rate,
                      channelType, /*CHANNEL_CONFIGURATION_STEREO*/
                      2);          /*ENCODING_PCM_16BIT*/

    if (buffer_size < 0) {
        AF_LOGE("Init failed device not support. sampleRate %d", mOutputInfo.sample_rate);
        return -5;
    }

    constructor_id = handle->GetMethodID(audio_track_impl, "<init>", "(IIIIII)V");
    const string &streamType = Cicada::globalSettings::getSetting().getProperty("audio.streamType");
    int audioStreamType = streamType.empty() ? 3 : atoi(streamType.c_str());
    AndroidJniHandle<jobject> audio_track_tmp(handle->NewObject(audio_track_impl, constructor_id,
                                                                audioStreamType,         /*AudioManager.stream type*/
                                                                mOutputInfo.sample_rate, /*sampleRateInHz*/
                                                                channelType,             /*CHANNEL_CONFIGURATION_STEREO*/
                                                                2,                       /*ENCODING_PCM_16BIT*/
                                                                buffer_size,             /*bufferSizeInBytes*/
                                                                1                        /*AudioTrack.MODE_STREAM*/
                                                                ));

    if (JniException::clearException(handle)) {
        AF_LOGE("audioTrack constructor exception. sample_rate %d, channel %d, bufferSize %d",
                mOutputInfo.sample_rate, channelType, buffer_size);
        return -6;
    }

    audio_track = handle->NewGlobalRef(audio_track_tmp);
    // get method
    method_setVolume = handle->GetMethodID(audio_track_impl, "setStereoVolume", "(FF)I");
    method_play = handle->GetMethodID(audio_track_impl, "play", "()V");
    method_pause = handle->GetMethodID(audio_track_impl, "pause", "()V");
    method_flush = handle->GetMethodID(audio_track_impl, "flush", "()V");
    method_stop = handle->GetMethodID(audio_track_impl, "stop", "()V");
    method_getPlayState = handle->GetMethodID(audio_track_impl, "getPlayState", "()I");
    method_getPosition = handle->GetMethodID(audio_track_impl, "getPlaybackHeadPosition", "()I");
    method_write = handle->GetMethodID(audio_track_impl, "write", "([BII)I");
    method_release = handle->GetMethodID(audio_track_impl, "release", "()V");
    return 0;
}

int AudioTrackRender::pause_device()
{
    /*
     * 幂等：已经在 PAUSED 就不要再调一次 pause()。
     *
     * 为什么必须幂等（Android 专有）：AudioTrack 和 MediaPlayer 一样继承框架的
     * android.media.PlayerBase，内部是 android.util.StateMachine（日志里 tag=PlayerBase
     * 的 "[HSM] PlayerBase play()/pause()/stop()" 就是它），native 侧对应
     * frameworks/av 的 libaudioclient/TrackPlayerBase.cpp。每次 play()/pause() 都会驱动
     * 那个状态机，并在某些 ROM 上触发它的超时收尾（日志里的 baseTimeout() → stop()）。
     * seek 时我们原来每次都会 flush→start，缓冲又 pause，于是日志里成对出现
     * basePause()/baseStart()，也就是那套状态机被反复踢。幂等之后：状态没变就不碰它，
     * 既不产生无谓的状态机迁移，也不会让 AudioTrack 被框架超时 stop 掉。
     *
     * 【本轮修正】幂等只作用于**框架那次 pause() 调用**，不能把下面"停写线程"一起跳掉。
     * 写线程的启停是我们这一侧的状态，和 AudioTrack 的 play()/pause() 是两件事：
     * 原来在"框架已经是 PAUSED"时直接 return，于是"框架状态说不用 pause、但我们
     * 的写线程还在跑"这种情况被漏掉；对称地，start_device() 里的早退会漏掉写线程的
     * start。一旦出现"框架 PLAYING、写线程还停着"（由 device_preClose / 异常路径 /
     * 上一次半途而废的 pause 造成），就再没有路径把它拉起来 —— 用户听到的就是
     * "声音突然没了，过一会儿又自己恢复"。afThread::start()/pause() 自身都是幂等的
     * （framework/utils/afThread.cpp：pause 只看 RUNNING，start 只是置位并 notify），
     * 所以这里无条件调用不会带来多余动作。
     */
    bool frameworkAlreadyPaused = false;

    if (audio_track && method_getPlayState) {
        JniEnv  jniEnv;
        JNIEnv *handle = jniEnv.getEnv();

        if (handle != nullptr && handle->CallIntMethod(audio_track, method_getPlayState) == PLAYSTATE_PAUSED) {
            frameworkAlreadyPaused = true;
        }
    }

    if (mWriteThread && std::this_thread::get_id() == mWriteThread->getId()) {
        //same thread
    } else if (frameworkAlreadyPaused) {
        /*
         * 框架已经是 PAUSED：这里**不能**用阻塞式的 mWriteThread->pause() 去等安全点 ——
         * 写线程可能正阻塞在 AudioTrack.write() 上，而暂停的音轨不会消耗缓冲，
         * 那个 write() 可能很久不返回，等它就会连带卡住调用者（控制/渲染线程）。
         * 改用 prePause()：只置"下一轮自己停"的标志，不阻塞，也不会让线程空转
         * （afThread 会在下一次循环把 mThreadStatus 置为 PAUSED 并睡在条件变量上）。
         */
        mRunning = false;
        if (mWriteThread) {
            mWriteThread->prePause();
        }
        return 0;
    } else {
        mRunning = false;
        if (mWriteThread) {
            mWriteThread->pause();
        }
    }

    if (frameworkAlreadyPaused) {
        return 0;
    }

    if (audio_track && method_pause) {
        JniEnv  jniEnv;
        JNIEnv *handle = jniEnv.getEnv();
        handle->CallVoidMethod(audio_track, method_pause);

        if (JniException::clearException(handle)) {
            AF_LOGE("AudioTrack pause exception. maybe IllegalStateException.");
            return -1;
        }

        return 0;
    }

    return -1;
}


int AudioTrackRender::start_device()
{
    /*
     * 幂等：已经在 PLAYING 就不要再调一次 play()（理由见 pause_device 上面那段）。
     * seek/flush 现在只 flush 数据、不再无条件重踢 play()，日志里成对的
     * basePause()/baseStart() 与框架状态机的超时收尾都会随之消失。
     *
     * 【本轮修正】同 pause_device：幂等只管框架那次 play()；下面写线程的 start
     * **必须照常执行**，否则"框架 PLAYING、写线程停着"会变成永久静音。
     */
    bool frameworkAlreadyPlaying = false;

    if (audio_track && method_getPlayState) {
        JniEnv  jniEnv;
        JNIEnv *handle = jniEnv.getEnv();

        if (handle != nullptr && handle->CallIntMethod(audio_track, method_getPlayState) == PLAYSTATE_PLAYING) {
            frameworkAlreadyPlaying = true;
        }
    }

    if (!frameworkAlreadyPlaying && audio_track && method_play) {
        JniEnv  jniEnv;
        JNIEnv *handle = jniEnv.getEnv();
        handle->CallVoidMethod(audio_track, method_play);

        if (JniException::clearException(handle)) {
            AF_LOGE("AudioTrack start exception. maybe IllegalStateException.");
            return -1;
        }
    }

    if (mWriteThread && std::this_thread::get_id() == mWriteThread->getId()) {
        //same thread
    } else {
        mRunning = true;
        if (mWriteThread) {
            mWriteThread->start();
        }
    }

    return 0;
}


void AudioTrackRender::logSilence(int reason, const char *detail)
{
    /*
     * 静音窗口只在状态变化时各打一条（开始/结束），不是周期日志。
     * 编码与内核侧 AUDIO_SILENCE_* 一致：0 有声 / 2 设备写失败或短写 / 3 设备被 flush 重建。
     * 本函数不改变任何播放行为，只把"设备侧为什么没声音"写进日志。
     */
    if (reason == mSilenceReason) {
        return;
    }

    if (reason == 0) {
        AF_LOGW("audio silence ends (device side, was reason=%d): pcm accepted by the track again\n",
                mSilenceReason);
    } else {
        AF_LOGW("audio silence starts (device side, reason=%d): %s\n", reason,
                detail != nullptr ? detail : "");
    }

    mSilenceReason = reason;
}

void AudioTrackRender::flush_device_inner(bool clearFrameQueue)
{
    JniEnv  jniEnv;
    JNIEnv *handle = jniEnv.getEnv();
    int state = 0;

    if (audio_track && method_getPlayState) {
        state = handle->CallIntMethod(audio_track, method_getPlayState);
    }

    if (state == PLAYSTATE_PLAYING) {
        pause_device();
    }

    mOverFlowPlayedSimples +=  (getDevicePlayedSimples() - mAudioFlushPosition);

    if (audio_track && method_flush) {
        handle->CallVoidMethod(audio_track, method_flush);

        if (JniException::clearException(handle)) {
            AF_LOGE("AudioTrack flush exception. maybe IllegalStateException.");
        }
    }

    /*
     * 设备被清空：从这一刻起到下一帧 PCM 被接受之前，用户听到的是静音。
     * 记一条"静音开始（设备侧 reason=3）"；重新写成功时会自动记"结束"。
     */
    logSilence(3, "the audio device was flushed, waiting for pcm to refill");

    /*
     * ============ 保活回合随本次 flush 结束 ============
     *
     * 保活上界（AUDIO_KEEP_ALIVE_MAX_WRITES）的语义是"只约束**一轮**连续无数据"，
     * 那么一轮的结束条件就必须包含"设备被 flush"——此前只写了"真实 PCM 写成功"
     * 这一个结束条件，于是跨过 flush 的那次 seek 会**继承**上一轮已经用尽的上界。
     *
     * 真机日志（Android / DASH VOD 253.6s，EOS 之后用户 seek 回 20.127s）：
     *   20:14:58.051  audio keep-alive: writing silence                     ← 上一轮从 EOS 开始
     *   20:15:00.212  audio silence starts (reason=3)（seek 把设备 flush 了）
     *   20:15:00.410  audio keep-alive: reached the resource cap（计数仍是旧回合的 120）
     *   20:15:00.936  onAudioException -1003 → baseTimeout/baseStop
     *   20:15:01.336  restartIfDisabled ... due to previous underrun, restarting
     *   20:15:01.325  audio first frame after seek ... afterSeekMs=1183
     * 即：seek 期间设备本来就是空的、最需要保活，而 writeKeepAliveSilence() 因为
     * mKeepAliveCapped 直接返回 false ⇒ 音轨被框架判 underrun 并停掉，随后才被重启，
     * 这一停一启就是那 1.18 秒硬静音；同一段时间渲染器要等音频对齐，画面跟着冻住
     * 约 1 秒（日志同一时刻 KPI total fps:1.0），用户感受就是"seek 之后卡一下"。
     *
     * flush 是明确的状态事件：flush 之后设备里一个样本都不剩，接下来必然是
     * "从零开始等 PCM"的**新回合**。在这里复位 = "上界只在同一回合内累计"，
     * 既没有放宽上界（一轮仍然是 120 × 20ms），也没有引入任何计时器/墙钟判据。
     */
    if (mKeepAliveActive || mKeepAliveSilenceWrites > 0 || mKeepAliveCapped) {
        AF_LOGW("audio keep-alive: episode ends with the device flush after %d silence write(s)%s — "
                "the next starvation episode starts from zero (the cap is per episode, not permanent)\n",
                mKeepAliveSilenceWrites,
                mKeepAliveCapped ? " (the resource cap had been reached)" : "");
        mKeepAliveActive = false;
        mKeepAliveSilenceWrites = 0;
        mKeepAliveCapped = false;
    }

    mSendSimples = 0;

    if (clearFrameQueue) {
        while (!mFrameQueue.empty()) {
            delete mFrameQueue.front();
            mFrameQueue.pop();
        }
        mOverFlowPlayedSimples = 0;
    }

    // 复位到"缓冲下限"而不是最低值：每次 flush（含每次 seek）之后也要留足抗抖动余量。
    mMaxQueSize = 9;

    /* work around some device position didn't set to zero, and MUST get after start_device */
    start_device();

    mAudioFlushPosition = getDevicePlayedSimples();

    if (state == PLAYSTATE_PAUSED) {
        pause_device();
    }
}


void AudioTrackRender::flush_device()
{
    flush_device_inner(true);
}

void AudioTrackRender::device_setVolume(float gain)
{
    mVolume = gain;
    if (mMute) {
        return;
    }
    if (audio_track && method_setVolume) {
        JniEnv  jniEnv;
        JNIEnv *handle = jniEnv.getEnv();
        handle->CallIntMethod(audio_track, method_setVolume, gain, gain);
    }
}
void AudioTrackRender::device_mute(bool bMute)
{
    mMute = bMute;
    if (bMute) {
        if (audio_track && method_setVolume) {
            JniEnv jniEnv;
            JNIEnv *handle = jniEnv.getEnv();
            handle->CallIntMethod(audio_track, method_setVolume, 0.0f, 0.0f);
        }
    } else {
        device_setVolume(mVolume);
    }
}

int64_t AudioTrackRender::device_get_position()
{
    /*
     * 【② 已整体回退（2026-xx，用户真机反馈后）—— 这里保持"设备停时返回 0"的老行为】
     *
     * 曾经在这里加过"非 PLAYING/PAUSED 就返回 INT64_MIN 哨兵"（回退点 B1），
     * 让 getAudioPlayTimeStamp() 在设备不产数据时返回 INT64_MIN、主时钟退回**本地系统时钟**。
     * 真机结果是"第一次 seek 画面不动、半天才动"：seek 窗口内 AudioTrack 本来就会经过
     * PAUSED → FLUSHED/STOPPED → PLAYING（`pause_device()` → `AudioTrack.flush()` →
     * `start_device()`，见 getDevicePlayedSimples 里读 playState 的那段与真机 AudioTrack 日志），
     * 框架 baseStop 也会让 STOPPED 持续一段；期间参考值被判"不可用" ⇒ 时钟按墙上时间自走，
     * 音频回来后 SystemReferClock::GetTime()（system_refer_clock.cpp:44-45）**双向** reSync
     * 又把它拉回音频位置（可能向后跳）⇒ 视频被判"未来"继续干等 = 双重卡顿。
     * 结论：这条兜底的失败模式比它要治的"参考值冻结"更糟（冻结只是等，不会来回跳），
     * 因此**整体回退**。真机需要的是"修设备停本身"（保活/不无谓 flush），不是改时钟参考。
     * 若将来重做：必须先把"seek 窗口内/瞬时状态"排除掉，并给出真机差分日志。
     */
    uint64_t playedSimples = mOverFlowPlayedSimples + getDevicePlayedSimples() - mAudioFlushPosition;
    int64_t position = static_cast<int64_t>((playedSimples) / (float(mOutputInfo.sample_rate) / 1000000));
    return position;
}

uint64_t AudioTrackRender::getDevicePlayedSimples()
{

    uint64_t simples = 0;
    JniEnv  jniEnv;
    JNIEnv *handle = jniEnv.getEnv();

    if (handle) {
        int state = 0;

        if (audio_track && method_getPlayState) {
            state = handle->CallIntMethod(audio_track, method_getPlayState);
        }

        if (state == PLAYSTATE_PLAYING || state == PLAYSTATE_PAUSED) {
            if (audio_track && method_getPosition) {
                simples = static_cast<uint64_t>(handle->CallIntMethod(audio_track,
                                                method_getPosition));
            } else {
                return static_cast<uint64_t>(mSendSimples);
            }
        }
    }

    return simples;
}


int AudioTrackRender::device_write(unique_ptr<IAFFrame> &frame)
{
    //    AF_LOGD("xxxxxx mFrameQueue.size() is %d\n", mFrameQueue.size());
    if (mFrameQueue.size() >= mMaxQueSize) {
        //    mMaxQueSize = std::max(MIN_FRAME_QUEUE_SIZE,mMaxQueSize -1);
        return -EAGAIN;
    }
    if (frame == nullptr) {
        return 0;
    }
    mFrameQueue.push(frame.release());
    return 0;
}
bool AudioTrackRender::writeKeepAliveSilence()
{
    /*
     * 无 PCM 可写时写一段**与本 AudioTrack 完全同格式**的静音，把音轨喂住。
     *
     * 状态前提（全部是状态判断，没有任何计时器）：
     *   a) 写线程在跑（mRunning）—— 暂停、缓冲、停止、换源都会走 pause_device()/
     *      device_preClose() 把写线程 park 掉，所以"这些状态下不填充"是自动成立的；
     *   b) AudioTrack 处于 PLAYING —— 只有"本应在播、只是暂时没数据"才保活；
     *   c) getPosition 可用 —— 记账补偿依赖**真实设备位置**（见下面等式），拿不到真实
     *      位置时宁可不填充，回退到原有空转（否则补偿方向会错，位置会倒退）；
     *   d) 未到资源上界（mKeepAliveCapped）。
     */
    if (!mRunning || mKeepAliveCapped || audio_track == nullptr || method_getPlayState == nullptr ||
        method_getPosition == nullptr || method_write == nullptr) {
        return false;
    }

    JniEnv jniEnv;
    JNIEnv *handle = jniEnv.getEnv();

    if (handle == nullptr) {
        return false;
    }

    if (handle->CallIntMethod(audio_track, method_getPlayState) != PLAYSTATE_PLAYING) {
        return false;
    }

    /*
     * 格式：与当前设备完全一致（输出固定 S16）。声道/采样率取当前已建立的输出参数
     * （device_get_position()/getDeviceQuequedDuration() 用的是同一个 mOutputInfo），
     * 不临时构造格式，避免 format 变化导致重配或爆音。
     */
    const int channels = (mOutputInfo.channels > 0) ? mOutputInfo.channels : 2;
    const int sampleRate = (mOutputInfo.sample_rate > 0) ? mOutputInfo.sample_rate : 48000;
    const int samplesPerWrite = sampleRate * AUDIO_KEEP_ALIVE_WRITE_MS / 1000;
    const int bytesPerSample = channels * 2;                 // 输出固定 S16
    const int len = samplesPerWrite * bytesPerSample;        // 20ms 的静音字节数

    if (len <= 0 || bytesPerSample <= 0) {
        return false;
    }

    /*
     * 静音缓冲区：懒创建的 JNI 全局引用。NewByteArray 的内容由 Java 保证全 0，
     * 而我们从不往它里面写数据，所以它可以一直被复用。
     */
    if (jSilenceBuffer == nullptr || jSilenceBufferLen < len) {
        if (jSilenceBuffer != nullptr) {
            handle->DeleteGlobalRef(jSilenceBuffer);
            jSilenceBuffer = nullptr;
        }

        jbyteArray obj = handle->NewByteArray(len);

        if (obj == nullptr) {
            return false;
        }

        jSilenceBuffer = handle->NewGlobalRef(obj);
        handle->DeleteLocalRef(obj);
        jSilenceBufferLen = len;

        if (jSilenceBuffer == nullptr) {
            return false;
        }
    }

    jint written = handle->CallIntMethod(audio_track, method_write, jSilenceBuffer, 0, len);

    if (JniException::clearException(handle)) {
        AF_LOGE("AudioTrack keep-alive write exception. maybe IllegalStateException.");
        return false;
    }

    if (written <= 0) {
        /* 这一次写不进去（例如非阻塞/已停）：交回原有 underrun 处理，不做任何记账。 */
        return false;
    }

    /*
     * ============ 记账自洽的等式证明 ============
     *
     * 定义（改动前就存在的三处）：
     *   device_get_position() 的样本数
     *       playedSimples = mOverFlowPlayedSimples + getDevicePlayedSimples() - mAudioFlushPosition
     *   getDeviceQuequedDuration() / device_get_que_duration() 用的是
     *       (mSendSimples - playedSimples) / (sample_rate / 1e6)
     * 其中 getDevicePlayedSimples() 是 AudioTrack.getPosition()（真实设备已播样本数）。
     *
     * 写 S 个静音样本后：设备位置前进 S，即 playedDevice' = playedDevice + S。
     * 我们**不**动 mSendSimples（它表示媒体样本），而是把 S 等量加进 mAudioFlushPosition：
     *   playedSimples' = over + (playedDevice + S) - (flush + S)
     *                  = over + playedDevice - flush
     *                  = playedSimples                       ← 与填充前完全相等
     * 于是：
     *   · device_get_position() 完全不变 ⇒ **音频时钟不前进**，A/V 不漂移；
     *   · (mSendSimples - playedSimples) 完全不变 ⇒ 两个"已排队时长"函数也不变，
     *     不会出现"静音被算成媒体时间"的情况；
     *   · 真实 PCM 恢复写入时，mSendSimples 正常前进，而 mAudioFlushPosition 里那 S 个
     *     样本的补偿仍然有效（它抵消的是**已经播掉的静音**），不会重复计入也不会计漏。
     *
     * 注意用 written（实际写入字节）而不是 len 来换算：短写时补偿必须与实际播出量严格相等，
     * 这一点与 device_write_internal() 里"短写按实际字节记账"的既有做法一致。
     */
    const uint64_t silenceSamples = static_cast<uint64_t>(written / bytesPerSample);
    mAudioFlushPosition += silenceSamples;

    if (!mKeepAliveActive) {
        mKeepAliveActive = true;
        AF_LOGW("audio keep-alive: writing silence (no pcm available, %d ms per write, %d samples, "
                "device=%d Hz/%d ch) so the track is not stopped by the framework buffer timeout\n",
                AUDIO_KEEP_ALIVE_WRITE_MS, samplesPerWrite, sampleRate, channels);
    }

    mKeepAliveSilenceWrites++;

    if (mKeepAliveSilenceWrites >= AUDIO_KEEP_ALIVE_MAX_WRITES) {
        mKeepAliveCapped = true;
        AF_LOGW("audio keep-alive: reached the resource cap (%d silence writes ≈ %d ms) — stop filling and "
                "hand back to the normal underrun handling (resource bound, not a liveness judgement)\n",
                mKeepAliveSilenceWrites, AUDIO_KEEP_ALIVE_MAX_WRITES * AUDIO_KEEP_ALIVE_WRITE_MS);
    }

    return true;
}

int AudioTrackRender::write_loop()
{
    if (mFrameQueue.empty()) {
        /*
         * 无帧可写。原有策略保留：逐步抬高队列下限，并按 20ms 空转（帧到达由 device_write
         * 入队，AudioTrack 内部是阻塞写，所以保活写入会自然被节流到实时速率）。
         *
         * 【B 方案】在空转之前先尝试静音保活：否则 AudioTrack 约 1 秒没被写入就判 underrun
         * （onAudioException -1003 → baseTimeout/baseStop → 1.45~1.6 秒硬静音）。
         * writeKeepAliveSilence() 自己校验状态前提与资源上界，不满足就返回 false，
         * 这里完全退回改动前的行为。
         */
        mMaxQueSize = std::min(mMaxQueSize + 1, MAX_FRAME_QUEUE_SIZE);

        if (writeKeepAliveSilence()) {
            return 0;
        }

        // 无帧可写：5ms→20ms 兜底轮询（帧到达由 device_write 入队，AudioTrack
        // 内部阻塞写），空转开销降为 1/4
        af_msleep(20);
        return 0;
    }
    while (!mFrameQueue.empty() && mRunning) {
        int ret = device_write_internal(mFrameQueue.front());
        if (ret == -EAGAIN) {
            af_msleep(20);
            //            break;
        } else {
            if (mFrameQueue.size() >= mMaxQueSize) {
                // TODO: How to decrease mMaxQueSize size, if use nonblock write, we can do this
                //     mMaxQueSize = std::max(MIN_FRAME_QUEUE_SIZE,mMaxQueSize -1);
            }
            if (mListener) {
                mListener->onFrameInfoUpdate(mFrameQueue.front()->getInfo(), true);
            }
            delete mFrameQueue.front();
            mFrameQueue.pop();
        }
    }
    return 0;
}

#define OVERFLOW_SAMPLES (0x7F000000)

int AudioTrackRender::device_write_internal(IAFFrame *frame)
{
    uint64_t playedSamples = getDevicePlayedSimples();
    if (playedSamples >= OVERFLOW_SAMPLES) {
        uint64_t deviceQueDuration = getDeviceQuequedDuration();
        AF_LOGW("device_write_internal() will over flow  deviceQueDuration= %lld", deviceQueDuration);
        if (deviceQueDuration > 0) {
            //will over flow , wait to que buffer be played
            return -EAGAIN;
        } else {
            flush_device_inner(false);
            //FIXME : Some devices position is not be reset to 0 after flush, need create new AudioTrack.
        }
    }

    IAFFrame::audioInfo *audioInfo = &(frame->getInfo().audio);
    assert(audioInfo->format == AF_SAMPLE_FMT_S16);
    JniEnv  jniEnv;
    JNIEnv *handle = jniEnv.getEnv();

    int len = getPCMDataLen(audioInfo->channels, static_cast<AVSampleFormat>(audioInfo->format), audioInfo->nb_samples);

    if (len > jBufferLen) {
        if (jbuffer != nullptr) {
            handle->DeleteGlobalRef(jbuffer);
            jbuffer = nullptr;
        }
        jBufferLen = len;
    }

    if (jbuffer == nullptr) {
        jbyteArray obj = handle->NewByteArray(jBufferLen);
        jbuffer = handle->NewGlobalRef(obj);
        handle->DeleteLocalRef(obj);
    }

    bool rendered = false;
    if (mRenderingCb != nullptr) {
        rendered = mRenderingCb(mRenderingCbUserData, frame);
    }

    if (audio_track && method_write) {
        handle->SetByteArrayRegion(static_cast<jbyteArray>(jbuffer), 0, len, (jbyte *) frame->getData()[0]);
        jint written = handle->CallIntMethod(audio_track, method_write, jbuffer, 0, len);

        /*
         * 短写必须按"实际写进去的字节"记账：flush / pause 竞态下 write() 会少写，
         * 以前无条件 mSendSimples += nb_samples 会让"时钟认为已播出的"与"实际发声的"
         * 不一致（表现是 A/V 缓慢漂移、偶发咔哒）。
         * 不在这里循环补齐：短写通常说明 audio_track 已被 stop/flush，
         * 循环补写会空转；只把账记对，并把这次短写记进日志。
         */
        if (written < 0) {
            AF_LOGW("AudioTrack write failed: ret=%d len=%d\n", (int) written, len);
            logSilence(2, "AudioTrack write failed (underrun, or the track was stopped/reset)");
        } else if (written < len) {
            AF_LOGW("AudioTrack short write: %d of %d bytes accepted (dropped; clock accounts the actual amount)\n",
                    (int) written, len);
            int bytesPerSample = audioInfo->channels * 2;   // 输出固定 S16

            if (bytesPerSample > 0) {
                mSendSimples += (uint64_t) (written / bytesPerSample);
            }

            logSilence(2, "AudioTrack short write (the track accepted only part of the pcm)");
        } else {
            mSendSimples += audioInfo->nb_samples;
            /*
             * 【B 方案】真实 PCM 写成功 = 保活回合结束：计数与资源闩一起复位
             * （所以上界只约束"一轮连续无数据"，不是永久失效），并按需打一条结束日志。
             * 结束日志与开始日志配对，便于下一份日志直接验证保活起止与持续帧数。
             */
            if (mKeepAliveActive || mKeepAliveSilenceWrites > 0 || mKeepAliveCapped) {
                AF_LOGW("audio keep-alive: pcm resumed after %d silence write(s)%s — keep-alive episode over\n",
                        mKeepAliveSilenceWrites, mKeepAliveCapped ? " (the resource cap had been reached)" : "");
                mKeepAliveActive = false;
                mKeepAliveSilenceWrites = 0;
                mKeepAliveCapped = false;
            }
            /* 整帧被设备接受 = 声音在流动：清掉静音窗口（会有则记一条结束日志）。 */
            logSilence(0, "");
        }
    }
    return 0;
}

uint64_t AudioTrackRender::device_get_que_duration()
{
    uint64_t duration = 0;
    if (!mFrameQueue.empty()) {
        duration += static_cast<uint64_t>(mFrameQueue.front()->getInfo().duration * mFrameQueue.size());
    }

    uint64_t playedSimples = getDevicePlayedSimples() - mAudioFlushPosition;
    if (mSendSimples < playedSimples) {
        return duration;
    }

    return static_cast<uint64_t>(duration + (mSendSimples - playedSimples) / (float(mOutputInfo.sample_rate) / 1000000));
}

uint64_t AudioTrackRender::getDeviceQuequedDuration()
{
    uint64_t playedSimples = getDevicePlayedSimples() - mAudioFlushPosition;
    if (mSendSimples < playedSimples) {
        return 0;
    }

    return static_cast<uint64_t>((mSendSimples - playedSimples) / (float(mOutputInfo.sample_rate) / 1000000));
}

void AudioTrackRender::device_preClose()
{

    mRunning = false;

    if (mWriteThread) {
        if (std::this_thread::get_id() == mWriteThread->getId()) {
            //same thread
        } else {
            mWriteThread->prePause();
        }
    }
}
