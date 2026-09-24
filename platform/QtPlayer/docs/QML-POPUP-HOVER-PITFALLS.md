# QML 浮层 / hover 避坑清单（本项目实测）

> 这份清单是 Qt 版播放器做弹幕浮层（音量、倍速、清晰度、弹幕设置面板、类型文字面板、发送栏）时
> **一条条踩出来的**。以后新增任何"浮层 / 浮起面板 / 鼠标交互区域"，先照这里对一遍。
> 每条的"现象"都是实际发生过的 bug，不是推测。

## 一、浮层用什么、怎么定尺寸

1. **浮层用 `QtQuick.Templates` 的 `T.Popup`**（本项目统一写法）。它是**纯 C++ 类型、没有 QML 样式文件**，
   因此：
   - **不会给 `background` 定尺寸** → 必须自己给 `background` 显式 `width`/`height`（或常量），
     否则那块判据区域是 0×0，鼠标移上去永远判成"不在里面"→ **一移上去就关**。
   - 同理 `contentItem` 不要指望被自动撑开；要判 hover 的那一层自己写死几何。
2. **`Popup` 没有可用的 `hovered`**（框架维护不了）→ 自己维护一个 `property bool pointerIn`，
   在 `background` 里放**一层** `hoverEnabled: true` 的 `MouseArea` 去改它，并在 `onClosed` 里清零
   （不清零会出现"关了但状态还是 true，下次打不开"）。
3. **浮层的 `width`/`height` 绝对不能绑到 `contentItem` 的 `width`/`height`**：
   框架会去**设置** contentItem 的尺寸 → 绑定循环 → QML 断开绑定 → **尺寸永远停在初始值**
   （现象：切页/换内容时宽高不变）。
   要给独立来源：普通属性、或 `内容列.implicitHeight + 内边距`。
4. **不要给浮层的 `x`/`y` 加 `Behavior`**：`x`/`y` 本身是有绑定的（由 width/height 推出来），
   再叠动画会和框架自己的定位打架 → 现象是"位置直接跳过去、动画很割裂"。
   要做过渡只给 `width`/`height` 加，位置让绑定跟着平滑走。

## 二、hover 到底谁说了算（本项目今天的头号坑）

5. **这个 App 的 hover 是"最上层独占"**：同一块位置上，**只有一个**接受 hover 的元素收得到它。
   推论：**浮层里只能有一个接受 hover 的东西**。
6. **面板内部的所有 `MouseArea` 一律不要写 `hoverEnabled: true`**。
   一写，鼠标压到那一项上时它就把 hover 抢走 → 外层判据和整层判据**同时**收不到 hover
   → 被判定"鼠标已离开"→ 浮层/面板自己关掉。（现象就是你反复看到的"移到面板上就关"。）
   - 需要 hover 变色的地方，用**鼠标位置 + `mapToItem` 自己判**（本项目：`panel.pointerOn(item)`）。
   - **`CheckBox`、`TextField`、任何 QtQuick.Controls 控件都自带 hover** ——
     放进浮层会抢判据，而且很多还是"控件内部行为"，关不掉。放进浮层前先确认。
7. **不要用 `acceptedButtons: Qt.NoButton` 当"只收 hover 不吃点击"的手段**：
   实测这层会**收不到 hover**（判据永远 false）。要"不吃点击"就把这层**垫到底层**
   （`z: -1`，点击自然被上面的项接走），而不是用 NoButton。
8. **`Image` 改不了 SVG 的 `fill`**：要按状态变色就用
   `MultiEffect { source: img; colorization: 1; colorizationColor: ... }`
   （源 SVG 是白色底时，着色直接得到目标色）。**不要为了变色去改 SVG 文件本身**。

## 三、触发区几何与"移出去才关"

9. **hover 只看几何，不看子树**（和 DOM 的 `mouseenter/mouseleave` 语义不同！）。
   DOM 里"菜单是按钮的子元素"所以进菜单不触发 leave；QML 必须**显式给一块区域**把
   "触发元素 + 中间的缝 + 浮层"合起来判。
10. **但触发区不要铺得比触发元素大太多**：铺大了会形成**死区** ——
    鼠标离图标还有一段距离就弹出面板、图标旁边点不到输入框。
    （正确做法：触发区只盖触发元素；"缝 + 浮层"交给单独一块区域或浮层自己的判据。）
