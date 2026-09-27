// ===========================================================================
// LivePlayerView —— **直播播放器的主界面**（复刻 B 站直播播放器，用户要求另写一套）
//
// 【和 PlayerView.qml（点播）的关系：没有关系】
//   用户原话："直播播放器 ui 不要使用目前写好的这个哟，重新写一个"。
//   两张界面的信息结构本来就不同，硬套只会得到四不像：
//     点播：顶部标题栏 + 底部进度条（可拖）+ 时间 + 倍速 + 选集 + 高能进度条 + 清晰度（清单内换档）
//     直播（本文件）：**没有进度条、没有时长、不能 seek**；清晰度是"换一条 .flv 重连"；
//                     要"直播中"状态、弹幕输入框、"回到直播"（暂停后追回直播进度）、自动重连
//
// 【参考实现能给的 / 不能给的】（工作区审计结论，见 docs/LIVE-PLAYER.md）
//   * 能给的：B 站直播页框架（front/hilihili-live）：播放器盒 1130×630/rgba(0,0,0,.7)/圆角 4、
//     底部 56px 渐变条、弹幕层契约（15° 斜向轨道 + 字号/颜色/描边变量）、关注按钮 89×32、
//     以及全部设计令牌（#00a1d6 / #00aeec / #FB7299 / hsla(0,0%,8%,.9) …）；
//   * **不能给的**：真正的直播控制栏在参考里是个**空 div**（web-player-controller-wrap），
//     直播中/人气徽标、清晰度档位表在工作区里都不存在 —— 那几块按 B 站直播的实际观感设计，
//     并且**凡是能拿真数据的地方一律用真数据**（分辨率/码率/缓存速度/帧率都从框架来），
//     不编"12345 人气"这种假数字。
//
// 【数据流】
//   LiveMain 设 source → livePlayer.source（同时先置 liveMode=true，缓冲才会按直播收小）
//   → 拉一次 <dir>/streams.json 得到清晰度档位 → 点某一档 = 换 source 重连（HTTP-FLV 没有清单）
//   → 播放中出错/断流 → 自动重连（指数退避，最多 5 次）并在画面上给一行提示
// ===========================================================================
import QtQuick
import CicadaPlayer

