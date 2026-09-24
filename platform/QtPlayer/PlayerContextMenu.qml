// ===========================================================================
// 播放器右键菜单（复刻参考实现 component/context/index.ts + .player-contextmenu）
//
// 【参考的 DOM】
//     <div class="player-context-area">
//       <ul class="player-contextmenu player-black">
//         <li data-action="copyLink">复制视频地址（精准空降）</li>
//         <li data-action="color">视频色彩调整</li>
//         <li data-action="keyboard">快捷键说明</li>
//         <li data-action="version">播放器版本 1.0.0</li>
//         <li data-action="info">视频统计信息</li>
//       </ul>
//     </div>
//
// 【参考的 CSS（index.scss:2159-2210）】
//     .player-context-area  { position:absolute; inset:0; z-index:1000; pointer-events:none }
//     .player-contextmenu   { background:rgba(33,33,33,.9); border-radius:4px; color:#eee;
//                             min-width:80px; padding:0; position:absolute; z-index:80;
//                             opacity:0; visibility:hidden;
//                             transition:opacity .1s cubic-bezier(0,0,.2,1) }
//     .player-contextmenu > li { color:#eee; cursor:pointer; font-size:12px; height:30px;
//                                line-height:30px; padding:4px 20px; text-align:left;
//                                white-space:nowrap; overflow:hidden; text-overflow:ellipsis }
//     .player-contextmenu > li:hover { background:hsla(0,0%,100%,.12) }
//     .player-contextmenu.player-active { opacity:1; visibility:visible }
//
// 上面的尺寸在 QML 里的对应：每个 li 高 38（30 行高 + 上下各 4 内边距），文字垂直居中、
// 左右各留 20；悬停底色 Qt.rgba(1,1,1,0.12)；整块底色 #E6212121（= rgba(33,33,33,.9)）。
//
// 【和参考的差别（有意为之）】
//   * 参考的"播放器版本 1.0.0"点了什么都不做；这里点一下弹一条 toast 把版本号显示出来
//     （同样的文案，不做假功能，也不让它成为一个"点了没反应"的死项）。
//   * 每一项都接了**真实动作**（见 Main/PlayerView 里的信号处理），没有占位项。
// ===========================================================================
import QtQuick
import QtPlayer

