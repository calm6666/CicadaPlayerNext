// ===========================================================================
// DanmakuAtlas —— 弹幕文字图集（GPU 批量渲染的第一块地基）
//
// 【为什么需要它】GPU 只会画纹理，不会画字符串。所以每条弹幕的"最终长相"
// （文字 + 阴影/描边 + 本人白框 + 内边距）必须**先烤成位图**，运行时只画一个
// 带 UV 的四边形。
//
// 【烤成多大】按**引擎给的整盒尺寸**（RenderItem::width × height）1:1 烤。
// 这样四边形就是引擎那个盒子本身，位置/碰撞（引擎算的）和画面（GPU 画的）天然一致，
// 渲染侧不需要再做任何"文字宽 + 内边距 + 边框"的加法。
//
// 【和 QML 版对齐】这里复刻的是 DanmakuItem.qml 那 186 行的画法：
//   * 文字盒高 = 字号（line-height:1），文字在盒里垂直居中 —— 见 paintEntry 里的基线算法；
//   * 描边三档（0 重墨 / 1 四向描边 / 2 45°投影）+ 固定弹幕那套更重的阴影；
//   * 本人弹幕（uid 1）的 2px 白框 + padding；
//   * 固定弹幕（顶/底）的 2px 8px padding。
//
// 【缓存键】文字 + 颜色 + 字号 + 粗体 + 字体族 + 描边档 + 白框 + 固定样式。
// 同一条弹幕只烤一次；颜色种类很少，所以键的数量级就是"视频里出现过的不同弹幕文案数"。
//
// 【生命周期】只在渲染线程用（QPainter 画 QImage、再用 QQuickWindow 转成纹理）。
// 图集满了就整体清空重烤（正常播放到不了那一步，日志里会留一行）。
// ===========================================================================
#ifndef CICADA_QT_DANMAKUATLAS_H
#define CICADA_QT_DANMAKUATLAS_H

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QRectF>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QImage>

QT_BEGIN_NAMESPACE
class QQuickWindow;
class QSGTexture;
QT_END_NAMESPACE

namespace cicadaqt {

    /* 一条弹幕在图集里占的位置 */
    struct DanmakuAtlasEntry {
        /* 归一化 UV 矩形（左上角 + 宽高，都是 0~1） */
        QRectF uv;
        /* 这张小图的像素尺寸 = 引擎给的整盒尺寸（向上取整） */
        int pixelWidth = 0;
        int pixelHeight = 0;
        /* 烤成功了没有。false 时渲染侧应当跳过这条（不画总比画错位强） */
        bool valid = false;
        /*
         * 这条是什么时候烤的（图集的烤制流水号）。
         *
         * 纹理上传是**合并**的（见 texture()）：刚烤进 QImage 的条目可能要等下一批
         * 上传才真的进了 GPU 纹理。渲染侧用
         *     entry.serial <= atlas.uploadedSerial()
         * 判断"这条已经在纹理里了"，没上传的先不画 —— 否则会去采样一块还没传上去的
         * 图集区域（画面上一块空白或者上一轮的残留）。
         */
        quint64 serial = 0;
    };

    class DanmakuAtlas {
    public:
        /* 一次烤字需要的全部输入（就是 DanmakuItem 那几行属性的来源） */
        struct Request {
            QString text;
            QColor color = QColor(255, 255, 255);
            int pixelSize = 18;
            bool bold = true;
            QString fontFamily = QStringLiteral("Microsoft YaHei");
            int outlineType = 0;
            bool selfBorder = false;
            bool fixedStyle = false;
            /*
             * 内边距 / 边框宽 / 整盒尺寸：**全部用引擎给的值**（RenderItem::padH/padV/
             * borderWidth/width/height）。不要在渲染侧自己按 QML 那套三元表达式再算一遍 ——
             * 引擎是"盒子尺寸"的权威，碰撞和摆位都按它算；渲染侧只要照着画，
             * 两边就不可能对不上。
             */
            float padH = 0;
            float padV = 0;
            float borderWidth = 0;
            float boxWidth = 0;
            float boxHeight = 0;
            /*
             * 设备像素比。小图按 `boxWidth × dpr` 的**设备像素**烤，四边形按逻辑像素画、
             * 由 qt_Matrix 放大到设备像素 —— 两边正好 1:1，所以 HiDPI 屏上文字是清晰的
             * （不乘 dpr 的话会被放大 2 倍糊掉）。
             */
            qreal devicePixelRatio = 1.0;
        };

        ~DanmakuAtlas();

        /* 取（没有就现烤）一条弹幕的图集条目 */
        DanmakuAtlasEntry entry(const Request &request);

