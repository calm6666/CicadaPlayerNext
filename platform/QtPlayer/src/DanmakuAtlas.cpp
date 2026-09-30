#include "DanmakuAtlas.h"

#include <QtGui/QFont>
#include <QtGui/QFontMetricsF>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGTexture>

#include <utils/frame_work_log.h>

#include <atomic>
#include <cmath>

#if defined(Q_OS_WIN)
/*
 * "一张纹理原地更新"那条路要直接碰 D3D11 设备/上下文，并把原生纹理包成 QSGTexture。
 * d3d11_1.h 必须先以 **C++** 链接进来：否则 libavutil/hwcontext_d3d11va.h 之类的
 * extern "C" 包含会让 D3D11_VIEWPORT / D3D11_RECT / D3D11_BOX 的 C++ 比较运算符
 * 拿到 C 链接，MSVC 报一片 C2733（理由与 CicadaTextureD3D11.cpp 开头那段一模一样）。
 */
#include "CicadaQtTextureWrap.h"
#include <QtQuick/QSGRendererInterface>
#include <d3d11_1.h>
#endif

namespace cicadaqt {

    /* 见 DanmakuAtlas.h 里那两个取值的说明：只做计数，不参与任何逻辑。 */
    static std::atomic<long long> g_atlasTexturesCreated{0};
    static std::atomic<long long> g_atlasTexturesDeleted{0};

    long long danmakuAtlasTexturesCreated()
    {
        return g_atlasTexturesCreated.load();
    }

    long long danmakuAtlasTexturesDeleted()
    {
        return g_atlasTexturesDeleted.load();
    }

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
#if defined(Q_OS_WIN)
        /* 正常路径上 forgetTexture()（场景图失效）已经先把指针清掉了。 */
        releaseD3D11Atlas();
#endif
    }

#if defined(Q_OS_WIN)

    void DanmakuAtlas::releaseD3D11Atlas()
    {
        if (m_d3dTexture != nullptr) {
            static_cast<ID3D11Texture2D *>(m_d3dTexture)->Release();
            m_d3dTexture = nullptr;
        }

        /* m_d3dDevice / m_d3dContext 是 Qt 场景图的，只丢指针，不做 Release。 */
        m_d3dDevice = nullptr;
        m_d3dContext = nullptr;
        m_d3dWidth = 0;
        m_d3dHeight = 0;
        m_d3dDirty = false;
    }

    QSGTexture *DanmakuAtlas::textureViaD3D11(QQuickWindow *window)
    {
        auto *rif = window->rendererInterface();

        if (rif == nullptr || rif->graphicsApi() != QSGRendererInterface::Direct3D11) {
            return nullptr;   /* 不是 D3D11 后端：交给调用方的通用路径 */
        }

        auto *device = static_cast<ID3D11Device *>(
                           rif->getResource(window, QSGRendererInterface::DeviceResource));
        auto *context = static_cast<ID3D11DeviceContext *>(
                            rif->getResource(window, QSGRendererInterface::DeviceContextResource));

        if (device == nullptr || context == nullptr) {
            return nullptr;
        }

        /*
         * 第一次（或尺寸变了 / 场景图重建过）：建一张 D3D11 纹理 + **只包一次** QSGTexture。
         * 这是整条路上唯一一次新建 D3D11 纹理 —— 之后每次内容变化都只是
         * UpdateSubresource 往同一张纹理里写（不产生任何新的 D3D11 分配）。
         */
        if (m_d3dTexture == nullptr || m_texture == nullptr ||
            m_d3dWidth != m_image.width() || m_d3dHeight != m_image.height()) {
            releaseTexture();       /* 旧的 QSGTexture 包装（可能是旧尺寸的）先放掉 */
            releaseD3D11Atlas();

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = static_cast<UINT>(m_image.width());
            desc.Height = static_cast<UINT>(m_image.height());
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            /*
             * **必须是 R8G8B8A8_UNORM**：包给 Qt 之后这张纹理是按场景图的约定（RGBA8）
             * 采样的（和 CicadaTextureD3D11 那条视频输出纹理同一个理由）。所以下面
             * UpdateSubresource 之前要把 QImage 转成 RGBA 字节序 —— QImage 的 ARGB32
             * 在内存里是 BGRA，直接塞进去会红蓝互换。
             */
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            ID3D11Texture2D *d3dTexture = nullptr;

            if (FAILED(device->CreateTexture2D(&desc, nullptr, &d3dTexture)) || d3dTexture == nullptr) {
                AF_LOGW("danmaku atlas: CreateTexture2D(%ux%u) failed; falling back to one "
                        "createTextureFromImage per upload\n", desc.Width, desc.Height);
                return nullptr;
            }

            m_d3dTexture = d3dTexture;
            m_d3dDevice = device;
            m_d3dContext = context;
            m_d3dWidth = m_image.width();
            m_d3dHeight = m_image.height();
            m_texture = wrapD3D11Texture(window, d3dTexture, m_image.size(), /* hasAlpha = */ true);

            if (m_texture == nullptr) {
                releaseD3D11Atlas();
                return nullptr;
            }

            m_d3dDirty = true;

            AF_LOGI("danmaku atlas: created ONE D3D11 texture (%dx%d) and wrapped it once; "
                    "all further updates go through UpdateSubresource (no new allocations)\n",
                    m_d3dWidth, m_d3dHeight);
        }

        if (m_d3dDirty) {
            const QImage rgba =
                (m_image.format() == QImage::Format_RGBA8888_Premultiplied)
                    ? m_image
                    : m_image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);

            context->UpdateSubresource(static_cast<ID3D11Texture2D *>(m_d3dTexture), 0, nullptr,
                                       rgba.constBits(), static_cast<UINT>(rgba.bytesPerLine()), 0);
            m_d3dDirty = false;
        }

        return m_texture;
    }

