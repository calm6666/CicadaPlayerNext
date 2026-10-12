// ===========================================================================
// 交互卡片关闭按钮 —— B 站式两半环倒计时圆环（照 hili-player closeCircle.ts 复刻）
//
// 结构（照 interaction.scss 底部 .nova-danmaku-x-circle* 的 DOM/CSS）：
//   * 根：16x16，绝对定位在卡片右上角（top/right -13）
//   * 左半容器：宽 50%（8px）、overflow hidden —— 只露出圆环的左半平面
//   * 右半容器：宽 50%（右对齐）、overflow hidden —— 只露出圆环的右半平面
//   * bar：完整 16x16 的"半染圆环"（border 2px，只染 top+left 或 right+bottom
//     两边 = 180° 弧），放在 clip 容器里绕自身中心旋转
//   * 中央 × 图标（close-x.svg，尺寸 90%）
//   * ::before 热区 40x40（left/top -12）—— 点击目标比可见圆环大一圈
//
// 【参数化变体（左下角缩小条目用，照 renderShrinkRing）】
//   ringSize 16 → 26：直径随条目放大（环色 #ffffffb3）
//   centerIcon 非空：环中心不是 × 而是 16x16 的卡片类型图标（圆形）
//   interactive false：不挂点击热区（点击交互由缩小条目根元素整体承接）
//
// 【两半环的几何（为什么这样转）】
//   remaining 语义 = 卡片剩余显示进度（1 满环 → 0 空环），角度公式照抄
//   closeCircle.ts 的 getLeftDeg/getRightDeg：
//     剩余 >= 50%：右半环静止满环，左半环从 -45°（满环）转到 -225°（消隐）
//     剩余 <  50%：左半环停在消隐角，右半环从 -45° 转到 -225°
//   换算到画布角度（y 向下、顺时针增；0°=3点、90°=6点、180°=9点、270°=12点）：
//     top+left 染色弧 = [135°, 315°]（跨西、北的 180°）
//     right+bottom 染色弧 = [315°, 135°]（跨 0° 的 180°）
//   旋转后再与半平面 clip 相交，正好得到"满环 → 右半 → 右上 1/4 → 空"的倒数视觉。
//
// 【为什么用 Canvas 画弧 + Item rotation 转弧】
//   QML 没有原生弧形元素；Canvas 只画一次静态 180° 弧（不重绘），
//   旋转全部交给 Item.rotation（场景图变换，还有 Behavior 0.3s ease
//   对应 CSS 的 transition: transform 0.3s ease）。
// ===========================================================================
import QtQuick

