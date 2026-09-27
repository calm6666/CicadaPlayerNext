//
// CicadaPlayerItem 的实现，设计说明见 CicadaPlayerItem.h。
//

#define LOG_TAG "CicadaPlayerItem"

#include "CicadaPlayerItem.h"
#include "CicadaVideoTexture.h"
#include "CicadaHardwareDevice.h"
/* 对象方式入口（setManifest(builder)）需要构造器的定义；头文件里只前向声明。 */
#include "CicadaManifestBuilder.h"
/* 截屏结果的出口：QML 的 Image 通过 image://snapshot/<rev> 向它取图（见文件头）。 */
#include "SnapshotImageProvider.h"

#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGSimpleTextureNode>
/* 旋转用场景图自带的变换节点（不需要自己写 GL/着色器）。 */
#include <QtQuick/QSGTransformNode>
#include <QtGui/QMatrix4x4>
#include <QtCore/QMetaObject>
#include <QtCore/QThread>
#include <QtGui/QGuiApplication>
/* 右键菜单的"复制视频地址"：QML 没有剪贴板 API，在 C++ 里写。 */
#include <QtGui/QClipboard>
#include <QtCore/QDebug>
/* 流媒体：清单对象序列化成 JSON 文本（setManifest 用），以及 MediaInfo 里读出来的清晰度列表。 */
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonValue>
#include <QtCore/QJsonParseError>
#include <QtCore/QVariantMap>
#include <QtCore/QVariantList>
/* loadManifestFile()：在 C++ 里读本地 .json（QML 的 XHR 读不了 qrc: → file://）。 */
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QUrl>
/* 清晰度列表按"分辨率"合并同一档的多条流（见 onMediaInfoGetCb）。 */
#include <QtCore/QHash>
/* 清单对象按协议补全时要用 QStringList（video/audio/subtitle 三类）和 ceil()。 */
#include <QtCore/QStringList>
/* 统计曲线的采样点带一个毫秒时间戳（QDateTime::currentMSecsSinceEpoch）。 */
#include <QtCore/QDateTime>
#include <cmath>
/* 截屏结果拷进 QImage（QQuickImageProvider 那一端也是按 QImage 交付的）。 */
#include <QtGui/QImage>
/* std::swap（旋转 90/270 时交换宽高比）。 */
#include <utility>
/* std::sort（清晰度列表按带宽排序）。 */
#include <algorithm>

#include <MediaPlayer.h>
#include <utils/frame_work_log.h>
#include <utils/property.h>
/* 零拷贝的全局开关是通过 globalSettings 设的（见 startPlaybackWhenReady）。 */
#include <utils/globalSettings.h>
#include <base/media/IAFPacket.h>
/* playerListener 里的 memset。 */
#include <cstring>
/* GetCurrentStreamMeta 拿到的 Stream_meta 必须用 releaseMeta() 释放（见统计信息那段）。 */
#include <utils/mediaFrame.h>

#if defined(Q_OS_MACOS)
/* kCVPixelFormatType_32BGRA：macOS 上要求 VideoToolbox 输出的像素格式。 */
#include <CoreVideo/CVPixelBuffer.h>
#endif

using namespace Cicada;

namespace cicadaqt {

    namespace {
        /*
         * 协议标签的两个小工具（定义在本文件下面"协议标签与按协议补全清单对象"那一节）。
         * 这里先声明：setSource() 在文件靠前的位置就要用到它们。
         */
        QString protocolFromExtension(const QString &path);
        QString protocolDetailFor(const QString &label, const QString &scheme, bool fromManifest);
    }

    /*
     * 播放器监听回调：这些函数都被框架在**内部线程**上调用（准备线程、
     * 播放器服务线程等），所以一律不允许碰界面，只能把结果投递回主线程 ——
     * QMetaObject::invokeMethod 的队列重载会把 lambda 送到 item 所在线程执行。
     */
    static void onPreparedCb(void *userData)
    {
        auto *item = static_cast<CicadaPlayerItem *>(userData);

        if (item == nullptr) {
            return;
        }

        QMetaObject::invokeMethod(item, [item]() {
            item->notifyPrepared();
        }, Qt::QueuedConnection);
    }

    static void onCompletionCb(void *userData)
    {
        auto *item = static_cast<CicadaPlayerItem *>(userData);

        if (item == nullptr) {
            return;
        }

        QMetaObject::invokeMethod(item, [item]() {
            item->notifyCompletion();
        }, Qt::QueuedConnection);
    }

    static void onFirstFrameCb(void *userData)
    {
        auto *item = static_cast<CicadaPlayerItem *>(userData);

        if (item == nullptr) {
            return;
        }

        QMetaObject::invokeMethod(item, [item]() {
            item->notifyFirstFrameShown();
        }, Qt::QueuedConnection);
    }

    static void onVideoSizeCb(int64_t width, int64_t height, void *userData)
    {
        auto *item = static_cast<CicadaPlayerItem *>(userData);

        if (item == nullptr) {
            return;
        }

        const int w = static_cast<int>(width);
        const int h = static_cast<int>(height);

        QMetaObject::invokeMethod(item, [item, w, h]() {
            item->notifyVideoSize(w, h);
        }, Qt::QueuedConnection);
    }

    static void onPositionCb(int64_t position, void *userData)
    {
        auto *item = static_cast<CicadaPlayerItem *>(userData);

        if (item == nullptr) {
            return;
        }

        const qint64 pos = static_cast<qint64>(position);

        QMetaObject::invokeMethod(item, [item, pos]() {
            item->notifyPosition(pos);
        }, Qt::QueuedConnection);
    }

    /*
     * 框架的**截屏结果**（进度条悬停预览用）。
     *
     * 【调用链】MediaPlayer::CaptureScreen()（mediaPlayer/MediaPlayer.cpp:380）
     *   -> SuperMediaPlayer::CaptureScreen()（SuperMediaPlayer.cpp:202）
     *   -> 渲染器 IVideoRender::captureScreen()（在框架 VSync 线程上回调
     *      CicadaVideoRender::device_captureScreen()）
     *   -> 回到 SuperMediaPlayer 的 lambda（SuperMediaPlayer.cpp:207-211）
     *   -> PlayerNotifier::NotifyCaptureScreen()（player_notifier.cpp:188-203，把像素
     *      拷进自己的事件缓冲、投递到 notifier 线程）
     *   -> 这里。
     *
     * 【参数语义】playerType123Callback 的形参名是给字幕那条路起的
     * （index/size/subtitle，见 native_cicada_player_def.h:90），但 CaptureScreen 这一路
     * 填的是 **width / height / buffer**：mediaPlayer/MediaPlayer.cpp:1056-1062 的
     * captureScreenResult(width, height, buffer, userData) 原样转给了监听者。
     *
     * 【像素格式与生命周期 —— 必须当场拷贝】
     *   * buffer 是 width*height*4 字节、字节序 **R,G,B,A**（等价 QImage::Format_RGBA8888）；
     *   * 它**只在这次回调期间有效**：生产者（渲染器）在回调返回后立刻 free
     *     （framework 自带实现 GLRender.cpp:455-465 就是这个顺序）；
     *   * 所以这里必须**拷成 QImage**，绝不能只记住指针。
     *
     * 【线程】这个回调**不在 GUI 线程**（PlayerNotifier 自己的线程）：
     *   * 拷贝就地做（纯内存操作，碰不到界面）；
     *   * 结果用 QMetaObject::invokeMethod + QueuedConnection 投回 item 所在线程，
     *     由 notifySnapshot() 在 GUI 线程更新版本号、发信号。
     *
     * 【"没有画面"】width/height <= 0 或 buffer == nullptr 是框架表达"这次截不到"的方式
     * （IVideoRender.h:174-177 的默认实现、SuperMediaPlayer.cpp:212-215 都这么回调）：
     * 什么都不做即可 —— 界面保持上一张/空框，绝不崩。
     */
    static void onCaptureScreenCb(int64_t width, int64_t height, const void *buffer, void *userData)
    {
        auto *item = static_cast<CicadaPlayerItem *>(userData);

        if (item == nullptr) {
            return;
        }

        const int w = static_cast<int>(width);
        const int h = static_cast<int>(height);

        if (w <= 0 || h <= 0 || buffer == nullptr) {
            /* 正常情况：还没出第一帧、或者这个帧拿不到 CPU 像素（渲染器那边已经打过日志）。 */
            AF_LOGW("captureScreen: the framework reported no picture (%dx%d)\n", w, h);
            return;
        }

        QImage image(w, h, QImage::Format_RGBA8888);

        if (image.isNull()) {
            AF_LOGE("captureScreen: cannot allocate a %dx%d snapshot image\n", w, h);
            return;
        }

        const auto *src = static_cast<const uchar *>(buffer);
        const int rowBytes = w * 4;

        /*
         * **逐行**拷：QImage 的每一行按 4 字节对齐（bytesPerLine() 可能大于 w*4），
         * 一次 memcpy(image.bits(), src, w*h*4) 在行宽不是 4 的倍数时就会错行。
         */
        for (int y = 0; y < h; ++y) {
            memcpy(image.scanLine(y),
                   src + static_cast<size_t>(y) * static_cast<size_t>(rowBytes),
                   static_cast<size_t>(rowBytes));
        }

        /* QImage 是隐式共享的：捕进 lambda 只是引用计数 +1，不拷像素。 */
        QMetaObject::invokeMethod(item, [item, image]() {
            item->notifySnapshot(image);
        }, Qt::QueuedConnection);
    }

    /*
     * 已缓冲位置。进度条上那段浅色（参考实现叫 buffered）用它，
     * 数据和播放位置一样来自框架回调，不是界面自己算的。
     */
    static void onBufferPositionCb(int64_t position, void *userData)
    {
        auto *item = static_cast<CicadaPlayerItem *>(userData);

        if (item == nullptr) {
            return;
        }

        const qint64 pos = static_cast<qint64>(position);

        QMetaObject::invokeMethod(item, [item, pos]() {
            item->notifyBufferedPosition(pos);
        }, Qt::QueuedConnection);
    }

    /*
     * 框架开始/结束一次 seek。
     *
     * 这两个回调是"单飞 + 最新优先"的判定依据：只有拿到 SeekEnd，我们才认为上一个
     * seek 真的结束了，才允许把拖动过程中攒下的最新位置发出去。
     */
    static void onSeekingCb(int64_t seekInCache, void *userData)
    {
        (void) seekInCache;
        auto *item = static_cast<CicadaPlayerItem *>(userData);

        if (item == nullptr) {
            return;
        }

        QMetaObject::invokeMethod(item, [item]() {
            item->notifySeekStarted();
        }, Qt::QueuedConnection);
    }

    static void onSeekEndCb(int64_t seekInCache, void *userData)
    {
        (void) seekInCache;
        auto *item = static_cast<CicadaPlayerItem *>(userData);

        if (item == nullptr) {
            return;
        }

        QMetaObject::invokeMethod(item, [item]() {
            item->notifySeekFinished();
        }, Qt::QueuedConnection);
    }

    static void onErrorCb(int64_t errorCode, const void *errorMsg, void *userData)
    {
        auto *item = static_cast<CicadaPlayerItem *>(userData);

        if (item == nullptr) {
            return;
        }

        const QString message = QString::fromUtf8(static_cast<const char *>(errorMsg));
        const int code = static_cast<int>(errorCode);

        QMetaObject::invokeMethod(item, [item, code, message]() {
            item->notifyError(code, message);
        }, Qt::QueuedConnection);
    }

    /*
     * 缓冲开始/结束 + 缓冲进度 + 当前下载速度。
     *
     * 框架侧（SuperMediaPlayer::mainService）在"读不到数据、缓冲为空"时会
     *   `mBufferingFlag = true; NotifyLoading(loading_event_start); mMasterClock.pause();
     *    pauseAudioRender(true);`
     * 缓冲够了再 `NotifyLoading(loading_event_end)` + 恢复主时钟和音频渲染
     * —— 也就是说**框架本来就有一套完整的缓冲状态机**，只是 Qt 这一侧从来没
     * 订阅过 LoadingStart/LoadingEnd（`playerListener` 里有这两个字段，
     * 只是 memset 之后没人赋值）。结果就是：视频冻住、弹幕照滚、界面没有任何提示。
     * 这里把它接上。
     */
    static void onLoadingStartCb(void *userData)
    {
        auto *item = static_cast<CicadaPlayerItem *>(userData);

        if (item == nullptr) {
            return;
        }

        QMetaObject::invokeMethod(item, [item]() {
            item->notifyBuffering(true);
        }, Qt::QueuedConnection);
    }

    static void onLoadingEndCb(void *userData)
    {
        auto *item = static_cast<CicadaPlayerItem *>(userData);

        if (item == nullptr) {
            return;
        }

        QMetaObject::invokeMethod(item, [item]() {
            item->notifyBuffering(false);
        }, Qt::QueuedConnection);
    }

    /* 缓冲进度 0~100，只用于日志（界面显示的是"缓冲中 + 速度"）。 */
    static void onLoadingProgressCb(int64_t progress, void *userData)
    {
        auto *item = static_cast<CicadaPlayerItem *>(userData);

        if (item == nullptr) {
            return;
        }

        QMetaObject::invokeMethod(item, [item, progress]() {
            item->notifyLoadingProgress(static_cast<int>(progress));
        }, Qt::QueuedConnection);
    }

    /* 当前下载速度（bit/s），框架每秒左右推一次；界面显示 KB/s 或 MB/s。 */
    static void onDownloadSpeedCb(int64_t speed, void *userData)
    {
        auto *item = static_cast<CicadaPlayerItem *>(userData);

        if (item == nullptr) {
            return;
        }

        QMetaObject::invokeMethod(item, [item, speed]() {
            item->notifyDownloadSpeed(speed);
        }, Qt::QueuedConnection);
    }

    /*
     * "这一路视频流属于菜单里的哪一行？"
     *
     * 先比 streamIndex（同名即同一行）；比不到再比**分辨率**。
     *
     * 【为什么还要比分辨率】菜单里一行 = 一档清晰度，而一档在清单里可能对应**多条流**
     * （同一分辨率的不同编码，见 onMediaInfoGetCb 的合并说明）。菜单留的是其中一条当代表，
     * 但 ABR 自动挡/框架切流完全可能停在同一分辨率的另一条上 —— 那时 streamIndex 对不上，
     * 只按 streamIndex 认就会"当前清晰度"高亮丢掉（错显示成未知/自动）。
     * 分辨率对得上就是同一档，界面认知和用户看到的一致。
     *
     * 两边都没有分辨率信息时（width/height 都是 0）退回纯 streamIndex 比较，
     * 和合并前的老行为完全一样。
     */
    static bool sameQualityRow(const QVariantMap &row, int streamIndex, int width, int height)
    {
        if (row.value(QStringLiteral("streamIndex")).toInt() == streamIndex) {
            return true;
        }

        /*
         * 兜底判据必须和 onMediaInfoGetCb 的分组键**同一口径**：那里优先按高度分档
         * （界面档位名就是高度），所以这里也先比高度；高度缺失才退到宽度。
         * 两边都缺就只剩 streamIndex 比较，和合并前的老行为一样。
         */
        if (height > 0) {
            return row.value(QStringLiteral("height")).toInt() == height;
        }

        return width > 0 && row.value(QStringLiteral("width")).toInt() == width;
    }

    /*
     * 框架把"有哪些流（清晰度/音轨/字幕轨）"通过这个消息报上来。
     *
     * **载荷形态必须记清楚**：`Infos` 指向的是一个 `MediaInfo*`（不是数组指针）。
     * 依据是框架自己的处理（mediaPlayer/MediaPlayer.cpp:1080 mediaInfoGetCallback）：
     *     auto *mediaInfo = (MediaInfo *) Infos;
     *     for (auto si : mediaInfo->mStreamInfoQueue) { ... }
     *     player->mListener.MediaInfoGet(count, Infos, ...);   // 原样转发给我们
     * 仓库里那个测试 mediaPlayer/tests/switch_stream/mediaPlayerSwitchStream.cpp:27 把它
     * 当成 `StreamInfo **` 用，是**错的**（照它写会读到野指针）；以框架实现为准。
     *
     * `count` 在这里没什么用（条数从队列里数），保留参数只为对上回调签名。
     */
    static void onMediaInfoGetCb(int64_t count, const void *infos, void *userData)
    {
        (void) count;
        auto *item = static_cast<CicadaPlayerItem *>(userData);

        if (item == nullptr) {
            return;
        }

        QVariantList list;

        if (const auto *mediaInfo = static_cast<const MediaInfo *>(infos)) {
            for (const StreamInfo *si : mediaInfo->mStreamInfoQueue) {
                if (si == nullptr || si->type != ST_TYPE_VIDEO) {
                    /* 清晰度菜单只列视频流；音轨/字幕轨以后要用了再说。 */
                    continue;
                }

                QVariantMap entry;
                entry.insert(QStringLiteral("streamIndex"), si->streamIndex);
                entry.insert(QStringLiteral("width"), si->videoWidth);
                entry.insert(QStringLiteral("height"), si->videoHeight);
                entry.insert(QStringLiteral("bandwidth"), si->videoBandwidth);
                entry.insert(QStringLiteral("bitrate"), static_cast<qint64>(si->bitrate));
                entry.insert(QStringLiteral("description"),
                             si->description != nullptr ? QString::fromUtf8(si->description) : QString());
                /*
                 * label：界面直接显示的文字。
                 *
                 * 参考实现里 HLS 用 "1080p"、DASH 用 "1920x1080"（两边不一致，见 hili-player 的
                 * HlsPlugin/DashPlugin getQualities），这里统一成大家习惯的那套叫法：
                 *    2160 → "4K"、1440 → "2K"（用户明确要求："2160p 这种写法不对，应该是 4K、1080p"），
                 *    其余按高度写 "1080P / 720P / 480P …"；
                 * 没有高度就退回宽x高，再没有就退回带宽。
                 */
                QString label;
                if (si->videoHeight > 0) {
                    switch (si->videoHeight) {
                        case 2160:
                            label = QStringLiteral("4K");
                            break;
                        case 1440:
                            label = QStringLiteral("2K");
                            break;
                        default:
                            label = QStringLiteral("%1P").arg(si->videoHeight);
                            break;
                    }
                } else if (si->videoWidth > 0) {
                    label = QStringLiteral("%1x%2").arg(si->videoWidth).arg(si->videoHeight);
                } else if (si->videoBandwidth > 0) {
                    label = QStringLiteral("%1kbps").arg(si->videoBandwidth / 1000);
                } else {
                    label = QStringLiteral("未知");
                }
                entry.insert(QStringLiteral("label"), label);
                list.append(entry);
            }
        }

        /*
         * 【同一档清晰度只留一条流】
         *
         * 清单里同一个分辨率常常挂着**多条**流：用户的 output.mpd 就是每个分辨率各一条
         * H.264 + 一条 H.265（8 条 AdaptationSet → 8 条流，见 DashManager::init()：
         * 一个 Representation 一条流）。原样列出来菜单里就是
         * "4K / 4K / 1080P / 1080P / 720P / 720P / 480P / 480P" ——
         * 用户实测报的就是这个（"dash 下，清晰度里面每个清晰度都有两个，重复了"）。
         *
         * 【为什么只能按分辨率合并，不能写成"4K (H.265)"】
         * 框架这一层**拿不到编解码器**：DASH 按 mimeType == "video/mp4" 认流
         * （DashManager::FindSuitableAdaptationSets，framework/demuxer/dash/DashManager.cpp:680），
         * Representation/StreamInfo 里也都没有 codec 字段（framework/demuxer/dash/ 下的 *.h 无 codecs
         * 相关成员），所以界面无从区分同分辨率的两条流 —— 只能视作同一档。
         *
         * 【合并规则】同分辨率取**带宽更大**的那条当代表：菜单本来就是"从清晰到模糊"排的
         * （下面的 sort），同分辨率下带宽大的那档画质更好。其余同分辨率的流仍留在框架的流
         * 列表里，**没有被动过** —— ABR 自动挡照旧在所有档之间切；万一它切到同分辨率的
         * "孪生流"，界面靠分辨率（而不是 streamIndex）认回同一行，高亮不会丢
         * （见 sameQualityRow / notifyStreamSwitched / notifyQualities）。
         */
        QVariantList merged;
        QHash<QString, int> rowOfGroup;

        for (const QVariant &variant : list) {
            const QVariantMap entry = variant.toMap();
            const int width = entry.value(QStringLiteral("width")).toInt();
            const int height = entry.value(QStringLiteral("height")).toInt();
            /*
             * 分组键：**优先高度**，没有高度才退回宽度，两个都没有就用 streamIndex
             * （等于不参与合并）。
             *
             * 【为什么不是"宽x高"】界面给用户看的档位名本来就是按高度定的
             * （2160→4K、1440→2K、其余 "%1P"，见上面 label 那段），所以"同名即同档"
             * 才是和界面一致的口径。而宽度可能缺值（换档窗口期解码器只填了高度 ——
             * 见 framework/demuxer/dash/DashStream.cpp 里"宽高各自补齐"那处修复），
             * 用 "宽x高" 当键会把 `0x2160` 和 `3840x2160` 分成两档，
             * 用户看到的就是"同一个清晰度有两个"。
             */
            const QString group = (height > 0)
                                  ? QStringLiteral("h%1").arg(height)
                                  : (width > 0
                                     ? QStringLiteral("w%1").arg(width)
                                     : QStringLiteral("#%1").arg(
                                               entry.value(QStringLiteral("streamIndex")).toInt()));

            const auto found = rowOfGroup.constFind(group);

            if (found == rowOfGroup.constEnd()) {
                rowOfGroup.insert(group, merged.size());
                merged.append(entry);
                continue;
            }

            const int row = found.value();

            if (entry.value(QStringLiteral("bandwidth")).toInt()
                > merged.at(row).toMap().value(QStringLiteral("bandwidth")).toInt()) {
                merged[row] = entry;
            }
        }

        /* 按带宽从高到低：和清单里 video[] 的顺序一致，界面从上到下就是"从清晰到模糊"。 */
        std::sort(merged.begin(), merged.end(), [](const QVariant &a, const QVariant &b) {
            return a.toMap().value(QStringLiteral("bandwidth")).toInt()
                 > b.toMap().value(QStringLiteral("bandwidth")).toInt();
        });

        QMetaObject::invokeMethod(item, [item, merged]() {
            item->notifyQualities(merged);
        }, Qt::QueuedConnection);
    }

