#include "DanmakuAtlas.h"

#include <QtGui/QFont>
#include <QtGui/QFontMetricsF>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGTexture>

#include <utils/frame_work_log.h>

#include <cmath>

namespace cicadaqt {

    namespace {

        /* 图集的初始边长。满一行就往下开新行；整个满了就清空重烤。 */
        constexpr int kAtlasInitialSize = 1024;
        /* 单块小图最多能有多大（超过就不烤，那条不画） */
        constexpr int kMaxTileWidth = 900;

        /*
         * 缓存键：任何会改变"烤出来长什么样"的输入都必须进键，否则会出现
         * "改了字号但画面还是旧的"（这条链路上最容易漏的就是颜色和字体族）。
         */
        QString makeKey(const DanmakuAtlas::Request &r)
        {
            /* \x1f 是单元分隔符，弹幕文字里不会出现，用它当分隔比 '|' 安全 */
            return r.text + QLatin1Char('\x1f')
                   + QString::number(r.color.rgba()) + QLatin1Char('\x1f')
                   + QString::number(r.pixelSize) + QLatin1Char('\x1f')
                   + (r.bold ? QLatin1Char('b') : QLatin1Char('n')) + QLatin1Char('\x1f')
                   + r.fontFamily + QLatin1Char('\x1f')
                   + QString::number(r.outlineType) + QLatin1Char('\x1f')
                   + (r.selfBorder ? QLatin1Char('1') : QLatin1Char('0')) + QLatin1Char('\x1f')
                   + (r.fixedStyle ? QLatin1Char('1') : QLatin1Char('0')) + QLatin1Char('\x1f')
                   + QString::number(r.devicePixelRatio) + QLatin1Char('\x1f')
                   + QString::number(std::lround(r.boxWidth)) + QLatin1Char('x')
                   + QString::number(std::lround(r.boxHeight));
        }

    }// namespace

    DanmakuAtlas::~DanmakuAtlas()
    {
        releaseTexture();
    }

    void DanmakuAtlas::releaseTexture()
    {
        delete m_texture;
        m_texture = nullptr;
    }

    bool DanmakuAtlas::allocate(int w, int h, QRect *out)
    {
        /*
         * 【高度也必须挡住（这条是补的）】
         *
         * w/h 是按 boxWidth × dpr / boxHeight × dpr 算出来的（见 entry()），而下面
         * `m_cursorY + h > m_image.height()` 那条分支只会 resetPacking() 再从头放一块 ——
         * 重置之后 cursorY = 0，`*out` 就成了 QRect(0, 0, w, h)，**h 可以比整张图集还高**
         * （dpr=2 时 boxHeight 768 → h=1536 > 1024）。后果：
         *   * QPainter 有 clipRect，画不出界，**不会越界写内存**（这条我核过）；
         *   * 但 result.uv 的 v 方向会 > 1 → 采样到图集外面（画面花屏）；
         *   * 而且装箱游标被这么一块"超高"的图推走，后面几条会白触发一次整体重烤。
         * 结论：放不下就按"太宽"同一处理 —— 返回 false，那条不画（entry() 会记一条空条目）。
         * 图集尺寸固定是 kAtlasInitialSize × kAtlasInitialSize（resetPacking 只清内容不改尺寸）。
         */
        if (w <= 0 || h <= 0 || w > kMaxTileWidth || h > kAtlasInitialSize)
            return false;

        if (m_image.isNull()) {
            m_image = QImage(kAtlasInitialSize, kAtlasInitialSize, QImage::Format_ARGB32_Premultiplied);
            m_image.fill(Qt::transparent);
            m_dirty = true;
        }

        /* 这一行放不下了 → 换行 */
        if (m_cursorX + w > m_image.width()) {
            m_cursorX = 0;
            m_cursorY += m_rowHeight + 1;
            m_rowHeight = 0;
        }

        /* 整个图集放不下 → 重置装箱（清缓存重烤） */
        if (m_cursorY + h > m_image.height()) {
            resetPacking();
        }

        *out = QRect(m_cursorX, m_cursorY, w, h);
        m_cursorX += w + 1;
        m_rowHeight = qMax(m_rowHeight, h);
        return true;
    }

    void DanmakuAtlas::resetPacking()
    {
        AF_LOGW("DanmakuAtlas: atlas is full (%dx%d, %d entries), clearing and re-baking\n",
                m_image.width(), m_image.height(), m_cache.size());

        m_cache.clear();
        m_image.fill(Qt::transparent);
        m_cursorX = 0;
        m_cursorY = 0;
        m_rowHeight = 0;
        m_dirty = true;
        /* 世代 +1：渲染器看到它变了就会重建顶点缓冲（旧 UV 全废了） */
        ++m_generation;
    }

