// ===========================================================================
// LiveMain —— **直播播放器窗口**（和点播的 Main.qml 完全独立的一套）
//
// 【为什么不复用 Main.qml】
//   用户要求："直播播放器 ui 不要使用目前写好的这个哟，重新写一个"。
//   Main.qml 那套是点播窗口：顶部悬浮标题栏 + PlayerView（进度条/时间/倍速/选集/高能进度条）
//   + 侧边设置页 + 弹幕设置面板 + 加载弹幕 XML…。直播一条都用不上，混在一起只会互相拖累。
//   本文件只保留"一个桌面窗口必须有的东西"：
//     * 无边框 + QWindowKit 接管（拖拽/最大化/系统菜单用系统行为）；
//     * 顶部 36px 热区 → 浮出标题栏（和点播窗口同一套手感，用户已经熟悉）；
//     * 关窗时**清空片源**（不然窗口关了直播还在后台拉流、解码、占内存）；
//     * 快捷键集中在 LivePlayerView 里（Space/K/M/F/↑↓）。
//
// 【和 Main.qml 一样的两条硬约定】（原因在主窗口文件里写得很详细，这里只留结论）
//   1. **不写 flags: Qt.FramelessWindowHint** —— 去边框是 QWindowKit 按平台做的，
//      自己加会把 WS_THICKFRAME 去掉 → 左右下三边和四个角都不能拖拽缩放；
//   2. **transientParent: null** —— 否则窗口变成从属窗口，任务栏里没有独立按钮
//      （用户要求"开几个窗口就有几个任务栏项"）。
// ===========================================================================
import QtQuick
import QWindowKit
import QtPlayer
import CicadaPlayer

