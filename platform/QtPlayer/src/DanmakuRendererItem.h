// ===========================================================================
// DanmakuRendererItem —— 弹幕渲染器（替代 DanmakuView 里的 QML 对象池）
//
// 【它替代掉的是什么】
// 旧方案：每条弹幕是一个 QML 对象（`DanmakuItem`：Item + 1~6 个 Text + 白框 Rectangle），
// 位置由 `XAnimator` 在渲染线程逐条插值，每帧 GUI 线程还要往每条上写十几个属性。
// 新方案：**一个 QML 对象都不用、整屏弹幕只有一个场景图节点、一次绘制**。
// 文字已经烤进纹理图集（见 DanmakuAtlas），运行时只写顶点、只采样，没有任何排版。
//
//   旧                                 新
//   N 个 QML 对象 / N×2~6 个文本节点   1 个 QSGGeometryNode
//   每帧 N 条属性写入（GUI 线程）      每帧写 N×4 个顶点（渲染线程 sync 阶段）
//   文字：每个 Text 各自排版           文字：一次性烤进图集，运行时只采样
//
// 【为什么不用自定义 QSGMaterial + 自己烘焙 QSB】
// 本轮先写的是"自定义材质 + 顶点着色器里算位置"的版本，材质/UBO 布局/顶点属性寄存器/
// 纹理提交全按 Qt 文档写对了，`qsb -d` 打开生成的 .qsb 也能看到 6 个后端（含 HLSL 50）
// 和完整反射信息 —— 但 Qt 的 D3D11 后端在运行时就是取不到那份 HLSL：
//     No HLSL (shader model 5.0) code found in baked shader QShader(stage=0 …)
//     HLSL shader compilation failed:
//     Failed to build graphics pipeline state
// 结果是图集烤好了、四边形建好了、uniform 全对，屏幕上**一个像素都没有**。
// 现在用的是 Qt 自带的 `QSGTextureMaterial`（着色器编在 Qt 库里）+ 自己按帧写顶点。
//
// 【为什么不是"每条弹幕一个 QSGImageNode"】
// 也试过：能画出来，但每条一个节点意味着 N 个几何、N 次节点遍历，实测滚动发涩、CPU 也高。
// 合成一个 QSGGeometryNode 之后：**一次绘制**、顶点直接写、没有逐节点开销。
//
// 【每帧到底做了什么】
//   * 定时器 tick（默认 60Hz）→ 只调一次 `update()`，把 item 标记为脏；
//   * sync 阶段（GUI 线程被阻塞）→ updatePaintNode()：
//       - "图集键"没变：不碰图集，只按这一时刻写顶点（滚动位置在 CPU 上算）；
//       - 变了（有弹幕上/下屏、字号颜色变了、图集重烤过）：重问一次图集，
//         把每条的归一化 UV 缓存下来，再写顶点。
//
// 【时间怎么走】QML 每个 tick 把 `timeMs`（= DanmakuView 的 smoothPositionMs）喂进来，
// 那是"引擎算这一帧 x_ref 时的时刻"。本类记下它对应的墙上时钟，渲染时按
// `now = timeMs + (墙上流逝 × 倍速)` 外推 —— 于是 GUI 30Hz、画面 60fps，且和引擎
// 的 x_ref 永远对得上（两者用的是同一条时间轴）。
//
// 【悬停】引擎会把悬停的那条冻住（引擎自己的时钟停走 + 我们这里不外推），
// 所以鼠标坐标不用交给渲染器 —— 命中检测仍然在 QML/引擎那一侧。
//
// 【不透明度】QSGNode 没有不透明度，Qt 的做法是 `QSGOpacityNode` 子树：根节点就是它，
// 弹幕开关面板里的"不透明度"整层作用上去（实时）。代价：引擎逐条的 alpha（新弹幕的
// 淡入淡出）在 GPU 这条路不生效 —— 逐条套一层 QSGOpacityNode 会让每条各自一次离屏渲染，
// 开销不能接受。要逐条 alpha 就用 `renderMode: "qml"`。
// ===========================================================================
#ifndef CICADA_QT_DANMAKURENDERERITEM_H
#define CICADA_QT_DANMAKURENDERERITEM_H

#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>
#include <QtQuick/QQuickItem>
#include <QtQml/qqmlregistration.h>

