#define LOG_TAG "ApsaraPlayerService"

#include "SuperMediaPlayer.h"
#include "media_player_error_def.h"
#include "media_player_error_map.h"
#include "utils/CicadaJSON.h"
#include "utils/CicadaUtils.h"
#include "utils/UrlUtils.h"
#include <cassert>
#include <cinttypes>
#include <cstdlib>
#include <codec/avcodecDecoder.h>
#include <codec/decoderFactory.h>
// isDrmVideo() 用 DrmHandlerPrototype::isSupport 判"本平台认不认这个 DRM scheme"，
// 所以需要 drm/DrmInfo.h（组探针）与 drm/DrmHandlerPrototype.h（查询）。两者都是
// 平台无关的共享头，各平台认不认由 framework/drm/ 下注册的 handler 决定。
#include <drm/DrmHandlerPrototype.h>
#include <drm/DrmInfo.h>
#include <data_source/dataSourcePrototype.h>
#include <demuxer/IDemuxer.h>
#include <demuxer/manifest/MediaManifestParser.h>
#include <render/renderFactory.h>
#include <utils/AFMediaType.h>
#include <utils/af_string.h>
#include <utils/err.h>
#include <utils/errors/framework_error.h>
#include <utils/ffmpeg_utils.h>
#include <utils/file/FileUtils.h>
#include <utils/frame_work_log.h>
#include <utils/oscl/oscl_utils.h>
#include <utils/property.h>
#include <utils/timer.h>


#ifdef __APPLE__

#include <TargetConditionals.h>
#include <codec/Apple/AppleVideoToolBox.h>
#include <render/audio/Apple/AFAudioSessionWrapper.h>

#endif

#define PTS_DISCONTINUE_DELTA (20 * 1000 * 1000)
#define VIDEO_PICTURE_MAX_CACHE_SIZE 2
#define FLOOD_LOG_WINDOW_MS (1000)
#define MAX_VIDEO_READ_AHEAD_US (2 * 1000 * 1000)
#define AUDIO_FRAME_QUEUE_FLOOR (9)
#define VIDEO_STARVE_ITERS (3)
#define AUDIO_SILENCE_NONE (0)
#define AUDIO_SILENCE_SEEK_CLOCK (1)
#define AUDIO_SILENCE_DEVICE_WRITE (2)
#define AUDIO_SILENCE_DEVICE_RESTART (3)
#define AUDIO_SILENCE_BUFFERING (4)
#define JOINING_STALE_FRAME_US (500 * 1000)
#define SEEK_CLOCK_TOLERANCE_US (300 * 1000)
#define PENDING_VIDEO_QUEUE_CAP 120
#define PENDING_VIDEO_QUEUE_HARD_CAP 320
#define PENDING_CAP_ACTIVE_MIN_US (3 * 1000 * 1000)
#define PENDING_CAP_AUDIO_MIN_US (2 * 1000 * 1000)
#define PENDING_DECODE_BURST 64
#define JOINING_FORCE_RENDER_MS (100)
#define VIDEO_STUCK_DISCARD_STREAK_MAX 8
#define VIDEO_CATCHUP_DISCARD_STREAK_MAX 20
#define PENDING_PREROLL_KEEP_US (0)


static int MAX_DECODE_ERROR_FRAME = 1000;

#define MAX_VIDEO_DECODER_REBUILDS (2)

#define PTS_REVERTING (mVideoPtsRevert != mAudioPtsRevert)

using namespace Cicada;
SuperMediaPlayer SuperMediaPlayer::se(1);

static MsgParam dummyMsg{{nullptr}};

const int64_t SuperMediaPlayer::SEEK_ACCURATE_MAX = 11 * 1000 * 1000;

#define HAVE_VIDEO (mCurrentVideoIndex >= 0)
#define HAVE_AUDIO (mCurrentAudioIndex >= 0)
#define HAVE_SUBTITLE (mCurrentSubtitleIndex >= 0)

#ifdef ENABLE_CACHE_MODULE
class SuperMediaPlayerCacheDataSource : public ICacheDataSource {
public:
    explicit SuperMediaPlayerCacheDataSource(SuperMediaPlayer &player) : mPlayer(player)
    {}

    ~SuperMediaPlayerCacheDataSource() override = default;

    int64_t getStreamSize() override
    {
        char streamSizeStr[MAX_OPT_VALUE_LENGTH] = {0};
        mPlayer.GetOption("mediaStreamSize", streamSizeStr);
        return atoll(streamSizeStr);
    }

    int64_t getDuration() override
    {
        /* 与 PlayerCacheDataSource 同口径：CicadaGetDuration 就是 GetDuration()。 */
        return mPlayer.GetDuration();
    }

    int getStreamMeta(Stream_meta *ptr, StreamType type) override
    {
        return mPlayer.getCurrentStreamMeta(ptr, type);
    }

private:
    SuperMediaPlayer &mPlayer;
};

std::shared_ptr<CacheManager> SuperMediaPlayer::cacheManagerOrNull()
{
    /*
     * 只做一次拷贝就放锁：拿到的 shared_ptr 保证这一包喂完之前管理器不会析构，
     * 而临界区里不做任何 I/O ⇒ 不会与 mCreateMutex 形成锁序。
     */
    std::lock_guard<std::mutex> lock(mCacheMutex);
    return mCacheManager;
}

void SuperMediaPlayer::sendMediaFrameToCache(const IAFPacket *frame, StreamType type)
{
    /*
     * 【缓存文件写成之后不再喂包】—— 这是"循环播放 / 反复 seek"下不涨内存的关键。
     *
     * CacheManager::complete()（在 EOF 处调用）会让 CacheModule::streamEnd() 把
     * mFrameEof 置真：remuxer 线程随即 break 出循环、关闭 muxer、把 .tmp 改名成正式
     * 缓存文件，然后**退出**。但 CacheModule::addFrame() 并不看这个状态 ——
     * 它照样 frame->clone() 后推进 mFrameInfoQueue，而那是一条只进不出的队列。
     * 于是"缓存已经写完、播放还在继续"（循环、或用户 seek 回去重播）时，
     * 队列会一直被填满而没有任何消费者。
     *
     * 所以成功回调触发过之后，这一层直接不再喂：文件已经完整，喂了也没有消费者。
     * 纯状态判据（一个 bool），没有计时器、没有轮次阈值。
     *
     * 顺带说明"为什么这里不像 C++ 门面那样在成功回调里临时关掉 loop"：
     * 门面 `MediaPlayer` 关 loop 是因为它自己的 `completionCallback` 会在播完之后
     * **重新 setDataSource(源 URL) + Prepare()** 把循环接过去（下一次直接播缓存文件）。
     * 本类（内核这一层）的循环是 `playCompleted()` 里原地 seek 回 0，**没有**那条
     * "播完重设源"的路径；在这里关掉 loop 只会让播放停住（而不是循环），
     * 用户看到的就是"一开缓存就不循环了"。上面的"写成后不再喂"已经解决了循环下的
     * 内存问题，所以这里不动循环语义。
     */
    if (mCacheSuccess.load()) {
        return;
    }

    if (std::shared_ptr<CacheManager> cacheManager = cacheManagerOrNull()) {
        cacheManager->sendMediaFrame(frame, type);
    }
}

void SuperMediaPlayer::ReleaseCacheManager()
{
    std::shared_ptr<CacheManager> manager;
    {
        std::lock_guard<std::mutex> lock(mCacheMutex);
        manager = std::move(mCacheManager);
        mCacheManager = nullptr;
    }

    if (manager != nullptr) {
        manager->stop("cache stopped by release");
        /* 交回 shared_ptr：若此刻播放线程正拿着同一份快照喂包，真正的析构会晚一步
         * 发生在他放掉那份快照之后（CacheManager 析构里会 delete 掉数据源对象）。 */
    }
}
#endif

SuperMediaPlayer::SuperMediaPlayer()
{
    AF_LOGD("SuperMediaPlayer()");

    mSet = static_cast<unique_ptr<player_type_set>>(new player_type_set());
    mBufferController = static_cast<unique_ptr<BufferController>>(new BufferController());
    mUtil = static_cast<unique_ptr<MediaPlayerUtil>>(new MediaPlayerUtil());
    mMPAUtil = static_cast<unique_ptr<MediaPlayerAnalyticsUtil>>(new MediaPlayerAnalyticsUtil());
    mMsgCtrlListener = static_cast<unique_ptr<SMPMessageControllerListener>>(new SMPMessageControllerListener(*this));
    mMessageControl = static_cast<unique_ptr<PlayerMessageControl>>(new PlayerMessageControl(*mMsgCtrlListener));
    mAudioRenderCB = static_cast<unique_ptr<ApsaraAudioRenderCallback>>(new ApsaraAudioRenderCallback(*this));
    mVideoRenderListener = static_cast<unique_ptr<ApsaraVideoRenderListener>>(new ApsaraVideoRenderListener(*this));
    mVideoProcessCb = static_cast<unique_ptr<ApsaraVideoProcessTextureCallback>>(new ApsaraVideoProcessTextureCallback(*this));
    mApsaraThread = static_cast<unique_ptr<afThread>>(new afThread([this]() -> int { return this->mainService(); }, LOG_TAG));

    /*
     * 纯加固（不是 16:34 那次闪退的原因）：把 mPNotifier 提前到 listener 之前创建。
     *
     * listener 一旦被建立，它的回调 onNetWorkRetry() / onNetWorkConnected()
     * （SuperMediaPlayerDataSourceListener.cpp:43 / :79）就会解引用 mPlayer.mPNotifier，
     * 而它原本要等到本函数末尾（原 :97）才被 new 出来，构造窗口内它是 nullptr。
     * 这和 enableRetry_l()（SuperMediaPlayerDataSourceListener.cpp:67）在构造期就读
     * mPlayer.mSet 属于同一类问题：listener 在构造期就会去碰 SuperMediaPlayer 的成员，
     * 所以它用到的成员都应该先于它初始化。PlayerNotifier 自身只创建一个通知线程对象，
     * 不依赖 SuperMediaPlayer 的其它成员，提前创建是安全的。
     */
    mPNotifier = new PlayerNotifier();

    mSourceListener = static_cast<unique_ptr<SuperMediaPlayerDataSourceListener>>(new SuperMediaPlayerDataSourceListener(*this));
    mDcaManager = static_cast<unique_ptr<SMP_DCAManager>>(new SMP_DCAManager(*this));
    mAVDeviceManager = static_cast<unique_ptr<SMPAVDeviceManager>>(new SMPAVDeviceManager());
    mRecorderSet = static_cast<unique_ptr<SMPRecorderSet>>(new SMPRecorderSet());

    Reset();
    mTimerInterval = 500;
}

SuperMediaPlayer::~SuperMediaPlayer()
{
    /*
     * 先把本实例发布给内核的"应用层编码能力/偏好"覆盖值撤掉（只撤自己发布的那一份），
     * 否则播放器释放之后 decoderFactory 还在按一个已经不存在的应用层结论做判定。
     * 纯状态收尾，不涉及任何线程/计时器。
     *
     * mine == nullptr（本实例从没接受过 CicadaSetVideoCodecSupport，包括进程级的
     * 原型实例 se）时**不碰**那个进程级落点：它是个函数内静态对象，退出阶段可能
     * 已经先于本对象析构，不必要地访问它没有好处。
     */
    {
        std::shared_ptr<const decoderFactory::AppCodecSupport> mine;
        {
            std::lock_guard<std::mutex> lock(mAppCodecSupportMutex);
            mine = mAppCodecSupport;
            mAppCodecSupport.reset();
        }

        if (mine != nullptr) {
            decoderFactory::clearAppCodecSupportIf(mine);
        }
    }

    if (mIsDummy) {
        return;
    }
    Stop();
    AF_LOGD("~SuperMediaPlayer");
    mCanceled = true;
    mPlayerCondition.notify_one();
    mApsaraThread->stop();
    mSubPlayer = nullptr;
    mSubListener = nullptr;
    // delete mPNotifier after mPMainThread, to avoid be using
    delete mPNotifier;
    mPNotifier = nullptr;
    mMessageControl = nullptr;
    mAVDeviceManager = nullptr;
#ifdef ENABLE_VIDEO_FILTER
    mFilterManager = nullptr;
#endif
}

void SuperMediaPlayer::putMsg(PlayMsgType type, const MsgParam &param, bool trigger)
{
    mMessageControl->putMsg(type, param);

    if (trigger) {
        mPlayerCondition.notify_one();
    }
}

void SuperMediaPlayer::SetView(void *view)
{
    mMsgCtrlListener->ProcessSetViewMsg(view);
}

void SuperMediaPlayer::ClearScreen()
{
    if (mAVDeviceManager->getVideoRender()) {
        mAVDeviceManager->getVideoRender()->clearScreen();
    }
}

int64_t SuperMediaPlayer::GetMasterClockPts()
{
    return mMasterClock.GetTime();
}

void SuperMediaPlayer::setBitStreamCb(readCB read, seekCB seek, void *arg)
{
    MsgParam param;
    MsgBitStreamParam msgBitStreamParam = {nullptr};
    msgBitStreamParam.read = read;
    msgBitStreamParam.seek = seek;
    msgBitStreamParam.arg = arg;
    param.msgBitStreamParam = msgBitStreamParam;
    putMsg(MSG_SET_BITSTREAM, param);
}

void SuperMediaPlayer::SetDataSource(const char *url)
{
    /*
     * 缓存的全部动作都发生在这一个函数里，位置与 C++ 门面
     * MediaPlayer::SetDataSource(const char *) 逐字一致 —— 因为这里正是
     * "C API 句柄那条路"的同一层（CicadaSetDataSourceWithUrl → ICicadaPlayer::
     * SetDataSource(const char *)），也就是说 MediaPlayer::SetDataSource 原来做的
     * 那套缓存流程被原样搬到了内核这一层，C API / NAPI / ArkTS 不需要各自再实现一遍。
     *
     * 为什么必须挂在这一层、而不是挂在底下 openUrl()/ProcessSetDataSourceMsg()：
     *   1) 缓存代理 URL 是在**发出 MSG_SETDATASOURCE 之前**就要算出来的 ——
     *      CacheManager::init() 只有在"缓存文件已经存在"时才返回本地文件路径，
     *      其余情况返回源 URL。换句话说"这次到底播哪个 URL"是一个**同步**决定，
     *      它决定了下游 MSG_SETDATASOURCE 里该放什么。挂到消息处理线程上就变成
     *      "消息已入队、再改 URL"，位置不对。
     *   2) 缓存的帧喂入需要 mCacheManager 在播放前就存在（播放线程每一帧都要查它）。
     *
     * 下游确实用的是代理 URL：mSet->url 由 ProcessSetDataSourceMsg() 落盘
     * （SMPMessageControllerListener.cpp:833-840），openUrl() 再用它建数据源
     * （同文件 :1715 `dataSourcePrototype::create(mPlayer.mSet->url, …)`）。
     * 所以这里把 proxyUrl 塞进那条通路就是"真正用在了 demuxer/数据源上"。
     *
     * 生效范围（本文件另外两个重载为什么不加）：
     *   · SetDataSource(const Manifest::MediaManifest &) 与 SetDataSource(const
     *     std::string &jsonManifest) 走的是"清单对象模式"，播放地址在清单**内部**，
     *     这一层拿不到最终 URL，缓存配置了一个 URL 也对不上；因此这两个重载保持原样。
     *   · 应用层若用清单模式，缓存不参与 —— 这是既有能力的边界，不是新增开关。
     */
    string playUrl = url ? url : "";
#ifdef ENABLE_CACHE_MODULE
    {
        /*
         * 换片源 ⇒ 上一份缓存管理器必须先停掉并释放（它记着旧 URL 与旧配置），
         * 否则新的 setSourceUrl 会落在旧对象上。stop 只停内部 remuxer 线程。
         */
        ReleaseCacheManager();
        mCacheSuccess = false;

        if (mCacheConfig.mEnable) {
            /* shared_ptr：喂包线程可能正拿着同一份快照（见 .h 里 mCacheManager 那段）。 */
            std::shared_ptr<CacheManager> manager(new CacheManager());
            manager->setCacheConfig(mCacheConfig);
            manager->setSourceUrl(playUrl);
            /*
             * description 原来由 C++ 门面从 C API 的 option 里取（CicadaGetOption），
             * 这里直接读同一份 option（SetOption("description", …) 的落点），
             * 少绕一层、语义完全相同；缓存文件里的 description 元数据就取它。
             */
            manager->setDescription(mSet->mOptions.get("description"));
            manager->setCacheFailCallback([this](int code, string msg) -> void {
                AF_LOGE("Cache fail : code = %d , msg = %s", code, msg.c_str());
                if (mPNotifier != nullptr) {
                    mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_CACHE_ERROR, msg.c_str());
                }
            });
            manager->setCacheSuccessCallback([this]() -> void {
                /*
                 * 缓存文件已经写成。除了上报事件，这个标志还是"不再往缓存里喂包"的判据
                 * （见 sendMediaFrameToCache()：complete() 之后 remuxer 线程已经退出，
                 * 继续喂只会往它的队列里堆没人消费的包）。
                 *
                 * 这里**不改循环语义**：C++ 门面 MediaPlayer 在成功回调里临时把 loop 关掉，
                 * 是因为它的 completionCallback 会重新 setDataSource(源 URL) 把循环接过去；
                 * 本类的循环是 playCompleted() 里原地 seek 回 0，没有那条重设源的路径，
                 * 在这里关 loop 只会让播放停住。详见 sendMediaFrameToCache() 的说明。
                 */
                mCacheSuccess = true;

                if (mPNotifier != nullptr) {
                    /*
                     * 事件描述必须是**非空**字符串：PlayerNotifier::NotifyEvent 里是
                     * `strdup(desc)`，传 nullptr 会直接解引用空指针。成功事件没有
                     * 原因可讲，就发空串（应用层拿到的 payload 是空字符串）。
                     */
                    mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_CACHE_SUCCESS, "");
                }
            });
            manager->setDataSource(new SuperMediaPlayerCacheDataSource(*this));
            playUrl = manager->init();
            {
                std::lock_guard<std::mutex> lock(mCacheMutex);
                mCacheManager = manager;
            }
        }
    }
#endif

    MsgParam param;
    MsgDataSourceParam dataSourceParam = {nullptr};
    dataSourceParam.url = new string(playUrl);
    param.dataSourceParam = dataSourceParam;
    putMsg(MSG_SETDATASOURCE, param);
}

void SuperMediaPlayer::SetDataSource(const Manifest::MediaManifest &manifest)
{
    MsgParam param;
    MsgManifestParam manifestParam{};
    manifestParam.manifest = new Manifest::MediaManifest(manifest);
    param.msgManifestParam = manifestParam;
    putMsg(MSG_SETMANIFESTSOURCE, param);
}

void SuperMediaPlayer::SetDataSource(const std::string &jsonManifest)
{
    std::string error;
    auto *manifest = new Manifest::MediaManifest();
    if (!Manifest::MediaManifestParser::parse(jsonManifest, *manifest, error)) {
        AF_LOGE("SetDataSource(json): manifest parse error: %s\n", error.c_str());
        /*
         * 【错误回调】原来这里只打一条 AF_LOGE 就静默 return，App 完全不知道
         * setDataSource(JSON) 失败（表现是"点了没反应"）。
         * 现在复用既有 error 通知通道上报一条明确错误：
         *   码   = MEDIA_PLAYER_ERROR_DEMUXER_MANIFEST_PARSE（ErrorCodeMap 映射 0x20030007）
         *   文案 = "MediaManifest JSON parse failed: <解析器给出的原因>"
         * 解析失败时不进入任何播放状态、不改动任何既有字段，对既有流程零影响；
         * 调用方可据该码/文案提示"清单 JSON 不合法"（见 docs/MANIFEST-OBJECT-GUIDE.md）。
         */
        const std::string notifyMsg = "MediaManifest JSON parse failed: " + error;
        mPNotifier->NotifyError(MEDIA_PLAYER_ERROR_DEMUXER_MANIFEST_PARSE, notifyMsg.c_str());
        delete manifest;
        return;
    }
    MsgParam param;
    MsgManifestParam manifestParam{};
    manifestParam.manifest = manifest;
    param.msgManifestParam = manifestParam;
    putMsg(MSG_SETMANIFESTSOURCE, param);
}

void SuperMediaPlayer::Prepare()
{

    if (mPlayStatus != PLAYER_INITIALZED && mPlayStatus != PLAYER_STOPPED) {
        Stop();
    }

    mMPAUtil->reset();

#if TARGET_OS_IPHONE
    AFAudioSessionWrapper::activeAudio();
#endif
    mPrepareStartTime = af_gettime_relative();
    std::unique_lock<std::mutex> uMutex(mPlayerMutex);
    putMsg(MSG_PREPARE, dummyMsg);
    mApsaraThread->start();
}

void SuperMediaPlayer::CaptureScreen()
{
    std::lock_guard<std::mutex> uMutex(mCreateMutex);

    if (mAVDeviceManager->getVideoRender()) {
        mAVDeviceManager->getVideoRender()->captureScreen([this](uint8_t *data, int width, int height) {
            if (this->mPNotifier) {
                this->mPNotifier->NotifyCaptureScreen(data, width, height);
            }
        });
    } else {
        if (this->mPNotifier) {
            this->mPNotifier->NotifyCaptureScreen(nullptr, 0, 0);
        }
    }
}

void SuperMediaPlayer::SetVolume(float volume)
{
    //TODO:put message to
    mSet->mVolume = volume;

    if (mSet->mVolume < 0) {
        mSet->mVolume = 0;
    } else if (mSet->mVolume > 1.0) {
        AF_LOGW("volume >1.0");
    }

    mAVDeviceManager->setVolume(mSet->mVolume);
}

void SuperMediaPlayer::Start()
{
    if ((PLAYER_INITIALZED == mPlayStatus) || (PLAYER_PREPARING == mPlayStatus) || PLAYER_PREPARINIT == mPlayStatus) {
        waitingForStart = true;
    }

    this->putMsg(MSG_START, dummyMsg);
}


void SuperMediaPlayer::Pause()
{
    waitingForStart = false;
    this->putMsg(MSG_PAUSE, dummyMsg);
}


void SuperMediaPlayer::SeekTo(int64_t pos, bool bAccurate)
{
    /*
     * 时长已知时把目标钳进 [0, duration]（含"目标 == duration"）：
     *   · 目标 >= duration ⇒ 每一条帧都在目标之前 ⇒ 只靠"包含/越过目标"永远无法结束过滤，
     *     只能靠 RenderVideo 的 EOF 结构性终止兜住（那条兜底仍然保留）；
     *   · 钳位之后"目标 == duration"必然被最后一帧覆盖（最后一帧的帧尾 >= duration），
     *     于是正常路径就能收口，而不是依赖 EOF 兜底。
     * 时长未知 / 实时流（mDuration <= 0）**不改**：那种情况本来就没有"末尾"可言。
     * 这不是新配置开关，只是对既有目标值域的约束。
     */
    int64_t seekTargetUs = (int64_t) pos * 1000;

    if (mDuration > 0) {
        if (seekTargetUs < 0) {
            seekTargetUs = 0;
        } else if (seekTargetUs > mDuration) {
            seekTargetUs = mDuration;
        }
    }

    /*
     * 【音频落点地板 / 声明的时机】必须在下面那次 beginDiscontinuity() **之前**声明
     * "本次 seek 会把音频时间轴重新锚到 seekTargetUs" —— 这次分支里的
     * beginDiscontinuity() 就会据此臂上 mDiscontinuity.audioLandingPending，
     * 让落在目标点之前的音频帧一律不上设备（见该字段的说明）。
     * 之后 ProcessSeekToMsg() 还会走一次 beginDiscontinuity(seekPos)，那时闩已被消费、
     * 但它会用同一个 targetUs 重新算出同样的结果（幂等，不会把地板丢掉）。
     * 它是"这次 seek 的属性"，所以由 seek 入口声明；换档 / Reset 走
     * beginDiscontinuity 时这个闩保持假，音频地板的臂上条件不成立。
     */
    mSeekAudioLandingReset = true;
    /*
     * 读到 mDiscontinuity，它看到的新代际就是"这一次 seek"，晚置会让这一小段
     * 时间窗内的包/帧被算到上一次代际上。
     */
    beginDiscontinuity(seekTargetUs);
    /* 【延迟量化】记下"用户这一刻要 seek"的墙钟，供后面几行日志给出各段耗时。 */
    mSeekRequestMs = af_getsteady_ms();
    mSeekDecodeStartIsKey = false;
    mVideoDiscardStreak = 0;
    mVideoDiscardGapAbsUs = INT64_MIN;
    mCatchUpDiscardStreak = 0;
    /*
     * "读到落点关键帧包那一刻，把音频包队列裁到目标点"这一个动作。
     *
     * 被删成员的复位一并消失（锚点事件闩 / 只锚一次闩 / 音频地板 / 连续性高水位 /
     * 音频重定位事件）：它们承载的"锚点事件、音频地板、高水位、重定位"全部不再存在。
     * 音频时钟基准不需要在这里清：seek 在途时 getAudioPlayTimeStamp() 用 mSeekFlag
     * 判"参考暂不可用"，而本次 seek 的 FlushAudioPath() 会在设备 flush 之后
     * 把基准重钉到 targetUs（见 FlushAudioPath 与 beginDiscontinuity 的说明）。
     */
    mSeekAudioAlignDone = false;
    mVideoDecodeRetrySeen = false;
    mDecodeStallIters = 0;
    mDecodeStallRebuildDone = false;
    MsgParam param;
    MsgSeekParam seekParam;
    /* 用钳位后的目标：消息处理线程与 mSeekPos / mDiscontinuity 必须是同一个值。 */
    seekParam.seekPos = seekTargetUs;
    seekParam.bAccurate = bAccurate;
    param.seekParam = seekParam;
    this->putMsg(MSG_SEEKTO, param);
    mSeekPos = seekTargetUs;
    mSeekNeedCatch = bAccurate;
    /*
     * 【位置上报在这里不做任何事，这是对的】
     * 位置上报与渲染节拍共用**唯一那根内容时间轴**（mMasterClock.GetTime()）：seek 一发起
     * ProcessSeekToMsg() 就把它钉在 seekPos，落点帧被采纳时 acceptDiscontinuityLandingFrame()
     * 再钉一次 targetUs。所以既不需要"下界"，也不需要"归属标记"，更不需要在 seek 入口
     * 预置任何位置类状态；上面那次 beginDiscontinuity() 已经够了。
     */
}

void SuperMediaPlayer::Mute(bool bMute)
{
    if (bMute == mSet->bMute) {
        return;
    }

    mSet->bMute = bMute;
    this->putMsg(MSG_MUTE, dummyMsg);
}

void SuperMediaPlayer::EnterBackGround(bool back)
{
    // lock mAppStatusMutex before mCreateMutex
    std::lock_guard<std::mutex> lock(mAppStatusMutex);
    MsgParam param;
    MsgHoldOnVideoParam holdParam;

    if (back) {
        AF_LOGI("EnterBackGround");
        mAppStatus = APP_BACKGROUND;
        holdParam.hold = true;
        param.msgHoldOnVideoParam = holdParam;
        putMsg(MSG_INTERNAL_VIDEO_HOLD_ON, param);

        if (mPlayStatus == PLAYER_PLAYING) {
            putMsg(MSG_INTERNAL_VIDEO_CLEAN_FRAME, dummyMsg);
        }
    } else {
        AF_LOGI("EnterBackGround APP_FOREGROUND");
        mAppStatus = APP_FOREGROUND;
        holdParam.hold = false;
        param.msgHoldOnVideoParam = holdParam;
        putMsg(MSG_INTERNAL_VIDEO_HOLD_ON, param);
    }
}

StreamType SuperMediaPlayer::SwitchStream(int streamIndex)
{
    MsgParam param;
    MsgChangeStreamParam streamParam;
    streamParam.index = streamIndex;
    param.streamParam = streamParam;
    StreamType streamType = ST_TYPE_UNKNOWN;
    PlayMsgType type = MSG_INVALID;
    std::deque<StreamInfo *> &streamInfoQueue = mMediaInfo.mStreamInfoQueue;
    for (auto &it : streamInfoQueue) {
        if (it->streamIndex == streamIndex) {
            switch (it->type) {
                case ST_TYPE_VIDEO:
                    streamType = ST_TYPE_VIDEO;
                    type = MSG_CHANGE_VIDEO_STREAM;
                    break;

                case ST_TYPE_AUDIO:
                    streamType = ST_TYPE_AUDIO;
                    type = MSG_CHANGE_AUDIO_STREAM;
                    break;

                case ST_TYPE_SUB:
                    streamType = ST_TYPE_SUB;
                    type = MSG_CHANGE_SUBTITLE_STREAM;
                    break;

                default:
                    AF_LOGE("unknown stream Type");
                    return streamType;
            }

            break;
        }
    }

    if (type != MSG_INVALID) {
        this->putMsg(type, param);
        AF_LOGI("switch stream request posted: index=%d type=%d\n", streamIndex, (int) streamType);
    } else {
        std::string knownStreams;

        for (auto &it : mMediaInfo.mStreamInfoQueue) {
            if (it != nullptr) {
                knownStreams += std::to_string(it->streamIndex) + ":t" + std::to_string((int) it->type) + " ";
            }
        }

        AF_LOGW("switch stream request DROPPED: index=%d is not in the kernel stream list; "
                "known streams (index:type) = %s\n",
                streamIndex, knownStreams.c_str());
    }

    return streamType;
}

void SuperMediaPlayer::Interrupt(bool inter)
{
    AF_TRACE;
    std::lock_guard<std::mutex> locker(mCreateMutex);

    if (mDataSource) {
        mDataSource->Interrupt(inter);
    } else {
        AF_TRACE;
    }

    if (mDemuxerService) {
        mDemuxerService->interrupt(inter);
        mDemuxerService->preStop();
    } else {
        AF_TRACE;
    }
}


int SuperMediaPlayer::Stop()
{
    if ((afThread::THREAD_STATUS_RUNNING != mApsaraThread->getStatus()) &&
        ((mPlayStatus == PLAYER_IDLE) || (mPlayStatus == PLAYER_STOPPED))) {
#ifdef ENABLE_CACHE_MODULE
        mCacheSuccess = false;
        ReleaseCacheManager();
#endif
        return 0;
    }

    /* 显式持有锁直到 Stop() 返回，避免 MSVC 将临时 lock 判定为未保护作用域。 */
    std::unique_lock<std::mutex> playerLock(mPlayerMutex);
    AF_LOGI("Player ReadPacket Stop");
    int64_t t1 = af_getsteady_ms();
    AF_TRACE;
    waitingForStart = false;
    mCanceled = true;
    mPNotifier->Clean();
    mPNotifier->Enable(false);

#ifdef ENABLE_CACHE_MODULE
    mCacheSuccess = false;
    ReleaseCacheManager();
#endif

    // video render use a dispatch_sync to main thread, to avoid dead lock,release the thread to deal dispatch_sync job
    // FIXME: create render in setView api in main thread on apple platform
    //    #ifdef __APPLE__
    //        if (strcmp(dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL), dispatch_queue_get_label(dispatch_get_main_queue())) == 0) {
    //            if (!mVideoRenderInited) {
    //                while (!mMainServiceCanceled) {
    //                    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, 1);
    //                    AF_LOGI("Waiting for main service canceled\n");
    //                }
    //            }
    //        }
    //    #endif
    Interrupt(true);
    mPlayerCondition.notify_one();
    mApsaraThread->pause();
    mAVDeviceManager->invalidDevices(SMPAVDeviceManager::DEVICE_TYPE_AUDIO | SMPAVDeviceManager::DEVICE_TYPE_VIDEO);
    mPlayStatus = PLAYER_STOPPED;
    //        ChangePlayerStatus(PLAYER_STOPPED);
    mBufferController->ClearPacket(BUFFER_TYPE_AV);

    AF_TRACE;
    FlushAudioPath();

    AF_TRACE;
    mBRendingStart = false;
    AF_TRACE;
    FlushVideoPath(true, true, __func__);
    // clear the message queue after flash video render
    mMessageControl->clear();
    AF_TRACE;

    if (mDemuxerService) {
        mDemuxerService->interrupt(1);

        if (mDataSource) {
            mDataSource->Interrupt(true);
        }

        std::lock_guard<std::mutex> uMutex(mCreateMutex);
        mDemuxerService->stop();
        mDemuxerService->close();

        if (mMixMode) {
            if (mMainStreamId != -1) {
                mDemuxerService->CloseStream(mMainStreamId);
            }

            if (mCurrentSubtitleIndex >= 0) {
                mDemuxerService->CloseStream(mCurrentSubtitleIndex);
            }
        } else {
            if (mCurrentAudioIndex >= 0) {
                mDemuxerService->CloseStream(mCurrentAudioIndex);
            }

            if (mCurrentVideoIndex >= 0) {
                mDemuxerService->CloseStream(mCurrentVideoIndex);
            }

            if (mCurrentSubtitleIndex >= 0) {
                mDemuxerService->CloseStream(mCurrentSubtitleIndex);
            }
        }
    }

    if (mDataSource) {
        mDataSource->Close();
        std::lock_guard<std::mutex> uMutex(mCreateMutex);
        delete mDataSource;
        mDataSource = nullptr;
    }

    if (mAVDeviceManager->getVideoRender()) {
        // lock mAppStatusMutex before mCreateMutex
        std::lock_guard<std::mutex> lock(mAppStatusMutex);

        // for iOS, don't delete render in background, and we should reuse it later.
        if (APP_BACKGROUND != mAppStatus) {
            std::lock_guard<std::mutex> uMutex(mCreateMutex);

            if (mSet->clearShowWhenStop) {
                mAVDeviceManager->getVideoRender()->clearScreen();
            }
        }
    }

    delete mVideoParser;
    mVideoParser = nullptr;
    {
        std::lock_guard<std::mutex> uMutex(mCreateMutex);
        std::deque<StreamInfo *> &streamInfoQueue = mMediaInfo.mStreamInfoQueue;
        for (StreamInfo *info : streamInfoQueue) {
            releaseStreamInfo(info);
        }
        streamInfoQueue.clear();
        mMediaInfo.totalBitrate = 0;
    }
    mBufferController->ClearPacket(BUFFER_TYPE_SUBTITLE);
    Reset();

    mRecorderSet->reset();

    AF_LOGD("stop spend time is %lld", af_getsteady_ms() - t1);
    return 0;
}

void SuperMediaPlayer::releaseStreamInfo(const StreamInfo *info) const
{
    if (info->subtitleLang) {
        free(info->subtitleLang);
    }

    if (info->audioLang) {
        free(info->audioLang);
    }

    if (info->description) {
        free(info->description);
    }

    delete info;
}

void SuperMediaPlayer::SetRefer(const char *referer)
{
    if (referer) {
        mSet->refer = referer;
    }
}

void SuperMediaPlayer::SetUserAgent(const char *userAgent)
{
    if (userAgent) {
        mSet->userAgent = userAgent;
    }
}

void SuperMediaPlayer::SetTimeout(int timeout)
{
    mSet->timeout_ms = timeout;
}

void SuperMediaPlayer::SetDropBufferThreshold(int dropValue)
{
    mSet->RTMaxDelayTime = dropValue * 1000;
}

void SuperMediaPlayer::SetLooping(bool looping)
{
    mSet->bLooping = looping;
}

bool SuperMediaPlayer::isLooping()
{
    return mSet->bLooping;
}

