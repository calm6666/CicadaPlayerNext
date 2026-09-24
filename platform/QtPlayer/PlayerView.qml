// ===========================================================================
// 独立播放器组件（把"播放器 + 控制栏 + 顶部栏 + 弹幕层 + 鼠标活动区"整块封起来）
//
// 【为什么单独一个文件】原来这一大坨直接写在 Main.qml 里：播放区、弹幕层、顶部栏、
// 控制栏、自动隐藏的四个定时器、画面区的鼠标事件全混在窗口那一层。要加第二个页面
// （比如列表页 / 详情页 / 再看一个播放器）时，Main.qml 就没法复用了。
//
// 现在边界很清楚：
//   * 本组件：**只管播放这件事** —— 画面、弹幕、控制栏、顶部栏、鼠标在画面区的交互
//     （单击暂停/双击全屏/移动显示控制栏/3 秒自动收起）。
//   * 外面（Main.qml）：窗口本身（无边框、标题栏、主题）、文件选择（对话框/拖拽/Ctrl+O）、
//     设置页、诊断文字。这些都和"播放器"无关，属于应用层。
//
// 外面通过属性和信号和它打交道，**不要**再往这个文件里塞应用逻辑：
//   属性（外面 → 播放器）：source / sourceTitle / fullscreen / overlayOpen / active
//   信号（播放器 → 外面）：settingsRequested / fullscreenRequested / activity
//   只读别名（外面还要用它内部的层）：player / controlBar / danmaku
//
// 一个容易被忽略的点：外面原来用 id 直接引用 `player` / `bar` / `danmakuLayer`。
// 现在这些 id 都在本文件里，外面通过下面的只读别名拿到（Main.qml 里也是这么接的），
// 所以老代码里 `player.source`、`bar.closeMenus()` 这种写法不用改。
// ===========================================================================
import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T
import QtPlayer

