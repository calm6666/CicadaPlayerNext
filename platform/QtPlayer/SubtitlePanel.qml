// ===========================================================================
// 字幕面板（参考 bilibili 播放器里那颗「字幕」按钮展开的面板）
//
// 对照的是两份东西：
//   * DOM 结构：CicadaPlayerNext/platform/subtitles.txt（.bpx-player-ctrl-subtitle 那一整棵）
//   * CSS 数值：CicadaPlayerNext/platform/style.css 里全部 .bpx-player-ctrl-subtitle-* /
//               .bui-select / .bui-slider / .bui-checkbox / .bui-switch / .bui-button 规则
//               （那个文件是压缩成一行的，规则已按"带 @media 上下文"的方式抽出来核对过）
//
// 结构（和 DOM 一一对应）：
//   .bui-panel-wrap 168x311            → 面板外壳（左页那一页的尺寸）
//     .bui-panel-move 434 = 168 + 266  → Item move（两页并排，切页时整条平移）
//       .bui-panel-item 168x311        → leftPage（字幕列表页）
//       .bui-panel-item 266x260        → rightPage（字幕设置页）
//
// 几个反直觉但**照参考抄**的点：
//   * 左页顶部是"字幕"（14px、opacity .4）一行 + "关闭"一行（40px 高、当前选中所以是主色），
//     "关闭"不是标题栏按钮，它就是列表里的"关闭字幕"那一项（参考给的类名是 close-switch）。
//   * 语言列表可视高度只有 135px，6 项 × 40px = 240px，所以它是**可滚动**的
//     （参考里右侧那条 3px 滚动条默认 opacity:0，鼠标移上去才显形）。
//   * 右页按 CSS 逐条累加是 266px，比这一页的 260 高 6px（参考自己就是把最后那截裁掉的，
//     "恢复默认设置"会缺下半截）。这里把底下两个 10px 的间距各收 3px → 正好 260 落地，
//     于是整页**不需要滚动**、也就不会冒出滚动条（容器仍是 Flickable，
//     以后真要放更多东西进去它还能滚）。
//   * 右页**没有返回按钮**：参考的 DOM 里那一页就只有 4 个设置项 + 恢复默认设置，
//     没有返回上一页的元素，所以这里也不加（离开面板再悬停会回到左页）。
//
// 交互约定（见 docs/QML-POPUP-HOVER-PITFALLS.md）：
//   * 面板里**所有** MouseArea 都**不写 hoverEnabled** —— 一写就会把浮层那层判据的 hover
//     抢走，鼠标压到控件上时面板会被判成"鼠标已离开"而自己关掉。
//   * hover 变色统一用 panel.pointerOn(item)（鼠标位置由浮层喂进来，见 pointerX/pointerY）。
//   * 下拉列表展开时用 z 抬到兄弟之上（浮层之上还是浮层，参考里是 z-index:2）。
// ===========================================================================
import QtQuick
import QtQuick.Effects

