// ===========================================================================
// LiveControlBar —— **直播播放器的控制栏**（复刻 B 站直播播放器的主控制条）
//
// 【为什么另写一个文件，不用 PointView 那套 PlayerControlBar】
//   用户明确要求："直播播放器 ui 不要使用目前写好的这个哟，重新写一个"。
//   两张控制栏的**信息结构本来就不一样**，硬套只会得到四不像：
//     点播（PlayerControlBar）：进度条 + 时间 + 清晰度 + 倍速 + 弹幕 + 音量 + 设置 + 全屏
//     直播（本文件）          ：**没有进度条/没有时长/不能 seek**，
//                               多了"直播中"状态、弹幕输入框、清晰度（换流重连）、
//                               以及"回到直播"（暂停之后追回直播进度）
//
// 【B 站直播控制栏的结构（本文件照这个复刻）】
//   底部一条半透明黑条（左到右渐深），内容自左向右：
//     [播放/暂停] [音量] [● 直播中 · 画质/码率]        [弹幕输入框 + 发送]        [清晰度 ▾] [弹幕开关] [统计信息] [网页全屏] [全屏]
//   弹幕输入框实际是在控制条**上方**一条（B 站那样），所以这里拆成两行：
//     上一行 = 弹幕输入（左下）
//     下一行 = 控制条本体
//   hover 才显示（3 秒自动收起），颜色/圆角/字号都按 B 站直播的观感：
//     主色 #FB7299（关注/高亮）、面板 rgba(20,20,20,.92)、文字白色 90%、
//     菜单项 hover 白 12%、选中项左侧一个小圆点。
//
// 【和播放器的接口】全部走属性 + 信号，本文件**不碰** CicadaPlayerItem：
//   in : shown / playing / paused / muted / volume / fullscreen / liveStats / quality* / danmakuEnabled
//   out: playPauseRequested / muteRequested / volumeRequested / qualityRequested(name,url)
//        danmakuToggled / danmakuSend(text) / infoRequested / webFullscreenRequested / fullscreenRequested
// ===========================================================================
import QtQuick
import QtPlayer

