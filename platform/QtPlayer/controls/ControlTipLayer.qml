// ===========================================================================
// 控制栏按钮的 tooltip 层（鼠标移入按钮 300ms 后出现的小黑条提示）
//
// 界面与交互**逐条对照参考实现** front/hili-player/packages/player：
//   * 视觉规格 —— tooltip.scss 的 .nova-player-tooltip-item：
//       黑底 #000、白字 12px、行高 1.5、内边距 6/8、圆角 2px、最大宽 18em(=216px)；
//       transition: transform 0.3s + opacity 0.3s linear；
//       隐藏态位移 translate(0px, 5px)（Tooltips.ts 的 defaultTransformOf：
//       feedback 按钮是 -5px，其余都是 +5px —— 本应用没有 feedback 按钮，全部 +5px）。
//   * 位置数学 —— Tooltips.ts 的 calculateTipPosition：
//       水平：tip 中心对齐按钮中心；垂直：danmaku_switch 在按钮上方 8px，
//       其余在上方 23px（普通）/ 32px（全屏）；水平夹紧在容器内、左右各留 12px。
//   * 交互节奏 —— Controls.ts 的 initTooltip / SendBar.ts 的 showSwitchTip：
//       mouseover → 300ms 延迟显示；mouseout → 立即隐藏（渐隐）；
//       同一按钮上去重、换按钮时撤销旧定时器并收起旧 tip。
//   * 动态文案 —— PlayerDocker.ts 里全屏切换时 updateTip("ctrl:fullscreen", …)：
//       非全屏“进入全屏 (f)”、全屏“退出全屏 (f)”（显示期间切换也要跟着变）。
//
// 【为什么做成“一个气泡 + 按名路由”而不是每颗按钮自己拖一个】
// 参考就是这套结构：所有提示项共用一个 Tooltips 组件，按钮只报名字。
// 好处照搬到 QML：300ms 定时器只有一份（换按钮天然“撤销旧的再等新的”）、
// 水平夹紧/垂直偏移规则集中在一处、以后加按钮只要挂两行 onEntered/onExited。
//
// 【渐隐期间文案/尺寸不要塌】参考 CSS 隐藏只是 opacity/transform 变，DOM 文本不动，
// 所以气泡在渐隐的 300ms 里尺寸位置稳定。QML 里如果文案绑定 activeName，
// activeName 一清空文本就变空、气泡当场缩成一条边 —— 视觉上是“闪一下再消失”。
// 因此文案用普通属性 currentText，只在“显示那一刻”和“全屏状态变化时”显式更新，
// 收起时不清；位置同理，reposition() 只在显示时调用，收起时保留旧坐标。
// ===========================================================================
import QtQuick

