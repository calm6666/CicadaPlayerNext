// ===========================================================================
// 评分卡片（照 hili-player ScoreCard.ts + interaction.scss 的 score 段复刻）
//
// DOM 结构（参考）：
//   .nova-danmaku-x-score                    —— 卡片根（206x62 / 226x66，圆角 8）
//   .nova-danmaku-x-circle                   —— 右上角关闭环（CloseCircle.qml）
//   .nova-danmaku-x-score-title              —— 标题（margin 10 0 4 10，高 12）
//   .nova-danmaku-x-score-area               —— 评分区（5 项横排，style color = 主题色）
//     .-area-item × 5                        —— 每项：图标 img（18x18，margin 7 11.5）
//                                                + 数值气泡 span（33x33，bottom 68）
//   .nova-danmaku-x-score-result             —— "平均 N"（right 10 top 34，span 20px，
//                                                0.17s 过渡 + 0.6s 延迟）
//   .nova-danmaku-x-score-count              —— "N人参与"（right 10 top 10，opacity .7）
//
// 【图片化图标（照 web 端定稿）】图标全部换为图片资源（描边版 off / 填充版 on）：
//   星星 five-pointed-star(-fill).png / 爱心 heart(-fill).png / 柠檬 lemon(-fill).png，
//   按评分类型（scoreType 1/2/3）映射；填充判定 = 悬停联动填充数
//   （hoverIndex > 0 ? hoverIndex : selected），项下标 < 填充数即填充版。
//
// 【悬停联动（B 站语义）】悬停某项：该项图标放大 1.3（0.2s 过渡），
//   该项及之前的全部图标切换为填充版；移开恢复；已评分后不再响应
//   （has-result 态评分区禁用指针）。
//
// 【主题色】星星 #ffb027 / 爱心 #FA5555 / 柠檬 #F7DC6F（与填充版图标主色一致）：
//   数值气泡数字 / 结果平均分大字的颜色都随图标类型切换。
//
// 【已评后收拢（has-result）】项依次左移收拢（translateX 0/-20/-40/-60/-80px，
//   延迟 0.2/0.3/0.4/0.5/0.6s，过渡 0.1s），结果区淡入。
//
// 【数值气泡（照 web 定稿）】悬停某项 → 该项及之前的全部气泡显示
// （hoverIndex >= value 显隐）；点击评分后 hoverIndex 清零 → 气泡即刻
// 全部消失（不常显）；已评后评分区禁用指针，气泡不再出现。
//
// 【悬停热区】评分区单一 MouseArea + mouseX 位置换算（不用每项独立
// MouseArea —— 相邻区域平行移动时 enter/exit 事件不可靠，用户实测
// 平移到相邻项不触发，必须移出再进）。
//
// 交互状态（照 ScoreCardState）：selected（0 = 未评）、count、totalScore、hoverIndex；
//   点击第 value 项 → selected = value、count+1、totalScore+value*2
//   （10 分制口径：图标数字 1-5，总分翻倍累计 —— 照 web onScoreSelect，
//   与参考 DOM「评 3 分 → 平均 6.0」一致）；
//   已评后不再响应（渲染层兜底）。
//   平均分 = totalScore/count 保留一位小数（count 0 时显示 "0"）。
// ===========================================================================
import QtQuick
import QtPlayer

