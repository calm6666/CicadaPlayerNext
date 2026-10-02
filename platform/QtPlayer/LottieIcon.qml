// ===========================================================================
// LottieIcon：播放器控制栏用的 Lottie 图标
//
// 和参考实现 front/hili-player/packages/player 的 `src/components/LottieIcon.ts`
// 是**同一套用法**（槽位序列 + startFrame + complete 规则），只是把渲染器从
// lottie-web 换成 Qt 官方的 `Qt.labs.lottieqt`（`LottieAnimation`，QQuickPaintedItem）。
// json 就是参考工程 src/assets/lottie-icon 里原样拷过来的那 19 个
// （见 assets/lottie-icon/，一个字节没改）。
//
// 【为什么不用 lottie-web】那是 JS 库，QML 里没有等价物。Qt 自带的这个 Lottie 实测能吃下
// 这批文件：19 个全部 `Ready`、`getDuration(true)` 帧数与 json 的 op 一致、
// `gotoAndStop(frame)` 逐帧都能画出正确像素（play-to-pause / pause-to-play 逐帧验证过）。
//
// 【参考实现的语义 → 这里的映射】
//   AnimationSlot（LottieIcon.ts:33-50）
//     path                 → slot.source
//     autoplay（默认 true） → slot.autoplay（这里也默认 true，和参考实现保持一致）
//     startFrame           → 播放前先 gotoAndStop(startFrame) 再播
//     complete: next|stop|loop → 播完的动作（见 onFinished）
//     stopFrame            → 播完停在哪一帧（缺省 = 动画末帧）
//     reverseToStopFrame   → slot.reverse（Qt 的 direction = Reverse）
//   方法 play/pause/stop/toggle/advanceSlot/setSequenceSlot/getCurrentSlotIndex/
//   showHover/hideHover 一一对应。
//
// 【Qt 这边的两个坑（都已处理，别再踩）】
//   1. `startFrame` / `endFrame` 是**只读**属性（qmltypes 里没有 write），
//      想跳帧必须用 `gotoAndPlay(frame)` / `gotoAndStop(frame)`；
//   2. 动画播完之后 Qt 不保证还留着最后一帧，所以 `onFinished` 里**显式**
//      `gotoAndStop(末帧)` —— 状态类图标（暂停键上的"播放三角"）必须一直看得见。
//
// 【用法（和参考实现各调用点一致）】
//   播放键：sequence 两槽 [pause-to-play, play-to-pause]，都 complete:"stop"；
//           暂停态 slotIndex=0、播放态 =1，各自 play() 一次就停在对应图标上。
//   音量键：四槽 [mute-to-volume(startFrame:2,next), volume-hover(stop),
//                volume-to-mute(next), volume-mute-hover(stop)]，
//           静止时停在槽 1（有声）或槽 3（静音），鼠标移入 showHover()。
//   全屏/设置/宽屏/网页全屏：单动画，鼠标移入 play()。
// ===========================================================================
import QtQuick
import Qt.labs.lottieqt