    /*
     * 一次切流（切清晰度/切轨）完成。
     *
     * 载荷：`type` 是 StreamType（0 视频 / 1 音频 / 2 字幕），`Info` 是**单个** StreamInfo*。
     * 同样是按框架自己的处理确认的（MediaPlayer.cpp:994 streamChangedSucCallback：
     * `auto *streamInfo = (StreamInfo *) Info;`）。这个回调就是"无感切换"的完成点 ——
     * 界面收到它才把"正在切换"的提示收起来。
     */
    static void onStreamSwitchSucCb(int64_t type, const void *info, void *userData)
    {
        auto *item = static_cast<CicadaPlayerItem *>(userData);

        if (item == nullptr) {
            return;
        }

        const int streamType = static_cast<int>(type);
        int streamIndex = -1;
        int width = 0;
        int height = 0;
        int bandwidth = 0;

        if (const auto *si = static_cast<const StreamInfo *>(info)) {
            streamIndex = si->streamIndex;
            width = si->videoWidth;
            height = si->videoHeight;
            bandwidth = si->videoBandwidth;
        }

        QMetaObject::invokeMethod(item, [item, streamType, streamIndex, width, height, bandwidth]() {
            item->notifyStreamSwitched(streamType, streamIndex, width, height, bandwidth);
        }, Qt::QueuedConnection);
    }

    static void onVideoQualitySwitchCb(int64_t status, int64_t streamIndex, const void *desc, void *userData)
    {
        // PlayerNotifier 在自己的事件线程调用这里；Qt 属性和信号只能在 item
        // 所属 GUI 线程修改，因此必须通过 QueuedConnection 转发。
        auto *item = static_cast<CicadaPlayerItem *>(userData);
        if (item == nullptr) return;
        const QString description = QString::fromUtf8(desc ? static_cast<const char *>(desc) : "");
        QMetaObject::invokeMethod(item, [item, status, streamIndex, description]() {
            item->notifyQualitySwitchStatus(static_cast<int>(status), static_cast<int>(streamIndex), description);
        }, Qt::QueuedConnection);
    }

    /*
     * 播放状态变化（PLAYER_PREPARED / PLAYER_PLAYING / PLAYER_PAUSED / ...）。
     *
     * 为什么用这个回调而不是"定时去问播放器"：MediaPlayer 门面上**没有**读取当前状态的
     * 接口（只有 SetListener/Start/Pause/... 这些动作），状态只能从回调里拿。
     * 用它之后组件显示的状态就和框架真实状态完全一致（包括缓冲、结束、错误这些
     * 自己触发不了的变化）。
     */
    static void onStatusChangedCb(int64_t oldStatus, int64_t newStatus, void *userData)
    {
        (void) oldStatus;

        auto *item = static_cast<CicadaPlayerItem *>(userData);

        if (item == nullptr) {
            return;
        }

        const int status = static_cast<int>(newStatus);

        QMetaObject::invokeMethod(item, [item, status]() {
            item->notifyPlayerStatus(status);
        }, Qt::QueuedConnection);
    }

    CicadaPlayerItem::CicadaPlayerItem(QQuickItem *parent)
        : QQuickItem(parent)
        , m_textureBackend(new CicadaVideoTexture())
    {
        /*
         * 画面由我们自己画：这是场景图里的一个普通 item，需要 setFlag(ItemHasContents)。
         */
        setFlag(ItemHasContents, true);

        /*
         * 【位置一律由框架**推送**，这里不再轮询】
         *
         * 以前这里挂着一个 250ms 的定时器去 `GetCurrentPosition()`，理由写着
         * "回调在播放器线程上、跨线程投递麻烦"。实测两个问题：
         *   1. 它是**第二个时钟**：轮询值是滞后的，而界面/弹幕侧按帧外推已经跑在前面，
         *      每 250ms 拿滞后值去校正 = 时钟被往回拽一次 —— 弹幕表现就是
         *      "往前滑一段、又退一格"，一卡一卡；
         *   2. 白烧 CPU：不管有没有人看，每秒都在跨线程问播放器要位置。
         *
         * 现在只认 listener 的 PositionUpdate / BufferPositionUpdate 回调
         * （onPositionCb / onBufferPositionCb，见 createPlayer 里的注册），
         * 时长在"准备好了 / 首帧 / seek 结束"这几个事件点上现取一次（refreshDuration），
         * 不再有定时器。
         */

        /*
         * 这里原来有一个 seek 看门狗（QTimer，2000 → 6000ms）。本轮按红线删除：
         * 它用"等 N 毫秒还没收到 SeekEnd"推断"seek 卡住了"，然后替框架清掉在途状态 ——
         * 这既是看门狗，又会和框架里仍在跑的那次 seek 叠加（日志实测"越拖越慢"）。
         * 正确做法：seek 的终态只由框架事件（Seeking / SeekEnd）驱动 ——
         * 内核侧已保证"落点帧上屏即结束 seek"（SuperMediaPlayer 的 K1）。
         */
    }

    CicadaPlayerItem::~CicadaPlayerItem()
    {
        /*
         * 析构顺序很重要：
         *   1. 先销毁播放器（它会停掉框架的 VSync 线程，之后不会再有帧/位置回调进来）；
         *   2. 最后放掉渲染资源（此时不可能有正在处理的帧）。
         * 反过来就有可能在回调里访问已经析构的对象。
         * （以前这里还要先停一个位置轮询定时器，现在位置只走回调，定时器已经删了。）
         */
        destroyPlayer();

        if (m_textureBackend) {
            m_textureBackend->releaseResources();
        }
    }

    /* ------------------------------------------------------------------ */
    /* 属性                                                               */
    /* ------------------------------------------------------------------ */

    void CicadaPlayerItem::setProtocol(const QString &label, const QString &detail)
    {
        if (m_protocol == label && m_protocolDetail == detail) {
            return;
        }

        m_protocol = label;
        m_protocolDetail = detail;
        emit protocolChanged();
    }

    /*
     * 直播模式开关（见头文件 Q_PROPERTY liveMode 那一大段）。
     *
     * 缓冲策略只在 createPlayer() 里应用，而 createPlayer() 只在"换片源"时跑。
     * 所以：
     *   * 在设 source **之前**调用（直播窗口就是这么用的）→ 这一路直接按直播策略起；
     *   * 在已经在播的时候改 → 不会当场重开（重开会黑一下、还会打断播放），
     *     只留下日志说明"要等下一次换片源才生效"，避免用户以为它没起作用。
     */
    void CicadaPlayerItem::setLiveMode(bool on)
    {
        if (mLiveMode == on) {
            return;
        }

        mLiveMode = on;
        emit liveModeChanged();

        AF_LOGI("liveMode = %s%s\n", on ? "true" : "false",
                (m_player != nullptr)
                ? "（注意：当前已经有片源在播，直播缓冲策略要等下一次换片源才生效）"
                : "");
    }

    void CicadaPlayerItem::setSource(const QUrl &source)
    {
        if (m_source == source && m_manifestJson.isEmpty()) {
            /*
             * 同一个地址重复设置：什么都不做（这是对的，不然会平白重开一次）。
             * 但如果用户以为"再选一次同一个文件"能重播，就会觉得按钮没反应 ——
             * 所以留一行日志，排查时一眼能看出来。
             */
            AF_LOGD("setSource: %s is already the current source, ignoring\n",
                    source.toString().toUtf8().constData());
            return;
        }

        /*
         * 换成普通地址 = 不再走清单对象那条路（两种片源不能串）。
         * 清清单时要补发 streamingChanged，界面才会把清晰度菜单收掉。
         */
        if (!m_manifestJson.isEmpty()) {
            m_manifestJson.clear();
            m_qualities.clear();
            m_qualityIndex = -1;
            emit streamingChanged();
            emit qualitiesChanged();
            emit qualityChanged();
        }

        /* 先把上一个地址记下来：下面那行日志要打"从谁换到谁"。 */
        const QUrl previous = m_source;

        m_source = source;
        emit sourceChanged();
        /* 有片源/没片源变了（界面里"没片源才显示提示"那种绑定看它） */
        emit hasMediaChanged();

        /*
         * 协议标签（界面左上角那颗徽标）：普通地址按"后缀 > scheme"判断，
         * 见 protocolFromExtension()。空地址 = 没有片源，清掉标签。
         */
        if (m_source.isEmpty()) {
            setProtocol(QString(), QString());
        } else {
            const QString label = protocolFromExtension(m_source.path());

            if (!label.isEmpty()) {
                setProtocol(label, protocolDetailFor(label, m_source.scheme(), false));
            } else {
                const QString scheme = m_source.scheme().toUpper();
                setProtocol(scheme.isEmpty() ? QStringLiteral("FILE") : scheme,
                            protocolDetailFor(scheme, m_source.scheme(), false));
            }
        }

        /*
         * 每次换片都打一行（和 QML 里 playFile 那行对应）：出问题时能立刻分清是
         * "对话框没给地址" 还是 "给了地址但播放器没起来"。
         */
        AF_LOGI("opening source: %s (previous: %s)\n",
                m_source.toString().toUtf8().constData(),
                previous.toString().toUtf8().constData());

        /* 换地址等于重新来一遍：先停掉旧的，再建新的。 */
        destroyPlayer();
        m_playerStarted = false;
        m_firstFrameEmitted = false;
        setErrorString(QString());
        setStatus(m_source.isEmpty() ? Null : Loading);

        if (!m_source.isEmpty()) {
            createPlayer();

            /*
             * 场景图已经初始化过（纹理后端探测过零拷贝）就立刻开始；否则等
             * onSceneGraphInitialized() 里那次队列调用 —— 顺序很重要，见
             * startPlaybackWhenReady() 的说明。
             */
            if (m_sceneGraphReady) {
                startPlaybackWhenReady();
            }
        }
    }

    void CicadaPlayerItem::setVolume(qreal volume)
    {
        volume = qBound(0.0, volume, 1.0);

        if (qFuzzyCompare(m_volume, volume)) {
            return;
        }

        m_volume = volume;
        emit volumeChanged();

        if (m_player) {
            m_player->SetVolume(static_cast<float>(m_volume));
        }
    }

    void CicadaPlayerItem::setMuted(bool muted)
    {
        if (m_muted == muted) {
            return;
        }

        m_muted = muted;
        emit mutedChanged();

        if (m_player) {
            m_player->SetMute(m_muted);
        }
    }

    void CicadaPlayerItem::setPlaybackRate(qreal rate)
    {
        if (rate <= 0.0 || qFuzzyCompare(m_playbackRate, rate)) {
            return;
        }

        m_playbackRate = rate;
        emit playbackRateChanged();

        if (m_player) {
            m_player->SetSpeed(static_cast<float>(m_playbackRate));
        }
    }

    void CicadaPlayerItem::setAutoPlay(bool autoPlay)
    {
        if (m_autoPlay == autoPlay) {
            return;
        }

        m_autoPlay = autoPlay;
        emit autoPlayChanged();
    }

    void CicadaPlayerItem::setLoop(bool loop)
    {
        if (m_loop == loop) {
            return;
        }

        m_loop = loop;
        emit loopChanged();

        if (m_player) {
            m_player->SetLoop(m_loop);
        }
    }

    void CicadaPlayerItem::setHardwareDecoding(bool enabled)
    {
        if (m_hardwareDecoding == enabled) {
            return;
        }

        m_hardwareDecoding = enabled;
        emit hardwareDecodingChanged();

        /*
         * 告诉渲染线程：把"零拷贝还是 CPU 路径"重新算一遍。
         *
         * 为什么不直接在渲染线程读 m_hardwareDecoding：那个 bool 只归 GUI 线程管，
         * 跨线程读一个正在被改的普通成员是数据竞争。这里把结果（而不是标志本身）
         * 放进原子量，渲染线程只认它：
         *   0 = 不允许零拷贝（硬解关了：解码器只交内存里的 CPU 帧）
         *   1 = 允许
         */
        m_textureReprobe = enabled ? 1 : 0;

        if (!m_sceneGraphReady) {
            /*
             * 场景图还没初始化（窗口还没显示），渲染线程也没开始跑 updatePaintNode。
             * 这时顺序天然是对的：onSceneGraphInitialized() 会取走上面那个原子量做初始
             * 探测，之后才 Prepare()。所以直接重开即可。
             */
            reloadCurrentSource();
            return;
        }

        /*
         * 场景图已经就绪：得**先**在渲染线程上重算，**再**重开片源。
         *
         * 因为 startPlaybackWhenReady() 要拿重算后的结论去决定开不开"解码纹理直通"：
         * 顺序反了的话，重新开硬解时会读到旧的"零拷贝不可用"，于是解码器仍然走
         * copy-back —— 用户看到的现象是"硬解开关打开了，但零拷贝一直是关的"。
         *
         * 重算没法在这里同步做（要拿 Qt 的 RHI 设备、碰 GPU 资源，只能在渲染线程），
         * 所以挂个标志 + 请求一次重绘，渲染线程做完后会回调 reloadCurrentSource()。
         */
        m_reloadPending = true;
        update();
    }

    void CicadaPlayerItem::reloadCurrentSource()
    {
        if (m_source.isEmpty() && m_manifestJson.isEmpty()) {
            /* 还没有片源：设置已经记下，下次打开自然生效。 */
            return;
        }

        /*
         * 记下当前位置，等新的 Prepared 回来再 seek 回去（见 notifyPrepared 里的
         * m_pendingSeekMs）—— 不然用户一改设置，画面就从头开始了。
         */
        m_pendingSeekMs = m_position;

        /*
         * 清单对象片源也要能重开（比如刚改了硬解开关）：把 JSON 取出来、清空、再喂一遍，
         * 走的就是 setManifestJson() 那条完整流程。
         */
        if (!m_manifestJson.isEmpty()) {
            const QString json = m_manifestJson;
            m_manifestJson.clear();
            setManifestJson(json);
            return;
        }

        /* 先清空再 setSource()：地址没变的话 setSource 会直接 return，不会重开。 */
        const QUrl source = m_source;
        m_source = QUrl();
        setSource(source);
    }

    void CicadaPlayerItem::setExtraRotation(int rotation)
    {
        /* 归一化到 0 ~ 359，并且只接受 90 的整数倍（其它角度会让画面糊掉）。 */
        rotation = ((rotation % 360) + 360) % 360;
        rotation = (rotation / 90) * 90;

        if (m_extraRotation == rotation) {
            return;
        }

        m_extraRotation = rotation;
        emit extraRotationChanged();
        update();
    }

    void CicadaPlayerItem::setMirror(bool mirror)
    {
        if (m_mirror == mirror) {
            return;
        }

        m_mirror = mirror;
        emit mirrorChanged();
        update();
    }

    int CicadaPlayerItem::effectiveRotation() const
    {
        int rotation = m_extraRotation;

        /*
         * 视频自带的旋转元数据：手机竖屏视频（1920x1080 + rotate=90）非常常见，
         * 不处理的话画面会横着显示。框架把它解析出来放在 GetVideoRotation() 里。
         */
        if (m_player != nullptr) {
            rotation += m_player->GetVideoRotation();
        }

        rotation = ((rotation % 360) + 360) % 360;
        return (rotation / 90) * 90;
    }

    QString CicadaPlayerItem::backend() const
    {
        if (!m_textureBackend) {
            return QStringLiteral("none");
        }

        return m_textureBackend->backendName();
    }

