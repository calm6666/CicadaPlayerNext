// ===========================================================================
// 桌面端列表滚动条 —— 主流的"滚动时出现、停止后淡出"半透明灰条
//
// 【为什么单独一个组件】弹幕列表面板和首页卡片网格都要同一种滚动条
// （用户要求：滚动条不要突兀的强调色、鼠标滚动时才显示、样式符合主流桌面），
// 两处内联重复又长又容易改丢一处，抽成组件一份写两处用。
//
// 显隐协议（主流桌面行为）：
//   * 默认隐藏（opacity 0）；
//   * 滚动时调用 kick()（WheelHandler 手动滚动那侧接线）→ 出现，
//     800ms 内没有新滚动就淡出（180ms 淡出动画）；
//   * 鼠标悬停在滚动条上（要拖它）→ 常显不淡出。
//
// 外观（中性灰，不挑主题色、不花哨）：
//   * 6px 宽圆角细条，无背景槽（background: null —— 主流桌面滚动条
//     不占内容宽、不留一条突兀的轨道底色）；
//   * 滑块 #8a9099（冷灰）35% 半透明：深浅主题下都只是"一条淡灰"，
//     hover 58%、按下 78% 逐级加深 —— 参考 Windows 11 / 各主流桌面应用。
//
// 用法（配 ListView/GridView 的 interactive: false + WheelHandler 手动滚动，
// 详见两处调用点的注释）：
//   ScrollBar.vertical: TransientScrollBar { id: listScrollBar }
//   WheelHandler { onWheel: (ev) => { ...滚动...; listScrollBar.kick() } }
// ===========================================================================
import QtQuick
import QtQuick.Controls

ScrollBar {
    id: transientBar

    /* 滚动发生时由宿主列表调用：出现并重置淡出计时 */
    function kick() {
        fadeTimer.restart()
    }

    /* 显隐：hover 在滚动条上（要拖）常显；否则跟最近一次滚动的计时走 */
    opacity: (transientBar.hovered || fadeTimer.running) ? 1 : 0

    Behavior on opacity {
        NumberAnimation {
            duration: 180
            easing.type: Easing.OutQuad
        }
    }

    /* 停止滚动 800ms 后淡出（主流量级：既能看到反馈，又不常驻碍眼） */
    Timer {
        id: fadeTimer

        interval: 800
        repeat: false
    }

    /* 无背景槽：不占视觉、不留轨道底色（见文件头说明） */
    background: null

    /* 滑块本体：中性冷灰三档（常态 35% / hover 58% / 按下 78%） */
    contentItem: Rectangle {
        implicitWidth: 6
        radius: 3
        color: transientBar.pressed ? "#C78A9099"
               : (transientBar.hovered ? "#948A9099" : "#598A9099")
    }
}