#include <vector>

#include "DanmakuAtlas.h"

/* RenderItem 是弹幕引擎的"这一帧要画什么"结构（rebuildGeometry 的入参） */
#include <Danmaku/DanmakuTypes.h>

QT_BEGIN_NAMESPACE
class QSGGeometry;
class QSGGeometryNode;
class QSGTextureMaterial;
QT_END_NAMESPACE

namespace cicadaqt {

    class DanmakuController;

    class DanmakuRendererItem : public QQuickItem {
        Q_OBJECT
        /* QML 里直接写 DanmakuRendererItem { }（少了 qqmlregistration.h 会报"缺少 ;"） */
        QML_ELEMENT

        /* 数据源：DanmakuView 里那个 DanmakuController 实例 */
        Q_PROPERTY(QObject *controller READ controller WRITE setController NOTIFY controllerChanged)

        /*
         * 弹幕时钟锚点（毫秒，媒体时间基准）。QML 每个 tick 喂 smoothPositionMs；
         * 本类按"（现在 − 锚点时间戳）× 倍速"外推（见文件头说明）。
         */
        Q_PROPERTY(qreal timeMs READ timeMs WRITE setTimeMs NOTIFY timeMsChanged)

        /* 上面那个锚点的时间戳（monoMs() 基准）；**必须和 timeMs 一起写** */
        Q_PROPERTY(qreal anchorMonoMs READ anchorMonoMs WRITE setAnchorMonoMs NOTIFY anchorMonoMsChanged)

        /* 播放倍速：两次锚点之间按它外推 */
        Q_PROPERTY(qreal rate READ rate WRITE setRate NOTIFY rateChanged)

        /* 时钟是否在走（暂停/停播 = false → 不外推，弹幕定住） */
        Q_PROPERTY(bool clockRunning READ clockRunning WRITE setClockRunning NOTIFY clockRunningChanged)

        /* 容器尺寸（逻辑像素）：算滚动速度要用（引擎也是按它算轨道的） */
        Q_PROPERTY(qreal containerWidth READ containerWidth WRITE setContainerWidth NOTIFY containerWidthChanged)
        Q_PROPERTY(qreal containerHeight READ containerHeight WRITE setContainerHeight NOTIFY containerHeightChanged)

        /* 渲染帧率上限。屏上没弹幕时自动降到 10Hz 慢查（和旧方案一样的策略） */
        Q_PROPERTY(int updateFps READ updateFps WRITE setUpdateFps NOTIFY updateFpsChanged)

        /* 诊断：这一帧画了多少条 / 图集里缓存了多少条 */
        Q_PROPERTY(int quadCount READ quadCount NOTIFY quadCountChanged)
        Q_PROPERTY(int atlasEntries READ atlasEntries NOTIFY quadCountChanged)

    public:
        explicit DanmakuRendererItem(QQuickItem *parent = nullptr);

        QObject *controller() const { return m_controllerObject; }
        void setController(QObject *controller);

        qreal timeMs() const { return m_timeMs; }
        void setTimeMs(qreal value);

        qreal rate() const { return m_rate; }
        void setRate(qreal value);

        bool clockRunning() const { return m_clockRunning; }
        void setClockRunning(bool value);

        qreal containerWidth() const { return m_containerWidth; }
        void setContainerWidth(qreal value);

        /* 锚点时间戳：必须和 timeMs 一起写（见成员说明） */
        qreal anchorMonoMs() const { return m_anchorMonoMs; }
        void setAnchorMonoMs(qreal value);

        /*
         * 单调时钟（毫秒，基准 = 进程内第一次调用）。
         * QML 用它取"喂锚点的那一刻"，渲染线程用同一个函数取"现在"，两边的时间差
         * 就是纯时间差。QElapsedTimer 的实现是 QueryPerformanceCounter 一类的高精度
         * 单调计时器（不受系统时间调整影响），elapsed() 是 const 的纯计算，两个线程
         * 同时读安全。
         */
        Q_INVOKABLE qint64 monoMs() const;

        qreal containerHeight() const { return m_containerHeight; }
        void setContainerHeight(qreal value);

        int updateFps() const { return m_updateFps; }
        void setUpdateFps(int value);

        int quadCount() const { return m_quadCount; }
        int atlasEntries() const { return m_atlas.cachedCount(); }