Item {
    id: liveBar

    /* ---- 输入 ---- */
    property bool shown: true                 /* 控制条显不显示（由外面按鼠标活动控制） */
    property bool playing: false
    property bool muted: false
    property real volume: 1.0
    property bool fullscreen: false
    /* 直播状态文字（"直播中 · 1920x1080 · 5000kbps"）—— 全部是真读数，见 LivePlayerView */
    property string liveStateText: ""
    /* 当前清晰度标签 + 可选档位：[{name,label,url,width,height,videoBitrate,streamIndex?}]
       FLV 的档位来自服务端 streams.json（换档 = 换流重连）；
       HLS 的档位来自 master 清单（换档 = 清单内换流，带 streamIndex）—— 两种情况都在
       LivePlayerView 里整理成同一种形状，这里只负责显示和把整项回传。 */
    property string qualityLabel: qsTr("原画")
    property var qualityList: []
    property bool danmakuEnabled: true
    /* 弹幕输入框里的文字（外面不用管，除非要预填） */
    property alias danmakuText: dmInput.text

    /* ---- 输出 ---- */
    signal playPauseRequested()
    signal muteRequested()
    signal volumeRequested(real value)
    signal qualityRequested(var item)
    signal danmakuToggled(bool enabled)
    signal danmakuSend(string text)
    signal infoRequested()
    signal webFullscreenRequested()
    signal fullscreenRequested()
    /* 清晰度菜单被打开（外面据此"按需"去拉清单，见 LivePlayerView 的处理） */
    signal qualityMenuOpened()

    /*
     * =======================================================================
     * 几何：**全部是官方 bundle 里的真值**（`room-player.min.js` 的 CSS-in-JS）
     *
     *   .web-player-controller-bg   （底衬）: height:56px; bottom:0;
     *       background-image: linear-gradient(180deg, rgba(0,0,0,0), rgba(0,0,0,.7))
     *   .live-web-player-controller （控制条本体，本身没有高度，靠绝对定位的内容行）
     *   .control-area               （内容行）: position:absolute; bottom:3px; left:0; right:0;
     *       padding:0 12px; height:36px; font-size:12px; display:flex; align-items:center
     *   .left-area / .right-area    : height:100%; display:flex
     *   .right-area                 : flex:1; flex-direction:row-reverse   ← 右组贴右边、DOM 顺序即"从右往左"
     *   .icon                       : display:inline-block; width:36px; height:36px; cursor:pointer
     *   .icon svg                   : fill:rgba(255,255,255,.9)  →  :hover fill:#fff
     *   .text                       : line-height:36px; padding:0 6px; color:rgba(255,255,255,.9) → hover #fff
     *
     * 所以这里：底衬 56px，内容行高 36px、距底 3px、左右各 12px；按钮 36×36。
     * 弹幕输入那一行摆在 56px 渐变条**上方**（官方是网页全屏时注入的发送栏，位置就在控制条上方）。
     * =======================================================================
     */
    readonly property int stripHeight: 56
    readonly property int contentRowHeight: 36
    readonly property int contentRowBottom: 3
    readonly property int contentRowPaddingH: 12
    readonly property int inputRowHeight: 24
    readonly property int inputRowGap: 6

    height: stripHeight + inputRowHeight + inputRowGap

    /* ---- B 站直播的配色（全部来自官方令牌/样式表，来源写在文件头） ---- */
    /*
     * 品牌粉：官方 v11 主题里是 `--Pi5:#FF6699`（早前那份 `#FB7299` 在当前主题文件里已经查不到，
     * 属于旧值）。关注按钮 / 发送按钮的 hover 都用它。
     */
    readonly property color livePink: "#FF6699"
    /* 直播里的功能蓝：官方直播播放器的水印/提示/按钮用的是 `#23ade5` */
    readonly property color liveBlue: QtPlayerTheme.playerAccent
    /* 清晰度菜单的当前档用官方 `.quality-it.selected{color:#23ade5}`，和上面同一个蓝 */
    readonly property color playerBlue: QtPlayerTheme.playerAccent
    /*
     * 【坑】QML 的 `color` 类型**不认 CSS 的函数写法**：`"rgba(255,255,255,0.9)"` 会直接报
     *     Invalid property assignment: color expected
     * 而且这是**运行期**错误（qmlcachegen 不做类型检查，编译一样过），后果是整个
     * LiveControlBar 加载失败 → LivePlayerView 不可用 → LiveMain 建不出来 →
     * "首页输入直播地址点播放没任何反应"，日志里只有这一行。
     * 所以颜色一律用 "#AARRGGBB" 或 Qt.rgba(...) 写。
     */
    readonly property color barText: Qt.rgba(1, 1, 1, 0.9)          /* = rgba(255,255,255,.9) */
    readonly property color barTextDim: Qt.rgba(1, 1, 1, 0.6)       /* = rgba(255,255,255,.6) */
    readonly property color menuBg: Qt.rgba(33 / 255, 33 / 255, 33 / 255, 0.9)

    /* 底衬：官方 `.web-player-controller-bg` —— 56px 高、180° 两段渐变（透明 → rgba(0,0,0,.7)） */
    Rectangle {
        id: strip

        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }
        height: liveBar.stripHeight
        gradient: Gradient {
            orientation: Gradient.Vertical
            GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, 0) }
            GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.7) }
        }
        opacity: liveBar.shown ? 1 : 0

        Behavior on opacity {
            NumberAnimation { duration: 200; easing.type: Easing.OutQuad }
        }
    }

    /* =======================================================================
     * 上一行：弹幕输入（官方那颗"发个弹幕呗~"）
     * ======================================================================= */
    Row {
        id: dmRow

        anchors {
            left: parent.left
            leftMargin: liveBar.contentRowPaddingH
            bottom: strip.top
            bottomMargin: liveBar.inputRowGap
        }
        height: liveBar.inputRowHeight
        spacing: 8
        opacity: liveBar.shown ? 1 : 0
        visible: opacity > 0

        Behavior on opacity {
            NumberAnimation { duration: 200; easing.type: Easing.OutQuad }
        }

        /* 弹幕开关（"弹"那颗）—— 放输入框左边（官方直播的弹幕开关就在输入框左端） */
        Rectangle {
            id: dmToggle

            width: 34
            height: 24
            radius: 3
            color: dmToggleHover.hovered ? Qt.rgba(1, 1, 1, 0.18) : Qt.rgba(1, 1, 1, 0.10)
            border.width: liveBar.danmakuEnabled ? 0 : 1
            border.color: Qt.rgba(1, 1, 1, 0.25)

            Text {
                anchors.centerIn: parent
                text: qsTr("弹")
                color: liveBar.danmakuEnabled ? "#fff" : liveBar.barTextDim
                font.pixelSize: 13
                font.bold: true
            }

            HoverHandler { id: dmToggleHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: liveBar.danmakuToggled(!liveBar.danmakuEnabled) }
        }

        /* 输入框：官方直播的"网页全屏发送栏"样式 —— **没有底色、只有一条下边线**
           （`border-bottom:2px solid #fff8`；#fff8 = 白色 53% 透明），高 24px，
           placeholder 原文 `发个弹幕呗~`，maxlength 30，聚焦时换成主题色。
           注意这里用的是 QML 的 `property color` + 一个 1~2px 的矩形做下边线，
           不用 Rectangle.border（那样四条边都会画）。 */
        Item {
            id: dmBox

            width: 260
            height: 24

            Rectangle {
                id: dmUnderline

                anchors {
                    left: parent.left
                    right: parent.right
                    bottom: parent.bottom
                }
                height: 2
                color: dmInput.activeFocus ? liveBar.playerBlue : Qt.rgba(1, 1, 1, 0.53)
            }

            TextInput {
                id: dmInput

                anchors {
                    fill: parent
                    leftMargin: 4
                    rightMargin: 4
                    bottomMargin: 2
                }
                verticalAlignment: TextInput.AlignVCenter
                color: "#fff"
                font.pixelSize: 12
                selectByMouse: true
                clip: true
                /* 官方直播弹幕上限 30 字（B 站直播发送框的 maxlength） */
                maximumLength: 30
                /* 空的时候显示占位（QML 的 TextInput 没有 placeholderText，用一个 Text 顶） */
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: dmInput.text.length === 0 && !dmInput.activeFocus
                    text: qsTr("发个弹幕呗~")
                    color: Qt.rgba(1, 1, 1, 0.53)
                    font.pixelSize: 12
                }
                onAccepted: liveBar.sendDanmakuIfAny()
            }
        }

        /* 发送：官方是设计系统的 primary 按钮（`.bl-button--primary`）——
           实心品牌蓝、白字、小圆角 */
        Rectangle {
            width: 52
            height: 24
            radius: 4
            color: sendHover.hovered ? Qt.lighter(liveBar.liveBlue, 1.1) : liveBar.liveBlue

            Text {
                anchors.centerIn: parent
                text: qsTr("发送")
                color: "#fff"
                font.pixelSize: 12
            }

            HoverHandler { id: sendHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: liveBar.sendDanmakuIfAny() }
        }
    }

    /* 把"输入框里有内容才发"这条规则收在一处（输入框回车、点发送都走它） */
    function sendDanmakuIfAny() {
        var t = ("" + dmInput.text).trim()

        if (t === "") {
            return
        }

        liveBar.danmakuSend(t)
        dmInput.text = ""
    }

    /* =======================================================================
     * 内容行：官方 `.control-area`（bottom:3px; height:36px; padding:0 12px）
     * 里面分左组 / 右组；官方 `.right-area{flex:1; flex-direction:row-reverse}` = 贴右边。
     * ======================================================================= */
    Row {
        id: barRow

        anchors {
            left: parent.left
            leftMargin: liveBar.contentRowPaddingH
            bottom: parent.bottom
            bottomMargin: liveBar.contentRowBottom
        }
        /* 控制条本体只在"显示"时可见 */
        spacing: 0
        opacity: liveBar.shown ? 1 : 0
        visible: opacity > 0
        height: liveBar.contentRowHeight

        Behavior on opacity {
            NumberAnimation { duration: 200; easing.type: Easing.OutQuad }
        }

        /* ---- 播放/暂停（直播也能暂停，暂停后可以"回到直播"）---- */
        LiveIconButton {
            kind: liveBar.playing ? "pause" : "play"
            tip: liveBar.playing ? qsTr("暂停") : qsTr("播放")
            onTapped: liveBar.playPauseRequested()
        }

        /* ---- 音量：图标（点了静音）+ 悬停展开的滑条 ---- */
        Item {
            width: 36
            height: barRow.height

            LiveIconButton {
                anchors.centerIn: parent
                kind: (liveBar.muted || liveBar.volume <= 0.001) ? "mute" : "volume"
                tip: liveBar.muted ? qsTr("取消静音") : qsTr("静音")
                onTapped: liveBar.muteRequested()
            }

            /* 滑条：默认收着，hover 展开（B 站直播的观感） */
            Rectangle {
                id: volTrack

                anchors {
                    left: parent.right
                    leftMargin: 2
                    verticalCenter: parent.verticalCenter
                }
                width: volArea.containsMouse ? 84 : 0
                height: 4
                radius: 2
                color: Qt.rgba(1, 1, 1, 0.25)
                clip: true

                Behavior on width {
                    NumberAnimation { duration: 150; easing.type: Easing.OutQuad }
                }

                Rectangle {
                    width: Math.max(0, Math.min(1, liveBar.muted ? 0 : liveBar.volume)) * volTrack.width
                    height: parent.height
                    radius: 2
                    color: liveBar.livePink
                }

                Rectangle {
                    /* 拖动的圆点 */
                    x: Math.max(0, Math.min(volTrack.width - width,
                            liveBar.volume * volTrack.width - width / 2))
                    anchors.verticalCenter: parent.verticalCenter
                    width: 10
                    height: 10
                    radius: 5
                    color: "#fff"
                    visible: volArea.containsMouse
                }

                MouseArea {
                    id: volArea

                    anchors.fill: parent
                    anchors.margins: -8        /* 命中区往外扩一点，4px 高的条子太难按 */
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onPressed: function (mouse) { liveBar.volumeRequested(valueAt(mouse.x)) }
                    onPositionChanged: function (mouse) {
                        if (pressed)
                            liveBar.volumeRequested(valueAt(mouse.x))
                    }
                    function valueAt(x) {
                        return Math.max(0, Math.min(1, (x + 8) / volTrack.width))
                    }
                }
            }
        }

        /* ---- ● 直播中 · 状态（真读数）---- */
        Item {
            width: liveStatusRow.width + 6
            height: barRow.height

            Row {
                id: liveStatusRow

                anchors.centerIn: parent
                spacing: 6

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 7
                    height: 7
                    radius: 3.5
                    /* 官方活体状态红（--Re5 的浅色主题值；旧代码里那个 #FA5A57 是近似） */
                    color: "#F85A54"

                    /* 呼吸一下，强调"在直播"（B 站直播那颗红点也是闪的） */
                    SequentialAnimation on opacity {
                        loops: Animation.Infinite
                        NumberAnimation { to: 0.35; duration: 900; easing.type: Easing.InOutQuad }
                        NumberAnimation { to: 1.0; duration: 900; easing.type: Easing.InOutQuad }
                    }
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("直播中") + (liveBar.liveStateText !== "" ? " · " + liveBar.liveStateText : "")
                    color: liveBar.barText
                    font.pixelSize: 12
                }
            }
        }
    }

    /*
     * 右组：官方 `.right-area`（`flex:1; flex-direction:row-reverse`）—— 贴着控制条右边。
     * 里面依次是：清晰度 · 弹幕开关 · 统计信息 · 网页全屏 · 全屏。
     */
    Row {
        id: rightArea

        anchors {
            right: parent.right
            rightMargin: liveBar.contentRowPaddingH
            bottom: parent.bottom
            bottomMargin: liveBar.contentRowBottom
        }
        spacing: 0
        opacity: liveBar.shown ? 1 : 0
        visible: opacity > 0
        height: liveBar.contentRowHeight

        Behavior on opacity {
            NumberAnimation { duration: 200; easing.type: Easing.OutQuad }
        }

        /* ---- 清晰度：官方是 `.text`（line-height:36px; padding:0 6px）---- */
        Item {
            id: qualityItem

            width: qualityRow.width + liveBar.contentRowPaddingH
            height: rightArea.height

            Row {
                id: qualityRow

                anchors.centerIn: parent
                spacing: 4

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: liveBar.qualityLabel
                    color: qualityHover.hovered || qualityPopup.opened ? "#fff" : liveBar.barText
                    font.pixelSize: 12
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "▾"
                    color: qualityHover.hovered || qualityPopup.opened ? "#fff" : liveBar.barTextDim
                    font.pixelSize: 10
                }
            }

            HoverHandler { id: qualityHover; cursorShape: Qt.PointingHandCursor }
            TapHandler {
                onTapped: {
                    qualityPopup.opened = !qualityPopup.opened

                    /* 打开的那一刻通知外面（HLS 下外面会"按需"去解析 master 清单） */
                    if (qualityPopup.opened)
                        liveBar.qualityMenuOpened()
                }
            }
        }

        /* ---- 弹幕开关（官方控制条里的"弹幕"那颗）---- */
        LiveIconButton {
            kind: "danmaku"
            tip: qsTr("弹幕开关")
            active: liveBar.danmakuEnabled
            onTapped: liveBar.danmakuToggled(!liveBar.danmakuEnabled)
        }

        /* ---- 统计信息（复用点播那套面板：数据是真的、曲线也是真的）---- */
        LiveIconButton {
            kind: "info"
            tip: qsTr("统计信息")
            onTapped: liveBar.infoRequested()
        }

        /* ---- 网页全屏（把页面其它部分藏掉、播放器铺满窗口；这里等同"窗口最大化 + 隐藏标题栏"）---- */
        LiveIconButton {
            kind: "webfull"
            tip: qsTr("网页全屏")
            onTapped: liveBar.webFullscreenRequested()
        }

        /* ---- 全屏 ---- */
        LiveIconButton {
            kind: "full"
            tip: qsTr("全屏")
            onTapped: liveBar.fullscreenRequested()
        }
    }

    /* =======================================================================
     * 清晰度菜单（B 站直播那颗"清晰度"点开的样子）
     *
     * 直播**没有清单**，所以这里的档位只能来自服务端给的一份列表
     * （本地测试服务 mock-live 提供 /live/streams.json，见 LivePlayerView 里的拉取逻辑）。
     * 拿不到列表时只有"当前地址"一项 —— 那是诚实的，不编造档位。
     * ======================================================================= */
    LiveMenu {
        id: qualityPopup

        /* 官方 `.panel{left:50%; transform:translate(-50%,-100%)}` —— **水平居中在那颗按钮上方** */
        x: qualityItem.mapToItem(liveBar, qualityItem.width / 2, 0).x - width / 2
        y: -height - 6

        model: liveBar.qualityList
        currentKey: liveBar.qualityLabel

        onPicked: function (item) {
            qualityPopup.opened = false
            /* 整项回传（HLS 需要里面的 streamIndex 走清单内换流，FLV 需要 url 走重连） */
            liveBar.qualityRequested(item)
        }
    }
}
