//
// 对象清单播放的单元测试 —— 测的是**核心播放器**能不能直接吃对象，不是应用层。
//
// 被测代码（核心，不在 platform/QtPlayer 里）：
//   framework/demuxer/manifest/MediaManifestParser.cpp
//       JSON 对象 -> Cicada::Manifest::MediaManifest（一次性转换，无清单文本）
// 它就是 MediaPlayer::SetDataSource(const std::string &jsonManifest) 的第一步：
//
//   MediaPlayer::SetDataSource(const std::string &)      mediaPlayer/MediaPlayer.cpp
//     -> SuperMediaPlayer::SetDataSource(const std::string &)
//     -> Manifest::MediaManifestParser::parse(...)       <<< 这里测的就是它
//     -> MSG_SETMANIFESTSOURCE -> demuxer_service 造 ManifestDemuxer
//     -> ManifestDemuxer::buildPlayList() 把对象展开成分片列表 + init 段
//
// 分片展开（模板展开、显式分片、URL 解析）在 ManifestDemuxer 里，依赖 playList
// 内部状态，不属于这个纯数据测试；URL 模式（SetDataSource(const char *)）走的是
// 另一条通路（原生 m3u8/mpd 解析），也不在这里测。
//
// 测试数据 = 参考工程里"真正会喂给播放器的那种对象"：
//   front/hili-player/dist/test-*.json
// 目录由 CMake 用 CICADA_QT_REFERENCE_DIST 传进来（相对源码目录推导），
// 不写死任何机器路径。
//
#include <QtTest/QtTest>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QStringList>

#include <demuxer/manifest/MediaManifestParser.h>

using Cicada::Manifest::MediaManifest;
using Cicada::Manifest::MediaManifestParser;
using Cicada::Manifest::MediaRepresentation;
using Cicada::Manifest::SegmentMode;

namespace {

    /* 参考对象的目录（CMake 传进来的编译期常量）。 */
    QString referenceDistDir()
    {
        return QString::fromUtf8(CICADA_QT_REFERENCE_DIST);
    }

    bool readFile(const QString &path, std::string &out)
    {
        QFile file(path);

        if (!file.open(QIODevice::ReadOnly)) {
            return false;
        }

        const QByteArray bytes = file.readAll();
        out.assign(bytes.constData(), static_cast<size_t>(bytes.size()));
        return true;
    }

    /* 解析一个参考对象；失败时 err 里带文件名，方便定位是哪个对象不行。 */
    bool parseReference(const QString &fileName, MediaManifest &manifest, QString &err)
    {
        std::string json;
        const QString path = referenceDistDir() + QLatin1Char('/') + fileName;

        if (!readFile(path, json)) {
            err = QStringLiteral("读不到文件: ") + path;
            return false;
        }

        std::string error;

        if (!MediaManifestParser::parse(json, manifest, error)) {
            err = fileName + QStringLiteral(": ") + QString::fromStdString(error);
            return false;
        }

        return true;
    }

} // namespace

class TestObjectManifest : public QObject {
    Q_OBJECT

private slots:
    /* 参考对象存在性自检：数据目录必须真的找到了，否则后面的用例没有意义。 */
    void referenceDistDirIsPresent()
    {
        const QDir dir(referenceDistDir());
        QVERIFY2(dir.exists(), qPrintable(QStringLiteral("参考对象目录不存在: ") + referenceDistDir()));

        const QStringList files = dir.entryList({QStringLiteral("test-*.json")}, QDir::Files);
        qInfo() << "参考对象目录:" << referenceDistDir() << "共" << files.size() << "个对象";
        QVERIFY2(files.size() >= 8, "参考对象太少，路径可能不对");
    }

    /*
     * 核心必须能**逐个**吃下参考工程里的每个对象：
     * 解析成功 + duration>0 + video[] 非空 + 每档都带 segmentInfo。
     * 这就是"核心播放器自己支持对象播放"的判据。
     */
    void parsesEveryReferenceObject()
    {
        const QDir dir(referenceDistDir());
        const QStringList files = dir.entryList({QStringLiteral("test-*.json")}, QDir::Files, QDir::Name);
        QVERIFY(!files.isEmpty());

        for (const QString &fileName : files) {
            MediaManifest manifest;
            QString err;

            QVERIFY2(parseReference(fileName, manifest, err), qPrintable(err));
            QVERIFY2(manifest.duration > 0, qPrintable(fileName + QStringLiteral(": duration 必须 > 0")));
            QVERIFY2(!manifest.video.empty(), qPrintable(fileName + QStringLiteral(": video[] 不能为空")));

            for (const MediaRepresentation &rep : manifest.video) {
                QVERIFY2(rep.hasSegmentInfo,
                         qPrintable(fileName + QStringLiteral(": 每档视频都必须有 segmentInfo")));
                QVERIFY2(!rep.baseUrl.empty(),
                         qPrintable(fileName + QStringLiteral(": baseUrl 必须被读进来")));
            }
        }
    }

