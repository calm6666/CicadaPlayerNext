// ===========================================================================
// 一条弹幕的"样子"（不含任何运动、轨道、时间逻辑 —— 那些在 DanmakuLayer.qml 里）
//
// 外观照着参考 front/hilihili-components/src/utils/danmaku/domEngine.ts 给每个弹幕
// 元素写的那串样式来（:517-533 的内联样式 + :328-385 注入的 CSS）：
//   font-weight: bold; line-height: 1; color: 弹幕自己的颜色（默认 #ffffff）
//   text-shadow: 1px 1px 2px rgba(0,0,0,.8)      → 这里用一层偏移 1px 的黑色文字近似
//   uid === 1（本人发的）→ border: 2px solid #fff; border-radius: 4px; padding: 2px 6px
//   :hover → 提到最上层并强制不透明（opacity: 1 !important）
//
// 【为什么文字宽度要由外面传进来】QML 的 Text.implicitWidth 要等一次布局才会更新，
// 引擎在同一帧里量完就要定位，等不了 —— 所以引擎用 TextMetrics 量（同步的），
// 把结果塞给 textAdvanceWidth / textLineHeight，这个文件只负责摆位置。
// ===========================================================================
import QtQuick

Item {
    id: view

    /* ---- 由引擎填的渲染数据 ---- */
    property string text: ""
    property color textColor: "#ffffff"
    property int fontSize: 18
    property real contentOpacity: 1
    /* 本人弹幕（uid === 1）：白边框。参考里**只有**本人弹幕有边框，
       悬停（:hover）只提层级 + 强制不透明，不加任何边框 —— 所以这里没有 hover 边框。 */
    property bool selfBorder: false
    /* 顶部/底部固定弹幕：参考给它们另追加了一条样式（padding: 2px 8px + 更重的阴影），
       而且追加在内联样式**之后**，所以连本人弹幕的 padding 也被它盖掉 */
    property bool fixedStyle: false
    /* TextMetrics 量出来的宽度 / 行高（line-height:1 → 行高就是字号） */
    property real textAdvanceWidth: 0
    property real textLineHeight: 18
    /* 渲染设置（对应面板右页那三项，由引擎下发） */
    property string fontFamily: "Microsoft YaHei"
    property bool fontBold: true
    /* 描边类型：0 重墨 / 1 描边 / 2 45°投影（参考右页那组单选） */
    property int outlineType: 0
    /* 点赞态（Tip 点赞后由 DanmakuView.present 每帧同步；初始渲染 like>0 同一机制） */
    property bool liked: false

    /* padding：固定弹幕 2px 8px、本人弹幕 2px 6px、其余不加；border 只有本人弹幕有 */
    readonly property int padH: fixedStyle ? 8 : (selfBorder ? 6 : 0)
    readonly property int padV: (fixedStyle || selfBorder) ? 2 : 0
    readonly property int borderW: selfBorder ? 2 : 0

    implicitWidth: Math.ceil(textAdvanceWidth) + 2 * (padH + borderW)
    implicitHeight: Math.ceil(textLineHeight) + 2 * (padV + borderW)

    opacity: contentOpacity
    /* 悬停时"提到最上层"（参考里是 z-index: 1000）—— 由引擎改 z，这里只管视觉 */
    transformOrigin: Item.TopLeft

    /* =======================================================================
     * 运动：交给 Qt 的**渲染线程**
     *
     * XAnimator 是 Qt 的"动画器"类型（Animator，不是 Animation）：它由场景图的动画驱动
     * 在渲染线程上逐帧插值，**GUI 线程不需要每帧来改 x**。而普通的 NumberAnimation /
     * 手工每帧赋 x，都是 GUI 线程在干活 —— 弹幕一多，CPU 就全花在"改坐标"上。
     *
     * 谁驱动它：DanmakuView.present()。每个 tick（30Hz）把
     *   当前 x → 结束 x（−整盒宽）、剩余时长（= 引擎总时长 ×(1−progress) ÷ 播放倍速）
     * 喂进来重播一次。于是：
     *   * 显示位置始终**贴着引擎**算出来的值（引擎仍是权威，碰撞/轨道都按它算），
     *   * 两次 tick 之间由渲染线程平滑插值 → 60fps 的连贯运动，但 GUI 只干 30Hz 的活。
     * ======================================================================= */
    function startMotion(fromX, toX, ms) {
        motion.stop()
        motion.from = fromX
        motion.to = toX
        motion.duration = Math.max(1, ms)
        motion.start()
    }

    /* 悬停冻结：原地停住（引擎那边同时也在冻结这条的时钟） */
    function pauseMotion() {
        if (motion.running)
            motion.pause()
    }

    /* 退场/变成固定弹幕：停掉动画，之后 x 由外面直接赋值 */
    function stopMotion() {
        motion.stop()
    }

    /* 位移动画是不是正在跑（外面靠它决定"要不要重新起一段"，见 DanmakuView.present） */
    function motionRunning() {
        return motion.running
    }

    /*
     * XAnimator：**它本身就是"动 x"的动画器**（不要写 property: "x" —— XAnimator 没有
     * 这个属性，写了整份文件加载失败，连锁 DanmakuView → PlayerView → Main 一起挂，
     * 程序直接退出。这个坑刚踩过。）
     */
    XAnimator {
        id: motion

        target: view
        easing.type: Easing.Linear
        running: false
    }

    /*
     * 文字本体：位置 = 内边距 + 边框，尺寸 = 文字宽 × 行盒高（line-height:1 → 行盒高 = 字号）。
     *
     * 【为什么不用 lineHeight: 1】CSS 的 line-height:1 是把**行盒**压成 1em，字在行盒里由
     * 字体自身的 ascent/descent 决定，视觉上是居中的；QML 的 Text 没有行盒概念，只给 lineHeight
     * 的话字会按自己的 ascent 贴顶排 → 白框里看着"字偏上、下面空一截"（就是反馈里那个不居中）。
     * 所以这里显式给出行盒的宽高，再让 Text 自己在里面垂直居中。
     */
    Text {
        id: label

        x: view.borderW + view.padH
        y: view.borderW + view.padV
        /*
         * 文字盒比测量宽度多给 1px（向上取整 + 1）。
         * 【为什么】测量给的是精确的 advance，最后那个字的右侧过冲/亚像素进位会让它
         * 刚好被自己的盒宽切掉一点 —— 表现就是"最后几个字看不到"。盒宽多 1px 就够，
         * 整盒（白框）的宽度仍由引擎按测量值算，观感不变。
         */
        width: Math.ceil(view.textAdvanceWidth) + 1
        height: view.textLineHeight
        verticalAlignment: Text.AlignVCenter
        text: view.text
        color: view.textColor
        /* 字体/粗体由面板右页的"弹幕字体 / 粗体"控制 */
        font.family: view.fontFamily
        font.pixelSize: view.fontSize
        font.bold: view.fontBold
        wrapMode: Text.NoWrap
    }

    /*
     * 阴影/描边：对应面板右页那三个单选（参考里是给文字加不同形态的 text-shadow）：
     *   0 重墨   → 偏移 2px 的浓黑阴影（参考固定弹幕那条 2px 2px 4px rgba(0,0,0,.9) 的近似）
     *   1 描边   → 四向各一份（上下左右各 1px），把字"包"起来，接近描边的观感
     *   2 45°投影→ 只保留右下 45° 方向的一份（1px 1px），最轻
     * 固定弹幕另有它自己的更重阴影（见 fixedStyle）。
     */
    Text {
        x: label.x + (view.outlineType === 1 ? 0 : (view.fixedStyle ? 2 : 1))
        y: label.y + (view.outlineType === 1 ? 0 : (view.fixedStyle ? 2 : 1))
        width: label.width
        height: label.height
        verticalAlignment: Text.AlignVCenter
        z: -1
        text: view.text
        color: Qt.rgba(0, 0, 0, view.fixedStyle || view.outlineType === 0 ? 0.9 : 0.8)
        font: label.font
        wrapMode: Text.NoWrap
    }

    /* 描边模式：上下左右各补一份（凑出"包边"的观感） */
    Repeater {
        model: view.outlineType === 1 ? 4 : 0

        Text {
            required property int index

            x: label.x + (index === 0 ? -1 : (index === 1 ? 1 : 0))
            y: label.y + (index === 2 ? -1 : (index === 3 ? 1 : 0))
            width: label.width
            height: label.height
            verticalAlignment: Text.AlignVCenter
            z: -2
            text: view.text
            color: Qt.rgba(0, 0, 0, 0.9)
            font: label.font
            wrapMode: Text.NoWrap
        }
    }

    /*
     * 本人白框（参考的 inline 样式：border: 2px solid #fff; border-radius: 4px）。
     * 包住整个盒子（文字 + padding + 边框），所以不会压到文字 —— 前提是外面把
     * width/height 设成"整盒尺寸"，见 DanmakuLayer._spawn。
     */
    Rectangle {
        anchors.fill: parent
        visible: view.selfBorder
        color: "transparent"
        radius: 4
        border.width: 2
        border.color: "#ffffff"
    }

    /*
     * 弹幕点赞图标（danmaku.scss .danmaku-x-liked-icon，B 站点赞手势）：
     * 常驻于弹幕元素内、挂在文字左前方 —— CSS 里 absolute 定位 + opacity:0
     * 不参与布局与尺寸测量（引擎量的仍是纯文字宽），点赞后淡入（150ms）。
     * 几何全按 CSS 的 em 值换算：见方 0.64em、左移 0.8em、底对齐行盒
     * （行盒高 = textLineHeight）。
     *
     * 【颜色 = 跟随弹幕文字色】参考 CSS 是 fill: currentColor（danmaku.scss:276，
     * 白字白色图标、彩字随字色）；素材 SVG 本体不带 fill（默认黑），所以这里
     * 按当前字色现拼一个 data URL —— path 取自 domEngine.ts:69 的 LIKED_ICON_SVG。
     * base64 是 UTF-8 安全的拼法（Qt.btoa 只认 Latin-1，中文不在 SVG 里但保持
     * 这套写法最稳）。
     */
    Image {
        /* domEngine.ts:69 LIKED_ICON_SVG 的 path d（viewBox 0 0 24 24） */
        readonly property string iconPath: "M4.909 8.543v15.275h-1.78a2.944 2.944 0 0 1-2.947-2.94"
                                           + "v-9.395c0-1.624 1.32-2.94 2.948-2.94zm8.64-8.114c1.282.62"
                                           + " 2.128 2.183 2.128 4.272q0 1.875-.468 3.842h5.655a2.955"
                                           + " 2.955 0 0 1 2.868 3.662l-1.865 7.568a5.32 5.32 0 0 1"
                                           + "-5.163 4.045H6.682V8.508l.132-.047c2.316-.894 3.634-3.072"
                                           + " 3.957-6.64C10.9.396 12.256-.196 13.548.428"

        width: 0.64 * view.fontSize
        height: 0.64 * view.fontSize
        x: label.x - 0.8 * view.fontSize
        y: label.y + label.height - height
        source: "data:image/svg+xml;base64,"
                + Qt.btoa('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24">'
                          + '<path fill="' + view.textColor + '" d="' + iconPath + '"/></svg>')
        sourceSize: Qt.size(width, height)
        smooth: true
        opacity: view.liked ? 1 : 0
        visible: opacity > 0.01

        /* CSS transition: opacity 150ms ease-in-out */
        Behavior on opacity {
            NumberAnimation { duration: 150; easing.type: Easing.InOutQuad }
        }
    }
}