    signals:
        void controllerChanged();
        void timeMsChanged();
        void rateChanged();
        void clockRunningChanged();
        void containerWidthChanged();
        void containerHeightChanged();
        void updateFpsChanged();
        void quadCountChanged();
        void anchorMonoMsChanged();

    protected:
        QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;

        /*
         * 场景图失效（换渲染后端 / 设备丢失 / 窗口重建上下文）时 Qt 会调这里。
         * 图集那张纹理属于场景图，此时已经被销毁 —— 必须把这个指针放掉，否则
         * 下一帧会把一个野指针绑到材质上（崩在 Qt 内部，栈里看不出来是谁）。
         * 同时把节点指针清空：整棵树马上就会被 Qt 删掉。
         */
        void releaseResources() override;

    private:
        /* 定时器回调：只做一件事 —— 把 item 标记为脏，让下一帧重新走 updatePaintNode */
        void onTick();

        /*
         * 按"时钟在不在走 / 屏上有没有弹幕"重算定时器间隔（见 .cpp 里 onTick 的说明）。
         * 只在 GUI 线程调用（属性 setter、onTick、setDrawnCount）。
         */
        void refreshTickInterval();

        /* 窗口（场景图）变了：接上/断开"每帧标脏"（见 .cpp 里那段说明） */
        void onWindowChanged(QQuickWindow *window);

        /* 更新 quadCount 并发信号（只在真的变了的时候发） */
        void setDrawnCount(int count);

        /*
         * 把这一帧要画的弹幕写进顶点缓冲（顶点数 = 条数 × 4，三角形 = 条数 × 2）。
         * 返回真正写进去的条数。
         */
        int rebuildGeometry(QSGGeometry *geometry, const std::vector<RenderItem> &items,
                            qreal nowSec, qreal refSec, qreal dpr);

        DanmakuController *m_controller = nullptr;
        /* 同时也存一份 QObject 指针：controller() 要原样返回给 QML，不能返回转换后的类型 */
        QObject *m_controllerObject = nullptr;
        /* 控制器析构时把指针清掉（否则定时器还会去碰一个已经没了的对象） */
        QMetaObject::Connection m_controllerDestroyed;

        DanmakuAtlas m_atlas;
        QTimer m_timer;

        qreal m_timeMs = 0.0;
        qreal m_rate = 1.0;
        bool m_clockRunning = false;
        qreal m_containerWidth = 1280.0;
        qreal m_containerHeight = 720.0;
        int m_updateFps = 60;

