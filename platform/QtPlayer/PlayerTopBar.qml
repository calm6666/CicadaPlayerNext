// ===========================================================================
// 播放器顶部栏（压在画面上方的那一条）
//
// 复刻参考实现的 `components/Top.ts` + `styles/topbar.scss`。数值来自 topbar.scss，
// 全屏那一档来自 index.scss:4894 的 screen-style mixin（QML 里用 fullscreen 表达）。
//
// 参考实现里的两个事实（都按它的本意实现，不是照抄它的 bug）：
//   1. `.player-top-mask`（顶部 240px 的"黑 -> 透明"渐变）在 Top.ts:134 上带着
//      `hidden: true`，所以**参考里它永远不显示**；本播放器把它正常画出来
//      （参考的 hidden 属于演示用的临时开关）。
//   2. `.player-top-left-title`（标题）在普通窗口下是 `display:none`
//      （topbar.scss:65-68），只有全屏/网页全屏那两档才 `display:block`
//      （index.scss:4630-4635）。所以这里也是**全屏才显示标题**，跟参考一致。
//
// 没有搬过来的元素：`关注`（89x32 的头像胶囊）和头像图片 —— 那是 B 站站内的
// "关注 UP 主"，本播放器没有这个业务对象，硬画一个空胶囊就是摆样子。
//
// 元素与数值：
//   .player-top-wrap      top:0/left:0/width:100%、color:#fff、pointer-events:none、
//                         z-index:45、transition all .2s ease-in-out
//   .player-top-mask      240px 高，linear-gradient(180deg, transparent 16.87%, rgba(0,0,0,.3) 94%)
//                         再被 transform: matrix(1,0,0,-1,0,0) 上下翻转 —— 效果是
//                         "最上面最深、往下渐隐"，这里直接用渐变复刻翻转后的结果
//   .player-top-left      left:12px、margin-top:18px（全屏：left:24px、margin-top:24px）
//   标题                   font-size:20px（Top.ts:142 内联覆盖了 CSS 的 16px）、
//                         font-weight:500、line-height:24px、全屏时 margin-bottom:12px
//   .player-top-issue     height:24px、line-height:24px、margin:18px 12px 0、min-width:36px
//                         （全屏：font-size:28px、width:54px）
//   .player-top-issue-icon 22x22（全屏：28x28），fill: hsla(0,0%,100%,.8)
// ===========================================================================
import QtQuick

Item {
    id: topBar

    /* ---- 对外接口 ---- */

    /* 标题（本播放器放文件名；参考里放站内视频标题） */
    property string title: ""

    /* 是否全屏：决定标题显不显示、以及整条栏的内边距 */
    property bool fullscreen: false

    /* 跟随控制栏一起淡入淡出（参考里 Top 不参与自动隐藏，这里让它们一致更自然） */
    property bool shown: true

    /* 点右上角那颗"?" */
    signal helpRequested()

    /*
     * 顶部渐变遮罩。
     *
     * CSS 原文：height:240px；linear-gradient(180deg, transparent 16.87%, rgba(0,0,0,.3) 94%)；
     * transform: matrix(1,0,0,-1,0,0)（垂直翻转）。
     * 翻转之后从头到脚是：0~6% 不透明 0.3、6%~83% 渐变到全透明、之后全透明。
     */
    Rectangle {
        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
        }
        height: 240
        gradient: Gradient {
            GradientStop {
                position: 0.0
                color: Qt.rgba(0, 0, 0, 0.3)
            }
            GradientStop {
                position: 0.06
                color: Qt.rgba(0, 0, 0, 0.3)
            }
            GradientStop {
                position: 0.83
                color: "transparent"
            }
            GradientStop {
                position: 1.0
                color: "transparent"
            }
        }
        /* 只是装饰 */
        enabled: false
    }

    /* 整条栏都跟着 shown 淡入淡出（和参考 topbar.scss:13 的 transition 一致） */
    opacity: shown ? 1 : 0
    visible: opacity > 0

    Behavior on opacity {
        NumberAnimation {
            duration: 200
            easing.type: Easing.InOutQuad
        }
    }

    /* 标题：只有全屏才显示（普通窗口下参考就是 display:none） */
    Text {
        id: titleText

        anchors {
            left: parent.left
            leftMargin: topBar.fullscreen ? 24 : 12
            top: parent.top
            topMargin: topBar.fullscreen ? 24 : 18
            right: issueButton.left
        }
        visible: topBar.fullscreen && topBar.title !== ""
        color: "#ffffff"
        font.pixelSize: 20
        font.weight: Font.Medium
        elide: Text.ElideRight
        maximumLineCount: 1
        text: topBar.title
    }

    /*
     * 【按用户要求：顶栏最外层不加鼠标事件】
     * 原来这里有一个横贯整条、吃掉左键的 MouseArea（目的是不让点击穿到下面的画面活动层，
     * 避免点顶栏空白也切换播放/暂停）。现在去掉了：
     *   * 顶栏这一条**穿透**到画面活动层（和参考的 .player-top-wrap { pointer-events: none } 一致，
     *     参考里只有关注/问题按钮才是 pointer-events: all）；
     *   * 内部元素（下面那个"?"按钮）自己的 MouseArea 保留，点击照常生效。
     * 如果以后又需要"点顶栏空白不切播放"，请用**画面活动层那侧**的判据（比如按坐标排除顶栏那一条），
     * 而不是在这里盖一层吃点击的 MouseArea。
     */

    /* 右上角"?"（参考里是 .player-top-issue + .player-top-issue-icon 里的一个字形） */
    Item {
        id: issueButton

        anchors {
            right: parent.right
            rightMargin: 12
            top: parent.top
            topMargin: 18
        }
        width: topBar.fullscreen ? 54 : Math.max(36, 22)
        height: topBar.fullscreen ? 28 : 24

        Text {
            anchors.centerIn: parent
            color: topBar.fullscreen ? "#ffffff" : Qt.rgba(1, 1, 1, 0.8)
            font.pixelSize: topBar.fullscreen ? 28 : 22
            text: "?"
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: topBar.helpRequested()
        }
    }
}
