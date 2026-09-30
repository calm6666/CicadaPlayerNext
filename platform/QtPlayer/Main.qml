//
// 组件演示：播放一个视频 + 控件 + 诊断信息。
//
// 这里刻意只用最基础的 QML 元素（没有 Qt Quick Controls，避免额外依赖）：
//   * 画面：CicadaPlayerItem 本身就是视频输出面（它把解码帧画进场景图）；
//   * 进度条：一个 Rectangle 手工画的滑块，点击/拖动 -> player.seek()；
//   * 播放/暂停、打开文件、设置、全屏：四个按钮；
//   * 音量：同样手画的滑条，拖动 -> player.volume；
//   * 设置页：遮罩 + 面板，硬解/循环/静音/倍速/镜像/旋转/诊断开关；
//   * 控制条自动隐藏：鼠标动一下就出来，停 3 秒淡出（鼠标压在控制条上时不藏）；
//   * 左上角文字：实时显示当前走的是零拷贝还是 CPU 回退，方便判断性能问题。
//
// 为什么按钮和滑条都手画、不用 QtQuick.Controls：见 README —— 少一个模块依赖，
// 组件拷到别的工程里也不会因为 Controls 的样式/版本问题出岔子。
//
// 怎么换片子（三条路都不用改 QML、不用重新编译）：
//   1. 点左下角「打开」按钮：系统文件对话框里挑一个本地视频；
//   2. 把视频文件直接**拖进窗口**；
//   3. 启动时带上路径：appQtPlayer.exe D:\video\4k.mp4
//      （见 main.cpp，那个路径通过 startupSource 属性传进来）
//
// 无边框窗口（QWindowKit）
// ----------------------
// 窗口没有系统标题栏。界面顶部那一条（标题 + 主题按钮 + 最小化/最大化/关闭）由本文件里的
// titleBar 代替，而且它是**默认隐藏**的：鼠标移到窗口最上面那 36px 才浮出来，
// 底色 transparent（直接压在画面上），全屏时永远不出来。
// 起因是用户的三条要求：**无标题栏、顶部透明、右上角三颗按钮只在鼠标移到顶部时显示**。
// 所以这个窗口和首页（HomeWindow.qml）刻意不一样 —— 首页是常显的标题栏。
// 实现要点（三条，缺一条就出问题）见下面 topHotZone / titleBar 那一段的注释。
//
// QWindowKit 只负责**窗口系统**那一层：
//   * Windows：原生 WM_NCHITTEST，所以贴边布局、双击标题栏最大化、右键系统菜单、
//     多屏不同 DPI 的命中测试都是系统行为；
//   * macOS / Linux 各自有原生实现。
// 它完全不碰图形 API —— 场景图用哪个 RHI 后端、用哪块 GPU 设备仍然百分之百由 Qt 决定，
// 我们既不指定也不替换（组件里对 QSGRendererInterface 只有只读探针）。
//
// 四个关键约定（少一个都会出问题）：
//   1. **不要自己写 `flags: Qt.FramelessWindowHint`** —— 去掉系统边框是 QWindowKit 按平台
//      自己做的事（macOS 它改 NSWindow、Linux 它给窗口设上 FramelessWindowHint、Windows 它
//      只去掉顶部那块非客户区）。自己加会把窗口变成 WS_POPUP 并丢掉 WS_THICKFRAME，
//      于是左右下三边和四个角全都不能拖拽缩放（详细原因见 Window 里那段注释）；
//   2. `WindowAgent` 必须在窗口创建之后 `setup(root)`，而且要先 `visible: false`、
//      都注册完再显示（QWindowKit README 的 Qt Quick 用法，见 Component.onCompleted）；
//   3. 可拖动区域要用 `setTitleBar(标题栏)` 告诉它，三颗窗口按钮要用
//      `setSystemButton(WindowAgent.Minimize/Maximize/Close, 按钮)` 注册 ——
//      注册过的区域才会被当成"标题栏按钮"：悬停能到 QML、Windows 11 的贴边布局
//      悬浮窗也会出现；
//   4. 标题栏里**非系统按钮**的可交互元素（那颗主题按钮）要 `setHitTestVisible(item, true)`，
//      否则它算在拖动区里，点击到不了 QML。
//
// 注意分工：QWindowKit 只做**窗口系统**这一层（命中测试、拖动、改大小、系统菜单），
// 它**不会**替我们最小化/最大化/关闭窗口 —— 点了按钮之后干什么，由下面按钮的
// onClicked 自己写。
//
// 主题（深浅色）
// --------------
// 调色板在单例 `QtPlayerTheme.qml` 里（一张扁平表，每个键都是 `dark ? 深 : 浅`），
// 这个文件里**不再出现任何颜色字面量**：全部走 `QtPlayerTheme.xxx`。
// 深浅从两个地方来：默认跟随系统（`Application.styleHints.colorScheme`，Qt 6.5+），
// 也可以在设置页里强制浅色/深色，或者点标题栏那颗太阳/月亮按钮快速切。
// 切换时要额外通知**窗口系统那一层**（QWindowKit 的 "dark-mode" 属性），见 applyWindowTheme()。
//
import QtQuick
import QtQuick.Dialogs
import QWindowKit
import QtPlayer
/* AppDialogs：**从 C++ 打开系统原生的文件/目录选择框**（见 src/AppFileDialogs.h）。
 * 用它替代 QML 的 FileDialog 是因为 QML 那个对象会被引擎持有、原生句柄关不掉
 * （"连开几次之后再点打开就没反应"就是它）。平台不支持时 available() 为 false，
 * 下面会自动退回 QML 的 FileDialog 那条路（代码保留，不再删除）。 */
import CicadaPlayer

