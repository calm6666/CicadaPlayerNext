//
// CicadaPlayerItem —— 给 QML 用的播放器组件（视频画面 + 播放控制）。
//
// QML 侧用法：
//     import QtPlayer
//     CicadaPlayerItem {
//         id: player
//         anchors.fill: parent
//         source: "file:///D:/video/4k.mp4"
//         autoPlay: true
//     }
//     Button { text: player.playing ? "暂停" : "播放"; onClicked: player.togglePause() }
//
// 线程模型（这是整个组件最关键的部分）
// ------------------------------------
//   * 框架的 VSync 线程  -> onVideoFrame()：只把帧 clone() 一份放进 m_pendingFrame
//     （加锁），然后**队列投递**一次 QQuickItem::update()。**不碰任何 Qt 图形 API**；
//     update() 本身不能在这个线程上直接调（Qt 会拒绝并打日志，画面就不重绘了）；
//   * Qt 的渲染线程      -> updatePaintNode()：把帧变成 GPU 纹理（零拷贝路由
//     CicadaVideoTexture 决定），交给 QSGSimpleTextureNode 绘制；
//   * Qt 的主/UI 线程    -> 播放控制（play/pause/seek）与属性更新。
//
// 为什么不能在 VSync 线程里直接建纹理：QSGTexture / QRhi / 原生设备句柄都只在渲染
// 线程有效（Qt 的场景图是单线程渲染模型），跨线程用轻则画面错乱重则崩溃。
//
#ifndef CICADA_QT_CICADAPLAYERITEM_H
#define CICADA_QT_CICADAPLAYERITEM_H

#include <QtQuick/QQuickItem>
#include <QtCore/QMutex>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
/* QVariantMap：统计信息（stats 属性）的载体，见 Q_PROPERTY stats。 */
#include <QtCore/QVariantMap>
/*
 * QImage：截屏结果的载体（m_lastSnapshot 是**按值成员**，所以必须完整定义，
 * 不能只前置声明）。见 notifySnapshot() 与 .cpp 里的 onCaptureScreenCb。
 */
#include <QtGui/QImage>
/* m_sceneGraphReady：跨线程的一次性标志。 */
#include <atomic>
#include <memory>

/*
 * playerListener：**必须给完整定义，不能只做前置声明**。
 *
 * 它定义在 mediaPlayer/native_cicada_player_def.h（全局命名空间里的
 * `typedef struct playerListener_t {...} playerListener;`），而 MediaPlayer.h 里有
 *     playerListener mListener{nullptr};      // 按值成员
 * 按值成员在**每个包含 MediaPlayer.h 的编译单元**里都要求完整类型；只前置声明的话
 * MSVC 会直接报 C2079 "使用未定义 struct playerListener"（C2027/C2440/C2439 一串）。
 * 所以这里把定义的头直接 include 进来。
 */
#include <native_cicada_player_def.h>

class QSGNode;
class QSGTexture;
class IAFFrame;

namespace Cicada {
    class MediaPlayer;
}

/*
 * 对象方式入口用到的构造器类型（见 platform/QtPlayer/src/CicadaManifestBuilder.h）。
 * 这里只前向声明：头文件不必把构造器实现拖进来，.cpp 里 include 即可。
 */
namespace CicadaManifest {
    class Builder;
}

namespace cicadaqt {

    class CicadaVideoTexture;

    class CicadaPlayerItem : public QQuickItem {
        Q_OBJECT
        QML_ELEMENT

