// ===========================================================================
// 网页窗口：首页那个「HTML 链接」输入框打开的就是这一个
//
// 【怎么被打开】HomeWindow.qml 里 openWebUrl(url) → ensureWebWindow() →
//   Qt.createComponent("qrc:/qt/qml/QtPlayer/WebWindow.qml").createObject(home.contentItem)
//   → 写 pageUrl → show()。窗口对象和直播窗口一样"关了再销毁"（见 HomeWindow 里那两段注释）。
//
// 【窗口形态】和首页完全一致：无系统边框（QWindowKit 接管）+ **自绘标题栏**。
// 下面 CaptionButton 组件、WindowAgent 登记、标题栏矩形这三块是**逐行复制
// HomeWindow.qml 的**（用户要求"顶部的标题栏直接完整复制首页的代码"）。
// 复制而不是抽公共组件的代价：以后改标题栏要**两处一起改**。
// 真要收敛的话，应该抽成一个 CaptionBar.qml 让两边都用。
//
// 【中间是 WebView】用的是 QtWebView（不是 QtWebEngine）：
//   * 它本身只是个壳，真正的实现在 plugins/webview/ 下按平台选后端；
//     Windows 上是 WebView2（Edge 运行时，Win10/11 自带）；
//   * 因此 main.cpp 里必须先调一次 QtWebView::initialize()，且 CMake 里要链
//     Qt6::WebView + 静态插件 Qt6::QWebView2WebViewPlugin（非 QML 插件，扫不到）。
// ===========================================================================
import QtQuick
import QtQuick.Controls.Basic
import QtWebView
import QWindowKit
import QtPlayer

