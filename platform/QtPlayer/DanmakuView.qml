// ===========================================================================
// DanmakuView —— 弹幕的**渲染层**（逻辑全在 C++，见 src/DanmakuController.h）
//
// 和旧的 DanmakuLayer.qml 的关系：
//   * DanmakuLayer.qml 把引擎（轨道分配、碰撞、分段、悬停、缩放）写成了 JS，每帧在 QML 的
//     JS 引擎里跑一遍，还要维护一堆 QML 对象 —— 这是性能上不去的根源。
//   * 这个文件**不做任何弹幕计算**：每帧只做三件事：
//       ① 把播放时间喂给 C++（engine.update(position)）；
//       ② 从 C++ 取回"这一帧要画哪些弹幕、画在哪"（engine.activeItems）；
//       ③ 把结果灌进一个**固定对象池**里的 DanmakuItem（只是赋属性，不建/不删对象）。
//     位置、轨道、淡入淡出、悬停冻结、字号缩放全在 C++ 算完了。
//
// 【为什么用对象池 + Repeater 数字模型，而不是 Repeater 直接遍历 activeItems】
// QML 的 Repeater 遇到模型数组变化会把 delegate **全部重建** —— 每帧重建上百个对象，
// 比 JS 计算还贵。所以这里池子固定：delegate 只在池子扩容时新建，之后每帧只改属性。
//
// 【对外 API 和 DanmakuLayer 一模一样】设置面板（DanmakuSettingPanel）、控制栏的发送框、
// Ctrl+D 的示例弹幕都直接读写 danmaku.area / .speed / .fontBold / setArea() /
// sendDanmaku() … ，所以这一层把同名属性和方法都留着，只把它们转给 C++ ——
// 这样换引擎不用改那三处调用方。
// ===========================================================================
import QtQuick
import QtPlayer

