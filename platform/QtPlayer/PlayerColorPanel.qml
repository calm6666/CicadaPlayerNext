// ===========================================================================
// 「视频色彩调整」面板（复刻参考实现 component/colorpanel/index.ts + index.scss）
//
// 【参考的 DOM】
//     <div class="player-color-panel">
//       <div class="player-color-panel-title">色彩调整 <span class="…-close">✕</span></div>
//       <div class="player-color-panel-saturate player-color-wrap">
//         <div class="player-color-panel-name">饱和度</div>
//         <div class="player-color-panel-slider ui ui-slider ui-dark">…</div>
//         <div class="player-color-panel-value">100</div>
//       </div>
//       … 亮度、对比度（同样三段）
//       <div class="player-color-panel-reset"><span class="…-btn ui ui-button">
//         <div class="ui-area ui-button-black">重置</div></span></div>
//     </div>
//
// 【参考的 CSS（colorpanel/index.scss + index.scss 的 .ui-slider）】
//     面板：background rgba(33,33,33,.9); border-radius 4px; color #fff;
//           left/top 50% + translate(-50%,-50%); width 400px; z-index 80
//     标题：border-bottom 1px hsla(0,0%,100%,.1); font-size 16px; line-height 40px;
//           margin-bottom 10px
//     行：display flex; align-items center; line-height 20px; padding 5px 10px 5px 20px
//         名称 flex:none width 50px；滑块 flex:1（参考里写 width 250px）；
//         取值 flex:none width 40px
//     重置：border-top 1px hsla(0,0%,100%,.1); margin-top 10px; padding 8px 0;
//           按钮底 hsla(0,0%,100%,.3)，hover .4
//     滑块：轨道高 2px（圆角 1.5px，暗色底 #505050），已填充部分 #00a1d6，
//           圆点 12px #00a1d6；取值范围 0~255，默认 100（参考里 scaleX(0.392157)=100/255）
//
// 【⚠ 这个面板的滑块目前是"只动 UI、不动画面"的】
//   核心里**没有任何**亮度/对比度/饱和度接口（详查见
//   docs/PLAN-QUALITY-SWITCH-FIX.md 第十四轮）：MediaPlayer 没有这类方法，
//   唯一的视频滤镜框架（SetFilterConfig）在当前代码里是死代码
//   （ffmpegVideoFilter::is_supported() 恒返回 false、setOption() 也恒返回 false），
//   所以调用它不会有任何效果。用户要求"实在没有的先写一个 ui 静态数据"，
//   这里就把参考的 UI 一比一复刻出来，滑块可拖、数值可记，但不欺骗用户：
//   面板底部有一行说明，日志里也打一行 warning。
//   （真实的实现路径见面板底部注释：D3D11 视频处理器的 ProcAmp / macOS 的 Metal 计算内核。）
// ===========================================================================
import QtQuick
import QtPlayer

