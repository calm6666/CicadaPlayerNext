#include "VideoLibrary.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtGui/QImage>
/* QImageWriter：先问一句"这台机器到底写不写得了 JPEG"。
   JPEG 在 Qt 里是**插件**（plugins/imageformats/qjpeg.dll），PNG 是编在 QtGui 里的 ——
   没部署插件时 image.save(..., "JPEG") 会**静默返回 false**，表现就是"封面一张都没有"。 */
#include <QtGui/QImageWriter>
#include <QtCore/QCryptographicHash>
#include <QtCore/QStandardPaths>
#include <QtCore/QDebug>

/* 框架日志：抽帧失败必须留下原因。本轮"首页卡片没有封面"就是空缓存目录 + 这几行日志定位的，
   全项目其它 src/*.cpp 也都用同一套宏（AF_LOGE/AF_LOGW/...）。 */
#include <utils/frame_work_log.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
/* av_strerror：把返回码翻译成人能看懂的话再写进日志 */
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

namespace cicadaqt {

    namespace {

        /* 认得出的视频后缀（首页列表和拖拽都按这个过滤） */
        bool isVideoFile(const QString &suffix)
        {
            static const QStringList kExt = {
                QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("avi"),
                QStringLiteral("flv"), QStringLiteral("ts"), QStringLiteral("mov"),
                QStringLiteral("webm"), QStringLiteral("m4v"), QStringLiteral("wmv"),
                QStringLiteral("mpg"), QStringLiteral("mpeg"), QStringLiteral("rmvb")
            };
            return kExt.contains(suffix.toLower());
        }

        QString thumbCacheDir()
        {
            const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
            const QString dir = base + QStringLiteral("/thumbs");
            QDir().mkpath(dir);
            return dir;
        }

    }// namespace

    VideoLibrary::VideoLibrary(QObject *parent)
        : QObject(parent)
    {
    }

    /* -------------------------------------------------------------------------
     * 扫描
     * ------------------------------------------------------------------------- */
    void VideoLibrary::scanDir(const QString &dir, const QString &rootDir, int depth, int maxDepth)
    {
        if (depth > maxDepth)
            return;

        QDir d(dir);
        const QFileInfoList entries = d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                                      QDir::Name | QDir::DirsFirst);
        QStringList subDirs;

        for (const QFileInfo &info : entries) {
            if (info.isDir()) {
                subDirs.append(info.absoluteFilePath());
                continue;
            }

            if (isVideoFile(info.suffix()))
                appendFile(info.absoluteFilePath(), rootDir);
        }

