// ===========================================================================
// 弹幕设置面板（两页）
//
// 结构完全照 `CicadaPlayerNext/platform/danmusetting.txt` 那份真实 DOM：
//   .bui-panel-wrap  320 x 322                ← 外壳（浮层的 contentItem）
//   .bui-panel-move  586 = 320 + 266 宽        ← 两页并排，切页就是它 translateX(0 → -320)
//     ├ 左页 320 x 322：按类型过滤 / 三个复选框 / 屏蔽词 / 四条滑块 / 高级设置
//     └ 右页 266 x 250：返回 + 更多弹幕设置 / 速度同步 / 弹幕字体 / 粗体 / 描边类型 / 恢复默认
// "向左滑出、新的从右边进来"就是同一条 move 的位移（不是两页各自动画），
// 所以这里给它一个 Behavior on x 当过渡。
//
// 图标全部是参考里那几个 SVG（放进 assets/images/dm/，未改 path）：
//   filter-{scroll,fixed,colorful,advanced}.svg  按类型过滤四项
//   filter-advanced-more.svg                     高级那项后面的 ?
//   checkbox.svg / checkbox-checked.svg          复选框两态（参考 bui-checkbox-icon-*，32x32）
//   arrow.svg                                    右箭头；返回箭头用它 rotation: 180
// 颜色：主题蓝 #00aeec（--player-fn-color）、文字 rgba(255,255,255,.8) / #ffffff。
// ===========================================================================
import QtQuick
/* 图标整颗着色（参考靠 CSS 的 fill，Qt 的 Image 改不了 fill）——白底 SVF + MultiEffect 着色 */
import QtQuick.Effects

