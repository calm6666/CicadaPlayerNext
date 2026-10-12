// ===========================================================================
// 交互卡片 API 面板 —— 右栏「交互卡片」Tab 的内容
// （照 hili-player demo 的编辑面板复刻：列表 + 选中编辑 + 添加 + 删除）
//
// 和 InteractionLayer 的分工（面板只动数据，不碰渲染）：
//   面板 → interactionLayer 的编辑 API（updateCardContent / removeCard / addCard）
//        → layer 落数据后全量重赋 interactionData → rebuildModel → 界面即刻更新
//   拖拽（编辑模式里在播放器上直接拖卡片）→ layer.cardMoved 信号 → 面板刷新列表
//
// 「细腻度更新」说明：hili-player 那边靠字段级信号（optionsVersion）做到
// 「改选项文案不碰 question」；QML 的 property var 没有字段级通知，这里走
// 「补丁落数据 + 全量重赋」的最小可靠路径 —— 卡片是 delegate 常驻重建，
// 交互状态已外置（InteractionLayer.persistStates），视觉上同样是精准更新
// （没有整卡闪烁：opacity 不重播、输入焦点在面板侧不受影响）。
// ===========================================================================
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtPlayer

Rectangle {
    id: interactionPanel

    /* 交互卡片层（ApiPanel 转进来） */
    property var interactionLayer: null

    color: "transparent"

    /* 五类的中文名与默认添加模板（照 demo INITIAL_INTERACTION_DATA 的字段形状） */
    readonly property var kindNames: ({
        guide: qsTr("三连"),
        link: qsTr("外链"),
        vote: qsTr("投票"),
        score: qsTr("评分"),
        scoreSum: qsTr("总结")
    })
    readonly property var kindKeys: ["guide", "link", "vote", "score", "scoreSum"]

    /* 展开的卡片行（getStatus 的深拷贝展开成一维，和 layer.masterModel 同序） */
    property var rows: []

    /* 当前选中行（-1 = 没选）与编辑草稿 */
    property int selectedRow: -1
    property var editDraft: null

    /* 底部一行操作日志 */
    property string logText: ""

    /* toast 提示文字（空串 = 不显示；时间窗重叠 / scoreSum 前置不满足时
       提示用户 —— 照 web demo 的 showToast / .demo-toast 样式） */
    property string toastText: ""

    readonly property bool hasLayer: interactionLayer !== null && interactionLayer !== undefined

    /* toast 提示（2400ms 后自动消失 —— 照 web demo-toast-show 的淡出节奏） */
    function showToast(text) {
        toastText = text
        toastTimer.restart()
    }

    /* ------------------------------------------------------------------ */
    /* 刷新：初次、数据变化（增删改）、拖拽结束（top/left 变了） */
    /* ------------------------------------------------------------------ */

    function refresh() {
        if (!hasLayer)
            return

        var status = interactionLayer.getStatus()
        var arr = []

        if (status !== null) {
            for (var k = 0; k < kindKeys.length; k++) {
                var kind = kindKeys[k]
                var list = status[kind + "List"]

                if (list === undefined || list === null)
                    continue

                for (var i = 0; i < list.length; i++)
                    arr.push({ kind: kind, listIndex: i, data: list[i] })
            }
        }

        rows = arr

        /* 选中行失效（被删/换数据）时清掉编辑区 */
        if (selectedRow >= rows.length) {
            selectedRow = -1
            editDraft = null
        }
    }

    Component.onCompleted: refresh()

    Connections {
        target: interactionPanel.hasLayer ? interactionPanel.interactionLayer : null

        function onCardsChanged() { interactionPanel.refresh() }
        function onCardMoved(kind, listIndex, top, left) {
            /* 拖拽结束：rows 是 property var，嵌套写入不通知（列表和编辑区
               都不刷新 —— 用户实测"拖动后右侧位置不更新"）→ 换新引用整体
               重赋；编辑区开着且正是这张卡时，草稿同步新坐标（输入框跟着重算） */
            var nextRows = []
            for (var i = 0; i < interactionPanel.rows.length; i++) {
                var row = interactionPanel.rows[i]
                if (row.kind === kind && row.listIndex === listIndex)
                    nextRows.push({ kind: row.kind, listIndex: row.listIndex,
                                    data: Object.assign({}, row.data, { top: top, left: left }) })
                else
                    nextRows.push(row)
            }
            interactionPanel.rows = nextRows

            if (interactionPanel.editDraft !== null
                    && interactionPanel.editDraft.kind === kind
                    && interactionPanel.editDraft.listIndex === listIndex) {
                interactionPanel.editDraft = Object.assign(
                            {}, interactionPanel.editDraft, { top: top, left: left })
            }

            interactionPanel.logText = qsTr("已拖动 %1 #%2 → top %3 / left %4")
                                        .arg(kind).arg(listIndex).arg(top).arg(left)
        }
    }

    /* ------------------------------------------------------------------ */
    /* 选中 → 构建编辑草稿（输入框全部读写草稿，点「应用」才落库） */
    /* ------------------------------------------------------------------ */

    function buildDraft(row) {
        var d = row.data

        var draft = {
            kind: row.kind,
            listIndex: row.listIndex,
            top: (d.top !== undefined ? d.top : 50),
            left: (d.left !== undefined ? d.left : 50),
            timeStart: (d.timeStart !== undefined ? d.timeStart : 0),
            timeEnd: (d.timeEnd !== undefined ? d.timeEnd : 0)
        }

        if (row.kind === "guide") {
            draft.guideType = (d.type !== undefined ? d.type : 1)
        } else if (row.kind === "link") {
            draft.linkContent = (d.linkContent !== undefined ? d.linkContent : "")
        } else if (row.kind === "vote") {
            draft.question = (d.question !== undefined ? d.question : "")
            var texts = []
            var opts = (d.options !== undefined && d.options !== null) ? d.options : []
            for (var i = 0; i < opts.length; i++)
                texts.push((opts[i].optionText !== undefined) ? opts[i].optionText : "")
            draft.optionTexts = texts
        } else if (row.kind === "score") {
            draft.title = (d.title !== undefined ? d.title : "")
            draft.scoreType = (d.scoreType !== undefined ? d.scoreType : 1)
        } else if (row.kind === "scoreSum") {
            /* 标题可编辑（题目列表 / 平均分 / 总人数运行期聚合 —— 照 web 定稿） */
            draft.title = (d.title !== undefined ? d.title : "")
        }

        editDraft = draft
    }

    /* 应用草稿（updateCardContent 的补丁口径：只带变化的字段没必要算，全带即可 ——
       未定义语义是"保持原值"，全量带等价于覆盖为草稿值；
       时间窗与其他卡片重叠 → 整个补丁被拒 + toast 提示，照 web 端定稿） */
    function applyDraft() {
        if (editDraft === null || !hasLayer)
            return

        var d = editDraft
        var patch = {
            top: Number(d.top),
            left: Number(d.left),
            timeStart: Number(d.timeStart),
            timeEnd: Number(d.timeEnd)
        }

        if (d.kind === "guide") {
            patch.guideType = d.guideType
        } else if (d.kind === "link") {
            patch.linkContent = d.linkContent
        } else if (d.kind === "vote") {
            patch.question = d.question
            patch.optionTexts = d.optionTexts
        } else if (d.kind === "score") {
            patch.title = d.title
            patch.scoreType = d.scoreType
        } else if (d.kind === "scoreSum") {
            /* 标题补丁（题目列表 / 平均分 / 总人数运行期聚合，不可编辑） */
            patch.title = d.title
        }

        if (!interactionLayer.updateCardContent(d.kind, d.listIndex, patch)) {
            showToast(qsTr("时间窗 %1-%2s 与其他卡片重叠，一次只能存在一张交互卡片")
                      .arg(patch.timeStart).arg(patch.timeEnd))
            return
        }
        logText = qsTr("已更新 %1 #%2").arg(kindNames[d.kind]).arg(d.listIndex)
    }

    /* ------------------------------------------------------------------ */
    /* 添加：五类各一个默认模板；时间窗取"当前播放位置起 30 秒"（加了立刻能看到）；
       时间窗重叠 / scoreSum 前置不满足 → 拒绝 + toast 提示（照 web 端定稿） */
    /* ------------------------------------------------------------------ */

    function addDefaultCard(kind) {
        if (!hasLayer)
            return

        var nowSec = 0
        if (interactionLayer.player !== null && interactionLayer.player !== undefined)
            nowSec = Math.floor(interactionLayer.player.position / 1000)

        var card = {
            top: 50,
            left: 50,
            timeStart: nowSec,
            timeEnd: nowSec + 30
        }

        if (kind === "guide") {
            card.type = 1
        } else if (kind === "link") {
            card.linkContent = qsTr("这是一个什么视频")
        } else if (kind === "vote") {
            card.question = qsTr("新投票")
            card.options = [{ optionText: qsTr("选项 A") }, { optionText: qsTr("选项 B") }]
        } else if (kind === "score") {
            card.title = qsTr("新评分")
            card.scoreType = 1
        } else if (kind === "scoreSum") {
            /* 标题可设置（汇总内容运行期聚合全部评分卡 —— 照 web 定稿） */
            card.title = qsTr("评分总结")
        }

        /* scoreSum 前置预检：必须已有至少一张评分卡（照 web addScoreSum） */
        if (kind === "scoreSum") {
            var st = interactionLayer.getStatus()
            if (st === null || st.scoreList === undefined
                    || st.scoreList === null || st.scoreList.length === 0) {
                showToast(qsTr("评分总结卡片需要先添加至少一张评分卡片"))
                return
            }
        }

        /* 时间窗互斥：addCard 返回 false = 与其他卡片重叠 → 拒绝 + toast */
        if (!interactionLayer.addCard(kind, card)) {
            showToast(qsTr("时间窗 %1-%2s 与其他卡片重叠，一次只能存在一张交互卡片")
                      .arg(nowSec).arg(nowSec + 30))
            return
        }
        logText = qsTr("已添加 %1（时间窗 %2-%3s）").arg(kindNames[kind]).arg(nowSec).arg(nowSec + 30)
    }

    /* 行显示名：各类型取最能区分的字段 */
    function rowTitle(row) {
        var d = row.data
        if (row.kind === "vote")
            return (d.question !== undefined) ? d.question : ""
        if (row.kind === "link")
            return (d.linkContent !== undefined) ? d.linkContent : ""
        if (row.kind === "score")
            return (d.title !== undefined) ? d.title : ""
        if (row.kind === "scoreSum")
            return (d.title !== undefined && d.title !== "") ? d.title : qsTr("评分总结")
        return qsTr("三连 / 关注")
    }

    /* ------------------------------------------------------------------ */
    /* 布局：添加行 → 卡片列表 → 编辑区 → 日志行 */
    /* ------------------------------------------------------------------ */

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 10

        /* ---- 添加（四颗按钮一行） ---- */
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Text {
                text: qsTr("添加：")
                color: QtPlayerTheme.panelText
                font.pixelSize: 12
            }

            Repeater {
                model: interactionPanel.kindKeys

                delegate: Button {
                    id: addCardButton

                    required property string modelData

                    Layout.preferredHeight: 26
                    text: interactionPanel.kindNames[addCardButton.modelData]
                    font.pixelSize: 12
                    flat: true

                    background: Rectangle {
                        radius: 4
                        color: parent.hovered
                                       ? (QtPlayerTheme.dark ? "#33FFFFFF" : "#33000000")
                                       : (QtPlayerTheme.dark ? "#1FFFFFFF" : "#1F000000")
                        border.width: 1
                        border.color: QtPlayerTheme.dark ? "#40FFFFFF" : "#40000000"
                    }

                    contentItem: Text {
                        text: parent.text
                        font: parent.font
                        color: QtPlayerTheme.panelText
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    /*
                     * 【必须用 addCardButton.modelData，不能写 parent.modelData】
                     * delegate 根（这颗 Button）的 parent 是 Repeater 塞进去的
                     * 布局容器，没有 modelData —— 以前写 parent.modelData 传出去
                     * 恒为 undefined：四类模板字段全没带上、日志行打出"已添加
                     * undefined"（用户实测的 bug）。required property 就在自己
                     * 身上，用 id 引用最稳。
                     */
                    onClicked: interactionPanel.addDefaultCard(addCardButton.modelData)
                }
            }

            Item { Layout.fillWidth: true }
        }

        /* ---- 卡片列表（可滚动） ---- */
        ListView {
            id: cardList

            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: interactionPanel.rows
            currentIndex: interactionPanel.selectedRow
            spacing: 4

            ScrollBar.vertical: ScrollBar {}

            delegate: Rectangle {
                id: listRow

                required property int index
                required property var modelData

                width: cardList.width
                height: 34
                radius: 4
                /* 选中态主色底、普通态浅底 */
                color: interactionPanel.selectedRow === listRow.index
                       ? Qt.alpha(QtPlayerTheme.playerAccent, 0.25)
                       : (QtPlayerTheme.dark ? "#14FFFFFF" : "#14000000")

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    spacing: 8

                    /* 类型徽标 */
                    Rectangle {
                        Layout.preferredWidth: 34
                        Layout.preferredHeight: 20
                        radius: 3
                        color: Qt.alpha(QtPlayerTheme.playerAccent, 0.8)

                        Text {
                            anchors.centerIn: parent
                            text: interactionPanel.kindNames[listRow.modelData.kind]
                            color: "#ffffff"
                            font.pixelSize: 11
                        }
                    }

                    /* 标题 + 下标 */
                    Text {
                        Layout.fillWidth: true
                        text: "#" + listRow.modelData.listIndex + "  "
                              + interactionPanel.rowTitle(listRow.modelData)
                        color: QtPlayerTheme.panelText
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }

                    /* 时间窗 */
                    Text {
                        Layout.alignment: Qt.AlignRight
                        text: listRow.modelData.data.timeStart + "-" + listRow.modelData.data.timeEnd + "s"
                        color: QtPlayerTheme.panelHintText
                        font.pixelSize: 11
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        interactionPanel.selectedRow = listRow.index
                        interactionPanel.buildDraft(listRow.modelData)
                    }
                }
            }

            /* 空态提示 */
            Text {
                anchors.centerIn: parent
                visible: interactionPanel.rows.length === 0
                text: interactionPanel.hasLayer
                      ? qsTr("没有卡片 —— mock 没带 interaction 数据，\n或点上面按钮添加")
                      : qsTr("播放器还没挂交互层")
                color: QtPlayerTheme.panelHintText
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
            }
        }

        /* ---- 编辑区（选中行才有；固定在列表下方，输入不随列表滚动） ---- */
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: editorColumn.implicitHeight + 16
            visible: interactionPanel.editDraft !== null
            radius: 6
            /* 深色分支用白 8% 提亮（黑 8% 在深色 panelBg 上不可见） */
            color: QtPlayerTheme.dark ? "#14FFFFFF" : "#0A000000"
            border.width: 1
            border.color: QtPlayerTheme.dark ? "#26FFFFFF" : "#26000000"

            ColumnLayout {
                id: editorColumn

                anchors.fill: parent
                anchors.margins: 8
                spacing: 8

                /* 标题行 */
                Text {
                    text: interactionPanel.editDraft !== null
                          ? qsTr("编辑 %1 #%2")
                            .arg(interactionPanel.kindNames[interactionPanel.editDraft.kind])
                            .arg(interactionPanel.editDraft.listIndex)
                          : ""
                    color: QtPlayerTheme.playerAccent
                    font.pixelSize: 13
                }

                /* 通用字段：top / left / timeStart / timeEnd（2×4 网格） */
                GridLayout {
                    Layout.fillWidth: true
                    columns: 4
                    columnSpacing: 8
                    rowSpacing: 6

                    Text { text: qsTr("top%"); color: QtPlayerTheme.panelHintText; font.pixelSize: 11 }
                    StyledField {
                        Layout.preferredWidth: 60
                        text: interactionPanel.editDraft !== null ? ("" + interactionPanel.editDraft.top) : ""
                        onTextChanged: {
                            if (interactionPanel.editDraft === null) return
                            var v = parseFloat(text)
                            if (!isNaN(v)) interactionPanel.editDraft.top = v
                        }
                    }
                    Text { text: qsTr("left%"); color: QtPlayerTheme.panelHintText; font.pixelSize: 11 }
                    StyledField {
                        Layout.preferredWidth: 60
                        text: interactionPanel.editDraft !== null ? ("" + interactionPanel.editDraft.left) : ""
                        onTextChanged: {
                            if (interactionPanel.editDraft === null) return
                            var v = parseFloat(text)
                            if (!isNaN(v)) interactionPanel.editDraft.left = v
                        }
                    }
                    Text { text: qsTr("起(秒)"); color: QtPlayerTheme.panelHintText; font.pixelSize: 11 }
                    StyledField {
                        Layout.preferredWidth: 60
                        text: interactionPanel.editDraft !== null ? ("" + interactionPanel.editDraft.timeStart) : ""
                        onTextChanged: {
                            if (interactionPanel.editDraft === null) return
                            var v = parseFloat(text)
                            if (!isNaN(v)) interactionPanel.editDraft.timeStart = v
                        }
                    }
                    Text { text: qsTr("止(秒)"); color: QtPlayerTheme.panelHintText; font.pixelSize: 11 }
                    StyledField {
                        Layout.preferredWidth: 60
                        text: interactionPanel.editDraft !== null ? ("" + interactionPanel.editDraft.timeEnd) : ""
                        onTextChanged: {
                            if (interactionPanel.editDraft === null) return
                            var v = parseFloat(text)
                            if (!isNaN(v)) interactionPanel.editDraft.timeEnd = v
                        }
                    }
                }

                /* ---- 类型特定字段 ---- */

                /* guide：形态（1 = 三连+关注 / 2 = 仅三连 / 3 = 仅关注） */
                RowLayout {
                    Layout.fillWidth: true
                    visible: interactionPanel.editDraft !== null
                             && interactionPanel.editDraft.kind === "guide"
                    spacing: 8

                    Text { text: qsTr("形态："); color: QtPlayerTheme.panelHintText; font.pixelSize: 11 }

                    Repeater {
                        model: [qsTr("三连+关注"), qsTr("仅三连"), qsTr("仅关注")]

                        delegate: Button {
                            id: guideTypeButton

                            required property int index
                            required property string modelData

                            Layout.preferredHeight: 24
                            flat: true
                            checkable: false
                            font.pixelSize: 11

                            /* 选中态 = 草稿的 guideType（1/2/3） */
                            readonly property bool active: interactionPanel.editDraft !== null
                                    && interactionPanel.editDraft.guideType === (guideTypeButton.index + 1)

                            background: Rectangle {
                                radius: 4
                                color: guideTypeButton.active
                                       ? Qt.alpha(QtPlayerTheme.playerAccent, 0.45)
                                       : (guideTypeButton.hovered
                                          ? (QtPlayerTheme.dark ? "#33FFFFFF" : "#33000000")
                                          : (QtPlayerTheme.dark ? "#1FFFFFFF" : "#1F000000"))
                            }

                            contentItem: Text {
                                text: guideTypeButton.modelData
                                font.pixelSize: 11
                                color: guideTypeButton.active ? "#ffffff" : QtPlayerTheme.panelText
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }

                            /* 【必须整体重赋新对象】property var 的嵌套字段写入
                               不触发 changed 信号 → active 绑定永不重算 → 按钮
                               看似点了没反应（用户实测）。Object.assign 生成新
                               引用，全部绑定立即重算（下同）。 */
                            onClicked: if (interactionPanel.editDraft !== null)
                                           interactionPanel.editDraft = Object.assign(
                                                       {}, interactionPanel.editDraft,
                                                       { guideType: guideTypeButton.index + 1 })
                        }
                    }

                    Item { Layout.fillWidth: true }
                }

                /* link：外链文案 */
                RowLayout {
                    Layout.fillWidth: true
                    visible: interactionPanel.editDraft !== null
                             && interactionPanel.editDraft.kind === "link"
                    spacing: 8

                    Text { text: qsTr("文案："); color: QtPlayerTheme.panelHintText; font.pixelSize: 11 }

                    StyledField {
                        Layout.fillWidth: true
                        text: (interactionPanel.editDraft !== null
                               && interactionPanel.editDraft.kind === "link")
                              ? interactionPanel.editDraft.linkContent : ""
                        onTextChanged: if (interactionPanel.editDraft !== null
                                           && interactionPanel.editDraft.kind === "link")
                                           interactionPanel.editDraft.linkContent = text
                    }
                }

                /* vote：问题 + 选项列表（增删改文案） */
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: interactionPanel.editDraft !== null
                             && interactionPanel.editDraft.kind === "vote"
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Text { text: qsTr("问题："); color: QtPlayerTheme.panelHintText; font.pixelSize: 11 }

                        StyledField {
                            Layout.fillWidth: true
                            text: (interactionPanel.editDraft !== null
                                   && interactionPanel.editDraft.kind === "vote")
                                  ? interactionPanel.editDraft.question : ""
                            onTextChanged: if (interactionPanel.editDraft !== null
                                               && interactionPanel.editDraft.kind === "vote")
                                               interactionPanel.editDraft.question = text
                        }
                    }

                    /* 选项行（文案 + 删除） —— 草稿的 optionTexts 整体重赋驱动重建 */
                    Repeater {
                        model: (interactionPanel.editDraft !== null
                                && interactionPanel.editDraft.kind === "vote"
                                && interactionPanel.editDraft.optionTexts !== undefined)
                               ? interactionPanel.editDraft.optionTexts : []

                        delegate: RowLayout {
                            id: optionRow

                            required property int index

                            Layout.fillWidth: true
                            spacing: 6

                            Text {
                                text: qsTr("选项%1：").arg(optionRow.index + 1)
                                color: QtPlayerTheme.panelHintText
                                font.pixelSize: 11
                            }

                            StyledField {
                                Layout.fillWidth: true
                                text: interactionPanel.editDraft !== null
                                      ? ("" + interactionPanel.editDraft.optionTexts[optionRow.index]) : ""
                                onTextChanged: if (interactionPanel.editDraft !== null) {
                                                   var texts = interactionPanel.editDraft.optionTexts
                                                   if (texts[optionRow.index] !== text) {
                                                       texts[optionRow.index] = text
                                                       interactionPanel.editDraft.optionTexts = texts
                                                   }
                                               }
                            }

                            Button {
                                Layout.preferredWidth: 24
                                Layout.preferredHeight: 24
                                flat: true
                                text: "×"
                                font.pixelSize: 12

                                background: Rectangle {
                                    radius: 4
                                    color: parent.hovered ? "#66FF5555" : "transparent"
                                }

                                contentItem: Text {
                                    text: parent.text
                                    color: "#FF8888"
                                    font.pixelSize: 13
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }

                                /* 拷贝新数组 + 整体重赋：同引用重赋不通知 →
                                   Repeater 的 model 不变 → 删不掉（用户实测） */
                                onClicked: {
                                    if (interactionPanel.editDraft === null)
                                        return
                                    var texts = interactionPanel.editDraft.optionTexts.slice()
                                    texts.splice(optionRow.index, 1)
                                    interactionPanel.editDraft = Object.assign(
                                                {}, interactionPanel.editDraft,
                                                { optionTexts: texts })
                                }
                            }
                        }
                    }

                    /* 添加选项（尾部追加一条空文案 —— 和 hili-player 的尾部增删语义一致） */
                    Button {
                        Layout.preferredHeight: 24
                        flat: true
                        text: qsTr("+ 添加选项")
                        font.pixelSize: 11

                        background: Rectangle {
                            radius: 4
                            color: parent.hovered
                                       ? (QtPlayerTheme.dark ? "#33FFFFFF" : "#33000000")
                                       : (QtPlayerTheme.dark ? "#1FFFFFFF" : "#1F000000")
                        }

                        contentItem: Text {
                            text: parent.text
                            font.pixelSize: 11
                            color: QtPlayerTheme.playerAccent
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        onClicked: {
                            if (interactionPanel.editDraft === null)
                                return
                            /* 拷贝新数组 + 整体重赋：同引用重赋不通知 →
                               新选项不出现（用户实测） */
                            var texts = interactionPanel.editDraft.optionTexts.slice()
                            texts.push(qsTr("新选项"))
                            interactionPanel.editDraft = Object.assign(
                                        {}, interactionPanel.editDraft,
                                        { optionTexts: texts })
                        }
                    }
                }

                /* score / scoreSum：标题（scoreSum 的题目列表 / 平均分 / 总人数
                   运行期聚合，仅标题可编辑 —— 照 web 定稿） */
                RowLayout {
                    Layout.fillWidth: true
                    visible: interactionPanel.editDraft !== null
                             && (interactionPanel.editDraft.kind === "score"
                                 || interactionPanel.editDraft.kind === "scoreSum")
                    spacing: 8

                    Text { text: qsTr("标题："); color: QtPlayerTheme.panelHintText; font.pixelSize: 11 }

                    StyledField {
                        Layout.fillWidth: true
                        text: (interactionPanel.editDraft !== null
                               && (interactionPanel.editDraft.kind === "score"
                                   || interactionPanel.editDraft.kind === "scoreSum"))
                              ? interactionPanel.editDraft.title : ""
                        onTextChanged: if (interactionPanel.editDraft !== null
                                           && (interactionPanel.editDraft.kind === "score"
                                               || interactionPanel.editDraft.kind === "scoreSum"))
                                           interactionPanel.editDraft.title = text
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: interactionPanel.editDraft !== null
                             && interactionPanel.editDraft.kind === "score"
                    spacing: 8

                    Text { text: qsTr("图标："); color: QtPlayerTheme.panelHintText; font.pixelSize: 11 }

                    Repeater {
                        model: [qsTr("星"), qsTr("心"), qsTr("柠")]

                        delegate: Button {
                            id: scoreTypeButton

                            required property int index
                            required property string modelData

                            Layout.preferredHeight: 24
                            Layout.preferredWidth: 40
                            flat: true

                            readonly property bool active: interactionPanel.editDraft !== null
                                    && interactionPanel.editDraft.scoreType === (scoreTypeButton.index + 1)

                            background: Rectangle {
                                radius: 4
                                color: scoreTypeButton.active
                                       ? Qt.alpha(QtPlayerTheme.playerAccent, 0.45)
                                       : (scoreTypeButton.hovered
                                          ? (QtPlayerTheme.dark ? "#33FFFFFF" : "#33000000")
                                          : (QtPlayerTheme.dark ? "#1FFFFFFF" : "#1F000000"))
                            }

                            contentItem: Text {
                                text: scoreTypeButton.modelData
                                font.pixelSize: 11
                                color: scoreTypeButton.active ? "#ffffff" : QtPlayerTheme.panelText
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }

                            onClicked: if (interactionPanel.editDraft !== null)
                                           interactionPanel.editDraft = Object.assign(
                                                       {}, interactionPanel.editDraft,
                                                       { scoreType: scoreTypeButton.index + 1 })
                        }
                    }

                    Item { Layout.fillWidth: true }
                }

                /* ---- 操作行：应用 / 关闭卡片 / 删除 ---- */
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Button {
                        Layout.preferredHeight: 28
                        text: qsTr("应用修改")
                        font.pixelSize: 12

                        background: Rectangle {
                            radius: 4
                            color: parent.hovered
                                   ? Qt.alpha(QtPlayerTheme.playerAccent, 0.9)
                                   : QtPlayerTheme.playerAccent
                        }

                        contentItem: Text {
                            text: parent.text
                            font.pixelSize: 12
                            color: "#ffffff"
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        onClicked: interactionPanel.applyDraft()
                    }

                    /* closeCard：本次播放关闭（展示模式语义，数据还在） */
                    Button {
                        Layout.preferredHeight: 28
                        text: qsTr("关闭卡片")
                        font.pixelSize: 12
                        flat: true

                        background: Rectangle {
                            radius: 4
                            color: parent.hovered
                                       ? (QtPlayerTheme.dark ? "#33FFFFFF" : "#33000000")
                                       : (QtPlayerTheme.dark ? "#1FFFFFFF" : "#1F000000")
                        }

                        contentItem: Text {
                            text: parent.text
                            font.pixelSize: 12
                            color: QtPlayerTheme.panelText
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        onClicked: {
                            if (interactionPanel.editDraft === null || !interactionPanel.hasLayer)
                                return
                            interactionPanel.interactionLayer.closeCard(
                                        interactionPanel.editDraft.kind,
                                        interactionPanel.editDraft.listIndex)
                            interactionPanel.logText = qsTr("已关闭 %1 #%2（本次播放）")
                                                       .arg(interactionPanel.kindNames[interactionPanel.editDraft.kind])
                                                       .arg(interactionPanel.editDraft.listIndex)
                        }
                    }

                    /* removeCard：从数据删除（编辑语义，不再出现） */
                    Button {
                        Layout.preferredHeight: 28
                        text: qsTr("删除卡片")
                        font.pixelSize: 12
                        flat: true

                        background: Rectangle {
                            radius: 4
                            color: parent.hovered ? "#66FF5555" : "#3FFF5555"
                        }

                        contentItem: Text {
                            text: parent.text
                            font.pixelSize: 12
                            color: "#ffffff"
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        onClicked: {
                            if (interactionPanel.editDraft === null || !interactionPanel.hasLayer)
                                return
                            interactionPanel.interactionLayer.removeCard(
                                        interactionPanel.editDraft.kind,
                                        interactionPanel.editDraft.listIndex)
                            interactionPanel.logText = qsTr("已删除 %1 #%2")
                                                       .arg(interactionPanel.kindNames[interactionPanel.editDraft.kind])
                                                       .arg(interactionPanel.editDraft.listIndex)
                            interactionPanel.selectedRow = -1
                            interactionPanel.editDraft = null
                        }
                    }

                    Item { Layout.fillWidth: true }
                }
            }
        }

        /* ---- 日志行 ---- */
        Text {
            Layout.fillWidth: true
            text: interactionPanel.logText
            color: QtPlayerTheme.panelHintText
            font.pixelSize: 11
            elide: Text.ElideMiddle
        }
    }

    /* ======================================================================
     * toast 浮层（时间窗重叠 / scoreSum 前置不满足的提示 —— 照 web demo 的
     * .demo-toast：深色胶囊浮在面板顶部中央，2400ms 后淡出，pointer-events:none
     * 用 visible + 不挂 MouseArea 实现（Rectangle 本身不吃鼠标事件）
     * ====================================================================== */
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 8
        z: 100
        width: toastTextItem.implicitWidth + 36
        height: 36
        radius: 8
        color: "#EB141418"      /* rgba(20,20,24,.92) */
        visible: interactionPanel.toastText !== ""
        opacity: interactionPanel.toastText !== "" ? 1 : 0

        Behavior on opacity {
            NumberAnimation { duration: 250; easing.type: Easing.Linear }
        }

        Text {
            id: toastTextItem

            anchors.centerIn: parent
            text: interactionPanel.toastText
            color: "#ffffff"
            font.pixelSize: 12
        }
    }

    Timer {
        id: toastTimer

        interval: 2400
        repeat: false
        onTriggered: interactionPanel.toastText = ""
    }

    /* ======================================================================
     * 深色小输入框（内联组件：面板里十几处用，统一规格 26 高 / 12 号字）
     * ====================================================================== */
    component StyledField: TextField {
        id: field

        color: QtPlayerTheme.panelText
        font.pixelSize: 12
        selectByMouse: true
        background: Rectangle {
            radius: 4
            color: field.focus
                   ? (QtPlayerTheme.dark ? "#33FFFFFF" : "#33000000")
                   : (QtPlayerTheme.dark ? "#1FFFFFFF" : "#1F000000")
            border.width: 1
            border.color: field.focus ? QtPlayerTheme.playerAccent : "transparent"
        }
    }
}
