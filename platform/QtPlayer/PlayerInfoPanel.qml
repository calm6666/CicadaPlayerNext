// ===========================================================================
// 「视频统计信息」面板（复刻参考实现 component/videoinfo/index.ts + index.scss）
//
// 【参考的 DOM】
//     <div class="player-info-container">
//       <div class="player-info-title">统计信息 <span class="player-info-close">✕</span></div>
//       <div class="player-info-panel">
//         <div class="info-line"><span class="info-title">媒体类型:</span>
//                                <span class="info-data">video/mp4;codecs="…",audio/mp4;codecs="…"</span></div>
//         … 播放器类型 / 分辨率 / 视频码率 / 音频码率 / 视频 Host / 音频 Host /
//           视频缓存速度 / 音频缓冲速度
//       </div>
//     </div>
//
// 【参考的 CSS（videoinfo/index.scss）】
//     .player-info-container { position:absolute; left:10px; top:10px; z-index:80;
//                              background:rgba(33,33,33,.9); border-radius:4px; color:#fff;
//                              line-height:18px; padding-bottom:15px; text-align:left }
//     .player-info-title     { border-bottom:1px solid hsla(0,0%,100%,.1); font-size:16px;
//                              line-height:40px; text-align:center }
//     .player-info-close     { position:absolute; right:10px; top:10px; width:22px; height:22px }
//     .player-info-panel     { padding:15px 26px 0 }
//     .info-line             { min-width:290px }
//     .info-title            { font-size:12px; font-weight:500; width:100px; text-align:right;
//                              margin-right:10px }
//     .info-data             { color:#999; font-size:12px; max-width:470px; margin-right:6px;
//                              white-space:nowrap; overflow:hidden; text-overflow:ellipsis }
//
// 【数据全部来自 player.stats（框架真实读数）】键名和来源见 CicadaPlayerItem::buildStats()。
// 唯一"取不到"的两项（核心没有任何 API 提供）如实标注：
//   * 视频/音频单独缓存速度 —— 核心只给一个合计值，所以两行都显示它（并注明"合计"）；
//   * 媒体类型里的 codecs 字符串 —— 清单对象直接取清单里的；普通 URL 片源核心只有
//     编解码器枚举，这里按枚举翻译（形如 video/* [H.264]）。
//
// 【有意偏离参考 CSS 的一处】参考的 `.info-title { text-indent: -99em }` 会把标签文字
// 推到可视区外面（等于只显示值、不显示标签），那是它那份样式里的一个 bug；
// 这里保留标签（面板才有可用性），字号/颜色/宽度/对齐其它都按参考来。
//
// 【布局：所有内容都放进**同一个 Column**，高度由它算出来】
//   上一版把标题、明细、曲线区做成三个各自锚定/相加的块，高度公式漏掉了两处外边距，
//   于是"面板高度偏低、底部那行文字溢出去"（用户实测）。现在内容一棵树（contentColumn），
//   面板高度 = contentColumn.height + 参考的底部 padding(15)，并且 clip: true —— 结构上
//   不可能再溢出。
// ===========================================================================
import QtQuick
import QtPlayer

