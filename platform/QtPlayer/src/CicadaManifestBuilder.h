//
// CicadaManifestBuilder.h
//
// 对象模式（MediaManifest）的 Qt 侧**类型化构造器**（header-only，避免改 CMakeLists）。
//
// 与内核逐字对应：
//   framework/demuxer/manifest/MediaManifest.h           结构体
//   framework/demuxer/manifest/MediaManifestParser.cpp   JSON 键名（camelCase）
//
// 链路（与 Web/Android 完全一致）：
//   本构造器 -> JSON 文本 -> MediaPlayer::SetDataSource(std::string jsonManifest)
//     -> SuperMediaPlayer::SetDataSource(const std::string &) -> MediaManifestParser::parse
//     -> MSG_SETMANIFESTSOURCE -> ManifestDemuxer::buildPlayList()
//   **内核用 cJSON 解析**；Qt 应用层不需要（也不应该）再解析一遍 JSON。
//
// 必填：duration > 0 且 video[] 非空；否则内核回调
//   MEDIA_PLAYER_ERROR_DEMUXER_MANIFEST_PARSE（0x20030007）。
//
// 用法：
//   CicadaManifest::Builder b;
//   b.duration(120).mediaSourceType(QStringLiteral("hls"))
//    .addVideo(CicadaManifest::videoRepresentation(QStringLiteral("v0"), QStringLiteral("https://cdn/v/"))
//                  .bandwidth(2000000).codecs(QStringLiteral("avc1.64001f"))
//                  .size(1920, 1080)
//                  .segmentTemplate(QStringLiteral("init.mp4"), QStringLiteral("seg-*.m4s")));
//   QString json = b.toJsonString();
//   // 注意：必须传 std::string；传 const char* 会命中 SetDataSource(const char *url) 重载
//   player->SetDataSource(std::string(json.toUtf8().constData()));
//