int SuperMediaPlayer::SetOption(const char *key, const char *value)
{
    if (key == nullptr) {
        return -1;
    }

    int duration;
    string theKey = key;

    if (theKey == "startBufferDuration") {
        duration = atoi(value);

        if (duration > 0) {
            mSet->startBufferDuration = duration * 1000;
        }
    } else if (theKey == "RTMaxDelayTime") {
        duration = atoi(value);

        if (duration > 0) {
            mSet->RTMaxDelayTime = duration * 1000;
            mSet->mOptions.set(theKey, std::to_string(mSet->RTMaxDelayTime), options::REPLACE);
        }
    } else if (theKey == "highLevelBufferDuration") {
        duration = atoi(value);

        if (duration > 0) {
            mSet->highLevelBufferDuration = duration * 1000;
            mSet->mOptions.set(theKey, std::to_string(mSet->highLevelBufferDuration), options::REPLACE);
        }
    } else if (theKey == "http_proxy") {
        mSet->http_proxy = value;
    } else if (theKey == "maxBufferDuration") {
        duration = atoi(value);

        if (duration > 0) {
            mSet->maxBufferDuration = int64_t(duration) * 1000;
            mSet->mOptions.set(theKey, std::to_string(mSet->maxBufferDuration), options::REPLACE);
        }
    } else if (theKey == "LowLatency") {
        mSet->bLowLatency = (bool) atoi(value);
    } else if (theKey == "ClearShowWhenStop") {
        int clearShowWhenStop = atoi(value);
        mSet->clearShowWhenStop = (bool) clearShowWhenStop;
    } else if (theKey == "enableVideoTunnelRender") {
        mSet->bEnableTunnelRender = (atoi(value) != 0);
    } else if (theKey == "disableAudio") {
        mSet->bDisableAudio = (atoi(value) != 0);
    } else if (theKey == "disableVideo") {
        mSet->bDisableVideo = (atoi(value) != 0);
    } else if (theKey == "timerInterval") {
        mTimerInterval = atoi(value);
    } else if (theKey == "Analytics.ReportID") {
        if (nullptr == value) {
            return -1;
        }

        int64_t eventReportID = atoll(value);
        mSet->AnalyticsID = eventReportID;
    } else if (theKey == "bandWidth") {
        mSet->mDefaultBandWidth = atoi(value);
    } else if (theKey == "description") {
        mSet->mOptions.set(theKey, value, options::REPLACE);
        return 0;
    } else if (theKey == "enableVRC") {
        mSet->bEnableVRC = (atoi(value) != 0);
    } else if (theKey == "maxAccurateSeekDelta") {
        mSet->maxASeekDelta = atoi(value) * 1000;
    } else if (theKey == "maxVideoRecoverSize") {
        mSet->maxVideoRecoverSize = atoi(value);
    } else if (theKey == "surfaceChanged") {
        std::lock_guard<std::mutex> uMutex(mCreateMutex);

        if (mAVDeviceManager->isVideoRenderValid()) {
            mAVDeviceManager->getVideoRender()->surfaceChanged();
        }
    } else if (theKey == "streamTypes") {
        uint64_t flags = atoll(value);
        mSet->bDisableAudio = mSet->bDisableVideo = true;
        if (flags & VIDEO_FLAG) {
            mSet->bDisableVideo = false;
        }
        if (flags & AUDIO_FLAG) {
            mSet->bDisableAudio = false;
        }
    } else if (theKey == "IPResolveType") {
        uint64_t type = atoll(value);
        mSet->mIpType = static_cast<IpResolveType>(type);
    } else if (theKey == "fastStart") {
        mSet->mFastStart = atol(value) != 0;
    } else if (theKey == "pixelBufferOutputFormat") {
        mSet->pixelBufferOutputFormat = atol(value);
    } else if (theKey == "seekExactLanding") {
        mSeekExactLanding = (atoi(value) != 0);
    } else if (theKey == "liveStartIndex") {
        mSet->mOptions.set(theKey, value, options::REPLACE);
    } else if (theKey == "DRMMagicKey") {
        mSet->drmMagicKey = value;
    } else if (theKey == "sessionId") {
        mSet->sessionId = value;

        std::lock_guard<std::mutex> uMutex(mCreateMutex);
        if (mDemuxerService != nullptr && mDemuxerService->getDemuxerHandle()) {
            mDemuxerService->getDemuxerHandle()->SetOption("sessionId", mSet->sessionId);
        }
    } else if (theKey == "networkRetryCount") {
        mSet->netWorkRetryCount = (int) atol(value);
    } else if (theKey == "maxBackwardBufferDuration") {
        mBufferController->SetMaxBackwardDuration(BUFFER_TYPE_ALL, atoll(value) * 1000);
    } else if (theKey == "preferAudio") {
        mSet->preferAudio = (atoi(value) != 0);
        AF_LOGI("preferAudio %d\n", mSet->preferAudio);
        std::lock_guard<std::mutex> uMutex(mCreateMutex);
        if (mDemuxerService && mDemuxerService->getDemuxerHandle()) {
            mDemuxerService->getDemuxerHandle()->SetOption("preferAudio", mSet->preferAudio);
        }
    }

    return 0;
}

void SuperMediaPlayer::GetOption(const char *key, char *value)
{
    if (key == nullptr) {
        return;
    }

    string theKey = key;

    if (theKey == "maxBufferDuration") {
        snprintf(value, MAX_OPT_VALUE_LENGTH, "%" PRId64 "", mSet->maxBufferDuration);
    } else if (theKey == "mediaStreamSize") {
        int64_t size = -1;
        std::unique_lock<std::mutex> uMutex(mCreateMutex);

        if (mDataSource && mDemuxerService) {
            if (!mDemuxerService->isPlayList()) {
                size = mDataSource->Seek(0, SEEK_SIZE);
            }
        } else if (mBSSeekCb) {
            size = mBSSeekCb(mBSCbArg, 0, SEEK_SIZE);
        }

        snprintf(value, MAX_OPT_VALUE_LENGTH, "%" PRId64 "", size);
    } else if (theKey == "description") {
        sprintf(value, "%s", mSet->mOptions.get("description").c_str());
    } else if (theKey == "descriptionLen") {
        snprintf(value, MAX_OPT_VALUE_LENGTH, "%lu", static_cast<unsigned long>(mSet->mOptions.get("description").length()));
    } else if (theKey == "renderFps") {
        float renderFps = GetVideoRenderFps();
        snprintf(value, MAX_OPT_VALUE_LENGTH, "%f", renderFps);
    } else if (theKey == "videoDroppedInfo") {
        uint64_t total, dropped;
        mUtil->getVideoDroppedInfo(total, dropped);
        snprintf(value, MAX_OPT_VALUE_LENGTH, "%" PRIu64 "/%" PRIu64, dropped, total);
    }
}

void SuperMediaPlayer::NotifyPosition(int64_t position)
{
    mPNotifier->NotifyPosition(position / 1000);
}

int64_t SuperMediaPlayer::getCurrentPosition()
{
    if (isSeeking()) {
        return mSeekPos;
    }

    mCurrentPos = mCurrentPos.load() < 0 ? 0 : mCurrentPos.load();

    if (mDuration > 0) {
        mCurrentPos = mCurrentPos.load() <= mDuration ? mCurrentPos.load() : mDuration;
    }
    (void) mDiscontinuity.generation.load();
    const int64_t discontinuityTargetUs = mDiscontinuity.targetUs;

    if (discontinuityTargetUs != INT64_MIN) {
        int64_t contentUs = mMasterClock.GetTime();

        /*
         * 坏值（未初始化 / 环绕 / 负值）不许上报：退回 mCurrentPos（帧驱动的管道位置）。
         * 与 getAudioPlayTimeStamp() 里"哨兵值不当基准"同一条口径（纯值判据）。
         */
        if (contentUs < 0 || af_clock_value_is_unset(contentUs)) {
            contentUs = mCurrentPos.load();
        }

        if (mDuration > 0 && contentUs > mDuration) {
            contentUs = mDuration;
        }

        return contentUs;
    }

    return mCurrentPos;
}

void SuperMediaPlayer::NotifyUtcTime()
{
    if (mCurrentFrameUtcTime < 0) {
        return;
    }
    mPNotifier->NotifyUtcTime(mCurrentFrameUtcTime / 1000);
}

void SuperMediaPlayer::SetScaleMode(ScaleMode mode)
{
    if (mode == mSet->scaleMode) {
        return;
    }

    mSet->scaleMode = static_cast<ScaleMode>(mode);
    this->putMsg(MSG_SET_DISPLAY_MODE, dummyMsg);
}

void SuperMediaPlayer::SetRotateMode(RotateMode mode)
{
    if (mode == mSet->rotateMode) {
        return;
    }

    mSet->rotateMode = static_cast<RotateMode>(mode);
    this->putMsg(MSG_SET_ROTATE_MODE, dummyMsg);
}

RotateMode SuperMediaPlayer::GetRotateMode()
{
    return mSet->rotateMode;
}

void SuperMediaPlayer::SetMirrorMode(MirrorMode mode)
{
    if (mode == mSet->mirrorMode) {
        return;
    }

    mSet->mirrorMode = static_cast<MirrorMode>(mode);
    this->putMsg(MSG_SET_MIRROR_MODE, dummyMsg);
}

void SuperMediaPlayer::SetVideoBackgroundColor(uint32_t color)
{
    if (color == mSet->mVideoBackgroundColor) {
        return;
    }

    mSet->mVideoBackgroundColor = color;
    this->putMsg(MSG_SET_VIDEO_BACKGROUND_COLOR, dummyMsg);
}

MirrorMode SuperMediaPlayer::GetMirrorMode()
{
    return mSet->mirrorMode;
}

/*
 * 色觉辅助滤镜
 * 与 SetMirrorMode 的唯一区别：矩阵逐元素比较（镜像是个枚举，可以直接 ==）。
 * 单位矩阵 = 关闭（见 player_types.h 里 colorMatrix 的默认值）。
 */
void SuperMediaPlayer::SetColorMatrix(const float matrix[9])
{
    if (matrix == nullptr) {
        return;
    }

    for (int i = 0; i < 9; i++) {
        if (mSet->colorMatrix[i] != matrix[i]) {
            for (int j = 0; j < 9; j++) {
                mSet->colorMatrix[j] = matrix[j];
            }
            this->putMsg(MSG_SET_COLOR_MATRIX, dummyMsg);
            return;
        }
    }
}

ScaleMode SuperMediaPlayer::GetScaleMode()
{
    return mSet->scaleMode;
}

int64_t SuperMediaPlayer::GetBufferPosition()
{
    int64_t bufferPosition = mBufferPosition;
    const int64_t currentPosition = getCurrentPosition();

    if (bufferPosition < currentPosition) {
        bufferPosition = currentPosition;
    }

    if (mDuration > 0 && bufferPosition > mDuration) {
        bufferPosition = mDuration;
    }

    return bufferPosition / 1000;
}

int64_t SuperMediaPlayer::GetDuration() const
{
    if (mDuration != INT64_MIN) {
        return mDuration / 1000;
    }
    return -1;
}

// TODO: change name to EnableHwDecode
void SuperMediaPlayer::SetDecoderType(DecoderType type)
{
    mSet->bEnableHwVideoDecode = (type == DT_HARDWARE);
}

void SuperMediaPlayer::AddCustomHttpHeader(const char *header)
{
    for (auto &item : mSet->customHeaders) {
        if (item == header) {
            return;
        }
    }

    mSet->customHeaders.emplace_back(header);
}

void SuperMediaPlayer::RemoveAllCustomHttpHeader()
{
    mSet->customHeaders.clear();
}

// TODO: move to mainService thread
void SuperMediaPlayer::setSpeed(float speed)
{
    speed = max(min(speed, MAX_SPEED), MIN_SPEED);
    MsgParam param;
    MsgSpeedParam speedParam;
    speedParam.speed = speed;
    param.msgSpeedParam = speedParam;
    putMsg(MSG_SET_SPEED, param);
}

float SuperMediaPlayer::getSpeed()
{
    return mSet->rate;
}

DecoderType SuperMediaPlayer::GetDecoderType()
{
    std::lock_guard<std::mutex> uMutex(mCreateMutex);

    if (mAVDeviceManager->getVideoDecoderFlags() & DECFLAG_HW) {
        return DT_HARDWARE;
    }

    return DT_SOFTWARE;
}

bool SuperMediaPlayer::IsVideoDecoderHardware()
{
    if (mAVDeviceManager == nullptr) {
        return false;
    }

    IDecoder *videoDecoder = mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);

    if (videoDecoder == nullptr) {
        return false;
    }

    return videoDecoder->isHardwareDecoderInUse();
}

PlayerStatus SuperMediaPlayer::GetPlayerStatus() const
{
    return mPlayStatus;
}

float SuperMediaPlayer::GetVolume() const
{
    return mSet->mVolume;
}

int64_t SuperMediaPlayer::GetPropertyInt(PropertyKey key)
{
    switch (key) {
        case PROPERTY_KEY_VIDEO_BUFFER_LEN: {
            int64_t duration = mBufferController->GetPacketDuration(BUFFER_TYPE_VIDEO);

            if (duration < 0) {
                duration = mBufferController->GetPacketLastPTS(BUFFER_TYPE_VIDEO) - mBufferController->GetPacketPts(BUFFER_TYPE_VIDEO);
            }

            return duration;
        }

        case PROPERTY_KEY_REMAIN_LIVE_SEG:
            return mRemainLiveSegment;

        case PROPERTY_KEY_NETWORK_IS_CONNECTED:
            return mSourceListener->isConnected();

        case PROPERTY_KEY_RE_BUFFERING:
            return mBufferingFlag;

        case PROPERTY_KEY_DOWNLOAD_COMPLETED:
            return mEof;

        default:
            break;
    }

    return 0;
}

std::string SuperMediaPlayer::GetPropertyString(PropertyKey key, const CicadaJSONItem &param)
{
    switch (key) {
        case PROPERTY_KEY_RESPONSE_INFO: {
            CicadaJSONArray array;
            std::lock_guard<std::mutex> uMutex(mCreateMutex);
            MediaPlayerAnalyticsUtil::addURLProperty("responseInfo", array, mDataSource);
            //if (mDemuxerService->isPlayList())
            {
                std::deque<StreamInfo *> &streamInfoQueue = mMediaInfo.mStreamInfoQueue;
                MediaPlayerAnalyticsUtil::getPropertyJSONStr("responseInfo", array, false, streamInfoQueue, mDemuxerService.get());
            }
            return array.printJSON();
        }

        case PROPERTY_KEY_CONNECT_INFO: {
            std::lock_guard<std::mutex> uMutex(mCreateMutex);

            if (mDataSource) {
                return mDataSource->GetOption("connectInfo");
            }

            return "";
        }

        case PROPERTY_KEY_OPEN_TIME_STR: {
            CicadaJSONArray array;
            CicadaJSONItem item;
            item.addValue("readpacketMS", (double) mFirstReadPacketSucMS);
            array.addJSON(item);
            std::lock_guard<std::mutex> uMutex(mCreateMutex);
            MediaPlayerAnalyticsUtil::addURLProperty("connectInfo", array, mDataSource);
            //if (mDemuxerService->isPlayList())
            {
                std::deque<StreamInfo *> &streamInfoQueue = mMediaInfo.mStreamInfoQueue;
                MediaPlayerAnalyticsUtil::getPropertyJSONStr("openJsonInfo", array, true, streamInfoQueue, mDemuxerService.get());
            }
            return array.printJSON();
        }

        case PROPERTY_KEY_PROBE_STR: {
            CicadaJSONArray array;
            std::lock_guard<std::mutex> uMutex(mCreateMutex);
            MediaPlayerAnalyticsUtil::addURLProperty("probeInfo", array, mDataSource);

            if (nullptr == mDemuxerService) {
                return array.printJSON();
            } else if (mDemuxerService->isPlayList()) {
                std::deque<StreamInfo *> &streamInfoQueue = mMediaInfo.mStreamInfoQueue;
                MediaPlayerAnalyticsUtil::getPropertyJSONStr("probeInfo", array, false, streamInfoQueue, mDemuxerService.get());
            } else {
                CicadaJSONItem item(mDemuxerService->GetProperty(0, "probeInfo"));
                item.addValue("type", "video");
                array.addJSON(item);
            }

            return array.printJSON();
        }

        case PROPERTY_KEY_DELAY_INFO: {
            std::lock_guard<std::mutex> uMutex(mCreateMutex);
            if (nullptr != mDemuxerService) {
                string ret = mDemuxerService->GetProperty(0, "delayInfo");
                return ret;
            }

            return "";
        }
        case PROPERTY_KEY_PLAY_CONFIG: {
            CicadaJSONItem item{};
            item.addValue("http_proxy", mSet->http_proxy);
            item.addValue("refer", mSet->refer);
            item.addValue("timeout_ms", (int) mSet->timeout_ms);
            item.addValue("RTMaxDelayTime", (int) mSet->RTMaxDelayTime);
            item.addValue("startBufferDuration", (int) mSet->startBufferDuration);
            item.addValue("highLevelBufferDuration", (int) mSet->highLevelBufferDuration);
            item.addValue("maxBufferDuration", (int) mSet->maxBufferDuration);
            return item.printJSON();
        }
        case PROPERTY_KEY_DECODE_INFO: {
            CicadaJSONArray decodeInfos{};
            if (HAVE_AUDIO) {
                CicadaJSONItem audioDecodeInfo{};
                audioDecodeInfo.addValue("type", "audio");
                audioDecodeInfo.addValue("createDecodeCost", (int) mRecorderSet->createAudioDecoderCostMs);
                audioDecodeInfo.addValue("decodeFirstCost", (int) mRecorderSet->decodeFirstAudioFrameInfo.getDecodeFirstFrameCost());
                audioDecodeInfo.addValue("firstSize", (int) mRecorderSet->decodeFirstAudioFrameInfo.firstPacketSize);
                audioDecodeInfo.addValue("firstPts", (double) mRecorderSet->decodeFirstAudioFrameInfo.firstPacketPts);
                decodeInfos.addJSON(audioDecodeInfo);
            }
            if (HAVE_VIDEO) {
                CicadaJSONItem videoDecodeInfo{};
                videoDecodeInfo.addValue("type", "video");
                videoDecodeInfo.addValue("createDecodeCost", (int) mRecorderSet->createVideoDecoderCostMs);
                videoDecodeInfo.addValue("decodeFirstCost", (int) mRecorderSet->decodeFirstVideoFrameInfo.getDecodeFirstFrameCost());
                videoDecodeInfo.addValue("firstSize", (int) mRecorderSet->decodeFirstVideoFrameInfo.firstPacketSize);
                videoDecodeInfo.addValue("firstPts", (double) mRecorderSet->decodeFirstVideoFrameInfo.firstPacketPts);
                decodeInfos.addJSON(videoDecodeInfo);
            }

            return decodeInfos.printJSON();
        }
        case PROPERTY_KEY_HLS_KEY_URL: {
            std::lock_guard<std::mutex> uMutex(mCreateMutex);
            if (nullptr != mDemuxerService) {
                return mDemuxerService->GetProperty(0, "keyUrl");
            }

            return "";
        }
        case PROPERTY_KEY_VIDEO_DROPPED_INFO: {
            char dropInfo[MAX_OPT_VALUE_LENGTH] = {0};
            GetOption("videoDroppedInfo", dropInfo);
            return dropInfo;
        }
        case PROPERTY_KEY_NETWORK_SPEED: {
            int64_t from = param.getInt64("from", -1);
            int64_t to = param.getInt64("to", -1);

            std::map<int64_t, int64_t> speeds = mMPAUtil->getNetworkSpeed(from, to);
            CicadaJSONItem value{};
            for (auto &item : speeds) {
                value.addValue(AfString::to_string(item.first), AfString::to_string((int) (item.second / 1024)));
            }

            return value.printJSON();
        }
        case PROPERTY_KEY_BUFFER_INFO: {
            int64_t from = param.getInt64("from", -1);
            int64_t to = param.getInt64("to", -1);
            std::map<int64_t, std::string> bufferInfo = mMPAUtil->getBufferInfo(from, to);
            CicadaJSONItem value{};
            for (auto &item : bufferInfo) {
                value.addValue(AfString::to_string(item.first), item.second);
            }

            return value.printJSON();
        }
        case PROPERTY_KEY_NETWORK_REQUEST_LIST: {
            int64_t from = param.getInt64("from", -1);
            int64_t to = param.getInt64("to", -1);
            return mMPAUtil->getNetworkRequestInfos(from, to);
        }
        case PROPERTY_KEY_RENDER_INFO: {
            return mMPAUtil->getRenderInfoAndReset();
        }
        case PROPERTY_KEY_CONTAINER_INFO: {
            return mContainerInfo;
        }
        default:
            break;
    }

    return "";
}

int SuperMediaPlayer::getCurrentStreamMeta(Stream_meta *meta, StreamType type)
{
    int streamIndex = -1;

    switch (type) {
        case ST_TYPE_VIDEO:
            streamIndex = mCurrentVideoIndex;
            break;

        case ST_TYPE_AUDIO:
            streamIndex = mCurrentAudioIndex;
            break;

        case ST_TYPE_SUB:
            streamIndex = mCurrentSubtitleIndex;
            break;

        default:
            return -EINVAL;
    }

    std::unique_lock<std::mutex> uMutex(mCreateMutex);

    if (streamIndex < 0 || mDemuxerService == nullptr) {
        return -EINVAL;
    }

    return mDemuxerService->GetStreamMeta(meta, streamIndex, false);
}

void SuperMediaPlayer::reLoad()
{
    mSourceListener->enableRetry();
    std::lock_guard<std::mutex> uMutex(mCreateMutex);
    if (mDemuxerService && mDemuxerService->getDemuxerHandle()) {
        mDemuxerService->getDemuxerHandle()->Reload();
    }
}

IVideoRender::Scale SuperMediaPlayer::convertScaleMode(ScaleMode mode)
{
    if (mode == ScaleMode::SM_CROP) {
        return IVideoRender::Scale::Scale_AspectFill;
    } else if (mode == ScaleMode::SM_FIT) {
        return IVideoRender::Scale::Scale_AspectFit;
    } else {
        return IVideoRender::Scale::Scale_Fill;
    }
}

IVideoRender::Rotate SuperMediaPlayer::convertRotateMode(RotateMode mode)
{
    if (mode == RotateMode::ROTATE_MODE_0) {
        return IVideoRender::Rotate::Rotate_None;
    } else if (mode == RotateMode::ROTATE_MODE_90) {
        return IVideoRender::Rotate::Rotate_90;
    } else if (mode == RotateMode::ROTATE_MODE_180) {
        return IVideoRender::Rotate::Rotate_180;
    } else if (mode == RotateMode::ROTATE_MODE_270) {
        return IVideoRender::Rotate::Rotate_270;
    } else {
        return IVideoRender::Rotate::Rotate_None;
    }
}


IVideoRender::Flip SuperMediaPlayer::convertMirrorMode(MirrorMode mode)
{
    switch (mode) {
        case MirrorMode::MIRROR_MODE_HORIZONTAL:
            return IVideoRender::Flip::Flip_Horizontal;
        case MirrorMode::MIRROR_MODE_VERTICAL:
            return IVideoRender::Flip::Flip_Vertical;
        default:
            return IVideoRender::Flip::Flip_None;
    }
}

float SuperMediaPlayer::GetVideoRenderFps()
{
    if (mAVDeviceManager->isVideoRenderValid()) {
        return mAVDeviceManager->getVideoRender()->getRenderFPS();
    }

    return mUtil->getVideoRenderFps();
}


float SuperMediaPlayer::GetVideoDecodeFps()
{
    return 0.0f;
}

void SuperMediaPlayer::NotifyError(int code)
{
    ChangePlayerStatus(PLAYER_ERROR);

    if (mErrorConverter) {
        int newErrorCode;
        std::string outStr;
        int processed = mErrorConverter->ConvertErrorCode(code, newErrorCode, outStr);

        if (processed) {
            AF_LOGE("Player ConvertErrorCode 0x%08x :%s\n", newErrorCode, outStr.c_str());
            mPNotifier->NotifyError(newErrorCode, outStr.c_str());
            return;
        }
    }

    int newErrorCode = framework_error2_code(code);
    char errbuf[128] = {0};
    int isFfmpegError = -1;

    if (newErrorCode == MEDIA_PLAYER_ERROR_UNKNOWN) {
        //maybe ffmpeg error code,,, try get ffmpeg error msg.
        isFfmpegError = get_ffmpeg_error_message(code, errbuf, 128);
    }

    if (isFfmpegError == 0) {
        AF_LOGE("Player ReadPacket ffmpeg error ?? 0x%04x :%s\n", -code, errbuf);
        mPNotifier->NotifyError(MEDIA_PLAYER_ERROR_DEMUXER_OPENSTREAM, errbuf);
    } else {
        char *desc = const_cast<char *>(framework_err2_string(code));
        mPNotifier->NotifyError(newErrorCode, desc);
    }
}

int SuperMediaPlayer::updateLoopGap()
{
    switch (mPlayStatus.load()) {
        case PLAYER_PREPARINIT:
        case PLAYER_PREPARING:
        case PLAYER_PREPARED:
            return 3;

        case PLAYER_PLAYING:
            if (!mFirstRendered) {
                return 3;
            } else if (HAVE_VIDEO) {
                if (mCurrentVideoMeta) {
                    // the loop gap can't too low
                    int fps = std::max(25, (int) (mCurrentVideoMeta->operator Stream_meta *()->avg_fps));
                    if (mVideoInterlaced == InterlacedType_YES) {
                        fps *= 2;
                    }
                    {
#ifdef ENABLE_VIDEO_FILTER
                        std::lock_guard<std::mutex> filterLock(mFilterManagerMutex);
                        if (mFilterManager != nullptr && !mFilterManager->isInvalid(IVideoFilter::Feature::Buffer, "vfi")) {
                            fps *= 2;
                        }
#endif
                    }
                    return 1000 / int((float) fps * mSet->rate * 1.5);
                }
            }
            return 1000 / int(50 * mSet->rate);

        default:
            return 40;
    }
}

int SuperMediaPlayer::mainService()
{

    if (mCanceled) {
        mMainServiceCanceled = true;
        return 0;
    } else {
        mMainServiceCanceled = false;
    }
    int64_t curTime = af_gettime_relative();
    mUtil->notifyPlayerLoop(curTime);
    sendDCAMessage();

    if (mMessageControl->empty() || (0 == mMessageControl->processMsg())) {
        ProcessVideoLoop();
        int loopGap = updateLoopGap();
        int64_t use = (af_gettime_relative() - curTime) / 1000;
        int64_t needWait = loopGap - use;
        // AF_LOGD("use :%lld, needWait:%lld", use, needWait);
        // FIXME : refactor it after made sure the goal of this logic
        if (needWait <= 0) {
            if (loopGap < 5) {
                needWait = 2;
            } else {
                return 0;
            }
        }
        const bool inSeekWindow = (mSeekFlag || mDiscontinuity.filterActive.load());
        if (inSeekWindow || mVideoCatchingUp) {
            if (needWait > 2) {
                needWait = 2;
            }
        }

        std::unique_lock<std::mutex> uMutex(mSleepMutex);
        mPlayerCondition.wait_for(uMutex, std::chrono::milliseconds(needWait), [this]() { return this->mCanceled.load(); });
    }

    return 0;
}

void SuperMediaPlayer::sendDCAMessage()
{
    string event = mDcaManager->getEvent();
    while (!event.empty()) {
        mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_DIRECT_COMPONENT_MSG, event.c_str());
        event = mDcaManager->getEvent();
    }
}

void SuperMediaPlayer::ProcessVideoLoop()
{
    int64_t curTime = af_gettime_relative() / 1000;
    {
#ifndef NDEBUG
        int streamIds[] = {mCurrentVideoIndex,         mCurrentAudioIndex,           mCurrentSubtitleIndex,
                           mVideoSwitchTargetIndex,    mWillChangedAudioStreamIndex, mWillChangedSubtitleStreamIndex};
        int size = sizeof(streamIds) / sizeof(streamIds[0]);

        for (int i = 0; i < size; ++i) {
            for (int j = 0; j < size; ++j) {
                const bool allowedPair = (i == 0 && j == 3) || (i == 3 && j == 0);

                if (i != j && streamIds[i] >= 0 && !allowedPair) {
                    assert(streamIds[i] != streamIds[j]);
                }
            }
        }

#endif
    }

    if (mSubPlayer) {
        mSubPlayer->onNoop();
    }

    if ((PLAYER_COMPLETION != mPlayStatus && (mPlayStatus < PLAYER_PREPARING || mPlayStatus > PLAYER_PAUSED)) ||
        nullptr == mDemuxerService) {// not working
        if (curTime - mTimerLatestTime > mTimerInterval) {
            OnTimer(curTime);
            mTimerLatestTime = curTime;
        }

        return;
    }
    logSeekPipelineState("loop");

    doReadPacket();
    doDeCode();

    // audio render will create after get a frame from decoder

    if (mCanceled) {
        return;
    }
    setUpAVPath();
    const bool bufferPass = DoCheckBufferPass();
    if (!bufferPass) {
        if (!(mSeekFlag && HAVE_VIDEO)) {
            return;
        }
    }

    if (!mBRendingStart && mPlayStatus == PLAYER_PLAYING && !mBufferingFlag) {
        if ((mEof && (!HAVE_AUDIO || mAVDeviceManager->isAudioRenderValid()) && (!HAVE_VIDEO || mAVDeviceManager->isVideoRenderValid())) ||
            // render out the cache frame in renders
            ((!HAVE_VIDEO || !mVideoFrameQue.empty() || (APP_BACKGROUND == mAppStatus)) && (!HAVE_AUDIO || !mAudioFrameQue.empty()))) {
            startRendering(true);
        }
    }

    doRender();
    if (mUtcTimer) {
        if (mFirstRendered) {
            if (mCurrentFrameUtcTime <= 0) {
                AF_LOGW("wrong current frame utc time");
            }
        }

        if (mCurrentFrameUtcTime > 0 && mPlayStatus == PLAYER_PLAYING) {
            //      AF_LOGD("delayTime is utc timer is %lld mCurrentFrameUtcTime us %lld\n",mUtcTimer->get(),mCurrentFrameUtcTime.load());
            LiveTimeSync(mUtcTimer->get() - mCurrentFrameUtcTime);
        }
    } else if (mDuration == 0) {
        int64_t lastAudio = mBufferController->GetPacketLastPTS(BUFFER_TYPE_AUDIO);
        if ((lastAudio != INT64_MIN) && (mPlayedAudioPts != INT64_MIN)) {
            int64_t delayTime = lastAudio - mPlayedAudioPts;
            static int64_t lastT = af_getsteady_ms();

            if (af_getsteady_ms() - lastT > 1000) {
                lastT = af_getsteady_ms();
                AF_LOGD("lastAudio:%lld mPlayedAudioPts:%lld, delayTime:%lld", lastAudio, mPlayedAudioPts, delayTime);
            }
        }
    }
    checkEOS();
    updateBufferInfo(false);
    logSeekPipelineState("loop-end");

    curTime = af_gettime_relative() / 1000;

    if (curTime - mTimerLatestTime > mTimerInterval) {
        OnTimer(curTime);
        mTimerLatestTime = curTime;
    }
}

void SuperMediaPlayer::updateBufferInfo(bool force)
{
    int64_t videoBufferDuration = INT64_MIN;
    int64_t audioBufferDuration = INT64_MIN;

    if (HAVE_VIDEO) {
        videoBufferDuration = mBufferController->GetPacketDuration(BUFFER_TYPE_VIDEO);
        if (videoBufferDuration < 0) {
            videoBufferDuration =
                    mBufferController->GetPacketLastPTS(BUFFER_TYPE_VIDEO) - mBufferController->GetPacketPts(BUFFER_TYPE_VIDEO);
        }
    }

    if (HAVE_AUDIO) {
        audioBufferDuration = mBufferController->GetPacketDuration(BUFFER_TYPE_AUDIO);
    }

    mMPAUtil->updateBufferInfo(force, videoBufferDuration, audioBufferDuration);
}

