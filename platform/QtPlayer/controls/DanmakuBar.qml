// ===========================================================================
// 底部中间的弹幕那一块（从 PlayerControlBar.qml 拆出来，内容**原样搬移**）
//
// 里面是四样东西：弹幕开关、设置齿轮 + 弹幕设置面板（DanmakuSettingPanel 浮层）、
// 发送输入框（胶囊 + 类型按钮 + 发送按钮）、以及类型浮层。
//
// 依赖（都用属性传进来，所以内部 bar.xxx / danmaku.xxx 的写法一个字没改）：
//   bar        —— 控制栏本体（主题数值、menuBottom、dmTypePanelOpen、danmakuEnabled…）
//   danmaku    —— 弹幕层（DanmakuView），设置面板所有控件作用在它身上
//   leftGroup / rightGroup —— 它靠这两个的左右边缘夹住自己（原来就是同一个文件里的兄弟节点）
//
// 对外只多一个 pointerInside：控制栏判"鼠标还在不在控制栏上"要用它
// （原来是把七个区域的 containsMouse 直接写在控制栏那个列表里）。
// ===========================================================================
import QtQuick
import QtQuick.Templates as T
/* 发送框的输入框用 Qt 自带控件（Basic 样式）；MultiEffect 用来给 SVG 着色。
   拆出来时必须一起带过来，少了就是致命错误：本文件加载失败 → 模块里没有 Main → 程序退出。 */