    /*
     * 把一条弹幕画进 target 那块区域。
     *
     * 这里逐条复刻 DanmakuItem.qml 的画法（对齐是刻意做的，改之前先看那个文件）：
     *   * 文字盒：x = borderW + padH、y = borderW + padV、宽 = ceil(advance)+1、高 = 字号；
     *     文字在盒里**垂直居中** —— QML 那边 `verticalAlignment: Text.AlignVCenter`，
     *     而 QML 的 Text 是把整条行盒（高 = fontMetrics.height() ≈ 1.2×字号）在盒里居中，
     *     所以基线 = 盒顶 + (字号 − 行盒高)/2 + ascent。少了这半步，字会整体偏上。
     *   * 阴影/描边：先画黑的（可能多份），再画本体；
     *   * 本人白框：最后画一圈 2px 白色圆角矩形（QML 里那个 Rectangle 是 anchors.fill）。
     */
    void DanmakuAtlas::paintEntry(const QRect &target, const Request &request)
    {
        QPainter painter(&m_image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        /* 只在这块小图里画，越界不会污染邻居 */
        painter.setClipRect(target);
        painter.translate(target.topLeft());
        /*
         * 小图是按设备像素烤的（见 Request::devicePixelRatio），这里把画笔缩到设备像素，
         * 于是下面所有坐标仍按**逻辑像素**写（和引擎给的盒子尺寸同一套单位）。
         */
        painter.scale(request.devicePixelRatio, request.devicePixelRatio);

        QFont font(request.fontFamily);
        font.setPixelSize(qMax(1, request.pixelSize));
        font.setBold(request.bold);

        /*
         * ★★★ 必须把 font 交给画笔，否则 drawText 用的是**画笔默认字体** ★★★
         *
         * 这个坑的表现极具迷惑性：下面的 border/padding 都是按 request.pixelSize 算的
         * （白框尺寸看起来完全正常），但真正画出来的字一直是默认小字（十几像素），
         * 于是"边框正常、文字很小、字号设置怎么调都没反应"。
         * QFontMetricsF(font) 只影响**量出来的宽高和基线**，跟画笔用什么字体毫无关系。
         */
        painter.setFont(font);

        const QFontMetricsF metrics(font);

        const qreal borderW = request.borderWidth;
        const qreal padH = request.padH;
        const qreal padV = request.padV;

        const qreal labelX = borderW + padH;
        const qreal labelY = borderW + padV;
        const qreal labelH = request.pixelSize;                 /* 文字盒高 = 字号 */
        const qreal baseline = labelY + (labelH - metrics.height()) / 2.0 + metrics.ascent();

        const QString text = request.text;

        /* ---- 阴影 / 描边（先画，压在本体下面）---- */
        const qreal shadowOffset = (request.outlineType == 1)
                                       ? 0.0
                                       : (request.fixedStyle ? 2.0 : 1.0);
        const qreal shadowAlpha = (request.fixedStyle || request.outlineType == 0) ? 0.9 : 0.8;

        painter.setPen(QColor(0, 0, 0, static_cast<int>(shadowAlpha * 255)));
        painter.drawText(QPointF(labelX + shadowOffset, baseline + shadowOffset), text);

        if (request.outlineType == 1) {
            /* 四向各一份，凑出"包边"的观感（QML 里那个 Repeater model = 4） */
            painter.setPen(QColor(0, 0, 0, static_cast<int>(0.9 * 255)));
            painter.drawText(QPointF(labelX - 1, baseline), text);
            painter.drawText(QPointF(labelX + 1, baseline), text);
            painter.drawText(QPointF(labelX, baseline - 1), text);
            painter.drawText(QPointF(labelX, baseline + 1), text);
        }

        /* ---- 文字本体 ---- */
        painter.setPen(request.color);
        painter.drawText(QPointF(labelX, baseline), text);

        /* ---- 本人弹幕的白框（Z 序最后，压在整盒边缘）---- */
        if (request.selfBorder) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(255, 255, 255), qMax(1.0, borderW)));
            /* QML: Rectangle 带 2px 边框时描边中心在边界上，所以往里收 borderW/2 */
            const qreal inset = borderW / 2.0;
            painter.drawRoundedRect(QRectF(inset, inset,
                                           std::lround(request.boxWidth) - borderW,
                                           std::lround(request.boxHeight) - borderW),
                                    4.0, 4.0);
        }

        painter.end();
    }

    DanmakuAtlasEntry DanmakuAtlas::entry(const Request &request)
    {
        const QString key = makeKey(request);
        const auto cached = m_cache.constFind(key);

        if (cached != m_cache.constEnd())
            return cached.value();

        DanmakuAtlasEntry result;

        const qreal dpr = request.devicePixelRatio > 0.0 ? request.devicePixelRatio : 1.0;
        /* 小图按**设备像素**烤：四边形按逻辑像素画、由 qt_Matrix 放大，两边 1:1 → 字清晰 */
        const int w = static_cast<int>(std::lround(request.boxWidth * dpr));
        const int h = static_cast<int>(std::lround(request.boxHeight * dpr));

        QRect target;

        if (!allocate(w, h, &target)) {
            /* 放不下（太宽/图集满）——记一条空条目，别再每帧重试 */
            m_cache.insert(key, result);
            return result;
        }

        paintEntry(target, request);

        result.pixelWidth = w;
        result.pixelHeight = h;
        result.uv = QRectF(static_cast<qreal>(target.x()) / m_image.width(),
                           static_cast<qreal>(target.y()) / m_image.height(),
                           static_cast<qreal>(w) / m_image.width(),
                           static_cast<qreal>(h) / m_image.height());
        result.valid = true;
        /* 流水号：渲染侧用它判断"这条进 GPU 纹理了没有"（上传是合并做的） */
        result.serial = ++m_bakeSerial;

        m_cache.insert(key, result);
        m_dirty = true;

        return result;
    }

    QSGTexture *DanmakuAtlas::texture(QQuickWindow *window)
    {
        if (m_image.isNull() || window == nullptr)
            return nullptr;

        if (m_texture != nullptr && !m_dirty)
            return m_texture;

        /*
         * 【合并上传】新烤的文案先攒着，最多每 kUploadIntervalMs 重建一次纹理。
         *
         * 之前是"每烤出一条就重建整张 1024×1024 纹理"：弹幕密集时每秒十几次
         * 新建 GPU 纹理 + 全量重传 4 MB，每次 1 ms 量级 —— 表现就是滚动时偶发卡顿。
         * 现在攒够一个间隔再传：新弹幕最多晚一个间隔（100 ms）出现，肉眼基本无感，
         * 上传次数从"每秒十几次"降到"每秒最多 10 次"。
         *
         * 没上传的条目这一帧先不画（渲染侧查 uploadedSerial()），
         * 否则会去采样一块还没传上去的图集区域。
         */
        constexpr qint64 kUploadIntervalMs = 100;

        /*
         * 【整批换字号那种切换要立刻传】见 requestImmediateUpload() 的说明：
         * 合并上传会让"刚烤好还没上传"的条目这一帧先不画，平时无感，
         * 但字号整批变（切尺寸/全屏）时那 100ms 就是用户看到的"闪一下"。
         * 纹理本身没了（forgetTexture / 第一次）也必须马上重建。
         */
        const qint64 sinceLastUpload = m_uploadClock.isValid() ? m_uploadClock.elapsed() : -1;
        const bool uploadNow = (m_texture == nullptr) || m_forceUpload
                               || sinceLastUpload < 0 || sinceLastUpload >= kUploadIntervalMs;

        if (!uploadNow)
            return m_texture;

        m_forceUpload = false;

        /*
         * 重建整张纹理。QQuickWindow::createTextureFromImage 只能在渲染线程调用
         * （我们就在 updatePaintNode 里），它会新建一个 QSGTexture。
         *
         * 【为什么整张重传而不是局部更新】局部更新要走 QSGTexture 的子类 +
         * 后端的纹理子资源上传，跨 D3D11/Metal/GL 三套写法都不一样。
         * 靠上面的合并上传把频率压下来之后，代价已经可以接受。
         */
        delete m_texture;
        m_texture = window->createTextureFromImage(m_image);

        /*
         * 【不要再写 setFlag(QSGTexture::TextureHasAlphaChannel) —— 那是 Qt 5 的 API】
         * Qt 6 的 QSGTexture 没有 setFlag/Flag，也不是"由调用方声明有没有 alpha"：
         * hasAlphaChannel() 是**虚函数**，由纹理自己的来源决定 —— 我们用
         * QImage::Format_ARGB32_Premultiplied 建纹理，它本来就报 true。
         * （编译报 "setFlag 不是 QSGTexture 的成员" 就是这个原因。）
         */
        if (m_texture != nullptr) {
            m_dirty = false;
            /* 这一批（含之前攒着的）都已经进纹理了 */
            m_uploadedSerial = m_bakeSerial;
            m_uploadClock.restart();
        }

        return m_texture;
    }

}// namespace cicadaqt
