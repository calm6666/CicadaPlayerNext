// ===========================================================================
// 应用设置页（遮罩 + 居中面板）
//
// 【从 Main.qml 抽出来的】原来这一整套（两个内联组件 SettingSwitch / SettingChoices +
// 遮罩 settingsScrim + 面板 settingsPanel + 设置项列 settingsColumn）直接写在 Main.qml 里，
// 于是只有**播放器窗口**能用；首页（HomeWindow.qml）那个"设置"按钮点了没有反应。
// 现在它独立成一个文件，两个窗口**共用同一份**：
//   * Main.qml（播放器窗口）：齿轮 / 顶栏的"?" → open()，player 传进去，所有行都可交互；
//   * HomeWindow.qml（视频库）：工具条的"设置" → open()，**没有播放器**（player 为 null），
//     与播放器有关的那几行自动禁用（enabled 取自 hasPlayer），只留「界面主题」可用 ——
//     首页本来就只想让用户在这儿切深浅色。
//
// 【不要删】用户强调过：Main.qml 里那份设置页要保留 —— 说的是这个功能不能消失。
// 现在 Main.qml 里留的是一个 `SettingsPage {}` 实例（见那边注释），设置页照样从齿轮打得开，
// 只是实现搬到了本文件，避免两份拷贝各自漂移。
//
// 内容来源（照抄，不自己发明）：
//   * 两个手写控件（开关 / 一排多选一）原来是 Main.qml 的内联组件，原样搬来；
//   * 面板布局、尺寸、配色是原来就调好的，数值一个没动（主题色全走 QtPlayerTheme 单例）。
//
// 【控件全部手写，不用 QtQuick.Controls】理由和 SettingsPanel.qml 一样：参考里开关/选项
// 都是纯 div，套原生控件反而要把 indicator/contentItem/background 全部推倒重写。
//
// 对外接口（就这五个，改本文件前先对一遍，别再删掉别人在用的属性 —— 踩过一次：
// SettingsPanel 删掉 viewHeight 之后，RightControls 读到 undefined，浮层高度变 0）：
//   property var  player           —— 播放器组件（CicadaPlayerItem）；首页传 null
//   readonly bool hasPlayer        —— player 非空；与播放器有关的行用它做 enabled
//   property bool showDiagnostics  —— 左上角诊断信息开关（播放器窗口别名出去用）
//   readonly bool opened           —— 现在是不是开着（外面据此判"要不要自动收起控制栏"）
//   open() / close()
// ===========================================================================
import QtQuick