    bool CicadaPlayerItem::zeroCopy() const
    {
        /*
         * 硬解关掉时，"零拷贝"这个说法本身就不成立（解码器交的是内存里的 CPU 帧），
         * 诊断文字要如实反映这一点，而不是只问后端"你支不支持"。
         */
        return m_hardwareDecoding && m_textureBackend != nullptr && m_textureBackend->isZeroCopy();
    }

    QString CicadaPlayerItem::device() const
    {
        return CicadaHardwareDevice::instance().description();
    }

    /* ------------------------------------------------------------------ */
    /* 控制                                                               */
    /* ------------------------------------------------------------------ */

    void CicadaPlayerItem::play()
    {
        if (m_player == nullptr) {
            return;
        }

        m_player->Start();
    }

    void CicadaPlayerItem::pause()
    {
        if (m_player == nullptr) {
            return;
        }

        m_player->Pause();
    }

    void CicadaPlayerItem::togglePause()
    {
        if (playing()) {
            pause();
        } else {
            play();
        }
    }

    void CicadaPlayerItem::stop()
    {
        if (m_player == nullptr) {
            return;
        }

        m_player->Stop();
        setStatus(Stopped);
    }

    void CicadaPlayerItem::requestSnapshot()
    {
        /*
         * 交给框架去截，**异步**拿结果：框架会在它自己的 VSync 线程上截一帧，结果经
         * onCaptureScreenCb 回来，再由 notifySnapshot() 在 GUI 线程更新 snapshotRevision
         * —— QML 的 Image 就是靠这个版本号换 URL、重新向 provider 取图的。
         *
         * 【为什么这里不做节流】调用方（ProgressRow 的悬停/拖动）已经用 150ms 的 Timer
         * 限流了。这里再加一层只会让两层节流叠加，界面感觉"半天不出图"。
         * 真要防手滑，框架那边本来也扛得住：一次 CaptureScreen 就是截一帧。
         *
         * 【调用时机】任何播放状态下都能截：
         *   * 暂停时也能 —— 渲染器缓存着"当前正在显示的那一帧"；
         *   * 还没出第一帧时框架会回调 (nullptr,0,0)，界面保持空框（见 onCaptureScreenCb）。
         */
        if (m_player == nullptr) {
            return;
        }

        m_player->CaptureScreen();
    }

    void CicadaPlayerItem::seek(qint64 positionMs)
    {
        if (m_player == nullptr || positionMs < 0) {
            return;
        }

        /* 通知 QML"这是一次真 seek"（弹幕层据此硬对齐时钟，见 seekRevision 的说明） */
        ++m_seekRevision;
        emit seekRevisionChanged();

        /*
         * 拖动进度条时这个函数会被高频调用（鼠标每动一下一次，几十次/秒），
         * 而框架的**精确 seek** 很贵：它要把包队列清掉、重新找关键帧、解码到目标帧，
         * 还要等"目标帧渲染出来"才算结束（SuperMediaPlayer 里的 mSeekFlag 状态机）。
         *
         * 把几十个 seek 一股脑塞进去会互相打架：每次新的 SEEKTO 都会把上一个 seek
         * 的状态覆盖掉、再 flush 一遍包队列，读写/解码线程一直在 flush 和重新找关键帧
         * 之间打转 —— 表现就是**拖动时画面/进度条卡住**（日志里能看到 read index
         * 的抖动、dropped 暴涨，偶尔还会把 demux 线程停在半路，卡住就不回来了）。
         * 这个问题和渲染后端无关，安卓/其它平台同样会碰到。
         *
         * 所以在这里做"单飞 + 最新优先"：
         *   * 已经有一次 seek 在跑（还没收到 SeekEnd）→ 只记下**最后**这个位置，
         *     等它结束再发。拖动过程中间那些位置本来也不用逐个播出来；
         *   * 没有在跑的 → 立刻发。
         * 这样无论调用方怎么高频调用，框架同时只会看到一个 seek。
         */
        if (m_seekInFlight) {
            m_seekPending = positionMs;
            return;
        }

        issueSeek(positionMs);
    }

    /* ------------------------------------------------------------------ */
    /* 流媒体：清单对象、清晰度、ABR                                        */
    /* ------------------------------------------------------------------ */

    bool CicadaPlayerItem::setManifest(const QVariantMap &manifest)
    {
        if (manifest.isEmpty()) {
            AF_LOGW("setManifest: 空对象，忽略\n");
            return false;
        }

        /*
         * 对象**原样**交给核心播放器，应用层不做任何字段转换。
         *
         * 核心本来就支持对象播放，整条链是：
         *   MediaPlayer::SetDataSource(const std::string &jsonManifest)   mediaPlayer/MediaPlayer.cpp
         *     -> SuperMediaPlayer::SetDataSource(const std::string &)     mediaPlayer/SuperMediaPlayer.cpp
         *     -> Manifest::MediaManifestParser::parse(...)                framework/demuxer/manifest/MediaManifestParser.cpp
         *     -> MSG_SETMANIFESTSOURCE -> demuxer_service 造 ManifestDemuxer
         *     -> ManifestDemuxer::buildPlayList()                          framework/demuxer/manifest/ManifestDemuxer.cpp
         *         一次性把对象变成内部 playList（零 m3u8/mpd 文本、零清单网络请求）
         *
         * 分片的展开也在核心里：mode=template 时读 mode/timescale/startNumber/
         * totalCount/suffix/segmentTimeline/mediaSequence，显式分片直接取 segments[]，
         * URL 由核心按"绝对地址 > 根路径 > baseUrl 相对拼接"解析。
         *
         * 所以这里只做一件事：把 QML 给的 QVariantMap 序列化成 JSON 文本，
         * 剩下的交给核心。不判断 HLS/DASH、不展开分片、不拼 URL。
         */
        const QJsonDocument doc(QJsonObject::fromVariantMap(manifest));

        return setManifestJson(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
    }

    /*
     * **对象方式入口**：直接接 CicadaManifest::Builder 的产物。
     *
     * 与 setManifest(QVariantMap) **完全同一条路**，唯一区别是"对象从哪来"：
     *   builder.toJsonString()  ->  setManifestJson(json)  ->  SetDataSource(std::string)
     *     ->  内核 cJSON 解析 -> ManifestDemuxer::buildPlayList()
     * 也就是说对象方式和 JSON 文本方式在运行时**是同一份 JSON 文本、同一条内核路径**，
     * 行为与性能完全一致；本函数不做任何解析、补全或分片展开。
     *
     * 注意：往核心传清单**必须传 std::string**（见 createPlayer() 里的说明），
     * 传 const char* 会命中 SetDataSource(const char *url)，清单会被当成 URL。
     */
    bool CicadaPlayerItem::setManifest(const CicadaManifest::Builder &builder)
    {
        const QString json = builder.toJsonString();

        if (json.isEmpty() || json == QStringLiteral("{}")) {
            AF_LOGW("setManifest(builder): 空对象，忽略\n");
            return false;
        }

        return setManifestJson(json);
    }

    /*
     * 从本地 .json 文件读一份清单对象直接播放。
     *
     * 为什么放在 C++ 而不是 QML 里用 XMLHttpRequest 读：**Qt 默认禁止从 qrc: 文档
     * 发起 file:// 的 XHR**（QML_XHR_ALLOW_FILE_READ 没开），QML 里读本地文件会直接
     * 失败。而命令行那条路（main.cpp 的 startupManifestFromCommandLine）本来就是在
     * C++ 里读的，这里沿用同一个做法，QML 只需要把路径传进来。
     */
    bool CicadaPlayerItem::loadManifestFile(const QString &path)
    {
        const QString local = path.trimmed();

        if (local.isEmpty()) {
            AF_LOGW("loadManifestFile: 路径为空，忽略\n");
            return false;
        }

        QString filePath = local;
        /* 兼容 QML 的 FileDialog（给的是 file:// URL）与命令行（给的是裸路径）。 */
        if (filePath.startsWith(QStringLiteral("file:"))) {
            filePath = QUrl(filePath).toLocalFile();
        }

        QFile file(filePath);

        if (!file.open(QIODevice::ReadOnly)) {
            AF_LOGE("loadManifestFile: 打不开 %s (%s)\n",
                    filePath.toUtf8().constData(), file.errorString().toUtf8().constData());
            setErrorString(tr("打不开清单文件：%1").arg(file.errorString()));
            return false;
        }

        const QByteArray content = file.readAll();
        file.close();

        AF_LOGI("manifest file: %s (%lld bytes)\n",
                filePath.toUtf8().constData(), static_cast<long long>(content.size()));

        /* 文件名只用于界面/日志（manifestHint），不参与管线选择。 */
        m_manifestHint = QFileInfo(filePath).fileName();

        return setManifestJson(QString::fromUtf8(content));
    }

    /*
     * 和 loadManifestFile() 完全一样，只是把内容交给"按协议补全"那条路
     * （见 setManifestForProtocol）。界面上"从 .json 文件载入"走的就是它：
     * 文件里的对象往往是从别处导出的，字段经常缺一半，补全之后再喂给核心。
     */
    bool CicadaPlayerItem::loadManifestFileForProtocol(const QString &path, const QString &protocol)
    {
        const QString local = path.trimmed();

        if (local.isEmpty()) {
            AF_LOGW("loadManifestFileForProtocol: 路径为空，忽略\n");
            return false;
        }

        QString filePath = local;

        if (filePath.startsWith(QStringLiteral("file:"))) {
            filePath = QUrl(filePath).toLocalFile();
        }

        QFile file(filePath);

        if (!file.open(QIODevice::ReadOnly)) {
            AF_LOGE("loadManifestFileForProtocol: 打不开 %s (%s)\n",
                    filePath.toUtf8().constData(), file.errorString().toUtf8().constData());
            setErrorString(tr("打不开清单文件：%1").arg(file.errorString()));
            return false;
        }

        const QByteArray content = file.readAll();
        file.close();

        AF_LOGI("manifest file (%s): %s (%lld bytes)\n", protocol.toUtf8().constData(),
                filePath.toUtf8().constData(), static_cast<long long>(content.size()));

        m_manifestHint = QFileInfo(filePath).fileName();

        return setManifestForProtocol(QString::fromUtf8(content), protocol, m_manifestHint);
    }

    /*
     * ==================================================================================
     * 统计信息（右键菜单 →「视频统计信息」）+ 剪贴板 + 精准空降链接
     * ==================================================================================
     *
     * 【数据来源都是框架的真实读数，一个都不编】
     *   * 分辨率 / 码率      StreamInfo（GetCurrentStreamInfo，清晰度列表用的也是它）
     *   * 源帧率 / 编解码器  Stream_meta（GetCurrentStreamMeta；**用完必须 releaseMeta**）
     *   * 渲染帧率           GetOption("renderFps")            （AFActiveVideoRender 计的）
     *   * 丢帧 / 总帧        GetOption("videoDroppedInfo")     （格式 "dropped/total"）
     *   * 缓存速度           CurrentDownLoadSpeed 回调（bps，视频+音频合在一起）
     *   * 协议 / 容器        GetPropertyString(PROPERTY_KEY_CONTAINER_INFO)
     *   * 连接地址           GetPropertyString(PROPERTY_KEY_CONNECT_INFO)（curl 的 connectInfo）
     *   * 解码方式           本组件的 backend()（零拷贝 / CPU 回拷，实际生效的那条路）
     *
     * 【核心拿不到的】
     *   * 编解码器**字符串**（只有 AFCodecID 枚举）与 mimeType —— 所以这里自己把常用枚举
     *     翻成 "H.264" / "AAC"，清单对象那条路则直接从清单 JSON 里取 codecs/mimeType
     *     （那是唯一带这些字符串的地方）；
     *   * 单独的视频/音频缓存速度（核心只有一个合计值）；
     *   * 当前正在读的分片 URL（没有任何 API 返回它）→ 用片源 URL / 清单 baseUrl 的 host。
     */
    namespace {

        /*
         * 这个地址是不是"分片清单"（DASH 的 .mpd / HLS 的 .m3u8）。
         *
         * 只用于决定缓冲策略（见 createPlayer 里那段）：分片流按需取分片，缓冲留 15 秒就够；
         * 连续流（普通 http mp4、rtmp）保持框架默认的 40 秒。
         * 判断只看**路径部分**的后缀，忽略 query（签名 URL 很常见：
         * `.../index.m3u8?auth=...`）。认不出来就返回 false（按连续流处理，更保守）。
         */
        bool isSegmentPlaylistUrl(const QUrl &url)
        {
            if (url.isEmpty()) {
                return false;
            }

            const QString path = url.path().toLower();
            return path.endsWith(QLatin1String(".mpd")) || path.endsWith(QLatin1String(".m3u8"));
        }

        /*
         * 统计曲线：采样间隔与点数上限。
         * 500ms × 60 点 = 30 秒窗口 —— 面板就 400px 宽，再多点也画不出来，
         * 而 30 秒足够看出"切清晰度/seek 前后缓存速度和帧率怎么变"。
         */
        constexpr int STATS_SAMPLE_INTERVAL_MS = 500;
        constexpr int STATS_HISTORY_MAX = 60;

        /* AFCodecID → 人看得懂的名字（核心没有这个映射，只能自己来）。 */
        QString codecName(int codec)
        {
            switch (codec) {
                case AF_CODEC_ID_H264:
                    return QStringLiteral("H.264 / AVC");
                case AF_CODEC_ID_HEVC:
                    return QStringLiteral("H.265 / HEVC");
                case AF_CODEC_ID_VP8:
                    return QStringLiteral("VP8");
                case AF_CODEC_ID_VP9:
                    return QStringLiteral("VP9");
                case AF_CODEC_ID_AV1:
                    return QStringLiteral("AV1");
                case AF_CODEC_ID_MPEG4:
                    return QStringLiteral("MPEG-4");
                case AF_CODEC_ID_AAC:
                    return QStringLiteral("AAC");
                case AF_CODEC_ID_MP3:
                    return QStringLiteral("MP3");
                case AF_CODEC_ID_AC3:
                    return QStringLiteral("AC-3");
                case AF_CODEC_ID_EAC3:
                    return QStringLiteral("E-AC-3");
                case AF_CODEC_ID_OPUS:
                    return QStringLiteral("Opus");
                case AF_CODEC_ID_FLAC:
                    return QStringLiteral("FLAC");
                case AF_CODEC_ID_VORBIS:
                    return QStringLiteral("Vorbis");
                case AF_CODEC_ID_PCM_ALAW:
                case AF_CODEC_ID_PCM_MULAW:
                    return QStringLiteral("PCM");
                default:
                    return QString();
            }
        }

        /* 码率（bps）→ 面板里那行字（和参考实现一样是 Kbps，超过 1000 就换成 Mbps）。 */
        QString bitrateText(qlonglong bps)
        {
            if (bps <= 0) {
                return QStringLiteral("--");
            }

            const double kbps = bps / 1000.0;

            if (kbps >= 1000.0) {
                return QStringLiteral("%1 Mbps").arg(kbps / 1000.0, 0, 'f', 2);
            }

            return QStringLiteral("%1 Kbps").arg(kbps, 0, 'f', 0);
        }

        /* 从 URL 里取 host[:port]（拿不到就返回空串）。 */
        QString hostOf(const QString &url)
        {
            if (url.isEmpty()) {
                return QString();
            }

            const QUrl u(url);

            if (!u.isValid() || u.host().isEmpty()) {
                return QString();
            }

            return u.port() > 0 ? QStringLiteral("%1:%2").arg(u.host()).arg(u.port())
                                : u.host();
        }

        /* 把一个 JSON 对象里的 "host" 字段取出来（connectInfo 用）。 */
        QString hostFromConnectInfo(const QString &json)
        {
            if (json.trimmed().isEmpty()) {
                return QString();
            }

            const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());

            if (!doc.isObject()) {
                return QString();
            }

            const QJsonObject o = doc.object();
            const QString ip = o.value(QStringLiteral("ip")).toString();

            if (!ip.isEmpty()) {
                return ip;
            }

            return hostOf(o.value(QStringLiteral("url")).toString());
        }

    }// anonymous namespace

