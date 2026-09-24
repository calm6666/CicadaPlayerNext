#include "DanmakuRendererItem.h"

/* updatePaintNode 里那次 requestUpdate 要排到 GUI 线程执行，用到 QMetaObject::invokeMethod */
#include <QtCore/QMetaObject>
/* 读引擎向量之前要先上锁拷一份快照（见 updatePaintNode 开头那段），用到 QMutexLocker */
#include <QtCore/QMutexLocker>

#include "DanmakuController.h"

#include <QtGui/QColor>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGGeometry>
#include <QtQuick/QSGGeometryNode>
#include <QtQuick/QSGNode>
#include <QtQuick/QSGTextureMaterial>

#include <Danmaku/DanmakuTypes.h>

#include <utils/frame_work_log.h>

#include <cmath>
/* 快照里把文字字节抄进 m_textArena，用到 memcpy */
#include <cstring>

namespace cicadaqt {

    namespace {

        /*
         * 0xRRGGBBAA → QColor，**只取 RGB**。
         * 颜色自带的 alpha 不参与：不透明度统一由引擎的 RenderItem::alpha 管，
         * 两边都乘一次的话半透明弹幕会比 QML 版暗（QML 那边读的也是不含 alpha 的 hex）。
         */
        QColor colorOf(quint32 rgba)
        {
            return QColor(int((rgba >> 24) & 0xFFu),
                          int((rgba >> 16) & 0xFFu),
                          int((rgba >> 8) & 0xFFu));
        }

        void mixIn(quint64 *hash, quint64 value)
        {
            /* FNV-1a：够快够散，用来判断"这一帧要不要重新问图集" */
            *hash = (*hash ^ value) * 1099511628211ull;
        }

        /*
         * 每条弹幕 4 个顶点、6 个索引。
         * 顶点布局用 Qt 自带的 `QSGGeometry::defaultAttributes_TexturedPoint2D()`
         * （位置 2 + 纹理坐标 2）—— 因为材质用的是 Qt 自带的 QSGTextureMaterial，
         * 它的着色器认的就是这套属性，**不能自己另写一套**。
         */
        constexpr int kVertexPerQuad = 4;
        constexpr int kIndexPerQuad = 6;

        /*
         * 防脏数据的上限（见 rebuildGeometry 里的说明）：盒子尺寸和文字长度不合理时
         * 直接跳过这条弹幕，绝不拿着它去建 QImage / QString。
         */
        constexpr float kMaxBoxWidth = 4096.0f;
        constexpr float kMaxBoxHeight = 1024.0f;
        constexpr int kMaxTextLength = 4096;

        /*
         * 一帧里"刚烤好、还没进 GPU 纹理"的条数达到多少，就按**整批重烤**处理：
         * 这一帧立刻把图集纹理传上去（见 updatePaintNode 里 bulkRebake 那段）。
         *
         * 8 的依据：正常播放时每帧新进屏的弹幕是一两条（弹幕时间点是稀疏的），
         * 而"图集满 → 整体重烤 / seek 后整批补上屏 / 改字号换成整批新条目"都是一次几十条，
         * 两者之间留一个数量级的余量。取小了会把"每帧都重传 4MB"放回来（当初压掉的卡顿），
         * 取大了整屏闪一下那类批量切换就漏过去了。
         */
        constexpr int kFreshBulkCount = 8;

        /*
         * 进程内共用的一条**单调**时钟（毫秒）。
         *
         * 【为什么要共用一条】弹幕位置是每帧现算的：now = 锚点 + (现在 − 锚点时间戳) × 倍速。
         * 锚点是 QML 在 GUI 线程推过来的，而"现在"是渲染线程算的 —— 如果两边各自取时间
         * （比如一边 Date.now()、一边自己 restart 一个秒表），得到的时间差里就混进了
         * "渲染线程什么时候才看到这个属性"的调度误差（最多一帧），屏幕上就是滚动时
         * 周期性的微小抖动。用同一条时钟 + 把时间戳一起推过来，这个误差恒等于 0。
         *
         * QElapsedTimer 用的是高精度单调计时器（Windows 上是 QPC 一类），不受系统时间
         * 调整影响；elapsed() 是 const 的纯计算，GUI/渲染两个线程同时读是安全的
         * （这个静态局部量的初始化本身也是线程安全的）。
         */
        qint64 monotonicMs()
        {
            /*
             * 直接复用控制器那条进程内单调时钟（DanmakuController.h 的 danmakuMonoMs()）：
             * 引擎时钟值的时间戳就是用它打的，这里减出来的才是纯时间差 ——
             * 两边各建一个 QElapsedTimer 的话基准不同，减出来毫无意义（见时钟那段说明）。
             */
            return danmakuMonoMs();
        }


    }// namespace

    DanmakuRendererItem::DanmakuRendererItem(QQuickItem *parent)
        : QQuickItem(parent)
    {
        /* 没有这一句 QQuickItem 不会调 updatePaintNode —— 这是自定义节点最经典的坑 */
        setFlag(QQuickItem::ItemHasContents, true);

        m_timer.setTimerType(Qt::PreciseTimer);
        m_timer.setInterval(qMax(8, 1000 / qMax(1, m_updateFps)));
        connect(&m_timer, &QTimer::timeout, this, &DanmakuRendererItem::onTick);
        /*
         * 定时器一直开着，但间隔是自适应的（见 onTick）：屏上有弹幕时按 updateFps
         * （默认 60Hz），一条都没有时降到 100ms 慢查。
         * 这一次 tick 做的事情只有一件 —— `update()`，把 item 标记为脏。
         */
        m_timer.start();

        /* 窗口（场景图）一挂上就接"每帧标脏"，见 onWindowChanged */
        connect(this, &QQuickItem::windowChanged, this, &DanmakuRendererItem::onWindowChanged);
        onWindowChanged(window());
    }