void SuperMediaPlayer::doReadPacket()
{
    //check packet queue full
    int64_t cur_buffer_duration = getPlayerBufferDuration(false, false);
    //100s
    mUtil->notifyRead(MediaPlayerUtil::readEvent_Loop, 0);

    if (mEof) {
        return;
    }

    //demuxer read
    int64_t read_start_time = af_gettime_relative();
    int timeout = 10000;
    mem_info info{};
    int checkStep = 0;
    while (true) {
        if (HAVE_VIDEO && mPlayStatus == PLAYER_PLAYING && !mBufferingFlag &&
            !mSeekFlag && !mSeekNeedCatch &&
            (!HAVE_AUDIO || mBufferController->GetPacketDuration(BUFFER_TYPE_AUDIO) > 1000 * 1000)) {
            const int64_t frontPts = mBufferController->GetPacketPts(BUFFER_TYPE_VIDEO);
            const int64_t masterPts = mMasterClock.GetTime(); 
            const int64_t queuedDurUs = mBufferController->GetPacketDuration(BUFFER_TYPE_VIDEO);
            const bool switchInFlight = mVideoSwitchInFlight;
            const bool queuedEnough = queuedDurUs >= 2 * MAX_VIDEO_READ_AHEAD_US;
            const bool audioEnough = !HAVE_AUDIO ||
                                     mBufferController->GetPacketDuration(BUFFER_TYPE_AUDIO) > PENDING_CAP_AUDIO_MIN_US;
            const int64_t frontNormalized =
                    (mActiveVideoPtsOffset != INT64_MIN && frontPts != INT64_MIN)
                    ? frontPts + mActiveVideoPtsOffset : frontPts;
            if (queuedEnough && audioEnough &&
                frontNormalized != INT64_MIN && masterPts > 0 &&
                frontNormalized - masterPts > MAX_VIDEO_READ_AHEAD_US) {
                if (floodLogAllowed(FLOOD_READ_AHEAD, 1, "read-ahead gate: video packet queue front is ahead of the clock")) {
                    AF_LOGW("read-ahead gate: video packet queue front %lld is %lld ms ahead of master %lld "
                            "(activeQ=%d queued=%lld ms, offset=%lld us → normalized front=%lld)\n",
                            (long long) frontPts, (long long) ((frontNormalized - masterPts) / 1000),
                            (long long) masterPts, (int) mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO),
                            (long long) (queuedDurUs / 1000),
                            (long long) (mActiveVideoPtsOffset == INT64_MIN ? 0 : mActiveVideoPtsOffset),
                            (long long) frontNormalized);
                }
                break;
            }
            if (mVideoSwitchInFlight && mDemuxerService != nullptr) {
                mDemuxerService->SetOption("alignedSwitchPlayheadUs", getCurrentPosition());
            }
        }
        if (mBufferIsFull) {
            static const int BufferGap = 1000 * 1000;

            if ((mSet->maxBufferDuration > 2 * BufferGap) && (cur_buffer_duration > mSet->maxBufferDuration - BufferGap) &&
                getPlayerBufferDuration(false, true) > mSet->startBufferDuration) {
                break;
            }
        }

        if (cur_buffer_duration > mSet->maxBufferDuration &&
            getPlayerBufferDuration(false, true) > mSet->startBufferDuration
            // we need readout the buffer in demuxer when no buffer in player, player keep at least start buffer duration
        ) {
            mBufferIsFull = true;
            break;
        }

        mBufferIsFull = false;

        if ((0 >= checkStep--) && (cur_buffer_duration > 1000 * 1000) && (AFGetSystemMemInfo(&info) >= 0)) {
            //AF_LOGD("system_availableram is %" PRIu64 "",info.system_availableram);
            if (info.system_availableram > 2 * mSet->lowMemSize) {
                checkStep = (int) (info.system_availableram / (5 * 1024 * 1024));
            } else if (info.system_availableram < mSet->lowMemSize) {
                AF_LOGW("low memery...");

                if (!mLowMem) {
                    mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_SYSTEM_LOW_MEMORY, "App Low memory");
                }

                mLowMem = true;

                if (mSet->highLevelBufferDuration > 800 * 1000) {
                    mSet->highLevelBufferDuration = 800 * 1000;
                }

                if (mSet->startBufferDuration > 800 * 1000) {
                    mSet->startBufferDuration = 800 * 1000;
                }

                break;
            } else {
                checkStep = 5;
                mLowMem = false;
            }
        }

        int ret = ReadPacket();

        if (ret == -EAGAIN) {
            if (0 == mDuration) {
                mRemainLiveSegment = mDemuxerService->GetRemainSegmentCount(mCurrentVideoIndex);
            }

            mUtil->notifyRead(MediaPlayerUtil::readEvent_Again, 0);
            break;
        } else if (ret == 0) {
            AF_LOGE("Player ReadPacket EOF");

            /*
             * 切换在途时旧档先读到 EOF：说明边界块没能执行（旧档没跨过分片末尾、目标档也没被定位）。
             * 这时必须收口成失败并回退 —— 留着在途状态会让同一目标档的后续点击被忽略、seek 被永久挂起
             * （真机表现是"一直切换中、只能重启"）。判据是状态（EOF + 在途），不是计时器。
             */
            if (mVideoSwitchInFlight) {
                AF_LOGW("quality switch FAILED: the demuxer hit EOF while the aligned switch to stream %d was "
                        "still in flight (the old stream ended before the boundary) — rolling back\n",
                        mVideoSwitchTargetIndex);
                finishQualitySwitch(false, "the old stream hit EOF before the switch boundary");
            }

            /*
             * 是不是"真的播完"由解复用层说了算：
             *   · 清单型（HLS/DASH/Manifest）：问它这一路还剩多少分片
             *       >0 ⇒ 还有内容没取到 ⇒ 暂时没数据；
             *       ==0 ⇒ 清单已到底 ⇒ 真播完；
             *   · 单文件型（本地文件 / 网络 mp4）：没有分片记账（返回 -1），问它"AVIO 是否真的读到
             *     范围末尾"（avFormatDemuxer::isDataExhausted()：av_read_frame 报 AVERROR_EOF 且
             *     pb->error 既非 EAGAIN 也非负数）。传输失败时它是 false ⇒ 暂时没数据，
             *     不会把断网当成播完（否则 mEof 一置，读循环再也不读，只能 seek/重启）。
             */
            const int remainSegments = (mDemuxerService != nullptr)
                                       ? mDemuxerService->GetRemainSegmentCount(mCurrentVideoIndex)
                                       : -1;
            const bool demuxerReportsComplete =
                    (remainSegments == 0) ||
                    (remainSegments < 0 && mDemuxerService != nullptr && mDemuxerService->isDataExhausted());

            if (!demuxerReportsComplete) {
                AF_LOGW("Player ReadPacket returned EOF but the demuxer has NOT reached the end "
                        "(remainSegments=%d, pos=%lld, duration=%lld) — treating it as a temporary stall; "
                        "the read path stays alive so playback resumes when the network is back\n",
                        remainSegments, (long long) getCurrentPosition(), (long long) mDuration);
                mUtil->notifyRead(MediaPlayerUtil::readEvent_Again, 0);
                break;
            }

            if (!mEof) {
#ifdef ENABLE_CACHE_MODULE
                if (std::shared_ptr<CacheManager> cacheManager = cacheManagerOrNull()) {
                    cacheManager->complete();
                }
#endif
                mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_DEMUXER_EOF, "Demuxer End of File");
            }

            if (mPlayStatus == PLAYER_PREPARING) {
                if (HAVE_VIDEO && !mHaveVideoPkt) {
                    closeVideo();
                }
                if (HAVE_AUDIO && !mHaveAudioPkt) {
                    closeAudio();
                }
            }

            mEof = true;
            break;
        } else if (ret == FRAMEWORK_ERR_EXIT) {
            AF_LOGE("Player ReadPacket error 0x%04x :%s\n", -ret, framework_err2_string(ret));
            break;
        } else if (ret == FRAMEWORK_ERR_FORMAT_NOT_SUPPORT) {
            AF_LOGE("read error %s\n", framework_err2_string(ret));
            NotifyError(ret);
            break;
        } else if (ret < 0) {
            if (!mBufferingFlag && mPlayStatus >= PLAYER_PREPARED) {
                //AF_LOGI("Player ReadPacket ret < 0 with data");
            } else {
                AF_LOGE("Player ReadPacket error 0x%04x :%s\n", -ret, framework_err2_string(ret));

                if (ret != FRAMEWORK_ERR_EXIT && !mCanceled) {
                    NotifyError(ret);
                }
            }

            break;
        }

        //AF_LOGI("Player ReadPacket have data");
        if (0 >= mFirstReadPacketSucMS) {
            mFirstReadPacketSucMS = af_getsteady_ms();
        }

        if (af_gettime_relative() - read_start_time > timeout) {
            AF_LOGD("Player ReadPacket time out\n");
            mUtil->notifyRead(MediaPlayerUtil::readEvent_timeOut, 0);
            //                    mMsgProcessTime = 0;
            break;
        }

        cur_buffer_duration = getPlayerBufferDuration(false, false);
        //                if(getPlayerBufferDuration(true) > mSet->maxBufferDuration * 2){
        //                    AF_LOGE("buffer stuffed\n");
        //                    mPNotifier->NotifyError(MEDIA_PLAYER_ERROR_BUFFER_STUFFED,"buffer stuffed");
        //                    break;
        //                }
    }
}

void SuperMediaPlayer::OnDemuxerCallback(const std::string &key, const std::string &value)
{}

bool SuperMediaPlayer::DoCheckBufferPass()
{
    int64_t cur_buffer_duration = getPlayerBufferDuration(false, false);
    int64_t HighBufferDur = mSet->highLevelBufferDuration;

    if (mEof) {
        mDemuxerService->getDemuxerHandle()->setClientBufferLevel(client_buffer_level_normal);
    } else {
        if (cur_buffer_duration < mSet->highLevelBufferDuration) {
            if (mPlayStatus == PLAYER_PLAYING) {
                mDemuxerService->getDemuxerHandle()->setClientBufferLevel(client_buffer_level_low);
            } else {
                mDemuxerService->getDemuxerHandle()->setClientBufferLevel(client_buffer_level_normal);
            }
        } else if (cur_buffer_duration >= mSet->maxBufferDuration - 3 * 1000 * 1000) {
            mDemuxerService->getDemuxerHandle()->setClientBufferLevel(client_buffer_level_low_full);
        } else if (cur_buffer_duration > 2 * mSet->highLevelBufferDuration) {
            mDemuxerService->getDemuxerHandle()->setClientBufferLevel(client_buffer_level_normal);
        } else {
            // TODO:
        }
    }

    if (mFirstBufferFlag && !mEof) {
        HighBufferDur = mSet->startBufferDuration;

        //clean late audio data
        if (cur_buffer_duration > HighBufferDur && HAVE_VIDEO && HAVE_AUDIO) {
            const int64_t cleanBeforeUs =
                    (mDiscontinuity.filterActive.load() && mDiscontinuity.targetUs != INT64_MIN)
                    ? mDiscontinuity.targetUs : mSoughtVideoPos;

            if (cleanBeforeUs > 0) {
                int64_t count = mBufferController->ClearPacketBeforeTimePos(BUFFER_TYPE_AUDIO, cleanBeforeUs);
                if (count > 0) {
                    AF_LOGW("clean late audio data %lld before %lld (seek-window target-point threshold)\n",
                            count, (long long) cleanBeforeUs);
                }
                int64_t pts = mBufferController->GetPacketPts(BUFFER_TYPE_AUDIO);

                if (mRemovedFirstAudioPts == INT64_MIN) {
                    mRemovedFirstAudioPts = pts;

                    if (mFirstAudioPts == INT64_MIN) {
                        mFirstAudioPts = pts - mFirstSeekStartTime;
                    }
                }

                cur_buffer_duration = getPlayerBufferDuration(false, false);

                if (cur_buffer_duration < HighBufferDur) {
                    return false;
                }
            }
        }
    }

    bool isTimeSync = false;
    if (mDemuxerService != nullptr) {
        isTimeSync = mDemuxerService->isWallclockTimeSyncStream(mCurrentVideoIndex);
    }

    if (mPlayStatus == PLAYER_PREPARING) {

        if (isTimeSync) {
            int currentStreamIndex = mCurrentAudioIndex;
            if (!HAVE_AUDIO) {
                currentStreamIndex = mCurrentVideoIndex;
            }
            int64_t durationToStart = mDemuxerService->getDurationToStartStream(currentStreamIndex);
            if (durationToStart < 0) {
                mPNotifier->NotifyPosition(durationToStart / 1000);
                return false;
            }
        }

        if ((cur_buffer_duration >= HighBufferDur &&
             (!HAVE_VIDEO || !mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO) || videoDecoderFull ||
              APP_BACKGROUND == mAppStatus || !mSet->mFastStart)) ||
            (mEof)) {
            if (mEof && getPlayerBufferDuration(true, false) <= 0) {
                // If player don`t get any packets when read eof
                if (mSeekPos > 0) {
                    updateBufferInfo(true);
                    //If caused by before prepare seeked, treat as play completed.
                    mPNotifier->NotifyLoading(loading_event_end, 0);
                    playCompleted();
                } else {
                    ChangePlayerStatus(PLAYER_ERROR);
                    mPNotifier->NotifyError(MEDIA_PLAYER_ERROR_DEMUXER_OPENSTREAM, "open stream failed");
                }
            } else {
                ChangePlayerStatus(PLAYER_PREPARED);
                if (mDemuxerService->isWallclockTimeSyncStream(mCurrentVideoIndex)) {
                    mUtcTimer = mDemuxerService->getDemuxerHandle()->getUTCTimer();
                }
                mDemuxerService->getDemuxerHandle()->SetOption("preferAudio", mSet->preferAudio);
                AF_LOGD("PLAYER_PREPARED");
                AF_LOGD("prepare use %lld ms\n", (af_gettime_relative() - mPrepareStartTime) / 1000);
                notifyPreparedCallback();

                if (mFirstBufferFlag) {
                    mFirstBufferFlag = false;
                }
            }
        } else {

            int64_t duration_v = -1;
            int64_t duration_a = -1;

            if (HAVE_VIDEO) {
                duration_v = mBufferController->GetPacketDuration(BUFFER_TYPE_VIDEO);
            }
            if (HAVE_AUDIO) {
                duration_a = mBufferController->GetPacketDuration(BUFFER_TYPE_AUDIO);
            }

            /**
             * if meta has audio and video stream infos , but after read 2 minutes duration ,
             * one of streams still has no buffer duration , close it to avoid read all packets.
             */
            if (std::min(duration_v, duration_a) == 0 && std::max(duration_v, duration_a) > 2 * 60 * 1000000) {
                if (duration_v > duration_a) {
                    if (mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_AUDIO)) {
                        closeAudio();
                    }
                } else {
                    if (mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)) {
                        closeVideo();
                    }
                }
            }
        }
    }
    if (HAVE_VIDEO && !videoDecoderEOS && !mEof && mPlayStatus == PLAYER_PLAYING &&
        mAVDeviceManager != nullptr) {
        const bool videoNothingToShow =
            mVideoFrameQue.empty() &&
            mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO) == 0 &&
            (!mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO) ||
             mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)->getInputPaddingSize() == 0);

        if (videoNothingToShow) {
            mVideoStarveIters++;
        } else {
            mVideoStarveIters = 0;
        }

        if (mVideoStarveIters >= VIDEO_STARVE_ITERS && cur_buffer_duration > 0) {
            bool audioAlsoDry = true;

            if (HAVE_AUDIO) {
                const bool audioDecoderDrained =
                    !mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_AUDIO) ||
                    mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_AUDIO)->getInputPaddingSize() == 0;

                audioAlsoDry = mAudioFrameQue.empty() &&
                               mBufferController->GetPacketSize(BUFFER_TYPE_AUDIO) == 0 &&
                               mAVDeviceManager->getAudioRenderQueDuration() == 0 &&
                               audioDecoderDrained;
            }

            if (!audioAlsoDry) {
                if (mVideoStarveIters == VIDEO_STARVE_ITERS) {
                    AF_LOGW("video path has nothing to show but the audio path still has data — NOT pausing the "
                            "clock/render (a video-only gap must never cut the sound); audio frameQ=%d "
                            "packetQ=%d renderQueDuration=%lld\n",
                            (int) mAudioFrameQue.size(),
                            (int) mBufferController->GetPacketSize(BUFFER_TYPE_AUDIO),
                            (long long) mAVDeviceManager->getAudioRenderQueDuration());
                }
            } else {
                if (mVideoStarveIters == VIDEO_STARVE_ITERS) {
                    AF_LOGW("video path has nothing to show and the audio path is dry too (frameQ=0, packetQ(v)=0, "
                            "inputPadding=0 for %d loops) — treating it as an empty cache: pausing the clock and "
                            "the audio render\n",
                            (int) VIDEO_STARVE_ITERS);
                }
                cur_buffer_duration = 0;
            }
        }
    } else {
        mVideoStarveIters = 0;
    }

    //check buffering empty
    if (!mEof && cur_buffer_duration <= 0 && !mBufferingFlag && (mPlayStatus == PLAYER_PLAYING || mPlayStatus == PLAYER_PAUSED)) {
        mBufferingFlag = true;
        if (!mSeekFlag) {
            mPNotifier->NotifyLoading(loading_event_start, 0);
            mBufferingNotified = true;
            AF_LOGD("loading start");
        } else {
            mBufferingNotified = false;
            AF_LOGD("seek in progress, parking clock at %lld without showing the buffering UI\n",
                    (long long) mMasterClock.GetTime());
        }
        mLoadingProcess = 0;
        mTimeoutStartTime = INT64_MIN;
        mMasterClock.pause();
        if (!mSeekFlag) {
            mAVDeviceManager->pauseAudioRender(true);
        } else {
            AF_LOGI("seek buffering: the audio device is deliberately NOT paused (it was already flushed by "
                    "the seek; pausing it again made the track do pause/flush/start twice per seek and the "
                    "system then reported onAudioException -1003/-1004)\n");
        }
        logAudioSilence(AUDIO_SILENCE_BUFFERING,
                        "buffering: the clock and the audio render are paused (empty cache)",
                        mPlayedAudioPts, mMasterClock.GetTime());
        return false;
    }

    //AF_LOGD("current duration is %lld,video duration is %lld,audio duration is %lld", cur_buffer_duration
    //	,mBufferController->GetPacketDuration(BUFFER_TYPE_VIDEO), mBufferController->GetPacketDuration(BUFFER_TYPE_AUDIO));
    bool isRealTime = false;
    if (mDemuxerService != nullptr) {
        isRealTime = mDemuxerService->isRealTimeStream(mCurrentVideoIndex);
    }

    while (isRealTime && mSet->RTMaxDelayTime > 0) {
        if (!HAVE_AUDIO) {
            int64_t maxBufferDuration = getPlayerBufferDuration(true, false);

            if (maxBufferDuration > mSet->RTMaxDelayTime + 1000 * 1000 * 5) {
                int64_t lastKeyPos = mBufferController->GetPacketLastKeyTimePos(BUFFER_TYPE_VIDEO);
                mBufferController->ClearPacketBeforeTimePos(BUFFER_TYPE_VIDEO, lastKeyPos);
                break;
            }

            LiveCatchUp(maxBufferDuration);
            break;
        }

        int64_t maxBufferDuration = getPlayerBufferDuration(true, false);

        if (maxBufferDuration > mSet->RTMaxDelayTime + 1000 * 1000 * 5) {
            //drop frame
            int64_t lastVideoPos = mBufferController->GetPacketLastTimePos(BUFFER_TYPE_VIDEO);
            int64_t lastAudioPos = mBufferController->GetPacketLastTimePos(BUFFER_TYPE_AUDIO);
            int64_t lastAudioPts = mBufferController->GetPacketLastPTS(BUFFER_TYPE_AUDIO);
            int64_t lastPos;

            if (lastVideoPos == INT64_MIN) {
                lastPos = lastAudioPos;
            } else if (lastAudioPos == INT64_MIN) {
                lastPos = lastVideoPos;
            } else {
                lastPos = lastAudioPos < lastVideoPos ? lastAudioPos : lastVideoPos;
            }

            lastPos -= min(mSet->RTMaxDelayTime, 500 * 1000);
            int64_t lastVideoKeyTimePos = mBufferController->GetKeyTimePositionBefore(BUFFER_TYPE_VIDEO, lastPos);
            int videoPacketCount = mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO);

            if (videoPacketCount == 0 || lastVideoKeyTimePos != INT64_MIN) {
                AF_LOGD("drop left lastPts %lld, lastVideoKeyPts %lld", lastPos, lastVideoKeyTimePos);
                mMsgCtrlListener->ProcessSetSpeed(1.0);

                int64_t clearPos = (lastVideoKeyTimePos != INT64_MIN) ? lastVideoKeyTimePos : lastPos;
                int64_t dropVideoCount = mBufferController->ClearPacketBeforeTimePos(BUFFER_TYPE_VIDEO, clearPos);
                int64_t dropAudioCount = mBufferController->ClearPacketBeforeTimePos(BUFFER_TYPE_AUDIO, clearPos);

                if (dropVideoCount > 0) {
                    FlushVideoPath(true, true, __func__);
                    AF_LOGD("drop left video duration is %lld,left video size is %d",
                            mBufferController->GetPacketDuration(BUFFER_TYPE_VIDEO), mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO));
                }

                if (dropAudioCount > 0) {
                    FlushAudioPath();
                    AF_LOGD("drop left aduio duration is %lld,left aduio size is %d",
                            mBufferController->GetPacketDuration(BUFFER_TYPE_AUDIO), mBufferController->GetPacketSize(BUFFER_TYPE_AUDIO));
                    mMasterClock.setTime(lastAudioPts);
                }
            }
        }

        int64_t lastAudio = mBufferController->GetPacketLastPTS(BUFFER_TYPE_AUDIO);

        if ((lastAudio != INT64_MIN) && (mPlayedAudioPts != INT64_MIN)) {
            int64_t delayTime = lastAudio - mPlayedAudioPts;
            static int64_t lastT = af_getsteady_ms();

            if (af_getsteady_ms() - lastT > 1000) {
                lastT = af_getsteady_ms();
                AF_LOGD("lastAudio:%lld mPlayedAudioPts:%lld, delayTime:%lld", lastAudio, mPlayedAudioPts, delayTime);
            }

            LiveCatchUp(delayTime);
        }

        break;
    }

    //check buffering status
    if ((mBufferingFlag || mFirstBufferFlag)) {
        if (((cur_buffer_duration > HighBufferDur || (HighBufferDur >= mSet->maxBufferDuration && mBufferIsFull)) &&
             (!HAVE_VIDEO || videoDecoderFull || APP_BACKGROUND == mAppStatus ||
              (HAVE_AUDIO && (!mAudioFrameQue.empty() ||
                              mBufferController->GetPacketSize(BUFFER_TYPE_AUDIO) > 0)))) ||
            mEof) {
            // if still in seek, wait for seek status be changed.
            if (!mSeekFlag || mEof) {
                if (mBufferingFlag) {
                    mLoadingProcess = -1;
                    updateBufferInfo(true);
                    if (mBufferingNotified) {
                        mPNotifier->NotifyLoading(loading_event_end, 0);
                        AF_LOGD("loading end");
                    } else {
                        AF_LOGD("seek buffering finished without a UI notification");
                    }

                    mBufferingNotified = false;

                    if (mPlayStatus == PLAYER_PLAYING) {
                        mMasterClock.start();
                        mAVDeviceManager->pauseAudioRender(false);
                        logAudioSilence(AUDIO_SILENCE_NONE,
                                        "buffering finished: the clock and the audio render are running again",
                                        mPlayedAudioPts, mMasterClock.GetTime());
                    }
                }

                mTimeoutStartTime = INT64_MIN;
                mFirstBufferFlag = false;
                mBufferingFlag = false;
            }
        } else if (cur_buffer_duration >= 0) {
            // TODO: Notify when change
            int prg = MIN(static_cast<int>(cur_buffer_duration * 100 / HighBufferDur), 100);

            if (prg > mLoadingProcess && mBufferingFlag) {
                mLoadingProcess = prg;
                mPNotifier->NotifyLoading(loading_event_progress, prg);
                mTimeoutStartTime = INT64_MIN;
            } else if (mLoadingProcess == prg) {//15seconds loading progress not changed
                int64_t curTime = af_gettime_relative() / 1000;

                if ((mTimeoutStartTime == INT64_MIN) || (PLAYER_PLAYING != mPlayStatus)) {
                    mTimeoutStartTime = curTime;
                } else if (curTime - mTimeoutStartTime >= mSet->timeout_ms) {
                    //TODO: demuxer still read crazy
                    if (mLowMem) {
                        mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_SYSTEM_LOW_MEMORY, "App Low memory");
                        // if it's a network error or waiting for network to retry, don't notify error
                    } else if (mSourceListener->isConnected() && !mSourceListener->isPending()) {
                        //    ChangePlayerStatus(PLAYER_ERROR);
                        //   notifyErrorCallback(MEDIA_PLAYER_ERROR_LOADING_TIMEOUT, "Loading timeout");
                    }

                    mTimeoutStartTime = curTime;
                }
            }
        }
    }

    return true;
}

void SuperMediaPlayer::closeAudio()
{
    AF_LOGW("close audio stream");
    mDemuxerService->CloseStream(mCurrentAudioIndex);
    mCurrentAudioIndex = -1;
    mMasterClock.setReferenceClock(nullptr, nullptr);
    //    mAudioFrameQue.clear();
    mBufferController->ClearPacket(BUFFER_TYPE_AUDIO);
    FlushAudioPath();
}

void SuperMediaPlayer::closeVideo()
{
    AF_LOGW("close video stream");
    mDemuxerService->CloseStream(mCurrentVideoIndex);
    mCurrentVideoIndex = -1;
    //  mVideoFrameQue.clear();
    mBufferController->ClearPacket(BUFFER_TYPE_VIDEO);
    FlushVideoPath(true, true, __func__);
}

void SuperMediaPlayer::LiveCatchUp(int64_t delayTime)
{
    int recoverGap = 50 * 1000;

    if (mSet->RTMaxDelayTime >= 1000 * 1000) {
        recoverGap = 500 * 1000;
    } else if (mSet->RTMaxDelayTime >= 200 * 1000) {
        recoverGap = 100 * 1000;
    }

    if ((delayTime > mSet->RTMaxDelayTime) && (150 * 1000 < delayTime)) {
        mMsgCtrlListener->ProcessSetSpeed(1.2);
    } else if ((delayTime < mSet->RTMaxDelayTime - recoverGap) || (100 * 1000 > delayTime)) {
        mMsgCtrlListener->ProcessSetSpeed(1.0);
    }
}

void SuperMediaPlayer::LiveTimeSync(int64_t delayTime)
{
    static int64_t lastT = af_getsteady_ms();
    if (af_getsteady_ms() - lastT > 1000) {
        lastT = af_getsteady_ms();
        AF_LOGD("delayTime is %lld rate is %f  buffer duration is %lld\n", delayTime, mBufferingFlag ? 0 : mSet->rate.load(),
                getPlayerBufferDuration(false, false));
    }
    int64_t maxGopTime = mDemuxerService->getDemuxerHandle()->getMaxGopTimeUs();
    if (maxGopTime <= 0) {
        maxGopTime = 2 * 1000 * 1000;
    }
    if (maxGopTime > mSuggestedPresentationDelay) {
        maxGopTime = mSuggestedPresentationDelay;
    }
    assert(mSuggestedPresentationDelay > 0);
    if (mSuggestedPresentationDelay <= 0) {
        return;
    }

    int64_t catchUpBufferDelta = std::max(mSet->highLevelBufferDuration, maxGopTime);

    if (delayTime > (mSuggestedPresentationDelay + 1000 * 1000 * 5) &&
        getPlayerBufferDuration(true, false) > (mSuggestedPresentationDelay + 1000 * 1000 * 5 + catchUpBufferDelta)) {
        //drop frame
        int64_t lateUTCTime = mUtcTimer->get() - (mSuggestedPresentationDelay + 1000 * 1000 * 5);
        int64_t lastKeyTimePos = mBufferController->GetKeyTimePositionBeforeUtcTime(BUFFER_TYPE_VIDEO, lateUTCTime);
        if (lastKeyTimePos == INT64_MIN) {
            lastKeyTimePos = mBufferController->GetKeyTimePositionBeforeUtcTime(BUFFER_TYPE_AUDIO, lateUTCTime);
        }

        // TODO: get the buffer duration after lateUTCTime, if buffer duration is too small, do not clear the buffers
        if (lastKeyTimePos != INT64_MIN) {
            AF_LOGD("drop left lateUTCTime %lld, lastVideoKeyPts %lld", lateUTCTime, lastKeyTimePos);
            int64_t dropVideoCount = mBufferController->ClearPacketBeforeTimePos(BUFFER_TYPE_VIDEO, lastKeyTimePos);
            int64_t dropAudioCount = mBufferController->ClearPacketBeforeTimePos(BUFFER_TYPE_AUDIO, lastKeyTimePos);

            if (dropVideoCount > 0) {
                FlushVideoPath(true, true, __func__);
                AF_LOGD("drop left video duration is %lld,left video size is %d", mBufferController->GetPacketDuration(BUFFER_TYPE_VIDEO),
                        mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO));
            }

            if (dropAudioCount > 0) {
                FlushAudioPath();
                AF_LOGD("drop left audio duration is %lld,left audio size is %d", mBufferController->GetPacketDuration(BUFFER_TYPE_AUDIO),
                        mBufferController->GetPacketSize(BUFFER_TYPE_AUDIO));
                mMasterClock.setTime(mBufferController->GetPacketPts(BUFFER_TYPE_AUDIO));
            }
        }
    }
    int64_t bufferDelay = mSuggestedPresentationDelay;

    switch (mLiveTimeSyncType) {

        case LiveTimeSyncType::LiveTimeSyncNormal:
            assert(mSet->rate == 1.0f);
            if ((delayTime > bufferDelay + maxGopTime / 2) && getPlayerBufferDuration(false, false) > catchUpBufferDelta) {
                mMsgCtrlListener->ProcessSetSpeed(1.2);
                mLiveTimeSyncType = LiveTimeSyncType::LiveTimeSyncCatchUp;
            } else if (delayTime < bufferDelay - maxGopTime / 2) {
                mMsgCtrlListener->ProcessSetSpeed(0.9);
                mLiveTimeSyncType = LiveTimeSyncType::LiveTimeSyncSlowDown;
                AF_LOGD("LiveTimeSync, delayTime=%lld, set speed 0.9", delayTime);
            }
            break;

        case LiveTimeSyncType::LiveTimeSyncCatchUp:
            //   AF_LOGD("CatchUp speed is %f\n", mSet->rate.load());
            assert(mSet->rate == 1.2f);
            if ((delayTime < bufferDelay) || (getPlayerBufferDuration(false, false) < catchUpBufferDelta)) {
                mMsgCtrlListener->ProcessSetSpeed(1.0);
                mLiveTimeSyncType = LiveTimeSyncType::LiveTimeSyncNormal;
                AF_LOGD("LiveTimeSync, delayTime=%lld, recover from catch up", delayTime);
            }
            break;
        case LiveTimeSyncType::LiveTimeSyncSlowDown:
            assert(mSet->rate == 0.9f);
            if (delayTime > bufferDelay) {
                mMsgCtrlListener->ProcessSetSpeed(1.0);
                mLiveTimeSyncType = LiveTimeSyncType::LiveTimeSyncNormal;
                AF_LOGD("LiveTimeSync, delayTime=%lld, recover from slow down", delayTime);
            }
            break;
    }
}

void SuperMediaPlayer::notifyPreparedCallback()
{
    if (waitingForStart && mSet->bLooping) {
        //when loop play don`t send prepare msg.
    } else {
        mPNotifier->NotifyPrepared();
    }

    if (waitingForStart || mAutoPlay) {
        Start();
        waitingForStart = false;
    }

    if (mAutoPlay) {
        mPNotifier->NotifyAutoPlayStart();
    }
}

/*
 * 洪水日志限频（见 SuperMediaPlayer.h 的 FloodLogId 注释）。
 *
 * 语义：每 FLOOD_LOG_WINDOW_MS 一个窗口，窗口内最多原样放行 perWindow 条；
 * 超出的丢进 dropped；窗口切换时（下一条日志进来时）先补一行汇总说明上一窗口
 * 压掉了多少条。这样“密度”和“总量”都还在，只是不再逐帧刷屏。
 */
bool SuperMediaPlayer::floodLogAllowed(FloodLogId id, int perWindow, const char *what)
{
    const int64_t now = af_getsteady_ms();
    FloodLogState &f = mFloodLog[id];

    if (f.windowStartMs == 0 || now - f.windowStartMs >= FLOOD_LOG_WINDOW_MS) {
        if (f.dropped > 0) {
            AF_LOGW("[flood] %s: %lld more lines suppressed in the previous second\n",
                    f.what != nullptr ? f.what : what, (long long) f.dropped);
        }

        f.windowStartMs = now;
        f.emitted = 0;
        f.dropped = 0;
        f.what = what;
    }

    if (f.emitted < perWindow) {
        ++f.emitted;
        return true;
    }

    ++f.dropped;
    return false;
}

void SuperMediaPlayer::finishQualitySwitch(bool ready, const char *reason)
{

    if (!mVideoSwitchInFlight) {
        return;
    }
    setVideoDecodeBoost(false);

    const int committedStream = mVideoSwitchTargetIndex;
    const int64_t committedPts = mPlayedVideoPts;

    if (ready && committedStream >= 0) {
        if (StreamInfo *info = GetCurrentStreamInfo(ST_TYPE_VIDEO)) {
            mPNotifier->NotifyStreamChanged(info, ST_TYPE_VIDEO);
        }
    }

    mPNotifier->NotifyVideoQualitySwitch(ready ? PLAYER_QUALITY_SWITCH_READY : PLAYER_QUALITY_SWITCH_FAILED,
                                         committedStream,
                                         reason != nullptr ? reason : (ready ? "quality switch rendered"
                                                                            : "quality switch finished"));


    AF_LOGI("finishQualitySwitch ready=%d stream=%d pts=%lld master=%lld reason=%s\n",
            (int) ready, committedStream, (long long) committedPts,
            (long long) mMasterClock.GetTime(), reason != nullptr ? reason : "-");
    if (mVideoSwitchOldStreamIndex >= 0 && mDemuxerService != nullptr) {
        AF_LOGI("quality switch: closing the old stream %d now (the new stream is already on screen)\n",
                mVideoSwitchOldStreamIndex);
        mDemuxerService->CloseStream(mVideoSwitchOldStreamIndex);
    }

    mVideoSwitchOldStreamIndex = -1;
    runDeferredPauseFrameRestore();
    replayDeferredUserSeek();
    mVideoSwitchInFlight = false;
}
void SuperMediaPlayer::logSeekPipelineState(const char *why)
{
    const int frameQueSize = (int) mVideoFrameQue.size();
    const bool vDecValid = (mAVDeviceManager != nullptr &&
                            mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO));
    const int playStatus = (int) mPlayStatus.load();
    if (mSeekFlag == mSeekDiagLastSeekFlag &&
        mSeekNeedCatch == mSeekDiagLastSeekNeedCatch &&
        mBufferingFlag == mSeekDiagLastBufferingFlag &&
        videoDecoderEOS == mSeekDiagLastVideoDecoderEOS &&
        playStatus == mSeekDiagLastPlayStatus &&
        vDecValid == mSeekDiagLastVideoDecoderValid) {
        return;
    }

    mSeekDiagLastSeekFlag = mSeekFlag;
    mSeekDiagLastSeekNeedCatch = mSeekNeedCatch;
    mSeekDiagLastBufferingFlag = mBufferingFlag;
    mSeekDiagLastVideoDecoderEOS = videoDecoderEOS;
    mSeekDiagLastPlayStatus = playStatus;
    mSeekDiagLastVideoDecoderValid = vDecValid;

    /*
     * 用 AF_LOGI：本次事故里能看到的日志下限就是 INFO（AF_LOGD 全被过滤了），
     * 这条日志的存在意义就是在"下一次只剩三条 INFO"时不至于又抓瞎。
     * 注意这里**只读**状态，不改任何标志，因此对播放语义零影响。
     */
    char masterText[32] = {0};
    const int64_t masterForDiag = mMasterClock.GetTime();

    if (af_clock_value_is_unset(masterForDiag)) {
        snprintf(masterText, sizeof(masterText), "unset");
    } else {
        snprintf(masterText, sizeof(masterText), "%lld us", (long long) masterForDiag);
    }

    AF_LOGI("[seekdiag] %s: seekFlag=%d seekNeedCatch=%d buffering=%d firstBuf=%d "
            "videoEOS=%d playStatus=%d frameQ=%d packetQ(v)=%d master=%s vDecValid=%d\n",
            why != nullptr ? why : "?",
            (int) mSeekFlag, (int) mSeekNeedCatch, (int) mBufferingFlag, (int) mFirstBufferFlag,
            (int) videoDecoderEOS, (int) mPlayStatus.load(), frameQueSize,
            mBufferController != nullptr ? (int) mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO) : -1,
            masterText,
            (int) (mAVDeviceManager != nullptr &&
                   mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)));
}
void SuperMediaPlayer::beginRendererJoining(const char *why)
{
    dropLateVideoFrames = true;
    mDropLateVideoFramesUntilMs = 0;
    if (!mVideoFrameQue.empty()) {
        const int64_t masterUs = mMasterClock.GetTime();
        int purged = 0;

        if (masterUs != INT64_MIN) {
            while (!mVideoFrameQue.empty() && mVideoFrameQue.front() != nullptr) {
                const int64_t framePts = mVideoFrameQue.front()->getInfo().pts;

                if (framePts != INT64_MIN && framePts + JOINING_STALE_FRAME_US < masterUs) {
                    /* mVideoFrameQue 是 std::queue：只有 pop()（弹队首），没有 pop_front()。 */
                    mVideoFrameQue.pop();
                    purged++;
                    continue;
                }

                break;
            }
        }

        if (purged > 0) {
            AF_LOGI("renderer joining (%s): convergent drop of %d already-late frame(s) before handing "
                    "anything to the renderer (master=%lld)\n",
                    why != nullptr ? why : "-", purged, (long long) masterUs);
        }
    }

    AF_LOGI("renderer joining started (%s): late frames will be dropped until a frame lands on the "
            "master clock (master=%lld)\n",
            why != nullptr ? why : "-", (long long) mMasterClock.GetTime());
}

