// ===========================================================================
// 首页窗口（程序启动默认打开这一个）
//
// 职责：
//   * 拖文件夹/视频进来（可多个）或点"打开文件夹"按钮 → 交给 VideoLibrary 递归扫描
//   * 网格卡片显示：缩略图 + 文件名 + 大小；点卡片 → 打开播放器窗口播放（复用同一个窗口）
//   * 设置入口：打开原来那个设置面板（点齿轮打开的那个，用户要求不要删）
//
// 数据全来自 VideoLibrary（src/VideoLibrary.{h,cpp}：递归扫描 + 首帧缩略图缓存 + 播放列表）。
// ===========================================================================
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QWindowKit
import QtPlayer
/* AppDialogs：文件/目录选择框**从 C++ 调**（详见 src/AppFileDialogs.h）。
 * 首页两处会用到：清单 JSON 文件、"打开文件夹"扫描视频库。
 * 平台不支持时 available() 为 false，会自动退回下面的 QML FolderDialog。 */
import CicadaPlayer

/* 【首页：QWindowKit + 有标题栏】用 WindowAgent 接管窗口（和播放器窗口同一条路），
   但**带一条自绘标题栏**（可拖拽 + 右上角三键）—— 播放器窗口才是"无标题栏、顶部透明"。
   底色绑 QtPlayerTheme.windowBg → 设置页切深浅色时跟着变。 */
