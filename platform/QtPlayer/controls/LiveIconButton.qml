// ===========================================================================
// LiveIconButton —— 直播控制栏里的一颗图标按钮（**点播那套 PlayerControlBar 不参与**）
//
// 【为什么单独一个文件】LiveControlBar.qml 里要用 7~8 颗按钮，每颗都要
// "36×22 的命中区 + 22px 的图标 + hover 变白 + 悬停气泡 + 点击信号"。
// 写成一个可复用组件，而不是在控制栏里复制八遍 —— 复制八遍的后果是"改一处忘七处"。
//
// 【尺寸/颜色出处】参考工作区里**没有**真实的直播控制栏（hilihili-live 里
// `.web-player-controller-wrap` 是个空 div，真正的控制条是运行时才填进去的），
// 所以这里按 B 站**点播**控制栏那颗 `.player-ctrl-btn` 的实测数值来，视觉语言一致：
//     .player-ctrl-btn { width:36px; height:22px; color:hsla(0,0%,100%,.8) }
//     hover → #fff；图标 22px、opacity .9（player.scss:285-322）
//
// 【图标来源：优先用工程里已有的原件，不新造素材】
//   play / pause     → assets/images/play.svg、pause.svg（工程里本来就有）
//   danmaku          → assets/images/dm/DanmakuSwitchOn.svg / DanmakuSwitchOff.svg
//   volume / mute    → assets/lottie-icon 里那几支音量动画（和点播控制栏同一套素材）
//   full / webfull / info → fullscreen-animation.json / web-fullscreen-animation.json /
//                           settings-animation.json（点播控制栏用的就是这三支）
// ===========================================================================
import QtQuick
import QtPlayer

Item {
    id: button

    /* 图标种类（见上面那段来源说明） */
    property string kind: "play"
    /* 悬停气泡文字（空串 = 不显示气泡） */
    property string tip: ""
    /* 高亮态（弹幕开关打开时那颗"弹"要亮着） */
    property bool active: false
    /* 是否可点（拿不到数据时可置假，比如清晰度列表为空） */
    property bool enabledAction: true

    signal tapped()

    /* 命中盒：官方 `.live-web-player-controller .icon{width:36px; height:36px}`（真值） */
    width: 36
    height: 36

    /* hover：用 HoverHandler（不是 hoverEnabled 的 MouseArea）——
       这个 App 的 hover 是"最上层独占"，控制栏里多挂一个吃 hover 的 MouseArea
       会把外层的"鼠标还在控制栏上"判据抢走（见 docs/QML-POPUP-HOVER-PITFALLS.md）。 */
    HoverHandler {
        id: hover
        enabled: button.enabledAction
        cursorShape: Qt.PointingHandCursor
    }

    TapHandler {
        enabled: button.enabledAction
        onTapped: button.tapped()
    }

    readonly property color idleColor: button.active ? "#FFFFFF" : Qt.rgba(1, 1, 1, 0.8)
    readonly property color hoverColor: "#FFFFFF"

    /* ---------------- 图标 ---------------- */

    /* play / pause：两张 SVG 原件（和点播控制栏"两张图叠着切显隐"一个做法） */
    Image {
        anchors.centerIn: parent
        width: 20
        height: 20
        visible: button.kind === "play" || button.kind === "pause"
        source: button.kind === "pause" ? "../assets/images/pause.svg" : "../assets/images/play.svg"
        sourceSize: Qt.size(20, 20)
        fillMode: Image.PreserveAspectFit
        smooth: true
        opacity: hover.hovered ? 1.0 : 0.9
        /* 原图是白色填充；用 MultiEffect 上色太贵（每帧一次），
           这里靠不透明度区分 hover 态，颜色保持白色 —— 和参考的 .player-ctrl-btn 一致 */
    }

    /* 弹幕开关：两张原件（开/关） */
    Image {
        anchors.centerIn: parent
        width: 20
        height: 20
        visible: button.kind === "danmaku"
        source: button.active ? "../assets/images/dm/DanmakuSwitchOn.svg"
                              : "../assets/images/dm/DanmakuSwitchOff.svg"
        sourceSize: Qt.size(20, 20)
        fillMode: Image.PreserveAspectFit
        smooth: true
        opacity: hover.hovered ? 1.0 : 0.9
    }

    /* 音量 / 静音：Lottie 素材（静态停一帧），两条叠着按 muted 切显隐 */
    LottieIcon {
        anchors.centerIn: parent
        width: 20
        height: 20
        visible: button.kind === "volume" || button.kind === "mute"
        canvasWidth: 88
        canvasHeight: 88
        opacity: hover.hovered ? 1.0 : 0.9
        sequence: [{
            "source": (button.kind === "mute")
                        ? "assets/lottie-icon/mute-off-animation.json"
                        : "assets/lottie-icon/volume-hover-animation.json",
            "autoplay": false
        }]
    }

    /* 全屏 / 网页全屏 / 统计信息：三支 Lottie（点播控制栏同一套素材，移入播一遍） */
    LottieIcon {
        id: lottie

        anchors.centerIn: parent
        width: 20
        height: 20
        visible: button.kind === "full" || button.kind === "webfull" || button.kind === "info"
        canvasWidth: 88
        canvasHeight: 88
        opacity: hover.hovered ? 1.0 : 0.9
        sequence: [{
            "source": button.kind === "full"
                        ? "assets/lottie-icon/fullscreen-animation.json"
                        : (button.kind === "webfull"
                           ? "assets/lottie-icon/web-fullscreen-animation.json"
                           : "assets/lottie-icon/settings-animation.json"),
            "autoplay": false
        }]

        Connections {
            target: hover
            function onHoveredChanged() {
                if (hover.hovered)
                    lottie.play()
            }
        }
    }

    /* ---------------- 悬停气泡 ---------------- */
    Rectangle {
        id: tipBox

        readonly property string label: button.tip

        visible: label !== "" && hover.hovered
        opacity: visible ? 1 : 0
        width: tipText.implicitWidth + 16
        height: 26
        radius: QtPlayerTheme.pbpPinTipRadius
        color: QtPlayerTheme.tooltipBg
        /* 摆在按钮正上方（控制栏在底部，只能往上弹） */
        x: (button.width - width) / 2
        y: -height - 6
        z: 100

        Behavior on opacity {
            NumberAnimation { duration: 150; easing.type: Easing.OutQuad }
        }

        Text {
            id: tipText

            anchors.centerIn: parent
            text: tipBox.label
            color: QtPlayerTheme.tooltipText
            font.pixelSize: 12
            /* 单行不设 lineHeight（QML 的 lineHeight 会把行距加到字形下面 → 字偏上），
               居中最省事也最准（和 ProgressRow 那颗 pin 的气泡同一个结论） */
        }
    }
}