    QVariantMap CicadaPlayerItem::buildStats() const
    {
        QVariantMap s;

        /*
         * stats() 是 const 属性读法，但这里要调 MediaPlayer 的 getter —— 它们是线程安全的
         * （走框架自己的锁/消息），所以用 const_cast 把 player 拿出来读。
         * 只在 GUI 线程调用（refreshStats 由 GUI 线程的定时器驱动）。
         */
        auto *self = const_cast<CicadaPlayerItem *>(this);

        if (self->m_player == nullptr) {
            return s;
        }

        MediaPlayer *p = self->m_player.get();

        s.insert(QStringLiteral("width"), self->m_videoWidth);
        s.insert(QStringLiteral("height"), self->m_videoHeight);
        s.insert(QStringLiteral("protocol"), self->m_protocol);
        s.insert(QStringLiteral("decodeMode"), self->backend());
        s.insert(QStringLiteral("zeroCopy"), self->zeroCopy());
        s.insert(QStringLiteral("device"), self->device());
        s.insert(QStringLiteral("positionMs"), self->m_position);
        s.insert(QStringLiteral("bufferedMs"), self->m_bufferedPosition);
        s.insert(QStringLiteral("durationMs"), p->GetDuration());
        s.insert(QStringLiteral("downloadSpeed"), self->m_downloadSpeed);
        s.insert(QStringLiteral("downloadSpeedText"), self->downloadSpeedText());

        /* ---- 视频流：分辨率 / 码率 / 源帧率 / 编解码器 ---- */
        qlonglong videoBitrate = 0;
        qlonglong videoBandwidth = 0;
        double sourceFps = 0.0;
        QString videoCodec = self->m_manifestVideoCodecs;

        if (const StreamInfo *vi = p->GetCurrentStreamInfo(ST_TYPE_VIDEO)) {
            s.insert(QStringLiteral("width"), vi->videoWidth > 0 ? vi->videoWidth : self->m_videoWidth);
            s.insert(QStringLiteral("height"), vi->videoHeight > 0 ? vi->videoHeight : self->m_videoHeight);
            videoBitrate = static_cast<qlonglong>(vi->bitrate);
            videoBandwidth = static_cast<qlonglong>(vi->videoBandwidth);
            s.insert(QStringLiteral("hdr"), vi->HDRType == VideoHDRType_HDR10 ? QStringLiteral("HDR10")
                                                                             : QStringLiteral("SDR"));
        }

        Stream_meta vmeta{};
        bool hasVMeta = (p->GetCurrentStreamMeta(&vmeta, ST_TYPE_VIDEO) == 0);

        if (hasVMeta) {
            if (vmeta.avg_fps > 0.0) {
                sourceFps = vmeta.avg_fps;
            }

            if (vmeta.bandwidth > 0) {
                videoBandwidth = static_cast<qlonglong>(vmeta.bandwidth);
            }

            if (vmeta.bitrate > 0 && videoBitrate <= 0) {
                videoBitrate = static_cast<qlonglong>(vmeta.bitrate);
            }

            if (videoCodec.isEmpty()) {
                videoCodec = codecName(static_cast<int>(vmeta.codec));
            }

            if (vmeta.width > 0 && vmeta.height > 0) {
                s.insert(QStringLiteral("width"), vmeta.width);
                s.insert(QStringLiteral("height"), vmeta.height);
            }

            /*
             * 【必须释放】GetCurrentStreamMeta 每次调用都会 strdup 里面的字符串
             * （lang/description/extradata/keyUrl/…），不释放就是每次刷新泄漏一份。
             */
            releaseMeta(&vmeta);
        }

        s.insert(QStringLiteral("frameRate"), sourceFps);
        s.insert(QStringLiteral("videoCodec"), videoCodec);
        s.insert(QStringLiteral("videoBitrate"), videoBitrate > 0 ? videoBitrate : videoBandwidth);
        s.insert(QStringLiteral("videoBitrateText"),
                 bitrateText(videoBitrate > 0 ? videoBitrate : videoBandwidth));

        /* ---- 音频流 ---- */
        qlonglong audioBitrate = 0;
        QString audioCodec = self->m_manifestAudioCodecs;
        int sampleRate = 0;
        int channels = 0;

        if (const StreamInfo *ai = p->GetCurrentStreamInfo(ST_TYPE_AUDIO)) {
            audioBitrate = static_cast<qlonglong>(ai->bitrate);
            sampleRate = ai->sampleRate;
            channels = ai->nChannels;
        }

        Stream_meta ameta{};
        bool hasAMeta = (p->GetCurrentStreamMeta(&ameta, ST_TYPE_AUDIO) == 0);

        if (hasAMeta) {
            if (ameta.bitrate > 0 && audioBitrate <= 0) {
                audioBitrate = static_cast<qlonglong>(ameta.bitrate);
            }

            if (ameta.bandwidth > 0 && audioBitrate <= 0) {
                audioBitrate = static_cast<qlonglong>(ameta.bandwidth);
            }

            if (audioCodec.isEmpty()) {
                audioCodec = codecName(static_cast<int>(ameta.codec));
            }

            if (ameta.samplerate > 0) {
                sampleRate = ameta.samplerate;
            }

            if (ameta.channels > 0) {
                channels = ameta.channels;
            }

            releaseMeta(&ameta);
        }

        s.insert(QStringLiteral("audioCodec"), audioCodec);
        s.insert(QStringLiteral("audioBitrate"), audioBitrate);
        s.insert(QStringLiteral("audioBitrateText"), bitrateText(audioBitrate));
        s.insert(QStringLiteral("sampleRate"), sampleRate);
        s.insert(QStringLiteral("channels"), channels);

        /* ---- 媒体类型（参考实现那一行就是 mime/codecs 串） ---- */
        QString mediaType;

        if (!self->m_manifestVideoCodecs.isEmpty() || !self->m_manifestAudioCodecs.isEmpty()) {
            if (!self->m_manifestVideoCodecs.isEmpty()) {
                mediaType += QStringLiteral("video/mp4;codecs=\"%1\"").arg(self->m_manifestVideoCodecs);
            }

            if (!self->m_manifestAudioCodecs.isEmpty()) {
                if (!mediaType.isEmpty()) {
                    mediaType += QLatin1Char(',');
                }

                mediaType += QStringLiteral("audio/mp4;codecs=\"%1\"").arg(self->m_manifestAudioCodecs);
            }
        } else if (hasVMeta || hasAMeta || !videoCodec.isEmpty() || !audioCodec.isEmpty()) {
            /* 没有清单对象（普通 URL 片源）：核心只给枚举，这里按它的名字拼 */
            if (!videoCodec.isEmpty()) {
                mediaType += QStringLiteral("video/* [%1]").arg(videoCodec);
            }

            if (!audioCodec.isEmpty()) {
                if (!mediaType.isEmpty()) {
                    mediaType += QLatin1Char(',');
                }

                mediaType += QStringLiteral("audio/* [%1]").arg(audioCodec);
            }
        }

        s.insert(QStringLiteral("mediaType"), mediaType);

        /* ---- 播放器类型（参考实现那行是 DashPlayer / HlsPlayer） ---- */
        QString playerType = self->m_protocol;

        if (playerType == QLatin1String("DASH")) {
            playerType = QStringLiteral("DashPlayer");
        } else if (playerType == QLatin1String("HLS")) {
            playerType = QStringLiteral("HlsPlayer");
        } else if (!self->m_source.isEmpty() && self->m_source.isLocalFile()) {
            playerType = QStringLiteral("LocalFile");
        } else if (!self->m_source.isEmpty()) {
            playerType = QStringLiteral("HttpPlayer");
        } else if (self->streaming()) {
            playerType = QStringLiteral("ManifestPlayer");
        } else {
            playerType = QStringLiteral("--");
        }

        s.insert(QStringLiteral("playerType"), playerType);

        /* ---- Host：清单对象用 baseUrl 的 host，普通地址用 source 的 host ---- */
        QString videoHost = self->m_manifestVideoHost;
        QString audioHost = self->m_manifestAudioHost;

        if (videoHost.isEmpty() && !self->m_source.isEmpty()) {
            videoHost = hostOf(self->m_source.toString());
        }

        if (audioHost.isEmpty()) {
            audioHost = videoHost;
        }

        /*
         * 兜底：从 curl 的 connectInfo 里拿（普通 URL 片源时它就是真正的连接地址/IP）。
         * 核心没有"当前分片 URL"的 API，这是最接近的一个真实读数。
         */
        if (videoHost.isEmpty()) {
            const std::string info = p->GetPropertyString(PROPERTY_KEY_CONNECT_INFO);
            videoHost = hostFromConnectInfo(QString::fromStdString(info));

            if (audioHost.isEmpty()) {
                audioHost = videoHost;
            }
        }

        s.insert(QStringLiteral("videoHost"), videoHost.isEmpty() ? QStringLiteral("--") : videoHost);
        s.insert(QStringLiteral("audioHost"), audioHost.isEmpty() ? QStringLiteral("--") : audioHost);

        /* ---- 缓存速度：核心只给一个合计值（bps） ---- */
        s.insert(QStringLiteral("videoSpeed"), self->m_downloadSpeed);
        s.insert(QStringLiteral("audioSpeed"), self->m_downloadSpeed);
        s.insert(QStringLiteral("videoSpeedText"), bitrateText(self->m_downloadSpeed));
        s.insert(QStringLiteral("audioSpeedText"), bitrateText(self->m_downloadSpeed));

        /* ---- 实际渲染帧率 / 丢帧（GetOption 的字符串读数） ---- */
        char buf[MAX_OPT_VALUE_LENGTH] = {};

        p->GetOption("renderFps", buf);
        s.insert(QStringLiteral("renderFps"), QString::fromLatin1(buf).toDouble());

        buf[0] = '\0';
        p->GetOption("videoDroppedInfo", buf);
        const QString dropped = QString::fromLatin1(buf);
        const QStringList parts = dropped.split(QLatin1Char('/'));
        s.insert(QStringLiteral("droppedFrames"),
                 parts.size() == 2 ? parts.at(0).toULongLong() : 0ULL);
        s.insert(QStringLiteral("totalFrames"),
                 parts.size() == 2 ? parts.at(1).toULongLong() : 0ULL);

        /* ---- 容器名（容器/封装那一行） ---- */
        const std::string container = p->GetPropertyString(PROPERTY_KEY_CONTAINER_INFO);
        QJsonObject containerObj;

        if (!container.empty()) {
            const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(container));

            if (doc.isObject()) {
                containerObj = doc.object();
            }
        }

        s.insert(QStringLiteral("container"), containerObj.value(QStringLiteral("containerName")).toString());
        s.insert(QStringLiteral("isMultiBitrate"), containerObj.value(QStringLiteral("isMultiBitrate")).toString());

        /* 缓冲时长 = 已缓冲位置 - 当前位置（核心没有"缓冲时长"的 getter）。 */
        const qlonglong bufferMs = self->m_bufferedPosition - self->m_position;
        s.insert(QStringLiteral("bufferMs"), bufferMs > 0 ? bufferMs : 0);

