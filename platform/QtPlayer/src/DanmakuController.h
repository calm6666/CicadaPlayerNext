// ===========================================================================
// DanmakuController —— 把独立的 C++ 弹幕引擎（Danmaku）接到 QML 上
//
// 分工（用户要求）：**逻辑全在 C++，QML 只渲染**。
//   * 引擎本体在 CicadaPlayerNext/danmaku/：纯 C++11、跨平台、不依赖 Qt / 本框架，
//     可以单独编成 Danmaku.dll 给别的项目用（那一侧已经有 305 项自检）。
//   * 本文件是**薄适配层**：把 QML 的属性/调用转成引擎调用，再把引擎算好的
//     "这一帧要画哪些弹幕、画在哪" 变成 QML 能直接用的列表（activeItems）。
//     它只依赖 QtCore，不含任何弹幕算法 —— 算法改了不用动这里。
//
// 为什么这样做：现在的 DanmakuLayer.qml 把轨道分配、碰撞、分段、悬停冻结全写在 JS 里，
// 每帧都要在 QML 的 JS 引擎里跑一遍（还有大量 QML 对象），性能上不去。
// 换成"每帧只从 C++ 取一次结果、QML 按结果摆位置"之后，计算侧就不再有 JS 开销。
// ===========================================================================
#ifndef CICADA_QT_DANMAKUCONTROLLER_H
#define CICADA_QT_DANMAKUCONTROLLER_H

#include <QtCore/QObject>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtCore/QString>
/* QSet 用来存"上一帧在屏的弹幕 id"（见 refreshActiveItems 的说明） */
#include <QtCore/QSet>
#include <QtCore/QElapsedTimer>
/* 异步加载：结果队列 + 保护它的互斥量（见 loadXmlFileAsync 的说明） */
#include <QtCore/QMutex>
#include <QtCore/QVector>
/* QML_ELEMENT 这个宏定义在这里；少了它 Qt 的类型注册看不到，而且编译器会在
   `public:` 那一行报 "缺少 ;"（宏没展开，整段变成非法声明）—— 踩过一次。 */
#include <QtQml/qqmlregistration.h>

/*
 * 【GUI 线程改引擎 / 渲染线程读引擎 共用的一把锁】
 *
 * 这是"弹幕一开始上屏就崩"的关键：`DanmakuRendererItem::updatePaintNode()` 跑在
 * **渲染线程**，它读的是引擎内部的 `_renderOut` 向量；而 GUI 线程每帧
 * （`engine.update()`，30Hz）都在**重写那个向量**。这条链上还有渲染器自驱的
 * 定时器 / frameSwapped / queued requestUpdate —— 只要有一次读到"正在被改"的向量，
 * 写坏的就是这块内存：
 *   * 表现为 `~DanmakuRendererItem` 里 `m_entries` 内部指针非法（CRT 断言
 *     `_Adjust_manually_vector_aligned: invalid argument`），
 *   * 以及在 QML 的 JS 引擎里随机 `0xc0000005`（用户日志里 22 层栈全在 Qt6Qmld）。
 * 所以：**改的时候上锁、读的时候拷一份快照再解锁**。
 *
 * 用 inline 函数里的 static：两个文件都能拿到同一把锁，不用改构造/析构，
 * 也不会出现"每个编译单元一把锁"的问题。
 */
namespace cicadaqt {
    inline QMutex &danmakuRenderMutex()
    {
        static QMutex mutex;
        return mutex;
    }

    /*
     * 进程内单调时钟（毫秒，基准 = 第一次调用）。
     *
     * 【为什么必须共用一条】控制器要记"引擎算这一帧 x 的时刻"，渲染器要算"从那一刻
     * 到现在过了多久" —— 这两个读数一减就是纯粹的时间差。如果两边各自 startNew()
     * 一个 QElapsedTimer，基准不同，减出来的东西毫无意义。QElapsedTimer 底下是
     * QueryPerformanceCounter 一类的高精度单调计时器（不受系统时间调整影响），
     * elapsed() 是纯计算，两个线程同时读安全。
     */
    inline qint64 danmakuMonoMs()
    {
        static QElapsedTimer clock = []() {
            QElapsedTimer t;
            t.start();
            return t;
        }();

        return clock.elapsed();
    }
}

#include <Danmaku/DanmakuEngine.h>

namespace cicadaqt {

    class DanmakuController : public QObject
    {
        Q_OBJECT
        /* QML 里直接写 `DanmakuController { }`（qt_add_qml_module 会注册这个类型） */
        QML_ELEMENT

