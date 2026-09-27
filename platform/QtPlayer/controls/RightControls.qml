// ===========================================================================
// 控制栏右边那一组（从 PlayerControlBar.qml 拆出来，内容**原样搬移**）
//
// 里面是：清晰度、选集（仅全屏）、倍速、字幕、音量、设置、全屏 七颗按钮，
// 以及它们各自的浮层菜单（volumeMenu / rateMenu / qualityMenu / subtitleMenu / eplistMenu）。
// 全部按"从右往左串"的锚法排：全屏 ← 设置 ← 音量 ← 字幕 ← 倍速 ← 选集 ← 清晰度。
//
// 依赖只有一个：bar（控制栏本体 —— 主题数值、menuBottom、player、episodes、
//                   以及 settingsRequested / fullscreenRequested / episodeSelected 三个信号）。
// 内部所有 bar.xxx 的写法一字未改。
//
// 对外两样东西：
//   pointerInside —— 这一组所有可交互区域 + 打开的浮层（控制栏判"鼠标还在不在"用）
//   closeMenus()  —— 收起全部浮层（控制栏收起 / 切页时调）
// ===========================================================================
import QtQuick
import QtQuick.Templates as T
/* 发送框的输入框用 Qt 自带控件（Basic 样式）；MultiEffect 用来给 SVG 着色。
   拆出来时必须一起带过来，少了就是致命错误：本文件加载失败 → 模块里没有 Main → 程序退出。 */
import QtQuick.Controls.Basic
import QtQuick.Effects
/* 相对目录导入：让编辑器（qmlls）按**文件系统**直接找到模块根目录里的组件
   （LottieIcon / QtPlayerTheme / SubtitlePanel…），不用它去猜构建目录、也不需要导入路径配置。
   下一行的 import QtPlayer 留给运行时用；两行指向同一个文件，不冲突。 */