        /*
         * 【QML 推来的锚点时间戳 —— 保留只为兼容 QML 那边的写入，不再参与定位】
         *
         * 渲染的时间基准已经换成**引擎时钟**：DanmakuController::engineClockMs() /
         * engineClockMonoMs()（见 .cpp 里"时钟"那一段，以及 DanmakuController.h 里
         * engineClockMs() 的说明 —— "一顿一顿"和"鼠标移上去往后跑一段"都出在那处基准上）。
         * 以前这里靠 QML 推来的 (timeMs, anchorMonoMs) 外推，还配了一整套暂停补偿
         * （m_carryMs / m_frozenMonoMs / m_lastFrameMonoMs）替两条时钟的差值打补丁；
         * 基准统一之后那套补偿连同这些成员一起删掉了。
         */
        qreal m_anchorMonoMs = -1.0;
        /*
         * 【外推量的增量累加】上一帧算出的外推量（毫秒）、那一帧的墙钟时间戳、
         * 以及上一帧看到的**引擎时钟时间戳**（变了 = 引擎又推了一帧，此时直接用
         * "现在 − 引擎那一刻"重算，见 .cpp 时钟那段）。
         *
         * 为什么必须增量：暂停时引擎不再 update，时间戳停在暂停那一刻；恢复瞬间
         * `clockRunning` 先变真、引擎那一帧还没到，直接做减法会把整段暂停时长算成流逝时间
         * ——弹幕往前飞一下（用户实测）。累加只加"上一帧→这一帧"（单帧上限 250ms，
         * GUI 卡顿/漏帧也不会让这一帧飞出去），不跨停止段。
         */
        qreal m_lastExtraMs = 0.0;
        qint64 m_lastFrameMonoMs = -1;
        qint64 m_lastEngineStampMono = -1;
        /*
         * 上一次打"引擎有在屏弹幕、本帧却一条都没画出来"这条警告的单调时间戳。
         *
         * 【为什么要有它】"弹幕不显示"是这个模块最容易复发、又最难从代码上看出来的
         * 故障（图集没纹理 / 条目还没上传 / 时间戳没来，都会表现成一样：全空）。
         * 这条警告只在**真的画不出来**时打，且 1 秒最多一条 —— 正常播放时 quads > 0，
         * 一行都不会有，所以它不在热路径上（见 .cpp 里 setDrawnCount 之后那段）。
         */
        qint64 m_lastEmptyWarnMonoMs = 0;
        /* 每帧标脏用的 frameSwapped 连接（见 .cpp 里 onWindowChanged 的说明） */
        QMetaObject::Connection m_frameSwapped;
        /*
         * 【抖动根治】换帧时刻的采样连接与它的时间戳（渲染线程写、渲染线程读）。
         *
         * 浏览器里 CSS transform 动画丝滑，是因为合成器在**每一帧的呈现时刻**按帧时间
         * 算矩阵；我们以前是在 updatePaintNode（sync 阶段）读墙上时钟，而 sync 什么时候
         * 发生取决于 GUI 线程忙不忙 → 采样点忽早忽晚 → 每帧步长忽大忽小（"一卡一卡"）。
         * frameSwapped 是渲染线程**换帧之后**立刻发的，用它当采样点就等于"按呈现时刻采样"。
         * 细节见 .cpp 里 updatePaintNode 的时钟那段。
         */
        QMetaObject::Connection m_frameSwappedStamp;
        qint64 m_lastSwapMonoMs = -1;
        /*
         * 场景图失效（窗口关 / 设备丢失）时的连接：只把图集纹理指针放掉，**不 delete**。
         * 少了它，~DanmakuAtlas 会在 QRhi 已经销毁之后去 delete 那张 QSGTexture ——
         * 关窗路径上的下一个崩点。见 .cpp 里 onWindowChanged 的说明。
         */
        QMetaObject::Connection m_sceneGraphInvalidated;
        /*
         * 容器尺寸变了（字号整批变）→ 下一帧要立刻把图集纹理传上去。
         * GUI 线程置真、渲染线程消费（见 updatePaintNode 里那次 requestImmediateUpload()），
         * 不要在 GUI 线程直接碰 m_atlas。
         */
        bool m_forceAtlasUpload = false;

        /* 上一帧画了多少条：定时器据此在 60Hz / 10Hz 之间切 */
        int m_quadCount = 0;

        /*
         * 场景图节点（根 = 不透明度节点，下面挂唯一的几何节点）。
         * 这两个指针**只在场景图活着的时候有效**：Qt 删掉整棵树之后会以
         * oldNode == nullptr 再调一次 updatePaintNode，那时在这里重新建（见 .cpp）。
         */
        QSGGeometryNode *m_geoNode = nullptr;
        QSGGeometry *m_geometry = nullptr;
        QSGTextureMaterial *m_material = nullptr;

        /*
         * 顶点缓冲已经能装多少条弹幕（容量，不是当前条数）。
         * 必须自己记：QSGGeometry 只留"当前条数"，用 vertexCount() 当容量判断的话，
         * 每帧都会认为"装不下"而重新分配一次缓冲（白烧 CPU）。
         */
        int m_quadCapacity = 0;

        /*
         * m_uvs[i] = 引擎这一帧第 i 条弹幕在图集里的**归一化** UV 矩形。
         * 它只在"图集键"变化时重算（见 m_builtSignature），每帧直接拿来写顶点。
         * 空矩形 = 这条没烤进图集（太宽/图集满），本帧不画。
         */
        std::vector<QRectF> m_uvs;

        /*
         * 这一帧问出来的图集条目（和 items 一一对应）。
         *
         * 【为什么要先存一份、再填 UV】UV 只能在纹理传完之后才定（见 .cpp 里"两趟"那段）：
         * 先问条目 + 数清有多少条还没进 GPU 纹理，据此决定这一帧要不要立刻把纹理传上去，
         * 传完再按"最终进没进纹理"填 m_uvs。存一份条目就省掉了第二遍图集查找
         * （虽然缓存命中也只是哈希查找，但每一帧每一条都来一次没必要）。
         */
        std::vector<DanmakuAtlasEntry> m_entries;