Rectangle {
    id: score

    // ---------------------------------------------------------------------
    // 对外接口
    // ---------------------------------------------------------------------

    /* 卡片数据：{ top, left, title, scoreType(1星/2心/3柠), timeStart, timeEnd } */
    property var cardData: ({})

    /* 大尺寸档（宽>1366.9） */
    property bool large: false

    /* 卡片剩余显示进度（喂给关闭环） */
    property real remaining: 1

    signal closeRequested()

    // ---------------------------------------------------------------------
    // 交互状态（照 ScoreCardState；delegate 不销毁 → 跨显隐保留）
    // ---------------------------------------------------------------------

    /* 选中的评分值（0 = 未评分，1-5 = 已评分） */
    property int selected: 0
    /* 参与人数 */
    property int count: 0
    /* 总分和（平均分 = totalScore / count） */
    property int totalScore: 0
    /* 悬停高亮的评分值（0 = 无悬停；悬停项及之前的图标填充） */
    property int hoverIndex: 0

    /* 状态持久化协议（InteractionLayer 接线）：编辑 API 重建 delegate 后
       状态按 stateKey 从外置表找回（照参考"交互状态按 listIndex 存 Map"） */
    property string stateKey: ""
    signal stateMutated(var snapshot)
    function restoreState(snapshot) {
        if (snapshot === null || snapshot === undefined)
            return
        if (snapshot.selected !== undefined)
            score.selected = snapshot.selected
        if (snapshot.count !== undefined)
            score.count = snapshot.count
        if (snapshot.totalScore !== undefined)
            score.totalScore = snapshot.totalScore
    }
    function emitState() {
        score.stateMutated({
            selected: score.selected,
            count: score.count,
            totalScore: score.totalScore
        })
    }
    onSelectedChanged: {
        /* 评分瞬间清掉悬停值（MouseArea 已禁用，exited 不保证触发，
           避免填充数残留悬停项 —— 照 web 端"已评后 hover 不再响应"） */
        if (score.selected > 0)
            score.hoverIndex = 0
        score.emitState()
    }
    onCountChanged: score.emitState()
    onTotalScoreChanged: score.emitState()

    // ---------------------------------------------------------------------
    // 图标对 / 主题色（照 icons.ts 的 getScoreIconPair / getScoreThemeColor）
    // ---------------------------------------------------------------------

    /* 图标图片对（on = 填充版 / off = 描边版；1 = 星星，2 = 爱心，3 = 柠檬） */
    readonly property var iconPair: {
        if (score.cardData.scoreType === 2)
            return { on: "../../assets/images/interaction/heart-fill.png",
                     off: "../../assets/images/interaction/heart.png" }
        if (score.cardData.scoreType === 3)
            return { on: "../../assets/images/interaction/lemon-fill.png",
                     off: "../../assets/images/interaction/lemon.png" }
        return { on: "../../assets/images/interaction/five-pointed-star-fill.png",
                 off: "../../assets/images/interaction/five-pointed-star.png" }
    }

    /* 主题色：数字 / 结果平均分文本颜色（与填充版图标主色一致） */
    readonly property color themeColor: {
        if (score.cardData.scoreType === 2)
            return "#FA5555"
        if (score.cardData.scoreType === 3)
            return "#F7DC6F"
        return "#ffb027"
    }

    /* 悬停联动填充数：悬停值（>0 时）或已选值（照 web 端定稿公式） */
    readonly property int fillCount: score.hoverIndex > 0 ? score.hoverIndex : score.selected

    /* 尺寸规格（normal / large）：图标 18x18 margin 7/11.5 → 20x20 margin 8/12.5 */
    readonly property real itemIconSize: score.large ? 20 : 18
    readonly property real itemWidth: score.large ? 45 : 41.5
    readonly property real itemHeight: score.large ? 36 : 32

    /* rgba(33,33,33,0.8)（#212121 @ 80%） */
    color: "#CC212121"
    radius: 8

    implicitWidth: score.large ? 226 : 206
    implicitHeight: score.large ? 66 : 62
    width: implicitWidth
    height: implicitHeight

    // ---------------------------------------------------------------------
    // 右上角关闭环
    // ---------------------------------------------------------------------

    CloseCircle {
        anchors.right: parent.right
        anchors.rightMargin: -13
        anchors.top: parent.top
        anchors.topMargin: -13
        remaining: score.remaining
        onClicked: score.closeRequested()
    }

    // ---------------------------------------------------------------------
    // 标题（margin 10 0 4 10，高 12）
    // ---------------------------------------------------------------------

    Text {
        x: 10
        y: 10
        width: parent.width - 20 - 60      /* 右侧给"N人参与"留位 */
        height: 12
        text: (score.cardData.title !== undefined) ? score.cardData.title : ""
        color: "#ffffff"
        font.pixelSize: 12
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }

    /* 参与人数（right 10 top 10，opacity 0.7） */
    Text {
        anchors.right: parent.right
        anchors.rightMargin: 10
        y: 10
        text: score.count + qsTr("人参与")
        color: "#ffffff"
        font.pixelSize: 12
        opacity: 0.7
    }

    // ---------------------------------------------------------------------
    // 评分区（5 项横排；区顶 = 标题底 + 4 的 margin）
    // 已评后收拢（has-result）：项依次左移（0/-20/-40/-60/-80px，错峰延迟
    // 0.2-0.6s，过渡 0.1s）—— 照 .nova-danmaku-x-score.nova-danmaku-x-has-result
    // ---------------------------------------------------------------------

    Item {
        id: scoreArea

        x: (parent.width - 5 * score.itemWidth) / 2
        y: 10 + 12 + 4
        width: 5 * score.itemWidth
        height: score.itemHeight

        Repeater {
            model: 5

            delegate: Item {
                id: scoreItem

                /* 本项分值 1-5（index 0-4） */
                readonly property int value: index + 1
                /* 填充态：项下标 < 填充数（悬停联动 / 已选值） */
                readonly property bool filled: index < score.fillCount
                /* 悬停放大（仅未评分时响应 —— 已评后 has-result 禁用整区指针） */
                readonly property bool hovered: score.hoverIndex === scoreItem.value

                /* 收拢偏移：web scss 的 translateX(0/-20/-40/-60/-80px) 是叠加在
                   flex 布局位（index*itemWidth）上的**相对位移**，不是绝对负坐标：
                   未评分 → 各项按 itemWidth 等距铺开；
                   已评分 → 在原位基础上左移 20*index（项2 从 41.5 → 21.5，而非 -20） */
                x: index * score.itemWidth + (score.selected > 0 ? -20 * index : 0)

                /* 收拢动画：错峰延迟 0.2 + index*0.1 秒 + 0.1s 过渡（照 scss） */
                Behavior on x {
                    enabled: score.selected > 0
                    SequentialAnimation {
                        PauseAnimation { duration: 200 + index * 100 }
                        NumberAnimation { duration: 100; easing.type: Easing.Linear }
                    }
                }

                width: score.itemWidth
                height: score.itemHeight

                /* 图标图片（18x18 / 20x20；填充版 / 描边版随填充态切换；
                   悬停放大 1.3 + 0.2s 过渡照 scss 的 :hover transform） */
                Image {
                    anchors.centerIn: parent
                    width: score.itemIconSize
                    height: score.itemIconSize
                    source: scoreItem.filled ? score.iconPair.on : score.iconPair.off
                    sourceSize: Qt.size(score.itemIconSize, score.itemIconSize)
                    smooth: true
                    fillMode: Image.PreserveAspectFit
                    /* 悬停放大 1.3（transformOrigin 默认中心，照 CSS scale） */
                    scale: scoreItem.hovered ? 1.3 : 1

                    Behavior on scale {
                        NumberAnimation { duration: 200; easing.type: Easing.Linear }
                    }
                }

                /* 数值气泡（33x33、字 20px、背景 rgba(0,0,0,.6)、圆角 2、
                   margin-left 5、bottom 68 = 浮在卡片上方；
                   悬停联动显示：悬停该项及之前的全部气泡（照 web 定稿
                   hoverIndex >= value 的显隐协议）；点击评分后 hoverIndex
                   清零 → 气泡即刻全部消失（不再常显）；
                   数字颜色 = 主题色（继承评分区的 color，随图标类型切换） */
                Rectangle {
                    x: 5
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 68
                    visible: score.hoverIndex > 0
                             && score.hoverIndex >= scoreItem.value
                    width: 33
                    height: 33
                    radius: 2
                    color: "#99000000"      /* rgba(0,0,0,.6) */

                    Text {
                        anchors.centerIn: parent
                        text: scoreItem.value
                        color: score.themeColor
                        font.pixelSize: 20
                    }
                }
            }
        }

        /* -----------------------------------------------------------------
         * 单一悬停/点击热区（覆盖整个评分区，按位置换算悬停值）：
         * 【为什么不用每项独立 MouseArea】相邻 MouseArea 间平行移动时
         * exited/entered 事件在共享边界不可靠 —— 用户实测"从卡片外移入
         * 首项可触发，平移到相邻项不触发，必须移出再进"。单一热区 +
         * mouseX 换算（floor(x/itemWidth)+1）与 web 端 CSS :hover 的
         * 连续悬停语义一致，任何平行移动都即时响应。
         * 已评分后禁用（照 has-result 的 pointer-events:none）。
         * ----------------------------------------------------------------- */
        MouseArea {
            id: scoreHoverArea

            anchors.fill: parent
            enabled: score.selected === 0
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor

            /* 位置 → 悬停值换算（1-5；越界/离开 0 = 无悬停） */
            function hoverValueAt(x) {
                if (x < 0 || x >= scoreArea.width)
                    return 0
                var idx = Math.floor(x / score.itemWidth)
                return (idx < 0 || idx > 4) ? 0 : idx + 1
            }

            /* 移动（含进入）即时换算悬停值 —— 联动填充 + 气泡显示 */
            onPositionChanged: function (mouse) {
                score.hoverIndex = scoreHoverArea.hoverValueAt(mouse.x)
            }

            /* 离开评分区：悬停归零（恢复已选值的填充态 / 气泡全隐） */
            onExited: score.hoverIndex = 0

            /* 点击按位置算分值；总分按 10 分制累计（图标数字 1-5，
               评分值 ×2 入账 —— 照 web onScoreSelect 的 totalScore + value*2，
               平均分口径与参考 DOM「评 3 分 → 平均 6.0」一致） */
            onClicked: function (mouse) {
                var value = scoreHoverArea.hoverValueAt(mouse.x)
                if (value === 0)
                    return
                score.selected = value
                score.count = score.count + 1
                score.totalScore = score.totalScore + value * 2
            }
        }
    }

    // ---------------------------------------------------------------------
    // 平均分（right 10 top 34，行高 20；"平均 " + 大字分数；
    //         0.17s 淡入 + 0.6s 延迟 —— 用 PauseAnimation 实现延迟；
    //         分数颜色 = 主题色（与图标填充色一致，照 web 端定稿））
    // ---------------------------------------------------------------------

    Item {
        anchors.right: parent.right
        anchors.rightMargin: 10
        y: 34
        width: scoreResultLabel.width + scoreResultValue.width
        height: 20

        opacity: score.selected > 0 ? 1 : 0

        Behavior on opacity {
            SequentialAnimation {
                PauseAnimation { duration: 600 }
                NumberAnimation { duration: 170; easing.type: Easing.Linear }
            }
        }

        Text {
            id: scoreResultLabel

            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("平均 ")
            color: "#B3FFFFFF"       /* rgba(255,255,255,.7) */
            font.pixelSize: 12
        }

        Text {
            id: scoreResultValue

            anchors.left: scoreResultLabel.right
            anchors.verticalCenter: parent.verticalCenter
            /* count > 0 时保留一位小数，否则 "0"（照参考的响应式文本） */
            text: score.count > 0 ? (score.totalScore / score.count).toFixed(1) : "0"
            color: score.themeColor
            font.pixelSize: 20
        }
    }
}