        /* 播放地址：支持本地文件（file:// 或绝对路径）和网络 URL。 */
        Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
        /* 是否正在播放（只读）。 */
        Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
        /* 播放状态（只读），对应下面的 Status 枚举。 */
        Q_PROPERTY(Status status READ status NOTIFY statusChanged)
        /* 当前播放位置，毫秒（只读，界面绑定进度条用）。 */
        Q_PROPERTY(qint64 position READ position NOTIFY positionChanged)
        /*
         * 已缓冲到哪儿，毫秒（只读）。
         *
         * 进度条上那一段浅色就是它（参考实现里叫 buffered）。值来自框架的
         * 播放器回调 `BufferPositionUpdate`，不是自己猜的。
         */
        Q_PROPERTY(qint64 bufferedPosition READ bufferedPosition NOTIFY bufferedPositionChanged)
        /*
         * 是否正在缓冲（读不到数据、视频路断粮）。
         *
         * 来自框架的 LoadingStart / LoadingEnd 回调（SuperMediaPlayer 起缓冲时
         * 会 `NotifyLoading(loading_event_start)` + 暂停主时钟 + 暂停音频渲染；
         * 缓冲够了再 `loading_event_end` 自动恢复）。**这是以前没接的一条线** ——
         * 界面因此只知道"打开中/就绪"，不知道"播放中卡住了"，于是出现过
         * "视频冻住但弹幕还在滚"。
         */
        Q_PROPERTY(bool buffering READ buffering NOTIFY bufferingChanged)
        /* 当前下载速度，bit/s（只读）。界面用它显示 KB/s / MB/s。 */
        Q_PROPERTY(qint64 downloadSpeed READ downloadSpeed NOTIFY downloadSpeedChanged)
        /*
         * 下载速度的显示文本：> 1MB/s 用 MB/s，否则用 KB/s（用户明确要求）。
         * 直接在这里格式化，界面不用再算。
         */
        Q_PROPERTY(QString downloadSpeedText READ downloadSpeedText NOTIFY downloadSpeedChanged)
        /* 总时长，毫秒；直播/未知时为 0（只读）。 */
        Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)
        /* 音量 0.0 ~ 1.0。 */
        Q_PROPERTY(qreal volume READ volume WRITE setVolume NOTIFY volumeChanged)
        /* 静音开关。 */
        Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)
        /* 倍速播放（0.5 ~ 2.0 之间比较安全）。 */
        Q_PROPERTY(qreal playbackRate READ playbackRate WRITE setPlaybackRate NOTIFY playbackRateChanged)
        /* 视频原始宽高（只读），旋转前。 */
        Q_PROPERTY(int videoWidth READ videoWidth NOTIFY videoSizeChanged)
        Q_PROPERTY(int videoHeight READ videoHeight NOTIFY videoSizeChanged)
        /* 加载完成后自动播放。 */
        Q_PROPERTY(bool autoPlay READ autoPlay WRITE setAutoPlay NOTIFY autoPlayChanged)
        /* 是否循环播放。 */
        Q_PROPERTY(bool loop READ loop WRITE setLoop NOTIFY loopChanged)
        /*
         * 是否请求硬件解码（硬解）。默认开。
         *
         * 关掉之后框架会用软件解码器（CPU 解码），画面自动走 CPU 回退路径
         * （swscale -> QImage -> QSGTexture）：零拷贝的前提是"解码器直接交 GPU 纹理"，
         * 软解交出来的是内存里的 CPU 帧，所以这时零拷贝必然不成立。
         *
         * 这个开关**只在建解码器的时候被读到**（MediaPlayer::EnableHardwareDecoder 是在
         * Prepare() 里生效的），所以运行中改它会自动把当前片源重新打开一次并 seek 回原来的
         * 位置（见 .cpp 的 setHardwareDecoding）。没有片源时只是记下设置。
         */
        Q_PROPERTY(bool hardwareDecoding READ hardwareDecoding WRITE setHardwareDecoding NOTIFY hardwareDecodingChanged)
        /*
         * 画面额外旋转角度（0/90/180/270，只影响显示）。
         *
         * 注意：视频自带的旋转元数据（手机竖屏视频常见）由组件自动读取
         * MediaPlayer::GetVideoRotation() 并叠加，不需要在这里设置。
         *
         * 为什么不叫 rotation：**QQuickItem 自己就有 `rotation` 属性**（qreal，item 的
         * 变换旋转）。同名会把它盖掉，Qt 每次实例化都警告
         *     Member rotation of the object cicadaqt::CicadaPlayerItem overrides a
         *     member of the base object. Consider renaming it ...
         * 而且 QML 里再也没法用标准的 `rotation` 去旋转 item 了。所以这个"额外角度"
         * 用一个不冲突的名字：extraRotation。
         */
        Q_PROPERTY(int extraRotation READ extraRotation WRITE setExtraRotation NOTIFY extraRotationChanged)
        /* 水平镜像显示（自拍视频常见）。 */
        Q_PROPERTY(bool mirror READ mirror WRITE setMirror NOTIFY mirrorChanged)
        /*
         * 诊断信息（只读）：
         *   backend  = 当前实际使用的渲染路径（零拷贝 or CPU 回退），
         *   zeroCopy = 是否正在零拷贝，
         *   device   = 解码用的 GPU 设备（Qt 的设备还是框架自己建的）。
         * 界面上挂个 Text 显示出来，排查"到底走没走零拷贝"最快。
         */
        Q_PROPERTY(QString backend READ backend NOTIFY backendChanged)
        Q_PROPERTY(bool zeroCopy READ zeroCopy NOTIFY backendChanged)
        Q_PROPERTY(QString device READ device NOTIFY backendChanged)
        /*
         * 【解码方式】（只读）：**当前活动解码器实际在用**的硬解/软解，配上当前流的
         * 编码短名，按市面通用写法组装：
         *     硬解 HEVC（NVIDIA GeForce RTX 4060）   —— 硬解且拿得到显卡名
         *     硬解 HEVC                              —— 硬解、但拿不到显卡名
         *     软解 H.264                              —— 软解（**不写**型号）
         *     硬解 / 软解                             —— 编码拿不到（不写"未知"、不猜）
         * 没有视频流 / 还没有播放器时是**空串**（界面按 "--" 处理）。
         *
         * 事实来源（不是能力、也不是配置）：内核**活动解码器实例**自己的读数
         * （MediaPlayer::IsVideoDecoderHardware() → ICicadaPlayer::IsVideoDecoderHardware()
         * → IDecoder::isHardwareDecoderInUse()），所以运行期从硬解退回软解之后，
         * 这里会跟着变成"软解"。编码来自当前流的 StreamInfo::videoCodec
         * （内核归一化好的**规范短名** "H.264" / "H.265" / "AV1" / "VP9" / "MPEG-4" / "MPEG-2"）；
         * 显示时经 .cpp 的 codecDisplayName() 换成用户认得的写法（H.265 → **HEVC**），
         * 其余编码原样 —— 短名本身没改，内部（分组/认行）用的还是它。
         *
         * 值跟着统计信息一起 2Hz 刷新（见 .cpp 的 refreshDecodeMethod()），
         * **不新增计时器**；变了才发 decodeMethodChanged()。
         *
         * 与 backend() 的区别：那个是"渲染/呈现走哪条路"（零拷贝还是 CPU 回拷），
         * 这个是"解码在 GPU 还是 CPU"，两者不是一回事，所以两个属性都保留。
         */
        Q_PROPERTY(QString decodeMethod READ decodeMethod NOTIFY decodeMethodChanged)
        /* 最近一次错误信息（空串表示没有错误）。 */
        Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)

        /*
         * ---- 流媒体（对象形式的 HLS/DASH 清单）----
         *
         * 除了"给个 URL"（source），组件还支持**直接把清单对象喂进来**：
         *     player.setManifest({
         *         duration: 268.2, minBufferTime: 8.3,
         *         video: [ { id: 0, baseUrl: "...", bandwidth: 10000000,
         *                    width: 3840, height: 2160, codecs: "avc1.4d4034",
         *                    segmentInfo: { mode: "template", initialization: "0-0.m4s", ... } }, ... ],
         *         audio: [ { id: 4, baseUrl: "...", bandwidth: 128000, ... } ]
         *     })
         * 结构就是 hili-player 的 `dist/test-{hls,dash}*.json` 那一套（camelCase，
         * `segmentInfo.mode` 支持 "template" 与 "list"）。框架侧对应
         * `MediaPlayer::SetDataSource(const std::string &jsonManifest)` —— 也就是说
         * **不用** m3u8/mpd 文本、不用自己去请求清单。
         *
         * `streaming`：当前片源是不是这种"清单对象"（界面据此决定要不要显示清晰度菜单）。
         */
        Q_PROPERTY(bool streaming READ streaming NOTIFY streamingChanged)
        /*
         * 有没有片源（地址 **或** 清单对象）。
         *
         * 【为什么不能只看 source】清单对象播放时 source 是空的（见 setManifestJson），
         * 界面里所有"有没有片源"的判断都必须用这个属性 —— 否则会出现"正在播清单对象，
         * 但'按 Ctrl+O 选择本地视频'那行提示一直挂在画面上"（用户实测报过）。
         */
        Q_PROPERTY(bool hasMedia READ hasMedia NOTIFY hasMediaChanged)
        Q_PROPERTY(QString manifestJson READ manifestJson NOTIFY streamingChanged)
        /*
         * 当前片源的**协议标签**（只读，界面左上角那颗小徽标显示的就是它）：
         *   DASH / HLS / FLV / MP4 / MKV / TS / …   —— 按扩展名/清单对象判断
         *   RTMP / RTSP / SRT / UDP / RTP           —— 按 URL scheme 判断
         *   FILE                                    —— 本地文件但后缀认不出来
         * 判断优先级：清单对象 > URL 后缀 > URL scheme（见 .cpp 的 detectProtocol()）。
         */
        Q_PROPERTY(QString protocol READ protocol NOTIFY protocolChanged)
        /* 协议的一句话说明（徽标的悬停提示用），例如 "HTTP 动态自适应流 (DASH)"。 */
        Q_PROPERTY(QString protocolDetail READ protocolDetail NOTIFY protocolChanged)

        /*
         * 直播模式开关（**由 QML 明确设置，组件不猜**）。
         *
         * 【为什么需要它】HTTP-FLV 直播和 FLV 点播在传输层长得一样（都是 http 上的 flv），
         * 靠后缀/URL 猜必然错一半（本地测试服务里 /live/1080p.flv 是直播、
         * /vod/sample.flv 是点播）。而**只有播放器界面知道用户点的是"直播"还是"点播"**，
         * 所以由它写这个属性。
         *
         * 置真之后组件做两件事（都在 createPlayer 里）：
         *   1. 把缓冲收成 4 秒上限/2 秒高水位/0.5 秒起播 —— 直播缓冲有多厚，画面就比主播晚多久，
         *      框架默认的 40 秒上限会把"直播"变成"延时 40 秒的录像"（见 .cpp 里那段说明）；
         *   2. 日志里明确写出走的是直播策略，排障时一眼能看出用的是哪套缓冲。
         *
         * 【必须在设置 source 之前写】缓冲参数只在 createPlayer()（= 换片源那一刻）生效，
         * 所以正确的顺序是：`player.liveMode = true; player.source = "http://…/live/1080p.flv"`。
         * 设晚了不会崩，只是那一路仍然按点播的缓冲跑（会在日志里说明）。
         */
        Q_PROPERTY(bool liveMode READ liveMode WRITE setLiveMode NOTIFY liveModeChanged)
        bool liveMode() const
        {
            return mLiveMode;
        }
        void setLiveMode(bool on);

        /*
         * ---- 统计信息（右键菜单 →「视频统计信息」那个面板）----
         *
         * 只读 map，2Hz 刷新，**全部是框架的真实读数**（键名见 .cpp 的 refreshStats()）：
         *   width/height/frameRate        当前视频流的分辨率与源帧率
         *   videoCodec/audioCodec         编解码器（枚举翻译成 "H.264"/"AAC" 这种）
         *   videoBitrate/audioBitrate     码率（bps）
         *   mediaType                     形如 video/mp4;codecs="avc1…",audio/mp4;codecs="mp4a…"
         *   playerType                    DashPlayer / HlsPlayer / LocalFile / HttpPlayer …
         *   videoHost/audioHost           当前流的来源主机（清单对象取 baseUrl 的 host，
         *                                 普通地址取 URL 的 host）
         *   videoSpeed/audioSpeed         缓存速度（bps；核心只有一个合计值，音频单独用同名值）
         *   renderFps                     实际渲染帧率（GetOption("renderFps")）
         *   droppedFrames/totalFrames     丢帧/总渲染帧（GetOption("videoDroppedInfo")）
         *   bufferMs/positionMs/durationMs 缓冲时长/位置/总时长（毫秒）
         *   decodeMode                    渲染/呈现走哪条路（零拷贝还是 CPU 回拷），
         *                                 取值是 backend() 那一类**实现细节**文案
         *   decodeMethod                  解码方式（"硬解 HEVC（显卡名）"/"软解 H.264"），
         *                                 统计面板「解码方式」那一行用的就是它，见 Q_PROPERTY
         *                                 decodeMethod（两个键不是一回事，别混用）
         *   container                     容器名
         *   hdr                           静态 HDR 类型（"SDR"/"HDR10"，拿不到就是空串）
         */
        Q_PROPERTY(QVariantMap stats READ stats NOTIFY statsChanged)

        /*
         * ---- 统计信息里的三条曲线（只在面板打开时才采样）----
         *
         * 每项形如 `{ t: <毫秒时间戳>, speed: <B/s>, fps: <帧率>, buffer: <秒> }`，
         * 只保留最近 `STATS_HISTORY_MAX` 个采样点（500ms 一个 → 约 30 秒窗口）。
         *
         * 【为什么要 start/stop】曲线要的是"持续的采样"，但用户在没开面板时不需要它 ——
         * 白采样白占内存，还多一份每 500ms 的定时器唤醒。所以：
         *   面板打开 → startStatsSampling()（同时开始刷 stats）
         *   面板关闭 → stopStatsSampling()
         * 采样与刷新共用一个定时器（见 ensureStatsTimer / stopStatsSampling）。
         */
        Q_PROPERTY(QVariantList statsHistory READ statsHistory NOTIFY statsHistoryChanged)

        /*
         * 开始/停止采样统计曲线（面板开/关时由 QML 调用）。
         * 开始会立刻采一个点并启动定时器；停止会清空历史（下次打开重新开始）。
         */
        Q_INVOKABLE void startStatsSampling();
        Q_INVOKABLE void stopStatsSampling();
        /* 曲线是不是正在采样（QML 用它显示"等待数据…"）。 */
        Q_PROPERTY(bool statsSampling READ statsSampling NOTIFY statsHistoryChanged)
        bool statsSampling() const
        {
            return m_statsSampling;
        }
        /*
         * 清单来源的提示（清单文件名或 URL）。
         *
         * 转换器用它决定走哪条内部管线，规则和参考实现一致：
         *   * 对象里 `isMasterPlaylist === true` → HLS（detectStreamTypeFromObject）；
         *   * 否则看名字/URL 里有没有 hls/dash、或后缀是不是 .m3u8/.mpd
         *     （VideoPlayer.detectStreamFormat() 那套判断）；
         *   * 都判断不出来 → DASH（参考实现的默认值）。
         * 命令行直接喂 .json 时（appQtPlayer.exe test-hls-v5.json）就是靠它区分的。
         */
        Q_PROPERTY(QString manifestHint READ manifestHint WRITE setManifestHint NOTIFY streamingChanged)
        /*
         * 清晰度列表（只读）。每项是一个 map：
         *   { streamIndex, width, height, bandwidth, bitrate, description, label,
         *     codec, codecLabel }
         * 数据来自框架的 MediaInfoGet 回调（见 .cpp 里 notifyQualities 的说明），
         * 按带宽从高到低排好序，界面直接照着渲染就行。
         *
         * `label`      —— 档位名（4K / 2K / 1080P / …，见 .cpp 里那套换算）。
         * `codec`      —— 框架归一化后的**编码短名**（"H.264"/"H.265"/"AV1"/…），**内部标识**：
         *                 合并分组与认行（sameQualityRow）用它，界面别拿它当显示文案。
         * `codecLabel` —— 给徽标直接显示的文本，= codec 经 .cpp 的 codecDisplayName() 换成的
         *                 **显示名**（H.265 → "HEVC"，其余同 codec）；**认不出来时是空串**，
         *                 界面此时不渲染徽标（不显示"未知"、也不去猜）。
         *
         * **一行 = 一个分辨率 + 一套编码**：同一分辨率挂着多条流时（DASH 里常见的
         * H.264 + H.265 各一条），同分辨率**不同编码**在 .cpp 里是**两条独立条目**
         * （各自带 codecLabel、各自有自己的 streamIndex，各切各的流）；同分辨率**同编码**
         * 若仍有多条，才合并成一条，`streamIndex` 取该组带宽最大的那条当代表，
         * 切档用的就是它。同一组的其它流对界面不可见。
         *
         * 非流媒体片源（本地文件、单码率 URL）这里是空列表。
         */
        Q_PROPERTY(QVariantList qualities READ qualities NOTIFY qualitiesChanged)
        /* 当前清晰度在 qualities 里的下标（-1 = 还不知道 / 非流媒体）。 */
        Q_PROPERTY(int qualityIndex READ qualityIndex NOTIFY qualityChanged)
        /*
         * 是否处于"自动清晰度"（ABR）状态。
         *
         * 语义和框架严格对齐：手动选过某一档之后框架会把 ABR 关掉（
         * MediaPlayer::SelectTrack 里对视频流调 mAbrManager->EnableAbr(false)），
         * 这个属性就跟着变 false；调 useAutoQuality() 才回到 true。
         * **不要**自己写它，只能读（没有 WRITE）。
         */
        Q_PROPERTY(bool autoQuality READ autoQuality NOTIFY qualityChanged)
        /* true 表示 pending decoder 尚未提交，QML 可据此显示切换中的提示。 */
        Q_PROPERTY(bool qualitySwitching READ qualitySwitching NOTIFY qualitySwitchStatusChanged)
        /*
         * 最近一张快照的**版本号**：每收到一张新快照 +1，还没有快照时是 0。
         *
         * 【为什么暴露版本号而不是 QImage 本身】
         * QML 的 Image 只认 URL，而 image://snapshot/<rev> 这种 URL 要靠 <rev> 变化
         * 才会重新去取图（provider 手里永远只有"最近一张"，见 SnapshotImageProvider.h）。
         * 把 QImage 直接暴露成属性没有意义：QML 画不了它，还会每次拷一份大图。
         *
         * 界面用法（进度条悬停气泡）：
         *     Image { source: "image://snapshot/" + player.snapshotRevision }
         *
         * 取值 0 表示"还没有任何快照"，此时 URL 是 image://snapshot/0，provider 返回空图
         * → 界面上就是一个空框（不是破图）。
         */
        Q_PROPERTY(int snapshotRevision READ snapshotRevision NOTIFY snapshotChanged)

        /*
         * seek 版本号：**每调用一次 seek() 就 +1**。
         *
         * 【为什么要有它】弹幕层判断"要不要把弹幕时钟硬对齐到新位置"时，不能靠"播放器报的位置
         * 跳了多少"去猜：框架恢复播放时会先补发一条**滞后**的位置，那会被误判成 seek，
         * 于是把时钟往回拽 —— 用户看到的就是"暂停再播放，弹幕往后弹一下"（实测两次）。
         * 有了这个版本号，"真 seek" 就等于"我们真的调过 seek()"，与其他位置抖动彻底分开。
         */
        Q_PROPERTY(int seekRevision READ seekRevision NOTIFY seekRevisionChanged)

    public:
        enum Status {
            Null,       /* 还没设置地址 */
            Loading,    /* 正在打开/缓冲 */
            Ready,      /* 就绪，还没播 */
            Playing,
            Paused,
            Stopped,
            End,        /* 播放结束 */
            Error
        };
        Q_ENUM(Status)

        explicit CicadaPlayerItem(QQuickItem *parent = nullptr);
        ~CicadaPlayerItem() override;

        QUrl source() const
        {
            return m_source;
        }
        void setSource(const QUrl &source);

        bool playing() const
        {
            return m_status == Playing;
        }
        Status status() const
        {
            return m_status;
        }
        qint64 position() const
        {
            return m_position;
        }
        qint64 bufferedPosition() const
        {
            return m_bufferedPosition;
        }
        bool buffering() const
        {
            return m_buffering;
        }
        qint64 downloadSpeed() const
        {
            return m_downloadSpeed;
        }
        QString downloadSpeedText() const;
        qint64 duration() const
        {
            return m_duration;
        }
        qreal volume() const
        {
            return m_volume;
        }
        void setVolume(qreal volume);
        bool muted() const
        {
            return m_muted;
        }
        void setMuted(bool muted);
        qreal playbackRate() const
        {
            return m_playbackRate;
        }
        void setPlaybackRate(qreal rate);
        int videoWidth() const
        {
            return m_videoWidth;
        }
        int videoHeight() const
        {
            return m_videoHeight;
        }
        bool autoPlay() const
        {
            return m_autoPlay;
        }
        void setAutoPlay(bool autoPlay);
        bool loop() const
        {
            return m_loop;
        }
        void setLoop(bool loop);
        bool hardwareDecoding() const
        {
            return m_hardwareDecoding;
        }
        void setHardwareDecoding(bool enabled);
        int extraRotation() const
        {
            return m_extraRotation;
        }
        void setExtraRotation(int rotation);
        bool mirror() const
        {
            return m_mirror;
        }
        void setMirror(bool mirror);

        QString backend() const;
        bool zeroCopy() const;
        QString device() const;
        /*
         * 解码方式（见上面 Q_PROPERTY 的说明）：返回最近一次刷新算出来的值，
         * 本身只是读成员，不去问框架（所以 QML 绑定里读它没有副作用、也没有锁）。
         */
        QString decodeMethod() const
        {
            return m_decodeMethod;
        }
        QString errorString() const
        {
            return m_errorString;
        }

        bool streaming() const
        {
            return !m_manifestJson.isEmpty();
        }
        /* 有没有片源（地址 **或** 清单对象）；见 Q_PROPERTY hasMedia 的说明。 */
        bool hasMedia() const
        {
            return !m_source.isEmpty() || !m_manifestJson.isEmpty();
        }
        QString manifestJson() const
        {
            return m_manifestJson;
        }
        QString manifestHint() const
        {
            return m_manifestHint;
        }
        /* 见上面 Q_PROPERTY 的说明：当前片的协议标签 + 一句话说明。 */
        QString protocol() const
        {
            return m_protocol;
        }
        QString protocolDetail() const
        {
            return m_protocolDetail;
        }
        /* 见 Q_PROPERTY stats 的说明（框架真实读数，2Hz 刷新）。 */
        QVariantMap stats() const
        {
            return m_stats;
        }
        /* 曲线数据（见 Q_PROPERTY statsHistory）。 */
        QVariantList statsHistory() const
        {
            return m_statsHistory;
        }
        /*
         * 把文本放进系统剪贴板（右键菜单里的"复制视频地址（精准空降）"）。
         * QML 自己没有剪贴板 API，所以在 C++ 里做。
         * 返回 false 表示拿不到剪贴板。
         */
        Q_INVOKABLE bool copyTextToClipboard(const QString &text);

        /*
         * ---- 视频色彩调整（右键菜单 →「视频色彩调整」）----
         *
         * 三条滑块都是 **0~200，100 = 中性**（就是面板上的原始值）。调用后：
         *   * 值记在本组件里（换后端/换片源后能重新应用）；
         *   * 转给渲染后端（Windows 的 D3D11 零拷贝路径用视频处理器的
         *     BRIGHTNESS/CONTRAST/SATURATION 过滤器实现，**每帧零额外代价**）。
         *
         * colorAdjustSupported 告诉界面"当前后端到底支不支持"：目前只有 Windows 的
         * D3D11 路径实现了；macOS(Metal)/Linux(VAAPI)/CPU 回退会返回 false，
         * 界面照实说明（而不是让用户拖了滑块却什么都没发生）。
         */
        Q_INVOKABLE void setColorAdjust(int brightness, int contrast, int saturation);
        Q_PROPERTY(bool colorAdjustSupported READ colorAdjustSupported NOTIFY backendChanged)
        bool colorAdjustSupported() const;
        /*
         * "精准空降"链接：当前片源 + 当前播放位置。
         *   * 普通地址 → "<url>?t=<秒>"（已有 query 时用 &t=）；
         *   * 清单对象 → "清单对象(<名字>) t=<秒>s"（没有可分享的 URL，写清楚是对象）；
         *   * 没有片源 → 空串。
         */
        Q_INVOKABLE QString landingLink() const;
        void setManifestHint(const QString &hint)
        {
            if (m_manifestHint == hint) {
                return;
            }
            m_manifestHint = hint;
            emit streamingChanged();
        }
        QVariantList qualities() const
        {
            return m_qualities;
        }
        int qualityIndex() const
        {
            return m_qualityIndex;
        }
        bool autoQuality() const
        {
            return m_autoQuality;
        }
        bool qualitySwitching() const
        {
            return m_qualitySwitching;
        }
        /* 最近一张快照的版本号（0 = 还没有快照）。见上面 Q_PROPERTY 的说明。 */
        int snapshotRevision() const
        {
            return m_snapshotRevision;
        }

        /* 见上面 Q_PROPERTY 的说明：弹幕层只在它变化时硬对齐时钟 */
        int seekRevision() const
        {
            return m_seekRevision;
        }

        /* ---- QML 可调用的控制接口 ---- */
        Q_INVOKABLE void play();
        /*
         * 请求一次截屏（进度条 hover 预览用）。
         *
         * 走框架的 CaptureScreen：渲染器那边我们补了 device_captureScreen()，
         * 软解/copy-back/硬解零拷贝都能拿到像素（硬解走 av_hwframe_transfer_data 回读）。
         * 结果是**异步**的：框架回调不在 GUI 线程，收到后拷成 QImage 再 queued 投回来，
         * 通过 snapshotRevision 通知 QML（QML 用 image://snapshot/<rev> 取图）。
         */
        Q_INVOKABLE void requestSnapshot();
        Q_INVOKABLE void pause();
        Q_INVOKABLE void togglePause();
        Q_INVOKABLE void stop();
        /*
         * 跳转到 positionMs（毫秒）。
         *
         * **可以随便高频调用**（拖动进度条时每帧都会调）：内部做了两件事，把
         * "几十次/秒的 seek 请求"变成框架扛得住的形式：
         *   1. 单飞：同一时刻只让一个 seek 在跑（用框架的 Seeking/SeekEnd 回调判定）；
         *   2. 最新优先：seek 在跑的时候，后来的请求只记下**最后一个**位置，
         *      上一个结束再发。
         * 细节和"为什么会卡"写在 .cpp 的 seek() 里。
         */
        Q_INVOKABLE void seek(qint64 positionMs);

        /*
         * ---- 流媒体的三个入口 ----
         *
         * setManifest()：喂清单**对象**（QVariantMap / QML 里的 JS 对象都行），
         *   内部转成 JSON 文本交给框架。返回 false 表示对象是空的或转不出 JSON。
         * setManifestJson()：直接喂 JSON 文本（命令行/测试用起来更顺手）。
         * 两者都会**换掉当前片源**（把 source 清空），随后自动 Prepare。
         *
         * 注意：换成普通 URL（setSource）时会反过来把清单清掉，两种片源不会串。
         */
        Q_INVOKABLE bool setManifest(const QVariantMap &manifest);
        /*
         * **对象方式入口**：直接接 CicadaManifest::Builder 的产物（类型安全、IDE 补全、
         * 字段写错在编译期就能发现）。实现与 setManifest(QVariantMap) **完全相同**：
         *   builder.toJsonString() -> setManifestJson(json)
         *     -> SetDataSource(std::string) -> 内核 cJSON 解析 -> ManifestDemuxer
         * 也就是：对象方式和 JSON 文本方式最终是**同一份 JSON 文本**走同一条内核路径，
         * 运行时行为与性能完全一致；应用层不做任何解析/转换。
         *
         * 仍然保留那条警告：往核心传清单**必须传 std::string**，传 const char* 会命中
         * SetDataSource(const char *url) 那个重载，清单会被当成 URL 去打开。
         */
        bool setManifest(const CicadaManifest::Builder &builder);
        Q_INVOKABLE bool setManifestJson(const QString &json);
        /*
         * **按协议**喂清单对象：protocol 是 "dash" 或 "hls"（大小写不敏感）。
         *
         * 【为什么要分开】这两种清单对象**长得不一样**，直接原样丢给核心时常打不开：
         *   * DASH 对象：分片时长来自 segmentInfo.segmentTimeline（可变时长，最后一个
         *     分片短一截），没有 mediaSequence 这个概念；分片号从 startNumber 数起；
         *   * HLS 对象：分片号从 segmentInfo.mediaSequence 数起（缺了它整条 playlist
         *     的段号会错位），时长通常是 targetDuration 的整数倍。
         * 这里做的是"补全缺失字段 + 纠正协议字段"（只补不改：已有字段一律不动，
         * mediaSourceType 按入口协议写死），补完再交给 setManifestJson()。
         * 失败时返回 false 并把**具体缺什么**写进 errorString()，界面直接显示。
         */
        Q_INVOKABLE bool setManifestForProtocol(const QString &json, const QString &protocol,
                                                const QString &name = QString());
        /*
         * 从本地 .json 文件读清单并播放（路径也接受 file:// URL）。
         *
         * 单独开这个入口是因为 **Qt 默认不允许 qrc: 文档对 file:// 发 XHR**
         * （QML_XHR_ALLOW_FILE_READ 未开），QML 里读本地文件会失败；读文件的活
         * 放在 C++ 里做，和命令行那条路一致。
         */
        Q_INVOKABLE bool loadManifestFile(const QString &path);
        /*
         * 按协议从本地 .json 文件读清单并播放（protocol 见 setManifestForProtocol）。
         */
        Q_INVOKABLE bool loadManifestFileForProtocol(const QString &path, const QString &protocol);

        /*
         * 手动选清晰度。参数是 `qualities` 里的 `streamIndex`（**不是**数组下标！
         * 数组下标 → streamIndex 的换算界面自己做，或者直接用下面那个重载思路：
         * 传数组下标进来也行 —— 这里会先按 `qualities` 找一次）。
         * 框架语义（MediaPlayer::SelectTrack）：选视频档会把 ABR 关掉。
         */
        Q_INVOKABLE void selectQuality(int streamIndex);
        /* 回到自动清晰度（ABR）。对应 SelectTrack(SELECT_TRACK_VIDEO_AUTO)。 */
        Q_INVOKABLE void useAutoQuality();
        /*
         * 起播档位：给一个目标带宽（bps），框架会挑最接近的那一档起播
         * （MediaPlayer::SetDefaultBandWidth）。0 = 用框架默认（最低档起播）。
         * 必须在 Prepare 之前设置 —— 组件会在 createPlayer() 里应用它。
         */
        Q_INVOKABLE void setInitialBandwidth(int bitsPerSecond);

        /*
         * ---- 【设备硬解能力 + 编码效率偏好：取一次、应用侧持久化、下次起播直接传】----
         *
         * 这两个方法把内核的新接口
         *     CicadaGetVideoCodecSupport(player) / CicadaSetVideoCodecSupport(player, json)
         * 暴露给 QML/应用层（内核契约见 mediaPlayer/media_player_api.h 里的注释）。
         * 组件内部走的是 **Cicada::MediaPlayer 的包装转发**
         *     MediaPlayer::GetVideoCodecSupport() / SetVideoCodecSupport(const char *)
         * （mediaPlayer/MediaPlayer.h 里为平台层加的这一对）：**字符串的所有权、
         * malloc/CicadaFreeString 的释放都在包装类里做完了 —— 本组件不持有、也不需要释放
         * 任何 C 字符串**，拿到的就是一个 std::string / QString 值。
         *
         *   videoCodecSupport()         取**当前生效**的那份 JSON：
         *                               {"source":"app"|"kernel",
         *                                "hwDecode":["H.265","H.264"],
         *                                "preference":["AV1","H.265","VP9","H.264","MPEG-4","MPEG-2"]}
         *                               source == "kernel" 表示这份是**内核探测本机**得到的；
         *                               "app" 表示是应用侧传进去、正在生效的那份。
         *                               拿不到（没有播放器 / 内核返回空）时返回空串。
         *   setVideoCodecSupport(json)  把应用侧的那份传给内核：**一旦传进去，内核就不再
         *                               探测设备**，ABR 与起播默认档都按这份集合/偏好选流。
         *                               传空串 = 清除，恢复内核自己探测。
         *                               返回 void：内核解析失败（畸形 JSON）时只是拒绝、
         *                               保持原状态（内核会打日志），组件不额外造一个错误通道。
         *
         * 【为什么应用侧要持久化】
         *   1. 探测"这台设备能硬解哪些编码"在部分平台上是**有成本的**（Android 要枚举
         *      MediaCodecList、Apple 要问 VTIsHardwareDecodeSupported、桌面要问 FFmpeg 的
         *      avcodec_get_hw_config）；而这份结果在一台设备上**基本不变**（除非换硬件/刷机），
         *      播放器却每次起播都要问它；
         *   2. 内核为此把"应用层传进来的那份"设计成**进程内共享的能力快照**
         *      （decoderFactory::AppCodecSupport），传一次就够 —— 它明确建议应用侧
         *      "持久化它，避免每次起播都探测设备"（见 media_player_api.h 的原文）。
         *   所以组件替应用把这件事收口：**QSettings 里有就直接传给内核；没有就先向内核取一次
         *   探测结果、存进 QSettings**（见 .cpp 的 applyPersistedCodecSupport）。
         *   QML 侧什么都不用做就能吃到这个省事；也可以在设置页里自己调这两个接口，
         *   例如把用户挑的偏好序存下来：
         *       Component.onCompleted: console.log(player.videoCodecSupport())
         *       player.setVideoCodecSupport('{"hwDecode":["H.265","H.264"],"preference":["H.265","H.264"]}')
         *
         * 落盘位置：QSettings(IniFormat, UserScope, org "CicadaPlayerNext", app "QtPlayer")，
         * 键 "videoCodecSupport/json"（Windows 上就是 %APPDATA%\CicadaPlayerNext\QtPlayer.ini）。
         * **只在 GUI 线程读写**（QSettings 不是线程安全的）—— 这两个入口、以及下面那个
         * 自动收口点 createPlayer() 本来就都跑在 GUI 线程上。类里不为此新增任何成员：
         * 唯一的存储就是 QSettings（见 .cpp 里的说明）。
         */
        Q_INVOKABLE QString videoCodecSupport() const;
        Q_INVOKABLE void setVideoCodecSupport(const QString &json);

        /*
         * 框架的帧回调（在框架的 VSync 线程上被调用，见 CicadaVideoRender）。
         * 由 QML 组件的构造/析构管理生命周期，外部不要直接调用。
         *
         * 实现要点：这个线程**不能**直接调 QQuickItem::update()，必须队列投递到
         * GUI 线程（否则 Qt 拒绝调度重绘，画面不更新，见 .cpp 里的说明）。
         */
        bool handleVideoFrame(IAFFrame *frame);

        /*
         * 下面这组是给框架的监听回调用的"内部通知接口"。
         *
         * 框架的监听回调跑在它自己的线程上，不能直接改界面属性，所以那些静态回调
         * 只做一件事：用 QMetaObject::invokeMethod(..., Qt::QueuedConnection) 把调用
         * 投递到 item 所在线程（主线程），最终执行的就是这几个函数。写 QML 应用时
         * 不需要调用它们。
         */
        void notifyPrepared();
        void notifyCompletion();
        void notifyFirstFrameShown();
        void notifyVideoSize(int width, int height);
        void notifyPosition(qint64 positionMs);
        /*
         * **GUI 线程**：收到一张新快照（QImage 已经在框架回调那边拷好了，见 .cpp 的
         * onCaptureScreenCb —— 那一侧不在 GUI 线程，且框架的 buffer 回调返回即失效）。
         * 这里只做三件事：记下最近一张、把缩小过的一份发布给 SnapshotImageProvider、
         * 版本号 +1 并发 snapshotChanged（QML 的 Image 靠版本号换 URL 重新取图）。
         */
        void notifySnapshot(const QImage &image);
        /* 主线程：缓冲状态 / 下载速度（来自框架回调，见 .cpp 的 onLoadingStartCb）。 */
        void notifyBuffering(bool buffering);
        void notifyLoadingProgress(int progress);
        void notifyDownloadSpeed(qint64 bitsPerSecond);
        /* 主线程：seek 完成/位置回调等（原有）。 */
        void notifyBufferedPosition(qint64 positionMs);
        void notifyError(int code, const QString &message);
        /* 框架开始/结束一次 seek（Seeking / SeekEnd 回调）。 */
        void notifySeekStarted();
        void notifySeekFinished();
        /*
         * 框架播放状态变化（参数是 PlayerStatus 的整数值，见
         * mediaPlayer/native_cicada_player_def.h）。用 int 是为了让本头文件不必
         * 包含框架的播放器定义。
         */
        void notifyPlayerStatus(int status);

        /* 框架给了新的清晰度列表（MediaInfoGet 回调，最终在 GUI 线程执行）。 */
        void notifyQualities(const QVariantList &qualities);
        /*
         * 一次切流/切轨完成（StreamSwitchSuc 回调）。
         * 参数：框架的流类型（0=视频 1=音频 2=字幕，见 StreamType）、该流的 streamIndex、
         * 视频流的宽/高/带宽，以及该流的编码短名（内核归一化后的，认不出来就是空串）
         * ——非视频流这几个是 0 / 空串。编码只用于"同分辨率、不同编码分列"后的认行
         * （见 .cpp 里 sameQualityRow），**不改变任何对外信号**。
         */
        void notifyStreamSwitched(int streamType, int streamIndex, int width, int height,
                                  int bandwidth, const QString &codec);
        void notifyQualitySwitchStatus(int status, int streamIndex, const QString &description);

        /*
         * 重新算一遍"解码方式"（硬解/软解 + 编码 + 显卡名，见 Q_PROPERTY decodeMethod）。
         *
         * 由 refreshStats() 在 GUI 线程上顺手调用（统计定时器 2Hz），**不新增计时器**：
         *   * 解码器是 Prepare 之后才有的，统计定时器也正好从那时起跑；
         *   * 运行期从硬解退回软解没有回调可挂，靠这个 2Hz 轮询跟上是合适的
         *     （面板本来就是这个刷新率）。
         * 值变了才发 decodeMethodChanged()。
         */
        void refreshDecodeMethod();

        /*
         * 解码/呈现诊断日志（见 .cpp 的实现说明）。
         *
         * 由 refreshDecodeMethod() 在同一拍里调用（统计定时器 2Hz，**不新增定时器**）：
         * 只有"是否硬解 / 是否请求硬解 / 编码 / 显卡名 / 渲染后端 / 零拷贝"这几个值
         * 拼出的签名与上次不同时才打一条 AF_LOGI，值没变就一行都不打。
         *
         * 参数就是 refreshDecodeMethod() 已经取好的那份事实读数，所以日志与面板显示
         * 永远说的是同一件事：
         *   rawCodec  —— 内核规范短名（StreamInfo::videoCodec 那个 char[16] 原样，可能为空串）；
         *   codec     —— 界面上显示的那一段（H.265 → HEVC，见 codecDisplayName()）；
         *   hardware  —— IsVideoDecoderHardware()（活动解码器实例的事实读数）；
         *   deviceName—— CicadaHardwareDevice::deviceName()（可能为空串）。
         */
        void reportDecoderDiagnostics(const char *rawCodec, const QString &codec,
                                      bool hardware, const QString &deviceName);

    signals:
        void sourceChanged();
        void playingChanged();
        void statusChanged();
        void positionChanged();
        void bufferedPositionChanged();
        /* 缓冲状态 / 下载速度（见上面 Q_PROPERTY 的说明）。 */
        void bufferingChanged();
        void downloadSpeedChanged();
        void durationChanged();
        void volumeChanged();
        void mutedChanged();
        void playbackRateChanged();
        void videoSizeChanged();
        void autoPlayChanged();
        void loopChanged();
        void hardwareDecodingChanged();
        void extraRotationChanged();
        void mirrorChanged();
        void backendChanged();
        /* 解码方式（硬解/软解 …）变了：见 Q_PROPERTY decodeMethod。 */
        void decodeMethodChanged();
        void errorStringChanged();
        /* 流媒体片源（清单对象）的有无/内容变化。 */
        void streamingChanged();
        /*
         * 有没有片源（地址或清单对象）变了。
         * 单独一个信号：界面里"没片源就显示提示文字"那种绑定必须跟着它走 ——
         * 清单对象那条路 sourceChanged / streamingChanged 的语义都不完全覆盖。
         */
        void hasMediaChanged();
        /* 协议标签变化（换片源、或清单对象的 mediaSourceType 被改写时）。 */
        void protocolChanged();
        /* 直播模式开关变化（见 Q_PROPERTY liveMode）。 */
        void liveModeChanged();
        /* 统计信息变化（见 Q_PROPERTY stats）。 */
        void statsChanged();
        /* 曲线数据变化（每个采样点一次；开始/停止采样也会发）。 */
        void statsHistoryChanged();
        void qualitiesChanged();
        void qualityChanged();
        /*
         * 一次切流完成，界面可以据此收起"正在切换清晰度"的提示。
         * autoMode = true 表示这次是 ABR 自己切的（不是用户点的）。
         */
        void qualitySwitched(int streamIndex, int width, int height, int bandwidth, bool autoMode);
        /* status 使用 player_quality_switch_status 的整数值，description 为 UTF-8 文本。 */
        void qualitySwitchStatusChanged(int status, int streamIndex, QString description);
        /* 第一帧真正上屏（QML 里可以用它隐藏 loading 动画）。 */
        void firstFrameRendered();
        /* 收到了一张新快照：snapshotRevision 变了，QML 的 Image 该按新 URL 取图了。 */
        void snapshotChanged();
        /* 调过一次 seek() → +1（见上面 Q_PROPERTY 的说明） */
        void seekRevisionChanged();
        /* 播放结束（非循环）。 */
        void finished();

    protected:
        /* 渲染线程：把解码帧变成纹理并绘制。 */
        QSGNode *updatePaintNode(QSGNode *node, UpdatePaintNodeData *data) override;
        /* 主线程：item 完成加载（QML 里的属性都赋值完了）。 */
        void componentComplete() override;
        /* 窗口尺寸变化：等比适配要重算，重新出一帧。 */
        void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

    private slots:
        /* 场景图初始化（渲染线程发出）：这时才能拿到 Qt 的 GPU 设备。 */
        void onSceneGraphInitialized();
        /* 场景图失效：渲染资源要在这里放掉（在渲染线程上）。 */
        void onSceneGraphInvalidated();
        /* 主线程：场景图设备就绪之后再真正开始准备播放。 */
        void startPlaybackWhenReady();

    private:
        /* 创建/销毁播放器（都在主线程）。 */
        void createPlayer();
        void destroyPlayer();
        /*
         * 【硬解能力/偏好的"取回 + 持久化"口子】只在 createPlayer() 里调一次（= 每次
         * 打开片源的那一刻），语义见上面两个 Q_INVOKABLE 的说明：
         *   * QSettings 里有存过的 JSON → 直接 setVideoCodecSupport 给内核（**不再探测设备**）；
         *   * 没有 → 先 videoCodecSupport() 取一次内核探测结果，落盘供下次直接用。
         * 析构时**什么都不做**：这份数据是要跨进程存活的（下次起播直接用），本来就不该
         * 在退出时删；而内核那份"应用层覆盖值"由内核自己在播放器析构时撤销
         * （SuperMediaPlayer 的 clearAppCodecSupportIf）。
         */
        void applyPersistedCodecSupport();
        /*
         * 现取一次总时长并发 durationChanged（变了才发）。
         *
         * listener 里**没有** DurationUpdate 回调，但也不做定时轮询 —— 只在
         * "准备好了 / 首帧 / seek 结束"这几个"时长可能刚变成已知"的事件点上调用。
         * （位置/缓冲位置一律走框架推送：PositionUpdate / BufferPositionUpdate。）
         */
        void refreshDuration();
        /* 真正把一次 seek 交给框架（终态由 Seeking / SeekEnd 事件驱动，没有看门狗）。 */
        void issueSeek(qint64 positionMs);
        /* seek 结束之后的收尾：清状态 + 如果有挂起目标就接着发。 */
        void finishSeek();
        /*
         * 把当前片源重新打开一遍（销毁 + 重建 + Prepare），并把播放位置记下来，
         * 等新的 Prepared 回来再 seek 回去。设置变化（目前是硬解开关）时用。
         */
        void reloadCurrentSource();
        /* 把当前状态同步到界面属性上。 */
        void setStatus(Status status);
        void setErrorString(const QString &error);
        /*
         * 更新协议标签（见上面 protocol/protocolDetail 两个 Q_PROPERTY）。
         * 换片源、换清单对象时调用；值没变不发信号。
         */
        void setProtocol(const QString &label, const QString &detail);

        /*
         * ---- 统计信息（见 Q_PROPERTY stats）----
         *
         * buildStats()          读一次框架的全部实时数字，拼成 QVariantMap；
         * refreshStats()        比较后决定要不要发 statsChanged；
         * ensureStatsTimer()    起 500ms 的刷新定时器（有播放器时才需要）；
         * parseManifestStatsInfo() 从清单 JSON 里取 codecs/mimeType/host（核心不提供
         *                        这些字符串，见 .cpp 里的说明）。
         */
        QVariantMap buildStats() const;
        void refreshStats();
        /* 往曲线里补一个点（只在 m_statsSampling 为真时调用）。 */
        void appendStatsSample(const QVariantMap &snapshot);
        void ensureStatsTimer();
        void parseManifestStatsInfo();
        /*
         * QML 侧适配矩形：按视频宽高比在 item 里算一个**等比居中**的矩形
         * （Scale_AspectFit：整幅画面都在 item 里，短的那一边留黑边）。
         * swapped = true 表示"旋转 90/270 之后"的宽高比（长宽互换）。
         */
        QRectF fittedRect(bool swapped = false) const;
        /*
         * 实际生效的显示角度（0/90/180/270）：
         * 视频自带旋转元数据（通过播放器读）+ QML 的 extraRotation 属性，归一化到 0~359。
         */
        int effectiveRotation() const;
        /* 主线程：把挂起的帧取出来（没有就用上一帧）。 */
        std::unique_ptr<IAFFrame> takeFrameForRendering();

        QUrl m_source;
        Status m_status = Null;
        qint64 m_position = 0;
        /* 已缓冲位置（毫秒），进度条上的缓冲段用它。 */
        qint64 m_bufferedPosition = 0;
        /* 是否在缓冲（框架 LoadingStart/End），界面据此显示"缓冲中 + 速度"。 */
        bool m_buffering = false;
        /* 当前下载速度（bit/s），框架 CurrentDownLoadSpeed 回调。 */
        qint64 m_downloadSpeed = 0;
        qint64 m_duration = 0;
        qreal m_volume = 1.0;
        bool m_muted = false;
        qreal m_playbackRate = 1.0;
        int m_videoWidth = 0;
        int m_videoHeight = 0;
        bool m_autoPlay = true;
        bool m_loop = false;
        /*
         * 是否请求硬解（见 Q_PROPERTY 的说明）。
         *
         * 只在 GUI 线程读写；渲染线程要判断"要不要重算零拷贝"时走 m_textureReprobe
         * 那个原子量，不直接读这个 bool（避免跨线程读一个正在被改的普通成员）。
         */
        bool m_hardwareDecoding = true;
        /*
         * "重新打开片源之后要跳回的位置"，毫秒；0 表示不用跳。
         * 配置变化导致重新 Prepare 时用它把播放位置找回来（见 notifyPrepared）。
         */
        qint64 m_pendingSeekMs = 0;
        /* QML 侧设置的额外旋转（和视频元数据里的旋转叠加）。 */
        int m_extraRotation = 0;
        bool m_mirror = false;
        QString m_errorString;

        /*
         * 流媒体（对象形式的清单）。为空 = 走 source（本地文件/URL）那条路。
         * 两者互斥：setSource() 清它，setManifest*() 清 source。
         */
        QString m_manifestJson;
        /*
         * 清单来源提示（文件名/URL）。**只是给界面/日志看的标签**：
         * 走 HLS 还是 DASH 管线由核心按对象里的 mediaSourceType 决定（缺省 hls），
         * 应用层不参与判断。
         */
        QString m_manifestHint;
        /* 起播档位（bps），0 = 框架默认；在 createPlayer() 里应用。 */
        int m_initialBandwidth = 0;
        /* 清晰度列表（见 notifyQualities）与当前下标、是否自动。 */
        QVariantList m_qualities;
        int m_qualityIndex = -1;
        bool m_autoQuality = true;
        bool m_qualitySwitching = false;

        std::unique_ptr<Cicada::MediaPlayer> m_player;
        /* playerListener 是全局命名空间的 C 结构体（native_cicada_player_def.h）。 */
        std::unique_ptr<playerListener> m_listener;
        std::unique_ptr<CicadaVideoTexture> m_textureBackend;

        /* 框架 VSync 线程 -> 渲染线程 的交接；用互斥锁保护。 */
        QMutex m_frameMutex;
        std::unique_ptr<IAFFrame> m_pendingFrame;
        /* 渲染线程持有的"当前帧"，窗口尺寸变化时重绘要用它。 */
        std::unique_ptr<IAFFrame> m_currentFrame;
        /* 渲染线程持有的纹理（QSGSimpleTextureNode 会接管它的释放）。 */
        bool m_flipVertically = false;
        int m_texturePixelWidth = 0;
        int m_texturePixelHeight = 0;

        /*
         * seek 的"单飞 + 最新优先"状态：
         *   m_seekInFlight —— 已经交给框架、还没等到 SeekEnd 的那一次；
         *   m_seekPending  —— 在它跑的时候用户又拖到的新位置（-1 = 没有）。
         */
        bool m_seekInFlight = false;
        qint64 m_seekPending = -1;
        bool m_playerStarted = false;
        /*
         * "首帧已经上屏"是否已经通知过。
         * 渲染线程（updatePaintNode）和主线程（框架的 FirstFrameShow 回调）都可能
         * 触发它，所以用原子量 + exchange：保证只发一次信号，也避免数据竞争。
         */
        std::atomic<bool> m_firstFrameEmitted{false};
        bool m_sceneGraphConnected = false;
        /*
         * 场景图是否已经初始化（也就是纹理后端是否已经探测过零拷贝可用性）。
         * startPlaybackWhenReady() 必须等它为 true 才会真正 Prepare()，
         * 否则可能在看不清零拷贝可用性的情况下就打开了直通开关。
         *
         * 用原子量：它在渲染线程上被置位，在主线程上被读（setSource）。
         */
        std::atomic<bool> m_sceneGraphReady{false};
        /*
         * 渲染线程下一次 updatePaintNode() 要不要把"零拷贝还是 CPU 路径"重新算一遍
         * （比如设置页里刚改了硬解开关）。
         *
         * 用原子量而不是 bool：写它的在 GUI 线程，用它的在渲染线程。取值约定：
         *   -1 = 没有待处理的重算
         *    0 = 重算，且**不允许**零拷贝（硬解关了）
         *    1 = 重算，且允许零拷贝
         *
         * 初值是 1：等于"初始状态允许零拷贝"，正好和 m_hardwareDecoding 的默认值一致，
         * 于是场景图初始化那次可以只用这个原子量做决定，不必去读 m_hardwareDecoding。
         */
        std::atomic<int> m_textureReprobe{1};
        /*
         * "重算完之后要把当前片源重开一遍"的挂起标志。
         *
         * 改硬解开关时在 GUI 线程置位，渲染线程在 updatePaintNode() 里重算完零拷贝路径
         * 后取走（exchange），再回调回 GUI 线程执行 reloadCurrentSource()。
         * 顺序必须是"先重算、再重开"，原因见 setHardwareDecoding() 的注释。
         */
        std::atomic<bool> m_reloadPending{false};

        /*
         * "渲染线程手里那两个会钉住上一个解码器表面池的东西该放掉了"的挂起标志。
         *
         * destroyPlayer()（GUI 线程）置位，updatePaintNode()（渲染线程）取走：要放的是
         * m_currentFrame 和 D3D11 的输入视图，两者都只归渲染线程，所以这里只能传
         * "要不要放"这个结果，不能从 GUI 线程直接去动它们。
         *
         * 为什么值得单开一个标志：FFmpeg 的 d3d11va 把 initial_pool_size 个 surface
         * 放在**同一个** ID3D11Texture2D 里（ArraySize = 池子大小），每个 surface 的
         * AVBufferRef 都对整张数组 AddRef，D3D11 的输入视图同样对解码纹理持有引用 ——
         * 这两个引用任意一个还活着，上一个解码器的整池显存（实测 1080p 约 60MB、
         * 4K 约 250MB）就回收不了。现象就是"放过几条片子之后内存一直下不来"。
         *
         * 用完（exchange）自动复位，不需要额外清理。
         */
        std::atomic<bool> m_releaseRenderState{false};
        bool m_listenerAttached = false;

        /*
         * 最近一张快照（GUI 线程独占：只在 notifySnapshot() 里写）。
         *
         * 分辨率 = 视频原始画布（快照就是整幅画面，例如 4K 是 3840x2160x4 = 33MB）。
         * 只有一张、每来一张换一张，所以内存是有界的；留给"以后再做个存截图的功能"直接用。
         * **给 QML 气泡看的那一份是缩小过的**（见 notifySnapshot() 里的说明）。
         */
        QImage m_lastSnapshot;
        /*
         * 快照版本号（每收到一张 +1）。QML 用它拼 URL：image://snapshot/<rev>，
         * URL 变了 Qt 才肯重新向 provider 取图。
         */
        int m_snapshotRevision = 0;
        /* seek 次数（见上面 Q_PROPERTY 的说明） */
        int m_seekRevision = 0;
        /*
         * 协议标签 + 说明（见上面 Q_PROPERTY 的说明）。
         * **追加在成员表末尾**：这个类是 QML_ELEMENT，Qt 按 sizeof 分配对象内存，
         * 改成员布局要让 qmltyperegistrations 重编（CMakeLists 里挂了头文件依赖），
         * 追加在末尾是最安全的改法。
         */
        QString m_protocol;
        QString m_protocolDetail;

        /*
         * ---- 统计信息面板用的实时数据（Q_PROPERTY stats）----
         *
         * 全部来自框架**真实**读数（见 .cpp 的 refreshStats()）：分辨率/码率/帧率来自
         * GetCurrentStreamInfo / GetCurrentStreamMeta，渲染帧率和丢帧来自
         * GetOption("renderFps") / GetOption("videoDroppedInfo")，缓存速度来自
         * CurrentDownLoadSpeed 回调，容器/协议/连接地址来自 GetPropertyString。
         * 值没变就不发 statsChanged（面板 2Hz 刷新，别让 QML 白重算）。
         */
        QVariantMap m_stats;
        /* 统计信息刷新的定时器（GUI 线程；有播放器时 500ms 一次）。 */
        QTimer *m_statsTimer = nullptr;
        /*
         * 曲线采样点（见 Q_PROPERTY statsHistory）：面板开着时增长，面板关闭清空。
         * 上限见 STATS_HISTORY_MAX（60 个点 = 30 秒窗口，约 10KB，内存可忽略）。
         */
        QVariantList m_statsHistory;
        /*
         * 曲线采样开关。
         *
         * 【为什么默认 true —— 这是"曲线一直没数据"的兜底】以前默认 false，全靠
         * QML 面板打开时调一次 startStatsSampling() 才置真。那一次调用一旦没落到
         *     （QML 里同一个信号写了两个处理器 → 后一个把前一个静默顶掉；
         *      或者实例上/组件里的作用域没解析到），sampling 就永远是 false，
         *      statsHistory 永远是空 → 曲线画不出来、曲线区"当前/峰值"也一直是空，
         *      而且**和 C++ 一点关系都没有，看代码还完全正常**。
         * 现在默认就是 true：定时器本来就在 Prepare 之后一直跑（见 ensureStatsTimer），
         * 多存 60 个点没有可测量的开销。于是"面板打开 → 采样"这条路径即使整条断掉，
         * 曲线照样有数据（最坏情况只是面板关了以后还在多存点，那点开销可忽略）。
         * QML 那边调 start/stop 仍然有意义：关面板时清空历史、把这一段时间窗重新开始。
         */
        bool m_statsSampling = true;
        /*
         * 清单对象里解析出来的静态信息（每个 manifest 只解析一次）：
         * codecs / mimeType / baseUrl 的 host —— 核心不提供这些字符串（只有枚举和
         * 数字），所以清单对象这条路直接从 JSON 里取。
         */
        QString m_manifestVideoCodecs;
        QString m_manifestAudioCodecs;
        QString m_manifestVideoHost;
        QString m_manifestAudioHost;
        /* 色彩调整（0~200，100 = 中性）：换后端/换片源后要重新应用，所以在这里留一份。 */
        int m_colorBrightness = 100;
        int m_colorContrast = 100;
        int m_colorSaturation = 100;

        /*
         * 直播模式（见 Q_PROPERTY liveMode）。
         *
         * 【成员一律加在**最后**】这个类的实例是 QML 引擎按 sizeof 分配的，
         * 中间插成员会和已有的偏移量对不上（历史上踩过 0xC0000005）。
         * 新成员只许往末尾追加 —— 这条对 SuperMediaPlayer 那些"会被别的编译单元
         * 按旧尺寸分配"的类尤其致命，这里同样遵守，省得以后挪动位置时踩雷。
         */
        bool mLiveMode = false;

        /*
         * 解码方式（"硬解 HEVC（显卡名）"/"软解 H.264" …；编码是显示名，内核短名 H.265 →
         * HEVC，见 .cpp 的 codecDisplayName()）的当前值，见 Q_PROPERTY
         * decodeMethod 与 refreshDecodeMethod()。
         *
         * 【成员一律加在最后】理由同上：QML 引擎按 sizeof 分配本对象，
         * 中间插成员会和已有偏移量对不上。
         */
        QString m_decodeMethod;

        /*
         * 【解码/呈现诊断日志的"上次打过的值"签名】（见 .cpp 的 refreshDecodeMethod()）
         *
         * 只用来做"上面那几个值变了才打一条"的比较（硬解与否、编码、显卡名、渲染后端、
         * 零拷贝），不参与任何显示逻辑、也不影响 decodeMethod 的取值。
         *
         * 【成员一律加在最后】理由同上：QML 引擎按 sizeof 分配本对象，中间插成员会和
         * 已有偏移量对不上。只在 GUI 线程读写（refreshDecodeMethod() 由统计那一拍调用）。
         */
        QString m_decodeDiagSignature;
    };

}// namespace cicadaqt

#endif// CICADA_QT_CICADAPLAYERITEM_H
