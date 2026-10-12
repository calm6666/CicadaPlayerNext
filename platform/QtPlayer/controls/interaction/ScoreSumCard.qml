// ===========================================================================
// 评分总结卡片（照 hili-player ScoreSumCard.ts + interaction.scss 的 scoreSum 段复刻）
//
// DOM 结构（参考）：
//   .nova-danmaku-x-scoreSum              —— 卡片根（203px 宽，rgba(33,33,33,.8)，
//                                             圆角 8px，缩放经 --scale CSS 变量协议）
//   .nova-danmaku-x-scoreSum-title        —— 空标题占位（margin 14 0 0 14，高 14）
//   .nova-danmaku-x-scoreSum-count        —— "N人参与"（absolute top 14 right 14，
//                                             opacity .7）
//   .nova-danmaku-x-scoreSum-area         —— 汇总区（padding 8px 14px，
//                                             pointer-events:none）
//     .-item × n                          —— 汇总行（高 24 + 底距 3，默认
//                                             opacity 0 + translateY(-10px)，
//                                             130ms 过渡；挂载后按序错峰显示）
//       .-item-name                       —— 题目（宽 150px）
//       .-item-count                      —— 平均分（absolute right 0，#ffb027）
//   .nova-danmaku-x-circle                —— 右上角关闭环（CloseCircle.qml）
//
// 数据（照 web 端定稿）：聚合值由 InteractionLayer 在条目创建时一次性计算注入
// （全部评分卡题目 + 当前平均分 toFixed(1) + 总参与人数；评分窗口与总结窗口
// 互斥，聚合值在进入窗口时计算一次即保持正确）。
// 汇总行错峰显示：行 i 在 100ms*(i+1) 后翻转显示态（上浮淡入 130ms）。
// ===========================================================================
import QtQuick
import QtPlayer

Rectangle {
    id: scoreSum

    // ---------------------------------------------------------------------
    // 对外接口
    // ---------------------------------------------------------------------

    /* 卡片数据：{ top, left, title, timeStart, timeEnd }（题目列表 / 平均分 /
       总人数由 summaryItems / totalCount 运行期聚合注入；title 可编辑 ——
       照 web InteractionScoreSum.title，缺省空占位保持参考 DOM 结构） */
    property var cardData: ({})

    /* 大尺寸档（保持 1 缩放 —— scss 大档媒体查询没有 scoreSum 规则） */
    property bool large: false

    /* 卡片剩余显示进度（喂给关闭环） */
    property real remaining: 1

    /* 聚合行：[{ name: 题目, average: "x.x" }]（创建条目时一次性算好） */
    property var summaryItems: []

    /* 总参与人数（全部评分卡参与数之和） */
    property int totalCount: 0

    signal closeRequested()

    /* rgba(33,33,33,0.8)（#212121 @ 80%） */
    color: "#CC212121"
    radius: 8

    /* 宽 203 / 高 = 标题区 28 + 汇总区上下内边距 16 + 每行 27（24 高 + 3 底距） */
    implicitWidth: 203
    implicitHeight: 28 + 16 + 27 * scoreSum.summaryItems.length
    width: implicitWidth
    height: implicitHeight

    /* 缩放协议（照 --scale CSS 变量，transform-origin center） */
    transformOrigin: Item.Center
    scale: 1

    // ---------------------------------------------------------------------
    // 右上角关闭环（同评分卡片）
    // ---------------------------------------------------------------------

    CloseCircle {
        anchors.right: parent.right
        anchors.rightMargin: -13
        anchors.top: parent.top
        anchors.topMargin: -13
        remaining: scoreSum.remaining
        onClicked: scoreSum.closeRequested()
    }

    // ---------------------------------------------------------------------
    // 标题（可编辑文本，margin 14 0 0 14 高 14 —— 缺省空占位保持参考
    // DOM 结构，标题区高度恒在；编辑走 updateCardContent 的 title 补丁）
    // ---------------------------------------------------------------------

    Text {
        x: 14
        y: 14
        width: parent.width - 28 - 70      /* 右侧给"N人参与"留位 */
        height: 14
        text: (scoreSum.cardData.title !== undefined)
              ? scoreSum.cardData.title : ""
        color: "#ffffff"
        font.pixelSize: 14
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }

    /* 总参与人数（absolute top 14 right 14，opacity 0.7，12px 白字） */
    Text {
        y: 14
        anchors.right: parent.right
        anchors.rightMargin: 14
        text: scoreSum.totalCount + qsTr("人参与")
        color: "#ffffff"
        font.pixelSize: 12
        opacity: 0.7
    }

    // ---------------------------------------------------------------------
    // 汇总区（padding 8px 14px；行错峰上浮淡入 —— 行 i 延迟 100ms*(i+1)）
    // ---------------------------------------------------------------------

    Column {
        x: 14
        y: 28 + 8
        spacing: 3

        Repeater {
            model: scoreSum.summaryItems

            delegate: Item {
                id: sumRow

                /* 行数据：{ name, average } */
                readonly property var rowData: modelData

                /* 错峰显示态：挂载后由行内 Timer 延迟翻转（照 web 端
                   itemShowSignals 的 100ms*(i+1) 错峰协议） */
                property bool shown: false

                /* 区内容宽 = 203 - 左右 padding 28 */
                width: 203 - 28
                height: 24

                /* 默认 opacity 0（上浮隐藏态），shown 后淡入（130ms 过渡） */
                opacity: sumRow.shown ? 1 : 0

                Behavior on opacity {
                    NumberAnimation { duration: 130; easing.type: Easing.Linear }
                }

                /* 行内容：默认 translateY(-10px)，shown 后上浮归位（130ms）；
                   y 放内层容器（外层 y 归 Column 布局管） */
                Item {
                    id: sumRowContent

                    anchors.fill: parent
                    y: sumRow.shown ? 0 : -10

                    Behavior on y {
                        NumberAnimation { duration: 130; easing.type: Easing.Linear }
                    }

                    /* 题目（宽 150，14px 白字） */
                    Text {
                        x: 0
                        width: 150
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        text: (sumRow.rowData !== undefined && sumRow.rowData.name !== undefined)
                              ? sumRow.rowData.name : ""
                        color: "#ffffff"
                        font.pixelSize: 14
                        elide: Text.ElideRight
                    }

                    /* 平均分（absolute right 0，#ffb027 —— 与星星填充主色一致） */
                    Text {
                        anchors.right: parent.right
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        text: (sumRow.rowData !== undefined && sumRow.rowData.average !== undefined)
                              ? sumRow.rowData.average : "0.0"
                        color: "#ffb027"
                        font.pixelSize: 14
                    }
                }

                /* 错峰定时器：行 i 在 100ms*(i+1) 后翻转显示态 */
                Timer {
                    interval: 100 * (index + 1)
                    running: true
                    repeat: false
                    onTriggered: sumRow.shown = true
                }
            }
        }
    }
}
