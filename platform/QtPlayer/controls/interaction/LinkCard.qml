// ===========================================================================
// 外链卡片（照 hili-player LinkCard.ts + interaction.scss 的 link 段复刻）
//
// DOM 结构（参考）：
//   .nova-danmaku-x-link                    —— 卡片根（50/62.5 高，圆角 10 8 8 10）
//   .nova-danmaku-x-circle                  —— 右上角关闭环（CloseCircle.qml）
//   .nova-danmaku-x-link-left               —— 左区（232 宽）：icon 90x50 + msg 134
//   .nova-danmaku-x-link-line               —— 分隔线（1x36，margin 0 8 0 0）
//   .nova-danmaku-x-link-right              —— 右区（45 宽，字 10px）：
//       watchlater 图标（24x24）+ "稍后再看"（竖排小字）
//
// 视觉规格（normal / large 档）：
//   根高 50 / 62.5；背景 rgba(33,33,33,0.8)；圆角 10 8 8 10（QML 只能统一圆角，取 9 折中）
//   左区 232 / 290；icon 90x50 / 112.5x62.5（内部 PNG 缩到 40px 显示、居中、
//     左侧圆角 8 8 0 0 —— clip 容器实现）；msg 134 / 177.5（行高 17、两行封顶 34、
//     word-break、省略）；字 12px
//   分隔线 1x36 / 1.25x45，色 hsla(0,0%,100%,.5)，margin 0 8 0 0
//   右区 45 / 56.25：icon 24 / 25，字 10px
//
// 交互：左区点击（外链跳转，QtPlayer 无业务 → 视觉反馈占位）、右区"稍后再看"点击、
//   关闭环点击（closed 标记由 InteractionLayer 接走）
// 文案：linkContent 为空时默认"这是一个什么视频"（照参考）
// ===========================================================================
import QtQuick
import QtQuick.Effects

Rectangle {
    id: link

    // ---------------------------------------------------------------------
    // 对外接口
    // ---------------------------------------------------------------------

    /* 卡片数据：{ top, left, linkContent, timeStart, timeEnd } */
    property var cardData: ({})

    /* 大尺寸档（宽>1366.9 且全屏） */
    property bool large: false

    /* 卡片剩余显示进度（喂给关闭环） */
    property real remaining: 1

    /* 关闭环点击 —— delegate 接住打 closed 标记（触发淡出） */
    signal closeRequested()

    /* 左区 / 右区点击（QtPlayer 没有外链业务，仅冒个日志占位） */
    signal linkClicked()
    signal watchLaterClicked()

    /*
     * 状态持久化协议（与另外三张卡片统一，InteractionLayer 一起接线）：
     * 外链卡没有观众交互状态 → 快照恒为空对象（恢复也是空操作）。
     */
    property string stateKey: ""
    signal stateMutated(var snapshot)
    function restoreState(snapshot) {
        /* 无状态：协议占位 */
    }
    function emitState() {
        link.stateMutated({})
    }

    /* rgba(33,33,33,0.8)（#212121 @ 80%） */
    color: "#CC212121"
    /* 参考是逐角圆角 10 8 8 10（左 10 右 8）——QML 的 Rectangle 只支持统一圆角，
       取 9 折中（1px 差异肉眼不可见） */
    radius: 9

    /* 尺寸规格（normal / large） */
    readonly property real leftWidth: link.large ? 290 : 232
    readonly property real iconWidth: link.large ? 112.5 : 90
    readonly property real msgWidth: link.large ? 177.5 : 134
    readonly property real lineWidth: link.large ? 1.25 : 1
    readonly property real lineHeight: link.large ? 45 : 36
    readonly property real rightWidth: link.large ? 56.25 : 45
    readonly property real watchIconSize: link.large ? 25 : 24

    implicitWidth: link.leftWidth + link.lineWidth + 8 + link.rightWidth
    implicitHeight: link.large ? 62.5 : 50
    width: implicitWidth
    height: implicitHeight

    // ---------------------------------------------------------------------
    // 右上角关闭环（top/right -13）
    // ---------------------------------------------------------------------

    CloseCircle {
        anchors.right: parent.right
        anchors.rightMargin: -13
        anchors.top: parent.top
        anchors.topMargin: -13
        remaining: link.remaining
        onClicked: link.closeRequested()
    }

    // ---------------------------------------------------------------------
    // 内容行：左区 + 分隔线 + 右区（照参考的 flex 居中排布）
    // ---------------------------------------------------------------------

    Row {
        anchors.centerIn: parent
        spacing: 0

        /* ---- 左区（232 宽）：链接缩略图 + 文案，整块可点击 ---- */
        Item {
            width: link.leftWidth
            height: link.height

            /* 缩略图底格（90x50，左侧圆角 8 —— 用 clip 容器实现） */
            Item {
                id: linkIconClip

                x: 0
                width: link.iconWidth
                height: link.height
                clip: true
                /* 背景与圆角（.nova-danmaku-x-link-icon 的 border-radius 8 0 0 8） */
                Rectangle {
                    anchors.fill: parent
                    color: "#00000000"
                    radius: 8
                }

                /* 缩略图本体（参考里 background-size:40px 居中显示） */
                Image {
                    anchors.centerIn: parent
                    width: 40
                    height: 40
                    source: "../../assets/images/interaction/link-icon.png"
                    sourceSize: Qt.size(40, 40)
                    smooth: true
                    fillMode: Image.PreserveAspectFit
                }
            }

            /* 文案区（msg 134 宽、行高 17、最多两行 34、margin-left 8、word-break） */
            Text {
                x: linkIconClip.width + 8
                width: link.msgWidth
                anchors.verticalCenter: parent.verticalCenter

                /* 空文案的默认值照参考（"这是一个什么视频"） */
                text: (link.cardData.linkContent !== undefined
                       && link.cardData.linkContent !== "") ? link.cardData.linkContent
                                                            : qsTr("这是一个什么视频")
                color: "#ffffff"
                font.pixelSize: 12
                lineHeight: 17
                lineHeightMode: Text.FixedHeight
                wrapMode: Text.WrapAnywhere
                elide: Text.ElideRight
                maximumLineCount: 2
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: link.linkClicked()
            }
        }

        /* ---- 分隔线（1x36，margin 0 8 0 0 → 右侧 8px 留白）---- */
        Item {
            width: link.lineWidth + 8
            height: link.height

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: link.lineWidth
                height: link.lineHeight
                color: "#80FFFFFF"      /* hsla(0,0%,100%,.5) */
            }
        }

        /* ---- 右区（45 宽）：稍后再看（icon + 竖排小字）---- */
        Item {
            width: link.rightWidth
            height: link.height

            Column {
                anchors.centerIn: parent
                spacing: 0

                Image {
                    id: watchIcon

                    anchors.horizontalCenter: parent.horizontalCenter
                    width: link.watchIconSize
                    height: link.watchIconSize
                    source: "../../assets/images/interaction/link-watchlater.svg"
                    sourceSize: Qt.size(link.watchIconSize, link.watchIconSize)
                    smooth: true
                    visible: false
                }

                MultiEffect {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: watchIcon.width
                    height: watchIcon.height
                    source: watchIcon
                    colorization: 1
                    colorizationColor: "#ffffff"
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("稍后再看")
                    color: "#ffffff"
                    font.pixelSize: 10
                }
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: link.watchLaterClicked()
            }
        }
    }
}