#ifndef CICADA_MANIFEST_BUILDER_H
#define CICADA_MANIFEST_BUILDER_H

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace CicadaManifest {

/** 一个视频/音频档位的构造器（内部就是 QJsonObject 的链式包装）。 */
class RepresentationBuilder {
public:
    RepresentationBuilder(const QString &id, const QString &baseUrl)
    {
        mObject.insert(QStringLiteral("id"), id);
        mObject.insert(QStringLiteral("baseUrl"), baseUrl);
    }

    RepresentationBuilder &bandwidth(qint64 bps)
    {
        mObject.insert(QStringLiteral("bandwidth"), static_cast<double>(bps));
        return *this;
    }

    RepresentationBuilder &averageBandwidth(qint64 bps)
    {
        mObject.insert(QStringLiteral("averageBandwidth"), static_cast<double>(bps));
        return *this;
    }

    RepresentationBuilder &mimeType(const QString &mimeType)
    {
        mObject.insert(QStringLiteral("mimeType"), mimeType);
        return *this;
    }

    RepresentationBuilder &codecs(const QString &codecs)
    {
        mObject.insert(QStringLiteral("codecs"), codecs);
        return *this;
    }

    RepresentationBuilder &size(int width, int height)
    {
        mObject.insert(QStringLiteral("width"), width);
        mObject.insert(QStringLiteral("height"), height);
        return *this;
    }

    RepresentationBuilder &frameRate(double fps)
    {
        mObject.insert(QStringLiteral("frameRate"), fps);
        return *this;
    }

    RepresentationBuilder &audioSamplingRate(int hz)
    {
        mObject.insert(QStringLiteral("audioSamplingRate"), hz);
        return *this;
    }

    RepresentationBuilder &lang(const QString &lang)
    {
        mObject.insert(QStringLiteral("lang"), lang);
        return *this;
    }

    RepresentationBuilder &role(const QString &role)
    {
        mObject.insert(QStringLiteral("role"), role);
        return *this;
    }

    RepresentationBuilder &name(const QString &name)
    {
        mObject.insert(QStringLiteral("name"), name);
        return *this;
    }

    RepresentationBuilder &isDefault(bool isDefault)
    {
        mObject.insert(QStringLiteral("isDefault"), isDefault);
        return *this;
    }

    RepresentationBuilder &backupUrls(const QStringList &urls)
    {
        if (!urls.isEmpty()) {
            QJsonArray arr;
            for (const QString &u : urls) {
                arr.append(u);
            }
            mObject.insert(QStringLiteral("backupUrls"), arr);
        }
        return *this;
    }

    /** 直接给出 segmentInfo（三种模式之一，见 SegmentInfoBuilder）。 */
    RepresentationBuilder &segmentInfo(const QJsonObject &segmentInfo)
    {
        if (!segmentInfo.isEmpty()) {
            mObject.insert(QStringLiteral("segmentInfo"), segmentInfo);
        }
        return *this;
    }

    /** template 模式便捷写法：URL 模板用 * 占位分片序号。 */
    RepresentationBuilder &segmentTemplate(const QString &initialization, const QString &media, qint64 startNumber = 1)
    {
        QJsonObject s;
        s.insert(QStringLiteral("mode"), QStringLiteral("template"));
        s.insert(QStringLiteral("initialization"), initialization);
        s.insert(QStringLiteral("media"), media);
        s.insert(QStringLiteral("startNumber"), static_cast<double>(startNumber));
        mObject.insert(QStringLiteral("segmentInfo"), s);
        return *this;
    }

    /** list 模式便捷写法：一个文件 + byteRange。 */
    RepresentationBuilder &segmentSingle(const QString &initialization, const QString &indexRange)
    {
        QJsonObject s;
        s.insert(QStringLiteral("mode"), QStringLiteral("single"));
        s.insert(QStringLiteral("initialization"), initialization);
        s.insert(QStringLiteral("indexRange"), indexRange);
        mObject.insert(QStringLiteral("segmentInfo"), s);
        return *this;
    }

    RepresentationBuilder &encryption(const QJsonObject &encryption)
    {
        if (!encryption.isEmpty()) {
            mObject.insert(QStringLiteral("encryption"), encryption);
        }
        return *this;
    }

    QJsonObject build() const
    {
        return mObject;
    }

private:
    QJsonObject mObject;
};

/** AES-128 分片加密（HLS EXT-X-KEY 等价物）。 */
inline QJsonObject aes128Encryption(const QString &keyUrl, const QString &ivHex = QString())
{
    QJsonObject e;
    e.insert(QStringLiteral("keyUrl"), keyUrl);
    if (!ivHex.isEmpty()) {
        e.insert(QStringLiteral("iv"), ivHex);
    }
    return e;
}

/** DRM 授权服务器。keyType: clearkey | aes128 | widevine | fairplay | playready。 */
inline QJsonObject licenseServer(const QString &url, const QString &keyType = QStringLiteral("clearkey"),
                                 const QString &contentId = QString())
{
    QJsonObject l;
    l.insert(QStringLiteral("url"), url);
    l.insert(QStringLiteral("keyType"), keyType);
    if (!contentId.isEmpty()) {
        l.insert(QStringLiteral("contentId"), contentId);
    }
    return l;
}

/**
 * UI 侧需要的静态信息（清晰度菜单/codecs/host）。
 *
 * 目的：**同一份对象**既喂内核、也喂界面，避免今天 Qt 应用层
 * （CicadaPlayerItem.cpp 里 QJsonDocument::fromJson + pickCodecs 那段）为了菜单
 * 再解析一遍同一份 JSON，造成两套 schema 知识漂移。
 */
struct StaticInfo {
    struct Track {
        QString id;
        QString codecs;
        QString host;
        qint64 bandwidth{0};
        int width{0};
        int height{0};
        QString lang;
    };

    QList<Track> videos;
    QList<Track> audios;
    QString primaryVideoCodecs;
    QString primaryAudioCodecs;

    /** 从**已经构造好的对象**提取，不再走 JSON 文本。 */
    static StaticInfo fromJson(const QJsonObject &manifest);
};

/** 构造器：链式累积，最后 toJsonString() 交给内核的 std::string 重载。 */
class Builder {
public:
    Builder &duration(double seconds)
    {
        mObject.insert(QStringLiteral("duration"), seconds);
        return *this;
    }

    /** 缺省 hls；传 "dash" 走 DASH 分片管线。 */
    Builder &mediaSourceType(const QString &type)
    {
        mObject.insert(QStringLiteral("mediaSourceType"), type);
        return *this;
    }

    Builder &minBufferTime(double seconds)
    {
        mObject.insert(QStringLiteral("minBufferTime"), seconds);
        return *this;
    }

    Builder &maxSegmentDuration(double seconds)
    {
        mObject.insert(QStringLiteral("maxSegmentDuration"), seconds);
        return *this;
    }

    Builder &title(const QString &title)
    {
        mObject.insert(QStringLiteral("title"), title);
        return *this;
    }

    Builder &live(bool live)
    {
        mObject.insert(QStringLiteral("live"), live);
        return *this;
    }

    Builder &liveConfig(const QJsonObject &config)
    {
        if (!config.isEmpty()) {
            mObject.insert(QStringLiteral("liveConfig"), config);
        }
        return *this;
    }

    Builder &startTimeOffset(double seconds)
    {
        mObject.insert(QStringLiteral("startTimeOffset"), seconds);
        return *this;
    }

    Builder &location(const QString &location)
    {
        mObject.insert(QStringLiteral("location"), location);
        return *this;
    }

    Builder &minimumUpdatePeriod(double seconds)
    {
        mObject.insert(QStringLiteral("minimumUpdatePeriod"), seconds);
        return *this;
    }

    Builder &addVideo(const QJsonObject &representation)
    {
        append(QStringLiteral("video"), representation);
        return *this;
    }

    Builder &addAudio(const QJsonObject &representation)
    {
        append(QStringLiteral("audio"), representation);
        return *this;
    }

    Builder &addSubtitle(const QJsonObject &subtitle)
    {
        append(QStringLiteral("subtitle"), subtitle);
        return *this;
    }

    Builder &addContentProtection(const QJsonObject &protection)
    {
        append(QStringLiteral("contentProtection"), protection);
        return *this;
    }

    Builder &encryption(const QJsonObject &encryption)
    {
        if (!encryption.isEmpty()) {
            mObject.insert(QStringLiteral("encryption"), encryption);
        }
        return *this;
    }

    Builder &licenseServer(const QJsonObject &server)
    {
        if (!server.isEmpty()) {
            mObject.insert(QStringLiteral("licenseServer"), server);
        }
        return *this;
    }

    QJsonObject toJson() const
    {
        return mObject;
    }

    QString toJsonString() const
    {
        return QString::fromUtf8(QJsonDocument(mObject).toJson(QJsonDocument::Compact));
    }

    /** 直接得到 UI 静态信息（同一份对象，不需要再解析 JSON）。 */
    StaticInfo staticInfo() const
    {
        return StaticInfo::fromJson(mObject);
    }

private:
    void append(const QString &key, const QJsonObject &item)
    {
        if (item.isEmpty()) {
            return;
        }
        QJsonArray arr = mObject.value(key).toArray();
        arr.append(item);
        mObject.insert(key, arr);
    }

    QJsonObject mObject;
};

inline QString hostOf(const QString &url)
{
    const QUrl u(url);
    return u.host();
}

inline StaticInfo StaticInfo::fromJson(const QJsonObject &manifest)
{
    StaticInfo info;
    auto take = [&info](const QJsonArray &arr, QList<StaticInfo::Track> &out) {
        for (const QJsonValue &v : arr) {
            const QJsonObject o = v.toObject();
            StaticInfo::Track t;
            t.id = o.value(QStringLiteral("id")).toString();
            t.codecs = o.value(QStringLiteral("codecs")).toString();
            t.host = hostOf(o.value(QStringLiteral("baseUrl")).toString());
            t.bandwidth = static_cast<qint64>(o.value(QStringLiteral("bandwidth")).toDouble());
            t.width = o.value(QStringLiteral("width")).toInt();
            t.height = o.value(QStringLiteral("height")).toInt();
            t.lang = o.value(QStringLiteral("lang")).toString();
            out.append(t);
        }
    };
    take(manifest.value(QStringLiteral("video")).toArray(), info.videos);
    take(manifest.value(QStringLiteral("audio")).toArray(), info.audios);
    if (!info.videos.isEmpty()) {
        info.primaryVideoCodecs = info.videos.first().codecs;
    }
    if (!info.audios.isEmpty()) {
        info.primaryAudioCodecs = info.audios.first().codecs;
    }
    return info;
}

} // namespace CicadaManifest

#endif // CICADA_MANIFEST_BUILDER_H