    void DanmakuRendererItem::onWindowChanged(QQuickWindow *window)
    {
        if (m_frameSwapped)
            disconnect(m_frameSwapped);

        if (m_sceneGraphInvalidated)
            disconnect(m_sceneGraphInvalidated);

        if (m_frameSwappedStamp)
            disconnect(m_frameSwappedStamp);

        m_frameSwapped = QMetaObject::Connection();
        m_sceneGraphInvalidated = QMetaObject::Connection();
        m_frameSwappedStamp = QMetaObject::Connection();
        m_lastSwapMonoMs = -1;

        if (window == nullptr)
            return;

        /*
         * 【为什么除了定时器还要按帧标脏】
         *
         * 位置是**每一帧**现算的（见 updatePaintNode 的时钟那段），所以每一帧都得重跑一次
         * updatePaintNode。原来只靠上面那个 60Hz 定时器标脏：它的名义间隔是 16ms，
         * 而屏幕是 16.67ms 一帧 —— 余量只有 0.67ms。GUI 线程稍微忙一点（视频那一路、
         * QML、弹幕引擎都在这一个线程上），某个定时器事件就会被挤到两帧之间丢掉，
         * 那一帧的场景图同步拿到的还是**上一次的顶点**（弹幕原地不动），下一帧才补上 ——
         * 屏幕上就是滚动时"有点抖"（位置的时间是连续的，但显示的节拍不齐）。
         *
         * frameSwapped 是"这一帧真的显示出来了"的信号，接上它标脏就变成**帧驱动**：
         * 每一帧都重算位置，不会有陈旧帧。用 QueuedConnection 是因为这个信号在渲染线程
         * 发出，而 QQuickItem::update() 只能在 GUI 线程调。
         * 定时器保留着当兜底（窗口被遮挡/最小化时不一定有帧）。
         */
        m_frameSwapped = connect(window, &QQuickWindow::frameSwapped, this,
                                 [this]() { update(); }, Qt::QueuedConnection);

        /*
         * 【抖动根治：在**换帧时刻**采一次时间戳，供 updatePaintNode 当采样点】
         *
         * 浏览器里 CSS transform 动画之所以丝滑：合成器在**每一帧的呈现时刻**按帧时间算
         * 矩阵（动画是 t 的连续函数，t 取的就是 vsync 那一刻）。我们以前是在
         * updatePaintNode（sync 阶段）读墙上时钟 —— 而 sync 什么时候发生取决于 GUI 线程
         * 忙不忙（视频、QML、弹幕引擎都在这一个线程上），采样点忽早忽晚（±几毫秒）。
         * 速度 400px/s 时 5ms 就是 2px，于是每帧步长忽大忽小，屏幕上就是"一卡一卡/轻微抖动"。
         *
         * frameSwapped 由渲染线程在**换帧之后**立刻发出，渲染线程是紧循环等 vsync 的，
         * 它的时间戳对"呈现时刻"的抖动远小于 GUI 线程的调度抖动。用它当采样点，位置的
         * 步进就变成"每个呈现帧一步"——和浏览器同一种做法。代价只是整体晚一帧，那是
         * **常量偏移**（不产生抖动）。
         *
         * 这里必须用 DirectConnection：信号在渲染线程发出，我们要在**渲染线程**里读时钟；
         * 写下的 m_lastSwapMonoMs 也只被渲染线程（updatePaintNode）读，不存在竞态。
         */
        m_frameSwappedStamp = connect(window, &QQuickWindow::frameSwapped, this,
                                      [this]() { m_lastSwapMonoMs = danmakuMonoMs(); },
                                      Qt::DirectConnection);

        /*
         * 【场景图要没了 → 立刻松开图集纹理的指针（只放指针，**不要 delete**）】
         *
         * 关窗的顺序实测是：**渲染线程先收 QRhi / 场景图**（日志里
         * "released the captured D3D11 device (its scene graph went away)"、
         * "QRhi … going down with 1 unreleased resources … Texture"），之后 GUI 线程
         * 才 deleteLater 掉整棵 QML 树、析构我们。少了这一步，~DanmakuAtlas 就会在
         * **上下文已经没了之后**去 delete 那张 QSGTexture（QSGRhiTexture 的析构要碰 QRhi）
         * —— 那是关窗路径上的下一个崩点。
         *
         * sceneGraphInvalidated 是**渲染线程**发出的（此刻 GUI 线程被阻塞），所以这里
         * 只做"放指针"这一件事（Qt 对这类处理的唯一许可动作，别的都不要碰）。
         */
        m_sceneGraphInvalidated = connect(window, &QQuickWindow::sceneGraphInvalidated, this,
                                          [this]() { m_atlas.forgetTexture(); },
                                          Qt::DirectConnection);
    }

    void DanmakuRendererItem::setController(QObject *controller)
    {
        if (m_controllerObject == controller)
            return;

        /* 换数据源：先断开对旧对象的析构监听 */
        if (m_controllerDestroyed)
            disconnect(m_controllerDestroyed);

        m_controllerObject = controller;
        m_controller = qobject_cast<DanmakuController *>(controller);

        if (m_controller != nullptr) {
            /*
             * 控制器被销毁时把自己的指针清掉。少了这一句，定时器还会在
             * updatePaintNode 里解引用一个已经没了的对象（关窗/清片源时就会发生）。
             */
            m_controllerDestroyed = connect(m_controller, &QObject::destroyed, this, [this]() {
                m_controller = nullptr;
                m_controllerObject = nullptr;
                m_controllerDestroyed = QMetaObject::Connection();
                /*
                 * 【这里不要再 m_uvs.clear()】
                 * m_uvs 是**渲染线程**的容器（updatePaintNode 里 assign / 按下标写）。
                 * 本 lambda 跑在 GUI 线程（控制器析构那一下，关窗/清片源时就会发生），
                 * 在渲染线程可能正读它的时候去 clear()，两个线程同时改一个 std::vector
                 * 的内部指针，正是"把 Debug STL 的内部状态写坏"的经典做法。
                 * 只把签名置 0 就够了：下一帧渲染线程自己会 assign(itemCount) 重建。
                 */
                m_builtSignature = 0;
                emit controllerChanged();
                update();
            });
        } else {
            m_controllerDestroyed = QMetaObject::Connection();
        }

        /* 数据源换了就当图集键全变了。
           【不要在 GUI 线程 m_uvs.clear()】理由同上：m_uvs 归渲染线程用，
           签名置 0 之后渲染线程下一帧会自己 assign(itemCount) 重建。 */
        m_builtSignature = 0;
        m_anchorMonoMs = -1.0;
        emit controllerChanged();
        update();
    }

    void DanmakuRendererItem::setAnchorMonoMs(qreal value)
    {
        if (qFuzzyCompare(m_anchorMonoMs, value))
            return;

        m_anchorMonoMs = value;
        /* 只写不读 → 不发信号 */
    }

    qint64 DanmakuRendererItem::monoMs() const
    {
        return monotonicMs();
    }

    void DanmakuRendererItem::setTimeMs(qreal value)
    {
        if (qFuzzyCompare(m_timeMs, value))
            return;

        m_timeMs = value;

        /*
         * 锚点变了 → 也要重画一帧。定时器没在跑的时候（暂停中拖动进度条）
         * 这一步是必须的，否则画面会停在旧位置。
         */
        update();

        /*
         * 【注意】这个属性现在只是"QML 推来的播放位置"，**不再是渲染的时间基准** ——
         * refSec/nowSec 走的是引擎时钟（见 updatePaintNode 里"时钟"那一段）。
         * 所以这里没有任何锚点微调：以前那两套（小幅回退钳制、±4% 速率收敛）都是在替
         * "渲染器与引擎两条时钟不同源"打补丁，现在补丁连同根因一起删掉了。
         */

        /*
         * 【不再 emit timeMsChanged() —— 这是崩溃链上的那一环】
         *
         * 用户日志（64 层栈）：
         *     #30 CicadaPlayerItem::positionChanged → QV4（QML 绑定求值）
         *     #18 qt_static_metacall → #17 setTimeMs → #16 timeMsChanged
         *     #08..#15 QV4 → 0xc0000005
         * 即"位置更新 → QML 给本属性赋值 → 我们发 NOTIFY → 信号投递重入 QV4 → 崩"。
         * 而 QML 侧对这些属性**只写不读**（`timeMs: player.position`），
         * 这个信号没有任何消费者 —— 去掉它就切断了这条重入链。
         */

    }

