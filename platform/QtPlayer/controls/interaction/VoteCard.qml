// ===========================================================================
// 投票卡片（照 hili-player VoteCard.ts + interaction.scss 的 vote 段复刻）
//
// DOM 结构（参考）：
//   .nova-danmaku-x-vote                    —— 卡片根（padding 12 10 4，圆角 8）
//   .nova-danmaku-x-circle                  —— 右上角关闭环（CloseCircle.qml）
//   .nova-danmaku-x-vote-question           —— 问题（157x14 行高 14，省略号）
//   .nova-danmaku-x-vote-an × N             —— 选项行（157x26，margin 6 0）：
//       .-an-bg                              —— 底条（hsla(0,0%,100%,.2)，圆角 4）
//         .-an-bg-buffer                     —— 进度层（26px 起、宽 1s 过渡）：
//                                                未投票透明；投票后 rgba(255,255,255,.2)；
//                                                选中行 rgba(0,161,214,.6)（scss 的
//                                                .nova-danmaku-x-selected / -select 两条）
//       .-an-text                            —— 文本层（行高 26，padding-right 20）：
//         .-an-text-index（宽 10，字母下标）  —— 参考里被注释掉不显示
//         .-an-text-doc（100px，省略号）
//       .-an-percent                         —— 百分比角标（right 4，行高 26；
//                                                voted/selected 后 opacity 0.9）
//
// buffer 的圆角协议（照 scss + VoteCard.ts 的实现，逐像素复刻）：
//   * 宽度：未投票 = 26px（scss 默认 width），投票后 = 百分比宽（1s 过渡）；
//   * 圆角：非 100% = 左 4 / 右 0（进度条形状）；**到 100% 且宽度过渡结束（1s）后**
//     才变成四角 4 —— scss 的 `transition: width 1s, border-radius 0s 1s`
//     （0s 时长 + 1s 延迟 = 宽度动画完成的同一刻圆角瞬切；反向同样延迟 1s）。
//   实现用单个 Rectangle + 四角独立圆角（Qt 6.7+ 的 topLeftRadius 等
//   per-corner 属性，构建环境 Qt 6.11 支持）：左上/左下恒 4，右上/右下
//   在 100% 时变 4、经 PauseAnimation(1s) + 0ms 瞬切，与参考行为一致。
//   【不要退回"左段圆角 + 右段直角"两段合成】两段在 x∈[4,8] 重叠、且
//   fillColor 半透明 → 重叠区 alpha 双重合成变亮，左段右侧圆角还会透过
//   右段半透明色露出 → 非 100% 行右端出现"多余圆角小块、重叠格格不入"
//   （用户实测），单矩形从根上消除重叠。
//
// 交互状态（照 VoteCardState）：votedIndex（-1 = 未投）、totalVotes（0 起）、
//   extras（本会话投给各选项的额外票）；点击选项 → 该项 extra +1、totalVotes +1、
//   显示百分比（computePercent 四舍五入）；已投后不再响应（渲染层兜底，照参考）。
//   状态经 stateMutated/restoreState 协议外置到 InteractionLayer.persistStates
//   （编辑 API 全量重赋数据会重建 delegate，状态靠外置表找回，照参考的 Map 语义）。
// ===========================================================================
import QtQuick