void SuperMediaPlayer::doRender()
{
    bool rendered = false;

    if (mSeekFlag && mPlayStatus != PLAYER_PREPARING) {
        if (HAVE_VIDEO && mAppStatus != APP_BACKGROUND) {
            rendered = RenderVideo(true);

            if (rendered) {
                AF_LOGD("TIMEPOS RenderVideo :%lld", mPlayedVideoPts / 1000);

                /*
                 *  set the position to video position tmp, audio will update the position when it rendered,
                 *  otherwise the position will update to the old audio position when audio not reach on time after seek.
                 *
                 */
                if (mSoughtVideoPos != INT64_MIN) {
                    mCurrentPos = mSoughtVideoPos;
                } else {
                    mCurrentPos = mPlayedVideoPts;
                }
                NotifyPosition(getCurrentPosition());

                // seek preview can't render audio,but set the audio clock to here pts
                if (HAVE_AUDIO && (mAudioTime.startTime <= 0)) {
                    if (!mAudioFrameQue.empty()) {
                        mAudioTime.startTime = mAudioFrameQue.front()->getInfo().pts;
                    } else {
                        mAudioTime.startTime = mPlayedVideoPts;
                    }
                }
            }
        } else {// audio only
            if (!mAudioFrameQue.empty()) {
                NotifyPosition(mAudioFrameQue.front()->getInfo().timePosition);
                rendered = true;
                mCurrentPos = mAudioFrameQue.front()->getInfo().timePosition;
                mAudioTime.startTime = mAudioFrameQue.front()->getInfo().pts;
            }
        }
    }

    if (mPlayStatus == PLAYER_PLAYING) {
        if (!mBufferingFlag) {
            rendered |= render();
        }
    }
    if ((mSeekFlag || mDiscontinuity.filterActive.load()) &&
        mPlayStatus == PLAYER_PAUSED &&
        mAppStatus != APP_BACKGROUND && HAVE_VIDEO) {
        /*
         * 只交一帧：去重靠 RenderVideo() 内部的既有判据（与 seek 分支同一个用法），
         * **不动主时钟、不发位置通知**。
         */
        RenderVideo(true);
    }

    if (rendered) {
        mFirstBufferFlag = false;
        //may audio already played over
        if (mEof && mAudioFrameQue.empty() && mBufferController->GetPacketSize(BUFFER_TYPE_AUDIO) == 0) {
            mMasterClock.setReferenceClock(nullptr, nullptr);
        }

        if (mSeekFlag) {
            // 暂停帧恢复期间：渲染门命中前不能结束 seek —— 门会把目标帧
            // 之前的帧全部挡掉，若按第一帧就宣告 seek 完成，暂停态的
            // 读包/解码管线会停，目标帧永远解码不出来（黑屏根因）
            IDecoder *videoDecoder = mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);
            if (videoDecoder == nullptr || videoDecoder->isRenderGateHit()) {
                mSeekFlag = false;
                /*
                 * seek 完成（目标帧已经上屏）：解码器是从目标点之前的关键帧解过来的，
                 * 队列里可能还压着一段“早于 seek 目标/主时钟”的帧。开一个追赶窗口，
                 * 让这些迟到帧被丢掉、画面尽快和主时钟对齐 —— 否则 seek 之后视频会
                 * 永久落后一段（实测 seek 到 33.6s 后视频停在 37s 且再也不动）。
                 */
                beginRendererJoining("seek finished");

                if (!mMessageControl->findMsgByType(MSG_SEEKTO)) {
                    // update position when seek end. in case of when paused.
                    // update position before reset seek status, so getCurrentPosition return mSeekPos instead of mCurrentPos
                    // fix bug the mCurrentPos not accuracy
                    NotifyPosition(getCurrentPosition());
                    ResetSeekStatus();
                    mPNotifier->NotifySeekEnd(mSeekInCache);
                    mSeekInCache = false;
                }
            }
        }
    }
}

void SuperMediaPlayer::doDeCode()
{
    //get video packet to decode
    if (HAVE_VIDEO && !videoDecoderEOS && mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)) {
        int max_cache_size = VIDEO_PICTURE_MAX_CACHE_SIZE;

        if (mPictureCacheType == picture_cache_type_cannot) {
            max_cache_size = 1;
        }

        unsigned long videoFrameSize = mVideoFrameQue.size();

        if (videoFrameSize < max_cache_size) {
            int64_t startDecodeTime = af_getsteady_ms();
            int64_t videoEarlyUs = 0;

            do {
                if (mCanceled) {
                    break;
                }
                // if still in seeking, don't send data to decoder in background
                // due to the playback position could be changed later.
                if ((APP_BACKGROUND == mAppStatus) && isSeeking()) {
                    break;
                }

                if (mVideoPacket == nullptr) {
                    mVideoPacket = mBufferController->getPacket(BUFFER_TYPE_VIDEO);
                }

                /* 画面队列已满就停止向解码器送包，否则解码输出会无限堆积。 */
                if (mVideoFrameQue.size() >= max_cache_size) {
                    break;
                }

                videoEarlyUs = mVideoPacket ? mVideoPacket->getInfo().dts - mMasterClock.GetTime() : 0;

                // don't send too much data when in background
                if (mVideoPacket && APP_BACKGROUND == mAppStatus && videoEarlyUs > 0) {
                    break;
                }

                FillVideoFrame();

                if (nullptr == mVideoPacket && !mEof) {
                    break;
                }

                if (mVideoPacket && (!HAVE_AUDIO || audioDecoderEOS)) {
                    if (!(mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)->getFlags() & DECFLAG_PASSTHROUGH_INFO) &&
                        mDemuxerService->getDemuxerHandle()->isTSDiscontinue()) {
                        if (mVideoPacket->getInfo().timePosition >= 0) {
                            mCurrentPos = mVideoPacket->getInfo().timePosition;
                        }
                        if (mVideoPacket->getInfo().utcTime >= 0) {
                            mCurrentFrameUtcTime = mVideoPacket->getInfo().utcTime;
                        }
                        //printTimePosition(mCurrentPos);
                    }
                }

                int ret = 0;
                /*
                 * 旧 representation 的包不能再喂给当前解码器：换档当场已经按流丢弃过
                 * 公共队列里的旧包（见 SwitchVideo ③b），这里再兜一次 —— codec/分辨率
                 * 不同的 DASH/HLS 流混入一个旧包就会触发错误帧、PTS 回退和低帧率。
                 * 正常本地文件和未切换路径不受影响。
                 */
                if (mVideoPacket && mCurrentVideoIndex >= 0 &&
                    mVideoPacket->getInfo().streamIndex != mCurrentVideoIndex) {
                    if (floodLogAllowed(FLOOD_STALE_DROP, 2, "drop stale video packet after quality switch")) {
                        AF_LOGD("drop stale video packet after quality switch: stream=%d current=%d pts=%lld\n",
                                mVideoPacket->getInfo().streamIndex, mCurrentVideoIndex,
                                (long long) mVideoPacket->getInfo().pts);
                    }

                    mVideoPacket.reset();
                    continue;
                }
                ret = DecodeVideoPacket(mVideoPacket);

                if (ret & STATUS_RETRY_IN) {
                    /* 解码器输出队列满了：先 break，下一轮开头的 FillVideoFrame()
                     * 会把输出排空。 */
                    break;
                }

                /* 一次主循环里不能连续解码几百毫秒，否则渲染线程没有机会送帧；
                 * 统一保持短时间片。 */
                if (af_getsteady_ms() - startDecodeTime > 50) {
                    break;
                }
            } while ((mSeekNeedCatch || dropLateVideoFrames) && (videoEarlyUs < 200 * 1000));
        }
    }

    //get audio packet to decode
    if (HAVE_AUDIO && mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_AUDIO)) {

        /*
         * 音频解码队列下限按"帧数"给足抗抖动余量（主流做法：ffplay 的音频 PCM 队列是 9 帧）。
         * 原来只留 2 帧（约 40ms），4K 解码突发或网络抖动一来就欠载 —— 这也是
         * "seek 之后声音不稳 / 起播咔哒"的通用原因（见方案 2.1）。
         */
        while (mAudioFrameQue.size() < AUDIO_FRAME_QUEUE_FLOOR && !audioDecoderEOS && !mCanceled) {

            if (mAudioPacket == nullptr) {
                mAudioPacket = mBufferController->getPacket(BUFFER_TYPE_AUDIO);
            }

            if (mAudioPacket) {
                int64_t timePosition = mAudioPacket->getInfo().timePosition;
                int64_t utcTime = mAudioPacket->getInfo().utcTime;
                int ret = DecodeAudio(mAudioPacket);
                if (mAudioPacket == nullptr &&
                    !(mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_AUDIO)->getFlags() & DECFLAG_PASSTHROUGH_INFO)) {
                    if (timePosition >= 0) {
                        mCurrentPos = timePosition;
                    }
                    if (utcTime >= 0) {
                        mCurrentFrameUtcTime = utcTime;
                    }
                    //printTimePosition(mCurrentPos);
                }
                if (ret == -EAGAIN) {
                    break;
                }
            } else if (mEof) {
                unique_ptr<IAFPacket> packet{};
                DecodeAudio(packet);
            } else
                break;
        }

        //            AF_LOGD("mAudioFrameQue.size is %d\n", mAudioFrameQue.size());
    }
}

bool SuperMediaPlayer::checkEOSAudio()
{

    if (!HAVE_AUDIO) {
        return true;
    }
    if (!audioDecoderEOS) {
        return false;
    }
    int packetSize = mBufferController->GetPacketSize(BUFFER_TYPE_AUDIO);
    int frameSize = static_cast<int>(mAudioFrameQue.size());
    if (frameSize > 0 || packetSize > 0) {
        AF_TRACE;
        return false;
    }

    uint64_t audioQueDuration = mAVDeviceManager->getAudioRenderQueDuration();

    if (audioQueDuration != 0) {
        AF_TRACE;
        //work around: xiaomi 5X 7.1.2 audioTrack getPosition always is 0 when seek to end
        int64_t now = af_getsteady_ms();

        if (mCheckAudioQueEOSTime == INT64_MIN || mAudioQueDuration != audioQueDuration) {
            mCheckAudioQueEOSTime = now;
            mAudioQueDuration = audioQueDuration;
        }

        if ((now - mCheckAudioQueEOSTime) * 1000 <= audioQueDuration) {
            return false;
        }
    }
    return true;
}

bool SuperMediaPlayer::checkEOSVideo()
{
    if (!HAVE_VIDEO) {
        return true;
    }
    if (mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO) && !videoDecoderEOS && (APP_BACKGROUND != mAppStatus)) {
        return false;
    }
    int packetSize = 0;
    int frameSize = 0;
    if ((APP_BACKGROUND != mAppStatus) && mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)) {
        frameSize += mVideoFrameQue.size();
        packetSize += mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO);
    }

    if (frameSize > 0 || packetSize > 0) {
        AF_TRACE;
        return false;
    }
    return true;
}

void SuperMediaPlayer::checkEOS()
{
    if (!mEof || PLAYER_COMPLETION == mPlayStatus) {
        return;
    }
    if (!mVideoEOS) {
        mVideoEOS = checkEOSVideo();
    }
    if (!mAudioEOS) {
        mAudioEOS = checkEOSAudio();
    }
    if (!(mVideoEOS && mAudioEOS)) {
        return;
    }
    NotifyPosition(mDuration);
    playCompleted();
}

void SuperMediaPlayer::playCompleted()
{
    //notify seek completion if seek to the end directly.
    if (mSeekFlag) {
        mSeekFlag = false;

        if (!mMessageControl->findMsgByType(MSG_SEEKTO)) {
            ResetSeekStatus();
            mPNotifier->NotifySeekEnd(mSeekInCache);
            mSeekInCache = false;
        }
    }

    if (mSet->bLooping && mDuration > 0) {
        mSeekPos = 0;//19644161: need reset seek position
        mMsgCtrlListener->ProcessSeekToMsg(0, false);
        mPNotifier->NotifyLoopStart();
        NotifyPosition(0);
    } else {
        if (mPlayStatus != PLAYER_COMPLETION) {
            if (APP_BACKGROUND == mAppStatus) {
                FlushVideoPath(true, true, __func__);
                mBufferController->ClearPacket(BUFFER_TYPE_VIDEO);
            }

            mPNotifier->NotifyCompletion();
            ChangePlayerStatus(PLAYER_COMPLETION);
            mUtil->reset();
        }
    }
}

int SuperMediaPlayer::DecodeVideoPacket(unique_ptr<IAFPacket> &pVideoPacket)
{
    int ret = 0;

    if (videoDecoderEOS) {
        return ret;
    }

    int64_t pos = getCurrentPosition();

    if (pVideoPacket != nullptr) {
        /*
         * 【切档那一跳重建解码器：推迟到这个包的现场来做】
         *
         * 见 switchVideoStream() 里 mVideoDecoderRebuildPending 的说明：切档那一刻新档的
         * meta 还没有 extradata（init 段由读线程稍后解析），按它重建会沿用旧档参数集，
         * 新档会整段判坏数据。这里已经是"新档第一个视频包到手"的时刻 —— meta 完整了，
         * 必须在把这一包交给解码器**之前**重建。
         */
        if (mVideoDecoderRebuildPending && mVideoSwitchInFlight &&
            pVideoPacket->getInfo().streamIndex == mCurrentVideoIndex) {
            mVideoDecoderRebuildPending = false;
            AF_LOGI("quality switch: first packet of stream %d arrived — rebuilding the in-place decoder now "
                    "(the target stream meta carries the init segment's parameter sets by now)\n",
                    mCurrentVideoIndex);

            if (rebuildVideoDecoder(false) < 0) {
                AF_LOGW("quality switch: deferred in-place decoder rebuild failed for stream %d\n",
                        mCurrentVideoIndex);
            }
        }
        if (pVideoPacket->getInfo().streamIndex == mCurrentVideoIndex) {
            const int64_t packetPtsForAxis = pVideoPacket->getInfo().pts;
            const int64_t packetPosForAxis = pVideoPacket->getInfo().timePosition;

            if (packetPtsForAxis != INT64_MIN && packetPosForAxis >= 0) {
                int64_t freshOffset = packetPosForAxis - packetPtsForAxis;

                /*
                 * 【轴归一化只对"原始 pts 轴不按节目速率推进"的片源生效】
                 *
                 * 这段重建偏移本来是给 DASH rendition 写的（那种片源的 raw pts 轴不按节目速率走，
                 * 不归一每个分片边界都会漂）。但单文件片源（本地/网络 mp4、FLV 直播）音视频共用
                 * 一条程序时钟，**只归视频**等于人为造出一个固定偏移。实测日志就是这么炸的：
                 *   video pts axis refreshed: stream=0 rawPts=115138000 timePosition=0
                 *                             offset=-115138000
                 *   drop frame: master played time is 115171218, video pts is 42000
                 * 视频 raw pts 从 115.138s 起被压到 0，而音频/主时钟仍在原轴（115.17s）⇒ 每帧都被
                 * 判"迟到"丢掉、每 8 帧才强制上屏一帧 ⇒ FLV 直播一跳一跳地卡。
                 *
                 * 判据用**已有的轴观测数据**（下面那两个 deque 里存的历史样本），不用解复用器类型
                 * （IDemuxer 并不继承 demuxerPrototype，getType() 在播放器侧取不到）：
                 * 取上一包的 (rawPts, timePosition)，若两者推进量基本一致（1:1，允许 500ms 抖动）
                 * ⇒ 这条轴本来就是节目的轴 ⇒ 保持恒等偏移（0），不做归一。
                 * DASH 那种轴在分片内 1:1、跨分片跳变的情况依旧会被归一（分片边界上差值会超阈值）。
                 */
                if (!mVideoAxisPts.empty()) {
                    const int64_t previousPtsSample = mVideoAxisPts.back();
                    const int64_t previousPosSample = mVideoAxisTimePos.back();
                    const int64_t ptsStep = packetPtsForAxis - previousPtsSample;
                    const int64_t posStep = packetPosForAxis - previousPosSample;

                    if (llabs(ptsStep - posStep) <= 500 * 1000) {
                        freshOffset = 0;
                    }
                } else {
                    /*
                     * 第一个包没有历史样本可比较 ⇒ 先不归一（保持"未设置"）：
                     * 否则单文件直播的首包会被压出一个 -115s 级的偏移、那一帧直接判迟到。
                     * DASH 那种轴在第 2 个包就会显出不一致并被正常归一，代价只是首包不动偏移。
                     */
                    freshOffset = INT64_MIN;
                }

                const int64_t previousOffset = mActiveVideoPtsOffset;
                const bool logThisRefresh = (previousOffset == INT64_MIN ||
                                             llabs(freshOffset - previousOffset) > 500 * 1000);
                mActiveVideoPtsOffset = freshOffset;
                mVideoAxisPts.push_back(packetPtsForAxis);
                mVideoAxisTimePos.push_back(packetPosForAxis);

                while (mVideoAxisPts.size() > 64) {
                    mVideoAxisPts.pop_front();
                    mVideoAxisTimePos.pop_front();
                }

                if (logThisRefresh) {
                    AF_LOGW("video pts axis refreshed from the packet: stream=%d rawPts=%lld timePosition=%lld "
                            "offset=%lld (was %lld, corrected by %lld ms) — a DASH rendition whose raw pts axis "
                            "does not advance at the program rate would otherwise drift out of sync after every "
                            "segment boundary\n",
                            pVideoPacket->getInfo().streamIndex, (long long) packetPtsForAxis,
                            (long long) packetPosForAxis, (long long) freshOffset,
                            (long long) (previousOffset == INT64_MIN ? 0 : previousOffset),
                            (long long) ((previousOffset == INT64_MIN ? 0 : (freshOffset - previousOffset)) / 1000));
                }
            }
        }

        // for cache video, or seeking accurate, check whether drop output frame
        /*
         * 落点帧即上屏之后，默认路径不再把"目标点之前的包"标成丢弃 —— 否则
         * 落点帧本身（它就在目标点之前）会被丢掉，画面出不来。
         * 只有 mSeekExactLanding（旧语义）才继续按目标点丢包；dropLateVideoFrames
         * 是"已上屏之后的迟到帧"策略，与 seek 无关，保持不变。
         */
        if ((mSeekExactLanding && mSeekNeedCatch) || dropLateVideoFrames) {
            int64_t checkPos = (mSeekExactLanding && mSeekNeedCatch) ? mSeekPos.load() : pos;

            // only decode and don't need output to render if too old
            if ((pVideoPacket->getInfo().timePosition < checkPos) && (pVideoPacket->getInfo().timePosition < mDuration - 200 * 1000)) {
                pVideoPacket->setDiscard(true);
            }
        }
        if (mDiscontinuity.filterActive.load() && pVideoPacket->getInfo().flags != 0) {
            /*
             * 【P0】demuxer 落点 = seek 之后第一个关键帧包的位置。这就是
             * Discontinuity::startUs：它可能早于也可能晚于 targetUs（稀疏 IDR 时早，
             * 目标落在分片之前时晚），渲染器的过滤规则只关心"包含目标的帧"，
             * 不要求它等于目标 —— 所以这里照实记录，不做任何夹取。
             */
            markDiscontinuityStartUs(pVideoPacket->getInfo().timePosition);
            if (!mSeekDecodeStartIsKey) {
                AF_LOGI("seek decode starts at a keyframe: pts=%lld flags=%d\n",
                        (long long) pVideoPacket->getInfo().timePosition, pVideoPacket->getInfo().flags);
            }
            if (!mSeekAudioAlignDone && HAVE_AUDIO) {
                const int64_t landingUs = pVideoPacket->getInfo().timePosition;
                const int64_t audioFront = mBufferController->GetPacketPts(BUFFER_TYPE_AUDIO);
                const int64_t audioAnchorUs = mDiscontinuity.targetUs;

                mSeekAudioAlignDone = true;

                /*
                 * 落点比目标早多少：只留一条对账日志（精度判据与目标本身比较，
                 * 与"落点离目标多远"无关，所以这里不参与任何判定）。
                 */
                if (audioAnchorUs != INT64_MIN && landingUs > 0 && audioAnchorUs > landingUs) {
                    AF_LOGI("seek: the landing keyframe is %lld ms before the target — decoding forward "
                            "to the frame that contains the target (exact landing, no budget)\n",
                            (long long) ((audioAnchorUs - landingUs) / 1000));
                }

                /*
                 * 对齐动作只依赖"目标点已知"，**不依赖音频包队列当时有没有数据**：
                 * 清已解码帧队列、重基时间轴都不需要读包队列，只有"裁包"这一步需要
                 * audioFront（丢包统计）。原来整块挂在 `audioFront != INT64_MIN` 之下，
                 * 一旦那一刻音频包队列还是空的就整块跳过 —— 等于对齐在最需要它的路径上失效。
                 */
                if (audioAnchorUs > 0) {
                    int64_t dropped = 0;

                    if (audioFront != INT64_MIN) {
                        dropped = mBufferController->ClearPacketBeforeTimePos(BUFFER_TYPE_AUDIO, audioAnchorUs);
                    }
                    if (pVideoPacket->getInfo().pts != INT64_MIN && pVideoPacket->getInfo().pts != landingUs) {
                        mActiveVideoPtsOffset = landingUs - pVideoPacket->getInfo().pts;
                        AF_LOGI("seek landing: this stream's raw pts axis is %lld ms away from the timePosition "
                                "axis (landing packet pts=%lld timePosition=%lld) - video frames are normalized "
                                "onto the master clock axis so they are no longer judged too early\n",
                                (long long) (mActiveVideoPtsOffset / 1000),
                                (long long) pVideoPacket->getInfo().pts, (long long) landingUs);
                    }
                    while (!mAudioFrameQue.empty()) {
                        mAudioFrameQue.pop_front();
                    }

                    mPlayedAudioPts = INT64_MIN;
                    mAudioTime.startTime = audioAnchorUs;
                    mAudioTime.deltaTime = 0;
                    mAudioTime.deltaTimeTmp = 0;
                    mLastAudioFrameDuration = -1;
                    /* 音频包指针也必须丢：它属于 seek 前的旧时间轴。 */
                    mAudioPacket = nullptr;

                    AF_LOGI("seek audio aligned to the TARGET point: landing=%lld target=%lld dropped=%lld "
                            "audioFront=%lld (audio starts at the same point the audio clock base is pinned "
                            "on; no floor and no watermark — old-timeline data was invalidated by the "
                            "discontinuity flush)\n",
                            (long long) landingUs, (long long) audioAnchorUs, (long long) dropped,
                            (long long) mBufferController->GetPacketPts(BUFFER_TYPE_AUDIO));
                } else {
                    AF_LOGI("seek audio align skipped (landing=%lld target unknown audioFront=%lld) — "
                            "the audio path continues on its existing timeline\n",
                            (long long) landingUs, (long long) audioFront);
                }
            }

            mSeekDecodeStartIsKey = true;
        }

        if (!mRecorderSet->decodeFirstVideoFrameInfo.isFirstPacketSendToDecoder) {
            DecodeFirstFrameInfo &info = mRecorderSet->decodeFirstVideoFrameInfo;
            info.isFirstPacketSendToDecoder = true;
            info.firstPacketSize = pVideoPacket->getSize();
            info.firstPacketPts = pVideoPacket->getInfo().pts;
            info.waitFirstFrame = true;
            info.sendFirstPacketTimeMs = af_getsteady_ms();
        }

        ret = mAVDeviceManager->sendPacket(pVideoPacket, SMPAVDeviceManager::DEVICE_TYPE_VIDEO, 0);
        // don't need pop if need retry later
        if (!(ret & STATUS_RETRY_IN)) {
            //  mBufferController->PopFrontPacket(BUFFER_TYPE_VIDEO);
            assert(pVideoPacket == nullptr);
        } else {
            /*
             * 状态签名：解码器连自己的输入队列都满了（包塞不进去）。这不是"没有输入"
             * （包就在这里），是 codec 拿着缓冲不放手 —— 后续由 doRender() 里那条
             * 纯状态判据决定是否走一次错误驱动的重建。
             * 这里**不再**要求 mSeekFlag：安卓实测该签名出现在 seek 结束之后
             * （seekFlag 只活 16~23ms），限定在 seek 窗口内等于永远看不到它。
             * 只记闩，不在解码线程里做任何判断或等待。
             */
            mVideoDecodeRetrySeen = true;
        }
    } else if (mEof) {
        //     mVideoDecoder->setEOF();
        mAVDeviceManager->sendPacket(pVideoPacket, SMPAVDeviceManager::DEVICE_TYPE_VIDEO, 0);
        ret = 0;
    }

    if (ret > 0) {
        bool haveError = false;
        bool drmError = false;

        if (ret & STATUS_HAVE_ERROR) {
            if (mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)->get_error_frame_no() > MAX_DECODE_ERROR_FRAME) {
                haveError = true;
            }
        }

        if (ret & STATUS_DRM_ERROR) {
            haveError = true;
            drmError = true;
        }

        //            if (ret & STATUS_CREATE_FAIL) {
        //                haveError = true;
        //            }

        if (haveError) {
            const bool canRecover = !drmError && mVideoDecodeRebuildCount < MAX_VIDEO_DECODER_REBUILDS;
            bool recovered = false;

            if (canRecover) {
                ++mVideoDecodeRebuildCount;
                AF_LOGW("video decode error: errorFrames=%d without a single output frame, rebuilding the "
                        "video decoder (attempt %d/%d, release + init with hw->sw fallback)\n",
                        (int) mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)->get_error_frame_no(),
                        (int) mVideoDecodeRebuildCount, (int) MAX_VIDEO_DECODER_REBUILDS);

                recovered = (rebuildVideoDecoder(false) >= 0);

                if (recovered) {
                    /* 新解码器从干净状态开始计数（重建成功≠已经有帧，计数由出帧处清零）。 */
                    IDecoder *videoDecoder = mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);
                    if (videoDecoder != nullptr) {
                        videoDecoder->clean_error();
                    }
                }
            }

            if (!recovered) {
                AF_LOGW("video decode error is not recoverable (attempts=%d/%d, drm=%d): reporting a fatal error\n",
                        (int) mVideoDecodeRebuildCount, (int) MAX_VIDEO_DECODER_REBUILDS, (int) drmError);
                ChangePlayerStatus(PLAYER_ERROR);
                mPNotifier->NotifyError(MEDIA_PLAYER_ERROR_DECODE_VIDEO, "video decode error");
            }
        }
    }

    return ret;
}

int SuperMediaPlayer::FillVideoFrame()
{
    int64_t pos = getCurrentPosition();
    unique_ptr<IAFFrame> pFrame{};
    int ret = mAVDeviceManager->getFrame(pFrame, SMPAVDeviceManager::DEVICE_TYPE_VIDEO, 0);

    if (ret == STATUS_EOS) {
        videoDecoderEOS = true;

        if (mSeekFlag && mSeekNeedCatch) {
            mSeekNeedCatch = false;
        }
    }

    if (pFrame != nullptr) {

        if (mSeekFlag && mPlayStatus == PLAYER_PAUSED) {
            AF_LOGI("PFR: frame pulled pts=%" PRId64 " seekFlag=%d\n", pFrame->getInfo().pts, (int) mSeekFlag);
        }

        if (mRecorderSet->decodeFirstVideoFrameInfo.waitFirstFrame) {
            DecodeFirstFrameInfo &info = mRecorderSet->decodeFirstVideoFrameInfo;
            info.getFirstFrameTimeMs = af_getsteady_ms();
            info.waitFirstFrame = false;
        }

        mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)->clean_error();
        /* 解码器又能出帧了 = “codec 恢复正常”：把错误驱动的重建次数清零，
         * 下次再出问题仍然允许重新尝试 release + init（语义见 DecodeVideoPacket）。 */
        mVideoDecodeRebuildCount = 0;

        /*
         * "有输入却零输出"那条状态判据的**自愈**（第 2 项）：只要有帧真的出来，
         * 卡死闩、轮数与输入饥饿签名一起复位 —— 下一次真卡死仍然允许重建一次。
         * 这就是它全部意义上的"冷却"：由出帧事件驱动，不用任何计时器。
         */
        mDecodeStallRebuildDone = false;
        mDecodeStallIters = 0;
        mVideoDecodeRetrySeen = false;

        /* 切换提交后，目标 decoder 的每一帧都必须使用与首帧相同的时间轴偏移。
         * 只修正首帧会导致后续帧突然跳回原始 rendition PTS，RenderVideo() 便会
         * 把它们当成严重落后帧持续丢弃，日志表现为 0/1 FPS。 */
        bool framePtsTakenFromPacketAxis = false;

        if (pFrame->getInfo().pts != INT64_MIN) {
            while (!mVideoAxisPts.empty() && mVideoAxisPts.front() < pFrame->getInfo().pts) {
                mVideoAxisPts.pop_front();
                mVideoAxisTimePos.pop_front();
            }

            if (!mVideoAxisPts.empty() && mVideoAxisPts.front() == pFrame->getInfo().pts) {
                pFrame->getInfo().pts = mVideoAxisTimePos.front();
                mVideoAxisPts.pop_front();
                mVideoAxisTimePos.pop_front();
                framePtsTakenFromPacketAxis = true;
            }
        }

        if (!framePtsTakenFromPacketAxis &&
            mActiveVideoPtsOffset != INT64_MIN && pFrame->getInfo().pts != INT64_MIN) {
            pFrame->getInfo().pts += mActiveVideoPtsOffset;
        }

        if (mSecretPlayBack) {
            pFrame->setProtect(true);
        }
        int64_t pts = pFrame->getInfo().pts;
        if (pFrame->getInfo().timePosition >= 0) {
            mActiveDecoderFramesCarryTimePosition = true;
        }

        if (mSeekFlag && mSeekNeedCatch) {
            mSeekNeedCatch = false;
        }

        auto *meta = (Stream_meta *) (mCurrentVideoMeta.get());

        if (!mAdaptiveVideo && mVideoWidth > 0 && (pFrame->getInfo().video.width != mVideoWidth || pFrame->getInfo().video.height != mVideoHeight)) {

            // displayWidth and displayHeight is changed, but we can't know the sar for now

            // TODO: get the sar from frame or update current video meta
            meta->displayWidth = meta->displayHeight = 0;
        }

        if (meta->displayWidth > 0 && meta->displayHeight > 0) {
            pFrame->getInfo().video.dar = 1.0 * meta->displayWidth / meta->displayHeight;
        } else if (pFrame->getInfo().video.width > 0 && pFrame->getInfo().video.height > 0) {
            //if not get displaywidth/height , set dar with width/height
            pFrame->getInfo().video.dar = 1.0 * pFrame->getInfo().video.width / pFrame->getInfo().video.height;
        }
        /* 宽高未知的帧（切档新建解码器后的头一帧）不再写 dar：0/0 会得到 NaN、x/0 会得到 inf，
         * 渲染侧 realWidth = 帧高 × dar 随之变成 NaN/0 ⇒ 绘制四边形退化，画面闪一下再弹回来。 */

        mDemuxerService->SetOption("V_FRAME_DECODED", pts);
        //            AF_LOGI("DecodeVideoPacket p_dec_delay frame :%lld pos:%lld, mPlayedAudioPts:%lld, posdiff:%lld audiodiff:%lld videodiff:%lld",
        //                    pFrame->GetPts()/1000, pos/1000, mPlayedAudioPts/1000, (pos - pFrame->GetPts())/1000,
        //                    (mLastInputAudio - mPlayedAudioPts)/1000, (mLastInputVideo - pFrame->GetPts())/1000);


        int format = pFrame->getInfo().video.format;
        bool success = push(pFrame);
        if (success) {
            while (true) {
                std::unique_ptr<IAFFrame> frame = nullptr;
                success = pull(format, frame);
                if (success) {
                    mVideoFrameQue.push(std::move(frame));
                } else {
                    break;
                }
            }
        } else {
            mVideoFrameQue.push(std::move(pFrame));
        }

        videoDecoderFull = true;
    }

    return ret;
}






bool SuperMediaPlayer::push(unique_ptr<IAFFrame> &frame)
{
#ifdef ENABLE_VIDEO_FILTER
    std::lock_guard<std::mutex> filterLock(mFilterManagerMutex);
    if (mFilterManager != nullptr) {
        bool success = mFilterManager->push(frame);
        return success;
    }
#endif
    return false;
}

bool SuperMediaPlayer::pull(int format, unique_ptr<IAFFrame> &frame)
{
#ifdef ENABLE_VIDEO_FILTER
    std::lock_guard<std::mutex> filterLock(mFilterManagerMutex);
    if (mFilterManager != nullptr) {
        bool success = mFilterManager->pull(format, frame);
        return success;
    }
#endif
    return false;
}

bool SuperMediaPlayer::render()
{
    //send to audio render
    bool audioRendered = false;
    bool videoRendered = false;
    if (mCurrentAudioIndex >= 0) {
        {
            int ret;
            do {
                ret = RenderAudio();
                if (RENDER_NONE != ret) {
                    audioRendered = true;
                }
            } while (ret == RENDER_FULL);
        }
    }

    if (HAVE_VIDEO) {
        videoRendered = RenderVideo(!mFirstRendered);
    }

    if (HAVE_VIDEO && HAVE_AUDIO) {
        // TODO: do it in reset()
        if (!PTS_REVERTING) {
            /* 不在该状态：把日志限频闩复位，下一次进入时还能各打一条。 */
            mPtsRevertLastLoggedAudio = INT64_MIN;

            if (videoRendered && audioRendered) {
                mVideoPtsRevert = mAudioPtsRevert = false;
            }
        } else {
            /*
             * 限频：只在音频位置**变化**时打一条（原来每轮一条，实测 1MB 日志整份被它打满）。
             * 不引入时间判据 —— 值没变就说明还卡在同一个状态，重复记录只会淹没日志。
             */
            if (mPtsRevertLastLoggedAudio != mPlayedAudioPts) {
                mPtsRevertLastLoggedAudio = mPlayedAudioPts;
                AF_LOGW("PTS_REVERTING audio pts is %lld ,video pts is %lld (logged on change; if this "
                        "stops appearing the pair is stuck in this state)\n",
                        (long long) mPlayedAudioPts, (long long) mPlayedVideoPts);
            }
        }
    } else {
        mVideoPtsRevert = mAudioPtsRevert = false;
    }

    if ((HAVE_SUBTITLE || mSubPlayer) && !mSeekFlag) {
        RenderSubtitle(mCurVideoPts);
    }

    return audioRendered || videoRendered;
}