    void DanmakuRendererItem::setRate(qreal value)
    {
        if (qFuzzyCompare(m_rate, value))
            return;

        m_rate = value;
        /* 只写不读 → 不发信号（QML 写属性→发信号→QV4 重入是崩溃链，见 setTimeMs 的说明） */
    }

    void DanmakuRendererItem::setClockRunning(bool value)
    {
        if (m_clockRunning == value)
            return;

        m_clockRunning = value;
        /* 停下来时补一帧：把弹幕定在当前帧的位置上 */
        update();
        /*
         * 【不再 emit clockRunningChanged()—— 这是崩溃点】
         *
         * 崩溃栈（用户 13:51 那次，64 层）：
         *     #16 DanmakuRendererItem::clockRunningChanged+0x25   (moc:260)
         *     #17 DanmakuRendererItem::setClockRunning+0x4a       (DanmakuRendererItem.cpp:254)
         *     #18 DanmakuRendererItem::qt_static_metacall+0x655   (moc:192)   ← QML 写属性
         *     #19..#35 QV4（QML 的 JS 引擎），带 _chkstk
         * 即：QML 写 `clockRunning` → setter → **发信号** → 信号投递又重入 QV4 → 在那里炸。
         * 而 QML 侧**没有任何地方读** clockRunning（它只被写：
         * `clockRunning: view._driving`），所以这个信号没有任何消费者，纯属多余。
         * 去掉它，就切断了"属性写入 → 信号投递 → QV4 重入"这条链。
         */
    }

    void DanmakuRendererItem::setContainerWidth(qreal value)
    {
        if (qFuzzyCompare(m_containerWidth, value))
            return;

        m_containerWidth = value;
        /* 速度是按容器宽算的 → 图集键跟着变（尺寸不同烤出来的盒子也不一样）。
           字号也随容器宽走（自动缩放），所以这是"整批换字号"，图集必须**立刻**传上去，
           否则那批条目会有最多 100ms 画不出来（切尺寸/全屏时"闪一下"）。 */
        m_builtSignature = 0;
        m_forceAtlasUpload = true;
        /* 只写不读 → 不发信号 */
        update();
    }

    void DanmakuRendererItem::setContainerHeight(qreal value)
    {
        if (qFuzzyCompare(m_containerHeight, value))
            return;

        m_containerHeight = value;
        /* 只写不读 → 不发信号 */
        update();
    }

    void DanmakuRendererItem::setUpdateFps(int value)
    {
        value = qBound(1, value, 240);

        if (m_updateFps == value)
            return;

        m_updateFps = value;
        /* 只写不读 → 不发信号 */
    }

    void DanmakuRendererItem::releaseResources()
    {
        /*
         * 【这是谁的线程 —— 查过 Qt 6 文档，别再按 Qt 5 的印象写】
         *
         * Qt 6.11 的 QQuickItem 文档 "Graphics Resource Handling" 一节写得很明确：
         *   * 场景图失效（换后端 / 设备丢失 / 窗口隐藏）走的是 **invalidateSceneGraph()
         *     这个 slot**（渲染线程调用，且调用期间 GUI 线程被阻塞）；
         *   * 而 **releaseResources() 是在 item 被移出场景时由 GUI 线程调用的**
         *     （"the QQuickItem::releaseResources() will be called on the GUI thread"）；
         *   * item 析构时"应当自己释放图形资源"（我们没有额外资源要放，见下）。
         * 本类没有实现 invalidateSceneGraph()，所以**不能**假设这里一定是渲染线程：
         * 凡是渲染线程会用的容器（m_uvs / m_textArena / m_itemsSnapshot）都不在这里碰
         * —— 下面把 m_uvs.clear() 去掉就是这个原因，只置签名，让渲染线程自己重建。
         *
         * 那棵树和图集纹理都已经不属于我们了：
         *   * 只把指针放掉，**不要 delete** —— Qt 已经销毁过它们，delete 就是双重释放；
         *   * 下一帧 updatePaintNode(oldNode == nullptr) 会重新建节点。
         */
        /*
         * 【析构路径上必须先把定时器停掉】
         *
         * 崩溃栈是 `~DanmakuRendererItem` → `_Tidy`/`_Orphan_all`（Debug STL 代理清理）→
         * `0xc0000005`：说明**对象正在被销毁时还有 tick 进来写成员**。停掉定时器之后
         * 就不会再有 onTick 闯进析构路径。（注意：这一条只能挡住"定时器"，挡不住
         * "引擎向量被并发改写"—— 那条已经由 updatePaintNode 里的加锁快照解决。）
         */
        m_timer.stop();

        m_atlas.forgetTexture();

        m_geoNode = nullptr;
        m_geometry = nullptr;
        m_material = nullptr;
        m_quadCapacity = 0;
        m_boundTexture = nullptr;
        m_builtSignature = 0;
        m_appliedOpacity = -1.0;
    }

    void DanmakuRendererItem::onTick()
    {
        /*
         * 自适应间隔：屏上有弹幕就按 updateFps 走（默认 60Hz），
         * 一条都没有就降到 100ms 慢查，别空转。
         */
        const int wanted = (m_quadCount > 0) ? qMax(8, 1000 / qMax(1, m_updateFps)) : 100;

        if (m_timer.interval() != wanted)
            m_timer.setInterval(wanted);

        /*
         * 这一个 tick 只做一件事：把 item 标脏，让下一帧重新走 updatePaintNode
         * （位置在渲染线程按"引擎时钟 + 墙钟流逝"现算，见 updatePaintNode 的时钟那段）。
         *
         * 【引擎的推进不在这里】它是 QML 每 tick 调的 engine.update()（DanmakuView.qml:354）。
         * 曾经试过在这个定时器里替 QML 推引擎（"C++ 自走时钟"），那版在"对象析构 / 控制器先死"
         * 的时序下会踩内存（栈：~DanmakuRendererItem → _Tidy/_Orphan_all → 0xc0000005），
         * 已经撤掉，别再放回来。
         */
        update();
    }