Rectangle {
    id: vote

    // ---------------------------------------------------------------------
    // 对外接口
    // ---------------------------------------------------------------------

    /* 卡片数据：{ top, left, question, options: [{optionText, votes?}], timeStart, timeEnd } */
    property var cardData: ({})

    /* 大尺寸档（宽>1366.9 且全屏） */
    property bool large: false

    /* 卡片剩余显示进度（喂给关闭环） */
    property real remaining: 1

    signal closeRequested()

    // ---------------------------------------------------------------------
    // 交互状态（照 VoteCardState；外置持久化协议见文件头注释）
    // ---------------------------------------------------------------------

    property int votedIndex: -1
    property int totalVotes: 0
    /* 本会话里投给各选项的额外票（点击时该项置 1）—— 提升到卡片级，
       行 delegate 重建/恢复都只读写这一份 */
    property var extras: []

    /* 状态持久化协议（InteractionLayer 接线）：key 由 layer 喂 */
    property string stateKey: ""
    signal stateMutated(var snapshot)
    function restoreState(snapshot) {
        if (snapshot === null || snapshot === undefined)
            return
        vote.votedIndex = snapshot.votedIndex !== undefined ? snapshot.votedIndex : -1
        vote.totalVotes = snapshot.totalVotes !== undefined ? snapshot.totalVotes : 0
        vote.extras = (snapshot.extras !== undefined && snapshot.extras !== null)
                      ? snapshot.extras : []
    }
    function emitState() {
        vote.stateMutated({
            votedIndex: vote.votedIndex,
            totalVotes: vote.totalVotes,
            extras: vote.extras
        })
    }
    onVotedIndexChanged: vote.emitState()
    onTotalVotesChanged: vote.emitState()

    /* 选项列表（缺省空数组） */
    readonly property var options: (vote.cardData.options !== undefined
                                    && vote.cardData.options !== null)
                                   ? vote.cardData.options : []

    /* 尺寸规格（normal / large） */
    readonly property real rowWidth: vote.large ? 196.25 : 157
    readonly property real rowHeight: vote.large ? 32.5 : 26
    readonly property real questionHeight: vote.large ? 17.5 : 14
    readonly property int questionFontSize: vote.large ? 13 : 12
    readonly property int rowFontSize: vote.large ? 13 : 12
    readonly property real docWidth: vote.large ? 120 : 100

    /* rgba(24,25,28,0.8)（#18191C @ 80%） */
    color: "#CC18191C"
    radius: 8

    /* 高度算式（与拆开定位后的实际布局严格相等，推导见下方内容布局注释）：
       padding-top 12 + 问题高 + N*(行高+12) + 4 —— 展开即
       12 + 问题 + 6 + N*行高 + (N-1)*12 + 行下 margin 6 + padding-bottom 4 */
    implicitWidth: vote.rowWidth + 20          /* 左右 padding 10 + 10 */
    implicitHeight: 12 + vote.questionHeight
                    + vote.options.length * (vote.rowHeight + 12) + 4
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
        remaining: vote.remaining
        onClicked: vote.closeRequested()
    }

    // ---------------------------------------------------------------------
    // 内容列：问题 + 选项行（padding 12 10 4）
    // ---------------------------------------------------------------------

    /* 内容布局（padding 12 10 4；间距照 CSS margin 折叠精确摊派）：
       问题与首行之间 = max(问题下 margin 0, 行上 margin 6) = 6；
       行与行之间 = 6+6 = 12；最后行 → 卡片底 = 行下 margin 6 + padding 4 = 10。
       【为什么要拆开定位】之前问题也在 Column 里、spacing 统一 12 →
       问题→首行被撑到 12、底部只剩 4（用户实测"问题间隔太大、底部太窄被挤出"）。
       问题单独定位（y:12），选项列 y = 12+问题高+6，总高公式不变：
       12 + 14 + N*38 + 4  ⇔  12 + 14 + 6 + N*26 + (N-1)*12 + 6 + 4
       （多出的那个 6+6 摊进 N*38 里，两边严格相等）。 */

    /* 问题（157x14 行高 14，一行省略） */
    Text {
        x: 10
        y: 12
        width: vote.rowWidth
        height: vote.questionHeight
        text: (vote.cardData.question !== undefined) ? vote.cardData.question : ""
        color: "#ffffff"
        font.pixelSize: vote.questionFontSize
        elide: Text.ElideRight
    }

    /* 选项行（Repeater —— options 数量在数据里定死，参考里也是静态展开） */
    Column {
        x: 10
        y: 12 + vote.questionHeight + 6
        spacing: 12

        Repeater {
            model: vote.options

            delegate: Item {
                id: voteRow

                /* 该选项数据 */
                readonly property var option: modelData
                /* 该选项当前票数（数据自带 votes 或 0；投票后由 extras 接管增量） */
                readonly property int baseVotes: (voteRow.option.votes !== undefined)
                                                 ? voteRow.option.votes : 0
                /* 本会话里投给这一项的额外票（读卡片级 extras） */
                readonly property int extraVotes: (vote.extras[index] !== undefined)
                                                  ? vote.extras[index] : 0
                readonly property bool selected: vote.votedIndex === index
                /* 百分比（四舍五入，总票 0 时为 0 —— 照 computePercent；
                   钳到 100 —— 恢复过期的持久化状态时 totalVotes 可能小于
                   某项票数，超 100 会把 buffer 撑出行外、还会误触发全圆角） */
                readonly property int percent: vote.totalVotes > 0
                        ? Math.min(100, Math.round((voteRow.baseVotes + voteRow.extraVotes)
                                                   / vote.totalVotes * 100))
                        : 0

                width: vote.rowWidth
                height: vote.rowHeight

                /* 底条（hsla(0,0%,100%,.2) 圆角 4） */
                Rectangle {
                    id: rowBg

                    width: parent.width
                    height: vote.rowHeight
                    radius: 4
                    color: "#33FFFFFF"      /* hsla(0,0%,100%,.2) */

                    /* 进度层（.-an-bg-buffer）：**单个 Rectangle + 四角独立圆角**。
                       宽度 = 未投票 0（大档 32.5）/ 投票后百分比宽（1s 过渡）；
                       颜色 = 选中行 rgba(0,161,214,.6)、其余已投票 rgba(255,255,255,.2)、
                       未投票透明（scss 的 -select / -selected 两条规则）；
                       圆角 = 左上/左下恒 4，右上/右下非 100% 为 0、100% 且 1s 后变 4
                       （照 border-radius 0s 1s 的过渡协议，反向同样延迟 1s）。
                       【为什么必须是单矩形】之前用"左段 radius4 全圆角 + 右段直角"
                       两段合成"左圆右直"，两段在 x∈[4,8] 必然重叠且 fillColor 半透明
                       → 重叠区 alpha 双重合成变亮、左段右侧圆角透过右段半透明色露出
                       → 非 100% 行右端出现多余的圆角小块（用户实测"格格不入"）。
                       单矩形无重叠，左圆右直由 per-corner 属性直接表达
                       （Qt 6.7+，构建环境 6.11 支持）。 */
                    Rectangle {
                        id: rowBuffer

                        x: 0
                        y: 0
                        height: parent.height
                        /* 【基态宽度照 scss】未投票 = 小档 0 / 大档 32.5（scss 基态
                           width:0，大档媒体查询覆盖为 32.5px —— 均无背景、不可见，
                           只作为投票后 width 过渡的起点）；投票后 = 百分比宽。
                           之前误用行高（小档 26）当起点 → 投小百分比选项时动画
                           变成"先闪一个 26px 色块再收缩"（用户实测的圆角小块），
                           web 端是从 0 纯生长。 */
                        width: vote.votedIndex < 0
                               ? (vote.large ? vote.rowHeight : 0)
                               : parent.width * voteRow.percent / 100

                        readonly property color fillColor: voteRow.selected
                            ? "#9900A1D6"    /* rgba(0,161,214,.6)（-select 行） */
                            : (vote.votedIndex >= 0
                               ? "#33FFFFFF" /* rgba(255,255,255,.2)（-selected 行） */
                               : "#00000000") /* 未投票透明（照 scss 无背景） */

                        color: rowBuffer.fillColor

                        /* 左端两个圆角恒在（进度条左端形状） */
                        topLeftRadius: 4
                        bottomLeftRadius: 4

                        /* 右端：非 100% 直角；100% 且宽度过渡（1s）结束的同一刻
                           瞬切成圆角 —— PauseAnimation(1s) + 0ms 等价 scss 的
                           `border-radius 0s 1s`，反向变化同样延迟 1s */
                        topRightRadius: voteRow.percent >= 100 ? 4 : 0
                        bottomRightRadius: voteRow.percent >= 100 ? 4 : 0

                        Behavior on topRightRadius {
                            SequentialAnimation {
                                PauseAnimation { duration: 1000 }
                                NumberAnimation { duration: 0 }
                            }
                        }
                        Behavior on bottomRightRadius {
                            SequentialAnimation {
                                PauseAnimation { duration: 1000 }
                                NumberAnimation { duration: 0 }
                            }
                        }

                        Behavior on width {
                            NumberAnimation { duration: 1000; easing.type: Easing.Linear }
                        }
                    }
                }

                /* 文本层：字母下标（参考里注释掉不显示）+ 选项文案（100px 省略） */
                Row {
                    width: parent.width
                    height: vote.rowHeight

                    /* padding-right 20（给百分比角标让位） */
                    Item {
                        width: parent.width - 20
                        height: vote.rowHeight

                        Text {
                            x: 10          /* .-an-text-index 宽 10（内容不显示，占位对齐） */
                            width: parent.width - 10
                            text: (voteRow.option.optionText !== undefined)
                                  ? voteRow.option.optionText : ""
                            color: "#ffffff"
                            font.pixelSize: vote.rowFontSize
                            elide: Text.ElideRight
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                }

                /* 百分比角标（right 4；voted/selected 后 opacity 0.9 —— 照 scss 两条规则） */
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    height: vote.rowHeight
                    verticalAlignment: Text.AlignVCenter
                    text: vote.votedIndex < 0 ? "" : voteRow.percent + "%"
                    color: "#ffffff"
                    font.pixelSize: vote.rowFontSize
                    opacity: voteRow.selected ? 0.9 : 0

                    Behavior on opacity {
                        NumberAnimation { duration: 350; easing.type: Easing.Linear }
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    /* 已投票后不再响应（照参考渲染层的兜底）；
                       编辑模式下拖拽层（InteractionLayer 的 dragArea）在上，
                       这里收不到事件 —— 编辑优先，与参考 mousedown 独占一致 */
                    onClicked: {
                        if (vote.votedIndex >= 0)
                            return
                        var arr = vote.extras.slice()
                        arr[index] = 1
                        vote.extras = arr
                        vote.votedIndex = index
                        /* 【照 web index.ts 的 onVoteSelect】总票数 = 全部选项
                           票数之和（数据自带 votes + 本会话 extras），不是简单 +1 ——
                           mock 数据的选项自带 votes（比如 2），只 +1 会让分母远小于
                           分子 → 百分比 200%/300%、进度条撑出卡片（用户实测）。 */
                        var sum = 0
                        for (var i = 0; i < vote.options.length; i++) {
                            var o = vote.options[i]
                            sum += (o.votes !== undefined) ? o.votes : 0
                            sum += (vote.extras[i] !== undefined) ? vote.extras[i] : 0
                        }
                        vote.totalVotes = sum
                        vote.emitState()
                    }
                }
            }
        }
    }
}