Item {
    id: playerView

    // -----------------------------------------------------------------------
    // 对外的接口
    // -----------------------------------------------------------------------

    /* 片源地址（命令行 / 文件对话框 / 拖拽都最终写到这里） */
    property url source: ""

    /* 换片源了：把右键菜单/统计面板这些浮层收掉（里面显示的还是上一条片源的数据） */
    onSourceChanged: playerView.closePanels()

    /* 顶部栏显示的标题（外面用文件名算好传进来，空串表示不显示） */
    property string sourceTitle: ""

    /* 是不是全屏（外面按窗口的 visibility 喂进来：控制栏按钮样子、顶部栏显示、双击行为都看它） */
    property bool fullscreen: false

    /*
     * 外面是否有"盖住播放器的模态层"开着（设置页 / 文件对话框）。
     * 有两个作用：① 这时候不要自动收起控制栏；② 不要藏鼠标指针。
     */
    property bool overlayOpen: false

    /* 播放器是否还应该响应鼠标活动（窗口没激活/正在关闭时可以置 false） */
    property bool active: true

    /*
     * 选集数据（全屏时控制栏右组那颗「选集」按钮用它）：
     *   episodes            —— 集数列表，每项至少 { title: "..." }，可以再带 cid / url
     *   currentEpisodeIndex —— 当前集下标（面板里那行会高亮 + 显示三柱图标）
     * 播放器自己**不认识业务上的"集"**，它只是把这份数据转交给控制栏、再把点击事件转出来。
     */
    property var episodes: []
    property int currentEpisodeIndex: -1

    /*
     * 还有没有上一条 / 下一条（左组那两颗按钮的显隐判据）。
     * 和 episodes 一样是**外面喂进来的**：播放器只负责转发，不知道"列表"是什么。
     */
    property bool hasPrevious: false
    property bool hasNext: false

    /*
     * 进度条上的两份业务数据（**外面喂进来，本组件只转发**，和 episodes 一个路子）：
     *   viewPoints —— 分段（视点），每项 {from, to, content}，from/to 单位**秒**；
     *   pbpData    —— 高能进度条，{step_sec, data:[0~1,…]}，第 i 点 = 第 i*step_sec 秒。
     * 转给控制栏之后，控制栏再转给左组那颗「章节」按钮和进度条那一行（两边用的是同一份）。
     *
     * 默认值 = 没有数据：分段横条不出现、pbp 曲线区 opacity 0，界面和加这两个功能之前完全一致。
     * 数据从哪来（HTTP / 本地文件 / 以后别的来源）**不关播放器的事**，所以放在这里的是
     * 三个纯粹的"入口"，取数据那段在应用层（Main.qml 的 fetchProgressMeta）。
     */
    property var viewPoints: []
    property var pbpData: null
    property bool highlightEnabled: true
    /*
     * 悬停气泡的位置帧（接口 videoshot/preview.bin 切出来的 data URL 数组）：
     * arr[0] 是空占位，arr[k]（k≥1）= 第 5*(k-1) 秒那一帧。空数组 = 接口没给（mock 没带 --video）。
     * 和 viewPoints / pbpData 一个路子：应用层喂进来，这里原样往下转。
     */
    property var previewFrames: []

    /*
     * mock 服务基址：外面（Main.qml）喂进来给设置面板显示，用户在面板里改完再由下面的信号转出去。
     * 播放器自己**不关心 mock 基址在哪**，它只做"转交"（和 episodes / hasPrevious 一个路子）。
     */


    /* 用户点了某一集：外面去换片 / 换页 */
    signal episodeSelected(int index, var episode)

    /*
     * 上一个 / 下一个视频：控制栏左组那两颗按钮（`controls/LeftControls.qml`）转出去，
     * 由应用层去换源 —— 播放器自己**不认识"播放列表"**，它只把请求转出去，
     * 和 episodeSelected 是同一个路子。
     *
     * 【这两个信号一定要声明】上一轮加"上一个/下一个"时只加了 PlayerControlBar 上的
     * 两个信号和 Main.qml 里的两个处理器，**漏了本文件这两个声明**，后果是双份的：
     *   1. Main.qml 里 `onPreviousRequested: ...` 找不到对应信号 →
     *      "Cannot assign to non-existent property onPreviousRequested"；
     *   2. 下面那两行 `playerView.previousRequested()` 在点击时抛
     *      TypeError（property is not a function）→ 表现就是**点了那两颗按钮毫无反应**。
     */
    signal previousRequested()
    signal nextRequested()

    /* 点顶部栏的"?"：外面去开设置页 */
    signal settingsRequested()

    /* 双击画面 / 点全屏按钮：外面去切窗口的全屏状态（只有窗口自己能改 visibility） */
    signal fullscreenRequested()

    /* 画面区有鼠标活动：外面据此重置它自己的自动隐藏（标题栏那份） */
    signal activity()

    /* 外面还要用到的内部层（Main.qml 里直接别名过去） */
    readonly property alias player: player
    readonly property alias controlBar: controlBar
    readonly property alias danmaku: danmakuLayer
    readonly property alias topBarVisible: topBar.shown

    /*
     * 右键菜单 + 三个面板 + 提示条（本轮新增，全部复刻参考实现的 DOM/CSS）：
     *   contextMenu —— .player-contextmenu（复制地址 / 色彩调整 / 快捷键 / 版本 / 统计信息）
     *   infoPanel   —— .player-info-container（统计信息，数据来自 player.stats，真读数）
     *   colorPanel  —— .player-color-panel（色彩调整；核心无此接口，滑块仅 UI 预览）
     *   hotkeyPanel —— .player-hotkey-panel（快捷键说明，列的是真支持的键）
     *   toast       —— .player-toast-wrap（"已复制…"/版本号）
     * 这几个面板的位置都相对**播放区**（参考里也是绝对定位在播放器容器里）。
     */
    readonly property alias contextMenu: contextMenu
    readonly property alias infoPanel: infoPanel
    readonly property alias colorPanel: colorPanel
    readonly property alias hotkeyPanel: hotkeyPanel
    readonly property alias toast: toast

    /* 有没有任何一个面板/菜单开着（外面判"浮层"用：这时不自动收控制栏、不藏鼠标） */
    readonly property bool panelOpen: contextMenu.opened || infoPanel.opened
                                      || colorPanel.opened || hotkeyPanel.opened

    /* 清晰度切换提示：固定在左下角控制栏上方，不覆盖进度条和按钮行。 */
    property bool qualityNoticeVisible: false
    property string qualityNoticeText: ""

    function qualityLabelForStream(streamIndex) {
        if (!player || !player.qualities)
            return ""
        for (var i = 0; i < player.qualities.length; ++i) {
            var row = player.qualities[i]
            if (row.streamIndex === streamIndex)
                return row.label || ""
        }
        return ""
    }

    /* 光标是不是停在画面区里（外面判"还在不在播放器上"用，见 Main.qml 的 pointerInPlayer） */
    readonly property bool pointerInside: activityArea.containsMouse

    /* 鼠标最后停在哪一行（-1 = 不在窗口里），用来决定要不要把指针藏起来 */
    property real lastMouseY: -1

    /* -----------------------------------------------------------------------
     * 画面 + 提示文字
     * ----------------------------------------------------------------------- */

    /*
     * 播放区**填满本组件**。原来它是锚在窗口的 titleBar 下面的 —— 现在那个偏移
     * 由外面摆放本组件时负责（外面 anchors.top: titleBar.bottom），
     * 这样本组件自己不需要知道"外面有没有标题栏"。
     */
    CicadaPlayerItem {
        id: player

        anchors.fill: parent

        source: playerView.source
        autoPlay: true
        volume: 1.0
        loop: false
    }

    Connections {
        target: player

        function onQualitySwitchStatusChanged(status, streamIndex, description) {
            var label = playerView.qualityLabelForStream(streamIndex)
            if (status === 0) {
                playerView.qualityNoticeText = label.length > 0
                        ? qsTr("正在切换到 %1…").arg(label)
                        : qsTr("正在切换清晰度…")
                playerView.qualityNoticeVisible = true
                qualityNoticeTimer.stop()
            } else if (status === 1) {
                playerView.qualityNoticeText = label.length > 0
                        ? qsTr("已切换到 %1").arg(label)
                        : qsTr("清晰度切换完成")
                playerView.qualityNoticeVisible = true
                qualityNoticeTimer.restart()
            } else if (status === 2) {
                playerView.qualityNoticeText = qsTr("清晰度切换失败，继续播放当前清晰度")
                playerView.qualityNoticeVisible = true
                qualityNoticeTimer.restart()
            } else if (status === 3) {
                playerView.qualityNoticeText = qsTr("清晰度切换已取消，继续播放")
                playerView.qualityNoticeVisible = true
                qualityNoticeTimer.restart()
            }
        }
    }

    Timer {
        id: qualityNoticeTimer
        interval: 2600
        repeat: false
        onTriggered: playerView.qualityNoticeVisible = false
    }

    /* 左下角提示卡片。anchors.bottom 指向控制栏顶部，故不会压住控制栏。 */
    Rectangle {
        id: qualityNotice
        visible: playerView.qualityNoticeVisible
        z: 4
        anchors.left: player.left
        anchors.bottom: controlBar.top
        anchors.leftMargin: 16
        anchors.bottomMargin: 10
        width: Math.min(player.width - 32, noticeRow.implicitWidth + 28)
        height: 34
        radius: 4
        color: "#D91B1B1B"

        // 提示内容较长时仍限制在视频区域内，避免窗口窄时把卡片撑出右边界。
        TextMetrics {
            id: noticeMetrics
            text: playerView.qualityNoticeText
            font.pixelSize: 13
        }

        Row {
            id: noticeRow
            anchors.centerIn: parent
            spacing: 10

            /*
             * 【两段文字必须各自垂直居中 —— 用户实测："提示文字不居中，关闭图标却是居中的"】
             *
             * Row 只按 y=0 从左往右摆，**不做垂直对齐**：13px 的提示文字和 20px 的 "×"
             * 高度不一样，各自贴在 Row 顶部 —— 于是大字号的 × 看起来居中、提示文字偏上。
             * 给每个子项的 anchors.verticalCenter 指到 Row 的垂直中线即可（Row 的高度是
             * 子项里最高的那个，所以两条中线都落在卡片里）。
             */
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: playerView.qualityNoticeText
                color: "white"
                font.pixelSize: 13
                verticalAlignment: Text.AlignVCenter
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "×"
                color: "#BDBDBD"
                font.pixelSize: 20
                verticalAlignment: Text.AlignVCenter

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        qualityNoticeTimer.stop()
                        playerView.qualityNoticeVisible = false
                    }
                }
            }
        }
    }

    /* 没设置地址时给个提示，免得看着一片黑以为坏了。 */
    Text {
        anchors.centerIn: player
        /*
         * 【判据必须用 hasMedia，不能只看 source】
         * 清单对象片源的 `source` 是**空的**（setManifestJson 会清掉它），
         * 于是"清单对象 + 正在播放"时这行提示死活不消失（用户实测报的就是这个）。
         * hasMedia = 有地址 **或** 有清单对象，才是"有片源"。
         */
        visible: !player.hasMedia
        color: QtPlayerTheme.hintText
        font.pixelSize: 18
        text: qsTr("按 Ctrl+O 选择本地视频，或把视频文件直接拖进窗口")
    }

    /*
     * -----------------------------------------------------------------------
     * 左上角：**协议徽标**（DASH / HLS / FLV / MP4 / RTMP …）
     *
     * 【为什么要显示】这次排查"切换卡顿/seek 卡顿"时最费时间的判断就是
     * "这一条到底是 DASH 还是 HLS"——两者分片时长、关键帧边界、段号起点都不一样，
     * 同一个现象在两种协议下的原因完全不同。界面直接把协议标出来，看一眼就知道该按
     * 哪条线的规律去分析。
     *
     * 数据来自 player.protocol / player.protocolDetail（CicadaPlayerItem 里按
     * "清单对象 > 后缀 > scheme"判断，见那两个属性的说明）。没有片源时不显示。
     *
     * 位置和样式：压在画面左上角、控制栏/顶部栏之上（z 5），半透明黑底 + 白字，
     * 鼠标悬停显示完整说明。**不接收鼠标事件**（enabled: false 的 MouseArea 只用来
     * 显示提示），所以不会影响画面区的拖拽/双击全屏。
     * -----------------------------------------------------------------------
     */
    Rectangle {
        id: protocolBadge

        z: 5
        /* 统计信息面板也贴在左上角（参考里就是 left:10 top:10），两者重叠时让位给面板 */
        visible: player.protocol !== "" && !infoPanel.opened
        anchors.left: player.left
        anchors.top: player.top
        anchors.leftMargin: 12
        anchors.topMargin: 12
        width: protocolRow.implicitWidth + 18
        height: 24
        radius: 4
        color: "#B31B1B1B"
        border.width: 1
        border.color: "#33FFFFFF"

        Row {
            id: protocolRow
            anchors.centerIn: parent
            spacing: 6

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: player.protocol
                color: "white"
                font.pixelSize: 12
                font.bold: true
                font.family: "Consolas"
            }

            /* DASH/HLS 是多码率自适应流，标一下"自动"，和清晰度菜单里的自动档对得上。 */
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: player.protocol === "DASH" || player.protocol === "HLS"
                text: player.autoQuality ? qsTr("自动") : qsTr("手动")
                color: player.autoQuality ? "#7ED321" : "#BDBDBD"
                font.pixelSize: 10
            }
        }

        /*
         * 只用一个 HoverHandler：它**不接收按键**（不像 MouseArea），所以徽标压在画面上
         * 也不会把拖拽/双击全屏吃掉；只用它的 hovered 驱动上面那条 ToolTip。
         */
        HoverHandler {
            id: protocolHover
        }

        ToolTip.visible: protocolHover.hovered && player.protocolDetail !== ""
        ToolTip.delay: 400
        ToolTip.text: player.protocolDetail
    }

    /* 加载中提示（首帧出来之后就隐藏）。此时播放区已经是黑底了，用压在画面上的那种文字色。 */
    Text {
        anchors.centerIn: player
        visible: player.status === CicadaPlayerItem.Loading
        color: QtPlayerTheme.onVideoText
        font.pixelSize: 16
        text: qsTr("正在加载…")
    }

    /*
     * 缓冲中提示（播放中途读不到数据、视频路断粮）。
     *
     * 判据是框架的 **LoadingStart / LoadingEnd** 回调（player.buffering）——起缓冲时
     * 框架会暂停主时钟和音频渲染，缓冲够了自动恢复，所以这里只是**显示**，不参与控制。
     *
     * 以前这条线没接，界面只知道"打开中/就绪"：视频卡住、弹幕照滚、用户不知道为什么。
     * 速度用 player.downloadSpeedText（>1MB/s 显示 MB/s，否则 KB/s）。
     */
    Rectangle {
        id: bufferingIndicator

        anchors.centerIn: player
        visible: player.buffering
        width: bufferingRow.implicitWidth + 28
        height: bufferingRow.implicitHeight + 20
        radius: 6
        color: Qt.rgba(0, 0, 0, 0.62)

        Row {
            id: bufferingRow

            anchors.centerIn: parent
            spacing: 8

            BusyIndicator {
                anchors.verticalCenter: parent.verticalCenter
                running: bufferingIndicator.visible
                width: 22
                height: 22
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                color: QtPlayerTheme.onVideoText
                font.pixelSize: 15
                text: player.downloadSpeedText.length > 0
                      ? qsTr("缓冲中 %1").arg(player.downloadSpeedText)
                      : qsTr("缓冲中…")
            }
        }
    }

    /* -----------------------------------------------------------------------
     * 弹幕层
     * ----------------------------------------------------------------------- */

    /*
     * 弹幕层：**渲染在 QML、逻辑在 C++**。
     *
     * 原来是 DanmakuLayer.qml（引擎用 JS 写的：轨道分配、碰撞、分段、悬停、缩放每帧在
     * QML 的 JS 里跑一遍）。现在换成 DanmakuView.qml —— 它每帧只做"把播放时间喂给 C++
     * 引擎、把 C++ 算好的位置灌进对象池"，算法全在独立的 CicadaDanmaku 库里
     * （CicadaPlayerNext/danmaku，纯 C++、可单独打包 DLL）。
     *
     * 对外 API 和旧的 DanmakuLayer **完全一样**（area / speed / fontBold / filter /
     * setArea() / sendDanmaku() / generateSample() …），所以控制栏的发送框、设置面板、
     * Ctrl+D 那三处调用方一行都不用改。
     *
     * 位置：压在画面上面、控制栏下面（控制栏 z:1，这层不给 z = 0）。
     * 鼠标位置由画面区那层 MouseArea 喂进去（原因见 DanmakuView.qml 里那段说明：
     * 这个 App 的 hover 是"最上层独占"，弹幕层自己挂 HoverHandler 会跟画面区抢）。
     */
    DanmakuView {
        id: danmakuLayer

        anchors.fill: player
        player: player
        fullscreen: playerView.fullscreen
        /* 开关由控制栏底部中间那颗"弹"控制 */
        danmakuEnabled: controlBar.danmakuEnabled

        pointerInside: activityArea.containsMouse
        pointerPosition: activityArea.mapToItem(danmakuLayer, activityArea.mouseX, activityArea.mouseY)
    }

    /* -----------------------------------------------------------------------
     * 顶部栏（照参考实现的 Top.ts + topbar.scss 复刻）
     * ----------------------------------------------------------------------- */

    /*
     * 参考里的顶部栏：左边标题、右上角一颗"?"，底下一层 240px 的"黑 -> 透明"渐变遮罩。
     * 两个事实按参考来（见 PlayerTopBar.qml 顶部注释）：
     *   * 标题只在**全屏**时显示（普通窗口下参考就是 display:none，index.scss:4630-4635）；
     *   * 参考的遮罩带着 hidden:true（等于不显示），这里按它的本意画出来。
     * 没有搬"关注/头像"那颗 B 站站内胶囊 —— 本播放器没有这个业务对象。
     */
    PlayerTopBar {
        id: topBar

        anchors {
            left: player.left
            right: player.right
            top: player.top
        }
        /* 和标题栏/控制栏一样压在鼠标活动探测层上面 */
        z: 1

        fullscreen: playerView.fullscreen
        shown: controlBar.shown
        title: playerView.sourceTitle

        onHelpRequested: playerView.settingsRequested()
    }

    /* -----------------------------------------------------------------------
     * 「点时间输入框以外的地方」那一层（跳转输入框的关闭手势）
     *
     * 【为什么放在这里、而不是放进控制栏】它必须盖住**整个播放区**（画面、控制栏、
     * 按钮……），而 QML 的点击只在"父对象的矩形范围内"才可能命中子项 ——
     * 一个塞在控制栏里的 MouseArea 哪怕尺寸写得再大，超出控制栏矩形的那部分也收不到点击。
     * 所以这一层挂在 PlayerView（铺满整窗）上，用 z 盖住控制栏（控制栏是 z:1）。
     *
     * 【为什么"不吃事件"】参考里是"document 上挂一个 click 监听"：点哪儿都关掉输入框，
     * 但被点到的那个元素自己该干嘛还干嘛（点播放键既关输入框、也真的暂停）。
     * 这里照做：在 onPressed 里先 commit（关 + seek），然后 `mouse.accepted = false`
     * 把这一下**放过去**给下面的项 —— 按钮、画面活动层都不会因为这一层而失灵。
     *
     * 【输入框那一格要放行】按在输入框矩形里的那一下**什么都不做**（不 commit，
     * 也不拦），交给下面的输入框自己处理（定位光标、选词）。矩形由控制栏转达
     * （LeftControls.seekInputRectIn）。
     *
     * z 说明：控制栏是 PlayerView 的直接子项、z:1，这一层 z:10 就在它上面；
     * 再往上的东西（Main.qml 的顶部栏 z:3/4、设置页 z:20）在**另一个父对象**里，
     * 按 z 的作用域规则本来就压在这一层上面，点它们不会走到这里（可以接受）。
     * ----------------------------------------------------------------------- */
    MouseArea {
        id: timeEditCatcher

        anchors.fill: parent
        visible: controlBar.timeEditing
        z: 10

        /*
         * 不接 hover：控制栏的自动收起、指针形状全靠 hover 判据，这一层不许插一脚
         * （它只是"按一下"的观察者）。
         */
        hoverEnabled: false

        onPressed: function (mouse) {
            var r = controlBar.timeInputRectIn(timeEditCatcher)

            if (mouse.x >= r.x && mouse.x <= r.x + r.width
                    && mouse.y >= r.y && mouse.y <= r.y + r.height) {
                mouse.accepted = false          /* 在输入框里：放行，什么都不做 */
                return
            }

            controlBar.commitTimeEdit()         /* 别的地方：先关掉（规范就顺带 seek） */
            mouse.accepted = false              /* 再把这一下放过去，原本该响应的照旧响应 */
        }
    }

    /* -----------------------------------------------------------------------
     * 控制栏（照参考实现复刻，见 PlayerControlBar.qml）
     * ----------------------------------------------------------------------- */

    /*
     * 这一层压在画面上，自下而上是：
     *   * 100px 的"透明 -> 黑"遮罩（assets/images/control-mask.png，参考那张 PNG 原图）；
     *   * 进度条那一行（底边距 44px）；
     *   * 35px 高的按钮行：播放/时间 + 清晰度/倍速/音量/设置/全屏。
     * 所有尺寸、颜色都在 QtPlayerTheme 里（出处是参考实现的 SCSS），这里不写死数值。
     *
     * 打开文件**不**在这一层（参考的控制栏里没有这个按钮）：走 Ctrl+O，或者把文件拖进窗口。
     */
    PlayerControlBar {
        id: controlBar

        anchors {
            left: player.left
            right: player.right
            bottom: player.bottom
        }

        /* 压在"鼠标活动探测"上面 */
        z: 1

        player: player
        /* 弹幕层：底部中间的弹幕开关 + 设置面板作用在它身上（见 PlayerControlBar 的 dmBar） */
        danmaku: danmakuLayer
        /* 全屏按钮的样子跟着窗口状态走（对应参考里给按钮加 state-active 的那套逻辑） */
        fullscreen: playerView.fullscreen
        /* 有浮层盖着时控制栏收起也不藏鼠标指针（转给控制栏那层判 cursorShape 用） */
        overlayOpen: playerView.overlayOpen

        /* 选集：数据转交 + 点击事件转出去（播放器不懂业务上的"集"） */
        episodes: playerView.episodes
        currentEpisodeIndex: playerView.currentEpisodeIndex
        /* 上一条/下一条存不存在：左组那两颗按钮的显隐判据，一起转给控制栏 */
        hasPrevious: playerView.hasPrevious
        hasNext: playerView.hasNext

        /* 分段（视点）+ 高能进度条数据：控制栏再往下转给左组「章节」按钮和进度条那一行 */
        viewPoints: playerView.viewPoints
        pbpData: playerView.pbpData
        highlightEnabled: playerView.highlightEnabled
        previewFrames: playerView.previewFrames

        onSettingsRequested: playerView.settingsRequested()
        onFullscreenRequested: playerView.fullscreenRequested()
        /* 上一个 / 下一个视频：控制栏那两颗按钮 → 这里转出去，由应用层换源 */
        onPreviousRequested: playerView.previousRequested()
        onNextRequested: playerView.nextRequested()
        onEpisodeSelected: (index, episode) => playerView.episodeSelected(index, episode)

        /* 鼠标停在控制栏上也算"有活动"：别在用户正要按按钮的时候把它收走 */
        Connections {
            target: controlBar

            function onHoveredChanged() {
                if (controlBar.hovered)
                    playerView.kickAutoHide()
            }
        }
    }

    /* -----------------------------------------------------------------------
     * 自动隐藏 + 画面区鼠标事件
     * ----------------------------------------------------------------------- */

    /* 显示控制栏并把 3 秒倒计时重新开始（外面也可以调） */
    function kickAutoHide() {
        controlBar.shown = true
        hideTimer.restart()
        playerView.activity()
    }

    /*
     * 自动隐藏的倒计时：鼠标停手 3 秒就把控制条藏起来（只在播放中，见下面）。
     *
     * 这几种情况**不藏**（不然用户会觉得控制条"乱闪"）：
     *   * 光标还在控制层上（controlBar.pointerInside）；
     *   * 外面有模态层开着（设置页 / 文件对话框，见 overlayOpen）。
     */
    Timer {
        id: hideTimer
        interval: 3000
        repeat: false
        onTriggered: {
            /*
             * 只有**正在播放**时才自动收起（参考 PlayerDocker.ts:690-700 判的就是这个）：
             * 暂停着、或者根本没片源时，控制栏一直留着，用户不用先去晃鼠标。
             */
            if (!controlBar.pointerInside && !playerView.overlayOpen && player.playing)
                controlBar.shown = false
        }
    }

    /*
     * 画面区的鼠标事件（照参考 PlayerDocker.ts:819-862 搬过来）：
     *   * mousemove  → 显示控制栏 + 重置 3 秒自动隐藏定时器（showControls）
     *   * mouseleave → 隐藏控制栏（hideControls）
     *   * click      → **延迟 400ms** 再播放/暂停（第二次点击取消定时器 → 双击不会顺手暂停）
     *   * dblclick   → 切换全屏
     *
     * 这一层铺满整个播放区、压在控制层下面（控制层 z:1、顶部栏 z:1），所以：
     *   * 点按钮/拖进度条时，事件被控制层自己的 MouseArea 接走，不受这里影响；
     *   * 点画面空白处才会走到这里的单击/双击。
     */
    MouseArea {
        id: activityArea
        anchors.fill: parent
        hoverEnabled: true
        /* 左键：用来做单击播放/暂停、双击全屏（参考里 click/dblclick 就绑在这块上） */
        acceptedButtons: Qt.LeftButton
        enabled: playerView.active

        /*
         * 控制条藏起来的时候把鼠标指针也一起藏掉（主流播放器就是这样：不动鼠标 = 专心看片，
         * 指针别挡着画面）。鼠标一动 → kickAutoHide() 让控制条回来 → 指针跟着回来。
         * 对应参考里的 `state-no-cursor` 类（hideControls 时加上）。
         *
         * 【必须用 controlBar.shown，不能用 visible】控制层把显隐动画拆到了三块子元素上，
         * 根对象本身**一直 visible**（否则没地方挂动画），所以 `!visible` 恒为 false ——
         * 指针永远不藏（这就是"鼠标指针隐藏没实现"的原因）。
         *
         * 两种情况不能藏：外面有模态层开着；鼠标不在窗口里（lastMouseY 是 -1）。
         */
        cursorShape: (!controlBar.shown && !playerView.overlayOpen && playerView.lastMouseY >= 0)
                     ? Qt.BlankCursor : Qt.ArrowCursor

        onPositionChanged: (mouse) => {
            playerView.lastMouseY = mouse.y
            playerView.kickAutoHide()
        }

        /*
         * 光标回到画面区 = 离开了控制层 → 把控制层上打开的浮层菜单（倍速/清晰度/音量…）都关掉。
         * 这就是"鼠标移出控制栏，菜单就收"的那一半；另一半由浮层自己的 closePolicy 负责
         * （点浮层外面、或按 Esc）。
         */
        onEntered: controlBar.closeMenus()

        /*
         * 单击：延迟 400ms 再播放/暂停（参考 PlayerDocker.ts:847-862 的 clickTimer）。
         * 双击时第二次点击会取消这个定时器，所以双击=只切全屏，不会顺带暂停一下。
         */
        onClicked: clickTimer.restart()

        /* 双击：切全屏（并且取消单击的定时器，避免"双击=切全屏+暂停"） */
        onDoubleClicked: {
            clickTimer.stop()
            playerView.fullscreenRequested()
        }

        /*
         * 光标离开画面区（或者离开窗口）→ 延迟一下确认，确实不在播放器里了就收起。
         *
         * 参考是立即隐藏（它的控制层是画面区的子元素，移到控制层上不会触发 mouseleave）；
         * 我这边控制层是兄弟层，移到控制层上同样会收到这个事件，所以延迟 120ms 让 hover
         * 状态落定后再按"到底在不在播放器容器里"判一次 —— 不延迟会误判成离开，
         * 于是"收起 → 光标下的区域消失 → 又触发一轮事件 → 再显示"，变成循环闪烁。
         */
        onExited: {
            playerView.lastMouseY = -1
            leaveHideTimer.restart()
        }
    }

    /* 单击延迟定时器（参考 PlayerDocker.ts:849-859 的 clickTimer，400ms 区分双击） */
    Timer {
        id: clickTimer

        interval: 400
        repeat: false
        onTriggered: player.togglePause()
    }

    /* 光标离开控制层（含从底部移出窗口）→ 同样延迟确认再收起 */
    Connections {
        target: controlBar

        function onPointerInsideChanged() {
            if (!controlBar.pointerInside)
                leaveHideTimer.restart()
        }
    }

    /*
     * 延迟确认"光标是不是真的离开了播放器"。
     *
     * 判据：既不在画面区（activityArea.containsMouse），也不在控制层（controlBar.pointerInside）。
     * 这样**不管从哪个方向移出**（包括从底部一路移出窗口）都会收起 —— 之前用坐标猜，
     * 从底部移出时会被误判成"压到控制栏上了"，所以怎么都不收。
     */
    Timer {
        id: leaveHideTimer

        interval: 120
        repeat: false
        onTriggered: {
            /* 外面有模态层（设置页）开着时不要收：那不是"离开播放器" */
            if (!activityArea.containsMouse && !controlBar.pointerInside
                    && !playerView.overlayOpen) {
                /* 控制层要收起了，上面打开的浮层菜单一起关掉（不然会孤零零留在画面上） */
                controlBar.closeMenus()
                controlBar.shown = false
            }
        }
    }

    /* -----------------------------------------------------------------------
     * 右键菜单 + 三个面板 + 提示条（复刻参考实现的 context/videoinfo/colorpanel/hotkeypanel/toast）
     * ----------------------------------------------------------------------- */

    /*
     * 右键：在**画面区**上接右键。
     *
     * 【为什么用 TapHandler 而不是铺满画面的 MouseArea —— 别改回去】
     * 上一版这里放了一个 `anchors.fill: player; acceptedButtons: Qt.RightButton` 的
     * MouseArea，结果**整块画面区的鼠标指针都变成了箭头**：Qt 决定光标形状时是从上往下
     * 找"最上面那个设置了光标的元素"，MouseArea 即使只接右键、即使 hoverEnabled=false，
     * 它的 cursorShape 默认也是箭头 —— 它盖在控制栏那些按钮（小手）上面就把它们全压住了。
     * TapHandler 是**手势处理器**，不设置光标、也不独占 hover，只认右键那一下 ✓。
     */
    TapHandler {
        id: contextTap

        acceptedButtons: Qt.RightButton
        onTapped: (eventPoint) => {
            /* 菜单里的坐标是相对播放区左上角的（参考也是这个参照系） */
            contextMenu.popup(eventPoint.position.x, eventPoint.position.y)
        }
    }

    PlayerContextMenu {
        id: contextMenu

        /*
         * 「复制视频地址（精准空降）」：链接和"复制"都在 C++ 里做
         * （QML 没有剪贴板 API；位置用播放器当前时间）。
         */
        onCopyLinkRequested: {
            const link = player.landingLink()

            if (link === "") {
                toast.show(qsTr("没有可复制的地址"))
            } else if (player.copyTextToClipboard(link)) {
                toast.show(qsTr("已复制：") + link)
            } else {
                toast.show(qsTr("复制失败（拿不到剪贴板）"))
            }
        }

        /* 「视频色彩调整」：和快捷键面板/统计面板互斥（参考里也是同时只开一个） */
        onColorRequested: playerView.openExclusivePanel(colorPanel)
        onHotkeyRequested: playerView.openExclusivePanel(hotkeyPanel)
        onInfoRequested: playerView.openExclusivePanel(infoPanel)

        /* 「播放器版本 1.0.0」：参考里点了没反应；这里给一条提示（文案一字不差） */
        onVersionRequested: toast.show(qsTr("播放器版本 1.0.0"))
    }

    /*
     * 统计信息：数据直接绑 player.stats（框架真实读数，2Hz 刷新）。
     *
     * 曲线（面板下面那三条）用 player.statsHistory —— **只有面板开着才采样**：
     * 打开 → startStatsSampling()（开始 500ms 一次采样），关闭 → stopStatsSampling()
     * （停掉定时器并清空历史）。
     *
     * 【采样开关搬走了】开关现在写在 PlayerInfoPanel.qml 内部（onOpenedChanged），
     * 这里只负责把播放器组件传进去。原因：原来在这里给实例写
     * `onOpenedChanged: player.startStatsSampling()` 实测没生效（曲线和数值一直是空），
     * 跨文件实例的信号处理器里 `player` / `opened` 的作用域解析不可靠；
     * 放进声明 opened 的那个组件里，只依赖传进去的 playerItem，路径最短、也最好查。
     * 打开面板后日志应出现 `[info] stats panel opened=true playerItem=ok`。
     */
    PlayerInfoPanel {
        id: infoPanel
        playerItem: player
        stats: player.stats
        history: player.statsHistory
        sampling: player.statsSampling
        onCloseRequested: close()
    }

    PlayerColorPanel {
        id: colorPanel
        /* 拖滑块时调 player.setColorAdjust(...)（真正应用在渲染后端，见那个面板的说明） */
        player: player
        onCloseRequested: close()
    }

    PlayerHotkeyPanel {
        id: hotkeyPanel
        onCloseRequested: close()
    }

    PlayerToast {
        id: toast
    }

    /*
     * 同一时刻只开一个面板（参考实现里那三个面板也是互斥的：右键一次只开一个）。
     * 已经开着的那一个再点一次就关掉（相当于开关）。
     */
    function openExclusivePanel(panel) {
        const wasOpen = panel.opened

        infoPanel.close()
        colorPanel.close()
        hotkeyPanel.close()

        if (!wasOpen)
            panel.open()

        kickAutoHide()
    }

    /* 关掉所有面板（Esc / 切全屏 / 画面被点的时候用） */
    function closePanels() {
        infoPanel.close()
        colorPanel.close()
        hotkeyPanel.close()
        contextMenu.close()
    }

    readonly property bool anyPanelOpened: infoPanel.opened || colorPanel.opened
                                           || hotkeyPanel.opened || contextMenu.opened
}