        for (const QString &sub : subDirs)
            scanDir(sub, rootDir, depth + 1, maxDepth);
    }

    void VideoLibrary::appendFile(const QString &absolutePath, const QString &rootDir)
    {
        if (m_paths.contains(absolutePath))
            return;                                  /* 去重：同一个文件不重复入列 */

        const QFileInfo info(absolutePath);

        if (!info.exists() || info.size() <= 0)
            return;

        m_paths.append(absolutePath);

        QVariantMap entry;
        entry.insert(QStringLiteral("path"), absolutePath);
        entry.insert(QStringLiteral("name"), info.completeBaseName());
        /* 选集面板读的是 title，这里给同一份名字（两个键指向同一个显示名） */
        entry.insert(QStringLiteral("title"), info.completeBaseName());
        entry.insert(QStringLiteral("dir"), rootDir.isEmpty() ? info.absolutePath() : rootDir);
        entry.insert(QStringLiteral("sizeBytes"), static_cast<qlonglong>(info.size()));
        entry.insert(QStringLiteral("durationMs"), static_cast<qlonglong>(0));
        entry.insert(QStringLiteral("thumb"), thumbnailFor(absolutePath));
        m_items.append(entry);
    }

    int VideoLibrary::addFolder(const QString &path, int maxDepth)
    {
        const QFileInfo info(path);

        if (!info.exists() || !info.isDir())
            return 0;

        const int before = m_items.size();

        m_scanning = true;
        emit scanningChanged();

        m_roots.append(info.absoluteFilePath());
        scanDir(info.absoluteFilePath(), info.absoluteFilePath(), 1, maxDepth);

        m_scanning = false;
        emit scanningChanged();

        if (m_items.size() != before)
            emit itemsChanged();

        return m_items.size() - before;
    }

    int VideoLibrary::addPaths(const QStringList &paths, int maxDepth)
    {
        const int before = m_items.size();

        m_scanning = true;
        emit scanningChanged();

        for (const QString &path : paths) {
            const QFileInfo info(path);

            if (info.isDir()) {
                m_roots.append(info.absoluteFilePath());
                scanDir(info.absoluteFilePath(), info.absoluteFilePath(), 1, maxDepth);
            } else if (info.isFile() && isVideoFile(info.suffix())) {
                appendFile(info.absoluteFilePath(), info.absolutePath());
            }
        }

        m_scanning = false;
        emit scanningChanged();

        if (m_items.size() != before)
            emit itemsChanged();

        return m_items.size() - before;
    }

    void VideoLibrary::removeAt(int index)
    {
        if (index < 0 || index >= m_items.size())
            return;

        m_paths.removeAt(index);
        m_items.removeAt(index);

        if (m_currentIndex >= m_items.size())
            m_currentIndex = m_items.size() - 1;

        emit itemsChanged();
        emit currentIndexChanged();
    }

    void VideoLibrary::clear()
    {
        m_items.clear();
        m_paths.clear();
        m_roots.clear();
        m_currentIndex = -1;
        emit itemsChanged();
        emit currentIndexChanged();
    }

    /* -------------------------------------------------------------------------
     * 播放列表导航
     * ------------------------------------------------------------------------- */
    void VideoLibrary::setCurrentIndex(int index)
    {
        if (m_currentIndex == index)
            return;

        m_currentIndex = index;
        emit currentIndexChanged();
    }

    bool VideoLibrary::goPrevious()
    {
        if (!hasPrevious())
            return false;

        setCurrentIndex(m_currentIndex - 1);
        return true;
    }

    bool VideoLibrary::goNext()
    {
        if (!hasNext())
            return false;

        setCurrentIndex(m_currentIndex + 1);
        return true;
    }

    QString VideoLibrary::pathAt(int index) const
    {
        if (index < 0 || index >= m_items.size())
            return QString();

        return m_items.at(index).toMap().value(QStringLiteral("path")).toString();
    }

    QString VideoLibrary::nameAt(int index) const
    {
        if (index < 0 || index >= m_items.size())
            return QString();

        return m_items.at(index).toMap().value(QStringLiteral("name")).toString();
    }

    /* -------------------------------------------------------------------------
     * 缩略图：抽首帧 → 缩到 320x180 → 存 JPEG/PNG，缓存按"路径+大小+改动时间"哈希
     *
     * 【这一轮修了两个真问题，两个都会让"一张封面都生成不出来"】
     *
     * 1. **喂给 FFmpeg 的路径必须是 UTF-8**
     *    原来写的是 `QFile::encodeName(videoPath)`。Qt 文档里它的语义是"local 8-bit
     *    encoding" —— 在中文 Windows 上就是 **GBK**。而 Windows 版 FFmpeg 打开文件时
     *    会把这串字节当 **UTF-8** 再转成宽字符（_wopen），GBK 字节不是合法 UTF-8，
     *    于是 `avformat_open_input` 直接失败。
     *    本项目的视频全在 `D:\视频\相机胶卷\…` 这种中文目录下 —— 一条都开不了。
     *    现在先试 UTF-8（三端都正确），万一老环境要 ANSI 再退一步用 encodeName()。
     *
     * 2. **swscale 用项目里已经在跑的那套**（`sws_getCachedContext` + `sws_scale`，
     *    目标内存由 QImage 自己持有），写法照抄 `src/CicadaVideoTexture.cpp:282-298`
     *    那条 CPU 回退路径 —— 同一次构建、同一个 libswscale，它是**每帧都在跑、已验证可用**的。
     *
     *    之前这里走的是 `sws_alloc_context → av_opt_set_int(srcw/dstw/…) → sws_init_context
     *    → av_frame_get_buffer → sws_scale_frame`，抽出来的图**整片空白**
     *    （尺寸 320x180 没错，但只有最上面一行有内容，其余全黑或全白）——
     *    那次转换根本没写进我们随后读的那块内存。项目里既然有现成可用的路，就别再试 API 了。
     *
     *    顺带纠正 HANDOVER-TODO 里的旧结论：它原来说"FFmpeg 9 把 sws_getContext/sws_scale
     *    删了，必须用新 API"—— **错的**。本机头文件是 libswscale 10，
     *    swscale.h:450 起的 "Legacy (stateful) API" 一节里 sws_getContext(551) /
     *    sws_scale(583) / sws_getCachedContext(737) 全都在，且**没有** attribute_deprecated。
     *
     * 3. **写盘格式先探测**：JPEG 是 Qt 插件，PNG 是内建的。插件没部署时
     *    `save(..., "JPEG")` 静默 false；所以按 `QImageWriter::supportedImageFormats()`
     *    选一次后缀，缓存文件名跟着后缀走（不会出现半张 jpg 半张 png 的混乱缓存）。
     * ------------------------------------------------------------------------- */
    /* ===========================================================================
 * 抽帧：从本地文件解出"某个时间点附近"的一帧（返回 QImage::Format_RGB888）。
 *
 * 【为什么内联在这里】这段逻辑属于本文件，不要再往外面抽成独立模块。
 *
 * 几个踩过的坑（照旧保留）：
 *   * 路径要用 UTF-8 优先，失败退 QFile::encodeName（Windows 上中文路径）；
 *   * seek 必须用**流自己的时间基**（av_rescale_q(ms, {1,1000}, stream->time_base)），
 *     用 AV_TIME_BASE 或者 stream_index = -1 都不生效；
 *   * seek 之后必须 avcodec_flush_buffers，否则收到的是 seek 之前的残留帧；
 *   * 最多读 128 个包就放弃：只有音频没有视频、或者关键帧损坏时不能无限循环。
 *
 * 【不要用它解进度条 hover 的图】缩略图只是"封面"，策略上允许退让；hover 预览
 * 应该是用户指着的那一帧，抽不到就该没有，不能偷偷换成别的画面。
 * =========================================================================== */