RENDER_RESULT SuperMediaPlayer::RenderAudio()
{
    RENDER_RESULT ret = RENDER_NONE;
    int64_t pts = INT64_MIN;
    int64_t duration = INT64_MIN;
    int64_t position = INT64_MIN;
    int render_ret;
//#define DUMP_PCM
#ifdef DUMP_PCM
    static int fd = open("out.pcm", O_CREAT | O_RDWR, 0666);
#endif

    if (mAudioFrameQue.empty()) {
        if (audioDecoderEOS && mAVDeviceManager->getAudioRenderQueDuration() == 0) {
            mMasterClock.setReferenceClock(nullptr, nullptr);
        }
        return ret;
    }

    pts = mAudioFrameQue.front()->getInfo().pts;
    position = mAudioFrameQue.front()->getInfo().timePosition;

    if (pts == INT64_MIN) {
        mAudioFrameQue.pop_front();
        return ret;
    }
    if (mDiscontinuity.filterActive.load() || mDiscontinuity.audioLandingPending.load()) {
        const int64_t targetUs = mDiscontinuity.targetUs;

        if (targetUs != INT64_MIN) {
            int droppedBeforeTarget = 0;
            int64_t firstDroppedUs = INT64_MIN;

            while (!mAudioFrameQue.empty() && mAudioFrameQue.front() != nullptr) {
                const int64_t headPts = mAudioFrameQue.front()->getInfo().pts;

                if (headPts == INT64_MIN || headPts >= targetUs) {
                    break;
                }

                if (firstDroppedUs == INT64_MIN) {
                    firstDroppedUs = headPts;
                }

                mAudioFrameQue.pop_front();
                ++droppedBeforeTarget;
            }

            if (droppedBeforeTarget > 0 && mAudioLandingDropLoggedGen != mDiscontinuity.generation.load()) {
                mAudioLandingDropLoggedGen = mDiscontinuity.generation.load();
                AF_LOGI("audio landing drop: dropped=%d pos=%lld target=%lld generation=%d (audio starts at the "
                        "SAME target point the video landing filter uses; the audio clock base is pinned on it, "
                        "so frames before it are never sent to the device)\n",
                        droppedBeforeTarget, (long long) firstDroppedUs,
                        (long long) targetUs, mAudioLandingDropLoggedGen);
            }

            /*
             * 地板的关闭点（纯状态判据，两个出口）：
             *   · 队首到达/越过目标点（`headPts >= targetUs` 让上面的 while 正常退出，
             *     或队首 pts 未知）⇒ 音频路已经走到本次不连续点的目标点，地板使命结束；
             *   · 音频解码器已 EOS ⇒ 不会再有更晚的帧，再闸下去只会把音频闷死。
             * 换档不臂地板，所以这里对"音频时间轴连续"的那些路径是空操作。
             */
            if (mDiscontinuity.audioLandingPending.load()) {
                const bool headReachedTarget =
                        mAudioFrameQue.empty() || mAudioFrameQue.front() == nullptr ||
                        mAudioFrameQue.front()->getInfo().pts == INT64_MIN ||
                        mAudioFrameQue.front()->getInfo().pts >= targetUs;

                if (headReachedTarget || audioDecoderEOS) {
                    mDiscontinuity.audioLandingPending = false;
                    AF_LOGI("audio landing reached: head=%lld target=%lld eos=%d droppedBeforeTarget=%d "
                            "generation=%d (the audio path is now at the same target point the video landing "
                            "filter and the audio clock base use, so it is handed to the device from here on)\n",
                            (long long) (mAudioFrameQue.empty() || mAudioFrameQue.front() == nullptr
                                         ? INT64_MIN : mAudioFrameQue.front()->getInfo().pts),
                            (long long) targetUs, (int) audioDecoderEOS, droppedBeforeTarget,
                            mDiscontinuity.generation.load());
                }
            }

            if (mAudioFrameQue.empty()) {
                return ret;
            }

            /* 队首可能刚变过：重新取时间戳（后面的推送与记账都用它）。 */
            pts = mAudioFrameQue.front()->getInfo().pts;
            position = mAudioFrameQue.front()->getInfo().timePosition;

            if (pts == INT64_MIN) {
                mAudioFrameQue.pop_front();
                return ret;
            }
        }
    }

    auto *avafFrame = dynamic_cast<AVAFFrame *>(mAudioFrameQue.front().get());

    if (avafFrame) {
        duration = getPCMFrameDuration(avafFrame->ToAVFrame());
    }

    if (mFrameCb && (!mSecretPlayBack || mDrmKeyValid)) {
        mFrameCb(mFrameCbUserData, avafFrame);
    }

    render_ret = mAVDeviceManager->renderAudioFrame(mAudioFrameQue.front(), 0);

    if (render_ret == IAudioRender::FORMAT_NOT_SUPPORT) {
        if (mAVDeviceManager->getAudioRenderQueDuration() == 0) {
            std::lock_guard<std::mutex> uMutex(mCreateMutex);
            mAudioTime.startTime = mAudioFrameQue.front()->getInfo().pts;
            mAudioTime.deltaTimeTmp = 0;
            mAudioTime.deltaTime = 0;
            mLastAudioFrameDuration = -1;
            setUpAudioRender(mAudioFrameQue.front()->getInfo().audio);
            if (mBRendingStart) {
                mAVDeviceManager->pauseAudioRender(false);
            }
            mAVDeviceManager->renderAudioFrame(mAudioFrameQue.front(), 0);
        }
    } else if (render_ret == IAudioRender::OPEN_AUDIO_DEVICE_FAILED) {
        AF_LOGE("render audio failed due to can not open device, close audio stream");
        mOpenAudioDeviceFailed = true;
        closeAudio();
        if (HAVE_VIDEO) {
            mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_OPEN_AUDIO_DEVICE_FAILED, "open audio device failed");
        } else {
            ChangePlayerStatus(PLAYER_ERROR);
            mPNotifier->NotifyError(MEDIA_PLAYER_ERROR_RENDER_AUDIO_OPEN_DEVICE_FAILED, "open audio device failed");
            return ret;
        }
    }

    if (!mAudioFrameQue.empty() && mAudioFrameQue.front() == nullptr) {
        mAudioFrameQue.pop_front();
        ret = RENDER_FULL;
    } else {
        return ret;
    }

#ifdef DUMP_PCM
    write(fd, buffer + mWriteAudioLen, len);
#endif

    if (!HAVE_VIDEO) {
        checkFirstRender();
    }

    if (mPlayedAudioPts == INT64_MIN) {
        const int64_t targetUs = mDiscontinuity.targetUs;
        const int64_t frameDurationUs = (duration > 0) ? duration
                                       : ((mLastAudioFrameDuration > 0) ? mLastAudioFrameDuration : 0);
        const bool firstFrameBeforeSeekTarget =
                (targetUs != INT64_MIN) && (pts != INT64_MIN) && (pts + frameDurationUs <= targetUs);

        if (firstFrameBeforeSeekTarget) {
            /*
             * 窗口外首帧：这是"落点地板放行了它"或"解码器在窗口外吐帧"的兜底。
             * 时间轴按**目标点**建立（与音频时钟基准 pinAudioClockBase(targetUs) 同一个值），
             * 而不是按这一帧的 pts —— 保证"时间轴的起点"与"时钟的基准"是同一个点。
             * 本帧仍会照常推给设备（不在这里丢帧：丢帧职责只有一个地方，上面的落点地板），
             * 所以不会引入任何新的等待或阈值。
             */
            mAudioTime.startTime = targetUs;
            mAudioTime.deltaTime = 0;
            mAudioTime.deltaTimeTmp = 0;
            AF_LOGI("audio first frame after seek is OUTSIDE the seek window: frame=%lld frameDuration=%lld "
                    "target=%lld — the audio timeline is started on the TARGET point instead of on this frame, "
                    "so the offset bookkeeping cannot inherit an out-of-window start\n",
                    (long long) pts, (long long) frameDurationUs, (long long) targetUs);
        } else {
            mAudioTime.startTime = pts;
            mAudioTime.deltaTime = 0;
            mAudioTime.deltaTimeTmp = 0;
        }

        /*
         * 诊断（每次 seek 后第一帧音频只打一条，天然限频 —— 因为 1b 会把 mPlayedAudioPts
         * 重置为 INT64_MIN，所以这个分支每个 seek 只会走到一次）：
         * 直接回答"音频首帧的 PTS 与目标点差多少"，也就是 afterSeekMs 的来源。
         *   pts    = 音频第一帧自己的 PTS（按 RenderAudio 的丢弃判据，它必然 >= 目标点）；
         *   target = 本次 seek 的目标点（音频时钟基准与位置上报用的都是它）。
         */
        if (mDiscontinuity.targetUs != INT64_MIN || mSeekFlag) {
            AF_LOGI("audio first frame after seek: pts=%lld target=%lld afterSeekMs=%lld "
                    "master=%lld audioBase=%lld consumed=%lld\n",
                    (long long) pts, (long long) mDiscontinuity.targetUs,
                    (long long) (mSeekRequestMs > 0 ? af_getsteady_ms() - mSeekRequestMs : -1),
                    (long long) mMasterClock.GetTime(),
                    (long long) mDiscontinuity.audioBaseUs.load(),
                    (long long) (mAVDeviceManager != nullptr ? mAVDeviceManager->getAudioRenderPosition() : INT64_MIN));
        }
        if (mDiscontinuity.audioBaseUs.load() == INT64_MIN) {
            pinAudioClockBase(pts);
        }

        /*
         * 把主时钟的参考交给音频：这是"有音频时主时钟的唯一来源"。
         * 无音频（或音频设备不可用）时参考为空，主时钟按落点采纳时钉在 targetUs 上
         * 自走 —— 也就是"无音频时 = 视频落点基准"，两个来源不会同时生效。
         */
        mMasterClock.setReferenceClock(getAudioPlayTimeStampCB, this);
    } else {
        if (mLastAudioFrameDuration > 0) {
            if (!mAudioPtsRevert) {
                mAudioPtsRevert = pts < mPlayedAudioPts - mPtsDiscontinueDelta;
                if (mAudioPtsRevert) {
                    AF_LOGI("PTS_REVERTING audio start\n");
                }
            }
            int64_t offset = pts - (mPlayedAudioPts + mLastAudioFrameDuration);

            /*
             * the mLastAudioFrameDuration and the pts are all not the accurate value,
             * the mLastAudioFrameDuration accurate for 1/1000000 s,
             * the pts maybe accurate for 1/1000 s (eg. flv file), so can't increase the deltaTimeTmp when
             * offset little than 1ms.
             */
            if (llabs(offset)) {
                //    AF_LOGW("offset is %lld,pts is %lld", offset, pts);
                mAudioTime.deltaTimeTmp += offset;
                mPlayedAudioPts += offset;
            }

            /*
             * 偏差修正不阶跃（对齐主流做法：小偏差渐进、真跳变才一次性对齐）。
             *   · |偏差| <= 100ms ：每帧最多并入 5ms，几十帧内平滑收敛，画面不会顿一下；
             *   · |偏差| > 100ms  ：一次性对齐（这是时间戳跳变/换段，属于事件，不是定时兜底）；
             *   · |偏差| > 10s    ：判定根本不同步，放弃修正（同 ffplay 的 AV_NOSYNC 阈值）。
             * 原来 >100ms 就把整段累积值一次并入，主时钟一跳、视频路就要丢/重排帧。
             */
            const int64_t kGradualStepUs = 5000;
            const int64_t kDiscontinuityUs = 100 * 1000;
            const int64_t kReanchorUs = 1000 * 1000;
            const int64_t kNoSyncUs = 10 * 1000 * 1000;

            if (llabs(mAudioTime.deltaTimeTmp) > kNoSyncUs) {
                mAudioTime.deltaTimeTmp = 0;
            } else if (llabs(mAudioTime.deltaTimeTmp) > kReanchorUs) {
                AF_LOGW("audio timeline re-anchored (offset %lld > 1s): audio pts=%lld, frameDuration=%lld "
                        "— the clock is NOT moved by this frame\n",
                        (long long) mAudioTime.deltaTimeTmp, (long long) pts,
                        (long long) mLastAudioFrameDuration);

                mPlayedAudioPts = pts;
                mAudioTime.startTime = pts;
                mAudioTime.deltaTimeTmp = 0;
                mAudioTime.deltaTime = 0;
            } else if (llabs(mAudioTime.deltaTimeTmp) > kDiscontinuityUs) {
                AF_LOGW("correct audio and master clock offset is %lld, frameDuration :%lld", mAudioTime.deltaTimeTmp,
                        mLastAudioFrameDuration);
                mAudioTime.deltaTime += mAudioTime.deltaTimeTmp;
                mAudioTime.deltaTimeTmp = 0;
            } else if (mAudioTime.deltaTimeTmp > kGradualStepUs) {
                mAudioTime.deltaTime += kGradualStepUs;
                mAudioTime.deltaTimeTmp -= kGradualStepUs;
            } else if (mAudioTime.deltaTimeTmp < -kGradualStepUs) {
                mAudioTime.deltaTime -= kGradualStepUs;
                mAudioTime.deltaTimeTmp += kGradualStepUs;
            }
            const int64_t kMaxClockCorrectionUs = 300 * 1000;

            if (mAudioTime.deltaTime > kMaxClockCorrectionUs) {
                mAudioTime.deltaTime = kMaxClockCorrectionUs;
            } else if (mAudioTime.deltaTime < -kMaxClockCorrectionUs) {
                mAudioTime.deltaTime = -kMaxClockCorrectionUs;
            }

            /*
             * 诊断打印的**行数上界是常数级**：`deltaTime` 已经被上面限幅在 ±300 ms，
             * 而水位每 100 ms 才动一次 ⇒ 单方向最多 3~6 行；换向时把水位清零重新起算，
             * 这样两个方向都能看到，但仍然不会刷屏（不需要额外的限流器、也没有计时器）。
             */
            if ((mAudioTime.deltaTime >= 0) != (mAudioClockDriftLoggedUs >= 0)) {
                mAudioClockDriftLoggedUs = 0;
            }

            if (llabs(mAudioTime.deltaTime) >= llabs(mAudioClockDriftLoggedUs) + 100 * 1000) {
                mAudioClockDriftLoggedUs = mAudioTime.deltaTime;
                AF_LOGW("audio clock drift: correction deltaTime=%lld us (capped at +-%lld), startTime=%lld, "
                        "aoutPos=%lld, lastFrameDuration=%lld, audio pts=%lld — if this keeps growing in one "
                        "direction the audio timeline bookkeeping (durations vs pts) is off, not the video\n",
                        (long long) mAudioTime.deltaTime, (long long) kMaxClockCorrectionUs,
                        (long long) mAudioTime.startTime,
                        (long long) mAVDeviceManager->getAudioRenderPosition(),
                        (long long) mLastAudioFrameDuration, (long long) pts);
            }
        }
    }

    if (mPlayedAudioPts == INT64_MIN && isSeeking()) {
        // update after send first frame in seeking, because audio render callback is async.
        // sometimes notify position before audio rendered callback , will cause position not right.
        mCurrentPos = position;
    }

    if (mPlayedAudioPts != INT64_MIN) {
        mPlayedAudioPts += duration;
    } else
        mPlayedAudioPts = pts;
    mLastAudioFrameDuration = duration;

    if (mAudioChangedFirstPts == pts && !mMixMode) {
        StreamInfo *info = GetCurrentStreamInfo(ST_TYPE_AUDIO);
        mPNotifier->NotifyStreamChanged(info, ST_TYPE_AUDIO);
        AF_LOGD("audio changed\n");
        mAudioChangedFirstPts = INT64_MIN;
    }

    return ret;
}

bool SuperMediaPlayer::RenderVideo(bool force_render)
{

    if (!mAVDeviceManager->isVideoRenderValid()) {
        return false;
    }
    //send to video render
    if (mVideoFrameQue.empty()) {
        return false;
    }

    unique_ptr<IAFFrame> &videoFrame = mVideoFrameQue.front();

    if (videoFrame == nullptr) {
        return false;
    }

    int64_t videoPts = videoFrame->getInfo().pts;

    // work around for huaweiP20 pro hardware decode get pts = INT64_MIN when change resolution.
    if (videoPts == INT64_MIN && videoPts < mPlayedVideoPts) {
        videoPts = mPlayedVideoPts + 1;
    }

    int frameWidth;
    int frameHeight = videoFrame->getInfo().video.height;
    if (videoFrame->getInfo().video.dar != 0) {
        frameWidth = videoFrame->getInfo().video.dar * videoFrame->getInfo().video.height;
    } else {
        frameWidth = videoFrame->getInfo().video.width;
    }
    videoFrame->getInfo().video.rotate = mVideoRotation;
    const bool seekLandingPending = mDiscontinuity.filterActive.load();

    if (seekLandingPending) {
        mVideoPtsRevert = false;
    } else if (!mVideoPtsRevert) {
        mVideoPtsRevert = mPlayedVideoPts != INT64_MIN && videoPts < mPlayedVideoPts - mPtsDiscontinueDelta;

        if (mVideoPtsRevert) {
            AF_LOGI("PTS_REVERTING video start\n");
        }
    }
    
    if (!seekLandingPending) {
    // audio pts first revert to small, force render the old video frame
    if (PTS_REVERTING && mAudioPtsRevert && videoPts - mPtsDiscontinueDelta > mPlayedAudioPts) {
        if (mPtsRevertWaitLogged != 1) {
            AF_LOGI("PTS_REVERTING force render the old video frame (state entered; audio pts=%lld video pts=%lld)\n",
                    (long long) mPlayedAudioPts, (long long) videoPts);
            mPtsRevertWaitLogged = 1;
        }
        force_render = true;
        // video pts first revert to small,the new video data wait audio pts to revert
    } else if (PTS_REVERTING && mVideoPtsRevert && mPlayStatus == PLAYER_PLAYING &&
               videoPts + mPtsDiscontinueDelta < mPlayedAudioPts) {
        if (mPtsRevertWaitLogged != 2) {
            AF_LOGI("PTS_REVERTING wait audio to revert (state entered; audio pts=%lld video pts=%lld; "
                    "logged once per episode)\n",
                    (long long) mPlayedAudioPts, (long long) videoPts);
            mPtsRevertWaitLogged = 2;
        }
        return false;
    } else if (mPtsRevertWaitLogged != 0) {
        AF_LOGI("PTS_REVERTING wait/force-render state left (audio pts=%lld video pts=%lld)\n",
                (long long) mPlayedAudioPts, (long long) videoPts);
        mPtsRevertWaitLogged = 0;
    }
    }

    int64_t masterPlayedTime = mMasterClock.GetTime();
    const bool masterClockUnset = af_clock_value_is_unset(masterPlayedTime);
    /*
     * 主时钟未建立时不能做这个减法：masterPlayedTime 是"哨兵 + elapsed"的环绕值，
     * 与 pts 相减可能**溢出**（有符号溢出是 UB），得到的结果也无意义。
     * 取 0 = "正好准时"，后续所有分支都会选择**放行这一帧**（见下面的 masterClockUnset 护栏）。
     */
    int64_t videoLateUs = masterClockUnset ? 0 : (masterPlayedTime - videoPts);// > 0 表示视频落后于主时钟
    videoLateUs -= mVideoDelayTime;

    /*
         *  if stc is free, video rectify it
         */
    if ((llabs(videoLateUs) > 1000 * 1000) || (llabs(videoLateUs) > mSet->maxBufferDuration)) {
        
        if ((!mMasterClock.haveMaster() || !mMasterClock.isMasterValid()) && !mSeekFlag &&
            !mDiscontinuity.filterActive.load()) {
            /*
             * S6：暂停态切档期间**不锚时钟**。这里的第一张帧是"暂停点之前的关键帧
             * 到暂停点之间"解出来的，它的 pts 比用户暂停的位置更早或更晚都可能；
             * 一旦锚上去，用户暂停时看到的位置就被改写了（而且它还会通过
             * getCurrentPosition 推给界面）。暂停语义要求时间**绝对不动**。
             * 停用条件只在"暂停切档欠一帧"期间成立，播放态与普通 seek 不受影响。
             */
            mMasterClock.setTime(videoPts);
            masterPlayedTime = videoPts;
        }
    }

    /* video early more than 10ms, don't (render||drop) it, deal it next time
         * if the pts is not continue drop it

         ---------------------------------------early----------------------------|------------------------late--------------

         ----------------------------------|-------------------------------|-----|-------------|--------------------------------------->
         drop if pts discontinue    disDelta s       render next time     10ms   0            500ms     drop
            && audio reverting
                                                                          |--------render-----|


         */
    bool render = force_render;
    if (mDiscontinuity.filterActive.load()) {
        
        int64_t frameTimePos = videoPts;

        if (frameTimePos == INT64_MIN) {
            frameTimePos = videoFrame->getInfo().timePosition;
        }

        int64_t seekFrameDurUs = videoFrame->getInfo().duration;

        if (seekFrameDurUs <= 0) {
            
            const double seekFps = (mCurrentVideoMeta != nullptr)
                                   ? (double) (mCurrentVideoMeta->operator Stream_meta *()->avg_fps)
                                   : 0.0;

            if (seekFps > 1.0) {
                seekFrameDurUs = (int64_t) (1000000.0 / seekFps + 0.5);
            } else {
                seekFrameDurUs = 0;
            }
        }
        const bool dropForLanding = shouldDropForDiscontinuity(frameTimePos, seekFrameDurUs);

        const bool eofLanding = dropForLanding && videoDecoderEOS && mVideoFrameQue.size() == 1;

        if (eofLanding) {
            
            acceptDiscontinuityLandingFrame(frameTimePos, mDiscontinuity.generation.load(),
                                            "no frame can reach the target (eof)");

            if (mSeekDecodeStartIsKey) {
                force_render = true;
                render = true;
            } else {
                render = false;
            }
        } else if (!mSeekDecodeStartIsKey) {
            
            render = false;
        } else if (dropForLanding) {
            
            render = false;

            
            if (mVideoLandingDropLoggedGen != mDiscontinuity.generation.load()) {
                mVideoLandingDropLoggedGen = mDiscontinuity.generation.load();
                AF_LOGW("seek landing filter: dropping frame pts=%lld timePosition=%lld usedPos=%lld dur=%lld "
                        "target=%lld generation=%d master=%lld — frames before the target are never shown; "
                        "printed once per discontinuity (if the filter then never accepts, the frame position "
                        "is on a different axis than the target)\n",
                        (long long) videoFrame->getInfo().pts,
                        (long long) videoFrame->getInfo().timePosition,
                        (long long) frameTimePos, (long long) seekFrameDurUs,
                        (long long) mDiscontinuity.targetUs, mVideoLandingDropLoggedGen,
                        (long long) masterPlayedTime);
            }
        } else {
            /*
             * 包含目标（framePos <= target < framePos + 帧长），或者已经越过目标
             * （framePos > target：目标落在本段之前，没有更早的帧可选）⇒ **强制上屏**。
             * 判据已在同一调用里结束本次过滤；"不等时钟"就体现在这里（force_render 跳过
             * 下面的节拍判定），不是靠"先出一张前缀帧"。
             */
            force_render = true;
            render = true;
        }

        if (!mDiscontinuity.filterActive.load()) {
            /*
             * 判据刚刚结束了本次过滤（或本来就没激活）⇒ 收尾一次（幂等）。
             *   · setVideoDecodeBoost(false)：包含目标的那一帧已经到手，追帧前缀追完；
             *   · 两个 revert 闩清零：seek 不是"时间戳不连续"，而暂停态 seek（PFR）会让
             *     mVideoPtsRevert 置真、mAudioPtsRevert 因暂停而恒假 —— 不清零的话下面的
             *     PTS_REVERTING 等待分支会永久挡住渲染（seekFlag 永远清不掉 = 一直转圈加载）。
             */
            setVideoDecodeBoost(false);
            mVideoPtsRevert = false;
            mAudioPtsRevert = false;
            mPtsRevertWaitLogged = 0;
        }
    }

    if (!force_render) {
        
        if (!masterClockUnset && videoLateUs < -10 * 1000 &&
            (!mDemuxerService->getDemuxerHandle()->isTSDiscontinue() || videoLateUs > -mPtsDiscontinueDelta || !mAudioPtsRevert)) {
            return false;
        }

       
        if (dropLateVideoFrames) {
            
            const bool renderedRecently = mLastVideoFrameRenderedMs > 0 &&
                                          (af_getsteady_ms() - mLastVideoFrameRenderedMs) < JOINING_FORCE_RENDER_MS;

            if (!masterClockUnset && videoLateUs > JOINING_STALE_FRAME_US && renderedRecently) {
                render = false;
            } else {
                dropLateVideoFrames = false;
                render = true;
            }
        } else if (videoLateUs < 500 * 1000) {// video early litter than 10ms or late litter than 500ms,render it
            render = true;
        }
        // Try to render if already haven't render more then 60ms
        else if (INT64_MIN == mPlayedVideoPts || (videoPts - mPlayedVideoPts) > 60 * 1000) {
            render = true;
        }
    }

    
    const int64_t videoGapAbsUs = masterClockUnset ? INT64_MIN : llabs(videoLateUs);

    
    const bool catchUpValveTripped = (!mDiscontinuity.filterActive.load() && dropLateVideoFrames &&
                                      mCatchUpDiscardStreak >= VIDEO_CATCHUP_DISCARD_STREAK_MAX);

    
    const bool landingFilterActive = mDiscontinuity.filterActive.load();

    if (!render && !landingFilterActive && mPlayStatus == PLAYER_PLAYING && !masterClockUnset &&
        (mVideoDiscardStreak >= VIDEO_STUCK_DISCARD_STREAK_MAX || catchUpValveTripped)) {
        render = true;

        AF_LOGW("video frames were rejected without the picture being updated (consecutive=%d, "
                "catchUpConsecutive=%d, gap=%lld ms, master=%lld, video pts=%lld, playStatus=%d, "
                "seekFlag=%d, dropLate=%d) — forcing this frame to the screen so the picture keeps "
                "moving instead of freezing; the pacing/landing rules are untouched and the counters "
                "reset here\n",
                mVideoDiscardStreak, mCatchUpDiscardStreak,
                (long long) (videoGapAbsUs / 1000),
                (long long) masterPlayedTime, (long long) videoPts,
                (int) mPlayStatus, (int) mSeekFlag, (int) dropLateVideoFrames);
    }

    if (render) {
        /* 画面又在动了：清掉“追赶中”，主循环恢复正常的等待间隔
         * （读取点在 mainService()，见 mVideoCatchingUp 的说明）。 */
        mVideoCatchingUp = false;
        /* B16：有帧真的上屏 ⇒ 连续拒帧计数与偏移采样一起归零（下一次从 0 开始累计）。 */
        mVideoDiscardStreak = 0;
        mVideoDiscardGapAbsUs = INT64_MIN;
        /* B16-b：追赶期阀门同样在上屏后清零（下一次仍要连续 20 帧才放行）。 */
        mCatchUpDiscardStreak = 0;
        /*
         * 帧真的上屏了 —— 这是“视频路还活着”的**唯一**权威证据（丢帧不算，
         * 丢帧风暴里画面其实冻着）。管线真死时的一次性恢复就看这个时间戳：
         * 超过 VIDEO_RECOVER_STALL_MS（seek 在途时是 VIDEO_RECOVER_STUCK_SEEK_MS）
         * 没有刷新，并且视频包队列也不消费，才做动作（见 doRender() 的探测段）。
         */
        mLastVideoFrameRenderedMs = af_getsteady_ms();
        SendVideoFrameToRender(std::move(videoFrame));

        if (frameWidth != mVideoWidth || frameHeight != mVideoHeight) {
            mVideoWidth = frameWidth;
            mVideoHeight = frameHeight;
            mPNotifier->NotifyVideoSizeChanged(mVideoWidth, mVideoHeight);
        }

        
        if (mVideoSwitchInFlight && mCurrentVideoIndex == mVideoSwitchTargetIndex) {
            AF_LOGI("quality switch rendered: stream=%d size=%dx%d (READY driver: this frame went through "
                    "SendVideoFrameToRender)\n",
                    mVideoSwitchTargetIndex, frameWidth, frameHeight);
            finishQualitySwitch(true, "quality switch rendered");
        }

        if (!HAVE_AUDIO) {
            /* 纯视频片源：第一张真正上屏的帧就是时钟锚点。
             * 但 **seek 期间不能锚** —— 理由和上面 RenderVideo 里那段一样：
             * seek 目标点的时钟已经由 ProcessSeekToMsg 钉住，而这里的第一张帧可能
             * 是解码器冲到目标点后面 2~3 秒才解出来的，锚上去等于把时钟推飞。 */
            if (mPlayedVideoPts == INT64_MIN && !mSeekFlag && !mDiscontinuity.filterActive.load()) {
                /*
                 * S6：暂停态切档期间不锚时钟（理由同上一条 guard）。暂停切档可能
                 * 是"纯视频片源 + 暂停中切档"这种组合，此时 mPlayedVideoPts 恰好
                 * 可能还是 INT64_MIN，是最容易误锚的一条路。
                 */
                mMasterClock.setTime(videoPts);
                mMasterClock.setReferenceClock(mClockRef, mCRArg);
            }
        }
    } else {
        
        if (masterClockUnset) {
            AF_LOGW("drop frame,master played time is unset (clock not established yet),video pts is %lld\n",
                    (long long) videoPts);
        } else {
            AF_LOGW("drop frame,master played time is %lld,video pts is %lld\n",
                    (long long) masterPlayedTime, (long long) videoPts);
        }

        videoFrame->setDiscard(true);
        mUtil->videoRendered(false);
        mMPAUtil->videoRendered(false);
        mVideoCatchingUp = true;
        
        if (videoGapAbsUs != INT64_MIN) {
            mVideoDiscardGapAbsUs = videoGapAbsUs;

            if (mVideoDiscardStreak < VIDEO_STUCK_DISCARD_STREAK_MAX + 1) {
                ++mVideoDiscardStreak;
            }
        }
        
        if (dropLateVideoFrames && !mDiscontinuity.filterActive.load()) {
            if (mCatchUpDiscardStreak < VIDEO_CATCHUP_DISCARD_STREAK_MAX + 1) {
                ++mCatchUpDiscardStreak;
            }
        } else {
            mCatchUpDiscardStreak = 0;
        }
        /* 又丢了一帧：置“追赶中”，让主循环以最多 2ms 的间隔尽快再跑一轮
         * （mainService() 里读它；这就是这个标志现在唯一的用途）。 */

        if (mFrameCb && (!mSecretPlayBack || mDrmKeyValid)) {
            mFrameCb(mFrameCbUserData, videoFrame.get());
        }
        RenderCallback(ST_TYPE_VIDEO, false, videoFrame->getInfo());
    }

    mPlayedVideoPts = videoPts;
    mVideoFrameQue.pop();
    return render;
}

void SuperMediaPlayer::RenderSubtitle(int64_t pts)
{
    if (mSubPlayer) {
        mSubPlayer->update(getCurrentPosition());

        if (mSubPlayer->isActive()) {
            return;
        }
    }

    auto iter = mSubtitleShowedQueue.begin();

    while (iter != mSubtitleShowedQueue.end()) {
        if (*iter) {
            if (((*iter)->getInfo().pts + (*iter)->getInfo().duration) <= pts) {
                mPNotifier->NotifySubtitleEvent(subTitle_event_hide, (*iter).release(), 0, nullptr);
                iter = mSubtitleShowedQueue.erase(iter);
                continue;
            }
        }

        iter++;
    }

    int64_t subTitlePts = mBufferController->GetPacketPts(BUFFER_TYPE_SUBTITLE);

    if (subTitlePts == INT64_MIN || subTitlePts > pts) {
        return;
    }

    unique_ptr<IAFPacket> pFrame = mBufferController->getPacket(BUFFER_TYPE_SUBTITLE);

    if (pFrame == nullptr) {
        return;
    }

    bool changed = false;

    if (pFrame->getInfo().pts + pFrame->getInfo().duration < pts) {
        AF_LOGD("read subtitle pts is long before pts is %lld,subtitle pts is %lld", pts, subTitlePts);
        mBufferController->ClearPacketBeforePts(BUFFER_TYPE_SUBTITLE, subTitlePts);

        if (subTitlePts <= mSubtitleChangedFirstPts && pts > mSubtitleChangedFirstPts) {
            changed = true;
        }
    } else {
        if (subTitlePts == mSubtitleChangedFirstPts) {
            changed = true;
        }

        AF_LOGD("read subtitle pts show pts is %lld,subtitle pts is %lld", pts, subTitlePts);
        pFrame->getInfo().dts = mSubtitleShowIndex++;
        //          pFrame->pBuffer[pFrame->size] = 0;
        mPNotifier->NotifySubtitleEvent(subTitle_event_show, pFrame.get(), 0, nullptr);
        mSubtitleShowedQueue.push_back(std::move(pFrame));
    }

    if (changed) {
        AF_LOGD("subtitle changed");
        StreamInfo *info = GetCurrentStreamInfo(ST_TYPE_SUB);
        mPNotifier->NotifyStreamChanged(info, ST_TYPE_SUB);
        mSubtitleChangedFirstPts = INT64_MIN;
    }
}

void SuperMediaPlayer::OnTimer(int64_t curTime)
{
    if (mPlayedAudioPts != INT64_MIN || mPlayedVideoPts != INT64_MIN) {
        /*
             * if have seek not completed,DO NOT update the position,it will lead process bar
             * jumping
             */
        if ((mPlayStatus == PLAYER_PLAYING) && !isSeeking()) {
            //AF_LOGD("TIMEPOS OnTimer :%lld", getCurrentPosition());
            NotifyPosition(getCurrentPosition());
            NotifyUtcTime();
        }

        PostBufferPositionMsg();
    }

    mPNotifier->NotifyCurrentDownloadSpeed(mUtil->getCurrentDownloadSpeed());
}

void SuperMediaPlayer::SendVideoFrameToRender(unique_ptr<IAFFrame> frame, bool valid)
{
    if (mFrameCb && (!mSecretPlayBack || mDrmKeyValid)) {
        bool rendered = mFrameCb(mFrameCbUserData, frame.get());
        if (rendered) {
            RenderCallback(ST_TYPE_VIDEO, true, frame->getInfo());
            return;
        }
    }
    if (mAVDeviceManager->isVideoRenderValid()) {
        int ret = mAVDeviceManager->renderVideoFrame(frame);

        if (ret < 0) {
            AF_LOGE("renderFrame error \n");
            // for windows init failed, which may need change render type in future.
            mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_VIDEO_RENDER_INIT_ERROR, "init video render failed");
        }
    } else {
        assert(0);
        //render directly
        RenderCallback(ST_TYPE_VIDEO, !frame->getDiscard(), frame->getInfo());
    }
}

int SuperMediaPlayer::DecodeAudio(unique_ptr<IAFPacket> &pPacket)
{
    if (audioDecoderEOS) {
        return 0;
    }

    unique_ptr<IAFFrame> frame{};
    int ret;

    do {
        ret = mAVDeviceManager->getFrame(frame, SMPAVDeviceManager::DEVICE_TYPE_AUDIO, 0);
        if (ret == STATUS_EOS) {
            audioDecoderEOS = true;
            break;
        }

        if (frame != nullptr) {

            int64_t duration = mBufferController->GetPacketDuration(BUFFER_TYPE_AUDIO);
            if (duration < 0) {
                //If audio duration is unknow when demux , update duration after decode one frame.
                IAFFrame::AFFrameInfo frameInfo = frame->getInfo();
                int64_t packetDuration = (int64_t) frameInfo.audio.nb_samples * 1000000 / frameInfo.audio.sample_rate;
                mBufferController->SetOnePacketDuration(BUFFER_TYPE_AUDIO, packetDuration);
            }

            if (mRecorderSet->decodeFirstAudioFrameInfo.waitFirstFrame) {
                DecodeFirstFrameInfo &info = mRecorderSet->decodeFirstAudioFrameInfo;
                info.getFirstFrameTimeMs = af_getsteady_ms();
                info.waitFirstFrame = false;
            }

            if (mSecretPlayBack) {
                frame->setProtect(true);
            }
            if (frame->getInfo().pts == INT64_MIN) {
                // TODO: why mAudioFrameQue.back()->getInfo().pts is INT64_MIN
                if (!mAudioFrameQue.empty() && mAudioFrameQue.back()->getInfo().pts != INT64_MIN) {
                    duration = ((double) frame->getInfo().audio.nb_samples) / frame->getInfo().audio.sample_rate;
                    frame->getInfo().pts = mAudioFrameQue.back()->getInfo().pts + duration * 1000000;
                } else {
                    //                       assert(0);
                }
            }
            mDemuxerService->SetOption("A_FRAME_DECODED", frame->getInfo().pts);
            mAudioFrameQue.push_back(std::move(frame));
        }
    } while (ret != -EAGAIN && ret != -EINVAL);

    if (!mRecorderSet->decodeFirstAudioFrameInfo.isFirstPacketSendToDecoder) {
        DecodeFirstFrameInfo &info = mRecorderSet->decodeFirstAudioFrameInfo;
        info.isFirstPacketSendToDecoder = true;
        info.waitFirstFrame = true;
        info.firstPacketSize = pPacket->getSize();
        info.firstPacketPts = pPacket->getInfo().pts;
        info.sendFirstPacketTimeMs = af_getsteady_ms();
    }

    ret = mAVDeviceManager->sendPacket(pPacket, SMPAVDeviceManager::DEVICE_TYPE_AUDIO, 0);

    if (ret > 0) {
        bool haveError = false;

        if (ret & STATUS_HAVE_ERROR) {
            if (mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_AUDIO)->get_error_frame_no() > MAX_DECODE_ERROR_FRAME) {
                haveError = true;
            }
        }

        if (ret & STATUS_DRM_ERROR) {
            haveError = true;
        }

        if (ret & STATUS_CREATE_FAIL) {
            haveError = true;
        }

        if (ret &= STATUS_RETRY_IN) {
            ret = -EAGAIN;
        }

        if (haveError) {
            ChangePlayerStatus(PLAYER_ERROR);
            mPNotifier->NotifyError(MEDIA_PLAYER_ERROR_DECODE_AUDIO, "audio decode error");
        }
    }

    return ret;
}