        /*
         * 【这一帧的数据快照 —— 用法见 .cpp 里 updatePaintNode 开头那段】
         *
         * 引擎那份 std::vector<RenderItem> 是**控制器对象的成员**，GUI 线程每帧
         * （update/load/clear/reset/append）都在重写它（DanmakuEngine.cpp:1281
         * rebuildRenderOut：clear + reserve + push_back），而渲染线程以前拿的是它的
         * **引用**、且不持锁。两者一交叉，渲染线程看到的就是"正在被改"的向量：
         * 容量换过 = 指针已经失效，条数还会在两次 size() 调用之间变大（这就是越界
         * 写 m_entries 的来源）。所以：**统一在 danmakuRenderMutex() 里拷一份**，
         * 锁外的整帧只用这份快照 —— 条数不会中途变，指针也不会被换掉。
         */
        std::vector<RenderItem> m_itemsSnapshot;

        /*
         * 【快照里那些 text 指针的落脚点】
         *
         * RenderItem::text 是指向引擎内部 std::string 的**裸指针**，有效期只到下一次
         * load/reset/clear/append（DanmakuTypes.h:214-226 写着这条约定）。拷快照挡不住
         * 它：解锁之后 GUI 线程做一次 load，字符串一没，指针就悬空了。所以拷快照时顺手
         * 把文字字节也抄进这块缓冲区，并把 text 指到我们自己的内存。
         */
        std::vector<char> m_textArena;

        /*
         * ⚠️ 【改本类的成员之后，必须让 QML 类型注册那个 TU 重编 —— 这是本工程里真实存在的坑】
         *
         * 本类在 QML 里是 `DanmakuRendererItem { }`（QML_ELEMENT），对象内存是**QML 引擎**
         * 按"注册时记下的 sizeof"分配、再用 placement new 构造的
         * （Qt 侧：include/QtQml/qqmlprivate.h 的 createInto<T>() 在第 182/214 行，
         *   sizeof(T) 在第 1094 行；分配在 QQmlType::create 里，见同一个头文件
         *   `operator delete` 上面那段注释）。而那个 sizeof 是在**生成的**
         * build/.../appqtplayer_qmltyperegistrations.cpp 里展开的（那个文件第 16-18 行
         * include 本头文件，第 36 行 qmlRegisterTypesAndRevisions<DanmakuRendererItem>）。
         *
         * 麻烦在于：本工程这个 build 树（NMake Makefiles JOM + CMake 4.2）里，
         * **那个 obj 的依赖表里没有本头文件** ——
         *   CMakeFiles/appQtPlayer.dir/compiler_depend.make:6 只有
         *       `...qmltyperegistrations.cpp.obj: appqtplayer_qmltyperegistrations.cpp`
         *   CMakeFiles/appQtPlayer.dir/build.make:488-490 也只有 flags.make / 那个 .cpp
         * 对比普通 TU（同两个文件里 DanmakuRendererItem.cpp.obj 有完整包含链）。
         * 而那个生成的 .cpp 只在 qmltyperegistrar 的输入变化时才重写（build.make:67-76），
         * 成员布局变化**不会**改变它 → 那个 obj 就是"旧对象文件"：
         * 按旧 sizeof 分配 + 用新头文件编译出来的构造/析构 → 构造就写到块外
         *   （CRT 弹 "HEAP CORRUPTION DETECTED … wrote to memory after end of heap buffer"），
         * 析构时 m_entries 的 Debug STL 代理读到块外内存 → `_Orphan_all` 里 0xc0000005。
         * 所以：**改完成员请把 CMakeFiles/appQtPlayer.dir 整个删掉再编**
         * （只重编这一个 target，不是全工程 clean）；或者给那个 obj 补 OBJECT_DEPENDS。
         */

        /*
         * m_uvs 是不是已经按当前的 uploadedSerial() 填好了。
         * 问条目那一趟（签名变化时）会把它置假，填完 UV 再置真 ——
         * 中间隔着一次 texture()（那一步才决定哪些条目真的进了纹理）。
         */
        bool m_uvsReady = true;

        /* 上一帧看到的图集世代：变了 = 图集刚整体重烤过（见 .cpp 里 bulkRebake 的说明） */
        quint32 m_lastAtlasGeneration = 0;