    void DanmakuRendererItem::setDrawnCount(int count)
    {
        if (m_quadCount == count)
            return;

        m_quadCount = count;
        /*
         * 【★ 信号必须排队到 GUI 线程再发 ★】
         *
         * 崩溃栈（用户 14:29，用户日志）：
         *     #16 DanmakuRendererItem::quadCountChanged   (moc:284)
         *     #17 DanmakuRendererItem::setDrawnCount      (DanmakuRendererItem.cpp:396)
         *     #18 DanmakuRendererItem::updatePaintNode    (DanmakuRendererItem.cpp:890)
         * 也就是说：**渲染线程**里发了信号，信号的投递会去碰 QML 的 JS 引擎（QV4）——
         * 而 QML/QV4 只允许 GUI 线程访问，跨线程访问就是 `0xc0000005`。
         * 这也是为什么"每次换一个属性名就崩一次"：内存/引擎状态被踩坏之后，
         * 下一个在渲染线程里发信号的属性就是下一个崩点。
         * 现在一律用 QueuedConnection 投到对象所属线程（GUI）再发：
         * 计数照样更新，界面照样能读到新值，只是通知晚一个事件循环节拍。
         */
        QMetaObject::invokeMethod(this, [this]() { emit quadCountChanged(); }, Qt::QueuedConnection);
    }

    int DanmakuRendererItem::rebuildGeometry(QSGGeometry *geometry,
                                             const std::vector<RenderItem> &items,
                                             qreal nowSec, qreal refSec, qreal dpr)
    {
        const int count = static_cast<int>(items.size());

        /*
         * 【★ 顶点/索引缓冲：只在**真的装不下**时才重新分配 ★】
         *
         * 老写法 `if (count > m_quadCapacity) { m_quadCapacity = qMax(count+32, count*2); … }`
         * 的错在于**拿我们自己记的数当权威**：只要 `m_quadCapacity` 与几何的真实容量
         * 不一致（几何被场景图回收重建、releaseResources 之后又画……），判断就会失真，
         * 于是按 count 写顶点成了**越界写堆** —— 用户抓到过
         * `HEAP CORRUPTION DETECTED … wrote to memory after end of heap buffer`。
         *
         * 但"条数一变就精确重分配"（上一版）也有代价：弹幕每条上/下屏都换一次缓冲，
         * 每帧都在 allocate → 顶点缓冲每帧换一块，用户看到的是**抖动/闪烁**。
         *
         * 现在两者兼顾：**权威永远是 `geometry->vertexCount()`**（下面 writableQuads 就是
         * 从它算的），只在真装不下时按"+32 / 翻倍"扩容一次，之后不再缩、也不再每帧换。
         */
        if (geometry->vertexCount() / kVertexPerQuad < count) {
            const int grown = qMax(count + 32, count * 2);

            m_quadCapacity = grown;
            geometry->allocate(grown * kVertexPerQuad, grown * kIndexPerQuad);
        }

        auto *vertices = geometry->vertexDataAsTexturedPoint2D();
        quint16 *indices = geometry->indexDataAsUShort();

        /*
         * 【再兜一道】缓冲是上面按 count 精确分配的；这里再取一次真实容量，
         * 万一将来有人改动分配逻辑、或几何被外部换掉，也不允许写超过它的条数。
         * （多这一道判断几乎不花钱，但把"越界写堆"这条路彻底封死。）
         */
        const int writableQuads = qMin(count, geometry->vertexCount() / kVertexPerQuad);

        int quads = 0;

        /*
         * 【把四边形吸附到设备像素栅格 —— 这条是"滚动发涩/抖"的直接原因】
         *
         * 图集是按设备像素 1:1 烤的，材质也是 Nearest 采样。滚动位置是**小数**（每帧
         * 往前推 speed×Δt），于是采样坐标落在纹理两个像素之间：Nearest 会把它们按
         * floor 归到某一侧，滚动时四边形边缘在纹理边界上蹭来蹭去 —— 肉眼看就是
         * "字在抖、滚动不平滑"。
         *
         * 把左上角对齐到设备像素栅格（逻辑坐标 = round(设备坐标)/dpr）之后，每一帧
         * 采样都正好落在像素中心：字是清晰的，运动是设备像素的整数步进（60Hz 下
         * 每步不到一个逻辑像素，看起来是连续的）。
         *
         * 【x 只对齐到 1/4 设备像素 —— 这是"移动时轻微抖动"的来源】
         * 屏幕缩放 100%（dpr = 1）时，x 被 round 到**整个逻辑像素**：滚动速度 400px/s、
         * 60Hz 下每帧移动 6.7px，取整就成了 7/6/7/7/6… 的不均匀步进，看惯平滑滚动的人
         * 一眼就能看出"轻微抖动"（位置本身是连续的，是显示这一步在抖）。
         * 1/4 像素栅格把误差压到 0.125px（肉眼不可见），而 y 是静止的、照旧对齐整像素
         * （字仍然清晰）。
         */
        const qreal snapScale = (dpr > 0.0) ? dpr : 1.0;
        /* x 用 1/4 栅格：见上面说明；y 保持整像素 */
        const qreal snapScaleX = snapScale * 4.0;

        for (int i = 0; i < count; ++i) {
            if (i >= static_cast<int>(m_uvs.size()))
                break;

            /* 缓冲写不下了就停（正常永远不该发生，见上面 writableQuads 的说明） */
            if (quads >= writableQuads)
                break;

            const QRectF &uv = m_uvs[i];

            if (uv.isEmpty())
                continue;                           /* 没烤进图集（太宽/图集满）：这一条不画 */

            const RenderItem &item = items[i];

            /*
             * 防一手脏数据：盒子尺寸/文字长度不合理（引擎数据被写坏、或者别的调用方
             * 塞了乱七八糟的条目）时直接跳过。不设这道闸的话，一个天文数字会被拿去
             * 建 QImage / QString —— 表现是"卡死然后闪退"，而且栈里看不出是谁干的。
             */
            if (!(item.width > 0.0f && item.height > 0.0f)
                || item.width > kMaxBoxWidth || item.height > kMaxBoxHeight
                || item.textLength > static_cast<size_t>(kMaxTextLength)) {
                continue;
            }

            /* 目标矩形 = 引擎算好的盒子（逻辑像素）；x 要按"这一时刻"往前推 */
            qreal x = item.x;

            if (item.type == DanmakuTypeScroll && !item.hovered && item.durationMs > 0.0f
                && m_containerWidth > 0.0) {
                /*
                 * 滚动速度（像素/秒，媒体时间基准）。引擎的语义是"总距离 = 容器宽 + 盒子宽、
                 * 总时长 = durationMs"，和 QML 版 startMotion(...) 是同一条公式。
                 * 悬停中的那条引擎已经把它冻住了 → 这里不推，位置就停在原地。
                 */
                const qreal speed = (m_containerWidth + item.width) / (item.durationMs / 1000.0);

                x -= speed * (nowSec - refSec);
            }

            const float x0 = static_cast<float>(std::round(x * snapScaleX) / snapScaleX);
            const float y0 = static_cast<float>(std::round(item.y * snapScale) / snapScale);
            const float x1 = x0 + item.width;
            const float y1 = y0 + item.height;

            const float u0 = static_cast<float>(uv.left());
            const float v0 = static_cast<float>(uv.top());
            const float u1 = static_cast<float>(uv.right());
            const float v1 = static_cast<float>(uv.bottom());

            const int base = quads * kVertexPerQuad;
            QSGGeometry::TexturedPoint2D *v = vertices + base;

            /* 顶点顺序：左上 → 右上 → 右下 → 左下（三角形 0-1-2 / 0-2-3） */
            v[0].set(x0, y0, u0, v0);
            v[1].set(x1, y0, u1, v0);
            v[2].set(x1, y1, u1, v1);
            v[3].set(x0, y1, u0, v1);

            quint16 *quadIndices = indices + quads * kIndexPerQuad;
            quadIndices[0] = static_cast<quint16>(base + 0);
            quadIndices[1] = static_cast<quint16>(base + 1);
            quadIndices[2] = static_cast<quint16>(base + 2);
            quadIndices[3] = static_cast<quint16>(base + 0);
            quadIndices[4] = static_cast<quint16>(base + 2);
            quadIndices[5] = static_cast<quint16>(base + 3);

            ++quads;
        }

        geometry->setVertexCount(quads * kVertexPerQuad);
        geometry->setIndexCount(quads * kIndexPerQuad);

        return quads;
    }

