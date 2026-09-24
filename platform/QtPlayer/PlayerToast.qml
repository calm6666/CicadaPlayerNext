// ===========================================================================
// 播放器提示条（toast）—— 复刻参考实现 component/toast/index.scss 的 .player-toast-wrap
//
// 【参考的 CSS】
//     bottom:70px; left:10px; background:rgba(0,0,0,.8); border-radius:4px;
//     color:#fff; line-height:36px; font-size:14px; padding:0 10px;
//     position:absolute; z-index:65; pointer-events:none; transition:all .3s
//
// 用在哪里：
//   * 右键菜单「复制视频地址（精准空降）」→ "已复制：<链接>"
//   * 右键菜单「播放器版本 1.0.0」→ 显示版本号（参考里这一项点了没反应，
//     这里给它一个不假但有用的反馈）
// ===========================================================================
import QtQuick
import QtPlayer

Rectangle {
    id: toast

    /* 要显示的文字（空串 = 不显示） */
    property string message: ""
    /* 多久之后自动消失 */
    property int duration: 2600

    readonly property bool shown: message !== ""

    function show(text) {
        message = text
        hideTimer.restart()
    }

    function hide() {
        message = ""
        hideTimer.stop()
    }

    /* bottom:70px; left:10px */
    x: 10
    anchors.bottom: parent.bottom
    anchors.bottomMargin: 70
    width: toastText.implicitWidth + 20   /* padding: 0 10px */
    height: 36                            /* line-height: 36px */
    color: Qt.rgba(0, 0, 0, 0.8)
    radius: 4
    visible: opacity > 0
    opacity: shown ? 1 : 0
    /* pointer-events:none（参考）：提示条不吃鼠标，别挡住下面的画面/控制栏 */
    z: 65

    Behavior on opacity {
        NumberAnimation { duration: 300 }   /* transition: all .3s */
    }

    Text {
        id: toastText

        anchors.centerIn: parent
        color: "#fff"
        font.pixelSize: 14
        text: toast.message
    }

    Timer {
        id: hideTimer
        interval: toast.duration
        repeat: false
        onTriggered: toast.hide()
    }
}