#endif// Q_OS_WIN

    void DanmakuAtlas::releaseTexture()
    {
        if (m_texture != nullptr) {
            delete m_texture;
            m_texture = nullptr;
            g_atlasTexturesDeleted++;
        }
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
         * ============ 【2026-09-30 回退：图集走回 createTextureFromImage】============
         *
         * 中间试过"一张 D3D11 纹理 + UpdateSubresource 原地更新"（见 textureViaD3D11()），
         * 目的是省掉"每次上传新建 4MB 纹理"。两条实测结论让它**不值得保留**：
         *
         *   1. **它没有解决泄漏**：改成原地更新之后，日志里 `danmaku atlas textures:
         *      created=0 deleted=0`（旧的建/删完全消失）、驱动线程与私有提交的**每轮增量
         *      一模一样**（nvwgf2umx.dll 仍是 +18 条/轮、commit 仍是 +46MB/轮）——
         *      也就是说那 176 张/轮的纹理本来就被 Qt/D3D 完整回收了，它不是漏的那一项；
         *   2. **它引入了两个新问题**：包装原生纹理时必须声明
         *      `QQuickWindow::TextureHasAlphaChannel`（否则透明区被当不透明画，整条弹幕糊上
         *      黑色背景），而加了那个声明之后，紧接着的构建**开窗即闪退**（日志停在视频解码器
         *      刚打开那一行）。
         *
         * 所以回到 Qt 自己的 `createTextureFromImage()`：它建出来的 QSGTexture 由 Qt 决定
         * alpha 语义（我们不用声明、也就不会声明错），并且已被证明不会泄漏。
         * 那 176 张/轮的分配是**可回收的 churn**，不是泄漏 —— 不该为它冒险。
         */
        /* 旧的先还掉（走 releaseTexture()：删掉并计数），再建新的。 */
        releaseTexture();
        m_texture = window->createTextureFromImage(m_image);

        if (m_texture != nullptr) {
            g_atlasTexturesCreated++;
        }

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