Item {
    id: tipLayer

    /* ---- 从外面传进来的依赖 ---- */

    /* 控制栏本体（PlayerControlBar 根）：
       读 fullscreen（垂直偏移 23/32 与全屏动态文案）、shown（控制栏收起时一起收）、
       width（水平夹紧的“容器”宽，参考是 .nova-player-container，这里就是控制栏宽） */
    property var bar: null

    /* ---- 状态 ---- */

    /* 正在显示的按钮名（"" = 没有在显示）。渐隐动画进行中它已经是 ""——
       与参考一致：hideTooltip 就是把名字从 activePositionsSig 里删掉，渐隐交给 CSS。 */
    property string activeName: ""

    /* 300ms 等待中的按钮名（"" = 没有在等）。
       与参考 initTooltip 的 inTimer 对应：mouseover 起 300ms、mouseout 撤销。 */
    property string pendingName: ""

    /* 目标按钮（位置计算的锚点）。跨组件传进来的 Item 引用（LeftControls /
       RightControls / DanmakuBar 里那几颗），坐标用 mapToItem 换算，不依赖层级。 */
    property var targetItem: null

    /* 当前气泡文案。★ 不绑定 activeName（见文件头【渐隐期间文案/尺寸不要塌】）。
       只在两处显式更新：显示那一刻、显示中全屏状态翻掉时（参考 updateTip 的行为）。 */
    property string currentText: ""

    /* -----------------------------------------------------------------------
     * 文案表（参考 Tooltips.ts 的 DEFAULT_TOOLTIP_ITEMS；
     * QtPlayer 只做了参考里实际存在对应按钮的四项 —— pip/宽屏/网页全屏/反馈/
     * 编码这几颗按钮本应用没有，跳过）
     * ----------------------------------------------------------------------- */
    function textOf(name) {
        if (bar === null)
            return ""

        if (name === "ctrl:fullscreen")
            return bar.fullscreen ? qsTr("退出全屏 (f)") : qsTr("进入全屏 (f)")

        if (name === "ctrl:prev")
            return qsTr("上一个 ([")

        if (name === "ctrl:next")
            return qsTr("下一个 (])")

        if (name === "danmaku_switch")
            return qsTr("关闭弹幕 (d)")

        return ""
    }

    /* -----------------------------------------------------------------------
     * 对外 API（按钮的 MouseArea 挂钩只调这两个；
     * PlayerControlBar 上有同名转发函数 requestTipShow / requestTipHide）
     * ----------------------------------------------------------------------- */

    /*
     * 请求显示某颗按钮的 tip（对应参考 mouseover 分支）。
     *   去重：这颗按钮正在显示 / 正在等待 → 直接返回（按钮内部子元素间移动
     *         在 QML 里本来就不会重复触发 entered，这里再兜一层，语义照 hoveredTipBtn）。
     *   换按钮：立即收起旧 tip（参考：mouseover 新按钮前必有 mouseout，
     *         定时器由 mouseout 撤销 —— QML 里 exited 一样先来，这里是保险）。
     */
    function requestShow(name, item) {
        if (activeName === name || pendingName === name)
            return

        if (activeName !== "")
            activeName = ""

        pendingName = name
        targetItem = item
        showTimer.restart()
    }

    /* 请求隐藏（对应参考 mouseout 分支：撤销等待定时器 + 立即开始渐隐） */
    function requestHide(name) {
        if (pendingName === name) {
            showTimer.stop()
            pendingName = ""
            targetItem = null
        }

        if (activeName === name)
            activeName = ""
    }

    /* 300ms 延迟显示（参考 rafTimeout(..., 300)；raf 与 Timer 在这里等价，
       都只在到点后一次性触发，被 stop/restart 撤销是同一语义） */
    Timer {
        id: showTimer

        interval: 300
        repeat: false
        onTriggered: {
            if (tipLayer.pendingName === "" || tipLayer.targetItem === null)
                return

            tipLayer.activeName = tipLayer.pendingName
            tipLayer.pendingName = ""
            /* 文案在这一刻定格（含全屏动态项），尺寸随之重算 */
            tipLayer.currentText = tipLayer.textOf(tipLayer.activeName)
            tipLayer.reposition()
        }
    }

    /*
     * 外部状态变化的兜底（Connections 挂在 bar 上）：
     *   * fullscreen 翻掉 —— 显示中的 tip 要换文案（参考 updateTip）+ 重算位置
     *     （垂直偏移 23↔32、按钮自身行高也变了，不重算会悬空错位）；
     *   * shown 收起 —— 控制栏隐藏时 tip 一起收（正常路径是按钮 MouseArea
     *     随 visible 失效触发 exited 自己清，这里是兜底，防“控制栏没了 tip 还挂着”）；
     *   * width 变化（拖窗口）—— 显示中的 tip 重新夹紧水平位置。
     */
    Connections {
        target: bar

        function onFullscreenChanged() {
            if (activeName !== "") {
                currentText = textOf(activeName)
                reposition()
            }
        }

        function onShownChanged() {
            if (!bar.shown && (activeName !== "" || pendingName !== "")) {
                showTimer.stop()
                activeName = ""
                pendingName = ""
            }
        }

        function onWidthChanged() {
            if (activeName !== "")
                reposition()
        }
    }

    /* -----------------------------------------------------------------------
     * 位置计算（Tooltips.ts calculateTipPosition 的 QML 版，坐标系换成 bar 本地）
     * ----------------------------------------------------------------------- */
    function reposition() {
        if (bar === null || targetItem === null)
            return

        /* 按钮左上角换到 bar 坐标系（tip 气泡挂在 tipLayer 上，tipLayer 铺在 bar
           原点上，所以 tipBubble.x/y 直接用 bar 坐标即可） */
        var origin = targetItem.mapToItem(bar, 0, 0)

        /* 水平：tip 中心对齐按钮中心 */
        var left = origin.x - (tipBubble.width / 2 - targetItem.width / 2)

        /* 垂直：danmaku_switch 上方 8px；其余上方 23px（普通）/ 32px（全屏） */
        var top = 0

        if (activeName === "danmaku_switch")
            top = origin.y - 8 - tipBubble.height
        else
            top = origin.y - (bar.fullscreen ? 32 : 23) - tipBubble.height

        /* 水平夹紧：bar 内左右各留 12px（参考取 .nova-player-container 的内边距值） */
        if (left < 12)
            left = 12
        else if (left + tipBubble.width > bar.width - 12)
            left = bar.width - 12 - tipBubble.width

        tipBubble.x = left
        tipBubble.y = top
    }

    /* -----------------------------------------------------------------------
     * 气泡本体（视觉规格照 tooltip.scss 的 .nova-player-tooltip-item）
     * ----------------------------------------------------------------------- */
    Rectangle {
        id: tipBubble

        /*
         * 永远在渲染树里、只靠 opacity 藏（参考就是 visibility/opacity 切换，
         * 0.3s 渐隐动画要能跑完；QML 里 visible:false 会把动画直接掐断）。
         * 不放任何 MouseArea —— 参考 pointer-events: none，气泡永远不接事件。
         */
        /* 宽 = 文字自然宽 + 左右内边距 8*2，上限 18em@12px = 216px */
        width: Math.min(tipText.implicitWidth + 16, 216)
        /* 高 = 文字行盒高（lineCount×18，见 tipText 处的行高说明）+ 上下内边距 6*2 */
        height: tipText.height + 12
        radius: 2
        color: "#000000"

        /* 显隐：activeName 非空且控制栏在显示。
           参考 transition: opacity 0.3s linear —— 【收紧到 150ms】参考的 0.3s
           用户实测嫌“墨迹”（出现/消失都拖泥带水），tooltip 这种轻量提示
           150ms 已足够顺滑不生硬，交互节奏的 300ms 显示延迟保持参考值不变。 */
        opacity: (tipLayer.activeName !== "" && tipLayer.bar !== null
                  && tipLayer.bar.shown) ? 1 : 0

        Behavior on opacity {
            NumberAnimation {
                duration: 150
                easing.type: Easing.Linear
            }
        }

        /*
         * 隐藏态位移 translate(0px, 5px)：显示时归零。
         * 用 transform 而不是动 x/y —— 参考 CSS 就是 transform（位置 left/top 不过渡，
         * 只有这个相对位移参与 transition），动 x/y 会和 reposition() 的定位打架
         * （换按钮时位置直接跳，动画却让旧位置滑过去，观感就错了）。
         * 时长与 opacity 一并收紧到 150ms（参考 0.3s，理由见上面 opacity 处）。
         */
        transform: Translate {
            y: tipLayer.activeName !== "" ? 0 : 5

            Behavior on y {
                NumberAnimation {
                    duration: 150
                    easing.type: Easing.InOutQuad
                }
            }
        }

        Text {
            id: tipText

            x: 8
            y: 6
            /*
             * 限宽换行（参考 max-width:18em + word-wrap:break-word/word-break:break-all）：
             * 官方推荐写法 —— width 取 min(implicitWidth, 上限)，implicitWidth 是
             * “不换行的自然宽”，不依赖 width，无绑定循环；超上限时 wrap 生效、
             * 行数重算。上限 = 216 − 左右 padding 16 = 200。
             */
            width: Math.min(implicitWidth, 200)
            wrapMode: Text.Wrap
            color: "#ffffff"
            font.pixelSize: 12
            /*
             * 【行高 1.5 不能用 Text.ProportionalHeight 实现】QML 的 proportional
             * 行高是“自然行高 × 1.5，但 ascent 不变” —— 额外的 0.5 倍空间全部
             * 堆在**行下方**（QTextLine::setLineHeight 的语义），和 CSS line-height
             * “18px 行盒、字形垂直居中”完全不同。之前照字面抄成 ProportionalHeight
             * 1.5 → 单行气泡高 ≈35px，文字顶上只有 6px、底下却空 14px —— 用户实测
             * “tip 太高、文字偏上、下面一大截空隙不协调”。
             *
             * 正确的等价实现：**显式行盒** —— height = lineCount × 18（12px × 1.5），
             * 内容在行盒里 AlignVCenter。单行时气泡高恰 18+12=30px（与 CSS 逐像素
             * 一致），字形上下均匀居中。lineCount 只依赖 text 和 width（不依赖
             * height），绑定无循环。
             * （多行是纯理论情况 —— 实际文案全部远短于 200px 限宽、永不换行；
             * 若真换行，行距按自然行高、整体在行盒内居中，与参考的逐行 18px
             * 行盒有肉眼难辨的差异，留档不追。）
             */
            height: lineCount * 18
            verticalAlignment: Text.AlignVCenter
            /* ★ 普通属性 currentText（见文件头【渐隐期间文案/尺寸不要塌】） */
            text: tipLayer.currentText
        }
    }
}