Item {
    id: panel

    /* -----------------------------------------------------------------------
     * 尺寸：照参考 bui-panel-wrap / bui-panel-item 的内联尺寸
     * ----------------------------------------------------------------------- */
    readonly property int pageWidth: 168         /* 左页 168x311 */
    readonly property int pageHeight: 311
    readonly property int rightPageWidth: 266    /* 右页 266x260 */
    readonly property int rightPageHeight: 260

    /* .bpx-player-ctrl-subtitle-menu-left / -right：padding: 0 20px */
    readonly property int padH: 20

    /* 颜色：--bpx-primary-color / 面板文字 / 分隔线 rgba(229,233,239,.1) / hover hsla(0,0%,100%,.1) */
    readonly property color fnColor: QtPlayerTheme.playerAccent
    readonly property color textColor: "#ffffff"
    readonly property color separatorColor: Qt.rgba(229 / 255, 233 / 255, 239 / 255, 0.1)
    readonly property color hoverBg: Qt.rgba(1, 1, 1, 0.1)
    readonly property color dimText: Qt.rgba(1, 1, 1, 0.4)      /* hsla(0,0%,100%,.4) */
    readonly property color selectListBg: "#212121"             /* .bui-select.bui-dark .bui-select-list */
    readonly property color sliderTrackBg: "#e7e7e7"            /* .bui-slider .bui-bar-wrap */

    /* 外壳尺寸跟着当前页走（浮层那边读 pageOffset，不绑本 Item 的 width/height，避免绑定循环） */
    width: pageOffset < 0 ? rightPageWidth : pageWidth
    height: pageOffset < 0 ? rightPageHeight : pageHeight

    /* 0 = 左页，-pageWidth = 右页（参考 bui-panel-move 的 translateX） */
    property real pageOffset: 0

    /* 鼠标在面板里的位置（由浮层 background 那层 MouseArea 喂进来） */
    property real pointerX: -1
    property real pointerY: -1

    /* 哪一页的下拉列表是展开的（-1 = 都收着；参考里同时只会开一个） */
    property int openSelect: -1

    /*
     * 鼠标在不在某个控件里：面板坐标 → 控件坐标（mapToItem，嵌套/滚动过的都能算对）。
     * 面板里所有 hover 变色都走它 —— 不能用 MouseArea 的 hoverEnabled（会抢浮层的判据）。
     */
    function pointerOn(item) {
        if (pointerX < 0 || pointerY < 0)
            return false

        var p = panel.mapToItem(item, pointerX, pointerY)

        return p.x >= 0 && p.x <= item.width && p.y >= 0 && p.y <= item.height
    }

    /* 没显示的那一页要裁掉（两页并排在 434 宽的 move 上，面板只有 168/266 宽） */
    clip: true

    function showPage(isRight) {
        pageOffset = isRight ? -pageWidth : 0
        openSelect = -1
    }

    /* 关面板时调用：下次打开回到"下拉都收着"的干净状态 */
    function resetLists() {
        openSelect = -1
    }

    /* -----------------------------------------------------------------------
     * 面板状态 —— 初始值照参考 DOM 里当前的选中项
     * ----------------------------------------------------------------------- */
    /* 语言列表：-1 = 「关闭」（DOM 里 bpx-state-active 挂在"关闭"上，说明当前没开字幕） */
    property int languageIndex: -1
    property int fontScaleIndex: 2      /* 适中（data-value="1"） */
    property int colorIndex: 0          /* 白色 */
    property int shadowIndex: 0         /* 无描边 */
    property int positionIndex: 1       /* 底部居中 */
    property real bgOpacity: 0.87       /* 背景不透明度 87% */
    property bool fitToScale: true      /* 等比缩放：DOM 里 checked */
    property bool fadeInOut: false      /* 淡入淡出：DOM 里没 checked */
    property bool bilingual: false      /* 双语字幕开关：DOM 里没 checked */

    /* ---- 选项表：条数和顺序照 DOM 里的 li ---- */
    readonly property var languageList: [
        { "label": "中文" },
        { "label": "English" },
        { "label": "日本語" },
        { "label": "Español" },
        { "label": "العربية" },
        { "label": "Português" }
    ]

    /* 字幕大小：data-value 0.6 / 0.8 / 1 / 1.3 / 1.6 */
    readonly property var fontScaleList: [
        { "label": "最小", "value": 0.6 },
        { "label": "较小", "value": 0.8 },
        { "label": "适中", "value": 1 },
        { "label": "较大", "value": 1.3 },
        { "label": "最大", "value": 1.6 }
    ]

    /*
     * 字幕颜色：data-value 是 10 进制色号，这里直接写 #RRGGBB。
     *
     * 【这是"字幕可选颜色" —— 数据，不是主题色】里面那几个蓝（#673AB7/#3F51B5/#2196F3/#03A9F4）
     * 是官方给用户挑的字幕颜色，**不要**换成 QtPlayerTheme.playerAccent：
     * 主题色管界面强调色（进度条/选中/hover），这里是"字幕本身的颜色"。
     * （find-blue-colors.js 会把这几行列出来 —— 属于预期。）
     */
    readonly property var colorList: [
        { "label": "白色", "value": 16777215, "color": "#ffffff" },
        { "label": "红色", "value": 16007990, "color": "#F44336" },
        { "label": "紫色", "value": 10233776, "color": "#9C27B0" },
        { "label": "深紫色", "value": 6765239, "color": "#673AB7" },
        { "label": "靛青色", "value": 4149685, "color": "#3F51B5" },
        { "label": "蓝色", "value": 2201331, "color": "#2196F3" },
        { "label": "亮蓝色", "value": 240116, "color": "#03A9F4" }
    ]

    /* 描边方式：0 无描边 / 1 重墨 / 2 描边 / 3 45°投影 */
    readonly property var shadowList: [
        { "label": "无描边", "value": 0 },
        { "label": "重墨", "value": 1 },
        { "label": "描边", "value": 2 },
        { "label": "45°投影", "value": 3 }
    ]

    /* 默认位置 */
    readonly property var positionList: [
        { "label": "左下角", "value": "bottom-left" },
        { "label": "底部居中", "value": "bottom-center" },
        { "label": "右下角", "value": "bottom-right" },
        { "label": "左上角", "value": "top-left" },
        { "label": "顶部居中", "value": "top-center" },
        { "label": "右上角", "value": "top-right" }
    ]

    function _resetDefaults() {
        fontScaleIndex = 2
        colorIndex = 0
        shadowIndex = 0
        positionIndex = 1
        bgOpacity = 0.87
        fitToScale = true
        fadeInOut = false
    }

    /* =======================================================================
     * 两页并排的那条轨道（.bui-panel-move：transform .25s）
     * ======================================================================= */
    Item {
        id: move

        width: panel.pageWidth + panel.rightPageWidth
        height: panel.height
        x: panel.pageOffset

        Behavior on x {
            NumberAnimation {
                duration: 250
                easing.type: Easing.OutCubic
            }
        }

        /* =============================== 左页 =============================== */
        /* .bui-panel-item.bui-panel-item-active —— 168x311
           y 用"贴底"：参考的 bui-panel-move 是 align-items:flex-end（两页底边对齐），
           切页时外壳高度在 311↔260 之间过渡，贴底才能让当前页始终压在底边上。 */
        Item {
            id: leftPage

            y: panel.height - height
            width: panel.pageWidth
            height: panel.pageHeight

            /* ---- "字幕"标题：14px、白色、opacity .4 ----
               .bpx-player-ctrl-subtitle-title-area{padding-top:4px}
               → .bpx-player-ctrl-subtitle-add-wrap{height:32px}
               → .bpx-player-ctrl-subtitle-title{margin-top:10px}                */
            Text {
                id: leftTitle

                x: panel.padH
                y: 4 + 10
                text: qsTr("字幕")
                color: panel.textColor
                opacity: 0.4
                font.pixelSize: 14
            }

            /* ---- "关闭"：它就是列表里的"关闭字幕"项（参考类名 close-switch）----
               height:40px; line-height:40px; margin:2px 8px 0 -12px;
               padding-left:12px; width:calc(100% + 12px)
               → 相对内容框（x=20）往左伸出 12px，所以面板坐标是 x=8、宽 128+12=140 */
            Item {
                id: closeRow

                x: panel.padH - 12
                y: 4 + 32 + 2
                width: panel.pageWidth - panel.padH * 2 + 12
                height: 40

                readonly property bool hovered: panel.pointerOn(closeRow)

                /*
                 * hover 高亮：参考是 .bpx-player-ctrl-subtitle-close-switch:hover
                 * {background:hsla(0,0%,100%,.1); border-radius:4px}，铺满整行 40px。
                 * 上下各收 2px：参考那种"相邻两行的高亮块上下贴死"在深色面板上看着很挤
                 * （用户反馈"左右有边距、上下没有，很别扭"），收 2px 之后两行高亮之间
                 * 有 4px 缝，行高和点击区域仍然是 40px（不产生点不到的死区）。
                 */
                Rectangle {
                    x: 8
                    y: 2
                    width: parent.width - 16
                    height: parent.height - 4
                    radius: 4
                    color: closeRow.hovered ? panel.hoverBg : "transparent"
                }

                Text {
                    anchors {
                        left: parent.left
                        leftMargin: 12
                    }
                    height: parent.height
                    verticalAlignment: Text.AlignVCenter
                    /* .bpx-state-active → 主色（当前就是"关闭"被选中） */
                    color: panel.languageIndex < 0 ? panel.fnColor : panel.textColor
                    font.pixelSize: 12
                    text: qsTr("关闭")
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: panel.languageIndex = -1
                }
            }

            /* ---- 语言列表 ----
               .bpx-player-ctrl-subtitle-language{margin-left:-20px; width:calc(100% + 40px)}
               → 列表本身占满整个面板宽度；每项 margin:0 8px（左右各内缩 8） */
            Item {
                id: languageBox

                x: 0
                y: 38 + 40
                width: panel.pageWidth
                height: 135          /* .bpx-player-ctrl-subtitle-major-content 的内联高度 */

                readonly property bool hovered: panel.pointerOn(languageBox)

                Flickable {
                    id: languageFlick

                    anchors.fill: parent
                    contentHeight: panel.languageList.length * 40
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    Column {
                        width: languageFlick.width

                        Repeater {
                            model: panel.languageList

                            Item {
                                id: languageItem

                                required property var modelData
                                required property int index

                                width: parent.width
                                height: 40

                                readonly property bool hovered: panel.pointerOn(languageItem)
                                readonly property bool selected: panel.languageIndex === languageItem.index

                                /* 每项自己再左右内缩 8（margin:0 8px）+ padding-left 12；
                                   高亮上下各收 2px（和"关闭"那一行同一个理由，见那段注释） */
                                Rectangle {
                                    x: 8
                                    y: 2
                                    width: parent.width - 16
                                    height: parent.height - 4
                                    radius: 4
                                    color: languageItem.hovered ? panel.hoverBg : "transparent"
                                }

                                Text {
                                    id: languageText

                                    x: 8 + 12
                                    height: parent.height
                                    verticalAlignment: Text.AlignVCenter
                                    font.pixelSize: 12
                                    color: languageItem.selected ? panel.fnColor : panel.textColor
                                    text: languageItem.modelData.label
                                }

                                /* 语言后面那个"AI 字幕"小胶囊（参考里每项都有这个 svg，14x12、上边距 14） */
                                Image {
                                    id: languageBadge

                                    x: languageText.x + languageText.implicitWidth + 4   /* flex gap:4px */
                                    y: 14
                                    width: 14
                                    height: 12
                                    source: "assets/images/subtitle-badge.svg"
                                    sourceSize: Qt.size(14, 12)
                                    smooth: true
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: panel.languageIndex = languageItem.index
                                }
                            }
                        }
                    }
                }

                /* 细滚动条：3px、#e5e7ef @ .3，默认 opacity 0、鼠标移上去才显形
                   （.bpx-player-ctrl-subtitle-major-content .bscroll-vertical-scrollbar） */
                Rectangle {
                    visible: languageFlick.contentHeight > languageFlick.height
                    width: 3
                    x: parent.width - 3
                    y: languageFlick.visibleArea.yPosition * languageFlick.height
                    height: Math.max(16, languageFlick.visibleArea.heightRatio * languageFlick.height)
                    color: "#e5e7ef"
                    opacity: languageBox.hovered ? 0.3 : 0
                    Behavior on opacity {
                        NumberAnimation { duration: 300 }
                    }
                }
            }

            /* ---- 分隔线 ----
               .bpx-player-ctrl-subtitle-separator-bottom{margin:7.5px 0 7.5px -8px;
                 width:calc(100% + 16px); border-bottom:1px solid rgba(229,233,239,.1)} */
            Rectangle {
                id: leftSeparator

                x: panel.padH - 8
                y: 213 + 7.5
                width: panel.pageWidth - panel.padH * 2 + 16
                height: 1
                color: panel.separatorColor
            }

            /* ---- 双语字幕开关 ----
               .bpx-player-ctrl-subtitle-bilingual-bottom{height:36px; padding-top:4px; width:100%}
               .bui-switch-label{justify-content:space-between; align-items:center; width:100%}
               .bui-switch-body{width:30px; height:20px; border-radius:10px; background:#757575}
                 选中 → #00a1d6；.bui-switch-dot{16x16 白色、left:2px、top:2px}
                 选中 → left:100%; margin-left:-18px（也就是 x=12） */
            Item {
                id: bilingualRow

                x: panel.padH
                y: 213 + 16
                width: panel.pageWidth - panel.padH * 2
                height: 36

                readonly property bool hovered: panel.pointerOn(bilingualRow)

                Text {
                    anchors {
                        left: parent.left
                        verticalCenter: parent.verticalCenter
                    }
                    text: qsTr("双语字幕")
                    font.pixelSize: 12
                    color: bilingualRow.hovered ? panel.fnColor : panel.textColor
                }

                Rectangle {
                    id: bilingualBody

                    anchors {
                        right: parent.right
                        verticalCenter: parent.verticalCenter
                    }
                    width: 30
                    height: 20
                    radius: 10
                    color: panel.bilingual ? panel.fnColor : "#757575"

                    Behavior on color {
                        ColorAnimation { duration: 200 }
                    }

                    Rectangle {
                        id: bilingualDot

                        y: 2
                        width: 16
                        height: 16
                        radius: 8
                        color: "#ffffff"
                        x: panel.bilingual ? parent.width - 18 : 2

                        Behavior on x {
                            NumberAnimation { duration: 200; easing.type: Easing.OutCubic }
                        }
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: panel.bilingual = !panel.bilingual
                }
            }

            /* ---- "字幕设置 >"：点它切到右页 ----
               .bpx-player-ctrl-subtitle-setting{height:40px; display:flex;
                 justify-content:space-between; align-items:center; fill:#fff}
               hover → fill/color 都变主色 */
            Item {
                id: settingRow

                x: panel.padH
                y: 213 + 16 + 36
                width: panel.pageWidth - panel.padH * 2
                height: 40

                readonly property bool hovered: panel.pointerOn(settingRow)

                Text {
                    anchors {
                        left: parent.left
                        verticalCenter: parent.verticalCenter
                    }
                    text: qsTr("字幕设置")
                    font.pixelSize: 12
                    color: settingRow.hovered ? panel.fnColor : panel.textColor
                    Behavior on color {
                        ColorAnimation { duration: 300 }
                    }
                }

                /* 箭头是 16px 宽的白色 svg（参考里那个 viewBox 0 0 16 16 的 chevron） */
                Image {
                    id: settingArrow

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

                MultiEffect {
                    anchors.fill: settingArrow
                    source: settingArrow
                    colorization: 1
                    colorizationColor: settingRow.hovered ? panel.fnColor : "#ffffff"
                    opacity: 0.9                     /* .bpx-player-ctrl-subtitle-setting .bpx-common-svg-icon{opacity:.9} */
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: panel.showPage(true)
                }
            }
        }

        /* =============================== 右页 =============================== */
        /* .bui-panel-item（266x260）—— 字幕设置页；同样贴底（见左页那段说明） */
        Item {
            id: rightPage

            x: panel.pageWidth
            y: panel.height - height
            width: panel.rightPageWidth
            height: panel.rightPageHeight

            /* 内容比这页高 5px（见文件头那笔账），所以按参考那样做滚动区 */
            Flickable {
                id: rightFlick

                x: panel.padH
                y: 0
                width: panel.rightPageWidth - panel.padH * 2      /* 226 */
                height: panel.rightPageHeight

                /* 内容正好 260（见文件头那笔账），所以正常情况不会出现滚动条；
                   真出现了也让它**贴着面板右边缘**（参考里这条滚动条就在
                   settings-content 的 right:0 上，而那个容器是伸出 20px 内边距的）。 */
                contentHeight: 260
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                readonly property bool hovered: panel.pointerOn(rightFlick)

                /* ---- 第一行：字幕大小 / 字幕颜色（各 50%，中间 margin-right 20）---- */
                SubSelect {
                    id: fontScaleSelect

                    x: 0
                    y: 12
                    width: 103
                    selectId: 0
                    title: qsTr("字幕大小")
                    options: panel.fontScaleList
                    currentIndex: panel.fontScaleIndex
                    onPicked: function (index) { panel.fontScaleIndex = index }
                }

                SubSelect {
                    id: colorSelect

                    x: 123
                    y: 12
                    width: 103
                    selectId: 1
                    title: qsTr("字幕颜色")
                    options: panel.colorList
                    currentIndex: panel.colorIndex
                    onPicked: function (index) { panel.colorIndex = index }
                }

                /* ---- 第二行：描边方式 / 默认位置 ---- */
                SubSelect {
                    id: shadowSelect

                    x: 0
                    y: 67
                    width: 103
                    selectId: 2
                    title: qsTr("描边方式")
                    options: panel.shadowList
                    currentIndex: panel.shadowIndex
                    onPicked: function (index) { panel.shadowIndex = index }
                }

                SubSelect {
                    id: positionSelect

                    x: 123
                    y: 67
                    width: 103
                    selectId: 3
                    title: qsTr("默认位置")
                    options: panel.positionList
                    currentIndex: panel.positionIndex
                    onPicked: function (index) { panel.positionIndex = index }
                }

                /* ---- 第三行：背景不透明度 ----
                   .bpx-player-ctrl-subtitle-opacity-header{margin-bottom:6px; min-height:16px}
                   .bpx-player-ctrl-subtitle-opacity-content{height:12px; width:100%}
                   （.bpx-player-ctrl-subtitle-opacity-percent 在参考里被 display:none 掉了，所以不画） */
                Item {
                    id: opacityItem

                    x: 0
                    y: 122
                    width: 226
                    height: 34

                    readonly property bool hovered: panel.pointerOn(opacityItem)

                    Text {
                        width: parent.width
                        height: 16
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("背景不透明度")
                        font.pixelSize: 12
                        color: panel.textColor
                    }

                    SubSlider {
                        id: opacitySlider

                        y: 22
                        width: parent.width
                        value: panel.bgOpacity
                        onMoved: function (v) { panel.bgOpacity = v }
                    }
                }

                /* ---- 第四行：其它设置（两个复选框）----
                   .bpx-player-ctrl-subtitle-other{margin:10px 0}（和上一项的 12px 折叠，取 12）
                   .bpx-player-ctrl-subtitle-other-title{margin:0 12px 8px; font-size:12px; opacity:.6}
                   .bpx-player-ctrl-subtitle-scale{margin:0 12px} / -fade{margin-left:14px} */
                Item {
                    id: otherItem

                    x: 0
                    y: 168
                    width: 226
                    height: 50

                    Text {
                        id: otherTitle

                        x: 12
                        width: parent.width - 24
                        height: 16
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("其它设置")
                        font.pixelSize: 12
                        color: panel.textColor
                        opacity: 0.6
                    }

                    SubCheck {
                        id: scaleCheck

                        x: 12
                        anchors.top: otherTitle.bottom
                        anchors.topMargin: 8
                        text: qsTr("等比缩放")
                        checked: panel.fitToScale
                        onToggled: panel.fitToScale = !panel.fitToScale
                    }

                    SubCheck {
                        id: fadeCheck

                        anchors {
                            left: scaleCheck.right
                            leftMargin: 14
                            top: scaleCheck.top
                        }
                        text: qsTr("淡入淡出")
                        checked: panel.fadeInOut
                        onToggled: panel.fadeInOut = !panel.fadeInOut
                    }
                }

                /* ---- 底部：分隔线 + 恢复默认设置 ----
                   .bpx-player-ctrl-subtitle-settings-footer .bpx-player-ctrl-subtitle-separator
                     {border-bottom:1px solid rgba(229,233,239,.1); margin:7.5px 0 7.5px -8px;
                      width:calc(100% + 16px)}
                   .bpx-player-ctrl-subtitle-reset{height:22px; width:116px; margin:10px 0}

                   【两处 10 收成 7】照参考原样累加是 266px，比这一页的 260 高 6px
                   （参考自己就是让最后那截被面板裁掉的）。裁掉的样子是"恢复默认设置"
                   下半截看不见，很难看；所以把"其它设置"的下边距和 reset 的上边距
                   各收 3px → 正好 260 落地：**整页不需要滚动，滚动条也就不会出现**。 */
                Rectangle {
                    id: footerSeparator

                    x: -8
                    y: 222.5
                    width: 226 + 16
                    height: 1
                    color: panel.separatorColor
                }

                Item {
                    id: resetButton

                    x: 0
                    y: 238
                    width: 116
                    height: 22

                    readonly property bool hovered: panel.pointerOn(resetButton)

                    /* .bui-button .bui-area.bui-button-transparent
                       {border:1px solid hsla(0,0%,100%,.2); border-radius:2px; color:#fff}
                       hover → border-color/color 都变主色 */
                    Rectangle {
                        anchors.fill: parent
                        radius: 2
                        color: "transparent"
                        border.width: 1
                        border.color: resetButton.hovered ? panel.fnColor : Qt.rgba(1, 1, 1, 0.2)
                        Behavior on border.color {
                            ColorAnimation { duration: 200 }
                        }

                        Text {
                            anchors.centerIn: parent
                            text: qsTr("恢复默认设置")
                            font.pixelSize: 12
                            color: resetButton.hovered ? panel.fnColor : panel.textColor
                            Behavior on color {
                                ColorAnimation { duration: 200 }
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: panel._resetDefaults()
                    }
                }
            }

            /* 右页的细滚动条（正常情况下不会出现，见上面 contentHeight 那段）。
               x 直接贴面板右边缘：参考那条是 right:0，而 settings-content 是
               `width:calc(100% + 20px)`（往右伸出内边距），所以它本来就压在最边上。 */
            Rectangle {
                visible: rightFlick.contentHeight > rightFlick.height
                width: 3
                x: panel.rightPageWidth - 3
                y: rightFlick.visibleArea.yPosition * rightFlick.height
                height: Math.max(16, rightFlick.visibleArea.heightRatio * rightFlick.height)
                color: "#e5e7ef"
                opacity: rightFlick.hovered ? 0.3 : 0
                Behavior on opacity {
                    NumberAnimation { duration: 300 }
                }
            }
        }
    }

    /* =======================================================================
     * 下拉框（参考 .bui-select.bui-dark）
     *   .bui-select-wrap{height:22px}
     *   .bui-select-border{border:1px solid hsla(0,0%,100%,.2); border-radius:2px}
     *   .bui-select-header{height:20px; line-height:20px; padding:0 22px 0 7px}
     *   .bui-select-arrow{position:absolute; right:0; width:22px; 居中}
     *   .bui-select-arrow-down{border-width:4px 3px 0; border-color:#999 transparent transparent}
     *   .bui-select-list{background:#212121}（bui-dark）项 20px、hover 底色 rgba(1,1,1,.1)
     *   .bpx-player-ctrl-subtitle-color .bui-select-result>span:first-child
     *     {12x12、圆角 2、margin-right:8px}（颜色那一列前面的色块）
     * ======================================================================= */
    component SubSelect: Item {
        id: sel

        property string title: ""
        property var options: []
        property int currentIndex: 0
        /* 一个面板里同时只开一个列表：用面板上的 openSelect 记 id */
        property int selectId: 0

        signal picked(int index)

        /* 标题 16 + 间距 5 + 下拉框 22 = 43 */
        height: 43

        readonly property bool open: panel.openSelect === sel.selectId
        readonly property var current: (currentIndex >= 0 && currentIndex < options.length)
                                       ? options[currentIndex] : null
        readonly property bool hasSwatch: current !== null && current.color !== undefined
        readonly property bool headerHovered: panel.pointerOn(selHeader)

        /* 展开时整行抬到后面的兄弟之上（z 只在同一父项内比较，所以加在行上） */
        z: open ? 30 : 0

        Text {
            id: selTitle

            width: parent.width
            height: 16
            verticalAlignment: Text.AlignVCenter
            text: sel.title
            font.pixelSize: 12
            color: panel.textColor
        }

        Rectangle {
            id: selBox

            anchors {
                left: parent.left
                right: parent.right
                top: selTitle.bottom
                topMargin: 5
            }
            height: 22
            radius: 2
            color: "transparent"
            border.width: 1
            border.color: Qt.rgba(1, 1, 1, 0.2)
        }

        /* 头部：hover 或展开时底色 #212121（.bui-select.bui-dark .bui-select-header:hover） */
        Rectangle {
            id: selHeader

            anchors.fill: selBox
            anchors.margins: 1
            color: (sel.headerHovered || sel.open) ? panel.selectListBg : "transparent"

            /* 颜色列前面的色块（12x12、圆角 2、右边距 8） */
            Rectangle {
                id: selSwatch

                visible: sel.hasSwatch
                x: 7
                anchors.verticalCenter: parent.verticalCenter
                width: 12
                height: 12
                radius: 2
                color: sel.hasSwatch ? sel.current.color : "transparent"
            }

            Text {
                anchors {
                    left: parent.left
                    leftMargin: sel.hasSwatch ? 7 + 12 + 8 : 7
                    right: selArrow.left
                    verticalCenter: parent.verticalCenter
                }
                elide: Text.ElideRight
                text: sel.current !== null ? sel.current.label : ""
                font.pixelSize: 12
                color: panel.textColor
            }

            /* 箭头区：宽 22、贴右、居中；三角形是 CSS 那个 border-width:4px 3px 0 的三角
               （宽 6、高 4、颜色 #999），这里用一个同形状的小 svg 画（assets/images/select-arrow.svg） */
            Item {
                id: selArrow

                anchors {
                    right: parent.right
                    top: parent.top
                    bottom: parent.bottom
                }
                width: 22

                Image {
                    anchors.centerIn: parent
                    width: 6
                    height: 4
                    source: "assets/images/select-arrow.svg"
                    sourceSize: Qt.size(6, 4)
                    smooth: true
                }
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: panel.openSelect = sel.open ? -1 : sel.selectId
            }
        }

        /*
         * 展开的列表：浮在下面的设置项之上（参考里 z-index:2），
         * 高度按"到面板底边留 6px"夹一下，放不下就在里面滚动。
         */
        Rectangle {
            id: selListBox

            anchors {
                left: parent.left
                right: parent.right
                top: selBox.bottom
            }
            readonly property real availableHeight:
                Math.max(40, panel.height - panel.mapFromItem(sel, 0, sel.height).y - 6)
            readonly property real contentHeight: sel.options.length * 20

            height: sel.open ? Math.min(contentHeight, availableHeight) : 0
            visible: height > 0
            color: panel.selectListBg
            radius: 2
            z: 40

            Flickable {
                id: selFlick

                anchors {
                    fill: parent
                    rightMargin: selListBox.contentHeight > height ? 8 : 0
                }
                contentHeight: selListBox.contentHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                Column {
                    width: selFlick.width

                    Repeater {
                        model: sel.options

                        Item {
                            id: selItem

                            required property var modelData
                            required property int index

                            width: parent.width
                            height: 20

                            readonly property bool hovered: panel.pointerOn(selItem)

                            /* 列表项高亮：上下各收 1px（20px 行 → 18px 高亮），
                               免得相邻两项的高亮块贴死（同语言列表那条理由） */
                            Rectangle {
                                y: 1
                                width: parent.width
                                height: parent.height - 2
                                color: selItem.hovered ? panel.hoverBg : "transparent"
                            }

                            Rectangle {
                                visible: selItem.modelData.color !== undefined
                                x: 7
                                width: 12
                                height: 12
                                radius: 2
                                anchors.verticalCenter: parent.verticalCenter
                                color: selItem.modelData.color !== undefined
                                       ? selItem.modelData.color : "transparent"
                            }

                            Text {
                                anchors {
                                    left: parent.left
                                    leftMargin: selItem.modelData.color !== undefined ? 7 + 12 + 8 : 7
                                    verticalCenter: parent.verticalCenter
                                }
                                text: selItem.modelData.label
                                font.pixelSize: 12
                                color: panel.textColor
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    sel.picked(selItem.index)
                                    panel.openSelect = -1
                                }
                            }
                        }
                    }
                }
            }

            /* 列表内的细滚动条 */
            Rectangle {
                visible: selFlick.contentHeight > selFlick.height
                width: 4
                radius: 2
                color: Qt.rgba(1, 1, 1, 0.35)
                x: selListBox.width - 6
                y: selFlick.visibleArea.yPosition * selFlick.height
                height: Math.max(16, selFlick.visibleArea.heightRatio * selFlick.height)
            }
        }
    }

    /* =======================================================================
     * 滑块（参考 .bui-slider）
     *   .bui-slider{height:12px}     .bui-track{height:2px; 居中}
     *   .bui-bar-wrap{background:#e7e7e7; border-radius:1.5px}
     *   .bui-bar{background:#00a1d6}   .bui-thumb-dot{12x12 圆形、#00a1d6}
     *   圆点位移 = 值 × (轨道宽 − 圆点宽)（参考 DOM 里 87% → translateX(186.18px)，214×0.87）
     * ======================================================================= */
    component SubSlider: Item {
        id: slider

        property real value: 0.87

        signal moved(real value)

        height: 12

        readonly property real travel: Math.max(0, width - 12)

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 2
            radius: 1.5
            color: panel.sliderTrackBg
        }

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width * Math.max(0, Math.min(1, slider.value))
            height: 2
            radius: 1.5
            color: panel.fnColor
        }

        Rectangle {
            width: 12
            height: 12
            radius: 6
            color: panel.fnColor
            anchors.verticalCenter: parent.verticalCenter
            x: slider.travel * Math.max(0, Math.min(1, slider.value))
        }

        /* 热区上下各放宽 9px（参考那个 12px 高的滑块本来就不好点） */
        MouseArea {
            anchors {
                fill: parent
                topMargin: -9
                bottomMargin: -9
            }
            cursorShape: Qt.PointingHandCursor
            onPressed: function (mouse) { slider.moved(_valueAt(mouse.x)) }
            onPositionChanged: function (mouse) {
                if (pressed)
                    slider.moved(_valueAt(mouse.x))
            }
        }

        function _valueAt(px) {
            if (slider.travel <= 0)
                return 0

            return Math.max(0, Math.min(1, (px - 6) / slider.travel))
        }
    }

    /* =======================================================================
     * 复选框（参考 .bui-checkbox.bui-dark）
     *   图标 16x16、margin-right:4px、svg fill:#fff；选中/悬停 → 主色
     *   文字 12px、#fff；悬停 → 主色
     * ======================================================================= */
    component SubCheck: Item {
        id: check

        property string text: ""
        property bool checked: false

        signal toggled()

        width: 16 + 4 + checkLabel.implicitWidth
        height: 16
        implicitWidth: width
        implicitHeight: 16

        readonly property bool hovered: panel.pointerOn(check)

        Image {
            id: checkIcon

            anchors.verticalCenter: parent.verticalCenter
            width: 16
            height: 16
            source: check.checked ? "assets/images/dm/checkbox-checked.svg"
                                  : "assets/images/dm/checkbox.svg"
            sourceSize: Qt.size(16, 16)
            smooth: true
            visible: false
        }

        MultiEffect {
            anchors.fill: checkIcon
            source: checkIcon
            colorization: 1
            colorizationColor: (check.checked || check.hovered) ? panel.fnColor : "#ffffff"
        }

        Text {
            id: checkLabel

            anchors {
                left: checkIcon.right
                leftMargin: 4
                verticalCenter: parent.verticalCenter
            }
            text: check.text
            font.pixelSize: 12
            color: check.hovered ? panel.fnColor : panel.textColor
            Behavior on color {
                ColorAnimation { duration: 200 }
            }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: check.toggled()
        }
    }
}