        /*
         * 这一帧往图集里加过东西吗？加过就要重新上传纹理。
         * 取走之后标志清零（每帧问一次）。
         */
        bool takeDirty()
        {
            const bool dirty = m_dirty;
            m_dirty = false;
            return dirty;
        }

        /*
         * 图集纹理。只在**渲染线程**调用（QSGTexture 是场景图对象）。
         * 有新增内容时会重建纹理；返回 nullptr 表示这块内容这一帧画不了。
         *
         * 【上传是合并的 —— 这是"滚动卡顿"的第二个大头】
         * 每烤一句新文案都重建整张 1024×1024 纹理的话，弹幕密集时每秒要新建 + 全量重传
         * 十几次（每次 4 MB），每次都是 1 ms 量级的 GPU 分配 + 上传 → 偶发掉帧。
         * 现在最多每 kUploadIntervalMs 上传一次，攒着的条目下一批一起传。
         * 没上传的条目渲染侧会先跳过（见 DanmakuAtlasEntry::serial）。
         */
        QSGTexture *texture(QQuickWindow *window);

        /* 已经上传进 GPU 纹理的最大流水号（渲染侧据此判断哪些条目可以先画） */
        quint64 uploadedSerial() const { return m_uploadedSerial; }

        /*
         * 让**下一帧**的 texture() 无视合并上传的间隔、立刻重建纹理。
         *
         * 【为什么要这个】合并上传的代价是"刚烤好但还没上传的条目这一帧先不画"。
         * 平时新弹幕一条条进来时无感；但**容器尺寸/全屏一变**，自动缩放比例变了 →
         * 在屏弹幕的字号全变 → 整批重新烤 → 这批条目会有最多 100ms 画不出来，
         * 用户看到的就是切尺寸/全屏（或暂停后点播放）时"弹幕闪一下"。
         * 这种"整批换字号"是一次性状态切换，不是每帧新增，直接同步传上去。
         */
        void requestImmediateUpload() { m_forceUpload = true; }

        /* 场景图销毁/上下文丢失时把纹理放掉 */
        void releaseTexture();

        /*
         * 场景图失效时用：**只把指针放掉，不 delete**。
         *
         * QSGTexture 属于场景图，Qt 在场景图失效时会自己销毁它（包括我们用
         * createTextureFromImage 建的那些），这时候再 delete 就是双重释放。
         * 放掉之后 m_dirty 置真：下一帧 texture() 会用新上下文重建一张。
         */
        void forgetTexture()
        {
            m_texture = nullptr;
            m_dirty = true;
        }

        /* 诊断用：当前缓存了多少条、图集用了多大 */
        int cachedCount() const { return m_cache.size(); }
        int atlasWidth() const { return m_image.width(); }
        int atlasHeight() const { return m_image.height(); }

        /*
         * 图集世代：**每次整体重烤（缓存清空）就 +1**。
         *
         * 渲染器把它算进"几何签名"里 —— 图集一重烤，之前发出去的 UV 全部失效，
         * 顶点缓冲必须重建，否则会去采样图集里的错误位置（画面上一片乱码）。
         * 少了这一个计数器就是"跑久了偶尔花屏"这种极难查的 bug。
         */
        quint32 generation() const { return m_generation; }

    private:
        /* 在整盒 (w,h) 里把这条弹幕画出来 */
        void paintEntry(const QRect &target, const Request &request);

        /* 找一块 w×h 的空位（货架式装箱）。放不下返回 false。 */
        bool allocate(int w, int h, QRect *out);

        /* 图集满了：清空缓存、从头开始 */
        void resetPacking();

        QImage m_image;
        QHash<QString, DanmakuAtlasEntry> m_cache;

        QSGTexture *m_texture = nullptr;
        bool m_dirty = false;
        /* requestImmediateUpload() 置真：下一帧跳过合并上传的间隔（见那个函数的说明） */
        bool m_forceUpload = false;

        /*
         * 烤制/上传流水号：
         *   m_bakeSerial     —— 每烤出一条新文案 +1，条目的 serial 就是它；
         *   m_uploadedSerial —— 最近一次把纹理传上去时，已经包含到哪个流水号。
         * 上传是合并做的，所以两者之间会有差；渲染侧只画 serial <= uploadedSerial 的条目。
         */
        quint64 m_bakeSerial = 0;
        quint64 m_uploadedSerial = 0;
        /* 距上次上传过了多久（用来合并上传） */
        QElapsedTimer m_uploadClock;

        /* 货架式装箱的游标 */
        int m_cursorX = 0;
        int m_cursorY = 0;
        int m_rowHeight = 0;

        /* 整体重烤的次数（见 generation()） */
        quint32 m_generation = 0;
    };

}// namespace cicadaqt

#endif// CICADA_QT_DANMAKUATLAS_H