Rectangle {
    id: infoPanel

    /* 开着没有（PlayerView 用它判浮层、协议徽标也要避让） */
    property bool opened: false
    /* 统计信息（player.stats 直接传进来） */
    property var stats: ({})
    /* 曲线数据（player.statsHistory：每项 {t,speed,fps,buffer}，500ms 一个点） */
    property var history: []
    /* 曲线是不是正在采样（player.statsSampling） */
    property bool sampling: false
    /*
     * 播放器组件本体（PlayerView 传进来）。
     * 【为什么要有它】采样的开关必须由**本组件**自己驱动 —— 见下面 onOpenedChanged 的说明。
     */
    property var playerItem: null

    signal closeRequested()

    function open() { opened = true }
    function close() { opened = false }

    /*
     * 重画信号：面板每次打开都 +1，交给下面三条曲线。
     *
     * 为什么需要它：曲线那几个画图元素在**面板隐藏**的时候也被创建过（QML 一上来就建），
     * 打开时显式让它们重画一次最稳（曲线区曾经空白，用户实测）。
     *
     * 【踩过的坑 —— 这就是曲线一直没数据的根因】不能为同一个信号写两个处理器！
     * 之前这里另起了一个 `onOpenedChanged: if (opened) repaintTick++`，
     * 于是同一个对象上有两个 openedChanged 处理器，**后写的那个把前面那个顶掉了**
     * （QML 不报错，静默丢弃一个），结果 PlayerView 里的
     * `player.startStatsSampling()` 永远没被执行 → sampling 一直是 false →
     * statsHistory 空 → 曲线画不出来、曲线区"当前/峰值"也一直是空。
     * 现在两件事合并进**同一个**处理器（下面 onOpenedChanged），再也不会互相顶掉。
     */
    property int repaintTick: 0

    /*
     * ---- 采样开关 + 重画（同一个 openedChanged 处理器，绝不能拆成两个）----
     *
     * 【为什么开关要写在这里而不是外面的实例上】上一版是在 PlayerView 里给这个实例写
     * `onOpenedChanged: if (opened) player.startStatsSampling() ...`，实测曲线一直没有数据
     * （曲线区那三个"当前/峰值"也永远是空），也就是**那段处理根本没跑到**：
     * 实例上的处理器和组件内部这个处理器同样是"同一个信号的第二个赋值"，一样会被顶掉。
     * 放进声明 opened 的这个组件里、只依赖传进来的 playerItem，路径最短、也最好查。
     *
     * 另外打一行 console.warn（会进应用日志）：`[info] stats panel opened=…`。
     * 打开面板后日志里没有这一行，就说明是 QML 这一层没走到（而不是 C++ 没采样）。
     */
    onOpenedChanged: {
        console.warn("[info] stats panel opened=" + opened
                     + " playerItem=" + (playerItem !== null ? "ok" : "null"))

        if (opened)
            repaintTick++

        if (playerItem === null)
            return

        if (opened)
            playerItem.startStatsSampling()
        else
            playerItem.stopStatsSampling()
    }

    /*
     * 采样数据到了就留一行日志（第 1 个点、以及其后每 10 个点 ≈ 5 秒一行）。
     * 和 C++ 里的 `stats curves: N points` 对着看，能立刻分清是
     * "C++ 没采" 还是 "采了没送到 QML"。
     */
    onHistoryChanged: {
        const n = history ? history.length : 0

        if (n > 0 && (n === 1 || n % 10 === 0))
            console.warn("[info] stats history in QML: " + n + " points")
    }

    /* 行数据：标题 + 取值（取不到就显示 "--"） */
    readonly property var lines: stats === undefined ? [] : [
        { title: qsTr("媒体类型:"),     value: text("mediaType") },
        { title: qsTr("播放器类型:"),   value: text("playerType") },
        { title: qsTr("分辨率:"),       value: resolutionText() },
        { title: qsTr("视频码率:"),     value: text("videoBitrateText") },
        { title: qsTr("音频码率:"),     value: text("audioBitrateText") },
        { title: qsTr("视频 Host:"),    value: text("videoHost") },
        { title: qsTr("音频 Host:"),    value: text("audioHost") },
        /*
         * 【这两行的值为什么一样】核心只给**一个**下载速度：MediaPlayerUtil 里
         * mReadGotSize 是所有流一起累加的（MediaPlayerUtil.cpp:79-135，
         * CurrentDownLoadSpeed 回调就这一个数，ABR 也只用它），没有"音频/视频分开"的
         * 吞吐量可读。参考那份面板是网页播放器（每个分片单独统计），所以有两行。
         * 这里保留参考的行名/顺序，但把"这是合计值"写在值里 —— 显示真实数据，
         * 不能让用户以为音频那一行是单独测出来的。
         */
        { title: qsTr("视频缓存速度:"), value: speedText("videoSpeedText") },
        { title: qsTr("音频缓冲速度:"), value: speedText("audioSpeedText") }
    ]

    /*
     * 参考那份面板只有上面 9 行；下面这几行是**我们能真实拿到、而且排查时最有用**的
     * （渲染帧率/丢帧、缓冲时长、解码方式、容器），单独一组放在分隔线之下，
     * 不去改动参考那 9 行的顺序和文案。
     */
    readonly property var extraLines: stats === undefined ? [] : [
        { title: qsTr("渲染帧率:"),   value: renderFpsText() },
        { title: qsTr("丢帧/总帧:"),  value: droppedText() },
        { title: qsTr("缓冲时长:"),   value: msText("bufferMs") },
        { title: qsTr("播放位置:"),   value: positionText() },
        /*
         * 【解码方式：硬解/软解 + 编码（+ 硬解时的显卡名）】
         *
         * 取的是 stats.decodeMethod —— C++ 侧组装好的标准文案，事实来源是**活动解码器
         * 实例**（硬解还是软解，含运行期退回软解）＋ 当前流的编码 ＋ 显卡名：
         *     硬解 HEVC（NVIDIA GeForce RTX 4060） / 硬解 HEVC / 软解 H.264 / 硬解 / 软解
         * （编码在 C++ 侧经 codecDisplayName() 换过显示名：内核短名 H.265 → **HEVC**。）
         * 拿不到时是空串，上面的 text() 会显示成 "--"。
         *
         * 注意别退回 stats.decodeMode：那个是"渲染/呈现走哪条路"（零拷贝还是 CPU 回拷，
         * 取值是 CicadaTextureD3D11::backendName() 那一类**实现细节**文案），不是解码方式。
         */
        { title: qsTr("解码方式:"),   value: text("decodeMethod") },
        { title: qsTr("容器:"),       value: text("container") },
        { title: qsTr("分辨率刷新:"), value: text("hdr") }
    ]

    /* .info-data 的 max-width:470px —— 值最长排到这里就省略 */
    readonly property int valueWidth: 470
    readonly property int labelWidth: 100
    /* 一行的高度（参考的 line-height: 18px） */
    readonly property int lineHeight: 18
    /* 内容左右内缩（参考 .player-info-panel 的 padding: 15px 26px 0） */
    readonly property int contentInset: 26

    function text(key) {
        const v = stats ? stats[key] : undefined
        return (v === undefined || v === null || v === "") ? "--" : ("" + v)
    }

    /*
     * 缓存/缓冲速度那一格：核心只给一个合计吞吐（见上面两行的说明），
     * 所以值后面挂一个"（合计）"，免得看着像音频/视频各自测出来的两个数。
     */
    function speedText(key) {
        const v = text(key)

        if (v === "--")
            return v

        return v + qsTr("（合计）")
    }

    function resolutionText() {
        const w = stats ? stats.width : 0
        const h = stats ? stats.height : 0

        if (!w || !h)
            return "--"

        const fps = (stats && stats.frameRate > 0) ? ("" + stats.frameRate.toFixed(3)) : "--"
        /* 参考格式：1280 x 720@30.000 */
        return w + " x " + h + "@" + fps
    }

    function renderFpsText() {
        const v = stats ? stats.renderFps : 0
        return v > 0 ? v.toFixed(1) + " fps" : "--"
    }

    function droppedText() {
        const d = stats ? stats.droppedFrames : 0
        const t = stats ? stats.totalFrames : 0
        return (t > 0 || d > 0) ? (d + " / " + t) : "--"
    }

    function msText(key) {
        const v = stats ? stats[key] : 0

        if (!v || v <= 0)
            return "--"

        const total = Math.floor(v / 1000)
        const m = Math.floor(total / 60)
        const sec = total % 60
        return (m > 0 ? (m + qsTr(" 分 ") + sec + qsTr(" 秒")) : (sec + qsTr(" 秒")))
    }

    function positionText() {
        const p = stats ? stats.positionMs : 0
        const d = stats ? stats.durationMs : 0

        if (!d || d <= 0)
            return "--"

        const total = Math.floor(p / 1000)
        const dt = Math.floor(d / 1000)
        return total + " / " + dt + qsTr(" 秒")
    }

    /* left:10 top:10（参考）；宽度按最长一行算并留出参考的 26px 内边距 */
    x: 10
    y: 10
    width: labelWidth + 10 + valueWidth + 52
    /* 高度 = 内容 + 参考的 padding-bottom:15px（见文件头那段"布局"说明） */
    height: contentColumn.height + 15
    color: "#E6212121"
    radius: 4
    visible: opened
    z: 80
    /* 兜底：以后再加内容也不会画到面板外面去 */
    clip: true

    /*
     * 【★吃掉鼠标事件：面板上点击/右键不许穿到画面区★】
     *
     * 起因（用户实测）：在统计信息面板上点一下，视频却暂停/播放了 —— 根因和修法
     * 和 PlayerColorPanel.qml 里那段长注释完全一样：Rectangle 不消费鼠标事件，
     * 点在面板空白处会往下落到铺满播放区的 activityArea（PlayerView.qml:644），
     * 它的 onClicked 会 togglePause()、右键还会弹播放器右键菜单。
     *
     * 这层必须是**第一个子元素**（z 序最低）：✕ 按钮、曲线区上面的交互都还在它上面，
     * 空白处的点击才归它。hoverEnabled: false —— 不抢 hover，
     * 画面区那套"鼠标一动就显示控制栏"照常工作。
     */
    MouseArea {
        id: panelBackdrop

        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: false
        cursorShape: Qt.ArrowCursor
    }

    /* -----------------------------------------------------------------------
     * 一条曲线（缓存速度 / 渲染帧率 / 缓冲时长）
     *
     * 数据是 `series`（player.statsHistory），500ms 一个点、最多 60 点（30 秒窗口）。
     * 采样由 C++ 侧负责（CicadaPlayerItem：定时器在 Prepare 之后一直跑，
     * **默认就在采**，见头文件里 m_statsSampling 的说明），面板只负责画。
     * 面板打开会调一次 player.startStatsSampling()（保证定时器在跑、历史不清空，
     * 所以一打开就有最近 30 秒的完整趋势），关闭调 stopStatsSampling() 停掉定时器。
     *
     * 画法用**一段一段的细 Rectangle 旋转到位**（见下面 plot 里那段说明：
     * Canvas / Shape 都试过，用户机上都是空白，最后只用最基础的矩形几何）。
     * 全部是属性绑定，不存在"什么时候重画"的问题。
     * ----------------------------------------------------------------------- */
    component StatsChart: Item {
        id: chart

        property string title: ""
        property string unit: ""
        /* 曲线颜色：**三条都是白色**（用户指定"曲线颜色要是白色才行"）。
           三条曲线各自一行、各有标题，所以同色也不会认错。 */
        property string color: "#ffffff"
        property var series: []
        property string field: "speed"
        property int tick: 0

        /* 曲线画布高度 */
        readonly property int plotHeight: 30
        /* 标题行与曲线之间的空隙 */
        readonly property int titleGap: 2
        /*
         * 标题行高按**字体度量**取，不写死 16。
         *
         * 说明：曲线压字的**真正根因是坐标系用错**（polyline 里用了整行的 height 而不是
         * plot 的 height，见下面 polyline 的说明），跟这里没关系。这里按字体量只是为了让
         * "标题行 + 空隙 + 曲线"三段加起来正好等于组件高度 —— 12px 字的 implicitHeight
         * 在本机是 15~17px（跟字体/字重/DPI 有关），写死 16 会让组件高度和内容差 1~3px，
         * 行与行之间就会挤在一起。取 `max(16, ceil(implicitHeight))` 两边都不会出问题。
         */
        readonly property real titleHeight: Math.max(16, Math.ceil(chartTitle.implicitHeight))

        width: infoPanel.labelWidth + 10 + infoPanel.valueWidth
        height: titleHeight + titleGap + plotHeight

        readonly property real maxValue: {
            let m = 0
            for (let i = 0; i < series.length; ++i) {
                const v = Number(series[i][field])
                if (isFinite(v) && v > m)
                    m = v
            }
            return m > 0 ? m : 1
        }

        readonly property real currentValue: series.length > 0
                                             ? Number(series[series.length - 1][field]) : 0

        function formatValue(v) {
            if (!isFinite(v))
                return "--"
            if (v >= 1000)
                return (v / 1000).toFixed(2) + "k"
            if (v >= 10)
                return v.toFixed(0)
            return v.toFixed(2)
        }

        Text {
            id: chartTitle

            color: "#fff"
            font.pixelSize: 12
            text: chart.title
        }

        Text {
            anchors.left: chartTitle.right
            anchors.leftMargin: 8
            anchors.verticalCenter: chartTitle.verticalCenter
            color: "#999"
            font.pixelSize: 11
            /* 当前值 + 峰值：曲线只有形状，数值还是要有 */
            text: qsTr("当前 ") + chart.formatValue(chart.currentValue) + " " + chart.unit
                  + qsTr("（峰值 ") + chart.formatValue(chart.maxValue) + " " + chart.unit + "）"
        }

        /*
         * -----------------------------------------------------------------------
         * 曲线区（**用普通 Rectangle 画折线，不用 Canvas / Shape**）
         *
         * 【为什么最后走到这一步】这块前后换过三次画法，用户三次反馈"没有渲染"：
         *   1. Canvas + requestPaint      —— 命令式绘制，面板"从隐藏变可见"时不一定重画；
         *   2. Shape + ShapePath/PathPolyline —— 声明式了，但用户机上依然一片空白
         *      （另外 Shape **只渲染自己的 ShapePath 子项**，我当时塞在里面的两条
         *       hairline Rectangle 根本没被画出来 —— 这也是"什么都没有"的一部分原因）；
         *   3. 现在：**一段一段的细 Rectangle + rotation**。
         * 折线本来就是"若干条线段"，每段用一个 1.6px 粗的矩形旋转到位即可 ——
         * 这种画法只用最基础的矩形几何，不依赖任何绘制上下文/材质/后端，一定画得出来。
         * 段数 = 采样点数 - 1（最多 59 段 × 3 条曲线，开销可以忽略）。
         * -----------------------------------------------------------------------
         */
        Item {
            id: plot

            anchors.top: chartTitle.bottom
            /* 空隙与高度都取自上面那两个常量 —— 组件的 height 就是用它们算的，
               这里再写死数字就会和 height 对不上（"曲线压到下一行文字上"就是这么来的） */
            anchors.topMargin: chart.titleGap
            width: chart.width
            height: chart.plotHeight

            /* 上下两条淡线：直接挂在 plot 上（不能放进 Shape 里，Shape 会忽略非 ShapePath 子项） */
            Rectangle {
                anchors { left: parent.left; right: parent.right; top: parent.top }
                height: 1
                color: Qt.rgba(1, 1, 1, 0.10)
            }

            Rectangle {
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height: 1
                color: Qt.rgba(1, 1, 1, 0.10)
            }

            /* 基线：没有数据时也能看出"这块是曲线区"，而不是一片空白 */
            Rectangle {
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                anchors.bottomMargin: 2
                height: 1
                color: Qt.rgba(1, 1, 1, 0.16)
            }

            /* 折线：每段一个矩形（x/y/长/角度都由 chart.segments 算好） */
            Repeater {
                model: chart.segments

                delegate: Rectangle {
                    required property var modelData

                    x: modelData.x
                    y: modelData.y
                    width: modelData.length
                    height: 1.6
                    color: chart.color
                    antialiasing: true
                    rotation: modelData.angle
                    /* 以左上角为旋转中心：这样 (x,y) 就是线段起点，角度朝终点 */
                    transformOrigin: Item.TopLeft
                }
            }
        }

        /*
         * 折线的顶点（横轴 = 最近 30 秒窗口，纵轴 = 值 / 峰值）。
         *
         * 【声明式】这是普通属性绑定：依赖 series / plot 的尺寸，任一变化都会重算，
         * 上面的 Repeater 自然跟着重建 —— 不存在"什么时候重画"的问题。
         * 少于 2 个采样点时返回空数组（此时只剩基线和两条淡线）。
         *
         * 【★必须用 plot.width / plot.height，绝不能用 width / height★】
         * 这是"曲线压到文字上、底部还和下面那行字齐平"的**真正根因**（RC-AM）：
         * 线段是画在 `plot` 这个 30px 高的框里的，而这里原来写的是组件自己的
         * `width` / `height` —— 那是**整行**的尺寸（标题 + 空隙 + 曲线 ≈ 54px）。
         * 于是曲线按 54px 的高度算坐标，却落在 30px 的框里：底部那一截（约 24px）
         * 整条溢出到下一行的标题文字上，而它的基线正好压在下一行文字的底边 ——
         * 用户看到的"渲染帧率/缓冲时长/60 个采样点 这些文字和曲线重叠、底部齐平"就是这个。
         * （前一版把组件高度从写死的 16+30 改成按字体量，只是让溢出量从 16 变成 24，
         *   越改越糟 —— 因为坐标系根本就不对，跟行高没关系。）
         */
        readonly property var polyline: {
            const pts = []
            const n = series.length
            /* 画布的**实际**尺寸（不是整行的尺寸）；plot 还没布局出来时先不画 */
            const plotW = plot.width
            const plotH = plot.height

            if (n < 2 || plotW <= 0 || plotH <= 0)
                return pts

            const maxV = maxValue > 0 ? maxValue : 1
            const stepX = plotW / (n - 1)
            /* 上下各留 4px：值 0 → 贴基线(y = plotH-4)，值 = 峰值 → 顶到 y = 4 */
            const usableH = plotH - 8

            for (let i = 0; i < n; ++i) {
                const v = Math.max(0, Number(series[i][field]) || 0)
                pts.push(Qt.point(i * stepX, plotH - 4 - (v / maxV) * usableH))
            }

            return pts
        }

        /* 把顶点两两连成线段（每段的起点/长度/角度），交给 Repeater 画 */
        readonly property var segments: {
            const pts = polyline
            const segs = []

            for (let i = 0; i + 1 < pts.length; ++i) {
                const dx = pts[i + 1].x - pts[i].x
                const dy = pts[i + 1].y - pts[i].y
                segs.push({
                    x: pts[i].x,
                    y: pts[i].y,
                    length: Math.sqrt(dx * dx + dy * dy),
                    angle: Math.atan2(dy, dx) * 180 / Math.PI
                })
            }

            return segs
        }
    }

    /* -----------------------------------------------------------------------
     * 内容（一棵树：标题 → 明细 → 分隔线 → 补充项 → 曲线 → 采样提示）
     * ----------------------------------------------------------------------- */
    Column {
        id: contentColumn

        width: parent.width

        /* 标题：40px 高、居中、底部 1px 分隔线（参考 .player-info-title） */
        Item {
            id: titleItem

            width: contentColumn.width
            height: 40

            Text {
                anchors.centerIn: parent
                color: "#fff"
                font.pixelSize: 16
                text: qsTr("统计信息")
            }

            Rectangle {
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height: 1
                color: Qt.rgba(1, 1, 1, 0.1)
            }

            /* 关闭按钮：22x22，right:10 top:10（两条 1.5px 线画成 ✕，和参考的 SVG 同形） */
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
                TapHandler { onTapped: infoPanel.closeRequested() }
            }
        }

        /* .player-info-panel 的 padding-top:15px */
        Item { width: 1; height: 15 }

        Repeater {
            model: infoPanel.lines

            delegate: Item {
                id: lineItem

                required property var modelData

                /* 宽度 = 面板宽（Column 只管 Y，不碰 X）；内容再用 x 内缩 26（参考的 padding-left） */
                width: contentColumn.width
                height: infoPanel.lineHeight

                Text {
                    x: infoPanel.contentInset
                    width: infoPanel.labelWidth
                    horizontalAlignment: Text.AlignRight
                    color: "#fff"
                    font.pixelSize: 12
                    font.weight: Font.Medium
                    text: lineItem.modelData.title
                }

                Text {
                    x: infoPanel.contentInset + infoPanel.labelWidth + 10
                    width: infoPanel.valueWidth
                    color: "#999"
                    font.pixelSize: 12
                    elide: Text.ElideRight
                    text: lineItem.modelData.value
                }
            }
        }

        /* 参考那 9 行之下的一条分隔线（下面那几行是"核心能真实拿到、排查最有用"的补充项） */
        Item {
            width: contentColumn.width
            height: 19

            Rectangle {
                x: infoPanel.contentInset
                width: infoPanel.labelWidth + 10 + infoPanel.valueWidth
                anchors.verticalCenter: parent.verticalCenter
                height: 1
                color: Qt.rgba(1, 1, 1, 0.1)
            }
        }

        Repeater {
            model: infoPanel.extraLines

            delegate: Item {
                id: extraItem

                required property var modelData

                width: contentColumn.width
                height: infoPanel.lineHeight

                Text {
                    x: infoPanel.contentInset
                    width: infoPanel.labelWidth
                    horizontalAlignment: Text.AlignRight
                    color: "#fff"
                    font.pixelSize: 12
                    font.weight: Font.Medium
                    text: extraItem.modelData.title
                }

                Text {
                    x: infoPanel.contentInset + infoPanel.labelWidth + 10
                    width: infoPanel.valueWidth
                    color: "#999"
                    font.pixelSize: 12
                    elide: Text.ElideRight
                    text: extraItem.modelData.value
                }
            }
        }

        /* 曲线区与上面留一点空隙 + 一条分隔线（视觉上成组） */
        Item {
            width: contentColumn.width
            height: 15

            Rectangle {
                x: infoPanel.contentInset
                width: infoPanel.labelWidth + 10 + infoPanel.valueWidth
                anchors.bottom: parent.bottom
                height: 1
                color: Qt.rgba(1, 1, 1, 0.1)
            }
        }

        /* 三条曲线：各自一行，左边距和上面那些行对齐 */
        Item {
            width: contentColumn.width
            height: chartsInner.height

            Column {
                id: chartsInner

                x: infoPanel.contentInset
                width: infoPanel.labelWidth + 10 + infoPanel.valueWidth

                StatsChart {
                    title: qsTr("缓存速度")
                    unit: "B/s"
                    /* 三条曲线都是白色（用户指定）；组件默认值就是 #ffffff，这里显式写出来 */
                    color: "#ffffff"
                    field: "speed"
                    series: infoPanel.history
                    tick: infoPanel.repaintTick
                }

                StatsChart {
                    title: qsTr("渲染帧率")
                    unit: "fps"
                    color: "#ffffff"
                    field: "fps"
                    series: infoPanel.history
                    tick: infoPanel.repaintTick
                }

                StatsChart {
                    title: qsTr("缓冲时长")
                    unit: "s"
                    color: "#ffffff"
                    field: "buffer"
                    series: infoPanel.history
                    tick: infoPanel.repaintTick
                }
            }
        }

        /* 没在采样/还没数据时给一句话，别留一块空白（这行也算进面板高度，不会再溢出） */
        Item {
            width: contentColumn.width
            height: sampleHint.implicitHeight + 8

            Text {
                id: sampleHint

                x: infoPanel.contentInset
                width: infoPanel.labelWidth + 10 + infoPanel.valueWidth
                color: infoPanel.history.length >= 2 ? "#7ed321" : "#e0c060"
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                /*
                 * 【这行既是提示也是自检】把"采到几个点、最后一个点是什么值"直接写在界面上：
                 * 曲线画不出来时，看一眼这行就能区分是"没采到数据"（点数一直是 0）
                 * 还是"采到了但没画出来"（点数在涨、曲线却没有）。
                 *
                 * 数据没通的时候（没在采样 / 点数不足 / 播放器引用没拿到），前面再挂一段
                 * `［自检 sampling=… 点数=… player=… stats=…］`，用来一刀切开几种故障：
                 *   stats=0               → 连 stats 都到不了 QML（属性绑定/元对象这一层坏了）
                 *   sampling=0            → QML 这一层没调到 startStatsSampling()（开关没开）
                 *   sampling=1 点数不动   → 开关开了，是 statsHistory 没送到 QML（属性&notify）
                 *   player=0              → PlayerView 没把播放器组件传进来
                 * 正常出曲线时这段自检不显示。
                 */
                text: {
                    const broken = !infoPanel.sampling || infoPanel.history.length < 2
                                   || infoPanel.playerItem === null

                    /* stats 里到底有没有键（stats=0 就说明整个属性通道都是死的） */
                    const statsKeys = infoPanel.stats ? Object.keys(infoPanel.stats).length : 0

                    const selfCheck = broken
                            ? "［自检 sampling=" + (infoPanel.sampling ? 1 : 0)
                              + " 点数=" + infoPanel.history.length
                              + " player=" + (infoPanel.playerItem !== null ? 1 : 0)
                              + " stats=" + statsKeys + "］"
                            : ""

                    if (!infoPanel.sampling)
                        return selfCheck + qsTr("曲线采样已停止（关闭面板即停止监听，节省资源）")

                    if (infoPanel.history.length < 2)
                        return selfCheck + qsTr("正在采样…（每 0.5 秒一个点，30 秒窗口）")

                    return selfCheck + qsTr("%1 个采样点｜速度 %2｜帧率 %3｜缓冲 %4 秒")
                            .arg(infoPanel.history.length)
                            .arg(infoPanel.lastText("speed", "B/s"))
                            .arg(infoPanel.lastText("fps", "fps"))
                            .arg(infoPanel.lastText("buffer", "s"))
                }
            }
        }
    }

    /* 最后一个采样点的某个字段（带单位），给上面那行自检文字用 */
    function lastText(field, unit) {
        if (!history || history.length === 0)
            return "--"

        const v = Number(history[history.length - 1][field])

        if (!isFinite(v))
            return "--"

        if (v >= 1000)
            return (v / 1000).toFixed(2) + "k " + unit

        return (v >= 10 ? v.toFixed(0) : v.toFixed(2)) + " " + unit
    }
}
