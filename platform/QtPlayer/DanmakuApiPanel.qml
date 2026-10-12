// ===========================================================================
// 弹幕 API 面板 —— 右栏「弹幕」Tab 的内容
//
// 直接调 DanmakuController（DanmakuView.danmakuEngine）的 Q_INVOKABLE 和属性：
//   * send(text, {type})   —— 发一条弹幕（self = true → 白框，可撤回；C++ 侧记进
//                             allItems 的自发记账，id 由控制器分配负 id）
//   * recallDanmaku(id)    —— 撤回自己发的那条（引擎按 id 找，记账同步移除）
//   * allItems             —— 【弹幕列表】全量弹幕（不只 in 屏）：引擎调度数据 +
//                             自己发的记账合并、按 timeMs 升序，NOTIFY allItemsChanged
//                             只在载入/发送/撤回/清空时发 —— 列表天然稳定，无需采样
//   * stats                —— activeCount / admitted / dropped / itemCount 统计
// ===========================================================================
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtPlayer

Rectangle {
    id: danmakuPanel

    /* 弹幕视图（ApiPanel 转进来；引擎经 danmakuView.danmakuEngine 拿） */
    property var danmakuView: null

    readonly property bool hasEngine: danmakuView !== null && danmakuView !== undefined
                                      && danmakuView.danmakuEngine !== undefined

    /* 发送类型（scroll / top / bottom —— send options 的 type 口径） */
    property string sendType: "scroll"

    /* 【弹幕列表】全量弹幕（不只 in 屏）：直接绑引擎的 allItems 属性
       （C++ 侧合并了调度数据 + 自己发的记账，按 timeMs 升序、变化自动通知） */
    property var onscreenItems: []

    /* 底部一行操作日志 */
    property string logText: ""

    /* 引擎引用（hasEngine 为真才有效；绑定式，视图换了自动跟上） */
    readonly property var engine: hasEngine ? danmakuView.danmakuEngine : null

    /*
     * 【浅色主题适配】本面板原先整套是"深色面板 + 白字"的写死配色：菜单主题的
     * menuText 是白色、行底/边框全是白系半透明 —— 浅色主题下 panelBg 是白色，
     * 白字白底整页看不见（用户实测弹幕列表一片空白）。文字改走主题 token
     * （panelText/panelHintText 深浅自适应），行底/细边框给两个本面板的衍生色：
     */
    readonly property color rowWash: QtPlayerTheme.dark ? "#0FFFFFFF" : "#0F000000"    /* 列表行底 */
    readonly property color hairline: QtPlayerTheme.dark ? "#26FFFFFF" : "#26000000"   /* 细边框 */
    readonly property color fieldWash: QtPlayerTheme.dark ? "#1FFFFFFF" : "#1F000000"  /* 输入框/按钮底 */

    /* ------------------------------------------------------------------ */
    /* 发送（照 hili-player 弹幕插件的 send：文本 + 类型 + self） */
    /* ------------------------------------------------------------------ */

    function sendDanmaku() {
        if (!hasEngine)
            return

        var text = inputField.text.trim()

        if (text === "") {
            logText = qsTr("内容为空，忽略")
            return
        }

        /* self = true：自己发的（白框 + 可撤回 —— 和右键菜单里那条撤回同一套） */
        var accepted = engine.send(text, { type: danmakuPanel.sendType, self: true })

        if (accepted) {
            logText = qsTr("已发送「%1」（%2）").arg(text).arg(danmakuPanel.sendType)
            inputField.text = ""
        } else {
            logText = qsTr("引擎没有接受这条弹幕（被屏蔽规则挡了 / 同屏到顶 / 类型非法）")
        }
    }

    /* ------------------------------------------------------------------
     * 弹幕文字的显示色（B 站列表排式：每条弹幕按**自身颜色**渲染文字）
     *
     * 【为什么之前深色下"颜色文字看不到"】列表所有文字统一用主题的 panelText
     * （深色=纯白）—— 弹幕的白/红/蓝/绿各色信息完全丢失，一整列全是同一个白，
     * 用户看起来就是"弹幕本色的文字看不到"。
     *
     * 合理设计（深浅两主题都成立）：
     *   * 深色主题：直接用弹幕本色 —— 弹幕本来就是渲染在黑色画面上的，颜色
     *     天生适配深底，红是红、蓝是蓝；
     *   * 浅色主题：彩色弹幕保持本色，但**太亮的弹幕**（白色弹幕这类，
     *     亮度 > 0.8）在白底上会消失 → 映射成 panelText 深字
     *     （B 站浅色网页也是这么处理白弹幕的）；
     *   * 自己发的：仍走 warnText 警示黄 + "我"徽标，和别条一眼区分。
     *
     * hex 形态是 "#rrggbbaa"（DanmakuController 的 danmakuColorToHex），
     * 取前三位分量算感知亮度（0.299/0.587/0.114 加权）。
     * ------------------------------------------------------------------ */
    function danmakuTextColor(entry) {
        if (entry.isSelf === true)
            return QtPlayerTheme.warnText

        var hex = entry.color !== undefined ? String(entry.color) : "#ffffffff"

        if (!QtPlayerTheme.dark) {
            var r = parseInt(hex.substring(1, 3), 16) / 255
            var g = parseInt(hex.substring(3, 5), 16) / 255
            var b = parseInt(hex.substring(5, 7), 16) / 255
            var lum = 0.299 * r + 0.587 * g + 0.114 * b

            if (lum > 0.8)
                return QtPlayerTheme.panelText
        }

        return hex
    }

    /* ------------------------------------------------------------------ */
    /* 布局：发送区 → 统计行 → 在屏列表 → 日志行 */
    /* ------------------------------------------------------------------ */

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 10

        /* ---- 发送区：输入框 + 类型三选一 + 发送按钮 ---- */
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                TextField {
                    id: inputField

                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    placeholderText: qsTr("发一条弹幕试试（Enter 发送）")
                    color: QtPlayerTheme.panelText
                    font.pixelSize: 12
                    selectByMouse: true

                    background: Rectangle {
                        radius: 4
                        color: inputField.focus
                               ? (QtPlayerTheme.dark ? "#33FFFFFF" : "#33000000")
                               : danmakuPanel.fieldWash
                        border.width: 1
                        border.color: inputField.focus ? QtPlayerTheme.playerAccent : "transparent"
                    }

                    onAccepted: danmakuPanel.sendDanmaku()
                }

                Button {
                    Layout.preferredHeight: 30
                    Layout.preferredWidth: 64
                    text: qsTr("发送")
                    font.pixelSize: 12

                    background: Rectangle {
                        radius: 4
                        color: parent.hovered
                               ? Qt.alpha(QtPlayerTheme.playerAccent, 0.9)
                               : QtPlayerTheme.playerAccent
                    }

                    contentItem: Text {
                        text: parent.text
                        font.pixelSize: 12
                        color: "#ffffff"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    onClicked: danmakuPanel.sendDanmaku()
                }
            }

            /* 类型选择（滚动 / 顶部 / 底部 —— send options.type 的三个常用值） */
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Text {
                    text: qsTr("类型：")
                    color: QtPlayerTheme.panelHintText
                    font.pixelSize: 11
                }

                Repeater {
                    model: [
                        { key: "scroll", name: qsTr("滚动") },
                        { key: "top", name: qsTr("顶部") },
                        { key: "bottom", name: qsTr("底部") }
                    ]

                    delegate: Button {
                        id: typeButton

                        required property var modelData

                        Layout.preferredHeight: 24
                        Layout.preferredWidth: 52
                        flat: true

                        readonly property bool active: danmakuPanel.sendType === typeButton.modelData.key

                        background: Rectangle {
                            radius: 4
                            color: typeButton.active
                                   ? Qt.alpha(QtPlayerTheme.playerAccent, 0.45)
                                   : (typeButton.hovered
                                      ? (QtPlayerTheme.dark ? "#33FFFFFF" : "#33000000")
                                      : danmakuPanel.fieldWash)
                        }

                        contentItem: Text {
                            text: typeButton.modelData.name
                            font.pixelSize: 11
                            color: typeButton.active ? "#ffffff" : QtPlayerTheme.panelText
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        onClicked: danmakuPanel.sendType = typeButton.modelData.key
                    }
                }

                Item { Layout.fillWidth: true }
            }
        }

        /* ---- 统计行（engine.stats：activeCount / admitted / dropped / itemCount） ---- */
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            radius: 4
            color: QtPlayerTheme.dark ? "#14FFFFFF" : "#14000000"

            Text {
                anchors.centerIn: parent
                text: danmakuPanel.hasEngine
                      ? qsTr("在屏 %1 · 累计准入 %2 · 丢弃 %3 · 总数 %4")
                        .arg(danmakuPanel.engine.stats.activeCount)
                        .arg(danmakuPanel.engine.stats.admitted)
                        .arg(danmakuPanel.engine.stats.dropped)
                        .arg(danmakuPanel.engine.stats.itemCount)
                      : qsTr("播放器还没挂弹幕层")
                color: QtPlayerTheme.panelHintText
                font.pixelSize: 11
            }
        }

        /* ---- 在屏列表（500ms 采样；GPU 渲染模式下也照常采样 ——
              ApiPanel 置了 danmakuView.apiListNeeded，引擎会建这张表） ---- */
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: 6
            color: QtPlayerTheme.dark ? "#14000000" : "#0A000000"
            border.width: 1
            border.color: danmakuPanel.hairline

            ListView {
                id: onscreenList

                anchors.fill: parent
                anchors.margins: 8
                clip: true
                model: danmakuPanel.onscreenItems
                spacing: 4

                /* 【桌面端滚动】禁掉内容拖拽（interactive: false）：这是桌面应用，
                   列表滚动只走**滚轮 + 拖滚动条**（ScrollBar 在下面）—— 按住内容
                   甩动是移动端触屏的手势，鼠标场景下"按住弹幕行拖一下整列跟着跑"
                   反而误触（用户要求去掉）。Flickable 的滚轮滚动不依赖 interactive，
                   照常可用。 */
                interactive: false

                ScrollBar.vertical: ScrollBar {}

                /* 行：文案 + 类型徽标 + （自己发的）撤回按钮 */
                delegate: Rectangle {
                    id: onscreenRow

                    required property int index
                    required property var modelData

                    width: onscreenList.width
                    height: 28
                    radius: 4
                    color: danmakuPanel.rowWash

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 4
                        spacing: 8

                        /*
                         * 时间点前缀（B 站弹幕列表排式）：[mm:ss]。
                         * timeMs 是毫秒（引擎口径），面板侧格式化。
                         */
                        Text {
                            Layout.preferredWidth: 42
                            text: {
                                var total = Math.max(0, Math.floor(
                                    (onscreenRow.modelData.timeMs !== undefined
                                     ? onscreenRow.modelData.timeMs : 0) / 1000))
                                var mm = Math.floor(total / 60)
                                var ss = total % 60
                                return "[" + (mm < 10 ? "0" : "") + mm + ":"
                                       + (ss < 10 ? "0" : "") + ss + "]"
                            }
                            color: QtPlayerTheme.panelHintText
                            font.pixelSize: 11
                        }

                        /*
                         * 顶部/底部弹幕的类型徽标（B 站列表里"顶/底"小标签；
                         * 滚动弹幕是主流、不占地方 —— 不显示）。底色用主题色
                         * 28% 透明、字走 panelText，深浅主题下都可读。
                         */
                        Rectangle {
                            visible: onscreenRow.modelData.type === "top"
                                     || onscreenRow.modelData.type === "bottom"
                            Layout.preferredWidth: 18
                            Layout.preferredHeight: 16
                            radius: 3
                            color: Qt.alpha(QtPlayerTheme.playerAccent, 0.28)

                            Text {
                                anchors.centerIn: parent
                                text: onscreenRow.modelData.type === "top"
                                      ? qsTr("顶") : qsTr("底")
                                color: QtPlayerTheme.panelText
                                font.pixelSize: 10
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: onscreenRow.modelData.text
                            /* 弹幕本色（见 danmakuTextColor：深色直接本色、
                               浅色下太亮的映射深字、自己发的警示黄） */
                            color: danmakuPanel.danmakuTextColor(onscreenRow.modelData)
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }

                        /* 自己发的标签（B 站列表里自己发的那条带"我"的标记） */
                        Rectangle {
                            visible: onscreenRow.modelData.isSelf === true
                            Layout.preferredWidth: 18
                            Layout.preferredHeight: 16
                            radius: 3
                            color: Qt.alpha(QtPlayerTheme.playerAccent, 0.35)

                            Text {
                                anchors.centerIn: parent
                                text: qsTr("我")
                                color: "#ffffff"
                                font.pixelSize: 10
                            }
                        }

                        /* 撤回（只有自己发的行给 —— 引擎按 id 找，别人发的撤不掉） */
                        Button {
                            visible: onscreenRow.modelData.isSelf === true
                            Layout.preferredWidth: 44
                            Layout.preferredHeight: 22
                            flat: true
                            text: qsTr("撤回")
                            font.pixelSize: 11

                            background: Rectangle {
                                radius: 4
                                color: parent.hovered ? "#66FF5555" : "#3FFF5555"
                            }

                            contentItem: Text {
                                text: parent.text
                                font.pixelSize: 11
                                color: "#ffffff"
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }

                            onClicked: {
                                if (danmakuPanel.hasEngine) {
                                    danmakuPanel.engine.recallDanmaku(onscreenRow.modelData.id)
                                    danmakuPanel.logText = qsTr("已请求撤回 id %1").arg(onscreenRow.modelData.id)
                                }
                            }
                        }
                    }
                }

                Text {
                    anchors.centerIn: parent
                    visible: danmakuPanel.onscreenItems.length === 0
                    text: qsTr("弹幕列表为空（等 mock 拉取 / 文件载入，或发一条）")
                    color: QtPlayerTheme.panelHintText
                    font.pixelSize: 12
                    horizontalAlignment: Text.AlignHCenter
                }
            }
        }

        /* ---- 日志行 ---- */
        Text {
            Layout.fillWidth: true
            text: danmakuPanel.logText
            color: QtPlayerTheme.panelHintText
            font.pixelSize: 11
            elide: Text.ElideMiddle
        }
    }

    /* 全量列表直接绑引擎属性（NOTIFY allItemsChanged 驱动；Timer 采样不再需要） */
    Binding {
        target: danmakuPanel
        property: "onscreenItems"
        value: danmakuPanel.hasEngine ? danmakuPanel.engine.allItems : []
    }
}