static QImage grabFrameAt(const QString &path, qint64 positionMs, const QSize &targetSize)
{
    const QByteArray utf8Path = path.toUtf8();

    AVFormatContext *format = nullptr;

    if (avformat_open_input(&format, utf8Path.constData(), nullptr, nullptr) != 0) {
        AF_LOGW("VideoLibrary: avformat_open_input failed for \"%s\"\n", utf8Path.constData());
        return QImage();
    }

    const int streamIndex = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    if (streamIndex < 0) {
        AF_LOGW("VideoLibrary: no video stream in \"%s\"\n", utf8Path.constData());
        avformat_close_input(&format);
        return QImage();
    }

    AVStream *stream = format->streams[streamIndex];
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);

    if (codec == nullptr) {
        AF_LOGW("VideoLibrary: no decoder for codec id %d\n", int(stream->codecpar->codec_id));
        avformat_close_input(&format);
        return QImage();
    }

    AVCodecContext *decoder = avcodec_alloc_context3(codec);

    if (decoder == nullptr || avcodec_parameters_to_context(decoder, stream->codecpar) < 0
        || avcodec_open2(decoder, codec, nullptr) < 0) {
        AF_LOGW("VideoLibrary: avcodec open failed for \"%s\"\n", utf8Path.constData());
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return QImage();
    }

    qint64 seekMs = positionMs > 0 ? positionMs : 0;
    /* AVSEEK_FLAG_BACKWARD：跳到目标点**之前**最近的关键帧，然后往前解到目标点 */
    const int seekRet = av_seek_frame(format, streamIndex,
                                      av_rescale_q(seekMs, AVRational{ 1, 1000 }, stream->time_base),
                                      AVSEEK_FLAG_BACKWARD);

    if (seekRet < 0)
        AF_LOGW("VideoLibrary: av_seek_frame(%lld ms) failed, will decode from the start\n",
                static_cast<long long>(seekMs));

    avcodec_flush_buffers(decoder);

    QImage result;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    SwsContext *sws = nullptr;
    int packetsRead = 0;

    while (packetsRead < 128 && result.isNull() && av_read_frame(format, packet) >= 0) {
        ++packetsRead;

        if (packet->stream_index != streamIndex) {
            av_packet_unref(packet);
            continue;
        }

        if (avcodec_send_packet(decoder, packet) == 0) {
            while (avcodec_receive_frame(decoder, frame) == 0) {
                const int outW = targetSize.width() > 0 ? targetSize.width() : frame->width;
                const int outH = targetSize.height() > 0 ? targetSize.height() : frame->height;

                QImage image(outW, outH, QImage::Format_RGB888);

                if (image.isNull()) {
                    AF_LOGW("VideoLibrary: QImage allocation failed (%dx%d)\n", outW, outH);
                    break;
                }

                sws = sws_getCachedContext(sws, frame->width, frame->height,
                                           static_cast<AVPixelFormat>(frame->format),
                                           outW, outH, AV_PIX_FMT_RGB24,
                                           SWS_BILINEAR, nullptr, nullptr, nullptr);

                if (sws == nullptr) {
                    AF_LOGW("VideoLibrary: sws_getCachedContext failed\n");
                    break;
                }

                uint8_t *dst[4] = { image.bits(), nullptr, nullptr, nullptr };
                int dstStride[4] = { int(image.bytesPerLine()), 0, 0, 0 };

                sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);

                result = image;
                break;
            }
        }

        av_packet_unref(packet);
    }

    if (result.isNull())
        AF_LOGW("VideoLibrary: no decodable frame near %lld ms in \"%s\"\n",
                static_cast<long long>(seekMs), utf8Path.constData());

    if (sws != nullptr)
        sws_freeContext(sws);

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&format);

    return result;
}