Window {
    id: home

    width: 1100
    height: 720
    visible: false                     /* 等 WindowAgent.setup() 装好再显示，避免闪一下系统标题栏 */
    title: qsTr("视频库")
    color: QtPlayerTheme.windowBg

    /* 标题栏按钮：照抄 Main.qml:144-329 的 CaptionButton（太阳/月亮、关闭/最大化/最小化/还原） */
    component CaptionButton: Rectangle {
        id: captionButton

        property string kind: "minimize"
        property bool maximized: false
        property bool dark: QtPlayerTheme.dark

        signal clicked()

        width: 46
        color: pointer.pressed ? QtPlayerTheme.captionPressed
                               : (pointer.containsMouse ? QtPlayerTheme.captionHover : "transparent")

        MouseArea {
            id: pointer
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.ArrowCursor
            onClicked: captionButton.clicked()
        }

        Item {
            anchors.centerIn: parent
            width: 14
            height: 14

            Rectangle {
                visible: captionButton.kind === "minimize"
                anchors.centerIn: parent
                width: 11
                height: 1
                color: QtPlayerTheme.captionGlyph
            }

            Rectangle {
                visible: captionButton.kind === "maximize" && !captionButton.maximized
                anchors.centerIn: parent
                width: 11
                height: 11
                color: "transparent"
                border.width: 1
                border.color: QtPlayerTheme.captionGlyph
            }

            Rectangle {
                visible: captionButton.kind === "maximize" && captionButton.maximized
                x: 4
                y: 0
                width: 10
                height: 1
                color: QtPlayerTheme.captionGlyph
            }

            Rectangle {
                visible: captionButton.kind === "maximize" && captionButton.maximized
                x: 13
                y: 0
                width: 1
                height: 10
                color: QtPlayerTheme.captionGlyph
            }

            Rectangle {
                visible: captionButton.kind === "maximize" && captionButton.maximized
                x: 0
                y: 4
                width: 10
                height: 10
                color: "transparent"
                border.width: 1
                border.color: QtPlayerTheme.captionGlyph
            }

            Rectangle {
                visible: captionButton.kind === "close"
                anchors.centerIn: parent
                width: 12
                height: 1
                color: QtPlayerTheme.captionGlyph
                rotation: 45
            }

            Rectangle {
                visible: captionButton.kind === "close"
                anchors.centerIn: parent
                width: 12
                height: 1
                color: QtPlayerTheme.captionGlyph
                rotation: -45
            }

            Rectangle {
                visible: captionButton.kind === "theme" && !captionButton.dark
                anchors.centerIn: parent
                width: 8
                height: 8
                radius: 4
                color: "transparent"
                border.width: 1
                border.color: QtPlayerTheme.captionGlyph
            }

            Rectangle {
                visible: captionButton.kind === "theme" && !captionButton.dark
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                width: 1
                height: 3
                color: QtPlayerTheme.captionGlyph
            }

            Rectangle {
                visible: captionButton.kind === "theme" && !captionButton.dark
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                width: 1
                height: 3
                color: QtPlayerTheme.captionGlyph
            }

            Rectangle {
                visible: captionButton.kind === "theme" && !captionButton.dark
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                width: 3
                height: 1
                color: QtPlayerTheme.captionGlyph
            }

            Rectangle {
                visible: captionButton.kind === "theme" && !captionButton.dark
                anchors.verticalCenter: parent.verticalCenter
                anchors.right: parent.right
                width: 3
                height: 1
                color: QtPlayerTheme.captionGlyph
            }

            Rectangle {
                visible: captionButton.kind === "theme" && captionButton.dark
                anchors.centerIn: parent
                width: 11
                height: 11
                radius: 5.5
                color: QtPlayerTheme.captionGlyph
            }
        }
    }

    WindowAgent {
        id: windowAgent
    }

    /* 和 Main.qml 同一套：把窗口交给 QWindowKit，再把标题栏/系统按钮登记进去。
       顺序不能反 —— setTitleBar 会清掉之前登记的按钮（见 abstractwindowcontext.cpp）。 */
    Component.onCompleted: {
        windowAgent.setup(home)
        windowAgent.setTitleBar(titleBar)

        if (!home.nativeCaptionButtons) {
            windowAgent.setSystemButton(WindowAgent.Minimize, minimizeButton)
            windowAgent.setSystemButton(WindowAgent.Maximize, maximizeButton)
            windowAgent.setSystemButton(WindowAgent.Close, closeButton)
        }

        /* 标题栏里除系统按钮外的可交互元素都要登记，否则整条标题栏是拖动区，
           点主题按钮只会拖窗口（照抄 Main.qml:769） */
        windowAgent.setHitTestVisible(themeButton, true)

        /* 把深浅色同步给窗口系统（标题栏/边框的暗色由它管），之后由下面的 Connections 跟着变 */
        home.applyWindowTheme()

        home.visible = true
    }

    readonly property bool nativeCaptionButtons: Qt.platform.os === "osx"

    function applyWindowTheme() {
        windowAgent.setWindowAttribute("dark-mode", QtPlayerTheme.dark)
    }

    /* 主题切换（设置页里切深浅色）时通知窗口系统那一层 —— 和 Main.qml 里那段一样 */
    Connections {
        target: QtPlayerTheme

        function onDarkChanged() {
            home.applyWindowTheme()
        }
    }

    /* ---------------- 标题栏（拖拽 + 右上角三键） ---------------- */
    Rectangle {
        id: titleBar

        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
        }
        height: 36
        color: QtPlayerTheme.windowBg

        /* 标题（照抄 Main.qml:909-924，只是文案换成"视频库"） */
        Text {
            anchors.left: parent.left
            anchors.leftMargin: home.nativeCaptionButtons ? 78 : 12
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: themeButton.left
            anchors.rightMargin: 8
            elide: Text.ElideRight
            color: QtPlayerTheme.titleText
            font.pixelSize: 13
            text: home.title
        }

        /* 主题按钮（太阳/月亮）：照抄 Main.qml:937-945 */
        CaptionButton {
            id: themeButton
            anchors.right: minimizeButton.visible ? minimizeButton.left : parent.right
            height: titleBar.height
            kind: "theme"
            dark: QtPlayerTheme.dark
            onClicked: QtPlayerTheme.mode = QtPlayerTheme.dark ? "light" : "dark"
        }

        /* 三颗窗口按钮：照抄 Main.qml:960-993（关闭在最右，往左排） */
        CaptionButton {
            id: closeButton
            anchors.right: parent.right
            height: titleBar.height
            kind: "close"
            visible: !home.nativeCaptionButtons
            onClicked: home.close()
        }

        CaptionButton {
            id: maximizeButton
            anchors.right: closeButton.left
            height: titleBar.height
            kind: "maximize"
            maximized: home.visibility === Window.Maximized
            visible: !home.nativeCaptionButtons
            onClicked: {
                if (maximized)
                    home.showNormal()
                else
                    home.showMaximized()
            }
        }

        CaptionButton {
            id: minimizeButton
            anchors.right: maximizeButton.left
            height: titleBar.height
            kind: "minimize"
            visible: !home.nativeCaptionButtons
            onClicked: home.showMinimized()
        }
    }

    /* 工具条/网格从标题栏下面开始 */

    /* 列表模型（C++） */
    VideoLibrary {
        id: library
    }

    /* 拖进来的东西交给 C++ 分类（文件直接入列，文件夹递归扫描，深度上限 3） */
    DropArea {
        anchors.fill: parent
        onDropped: (drop) => {
            if (!drop.hasUrls)
                return

            var paths = []

            for (var i = 0; i < drop.urls.length; ++i)
                paths.push(drop.urls[i].toString().replace(/^file:\/{2,3}/, ""))

            var added = library.addPaths(paths, 3)
            console.warn("[library] 拖入 " + paths.length + " 项，新增 " + added + " 个视频")
        }
    }

    /* ---------------- 顶部工具条 ---------------- */
    Rectangle {
        id: toolBar

        anchors {
            left: parent.left
            right: parent.right
            top: titleBar.bottom
        }
        height: 56
        color: "#1b1e24"

        Row {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 16
            spacing: 12

            Button {
                text: qsTr("打开文件夹")
                onClicked: home.chooseFolder()
            }

            Button {
                text: qsTr("清空列表")
                enabled: library.count > 0
                onClicked: library.clear()
            }

            Button {
                text: qsTr("设置")
                /*
                 * 打开的是**共用的**那一份设置页（SettingsPage.qml，和播放器窗口同一份）。
                 * 以前这里写的是 `home.settingsRequested()` —— 只发信号、没人接，点了没反应。
                 */
                onClicked: home.openSettings()
            }

            /*
             * 【网络片源】DASH（.mpd）和 HLS（.m3u8）各一颗按钮：点一下弹出各自的输入框，
             * 填地址 → 播放。清晰度不用在这里填 —— 播放器控制栏那份清晰度菜单读的就是
             * 框架从 mpd/m3u8 里解析出来的**真实多码率列表**（qualities），
             * 点某一档走 player.selectQuality()（见 CicadaPlayerItem::selectQuality）。
             */
            Button {
                text: qsTr("DASH 地址")
                onClicked: dashDialog.start()
            }

            Button {
                text: qsTr("HLS 地址")
                onClicked: hlsDialog.start()
            }

            /*
             * 【清单对象】直接把**清单对象**（hili-player 那套 JSON：duration /
             * video[] / audio[] / segmentInfo）喂给播放器，不经过 m3u8/mpd 文本、
             * 也不需要清单的网络请求。适合：
             *   * 后端已经把清单解析成对象发过来了，不想再落一份 m3u8/mpd 文件；
             *   * 调试某一份清单时手里只有对象。
             *
             * 【为什么分成 DASH / HLS 两个入口（用户要求）】这两种对象**不是一回事**：
             *   * DASH 对象：分片时长在 segmentInfo.segmentTimeline 里（可变时长，
             *     最后一个分片通常是"剩余时间"，比前面短），分片号从 startNumber 数起；
             *   * HLS 对象：分片号从 segmentInfo.mediaSequence 数起（缺了它整条 playlist
             *     的段号会错位），时长通常是 targetDuration 的整数倍。
             * 混用一个输入框的结果就是"粘贴进去打不开，也不知道缺什么"。现在两个入口
             * 各自带自己的示例、自己的校验，并且会把对象**按协议补全**再交给核心
             * （CicadaPlayerItem::setManifestForProtocol），缺字段会直接告诉你缺哪个。
             */
            Button {
                text: qsTr("DASH 对象")
                onClicked: dashManifestDialog.start()
            }

            Button {
                text: qsTr("HLS 对象")
                onClicked: hlsManifestDialog.start()
            }

            /*
             * ===================================================================
             * 【FLV 两个入口】（用户要求：首页要有"直播 FLV 输入"和"FLV 视频播放"）
             *
             * 这两个入口的**去处完全不同**，所以是两颗按钮、两个窗口：
             *
             *   「直播 FLV 地址」→ 打开**直播窗口**（LiveMain.qml，另写的一套 UI）
             *      * 直播播放器的界面和点播不是一回事：没有进度条/没有时长/不能 seek，
             *        要显示"直播中 · 人气"、清晰度是**换一条流重连**（HTTP-FLV 没有清单）、
             *        弹幕是实时流。用点播那套控制栏硬套只会得到一个不像直播的播放器，
             *        所以这里按用户要求**另写一个直播 UI**（复刻 B 站直播播放器的主界面）。
             *      * 地址形如：http://127.0.0.1:9200/live/1080p.flv
             *        （本地测试服务：front/player/mock-live/server.js）
             *
             *   「FLV 视频」→ 打开**现有的点播播放器**（Main.qml）
             *      * FLV 点播和 mp4 完全一样：有时长、能拖进度、能暂停。
             *      * 既支持本地 .flv 文件（走 C++ 原生文件框），也支持 http(s) 的 .flv 地址
             *        —— 后者直接用「直播 FLV 地址」那颗按钮旁边那个输入框也行（同一份弹窗）。
             * ===================================================================
             */
            Button {
                text: qsTr("直播地址")
                onClicked: flvLiveDialog.start()
            }

            Button {
                text: qsTr("FLV 视频")
                onClicked: home.openFlvFile()
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: library.scanning ? qsTr("扫描中…")
                                       : qsTr("共 %1 个视频").arg(library.count)
                color: "#9aa4b2"
                font.pixelSize: 13
            }
        }
    }

    /*
     * "打开文件夹"：**优先走 C++ 的原生目录选择框**（AppDialogs.openDirectory）。
     *
     * 为什么不再直接用 QML 的 FolderDialog：它和 FileDialog 是同一个毛病 ——
     * 组件对象被引擎持有，底下那个原生选择框的句柄（以及文件系统监视器）不随
     * close() 释放，连开几次就会"没反应 / 目录被占用"。C++ 那条路每次现建现销，
     * 句柄当场释放（见 src/AppFileDialogs.h 的文件头）。
     * 平台不支持原生对话框时才退回下面这个 FolderDialog。
     */
    FolderDialog {
        id: folderDialog
        title: qsTr("选择视频文件夹")
        onAccepted: {
            var path = selectedFolder.toString().replace(/^file:\/{2,3}/, "")
            var added = library.addFolder(path, 3)
            console.warn("[library] 扫描 " + path + "，新增 " + added + " 个视频")
        }
    }

    /*
     * 「FLV 视频」的**退回**路径：平台没有原生文件对话框时才走到这里
     * （优先那条路见 home.openFlvFile()：C++ 每次现建现销，句柄随对象释放）。
     * 过滤器把 FLV 放在第一位 —— 这颗按钮的语义就是"播 FLV 文件"。
     */
    FileDialog {
        id: videoFileDialog
        title: qsTr("选择 FLV 视频文件")
        nameFilters: [qsTr("FLV 视频 (*.flv)"),
                      qsTr("视频文件 (*.mp4 *.mkv *.mov *.flv *.ts *.m2ts *.avi *.webm)"),
                      qsTr("所有文件 (*)")]
        onAccepted: home.playLocalFile(selectedFile.toString().replace(/^file:\/{2,3}/, ""))
    }

    /* =======================================================================
     * 网络地址输入弹窗（首页那两颗 DASH / HLS 按钮）
     *
     * 一个 component 两处用。**每颗按钮各有自己的输入对象**：
     *   dashDialog.url / hlsDialog.url（就是各自那个 TextField 里的文字）。
     * 点"播放"（或输入框里按回车）→ home.openPlayerUrl(url)：
     *   * 地址原样交给播放器窗口 → playerView.source → CicadaPlayerItem::setSource；
     *   * 框架按地址（.mpd / .m3u8）自己判断走 DASH 还是 HLS；
     *   * 多码率由框架 MediaInfoGet 报上来，界面那份清晰度菜单显示的就是它。
     * ======================================================================= */
    component UrlDialog: Dialog {
        id: urlDialog

        /* 输入框里的地址（调用方读它） */
        property alias url: urlField.text
        property string heading: ""
        property string placeholder: ""
        /*
         * 是不是"直播地址"。
         * 【为什么要这个开关】同一份弹窗三种用途（DASH / HLS / FLV 直播），
         * 直播那条路的**去处不一样**（打开直播窗口，不是点播窗口），而且下面的提示文字
         * 也要换（直播没有清单、清晰度靠换流重连）。用属性传进来，别在弹窗里猜协议。
         */
        property bool live: false
        /* 提示文字：不传就按点播那套 */
        property string hint: qsTr("支持直接粘贴 mpd / m3u8 地址；清晰度会从清单里读出来，在播放器控制栏选。")

        /*
         * 【不要设 title】Basic 样式里 title 会带出它自己那个 header（用的是 Application
         * 调色板），在这个深色应用里就是"白底黑字的标题条 + 深色面板"，两种风格打架。
         * 标题自己画在 contentItem 里，配色和设置页那套面板一致。
         */
        modal: true
        width: 560

        /* 弹出来就聚焦输入框：直接粘贴地址就能回车播放 */
        function start() {
            urlField.text = ""
            open()
            /* 弹窗的内容元素是 open() 里才建出来的，推到下一轮再要焦点更稳 */
            Qt.callLater(function () { urlField.forceActiveFocus() })
        }

        background: Rectangle {
            color: QtPlayerTheme.panelBg
            border.width: 1
            border.color: QtPlayerTheme.panelBorder
            radius: 4
        }

        contentItem: Column {
            spacing: 10
            topPadding: 18
            bottomPadding: 6
            leftPadding: 18
            rightPadding: 18

            Text {
                text: urlDialog.heading
                color: QtPlayerTheme.panelText
                font.pixelSize: 14
                font.bold: true
            }

            TextField {
                id: urlField

                width: parent.width - parent.leftPadding - parent.rightPadding
                height: 30
                placeholderText: urlDialog.placeholder
                placeholderTextColor: QtPlayerTheme.panelHintText
                color: QtPlayerTheme.controlText
                font.pixelSize: 13
                leftPadding: 8
                rightPadding: 8
                verticalAlignment: TextInput.AlignVCenter
                selectByMouse: true
                background: Rectangle {
                    color: QtPlayerTheme.settingsPanelInputBg
                    border.width: 1
                    border.color: QtPlayerTheme.panelBorder
                    radius: 2
                }
                /* 回车 = 播放（和点"播放"那颗按钮同一条路） */
                onAccepted: urlDialog.accept()
            }

            Text {
                text: urlDialog.hint
                color: QtPlayerTheme.panelHintText
                font.pixelSize: 11
                width: parent.width - parent.leftPadding - parent.rightPadding
                wrapMode: Text.WordWrap
            }
        }

        footer: Item {
            implicitHeight: 52

            Row {
                anchors.right: parent.right
                anchors.rightMargin: 18
                anchors.verticalCenter: parent.verticalCenter
                spacing: 12

                Button {
                    text: qsTr("取消")
                    onClicked: urlDialog.reject()
                }

                Button {
                    text: qsTr("播放")
                    enabled: urlDialog.url.trim() !== ""
                    onClicked: urlDialog.accept()
                }
            }
        }
    }

    UrlDialog {
        id: dashDialog

        heading: qsTr("播放 DASH 地址（.mpd）")
        placeholder: qsTr("https://…/manifest.mpd")
        onAccepted: home.openPlayerUrl(url)
    }

    UrlDialog {
        id: hlsDialog

        heading: qsTr("播放 HLS 地址（.m3u8）")
        placeholder: qsTr("https://…/index.m3u8")
        onAccepted: home.openPlayerUrl(url)
    }

    /*
     * 直播地址（首页「直播 FLV 地址」）。
     *
     * **FLV 与 HLS 两种直播都走这一颗按钮**（用户要求加 HLS 直播支持）：
     *   * HTTP-FLV：http://127.0.0.1:9200/live/1080p.flv —— 一档一个地址，切档靠换流重连；
     *   * HTTP-HLS：http://127.0.0.1:9200/live/hls/master.m3u8 —— 一个地址带全部档位，
     *     切档走清单内换流（不用重连）。逐档地址是 …/live/hls/720p/index.m3u8。
     * 播放器侧按后缀（.m3u8 / .flv）自动走对应的那套，界面上不用再选协议。
     * 本地测试服务就是 front/player/mock-live/server.js。
     */
    UrlDialog {
        id: flvLiveDialog

        live: true
        heading: qsTr("播放直播地址（HTTP-FLV / HTTP-HLS）")
        placeholder: qsTr("http://127.0.0.1:9200/live/1080p.flv 或 …/live/hls/master.m3u8")
        hint: qsTr("两种都支持：FLV 直播填 …/live/1080p.flv（一档一个地址，切清晰度＝换流重连）；"
                   + "HLS 直播填 …/live/hls/master.m3u8（一个地址带全部档位，切清晰度不用重连）。"
                   + "本地测试：front/player/mock-live/server.js（它同时提供 FLV 与 HLS 两路直播，"
                   + "HLS 可用 --no-hls 关掉）。")
        onAccepted: home.openLiveUrl(url)
    }

    /* =======================================================================
     * 清单对象输入弹窗（首页"DASH 对象" / "HLS 对象"两颗按钮共用这一份）
     *
     * 【为什么要分协议】见上面那两个按钮的注释：DASH 和 HLS 的清单对象字段不一样
     * （DASH 靠 segmentTimeline 描述可变分片时长、段号从 startNumber 起；HLS 的段号
     * 从 mediaSequence 起），一个框贴两种对象的结果就是"打不开也不知道缺什么"。
     *
     * 这个组件因此带上 `protocol`：
     *   * 标题/占位示例/说明都跟着协议走；
     *   * 校验按协议做（缺 duration、缺 video[]、缺 segmentInfo、template 缺
     *     initialization、list 缺 segments[]、mediaSourceType 对不上…… 都会当场指出来）；
     *   * 交给播放器时也带上协议 —— CicadaPlayerItem::setManifestForProtocol() 会
     *     **补全缺失字段**（segmentTimeline → totalCount、initialization → media 模板、
     *     HLS 的 mediaSequence…）再喂给核心，所以"字段少几个"不再等于打不开。
     *   * 校验本身只做**提示**：真正的补全/拒绝以 C++ 那边为准（它的错误信息更具体）。
     *
     * "从 .json 文件载入"走 **AppDialogs.openJsonFile()**（C++ 原生对话框，句柄随
     * 对象销毁释放；见 AppFileDialogs.h），拿到的路径交给 playManifestFile(path, protocol)
     * —— 读文件也在 C++ 里做。
     * ======================================================================= */
    component ManifestDialog: Dialog {
        id: manifestDialog

        property alias text: manifestArea.text
        property string heading: ""
        /* "dash" 或 "hls"：决定校验规则、示例内容，以及交给播放器时的那条路。 */
        property string protocol: "dash"
        /* 从文件载入时记下文件名，给 player.manifestHint 用 */
        property string sourceName: ""

        modal: true
        width: 760

        /* 校验信息：为空表示没问题；非空时红色显示、并禁用"播放" */
        property string validationError: ""

        readonly property bool isHls: protocol === "hls"

        /* 本机示例（MinIO 上的测试片源）：示例按钮和占位提示都用它。 */
        readonly property string sampleBaseUrl: "http://127.0.0.1:9000/video/"
        readonly property string sampleIdPrefix: "333632953978589184-"

        /*
         * 生成一份**能直接跑**的示例清单对象。
         *
         * 用的是本机 MinIO 上那份测试片源的真实参数（8 条视频流 + 1 条音频，
         * 27 个分片、每片 10 秒、最后一片 8.2 秒 = 268.2 秒，和 output.mpd 一致），
         * 只取两条视频流（1080p / 480p）以免示例太长。
         *
         * **两种格式都支持**（用户要求，也是 hili-player 的 dist/test-{dash,hls}-v4*.json 那两套）：
         *   * explicit=false → `mode: "template"` + `segmentTimeline`（分片由
         *     initialization/media 模板 + 时间线展开，DASH 清单的常规形态）；
         *   * explicit=true  → `mode: "list"` + `segments[]`（每片显式给
         *     duration + url，等价于 m3u8 里一条条 EXTINF）。
         * HLS 的两份都多一个 mediaSequence（段号起点）。
         */
        function sampleManifest(explicit) {
            var base = manifestDialog.sampleBaseUrl
            var pfx = manifestDialog.sampleIdPrefix
            var isHls = manifestDialog.isHls
            var timeline = [ { t: 0, d: 10000000, r: 25 }, { d: 8200000 } ]
            var totalSegments = 27

            function segInfo(id) {
                var s = {
                    mode: explicit ? "list" : "template",
                    initialization: id + "-0.m4s",
                    targetDuration: 10,
                    timescale: 1000000,
                    startNumber: 1
                }

                if (explicit) {
                    /* 显式分片：26 片 10s + 最后一片 8.2s，URL 就是真实文件名。 */
                    var segs = []

                    for (var n = 1; n <= totalSegments; ++n) {
                        segs.push({
                            duration: (n === totalSegments) ? 8.2 : 10.0,
                            url: id + "-" + n + ".m4s"
                        })
                    }

                    s.segments = segs
                } else {
                    s.media = id + "-*.m4s"
                    s.totalCount = totalSegments
                    s.segmentTimeline = timeline
                }

                /* HLS：段号从 mediaSequence 起算（缺了它段号会错位）。 */
                if (isHls)
                    s.mediaSequence = 1

                return s
            }

            var reps = [
                { name: "video-h264-1920_1080", bw: 5000000, codecs: "avc1.4d402a", w: 1920, h: 1080 },
                { name: "video-h264-854_480", bw: 1500000, codecs: "avc1.4d401f", w: 854, h: 480 }
            ]
            var video = []

            for (var i = 0; i < reps.length; ++i) {
                var r = reps[i]
                var id = pfx + r.name
                video.push({
                    id: id,
                    baseUrl: base,
                    bandwidth: r.bw,
                    mimeType: "video/mp4",
                    codecs: r.codecs,
                    width: r.w,
                    height: r.h,
                    frameRate: 60,
                    segmentInfo: segInfo(id)
                })
            }

            var audioId = pfx + "audio"
            var obj = {
                mediaSourceType: manifestDialog.protocol,
                duration: 268.2,
                minBufferTime: 10,
                maxSegmentDuration: 10,
                video: video,
                audio: [ {
                    id: audioId,
                    baseUrl: base,
                    bandwidth: 128000,
                    mimeType: "audio/mp4",
                    codecs: "mp4a.40.2",
                    lang: "und",
                    segmentInfo: segInfo(audioId)
                } ]
            }

            return JSON.stringify(obj, null, 2)
        }

        /*
         * 按协议自检。返回空串 = 没发现问题。
         * 规则和 C++ 的 normalizeManifestObject() 一一对应（那边是权威实现）。
         */
        function check(o) {
            if (o.mediaSourceType !== undefined && ("" + o.mediaSourceType).toLowerCase() !== protocol) {
                return qsTr("这个对象的 mediaSourceType 是 “%1”，但你用的是 %2 入口。"
                            + "两种清单对象的分片字段不一样，请换对应的入口。")
                        .arg(o.mediaSourceType).arg(protocol.toUpperCase())
            }

            if (o.live !== true && !(o.duration > 0)) {
                return qsTr("缺少 duration（或 <= 0）。VOD 清单必须给总时长，例如 \"duration\": 268.2；"
                            + "直播清单请写 \"live\": true。")
            }

            if (!Array.isArray(o.video) || o.video.length === 0) {
                return qsTr("缺少 video[]（或它是空的）：核心要求至少一条视频流，否则会拒绝打开。")
            }

            var kinds = [ "video", "audio", "subtitle" ]

            for (var k = 0; k < kinds.length; ++k) {
                var arr = o[kinds[k]]
                if (!Array.isArray(arr))
                    continue

                for (var i = 0; i < arr.length; ++i) {
                    var rep = arr[i]
                    var where = kinds[k] + "[" + i + "] (id=" + (rep && rep.id ? rep.id : "?") + ")"

                    if (!rep || typeof rep !== "object")
                        return qsTr("%1 不是对象。").arg(where)

                    if (!rep.segmentInfo || typeof rep.segmentInfo !== "object") {
                        return qsTr("%1 缺少 segmentInfo：核心要靠它展开分片"
                                    + "（mode + initialization + media，或者直接给 segments[]）。").arg(where)
                    }

                    var si = rep.segmentInfo
                    var mode = ("" + (si.mode === undefined ? "" : si.mode)).toLowerCase()

                    if (mode === "")
                        mode = (Array.isArray(si.segments) && si.segments.length > 0) ? "list" : "template"

                    if (mode === "template" && (!si.initialization || ("" + si.initialization) === "")) {
                        return qsTr("%1 是 template 模式但没有 initialization"
                                    + "（初始化分片，例如 \"video-h264-1920_1080-0.m4s\"）。").arg(where)
                    }

                    if (mode === "list" && (!Array.isArray(si.segments) || si.segments.length === 0)) {
                        return qsTr("%1 是 list 模式但 segments[] 是空的。").arg(where)
                    }

                    if (mode !== "template" && mode !== "list" && mode !== "single") {
                        return qsTr("%1 的 segmentInfo.mode 是 “%2”，只支持 template / list / single。")
                                .arg(where).arg(si.mode)
                    }
                }
            }

            return ""
        }

        function validate() {
            var t = ("" + manifestArea.text).trim()

            if (t === "") {
                validationError = qsTr("内容为空")
                return false
            }

            var parsed = null

            try {
                parsed = JSON.parse(t)
            } catch (e) {
                validationError = qsTr("不是合法的 JSON：") + e.message
                return false
            }

            if (parsed === null || typeof parsed !== "object" || Array.isArray(parsed)) {
                validationError = qsTr("最外层必须是一个清单对象 {}")
                return false
            }

            validationError = check(parsed)
            return validationError === ""
        }

        function start() {
            manifestArea.text = ""
            sourceName = ""
            validationError = ""
            open()
            Qt.callLater(function () { manifestArea.forceActiveFocus() })
        }

        background: Rectangle {
            color: QtPlayerTheme.panelBg
            border.width: 1
            border.color: QtPlayerTheme.panelBorder
            radius: 4
        }

        contentItem: Column {
            spacing: 10
            topPadding: 18
            bottomPadding: 6
            leftPadding: 18
            rightPadding: 18

            Text {
                width: parent.width - parent.leftPadding - parent.rightPadding
                wrapMode: Text.WordWrap
                text: manifestDialog.heading
                color: QtPlayerTheme.panelText
                font.pixelSize: 14
                font.bold: true
            }

            ScrollView {
                width: parent.width - parent.leftPadding - parent.rightPadding
                height: 260
                clip: true

                TextArea {
                    id: manifestArea

                    placeholderText: manifestDialog.isHls
                                     ? qsTr('{\n  "mediaSourceType": "hls",\n  "duration": 268.2,\n  "video": [ { "id": "…-video-h264-1920_1080", "baseUrl": "http://…/",\n                 "bandwidth": 5000000, "width": 1920, "height": 1080,\n                 "codecs": "avc1.4d402a", "mimeType": "video/mp4",\n                 "segmentInfo": { "mode": "template", "mediaSequence": 1,\n                                  "initialization": "…-0.m4s", "media": "…-*.m4s",\n                                  "timescale": 1000000, "startNumber": 1, "totalCount": 27 } } ],\n  "audio": [ … ]\n}')
                                     : qsTr('{\n  "mediaSourceType": "dash",\n  "duration": 268.2,\n  "video": [ { "id": "…-video-h264-1920_1080", "baseUrl": "http://…/",\n                 "bandwidth": 5000000, "width": 1920, "height": 1080,\n                 "codecs": "avc1.4d402a", "mimeType": "video/mp4",\n                 "segmentInfo": { "mode": "template",\n                                  "initialization": "…-0.m4s", "media": "…-*.m4s",\n                                  "timescale": 1000000, "startNumber": 1,\n                                  "segmentTimeline": [ { "t": 0, "d": 10000000, "r": 25 },\n                                                       { "d": 8200000 } ] } } ],\n  "audio": [ … ]\n}')
                    placeholderTextColor: QtPlayerTheme.panelHintText
                    color: QtPlayerTheme.controlText
                    font.pixelSize: 12
                    font.family: "Consolas"
                    wrapMode: TextArea.Wrap
                    selectByMouse: true
                    background: Rectangle {
                        color: QtPlayerTheme.settingsPanelInputBg
                        border.width: 1
                        border.color: QtPlayerTheme.panelBorder
                        radius: 2
                    }
                    onTextChanged: manifestDialog.validationError = ""
                }
            }

            Row {
                spacing: 10

                Button {
                    text: qsTr("填入示例（模板分片）")
                    onClicked: {
                        manifestArea.text = manifestDialog.sampleManifest(false)
                        manifestDialog.validationError = ""
                    }
                }

                Button {
                    text: qsTr("填入示例（显式分片）")
                    onClicked: {
                        manifestArea.text = manifestDialog.sampleManifest(true)
                        manifestDialog.validationError = ""
                    }
                }

                Button {
                    text: qsTr("从 .json 文件载入")
                    /*
                     * 优先 C++ 的原生对话框（句柄随对象销毁释放）；平台不支持时
                     * 退回 QML 的 FileDialog。两条路最后都走 playManifestFile(path, protocol)。
                     */
                    onClicked: {
                        if (AppDialogs.available) {
                            var f = AppDialogs.openJsonFile()

                            if (f !== "") {
                                manifestDialog.sourceName = f
                                manifestDialog.loadAndPlay(f)
                            } else if (AppDialogs.lastError !== "") {
                                console.warn("[home] 原生文件对话框不可用：" + AppDialogs.lastError)
                                manifestFileDialog.open()
                            }
                        } else {
                            manifestFileDialog.open()
                        }
                    }
                }

                Text {
                    width: 220
                    elide: Text.ElideMiddle
                    anchors.verticalCenter: parent.verticalCenter
                    text: manifestDialog.sourceName
                    color: QtPlayerTheme.panelHintText
                    font.pixelSize: 11
                }
            }

            Text {
                width: parent.width - parent.leftPadding - parent.rightPadding
                wrapMode: Text.WordWrap
                font.pixelSize: 11
                color: manifestDialog.validationError !== "" ? "#ff6b6b" : QtPlayerTheme.panelHintText
                text: manifestDialog.validationError !== ""
                      ? manifestDialog.validationError
                      : (manifestDialog.isHls
                         ? qsTr("HLS 对象：支持 mode=template（segmentTimeline + mediaSequence 段号起点）和 mode=list（segments[] 显式分片）两种格式。缺字段会在播放时自动补全（CicadaPlayerItem::setManifestForProtocol）。")
                         : qsTr("DASH 对象：支持 mode=template（segmentTimeline，最后一片可以更短）和 mode=list（segments[] 显式分片）两种格式。缺字段会在播放时自动补全（CicadaPlayerItem::setManifestForProtocol）。"))
            }
        }

        /*
         * 选好文件之后：交给首页那条统一入口（它会建/复用播放器窗口，读文件在 C++ 里做）。
         * 组件本身不解析文件内容 —— 解析/补全/校验都在 C++ 的核心那条路上。
         */
        function loadAndPlay(path) {
            home.openPlayerManifestFile(path, protocol)
            manifestDialog.reject()
        }

        footer: Item {
            implicitHeight: 52

            Row {
                anchors.right: parent.right
                anchors.rightMargin: 18
                anchors.verticalCenter: parent.verticalCenter
                spacing: 12

                Button {
                    text: qsTr("取消")
                    onClicked: manifestDialog.reject()
                }

                Button {
                    text: qsTr("播放")
                    enabled: manifestArea.text.trim() !== ""
                    onClicked: {
                        if (manifestDialog.validate())
                            manifestDialog.accept()
                    }
                }
            }
        }
    }

    ManifestDialog {
        id: dashManifestDialog

        protocol: "dash"
        heading: qsTr("播放 DASH 清单对象（JSON）")
        onAccepted: home.openPlayerManifest(text, sourceName, protocol)
    }

    ManifestDialog {
        id: hlsManifestDialog

        protocol: "hls"
        heading: qsTr("播放 HLS 清单对象（JSON）")
        onAccepted: home.openPlayerManifest(text, sourceName, protocol)
    }

    FileDialog {
        id: manifestFileDialog

        title: qsTr("选择清单 JSON 文件")
        nameFilters: [qsTr("清单 JSON (*.json)"), qsTr("所有文件 (*)")]
        /*
         * **兜底路径**：只有平台不支持 C++ 原生对话框（AppDialogs.available 为 false）
         * 时才会走到这里。优先那条路见按钮里的 onClicked（AppDialogs.openJsonFile()）。
         *
         * 直接调 C++ 的 loadManifestFile()，**不在 QML 里读文件**：
         * Qt 默认禁止 qrc: 文档对 file:// 发 XHR（QML_XHR_ALLOW_FILE_READ 未开），
         * 在 QML 里读本地文件会失败。读文件 + 交给播放器都在组件里做（和命令行
         * appQtPlayer.exe xxx.json 完全同一条路）。
         */
        onAccepted: {
            /* 哪个对话框叫起来的就按哪个协议走（两个入口各带自己的协议）。 */
            const dlg = dashManifestDialog.visible ? dashManifestDialog : hlsManifestDialog
            dlg.sourceName = selectedFile
            dlg.loadAndPlay(selectedFile)
        }
    }

    /* ---------------- 卡片网格 ---------------- */
    GridView {
        id: grid

        anchors {
            left: parent.left
            right: parent.right
            top: toolBar.bottom
            bottom: parent.bottom
            margins: 16
        }
        clip: true
        cellWidth: 240
        cellHeight: 176
        model: library.items

        delegate: Item {
            id: card

            required property var modelData
            required property int index

            width: grid.cellWidth - 12
            height: grid.cellHeight - 12

            Rectangle {
                id: cardBody

                anchors.fill: parent
                radius: 6
                color: cardHover.hovered ? "#2a2f38" : "#20242b"
                border.width: (library.currentIndex === card.index) ? 2 : 0
                border.color: QtPlayerTheme.playerAccent

                Image {
                    id: thumb

                    anchors {
                        left: parent.left
                        right: parent.right
                        top: parent.top
                    }
                    height: 120
                    source: card.modelData.thumb ? "file:///" + card.modelData.thumb : ""
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    clip: true

                    Rectangle {
                        anchors.fill: parent
                        visible: thumb.status !== Image.Ready
                        color: "#14161a"

                        Text {
                            anchors.centerIn: parent
                            text: qsTr("无缩略图")
                            color: "#5c6674"
                            font.pixelSize: 12
                        }
                    }
                }

                Column {
                    anchors {
                        left: parent.left
                        right: parent.right
                        top: thumb.bottom
                        margins: 8
                    }
                    spacing: 4

                    Text {
                        width: parent.width
                        text: card.modelData.name
                        color: "#e8ecf1"
                        font.pixelSize: 13
                        elide: Text.ElideMiddle
                    }

                    Text {
                        text: (card.modelData.sizeBytes / 1048576).toFixed(1) + " MB"
                        color: "#8b95a3"
                        font.pixelSize: 11
                    }
                }
            }

            HoverHandler {
                id: cardHover
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: home.openPlayer(card.index)
            }
        }
    }

    Text {
        anchors.centerIn: parent
        visible: library.count === 0
        text: qsTr("把视频文件夹拖进来，或者点左上角的“打开文件夹”")
        color: "#5c6674"
        font.pixelSize: 15
    }

    /* 应用层（main.cpp）接这两个信号：打开设置页 / 打开（或复用）播放器窗口 */
    signal settingsRequested()

    /*
     * ---------------- 设置页 ----------------
     *
     * 【本轮补上的】原来这个窗口只发了个 `settingsRequested()` 信号，而 main.cpp 那边
     * 没有接、也没有别的地方接 —— 所以工具条那颗「设置」按钮点下去**什么都不会发生**。
     * 现在直接挂上 SettingsPage.qml 那一份（和播放器窗口**共用同一个文件**，用户要求
     * "首页的设置按钮 → 打开它；播放器窗口的齿轮 → 打开同一份"）。
     *
     * player 传的是**当前那个播放器窗口里的播放器**（还没开播放器窗口时是 null）。
     *
     * 【为什么不干脆写 null】用户要求"设置的内容**实时生效**"：在首页把「硬件解码」一关，
     * 正在播的那条视频就得当场重开并跳回原来的位置。所以只要播放器窗口已经开出来了，
     * 首页这份设置页就**直接作用在它身上** —— 两个窗口改的是同一个 player 对象，
     * 天然联动，不需要任何同步代码。
     * 还没开播放器窗口时没有对象可作用，那几行由 SettingsPage 自己按 hasPlayer 置灰
     * 不可点（见那个文件里 `enabled: page.hasPlayer` 那几处），「界面主题」照常可用。
     *
     * 摆放：铺满整个窗口。SettingsPage 根对象自带 z:20，所以它会盖住标题栏、工具条和
     * 卡片网格；点遮罩（面板以外）就关掉，和播放器窗口那边行为一致。
     */
    SettingsPage {
        id: settingsPage

        anchors.fill: parent
        /*
         * 【为什么写 home.playerWindow 而不是 playerWindow】playerWindow 是本窗口的属性，
         * 而 `player` 这一行左边也是本组件的一个属性名 —— 不加 home. 的话右边那个
         * playerWindow 没问题，但整行读起来容易和"同名自引用"那个坑混起来
         * （见 HANDOVER-TODO.md 第四节第 1 条：`danmaku: danmaku` 右边会解析成组件自己的属性）。
         * 这里用 id 前缀把话说死。
         */
        player: home.playerWindow !== null ? home.playerWindow.player : null
    }

    function openSettings() {
        settingsPage.open()
        /* 信号照旧发出去：main.cpp 现在没接，但它是这个窗口的对外接口，留着不破坏调用方 */
        settingsRequested()
    }

    /*
     * 播放器窗口：**只有一个实例，开着的时候复用；关掉之后销毁**。
     *   * 第一次点卡片 → 用 Qt.createComponent 建 Main.qml（无标题栏那个播放器窗口）；
     *   * 还开着的时候再点别的视频 → 不再新建，只换源 + 提到最前（用户要求"复用"）；
     *   * 用户关掉它 → 等它**真的关掉**之后销毁对象，下次点卡片新建一个。
     *
     * ===========================================================================
     * 【什么时候销毁是安全的 —— 两个陷阱，我上一版两个都踩了】
     *
     * ⚠️ 陷阱一：**没有 QObject 父对象的对象，`destroy()` 是"立刻 delete"**
     *     QML 的 `Object::destroy()` 内部是这么分支的（qv4qobjectwrapper.cpp）：
     *         if (object->parent()) object->deleteLater();   // 有父对象 → 推迟到下一轮事件循环
     *         else                  delete object;           // 没有父对象 → 当场删
     *     上一版我把窗口建成了 `createObject(null)`（为了任务栏能出现），于是 destroy()
     *     走的是**当场 delete** 那一条：QQuickWindow 在它的场景图 / RHI / 渲染线程还在
     *     收尾的时候被直接删掉 → 用户实测"关掉播放器后再点卡片，卡死 + 整个程序闪退"。
     *
     * ⚠️ 陷阱二：**不能在 `closing` 信号里动手**
     *     那一刻窗口正在处理关闭事件，close 还没走完、场景图也还活着。
     *
     * ✅ 所以正确的做法是两件事一起做：
     *     1) **给它一个 QObject 父对象，但这个父对象不能是 Window**：
     *        用 `home.contentItem`（一个 QQuickItem）。
     *          * 是 Window 的话 → 新窗口变成从属窗口（owned window），任务栏里就不显示了
     *            （用户报的"开两个窗口任务栏只有一个"就是这个）；
     *          * 是 Item 的话 → 既不是从属窗口（有自己的任务栏按钮），又**有父对象**，
     *            于是 destroy() 走 deleteLater()，在下一轮事件循环里安全地删。
     *     2) **等它真的不可见了再销毁**：挂 `visibleChanged`，`visible === false`
     *        （= close 走完了）再用 Qt.callLater 推到下一轮 —— 那时既没有关闭事件在处理，
     *        场景图也收完了，不管 destroy() 走哪条分支都不会撞上。
     *
     * 顺序上还有一条：**先把 playerWindow 置空、再 destroy**。置空会让 SettingsPage
     * 那句 `player: home.playerWindow.player` 立刻变 null，不会再有绑定去读一个
     * 正在被销毁的对象。
     * ===========================================================================
     */
    property var playerWindow: null

    /*
     * 建（或复用）播放器窗口，返回它；失败返回 null。
     *
     * 点卡片和点 DASH/HLS 按钮走的是**同一套**：窗口只有一个、开着就复用、
     * 关掉之后销毁（那两个陷阱和理由见上面那段注释）。
     */
    function ensurePlayerWindow() {
        if (playerWindow === null) {
            var component = Qt.createComponent("qrc:/qt/qml/QtPlayer/Main.qml")

            if (component.status !== Component.Ready) {
                console.warn("[home] 播放器窗口创建失败：" + component.errorString())
                return null
            }

            /* parent 用 contentItem，不是 home、也不是 null —— 两条理由见上面那段 */
            playerWindow = component.createObject(home.contentItem)

            if (playerWindow !== null) {
                /*
                 * 关掉之后（visible 变假）销毁。用 visibleChanged 而不是 closing：
                 * closing 是"正在关"，visible=false 才是"已经关完了"。
                 * 这里用 home.playerWindow 而不是闭包捕获，保证永远操作当前那一个。
                 */
                playerWindow.visibleChanged.connect(function () {
                    if (home.playerWindow !== null && !home.playerWindow.visible)
                        Qt.callLater(home.reapPlayerWindow)
                })
            }
        }

        if (playerWindow === null)
            console.warn("[home] 播放器窗口对象为空")

        return playerWindow
    }

    /* 把窗口显示出来并提到最前（三处调用都一样） */
    function showPlayerWindow() {
        playerWindow.show()
        playerWindow.raise()
        playerWindow.requestActivate()
    }

    function openPlayer(index) {
        var path = library.pathAt(index)

        if (path === "")
            return

        library.currentIndex = index

        if (ensurePlayerWindow() === null)
            return

        playerWindow.playFromLibrary(library, index)
        showPlayerWindow()
    }

    /*
     * 地址规范化（用户贴进来的东西三种都常见）。**三条路（DASH / HLS / FLV 直播）共用这一份**，
     * 免得哪天只改了一条：
     *   * 已经有**已知** scheme（http/https/file/rtsp/rtmp…）→ 原样用；
     *   * "D:\x.m3u8" / "D:/x.m3u8"（盘符开头）→ 补成 file:///；
     *   * 其余（比如 "cdn.xx.com/a/index.m3u8"）→ 补 https://。
     */
    function normalizeAddress(url) {
        var u = ("" + url).trim()

        if (u === "") {
            console.warn("[home] 地址为空，忽略")
            return ""
        }

        /*
         * 【2026-09-21 修：scheme 必须**白名单**匹配，不能只判"像不像 scheme"】
         *
         * 原来只有一句 `/^[a-zA-Z][a-zA-Z0-9+.\-]*:/`，任何 "word:" 都算协议头。
         * 两个后果：
         *   1) 打错一个字母（"zhttp://cdn.x/a.m3u8"）会被当成"协议齐全"原样交给
         *      播放器，于是打开失败，日志里只有一句莫名其妙的 URL，看不出是输入问题；
         *   2) 更隐蔽的是 "D:\x.m3u8" —— "D:" 也满足那个正则，于是盘符路径连
         *      file:/// 都补不上（下面那段盘符判断永远轮不到它）。
         *
         * 现在：先认盘符，再按白名单判协议。白名单外的协议说明是打错了，
         * 去掉它、按域名补 https://（"zhttp://cdn.x/a.m3u8" → "https://cdn.x/a.m3u8"），
         * 这样用户改错一个字母仍然能播，而不是拿到一个必失败的地址。
         */
        var knownSchemes = ["http", "https", "file", "ftp", "ftps", "qrc", "content",
                            "rtsp", "rtmp", "rtmps", "rtp", "udp", "srt", "tcp", "hls", "data"]
        var isDrivePath = /^[a-zA-Z]:[\\/]/.test(u)
        var schemeMatch = /^([a-zA-Z][a-zA-Z0-9+.\-]*):/.exec(u)
        var scheme = (schemeMatch !== null && !isDrivePath)
                ? schemeMatch[1].toLowerCase() : ""

        if (scheme !== "" && knownSchemes.indexOf(scheme) < 0) {
            /* 只对 "xxx://host/…" 这种做纠正；"word:something" 也可能是个不含
             * 域名的怪输入，那种情况去掉协议头后会带上前导斜杠，一起清掉。 */
            var rest = u.substring(schemeMatch[0].length).replace(/^\/+/, "")
            console.warn("[home] 未知协议 \"" + scheme + ":\"，按 https 处理：" + rest)
            u = rest
            scheme = ""
        }

        if (isDrivePath) {
            u = "file:///" + u.replace(/\\/g, "/")
        } else if (scheme === "") {
            u = "https://" + u
        }

        return u
    }

    /*
     * 用**网络地址**打开播放器（首页那两颗 DASH / HLS 按钮）。
     *
     * 和 openPlayer(index) 只差"片源从哪来"：窗口那一套完全一样。
     * 不做别的加工 —— 清单的解析和"有哪些清晰度"都是框架的事。
     */
    function openPlayerUrl(url) {
        var u = normalizeAddress(url)

        if (u === "") {
            return
        }

        if (ensurePlayerWindow() === null)
            return

        playerWindow.playUrl(u)
        showPlayerWindow()
    }

    /*
     * 用 **FLV 直播地址**打开**直播窗口**（首页「直播 FLV 地址」）。
     *
     * 【为什么不复用点播窗口】用户要求直播播放器**另写一套 UI**：直播没有进度条/时长、
     * 不能 seek、清晰度是"换一条流重连"、还要显示"直播中/人气"和实时弹幕。
     * 所以这里建的是 LiveMain.qml（见 LivePlayerView.qml），和 Main.qml 那套完全独立。
     *
     * 地址规范化和点播共用 normalizeAddress()：本地测试时直接填
     *   http://127.0.0.1:9200/live/1080p.flv
     * 就行（或者只填 127.0.0.1:9200/live/1080p.flv，会自动补 https —— 那种情况请写全 http://，
     * 因为本地服务是**明文 HTTP**，补成 https 会连不上）。
     */
    function openLiveUrl(url) {
        var u = normalizeAddress(url)

        if (u === "") {
            return
        }

        if (ensureLiveWindow() === null)
            return

        liveWindow.playUrl(u)
        showLiveWindow()
    }

    /*
     * 「FLV 视频」：选一个**本地 FLV 文件**（或任意视频文件）→ 用**现有（点播）播放器**播。
     *
     * 为什么也允许别的后缀：这颗按钮的语义是"播一个 FLV 文件"，但用户手边那个文件可能是
     * .mp4/.mkv；文件框里本来就有"所有文件"，选到什么就放什么，不必多弹一次错误。
     * FLV 点播和 mp4 一样有时长、能拖进度（框架用 ffmpeg 解 FLV，走的就是同一条路）。
     *
     * 文件框优先 C++ 的原生对话框（句柄随对象销毁释放，见 src/AppFileDialogs.h）；
     * 平台不支持时退回 QML 的 FileDialog（下面那个 videoFileDialog）。
     */
    function openFlvFile() {
        if (AppDialogs.available) {
            var f = AppDialogs.openVideoFile()

            if (f !== "") {
                playLocalFile(f)
            } else if (AppDialogs.lastError !== "") {
                console.warn("[home] 原生文件对话框不可用：" + AppDialogs.lastError)
                videoFileDialog.open()
            }

            return
        }

        videoFileDialog.open()
    }

    /* 本地文件（路径）→ 现有播放器窗口。统一在这里补 file:/// 。 */
    function playLocalFile(path) {
        var p = ("" + path).trim()

        if (p === "") {
            return
        }

        var u = /^[a-zA-Z]:[\\/]/.test(p) ? "file:///" + p.replace(/\\/g, "/")
                                         : normalizeAddress(p)

        if (ensurePlayerWindow() === null)
            return

        playerWindow.playUrl(u)
        showPlayerWindow()
    }

    /*
     * 用**清单对象**打开播放器（首页"DASH 对象"/"HLS 对象"两颗按钮；集成方也可以直接调）。
     *
     *   openPlayerManifest(jsonText, name, protocol)  —— JSON 文本（粘贴/文件）
     *   playManifestObject(obj, name)                 —— 直接把 JS 对象传进来
     *
     * protocol（"dash"/"hls"，可省）非空时走**按协议补全**那条路
     * （CicadaPlayerItem::setManifestForProtocol）：补齐 segmentInfo 里缺的字段、
     * 写死 mediaSourceType，缺关键字段时给出人话错误。省掉 protocol = 原样透传。
     *
     * 和 openPlayerUrl 只差"片源从哪来"：窗口那一套（复用/销毁/置顶）完全一样。
     * 之后的解码、渲染（**macOS 的 VideoToolbox 零拷贝也走这一条**）、清晰度列表、
     * 双路切换和 ABR 全都和 URL 片源一致 —— 见 Main.qml 里 playManifestObject
     * 那段说明。
     */
    function openPlayerManifest(jsonText, name, protocol) {
        var text = ("" + jsonText).trim()

        if (text === "") {
            console.warn("[home] 清单内容为空，忽略")
            return false
        }

        if (ensurePlayerWindow() === null)
            return false

        var ok = playerWindow.playManifestJson(text, name, protocol)
        showPlayerWindow()
        return ok
    }

    /* 直接把对象交给播放器（QML/JS 里 `home.playManifestObject({...})` 即可）。 */
    function playManifestObject(manifest, name) {
        if (manifest === null || manifest === undefined) {
            console.warn("[home] 清单对象为空，忽略")
            return false
        }

        if (ensurePlayerWindow() === null)
            return false

        var ok = playerWindow.playManifestObject(manifest, name)
        showPlayerWindow()
        return ok
    }

    /* 本地 .json 清单文件（路径或 file:// URL）：读文件在组件里做。
     * protocol 可省；给了就按协议补全。 */
    function openPlayerManifestFile(path, protocol) {
        if (ensurePlayerWindow() === null)
            return false

        var ok = playerWindow.playManifestFile(path, protocol)
        showPlayerWindow()
        return ok
    }

    /*
     * "打开文件夹"：优先 C++ 的原生目录选择框（见 src/AppFileDialogs.h），
     * 选完交给 VideoLibrary 递归扫描。平台不支持时才退回 QML 的 FolderDialog。
     *
     * 为什么不用 QML 的 FolderDialog 作为主路径：它底层同样是个原生选择框，
     * 组件对象被 QML 引擎持有 ⇒ 句柄（和文件系统监视器）不随 close() 释放，
     * 连开几次就会出现"点了没反应 / 目录被占用"。C++ 那条路每次现建现销。
     */
    function chooseFolder() {
        if (!AppDialogs.available) {
            folderDialog.open()
            return
        }

        home.requestActivate()

        var dir = AppDialogs.openDirectory()

        if (dir === "") {
            if (AppDialogs.lastError !== "") {
                console.warn("[library] 原生目录选择框不可用：" + AppDialogs.lastError + "（退回 QML FolderDialog）")
                folderDialog.open()
            }
            return
        }

        var added = library.addFolder(dir, 3)
        console.warn("[library] 扫描 " + dir + "，新增 " + added + " 个视频")
    }

    /*
     * 销毁那个**已经关掉**的播放器窗口对象。
     *
     * 两道保险：
     *   * 还可见就什么都不做 —— 说明用户又打开了（或者根本没关成），不能销毁；
     *   * 先把引用置空再 destroy —— 断掉所有指向它的绑定，之后它才是"没人引用"的。
     *
     * 资源其实在 Main.qml 的 onClosing 里就已经还回去了（清空片源 → destroyPlayer()
     * → 解码器/帧/输入视图/表面池），这里销毁的是窗口和它那棵 QML 对象树本身。
     */
    function reapPlayerWindow() {
        if (playerWindow === null)
            return

        if (playerWindow.visible)
            return

        var w = playerWindow
        playerWindow = null
        w.destroy()
    }

    /* =======================================================================
     * 直播窗口（LiveMain.qml）：和点播窗口**各自一个实例**，互不干扰
     *
     * 为什么不像点播那样"一个窗口复用"：用户可能一边开着点播、一边看直播
     * （两个窗口同时在任务栏里，这是合理的）。所以这里单独一份引用、单独一套
     * 建/销逻辑 —— 但**两个陷阱完全一样**（destroy() 会当场 delete、不能在 closing 里
     * 动手），所以写法逐行照抄上面 ensurePlayerWindow/reapPlayerWindow：
     *   * parent 用 home.contentItem（有父对象 → destroy() 走 deleteLater；不是 Window
     *     → 不会变成从属窗口、任务栏里有独立按钮）；
     *   * 等 visible 变假（= 关完了）再 Qt.callLater 销毁；
     *   * 先置空引用、再 destroy。
     * ======================================================================= */

    property var liveWindow: null

    function ensureLiveWindow() {
        if (liveWindow === null) {
            var component = Qt.createComponent("qrc:/qt/qml/QtPlayer/LiveMain.qml")

            if (component.status !== Component.Ready) {
                console.warn("[home] 直播窗口创建失败：" + component.errorString())
                return null
            }

            liveWindow = component.createObject(home.contentItem)

            if (liveWindow !== null) {
                liveWindow.visibleChanged.connect(function () {
                    if (home.liveWindow !== null && !home.liveWindow.visible)
                        Qt.callLater(home.reapLiveWindow)
                })
            }
        }

        if (liveWindow === null)
            console.warn("[home] 直播窗口对象为空")

        return liveWindow
    }

    function showLiveWindow() {
        liveWindow.show()
        liveWindow.raise()
        liveWindow.requestActivate()
    }

    function reapLiveWindow() {
        if (liveWindow === null)
            return

        if (liveWindow.visible)
            return

        var w = liveWindow
        liveWindow = null
        w.destroy()
    }
}