        /* ---------------- 开关与渲染设置（对应参考里的 options） ---------------- */

        /* 总开关：关掉时 update() 不产生任何在屏项（控制栏那颗"弹"控制它） */
        Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)

        /* 显示区域占比（1.0 = 全屏；参考里是 4 档：1 / 0.75 / 0.5 / 0.25） */
        Q_PROPERTY(qreal areaRatio READ areaRatio WRITE setAreaRatio NOTIFY areaRatioChanged)

        /* 滚动速度倍率：参考里 5 档（0.5 / 0.75 / 1 / 1.5 / 2），基准 150px/s */
        Q_PROPERTY(qreal speedMultiplier READ speedMultiplier WRITE setSpeedMultiplier
                   NOTIFY speedMultiplierChanged)

        /* 字号缩放 + 自动缩放（窗口变宽时自动放大，夹在 0.75~1.5） */
        Q_PROPERTY(qreal fontSizeScale READ fontSizeScale WRITE setFontSizeScale NOTIFY fontSizeScaleChanged)
        Q_PROPERTY(bool autoScale READ autoScale WRITE setAutoScale NOTIFY autoScaleChanged)
        /* 基准字号（设置面板"字号"那一行写的就是它） */
        Q_PROPERTY(int baseFontSize READ baseFontSize WRITE setBaseFontSize NOTIFY baseFontSizeChanged)

