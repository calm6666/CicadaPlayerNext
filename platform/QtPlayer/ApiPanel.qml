// ===========================================================================
// API 操作面板 —— 播放器窗口右栏的容器（常驻显示，由 Main.qml 摆放）
//
// 【为什么是"左右两栏"】用户要求：播放器窗口左边是播放器、右边是各种 API 的
// 操作（弹幕列表、交互卡片的编辑/添加/修改/更新等）。右栏**常驻**（不再依赖
// 首页的模式开关），全屏或点击播放器右缘的折叠箭头时收起。
// 交互卡片的「编辑 / 展示」模式在本面板顶部切换（写全局单例
// QtPlayerAppMode.interactionMode，和首页的开关同一个值，两边永远同步）。
//
// 本文件只做容器：顶部两个 Tab（交互卡片 / 弹幕）+ 内容区二选一显示。
// 具体功能在两个子面板里：
//   * InteractionApiPanel —— 交互卡片的增删改查（照 hili-player demo 的编辑面板）
//   * DanmakuApiPanel     —— 弹幕发送 / 撤回 / 在屏列表 / 统计（engine 的 Q_INVOKABLE）
//
// 两个子面板要的依赖（interactionLayer / danmakuView）由 Main.qml 喂进来，
// 本容器原样往下转 —— 和 PlayerView 的 episodes 转交是同一个路子。
// ===========================================================================
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtPlayer

Rectangle {
    id: panel

    /* 交互卡片层（InteractionLayer.qml 的实例，经 PlayerView.interactionLayer 转出） */
    property var interactionLayer: null

    /* 弹幕视图（DanmakuView.qml 的实例，经 PlayerView.danmaku 转出） */
    property var danmakuView: null

    /* 【背景跟主题】之前用的是 menuBg（固定深黑，不跟主题）—— 那是压在视频
       上的浮层菜单的底色；右栏是独立侧栏，浅色主题下黑底配深字根本看不见。
       panelBg 深色 #1e1e26 / 浅色 #ffffff，配合 panelText/panelHintText
       两套都成立（黑主题下深底 + 白字，用户要求）。 */
    color: QtPlayerTheme.panelBg

    /*
     * 【在屏列表的需求开关】弹幕 Tab 可见（面板自己也可见）时置 true：
     * GPU 渲染模式下引擎默认不建 activeItems 表（省 CPU），但右栏的在屏
     * 列表必须能看 —— 置 true 后 DanmakuView 的 gpuLayer 绑定退回 false，
     * 控制器恢复建表（渲染仍走 GPU 不受影响）。面板藏起来 / 切走 Tab 就还回去。
     */
    function syncListNeed() {
        if (danmakuView !== null && danmakuView !== undefined)
            danmakuView.apiListNeeded = visible && tabIndex === 1
    }

    onVisibleChanged: syncListNeed()
    onTabIndexChanged: syncListNeed()
    onDanmakuViewChanged: syncListNeed()

    /* 顶部 Tab 栏 + 内容区（上下摆；Tab 高 40，内容区填剩余） */
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        /* ---- Tab 栏（两颗按钮，选中态主色描边） ---- */
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            color: QtPlayerTheme.dark ? "#1AFFFFFF" : "#14000000"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 8

                Repeater {
                    model: [qsTr("交互卡片"), qsTr("弹幕")]

                    delegate: Button {
                        id: tabButton

                        required property int index
                        required property var modelData

                        Layout.preferredHeight: 28
                        flat: true
                        text: tabButton.modelData
                        font.pixelSize: 13
                        checked: panel.tabIndex === tabButton.index
                        /* 选中态：主色文字 + 底部 2px 主色条（描边容易糊，实条更清楚） */
                        contentItem: Text {
                            text: tabButton.text
                            font: tabButton.font
                            color: panel.tabIndex === tabButton.index
                                   ? QtPlayerTheme.playerAccent : QtPlayerTheme.panelText
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            color: "transparent"
                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                height: 2
                                visible: panel.tabIndex === tabButton.index
                                color: QtPlayerTheme.playerAccent
                            }
                        }
                        onClicked: panel.tabIndex = tabButton.index
                    }
                }

                Item { Layout.fillWidth: true }

                /*
                 * 交互卡片的「编辑 / 展示」切换（用户要求放在右栏里，不再只靠首页）：
                 * 写全局单例 QtPlayerAppMode.interactionMode —— InteractionLayer 的
                 * editMode 读同一个值，卡片立即进入/退出可拖拽的编辑态；首页那个
                 * 开关写的也是它，两边永远同步。写法和下面弹幕面板的类型按钮一致。
                 */
                RowLayout {
                    Layout.alignment: Qt.AlignRight
                    spacing: 4

                    Repeater {
                        model: [
                            { key: "edit", name: qsTr("编辑") },
                            { key: "interactive", name: qsTr("展示") }
                        ]

                        delegate: Button {
                            id: modeButton

                            required property var modelData

                            Layout.preferredHeight: 22
                            Layout.preferredWidth: 42
                            flat: true
                            readonly property bool active: QtPlayerAppMode.interactionMode === modeButton.modelData.key

                            background: Rectangle {
                                radius: 4
                                color: modeButton.active
                                       ? Qt.alpha(QtPlayerTheme.playerAccent, 0.45)
                                       : (modeButton.hovered
                                          ? (QtPlayerTheme.dark ? "#33FFFFFF" : "#33000000")
                                          : (QtPlayerTheme.dark ? "#1FFFFFFF" : "#1F000000"))
                            }

                            contentItem: Text {
                                text: modeButton.modelData.name
                                font.pixelSize: 11
                                color: modeButton.active ? "#ffffff" : QtPlayerTheme.panelHintText
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }

                            onClicked: QtPlayerAppMode.interactionMode = modeButton.modelData.key
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: QtPlayerTheme.dark ? "#33FFFFFF" : "#33000000"
        }

        /* ---- 内容区（两个子面板按 Tab 二选一，都常驻 —— 切 Tab 不丢输入） ---- */
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: panel.tabIndex

            InteractionApiPanel {
                interactionLayer: panel.interactionLayer
            }

            DanmakuApiPanel {
                danmakuView: panel.danmakuView
            }
        }
    }

    /* 当前 Tab（0 = 交互卡片，1 = 弹幕） */
    property int tabIndex: 0
}