import ".."
import QtPlayer

        Item {
            id: rightGroup

                /* ---- 从外面传进来的依赖（见文件头说明）---- */
                property var bar: null
                /* 播放器：块里有一处直接用 player（原来它就在同一个文件里）。
                   从 bar 上取，保持搬移的代码一字未改。 */
                property var player: bar ? bar.player : null

                /*
                 * 四颗**文字**按钮（清晰度 / 选集 / 倍速 / 字幕）统一的左右内边距。
                 *
                 * 这个值不是随便定的：**就等于图标按钮（音量 / 设置 / 全屏）的内边距**
                 *     (bar.btnWidth − bar.iconSize) / 2
                 * 全屏： (54 − 28) / 2 = 13      非全屏： (36 − 22) / 2 = 7
                 *
                 * 【为什么必须跟它们对齐】整组按钮是"边缘挨边缘"锚的，两段内容之间的空隙
                 * = 两颗按钮内边距之和。图标按钮的内边距是宽度和图标尺寸定死的（改不了，
                 * 改了图标就不居中），所以文字按钮必须跟它们用同一个值，整组间距才会一致。
                 * 写成"跟着 btnWidth/iconSize 算"还顺带解决两档：全屏 13、窗口 7，各自都对上。
                 *
                 * 【别再写死数字】之前清晰度写 max(文字宽,40) → 内边距只剩 3~4px，
                 * 于是"清晰度 ↔ 选集"明显比"音量 ↔ 设置 ↔ 全屏"那段挤。踩过。
                 */
                readonly property int textBtnPad: Math.round((bar.btnWidth - bar.iconSize) / 2)

                /*
                 * 鼠标是不是停在这一组的任意可交互区域（或它展开的浮层）上。
                 * 原来这十几行直接写在控制栏那个大表达式里；搬过来后由本组件自己汇总 ——
                 * 漏一处就会被判成"离开控制栏"，整条控制栏直接收起（这个坑踩过三次）。
                 */
                readonly property bool pointerInside: fullscreenArea.containsMouse
                                                      || settingArea.containsMouse
                                                      || volumeArea.containsMouse
                                                      || volumeMenu.opened
                                                      || rateArea.containsMouse
                                                      || rateMenu.opened
                                                      || qualityArea.containsMouse
                                                      || qualityMenu.opened
                                                      || subtitleArea.containsMouse
                                                      || subtitleMenu.opened
                                                      || eplistArea.containsMouse
                                                      || eplistMenu.opened
                                                      /* 设置浮层（设置齿轮的面板）：漏了它，
                                                         鼠标一挪到面板上就会被判成"离开控制栏"→ 整条收起 */
                                                      || settingMenu.opened

                /* 收起这一组上开着的所有浮层（控制栏收起 / 切页时调） */
                function closeMenus() {
                    if (volumeMenu.opened)
                        volumeMenu.close()

                    if (rateMenu.opened)
                        rateMenu.close()

                    if (qualityMenu.opened)
                        qualityMenu.close()

                    if (subtitleMenu.opened)
                        subtitleMenu.close()

                    if (eplistMenu.opened)
                        eplistMenu.close()

                    /* 设置浮层**不在这里关**：全屏时面板悬在画面上方，鼠标从齿轮移到面板上会进入
                       画面活动层，那边一 entered 就调 closeMenus() → 面板被顺手关掉（这就是
                       "全屏鼠标一上去就关闭"）。它只按自己的 hover 判据关（移出才关）。 */
                }



            anchors {
                right: parent.right
                /* 同样贴行顶（原因见左边那段注释） */
                top: parent.top
            }
            width: Math.max(bar.rightGroupMinWidth,
                            qualityButton.width + eplistButton.width + rateButton.width
                            + subtitleButton.width
                            + volumeButton.width
                            + settingButton.width + fullscreenButton.width)
            height: bar.btnHeight

            /* ---------------- 全屏（最右） ---------------- */
            Item {
                id: fullscreenButton

                anchors {
                    right: parent.right
                    verticalCenter: parent.verticalCenter
                }
                width: bar.btnWidth
                height: bar.btnHeight

                LottieIcon {
                    id: fullscreenIcon

                    anchors {
                        top: parent.top
                        topMargin: (bar.btnLineHeight - bar.iconSize) / 2
                        horizontalCenter: parent.horizontalCenter
                    }
                    width: bar.iconSize
                    height: bar.iconSize
                    canvasWidth: 88
                    canvasHeight: 88
                    sequence: [
                        {
                            "source": "assets/lottie-icon/fullscreen-animation.json",
                            "autoplay": false
                        }
                    ]
                }

                /* 参考里鼠标移入就 play() 一遍（RightControls.ts:128-130） */
                MouseArea {
                    anchors.fill: parent
                    id: fullscreenArea
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onEntered: fullscreenIcon.play()
                    onClicked: bar.fullscreenRequested()
                }
            }

            /* ---------------- 设置（齿轮：移入播动画，点击打开本应用的设置页） ---------------- */

            /*
             * 这里**不**做参考实现那个"镜像画面/洗脑循环/自动开播/更多播放设置"菜单：
             * 按用户要求，齿轮点一下直接打开现有的设置页（主题/硬解/诊断，
             * 见 Main.qml 的 openSettings()）。
             */
            Item {
                id: settingButton

                anchors {
                    right: fullscreenButton.left
                    verticalCenter: parent.verticalCenter
                }
                width: bar.btnWidth
                height: bar.btnHeight

                LottieIcon {
                    id: settingIcon

                    anchors {
                        top: parent.top
                        topMargin: (bar.btnLineHeight - bar.iconSize) / 2
                        horizontalCenter: parent.horizontalCenter
                    }
                    width: bar.iconSize
                    height: bar.iconSize
                    canvasWidth: 88
                    canvasHeight: 88
                    sequence: [
                        {
                            "source": "assets/lottie-icon/settings-animation.json",
                            "autoplay": false
                        }
                    ]
                }

                MouseArea {
                    anchors.fill: parent
                    id: settingArea
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onEntered: {
                        settingIcon.play()
                        /* 鼠标进齿轮 → 展开设置浮层（和字幕/音量/倍速/清晰度同一套：
                           移入开、移出 100ms 后关，见 docs/QML-POPUP-HOVER-PITFALLS.md） */
                        settingMenu.open()
                        settingCloseTimer.stop()
                    }
                    /* 【只有 hover，没有点击】和字幕面板完全一样：鼠标移上去展开、
                       移开（含 100ms 容错）收起。齿轮不再绑任何点击行为。
                       原来的应用设置页（Main.openSettings()）组件保留，后续从面板里再入口。 */
                    onExited: settingCloseTimer.restart()
                }

                Timer {
                    id: settingCloseTimer

                    interval: 100
                    repeat: false
                    onTriggered: {
                        /* 【照字幕面板：三个判据，一个不多一个不少】按钮 → 缝区 → 浮层
                           （见上面 subtitleCloseTimer，RightControls.qml:733-744）。
                           浮层那一项只看 background 那层 MouseArea 的 pointerIn —— 面板里已经
                           没有任何接受 hover 的元素（SettingsPanel 的 hover 全走
                           pointerOn() 被动判），指针压在开关/单选行上时它仍然是 true，
                           所以不需要第二个兜底判据；多那一个的后果正是"移出面板也不关"（用户实测）。 */
                        if (!settingArea.containsMouse && !settingHoverZone.pointerIn
                                && !settingMenu.pointerIn)
                            settingMenu.close()
                    }
                }

                /*
                 * ============ 缝区：照 eplistHoverZone 的写法（RightControls.qml:942-965） ============
                 *
                 * 几何（和 eplistHoverZone 同一个式子，只是用浮层自己的 y/height 表达）：
                 *     y      = settingMenu.y + settingMenu.height   → 上边缘接浮层底边
                 *     height = -y                                    → 下边缘正好落在齿轮顶边
                 * 于是它正好盖住"浮层底边 ~ 齿轮顶边"那段缝：
                 *     settingMenu 底边 = parent.height − bar.menuBottom = 22 − 41 = **−19**（非全屏）
                 *                                                       = 43 − 74 = **−31**（全屏）
                 * 这段在补上之前不属于任何东西（齿轮 MouseArea 只管 y ≥ 0、浮层的 background
                 * 只管浮层自己的矩形），而按钮行正上方就是进度条的热区 →
                 * 鼠标从齿轮直线往上走会被进度条接走（圆点/预览气泡/缩放被触发）。
                 *
                 * 另外四个浮层每个都有这么一块（volumeHoverZone:398-414、subtitleHoverZone:699-714、
                 * eplistHoverZone:942-965、rateHoverZone:1325-1341、qualityHoverZone:1554-1570），
                 * 这里就是照它们写的：`visible: 浮层.visible` + `property bool pointerIn` +
                 * 一层 hoverEnabled 的 MouseArea 维护 pointerIn 与 100ms 容错定时器
                 * （缝区自己没有子控件，不存在"移到子控件上误判离开"的问题，所以用 MouseArea 就够）。
                 * 与齿轮触发区只相邻不重叠（清单第 11 条）。
                 */
                Item {
                    id: settingHoverZone

                    parent: settingButton
                    /* 【visible 而不是 opened】浮层刚打开那一瞬 `opened` 可能还没置位（opening 过渡），
                       缝区若跟着不生效，鼠标正好在缝里就没人接 hover → 直接穿到后面的进度条。 */
                    visible: settingMenu.visible
                    x: settingMenu.x
                    y: settingMenu.y + settingMenu.height
                    width: settingMenu.width
                    height: -y
                    property bool pointerIn: false

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        onEntered: {
                            settingHoverZone.pointerIn = true
                            settingCloseTimer.stop()
                        }
                        onExited: {
                            settingHoverZone.pointerIn = false
                            settingCloseTimer.restart()
                        }
                    }
                }

                /*
                 * 详情：参考 .bpx-player-ctrl-setting-box
                 *   非全屏 bottom:41px right:-48px；全屏 bottom:74px right:-40px
                 * （面板本体在 SettingsPanel.qml，DOM/CSS 出处都写在那个文件的头注释里）
                 */
                T.Popup {
                    id: settingMenu

                    /*
                     * 鼠标在不在浮层里 —— 照 volumeMenu 的做法（RightControls.qml:466 起那个浮层：
                     * `property bool pointerIn` + background 里**一层** hoverEnabled 的 MouseArea
                     * 在 onEntered/onExited 里改它 + onClosed 清零），判据只有这一处。
                     *
                     * 【为什么现在可以照它们这么写】它们的内容就画在 background 那一层里，
                     * 面板里没有任何"接受 hover"的元素，所以移到内容上行上不会让 background 的
                     * MouseArea 收到 exit。设置面板的内容虽然是独立组件（SettingsPanel），但
                     * 实测（grep 整个文件）：那边 `hoverEnabled` **只出现在注释里**，
                     * 所有行用的都是**被动** HoverHandler（:145/:243/:324/:429）+
                     * 不写 hoverEnabled 的 MouseArea（:328 那段注释专门记着这个坑）——
                     * 也就是说它和 volumeMenu 一样**没有接受 hover 的行**；
                     * 唯一接受 hover 的 TextInput（原「手填 mock 基址」那行）已经按用户要求删掉了。
                     * 所以 background 那层就是浮层里唯一接受 hover 的东西，pointerIn 由它维护即可。
                     */
                    property bool pointerIn: false

                    /* 位置：**右边缘和底边固定，切页时向左、向上长**
                       （宽度/高度变了 x、y 自动跟着减，所以面板不会往右挤、也不会从右边被遮住）。
                       底边 = 按钮底边 - bar.menuBottom（非全屏 41 / 全屏 74，同 CSS）。
                       y 再夹一道：面板顶边不允许越过播放区顶边（切到更高的第二页时向上长，
                       但不会长到窗口外面去）。 */
                    parent: settingButton
                    /* 整体再往左让 28px（用户要求：别被右边遮住）。CSS 原值是"右边缘比按钮
                       右边缘外扩 48 / 全屏 40"，这里保留那个关系、再统一左移。 */
                    x: parent.width - width + (bar.fullscreen ? 40 : 48) - 28
                    y: Math.max(-(bar.parent ? bar.parent.height - bar.height : 0),
                                parent.height - bar.menuBottom - height)

                    width: settingPanelBody.implicitWidth
                    /* 【高度跟着页面走】原来读 settingPanelBody.viewHeight —— 那个属性在
                       重写 SettingsPanel 时被删了，读到 undefined → 浮层高度为 0 → 面板看不见。
                       现在直接用 implicitHeight：左页 140，右页**按内容算**（四组单选 + 两行开关
                       + 底部返回条，见 SettingsPanel.qml 里的 rightContentHeight/rightHeight）——
                       写死高度会把右页最后两行裁掉，那颗「高能进度条」开关就点不到了。 */
                    height: settingPanelBody.implicitHeight

                    padding: 0
                    /* 【只靠"移出关闭"】Esc 是退全屏用的、也不要点击面板外关闭 */
                    closePolicy: T.Popup.NoAutoClose

                    contentItem: SettingsPanel {
                        id: settingPanelBody

                        bar: rightGroup.bar

                        /*
                         * 「高能进度条」开关：值来自控制栏（bar.highlightEnabled → ProgressRow），
                         * 改动写回控制栏 —— 单向数据流，面板自己不存状态
                         * （理由见 SettingsPanel.qml 里那两个声明的说明）。
                         */
                        highlightEnabled: rightGroup.bar.highlightEnabled
                        onHighlightToggled: function (value) { rightGroup.bar.highlightEnabled = value }
                    }

                    /* 关掉后回到第一页：第二页没有可见的返回行，不重置的话下次进来还停在第二页。
                       pointerIn 清零照 volumeMenu / eplistMenu 的 onClosed（RightControls.qml:1052）；
                       坐标一起清掉，免得下次打开时上一轮压着的那一行还亮着。 */
                    onClosed: {
                        pointerIn = false
                        settingPanelBody.pageIndex = 0
                        settingPanelBody.pointerX = -1
                        settingPanelBody.pointerY = -1
                    }

                    /* 浮层那一层判据：**照 volumeMenu 的分层**（RightControls.qml:490-513）——
                       background 是显式尺寸的 Rectangle，里面**一层** hoverEnabled 的 MouseArea：
                       它既"吃掉 hover"（不让指针穿到后面的进度条），又维护 settingMenu.pointerIn
                       并停/起 100ms 容错定时器。面板里没有第二个接受 hover 的元素（见上面 Popup 那段
                       的实测依据），所以移到开关/单选行上不会打断它。
                       底色透明：面板外观由 SettingsPanel 自己画（volumeMenu 的外观就是这层背景画的）。 */
                    background: Rectangle {
                        width: settingMenu.width
                        height: settingMenu.height
                        color: "transparent"

                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            onEntered: {
                                settingMenu.pointerIn = true
                                settingCloseTimer.stop()
                            }
                            /* 【照字幕面板】移出浮层时把喂给面板的坐标清掉：不清的话面板里
                               最后压着的那一行会一直停在 hover 态（看起来像"没反应"）。 */
                            onExited: {
                                settingMenu.pointerIn = false
                                settingPanelBody.pointerX = -1
                                settingPanelBody.pointerY = -1
                                settingCloseTimer.restart()
                            }
                            /* 面板里所有 hover 都靠这两个坐标被动判（见 SettingsPanel.pointerOn）——
                               这就是"面板里不许有接受 hover 的元素"那条规矩换来的东西。 */
                            onPositionChanged: function (mouse) {
                                settingPanelBody.pointerX = mouse.x
                                settingPanelBody.pointerY = mouse.y
                            }
                        }
                    }
                }
            }
            /* ---------------- 音量 ---------------- */
            Item {
                id: volumeButton

                anchors {
                    right: settingButton.left
                    verticalCenter: parent.verticalCenter
                }
                width: bar.btnWidth
                height: bar.btnHeight

                property bool open: false

                /* 四槽（VolumeSlider.ts:340-362）；画布 88x88 */
                LottieIcon {
                    id: volumeIcon

                    anchors {
                        top: parent.top
                        topMargin: (bar.btnLineHeight - bar.iconSize) / 2
                        horizontalCenter: parent.horizontalCenter
                    }
                    width: bar.iconSize
                    height: bar.iconSize
                    canvasWidth: 88
                    canvasHeight: 88
                    sequence: [
                        {
                            "source": "assets/lottie-icon/mute-to-volume-animation.json",
                            "startFrame": 2,
                            "complete": "next",
                            "autoplay": false
                        },
                        {
                            "source": "assets/lottie-icon/volume-hover-animation.json",
                            "complete": "stop",
                            "autoplay": false
                        },
                        {
                            "source": "assets/lottie-icon/volume-to-mute-animation.json",
                            "complete": "next",
                            "autoplay": false
                        },
                        {
                            "source": "assets/lottie-icon/volume-mute-hover-animation.json",
                            "complete": "stop",
                            "autoplay": false
                        }
                    ]
                    /* 初始槽 = 静音 ? 3 : 1（参考 VolumeSlider.ts:339 的 initialSlotIndex） */
                    slotIndex: (bar.player && bar.player.muted) ? 3 : 1
                }

                Timer {
                    id: volumeCloseTimer

                    /* 只是"从图标挪到浮层上"那一小段路的容错，不是等待时间 */
                    interval: 100
                    repeat: false
                    /* 到点再确认一次：图标、缝里、浮层里都不在鼠标下才关 */
                    onTriggered: {
                        if (!volumeArea.containsMouse && !volumeHoverZone.pointerIn
                                && !volumeMenu.pointerIn)
                            volumeMenu.close()
                    }
                }

                MouseArea {
                    id: volumeArea

                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onEntered: {
                        volumeIcon.play()
                        volumeCloseTimer.stop()
                        volumeMenu.open()
                    }
                    onExited: volumeCloseTimer.restart()
                    /* 点图标 = 切静音（VolumeSlider.ts:331） */
                    onClicked: {
                        if (bar.player)
                            bar.player.muted = !bar.player.muted
                    }
                }

                /*
                 * 图标上面那一段（浮层本身 + 浮层和图标之间那条缝）：鼠标在这一段里也算"还在菜单上"。
                 * 高度只到图标顶边为止，**不盖住图标**，免得和图标自己的 hover 打架。
                 * 浮层没开时整块不显示。
                 */
                Item {
                    id: volumeHoverZone

                    parent: volumeButton
                    visible: volumeMenu.opened
                    x: volumeMenu.x
                    y: volumeMenu.y
                    width: Math.max(volumeButton.width, volumeMenu.width)
                    height: -volumeMenu.y
                    property bool pointerIn: false

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onEntered: {
                            volumeHoverZone.pointerIn = true
                            volumeCloseTimer.stop()
                        }
                        onExited: {
                            volumeHoverZone.pointerIn = false
                            volumeCloseTimer.restart()
                        }
                    }
                }

                Connections {
                    target: bar.player

                    function onMutedChanged() {
                        volumeIcon.setSequenceSlot(bar.player && bar.player.muted ? 2 : 0)
                        volumeIcon.play()
                    }
                }

                /*
                 * 音量图标只在三个时机动，和参考一致（VolumeSlider.ts:84-86 / :199-216 / :173-189）：
                 *   * 鼠标移到**这颗图标**上 → play()
                 *   * 点它 → 切槽 0/2 再 play()
                 *   * 静音状态变了 → 切槽 2/0 再 play()
                 * 其它时候一律停住（LottieIcon 的 onStatusChanged/restFrame 保证"加载完不动"），
                 * 所以不会出现"鼠标一进控制栏一排图标都在播"。
                 */

                /*
                 * 音量弹层：同样用 Popup 浮层（和倍速/清晰度统一，原因见倍速菜单那段说明）。
                 * 尺寸/位置照参考：32x100、底边距按钮底边 41px（全屏 74px）；
                 * 数字 28 高，滑轨区 60 高、轨道 2px、圆点 12px ——controls.scss:949-1035。
                 */
                T.Popup {
                    id: volumeMenu

                    /* 自己维护"鼠标在不在浮层里"（框架的 Popup 没有可用的 hovered 属性），关掉时清零 */
                    property bool pointerIn: false

                    /*
                     * 显示用音量：静音时进度和数字都按 0 显示，**实际音量值不动**
                     * （取消静音后原值回来）—— 参考 VolumeSlider.ts:100-113。
                     */
                    readonly property real shownVolume: {
                        if (!bar.player)
                            return 1

                        return bar.player.muted ? 0 : Math.max(0, Math.min(1, bar.player.volume))
                    }

                    onClosed: {
                        pointerIn = false
                        volumeHoverZone.pointerIn = false
                    }

                    parent: volumeButton
                    width: QtPlayerTheme.volumeBoxWidth
                    height: QtPlayerTheme.volumeBoxHeight
                    x: (parent.width - width) / 2
                    y: parent.height - bar.menuBottom - height

                    padding: 0
                    /*
                     * 只留 Esc：音量图标点一下就是切静音，用 CloseOnPressOutside 的话
                     * 那一按会被当成"点了浮层外面"，面板当场就关了（参考里面板是靠 hover 收的）。
                     * 关面板交给下面那层 hover 判据。
                     */
                    closePolicy: T.Popup.CloseOnEscape
                    enter: Transition {}
                    exit: Transition {}

                    /*
                     * 浮层自己的"鼠标在不在里面"判据。
                     * 注意：QtQuick.Templates.Popup 是纯 C++ 类型、没有 QML 样式，**不会**给
                     * background 定尺寸 —— 所以必须自己 anchors.fill: parent 撑满，
                     * 否则里面这块 MouseArea 是 0 尺寸，鼠标移到浮层上永远判成"不在里面"，
                     * 表现出来就是"一移上去就关"。
                     * 判据用进出事件维护的 flag（不用 containsMouse），关闭时清零，不会留残余值。
                     */
                    background: Rectangle {
                        /* 显式按浮层尺寸来（不靠模板、也不靠 anchors）：
                           这块必须真的盖住整层，里面那层 MouseArea 才收得到 hover */
                        width: volumeMenu.width
                        height: volumeMenu.height
                        color: QtPlayerTheme.menuBg
                        radius: QtPlayerTheme.menuRadius

                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onEntered: {
                                volumeMenu.pointerIn = true
                                volumeCloseTimer.stop()
                            }
                            onExited: {
                                volumeMenu.pointerIn = false
                                volumeCloseTimer.restart()
                            }
                        }
                    }

                    contentItem: Item {
                        /* 音量数字：参考里是 0~100 的整数（VolumeSlider.ts:111） */
                        Text {
                            id: volumeNumber

                            anchors {
                                top: parent.top
                                left: parent.left
                                right: parent.right
                            }
                            height: QtPlayerTheme.volumeNumberHeight
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            color: QtPlayerTheme.volumeNumberText
                            font.pixelSize: QtPlayerTheme.ctrlTimeFontSize
                            text: Math.round(volumeMenu.shownVolume * 100)
                        }

                        /* 竖向滑轨：底 = 最大 */
                        Item {
                            id: volumeSlider

                            anchors {
                                top: volumeNumber.bottom
                                topMargin: 2
                                horizontalCenter: parent.horizontalCenter
                            }
                            width: QtPlayerTheme.volumeSliderWidth
                            height: QtPlayerTheme.volumeProgressHeight

                            /* 静音时这里是 0 → 进度条和圆点都落到底（数字同源，见 volumeMenu.shownVolume） */
                            readonly property real value: volumeMenu.shownVolume

                            Rectangle {
                                anchors.fill: parent
                                radius: QtPlayerTheme.volumeSliderRadius
                                color: QtPlayerTheme.volumeSliderBg
                            }

                            Rectangle {
                                anchors {
                                    left: parent.left
                                    right: parent.right
                                    bottom: parent.bottom
                                }
                                height: parent.height * volumeSlider.value
                                radius: QtPlayerTheme.volumeSliderRadius
                                color: QtPlayerTheme.volumeThumbBg
                            }

                            /* 手指跟手：底部 = 最大 */
                            Rectangle {
                                width: QtPlayerTheme.volumeThumbSize
                                height: QtPlayerTheme.volumeThumbSize
                                radius: width / 2
                                color: QtPlayerTheme.volumeThumbBg
                                x: (volumeSlider.width - width) / 2
                                y: volumeSlider.height - volumeSlider.height * volumeSlider.value - height / 2
                            }

                            MouseArea {
                                id: volumeSliderArea

                                anchors {
                                    fill: parent
                                    leftMargin: -12
                                    rightMargin: -12
                                }
                                cursorShape: Qt.PointingHandCursor

                                function volumeAt(py) {
                                    if (height <= 0)
                                        return 0

                                    var ratio = 1 - Math.max(0, Math.min(1, py / height))
                                    return Math.round(ratio * 100) / 100
                                }

                                onPressed: function (mouse) {
                                    if (bar.player)
                                        bar.player.volume = volumeAt(mouse.y)
                                }
                                onPositionChanged: function (mouse) {
                                    if (pressed && bar.player)
                                        bar.player.volume = volumeAt(mouse.y)
                                }
                            }
                        }
                    }
                }
            }

            /* ---------------- 字幕 ---------------- */
            /*
             * 参考：`.bpx-player-ctrl-btn.bpx-player-ctrl-subtitle`（platform/subtitles.txt 第一行那棵树）。
             *   .bpx-player-ctrl-subtitle{font-size:14px; margin:0 5px; width:auto;
             *                             flex:0 1 auto; min-width:0}
             *   .bpx-player-ctrl-subtitle-result{cursor:pointer; font-weight:600;
             *                                    white-space:nowrap; font-size:14px}
             *   @media (min-width:750px) + [data-screen=full|web] → font-size:16px
             * 也就是说这颗按钮**没有图标**：就是"字幕"两个字
             * （DOM 里那个胶囊 svg 的 span 是 style="display:none"，所以不画）。
             *
             * 宽度照 width:auto：文字宽 + 左右各 5px（参考的 margin:0 5px）。
             * 我们这一行按钮之间本来就没有外边距（参考每颗是 margin:0 3px），
             * 所以把那 5px 直接折进宽度里 —— 字号跟着 bar.qualityFontSize 走
             * （普通 14 / 全屏 16，和参考两档一致），宽度自然跟着变。
             *
             * 展开面板的写法**和音量/倍速/清晰度完全一样**：
             *   T.Popup + 自己维护 pointerIn + 100ms 容错定时器 + closePolicy + hover 判据区，
             * 细节见下面每一段的注释和 docs/QML-POPUP-HOVER-PITFALLS.md。
             */
            Item {
                id: subtitleButton

                anchors {
                    right: volumeButton.left
                    verticalCenter: parent.verticalCenter
                }
                width: subtitleText.implicitWidth + 2 * rightGroup.textBtnPad
                height: bar.btnHeight

                /*
                 * 浮层尺寸：跟着当前页走（左页 168x311、右页 266x260）。
                 * 位置**不再照参考的 right:-115px，改成和其它三个菜单一样"居中于按钮"**
                 * （音量/倍速/清晰度都是 x:(parent.width-width)/2）—— 用户要求的：
                 * 参考那个 -115 是"右边缘比按钮右边缘外扩 115"，面板底边中心根本不在
                 * "字幕"两个字下面（偏右 98px）。居中之后左页 168、右页 266 都对称长在
                 * 按钮中心两侧；实测 1280 窗口下右页右边缘 1274 < 1280，不会被窗口切。
                 * 判据区读这两个值，不去读浮层的 x/y（浮层尺寸还没定下来时读到 0/NaN，
                 * 判据会退化成一条缝 —— 这个坑 dmSetting 那段踩过）。
                 */
                readonly property real boxWidth: subtitlePanelBody.pageOffset < 0 ? 266 : 168
                readonly property real boxHeight: subtitlePanelBody.pageOffset < 0 ? 260 : 311

                Text {
                    id: subtitleText

                    /* 文字在 btnLineHeight 那条行盒里居中（全屏行盒 32 < 盒高 43） */
                    anchors {
                        top: parent.top
                        left: parent.left
                        right: parent.right
                    }
                    height: bar.btnLineHeight
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    color: subtitleArea.containsMouse ? QtPlayerTheme.ctrlBtnTextHover
                                                      : QtPlayerTheme.ctrlBtnText
                    /* 普通 14 / 全屏 16（参考那两档），和清晰度结果文字同一个值 */
                    font.pixelSize: bar.qualityFontSize
                    font.weight: Font.DemiBold      /* 参考 font-weight:600 */
                    text: qsTr("字幕")
                }

                Timer {
                    id: subtitleCloseTimer

                    /* 只是"从按钮挪到浮层上"那一小段路的容错，不是等待时间 */
                    interval: 100
                    repeat: false
                    onTriggered: {
                        if (!subtitleArea.containsMouse && !subtitleHoverZone.pointerIn
                                && !subtitleMenu.pointerIn)
                            subtitleMenu.close()
                    }
                }

                MouseArea {
                    id: subtitleArea

                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onEntered: {
                        subtitleCloseTimer.stop()
                        subtitleMenu.open()
                    }
                    onExited: subtitleCloseTimer.restart()
                }

                /* 按钮和浮层之间那道缝（41/74px）的判据，位置严格等于"浮层 + 缝"，
                   坐标原点和浮层左上角重合，所以这里喂给面板的 mouseX/mouseY 就是面板坐标 */
                Item {
                    id: subtitleHoverZone

                    parent: subtitleButton
                    visible: subtitleMenu.opened
                    x: (subtitleButton.width - subtitleButton.boxWidth) / 2
                    y: subtitleButton.height - bar.menuBottom - subtitleButton.boxHeight
                    width: subtitleButton.boxWidth
                    height: -y
                    property bool pointerIn: false

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        onEntered: {
                            subtitleHoverZone.pointerIn = true
                            subtitleCloseTimer.stop()
                        }
                        onExited: {
                            subtitleHoverZone.pointerIn = false
                            subtitleCloseTimer.restart()
                        }
                        onPositionChanged: function (mouse) {
                            subtitlePanelBody.pointerX = mouse.x
                            subtitlePanelBody.pointerY = mouse.y
                        }
                    }
                }

                /*
                 * 字幕面板浮层。
                 * 位置：底边距按钮底边 41px（全屏那一档 74px，就是 bar.menuBottom），
                 * 横向**居中于按钮**（和音量/倍速/清晰度一致，不照参考的 right:-115px）。
                 * 尺寸跟着当前页走（参考是 JS 改 bui-panel-wrap 的内联尺寸）。
                 */
                T.Popup {
                    id: subtitleMenu

                    /* 自己维护"鼠标在不在浮层里"；关掉时清零 + 回到左页 + 收起下拉列表 */
                    property bool pointerIn: false

                    onClosed: {
                        pointerIn = false
                        subtitlePanelBody.showPage(false)
                        subtitlePanelBody.resetLists()
                        subtitlePanelBody.pointerX = -1
                        subtitlePanelBody.pointerY = -1
                    }

                    parent: subtitleButton
                    width: subtitleButton.boxWidth
                    height: subtitleButton.boxHeight

                    /* 切页时尺寸做过渡（参考 bui-panel-wrap 的 transition:all .25s）。
                       【不要给 x/y 加 Behavior】浮层的 x/y 是按 width/height 推出来的绑定，
                       再叠动画会和框架自己的定位打架。 */
                    Behavior on width {
                        NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
                    }
                    Behavior on height {
                        NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
                    }

                    /* 位置照其它三个菜单：居中于按钮（见上面 boxWidth 那段说明） */
                    x: (subtitleButton.width - width) / 2
                    y: subtitleButton.height - bar.menuBottom - height

                    padding: 0
                    /* 面板里全是下拉/滑块/勾选，点它们不能关；关面板交给 hover / Esc / 点外面 */
                    /* 【只靠"移出关闭"】Esc 是退全屏用的、也不要点击面板外关闭 */
                    closePolicy: T.Popup.NoAutoClose
                    enter: Transition {}
                    exit: Transition {}

                    /* 浮层那层判据：严格等于浮层矩形（必须自己撑尺寸，模板不会给 background 定尺寸） */
                    background: Rectangle {
                        width: subtitleMenu.width
                        height: subtitleMenu.height
                        color: QtPlayerTheme.menuBg
                        radius: QtPlayerTheme.menuRadius

                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            onEntered: {
                                subtitleMenu.pointerIn = true
                                subtitleCloseTimer.stop()
                            }
                            onExited: {
                                subtitleMenu.pointerIn = false
                                subtitleCloseTimer.restart()
                                subtitlePanelBody.pointerX = -1
                                subtitlePanelBody.pointerY = -1
                            }
                            onPositionChanged: function (mouse) {
                                subtitlePanelBody.pointerX = mouse.x
                                subtitlePanelBody.pointerY = mouse.y
                            }
                        }
                    }

                    contentItem: SubtitlePanel {
                        id: subtitlePanelBody
                    }
                }
            }

            /* ---------------- 选集（只在全屏显示） ---------------- */
            /*
             * 参考：front/hili-player/player（仅参考不要动里面的代码）/
             *   src/component/controls/index.ts:1356-1381（DOM）
             *   + src/index.scss:2803-2897（按钮与被展开的面板）
             *
             * 按钮顺序（参考 RightControls.ts:448）：清晰度 → **选集** → 倍速 → 音量 → 设置 → 全屏。
             *
             * 【可见性照参考的规则，不是我们自己定的】参考里：
             *   .player-ctrl-eplist { visibility: hidden; width: 0 }   ← 普通窗口（index.scss:2803-2806）
             *   [data-screen="full"] … { visibility: visible }          ← 全屏（index.scss:4794-4798）
             * 参考在 full 和 web 两种模式下都显示，**我们只取 full 那一半**（用户要求：只有全屏才显示）。
             * 非全屏时隐藏**并且宽度归零** —— 参考的 width:0 就是这个意思，右边那几颗按钮不会跟着挪。
             *
             * 【用户后来又加了一条】列表里**只有一个视频**时也不显示这颗按钮（没有"选集"可言），
             * 处理方式和非全屏完全一样（隐藏 + 宽度归零）。判据就是 `bar.episodes.length > 1`。
             *
             * 面板写法**和音量/倍速/清晰度完全一致**：T.Popup + 自己维护 pointerIn + 100ms 容错定时器
             * + closePolicy + 独立的 hover 判据区。参考里面板是按钮的**子元素**、靠 300ms 延迟保持打开，
             * 那是 DOM 的子树语义；QML 里 hover 只看几何（见 docs/QML-POPUP-HOVER-PITFALLS.md 第 9 条），
             * 所以"按钮 + 缝 + 面板"必须显式给判据区。
             *
             * 参考面板的数值：宽 320、底 hsla(0,0%,8%,.9)、圆角 2、bottom 41px（全屏 74px）、
             * 水平居中对齐按钮（left:50% + translateX(-50%)）、max-height 580、列表区 max-height 520、
             * 行高 30、行内边距 0 20px、12px 字、hover rgba(1,1,1,.1)、
             * 当前集 = 主题色文字 + 12px 三柱 PlayingIcon（assets/images/eplist-playing.svg，参考 icons/index.ts:843-848）。
             * 参考**没有**分组标题、没有滚动条样式、没有自动滚到当前集、也没有选中集的处理 ——
             * 这些我们都不自己加；只是列表超出 520px 时给它可滚动（参考是 overflow:hidden 直接裁掉，
             * 裁掉以后后面的集永远点不到，那不能算"一样"）。
             */
            Item {
                id: eplistButton

                /*
                 * 【锚点：插进"从右往左串"的那条链里，不是找某个空隙】
                 * 这一组**每一个**按钮都是从右往左串的：
                 *     全屏.right = 组.right
                 *     设置.right = 全屏.left
                 *     音量.right = 设置.left
                 *     字幕.right = 音量.left
                 *     倍速.right = 字幕.left
                 *     清晰度.right = **倍速.left**   ← 注意清晰度也是串的，不是钉在组左边！
                 * 所以"插在清晰度和倍速之间"只能这么写：
                 *     选集.right = 倍速.left
                 *     清晰度.right = 选集.left
                 * 【两次踩坑】
                 *   ① 第一次：选集也写 `right: 倍速.left` —— 和清晰度抢**同一个槽位**，
                 *      两块直接叠在一起（用户看到的就是"选集和清晰度重合"）；
                 *   ② 第二次：以为清晰度是 `left: parent.left` 钉住的，就把选集写成
                 *      `left: 清晰度.right` —— 结果它落到倍速左边缘的**右边**，压住倍速。
                 * 正确做法就下面两行：一个改自己的锚点，一个把清晰度的锚点改到选集上。
                 */
                anchors {
                    right: rateButton.left
                    verticalCenter: parent.verticalCenter
                }

                /* 参考：非全屏 visibility:hidden + width:0（隐藏时不占位）
                   【用户要求】**只有一个视频时也不显示**：一个视频没有"选集"可言，
                   这时整颗按钮按非全屏那套处理（隐藏 + 宽度归 0），右边那几颗不会跟着挪。 */
                visible: bar.fullscreen && bar.episodes.length > 1

                /*
                 * 宽度用 **TextMetrics 量**，不要去读 eplistText.implicitWidth：
                 * 那样会形成绑定环 ——
                 *     按钮.width ← 文字.implicitWidth ← 文字.width ← 按钮.width（文字左右都锚在按钮上）
                 * QML 会在启动时报 `Binding loop detected for property "width"`（实测日志里有一条，
                 * 就在这一行）。TextMetrics 是独立量文本的，和按钮宽度无关，环就断了。
                 * 文案是常量，所以这里量一次就够。
                 */
                TextMetrics {
                    id: eplistMetrics

                    font: eplistText.font
                    text: qsTr("选集")
                }
                width: visible ? eplistMetrics.advanceWidth + 2 * rightGroup.textBtnPad : 0
                height: bar.btnHeight

                /* 面板尺寸（浮层和判据区都用这两个值算，不去读浮层的 x/y/height） */
                readonly property int rowHeight: 30                 /* 参考 .player-ctrl-eplist-multi-menu-item height:30px */
                readonly property int panelWidth: 320               /* 参考 .player-ctrl-eplist-menu-wrap width:320px */
                readonly property int panelMaxHeight: 520           /* 参考 section-bottom max-height:520px（≈17 行） */
                readonly property int panelMinHeight: 180           /* 参考内联 min-height:180px */
                readonly property int panelHeight:
                    Math.max(panelMinHeight,
                             Math.min(panelMaxHeight, bar.episodes.length * rowHeight))

                Text {
                    id: eplistText

                    anchors {
                        top: parent.top
                        left: parent.left
                        right: parent.right
                    }
                    height: bar.btnLineHeight
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    color: eplistArea.containsMouse ? QtPlayerTheme.ctrlBtnTextHover
                                                    : QtPlayerTheme.ctrlBtnText
                    /* 参考：普通 14px、全屏 16px（index.scss:2804 / 5026-5029），和清晰度同一个值 */
                    font.pixelSize: bar.qualityFontSize
                    font.weight: Font.DemiBold          /* 参考 font-weight:600（index.scss:2809） */
                    text: qsTr("选集")
                }

                Timer {
                    id: eplistCloseTimer

                    /* 只是"从按钮挪到浮层上"那一小段路的容错，不是等待时间 */
                    interval: 100
                    repeat: false
                    onTriggered: {
                        if (!eplistArea.containsMouse && !eplistHoverZone.pointerIn
                                && !eplistMenu.pointerIn)
                            eplistMenu.close()
                    }
                }

                MouseArea {
                    id: eplistArea

                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onEntered: {
                        eplistCloseTimer.stop()

                        /* 没有集数数据就不弹空面板（和清晰度那颗按钮同一个处理）；
                           **只有一个视频也不弹** —— 一个视频没有"选集"可言（用户要求），
                           和上面那颗按钮的显隐判据保持一致。 */
                        if (bar.episodes.length > 1)
                            eplistMenu.open()
                    }
                    onExited: eplistCloseTimer.restart()
                }

                /* 按钮上面那一段（缝 + 浮层）的判据区，写法说明见音量那段 */
                Item {
                    id: eplistHoverZone

                    parent: eplistButton
                    visible: eplistMenu.opened
                    x: (eplistButton.width - eplistButton.panelWidth) / 2
                    y: -(bar.menuBottom + eplistButton.panelHeight)
                    width: eplistButton.panelWidth
                    height: -y
                    property bool pointerIn: false

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        onEntered: {
                            eplistHoverZone.pointerIn = true
                            eplistCloseTimer.stop()
                        }
                        onExited: {
                            eplistHoverZone.pointerIn = false
                            eplistCloseTimer.restart()
                        }
                    }
                }

                /* 面板浮层：位置照参考（底边距按钮底边 bar.menuBottom = 41/74，水平居中于按钮） */
                T.Popup {
                    id: eplistMenu

                    /* 自己维护"鼠标在不在浮层里"（框架的 Popup 没有可用的 hovered），关掉时清零 */
                    property bool pointerIn: false

                    /*
                     * 鼠标停在第几行上（-1 = 没有）。
                     *
                     * 【本轮修的 bug：列表滚动之后高亮错行】
                     * 原来这里写的是 `Math.floor(eplistHover.mouseY / rowHeight)` —— mouseY 是
                     * **视口坐标**，而列表一旦滚动（contentY > 0），行在视口里的位置整体上移了
                     * contentY，mouseY 却还是从视口顶算起 → 算出来的行号**偏小**。
                     * 现象就是用户报的："鼠标移到下面那一项上，亮的却是上面那一项"
                     * （差几行 = contentY / 行高；刚好滚了一两行时，亮的就是紧挨着的上一行）。
                     *
                     * 现在改成用 `mapToItem` 把鼠标点映射进**行列自己的坐标系**再除行高：
                     * 滚动量、以后加内边距 / 再套一层容器，都由映射负责，不用手算。
                     * （同一个套路在 SubtitlePanel.qml 的 pointerOn() 里已经用过 —— 那里面板
                     * 的下拉列表也是可滚动的，hover 一直是准的。）
                     *
                     * 【为什么依赖里要显式写 contentY / height】滚轮滚动时鼠标是不动的：
                     * 只有 mouseX/mouseY 一个依赖的话，滚动后这个绑定不会重算，
                     * 高亮会停在旧行上。所以把 contentY / 行数也读一下，当成依赖。
                     *
                     * 【pointerIn 的检查必须放在最前面】那两行是"读一下当依赖"，本身没有副作用；
                     * 但绑定第一次求值时（pointerIn 还是 false）background/contentItem 里的
                     * 对象可能还没建好，先读它们会抛 TypeError。鼠标没进面板时提前 return 就没这问题，
                     * 而且 pointerIn 本身是依赖 —— 它一变真，绑定会重算，那时才把 contentY 记成依赖。
                     */
                    readonly property int hoverIndex: {
                        if (!pointerIn)
                            return -1

                        eplistFlick.contentY            /* 依赖：滚轮滚动（鼠标不动）也要重算 */
                        eplistColumn.height             /* 依赖：行数 / 行高变化 */

                        return rowAt(eplistHover.mouseX, eplistHover.mouseY)
                    }

                    /*
                     * 浮层坐标 (x, y) → 命中第几行（-1 = 没命中任何一行）。
                     *
                     * 坐标系以 eplistHover（浮层背景那层判据 MouseArea）为基准 ——
                     * 和 mouse.x / mouse.y、mouseX / mouseY 是同一套，调用方直接传即可。
                     * 映射到 eplistColumn 之后，y 就是**内容坐标**（已经含滚动量）。
                     */
                    function rowAt(x, y) {
                        var n = bar.episodes ? bar.episodes.length : 0

                        if (n <= 0 || !eplistHover || !eplistColumn || eplistColumn.height <= 0)
                            return -1

                        var p = eplistHover.mapToItem(eplistColumn, x, y)

                        if (p.y < 0 || p.y >= eplistColumn.height)
                            return -1

                        var i = Math.floor(p.y / eplistButton.rowHeight)
                        return (i >= 0 && i < n) ? i : -1
                    }

                    /*
                     * 选中某一集（换片 + 关面板）。
                     *
                     * 【为什么做成一个函数】原来只有"行自己的 MouseArea"这一条路：
                     *     onClicked → bar.episodeSelected(...) + eplistMenu.close()
                     * 用户实测点了没反应（根因见下面行 MouseArea 那段注释：滚轮滚动后
                     * 按下事件会被 Flickable 过滤掉）。现在**两条路都调它**：
                     *   ① 行的 MouseArea（按下落到行上时）；
                     *   ② 浮层判据层 eplistHover 的 onClicked（按下没落到行上时的兜底）。
                     * 一次点击只会命中其中一条（按下事件只投递给一个 item），不会重复换片；
                     * 逻辑也只有这一份，不会两边走偏。
                     */
                    function activate(index) {
                        var n = bar.episodes ? bar.episodes.length : 0

                        if (index < 0 || index >= n)
                            return

                        bar.episodeSelected(index, bar.episodes[index])
                        eplistMenu.close()
                    }

                    onClosed: pointerIn = false

                    parent: eplistButton
                    width: eplistButton.panelWidth
                    height: eplistButton.panelHeight

                    x: (parent.width - width) / 2
                    y: parent.height - bar.menuBottom - height

                    padding: 0
                    /* 【只靠"移出关闭"】Esc 是退全屏用的、也不要点击面板外关闭 */
                    closePolicy: T.Popup.NoAutoClose
                    enter: Transition {}
                    exit: Transition {}

                    /* 浮层那层判据：必须自己撑尺寸（模板不会给 background 定尺寸）。
                       浮层里只有这一处收 hover，所以它一定收得到。 */
                    background: Rectangle {
                        width: eplistMenu.width
                        height: eplistMenu.height
                        color: QtPlayerTheme.menuBg
                        radius: QtPlayerTheme.menuRadius

                        MouseArea {
                            id: eplistHover

                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onEntered: {
                                eplistMenu.pointerIn = true
                                eplistCloseTimer.stop()
                            }
                            onExited: {
                                eplistMenu.pointerIn = false
                                eplistCloseTimer.restart()
                            }

                            /*
                             * 【本轮加的兜底】按下没落到任何一行上时（比如落在面板的空白带里），
                             * 也按同一套坐标换算补一刀；落在空白处 rowAt 返回 -1，什么也不做。
                             *
                             * 正常情况下这条不会触发 —— 按下落在行上时由行那层接走（一个按下事件
                             * 只投递给一个 item），所以不会和行那条路重复换集。
                             * 留着是因为它读的就是这一层自己的坐标，行那层哪天被别的层盖住时
                             * 这里还能兜住，而这一层是"鼠标在不在浮层里"（hover 高亮）实际在用的那层。
                             */
                            onClicked: function (mouse) {
                                eplistMenu.activate(eplistMenu.rowAt(mouse.x, mouse.y))
                            }
                        }
                    }

                    contentItem: Flickable {
                        id: eplistFlick

                        /*
                         * 参考里列表区是 overflow:hidden（超出 520px 直接裁掉，后面的集点不到）；
                         * 这里改成可滚动 —— 只有"内容超过可视区"时才滚，正常集数下和参考看不出区别。
                         * 用 Flickable 是因为滚轮事件不需要额外的 MouseArea（行里的 MouseArea 不收滚轮，
                         * 事件会往上冒到这里）。
                         */
                        contentHeight: eplistColumn.height
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        interactive: contentHeight > height

                        /*
                         * 滚动条。
                         *
                         * 【参考里没有这个】参考的列表区是 `overflow:hidden`（超出 520px 直接裁掉，
                         * 后面的集根本点不到），压根没有滚动条样式可抄；我们要可滚动，就得自己画一条。
                         * 样式跟项目里另外两条保持一致（字幕面板的语言列表 / 弹幕设置面板的右页）：
                         * 3px 宽、`#e5e7ef`、贴视口右边缘、圆角 1.5。
                         *
                         * 【为什么 x/y 要加 contentX/contentY】Flickable 的子项都在**内容坐标系**里，
                         * 只写 y 会跟着列表一起滚走。加上 contentX/contentY 把滚动量抵消掉，
                         * 它就固定在视口上了 —— 这样不用再套一层 Item 去装"Flickable + 滚动条"。
                         *
                         * 只有内容超出可视区时才出现（集数少的时候和参考一模一样，不会多出一条）；
                         * 鼠标在面板上时亮一点。
                         */
                        Rectangle {
                            id: eplistScrollBar

                            visible: eplistFlick.contentHeight > eplistFlick.height
                            width: 3
                            radius: 1.5
                            color: "#e5e7ef"
                            opacity: eplistMenu.pointerIn ? 0.45 : 0.25
                            z: 50
                            x: eplistFlick.contentX + eplistFlick.width - width
                            y: eplistFlick.contentY
                               + eplistFlick.visibleArea.yPosition * eplistFlick.height
                            height: Math.max(16, eplistFlick.visibleArea.heightRatio * eplistFlick.height)

                            Behavior on opacity {
                                NumberAnimation { duration: 200 }
                            }
                        }

                        Column {
                            id: eplistColumn

                            width: eplistFlick.width

                            Repeater {
                                model: bar.episodes

                                Item {
                                    id: eplistRow

                                    required property var modelData
                                    required property int index

                                    readonly property bool current: index === bar.currentEpisodeIndex
                                    readonly property bool hovered: eplistMenu.hoverIndex === index

                                    width: eplistColumn.width
                                    height: eplistButton.rowHeight

                                    /* hover 底色：参考 :hover { background-color: hsla(0,0%,100%,.1) } */
                                    Rectangle {
                                        anchors.fill: parent
                                        color: eplistRow.hovered ? QtPlayerTheme.menuHoverBg : "transparent"
                                    }

                                    /*
                                     * 当前集前面的三柱图标：参考里**只有当前集**有这个 span
                                     * （index.ts:1364-1368；port 版给每一行都渲染了空 span，那是它的 bug）。
                                     * 槽宽 12、右边距 4（index.scss:2857-2864）。
                                     */
                                    Image {
                                        id: eplistPlayingIcon

                                        visible: eplistRow.current
                                        anchors {
                                            left: parent.left
                                            leftMargin: 20                  /* 行内边距 padding: 0 20px */
                                            verticalCenter: parent.verticalCenter
                                        }
                                        width: 12
                                        height: 13
                                        source: "../assets/images/eplist-playing.svg"
                                        sourceSize: Qt.size(12, 13)
                                        smooth: true
                                    }

                                    Text {
                                        anchors {
                                            left: eplistPlayingIcon.visible ? eplistPlayingIcon.right
                                                                            : parent.left
                                            leftMargin: eplistPlayingIcon.visible ? 4 : 20
                                            right: parent.right
                                            rightMargin: 20
                                            verticalCenter: parent.verticalCenter
                                        }
                                        elide: Text.ElideRight                  /* 参考 text-overflow:ellipsis */
                                        text: (eplistRow.modelData && eplistRow.modelData.title !== undefined)
                                              ? eplistRow.modelData.title : ""
                                        font.pixelSize: 12                      /* 参考 font-size:12px */
                                        /* 参考：普通行继承按钮的 0.8 白，当前集是主题色（index.scss:2881-2884） */
                                        color: eplistRow.current ? QtPlayerTheme.qualityActiveText
                                                                 : QtPlayerTheme.ctrlBtnText
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor

                                        /*
                                         * 【本轮修"点击没反应"的关键】
                                         * 能滚的 `Flickable`（`interactive: true`）会**过滤掉
                                         * 子项的按下事件** —— 实测的现象是：**滚轮滚动之后的那一拍
                                         * 点击会丢**（行收不到按下，于是 `onClicked` 不触发），
                                         * 而不滚动直接点、或者按下后抖 2px 再松，都是正常的
                                         * （所以"拖动超阈值才被抢"那种解释是不对的，
                                         *  说明见 docs/QML-POPUP-HOVER-PITFALLS.md 第 22 条，
                                         *  结论来自 qmltestrunner 复刻面板骨架的实测：
                                         *  同一条用例，没有本行时 clicked = -1，有本行时 clicked = 2）。
                                         *
                                         * `preventStealing` 正是为这件事准备的：它内部就是
                                         * `setKeepMouseGrab(true)`，Flickable 看到子项 keepMouseGrab
                                         * 就不再过滤/抢 → 滚轮之后再点也一定成立。
                                         *
                                         * 代价：**在行上按住拖动不再能滚列表**（行占满整个视口，所以拖动
                                         * 滚动基本等于关了）。滚轮照旧，而参考里这个列表本来就不可滚
                                         * （`overflow:hidden`）——先用滚轮 + 保住点击。
                                         */
                                        preventStealing: true

                                        /* 参考里"选中某一集"是**没实现的**（eplistChange 只声明没发射），
                                           所以这一下是我们自己定义的出口：交给应用层去换片。
                                           （和浮层判据层那条路共用 activate()，见上面的说明） */
                                        onClicked: eplistMenu.activate(eplistRow.index)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            /* ---------------- 倍速 ---------------- */
            Item {
                id: rateButton

                anchors {
                    /* 右边挨着新加的"字幕"按钮（字幕插在音量/倍速之间，和参考的按钮顺序一致：
                       清晰度 · 倍速 · 字幕 · 音量 · 设置 · 全屏） */
                    right: subtitleButton.left
                    verticalCenter: parent.verticalCenter
                }
                /* 宽度：以固定值 rateWidth 为下限（"倍速"两个字时正好是它），
                   标签变长（1.25X）时按同一内边距撑开 —— 内边距始终 = textBtnPad。 */
                width: Math.max(QtPlayerTheme.rateWidth, rateText.implicitWidth + 2 * rightGroup.textBtnPad)
                height: bar.btnHeight

                property bool open: false

                Text {
                    id: rateText

                    /* 文字在 btnLineHeight 那条行盒里居中（全屏行盒 32 < 盒高 43） */
                    anchors {
                        top: parent.top
                        left: parent.left
                        right: parent.right
                    }
                    height: bar.btnLineHeight
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    color: rateArea.containsMouse ? QtPlayerTheme.ctrlBtnTextHover : QtPlayerTheme.ctrlBtnText
                    font.pixelSize: bar.qualityFontSize
                    font.bold: true
                    /* 参考 PlaybackRateMenu.ts:131：1 倍显示"倍速"，否则 "1.25X" */
                    text: {
                        var r = bar.player ? bar.player.playbackRate : 1
                        return Math.abs(r - 1) < 0.001 ? qsTr("倍速") : (r + "X")
                    }
                }

                Timer {
                    id: rateCloseTimer

                    /* 只是"从图标挪到浮层上"那一小段路的容错，不是等待时间 */
                    interval: 100
                    repeat: false
                    /* 到点再确认一次：图标、缝里、浮层里都不在鼠标下才关 */
                    onTriggered: {
                        if (!rateArea.containsMouse && !rateHoverZone.pointerIn
                                && !rateMenu.pointerIn)
                            rateMenu.close()
                    }
                }

                MouseArea {
                    id: rateArea

                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onEntered: {
                        rateCloseTimer.stop()
                        rateMenu.open()
                    }
                    onExited: rateCloseTimer.restart()
                }

                /* 图标上面那一段（浮层 + 中间那条缝），判据说明见音量那段 */
                Item {
                    id: rateHoverZone

                    parent: rateButton
                    visible: rateMenu.opened
                    x: rateMenu.x
                    y: rateMenu.y
                    width: Math.max(rateButton.width, rateMenu.width)
                    height: -rateMenu.y
                    property bool pointerIn: false

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onEntered: {
                            rateHoverZone.pointerIn = true
                            rateCloseTimer.stop()
                        }
                        onExited: {
                            rateHoverZone.pointerIn = false
                            rateCloseTimer.restart()
                        }
                    }
                }

                /*
                 * 倍速菜单：用 QtQuick.Templates 的 Popup（框架自带的浮层），不再用自绘 Item。
                 *
                 * 【为什么换成 Popup】自绘菜单是控制层里的普通子元素，它和按钮、进度条、容器之间
                 * **互相抢 hover**：谁挡住谁、enter/exit 谁先到都不确定 —— 之前"移到选择框上就消失"、
                 * "控制栏一直闪烁"全是这一类问题（音量弹层没出问题，只是因为它里面没有子项）。
                 *
                 * Popup 是**浮层**：出现时独占指针事件，下面的元素根本收不到，压根不存在抢夺；
                 * 关不关由 closePolicy（点外面/Esc）+ 浮层自己那层 MouseArea 的 containsMouse 决定。
                 * 这也是 Qt 6 官方推荐的浮层做法，见 Qt 博客 Popups and Menus in Qt Quick 6.8。
                 */
                T.Popup {
                    id: rateMenu

                    /* 自己维护"鼠标在不在浮层里"（框架的 Popup 没有可用的 hovered 属性），关掉时清零 */
                    property bool pointerIn: false

                    onClosed: {
                        pointerIn = false
                        rateHoverZone.pointerIn = false
                    }

                    /*
                     * 鼠标停在第几项上（-1 = 没有）。
                     * 菜单项自己**不带 hover**（带了就会和浮层这层判据互相抢，谁挡谁不确定），
                     * 高亮统一按鼠标在浮层里的位置算出来。
                     */
                    readonly property int hoverIndex: {
                        if (!pointerIn)
                            return -1

                        var i = Math.floor(rateMenuHover.mouseY / QtPlayerTheme.rateItemHeight)
                        return (i >= 0 && i < QtPlayerTheme.rateOptions.length) ? i : -1
                    }

                    parent: rateButton
                    width: QtPlayerTheme.rateMenuWidth
                    height: contentItem.implicitHeight
                    /* 居中于按钮；底边距按钮底边 41px（全屏 74px）——参考 controls.scss:912-921 */
                    x: (parent.width - width) / 2
                    y: parent.height - bar.menuBottom - height

                    padding: 0
                    /* Esc / 点外面关闭；点菜单项由项自己关 */
                    /* 【只靠"移出关闭"】Esc 是退全屏用的、也不要点击面板外关闭 */
                    closePolicy: T.Popup.NoAutoClose
                    /* 参考里菜单是直接显隐，没有进出动画 */
                    enter: Transition {}
                    exit: Transition {}

                    /*
                     * 浮层那层"鼠标在不在里面"的判据，说明见音量浮层那段
                     * （必须 anchors.fill 撑满：浮层模板不会给 background 定尺寸）。
                     * 浮层里只有这一处收 hover，所以它一定收得到。
                     */
                    background: Rectangle {
                        /* 显式按浮层尺寸来（不靠模板、也不靠 anchors） */
                        width: rateMenu.width
                        height: rateMenu.height
                        color: QtPlayerTheme.menuBg
                        radius: QtPlayerTheme.menuRadius

                        MouseArea {
                            id: rateMenuHover

                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onEntered: {
                                rateMenu.pointerIn = true
                                rateCloseTimer.stop()
                            }
                            onExited: {
                                rateMenu.pointerIn = false
                                rateCloseTimer.restart()
                            }
                        }
                    }

                    contentItem: Column {
                        id: rateColumn

                        Repeater {
                            /* 顺序照抄参考 RightControls.ts:59 */
                            model: QtPlayerTheme.rateOptions

                            Rectangle {
                                required property var modelData
                                /* index 必须也声明成 required：委托里一旦有 required 属性，
                                   没声明的 index 就不会注入（清晰度那份就是这么写的） */
                                required property int index

                                width: QtPlayerTheme.rateMenuWidth
                                height: QtPlayerTheme.rateItemHeight
                                color: rateMenu.hoverIndex === index ? QtPlayerTheme.menuHoverBg : "transparent"

                                Text {
                                    anchors.centerIn: parent
                                    color: (bar.player && Math.abs(bar.player.playbackRate - modelData) < 0.001)
                                           ? QtPlayerTheme.qualityActiveText : QtPlayerTheme.menuText
                                    font.pixelSize: QtPlayerTheme.ctrlTimeFontSize
                                    text: modelData === 1 ? "1.0X" : (modelData + "X")
                                }

                                MouseArea {
                                    id: rateItemArea

                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        if (bar.player)
                                            bar.player.playbackRate = modelData

                                        rateMenu.close()
                                    }
                                }
                            }
                        }
                    }
                }
            }

            /* ---------------- 清晰度 ---------------- */
            Item {
                id: qualityButton

                anchors {
                    /* 清晰度的右边接**选集**（原来接的是倍速）—— 这样"清晰度 → 选集 → 倍速"
                       才成立；锚点说明见 eplistButton 那一段。 */
                    right: eplistButton.left
                    verticalCenter: parent.verticalCenter
                }
                /*
                 * 宽度 = 文字宽 + 两边各 9px。
                 *
                 * 【为什么是 9】9px 就是"倍速"那颗按钮的左右内边距：它宽度固定
                 * QtPlayerTheme.rateWidth = 50，全屏 16px 字号下"倍速"两个字约 32px，
                 * 两边正好各 9px。清晰度原来写的是 max(文字宽, 40)，两边只剩 3~4px ——
                 * 所以只有"清晰度 ↔ 选集"这一档比"选集 ↔ 倍速"、"倍速 ↔ 字幕"看着挤。
                 * 现在按同一套内边距算，四颗文字按钮（清晰度/选集/倍速/字幕）的间隙就一致了。
                 */
                width: qualityText.implicitWidth + 2 * rightGroup.textBtnPad
                height: bar.btnHeight

                property bool open: false
                readonly property bool hasQuality: bar.player && bar.player.qualities.length > 0
                /*
                 * 当前这一档的名字（= 结果文字）。
                 *
                 * 【用户要求】"底部控制栏按钮显示当前的清晰度"：**不管自动还是手动**，
                 * 显示的都是**当前真实在播的那一档**（自动模式下就是 ABR 选中的那一档）；
                 * "自动"这个状态由右上角那颗小角标表达（见 autoBadge），不再用文字顶替。
                 */
                readonly property string currentLabel: {
                    if (!bar.player || bar.player.qualityIndex < 0)
                        return qsTr("自动")

                    var list = bar.player.qualities
                    var i = bar.player.qualityIndex
                    return (i >= 0 && i < list.length) ? list[i].label : qsTr("自动")
                }

                readonly property bool autoMode: bar.player ? bar.player.autoQuality : true
                /*
                 * 正在切档（内核 STARTED → READY/FAILED/CANCELED）。数据源就是
                 * `player.qualitySwitching`（与 qualitySwitchingStreamIndex 同一个 NOTIFY 信号），
                 * 所以这里**不需要**任何计时器：点完立刻出现，READY/失败/取消立刻消失。
                 *
                 * 【现实提醒】单解码器切档实测 30~90ms，所以下面那颗角标**只闪一下**；
                 * "切完了"的可感反馈是档位文案/高亮在同一拍内换成新档（见 currentLabel）。
                 */
                readonly property bool switching: bar.player ? bar.player.qualitySwitching : false

                Text {
                    id: qualityText

                    /* 文字在 btnLineHeight 那条行盒里居中（全屏行盒 32 < 盒高 43） */
                    anchors {
                        top: parent.top
                        left: parent.left
                        right: parent.right
                    }
                    height: bar.btnLineHeight
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    color: qualityArea.containsMouse ? QtPlayerTheme.ctrlBtnTextHover : QtPlayerTheme.ctrlBtnText
                    font.pixelSize: bar.qualityFontSize
                    font.bold: true
                    text: qualityButton.currentLabel
                }

                /*
                 * 「自动」角标（**只有自动模式才显示**）—— 复刻参考的 DOM/CSS：
                 *
                 *     .bpx-player-ctrl-quality-result-wrap            { display:inline-block; position:relative }
                 *     .bpx-player-ctrl-quality-result-auto-badge      { position:absolute; left:100%; top:0;
                 *         margin-left:-12px; margin-top:-10px; height:11px; font-size:7px; line-height:1;
                 *         padding:0 2px; border-radius:3px; color:hsla(0,0%,100%,.5);
                 *         background:hsla(0,0%,100%,.12); border:.5px solid hsla(0,0%,100%,.3) }
                 *     （全屏/网页档：height:13px; font-size:8px; margin-left:-14px; padding:0 3px）
                 *
                 * QML 的两处等价换算：
                 *   * `left:100% + margin-left:-12px` → 角标左边缘 = 名字右边 - 12px
                 *     （我们的名字是**居中**在按钮里的，右边 = 按钮宽 - 内边距 9 → 见 x 的表达式）；
                 *   * `margin-top:-10px`（相对名字行盒）→ 角标顶边比名字行盒顶边高 10px；
                 *     我们的名字行盒从按钮顶边开始，所以 y = -10 会把角标顶到按钮上方 ——
                 *     按钮上方正好是控制栏的空白区，不会被裁（这一点和参考的观感一致）。
                 *   * CSS 的 0.5px 边框在 QML 里做不到（Rectangle.border.width 是整数），
                 *     用 1px + 40% 白近似。
                 */
                Rectangle {
                    id: autoBadge

                    /*
                     * 「切换中」角标与它**共用同一个位置**（都在按钮上方那一小块空白区，
                     * 见上面那段坐标说明），所以两者互斥：正在切档时先让位给"切换中"
                     * （切档实测只 30~90ms，切完立刻回到原样）。
                     */
                    visible: qualityButton.autoMode && !qualityButton.switching
                    readonly property bool big: bar.fullscreen
                    x: qualityText.width - (big ? 14 : 12)
                    y: -10 + (bar.btnLineHeight - height) / 2
                    width: autoBadgeText.implicitWidth + (big ? 6 : 4)   /* padding: 0 3px / 0 2px */
                    height: big ? 13 : 11
                    radius: 3
                    color: Qt.rgba(1, 1, 1, 0.12)
                    border.width: 1
                    border.color: Qt.rgba(1, 1, 1, 0.3)

                    Text {
                        id: autoBadgeText

                        anchors.centerIn: parent
                        color: Qt.rgba(1, 1, 1, 0.5)
                        /* font-size: 8px（全屏/网页档）/ 7px */
                        font.pixelSize: autoBadge.big ? 8 : 7
                        font.weight: Font.Normal
                        /* line-height: 1 —— 单行不让 QML 的默认行距把字推下去 */
                        lineHeight: 1.0
                        text: qsTr("自动")
                    }
                }

                /*
                 * 「切换中」角标（**只在切档那 30~90ms 出现**）。
                 *
                 * 【为什么要它】用户报"为什么没有切换中的 UI 提示"：`qualitySwitching` 这个属性
                 * 以前 QML 一处都没用，所以提示不是"太快看不见"，而是**从来没接**。
                 *
                 * 样式照搬上面的 autoBadge（同一套小圆角标签：半透明白底 + 1px 描边 + 7/8px 字），
                 * 只是**文字用主题强调色**（QtPlayerTheme.qualityActiveText / playerAccent）——
                 * 不新增任何主题常量，也和"选中档位"的高亮色是同一个色。
                 *
                 * 位置、尺寸表达式与 autoBadge 完全一致（共用按钮上方那块空白区，两者互斥），
                 * 所以**不会挤动按钮宽度**：按钮宽度只由 qualityText.implicitWidth 决定，
                 * 这颗角标不参与布局（否则控制栏整条会跟着抖一下）。
                 */
                Rectangle {
                    id: switchingBadge

                    visible: qualityButton.switching
                    readonly property bool big: bar.fullscreen
                    x: qualityText.width - (big ? 14 : 12)
                    y: -10 + (bar.btnLineHeight - height) / 2
                    width: switchingBadgeText.implicitWidth + (big ? 6 : 4)
                    height: big ? 13 : 11
                    radius: 3
                    color: Qt.rgba(1, 1, 1, 0.12)
                    border.width: 1
                    border.color: QtPlayerTheme.qualityActiveText

                    Text {
                        id: switchingBadgeText

                        anchors.centerIn: parent
                        color: QtPlayerTheme.qualityActiveText
                        font.pixelSize: switchingBadge.big ? 8 : 7
                        font.weight: Font.Normal
                        lineHeight: 1.0
                        text: qsTr("切换中")
                    }
                }

                Timer {
                    id: qualityCloseTimer

                    /* 只是"从图标挪到浮层上"那一小段路的容错，不是等待时间 */
                    interval: 100
                    repeat: false
                    /* 到点再确认一次：图标、缝里、浮层里都不在鼠标下才关（原因见 rateCloseTimer） */
                    onTriggered: {
                        if (!qualityArea.containsMouse && !qualityHoverZone.pointerIn
                                && !qualityMenu.pointerIn)
                            qualityMenu.close()
                    }
                }

                MouseArea {
                    id: qualityArea

                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onEntered: {
                        qualityCloseTimer.stop()

                        /* 没有清晰度数据就不弹空菜单（参考里也是按 config 条件渲染） */
                        if (qualityButton.hasQuality)
                            qualityMenu.open()
                    }
                    onExited: qualityCloseTimer.restart()
                }

                /* 图标上面那一段（浮层 + 中间那条缝），判据说明见音量那段 */
                Item {
                    id: qualityHoverZone

                    parent: qualityButton
                    visible: qualityMenu.opened
                    x: qualityMenu.x
                    y: qualityMenu.y
                    width: Math.max(qualityButton.width, qualityMenu.width)
                    height: -qualityMenu.y
                    property bool pointerIn: false

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onEntered: {
                            qualityHoverZone.pointerIn = true
                            qualityCloseTimer.stop()
                        }
                        onExited: {
                            qualityHoverZone.pointerIn = false
                            qualityCloseTimer.restart()
                        }
                    }
                }

                /*
                 * 清晰度菜单：同样改用 Popup 浮层（原因见上面倍速菜单那段说明）。
                 * 项 145x36、内边距 12 ——参考 controls.scss:765-775。
                 */
                T.Popup {
                    id: qualityMenu

                    /* 自己维护"鼠标在不在浮层里"（框架的 Popup 没有可用的 hovered 属性），关掉时清零 */
                    property bool pointerIn: false

                    onClosed: {
                        pointerIn = false
                        qualityHoverZone.pointerIn = false
                    }

                    /*
                     * 鼠标停在第几项上（-1 = 没有）：0..n-1 = qualities[i]（手动档在上），
                     * n = "自动"（在最后一项，见 contentItem 里的顺序说明）。
                     * 项自己不带 hover（会和浮层这层判据互相抢），高亮按鼠标位置算。
                     *
                     * 【本轮修的 bug：列表滚动之后高亮错行】
                     * 原来这里写的是 `Math.floor(qualityMenuHover.mouseY / qualityItemHeight)` ——
                     * `MouseArea.mouseY` 是**视口坐标**，而列表一旦滚动（`contentY > 0`），
                     * 行在视口里的位置就整体上移了 `contentY`，mouseY 却还是从视口顶算起
                     * → 算出来的行号**偏小**（差几行 = contentY / 行高；鼠标在第 N 项上、
                     * 亮的却是上面第 N-1 项）。菜单在上一轮只有内容刚好等于可视高时才成立
                     * （interactive 恒为 false），所以没暴露；本轮加了上限之后它**一定能滚**，
                     * 必须一起修。
                     *
                     * 现在改成用 `mapToItem` 把鼠标点映射进**行列自己的坐标系**（内容坐标系，
                     * 已经含滚动量）再除行高：滚动量、以后加内边距 / 再套一层容器都由映射负责，
                     * 不用手算 —— 和"选集"菜单的 `eplistMenu.rowAt()` 完全同一套写法，
                     * 依据是 docs/QML-POPUP-HOVER-PITFALLS.md 第 21 条。
                     *
                     * 【为什么依赖里要显式写 contentY / height】滚轮滚动时鼠标是不动的：
                     * 只依赖 mouseX/mouseY 的话，滚动后这个绑定不会重算，高亮会停在旧行上。
                     *
                     * 【pointerIn 的检查必须放在最前面】那两行是"读一下当依赖"，本身没有副作用；
                     * 但绑定第一次求值时（pointerIn 还是 false）background/contentItem 里的对象
                     * 可能还没建好，先读它们会抛 TypeError。鼠标没进面板时提前 return 就没这问题，
                     * 而且 pointerIn 本身是依赖 —— 它一变真，绑定会重算，那时才把 contentY 记成依赖。
                     */
                    readonly property int hoverIndex: {
                        if (!pointerIn)
                            return -1

                        qualityScroll.contentY        /* 依赖：滚轮滚动（鼠标不动）也要重算 */
                        qualityColumn.height          /* 依赖：行数 / 行高变化 */

                        return rowAt(qualityMenuHover.mouseX, qualityMenuHover.mouseY)
                    }

                    /*
                     * 浮层坐标 (x, y) → 命中第几行（-1 = 没命中任何一行）。
                     *
                     * 坐标系以 qualityMenuHover（浮层背景那层判据 MouseArea）为基准 ——
                     * 和 mouse.x / mouse.y、mouseX / mouseY 是同一套，调用方直接传即可。
                     * 映射到 qualityColumn 之后，y 就是**内容坐标**（已经含 contentY 的滚动量），
                     * 所以滚动多少行都不会算歪。
                     *
                     * 行号语义不变：0..n-1 = qualities[i]，n = "自动"（最后一项）。
                     */
                    function rowAt(x, y) {
                        var rows = (bar.player ? bar.player.qualities.length : 0) + 1

                        if (rows <= 1 || !qualityMenuHover || !qualityColumn
                                || qualityColumn.height <= 0)
                            return -1

                        var p = qualityMenuHover.mapToItem(qualityColumn, x, y)

                        if (p.y < 0 || p.y >= qualityColumn.height)
                            return -1

                        var i = Math.floor(p.y / QtPlayerTheme.qualityItemHeight)
                        return (i >= 0 && i < rows) ? i : -1
                    }

                    /*
                     * 选中某一行（点击语义和以前完全一样，只是抽成一个函数，
                     * 让"行自己的 MouseArea"和"判据层的兜底 onClicked"共用同一份逻辑）：
                     *   index <  n → selectQuality(qualities[index].streamIndex)（手动选档）
                     *   index == n → useAutoQuality()（回到自动 / ABR）
                     * 末尾都关菜单。越界（点在没有行的空白处）什么都不做。
                     */
                    function activateRow(index) {
                        if (!bar.player)
                            return

                        var rows = bar.player.qualities.length

                        if (index < 0 || index > rows)
                            return

                        if (index === rows)
                            bar.player.useAutoQuality()
                        else
                            bar.player.selectQuality(bar.player.qualities[index].streamIndex)

                        qualityMenu.close()
                    }

                    parent: qualityButton
                    width: QtPlayerTheme.qualityItemWidth

                    /*
                     * 菜单顶边与窗口上沿之间的安全边距（px）。**只在本菜单用，不进主题**
                     * （QtPlayerTheme.qml 不在本轮改动范围内）。
                     */
                    readonly property int menuTopGap: 8

                    /*
                     * 【按钮上方可用高度】菜单是**向上弹**的（几何见下面 y 的表达式），所以它能用的
                     * 垂直空间只有"菜单底边在窗口里的 y"再往上那一段；再减掉一点安全边距，
                     * 免得菜单顶边贴死窗口上沿（全屏时那就是画面最上沿）。
                     *
                     * 菜单底边在窗口里的 y = 按钮顶边的窗口 y + 按钮高 - bar.menuBottom
                     *   —— 和下面 y 的表达式是**同一个几何**（y 只决定顶边，底边与菜单高度无关），
                     *      改一处别忘了另一处。
                     *
                     * 【窗口缩放时必须重算】这件事的坑和做法写在下面 aboveAvailableHeight 里
                     * （mapToItem 不进依赖表 ⇒ 依赖要手写读一遍），这里不重复。
                     *
                     * 【至少一行】算出来的可用高度比一行（qualityItemHeight = 36）还小时也按一行算 ——
                     * 绝不能得到 0 或负数，那会是一个高度为 0、点不动的空菜单
                     * （只有"菜单底边离窗口上沿太近"时才可能发生；此时"能滚的一行"仍是正确的降级形态）。
                     */
                    readonly property real aboveAvailableHeight: {
                        var win = qualityButton.window

                        /* 还没进窗口（极端早期）：不收窄，交回给 qualityMenuMaxHeight 那道上限 */
                        if (win === null || win === undefined)
                            return QtPlayerTheme.qualityMenuMaxHeight

                        /*
                         * 【显式依赖：必须手写读一遍 —— 这就是本改动唯一的坑】
                         * `mapToItem()` 内部走的是 C++ 属性读取，**不会进 QML 绑定的依赖表**：
                         * 只写 mapToItem 的话，窗口一缩放这个绑定不会重算，菜单高度就停在旧值上
                         * （窗口变小时菜单又会被上沿截掉）。所以下面几个"会变的量"要自己读一次
                         * 当依赖 —— 写法同本文件 eplistMenu.hoverIndex 里那两行裸表达式。
                         */
                        win.height                /* 窗口缩放（主要就是靠它重算） */
                        bar.height                /* 控制栏高度 / 布局变化 */
                        qualityButton.height      /* 按钮高度（全屏 / 网页档两套 btnLineHeight） */
                        bar.menuBottom            /* 菜单底边距按钮底边：全屏 74 / 非全屏 41 */

                        /* 按钮顶边在**窗口**里的 y：mapToItem(null, …) 给的就是场景（= 窗口内容区）坐标 */
                        var buttonTopY = qualityButton.mapToItem(null, 0, 0).y
                        var menuBottomY = buttonTopY + qualityButton.height - bar.menuBottom
                        var avail = Math.min(menuBottomY - menuTopGap,
                                             win.height - menuTopGap)

                        /* 至少一行（qualityItemHeight = 36）：绝不出现高度 0、点不动的空菜单 */
                        return Math.max(QtPlayerTheme.qualityItemHeight, avail)
                    }

                    /*
                     * 【本轮加的上限 + 滚动】
                     * 菜单高度取三个上限的最小值：
                     *   1. 内容自然高度（档位少时就是它）；
                     *   2. 主题里的 `qualityMenuMaxHeight`（= 580，参考实现的 max-height）；
                     *   3. `aboveAvailableHeight` —— 按钮上方到窗口上沿的可用高度（至少一行）。
                     * 只要 1 > min(2, 3)，下面 contentItem 的 Flickable 就能滚（写法照 eplistMenu，
                     * 含右边缘那条 3px 滚动条）；上限都不起作用时和以前一模一样（interactive = false）。
                     *
                     * 用 `contentItem.implicitHeight`（Flickable 的**自然**高度）而不是去读它自己的
                     * height：后者会被 Popup 定成可视高，形成绑定环。
                     */
                    height: Math.min(contentItem.implicitHeight,
                                     QtPlayerTheme.qualityMenuMaxHeight,
                                     aboveAvailableHeight)
                    x: (parent.width - width) / 2
                    y: parent.height - bar.menuBottom - height

                    padding: 0
                    /* 【只靠"移出关闭"】Esc 是退全屏用的、也不要点击面板外关闭 */
                    closePolicy: T.Popup.NoAutoClose
                    enter: Transition {}
                    exit: Transition {}

                    /*
                     * 浮层那层"鼠标在不在里面"的判据，说明见音量浮层那段
                     * （必须 anchors.fill 撑满：浮层模板不会给 background 定尺寸）。
                     * 浮层里只有这一处收 hover，所以它一定收得到。
                     */
                    background: Rectangle {
                        anchors.fill: parent
                        color: QtPlayerTheme.menuBg
                        radius: QtPlayerTheme.menuRadius

                        MouseArea {
                            id: qualityMenuHover

                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onEntered: {
                                qualityMenu.pointerIn = true
                                qualityCloseTimer.stop()
                            }
                            onExited: {
                                qualityMenu.pointerIn = false
                                qualityCloseTimer.restart()
                            }

                            /*
                             * 【本轮加的兜底】按下没落到任何一行上时（比如落在滚动条那条
                             * 3px 宽的带子里），也按同一套坐标换算补一刀；落在空白处
                             * rowAt 返回 -1，什么也不做。
                             *
                             * 正常情况下这条不会触发 —— 按下落在行上时由行那层接走
                             * （一个按下事件只投递给一个 item），所以不会和行那条路重复选档。
                             * 留着的原因和"选集"面板完全一样（docs/QML-POPUP-HOVER-PITFALLS.md
                             * 第 22 条的第二条对策）：行那层哪天被别的层盖住时这里还能兜住，
                             * 而这一层是"鼠标在不在浮层里"（hover 高亮）实际在用的那层。
                             */
                            onClicked: function (mouse) {
                                qualityMenu.activateRow(qualityMenu.rowAt(mouse.x, mouse.y))
                            }
                        }
                    }

                    /*
                     * 内容区改成**可滚动**的 Flickable（和"选集"菜单同一套写法，见 eplistMenu）。
                     *
                     * 【为什么本轮必须加】档位从 4 档变成 8~9 档之后（同分辨率的不同编码分开列），
                     * 菜单高度 = 档位数 x 36，小窗口/非全屏下会顶出窗口上沿、最高档点不到 ——
                     * 这是本轮 UI 改动的直接后果，所以在这里收口（不改任何其它交互）。
                     *
                     * 【可视高从哪来】上面的 Popup.height = min(内容自然高, qualityMenuMaxHeight)，
                     * Popup 会把 contentItem（本 Flickable）定成那个高度，于是
                     * `interactive: contentHeight > height` 只在上限真正生效时才为真。
                     *
                     * 【滚轮不需要额外的 MouseArea】行里的 MouseArea 不收滚轮，事件会冒到
                     * 这里的 Flickable（eplistMenu 那段注释里已核实过同一件事）。
                     * 只有"内容超过可视区"时才 interactive，档位少的片源和以前一模一样。
                     */
                    contentItem: Flickable {
                        id: qualityScroll

                        /*
                         * 内容自然高度（= 所有档位行 + "自动"那一行的总高）。
                         * 上面的 Popup.height 是"它和 qualityMenuMaxHeight 取小"：
                         * 这里给的是**内容**高，Popup 只决定**可视**高 —— 两者不等时
                         * 下面 interactive 为真，可以滚。
                         * （不能把上限写在这一行：implicitHeight 一旦被压小，
                         *  Popup.height 跟着变小，就再也滚不动了。）
                         */
                        implicitHeight: qualityColumn.implicitHeight
                        contentHeight: qualityColumn.implicitHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        interactive: contentHeight > height

                        /*
                         * 滚动条：只有内容超出可视区时才出现，样式跟选集/字幕/弹幕那几条一致
                         * （3px 宽、#e5e7ef、贴视口右边缘、圆角 1.5；鼠标在面板上时亮一点）。
                         * x/y 必须加上 contentX/contentY 把滚动量抵消掉，才固定在视口上 ——
                         * 原因见 eplistScrollBar 的注释（Flickable 的子项都在内容坐标系里）。
                         */
                        Rectangle {
                            id: qualityScrollBar

                            visible: qualityScroll.contentHeight > qualityScroll.height
                            width: 3
                            radius: 1.5
                            color: "#e5e7ef"
                            opacity: qualityMenu.pointerIn ? 0.45 : 0.25
                            z: 50
                            x: qualityScroll.contentX + qualityScroll.width - width
                            y: qualityScroll.contentY
                               + qualityScroll.visibleArea.yPosition * qualityScroll.height
                            height: Math.max(16, qualityScroll.visibleArea.heightRatio * qualityScroll.height)

                            Behavior on opacity {
                                NumberAnimation { duration: 200 }
                            }
                        }

                        Column {
                            id: qualityColumn

                            width: qualityScroll.width

                            /*
                             * 【顺序：各档清晰度在前，**「自动」放最下面**（用户要求）】
                             *
                             * 参考默认也是"手动档在上、自动在最后一项"（用户给的 DOM 里
                             * `.bpx-player-ctrl-quality-menu-item` 列表末尾才是自动）。
                             * hoverIndex 的语义跟着变：0..n-1 = qualities[i]，n = 自动。
                             */
                            Repeater {
                                model: bar.player ? bar.player.qualities : []

                            Rectangle {
                                id: qualityRow

                                required property var modelData
                                required property int index

                                /*
                                 * 这一档的**编码徽标文本**：C++ 侧已经在
                                 * CicadaPlayerItem.cpp::codecDisplayName() 里换成了**显示名**
                                 * （qualities[i].codecLabel，例如 "H.264" / "HEVC" / "AV1"；
                                 *  内核短名 H.265 → HEVC）。界面这边**不再做任何映射**：
                                 * 显示名只有那一处来源，免得同一个 UI 里两套写法并存。
                                 *
                                 * 【空串就不渲染徽标】内核认不出编码时给的是空串（不猜、不写"未知"），
                                 * 这时下面那颗 codecBadge 整个不显示，档位行还是老样子。
                                 *
                                 * 【同分辨率不同编码是两行】1080P 的 H.264 与 H.265 在
                                 * CicadaPlayerItem.cpp 的 onMediaInfoGetCb 里已按"分辨率 + 编码"
                                 * 分成两条独立条目，所以这里会出现两行 1080P、各带自己的徽标
                                 * （`1080P [H.264]` / `1080P [HEVC]`）—— 这是预期效果，不是重复。
                                 * （分组/认行用的仍是内核短名，见那边 onMediaInfoGetCb 的说明。）
                                 */
                                readonly property string codecLabel: {
                                    if (modelData === undefined || modelData === null)
                                        return ""

                                    var c = modelData.codecLabel

                                    return (c === undefined || c === null) ? "" : ("" + c).trim()
                                }

                                /*
                                 * 【这一行是不是"正在切过去"的目标】
                                 *
                                 * 判据完全按 **streamIndex**（不是数组下标）：
                                 *     player.qualitySwitching && player.qualitySwitchingStreamIndex === 行的 streamIndex
                                 * 两者都来自同一个 NOTIFY 信号（qualitySwitchStatusChanged），
                                 * 所以 STARTED 那一刻两者同时就绪、READY/FAILED/CANCELED 同时撤掉，
                                 * 不存在"亮着但不知道该标哪行"的中间态。
                                 *
                                 * ⚠ 菜单在点击后**立刻关闭**（行的 onClicked → qualityMenu.close()），
                                 * 而切档实测只 30~90ms ⇒ 这颗角标通常来不及被看到；它是"菜单恰好开着
                                 * 时也能标对行"的完整实现（例如菜单开着时 ABR 自己切档）。
                                 * 真正给人看的是控制栏那颗按钮上的同名角标（见 switchingBadge）。
                                 */
                                readonly property bool switching: {
                                    if (modelData === undefined || modelData === null)
                                        return false

                                    if (!bar.player || !bar.player.qualitySwitching)
                                        return false

                                    return bar.player.qualitySwitchingStreamIndex === modelData.streamIndex
                                }

                                width: QtPlayerTheme.qualityItemWidth
                                height: QtPlayerTheme.qualityItemHeight
                                color: qualityMenu.hoverIndex === index ? QtPlayerTheme.menuHoverBg : "transparent"

                                Text {
                                    id: qualityRowLabel

                                    anchors {
                                        verticalCenter: parent.verticalCenter
                                        left: parent.left
                                        leftMargin: QtPlayerTheme.qualityItemPaddingH
                                    }
                                    color: (bar.player && !bar.player.autoQuality
                                            && bar.player.qualityIndex === index)
                                           ? QtPlayerTheme.qualityActiveText : QtPlayerTheme.menuText
                                    font.pixelSize: QtPlayerTheme.ctrlTimeFontSize
                                    text: modelData.label
                                }

                                /*
                                 * 编码徽标：紧跟在档位名右边的那个小圆角标签（`1080P [HEVC]`；
                                 * 文本就是上面的 codecLabel，显示名已在 C++ 侧统一换好）。
                                 * 样式沿用"自动"角标那一套（半透明白底 + 1px 描边 + 次要白字，
                                 * 见上面 autoBadge 的说明），只是字号更小、位置贴在档位名右侧。
                                 * 只做展示，不接收鼠标 —— 整行的点击仍然由下面的 MouseArea 负责。
                                 */
                                Rectangle {
                                    id: codecBadge

                                    /*
                                     * 【切换中的那一行让位】"切换中"角标锚在行的**右边缘**，
                                     * 而这颗编码徽标跟在档位名右边；档位名偏长 + 编码偏长
                                     * （例如 `2160P [MPEG-4]`）时两者会叠在一起。切档只有
                                     * 30~90ms，所以切换期间让编码徽标先不画，切完立刻回来。
                                     */
                                    visible: qualityRow.codecLabel !== "" && !qualityRow.switching
                                    anchors {
                                        left: qualityRowLabel.right
                                        leftMargin: 6
                                        verticalCenter: parent.verticalCenter
                                    }
                                    width: codecBadgeText.implicitWidth + 8   /* padding: 0 4px */
                                    height: 14
                                    radius: 3
                                    color: Qt.rgba(1, 1, 1, 0.12)
                                    border.width: 1
                                    border.color: Qt.rgba(1, 1, 1, 0.3)

                                    Text {
                                        id: codecBadgeText

                                        anchors.centerIn: parent
                                        color: Qt.rgba(1, 1, 1, 0.55)
                                        /* 次要信息：比档位名（ctrlTimeFontSize）小一档 */
                                        font.pixelSize: 9
                                        font.weight: Font.Normal
                                        /* line-height: 1 —— 单行不让默认行距把字推下去（同 autoBadge） */
                                        lineHeight: 1.0
                                        text: qualityRow.codecLabel
                                    }
                                }

                                /*
                                 * 「切换中」角标（只画在**正在切过去的那一行**上）。
                                 *
                                 * 样式沿用同一套小圆角标签（半透明白底 + 1px 描边 + 9px 字，同 codecBadge），
                                 * 但**描边与文字用主题强调色**（QtPlayerTheme.qualityActiveText）——
                                 * 不新增主题常量，也和"当前档选中"的高亮是同一种色，一眼能认出来。
                                 *
                                 * 位置：锚在行的**右边缘**（内边距同 qualityItemPaddingH），与左侧的
                                 * 档位名 / 编码徽标互不干扰；编码徽标在切换期间让位（见上）。
                                 * 只做展示，不接收鼠标 —— 点击仍旧由下面整行的 MouseArea 负责。
                                 */
                                Rectangle {
                                    id: switchingBadge

                                    visible: qualityRow.switching
                                    anchors {
                                        right: parent.right
                                        rightMargin: QtPlayerTheme.qualityItemPaddingH
                                        verticalCenter: parent.verticalCenter
                                    }
                                    width: switchingBadgeText.implicitWidth + 8
                                    height: 14
                                    radius: 3
                                    color: Qt.rgba(1, 1, 1, 0.12)
                                    border.width: 1
                                    border.color: QtPlayerTheme.qualityActiveText

                                    Text {
                                        id: switchingBadgeText

                                        anchors.centerIn: parent
                                        color: QtPlayerTheme.qualityActiveText
                                        font.pixelSize: 9
                                        font.weight: Font.Normal
                                        lineHeight: 1.0
                                        text: qsTr("切换中")
                                    }
                                }

                                MouseArea {
                                    id: qualityItemArea

                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor

                                    /*
                                     * 【本轮修"滚动之后点一下没反应"的关键】
                                     * 能滚的 Flickable（interactive: true）会把"下一拍"的按下事件
                                     * 从行里的 MouseArea 手上过滤掉 —— 现象是"滚轮滚动之后那一拍
                                     * 点击会丢"（不滚动直接点、或按下后抖 2px 再点都正常）。
                                     * 依据与实测见 docs/QML-POPUP-HOVER-PITFALLS.md 第 22 条，
                                     * 和"选集"面板行里的那一行是同一件事（eplistRow）。
                                     * `preventStealing` 内部就是 setKeepMouseGrab(true)，Flickable
                                     * 看到子项 keepMouseGrab 就不再过滤。代价：在行上按住拖动不再能
                                     * 滚列表（行占满整个视口，相当于关掉拖动滚动；**滚轮照旧**）。
                                     *
                                     * 本轮的滚动是新增能力，所以这一行属于"点击语义保持不变"的必要
                                     * 配套：不加它，档位一多、一滚动，用户点档就会丢。
                                     */
                                    preventStealing: true

                                    /* 点击语义**一字未改**（这一行和下面那个 onClicked 都是原文） */
                                    onClicked: {
                                        if (bar.player)
                                            bar.player.selectQuality(modelData.streamIndex)

                                        qualityMenu.close()
                                    }
                                }
                            }
                        }

                        /*
                         * 自动档（最后一项）：走 ABR。
                         *
                         * 【用户要求】"未选择自动时面板上就显示自动；一旦选中自动，
                         * 这一项就显示成 **自动（实际清晰度）** —— 带括号，而且**整段
                         * 用和"自动"一样的激活色**"。所以这里只有**一个** Text：
                         * 文案在选中时拼成 `自动（<当前在播的那一档>）`，颜色仍然是
                         * qualityActiveText（和手动档选中时同一个高亮色）。
                         */
                        Rectangle {
                            id: autoQualityRow

                            readonly property int autoIndex: bar.player ? bar.player.qualities.length : 0
                            readonly property bool autoActive: bar.player ? bar.player.autoQuality : false

                            /*
                             * 档位还不知道时（qualityIndex < 0，例如刚起播）只显示"自动"，
                             * 免得拼出"自动（自动）"这种看着像 bug 的文案。
                             */
                            readonly property string label: {
                                if (!autoActive || !bar.player || bar.player.qualityIndex < 0)
                                    return qsTr("自动")

                                return qsTr("自动（%1）").arg(qualityButton.currentLabel)
                            }

                            width: QtPlayerTheme.qualityItemWidth
                            height: QtPlayerTheme.qualityItemHeight
                            color: qualityMenu.hoverIndex === autoQualityRow.autoIndex
                                   ? QtPlayerTheme.menuHoverBg : "transparent"

                            Text {
                                id: autoQualityLabel

                                anchors {
                                    verticalCenter: parent.verticalCenter
                                    left: parent.left
                                    leftMargin: QtPlayerTheme.qualityItemPaddingH
                                    right: parent.right
                                    rightMargin: QtPlayerTheme.qualityItemPaddingH
                                }
                                /* 选中自动时整段都用激活色（和"自动"两个字同色，用户要求） */
                                color: autoQualityRow.autoActive ? QtPlayerTheme.qualityActiveText
                                                                 : QtPlayerTheme.menuText
                                font.pixelSize: QtPlayerTheme.ctrlTimeFontSize
                                elide: Text.ElideRight
                                text: autoQualityRow.label
                            }

                            MouseArea {
                                id: autoQualityArea

                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor

                                /*
                                 * 和上面手动档那一行同理：菜单能滚之后，不加这一行
                                 * "滚过一下再点自动"会丢点击（docs/QML-POPUP-HOVER-PITFALLS.md
                                 * 第 22 条）。点击语义本身一字未改。
                                 */
                                preventStealing: true

                                onClicked: {
                                    if (bar.player)
                                        bar.player.useAutoQuality()

                                    qualityMenu.close()
                                }
                            }
                        }
                    }
                }
            }
        }
}