Item {
    id: page

    /*
     * 【z 必须给在**根对象**上，不能只给里面那个遮罩】
     *
     * 原来遮罩是直接写在 Main.qml 里的，`z: 10` 意味着"比标题栏(2)、控制栏、诊断文字都高"。
     * 现在遮罩是**本组件的子对象**，而 Qt Quick 的 z 只在**同一父对象的兄弟之间**排序：
     * 子对象 z 再大，整棵子树还是按根对象自己的 z 插在宿主窗口的孩子列表里。也就是说，
     * 只在里面写 z:10 的话，设置页会被宿主的标题栏（Main.qml 里 z:4）和左上角诊断文字
     * （z:0 但声明在后面）**盖住** —— 遮罩上浮着一层诊断文字。
     * 所以整页的 z 给在这里（20 > 宿主用到的一切）。
     */
    z: 20

    /* -----------------------------------------------------------------------
     * 对外接口
     * ----------------------------------------------------------------------- */

    /*
     * 播放器组件。**首页那个实例传 null**（那里没有播放器），
     * 所以下面每一行与播放器有关的绑定都必须先判 hasPlayer —— 否则
     * `player.hardwareDecoding` 会报 TypeError 并且随绑定刷新反复刷屏（main.cpp 把
     * Qt 的告警接进了日志，一条条都会打出来）。
     */
    property var player: null

    readonly property bool hasPlayer: player !== null

    /*
     * 左上角那几行诊断信息要不要显示。**状态住在这里**（原来住在 Main.qml 的根对象上），
     * Main.qml 用 `property alias showDiagnostics: settingsPage.showDiagnostics` 别名出去，
     * 那边的诊断 Column 照旧 `visible: root.showDiagnostics`。
     */
    property bool showDiagnostics: true

    /* 现在开着没有。外面的 `overlayOpen`（控制栏不自动收起 / 不藏鼠标指针）读它。 */
    readonly property alias opened: scrim.visible

    /*
     * 打开 / 关闭。
     *
     * 【为什么是函数而不是让外面直接写 opened】opened 是 alias（只读），
     * 而且外面关的时候往往还要顺手做别的事（Main.qml 会调 kickAutoHide() 让控制栏复活），
     * 统一走这两个函数，调用方就不用各自记"还要补哪一句"。
     */
    function open() {
        scrim.visible = true
    }

    function close() {
        scrim.visible = false
    }

    /*
     * 一行"开关"：左边标题，右边一个滑动小开关。
     *
     * 用 inline component（QML 5.15 起支持，写在本文档里、全文档可用）是为了不把
     * 同一个开关的样式抄七遍；用法就是下面 SettingSwitch { ... } 那几行。
     *
     * opacity 跟着 enabled 走：首页那个实例没有播放器，与播放器有关的行 `enabled: false`
     * （Item 的 enabled 会**继承**给子对象，里面的 MouseArea 自动失效），
     * 但光靠 enabled 看不出来，所以这里再补一个变淡 —— 只加一条绑定，没有新增任何控件。
     */
    component SettingSwitch: Item {
        id: switchRow

        property string title
        property bool checked
        signal toggled()

        width: parent.width
        implicitHeight: 34
        opacity: enabled ? 1 : 0.45

        Text {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            color: QtPlayerTheme.panelText
            font.pixelSize: 14
            text: switchRow.title
        }

        Rectangle {
            id: switchTrack
            width: 46
            height: 24
            radius: 12
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            color: switchRow.checked ? QtPlayerTheme.accent : QtPlayerTheme.switchOffBg

            Behavior on color {
                ColorAnimation { duration: 120 }
            }

            /* 滑块 */
            Rectangle {
                width: 18
                height: 18
                radius: 9
                color: QtPlayerTheme.switchKnob
                anchors.verticalCenter: parent.verticalCenter
                x: switchRow.checked ? parent.width - width - 3 : 3

                Behavior on x {
                    NumberAnimation { duration: 120 }
                }
            }

            MouseArea {
                anchors.fill: parent
                onClicked: switchRow.toggled()
            }
        }
    }

    /*
     * 一行"多选一"：标题 + 一排按钮，当前值高亮。
     * values 是显示文本，keys 是对应的值（两者一一对应）。
     */
    component SettingChoices: Item {
        id: choiceRow

        property string title
        property var values: []
        property var keys: []
        property var current
        /* 每颗按钮的宽度：默认够放 "0.5x" / "90°" 这种；文字长了（如「跟随系统」）自己加宽 */
        property int buttonWidth: 46
        signal picked(var value)

        width: parent.width
        implicitHeight: 34
        opacity: enabled ? 1 : 0.45

        Text {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            color: QtPlayerTheme.panelText
            font.pixelSize: 14
            text: choiceRow.title
        }

        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6

            /*
             * model 用整数（值的个数）：delegate 里就能用 required property int index
             * 直接取下标，不必依赖 modelData（Qt 6 里 delegate 的隐式 index 已经不推荐）。
             */
            Repeater {
                model: choiceRow.keys.length

                Rectangle {
                    required property int index

                    readonly property bool active: choiceRow.keys[index] === choiceRow.current

                    width: choiceRow.buttonWidth
                    height: 26
                    radius: 4
                    color: active ? QtPlayerTheme.choiceActiveBg : QtPlayerTheme.choiceBg

                    Text {
                        anchors.centerIn: parent
                        color: QtPlayerTheme.controlText
                        font.pixelSize: 12
                        text: choiceRow.values[index]
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: choiceRow.picked(choiceRow.keys[index])
                    }
                }
            }
        }
    }

    /* -----------------------------------------------------------------------
     * 遮罩 + 面板
     *
     * 层级见根对象上那个 z:20 的说明；宿主只要把自己的实例铺满窗口即可。
     * ----------------------------------------------------------------------- */

    /*
     * 遮罩。点面板以外的地方就关掉 —— 和抽出来之前的行为完全一致。
     *
     * 【注意这块 MouseArea 没有 hoverEnabled】所以它不吃 hover：
     * Main.qml 顶部那条"鼠标进顶部就显示标题栏"的被动 HoverHandler 在设置页开着时
     * 照样收得到（不过那边另有 overlayOpen 判据，开着浮层时本来就不露头）。
     */
    Rectangle {
        id: scrim
        anchors.fill: parent
        color: QtPlayerTheme.scrim
        visible: false

        MouseArea {
            anchors.fill: parent
            onClicked: page.close()
        }

        Rectangle {
            id: settingsPanel
            width: Math.min(460, parent.width - 40)
            /*
             * 高度自己算：标题 16 上边距 + 标题 + 16 间距 + 内容 + 16 下边距。
             * Rectangle 不会根据子元素自动定高，而 Column 的 implicitHeight 是有的。
             */
            height: settingsTitle.height + settingsColumn.implicitHeight + 48
            anchors.centerIn: parent
            radius: 10
            color: QtPlayerTheme.panelBg
            border.width: 1
            border.color: QtPlayerTheme.panelBorder

            /* 吃掉落在面板上的点击，免得冒泡到遮罩把设置页关了。 */
            MouseArea {
                anchors.fill: parent
            }

            Text {
                id: settingsTitle
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 16
                color: QtPlayerTheme.panelText
                font.pixelSize: 16
                text: qsTr("设置")
            }

            Rectangle {
                id: settingsClose
                width: 28
                height: 28
                radius: 6
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 12
                color: closeMouse.pressed ? QtPlayerTheme.buttonPressedBg : QtPlayerTheme.buttonBg

                Text {
                    anchors.centerIn: parent
                    color: QtPlayerTheme.controlText
                    font.pixelSize: 16
                    text: "×"
                }

                MouseArea {
                    id: closeMouse
                    anchors.fill: parent
                    onClicked: page.close()
                }
            }

            Column {
                id: settingsColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: settingsTitle.bottom
                anchors.margins: 16
                spacing: 12

                /*
                 * 界面主题：跟随系统 / 浅色 / 深色。
                 *
                 * 和标题栏那颗太阳/月亮按钮是**同一个状态**（都是 QtPlayerTheme.mode），
                 * 所以两边会互相联动。默认 "system" —— 跟随系统深浅（见 QtPlayerTheme）。
                 * 选项只有三个，按钮宽度 46 够放两个字。
                 *
                 * 【这一行永远可交互】它改的是全局单例，和有没有播放器无关
                 * —— 首页那个实例就是靠它切深浅色的。
                 */
                SettingChoices {
                    title: qsTr("界面主题")
                    values: [qsTr("跟随系统"), qsTr("浅色"), qsTr("深色")]
                    keys: QtPlayerTheme.modeKeys
                    current: QtPlayerTheme.mode
                    /* 「跟随系统」四个字，46px 放不下，加宽一点 */
                    buttonWidth: 64
                    onPicked: (value) => QtPlayerTheme.mode = value
                }

                /*
                 * 硬解开关。改它会**重新打开当前视频并跳回原位置**（解码器是在
                 * Prepare 的时候建的，改完必须重来一遍），见 CicadaPlayerItem。
                 *
                 * 【下面这几行的写法】 `page.hasPlayer ? page.player.xxx : false`：
                 * 首页那个实例 player 是 null，直接读 player.xxx 会 TypeError。
                 * 交互出口（onToggled）同样要先判一次，不能只靠 checked 的显示值。
                 */
                SettingSwitch {
                    title: qsTr("硬件解码（硬解）")
                    enabled: page.hasPlayer
                    checked: page.hasPlayer ? page.player.hardwareDecoding : false
                    onToggled: if (page.hasPlayer) page.player.hardwareDecoding = !page.player.hardwareDecoding
                }

                SettingSwitch {
                    title: qsTr("循环播放")
                    enabled: page.hasPlayer
                    checked: page.hasPlayer ? page.player.loop : false
                    onToggled: if (page.hasPlayer) page.player.loop = !page.player.loop
                }

                SettingSwitch {
                    title: qsTr("静音")
                    enabled: page.hasPlayer
                    checked: page.hasPlayer ? page.player.muted : false
                    onToggled: if (page.hasPlayer) page.player.muted = !page.player.muted
                }

                SettingChoices {
                    title: qsTr("播放倍速")
                    values: ["0.5x", "1x", "1.5x", "2x"]
                    keys: [0.5, 1.0, 1.5, 2.0]
                    enabled: page.hasPlayer
                    current: page.hasPlayer ? page.player.playbackRate : 1.0
                    onPicked: (value) => { if (page.hasPlayer) page.player.playbackRate = value }
                }

                SettingSwitch {
                    title: qsTr("水平镜像")
                    enabled: page.hasPlayer
                    checked: page.hasPlayer ? page.player.mirror : false
                    onToggled: if (page.hasPlayer) page.player.mirror = !page.player.mirror
                }

                SettingChoices {
                    title: qsTr("画面旋转")
                    values: ["0°", "90°", "180°", "270°"]
                    keys: [0, 90, 180, 270]
                    enabled: page.hasPlayer
                    current: page.hasPlayer ? page.player.extraRotation : 0
                    onPicked: (value) => { if (page.hasPlayer) page.player.extraRotation = value }
                }

                /*
                 * 诊断信息开关：它属于**播放器窗口**（首页没有那几行文字），但组件是共用的，
                 * 所以首页那个实例上它也能点、只是没人读 —— 这不影响什么，比"凭空少一行"更整齐。
                 */
                SettingSwitch {
                    title: qsTr("显示诊断信息")
                    checked: page.showDiagnostics
                    onToggled: page.showDiagnostics = !page.showDiagnostics
                }

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    color: QtPlayerTheme.panelHintText
                    font.pixelSize: 11
                    text: qsTr("硬解开关会重新打开当前视频并跳回原来的位置。"
                               + "这些设置只作用于本次运行，没有落盘保存。")
                }
            }
        }
    }
}