void SuperMediaPlayer::ProcessOpenStreamInit(int streamIndex)
{
    AF_LOGD("ProcessOpenStreamInit ProcessOpenStreamInit start");
    std::deque<StreamInfo *> &streamInfoQueue = mMediaInfo.mStreamInfoQueue;
    int streamCount = (int) streamInfoQueue.size();
    int videoStreams = 0;

    for (int i = 0; i < streamCount; i++) {
        StreamInfo *info = streamInfoQueue[i];

        if (info->type == ST_TYPE_VIDEO) {
            videoStreams++;
        }
    }

    if (videoStreams > 1) {
        mAdaptiveVideo = true;
    } else {
        mAdaptiveVideo = false;
    }

    if (mMixMode) {
        if (mCurrentVideoIndex < 0 && mCurrentAudioIndex < 0) {
            unique_ptr<streamMeta> pMeta;
            Stream_meta *meta{};
            int nbSubStream = mDemuxerService->GetNbSubStream(mMainStreamId);

            for (int j = 0; j < nbSubStream; j++) {
                mDemuxerService->GetStreamMeta(pMeta, GEN_STREAM_ID(mMainStreamId, j), true);
                meta = (Stream_meta *) (pMeta.get());
                AF_LOGD("get a stream %d\n", meta->type);

                if (!mSet->bDisableVideo && meta->type == STREAM_TYPE_VIDEO && mCurrentVideoIndex < 0 && meta->height > 0 &&
                    meta->attached_pic == 0) {
                    AF_LOGD("get a video stream\n");
                    mCurrentVideoIndex = GEN_STREAM_ID(mMainStreamId, j);
                    mVideoInterlaced = meta->interlaced;
                    updateVideoMeta();
                } else if (!mSet->bDisableAudio && !mOpenAudioDeviceFailed && meta->type == STREAM_TYPE_AUDIO && mCurrentAudioIndex < 0 && meta->channels > 0) {
                    AF_LOGD("get a audio stream\n");
                    mCurrentAudioIndex = GEN_STREAM_ID(mMainStreamId, j);
                    mCATimeBase = meta->ptsTimeBase;
                } else if (meta->type == STREAM_TYPE_SUB && mCurrentSubtitleIndex < 0) {
                    AF_LOGD("get a subtitle stream\n");
                    mCurrentSubtitleIndex = GEN_STREAM_ID(mMainStreamId, j);
                    if (meta->extradata && meta->extradata_size > 0) {
                        mPNotifier->NotifySubtitleHeader(mCurrentSubtitleIndex, (const char *) meta->extradata);
                    }
                }
            }
        }
    }

    mPNotifier->CancelNotifyMediaInfo();
    //post before PostMsg(PreparedReq). sdk will get these infos on Prepared callback.
    mPNotifier->NotifyMediaInfo(&mMediaInfo);
}

void SuperMediaPlayer::setUpAVPath()
{
    if (!mInited) {
        return;
    }

    if (!mSet->mFastStart && mPlayStatus < PLAYER_PLAYING) {
        AF_LOGI("not fast start mode\n");
        return;
    }

    if (mCurrentAudioIndex >= 0 &&
        (!mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_AUDIO) || !mAVDeviceManager->isAudioRenderValid())) {
        AF_LOGD("SetUpAudioPath start");
        int ret = SetUpAudioPath();

        if (ret < 0) {
            AF_LOGE("%s SetUpAudioPath failed,url is %s %s", __FUNCTION__, mSet->url.c_str(), framework_err2_string(ret));
            mOpenAudioDeviceFailed = true;
            closeAudio();
            mCATimeBase = 0;
        } else {
        }
    }

    if (mCurrentVideoIndex >= 0) {
        int ret = SetUpVideoPath();

        if (ret < 0) {
            AF_LOGE("%s SetUpVideoPath failed,url is %s %s", __FUNCTION__, mSet->url.c_str(), framework_err2_string(ret));
            closeVideo();
        }
    }

    if (mCurrentVideoIndex < 0 && mCurrentAudioIndex < 0) {
        ChangePlayerStatus(PLAYER_ERROR);
        mPNotifier->NotifyError(MEDIA_PLAYER_ERROR_DEMUXER_NO_VALID_STREAM, "No valid stream");
    }
}

bool SuperMediaPlayer::NeedDrop(int64_t pts, int64_t refer)
{
    return (pts < refer) && (pts < mDuration - 200 * 1000);
}

int SuperMediaPlayer::ReadPacket()
{
    IAFPacket *pFrame = nullptr;
    std::unique_ptr<IAFPacket> pMedia_Frame{};

    if (mDemuxerService == nullptr) {
        assert(0);
    }

    int index = -1;

    if (HAVE_SUBTITLE && !mSubtitleEOS) {
        if (mBufferController->GetPacketDuration(BUFFER_TYPE_SUBTITLE) <= 0) {
            if (mSubtitleChangedFirstPts != INT64_MIN || 1) {
                index = mCurrentSubtitleIndex;
            }
        }
    }

    int ret = mDemuxerService->readPacket(pMedia_Frame, index);

    if (pMedia_Frame == nullptr) {
        //  AF_LOGD("Can't read packet %d\n", ret);
        if (ret == 0) {
            mSubtitleEOS = true;

            if (index != -1) {
                ret = -EAGAIN;
            }
        }

        return ret;
    }

    if (mPtsDiscontinueDelta == INT64_MIN) {
        int64_t maxGopTimeUs = mDemuxerService->getDemuxerHandle()->getMaxGopTimeUs();
        if (maxGopTimeUs > 0) {
            mPtsDiscontinueDelta = maxGopTimeUs;
        } else {
            mPtsDiscontinueDelta = PTS_DISCONTINUE_DELTA;
        }
        AF_LOGI("mPtsDiscontinueDelta = %lld", mPtsDiscontinueDelta);
    }

    // FIXME: transfer to frame
    if (pMedia_Frame->isProtected() && !mSecretPlayBack) {
        AF_LOGI("SecretPlayBack\n");
        mSecretPlayBack = true;

        if (!pMedia_Frame->getMagicKey().empty() && pMedia_Frame->getMagicKey() == mSet->drmMagicKey) {
            mDrmKeyValid = true;
        }
    }

    pFrame = pMedia_Frame.get();
    uint64_t size = mCalculateSpeedUsePacket ? pFrame->getSize() : 0;
    mUtil->notifyRead(MediaPlayerUtil::readEvent_Got, size);
    mMPAUtil->updateNetworkReadSize(size);

    // TODO: get the min first stream pts
    if (pFrame->getInfo().timePosition >= 0 && mMediaStartPts == INT64_MIN && pFrame->getInfo().streamIndex != mCurrentSubtitleIndex &&
        pFrame->getInfo().streamIndex != mWillChangedSubtitleStreamIndex) {
        mMediaStartPts = pFrame->getInfo().pts - pFrame->getInfo().timePosition;
    }

    if (mSeekFlag && mSeekNeedCatch) {
        if (pFrame->getInfo().timePosition < (mSeekPos - mSet->maxASeekDelta)) {
            // first frame is far away from seek position, don't suppport accurate seek
            mSeekNeedCatch = false;
        }
    }

    int id = GEN_STREAM_INDEX(pFrame->getInfo().streamIndex);

    if (mDuration < 0) {
        unique_ptr<streamMeta> pMeta;
        mDemuxerService->GetStreamMeta(pMeta, pFrame->getInfo().streamIndex, false);
        mDuration = ((Stream_meta *) (pMeta.get()))->duration;
    }

    std::deque<StreamInfo *> &streamInfoQueue = mMediaInfo.mStreamInfoQueue;
    if (id < streamInfoQueue.size() && streamInfoQueue[id]->type == ST_TYPE_VIDEO && mMainStreamId != -1 && id != mMainStreamId) {
        unique_ptr<streamMeta> pMeta;
        Stream_meta *meta{};
        int count = mDemuxerService->GetNbSubStream(id);

        for (int i = 0; i < count; i++) {
            mDemuxerService->GetStreamMeta(pMeta, GEN_STREAM_ID(id, i), true);
            meta = (Stream_meta *) (pMeta.get());
            int streamId = GEN_STREAM_ID(id, i);

            if (meta->type == STREAM_TYPE_VIDEO && meta->height > 0 && streamId != mCurrentVideoIndex) {
                /*
                 * 【单解码器模型】清单里主视频档变了（demuxer 报告的 period/自适应切换）
                 * ⇒ 走**既有**切档入口：SwitchStream → MSG_CHANGE_VIDEO_STREAM →
                 * SMPMessageControllerListener::switchVideoStream() → SwitchVideo()。
                 * 切档在途时不重复发请求（监听类对"同一目标档"本身也会去重）。
                 * 旧模型在这里只记一个"即将切换到的视频流索引"，等"新流第一个包"
                 * 到了才启动 pending 机器 —— 那条路已随 pending 机器整体删除。
                 */
                if (!mVideoSwitchInFlight) {
                    SwitchStream(streamId);
                }
            } else if (meta->type == STREAM_TYPE_AUDIO && meta->channels > 0 && streamId != mCurrentAudioIndex) {
                mWillChangedAudioStreamIndex = streamId;
                mWATimeBase = meta->ptsTimeBase;
            } else if (meta->type == STREAM_TYPE_SUB && streamId != mCurrentSubtitleIndex) {
                mWillChangedSubtitleStreamIndex = streamId;
            }
        }

        mMainStreamId = id;
    }

    if (!mInited) {
        ProcessOpenStreamInit(pFrame->getInfo().streamIndex);
        ProcessUpdateView();
        mInited = true;
    }

    //        AF_LOGD("read packet pts is %lld,streamIndex is %d duration is %d\n", pFrame->getInfo().pts, pFrame->getInfo().streamIndex,
    //                pFrame->getInfo().duration);

    if (pFrame->getInfo().streamIndex == mCurrentVideoIndex ||
        (mVideoSwitchInFlight && pFrame->getInfo().streamIndex == mVideoSwitchTargetIndex)) {
        mHaveVideoPkt = true;
        if (mVideoSwitchInFlight && mDemuxerService != nullptr && mVideoSwitchTargetIndex >= 0 &&
            pFrame->getInfo().streamIndex == mVideoSwitchTargetIndex) {
            const std::string alignedResult =
                    mDemuxerService->GetProperty(mVideoSwitchTargetIndex, "alignedSwitchResult");

            if (!alignedResult.empty() && alignedResult[0] == '-') {
                AF_LOGW("quality switch (aligned): the demuxer could not switch to stream %d (result=%s) — reporting "
                        "PLAYER_QUALITY_SWITCH_FAILED and dropping this stale packet of the aborted target\n",
                        mVideoSwitchTargetIndex, alignedResult.c_str());
                mPNotifier->NotifyVideoQualitySwitch(PLAYER_QUALITY_SWITCH_FAILED, mVideoSwitchTargetIndex,
                                                     "target segment does not exist");
                finishQualitySwitch(false, "target segment does not exist");
                pMedia_Frame->setDiscard(true);
                return ret;
            }
        }
#ifdef ENABLE_CACHE_MODULE
        
        sendMediaFrameToCache(pMedia_Frame.get(), ST_TYPE_VIDEO);
#endif
        if (mMediaFrameCb && (!pMedia_Frame->isProtected() || mDrmKeyValid)) {
            mMediaFrameCb(mMediaFrameCbArg, pMedia_Frame.get(), ST_TYPE_VIDEO);
        }

        
        if (mVideoSwitchInFlight && pFrame->getInfo().streamIndex == mVideoSwitchTargetIndex &&
            mCurrentVideoIndex != mVideoSwitchTargetIndex &&
            
            mDemuxerService != nullptr) {
           
           
            const int64_t firstPacketPosUs =
                    (pFrame->getInfo().pts != INT64_MIN && mActiveVideoPtsOffset != INT64_MIN)
                    ? pFrame->getInfo().pts + mActiveVideoPtsOffset
                    : INT64_MIN;

            commitAlignedVideoSwitch(firstPacketPosUs, pFrame->getInfo().flags != 0);
        }

        
        if (mVideoSwitchInFlight && pFrame->getInfo().streamIndex != mCurrentVideoIndex &&
            pFrame->getInfo().streamIndex != mVideoSwitchTargetIndex) {
            pMedia_Frame->setDiscard(true);
            return ret;
        }

        mBufferController->AddPacket(std::move(pMedia_Frame), BUFFER_TYPE_VIDEO);
        mDemuxerService->SetOption("V_FRAME_RECEIVE", pFrame->getInfo().pts);

        if (mVideoInterlaced == InterlacedType_UNKNOWN) {
            if (mVideoParser == nullptr) {
                //FIXME mCurrentVideoMeta can not be update here.
                mDemuxerService->GetStreamMeta(mCurrentVideoMeta, pFrame->getInfo().streamIndex, false);
                auto *meta = (Stream_meta *) (mCurrentVideoMeta.get());
                mVideoParser = new bitStreamParser();
                mVideoParser->init(meta);

                {
#ifdef ENABLE_VIDEO_FILTER
                    std::lock_guard<std::mutex> filterLock(mFilterManagerMutex);
                    if (mFilterManager != nullptr) {
                        mFilterManager->setStreamMeta(meta);
                    }
#endif
                }
            }

            mVideoParser->parser(pFrame->getData(), static_cast<int>(pFrame->getSize()));
            mVideoInterlaced = mVideoParser->getInterlaced();

            if (mVideoInterlaced != InterlacedType_UNKNOWN) {
                delete mVideoParser;
                mVideoParser = nullptr;
            } else {
                mVideoParserTimes++;

                if (mVideoParserTimes > 10) {
                    mVideoInterlaced = InterlacedType_NO;
                    delete mVideoParser;
                    mVideoParser = nullptr;
                    mVideoParserTimes = 0;
                }
            }
        }

        if (mFirstVideoPts == INT64_MIN) {
            mFirstVideoPts = pFrame->getInfo().pts - pFrame->getInfo().timePosition;

            if (!HAVE_AUDIO) {
                mFirstSeekStartTime = pFrame->getInfo().timePosition;
            }
        }

        if (mSoughtVideoPos == INT64_MIN) {
            if (mSeekNeedCatch) {
                mSoughtVideoPos = mSeekPos;
            } else {
                mSoughtVideoPos = pFrame->getInfo().timePosition;
            }

            /*
                 * seek would clean the packet which have ExtraData in decoder queue,
                 * so add the ExtraData after seek on key frame
                 */
            /* 这里原来还有一个 `mAdaptiveVideo &&` 的条件：该状态随双解码器/自适应切档机器一起删除，
             * 现在只看"是不是关键帧包"。 */
            if (pFrame->getInfo().flags) {
                unique_ptr<streamMeta> pMeta;
                mDemuxerService->GetStreamMeta(pMeta, pFrame->getInfo().streamIndex, false);
                pFrame->setExtraData(((Stream_meta *) (*pMeta))->extradata, ((Stream_meta *) (*pMeta))->extradata_size);
            }
        }

    } else if (pFrame->getInfo().streamIndex == mCurrentAudioIndex || pFrame->getInfo().streamIndex == mWillChangedAudioStreamIndex) {
        // printTimePosition(pFrame->getInfo().timePosition);
        mHaveAudioPkt = true;

        if (mFirstAudioPts == INT64_MIN) {
            mFirstAudioPts = pFrame->getInfo().pts - pFrame->getInfo().timePosition;
            mFirstSeekStartTime = pFrame->getInfo().timePosition;
        }

        if (mSeekFlag && mSeekExactLanding && mSeekNeedCatch &&
            NeedDrop(pFrame->getInfo().timePosition, mSeekPos)) {
            return ret;
        }

        if (pFrame->getInfo().streamIndex == mWillChangedAudioStreamIndex) {
            mCurrentAudioIndex = mWillChangedAudioStreamIndex;
            mCATimeBase = mWATimeBase;
            mWillChangedAudioStreamIndex = -1;
        }

        if (mAudioChangedFirstPts == INT64_MAX) {
            int64_t playedTime = mMasterClock.GetTime();

            if (pFrame->getInfo().pts < playedTime) {
                return ret;
            } else {
                //recodr 64MAX for audio stream changed for first frame
                mAudioChangedFirstPts = pFrame->getInfo().pts;
            }
        }

#ifdef ENABLE_CACHE_MODULE
        /* 【播放缓存】音频编码包，同上。 */
        sendMediaFrameToCache(pMedia_Frame.get(), ST_TYPE_AUDIO);
#endif
        if (mMediaFrameCb && (!pMedia_Frame->isProtected() || mDrmKeyValid)) {
            mMediaFrameCb(mMediaFrameCbArg, pMedia_Frame.get(), ST_TYPE_AUDIO);
        }

        mBufferController->AddPacket(std::move(pMedia_Frame), BUFFER_TYPE_AUDIO);
        mDemuxerService->SetOption("A_FRAME_RECEIVE", pFrame->getInfo().pts);
    } else if (pFrame->getInfo().streamIndex == mCurrentSubtitleIndex || pFrame->getInfo().streamIndex == mWillChangedSubtitleStreamIndex) {
#ifdef ENABLE_CACHE_MODULE
        /* 【播放缓存】字幕包（缓存文件里的字幕轨），同上。 */
        sendMediaFrameToCache(pMedia_Frame.get(), ST_TYPE_SUB);
#endif
        if (mMediaFrameCb && (!pMedia_Frame->isProtected() || mDrmKeyValid)) {
            mMediaFrameCb(mMediaFrameCbArg, pMedia_Frame.get(), ST_TYPE_SUB);
        }

        mBufferController->AddPacket(std::move(pMedia_Frame), BUFFER_TYPE_SUBTITLE);
        AF_LOGD("read subtitle pts is %lld", pFrame->getInfo().pts);

        if (pFrame->getInfo().streamIndex == mWillChangedSubtitleStreamIndex) {
            mCurrentSubtitleIndex = mWillChangedSubtitleStreamIndex;
            mWillChangedSubtitleStreamIndex = -1;
        }
        if (mSubtitleChangedFirstPts == INT64_MAX) {
            mSubtitleChangedFirstPts = pFrame->getInfo().pts;
        }
    } else {
        if (floodLogAllowed(FLOOD_UNKNOWN_STREAM, 2, "unknown stream, read packet dropped")) {
            AF_LOGW("unknown stream %x, read packet pts is %lld; currentVideo=%d targetVideo=%d currentAudio=%d\n",
                    pFrame->getInfo().streamIndex, pFrame->getInfo().pts,
                    mCurrentVideoIndex, mVideoSwitchTargetIndex,
                    mCurrentAudioIndex);
        }

        if (mCurrentAudioIndex < 0 || mCurrentVideoIndex < 0) {
            std::unique_ptr<streamMeta> meta;
            int ret1 = mDemuxerService->GetStreamMeta(meta, pFrame->getInfo().streamIndex, true);
            if (ret1 >= 0) {
                switch (((Stream_meta *) (*meta))->type) {
                    case STREAM_TYPE_VIDEO: {
                        if (!mSet->bDisableVideo && mCurrentVideoIndex < 0 && ((Stream_meta *) (*meta))->width > 0) {
                            mCurrentVideoIndex = pFrame->getInfo().streamIndex;
                            updateVideoMeta();
                        }
                        break;
                    }
                    case STREAM_TYPE_AUDIO: {
                        if (!mSet->bDisableAudio && !mOpenAudioDeviceFailed && mCurrentAudioIndex < 0 && ((Stream_meta *) (*meta))->channels > 0) {
                            mCurrentAudioIndex = pFrame->getInfo().streamIndex;
                            mCATimeBase = ((Stream_meta *) (*meta))->ptsTimeBase;
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    }

    if (mWillSwitchVideo) {
        int videoCount = 0;
        int64_t startTime = mBufferController->FindSeamlessPointTimePosition(BUFFER_TYPE_VIDEO, videoCount);

        if (startTime == 0 || videoCount < 40) {
            return ret;
        }

        if (mMixMode) {
            int64_t startTimeA = mBufferController->FindSeamlessPointTimePosition(BUFFER_TYPE_AUDIO, videoCount);

            if (startTimeA == 0 || videoCount < 40) {
                return ret;
            }

            startTime = std::max(startTime, startTimeA);
        }

        SwitchVideo(startTime);
        mWillSwitchVideo = false;
    }

    return ret;
}

void SuperMediaPlayer::printTimePosition(int64_t time) const
{
    if (time > 0) {
        time /= 1000;
        int timeM = static_cast<int>((time / 1000) / 60);
        int timeS = static_cast<int>((time / 1000) % 60);
        int timeMS = static_cast<int>(time - timeM * 60000 - timeS * 1000);
        AF_LOGD("timePosition is %d:%d:%d\n", timeM, timeS, timeMS);
    }
}

void SuperMediaPlayer::logAudioSilence(int reason, const char *detail, int64_t audioPts, int64_t clockUs)
{
    /*
     * 静音窗口**只在状态变化时**各打一条（开始一条、结束一条）。
     *   · 不是周期日志：reason 与上次相同就直接返回，所以不会随时间刷屏；
     *   · 也不是判据：本函数不改变任何播放行为，只把"现在有没有声音、为什么"写进日志，
     *     让下一份日志能直接区分是 1（内核等时钟）还是 2/3（设备侧欠载/被重建）。
     * 编码定义见文件头部 AUDIO_SILENCE_*；设备侧同名编码在 AudioTrackRender.cpp。
     */
    if (reason == mAudioSilenceReason) {
        return;
    }

    if (reason == AUDIO_SILENCE_NONE) {
        AF_LOGW("audio silence ends (was reason=%d): pcm flowing again, audioPts=%lld masterClock=%lld\n",
                (int) mAudioSilenceReason, (long long) audioPts, (long long) clockUs);
    } else {
        AF_LOGW("audio silence starts (reason=%d audioPts=%lld masterClock=%lld): %s\n",
                (int) reason, (long long) audioPts, (long long) clockUs,
                detail != nullptr ? detail : "");
    }

    mAudioSilenceReason = reason;
}

void SuperMediaPlayer::FlushAudioPath()
{
    /* 只有 seek 需要把主时钟重新锚到下一张音频帧。普通的缓冲追赶、停止或
     * 内部重建也会调用本函数，但这些场景不能无条件改写正在运行的主时钟，
     * 否则会把一次短暂的音频 flush 变成新的 A/V 跳变。 */
    const bool reanchorAfterFlush = mSeekFlag;
    /* 基准重设要用到"flush 之前的内容位置"，必须在下面把它清成 INT64_MIN 之前读。 */
    const int64_t contentPosBeforeFlush = mPlayedAudioPts;

    
    const int64_t consumedBeforeFlushUs = mAVDeviceManager->getAudioRenderPosition();

    mAVDeviceManager->flushDevice(SMPAVDeviceManager::DEVICE_TYPE_AUDIO);

    
    if (reanchorAfterFlush && mDiscontinuity.targetUs != INT64_MIN) {
        pinAudioClockBase(mDiscontinuity.targetUs);
    } else {
        const int64_t oldBaseUs = mDiscontinuity.audioBaseUs.load();
        const int64_t oldConsumedUs = mDiscontinuity.audioBaseConsumedUs.load();
        const bool consumedBeforeFlushValid =
                (consumedBeforeFlushUs >= 0 && !af_clock_value_is_unset(consumedBeforeFlushUs));

        if (oldBaseUs != INT64_MIN && oldConsumedUs != INT64_MIN && consumedBeforeFlushValid) {
            pinAudioClockBase(oldBaseUs + (consumedBeforeFlushUs - oldConsumedUs));
        } else if (contentPosBeforeFlush != INT64_MIN) {
            pinAudioClockBase(contentPosBeforeFlush);
        } else {
            pinAudioClockBase(INT64_MIN);
        }
    }

    audioDecoderEOS = false;

    //flush frame queue
    while (!mAudioFrameQue.empty()) {
        mAudioFrameQue.pop_front();
    }

    mPlayedAudioPts = INT64_MIN;
    mAudioPtsRevert = false;
    mAudioTime.startTime = 0;
    mAudioTime.deltaTime = 0;
    mAudioTime.deltaTimeTmp = 0;
    mAudioPacket = nullptr;
    mAudioEOS = false;
    // seek 后第一张音频帧重新建立音频时间轴；不能沿用旧流的 PTS 修正状态。
    mFirstAudioPts = INT64_MIN;
    mFirstSeekStartTime = 0;
    mRemovedFirstAudioPts = INT64_MIN;
}

void SuperMediaPlayer::pinAudioClockBase(int64_t baseUs)
{
    const int64_t consumedUs = mAVDeviceManager->getAudioRenderPosition();

    mDiscontinuity.audioBaseUs = baseUs;
    mDiscontinuity.audioBaseConsumedUs =
            (consumedUs >= 0 && !af_clock_value_is_unset(consumedUs)) ? consumedUs : INT64_MIN;
}

void SuperMediaPlayer::FlushVideoPath(bool flushRender, bool cancelPendingSwitch, const char *from)
{
    /*
     * 每次调用都留一行（限频）：本函数会顺手结束"在途的切档"（必要时发 FAILED 终态），
     * 没有这行日志时"seek 之后切换状态到底是谁清的"只能靠猜。
     */
    AF_LOGW("FlushVideoPath from %s (flushRender=%d cancelPendingSwitch=%d): current=%d "
            "switchInFlight=%d seekFlag=%d seekNeedCatch=%d\n",
            from != nullptr ? from : "?", (int) flushRender, (int) cancelPendingSwitch,
            mCurrentVideoIndex, (int) mVideoSwitchInFlight,
            (int) mSeekFlag, (int) mSeekNeedCatch);

    
    mAVDeviceManager->releaseVideoRenderFrames();

    /*
     * 【入口快照：这一次 flush 是不是"外力中止了一次在途切档"】
     * 单解码器模型下切档只有一个在途闩，所以下面那条终态出口与这个快照同源；
     * 记成局部量是为了在函数末尾（所有状态清干净之后）补做被推迟的 PFR / 用户 seek。
     */
    const bool switchCancelNotifiedOnEntry = (cancelPendingSwitch && mVideoSwitchInFlight);

    if (flushRender) {
        mAVDeviceManager->flushDevice(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);
    } else {
        
        IDecoder *videoDecoder = mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);

        if (videoDecoder != nullptr) {
            videoDecoder->flush();
        }
    }

    videoDecoderEOS = false;
    //flush frame queue

    while (!mVideoFrameQue.empty()) {
        mVideoFrameQue.front()->setDiscard(true);
        mMsgCtrlListener->ProcessRenderedMsg(ST_TYPE_VIDEO, mVideoFrameQue.front()->getInfo(), af_getsteady_ms(), false, nullptr);
        mVideoFrameQue.pop();
    }
    /*
     * 只有**外力**的 flush 才算"打断在途切档"：seek / stop / 换源 / 后台 flush 都传
     * cancelPendingSwitch=1（见各调用点）。切档**自己**那次内部 flush
     * （SwitchVideo 里的 "quality switch immediate"）传的是 0 —— 它是切换流程的一部分，
     * 绝不允许在这里把自己判成 FAILED。
     *
     * 真机实测（2026-09-27 Qt 播放器，DASH ABR 升档）就是这个形态：
     *   status=0 STARTED → 内部 flush 立刻 status=2 FAILED("video path flushed") →
     *   46ms 后 "single-decoder switch applied"（切换其实成功了）→
     *   落点帧 250ms 后正常被采纳，但 mVideoSwitchInFlight 已被清零 ⇒ 再也不发 READY；
     *   界面把高亮退回旧档，用户手动再点新档又被判"already the playing video stream"⇒ 看起来"点了没反应"。
     */
    if (cancelPendingSwitch && mVideoSwitchInFlight) {
        finishQualitySwitch(false, "video path flushed (seek/stop/catch-up)");
    }

    /*
     * 这里原来要"立即释放退役解码器，防止下一次 seek 长时间占着旧硬解 surface"。
     * 单解码器模型下不存在退役解码器：切换时旧流当场 CloseStream、解码器原地重建，
     * 任何时刻只有一块视频解码器，没有第二个 surface 需要回收。
     */

    mActiveVideoPtsOffset = INT64_MIN;
    /* B10：时间轴重建（seek/stop/换源）时逐帧配对表必须一起丢掉，否则旧代的 pts 可能撞上新帧 */
    mVideoAxisPts.clear();
    mVideoAxisTimePos.clear();

    
    mSeekDecodeStartIsKey = false;

    mPlayedVideoPts = INT64_MIN;
    mCurVideoPts = INT64_MIN;
    videoDecoderFull = false;
    mVideoPtsRevert = false;
    // seek 会建立全新的音视频时间轴；切换清晰度时保留音频时钟，但 seek 时必须
    // 清掉上一段流的音频滤波器累计偏移和输出队列，避免 DASH seek 后出现几十秒级
    // mDeltaPts，导致音频和视频再次脱节。
    mVideoPacket = nullptr;
    dropLateVideoFrames = false;
    mVideoCatchingUp = false;
    mVideoEOS = false;

    
    if (switchCancelNotifiedOnEntry) {
        runDeferredPauseFrameRestore();
        /*
         * 切档被这次 flush 打成 FAILED 终态时，finishQualitySwitch() 那条出口也会补
         * 做一次；这里再兜一次：入口内先清闩、不递归，且它自己会用播放状态把
         * stop / 换源 / 后台 flush 这三种情况丢掉。
         * 普通 seek 的 flush（cancelPendingSwitch=0）不进这个分支。
         */
        replayDeferredUserSeek();
    }
}

void SuperMediaPlayer::FlushSubtitleInfo()
{
    while (!mSubtitleShowedQueue.empty()) {
        if (mSubtitleShowedQueue.front()) {
            mPNotifier->NotifySubtitleEvent(subTitle_event_hide, mSubtitleShowedQueue.front().release(), 0, nullptr);
        }

        mSubtitleShowedQueue.pop_front();
    }

    mSubtitleShowedQueue.clear();
    mSubtitleShowIndex = 0;
    mSubtitleEOS = false;
    if (mSubPlayer) {
        mSubPlayer->flush();
    }
}

void SuperMediaPlayer::PostBufferPositionMsg()
{
    if (mPlayStatus == PLAYER_PAUSED || mPlayStatus == PLAYER_PLAYING) {
        
        if (isSeeking()) {
            return;
        }

        int64_t duration = getPlayerBufferDuration(false, false);
        /*
         * 当前位置：seek 在途时它等于 mSeekPos；seek 已宣告结束、管道还在从落点往目标
         * 追赶时它由不连续点的目标点基准给出（见 getCurrentPosition()）。
         */
        const int64_t position = getCurrentPosition();

        /*
         * 本地/单文件源：缓冲位置 = **已读入的字节范围**映射到时间轴（按总长线性换算）。
         *
         * 为什么不用 getBufferDuration() 那条路：它的实现依赖缓存模块（本地不走缓存时默认返回 0），
         * 于是缓冲值退化成读前窗口里的包队列深度 —— 既不等于"已读入多少"，又会随队列锯齿小幅回缩。
         * 判据是数据源能否报出读取游标与总长（IDataSource::getReadPosition/getTotalLength，
         * 分片流与网络流返回 -1）⇒ 报不出来就照旧走下面的原算法，不影响 DASH/HLS。
         * 往回 seek 后文件游标也回到落点 ⇒ 这里的值同步变小，界面看到的就是真实的已读范围。
         */
        if (mDuration > 0 && mDemuxerService != nullptr) {
            const int64_t readBytes = mDemuxerService->getSourceReadPosition();
            const int64_t totalBytes = mDemuxerService->getSourceTotalLength();

            if (readBytes > 0 && totalBytes > 0) {
                int64_t buffered = static_cast<int64_t>(static_cast<double>(mDuration) *
                                                        static_cast<double>(readBytes) /
                                                        static_cast<double>(totalBytes));

                if (buffered > mDuration) {
                    buffered = mDuration;
                }

                /* 落点刚开始读、游标还没追到播放头时（buffered < position）交给下面的原算法。 */
                if (buffered >= position) {
                    mBufferPosition = buffered;
                    mPNotifier->NotifyBufferPosition((mBufferPosition <= mDuration ? mBufferPosition : mDuration) / 1000);
                    return;
                }
            }
        }

        if (duration >= 0) {
            mBufferPosition = position + duration;
        } else if (!mDiscontinuity.filterActive.load()) {
            /* 非 seek 窗口：真实缓冲时长测不出来（包队列暂时空）时不发布，维持原行为。 */
            return;
        } else {
            
            mBufferPosition = position;
        }

        if (mEof) {
            mBufferPosition = mDuration;
        }

        mPNotifier->NotifyBufferPosition((mBufferPosition <= mDuration ? mBufferPosition : mDuration) / 1000);
    }
}

int64_t SuperMediaPlayer::getPlayerBufferDuration(bool gotMax, bool internal)
{
    int64_t durations[3] = {-1, -1, -1};
    int i = 0;
    int64_t duration = -1;

    if (HAVE_AUDIO) {
        int64_t &duration_c = durations[i++];
        duration_c = mBufferController->GetPacketDuration(BUFFER_TYPE_AUDIO);
        //            AF_LOGD("audioDuration is %lld\n",audioDuration);
        if (!internal && mDemuxerService && mDemuxerService->getDemuxerHandle()) {
            duration_c += mDemuxerService->getDemuxerHandle()->getBufferDuration(mCurrentAudioIndex);
        }

        if (mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_AUDIO)) {
            int64_t audioPacketDuration = mBufferController->GetOnePacketDuration(BUFFER_TYPE_AUDIO);
            if (audioPacketDuration <= 0) {
                audioPacketDuration = 23 * 1000;
            }
            duration_c += mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_AUDIO)->getInputPaddingSize() * audioPacketDuration;
        }

        if (mSet->preferAudio) {
            return duration_c;
        }
    }

    if (HAVE_VIDEO) {
        int64_t &duration_c = durations[i++];
        duration_c = mBufferController->GetPacketDuration(BUFFER_TYPE_VIDEO);

        //            AF_LOGD("videoDuration is %lld\n",videoDuration);
        if (duration_c < 0 && !HAVE_AUDIO) {
            duration_c = mBufferController->GetPacketLastPTS(BUFFER_TYPE_VIDEO) - mBufferController->GetPacketPts(BUFFER_TYPE_VIDEO);

            if (duration_c <= 0) {
                duration_c = (int64_t) mBufferController->GetPacketSize(BUFFER_TYPE_VIDEO) * 40 * 1000;
            }
        }
        if (!internal && mDemuxerService && mDemuxerService->getDemuxerHandle()) {
            duration_c += mDemuxerService->getDemuxerHandle()->getBufferDuration(mCurrentVideoIndex);
        }

        if (mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)) {
            // FIXME: get the accurate duration
            duration_c += mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)->getInputPaddingSize() * 40 * 1000;
        }
    }

    /*
         *  Do not let player loading when switching subtitle, we'll read subtitle first
         *  in ReadPacket()
         */
    if (HAVE_SUBTITLE && !mSubtitleEOS && mSubtitleChangedFirstPts == INT64_MIN) {
        int64_t &duration_c = durations[i++];
        duration_c = mBufferController->GetPacketDuration(BUFFER_TYPE_SUBTITLE);
        if (!internal && mDemuxerService && mDemuxerService->getDemuxerHandle()) {
            duration_c += mDemuxerService->getDemuxerHandle()->getBufferDuration(mCurrentSubtitleIndex);
        }
    }

    int num = i;

    for (i = 0; i < num; i++) {
        if (durations[i] < 0) {
            continue;
        }
        if (duration < 0) {
            duration = durations[i];
        } else {
            duration = gotMax ? std::max(duration, durations[i]) : std::min(duration, durations[i]);
        }
    }

    return duration;
}

