// ===========================================================================
// LiveMenu —— 直播播放器里的浮层菜单（当前只有"清晰度"一个使用者）
//
// 【数值出处：**用户下载的官方 bundle** `room-player.min.js` 里的 CSS-in-JS】
// 这是真值，不是猜的、也不是从右键菜单那套推的（两套样式不一样，见下面 note）：
//
//   .live-web-player-controller .panel {                ← 所有浮层（含清晰度）的公共壳
//       position:absolute; left:50%; transform:translate(-50%,-100%);
//       opacity:0.9; background-color:rgba(33,33,33,0.9);
//       color:rgba(255,255,255,0.9); border-radius:4px; z-index:100 }
//   .quality-wrap .panel { padding:9px 0; min-height:94px }
//   .quality-it { cursor:pointer; line-height:22px; padding:0 20px; white-space:nowrap }
//   .quality-it:hover { background-color:rgba(255,255,255,0.1) }
//   .quality-it.selected { color:#23ade5 }              ← **当前档的标记方式就是这个**：文字变 #23ade5
//
// 【note：不要和右键菜单搞混】bundle 里另有一套 `_web-player-context-menu_*`
// （`rgba(28,28,28,.9)`、条目 `padding:3px 20px; width:200px`）—— 那是**右键菜单**，
// 不是清晰度面板。清晰度面板用的是上面 `.panel + .quality-it` 这一套（svelte 注入）。
//
// 【没有做的：线路（线路选择器）】官方清晰度面板上方还有一个线路选择器
//   `.line-wrap{width:66px;margin-bottom:28px;padding:3px 20px 0 20px}`
//   `.line-selector{width:66px;border:1px solid rgba(255,255,255,.4);border-radius:2px;background:#212121}`
//   `.line-label/.option{height:22px;padding-left:9px;line-height:22px}`
//   `.arrow{0×0;top:8px;right:8px;border:4px solid transparent;border-top:6px solid #797979}`
// 本项目的直播源只有一条线路（mock 也只给一条），**没有线路数据就不摆一个假的线路选择器**；
// 等以后接了"多线路"再按上面这组数值补上。
//
// 【模型契约】model 是一个 JS 数组，每项：
//     { label: "原画", url: "http://…/live/1080p.flv",           ← FLV：换流重连
//       width: 1920, height: 1080, videoBitrate: 5000000 }
//     { label: "1080P", streamIndex: 3, … }                     ← HLS：清单内换流
// currentKey 用来判断哪一项是"当前档"（拿 label 比）。
// ===========================================================================
import QtQuick
import QtPlayer

Item {
    id: menu

    property bool opened: false
    property var model: []
    property string currentKey: ""

    /* 点了某一项：把那一项原样交出去（FLV 拿 url 重连、HLS 拿 streamIndex 换流） */
    signal picked(var item)

    /* 官方值：条目行高 22 + 左右 padding 20；面板 padding 9px 0、min-height 94px */
    readonly property int itemLineHeight: 22
    readonly property int itemPaddingH: 20
    readonly property int panelPaddingV: 9
    readonly property int panelMinHeight: 94

    /* 宽度按最长条目文字算（官方没有写死宽度：`.quality-it` 是 nowrap + padding:0 20px，
       面板宽度自然由内容撑开）。这里用 TextMetrics 量一次，避免 QML 里 Column 宽度来回依赖。 */
    TextMetrics {
        id: metrics

        font.pixelSize: 12
        text: {
            var longest = ""
            for (var i = 0; i < menu.model.length; ++i) {
                var t = menu.model[i].label !== undefined ? ("" + menu.model[i].label) : ("" + menu.model[i].name)
                if (t.length > longest.length)
                    longest = t
            }
            return longest === "" ? qsTr("这个地址没有清晰度清单") : longest
        }
    }

    width: metrics.advanceWidth + itemPaddingH * 2
    height: Math.max(panelMinHeight, Math.max(1, model.length) * itemLineHeight + panelPaddingV * 2)
    visible: opened
    z: 100

    Rectangle {
        anchors.fill: parent
        /* rgba(33,33,33,.9) + 圆角 4 + 整体 opacity .9（官方 .panel 的三个值） */
        color: Qt.rgba(33 / 255, 33 / 255, 33 / 255, 0.9)
        radius: 4
        opacity: 0.9
    }

    Column {
        id: items

        x: 0
        y: menu.panelPaddingV
        width: menu.width

        Repeater {
            model: menu.model

            delegate: Rectangle {
                id: row

                required property var modelData

                readonly property bool current: menu.currentKey === ("" + modelData.label)

                width: menu.width
                height: menu.itemLineHeight
                color: rowHover.hovered ? Qt.rgba(1, 1, 1, 0.1)   /* hsla(0,0%,100%,.1) */
                                        : "transparent"

                /* hover：照这个 App 的规矩用 HoverHandler（不吃掉外层 hover 判据） */
                HoverHandler { id: rowHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: menu.picked(row.modelData) }

                Text {
                    anchors {
                        left: parent.left
                        leftMargin: menu.itemPaddingH
                        right: parent.right
                        rightMargin: menu.itemPaddingH
                        verticalCenter: parent.verticalCenter
                    }
                    height: menu.itemLineHeight
                    verticalAlignment: Text.AlignVCenter
                    text: modelData.label !== undefined ? modelData.label : ("" + modelData.name)
                    /* **当前档 = #23ade5（官方 .quality-it.selected）；hover 变纯白** */
                    color: row.current ? QtPlayerTheme.playerAccent
                                       : (rowHover.hovered ? "#ffffff" : Qt.rgba(1, 1, 1, 0.9))
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
            }
        }
    }

    /* 拿不到档位（比如用户手填了一个没有清单/没有 streams.json 的地址）时也给一句话，
       而不是弹出一个空框 —— 空框看起来像坏了 */
    Text {
        anchors {
            left: parent.left
            leftMargin: menu.itemPaddingH
            right: parent.right
            rightMargin: menu.itemPaddingH
            verticalCenter: parent.verticalCenter
        }
        visible: menu.model.length === 0
        text: qsTr("这个地址没有清晰度清单")
        color: Qt.rgba(1, 1, 1, 0.9)
        font.pixelSize: 12
        wrapMode: Text.WordWrap
    }
}