11. **触发区判据和浮层判据绝对不要互相重叠**：
    重叠 → 互相抢 hover → 出现死循环：`打开 → 被抢走 → 判定离开 → 关闭 → hover 回来 → 又打开`，
    现象是**面板/文字颜色疯狂闪烁**。
12. **关面板的判定用"多来源取或 + 容错定时器"**：
    `if (!触发区.containsMouse && !缝区域.pointerIn && !浮层.pointerIn) 关闭`
    定时器 100ms 左右（只是"从触发元素挪到浮层上"那段路的容错，不是等待时间）。
    少任何一个来源都会误关；把某一块铺大去"兼顾"另一块就会变成第 11 条的闪烁。

## 四、控制栏自动隐藏（`pointerInside`）

13. **每新增一个交互区域，必须登记进 `pointerInside`**（"光标是不是在控制栏上"的唯一判据）。
    要登记的至少有：
    - 该区域的 MouseArea（`xxx.containsMouse`）
    - **它对应的浮层**（`xxxPopup.opened` / `xxxMenu.opened`）← 最容易漏，一漏就是
      "鼠标移动到浮层上，整条控制栏直接隐藏"
    - 面板内的输入框/按钮（`input.hovered` 等）
    （本项目已经因此踩过三次：弹幕开关+齿轮、发送栏三块、类型面板浮层。）

## 五、QML 致命错误（会让**程序直接退出**）

14. **重复 `id`** → `Property value set multiple times`。
    搬移代码块时最容易发生（旧块没删干净）。
15. **引用了已删除的 id** → 运行时 `is not a function` / `Unable to read property`。
16. **`X is not a type`** —— 忘了 import。例子：`MultiEffect` 需要 `import QtQuick.Effects`。
    这几种都让**整个文件加载失败** → 模块里没有 `Main` → 启动即退出。
    **改完一定通读一遍改动涉及的区域再交付。**

## 六、其它小坑（都实际踩过）

17. **别用 `palette` 当自定义属性名**：`QQuickItem.palette` 会被覆盖，Qt 报
    `Member palette overrides a member of the base object`。
18. **信号处理器要写形参**：`onPositionChanged: function (mouse) { ... }`；
    直接写 `onPositionChanged: { ... mouse.x ... }` 会报
    `Parameter "mouse" is not declared ... deprecated`。
19. **`file(GLOB)` 默认连目录一起匹配** → 在素材目录下新建子目录会让
    `qt_add_qml_module` 的拷贝步骤炸（`_qt_internal_qml_copy_file` 拿目录去 file()）。
    给 GLOB 加 `LIST_DIRECTORIES false`。
20. **`Popup` 与它的 `contentItem` 是两个对象**：`opened` / `close()` 在 `Popup` 上；
    内容组件自己的函数在内容组件的根对象上。混用报 `is not a function`
    （本项目：`dmPanel.syncFromEngine()` 应为 `dmPanelBody.syncFromEngine()`）。
21. **滚动列表（`Flickable` + 行）里的 hover 不能用 `mouseY / 行高` 算行号**（选集面板实测）。
    `MouseArea.mouseY` 是**视口坐标**，`Flickable` 一滚动（`contentY > 0`）行在视口里的位置就
    整体上移了 `contentY`，而 `mouseY` 还是从视口顶算起 → 算出来的行号**偏小**
    （现象：鼠标在第 N 项上，亮的却是上面第 N-1 项；差几行 = `contentY / 行高`）。
    **正确做法**：`mapToItem(行容器, x, y)` 把点映射进**内容坐标系**再算
    （本项目：`SubtitlePanel.pointerOn()`、选集面板的 `eplistMenu.rowAt()`）。
    另外**绑定的依赖里必须显式读一下 `contentY`**：滚轮滚动时鼠标不动，
    只依赖 `mouseX/mouseY` 的高亮不会重算，会停在旧行上。
    （"鼠标在不在面板里"那种判断要放在最前面 —— 绑定第一次求值时面板里的对象可能还没建好，
    先读它们会抛 `TypeError`。）
    **实测（qmltestrunner 复刻面板骨架）**：滚两行后鼠标在视口 y=15 处，
    旧算法给 **0**、`mapToItem` 版给 **2**；滚轮滚动（鼠标不动）时带 `contentY` 依赖的绑定
    从 0 自动重算到 3。