Window {
    id: webWindow

    width: 1000
    height: 700
    visible: false                     /* 等 WindowAgent.setup() 装好再显示，避免闪一下系统标题栏 */
    /* 标题跟着网页标题走（WebView.title），没拿到之前显示占位文案 */
    title: webView.title.length > 0 ? webView.title : qsTr("网页")
    color: QtPlayerTheme.windowBg

    /* 要打开的链接。HomeWindow 建好窗口后写这个属性（写法见文件头）。 */
    property string pageUrl: ""

    /*
     * 打开一个链接（首页按钮调这里；地址栏回车也调这里）。
     * 允许用户只写 "www.bilibili.com" 这种没有协议的形式 —— 补一个 https://，
     * 否则 WebView 会把它当相对路径。
     */
    function openPage(u) {
        var s = ("" + u).trim()

        if (s.length === 0)
            return

        if (!/^[a-zA-Z][a-zA-Z0-9+.-]*:\/\//.test(s))
            s = "https://" + s

        webView.url = s
        urlField.text = s
    }

    onPageUrlChanged: openPage(pageUrl)

    /* 标题栏按钮：照抄 Main.qml:144-329 的 CaptionButton（太阳/月亮、关闭/最大化/最小化/还原） */
    component CaptionButton: Rectangle {
        id: captionButton

        property string kind: "minimize"
        property bool maximized: false
        property bool dark: QtPlayerTheme.dark

        signal clicked()

        width: 46
        color: pointer.pressed ? QtPlayerTheme.captionPressed
                               : (pointer.containsMouse ? QtPlayerTheme.captionHover : "transparent")

        MouseArea {
            id: pointer
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.ArrowCursor
            onClicked: captionButton.clicked()
        }

        Item {
            anchors.centerIn: parent
            width: 14
            height: 14

            Rectangle {
                visible: captionButton.kind === "minimize"
                anchors.centerIn: parent
                width: 11
                height: 1
                color: QtPlayerTheme.captionGlyph
            }

            Rectangle {
                visible: captionButton.kind === "maximize" && !captionButton.maximized
                anchors.centerIn: parent
                width: 11
                height: 11
                color: "transparent"
                border.width: 1
                border.color: QtPlayerTheme.captionGlyph
            }

            Rectangle {
                visible: captionButton.kind === "maximize" && captionButton.maximized
                x: 4
                y: 0
                width: 10
                height: 1
                color: QtPlayerTheme.captionGlyph
            }

            Rectangle {
                visible: captionButton.kind === "maximize" && captionButton.maximized
                x: 13
                y: 0
                width: 1
                height: 10
                color: QtPlayerTheme.captionGlyph
            }

            Rectangle {
                visible: captionButton.kind === "maximize" && captionButton.maximized
                x: 0
                y: 4
                width: 10
                height: 10
                color: "transparent"
                border.width: 1
                border.color: QtPlayerTheme.captionGlyph
            }

            Rectangle {
                visible: captionButton.kind === "close"
                anchors.centerIn: parent
                width: 12
                height: 1
                color: QtPlayerTheme.captionGlyph
                rotation: 45
            }

            Rectangle {
                visible: captionButton.kind === "close"
                anchors.centerIn: parent
                width: 12
                height: 1
                color: QtPlayerTheme.captionGlyph
                rotation: -45
            }

            Rectangle {
                visible: captionButton.kind === "theme" && !captionButton.dark
                anchors.centerIn: parent
                width: 8
                height: 8
                radius: 4
                color: "transparent"
                border.width: 1
                border.color: QtPlayerTheme.captionGlyph
            }

            Rectangle {
                visible: captionButton.kind === "theme" && !captionButton.dark
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                width: 1
                height: 3
                color: QtPlayerTheme.captionGlyph
            }

            Rectangle {
                visible: captionButton.kind === "theme" && !captionButton.dark
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                width: 1
                height: 3
                color: QtPlayerTheme.captionGlyph
            }

            Rectangle {
                visible: captionButton.kind === "theme" && !captionButton.dark
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                width: 3
                height: 1
                color: QtPlayerTheme.captionGlyph
            }

            Rectangle {
                visible: captionButton.kind === "theme" && !captionButton.dark
                anchors.verticalCenter: parent.verticalCenter
                anchors.right: parent.right
                width: 3
                height: 1
                color: QtPlayerTheme.captionGlyph
            }

            Rectangle {
                visible: captionButton.kind === "theme" && captionButton.dark
                anchors.centerIn: parent
                width: 11
                height: 11
                radius: 5.5
                color: QtPlayerTheme.captionGlyph
            }
        }
    }

    WindowAgent {
        id: windowAgent
    }

    /* 和 Main.qml / HomeWindow.qml 同一套：把窗口交给 QWindowKit，再把标题栏/系统按钮登记进去。
       顺序不能反 —— setTitleBar 会清掉之前登记的按钮（见 abstractwindowcontext.cpp）。 */
    Component.onCompleted: {
        windowAgent.setup(webWindow)
        windowAgent.setTitleBar(titleBar)

        if (!webWindow.nativeCaptionButtons) {
            windowAgent.setSystemButton(WindowAgent.Minimize, minimizeButton)
            windowAgent.setSystemButton(WindowAgent.Maximize, maximizeButton)
            windowAgent.setSystemButton(WindowAgent.Close, closeButton)
        }

        /* 标题栏里除系统按钮外的可交互元素都要登记，否则整条标题栏是拖动区，
           点主题按钮只会拖窗口（照抄 Main.qml:769） */
        windowAgent.setHitTestVisible(themeButton, true)

        /* 把深浅色同步给窗口系统（标题栏/边框的暗色由它管），之后由下面的 Connections 跟着变 */
        webWindow.applyWindowTheme()

        webWindow.visible = true
    }

    readonly property bool nativeCaptionButtons: Qt.platform.os === "osx"

    function applyWindowTheme() {
        windowAgent.setWindowAttribute("dark-mode", QtPlayerTheme.dark)
    }

    /* 主题切换（设置页里切深浅色）时通知窗口系统那一层 —— 和 Main.qml 里那段一样 */
    Connections {
        target: QtPlayerTheme

        function onDarkChanged() {
            webWindow.applyWindowTheme()
        }
    }

    /* ---------------- 标题栏（拖拽 + 右上角三键） ---------------- */
    Rectangle {
        id: titleBar

        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
        }
        height: 36
        color: QtPlayerTheme.windowBg

        /* 标题（照抄 Main.qml:909-924，文案换成网页标题） */
        Text {
            anchors.left: parent.left
            anchors.leftMargin: webWindow.nativeCaptionButtons ? 78 : 12
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: themeButton.left
            anchors.rightMargin: 8
            elide: Text.ElideRight
            color: QtPlayerTheme.titleText
            font.pixelSize: 13
            text: webWindow.title
        }

        /* 主题按钮（太阳/月亮）：照抄 Main.qml:937-945 */
        CaptionButton {
            id: themeButton
            anchors.right: minimizeButton.visible ? minimizeButton.left : parent.right
            height: titleBar.height
            kind: "theme"
            dark: QtPlayerTheme.dark
            onClicked: QtPlayerTheme.mode = QtPlayerTheme.dark ? "light" : "dark"
        }

        /* 三颗窗口按钮：照抄 Main.qml:960-993（关闭在最右，往左排） */
        CaptionButton {
            id: closeButton
            anchors.right: parent.right
            height: titleBar.height
            kind: "close"
            visible: !webWindow.nativeCaptionButtons
            onClicked: webWindow.close()
        }

        CaptionButton {
            id: maximizeButton
            anchors.right: closeButton.left
            height: titleBar.height
            kind: "maximize"
            maximized: webWindow.visibility === Window.Maximized
            visible: !webWindow.nativeCaptionButtons
            onClicked: {
                if (maximized)
                    webWindow.showNormal()
                else
                    webWindow.showMaximized()
            }
        }

        CaptionButton {
            id: minimizeButton
            anchors.right: maximizeButton.left
            height: titleBar.height
            kind: "minimize"
            visible: !webWindow.nativeCaptionButtons
            onClicked: webWindow.showMinimized()
        }
    }

    /* ---------------- 浏览器工具条：后退 / 前进 / 刷新 / 地址栏 ---------------- */
    Rectangle {
        id: toolbar

        anchors {
            left: parent.left
            right: parent.right
            top: titleBar.bottom
        }
        height: 40
        color: QtPlayerTheme.windowBg

        Row {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 8

            Button {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("←")
                enabled: webView.canGoBack
                onClicked: webView.goBack()
            }

            Button {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("→")
                enabled: webView.canGoForward
                onClicked: webView.goForward()
            }

            /*
             * 刷新 / 停止 合成一颗：正在加载时显示"停止"（就是浏览器的常规做法），
             * 加载完了显示"刷新"。用户要的"支持刷新"就是这一颗。
             */
            Button {
                anchors.verticalCenter: parent.verticalCenter
                text: webView.loading ? qsTr("停止") : qsTr("刷新")
                onClicked: {
                    if (webView.loading)
                        webView.stop()
                    else
                        webView.reload()
                }
            }

            TextField {
                id: urlField

                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 3 * 66 - 3 * parent.spacing - 120
                placeholderText: qsTr("https://...")
                placeholderTextColor: QtPlayerTheme.panelHintText
                selectByMouse: true
                /* 地址栏显示当前真实地址（页面里跳转后跟着变） */
                text: webView.url
                onAccepted: webWindow.openPage(text)
            }

            Button {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("转到")
                enabled: urlField.text.length > 0
                onClicked: webWindow.openPage(urlField.text)
            }
        }
    }

    /* 加载进度：细一条，压在工具条下沿（浏览器常见做法）。不加载时完全不占位。 */
    Rectangle {
        id: progressBar

        anchors {
            left: parent.left
            right: parent.right
            top: toolbar.bottom
        }
        height: 2
        visible: webView.loading
        color: "transparent"

        Rectangle {
            height: parent.height
            width: parent.width * Math.max(0, Math.min(100, webView.loadProgress)) / 100
            color: QtPlayerTheme.playerAccent
        }
    }

    /* ---------------- 内容区：网页本体 ---------------- */
    WebView {
        id: webView

        anchors {
            left: parent.left
            right: parent.right
            top: progressBar.visible ? progressBar.bottom : toolbar.bottom
            bottom: parent.bottom
        }

        /*
         * 加载失败要说人话：QtWebView 的 loadingChanged 会带一个 WebViewLoadRequest，
         * status 是 Error 时把 errorString 打到控制台（窗口里没有 toast，先不引）。
         */
        onLoadingChanged: function (request) {
            if (request.status === WebView.LoadFailedStatus) {
                console.warn("[web] 加载失败（" + request.errorString + "）：" + request.url)
            }
        }
    }
}