        return s;
    }

    void CicadaPlayerItem::refreshStats()
    {
        const QVariantMap fresh = buildStats();

        /*
         * 值没变就不发信号：面板 2Hz 刷新，每次都发会让 QML 里那一堆绑定白重算
         * （而且 QVariantMap 的相等比较很便宜）。
         */
        if (fresh == m_stats) {
            /* 值没变，但采样还在跑的话也要往曲线上补一个点（曲线要的是时间序列）。 */
            if (m_statsSampling) {
                appendStatsSample(fresh);
            }

            return;
        }

        m_stats = fresh;
        emit statsChanged();

        if (m_statsSampling) {
            appendStatsSample(fresh);
        }
    }

    /*
     * 往曲线里补一个采样点。
     *
     * 采样点形状（QML 里画三条曲线用同一份数据）：
     *   { t: <毫秒>, speed: <B/s>, fps: <帧率>, buffer: <秒> }
     * 上限 STATS_HISTORY_MAX 个（500ms 一个 ≈ 30 秒窗口），超了从头丢 —— 曲线是
     * "最近一段时间的趋势"，不需要无限增长（内存有界，这点在长时间挂着时很重要）。
     */
    void CicadaPlayerItem::appendStatsSample(const QVariantMap &snapshot)
    {
        QVariantMap point;
        point.insert(QStringLiteral("t"), QDateTime::currentMSecsSinceEpoch());
        /* 缓存速度：核心给的是 bps，这里换成 B/s（曲线量纲和面板文字不同，QML 各自格式化） */
        point.insert(QStringLiteral("speed"), snapshot.value(QStringLiteral("downloadSpeed")).toLongLong() / 8);
        point.insert(QStringLiteral("fps"), snapshot.value(QStringLiteral("renderFps")).toDouble());
        point.insert(QStringLiteral("buffer"), snapshot.value(QStringLiteral("bufferMs")).toLongLong() / 1000.0);
        m_statsHistory.append(point);

        while (m_statsHistory.size() > STATS_HISTORY_MAX) {
            m_statsHistory.removeFirst();
        }

        /* 前 3 个点每个都留一行，之后每 10 个点一行：曲线画不出来时，日志能立刻区分
         * "没采到数据"（一行都没有）还是"采到了没画出来"（这几行在涨）。 */
        if (m_statsHistory.size() <= 3 || m_statsHistory.size() % 10 == 0) {
            AF_LOGI("stats curves: %d points (last: speed=%lld B/s fps=%.1f buffer=%.1fs)\n",
                    (int) m_statsHistory.size(),
                    (long long) point.value(QStringLiteral("speed")).toLongLong(),
                    point.value(QStringLiteral("fps")).toDouble(),
                    point.value(QStringLiteral("buffer")).toDouble());
        }

        emit statsHistoryChanged();
    }

    void CicadaPlayerItem::startStatsSampling()
    {
        /*
         * 面板打开了。默认就已经在采（见头文件里 m_statsSampling 的说明：这是兜底，
         * 免得"QML 那一次调用没落到"就让曲线永远空着），所以这里绝大多数时候只是
         * 保证定时器在跑 + 通知一次 QML，**不清历史** ——
         * 这样一来面板一打开就能看到最近 30 秒的完整趋势，而不是从空白开始画。
         */
        ensureStatsTimer();

        if (m_statsSampling) {
            emit statsHistoryChanged();
            return;
        }

        m_statsSampling = true;
        AF_LOGI("stats curves: sampling resumed\n");
        /* 立刻采一个点：面板一打开就有东西可画 */
        refreshStats();
        emit statsHistoryChanged();
    }

    void CicadaPlayerItem::stopStatsSampling()
    {
        if (!m_statsSampling) {
            return;
        }

        m_statsSampling = false;

        /*
         * 面板关了就把定时器停掉：每 500ms 一次的唤醒、以及每次
         * GetCurrentStreamMeta/GetOption 那点开销都省掉（用户要求"关了就停止监听"）。
         * stats 属性本身不再刷新也没关系 —— 下次打开会重新 ensureStatsTimer()。
         *
         * **历史不清空**：清掉的话，下次打开面板要等 30 秒才能把曲线填满；
         * 留着的话一打开就是一条完整的趋势线（多留 60 个小 map，内存可忽略）。
         */
        if (m_statsTimer != nullptr) {
            m_statsTimer->stop();
        }

        AF_LOGI("stats curves: sampling stopped (history kept: %d points)\n",
                (int) m_statsHistory.size());
        emit statsHistoryChanged();
    }

    void CicadaPlayerItem::ensureStatsTimer()
    {
        if (m_statsTimer == nullptr) {
            m_statsTimer = new QTimer(this);
            m_statsTimer->setInterval(STATS_SAMPLE_INTERVAL_MS);
            m_statsTimer->setTimerType(Qt::CoarseTimer);
            QObject::connect(m_statsTimer, &QTimer::timeout, this, [this]() {
                refreshStats();
            });
        }

        if (!m_statsTimer->isActive()) {
            refreshStats();
            m_statsTimer->start();
        }
    }

    /*
     * 从清单 JSON 里把编解码器字符串和 host 抠出来（每个清单只做一次）。
     * 为什么只能从这里拿：核心的 StreamInfo/Stream_meta 只有 AFCodecID 枚举和数字，
     * 没有任何地方保存 codecs/mimeType 字符串 —— 清单对象是唯一的来源。
     */
    void CicadaPlayerItem::parseManifestStatsInfo()
    {
        m_manifestVideoCodecs.clear();
        m_manifestAudioCodecs.clear();
        m_manifestVideoHost.clear();
        m_manifestAudioHost.clear();

        if (m_manifestJson.isEmpty()) {
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(m_manifestJson.toUtf8());

        if (!doc.isObject()) {
            return;
        }

        const QJsonObject root = doc.object();
        const QJsonArray videos = root.value(QStringLiteral("video")).toArray();
        const QJsonArray audios = root.value(QStringLiteral("audio")).toArray();

        /* 取码率最高的那一条视频/音频（面板显示的就是当前在播的那一档的近似值）。 */
        auto pickCodecs = [](const QJsonArray &array, QString *codecs, QString *host, bool highest) {
            double bestBandwidth = -1.0;

            for (const QJsonValue &v : array) {
                const QJsonObject rep = v.toObject();
                const double bw = rep.value(QStringLiteral("bandwidth")).toDouble(0);

                if (!highest && bestBandwidth >= 0) {
                    break;
                }

                if (bw < bestBandwidth) {
                    continue;
                }

                bestBandwidth = bw;
                *codecs = rep.value(QStringLiteral("codecs")).toString();

                if (host != nullptr && host->isEmpty()) {
                    *host = hostOf(rep.value(QStringLiteral("baseUrl")).toString());
                }
            }
        };

        pickCodecs(videos, &m_manifestVideoCodecs, &m_manifestVideoHost, true);
        pickCodecs(audios, &m_manifestAudioCodecs, &m_manifestAudioHost, true);
    }

    bool CicadaPlayerItem::copyTextToClipboard(const QString &text)
    {
        QClipboard *clipboard = QGuiApplication::clipboard();

        if (clipboard == nullptr) {
            AF_LOGW("copyTextToClipboard: 拿不到系统剪贴板\n");
            return false;
        }

        clipboard->setText(text);
        AF_LOGI("copyTextToClipboard: 已复制 %lld 个字符\n", static_cast<long long>(text.size()));
        return true;
    }

    /*
     * ==================================================================================
     * 视频色彩调整（右键菜单 →「视频色彩调整」）
     *
     * 值：0~200，100 = 中性（面板三条滑块的原始值）。
     *
     * 实现落在**渲染后端**：Windows 的 D3D11 零拷贝路径本来每帧要用视频处理器把
     * NV12/P010 转成 RGBA，视频处理器自带 BRIGHTNESS / CONTRAST / SATURATION / HUE
     * 四个「过滤器」（ID3D11VideoContext::VideoProcessorSetStreamFilter），
     * 在那里应用是**零额外代价**的（不额外拷贝、不退出零拷贝路径）。
     * macOS(Metal) / Linux(VAAPI) / CPU 回退目前没有实现，colorAdjustSupported()
     * 会返回 false，界面如实说明 —— 见 CicadaVideoTexture::colorAdjustSupported()。
     */
    void CicadaPlayerItem::setColorAdjust(int brightness, int contrast, int saturation)
    {
        m_colorBrightness = qBound(0, brightness, 200);
        m_colorContrast = qBound(0, contrast, 200);
        m_colorSaturation = qBound(0, saturation, 200);

        AF_LOGI("colour adjust: brightness=%d contrast=%d saturation=%d (0~200, 100 = neutral)\n",
                m_colorBrightness, m_colorContrast, m_colorSaturation);

        if (m_textureBackend) {
            /* 后端线程安全（内部是原子量），这一步可以从 GUI 线程直接调 */
            m_textureBackend->setColorAdjust(m_colorBrightness, m_colorContrast, m_colorSaturation);
        }
    }

    bool CicadaPlayerItem::colorAdjustSupported() const
    {
        if (m_textureBackend) {
            return m_textureBackend->colorAdjustSupported();
        }

        /* 后端还没建（起播前）：按平台答，Windows 上是 true */
#if defined(Q_OS_WIN)
        return true;
#else
        return false;
#endif
    }

    QString CicadaPlayerItem::landingLink() const
    {
        const qlonglong seconds = m_position / 1000;

        if (!m_source.isEmpty()) {
            QString url = m_source.toString();
            /* "精准空降"：把当前播放位置带上（已有查询串就用 &t=）。 */
            url += url.contains(QLatin1Char('?')) ? QStringLiteral("&t=%1").arg(seconds)
                                                  : QStringLiteral("?t=%1").arg(seconds);
            return url;
        }

        if (!m_manifestJson.isEmpty()) {
            /* 清单对象没有可分享的 URL：写清楚"这是清单对象 + 位置"，别伪造一个链接。 */
            const QString name = m_manifestHint.isEmpty() ? tr("清单对象") : m_manifestHint;
            return tr("%1（%2）播放位置 %3 秒").arg(name, m_protocol).arg(seconds);
        }

        return QString();
    }

    /*
     * ==================================================================================
     * 协议标签（界面左上角那颗小徽标）与"按协议的清单对象补全"
     * ==================================================================================
     */

    namespace {

        /* 后缀 → 协议标签。认不出来返回空串。 */
        QString protocolFromExtension(const QString &path)
        {
            const int dot = path.lastIndexOf(QLatin1Char('.'));

            if (dot < 0 || dot + 1 >= path.size()) {
                return QString();
            }

            const QString ext = path.mid(dot + 1).toLower();

            if (ext == QLatin1String("mpd")) {
                return QStringLiteral("DASH");
            }

            if (ext == QLatin1String("m3u8")) {
                return QStringLiteral("HLS");
            }

            if (ext == QLatin1String("flv")) {
                return QStringLiteral("FLV");
            }

            if (ext == QLatin1String("ts") || ext == QLatin1String("m2ts")) {
                return QStringLiteral("TS");
            }

            if (ext == QLatin1String("mp4") || ext == QLatin1String("m4v")) {
                return QStringLiteral("MP4");
            }

            if (ext == QLatin1String("mkv")) {
                return QStringLiteral("MKV");
            }

            if (ext == QLatin1String("mov")) {
                return QStringLiteral("MOV");
            }

            if (ext == QLatin1String("avi")) {
                return QStringLiteral("AVI");
            }

            if (ext == QLatin1String("webm")) {
                return QStringLiteral("WEBM");
            }

            if (ext == QLatin1String("wmv")) {
                return QStringLiteral("WMV");
            }

            if (ext == QLatin1String("mpg") || ext == QLatin1String("mpeg")) {
                return QStringLiteral("MPEG");
            }

            /* 认不出来的后缀：原样大写显示，比只写 "FILE" 有信息量。 */
            return ext.toUpper();
        }

        /* 协议标签的一句话说明（徽标悬停提示用）。 */
        QString protocolDetailFor(const QString &label, const QString &scheme, bool fromManifest)
        {
            if (fromManifest) {
                if (label == QLatin1String("DASH")) {
                    return QObject::tr("清单对象（DASH）：分片由 segmentInfo 展开，无清单请求");
                }

                if (label == QLatin1String("HLS")) {
                    return QObject::tr("清单对象（HLS）：分片号从 mediaSequence 起算，无清单请求");
                }

                return QObject::tr("清单对象");
            }

            const QString s = scheme.isEmpty() ? QStringLiteral("file") : scheme;

            if (label == QLatin1String("DASH")) {
                return QObject::tr("%1 上的 MPEG-DASH（.mpd）：多码率 + 自适应").arg(s);
            }

            if (label == QLatin1String("HLS")) {
                return QObject::tr("%1 上的 HLS（.m3u8）：多码率 + 自适应").arg(s);
            }

            if (label == QLatin1String("FLV")) {
                return QObject::tr("%1 上的 FLV").arg(s);
            }

            if (s == QLatin1String("rtmp") || s == QLatin1String("rtsp") || s == QLatin1String("srt")
                || s == QLatin1String("udp") || s == QLatin1String("rtp")) {
                return QObject::tr("实时流（%1）").arg(s.toUpper());
            }

            if (s == QLatin1String("http") || s == QLatin1String("https")) {
                return QObject::tr("%1 上的渐进式下载（%2）").arg(s).arg(label);
            }

            if (s == QLatin1String("file") || s == QLatin1String("qrc")) {
                return QObject::tr("本地文件（%1）").arg(label);
            }

            return QObject::tr("%1（%2）").arg(s).arg(label);
        }

        /*
         * 按协议补全清单对象。**只补不改**：对象里已有的字段一律不动，唯一会改写的
         * 是 mediaSourceType（按用户点的入口写死），因为它就是"走哪条分片管线"的声明。
         *
         * 补的东西来自核心的实际消费点：
         *   Manifest::MediaManifestParser::parse()      → 字段名/默认值
         *   ManifestDemuxer::expandTemplate()           → mode/initialization/media/
         *                                                 timescale/startNumber/totalCount
         *   ManifestDemuxer::buildPlayList()            → mediaSequence（HLS 段号起点）
         * 返回 false 时 error 里写的是"到底缺什么"，界面直接显示给用户。
         */
        bool normalizeManifestObject(QJsonObject &root, const QString &protocol, QString &error,
                                     int *injectedFields)
        {
            const QString proto = protocol.trimmed().toLower();
            const bool isDash = (proto != QLatin1String("hls"));   /* 缺省 dash */
            const QString wanted = isDash ? QStringLiteral("dash") : QStringLiteral("hls");

            auto note = [injectedFields](const QString &what) {
                if (injectedFields != nullptr) {
                    ++(*injectedFields);
                }
                AF_LOGI("normalizeManifestObject: 补上 %s\n", what.toUtf8().constData());
            };

            /* ---- 顶层：协议、时长、video[] ---- */
            const QString declared = root.value(QStringLiteral("mediaSourceType")).toString().trimmed().toLower();

            if (!declared.isEmpty() && declared != wanted) {
                error = QObject::tr("这个对象的 mediaSourceType 是 “%1”，但你用的是 %2 入口；"
                                    "两种清单对象的分片字段不一样，请换对应的入口，"
                                    "或者把 mediaSourceType 改成 “%1” 对应的那个入口再试。")
                        .arg(declared, isDash ? QStringLiteral("DASH") : QStringLiteral("HLS"));
                return false;
            }

            if (declared != wanted) {
                root.insert(QStringLiteral("mediaSourceType"), wanted);
                note(QStringLiteral("mediaSourceType=%1").arg(wanted));
            }

            const bool live = root.value(QStringLiteral("live")).toBool(false);
            const double duration = root.value(QStringLiteral("duration")).toDouble(0);

            if (!live && duration <= 0) {
                error = QObject::tr("对象里没有 duration（或 <= 0）。VOD 清单必须给总时长"
                                    "（秒，例如 268.2）；直播清单请显式写 \"live\": true。");
                return false;
            }

            if (!root.contains(QStringLiteral("minBufferTime"))) {
                root.insert(QStringLiteral("minBufferTime"), 1.5);
                note(QStringLiteral("minBufferTime=1.5（框架默认值）"));
            }

            QJsonArray video = root.value(QStringLiteral("video")).toArray();

            if (video.isEmpty()) {
                error = QObject::tr("对象里没有 video[]（或它是空的）。核心要求至少一条视频"
                                    "Representation，否则直接拒绝打开。");
                return false;
            }

            /* ---- 逐条 Representation 检查/补全分片信息 ---- */
            const QStringList kinds = { QStringLiteral("video"), QStringLiteral("audio"),
                                        QStringLiteral("subtitle") };

            for (const QString &kind : kinds) {
                QJsonArray array = root.value(kind).toArray();

                for (int i = 0; i < array.size(); ++i) {
                    if (!array.at(i).isObject()) {
                        error = QObject::tr("%1[%2] 不是对象。").arg(kind).arg(i);
                        return false;
                    }

                    QJsonObject rep = array.at(i).toObject();
                    const QString id = rep.value(QStringLiteral("id")).toString(QStringLiteral("#%1").arg(i));

                    if (kind == QLatin1String("video")
                        && rep.value(QStringLiteral("bandwidth")).toDouble(0) <= 0) {
                        /* 不是致命的，但带宽是 ABR 和清晰度菜单唯一的排序依据。 */
                        AF_LOGW("manifest %s (id=%s): 没有 bandwidth，ABR/清晰度列表会退化\n",
                                kind.toUtf8().constData(), id.toUtf8().constData());
                    }

                    if (!rep.value(QStringLiteral("segmentInfo")).isObject()) {
                        error = QObject::tr("%1 (id=%2) 缺少 segmentInfo：核心要靠它展开分片"
                                            "（mode + initialization + media，或者直接给 segments[]）。")
                                .arg(kind, id);
                        return false;
                    }

                    QJsonObject seg = rep.value(QStringLiteral("segmentInfo")).toObject();
                    QString mode = seg.value(QStringLiteral("mode")).toString().trimmed().toLower();
                    const QJsonArray segments = seg.value(QStringLiteral("segments")).toArray();
                    const QString init = seg.value(QStringLiteral("initialization")).toString();
                    const QString media = seg.value(QStringLiteral("media")).toString();
                    const QJsonArray timeline = seg.value(QStringLiteral("segmentTimeline")).toArray();

                    /* mode 缺失时按内容推断（和核心的两种展开方式一一对应）。 */
                    if (mode.isEmpty()) {
                        if (!segments.isEmpty()) {
                            mode = QStringLiteral("list");
                        } else if (!init.isEmpty() || !media.isEmpty()) {
                            mode = QStringLiteral("template");
                        } else {
                            error = QObject::tr("%1 (id=%2) 的 segmentInfo 既没有 segments[]，"
                                                "也没有 initialization/media —— 补不出分片列表。")
                                    .arg(kind, id);
                            return false;
                        }

                        seg.insert(QStringLiteral("mode"), mode);
                        note(QStringLiteral("%1 (id=%2) segmentInfo.mode=%3（按内容推断）")
                             .arg(kind, id, mode));
                    }

                    if (mode == QLatin1String("template")) {
                        if (init.isEmpty()) {
                            error = QObject::tr("%1 (id=%2) 是 template 模式但没有 initialization"
                                                "（初始化分片，例如 \"video-1080-0.m4s\"）。")
                                    .arg(kind, id);
                            return false;
                        }

                        if (media.isEmpty()) {
                            /* 核心能自己从 initialization 推出 "...-*.m4s"，这里补出来是为了
                             * 日志/界面里能直接看到最终用的模板，语义和核心一致。 */
                            const int dot = init.lastIndexOf(QLatin1Char('.'));
                            const int dash = init.lastIndexOf(QLatin1Char('-'), dot < 0 ? init.size() : dot);

                            if (dot > 0 && dash > 0) {
                                seg.insert(QStringLiteral("media"),
                                           init.left(dash + 1) + QLatin1Char('*') + init.mid(dot));
                                note(QStringLiteral("%1 (id=%2) segmentInfo.media=%3（从 initialization 推）")
                                     .arg(kind, id, seg.value(QStringLiteral("media")).toString()));
                            } else {
                                error = QObject::tr("%1 (id=%2) 的 initialization 是 “%3”，推不出分片"
                                                    "模板（需要形如 “xxx-0.m4s”）；请显式给 segmentInfo.media。")
                                        .arg(kind, id, init);
                                return false;
                            }
                        }

                        /* timescale：只有 segmentTimeline 的 d 依赖它，没给就按微秒（DASH 惯例）。 */
                        if (!seg.contains(QStringLiteral("timescale")) && !timeline.isEmpty()) {
                            seg.insert(QStringLiteral("timescale"), 1000000);
                            note(QStringLiteral("%1 (id=%2) segmentInfo.timescale=1000000").arg(kind, id));
                        }

                        if (!seg.contains(QStringLiteral("startNumber"))) {
                            seg.insert(QStringLiteral("startNumber"), 1);
                            note(QStringLiteral("%1 (id=%2) segmentInfo.startNumber=1").arg(kind, id));
                        }

                        /* totalCount：核心没给时会用 ceil(duration/targetDuration)，那只是近似；
                         * 有 segmentTimeline 就按时间线把条目数（含 r 展开）算准。 */
                        if (!seg.contains(QStringLiteral("totalCount"))) {
                            int64_t count = 0;

                            for (const QJsonValue &v : timeline) {
                                const QJsonObject entry = v.toObject();
                                const int64_t r = entry.value(QStringLiteral("r")).toInt(0);
                                const bool hasR = entry.contains(QStringLiteral("r"));
                                count += hasR ? (r >= 0 ? r + 1 : 0) : 1;
                            }

                            if (count <= 0) {
                                const double target = seg.value(QStringLiteral("targetDuration")).toDouble(4);

                                if (target > 0 && duration > 0) {
                                    count = static_cast<int64_t>(std::ceil(duration / target));
                                }
                            }

                            if (count > 0) {
                                seg.insert(QStringLiteral("totalCount"), static_cast<double>(count));
                                note(QStringLiteral("%1 (id=%2) segmentInfo.totalCount=%3")
                                     .arg(kind, id).arg(count));
                            }
                        }

                        /* HLS：段号从 mediaSequence 起算。缺了它，核心会退回 startNumber，
                         * 大多数情况能用，但和 m3u8 里的 #EXT-X-MEDIA-SEQUENCE 不一致时
                         * 段号就错了（相对地址/后缀命名会跟着错）。 */
                        if (!isDash && !seg.contains(QStringLiteral("mediaSequence"))) {
                            seg.insert(QStringLiteral("mediaSequence"),
                                       seg.value(QStringLiteral("startNumber")).toInt(1));
                            note(QStringLiteral("%1 (id=%2) segmentInfo.mediaSequence=%3（HLS 段号起点）")
                                 .arg(kind, id).arg(seg.value(QStringLiteral("mediaSequence")).toInt(1)));
                        }

                        if (!seg.contains(QStringLiteral("targetDuration")) && !timeline.isEmpty()) {
                            /* 只是给核心一个合理的默认（用于 totalCount 的兜底和缓冲估算）。 */
                            seg.insert(QStringLiteral("targetDuration"), 4);
                            note(QStringLiteral("%1 (id=%2) segmentInfo.targetDuration=4").arg(kind, id));
                        }
                    } else if (mode == QLatin1String("list")) {
                        if (segments.isEmpty()) {
                            error = QObject::tr("%1 (id=%2) 是 list 模式但 segments[] 是空的。")
                                    .arg(kind, id);
                            return false;
                        }
                    } else if (mode != QLatin1String("single")) {
                        error = QObject::tr("%1 (id=%2) 的 segmentInfo.mode 是 “%3”，只支持 "
                                            "template / list / single。").arg(kind, id, mode);
                        return false;
                    }

                    rep.insert(QStringLiteral("segmentInfo"), seg);
                    array.replace(i, rep);
                }

                if (!array.isEmpty()) {
                    root.insert(kind, array);
                }
            }

            /* DASH 对象里混进 mediaSequence 是"对象拿错了"的典型特征，提醒一句。 */
            if (isDash) {
                const QJsonArray v = root.value(QStringLiteral("video")).toArray();

                if (!v.isEmpty()
                    && v.first().toObject().value(QStringLiteral("segmentInfo")).toObject()
                       .contains(QStringLiteral("mediaSequence"))) {
                    AF_LOGW("normalizeManifestObject: 对象里有 mediaSequence（HLS 字段），"
                            "但你用的是 DASH 入口 —— 段号会从它数起，注意确认\n");
                }
            }

            return true;
        }

    }// anonymous namespace

    bool CicadaPlayerItem::setManifestForProtocol(const QString &json, const QString &protocol,
                                                  const QString &name)
    {
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &parseError);

        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            AF_LOGE("setManifestForProtocol: JSON 解析失败 (%s at %d)\n",
                    parseError.errorString().toUtf8().constData(), parseError.offset);
            setErrorString(tr("流清单不是合法的 JSON：%1").arg(parseError.errorString()));
            return false;
        }

        QJsonObject root = doc.object();
        QString error;
        int injected = 0;

        if (!normalizeManifestObject(root, protocol, error, &injected)) {
            AF_LOGE("setManifestForProtocol(%s): 对象不完整 —— %s\n",
                    protocol.toUtf8().constData(), error.toUtf8().constData());
            setErrorString(error);
            return false;
        }

        if (!name.trimmed().isEmpty()) {
            m_manifestHint = name.trimmed();
        }

        AF_LOGI("setManifestForProtocol(%s): 校验通过，补全 %d 个字段%s\n",
                protocol.toUtf8().constData(), injected,
                name.isEmpty() ? "" : ("，名字 " + name).toUtf8().constData());

        return setManifestJson(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    }

    bool CicadaPlayerItem::setManifestJson(const QString &json)
    {        if (json.trimmed().isEmpty()) {
            AF_LOGW("setManifestJson: 空 JSON，忽略\n");
            return false;
        }

        /* 先自检一下是不是合法 JSON：否则错误要等到框架里才报，很难定位。 */
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &parseError);

        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            AF_LOGE("setManifestJson: JSON 解析失败 (%s at %d)\n",
                    parseError.errorString().toUtf8().constData(), parseError.offset);
            setErrorString(tr("流清单不是合法的 JSON：%1").arg(parseError.errorString()));
            return false;
        }

        const QJsonObject root = doc.object();
        const int videoCount = root.value(QStringLiteral("video")).toArray().size();

        if (videoCount == 0) {
            AF_LOGW("setManifestJson: 清单里没有 video[]，框架可能拒绝打开\n");
        }

        m_manifestJson = json;

        /*
         * 协议标签：清单对象走哪条分片管线由 mediaSourceType 决定（核心默认 hls）；
         * 拿不到就退回按文件名/提示（manifestHint）判断，最后缺省 DASH。
         */
        {
            QString proto = root.value(QStringLiteral("mediaSourceType")).toString().trimmed().toLower();

            if (proto != QLatin1String("dash") && proto != QLatin1String("hls")) {
                const QString hint = m_manifestHint.toLower();

                if (hint.contains(QLatin1String("hls")) || hint.contains(QLatin1String("m3u8"))) {
                    proto = QStringLiteral("hls");
                } else {
                    proto = QStringLiteral("dash");
                }
            }

            const QString label = (proto == QLatin1String("hls")) ? QStringLiteral("HLS")
                                                                  : QStringLiteral("DASH");
            setProtocol(label, protocolDetailFor(label, QString(), true));
        }

        /*
         * 清单和普通地址互斥：清掉 source（同时补发 sourceChanged，界面上的文件名之类
         * 才不会显示成上一个片源）。下面这段必须放在 destroyPlayer() 之前 ——
         * createPlayer() 要靠 m_manifestJson 决定走哪条 SetDataSource。
         */
        const bool hadSource = !m_source.isEmpty();
        m_source = QUrl();

        if (hadSource) {
            emit sourceChanged();
        }

        emit streamingChanged();
        /* 清单对象也属于"有片源"（hasMedia 的判据是地址或清单对象） */
        emit hasMediaChanged();

        AF_LOGI("opening manifest: %d 个视频档, %lld 字节 JSON\n",
                videoCount, static_cast<long long>(json.size()));

        /* 和换 URL 一样：先把旧的停掉，再按新片源建一个，然后交给场景图就绪那套流程。 */
        destroyPlayer();
        m_playerStarted = false;
        m_firstFrameEmitted = false;
        m_qualities.clear();
        m_qualityIndex = -1;
        emit qualitiesChanged();
        emit qualityChanged();
        setErrorString(QString());
        setStatus(Loading);

        createPlayer();

        if (m_sceneGraphReady) {
            startPlaybackWhenReady();
        }

        return true;
    }

    void CicadaPlayerItem::selectQuality(int streamIndex)
    {
        if (m_player == nullptr) {
            return;
        }

        /* 切换中再次点击时允许“最新请求覆盖旧请求”。核心会取消旧 pending
         * decoder、保留 active decoder 和当前画面，不会让多个目标同时进入渲染链路。 */

        /*
         * 参数既接受 qualities 里真正的 streamIndex，也接受**数组下标**（界面通常手里
         * 就只有下标）——所以先按下标查一遍：命中了就用它的 streamIndex。
         */
        int target = streamIndex;

        if (streamIndex >= 0 && streamIndex < m_qualities.size()) {
            target = m_qualities.at(streamIndex).toMap().value(QStringLiteral("streamIndex"),
                                                               streamIndex).toInt();
        }

        /*
         * 框架语义（mediaPlayer/MediaPlayer.cpp:286 SelectTrack）：
         *   index == SELECT_TRACK_VIDEO_AUTO(-1) → EnableAbr(true)；
         *   index >= 0                            → CicadaSwitchStreamIndex()，
         *                                           **并且视频流切换会把 ABR 关掉**。
         * 所以这里的 m_autoQuality 只是把框架的行为反映到界面上，不是我们自己发明的状态。
         */
        m_player->SelectTrack(target);
        /* m_qualityIndex 不在点击瞬间修改；只有核心确认新帧已经渲染后，
         * notifyStreamSwitched() 才更新它并发出 qualityChanged。 */
        m_autoQuality = false;

        AF_LOGI("selectQuality: index=%d (streamIndex=%d)，ABR 已由框架关闭\n",
                streamIndex, target);
    }

    void CicadaPlayerItem::useAutoQuality()
    {
        if (m_player == nullptr) {
            return;
        }

        /* 自动档也遵循最新请求优先；旧 pending 会由公共核心安全取消。 */

        m_player->SelectTrack(SELECT_TRACK_VIDEO_AUTO);
        m_autoQuality = true;
        /* 自动档 UI 同样等 READY/StreamChanged 后再刷新，避免菜单先变而画面未变。 */

        AF_LOGI("useAutoQuality: 回到自动清晰度（ABR）\n");
    }

    void CicadaPlayerItem::setInitialBandwidth(int bitsPerSecond)
    {
        m_initialBandwidth = bitsPerSecond < 0 ? 0 : bitsPerSecond;

        /*
         * 框架里 SetDefaultBandWidth 只在准备阶段被读（挑最接近的那一档起播），
         * 所以运行中改它不会立刻生效 —— 换片源/重开才会用上新值。
         */
        if (m_player != nullptr && m_initialBandwidth > 0) {
            m_player->SetDefaultBandWidth(m_initialBandwidth);
        }
    }

    void CicadaPlayerItem::notifyQualities(const QVariantList &qualities)
    {
        m_qualities = qualities;

        /*
         * 列表刷新时顺手问一下框架"当前是哪一档"，让界面初次显示就落在正确的高亮上。
         * GetCurrentStreamInfo 只有在 prepare 之后才有意义，拿不到就保持 -1。
         */
        m_qualityIndex = -1;

        if (m_player != nullptr) {
            if (const StreamInfo *current = m_player->GetCurrentStreamInfo(ST_TYPE_VIDEO)) {
                for (int i = 0; i < m_qualities.size(); ++i) {
                    /* 认行按"streamIndex 或分辨率"（见 sameQualityRow：同一档可能有多条流）。 */
                    if (sameQualityRow(m_qualities.at(i).toMap(), current->streamIndex,
                                       current->videoWidth, current->videoHeight)) {
                        m_qualityIndex = i;
                        break;
                    }
                }
            }
        }

        AF_LOGI("qualities: %lld 档，当前下标 %d\n",
                static_cast<long long>(m_qualities.size()), m_qualityIndex);

        /*
         * 【2026-09-21 新增：自动档必须在**起播时**就把框架的 ABR 打开】
         *
         * 组件默认就是"自动清晰度"（m_autoQuality = true），但框架的 ABR 只有在
         * `SelectTrack(SELECT_TRACK_VIDEO_AUTO)` 之后才会 `mAbrManager->EnableAbr(true)`
         * （mediaPlayer/MediaPlayer.cpp:292）。不调这一步的后果是：
         *   * 框架按 `mSet->mDefaultBandWidth`（默认 0）去挑"离 0 最近"的那一档，
         *     也就是码率**最低**的一档 —— 这就是"自动档永远从最糊的那档起播"；
         *   * 此后 ABR 线程根本不会跑（AbrManager::AbrAdjustFun 里 `mEnableAbr`
         *     为假就直接返回），所以再快的网络也不会自己升上去。
         *
         * 界面上的"自动"和框架的"自动"必须是同一个状态，所以清单/清晰度列表一
         * 就绪（说明 prepare 完成、ABR 需要的 mBitRates 也已经注册好）就补这一次
         * 调用。用户手动点过某一档之后 m_autoQuality 变 false，这里不会再把 ABR
         * 打开（框架在手动切流时也会自己关掉它）。
         */
        if (m_player != nullptr && m_autoQuality && !m_qualitySwitching) {
            m_player->SelectTrack(SELECT_TRACK_VIDEO_AUTO);
            AF_LOGI("auto quality: framework ABR enabled (auto mode)\n");
        }

        emit qualitiesChanged();
        emit qualityChanged();
    }

    void CicadaPlayerItem::notifyStreamSwitched(int streamType, int streamIndex,
                                                int width, int height, int bandwidth)
    {
        if (streamType != ST_TYPE_VIDEO) {
            /* 音轨/字幕轨的切换先只记日志，清晰度界面不受影响。 */
            AF_LOGI("stream switched: type=%d index=%d\n", streamType, streamIndex);
            return;
        }

        for (int i = 0; i < m_qualities.size(); ++i) {
            /* 同上：ABR 切到同分辨率的孪生流（另一套编码）也算停在这一档。 */
            if (sameQualityRow(m_qualities.at(i).toMap(), streamIndex, width, height)) {
                m_qualityIndex = i;
                break;
            }
        }

        AF_LOGI("stream switched: video index=%d %dx%d %dkbps (auto=%d)\n",
                streamIndex, width, height, bandwidth / 1000, m_autoQuality ? 1 : 0);

        emit qualityChanged();
        /*
         * autoMode 用当前状态表示"这次是 ABR 自己切的还是用户点的"：手动切完
         * m_autoQuality 已经是 false，ABR 切流时它保持 true。界面据此决定提示文案。
         */
        emit qualitySwitched(streamIndex, width, height, bandwidth, m_autoQuality);
    }

    void CicadaPlayerItem::notifyQualitySwitchStatus(int status, int streamIndex, const QString &description)
    {
        m_qualitySwitching = (status == PLAYER_QUALITY_SWITCH_STARTED);
        AF_LOGI("quality switch status=%d stream=%d: %s\n", status, streamIndex,
                description.toUtf8().constData());
        emit qualitySwitchStatusChanged(status, streamIndex, description);
        if (status != PLAYER_QUALITY_SWITCH_STARTED) {
            /* 失败/取消也要让控制栏恢复可点击；当前清晰度保持旧值。 */
            emit qualityChanged();
        }
    }

    void CicadaPlayerItem::issueSeek(qint64 positionMs)
    {
        if (m_player == nullptr) {
            return;
        }

        m_seekInFlight = true;
        /*
         * 位置地板**不再由端侧维护**（本轮收敛到内核一处）：
         * 内核 SuperMediaPlayer 会在 seek 一发起就把上报位置（不连续点的目标点）
         * 钉在目标点，直到管道真的走到它 —— 端侧再挡一次是重复实现。
         * 这里只做"乐观地把目标立刻报给界面"，避免松手瞬间闪回旧位置。
         */
        if (m_position < positionMs) {
            m_position = positionMs;
            emit positionChanged();
        }

        /* 终态由框架事件驱动（Seeking / SeekEnd），这里不再起任何看门狗。 */
        m_player->SeekTo(positionMs, SEEK_MODE_ACCURATE);
    }

    void CicadaPlayerItem::finishSeek()
    {
        m_seekInFlight = false;

        if (m_seekPending >= 0) {
            const qint64 next = m_seekPending;
            m_seekPending = -1;
            issueSeek(next);
        }
    }

    void CicadaPlayerItem::notifySeekStarted()
    {
        /*
         * 框架说"开始处理这次 seek 了"。状态本来就在 issueSeek() 里抢跑了（乐观置位），
         * 这里只做日志 —— 抢跑是必须的：从 SeekTo 到这条回调之间还有一个消息队列的
         * 往返，那段时间里如果又不置位，高频拖动照样会堆一串 seek 进去。
         */
        AF_LOGD("the player started seeking\n");
    }

    void CicadaPlayerItem::notifySeekFinished()
    {
        /* seek 结束是又一个"时长可能刚变成已知"的点（尤其是流媒体） */
        refreshDuration();
        finishSeek();
    }

    /* onSeekWatchdog 已删除（本轮）：seek 终态只由框架的 Seeking / SeekEnd 事件驱动。 */

    /* ------------------------------------------------------------------ */
    /* 播放器生命周期                                                     */
    /* ------------------------------------------------------------------ */

    void CicadaPlayerItem::componentComplete()
    {
        QQuickItem::componentComplete();

        QQuickWindow *w = window();

        if (w != nullptr && !m_sceneGraphConnected) {
            m_sceneGraphConnected = true;
            /*
             * sceneGraphInitialized 是在**渲染线程**上发出的，所以必须用直连
             * （DirectConnection）：我们要在那个线程上问 Qt 要 GPU 设备句柄。
             */
            connect(w, &QQuickWindow::sceneGraphInitialized, this,
                    &CicadaPlayerItem::onSceneGraphInitialized, Qt::DirectConnection);
            connect(w, &QQuickWindow::sceneGraphInvalidated, this,
                    &CicadaPlayerItem::onSceneGraphInvalidated, Qt::DirectConnection);
        }

        /* 清单对象片源时 m_source 是空的（见 createPlayer 的说明），要一起判断。 */
        if ((!m_source.isEmpty() || !m_manifestJson.isEmpty()) && m_player == nullptr) {
            createPlayer();
        }
    }

    void CicadaPlayerItem::createPlayer()
    {
        /*
         * 【必须把"清单对象"这条片源算进来】清单对象播放时 m_source 是**空的**
         * （setManifestJson 会主动清掉 source，两种片源互斥），所以只判断
         * m_source.isEmpty() 会让这里**直接返回、播放器根本不建** ——
         * 现象就是用户报的"清单对象传进去没反应"：JSON 解析通过了、
         * 归一化日志也打了，但既没有 Prepared 也没有画面（日志里
         * `opening manifest: 8 个视频档` 之后不会再有任何播放器日志）。
         */
        if (m_player != nullptr || (m_source.isEmpty() && m_manifestJson.isEmpty())) {
            return;
        }

        /*
         * 注意：**不在这里**打开解码纹理直通（video.render.hw.direct_texture）。
         *
         * 那个开关一打开，解码器就不再下载画面，交出来的是原生 GPU 句柄（D3D11 纹理 /
         * VAAPI surface），CPU 回退路径根本处理不了（把纹理指针当像素数据用会直接崩）。
         * 所以必须等场景图初始化、纹理后端确认"零拷贝真的可用"之后再打开 —— 见
         * startPlaybackWhenReady()。
         */

        m_player.reset(new MediaPlayer());

        /* 监听回调：准备完成、尺寸变化、首帧、结束、错误、位置。 */
        m_listener.reset(new playerListener());
        memset(m_listener.get(), 0, sizeof(playerListener));
        m_listener->userData = this;
        m_listener->Prepared = onPreparedCb;
        m_listener->Completion = onCompletionCb;
        m_listener->FirstFrameShow = onFirstFrameCb;
        m_listener->VideoSizeChanged = onVideoSizeCb;
        m_listener->ErrorCallback = onErrorCb;
        m_listener->PositionUpdate = onPositionCb;
        m_listener->BufferPositionUpdate = onBufferPositionCb;
        /*
         * 截屏结果（进度条悬停预览）：requestSnapshot() -> 框架 CaptureScreen() ->
         * 渲染器截一帧 -> 这个回调。语义/像素格式/线程约定见 onCaptureScreenCb 的注释。
         * **这张表以前没有它**，所以框架截完图没人接（回调压根不会到应用层）。
         */
        m_listener->CaptureScreen = onCaptureScreenCb;
        /* seek 的开始/结束：组件的"单飞 + 最新优先"就靠这两个回调（见 seek()）。 */
        m_listener->Seeking = onSeekingCb;
        m_listener->SeekEnd = onSeekEndCb;
        m_listener->StatusChanged = onStatusChangedCb;
        /* 流媒体：清晰度列表（MediaInfoGet）与切流完成（StreamSwitchSuc）。 */
        m_listener->MediaInfoGet = onMediaInfoGetCb;
        m_listener->StreamSwitchSuc = onStreamSwitchSucCb;
        m_listener->VideoQualitySwitch = onVideoQualitySwitchCb;
        /*
         * 缓冲状态 + 下载速度：见 onLoadingStartCb 的说明。
         * 这两条以前完全没接，所以"视频卡住、弹幕照滚、界面无提示"。
         */
        m_listener->LoadingStart = onLoadingStartCb;
        m_listener->LoadingEnd = onLoadingEndCb;
        m_listener->LoadingProgress = onLoadingProgressCb;
        m_listener->CurrentDownLoadSpeed = onDownloadSpeedCb;
        m_player->SetListener(*m_listener);
        m_listenerAttached = true;

        /*
         * 视频帧改由 CICADA_QT_ITEM 这套组件自己接管：
         * 注册回调之后，帧会先到我们的 CicadaVideoRender（框架侧注入的渲染器），
         * 再由它调用这里注册的函数（在框架 VSync 线程上）。
         */
        m_player->SetVideoRenderingCallback(
        [](void *userData, IAFFrame *frame, const CicadaJSONItem &params) -> bool {
            (void) params;
            auto *item = static_cast<CicadaPlayerItem *>(userData);

            if (item == nullptr || frame == nullptr) {
                return false;
            }

            return item->handleVideoFrame(frame);
        }, this);

        /* 硬解开关由 QML 侧的设置页控制；解不出来时框架会自己退回软解。 */
        m_player->EnableHardwareDecoder(m_hardwareDecoding);
        m_player->SetAutoPlay(false); /* 由组件在设备就绪后统一 Start() */

#if defined(Q_OS_MACOS)
        /*
         * macOS：要求 VideoToolbox 直接输出 **32BGRA** 的 CVPixelBuffer。
         *
         * 原因见 CicadaTextureMetal.h：Qt 的默认材质只能采样"一张普通纹理"，
         * 双平面 NV12 需要两张纹理 + 自定义着色器才能合成，所以这里让解码器多做
         * 一步格式转换（在 VideoToolbox 内部完成，仍然不落内存、不经 CPU），
         * 换来的是 Qt 侧一条干净的零拷贝路：CVPixelBuffer -> MTLTexture(IOSurface)
         * -> QSGTexture。
         *
         * SetOption 必须在 Prepare() 之前调用：框架在建立视频解码器时会读它
         * （SuperMediaPlayer.cpp 里 dstFormat = mSet->pixelBufferOutputFormat）。
         */
        m_player->SetOption("pixelBufferOutputFormat",
                            QString::number(static_cast<qulonglong>(kCVPixelFormatType_32BGRA))
                            .toUtf8().constData());
#endif

        /*
         * 片源分两条路：
         *   * 清单对象（setManifest/setManifestJson）→ 直接把 JSON 文本交给框架的
         *     `SetDataSource(const std::string &jsonManifest)` 重载。**必须传 std::string**
         *     而不要传 const char*：那样会命中 `SetDataSource(const char *url)` 那个重载，
         *     框架会把一大段 JSON 当 URL 去打开。
         *   * 普通地址：本地文件转成普通路径，网络地址原样交给框架。
         */
        if (!m_manifestJson.isEmpty()) {
            m_player->SetDataSource(std::string(m_manifestJson.toUtf8().constData()));
        } else {
            const QString localPath = m_source.isLocalFile() ? m_source.toLocalFile()
                                      : m_source.toString();
            m_player->SetDataSource(localPath.toUtf8().constData());
        }

        /*
         * 【DASH / HLS（分片流）：缓冲上限从框架默认的 40 秒收到 15 秒】
         *
         * 用户要求："dash 和 hls 的缓冲不要缓冲太多，不然切换清晰度后缓冲的都直接没用了，
         * 节省网络资源和内存"。
         *
         * 为什么分片流不该留 40 秒：
         *   1. **换档就作废**：DASH/HLS 换 Representation 之后，旧档读进来的包
         *      （按旧档的码率、旧档的分片号）全都要丢掉 —— 框架日志里那句
         *      `dropped 2761 stale video packets of the retired stream at commit`
         *      就是它。缓冲越厚，这一下白扔的越多（40 秒 × 4K 10Mbps ≈ 50MB 白下）；
         *   2. **内存**：包缓冲的常驻内存 = 码率 × 秒数。4K 10Mbps：40s ≈ 50MB、
         *      15s ≈ 19MB；1080p 5Mbps：40s ≈ 25MB、15s ≈ 9MB；
         *   3. **分片流根本不需要那么厚**：分片是**可以按需边下边播**的离散单元
         *      （不像 rtsp 那种连续流），15 秒 = 1.5 个 10 秒分片（或 ~3.6 个
         *      4.17 秒分片），足够吃掉一次网络抖动；真断粮了框架本来就会进缓冲态。
         *
         * ⚠ 15 秒是**和 ABR 的阈值配套**的（mediaPlayer/abr/AbrBufferAlgoStrategy.cpp：
         *   上切要求缓冲 ≥ 8 秒、连续低于 7 秒才降档）—— 改成别的值时要一起看，
         *   否则会出现"永远够不到上切门槛"（上限低于 8 秒）或者"降档太迟钝"。
         *
         * `highLevelBufferDuration`（5 秒）是"起播缓冲结束"的水位，也是进度条进度的基准；
         * `startBufferDuration`（1 秒）是起播前至少攒多少 —— 两个都保持稳妥值。
         *
         * 单位是**毫秒**（SuperMediaPlayer.cpp 收到之后 ×1000 转微秒），
         * 并且必须在 Prepare() 之前调用 —— 和上面 pixelBufferOutputFormat 同一个道理。
         */
        const bool segmentStream = !m_manifestJson.isEmpty() || isSegmentPlaylistUrl(m_source);

        if (segmentStream) {
            m_player->SetOption("maxBufferDuration", "15000");
            m_player->SetOption("highLevelBufferDuration", "5000");
            m_player->SetOption("startBufferDuration", "1000");
            AF_LOGI("segment stream (DASH/HLS): buffer capped at 15s "
                    "(maxBuffer=15000ms high=5000ms start=1000ms) to avoid re-downloading "
                    "a thick buffer after every quality switch\n");
        }

        /*
         * 【本地文件：把预读缓冲收小 —— 这是内存占用最大的一笔可省项】
         *
         * 框架默认是"最多缓冲 40 秒"：
         *     mediaPlayer/player_types.cpp:16-18
         *         START_BUFFER_DURATION_DEFAULT  = 1 秒起播
         *         HIGH_BUFFERING_LEVEL_DEFAULT   = 5 秒高水位
         *         MAX_BUFFER_DURATION_DEFAULT    = 40 秒上限
         * 40 秒的**压缩包**缓冲对本地文件是纯浪费：磁盘随手就能读到，主流播放器读本地
         * 只留几秒。实测这一项的常驻内存 = 码率 × 秒数：
         *     1080p 15Mbps → 40s ≈ 74MB（实测播放峰值比空载 +69MB）
         *     4K    57Mbps → 40s ≈ 285MB（实测 +190MB，采样时还没填满 40 秒）
         * 收成 5 秒上限之后，同一素材只需要 1/8 的包缓冲。
         *
         * 其余网络源（普通 http(s) 的 mp4、rtmp…）保持框架默认 40 秒：那是**连续流**，
         * 缓冲厚一点才能抗抖动，而且它没有"换档作废"这个问题。
         */
        const bool localFileSource = m_manifestJson.isEmpty() && m_source.isLocalFile();

        if (localFileSource) {
            m_player->SetOption("maxBufferDuration", "5000");
            m_player->SetOption("highLevelBufferDuration", "3000");
            m_player->SetOption("startBufferDuration", "500");
        }

        /*
         * =====================================================================
         * 【FLV 直播（HTTP-FLV）：缓冲必须收得很小，否则"直播"会变成"延时 40 秒的录像"】
         *
         * 直播和点播的缓冲目标**正好相反**：
         *   * 点播要厚 —— 抗抖动，卡一下无所谓；
         *   * 直播要薄 —— 缓冲有多厚，你看到的画面就比主播晚多久。框架默认上限 40 秒
         *     （player_types.cpp: MAX_BUFFER_DURATION_DEFAULT = 40s），照默认值跑
         *     就是"延迟 40 秒的直播"，体验完全不对。
         *
         * 这里给 4 秒上限 / 2 秒高水位 / 0.5 秒起播：
         *   * 0.5 秒起播 = 秒开（配合 mock 那边"新客户端补发最近一个 GOP"）；
         *   * 2 秒高水位 = 正常播放时的稳态延迟量级（本地/局域网实测 1~2 秒）；
         *   * 4 秒上限 = 网络一抖就进缓冲态重连式追赶，但不会越积越多。
         *
         * 【谁来告诉我们是直播】QML 那边（LivePlayerView）在设 source **之前** 写
         * `player.liveMode = true`。为什么不自己按 URL 猜：`.flv` 既可能是直播也可能是
         * 点播文件（本地测试服务里 /live/ 下的 *.flv 是直播、/vod/sample.flv 是点播），
         * 靠后缀猜必然有一半是错的 —— 让**知道真相的那一层**明确告诉组件。
         *
         * ⚠ 这里**故意不设** RTMaxDelayTime（框架里那套"直播追帧/丢旧缓冲"就靠它开启，
         *   默认 0 = 关闭）。原因：它是给 HLS/DASH 写的，内部会走 LiveCatchUp/LiveTimeSync，
         *   而 LiveTimeSync 用到 mUtcTimer（FLV 直播不会创建 UTC 定时器）——
         *   在没实测过的情况下打开一个"可能空指针"的分支，风险大于收益。
         *   要做"直播低延迟追赶"应该单独一轮，把 avFormatDemuxer 的 UTC 时间同步一起做。
         * =====================================================================
         */
        /*
         * 【★只对"连续流"（HTTP-FLV 这类）收薄，分片流（HLS/DASH）不碰★】
         *
         * 这是上一版的一个**真错误**：只要 QML 置了 liveMode，不管是 FLV 直播还是 HLS 直播
         * 都套上"4 秒上限 / 2 秒高水位 / 0.5 秒起播"。但 **HLS 是按分片播的**：
         *   * 分片时长由**源视频的关键帧间隔**决定，`-c copy` 时 ffmpeg 只能在关键帧处切，
         *     所以素材是 5 秒关键帧时，分片就是 **5 秒一片**（实测用户的片子正是这样）；
         *   * 4 秒的缓冲上限**比一个分片还短**，分片流于是"永远攒不够可播数据" ——
         *     核心里就是每 77ms 重试一次（`HLSStream: open_internal again` 刷屏），
         *     一片都放不出来。
         * 所以：**分片流（segmentStream，也就是 .m3u8/.mpd/清单对象）一律不套薄缓冲** ——
         * 它有自己的 15 秒策略（上一段），那是按"分片"设计的、也验证过；
         * 薄缓冲只给真正的连续流（HTTP-FLV / rtmp 这类）用 —— 那是它该有的语义
         * （连续流的缓冲厚度 = 延迟）。
         */
        if (mLiveMode && !segmentStream) {
            m_player->SetOption("maxBufferDuration", "4000");
            m_player->SetOption("highLevelBufferDuration", "2000");
            m_player->SetOption("startBufferDuration", "500");
            AF_LOGI("live continuous stream (HTTP-FLV/RTMP): buffer capped at 4s "
                    "(maxBuffer=4000ms high=2000ms start=500ms) so that live latency "
                    "stays around 1-2s instead of the 40s default\n");
        } else if (mLiveMode && segmentStream) {
            /*
             * 直播分片流：保持上面 segmentStream 那套（15s/5s/1s），只把日志写清楚，
             * 免得以后看日志的人以为"直播怎么没走薄缓冲"是漏了。
             */
            AF_LOGI("live segment stream (HLS/DASH): keeping the 15s segment buffer "
                    "(a thinner buffer than one segment would starve playback)\n");
        }

        /*
         * 起播档位：必须在 Prepare() 之前设（框架在准备阶段按带宽挑最接近的那一档）。
         * 0 = 用框架默认（最低档起播）。
         */
        if (m_initialBandwidth > 0) {
            m_player->SetDefaultBandWidth(m_initialBandwidth);
        }

        m_player->SetVolume(static_cast<float>(m_volume));
        m_player->SetMute(m_muted);
        m_player->SetLoop(m_loop);
    }

    void CicadaPlayerItem::destroyPlayer()
    {
        /*
         * seek 的单飞状态跟着播放器生命周期走：播放器没了（换片源、切硬解、析构），
         * 上一个 seek 的 SeekEnd 永远不会来了，不清掉的话新播放器上的 seek 会被
         * m_seekInFlight 一直挡住（"换完片子之后进度条就拖不动了"）。
         */
        m_seekInFlight = false;
        m_seekPending = -1;

        /* 统计信息定时器跟着播放器一起停：没有播放器就没有读数可刷。 */
        if (m_statsTimer != nullptr) {
            m_statsTimer->stop();
        }

        /* 统计面板的值清空（面板还开着时显示成空档，而不是上一条片源的旧数字）。 */
        m_manifestVideoCodecs.clear();
        m_manifestAudioCodecs.clear();
        m_manifestVideoHost.clear();
        m_manifestAudioHost.clear();

        if (!m_stats.isEmpty()) {
            m_stats.clear();
            emit statsChanged();
        }

        /* 缓冲位置跟着这一条片源走：换片源/析构后进度条上的缓冲段要归零。 */
        if (m_bufferedPosition != 0) {
            m_bufferedPosition = 0;
            emit bufferedPositionChanged();
        }

        /*
         * 播放器（连同它的解码器和表面池）马上要被销毁，但渲染线程手里还留着上一帧和
         * D3D11 的输入视图 —— 这两个引用会让上一个解码器的整池表面纹理回收不了
         * （原因写在 CicadaPlayerItem.h 里 m_releaseRenderState 那段）。置个标志，
         * 让渲染线程在它自己的线程上放掉。
         *
         * 放在 m_player == nullptr 这个提前返回**之前**：没有播放器时同样可能还留着
         * 上一个片源的池（例如 setSource(QUrl()) 走到这里时 player 已经没了）。
         *
         * update() 保证标志被及时取走（暂停/停止后本来就没有新帧、不会自动重绘）；
         * 从 GUI 线程调 update() 是 Qt 允许的，本文件里 setHardwareDecoding()
         * 就是"置标志 + update()"的同一套写法。
         */
        m_releaseRenderState = true;
        update();

        if (m_player == nullptr) {
            return;
        }

        /*
         * Stop() 会停掉解码/渲染线程；析构则保证框架不再调用我们的回调。
         * 顺序：先清回调（防止析构过程里还有帧回调进来），再销毁。
         */
        m_player->SetVideoRenderingCallback(nullptr, nullptr);
        m_player->Stop();
        m_player.reset();
        m_listener.reset();
        m_listenerAttached = false;

        /* 清掉挂起的帧，避免渲染线程去访问已经释放的解码器缓冲。 */
        QMutexLocker locker(&m_frameMutex);
        m_pendingFrame.reset();
    }

    void CicadaPlayerItem::startPlaybackWhenReady()
    {
        if (m_player == nullptr || m_playerStarted) {
            return;
        }

        m_playerStarted = true;

        /*
         * 只有纹理后端确认零拷贝可用时，才让解码器交出原生 GPU 帧
         * （"video.render.hw.direct_texture"，见 property.h）。
         *
         * 为什么顺序是这样：这个全局设置由解码器在 Prepare() 里读取，而"零拷贝是否
         * 可用"要到场景图初始化（onSceneGraphInitialized -> prepare()）才知道；
         * 所以本函数是在那之后才被调用的（见 onSceneGraphInitialized 的队列调用）。
         *
         * 反过来的代价很大：开关打开但后端不可用时，解码器交出来的 GPU 帧既不能下载
         * 也不能转换，画面会卡在最后一帧。所以宁可退回 copy-back（慢，但一定能播）。
         */
        /*
         * 硬解关掉时，解码器交出来的是内存里的 CPU 帧，零拷贝无从谈起：这时**不能**
         * 打开直通开关（打开了也没有 GPU 纹理可交，只会让 CPU 路径反复拒绝这些帧）。
         */
        const bool zeroCopy = m_hardwareDecoding && (m_textureBackend != nullptr) &&
                              m_textureBackend->isZeroCopy();

        if (zeroCopy) {
            /*
             * 注意这里改的是**框架的全局设置**，不是 QObject 的动态属性。
             *
             * 曾经写成裸的 setProperty(key, "ON")，那是 QObject::setProperty()：只在
             * 本对象上挂了个动态属性，解码器根本看不到 —— 表现是"日志说零拷贝可用，
             * 但一直走 copy-back"，非常难查。正确的入口是
             * globalSettings::getSetting().setProperty()（解码器读的就是这里）。
             *
             * 属性是进程级的（globalSettings 是单例），在线程 A 设、线程 B 读没问题；
             * 只有 "protected." 开头的键才绑定线程。
             */
            Cicada::globalSettings::getSetting().setProperty(PROPERTY_KEY_HW_DIRECT_TEXTURE, "ON");
            AF_LOGI("zero-copy presentation is available: asking the decoder for raw GPU "
                    "frames (%s)\n", m_textureBackend->backendName().toUtf8().constData());
        } else {
            /*
             * 明确写回 "OFF"：这个键是进程级的，上一次播放（或者别的组件）可能把它设成
             * "ON" 过。留着 "ON" 而后面又走 CPU 路径的话，解码器会交出没下载的 GPU 帧，
             * CPU 路径处理不了（只能保持上一帧不动 —— 画面会卡住）。
             */
            Cicada::globalSettings::getSetting().setProperty(PROPERTY_KEY_HW_DIRECT_TEXTURE, "OFF");
            AF_LOGW("zero-copy presentation is not available on this setup: the player uses "
                    "the copy-back path (hardware decoding is still on, the frames are just "
                    "downloaded to memory)\n");
        }

        setStatus(Loading);
        m_player->Prepare();
    }

    void CicadaPlayerItem::onSceneGraphInitialized()
    {
        /*
         * **这个函数在渲染线程上执行**（sceneGraphInitialized 是渲染线程发出的）。
         *
         * 两件事必须在这里做，因为它们都要求"渲染线程 + 场景图已就绪"：
         *   1. 把 Qt 的 GPU 设备交给 FFmpeg（Windows：ID3D11Device）—— 零拷贝的前提；
         *   2. 准备纹理后端（探测零拷贝是否可用，拿 RHI 设备）。
         *
         * 完成之后再回到主线程去 Prepare()：解码器初始化发生在 Prepare 里，那时设备
         * 必须已经交给框架了。
         */
        QQuickWindow *w = window();
        CicadaHardwareDevice::instance().captureFromSceneGraph(w);

        if (m_textureBackend) {
            /*
             * 初始决策（走零拷贝还是 CPU 路径）也在渲染线程上做 —— 探测要拿 Qt 的 RHI
             * 设备、建 GPU 纹理。允不允许零拷贝由设置页里的硬解开关决定，值就放在
             * m_textureReprobe 里（初值 1 = 允许，和 m_hardwareDecoding 的默认值一致）。
             * 这里把它取走，免得 updatePaintNode 再做一次无谓的重算。
             */
            const int wanted = m_textureReprobe.exchange(-1);
            m_textureBackend->reset(w, wanted != 0);

            /*
             * 后端刚（重新）探测/重建过：把当前的色彩调整值补一遍。
             * 少了这一句，换片源/改硬解之后用户拖过的滑块就"自己变回中性"了
             * （处理器是新建的，之前设过的过滤器全没了）。
             */
            m_textureBackend->setColorAdjust(m_colorBrightness, m_colorContrast, m_colorSaturation);
        }

        /* 渲染线程上先把标志置上，随后那次队列调用才会真正 Prepare()。 */
        m_sceneGraphReady = true;

        QMetaObject::invokeMethod(this, [this]() {
            emit backendChanged();
            startPlaybackWhenReady();
        }, Qt::QueuedConnection);
    }

    void CicadaPlayerItem::onSceneGraphInvalidated()
    {
        /* 渲染线程：RHI 资源已经失效，纹理后端持有的东西必须放掉。 */
        if (m_textureBackend) {
            m_textureBackend->releaseResources();
        }

        /*
         * 借给 FFmpeg 的那份场景图设备也要还回去。
         *
         * 【为什么必须还】播放器窗口在本工程里是"关掉就销毁、下次再建"的；窗口一销毁，
         * Qt 就把它那套 RHI 设备放掉了。单例里如果还留着"从旧窗口借来的设备"，下一个
         * 窗口就会出现两种坏结果：解码在旧设备上、显示在新设备上（首帧永远出不来，
         * 界面卡在"正在加载"），或者那个设备已经被释放、AddRef 到已释放对象上（直接闪退）。
         *
         * releaseFromSceneGraph() 只放"从本窗口借的那一份"：首页窗口的场景图被回收时，
         * 不会把播放器窗口正在用的设备顺手放掉（两个窗口同时活着）。
         */
        cicadaqt::CicadaHardwareDevice::instance().releaseFromSceneGraph(window());
    }

    void CicadaPlayerItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
    {
        QQuickItem::geometryChange(newGeometry, oldGeometry);

        /*
         * 尺寸变了要重新画一帧（场景图会在下一次 update() 时调用 updatePaintNode()），
         * 否则窗口拉伸后画面会停在旧尺寸上。
         */
        if (newGeometry.size() != oldGeometry.size()) {
            update();
        }
    }

    /* ------------------------------------------------------------------ */
    /* 帧的交接与绘制                                                     */
    /* ------------------------------------------------------------------ */

    bool CicadaPlayerItem::handleVideoFrame(IAFFrame *frame)
    {
        /*
         * **这个函数在框架的 VSync 线程上执行**，只允许做两件事：
         *   1. clone() 一份帧（只是引用计数 +1，很便宜），交给渲染线程；
         *   2. 请求一次重绘（但不能直接调 update()，见下）。
         *
         * clone() 是必须的：回调返回之后框架就会回收这一帧，而渲染线程可能还在用它。
         * 副本保证了解码器的表面池不会在我们画之前被复用。
         */
        {
            QMutexLocker locker(&m_frameMutex);
            m_pendingFrame = frame->clone();
        }

        /*
         * 请求重绘 —— **必须投递到 GUI 线程再调 update()**。
         *
         * QQuickItem::update() 不是线程安全的（这一点曾经想当然写错过）：Qt 只允许在
         * GUI 线程、或者 updatePaintNode() 里调度重绘，其它线程调它会被直接拒绝并打日志
         *     Updates can only be scheduled from GUI thread or from
         *     QQuickItem::updatePaintNode()
         * 后果是这一帧永远不会被重绘 —— 画面停住或者干脆全黑，而且那条日志每帧刷一次。
         *
         * 队列连接把调用投递到 item 所属线程（GUI 线程）执行，等于把"请求重绘"这件事
         * 变成一次普通的事件派发。item 析构时 Qt 会自动丢掉还没派发的调用，不会有
         * 悬空指针的问题。
         */
        QMetaObject::invokeMethod(this, [this]() { update(); }, Qt::QueuedConnection);
        return true;
    }

    std::unique_ptr<IAFFrame> CicadaPlayerItem::takeFrameForRendering()
    {
        QMutexLocker locker(&m_frameMutex);

        if (m_pendingFrame != nullptr) {
            return std::move(m_pendingFrame);
        }

        /* 没有新帧（窗口刚改变尺寸、暂停中）：沿用当前帧重画一遍。 */
        return m_currentFrame != nullptr ? m_currentFrame->clone() : nullptr;
    }

    QRectF CicadaPlayerItem::fittedRect(bool swapped) const
    {
        const QRectF bounds = boundingRect();

        int videoWidth = m_videoWidth;
        int videoHeight = m_videoHeight;

        if (swapped) {
            /* 旋转 90/270 之后，画面在屏幕上占的宽高比是反过来的。 */
            std::swap(videoWidth, videoHeight);
        }

        if (videoWidth <= 0 || videoHeight <= 0 || bounds.isEmpty()) {
            return bounds;
        }

        /*
         * 等比缩放 + 居中（Scale_AspectFit）：整幅画面都画在 item 里，
         * 短的那一边自然留出黑边。**这是唯一的适配方式** —— 不做任何"放大铺满"，
         * 所以画面永远不会有任何一部分被 item 边界裁掉。
         * 旋转 90/270（swapped = true）走的是同一段代码：调用方传进来的已经
         * 是"旋转之后"的宽高比，这里只处理几何，不碰 extraRotation。
         */
        const qreal videoRatio = static_cast<qreal>(videoWidth) / videoHeight;
        const qreal itemRatio = bounds.width() / bounds.height();

        QRectF result = bounds;

        if (videoRatio <= itemRatio) {
            result.setWidth(bounds.height() * videoRatio);
        } else {
            result.setHeight(bounds.width() / videoRatio);
        }

        result.moveCenter(bounds.center());

        return result;
    }

    QSGNode *CicadaPlayerItem::updatePaintNode(QSGNode *node, UpdatePaintNodeData *data)
    {
        Q_UNUSED(data)

        /*
         * **这个函数在 Qt 的渲染线程上执行**，所有 GPU 相关的活都在这里：
         * 把解码帧变成 QSGTexture，再交给 QSGSimpleTextureNode 绘制。
         */

        /*
         * 硬解开关刚被改过（GUI 线程只在这里挂了个原子量）：趁本帧还没处理，先把
         * "零拷贝还是 CPU 路径"重新算一遍。
         *
         * 为什么必须在这里做：探测要拿 Qt 的 RHI 设备，还可能释放/重建 GPU 资源，
         * 都只能在渲染线程上干（setter 在 GUI 线程）。
         *
         * 为什么放在处理帧之前：重开片源后第一帧可能已经是 GPU 纹理了，若按旧结论
         * （比如"上次是 CPU 路径"）去处理，这一帧会被当成不认识的格式丢掉。
         */
        const int reprobe = m_textureReprobe.exchange(-1);

        if (reprobe >= 0 && m_textureBackend != nullptr) {
            QQuickWindow *probeWindow = window();

            if (probeWindow != nullptr) {
                m_textureBackend->reset(probeWindow, reprobe != 0);

                /* 诊断文字绑在 backendChanged 上，切了路径要让界面跟着更新（回 GUI 线程发）。 */
                QMetaObject::invokeMethod(this, [this]() {
                    emit backendChanged();
                }, Qt::QueuedConnection);

                /*
                 * 重算完了才能重开片源：startPlaybackWhenReady() 就是拿这个结论去决定
                 * 开不开"解码纹理直通"的（顺序反了的后果见 setHardwareDecoding）。
                 */
                if (m_reloadPending.exchange(false)) {
                    QMetaObject::invokeMethod(this, [this]() {
                        reloadCurrentSource();
                    }, Qt::QueuedConnection);
                }
            }
        }

        /*
         * 换片源/停播时（见 destroyPlayer）：把渲染线程手里的上一帧和 D3D11 输入视图
         * 放掉，否则上一个解码器的整池表面纹理回收不了。
         *
         * 为什么一个引用就够钉住整池：FFmpeg 的 d3d11va 把池子里所有 surface 放在
         * **同一个** ID3D11Texture2D 里，每个 surface 的 AVBufferRef 都对整张数组
         * AddRef，所以只要还有一帧活着，整张数组（1080p 约 60MB / 4K 约 250MB）
         * 就释放不掉。输入视图同理（D3D11 的视图对资源持有强引用）。
         *
         * 只能在渲染线程上做：m_currentFrame 只在这个线程被读写，D3D11 资源也只在这里碰。
         * 取走标志用 exchange，不会被处理两遍。
         *
         * 副作用（已知、可接受）：放掉之后如果这一刻还没有新帧，下面
         * takeFrameForRendering() 会返回 nullptr，本 item 删掉节点、暂时不画 ——
         * 也就是换片源/停播时画面区立刻空掉，而不是把上一条片子的最后一帧留在屏幕上，
         * 换片源时本来就该这样。
         *
         * 不影响 isZeroCopy()/直通决策：这里不碰设备、处理器、枚举器和输出纹理，
         * 下一帧 ensureInputView() 会因为输入视图为空把它重建出来。
         */
        if (m_releaseRenderState.exchange(false)) {
            m_currentFrame.reset();

            if (m_textureBackend != nullptr) {
                m_textureBackend->releaseInputState();
            }
        }

        std::unique_ptr<IAFFrame> frame = takeFrameForRendering();

        if (frame != nullptr) {
            m_currentFrame = std::move(frame);
        }

        if (m_currentFrame == nullptr) {
            /* 还没有任何一帧：什么都不画（保持透明，QML 侧可以放背景图/loading）。 */
            delete node;
            return nullptr;
        }

        const int width = m_currentFrame->getInfo().video.width;
        const int height = m_currentFrame->getInfo().video.height;

        QQuickWindow *w = window();

        if (w == nullptr) {
            delete node;
            return nullptr;
        }

        bool flip = false;
        QSGTexture *texture = m_textureBackend->textureForFrame(w, m_currentFrame.get(), &flip);

        if (texture == nullptr) {
            /* 这一帧没能变成纹理（格式不认识之类）：保留上一帧，别把节点删了闪一下。 */
            return node;
        }

        /* 只有新尺寸的纹理已经成功创建并交给场景图节点后，才更新尺寸属性。
         * 这样低清→高清和高清→低清都不会先收到错误的宽高回调。 */
        if (width > 0 && height > 0 && (width != m_videoWidth || height != m_videoHeight)) {
            m_videoWidth = width;
            m_videoHeight = height;
            QMetaObject::invokeMethod(this, [this]() {
                emit videoSizeChanged();
            }, Qt::QueuedConnection);
        }

        /*
         * 旋转：视频自带旋转元数据（手机竖屏视频）+ QML 的 extraRotation 属性。
         *
         * 90/270 度没法只靠纹理坐标变换实现（Qt 的
         * QSGSimpleTextureNode::TextureCoordinatesTransform 只支持镜像），所以用
         * QSGTransformNode 包一层 —— 这是 Qt 场景图自带的节点类型，旋转矩阵由场景图
         * 执行，我们不需要自己写任何着色器/GL 代码。
         *
         * 节点结构：
         *   需要旋转： QSGTransformNode(绕 item 中心旋转)  ->  QSGSimpleTextureNode
         *   不需要  ： QSGSimpleTextureNode
         * 结构变了（例如这一帧开始需要旋转）就把旧节点删掉重建，重建的只是两个空节点。
         */
        const int rotation = effectiveRotation();
        const bool rotateSideways = (rotation == 90 || rotation == 270);

        QSGTransformNode *transformNode = nullptr;
        QSGSimpleTextureNode *textureNode = nullptr;

        if (rotation != 0) {
            transformNode = dynamic_cast<QSGTransformNode *>(node);

            if (transformNode == nullptr) {
                delete node;
                transformNode = new QSGTransformNode();
            }

            if (transformNode->childCount() == 0) {
                auto *child = new QSGSimpleTextureNode();
                /* 让叶子节点负责释放纹理（换纹理或节点销毁时自动 delete 旧的）。 */
                child->setOwnsTexture(true);
                transformNode->appendChildNode(child);
            }

            textureNode = static_cast<QSGSimpleTextureNode *>(transformNode->firstChild());
        } else {
            textureNode = dynamic_cast<QSGSimpleTextureNode *>(node);

            if (textureNode == nullptr) {
                delete node;
                textureNode = new QSGSimpleTextureNode();
                textureNode->setOwnsTexture(true);
            }
        }

        if (textureNode == nullptr) {
            return node;
        }

        textureNode->setTexture(texture);

        /*
         * 纹理坐标变换：上下翻转（OpenGL 纹理原点在左下，需要翻）+ 水平镜像。
         * 两个标志位可以叠加，QSGSimpleTextureNode 支持组合。
         */
        QSGSimpleTextureNode::TextureCoordinatesTransformMode transform =
            QSGSimpleTextureNode::NoTransform;

        if (flip) {
            transform |= QSGSimpleTextureNode::MirrorVertically;
        }

        if (m_mirror) {
            transform |= QSGSimpleTextureNode::MirrorHorizontally;
        }

        textureNode->setTextureCoordinatesTransform(transform);

        /*
         * 几何：
         *   * 不旋转：直接就是等比适配后的矩形；
         *   * 旋转 90/270：先算出"旋转之后"应该占的矩形，再把它转回旋转前的宽高
         *     （绕中心转 90 度会把 A×B 变成 B×A），这样转完正好落在正确位置；
         *   * 旋转 180：矩形不变（还是原来的等比矩形）。
         */
        if (rotateSideways) {
            const QRectF displayed = fittedRect(true);
            QRectF childRect(0, 0, displayed.height(), displayed.width());
            childRect.moveCenter(boundingRect().center());
            textureNode->setRect(childRect);
        } else {
            textureNode->setRect(fittedRect());
        }

        textureNode->setFiltering(QSGTexture::Linear);
        textureNode->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);

        if (transformNode != nullptr) {
            /*
             * 绕 item 中心旋转：先平移到中心，转，再平移回去。
             *
             * translate 的两个重载在 Qt 6 里是 QVector3D 和 (x, y, z)，**没有**
             * translate(QPointF)（QPointF→QVector3D 的转换是 explicit 的，MSVC 会报
             * C2665 找不到重载）。所以这里显式传三个浮点分量。
             */
            const QRectF bounds = boundingRect();
            const QPointF center = bounds.center();
            QMatrix4x4 matrix;
            matrix.translate(static_cast<float>(center.x()),
                             static_cast<float>(center.y()), 0.0f);
            matrix.rotate(static_cast<float>(rotation), 0.0f, 0.0f, 1.0f);
            matrix.translate(static_cast<float>(-center.x()),
                             static_cast<float>(-center.y()), 0.0f);
            transformNode->setMatrix(matrix);
            transformNode->markDirty(QSGNode::DirtyMatrix);
        }

        m_flipVertically = flip;
        m_texturePixelWidth = width;
        m_texturePixelHeight = height;

        if (!m_firstFrameEmitted.exchange(true)) {
            QMetaObject::invokeMethod(this, [this]() {
                emit firstFrameRendered();
                emit backendChanged();
            }, Qt::QueuedConnection);
        }

        return transformNode != nullptr ? static_cast<QSGNode *>(transformNode)
               : static_cast<QSGNode *>(textureNode);
    }

    /* ------------------------------------------------------------------ */
    /* 框架监听回调 -> 主线程通知                                          */
    /* ------------------------------------------------------------------ */

    /*
     * 现取一次时长（框架的 listener 里**没有** DurationUpdate 回调，所以不做定时轮询、
     * 只在"时长可能刚变成已知"的事件点上取：准备好了 / 首帧 / seek 结束）。
     * 直播这类永远拿不到时长的源，duration 保持 0，界面按"未知时长"处理。
     */
    void CicadaPlayerItem::refreshDuration()
    {
        if (m_player == nullptr) {
            return;
        }

        const qint64 duration = m_player->GetDuration();

        if (duration > 0 && duration != m_duration) {
            m_duration = duration;
            emit durationChanged();
        }
    }

    void CicadaPlayerItem::notifyPrepared()
    {
        setStatus(Playing == m_status ? Playing : Ready);

        refreshDuration();

        /*
         * 统计信息从"准备好了"开始刷新（右键菜单 →「视频统计信息」）：
         * 此刻 GetCurrentStreamInfo / GetCurrentStreamMeta 才有值。
         *
         * 顺便把曲线历史清空：换片源了，旧片的读数不能混进新片的曲线里
         * （曲线是"最近 30 秒趋势"，跨片源的时间序列没有意义）。
         */
        parseManifestStatsInfo();
        m_statsHistory.clear();
        emit statsHistoryChanged();
        ensureStatsTimer();

        if (m_autoPlay) {
            play();
        }

        /*
         * 重新打开过的片源要回到原来的位置（改硬解开关走的就是这条路，见
         * setHardwareDecoding）。放在 Start() **之后**：太早调 SeekTo 会被框架当成
         * "还没准备好"处理，位置就丢了。
         */
        if (m_pendingSeekMs > 0 && m_player != nullptr) {
            const qint64 target = m_pendingSeekMs;
            m_pendingSeekMs = 0;
            AF_LOGI("reopened the stream, seeking back to %lld ms\n",
                    static_cast<long long>(target));
            /* 走和用户拖动同一个入口：同样受"单飞 + 最新优先"约束，不会和用户抢。 */
            issueSeek(target);
        }
    }

    void CicadaPlayerItem::notifyCompletion()
    {
        setStatus(End);
        emit finished();
    }

    void CicadaPlayerItem::notifyFirstFrameShown()
    {
        /*
         * 框架的"首帧已显示"：此刻画面已经真的出来了（不是"解出了第一帧"），
         * 适合用来收起 loading 动画。
         */
        if (m_status == Loading || m_status == Ready) {
            setStatus(Playing);
        }

        /* 有些源（流媒体）要开始播之后才报得出总时长，这里补取一次 */
        refreshDuration();

        if (!m_firstFrameEmitted.exchange(true)) {
            emit firstFrameRendered();
        }
    }

    void CicadaPlayerItem::notifyVideoSize(int width, int height)
    {
        if (width <= 0 || height <= 0 || (width == m_videoWidth && height == m_videoHeight)) {
            return;
        }

        m_videoWidth = width;
        m_videoHeight = height;
        emit videoSizeChanged();
        update();
    }

    void CicadaPlayerItem::notifyPosition(qint64 positionMs)
    {
        /*
         * 位置地板只保留内核那一处（本轮收敛完成）：
         * SuperMediaPlayer::getCurrentPosition() 由不连续点的目标点基准把上报位置钉在
         * seek 目标点，直到管道真的走到它为止，所以框架推过来的位置**天然不会回退**。
         * 端侧以前再挡一次（m_seekUiFloorMs）是重复实现，而且它要靠"端侧自己判断 seek
         * 结束"来撤地板 —— 那套判断正是看门狗的来源，已删。
         */
        if (positionMs == m_position) {
            return;
        }

        m_position = positionMs;
        emit positionChanged();
    }

    void CicadaPlayerItem::notifySnapshot(const QImage &image)
    {
        if (image.isNull()) {
            return;
        }

        /*
         * m_lastSnapshot 留**整幅**（视频原始分辨率）：它才是"这次截到的那张图"
         * （以后要做"保存截图"直接用），也让版本号只反映真实收到的结果。
         */
        m_lastSnapshot = image;

        /*
         * 交给 provider 的那一份**先缩小**。
         *
         * 原因：进度条气泡只有 160x90，而快照是整幅画面（4K 就是 3840*2160*4 = 33MB）。
         * 不缩的话，场景图每次都要为一个小小的气泡上传一张 4K 纹理。
         * 320 = 160 逻辑像素在 2 倍屏（devicePixelRatio 2）上的物理尺寸，够清晰。
         * **只缩不放**：本来就比 320 窄的画面原样用，不白做一次插值。
         */
        constexpr int kSnapshotPreviewMaxWidth = 320;
        QImage preview = image;

        if (preview.width() > kSnapshotPreviewMaxWidth) {
            preview = preview.scaledToWidth(kSnapshotPreviewMaxWidth, Qt::SmoothTransformation);
        }

        SnapshotImageProvider::publish(preview);

        /*
         * 版本号 +1 → QML 里 `"image://snapshot/" + snapshotRevision` 的 URL 跟着变
         * → Image 重新向 provider 取图（气泡那张 Image 开了 asynchronous + cache: false，
         *   正是为这条服务的）。
         */
        ++m_snapshotRevision;
        emit snapshotChanged();
    }

    void CicadaPlayerItem::notifyBufferedPosition(qint64 positionMs)
    {
        if (positionMs == m_bufferedPosition) {
            return;
        }

        m_bufferedPosition = positionMs;
        emit bufferedPositionChanged();
    }

    void CicadaPlayerItem::notifyBuffering(bool buffering)
    {
        if (buffering == m_buffering) {
            return;
        }

        m_buffering = buffering;
        AF_LOGI("buffering %s\n", buffering ? "start" : "end");
        emit bufferingChanged();
    }

    void CicadaPlayerItem::notifyLoadingProgress(int progress)
    {
        /* 只在缓冲期间有意义；界面不显示百分比，所以只记日志（限频由框架侧负责）。 */
        AF_LOGD("loading progress %d%%\n", progress);
    }

    void CicadaPlayerItem::notifyDownloadSpeed(qint64 bitsPerSecond)
    {
        if (bitsPerSecond == m_downloadSpeed) {
            return;
        }

        m_downloadSpeed = bitsPerSecond;
        emit downloadSpeedChanged();
    }

    /*
     * 下载速度显示文本：**超过 1MB/s 用 MB/s，小于 1MB 用 KB/s**（用户明确要求）。
     * 框架给的是 bit/s（MediaPlayerUtil 里 mReadGotSize*8/timeS），先换成字节再除 1024。
     */
    QString CicadaPlayerItem::downloadSpeedText() const
    {
        if (m_downloadSpeed <= 0) {
            return QString();
        }

        const double kbPerSecond = static_cast<double>(m_downloadSpeed) / 8.0 / 1024.0;

        if (kbPerSecond >= 1024.0) {
            return QStringLiteral("%1 MB/s").arg(kbPerSecond / 1024.0, 0, 'f', 2);
        }

        return QStringLiteral("%1 KB/s").arg(kbPerSecond, 0, 'f', 0);
    }

    void CicadaPlayerItem::notifyError(int code, const QString &message)
    {
        AF_LOGE("player error %d: %s\n", code, message.toUtf8().constData());
        setErrorString(QStringLiteral("[%1] %2").arg(code).arg(message));
    }

    void CicadaPlayerItem::notifyPlayerStatus(int status)
    {
        /*
         * 把框架的 PlayerStatus（native_cicada_player_def.h）映射到 QML 侧的 Status。
         *
         * 参数用 int 而不是枚举类型，是为了让头文件不必包含框架的播放器定义。
         */
        switch (static_cast<PlayerStatus>(status)) {
            case PLAYER_PREPARING:
                /*
                 * 打开/缓冲中。已经出过画面的情况（例如 seek 之后重新缓冲）不要退回
                 * Loading，否则界面上的 loading 动画会在播放中途闪出来。
                 */
                if (m_status == Null || m_status == Ready) {
                    setStatus(Loading);
                }

                break;

            case PLAYER_PREPARED:
                /* 就绪：自动播放时紧接着就会是 PLAYING，这里先给 Ready 也不冲突。 */
                setStatus(m_status == Playing ? Playing : Ready);
                break;

            case PLAYER_PLAYING:
                setStatus(Playing);
                break;

            case PLAYER_PAUSED:
                setStatus(Paused);
                break;

            case PLAYER_STOPPED:
                /*
                 * 播完（Completion 回调已经把状态设成 End）之后框架也会走到 STOPPED，
                 * 不要把 End 覆盖掉；已经出错时同理保留 Error。
                 */
                if (m_status != End && m_status != Null && m_status != Error) {
                    setStatus(Stopped);
                }

                break;

            case PLAYER_COMPLETION:
                setStatus(End);
                break;

            case PLAYER_ERROR:
                /* 具体错误文本由 ErrorCallback 带来，这里只保证状态是对的。 */
                setStatus(Error);
                break;

            default:
                /* PLAYER_IDLE / PLAYER_INITIALZED / PLAYER_PREPARINIT：界面状态不动。 */
                break;
        }
    }

    /* ------------------------------------------------------------------ */
    /* 状态同步                                                           */
    /* ------------------------------------------------------------------ */

    void CicadaPlayerItem::setStatus(Status status)
    {
        if (m_status == status) {
            return;
        }

        const bool wasPlaying = (m_status == Playing);
        m_status = status;
        emit statusChanged();

        if (wasPlaying != (m_status == Playing)) {
            emit playingChanged();
        }
    }

    void CicadaPlayerItem::setErrorString(const QString &error)
    {
        if (m_errorString == error) {
            return;
        }

        m_errorString = error;
        emit errorStringChanged();

        if (!error.isEmpty()) {
            setStatus(Error);
        }
    }

    /*
     * 【onPositionTimer 已经删除】
     * 位置/缓冲/时长都不再轮询：
     *   * 位置      —— listener 的 PositionUpdate（onPositionCb → notifyPosition）；
     *   * 缓冲位置  —— listener 的 BufferPositionUpdate（onBufferPositionCb）；
     *   * 时长      —— 在"准备好了 / 首帧 / seek 结束"事件点上现取（refreshDuration）。
     * 见构造函数里那段说明（轮询是第二个时钟，会把弹幕时钟往回拽）。
     */

}// namespace cicadaqt