    /*
     * template 模式的关键字段要原样进到对象模型里 ——
     * 这些字段（startNumber / mediaSequence / suffix / timescale ...）是核心自己
     * 在 ManifestDemuxer::expandTemplate 里用的，应用层不许碰。
     * 数据：test-hls.json（没有 media 字段，分片模式要从 initialization 推）
     */
    void templateFieldsSurvive()
    {
        MediaManifest manifest;
        QString err;
        QVERIFY2(parseReference(QStringLiteral("test-hls.json"), manifest, err), qPrintable(err));

        QVERIFY(!manifest.video.empty());
        const MediaRepresentation &rep = manifest.video.front();
        const Cicada::Manifest::SegmentInfo &seg = rep.segmentInfo;

        QCOMPARE(seg.mode, SegmentMode::Template);
        QCOMPARE(seg.initialization, std::string("0-0.m4s"));
        QVERIFY2(seg.media.empty(), "test-hls.json 里没有 media 字段，核心要靠 initialization 自己推");
        QVERIFY(seg.hasStartNumber);
        QCOMPARE(seg.startNumber, static_cast<int64_t>(1));
        QVERIFY(seg.hasMediaSequence);
        QCOMPARE(seg.mediaSequence, static_cast<int64_t>(1));
        QCOMPARE(seg.suffix, std::string("m4s"));
        QCOMPARE(seg.timescale, static_cast<int64_t>(60000));
        QVERIFY(seg.targetDuration > 4.0);

        /* backupUrls 是数组，解析器要能读到元素本身（不是按 key 读）。 */
        QCOMPARE(rep.backupUrls.size(), static_cast<size_t>(2));
    }

    /* template + segmentTimeline + totalCount（DASH 语义的对象）也要原样读进来。 */
    void templateTimelineFieldsSurvive()
    {
        MediaManifest manifest;
        QString err;
        QVERIFY2(parseReference(QStringLiteral("test-dash-v5.json"), manifest, err), qPrintable(err));
        QVERIFY(!manifest.video.empty());

        const Cicada::Manifest::SegmentInfo &seg = manifest.video.front().segmentInfo;
        QCOMPARE(seg.mode, SegmentMode::Template);
        QVERIFY(seg.hasTotalCount);
        QCOMPARE(seg.totalCount, static_cast<int64_t>(24));
        QCOMPARE(seg.timescale, static_cast<int64_t>(1000000));
        QVERIFY2(!seg.segmentTimeline.empty(), "segmentTimeline 必须被读进来（核心按它算每片时长）");

        for (const Cicada::Manifest::SegmentTimelineEntry &entry : seg.segmentTimeline) {
            QVERIFY(entry.d > 0);
        }
    }

    /*
     * 显式分片（mode=list）：segments[] 原样进对象模型。
     * 数据：test-hls-explicit.json —— 注意它带 suffix "ts"，
     * 但参考实现里 list 模式**不**套 suffix（suffix 只作用于模板），核心同样不套，
     * 所以这里只断言字段本身进来了，不断言 URL 被改写。
     */
    void explicitListFieldsSurvive()
    {
        MediaManifest manifest;
        QString err;
        QVERIFY2(parseReference(QStringLiteral("test-hls-explicit.json"), manifest, err), qPrintable(err));
        QVERIFY(!manifest.video.empty());

        const Cicada::Manifest::SegmentInfo &seg = manifest.video.front().segmentInfo;
        QCOMPARE(seg.mode, SegmentMode::List);
        QCOMPARE(seg.initialization, std::string("0-0.m4s"));
        QCOMPARE(seg.suffix, std::string("ts"));
        QVERIFY2(seg.segments.size() > 10, "显式分片列表应该被完整读进来");

        for (const Cicada::Manifest::Segment &s : seg.segments) {
            QVERIFY(s.duration > 0);
            QVERIFY(!s.url.empty());
        }
    }

    /* 同一个对象里的音频档（分开的 audio[]）也要能读出来。 */
    void audioRepresentationsSurvive()
    {
        MediaManifest manifest;
        QString err;
        QVERIFY2(parseReference(QStringLiteral("test-hls.json"), manifest, err), qPrintable(err));

        QVERIFY2(!manifest.audio.empty(), "test-hls.json 里应该有 audio[]");
        const MediaRepresentation &aud = manifest.audio.front();
        QVERIFY(aud.hasSegmentInfo);
        QVERIFY(!aud.lang.empty());
    }

    /*
     * 缺 duration（非直播）或缺 video[] 的对象必须被核心拒绝，而不是崩在后面。
     * 注意：这里不用 C++11 的原始字符串字面量 R"(...)"，moc 解析不了它。
     */
    void invalidObjectsAreRejected()
    {
        MediaManifest manifest;
        std::string error;

        QVERIFY(!MediaManifestParser::parse("{\"video\":[{\"id\":\"v0\"}]}", manifest, error));
        QVERIFY(!error.empty());

        error.clear();
        QVERIFY(!MediaManifestParser::parse("{\"duration\":10.0}", manifest, error));
        QVERIFY(!error.empty());

        /* live 的流可以没有 duration。 */
        error.clear();
        QVERIFY(MediaManifestParser::parse(
                "{\"live\":true,\"video\":[{\"id\":\"v0\",\"baseUrl\":\"http://a/b/\"}]}", manifest, error));
    }
};

QTEST_GUILESS_MAIN(TestObjectManifest)

#include "testObjectManifest.moc"