import QtQuick.Controls.Basic
import QtQuick.Effects
/* 相对目录导入：让编辑器（qmlls）按文件系统直接找到根目录的 DanmakuSettingPanel / QtPlayerTheme。 */
import ".."
import QtPlayer

        Item {
            id: dmBar

                /* ---- 从外面传进来的依赖（见文件头说明）---- */
                property var bar: null
                property var danmaku: null
                property var leftGroup: null
                property var rightGroup: null

                /*
                 * 鼠标是不是停在这一块的任意可交互区域上（控制栏的 pointerInside 用它）。
                 * 原来这七行直接写在控制栏那个大表达式里，搬过来之后由本组件自己汇总 ——
                 * 少一处都会被判成"离开控制栏"，整条控制栏直接收起（踩过三次）。
                 */
                readonly property bool pointerInside: dmSwitchArea.containsMouse
                                                      || dmSettingArea.containsMouse
                                                      || dmPanel.opened
                                                      || dmTypeArea.containsMouse
                                                      || dmSendArea.containsMouse
                                                      || dmInput.hovered
                                                      || dmTypePopup.opened

                /* 关掉这一块上开着的浮层（控制栏收起/切页时调） */
                function closePanels() {
                    if (dmPanel.opened)
                        dmPanel.close()

                    if (dmTypePopup.opened)
                        dmTypePopup.close()
                }

                /* ---- 发送：原来在控制栏根对象上（因为它读 dmInput，而 dmInput 在这一块里）---- */
    function sendDanmakuText() {
        /*
         * 【为什么写成这样】拆组件之后这条链出过好几次"点了没反应"：
         *   * 调错对象（bar.sendDanmakuText 不是函数）—— 已修；
         *   * 读不到外面那个 dmType（ReferenceError，函数当场中断、连输入框都不清）—— 已修；
         * 每次都是**静默失败**（QML 里一个异常就把整个函数掐断），所以这里：
         *   * 每个失败分支都打一条 warn 日志（Release 下也看得见），
         *   * 取颜色单独包一层（拿不到就白色，不要因为配色表读不到而整条发不出去），
         *   * sendDanmaku 单独包一层，只有真的发出去了才清空输入框。
         */
        if (danmaku === null) {
            console.warn("[danmaku] 发送失败：弹幕层对象是 null（PlayerControlBar.danmaku 没接上）")
            return
        }

        if (dmInput.text.length === 0) {
            console.warn("[danmaku] 发送失败：输入框是空的")
            return
        }

        var text = dmInput.text
        var color = "#ffffff"

        try {
            var list = dmColorRow.colorList

            if (list && dmColorRow.dmColorIndex >= 0 && dmColorRow.dmColorIndex < list.length)
                color = list[dmColorRow.dmColorIndex]
        } catch (e) {
            console.warn("[danmaku] 取颜色失败，改用白色：" + e)
        }

        var type = (bar !== null && bar !== undefined) ? bar.dmType : 1

        try {
            var accepted = danmaku.sendDanmaku(text, { "type": type, "uid": 1, "color": color })

            /*
             * 【引擎说没收下就别清输入框】sendDanmaku 现在把引擎的裁决透出来了
             * （以前是 void，"没反应"和"发成功"在日志里一模一样）。
             * 被屏蔽规则挡 / 同屏条数到顶时：留着文字让用户改一改再发。
             */
            if (accepted === false) {
                console.warn("[danmaku] 发送失败：引擎没有接受这条（屏蔽规则挡了 / 同屏条数到顶）"
                             + "  type=" + type + "  color=" + color)
                return
            }

            dmInput.text = ""
            console.warn("[danmaku] 已发送：" + text + "  type=" + type + "  color=" + color)
        } catch (e2) {
            console.warn("[danmaku] sendDanmaku 抛错：" + e2)
        }
    }


            /*
             * 横向：**夹在左右两组之间**（不再按整行居中）。
             * 按整行居中会有问题：左右两组宽度不对称（左边播放+时间 ≈130，右边五个按钮 ≈300），
             * 居中的胶囊会伸到右边那组后面去 —— 而左右组声明在它之后、画在它上面，看着就是"胶囊跑到按钮后面"。
             * 现在左边贴左组的右边缘、右边贴右组的左边缘，于是它的宽度天然 = 中间真正剩下的空间，
             * 里面的 Row 又是 centerIn，所以胶囊永远居中在那段空白里。
             */
            anchors {
                left: leftGroup.right
                right: rightGroup.left
                bottom: parent.bottom
                bottomMargin: bar.bottomRowHeight
                              - (bar.btnLineHeight - bar.iconSize) / 2 - bar.iconSize / 2
                              - height / 2
            }
            height: 34

            Row {
                id: dmRow

                anchors.centerIn: parent
                spacing: 12

                /* 弹幕开关 */
                Item {
                    id: dmSwitch

                    width: 28
                    height: 34

                    /*
                     * 弹幕开关图标：项目里的 DanmakuSwitchOn/Off.svg 原件（开/关两态）。
                     * 尺寸和颜色照参考 sendbar/index.scss:75-99：
                     *   24x24、默认填充 #757575（--player-dmsend-switch-icon）、hover 变 #00a1d6。
                     * SVG 是白色底、Image 改不了 fill，所以用 MultiEffect 整颗着色。
                     */
                    Image {
                        id: dmSwitchIcon

                        anchors.centerIn: parent
                        width: 24
                        height: 24
                        source: bar.danmakuEnabled ? "../assets/images/dm/DanmakuSwitchOn.svg"
                                                   : "../assets/images/dm/DanmakuSwitchOff.svg"
                        sourceSize: Qt.size(24, 24)
                        smooth: true
                        visible: false
                    }

                    MultiEffect {
                        anchors.fill: dmSwitchIcon
                        source: dmSwitchIcon
                        colorization: 1
                        colorizationColor: dmSwitchArea.containsMouse ? QtPlayerTheme.playerAccentHover : "#ffffff"
                    }

                    MouseArea {
                        id: dmSwitchArea

                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: bar.danmakuEnabled = !bar.danmakuEnabled
                    }
                }

                /* 设置齿轮 + 弹幕设置浮层 */
                /*
                 * 浮层写法**和音量/倍速/清晰度那三个菜单完全一致**
                 * （T.Popup + 自己维护 pointerIn + 100ms 容错定时器 + closePolicy）——
                 * 自绘浮层会和平级元素互相抢 hover，出现"面板闪烁 / 移上去就关"，
                 * 三个菜单当初就是踩了这个坑才统一换成 Popup 的。
                 */
                Item {
                    id: dmSetting

                    width: 28
                    height: 34

                    /* 设置齿轮：DanmakuSetting.svg 原件；颜色照参考（#757575 → hover #00a1d6） */
                    Image {
                        id: dmSettingIcon

                        anchors.centerIn: parent
                        width: 24
                        height: 24
                        source: "../assets/images/dm/DanmakuSetting.svg"
                        sourceSize: Qt.size(24, 24)
                        smooth: true
                        visible: false
                    }

                    MultiEffect {
                        anchors.fill: dmSettingIcon
                        source: dmSettingIcon
                        colorization: 1
                        colorizationColor: (dmPanel.opened || dmSettingArea.containsMouse)
                                           ? QtPlayerTheme.playerAccentHover : "#ffffff"
                    }

                    MouseArea {
                        id: dmSettingArea

                        /*
                         * 触发区**只等于齿轮本身**（anchors.fill）。
                         * 之前把它铺成"面板 + 缝 + 齿轮"一大块，结果它和浮层判据互相抢 hover，
                         * 出现"打开→抢走→判定离开→关掉→又打开"的循环（颜色一直闪）。
                         * 现在三块区域各管各的、互不重叠：
                         *   齿轮本体 → 这个 MouseArea；中间的缝 → dmHoverZone；面板 → 浮层判据。
                         */
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onEntered: {
                            dmCloseTimer.stop()
                            dmPanelBody.syncFromEngine()
                            dmPanel.open()
                        }
                        onExited: dmCloseTimer.restart()
                    }

                    /*
                     * 写法完全照三 个浮层菜单（音量/倍速/清晰度）：
                     *   * 100ms 容错定时器：到点再看"鼠标还在不在这块区域里"，不在才关；
                     *   * dmHoverZone：把"齿轮 + 中间的缝 + 浮层"合成一块几何区域来判 hover。
                     *     这一块是三个菜单都有的，弹幕面板之前漏了，所以才会出现
                     *     "鼠标移到底部就被判离开、面板自己关掉 / 或移出去永远不关"。
                     */
                    Timer {
                        id: dmCloseTimer

                        interval: 100
                        repeat: false
                        onTriggered: {
                            /*
                             * 三块区域取或（每块只有它自己是 hover 目标，互不重叠，所以不会互相抢）：
                             *   齿轮本体 → dmSettingArea；缝 → dmHoverZone；面板 → 浮层 pointerIn。
                             * 少了任何一个来源都不行；把某一块铺大去"兼顾"另一块，就会变成抢 hover 的死循环。
                             */
                            if (!dmSettingArea.containsMouse && !dmHoverZone.pointerIn
                                    && !dmPanel.pointerIn)
                                dmPanel.close()
                        }
                    }

                    Item {
                        id: dmHoverZone

                        parent: dmSetting
                        visible: dmPanel.opened
                        /*
                         * 位置用**常量**直接写死，不去读 dmPanel.x/y —— 读浮层的坐标依赖
                         * `x: parent.width + 149 - width` 那串绑定先算好，一旦在浮层尺寸定下来
                         * 之前求值就会得到 0/NaN，整块判据退化成一条缝（这正是"鼠标移到面板
                         * 下半部分就被判离开、面板自己关掉"的原因）。
                         *   x = 28(齿轮宽) + 149 - 320 = -143
                         *   y = 34(齿轮高) - 46 - 322 = -334
                         * 高度 -y = 334 → 覆盖"面板整块 + 中间那道缝 + 到齿轮顶边"。
                         */
                        x: -143
                        y: -334
                        width: 320
                        height: 334
                        property bool pointerIn: false

                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onEntered: {
                                dmHoverZone.pointerIn = true
                                dmCloseTimer.stop()
                            }
                            onExited: {
                                dmHoverZone.pointerIn = false
                                dmCloseTimer.restart()
                            }
                            /*
                             * 把鼠标位置喂给面板：这块判据区域的**坐标原点和面板左上角是同一个点**
                             * （都在齿轮这个坐标系里，x/y 取值相同），所以这里的 mouseX/mouseY
                             * 就是面板坐标 —— 面板里的控件据此自己算 hover 变色。
                             */
                            onPositionChanged: function (mouse) {
                                dmPanelBody.pointerX = mouse.x
                                dmPanelBody.pointerY = mouse.y
                            }
                        }
                    }

                    T.Popup {
                        id: dmPanel

                        /* 自己维护"鼠标在不在浮层里"（框架的 Popup 没有可用的 hovered 属性）。
                           关掉时清零 + **回到第一页**（下次打开是左页，不会停在上次翻到的右页）。 */
                        property bool pointerIn: false

                        onClosed: {
                            pointerIn = false
                            dmPanelBody.showPage(false)
                        }

                        parent: dmSetting
                        /*
                         * 尺寸跟当前页走：左页 320x322、右页 266x250。
                         * 【不能绑 dmPanelBody.width/height】浮层会去**设置** contentItem 的尺寸，
                         * 那样就是绑定循环，QML 会把绑定断开，尺寸永远停在初始值（这就是
                         * "切页了宽高没变"的原因）。这里读的是面板的一个普通属性 pageOffset，
                         * 不构成环。
                         */
                        width: dmPanelBody.pageOffset < 0 ? 266 : 320
                        height: dmPanelBody.pageOffset < 0 ? 250 : 322

                        /* 切页时尺寸做过渡（参考 bui-panel-move 那条 cubic-bezier 的等价写法）。
                           【不要在 x/y 上加 Behavior】浮层的 x/y 是有绑定的（按 width/height 推出来），
                           再叠 Behavior 会和框架自己的定位打架 —— 那正是"位置直接变了、动画很割裂"的原因。
                           只动 width/height，位置由绑定跟着平滑走。 */
                        Behavior on width {
                            NumberAnimation { duration: 260; easing.type: Easing.OutCubic }
                        }
                        Behavior on height {
                            NumberAnimation { duration: 260; easing.type: Easing.OutCubic }
                        }
                        /*
                         * 参考 sendbar 的定位：面板右边缘比齿轮右边缘外扩 149px、
                         * 底边距齿轮底边 46px（所以 x 是 +149、y 是 − 46 − height）。
                         */
                        x: parent.width + 149 - width
                        y: parent.height - 46 - height

                        padding: 0
                        /* 面板里全是滑块和勾选，点它们不能关；关面板交给 hover / Esc / 点外面 */
                        closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutside
                        enter: Transition {}
                        exit: Transition {}

                        /*
                         * 浮层那层"鼠标在不在里面"的判据：**严格等于浮层自己的矩形**。
                         * 【别再铺大】之前为了绕开"下半部分收不到 hover"把它往齿轮方向多铺了 60px，
                         * 结果它压在触发区上面抢走 hover → dmSettingArea 收到 exit → 100ms 后关面板
                         * → 面板一关 hover 又回到触发区 → 又打开，形成死循环（鼠标和文字颜色一直闪）。
                         */
                        background: Rectangle {
                            width: dmPanel.width
                            height: dmPanel.height
                            color: Qt.rgba(0.08, 0.08, 0.08, 0.9)
                            radius: 2

                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                onEntered: {
                                    dmPanel.pointerIn = true
                                    dmCloseTimer.stop()
                                }
                                onExited: {
                                    dmPanel.pointerIn = false
                                    dmCloseTimer.restart()
                                    /* 鼠标离开面板：把喂给面板的位置清掉（图标的 hover 变色要跟着灭） */
                                    dmPanelBody.pointerX = -1
                                    dmPanelBody.pointerY = -1
                                }
                                /* 位置喂给面板内容，图标自己算 hover（面板里不能放 hoverEnabled 的 MouseArea）。
                                   参数必须写成 function (mouse)：直接写 onPositionChanged: { mouse.x } 会报
                                   "Parameter mouse is not declared ... deprecated"。 */
                                onPositionChanged: function (mouse) {
                                    dmPanelBody.pointerX = mouse.x
                                    dmPanelBody.pointerY = mouse.y
                                }
                            }
                        }

                        contentItem: DanmakuSettingPanel {
                            id: dmPanelBody

                            danmaku: bar.danmaku
                        }
                    }
                }

                /*
                 * 输入胶囊 + 发送按钮。
                 *
                 * 参考的结构是：`.player-video-inputbar`（浅灰胶囊，圆角 8px 0 0 8px）里装着
                 *   `.player-video-inputbar-wrap`（flex:1）→ 里面才是 `.player-video-btn-dm`（类型图标）
                 *   + `input.player-dm-input`（输入框本体）
                 * 然后紧挨着 `.player-dm-btn-send`（圆角 0 8px 8px 0）——
                 * **类型图标是在胶囊"里面"的**（所以它贴着输入框、底色是胶囊的浅灰），
                 * 输入框和发送按钮之间也没有缝。这一整组我用 spacing:0 的 Row + 一层胶囊 Rectangle 写出来。
                 */
                Row {
                    id: dmSendGroup

                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 0

                    /*
                     * 弹幕关掉（左边那颗开关）时，整个发送框一起**禁用**：
                     *   * enabled: false → 里面的 MouseArea（类型按钮）和 TextField 都不吃事件，
                     *     输入框点不进去、也打不了字（QML 的 enabled 会传给子项）；
                     *   * opacity 0.45 → 视觉上也是"灰掉"的样子（和发送按钮自己的禁用态
                     *     rgba(1,1,1,.2)/.45 一个风格）。
                     * 位置、顺序都不动 —— 只改"能不能用"。
                     */
                    enabled: bar.danmakuEnabled
                    opacity: bar.danmakuEnabled ? 1 : 0.45

                    Behavior on opacity {
                        NumberAnimation { duration: 150 }
                    }

                    /* 胶囊：左圆右方，右边直接接发送按钮。
                       底色/文字用**深色主题**那套（我们是压在画面上的深色控制栏）：
                       半透明白底 + 白字 + 半透明白占位 —— 浅灰实底(#f4f4f4)在深色条上看着像禁用。 */
                    Rectangle {
                        id: dmInputPill

                        /*
                         * 可用宽度 = **这一行自己的宽度**（它被夹在左右两组之间，见 dmBar 的 anchors），
                         * 所以"动态缩放"是天然的：窗口变窄 → 这段空白直接变小 → 胶囊跟着缩。
                         * 只缩输入框（类型图标 30 与发送按钮 62 不缩）；实在放不下就整块藏掉，
                         * 只留开关和齿轮。窗口小于参考的窄屏断点 750 时也藏（index.scss:4895）。
                         */
                        /*
                         * 可用宽度：**直接按几何算**左右两组之间的空白 ——
                         *   rightGroup.x（右组左边缘） − (leftGroup.x + leftGroup.width)（左组右边缘）
                         * 【为什么不用 dmBar.width】那一行是靠 anchors.left/right 卡出来的，
                         * 一旦锚点没按预期生效（或者算进来别的边距），读到的就不是真正的空白，
                         * 于是"减 62 减 24"怎么调都还是重叠。这里用两组的实际 x/width 相减，值是确定的。
                         */
                        readonly property real availableWidth:
                            Math.max(0, rightGroup.x - (leftGroup.x + leftGroup.width))
                        /*
                         * 这一行里"胶囊以外"的固定占用（用来反推胶囊能有多宽）。
                         * 【踩过的坑 · 重叠的真正原因】之前只减了"发送按钮 62 + 24"，
                         * **漏掉了同一个 Row 里的开关 28、齿轮 28 和两个 spacing 12**（共 80），
                         * 于是 行宽 = 80 + 胶囊 + 62 = (availableWidth − 86) + 142 = availableWidth + 56
                         * —— 行永远比空白宽 56，Row 又是 centerIn，就平均往两边各压 28px 盖到按钮上。
                         * 现在把 166 一次减干净（80 固定占用 + 62 发送 + 两边各 12 的缝），
                         * 行宽 = 142 + 胶囊 ≤ availableWidth − 24 **恒成立**，任何宽度都不会再重叠。
                         */
                        readonly property int dmRowFixedWidth: 28 + 12 + 28 + 12   /* 开关 + 缝 + 齿轮 + 缝 */
                        readonly property int dmRowSendWidth: 62
                        readonly property int dmRowSideGap: 12                     /* 与左右两组各留 12 */
                        readonly property int dmRowReserve: dmRowFixedWidth + dmRowSendWidth
                                                           + dmRowSideGap * 2      /* = 166 */
                        /*
                         * 最宽 450（原来 700 太宽，收窄）。
                         * 中间空白 = 窗口宽 − 348（12+12 内边距 + 左组 126 + 右组 198）：
                         *   窗口 ≤ 1014 → 空白 ≤ 666 → 空白 − 166 ≤ 450 → 胶囊开始**连续变窄**
                         *   窗口 750~1014 → 胶囊 254~450，这一段拖窗口就能看见它在动
                         *   窗口 < 750   → 整块隐藏（参考的窄屏断点 index.scss:4895）
                         * 实测日志（上限 700 那版）已验证动态是按这个公式走的：
                         *   bar=1094→pill=580、bar=928→414、bar=880→366、bar=874→360、bar=734→隐藏
                         */
                        readonly property int dmPillMaxWidth: 450
                        readonly property bool capsuleVisible:
                            bar.width >= 750 && availableWidth >= 150 + dmRowReserve
                        visible: capsuleVisible
                        /*
                         * 宽度：动态 + 最宽限制。
                         *   availableWidth − 166 → 减掉"胶囊以外的固定占用 + 两边的缝"
                         *   min(450, ...)        → 最宽 450
                         *   下限 150             → 类型图标 30 + 输入框最小 120
                         */
                        width: capsuleVisible ? Math.max(150, Math.min(dmPillMaxWidth, availableWidth - dmRowReserve)) : 0
                        height: 32
                        radius: 8
                        topRightRadius: 0
                        bottomRightRadius: 0
                        color: Qt.rgba(1, 1, 1, 0.2)

                        /* ---- 类型按钮（胶囊内，参考 .player-video-btn-dm）---- */
                        Item {
                            id: dmTypeButton

                            width: 30
                            height: 30
                            anchors.verticalCenter: parent.verticalCenter

                            /* hover 用"鼠标位置 + 是否在按钮或弹出列表上"判定：
                               给按钮加 hoverEnabled 会抢外层判据（这个坑今天踩过多次） */
                            readonly property bool hovered: bar.dmTypePanelOpen

                            Image {
                                id: dmTypeIcon

                                anchors.centerIn: parent
                                width: 24
                                height: 24
                                source: "../assets/images/dm/DanmakuTextSetting.svg"
                                sourceSize: Qt.size(24, 24)
                                smooth: true
                                visible: false
                            }

                            MultiEffect {
                                anchors.fill: dmTypeIcon
                                source: dmTypeIcon
                                colorization: 1
                                colorizationColor: dmTypeButton.hovered ? QtPlayerTheme.playerAccent : "#ffffff"
                            }

                            /*
                             * 触发区只盖**图标本身**（面板已经搬进浮层，不再需要那块"盖住面板"的大区域 ——
                             * 留着会在图标周围形成死区：鼠标离图标还挺远就弹出、图标旁边点不到输入框）。
                             * 面板位置由浮层自己算（贴在图标上方居中），hover 由"图标 / 面板整层 / 浮层判据"
                             * 三处取或来判，和设置面板一致。
                             */
                            MouseArea {
                                id: dmTypeArea

                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onEntered: {
                                    dmTypeHideTimer.stop()
                                    bar.dmTypePanelOpen = true
                                }
                                onExited: dmTypeHideTimer.restart()
                            }
                        }

                        /* 弹幕被关掉的那一刻，顺手把已经展开的"类型"浮层收掉
                           （不然它会在禁用的发送框上面孤零零留着） */
                        Connections {
                            target: bar

                            function onDanmakuEnabledChanged() {
                                if (!bar.danmakuEnabled)
                                    bar.dmTypePanelOpen = false
                            }
                        }

                        /* ---- 输入框本体（底色交给胶囊，自己不画底）---- */
                        TextField {
                            id: dmInput

                            anchors {
                                left: dmTypeButton.right
                                verticalCenter: parent.verticalCenter
                            }
                            /*
                             * 输入框占**胶囊剩下的宽度**：胶囊 − 类型图标 30 − 右边 10 的余量。
                             * 【踩过的坑】以前按 availableWidth 算（availableWidth − 100），
                             * 胶囊变窄之后输入框就比胶囊还宽、把文字和光标顶到发送按钮上；
                             * 现在只跟着胶囊走，胶囊多宽它就有多宽，永远不会越出去。
                             */
                            width: Math.max(60, dmInputPill.width - 30 - 10)
                            height: 32
                            placeholderText: qsTr("发个友善的弹幕见证当下")
                            placeholderTextColor: Qt.rgba(1, 1, 1, 0.6)
                            color: "#ffffff"
                            font.pixelSize: 12
                            leftPadding: 10
                            rightPadding: 10
                            selectByMouse: true
                            onAccepted: dmBar.sendDanmakuText()

                            background: null
                        }

                        /*
                         * 类型弹出列表（参考 .player-mode-selection-container：
                         * 216 宽、底色 hsla(0,0%,8%,.9)、圆角 2、bottom 39、水平居中、padding 2px 0 0）。
                         * 三项：滚动 / 顶部 / 底部（对应引擎的 type 1/2/3）。
                         */
                        /*
                         * 类型面板的**浮层**（和设置面板同一个写法：T.Popup + 自己的 pointerIn + closePolicy）。
                         * 面板内容就是下面那个 Item —— 只把它的 parent 指到这个浮层的 contentItem，
                         * 于是它落在 overlay 层，不再和同层元素抢 hover / 层级关系。
                         */
                        T.Popup {
                            id: dmTypePopup

                            property bool pointerIn: false

                            parent: dmTypeButton
                            /*
                             * 【不要绑 dmTypePanel.width/height】内容 Item 现在是 anchors.fill: parent，
                             * 尺寸是被浮层设的 —— 再反过来绑回去就是绑定循环，QML 会断开绑定、尺寸永远不对
                             * （设置面板当初就踩过这个：切页了宽高不变）。这里给独立来源：
                             * 宽 196、高 = 内容列 implicitHeight + 上下内边距 20。
                             */
                            width: 166
                            /* 上下内边距一样（各 12） */
                            height: dmTypePanelColumn.implicitHeight + 24
                            /* 底边贴在胶囊顶边上方 5px（胶囊相对按钮顶边是 −1，所以再 −5） */
                            x: (parent.width - width) / 2
                            y: -6 - height

                            padding: 0
                            closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutside
                            enter: Transition {}
                            exit: Transition {}

                            /*
                             * 浮层那层"鼠标在不在里面"的判据 —— 和设置面板一样放在 background 里：
                             * 面板内容（dmTypePanel）盖在它上面，它垫底、不挡点击，却是浮层里
                             * 唯一负责维持 pointerIn 的地方（内容里那些 MouseArea 都不接受 hover，
                             * 一旦接受就会把 hover 抢走、导致浮层被误判成"离开"而关掉）。
                             */
                            background: Rectangle {
                                color: "transparent"

                                MouseArea {
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onEntered: {
                                        dmTypePopup.pointerIn = true
                                        dmTypeHideTimer.stop()
                                    }
                                    onExited: {
                                        dmTypePopup.pointerIn = false
                                        dmTypeHideTimer.restart()
                                    }
                                }
                            }

                            contentItem: Item {
                                id: dmTypeContent
                            }
                        }

                        /* dmTypePanelOpen 是唯一状态：它变就开/关浮层；浮层被 Esc/点外面关掉时同步回来 */
                        Connections {
                            target: bar

                            function onDmTypePanelOpenChanged() {
                                if (bar.dmTypePanelOpen && !dmTypePopup.opened)
                                    dmTypePopup.open()
                                else if (!bar.dmTypePanelOpen && dmTypePopup.opened)
                                    dmTypePopup.close()
                            }
                        }

                        /* ---- 类型弹出列表（内容本体；parent 指向上面浮层的 contentItem）---- */
                        Item {
                            id: dmTypePanel

                            /* 内容归浮层管：position 由浮层决定，这里只填满 contentItem */
                            parent: dmTypeContent
                            anchors.fill: parent
                            z: 30

                            Rectangle {
                                anchors.fill: parent
                                color: Qt.rgba(0.08, 0.08, 0.08, 0.9)
                                radius: 2
                            }

                            Column {
                                id: dmTypePanelColumn

                                /*
                                 * 内边距：上 12、左右各 14、下 8。
                                 * 左右靠"列自身再内缩 4 + 各行本来还有 x:10"凑出 14；高度 = implicitHeight + 20。
                                 */
                                x: 4
                                y: 12
                                width: parent.width - 8

                                /*
                                 * 参考 selection/index.ts:34-46 的"字号"那一行 ——
                                 * **两行结构**：.row-title（标题）在上、.row-selection（选项）在下。
                                 * 参考 DOM 就是这样：<div class="row-title">字号</div>
                                 *                  <div class="row-selection">[小][标准]</div>
                                 */
                                Item {
                                    id: dmFontRow

                                    x: 10
                                    width: parent.width - 20
                                    /* 标题 16 + 标题与选项之间 6 + 选项行 24 */
                                    height: 16 + 6 + 24

                                    /*
                                     * 字号两档：参考把"小 / 标准"定义成**相对基准字号的倍数**
                                     * （DanmakuFontSize.SMALL = 0.8 / NORMAL = 1.0，
                                     *   见 plugins/src/utils/danmaku/README.md:628-633），不是固定像素；
                                     * 参考自己的初值也是 'normal'（Selection.ts:62）。
                                     * 本应用层的基准字号是 DanmakuView.baseFontSize = **25**
                                     * （1080p 下按要求从引擎默认 18 提上来的，见那个属性的说明），
                                     * 所以两档 = 25×0.8 = 20px / 25×1.0 = 25px —— 默认正好是"标准"。
                                     *
                                     * 【原来为什么两档都不亮】模型里写的是引擎层的绝对像素 14/18
                                     * （那是基准 18 时的两档），而实际基准是 25 → 和 20/25 都不相等
                                     * → 两档全灭（用户实测："字号没有选中"）。
                                     * 现在按"离哪一档最近"判定：永远有且只有一档亮着；
                                     * 从设置面板把字号拖到中间值时也会落到最近的那一档。
                                     */
                                    readonly property real baseFontSize: 25
                                    readonly property real nearestRatio: {
                                        if (bar.danmaku === null || bar.danmaku === undefined)
                                            return 1.0

                                        var base = bar.danmaku.baseFontSize
                                        return Math.abs(base - baseFontSize * 0.8)
                                               <= Math.abs(base - baseFontSize * 1.0) ? 0.8 : 1.0
                                    }

                                    Text {
                                        width: parent.width
                                        height: 16
                                        text: qsTr("字号")
                                        color: "#ffffff"
                                        font.pixelSize: 12
                                        verticalAlignment: Text.AlignVCenter
                                    }

                                    Row {
                                        y: 22
                                        height: 24
                                        spacing: 8

                                        Repeater {
                                            model: [{ "label": "小", "ratio": 0.8 },
                                                    { "label": "标准", "ratio": 1.0 }]

                                            Rectangle {
                                                id: dmFontItem

                                                required property var modelData

                                                width: 44
                                                height: 22
                                                anchors.verticalCenter: parent.verticalCenter
                                                radius: 2
                                                color: dmFontItem.active ? QtPlayerTheme.playerAccent : "transparent"
                                                border.width: 1
                                                /* 哪一档亮，由"当前字号离哪一档最近"算出来（见 dmFontRow 那段） */
                                                readonly property bool active: dmFontRow.nearestRatio
                                                                               === dmFontItem.modelData.ratio
                                                readonly property bool hovered: dmFontArea.containsMouse
                                                border.color: dmFontItem.active ? QtPlayerTheme.playerAccent
                                                                                : (dmFontItem.hovered ? QtPlayerTheme.playerAccent
                                                                                                      : Qt.rgba(1, 1, 1, 0.3))

                                                Text {
                                                    anchors.centerIn: parent
                                                    text: dmFontItem.modelData.label
                                                    font.pixelSize: 12
                                                    color: dmFontItem.active ? "#ffffff" : Qt.rgba(1, 1, 1, 0.8)
                                                }

                                                MouseArea {
                                                    id: dmFontArea

                                                    anchors.fill: parent
                                                    /* 不能 hoverEnabled：见 dmTypePanelHover 的说明（会抢整层判据） */
                                                    cursorShape: Qt.PointingHandCursor
                                                    onEntered: dmTypeHideTimer.stop()
                                                    onExited: dmTypeHideTimer.restart()
                                                    onClicked: {
                                                        /* 基准字号 × 这一档的倍数（参考的 SMALL/NORMAL 比例） */
                                                        if (bar.danmaku === null || bar.danmaku === undefined)
                                                            return

                                                        bar.danmaku.setFontSize(
                                                                    Math.round(dmFontRow.baseFontSize
                                                                               * dmFontItem.modelData.ratio))
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }

                                /*
                                 * 参考 selection/index.ts:47-74 的"模式"那一行：
                                 * 三个 **图标 + 文字** 横排（滚动 / 顶部 / 底部），
                                 * 选中项和 hover 都变主题蓝。图标直接用项目里那三个 SVG 原件
                                 * （filter-scroll / filter-top / filter-bottom —— 和参考内嵌的是同一份 path）。
                                 * 标题「模式」单独一行（参考 .row-title），图标行在它下面。
                                 */
                                /* 字号 与 模式 之间加大上下距离 */
                                Item {
                                    width: 1
                                    height: 10
                                }

                                Text {
                                    x: 10
                                    width: parent.width - 20
                                    height: 16
                                    text: qsTr("模式")
                                    color: "#ffffff"
                                    font.pixelSize: 12
                                    verticalAlignment: Text.AlignVCenter
                                }

                                /* 标题「模式」和下面的图标行之间留 6px */
                                Item {
                                    width: 1
                                    height: 6
                                }

                                Row {
                                    x: 10
                                    spacing: 16

                                    Repeater {
                                        model: [
                                            { "label": "滚动", "type": 1, "icon": "filter-scroll.svg" },
                                            { "label": "顶部", "type": 2, "icon": "filter-top.svg" },
                                            { "label": "底部", "type": 3, "icon": "filter-bottom.svg" }
                                        ]

                                        Item {
                                            id: dmModeItem

                                            required property var modelData

                                            width: 28
                                            height: 46

                                            readonly property bool hovered: modeArea.containsMouse
                                            readonly property bool active: bar.dmType === modelData.type

                                            Image {
                                                id: modeIcon

                                                x: (parent.width - width) / 2
                                                width: 28
                                                height: 28
                                                source: "../assets/images/dm/" + dmModeItem.modelData.icon
                                                sourceSize: Qt.size(28, 28)
                                                smooth: true
                                                visible: false
                                            }

                                            MultiEffect {
                                                anchors.fill: modeIcon
                                                source: modeIcon
                                                colorization: 1
                                                colorizationColor: (dmModeItem.active || dmModeItem.hovered)
                                                                   ? QtPlayerTheme.playerAccent : "#ffffff"
                                            }

                                            Text {
                                                anchors {
                                                    top: modeIcon.bottom
                                                    topMargin: 2
                                                    horizontalCenter: parent.horizontalCenter
                                                }
                                                text: dmModeItem.modelData.label
                                                font.pixelSize: 12
                                                color: (dmModeItem.active || dmModeItem.hovered)
                                                       ? QtPlayerTheme.playerAccent : Qt.rgba(1, 1, 1, 0.8)
                                            }

                                            MouseArea {
                                                id: modeArea

                                                anchors.fill: parent
                                                /* 不能 hoverEnabled：见 dmTypePanelHover 的说明（会抢整层判据） */
                                                cursorShape: Qt.PointingHandCursor
                                                onEntered: dmTypeHideTimer.stop()
                                                onExited: dmTypeHideTimer.restart()
                                                onClicked: bar.dmType = dmModeItem.modelData.type
                                            }
                                        }
                                    }
                                }

                                /*
                                 * 参考 selection/index.ts:75-146 的"颜色"那一行：输入框 + 色板。
                                 * 色值原样用参考内嵌的那 14 个；选中加主题蓝描边，hover 提亮。
                                 * 状态放在这一行自己身上（dmColorIndex + colorList），
                                 * 发送时 sendDanmakuText 读 dmColorRow.colorList[dmColorRow.dmColorIndex]。
                                 */
                                /* 标题「颜色」单独一行（参考 .row-title），色板在它下面 */
                                /* 模式 与 颜色 之间加大上下距离 */
                                Item {
                                    width: 1
                                    height: 10
                                }

                                Text {
                                    x: 10
                                    width: parent.width - 20
                                    height: 16
                                    text: qsTr("颜色")
                                    color: "#ffffff"
                                    font.pixelSize: 12
                                    verticalAlignment: Text.AlignVCenter
                                }

                                /* 标题「颜色」和下面的色板之间留 6px */
                                Item {
                                    width: 1
                                    height: 6
                                }

                                Row {
                                    id: dmColorRow

                                    x: 10
                                    height: 35
                                    spacing: 8

                                    /*
                                     * 【这是"弹幕可选颜色"的调色板 —— 数据，不是主题色】
                                     * 官方发送框给的 14 个颜色就是这个集合（红橙黄绿青蓝紫灰白）。
                                     * **不要**把它们换成 QtPlayerTheme.playerAccent：
                                     * 主题色管的是"界面的强调色"（进度条/选中/hover），
                                     * 而这里是"用户能选来发出去的弹幕颜色"，换了就等于改了功能。
                                     * （用 find-blue-colors.js 扫全项目时，这几行会被列出来 —— 属于预期。）
                                     */
                                    readonly property var colorList: [
                                        "#FE0302", "#FF7204", "#FFAA02", "#FFD302", "#FFFF00",
                                        "#A0EE00", "#00CD00", "#019899", "#4266BE", "#89D5FF",
                                        "#CC0273", "#222222", "#9B9B9B", "#FFFFFF"
                                    ]
                                    property int dmColorIndex: 13      /* 默认白 */

                                    /* 行内不再放"颜色"二字：标题已经单独一行了（见上） */
                                    Text {
                                        visible: false
                                        width: 0
                                        height: 1
                                    }

                                    /* 当前色的预览块 + 等宽排开的 14 格色板（两行 7 格） */
                                    Grid {
                                        anchors.verticalCenter: parent.verticalCenter
                                        columns: 7
                                        spacing: 3

                                        Repeater {
                                            model: dmColorRow.colorList

                                            Rectangle {
                                                id: dmColorItem

                                                required property string modelData
                                                required property int index

                                                width: 16
                                                height: 16
                                                radius: 2
                                                color: dmColorItem.modelData
                                                border.width: (dmColorRow.dmColorIndex === dmColorItem.index)
                                                              || dmColorHover.containsMouse ? 2 : 1
                                                border.color: (dmColorRow.dmColorIndex === dmColorItem.index)
                                                              ? QtPlayerTheme.playerAccent
                                                              : (dmColorHover.containsMouse ? QtPlayerTheme.playerAccentHover
                                                                                            : Qt.rgba(1, 1, 1, 0.35))

                                                MouseArea {
                                                    id: dmColorHover

                                                    anchors.fill: parent
                                                    /* 不能 hoverEnabled：见 dmTypePanelHover 的说明（会抢整层判据） */
                                                    cursorShape: Qt.PointingHandCursor
                                                    onEntered: dmTypeHideTimer.stop()
                                                    onExited: dmTypeHideTimer.restart()
                                                    onClicked: dmColorRow.dmColorIndex = dmColorItem.index
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            /*
                             * 面板整层的 hover 判据 —— 它必须是面板里**唯一**接受 hover 的东西。
                             *
                             * 【关键】面板内各行的 MouseArea 都**不能**写 hoverEnabled：一写，鼠标压到
                             * 那一项上时它们就把 hover 抢走，整层判据和外面那块大区域**同时**收不到 hover，
                             * 120ms 后就被判成"离开" → 面板自己关掉（这就是反复出现的"移到面板上就关"）。
                             * 所以这里垫底（z: -1，不挡点击），面板内的项一律只做点击。
                             */
                            MouseArea {
                                id: dmTypePanelHover

                                anchors.fill: parent
                                hoverEnabled: true
                                onEntered: dmTypeHideTimer.stop()
                                onExited: dmTypeHideTimer.restart()
                                z: -1
                            }
                        }
                    }

                    Timer {
                        id: dmTypeHideTimer

                        interval: 120
                        repeat: false
                        onTriggered: {
                            /*
                             * 【这里原来写的是 dmTypePanel.containsMouse】—— dmTypePanel 是普通 Item，
                             * 没有 containsMouse 这个属性，取到 undefined、取反恒为真，于是鼠标一移到面板上
                             * 就被判成"离开"、面板立刻收起。要用面板里那层 MouseArea 的 containsMouse。
                             */
                            if (!dmTypeArea.containsMouse && !dmTypePanelHover.containsMouse
                                    && !dmTypePopup.pointerIn)
                                bar.dmTypePanelOpen = false
                        }
                    }

                    /* 发送按钮：参考 .player-dm-btn-send > .button-blue */
                    Item {
                        id: dmSendButton

                        /* 跟着胶囊一起显隐：胶囊藏起来时不能只剩一个孤零零的发送按钮。
                           宽度也一起归零 —— 不依赖"Row 会不会给不可见子项留位"，
                           藏起来时这一行就精确只剩开关 + 齿轮（80），窄窗口也不会再把两边压住。 */
                        visible: dmInputPill.capsuleVisible
                        width: dmInputPill.capsuleVisible ? 62 : 0
                        height: 32

                        /* 弹幕关掉时发送按钮也是禁用态（不只是灰：点它不会发出去） */
                        readonly property bool ready: bar.danmakuEnabled && dmInput.text.length > 0
                        readonly property bool hovered: dmSendArea.containsMouse && ready

                        Rectangle {
                            anchors.fill: parent
                            /* 右圆左方：左边直接接输入框 */
                            radius: 8
                            topLeftRadius: 0
                            bottomLeftRadius: 0
                            color: dmSendButton.ready
                                   ? (dmSendButton.hovered ? QtPlayerTheme.playerAccentHover : QtPlayerTheme.playerAccent)
                                   : Qt.rgba(1, 1, 1, 0.2)

                            Behavior on color {
                                ColorAnimation { duration: 200 }
                            }
                        }

                        Text {
                            anchors.centerIn: parent
                            text: qsTr("发送")
                            font.pixelSize: 13
                            color: dmSendButton.ready ? "#ffffff" : Qt.rgba(1, 1, 1, 0.45)
                        }

                        MouseArea {
                            id: dmSendArea

                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: dmSendButton.ready ? Qt.PointingHandCursor : Qt.ArrowCursor
                            /* 【不要再往这里加 ready 判断】加上之后"弹幕开关没开/输入框空"就变成
                               **点了完全没反应**（连日志都没有，查不出来）。判断放在
                               sendDanmakuText() 里，每个失败分支都会打一条 warn。 */
                            onClicked: dmBar.sendDanmakuText()
                        }
                    }
                }

            }
        }

        /* ------------------------------ 左边 ------------------------------ */
        /*
         * 左右两组**贴行顶**，不垂直居中。
         * 参考里 .player-control-bottom 是 flex 容器且没写 align-items（默认 stretch），
         * 而按钮 .player-ctrl-btn 是固定 height:22px —— 固定高度的 flex 项在 stretch 下
         * 等价于 flex-start，也就是**贴在 35px 行的顶部**（图标占 13~35px）✓
         * 居中会整体低 6.5px，按钮与进度条之间的间距就比参考多 6.5px。
         */