Item {
    id: root

    /*
     * 槽位序列。每项是一个 JS 对象：
     *   { source: "assets/lottie-icon/xxx.json",   // 相对本组件的资源路径
     *     autoplay: true,          // 切到该槽是否自动播（默认 true，和参考实现一致）
     *     startFrame: 0,           // 起播/停住用哪一帧（不填就按动画自己的 0 / 末帧）
     *     stopFrame: -1,           // 播完停在哪帧（-1/不填 = 末帧）
     *     complete: "next",        // 播完动作：next（默认）| stop | loop
     *     reverse: false }         // 反向播
     */
    property var sequence: []

    /* 当前槽位下标 */
    property int slotIndex: 0

    /* 隐藏时停掉动画（省 CPU）；重新显示会回到当前槽 */
    property bool running: true

    /* 当前 json 是否加载成功 */
    readonly property bool ready: anim.status === LottieAnimation.Ready
    readonly property bool failed: anim.status === LottieAnimation.Error

    /*
     * 取第 index 个槽。**组件内部一律用这个函数，不要用下面的 currentSlot 属性。**
     *
     * 【为什么必须这样（Qt 6.11.1 实测）】currentSlot 是带绑定的 readonly 属性，
     * QML 对它的重算是**惰性**的：在 onSequenceChanged / onSlotIndexChanged 这种
     * "属性刚变"的回调里读它，拿到的还是**上一次的缓存值** —— 序列已经是新的了，
     * currentSlot 还是旧的。实测日志（同一函数内并排打印两个值）：
     *
     *     applySlot: auto=false  curSlot.autoplay=false   seq0.autoplay=true
     *                             ^^^ 旧值                ^^^ 已经是新值
     *
     * 于是 applySlot() 按**旧槽位**判定"这一槽不该自动播"，动画永远起不来
     * （现象：Lottie 图标停在第一帧不动，而 status 是 Ready、autoplay 明明是 true）。
     *
     * 这个坑只在"sequence 本身是个会变的绑定"时才暴露：以前各调用点写的都是常量序列
     * （sequence 只赋值一次），所以一直没被发现。改成函数之后每次读都直接查 sequence，
     * 与惰性重算无关。
     */
    function slotAt(index) {
        if (index < 0 || index >= sequence.length)
            return ({})

        var slot = sequence[index]
        return (slot === undefined || slot === null) ? ({}) : slot
    }

    /*
     * 当前槽。**对外保留这个属性**（历史用法 / 调试用），组件内部请用 slotAt(slotIndex)。
     * 越界**或者数组里那一项是 undefined/null** 时都给空对象，调用方不用判空。
     * 【踩过的坑】只判越界不够：sequence 里只要有一个空位，sequence[slotIndex] 就是 undefined，
     * 于是 applySlot 里读 slot.source 直接抛
     * "TypeError: Cannot read property 'source' of undefined"（运行日志里那条）。
     */
    readonly property var currentSlot: slotAt(slotIndex)

    /*
     * 动画自己的画布尺寸 —— 就是 json 里的 "w"/"h"（每个图标不一样，别当成 88 一刀切）：
     *   play-to-pause / pause-to-play   28x28
     *   volume-* / settings / fullscreen 88x88
     *   Thumb                            18x18
     *
     * 【为什么必须传对】Qt 的 LottieAnimation（QQuickPaintedItem）是**按原始画布像素**
     * 画进自己的 item 矩形里的，**不会**自动缩放到 item 大小：
     *   * 88x88 的图标塞进 22x22 的 item → 只露出左上角一小块（"图标只显示一点"）；
     *   * 28x28 的图标塞进 22x22 的 item → 四边被切掉一点（"播放暂停图标显示不全"）。
     * 所以这里的做法是：LottieAnimation 按画布尺寸摆，再用 Scale 变换整体缩到
     * 调用方要的 width/height。这样无论画布多大，图标都是完整的一颗。
     */
    property real canvasWidth: 88
    property real canvasHeight: 88

    /* 一槽播完（参数是槽位下标；complete 规则已处理） */
    signal slotFinished(int index)

    /* 换槽后等 Ready 再决定要不要自动播 */
    property bool pendingPlay: false

    /*
     * 播放当前槽。
     *
     * 参考实现（LottieIcon.ts:415-425）是：
     *     序列模式 → animationItem.goToAndPlay(sequence[currentSlotIndex].startFrame ?? 0, true)
     *     单动画   → animationItem.goToAndPlay(0)
     * 也就是**每次都从该槽的起始帧重新播**，而不是"从当前帧继续"。
     *
     * 这里必须一样：动画播完会停在末帧，如果用 anim.play() 续播，它已经在末帧了 →
     * 再移上去什么都不动（"图标起不来"）。所以统一走 gotoAndPlay(startFrame ?? 0)。
     */
    function play() {
        if (!running || anim.status !== LottieAnimation.Ready)
            return

        var slot = slotAt(slotIndex)
        var from = (slot.startFrame !== undefined && slot.startFrame > 0) ? slot.startFrame : 0
        anim.gotoAndPlay(from)
    }

    function pause() {
        anim.pause()
    }

    /* 停住并回到当前槽的起始帧（参考实现 stop() 的语义） */
    function stop() {
        var slot = slotAt(slotIndex)
        if (slot.startFrame !== undefined)
            anim.gotoAndStop(slot.startFrame)
        else
            anim.stop()
    }

    function toggle() {
        if (anim.status === LottieAnimation.Ready && anim.playing)
            pause()
        else
            play()
    }

    /* 切到下一槽（末槽回第 0 槽） */
    function advanceSlot() {
        setSequenceSlot((slotIndex + 1) % Math.max(1, sequence.length))
    }

    /* 切到指定槽：换 source、落到起始帧、按槽的 autoplay 决定是否播 */
    function setSequenceSlot(index) {
        if (sequence.length === 0)
            return
        var i = Math.max(0, Math.min(sequence.length - 1, index))
        if (i === root.slotIndex && anim.source.toString() !== "")
            return
        root.slotIndex = i
        applySlot(true)
    }

    function getCurrentSlotIndex() {
        return root.slotIndex
    }

    /* 悬停：参考实现里各按钮在 mouseenter 里就是调 play() */
    function showHover() {
        play()
    }

    /* 离开：停在起始帧（音量那套 hover 动画要回到静止态） */
    function hideHover() {
        var slot = slotAt(slotIndex)
        if (slot.startFrame !== undefined)
            anim.gotoAndStop(slot.startFrame)
        else
            anim.stop()
    }

    /*
     * 静止时停在哪一帧（切槽/加载完、且这一槽不该自动播时用）。
     *
     * 规则只有两条，都来自参考实现：
     *   * 槽里写了 stopFrame → 停它；
     *   * 槽里写了 startFrame → 停它（例如音量 mute-to-volume 的 startFrame:2）；
     *   * 其余一律**第 0 帧**。
     *
     * 【不要自己发明第三条】我一度在这里加了"complete:'stop' 的槽停到末帧"，
     * 结果播放键的初始暂停态变成两条竖线（= 播放中的样子）—— 就是"播放/暂停反了"。
     * 参考里挂载时是**不播动画、停第 0 帧**：pause-to-play 的第 0 帧正好是播放三角，
     * 所以挂载时的暂停态本来就是对的。动画只有在被真正 play() 之后才会走到末帧。
     */
    function restFrame() {
        var slot = slotAt(slotIndex)

        if (slot.stopFrame !== undefined && slot.stopFrame >= 0)
            return slot.stopFrame

        if (slot.startFrame !== undefined && slot.startFrame > 0)
            return slot.startFrame

        return 0
    }

    /* 把当前槽的配置套到 LottieAnimation 上（注意 startFrame/endFrame 只读，只能跳帧） */
    function applySlot(autoPlayIt) {
        var slot = slotAt(slotIndex)

        if (slot === undefined || slot === null)
            return

        if (slot.source === undefined || slot.source === "") {
            anim.source = ""
            return
        }
        if (anim.source.toString() !== Qt.resolvedUrl(slot.source).toString())
            anim.source = slot.source
        anim.direction = slot.reverse ? LottieAnimation.Reverse : LottieAnimation.Forward
        anim.loops = (slot.complete === "loop") ? -1 : 1
        var auto = (slot.autoplay === undefined) ? true : slot.autoplay
        pendingPlay = autoPlayIt && auto

        if (anim.status === LottieAnimation.Ready) {
            if (pendingPlay) {
                pendingPlay = false
                play()
            } else {
                /* 不该播就停住（否则切槽/加载完它会自己跑一遍） */
                anim.stop()
                anim.gotoAndStop(restFrame())
            }
        }
    }

    LottieAnimation {
        id: anim

        /*
         * 按**画布尺寸**摆，不跟着 item 走 —— Qt 这个 QQuickPaintedItem 是把动画按原始
         * 画布像素画进自己的矩形的，item 开多大它就画多大，不会缩放。
         * 所以：item = 画布大小（画得完整），再用下面的 Scale 缩到调用方要的尺寸。
         */
        width: root.canvasWidth
        height: root.canvasHeight

        transform: Scale {
            origin.x: 0
            origin.y: 0
            xScale: root.canvasWidth > 0 ? root.width / root.canvasWidth : 1
            yScale: root.canvasHeight > 0 ? root.height / root.canvasHeight : 1
        }

        autoPlay: false                 // 一律由上面的 play()/applySlot() 控制
        loops: 1

        onStatusChanged: {
            if (status === LottieAnimation.Error) {
                console.log("LottieIcon: 加载失败 " + source)
                return
            }
            if (status !== LottieAnimation.Ready)
                return
            /*
             * 加载完成：该播就播一次，不该播就**停住**。
             *
             * 这里必须显式停：Qt 的 LottieAnimation 在 source 设好之后会自己开始跑，
             * 而 QML 是懒加载的（控制栏第一次可见时才建这些图标）—— 不停的话，
             * 鼠标一进控制栏就能看到一排图标同时在动。
             */
            if (root.pendingPlay) {
                root.pendingPlay = false
                root.play()
            } else {
                anim.stop()
                anim.gotoAndStop(root.restFrame())
            }
        }

        onFinished: {
            var slot = root.slotAt(root.slotIndex)
            /* 显式停在某一帧：Qt 播完不保证留着末帧，状态类图标必须一直可见 */
            var last = Math.max(0, anim.getDuration(true) - 1)
            var stopAt = (slot.stopFrame !== undefined && slot.stopFrame >= 0) ? slot.stopFrame : last
            anim.gotoAndStop(stopAt)

            root.slotFinished(root.slotIndex)
            if (slot.complete === "next")
                root.advanceSlot()
        }
    }

    onSlotIndexChanged: applySlot(false)
    onSequenceChanged: applySlot(true)
    Component.onCompleted: applySlot(true)
}