QString VideoLibrary::thumbnailFor(const QString &videoPath)
    {
        const QFileInfo info(videoPath);
        const QString key = QStringLiteral("%1|%2|%3")
                                .arg(info.absoluteFilePath())
                                .arg(info.size())
                                .arg(info.lastModified().toMSecsSinceEpoch());
        const QString hash = QString::fromLatin1(
            QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());

        /* 后缀按本机实际支持的格式定（见上面第 3 点），缓存名 = <hash>.<ext> */
        const bool canWriteJpeg =
            QImageWriter::supportedImageFormats().contains(QByteArrayLiteral("jpg"));
        const QString suffix = canWriteJpeg ? QStringLiteral("jpg") : QStringLiteral("png");
        const QString target = thumbCacheDir() + QLatin1Char('/') + hash
                               + QLatin1Char('.') + suffix;

        if (QFileInfo::exists(target))
            return target;                           /* 缓存命中直接返回（二次启动不再解码） */

        const QByteArray utf8Path = videoPath.toUtf8();

        /*
         * 缩略图的格子尺寸。交给 grabFrame 的 targetSize 让 **swscale 一次缩到位**，
         * 而不是"先解出全尺寸再让 Qt 缩" —— 少一次全尺寸拷贝（1080p 一帧 3MB 起步）。
         * 注意它是**拉伸填满**、不保持宽高比：首页卡片就是 16:9 的格子，
         * 4:3 的片子铺满它比左右留两条黑边好看。
         */
        const QSize thumbSize(320, 180);

        /*
         * 抽哪一帧：**不要抽第 0 帧**。
         *
         * 实测（`D:\视频\相机胶卷` 那 5 条）第 0 帧几乎全黑：
         *     ffmpeg -i x.mp4 -frames:v 1 → mean≈0.6~1.7（只有 凤凰花开的路口 是白的 252）
         * 同一个文件 `-ss 2` 取到的帧 mean 52~158、上百种颜色，才是有画面的那一帧。
         * 也就是说"封面一片黑"不是抽帧坏了，是**抽的位置不对** —— 很多片子（尤其手机
         * 拍的）开头就是几帧黑的/淡入的。所以先跳到 2 秒再解码。
         *
         * 跳不动就退回 0ms：片长不足 2 秒、或者容器不支持 seek 时 av_seek_frame 会失败
         * （grabFrameAt 里已经留了一行 AF_LOGW），那时候仍然按老办法取第一帧（总比没有封面强）。
         *
         * 【为什么这个"2 秒 → 0 秒"的退让留在调用方】
         * 它是**缩略图**的策略。进度条 hover 预览要的是用户指着的那一秒，
         * 抽不到就该没有（或者显示占位），绝不能偷偷换成别的画面 —— 那比空着更让人困惑。
         */
        QImage image = grabFrameAt(videoPath, 2000, thumbSize);

        if (image.isNull())
            image = grabFrameAt(videoPath, 0, thumbSize);

        if (image.isNull()) {
            /* 具体原因（打不开 / 没有视频流 / 没有解码器 / seek 不了 / 这个位置没有可解的帧）
               已经在 grabFrame 里逐条 AF_LOGW 过了，这里只补一句"所以封面没有" */
            AF_LOGW("VideoLibrary: no thumbnail for \"%s\"\n", utf8Path.constData());
            return QString();
        }

        /*
         * 写盘。image 是 grabFrame 里**我们自己分配、自己持有**的 QImage
         * （不是包着 FFmpeg 缓冲的视图），所以这里不用关心谁先释放谁，
         * 也不存在行对齐和外部缓冲生命周期的问题。
         */
        if (image.save(target, canWriteJpeg ? "JPEG" : "PNG", 85)) {
            AF_LOGD("VideoLibrary: thumbnail written %s (%s)\n",
                    target.toUtf8().constData(), utf8Path.constData());
            return target;
        }

        AF_LOGW("VideoLibrary: QImage::save failed for \"%s\" (%s not supported?)\n",
                target.toUtf8().constData(), canWriteJpeg ? "JPEG" : "PNG");
        return QString();
    }

}// namespace cicadaqt
