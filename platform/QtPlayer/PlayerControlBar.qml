// ===========================================================================
// 播放器控制栏（压在画面上的那一层）—— 重写版
//
// 界面按参考实现 front/hili-player/packages/player 复刻，数值全部来自它的 SCSS
// （注释里标了出处）；代码是 Qt/QML 写的，不是抄 TS。
//
// 结构：
//   ┌───────────────────────────────────────────────────────────────┐
//   │                        （画面）                                │
//   │  ┈┈┈ 遮罩：参考的 control-mask.png（原图 1xN，取最下面 100/120px）┈│
//   │  ────────────── 进度条（bottom:44px，两侧 padding 12px）────── │
//   │  ▶ 00:00 / 01:23    清晰度 倍速 🔊 ⚙ ⛶   （35px 高，贴右）     │
//   └───────────────────────────────────────────────────────────────┘
//
// 这一版针对上一版实测出来的问题逐条改（每条都写进注释，别再犯）：
//   1. 图标被裁：Qt 的 LottieAnimation 按**原始画布**画进 item，不自动缩放。
//      各图标画布不同（play/pause 28、volume/settings/fullscreen 88、Thumb 18），
//      所以每个 LottieIcon 都要给对 canvasWidth/canvasHeight。
//   2. 菜单位置乱：菜单必须锚到**自己那颗按钮**上（bottom:41px 是相对按钮底边的），
//      上一版锚在一个 0x0 的中间对象上，于是三个菜单都叠在同一处。
//   3. 菜单关不掉：改成"移入开、移出 300ms 后关"（参考 Controls.handleMenuAnimation
//      就是 300ms 延迟），并且**点中菜单项立刻关**；鼠标从按钮走到浮层要跨 19px
//      空隙，靠这 300ms 兜住。
//   4. 全屏右组不贴右：右组从**右边**串起来（fullscreen ← setting ← volume ← rate ← quality）。
//   5. 进度乱跑：seek 是异步的，松手后先停在目标位置（pendingSeekMs），等 position
//      到位再放开，否则会"跳回旧位置再跳过去"。
//   6. 三角不出现：参考是 `.player-progress-area:hover` 就显示上下三角（不只是拖动）。
//   7. 圆点：参考 CSS 写的是 20x20 的 .player-progress-thumb、里面 18x18 的图标，
//      素材就是 assets/lottie-icon/Thumb-animation.json（画布 18x18）—— 用真素材。
//
// 响应式（对应参考的媒体查询 + [data-screen]）见下面那组 readonly 属性。
// ===========================================================================
import QtQuick
/*
 * 浮层菜单用 QtQuick.Templates 的 Popup（只有浮层这套机制，不引入任何控件样式依赖）。
 * 用它是因为浮层出现时**独占指针事件**，下面的按钮/进度条收不到 —— 从根上避免
 * "菜单和按钮互相抢 hover"导致的一串问题（菜单自己消失、控制栏闪烁）。
 */
import QtQuick.Templates as T
/* 发送栏的输入框用 Qt 自带控件（Basic 样式）自己配颜色，不手画光标/选区那一套 */
import QtQuick.Controls.Basic
/*
 * 弹幕开关/齿轮的图标是项目里的 SVG 原件（白色底，改不了 fill），
 * 要按参考上色只能整颗着色 —— 这里必须显式 import，否则会报
 * "MultiEffect is not a type"，那是致命错误：本文件加载失败 → 模块里没有 Main → 程序退出。
 */
import QtQuick.Effects