Window {
    id: root
    /*
     * 默认窗口尺寸：用户要求"播放器窗口缩小一点"（原 1280x720 → 1080x608，
     * 仍是 16:9，面积小了约 29%，在 1080p 屏上不会顶到任务栏）。
     */
    width: 1080
    height: 608
    /*
     * 先不显示。这是 QWindowKit README 里 Qt Quick 那一节的用法：
     *     visible: false  →  windowAgent.setup(window)  →  window.visible = true
     * 目的是让 QWindowKit 在窗口**第一次显示之前**就把窗口过程挂好、把系统标题栏处理掉，
     * 否则会先按普通窗口显示一下再被改成无边框（可能闪一下原生标题栏）。
     * 真正的"显示"在下面 Component.onCompleted 的最后一句。
     */
    visible: false
    title: qsTr("CicadaPlayer Qt6 QML 组件示例")

    /*
     * 【必须显式清成 null —— 否则任务栏里看不到这个窗口】
     *
     * `transientParent` 一旦指向另一个窗口，本窗口就成了那个窗口的**从属窗口**，
     * Windows 下从属窗口**没有自己的任务栏按钮**（Alt+Tab 里也并到父窗口那一项）。
     * 用户要的是"开几个窗口就有几个任务栏项"，所以这里钉死 null。
     * （顺带把 HomeWindow 那边 createObject 的 parent 也改成 null 了，见那个文件的注释：
     *   光这里清掉还不够，创建时的 parent 是 Window 的话 Qt 已经把它当从属窗口建出来了。）
     */
    transientParent: null

    /*
     * 窗口关闭时要**显式停播 + 清掉片源**。
     *
     * 【不写这一句的后果，用户实测报的就是这些】
     * Qt 里 `close()` 只是把窗口**藏起来**，不是销毁：
     *   * QML 对象树还在 → `CicadaPlayerItem` 还在 → `MediaPlayer` 的解码线程、音频线程
     *     照常跑 → **窗口关了声音还在放**；
     *   * `DanmakuView` 的计时器是 `running: player.playing`，弹幕层按 30Hz 继续 tick、
     *     视频继续解码 → **CPU 一直高**；
     *   * 解码器那张表面池（1080p 约 60MB、4K 约 250MB，见 docs/MEMORY.md）也还挂着
     *     → **内存下不来**。
     * 一句话：关窗不停播 = 后台偷偷继续放。
     *
     * 【为什么是"清片源"而不是"销毁窗口对象"】
     * 上一版让 HomeWindow 在窗口关闭后把这个对象 `destroy()` 掉，结果是用户实测的
     * **"关掉播放器后再点卡片，播放器卡死、整个程序闪退"**：QML 的 destroy() 会把
     * QQuickWindow **立刻 delete**，而那一刻它的场景图 / RHI / 渲染线程还在收尾。
     * 现在改成清空片源 —— `setSource("")` 内部会调 `destroyPlayer()`，
     * 该释放的（解码器、渲染线程手里的帧、D3D11 输入视图、整张表面池）全都释放，
     * 而**窗口对象留着**：下次点卡片直接复用，没有任何生命周期上的花样。
     */
    /*
     * 【关窗即销毁的触发点】这个信号由本窗口在 `onClosing` 里发出，由创建它的
     * HomeWindow 接住并**推迟到本次事件处理之后**销毁本窗口对象（见那个文件里
     * ensurePlayerWindow / reapPlayerWindow 的说明）。
     *
     * 【为什么必须由 closing 驱动，而不是看 visible】
     * ✕ 走的是原生关闭，Qt 的 `visible` 标志在平台/QWindowKit 这一层**不保证**会在
     * close 之后变成 false（本仓 `Main.qml` 文件对话框那一段就吃过同一个坑：
     * "它的 visible 可能一直停在 true"）。而 `visibleChanged` 是本工程原来唯一的销毁触发点，
     * 于是只要 visible 不变，销毁那一跳**永远不会被调用**，窗口对象和它整棵 QML 树
     * 就留到进程退出 —— 这是实打实的"每开/关一轮涨一块"的形状。
     * `closing` 是 Qt 在关闭事件里必然发出的（✕、Alt+F4、系统菜单关闭都会走），
     * 所以拿它当触发点是**确定性**的，不依赖任何平台读数。
     */
    signal closeCommitted()

    onClosing: {
        player.stop()
        playerView.source = ""

        /*
         * 释放已在上面的 `source = ""` 里做完（destroyPlayer()：解码器 + 表面池 +
         * 队列 + demuxer/数据源 + 纹理）。这里再发一个信号，让**窗口对象自己**
         * 也被销毁 —— 只销毁播放器窗口，首页/主窗口与进程都留着。
         *
         * 只发信号、不在这里 destroy()：closing 正在处理中，此刻销毁 QQuickWindow
         * 会撞上还在收尾的场景图/RHI（本文件上面那段注释解释过）。接线方会把它
         * 推迟到本次事件处理结束之后（Qt.callLater），那时 close 已经走完。
         */
        root.closeCommitted()
    }

    /*
     * 【释放探针】整棵播放器窗口的 QML 对象树被销毁的那一刻。
     *
     * 用 `console.warn` 而不是 `console.log`：**Release 构建里 console.log 看不到**。
     * 链路是 main.cpp:842 `qInstallMessageHandler` → cicadaQtMessageHandler：
     *   QtDebugMsg -> AF_LOGD（main.cpp:402-403），而 Release 下框架日志级别是
     *   AF_LOG_LEVEL_INFO（frame_work_log.c:268-272），`__log_print` 在 prio > level 时
     *   直接 return（frame_work_log.c:279）⇒ **D 级被丢掉**。
     *   QtWarningMsg -> AF_LOGW（main.cpp:410-411），24 <= 32，**必然进日志**。
     * 这一条只是辅助；"窗口对象到底死没死"以 C++ 侧那条
     * `[mem] the player window object was destroyed`（CicadaPlayerItem 里接的
     * QQuickWindow::destroyed）为准 —— 那条走 AF_LOGI，必定进同一份日志。
     */
    Component.onDestruction: console.warn("[mem] Main.qml: player window QML tree destroyed")
    /*
     * 窗口底色分两种状态（这就是"深浅色只作用于没选片子时那块 UI"的落点）：
     *   * 没选片子：主题色 windowBg —— 中间的提示文字、控制条、设置页都在这块底上，
     *     深浅色切换看的就是这里；
     *   * 选了片子：黑色 videoBg —— 播放区整块是黑的，画面比窗口小时露出来的也是黑边
     *     （浅色主题下也不会出现"视频后面一片白"）。
     * 顶部那一条现在是**透明**的（见下面的 titleBar），所以"只有顶部是主题色"那种分割
     * 已经没有了：顶部栏浮出来的时候直接压在画面上。
     */
    color: hasSource ? QtPlayerTheme.videoBg : QtPlayerTheme.windowBg

    /*
     * 这里**故意不写 `flags:`**（不设 Qt.FramelessWindowHint）—— 和 HuskarUI 的
     * HusWindow.qml、QWindowKit README 里的 Qt Quick 示例完全一致：窗口就当一个普通窗口建，
     * "去掉系统边框"这件事由 QWindowKit 按平台自己去做。
     *
     * 为什么自己加 FramelessWindowHint 反而是错的（踩过，四个角 + 左右下三边全都不能拖拽缩放）：
     *   * Qt 收到 FramelessWindowHint 会把窗口样式改成 WS_POPUP，**并且把 WS_THICKFRAME
     *     去掉**。而 Windows 上那三边的原生 resize 区域恰恰是 WS_THICKFRAME 带来的
     *     （左/右/下各一条 8px 的隐形区域，由 DWM 负责）；
     *   * QWindowKit 的 Windows 实现走的是"**只去掉顶部的非客户区**、保留另外三边原生边框"
     *     这条路（win32windowcontext.cpp 的 WM_NCCALCSIZE 处理，注释里写得很清楚：
     *     1.x 是完全去边框、2.x 改成保留三边）：系统边框开着时它只会把你的 WS_SYSMENU
     *     去掉，**不会**替你补 WS_THICKFRAME（补那一位只发生在"关掉系统边框"的分支里）；
     *   * 于是窗口既没有顶部非客户区、又没有 WS_THICKFRAME：左边/右边/下边/四个角全变成
     *     HTCLIENT（拖不动），只剩顶部那条是 QWindowKit 自己算的 HTTOP，看起来就是
     *     "只有上边能缩放"。实测过：加 FramelessWindowHint 时 style=0x96030000（无
     *     WS_THICKFRAME），左边/右边/下边/角全是 HTCLIENT；去掉之后三边和四角都恢复。
     *
     * 另外两个平台也是 QWindowKit 自己处理的，所以不用我们操心：
     *   * macOS：CocoaWindowContext::winIdChanged 里 `setSystemTitleBarVisible(false)`；
     *   * Linux：QtWindowContext::winIdChanged 里它自己给窗口设上 Qt::FramelessWindowHint。
     *
     * 顺带说明：默认的 Qt.Window 已经带最小化/最大化/关闭这几个提示位，所以原来那串
     * `| Qt.WindowMinimizeButtonHint | ...` 也是多余的；而 WS_SYSMENU 反正会被 QWindowKit
     * 清掉（它自己用 TrackPopupMenu 弹系统菜单，不依赖这个样式位）。
     */

    /*
     * 无边框窗口代理。它把窗口"接管"过来：拖动标题栏移动窗口、贴边、边缘改大小、
     * 右键弹系统菜单。类型由 main.cpp 里的 QWK::registerTypes(engine) 注册
     * （静态链接，没有 qmldir 文件，所以必须 imperative 注册，QML 才能 import）。
     */
    WindowAgent {
        id: windowAgent
    }

    /*
     * 是不是用系统自带的窗口按钮。
     *
     * macOS 上**不自己画**最小化/最大化/关闭：QWindowKit 会把系统原生那三个"红黄绿"
     * 保留下来（见 cocoawindowcontext.mm 里强制显示 standardWindowButton 那段），
     * 那是 mac 用户的肌肉记忆，再画一套反而难用；它们固定在左上角，所以标题文字
     * 要往右让出位置（见 titleBar 里那个 anchors.leftMargin）。
     * Windows / Linux 上系统没有可用的按钮，才由我们自己画。
     */
    readonly property bool nativeCaptionButtons: Qt.platform.os === "osx"

    /*
     * 顶部栏压在什么底色上 —— 它现在是**透明**的（用户要求"无标题栏、顶部透明"），
     * 所以上面的文字和图标必须自己保证在两种底色上都看得清。这和左上角那几行诊断文字
     * 是同一个问题，做法也一样：
     *   * 没选片子：窗口底色是 windowBg → 用主题里那套 captionGlyph / titleText；
     *   * 选了片子：窗口底色是 videoBg（**永远黑**）→ 换成 onVideoText。
     *     少了这一条，浅色主题下深灰色的关闭/最大化图标压在黑画面上等于看不见。
     */
    readonly property bool hasSource: player.source.toString() !== ""
    /*
     * 顶栏现在是**带背景**的浮层（QtPlayerTheme.captionBarBg），标题和图标压的不再是
     * 黑画面而是那条底色 —— 所以直接用主题里"压在标题栏底色上"的那套颜色就行，
     * 不需要再按 hasSource 换色（以前透明顶栏才需要，见 QtPlayerTheme 里的说明）。
     */
    readonly property color captionForeground: QtPlayerTheme.captionGlyph
    readonly property color captionTitleColor: QtPlayerTheme.titleText

    /* 顶部栏的高度。它是"鼠标进顶部热区才显示"，收起时**高度也归零**（见 titleBar）。 */
    readonly property int titleBarHeight: 36

    /*
     * 顶部栏现在显不显示。
     *
     * 判据见下面 titleBarHovered（三个来源取或：顶部热区的被动 HoverHandler、
     * 三颗按钮自己的 hover、以及一条不依赖 hover 分发的坐标兜底）。
     * 全屏时永远为假（用户要求：全屏不显示那三颗按钮）。
     */
    property bool titleBarShown: false

    /* 是不是全屏。控制栏、顶部栏、诊断文字现在都读这一个值。 */
    readonly property bool fullscreen: root.visibility === Window.FullScreen

    /*
     * 外面有没有"盖住整个窗口的浮层"开着（设置页 / 文件对话框）。
     * 两个作用：① 控制栏这时不自动收起、也不藏鼠标指针；② 顶部栏这时不露头
     * （它会被遮罩盖住，露出来只会留下一个"鼠标早就离开顶部了、它却还开着"的脏状态）。
     */
    readonly property bool overlayOpen: settingsPage.opened || root.fileDialog !== null

    /*
     * 顶部热区的**第二条判据**（兜底，故意的双保险）。
     *
     * 上面那条 topHotHover 走的是"一个被动 HoverHandler 抢在画面活动层前面拿到 hover"，
     * 这条路我按 Qt Quick 的"hover 归最上层能收 hover 的元素"来设计的；万一哪天
     * 覆盖顺序变了、或者 Qt 的 hover 分发改了行为，代价是**顶部栏再也浮不出来 ——
     * 用户既拖不动窗口也关不掉它**。这个后果太重，所以再补一条完全不依赖 hover 分发的判据：
     *
     *     光标在画面区里（activityArea 自己的 containsMouse）
     *     且它最后停下的 y 坐标在顶部这 36px 之内
     *
     * PlayerView 本来就在实时维护这两个值（`pointerInside` / `lastMouseY`，
     * 见那个文件里 activityArea 的 onPositionChanged / onExited），不需要新增任何东西。
     *
     * 两条判据刚好互补：HoverHandler 拿到 hover 时它就为真；HoverHandler 没拿到时
     * hover 就还在 activityArea 手里，这条就为真。**必有一条成立**，所以顶部栏一定浮得出来。
     */
    readonly property bool topZoneFallback: playerView.pointerInside
                                            && playerView.lastMouseY >= 0
                                            && playerView.lastMouseY < root.titleBarHeight

    /* 显示条件。三个"不显示"：鼠标不在顶部、全屏、有浮层盖着。 */
    readonly property bool titleBarHovered: (topHotHover.hovered || titleBar.captionHovered
                                             || titleBar.surfaceHovered
                                             || root.topZoneFallback)
                                            && !root.fullscreen && !root.overlayOpen

    /*
     * titleBarHovered 一变就把显隐跟着定下来。
     *
     * 这句话写在**根对象**上，而不是给热区挂 onEntered/onExited：判据是两个对象的合成，
     * 只在热区那几个信号里改状态迟早会漏（比如"从热区直接移到关闭按钮上"，
     * 热区确实 exited 了，但按钮进来了，不该收）—— 合成出来的布尔量一变就重算最稳。
     */
    onTitleBarHoveredChanged: {
        if (titleBarHovered) {
            titleBarShown = true
            titleBarHideTimer.stop()
        } else {
            titleBarHideTimer.restart()
        }
    }

    /* 进全屏就立刻收掉，退出全屏后要重新把鼠标移上去才会出来。 */
    onFullscreenChanged: {
        if (fullscreen) {
            titleBarShown = false
            titleBarHideTimer.stop()
        }
    }

    /*
     * 收起的容错延时。
     *
     * 【为什么不是"移出即收"】判据要跨两个对象（顶部热区 / 三颗按钮各自的 hover），
     * 中间必然有一两帧两边都不为真的空档；不延时的话顶部栏会闪。
     * 300ms 和参考实现里那些浮层菜单的容错（100ms 挪动 + 300ms 关闭）是一个量级。
     */
    Timer {
        id: titleBarHideTimer

        interval: 300
        repeat: false

        onTriggered: {
            if (!root.titleBarHovered)
                root.titleBarShown = false
        }
    }

    /*
     * 标题栏上的一颗按钮（最小化 / 最大化 / 关闭，以及主题那颗太阳/月亮）。
     *
     * 图标**全部用 Rectangle 画**，没用字符也没用图标字体：像 "✕" "❐" "☀" 这些字形在部分
     * Linux 发行版上会缺字变成小方框，而字体文件还得跟着打包 —— 画出来的最省事。
     *
     * 用法：kind 选 "minimize" / "maximize" / "close" / "theme"；点下去只发 clicked 信号，
     * 具体干什么由调用方决定（QWindowKit 不替我们操作窗口，理由见文件头）。
     *
     * 颜色直接取主题单例：**单例是类型名，inline component 里也能用**（换成外层文档的 id
     * 就不行了，那属于 unqualified access）—— 这也是调色板做成单例的原因之一。
     */
    component CaptionButton: Rectangle {
        id: captionButton

        /* 画哪种图标：minimize / maximize / close / theme */
        property string kind: "minimize"
        /* 只对 maximize 有效：窗口已经最大化时画"还原"图标（两个错开的方框） */
        property bool maximized: false
        /* 只对 theme 有效：当前是深色主题就画月亮，浅色就画太阳 */
        property bool dark: QtPlayerTheme.dark

        /*
         * 图标线条的颜色。默认是主题里那套"压在窗口底色上"的深/浅色；播放器窗口的顶部栏
         * 是**透明**的（直接压在画面上），那边会换成 onVideoText —— 否则浅色主题下
         * 深灰图标压在黑画面上根本看不见。
         */
        property color glyphColor: QtPlayerTheme.captionGlyph

        /*
         * 鼠标是不是停在这颗按钮上（**透出去给外面用**）。
         *
         * 【为什么必须透出去】顶部栏是"鼠标进顶部才显示"的。显示出来之后，光标一旦停到
         * 某颗按钮上，hover 就归这颗按钮了（Qt Quick 的 hover 是**最上层独占**，见下面
         * pointerInPlayer 那段踩坑记录），那条用来"发现鼠标进来了"的被动 HoverHandler
         * 收不到 hover → 被判成"鼠标已经离开" → 顶部栏刚露出来就被收回去，表现是**一直闪**。
         * 所以显隐判据必须把"任一颗按钮自己的 hover"也算进去。
         */
        readonly property alias hovered: pointer.containsMouse

        signal clicked()

        width: 46
        /*
         * 底色：常态**透明** —— 标题栏是沉浸式的（那一整条没有背景色，直接压在画面上），
         * 按钮也就不该有块底色；只有悬停/按下时给一层半透明色（见主题里的 captionHover）。
         * 相应地，图标里**不再有"挖洞"的画法**（原来还原图标和月亮要靠按钮底色去遮一条线），
         * 这样按钮底色是透明还是半透明都不影响图标形状。
         */
        color: pointer.pressed ? QtPlayerTheme.captionPressed
                               : (pointer.containsMouse ? QtPlayerTheme.captionHover : "transparent")

        /*
         * 只管悬停和点击。
         *
         * hoverEnabled 是给 containsMouse（悬停变色）用的；Windows 上 QWindowKit 会把
         * 非客户区的 WM_NCMOUSEMOVE 转成客户区消息再发回来，所以光标停在按钮上时
         * QML 这边照样收得到消息，"悬停高亮"是真的能亮。
         *
         * cursorShape 显式写箭头：播放器在没有控制条时会把整个窗口的光标设成"空白"
         * （见 activityArea），这里写死箭头可以保证光标停在窗口按钮上时一定看得见。
         */
        MouseArea {
            id: pointer
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.ArrowCursor
            onClicked: captionButton.clicked()
        }

        /* 图标画在这个 14x14 的小方块里，居中 */
        Item {
            anchors.centerIn: parent
            width: 14
            height: 14

            /* 最小化：一条横线 */
            Rectangle {
                visible: captionButton.kind === "minimize"
                anchors.centerIn: parent
                width: 11
                height: 1
                color: captionButton.glyphColor
            }

            /* 最大化：一个空心方框 */
            Rectangle {
                visible: captionButton.kind === "maximize" && !captionButton.maximized
                anchors.centerIn: parent
                width: 11
                height: 11
                color: "transparent"
                border.width: 1
                border.color: captionButton.glyphColor
            }

            /*
             * 还原（窗口已最大化）：就是系统原生那个图标的样子 ——
             * 前面一个空心方框（左下），后面那个只露出**上边和右边两条线**（右上）。
             * 只用线条、不填色，所以按钮底色是透明也不会有问题（原来是拿按钮底色把
             * 后面方框被压住的部分"挖掉"，现在把压住的部分干脆不画）。
             */
            Rectangle {
                visible: captionButton.kind === "maximize" && captionButton.maximized
                x: 4
                y: 0
                width: 10
                height: 1
                color: captionButton.glyphColor
            }

            Rectangle {
                visible: captionButton.kind === "maximize" && captionButton.maximized
                x: 13
                y: 0
                width: 1
                height: 10
                color: captionButton.glyphColor
            }

            Rectangle {
                visible: captionButton.kind === "maximize" && captionButton.maximized
                x: 0
                y: 4
                width: 10
                height: 10
                color: "transparent"
                border.width: 1
                border.color: captionButton.glyphColor
            }

            /* 关闭：两条 45° 交叉的细线 */
            Rectangle {
                visible: captionButton.kind === "close"
                anchors.centerIn: parent
                width: 12
                height: 1
                color: captionButton.glyphColor
                rotation: 45
            }

            Rectangle {
                visible: captionButton.kind === "close"
                anchors.centerIn: parent
                width: 12
                height: 1
                color: captionButton.glyphColor
                rotation: -45
            }

            /*
             * 主题：深色时画月亮，浅色时画太阳。
             *
             * 两个图标都**不填背景色**（按钮底色是透明的）：
             *   月亮 = 一个实心圆。弯月那个"咬一口"要靠一个底色的圆去遮，按钮没底色就画不了，
             *          所以这里就用实心圆 —— 深色模式图标里这是最常见的画法。
             *   太阳 = 一个**空心圆** + 上下左右四道短射线（空心圆代替实心圆心，同样是为了不填色；
             *          14 像素下空心还是有实心的，一眼就能分辨）。
             */
            Rectangle {
                visible: captionButton.kind === "theme" && !captionButton.dark
                anchors.centerIn: parent
                width: 8
                height: 8
                radius: 4
                color: "transparent"
                border.width: 1
                border.color: captionButton.glyphColor
            }

            Rectangle {
                visible: captionButton.kind === "theme" && !captionButton.dark
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                width: 1
                height: 3
                color: captionButton.glyphColor
            }

            Rectangle {
                visible: captionButton.kind === "theme" && !captionButton.dark
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                width: 1
                height: 3
                color: captionButton.glyphColor
            }

            Rectangle {
                visible: captionButton.kind === "theme" && !captionButton.dark
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                width: 3
                height: 1
                color: captionButton.glyphColor
            }

            Rectangle {
                visible: captionButton.kind === "theme" && !captionButton.dark
                anchors.verticalCenter: parent.verticalCenter
                anchors.right: parent.right
                width: 3
                height: 1
                color: captionButton.glyphColor
            }

            Rectangle {
                visible: captionButton.kind === "theme" && captionButton.dark
                anchors.centerIn: parent
                width: 11
                height: 11
                radius: 5.5
                color: captionButton.glyphColor
            }
        }
    }

    /*
     * 启动时由 main.cpp 用 QQmlApplicationEngine::setInitialProperties() 写进来 ——
     * 也就是命令行里给的那个视频路径。没有参数时是空 url。
     */
    property url startupSource

    onStartupSourceChanged: {
        if (startupSource.toString() !== "")
            playFile(startupSource)
    }

    /*
     * 命令行给的是**流媒体清单**（.json 文件）时走这里：内容就是 hili-player
     * `dist/test-{hls,dash}*.json` 那套对象（duration / minBufferTime / video[] / audio[] /
     * segmentInfo）。组件侧把这段文本**原样**透传（CicadaPlayerItem::setManifestJson），
     * 解析和分片展开都在核心播放器里做，Qt 层不转换、不判断 HLS/DASH。
     *
     *     appQtPlayer.exe D:\...\test-hls-v5.json
     *
     * startupManifestName 是那份清单的文件名，只用于界面/日志显示
     * （见 CicadaPlayerItem 的 manifestHint 属性）。
     */
    property string startupManifest
    property string startupManifestName

    onStartupManifestChanged: {
        if (startupManifest !== "") {
            player.manifestHint = startupManifestName
            player.setManifestJson(startupManifest)
        }
    }

    /* =======================================================================
     * 分段（视点）+ 高能进度条（pbp）取数（mock 基址由自动探测/环境变量给）
     *
     * 数据来自本仓库那个离线 mock 服务（front/player/mock-server）：
     *     GET {mock 基址}/x/player/v2?aid=&cid=   → data.view_points（每项 {from,to,content}，**秒**）
     *     GET {mock 基址}/x/player/pbp?aid=&cid=  → data（{step_sec, data:[0~1,…]}，第 i 点 = 第 i*step_sec 秒）
     * 两份都是**可选**数据：拿不到就静默降级 —— 界面保持"没有这两个功能"的样子（分段横条不出现、
     * pbp 区域 opacity 0），只往控制台留一行原因，不弹窗、不重试、不影响播放。
     *
     * 【时机】片源**真正开始播放之后**拉一次；换片先清空、重新拉。
     *   * 用 playing 的上升沿，而不是"改了 source 就拉"：setSource 之后框架要先 Prepare，
     *     那之前回来的数据和这一部可能对不上，何况片源还可能根本打不开；
     *   * 换片必清（下面 resetProgressMeta）是弹幕那条路同一个教训：不清的话上一部的章节会
     *     挂在新片上（见 loadSidecarDanmaku 的注释）。
     *
     * 【为什么放在 Main.qml 这一层】播放器组件（PlayerView）只声明"数据入口"，不知道数据从哪来；
     * 取数据属于应用层的事 —— 和弹幕走"同目录同名 xml"那条路一样，都在这一层接。
     * ======================================================================= */

    /*
     * mock 服务基址（末尾不带 /）。**默认空串 = 自动探测**：
     *   * 先试 baseUrlCandidates 里的地址（9101 → 9000），GET /healthz 返回 {"ok":true} 的胜出；
     *   * 探测到的地址存进 baseUrlDetected，两次取数（v2 / pbp）都用它；
     *   * 这里非空 = 手工指定（设置面板里手填，或 main.cpp 从环境变量 CICADA_MOCK_BASE 注入），
     *     那就**跳过探测**直接用 —— 环境变量优先（见 baseUrl 那行）。
     *
     * 【为什么不能再写死一个端口】用户实测：mock 起在 9000，播放器写死 9101 → 三个接口全不通。
     */
    /*
     * 环境变量 CICADA_MOCK_BASE 的值，由 main.cpp 用 qEnvironmentVariable() 读出来、
     * 在 QQmlApplicationEngine::setInitialProperties 时注入（QML 自己读不到环境变量）。
     */
    property string mockBaseEnv: ""
    /* 这一次探测的结果（只有自动模式会写它；探测失败就清空 = 界面保持没有这两个功能的样子） */
    property string baseUrlDetected: ""
    /* 候选地址：9101 是 mock 的默认端口（front/player/mock-server/server.js:70），9000 是常见的手工端口 */
    readonly property var baseUrlCandidates: ["http://127.0.0.1:9101", "http://127.0.0.1:9000"]
    /* 探测单个候选时的超时：本机地址，1.5 秒足够；超时就当这个候选不可用 */
    readonly property int mockProbeTimeoutMs: 1500

    /* 最终用哪个基址：环境变量 > 自动探测结果 */
    readonly property string baseUrl: mockBaseEnv !== "" ? mockBaseEnv : baseUrlDetected

    /* 业务 id：本地播放没有真的 aid/cid，默认空串（mock 服务不看它们的值，只看路径） */
    property string mediaAid: ""
    property string mediaCid: ""

    /* 已经"武装"好：等下一次 playing 上升沿就去拉（playFile 里置位） */
    property bool progressMetaArmed: false

    /*
     * 请求代次：换片 +1。晚到的响应（上一部的）一看代次不对就丢掉 ——
     * 网络慢的时候"上一部的章节先闪一下再被覆盖"就是少了这一道。
     */
    property int progressMetaToken: 0

    /* 在飞的 XHR（换片时统一 abort，别让旧请求的回调再落地） */
    property var progressMetaRequests: []

    /* 换片时调用：清掉旧数据 + 把这一部标成"等播放开始就拉" */
    function resetProgressMeta() {
        progressMetaToken += 1
        abortProgressMeta()
        playerView.viewPoints = []
        playerView.pbpData = null

        /*
         * 正常情况下"拉"交给 playing 的上升沿（见下面那个 Connections）；
         * 但有一种情况上升沿不会来：调用这一刻播放器**已经是播放状态**
         * （同一个地址重播、或 source 换了但旧播放器还没被销毁）——那时直接拉，
         * 否则这一部会一直没有数据。
         */
        if (player && player.playing) {
            progressMetaArmed = false
            fetchProgressMeta()
        } else {
            progressMetaArmed = true
        }
    }

    function abortProgressMeta() {
        for (var i = 0; i < progressMetaRequests.length; ++i) {
            try {
                progressMetaRequests[i].abort()
            } catch (e) {
                /* 已经结束的请求再 abort 是空操作；这里只是不让异常冒到外面 */
            }
        }

        progressMetaRequests = []
    }

    /*
     * 取分段 + 高能进度条数据。**基址先解析**：
     *   * 已经知道基址（环境变量 / 手填 / 上次探测成功）→ 直接取；
     *   * 否则先探测（9101 → 9000，见 detectMockBase），探到再取；探不到就静默降级。
     */
    function fetchProgressMeta() {
        if (baseUrl !== "") {
            fetchProgressMetaFrom(baseUrl)
            return
        }

        detectMockBase(function (found) {
            if (found !== "")
                fetchProgressMetaFrom(found)
        })
    }

    /*
     * 依次探活候选地址：GET {base}/healthz，第一个 `data.ok === true` 的胜出。
     * 每个失败的候选都打一行日志（连不上 / 超时 / HTTP 码 / 不是 JSON），
     * 全部失败再打一行总结 —— "静默降级"是指界面不动，不是日志也哑掉。
     */
    function detectMockBase(onDone) {
        var i = 0

        function tryNext() {
            if (i >= baseUrlCandidates.length) {
                console.warn("[mock] 没有可用的 mock 服务（试过：" + baseUrlCandidates.join("、")
                             + "）；分段/高能进度条保持不可用，界面不变")
                baseUrlDetected = ""
                onDone("")
                return
            }

            var candidate = baseUrlCandidates[i]
            i += 1

            probeMockHealth(candidate, function (ok, why) {
                if (ok) {
                    baseUrlDetected = candidate
                    onDone(candidate)
                } else {
                    console.warn("[mock] mock 探测失败：" + candidate + " —— " + why)
                    tryNext()
                }
            })
        }

        tryNext()
    }

    /* 探一个地址的 /healthz；回调 (ok, 失败原因) */
    function probeMockHealth(base, onResult) {
        var xhr = new XMLHttpRequest()
        var url = base + "/healthz"
        var settled = false

        progressMetaRequests.push(xhr)

        function finish(ok, why) {
            /* 超时和 error 可能前后脚都到；只认第一个结果 */
            if (settled)
                return

            settled = true
            onResult(ok, why)
        }

        xhr.onreadystatechange = function () {
            if (xhr.readyState !== 4)
                return

            if (xhr.status !== 200) {
                finish(false, "HTTP " + xhr.status)
                return
            }

            var root = null

            try {
                root = JSON.parse(xhr.responseText)
            } catch (e) {
                finish(false, "返回的不是 JSON")
                return
            }

            if (!root || !root.data || root.data.ok !== true) {
                finish(false, "healthz 里没有 data.ok=true（可能不是这个服务）")
                return
            }

            finish(true, "")
        }

        xhr.ontimeout = function () { finish(false, "超时（" + mockProbeTimeoutMs + "ms）") }
        xhr.onerror = function () { finish(false, "连不上（服务没起 / 端口不对）") }

        xhr.open("GET", url)
        xhr.timeout = mockProbeTimeoutMs
        xhr.send()
    }

    /* 真的去取那两份数据（基址已经确定） */
    function fetchProgressMetaFrom(base) {
        if (base === "")
            return

        /*
         * 实时弹幕：把 mock 的 WS 地址交给弹幕层（连不连、断线怎么重连都在 DanmakuView 里管）。
         * 地址由同一份基址推出来：http://127.0.0.1:9101 → ws://127.0.0.1:9101/danmaku/ws
         */
        danmakuLayer.liveUrl = base.replace(/^http/, "ws") + "/danmaku/ws"

        /*
         * 弹幕也在基址确定后才拉得动：片子在探测完成前就打开了的话，
         * loadSidecarDanmaku 里那次 fetch 会因为"还没有基址"空跑，这里补一次。
         */
        fetchDanmakuFromMock()

        var query = "?aid=" + encodeURIComponent(mediaAid) + "&cid=" + encodeURIComponent(mediaCid)
        var token = progressMetaToken

        getProgressJson(base + "/x/player/v2" + query, token, function (data) {
            /* 结构不对就当没有（ProgressRow 那边也是"空 = 不显示"） */
            var list = (data && Array.isArray(data.view_points)) ? data.view_points : []
            playerView.viewPoints = list
        }, "v2")

        getProgressJson(base + "/x/player/pbp" + query, token, function (data) {
            playerView.pbpData = (data && Array.isArray(data.data) && data.data.length > 0) ? data : null
        }, "pbp")

        /*
         * 悬停气泡的**位置帧**：接口的 preview.bin（和上面两条同一次取数、同一个基址）。
         * 照参考实现（front/player/src/api/preview.ts）：它是 UTF-8 文本，
         * `TextDecoder("utf-8").decode(arrayBuffer).split("\u001F")` → string[]，
         * arr[0] 是空占位、arr[k]（k≥1）= 第 5*(k-1) 秒那一帧的 data URL
         * → 气泡里用 `arr[floor(悬停秒/5) + 1]` 取（controls/index.ts:1410-1417）。
         */
        getProgressText(base + "/videoshot/preview.bin" + query, token, function (text) {
            var frames = text.split("\u001F")

            /* 【空文件 = 合法降级】preview.bin 是 0 字节时切出来只有 [""]（一帧都没有）：
               存空数组，气泡那边退回 CaptureScreen 的当前画面。 */
            if (text === "" || frames.length <= 1) {
                playerView.previewFrames = []
                return
            }

            playerView.previewFrames = frames
        }, "preview.bin")
    }

    /*
     * （原「手填基址 → 写回并重取」这条路已按用户要求删除。
     *   基址现在只由 detectMockBase() 自动探测（9101 → 9000）或 CICADA_MOCK_BASE 环境变量决定，
     *   所以这里不再需要 onDataBaseUrlChanged —— 那个属性已经不存在了。）
     */

    /*
     * 一个 GET + JSON.parse 的小工具。**任何一步失败都只打一行日志**：
     * 界面停在"没有这份数据"的样子（需求：静默降级）。
     * onData 是回调，拿到的是接口里的 data 字段（两份数据的 data 结构不一样，所以由回调自己挑）。
     */
    function getProgressJson(url, token, onData, tag) {
        var xhr = new XMLHttpRequest()

        progressMetaRequests.push(xhr)

        xhr.onreadystatechange = function () {
            if (xhr.readyState !== 4)        /* 4 = XMLHttpRequest.DONE */
                return

            /* 代次不对 = 中间换过片，这次结果作废（连日志都不打） */
            if (token !== progressMetaToken)
                return

            if (xhr.status !== 200) {
                console.warn("[mock] " + tag + " HTTP " + xhr.status + "（" + url + "），这一项降级为不可用")
                return
            }

            var root = null

            try {
                root = JSON.parse(xhr.responseText)
            } catch (e) {
                console.warn("[mock] " + tag + " 返回的不是 JSON（" + url + "），这一项降级为不可用")
                return
            }

            if (!root || root.code !== 0) {
                console.warn("[mock] " + tag + " 接口 code=" + (root ? root.code : "null") + "（" + url + "）")
                return
            }

            onData(root.data)
        }

        /* 超时 / 连不上：同样是"这一项不可用"，不重试、不打扰用户 */
        xhr.ontimeout = function () {
            if (token === progressMetaToken)
                console.warn("[mock] " + tag + " 超时（" + url + "），这一项降级为不可用")
        }
        xhr.onerror = function () {
            if (token === progressMetaToken)
                console.warn("[mock] " + tag + " 连不上（" + url + "），这一项降级为不可用")
        }

        xhr.open("GET", url)
        xhr.timeout = 5000
        xhr.send()
    }

    /*
     * 和上面那个一样，只是**按文本拿**（preview.bin 是 UTF-8 文本、不是 JSON，
     * 所以绝对不能走 JSON.parse）。失败/超时的日志与降级口径也一致。
     */
    function getProgressText(url, token, onText, tag) {
        var xhr = new XMLHttpRequest()

        progressMetaRequests.push(xhr)

        xhr.onreadystatechange = function () {
            if (xhr.readyState !== 4)        /* 4 = XMLHttpRequest.DONE */
                return

            if (token !== progressMetaToken)
                return

            if (xhr.status !== 200) {
                console.warn("[mock] " + tag + " HTTP " + xhr.status + "（" + url + "），这一项降级为不可用")
                return
            }

            onText(xhr.responseText)
        }

        xhr.ontimeout = function () {
            if (token === progressMetaToken)
                console.warn("[mock] " + tag + " 超时（" + url + "），这一项降级为不可用")
        }
        xhr.onerror = function () {
            if (token === progressMetaToken)
                console.warn("[mock] " + tag + " 连不上（" + url + "），这一项降级为不可用")
        }

        xhr.open("GET", url)
        xhr.timeout = 5000
        xhr.send()
    }

    /* 片源开始播放的上升沿：如果要拉，就拉一次（并且只拉这一次） */
    Connections {
        target: player

        function onPlayingChanged() {
            if (!player.playing || !root.progressMetaArmed)
                return

            root.progressMetaArmed = false
            root.fetchProgressMeta()
        }
    }

    /*
     * 播放一个地址（本地文件或网络地址）。
     *
     * 三条入口 —— 文件对话框、拖拽、命令行 —— 都调这一个函数，行为完全一致。
     *
     * 只改 source 就够了：CicadaPlayerItem::setSource() 会把旧播放器整个销毁再重建
     * （所以中途换片子不会串上一条流的音轨），而 autoPlay 为真时组件会在新的
     * Prepare() 完成后自动 Start()。这里**不要**再补一句 player.play()：那一刻新
     * 播放器还没 Prepare，调了也没用。
     */
    function playFile(fileUrl) {
        if (fileUrl === undefined || fileUrl === null || fileUrl.toString() === "")
            return

        /*
         * 每次换片都打一行 —— 出问题时这一行 + 组件里那行 "opening source:"
         * 就能看出是"对话框没给地址"还是"给了地址没播起来"。
         */
        console.log("playFile: " + fileUrl + "   (current source: " + player.source + ")")
        playerView.source = fileUrl

        loadSidecarDanmaku(fileUrl)

        /*
         * 换片必清：分段/高能进度条那份数据先清空并作废在飞的请求，
         * 等这一部真的播起来（playing 上升沿）再重新拉 —— 见上面 fetchProgressMeta 那段说明。
         */
        resetProgressMeta()
    }

    /*
     * 弹幕数据源（第一条路）：**视频同目录同名的 .xml**（B 站导出的那种格式）。
     *
     * 为什么做成"同名文件配对"：这样不需要任何账号/接口就能验证整条弹幕链路
     * （解析 → 引擎 → 渲染），和主流播放器"片子旁边放同名弹幕"的习惯一致。
     * 联网按 cid/avid 抓弹幕是下一条路，接在这个函数后面即可。
     *
     * 换片必清：先 reset（连数据一起丢），再尝试加载 —— 否则上一部的弹幕会串到这一部。
     */
    function loadSidecarDanmaku(fileUrl) {
        var url = fileUrl.toString()

        /*
         * 换片必清 + 把上一次异步加载作废（reset() 会把加载序号顶掉，
         * 上一次的结果回来时会被丢掉，不会串到这一部上）。
         */
        danmakuLayer.reset()
        _sidecarToken = -1
        _mockToken = -1
        _sidecarLoaded = false

        if (url.indexOf("file:") !== 0)
            return                                   /* 网络地址暂时没有配对规则 */

        /*
         * file:///D:/x/y.mp4 → D:/x/y.mp4（Windows 本地路径）。
         *
         * 【这里错过一次，日志里的 `///D:/…xml` 就是它】原来写的是
         *     var path = url.substring(5)                       // 只去掉 "file:" → 剩 "///D:/…"
         *     if (path[0] === '/' && path[2] === ':') …          // path[2] 其实是 '/'，条件永不成立
         * 于是 "///" 一路带进 QFile → 打不开（中文路径本身没问题，是路径前缀重复了）。
         * 现在：先按整个 "file://" 去掉（7 个字符），剩下正好是 "/D:/…"，
         * 再去掉那一个前导 '/'；`file://D:/…`（没有第三个斜杠）也照样对。
         * UNC（file://server/share）去掉 scheme 后是 "server/share"，不含前导 '/'，不受影响。
         * 传给 C++ 的**统一是本地路径**（不带 file: scheme），那边只用 QFile 一种打开方式。
         */
        var path = url.substring("file://".length)   /* "/D:/x/y.mp4" */

        if (path.length > 2 && path.charAt(0) === '/' && path.charAt(2) === ':')
            path = path.substring(1)                 /* "D:/x/y.mp4" */

        _sidecarXmlPath = path.replace(/\.[^.\/\\]+$/, "") + ".xml"

        /*
         * 【★ 异步加载：这里是"打开视频卡一下"的根因】以前这里是一句同步的
         * `danmakuLayer.loadXmlFile(xmlPath)` —— 读文件 + 解析整份 XML + 建上千个对象
         * 全在 GUI 线程上跑完才返回，弹幕越大卡得越久。
         * 现在只发一个请求立刻返回，结果从下面那个 Connections 回来：
         *   * 载入到了       → 打一行日志；
         *   * 没这个文件 / 里面没弹幕 → 改走 mock 那条路（和以前一样的兜底）。
         */
        _sidecarToken = danmakuLayer.loadXmlFileAsync(_sidecarXmlPath)
    }

    /* 同名弹幕文件对应的那个 .xml 路径（日志要用，见上面） */
    property string _sidecarXmlPath: ""
    /* 两条异步加载各自的序号（-1 = 没有在等的） */
    property int _sidecarToken: -1
    property int _mockToken: -1
    /*
     * 这一部片子**已经有**同名弹幕文件了（异步加载回来的结果）。
     * 【为什么要这个标志】mock 那条路是"没有同名文件"时的**兜底**（见 fetchDanmakuFromMock
     * 的说明）；不加这一道的话，框架开始播放之后那次 mock 拉取会把刚加载好的同名弹幕
     * 整份换掉（引擎的 load() 是替换而不是追加），片子旁边的 xml 就白配了。
     */
    property bool _sidecarLoaded: false

    /*
     * 异步加载的结果：token 认领自己那一次，然后打日志 / 走兜底。
     *
     * 【为什么必须认 token】加载期间用户可能已经换了片子（或者同名文件没有、改去拉 mock），
     * 晚到的旧结果只能丢掉 —— C++ 那边也会按同一个序号过滤（见 DanmakuController.h）。
     */
    Connections {
        target: danmakuLayer

        function onDanmakuLoaded(token, count) {
            if (token === root._sidecarToken) {
                root._sidecarToken = -1

                if (count > 0) {
                    console.warn("[danmaku] 载入同名弹幕文件 " + root._sidecarXmlPath + "：" + count + " 条")
                    root._sidecarLoaded = true
                    return
                }

                console.warn("[danmaku] 同名弹幕文件里没有有效弹幕（" + root._sidecarXmlPath + "），改从 mock 拉")
                root.fetchDanmakuFromMock()
                return
            }

            if (token === root._mockToken) {
                root._mockToken = -1
                console.warn(count > 0 ? "[danmaku] 从 mock 载入 " + count + " 条弹幕"
                                       : "[danmaku] mock 的弹幕列表是空的 / 解析不出 <d> 节点")
            }
        }

        function onDanmakuLoadFailed(token, reason) {
            /* reason 里已经带了路径和原因（C++ 那边分的"不存在/打不开"），原样打出来 */
            console.warn("[danmaku] " + reason)

            if (token === root._sidecarToken) {
                root._sidecarToken = -1
                /*
                 * 没有同名文件 → 走**第二条路**：mock 的弹幕列表（1000 条随机弹幕）。
                 * 【为什么必须有这条】只认同名 xml 的话，随便打开一部片子弹幕层就是空的 ——
                 * 界面上表现为"一条弹幕都没有"，tip/实时弹幕也就全都试不出来（用户实测）。
                 */
                console.warn("[danmaku] 改从 mock 拉弹幕列表")
                root.fetchDanmakuFromMock()
                return
            }

            if (token === root._mockToken)
                root._mockToken = -1
        }
    }

    /*
     * 弹幕数据源（第二条路）：mock 的 `/x/v1/dm/list.so`（B 站那条接口的同形 XML）。
     *
     * 返回的是 `<d p="时间,模式,字号,颜色,时间戳,池,uid,行号">文字</d>`。
     *
     * 【★ 解析不再在 QML 里做】以前这里是一个正则循环，把上千个 <d> 解成 JS 对象数组
     * 再 loadDanmaku() 交给引擎 —— 那一下全在 GUI 线程上（打开视频卡一下的另一半）。
     * 现在只把**原始 XML 文本**交给 C++ 的异步接口（loadXmlTextAsync），解析在工作线程上做，
     * 而且用的就是解析同名 xml 那**同一个纯 C++ 解析器**（格式本来就是同一种，
     * 模式 1/4/5 → 滚动/底部/顶部 的换算也在它里面，见 DanmakuXml.h）。
     */
    function fetchDanmakuFromMock() {
        if (baseUrl === "") {
            /* 【别静默】基址还没探测到是最常见的"一条弹幕都没有"：留一行日志 */
            console.warn("[danmaku] 还没有 mock 基址，先跳过拉弹幕（探测完成后会自动重拉）")
            return
        }

        if (_sidecarLoaded) {
            /* 这一部已经有同名弹幕文件了，兜底那条路就不该把它换掉（见 _sidecarLoaded 的说明） */
            console.warn("[danmaku] 已有同名弹幕文件，跳过 mock 弹幕列表")
            return
        }

        var xhr = new XMLHttpRequest()
        var query = "?oid=" + encodeURIComponent(mediaCid !== "" ? mediaCid : mediaAid)
        var url = baseUrl + "/x/v1/dm/list.so" + query

        xhr.open("GET", url)
        xhr.onreadystatechange = function () {
            if (xhr.readyState !== 4)
                return

            if (xhr.status !== 200) {
                console.warn("[danmaku] 拉 mock 弹幕失败：" + url + " → HTTP " + xhr.status)
                return
            }

            root._mockToken = danmakuLayer.loadXmlTextAsync(xhr.responseText)
        }
        xhr.send()
    }

    /*
     * 打开文件对话框（「打开」按钮和 Ctrl+O 都走这里）。
     *
     * **每次现建一个 FileDialog，用完（accepted/rejected）就销毁**，不复用同一个对象。
     *
     * 起因是"选了三个片子之后再点「打开」就什么反应都没有"，而且当时那句
     * `visible = false; open()` 的复位只是把问题从第二次推后到第四次 —— 说明问题出在
     * **复用同一个对话框对象**上，而不是某一次调用写错了。
     *
     * 为什么复用会出问题：这类对话框的"可见"在 Qt 内部是**两段式**的
     * （`QQuickAbstractDialog` 里 m_visibleRequested 和 m_visible 是两个变量，
     * 见 qtquickdialogs2/private/qquickabstractdialog_p.h）：open() 只是记下"想显示"，
     * 原生对话框要到下一轮事件循环才建；关闭同样是先清状态、再由 destroy() 收原生句柄。
     * 反复开关同一个对象时，这两段状态和"平台对话框句柄"之间就有交错的机会，表现出来
     * 正是"调用没报错、也没有窗口出现"。
     * 它本来就是"点一次、用一次"的东西，干脆每次 createObject 一个新的、关掉就销毁，
     * 从根上不和历史状态打交道。（这条路是"绕开"而不是"治本"：Qt 那两段式设计没法从
     * 外部改，但对我们这种一次性用法，不复用就没有这个状态可交错。）
     *
     * 主题：这个对话框是**系统原生**的，它的深浅跟着**操作系统**走，不跟我们的
     * QtPlayerTheme —— Windows 的 IFileDialog 自己读系统设置。所以强制深色时它可能仍是
     * 浅色的，这是正常的，不要试图去改它（那要动 QPA 或者自己实现一套文件选择器）。
     */
    function chooseFile() {
        /*
         * 【首选：C++ 的原生对话框】
         *
         * AppDialogs.openVideoFile() 内部每次现建一个平台对话框对象、阻塞等用户选完、
         * 立刻销毁它 —— 原生句柄（Windows 的 IFileDialog / COM 对象）随析构释放，
         * 和 QML 引擎没有任何长生命周期绑定。**这个函数是阻塞的**（内嵌事件循环），
         * 和 QFileDialog::getOpenFileName 一个语义，返回时结果已经拿到了。
         *
         * 下面那一整套 QML 动态创建 / 销毁 / 重试的逻辑**保留**着，只在一个情况下走：
         * 这个 Qt 构建里没有平台对话框支持（AppDialogs.available 为 false，
         * 见 CMakeLists 里 Qt6::GuiPrivate 的探测）。
         */
        if (AppDialogs.available) {
            /* 让窗口先到前台：Windows 的前台锁会让"不在前台的进程"弹不出对话框。 */
            root.requestActivate()

            const file = AppDialogs.openVideoFile(player.source.toString())

            if (file !== "") {
                root.playFile(file)
            } else if (AppDialogs.lastError !== "") {
                console.warn("[dialog] 原生文件对话框不可用：" + AppDialogs.lastError
                             + "（退回 QML FileDialog）")
                _openFileDialog(0)
            }

            return
        }

        /*
         * 【不要用"fileDialog 非空就直接 return"】那正是"开几次之后再也打不开"的成因：
         * 只要 accepted / rejected 有一个没送到（原生对话框被别的方式关掉、
         * 或者 Qt 那两段式可见性没走完），这个对象就会**永远**挡着后面的每一次点击，
         * 而且它的 visible 可能一直停在 true，连"已经关了就先销毁"那条自救也走不到。
         *
         * 现在这里只做**防抖**（800ms 内重复点击忽略，防手快双击；对话框是模态的，
         * 真正开着的时候主窗口根本收不到点击，所以这个窗口期不会有副作用），
         * 超过就认为上一个已经废了：强制收掉再建新的。任何情况下都不会被永久挡住。
         */
        if (fileDialog && (Date.now() - fileDialogOpenedAt) < 800) {
            console.log("chooseFile: debounced (a dialog was just opened)")
            return
        }

        if (fileDialog)
            releaseFileDialog()

        _openFileDialog(0)
    }

    /*
     * 真正开对话框（attempt = 第几次尝试）。
     *
     * 【为什么要重试】Qt 的 Windows 平台插件有一个失败模式（实测日志原文）：
     *     W [qt] ...qwindowsdialoghelpers.cpp:92
     *       Native file dialog: unable to get dialog's window.
     *     D [qt] Main.qml:451 file dialog rejected (cancelled)
     * 也就是"**拿不到可以挂对话框的窗口**" → Qt 直接当成取消（rejected）→ 用户看到
     * "点了没反应 / 打不开"。这条**不是我们代码状态卡住**（日志里 chooseFile 每次都走到了
     * 创建对象那一步），而是平台侧那一刻没给出可用窗口。
     * 这种失败是**瞬时的**：同一个进程里过一会儿再来一次通常就好了 —— 所以这里
     * "没真正显示出来就被拒"当成失败，隔一小会儿自动重试（最多 2 次）。
     *
     * 判据用 dialogVisible：Qt 只有真的把原生窗口弹出来才会把 visible 置真；
     * 用户自己点了取消（或者按 Esc）时它早就为真了，所以不会被误判成失败重试。
     */
    function _openFileDialog(attempt) {
        console.log("chooseFile: creating a fresh file dialog (attempt " + attempt + ", "
                    + "window active=" + root.active + " visible=" + root.visible + ")")

        const dialog = fileDialogComponent.createObject(root.contentItem)

        if (dialog === null) {
            console.log("chooseFile: createObject returned null!")
            return
        }

        fileDialog = dialog
        fileDialogOpenedAt = Date.now()
        fileDialogAttempt = attempt
        fileDialogShown = false

        /*
         * 信号在这里用 JS 接，而不是写在 Component 里的 onAccepted：
         * Component 是**独立作用域**，里面去访问外层的 id（root.xxx）要靠运行期动态解析
         * （qmllint 会警告 unqualified access，还得开 pragma ComponentBehavior: Bound 才
         * 干净）。写在 chooseFile() 里就没有这个问题 —— 这段 JS 的作用域就是根对象。
         */
        dialog.accepted.connect(function () {
            console.log("file dialog accepted: " + dialog.selectedFile)
            /* 和拖拽/命令行同一条路：playFile -> setSource（会重建播放器）。 */
            root.playFile(dialog.selectedFile)
            root.releaseFileDialog()
        })

        dialog.rejected.connect(function () {
            /*
             * 从来没显示出来就被拒 = 平台侧那次没挂上窗口（见上面那段日志）。
             * 这种不是用户取消，退一步重试；重试用完才认账并留下日志。
             */
            if (!root.fileDialogShown && root.fileDialogAttempt < 2) {
                console.log("file dialog: rejected without ever showing -> retry #"
                            + (root.fileDialogAttempt + 1))
                root.releaseFileDialog()
                fileDialogRetryTimer.interval = 250 * (root.fileDialogAttempt + 1)
                fileDialogRetryTimer.restart()
                return
            }

            console.log("file dialog rejected (cancelled)"
                        + (root.fileDialogShown ? "" : " (never became visible — platform refused it)"))
            root.releaseFileDialog()
        })

        /* 真的显示出来了就记一笔：上面那个重试判据靠它区分"用户取消"和"根本没弹出来" */
        dialog.visibleChanged.connect(function () {
            if (dialog.visible)
                root.fileDialogShown = true
        })

        /*
         * 【开之前先把自己激活】原生模态对话框要求"调用它的窗口"是当前活动窗口。
         * Windows 有前台锁：应用不在前台时，IFileDialog 可能**根本不显示**，或者显示在
         * 主窗口后面 —— 用户看到的就是"点打开没反应"；更坑的是那个"看不见的模态对话框"
         * **仍然占着输入**，于是"往窗口里拖文件也没反应"。
         *
         * 实测（本机、Release）：不激活就 open()，进程里连一个对话框窗口都不会出现
         * （枚举顶层窗口只有主窗口）；先 requestActivate() 再 open()，
         * `class='#32770' title='选择要播放的视频文件'` 立刻就出来了。
         * 所以这两个现象是一个原因，这一句就是修它。
         */
        root.requestActivate()
        dialog.open()
    }

    /* 当前那个对话框对象；为 null 表示现在没有对话框。 */
    property FileDialog fileDialog

    /* 上一次创建对话框的时刻（只用于 chooseFile 里那 800ms 防抖） */
    property double fileDialogOpenedAt: 0

    /* 已经关掉、等一拍再销毁的那个对象（见 releaseFileDialog 的说明） */
    property var fileDialogRetiring: null

    /* 这一轮是第几次尝试（0 起），见 _openFileDialog */
    property int fileDialogAttempt: 0

    /* 这一轮的对话框有没有**真的显示出来过**：用来区分"用户取消"和"平台没挂上窗口" */
    property bool fileDialogShown: false

    /*
     * 平台没能弹出对话框时（"rejected without ever showing"）退一步重试。
     * 间隔按次数递增（250ms / 500ms），给平台侧那点瞬时状态一点时间。
     */
    Timer {
        id: fileDialogRetryTimer

        interval: 250
        repeat: false
        onTriggered: root._openFileDialog(root.fileDialogAttempt + 1)
    }

    /*
     * 关掉并安排销毁当前对话框。
     *
     * 【为什么是"先 close、隔一拍再 destroy"】这类对话框的可见性是**两段式**的
     * （QQuickAbstractDialog：m_visibleRequested 和 m_visible 是两个变量，
     * 见 qtquickdialogs2/private/qquickabstractdialog_p.h）。
     * 在 accepted 信号里立刻 destroy()，原生那一侧（关窗、放 COM 句柄）可能还没收完，
     * 后果不只是"对话框状态卡住"：那个**原生窗口可能还占着拖拽目标**，
     * 于是"再往窗口里拖文件也没反应了"（用户报的第二个现象）。
     * 所以顺序改成：
     *   1. 先把 fileDialog 置空 —— 后面点「打开」绝不会再被它挡住；
     *   2. 显式 close() 一次，让原生那一步走完；
     *   3. 400ms（事件循环跑过几轮）之后再真正 destroy()。
     */
    function releaseFileDialog() {
        if (!fileDialog)
            return

        const dialog = fileDialog
        fileDialog = null
        console.log("file dialog closed: retiring it")

        if (dialog.visible)
            dialog.close()

        fileDialogRetiring = dialog
        fileDialogGcTimer.restart()
    }

    /* 收尾定时器：把上一拍退役下来的对话框真正销毁（不能更早，原因见上面那段）。 */
    Timer {
        id: fileDialogGcTimer

        interval: 400
        repeat: false
        onTriggered: {
            if (root.fileDialogRetiring) {
                root.fileDialogRetiring.destroy()
                root.fileDialogRetiring = null
                console.log("file dialog: retired object destroyed")
            }
        }
    }

    /*
     * 对话框"模板"：每次用 Component.createObject() 现建一份，信号在 chooseFile() 里接。
     *
     * nameFilters 只是"挑起来方便"，不决定能不能播：支持哪些封装/编码由框架里的
     * FFmpeg 决定，所以最后一条留了「所有文件」，免得遇到少见后缀（m2ts、flv、
     * 无后缀的本地缓存文件……）反而选不中。
     *
     * 只有 accepted / rejected 两个出口（用户点右上角 X 关窗，原生对话框走的就是
     * reject），两个出口都会把对象销毁掉（见 chooseFile 里接的信号），所以除了
     * "正在显示"之外不会留下任何状态。
     */
    Component {
        id: fileDialogComponent

        FileDialog {
            title: qsTr("选择要播放的视频文件")
            fileMode: FileDialog.OpenFile
            nameFilters: [
                qsTr("视频文件 (*.mp4 *.mkv *.mov *.flv *.ts *.m2ts *.avi *.webm *.wmv *.mpg *.mpeg)"),
                qsTr("所有文件 (*)")
            ]
        }
    }

    /* 诊断信息里显示的"当前文件名"：去掉目录和查询串。 */
    function sourceName() {
        const text = player.source.toString()
        if (text === "")
            return "（未选择）"
        /*
         * url 里的非 ASCII 字符是百分号转义的，转回来才看得清（中文文件名很常见）。
         * QUrl::toString() 生成的转义一定是合法的，decodeURIComponent 不会抛异常。
         */
        return decodeURIComponent(text.split("/").pop().split("?")[0])
    }

    /*
     * 进度条、时间显示这些已经全部搬进 PlayerControlBar.qml 了（那里自己带
     * formatTime/seek 换算），所以根对象上不再留这两个工具函数。
     *
     * 留个记号（踩过的坑）：QML 的名字解析只会把**组件根对象**上的函数/属性带进
     * 整个组件的静态作用域；声明在非根对象里的函数，它自己子对象里的绑定找不到，
     * 表现是 `ReferenceError: xxx is not defined` 且随绑定刷新反复刷屏。
     * 所以给"子对象里的绑定"用的函数，要么放根对象，要么跟着那段界面一起放进
     * 独立的 .qml 组件（现在控制栏就是后者）。
     */

    /*
     * 全屏切换。
     *
     * 只改窗口自己的 visibility，不去调 Win32/平台的窗口 API —— 全屏是 Qt 的事
     * （多屏、DPI、DWM 那些差异它自己会处理）。再点一次按钮，或者**按 Esc** 就还原。
     */
    function toggleFullscreen() {
        if (root.visibility === Window.FullScreen)
            root.visibility = Window.Windowed
        else
            root.visibility = Window.FullScreen
    }

    /*
     * Esc = 退出全屏。
     *
     * 【这一条以前是缺的】全屏按钮、双击画面都写了，全项目注释里也一直念叨"Esc 留给退全屏"
     * （见 PlayerControlBar 里那几处 closePolicy 的说明、以及 SettingsPanel 的注释），
     * 但**没有人真的接这个按键** —— 全屏之后按 Esc 什么都没发生。
     * Qt 的 Window 不会自己处理 Esc（那是 WPF/浏览器那套的习惯），必须显式声明。
     *
     * 【和弹幕类型面板那颗 Esc 互斥，别改成两个都能触发】
     * PlayerControlBar 里还有一颗 Esc（收起"滚动/顶部/底部"那个类型浮层），
     * 它的 enabled 是 `bar.dmTypePanelOpen`。两颗 Shortcut 的 sequence 一样时，
     * 同时 enable 会让 Qt 报 "Ambiguous shortcut overload" 并且**两颗都可能不响**，
     * 所以这里加了 `&& !bar.dmTypePanelOpen`：同一时刻只有一颗是活的。
     */
    Shortcut {
        sequence: "Escape"
        enabled: root.fullscreen && !bar.dmTypePanelOpen
        onActivated: root.toggleFullscreen()
    }

    /*
     * Esc 的第二档：**面板/右键菜单开着时先关面板**（不管全屏与否）。
     *
     * 和上面那颗互斥（两颗 sequence 相同、同时 enabled 会报
     * "Ambiguous shortcut overload" 并且两颗都可能不响）：
     * 上面那颗管"全屏 + 没有浮层"，这一颗管"有浮层"——任一时刻只有一颗是活的。
     */
    Shortcut {
        sequence: "Escape"
        enabled: playerView.anyPanelOpened && !bar.dmTypePanelOpen
        onActivated: playerView.closePanels()
    }

    /* ---------------- 控制栏自动隐藏 / 设置页 ---------------- */

    /*
     * 鼠标最后一次停在窗口里的 y 坐标（-1 表示不在窗口里，或者还没动过）。
     * 只给"藏鼠标指针"（cursorShape）用，不参与显隐判断。
     */
    /* lastMouseY 跟着画面区的鼠标事件搬进了 PlayerView（见那个文件） */

    /*
     * 光标是不是还在**播放器容器里**（画面区 or 控制层）。
     *
     * 两个判据都是"某个 MouseArea 自己的 containsMouse"，可靠且不会互相打架：
     *   * activityArea：铺满窗口那层（画面区），光标在画面里时它为真；
     *   * bar.pointerInside：控制层里任一可交互区域（按钮/进度条/菜单/音量滑杆）的 hover 计数 > 0。
     *
     * 【为什么不用坐标或 HoverHandler】
     *   * 坐标：控制层压在 activityArea 上面，光标一进控制层，activityArea 就不再更新位置，
     *     只能拿"刚进去之前"的旧坐标猜，边界上必然误判（之前就是这么误判成"已离开"，
     *     于是收起 → 光标下的区域消失 → 又触发一轮 hover 事件 → 再显示……**循环闪烁**）；
     *   * bar.hovered（HoverHandler）：按钮上那些 hoverEnabled 的 MouseArea 会把 hover 吃掉，
     *     父级 HoverHandler 根本收不到（实测）。
     */
    readonly property bool pointerInPlayer: playerView.pointerInside || bar.pointerInside

    /*
     * 左上角那几行诊断信息要不要显示（设置页里可以关掉，免得挡住画面）。
     *
     * **状态住在设置页里**（SettingsPage.qml 的 showDiagnostics），这里只是一个别名：
     * 设置页现在是两个窗口共用的组件，开关和读它的地方分在两头会让"到底谁说了算"变含糊。
     */
    property alias showDiagnostics: settingsPage.showDiagnostics

    /*
     * 右方向键上一次触发的时间（毫秒，0 = 没在按）。
     * 只给"按住 → 2 倍速"用：键盘没有抬起事件，用两次触发的间隔判按住，见那段注释。
     */
    property double lastRightKeyMs: 0

    /*
     * 鼠标一动就显示控制条，并把倒计时重新开始；到点了再淡出（见 hideTimer）。
     *
     * 这几个函数同样**必须住在根对象上**（原因见 formatTime 上面那段）。
     */
    function kickAutoHide() {
        /* 控制栏和它那套 3 秒自动隐藏都在 PlayerView 里（搬过去的时候一起带走了） */
        playerView.kickAutoHide()
    }

    /*
     * 打开 / 关闭设置页。
     *
     * 真正的界面在 SettingsPage.qml（从本文件抽出去的，首页那个窗口也在用同一份），
     * 这里只做两件事：开/关，以及顺手叫一下自动隐藏 —— 设置页开着的时候用户不会去动鼠标，
     * 但那 3 秒倒计时归零时控制栏不该溜走（PlayerView 那边靠 overlayOpen 判，这里叫一下
     * 只是为了把倒计时重置到一个干净的起点）。
     */
    function openSettings() {
        settingsPage.open()
        kickAutoHide()
    }

    function closeSettings() {
        settingsPage.close()
        kickAutoHide()
    }

    /*
     * -----------------------------------------------------------------------
     * 播放器快捷键（右键菜单 →「快捷键说明」里列的就是这一组，一一对应）
     *
     * 实现放在根对象上，用 `Shortcut`：Shortcut 是**窗口级**的，不需要哪个元素
     * 拿到焦点就能生效（播放器窗口里没有"输入焦点"的概念，用 Keys 挂在某个 Item 上
     * 很容易因为焦点跑掉而失效）。
     *
     * 和参考实现的取舍：
     *   * 参考那份里的 E（收藏）、Enter（发弹幕）、[ ]（多P）本项目**没有对应功能**，
     *     所以没有实现、也没有列进面板（列了就是"按了没反应"的假快捷键）；
     *   * → 的"长按倍速播放"用 autoRepeat + 一个 400ms 的收尾定时器近似实现：
     *     连按（按住）时切到 2 倍速，停手 400ms 后恢复 1 倍速（键盘没有"抬起"事件）。
     * -----------------------------------------------------------------------
     */

    /* 空格 / K：播放-暂停 */
    Shortcut {
        sequence: "Space"
        onActivated: player.togglePause()
    }

    Shortcut {
        sequence: "K"
        onActivated: player.togglePause()
    }

    /*
     * 右：快进 5 秒；**按住**（autoRepeat 连续触发）→ 切 2 倍速播放，松开后自动恢复。
     *
     * 键盘没有"抬起"事件，所以用"两次触发间隔"来判按住：
     *   * 距上一次触发 > 900ms → 当成新的单击：seek +5s；
     *   * ≤ 900ms（系统的按键重复延迟通常 500ms 左右）→ 当成按住：切 2 倍速，
     *     并把 500ms 的恢复定时器重置。停手 500ms 后定时器到点 → 回到 1 倍速。
     */
    Shortcut {
        sequence: "Right"
        autoRepeat: true
        onActivated: {
            const now = Date.now()
            const holding = root.lastRightKeyMs > 0 && (now - root.lastRightKeyMs) < 900
            root.lastRightKeyMs = now

            if (holding) {
                if (player.playbackRate !== 2.0) {
                    player.playbackRate = 2.0
                    playerView.toast.show(qsTr("2 倍速快进中（松开按键恢复）"))
                }

                accelerateHoldTimer.restart()
            } else {
                player.seek(player.position + 5000)
            }
        }
    }

    /* 左：快退 5 秒 */
    Shortcut {
        sequence: "Left"
        onActivated: player.seek(player.position - 5000)
    }

    /* 上 / 下：音量 ±10% */
    Shortcut {
        sequence: "Up"
        onActivated: player.volume = Math.min(1.0, player.volume + 0.1)
    }

    Shortcut {
        sequence: "Down"
        onActivated: player.volume = Math.max(0.0, player.volume - 0.1)
    }

    /* M：静音开关 */
    Shortcut {
        sequence: "M"
        onActivated: {
            player.muted = !player.muted
            playerView.toast.show(player.muted ? qsTr("已静音") : qsTr("已取消静音"))
        }
    }

    /* D：弹幕开关 */
    Shortcut {
        sequence: "D"
        onActivated: {
            bar.danmakuEnabled = !bar.danmakuEnabled
            playerView.toast.show(bar.danmakuEnabled ? qsTr("弹幕已开启") : qsTr("弹幕已关闭"))
        }
    }

    /* F：全屏 / 退出全屏 */
    Shortcut {
        sequence: "F"
        onActivated: root.toggleFullscreen()
    }

    /* 媒体键：播放 / 暂停（键盘上那几颗，Qt 的 StandardKey 里就有） */
    Shortcut {
        sequence: StandardKey.MediaPlay
        onActivated: player.togglePause()
    }

    Shortcut {
        sequence: StandardKey.MediaPause
        onActivated: player.togglePause()
    }

    Shortcut {
        sequence: StandardKey.MediaTogglePlayPause
        onActivated: player.togglePause()
    }

    /*
     * "按住 → 倍速"的收尾定时器：倍速中每收到一次 autoRepeat 就重置，
     * 停手 500ms 后恢复 1 倍速（键盘没有"抬起"事件，只能用这个近似，见上面那段注释）。
     */
    Timer {
        id: accelerateHoldTimer
        interval: 500
        repeat: false
        onTriggered: {
            if (player.playbackRate !== 1.0) {
                player.playbackRate = 1.0
                playerView.toast.show(qsTr("恢复正常速度"))
            }
            root.lastRightKeyMs = 0
        }
    }

    /*
     * 把当前深浅色告诉**窗口系统那一层**。
     *
     * 对应 HuskarUI 的 `HusWindow.setWindowMode(isDark)`：界面本身不用通知 —— 全是
     * `QtPlayerTheme` 的绑定，`dark` 一变整屏就自己重画了；但窗口**边框/阴影**是系统画的，
     * 得单独说一声，否则浅色界面配一圈深色边框会很难看：
     *   * Windows：QWindowKit 会写 `DWMWA_USE_IMMERSIVE_DARK_MODE`（Win10 1809+），
     *     顺带刷新一次系统菜单主题（右键标题栏那个菜单）；
     *   * macOS / Linux：QWindowKit 各自有实现，拿不到的系统上它返回 false、什么都不做，
     *     不影响功能。
     * 属性名 "dark-mode" 就是 QWindowKit 自己定义的那个（见它 win32windowcontext.cpp）。
     */
    function applyWindowTheme() {
        windowAgent.setWindowAttribute("dark-mode", QtPlayerTheme.dark)
    }

    /*
     * 启动后就开始倒计时：用户一直不动鼠标的话，控制条自己让开（主流播放器就是这样）。
     * 文件对话框 / 设置页开着时倒计时到点也不会藏，见 hideTimer。
     *
     * 这里同时做无边框窗口的初始化。放在 Component.onCompleted 是必须的：
     * 这一步要拿到**已经创建好的** QQuickWindow，太早调用 setup() 没有窗口可接管。
     * 顺序也不能反 —— 必须先 setup()（这时候窗口才归它管），再 setTitleBar() 告诉它
     * 哪块能拖，最后才注册三颗系统按钮（setTitleBar 会把之前注册的按钮清空，见
     * abstractwindowcontext.cpp：换标题栏会 removeSystemButtonsAndHitTestItems()）。
     */
    Component.onCompleted: {
        windowAgent.setup(root)
        /*
         * 【顶部栏启动时是收起的（高度 0 + 不可见），这样注册没问题】
         * QWindowKit 不是"注册那一刻记一个矩形"，而是每次命中测试现算：
         * isInTitleBarDraggableArea() 里先看 `m_delegate->isVisible(titleBar)`，
         * 再看 mapGeometryToScene() 出来的矩形（abstractwindowcontext.cpp:102-136）。
         * 所以高度归零时它认为"这个窗口没有标题栏"（顶部那条是普通客户区，
         * 鼠标移动才送得到 QML，顶部栏才浮得出来）；等它浮出来变成可见，
         * 同一片区域立刻又是拖动区，拖它移动窗口/双击最大化都是系统行为。
         */
        windowAgent.setTitleBar(titleBar)
        if (!root.nativeCaptionButtons) {
            windowAgent.setSystemButton(WindowAgent.Minimize, minimizeButton)
            windowAgent.setSystemButton(WindowAgent.Maximize, maximizeButton)
            windowAgent.setSystemButton(WindowAgent.Close, closeButton)
        }

        /*
         * 主题按钮（太阳/月亮）是"标题栏里的一颗普通按钮"，不是系统按钮 —— Windows 上
         * 标题栏那片区域整体被当成 HTCAPTION（拖动区），不特殊声明的话点它只会拖动窗口，
         * 点击根本到不了 QML。HuskarUI 里对应的是 `addInteractionItem()` 那个函数。
         * 一句话：**标题栏里除了系统按钮之外的可交互元素，都要 setHitTestVisible(item, true)**。
         */
        windowAgent.setHitTestVisible(themeButton, true)

        /* 首次把深浅色同步给窗口系统（后面由 Connections 里的 dark 变化负责）。 */
        root.applyWindowTheme()

        /*
         * 都准备好了，现在才显示（对应文件开头那句 visible: false，见 QWindowKit README 的
         * Qt Quick 用法）。setup() 里那句 winId() 会**在隐藏状态下**把原生窗口建出来，
         * 所以上面的注册都已经落到真正的窗口上了。
         */
        root.visible = true

        root.kickAutoHide()
    }

    /*
     * 主题变了 → 重新通知窗口系统。
     * 用 `Connections` 而不是写 `QtPlayerTheme.onDarkChanged: ...`：给**单例**直接挂信号
     * 处理器这种写法在 Qt 6 里不可靠（单例不是实例化出来的对象），Connections 是官方推荐
     * 的写法，行为也确定。
     */
    Connections {
        target: QtPlayerTheme
        function onDarkChanged() {
            root.applyWindowTheme()
        }
    }

    /*
     * 播放器组件。source 支持：
     *   * 本地文件：file:///D:/video/4k.mp4（Windows）/ file:///home/me/a.mp4
     *   * 网络地址：https://... （框架自带 curl/http2 支持）
     *
     * 留空也行：那时只显示提示文字，等你用「打开」按钮或拖拽选片子。
     */
    /* ---------------- 播放器整体（画面 + 弹幕 + 顶部栏 + 控制栏 + 鼠标活动区） ---------------- */

    /*
     * 播放器整块搬到 PlayerView.qml 里了（原来散在这一层：播放区、弹幕层、顶部栏、控制栏、
     * 四个自动隐藏/鼠标定时器）。本文件从此只留**应用层**的东西：
     * 窗口本身（无边框/标题栏/主题）、文件选择（对话框/拖拽/Ctrl+O）、设置页、诊断文字。
     * 拆分的理由见 PlayerView.qml 开头的说明（为多页面打基础）。
     *
     * 下面三个只读别名是给本文件里老代码留的兼容：原来直接用 id（player / bar / danmakuLayer）
     * 引用的地方照旧能编译，不用为了这次拆分改上百处。
     */
    PlayerView {
        id: playerView

        /*
         * **铺满整个窗口**（原来顶部让给 titleBar 36px）。
         *
         * 顶部栏现在是"鼠标进顶部才浮出来"，而且要求**顶部透明**（压在画面上，不是单独占一条），
         * 所以画面区必须一直顶到窗口最上面 —— 否则顶部会留下一条 36px、和画面不同色的空带，
         * 一显一隐地跳。
         */
        anchors.fill: parent

        source: ""
        sourceTitle: player.source.toString() === "" ? "" : root.sourceName()
        fullscreen: root.fullscreen
        /* 设置页 / 文件对话框开着时：不要自动收起控制栏、不要藏鼠标指针 */
        overlayOpen: root.overlayOpen

        onSettingsRequested: root.openSettings()
        onFullscreenRequested: root.toggleFullscreen()

        /* （这里原来有一行 `highlightEnabled: root.highlightEnabled` —— 那是删除「手填 mock 基址」
           接线时误留的：**Main 的根对象根本没有 highlightEnabled 这个属性**，所以求值得到
           undefined → 运行期警告
               Main.qml:1403: Unable to assign [undefined] to bool
           那一条现在删掉了。高能进度条开关的链路本来就是
               SettingsPanel（读写 bar.highlightEnabled）
                 → PlayerControlBar.highlightEnabled（默认 true）
                 → ProgressRow.highlightEnabled
           不经过 Main，也不需要它参与。） */
        /* 上一个 / 下一个：直接换到播放列表里的相邻一条（同一个窗口换源） */
        onPreviousRequested: if (root.library) root.playFromLibrary(root.library, root.library.currentIndex - 1)
        onNextRequested: if (root.library) root.playFromLibrary(root.library, root.library.currentIndex + 1)

        /* 选集 = **真实播放列表**（首页那个 VideoLibrary 实例，见下面的 playFromLibrary） */
        episodes: root.library ? root.library.items : []
        currentEpisodeIndex: root.library ? root.library.currentIndex : -1

        /*
         * 上一条 / 下一条的显隐判据：**直接问库**（VideoLibrary.hasPrevious/hasNext），
         * 不在 QML 里重算一遍 —— 这样"第一条视频不显示上一个、最后一条不显示下一个"
         * 就只有一个地方说了算（C++ 那边的注释写的就是这件事）。
         * 命令行直接开播放器时 root.library 是 null → 两颗都不显示（单文件没有列表）。
         */
        hasPrevious: root.library ? root.library.hasPrevious : false
        hasNext: root.library ? root.library.hasNext : false

        onEpisodeSelected: (index, episode) => {
            /* 点某一集 = 换到那一集播（同一个窗口换源，不新建窗口） */
            if (root.library)
                root.playFromLibrary(root.library, index)
        }
    }

    /* 播放列表（由首页通过 playFromLibrary 传进来；命令行直接开播放器时为 null） */
    property var library: null

    function playFromLibrary(lib, index) {
        library = lib

        var path = lib.pathAt(index)

        if (path === "")
            return

        lib.currentIndex = index
        playFile("file:///" + path)
    }

    /*
     * 用**网络地址**播放（首页那两颗 DASH / HLS 按钮填进来的 .mpd / .m3u8）。
     *
     * 走的是和"点卡片播本地文件"完全同一条路（playFile）：
     *   * setSource 之后框架**按地址自己判断**是 DASH 还是 HLS（后缀 .mpd/.m3u8，
     *     见 CicadaPlayerItem 里 manifestHint 那段说明），不需要我们喂清单文本；
     *   * "有哪些清晰度"由框架解析清单后经 MediaInfoGet 报上来 → player.qualities，
     *     控制栏那份清晰度菜单读的就是它，点某一档走 player.selectQuality(streamIndex)
     *     —— 也就是说菜单里是**清单里真实的多码率**，不是我们自己编的几档。
     *
     * 这里唯一多做一件事：把播放列表清掉。网络源不在库里，"上一个/下一个"那颗按钮
     * 的显示判据是 root.library（见 hasPrevious/hasNext），不清的话它会挂在上一条视频上。
     */
    function playUrl(url) {
        var u = ("" + url).trim()

        if (u === "") {
            console.warn("[player] playUrl: 地址为空，忽略")
            return
        }

        library = null

        console.log("playUrl: " + u)
        playFile(u)
    }

    /* =======================================================================
     * 用**清单对象**播放（首页"清单对象"入口、命令行 .json、或集成方直接调用）
     *
     * 走的链路和"给个 URL"完全不同的只有**片源从哪来**这一步：
     *   setManifest / setManifestJson → CicadaPlayerItem 把对象原样序列化成 JSON
     *     → MediaPlayer::SetDataSource(json) → ManifestDemuxer::buildPlayList()
     *     → 内部 playList（Representation + 分片寻址）→ 之后**和 URL 路径同一条管线**：
     *       同一个解码器工厂、同一个视频渲染回调（零拷贝那条路）、同一套
     *       MediaInfoGet 清晰度列表、同一套双路解码无感切换 + ABR。
     * 所以清单对象不会丢掉任何"播放器能力"：macOS 的 VideoToolbox 零拷贝、
     * 多码率切换、ABR、seek、音轨/字幕轨、AES-128/DRM 都照旧。
     *
     * 两个函数：
     *   playManifestObject(对象, 名字)  —— QML/JS 里直接给对象（QVariantMap）
     *   playManifestJson(文本, 名字)    —— 给 JSON 文本（粘贴/命令行更顺手）
     * 名字只用于日志和界面提示（manifestHint），不影响管线选择：走 HLS 还是 DASH
     * 分片管线由对象里的 mediaSourceType 决定（缺省 hls，见 MediaManifest.h）。
     * ======================================================================= */

    function playManifestObject(manifest, name) {
        if (manifest === null || manifest === undefined) {
            console.warn("[player] playManifestObject: 空对象，忽略")
            return false
        }

        /* 网络/对象片源都不在播放列表里，清掉，免得"上一个/下一个"挂在上一条视频上
         * （和 playUrl 同一个理由）。 */
        library = null

        player.manifestHint = (name === undefined || name === null) ? "" : ("" + name)

        var ok = player.setManifest(manifest)
        console.log("playManifestObject: " + (ok ? "已交给播放器" : "被拒绝（看日志）"))
        return ok
    }

    /*
     * 清单 JSON 文本。protocol（可选，"dash"/"hls"）非空时走**按协议补全**那条路
     * （CicadaPlayerItem::setManifestForProtocol）：补全缺失的 segmentInfo 字段、
     * 写死 mediaSourceType，并对缺关键字段的对象给出人话错误。
     * 不给 protocol 就是原样透传（命令行 / 老调用方）。
     */
    function playManifestJson(jsonText, name, protocol) {
        var text = ("" + jsonText).trim()

        if (text === "") {
            console.warn("[player] playManifestJson: 内容为空，忽略")
            return false
        }

        library = null

        player.manifestHint = (name === undefined || name === null) ? "" : ("" + name)

        var ok

        if (protocol !== undefined && protocol !== null && ("" + protocol) !== "") {
            ok = player.setManifestForProtocol(text, "" + protocol, player.manifestHint)
        } else {
            ok = player.setManifestJson(text)
        }

        console.log("playManifestJson: " + (ok ? "已交给播放器" : "被拒绝（看日志）"))
        return ok
    }

    /* 本地 .json 文件（路径或 file:// URL）：读文件在 C++ 里做，见 loadManifestFile。
     * 给了 protocol（"dash"/"hls"）就走按协议补全那条路。 */
    function playManifestFile(path, protocol) {
        var p = ("" + (path === undefined || path === null ? "" : path)).trim()

        if (p === "") {
            console.warn("[player] playManifestFile: 路径为空，忽略")
            return false
        }

        library = null

        var ok

        if (protocol !== undefined && protocol !== null && ("" + protocol) !== "") {
            ok = player.loadManifestFileForProtocol(p, "" + protocol)
        } else {
            ok = player.loadManifestFile(p)
        }

        console.log("playManifestFile: " + (ok ? "已交给播放器" : "被拒绝（看日志）"))
        return ok
    }

    /*
     * 【已删】原来这里有一段 demoEpisodes / demoCurrentEpisode（24 条假集数）。
     * 选集接上真实播放列表（VideoLibrary.items）之后它就没有任何引用了，留着只会让人
     * 以为选集还是假的 —— 见 HANDOVER-TODO.md 第三节第 3 条。
     */

    readonly property alias player: playerView.player
    readonly property alias bar: playerView.controlBar
    readonly property alias danmakuLayer: playerView.danmaku
    /* ---------------- 顶部栏（无标题栏 + 透明 + 鼠标进顶部才出来） ---------------- */

    /*
     * 【用户要求】播放器窗口：**无标题栏、顶部透明、右上角三颗按钮只在鼠标移到顶部时显示、
     * 全屏时不显示**。
     *
     * 所以这一条和首页（HomeWindow.qml）**刻意不一样**：
     *   * 首页：常显的标题栏（QWindowKit + 有标题栏），那是视频库窗口该有的样子；
     *   * 这里：默认**完全不占位**（高度 0 + 不可见），鼠标移到窗口最上面那 36px 才浮出来，
     *     底色 `transparent`（直接压在画面上），全屏时永远不出来。
     *
     * 三条实现要点，缺一条就出问题：
     *   1. **判据里必须带上"某颗按钮自己的 hover"**（见 titleBar.captionHovered）——
     *      hover 是最上层独占的，光标一停到按钮上热区就收不到 hover 了，只看热区会闪；
     *   2. **热区用被动 HoverHandler，不用 MouseArea**：MouseArea 即使
     *      acceptedButtons: NoButton 也仍是"能收 hover 的元素"，会把鼠标运动从下面的
     *      画面活动层手里拿走（那会变成"鼠标一移到窗口顶部，控制栏就不自动隐藏了"）；
     *      HoverHandler 是**被动**的，只观察、不参与独占，点击照样穿到下面去
     *      （点顶部空白 = 暂停/播放，和别处一致）。
     *   3. **收起时高度也要归零**：QWindowKit 的 isInTitleBarDraggableArea() 看的是
     *      `m_delegate->isVisible(titleBar)` + 映射出来的矩形（见
     *      abstractwindowcontext.cpp:102-122），高度 0 / 不可见时它就认定"这个窗口没有
     *      标题栏"，顶部那一条于是是普通客户区 —— 鼠标移动才送得到 QML，顶部栏才浮得出来。
     *      反过来，露出来的时候它就是拖动区：拖它移动窗口、双击最大化都是系统行为。
     */
    Item {
        id: topHotZone

        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
        }
        height: root.titleBarHeight
        /* 全屏不出顶部栏，热区也就没必要存在（顺便不再去抢顶部栏那个"?"的 hover） */
        visible: !root.fullscreen
        enabled: visible
        /*
         * 【必须在标题栏**上面**（z 比 titleBar 大）—— 别改回 3，这是"顶部栏一直闪"的修复】
         *
         * 之前热区在标题栏下面：鼠标一移到顶部，热区拿到 hover → 标题栏露出来（高 36）→
         * 热区被标题栏盖住 → Qt 把 hover 从热区转走；而标题栏里那层 MouseArea 的
         * containsMouse 在"鼠标没动、元素刚出现"这一帧**不一定**会立刻变成真 →
         * 三个 hover 来源同时为假 → 300ms 后标题栏收起 → 热区又拿到 hover → 再露出……
         * 用户看到的就是"鼠标移上去一直在闪烁"。
         *
         * 现在热区压在标题栏之上：它用的是**被动**的 HoverHandler（HoverHandler 不独占
         * hover，也不参与按键分发），所以
         *   * 只要光标在这 36px 里，topHotHover.hovered 就一直是真 → 标题栏稳定不闪；
         *   * 下面标题栏里的按钮/拖拽 MouseArea 照样能拿到 hover 和点击（被动 handler
         *     不会把它们挡掉）；
         *   * 热区本身是一个没有 MouseArea 的 Item，不消费任何按键 —— 点击照样落在
         *     标题栏的拖拽区（QWindowKit 的命中测试在窗口系统那一层，和 QML 的 z 无关）。
         */
        z: 5

        HoverHandler {
            id: topHotHover
        }
    }

    /*
     * 顶部栏本体：左边标题，右边主题按钮 + 三颗窗口按钮。
     *
     * 【本轮（用户要求）】两处行为改了，别改回去：
     *
     *   1. **带背景，不再透明**。以前是 `color: "transparent"`（"顶部透明"），后果是
     *      用户看不出哪一条能拖窗口 —— 鼠标移上去以为在画面区，按下去那一下被当成
     *      "点画面"（暂停/播放），拖拽也时灵时不灵。现在给一条**不透明底色**
     *      （QtPlayerTheme.captionBarBg，深浅色各一档），拖拽区一眼可见。
     *      因为底色不再是画面，标题/图标的颜色也就不用再"跟着底下是不是黑画面"换了。
     *
     *   2. **鼠标停在上面就一直显示，移出去才收**（原来是 300ms 容错 + 只认三个 hover
     *      来源）。现在多一条判据：`titleBar.surfaceHovered`（顶栏自己那一层的 hover），
     *      三条 hover 来源取或；移出后 300ms 才收（跨对象时必然有一两帧的空档，
     *      不留这点时间会闪）。
     *
     * 另外给顶栏加了一层 MouseArea：它把落在拖拽区里的**按下**事件吃掉，
     * 顺手调 Window.startSystemMove() —— 这样即使某个平台的命中测试没把这一条
     * 判成 HTCAPTION，拖动也照样能用，而"按下"不会再漏到下面的画面区变成暂停。
     */
    Rectangle {
        id: titleBar

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        /*
         * 收起时**高度归零**，不只是 visible: false —— 原因见上面第 3 条：
         * QWindowKit 判"这一条还在不在"看的是可见性 + 映射矩形，高度 0 才是彻底"不存在"，
         * 顶部那一条才会变回客户区、鼠标移动才送得到 QML（否则顶部栏再也浮不出来）。
         */
        height: root.titleBarShown ? root.titleBarHeight : 0
        visible: height > 0
        /* 不透明底色（用户要求"要带背景"）：见 QtPlayerTheme.captionBarBg */
        color: QtPlayerTheme.captionBarBg
        /*
         * 压在 PlayerView（画面 + 弹幕 + 顶部栏 + 控制栏）**整块**上面 —— 这是拆分之后
         * 新增的一层：以前它只跟 PlayerView 内部那几层比大小。Qt Quick 的 hover 只送给
         * **最上面**那个能收 hover 的元素，不给 z 的话铺满窗口那块画面活动层会把标题栏
         * 按钮的 hover 全吃掉（表现就是"图标移上去没反应"）。
         */
        z: 4

        /* 底边一条分隔线（和参考那些浮层面板一样的 hairlines） */
        Rectangle {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: 1
            color: QtPlayerTheme.captionBarBorder
        }

        /*
         * 鼠标在顶栏上（除按钮之外的区域）—— "鼠标在上面就一直显示"的那条判据。
         * 用普通 MouseArea 的 containsMouse：它同时负责吃掉按下事件（见下面的 onPressed）。
         */
        readonly property bool surfaceHovered: titleDragArea.containsMouse

        MouseArea {
            id: titleDragArea

            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.ArrowCursor

            /*
             * 拖窗口：QWindowKit 在 Windows 上已经把这一条当 HTCAPTION（系统拖动），
             * 但那条路依赖"命中测试那一刻它是可见的"。这里再补一条 Qt 自己的
             * startSystemMove()，任何平台都能拖；同时这次"按下"被本 MouseArea 吃掉，
             * **不会漏到下面的画面区**变成暂停/播放（用户报的就是这个）。
             */
            onPressed: {
                titleBarHideTimer.stop()
                root.startSystemMove()
            }

            /* 双击顶栏 = 最大化 / 还原（和系统标题栏一致） */
            onDoubleClicked: {
                if (root.visibility === Window.Maximized)
                    root.showNormal()
                else
                    root.showMaximized()
            }
        }

        /*
         * 四颗按钮里有没有哪一颗正被鼠标压着 —— "鼠标还在顶部栏上"的第二个判据。
         *
         * 光标停在按钮上时上面那条热区（topHotHover）收不到 hover（最上层独占，
         * 见 CaptionButton.hovered 的说明），只靠热区判会把顶部栏当场收回去。
         * 隐藏的按钮（macOS 上那三颗 ours 为假）containsMouse 恒为假，不影响。
         */
        readonly property bool captionHovered: themeButton.hovered || closeButton.hovered
                                               || maximizeButton.hovered || minimizeButton.hovered

        /* 标题。用窗口自己的 title，任务栏上显示的就是同一个字符串。 */
        Text {
            anchors.left: parent.left
            /*
             * macOS 上左上角是系统原生的三个按钮，标题要让开；
             * 其它平台左边什么都没有，留个普通边距就行。
             */
            anchors.leftMargin: root.nativeCaptionButtons ? 78 : 12
            anchors.verticalCenter: parent.verticalCenter
            /* 右边给主题按钮 + 三颗窗口按钮让位（macOS 上没有我们那几颗，就贴到窗口右边） */
            anchors.right: themeButton.left
            anchors.rightMargin: 8
            elide: Text.ElideRight
            /* 顶部透明 → 文字颜色要跟着"底下是窗口底色还是黑画面"走，见 captionTitleColor */
            color: root.captionTitleColor
            font.pixelSize: 13
            text: root.title
        }

        /*
         * 主题按钮（太阳 / 月亮）：快速切深浅，和设置页里那行「界面主题」是同一个状态。
         *
         * 它放在窗口按钮**左边**、四颗都贴着标题栏右边；点击动作在本文件里：
         *   * 深色 → 切浅色，浅色 → 切深色。也就是从"跟随系统"一按就变成**显式**选定的
         *     相反色（用户按了按钮，意图就该是"我就要这个颜色"，而不是继续跟随）；
         *   * 严格跟随系统时按一下同样会落到显式模式，再想回去就点设置页里的「跟随系统」。
         *
         * 注意它必须在 Component.onCompleted 里用 setHitTestVisible() 注册，否则 Windows
         * 上整条标题栏都是拖动区，点它只会拖窗口（见那边的注释）。
         */
        CaptionButton {
            id: themeButton
            anchors.right: minimizeButton.visible ? minimizeButton.left : parent.right
            height: titleBar.height
            kind: "theme"
            /* 图标跟着当前生效的深浅走 */
            dark: QtPlayerTheme.dark
            /* 图标线条颜色跟着底色走（透明顶部栏，见 captionForeground） */
            glyphColor: root.captionForeground
            onClicked: QtPlayerTheme.mode = QtPlayerTheme.dark ? "light" : "dark"
        }

        /*
         * 三颗窗口按钮。从右往左排（关闭在最右，和系统标题栏一致）。
         *
         * macOS 上整组隐藏：那里用系统原生的"红黄绿"（见 nativeCaptionButtons），
         * 主题按钮这时就直接贴到窗口右边（见上面那个 anchors.right）。
         *
         * "点击之后干什么"必须我们自己在 onClicked 里写，这不是偷懒省事，而是
         * QWindowKit 的设计：它在 Windows 上把这几颗按钮收到的非客户区消息转成了
         * 客户区消息（WM_NCLBUTTONDOWN → WM_LBUTTONDOWN，见 win32windowcontext.cpp
         * 的 emulateClientAreaMessage），并没有让 DefWindowProc 去最小化/关闭窗口；
         * 在 Linux 上它们本来就是普通 QML 按钮。所以三个平台都交给这里调 Qt 的
         * Window 接口来操作窗口，行为才是一致的。
         */
        CaptionButton {
            id: closeButton
            anchors.right: parent.right
            height: titleBar.height
            kind: "close"
            glyphColor: root.captionForeground
            visible: !root.nativeCaptionButtons
            /* 不是 exit()：走正常的关窗流程，Qt.quitOnLastWindowClosed 会去退出事件循环 */
            onClicked: root.close()
        }

        CaptionButton {
            id: maximizeButton
            anchors.right: closeButton.left
            height: titleBar.height
            kind: "maximize"
            /* 这个绑定就是按钮图标在"最大化"和"还原"之间切换的原因。 */
            maximized: root.visibility === Window.Maximized
            glyphColor: root.captionForeground
            visible: !root.nativeCaptionButtons
            onClicked: {
                if (maximized)
                    root.showNormal()
                else
                    root.showMaximized()
            }
        }

        CaptionButton {
            id: minimizeButton
            anchors.right: maximizeButton.left
            height: titleBar.height
            kind: "minimize"
            glyphColor: root.captionForeground
            visible: !root.nativeCaptionButtons
            onClicked: root.showMinimized()
        }
    }

    /* ---------------- 设置页（实现在 SettingsPage.qml） + Ctrl+D 灌示例弹幕 ---------------- */

    /*
     * 【临时测试入口】Ctrl+D：往弹幕层灌一批示例弹幕（对应参考的 danmakuGenerator，
     * 那边是 examples 里手动调用的工具）。真实的弹幕数据源接进来以后这条可以删。
     */
    Shortcut {
        sequence: "Ctrl+D"
        onActivated: {
            var dur = player.duration > 0 ? player.duration / 1000 : 120
            danmakuLayer.generateSample(400, dur)
        }
    }

    /*
     * 设置页本体。**实现在 SettingsPage.qml**（本轮从那一段搬出去的），这里只是一个实例。
     *
     * 【用户说"Main.qml 里那份不要删"】—— 那份指的是"播放器窗口里那个设置页"，不是
     * "必须把这几百行留在本文件里"。所以这里是**实例化**、不是删除：齿轮（控制栏右边那颗）、
     * 顶栏右上角那个"?"、openSettings() / closeSettings() 全部照旧可用，只是实现和
     * 首页（HomeWindow.qml）共用同一份，不会出现"改了一边忘了另一边"。
     *
     * 顺带解决了一件老问题：原来那两个内联组件（SettingSwitch / SettingChoices）写在
     * 本文件里，只有这个窗口能用；首页那个"设置"按钮点了是**没有任何反应**的
     * （HomeWindow 只发了个没人接的 settingsRequested 信号）。现在两个窗口共用同一份。
     *
     * 位置：铺满窗口（原来那个遮罩就是 anchors.fill: parent），层级由 SettingsPage 内部给（z:10）。
     *
     * 【player 必须写 root.player】引用的是根对象上那个只读别名，不是本行左边的属性名 ——
     * 同名自引用是这棵树里踩过最多的坑（见 HANDOVER-TODO.md 第四节第 1 条：
     * `danmaku: danmaku` 右边会被解析成组件自己的属性，初值 null）。
     */
    SettingsPage {
        id: settingsPage

        anchors.fill: parent
        player: root.player
    }

    /* ---------------- 诊断信息（排查性能问题先看这里） ---------------- */
    Column {
        anchors.left: parent.left
        /*
         * 【为什么不再锚 titleBar.bottom】顶部栏现在是"鼠标进顶部才浮出来"的，锚在它下面就
         * 意味着这几行字会跟着一起上上下下地跳。所以固定锚在窗口顶端，再留出顶部栏那么高
         * 的余量：顶部栏露出来时两者不打架，收起时也只是整体低 48px（全屏同理 ——
         * 那正好让开了 PlayerTopBar 左上角的标题，原来两者是叠在一起的）。
         */
        anchors.top: parent.top
        anchors.leftMargin: 12
        anchors.topMargin: 48
        spacing: 2

        /* 设置页里可以关掉。 */
        visible: root.showDiagnostics

        /*
         * 这几行的颜色要跟着底色走：没选片子时压在 windowBg 上（用主题色），
         * 选了片子之后窗口是黑的，主题里那套深色字看不清，换成 onVideoText（见 QtPlayerTheme）。
         * 所以这里不直接写 diagText，而是按"有没有片源"在这两个色之间切
         * （判据就是根对象上那个 hasSource，顶部栏换色用的是同一个）。
         */
        readonly property color textColor: root.hasSource ? QtPlayerTheme.onVideoText
                                                         : QtPlayerTheme.diagText

        /*
         * 零拷贝这一行。
         *
         * 【为什么要分三种写法】`player.zeroCopy` 是
         *     m_hardwareDecoding && m_textureBackend && m_textureBackend->isZeroCopy()
         * 三个条件相与（见 CicadaPlayerItem.cpp:628）。原来不管哪一条不成立都显示
         * "零拷贝: 否（CPU 回退）"，于是：
         *   * 用户自己把「硬件解码（硬解）」关掉了 —— 那是他自己关的，不是"回退"；
         *   * 场景图还没初始化（textureBackend 还是 null）—— 那时**根本没探测**，也不是回退；
         * 两种都被说成"CPU 回退"，看着就像程序自己坏了。现在分开写清楚。
         * 【真正的探测失败】backend 那一栏会显示 CPU 回退的那个后端名，
         * 日志里还有一行 `zero-copy presentation is not available...` 或
         * `the video processor cannot take input format %d` 说明具体原因。
         */
        readonly property string zeroCopyText: {
            if (player.zeroCopy)
                return "零拷贝: 是"
            if (!player.hardwareDecoding)
                return "零拷贝: 否（硬解已关）"
            if (player.backend === "none")
                return "零拷贝: 待探测（场景图未就绪）"
            return "零拷贝: 否（CPU 回退）"
        }

        Text {
            color: player.zeroCopy ? QtPlayerTheme.okText : QtPlayerTheme.warnText
            font.pixelSize: 12
            text: parent.zeroCopyText + " | " + player.backend
        }

        Text {
            color: parent.textColor
            font.pixelSize: 12
            text: "文件: " + sourceName()
        }

        /*
         * 解码方式（诊断用这一行）。
         *
         * 【为什么不显示 player.device 了】那是**设备描述**（排查零拷贝用的实现细节
         * 文案，形如 "Qt D3D11 device (NVIDIA ...)"），不是"解码方式"。用户要的是
         * 市面通用的写法：硬解/软解 + 编码（硬解时再带显卡名），也就是
         * player.decodeMethod，和统计信息面板「解码方式」那一行是**同一个数据源**
         * （CicadaPlayerItem::refreshDecodeMethod）。
         * 没有片源/没有视频流时它是空串，这里按 "--" 显示（面板那边也是这个约定）。
         */
        Text {
            color: parent.textColor
            font.pixelSize: 12
            text: "解码方式: " + (player.decodeMethod !== "" ? player.decodeMethod : "--")
        }

        Text {
            color: parent.textColor
            font.pixelSize: 12
            text: "视频: " + player.videoWidth + "x" + player.videoHeight
                  + " | 状态: " + player.status
        }

        Text {
            visible: player.errorString !== ""
            color: QtPlayerTheme.errorText
            font.pixelSize: 12
            text: "错误: " + player.errorString
        }
    }

    /* ---------------- 选片子：文件对话框 + 拖拽 ---------------- */

    /* 文件对话框的模板/函数在文件靠上的位置（chooseFile 那一带），这里只放拖拽。 */

    /* Ctrl+O 打开文件（换片子不用先去点按钮）。 */
    Shortcut {
        sequence: "Ctrl+O"
        onActivated: root.chooseFile()
    }

    /*
     * 把文件拖进窗口直接播。
     *
     * 铺满整个窗口是安全的，不会抢鼠标事件：拖拽事件只送给 acceptDrops 为真的元素，
     * 而普通 Item（包括 DropArea 自己）默认**不接受任何鼠标按键**
     * （QQuickItem::acceptedMouseButtons() 默认是 NoButton），只有 MouseArea 这类
     * 显式声明的才收。所以控制条上的按钮/滑条照常工作。多选时只取第一个文件。
     */
    DropArea {
        id: dropArea
        anchors.fill: parent
        onDropped: (drop) => {
            if (drop.hasUrls && drop.urls.length > 0) {
                /* 明确接受这次拖放：不写这一句时，拖放源（资源管理器）拿到的
                   dropEffect 可能是"没接受"，表现就是"松手了但什么都没发生"。 */
                drop.acceptProposedAction()
                root.playFile(drop.urls[0])
            }
        }
    }
}
