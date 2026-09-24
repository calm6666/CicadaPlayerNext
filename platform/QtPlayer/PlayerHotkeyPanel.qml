// ===========================================================================
// 「快捷键说明」面板（复刻参考实现 component/hotkeypanel/index.ts + index.scss）
//
// 【参考的 DOM】
//     <div class="player-hotkey-panel">
//       <div class="player-hotkey-panel-title">快捷键说明 <span class="…-close">✕</span></div>
//       <div class="player-hotkey-panel-area">
//         <div class="player-hotkey-panel-content">
//           <div class="player-hotkey-panel-content-item">
//             <span class="…-content-name">Space</span>
//             <span class="…-content-desc">播放/暂停</span>
//           </div>
//           …（14 条）
//         </div>
//       </div>
//     </div>
//
// 【参考的 CSS（hotkeypanel/index.scss）】
//     面板：background rgba(33,33,33,.9); border-radius 4px; color #fff;
//           left/top 50% + translate(-50%,-50%); width 400px; z-index 80
//     标题：和色彩面板同一套（40px 行高、字号 16、底部 1px 分隔线、右上角 22x22 关闭）
//     列表区：margin 10px 0; max-height 268px; overflow-y scroll（滚动条隐藏）
//     行：font-size 12px; height 24px; line-height 24px; min-width 360px; text-align center
//         名称 width 120px；说明 width 190px、颜色 #999
//
// 【快捷键列表 = 本播放器**真实支持**的那些（不是照抄参考那份）】
//   参考那份里有几条我们根本没有对应功能（E 收藏、Enter 发弹幕、[ ] 多P），
//   照抄会变成"按了没反应"的假快捷键 —— 所以这里只列真正接上的，
//   每条都在 Main.qml 里有对应的 Shortcut / Keys 处理（注释里注明）。
// ===========================================================================
import QtQuick
import QtQuick.Controls
import QtPlayer

Rectangle {
    id: hotkeyPanel

    property bool opened: false

    signal closeRequested()

    function open() { opened = true }
    function close() { opened = false }

    /* name = 键位（参考里那一列），desc = 说明 */
    readonly property var hotkeys: [
        { name: "Space",  desc: qsTr("播放 / 暂停") },
        { name: "→",      desc: qsTr("快进 5 秒（长按 2 倍速）") },
        { name: "←",      desc: qsTr("快退 5 秒") },
        { name: "↑",      desc: qsTr("音量增加 10%") },
        { name: "↓",      desc: qsTr("音量降低 10%") },
        { name: "M",      desc: qsTr("开启 / 关闭静音") },
        { name: "D",      desc: qsTr("开启 / 关闭弹幕") },
        { name: "F",      desc: qsTr("全屏 / 退出全屏") },
        { name: "Esc",    desc: qsTr("退出全屏 / 关闭面板") },
        { name: "媒体键 play/pause", desc: qsTr("播放 / 暂停") },
        { name: "Ctrl+O", desc: qsTr("打开本地视频文件") },
        { name: "Ctrl+D", desc: qsTr("灌入一批示例弹幕（调试用）") }
    ]

    width: 400
    /*
     * 高度 = 标题 + 上下各 10（参考 .player-hotkey-panel-area 的 margin:10px 0）+ 列表区。
     *
     * 【上一版少了下面那 10px】它写的是 titleItem.height + area.height，而列表区的 y 是
     * titleItem.height + 10 —— 于是面板底边比内容**低 10px**，最后一行被面板边缘切掉
     * （用户实测："内部内容溢出了"）。现在把上下两个外边距都算进去。
     */
    height: titleItem.height + 10 + area.height + 10
    x: (parent.width - width) / 2
    y: (parent.height - height) / 2
    color: "#E6212121"
    radius: 4
    visible: opened
    z: 80

    /* 【同一个坑】面板要自己吃掉鼠标事件，否则点在快捷键面板上会穿到画面区去
     * 触发播放/暂停（详见 PlayerColorPanel.qml 里 panelBackdrop 那段说明）。
     * 必须是第一个子元素（z 序最低），hoverEnabled: false 不抢画面区的 hover。 */
    MouseArea {
        id: panelBackdrop

        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: false
        cursorShape: Qt.ArrowCursor
    }

    Item {
        id: titleItem

        width: parent.width
        height: 40

        Text {
            anchors.centerIn: parent
            color: "#fff"
            font.pixelSize: 16
            text: qsTr("快捷键说明")
        }

        Rectangle {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: 1
            color: Qt.rgba(1, 1, 1, 0.1)
        }

        Item {
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.top: parent.top
            anchors.topMargin: 10
            width: 22
            height: 22

            Rectangle { anchors.centerIn: parent; width: 13; height: 1.5; color: "#fff"; rotation: 45 }
            Rectangle { anchors.centerIn: parent; width: 13; height: 1.5; color: "#fff"; rotation: -45 }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: hotkeyPanel.closeRequested() }
        }
    }

    /*
     * margin:10px 0; max-height:268px（超了滚动）。
     *
     * 【滚动条：这里**有意和参考不同**】参考是 `scrollbar-width: none`（隐藏滚动条），
     * 但用户明确要求"把滚动条显示出来"—— 列表 12 项超过 268px 时，没有滚动条就看不出
     * 下面还有内容（"虽然可以滚动但溢出了"）。所以这里用 AsNeeded + 一条细样式。
     */
    Flickable {
        id: area

        y: titleItem.height + 10
        width: parent.width
        height: Math.min(contentHeight, 268)
        contentWidth: width - (scrollBar.visible ? scrollBar.width : 0)
        contentHeight: contentColumn.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ScrollBar.vertical: ScrollBar {
            id: scrollBar

            policy: ScrollBar.AsNeeded
            width: 6
            anchors.right: parent.right
            anchors.rightMargin: 2

            contentItem: Rectangle {
                implicitWidth: 6
                radius: 3
                color: scrollBar.pressed ? Qt.rgba(1, 1, 1, 0.45) : Qt.rgba(1, 1, 1, 0.28)
            }

            background: Rectangle {
                implicitWidth: 6
                color: Qt.rgba(1, 1, 1, 0.08)
                radius: 3
            }
        }

        Column {
            id: contentColumn

            /* padding: 0 20px（右边再给滚动条让出位置） */
            x: 20
            width: area.width - 40 - (scrollBar.visible ? scrollBar.width : 0)

            Repeater {
                model: hotkeyPanel.hotkeys

                delegate: Item {
                    id: hotkeyRow

                    required property var modelData

                    width: contentColumn.width
                    height: 24   /* height/line-height: 24px */

                    /* 参考那一行是 text-align:center + 名称 120 / 说明 190 →
                       这一对（310）在行内**居中**，不是贴左。 */
                    readonly property real pairWidth: 120 + 190
                    readonly property real pairX: Math.max(0, (width - pairWidth) / 2)

                    Text {
                        x: hotkeyRow.pairX
                        width: 120
                        anchors.verticalCenter: parent.verticalCenter
                        horizontalAlignment: Text.AlignHCenter
                        color: "#fff"
                        font.pixelSize: 12
                        text: hotkeyRow.modelData.name
                    }

                    Text {
                        x: hotkeyRow.pairX + 120
                        width: 190
                        anchors.verticalCenter: parent.verticalCenter
                        horizontalAlignment: Text.AlignHCenter
                        color: "#999"
                        font.pixelSize: 12
                        elide: Text.ElideRight
                        text: hotkeyRow.modelData.desc
                    }
                }
            }
        }
    }
}