Item {
    id: liveView

    /* 直播地址（由 LiveMain 写进来） */
    property string source: ""
    /* 是否全屏（跟着窗口状态走，用来切控制栏尺寸和"网页全屏"按钮状态） */
    property bool fullscreen: false
    /* "网页全屏"（把窗口内容区的 chrome 藏掉）—— 由 LiveMain 决定具体行为 */
    property bool webFullscreen: false
    /* 供外面读：当前清晰度标签、档位表、是否在重连 */
    readonly property string qualityLabel: liveView.currentQualityLabel
    readonly property var qualityList: liveView.currentQualityList
    readonly property bool reconnecting: reconnectTimer.running || liveView.reconnectAttempt > 0

    /*
     * =======================================================================
     * 【FLV 直播 / HLS 直播 的差别，就在这里】
     *
     *   FLV（HTTP-FLV）：**没有清单**。地址就是一档流，档位表只能靠服务端另外给的
     *       streams.json（本地测试服务提供）。切档 = 换一条地址 → **断开重连**
     *       （黑一下、等一个 GOP，约 1 秒）。core 里的 SwitchStreamAligned 对 FLV 不生效
     *       （那条路只有 HLS/DASH 的清单管理器实现）。
     *
     *   HLS（HTTP-HLS）：m3u8 里**自带多码率清单**（master）。核心的 HLS 管线会把它解析成
     *       MediaInfoGet → `player.qualities`，于是：
     *         * 档位表直接用 qualities（label/width/height/bandwidth 都是框架报的真实值）；
     *         * 切档走 `player.selectQuality(streamIndex)` —— **清单内换流，不用重连**，
     *           画面不会黑，切换在分段边界完成（这是 HLS 直播相对 FLV 直播的最大优势）。
     *       所以下面所有"档位/当前档/切档"的地方都按协议分了两条路。
     * =======================================================================
     */
    readonly property bool isHls: ("" + source).toLowerCase().indexOf(".m3u8") >= 0

    /* FLV 那份档位表（来自 streams.json）；HLS 不用它 */
    property var renditions: []

    /*
     * HLS：自己解析 master 清单得到的档位表（**兜底**，见下面两条路的说明）。
     * 每项形状和 FLV 的一样（带 url），所以"换流重连"那条切换逻辑可以直接复用。
     */
    property var hlsParsedRenditions: []

    /*
     * =======================================================================
     * HLS 的档位有**两条路**，谁通用谁：
     *
     *   ① 框架上报（首选）：核心的 HLS 管线解析 master 后经 MediaInfoGet 报上来 →
     *      `player.qualities`（label/width/height/bandwidth/streamIndex 都是真值）。
     *      切档走 `player.selectQuality(streamIndex)` = **清单内换流**，不重连、不黑屏。
     *
     *   ② 自己解析 master（兜底）：框架**不一定**会上报（直播 HLS 这条路没实测过，
     *      而且有些实现只在点播时列 representation）。所以 QML 自己也拉一次 master.m3u8，
     *      把 `#EXT-X-STREAM-INF` 解析成档位表。切档就退化成"换一条 variant 地址重连"
     *      （和 FLV 那条路一样）。
     *
     * 【为什么必须有②】用户实测报的正是"HLS 下清晰度菜单没有选项、切不了"——
     * 之前菜单绑的是 FLV 专用的 renditions（HLS 下恒为空），而框架上报又不保证有。
     * 两条路都接上之后，菜单在任何情况下都有档位。
     * =======================================================================
     */
    readonly property var hlsQualityList: {
        /* ① 框架上报的优先 */
        var list = []

        if (player !== null && player.qualities !== undefined) {
            for (var i = 0; i < player.qualities.length; ++i) {
                var q = player.qualities[i]
                list.push({
                    "name": q.label,
                    "label": q.label,
                    "width": q.width,
                    "height": q.height,
                    "videoBitrate": q.bandwidth,
                    "streamIndex": q.streamIndex,
                    /* 编码短名（内核归一化后给的；认不出来是空串）：菜单用它画徽标，
                       没有徽标时 LiveMenu 不渲染那一块 —— 不猜、不写"未知"。 */
                    "codecLabel": q.codecLabel,
                    "url": ""
                })
            }
        }

        if (list.length > 0)
            return list

        /* ② 退回自己解析的 master（按高度从高到低，和 B 站"原画在最上面"一致） */
        var parsed = hlsParsedRenditions.slice(0)

        if (parsed.length > 0) {
            parsed.sort(function (a, b) { return (b.height || 0) - (a.height || 0) })
            return parsed
        }

        /* ③ 什么都没有（单档清单且同目录没有 master，或还没去拉）：**显示当前这一档**，
              而不是一个空菜单 —— 用户至少知道现在在播什么、为什么切不了。 */
        return singleVariantFallback()
    }

    /*
     * 单档回退项：名字优先取 URL 里那一层目录（…/1080p/index.m3u8 → "1080P"），
     * 没有就从真实分辨率推；streamIndex = -1 表示"**不能**清单内换流"（见 switchQuality）。
     */
    function singleVariantFallback() {
        var label = ""
        var m = /\/(\d{3,4}p)\//.exec(("" + source).toLowerCase())

        if (m !== null)
            label = m[1].toUpperCase()

        if (label === "" && player !== null && player.stats !== undefined && player.stats.height > 0)
            label = player.stats.height + "P"

        if (label === "")
            label = qsTr("当前档")

        return [{ "name": label, "label": label, "url": source,
                  "width": 0, "height": 0, "videoBitrate": 0, "streamIndex": -1 }]
    }

    /* 菜单用的档位表：按协议二选一 */
    readonly property var currentQualityList: isHls ? hlsQualityList : renditions

    /* 当前档标签：HLS 读框架的 qualityIndex（永远是真在播的那一档）；FLV 用我们自己记的 */
    property string flvQualityLabel: qsTr("原画")

    readonly property string currentQualityLabel: {
        if (!isHls)
            return flvQualityLabel

        var p = player

        if (p === null || p.qualities === undefined || p.qualities.length === 0)
            return qsTr("清晰度")

        var i = p.qualityIndex

        if (i < 0 || i >= p.qualities.length)
            i = 0

        return p.qualities[i].label
    }

    signal closeRequested()

    /* ---------------- 状态 ---------------- */

    /* 播放器组件本身（CicadaPlayerItem：解码 + 上屏 + 统计，和点播用的是同一个组件） */
    property alias player: livePlayer

    /* renditions 已经在上面声明过（FLV 档位表），这里不再重复声明 ——
       重复声明 QML 会直接报 "Duplicate property name" 编译失败。 */
    property string roomTitle: qsTr("直播")
    property string roomHost: ""
    /* 重连次数（>0 表示正在重连流程里；成功后清零） */
    property int reconnectAttempt: 0
    readonly property int maxReconnectAttempt: 5

    /* 控制栏显示状态（鼠标 3 秒不动就收） */
    property bool controlsShown: true

    /* =======================================================================
     * 视频画面（黑色底 + 等比适配：直播源分辨率五花八门，不能拉伸）
     * ======================================================================= */
    Rectangle {
        anchors.fill: parent
        color: "#000000"
    }

    CicadaPlayerItem {
        id: livePlayer

        anchors.fill: parent
        /*
         * 【★这一行是"首页填了地址却一直显示连接中"的根因★】
         *
         * 本组件对外只暴露一个 `source` 属性（LiveMain 写它），里层的 CicadaPlayerItem
         * **必须**用绑定接上。上一版忘了这一句：`onSourceChanged` 里只去拉了清晰度清单
         * （所以日志里能看到"4 档"），却**从没把地址交给播放器** —— 表现就是
         * "直播窗口打开着、界面显示连接中、什么都没有；点了切换清晰度反而能播"
         * （因为 switchQuality() 是直接给 player.source 赋值的，绕过了这条路）。
         *
         * 【必须用绑定 + 所有换地址的地方都改 liveView.source】
         * QML 里"给一个已经绑定的属性赋值"会**打断绑定**。所以重连、换清晰度这两处
         * 都不能再写 `player.source = …`，而是写 `liveView.source = …`，让绑定把它传下来。
         */
        source: liveView.source
        /* 画面适配由组件内部按源分辨率做等比适配（CicadaPlayerItem 没有 fillMode 属性，
           点播那套也是 anchors.fill: parent 直接铺满 —— 见 PlayerView.qml:180-189）。 */

        /*
         * 直播缓冲策略开关：**必须在 source 之前**置真（见 CicadaPlayerItem.h 里
         * liveMode 那一大段）。顺序写反了不会崩，但会按点播的 40 秒缓冲跑 ——
         * 那等于"延时 40 秒的直播"。组件里对"设晚了一步"也会打日志说明。
         */
        liveMode: true

        /*
         * 音量默认 0.9：官方直播播放器里 video 元素的 `volume = .9`，用户设置项默认值也是
         * `volume:{value:90}`（0~100）。照这个来，不做 100% 起播（那样一进来就顶满音量）。
         */
        volume: 0.9

        onStatusChanged: {
            if (status === CicadaPlayerItem.Error) {
                console.warn("[live] 播放出错：" + errorString)
                liveView.scheduleReconnect("播放出错")
            } else if (status === CicadaPlayerItem.End) {
                console.warn("[live] 流结束（服务端断开或推到结尾）")
                liveView.scheduleReconnect("流结束")
            } else if (status === CicadaPlayerItem.Playing) {
                if (liveView.reconnectAttempt > 0) {
                    liveView.reconnectAttempt = 0
                    liveView.notify(qsTr("已重新连上直播"))
                }
            }
        }

        onErrorStringChanged: if (errorString !== "") console.warn("[live] errorString: " + errorString)
    }

    /* =======================================================================
     * 弹幕层（复用 DanmakuView：算法在 C++，QML 只喂时间/位置）
     *
     * 直播的弹幕本来是实时流（B 站走 WebSocket）。本地 mock-live **没有**弹幕服务，
     * 所以这里：
     *   * 用 generateSample() 造一批本地示例弹幕，让"弹幕上屏 + 输入框 + 开关"这条链
     *     在直播界面里是真的能跑通的（不是画个假框）；
     *   * liveUrl 留给"有实时弹幕服务"的情况（填上就连，收不到就不连，不报错）。
     * ======================================================================= */
    DanmakuView {
        id: danmakuLayer

        anchors.fill: parent
        player: livePlayer
        fullscreen: liveView.fullscreen
        danmakuEnabled: liveView.danmakuEnabled

        pointerInside: activityArea.containsMouse
        pointerPosition: activityArea.mapToItem(danmakuLayer, activityArea.mouseX, activityArea.mouseY)

        Component.onCompleted: {
            /* 60 秒的示例弹幕（和点播界面那条"本地示例"一个路子，日志里说明白） */
            liveView.logDanmakuSample()
        }
    }

    property bool danmakuEnabled: true

    function logDanmakuSample() {
        console.warn("[live] 本地示例弹幕已生成（mock-live 不带弹幕服务，实时弹幕要另外接 WebSocket）")
        danmakuLayer.generateSample(120, 60)
    }

    /* =======================================================================
     * 鼠标活动层：动一下就出控制栏（3 秒后收起），收起时藏指针
     * ======================================================================= */
    MouseArea {
        id: activityArea

        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: liveView.controlsShown ? Qt.ArrowCursor : Qt.BlankCursor

        onPositionChanged: liveView.kickControls()
        onPressed: liveView.kickControls()
        onDoubleClicked: liveView.toggleFullscreenRequested()
    }

    Timer {
        id: hideControlsTimer
        interval: 3000
        onTriggered: {
            /* 指针压在控制栏/弹幕输入框/菜单上时不收（不然正要点按钮它就没了） */
            if (!controlBarHover.containsMouse && !anyMenuOpen)
                liveView.controlsShown = false
        }
    }

    readonly property bool anyMenuOpen: infoPanel.opened

    function kickControls() {
        controlsShown = true
        hideControlsTimer.restart()
    }

    /* =======================================================================
     * 顶部信息（左上：头像/房间名/关注；右上：统计信息开关那类"轻按钮"）
     *
     * 参考里 .player-top-mask 是 240px 的"透明→黑"渐变；关注按钮 89×32、圆角 26、
     * 头像 24×24 圆形。这里照这套数值做，但**不编造人气/粉丝数** ——
     * 房间名用地址推导（可读），下面一行放真实的分辨率/码率。
     * ======================================================================= */
    Rectangle {
        id: topMask

        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
        }
        height: 120
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, 0.45) }
            GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.0) }
        }
        opacity: liveView.controlsShown || topHover.hovered ? 1 : 0
        visible: opacity > 0

        Behavior on opacity {
            NumberAnimation { duration: 200; easing.type: Easing.OutQuad }
        }
    }

    HoverHandler { id: topHover }

    Row {
        id: roomRow

        anchors {
            left: parent.left
            leftMargin: 12
            top: parent.top
            topMargin: 12
        }
        spacing: 10
        opacity: liveView.controlsShown || topHover.hovered ? 1 : 0
        visible: opacity > 0

        Behavior on opacity {
            NumberAnimation { duration: 200; easing.type: Easing.OutQuad }
        }

        /* 头像：没有真实头像素材（工作区里没有），用一个圆形色块 + "LIVE"，
           而不是随便找一张网图假装是主播头像。
           尺寸按官方 `.blive-avatar-face` 的 32×32。 */
        Rectangle {
            width: 32
            height: 32
            radius: 16
            color: "#FF6699"
            anchors.verticalCenter: parent.verticalCenter

            Text {
                anchors.centerIn: parent
                text: "LIVE"
                color: "#fff"
                font.pixelSize: 9
                font.bold: true
            }
        }

        Column {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2

            Text {
                text: liveView.roomTitle
                color: "#fff"
                font.pixelSize: 14
                font.bold: true
                elide: Text.ElideRight
                /* 房间名宽度上限按官方 `.room-owner-username{max-width:150px}` 的量级取
                   （直播播放器顶部要留出右侧热度/按钮的位置） */
                width: Math.min(implicitWidth, 240)
            }

            Row {
                spacing: 8

                Text {
                    text: liveView.liveStateText
                    color: Qt.rgba(1, 1, 1, 0.75)
                    font.pixelSize: 11
                }

                Text {
                    visible: liveView.roomHost !== ""
                    text: liveView.roomHost
                    color: Qt.rgba(1, 1, 1, 0.45)
                    font.pixelSize: 11
                }
            }
        }

        /* 关注按钮（89×32/圆角 26/rgba(0,0,0,.4)）—— 本播放器没有账号体系，
           点一下给出说明，不假装关注成功 */
        Rectangle {
            width: 89
            height: 32
            radius: 26
            color: followHover.hovered ? Qt.rgba(1, 1, 1, 0.16) : Qt.rgba(0, 0, 0, 0.4)
            anchors.verticalCenter: parent.verticalCenter

            Row {
                anchors.centerIn: parent
                spacing: 5

                /* 加号：两根小矩形（不引素材） */
                Item {
                    width: 12
                    height: 12
                    anchors.verticalCenter: parent.verticalCenter

                    Rectangle {
                        anchors.centerIn: parent
                        width: 12
                        height: 2
                        color: "#fff"
                    }

                    Rectangle {
                        anchors.centerIn: parent
                        width: 2
                        height: 12
                        color: "#fff"
                    }
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("关注")
                    color: "#fff"
                    font.pixelSize: 12
                }
            }

            HoverHandler { id: followHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: liveView.notify(qsTr("本播放器没有账号体系，关注功能未接入")) }
        }
    }

    /* 真实读数（分辨率 · 码率 · 缓存速度 · 接收帧率）：全部来自框架的 stats */
    readonly property string liveStateText: {
        var s = player ? player.stats : null

        if (s === null || s === undefined || s.width === undefined)
            return qsTr("连接中…")

        var parts = []
        parts.push(s.width + "x" + s.height)

        if (s.videoBitrate > 0)
            parts.push(Math.round(s.videoBitrate / 1000) + "kbps")

        if (player.downloadSpeed > 0)
            parts.push(player.downloadSpeedText)

        if (s.renderFps > 0)
            parts.push(s.renderFps.toFixed(0) + " fps")

        return parts.join(" · ")
    }

    /* =======================================================================
     * 底部控制条（另写的 LiveControlBar，见那个文件）
     * ======================================================================= */
    LiveControlBar {
        id: controlBar

        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }
        /* 高度由控制条自己按 fullscreen 算（见 LiveControlBar 里的 height 绑定），
           这里不重复写一份，免得以后只改了一边 */
        shown: liveView.controlsShown || controlBarHover.containsMouse
        playing: livePlayer.playing
        muted: livePlayer.muted
        volume: livePlayer.volume
        fullscreen: liveView.fullscreen
        liveStateText: liveView.liveStateText
        qualityLabel: liveView.currentQualityLabel
        /*
         * 【★这里曾经写的是 liveView.renditions —— HLS 直播下清晰度菜单因此是空的★】
         *
         * renditions 只有 **FLV** 那条路才填（来自服务端 streams.json）；HLS 的档位来自
         * master 清单，由框架上报到 `player.qualities`，整理后放在 currentQualityList 里。
         * 菜单绑到 renditions 上，HLS 就永远是空列表 → "清晰度菜单没有选项、切不了"。
         * 正确写法就是下面这一行：按协议自己挑（`isHls ? hlsQualityList : renditions`）。
         */
        qualityList: liveView.currentQualityList
        danmakuEnabled: liveView.danmakuEnabled

        onPlayPauseRequested: livePlayer.togglePause()
        onMuteRequested: livePlayer.muted = !livePlayer.muted
        onVolumeRequested: function (v) {
            livePlayer.muted = false
            livePlayer.volume = v
        }
        onQualityRequested: function (item) {
            liveView.switchQuality(item)
        }
        /*
         * 用户点开清晰度菜单：HLS 下如果框架没上报档位、自己也还没解析过 master，
         * 这时才去拉一次（**不在起播时拉** —— 起播那一刻不引入任何额外网络请求）。
         */
        onQualityMenuOpened: {
            if (isHls && hlsParsedRenditions.length === 0 && source !== "")
                fetchHlsMaster(source)
        }
        onDanmakuToggled: function (enabled) {
            liveView.danmakuEnabled = enabled
        }
        onDanmakuSend: function (text) {
            /* 本地示例弹幕：直接上屏（sendDanmaku 是引擎的真函数，会按滚动弹幕走） */
            danmakuLayer.sendDanmaku(text, { "mode": "scroll" })
            liveView.notify(qsTr("弹幕已上屏（本地示例通道）"))
        }
        onInfoRequested: {
            infoPanel.opened = !infoPanel.opened
            liveView.kickControls()
        }
        onWebFullscreenRequested: liveView.webFullscreenRequested()
        onFullscreenRequested: liveView.toggleFullscreenRequested()
    }

    /* 控制条 hover 判据：用一层不可见的 MouseArea 报"指针在控制条区域内"。
       位置刻意贴住底部那条（高度 = 控制条高度 + 弹幕输入那 34px）。 */
    MouseArea {
        id: controlBarHover

        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }
        height: controlBar.height + 40
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
        onContainsMouseChanged: if (containsMouse) liveView.kickControls()
    }

    /* =======================================================================
     * 暂停之后的"回到直播"
     *
     * 直播暂停 = 画面停在那一刻；再按播放会**从暂停处继续**（比主播晚）。
     * B 站直播的做法是给一个"回到直播"按钮，点了跳到最新。HTTP-FLV 不能 seek，
     * 所以这里的实现是**重连一次**（重新连上就是最新的直播位置，正好是我们要的语义）。
     * 只在"暂停过"或"落后于直播"时显示。
     *
     * 尺寸/配色按官方值：高 22px、padding 0 8px、圆角 12px、底色 rgba(51,51,51,.8)、
     * 字 12px，左边一颗 6px 的红点 #ff5243。
     * ======================================================================= */
    Rectangle {
        id: backToLive

        anchors {
            right: parent.right
            rightMargin: 9
            bottom: controlBar.top
            bottomMargin: 12
        }
        width: backRow.width + 16
        height: 22
        radius: 12
        color: backHover.hovered ? Qt.rgba(51 / 255, 51 / 255, 51 / 255, 1.0)
                                 : Qt.rgba(51 / 255, 51 / 255, 51 / 255, 0.8)
        visible: !livePlayer.playing && livePlayer.hasMedia && !liveView.reconnecting

        Behavior on color {
            ColorAnimation { duration: 150 }
        }

        Row {
            id: backRow

            anchors.centerIn: parent
            spacing: 5

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 6
                height: 6
                radius: 3
                color: "#ff5243"
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("回到直播")
                color: "#fff"
                font.pixelSize: 12
            }
        }

        HoverHandler { id: backHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: liveView.reconnectNow(qsTr("回到直播")) }
    }

    /* =======================================================================
     * 重连（直播必需：网络一抖/服务端重启，流就断了）
     * ======================================================================= */
    Timer {
        id: reconnectTimer
        interval: 1000
        onTriggered: liveView.reconnectNow(qsTr("自动重连"))
    }

    function scheduleReconnect(why) {
        if (source === "" || reconnectAttempt >= maxReconnectAttempt)
            return

        /* 已经排了就不再排（status 会连着变几次，不去重会一次断流触发好几条重连） */
        if (reconnectTimer.running)
            return

        reconnectAttempt++
        /* 指数退避：1s、2s、4s、8s…（第一下快，后面对服务端友好） */
        reconnectTimer.interval = Math.min(1000 * Math.pow(2, reconnectAttempt - 1), 8000)
        reconnectTimer.start()
        notify(qsTr("%1：%2 秒后重连（第 %3/%4 次）").arg(why)
                                                    .arg(Math.round(reconnectTimer.interval / 1000))
                                                    .arg(reconnectAttempt)
                                                    .arg(maxReconnectAttempt))
    }

    /*
     * 立刻重连（自动重连和"回到直播"共用）。
     *
     * 【为什么要"清一下再设"】CicadaPlayerItem::setSource() 对**同一个地址**会直接返回
     * （它认为你在重复设置同一路片源）。直播重连恰恰就是要用同一个地址重新连 ——
     * 所以先把 source 置空（组件会 destroyPlayer），下一轮再设回那个地址。
     *
     * 【为什么改的是 liveView.source 而不是 player.source】
     * player.source 现在绑在 liveView.source 上（见上面那段说明）。直接给 player.source
     * 赋值会把绑定打断 —— 那样"重连一次之后，以后再换地址就再也传不下去了"。
     */
    function reconnectNow(why) {
        reconnectTimer.stop()

        if (source === "")
            return

        var target = source
        console.warn("[live] " + why + " → 重连 " + target)
        liveView.source = ""
        Qt.callLater(function () {
            liveView.source = target
        })
    }

    /* =======================================================================
     * 清晰度（HLS 那条路）：自己解析 master 清单
     *
     * 为什么自己解析：见 hlsQualityList 的说明 —— 框架上报是首选，但不保证有，
     * 而"HLS 下菜单没选项"正是之前用户实测的问题。自己解析一份就永久不空。
     *
     * 解析规则（RFC 8216 里 master playlist 的最小可用子集）：
     *   #EXT-X-STREAM-INF:BANDWIDTH=5128000,RESOLUTION=1920x1080,NAME="原画"
     *   1080p/index.m3u8                     ← 下一行非 # 开头就是这一档的地址（相对路径要拼上目录）
     * 取 BANDWIDTH（没有就 AVERAGE-BANDWIDTH）、RESOLUTION，NAME 当显示名；
     * 没有 NAME 就按高度写 "1080P"，再没有就退到带宽。
     *
     * 【★用户填的是"单档"地址怎么办（实测反馈）★】
     *   `…/live/hls/master.m3u8`（多码率 master）能播、能列档位 ✓；
     *   但 `…/live/hls/1080p/index.m3u8`（**单档媒体清单**）里**没有** #EXT-X-STREAM-INF，
     *   解析出来 0 档 → 菜单显示"没有清晰度清单"（用户说的"无法解析"），
     *   而且裸单档直播清单在核心里也不一定能起播。
     *   所以这里多做一件事：**自动把单档地址升级成同目录的 master.m3u8**
     *   （`…/hls/1080p/index.m3u8` → `…/hls/master.m3u8`），换一次源、之后一切照 master 走。
     *   找不到 master 才退化成"只有当前这一档"（菜单里也会明说）。
     * ======================================================================= */

    /* 单档地址 → 候选 master 地址（先父目录、再同目录；同名 master 不再递归） */
    function siblingMasterCandidates(url) {
        var u = ("" + url)
        var q = u.indexOf("?")
        var query = q >= 0 ? u.substring(q) : ""
        var base = q >= 0 ? u.substring(0, q) : u
        var slash = base.lastIndexOf("/")

        if (slash < 0)
            return []

        var file = base.substring(slash + 1).toLowerCase()
        var dir = base.substring(0, slash + 1)                 /* …/hls/1080p/ */
        var parent = dir.replace(/[^\/]+\/$/, "")              /* …/hls/ */
        var out = []

        if (file !== "master.m3u8") {
            out.push(parent + "master.m3u8" + query)
            out.push(dir + "master.m3u8" + query)
        }

        return out
    }

    /* 探一下候选地址，是 200 且**真的含 #EXT-X-STREAM-INF** 才升级（避免把另一个单档当 master） */
    function upgradeVariantToMaster(url) {
        var cands = siblingMasterCandidates(url)

        if (cands.length === 0)
            return

        var index = 0

        function tryNext() {
            if (index >= cands.length) {
                console.warn("[live] 这个 HLS 地址是单档清单，同目录也没找到 master.m3u8；"
                             + "清晰度菜单里只会显示当前这一档（想切档请用 master.m3u8 地址）")
                return
            }

            var cand = cands[index++]
            var xhr = new XMLHttpRequest()
            xhr.open("GET", cand, true)
            xhr.timeout = 4000

            xhr.onreadystatechange = function () {
                if (xhr.readyState !== XMLHttpRequest.DONE)
                    return

                if (xhr.status === 200 && ("" + xhr.responseText).indexOf("#EXT-X-STREAM-INF") >= 0) {
                    /* 只在这一刻、且用户没又换了地址时升级（防抖：避免把新地址覆盖掉） */
                    if (liveView.source === url) {
                        console.warn("[live] 检测到单档 HLS 地址，自动升级到多码率清单：" + cand
                                     + "（这样清晰度列表和切档才有意义）")
                        liveView.source = cand
                    }
                    return
                }

                if (xhr.status === 200 && ("" + xhr.responseText).indexOf("#EXT-X-STREAM-INF") < 0) {
                    console.warn("[live] 候选 " + cand + " 是单档清单（没有 #EXT-X-STREAM-INF），跳过")
                }

                tryNext()
            }

            xhr.send()
        }

        tryNext()
    }

    /* 把 `KEY=VALUE,KEY="VALUE"` 这种属性串解析成对象（引号内的逗号不切） */
    function parseHlsAttributes(text) {
        var attrs = {}
        var re = /([A-Z0-9-]+)=("[^"]*"|[^,]*)/g
        var m

        while ((m = re.exec(text)) !== null) {
            var v = m[2]

            if (v.length >= 2 && v.charAt(0) === '"' && v.charAt(v.length - 1) === '"')
                v = v.substring(1, v.length - 1)

            attrs[m[1]] = v
        }

        return attrs
    }

    function fetchHlsMaster(masterUrl) {
        var xhr = new XMLHttpRequest()
        xhr.open("GET", masterUrl, true)
        xhr.timeout = 5000

        xhr.onreadystatechange = function () {
            if (xhr.readyState !== XMLHttpRequest.DONE)
                return

            if (xhr.status !== 200) {
                console.warn("[live] 拉 HLS master 失败（HTTP " + xhr.status + "）：" + masterUrl
                             + "；清晰度菜单将只有框架上报的那份（可能为空）")
                return
            }

            var parsed = parseHlsMaster(xhr.responseText, masterUrl)
            liveView.hlsParsedRenditions = parsed

            var names = []
            for (var i = 0; i < parsed.length; ++i)
                names.push(parsed[i].label)

            console.warn("[live] HLS master 解析：" + parsed.length + " 档（" + names.join(" / ") + "）")
        }

        xhr.send()
    }

    function parseHlsMaster(text, baseUrl) {
        var out = []
        var dir = baseUrl.substring(0, baseUrl.lastIndexOf("/") + 1)
        var lines = ("" + text).split(/\r?\n/)
        var attrs = null

        for (var i = 0; i < lines.length; ++i) {
            var line = lines[i].trim()

            if (line.indexOf("#EXT-X-STREAM-INF:") === 0) {
                attrs = parseHlsAttributes(line.substring("#EXT-X-STREAM-INF:".length))
                continue
            }

            if (line === "" || line.charAt(0) === "#" || attrs === null)
                continue

            /* 这一行就是档位地址 */
            var url = line.indexOf("://") > 0 ? line : dir + line
            var res = ("" + (attrs["RESOLUTION"] !== undefined ? attrs["RESOLUTION"] : "")).split("x")
            var w = Number(res[0]) || 0
            var h = Number(res[1]) || 0
            var bw = Number(attrs["BANDWIDTH"] !== undefined ? attrs["BANDWIDTH"]
                                                            : (attrs["AVERAGE-BANDWIDTH"] || 0))
            var label = attrs["NAME"] !== undefined ? attrs["NAME"]
                                                   : (h > 0 ? (h + "P") : (bw > 0 ? (bw + "bps") : url))

            out.push({
                "name": label,
                "label": label,
                "url": url,
                "width": w,
                "height": h,
                "videoBitrate": bw
            })

            attrs = null
        }

        return out
    }

    /* =======================================================================
     * 清晰度：HTTP-FLV 没有清单，档位来自服务端给的 streams.json
     *
     * 约定（本地测试服务 mock-live 就是这么提供的）：
     *     http://host:port/live/1080p.flv   →   http://host:port/live/streams.json
     * 也就是"把最后一段文件名换成 streams.json"。拿不到就退回"只有当前一路"，
     * 菜单里会写明"这个地址没有清晰度清单" —— 不编档位。
     * ======================================================================= */
    function streamsUrlFor(flvUrl) {
        var u = ("" + flvUrl)
        var q = u.indexOf("?")
        var query = q >= 0 ? u.substring(q) : ""
        var base = q >= 0 ? u.substring(0, q) : u
        var slash = base.lastIndexOf("/")

        if (slash < 0)
            return ""

        return base.substring(0, slash + 1) + "streams.json" + query
    }

    function fetchRenditions(flvUrl) {
        var url = streamsUrlFor(flvUrl)

        renditions = []
        if (url === "")
            return

        var xhr = new XMLHttpRequest()
        xhr.open("GET", url, true)
        xhr.timeout = 4000

        xhr.onreadystatechange = function () {
            if (xhr.readyState !== XMLHttpRequest.DONE)
                return

            if (xhr.status !== 200) {
                console.warn("[live] 没有清晰度清单（" + url + " → HTTP " + xhr.status
                             + "），清晰度菜单里只会有当前这一路")
                return
            }

            try {
                var j = JSON.parse(xhr.responseText)
                var list = (j && j.renditions) ? j.renditions : []
                liveView.renditions = list

                /* 当前档：地址完全一样的那个；否则用第一档的标签 */
                var label = ""
                for (var i = 0; i < list.length; ++i) {
                    if (list[i].url === flvUrl && list[i].label !== undefined) {
                        label = list[i].label
                        break
                    }
                }

                if (label === "" && list.length > 0)
                    label = list[0].label !== undefined ? list[0].label : list[0].name

                if (label !== "")
                    liveView.flvQualityLabel = label

                console.warn("[live] 清晰度清单：" + list.length + " 档（"
                             + list.map(function (r) { return r.label; }).join(" / ") + "）")
            } catch (e) {
                console.warn("[live] 清晰度清单解析失败：" + e)
            }
        }

        xhr.send()
    }

    /* 换档：
     *   HLS → 有 streamIndex → **清单内换流**（player.selectQuality，不重连、不黑屏）；
     *   FLV → 只有 url → 换一条 .flv 重连（HTTP-FLV 没有清单，只能这样）。
     */
    function switchQuality(item) {
        if (item === null || item === undefined)
            return

        /* ---- HLS：清单内换流 ---- */
        if (isHls) {
            if (item.streamIndex !== undefined && item.streamIndex >= 0) {
                console.warn("[live] 切换清晰度（HLS 清单内换流）→ " + item.label
                             + "（streamIndex=" + item.streamIndex + "）")
                notify(qsTr("正在切换到 %1…").arg(item.label))
                player.selectQuality(item.streamIndex)
                return
            }

            /*
             * 没有 streamIndex（框架没上报档位，手里只有"自己解析出来的地址"）：
             * **绝对不要**去加载那一档的 variant 地址 —— 裸单档直播清单核心不一定能起播
             * （用户实测："单档 1080p 地址无法解析"）。宁可说清楚，也不要把正在播的流换坏。
             */
            console.warn("[live] 这一档没有 streamIndex（框架没上报），不做清单内换流；"
                         + "要切档请用 master.m3u8 地址")
            notify(qsTr("这个地址不支持切档，请用 master.m3u8 地址"))
            return
        }

        /* ---- FLV：换流重连 ---- */
        var url = item.url !== undefined ? item.url : ""
        var name = item.label !== undefined ? item.label : ("" + item.name)

        if (url === "")
            return

        console.warn("[live] 切换清晰度 → " + name + "（" + url + "）")
        notify(qsTr("正在切换到 %1…").arg(name))

        /* 先记下目标档标签，界面立刻有反馈（不用等连上） */
        flvQualityLabel = name

        reconnectAttempt = 0
        /* 同样走 liveView.source（player.source 是绑定，赋值会打断它 —— 见 reconnectNow 的说明） */
        liveView.source = ""
        Qt.callLater(function () {
            liveView.source = url
        })
    }

    /* =======================================================================
     * 统计信息面板（**直接复用点播那一个** PlayerInfoPanel）：
     * 数据链路、采样、曲线都是同一份，没必要为直播再写一个
     * ======================================================================= */
    PlayerInfoPanel {
        id: infoPanel

        playerItem: livePlayer
        stats: livePlayer.stats
        history: livePlayer.statsHistory
        sampling: livePlayer.statsSampling
        onCloseRequested: infoPanel.opened = false
    }

    /* =======================================================================
     * 提示条（"重连中…"这类；位置和点播那个 toast 一个风格：左下、黑底、圆角 4）
     * ======================================================================= */
    Rectangle {
        id: notice

        anchors {
            left: parent.left
            leftMargin: 10
            bottom: parent.bottom
            bottomMargin: controlBar.height + 44
        }
        width: noticeText.implicitWidth + 24
        height: 36
        radius: 4
        color: Qt.rgba(0, 0, 0, 0.8)
        opacity: noticeText.text === "" ? 0 : 1
        visible: opacity > 0
        z: 300

        Behavior on opacity {
            NumberAnimation { duration: 300 }
        }

        Text {
            id: noticeText

            anchors.centerIn: parent
            color: "#fff"
            font.pixelSize: 13
        }
    }

    Timer {
        id: noticeTimer
        interval: 2500
        onTriggered: noticeText.text = ""
    }

    function notify(text) {
        noticeText.text = text
        noticeTimer.restart()
    }

    /* =======================================================================
     * 对外动作
     * ======================================================================= */
    signal toggleFullscreenRequested()
    signal webFullscreenRequested()

    /* 换片源（LiveMain 调）：拉清单 + 踢一下控制栏 */
    onSourceChanged: {
        if (source === "")
            return

        /* 标题：HLS 用 master 的名字、FLV 用文件名都行，统一"来源主机 + 末段" */
        roomHost = ("" + source).replace(/^[a-z]+:\/\//i, "").split("/")[0]
        roomTitle = (isHls ? qsTr("HLS 直播 · ") : qsTr("FLV 直播 · ")) + ("" + source).split("/").pop()
        reconnectAttempt = 0

        /*
         * 清晰度清单：
         *   * HLS —— 清单在 m3u8 里，**不用在这里拉**（框架上报优先；
         *     框架没上报时，等用户**打开清晰度菜单**的那一刻再自己去拉 master，
         *     见 qualityMenuOpened 的处理与 fetchHlsMaster）。
         *     【为什么不在这里拉】起播这一刻是和播放器抢时间的关键路径，
         *     多一个无关的 HTTP 请求只会多一个变量；菜单是用户点出来的，那时候再拉最安全。
         *   * FLV —— 没有清单，只能问服务端要 streams.json（约定：同目录下的 streams.json）。
         */
        var hlsSource = ("" + source).toLowerCase().indexOf(".m3u8") >= 0

        if (hlsSource) {
            renditions = []
            hlsParsedRenditions = []
            console.warn("[live] HLS 直播：清晰度优先用框架上报（player.qualities）；"
                         + "没有的话，打开清晰度菜单时自动解析 master 清单")
            /* 单档地址（…/1080p/index.m3u8）：自动升级到同目录的 master.m3u8（见那边的说明） */
            upgradeVariantToMaster(source)
        } else {
            fetchRenditions(source)
        }

        kickControls()
    }

    /* 键盘：Space/K 暂停、M 静音、F 全屏、Esc 退全屏、↑↓ 音量（和点播那套一致的手感） */
    Shortcut { sequence: "Space"; onActivated: livePlayer.togglePause() }
    Shortcut { sequence: "K"; onActivated: livePlayer.togglePause() }
    Shortcut { sequence: "M"; onActivated: livePlayer.muted = !livePlayer.muted }
    Shortcut { sequence: "F"; onActivated: liveView.toggleFullscreenRequested() }
    Shortcut { sequence: "Up"; onActivated: livePlayer.volume = Math.min(1, livePlayer.volume + 0.05) }
    Shortcut { sequence: "Down"; onActivated: livePlayer.volume = Math.max(0, livePlayer.volume - 0.05) }

    Component.onCompleted: kickControls()

    /* 面板开着的时候别自动收控制栏（不然鼠标一动面板/控制栏一起闪） */
    onAnyMenuOpenChanged: if (anyMenuOpen) kickControls()
}