    QSGNode *DanmakuRendererItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
    {
        /*
         * 【oldNode == nullptr 说明上一次那棵树已经没了】
         *   * 第一次进来；
         *   * 或者场景图整个失效了（换渲染后端 / 设备丢失 / 窗口重建上下文）——
         *     这时 Qt 会把整棵树丢掉，缓存里的指针全是野指针。
         * 所以这里必须先清缓存再重建。少了这一句，表现就是"关掉播放器窗口、再打开一个
         * 视频"时随机崩溃（父节点已经析构，子节点指针还在用）。
         */
        if (oldNode == nullptr) {
            m_geoNode = nullptr;
            m_geometry = nullptr;
            m_material = nullptr;
            m_quadCapacity = 0;
            m_uvs.clear();
            m_builtSignature = 0;
            m_appliedOpacity = -1.0;
        }

        DanmakuController *controller = m_controller;

        if (controller == nullptr || !isVisible() || width() <= 0.0 || height() <= 0.0) {
            setDrawnCount(0);
            m_geoNode = nullptr;
            m_geometry = nullptr;
            m_material = nullptr;
            m_quadCapacity = 0;
            m_boundTexture = nullptr;
            m_uvs.clear();
            m_builtSignature = 0;
            m_appliedOpacity = -1.0;
            delete oldNode;
            return nullptr;
        }

        /*
         * 根节点 = QSGOpacityNode（整层不透明度），下面挂**唯一**的几何节点。
         * 整屏弹幕一次绘制 —— 这是 CPU 和流畅度的关键（见文件头说明）。
         */
        auto *root = static_cast<QSGOpacityNode *>(oldNode);

        if (root == nullptr) {
            root = new QSGOpacityNode;

            m_geoNode = new QSGGeometryNode;
            m_material = new QSGTextureMaterial;
            m_material->setFiltering(QSGTexture::Nearest);
            m_material->setMipmapFiltering(QSGTexture::None);
            m_geoNode->setMaterial(m_material);
            m_geoNode->setFlag(QSGNode::OwnsMaterial);

            m_geometry = new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 0, 0,
                                         QSGGeometry::UnsignedShortType);
            m_geometry->setDrawingMode(QSGGeometry::DrawTriangles);
            m_geoNode->setGeometry(m_geometry);
            m_geoNode->setFlag(QSGNode::OwnsGeometry);

            root->appendChildNode(m_geoNode);
            m_quadCapacity = 0;
            m_boundTexture = nullptr;
            m_builtSignature = 0;
        }

        /*
         * 【加固】releaseResources() 会把 m_geoNode / m_geometry / m_material 放成
         * nullptr（那棵树已经不属于我们了），但**本函数下一次并不保证拿到
         * oldNode == nullptr** —— 拿到非空 oldNode 时上面那段重建就不会跑，
         * 于是下面 m_geometry->setVertexCount() / m_material->setTexture() 就是
         * 空指针解引用。缓存指针不全就干脆把旧节点丢掉，下一帧按 nullptr 走完整重建。
         * （这条是防"关闭窗口/换后端之后再画一帧"的路径，属于加固，不是本次崩溃的根因。）
         */
        if (m_geoNode == nullptr || m_geometry == nullptr || m_material == nullptr) {
            delete oldNode;
            return nullptr;
        }

        const qreal layerOpacity = qBound(0.0, controller->danmakuOpacity(), 1.0);

        if (!qFuzzyCompare(m_appliedOpacity, layerOpacity)) {
            m_appliedOpacity = layerOpacity;
            root->setOpacity(layerOpacity);
            /* setOpacity() 不会自己标脏，这里必须显式标一次（Qt 的约定） */
            root->markDirty(QSGNode::DirtyOpacity);
        }

        /*
         * ---- 时钟 ----
         *
         * 【基准是**引擎的时钟**，不是 QML 推来的播放位置】
         *
         * items[i].x 是引擎按它自己的时钟（DanmakuEngine::_clockMs，
         * danmaku/src/DanmakuEngine.cpp:306）算出来的，所以：
         *
         *     refSec = 引擎算这批 x 的那一刻（controller->engineClockMs()）
         *     nowSec = refSec + (现在 − 引擎打那一刻的墙钟时间戳) × 倍速
         *
         * 两条读数共用 danmakuMonoMs() 这**一条**单调时钟（见 DanmakuController.h）。
         *
         * 【为什么不能拿 player.position 当基准（用户实测的两个现象都出在这）】
         * player.position 是框架 2Hz 推来的原始位置，而引擎吃的是 QML 那条平滑时钟
         * smoothPositionMs（DanmakuView.qml:342）。两条时钟差一档（最多 500ms ≈ 几百像素）：
         *   * 画面上“锚点 + 外推”的位置随那一档一进一退 → "一顿一顿"；
         *   * 引擎命中检测用的还是它自己那份 x（对应屏幕上另一个位置），鼠标移到**看得见**
         *     的那条上，冻住的却是引擎认为在那儿的那条 → "鼠标移上去往后跑一段"。
         * 换成引擎时钟之后误差只剩"两个引擎 tick 之间"那一点点外推（≤33ms），
         * 而且命中检测和画面用的是同一个 x —— 悬停冻的就是你看见的那一条。
         *
         * 【暂停】引擎暂停时不推进自己的时钟（DanmakuEngine.cpp:294-297），这里也不外推
         * （clockRunning=false → extraMs=0）→ 弹幕画在引擎冻结的位置上，一格都不动。
         * 【seek / 换片】引擎 applySeek 把时钟挪过去，engineClockMs() 直接跟着变，
         * 不需要任何平滑 —— 那时候画面本来就在跳。
         */
        const qint64 engineStampMono = controller->engineClockMonoMs();
        /*
         * 【采样点 = 最近一次**换帧**时刻，而不是"现在"】
         * 见 onWindowChanged 里那段说明：sync 阶段的"现在"带着 GUI 线程的调度抖动，
         * 而换帧时刻是渲染线程紧循环等 vsync 采到的，用它算位置，步进才是均匀的
         * （和浏览器 CSS 动画同一个道理）。第一帧还没有换帧记录时退回"现在"。
         */
        const qint64 monoNowRaw = (m_lastSwapMonoMs >= 0) ? m_lastSwapMonoMs : monotonicMs();

