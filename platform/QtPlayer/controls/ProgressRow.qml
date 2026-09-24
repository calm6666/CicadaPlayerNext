// ===========================================================================
//
// 拆出来的原因：控制栏那个文件已经三千多行。进度条（轨道/圆点/拖动指示/悬停预览）自成一块，
// 和按钮、弹幕发送框互不依赖，混在一起只会让改动互相干扰。
//
// 【为什么外面还要包一层 root】shadowProgress（拖动时的"影子进度"）原来是 progressRow 的
// **兄弟节点**、锚在它身上。搬出来时若塞进 progressRow 内部就变成自引用，所以这一层 root
// 保持原来的父子关系；尺寸由 ControlBar 锚成和原来一样（铺满控制栏），内部几何不变。
//
// 对外：bar（控制栏本体，取主题数值和 formatTime）、player（播放器）。
//
// ---------------------------------------------------------------------------
// 本轮新增的两块（都在这一个文件里，参考实现 front/player 的对应关系写在各自代码旁）：
//   1. 分段进度条：有视点数据（viewPoints）时，每个视点一条独立横条 + 悬停气泡里的段标题；
//   2. 高能进度条（pbp）：进度条上方那条 28px 的曲线区，数据来自 pbpData。
// 两者的属性初值分别是空数组 / null，**没有数据时这一屏与加功能之前完全一致**
// （普通轨道照旧、pbp 区域 opacity 0 + enabled 假），这是本轮最硬的一条约束。
// ---------------------------------------------------------------------------
// ===========================================================================
import QtQuick
import QtQuick.Templates as T
/* 发送框的输入框用 Qt 自带控件（Basic 样式）；MultiEffect 用来给 SVG 着色。
   拆出来时必须一起带过来，少了就是致命错误：本文件加载失败 → 模块里没有 Main → 程序退出。 */
import QtQuick.Controls.Basic
import QtQuick.Effects
/* 相对目录导入：让编辑器（qmlls）按文件系统直接找到根目录的 LottieIcon（说明见 RightControls.qml）。 */
import ".."
import QtPlayer

