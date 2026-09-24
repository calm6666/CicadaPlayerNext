#include "DanmakuController.h"

/* _CrtCheckMemory / _CrtSetDbgFlag：只用于 Debug 构建的堆校验（见 update() 里那段） */
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

#include <Danmaku/DanmakuXml.h>

#include <QtCore/QDateTime>
#include <QtCore/QFile>
/* QHash 给"颜色 → hex 字符串"的缓存用（见 cachedColorHex） */
#include <QtCore/QHash>
/* 异步加载：任务丢进线程池，结果用一次排到 GUI 线程的调用取走（见 DanmakuParseTask） */
#include <QtCore/QMutexLocker>
#include <QtCore/QRunnable>
#include <QtCore/QThreadPool>
#include <QtCore/QtMath>
/* 【注意】QFont / QFontMetrics / QFontMetricsF 在 Qt 6 里属于 **QtGui**，
   写成 <QtCore/QFont> 会直接 "Cannot open include file"（踩过一次）。 */
#include <QtGui/QFont>
#include <QtGui/QFontMetrics>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace cicadaqt {

    namespace {

        /* 引擎的像素宽度回调：宿主没给 TextMetrics 时的兜底量法。
           引擎里本来有个"码点数 × 字号 × 0.6"的兜底，这里不用它，是因为 QML 侧
           一旦接上 TextMetrics 就会用 setMeasuredWidth() 覆盖 —— 两条路留一条就够。 */

        inline QString typeNameOf(int type)
        {
            switch (type) {
                case DanmakuTypeScroll:
                    return QStringLiteral("scroll");
                case DanmakuTypeTop:
                    return QStringLiteral("top");
                case DanmakuTypeBottom:
                    return QStringLiteral("bottom");
                case DanmakuTypeAdvanced:
                    return QStringLiteral("advanced");
                default:
                    return QStringLiteral("unknown");
            }
        }

        /*
         * RGBA → "#rrggbb" 的缓存（降 CPU 用）。
         *
         * 颜色就那么几种（白、红、橙、黄、绿、蓝、紫…），但原来**每帧每条弹幕**都要
         * `QString::fromStdString(danmakuColorToHex(rgba))` —— 一次 std::string 构造
         * 加一次 QString 构造，纯属白烧。这里按 RGBA 值查表，命中就只是两次哈希查找。
         *
         * 超过 256 种就整表清掉：正常播放永远到不了这个数，只是防"有人灌一堆随机颜色"
         * 把这张表无限撑大（清掉重建比无界增长划算）。
         */
        inline QString cachedColorHex(quint32 rgba)
        {
            static QHash<quint32, QString> cache;

            const auto it = cache.constFind(rgba);

            if (it != cache.constEnd())
                return it.value();

            if (cache.size() >= 256)
                cache.clear();

            const QString hex = QString::fromStdString(danmakuColorToHex(rgba));
            cache.insert(rgba, hex);
            return hex;
        }

        inline int typeFromName(const QVariant &value)
        {
            const QString name = value.toString().toLower();

            if (name == QLatin1String("top") || name == QLatin1String("2"))
                return DanmakuTypeTop;
            if (name == QLatin1String("bottom") || name == QLatin1String("3"))
                return DanmakuTypeBottom;
            if (name == QLatin1String("advanced") || name == QLatin1String("4"))
                return DanmakuTypeAdvanced;

            /* 数字、或者 "scroll"/"1" 都按滚动处理（默认就是滚动） */
            bool ok = false;
            const int numeric = value.toInt(&ok);

            if (ok && numeric >= DanmakuTypeScroll && numeric <= DanmakuTypeAdvanced)
                return numeric;

            return DanmakuTypeScroll;
        }

        /* QML 里颜色可能是 "#rrggbb" / "#aarrggbb" / 数字色号 —— 引擎提供了解析 */
        inline uint32_t colorOf(const QVariant &value, uint32_t fallback)
        {
            if (!value.isValid() || value.isNull())
                return fallback;

            if (value.canConvert<int>()) {
                bool ok = false;
                const int numeric = value.toInt(&ok);

                if (ok)
                    return danmakuColorFromRgb(static_cast<uint8_t>((numeric >> 16) & 0xFF),
                                               static_cast<uint8_t>((numeric >> 8) & 0xFF),
                                               static_cast<uint8_t>(numeric & 0xFF));
            }

            return danmakuColorFromHex(value.toString().toStdString(), fallback);
        }

        /*
         * 一次**异步**弹幕加载任务（跑在 QThreadPool 的工作线程上）。
         *
         * 只做三件纯计算的事：取字节（读文件，或者直接用调用方给的 XML 文本）、解析、
         * 建列表。**不碰引擎、不碰 QML、不碰任何界面对象** —— 结果通过
         * DanmakuController::onDanmakuParsed() 交回 GUI 线程，那边才喂引擎。
         *
         * 【为什么用 QRunnable + 全局线程池，而不是自己养一条常驻线程】
         * 一次打开视频才解析一次（几毫秒到几十毫秒），池子里跑完就把线程还回去；
         * 常驻线程要自己管生命周期，收益为零。
         *
         * 【生命周期】任务只活到 run() 结束；宿主析构时会 voidForDone（见
         * DanmakuController::~DanmakuController），所以 run() 里那个 this 指针不会悬空。
         */
        class DanmakuParseTask : public QRunnable {
        public:
            DanmakuParseTask(DanmakuController *owner, int token, const QString &path, const QString &xml)
                : m_owner(owner), m_token(token), m_path(path), m_xml(xml)
            {
            }

            void run() override
            {
                QByteArray bytes;
                QString error;

                if (m_path.isEmpty()) {
                    /* 网络那条路（mock 的弹幕列表）：调用方已经把响应文本拿来了 */
                    bytes = m_xml.toUtf8();
                } else {
                    QFile file(m_path);

                    if (!file.open(QIODevice::ReadOnly)) {
                        /*
                         * 分两种报，免得"文件不存在"和"路径拼错/没权限"混成一句：
                         * QML 侧传进来的**必须是本地路径**（不带 file: scheme）。
                         */
                        error = QFile::exists(m_path)
                                    ? QStringLiteral("打不开弹幕文件（存在但打不开，权限/占用？）：") + m_path
                                    : QStringLiteral("弹幕文件不存在：") + m_path;
                    } else {
                        bytes = file.readAll();
                    }
                }

                std::vector<DanmakuItem> items;

                if (error.isEmpty()) {
                    /* 纯函数、不碰全局状态，任何线程都能调（见 DanmakuXml.h 的"线程"那条）。
                       同名 xml 和 mock 那份 XML 是同一种格式，所以两条路共用这一个解析器。 */
                    items = danmakuParseBilibiliXml(
                        std::string(bytes.constData(), static_cast<size_t>(bytes.size())));
                }

                m_owner->onDanmakuParsed(m_token, std::move(items), error);
            }

        private:
            DanmakuController *m_owner = nullptr;
            int m_token = 0;
            QString m_path;
            QString m_xml;
        };

    }// namespace

    DanmakuController::DanmakuController(QObject *parent)
        : QObject(parent)
    {
        /* 引擎默认值就是参考实现那一套；这里只把"宿主必须显式给"的容器尺寸对齐到 16:9 */
        m_engine.setContainerSize(1280.0f, 720.0f);

        /*
         * 【接上真实文字测量】引擎自己只能按"码点数 × 字号"估算宽度，估出来偏小 ——
         * 本人的白框就比真实文字窄、右边漏出一截（用户反馈的正是这个）。
         * 这里用 QFontMetricsF 按"QML 正在用的字体族 + 这条弹幕的字号 + 是否粗体"量真值；
         * 引擎会把结果按弹幕 id 缓存，所以一条只量一次（fontFamily/粗体变了会清缓存重来）。
         */
        m_engine.setMeasureFunction([this](const std::string &text, int fontPixelSize, bool bold) {
            return measureTextWidth(text, fontPixelSize, bold);
        });

        applyConfig();
        refreshActiveItems();
    }

    float DanmakuController::measureTextWidth(const std::string &text, int fontPixelSize, bool bold) const
    {
        if (text.empty())
            return 0.0f;

        QFont font(m_fontFamily);

        font.setPixelSize(fontPixelSize > 0 ? fontPixelSize : 18);
        font.setBold(bold);

        const QString str = QString::fromUtf8(text.data(), static_cast<int>(text.size()));

        return static_cast<float>(QFontMetricsF(font).horizontalAdvance(str));
    }

    void DanmakuController::setFontFamily(const QString &family)
    {
        if (m_fontFamily == family)
            return;

        m_fontFamily = family;

        /* 宽度全变了：把引擎里量好的缓存清掉，让它按新字体重量一遍，然后重排在屏弹幕 */
        m_engine.clearMeasuredWidths();
        m_engine.rescaleActive();
        refreshActiveItems();
        emit fontFamilyChanged();
    }

    DanmakuController::~DanmakuController()
    {
        /*
         * 解析任务在工作线程上跑，它手里有 this —— 析构前必须等它们全部结束，
         * 否则最后那几毫秒里回来的结果会碰到一个已经在拆的对象（崩在随机地方）。
         * 解析本身只有几毫秒，这里等一下可以接受（只在关窗/退出时发生一次）。
         * 排队等着的 consumeParsedDanmaku() 不用管：QObject 析构会把发给自己的
         * 待处理事件一起删掉。
         */
        QThreadPool::globalInstance()->waitForDone();

        /* 在飞的任务即使还在跑，拿到的 token 也已经过期，结果会被丢掉 */
        ++m_loadToken;
    }

    /* ------------------------------------------------------------------ */
    /* 属性                                                                */
    /* ------------------------------------------------------------------ */

    void DanmakuController::setEnabled(bool value)
    {
        if (m_enabled == value)
            return;

        m_enabled = value;
        emit enabledChanged();
        refreshActiveItems();
    }

    void DanmakuController::setGpuLayer(bool value)
    {
        if (m_gpuLayer == value)
            return;

        m_gpuLayer = value;

        /*
         * 切回 QML 模式（renderMode: "qml" 的回退路径）时，下一帧必须**全量**下发：
         * QML 那套"只有槽位换了弹幕才重写 text/color"靠 m_lastIds 去重，而 m_lastIds
         * 是在"建列表"那条路上维护的 —— GPU 模式下它没被更新过，要是不清掉，
         * QML 会以为这些条目都见过，第一帧不给 text，屏幕上就是一条条空白。
         */
        if (!m_gpuLayer)
            m_lastIds.clear();

        emit gpuLayerChanged();
        refreshActiveItems();
    }

    void DanmakuController::setAreaRatio(qreal value)
    {
        value = qBound(0.1, value, 1.0);

        if (qFuzzyCompare(m_areaRatio, value))
            return;

        m_areaRatio = value;
        applyConfig();
        emit areaRatioChanged();
    }

    void DanmakuController::setSpeedMultiplier(qreal value)
    {
        if (qFuzzyCompare(m_speedMultiplier, value))
            return;

        m_speedMultiplier = value;
        applyConfig();
        emit speedMultiplierChanged();
    }

    void DanmakuController::setFontSizeScale(qreal value)
    {
        if (qFuzzyCompare(m_fontSizeScale, value))
            return;

        m_fontSizeScale = value;
        applyConfig();
        /*
         * ★ 改字号必须先把引擎里"量好的文字宽度"清掉 ★
         *
         * 引擎的 _measuredWidths 是**按弹幕 id 缓存**的（`measureBoxTextWidth` 先查它），
         * 缓存里不含字号 —— 不清的话，量过的那条会一直用**旧字号**的宽度：图集按新字号
         * 重烤了文字、盒子却还是旧尺寸，表现就是用户报的"文字和边框不匹配，字号设置没生效"。
         * 清掉之后 rescaleActive() 会按新字号重新量一遍，盒子和文字才重新对上。
         */
        m_engine.clearMeasuredWidths();
        /* 已经上屏的由 rescaleActive() 在现场重算（和 QML 版行为一致） */
        m_engine.rescaleActive();
        emit fontSizeScaleChanged();
        refreshActiveItems();
    }

    void DanmakuController::setAutoScale(bool value)
    {
        if (m_autoScale == value)
            return;

        m_autoScale = value;
        applyConfig();
        /* 同上：自动缩放动的也是字号 → 量宽缓存必须作废 */
        m_engine.clearMeasuredWidths();
        m_engine.rescaleActive();
        emit autoScaleChanged();
        refreshActiveItems();
    }

    void DanmakuController::setBaseFontSize(int value)
    {
        /* 参考里字号范围就是 12~32（设置面板滑条也是这个区间） */
        value = qBound(12, value, 32);

        if (m_baseFontSize == value)
            return;

        m_baseFontSize = value;
        applyConfig();
        /*
         * 【这里以前漏了两步】只 applyConfig() 的话：
         *   * 引擎里按 id 缓存着的旧宽度继续生效 → 盒子尺寸不跟着字号走（"边框和文字不匹配"）；
         *   * 已经在屏上的弹幕不会重新量/重新排版 → "字号设置没生效"。
         * 设置面板里拖字号滑条是实时生效的，这两步必须跟上。
         */
        m_engine.clearMeasuredWidths();
        m_engine.rescaleActive();
        emit baseFontSizeChanged();
        refreshActiveItems();
    }

    void DanmakuController::setDanmakuOpacity(qreal value)
    {
        value = qBound(0.0, value, 1.0);

        if (qFuzzyCompare(m_danmakuOpacity, value))
            return;

        m_danmakuOpacity = value;
        applyConfig();
        emit danmakuOpacityChanged();
        refreshActiveItems();
    }

    void DanmakuController::setFontBold(bool value)
    {
        if (m_fontBold == value)
            return;

        m_fontBold = value;
        applyConfig();
        /*
         * 粗体也会改变文字宽度（引擎量宽时把 bold 传给了宿主），所以量宽缓存同样要作废，
         * 否则"打开粗体"之后盒子还是细体的宽度 → 白框和文字又对不上。
         */
        m_engine.clearMeasuredWidths();
        m_engine.rescaleActive();
        emit fontBoldChanged();
        refreshActiveItems();
    }

    void DanmakuController::setOutlineType(int value)
    {
        if (m_outlineType == value)
            return;

        m_outlineType = value;
        applyConfig();
        emit outlineTypeChanged();
        refreshActiveItems();
    }

    void DanmakuController::setBlockScroll(bool value)
    {
        if (m_blockScroll == value)
            return;
        m_blockScroll = value;
        applyConfig();
        emit filterChanged();
        refreshActiveItems();
    }

    void DanmakuController::setBlockFixed(bool value)
    {
        if (m_blockFixed == value)
            return;
        m_blockFixed = value;
        applyConfig();
        emit filterChanged();
        refreshActiveItems();
    }

    void DanmakuController::setBlockColorful(bool value)
    {
        if (m_blockColorful == value)
            return;
        m_blockColorful = value;
        applyConfig();
        emit filterChanged();
        refreshActiveItems();
    }

    void DanmakuController::setBlockAdvanced(bool value)
    {
        if (m_blockAdvanced == value)
            return;
        m_blockAdvanced = value;
        applyConfig();
        emit filterChanged();
        refreshActiveItems();
    }

    /* ------------------------------------------------------------------ */
    /* 配置同步                                                            */
    /* ------------------------------------------------------------------ */

    void DanmakuController::applyConfig()
    {
        DanmakuConfig config = m_engine.config();

        config.areaRatio = static_cast<float>(m_areaRatio);
        config.opacity = static_cast<float>(m_danmakuOpacity);
        config.fontBold = m_fontBold;
        config.outlineType = m_outlineType;
        config.fontSizeScale = static_cast<float>(m_fontSizeScale);
        config.autoScale = m_autoScale;
        config.baseFontSize = m_baseFontSize;

        /*
         * 速度：引擎的 `speed` / `defaultSpeed` 是**1..5 的档位**（DanmakuEngine.cpp:970-976：
         * 越界才退回 defaultSpeed，取速度是 speedMultipliers[useSpeed-1]），
         * 也就是说 3 = 1.0 倍（和 DanmakuConfig 的默认值一致）。
         * 【踩过的坑】一开始我把 0..4 的下标直接赋给 defaultSpeed，等于整体慢了/快了一档，
         * 而且 QML 传 1.0（第 3 档）会被算成第 2 档 —— 所以这里要 +1。
         */
        const float multipliers[] = { 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
        const float wanted = static_cast<float>(m_speedMultiplier);
        int best = 2;

        float bestDelta = std::fabs(multipliers[2] - wanted);

        for (int i = 0; i < 5; ++i) {
            const float delta = std::fabs(multipliers[i] - wanted);

            if (delta < bestDelta) {
                bestDelta = delta;
                best = i;
            }
        }

        config.defaultSpeed = best + 1;

        /* 屏蔽开关：true = 屏蔽这一类（引擎语义，见 DanmakuController.h 的说明） */
        DanmakuFilter filter;
        filter.scroll = m_blockScroll;
        filter.top = m_blockFixed;
        filter.bottom = m_blockFixed;
        filter.fixed = m_blockFixed;
        filter.colorful = m_blockColorful;
        filter.advanced = m_blockAdvanced;
        m_engine.setFilter(filter);

        m_engine.setConfig(config);
    }

    /* ------------------------------------------------------------------ */
    /* 每帧结果 → QML                                                      */
    /* ------------------------------------------------------------------ */

    /*
     * 引擎结果 → QML。**每个 tick（30Hz）都被 update() 调一次**，是这条链路的 CPU 热点。
     *
     * 【本轮的优化：fullPayload】
     *
     * QVariantMap 是 QMap（红黑树）：插 20 个键 = 20 次树节点分配 + 20 次键比较，
     * 每条弹幕每帧来一遍；其中还有两次**字符串构造**
     * （QString::fromUtf8(文字) 和 QString::fromStdString(颜色 hex)）。
     * 而 QML 那边 present() 只在"这一格换了另一条弹幕 / 样式变了"（rebound）时
     * 才去读 text / color / isSelf / textAdvance / h 这几个**整条弹幕不变**的字段
     * —— 就在 DanmakuView.qml 的 `if (!rebound) continue` 后面那一段。
     *
     * 所以稳态帧只推"每帧真的会变"的那 10 个键，其余 5 个只在 fullPayload 为真时带上：
     *   * update() 传 false（播放中的常规帧）；
     *   * 其它入口（load / loadXmlFile / send / seekTo / clear / 各种设置 setter）
     *     用默认的 true —— 加载、跳转、改样式都会让 QML 那边 rebound，必须给全量。
     *
     * 【新入场的必须给全量】fullPayload 为假时再看一眼 id：上一帧没出现过的 id
     * （这一帧刚上屏的那几条）照样给全量字段，否则它们第一帧没有 text 就是一条空白。
     *
     * ⚠️ 下面这 10 个键**任何一帧都不能少**，理由都是"QML 在 rebound 判断之前就会读到它"：
     *     id        —— 算 rebound 的 key
     *     fontSize  —— 同上（缺了会退化成默认 18，于是每帧都判定成 rebound）
     *     type      —— present() 第 337 行就算 isScroll；缺了会被当成固定弹幕（弹幕当场不动）
     *     durationMs—— 重新对齐动画时用它算剩余时长（见 present() 第 375 行）；缺了会退化成 0
     *     x/y/w/alpha/progress/hovered —— 每帧都会读
     * 少任何一个都会出现"弹幕不显示 / 不动"这类现象，别顺手删。
     */
    void DanmakuController::refreshActiveItems(bool fullPayload)
    {
        const std::vector<RenderItem> &items = m_engine.activeItems();
        QVariantList list;
        QSet<qint64> currentIds;

        /*
         * GPU 渲染路径下这张列表**没有任何消费者**（见 gpuLayer 的 Q_PROPERTY 说明），
         * 所以直接不建 —— 建一次就是每条 10 个键的 QVariantMap。
         * 统计和 frameChanged 照旧：悬停气泡（hoveredItem）和诊断浮层（stats）靠它们。
         */
        if (m_enabled && !m_gpuLayer) {
            list.reserve(static_cast<int>(items.size()));
            currentIds.reserve(static_cast<int>(items.size()));

            for (size_t i = 0; i < items.size(); ++i) {
                const RenderItem &item = items[i];
                QVariantMap map;

                /* ---- 每帧都推（10 个，见函数头那段"不能少"的说明）---- */
                map.insert(QStringLiteral("id"), QVariant::fromValue(item.id));
                map.insert(QStringLiteral("fontSize"), item.fontPixelSize);
                map.insert(QStringLiteral("type"), typeNameOf(item.type));
                map.insert(QStringLiteral("durationMs"), item.durationMs);
                map.insert(QStringLiteral("x"), item.x);
                map.insert(QStringLiteral("y"), item.y);
                map.insert(QStringLiteral("w"), item.width);
                map.insert(QStringLiteral("alpha"), item.alpha);
                map.insert(QStringLiteral("progress"), item.progress);
                map.insert(QStringLiteral("hovered"), item.hovered);

                currentIds.insert(item.id);

                /* ---- 只在"全量帧"或"这条刚上屏"时推 ----
                   QML 那边读它们的唯一时机就是 rebound（见 DanmakuView.qml）。 */
                if (fullPayload || !m_lastIds.contains(item.id)) {
                    /* 文字本体宽度：渲染侧不用再量一遍（QML 的 implicitWidth 要等一次布局，
                       引擎这帧就要定位，等不了）。以前漏了这一项，QML 那边赋值 undefined
                       直接抛错、把整个摆位循环中断了 —— 表现是"一条都不显示"。 */
                    map.insert(QStringLiteral("textAdvance"), item.textAdvance);
                    map.insert(QStringLiteral("h"), item.height);
                    map.insert(QStringLiteral("text"),
                               QString::fromUtf8(item.text, static_cast<int>(item.textLength)));
                    map.insert(QStringLiteral("color"), cachedColorHex(item.colorRGBA));
                    map.insert(QStringLiteral("isSelf"), item.isSelf);
                }

                list.append(map);
            }
        }

        m_lastIds = currentIds;
        m_activeItems = list;
        ++m_frameRevision;

        const DanmakuStats stats = m_engine.stats();
        QVariantMap statsMap;

        statsMap.insert(QStringLiteral("activeCount"), m_enabled ? stats.activeCount : 0);
        statsMap.insert(QStringLiteral("trackCount"), stats.trackCount);
        statsMap.insert(QStringLiteral("maxTrackCount"), stats.maxTrackCount);
        statsMap.insert(QStringLiteral("admitted"), stats.admitted);
        statsMap.insert(QStringLiteral("dropped"), stats.dropped);
        statsMap.insert(QStringLiteral("pending"), stats.pending);
        statsMap.insert(QStringLiteral("itemCount"), static_cast<qint64>(m_engine.itemCount()));

        /*
         * stats 只在**真的变了**的时候才发信号：这个属性也挂着 QML 绑定，
         * 每帧都 emit 一次就是让 QML 每帧把这张 map 再转一遍 JS 对象（白烧）。
         * 播放中它绝大多数帧都是同一个值（就 pending/activeCount 偶尔动一下）。
         */
        /*
         * 【★ GPU 模式：这两个信号一个都不发 ★】
         *
         * 堆自检（_CrtCheckMemory）已经证明"不是内存被写坏"，那崩溃就只能是
         * QML 的 JS 在**执行中**踩到失效对象/爆栈（日志里 22 层栈全在 QV6Qml 的 JS 引擎、
         * 且带 _chkstk 栈探测）。而这些 JS 全部是被**这两个信号**驱动的：
         *   * `frameChanged` → QML `onFrameChanged`（GPU 模式下本来就是空操作）
         *   * `statsChanged` → `_onscreenCount` 绑定 → 每帧去读 `engine.stats` 那张 map
         * 而 GPU 渲染路径**根本不消费它们**（渲染器直接读引擎数据，见 gpuLayer 的说明）。
         * 所以 GPU 模式下干脆不发：JS 侧的每帧回调与绑定重算**全部消失**，
         * 既去掉崩溃那条路，也把 CPU 再降一截（用户原本就是这么要求的）。
         * qml 回退路径照旧发（它需要 frameChanged 来调 present()）。
         */
        if (statsMap != m_stats) {
            m_stats = statsMap;
            emit statsChanged();
        }

        emit frameChanged();
    }

    /* ------------------------------------------------------------------ */
    /* QML 接口                                                            */
    /* ------------------------------------------------------------------ */

    void DanmakuController::setContainerSize(qreal width, qreal height)
    {
        m_engine.setContainerSize(static_cast<float>(width), static_cast<float>(height));
        /* 尺寸变了在屏弹幕要重算位置（和 QML 版 _rescaleActive 一致） */
        m_engine.rescaleActive();
        refreshActiveItems();
    }

    void DanmakuController::load(const QVariantList &items)
    {
        /* 同步灌数据 = 作废在飞的异步加载（结果回来会被丢掉，别让它盖掉这一份） */
        ++m_loadToken;

        std::vector<DanmakuItem> list;

        list.reserve(static_cast<size_t>(items.size()));

        for (int i = 0; i < items.size(); ++i) {
            const QVariantMap map = items.at(i).toMap();
            DanmakuItem item;

            item.text = map.value(QStringLiteral("text")).toString().toStdString();
            item.timeMs = map.value(QStringLiteral("timeMs")).toLongLong();
            item.type = typeFromName(map.value(QStringLiteral("type")));
            item.fontSize = map.value(QStringLiteral("fontSize")).toInt();
            item.colorRGBA = colorOf(map.value(QStringLiteral("color")), 0xFFFFFFFFu);
            item.isSelf = map.value(QStringLiteral("isSelf")).toBool();
            item.uid = map.value(QStringLiteral("uid")).toString().toStdString();
            item.id = map.value(QStringLiteral("id")).toLongLong();

            if (item.id == 0)
                item.id = 1000 + i;

            item.speed = map.value(QStringLiteral("speed"), -1).toInt();
            list.push_back(item);
        }

        m_engine.load(list);
        refreshActiveItems();
    }

    int DanmakuController::loadXmlFile(const QString &path)
    {
        /* 同步加载 = 作废在飞的异步加载（见 load() 里同样的说明） */
        ++m_loadToken;

        QFile file(path);

        if (!file.open(QIODevice::ReadOnly)) {
            /*
             * 分两种打，免得"文件不存在"和"路径拼错/没权限"混成一句看不出原因：
             * QML 侧传进来的**必须是本地路径**（不带 file: scheme）；
             * 以前那边拼出过 "///D:/…"（前缀重复），日志只显示"打不开"，很难查。
             */
            if (!QFile::exists(path)) {
                qWarning("[danmaku] 弹幕文件不存在：%s", qPrintable(path));
            } else {
                qWarning("[danmaku] 打不开弹幕文件（存在但打不开，权限/占用？）：%s", qPrintable(path));
            }

            return 0;
        }

        const QByteArray bytes = file.readAll();

        /* 解析全在纯 C++ 库那边（不碰 Qt），这里只把字节递进去 */
        const std::vector<DanmakuItem> list =
            danmakuParseBilibiliXml(std::string(bytes.constData(), static_cast<size_t>(bytes.size())));

        if (list.empty()) {
            qWarning("[danmaku] 弹幕文件里没有有效弹幕：%s", qPrintable(path));
            return 0;
        }

        m_engine.load(list);
        refreshActiveItems();
        return static_cast<int>(list.size());
    }

    int DanmakuController::loadXmlFileAsync(const QString &path)
    {
        const int token = ++m_loadToken;

        /*
         * 立刻返回：读文件 + 解析都在池子里的工作线程上做（见 DanmakuParseTask）。
         * 结果回来时如果 m_loadToken 已经不是这个 token 了（中间又发起过一次加载），
         * 它会**被丢掉** —— 换片之后才回来的上一部的弹幕不能上屏。
         */
        QThreadPool::globalInstance()->start(new DanmakuParseTask(this, token, path, QString()));

        return token;
    }

    int DanmakuController::loadXmlTextAsync(const QString &xml)
    {
        const int token = ++m_loadToken;

        QThreadPool::globalInstance()->start(new DanmakuParseTask(this, token, QString(), xml));

        return token;
    }

    /* 【工作线程】解析完了：塞进队列 + 往 GUI 线程排一次（这里绝不能碰引擎） */
    void DanmakuController::onDanmakuParsed(int token, std::vector<DanmakuItem> items, const QString &error)
    {
        {
            QMutexLocker locker(&m_loadMutex);

            ParsedLoad entry;
            entry.token = token;
            entry.items = std::move(items);
            entry.error = error;

            m_parsedQueue.append(std::move(entry));
        }

        /*
         * 【必须排队，不能直接调用】
         *
         * 解析在工作线程上跑（见 loadXmlFileAsync），所以结果必须投递回 GUI 线程。
         * **更关键的是顺序**：`_sidecarToken = loadXmlFileAsync(...)` 这句
         * （Main.qml 里）要等函数**返回之后**才拿到 token；如果在这里直接同步调用
         * consumeParsedDanmaku()，失败/成功的信号会在函数返回之前就发出去 ——
         * QML 那边 `token === root._sidecarToken` 永远不成立，于是
         *   * 失败时不会改走 mock 兜底（用户实测：**弹幕一条都没有**），
         *   * 成功时也不会打"载入 N 条"那行日志。
         * 试过一次"改同步"，就是这么把弹幕彻底弄没的 —— 别再改回去。
         */
        QMetaObject::invokeMethod(this, [this]() { consumeParsedDanmaku(); }, Qt::QueuedConnection);
    }

    /* 【GUI 线程】把攒下的结果喂引擎 */
    void DanmakuController::consumeParsedDanmaku()
    {
        QVector<ParsedLoad> done;

        {
            QMutexLocker locker(&m_loadMutex);
            done.swap(m_parsedQueue);
        }

        for (ParsedLoad &loaded : done) {
            /* 过期的那次（这中间又发起过一次加载）直接丢掉 —— 结果留着也没人要 */
            if (loaded.token != m_loadToken)
                continue;

            if (!loaded.error.isEmpty()) {
                /* 原因不在这里打：宿主拿到 reason 之后会带着上下文（是哪个文件、接下来怎么办）打一行 */
                emit danmakuLoadFailed(loaded.token, loaded.error);
                continue;
            }

            if (loaded.items.empty()) {
                /* 文件/响应里一个有效 <d> 都没有：交给宿主决定下一步（去拉 mock / 就这样） */
                emit danmakuLoaded(loaded.token, 0);
                continue;
            }

            /*
             * 到这一步才碰引擎。这一步本身很轻：几百上千条就是一次 vector 搬移 + 一次排序，
             * 重的那部分（读文件、解析文本、建上千个字符串/对象）已经在工作线程上做完了 ——
             * 打开视频不再卡一下，靠的就是这个分工。
             */
            const int count = static_cast<int>(loaded.items.size());

            {
                /* 换整批数据也会重写 _renderOut（而且这一下最猛：1000 条一起进来），
                   必须和渲染线程互斥 —— 用户那边"载入 1000 条之后 0.2 秒必崩"就是这个窗口。 */
                QMutexLocker renderLocker(&danmakuRenderMutex());

                m_engine.load(loaded.items);
                refreshActiveItems();
            }

#if defined(_MSC_VER) && defined(_DEBUG)
            /*
             * 【把"哪一步写坏堆"钉死】
             *
             * Debug CRT 会校验整个堆。`_CrtCheckMemory()` 返回 false 就说明**此刻之前**
             * 有人写越界了。在"载入整批弹幕"这一步之后立刻查一次：
             *   * 这里就报 → 根因在 DanmakuEngine::load()（纯 C++ 引擎里）；
             *   * 这里不报、而后面每帧 update() 时报 → 根因在准入/分轨/渲染那条链上。
             * 不用装任何东西、也不改行为（只有 Debug 构建、且只查一次）。
             */
            if (!_CrtCheckMemory()) {
                qWarning("[heap] 堆在 \"载入 1000 条弹幕\" 这一步之后就坏了 —— 根因在 DanmakuEngine::load()\n");
            }
#endif

            emit danmakuLoaded(loaded.token, count);
        }
    }

    void DanmakuController::append(const QVariantMap &item, qint64 currentMs)
    {
        DanmakuItem entry;

        entry.text = item.value(QStringLiteral("text")).toString().toStdString();
        entry.timeMs = item.value(QStringLiteral("timeMs"), currentMs).toLongLong();
        entry.type = typeFromName(item.value(QStringLiteral("type")));
        entry.fontSize = item.value(QStringLiteral("fontSize")).toInt();
        entry.colorRGBA = colorOf(item.value(QStringLiteral("color")), 0xFFFFFFFFu);
        entry.isSelf = item.value(QStringLiteral("isSelf")).toBool();
        entry.uid = item.value(QStringLiteral("uid")).toString().toStdString();
        entry.id = item.value(QStringLiteral("id")).toLongLong();
        entry.speed = item.value(QStringLiteral("speed"), -1).toInt();

        if (m_engine.addDanmaku(entry, currentMs))
            refreshActiveItems();
    }

    void DanmakuController::clear()
    {
        /* 清屏也作废在飞的异步加载：否则"清空"之后旧结果又把它灌回来了 */
        ++m_loadToken;

        m_engine.clear();
        refreshActiveItems();
    }

    void DanmakuController::reset()
    {
        ++m_loadToken;

        m_engine.reset();

        /* 时钟被清回 0 → 基准要跟着重记（否则渲染器会拿着 seek 前的时间戳去外推） */
        m_engineClockMs = m_engine.clockMs();
        m_engineClockMonoMs = danmakuMonoMs();

        refreshActiveItems();
    }

    void DanmakuController::update(qint64 timeMs)
    {
        /* 引擎这一帧的重写（含 _renderOut 向量）必须与渲染线程的读取互斥：
           渲染器读的正是这个向量，见 danmakuRenderMutex() 的说明。 */
        QMutexLocker renderLocker(&danmakuRenderMutex());

        m_engine.update(timeMs);

        /*
         * 【把"引擎这一刻的时钟"连同墙钟时间戳一起存下来 —— 渲染器的时间基准】
         *
         * 取 clockMs() 而不是入参 timeMs：引擎在暂停分支（DanmakuEngine.cpp:294-297）
         * 和 seek 分支里自己决定时钟停在哪/跳到哪，渲染器必须跟着**引擎实际的**时钟走，
         * 否则暂停时它还会按入参往前外推，弹幕就又飘了。
         * 成对写（值 + 时间戳）的理由见 DanmakuController.h 里 engineClockMs() 的说明。
         */
        m_engineClockMs = m_engine.clockMs();
        m_engineClockMonoMs = danmakuMonoMs();

#if defined(_MSC_VER) && defined(_DEBUG)
        /*
         * 每帧（Debug 构建）再查一次堆：如果"载入那一步"没报、这里报了，
         * 就说明写坏堆的是**每帧这条链**（准入/分轨/渲染取数），而不是 load()。
         * 只在真的坏掉那一刻打印（正常播放一句都不会有，CPU 开销是一次堆校验——
         * 如果嫌慢，定位完我会把这一段删掉）。
         */
        static int heapCheckTick = 0;

        if (++heapCheckTick >= 60) {
            heapCheckTick = 0;

            if (!_CrtCheckMemory()) {
                qWarning("[heap] 堆在每帧 update() 之后坏了 —— 根因在准入/分轨/渲染这条链\n");
            }
        }
#endif
        /*
         * **false** = 播放中的常规帧：只推"每帧真的会变"的那 10 个字段。
         * 这是本轮降 CPU 的关键一处（30Hz × 屏上条数 × 20 次红黑树插入 → 10 次，
         * 且去掉了两次字符串构造），详见 refreshActiveItems 的说明。
         * 别改成默认的 true —— 那就等于每帧又把全量字段重建一遍。
         */
        refreshActiveItems(false);
    }

    void DanmakuController::seekTo(qint64 timeMs)
    {
        /* 同上：清场/重排也会重写 _renderOut，必须和渲染线程互斥 */
        QMutexLocker renderLocker(&danmakuRenderMutex());

        m_engine.seekTo(timeMs);

        /*
         * seek 之后引擎时钟直接跳过去了：基准必须当场重记。
         * 不重记的话，渲染器会拿"seek 之前那个时间戳"去算已经流逝了多久，弹幕会先错位一帧。
         */
        m_engineClockMs = m_engine.clockMs();
        m_engineClockMonoMs = danmakuMonoMs();

        refreshActiveItems();
    }

    bool DanmakuController::send(const QString &text, const QVariantMap &options)
    {
        DanmakuSendOptions sendOptions;

        sendOptions.type = typeFromName(options.value(QStringLiteral("type"), QStringLiteral("scroll")));
        sendOptions.fontSize = options.value(QStringLiteral("fontSize")).toInt();
        sendOptions.speed = options.value(QStringLiteral("speed"), -1).toInt();
        sendOptions.colorRGBA = colorOf(options.value(QStringLiteral("color")), 0xFFFFFFFFu);
        /* 自己发的：true —— 弹幕层会给它加白框（和 QML 版一致） */
        sendOptions.self = options.value(QStringLiteral("self"), true).toBool();
        sendOptions.timeMs = options.value(QStringLiteral("timeMs"), -1).toLongLong();
        sendOptions.uid = options.value(QStringLiteral("uid"), QStringLiteral("1")).toString().toStdString();
        sendOptions.id = options.value(QStringLiteral("id")).toLongLong();

        /*
         * 引擎的裁决如实返回（false = 被屏蔽规则挡了 / 同屏条数到顶 / 类型非法）。
         * 返回 false 时也要 refresh 一次：调用方可能正在等界面更新，而这一帧的
         * activeItems 至少是"当前真实在屏的集合"。
         */
        const bool accepted = m_engine.sendDanmaku(text.toStdString(), sendOptions);

        refreshActiveItems();

        if (!accepted) {
            const DanmakuStats s = m_engine.stats();
            qWarning("[danmaku] 引擎没有接受这条弹幕（active=%d dropped=%d maxRenderCount 可能到顶，"
                     "或它被屏蔽规则挡了）", s.activeCount, s.dropped);
        }

        return accepted;
    }

    /*
     * 悬停中那条弹幕的信息（弹幕悬停气泡用）。
     *
     * 【为什么在这里扫一遍列表】引擎已经算好"命中哪一条"（syncHover 的矩形包含检测 +
     * 悬停冻结），hoveredId() 就是它；这里只是把那条的几何/文字/本人标记取出来给 QML。
     * 不在 QML 里自己再命中一次：两套命中逻辑迟早会不一致（引擎那份还负责冻结）。
     */
    QVariantMap DanmakuController::hoveredItem() const
    {
        QVariantMap map;

        const int64_t id = m_engine.hoveredId();

        if (id == 0)
            return map;

        const std::vector<RenderItem> &items = m_engine.activeItems();

        for (size_t i = 0; i < items.size(); ++i) {
            const RenderItem &item = items[i];

            if (item.id != id)
                continue;

            map.insert(QStringLiteral("id"), QVariant::fromValue(item.id));
            map.insert(QStringLiteral("text"),
                       QString::fromUtf8(item.text, static_cast<int>(item.textLength)));
            /* isSelf 就是参考 index.tsx:1045 的 `uid === 1 || uid === '1'`（引擎里同一个判据） */
            map.insert(QStringLiteral("isSelf"), item.isSelf);
            /* 气泡定位用：x/y 是整盒左上角，w/h 是整盒尺寸（参考给的是"中心 x / 底边 y"，
               那一步在 QML 里算，见 DanmakuView.qml 的 danmakuTip） */
            map.insert(QStringLiteral("x"), item.x);
            map.insert(QStringLiteral("y"), item.y);
            map.insert(QStringLiteral("w"), item.width);
            map.insert(QStringLiteral("h"), item.height);
            break;
        }

        return map;
    }

    bool DanmakuController::recallDanmaku(qint64 id)
    {
        if (id == 0)
            return false;

        const bool removed = m_engine.removeDanmaku(int64_t(id));

        if (removed) {
            /* 立刻重建这一帧：被撤的那条要马上从 activeItems 里消失，
               气泡也就不用再指着它了（QML 读 hoveredItem 会立刻变空）。 */
            refreshActiveItems();
        }

        return removed;
    }

    void DanmakuController::generateSample(int count, int durationSec)
    {
        if (count <= 0)
            return;

        const int durationMs = qMax(1000, durationSec * 1000);
        const char *texts[] = { "233333", "前方高能", "awsl", "这是弹幕", "测试一下",
                                "哈哈哈哈", "2333", "打卡", "名场面", "有内味了" };
        const uint32_t colors[] = { 0xFFFFFFFFu, 0xFFFFD700u, 0xFF00A1D6u, 0xFFFF6699u, 0xFF7FFF00u };

        std::vector<DanmakuItem> list;

        list.reserve(static_cast<size_t>(count));

        for (int i = 0; i < count; ++i) {
            DanmakuItem item;

            item.id = 100000 + i;
            item.text = texts[i % 10];
            item.timeMs = (static_cast<int64_t>(durationMs) * i) / count;
            /* 滚动为主，穿插固定弹幕（和 QML 版的示例生成一致的味道） */
            item.type = (i % 7 == 0) ? DanmakuTypeTop : ((i % 11 == 0) ? DanmakuTypeBottom
                                                                      : DanmakuTypeScroll);
            item.colorRGBA = colors[i % 5];
            item.uid = std::string("sample");
            list.push_back(item);
        }

        m_engine.load(list);
        refreshActiveItems();
    }

    void DanmakuController::setPointer(qreal x, qreal y, bool inside)
    {
        m_engine.setPointerPosition(static_cast<float>(x), static_cast<float>(y), inside);
    }

    void DanmakuController::setPaused(bool paused)
    {
        m_engine.setPaused(paused);
    }

}// namespace cicadaqt