Item {
    id: circle

    // ---------------------------------------------------------------------
    // 对外接口
    // ---------------------------------------------------------------------

    /* 卡片剩余显示进度（1 → 0，随播放推进），由 InteractionLayer 的 delegate 喂进来 */
    property real remaining: 1

    /* 环直径（关闭环 16 / 缩小条目进度环 26 —— 照 scss 的 --width/--height 变量） */
    property real ringSize: 16

    /* 环色（关闭环 rgba(255,255,255,0.70196) / 缩小条目 #ffffffb3） */
    property color ringColor: Qt.rgba(1, 1, 1, 0.70196)

    /*
     * 环中心图标：空串 = 关闭环的 × 图标（close-x.svg，尺寸 90%）；
     * 非空 = 缩小条目的卡片类型图标（16x16 圆形，照 .nova-danmaku-x-shrink-img）
     */
    property string centerIcon: ""

    /* 是否挂点击热区（缩小条目的环不吃点击 —— 点击由条目根整体承接） */
    property bool interactive: true

    /* 点击（关闭卡片）—— 展示模式由 delegate 接住去打 closed 标记 */

    signal clicked()

    width: circle.ringSize
    height: circle.ringSize

    // ---------------------------------------------------------------------
    // 两半环角度（照 closeCircle.ts 的公式，一字不改）
    // ---------------------------------------------------------------------

    readonly property real leftDeg: circle.remaining >= 0.5
                                   ? -45 - (1 - circle.remaining) * 360
                                   : -225
    readonly property real rightDeg: circle.remaining >= 0.5
                                    ? -45
                                    : -45 - (0.5 - circle.remaining) * 360

    /* 弧线规格：border 2px；box 直径 → 弧中心线半径 = 直径/2 - 1（16→7 / 26→12） */
    readonly property real ringRadius: circle.ringSize / 2 - 1
    readonly property color barColor: circle.ringColor

    // ---------------------------------------------------------------------
    // 左半环：clip 左半平面 + 旋转的 top+left 180° 弧
    // ---------------------------------------------------------------------

    Item {
        x: 0
        width: circle.ringSize / 2
        height: circle.ringSize
        clip: true

        Canvas {
            id: leftBar

            x: 0
            width: circle.ringSize
            height: circle.ringSize

            /* bar 绕自身中心旋转 —— 与 CSS transform: rotate(var(--leftDeg)) 等价。
               【时长必须是 500ms + Linear，不能照抄 web 的 0.3s ease】平滑的前提是
               "动画时长 ≥ 数据更新周期"：框架的位置上报周期是 500ms
               （SuperMediaPlayer 的 mTimerInterval，QtPlayer 未覆盖默认值），
               web 的 timeupdate 约 250ms 一次，300ms transition 每次还没走完就被
               新目标接管 → 连续追帧，所以 web 用 0.3s 也平滑；Qt 这边若用 300ms，
               动画提前 200ms 走完、圆环静止等下一拍 → "转-停-转"顿挫（用户实测）。
               500ms Linear = 每拍 retarget 时恰好未完成，从当前角度匀速追新目标，
               恒定角速度无缝衔接；暂停时 position 停发，圆环随之停住
               （与 web"TIME_UPDATE 停发 → 圆环停住"语义一致）。 */
            rotation: circle.leftDeg

            Behavior on rotation {
                NumberAnimation {
                    duration: 500
                    easing.type: Easing.Linear
                }
            }

            onPaint: {
                var ctx = leftBar.getContext("2d")
                var c = circle.ringSize / 2
                ctx.reset()
                ctx.strokeStyle = circle.barColor
                ctx.lineWidth = 2
                ctx.beginPath()
                /* top+left 染色弧：135° 顺时针到 315°（跨西 180°、北 270°） */
                ctx.arc(c, c, circle.ringRadius, 135 * Math.PI / 180, 315 * Math.PI / 180, false)
                ctx.stroke()
            }
        }
    }

    // ---------------------------------------------------------------------
    // 右半环：clip 右半平面 + 旋转的 right+bottom 180° 弧
    // ---------------------------------------------------------------------

    Item {
        x: circle.ringSize / 2
        width: circle.ringSize / 2
        height: circle.ringSize
        clip: true

        Canvas {
            id: rightBar

            /* bar 本体直径大小，靠左偏移半宽，让右半露在 clip 窗口里（CSS 的 right:0） */
            x: -circle.ringSize / 2
            width: circle.ringSize
            height: circle.ringSize

            rotation: circle.rightDeg

            Behavior on rotation {
                /* 同左半环：500ms Linear 对齐框架 500ms 的位置上报周期
                   （原理见左半环处的注释） */
                NumberAnimation {
                    duration: 500
                    easing.type: Easing.Linear
                }
            }

            onPaint: {
                var ctx = rightBar.getContext("2d")
                var c = circle.ringSize / 2
                ctx.reset()
                ctx.strokeStyle = circle.barColor
                ctx.lineWidth = 2
                ctx.beginPath()
                /* right+bottom 染色弧：315° 顺时针到 495°（=135°，跨 0°/东） */
                ctx.arc(c, c, circle.ringRadius, 315 * Math.PI / 180, 495 * Math.PI / 180, false)
                ctx.stroke()
            }
        }
    }

    // ---------------------------------------------------------------------
    // 环中心图标：默认 × 图标（close-x.svg 原图：外圆 #000@.3 + 白 ×；
    // 尺寸 90% = scss 的 width/height 90%）；centerIcon 非空时换卡片类型图标
    // （16x16 圆形 —— 照 .nova-danmaku-x-shrink-img 的 16px 规格）
    // ---------------------------------------------------------------------

    Image {
        anchors.centerIn: parent
        width: circle.centerIcon !== "" ? 16 : parent.width * 0.9
        height: circle.centerIcon !== "" ? 16 : parent.height * 0.9
        source: circle.centerIcon !== ""
                ? circle.centerIcon
                : "../../assets/images/interaction/close-x.svg"
        sourceSize: Qt.size(width, height)
        smooth: true
        fillMode: Image.PreserveAspectFit
    }

    // ---------------------------------------------------------------------
    // 点击热区：40x40、相对可见圆环 left/top -12（照 .nova-danmaku-x-circle::before）
    // （缩小条目变体不挂 —— 环不吃点击）
    // ---------------------------------------------------------------------

    MouseArea {
        visible: circle.interactive
        x: -12
        y: -12
        width: 40
        height: 40
        cursorShape: Qt.PointingHandCursor
        onClicked: circle.clicked()
    }
}
