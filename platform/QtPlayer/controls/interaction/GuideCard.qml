// ===========================================================================
// 点赞关注卡片（照 hili-player GuideCard.ts + interaction.scss 的 guide 段复刻）
//
// DOM 结构（参考）：
//   .nova-danmaku-x-editor                        —— 卡片根（36/45 高，圆角 8）
//     .nova-danmaku-x-guide-three                 —— 三连区（142x36，padding 0 3px）
//       span.like / span.coin / span.collect      —— 三个图标格（44x38，padding 8 12.5）
//     .nova-danmaku-x-guide-follow                —— 关注钮（66x24，margin 0 6px）
//       .-follow-0（plus 图标 + "关注"）/ .-follow-1（"已关注"）
//
// 视觉规格（normal / large 档，large = 宽>1366.9 且全屏的媒体查询）：
//   根高 36 / 45；背景 rgba(24,25,28,0.8)；圆角 8
//   三连区 142x36 / 177.5x45；图标格 44x38 / 56.25x45（padding 8 12.5 / 11 10.5
//   → 图标显示区 19x22 / 35.25x23，照 CSS 的 svg 100% 填满）
//   关注钮 66x24 / 82.5x30，margin 0 6px，圆角 4，背景主色，字 14px，plus 16x16
//
// 交互状态（照 GuideCardState）：
//   likeActive 默认 true（到点默认"已点赞"）；coin/collect/following 默认 false
//   点击三个图标格切换激活态（激活 = 图标染主色，对应 CSS 的 .is_active fill）
//   关注钮 no-follow / following 互斥切换
//
// type 变体：type=2 隐藏关注钮、type=3 隐藏三连区（照根元素的变体类）
// 主色：QtPlayerTheme.playerAccent（QtPlayer 是粉色系主题，对应参考的
//   --nova-player-primary-color(#00aeec)，不硬编码蓝色）
//
// 显隐 / 关闭环 / 时间窗都由 InteractionLayer 的 delegate 管，本组件只管内容。
// ===========================================================================
import QtQuick
import QtQuick.Effects
import QtPlayer

