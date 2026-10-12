// ===========================================================================
// 左下角缩小条目（照 hili-player ShrinkBar.ts + interaction.scss 的
// .nova-danmaku-x-cmd-shrink 段复刻）
//
// 展示模式下卡片被手动关闭（关闭环未归零时点击 ×）后，在播放区左下角
// 生成的倒计时残留条目：
//   * 深色胶囊（高 30px 圆角 20px，rgba(24,25,28,.8)，padding 0 10px 0 36px）
//   * 左侧 26px 进度环（top/left 2px，环色 #ffffffb3）中心为卡片类型图标
//     （16x16 圆形 —— link→link-video / vote→vote / score|scoreSum→score）
//   * 标题白字（"推荐" / "投票"，由卡片类型映射）
//   * 进度环沿用卡片原时间窗继续随播放倒数，归零后条目移除（由
//     InteractionLayer 的同步函数管，这里只展示 remaining）
//   * 点击条目任意区域重新打开原卡片（清关闭标记 → 卡片立即重建显示）
//
// 位置 left:23px / bottom:71px（CSS 常量，照 scss）；缩放经 --scale CSS
// 变量协议（transform-origin left bottom → Qt 的 Item.BottomLeft）。
// ===========================================================================
import QtQuick

Rectangle {
    id: shrinkBar

    // ---------------------------------------------------------------------
    // 对外接口
    // ---------------------------------------------------------------------

    /* 条目标题（"推荐" / "投票" —— 由被关闭卡片的类型映射） */
    property string title: ""

    /* 进度环中心的类型图标（图片路径，由被关闭卡片的类型映射） */
    property string iconSource: ""

    /* 进度环剩余进度（1 → 0，随播放按原卡片时间窗继续倒数） */
    property real remaining: 1

    /* 显示态（挂载后翻 true 播放 0.35s 淡入 —— 照显隐过渡协议） */
    property bool show: false

    /* 点击条目（重开原卡片 —— 由 InteractionLayer 接线） */
    signal clicked()

    /* rgba(24,25,28,0.8)（#18191C @ 80%） */
    color: "#CC18191C"
    radius: 20
    height: 30
    /* 宽 = 左侧环区 36（26 环 + 左内边距差量） + 标题宽 + 右内边距 10 */
    implicitWidth: 36 + shrinkTitleText.implicitWidth + 10
    width: implicitWidth

    /* 缩放协议（照 --scale CSS 变量；transform-origin: left bottom） */
    transformOrigin: Item.BottomLeft
    scale: 1

    /* 显隐过渡：默认 opacity 0，show 翻转播放 0.35s 淡入（照 scss） */
    opacity: shrinkBar.show ? 1 : 0

    Behavior on opacity {
        NumberAnimation { duration: 350; easing.type: Easing.Linear }
    }

    // ---------------------------------------------------------------------
    // 挂载后自动翻显示态（照 web 的双 rAF 后 scheduleShow：先以 opacity 0
    // 渲染一帧，再翻 show 播放 0.35s 淡入过渡 —— 同帧赋值不触发过渡）
    // ---------------------------------------------------------------------

    Timer {
        interval: 16
        running: true
        repeat: false
        onTriggered: shrinkBar.show = true
    }

    // ---------------------------------------------------------------------
    // 26px 进度环（top/left 2px，环色 #ffffffb3，中心 16x16 类型图标，
    //             不挂点击热区 —— 点击交互由条目根整体承接）
    // ---------------------------------------------------------------------

    CloseCircle {
        x: 2
        y: 2
        ringSize: 26
        ringColor: "#ffffffb3"
        centerIcon: shrinkBar.iconSource
        interactive: false
        remaining: shrinkBar.remaining
    }

    // ---------------------------------------------------------------------
    // 标题（x 36 起，行高 30 垂直居中，白字 12px）
    // ---------------------------------------------------------------------

    Text {
        id: shrinkTitleText

        x: 36
        width: implicitWidth
        height: parent.height
        verticalAlignment: Text.AlignVCenter
        text: shrinkBar.title
        color: "#ffffff"
        font.pixelSize: 12
    }

    // ---------------------------------------------------------------------
    // 点击热区（整条 —— 点击任意区域重开原卡片）
    // ---------------------------------------------------------------------

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: shrinkBar.clicked()
    }
}