        /*
         * 弹幕字体族：**只为"量文字宽度"用**（渲染那边的字体由 QML 的 DanmakuItem 自己设）。
         *
         * 引擎必须知道每条弹幕的像素宽度才能算轨道、算碰撞、算整盒尺寸；不接测量的话它只能
         * 按"码点数 × 字号"估算 —— 估算偏小，本人的白框就会比真实文字窄、右边漏出一截
         * （用户反馈的就是这个）。所以这里把 QML 正在用的字体族交给它，用 QFontMetricsF 量真值。
         */
        Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY fontFamilyChanged)

        /* 不透明度（0.1 ~ 1） */
        Q_PROPERTY(qreal danmakuOpacity READ danmakuOpacity WRITE setDanmakuOpacity
                   NOTIFY danmakuOpacityChanged)

        /* 粗体 / 描边方式（0 重墨、1 描边、2 45°投影 —— 和设置面板里的三档一致） */
        Q_PROPERTY(bool fontBold READ fontBold WRITE setFontBold NOTIFY fontBoldChanged)
        Q_PROPERTY(int outlineType READ outlineType WRITE setOutlineType NOTIFY outlineTypeChanged)

        /*
         * 屏蔽开关（和设置面板里那排图标一一对应）。
         * 【语义：置 true = **屏蔽掉**这一类】（引擎里 `matchesFilter` 的注释写得很清楚：
         * QML:534-557 / domEngine.ts:469-496 "置 true = 过滤掉"）。
         * 【踩过的坑】这四个属性一开始我按"显示开关"给了默认 true —— 结果等于"全部屏蔽"，
         * 灌进去 50 条一条都没上屏（日志里 admitted=0 / dropped=17），排查了半天。
         * 默认必须是 false（什么都不屏蔽）。
         */
        Q_PROPERTY(bool blockScroll READ blockScroll WRITE setBlockScroll NOTIFY filterChanged)
        Q_PROPERTY(bool blockFixed READ blockFixed WRITE setBlockFixed NOTIFY filterChanged)
        Q_PROPERTY(bool blockColorful READ blockColorful WRITE setBlockColorful NOTIFY filterChanged)
        Q_PROPERTY(bool blockAdvanced READ blockAdvanced WRITE setBlockAdvanced NOTIFY filterChanged)

        /* ---------------- 每帧结果（QML 只读这个） ---------------- */

        /*
         * 这一帧要画的弹幕列表：每项是一个 map
         *   { id, text, x, y, w, h, fontSize, alpha, scale, color, type, isSelf,
         *     borderWidth, bold, outlineType }
         * x/y 是**弹幕层坐标**（左上角），渲染侧不需要再做任何计算。
         * 顺序稳定（引擎按准入顺序给），QML 用 Repeater 直接铺即可。
         */
        Q_PROPERTY(QVariantList activeItems READ activeItems NOTIFY frameChanged)

        /* 帧号：只有它变了才需要重新读 activeItems（QML 里 bind 它做去重） */
        Q_PROPERTY(int frameRevision READ frameRevision NOTIFY frameChanged)

        /*
         * 宿主是不是在用 **GPU 渲染路径**（DanmakuRendererItem 直接读引擎数据）。
         *
         * 【为什么要告诉控制器】GPU 那条路根本不经过 activeItems 这张 QML 列表
         * （见 activeRenderItems() 的说明），可 update() 每帧还是把整张列表建一遍
         * （每条 10 个键的 QVariantMap）—— 建了没人看，纯白烧。
         * 置真之后 refreshActiveItems() 就**只更新统计 + 发 frameChanged**（悬停气泡
         * 和诊断浮层要的还是这两个），不再建列表。
         * QML 那边按 renderMode 直接绑（DanmakuView.qml）。
         */
        Q_PROPERTY(bool gpuLayer READ gpuLayer WRITE setGpuLayer NOTIFY gpuLayerChanged)

        /* 诊断：活动数 / 轨道数 / 累计准入 / 丢弃 / 待发（接设置页或调试浮层） */
        Q_PROPERTY(QVariantMap stats READ stats NOTIFY statsChanged)

        /*
         * 悬停中那条弹幕：{ id, text, isSelf, x, y, w, h }；没有悬停目标时是**空 map**。
         *
         * 【谁在用】弹幕悬停气泡（DanmakuView.qml 里那个 danmakuTip）。参考
         * VideoPlayer/index.tsx:417-446 的 onDanmakuHover 回调就是把这些交给气泡的
         * （那边给 text/uid 和 position，这里把定位要用的 x/y/w/h 一起给全）。
         * 跟着 frameChanged 走：引擎每帧同步一次悬停状态，悬停超时（hoverTimeoutMs=3000）
         * 或鼠标移开时它自然变成空 map，气泡跟着消失 —— 参考那 3 秒自动关闭就是它。
         */
        Q_PROPERTY(QVariantMap hoveredItem READ hoveredItem NOTIFY frameChanged)

    public:
        explicit DanmakuController(QObject *parent = nullptr);
        ~DanmakuController() override;

        bool enabled() const { return m_enabled; }
        void setEnabled(bool value);

        qreal areaRatio() const { return m_areaRatio; }
        void setAreaRatio(qreal value);

        qreal speedMultiplier() const { return m_speedMultiplier; }
        void setSpeedMultiplier(qreal value);

        qreal fontSizeScale() const { return m_fontSizeScale; }
        void setFontSizeScale(qreal value);

        bool autoScale() const { return m_autoScale; }
        void setAutoScale(bool value);

        int baseFontSize() const { return m_baseFontSize; }
        void setBaseFontSize(int value);

        qreal danmakuOpacity() const { return m_danmakuOpacity; }
        void setDanmakuOpacity(qreal value);

        bool fontBold() const { return m_fontBold; }
        void setFontBold(bool value);

        int outlineType() const { return m_outlineType; }
        void setOutlineType(int value);

        bool blockScroll() const { return m_blockScroll; }
        void setBlockScroll(bool value);

        bool blockFixed() const { return m_blockFixed; }
        void setBlockFixed(bool value);

        bool blockColorful() const { return m_blockColorful; }
        void setBlockColorful(bool value);

        bool blockAdvanced() const { return m_blockAdvanced; }
        void setBlockAdvanced(bool value);

        QVariantList activeItems() const { return m_activeItems; }
        int frameRevision() const { return m_frameRevision; }

        bool gpuLayer() const { return m_gpuLayer; }
        void setGpuLayer(bool value);
        QVariantMap stats() const { return m_stats; }

        /* 悬停中那条弹幕的信息（见上面 Q_PROPERTY 的说明）；没有悬停目标时返回空 map */
        QVariantMap hoveredItem() const;

        /*
         * ------------------------------------------------------------------
         * 给 GPU 渲染器（DanmakuRendererItem）读的**一帧快照**。
         *
         * 直接返回引擎那份 `std::vector<RenderItem>`，**不经过 QVariantList →
         * QVariantMap → JS 对象**那一圈转换 —— 那是现在 30Hz 每帧的主要开销之一。
         *
         * 【为什么可以直接读、不用加锁】
         * 渲染器的 updatePaintNode() 跑在渲染线程的 **sync 阶段**，而 Qt 在 sync 期间
         * 会把 GUI 线程**阻塞住**（这正是场景图"GUI 线程写的属性可以安全地被渲染线程读到"
         * 的机制，所有 updatePaintNode 读 QQuickItem 成员的地方都靠它）。引擎只在
         * GUI 线程的 update() 里改这份 vector，所以两边不会同时碰它。
         *
         * 【text 指针的有效期】按引擎的约定，RenderItem::text 只在下一次
         * load/append/reset/clear 之前有效。渲染器只在本帧内拿它当图集缓存键，
         * 不跨帧保存 —— 符合约定。
         *
         * 【弹幕开关（enabled）必须在这里生效】GPU 这条路**不走 QML 的 activeItems 模型**
         * （那个模型在 refreshActiveItems 里被 m_enabled 挡住了，但 GPU 渲染器直接读引擎），
         * 所以只挡模型没用 —— 关掉弹幕开关时画面上的弹幕照样在动。
         * 这里统一：开关关掉 = 只剩**自己发的那几条**（用户在参考里的行为：关掉弹幕之后
         * 自己发的仍然看得见，否则点"发送"没有任何反馈）。
         * ------------------------------------------------------------------
         */
        const std::vector<RenderItem> &activeRenderItems() const
        {
            if (m_enabled) {
                return m_engine.activeItems();
            }

            /* 关掉开关：只挑出自己发的（每条都很短，条数也就是屏幕上那几十条） */
            m_renderScratch.clear();

            const std::vector<RenderItem> &all = m_engine.activeItems();

            for (size_t i = 0; i < all.size(); ++i) {
                if (all[i].isSelf)
                    m_renderScratch.push_back(all[i]);
            }

            return m_renderScratch;
        }

        /* ---------------- QML 调用的接口 ---------------- */

        /* 容器尺寸：必须在开播前/尺寸变化时调用（引擎的轨道表按它算） */
        Q_INVOKABLE void setContainerSize(qreal width, qreal height);

        /* 灌一批弹幕：每项 { text, timeMs, type, fontSize, color, isSelf, uid } */
        Q_INVOKABLE void load(const QVariantList &items);

        /*
         * 直接读一个弹幕文件（B 站那种 XML）并灌进引擎，返回解析出来的条数。
         *
         * 解析本身在**纯 C++ 的 Danmaku 库**里（Danmaku/DanmakuXml.h），这里只负责
         * "读文件 + 把结果交给引擎"—— 弹幕逻辑不落在 Qt 层，保持独立模块的边界。
         * 文件打不开、或里面一条有效弹幕都没有时返回 0（不报错、也不清掉已有弹幕）。
         *
         * ⚠️ 这是**同步**版本（读文件 + 解析都在调用线程上）。界面请用下面的
         * loadXmlFileAsync() —— 打开视频时那一下"卡"就是它造成的。
         */
        Q_INVOKABLE int loadXmlFile(const QString &path);

        /* ---------------- 异步加载（打开视频不再卡一下） ---------------- */

        /*
         * 异步加载弹幕：**读文件 / 解析 XML / 建列表全部丢到 Qt 线程池里**，
         * 解析完把结果交回 GUI 线程才喂引擎（引擎只在 GUI 线程上跑）。
         *
         * 【为什么要异步】这两条路原来都是同步的：一次解析上千条弹幕（readAll +
         * 解析 + 建上千个字符串/对象）全在 GUI 线程上跑完才继续 —— 用户看到的
         * 就是"每次打开视频，弹幕加载都会卡一下"。
         *
         * 返回**这次加载的序号**（token）：加载期间又发起过一次（换片；或者同名 xml
         * 不存在、改从 mock 拉）时，先回来的旧结果会被丢掉；QML 用这个序号认领自己的结果
         * （见下面两个信号）。调用本身不做任何解析，立刻返回。
         */
        Q_INVOKABLE int loadXmlFileAsync(const QString &path);
        Q_INVOKABLE int loadXmlTextAsync(const QString &xml);

        /* 单条追加；currentMs >= 0 时表示"立刻上屏"（自己发的弹幕走这条） */
        Q_INVOKABLE void append(const QVariantMap &item, qint64 currentMs = -1);

        /* 清屏（数据保留）/ 连数据一起清 */
        Q_INVOKABLE void clear();
        Q_INVOKABLE void reset();

        /* 每帧调一次：喂当前播放时间（毫秒），算完更新 activeItems */
        Q_INVOKABLE void update(qint64 timeMs);

        /* 跳转：清场并把游标挪到指定时间（下一帧 update 才发射） */
        Q_INVOKABLE void seekTo(qint64 timeMs);

        /*
         * 自己发一条：{ type, fontSize, speed, color, self, uid }。
         *
         * 【为什么返回 bool】以前是 void，于是"引擎根本没让这条上屏"（被屏蔽规则挡了、
         * 或者同屏条数到顶）时调用方完全看不出来 —— 用户看到的现象就是"弹幕发出去没反应"，
         * 而日志里只有一句"已发送"。现在把引擎的裁决如实返回，QML 侧据此打不同的日志（见
         * controls/DanmakuBar.qml 的 sendDanmakuText）。
         */
        Q_INVOKABLE bool send(const QString &text, const QVariantMap &options = QVariantMap());

        /*
         * 撤回一条**在屏**弹幕（悬停气泡里那颗"撤回"按钮；参考 index.tsx:905-923）。
         * 转发给引擎的 removeDanmaku()，然后立刻重建这一帧（气泡当场收掉）。
         * QML 侧只在**本人**那条上调用（isSelf）。返回是否真的撤掉了。
         */
        Q_INVOKABLE bool recallDanmaku(qint64 id);

        /* 往弹幕层灌 count 条示例弹幕（Ctrl+D 那个演示入口用；接了真实弹幕源就能删） */
        Q_INVOKABLE void generateSample(int count, int durationSec);

        /* 鼠标位置（悬停冻结 + hoveredId）；inside=false 表示鼠标不在弹幕层上 */
        Q_INVOKABLE void setPointer(qreal x, qreal y, bool inside);

        /*
         * 【引擎时钟 —— 渲染器唯一的时间基准】
         *
         * items[i].x 是引擎按**自己的时钟**（DanmakuEngine::_clockMs，写于
         * danmaku/src/DanmakuEngine.cpp:306）算出来的。渲染器要把那条弹幕外推到"这一帧"，
         * 就必须知道这个 x 对应的到底是哪一刻。
         *
         * 之前渲染器用的是 QML 绑过去的 `player.position`（框架 2Hz 推来的**原始**位置），
         * 而引擎吃的是 QML 那条平滑时钟 smoothPositionMs（DanmakuView.qml:342 的 smoothAt()：
         * 它领先于 player.position，而且是按位置回调一档一档走的）。两条时钟差一档
         * （最多 500ms ≈ 几百像素）时必然出两个现象：
         *   * 画面上"锚点 + 外推"算出来的位置随台阶一进一退 → 用户说的"一顿一顿"；
         *   * 引擎命中检测用的还是它自己那份 x（对应屏幕上另一个位置），鼠标移到**看得见**
         *     的那条上，冻住的却是引擎认为在那儿的那条 → 用户说的"鼠标移上去往后跑一段"。
         *
         * 现在把引擎那一刻的时钟值和墙钟时间戳成对记下来（见 .cpp 的 update()），
         * 渲染器以它们为基准：refSec = 引擎算这批 x 的时刻，now = refSec + 墙钟流逝 × 倍速。
         * 两条时钟合成一条，残余误差只剩"两个引擎 tick 之间"那一点点外推。
         */
        qint64 engineClockMs() const { return m_engineClockMs; }
        /* 上面那个时钟值对应的墙钟时间戳（danmakuMonoMs() 基准；-1 = 还没跑过一帧） */
        qint64 engineClockMonoMs() const { return m_engineClockMonoMs; }

        /* 暂停（暂停时 update 用同一个时间，弹幕就停住） */
        Q_INVOKABLE void setPaused(bool paused);

        /* 弹幕字体族（见上面 Q_PROPERTY 的说明：只用来量文字宽度） */
        QString fontFamily() const { return m_fontFamily; }
        void setFontFamily(const QString &family);

        /*
         * 【内部用，QML 不要调】工作线程解析完调这个：把结果塞进队列，再往 GUI 线程
         * 排一次 consumeParsedDanmaku() —— 真正的"喂引擎"必须在 GUI 线程做。
         */
        void onDanmakuParsed(int token, std::vector<DanmakuItem> items, const QString &error);

    signals:
        /* 异步加载完成：token = 发起时拿到的序号；count = 解析出来的条数（0 = 里面没有有效弹幕） */
        void danmakuLoaded(int token, int count);
        /* 异步加载失败（文件不存在 / 打不开）：reason 是给人看的原因 */
        void danmakuLoadFailed(int token, const QString &reason);
        void enabledChanged();
        void areaRatioChanged();
        void speedMultiplierChanged();
        void fontSizeScaleChanged();
        void autoScaleChanged();
        void baseFontSizeChanged();
        void danmakuOpacityChanged();
        void fontBoldChanged();
        void outlineTypeChanged();
        void filterChanged();
        void fontFamilyChanged();
        /* 每帧（或状态变化）发一次；QML 据此重画 */
        void frameChanged();
        void statsChanged();
        void gpuLayerChanged();

    private:
        /* 把 QML 属性写回引擎配置（改完不用重开片源） */
        void applyConfig();
        /*
         * 引擎结果 → QVariantList。
         *
         * fullPayload = 是否带上"整条弹幕不会变"的字段（text / color / isSelf / h / textAdvance）。
         * update() 传 **false**（30Hz 的常规帧只需要位置类字段），其余入口用默认 true。
         * 这一条是本轮为降 CPU 加的，理由和"哪 10 个字段任何一帧都不能少"写在
         * DanmakuController.cpp 里 refreshActiveItems 的函数头注释上 —— 改之前先读那段。
         */
        void refreshActiveItems(bool fullPayload = true);

        /* 把攒下的解析结果喂给引擎（只在 GUI 线程调，见 .cpp 里 consumeParsedDanmaku） */
        void consumeParsedDanmaku();

        /*
         * 一次异步解析的结果（工作线程写、GUI 线程取，用 m_loadMutex 保护）。
         * 做成队列而不是单个槽位：连发两次加载时，两个任务完成的**先后顺序**是不确定的，
         * 单个槽位会被后完成的那个覆盖，先完成的那个（可能正是最新的那次）就丢了。
         */
        struct ParsedLoad {
            int token = 0;
            std::vector<DanmakuItem> items;
            QString error;
        };

        QVector<ParsedLoad> m_parsedQueue;
        QMutex m_loadMutex;
        /*
         * 最近一次发起的加载序号。只有序号相等的结果才会被应用 —— 换片/改拉 mock 之后
         * 才回来的旧结果直接丢掉（否则上一部的弹幕会串到这一部上）。
         */
        int m_loadToken = 0;

        /* 上一帧在屏的弹幕 id：用来判断"这条是不是刚上屏"，刚上屏的必须给全量字段 */
        QSet<qint64> m_lastIds;
        /* 用 QFontMetricsF 量一条弹幕的文字宽度（引擎的测量回调，QFont 每次现建 —— 量一次就被引擎缓存） */
        float measureTextWidth(const std::string &text, int fontPixelSize, bool bold) const;

        DanmakuEngine m_engine;

        /*
         * 引擎这一帧的时钟值（= m_engine.clockMs()）和它的墙钟时间戳。
         * GUI 线程在 update() 里**成对**写，渲染线程在 updatePaintNode() 里成对读 ——
         * updatePaintNode 跑在场景图的 sync 阶段，那一刻 GUI 线程是被阻塞的
         * （和 timeMs/anchorMonoMs 那条约定一样，见 DanmakuRendererItem.h）。
         */
        qint64 m_engineClockMs = 0;
        qint64 m_engineClockMonoMs = -1;

        bool m_enabled = true;
        /* activeRenderItems() 在"弹幕开关关掉"时用的临时缓冲（渲染线程单线程访问） */
        mutable std::vector<RenderItem> m_renderScratch;
        qreal m_areaRatio = 1.0;
        qreal m_speedMultiplier = 1.0;
        qreal m_fontSizeScale = 1.0;
        bool m_autoScale = true;
        int m_baseFontSize = 18;
        qreal m_danmakuOpacity = 1.0;
        bool m_fontBold = true;
        int m_outlineType = 0;
        /* 渲染那边用的字体族（量文字宽度时必须一致，否则白框宽窄对不上真实文字） */
        QString m_fontFamily = QStringLiteral("Microsoft YaHei");
        bool m_blockScroll = false;      /* false = 不屏蔽（默认什么都不屏蔽） */
        bool m_blockFixed = false;
        bool m_blockColorful = false;
        bool m_blockAdvanced = false;

        QVariantList m_activeItems;
        int m_frameRevision = 0;
        QVariantMap m_stats;
        /* 见 gpuLayer 的 Q_PROPERTY 说明：置真 = 列表没人看，refreshActiveItems 不再建它 */
        bool m_gpuLayer = false;
    };

}// namespace cicadaqt

#endif// CICADA_QT_DANMAKUCONTROLLER_H