Window {
    id: liveRoot

    width: 1080
    height: 640
    visible: false
    title: qsTr("hilihili 直播播放器")

    /* 见文件头约定 2 */
    transientParent: null

    /* 直播画面永远是黑的底（和 B 站直播那个 rgba(0,0,0,.7) 的播放器盒一致，这里用全黑） */
    color: "#000000"

    /*
     * 关窗 = 停播 + 清片源。
     * Qt 的 close() 只把窗口藏起来（对象树还在），不主动清片源的话
     * 解码线程/音频线程/表面池会一直挂着 —— 表现是"窗口关了还在拉流、内存不降"。
     * 这条和点播窗口 Main.qml:124-127 是同一个理由。
     */
    onClosing: {
        liveView.player.stop()
        liveView.source = ""
    }

    /* 对外：用地址起播（HomeWindow.openLiveUrl 调进来） */
    function playUrl(url) {
        var u = ("" + url).trim()

        if (u === "") {
            return
        }

        console.log("[live] playUrl: " + u)
        liveView.source = u
    }

    /* =======================================================================
     * 直播画面 + 直播 UI
     * ======================================================================= */
    LivePlayerView {
        id: liveView

        anchors.fill: parent
        fullscreen: liveRoot.visibility === Window.FullScreen

        /* F / 双击画面：切全屏 */
        onToggleFullscreenRequested: {
            if (liveRoot.visibility === Window.FullScreen)
                liveRoot.showNormal()
            else
                liveRoot.showFullScreen()
        }

        /* 网页全屏：直播页里是"把页面其它部分藏掉、播放器铺满"。
           桌面窗口里对应的动作就是**最大化 + 隐藏标题栏**（标题栏本来就只在顶部热区浮出）。 */
        onWebFullscreenRequested: {
            if (liveRoot.visibility === Window.Maximized)
                liveRoot.showNormal()
            else
                liveRoot.showMaximized()
        }
    }

    /* =======================================================================
     * 顶部热区 + 悬浮标题栏（照点播窗口那套：进顶部 36px 才浮出、离开就收）
     * ======================================================================= */
    HoverHandler {
        id: topZone

        /* 只关心"指针在不在窗口最上面那条" —— 用 point.position 判，
           而不是再放一个 MouseArea 去抢 hover（见 docs/QML-POPUP-HOVER-PITFALLS.md） */
        readonly property bool onTop: point.position.y <= 36 && point.position.x >= 0
        onPointChanged: if (onTop) titleHideTimer.stop()
    }

    Timer {
        id: titleHideTimer
        interval: 600
        onTriggered: if (!titleBarHover.hovered) titleBar.opacity = 0
    }

    Connections {
        target: topZone

        function onOnTopChanged() {
            if (topZone.onTop) {
                titleBar.opacity = 1
                titleBar.visible = true
            } else {
                titleHideTimer.restart()
            }
        }
    }

    Rectangle {
        id: titleBar

        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
        }
        height: 36
        /* 半透明黑：压在直播画面上，全屏时永远不出现（和点播窗口一致） */
        color: Qt.rgba(0, 0, 0, 0.75)
        opacity: 0
        visible: opacity > 0
        z: 50

        Behavior on opacity {
            NumberAnimation { duration: 150 }
        }

        HoverHandler { id: titleBarHover }

        Text {
            anchors {
                left: parent.left
                leftMargin: 12
                verticalCenter: parent.verticalCenter
                /* 右边界锚到最左边那颗窗口按钮（minimizeButton）。
                   原来写的是已删掉的 Repeater 的 id `buttonsRow` —— 日志里那条
                       LiveMain.qml:146: ReferenceError: buttonsRow is not defined
                   就是它：标题会一直顶到窗口右边、压在窗口按钮下面。 */
                right: minimizeButton.left
                rightMargin: 8
            }
            elide: Text.ElideRight
            color: "#fff"
            font.pixelSize: 13
            text: liveRoot.title + (liveView.source !== "" ? "  —  " + liveView.source : "")
        }

        /*
         * 三颗窗口按钮：**必须显式写出来 + 逐个注册给 QWindowKit**。
         *
         * 上一版我用 Repeater 生成三颗，看着更短 —— 但那三颗点不动：
         * QWindowKit 的标题栏整条是"拖动区"，里面的**非系统按钮**不注册就收不到点击
         * （事件在命中测试那一层就被当成"拖窗口"吃掉了）。Main.qml:45-50 记的正是这条。
         * 所以：
         *   * 三颗按钮写成有 id 的实例（Repeater 的 delegate 没有稳定 id 可注册）；
         *   * Component.onCompleted 里用 setSystemButton(Minimize/Maximize/Close, …) 登记。
         */
        Rectangle {
            id: closeButton

            anchors {
                right: parent.right
                verticalCenter: parent.verticalCenter
            }
            width: 46
            height: titleBar.height
            color: closeHover.hovered ? "#E81123" : "transparent"

            HoverHandler { id: closeHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: liveRoot.close() }

            Text {
                anchors.centerIn: parent
                text: "✕"
                color: "#fff"
                font.pixelSize: 12
            }
        }

        Rectangle {
            id: maximizeButton

            anchors {
                right: closeButton.left
                verticalCenter: parent.verticalCenter
            }
            width: 46
            height: titleBar.height
            color: maxHover.hovered ? Qt.rgba(1, 1, 1, 0.14) : "transparent"

            HoverHandler { id: maxHover; cursorShape: Qt.PointingHandCursor }
            TapHandler {
                onTapped: liveRoot.visibility === Window.Maximized ? liveRoot.showNormal()
                                                                   : liveRoot.showMaximized()
            }

            Text {
                anchors.centerIn: parent
                text: liveRoot.visibility === Window.Maximized ? "❐" : "▢"
                color: "#fff"
                font.pixelSize: 13
            }
        }

        Rectangle {
            id: minimizeButton

            anchors {
                right: maximizeButton.left
                verticalCenter: parent.verticalCenter
            }
            width: 46
            height: titleBar.height
            color: minHover.hovered ? Qt.rgba(1, 1, 1, 0.14) : "transparent"

            HoverHandler { id: minHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: liveRoot.showMinimized() }

            Text {
                anchors.centerIn: parent
                text: "—"
                color: "#fff"
                font.pixelSize: 13
            }
        }
    }

    /* =======================================================================
     * 窗口系统（QWindowKit）：setup 必须在显示之前，先注册标题栏和系统按钮
     * ======================================================================= */
    WindowAgent {
        id: windowAgent
    }

    Component.onCompleted: {
        windowAgent.setup(liveRoot)
        /* 顺序不能反：setTitleBar 会清掉之前登记过的按钮 */
        windowAgent.setTitleBar(titleBar)
        windowAgent.setSystemButton(WindowAgent.Minimize, minimizeButton)
        windowAgent.setSystemButton(WindowAgent.Maximize, maximizeButton)
        windowAgent.setSystemButton(WindowAgent.Close, closeButton)
        windowAgent.setWindowAttribute("dark-mode", true)
        liveRoot.visible = true
    }

    /* Esc：全屏时退出全屏（不全屏时不动，交给上层窗口管理） */
    Shortcut {
        sequence: "Escape"
        enabled: liveRoot.visibility === Window.FullScreen
        onActivated: liveRoot.showNormal()
    }
}