        /* 当前绑在材质上的图集纹理；图集重烤会换一张，换了要重绑 */
        QSGTexture *m_boundTexture = nullptr;

        /*
         * 已经写进根节点（QSGOpacityNode）的那层弹幕不透明度。
         * QSGOpacityNode 没有 opacity() 取值接口，所以自己记一份用来去重 —— 每帧无脑
         * setOpacity 会让 Qt 每帧都重建一次 opacity 更新流程。
         */
        qreal m_appliedOpacity = -1.0;

        /*
         * 上一帧"图集键"的签名（条数 + 每条弹幕的 id/字号/描边/颜色/盒子尺寸 + 图集世代）。
         * 和这一帧一样就**不重新问图集** —— 稳态每帧只写顶点。
         * 光比条数不够：同一格里换了一条弹幕、或者改了字号，条数不变但 UV 必须重问。
         */
        quint64 m_builtSignature = 0;

        /*
         * ============ 【暂停后不再每帧标脏：省掉"白烧 GPU"的那条自维持链】============
         *
         * 【要修的现象】点暂停之后画面明明不动了，GPU 占用却一直下不来。
         *
         * 【根因（两条，缺一条都修不掉）】
         *   1. m_timer 一直开着，而 onTick() 只看 m_quadCount 决定 60Hz/10Hz ——
         *      暂停时弹幕冻结在屏上，m_quadCount 仍然是 >0 ⇒ **暂停后照样以 60Hz 标脏**；
         *   2. onWindowChanged() 里那条 `frameSwapped → update()` 连接是**自维持**的：
         *      标脏 → 场景图出一帧 → frameSwapped → 再标脏。哪怕把定时器停了，这一条也
         *      能靠"已经有一帧在画"自己转下去。
         *   于是场景图**每帧重绘**，连带 CicadaPlayerItem::updatePaintNode() 每帧都要提交
         *   同一帧（同一帧重跑视频处理器那半由 CicadaPlayerItem 的纹理缓存堵掉了）。
         *
         * 【修法】两条链都加同一个闸门：**只有"弹幕时钟在走"时才需要连续标脏**。
         *   暂停 / 停播时（m_clockRunning == false）位置只在引擎推来新弹幕时变，而那一刻
         *   QML 会写 timeMs / controller 属性 → 那两个 setter 会 update() 一帧（见 .cpp），
         *   不需要按帧轮询。暂停那一刻的"最后一帧"由 setClockRunning(false) 自己补。
         *
         * 【为什么是节流而不是彻底不标】暂停时留一个**慢心跳**（kPausedTickIntervalMs =
         *   500ms，frameSwapped 那条 1500ms）只是兜底：万一有哪条状态变化没走到那两个 setter
         *   （历史上有过"弹幕加载完却不显示"的坑），最多晚 500ms 补上。相比原来的 60Hz，
         *   无效帧压到 1/30，可以忽略。
         */
        qint64 m_frameSwappedTickMonoMs = -1;
        qint64 m_lastRepaintMonoMs = -1;

        /*
         * ============ 【"停 → 走"边沿检测：恢复播放的首帧不许外推】============
         *
         * 取值：-1 = 还没画过第一帧（状态未知）；0 = 上一帧时钟是停的；1 = 上一帧在走。
         *
         * 【为什么需要它】恢复播放时 `clockRunning` 绑定先变真，而引擎恢复后的第一帧
         * （DanmakuController::update → engine.update）**还没到**。那一帧里引擎时间戳没变，
         * 于是外推会落到"增量累加"分支，把 `monoNow − m_lastFrameMonoMs` —— 也就是
         * **整段暂停时长**（被 qBound 钳到 250ms 上限）—— 当成流逝时间加上去：
         * 屏幕上就是"往前弹一下"，下一帧引擎时间戳一变又缩回去
         * （用户实测："暂停后再继续会往前弹一下再往后缩最后再滚动"）。
         * 详见 .cpp 里 updatePaintNode 的 `resumed` 那段。
         *
         * 只在渲染线程读写（updatePaintNode 一帧一次），不需要同步。
         */
        int m_clockWasRunning = -1;
    };

}// namespace cicadaqt

#endif// CICADA_QT_DANMAKURENDERERITEM_H
