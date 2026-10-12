// ===========================================================================
// 评分卡片（照 hili-player ScoreCard.ts + interaction.scss 的 score 段复刻）
//
// DOM 结构（参考）：
//   .nova-danmaku-x-score                    —— 卡片根（206x62 / 226x66，圆角 8）
//   .nova-danmaku-x-circle                   —— 右上角关闭环（CloseCircle.qml）
//   .nova-danmaku-x-score-title              —— 标题（margin 10 0 4 10，高 12）
//   .nova-danmaku-x-score-area               —— 评分区（5 项横排）
//     .-area-item × 5                        —— 每项：图标（28x28，margin 0 6.5）
//                                                + 数值气泡 span（33x33，bottom 68）
//   .nova-danmaku-x-score-result             —— "平均 N"（right 10 top 34，span 20px，
//                                                0.17s 过渡 + 0.6s 延迟）
//   .nova-danmaku-x-score-count              —— "N人参与"（right 10 top 10，opacity .7）
//
// 交互状态（照 ScoreCardState）：selected（0 = 未评）、count、totalScore；
//   点击第 value 项 → selected = value、count+1、totalScore+value；
//   已评后不再响应（渲染层兜底）。
//   激活态 = 选中值及以下的项染主色（.-area-item-active 的 svg path fill 主色）；
//   数值气泡 voted 前不显示、voted 后 block（显示各项分值 1-5，浮在卡片上方）。
//   平均分 = totalScore/count 保留一位小数（count 0 时显示 "0"）。
// ===========================================================================
import QtQuick
import QtQuick.Effects
import QtPlayer

Rectangle {
    id: score

    // ---------------------------------------------------------------------
    // 对外接口
    // ---------------------------------------------------------------------

    /* 卡片数据：{ top, left, title, scoreType(1星/2心/3柠), timeStart, timeEnd } */
    property var cardData: ({})

    /* 大尺寸档（宽>1366.9 且全屏） */
    property bool large: false

    /* 卡片剩余显示进度（喂给关闭环） */
    property real remaining: 1

    signal closeRequested()

    // ---------------------------------------------------------------------
    // 交互状态（照 ScoreCardState；delegate 不销毁 → 跨显隐保留）
    // ---------------------------------------------------------------------

    property int selected: 0
    property int count: 0
    property int totalScore: 0

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
    onSelectedChanged: score.emitState()
    onCountChanged: score.emitState()
    onTotalScoreChanged: score.emitState()

    /* 图标源（1 = 星星、2 = 爱心、3 = 柠檬，默认星星 —— 照 renderScoreIcon） */
    readonly property string iconSource: {
        if (score.cardData.scoreType === 2)
            return "../../assets/images/interaction/score-love.svg"
        if (score.cardData.scoreType === 3)
            return "../../assets/images/interaction/score-lemon.svg"
        return "../../assets/images/interaction/score-star.svg"
    }

    /* 尺寸规格（normal / large） */
    readonly property real itemIconSize: score.large ? 32 : 28
    /* 评分项宽 = 图标 + 左右 margin 6.5（横排总宽 = 5 * (28+13) = 205 ≈ 卡片内宽） */
    readonly property real itemWidth: score.itemIconSize + 13

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
    // ---------------------------------------------------------------------

    Row {
        x: (parent.width - 5 * score.itemWidth) / 2
        y: 10 + 12 + 4
        spacing: 0

        Repeater {
            model: 5

            delegate: Item {
                id: scoreItem

                /* 本项分值 1-5（index 0-4） */
                readonly property int value: index + 1
                /* 激活 = 选中值及以下（照 -active 类的判定） */
                readonly property bool active: scoreItem.value <= score.selected

                width: score.itemWidth
                height: score.itemIconSize

                /* 图标（28x28 / 32x32；白 → 激活染主色；0.2s 过渡照 svg transition） */
                Image {
                    id: scoreIcon

                    anchors.centerIn: parent
                    width: score.itemIconSize
                    height: score.itemIconSize
                    source: score.iconSource
                    sourceSize: Qt.size(score.itemIconSize, score.itemIconSize)
                    smooth: true
                    visible: false
                }

                MultiEffect {
                    anchors.fill: scoreIcon
                    source: scoreIcon
                    colorization: 1
                    colorizationColor: scoreItem.active ? QtPlayerTheme.playerAccent
                                                         : "#ffffff"
                }

                /* 数值气泡（33x33、字 20px、背景 rgba(0,0,0,.6)、圆角 2、
                   margin-left 5、bottom 68 = 浮在卡片上方；voted 前不显示） */
                Rectangle {
                    x: 5
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 68
                    visible: score.selected > 0
                    width: 33
                    height: 33
                    radius: 2
                    color: "#99000000"      /* rgba(0,0,0,.6) */

                    Text {
                        anchors.centerIn: parent
                        text: scoreItem.value
                        color: "#ffffff"
                        font.pixelSize: 20
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    /* 已评分后不再响应（照参考渲染层的兜底） */
                    onClicked: {
                        if (score.selected > 0)
                            return
                        score.selected = scoreItem.value
                        score.count = score.count + 1
                        score.totalScore = score.totalScore + scoreItem.value
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------------
    // 平均分（right 10 top 34，行高 20；"平均 " + 大字分数；
    //         0.17s 淡入 + 0.6s 延迟 —— 用 PauseAnimation 实现延迟）
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
            color: "#B3FFFFFF"
            font.pixelSize: 20
        }
    }
}
