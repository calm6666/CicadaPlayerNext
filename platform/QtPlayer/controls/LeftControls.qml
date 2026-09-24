// ===========================================================================
// 左边那一组：上一个 / 播放暂停 / 下一个 / 时间（从 PlayerControlBar.qml 拆出来）
//
// 【按钮顺序按参考实现来，不是我们自己排的】
//   front/hili-player/packages/player/src/components/LeftControls.ts:350
//       const bottomLeftOrder = ['prev', 'play', 'next', 'time', 'viewpoint'];
//   也就是说 **上一个在播放/暂停的左边**、下一个在右边：
//
//       [◀◀ 上一个] [▶ 播放/暂停] [下一个 ▶▶] [ 00:12 / 03:45 ]
//
//   曾经排成 [播放] [上一个] [下一个] [时间]（把两个按钮都挂到播放右边），后果有两个，
//   都是用户实测反馈的：① 上一个跑到播放键后面去了；② 下一个和播放键之间白隔了一个
//   按钮的宽度，"距离太远"。参考里三颗按钮是**紧挨着**的。
//
// 【间距】参考没有给这三颗按钮任何额外外边距，全靠按钮自己 36x22（全屏 54x43）的
//   盒子 + 里面 22px（全屏 28px）的图标居中撑开 —— 相邻两颗图标之间就是
//   (btnWidth-iconSize) 一共 14px（全屏 26px）。所以这里**什么都不用加**：
//   bar.btnWidth / bar.iconSize 就是参考那两个值（见 QtPlayerTheme 的出处注释），
//   四块用一个 `Row` 挨着排出来就是参考的间距。
//
// 【排版用 Row，不用"anchors.left = 前一颗.right"那条链】见下面 leftGroup 那段注释：
//   链式锚点会把**顺序藏在四条锚点里**，写错一条屏幕上是另一个顺序，看结构看不出来。
//   现在四个子项的书写顺序 = 从左到右的屏幕顺序。
//
// 【图标颜色】参考的箭头是**内联 svg**，path 上没有 fill，靠 CSS 继承：
//   index.scss:309  .player-ctrl-btn { fill: #fff; color: hsla(0,0%,100%,.8) }
//   index.scss:337  .player-ctrl-btn-icon > .common-svg-icon { height: 22px; opacity: 0.9 }
//   QML 里没有 CSS 继承这回事，Image 直接按 SVG 自己的属性画 —— 而那份 SVG 不带 fill，
//   浏览器/QtSvg 的默认填充都是**黑色**，压在深色控制栏上等于看不见。
//   所以：素材里把参考那两条 CSS 的最终效果**固化**下来（path 写死 fill="#ffffff"），
//   透明度交给这里的 opacity（= 主题里的 ctrlIconOpacity，正好是参考的 0.9）。
//   **换素材时别再把这个 fill 丢掉。**
//
// 依赖：bar（主题数值 / formatTime / displayPositionMs / hasPrevious / hasNext）、
//       player（播放器）。
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
    property var bar: null
    property var player: null

    /* -----------------------------------------------------------------------
     * 分段（视点）数据 —— 控制栏左组最后那颗「章节 · xxx」按钮 + hover 面板的数据源
     *
     * 每项：`{from, to, content}`
     *   from    —— 这一段从第几秒开始（**秒**，参考 `viewpoint.startTime`；点击时 ×1000 转毫秒）
     *   to      —— 这一段到第几秒结束（秒）。本文件**只用来占位**（参考的列表项也没读它），
     *              留着是因为它和 front/hili-player 的进度条分段模型是同一份数据
     *   content —— 段标题（参考里的 pointText）
     *
     * 【默认空数组 = 按钮不存在】空数组或只有 1 段时那颗按钮 visible 为假、宽度 0，
     * 控制栏和加这个功能之前**完全一样**（参考也是 `viewpoints.length > 1` 才拼进 DOM，
     * controls/index.ts:1047-1049）。目前应用层还没有喂数据的入口，所以现状就是"按钮不出现"。
     *
     * 【秒还是毫秒】这里坚持用**秒**：参考的 data-time 就是秒
     * （controls/index.ts:1089 写 `viewpoint.startTime`，:834 直接喂给 change-currentTime），
     * 应用层那边拿到的是 B 站的进度点数据，也是秒。换算只做一次，就在 viewPointChoose 里。
     * ----------------------------------------------------------------------- */
    property var viewPoints: []
    /*
     * 段数据换了（换片/重新拉取）→ 收起面板、标题回到第 1 段。
     * 【为什么必须挂在根对象上】`onViewPointsChanged` 只能由**声明该属性的对象**来写；
     * 之前它被写在按钮子对象里，导致 LeftControls 直接加载失败（连锁到 Main），
     * 表现就是"点视频卡片打不开播放窗口"。这里才是唯一合法的地方。
     */
    onViewPointsChanged: viewPointButton.viewPointReset()

    /*
     * 选了某一段：**参数是毫秒**（已经由 viewPointChoose 从秒换过来）。
     *
     * 【为什么由父对象去 seek，而不是本文件直接 bar.player.seek()】秒→毫秒的换算、
     * "点了哪一段"这类状态留在本文件，真正动播放器的动作交给 PlayerControlBar 转发 ——
     * 和本文件既有的 previousRequested / nextRequested 一个路子（那两个信号也是在
     * PlayerControlBar 上被接住再转给应用层）。
     * 只 seek，**不限制只在这一段内播放**（用户明确要求；参考的 change-currentTime
     * 也只是一次普通跳转，见 controls/index.ts:830-838）。
     */
    signal viewPointSelected(real timeMs)

    /*
     * 三颗播放控制按钮实际用的宽度 —— **按图标真正画出来的宽度算，不照抄 CSS 的 36px**。
     *
     * 【为什么不直接用 (btnWidth - iconSize)/2 那套】参考 `.player-ctrl-btn { width:36px }`
     * （全屏 54）、里面 22px（全屏 28）图标居中；那个公式**默认图标把 iconSize 填满了**。
     * 而我们的箭头素材 next-play.svg 的 viewBox 是 `0 0 22 22`，path 实际只画到
     * x∈[5,17]、y∈[5,16.8]，也就是 **12 × 11.8 个单位** —— 22 里有近一半是透明的。
     * 渲染成 22px 后箭头本体只有约 12px 宽，左右各多出约 5px 透明。
     *
     * 于是照抄 36px 的实际效果是：
     *     每颗按钮的空隙 = 盒子 (36-22)/2 = 7px  ＋ 图标自带 (22-12)/2 = 5px  = 12px
     *     相邻两颗图标之间 = 12 + 12 = **24px**，看着就是"间距太远"。
     *
     * 所以这里把**图标自带的透明边从盒子里扣掉**：
     *     buttonWidth = iconSize + 2*pad − (iconSize − iconSize*inkRatio)
     *                 = iconSize*inkRatio + (btnWidth − iconSize)
     * 其中 pad = (btnWidth − iconSize)/2 就是参考的盒子内边距（非全屏 7、全屏 13）。
     * 效果：相邻两颗图标的**墨迹**之间 = btnWidth − iconSize（非全屏 14px、全屏 26px），
     * "盒子挨盒子"的意图没变，只是不再把图标的透明边也算成间距。
     *
     *   非全屏：22 × 12/22 + (36 − 22) = 12 + 14 = **26px**（原来 36）
     *   全屏  ：28 × 12/22 + (54 − 28) ≈ 15 + 26 = **41px**（原来 54）
     *
     * 换素材时 inkRatio 要跟着改：去看新 SVG 的 path 在 viewBox 里占了多宽。
     */
    readonly property real iconInkRatio: 12.0 / 22.0
    readonly property real buttonWidth: Math.round(bar.iconSize * iconInkRatio)
                                        + (bar.btnWidth - bar.iconSize)

    /*
     * 本组件的尺寸 = 里面那层 leftGroup 的尺寸。
     *
     * 【为什么必须补这两行 · 发送栏不随窗口缩放的根因】拆成组件之后，外面看到的"左边那一组"是
     * **本组件这个实例**，而不是里面那层有几何的 leftGroup；本组件自己不设宽高就是 0x0，于是：
     *   * 控制栏里发送栏的锚点       left: leftGroup.right          → 变成 0（发送栏贴到最左边）
     *   * 弹幕那一块算可用宽度       rightGroup.x − (leftGroup.x + leftGroup.width)
     *                                                            → 凭空多出一个左组的宽度
     * 可用宽度偏大 → 胶囊被顶到最宽 450 并停在那里 → 拖窗口时不再跟着缩放。
     * 里面那层 leftGroup 锚在本组件左上角（本组件 x 恒为 0），所以这里跟上它的宽高，
     * 上面两个表达式就都和拆分前**完全一致**了。
     */
    implicitWidth: leftGroup.width
    implicitHeight: leftGroup.height

    /* 播放按钮上的鼠标判据：控制栏的 pointerInside 要用它（原来直接写 playArea.containsMouse） */
    readonly property alias pointerInside: anyHover.hovered

    /*
     * 时间那一格自己的 hover（上面那个 HoverHandler 会被这一格的 MouseArea 抢走 hover，
     * 所以控制栏的 pointerInside 里也要加上它 —— 漏了就是"鼠标停在时间上控制栏却收起来"）。
     */
    readonly property alias timeHovered: timeClickArea.containsMouse

    /*
     * 视点按钮 / 它的面板自己的 hover —— 同 timeHovered 的道理：
     * 那两块 MouseArea 开着 hoverEnabled，会把 hover 从外面的控制栏容器手里拿走，
     * 所以控制栏的 pointerInside 必须带上这一条（漏了 → 鼠标停到视点按钮上，
     * 控制栏以为指针走了，整条自己收起来）。
     * 这里直接用**按钮自己那条 hover 判据**（含下面板那块区域的并集），
     * 不是另算一份 —— 面板开着时指针在面板上，控制栏也不该收。
     */
    readonly property alias viewPointHovered: viewPointButton.pointerInViewPointArea

    /*
     * 把视点数据里的时间字段读成数字（秒）。
     *
     * 【为什么要这道手续】视点数据是从应用层 / C++ 那边喂进来的，字段可能是
     * number，也可能是 JSON 或接口给的字符串（"123.5"）。`from * 1000` 遇到字符串会得到
     * NaN，seek 到 NaN 的后果是"点了没反应"或者跳到 0，很难查。
     * 这里统一收成数字；读不出来就返回 0（= 从头开始，和无数据时的行为一致）。
     */
    function viewPointToNumber(value) {
        if (value === undefined || value === null)
            return 0

        var n = (typeof value === "number") ? value : parseFloat("" + value)

        return isNaN(n) ? 0 : n
    }

    /* -----------------------------------------------------------------------
     * 时间输入框（跳转）：参考 .player-ctrl-time-seek
     *
     * 参考出处：controls.scss:599-613（老参考）+ 用户给的 bpx- 版本 CSS，两边数值一样：
     *     position:absolute; left:6px; top:0; width:60px; height:20px; padding:0 5px;
     *     background: hsla(0,0%,100%,.2); border:1px solid transparent;
     *     color: inherit; font-size:12px; line-height:20px; text-align:center;
     * 参考里那个 input 只是个**空壳**（LeftControls.ts:329 声明了
     * `#playerCtrlTimeSeekInput`，但整个参考工程没有一处引用它），所以行为按用户要求实现：
     *     点时间 → 输入框出现、时间文字隐藏，输入框里预填**当前播放时间**；
     *     点输入框以外 → 关掉，并按输入的时间 seek；
     *     输入不规范 → 只关掉、**不 seek**（用户明确要求，防止乱输入把人 seek 飞）。
     * "点外面"那一层在 PlayerView.qml（要在控制栏之上，见那边的长注释）。
     * ----------------------------------------------------------------------- */
    property bool timeEditing: false

    /*
     * 参考 .player-ctrl-time-seek 的 `left: 6px`（controls.scss:599-613）。
     * 现在它只在**全屏**那一档被用到（timeLabel.noNextGap 的全屏分支）——
     * 非全屏那一档用户要求更大（见 noNextGap 的说明）；输入框自己的 x 也不再是写死的 6，
     * 而是跟着时间文字的左边缘走（这样数字不会和时间错位）。
     */
    readonly property int seekInputLeft: 6

    function beginTimeEdit() {
        if (!bar || !bar.player)
            return

        timeEditing = true
        /* 预填"点击那一刻的播放时间"（如 00:03）—— 用显示用的那个格式化函数，格式一致。 */
        seekInput.text = bar.formatTime(bar.displayPositionMs)
        seekInput.forceActiveFocus()
        seekInput.selectAll()          /* 直接覆盖输入，不用先删旧的 */
    }

    function commitTimeEdit() {
        if (!timeEditing)
            return

        var text = seekInput.text
        timeEditing = false

        var ms = parseSeekTime(text)

        if (ms < 0)
            return                     /* 不规范：只关，不 seek */

        var total = bar.player ? bar.player.duration : 0

        if (total > 0 && ms > total)
            ms = total                 /* 超出总时长就夹到末尾（不往片尾之后 seek） */

        if (bar.player)
            bar.player.seek(ms)
    }

    /*
     * 用户输入 → 毫秒；**不规范返回 -1**。
     *
     * 接受的写法（参考里那个 input 的初值就是 `0:00` 这种不补零的写法）：
     *     M:SS / MM:SS         分 + 秒        秒必须 0-59
     *     H:MM:SS / HH:MM:SS   时 + 分 + 秒   分、秒必须 0-59
     * 其它一律算不规范：空串、带字母或符号、冒号个数不对（0 个 / 3 个以上）、
     * 分或秒 ≥ 60、某一段超过 3 位数字、负数……（用户要求：不规范就只关不 seek）
     */
    function parseSeekTime(text) {
        if (text === undefined || text === null)
            return -1

        var s = ("" + text).trim()

        if (s === "")
            return -1

        var parts = s.split(":")

        if (parts.length !== 2 && parts.length !== 3)
            return -1

        var nums = []

        for (var i = 0; i < parts.length; ++i) {
            var p = parts[i]

            /* 只允许纯数字、最多 3 位（挡住 "1e3"、"+5"、" 1 2"、"9999" 这类） */
            if (!/^[0-9]{1,3}$/.test(p))
                return -1

            nums.push(parseInt(p, 10))
        }

        if (parts.length === 2) {
            var minutes = nums[0]
            var seconds = nums[1]

            if (seconds > 59)
                return -1

            return (minutes * 60 + seconds) * 1000
        }

        var hours = nums[0]
        var mm = nums[1]
        var ss = nums[2]

        if (mm > 59 || ss > 59)
            return -1

        return ((hours * 60 + mm) * 60 + ss) * 1000
    }

    /*
     * 输入框在**给定 item 的坐标系**里的矩形。
     * "点外面就关"那一层用它判断"这一下按在不在输入框里"（在里面就不关、交给输入框自己）。
     */
    function seekInputRectIn(item) {
        var p = seekInput.mapToItem(item, 0, 0)
        return Qt.rect(p.x, p.y, seekInput.width, seekInput.height)
    }

    HoverHandler {
        id: anyHover
    }


        /*
         * 【用 Row，不用"anchors.left: 前一颗.right"那条链 —— 这是用户反复反馈的那个 bug】
         *
         * 以前四个孩子是一颗串一颗串起来的：
         *     prev.left = parent.left
         *     play.left = prev.right
         *     next.left = play.right
         *     time.left = next.right
         * 这样写，**顺序藏在四条锚点里**：任何一条写错、或者以后有人只改了其中一条，
         * 屏幕上就是另一个排列，而看代码结构一眼看不出来（"上一个跑到时间右边"就是
         * 这一类问题：顺序不是写在那四行的位置上的）。
         *
         * 现在改成 Row：**子项的书写顺序 = 屏幕上从左到右的顺序**，
         * 和参考实现 bottomLeftOrder = ['prev','play','next','time'] 一一对应。
         * 想改顺序，就把整块上下挪，不用去碰任何一条锚点。
         *
         * 下面四个 Item 就是最终排版，顺序不会再被别的东西影响：
         *     1) prevButton（上一个）  2) playButton（播放/暂停）
         *     3) nextButton（下一个）  4) timeLabel（时间）
         */
        Row {
            id: leftGroup

            /*
             * 【x/y 直接写 0，不用 anchors】anchors 会被"应用的布局方向"镜像
             * （RTL 时 `left: parent.left` 会变成贴右边），而这里的 x 就是相对
             * 控制栏这一行左上角的，写死 0 没有任何镜像语义、不会被翻。
             */
            x: 0
            y: 0

            /*
             * 【强制从左到右 —— 这一条是"上一个跑到时间右边"的直接原因】
             *
             * `Row` 是按 `layoutDirection` 排子项的，默认值是 `Qt.LayoutDirectionAuto`，
             * 而 Auto 会跟着**应用/系统的布局方向**走（Qt 根据区域设置推导）。
             * 一旦是 RightToLeft，Row 就**从右往左**摆：
             *
             *     [上一个][播放][下一个][时间]   →   [时间][下一个][播放][上一个]
             *
             * 也就是"上一个"整颗跑到最右边、正好在"时间"的右边 —— 和用户反馈完全一致。
             * 而且这种翻转**只影响渲染**：每个子项自己的 x 照样是 0/36/72/108
             * （Row 改的是它写进去的 x），所以打印子项坐标是看不出来的。
             *
             * 参考实现（front/hili-player）是固定的
             * bottomLeftOrder = ['prev','play','next','time']，不跟语言方向翻，
             * 所以这里把方向钉死。
             */
            layoutDirection: Qt.LeftToRight

            /* Row 不带间距：参考里这三颗按钮也是紧挨着的（没有任何 margin），
               间距完全由"按钮 36x22 的盒子 + 里面居中的 22px 图标"决定。 */
            spacing: 0
            /* 宽度 = 实际显示的那几颗之和（隐藏的按钮宽度是 0，不占位）；
               bar.leftGroupMinWidth 只有全屏才是 316（参考 index.scss:4965 的 min-width:316px），
               窗口模式下是 0。 */
            width: Math.max(bar.leftGroupMinWidth, childrenRect.width)
            height: bar.btnHeight

            /* ===============================================================
             * 1) 上一个视频 —— **最左边**（参考 bottomLeftOrder 的第一个就是 prev）
             *
             * 显隐：列表里有上一条才显示（bar.hasPrevious，由 VideoLibrary 算好喂进来）。
             * 所以**第一条视频不会出现这颗按钮**；隐藏时 width 归 0，播放键自动顶到最左，
             * 后面几颗跟着整体左移（和参考里 config.prev 为假时干脆不渲染那颗按钮一致）。
             * 图标：同一张 next-play.svg **旋转 180°**（参考 controls.scss:587-590
             * `.player-ctrl-prev .player-ctrl-btn-icon { transform: rotate(180deg) }`，
             * 用户也要求不另存一份文件）。
             * =============================================================== */
            Item {
                id: prevButton

                visible: bar.hasPrevious
                /* 隐藏时宽度归 0，Row 自动把后面几颗整体左移
                   （等价于参考里 config.prev 为假时干脆不渲染那颗按钮）。
                   宽度用 buttonWidth（= 图标实际画幅 + 参考的盒子内边距），
                   不是照抄的 bar.btnWidth，理由见根对象上那段说明。 */
                width: visible ? buttonWidth : 0
                height: bar.btnHeight
                /* 【这里没有 anchors.left】位置由外面的 Row 按书写顺序决定 */

                Image {
                    id: prevButtonIcon

                    /*
                     * 【位置手算，不用 anchors、也不用 transform】
                     *
                     * 之前是
                     *     anchors { top: parent.top; topMargin: ...; horizontalCenter: parent.horizontalCenter }
                     *     transform: Rotation { origin.x: width/2; origin.y: height/2; angle: 180 }
                     * 这两样叠起来有个要命的地方：**anchors 按"没有 transform 时的几何"算，
                     * 而屏幕上画的是 transform 之后的结果**。只要 Rotation 的 origin 算错
                     * （`width` 解析成别的东西、或求值时还是 0），图标就整体平移一段，
                     * 而按钮容器自己的 x 一点没变 —— "打印按钮坐标"看起来完全正常，
                     * 渲染出来的图标却在别的位置。用户指的就是这一块。
                     *
                     * 现在位置只有一条一眼能验的公式，翻转交给 Image 自带的 mirror，
                     * **完全没有 origin 参与**，图标不可能跑到按钮外面。
                     */
                    x: (parent.width - width) / 2
                    y: (bar.btnLineHeight - height) / 2
                    width: bar.iconSize
                    height: bar.iconSize
                    source: "../assets/images/next-play.svg"
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    /* 参考 .common-svg-icon{opacity:.9} */
                    opacity: QtPlayerTheme.ctrlIconOpacity
                    enabled: false

                    /*
                     * 上一个 = 同一张 next-play.svg **水平翻转**（用户要求不另存一份素材）。
                     *
                     * 【为什么用 mirror 而不是 transform: Rotation{angle:180}】
                     * 这张图是"播放三角 + 右边一条竖杠"，上下对称，所以 mirror（水平翻转）
                     * 和 rotate(180°) **画出来一模一样**；但 mirror 是 Image 自己的属性，
                     * 不参与坐标计算、也不碰 origin —— rotate 那条路要自己算 origin，
                     * 算错就是"图标整体偏移"（见上面那段）。
                     * 参考那边用的是 CSS `transform: rotate(180deg)`（controls.scss:587-590），
                     * 效果等价于这里的 mirror。
                     */
                    mirror: true
                }

                MouseArea {
                    id: prevArea

                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: bar.previousRequested()
                }
            }

            /* ===============================================================
             * 2) 播放 / 暂停 —— 夹在上一个和下一个中间（参考的第二个就是 play）
             *
             * 【位置由外面的 Row 决定】上一版这里写的是 `anchors.left: prevButton.right`，
             * 也就是"锚到前一颗的右边"；现在一律交给 Row，四块只按书写顺序排，
             * 少一层"顺序藏在锚点里"的隐患。
             * =============================================================== */
            Item {
                id: playButton

                /* 同 prevButton：宽度按图标实际画幅算，见根对象上那段说明 */
                width: buttonWidth
                height: bar.btnHeight

                /*
                 * 播放/暂停：两槽，顺序和参考 LeftControls.ts:296/301 一样
                 *   slot 0 = pause-to-play-animation.json
                 *   slot 1 = play-to-pause-animation.json
                 *
                 * 【哪个槽对应哪个状态】按参考 LeftControls.ts:111-120 的映射（不是按文件名猜）：
                 *     if (playing) { playToPauseAnimation(); }   → 播放中用 slot 1
                 *     else         { pauseToPlayAnimation(); }   → 暂停中用 slot 0
                 * 也就是：**slot 1 的末帧 = 暂停态该显示的样子（播放三角），
                 *          slot 0 的末帧 = 播放态该显示的样子（两条竖线）**。
                 * 两个槽都是 complete:"stop"，所以"播一遍就停在末帧"。
                 */
                LottieIcon {
                    id: playIcon

                    /* 位置手算（和两颗箭头图标同一套）：图标在 btnLineHeight 那条行盒里
                       居中（全屏行盒 32 < 盒高 43，所以不是居盒中）。不用 anchors —— 免得
                       被布局方向镜像。 */
                    x: (parent.width - width) / 2
                    y: (bar.btnLineHeight - height) / 2
                    width: bar.iconSize
                    height: bar.iconSize
                    canvasWidth: 28
                    canvasHeight: 28
                    /* 和相邻两颗 SVG 箭头一致的不透明度（参考 .common-svg-icon{opacity:.9}） */
                    opacity: QtPlayerTheme.ctrlIconOpacity
                    sequence: [
                        {
                            "source": "assets/lottie-icon/pause-to-play-animation.json",
                            "complete": "stop",
                            "autoplay": false
                        },
                        {
                            "source": "assets/lottie-icon/play-to-pause-animation.json",
                            "complete": "stop",
                            "autoplay": false
                        }
                    ]
                    /*
                     * 初始槽 = 0（参考 LeftControls.ts:292-307 没有传 initialSlotIndex，
                     * LottieIcon.ts 的默认值就是 0）。挂载时不播动画 → 停在第 0 帧，
                     * 而 pause-to-play 的第 0 帧就是"播放三角" = 暂停态该有的样子。
                     * 不要按 playing 去算初始槽：那样会用 play-to-pause 的第 0 帧（空帧）。
                     */
                    slotIndex: 0
                }

                MouseArea {
                    anchors.fill: parent
                    id: playArea
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (bar.player)
                            bar.player.togglePause()
                    }
                }

                /* 图标跟**真实播放状态**走（参考那颗按钮只改自己的变量、事件没人接） */
                Connections {
                    target: bar.player

                    function onPlayingChanged() {
                        /* playing → 槽 0；暂停 → 槽 1（见上面那段说明） */
                        var want = (bar.player && bar.player.playing) ? 0 : 1

                        /* 参考 LeftControls.ts:126-142 的写法：槽不对就 advanceSlot()，然后 play() */
                        if (playIcon.slotIndex !== want)
                            playIcon.advanceSlot()

                        playIcon.play()
                    }
                }
            }

            /* ===============================================================
             * 3) 下一个视频 —— 紧挨着播放/暂停的右边（参考 bottomLeftOrder 的第三个）
             *
             * 显隐：列表里有下一条才显示（bar.hasNext）。
             * 所以**最后一条视频不会出现这颗按钮**。隐藏时 width 归 0，时间跟着左移。
             * 【注意这颗是右箭头原样，不旋转】旋转的是上一个那颗。
             * =============================================================== */
            Item {
                id: nextButton

                visible: bar.hasNext
                /* 隐藏时宽度归 0，时间跟着左移（最后一条视频不显示这颗） */
                width: visible ? buttonWidth : 0
                height: bar.btnHeight
                /* 【这里没有 anchors.left】位置由外面的 Row 按书写顺序决定 */

                Image {
                    id: nextButtonIcon

                    /* 位置手算（和 prevButtonIcon 同一套，见那边的说明）：
                       不用 anchors、也不用 transform，图标只会在按钮盒子正中。 */
                    x: (parent.width - width) / 2
                    y: (bar.btnLineHeight - height) / 2
                    width: bar.iconSize
                    height: bar.iconSize
                    source: "../assets/images/next-play.svg"
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    opacity: QtPlayerTheme.ctrlIconOpacity
                    enabled: false

                    /* 【这一颗**不翻转**】原图就是"下一个"的样子；翻转的是上一个那颗 */
                }

                MouseArea {
                    id: nextArea

                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: bar.nextRequested()
                }
            }

            /* ===============================================================
             * 4) 时间（最后一位）
             *
             * 【前面那个 " / " 是两个 span】参考里是 current + divide + duration 三段，
             * divide 的 padding 由 ctrlTimeDividePadding 给出（控制栏那边拼好了）。
             *
             * 【"没有下一个按钮"时要多留一段距离 —— 用户要求，两轮才说清】
             * `Row` 里隐藏的子项宽度归 0，所以最后一条视频（`bar.hasNext` 为假）时时间会
             * **直接贴到播放/暂停键上**，比"下一个还在"的时候近一大截：
             *     有下一个：播放键盒子 + 下一个按钮槽位(26/41) + 时间左边距(0/16)
             *     没下一个：播放键盒子 + 时间左边距(0/16)                 ← 用户说的"太近"
             *
             * 【留多少】**不是**把下一个按钮的整个槽位空出来（那样中间一大块空白，很难看 ——
             * 用户明确否掉了这一版），而是只补**一段距离**，取值就是参考里"相邻两颗按钮之间的
             * 墨迹距离"：
             *     btnWidth − iconSize     非全屏 14px、全屏 26px
             * （和本文件根对象那段"要考虑图标实际画幅"的结论同一个值 —— 参考的按钮盒子里，
             *  图标两侧各有 (btnWidth−iconSize)/2 的透明边，这个差值就是两颗图标墨迹之间的空隙。）
             *
             * 【怎么加】加在**时间文字自己**的 x 上，不是加一个占位 Item：
             *   * 占位 Item 会在播放键和时间盒子之间留出一块**可见的空白**（槽位版就是这么被否的）；
             *   * 加在文字上则盒子位置不动、只有文字右移，看上去就是一条边距，不会有"空洞"。
             * 盒子宽度同步加上这段距离，保证文字不会被挤出盒子。
             * =============================================================== */
            Item {
                id: timeLabel

                /*
                 * 时间文字的左偏移 = 它离**前面那颗按钮盒子**的距离。
                 *
                 * 【为什么不照抄参考的 timeIndent】参考那颗按钮盒子是 36px（全屏 54）、图标
                 * 22（全屏 28）居中；**我们按"图标的实际画幅"把盒子裁窄了**（见根对象那段说明：
                 * 非全屏 26 / 全屏 41），所以照抄参考的偏移，文字会比参考**贴着图标近 10px**。
                 * 这是用户来回反馈三轮才定下来的口径：
                 *   非全屏：**有没有「下一个」都用 `btnWidth − iconSize`（14px）** ——
                 *           前面无论是播放键还是「下一个」，盒子都是 26px 宽，距离取同一个值。
                 *           参考的 `timeIndent = 0` 会让文字直接贴在前一颗按钮上
                 *           （用户："非全屏并且存在下一个按钮时…离下一个太近了"）。
                 *   全屏  ：有「下一个」时用参考的 indent（16）；没有时用 6px
                 *           （用户实测"全屏看着可以"，这一档别动）。
                 *
                 * 【一个数就够】想微调只改这里：非全屏 = `btnWidth − iconSize`，全屏两支各一个常量。
                 */
                readonly property real noNextGap: bar.fullscreen
                                                  ? seekInputLeft
                                                  : Math.round(bar.btnWidth - bar.iconSize)
                readonly property real textGap: bar.fullscreen
                                                ? (bar.hasNext ? bar.timeIndent : noNextGap)
                                                : noNextGap

                /*
                 * 这一格的"内容宽度" = 时间文字那一整段的长度（参考 min-width:90px 兜底）。
                 * 只用在本格的宽度上；**输入框不用它**（输入框贴的是"文字那一串"的矩形，见下面），
                 * 否则文字比兜底宽度短的时候，输入框会在右边多出一截（用户："右边是对齐的"那类反馈）。
                 */
                readonly property real contentWidth:
                    Math.max(QtPlayerTheme.ctrlTimeMinWidth, timeText.implicitWidth + textGap)

                /* 【这里没有 anchors】位置在 Row 里的第四个（最后一位） */
                width: contentWidth
                height: bar.btnLineHeight

                Text {
                    id: timeText

                    /* 编辑中就把时间文字藏起来（用户要求："点击时间就打开输入框隐藏时间"） */
                    visible: !timeEditing

                    /* 位置/宽度手算（不用 anchors，避免被布局方向镜像）。 */
                    x: timeLabel.textGap
                    y: 0
                    width: parent.width - timeLabel.textGap
                    height: bar.btnLineHeight
                    /* 文字同样在 btnLineHeight 那条行盒里居中（全屏行盒 32 < 盒高 43） */
                    verticalAlignment: Text.AlignVCenter
                    color: QtPlayerTheme.ctrlBtnText
                    font.pixelSize: bar.timeFontSize
                    text: bar.formatTime(bar.displayPositionMs)
                          + " / " + bar.formatTime(bar.player ? bar.player.duration : 0)
                }

                /*
                 * 点时间 → 打开输入框。
                 *
                 * 【为什么不用 TapHandler】这一格在控制栏那个"整块容器"的 MouseArea（containerHover）
                 * 上面，MouseArea 对按下的优先级是确定的（在最上面的先接），TapHandler 那种
                 * "手势竞技场"的路子在这种"叠在一大片 MouseArea 上"的场景里不好判断；
                 * 这里要的只是"点一下"，用 MouseArea 最直白。
                 *
                 * hoverEnabled: true 会把 hover 从 containerHover / 上面的 HoverHandler 手里拿走，
                 * 所以根对象上透出了 `timeHovered`，控制栏的 pointerInside 里必须带上它
                 * （否则鼠标停在时间上会被判成"离开控制栏"→ 控制栏自己收起来）。
                 */
                MouseArea {
                    id: timeClickArea

                    anchors.fill: parent
                    enabled: !timeEditing          /* 编辑中这一层让开，别挡住输入框的点击 */
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: beginTimeEdit()
                }

                /*
                 * 输入框本体：数值/样式照参考（.player-ctrl-time-seek），见根对象上那段说明。
                 * 声明在最后 = 盖在点击层上面（编辑中点击层又是 disabled 的，双保险）。
                 */
                TextField {
                    id: seekInput

                    visible: timeEditing
                    /*
                     * 【矩形直接贴住"时间那一串字"本身，文字居中】
                     *   x     = 时间文字的 x（timeText.x）
                     *   width = 时间文字的**实际字宽**（timeText.implicitWidth）
                     * 盒子的左右边缘都和时间文字严格对齐（用户："右边是对齐的"）。
                     *
                     * 【走过的三版弯路，别再折腾】
                     *   ① 盒子写死 left:6 → 全屏那档文字在 16（timeIndent）→ "输入框左边比时间文字长"；
                     *   ② 盒子铺到"这一格"右边缘 → 文字比兜底 min-width(90) 短时右边多出一截；
                     *   ③ 把文字设成左对齐去凑时间文字 → 用户："seek 输入框文字要居中，现在怎么靠左了"。
                     * 结论：**盒子的矩形要贴住文字（①②），但文字本身仍然按参考居中（③）** ——
                     * 这两件事互不冲突，之前把它们混在一起了。
                     */
                    x: timeText.x
                    y: (parent.height - height) / 2
                    width: timeText.implicitWidth

                    /*
                     * 高度/字号跟模式走（参考里是那条媒体查询）：
                     *   基础规则：height:20px font-size:12px；全屏：height:30px font-size:14px
                     * 字号用 bar.timeFontSize（非全屏 12 / 全屏 14）—— 和时间文字**同一个值**，
                     * 点开输入框时字号不跳。
                     */
                    height: bar.fullscreen ? 30 : 20

                    /* padding: 0 5px（参考）。文字居中时它是左右对称的，不会把文字推偏。 */
                    leftPadding: 5
                    rightPadding: 5
                    topPadding: 0
                    bottomPadding: 0

                    horizontalAlignment: TextInput.AlignHCenter   /* text-align: center（参考，用户要求） */
                    verticalAlignment: TextInput.AlignVCenter     /* line-height（单行居中，20 / 30px） */
                    color: QtPlayerTheme.ctrlBtnText              /* color: inherit（跟时间文字同色） */
                    font.pixelSize: bar.timeFontSize              /* 非全屏 12 / 全屏 14（和时间文字同值） */
                    selectByMouse: true

                    background: Rectangle {
                        color: Qt.rgba(1, 1, 1, 0.2)              /* hsla(0,0%,100%,.2) */
                        border.width: 1                           /* border: 1px solid transparent */
                        border.color: "transparent"
                    }

                    /* 回车 = 提交（用户没要求，但输入框里按回车是本能；"点外面"那条路照旧） */
                    onAccepted: commitTimeEdit()
                }
            }

            /* ===============================================================
             * 5) 分段（视点）—— **追加在最后，也就是时间之后**（唯一允许的位置）
             *
             * 参考的 bottomLeftOrder 就是 ['prev','play','next','time','viewpoint']
             * （component/controls/LeftControls.ts:350，拼接顺序见 index.ts:1000-1049：
             *  时间那一段拼完之后，才 `if (this.config.viewpoint && viewpoints.length > 1)` 追加）。
             * 所以 [上一个][播放/暂停][下一个][时间] 这四块**一个像素都不动**：
             * 顺序、间距、显隐规则全部保持原样，视点按钮只能往后接。
             *
             * 【无数据时的行为】viewPoints 默认是空数组（`[]`，见本文件根对象），
             * 段数 ≤ 1 时这颗按钮 visible = false 且 width = 0 —— 和 prev/next 隐藏时
             * 同一套做法，Row 里不占位：**控制栏和现在长得一模一样**。
             * （参考那边更彻底：`viewpoints.length > 1` 为假时模板根本不拼进 DOM，
             *   index.ts:1047-1049。QML 里 Row 的子项必须在结构里，"隐藏 + 宽 0"是等价替代；
             *   浮层那边也串了同一条判据：面板没数据时不建列表项、每次数据变化还会主动 close，
             *   不会留一个看不见却点得到的浮层。）
             * =============================================================== */
            Item {
                id: viewPointButton

                /*
                 * 【出现条件】参考是 `this.config.viewpoint && viewpoints.length > 1`
                 * （index.ts:1047）：开关打开 **且** 段数严格大于 1。
                 * 我们这边没有那个总开关（应用层给空数组就等于关掉），只留段数这一条。
                 */
                readonly property bool available: viewPoints !== undefined && viewPoints !== null
                                                  && viewPoints.length > 1

                /* 当前段标题 = 按钮上「章节 · xxx」那一段（点列表项会换成被点的那段）。
                   初值取第 1 段 —— 对应参考模板里的 `viewpoints[0].pointText`（index.ts:1070）。 */
                property string currentTitle: ""

                visible: available
                /*
                 * 【宽度两档，都含 margin】参考 `.player-ctrl-viewpoint`：
                 *     flex-grow:1; flex-shrink:0; margin:0 10px;
                 *     max-width:240px; min-width:45px          （index.scss:1076-1086）
                 *
                 * QML 的 Row **不认识 margin**：外边距只能自己算进盒子里，否则左右各 10px
                 * 会凭空消失，"章节"两个字就贴到时间上。所以这里的 width **含左右各 10px**；
                 * 里面那层 content 再把这 10px 让出来（contentX / contentWidth）。
                 * max-width 240 量的也是**内容盒**，所以最大总宽 = 240 + 20 = 260。
                 *
                 * 【max-width 在 QML 里的等价写法】浏览器里它是"最多长到 240"的护栏：
                 * 自然宽度（文字 + 图标）和 240 取小 —— 长标题不会把这颗按钮撑到把右组挤走；
                 * min-width 45 是下限（内容盒最窄 45），所以里面的文字格永远 ≥ 27px。
                 *
                 * 【自然宽度必须用 TextMetrics 量，**不能**读 viewPointTextContent.implicitWidth】
                 * 那个 Text 的 width 是 `textWidth = contentWidth − 图标`、contentWidth 又来自
                 * 本按钮的 width，而它带 `elide: ElideRight`（宽度受限的排版）—— 于是
                 *     button.width → textWidth → Text.width → Text.implicitWidth → button.width
                 * 成了一条自引用链：隐式宽度会被"省略之后的宽度"顶替，整颗按钮（连同它上面那条
                 * hover 触发区）就被钉在最小宽度那一档 —— 触发区跟着缩到 45px 宽，
                 * 鼠标停在"章节 · xxx"那段字上时判不进去 = 用户报的"鼠标移上去没反应"。
                 * TextMetrics 只按字体 + 字符串算 advanceWidth，**和任何 width 无关**，没有这条链。
                 */
                readonly property int iconSide: bar.iconSize

                TextMetrics {
                    id: viewPointTitleMetrics

                    text: "章节 · " + viewPointButton.currentTitle
                    font.pixelSize: bar.qualityFontSize
                }

                width: visible
                       ? QtPlayerTheme.viewPointMarginH * 2
                         + Math.max(QtPlayerTheme.viewPointMinWidth,
                                    Math.min(QtPlayerTheme.viewPointMaxWidth,
                                             viewPointTitleMetrics.advanceWidth + iconSide))
                       : 0
                /*
                 * 【高度 = 那一行"行盒"的高度，和左边那格"时间"完全一样】
                 * 时间那一格的高度就是 `bar.btnLineHeight`（非全屏 22 / 全屏 32，LeftControls:621），
                 * 而左组其它按钮用 `bar.btnHeight`（43）自己再把内容居中在 32 的行盒里。
                 * 这一颗如果跟着用 btnHeight，全屏下它的内容中心就在 43/2 = 21.5，
                 * 而时间的文字中心在 32/2 = 16 —— **比时间低 5.5px**，用户看到的
                 * "全屏下垂直不居中"就是这一条。取 btnLineHeight 之后：
                 *     按钮高 = 行盒高 → 内容盒 y = (高 − 高)/2 = 0 → 文字/图标中心 = 16 = 时间的中心，
                 * 两档（22 / 32）都自动对齐，不需要再加任何偏移。
                 */
                height: bar.btnLineHeight

                /* 【这里没有 anchors.left】位置由外面的 Row 按书写顺序决定（第五位 = 时间之后） */

                /* 面板底边到按钮盒底边的距离：非全屏 41 / 全屏 74
                   （index.scss:1131 与 2621-2623；右边的设置浮层用的是同一个 bar.menuBottom，
                     见 RightControls.qml:244-254 的说明）。 */
                readonly property int panelBottom: bar.fullscreen
                                                   ? QtPlayerTheme.viewPointPanelBottomFs
                                                   : QtPlayerTheme.menuBottom

                /* ------------- 内部几何（都在按钮坐标系里手算，理由见文件头那段说明） ------------- */

                /* 内容盒（= CSS 里的按钮盒）：把左右各 10px 的 margin 让出来 */
                readonly property real contentX: QtPlayerTheme.viewPointMarginH
                readonly property real contentWidth: Math.max(0, width - QtPlayerTheme.viewPointMarginH * 2)

                /*
                 * 面板的**水平位置**：以「章节」按钮为中心展开（用户要求）。
                 *
                 * 【与参考的差别】参考写的是 `left: -36px`（index.scss:1134，相对按钮**内容盒**左边缘，
                 * 见 QtPlayerTheme.viewPointPanelLeft）——**用户要求改成"相对按钮水平居中"**，
                 * 所以这里覆盖参考值，并按"贴边不越界"夹一下。
                 *
                 * x 是**按钮坐标系**里的值，所以"不越出播放器边界"要换算一次：
                 *     面板在控制栏里的左边缘 = 按钮在控制栏里的 x（buttonXInBar）+ x
                 * 把左边缘夹到 [0, 控制栏宽 − 面板宽]，就是下面这个
                 *     Math.max(-buttonXInBar, Math.min(居中值, bar.width − 面板宽 − buttonXInBar))
                 * —— 越界时面板贴住边界停住（左/右都算），不会跑到播放器外面。
                 */
                readonly property real buttonXInBar: bar ? viewPointButton.mapToItem(bar, 0, 0).x : 0
                readonly property real panelX: {
                    var centred = viewPointButton.width / 2
                                  - QtPlayerTheme.viewPointPanelWidth / 2

                    if (!bar)
                        return centred

                    return Math.max(-buttonXInBar,
                                    Math.min(centred,
                                             bar.width - QtPlayerTheme.viewPointPanelWidth
                                             - buttonXInBar))
                }

                /* 文字那一格的宽度 = 内容盒 − 图标
                   （参考写的是 width: calc(100% - 12px)（index.scss:1111），但在 flex 行里
                     真正占着位置的是那颗图标（index.scss:1114-1126），文字让位多少看图标。
                     这里按图标实际宽度让位，保证文字和图标**不重叠**、图标也永远在格子右边缘上。
                     图标边长 = iconSide（bar.iconSize），见上面那条说明。） */
                readonly property real textWidth:
                    Math.max(0, contentWidth - iconSide)

                /*
                 * 面板几何（给浮层和它的 hover 判据一起用，只算一遍）
                 *
                 * 参考里面板是按钮盒里的 absolute 元素，所以左边能跑到按钮外面：
                 *   left: -36px（index.scss:1134）  → 相对**内容盒**左边缘（我们的 contentX 处）
                 *   bottom: 41px / 74px（index.scss:1131 / 2621-2623）→ 面板底边离盒底边
                 * 我们的浮层 `parent` 就是这个按钮（见下面 T.Popup），坐标系和按钮一致，
                 * 所以 x/y 直接就是这两条（y = 盒高 − bottom − 浮层高）。
                 *
                 * 【高度为什么自己算，不绑 contentItem 的 implicitHeight】避坑清单第 3 条：
                 * 浮层会去**设置** contentItem 的尺寸，height 绑 contentItem 就是绑定循环、
                 * 绑定会被 QML 断掉，尺寸永远停在初始值。这里读的是**视点数据条数**这个独立来源
                 * （和 DanmakuBar 读 pageOffset、音量浮层读常量是同一个道理）。
                 */
                readonly property real contentLeft: contentX
                readonly property real panelHeight: Math.min(panelRequestedHeight,
                                                             QtPlayerTheme.viewPointPanelMaxHeight)
                readonly property real panelRequestedHeight:
                    (viewPoints && viewPoints.length > 0)
                    ? viewPoints.length * QtPlayerTheme.viewPointItemHeight : 0

                /*
                 * "缝"那一块的判据：**浮层整块 + 浮层到按钮之间那道缝**，横向盖住浮层整宽。
                 * 【为什么要有】避坑清单第 9 条：QML 的 hover 只看几何、不看子树 ——
                 * 鼠标从按钮往浮层上走要经过 panelBottom（41 / 全屏 74）那段缝，中间没有任何
                 * 元素接得住，没有这一块就会在缝里被判成"离开"→ 浮层关掉。
                 * 【高度怎么算 —— 这里错过一次】这一块是"浮层 + 到按钮顶边为止的那道缝"，
                 * 所以高度 = `-panelY`（= panelBottom + 浮层高 − 按钮高）：
                 * 上边缘落在浮层顶边、下边缘正好落在**按钮顶边**（y = 0），
                 * 和按钮自己的触发区**不重叠**（第 11 条：重叠就会互相抢 hover、面板疯狂闪）。
                 * 原来写的是 `panelHeight + panelBottom` —— 那比需要的**多出整颗按钮的高度**
                 * （41 + 150 = 191 > 169），这块的下半截整个压在按钮那一格上，
                 * 与上面"不重叠"这句自相矛盾，也正是面板开着时判据互相打架的来源之一。
                 */
                readonly property real hoverZoneHeight: -panelY

                /*
                 * 浮层所在的 y（按钮坐标系）—— 浮层和它的"缝"判据共用这一个值：
                 * 浮层底边离按钮盒底边 panelBottom，所以 y = 盒高 − panelBottom − 浮层高。
                 * 单独抽出来是因为**判据那一块不能去读浮层的 y**（DanmakuBar.qml:263-271 记过：
                 * 读早了浮层的 width/height 还没算好，会得到 0/NaN，判据退化成一条缝）。
                 */
                readonly property real panelY: height - panelBottom - panelHeight

                /*
                 * 鼠标在不在**这一块**上：按钮本体 **或** 缝+浮层区 **或** 浮层自己。
                 * 三处取或，和现有浮层的容错定时器判据一模一样
                 * （DanmakuBar.qml:252-253、RightControls.qml:348-349）：
                 * 少任何一个来源都会误关；把某一块铺大去"兼顾"另一块就会互相抢 hover（清单第 12 条）。
                 */
                readonly property bool pointerInViewPointArea: viewPointHoverZone.containsMouse
                                                              || viewPointPanelHoverZone.pointerIn
                                                              || viewPointPanel.pointerIn

                /*
                 * 鼠标是不是正压在这一行列表项上 —— 面板里**不能用 MouseArea 的 hoverEnabled**
                 * （会抢浮层的判据，清单第 6 条），所以照 SubtitlePanel.pointerOn()
                 * （SubtitlePanel.qml:74-85）的做法：拿浮层喂进来的鼠标位置 mapToItem 到这一行再判。
                 * 位置无效（-1，鼠标不在浮层里）或那一行还没建出来时一律 false。
                 */
                function viewPointMouseAt(item) {
                    /* 面板里**没有**接受 hover 的 MouseArea（清单第 5/6 条），判据来自
                       background 上那个**被动** HoverHandler：hovered = 指针在不在面板里，
                       point.position 在它父项（背景矩形）的坐标系里。 */
                    if (!item || !viewPointPanelHover.hovered)
                        return false

                    var p = viewPointPanelBgRect.mapToItem(item, viewPointPanelHover.point.position.x,
                                                           viewPointPanelHover.point.position.y)

                    return p.x >= 0 && p.x <= item.width && p.y >= 0 && p.y <= item.height
                }

                /* ---------------- 开合状态（hover，两个 300ms 定时器） ----------------
                 *
                 * 参考：按钮的 mouseenter → show、mouseleave → hide，两边各延迟 300ms
                 * （controls/index.ts:360-369 挂事件、:1729-1750 里 `rafTimeout(..., 300)` 到点切
                 *  `state-show`，样式在 index.scss:1027-1032）。
                 *
                 * 【写法照项目现有浮层，不另搞一套】`T.Popup` + 自己维护 `pointerIn` +
                 * "触发区 / 缝区 / 浮层"三处取或 + 容错定时器 —— 和音量/倍速/清晰度/弹幕设置面板
                 * 完全一致（RightControls.qml:340-402、DanmakuBar.qml:241-300，
                 * 以及 docs/QML-POPUP-HOVER-PITFALLS.md 的模板）。三块区域**互不重叠**是硬要求：
                 * 谁和谁重叠就会互相抢 hover，出现"打开→抢走→判定离开→关掉→又打开"的闪烁。
                 *
                 * 【★ 这一版整块换掉了：开合照抄 DanmakuBar 的齿轮浮层】
                 * 之前这里是两个 300ms 定时器（enter 起 show、exit 杀 show 再起 hide），
                 * 那种"延迟弹出"的写法有两个能把面板永久卡死的断点：hover 抖一次就再也不弹、
                 * 数据重发时被 viewPointReset 清掉。现在**不再自创**，逐条照抄本项目里用户日常在用的
                 * 那套（controls/DanmakuBar.qml 的「齿轮 → 弹幕设置浮层」）：
                 *   * 触发区 = 触发元素本身，onEntered **立即 open()**，onExited 只 restart 关闭定时器
                 *     —— DanmakuBar.qml:223-231（本项目所有能用的浮层都是这个节奏，没有"进入延迟"）
                 *   * 唯一的定时器是 **100ms 容错关闭**，到点"三处取或"都不在才关 —— DanmakuBar.qml:241-256
                 *   * "缝 + 浮层"合成一块几何区域判 hover —— DanmakuBar.qml:258-300
                 *   * T.Popup：明确 parent + 由常量推出的 x/y + closePolicy + pointerIn 维护
                 *     —— DanmakuBar.qml:302-346 / :354-366
                 * 与它的唯一差别：浮层背景那层判据用**被动 HoverHandler**（不独占 hover，
                 * 面板上的点击照旧落到列表项上；本项目 SettingsPanel.pointerIn 就是这套写法）。
                 */

                /* 段数 / 内容变了 → 收起面板 + 标题回到第 1 段。
                   【为什么要收敛状态】viewPoints 是外面随时可换的数据（换片子只是其中一种）：
                   不拉回初值就会出现"按钮还写着上一部片子的段名"或"面板开着、
                   里面已经换成另一批段"这类状态残留。

                   【★ 这里以前写的是 `onViewPointsChanged: viewPointReset()` —— 那是让"点视频卡片
                   打不开播放窗口"的根因】`viewPoints` 是**根对象**的属性（本文件 :68），而这段代码
                   在子对象（按钮）里；QML 的隐式信号处理器只能给**本对象自己声明**的属性挂，
                   写在子对象上会报：
                       Cannot assign to non-existent property "onViewPointsChanged"
                   而且它不是局部错误 —— LeftControls 加载失败会连锁
                       LeftControls → PlayerControlBar → PlayerView → Main 全部 unavailable，
                   结果就是点卡片建不出播放窗口（实测 stderr 链就是这么打的）。
                   数据变化的收敛改由**根对象**上的处理器统一触发（根对象的属性变化只有它自己能收）。 */

                function viewPointReset() {
                    currentTitle = (viewPoints && viewPoints.length > 0 && viewPoints[0])
                                   ? ("" + viewPoints[0].content) : ""

                    /* 【数据重发时不要"当着用户的面"把面板收掉】
                       viewPoints 是外面随时会重发的数据（Main.qml 在播放上升沿 / 基址探测成功时
                       都会重新赋一次，见 Main.qml 里 playing 的上升沿处理器）。
                       这里只做两件事：换标题 + **指针不在按钮/面板上时**才把面板收掉；
                       指针在的话一个状态都不动（原来那句"无条件 stop 两个定时器 + close()"
                       会把用户的 hover 意图一起清掉，表现就是"hover 是死的"）。 */
                    if (!pointerInViewPointArea)
                        viewPointPanel.close()
                }

                /* 点列表项：跳转 + 换按钮文案（参考 controls/index.ts:830-838，逐条对应）
                 *   ① `trigger('change-currentTime', Number(data-time))` —— data-time 写进去的
                 *      是 `viewpoint.startTime`（index.ts:1089），**秒**；我们的 `{from,to,content}`
                 *      里 from 就是它。秒 → 毫秒只在这一处做；
                 *   ② 按钮文案换成被点那一段的标题（index.ts:835）；
                 *   ③ **只 seek，不做"限制在这段内播放"**（用户明确要求，参考也没有这道限制）；
                 *   ④ 列表项**不做当前项高亮**（照参考：li 只有 :hover 一条样式，
                 *      index.scss:1149-1163，点完不换类、不加任何标记）。 */
                function viewPointChoose(index) {
                    if (!viewPoints || index < 0 || index >= viewPoints.length)
                        return

                    var item = viewPoints[index]

                    if (!item)
                        return

                    currentTitle = ("" + item.content)
                    /* 秒 → 毫秒；出口只有一个信号，父对象（PlayerControlBar）负责转给播放器 */
                    viewPointSelected(Math.round(viewPointToNumber(item.from) * 1000))
                }

                /*
                 * 按钮文案跟着**播放位置**走（参考 updateSegmentedProgress 里那一段：
                 * 播放时间落进哪一段，就把按钮文字换成 `'章节 · ' + pointText`，
                 * controls/index.ts:1601-1605）。
                 *
                 * 【少了这一段会怎样】标题只会在"数据变了"和"点过某一项"时更新 ——
                 * 片子播到第 3 章、按钮上还写着第 1 章，和进度条上的蓝色分段对不上。
                 *
                 * 判据照参考：原始区间 `from <= t < to`（和悬停气泡里那个标题同一套，
                 * 不做分段填充用的 ±adjustment 内缩）；播放位置是**毫秒**、视点是**秒**，
                 * 所以除以 1000 再比。只在标题真的变了时才赋值，免得每帧都触发重绑。
                 */
                Connections {
                    target: bar ? bar.player : null

                    function onPositionChanged() {
                        if (!viewPointButton.available || !bar.player)
                            return

                        var t = bar.player.position / 1000

                        for (var i = 0; i < viewPoints.length; ++i) {
                            var point = viewPoints[i]

                            if (!point)
                                continue

                            var from = viewPointToNumber(point.from)
                            var to = viewPointToNumber(point.to)

                            if (from <= t && t < to) {
                                var title = (point.content === undefined || point.content === null)
                                            ? "" : ("" + point.content)

                                if (viewPointButton.currentTitle !== title)
                                    viewPointButton.currentTitle = title

                                return
                            }
                        }
                    }
                }

                /*
                 * "缝"那一块 + 浮层自己那一块，合成一个判据来源（避坑清单第 9/12 条）。
                 *
                 * 【几何用常量算，不去读浮层的 x/y】DanmakuBar.qml:263-271 记过这个坑：
                 * 浮层的坐标依赖它自己的 width/height 先算好，读早了会得到 0/NaN，
                 * 整块判据退化成一条缝（现象是"鼠标移到面板下半部分就被判离开、面板自己关掉"）。
                 * 这里直接由按钮几何推：面板在按钮坐标系里位于
                 *     x = contentLeft − 36、y = height − panelBottom − 浮层高，
                 * 所以这块区域就是"面板整块 + 缝 + 到按钮顶边"。
                 *
                 * 【几何核对】这块区域：
                 *     y      = 盒高 − panelBottom − 浮层高   （= 浮层顶边）
                 *     height = 浮层高 + panelBottom           （浮层 + 中间那道缝）
                 *   → 下边缘 y + height = 盒高 = **按钮底边**：缝的底正好接在按钮上边缘，
                 *     上边缘接在浮层顶边，中间不留死区。
                 *   按钮自己的触发区是**整颗按钮**（anchors.fill，见下面那段），它的 y 从 0 起算，
                 *   而这一块的 y + height = 0 —— 两块**只相邻、不重叠**，不会互相抢 hover
                 *   （第 11 条：重叠会形成开/关死循环，表现为面板疯狂闪）。
                 */
                Item {
                    id: viewPointPanelHoverZone

                    parent: viewPointButton                    /* 同 DanmakuBar.qml:261 的写法 */
                    /* x/width 和面板**用同一个来源**（viewPointButton.panelX）—— 用户要求面板相对按钮
                       居中，缝区必须跟着走，否则鼠标从按钮往上走到面板上会被判成"离开"。 */
                    x: viewPointButton.panelX
                    y: viewPointButton.height - viewPointButton.panelBottom - viewPointButton.panelHeight
                    width: QtPlayerTheme.viewPointPanelWidth
                    /* 高度 = −y：覆盖"浮层整块 + 到按钮顶边为止那道缝"，下边缘正好落在按钮顶边，
                       和按钮自己的触发区只相邻、不重叠（DanmakuBar.qml:258-300 那块同款）。 */
                    height: -y
                    /* 浮层没开时整块不显示、也不接 hover（浮层关着时它悬在控制栏一段空白上，
                       开着的话那里点不到东西，还会盖住进度条下面一截）。 */
                    visible: viewPointPanel.opened
                    property bool pointerIn: false

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onEntered: {
                            viewPointPanelHoverZone.pointerIn = true
                            viewPointCloseTimer.stop()
                        }
                        onExited: {
                            viewPointPanelHoverZone.pointerIn = false
                            viewPointCloseTimer.restart()
                        }
                    }
                }

                /* 按钮本体的触发区。判据用 MouseArea.containsMouse，**不用 HoverHandler**：
                   同一区域内 hoverEnabled 的 MouseArea 会把 hover 从 HoverHandler 手里拿走
                   （本文件根对象那段 anyHover / pointerInside 的说明，PlayerControlBar 里
                    pointerInside 那段也写着"实测过"）；项目里现有浮层的触发区清一色是
                   `anchors.fill` / 定尺寸的 MouseArea（DanmakuBar.qml:213-232、
                   RightControls.qml:354-371）。
                   hover 被这一层吃掉之后外面的控制栏容器就收不到进出了，所以根对象上透出
                   `viewPointHovered`，控制栏的 pointerInside 必须带上它（否则鼠标停在视点按钮上
                   会被判成"离开控制栏"→ 整条控制栏自己收起来；浮层那一侧同理）。 */
                MouseArea {
                    id: viewPointHoverZone

                    /* 【范围 = 整颗按钮】参考挂 mouseenter/mouseleave 的是**整个**
                       `.player-ctrl-viewpoint` 元素（controls/index.ts:361-367 里 querySelector 拿到的
                       就是按钮本身），所以这里 anchors.fill: parent。
                       以前写的是内缩到 contentX/contentWidth —— 那版还有第二个坑：触发区宽度来自
                       按钮宽度，而按钮宽度早先又是从"被 elide 的文字的 implicitWidth"反推出来的
                       （自引用链，见 width 那段说明）；两者一起塌下去，就成了
                       "鼠标停在章节文字上没反应"。 */
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    /* 【立即打开】照抄 DanmakuBar.qml:226-231（那套是用户日常在用的）：
                       进入只做"取消关闭 + open()"，离开只 restart 关闭定时器，
                       要不要关由 100ms 后在定时器里那次判据决定。 */
                    onEntered: {
                        viewPointCloseTimer.stop()
                        viewPointPanel.open()
                    }
                    /* 【兜底入口：点一下也开】用户要求 hover 是首选、点击是保险。
                       只 open、**不做 toggle** —— 所以点按钮永远不会把已经开着的面板关掉
                       （和参考一致：参考按钮上只有 mouseenter/mouseleave、没有 click 切换）。
                       面板已经开着时 open() 是幂等的。 */
                    onClicked: {
                        viewPointCloseTimer.stop()
                        viewPointPanel.open()
                    }
                    onExited: viewPointCloseTimer.restart()
                }

                /* 【唯一的定时器：100ms 容错关闭】照抄 DanmakuBar.qml:241-256
                   （interval 100 也是照抄，不是等待时间，只是"从按钮挪到浮层上"那段路的容错）。
                   到点把三处判据取或看一遍：触发区 / 缝区 / 浮层，都不在才关。 */
                Timer {
                    id: viewPointCloseTimer

                    interval: 100
                    repeat: false
                    onTriggered: {
                        if (!viewPointHoverZone.containsMouse && !viewPointPanelHoverZone.pointerIn
                                && !viewPointPanel.pointerIn)
                            viewPointPanel.close()
                    }
                }

                /* ---------------- 按钮内容：`章节 · <当前段标题>` + 18px 右箭头 ---------------- */

                Item {
                    id: viewPointContent

                    /* 垂直居中；横向让开左右各 10px 的 margin（见 width 那段说明） */
                    x: viewPointButton.contentX
                    y: (viewPointButton.height - height) / 2
                    width: viewPointButton.contentWidth
                    height: bar.btnLineHeight
                    /* 参考 `.player-ctrl-viewpoint-content { overflow: hidden }`（index.scss:1095）：
                       文字本身已经按省略号截断，这里再裁一道兜底。 */
                    clip: true

                    /* 标题文字：`.player-ctrl-viewpoint-text`（index.scss:1105-1112）
                       —— text-overflow: ellipsis + white-space: nowrap + text-align: left */
                    Text {
                        id: viewPointTextContent

                        x: 0
                        y: (parent.height - height) / 2
                        /* 内容盒宽度 − 图标 18px（见 textWidth 的说明）。
                           width 必须给死：只靠 implicitWidth 是没有上限的，标题多长按钮就多宽。 */
                        width: viewPointButton.textWidth
                        height: parent.height
                        text: "章节 · " + viewPointButton.currentTitle
                        color: viewPointButton.pointerInViewPointArea
                               ? QtPlayerTheme.viewPointTextHover
                               : QtPlayerTheme.viewPointText
                        /* 【字号：跟控制栏右边那套按钮同一个 token】用户要求"全屏和非全屏的文字大小
                           都要和右边那几个按钮的文字一样"。
                           参考这里是 `font-size: 14px` 固定值（.player-ctrl-viewpoint-content，
                           index.scss:1094，全屏也没有再覆盖），而控制栏右组的文字是
                           非全屏 14 / 全屏 16（bar.qualityFontSize，PlayerControlBar.qml:338 引
                           index.scss:2641-2643）—— 所以**这里是有意偏离参考的固定 14**，
                           全屏跟着涨到 16，和右组按钮对齐。 */
                        font.pixelSize: bar.qualityFontSize
                        /* 行内省略号（参考的 text-overflow: ellipsis + white-space: nowrap） */
                        elide: Text.ElideRight
                        wrapMode: Text.NoWrap
                        verticalAlignment: Text.AlignVCenter
                    }

                    /* 右箭头图标容器：位置仍照参考（.player-ctrl-viewpoint-icon 贴在内容盒右边缘，
                       index.scss:1114-1126），但**边长跟控制栏那套按钮走**：iconSide = bar.iconSize
                       （非全屏 22 / 全屏 28）。用户要求"图标也要和右边那几个按钮一样"，
                       参考里它是固定 18px —— 这是有意偏离（只大不小，仍然垂直居中）。 */
                    Item {
                        id: viewPointIconBox

                        x: parent.width - width
                        y: (parent.height - height) / 2
                        width: viewPointButton.iconSide
                        height: viewPointButton.iconSide

                        Image {
                            anchors.centerIn: parent
                            width: viewPointButton.iconSide
                            height: viewPointButton.iconSide
                            source: "../assets/images/dm/arrow.svg"
                            fillMode: Image.PreserveAspectFit
                            /* SVG 按目标尺寸栅格化，免得被放大后再采样糊掉 */
                            sourceSize.width: viewPointButton.iconSide
                            sourceSize.height: viewPointButton.iconSide
                            smooth: true
                            opacity: QtPlayerTheme.viewPointIconOpacity
                            enabled: false
                        }
                    }
                }

                /* ---------------- 展开面板：T.Popup 浮层（hover 才显示；点列表项跳转） ----------------
                 *
                 * 【用 T.Popup，不自己画】浮层统一用 QtQuick.Templates 的 T.Popup +
                 * 自己维护 `pointerIn` + closePolicy + "触发区 / 缝 / 浮层"三处取或 + 容错定时器
                 * —— 音量/倍速/清晰度/字幕/选集/弹幕设置面板全是这一套
                 * （RightControls.qml:340-402、427-494、DanmakuBar.qml:241-300 与 :302-388），
                 * 完整清单见 docs/QML-POPUP-HOVER-PITFALLS.md。这里照抄那个顺序写，**不另搞一套**。
                 *
                 * 元素层级仍然照参考的模板（index.ts:1065-1085）：
                 *   .player-ctrl-viewpoint-menu-wrap（面板）→ ul.player-ctrl-viewpoint-menu
                 *     → li.player-ctrl-viewpoint-menu-item（列表项）
                 *
                 * 【两个已知坑，这里都按清单避开了】
                 *   * `background` 必须**显式给尺寸**（T.Popup 是纯 C++ 类型，不会撑 background）：
                 *     否则里面那层判据 MouseArea 是 0×0，鼠标移上去永远判成"不在里面"→一移上去就关
                 *     （清单第 1 条，RightControls.qml:465-472 也记着）。
                 *   * 浮层里**只能有一个接受 hover 的东西**：所以面板里那些列表项的 MouseArea
                 *     **一律不写 hoverEnabled**（清单第 6 条，SubtitlePanel.qml:28-31 同样记着），
                 *     hover 变色改用"鼠标位置 + mapToItem"自己判（见 viewPointMouseAt）。
                 */
                T.Popup {
                    id: viewPointPanel

                    /* 【鼠标在不在浮层里】清单第 2 条要求自己维护这个判据（框架的 Popup 确实
                       没有可用的 hovered）。这里由 background 上那个**被动 HoverHandler** 来改它
                       （不是 hoverEnabled 的 MouseArea，理由见 background 那段），并**在 onClosed
                       里清零** —— 不清零会出现"关了但状态还是 true、下次判据失真"（清单第 2 条原话）。 */
                    property bool pointerIn: false

                    onClosed: pointerIn = false

                    /* 浮层挂在**按钮**上：x/y 就是"按钮坐标系里的位置"（和现有浮层一样用 parent）。
                       宽度 210（index.scss:1141）；高度 = 条数×30 再夹到 max-height 320
                       （index.scss:1135，超过是**裁掉**不是滚动）。 */
                    parent: viewPointButton
                    width: QtPlayerTheme.viewPointPanelWidth
                    height: viewPointButton.panelHeight
                    /* 水平位置 = 按钮中心（用户要求；参考是 left:-36px，见 viewPointButton.panelX 那段） */
                    x: viewPointButton.panelX
                    y: viewPointButton.height - viewPointButton.panelBottom - height

                    padding: 0
                    /* 照抄 DanmakuBar.qml:344 的 closePolicy（用户日常在用那套）：
                       Esc / 点面板外面关；不做自动关（面板里全是可点的列表项）。 */
                    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutside
                    enter: Transition {}
                    exit: Transition {}

                    /* 面板高过 max-height 时把超出的列表项裁掉（参考的 overflow:hidden）。
                       不加这个的话内容是"画到面板外面"而不是被切掉。 */
                    clip: true

                    /* 浮层自己那层判据：**严格等于浮层矩形**（别铺大，铺大就会压在触发区上
                       抢 hover，形成"开→抢→判离开→关→又开"的死循环，清单第 11 条）。
                       【这一层用**被动 HoverHandler**，不再用 hoverEnabled 的 MouseArea】
                       清单第 5/6 条：同一位置上只有一个元素收得到 hover，MouseArea 一旦写了
                       hoverEnabled 就是"独占"；而 HoverHandler 是**被动**的 —— 只观察、不参与独占，
                       点击也照旧落到下面的列表项上。这样面板上既拿得到"指针在不在面板里"、
                       又不会去抢触发区/列表项的 hover（本项目 SettingsPanel.pointerIn 就是这套写法）。
                       列表项自己的 MouseArea **一律不写 hoverEnabled**（清单第 6 条），
                       hover 底色由 viewPointMouseAt() 用这个 handler 的坐标算。 */
                    background: Rectangle {
                        id: viewPointPanelBgRect

                        width: viewPointPanel.width
                        height: viewPointPanel.height
                        color: QtPlayerTheme.viewPointPanelBg    /* hsla(0,0%,8%,.9) */
                        radius: QtPlayerTheme.viewPointPanelRadius

                        HoverHandler {
                            id: viewPointPanelHover

                            /* 和 DanmakuBar.qml:363-367 那层判据同一个动作：
                               进面板 → pointerIn = true + 取消关闭；离开 → pointerIn = false + 安排关闭 */
                            onHoveredChanged: {
                                viewPointPanel.pointerIn = hovered

                                if (hovered)
                                    viewPointCloseTimer.stop()
                                else
                                    viewPointCloseTimer.restart()
                            }
                        }
                    }

                    contentItem: Column {
                        id: viewPointItemList

                        /* `.player-ctrl-viewpoint-menu` 就是个 margin/padding 全 0 的 ul
                           （index.scss:1144-1147），所以各项从面板顶边开始、一项挨一项。 */
                        spacing: 0

                        Repeater {
                            model: viewPointButton.available ? viewPoints : null

                            delegate: Rectangle {
                                id: viewPointItem

                                required property int index
                                required property var modelData

                                width: viewPointItemList.width
                                height: QtPlayerTheme.viewPointItemHeight
                                /* 【没有"当前项"高亮】参考的 li 只有 :hover 一条样式
                                   （index.scss:1149-1163），点完不换类、不加标记，
                                   所以这里除 hover 底色外**不做任何选中态**。
                                   hover 底色用鼠标位置算（面板里不能有接受 hover 的 MouseArea）。 */
                                color: viewPointButton.viewPointMouseAt(viewPointItem)
                                       ? QtPlayerTheme.viewPointItemHoverBg
                                       : "transparent"

                                Text {
                                    /* 左右各 20px（padding: 0 20px，index.scss:1155） */
                                    x: QtPlayerTheme.viewPointItemPaddingH
                                    y: 0
                                    width: Math.max(0, parent.width
                                                       - QtPlayerTheme.viewPointItemPaddingH * 2)
                                    height: parent.height
                                    text: viewPointItem.modelData
                                          ? ("" + viewPointItem.modelData.content) : ""
                                    color: QtPlayerTheme.menuText
                                    font.pixelSize: QtPlayerTheme.viewPointItemFontSize
                                    /* 行高 30 靠垂直居中表达；超出省略号（index.scss:1151-1158） */
                                    elide: Text.ElideRight
                                    wrapMode: Text.NoWrap
                                    verticalAlignment: Text.AlignVCenter
                                }

                                MouseArea {
                                    id: viewPointItemArea

                                    anchors.fill: parent
                                    /* 【不写 hoverEnabled】一写就会把浮层那层判据的 hover 抢走
                                       → 鼠标压到列表项上时面板被判"鼠标已离开"而自己关掉
                                       （清单第 6 条，即"移到面板上就关"那个现象）。 */
                                    cursorShape: Qt.PointingHandCursor   /* cursor: pointer（index.scss:1150） */
                                    onClicked: viewPointButton.viewPointChoose(viewPointItem.index)
                                }
                            }
                        }
                    }
                }
            }
        }
}
