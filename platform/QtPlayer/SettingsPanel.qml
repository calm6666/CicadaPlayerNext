// ===========================================================================
// 设置面板（控制栏"设置"齿轮的浮层内容）
//
// DOM 出处：CicadaPlayerNext/platform/ctrl-setting.txt
//   左页 .bpx-player-ctrl-setting-menu-left  ：三行开关 + "更多播放设置"
//   右页 .bpx-player-ctrl-setting-menu-right ：播放方式 / 视频比例 / 播放策略 / 音量均衡
// 数值出处：platform/style.css 同名 class 的实测值（写在各自注释里）
//
// 【全部手写，不用 QtQuick.Controls】参考里开关/选项都是纯 div，样式非常特化
// （30x20 轨道、16px 圆点、10px 选项文字、hover 底 #757575…），套原生控件反而要把
// indicator/contentItem/background 全部推倒重写，还容易残留原生尺寸与内边距。
//
// 开合、hover 容错、Esc/点外策略都在 RightControls.qml 那一层，本文件只是内容。
// 切页时浮层**右边缘和底边固定**：宽度 132→286 往左长、高度 140→228 往上长。
// ===========================================================================
import QtQuick

Item {
    id: panel

    property var bar: null
    property var player: bar ? bar.player : null

    /* 0 = 左页，1 = 右页 */
    property int pageIndex: 0
    readonly property bool onRightPage: pageIndex === 1

    /* 鼠标在面板里的位置（由浮层 background 那层 MouseArea 喂进来，见 RightControls 的 settingMenu）。
       【照 SubtitlePanel.qml:67-85 的写法】面板里**不允许有任何接受 hover 的元素**：
       HoverHandler / 写了 hoverEnabled 的 MouseArea 都会把浮层那层判据的 hover 抢走 ——
       鼠标一压到控件上，浮层就收到 exit，"移出关闭"的定时器立刻把面板关掉（反过来，
       若给关闭条件再加一项兜底，就变成"移出面板也不关"）。所以本面板所有 hover 一律
       走 pointerOn(item) 被动判，位置由浮层喂。 */
    property real pointerX: -1
    property real pointerY: -1

    /* 鼠标在不在某个控件里：面板坐标 → 控件坐标（mapToItem，嵌套/滚动过的都能算对） */
    function pointerOn(item) {
        if (pointerX < 0 || pointerY < 0)
            return false

        var p = panel.mapToItem(item, pointerX, pointerY)

        return p.x >= 0 && p.x <= item.width && p.y >= 0 && p.y <= item.height
    }

    /* 左页 132x140、右页 286 宽（数值全在 QtPlayerTheme 里，出处见那边的注释） */
    readonly property int leftWidth: QtPlayerTheme.settingsPanelLeftWidth
    readonly property int rightWidth: QtPlayerTheme.settingsPanelRightWidth
    readonly property int leftHeight: QtPlayerTheme.settingsPanelLeftHeight

    /*
     * 【右页高度按内容算，不是写死的数】原来是一个固定值（236）—— 加进「高能进度条」那一行
     * 之后，最后两行（高能进度条、底部那条返回热区）会被面板底部的 clip 裁掉：
     * 看得见半截、也点不到。
     *
     * 现在取两个判据里较大的那个：
     *   * rightContentHeight —— 用主题 token 明算出来的内容高度
     *     （上内边距 + **四组单选** + 「其他设置」这一组 + 底部返回条 + **五个**组间距）；
     *   * rightColumn.implicitHeight —— 那一列按**实际子项**算出来的高度。
     * 前者保证内容一定装得下；后者保证以后再往右页加行时面板会自动长高
     * （谁大听谁的 —— 只会偏大，不会把内容切掉）。左页仍然是固定的 140，布局一个像素没动。
     *
     * 右页这一列现在是 **6 个子项**：4 组单选 + 「其他设置」那一组 + 返回热区
     * → Column 的 spacing 加在相邻两项之间，共 **5 个组间距**；
     * 而「其他设置」那一组**内部**有 2 个子项（组标题 / 高能进度条开关）
     * → 内部 2 个间距（settingsPanelGroupTitleGap = 6）。
     * 合计 = 12 + 4×44 + 16 + 2×6 + 20 + 22 + 16 + 5×12 = **334**。
     * 每一项都是 token，改行高/间距就跟着变，这里没有任何写死的数。
     */
    readonly property int rightContentHeight: QtPlayerTheme.settingsPanelPadV
                                             + 4 * QtPlayerTheme.settingsPanelRadioGroupHeight
                                             + QtPlayerTheme.settingsPanelGroupTitleHeight
                                             + 2 * QtPlayerTheme.settingsPanelGroupTitleGap
                                             + QtPlayerTheme.settingsPanelCheckRowHeight
                                             + QtPlayerTheme.settingsPanelBackRowHeight
                                             + 5 * QtPlayerTheme.settingsPanelGroupSpacing
    readonly property int rightHeight: Math.max(QtPlayerTheme.settingsPanelRightMinHeight,
                                                Math.max(rightContentHeight,
                                                         rightColumn.implicitHeight))

    implicitWidth: onRightPage ? rightWidth : leftWidth
    implicitHeight: onRightPage ? rightHeight : leftHeight

    /* =====================================================================
     * 手写开关行：文字在左、30x20 轨道在右（.bui-switch）
     * ===================================================================== */
    /*
     * 「高能进度条」开关（pbp 曲线）的状态与出口。
     *
     * 【为什么要单独声明、而不是直接绑某个对象】这个值真正的主人是
     * PlayerView.highlightEnabled → PlayerControlBar.highlightEnabled → ProgressRow.highlightEnabled
     * （默认 true）。本面板是 RightControls 里用 contentItem 创建的，拿不到 PlayerView，
     * 所以由调用方把控制栏上的那个值传进来、把开关的改动回写上去 —— 单向数据流，
     * 面板自己不存状态，避免"面板里开着、曲线却没出来"这种两处状态不同步的坑。
     */
    property bool highlightEnabled: true
    signal highlightToggled(bool value)

    /*
     * （原「手填 mock 基址」那条接线已按用户要求删除：用户没提过要这个输入框。
     *   mock 基址只由 Main.qml 自动探测 9101 → 9000，或用 CICADA_MOCK_BASE 环境变量覆盖。）
     */

    component SettingSwitchRow: Item {
        id: swRow

        property string title
        property bool checked
        signal toggled()

        /* hover 判据：照 SubtitlePanel 的 panel.pointerOn()（本面板不写任何接受 hover 的元素） */
        readonly property bool hovered: panel.pointerOn(swRow)

        implicitHeight: QtPlayerTheme.settingsPanelSwitchRowHeight

        Text {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            text: swRow.title
            color: swRow.hovered ? QtPlayerTheme.playerAccent : "#ffffff"
            font.pixelSize: 12
        }

        /* .bui-switch-body：30x20、圆角 10、底 #757575；选中 #00a1d6 */
        Rectangle {
            id: swTrack

            anchors.verticalCenter: parent.verticalCenter
            anchors.right: parent.right
            width: 30
            height: 20
            radius: 10
            color: swRow.checked ? QtPlayerTheme.playerAccent : "#757575"

            Behavior on color {
                ColorAnimation {
                    duration: 300
                }
            }

            /* .bui-switch-dot：16x16 白圆、left 2px → 选中 left:100% margin-left:-18px */
            Rectangle {
                width: 16
                height: 16
                radius: 8
                color: "#ffffff"
                y: 2
                x: swRow.checked ? swTrack.width - 18 : 2

                Behavior on x {
                    NumberAnimation {
                        duration: 300
                        easing.type: Easing.InOutQuad
                    }
                }
            }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: swRow.toggled()
        }
    }

    /* =====================================================================
     * 手写单选组：标题 16 + 缝 4 + 选项行 22，列距 8（.bui-radio）
     * 选项是纯文字：未选 #99a2aa、hover #00a1d6 + 底 #757575、选中 #00a1d6
     * ===================================================================== */
    component SettingRadioGroup: Item {
        id: group

        property string title
        property var options: []                 /* [{ label, value }] */
        property var currentValue: 0
        signal picked(var value)

        implicitHeight: QtPlayerTheme.settingsPanelRadioGroupHeight

        Text {
            id: groupTitle

            anchors.left: parent.left
            anchors.right: parent.right
            /* 分组标题的行高走主题 token：右页的「其他设置」标题用的也是同一个值，
               两处"同一套写法"才是真的同一套（不是各写一个 16） */
            height: QtPlayerTheme.settingsPanelGroupTitleHeight
            text: group.title
            color: "#ffffff"
            font.pixelSize: 12
            verticalAlignment: Text.AlignVCenter
        }

        Row {
            id: groupRow

            anchors.top: groupTitle.bottom
            anchors.topMargin: 4
            anchors.left: parent.left
            spacing: 8                            /* .bui-radio-group{column-gap:8px} */
            height: 24                            /* 用户要求：选项行 24px（CSS 是 22） */

            Repeater {
                model: group.options

                Item {
                    id: optionItem

                    required property var modelData

                    /* hover 判据：照 SubtitlePanel 的 panel.pointerOn() */
                    readonly property bool hovered: panel.pointerOn(optionItem)

                    /* 等宽均分（CSS 是 grid 的 repeat(N, minmax(max-content,1fr))） */
                    width: (group.width - (group.options.length - 1) * 8) / group.options.length
                    height: 24

                    /*
                     * 选项底色（用户给的规格）：
                     *   未选：background-color: hsla(0,0%,100%,.3)、border-radius:2px、padding:2px 0
                     *   hover：hsla(0,0%,100%,.4)（CSS 里 .bui-radio-item:hover .bui-radio-label）
                     *   选中：换成播放器主题蓝 #00a1d6
                     * 文字**始终白色**。
                     */
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        y: 2                                  /* padding: 2px 0 */
                        height: parent.height - 4
                        radius: 2
                        color: (group.currentValue === optionItem.modelData.value)
                               ? QtPlayerTheme.playerAccent
                               : (optionItem.hovered ? Qt.rgba(1, 1, 1, 0.4) : Qt.rgba(1, 1, 1, 0.3))

                        Behavior on color {
                            ColorAnimation {
                                duration: 150
                            }
                        }

                        Text {
                            anchors.fill: parent
                            /* .bui-radio-text：10px、行高 16、左右 padding 8px */
                            leftPadding: 8
                            rightPadding: 8
                            text: optionItem.modelData.label
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            color: "#ffffff"
                            /* 用户要求：字号和第一页（开关那三行）一致 = 12px
                               （CSS 里写的是 10px，这里按用户要求覆盖） */
                            font.pixelSize: 12
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            group.currentValue = optionItem.modelData.value
                            group.picked(optionItem.modelData.value)
                        }
                    }
                }
            }
        }
    }

    /* =====================================================================
     * 复选行（图标用参考那两个 32x32 的 SVG，不手画方框）
     *
     * 【整段抄自】DanmakuSettingPanel.qml:772-827 的 `component SettingCheck`
     * ——就是弹幕设置面板里「弹幕随屏幕缩放」那一行（那边 :247-257 的用法）用的控件。
     * 尺寸/颜色/透明度/点击写法**逐行照抄**，只把写死的数值换成主题 token
     * （数值一个字没改：16 / 6 / 20 / 12 / 0.75 / #ffffff / rgba(255,255,255,.8)），
     * 以及 hover 的来源见下面那条注释。
     * 图标仍是同一对素材：assets/images/dm/checkbox.svg / checkbox-checked.svg
     * （参考里那两个 32×32 的 bui-checkbox 图标：未选空心方框、选中方框 + 对勾）。
     * ===================================================================== */
    component SettingCheck: Item {
        id: check

        property string text: ""
        property bool checked: false

        signal toggled()

        width: parent.width
        height: QtPlayerTheme.settingsPanelCheckRowHeight
        /* 放进 Row 时用 implicitWidth 横排（见 left-radio 那三行） */
        implicitWidth: QtPlayerTheme.settingsPanelCheckIconSize
                       + QtPlayerTheme.settingsPanelCheckGap + labelText.implicitWidth
        implicitHeight: QtPlayerTheme.settingsPanelCheckRowHeight

        /*
         * hover 判据：**照参考的 panel.pointerOn(check)**（SubtitlePanel.qml:74-85 同一个函数，
         * 位置由浮层那层 MouseArea 喂进来）—— 本面板里不允许出现任何接受 hover 的元素，
         * 否则浮层那层判据会被抢走、鼠标一压到这一行上就被判成"已离开面板"。
         * 外观/尺寸/颜色/点击与弹幕面板里那一行完全一致，只有"怎么知道鼠标压在上面"这一段不同。
         */
        readonly property bool hovered: panel.pointerOn(check)

        Image {
            id: box

            anchors.verticalCenter: parent.verticalCenter
            width: QtPlayerTheme.settingsPanelCheckIconSize
            height: QtPlayerTheme.settingsPanelCheckIconSize
            source: check.checked ? "assets/images/dm/checkbox-checked.svg"
                                  : "assets/images/dm/checkbox.svg"
            sourceSize: Qt.size(QtPlayerTheme.settingsPanelCheckIconSize,
                                QtPlayerTheme.settingsPanelCheckIconSize)
            smooth: true
            opacity: check.checked ? 1 : QtPlayerTheme.settingsPanelCheckIdleOpacity
        }

        Text {
            id: labelText

            anchors {
                left: box.right
                leftMargin: QtPlayerTheme.settingsPanelCheckGap
                verticalCenter: parent.verticalCenter
            }
            text: check.text
            font.pixelSize: QtPlayerTheme.settingsPanelCheckFontSize
            /* 选中纯白；只是 hover 也变亮一点（参考的 hover 变白） */
            color: (check.checked || check.hovered) ? QtPlayerTheme.settingsPanelCheckTextActive
                                                    : QtPlayerTheme.settingsPanelCheckText
        }

        MouseArea {
            anchors.fill: parent
            /* 不写 hoverEnabled: true（弹幕面板那边的原注释：这条坑了四次 —— 接受 hover 之后
               浮层那两层判据会收到 exit，鼠标一移上来就被判成"离开面板"、面板自动关掉）。
               本行的 hover 变色交给上面那个 pointerOn()。 */
            cursorShape: Qt.PointingHandCursor
            onClicked: check.toggled()
        }
    }

    /* =====================================================================
     * 面板底：.bui-panel.bui-dark（hsla(0,0%,8%,.9)），.…-menu 把圆角覆盖成 2px
     * ===================================================================== */
    Rectangle {
        anchors.fill: parent
        radius: 2
        color: Qt.rgba(0.08, 0.08, 0.08, 0.9)
        clip: true

        Item {
            id: viewport

            anchors.fill: parent
            clip: true

            Row {
                id: moveRow

                height: parent.height
                /* 左页 0、右页 -132（.bui-panel-move 的 translateX，transition .25s） */
                x: panel.onRightPage ? -panel.leftWidth : 0

                Behavior on x {
                    NumberAnimation {
                        duration: 250
                        easing.type: Easing.InOutQuad
                    }
                }

                /* ------------- 左页：.…-menu-left（min-width 132、padding 6px 20px）------------- */
                Column {
                    width: panel.leftWidth
                    height: viewport.height
                    leftPadding: 20
                    rightPadding: 20
                    topPadding: 6
                    spacing: 0
                    opacity: panel.onRightPage ? 0 : 1
                    enabled: !panel.onRightPage

                    Behavior on opacity {
                        NumberAnimation {
                            duration: 100
                        }
                    }

                    SettingSwitchRow {
                        width: parent.width - 40
                        title: qsTr("镜像画面")
                        checked: panel.player ? panel.player.mirror : false
                        onToggled: if (panel.player) panel.player.mirror = !panel.player.mirror
                    }

                    SettingSwitchRow {
                        width: parent.width - 40
                        title: qsTr("单集循环")
                        checked: panel.player ? panel.player.loop : false
                        onToggled: if (panel.player) panel.player.loop = !panel.player.loop
                    }

                    SettingSwitchRow {
                        width: parent.width - 40
                        title: qsTr("自动开播")
                        checked: panel.player ? panel.player.autoPlay : false
                        onToggled: if (panel.player) panel.player.autoPlay = !panel.player.autoPlay
                    }

                    /* .…-more：32px 行高、右侧 16x16 箭头、hover 一起变蓝。
                       再点一次回左页（参考里没有"返回"行，不自己加）。 */
                    Item {
                        id: moreRow

                        width: parent.width - 40
                        height: 32

                        /* hover 判据：照 SubtitlePanel 的 panel.pointerOn() */
                        readonly property bool hovered: panel.pointerOn(moreRow)

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            text: qsTr("更多播放设置")
                            color: moreRow.hovered ? QtPlayerTheme.playerAccent : "#ffffff"
                            font.pixelSize: 12
                        }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.right: parent.right
                            width: 16
                            text: "›"
                            horizontalAlignment: Text.AlignHCenter
                            color: moreRow.hovered ? QtPlayerTheme.playerAccent : "#ffffff"
                            font.pixelSize: 16
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: panel.pageIndex = 1
                        }
                    }
                }

                /* ------------- 右页：.…-menu-right（min-width 286、padding 12px 20px）------------- */
                Column {
                    id: rightColumn

                    width: panel.rightWidth
                    /*
                     * 【这里绝对不能写 height】高度跟着 implicitHeight 走（= 内容高度），
                     * 面板高度正是按它算出来的（见上面 rightHeight）。
                     * 原来写的是 height: viewport.height —— 那样会形成绑定环
                     * （面板高 ← 列高 ← viewport 高 ← 面板高），QML 会报 Binding loop 且布局乱掉。
                     */
                    leftPadding: QtPlayerTheme.settingsPanelPadH
                    rightPadding: QtPlayerTheme.settingsPanelPadH
                    topPadding: QtPlayerTheme.settingsPanelPadV
                    spacing: QtPlayerTheme.settingsPanelGroupSpacing  /* 组间距：.…-content{margin-bottom:12px} */
                    opacity: panel.onRightPage ? 1 : 0
                    enabled: panel.onRightPage

                    Behavior on opacity {
                        NumberAnimation {
                            duration: 100
                        }
                    }

                    /* 四组。注：后端接口框架里还没有，目前只有面板自身状态（待接） */
                    SettingRadioGroup {
                        width: parent.width - 40
                        title: qsTr("播放方式")
                        options: [{ "label": qsTr("自动切集"), "value": 0 },
                                  { "label": qsTr("播完暂停"), "value": 2 }]
                        currentValue: 2
                    }

                    SettingRadioGroup {
                        width: parent.width - 40
                        title: qsTr("视频比例")
                        options: [{ "label": qsTr("自动"), "value": "0:0" },
                                  { "label": "4:3", "value": "4:3" },
                                  { "label": "16:9", "value": "16:9" }]
                        currentValue: "0:0"
                    }

                    SettingRadioGroup {
                        width: parent.width - 40
                        title: qsTr("播放策略")
                        options: [{ "label": qsTr("默认"), "value": 0 },
                                  { "label": "AV1", "value": 3 },
                                  { "label": "HEVC", "value": 1 },
                                  { "label": "AVC", "value": 2 }]
                        currentValue: 0
                    }

                    SettingRadioGroup {
                        width: parent.width - 40
                        title: qsTr("音量均衡")
                        options: [{ "label": qsTr("标准"), "value": 1 },
                                  { "label": qsTr("高动态"), "value": 2 },
                                  { "label": qsTr("关闭"), "value": 0 }]
                        currentValue: 0
                    }

                    /*
                     * 「其他设置」这一组 = **标题 + 标题下面那一项**，打包成外层 Column 的**一个**子项。
                     *
                     * 参考 platform\ctrl-setting.txt:175-177：
                     *     <div class="bpx-player-ctrl-setting-others">
                     *         <div class="bpx-player-ctrl-setting-others-title">其他设置</div>
                     *         <div class="bpx-player-ctrl-setting-others-content">
                     *             …隐藏黑边 / 高能进度条 / 原生画中画（都是 bui-checkbox）…
                     * 标题与内容在参考里是**同一个分组（同一个 div）**里的两行 —— 所以这里也包成一项：
                     *   * 它和上面「音量均衡」之间仍是外层 Column 的 12px **组间距**（分组之间才该松）；
                     *   * 标题与下面那一项之间用组内间距 settingsPanelGroupTitleGap = **6px**
                     *     （参考没给 `.others-title` 的 margin 值 —— 只有一个 class 名，
                     *      6px 取的是弹幕设置面板"标题→内容"那一档接近的口径）。
                     *
                     * 标题样式直接复用本页四个单选组的标题写法（SettingRadioGroup 里那个 title：
                     * 高 = settingsPanelGroupTitleHeight(16)、12px 白字、垂直居中），不另定字号。
                     */
                    Column {
                        id: othersGroup

                        width: parent.width - 40
                        spacing: QtPlayerTheme.settingsPanelGroupTitleGap    /* 组内：标题 → 内容 6px */

                        Text {
                            width: parent.width
                            height: QtPlayerTheme.settingsPanelGroupTitleHeight
                            text: qsTr("其他设置")
                            color: "#ffffff"
                            font.pixelSize: 12
                            verticalAlignment: Text.AlignVCenter
                        }

                        /*
                         * 高能进度条（pbp 曲线）—— 它在参考里属于上面那个「其他设置」分组
                         * （`.bpx-player-ctrl-setting-others-content` 里的 `bpx-player-ctrl-setting-highenergy`），
                         * 所以放在这个标题下面；位置仍在右页最后（返回热区之前）。
                         *
                         * 依据前端参考：这一项的 DOM 位置是
                         *     .player-ctrl-setting-others .player-ctrl-setting-others-content
                         * （front\player\src\component\controls\index.ts:1337；左页只有
                         *  镜像画面 / 单集循环 / 自动开播 / "更多播放设置"，见 platform\ctrl-setting.txt:5-41），
                         * 也就是翻到右页之后才看得到它。
                         *
                         * 【控件形态】用户指定：**和弹幕设置面板里「弹幕随屏幕缩放」那一行用同一个控件**。
                         * 所以这里用的 SettingCheck 就是 DanmakuSettingPanel.qml:772-827 那个组件
                         * （那边 :247-257 的用法），尺寸/颜色/透明度/点击写法逐行照抄：
                         *   图标 16×16（assets/images/dm/checkbox.svg / checkbox-checked.svg）、
                         *   图标与文字间距 6、整行高 20、文字 12px、未选图标 opacity .75、
                         *   未选文字 rgba(255,255,255,.8)、选中或悬停纯白；点整行切换。
                         * hover 的来源和弹幕面板那边**完全一样**：都是 panel.pointerOn()
                         * （位置由浮层那层 MouseArea 喂进来），外观一模一样。
                         *
                         * 【语义不变】它就是"高能进度条"的开关：checked = highlightEnabled（默认 true）。
                         * 值一路传到 ProgressRow.highlightEnabled（PlayerView → PlayerControlBar → ProgressRow），
                         * 控制那条 28px 的 pbp 曲线显不显示；参考里这颗是**假开关**（点它只切自己的 CSS 类、
                         * 曲线是写死的静态 SVG），我们这颗是真开关。
                         */
                        SettingCheck {
                            /* 宽度按本页其它行的口径（列宽 − 40），只影响那层不可见的点击区；
                               图标和文字的位置、大小与弹幕面板那一行完全一致 */
                            width: parent.width
                            text: qsTr("高能进度条")
                            /* 默认「开」：panel.highlightEnabled 初值 true（对齐参考 ctrl-setting.txt:178 的 checked 默认开） */
                            checked: panel.highlightEnabled
                            onToggled: panel.highlightToggled(!panel.highlightEnabled)
                        }

                        /* （这里原来有一行手填 mock 基址的输入框，用户明确说没提过要它 → 已删除。
                            面板高度算式里相应的那一项也去掉了，见 rightContentHeight。） */
                    }

                    /* 点这一条回左页（参考右页没有返回行，所以不画"返回"，用一个不可见的 16px 条 */
                    Item {
                        width: parent.width - 40
                        height: QtPlayerTheme.settingsPanelBackRowHeight

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: panel.pageIndex = 0
                        }
                    }
                }
            }
        }
    }
}