Rectangle {
    id: colorPanel

    /* 开着没有 */
    property bool opened: false
    /* 播放器组件（PlayerView 传进来）：拖滑块时调 player.setColorAdjust(...) */
    property var player: null

    /* 三个值：0~255，默认 100（和参考的初值一致） */
    property int saturate: 100
    property int brightness: 100
    property int contrast: 100

    /* 有没有被改动过（改了才提示"未生效"，没改过就不打扰） */
    readonly property bool touched: saturate !== 100 || brightness !== 100 || contrast !== 100

    signal closeRequested()

    function open() { opened = true }
    function close() { opened = false }

    function reset() {
        saturate = 100
        brightness = 100
        contrast = 100
        apply()
    }

    /*
     * 把三个值交给播放器（CicadaPlayerItem::setColorAdjust）。
     *
     * 真正生效的地方是**渲染后端**：Windows 的 D3D11 零拷贝路径用视频处理器的
     * BRIGHTNESS/CONTRAST/SATURATION 过滤器实现，所以是"零额外代价"的（不退出零拷贝、
     * 不多一次拷贝）。macOS/Linux/CPU 回退还没实现 —— 那时 player.colorAdjustSupported
     * 为 false，面板下面那行提示会如实说明，不会让用户以为生效了。
     */
    function apply() {
        if (player)
            player.setColorAdjust(brightness, contrast, saturate)

        console.warn("[color] 饱和度=" + saturate + " 亮度=" + brightness + " 对比度=" + contrast
                     + (supported ? "（已应用）" : "（当前渲染后端不支持，仅界面预览）"))
    }

    /* 当前后端支不支持（不支持的平台上拖滑块不会改变画面） */
    readonly property bool supported: player ? player.colorAdjustSupported : false

    /* 拖完一次就记一行（值 + 支持情况），方便日后接真实实现时对比 */
    function noteChanged(name, value) {
        console.warn("[color] " + name + " = " + value)
    }

    width: 400
    height: contentColumn.height
    x: (parent.width - width) / 2
    y: (parent.height - height) / 2
    color: "#E6212121"
    radius: 4
    visible: opened
    z: 80

    /*
     * 【★面板要自己"吃掉"鼠标事件，否则会穿到画面区去★】（用户实测：在面板上点一下就暂停/播放）
     *
     * 根因：面板是个普通 Rectangle（还有里面的 Column / Text / 分隔线），QML 里
     * **只有 MouseArea / TapHandler 这类"接收者"才消费鼠标事件**，Rectangle 不消费。
     * 所以点在面板的空白处时，事件会继续往下找，落到铺满整个播放区的 activityArea
     * （PlayerView.qml:644）上 → 它的 onClicked 起 400ms 定时器 → player.togglePause()。
     * 结果就是"点信息面板/色彩调整面板，视频却暂停了"。
     *
     * 做法：在面板根部铺一层**不显示任何东西**的 MouseArea，接所有键。
     *   * 它必须是**第一个子元素**（z 序最低）—— 上面的滑块、✕、重置按钮自己接走事件，
     *     空白处的点击才落到这一层，两者互不影响；
     *   * acceptedButtons: Qt.AllButtons —— 右键也一样吃掉（右键菜单的 TapHandler 在更下面，
     *     否则在面板上右键会弹出播放器右键菜单）；
     *   * hoverEnabled: false —— **故意不抢 hover**：activityArea 的鼠标移动
     *     （显示控制栏 + 重置自动隐藏定时器）和光标形状都还照常工作，鼠标划过面板
     *     不会把控制栏"冻住"或让指针乱变；
     *   * cursorShape 显式给箭头：面板默认是箭头，不跟着画面区变成"小手/空白光标"。
     */
    MouseArea {
        id: panelBackdrop

        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: false
        cursorShape: Qt.ArrowCursor
    }

    /*
     * 面板里的滑块（复刻 .ui-slider.ui-dark）。
     * 自己画而不是用 QtQuick.Controls 的 Slider：参考那一套轨道/圆点的尺寸和颜色
     * （2px 轨道 + 12px 圆点 + #00a1d6）在 Controls 的样式里要改一堆东西才能对上。
     */
    component ColorSlider: Item {
        id: slider

        property int value: 100
        property int from: 0
        property int to: 255
        signal moved(int value)

        implicitHeight: 12
        height: 12

        /* 轨道：高 2、圆角 1.5、暗色底 */
        Rectangle {
            id: track

            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - thumbDot.width
            height: 2
            radius: 1.5
            color: "#505050"

            /* 已填充部分（transform-origin:0 0 + scaleX） */
            Rectangle {
                width: parent.width * ratio
                height: parent.height
                radius: parent.radius
                color: QtPlayerTheme.playerAccent
            }
        }

        readonly property real ratio: (value - from) / Math.max(1, to - from)

        /* 圆点：12px 圆（参考里是 .ui-thumb-dot） */
        Rectangle {
            id: thumbDot

            width: 12
            height: 12
            radius: 6
            color: QtPlayerTheme.playerAccent
            anchors.verticalCenter: parent.verticalCenter
            x: (parent.width - width) * slider.ratio
        }

        MouseArea {
            id: sliderArea

            anchors.fill: parent
            /* 竖直方向也好按一些（参考的滑块只有 12px 高） */
            anchors.topMargin: -6
            anchors.bottomMargin: -6
            cursorShape: Qt.PointingHandCursor
            preventStealing: true

            function valueAt(mx) {
                const span = Math.max(1, slider.width - thumbDot.width)
                const r = Math.max(0, Math.min(1, mx / span))
                return Math.round(slider.from + r * (slider.to - slider.from))
            }

            /*
             * 【只发信号，**不要**直接写 slider.value —— 这是"重置后滑块不归位"的根因】
             *
             * 面板里每个滑块都是这么绑的：
             *     ColorSlider { value: colorPanel.saturate }      ← 唯一数据源是面板属性
             * 一旦这里写一句 `slider.value = ...`，**这个绑定就被打断**（QML 的赋值会覆盖绑定），
             * 于是滑块从此只认自己的值：之后 `reset()` 把面板属性改回 100，
             * 滑块照样停在老位置（用户实测："重置后都恢复默认，现在滑块这些都没恢复默认"）。
             *
             * 正确做法：拖动只把新值报出去（moved）→ 父级的 onMoved 写面板属性 →
             * 属性变化经上面那条绑定流回 slider.value → 圆点跟着走。
             * 单向数据流，reset() 自然就有效了。
             */
            onPressed: (mouse) => slider.moved(valueAt(mouse.x))
            onPositionChanged: (mouse) => {
                /* 用 MouseArea 自己的 pressed（QQuickMouseEvent 上没有这个属性） */
                if (sliderArea.pressed)
                    slider.moved(valueAt(mouse.x))
            }
        }
    }

    Column {
        id: contentColumn

        width: parent.width

        /* 标题：40px 高、居中、底部 1px 分隔线、下方 10px */
        Item {
            id: colorTitle

            width: parent.width
            height: 40

            Text {
                anchors.centerIn: parent
                color: "#fff"
                font.pixelSize: 16
                text: qsTr("色彩调整")
            }

            Rectangle {
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height: 1
                color: Qt.rgba(1, 1, 1, 0.1)
            }

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
                TapHandler { onTapped: colorPanel.closeRequested() }
            }
        }

        Item { width: 1; height: 10 }   /* margin-bottom:10px */

        /* 三行：饱和度 / 亮度 / 对比度 */
        Repeater {
            model: [
                { name: qsTr("饱和度"), key: "saturate" },
                { name: qsTr("亮度"),   key: "brightness" },
                { name: qsTr("对比度"), key: "contrast" }
            ]

            delegate: Item {
                id: colorRow

                required property var modelData

                width: contentColumn.width
                height: 30   /* line-height 20 + padding 5/5 */

                Text {
                    x: 20
                    width: 50
                    anchors.verticalCenter: parent.verticalCenter
                    color: "#fff"
                    font.pixelSize: 12
                    text: colorRow.modelData.name
                }

                ColorSlider {
                    id: rowSlider
                    x: 70
                    width: colorRow.width - 70 - 50
                    anchors.verticalCenter: parent.verticalCenter
                    value: colorRow.modelData.key === "saturate" ? colorPanel.saturate
                           : (colorRow.modelData.key === "brightness" ? colorPanel.brightness
                                                                      : colorPanel.contrast)
                    onMoved: (v) => {
                        if (colorRow.modelData.key === "saturate")
                            colorPanel.saturate = v
                        else if (colorRow.modelData.key === "brightness")
                            colorPanel.brightness = v
                        else
                            colorPanel.contrast = v

                        colorPanel.noteChanged(colorRow.modelData.name, v)
                        colorPanel.apply()
                    }
                }

                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    width: 40
                    horizontalAlignment: Text.AlignRight
                    anchors.verticalCenter: parent.verticalCenter
                    color: "#fff"
                    font.pixelSize: 12
                    text: rowSlider.value
                }
            }
        }

        /*
         * 生效说明（**如实**）：支持的平台（Windows D3D11 零拷贝）说"已生效"，
         * 其它后端说"仅界面预览"。只在用户真的动过滑块之后出现（没动过就不打扰）。
         */
        /*
         * 【★已按要求删掉那行绿字★】（2026-09-23）
         *
         * 原来这里在"后端支持"时显示一行**绿色**的
         * 「色彩调整已即时生效（在 D3D11 视频处理器上完成，不额外拷贝、不退出零拷贝）。」
         * 用户要求把它删掉 —— 现在 supported 为真（这台 Windows/D3D11 机器就是）
         * 整块 Text 都不显示，面板底部是干净的。
         *
         * 只删绿字、**保留**下面这条琥珀色的"后端不支持"提示：那种情况下拖滑块真的不动画面，
         * 必须如实说，否则就是在骗用户（Linux VAAPI / CPU 回退；macOS 已经实现了，
         * 见 CicadaTextureMetal 里的 Metal 计算内核）。
         */
        Text {
            width: parent.width - 40
            x: 20
            visible: colorPanel.touched && !colorPanel.supported
            wrapMode: Text.WordWrap
            color: "#e0c060"
            font.pixelSize: 11
            text: qsTr("当前渲染后端（Linux VAAPI / CPU 回退）尚未实现色彩调整，以上数值仅作界面预览。")
        }

        /* 重置：border-top + padding 8px 0，按钮底 hsla(0,0%,100%,.3) */
        Item {
            width: parent.width
            height: 8 + 28 + 8

            Rectangle {
                anchors { left: parent.left; right: parent.right; top: parent.top }
                height: 1
                color: Qt.rgba(1, 1, 1, 0.1)
            }

            Rectangle {
                id: resetButton

                anchors.centerIn: parent
                width: 72
                height: 28
                radius: 2
                color: resetHover.hovered ? Qt.rgba(1, 1, 1, 0.4) : Qt.rgba(1, 1, 1, 0.3)

                Text {
                    anchors.centerIn: parent
                    color: "#fff"
                    font.pixelSize: 12
                    text: qsTr("重置")
                }

                HoverHandler { id: resetHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: colorPanel.reset() }
            }
        }
    }
}