        qreal extraMs = 0.0;

        if (m_clockRunning && engineStampMono >= 0) {
            if (engineStampMono != m_lastEngineStampMono) {
                /*
                 * 引擎刚推了一帧（时间戳变了）→ 直接按"引擎那一刻"重算。
                 * 这是**权威值**：顺带把增量累加攒下的那点误差清掉（不累积漂移）。
                 */
                extraMs = qMax<qreal>(0.0, static_cast<qreal>(monoNowRaw - engineStampMono)) * m_rate;
            } else if (m_lastFrameMonoMs >= 0) {
                /*
                 * 引擎这一帧还没推（两次 engine.update 之间）：按**上一帧到这一帧**的真实
                 * 间隔增量累加。
                 *
                 * 【为什么必须增量、不能每次都拿 monoNow − engineStampMono】
                 * 暂停时引擎不再 update，时间戳就停在**暂停那一刻**；恢复的瞬间
                 * `clockRunning` 先变真（QML 绑定），而引擎恢复后的第一帧还没到 ——
                 * 这时候做减法就把**整段暂停时长**当成了"流逝时间"，弹幕猛地往前飞一段
                 * （用户实测："暂停继续会往前弹一下再往后继续"）。增量累加永远只加
                 * "上一帧→这一帧"这一小段，天然不会跨过任何停止段。
                 */
                extraMs = m_lastExtraMs
                          + qBound<qreal>(0.0, static_cast<qreal>(monoNowRaw - m_lastFrameMonoMs) / 1000.0,
                                          0.25) * 1000.0 * m_rate;
            }
        }

        /* 暂停/停播：一条都不外推，弹幕就画在引擎冻结的位置上 */
        if (!m_clockRunning)
            extraMs = 0.0;

        m_lastExtraMs = extraMs;
        m_lastFrameMonoMs = monoNowRaw;
        m_lastEngineStampMono = engineStampMono;

        const qreal refSec = static_cast<qreal>(controller->engineClockMs()) / 1000.0;
        const qreal nowSec = refSec + extraMs / 1000.0;

        /*
         * ---- 【★ 这一帧只用自己的一份快照，不再直接引用引擎的向量 ★】----
         *
         * 老写法 `const std::vector<RenderItem> &items = controller->activeRenderItems();`
         * 拿的是**控制器对象里那个向量本身**的引用，而 GUI 线程每帧都在重写它
         * （DanmakuController::update/load/clear/reset/append → 引擎的 _renderOut，
         *   见 danmaku/src/DanmakuEngine.cpp:1281 rebuildRenderOut：clear + reserve + push_back）；
         * 本函数跑在渲染线程、读的时候**不持锁**，于是有两个实实在在的坏结果：
         *
         *   1) 下面 `m_entries.resize(items.size())` 按"那一刻"的条数分配，而第一趟那个
         *      循环的条件写的是 `i < items.size()` —— **每一次迭代都重新读一遍 size()**。
         *      写方在中间 push_back 的话，循环就会跑到比 resize 出来的长度更远的地方，
         *      `m_entries[i] = …` 越过 m_entries 堆块的末尾 —— 用户抓到过的那句
         *      `HEAP CORRUPTION DETECTED … wrote to memory after end of heap buffer`
         *      就是它；Debug STL 那套迭代器代理（m_entries 的内部状态）被旁边的堆块踩坏
         *      之后，析构时 `~vector → _Tidy → _Orphan_all` 读代理链就是 0xc0000005，
         *      正是用户日志 1641 行往下那 8 层栈。
         *   2) 写方 reserve/push_back 触发的**重新分配**会把旧缓冲释放掉，而我们这边的
         *      引用还指着它 → items[i] 读到的是一条已释放/已改写过的 RenderItem。
         *
         * 修法就是 DanmakuController.h 里那把锁注释里本来写着的那句
         * ——"改的时候上锁、读的时候**拷一份快照**再解锁"：拷贝只在锁内发生
         * （几十条 POD，μs 级），锁外的整帧都只用这份快照，条数和指针都不再会变。
         */
        std::vector<RenderItem> &items = m_itemsSnapshot;

        {
            QMutexLocker renderLocker(&danmakuRenderMutex());

            items = controller->activeRenderItems();

            /*
             * 【加固】顺手把文字字节抄进 m_textArena，并让 text 指向我们自己的内存。
             * RenderItem::text 是引擎内部 std::string 的裸指针，有效期只到下一次
             * load/reset/clear/append（DanmakuTypes.h:214-226 写着这条约定）；拷快照
             * 挡不住它 —— 解锁之后 GUI 线程做一次 load，这些指针就悬空了，而下面
             * `QString::fromUtf8(item.text, …)`（第一趟那个循环里）会照着它读。
             * （这一条不是本次崩溃栈的直接原因，属同一段代码上的已知隐患。）
             * 太长的文字按老规矩当"没有文字"（第一趟里那条 textLength 检查会跳过它）。
             */
            const auto usableTextLength = [](const RenderItem &item) -> size_t {
                return (item.text != nullptr && item.textLength > 0
                        && item.textLength <= static_cast<size_t>(kMaxTextLength))
                           ? item.textLength
                           : 0;
            };

            size_t arenaBytes = 0;

            for (const RenderItem &item : items)
                arenaBytes += usableTextLength(item) + 1;

            m_textArena.resize(arenaBytes);

            char *write = m_textArena.data();

            for (RenderItem &item : items) {
                const size_t len = usableTextLength(item);

                if (len == 0) {
                    item.text = nullptr;
                    item.textLength = 0;
                    continue;
                }

                std::memcpy(write, item.text, len);
                item.text = write;
                item.textLength = len;
                write[len] = '\0';
                write += len + 1;
            }
        }

        /*
         * 【条数只读一次】快照之后它本来就不会再变，这里显式钉成局部量：
         * 下面所有按下标写 m_entries / m_uvs 的地方都必须用它，不许再出现 items.size()
         * —— 那正是"循环条件比 resize 长度大"这类越界写的入口。
         */
        const size_t itemCount = items.size();

        const qreal dpr = window() != nullptr ? window()->effectiveDevicePixelRatio() : 1.0;

        /*
         * ---- "图集键"签名 ----
         * 只要它没变，就不用再问一次图集（每条一次哈希查找 + 可能需要烤一张图）。
         * 签名里必须带上"会改变烤出来长什么样"的那些字段（字号/描边/颜色/盒子尺寸）
         * 和图集世代（图集满了会整体重烤，旧 UV 就全废了）。
         */
        quint64 signature = 1469598103934665603ull;