bool SuperMediaPlayer::SeekInCache(int64_t pos)
{
    int64_t audioLastPos = mBufferController->GetPacketLastTimePos(BUFFER_TYPE_AUDIO);
    int64_t videoLastPos = mBufferController->GetPacketLastTimePos(BUFFER_TYPE_VIDEO);
    int64_t minLastPos = -1;

    if (HAVE_VIDEO && HAVE_AUDIO) {
        minLastPos = audioLastPos < videoLastPos ? audioLastPos : videoLastPos;
    } else if (HAVE_VIDEO) {
        minLastPos = videoLastPos;
    } else if (HAVE_AUDIO) {
        minLastPos = audioLastPos;
    } else {
        // no video and audio ?
    }

    //seek bigger than last frame
    if (minLastPos > 0 && pos > minLastPos) {
        return false;
    }

    //seek back
    if (pos <= mCurrentPos) {
        mBufferController->Rewind(BUFFER_TYPE_ALL);
        int64_t audioFirstPos = mBufferController->GetPacketFirstTimePos(BUFFER_TYPE_AUDIO);
        int64_t videoFirstPos = mBufferController->GetPacketFirstTimePos(BUFFER_TYPE_VIDEO);
        int64_t maxFirstPos = -1;
        if (HAVE_VIDEO && HAVE_AUDIO) {
            maxFirstPos = std::max(audioFirstPos, videoFirstPos);
        } else if (HAVE_VIDEO) {
            maxFirstPos = videoFirstPos;
        } else if (HAVE_AUDIO) {
            maxFirstPos = audioFirstPos;
        } else {
            // no video and audio ?
        }

        if (maxFirstPos > 0 && pos < maxFirstPos) {
            return false;
        }
    }

    int64_t keyPosBefore = INT64_MIN;

    //can not find last key video pts,return
    if (HAVE_VIDEO) {
        keyPosBefore = mBufferController->GetKeyTimePositionBefore(BUFFER_TYPE_VIDEO, pos);
    } else if (HAVE_AUDIO) {
        keyPosBefore = mBufferController->GetKeyTimePositionBefore(BUFFER_TYPE_AUDIO, pos);
    }

    if (keyPosBefore == INT64_MIN) {
        return false;
    }

    mBufferController->ClearPacketBeforeTimePos(BUFFER_TYPE_ALL, keyPosBefore);
    mSoughtVideoPos = keyPosBefore;
    return true;
}


bool SuperMediaPlayer::isVideoDecoderMetaMatched(const Stream_meta *newMeta) const
{
    if (newMeta == nullptr) {
        return true;
    }

    /*
     * 用解码器自己的 meta（setUpDecoder 建解码器时存下来的那份）来比，
     * 不用 mCurrentVideoMeta：后者会被 ProcessUpdateView() 等路径按
     * mCurrentVideoIndex 刷新，切换途中有可能已经被刷成新码流的 meta，
     * 那样就会误判成“能解”。
     */
    const Stream_meta *cur = mAVDeviceManager->getDecoderMeta(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);

    if (cur == nullptr || cur->codec == 0) {
        return true;
    }

    if (cur->codec != newMeta->codec) {
        return false;
    }

    if (cur->width > 0 && cur->height > 0 && newMeta->width > 0 && newMeta->height > 0) {
        return cur->width == newMeta->width && cur->height == newMeta->height;
    }

    return true;
}

void SuperMediaPlayer::SwitchVideo(int64_t switchPos)
{
    
    const int targetStreamIndex = mVideoSwitchTargetIndex;

    if (targetStreamIndex < 0) {
        AF_LOGW("switch video: no target stream (the in-flight request is gone) — nothing to do\n");
        return;
    }

    AF_LOGD("video change find start time is %lld", switchPos);
    const int64_t switchStartMs = af_getsteady_ms();

    
    /* 上一次切档如果没走到"新档第一个包"就结束了，这里清掉残留的推迟标记（见成员说明）。 */
    mVideoDecoderRebuildPending = false;

    const int oldStreamIndex = mCurrentVideoIndex;

    if (oldStreamIndex >= 0 && oldStreamIndex != targetStreamIndex && mDemuxerService != nullptr) {
        mDemuxerService->CloseStream(oldStreamIndex);
        AF_LOGI("quality switch: closed the old video stream %d immediately (single-decoder switch)\n",
                oldStreamIndex);
    }

    /* ②b 打开新流 */
    int ret = mDemuxerService->OpenStream(targetStreamIndex);

    if (ret < 0) {
        AF_LOGW("switch video open stream failed, target stream index %d\n", targetStreamIndex);
        mPNotifier->NotifyVideoQualitySwitch(PLAYER_QUALITY_SWITCH_FAILED, targetStreamIndex,
                                             "target video stream open failed");
        mVideoSwitchInFlight = false;
        mVideoSwitchTargetIndex = -1;
        return;
    }

    AF_LOGI("quality switch: target stream %d opened in %lld ms (synchronous demuxer open)\n",
            targetStreamIndex, (long long) (af_getsteady_ms() - switchStartMs));

    /* ②c 把新流按流定位到切换点（只这一路；音频不被重定位） */
    const int64_t seekStartMs = af_getsteady_ms();
    ret = mDemuxerService->Seek(switchPos / 1000 * 1000, 0, targetStreamIndex);

    if (ret < 0) {
        AF_LOGW("switch video seek failed, target stream index %d ret=%d\n", targetStreamIndex, ret);
        mPNotifier->NotifyVideoQualitySwitch(PLAYER_QUALITY_SWITCH_FAILED, targetStreamIndex,
                                             "target video stream seek failed");
        mVideoSwitchInFlight = false;
        mVideoSwitchTargetIndex = -1;
        return;
    }

    AF_LOGI("quality switch: target stream %d seeked to %lld us in %lld ms (synchronous demuxer seek)\n",
            targetStreamIndex, (long long) switchPos, (long long) (af_getsteady_ms() - seekStartMs));

    /*
     * 提交"当前档"必须早于 ③/④：FlushVideoPath() 与 rebuildVideoDecoder() 都按
     * mCurrentVideoIndex 取 meta / 打日志 / 归零偏移，所以先把目标档坐实，后面的
     * flush 与重建就是"对新档做一次单解码器复位"。
     */
    mCurrentVideoIndex = targetStreamIndex;

    /* ③ 只 flush 视频：解码器 + 解码帧队列（音频一个字节都不动） */
    FlushVideoPath(false, false, "quality switch immediate");

    /*
     * ③b CloseStream 只停止投递，**不清**已经进公共队列的旧流包。单解码器下这些包
     * 必须当场丢掉，否则它们会被解出来、可能被落点过滤当成"包含 switchPos 的帧"而
     * 误报 READY（画面还是旧档）。按流丢弃是既有能力（DropPacketsByStream）。
     */
    if (oldStreamIndex >= 0 && oldStreamIndex != targetStreamIndex) {
        const int droppedOldPackets = mBufferController->DropPacketsByStream(BUFFER_TYPE_VIDEO, oldStreamIndex);

        if (droppedOldPackets > 0) {
            AF_LOGI("quality switch: dropped %d stale video packet(s) of the old stream %d at switch time\n",
                    droppedOldPackets, oldStreamIndex);
        }
    }

    /* ④ meta 不匹配才重建**同一块**解码器（无第二实例、无占位面） */
    Stream_meta newMeta{};

    if (mDemuxerService->GetStreamMeta(&newMeta, targetStreamIndex, false) < 0) {
        AF_LOGW("switch video: cannot read the target stream meta (stream=%d) — reporting FAILED\n",
                targetStreamIndex);
        mPNotifier->NotifyVideoQualitySwitch(PLAYER_QUALITY_SWITCH_FAILED, targetStreamIndex,
                                             "target video stream meta unavailable");
        mVideoSwitchInFlight = false;
        mVideoSwitchTargetIndex = -1;
        return;
    }

    if (!isVideoDecoderMetaMatched(&newMeta)) {
        AF_LOGI("quality switch: the target stream needs an in-place decoder rebuild (codec=%d %dx%d) — the "
                "SINGLE decoder is rebuilt, never a second one\n",
                (int) newMeta.codec, newMeta.width, newMeta.height);
        if (newMeta.extradata == nullptr || newMeta.extradata_size == 0) {
            AF_LOGI("quality switch: the target stream meta carries no extradata yet (its init segment is "
                    "parsed by the read thread) — deferring the in-place decoder rebuild to the first packet "
                    "of the new stream\n");
            mVideoDecoderRebuildPending = true;
        } else if (rebuildVideoDecoder(false) < 0) {
            AF_LOGW("switch video: in-place decoder rebuild failed for stream %d\n", targetStreamIndex);
            mPNotifier->NotifyVideoQualitySwitch(PLAYER_QUALITY_SWITCH_FAILED, targetStreamIndex,
                                                 "in-place decoder rebuild failed");
            mVideoSwitchInFlight = false;
            mVideoSwitchTargetIndex = -1;
            return;
        }
    } else {
        AF_LOGI("quality switch: the target stream matches the current decoder meta — only the flush is "
                "needed (no second decoder, no rebuild)\n");
    }

    /*
     * ⑤ 脏帧保护重新武装 + 单一落点过滤接管。
     * 顺序不能反：必须等 ③（清帧/清包）与 ④（重建）都完成之后再武装过滤，否则窗口里
     * 旧流的帧会被当成落点帧。武装之后，新流首个上屏帧 = 包含 switchPos 的那一帧。
     */
    mSeekDecodeStartIsKey = false;
    beginDiscontinuity(switchPos);

    mWillSwitchVideo = false;
    mVideoChangedFirstPts = INT64_MAX;
    mEof = false;

    AF_LOGI("quality switch: single-decoder switch applied in %lld ms (stream=%d switchPos=%lld); the landing "
            "filter now waits for the frame that contains switchPos\n",
            (long long) (af_getsteady_ms() - switchStartMs), targetStreamIndex, (long long) switchPos);
}

void SuperMediaPlayer::SwitchVideoAligned(int64_t switchPos)
{
    const int targetStreamIndex = mVideoSwitchTargetIndex;

    if (targetStreamIndex < 0) {
        AF_LOGW("switch video (aligned): no target stream (the in-flight request is gone) — nothing to do\n");
        return;
    }

    /*
     * 起播就切（没有旧档可交接）或者目标就是当前档：没有"分片边界"可等，退回立即路径。
     * 这是**状态判据**，不是开关：mCurrentVideoIndex < 0 只在起播那一刻成立。
     */
    if (mCurrentVideoIndex < 0 || mCurrentVideoIndex == targetStreamIndex) {
        SwitchVideo(switchPos);
        return;
    }

    if (mDemuxerService == nullptr) {
        AF_LOGW("switch video (aligned): no demuxer service\n");
        mPNotifier->NotifyVideoQualitySwitch(PLAYER_QUALITY_SWITCH_FAILED, targetStreamIndex,
                                             "no demuxer service");
        finishQualitySwitch(false, "no demuxer service");
        return;
    }

    
    const int ret = mDemuxerService->SwitchStreamAligned(mCurrentVideoIndex, targetStreamIndex);

    if (ret < 0) {
        AF_LOGW("switch video (aligned): the demuxer refused the aligned switch %d -> %d (ret=%d)\n",
                mCurrentVideoIndex, targetStreamIndex, ret);
        mPNotifier->NotifyVideoQualitySwitch(PLAYER_QUALITY_SWITCH_FAILED, targetStreamIndex,
                                             "aligned switch not armed");
        finishQualitySwitch(false, "aligned switch not armed");
        return;
    }

    AF_LOGI("quality switch: armed an ALIGNED switch %d -> %d at the next segment boundary (the old stream "
            "keeps being read and rendered until then; no seek, no flush, no decoder touched yet)\n",
            mCurrentVideoIndex, targetStreamIndex);

    
}

void SuperMediaPlayer::commitAlignedVideoSwitch(int64_t firstPacketPosUs, bool firstPacketIsKey)
{
    const int targetStreamIndex = mVideoSwitchTargetIndex;
    const int oldStreamIndex = mCurrentVideoIndex;

    if (targetStreamIndex < 0 || targetStreamIndex == oldStreamIndex) {
        return;
    }

    

    
    AF_LOGI("quality switch (aligned): the boundary switch has completed and the target stream delivered its "
            "first packet (on its own axis=%lld us, key=%d) — switching %d -> %d now (no waiting for the old "
            "buffer to drain)\n",
            (long long) firstPacketPosUs, firstPacketIsKey ? 1 : 0, oldStreamIndex, targetStreamIndex);

   

    mCurrentVideoIndex = targetStreamIndex;

    

    Stream_meta newMeta{};
    const bool metaRead = (mDemuxerService != nullptr) &&
                          (mDemuxerService->GetStreamMeta(&newMeta, targetStreamIndex, false) >= 0);
    const bool metaMatched = metaRead && isVideoDecoderMetaMatched(&newMeta);

    if (!metaMatched) {
        if (oldStreamIndex >= 0) {
            const int droppedOldPackets = mBufferController->DropPacketsByStream(BUFFER_TYPE_VIDEO, oldStreamIndex);

            if (droppedOldPackets > 0) {
                AF_LOGI("quality switch (aligned): dropped %d stale video packet(s) of the old stream %d "
                        "at the boundary commit (the decoder is being replaced)\n",
                        droppedOldPackets, oldStreamIndex);
            }
        }
        FlushVideoPath(false, false, "quality switch aligned commit (decoder changed)");

        if (metaRead && (newMeta.extradata == nullptr || newMeta.extradata_size == 0)) {
            mVideoDecoderRebuildPending = true;
            AF_LOGI("quality switch (aligned): target meta carries no extradata yet — deferring the in-place "
                    "decoder rebuild to the first packet of the new stream\n");
        } else if (metaRead && rebuildVideoDecoder(false) < 0) {
            AF_LOGW("switch video (aligned): in-place decoder rebuild failed for stream %d\n", targetStreamIndex);
            mPNotifier->NotifyVideoQualitySwitch(PLAYER_QUALITY_SWITCH_FAILED, targetStreamIndex,
                                                 "in-place decoder rebuild failed");
            finishQualitySwitch(false, "in-place decoder rebuild failed");
            return;
        }
    } else if (!firstPacketIsKey) {
   
        FlushVideoPath(false, false, "quality switch aligned commit (non-key first packet)");
        AF_LOGI("quality switch (aligned): the new stream does not start at a keyframe — flushed the decoder "
                "only (no rebuild) so it re-syncs on this packet\n");
    } else {

        AF_LOGI("quality switch (aligned): decoder meta matches and the new stream starts at a keyframe — "
                "committing with NO flush and NO packet drop (seamless)\n");
    }
    mVideoSwitchOldStreamIndex = oldStreamIndex;

    mWillSwitchVideo = false;
    mEof = false;
}

int64_t SuperMediaPlayer::getAudioPlayTimeStampCB(void *arg)
{
    auto *pHandle = static_cast<SuperMediaPlayer *>(arg);
    return pHandle->getAudioPlayTimeStamp();
}

int64_t SuperMediaPlayer::getAudioPlayTimeStamp()
{
    if (!mAVDeviceManager->isAudioRenderValid()) {
        return INT64_MIN;
    }

    
    if (mSeekFlag) {
        return INT64_MIN;
    }

    
    const int64_t baseUs = mDiscontinuity.audioBaseUs.load();

    if (baseUs == INT64_MIN) {
        return INT64_MIN;
    }

    const int64_t consumedUs = mAVDeviceManager->getAudioRenderPosition();
    if (consumedUs < 0 || af_clock_value_is_unset(consumedUs)) {
        return INT64_MIN;
    }

    int64_t baseConsumedUs = mDiscontinuity.audioBaseConsumedUs.load();

    if (baseConsumedUs == INT64_MIN) {
        mDiscontinuity.audioBaseConsumedUs = consumedUs;
        return INT64_MIN;
    }

    const int64_t delta = consumedUs - baseConsumedUs;

    if (delta < 0) {
       
        mDiscontinuity.audioBaseConsumedUs = consumedUs;
        return INT64_MIN;
    }

    if (delta == 0) {
        /* 刚锚定 / 设备还没消费：位置与基准相同，交出去只会让参考时钟原地不动。 */
        return INT64_MIN;
    }

    return baseUs + delta;
}

void SuperMediaPlayer::GetVideoResolution(int &width, int &height)
{
    width = mVideoWidth;
    height = mVideoHeight;
}

void SuperMediaPlayer::GetVideoRotation(int &rotation)
{
    rotation = mVideoRotation;
}

int SuperMediaPlayer::setUpAudioDecoder(const Stream_meta *meta)
{
    int ret = 0;

    if (meta->samplerate <= 0) {// meta.frame_size maybe 0 when playing artp
        ret = 0;
        return 0;
    }

    if (meta->duration > mDuration) {
        mDuration = meta->duration;
    }

    //setVolume to current setting after create new.
    SetVolume(mSet->mVolume);

    if (mSet->bMute) {
        mMsgCtrlListener->ProcessMuteMsg();
    }

    uint64_t flags = DECFLAG_SW;

#ifdef ANDROID
    bool isDrmContent = isDrmVideo(meta);
    if (isDrmContent) {
        flags |= DECFLAG_HW;
    }
    // 音频硬解（AAC→MediaCodec）在这台机器上实测是净亏损：MediaCodec 以
    // 进程内 codec2 模式运行，每多一个实例就多一个厂商 looper 线程
    // （MediaCodec_loop + CodecLooper）加一个 10ms 轮询的输入循环
    // （AFActiveDecoder），合计 ≈9% CPU；而 AAC 软解只需 ~1-2%。
    // ExoPlayer 的 MediaCodecAudioRenderer 靠异步回调（setCallback）省掉
    // 轮询，但异步模式在本机回调不送达会起播失败（ASYNC_ENABLED 默认关）。
    // 结论：默认走 FFmpeg 软解，硬件音频解码等异步回调在目标机验证后再开。
    bool tryHwAudio = false && (meta->codec == AF_CODEC_ID_AAC)
            && (meta->extradata != nullptr && meta->extradata_size >= 2)
            && mSet->bEnableHwVideoDecode;
    if (tryHwAudio) {
        flags |= DECFLAG_HW;
    }
#endif

    ret = mAVDeviceManager->setUpDecoder(flags, meta, nullptr, SMPAVDeviceManager::DEVICE_TYPE_AUDIO, 0);

#ifdef ANDROID
    if (ret < 0 && tryHwAudio) {
        // MediaCodec AAC 创建失败 → 回退纯软解，保证可播
        AF_LOGW("audio hw decode failed, fallback to software decoder");
        ret = mAVDeviceManager->setUpDecoder(DECFLAG_SW, meta, nullptr, SMPAVDeviceManager::DEVICE_TYPE_AUDIO, 0);
    }
#endif

    if (ret < 0) {
        MediaPlayerEventType type = MEDIA_PLAYER_EVENT_AUDIO_DECODER_DEVICE_ERROR;
        if (ret == gen_framework_errno(error_class_codec, codec_error_audio_not_support)) {
            type = MEDIA_PLAYER_EVENT_AUDIO_CODEC_NOT_SUPPORT;
        }
        AF_LOGE("setUpAudioDecoder error %d\n", ret);
        mPNotifier->NotifyEvent(type, framework_err2_string(ret));
        return ret;
    }

    return ret;
}

int SuperMediaPlayer::SetUpAudioPath()
{
    int ret = 0;
    if (!mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_AUDIO)) {

        /*
         * make sure the audio stream is opened before get stream meta,
         * otherwise, will crash in hls stream
         */
        if (mBufferController->IsPacketEmtpy(BUFFER_TYPE_AUDIO)) {
            return 0;
        }

        unique_ptr<streamMeta> pMeta{};
        mDemuxerService->GetStreamMeta(pMeta, mCurrentAudioIndex, false);
        auto *meta = (Stream_meta *) (pMeta.get());

        int64_t startTimeMs = af_getsteady_ms();

        ret = setUpAudioDecoder(meta);

        int64_t costTimeMs = af_getsteady_ms() - startTimeMs;
        mRecorderSet->createAudioDecoderCostMs = costTimeMs;

        if (ret < 0) {
            return ret;
        }
    }

    if (mAudioFrameQue.empty() || mAVDeviceManager->isAudioRenderValid()) {
        return 0;
    }

    //        IAFFrame::audioInfo info = ;
    //        info.channels = meta->channels;
    //        info.sample_rate = meta->samplerate;
    //        info.format = meta->sample_fmt;
    //        info.nb_samples = meta->frame_size;
    //        info.channel_layout = meta->channel_layout;
    setUpAudioRender(mAudioFrameQue.front()->getInfo().audio);
    return ret;
}

int SuperMediaPlayer::setUpAudioRender(const IAFFrame::audioInfo &info)
{
    int ret = mAVDeviceManager->setUpAudioRender(info);

    if (ret < 0) {
        AF_LOGE("AudioOutHandle Init Error is %d", ret);
        // don't release audio handle because we only new it in constructor
        // PS: we should try to recover it later, or notify error
        //                    mAudioOutHandle = 0;
        mCurrentAudioIndex = -1;
        mCATimeBase = 0;
        return -1;
    }
    mAVDeviceManager->setAudioRenderListener(mAudioRenderCB.get());
    mAVDeviceManager->setSpeed(mSet->rate);
    mAVDeviceManager->setMute(mSet->bMute);
    mAVDeviceManager->setVolume(mSet->mVolume);

    if (!mSecretPlayBack) {
        mAVDeviceManager->setAudioRenderingCb(mAudioRenderingCb, mAudioRenderingCbUserData);
    }
    return 0;
}

int SuperMediaPlayer::setUpVideoRender(uint64_t flags)
{
    if (mAppStatus == APP_BACKGROUND) {
        AF_LOGW("create video render in background");
    }

    AF_LOGD("SetUpVideoRender start");
    CreateVideoRender(flags);
    if (!mAVDeviceManager->isVideoRenderValid()) {
        AF_LOGE("can't create video render\n");
        mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_VIDEO_RENDER_INIT_ERROR, "init video render failed");
        return -EINVAL;
    }

    if (!mSecretPlayBack) {
        mAVDeviceManager->setVideoRenderingCb(mVideoRenderingCb, mVideoRenderingCbUserData);
    }

    //re set view in case for not set view before
    if (mSet->mView) {
        if (mAVDeviceManager->isVideoRenderValid()) {
            mAVDeviceManager->getVideoRender()->setDisPlay(mSet->mView);
        }
    }

    return 0;
}


int SuperMediaPlayer::SetUpVideoPath()
{
    if (mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO) && (mAVDeviceManager->isVideoRenderValid())) {
        return 0;
    }

    if (mBufferController->IsPacketEmtpy(BUFFER_TYPE_VIDEO)) {
        return 0;
    }

    if (mVideoInterlaced == InterlacedType_UNKNOWN) {
        AF_LOGW("Wait for parser video interlaced Type");
        return 0;
    }
    /*
     * update the video meta after the first video packet was reached,
     * otherwise the video meta is incomplete when playing a master hls playList.
     */
    updateVideoMeta();
    auto *meta = (Stream_meta *) (mCurrentVideoMeta.get());

    bool bHW = false;
    if (mSet->bEnableHwVideoDecode) {
        switch (meta->codec) {
            case AF_CODEC_ID_H264: {
                string value = getProperty("ro.video.dec.h264");
                bHW = !(value == "OFF");
                break;
            }
            case AF_CODEC_ID_HEVC: {
                string value = getProperty("ro.video.dec.hevc");
                bHW = !(value == "OFF");
                break;
            }

            default:
                bHW = true;
                break;
        }
    }

    bool tunnelRender = mSet->bEnableTunnelRender;
    if (!mSet->bEnableHwVideoDecode || !bHW) {
        //soft decoder not support tunnel Render
        tunnelRender = false;
    }

    uint64_t flags = 0;


    if (isHDRVideo(meta)) {
        /*
         * HDR video must use mediaCodec to render direct on Android,
         * we use a dummy render to release the frame simply
         */
#ifdef ANDROID
        flags |= IVideoRender::FLAG_DUMMY;
#else
        flags |= IVideoRender::FLAG_HDR;
#endif
    }
    /*
     * 平台 CDM 解的那一路 DRM 必须用 dummy（安全）渲染面。
     *
     * 这里刻意**不加平台宏**：isDrmVideo 问的是"本平台注册的 DRM handler 认不认"，
     * 没有注册 handler 的平台（Windows/Linux/macOS）恒为 false —— 而那些平台上
     * DASH 的 CENC 由内核软解，keyFormat 本来就不会被填（见 DashStream 的说明），
     * 所以这条判据在所有平台上都恰好是它该有的取值。
     */
    bool isDrmContent = isDrmVideo(meta);

    if (tunnelRender || isDrmContent) {
        flags |= IVideoRender::FLAG_DUMMY;
    }

    /*
     * 【硬解直出 / 软解上屏的分流】平台要求"硬解直出"时补上 FLAG_DUMMY：
     * 鸿蒙的硬解是 OH_AVCodec 把画面直接写进 XComponent 窗口（零拷贝、帧不进框架），
     * 带上这个标志才会走那条路，行为与开 GLRender 之前一字不差；软解（bHW == false）
     * 不加，渲染器就是 GLRender，CPU 帧能上屏。其它平台这个函数返回 false，
     * flags 与今天完全一致。硬解创建失败时下面会去掉 FLAG_DUMMY 重建渲染器，
     * 于是"硬解失败退软解"这条既有路径会自动切到 GLRender。
     */
    if (bHW && videoRenderFactory::preferDirectSurfaceForHardwareDecode()) {
        flags |= IVideoRender::FLAG_DUMMY;
    }

    AF_LOGI("SetUpVideoPath tunnelRender=%d hw=%d renderFlags=%" PRIx64 " view=%p\n",
            (int) tunnelRender, (int) bHW, flags, mSet->mView.load());

    int ret = setUpVideoRender(flags);
    if (ret < 0) {
        return ret;
    }

    if (mAVDeviceManager->isDecoderValid(SMPAVDeviceManager::DEVICE_TYPE_VIDEO)) {
        return 0;
    }

    AF_LOGD("SetUpVideoDecoder start");

    if (meta->interlaced == InterlacedType_UNKNOWN) {
        meta->interlaced = mVideoInterlaced;
    }

#ifdef ANDROID
    if (bHW && mAVDeviceManager->isVideoRenderValid() && mAVDeviceManager->getVideoRender()->getFlags() & IVideoRender::FLAG_DUMMY &&
        mSet->mView == nullptr) {
        return 0;
    }
#endif

    int64_t startTimeMs = af_getsteady_ms();
    ret = CreateVideoDecoder(bHW, *meta);

    if (ret < 0) {
        if (bHW) {

            if (flags & IVideoRender::FLAG_DUMMY) {
                flags &= ~IVideoRender::FLAG_DUMMY;
                ret = setUpVideoRender(flags);
                if (ret < 0) {
                    return ret;
                }
            }

            // Hardware decoding is the default, but a failure here must not cost
            // the user the video: drop to the software decoder and keep playing.
            // On Windows that is the D3D11VA path in avcodecDecoder failing
            // (no hwaccel in the linked FFmpeg, or the D3D11 device could not be
            // created); avcodec_get_hw_config() decides, so an unavailable
            // hwaccel is detected before any frame is decoded.
            ret = CreateVideoDecoder(false, *meta);
        }
    }

    int64_t costTimeMs = af_getsteady_ms() - startTimeMs;
    mRecorderSet->createVideoDecoderCostMs = costTimeMs;

    if (ret < 0) {
        AF_LOGE("%s CreateVideoDecoder failed, error msg is  %s", __FUNCTION__, framework_err2_string(ret));

        if (ret == gen_framework_errno(error_class_codec, codec_error_video_not_support)) {
            mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_VIDEO_CODEC_NOT_SUPPORT, framework_err2_string(ret));
        } else if (ret == gen_framework_errno(error_class_codec, codec_error_video_device_error)) {
            mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_VIDEO_DECODER_DEVICE_ERROR, framework_err2_string(ret));
        }

        return ret;
    }

    if (mAVDeviceManager->getVideoDecoderFlags() & DECFLAG_HW) {
    } else {
        if (mSet->bEnableHwVideoDecode) {
            mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_SW_VIDEO_DECODER, "Switch to software video decoder");
        }
    }

    if (meta->duration > mDuration) {
        mDuration = meta->duration;
    }

    return ret;
}

void SuperMediaPlayer::updateVideoMeta()
{
    mDemuxerService->GetStreamMeta(mCurrentVideoMeta, mCurrentVideoIndex, false);
    auto *meta = (Stream_meta *) (mCurrentVideoMeta.get());
    {
#ifdef ENABLE_VIDEO_FILTER
        std::lock_guard<std::mutex> filterLock(mFilterManagerMutex);
        if (mFilterManager != nullptr) {
            mFilterManager->setStreamMeta(meta);
        }
#endif
    }

    int with = meta->displayWidth == 0 ? meta->width : meta->displayWidth;
    int height = meta->displayHeight == 0 ? meta->height : meta->displayHeight;
    // TunnelRender（dummy render）直通时旋转由 MediaCodec 完成，
    // 对外上报的尺寸需与旋转后的显示方向一致（90/270 交换宽高）
    if (mAVDeviceManager->isVideoRenderValid() &&
        (mAVDeviceManager->getVideoRender()->getFlags() & IVideoRender::FLAG_DUMMY) &&
        (meta->rotate == 90 || meta->rotate == 270)) {
        int tmp = with;
        with = height;
        height = tmp;
    }
    if (mVideoWidth != with || mVideoHeight != height || mVideoRotation != meta->rotate) {
        mVideoWidth = with;
        mVideoHeight = height;
        mVideoRotation = meta->rotate;
        mPNotifier->NotifyVideoSizeChanged(mVideoWidth, mVideoHeight);
    }
}

bool SuperMediaPlayer::CreateVideoRender(uint64_t flags)
{
    if (mAVDeviceManager->isVideoRenderValid() && mAVDeviceManager->getVideoRender()->getFlags() == flags) {
        return true;
    }
    mAVDeviceManager->createVideoRender(flags);
    if (!mAVDeviceManager->getVideoRender()) {
        return false;
    }
    mAVDeviceManager->getVideoRender()->setScale(convertScaleMode(mSet->scaleMode));
    mAVDeviceManager->getVideoRender()->setRotate(convertRotateMode(mSet->rotateMode));
    mAVDeviceManager->getVideoRender()->setBackgroundColor(mSet->mVideoBackgroundColor);
    mAVDeviceManager->getVideoRender()->setFlip(convertMirrorMode(mSet->mirrorMode));
    /* 【色觉辅助滤镜 / 回退点 C8】照上一行 setFlip 的写法：建渲染器时把**启动配置**里的矩阵
     * 也下发一次。否则"先 setColorMatrix 再 prepare/播放"时首帧不带滤镜
     * （渲染器是后建的，错过那条消息）。空默认实现下非 GL 渲染器无副作用。 */
    mAVDeviceManager->getVideoRender()->setColorMatrix(mSet->colorMatrix);
    mAVDeviceManager->getVideoRender()->setDisPlay(mSet->mView);
    mAVDeviceManager->setVideoRenderListener(mVideoRenderListener.get());
    mAVDeviceManager->getVideoRender()->setVideoProcessTextureCb(mVideoProcessCb.get());

    int renderRet = mAVDeviceManager->getVideoRender()->init();

    if (renderRet != 0) {
        // for windows init failed, which may need change render type in future.
        mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_VIDEO_RENDER_INIT_ERROR, "init video render failed");
    }
    mAVDeviceManager->setSpeed(mSet->rate);
    mVideoRenderInited = true;
    return true;
}

int SuperMediaPlayer::CreateVideoDecoder(bool bHW, Stream_meta &meta)
{
    int ret;
    uint64_t decFlag = 0;

    if (bHW) {
        decFlag |= DECFLAG_HW;
    } else {
        decFlag |= DECFLAG_SW;
    }

    if (mAdaptiveVideo) {
        decFlag |= DECFLAG_ADAPTIVE;
    }
    if (!mSet->bLowLatency) {
        mSet->bLowLatency = mDemuxerService->getDemuxerHandle()->isLowLatency();
    }

    if (mSet->bLowLatency) {
        decFlag |= DECFLAG_OUTPUT_FRAME_ASAP;
    }

    void *view = nullptr;
    
    mActiveVideoSurface = nullptr;
    
    mAVDeviceManager->releaseVideoRenderFrames();
    mAVDeviceManager->flushVideoRender();

    if (bHW) {
        if (mAVDeviceManager->isVideoRenderValid() && mAVDeviceManager->getVideoRender()->getFlags() & IVideoRender::FLAG_DUMMY) {
            view = mSet->mView;
            decFlag |= DECFLAG_DIRECT;
        } else {
            if (mAVDeviceManager->isVideoRenderValid()) {
                /*
                 * Get a cached surface, then the mediaCodec video decoder can be reuse,
                 * otherwise the mediaCodec video decoder will be recreate on setUpDecoder.
                 */
                view = mAVDeviceManager->getVideoRender()->getSurface(false);
            }
        }
    }

    if (!mSet->bLowLatency) {
        mSet->bLowLatency = mDemuxerService->getDemuxerHandle()->isLowLatency();
    }

    if (mSet->bLowLatency) {
        decFlag |= DECFLAG_OUTPUT_FRAME_ASAP;
    }
    uint32_t dstFormat = 0;
#ifdef __APPLE__
    dstFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    if ((mFrameCb || mVideoRenderingCb) && mSet->pixelBufferOutputFormat) {
        dstFormat = mSet->pixelBufferOutputFormat;
    } else {
        // TODO: move to VTB decoder
        /*
         *  must set hdr video to output x420, otherwise vtb will output a p420 format
         *  if iOS version little than 14 that can't be displayed by AVSampleBufferDisplayLayer
         */
        if (meta.pixel_fmt == AF_PIX_FMT_YUV420P10BE || meta.pixel_fmt == AF_PIX_FMT_YUV420P10LE) {
            dstFormat = kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
        }
    }
#endif
    ret = mAVDeviceManager->setUpDecoder(decFlag, (const Stream_meta *) (&meta), view, SMPAVDeviceManager::DEVICE_TYPE_VIDEO, dstFormat);
    AF_LOGI("CreateVideoDecoder bHW=%d decFlag=%" PRIx64 " view=%p ret=%d\n",
            (int) bHW, decFlag, view, ret);
    if (ret < 0) {
        return ret;
    }

    /* B4：记住这块解码器真正绑定的输出面（隧道 = App 的 view；GL = 渲染器 SurfaceTexture 的面）。 */
    mActiveVideoSurface = view;

    
    mPictureCacheType = IsVideoDecoderHardware() ? picture_cache_type_cannot : picture_cache_type_soft;
    AF_LOGI("video frame queue cap: pictureCacheType=%s maxCacheSize=%d (hardware=%d)\n",
            (mPictureCacheType == picture_cache_type_cannot) ? "cannot" : "soft",
            (mPictureCacheType == picture_cache_type_cannot) ? 1 : VIDEO_PICTURE_MAX_CACHE_SIZE,
            (int) IsVideoDecoderHardware());

   
    videoDecoderEOS = false;

    {
        std::lock_guard<std::mutex> lock(mAppStatusMutex);
        mMsgCtrlListener->ProcessVideoHoldMsg(mAppStatus == APP_BACKGROUND);
    }
    return ret;
}