Rectangle {
    id: guide

    // ---------------------------------------------------------------------
    // 对外接口
    // ---------------------------------------------------------------------

    /* 卡片数据：{ type, top, left, timeStart, timeEnd }（type 1/2/3 决定变体） */
    property var cardData: ({})

    /* 大尺寸档（宽>1366.9 且全屏），由 InteractionLayer 喂进来 */
    property bool large: false

    // ---------------------------------------------------------------------
    // 交互状态（照 GuideCardState；跨卡片显隐保留 —— delegate 不销毁它们就不丢）
    // ---------------------------------------------------------------------

    property bool likeActive: true       /* 默认已点赞（对齐参考渲染行为） */
    property bool coinActive: false
    property bool collectActive: false
    property bool following: false

    /* 状态持久化协议（InteractionLayer 接线）：编辑 API 重建 delegate 后
       状态按 stateKey 从外置表找回（照参考"交互状态按 listIndex 存 Map"） */
    property string stateKey: ""
    signal stateMutated(var snapshot)
    function restoreState(snapshot) {
        if (snapshot === null || snapshot === undefined)
            return
        if (snapshot.likeActive !== undefined)
            guide.likeActive = snapshot.likeActive
        if (snapshot.coinActive !== undefined)
            guide.coinActive = snapshot.coinActive
        if (snapshot.collectActive !== undefined)
            guide.collectActive = snapshot.collectActive
        if (snapshot.following !== undefined)
            guide.following = snapshot.following
    }
    function emitState() {
        guide.stateMutated({
            likeActive: guide.likeActive,
            coinActive: guide.coinActive,
            collectActive: guide.collectActive,
            following: guide.following
        })
    }
    onLikeActiveChanged: guide.emitState()
    onCoinActiveChanged: guide.emitState()
    onCollectActiveChanged: guide.emitState()
    onFollowingChanged: guide.emitState()

    /* 卡片根背景：rgba(24,25,28,0.8)（#18191C @ 80%） */
    color: "#CC18191C"
    radius: 8

    /* 三连区可见性（type=3 隐藏三连）与关注钮可见性（type=2 隐藏关注） */
    readonly property bool showThree: guide.cardData.type !== 3
    readonly property bool showFollow: guide.cardData.type !== 2

    /* 内容总宽（width:auto 的等价算式）：三连 142 + 关注(66+左右 margin 12) */
    readonly property real threeWidth: guide.large ? 177.5 : 142
    readonly property real followWidth: guide.large ? 82.5 : 66
    readonly property real followHeight: guide.large ? 30 : 24
    readonly property real cellWidth: guide.large ? 56.25 : 44
    readonly property real cellHeight: guide.large ? 45 : 38
    /* 图标显示区（span padding 8 12.5 / 11 10.5 之内） */
    readonly property real iconWidth: guide.large ? 35.25 : 19
    readonly property real iconHeight: guide.large ? 23 : 22

    implicitWidth: (guide.showThree ? guide.threeWidth : 0)
                   + (guide.showFollow ? guide.followWidth + 12 : 0)
    implicitHeight: guide.large ? 45 : 36
    width: implicitWidth
    height: implicitHeight

    // ---------------------------------------------------------------------
    // 内容行：三连区 + 关注钮（照参考的 flex 居中排布）
    // ---------------------------------------------------------------------

    Row {
        anchors.centerIn: parent
        spacing: 0

        /* ---- 三连区（142x36，padding 0 3px → 用锚点居中实现同样的留白）---- */
        Item {
            width: guide.threeWidth
            height: guide.height
            visible: guide.showThree

            Row {
                anchors.centerIn: parent

                GuideCell {
                    cellWidth: guide.cellWidth
                    cellHeight: guide.cellHeight
                    iconWidth: guide.iconWidth
                    iconHeight: guide.iconHeight
                    iconSource: "../../assets/images/interaction/guide-like.svg"
                    active: guide.likeActive
                    onToggled: guide.likeActive = !guide.likeActive
                }

                GuideCell {
                    cellWidth: guide.cellWidth
                    cellHeight: guide.cellHeight
                    iconWidth: guide.iconWidth
                    iconHeight: guide.iconHeight
                    iconSource: "../../assets/images/interaction/guide-coin.svg"
                    active: guide.coinActive
                    onToggled: guide.coinActive = !guide.coinActive
                }

                GuideCell {
                    cellWidth: guide.cellWidth
                    cellHeight: guide.cellHeight
                    iconWidth: guide.iconWidth
                    iconHeight: guide.iconHeight
                    iconSource: "../../assets/images/interaction/guide-collect.svg"
                    active: guide.collectActive
                    onToggled: guide.collectActive = !guide.collectActive
                }
            }
        }

        /* ---- 关注钮（66x24，margin 0 6px → 左右各 6 的留白）---- */
        Item {
            width: guide.followWidth + 12
            height: guide.height
            visible: guide.showFollow

            Rectangle {
                id: followButton

                anchors.verticalCenter: parent.verticalCenter
                anchors.horizontalCenter: parent.horizontalCenter
                width: guide.followWidth
                height: guide.followHeight
                radius: 4

                /* 背景主色（参考 var(--nova-player-primary-color)） */
                color: QtPlayerTheme.playerAccent

                /* 未关注：plus 图标 + "关注"；已关注：只显示"已关注"（互斥） */
                Item {
                    anchors.centerIn: parent
                    width: followLabel.x + followLabel.width
                    height: parent.height
                    visible: !guide.following

                    Image {
                        id: plusIcon

                        anchors.verticalCenter: parent.verticalCenter
                        width: 16
                        height: 16
                        source: "../../assets/images/interaction/guide-plus.svg"
                        sourceSize: Qt.size(16, 16)
                        smooth: true
                        visible: false
                    }

                    /* plus 图标白色（按钮背景已经是主色，图标保持白色） */
                    MultiEffect {
                        anchors.fill: plusIcon
                        source: plusIcon
                        colorization: 1
                        colorizationColor: "#ffffff"
                    }

                    Text {
                        id: followLabel

                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: plusIcon.right
                        anchors.leftMargin: 0
                        text: qsTr("关注")
                        color: "#ffffff"
                        font.pixelSize: 14
                    }
                }

                Text {
                    anchors.centerIn: parent
                    visible: guide.following
                    text: qsTr("已关注")
                    color: "#ffffff"
                    font.pixelSize: 14
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: guide.following = !guide.following
                }
            }
        }
    }

    // ---------------------------------------------------------------------
    // 三连区的一个图标格（内联子组件：44x38 的点击格 + 染色图标）
    // ---------------------------------------------------------------------

    component GuideCell: Item {
        id: cell

        property real cellWidth: 44
        property real cellHeight: 38
        property real iconWidth: 19
        property real iconHeight: 22
        property string iconSource: ""
        property bool active: false

        signal toggled()

        width: cell.cellWidth
        height: cell.cellHeight

        Image {
            id: cellIcon

            anchors.centerIn: parent
            width: cell.iconWidth
            height: cell.iconHeight
            source: cell.iconSource
            sourceSize: Qt.size(cell.iconWidth, cell.iconHeight)
            smooth: true
            fillMode: Image.PreserveAspectFit
            visible: false
        }

        /* 白底 SVG + MultiEffect 整颗着色：默认白、激活染主色（照 .is_active 的 fill） */
        MultiEffect {
            anchors.fill: cellIcon
            source: cellIcon
            colorization: 1
            colorizationColor: cell.active ? QtPlayerTheme.playerAccent : "#ffffff"
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: cell.toggled()
        }
    }
}