22. **滚轮滚过之后，`Flickable` 会把"下一拍"的按下事件从行里的 `MouseArea` 手上过滤掉**
    （选集面板实测，用户报的"点了没反应"就是它）。注意**不是**"按下之后拖远了才被抢"——
    实测：不滚动直接点、或抖 2px 再点，两种情况都能收到 `onClicked`；只有**滚轮滚动之后**
    那一拍会丢（`clicked` 收不到，等于点击没生效）。
    两条对策，本项目两条都上了：
    - 行里的 `MouseArea` 写 **`preventStealing: true`**（内部就是 `setKeepMouseGrab(true)`，
      Flickable 看到子项 keepMouseGrab 就不再过滤/抢）。**实测这一条就是决定性的**：
      同一个"滚轮之后点一下"的用例，没有它 `clicked = -1`，有它 `clicked = 2`。
      代价：在行上按住拖动不再能滚列表（滚轮照旧；参考里这个列表本来就不可滚）。
    - 面板**判据层**（`background` 那层 `hoverEnabled` 的 `MouseArea`）也接一个 `onClicked`，
      按同一套坐标换算走同一个激活函数，当兜底。判据层是 hover 高亮实际在用的那层；
      按下事件只投递给一个 item，所以两条路不会同时触发、不会重复执行。

## 七、新增浮层的推荐模板（照抄这个顺序写）

```qml
Item {                       // ① 触发元素
    id: trigger
    width: 28; height: 28

    Image { id: icon; ... visible: false }        // ② 图标（白色 SVG）
    MultiEffect {                                 // ③ 着色：默认白 / hover 蓝 / 选中蓝
        anchors.fill: icon
        source: icon
        colorization: 1
        colorizationColor: hovered ? "#00a1d6" : "#ffffff"
    }

    MouseArea {                                   // ④ 触发区：只盖触发元素
        id: triggerArea
        anchors.fill: parent
        hoverEnabled: true
        onEntered: { closeTimer.stop(); popup.open() }
        onExited:  closeTimer.restart()
    }

    Item {                                        // ⑤ 缝 + 浮层那块合并区域的判据
        id: zone
        parent: trigger
        visible: popup.opened
        x: -((popupWidth - trigger.width) / 2)
        y: -(popupHeight + gap)
        width: popupWidth
        height: trigger.height + gap + popupHeight
        property bool pointerIn: false

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true                        // 这一层**要** hover
            onEntered: { zone.pointerIn = true;  closeTimer.stop() }
            onExited:  { zone.pointerIn = false; closeTimer.restart() }
        }
    }

    Timer {                                       // ⑥ 容错定时器：三处都离开才关
        id: closeTimer
        interval: 100
        onTriggered: {
            if (!triggerArea.containsMouse && !zone.pointerIn && !popup.pointerIn)
                popup.close()
        }
    }

    T.Popup {                                     // ⑦ 浮层
        id: popup
        property bool pointerIn: false
        onClosed: pointerIn = false
        parent: trigger
        width: 196                                // 独立尺寸，别绑 contentItem
        height: body.implicitHeight + 24
        x: (parent.width - width) / 2
        y: -6 - height
        padding: 0
        closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutside
        enter: Transition {}
        exit: Transition {}

        background: Rectangle {                   // ⑧ 浮层判据：背景层、垫底、唯一接受 hover
            anchors.fill: parent                  //    （模板不给 background 定尺寸，自己撑）
            color: Qt.rgba(0.08, 0.08, 0.08, 0.9)
            radius: 2

            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                onEntered: { popup.pointerIn = true;  closeTimer.stop() }
                onExited:  { popup.pointerIn = false; closeTimer.restart() }
            }
        }

        contentItem: Column {                      // ⑨ 内容：内部 MouseArea 一律不写 hoverEnabled
            id: body
            // ...
        }
    }
}
```

另外别忘了：**把 `triggerArea.containsMouse` 和 `popup.opened` 加进 `pointerInside`**（第四条）。