Item {
    id: bar

    /* ---- 对外接口 ---- */

    /* 播放器组件（CicadaPlayerItem） */
    property var player: null

    /* 弹幕层（DanmakuLayer）：底部中间那个弹幕开关和设置面板作用在它身上 */
    property var danmaku: null

    /* 弹幕开关（对应参考 SendBar 的 .player-dm-switch） */
    property bool danmakuEnabled: true

    /*
     * 分段（视点）数据 —— 每项 `{from, to, content}`，from/to 单位是**秒**。
     *
     * 【为什么加在控制栏这一层】左组最后那颗「章节 · xxx」按钮（LeftControls 的
     * viewPointButton）和进度条上的分段横条（ProgressRow.viewPoints）用的是**同一份**数据。
     * 这里开一个入口，下面原样转给 leftGroup —— 应用层（Main.qml）拿到 B 站的进度点之后
     * 往这里一喂即可，LeftControls 那边不用再想办法跨文件取数据。
     *
     * 【默认空数组 = 按钮不存在】空数组或只有 1 段时 LeftControls 里那颗按钮
     * visible 为假、宽度 0（判据 `viewPoints.length > 1`，参考 controls/index.ts:1047-1049），
     * 控制栏的视觉和加这个功能之前**一个像素都不差**。
     *
     * 【没动左组】左组里 [上一个][播放/暂停][下一个][时间] 的顺序、间距、显隐规则一律没改，
     * 视点按钮是追加在**时间之后**的第五项（参考 bottomLeftOrder，LeftControls.ts:350）。
     */
    property var viewPoints: []

    /*
     * 高能进度条（pbp）数据 —— `{step_sec, data: [0~1, …]}`；第 i 个采样点 = 第 i*step_sec 秒。
     * 和 viewPoints 同一个路子：应用层喂进来，这里原样转给进度条那一行（ProgressRow 早就
     * 声明好了这个属性，名字不能改）。默认 null = 没有数据 → ProgressRow 里那条 28px 的曲线区
     * opacity 恒为 0、enabled 恒为假，控制栏和加这个功能之前一模一样。
     */
    property var pbpData: null

    /*
     * 悬停气泡的位置帧（videoshot/preview.bin 切出来的 data URL 数组；arr[0] 是空占位）。
     * 和上面两条同一个路子：应用层喂进来、这里原样转给进度条那一行（ProgressRow.previewFrames）。
     * 空数组 = 接口没提供 → 气泡退回 CaptureScreen 的"当前画面"。
     */
    property var previewFrames: []

    /* 高能进度条的开关（设置页那个复选框的直连版；参考里它是假的，见 ProgressRow 里的说明）。
       默认 true，但**没有数据时不会显示任何东西** —— 亮不亮由 ProgressRow 的 active 一起判。 */
    property bool highlightEnabled: true

    /*
     * 发送栏：弹幕类型（1 滚动 / 2 顶部 / 3 底部，对应参考的三种 type）与发送动作。
     * sendDanmakuText 走引擎的 sendDanmaku(text, options)：uid 传 1（本人弹幕，会画白框）。
     */
    property int dmType: 1
    /* 类型弹出列表是否展开（鼠标移到类型图标上展开、移开 120ms 收起，和浮层菜单一套节奏） */
    property bool dmTypePanelOpen: false

    /* Esc 收起类型面板（等价于浮层的 closePolicy: CloseOnEscape） */
    Shortcut {
        sequence: "Escape"
        enabled: bar.dmTypePanelOpen
        onActivated: bar.dmTypePanelOpen = false
    }
    readonly property string dmTypeLabel: dmType === 2 ? qsTr("顶部")
                                                       : (dmType === 3 ? qsTr("底部") : qsTr("滚动"))


    /* 是否全屏（全屏按钮状态 + 整套全屏尺寸都跟着它） */
    property bool fullscreen: false

    /* 是否显示（自动隐藏用，见 Main.qml 的 hideTimer / kickAutoHide） */
    property bool shown: true

    /*
     * 外面有没有"盖住整个窗口的浮层"开着（设置页 / 文件对话框）。
     *
     * 【为什么必须有这个属性】这一层下面有一处 `cursorShape` 原来是这么写的：
     *     cursorShape: (!bar.shown && !settingsScrim.visible) ? Qt.BlankCursor : Qt.ArrowCursor
     * 那是我把这段界面从 Main.qml 拆出来时**忘了改的一处**：`settingsScrim` 是 Main.qml 里
     * 那个遮罩的 **id**，而 QML 的 id 是**按文档**的 —— 这个文件里根本没有这个名字，
     * 于是这句绑定每次求值都抛 ReferenceError，`cursorShape` 永远停在默认值上
     * （表现：控制栏收起之后鼠标指针该藏起来却没藏）。
     * 现在改成由外面喂进来（PlayerView 把它的 overlayOpen 转过来），不再跨文件摸 id。
     */
    property bool overlayOpen: false

    /* 鼠标是否停在本控制栏上（保留给需要的地方用；判断"要不要自动收起"请用 pointerInside） */
    readonly property alias hovered: barHover.hovered

    /*
     * 光标是否停在控制层的**任一可交互区域**上（按钮、进度条、菜单、音量滑杆……）。
     *
     * 做法是**实时询问**每个 MouseArea 自己的 containsMouse（或起来），不用事件计数：
     * 计数会在"某个区域自己 visible 变假"时收不到 hover 离开事件，于是永远 > 0 ——
     * 表现就是"以为光标还在控制层上"→ 从底部移出窗口也不隐藏、鼠标停在控制栏上反而隐藏。
     * containsMouse 是 Qt 自己维护的属性，不会有这个问题。
     *
     * 菜单里的单选项（清晰度项/倍速项）在 Repeater 的 delegate 作用域里，这里引用不到；
     * 它们本来就位于对应菜单区域之内，光标在它们上面时不会产生"进入/离开"跳变，
     * 所以不影响隐藏时机。
     *
     * 根上的 HoverHandler（hovered）不能当判据：按钮上那些 hoverEnabled 的 MouseArea
     * 会把 hover 吃掉，父级 HoverHandler 收不到（实测过）。
     */
    readonly property bool pointerInside: containerHover.containsMouse
                                          || progressRow.containsMouse      /* 进度条区域的判据（ProgressRow 透出的 alias） */
                                          /* pbp 曲线那一条 28px 在进度条**上方**，不属于上面那个热区：
                                             不带上它的话，光标一碰到曲线就被判成"离开控制层"→ 控制栏收起 →
                                             曲线跟着消失（就是上面 125-131 行记过的那个坑）。
                                             没有数据时这个 alias 恒为 false，等于没加。 */
                                          || progressRow.curveContainsMouse
                                          || timeEditing                    /* 时间输入框开着：控制栏不能自动收起（见 timeEditing 的说明）*/
                                          || leftGroup.timeHovered          /* 时间那一格自己的 hover（它的 MouseArea 会抢走 hover）*/
                                          /* 分段（视点）按钮 + 它的 hover 面板：同 timeHovered 的道理，
                                             那两块 MouseArea 也会把 hover 拿走；漏了就是"鼠标停在视点按钮上
                                             整条控制栏自己收起"（面板开着、指针在面板上时同理）。
                                             来源：LeftControls.viewPointHovered。 */
                                          || leftGroup.viewPointHovered
                                          || leftGroup.pointerInside          /* 播放按钮的判据（LeftControls 透出的 alias） */
                                          /* 右边那一组（七颗按钮 + 五个浮层）：判据由 RightControls 汇总。 */
                                          || rightGroup.pointerInside
                                          /* 底部中间的弹幕开关 / 设置齿轮 + 弹幕设置浮层。
                                             漏了这两块的话，鼠标移上去会被判成"离开控制栏"，
                                             整条控制栏直接隐藏（就是那条"移上去控制栏消失"的现象）。 */
                                          /* 弹幕那一块（开关/齿轮+面板/发送框/类型浮层）：七个区域的
                                             判据现在由 DanmakuBar 自己汇总成一条 —— 少一处就会被
                                             判成离开控制栏、整条控制栏收起（这个坑踩过三次）。 */
                                          || dmBar.pointerInside

    signal fullscreenRequested()
    /* 上一个 / 下一个视频（左组那两颗按钮；由应用层换源，复用同一个播放器窗口） */
    signal previousRequested()
    signal nextRequested()
    /* 点齿轮：打开本应用自己的设置页（主题 / 硬解 / 诊断，见 Main.qml 的 openSettings()） */
    signal settingsRequested()

    /* 左组那颗「分段（视点）」按钮的出口：把 seek 转给播放器。
       参数已经是毫秒（LeftControls 里做过秒→毫秒的换算，见那边 viewPointChoose 的说明）。
       应用层目前没有视点数据（leftGroup.viewPoints 默认空数组）→ 按钮不出现、这条不会触发。
       注意：**没有**改左组任何顺序/间距，只是接一个信号。

       【★ 必须自己声明这个信号 —— 这就是"点视频卡片打不开播放窗口"的第二个根因 ★】
       `onViewPointSelected` 这种隐式处理器只能挂在"声明了该信号的对象"上：信号本来只在
       LeftControls 的根对象上（controls/LeftControls.qml:87），上一版却在控制栏根层级直接写
       处理器，QML 报
           Cannot assign to non-existent property "onViewPointSelected"
       然后连锁 PlayerControlBar → PlayerView → Main 全部 unavailable → 点卡片建不出窗口
       （实测 stderr 就是这么打的）。这里声明同名信号后，LeftControls 实例里的
       `onViewPointSelected` 负责把事件转发上来，再由下面这个处理器 seek。 */
    signal viewPointSelected(real timeMs)

    onViewPointSelected: function(timeMs) {
        if (player)
            player.seek(timeMs)
    }

    /*
     * 选集（全屏时右组那颗「选集」按钮）：
     *   episodes           —— 集数列表，每项至少要有 title（可选 cid / url），应用层喂进来
     *   currentEpisodeIndex —— 当前正在播的是第几集（面板里那行会变主题色 + 三柱图标）
     *   episodeSelected     —— 点了某一集；参考里这件事**没有实现**（eplistChange 只声明没发射），
     *                          所以出口由我们定：交给应用层去换片 / 换页
     * 空列表时那颗按钮不弹面板（和清晰度按钮的 hasQuality 一个处理）。
     */
    property var episodes: []
    property int currentEpisodeIndex: -1
    signal episodeSelected(int index, var episode)

    /*
     * 还有没有上一条 / 下一条 —— 左组那两颗按钮的**唯一显隐判据**。
     *
     * 由 `VideoLibrary.hasPrevious / hasNext` 一路喂进来（C++ 那边就是为这件事准备的，
     * 见 VideoLibrary.h 里那两行 Q_PROPERTY 的注释："控制栏那两颗按钮的显示判据"）。
     *
     * 【为什么不在这里自己算】原来 LeftControls 里读的是
     *   `currentEpisodeIndex > 0` 和 `currentEpisodeIndex + 1 < episodes.length`，
     * 那是**在 QML 里重算了一遍** C++ 已经有的东西：两处判据一旦不一致，
     * 就会出现"第一条视频还显示上一个"或者"最后一条还显示下一个"。
     * 现在只有一处真相：库自己算，库变了这里跟着变。
     */
    property bool hasPrevious: false
    property bool hasNext: false

    /*
     * 当前"显示位置"（毫秒）：平时是播放位置，拖动进度条时是拖动到的位置。
     * 左组那行 `00:12 / 03:45` 读它。
     *
     * 【为什么必须由控制栏转一手】ProgressRow 的 id（`progressRow`）只在**本文件**这个
     * 文档里有效。LeftControls.qml 是另一个文档，它里面原来直接写
     *     text: bar.formatTime(progressRow.displayPositionMs)
     * —— 那个 `progressRow` 在它自己的作用域里根本不存在，每次求值都抛
     * `ReferenceError: progressRow is not defined`，于是**那行时间永远是空的/不更新**
     * （和之前 settingsScrim 那个坑同一类：QML 的 id 是**按文档**的，跨文件摸不到）。
     * ProgressRow.qml 早就把 displayPositionMs 用 alias 透出来了（那个文件 31-32 行的注释
     * 写的就是"左边那组的时间要读它"），这里接上即可。
     */
    readonly property real displayPositionMs: progressRow.displayPositionMs

    /*
     * 时间输入框（点时间那一格跳转）的状态，从 LeftControls 透上来：
     *   * `timeEditing` —— 输入框开着没有。控制栏的 pointerInside 要用它（开着的时候
     *     不能让控制栏自动收起，否则用户打字打到一半整条控制栏连输入框一起消失）；
     *     PlayerView 里那层"点外面就关"也用它决定要不要显示。
     *   * `commitTimeEdit()` —— 提交（点外面时由那一层调）。
     *   * `timeInputRectIn(item)` —— 输入框在给定坐标系里的矩形，那一层用它判断
     *     "这一下按在不在输入框里"。
     */
    readonly property alias timeEditing: leftGroup.timeEditing

    function commitTimeEdit() {
        leftGroup.commitTimeEdit()
    }

    function timeInputRectIn(item) {
        return leftGroup.seekInputRectIn(item)
    }

    HoverHandler {
        id: barHover
    }

    readonly property bool ready: player !== null && player.status !== 0 /* Null */

    /* -----------------------------------------------------------------------
     * 响应式数值（参考里是 screen-style mixin + min/max-width 媒体查询）
     *   index.scss:4894-4957 全屏那一档；4895/5085 两档宽度查询
     * 普通窗口 / 全屏：遮罩 100→**112**、进度行 bottom 44→**60**、按钮行 35→45、
     * 按钮 36x22→54x43、图标 22→28、时间 12→14(左缩进 16)、清晰度 14→16、
     * 菜单底边距 41→74、左组最小宽 0→316(<=1366 时 273)、右组 0→370、整层 55→**69**
     *
     * 三处对参考的**有意偏离**（用户要求，其余全按参考）：
     *   * 全屏进度行 bottom：参考 68，这里 60（-8），见 progressBottom
     *   * 全屏遮罩高度：参考 120，这里 112（-8），见 maskHeight
     *   * 底部余量：参考 非全屏 0 / 全屏 8，这里 1 / 4（两种模式都让图标垂直居中），见 entitySlack
     * ----------------------------------------------------------------------- */
    readonly property bool narrowEnoughForSmallLeft: width <= 1366

    /*
     * 遮罩（control-mask.png）高度：参考全屏是 120（.player-control-mask height:120px，
     * index.scss:4947-4949），按用户要求 **-8 = 112** —— 遮罩底边贴播放区底边不动，
     * 高度少 8px 就是"整块跟着内容一起下移 8px"（渐变顶端同步降下来）。
     * 非全屏没有那 8px 的变化，所以还是 100。
     */
    readonly property int maskHeight: fullscreen ? 112 : QtPlayerTheme.controlMaskHeight
    /*
     * 全屏进度行底边距：参考是 68（.player-progress bottom，index.scss:4894 那档），
     * 按用户要求 **-8 = 60** —— 和整层去掉的那 8px 同步下移，
     * 按钮↔进度条之间那段间距保持不变。
     */
    readonly property int progressBottom: fullscreen ? 60 : QtPlayerTheme.progressRowBottom
    readonly property int bottomRowHeight: fullscreen ? 45 : QtPlayerTheme.controlBottomHeight
    readonly property int menuBottom: fullscreen ? 74 : QtPlayerTheme.menuBottom
    readonly property int btnWidth: fullscreen ? 54 : QtPlayerTheme.ctrlBtnWidth
    readonly property int btnHeight: fullscreen ? 43 : QtPlayerTheme.ctrlBtnHeight
    readonly property int iconSize: fullscreen ? 28 : QtPlayerTheme.ctrlIconSize
    /*
     * 按钮里的"行盒"高度 —— 图标和文字都在这条行盒里居中，而不是在整个按钮盒里居中。
     *   非全屏：.player-ctrl-btn 是 height:22 / line-height:22（index.scss:313-314）
     *           → 行盒高 = 盒高，图标居中 = 居盒中，两者等价；
     *   全屏：  height:43 / line-height:32（index.scss:4969-4970）
     *           → 行盒只有 32px 且贴在盒顶，28px 的图标在**行盒**里居中
     *             （.player-ctrl-btn-icon > .common-svg-icon { height:28px; vertical-align:middle }，
     *              index.scss:4973-4975）→ 图标顶边 = 盒顶 + (32-28)/2 = +2px。
     * 少了这一条，全屏时图标会低 5.5px，按钮到进度条的距离就比参考大。
     */
    readonly property int btnLineHeight: fullscreen ? 32 : QtPlayerTheme.controlLineHeight

    /*
     * 时间那一格的字号（**两档，按参考**）：
     *   非全屏 12px（.player-ctrl-time { font-size:12px }，index.scss:326-330）
     *   全屏   14px（.player-ctrl-time-label { font-size:14px }，index.scss:4978-4982）
     *
     * 【用户要求：时间文字和输入框必须是同一档】"非全屏还是 12、只有全屏才 14，
     * 输入框也一样" —— 所以跳转输入框（LeftControls 里的 seekInput）也读这个值。
     * 别再把它改成"统一 14"：那样非全屏的时间会比参考大一档，用户不要。
     */
    readonly property int timeFontSize: fullscreen ? 14 : QtPlayerTheme.ctrlTimeFontSize
    readonly property int qualityFontSize: fullscreen ? 16 : QtPlayerTheme.qualityResultFontSize
    readonly property int timeIndent: fullscreen ? 16 : 0
    readonly property int leftGroupMinWidth: fullscreen ? (narrowEnoughForSmallLeft ? 273 : 316) : 0
    readonly property int rightGroupMinWidth: fullscreen ? 370 : 0

    /*
     * 底部余量：把图标放在"播放区底边 ↔ 进度条轨道底边"**正中间**量出来的值。
     * （图标底边离按钮行底边有个固定差：行高 − 行盒高 + 图标在行盒里居中的偏移）
     *
     *   非全屏：轨道底边 = progressBottom 44 + 进度行 padding-bottom 6 = 50；图标 22px
     *           → 居中时图标底边 = 14；图标底边离行底边 = 35 − 22 = 13
     *           → 行底边 = 14 − 13 = 1  → 余量 **1**
     *   全屏：  轨道底边 = 60 + 6 = 66；图标 28px
     *           → 居中时图标底边 = 19；图标底边离行底边 = 45 − 32 + 2 = 15
     *           → 行底边 = 19 − 15 = 4  → 余量 **4**
     *
     * 参考原值：非全屏 0（图标离底边比离轨道近 2px）、全屏 8（相对 60 的轨道则近 8px）——
     * 用户要求两种模式都垂直居中，所以统一改成上面量出来的 1 / 4。
     */
    readonly property int entitySlack: fullscreen ? 4 : 1

    /*
     * 控制层整块高度 = 按钮行高 + 上面那段 20px（= .player-control-entity 的内容高）+ 底部余量
     * 全屏 45+20+4 = 69、非全屏 35+20+1 = 56。
     *
     * 参考全屏那档写的是 height:73（内容 65 + 底部 8）、非全屏是 auto（= 55）。
     */
    readonly property int totalHeight: bottomRowHeight + QtPlayerTheme.controlBottomMarginTop + entitySlack

    implicitHeight: totalHeight

    /*
     * -----------------------------------------------------------------------
     * 控制容器整块的 hover 区域（**最先声明 = 在最底层**）
     *
     * 参考里控制层是一个整体容器：鼠标移到容器里的**任何位置**（按钮之间的空隙、
     * 时间文字与清晰度之间、按钮行的上下、进度条上下方）都算"在控制栏上"，都不会隐藏；
     * 只有移到容器外面才隐藏。
     *
     * 我之前只盯着"按钮/进度条"这些可交互区域，空隙处没有任何 hover 目标 →
     * 被判成"光标已经离开" → 一移上去就收起（也就是"移到 control 非按钮区域就隐藏"
     * 和"按钮上想选倍速却先隐藏了"）。
     *
     * 放最底层的原因：光标压在按钮/进度条上时，是它们自己的 MouseArea 接走 hover，
     * 这一层收不到；空隙处才轮到它。两者都通过 pointerInside 反映出来，合起来就是
     * "整块容器"的范围。
     * -----------------------------------------------------------------------
     */
    MouseArea {
        id: containerHover

        anchors.fill: parent
        hoverEnabled: true
        /*
         * 既判 hover、也**吃掉左键点击**。
         *
         * 参考里控制层是压在画面之上的普通 div，点它的空白处不会穿透到画面
         * （只有点 .player-video-perch = 画面才切播放/暂停）。
         * 这里若只判 hover 而不吃点击，点控制栏空白就会穿到下面的活动层 →
         * 变成"点哪里都暂停/继续"。
         *
         * 按钮、进度条、菜单都在这一层**上面**（各自的 MouseArea 先接到事件），
         * 所以它们的点击/拖动完全不受影响。
         */
        acceptedButtons: Qt.LeftButton

        /*
         * 控制层收起时这一层要**失效**：否则它一直占着底部这 55px 的 hover，
         * 也一直按自己的指针形状画箭头 —— 结果就是"控制栏藏了、鼠标指针却还在"。
         * 对应参考里 hideControls() 给容器加 `state-no-cursor`（整块区域 cursor: none）。
         */
        enabled: bar.shown

        /* 和画面层用同一套"藏指针"判断（控制栏收起、且没有浮层盖着） */
        cursorShape: (!bar.shown && !bar.overlayOpen)
                     ? Qt.BlankCursor : Qt.ArrowCursor
    }

    /*
     * 显隐动画：**这一层自己不淡出**，三块各自按参考的曲线淡入淡出 + 从下往上滑进来。
     *
     * 参考里这就是两套不同的逻辑（controls.scss:1192-1201 + 每块自己的 transition）：
     *   * 控制层（.player-control-top / .player-control-bottom，由 [data-shadow-show] 控制）
     *       - 进度条那一行  opacity 0.2s ease-out   （controls.scss:45-46）
     *       - 按钮那一行    all     0.2s ease-out   （controls.scss:339-340）
     *       - 遮罩          opacity 0.2s ease-in-out（controls.scss:16-17）
     *   * 影条（.player-shadow-progress-area，控制层收起时才出现）
     *       - opacity 0.4s ease-in                  （controls.scss:1072-1073）
     *
     * 所以这里不能把 opacity 挂在整层上：影条要跟三块**反过来**动。见下面每块自己的
     * Behavior，以及底部的 shadowProgress。
     */

    /*
     * 毫秒 -> 时间文字（本播放器自己的写法，和参考的 formatTime 无关）。
     *
     * 【形状按"片长"决定，两侧统一】
     *   片长 < 1 小时： MM:SS      —— 00:03 / 05:11
     *   片长 ≥ 1 小时： H:MM:SS    —— 0:00:02 / 1:06:44（小时位**不补零**，分、秒各两位）
     *
     * 【为什么不能只看"这一侧的值"】参考里长片的**当前时间也带小时位**（0:00:02）：
     * 两边形状必须一样，否则会出现 "0:00:02 / 66:44" 这种拧巴的东西。
     *
     * 【旧版的 bug】原来 minutes = floor(总秒/60)、没有小时位，所以 1 小时以上会显示成
     * "66:44 / 1:06:44" 这种（时长侧 4004 秒 → 66:44）。用户报的就是这个。
     *
     * 【兜底】流媒体拿不到总时长（duration 为 0）时，如果**这一侧的值**已经过了一小时，
     * 也切成带小时位的那一档，免得显示成 66:44。
     */
    function formatTime(ms) {
        const total = (player !== null && player.duration > 0) ? player.duration : 0
        const value = ms > 0 ? ms : 0

        const totalSeconds = Math.floor(value / 1000)
        const hours = Math.floor(totalSeconds / 3600)
        const minutes = Math.floor((totalSeconds % 3600) / 60)
        const seconds = totalSeconds % 60

        if (total >= 3600000 || value >= 3600000)
            return hours + ":" + twoDigits(minutes) + ":" + twoDigits(seconds)

        /* 一小时内：维持在"分:秒"（分和秒都补零，参考里的 05:11 就是这个样子） */
        return twoDigits(hours * 60 + minutes) + ":" + twoDigits(seconds)
    }

    /* 补零到两位。只给分、秒用 —— **小时位不补零**（参考是 1:06:44，不是 01:06:44）。 */
    function twoDigits(n) {
        return (n < 10 ? "0" : "") + n
    }

    function ratioOf(ms) {
        var d = player ? player.duration : 0
        return d > 0 ? Math.max(0, Math.min(1, ms / d)) : 0
    }

    /*
     * 关掉三个浮层菜单（音量的那个浮层 id 是 volumeMenu，另外两个是 rateMenu / qualityMenu）。
     * Main.qml 在两处调它：光标回到画面区（等于离开了控制层）、以及要收起控制栏的时候。
     * 【踩过的坑】这个函数是在把控制栏改成 Popup 浮层那轮加进 Main.qml 的，后来从构建目录
     * 恢复文件时丢了实现，于是运行期一直报 "Property 'closeMenus' ... is not a function"。
     */
function closeMenus() {
        /* 浮层都住在各自的子组件里，这里只负责转达：
           右边那一组（音量/倍速/清晰度/字幕/选集）→ rightGroup.closeMenus()
           底部弹幕那一块（设置面板/类型面板）        → dmBar.closePanels() */
        rightGroup.closeMenus()
        dmBar.closePanels()
    }

    /* -----------------------------------------------------------------------
     * 遮罩：参考 .player-control-mask（controls.scss:8-20）
     *   height:100px；background:url(那张 PNG) repeat-x bottom
     * 关键在 **bottom**：原图（1px 宽、比容器高）底边对齐容器底边，超出的上半部分被裁掉，
     * 所以**非全屏**看到的 = 原图最下面 100px。
     *
     * 【全屏那档 (index.scss:4947 把 height 改成 120px) 怎么理解】
     * 全屏不是"接着往下再多裁 20px"，而是把**非全屏看到的那一段整体放大**到 120px
     * （渐变形状一样，只是拉长）—— 直接多裁会让渐变比例变掉、观感不对。
     * 所以这里的做法是：以"非全屏可见的 100px"为基准做缩放，缩放比 = maskHeight / 100，
     * 再套一个 maskHeight 高的裁剪容器、底边对齐。这样：
     *   非全屏：缩放比 1   → 原图原尺寸，露底部 100px（和参考一致）；
     *   全屏：  缩放比 1.2 → 整个底部 100px 那段放大到 120px（就是"非全屏那版的缩放"）。
     *
     * 【别再踩的坑】不要用 Image.sourceClipRect 去"取底部一段"：一旦设了它，
     * Image 的 implicitHeight 会变成**裁剪后**的高度，`y = implicitHeight - height` 恒为 0，
     * 结果取到的是**顶部**那一段（渐变全反）。
     * ----------------------------------------------------------------------- */
    Item {
        id: maskClip

        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }
        height: bar.maskHeight
        clip: true

        /*
         * 遮罩自己的淡入淡出：参考 .player-control-mask 是
         * `transition: opacity 0.2s ease-in-out`（controls.scss:16-17）。
         */
        opacity: bar.shown ? 1 : 0
        visible: opacity > 0

        Behavior on opacity {
            NumberAnimation {
                duration: 200
                easing.type: Easing.InOutQuad
            }
        }

        Image {
            anchors {
                left: parent.left
                right: parent.right
                bottom: parent.bottom
            }
            /*
             * 高度 = 原图原始高度 × 缩放比。缩放比以"非全屏可见的 100px"为基准：
             *   非全屏 100/100 = 1   全屏 120/100 = 1.2
             * 横向拉伸 = CSS 的 repeat-x（原图只有 1px 宽）。
             */
            height: implicitHeight * (bar.maskHeight / QtPlayerTheme.controlMaskHeight)
            source: "assets/images/control-mask.png"
            fillMode: Image.Stretch
            smooth: true
            enabled: false
        }
    }

    /* =======================================================================
     * 进度条
     * ======================================================================= */
    /* ---------------- 进度条那一行（拆到 controls/ProgressRow.qml） ---------------- */
    /*
     * 轨道/圆点/拖动指示/悬停预览 + 影子进度全在那一个文件里。
     * 【id 故意还叫 progressRow】本文件里 `progressRow.displayPositionMs` 这类引用因此不用改；
     * 它自己把 displayPositionMs / pointerInside 用 alias 透出来。
     * anchors.fill: parent —— 里面那块的锚点是相对本组件的，铺满控制栏后几何与拆分前一致。
     */
    ProgressRow {
        id: progressRow

        anchors.fill: parent
        bar: bar
        /*
         * 【不能写 player: player】右边那个 player 会被解析成**本组件自己**的同名属性
         * （初值 null）→ 绑定成了自引用，组件里拿到的 player 永远是 null。
         * bar 是根对象的 id，用 bar.player 才不会歧义。（danmaku 那一处踩过同一个坑。）
         */
        player: bar.player
        /* 分段（视点）+ 高能进度条（pbp）的数据与开关：应用层 → 本组件 → 进度条那一行。
           三个属性名是 ProgressRow 里声明好的，这里只做"原样转交"（和 leftGroup 那份同一份数据，
           分段横条与左组那颗「章节」按钮因此永远一致）。 */
        viewPoints: bar.viewPoints
        pbpData: bar.pbpData
        highlightEnabled: bar.highlightEnabled
        previewFrames: bar.previewFrames
    }
    Item {
        id: bottomRow

        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
            /* 底部余量：非全屏 1px、全屏 4px（让图标在底边↔进度条之间居中，见 entitySlack） */
            bottomMargin: bar.entitySlack
            leftMargin: QtPlayerTheme.controlPaddingH
            rightMargin: QtPlayerTheme.controlPaddingH
        }
        /*
         * 行高必须显式给：参考 .player-control-bottom 是 `height:35px`（controls.scss:334）。
         * 【踩过的坑】这行一度被注释掉 → 行高变 0 → 里面按 verticalCenter 对齐的内容就
         * 以"播放区底边"为中心，一半掉到外面（"按钮有一部分在窗口外"），
         * 同时离进度条多出十几像素（"中间太宽"）。两个症状同一个原因。
         */
        height: bar.bottomRowHeight

        /* 按钮那一行自己的淡入淡出：.player-control-bottom 是 `all 0.2s ease-out` */
        opacity: bar.shown ? 1 : 0
        visible: opacity > 0

        Behavior on opacity {
            NumberAnimation {
                duration: 200
                easing.type: Easing.OutQuad
            }
        }

        /* ------------------------- 中间：弹幕开关 + 弹幕设置 ------------------------- */
        /*
         * 参考里这块是"弹幕发送栏"（SendBar.ts）：底部中间 34px 高的一行，里面有弹幕开关
         * （.player-dm-switch）、发送输入框、发送按钮、设置齿轮（.player-dm-setting）。
         * 这里先做**开关 + 齿轮 + 设置面板**（DmSetting.ts），发送输入框还没做。
         *
         * 面板位置照 CSS：320x270、底边距齿轮底边 63px、右边缘比齿轮右边缘外扩 105px
         * （dmsetting.scss:1-11 的尺寸 + index.scss:5002-5005 的 bottom/right）。
         * 齿轮 hover 展开面板是参考的行为（SendBar.ts:208/229 加/去 player-dm-setting-show）。
         *
         * 【为什么要有 dmHoverZone 这一层】DOM 里 hover 是按**子树**算的，面板是齿轮的子元素，
         * 所以鼠标进面板不会触发齿轮的 mouseleave；QML 的 hover 只看几何，齿轮和面板中间还
         * 隔着 29px 缝，必须把"齿轮 + 缝 + 面板"合成一块区域来判（和三个浮层菜单一个套路）。
         */
            /* ---------------- 弹幕那一块（拆到 controls/DanmakuBar.qml） ---------------- */
            /*
             * 开关 / 设置齿轮 + 设置面板 / 发送框（胶囊 + 类型 + 发送）/ 类型浮层，全在那一个文件里。
             * 【id 还叫 dmBar】本文件里 dmBar.xxx 这类引用因此不用改；
             * 它把七个区域的 hover 判据汇总成 pointerInside，并透出 closePanels()。
             */
            DanmakuBar {
                id: dmBar

                bar: bar
                /* 【这里原来写的是 danmaku: danmaku —— 自引用，拿到的永远是 null】
                   现象就是"点发送什么都没反应"：发送函数第一步 `if (danmaku === null) return`。
                   同名传递在 QML 里会被解析成本组件自己的属性，必须写成 bar.danmaku。 */
                danmaku: bar.danmaku
                leftGroup: leftGroup
                rightGroup: rightGroup
            }
            /* ---------------- 左边那一组（拆到 controls/LeftControls.qml） ---------------- */
            LeftControls {
                id: leftGroup

                bar: bar
                /* 同 ProgressRow：player: player 是自引用（永远 null），必须 bar.player */
                player: bar.player
                /* 分段（视点）：控制栏这一层的入口原样转给左组那颗「章节 · xxx」按钮
                   （见 bar.viewPoints 的说明；空数组时那颗按钮不出现） */
                viewPoints: bar.viewPoints
                /* 【必须在这里接一手】LeftControls 那颗「章节」按钮点某一段时发的是**它自己**
                   的信号（controls/LeftControls.qml:87 声明）；控制栏根对象上也声明了同名信号
                   （本文件 :192），这里把左组的信号转发上去，根对象上那个
                   `onViewPointSelected` 才能真正收到并 seek。
                   少了这一行：点了没反应（信号发出去了，但没人接）。 */
                onViewPointSelected: function(timeMs) {
                    bar.viewPointSelected(timeMs)
                }
            }
            /* ---------------- 右边那一组（拆到 controls/RightControls.qml） ---------------- */
            /*
             * 清晰度 / 选集 / 倍速 / 字幕 / 音量 / 设置 / 全屏 七颗按钮 + 它们各自的浮层菜单，
             * 全在那一个文件里。【id 还叫 rightGroup】—— dmBar 靠它当右边界锚点，其它引用也不用改。
             */
            RightControls {
                id: rightGroup

                bar: bar
            }
    }
}