Item {
    id: menuRoot

    /* 菜单开着没有（外部读写；PlayerView 用它判"有浮层"） */
    property bool opened: false
    /* 弹出的位置（相对本组件，也就是播放区左上角） */
    property point popupPoint: Qt.point(0, 0)
    /* 菜单右侧/下侧至少留这么多边距，超出就往回收 */
    property int edgeMargin: 6

    /* 五个动作，全部由外面接（真功能都在 PlayerView/Main 里） */
    signal copyLinkRequested()
    signal colorRequested()
    signal hotkeyRequested()
    signal versionRequested()
    signal infoRequested()

    /* 参考里那一份菜单项（文案、顺序一字不差） */
    readonly property var menuItems: [
        { action: "copyLink", text: qsTr("复制视频地址（精准空降）") },
        { action: "color",    text: qsTr("视频色彩调整") },
        { action: "keyboard", text: qsTr("快捷键说明") },
        { action: "version",  text: qsTr("播放器版本 1.0.0") },
        { action: "info",     text: qsTr("视频统计信息") }
    ]

    anchors.fill: parent
    /* 参考的 .player-context-area 是 z-index:1000（压在控制栏/顶部栏之上） */
    z: 1000
    visible: opened

    /*
     * 【菜单宽度必须**独立**算出来 —— 这是上一版把菜单做窄了的根因】
     *
     * 上一版写的是：Column 宽 = max(80, implicitWidth)，delegate 宽 = menuColumn.width。
     * 而 Column 的 implicitWidth 本身就是**由子项宽度算出来的** —— 于是形成循环依赖：
     * Qt 解析不出结果，宽度塌成 80px，菜单里所有文字都被截断（用户实测："整个变得很窄，
     * 文字都显示不全了"）。
     *
     * 现在用 TextMetrics 量一遍最长的那个菜单文案（和 delegate 里 Text 的字体一致），
     * 加上 CSS 的左右内边距 20+20，一次性算出菜单宽度；Column 和每个 delegate 都用它 ——
     * 没有任何宽度依赖自己，循环不存在，而且每一项都撑满整条菜单（hover 底色也横贯整条）。
     */
    readonly property real menuWidth: {
        var maxWidth = 0

        for (var i = 0; i < menuItems.length; ++i) {
            menuMetrics.text = menuItems[i].text

            if (menuMetrics.width > maxWidth)
                maxWidth = menuMetrics.width
        }

        /* min-width:80px；文字宽 + padding 4px 20px（左右各 20） */
        return Math.max(80, Math.ceil(maxWidth) + 40)
    }

    TextMetrics {
        id: menuMetrics

        /* 必须和菜单项里的 Text 完全一致（字号/字重/字体族），否则量出来的宽度对不上 */
        font.pixelSize: 12
        text: ""
    }

    function popup(x, y) {
        popupPoint = Qt.point(x, y)
        opened = true
    }

    function close() {
        opened = false
    }

    function trigger(action) {
        close()

        switch (action) {
        case "copyLink":
            menuRoot.copyLinkRequested()
            break
        case "color":
            menuRoot.colorRequested()
            break
        case "keyboard":
            menuRoot.hotkeyRequested()
            break
        case "version":
            menuRoot.versionRequested()
            break
        case "info":
            menuRoot.infoRequested()
            break
        }
    }

    /*
     * 点菜单外面任意位置就关掉（对应参考里 document 上的那个 click 监听）。
     * 铺满整个播放区、接所有按键 —— 它只负责"关"，不消费事件给别处，
     * 所以点击外面时菜单关掉、这一次点击也就到此为止（和浏览器里一致）。
     */
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onClicked: menuRoot.close()
    }

    Rectangle {
        id: menuBox

        /*
         * 位置：以右键点为左上角，但超出播放区就贴着边缘收回来
         * （参考里是直接写死 left/top，没有回收；这里补上，免得菜单跑到窗口外面）。
         */
        x: Math.max(0, Math.min(menuRoot.popupPoint.x, menuRoot.width - width))
        y: Math.max(0, Math.min(menuRoot.popupPoint.y, menuRoot.height - height))
        /* 宽度由 TextMetrics 独立算出（见 menuRoot.menuWidth 的说明），不依赖子项宽度 */
        width: menuRoot.menuWidth
        height: menuColumn.height
        /* rgba(33,33,33,0.9) */
        color: "#E6212121"
        radius: 4
        border.width: 0
        clip: true

        /* transition: opacity .1s cubic-bezier(0,0,.2,1) */
        opacity: menuRoot.opened ? 1 : 0
        Behavior on opacity {
            NumberAnimation { duration: 100; easing.type: Easing.OutCubic }
        }

        Column {
            id: menuColumn

            /* 宽度固定为算出来的菜单宽度（列本身不再参与"决定菜单多宽"，避免循环依赖） */
            width: menuRoot.menuWidth

            Repeater {
                model: menuRoot.menuItems

                delegate: Item {
                    id: menuItem

                    required property var modelData

                    /* li: 占满菜单宽度（= 最长那一项 + 左右 20）；高度 30 + 上下 padding 4 → 38 */
                    width: menuRoot.menuWidth
                    height: 38

                    Rectangle {
                        anchors.fill: parent
                        color: itemHover.hovered ? Qt.rgba(1, 1, 1, 0.12) : "transparent"
                    }

                    Text {
                        id: menuLabel
                        x: 20
                        anchors.verticalCenter: parent.verticalCenter
                        /* li 是 white-space:nowrap + ellipsis；这里给足宽度就不会截断 */
                        color: "#eee"
                        font.pixelSize: 12
                        text: menuItem.modelData.text
                    }

                    HoverHandler {
                        id: itemHover
                        cursorShape: Qt.PointingHandCursor
                    }

                    TapHandler {
                        onTapped: menuRoot.trigger(menuItem.modelData.action)
                    }
                }
            }
        }
    }
}