Item {
    id: progressRowRoot

    /* 控制栏本体：原来这些代码就在 PlayerControlBar 里、直接写 bar.xxx；现在用属性传进来，
       所有 bar.xxx 的写法一个字都不用改。 */
    property var bar: null
    property var player: null

    /* -----------------------------------------------------------------------
     * 视点（分段进度条）与高能进度条（pbp）的数据入口
     *
     * 【默认值就是"没有数据"】两个属性的初值分别是空数组和 null，此时：
     *   * 分段那条轨道整块不可见（visible: hasSegments），进度条本体照旧（整条底 + 缓冲 + 已播）；
     *   * pbp 那条区域 opacity 恒为 0 且 enabled 为假（见下面 pbpArea 的 active）。
     * 所以"没接数据"时这一屏和加这两个功能之前**一个像素都不差**。
     *
     * 【pbp 与进度条完全解耦】pbp 的 hasData / active / highlightEnabled / pbpPinned
     * 只出现在 pbpArea 自己的 opacity / visible / enabled / 几何里；下面那条进度条（本体 + 分段）
     * 的 visible / opacity / enabled / z 只看 viewPoints（hasSegments）和控制栏显隐 —— 自查过。
     * ----------------------------------------------------------------------- */

    /* 视点：每项 {from, to, content}，from/to 单位是**秒**（和参考 controls/index.ts 一致） */
    property var viewPoints: []
    /* 高能进度条数据：{step_sec, data: [0~1, …]}，step_sec 是横向步长（秒） */
    property var pbpData: null
    /*
     * 悬停气泡的**位置帧**：接口 videoshot/preview.bin 按 \u001F 切出来的数组
     * （arr[0] 是空占位，arr[k]（k≥1）= 第 5*(k-1) 秒那一帧的 data URL）。
     * 空数组 = 接口没给 → 气泡退回 CaptureScreen 的"当前画面"。
     */
    property var previewFrames: []

    /* 高能进度条开关（默认**开**）：从控制栏一路传进来，pbpArea.active 直接读它 ——
       关掉之后曲线和 pin 都不显示、也不响应鼠标。 */
    property bool highlightEnabled: true

    /*
     * 高能进度条"常驻"：点右边那颗 pin 切换，默认 **false**。
     *
     * 常驻 = 控制栏自动收起之后，曲线和 pin 仍然留在底部进度条上方
     */
    property bool pbpPinned: false

    /*
     * pbpData 是不是"真的有东西可画"。没有数据就整块不显示 —— 这条是"默认界面不变"的保证：
     * 光有 highlightEnabled = true（默认值）不会让画布上多出任何东西。
     */
    readonly property bool hasPbpData: pbpData !== undefined && pbpData !== null
                                       && pbpData.data !== undefined && pbpData.data !== null
                                       && pbpData.data.length > 0
                                       && Number(pbpData.step_sec) > 0

    /* 外面要用的东西原样透出（左边那组的时间要读 displayPositionMs） */
    readonly property alias displayPositionMs: progressRow.displayPositionMs
    readonly property alias containsMouse: progressArea.containsMouse
    /*
     * pbp 曲线那条区域自己的 hover 判据（没有数据时恒为 false）。
     */
    readonly property alias curveContainsMouse: pbpMouse.containsMouse


    /* =======================================================================
     * "停靠"态：常驻（pbpPinned）+ 控制栏收起 —— **只对 pbp 生效**
     *
     * ======================================================================= */
    readonly property bool docked: pbpPinned && bar !== null && !bar.shown

    Item {
        id: progressRow
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
            /* 参考 .player-control-top 的 bottom:44px —— 相对**播放区底边**量 44px */
            bottomMargin: bar.progressBottom
            leftMargin: QtPlayerTheme.progressRowPaddingH
            rightMargin: QtPlayerTheme.progressRowPaddingH
        }
        height: QtPlayerTheme.progressHeight + QtPlayerTheme.progressWrapPaddingBottom

        /* 进度条那一行自己的淡入淡出。
           【只看控制栏，跟 pbp 的常驻无关】用户否掉了"常驻时这一行也停靠到底部"的做法。 */
        opacity: bar.shown ? 1 : 0
        visible: opacity > 0

        Behavior on opacity {
            NumberAnimation {
                duration: 200
                easing.type: Easing.OutQuad
            }
        }

        property bool hovered: false
        property bool dragging: false
        property real dragRatio: 0
        /* 鼠标悬停位置（-1 = 不在上面） */
        property real hoverRatio: -1
        /* 松手之后、框架到位之前先停在这个位置（见上面第 5 条） */
        property real pendingSeekMs: -1

        readonly property real displayPositionMs: dragging
                                                 ? dragRatio * (bar.player ? bar.player.duration : 0)
                                                 : (pendingSeekMs >= 0 ? pendingSeekMs
                                                                       : (bar.player ? bar.player.position : 0))
        readonly property real shownRatio: bar.player ? bar.ratioOf(displayPositionMs) : 0
        readonly property real bufferRatio: bar.player ? bar.ratioOf(bar.player.bufferedPosition) : 0
        /* 三角跟"正在操作的位置" */
        readonly property real indicatorRatio: dragging ? dragRatio
                                                : (hoverRatio >= 0 ? hoverRatio : shownRatio)

        /* ===================================================================
         * 分段进度条（视点）的计算
         *
         * 视点的 from/to 是**秒**，而播放器的 position/duration/bufferedPosition 是**毫秒**：
         * 分段归一化（参考 calculateSegmentScale，controls/index.ts:1673-1677）必须在同一单位下算，
         * 所以这里统一换成秒，下面各处不再零散地除 1000。
         * =================================================================== */

        readonly property real durationSec: bar.player ? bar.player.duration / 1000 : 0
        /* 当前显示位置（拖动/待到位那套逻辑都在 displayPositionMs 里，这里跟它走） */
        readonly property real playedSec: displayPositionMs / 1000
        readonly property real bufferedSec: bar.player ? bar.player.bufferedPosition / 1000 : 0

        /*
         * 有没有视点数据。有 → 每个视点一条独立横条（segmentTrack）；没有 → 下面那条普通进度条
         * 原样工作。durationSec > 0 是必须的：下面所有几何都是"占时长的百分比"，时长为 0 时
         * （直播/还没拿到时长）除出来是 NaN。
         */
        readonly property bool hasSegments: viewPoints !== undefined && viewPoints !== null
                                            && viewPoints.length >= 1 && durationSec > 0

        /*
         * 鼠标正压在哪一段上（-1 = 不在任何一段上）。
         */
        readonly property int hoveredSegmentIndex: hasSegments && hoverRatio >= 0
                                                   ? segmentIndexAt(hoverRatio) : -1

        /* 悬停气泡里要显示的"当前段标题"（空串 = 不显示，见下面 hotspot 那段） */
        readonly property string hoverSegmentTitle: hasSegments && hoverRatio >= 0
                                                    ? segmentTitleAt(hoverRatio) : ""

        /*
         * 视点 index 的**原始**时间区间 [from, to]（秒，各自 clamp 到 0..durationSec）。，
         * "内缩"只发生在算填充比例的时候（calculateTimeRange，:1644-1664）。
         */
        function segmentRawRange(index) {
            var point = viewPoints[index]

            if (!point)
                return [0, 0]

            var from = Number(point.from)
            var to = Number(point.to)

            if (!isFinite(from))
                from = 0

            if (!isFinite(to))
                to = from

            from = Math.max(0, Math.min(durationSec, from))
            to = Math.max(from, Math.min(durationSec, to))
            return [from, to]
        }

        /*
         * 每段的横条几何，单位是"比例"（相对整条轨道宽度）—— 照抄参考的三条策略
         * 真正看得见的空隙来自"本段右端 -0.15% + 下一段左端 +0.15%" = 0.3%，
         * 也就是这里的 inset/gap 两个 token 算出来的结果 —— 再扣一次 margin 就会比参考窄 0.3%。
         */
        function segmentGeometry(index) {
            if (!hasSegments)
                return {
                    "left": 0,
                    "width": 0
                }

            var range = segmentRawRange(index)
            var count = viewPoints.length
            var spanPercent = (range[1] - range[0]) / durationSec * 100
            var inset = QtPlayerTheme.progressSegmentInset
            var left = 0
            var width = 0

            if (index === 0) {
                left = 0
                width = spanPercent - inset
            } else if (index === count - 1) {
                left = range[0] / durationSec * 100 + inset
                width = spanPercent - inset
            } else {
                left = range[0] / durationSec * 100 + inset
                width = spanPercent - QtPlayerTheme.progressSegmentGap
            }

            return {
                "left": Math.max(0, left) / 100,
                "width": Math.max(0, width) / 100
            }
        }

        /*
         * 蓝色填充/缓冲用的区间（秒）：
         * adjustment = duration * 0.0015，首段 end -= adj、末段 start += adj 且 end = duration、
         * 中间段两端各 ±adj。这样每段的 0..1 是"段内归一化"的，播放位置落在别的段里时
         * 本段该是满的就是满的，不会被相邻段的时间误差带偏。
         * 走的是另一条路（一个视点根本不建分段，就是普通进度条，见 :520-531 / :558-564）；
         * 本需求要求 length >= 1 就按分段画，所以单点那段也按"首段"内缩（索引 0 先命中 first，
         * 见上面 segmentGeometry 的说明），保持"段内归一化"这条不变量 —— 否则单点只覆盖
         * 局部区间时（比如视点只标了 100s~200s），填充会溢出条外、只能一直显示满格。
         */
        function segmentFillRange(index) {
            var range = segmentRawRange(index)
            var count = viewPoints.length
            var adjustment = durationSec * QtPlayerTheme.progressSegmentAdjustRatio

            if (index === 0) {
                range[1] -= adjustment
            } else if (index === count - 1) {
                range[0] += adjustment
                range[1] = durationSec
            } else {
                range[0] += adjustment
                range[1] -= adjustment
            }

            return range
        }

        /*
         * 段内比例（0..1）—— 参考 calculateSegmentScale（controls/index.ts:1673-1677）：
         * 值 <= start 记 0、>= end 记 1，中间线性；外面 applyTransform 再 clamp 一次（:1692）。
         */
        function segmentRatio(index, valueSec) {
            var range = segmentFillRange(index)

            if (valueSec <= range[0])
                return 0

            if (valueSec >= range[1])
                return 1

            return Math.max(0, Math.min(1, (valueSec - range[0]) / (range[1] - range[0])))
        }

        /* 鼠标比例位置落在哪一段的横条上（-1 = 都不在，用于 scaleY(1.8)） */
        function segmentIndexAt(ratio) {
            for (var i = 0; i < viewPoints.length; ++i) {
                var geometry = segmentGeometry(i)

                if (ratio >= geometry.left && ratio <= geometry.left + geometry.width)
                    return i
            }

            return -1
        }

        /*
         * 悬停位置所在段的标题
         * 用的是 vp 的原始区间（参考那里也是直接读 item.startTime/endTime，不走内缩）。
         * 注意端点：最右端那条（t 正好等于 duration）不属于任何一段 —— 和参考一致。
         */
        function segmentTitleAt(ratio) {
            var timeSec = ratio * durationSec

            for (var i = 0; i < viewPoints.length; ++i) {
                var range = segmentRawRange(i)

                if (range[0] <= timeSec && timeSec < range[1]) {
                    var point = viewPoints[i]
                    return (point && point.content !== undefined && point.content !== null)
                            ? String(point.content) : ""
                }
            }

            return ""
        }

        Connections {
            target: bar.player

            function onPositionChanged() {
                if (progressRow.pendingSeekMs >= 0
                        && Math.abs(bar.player.position - progressRow.pendingSeekMs) < 1000)
                    progressRow.pendingSeekMs = -1
            }
        }

        /* 兜底：框架一直没报到位（直播/切片）时也要放开 */
        Timer {
            running: progressRow.pendingSeekMs >= 0
            interval: 1500
            repeat: false
            onTriggered: progressRow.pendingSeekMs = -1
        }

        /* ---- 缩略图请求（悬停预览）的节流 ----
         *
         * 每请求一次就是**截一帧**：硬解时要在渲染器里回读一帧 GPU 表面（4K 十几毫秒），
         * 而 onPositionChanged 一秒能来几十次 —— 不节流会把播放器拖住。
         *
         * 用的是"前缘节流 + 尾随补一次"：
         *   * 第一次立刻发（悬停进去马上有图，不用等 150ms）；
         *   * 之后 150ms 内最多一次；
         *   * 这 150ms 里如果还有请求（鼠标一直在动、或一直按着拖），冷却结束再补发最后
         *     一次 —— 保证"停下来/松开手"的位置一定拿到图，而不是只有窗口里那一次。
         *
         * 【它只管"请求截图"这一件事】不参与显隐、也不负责清图 ——
         * 预览图上一次显示什么，在下一张**加载好**之前一直不动（见 preview 里那段）。
         */
        property bool snapshotPending: false

        /*
         * 已经为"每 5 秒一帧"的哪一档请求过截图（-1 = 还没请求 / 已经离开进度条）。
         * 同一档里鼠标怎么动都不重新取图，**只有跨档才换**。
         */
        property int requestedFrameIndex: -1

        /* 悬停位置落在第几档（floor(秒 / 5)，见 progressPreviewFrameSec）；-1 = 不在进度条上 */
        readonly property int hoverFrameIndex: {
            if (hoverRatio < 0 || !bar.player || bar.player.duration <= 0)
                return -1

            var sec = hoverRatio * bar.player.duration / 1000
            return Math.floor(sec / QtPlayerTheme.progressPreviewFrameSec)
        }

        /* 档位一变就请求一张（节流器保证 150ms 内最多真的截一次） */
        onHoverFrameIndexChanged: requestSnapshotForFrame()

        function requestSnapshotForFrame() {
            if (hoverFrameIndex < 0) {
                requestedFrameIndex = -1
                return
            }

            if (hoverFrameIndex === requestedFrameIndex)
                return

            requestedFrameIndex = hoverFrameIndex
            requestSnapshotThrottled()
        }

        function requestSnapshotThrottled() {
            /* 【有接口帧就别再截屏】位置帧由 videoshot/preview.bin 提供（onlineSource 非空就是
               这一档有接口帧）；CaptureScreen 给的是"此刻正在显示"的那一帧，只在接口整条不可用
               时才当兜底（判断放在最前面：连 150ms 节流都不用起）。 */
            if (preview.onlineSource !== "")
                return

            /* 没播放器（还没 Prepare）或还没就绪时框架只会回调"没有画面"，白截。 */
            if (!bar.player || !bar.ready)
                return

            if (snapshotCooldown.running) {
                progressRow.snapshotPending = true
                return
            }

            bar.player.requestSnapshot()
            snapshotCooldown.restart()
        }

        Timer {
            id: snapshotCooldown
            interval: 150
            repeat: false
            onTriggered: {
                if (progressRow.snapshotPending) {
                    progressRow.snapshotPending = false
                    progressRow.requestSnapshotThrottled()
                }
            }
        }

        /* ---- 轨道：底色 + 缓冲 + 已播放；悬停时 scaleY(1.5) ---- */
        Item {
            id: track

            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
            }
            height: QtPlayerTheme.progressHeight
            /*
             * 【有视点时本体让位 —— 这是参考的语义，别再改】
             * 这行判据里**不允许**出现 hasData / active / highlightEnabled / pbpPinned ——
             * 高能进度条（pbp）载入、开关、常驻都**不许**动下面任何一条进度条的
             * visible / opacity / enabled / z。pbp 只是叠在它们上方的一条独立区域
             * （几何不重叠、z: -1 在它们后面）。
             */
            visible: !progressRow.hasSegments
            /*
             * 悬停时把轨道**只纵向**拉高 1.5 倍 。
             * 注意不能用 Item.scale：那是横纵等比放大，会让进度条变长变粗（上一版就是这么错的）。
             */
            transform: Scale {
                origin.x: track.width / 2
                origin.y: track.height / 2
                yScale: progressRow.hovered || progressRow.dragging ? QtPlayerTheme.progressHoverScale : 1

                Behavior on yScale {
                    NumberAnimation {
                        duration: 100
                        easing.type: Easing.OutCubic
                    }
                }
            }

            /* 底色 hsla(0,0%,100%,.2)（分段态由每段自己那条画，见 segmentTrack） */
            Rectangle {
                anchors.fill: parent
                radius: QtPlayerTheme.progressRadius
                color: QtPlayerTheme.progressTrackBg
            }

            /* 缓冲 hsla(0,0%,100%,.3)、已播 #00a1d6、圆角 1.5 —— **恢复成原来那套**，
               不要在这里加任何 visible 条件（有视点时整条 track 一起让位，见上面的注释） */
            Rectangle {
                anchors {
                    left: parent.left
                    top: parent.top
                    bottom: parent.bottom
                }
                width: parent.width * Math.max(0, Math.min(1, progressRow.bufferRatio))
                radius: QtPlayerTheme.progressRadius
                color: QtPlayerTheme.progressBufferBg
            }

            Rectangle {
                id: trackPlayed

                anchors {
                    left: parent.left
                    top: parent.top
                    bottom: parent.bottom
                }
                width: parent.width * Math.max(0, Math.min(1, progressRow.shownRatio))
                radius: QtPlayerTheme.progressRadius
                color: QtPlayerTheme.progressPlayedBg
            }
        }

        /* ---- 分段进度条：每个视点一条独立的进度条（有视点数据时才存在） ----
         * 所以每条段自己带底色 + 缓冲 + 已播，段间 0.3% 的空隙（三条策略里的 inset/gap）就是
         * 参考那套"分段留缝"的机制。**没有"本体 + 分段同时可见"这回事**（那是被否掉的自创做法）。
         *
         * 这里用 Repeater + 一条不动的模型列表：
         *   * viewPoints 空 → 一个 delegate 都没有，整块 visible 为假，本体照旧（界面和以前一模一样）；
         *   * 每段的几何/填充都在 delegate 里按 index 现算（progressRow.segmentGeometry /
         *     segmentRatio），拖动、缓冲、seek 全都会自动重算 —— 不需要任何"手动刷新"。
         */
        Item {
            id: segmentTrack

            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
            }
            height: QtPlayerTheme.progressHeight
            visible: progressRow.hasSegments

            Repeater {
                id: segmentRepeater

                /*
                 * 没数据（或时长还没拿到）时喂空列表：delegate 一个都不建，避免除零算出 NaN。
                 *
                 * 【限定访问只能写 progressRowRoot.viewPoints —— 这里错过一次，代价是"整条进度条不见了"】
                 * 限定访问 `obj.prop` 只认**那个对象自己声明的**属性（含继承来的），**不走作用域链**。
                 * viewPoints 声明在**根对象**上（progressRowRoot，见本文件 52-53 行），内层这个
                 * progressRow 上并没有它 —— 所以原来那句 `progressRow.viewPoints` 求值是
                 * **undefined**，Repeater 一个 delegate 都不建。
                 *
                 * 而 hasSegments 用的是**不带限定**的名字，走作用域链拿到的是根对象那份真数据 →
                 * 为真 → 于是同一时刻：track 让位（visible: !hasSegments）+ 分段一个都没有
                 * = **屏幕上一条进度条都没有**。dot / 上下三角 / 预览气泡照常显示，因为它们
                 * 是普通 anchors 摆的、不经过 Repeater（用户实测反馈正是这一条：
                 * "鼠标移上去 dot 和预览缩略图都正常，就是那条进度条看不见"）。
                 * 又因为 pbp 与 view_points 是同一次取数一起到的，现象看起来像"pbp 一加载进度条就没了"。
                 *
                 * 修法：带限定指到声明处（progressRowRoot）。不加限定的 `viewPoints` 也能拿到根对象那份
                 * （同一作用域链），但根对象上的名字一旦被内层同名属性遮住就会再次踩坑 —— 带限定更稳。
                 */
                model: progressRow.hasSegments ? progressRowRoot.viewPoints : []

                delegate: Item {
                    id: segment

                    /* 只用到索引（视点对象由 progressRow 里的几个函数按索引现取） */
                    required property int index

                    /* 本段横条的位置/宽度（像素）：几何比例 × 轨道宽度 —— 比例由 segmentGeometry 现算，
                       所以视点数据、时长一变就自动重排，不需要任何"手动刷新" */
                    readonly property real segmentLeft: progressRow.width * progressRow.segmentGeometry(index).left
                    readonly property real segmentWidth: Math.max(0, progressRow.width * progressRow.segmentGeometry(index).width)
                    /* 鼠标正压在这一段上：参考的 .hover（index.scss:1569-1572 → scaleY(1.8)） */
                    readonly property bool segmentHovered: progressRow.hoveredSegmentIndex === index

                    x: segmentLeft
                    width: segmentWidth
                    height: QtPlayerTheme.progressHeight

                    /*
                     * 悬停放大只纵向拉（和普通轨道那条一样，不能用 Item.scale —— 那是横纵等比）。
                     * 参考里这两个是**叠乘**的（外层 wrap 1.5 × 本段 1.8 = 2.7）；
                     * 本需求把两档分开用：压在某段上时该段 = progressSegmentHoverScale（2.4，比非分段
                     * 那档 1.5 明显大，取值理由见 QtPlayerTheme 里那条），只是扫过时整体 1.5，不叠乘。
                     */
                    transform: Scale {
                        /* 【原点在中线 —— 和普通轨道那条同理】参考的 scaleY(1.8) 用的也是 CSS 默认的
                           transform-origin: 50% 50%，即上下对称放大；QML 必须显式写
                           origin = (宽/2, 高/2)，不写默认 (0,0) 就变成"只往下长"。 */
                        origin.x: segment.width / 2
                        origin.y: segment.height / 2
                        yScale: segment.segmentHovered ? QtPlayerTheme.progressSegmentHoverScale
                                                       : (progressRow.hovered || progressRow.dragging
                                                          ? QtPlayerTheme.progressHoverScale : 1)

                        /* transition: transform 0.1s cubic-bezier(0,0,.2,1)（index.scss:1562-1566）
                           —— 和本文件普通轨道那条一样，用 OutCubic 近似这条 CSS 贝塞尔（项目一贯做法） */
                        Behavior on yScale {
                            NumberAnimation {
                                duration: QtPlayerTheme.progressSegmentTransitionMs
                                easing.type: Easing.OutCubic
                            }
                        }
                    }

                    /* 底色 hsla(0,0%,100%,.2)：和本体同一个 token，圆角也同本体 1.5
                       （progressSegmentRadius —— 用户要求覆盖参考的 0，见 QtPlayerTheme 那条注释）。
                       这一段就是"本段那条进度条"的轨道底色 —— 有视点时本体整条让位，
                       整条进度条就是由这些段拼出来的（段间 0.3% 空隙是参考的机制）。 */
                    Rectangle {
                        anchors.fill: parent
                        radius: QtPlayerTheme.progressSegmentRadius
                        color: QtPlayerTheme.progressTrackBg
                    }

                    /* 缓冲：段内归一*/
                    Rectangle {
                        anchors {
                            left: parent.left
                            top: parent.top
                            bottom: parent.bottom
                        }
                        width: parent.width * progressRow.segmentRatio(index, progressRow.bufferedSec)
                        radius: QtPlayerTheme.progressSegmentRadius
                        color: QtPlayerTheme.progressBufferBg
                    }

                    /* 已播：同一套段内归一化 */
                    Rectangle {
                        anchors {
                            left: parent.left
                            top: parent.top
                            bottom: parent.bottom
                        }
                        width: parent.width * progressRow.segmentRatio(index, progressRow.playedSec)
                        radius: QtPlayerTheme.progressSegmentRadius
                        color: QtPlayerTheme.progressPlayedBg
                    }
                }
            }
        }

        /* ---- 圆点：20x20 的容器 + 18x18 的 Thumb 动画（参考的素材） ---- */
        Item {
            id: thumb

            width: QtPlayerTheme.progressThumbSize
            height: QtPlayerTheme.progressThumbSize
            x: Math.max(0, Math.min(progressRow.width - width,
                                    progressRow.width * progressRow.shownRatio - width / 2))
            /*
             * 圆点要对齐**轨道中心**，不是对齐整行中心：   
             * 里的居中项（controls.scss:154-182），所以它的中心 = 轨道中心 = 行顶上 2px。
             * 整行是 4px 轨道 + 6px padding-bottom，按行居中会低 3px（"dot 太偏下"）。
             */
            y: (QtPlayerTheme.progressHeight - height) / 2
            scale: progressRow.hovered || progressRow.dragging ? 1 : 0

            Behavior on scale {
                NumberAnimation {
                    duration: 200
                }
            }

            LottieIcon {
                id: thumbIcon

                anchors.centerIn: parent
                width: 18
                height: 18
                canvasWidth: 18
                canvasHeight: 18
                sequence: [
                    {
                        "source": "assets/lottie-icon/Thumb-animation.json",
                        "autoplay": false
                    }
                ]
            }

            onScaleChanged: {
                if (scale > 0.5)
                    thumbIcon.play()
                else
                    thumbIcon.stop()
            }
        }

        /* ---- 上下三角（悬停就出现，跟鼠标/手指） ---- */
        Canvas {
            id: moveIndicator

            width: QtPlayerTheme.moveIndicatorWidth
            height: QtPlayerTheme.moveIndicatorHeight
            x: Math.max(0, Math.min(progressRow.width - width,
                                    progressRow.width * progressRow.indicatorRatio - width / 2))
            /*
             * 指示器是 **16px 高、以 4px 轨道为中心**摆的：
             * 顶边 = (4 - 16) / 2 = -6px（上三角占 -6~-2、下三角占 6~10，关于轨道中心对称）。
             * 之前写成 y = -4（低了 2px），就是"两个三角有点偏下"。
             */
            y: (QtPlayerTheme.progressHeight - height) / 2
            visible: progressRow.hovered || progressRow.dragging

            onPaint: {
                var ctx = getContext("2d")
                var a = QtPlayerTheme.moveIndicatorArrow
                ctx.reset()
                ctx.fillStyle = QtPlayerTheme.progressPlayedBg
                ctx.beginPath()
                ctx.moveTo(0, 0)
                ctx.lineTo(a * 2, 0)
                ctx.lineTo(a, a)
                ctx.closePath()
                ctx.fill()
                ctx.beginPath()
                ctx.moveTo(a, height - a)
                ctx.lineTo(a * 2, height)
                ctx.lineTo(0, height)
                ctx.closePath()
                ctx.fill()
            }
        }

        /* ---- 悬停预览（160x90，底部一条 18px 的时间标签） ----
         *
         * 位置帧**优先走接口**（videoshot/preview.bin，每 5 秒一档，下标 = floor(悬停秒/5)+1）；
         * 这一档接口没给时才退回框架 **CaptureScreen** 的当前画面（渲染器侧 device_captureScreen()
         * 已补：硬解零拷贝帧走 av_hwframe_transfer_data 回读，拷成 QImage 后由 QQuickImageProvider
         * 暴露成 `image://snapshot/<rev>`）。
         * 拿不到图时保持空框（参考实现没图时也是空框），绝不显示破图。
         */
        Item {
            id: preview

            /*
             * 【位置帧优先走接口】参考 controls/index.ts:1410-1417：
             *     const currTime = Math.floor(this.popup.currentTime / 5);
             *     this.previewImage.src = this.videoshot[currTime + 1];   // 下标 = floor(秒/5)+1
             * 就是"每 5 秒一档、跨档换图"，图**直接交给 Image**。
             *
             * 【为什么不加中转】曾经有一段"隐藏加载器先异步加载，Ready 之后再把它交给可见那张"
             * 的两段式中转，守卫写的是 `source === loaderSource`：第一条图之后这个等式再也追不上
             * （换 URL 的速度比加载快），于是 shownSource 停在第一张不动 —— 用户实测就是
             * "永远只有第一张"。中转整段删掉：可见那张的 source **直接绑**接口 URL。
             */
            readonly property int apiFrameIndex:
                (progressRow.hoverRatio >= 0 && progressRow.durationSec > 0)
                ? Math.floor(progressRow.hoverRatio * progressRow.durationSec
                             / QtPlayerTheme.progressPreviewFrameSec) + 1
                : -1

            /* 接口这一条整体可用吗：arr[0] 是空占位，所以"只有 1 项"等于一帧都没有 */
            readonly property bool apiFramesAvailable: previewFrames !== undefined
                                                       && previewFrames !== null
                                                       && previewFrames.length > 1

            /* 这一档的接口帧（data URL）；没有就返回空串 */
            function apiFrameSourceAt(index) {
                if (index < 0 || !apiFramesAvailable || index >= previewFrames.length)
                    return ""

                var source = "" + previewFrames[index]
                return (source === "undefined" || source === "null") ? "" : source
            }

            /* 在线帧：档位变 → URL 变 → Image 换图（纯绑定，没有任何中转和手写状态） */
            readonly property string onlineSource: apiFrameSourceAt(apiFrameIndex)

            /* 本地帧（只在接口不可用时显示）：框架 CaptureScreen 的当前画面，零拷贝回读。
               版本号拼进 URL 是因为 provider 手里永远只有"最近一张"（见 SnapshotImageProvider.h）。

               【revision 为 0 时不许拼 URL】0 = 到这一刻还没有任何快照，provider 手里没有图、
               只能返回空 QImage，而 QML 的 Image 拿到空图会把它当"取图失败"打一条
               `QML QQuickImage: Failed to get image from provider: image://snapshot/0`
               （用户日志里的那两条警告就是这么来的，ProgressRow.qml:765）。
               空串才是"这一帧先不显示"的正确表达：Image 不请求、界面是空框、也没有警告。
               悬停后 requestSnapshotForFrame() 会去截第一张，revision 一变 URL 自然就有了。 */
            readonly property string localSource: (bar.player && bar.player.snapshotRevision > 0)
                                                  ? "image://snapshot/" + bar.player.snapshotRevision
                                                  : ""

            visible: progressRow.hovered && !progressRow.dragging && progressRow.hoverRatio >= 0 && bar.ready
            width: QtPlayerTheme.progressPopupWidth
            height: QtPlayerTheme.progressPopupHeight
            x: Math.max(0, Math.min(progressRow.width - width,
                                    progressRow.width * progressRow.hoverRatio - width / 2))
            /* 参考 .player-progress-popup 的 bottom:22px 是相对 4px 轨道那条（controls.scss:234-246）。
               有段落标题时整块再抬一截：参考里标题是 popup 的**最后一个块级子元素**，而 popup 靠
               bottom 定位 —— 所以标题一出现，预览图是被顶上去的、气泡底边不动（见下面 hotspot）。 */
            y: QtPlayerTheme.progressHeight - QtPlayerTheme.progressPopupBottom - height
               - (hotspot.visible ? hotspot.height : 0)

            /*
             * 预览图（在线帧）。fillMode 用 Stretch：参考 .player-progress-preview-image 是
             * width/height:100% 且没有 object-fit，会被拉成 160x90（index.scss:699-710）。
             *
             * 【asynchronous: false 是有意的】接口帧是 data URL，本来就在内存里，而且 mock/参考
             * 的帧就是 160x90（server.js:89-90）—— 同步解码不到 1ms，换来的是"换档瞬间就换图"。
             * 异步那条路会在换 URL 时先丢掉旧图、解完再显示（中间一帧空白），当初就是为它加的
             * 加载器中转，而中转本身又会卡在 `source === loaderSource` 上永远只显示第一张。
             */
            Image {
                visible: preview.onlineSource !== ""
                anchors.fill: parent
                fillMode: Image.Stretch
                asynchronous: false
                sourceSize: Qt.size(QtPlayerTheme.progressPopupWidth,
                                    QtPlayerTheme.progressPopupHeight)
                source: preview.onlineSource
            }

            /*
             * 本地帧（接口整条不可用时的兜底）：零拷贝快照那条路，URL 跟着 revision 走。
             * cache 关掉：revision 一直在变，缓存旧的是白占内存。
             */
            Image {
                visible: !preview.apiFramesAvailable && preview.localSource !== ""
                anchors.fill: parent
                fillMode: Image.Stretch
                asynchronous: false
                cache: false
                sourceSize: Qt.size(QtPlayerTheme.progressPopupWidth,
                                    QtPlayerTheme.progressPopupHeight)
                source: preview.localSource
            }

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                height: QtPlayerTheme.progressPopupTimeHeight
                width: previewTime.implicitWidth + QtPlayerTheme.progressPopupTimePaddingH * 2
                radius: QtPlayerTheme.progressPopupRadius
                color: QtPlayerTheme.menuBg

                Text {
                    id: previewTime

                    anchors.centerIn: parent
                    color: QtPlayerTheme.menuText
                    font.pixelSize: QtPlayerTheme.progressPopupFontSize
                    text: bar.formatTime(progressRow.hoverRatio * (bar.player ? bar.player.duration : 0))
                }
            }
        }

        /* ---- 气泡里的"当前段标题"：.player-progress-hotspot（index.scss:730-746） ----
         *
         * 【为什么贴在预览图下沿】popup 是 bottom:22px 靠底摆的，标题占掉的高度会把预览图
         * 整体顶上去（上面 preview 的 y 里减了它）—— 这样气泡底边始终离轨道 22px，
         * 和参考一样；不会压到进度条上。
         *
         * 没有视点数据、或悬停位置不在任何一段里（比如最右端那一点）→ title 为空 → 不显示，
         * preview 的 y 也就不受任何影响：界面和加这个功能之前完全一样。
         *
         * 【单点视点也显示标题】参考是 `points.length > 1` 才往气泡里写标题
         * （controls/index.ts:1397-1408），本轮 length >= 1 都算分段（见上面 segmentFillRange 的说明），
         * 所以只有一个视点时也会显示它那一段的标题 —— 和"每个视点一条横条"这条规则保持一致。
         */
        Rectangle {
            id: hotspot

            readonly property string title: progressRow.hoverSegmentTitle

            x: preview.x
            y: preview.y + preview.height
            width: QtPlayerTheme.progressPopupWidth
            height: Math.min(QtPlayerTheme.progressHotspotMaxHeight,
                             hotspotText.implicitHeight + QtPlayerTheme.progressHotspotPaddingV * 2)
            visible: title !== "" && preview.visible
            /* 底色 hsla(0,0%,8%,.9)、圆角 2px：和菜单是同一个色值/圆角
               （标题的底色在 index.scss:731；菜单那两处在 controls.scss:679、746，主题里的
               menuBg/menuRadius 就是按它们定的）—— 参考里标题自己没写圆角，是被外层 popup 的
               overflow:hidden + border-radius:2px 裁出来的（index.scss:685-693）。 */
            color: QtPlayerTheme.menuBg
            radius: QtPlayerTheme.menuRadius
            /* max-height:48px + overflow:hidden：超过两行就裁掉，不做省略号（参考行为一致） */
            clip: true

            Text {
                id: hotspotText

                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    leftMargin: QtPlayerTheme.progressHotspotPaddingH
                    rightMargin: QtPlayerTheme.progressHotspotPaddingH
                    topMargin: QtPlayerTheme.progressHotspotPaddingV
                }
                color: QtPlayerTheme.menuText
                font.pixelSize: QtPlayerTheme.progressPopupFontSize
                /* line-height:18px —— QML 的 lineHeight 默认是"倍数"，要拿到正好 18px 必须换成
                   FixedHeight 模式（单位才是像素），否则字号一改行高就跟着跑 */
                lineHeight: QtPlayerTheme.progressHotspotLineHeight
                lineHeightMode: Text.FixedHeight
                /* word-break: break-all —— 长中文/长串一路折行，不缩不断 */
                wrapMode: Text.WrapAnywhere
                text: hotspot.title
            }
        }

        /* ---- 交互 ---- */
        MouseArea {
            id: progressArea

            anchors.fill: parent
            /* 热区比轨道高：.player-progress-schedule-padding height:14 */
            anchors.topMargin: -(QtPlayerTheme.progressPaddingHeight - progressRow.height)
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            acceptedButtons: Qt.LeftButton

            function ratioAt(px) {
                return width > 0 ? Math.max(0, Math.min(1, px / width)) : 0
            }

            onEntered: {
                progressRow.hovered = true
                /* 进来先按当前档位取一张（requestSnapshotForFrame 内部认档位 + 节流，
                   同一档内不会重复截；以前这里是"每次移动都截一张"，正是闪的根源）。 */
                progressRow.requestSnapshotForFrame()
            }
            onExited: {
                progressRow.hovered = false
                progressRow.hoverRatio = -1
                /* 气泡已经收起来了，冷却结束时那次补发没必要再截。 */
                progressRow.snapshotPending = false
                /* 下一次悬停重新按档位取（同一档内不重复截的判据也要跟着清掉） */
                progressRow.requestedFrameIndex = -1
            }
            onPositionChanged: function (mouse) {
                /* 只更新位置：时间文字、气泡位置、档位（apiFrameIndex → onlineSource）都是绑
                   hoverRatio 的，跟着走；截图那条兜底由 requestSnapshotForFrame 按档位请求。 */
                progressRow.hoverRatio = ratioAt(mouse.x)

                if (pressed)
                    progressRow.dragRatio = ratioAt(mouse.x)
            }
            onPressed: function (mouse) {
                /* 新的一次拖动/点击：先放掉上一个挂起位置 */
                progressRow.pendingSeekMs = -1
                progressRow.dragging = true
                progressRow.dragRatio = ratioAt(mouse.x)
            }
            onReleased: function (mouse) {
                progressRow.dragging = false

                if (!bar.player)
                    return

                var d = bar.player.duration

                if (d > 0) {
                    var target = Math.round(ratioAt(mouse.x) * d)
                    /* 界面先停在目标位置，等框架到位再交给 position（否则会跳回去） */
                    progressRow.pendingSeekMs = target
                    bar.player.seek(target)
                }
            }
        }
    }

    /* =======================================================================
     * 高能进度条（pbp）
     *
     * 【"bottom: 100%" 相对谁量】参考的包含块是控制层那一块，而它里面唯一在文档流里的子元素
     * 就是底部按钮行（height:35px + margin-top:20px，index.scss:777-792；遮罩/进度行/影子进度
     * 都是 absolute），所以 bottom:100% = 控制层底边往上 55px。本项目里"进度条那一行的上沿"
     * 是 44px（progressBottom）+ 10px（行高）= 54px，差 1px —— 这里取贴住进度条那一行的上沿：
     * 曲线本来就该紧跟进度条，1px 的差别肉眼看不出来，但"和进度条对齐"这件事必须成立。
     * （左右同理：进度行自己已经有 12px 的 padding，再外扩 12px 就抵到控制层两侧。）
     *
     * ======================================================================= */
    Item {
        id: pbpArea

        /* 有数据可画（progressRowRoot.hasPbpData 的别名，下面 Canvas 里读起来短一点） */
        readonly property bool hasData: progressRowRoot.hasPbpData

        /*
         * 什么时候亮起、可交互。三个条件：
         *   highlightEnabled —— 开关（默认 true）；
         *   hasData           —— 真的要曲线数据，没有就什么都不显示（保证默认界面不变）；
         *   bar.shown || pbpPinned
         *                     —— 控制栏显示时照常显示；**常驻（pbpPinned）时控制栏收起也留着**，
         *                        这正是那颗 pin 的工具提示"打开《高能进度条》常驻"要的效果。
         * 默认（无数据）时 opacity 恒为 0、enabled 恒为假 —— 界面和加这两个功能之前一模一样。
         */
        readonly property bool active: progressRowRoot.highlightEnabled
                                       && pbpArea.hasData
                                       && (bar.shown || progressRowRoot.pbpPinned)

        /*
         * 指针是不是压在那颗 pin 上（气泡显隐、手型、点击都看它）。
         *
         * 【为什么不给 pin 单挂一个 MouseArea】这个 App 的 hover 是"最上层独占"：pin 上再挂一个
         * hoverEnabled 的 MouseArea，就会把整条 pbp 的 hover 判据抢走（curveContainsMouse 变假、
         * pointerInside 跟着抖）—— 正是 docs/QML-POPUP-HOVER-PITFALLS.md 第 5、6 条记的坑。
         * 所以整条 pbp 只有 pbpMouse 一个 MouseArea，pin 的命中用坐标算出来。
         */
        readonly property bool pinHovered: pbpMouse.containsMouse
                                           && pbpArea.pointerOnPin(pbpMouse.mouseX, pbpMouse.mouseY)

        /* 这个点是不是落在那颗 pin 的命中盒里（16×16 外扩 pbpPinHitPadding） */
        function pointerOnPin(px, py) {
            /*
             * 【停靠态不接受 pin 的命中】停靠时那颗 pin 是**隐藏**的（见 pbpPin.visible）。
             * 不加这一句的话，隐藏的图标那一小块仍然会把点击吃掉、把"常驻"关掉 ——
             * 用户看到的就是"贴到底部后点一下曲线右边，常驻莫名其妙没了"。
             */
            if (pbpArea.docked) {
                return false
            }

            var pad = QtPlayerTheme.pbpPinHitPadding

            return px >= pbpPin.x - pad && px <= pbpPin.x + pbpPin.width + pad
                   && py >= pbpPin.y - pad && py <= pbpPin.y + pbpPin.height + pad
        }

        /*
         * 【几何：手算 x/y/width，不用 anchors】
         *
         * 为什么不用 anchors：常驻态要在"跟随控制栏"和"贴到影子进度条上方"两种几何之间
         * **平滑过渡**，而 anchors 推出来的 y 加不了 Behavior（锚点一改就是瞬移）。
         * 手算之后 y/x/width 都能挂 Behavior（见下面三个 Behavior）。
         *
         * 两种状态（都在本组件坐标系里，本组件的 y=0 就是**播放器底边**）：
         *   * 跟随控制栏（默认）：底边贴进度条那一行的**上沿**，也就是紧贴进度条轨道上方
         *     （进度行高 = 4px 轨道 + 6px padding-bottom，行顶边就是轨道顶边）。
         *     非全屏：行顶边 = 底边往上 44（progressBottom）+ 10（行高）= 54px，
         *     所以曲线占 54~82px 这一段 —— 不是控制栏顶部、也不是画面中间；
         *     全屏再按参考抬 7px（.player-pbp.show { bottom: calc(100% + 7px) }，index.scss:2486-2488）。
         *   * 常驻且控制栏收起（docked）：**基准就是 shadowProgress 那条 2px 影子进度条**
         *     —— 进度条那一行这时**不停靠**（用户明确否掉了），屏幕底部只剩它，
         *     所以 pbp 就落在它**正上方**：底边 = 它顶边 − pbpDockedGap(3px)。
         *     具体数值（y = 0 是播放器底边）：影子条 [-2, 0] → pbp 的 y = -2 - 3 - 28 = -33
         *     → pbp 占 [-33, -5]，与影子条之间 3px，不重叠。
         *     横向：x/width **直接绑 shadowProgress 的几何**（同宽，不许短一截）；
         *     里面那层 Canvas 在 docked 时也不再留左右各 12px（见 Canvas 处），
         *     否则曲线会比它下面那条影子条窄 24px。
         *     全屏不额外抬 7px：那 7px 是；控制栏已经收起时基准只有这条影子条，两种屏幕位置一样。
         *
         * 跟随控制栏（未常驻）时：x/width 以进度行为准（左右各外扩 12px 抵回控制栏两侧），
         * 底边贴进度行的上沿（紧贴不留缝），全屏按参考抬 7px。
         */
        readonly property bool docked: progressRowRoot.docked

        /* 停靠：直接绑 shadowProgress 的几何（它就是基准）；
           跟随控制栏：进度行左右各外扩 pbpOverhang(12px)，抵回控制栏两侧 */
        x: docked ? shadowProgress.x : progressRow.x - QtPlayerTheme.pbpOverhang
        width: docked ? shadowProgress.width : progressRow.width + QtPlayerTheme.pbpOverhang * 2
        /*
         * 【两档的"缝"不一样 —— 用户明确要求】
         *   * 跟随控制栏（未常驻）：底边 = 进度行顶边再往上 pbpDockedGap(3px)（用户要求"高能进度条在
         *     进度条上方要有一定距离"，照 shadowProgress 上方那条的距离）—— **保持不变**。
         *   * 常驻且控制栏收起（贴到底部）：**一点缝都不留**，pbp 的底边直接压在下面那条 2px
         *     影子进度条的顶边上（用户原话："吸附到底部的那个进度条之间不要距离、不要间隔"）。
         *     所以这里 docked 那一支减的是 height，不减 pbpDockedGap。
         * 只动 pbp 自己的 y —— 进度行的位置/尺寸一个数都没改（44 / 60 / 10 / 4 / 6 全在）。 */
        y: docked ? (shadowProgress.y - height)
                  : (progressRow.y - QtPlayerTheme.pbpDockedGap - height)
        height: QtPlayerTheme.pbpHeight
        z: -1
        opacity: active ? 1 : 0
        /* 和本文件另外两块一个套路：淡出结束后彻底不可见（opacity 为 0 的 Item 仍然会被点中，
           这里再补一道 visible，保证"没有数据时"连鼠标事件都不会被它接走） */
        visible: opacity > 0
        enabled: active

        /* 两种几何之间平滑过渡（时长跟淡入淡出同一套口径：pbpFadeMs = 200ms） */
        Behavior on y {
            NumberAnimation {
                duration: QtPlayerTheme.pbpFadeMs
                easing.type: Easing.OutCubic
            }
        }
        Behavior on x {
            NumberAnimation {
                duration: QtPlayerTheme.pbpFadeMs
                easing.type: Easing.OutCubic
            }
        }
        Behavior on width {
            NumberAnimation {
                duration: QtPlayerTheme.pbpFadeMs
                easing.type: Easing.OutCubic
            }
        }

        /* 曲线的淡入淡出跟着控制栏走（和进度条那一行同一个节奏：0.2s，见上面的 opacity Behavior） */
        Behavior on opacity {
            NumberAnimation {
                duration: QtPlayerTheme.pbpFadeMs
                easing.type: Easing.OutQuad
            }
        }

        Canvas {
            id: pbpCurve

            /*
             * 曲线左右各让 12px：参考 .player-pbp 有 padding: 0 12px，曲线两端和**控制栏里那条
             * 进度条**对齐（进度行自己左右各缩进 12px，不这么做曲线会比进度条宽出 24px，
             * "高能"位置和进度位置就对不上了）。
             *
             * 【常驻 docked 时不让】那时基准是 shadowProgress，而它是**铺满整宽**的
             * （anchors left/right 到播放器两侧），所以曲线也必须铺满整宽 ——
             * 否则就会比它下面那条影子进度条"短一截"（用户实测反馈）。
             */
            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
                bottom: parent.bottom
                leftMargin: pbpArea.docked ? 0 : QtPlayerTheme.pbpPaddingH
                rightMargin: pbpArea.docked ? 0 : QtPlayerTheme.pbpPaddingH
            }

            /*
             * 这三个属性本身不画东西，只为了让"数据变 / 进度变 / 时长变 / 尺寸变"能触发重画：
             * Canvas 不会自己盯着外部数据，必须显式 requestPaint。
             * durationSec 也要盯着：横轴刻度是 i*step_sec/duration，时长一变（换片、元数据迟到）
             * 曲线就该重画 —— 光看 playedRatio 是发现不了的（时长从 0 变正常时比例可能还是 0）。
             */
            property var curveData: progressRowRoot.pbpData
            property real playedRatio: progressRow.shownRatio
            property real durationSec: progressRow.durationSec

            onCurveDataChanged: requestPaint()
            onPlayedRatioChanged: requestPaint()
            onDurationSecChanged: requestPaint()
            Component.onCompleted: requestPaint()

            /* 尺寸变了画布要重画（窗口缩放/全屏切换都会走到这里） */
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()

            /* 重新变可见时补一次重画：画布内容在不可见期间可能被丢弃，
               少了这一下会出现"开关打开、曲线却空着" */
            onVisibleChanged: {
                if (visible)
                    requestPaint()
            }

            /*
             * 颜色转换：Canvas 的 fillStyle 直接吃 QColor 也行（本文件 moveIndicator 就是那么写的），
             * 但这三处颜色都**带 alpha**，走 CSS 的 rgba() 串最保险；也顺手避开
             * QColor.toString() 那个坑 —— 它给的是 #AARRGGBB，CSS 会按 #RRGGBBAA 解读，颜色全反。
             */
            function cssColor(c) {
                return "rgba(" + Math.round(c.r * 255) + ", " + Math.round(c.g * 255)
                        + ", " + Math.round(c.b * 255) + ", " + c.a + ")"
            }

            /*
             * 曲线下方面积的路径。
             *
             * 【横轴】按**像素**重采样（每 pbpSamplePx 像素一个点），每个点用
             * `curveValueAt()` 在相邻两个采样点之间线性插值取值 —— 原始点距是 step_sec
             * （30 秒 ≈ 几十像素），直接连折线又硬又有棱角。
             * 【纵轴】值 0~1 映射成高度，**值越大画得越高**（上=高能）→ y = 高度 * (1 - 值)；
             * 再乘一个两端的渐隐系数（endFade），让首尾**平滑收敛到基线**，
             * 而不是像以前那样在两端"立起来"（数据第一个点的值通常不是 0，
             * 旧画法从 (0, 基线) 直接跳到 (0, 该点高度)，看着就是一根竖线）。
             */
            function curvePath(ctx, values, stepSec, totalSec) {
                ctx.beginPath()
                ctx.moveTo(0, height)

                var fadePx = QtPlayerTheme.pbpEndFadeRatio * width
                var stepPx = Math.max(1, QtPlayerTheme.pbpSamplePx)

                for (var x = 0; x <= width; x += stepPx) {
                    var v = curveValueAt(values, stepSec, totalSec, (x / width) * totalSec)
                    v *= endFade(x, fadePx)
                    ctx.lineTo(x, height - v * height)
                }

                /* 收尾：补齐最右边一像素并贴回基线（closePath 之前必须回到 baseline）
                   —— 就算上面最后一个采样点落不到 width 上，右端也不会缺一角 */
                ctx.lineTo(width, height - curveValueAt(values, stepSec, totalSec, totalSec)
                                    * endFade(width, fadePx) * height)
                ctx.lineTo(width, height)
                ctx.closePath()
            }

            /* 第 sec 秒处的曲线值：在相邻两个采样点之间线性插值（越界就取端点值） */
            function curveValueAt(values, stepSec, totalSec, sec) {
                if (!values || values.length === 0 || stepSec <= 0)
                    return 0

                var f = sec / stepSec
                var i = Math.floor(f)

                if (i < 0)
                    i = 0

                if (i >= values.length - 1)
                    return Math.max(0, Math.min(1, Number(values[values.length - 1])))

                var frac = f - i
                var a = Math.max(0, Math.min(1, Number(values[i])))
                var b = Math.max(0, Math.min(1, Number(values[i + 1])))
                return a + (b - a) * frac
            }

            /*
             * 两端渐隐系数：0（贴边）→ 1（走完过渡段），中间用 smoothstep `3t² - 2t³`
             * ——它在 t=0 和 t=1 处的一阶导都是 0，所以曲线和基线、和中间段**都是平滑相接**的，
             * 不会像线性斜坡那样留下折角（"缓缓归零"要的正是这个）。
             * fadePx 由主题里的 pbpEndFadeRatio 决定（曲线宽度的 3%）。
             */
            function endFade(x, fadePx) {
                if (fadePx <= 0)
                    return 1

                var t = Math.min(1, x / fadePx, (width - x) / fadePx)

                if (t <= 0)
                    return 0

                return t * t * (3 - 2 * t)
            }

            onPaint: {
                var ctx = getContext("2d")
                ctx.reset()

                if (!pbpArea.hasData || !curveData || progressRow.durationSec <= 0
                        || width <= 0 || height <= 0)
                    return

                var values = curveData.data
                var stepSec = Number(curveData.step_sec)
                var totalSec = progressRow.durationSec
                var ratio = Math.max(0, Math.min(1, playedRatio))

                /* 1) 未播部分：白色 20% 填满曲线下方的面积
                      （参考是一张白色 rect + fill-opacity .2，被曲线路径裁出形状，icons/index.ts:424-429） */
                curvePath(ctx, values, stepSec, totalSec)
                ctx.fillStyle = cssColor(QtPlayerTheme.pbpCurveBg)
                ctx.fill()

                /* 2) 已播部分（0..当前进度）再用主色填一层
                      （参考第二层 rect 用 played-path 再裁一次，icons/index.ts:430-435） */
                if (ratio > 0) {
                    ctx.save()
                    ctx.beginPath()
                    ctx.rect(0, 0, width * ratio, height)
                    ctx.clip()
                    curvePath(ctx, values, stepSec, totalSec)
                    ctx.fillStyle = cssColor(QtPlayerTheme.pbpCurvePlayedBg)
                    ctx.fill()
                    ctx.restore()
                }

                /* 3) 当前进度处一条 1px 竖线（参考 <line> stroke rgba(255,255,255,.2)，icons/index.ts:436-442）。
                      进度到 100% 时 x 正好等于画布宽度，线会整条落到画布外看不见 —— 往回收 1px（clamp） */
                ctx.fillStyle = cssColor(QtPlayerTheme.pbpCursorLine)
                ctx.fillRect(Math.min(Math.round(width * ratio), width - QtPlayerTheme.pbpCursorLineWidth),
                             0, QtPlayerTheme.pbpCursorLineWidth, height)
            }
        }

        /*
         * 右侧那颗 pin。
         *
         * 两个状态的素材都是**用户提供的原件、path 一个字没改**（只去掉了 class/data-pointer）：
         *     未常驻 ../assets/images/pbp-pin-off.svg —— 就是参考里那枚 PinIcon（icons/index.ts:446-457）
         *     已常驻 ../assets/images/pbp-pin-on.svg
         * 白色来自 SVG 自己的 fill="#fff"，所以直接 Image 画，不做着色（Image 也改不了 SVG 的 fill）。
         *
         * 【旋转：只转"斜着画"的那一枚】
         * 参考对图标框写的是 rotate(45deg)，但那是为 off 这枚服务的 —— 它的 path 本身就是
         * 斜 45° 画的（轴线从左上到右下），**转 45° 才是"正"的**；
         * 而 on 那枚的 path 本身已经是竖直的（左右对称、针朝下，见 pbp-pin-on.svg），
         * 再套 45° 就歪了。所以这里 off 保留参考的 45°，on 不转 —— 结果两枚都是正的，
         * 状态区别由"线框 pin / 实心 pin"两套形状给出（用户给的就是这两枚）。
         */
        Item {
            id: pbpPin

            width: QtPlayerTheme.pbpPinSize
            height: QtPlayerTheme.pbpPinSize
            x: parent.width - width - QtPlayerTheme.pbpPinRight
            /*
             * 【停靠态把 pin 藏起来 —— 用户要求，不是 bug 修复】
             * 常驻打开、控制栏收起、曲线贴到底部之后，这颗"常驻开关"已经没用了
             * （它唯一的作用就是把常驻打开），挂着反而挡着曲线右端；
             * 用户原话："高能进度条吸附到底部的那个进度条之后才隐藏图标，其它情况不改变"。
             * 所以只在 docked 时隐藏：控制栏一浮出来（docked 变假）它立刻回来。
             * 命中判据同步关掉（见 pbpArea.pointerOnPin），免得点到"看不见的开关"。
             */
            visible: !pbpArea.docked
            /*
             * 参考写的是 `bottom: 8%`（相对 28px 高的 pbp 区、离底边 2.24px），
             * 而 QML 的 y 是**顶边**：顶边 = 区高 - 图标高 - 区高*8%。
             * 直接写 parent.height * 0.08 会把"离底 8%"当成"离顶 8%"，pin 会高 7.5px。
             */
            y: parent.height - height - parent.height * QtPlayerTheme.pbpPinBottomRatio

            Image {
                id: pbpPinIcon

                anchors.fill: parent
                /* 常驻状态换图标（用户提供的两枚原件），不是靠颜色/透明度区分。
                   路径带 ../ —— 本文件在 controls/ 子目录里，QML 的相对 URL 是按**文件**算的
                   （同目录其它文件都是 `../assets/images/...`，见 LeftControls 的 arrow.svg）。 */
                source: progressRowRoot.pbpPinned ? "../assets/images/pbp-pin-on.svg"
                                                  : "../assets/images/pbp-pin-off.svg"
                /* SVG 是矢量的：按 16px 栅格化，免得被拉到别处再缩放糊掉 */
                sourceSize.width: QtPlayerTheme.pbpPinSize
                sourceSize.height: QtPlayerTheme.pbpPinSize
                fillMode: Image.PreserveAspectFit
                smooth: true
                /* 旋转放在 Image 上（不是外层 Item）：外层 Item 的矩形要留给"命中盒"用，
                   转了之后命中盒就跟着斜了（参见 pbpArea.pointerOnPin） */
                rotation: progressRowRoot.pbpPinned ? 0 : QtPlayerTheme.pbpPinRotation
                /* 图标自己不接鼠标：整条 pbp 只有 pbpMouse 一个 MouseArea（原因见 pinHovered 那段） */
                enabled: false
            }
        }

        /*
         * 那颗 pin 的提示气泡：。
         *
         * 参考里它 `display:none` 是**死代码**（谁也没打开过），本需求做成真的 hover 提示。
         * 样式全部按参考：黑底 + 圆角 2 + 白字 12px + padding 4px 8px + 0.3s 过渡
         * （底色/字色就是那对 tooltip 变量，所以复用主题的 tooltipBg / tooltipText）。
         *
         * 【位置】参考写的是 `right:0; top:0`（相对那个**已经转了 45° 的 16px 图标框**），
         * 而且是死代码、没有实际观感可对。这里按"气泡在 pin 上方、右边缘与 pin 对齐"摆：
         * 往上长不会压住下面的进度条，右对齐则保证它始终留在进度条这一侧、不会跑到画面中间去。
         * 文案随状态变（参考只有"打开…"那一句；已经常驻了还说"打开"就是假话，所以补了另一半）。
         */
        Rectangle {
            id: pbpPinTip

            readonly property string label: progressRowRoot.pbpPinned
                                            ? qsTr("关闭《高能进度条》常驻")
                                            : qsTr("打开《高能进度条》常驻")

            x: Math.max(0, pbpPin.x + pbpPin.width - width)
            y: pbpPin.y - height - QtPlayerTheme.pbpPinTipGap
            width: Math.min(QtPlayerTheme.pbpPinTipMaxWidth,
                            pbpPinTipText.implicitWidth + QtPlayerTheme.pbpPinTipPaddingH * 2)
            /*
             * 高度按**参考的 CSS 公式**算：行高(1.5 × 12px = 18) + 上下 padding(4px) = 26。
             * 不用 `Text.implicitHeight`：QML 的 implicitHeight 走的是**字体度量**
             * （12px 字体的自然行高可能 14~16px，×1.5 之后 21~24px），算出来比参考高 3~6px ——
             * 这正是用户说的"后面的黑色背景有点高"。
             */
            height: Math.round(QtPlayerTheme.pbpPinTipFontSize * QtPlayerTheme.pbpPinTipLineHeight)
                    + QtPlayerTheme.pbpPinTipPaddingV * 2
            radius: QtPlayerTheme.pbpPinTipRadius
            color: QtPlayerTheme.tooltipBg
            /* 淡入淡出（参考那条 transition: all .3s ease-in-out），淡完再彻底不可见 */
            opacity: pbpArea.pinHovered ? 1 : 0
            visible: opacity > 0
            /* 气泡本身**不能**接鼠标：它压在 pin 上方，一接 hover 就会把 pbpMouse 的判据抢走
               （气泡一出现、pin 就"不再被悬停"→ 气泡又消失，来回闪）—— 所以这里不挂任何 MouseArea */
            enabled: false

            Behavior on opacity {
                NumberAnimation {
                    duration: QtPlayerTheme.pbpPinTipFadeMs
                    easing.type: Easing.InOutQuad
                }
            }

            Text {
                id: pbpPinTipText

                /*
                 * 【文字垂直居中：别改成 anchors.centerIn 就完事】
                 *
                 * 用户实测："文字垂直不居中、后面的黑色背景有点高、文字偏上"。
                 * 原因是 QML 和 CSS 对 line-height 的处理不一样：
                 *   * CSS：行盒比字形高出来的部分**上下均分**，所以 padding 4px +
                 *     line-height 1.5 的盒子里，文字看起来是居中的；
                 *   * QML：`lineHeight: 1.5` 多出来的行距**全加到下面**（字形贴着行盒顶），
                 *     于是 `anchors.centerIn` 居中的是"行盒"，字形却偏上 0.25em（12px 字号 ≈ 3px），
                 *     盒子越高、越明显。
                 * 所以这里改成：文字块占满内边距以内的区域，再用
                 * `verticalAlignment: AlignVCenter` 把**行盒**摆中间（Qt 会把这一行居中），
                 * 视觉上就和参考一致了。盒子总高仍按参考公式算（1.5em + 上下 padding）。
                 */
                x: QtPlayerTheme.pbpPinTipPaddingH
                y: QtPlayerTheme.pbpPinTipPaddingV
                width: Math.min(implicitWidth, pbpPinTip.width - QtPlayerTheme.pbpPinTipPaddingH * 2)
                height: pbpPinTip.height - QtPlayerTheme.pbpPinTipPaddingV * 2
                verticalAlignment: Text.AlignVCenter
                color: QtPlayerTheme.tooltipText
                font.pixelSize: QtPlayerTheme.pbpPinTipFontSize
                /*
                 * 单行文本**不设 lineHeight**：CSS 里 `line-height: 1.5` 对单行的作用就是
                 * "把 12px 的字形摆在这 18px 的行盒中间"，而 QML 的 lineHeight 只会把多出来的
                 * 行距加到字形**下面**（= 字偏上，用户实测的那个问题）。这里改成
                 * "行盒高度由外框给、字形用 AlignVCenter 居中"，视觉上和参考一致。
                 */
                /* white-space: nowrap → 不折行，太长就省略号 */
                elide: Text.ElideRight
                text: pbpPinTip.label
            }
        }

        /*
         * 整条 pbp 上**唯一**的鼠标区。
         * 它管三件事：
         *   ① 把"光标在这条区域上"报给控制栏（progressRowRoot.curveContainsMouse → pointerInside，
         *      少了它，光标一碰曲线就被判成"离开控制层"）；enabled 跟着 active 走，
         *      没有数据时连鼠标都不吃（和参考那条 opacity:0 的区域一个效果）；
         *   ② 悬停 pin → 显示提示气泡（pinHovered 用坐标算，见上面那段说明）；
         *   ③ 点 pin → 切换"常驻"（参考里这颗 pin 是死的，工具提示的文案就是它的本意）。
         *
         * 【手型只给 pin】曲线本身不是可点区域，所以曲线上给箭头 —— 免得用户以为点一下会 seek
         */
        MouseArea {
            id: pbpMouse

            anchors.fill: parent
            hoverEnabled: true
            cursorShape: pbpArea.pinHovered ? Qt.PointingHandCursor : Qt.ArrowCursor

            onClicked: function (mouse) {
                if (pbpArea.pointerOnPin(mouse.x, mouse.y))
                    progressRowRoot.pbpPinned = !progressRowRoot.pbpPinned
            }
        }
    }

    /* =======================================================================
     * 底部按钮行
     * ======================================================================= */

    Item {
        id: shadowProgress

        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }
        height: 2

        opacity: bar.shown ? 0 : 1
        visible: opacity > 0
        /* 只是装饰，不参与鼠标交互 */
        enabled: false

        Behavior on opacity {
            NumberAnimation {
                duration: 400
                easing.type: Easing.InQuad
            }
        }

        readonly property real bufferRatio: bar.player ? bar.ratioOf(bar.player.bufferedPosition) : 0
        readonly property real playedRatio: bar.player ? bar.ratioOf(bar.player.position) : 0

        Rectangle {
            anchors.fill: parent
            radius: 1.5
            color: QtPlayerTheme.progressTrackBg
        }

        Rectangle {
            anchors {
                left: parent.left
                top: parent.top
                bottom: parent.bottom
            }
            width: parent.width * Math.max(0, Math.min(1, shadowProgress.bufferRatio))
            radius: 1.5
            color: QtPlayerTheme.progressBufferBg
        }

        Rectangle {
            anchors {
                left: parent.left
                top: parent.top
                bottom: parent.bottom
            }
            width: parent.width * Math.max(0, Math.min(1, shadowProgress.playedRatio))
            radius: 1.5
            color: QtPlayerTheme.progressPlayedBg
        }
    }
}