        for (size_t i = 0; i < itemCount; ++i) {
            const RenderItem &item = items[i];

            mixIn(&signature, static_cast<quint64>(item.id));
            mixIn(&signature, static_cast<quint64>(item.fontPixelSize));
            mixIn(&signature, static_cast<quint64>(item.outlineType));
            mixIn(&signature, static_cast<quint64>(item.bold ? 1 : 0));
            mixIn(&signature, static_cast<quint64>(item.isSelf ? 1 : 0));
            mixIn(&signature, static_cast<quint64>(item.colorRGBA));
            mixIn(&signature, static_cast<quint64>(std::lround(item.width)));
            mixIn(&signature, static_cast<quint64>(std::lround(item.height)));
        }

        mixIn(&signature, static_cast<quint64>(m_atlas.generation()));
        /*
         * 上传流水号也要进签名：图集是**合并上传**的，刚烤好的条目要等下一批上传
         * 才能真正画出来 —— 上传一发生这个号就变，于是这一帧会重新问一次图集，
         * 新弹幕立刻出现（不用等下一次集合变化）。
         */
        mixIn(&signature, static_cast<quint64>(m_atlas.uploadedSerial()));
        mixIn(&signature, static_cast<quint64>(std::lround(dpr * 1000.0)));

        if (signature != m_builtSignature) {
            m_entries.resize(itemCount);

            int freshCount = 0;

            /*
             * ---- 第一趟：把这一帧每条的图集条目问出来（缓存命中就是一次哈希查找）----
             * 顺带数出有多少条**刚烤好、还没进 GPU 纹理**（上传是合并做的，见
             * DanmakuAtlas::texture）：这个数字决定下面要不要在本帧就把纹理传上去。
             *
             * 【这一趟可能要重来一次】图集满了会在**这一趟中途**整体重烤
             * （DanmakuAtlas::allocate → resetPacking）。重烤之后，之前那几条拿到的 UV
             * 已经指向新图集里的**别的**文案了 —— 再照着画就是花屏（"图集世代"那个
             * 计数器就是为这件事存在的，见 DanmakuAtlas.h 的 generation()）。
             * 所以每趟先记下世代号，一趟走完发现世代变了就**再走一趟**：第二趟里，
             * 重烤之后已经烤进去的条目是缓存命中，重烤之前的那几条会被补烤进新图集，
             * 于是全部条目都是有效、且和最终那张图集对得上的 UV。
             * 最多两趟 —— 免得"在屏条数比整张图集装得下的还多"时来回死循环
             * （正常在屏也就几十条，1024² 图集放得下；真放不下时引擎那边也会按
             * maxRenderCount 丢，不会无限重烤）。
             */
            for (int attempt = 0; attempt < 2; ++attempt) {
                const quint32 generationBefore = m_atlas.generation();

                freshCount = 0;

                /*
                 * 【条数用上面钉死的 itemCount，不再每次迭代重读 items.size()】
                 * 老写法 `i < items.size()` 就是"越界写 m_entries"的入口：
                 * resize(itemCount) 之后写方若在中间把 items 加长了，这个循环就会
                 * 跑到 m_entries 的容量外面去（见 updatePaintNode 开头那段说明）。
                 */
                for (size_t i = 0; i < itemCount; ++i) {
                    const RenderItem &item = items[i];

                    /*
                     * 先清空这一格：下面那些检查挡掉的条目（没文字 / 盒子尺寸不合理）必须
                     * 留下一个**空**条目 —— m_entries 是按**下标**复用的，不清的话上一帧
                     * 存在这里的旧条目会被第二趟当成它的 UV，等于画了上一格的内容。
                     */
                    m_entries[i] = DanmakuAtlasEntry();

                    if (item.text == nullptr || item.textLength == 0
                        || item.textLength > static_cast<size_t>(kMaxTextLength))
                        continue;

                    if (!(item.width > 0.0f && item.height > 0.0f)
                        || item.width > kMaxBoxWidth || item.height > kMaxBoxHeight)
                        continue;

                    DanmakuAtlas::Request request;
                    request.text = QString::fromUtf8(item.text, static_cast<int>(item.textLength));
                    request.color = colorOf(item.colorRGBA);
                    request.pixelSize = item.fontPixelSize;
                    request.bold = item.bold;
                    request.fontFamily = controller->fontFamily();
                    request.outlineType = item.outlineType;
                    request.selfBorder = item.isSelf;
                    request.fixedStyle = (item.type != DanmakuTypeScroll);
                    request.padH = item.padH;
                    request.padV = item.padV;
                    request.borderWidth = item.borderWidth;
                    request.boxWidth = item.width;
                    request.boxHeight = item.height;
                    request.devicePixelRatio = dpr;

                    m_entries[i] = m_atlas.entry(request);

                    if (m_entries[i].valid && m_entries[i].serial > m_atlas.uploadedSerial())
                        ++freshCount;
                }

                /* 世代没变 = 这一趟没有整体重烤 → 拿到的 UV 全都对得上，走下一段 */
                if (m_atlas.generation() == generationBefore)
                    break;
            }

            /*
             * 【★ "刚烤好还没上传"的条目为什么不能再一律跳过 ★】
             *
             * 上传是合并做的（最多每 100ms 一次，见 DanmakuAtlas::texture）——
             * 这条策略本身是对的：每烤一句就重建整张 1024² 纹理（4MB）会卡。
             * 但以前这里对"serial > uploadedSerial"的条目**一律这一帧不画**，
             * 于是**图集满 → 整体重烤**（DanmakuAtlas::resetPacking：缓存清空、世代 +1）
             * 那一下，在屏的**每一条**都变成"刚烤好还没上传" → 整屏弹幕集体消失，
             * 一直等到下一个上传窗口（最多 100ms）才回来 ——
             * 用户看到的就是"整屏弹幕闪一下"（图集 1024² 只放得下几十条文案，
             * 一部片子里会重烤十几次，所以是"有时候"闪）。
             *
             * 所以：**一次重烤了一大批**（条数够多，或者图集世代刚变）的时候，本帧就把
             * 纹理传上去 —— 传完再填 UV，同一帧里弹幕一条都不会消失（下面"第二趟"）。
             * 单条/两三条新弹幕仍旧照旧攒到下一个上传窗口：它本来就从右边缘滑进来，
             * 晚 100ms 看不出来，而且**不能**为它每帧重传 4MB —— 那正是当初要压掉的卡顿。
             */
            /*
             * 【★ 只要有新烤的条目就立刻上传 —— 这是"弹幕会闪一下"的根治 ★】
             *
             * 老条件只在"一次重烤了一大批"或"图集世代变了"时才立刻上传；单条新文案
             * 要等最多 100ms 的合并上传窗口。而**还没上传的条目这一帧是跳过不画的**
             * （m_uvs 保持空矩形）—— 于是：
             *   * 暂停/继续、滚动中途来了新文案 → 那一两条先消失一下（"闪一下"）；
             *   * 图集满 → resetPacking 整体重烤那一下 → 整层一起消失到下一个上传窗口
             *     （用户说的"整屏闪一下"）。
             * 现在改成 freshCount > 0 就 force：新文案**当帧就进纹理**，屏幕上永远不会
             * 出现"该画却因为没上传而不画"的一帧。
             *
             * 代价是上传次数变多（每次都要重传整张图集），但只有**真的烤了新文案**时才发生
             * （稳态下每秒几条，不是每帧）——比"看得见的闪烁"划算得多。
             */
            const bool bulkRebake = (freshCount > 0)
                                    || (m_lastAtlasGeneration != m_atlas.generation());

            if (freshCount > 0 && bulkRebake)
                m_forceAtlasUpload = true;

            m_lastAtlasGeneration = m_atlas.generation();

            m_uvs.assign(itemCount, QRectF());
            m_uvsReady = false;         /* UV 要等纹理传完（第二趟）才能定 */
            m_builtSignature = signature;
        }