Item {
    id: view

    // -----------------------------------------------------------------------
    // 旧 DanmakuLayer 的兼容 API（调用方：DanmakuSettingPanel / PlayerControlBar / Main）
    // -----------------------------------------------------------------------

    /* 播放器组件（拿播放时间用；位置就是主时钟） */
    property var player: null
    /* 全屏（旧实现里影响自动缩放；现在也留着，接真实弹幕源后可能影响区域） */
    property bool fullscreen: false
    /* 总开关：控制栏底部那颗"弹" */
    property bool danmakuEnabled: true

    /*
     * 渲染方式（A/B 开关，也是回退路径）：
     *   "gpu" —— C++ 的 DanmakuRendererItem：整屏弹幕合并成一个场景图节点，
     *            每条弹幕烤成图集里的一个四边形，**位置由顶点着色器算**。
     *   "qml"（**当前默认**）—— QML 对象池（下面那个 Repeater + DanmakuItem + XAnimator），
     *                    每条弹幕是一棵 QML 对象树。CPU 高一些，但它是**跑通过**的那条路。
     *
     * 【为什么默认必须是 "gpu"】弹幕的 CPU 预算是硬要求（用户指标：1 万条弹幕也不能把
     * CPU 打满）。"qml" 那条路每帧要给每条弹幕写十几个 QML 属性 + 维护对象池，
     * 1 万条直接卡死 —— 它只是对照/回退用的，**不是**日常路径。
     * （2026-09-20 我曾把默认临时拨成 "qml" 想让弹幕先看得见，代价就是 CPU 飙高，
     *   用户实测后明确要求改回 GPU：这里改回来，并把"弹幕上屏就崩"当成 GPU 路径的
     *   必修 bug 去查，而不是靠回退绕过去。）
     */
    property string renderMode: "gpu"

    /* 鼠标位置由画面区那层 MouseArea 喂进来（弹幕层自己挂 HoverHandler 会跟它抢 hover） */
    property bool pointerInside: false
    property point pointerPosition: Qt.point(-1, -1)

    /* ---- 渲染设置（设置面板右页那几项）---- */
    property bool fontBold: true
    /* 0 重墨 / 1 描边 / 2 45°投影 */
    property int outlineType: 0
    /* 弹幕速度是否跟随倍速（面板里的"倍速同步"） */
    property bool speedFollowRate: true
    property string fontFamily: "Microsoft YaHei"

    /* ---- 显示设置（设置面板左页）---- */
    property bool autoScale: true
    property real area: 1.0                 /* 显示区域占比 0.25~1 */
    property real danmakuOpacity: 1.0
    /*
     * 基准字号：**25px**。
     *
     * 引擎/设置的默认是 18，但那是参考实现里 options.fontSize 的旧值；实测在 1080p 窗口下
     * 显出来的字明显偏小（用户反馈"太小了、有些字看不清"）。B 站播放器 1080p 下的弹幕就是
     * 25px 左右，所以这里把应用层的默认值提到 25 —— 引擎和参考那边的默认值不动。
     * 自动缩放（autoScale）仍然会在此基础上按窗口宽/1280 再乘一次（夹在 0.75~1.5）。
     * 设置面板"字号"那一行读的就是这个值，用户改了以面板为准。
     */
    property int baseFontSize: 25
    property int speed: 3                   /* 1~5 档（参考的 5 档倍率） */

    /*
     * 屏蔽表：**置 true = 屏蔽掉这一类**（和设置面板那排图标一致，也是 C++ 引擎的语义）。
     * 默认全 false = 什么都不屏蔽。
     */
    property var filter: ({ "scroll": false, "fixed": false, "colorful": false, "advanced": false })

    /* 速度：5 档倍率表 × （跟随倍速时的播放倍速）。引擎按"档位"走，所以这里给的是倍率。 */
    readonly property real speedMultiplier: {
        var table = [0.5, 0.75, 1.0, 1.5, 2.0]
        var gear = Math.max(1, Math.min(5, Math.round(view.speed)))
        var mult = table[gear - 1]

        if (view.speedFollowRate && view.player !== null && view.player.playbackRate > 0)
            mult = mult * view.player.playbackRate

        return mult
    }

    /* 统计（给诊断浮层/调试用，和旧实现同名） */
    function getStats() {
        return engine.stats
    }

    /* -----------------------------------------------------------------------
     * C++ 引擎（DanmakuController，里面是独立的 CicadaDanmaku 库）
     * ----------------------------------------------------------------------- */

    /* 异步加载的两个出口（引擎那两个信号转出来，见 loadXmlFileAsync 那段说明） */
    signal danmakuLoaded(int token, int count)
    signal danmakuLoadFailed(int token, string reason)

    DanmakuController {
        id: engine

        enabled: view.danmakuEnabled
        areaRatio: view.area
        speedMultiplier: view.speedMultiplier
        autoScale: view.autoScale
        baseFontSize: view.baseFontSize
        danmakuOpacity: view.danmakuOpacity
        fontBold: view.fontBold
        outlineType: view.outlineType
        /* 字体族也要给引擎：它用同一个字体量文字宽度（白框/碰撞都按这个宽算），
           不给的话渲染是真字体、量宽度是估算值，白框就会比文字窄。 */
        fontFamily: view.fontFamily

        /* GPU 模式那条路不经过 activeItems 这张 QML 列表（渲染器直接读引擎），
           告诉控制器别每帧白建它 —— 见 DanmakuController.h 里 gpuLayer 的说明 */
        gpuLayer: view.renderMode === "gpu"

        /* 屏蔽开关（引擎语义：true = 屏蔽） */
        blockScroll: view.filter.scroll === true
        blockFixed: view.filter.fixed === true
        blockColorful: view.filter.colorful === true
        blockAdvanced: view.filter.advanced === true

        /*
         * 每帧结果出来就摆位置。
         *
         * 【GPU 模式下**不调用 present()**】那是旧的逐条渲染路径（对每条弹幕写十几个
         * QML 属性）。GPU 模式下画面由 DanmakuRendererItem 在渲染线程直接读引擎数据，
         * 连这一圈 QVariantList → JS 对象的转换都省掉了 —— 这正是换渲染层要拿的那部分 CPU。
         * 引擎本身照旧每 tick 被 update()，轨位/碰撞/准入都不受影响。
         */
        onFrameChanged: if (view.renderMode !== "gpu") view.present()

        /* 异步加载的结果往外面透一手（Main.qml 认领自己的那次，见那两个 signal 的说明） */
        onDanmakuLoaded: (token, count) => view.danmakuLoaded(token, count)
        onDanmakuLoadFailed: (token, reason) => view.danmakuLoadFailed(token, reason)
    }

    /* 容器尺寸变了要告诉引擎（轨道表是按它算的） */
    onWidthChanged: engine.setContainerSize(width, height)
    onHeightChanged: engine.setContainerSize(width, height)
    Component.onCompleted: engine.setContainerSize(width, height)

    /* 鼠标位置 → 引擎（悬停冻结 + hoveredId） */
    onPointerInsideChanged: pushPointer()
    onPointerPositionChanged: pushPointer()

    function pushPointer() {
        engine.setPointer(pointerPosition.x, pointerPosition.y, pointerInside)

        /*
         * 【暂停时也要能悬停】命中检测是在引擎的 update() 里做的（DanmakuEngine.cpp:1228 的
         * syncHover），而暂停时 stepTimer 停了（view._driving 为假）→ 不补这一次 update，
         * 鼠标移到弹幕上根本没有悬停目标，tip 自然不弹（用户实测）。
         * 引擎在暂停态不推进时钟（引擎的 _paused 分支，见下面 setPaused），所以这一次调用
         * 只是"同步一次悬停"，不会让弹幕动。
         *
         * 【_mediaStalled 时也必须补这一次】那一条判据冻结的是**时钟**（stepDanmaku 不推进
         * _clock），引擎的 update() 本来还在每帧被调用；如果这里只认 _driving，就会出现
         * "缓冲/画面冻结期间鼠标移到弹幕上没有悬停"——那是把功能做没了，不是冻结。
         */
        if (!view._driving || view._mediaStalled)
            engine.update(view.smoothPositionMs)
    }

    /* -----------------------------------------------------------------------
     * 每帧驱动
     * ----------------------------------------------------------------------- */

    /*
     * 平滑时钟（弹幕卡顿的根因就在这）。
     *
     * 【为什么不能直接用 player.position 喂引擎】CicadaPlayerItem 里那个位置是
     * m_positionTimer 每 250ms 轮询一次得到的（setInterval(250) + CoarseTimer），
     * 也就是每秒只变 4 次。直接拿它当弹幕时钟 → 弹幕每 250ms 才挪一格，看着一顿一顿的
     * （视频本身是顺的，只有弹幕卡）。
     *
     * 做法：本地的 _clock 只按帧累加（帧间隔 × 播放倍速往前走），
     * 播放器每报一次新位置就把锚点重新校准一次 —— 于是拖动进度条、暂停恢复、
     * 改倍速都能立刻对齐，平时也不会累积漂移。
     */
    property real smoothPositionMs: 0
    /* 本地秒表：只在 FrameAnimation 回调里累加，暂停时自然停住 */
    property real _clock: 0
    property real _anchorClock: 0
    property real _anchorPositionMs: 0
    /*
     * 上面那个锚点的**时间戳**（单调毫秒，来自 gpuRenderer.monoMs()）。
     * 它和 smoothPositionMs 成对推给渲染器，渲染器靠它把两次锚点之间的位置外推出来
     * （见 DanmakuRendererItem.cpp 的时钟那段）。少了它，外推的起点只能靠"渲染线程
     * 什么时候看到这个锚点"来猜 —— 那个误差就是滚动时"有点抖"的来源。
     *
     * 【初值必须是 -1，不能是 0】渲染器把"负值"当作"还没有时间戳"，此时不外推、
     * x 就按锚点算（见 src/DanmakuRendererItem.cpp 里 `m_anchorMonoMs > 0.0` 那条判据）。
     * 写 0 会被当成"时间戳 = 进程启动那一刻"这个**合法**值：
     *     extraMs = (monoNow − 0) × rate = 进程已经运行的毫秒数
     * 于是 nowSec 比 refSec 大出几十秒，滚动弹幕的 x 被 `x -= speed × (nowSec − refSec)`
     * 一路推到屏幕左侧外 —— 表现就是"引擎收下了、屏幕上一条都看不见"
     * （HANDOVER 4h 记过这个现象：quads=1 但零像素变化）。
     * 只有等 stepTimer 真的 tick 过一次（下面 stepDanmaku 里写这个属性）才会被纠正，
     * 而 tick 在暂停/未驱动时根本不跑，所以这个初值本身就是一个真 bug。
     */
    property real _anchorMonoMs: 0
    /*
     * 时钟微调系数（1.0 = 不调，范围约 0.96~1.04）。
     * 和播放器的误差**只用速率吃掉**，不靠跳变对齐 —— 见 onPositionChanged。
     */
    property real _rateTrim: 1.0
    /* 播放器**上一次报的位置**：用来兜住"换片/循环"这种大跳（见 onPositionChanged） */
    property real _lastPushedPos: -1
    /*
     * 有一次**真 seek** 在等位置更新（由 player.seekRevision 的变化置起，见下面 Connections）。
     * seek 是异步的：调完 seek() 那一刻 player.position 往往还是旧值，所以要等下一次位置回调
     * 再硬对齐 —— 这也正是"不能用位置跳变猜 seek"的原因之一。
     */
    property bool _pendingSeek: false

    /* 播放器在播 + 弹幕开着 = 弹幕该动 */
    readonly property bool _driving: view.player !== null && view.player.playing
                                     && view.danmakuEnabled

    /*
     * 【媒体时钟到底有没有在走 —— 暂停/缓冲时弹幕必须停】
     *
     * 用户实测："点了暂停，画面停住，弹幕还在滚"，而且时好时坏（"很玄学"）。
     * 弹幕时钟原本只看 `player.playing`（也就是框架的状态回调）：只要框架**没有**报
     * PLAYER_PAUSED —— 暂停发生在缓冲/加载中、回调没到、或者之后又被打了一次
     * PLAYER_PLAYING —— 弹幕就会一直滚，而画面是停的。**这不是竞态，是判据太单一。**
     *
     * 这里补一条**不依赖框架状态**的判据：**播放位置多久没变了**。
     *
     * 【为什么必须这样判（两路子代理查证，2026-09-20）】
     * `SuperMediaPlayer.cpp:1638-1646` 起缓冲时只做 `NotifyLoading` + 主时钟 pause +
     * 音频渲染 pause，**`mPlayStatus` 仍然是 PLAYER_PLAYING**（重缓冲永远不会再进
     * PREPARING，全框架唯一的 PREPARING 在 `SMPMessageControllerListener.cpp:385` 首次 Prepare）；
     * 而 `native_cicada_player_def.h` 的 listener **没有 Loading 回调** ⇒ Qt 这侧收不到
     * 任何"正在缓冲"的信号，`playing` 一直是真。画面那边确实冻住
     * （`SuperMediaPlayer.cpp:2164-2168` 缓冲期间不 render）——于是就成了"视频停住、
     * 弹幕照滚"。本地文件几乎不缓冲、网络源一卡就复现，这就是"有时能停有时不能"的来源。
     * 另一条路径：`SMPMessageControllerListener.cpp:445-449` 的 `ProcessPauseMsg()` 首句
     * `if (mPlayStatus != PLAYER_PLAYING) return;` —— 加载中/seek 中按暂停会被**静默丢弃**。
     *
     * 位置随音频帧消费推送（`SuperMediaPlayer.cpp:2156`，约 20–40ms 一次），所以
     * `600ms` 一点没动有 15~30 倍余量，只可能是：真暂停、缓冲、或者画面冻结 —— 这三种
     * 情况下弹幕本来就该停住。判据在**已有的** stepTimer 里更新（不新增定时器、不轮询
     * 新东西）；阈值取 600ms 是为了"暂停后弹幕最多再滚 0.6 秒"。
     *
     * 【本文件里"位置是 m_positionTimer 每 250ms 轮询"的旧注释都已过时】那个轮询已经在
     * `CicadaPlayerItem.cpp:2120-2127` 删掉了，位置现在全走回调。
     */
    property real _lastProgressPos: -1
    property real _lastProgressMono: 0
    /* 媒体时钟停住了（位置不再前进）；渲染器据此冻结，见下面 gpuRenderer 的 clockRunning */
    property bool _mediaStalled: false

    /*
     * 【锚点由播放器的信号**推**过来，但不是"对齐"，是"微调"】
     *
     * player.position 是**滞后**的：框架回调要跨线程投递到 GUI 线程，而本地时钟按帧
     * 外推早就跑到前面去了。如果每次推送都
     *     smoothPositionMs = player.position
     * 就等于每来一次推送把时钟往回拽一次 —— 用户看到的是"弹幕后退一下再继续往前"，
     * 一卡一卡。上一版就是这么写的，所以怎么改都还在退。
     *
     * 现在的做法和音视频同步一样：**位置永不回退，误差分摊到速率上**。
     *   * |误差| > 600 ms：这是真跳转（拖进度条 / 换片 / 长时间卡顿恢复）→ 硬对齐，
     *     这种时候画面本来就在跳，不需要平滑；
     *   * 否则：锚点保持**当前外推值**（位置连续、零跳变），只把 _rateTrim 调一点
     *     （±1%），让本地时钟在接下来几秒里悄悄追上真值。
     *
     * 【★ 为什么锚点不能每来一次回调就重设成 pos（或 max(外推, pos)）★】
     * 锚点一动，"当前这一帧画面上算出来的位置"就跟着动一下 —— 位置回调是 2Hz
     * （框架那个 500ms 的 Notifier），于是滚动时会**每 500ms 出现一个极小的台阶**，
     * 肉眼看就是"有点抖"。锚点不动、只调速率才是真的连续（速度差 1% 看不出来）。
     */
    Connections {
        target: view.player

        function onSeekRevisionChanged() {
            /* 真 seek（用户拖进度条/快捷键）：等下一次位置回调到了再硬对齐 */
            view._pendingSeek = true
        }

        function onPositionChanged() {
            if (view.player === null)
                return

            const pos = view.player.position


            /*
             * 【★ 暂停即冻结：暂停期间完全不用播放器位置去对齐时钟】
             * 这是主流实现的共识：参考 web 端是 `animation-play-state: paused`（冻结 CSS 动画）、
             * Qt 的 danmaQ 暂停 QPropertyAnimation、B 站 DanmakuFlameMaster 停自己的时钟 ——
             * 位置回调只用于"什么时候创建这条弹幕"和"真 seek 后重建"，暂停/恢复之间**不做任何对齐**。
             *
             * 我们之前在这里仍然做 `max(smooth, pos)`：只要框架在暂停/恢复瞬间报来的位置比冻结值大
             * （框架的位置计数在暂停时不一定真停），弹幕时钟就被瞬间往前拽、随后再被速率拉回，
             * 人眼看到的就是"暂停后播放弹幕弹一下"。
             *
             * 【但"冻结期间真的 seek 了"必须例外 —— 这是暂停冻结的配套项】
             * 原来这一条对**所有**非播放状态一律 return，于是：
             *   * 用户暂停着拖进度条：position 回调被丢掉 → 弹幕停在旧位置，要等按下播放才对齐；
             *   * 缓冲（_mediaStalled，播放状态没变）期间框架自己也会推位置（seek 结束那条
             *     SuperMediaPlayer.cpp:2190），一样被丢掉。
             * 判据用 player.seekRevision（CicadaPlayerItem.cpp:899-907，每次 seek() 自增）推来的
             * _pendingSeek，**不改播放状态**：位置与锚点当场对齐并把引擎硬切过去（引擎 applySeek
             * 在暂停态本来就会执行），画面那一帧由 setTimeMs()→update() 触发重画，然后维持冻结。
             * 不用新定时器、也不问"位置跳了多少"（暂停时位置本来就可能抖动）。
             */
            if (!view.player.playing || view._mediaStalled) {
                view._lastPushedPos = pos

                if (view._pendingSeek) {
                    view._pendingSeek = false
                    view._anchorPositionMs = pos
                    view._anchorClock = view._clock
                    view._rateTrim = 1.0
                    view.smoothPositionMs = pos
                    /* 引擎时钟直接挪过去（seekTo 只挪时钟，不额外推进，见 DanmakuController.cpp:903-918） */
                    engine.seekTo(pos)
                }

                return
            }

            /*
             * 【只有"真 seek"才硬对齐（可以回退）】
             * 判据来自 player.seekRevision（CicadaPlayerItem 每次 seek() 自增）——
             * 不再靠"位置跳了多少"去猜。另留 3 秒兜底：换片/循环这种没人调 seek() 的大跳也要对齐。
             */
            const jumped = view._lastPushedPos >= 0
                           && Math.abs(pos - view._lastPushedPos) > 3000
            view._lastPushedPos = pos

            if (view._pendingSeek || jumped) {
                view._pendingSeek = false
                view._anchorPositionMs = pos
                view._anchorClock = view._clock
                view._rateTrim = 1.0
                return
            }

            /*
             * 不是 seek：误差只用速率吃掉。
             *
             * 【阈值 600ms → 1000ms，而且**暂停恢复不再靠它硬对齐**】
             * 暂停时时钟已经在暂停那一刻冻住（见 stepDanmaku 的明确信号），恢复时弹幕时钟
             * 与视频本来就在同一个时间点上，正常不会有大偏差；剩下的几百毫秒偏差用 ±1% 的
             * 速率慢慢吃（或者等下一次真 seek 一次性对齐）比"瞬移一下"舒服得多。
             * 所以这里只把**真正的大跳**（>1000ms，例如换片/循环/框架位置异常）当硬对齐。
             */
            const current = view.smoothAt(view._clock)   /* 外推到**此刻**，保证连续 */

            if (Math.abs(pos - current) > 1000) {
                view._anchorPositionMs = pos
                view._anchorClock = view._clock
                view._rateTrim = 1.0
                return
            }

            /* 锚点 = 当前外推值：位置一格都不动，只调速率 */
            view._anchorPositionMs = current
            view._anchorClock = view._clock

            const wanted = 1.0 + Math.max(-0.01, Math.min(0.01, (pos - current) / 500.0))
            view._rateTrim = view._rateTrim * 0.7 + wanted * 0.3
        }
    }

    /*
     * 锚点外推到**指定时刻**的播放时间（唯一的时钟公式）。
     * _clock 是本地秒表（只按真实流逝时间前进），锚点每来一次位置回调就重设一次，
     * 所以这个函数既用在每帧推进（stepDanmaku），也用在位置回调里"取此刻的值"。
     */
    function smoothAt(clock) {
        var rate = view.player !== null ? Math.max(0.05, view.player.playbackRate) : 1

        return view._anchorPositionMs
               + (clock - view._anchorClock) * 1000 * rate * view._rateTrim
    }

    /*
     * 走一帧弹幕推进（dt = 距上一帧的秒数，monoNow = 这一刻的单调毫秒，见下）。
     *
     * 时钟：本地 _clock 按真实帧间隔累加，播放器每次推位置就微调速率（见上），
     * 所以暂停、拖动、改倍速都能对上，平时也不会累积漂移 —— 而且**不会回退**。
     */
    function stepDanmaku(dt, monoNow) {
        const clockNow = monoNow !== undefined ? monoNow : Date.now()
        const wasStalled = view._mediaStalled

        /*
         * 【先判"媒体时钟有没有在走"】位置一变就刷新时间戳。
         *
         * 【阈值从 600ms 提到 1500ms —— 这是"滚动有点抖"的主因】
         * 框架的位置上报节拍是 **500ms**（SuperMediaPlayer.cpp:323 `mTimerInterval = 500`，
         * :5071 的 NotifyPosition 就在那条周期路径上）。原来用 600ms 判"位置没动 = 停住了"，
         * 余量只有 100ms —— GUI 稍忙、或定时器晚一档就**误判成停帧**：弹幕时钟冻一下、
         * 下一个位置回调再追上去。屏幕上就是"一秒一次的细碎抖动"（用户实测）。
         * 1500ms = 3× 框架节拍，正常播放不会误判；真卡住/暂停由下面两条**明确信号**立刻冻住。
         */
        const mediaPos = view.player !== null ? view.player.position : -1

        if (mediaPos !== view._lastProgressPos) {
            view._lastProgressPos = mediaPos
            view._lastProgressMono = clockNow
        }

        view._mediaStalled = (view._lastProgressMono > 0
                              && (clockNow - view._lastProgressMono) > 1500)

        /*
         * 【2026-09-21 新增：框架明确报了"正在缓冲"就立刻冻结弹幕】
         *
         * 上面那条 1500ms 的判据是**推断**（位置不动了），缓冲刚开始的那段时间里
         * 弹幕还会往前滚一下。现在框架的 LoadingStart/LoadingEnd 已经接到组件上了
         * （player.buffering），这是**明确信号**，比推断更早、更准 ——
         * 用户的要求就是"只要缓冲就弹幕和声音都暂停"，声音那边框架自己会暂停音频渲染，
         * 弹幕这边用这个属性立刻冻结。
         */
        if (view.player !== null && view.player.buffering)
            view._mediaStalled = true

        /*
         * 【明确信号二：暂停/停止 —— 暂停那一刻就冻住，不再等推断】
         *
         * 原来暂停后只能靠上面那条 1500ms(旧 600ms) 的推断，也就是暂停后最多几百毫秒
         * 弹幕还在往前走；等恢复时弹幕时钟已经比视频多走了那一段，于是出现
         * "恢复后弹一下"或"长期错位"（用户实测）。现在和 buffering 一样当成明确信号：
         * 一暂停就冻，时钟停在暂停那一刻 = 和视频同一个时间点，恢复时也就没有偏差要补。
         */
        if (view.player !== null && !view.player.playing)
            view._mediaStalled = true

        /*
         * 【★刚进入冻结的这一 tick 也要把 dt 走完★】
         *
         * 位移是渲染线程按"锚点 + (现在 − 锚点时间戳) × 倍速"**逐帧外推**的：上一次 tick
         * 把锚点设成 C，之后它自己又往前推了约一个 tick（dt）的距离；如果冻结这一帧把
         * 时钟停在 C 上，渲染线程就必须把那一截外推量**往回还** —— 屏幕上就是
         * "暂停的一瞬间往后弹一下"（用户实测的症状）。
         * 所以进入冻结的这唯一一次 tick 仍然把 dt 加上：冻结位置正好等于渲染线程**已经
         * 画出来**的位置，要还的外推量 = 0；之后每次 tick 都真的不再推进（真·冻结）。
         * 一帧的运动量（≤ dt ≈ 33ms）在视觉上不可见，但换来"零回弹"。
         */
        if (!view._mediaStalled || !wasStalled)
            view._clock += dt

        view.smoothPositionMs = view.smoothAt(view._clock)

        /*
         * 【★停 ↔ 走切换的那一刻，把渲染线程的"待外推量"清零★】
         *
         * 位移是渲染线程按"锚点 + (现在 − 锚点时间戳) × 倍速"逐帧外推的
         * （DanmakuRendererItem.cpp:681-699）。冻结之前它已经按外推把画面往前推了一点
         * （最多一个 tick 的运动量，旧锚点更久时就是几十像素），下一帧拿到"位置没变"的锚点
         * 就必须**往回弹**那一段 —— 这正是用户看到的"暂停时弹一下"。
         * 做法：在切换的那一次 tick 里，把锚点取成**当前外推值**（而不是切换前的旧值），
         * 渲染线程要还的外推量就成了 0；_anchorMonoMs 在下面统一刷成这一刻。
         */
        if (view._mediaStalled !== wasStalled) {
            view._anchorPositionMs = view.smoothPositionMs
            view._anchorClock = view._clock
            view._rateTrim = 1.0
        }

        view._anchorMonoMs = monoNow !== undefined ? monoNow : Date.now()

        engine.update(view.smoothPositionMs)
        /* present() 由 engine.onFrameChanged 触发，这里不用再调 */
    }

    /*
     * 驱动方式：**30Hz 的 Timer**（不再是 FrameAnimation / vsync）。
     *
     * 为什么现在可以用 Timer 了：弹幕的**位移**已经交给渲染线程的 XAnimator 逐帧插值
     * （见 DanmakuItem.qml 里 startMotion 那段说明），这个 Timer 只负责"逻辑"——
     * 喂时钟给引擎、入场/退场、悬停、透明度、以及每 33ms 把动画重新对齐到引擎的位置。
     * 所以 tick 率不再影响运动是否连贯：30Hz 的逻辑 + 渲染线程 60fps 的插值 = 又顺又省。
     *
     * 屏上一条弹幕都没有时降到 100ms 慢查：Timer 不逼窗口重绘，空屏时窗口只在视频换帧时重画。
     *
     * 【"屏上有没有弹幕"这个判据在 GPU 模式下必须换一个来源】见下面 _onscreenCount：
     * 原来用的 _presentedCount 只有 present() 会写，而 present() 是 QML 模式专用的，
     * GPU 模式下它恒为 0 —— 也就是这个 Timer 一直按"空屏"的 100ms 在跑（10Hz），
     * 30Hz 那条分支从来没生效过。
     */
    property int updateFps: 30
    /* 上一个 tick 的单调时间戳（单位 ms；0 = 还没 tick 过）。见下面 onTriggered */
    property real _lastTickMono: 0

    /*
     * 【屏上到底有没有弹幕】—— 决定这个 Timer 用 30Hz 还是 100ms。
     *
     * GPU 模式：读控制器统计里的 activeCount。它只在**真的变了**的时候才发信号
     *   （DanmakuController::refreshActiveItems 里 `statsMap != m_stats` 那道判断），
     *   所以拿它当判据不会每帧把整张弹幕列表转成 JS —— 那正是 GPU 模式要省掉的开销。
     * QML 模式：还是 _presentedCount（present() 每帧写，就是这一帧摆出去几条）。
     *
     * 【为什么必须换】_presentedCount 只有 present() 会写，而 present() 在 GPU 模式下
     * **根本不会被调用**（见上面 onFrameChanged：画面由 DanmakuRendererItem 在渲染线程
     * 直接读引擎数据，根本不经过 QML 那张列表），于是它永远是 0。判据恒为假 →
     * 引擎和渲染锚点都只有 10Hz：
     *   * 入场/退场、悬停命中、_rateTrim 收敛都慢三倍（悬停 tip 要晚最多 100ms 才认出目标）；
     *   * 更要命的是**锚点太旧**：渲染器两次锚点之间靠外推补位置（见
     *     DanmakuRendererItem.cpp 里时钟那段的说明），暂停冻结那一帧要把这截外推量
     *     还回去 —— 100ms（GUI 再卡一下更久）在屏幕上就是几十像素的"往后弹一下"。
     */
    readonly property int _onscreenCount: view.renderMode === "gpu"
                                          ? ((engine.stats !== undefined && engine.stats !== null
                                              && engine.stats.activeCount !== undefined)
                                             ? engine.stats.activeCount : 0)
                                          : view._presentedCount

    Timer {
        id: stepTimer

        interval: view._onscreenCount > 0
                  ? Math.max(8, Math.round(1000 / Math.max(1, view.updateFps)))
                  : 100
        repeat: true
        /*
         * 【关于"tick 间隔稳不稳"—— 这里**不能**写 timerType（会直接编译不过 QML）】
         *
         * 已经试过并回退：`timerType: Qt.PreciseTimer`。QML 的 Timer **没有**这个属性
         * （它不是 QTimer，而是 QQmlTimer = QPauseAnimationJob 挂在**动画时钟**上，
         * 源码 qqmltimer.cpp 里根本没有 timerType），写了会让 DanmakuView 整个
         * 组件加载失败：`Cannot assign to non-existent property "timerType"`，
         * 于是一开播放器就报 PlayerView/ DanmakuView unavailable（2026-09-23 实测）。
         *
         * 而且它**本来也不需要**：位置不是靠"定时器准不准"堆出来的，是每个 tick 用
         * 实测的 dt 积分出来的（见 stepDanmaku(dt, mono)）。tick 早晚几个毫秒只会让
         * 积分步长变大变小，出来的 smoothPositionMs 仍然是单调、按真实时间走的；
         * 真正会造成"一抖一抖"的是**误判停帧**（已把阈值提到 1500ms）和下面
         * 冻结/解冻时的重新取锚（已改成锚点=当前外推值）。所以这里什么都不用配。
         */
        /* 【回退】时钟重新交回 QML 每帧（C++ 自走时钟那版在退出路径上会崩，已撤） */
        running: view._driving
        onRunningChanged: {
            /*
             * 播放/暂停状态也喂给引擎：暂停时 update() 只同步悬停、不推进时钟
             * （引擎的 _paused 分支）—— 这是"暂停后鼠标移到弹幕上 tip 也能弹出"的前提，
             * 见 pushPointer() 里那次补发的 update()。
             */
            engine.setPaused(!running)

            /*
             * 【诊断，一行一次，只在暂停/恢复时打】
             * 用户实测"有时视频暂停了弹幕还在动，有时又能停"。弹幕时钟只有两个开关：
             * 这个 running（= view._driving）和引擎的 _paused。要分清是
             *   * 播放器状态**根本没变成暂停**（框架还在报 Playing：缓冲/卡住、
             *     或者暂停发生在状态回调之前），还是
             *   * 渲染器**没跟着停**（clockRunning 那条绑定出问题），
             * 就在状态切换这一刻把四个判据打出来。正常播放时一句都不会有。
             */
            console.log("[danmaku] 时钟 driving=" + running
                        + " status=" + (view.player !== null ? view.player.status : -1)
                        + " playing=" + (view.player !== null ? view.player.playing : false)
                        + " enabled=" + view.danmakuEnabled)

            /* 暂停/停播：把所有动画一起停住 —— 否则视频暂停了弹幕还在飘。
               这里用 stop 而不是 pause：恢复后第一个 tick 会按引擎的位置重新起动画，
               而引擎的时钟（暂停时）没走，所以接得上、不会跳。 */
            if (!running)
                view.stopAllMotion()

            view._lastTickMono = 0
        }
        onTriggered: {
            /*
             * 用真实流逝时间算 dt（Timer 会被事件循环挤后，interval 只是名义值）。
             *
             * 【时间源用渲染器那条单调时钟，不用 Date.now()】锚点的时间戳就是同一个
             * 函数的返回值（见 stepDanmaku），渲染线程按"现在 − 时间戳"外推 ——
             * 两边必须是同一条时钟，否则时间差里会混进别的误差；
             * 而且它是**单调**的（不受系统时间调整影响，分辨率也更高）。
             */
            /*
             * 【时间源不能依赖渲染器存在】
             * `gpuRenderer.monoMs()` 是渲染器那条单调时钟（它同时用来算"现在 − 锚点"），
             * 正常情况下两边同源最好。但用户实测的日志里出现过
             *     DanmakuView.qml:431 TypeError: Cannot call method 'monoMs' of null
             * 一秒三十条、连刷十九秒：**这一步抛异常 → stepDanmaku() 整个不执行 →
             * engine.update() 永远不被调用 → 一条弹幕都不会上屏**（而且异常日志本身
             * 就把 GUI 线程拖住，界面点起来像卡死）。所以这里必须兜住：
             * 渲染器在就用它的时钟（精确同源），不在就退到墙上时钟 —— 只影响外推精度，
             * 不影响"引擎时钟要不要走"这件更根本的事。
             */
            /* 【不再每帧调 C++ 的 monoMs()】外推已经去掉，这里只需要一个"流逝时间"，
               用 QML 内建的墙上时钟即可 —— 每帧少一次跨语言调用，也彻底不碰渲染器对象
               （那个对象在异常时序下是崩溃来源之一）。 */
            var mono = Date.now()
            var dt = view._lastTickMono > 0 ? Math.min((mono - view._lastTickMono) / 1000, 0.25) : 0
            view._lastTickMono = mono
            view.stepDanmaku(dt, mono)
        }
    }

    /* -----------------------------------------------------------------------
     * 对象池 + 渲染
     * ----------------------------------------------------------------------- */

    /* 池子大小：按需翻倍扩容（上限就是引擎那边的 maxRenderCount = 2000） */
    property int poolSize: 64

    /* 这一帧实际摆出去几条（= C++ 给的 activeItems 数量）。调试/自检用。 */
    readonly property int presentedCount: _presentedCount
    property int _presentedCount: 0
    /* 上一帧摆出去几条（收尾关池子项用，见 present()） */
    property int _lastPresentedCount: 0

    /*
     * 手动走一帧（外面调试/自检用；正常运行由上面的 FrameAnimation 驱动）。
     * 注意它不动播放器时间，用的是当前位置 —— 所以暂停时调它就是"按当前时间重画一次"。
     */
    function tick() {
        engine.update(view.player !== null ? view.player.position : 0)
        present()
    }

    /* =======================================================================
     * 弹幕渲染（renderMode === "gpu"，默认）
     *
     * 一个 Item 承载整屏弹幕：内部是**一个几何节点**（根节点兼管整层不透明度），
     * 每条弹幕 4 个顶点，画的是纹理图集里属于它的那一块
     * （见 src/DanmakuRendererItem.cpp）。
     *   * 文字：一次性烤进纹理图集，运行时只采样，没有任何排版；
     *   * 位置：`x = x_ref − speed × (now − t_ref)`，在 sync 阶段算好后写进顶点；
     *   * 没有 QML 对象、没有属性绑定、没有 XAnimator，整层一次绘制。
     *
     * （曾经用过"自定义 QSGMaterial + 自己烘焙 QSB、顶点着色器里算位置"的写法，
     *   Qt 的 D3D11 后端运行时取不到那份 HLSL，管线建不起来、一条都画不出来；
     *   也试过"每条弹幕一个 QSGImageNode"，能画但滚动发涩、CPU 高。
     *   两种情况都写在 src/DanmakuRendererItem.h 文件头，别再走回去。）
     *
     * timeMs / rate / clockRunning 三个一起构成"弹幕时钟"：
     *   锚点（timeMs）由这里 30Hz 喂，渲染器在两次锚点之间按 rate 外推 ——
     *   于是 GUI 30Hz、画面 60fps，而且和引擎算 x_ref 的时间轴完全一致。
     * ======================================================================= */
    DanmakuRendererItem {
        id: gpuRenderer

        anchors.fill: parent
        z: 1
        visible: view.renderMode === "gpu"

        /* 数据源就是上面那个 C++ 引擎（渲染器直接读它的 std::vector，不经 JS） */
        controller: engine

        /*
         * 【位置：直接绑播放器的位置（框架推位置时才变，2Hz）】
         *
         * 以前是 `timeMs: view.smoothPositionMs` —— 那是 QML 的本地时钟，**每帧都变**，
         * 于是每帧都要写一次这个 C++ 属性（崩溃栈上那条 `QML 写属性 → QV4 重入`）。
         * 现在只写"框架报来的真实位置"，两次之间由渲染线程按锚点外推 ——
         * 平滑照旧，但 QML 不再每帧碰渲染器。
         */
        timeMs: (view.player !== null) ? view.player.position : 0
        /* 和 timeMs **成对**推过去的时间戳（同一条单调时钟，见 stepDanmaku 的说明） */
        /*
         * 【这一行不能删】anchorMonoMs 确实不再从 QML 写了（C++ 的 setTimeMs() 自己打
         * 时间戳），但 `clockRunning` **必须**继续写：C++ 那侧的默认值是 **false**
         * （DanmakuRendererItem.h 里 `bool m_clockRunning = false;`，默认不外推），
         * 全靠这里每帧把真实状态推过去。上面几段"不再从 QML 写 clockRunning、属性默认 true"
         * 的说法是旧的，照着删会让弹幕**永远不动**。
         * `_mediaStalled` 见上面的说明：位置不再前进（暂停/缓冲/画面冻结）时一起冻结。
         */
        clockRunning: view._driving && !view._mediaStalled
        /*
         * 外推速率要和弹幕时钟**完全一致**：那边是 rate × _rateTrim（见上），
         * 这里少乘一个 _rateTrim 的话，两次 30Hz 喂时钟之间会差 1~4%（约 1 ms 的台阶），
         * 虽然很小，但没理由留着。
         */
        rate: ((view.player !== null && view.player.playbackRate > 0)
               ? view.player.playbackRate : 1) * view._rateTrim
        /*
         * 【不再从 QML 写 clockRunning —— 这一写就是崩溃点】
         *
         * 崩溃栈（用户 13:51，64 层）：
         *     QV4（QML 绑定求值）→ qt_static_metacall → setClockRunning → clockRunningChanged → QV4 崩
         * 也就是"第一帧开始播放时 `_driving` 变真 → QML 写这个属性"那一下炸的。
         * 而**去掉外推之后**（extraMs = 0），位置完全由 `timeMs` 决定：
         * 暂停时 `timeMs` 不再更新 → 弹幕自然定住，根本不需要这个属性。
         * 所以直接不写它（属性默认 true），把这条"QML 写 → QV4 重入"的链彻底切断。
         */

        /* 容器尺寸要和喂给引擎的完全一致，否则按容器宽算出来的滚动速度会对不上 */
        containerWidth: view.width
        containerHeight: view.height
    }

    /*
     * 【弹幕可见性看门狗 —— 只在 GPU 模式下跑，2 秒一次】
     *
     * 为什么需要它：GPU 模式是**唯一**在画的路径（present() 与对象池只在 renderMode==="qml"
     * 时工作，见上面 onFrameChanged / Repeater model），而 `renderMode` 界面上没有切换入口。
     * 于是只要 GPU 链（图集纹理 / UV / 几何）任何一环断了，用户看到的就是"一张弹幕都没有"，
     * 而且没有任何提示、也没有回退。要跑起来要么看得见弹幕、要么留下一行能定位的日志。
     *
     * 判据只用引擎自报的两个数（`engine.stats`）和渲染器自报的 quadCount：
     *   * 载入 0 条            → 数据链的问题（日志里 `[danmaku] …` 那几行会说明是哪种）；
     *   * 在屏有、quad = 0     → GPU 链断了 → 记一行 + 回退到 qml 渲染路径（先让弹幕看得见）；
     *   * 在屏有、quad > 0     → GPU 在画（那"看不见"就是位置/纹理问题，C++ 侧的日志会打）。
     * 正常播放时两个条件都不成立，这个 Timer 只做几次整数比较，不碰任何热路径。
     */
    Timer {
        id: visibilityWatchdog

        property int stuckTicks: 0

        interval: 2000
        repeat: true
        /*
         * 【暂时停用】它是我为"卡住没有日志"加的运行期 JS 定时器（每 2 秒读 engine.stats 和
         * gpuRenderer.quadCount）。现在的故障是**崩溃**不是卡死，而崩溃栈整段落在 QML 的
         * JS 引擎里，所以先把这个"我自己加进弹幕路径的周期 JS"关掉，排除它之后再谈卡死。
         */
        running: false

        /*
         * 开机自检一行：本组件声明在 DanmakuRendererItem **之后**，所以这里读到的
         * 就是"那个对象到底建出来没有"。用户日志里 `gpuRenderer` 一直是 null
         * （DanmakuView.qml 里 560 条 `monoMs of null`），但没有任何 QML 创建失败的报错 ——
         * 这一行能一句话把它钉死（是"从来没建出来"还是"后来被销毁了"，配合下面
         * 看门狗那行 warn 的时机一起看）。
         */
        Component.onCompleted: console.warn("[danmaku] 渲染器自检："
                                            + (gpuRenderer !== null
                                               ? "GPU item 已创建（renderMode=" + view.renderMode + "）"
                                               : "gpuRenderer 为 null —— GPU 路径不可用，稍后自动回退 qml"))

        onTriggered: {
            var st = engine.stats
            var loaded = (st !== undefined && st !== null && st.itemCount !== undefined) ? st.itemCount : 0
            var active = (st !== undefined && st !== null && st.activeCount !== undefined) ? st.activeCount : 0

            if (loaded <= 0)
                return

            if (active > 0 && (gpuRenderer === null || gpuRenderer.quadCount === 0)) {
                stuckTicks += 1

                if (stuckTicks >= 2) {
                    stuckTicks = 0
                    console.warn("[danmaku] GPU 渲染路径画不出东西（渲染器="
                                 + (gpuRenderer === null ? "null（对象没建出来）" : "quad=0")
                                 + "、载入 " + loaded + " 条、在屏 " + active
                                 + " 条）→ 回退到 qml 渲染路径")
                    view.renderMode = "qml"
                }
            } else {
                stuckTicks = 0
            }
        }
    }

    Repeater {
        id: pool
        /*
         * GPU 模式下 model 是 0 —— 一个 QML 弹幕对象都不创建（这正是省下来的那部分）。
         * 切成 "qml" 才会按 poolSize 建出旧的对象池。
         */
        model: view.renderMode === "gpu" ? 0 : view.poolSize

        delegate: DanmakuItem {
            /* 池子里的项默认不显示，present() 会按引擎结果打开前 N 个 */
            visible: false
        }
    }

    /* =======================================================================
     * 弹幕悬停气泡（tip）+ 本人弹幕的"撤回"
     *
     * 【参考就是这一份】front/hilihili-player/player/src/component/dialog：
     *   DOM  —— index.ts:37-65（initDmTip 的模板）
     *   行为 —— index.ts:67-124（showDmTip / hideDmTip / inTip）
     *   CSS  —— index.scss:1-123（尺寸、图标位置、tipsWrap / tipsWrapReturn）
     *   图标 —— component/icons/index.ts:769-800（TipSvgm / TipLike / TipCopy / TipRecall / TipBack，
     *            参考里是**内联 SVG 字符串**，已按原文逐字落成
     *            assets/images/dm/dm-tip-{svgm,like,copy,recall,back}.svg）
     *
     * 结构（参考的模板）：
     *   <div class="player-dm-tip player-showB">        162×48、translateX(-50%)、margin-top 6px
     *     <div class="player-dm-tip-svgm">气泡 SVG</div>   162×48，标了 100%×100%
     *     <div class="player-dm-tip-svgl">…</div>          参考里它的 opacity **永远是 0**（没有任何规则
     *                                                     把它置 1），所以这里不画那一层
     *     <div class="player-dm-tip-like">点赞图标</div>    23×23，left 20 / top 16
     *     <div class="player-dm-tip-like-num">0</div>      left 40 / top 13、#fff
     *     <div class="player-dm-tip-copy">复制图标</div>    left 70
     *     <div class="player-dm-tip-recall">撤回图标</div>   left 120，**只有本人（player-master）显示**
     *     <div class="player-dm-tip-back">警告图标</div>     left 120，本人时隐藏（与 recall 互斥）
     *   </div>
     *
     * 行为（参考的时序，一个数没改）：
     *   * 悬停命中 → **300ms** 后弹出（index.ts:68 的 rafTimeout(..., 300)）
     *   * 弹出后 **2000ms** 自动收起（index.ts:96-103 的 inTimer）
     *   * 指针移到 tip 上 → 这 2000ms 重新计（index.ts:119-124 的 inTip）
     *   * 指针离开 tip → 200ms 后收起（index.ts:109-117 的 outTimer）
     *     （"从弹幕挪到 tip 上点撤回"在参考里就是这么成立的：离开弹幕只启动 200ms 收起，
     *       tip 自己的 mouseenter 把它取消掉）
     *   * 位置：left = clamp(鼠标 x, 81, 容器宽 − 81)（index.ts:76-82；81 = 162/2），
     *           top  = 该条弹幕底边 + CSS 的 margin-top:6px（index.ts:83）
     *   * 动画 = tipsWrap / tipsWrapReturn（index.scss:90-122）：
     *       弹出 0.25s：opacity 0→1、scale 0.6 →(50%)→ 1.05 → 1
     *       收起 0.25s：scale 1 →(20%)→ 1.05 → 0.6、opacity 1→0
     *
     * 【★ tip 里所有 MouseArea 都不写 hoverEnabled】本项目 hover 是"最上层接受者独占"
     * （docs/QML-POPUP-HOVER-PITFALLS.md）：一写就会把画面区那层 hover 抢走，引擎随即收到
     * inside=false → 悬停目标被释放 → tip 当场消失，撤回根本点不到。
     * 所以"指针在不在 tip 上"用**被动判**：画面区喂进来的 pointerPosition + mapToItem。
     * ======================================================================= */
    Item {
        id: danmakuTip

        /* 引擎给的"悬停中那一条"：{ id, text, isSelf, x, y, w, h }；没有目标时是空 map */
        readonly property var hovered: engine.hoveredItem
        readonly property bool hasItem: hovered !== undefined && hovered !== null
                                        && hovered.id !== undefined && hovered.id !== 0

        /*
         * 弹出那一刻定下来的那一份（位置/文字/本人标记）。
         * 参考也是一次性定位（index.ts:76-83 只设一次 left/top）；这里快照还多一个作用：
         * 要挪鼠标到 tip 上点"撤回"时，引擎的悬停目标已经没了 —— 不快照就点不到。
         */
        property var latched: null
        property real tipCenterX: 0
        property real tipTop: 0
        property bool shown: false

        readonly property bool isSelf: latched !== null && latched.isSelf === true

        /* 参考 .player-dm-tip：162×48、z-index 999999、transform:translateX(-50%) */
        width: 162
        height: 48
        z: 2
        x: tipCenterX - width / 2
        y: tipTop + 6                      /* CSS 的 margin-top:6px */
        visible: shown || tipHide.running
        /* 动画起点照 CSS 的初始态：opacity 0、scale(0.6)、transform-origin:center */
        opacity: 0
        scale: 0.6
        transformOrigin: Item.Center

        /*
         * 指针在不在 tip 上（对应参考的 mouseenter / mouseleave）。
         * **被动判**：画面区喂进来的 pointerPosition 就是本层坐标，mapToItem 到这一块再判；
         * 不能用 MouseArea 的 hoverEnabled（见文件头那段）。
         */
        readonly property bool pointerOverTip: {
            if (!shown || !view.pointerInside)
                return false

            var p = view.mapToItem(danmakuTip, view.pointerPosition.x, view.pointerPosition.y)

            return p.x >= 0 && p.x <= width && p.y >= 0 && p.y <= height
        }

        /* 悬停目标变了：换了一条 → 300ms 后弹出；目标没了 → 收起（指针在 tip 上时不算，见下） */
        onHasItemChanged: {
            if (hasItem) {
                if (latched === null || latched.id !== hovered.id) {
                    hideDelay.stop()
                    showDelay.restart()
                }
            } else if (!pointerOverTip) {
                hideDelay.restart()
            }
        }

        onPointerOverTipChanged: {
            if (pointerOverTip) {
                /* 参考 inTip（index.ts:119-124）：取消收起 + 重新计那 2000ms */
                hideDelay.stop()

                if (shown)
                    autoHide.restart()
            } else if (shown) {
                /* 参考 mouseleave（index.ts:88-92）→ hideDmTip → 200ms 后收起 */
                hideDelay.restart()
            }
        }

        onShownChanged: if (shown) tipShow.restart()

        /* 300ms：悬停命中之后才弹（index.ts:68 的 rafTimeout(..., 300)） */
        Timer {
            id: showDelay

            interval: 300
            onTriggered: {
                if (!danmakuTip.hasItem)
                    return

                danmakuTip.latched = danmakuTip.hovered
                /* index.ts:76-82：left 夹到 [81, 容器宽 − 81]（81 = 162/2，保证 tip 不出界） */
                danmakuTip.tipCenterX = Math.max(81, Math.min(view.width - 81,
                                                              view.pointerPosition.x))
                /* index.ts:83：top = 该条弹幕底边（相对弹幕层）；+6 由 CSS 的 margin-top 给 */
                danmakuTip.tipTop = danmakuTip.hovered.y + danmakuTip.hovered.h
                danmakuTip.shown = true
                autoHide.restart()
            }
        }

        /* 2000ms：弹出后自动收起（index.ts:96-103 的 inTimer） */
        Timer {
            id: autoHide

            interval: 2000
            onTriggered: hideDelay.restart()
        }

        /* 200ms：收起的延迟（index.ts:112-116 的 outTimer） */
        Timer {
            id: hideDelay

            interval: 200
            onTriggered: {
                danmakuTip.shown = false
                danmakuTip.latched = null
                tipHide.restart()
            }
        }

        /* tipsWrap（index.scss:90-105）：0% opacity 0 + scale .6 → 50% scale 1.05 → 100% opacity 1 + scale 1 */
        ParallelAnimation {
            id: tipShow

            NumberAnimation {
                target: danmakuTip
                property: "opacity"
                from: 0
                to: 1
                duration: 250
                easing.type: Easing.OutCubic
            }

            SequentialAnimation {
                NumberAnimation {
                    target: danmakuTip
                    property: "scale"
                    from: 0.6
                    to: 1.05
                    duration: 125
                    easing.type: Easing.OutCubic
                }

                NumberAnimation {
                    target: danmakuTip
                    property: "scale"
                    to: 1
                    duration: 125
                    easing.type: Easing.OutCubic
                }
            }
        }

        /* tipsWrapReturn（index.scss:107-122）：0% scale 1 → 20% scale 1.05 → 100% scale 0.6 + opacity 0 */
        ParallelAnimation {
            id: tipHide

            NumberAnimation {
                target: danmakuTip
                property: "opacity"
                from: 1
                to: 0
                duration: 250
                easing.type: Easing.OutCubic
            }

            SequentialAnimation {
                NumberAnimation {
                    target: danmakuTip
                    property: "scale"
                    from: 1
                    to: 1.05
                    duration: 50
                    easing.type: Easing.OutCubic
                }

                NumberAnimation {
                    target: danmakuTip
                    property: "scale"
                    to: 0.6
                    duration: 200
                    easing.type: Easing.OutCubic
                }
            }
        }

        /* ---- .player-dm-tip-svgm：气泡本体（162×48，icons/index.ts:769-773） ---- */
        Image {
            anchors.fill: parent
            source: "assets/images/dm/dm-tip-svgm.svg"
            sourceSize: Qt.size(danmakuTip.width, danmakuTip.height)
            smooth: true
        }

        /*
         * tip 把点击吞掉：参考里 tip 是 DOM 中 video 之外的兄弟节点，点它不会切播放/暂停；
         * 本项目画面区那层 MouseArea 铺满整块，不挡的话点气泡会切播放/暂停 —— 与参考不一致。
         * 子项里只有它（以及下面四个图标里的）不写 hoverEnabled（原因见文件头那段）。
         * 声明在四个图标**之前**，否则会把图标的点击也吞掉。
         */
        MouseArea {
            anchors.fill: parent
        }

        /* ---- 点赞图标 + 数字（CSS：图标 left 20 / top 16；数字 left 40 / top 13） ---- */
        Image {
            x: 20
            y: 16
            width: 23
            height: 23
            source: "assets/images/dm/dm-tip-like.svg"
            sourceSize: Qt.size(23, 23)
            smooth: true

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
            }
        }

        Text {
            x: 40
            y: 13
            /* 参考里这个数字没写 font-size（继承容器），这里跟播放器其它小字一致用 12 */
            text: "0"
            color: "#fff"
            font.pixelSize: 12
        }

        /* ---- 复制图标（left 70） ---- */
        Image {
            x: 70
            y: 16
            width: 23
            height: 23
            source: "assets/images/dm/dm-tip-copy.svg"
            sourceSize: Qt.size(23, 23)
            smooth: true

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
            }
        }

        /* ---- 撤回图标（left 120）：参考 CSS `.player-master .player-dm-tip-recall{display:block}` ---- */
        Image {
            x: 120
            y: 16
            width: 23
            height: 23
            visible: danmakuTip.isSelf
            source: "assets/images/dm/dm-tip-recall.svg"
            sourceSize: Qt.size(23, 23)
            smooth: true

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                /* 撤回：转发给引擎（引擎那边 = 参考的 removeDanmaku(renderId)） */
                onClicked: if (danmakuTip.latched !== null)
                               engine.recallDanmaku(danmakuTip.latched.id)
            }
        }

        /* ---- 非本人时显示的那颗图标（同一位置，与撤回互斥；参考 CSS 同上） ---- */
        Image {
            x: 120
            y: 16
            width: 23
            height: 23
            visible: !danmakuTip.isSelf
            source: "assets/images/dm/dm-tip-back.svg"
            sourceSize: Qt.size(23, 23)
            smooth: true

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
            }
        }
    }

    /*
     * 每个池子槽位"当前绑的是哪条弹幕"：由 id + 字号 + 渲染设置版本 拼成。
     * 只有这个 key 变了才重写文字/尺寸/字体/边框这些**会触发排版**的属性；
     * 同一条弹幕的后续帧只改 x / y（纯位移，不动排版）和透明度、层级。
     *
     * 【为什么必须这样】以前每帧给每条弹幕写十六个属性，其中 text / width / height /
     * fontSize / textAdvanceWidth / fontFamily 都会让 QML 的 Text **重新排版** ——
     * 而一条弹幕里有 2~6 个 Text（本体 + 阴影，描边模式再加四份），
     * 十几条弹幕就是每秒几千次文本重排，CPU 全烧在这里（"十几条就 80%"就是这么来的），
     * 弹幕上千条时更是按条数线性爆炸。位置改动只是节点的变换矩阵变化，不重排，
     * 所以每帧只动 x/y 是安全的，而且每帧成本从 O(条数×16) 降到 O(条数×2)。
     */
    property var _slotKeys: []

    /* 渲染设置（字体/粗体/描边）变一次就 +1 —— 让在屏弹幕重新绑一遍样式 */
    property int _styleRevision: 0
    onFontFamilyChanged: view._styleRevision++
    onFontBoldChanged: view._styleRevision++
    onOutlineTypeChanged: view._styleRevision++

    /*
     * 把池子里所有条目的位移动画停掉（暂停、关弹幕、整层收起时用）。
     * 循环整个池子是有意的：这里只在"状态切换"时跑一次，不在每帧路径上。
     */
    function stopAllMotion() {
        for (var i = 0; i < view.poolSize; ++i) {
            var it = pool.itemAt(i)

            if (it !== null)
                it.stopMotion()
        }
    }

    /*
     * 把引擎这一帧的结果灌进池子。
     *
     * 每帧只做"位移 + 透明度 + 层级"；文字/尺寸/字体只在**这一槽换了弹幕**
     * （或字号、渲染设置变了）时才写一次 —— 见上面 _slotKeys 那段说明。
     */
    function present() {
        var items = engine.activeItems
        var count = items.length

        view._presentedCount = count

        if (count > view.poolSize)
            view.poolSize = Math.min(2000, Math.max(count, view.poolSize * 2))

        /*
         * 【只循环这一帧真要画的条数】原来是 0 → poolSize（池子固定 64）全跑一遍：
         * 屏幕上只有 3 条弹幕时，每帧也要做 64 × 十几个属性赋值 —— 60fps 下是每秒几万次
         * QML 属性写入，白吃掉好几毫秒。现在按实际条数循环，最后再把"上一帧画过、
         * 这一帧不用了"的那几个池子项关掉（只在条数变少时才走那一段）。
         */
        for (var i = 0; i < count; ++i) {
            var item = pool.itemAt(i)

            if (item === null)
                continue

            var data = items[i]

            /*
             * 【为什么每个字段都带兜底】这段循环是"一帧的全部渲染工作"，一旦某个字段是
             * undefined，QML 会在赋值那行**抛异常并中断整个函数** —— 后果不是"这一条画错"，
             * 而是"这一帧后面的所有弹幕都不画、也不显示"（踩过一次：C++ 那边漏传
             * textAdvance，表现就是一条弹幕都看不到）。所以这里宁可给默认值。
             */
            var fontSize = data.fontSize !== undefined ? data.fontSize : 18
            var id = data.id !== undefined ? data.id : -1
            var key = id + ":" + fontSize + ":" + view._styleRevision
            var rebound = view._slotKeys[i] !== key

            /*
             * 每帧的部分（**不触发任何文本排版**）：
             *   y      —— 引擎给的是"整盒左上角"，直接摆
             *   x      —— 滚动弹幕交给渲染线程的动画（见下），固定弹幕直接摆
             *   alpha  —— 固定弹幕的淡入淡出
             *   z/opacity —— 悬停的那条提到最上层并强制不透明
             */
            var isScroll = (data.type === "scroll")
            var durationMs = data.durationMs !== undefined ? data.durationMs : 0
            var progress = data.progress !== undefined ? data.progress : 0
            var rate = (view.player !== null && view.player.playbackRate > 0)
                       ? view.player.playbackRate : 1

            item.y = data.y !== undefined ? data.y : 0
            item.contentOpacity = data.alpha !== undefined ? data.alpha : 1
            item.z = data.hovered === true ? 1000 : (data.type !== "scroll" ? 2 : 1)
            item.opacity = data.hovered === true ? 1 : item.contentOpacity
            item.visible = true

            if (isScroll) {
                /*
                 * 滚动弹幕：**一段动画跑到底，不再每个 tick 重起**。
                 *
                 * 【踩过的坑】之前每个 tick（30Hz）都"停掉 → 设 x → 重新 start"，两次 tick 之间
                 * 只有 ~33ms 的运动段，而 QML 的 Timer 触发时刻本身有 ±10ms 抖动 ——
                 * 每段的时长大不一样，速度就忽快忽慢，看着"不太流畅"。
                 *
                 * 现在只在**真的需要重新对齐**时才重起动画：
                 *   * rebound            —— 这个槽位换了另一条弹幕；
                 *   * !motionRunning()   —— 动画没在跑（刚恢复播放、刚取消悬停、刚入场）；
                 *   * 位置差 > 4px        —— 引擎认为该在的位置和显示对不上（拖动进度条、
                 *                          改倍速、窗口尺寸变化之后引擎重算了全部位置）。
                 * 其余 tick 什么都不做，动画在渲染线程上一路插值到底 —— 恒定速度，才是顺的。
                 * 悬停时只冻结、不动它（引擎那边同时也在冻结这条的时钟）。
                 */
                var boxX = data.x !== undefined ? data.x : item.x
                var boxW = data.w !== undefined ? data.w : item.width

                if (data.hovered === true) {
                    if (item.motionRunning())
                        item.pauseMotion()
                } else if (view._driving && durationMs > 0 && progress < 1) {
                    if (rebound || !item.motionRunning() || Math.abs(item.x - boxX) > 4) {
                        item.stopMotion()
                        item.x = boxX
                        item.startMotion(boxX, -boxW, durationMs * (1 - progress) / rate)
                    }
                } else {
                    /* 暂停中 / 已经走完：原地停住，位置对齐引擎 */
                    item.stopMotion()
                    item.x = boxX
                }
            } else {
                /* 顶部/底部固定弹幕：不移动 */
                item.stopMotion()
                item.x = data.x !== undefined ? data.x : 0
            }

            if (!rebound)
                continue

            /* ↓↓↓ 从这里往下只在"换了弹幕 / 字号或渲染设置变了"时执行（会重排一次） ↓↓↓ */
            view._slotKeys[i] = key

            var advance = data.textAdvance !== undefined ? data.textAdvance : data.w

            item.width = data.w !== undefined ? data.w : 0
            item.height = data.h !== undefined ? data.h : 0
            item.text = data.text !== undefined ? data.text : ""
            item.textColor = data.color !== undefined ? data.color : "#ffffff"
            item.fontSize = fontSize
            /* 本人弹幕（uid 1）→ 白框；固定弹幕 → 参考里那套更重的样式 */
            item.selfBorder = data.isSelf === true
            item.fixedStyle = data.type !== undefined && data.type !== "scroll"
            /* TextMetrics 量出来的宽度由引擎带着一起回来（QML 不用再量一遍） */
            item.textAdvanceWidth = advance !== undefined ? advance : 0
            item.textLineHeight = item.fontSize

            /* 渲染设置（面板右页那三项） */
            item.fontFamily = view.fontFamily
            item.fontBold = view.fontBold
            item.outlineType = view.outlineType
        }

        /* 收尾：这一帧用不到的池子项（上一帧还亮着的那几个）关掉。
           【必须一起停动画】XAnimator 只要在跑就会一直请求重绘，藏起来的条目还留着动画
           就是白烧 CPU（弹幕一退场、或者整层被关掉都必须停）。 */
        for (var j = count; j < view._lastPresentedCount; ++j) {
            var stale = pool.itemAt(j)

            if (stale !== null && stale.visible) {
                stale.stopMotion()
                stale.visible = false
            }
        }

        view._lastPresentedCount = count
    }

    /* -----------------------------------------------------------------------
     * 旧 API 的方法（调用方原样在用）
     * ----------------------------------------------------------------------- */

    function setArea(value) { view.area = value }
    function setSpeed(gear) { view.speed = gear }
    function setFontSize(pixelSize) { view.baseFontSize = pixelSize }
    function setAutoScale(on) { view.autoScale = on }
    function setFilter(next) { view.filter = next }
    function resetFilter() {
        view.filter = ({ "scroll": false, "fixed": false, "colorful": false, "advanced": false })
    }

    /* 灌一批弹幕：每项 { text, timeMs, type, fontSize, color, isSelf, uid } */
    function loadDanmaku(list) { engine.load(list) }

    /*
     * 清屏（数据保留）/ 连数据一起清。
     *
     * 【为什么必须补这两个转发】Main.qml 的 loadSidecarDanmaku() 里第一句就是
     *     danmakuLayer.reset()
     * （"换片必清：先 reset（连数据一起丢），再尝试加载 —— 否则上一部的弹幕会串到这一部"）。
     * 但本文件当初换掉旧的 JS 版 DanmakuLayer 时**漏了这两个转发**，C++ 那侧
     * DanmakuController 明明是有的（clear() / reset()，见那个头文件 169-170 行）。
     * 后果是运行期每次打开片源都报：
     *     TypeError: Property 'reset' of object DanmakuView_QMLTYPE_... is not a function
     * 然后加载同名 .xml 弹幕 —— 表现就是**换集之后上一部的弹幕还挂在屏幕上**。
     * 一条 TypeScript 时代的调用就这么静默失效了，所以这两个函数别再删。
     */
    function clear() { engine.clear() }
    function reset() { engine.reset() }

    /*
     * 从文件灌弹幕（B 站那种 XML）：Main.qml 在打开片源时会找"同目录同名的 .xml"，
     * 找到就调这里。返回解析出来的条数（0 = 文件不存在或里面没有有效弹幕）。
     * 解析在纯 C++ 的 Danmaku 库里做，这层只是转一手。
     *
     * ⚠️ 同步版本：读文件 + 解析都在**调用线程（GUI 线程）**上跑完才返回。
     * 界面请用下面的 loadXmlFileAsync() —— 一次解析上千条弹幕就是"打开视频卡一下"。
     */
    function loadXmlFile(path) { return engine.loadXmlFile(path) }

    /* =======================================================================
     * 异步加载（打开视频不再卡一下）
     *
     * 读文件 / 解析 XML / 建列表全部在 Qt 线程池的工作线程上做，解析完把结果交回
     * GUI 线程才喂引擎。这里只是在引擎外面转一手 + 把两个信号透出去：
     *     danmakuLoaded(token, count)      —— 解析完成（count = 0 表示里面没有有效弹幕）
     *     danmakuLoadFailed(token, reason) —— 文件不存在 / 打不开
     * token 是发起时返回的序号，QML 用它认领自己的那次结果（换片之后回来的旧结果会被丢掉）。
     * ======================================================================= */

    /* 异步：从本地弹幕文件加载 */
    function loadXmlFileAsync(path) { return engine.loadXmlFileAsync(path) }

    /* 异步：直接给 XML 文本（网络那条路用：mock 的 /x/v1/dm/list.so 返回的就是这种 XML） */
    function loadXmlTextAsync(xml) { return engine.loadXmlTextAsync(xml) }

    /* 自己发一条：{ type, fontSize, speed, color, self, uid } —— 控制栏发送框在调 */
    function sendDanmaku(text, options) {
        var opts = (options === undefined) ? ({}) : options
        /* 把引擎的裁决原样透出去（false = 没被接受），调用方据此决定清不清输入框、打什么日志 */
        var accepted = engine.send(text, opts)

        /* 本地收下了就同时广播给 mock（测试页/别的客户端才看得到这条） */
        if (accepted !== false)
            view.publishDanmaku(text, opts)

        return accepted
    }

    /* 示例弹幕（Ctrl+D 那个演示入口） */
    function generateSample(count, durationSec) { engine.generateSample(count, durationSec) }

    /* =======================================================================
     * 实时弹幕（WebSocket）
     *
     * 契约（mock 那边定的，见 front/player/mock-server/README.md）：
     *   地址  ws://<mock>/danmaku/ws
     *   下行  {"type":"danmaku","text":…,"timeSec":12.34,"mode":"scroll|top|bottom",
     *          "fontSize":25,"color":"#FFFFFF","uid":"1","id":"dm-7","seq":7}
     *         连上先收一条 hello；{"type":"pong"} 是 ping 的回应
     *   上行  {"type":"send","text":…,"timeSec":…,"mode":…,"color":…}（我们走 POST /danmaku/send）
     *
     * 【为什么用 WebSocket 而不是轮询】轮询是"每 N 毫秒发一次请求"，没有新弹幕时也在空转；
     * WS 是服务端推，只有真有新弹幕时才来数据。
     * 【历史 vs 实时】那 1000 条历史弹幕走 /x/v1/dm/list.so（Main.qml 里加载），
     * WS 只负责"新发的"（mock 连上时不回放历史）。
     * ======================================================================= */

    /* mock 的 WS 地址；空串 = 没有 mock（不连接）。Main.qml 探测到基址后写进来。 */
    property string liveUrl: ""

    /* 自己刚发出去的那些（text|0.1 秒）：mock 会把我们发的也广播回来，用它去重、避免重复上屏 */
    property var _echoKeys: []

    /*
     * 【★ 为什么实时弹幕要放在独立文件里用 Loader 加载】
     * `import QtWebSockets` 是**硬依赖**：某个 kit（本机 Debug/静态那套）没有这个 QML 模块时，
     * 带那句 import 的文件会**整体加载失败** → DanmakuView 挂 → PlayerView 挂 →
     * **播放器窗口根本建不出来**（用户实测："点击视频打不开窗口了"）。
     * 动态加载之后：模块缺失只是这个 Loader 加载失败 + 一行日志，实时弹幕降级不可用，
     * 窗口和其它弹幕功能完全不受影响。
     */
    Loader {
        id: liveLoader

        active: view.liveUrl !== ""
        source: "DanmakuLive.qml"

        onLoaded: {
            item.liveUrl = Qt.binding(function () { return view.liveUrl })
            item.danmakuReceived.connect(view.handleLiveDanmaku)
            item.failed.connect(function (why) { console.warn("[danmaku] 实时弹幕：" + why) })
        }
        onStatusChanged: if (status === Loader.Error)
                             console.warn("[danmaku] 实时弹幕不可用：DanmakuLive.qml 加载失败"
                                          + "（这个 kit 可能没装 QtWebSockets 的 QML 模块）")
    }

    /*
     * 收到一条下行消息。
     * 【为什么用 engine.send() 而不是 append()】send() 是"立刻上屏"（引擎的 addImmediate），
     * append() 是塞进调度表、要等它自己的时间点才出现 —— 实时弹幕要的是前者。
     */
    function handleLiveDanmaku(msg) {
        /* JSON 解析在 DanmakuLive.qml 里做（那边才有 WebSocket 依赖） */
        if (msg === null || msg === undefined || msg.type === undefined)
            return

        if (msg.type === "danmaku") {
            /* 自己发的会被广播回来：认出来就跳过（本地已经立刻上屏过了） */
            var key = ("" + msg.text) + "|" + Math.round(Number(msg.timeSec) * 10)
            var at = view._echoKeys.indexOf(key)

            if (at >= 0) {
                view._echoKeys.splice(at, 1)
                return
            }

            engine.send("" + msg.text, {
                            "type": "" + msg.mode,          /* 控制器认 "scroll"/"top"/"bottom" */
                            "color": "" + msg.color,
                            "fontSize": Number(msg.fontSize),
                            "uid": "" + msg.uid,
                            "self": false                    /* 别人发的：不加本人白框 */
                        })
            return
        }

        if (msg.type === "error")
            console.warn("[danmaku] mock 拒绝了这条实时弹幕：" + msg.message)
    }

    /*
     * 把一条弹幕发给 mock（这样测试页/别的客户端才看得到）。
     * 走 README 里的 POST /danmaku/send（字段宽容：mode 字符串或数字、color #RRGGBB 或十进制）。
     * 【失败不影响本地显示】本地已经立刻上屏了，这里只是广播，失败只打一行日志。
     */
    function publishDanmaku(text, options) {
        if (view.liveUrl === "")
            return

        var base = view.liveUrl.replace(/^ws/, "http").replace(/\/danmaku\/ws\/?$/, "")
        var opts = (options === undefined) ? ({}) : options
        var body = {
            "text": text,
            "timeSec": view.smoothPositionMs / 1000.0,
            "mode": (opts.type !== undefined) ? opts.type : "scroll",
            "color": (opts.color !== undefined) ? opts.color : "#FFFFFF",
            "uid": "1"
        }

        /* 记下去重键（和 handleLiveMessage 里那条算法一致：timeSec × 10 取整） */
        view._echoKeys.push(text + "|" + Math.round(view.smoothPositionMs / 100.0))

        if (view._echoKeys.length > 20)
            view._echoKeys.shift()

        var xhr = new XMLHttpRequest()

        xhr.open("POST", base + "/danmaku/send")
        xhr.setRequestHeader("Content-Type", "application/json")
        xhr.onreadystatechange = function () {
            if (xhr.readyState === 4 && xhr.status !== 200)
                console.warn("[danmaku] 广播到 mock 失败：HTTP " + xhr.status)
        }
        xhr.send(JSON.stringify(body))
    }
}