int SuperMediaPlayer::rebuildVideoDecoder(bool requireDummyRender)
{
    /*
     * 解码器槽位可以是 valid=false（“僵尸”）状态 —— 那正是需要重建的时候，
     * 所以这里**不**要求解码器当前有效，只要求视频路存在且渲染器可用。
     */
    if (mAVDeviceManager == nullptr || !HAVE_VIDEO ||
        !mAVDeviceManager->isVideoRenderValid()) {
        return -EINVAL;
    }

    if (requireDummyRender &&
        (mAVDeviceManager->getVideoRender()->getFlags() & IVideoRender::FLAG_DUMMY) == 0) {
        // RestartVideoDecoder() 的调用场景（surface 热切换）只对 dummy/tunnel 有意义。
        return -EINVAL;
    }

    // 用栈上副本，避免与主循环线程并发读写共享的 mCurrentVideoMeta
    Stream_meta meta{};
    if (mDemuxerService == nullptr ||
        mDemuxerService->GetStreamMeta(&meta, mCurrentVideoIndex, false) < 0) {
        return -EINVAL;
    }

    // 沿用上一次的解码方式（硬解/软解）
    bool bHW = (mAVDeviceManager->getVideoDecoderFlags() & DECFLAG_HW) != 0;

    /*
     * 重建同样是"关掉旧解码器"：走 invalidateDecoder + CreateVideoDecoder 这条路时，
     * 旧解码器的表面池若被渲染器手上的帧钉着，就会活到新解码器的第一帧为止
     * （错误恢复场景下那可能是几百毫秒）。所以和 FlushVideoPath() 一样，
     * 在动解码器之前先同步放掉渲染侧的帧（契约见 IVideoRender::releaseFrames()）。
     */
    mAVDeviceManager->releaseVideoRenderFrames();

    // 置无效后 setUpDecoder 才会关掉旧（僵尸）解码器并新建
    mAVDeviceManager->invalidateDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);
    int ret = CreateVideoDecoder(bHW, meta);
    if (ret < 0 && bHW) {
        /*
         * 【tunnel（dummy render）下不做软解兜底 —— 2026-09-24】
         *
         * DummyVideoRender 只负责把 MediaCodec 的 buffer 交给 Surface；软解帧是
         * AVAFFrame 的 YUV 数据，走不了这条路 —— 兜底"成功"也只会得到"有声音、没画面"，
         * 而且会把真正的失败原因从日志里抹掉（真机日志里就出现过
         * `CreateVideoDecoder bHW=0 … ret=0` + `rebuildVideoDecoder … ret=0` 这种假成功）。
         *
         * 判据是渲染器带 FLAG_DUMMY（既有状态，不新增成员）；Qt/GL 路恒不进入 ⇒
         * 那边的"硬解失败落软解"行为逐字不变。
         */
        const bool dummyRender =
            mAVDeviceManager->isVideoRenderValid() &&
            (mAVDeviceManager->getVideoRender()->getFlags() & IVideoRender::FLAG_DUMMY);
        if (dummyRender) {
            AF_LOGW("rebuildVideoDecoder: hardware rebuild failed (%d) and the tunnel render cannot "
                    "present software frames — skipping the software fallback\n", ret);
        } else {
            // 软解兜底（罕见路径：硬解重建失败时保音频不断播）
            ret = CreateVideoDecoder(false, meta);
        }
    }

    if (ret < 0) {
        AF_LOGW("%s rebuild video decoder failed %s\n", __FUNCTION__, framework_err2_string(ret));
    } else {
        AF_LOGI("rebuildVideoDecoder dummyOnly=%d bHW=%d ret=%d\n", (int) requireDummyRender, (int) bHW, ret);
    }
    return ret;
}

int SuperMediaPlayer::RestartVideoDecoder()
{
    // 兜底路径，由 ProcessSetViewMsg 在持有 mCreateMutex 时调用。
    // 语义与参数说明见 rebuildVideoDecoder()：这里只处理 dummy/tunnel 渲染器
    // （surface 热切换失败的场景），且不得改变既有行为。
    return rebuildVideoDecoder(true);
}


bool SuperMediaPlayer::qualitySwitchInFlight() const
{
    
    return mVideoSwitchInFlight;
}

int SuperMediaPlayer::RestorePausedVideoFrame()
{
    // 由 ProcessSetViewMsg 在持有 mCreateMutex 时调用（仅暂停状态）。
    // surface 重建后 ACodec 不会重绘最后一帧（ExoPlayer 2.9.6 的
    // setOutputSurface 热切换同样不重绘）。以解码器记录的"最后渲染帧
    // PTS"为渲染门（只放行这一帧，避免从关键帧开始闪帧），原地 seek
    // 解码，逐帧精确恢复暂停画面。
    if (mPlayStatus != PLAYER_PAUSED) {
        AF_LOGI("PFR: skip, status=%d\n", (int) mPlayStatus.load());
        return 0;
    }

    
    if (qualitySwitchInFlight()) {
        if (!mPauseFrameRestorePending) {
            AF_LOGI("PFR: deferred — a quality switch is still in flight (switchInFlight=%d target=%d), "
                    "so no seek is issued now; the restore will run ONCE at the switch's terminal "
                    "state (READY / FAILED)\n",
                    (int) mVideoSwitchInFlight, mVideoSwitchTargetIndex);
        }

        mPauseFrameRestorePending = true;
        return 0;
    }
    IDecoder *decoder = mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);
    if (decoder == nullptr) {
        AF_LOGI("PFR: skip, no video decoder\n");
        return 0;
    }
    int64_t lastPts = decoder->getLastRenderedVideoPts();
    if (lastPts == INT64_MIN) {
        AF_LOGI("PFR: skip, lastRenderedPts unknown\n");
        return 0;
    }
    int64_t frameDur = decoder->getLastRenderedVideoFrameDur();
    if (frameDur <= 0) {
        // 未知帧间隔时按 50ms 兜底：只影响 seek 目标（多读少量包），
        // 渲染门会丢弃目标帧之外的所有帧
        frameDur = 50000;
    }

    
    const int64_t lastPtsOnClockAxis =
            (mActiveVideoPtsOffset != INT64_MIN) ? lastPts + mActiveVideoPtsOffset : lastPts;

    decoder->setRenderGate(lastPts);
    mRestoringPausedFrame = true;
    // seek 到"暂停帧之后一帧以内"：保证暂停帧所在包被读到，
    // 且不会多解多少包（向上取整避免毫秒截断到暂停帧之前）
    SeekTo((lastPtsOnClockAxis + frameDur + 999) / 1000, false);
    AF_LOGI("PFR: restore start, lastPts=%" PRId64 " frameDur=%" PRId64
            " offset=%" PRId64 " seekToPos=%" PRId64 "\n",
            lastPts, frameDur,
            (int64_t) (mActiveVideoPtsOffset == INT64_MIN ? 0 : mActiveVideoPtsOffset),
            (int64_t) (lastPtsOnClockAxis / 1000));
    return 0;
}


void SuperMediaPlayer::replayDeferredUserSeek()
{
    if (!mDeferredUserSeekPending) {
        return;
    }

    const int64_t targetUs = mDeferredUserSeekUs;
    const bool accurate = mDeferredUserSeekAccurate;

    /* 先清闩：防止下面这次 SeekTo 又触发一次补做（不递归）。 */
    mDeferredUserSeekPending = false;
    mDeferredUserSeekUs = INT64_MIN;
    mDeferredUserSeekAccurate = false;

    if (targetUs == INT64_MIN) {
        return;
    }

    /*
     * 只在这个播放器"seek 还有意义"的状态下补做。本入口除了切档终态，也被
     * FlushVideoPath() 末尾那条"外力中止切档"的分支调用，而 stop / 换源 / 后台 flush
     * 走的正是同一个分支（它们的 cancelPendingSwitch 也是 1）。那时玩家已经在
     * PLAYER_STOPPED / PREPARING 上，补做一次 seek 只会把用户早已放弃的定位需求
     * 打进新片源的准备流程里。与 PFR 补做入口同一种写法：状态不符合就丢弃这一次。
     */
    const PlayerStatus statusNow = mPlayStatus.load();

    if (statusNow != PLAYER_PREPARED && statusNow != PLAYER_PLAYING && statusNow != PLAYER_PAUSED &&
        statusNow != PLAYER_COMPLETION) {
        AF_LOGI("user seek: deferred seek DROPPED — the player is no longer in a seekable state "
                "(status=%d), target=%lld us\n",
                (int) statusNow, (long long) targetUs);
        return;
    }

    if (qualitySwitchInFlight()) {
        /* 又有一次切档在途：把目标重新挂上，等那一次的终态再补（不立即重试）。 */
        mDeferredUserSeekUs = targetUs;
        mDeferredUserSeekAccurate = accurate;
        mDeferredUserSeekPending = true;
        return;
    }

    AF_LOGW("user seek: replaying the DEFERRED seek now that the quality switch reached its terminal "
            "state — target=%lld us accurate=%d (the landing window is built on the post-switch timeline, "
            "so the switch's demuxer-level seek can no longer overwrite it)\n",
            (long long) targetUs, (int) accurate);

    SeekTo(targetUs / 1000, accurate);
}
bool SuperMediaPlayer::IsStreamSwitchInFlight() const
{
    return qualitySwitchInFlight();
}

void SuperMediaPlayer::setVideoDecodeBoost(bool boost)
{
    if (mAVDeviceManager == nullptr) {
        return;
    }

    IDecoder *decoder = mAVDeviceManager->getDecoder(SMPAVDeviceManager::DEVICE_TYPE_VIDEO);

    if (decoder != nullptr) {
        decoder->setDecodeBoost(boost);
    }
}


void SuperMediaPlayer::runDeferredPauseFrameRestore()
{
    if (!mPauseFrameRestorePending) {
        return;
    }

    mPauseFrameRestorePending = false;

    if (mPlayStatus != PLAYER_PAUSED) {
        AF_LOGI("PFR: deferred restore dropped — the player is no longer paused (status=%d)\n",
                (int) mPlayStatus.load());
        return;
    }

    if (qualitySwitchInFlight()) {
        /* 终态出口处又有新的切档在途：推迟到它自己的终态，绝不在这里立即重试。 */
        mPauseFrameRestorePending = true;
        return;
    }

    AF_LOGI("PFR: running the deferred pause-frame restore now that the quality switch reached its "
            "terminal state (no seek was issued while the switch was in flight)\n");

    RestorePausedVideoFrame();
}


void SuperMediaPlayer::Reset()
{
    mCurrentVideoIndex = -1;
    mCurrentAudioIndex = -1;
    mCurrentSubtitleIndex = -1;
    mVideoWidth = 0;
    mVideoHeight = 0;
    mVideoRotation = 0;
    mDuration = INT64_MIN;
    mBufferPosition = 0;
    mSeekPos = INT64_MIN;
    mPlayedVideoPts = INT64_MIN;
    mPlayedAudioPts = INT64_MIN;
   
    mDiscontinuity.audioBaseUs = INT64_MIN;
    mDiscontinuity.audioBaseConsumedUs = INT64_MIN;
    mSeekFlag = false;
    /* 墙钟计时跟 seek 状态一起复位，否则上一次播放留下的时间戳会让新一次
     * seek 的"音频解锁"立刻命中（见 render() 里 SEEK_CATCH_AUDIO_UNBLOCK_MS）。 */
    mSeekCatchStartMs = 0;
    mSeekNoFrameSinceMs = 0;
    mFirstAudioPts = INT64_MIN;
    mFirstVideoPts = INT64_MIN;
    mMediaStartPts = INT64_MIN;
    mEof = false;
    mFirstBufferFlag = true;
    mBufferingFlag = false;
    mBufferingNotified = false;
    mCurVideoPts = INT64_MIN;
    mLastAudioFrameDuration = INT64_MIN;
    mTimeoutStartTime = INT64_MIN;
    mSubtitleShowIndex = 0;
    mActiveVideoPtsOffset = INT64_MIN;
    /* B10：时间轴重建（seek/stop/换源）时逐帧配对表必须一起丢掉，否则旧代的 pts 可能撞上新帧 */
    mVideoAxisPts.clear();
    mVideoAxisTimePos.clear();
    /* 切档的在途闩与目标档跨片源作废（旧模型这里是三个提交闩，等价于一处在途闩）。 */
    mVideoSwitchInFlight = false;
    mVideoSwitchTargetIndex = -1;
    mWillChangedAudioStreamIndex = -1;
    mWillChangedSubtitleStreamIndex = -1;
    mBufferIsFull = false;
    mWillSwitchVideo = false;
    mMixMode = false;
    mFirstRendered = false;
    mInited = false;
    mSeekNeedCatch = false;
    
    beginDiscontinuity(INT64_MIN);
    /* 【B19 硬要求】换片源/停止/Reset ⇒ 不跨片源补做 PFR：闩一起清掉。 */
    mPauseFrameRestorePending = false;
    /* 【B20 硬要求 B】换片源/停止/Reset ⇒ 在途被推迟的用户 seek 也必须作废（不跨片源重放）。 */
    mDeferredUserSeekPending = false;
    mDeferredUserSeekUs = INT64_MIN;
    mDeferredUserSeekAccurate = false;
    
    mSeekDecodeStartIsKey = false;
    /* B16：与 SeekTo 同一个复位口径。 */
    mVideoDiscardStreak = 0;
    mVideoDiscardGapAbsUs = INT64_MIN;
    mCatchUpDiscardStreak = 0;
    mSeekCatchStartMs = 0;
    mVideoStarveIters = 0;
    mSeekAudioAlignDone = false;
    /* P2：诊断限频闩也跨片源作废（下一次 seek 的第一条落点丢弃日志要能打出来）。 */
    mAudioLandingDropLoggedGen = -1;
    mVideoDecodeRetrySeen = false;
    mDecodeStallIters = 0;
    mDecodeStallRebuildDone = false;
    mMainStreamId = -1;
    mRemovedFirstAudioPts = INT64_MIN;
    mFirstSeekStartTime = 0;
    mAudioChangedFirstPts = INT64_MIN;
    mVideoChangedFirstPts = INT64_MIN;
    mSubtitleChangedFirstPts = INT64_MIN;
    mSoughtVideoPos = INT64_MIN;
    mFirstReadPacketSucMS = 0;
    // 管线恢复的状态跟着播放器一起复位，否则上一次播放留下的时间戳/冷却
    // 会影响下一次播放的判定。
    mLastVideoFrameRenderedMs = 0;
    mVideoRecoverCooldownMs = 0;
    mRecoverSampleMs = 0;
    mRecoverSamplePackets = -1;
    mVideoDecodeRebuildCount = 0;
    mCanceled = false;
    mPNotifier->Enable(true);
    FlushSubtitleInfo();
    mSubtitleShowedQueue.clear();
    mSubPlayer = nullptr;
    mBSReadCb = nullptr;
    mBSCbArg = nullptr;
    mBSSeekCb = nullptr;
    mBSCbArg = nullptr;
    mUtil->reset();
    mDcaManager->reset();
    mVideoInterlaced = InterlacedType_UNKNOWN;
    mVideoParserTimes = 0;
    mVideoPtsRevert = mAudioPtsRevert = false;
    mHaveVideoPkt = mHaveAudioPkt = false;
    mLowMem = false;
    mCurrentVideoMeta = nullptr;
    mAdaptiveVideo = false;
    dropLateVideoFrames = false;
    mVideoCatchingUp = false;
    mBRendingStart = false;
    mSubtitleEOS = false;
    mSecretPlayBack = false;
    mDrmKeyValid = false;
    mPtsDiscontinueDelta = INT64_MIN;
    mCurrentPos = 0;
    mCurrentFrameUtcTime = -1;
    mCATimeBase = 0;
    mWATimeBase = 0;
    mSuggestedPresentationDelay = 0;
    mLiveTimeSyncType = LiveTimeSyncType::LiveTimeSyncNormal;
    mCalculateSpeedUsePacket = true;
    mUtcTimer = nullptr;
    mContainerInfo = {};
    mOpenAudioDeviceFailed = false;
}

int SuperMediaPlayer::GetCurrentStreamIndex(StreamType type)
{
    int streamIndex = -1;

    if (mMixMode && type != ST_TYPE_SUB) {
        if (HAVE_VIDEO) {
            streamIndex = GEN_STREAM_INDEX(mCurrentVideoIndex);
        } else if (HAVE_AUDIO) {
            streamIndex = GEN_STREAM_INDEX(mCurrentAudioIndex);
        }
    } else {
        switch (type) {
            case ST_TYPE_AUDIO:
                streamIndex = mCurrentAudioIndex;
                break;

            case ST_TYPE_VIDEO:
                streamIndex = mCurrentVideoIndex;
                break;

            case ST_TYPE_SUB:
                streamIndex = mCurrentSubtitleIndex;
                break;

            default:
                break;
        }
    }

    return streamIndex;
}

StreamInfo *SuperMediaPlayer::GetCurrentStreamInfo(StreamType type)
{
    int streamIndex = GetCurrentStreamIndex(type);

    if (streamIndex != -1) {
        std::deque<StreamInfo *> &streamInfoQueue = mMediaInfo.mStreamInfoQueue;
        for (StreamInfo *info : streamInfoQueue) {
            if (info->streamIndex == streamIndex) {
                return info;
            }
        }
    }

    return nullptr;
}

void SuperMediaPlayer::RenderCallback(StreamType type, bool rendered, IAFFrame::AFFrameInfo &info)
{
#if 0
    static int64_t audioPts = INT64_MIN;
    if (type == ST_TYPE_AUDIO){
        audioPts = info.pts;
    } else if (type == ST_TYPE_VIDEO){
        if (audioPts != INT64_MIN){
            AF_LOGD("avsync video late %lld (%lld - %lld) clock is %lld\n",audioPts - info.pts,audioPts, info.pts,mMasterClock.GetTime());
        }
    }
#endif
    //   AF_LOGD("video stream render pts is %lld", pts);
    if (mCanceled) {
        return;
    }

    if ((PLAYER_PREPARED != mPlayStatus) && (PLAYER_PAUSED != mPlayStatus) && (PLAYER_PLAYING != mPlayStatus)) {
        return;
    }

    MsgParam param;
    param.renderedParam.info = info;
    param.renderedParam.rendered = rendered;
    param.renderedParam.type = type;
    param.renderedParam.timeMs = af_getsteady_ms();
    putMsg(MSG_INTERNAL_RENDERED, param, false);
}

void SuperMediaPlayer::checkFirstRender()
{
    if (!mFirstRendered) {
        mFirstRendered = true;
        AF_LOGI("Player NotifyFirstFrame");
        mPNotifier->NotifyFirstFrame();
    }
}

void SuperMediaPlayer::ChangePlayerStatus(PlayerStatus newStatus)
{
    mOldPlayStatus = mPlayStatus;

    if (mPlayStatus != newStatus) {
        if (newStatus == PLAYER_ERROR) {
            if (mDataSource) {
                mDataSource->clearCache();
            }
        }
        mPNotifier->NotifyPlayerStatusChanged(mPlayStatus, newStatus);
        mPlayStatus = newStatus;
    }
}

void SuperMediaPlayer::ResetSeekStatus()
{
    mSeekPos = INT64_MIN;
    mSeekNeedCatch = false;
    mVideoCatchingUp = false;
    mLastVideoFrameRenderedMs = af_getsteady_ms();
    mRecoverSampleMs = 0;
    mRecoverSamplePackets = -1;
    setVideoDecodeBoost(false);
}

void SuperMediaPlayer::notifySeekEndCallback()
{
    mPNotifier->NotifySeekEnd(mSeekInCache);
}

bool SuperMediaPlayer::IsMute() const
{
    return mSet->bMute;
}

int SuperMediaPlayer::SetListener(const playerListener &Listener)
{
    mSet->mPlayerListener = Listener;

    if (mPNotifier) {
        mPNotifier->setListener(Listener);
    }

    return 0;
}

void SuperMediaPlayer::SetAutoPlay(bool bAutoPlay)
{
    mAutoPlay = bAutoPlay;
}

bool SuperMediaPlayer::IsAutoPlay()
{
    return mAutoPlay;
}

void SuperMediaPlayer::SetFilterConfig(const std::string &filterConfig)
{
#ifdef ENABLE_VIDEO_FILTER
    std::lock_guard<std::mutex> filterLock(mFilterManagerMutex);
    if (mFilterManager != nullptr) {
        AF_LOGW("not support change filterConfig after be set");
        return;
    }

    mFilterConfig = std::unique_ptr<CicadaJSONArray>(new CicadaJSONArray(filterConfig));
    if (mFilterConfig == nullptr || !mFilterConfig->isValid()) {
        AF_LOGD("filterConfig not JSON Array");
        return;
    }

    //TODO videoInfo is useless now.
    mFilterManager = std::unique_ptr<FilterManager>(new FilterManager(IAFFrame::videoInfo(), *mFilterConfig.get()));
    mDcaManager->createObservers();
#endif
}

void SuperMediaPlayer::UpdateFilterConfig(const std::string &target, const std::string &options)
{
#ifdef ENABLE_VIDEO_FILTER
    std::lock_guard<std::mutex> filterLock(mFilterManagerMutex);
    if (mFilterManager != nullptr) {
        mFilterManager->updateFilter(target, options);
    }
#endif
}

void SuperMediaPlayer::SetFilterInvalid(const std::string &target, bool invalid)
{
#ifdef ENABLE_VIDEO_FILTER
    std::lock_guard<std::mutex> filterLock(mFilterManagerMutex);
    if (mFilterManager != nullptr) {
        mFilterManager->setInvalid(target, invalid);
    }
#endif
}

void SuperMediaPlayer::addExtSubtitle(const char *uri)
{
    MsgParam param;
    MsgDataSourceParam dataSourceParam = {nullptr};
    dataSourceParam.url = new string(uri ? uri : "");
    param.dataSourceParam = dataSourceParam;
    putMsg(MSG_ADD_EXT_SUBTITLE, param);
}

int SuperMediaPlayer::selectExtSubtitle(int index, bool bSelect)
{
    if (!(index & EXT_STREAM_BASE)) {
        AF_LOGE("select ext subtitle error\n");
        mPNotifier->NotifyEvent(MEDIA_PLAYER_EVENT_SUBTITLE_SELECT_ERROR, "Not a ext stream");
        return -1;
    }

    MsgParam param;
    MsgSelectExtSubtitleParam track = {index, bSelect};
    param.msgSelectExtSubtitleParam = track;
    putMsg(MSG_SELECT_EXT_SUBTITLE, param);
    return 0;
}

int SuperMediaPlayer::setStreamDelay(int index, int64_t time)
{
    // FIXME : delay all video here now
    if ((HAVE_VIDEO && (index == mCurrentVideoIndex)) || index == -1) {
        mVideoDelayTime = time * 1000;
        return 0;
    }
    if (!(index & EXT_STREAM_BASE) || !mSubPlayer) {
        AF_LOGE("setStreamDelay support ext subtitle only for now\n");
        return -ENOSYS;
    }
    if (mSubPlayer) {
        mSubPlayer->setDelayTime(index, time * 1000);
        mSubPlayer->seek(std::max(getCurrentPosition() + time * 1000, (int64_t) 0));
    }
    return 0;
}

void SuperMediaPlayer::startRendering(bool start)
{
    if (start == mBRendingStart) {
        return;
    }

    mBRendingStart = start;

    if (start) {
        mMasterClock.start();
    } else {
        mMasterClock.pause();
    }
    mAVDeviceManager->pauseAudioRender(!start);
}

void SuperMediaPlayer::SetOnRenderCallBack(onRenderFrame cb, void *userData)
{
    mFrameCb = cb;
    mFrameCbUserData = userData;
}

void SuperMediaPlayer::SetAudioRenderingCallBack(onRenderFrame cb, void *userData)
{
    mAudioRenderingCb = cb;
    mAudioRenderingCbUserData = userData;
}

void SuperMediaPlayer::SetVideoRenderingCallBack(videoRenderingFrameCB cb, void *userData)
{
    mVideoRenderingCb = cb;
    mVideoRenderingCbUserData = userData;
}

void SuperMediaPlayer::SetUpdateViewCB(UpdateViewCB cb, void *userData)
{
    mUpdateViewCB = cb;
    mUpdateViewCBUserData = userData;
}

void SuperMediaPlayer::SetUrlHashCB(UrlHashCB cb, void *userData)
{
    mUrlHashCb = cb;
    mUrlHashCbUserData = userData;
}

int SuperMediaPlayer::invokeComponent(std::string content)
{
    return mDcaManager->invoke(content);
}

void SuperMediaPlayer::setDrmRequestCallback(const std::function<DrmResponseData *(const DrmRequestParam &drmRequestParam)> &drmCallback)
{
    mAVDeviceManager->setDrmRequestCallback(drmCallback);
}

void SuperMediaPlayer::ProcessUpdateView()
{

    if (mCurrentVideoIndex < 0) {
        return;
    }

    int videoTag = VideoTag::VIDEO_TAG_NONE;

    updateVideoMeta();
    auto *meta = (Stream_meta *) (mCurrentVideoMeta.get());

    bool isHDR = isHDRVideo(meta);
    if (isHDR) {
        videoTag |= VideoTag::VIDEO_TAG_HDR10;
    }

#ifdef ANDROID
    bool isWideVine = isWideVineVideo(meta);
    if (isWideVine) {
        //TODO set widevine level  by user
        videoTag |= VideoTag::VIDEO_TAG_WIDEVINE_L1;
    }
    if (mUpdateViewCB != nullptr) {
        bool update = mUpdateViewCB(videoTag, mUpdateViewCBUserData);
        if (update) {
            //do not use old view to create decoder
            mSet->mView = nullptr;
        }
    }
#endif
}

bool SuperMediaPlayer::isWideVineVideo(const Stream_meta *meta)
{
    bool isWideVineVideo = (meta->keyFormat != nullptr && strcmp(meta->keyFormat, "urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed") == 0);
    return isWideVineVideo;
}

bool SuperMediaPlayer::isDrmVideo(const Stream_meta *meta)
{
    if (meta == nullptr || meta->keyFormat == nullptr) {
        return false;
    }
    DrmInfo probe{};
    probe.format = meta->keyFormat;
    probe.uri = meta->keyUrl == nullptr ? "" : meta->keyUrl;
    probe.pssh = meta->drmPssh == nullptr ? "" : meta->drmPssh;
    probe.keyId = meta->drmKeyId == nullptr ? "" : meta->drmKeyId;

    return DrmHandlerPrototype::isSupport(&probe);
}

bool SuperMediaPlayer::isHDRVideo(const Stream_meta *meta)
{
    bool isHDRVideo = false;

    if (meta->pixel_fmt == AF_PIX_FMT_YUV420P10BE || meta->pixel_fmt == AF_PIX_FMT_YUV420P10LE) {
        AF_LOGD("HDR video\n");
        isHDRVideo = true;
    }
    return isHDRVideo;
}

float SuperMediaPlayer::getCurrentDownloadSpeed()
{
    return mUtil->getCurrentDownloadSpeed();
}

void SuperMediaPlayer::ApsaraAudioRenderCallback::onFrameInfoUpdate(IAFFrame::AFFrameInfo &info, bool rendered)
{
    mPlayer.RenderCallback(ST_TYPE_AUDIO, rendered, info);
}

void SuperMediaPlayer::ApsaraVideoRenderListener::onFrameInfoUpdate(IAFFrame::AFFrameInfo &info, bool rendered)
{
    mPlayer.RenderCallback(ST_TYPE_VIDEO, rendered, info);
}

bool SuperMediaPlayer::ApsaraVideoProcessTextureCallback::init(int type)
{
#ifdef ENABLE_VIDEO_FILTER
    std::lock_guard<std::mutex> filterLock(mPlayer.mFilterManagerMutex);
    if (mPlayer.mFilterManager) {
        return mPlayer.mFilterManager->initFilter(IVideoFilter::Texture, type);
    }
#endif
        return false;

}

bool SuperMediaPlayer::ApsaraVideoProcessTextureCallback::needProcess()
{
#ifdef ENABLE_VIDEO_FILTER
    std::lock_guard<std::mutex> filterLock(mPlayer.mFilterManagerMutex);
    if (mPlayer.mFilterManager) {
        return mPlayer.mFilterManager->hasFilter(IVideoFilter::Texture, "");
    }
#endif
        return false;

}

bool SuperMediaPlayer::ApsaraVideoProcessTextureCallback::push(std::unique_ptr<IAFFrame> &textureFrame)
{
    return mPlayer.push(textureFrame);
}

bool SuperMediaPlayer::ApsaraVideoProcessTextureCallback::pull(std::unique_ptr<IAFFrame> &textureFrame)
{
    return mPlayer.pull(AF_PIX_FMT_CICADA_TEXTURE, textureFrame);
}
void SuperMediaPlayer::beginDiscontinuity(int64_t targetUs)
{
    mDiscontinuity.generation.fetch_add(1);
    mDiscontinuity.targetUs = targetUs;
    mDiscontinuity.startUs = INT64_MIN;

    
    mDiscontinuity.acceptedFramePos = INT64_MIN;
    
    const bool armAudioLanding = mSeekAudioLandingReset && (targetUs != INT64_MIN);
    mSeekAudioLandingReset = false;
    mAudioLandingDropLoggedGen = -1;
    mDiscontinuity.audioLandingPending = armAudioLanding;
    
    mDiscontinuity.filterActive = (targetUs != INT64_MIN);
}

void SuperMediaPlayer::markDiscontinuityStartUs(int64_t startUs)
{
    if (startUs > INT64_MIN && mDiscontinuity.startUs == INT64_MIN) {
        mDiscontinuity.startUs = startUs;
    }
}

int SuperMediaPlayer::discontinuityGeneration() const
{
    return mDiscontinuity.generation.load();
}


bool SuperMediaPlayer::shouldDropForDiscontinuity(int64_t framePos, int64_t frameDur)
{
    const int generation = mDiscontinuity.generation.load();

    /* 过滤未激活：正常播放，或本次不连续点已经由落点帧结束掉 ⇒ 纯查询、无副作用。 */
    if (!mDiscontinuity.filterActive.load()) {
        return false;
    }

    const int64_t targetUs = mDiscontinuity.targetUs;

    /* 目标未知（Reset 之后不该走到这里，防御性兜底）：不过滤。 */
    if (targetUs == INT64_MIN) {
        mDiscontinuity.filterActive = false;
        return false;
    }
    if (framePos == INT64_MIN) {
        acceptDiscontinuityLandingFrame(framePos, generation, "the frame position is unknown");
        return false;
    }
    if (frameDur <= 0) {
        frameDur = 1;
    }

    if (framePos + frameDur <= targetUs) {
        return true;
    }
    acceptDiscontinuityLandingFrame(framePos, generation, "the frame contains or passes the target");
    return false;
}


void SuperMediaPlayer::acceptDiscontinuityLandingFrame(int64_t framePos, int generation, const char *reason)
{
    const int64_t targetUs = mDiscontinuity.targetUs;
    const int64_t offsetFromTargetUs =
            (framePos == INT64_MIN || targetUs == INT64_MIN) ? 0 : (framePos - targetUs);

    mDiscontinuity.acceptedFramePos = framePos;
    /* 这就是"结束本次过滤"的唯一写点（另一个出口是 beginDiscontinuity）。 */
    mDiscontinuity.filterActive = false;

    
    if (mVideoSwitchInFlight) {
        finishQualitySwitch(true, "quality switch rendered");
    }

   
    if (targetUs != INT64_MIN) {
        mMasterClock.setTime(targetUs);
    }

    AF_LOGW("seek landing frame accepted: pts=%lld, offsetFromTarget=%+lld ms, generation=%d, "
            "afterSeekMs=%lld, reason=%s — the filter stops HERE and only here (SeekEnd never closes it); "
            "position and master clock are both based on target=%lld and are monotonic from now on\n",
            (long long) framePos, (long long) (offsetFromTargetUs / 1000), generation,
            (long long) (mSeekRequestMs > 0 ? af_getsteady_ms() - mSeekRequestMs : -1),
            reason != nullptr ? reason : "-", (long long) targetUs);
}


std::string SuperMediaPlayer::GetVideoCodecSupportJson()
{
    return decoderFactory::getEffectiveCodecSupportJson();
}

int SuperMediaPlayer::SetVideoCodecSupportJson(const std::string &json)
{
    if (json.empty()) {
        /* 空串 = 清除应用层覆盖，恢复内核自己探测（get 之后返回 source = "kernel"）。 */
        std::shared_ptr<const decoderFactory::AppCodecSupport> mine;
        {
            std::lock_guard<std::mutex> lock(mAppCodecSupportMutex);
            mine = mAppCodecSupport;
            mAppCodecSupport.reset();
        }
        /* 只清"还是自己发布的那一份"：别的播放器实例后来发布过就不动它。 */
        decoderFactory::clearAppCodecSupportIf(mine);
        AF_LOGI("app video codec support cleared: the kernel probes the device again\n");
        return 0;
    }

    /*
     * 先解析并**完整校验**，成功了才动状态：畸形/非法 JSON 一律返回非 0，
     * 当前状态一个字节都不变（不允许半套用）。
     */
    std::shared_ptr<const decoderFactory::AppCodecSupport> parsed;
    const int ret = decoderFactory::parseAppCodecSupportJson(json, parsed);

    if (ret != 0 || parsed == nullptr) {
        AF_LOGW("reject app video codec support json (ret=%d): the current state is unchanged\n", ret);
        return (ret != 0) ? ret : -EINVAL;
    }

    {
        std::lock_guard<std::mutex> lock(mAppCodecSupportMutex);
        mAppCodecSupport = parsed;
    }
    decoderFactory::publishAppCodecSupport(parsed);
    AF_LOGI("app video codec support accepted: %d hardware codec(s), %d preference entr(ies), "
            "preferred=%s; the kernel will NOT probe the device while this is set\n",
            (int) parsed->hwDecode.size(), (int) parsed->preference.size(),
            parsed->preferred.empty() ? "(auto)" : parsed->preferred.c_str());
    return 0;
}


void SuperMediaPlayer::SetCacheConfig(const CacheConfig &config)
{
#ifdef ENABLE_CACHE_MODULE
    if (!mCacheConfig.isSame(config)) {
        if (std::shared_ptr<CacheManager> cacheManager = cacheManagerOrNull()) {
            cacheManager->stop("cache stopped by change config");
        }

        mCacheConfig = config;
    }
#else
    (void) config;
#endif
}

std::string SuperMediaPlayer::GetCachePathByURL(const std::string &url)
{
#ifdef ENABLE_CACHE_MODULE
    return CacheManager::getCachePath(url, mCacheConfig);
#else
    (void) url;
    return "";
#endif
}