        /*
         * 尺寸/全屏刚变过（GUI 线程置的那个标志），或者上面判定"整批重烤"：
         * 这一帧立刻把纹理传上去 —— 否则这批条目最多 100ms 画不出来，
         * 用户看到的就是切尺寸/全屏或图集重烤时"整屏弹幕闪一下"。
         * 只在渲染线程碰 m_atlas（见上面时钟那段线程说明）。
         */
        if (m_forceAtlasUpload) {
            m_atlas.requestImmediateUpload();
            m_forceAtlasUpload = false;
        }

        QSGTexture *texture = m_atlas.texture(window());

        if (texture == nullptr) {
            /* 图集这一帧没有可用纹理（窗口/上下文刚丢）：这一帧不画，等下一帧 */
            m_geometry->setVertexCount(0);
            m_geometry->setIndexCount(0);
            m_geoNode->markDirty(QSGNode::DirtyGeometry);
            setDrawnCount(0);
            return root;
        }

        if (texture != m_boundTexture) {
            m_material->setTexture(texture);
            m_boundTexture = texture;
            m_geoNode->markDirty(QSGNode::DirtyMaterial);
        }

        /*
         * ---- 第二趟：填 UV（必须在 texture() 之后）----
         *
         * 上面的 texture() 要么刚把这一批传上去了、要么按合并策略先攒着 ——
         * uploadedSerial() 到这一步才是最终值，所以"这条到底进没进纹理"现在才问得准。
         * 还没进的照样先不画：抢着画会去采样一块还没传上去的区域
         * （画面上一块空白或上一轮的残留）。
         *
         * 顶点里的纹理坐标是**归一化**的（Qt 的纹理材质就是这么用的：
         * QSGImageNode::rebuildGeometry 的文档写着"texCoordMode 用来把 sourceRect
         * 归一化"），所以这里直接用图集给的 UV 矩形。
         */
        if (!m_uvsReady) {
            const quint64 uploaded = m_atlas.uploadedSerial();
            const size_t count = qMin(m_entries.size(), m_uvs.size());

            for (size_t i = 0; i < count; ++i) {
                if (!m_entries[i].valid || m_entries[i].serial > uploaded)
                    continue;

                m_uvs[i] = m_entries[i].uv;
            }

            m_uvsReady = true;
        }

        const int quads = rebuildGeometry(m_geometry, items, nowSec, refSec, dpr);

        m_geoNode->markDirty(QSGNode::DirtyGeometry);
        setDrawnCount(quads);

        /*
         * 【冷路径诊断】引擎说"现在屏上该有弹幕"，可这一帧一条都没画出来 —— 这就是
         * 用户嘴里的"弹幕不显示"。这条日志只在**真的画不出来**时打，而且 1 秒最多一条：
         * 正常播放时 quads > 0，一行都不会有，所以它不在热路径上。
         * （四种原因要靠这一行区分：图集没有可用纹理 → 上面已经 return；条目没进纹理
         *   → uploadedSerial 明显落后；盒子尺寸/文字被挡掉 → readyEntries=0；
         *   时钟没来 → 位置在屏幕外，这时 quads 反而 > 0，看 x 就知道。）
         */
        if (!items.empty() && quads == 0 && monoNowRaw - m_lastEmptyWarnMonoMs >= 1000) {
            m_lastEmptyWarnMonoMs = monoNowRaw;

            int readyEntries = 0;

            for (const DanmakuAtlasEntry &entry : m_entries) {
                if (entry.valid) {
                    ++readyEntries;
                }
            }

            AF_LOGW("弹幕没画出来：引擎在屏 %zu 条，图集可用条目 %d，uploadedSerial=%llu，世代=%u\n",
                    itemCount, readyEntries,
                    static_cast<unsigned long long>(m_atlas.uploadedSerial()),
                    static_cast<unsigned>(m_atlas.generation()));
        }

        /*
         * 【让渲染循环来驱动下一帧，而不是等定时器】
         *
         * 位置是在这里（sync 阶段）按墙上时钟现算的，所以"每一帧画出来的位置"永远是对的；
         * 剩下的抖动只可能来自**帧节奏**：定时器是自由跑的 60Hz，和显示器的 vsync
         * （59.94Hz 之类）不同步，久了就会偶尔挤掉/重复一帧。
         * requestUpdate() 是"请渲染循环再给我一帧"的官方入口：帧由 vsync 排，
         * 每帧的位置也就在那一帧真正的时间点上算出来 —— 这是能做到的最平滑的形态。
         *
         * 只在**时钟在走（播放中）且屏上真有弹幕**时请求：暂停或空屏时没人动，
         * 不该白烧 GPU。空屏之后新弹幕上屏的那一帧由 QML 侧 30Hz 的 setTimeMs 触发，
         * 一旦画出来（quads > 0）就又回到渲染循环驱动。
         * 定时器保留着当兜底（窗口被遮挡/最小化时 requestUpdate 不一定给帧）。
         */
        /*
         * 【requestUpdate() 只能从 GUI 线程调】本函数（updatePaintNode）在**渲染线程**执行，
         * 直接调会撞 Qt 的断言（Debug 下弹框，Release 下 Q_ASSERT 被编掉所以看不出来）：
         *     ASSERT failure in QWindow: "Updates can only be scheduled from the GUI (main) thread"
         *     （qwindow.cpp requestUpdate）
         * 以前弹幕层是空的 → quads == 0 → 这行从不执行，所以一直没暴露；
         * 现在有弹幕在飞，它每帧都跑。改成**排到 GUI 线程**执行：语义不变（还是"再给我一帧"），
         * 但满足 QWindow 的线程要求。窗口/ui 在排队期间被销毁也没关系，queued 调用会随对象丢弃。
         */
        if (m_clockRunning && quads > 0 && window() != nullptr)
            QMetaObject::invokeMethod(window(), "requestUpdate", Qt::QueuedConnection);

        return root;
    }

}// namespace cicadaqt