Item {
    id: panel

    /* 弹幕层（DanmakuLayer）：所有控件都作用在它身上 */
    property var danmaku: null

    readonly property int pageWidth: 320
    readonly property int pageHeight: 322
    readonly property int rightPageWidth: 266
    readonly property int rightPageHeight: 250
    readonly property int padH: 20
    readonly property int padV: 12

    readonly property color fnColor: QtPlayerTheme.playerAccentHover
    readonly property color textColor: Qt.rgba(1, 1, 1, 0.8)

    /* 两页尺寸不一样：左页 320x322、右页 266x250（参考 bui-panel-item 的两个 width/height），
       所以外壳跟着当前页走；浮层那边把 width/height 绑到这里的 width/height。 */
    width: pageOffset < 0 ? rightPageWidth : pageWidth
    height: pageOffset < 0 ? rightPageHeight : pageHeight

    /* 0 = 左页，-pageWidth = 右页 */
    property real pageOffset: 0

    /* 鼠标在面板里的位置（由浮层 background 那层 MouseArea 喂进来，见 hoveredFilterKey） */
    property real pointerX: -1
    property real pointerY: -1

    /*
     * 鼠标在不在某个控件里（面板坐标 → 控件坐标，用 mapToItem 换算，嵌套的右页也能算对）。
     * 面板里所有控件都用它判 hover —— **不能**用 MouseArea 的 hoverEnabled：
     * 那样会抢浮层的 hover 判据，面板会被自己关掉（这一坑踩过三次）。
     */
    function pointerOn(item) {
        if (pointerX < 0 || pointerY < 0)
            return false

        var p = panel.mapToItem(item, pointerX, pointerY)

        return p.x >= 0 && p.x <= item.width && p.y >= 0 && p.y <= item.height
    }

    property int filterVersion: 0

    /*
     * 没显示的那一页要裁掉：两页并排在 586 宽的 move 上，面板只有 320 宽，
     * 不裁的话另一页会露在面板外面（也会挡住 / 抢事件）。裁在面板这一层最省事。
     */
    clip: true

    function showPage(isRight) {
        pageOffset = isRight ? -pageWidth : 0
    }

    /* 鼠标压在哪一项屏蔽图标上（面板里不能放 hoverEnabled 的 MouseArea，会抢浮层的 hover 判据） */
    readonly property string hoveredFilterKey: {
        if (pointerX < 0 || pointerY < 0)
            return ""

        var row = filterRow

        if (pointerY < row.y || pointerY > row.y + row.height)
            return ""

        for (var i = 0; i < row.children.length; ++i) {
            var item = row.children[i]

            if (item === null || item === undefined || item.modelData === undefined)
                continue

            if (pointerX >= item.x && pointerX <= item.x + item.width)
                return item.modelData.key
        }

        return ""
    }

    readonly property var filterTypes: [
        { "key": "scroll", "label": "滚动" },
        { "key": "fixed", "label": "固定" },
        { "key": "colorful", "label": "彩色" },
        { "key": "advanced", "label": "高级" }
    ]

    /* =======================================================================
     * 两页并排的那条轨道
     * ======================================================================= */
    Item {
        id: move

        width: panel.pageWidth + panel.rightPageWidth
        height: panel.height
        x: panel.pageOffset

        /* 切页过渡（参考是 CSS transition，这里等价） */
        Behavior on x {
            NumberAnimation {
                duration: 260
                easing.type: Easing.OutCubic
            }
        }

        /* =============================== 左页 =============================== */
        Item {
            id: leftPage

            width: panel.pageWidth
            height: panel.pageHeight

            Column {
                x: panel.padH
                y: panel.padV
                width: panel.width - panel.padH * 2
                spacing: 0

                /* ---- 按类型过滤 ---- */
                Item {
                    width: parent.width
                    height: 16 + 8 + 28 + 14

                    Text {
                        id: blockTitle

                        width: parent.width
                        height: 16
                        text: qsTr("按类型过滤")
                        color: "#ffffff"
                        font.pixelSize: 12
                        verticalAlignment: Text.AlignVCenter
                    }

                    Row {
                        id: filterRow

                        anchors {
                            top: blockTitle.bottom
                            topMargin: 8
                            left: parent.left
                        }
                        spacing: 22

                        Repeater {
                            model: panel.filterTypes

                            Item {
                                required property var modelData

                                readonly property bool selected: panel._isFiltered(modelData.key)
                                readonly property bool hovered: panel.hoveredFilterKey === modelData.key

                                width: 28
                                height: 28 + 14

                                Image {
                                    id: filterIcon

                                    width: 28
                                    height: 28
                                    source: "assets/images/dm/filter-" + modelData.key + ".svg"
                                    sourceSize: Qt.size(28, 28)
                                    smooth: true
                                    visible: false
                                }

                                /*
                                 * 整颗着色：未选中 rgba(255,255,255,.8)、hover 纯白、选中主题蓝
                                 * （参考是 CSS 的 fill/color；Qt 这边 Image 不能改 fill，用着色顶）。
                                 * 源 SVG 本身是白色，所以 colorization: 1 得到的就是目标色。
                                 */
                                MultiEffect {
                                    anchors.fill: filterIcon
                                    source: filterIcon
                                    colorization: 1
                                    colorizationColor: parent.selected ? panel.fnColor
                                                                       : (parent.hovered ? "#ffffff"
                                                                                         : panel.textColor)
                                }

                                Text {
                                    y: 30
                                    width: 28
                                    height: 16
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                    text: parent.modelData.label
                                    font.pixelSize: 12
                                    color: parent.selected ? panel.fnColor
                                                           : (parent.hovered ? "#ffffff" : panel.textColor)
                                }

                                Image {
                                    visible: modelData.key === "advanced"
                                    x: 24
                                    y: 30
                                    width: 11
                                    height: 11
                                    source: "assets/images/dm/filter-advanced-more.svg"
                                    sourceSize: Qt.size(11, 11)
                                    smooth: true
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: panel._toggleFilter(parent.modelData.key)
                                }
                            }
                        }
                    }
                }

                /*
                 * 三个复选框：参考是 flex 一行，但那是 320 宽的面板里塞三个中文标签 ——
                 * 实测一行放不下（七个字 + 四个字 + 六个字 ≈ 300px > 280px 可用宽），
                 * 硬挤就会横向溢出、把面板撑坏，所以拆成两行（2 + 1）。
                 * 「弹幕观看屏蔽词 / 同步屏蔽列表」按用户要求删掉了 —— 面板本来也不够高
                 * （外壳 320x322，参考自己那几行是 display:none 没显示的）。
                 */
                /* 三个复选框和上面"按类型过滤"之间拉开距离（原来贴太近） */
                Item {
                    width: parent.width
                    height: 12
                }

                Row {
                    width: parent.width
                    height: 20
                    spacing: 16

                    SettingCheck {
                        width: implicitWidth
                        text: qsTr("弹幕随屏幕缩放")
                        checked: panel._autoScale()
                        onToggled: panel._toggleAutoScale()
                    }

                    SettingCheck {
                        width: implicitWidth
                        text: qsTr("防挡字幕")
                        checked: panel.keepBottomBlank
                        onToggled: panel.keepBottomBlank = !panel.keepBottomBlank
                    }
                }

                Item {
                    width: parent.width
                    height: 20

                    SettingCheck {
                        width: implicitWidth
                        text: qsTr("智能防挡弹幕")
                        checked: panel.smartMask
                        onToggled: panel.smartMask = !panel.smartMask
                    }
                }

                /* ---- 四条滑块 ---- */
                DmSlider {
                    id: areaSlider

                    width: parent.width
                    title: qsTr("显示区域")
                    from: 0
                    to: 100
                    step: 25
                    suffix: "%"
                    onMoved: function (v) { panel._applyArea(v) }
                }

                DmSlider {
                    id: opacitySlider

                    width: parent.width
                    title: qsTr("不透明度")
                    from: 0
                    to: 100
                    step: 1
                    suffix: "%"
                    onMoved: function (v) { panel._applyOpacity(v) }
                }

                DmSlider {
                    id: fontSizeSlider

                    width: parent.width
                    title: qsTr("弹幕字号")
                    from: 0
                    to: 100
                    step: 1
                    suffix: "%"
                    onMoved: function (v) { panel._applyFontSize(v) }
                }

                DmSlider {
                    id: speedSlider

                    width: parent.width
                    title: qsTr("弹幕速度")
                    from: 0
                    to: 100
                    step: 25
                    suffix: ""
                    /* 参考里这条右边显示档位名（那份 DOM 上是"适中"） */
                    valueNames: ["极慢", "较慢", "适中", "较快", "极快"]
                    onMoved: function (v) { panel._applySpeed(v) }
                }

                /* ---- 高级设置入口 ---- */
                Item {
                    id: moreEntry

                    width: parent.width
                    height: 40

                    /* hover 由面板统一算（面板里不能放 hoverEnabled 的 MouseArea） */
                    readonly property bool hovered: panel.pointerOn(moreEntry)

                    Text {
                        anchors {
                            left: parent.left
                            verticalCenter: parent.verticalCenter
                        }
                        text: qsTr("高级设置")
                        font.pixelSize: 13
                        color: moreEntry.hovered ? panel.fnColor : "#ffffff"
                    }

                    /* 「全新【硬核会员弹幕模式】」那行字按用户要求去掉 */

                    Image {
                        id: moreArrow

                        anchors {
                            right: parent.right
                            verticalCenter: parent.verticalCenter
                        }
                        width: 16
                        height: 16
                        source: "assets/images/dm/arrow.svg"
                        sourceSize: Qt.size(16, 16)
                        smooth: true
                        visible: false
                    }

                    /*
                     * 箭头也要跟着变色（参考里 hover 时文字和箭头都变主题色）。
                     * SVG 是白色底、Image 改不了 fill，所以和四个屏蔽图标一样用整颗着色：
                     * 平时纯白、hover 变主题蓝。
                     */
                    MultiEffect {
                        anchors.fill: moreArrow
                        source: moreArrow
                        colorization: 1
                        colorizationColor: moreEntry.hovered ? panel.fnColor : "#ffffff"
                    }

                    MouseArea {
                        id: moreArea

                        anchors.fill: parent
                        /* 不能写 hoverEnabled: true —— 这条坑了四次：它接受 hover 后，浮层那两层判据
               会收到 exit，于是鼠标一移上来就被判成"离开面板"、面板自动关掉。
               hover 变色改用 panel.pointerOn(...)（位置由浮层喂进来）。 */
                        cursorShape: Qt.PointingHandCursor
                        onClicked: panel.showPage(true)
                    }
                }
            }
        }

        /* =============================== 右页 =============================== */
        Item {
            id: rightPage

            x: panel.pageWidth
            width: panel.rightPageWidth
            height: panel.rightPageHeight

            Column {
                x: panel.padH
                y: panel.padV
                width: parent.width - panel.padH * 2

                /* ---- 返回 + 标题 ---- */
                Item {
                    id: backRow

                    width: parent.width
                    height: 24

                    /* hover 由面板按鼠标位置判定（面板里不能放 hoverEnabled 的 MouseArea，
                       否则浮层判据会收到 exit、面板被误关） */
                    readonly property bool hovered: panel.pointerOn(backRow)

                    Image {
                        id: backArrow

                        anchors {
                            left: parent.left
                            verticalCenter: parent.verticalCenter
                        }
                        width: 16
                        height: 16
                        source: "assets/images/dm/arrow.svg"
                        sourceSize: Qt.size(16, 16)
                        smooth: true
                        rotation: 180
                        visible: false
                    }

                    /*
                     * 返回箭头同样跟着变色（和「高级设置」那个箭头一套做法）。
                     * 【旋转要写在这一层】Image 已经 visible: false，真正显示的是这个 MultiEffect，
                     * 所以 180 度必须加在它身上 —— 上一版写在 Image 上，显示的那层没转，箭头方向就不对了。
                     */
                    MultiEffect {
                        anchors.fill: backArrow
                        source: backArrow
                        rotation: 180
                        transformOrigin: Item.Center
                        colorization: 1
                        colorizationColor: parent.hovered ? panel.fnColor : "#ffffff"
                    }

                    Text {
                        anchors {
                            left: backArrow.right
                            leftMargin: 6
                            verticalCenter: parent.verticalCenter
                        }
                        text: qsTr("更多弹幕设置")
                        font.pixelSize: 13
                        color: parent.hovered ? panel.fnColor : "#ffffff"
                    }

                    MouseArea {
                        id: backArea

                        anchors.fill: parent
                        /* 不能写 hoverEnabled: true —— 这条坑了四次：它接受 hover 后，浮层那两层判据
               会收到 exit，于是鼠标一移上来就被判成"离开面板"、面板自动关掉。
               hover 变色改用 panel.pointerOn(...)（位置由浮层喂进来）。 */
                        cursorShape: Qt.PointingHandCursor
                        onClicked: panel.showPage(false)
                    }
                }

                Rectangle {
                    width: parent.width
                    height: 1
                    color: Qt.rgba(1, 1, 1, 0.1)
                }

                Item { width: 1; height: 10 }

                SettingCheck {
                    text: qsTr("弹幕速度同步播放倍数")
                    checked: panel.speedSync
                    onToggled: panel.speedSync = !panel.speedSync
                }

                Item { width: 1; height: 10 }

                /* ---- 弹幕字体：下拉框（参考 bui-select；点标题栏展开/收起，点项目选中） ---- */
                Item {
                    id: fontRow

                    width: parent.width
                    /* 高度固定 42：展开的列表是**浮在**下面的元素上面，不占布局位置 ——
                       之前把列表高度算进本行，等于把「粗体 / 描边类型 / 恢复默认」往下推，
                       面板一裁它们就不见了。 */
                    height: 42
                    /* 展开时整行要在后面的兄弟之上（z 只在同一父项内比较，所以加在行上） */
                    z: listOpen ? 30 : 0

                    readonly property bool hovered: panel.pointerOn(fontRow)
                    property bool listOpen: false

                    Text {
                        text: qsTr("弹幕字体")
                        color: fontRow.hovered ? panel.fnColor : "#ffffff"
                        font.pixelSize: 12
                        height: 18
                    }

                    Rectangle {
                        id: fontBox

                        anchors {
                            left: parent.left
                            right: parent.right
                            top: parent.top
                            topMargin: 20
                        }
                        height: 22
                        radius: 2
                        color: Qt.rgba(1, 1, 1, 0.08)

                        Text {
                            anchors {
                                left: parent.left
                                leftMargin: 8
                                verticalCenter: parent.verticalCenter
                            }
                            text: panel.danmakuFont
                            color: fontRow.hovered ? panel.fnColor : panel.textColor
                            font.pixelSize: 12
                        }

                        Image {
                            id: fontArrow

                            anchors {
                                right: parent.right
                                rightMargin: 6
                                verticalCenter: parent.verticalCenter
                            }
                            width: 10
                            height: 10
                            source: "assets/images/dm/arrow.svg"
                            sourceSize: Qt.size(10, 10)
                            smooth: true
                            visible: false
                        }

                        MultiEffect {
                            anchors.fill: fontArrow
                            source: fontArrow
                            /* 收起时箭头朝下(90°)、展开时朝上(270°) */
                            rotation: fontRow.listOpen ? 270 : 90
                            transformOrigin: Item.Center
                            colorization: 1
                            colorizationColor: fontRow.hovered ? panel.fnColor : "#ffffff"
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: fontRow.listOpen = !fontRow.listOpen
                        }
                    }

                    /*
                     * 展开的字体列表：高度要**按面板底部夹住**（离底边留 8px），
                     * 放不下就在列表里滚动，并在右侧显示细滚动条。
                     */
                    Rectangle {
                        id: fontListBox

                        anchors {
                            left: parent.left
                            right: parent.right
                            top: fontBox.bottom
                        }
                        /*
                         * 高度上限：参考里列表是 `max-height: 100px`（danmusetting.txt 里
                         * bui-select-list-wrap 那个内联样式），再夹一层"到面板底边留 6px"，
                         * 取小值；放不下就在内部滚动 + 右侧细滚动条。
                         * 不再把高度算进本行布局，所以列表是浮在下面元素上面、不会把它们挤没。
                         */
                        readonly property real availableHeight:
                            Math.max(44, panel.height - panel.mapFromItem(fontRow, 0, fontRow.height).y - 6)
                        readonly property real contentHeight: panel.fontList.length * 22

                        height: fontRow.listOpen ? Math.min(Math.min(contentHeight, 100), availableHeight) : 0
                        visible: height > 0
                        color: Qt.rgba(0.08, 0.08, 0.08, 0.98)
                        radius: 2
                        border.width: 1
                        border.color: Qt.rgba(1, 1, 1, 0.15)
                        z: 20

                        Flickable {
                            id: fontFlick

                            anchors {
                                fill: parent
                                rightMargin: fontListBox.contentHeight > height ? 8 : 0
                            }
                            contentHeight: fontListBox.contentHeight
                            clip: true
                            boundsBehavior: Flickable.StopAtBounds

                            Column {
                                width: fontFlick.width

                                Repeater {
                                    model: panel.fontList

                                    Item {
                                        id: fontItem

                                        required property var modelData

                                        width: parent.width
                                        height: 22

                                        readonly property bool hovered: panel.pointerOn(fontItem)

                                        Text {
                                            anchors {
                                                left: parent.left
                                                leftMargin: 8
                                                verticalCenter: parent.verticalCenter
                                            }
                                            text: fontItem.modelData.label
                                            font.pixelSize: 12
                                            color: (panel.danmakuFont === fontItem.modelData.label
                                                    || fontItem.hovered) ? panel.fnColor : panel.textColor
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                panel.danmakuFont = fontItem.modelData.label
                                                fontRow.listOpen = false
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        /* 细滚动条：只有内容超出时才出现 */
                        Rectangle {
                            visible: fontFlick.contentHeight > fontFlick.height
                            width: 4
                            radius: 2
                            color: Qt.rgba(1, 1, 1, 0.35)
                            x: fontListBox.width - 6
                            y: fontFlick.visibleArea.yPosition * fontFlick.height
                            height: Math.max(16, fontFlick.visibleArea.heightRatio * fontFlick.height)
                        }
                    }
                }

                SettingCheck {
                    text: qsTr("粗体")
                    checked: panel.boldFont
                    onToggled: panel.boldFont = !panel.boldFont
                }

                Item { width: 1; height: 8 }

                /* ---- 描边类型（三个单选） ---- */
                Text {
                    text: qsTr("描边类型")
                    color: "#ffffff"
                    font.pixelSize: 12
                    height: 18
                }

                Row {
                    spacing: 10

                    Repeater {
                        model: ["重墨", "描边", "45°投影"]

                        Item {
                            id: outlineItem

                            required property string modelData
                            required property int index

                            width: outlineText.implicitWidth + 16
                            height: 22

                            /* hover 按鼠标位置判定（面板里不能放 hoverEnabled 的 MouseArea） */
                            readonly property bool hovered: panel.pointerOn(outlineItem)

                            Rectangle {
                                anchors.fill: parent
                                radius: 2
                                color: panel.outlineType === outlineItem.index ? panel.fnColor : "transparent"
                                border.width: 1
                                border.color: panel.outlineType === outlineItem.index
                                              ? panel.fnColor
                                              : (outlineItem.hovered ? panel.fnColor : Qt.rgba(1, 1, 1, 0.3))
                            }

                            Text {
                                id: outlineText

                                anchors.centerIn: parent
                                text: outlineItem.modelData
                                font.pixelSize: 12
                                color: panel.outlineType === outlineItem.index
                                       ? "#ffffff"
                                       : (outlineItem.hovered ? panel.fnColor : panel.textColor)
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: panel.outlineType = outlineItem.index
                            }
                        }
                    }
                }

                Rectangle {
                    width: parent.width
                    height: 1
                    color: Qt.rgba(1, 1, 1, 0.1)
                }

                Item { width: 1; height: 10 }

                /* ---- 恢复默认设置 ---- */
                Item {
                    id: resetRow

                    width: parent.width
                    height: 26

                    /* hover 按鼠标位置判（面板里不能放 hoverEnabled 的 MouseArea） */
                    readonly property bool hovered: panel.pointerOn(resetRow)

                    Rectangle {
                        anchors.fill: parent
                        radius: 2
                        color: "transparent"
                        border.width: 1
                        border.color: resetRow.hovered ? panel.fnColor : Qt.rgba(1, 1, 1, 0.3)
                    }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("恢复默认设置")
                        font.pixelSize: 12
                        color: resetRow.hovered ? panel.fnColor : panel.textColor
                    }

                    MouseArea {
                        id: resetArea

                        anchors.fill: parent
                        /* 不能写 hoverEnabled: true —— 这条坑了四次：它接受 hover 后，浮层那两层判据
               会收到 exit，于是鼠标一移上来就被判成"离开面板"、面板自动关掉。
               hover 变色改用 panel.pointerOn(...)（位置由浮层喂进来）。 */
                        cursorShape: Qt.PointingHandCursor
                        onClicked: panel._resetDefaults()
                    }
                }
            }
        }
    }

    /* =======================================================================
     * 复选行（图标用参考那两个 32x32 的 SVG，不手画方框）
     * ======================================================================= */
    component SettingCheck: Item {
        id: check

        property string text: ""
        property bool checked: false

        signal toggled()

        width: parent.width
        height: 20
        /* 放进 Row 时用 implicitWidth 横排（见 left-radio 那三行） */
        implicitWidth: 16 + 6 + labelText.implicitWidth
        implicitHeight: 20

        /* hover 由面板按鼠标位置判定（不能用 MouseArea 的 hoverEnabled，见下面那条注释） */
        readonly property bool hovered: panel.pointerOn(check)

        Image {
            id: box

            anchors.verticalCenter: parent.verticalCenter
            width: 16
            height: 16
            source: check.checked ? "assets/images/dm/checkbox-checked.svg"
                                  : "assets/images/dm/checkbox.svg"
            sourceSize: Qt.size(16, 16)
            smooth: true
            opacity: check.checked ? 1 : 0.75
        }

        Text {
            id: labelText

            anchors {
                left: box.right
                leftMargin: 6
                verticalCenter: parent.verticalCenter
            }
            text: check.text
            font.pixelSize: 12
            /* 选中纯白；只是 hover 也变亮一点（参考的 hover 变白） */
            color: (check.checked || check.hovered) ? "#ffffff" : panel.textColor
        }

        MouseArea {
            anchors.fill: parent
            /* 不能写 hoverEnabled: true —— 这条坑了四次：它接受 hover 后，浮层那两层判据
               会收到 exit，于是鼠标一移上来就被判成"离开面板"、面板自动关掉。
               hover 变色改用 panel.pointerOn(...)（位置由浮层喂进来）。 */
            cursorShape: Qt.PointingHandCursor
            onClicked: check.toggled()
        }
    }

    /* =======================================================================
     * 滑块：30px 行高、标题 80、轨道 6px/圆角 6、圆点 14、数值框 60 + padding-left 20
     * ======================================================================= */
    component DmSlider: Item {
        id: slider

        property string title: ""
        property real from: 0
        property real to: 100
        property real step: 1
        property string suffix: ""
        property var valueNames: null
        property real value: 50

        signal moved(real value)

        /* 行高 30（参考 .bui-progress 就是 height:30px + margin-bottom:6px） */
        height: 30
        property real titleWidth: 80

        readonly property int stepCount: step > 0 ? Math.round((to - from) / step) : 0
        readonly property bool showTicks: stepCount > 0 && stepCount <= 5

        readonly property string valueText: {
            if (valueNames !== null && valueNames.length > 0) {
                var i = Math.round((value - from) / Math.max(1, step))

                if (i >= 0 && i < valueNames.length)
                    return valueNames[i]
            }

            return Math.round(value) + suffix
        }

        function setValue(v, emit) {
            var clamped = Math.max(from, Math.min(to, v))

            if (step > 0)
                clamped = Math.round(clamped / step) * step

            value = clamped

            if (emit === true)
                moved(value)
        }

        function valueAtX(px) {
            var w = trackArea.width

            if (w <= 0)
                return from

            return from + (to - from) * Math.max(0, Math.min(1, px / w))
        }

        Text {
            width: slider.titleWidth
            height: parent.height
            text: slider.title
            color: "#ffffff"
            font.pixelSize: 12
            verticalAlignment: Text.AlignVCenter
        }

        Item {
            id: trackArea

            anchors {
                left: parent.left
                leftMargin: slider.titleWidth
                right: valueLabel.left
                rightMargin: 0
                verticalCenter: parent.verticalCenter
            }
            height: 14

            readonly property real ratio: (slider.to - slider.from) > 0
                                          ? (slider.value - slider.from) / (slider.to - slider.from)
                                          : 0

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: 6
                radius: 6
                color: Qt.rgba(1, 1, 1, 0.2)
            }

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width * Math.max(0, Math.min(1, trackArea.ratio))
                height: 6
                radius: 6
                color: panel.fnColor
            }

            /* 步长刻度点：照参考 .bui-progress-item 的定位（容器左右各收 6px + 两端 margin 修正） */
            Repeater {
                model: slider.showTicks ? slider.stepCount + 1 : 0

                Rectangle {
                    required property int index

                    width: 4
                    height: 4
                    radius: 2
                    color: "#ffffff"
                    y: (trackArea.height - height) / 2 - 1
                    x: 6 + (trackArea.width - 12) * (index / slider.stepCount)
                       + (index === 0 ? -4 : (index === slider.stepCount ? 0 : -1))
                }
            }

            /* 圆点 14x14 白色 */
            Rectangle {
                width: 14
                height: 14
                radius: 7
                color: "#ffffff"
                x: parent.width * Math.max(0, Math.min(1, trackArea.ratio)) - width / 2
                anchors.verticalCenter: parent.verticalCenter
            }

            MouseArea {
                id: sliderArea

                anchors {
                    fill: parent
                    topMargin: -9
                    bottomMargin: -9
                    leftMargin: -6
                    rightMargin: -6
                }
                cursorShape: Qt.PointingHandCursor
                onPressed: function (mouse) { slider.setValue(slider.valueAtX(mouse.x + sliderArea.anchors.leftMargin), true) }
                onPositionChanged: function (mouse) {
                    if (pressed)
                        slider.setValue(slider.valueAtX(mouse.x + sliderArea.anchors.leftMargin), true)
                }
            }
        }

        Text {
            id: valueLabel

            anchors {
                right: parent.right
                verticalCenter: parent.verticalCenter
            }
            width: 60
            leftPadding: 20
            horizontalAlignment: Text.AlignRight
            text: slider.valueText
            color: panel.textColor
            font.pixelSize: 12
            verticalAlignment: Text.AlignVCenter
        }
    }

    /* =======================================================================
     * 面板自身的开关项状态（参考右页那几个复选框；防挡相关的引擎侧还没接）
     * ======================================================================= */
    property bool keepBottomBlank: false
    property bool smartMask: true
    property bool speedSync: true
    property bool boldFont: true
    property int outlineType: 0
    property string danmakuFont: qsTr("黑体")

    /* 参考的字体下拉里那 8 项（value 就是 CSS font-family，Qt 侧取第一个可用的字体名） */
    readonly property var fontList: [
        { "label": "黑体", "family": "SimHei" },
        { "label": "宋体", "family": "SimSun" },
        { "label": "新宋体", "family": "NSimSun" },
        { "label": "仿宋", "family": "FangSong" },
        { "label": "微软雅黑", "family": "Microsoft YaHei" },
        { "label": "微软雅黑 Light", "family": "Microsoft YaHei Light" },
        { "label": "Noto Sans DemiLight", "family": "Noto Sans CJK SC DemiLight" },
        { "label": "Noto Sans Regular", "family": "Noto Sans CJK SC Regular" }
    ]

    /*
     * 这四项一改就写进引擎（面板里那几个控件只改自己的状态，所以在这里统一转发）。
     * 引擎侧对应：fontBold / outlineType / speedFollowRate / fontFamily。
     */
    onBoldFontChanged: if (danmaku !== null) danmaku.fontBold = boldFont
    onOutlineTypeChanged: if (danmaku !== null) danmaku.outlineType = outlineType
    onSpeedSyncChanged: if (danmaku !== null) danmaku.speedFollowRate = speedSync
    onDanmakuFontChanged: {
        if (danmaku === null)
            return

        for (var i = 0; i < fontList.length; ++i) {
            if (fontList[i].label === danmakuFont) {
                danmaku.fontFamily = fontList[i].family
                return
            }
        }

        danmaku.fontFamily = danmakuFont
    }

    /* 弹幕字体：点一下切到下一项（参考是下拉列表，这里先把功能接通） */
    function _cycleFont() {
        for (var i = 0; i < fontList.length; ++i) {
            if (fontList[i].label === danmakuFont) {
                danmakuFont = fontList[(i + 1) % fontList.length].label
                return
            }
        }

        danmakuFont = fontList[0].label
    }

    /* =======================================================================
     * 与弹幕层之间的读写
     * ======================================================================= */
    function _autoScale() {
        return danmaku !== null && danmaku.autoScale
    }

    function _toggleAutoScale() {
        if (danmaku !== null)
            danmaku.setAutoScale(!danmaku.autoScale)
    }

    function syncFromEngine() {
        if (danmaku === null)
            return

        areaSlider.setValue(Math.round(danmaku.area * 100), false)
        opacitySlider.setValue(Math.round(danmaku.danmakuOpacity * 100), false)
        fontSizeSlider.setValue((danmaku.baseFontSize - 12) / 20 * 100, false)
        speedSlider.setValue((danmaku.speed - 1) * 25, false)
        /* 右页那四项也从引擎读回来 */
        boldFont = danmaku.fontBold
        outlineType = danmaku.outlineType
        speedSync = danmaku.speedFollowRate
        filterVersion = filterVersion + 1
    }

    function _isFiltered(key) {
        var v = filterVersion

        if (danmaku === null)
            return false

        var f = danmaku.filter

        if (f === null || f === undefined)
            return false

        return f[key] === true
    }

    function _toggleFilter(key) {
        if (danmaku === null)
            return

        var f = danmaku.filter

        if (f === null || f === undefined)
            f = ({})

        var next = ({})

        for (var k in f)
            next[k] = f[k]

        next[key] = !(f[key] === true)
        danmaku.setFilter(next)
        filterVersion = filterVersion + 1
    }

    function _applyArea(v) {
        if (danmaku !== null)
            danmaku.setArea(Math.max(0.25, v / 100))
    }

    function _applyOpacity(v) {
        if (danmaku !== null)
            danmaku.danmakuOpacity = v / 100
    }

    function _applyFontSize(v) {
        if (danmaku !== null)
            danmaku.setFontSize(Math.round(12 + v * 0.2))
    }

    function _applySpeed(v) {
        if (danmaku !== null)
            danmaku.setSpeed(1 + Math.round(v / 25))
    }

    function _resetDefaults() {
        if (danmaku === null)
            return

        danmaku.setArea(1)
        danmaku.setSpeed(3)
        danmaku.setFontSize(18)
        danmaku.danmakuOpacity = 1
        danmaku.setAutoScale(true)
        danmaku.resetFilter()
        /* 右页那四项也一起还原 */
        boldFont = true
        outlineType = 0
        speedSync = true
        danmakuFont = fontList[0].label
        syncFromEngine()
    }
}
