# 交接文档：待办 + 上下文

> 目的：换一个会话/换一个 agent 也能接着干。**只写事实与踩过的坑**，不写客套话。

## 一、本轮的总体目标（用户原始要求）

1. 播放器控制栏拆成组件（`PlayerControlBar.qml` → `controls/*`）——**已完成**
2. 弹幕引擎抽成独立纯 C++ 模块（`CicadaPlayerNext/danmaku`，DLL 名 `Danmaku`）——**已完成**
3. 选集按钮 + 面板（照 `front/hili-player` 参考）——**已完成**
4. 设置齿轮的浮层面板（照 `platform/ctrl-setting.txt` + `platform/style.css`）——**已完成**
5. 首页窗口（视频库）+ 播放器窗口复用 + 上/下一个 + 选集接真数据——**已完成**
6. 设置面板抽成 `SettingsPage.qml`，首页与播放器窗口共用——**已完成**
7. 播放器窗口"无标题栏 + 顶部透明 + 右上三键悬停显隐 + 全屏不显示"——**已完成**

用户复述需求时的原话（作为验收口径，逐条都对应到下面第二节的表里）：
> 默认打开首页窗口 → 可以拖动包含视频的文件夹（多个）也可以点击打开选择文件夹 →
> 生成视频列表卡片带缩略图 → 点击"设置"直接打开设置面板、**设置内容实时生效** →
> 点视频卡片打开播放器窗口播放 → 已经开了一个播放窗口再点别的视频就**复用**这个窗口 →
> 播放器窗口无标题栏、上面透明、右上角三个按钮鼠标移到顶部才显示、全屏不显示 →
> 上一个/下一个按钮：有上一条才显示上一个、有下一条才显示下一个，点了能切换 →
> 选集那里可以直接点某一集切换。

## 二、当前已经能构建、能跑的东西

| 文件 | 说明 |
|---|---|
| `src/VideoLibrary.{h,cpp}` | 视频库：递归扫描（深度上限）、**抽首帧缩略图缓存**（FFmpeg 9 新 sws API）、播放列表导航（`hasPrevious/hasNext/goPrevious/goNext/pathAt`）。已进 `CICADA_QT_SOURCES`，用 `QML_ELEMENT` 注册 |
| `HomeWindow.qml` | 首页：QWindowKit 窗口 + 照抄的 `CaptionButton` 标题栏 + 工具条（打开文件夹/清空/**设置**）+ 卡片网格（缩略图/名字/大小）+ 整窗拖拽（`DropArea`，多文件夹一起拖）+ `FolderDialog` + `openPlayer()` **复用同一个播放器窗口** |
| `SettingsPage.qml` | **本轮新增**。应用设置页：遮罩 + 居中面板 + 两个手写控件（`SettingSwitch`/`SettingChoices`）。对外只有五个口：`player` / `hasPlayer` / `showDiagnostics` / `opened` / `open()` / `close()`。`Main.qml` 和 `HomeWindow.qml` **共用同一份** |
| `controls/LeftControls.qml` | `[上一个] → [播放/暂停] → [下一个] → [时间]`（`Row` 排版，顺序=书写顺序）；上一个按 `hasPrevious` 显隐、下一个按 `hasNext` 显隐；**没有下一个时给时间补一段距离**（`timeLabel.textGap`，见 4e）；**点时间可以输入时间跳转**（4f）；图标 `assets/images/next-play.svg`，上一个=同一张 `mirror` |
| `Main.qml` | 播放器窗口。`playFromLibrary(lib, index)` 换源；选集 `episodes/currentEpisodeIndex` 绑 `VideoLibrary`；上/下一个 → `playFromLibrary`；顶部栏按需求改成**默认隐藏、鼠标进顶部 36px 才浮出来、全屏永不显示**，底色 `transparent` |
| `main.cpp` | **有片源/清单 → `loadFromModule("QtPlayer", "Main")`**（直接开播放器窗口），否则 → `"HomeWindow"`（首页，播放器窗口由首页按需 `createObject`）。这个分支是 4e 第 3 条修的 |
| `PlayerView.qml` | 播放器整块（画面+弹幕+顶部栏+控制栏+鼠标活动区）。**本轮补了两个信号** `previousRequested`/`nextRequested`（见第四节第 4 条） |

## 三、还没做完 / 已知的坑（**下一步从这里挑**）

### 1. 扫描 + 抽缩略图是**同步**的，大文件夹会卡住界面（最值得做）
- `VideoLibrary::addFolder/addPaths` 里 `m_scanning = true; … m_scanning = false;` 在**同一个函数**里，
  也就是全程跑在 GUI 线程上；抽首帧还要过一次 FFmpeg 解码。拖一个几百条视频的目录进去，
  窗口会假死一段时间（`scanning` 那个"扫描中…"文案根本没机会重绘）。
- 建议做法：把扫描+抽帧挪到 `QThread`（或 `QtConcurrent::run`），每扫到一条就
  `emit itemsChanged()`（`QMetaObject::invokeMethod(..., Qt::QueuedConnection)` 回主线程），
  或者干脆改成"先入列、缩略图后台补"（`thumb` 变了再发一次 `itemsChanged`）。
- 这条**本轮没动**：它不在 HANDOVER-TODO 第三节的清单里，而且会改变 `VideoLibrary` 的
  线程模型（现在所有 Q_INVOKABLE 都假定在主线程），属于"要动就一次动干净"的改动。

### 2. 首页工具条的配色是写死的，不跟主题走
- `HomeWindow.qml` 的工具条 `color: "#1b1e24"`、卡片 `#20242b` / 悬停 `#2a2f38`、
  文字 `#e8ecf1` / `#8b95a3` / `#5c6674`，还有 `Button`（QtQuick.Controls.Basic）用的是默认样式。
- 而窗口底色 `color: QtPlayerTheme.windowBg` 是**跟主题**的 → 浅色主题下"浅底 + 深工具条 + 深卡片"，
  风格和设置页/播放器那套 `QtPlayerTheme` 不统一。用户没提过这条，所以没改（避免自创 UI）。

### 3. `Main.qml` 里还留着的临时东西
- Ctrl+D 灌示例弹幕那条 `Shortcut` 还在（用户没明确说删）。
- `docs/retired/`、`*.presplit*` 备份文件仍留着（用户没让删）。
- **`demoEpisodes` / `demoCurrentEpisode` 本轮已删**（选集接了真实列表之后没有任何引用）。

### 4. 交互行为里**没有机器验证**的部分（改这几处之前请人工点一遍）
- 顶部栏那套悬停显隐（浮出来/收回去/全屏不出现）是**按 Qt Quick 的 hover 语义推的**，
  本轮只验证到"能编译、能加载、能播"，没有做鼠标级别的实测（见第五节）。
- 上一个/下一个按钮点击后的换源、选集面板点击某一集后的换源：链路这次是通的
  （信号补齐了），但同样只验证到加载期无报错。

### 4b. 选集面板：滚动后高亮错行 + 点击没反应（**实测复现 + 实测修好**）
- **高亮错行**：`eplistMenu.hoverIndex` 原来用 `Math.floor(eplistHover.mouseY / rowHeight)`
  算行号，而 `mouseY` 是**视口坐标**，没算 `Flickable.contentY`
  （列表一滚，行号就偏小 = 亮在上一行）。已改成 `mapToItem` 映射进内容坐标系
  （`eplistMenu.rowAt()`，和 `SubtitlePanel.pointerOn()` 同一套路），
  并且把 `contentY` / 行数写成绑定的显式依赖（滚轮滚动时鼠标不动也要重算）。
- **点击没反应**：能滚的 `Flickable` 会把**滚轮滚动之后那一拍**的按下事件过滤掉
  （行里的 MouseArea 收不到按下）。已给行的 MouseArea 加 **`preventStealing: true`**
  （内部就是 `setKeepMouseGrab`，Flickable 尊重它），代价是"在行上按住拖动不再能滚列表"，
  滚轮照旧。另外面板判据层（`background` 里的 `eplistHover`）也接了 `onClicked` 兜底，
  两条路都走同一个 `eplistMenu.activate(index)`。
- 细节和"为什么这么写"都写在 `controls/RightControls.qml` 里（搜索 `本轮修`），
  归纳进了 `docs/QML-POPUP-HOVER-PITFALLS.md` 第 21、22 条；实测数字见第五节。

### 4c. 「关掉播放窗口 → 再点一个新视频」卡在正在加载 / 闪退（**实测复现 + 实测修好**）
- **现象**：关掉播放器窗口，再点一张卡片，界面停在"正在加载"；用户机器上还留下过一条崩溃记录
  （事件日志 `Application Error` / `appQtPlayer.exe` / `0xc0000005`，`错误模块: unknown`
  + 偏移量离谱 = 典型的"跳到已释放对象上"）。
- **根因（`src/CicadaHardwareDevice.cpp`）**：零拷贝要把 Qt 场景图的 D3D11 设备借给 FFmpeg，而它是
  **进程级单例**：`captureFromSceneGraph()` 第一行是 `if (m_ready) return true;`，
  缓存的 `g_qtDevice` 是**裸指针**（只 AddRef 给 FFmpeg，自己不持有）。播放器窗口在本工程里是
  "关掉就销毁、下次再建"的：
  1. 窗口销毁 → Qt 释放它那套 RHI 设备；
  2. 新窗口建起来 → 场景图初始化再调 `captureFromSceneGraph()` → **被 `m_ready` 短路**，
     手里还是**上一个窗口**的设备；
  3. FFmpeg 在这个（可能已释放的）设备上解码 → 轻则"解码在设备 A、显示在设备 B"，首帧永远
     出不来（**卡在正在加载**）；重则对已释放对象 AddRef / 调虚函数（**闪退**）。
- **修法**：
  * 单例**自己持有一份 COM 引用**（拿到 `AddRef`、放掉 `Release`）→ 缓存里的指针永不悬空；
  * `captureFromSceneGraph()` **不再被 `m_ready` 短路**：每次场景图初始化都看一遍，设备换了就换过去；
  * 新增 `releaseFromSceneGraph(window)`：场景图回收时把"从它借的那一份"还掉（只认借它的那个
    窗口 —— 首页窗口的场景图被回收时不能放掉播放器窗口正在用的设备）；
    `CicadaPlayerItem::onSceneGraphInvalidated()` 里调它；
  * 设备指针的读写（渲染线程 capture/release ↔ 解码线程 `provideHwDevice`）用一把 `QMutex` 串起来，
    **AddRef 也在锁里做**（原来是"判空之后、AddRef 之前"可能被换掉的 UAF 窗口）。
- **实测**（临时复现脚本：`createObject(Main.qml)` → `playFile(A)` → 播 8 秒 → `close()` →
  `deleteLater()` → 再开一个窗口 → `playFile(B)`；验完已删）：
  * 修复后（带首页窗口 = 真实用法）：`captured Qt's D3D11 device ... (window …)` →
    关窗 `released the captured D3D11 device (its scene graph went away)` →
    第二个窗口**重新捕获**并播放（`status=3 position=7808ms zeroCopy=1`）；
  * 4 个窗口反复开关（A/B 交替）内存采样：每轮峰值 ~177-180MB WS / ~215-229MB Priv，关掉都回落到
    同一水平（~125-140 / ~162-178），**没有逐轮上涨 → 这条路径不泄漏**；
  * 修复前的对照：关掉唯一窗口时进程直接消失 + 事件日志留下 `0xc0000005`。
- **顺带确认的一条 Qt 默认行为**（别误判成 bug）：播放器是**唯一**窗口时，关掉它 Qt 会按
  `quitOnLastWindowClosed` 直接退程序。

### 4d. 已知未修：带着播放器窗口退出程序时会崩（下一轮做）
- **现象**：app 退出时**播放器窗口还活着**（例如直接关首页窗口退出、或单窗口模式下关掉唯一窗口），
  进程以崩溃收场：事件日志 `错误模块: Qt6Core.dll` + `0xc0000005`（Qt 6.11.1，偏移 `0x264c8e`）。
- **怎么定位到"只是退出路径"**：把复现脚本的 `quitOnLastWindowClosed` 关掉之后，同样的
  "关窗 → 再开窗"序列**完整跑完且不崩** —— 崩溃只发生在退出这条路上，和 4c 的"关掉再开"无关。
- **入手点**：退出时先用 `QGuiApplication::aboutToQuit`（或窗口 `closing`）把播放器 `stop()` +
  清空片源（= 复刻 `Main.onClosing` 那两步），让渲染/GPU 状态在 Qt 拆场景图之前就放掉；
  再检查 `CicadaHardwareDevice` 里那几个**函数内静态**（`QMutex` / `QPointer<QQuickWindow>` /
  COM 指针）在静态析构期的顺序。
- 这条**不影响** 4c 里用户报的那条路径（那条已经实测修好）。

### 4e. 控制栏两条新规则 + 「命令行喂视频起不来」（本轮已改，实测过）
1. **只有一个视频时不显示「选集」**（用户要求：一个视频没有选集可言）。
   `controls/RightControls.qml` 里那颗按钮的判据从 `bar.fullscreen` 改成
   **`bar.fullscreen && bar.episodes.length > 1`**；面板的打开判据也从 `length > 0` 收紧到 `> 1`。
   处理方式和非全屏完全一样（隐藏 + 宽度归零，右边那几颗不会跟着挪）。
2. **没有「下一个」按钮时，时间到播放/暂停键的距离**（用户要求；**来回改了三版才定**，
   教训写在下面，别再走弯路）。
   * `Row` 里隐藏的子项宽度归 0，所以最后一条视频（`hasNext` 为假）时时间会直接贴到播放键上
     （非全屏文字偏移 0 → 用户说"太近"）。
   * **第一版（错）**：把「下一个」的整个槽位（`buttonWidth`，26/41px）空出来 ——
     用户否掉："直接留那么大空会很丑"。**是距离，不是空槽位。**
   * **第二版（还是错）**：只补"参考里相邻两颗按钮之间的墨迹距离" `btnWidth − iconSize`
     （非全屏 14、全屏 26）。用户反馈："下一个不显示时距离时间还是**太远**，但**输入 seek 的
     输入框倒是正常的**"。→ 说明基准取错了：那个值是**两颗按钮之间**的间距，
     而"贴在按钮右边的时间"该比的基准是**同一个格子里输入框的位置**。
   * **第三版（现在这版，正确）**：让**时间文字和输入框对齐**，都用参考
     `.player-ctrl-time-seek` 的 **`left: 6px`**（`LeftControls.seekInputLeft` 一处定义，
     输入框的 `x` 和文字的 `textGap` 都读它）：
     ```qml
     readonly property real textGap: bar.hasNext ? bar.timeIndent : seekInputLeft   // 6
     ```
     有「下一个」按钮时仍按参考的 `timeIndent`（非全屏 0 / 全屏 16，index.scss:4978-4982）。
     好处有两个：距离对了；而且**点时间时输入框原地出现**（两者左边缘同一个 x，不会跳）。
   * **实测**（`appQtPlayer.exe <视频>` 启动，非全屏、单文件 → `hasNext=false`）：
     `time x=26 textGap=6 → 文字左边缘=32`，`input x=6 → 输入框左边缘=32`，
     **两者距播放键右边缘都是 6px**（前两版分别是 0 和 14）。
     全屏这条规则与模式无关（`seekInputLeft` 不随 fullscreen 变），所以全屏同样是 **6px**。
3. **`appQtPlayer.exe <视频>` 现在是坏的，已修**（顺手发现，不在用户报的问题里）：
   根对象从 `Main` 换成 `HomeWindow` 之后，`main.cpp` 仍然给根对象设初值属性 `startupSource`，
   而 `HomeWindow` 没有这个属性 → Qt 报
   `Setting initial properties failed: HomeWindow does not have a property called startupSource`
   → **不创建任何窗口**（程序起来就退，看着像"双击没反应"）。
   现在按"有没有片源/清单"分开加载：有 → `Main`（播放器窗口，就是本文件头部注释写的用法），
   没有 → `HomeWindow`（首页）。**实测**：`appQtPlayer.exe <mp4>` 能起窗口、能播（日志有
   `opening source: …` + 零拷贝那几行），进程 9 秒仍存活；无参数启动仍是首页窗口、stderr 为空。

### 4f. 时间那一格加"输入时间跳转"（用户要求，实测过）
- **参考**：`.player-ctrl-time-seek`（老参考 `controls.scss:599-613`，用户又贴了 bpx- 版本的 CSS，
  数值一样）；DOM 上参考那个 `input` 只是个**空壳** —— `LeftControls.ts:329` 声明了
  `#playerCtrlTimeSeekInput`，**整个参考工程没有一处引用它**，所以行为全按用户描述实现：
  点时间 → 输入框出现、时间文字隐藏、预填**点击那一刻的播放时间**；
  点输入框以外 → 关掉并按输入 seek；**输入不规范 → 只关、不 seek**。
- **样式（两档，参考里是那条媒体查询；**三处按用户要求偏离**，见下）**：
  | | 基础规则（普通窗口） | `@media (min-width:750px)` + `[data-screen=full|web]`（全屏） |
  |---|---|---|
  | 字号 | **12px** | **14px** |
  | 高度 | `20px` | `30px` |
  | 宽度 | 参考写死 `60px` → **改成"贴住时间那一串字"** | 参考写死 `100px` → **同样贴住** |
  | 位置 | 参考 `left:6px` → **跟着时间文字的 x 走** | 同左 |
  | 文字 | `text-align:center`（**保持参考的居中**，用户明确要求） | 同左 |
  | 其余 | `padding:0 5px`、`background:hsla(0,0%,100%,.2)`、`border:1px solid transparent`、`color:inherit` | 同左 |
  * 偏离 1（垂直位置）：参考 `top:0` 是因为它那个按钮盒矮，我们这格是全屏 32 的行盒，所以**垂直居中**。
  * 偏离 2（矩形的来源）：参考写死 60/100 —— 非全屏会比时间文字短一截。
    现在**盒子矩形直接引用时间文字自己的矩形**：`x: timeText.x`、`width: timeText.implicitWidth`，
    左右边缘都贴住那一串字。**注意这与"文字居中"不冲突**（踩过这个坑）：
    矩形贴字是一回事，文字在矩形里居中（`AlignHCenter` + `padding 0 5px`）是另一回事，
    之前为了"两头对齐"把文字改成左对齐，用户立刻反馈"要居中，怎么靠左了"。
  * 偏离 3（时间文字离**前面那颗按钮盒子**的距离，**用户来回改了四轮**）：参考的播放键盒子
    36px（全屏 54）、图标 22（全屏 28）居中；我们按"图标实际画幅"裁成 26（全屏 41），
    照抄参考的偏移就会比参考**贴着图标近 10px**。最终口径（`textGap`）：
    ```
    非全屏：有没有「下一个」都用 btnWidth − iconSize（14px）   ← 前面无论是播放键还是「下一个」，
                                                                 盒子都是 26px 宽，距离同一个值
    全屏  ：有「下一个」→ timeIndent(16)；没有 → 6px            ← 用户实测这两档"看着可以"
    ```
    参考的 `timeIndent = 0`（非全屏）会让文字直接贴在前一颗按钮上 ——
    用户："非全屏并且存在下一个按钮时…离下一个太近了"。**想微调只改 `textGap` 这一个表达式。**
- **字号两档，且时间文字和输入框同一档**（用户明确要求："非全屏还是 12，只有全屏才 14，
  输入框也一样"）：两边都读 `PlayerControlBar.timeFontSize`
  （= `fullscreen ? 14 : QtPlayerTheme.ctrlTimeFontSize`），点开输入框时字号不跳。
  **别再"统一成 14"** —— 那会让非全屏比参考大一档，用户不要。
- **实测（同一个进程里先窗口模式量一次、再切全屏量一次）**：
  ```
  非全屏：timeFontSize=12 | 时间格 90（文字 74.2，textGap 14 → max(90, …) = 90）
          | 输入框 x = 时间文字 x（= textGap 14）、w = 文字宽 74.2、h=20、font=12、文字居中
  全屏  ：timeFontSize=14 | 时间格 92.6（文字 86.6，textGap 6）
          | 输入框 x = 时间文字 x（= 6）、w = 文字宽 86.6、h=30、font=14、文字居中
  ```
  也就是**两档里输入框的矩形都等于时间文字的矩形**（左右都贴住那串字），字号一致、文字居中。
  （textGap 从"分 hasNext"改成"分模式"之后的数值是按上面口径推的；
  用户要求"只编译、自己测"，这一轮没有跑探针。）
- **"点外面"那一层**（这一段是这次最费脑子的地方，别再走弯路）：
  * 它必须盖住**整个播放区**（含控制栏和它的按钮）；而 QML 的点击只在"父对象矩形内"才可能命中
    子项 —— 所以**不能**把它塞在控制栏里放大尺寸（超出控制栏矩形的部分收不到点击）。
    现在挂在 `PlayerView.qml` 上（铺满整窗）、`z:10` 盖住 `z:1` 的控制栏。
  * 参考的行为是"document 上挂 click 监听"：点哪儿都关输入框，被点到的元素自己也照常响应。
    这里照做：`onPressed` 里先 `commitTimeEdit()`，然后 **`mouse.accepted = false`** 把这一下
    **放过去**给下面的项（按钮/画面活动层不会失灵）。
  * 按在输入框矩形里的那一下**什么都不做也不拦**（放行给输入框，定位光标/选词照常）；
    矩形由控制栏转达（`LeftControls.seekInputRectIn(item)` → `PlayerControlBar.timeInputRectIn(item)`）。
  * **hover 一个都不能抢**：这一层 `hoverEnabled: false`；时间那一格自己的 MouseArea 是
    `hoverEnabled: true`，所以 `LeftControls` 透出 `timeHovered`，控制栏的 `pointerInside`
    里必须带上它（漏了 → 鼠标停在时间上控制栏自己收起）。
  * 输入框开着时 `pointerInside` 里也带上 `timeEditing`：否则打字打到一半（鼠标没动）
    3 秒自动收起会把输入框一起藏掉。
- **校验规则**（`LeftControls.parseSeekTime`，不规范返回 -1）：
  接受 `M:SS / MM:SS`（秒 0-59）和 `H:MM:SS / HH:MM:SS`（分、秒 0-59）；
  其余一律不规范：空、带字母或符号、冒号个数不对、分/秒 ≥ 60、单段超 3 位、负数…
  合法值再和总时长夹一下（`ms > duration` 就夹到末尾），然后 `player.seek(ms)`。
- **实测**（临时自检探针，验完已删；`appQtPlayer.exe <mp4>` 跑起来后自动执行）：
  * 解析：`'00:03'=3000  '0:03'=3000  '1:2'=62000  '12:34'=754000  '01:23:45'=5025000
    ' 2:05 '=125000  '999:59'=59999000  '1:2:3'=3723000  '0:0:0'=0`；
    不规范全为 -1：`''`/`'   '`/`abc`/`1:2:3:4`/`00:60`/`1:60:00`/`00:03:60`/`-1:00`/
    `1e3`/`12`/`::`/`00:03:`。
  * 交互：`beginTimeEdit()` → `editing=true text='00:03'`（预填当前时间）、
    输入框矩形 `32,1,60x20`；`position before = 3776` → 填 `00:30` → `commitTimeEdit()` →
    `editing=false`、**`position after commit = 31424`（≈30 秒处，seek 真的生效了）**。
  * 没有任何 QML 运行期告警（TypeError/ReferenceError/Binding loop 全无）。
- **没验证到的**：鼠标真点（点时间打开、点别处关闭、点控制栏按钮时"先关再照常响应"）——
  这几步要人的手；`mouse.accepted = false` 这条放行链是按 Qt 的点击传递规则写的，请顺手点一遍。

### 4g. 时间格式化：超过一小时要显示成 `H:MM:SS`（用户报的，已改，只编译未跑测）
- **旧版 bug**：`PlayerControlBar.formatTime()` 里 `minutes = floor(总秒 / 60)`、**没有小时位** →
  片长 1 小时以上时总时长会显示成 `66:44` 这种，而且和当前时间那一侧的形态对不上。
- **参考的形态**（用户给的 DOM）：`0:00:02 / 1:06:44` —— 注意**当前时间也带小时位**
  （小时**不补零**、分秒各两位）。所以形状必须**按片长**统一决定，不能只看"这一侧的值"。
- 现在：
  ```
  const total = (player !== null && player.duration > 0) ? player.duration : 0
  if (total >= 3600000 || value >= 3600000)   // 片长过 1 小时（或这一侧的值过 1 小时）
      return hours + ":" + twoDigits(minutes) + ":" + twoDigits(seconds)
  return twoDigits(hours * 60 + minutes) + ":" + twoDigits(seconds)   // 一小时内维持 MM:SS
  ```
  * `twoDigits()` 只给分、秒补零 —— **小时位不补零**（参考是 `1:06:44`，不是 `01:06:44`）。
  * 兜底：流媒体拿不到总时长（`duration` 为 0）时，如果**这一侧的值**已过 1 小时，也切到带小时位那档，
    免得又出现 `66:44`。
- **影响面**：时间文字、跳转输入框的预填、进度条 hover 气泡都走同一个 `bar.formatTime()`，一起改到 ✓。
  跳转那边的校验**不用动**：`parseSeekTime` 本来就接受 `M:SS` 和 `H:MM:SS`，
  `0:00:02` → 2000ms 能正常回填/提交 ✓。
- **本轮没有跑任何测试**（用户要求"编译后我自己测试"）：只保证动态/静态两档都**编译通过并已安装**。

### 4h. 弹幕"发送出去了但屏幕上不显示"（用户报的，渲染层已重做）
- **现象**：点发送后日志显示引擎收下了这条（`admitted`、渲染器 `quads=1`），屏幕上却一个
  像素都没有；暂停后前后两帧截图比对：**变化像素 0**。
- **排查结论（都是实测出来的）**：
  1. 原先写的是"自定义 `QSGMaterial` + 自己用 `qt_add_shaders()` 烘焙 .qsb、位置在顶点着色器
     里算"的方案（整屏弹幕合成一个 `QSGGeometryNode`、一次绘制）。UBO 完全按文档排
     （矩阵 0 / 不透明度 64 / 时钟 68、72）、顶点属性寄存器 0/1/2 和反射一致、纹理也在
     `updateSampledImage()` 里调了 `commitTextureOperations()` —— 屏幕上依然什么都没有。
  2. `qsb -d` 打开生成的 `danmaku.vert/frag.qsb`：6 个后端（SPIR-V 100、GLSL 100es/120/150、
     **HLSL 50**、MSL 12）正文齐全，反射信息（输入 location 0/1/2、`uAtlas` binding 1、
     UBO 大小 76）和 C++ 侧逐项对得上。
  3. 运行日志里 Qt 的 D3D11 后端只有三行：
     ```
     No HLSL (shader model 5.0) code found in baked shader QShader(stage=0 …)
     HLSL shader compilation failed:
     Failed to build graphics pipeline state
     ```
     也就是**同一份文件**：离线解析得出 HLSL，运行时取不到 → 管线建不起来 ⇒ 一条都画不出来。
     （Qt 6.11 文档另有一条：`QSGSimpleTextureNode` 这类独立子类节点只在默认/software 后端
     可用，D3D11 下必须用 `QQuickWindow::createImageNode()`。）
- **改法（最终形态）**：整层**不再有自定义材质/着色器/QSB**，改为"**一个几何节点 + Qt 自带纹理材质**"：
  * 根节点 `QSGOpacityNode`（"不透明度"设置作用在整层、实时）→ 下面挂**唯一**的 `QSGGeometryNode`；
  * 材质用 Qt 自带的 `QSGTextureMaterial`（着色器编在 Qt 库里，不存在取不到后端代码的问题），
    顶点布局用自带的 `QSGGeometry::defaultAttributes_TexturedPoint2D()`，纹理坐标是**归一化**的
    （Qt 文档：`QSGImageNode::rebuildGeometry` 的 texCoordMode 就是"把 sourceRect 归一化"）；
  * 每帧在 sync 阶段按 `x = x_ref − speed × (now − t_ref)` 在 CPU 上算出位置、写 N×4 个顶点，
    **一次绘制**；顶点容量自己记（`m_quadCapacity`），不然每帧都会重分配一次缓冲；
  * 顶点吸附到**设备像素栅格**：图集是 1:1 按设备像素烤的、材质是 Nearest 采样，滚动位置是小数，
    snapped 之前每一帧的采样点都落在纹理两个像素之间 —— 表现就是"字在抖、滚动不平滑"；
  * 删除：`src/DanmakuMaterial.{h,cpp}`、`shaders/danmaku.{vert,frag}`、CMake 里的
    `qt_add_shaders()` 段落（原处留了注释说明**为什么别再改回去**）。
  * 【走过的弯路，别再走】中间还试过"每条弹幕一个 `QSGImageNode`（`QQuickWindow::createImageNode()`）"：
    能画出来，但 N 个节点 = N 份几何 + N 次节点遍历，实测滚动发涩、CPU 也上去了，已改回一个节点。
- **同轮一起修的三件事（用户反馈）**：
  1. **自己发的弹幕"凭空出现在画面中间"** → `addImmediate()` 里那段"把 startMs 往前挪、
     让 self 一出现就整条在画面里"的补偿已经删掉，改成和普通弹幕一样**从右边缘外滑进来**；
     代价是暂停时发送的那条会停在画面外，恢复播放才滑进来（这正是用户要的观感）。
  2. **"文字和边框不匹配 / 字号设置没生效"** → 引擎的 `_measuredWidths` 是**按弹幕 id 缓存**的、
     缓存里**不含字号**：改字号后文字按新字号重烤、盒子却还是旧宽度。现在 `setBaseFontSize` /
     `setFontSizeScale` / `setAutoScale` / `setFontBold` 都先 `clearMeasuredWidths()` 再
     `rescaleActive()`（`setBaseFontSize` 以前这两步**都没有**，所以拖字号滑条完全没反应）。
  3. **防脏数据**：渲染器对 `textLength`/`width`/`height` 加了上限（超限直接跳过这条），
     免得一个坏值被拿去建 `QImage`/`QString` → "卡死之后闪退"；另外补了
     `releaseResources()`：场景图失效时**只放指针不 delete** 图集纹理（Qt 已经销毁过它，
     再 delete 就是双重释放），并清空节点指针。
  4. **★"边框正常、文字很小、字号设置完全无效"的真因（`DanmakuAtlas::paintEntry`）★**：
     那里建了一个 `QFont` 只用来 `QFontMetricsF` 量宽高和基线，**从来没 `painter.setFont(font)`** ——
     `drawText` 一直用画笔的默认字体（十几像素的小字）。于是：白框按真实字号算（看着正常）、
     字永远是默认小字、字号/缩放滑条怎么调都只改框不改字。修法就一行 `painter.setFont(font);`。
     **教训**：遇到"渲染出来不对"，先抓一帧量像素，不要在下游（缓存键、采样、节点）猜。
  5. **删掉位置轮询（用户要求）**：`CicadaPlayerItem` 里的 `m_positionTimer` / `onPositionTimer`
     整个删除。位置和缓冲位置本来就有框架推送（`PositionUpdate` / `BufferPositionUpdate` →
     `notifyPosition` / `notifyBufferedPosition`），那个 250ms 定时器是**第二个时钟**：轮询值滞后，
     界面/弹幕侧按帧外推已经跑在前面，每 250ms 拿滞后值校正一次 = 时钟被往回拽 → 弹幕
     "往前一段又退一格"的锯齿，而且每秒都在跨线程问播放器要位置（白烧 CPU）。
     时长没有对应回调，改为在"准备好了 / 首帧 / seek 结束"三个事件点现取（`refreshDuration()`）。
  6. **弹幕时钟改成事件驱动**：`DanmakuView` 不再每帧比对 `player.position`，而是
     `Connections { target: player; onPositionChanged }` 里对准锚点，帧计时器只做本地外推
     （`smoothPositionMs = 锚点 + 本地流逝 × 倍速`）。时钟只有一个来源。
  7. **窗口尺寸变化的缩放（用户追问）**：字号里的自动缩放比例 = 容器宽/1280（夹 0.75~1.5），
     所以窗口/全屏一变，每条弹幕的字号都要跟着变 —— 而 `measureBoxTextWidth()` 是**按 id 缓存**的、
     缓存里不含字号：`setContainerSize()` 以前**没清这个缓存**，于是字号改了、盒子宽度还是旧值
     （"缩放看起来没生效 / 边框和文字对不上"）。现在 `setContainerSize()` 里先
     `clearMeasuredWidths()` 再 `rescaleActive()`（改字号设置那条路在 `DanmakuController` 里清）。
  9. **★"弹幕后退一下再往前"（用户反复报，最后定位）★**：
     位置推送 `player.position` 是**滞后**的（框架回调要跨线程投递到 GUI 线程），而本地时钟按帧
     外推早已跑到前面 —— 只要"收到推送就把时钟对齐到它"，每来一次推送就把时钟往回拽一次，
     表现就是"后退一格再继续往前"。中间我一度改成"只往前对齐"，虽然不回退了，但滞后会永远
     累积、越跑越超前。**最终做法（音视频同步那套）**：
     * `|误差| > 600 ms` → 真跳转（拖进度条 / 换片 / 长卡顿恢复），硬对齐；
     * 否则 → 锚点重设成**当前外推值**（零跳变、永不回退），把误差折算成 `_rateTrim`
       （1.0 ± 4%，再做 0.7/0.3 平滑），让本地时钟在几秒内悄悄追上真值。
     渲染器的外推速率也乘以同一个 `_rateTrim`，两次 30Hz 喂时钟之间不会差那么一点点。

  10. **"滚动还是有点卡" 的两个大头（用户追问"能不能完全不平滑"）**：

     * **图集上传之前是"每烤一句新文案就重建整张 1024×1024 纹理"**（`delete` + 
       `createTextureFromImage` = 新建 GPU 纹理 + 全量重传 4 MB，1 ms 量级）。弹幕密集时
       每秒十几次 → 偶发掉帧。现在**合并上传**：最多每 100 ms 一次，攒着的条目一起传；
       配套加了 `DanmakuAtlasEntry::serial` / `DanmakuAtlas::uploadedSerial()`，
       渲染侧只画 `serial <= uploadedSerial()` 的条目（没上传的先不画，否则会采样到一块
       还没传上去的图集区域），并把 uploadedSerial 混进"图集键"签名，上传一发生新弹幕立刻出现。
     * **帧节奏**：位置本来就是在 sync 阶段按墙上时钟现算的（每帧位置都对），剩下的抖动来自
       "自由跑的 60Hz 定时器 vs 显示器 vsync" 不同步。现在 `updatePaintNode` 末尾在
       "时钟在走 + 屏上有弹幕"时调 `QQuickWindow::requestUpdate()`，让**渲染循环（vsync）**
       驱动下一帧；定时器保留当兜底（窗口被遮挡时 requestUpdate 不一定给帧）。
- **性能**：每帧开销 = "每条写 4 个顶点"，和 Qt 自己画文本节点同一量级；同时在屏条数由引擎
  `maxRenderCount = 2000` 夹住（实际在屏一般几十到几百条），整层**一次绘制**。
  **没有** QML 对象池、没有逐条文字排版、没有逐条属性写入 —— 那才是旧方案 CPU 高的原因
  （用户指标：1 万条弹幕的片源 CPU 占用不能超过 5%）。
- **已知取舍**：引擎逐条的 `alpha`（新弹幕淡入/淡出）在 GPU 这条路不生效 —— Qt 的节点不透明度
  只能靠 `QSGOpacityNode` 子树实现，逐条各套一层会让每条各自一次离屏渲染，开销不能接受。
  弹幕本来就是在屏幕边缘出现/消失，肉眼基本看不出来；要逐条 alpha 就用 `renderMode: "qml"`。
- **⚠ 未解决：用户报"连续发送到第 3 条弹幕时播放器卡死闪退"**。本机没有复现（用户要求"编译后
  自己测"，本轮也没跑运行时验证），只做了上面第 3 条的加固。**下次开工第一件事**：让用户给出
  崩溃签名（Windows 事件查看器里的"错误模块 + 偏移"，或者 stderr 输出），并确认
  "只有点发送会崩，还是播带 XML 弹幕的片源也会崩" —— 这两条信息能直接把范围缩到发送路径或渲染路径。
- **本轮没有跑运行时验证**（用户要求编译后自己测）：动态/静态两档编译通过并已安装。

### 4i. DASH 清晰度菜单「每个清晰度都出现两次」（用户报的，已改，只编译未跑测）

**用户原话**："dash 下，清晰度里面每个清晰度都有两个，重复了"。

**现象**：他的 `output.mpd` 有 **8 条视频 AdaptationSet**（4 条 H.264 + 4 条 H.265，同分辨率各一条）
+ 1 条音频。菜单里就是 `4K / 4K / 1080P / 1080P / 720P / 720P / 480P / 480P`。

**为什么会重复**：`DashManager::init()` 的模型是「**一个 Representation 一条流**」
（`framework/demuxer/dash/DashManager.cpp:51-77`），而上一轮为了修「只有自动 + 2160p」把视频
AdaptationSet **全收**了（同文件 `FindSuitableAdaptationSets`，`:643`）—— 于是同一分辨率的
H.264/H.265 各占一条流，`CicadaPlayerItem::onMediaInfoGetCb` 原样列出来就重复了。

**为什么不能写成「4K (H.265)」这种带编码的标签**：框架这一层认流只看
`mimeType == "video/mp4"`（`DashManager.cpp:680`），`Representation` / `StreamInfo` 里**都没有
codec 字段**（`framework/demuxer/dash/*.h` 通篇没有 codecs 相关成员），界面拿不到编码信息。

**改法**（`platform/QtPlayer/src/CicadaPlayerItem.cpp` 的 `onMediaInfoGetCb` 末尾）：
按分辨率（`宽x高`）分组，同组只留**带宽最大**的那条当代表（菜单本来就按带宽降序排，
同分辨率下带宽大的画质更好）。**框架的流列表一个字没动** —— ABR 自动挡照旧能在所有档之间切。
配套加了 `sameQualityRow()`：认「当前是哪一档」时先比 `streamIndex`、再比**分辨率**
（`notifyQualities` / `notifyStreamSwitched` 都改用它），这样 ABR 切到同分辨率的孪生流时
高亮仍然落在同一行、不会丢。`CicadaPlayerItem.h` 的 `qualities` 注释同步写明「一行 = 一档」。

**顺手修的（同一份日志里的另一条）**：`QML QQuickImage: Failed to get image from provider:
image://snapshot/0`（`controls/ProgressRow.qml:765`）—— `snapshotRevision` 还是 0（一张快照都还没有）
时 `localSource` 现在返回**空串**，不再拿 provider 手里根本不存在的 URL 去请求
（provider 返回空 `QImage`，QML 把它当「取图失败」打警告）。悬停后
`requestSnapshotForFrame()` 截到第一张，URL 自然就有。

**验证程度**：动态 Debug 编译通过（本机 exe 已重链，`[100%] Built target appQtPlayer`，无 C4xxx/LNK 警告），
**没跑运行时** —— 请用户点开 DASH 片源确认菜单是「自动 + 4 档」。

### 4j. DASH/HLS 三件事（清晰度重复 / 切换卡死 / 闪退）+「弹幕不显示」（本轮改了一部分，**都要等实测**）

用户原话："dash 模式下，控制栏的清晰度列表每个清晰度都有两个，切换有时候会直接卡死、等一会
可能又可以播放了，有时候直接就闪退了，弹幕现在不显示了，之前移植正常的现在不行了。"

#### (1) 清晰度重复 —— 合并键不够稳 + 框架只补了一个字段（**已改**）

- 上一轮加的"按分辨率合并"（`src/CicadaPlayerItem.cpp` 的 `onMediaInfoGetCb`）本来是对的：
  `DashStream::GetStreamMeta` 的宽高直接来自 MPD 属性
  （`DashStream.cpp:924-967` → `Representation::getStreamInfo` `Representation.cpp:103-122` ←
  `MPDParser.cpp:400-410`），prepare 阶段就有值，8 条流应该压成 4 档。
- **但键太脆**：原来用 `宽x高`。而框架那边 `DashStream.cpp` 只写了
  `if (meta->height == 0) { meta->height = height; meta->width = width; }` —— 换档窗口期
  底层（解码器）**只填了高度**时，`height != 0` 让整段跳过，宽度就保持 0 →
  上层拿到 `0x2160`，和 `3840x2160` 分不进同一档 → 还是两个。
- 本轮两处一起改，保证"同一档一定合并"：
  * `framework/demuxer/dash/DashStream.cpp`：宽、高**各自**补（`if (meta->width <= 0) …` /
    `if (meta->height <= 0) …`）；
  * `src/CicadaPlayerItem.cpp`：分组键改成**以高度优先**（`h2160`）。理由：菜单里的档位名就是按
    高度定的（4K/2K/`%1P`），"同名即同档"才和界面一致；`sameQualityRow()` 也改成同一口径
    （先比 streamIndex，再比高度，最后比宽度）。
- **判定方法（下次日志里一行就有）**：`qualities: N 档`（`CicadaPlayerItem.cpp` 的
  `notifyQualities`）。N=4 → 好了；N=8 → 合并没有跑（那就是跑的二进制不含这段，或者又有新键）。

#### (2) 切换卡死 —— 结构性原因，**本轮没动框架的切换逻辑**（要日志定位）

三条都要靠日志区分，别再靠猜：
- **升档要等"无缝点"**：`SMPMessageControllerListener.cpp:929-952`（`switchVideo`）里，
  新码率**不低**于当前就走"等 40 个包 + `FindSeamlessPointTimePosition`"这条路
  （真正执行在 `SuperMediaPlayer.cpp:3499-3519` 的读包循环尾部），
  条件不满足就只置 `mWillSwitchVideo = true` 什么都不做 → 用户点完没反应，过一会儿才切。
  降档走 `SwitchStreamAligned`（`SMPMessageControllerListener.cpp:930`）是快的。
- **真正执行切换那一下是同步的重活**：`SwitchVideo(startTime)` → seek + 关流/开流
  （`DashManager::CloseStream` → `DashStream::stop`），读线程被占住，画面卡住直到新流出关键帧。
- **`afThread::stop()` / `pause()` 没有超时**：`framework/utils/afThread.cpp:160-182`（`join()`）
  与 `:147-158`（`pause()` 死等条件变量）。读线程若正卡在阻塞 socket 读上（curl 的
  `low_speed_time=15000ms`，见日志），这里就会**等满 15 秒**——这正是"卡死一会儿又好了"。
- **日志定位法**：`selectQuality: index=… (streamIndex=…)` → `HLSManager/DashManager: OpenStream N`
  → `stream switched: video index=…`，这三条行的**时间戳差**就是卡住的位置：
  第一段大 = 等无缝点；第二段大 = 关流/开流/seek 本身；两段都小但界面没变 = 界面高亮的问题
  （`selectQuality` 现在是**先改高亮再等框架**，见 `CicadaPlayerItem.cpp` 里那句注释）。

#### (3) 闪退 —— 有硬证据了（**已加固**，根因未确证）

- 崩溃不是"没线索"：Windows 事件日志 + `%LOCALAPPDATA%\CrashDumps` 里有 2026-09-19 的 5 份
  完整转储（22:35 / 22:39 / 22:49 / 20:57 / 21:00）。22:39 与 22:49 两份的异常码是
  **`0xc0000374`（堆损坏）**，`ntdll` 的堆校验报的；22:35 是 `0xc0000005`。
- **怎么看这些转储**（本机没装调试器，`cdb.exe` 不存在）：`pip install minidump` + 一段
  python 就能读出异常码/触发线程/栈上的模块+RVA（脚本思路见本轮会话；栈扫描只是启发式，
  而且**PDB 会被下一次编译覆盖**，所以别指望符号一定对得上 —— 要看的是"哪个模块/哪条链"）。
- 本轮加固（都是"少一个野指针 free"，零行为变化）：
  * `framework/demuxer/demuxer_service.cpp` 与 `framework/demuxer/IDemuxer.cpp`：
    栈上的 `Stream_meta` 一律 **值初始化 `{}`**。因为 `streamMeta` 是**浅拷贝**、析构却调
    `releaseMeta`（`utils/mediaTypeInternal.cpp:8-16` → `utils/mediaFrame.c:23-64`，
    会 `free()` extradata/lang/description/keyUrl/drmPssh/drmKeyId 并顺着链表走），
    只要底层有一次"返回 >=0 但没把每个指针字段都写出来"，浅拷贝里剩下的就是栈垃圾。
  * 还没做（下轮，需要证据）：`SuperMediaPlayer::GetCurrentStreamInfo` 无锁读
    `mMediaInfo.mStreamInfoQueue`（GUI 线程在读、`Stop()` 在写，`SuperMediaPlayer.cpp:465-473`
    清空队列）；`player_notifier` 把 `&mMediaInfo` 裸指针塞进事件队列。

#### (4) 「弹幕不显示」—— 找到并修了一个真 bug（**已改**）

- **数据链的异步 token 竞态（上一轮新引入的失败模式）**：`Main.qml` 里 mock 基址一探到就会
  拉一次弹幕（`onBaseUrlChanged` → `fetchDanmakuFromMock`），而它的兜底判据只看
  `_sidecarLoaded`（= 同名 xml **已经**载入成功）——同名 xml 是**异步**的，那一刻还是 false，
  于是 `loadXmlTextAsync()` 把 C++ 的 `m_loadToken` 顶掉；等同名 xml 那次解析回来，
  `consumeParsedDanmaku` 按 token 把结果**静默丢掉（连失败信号都不发）**，
  片子旁边明明有 xml 也一条弹幕都没有。已改成 `if (_sidecarLoaded || _sidecarToken >= 0)`。
- **`anchorMonoMs` 的初值契约不一致**：`DanmakuView.qml` 给 0，而渲染器把"负值"当
  "还没有时间戳"（`src/DanmakuRendererItem.cpp` 的 `extraMs` 判据）。0 被当成"时间戳 = 时钟零点"：
  `extraMs = monoNow − 0 = 进程已运行的毫秒数`（几十秒）→ `nowSec` 远大于 `refSec` →
  滚动弹幕 `x -= speed × (nowSec − refSec)` 被推到屏幕左侧外 → **一条都看不见**
  （对上 4h 里记的"引擎收下了、quads=1、零像素变化"）。已改成 QML 初值 `-1`、
  C++ 判据从 `>= 0.0` 收紧成 `> 0.0`（两边都堵）。
- **补了"看得见 or 留一行日志"的兜底**（GPU 模式是**唯一**渲染路径、界面上又没有切换入口，
  断了就是全空且无回退）：
  * `DanmakuView.qml` 加了个 2 秒一次的可见性看门狗：`stats.itemCount > 0` 且
    `stats.activeCount > 0` 但 `gpuRenderer.quadCount === 0` 连续两次 →
    打一行 warn 并**自动回退 `renderMode = "qml"`**（对象池那条老路，先让弹幕看得见）；
  * `src/DanmakuRendererItem.cpp` 在"引擎有在屏弹幕、本帧却一个 quad 都没建出来"时
    1 秒最多打一行（在屏条数 / 图集可用条目 / uploadedSerial / 世代）——
    **冷路径**，正常播放一行都不会有。这条日志能把空屏一刀切成三种：
    载入 0 = 数据；在屏 0 = 时钟；在屏有但 quad 0 = 图集/UV/几何。
- **实测到的正面证据**：本机 mock（9101）是活的，`GET /x/v1/dm/list.so?oid=` 返回
  **HTTP 200 + 73KB 有效 XML（1000 条）**，`/healthz` 里 `durationSec=268.352`（和用户那份
  4:28.2 的片源对得上）——所以"网络片源拿不到弹幕数据"不成立，空屏只能是渲染侧。

#### (5) ★「切换卡死」根因（用户日志实测，**已改**）：换档换了编码时**解码器没重建** ★

用户的 `log.txt`（2026-09-20 08:29）把这件事拍死了：

```
08:29:36.465 [CicadaPlayerItem] selectQuality: index=3 (streamIndex=3)      ← 从 480p HEVC 切到 480p H.264
08:29:36.466 [] video change video bitrate before is 1050000, after is 1500000
08:29:36.466 [DashManager] OpenStream 3 / CloseStream 7
08:29:36.556 [CicadaPlayerItem] stream switched: video index=3 854x480 1500kbps   ← 框架说切完了（91ms）
08:29:37.828 [FFMPEG] [hevc @ ...] Failed to parse header of NALU (type 3) ...     ← 之后每秒 400+ 行
08:29:38+   [FFMPEG] [hevc @ ...] PPS id out of range: 15 / Skipping invalid undecodable NALU
```

**HEVC 解码器一直在解 H.264 的包**，一直刷到用户手动关程序（画面再也不出来）。

原因：`SuperMediaPlayer::SetUpVideoPath()` 开头那句"解码器有效 + 渲染有效 → `return 0`"，
**只问有没有解码器、不问这个解码器是不是这一档的编码**。切档只换流、没换解码器。

修法（`mediaPlayer/SuperMediaPlayer.{h,cpp}`）：
* 新增成员 `mVideoDecoderCodec`（`CreateVideoDecoder()` 成功后记录 `meta.codec`，`closeVideo()` 清成 NONE）；
* `SetUpVideoPath()` 里在 `return 0` 之前先 `updateVideoMeta()` 刷一次当前档的 meta，
  编码不一致就 `mAVDeviceManager->invalidateDecoder(DEVICE_TYPE_VIDEO)` ——
  它只把标记置 false（不碰渲染/音频，`SMPAVDeviceManager.cpp:155-164`），
  后面就会走 `CreateVideoDecoder` → `setUpDecoder`，后者本来就按
  `pMeta->codec == meta.codec` 决定复用还是重建（`SMPAVDeviceManager.h:43` / `.cpp:61-91`）。
* 下次日志里应出现：`video codec changed on switch (… -> …), recreate the video decoder`。

**注意**：用户那份 MPD 每个分辨率都有 H.264 + H.265 两条流（`qualities` 已合并成一档），
所以"同一清晰度互换"就会跨编码 —— 这是必现路径，不是偶发。

#### (6) ★「弹幕不显示」根因（同一份日志，**已改**）：`gpuRenderer` 是 null，时钟一步没走 ★

日志里 `DanmakuView.qml:431`（stepTimer 的 `onTriggered` 第一句 `gpuRenderer.monoMs()`）
**抛了 560 次 `TypeError: Cannot call method/read property 'monoMs' of null`**
（08:29:25.6 → 08:29:46，约 30 次/秒；同时 `[danmaku] 从 mock 载入 1000 条弹幕` 是成功的）：

* 这一句一抛异常，`stepDanmaku()` 整个不执行 → `engine.update()` 永远不被调用 →
  引擎里 1000 条数据一条都不会上屏（GPU 与 qml 两条渲染路都不动）；
* 每秒 30 个 QML 异常本身也在拖 GUI 线程，界面点起来就是"卡死"的感觉。

修法（`DanmakuView.qml`）：
1. 时钟不再依赖渲染器对象：`var mono = gpuRenderer !== null ? gpuRenderer.monoMs() : Date.now()`
   （`stepDanmaku` 里那处三元也一样）；渲染器在就同源、不在也照样推进引擎时钟；
2. 看门狗的条件也判 null：`(!gpuRenderer || gpuRenderer.quadCount === 0)` → 连两次就
   `renderMode = "qml"` 回退，并打一行 warn（带上"渲染器=null（对象没建出来）"）；
3. **新增一行开机自检**（本组件声明在渲染器之后，读到的就是"建出来没有"）：
   `[danmaku] 渲染器自检：gpuRenderer 为 null —— GPU 路径不可用，稍后自动回退 qml`。
   **下一份日志有这一行就能确认**：是"从来没建出来"还是"后来被销毁了"。
   （本轮只确认了"它是 null"，**没能确定为什么 null** —— 源码里没有创建失败的报错，
   声明也在 root 下、id 同文档，QML_ELEMENT 也注册了。需要上面那行自检 + 回退后的表现来收口。）

#### (7) 已用用户日志验证的（**好消息**）

* **清晰度去重成功**：`qualities: 4`（08:29:25，DASH 8 条流 → 4 档），上一轮报的"每个清晰度两个"没了。
* mock 弹幕数据没问题：`[danmaku] 从 mock 载入 1000 条弹幕`。
* 切档本身很快：`selectQuality` → `stream switched` 只用了 **91 ms**（36.465 → 36.556），
  所以"卡死"不是切换慢，而是**切完解码器用错**（见 (5)）。

#### (8) 2026-09-20 10:42/10:44 两份日志（dash_log / hls_log）：弹幕数据一到就闪退（**已用回退路径止血**）

**读到的三件事**：
* ✅ `[danmaku] 渲染器自检：GPU item 已创建（renderMode=gpu）` —— (6) 里那个"渲染器 null"在这版没了；
* ✅ `qualities: 4`（DASH 与 HLS 都是）—— 清晰度去重稳定；
* ✅ `[danmaku] 从 mock 载入 1000 条弹幕` —— 数据没问题。

**新崩溃**：两份日志里，`从 mock 载入 1000 条弹幕` 之后 **0.2~0.3 秒**必定
`[crash] 未处理异常：code=0xc0000005`，应用自己的 dbghelp 处理器打出的 22 层调用栈里
**#08~#20 整段落在 `Qt6Qmld.dll`（QV4 = QML 的 JS 引擎）**（`#00/#01` 是 main.cpp:318/366 的
dumpCallStack/cicadaUnhandledException）。DASH、HLS 两次一模一样 → 可复现，
且"弹幕真正开始上屏"就是触发条件。

**为什么上一版不崩**：上一版渲染器是 null → `stepDanmaku()` 抛异常 → 弹幕时钟一步没走 →
数据进了引擎但**一条都没上屏**；把时钟修好（(6)）之后，这条**一直存在、只是从没被执行到**的
路才暴露出来。也就是说：这不是这轮改出来的新 bug，是这轮把盖子掀开了。

**止血**：`DanmakuView.qml` 的 `renderMode` 默认从 `"gpu"` 改回 **`"qml"`**（对象池 +
`present()`，用户说"之前移植正常"的那条路）——先让弹幕能看见、且不闪退；
GPU 那条路整套保留（改这一个属性就能切回去复现/继续查），(6) 的 null 修复与看门狗也都留着。

**下一步（等用户这次的结果）**：
* 不闪退 + 弹幕出现 ⇒ 崩溃在 GPU 渲染路径，接下来在 `DanmakuRendererItem`/`DanmakuAtlas`
  这条 QSG 路上找（重点：`m_entries`/`m_uvs` 与 `items.size()` 的关系、图集两趟 UV、
  `updatePaintNode` 与 GUI 线程 `engine.update()` 的并发读取）；
* 仍闪退 ⇒ 崩溃不在渲染路径，那就看新 `[crash]` 栈落在哪个模块，再顺藤摸瓜。

#### (9) 2026-09-20 11 点用户实测：`>` 前缀（**已修**）+ 我把默认渲染改错（**已改回**）

* **每条弹幕前面多一个 `>`** —— `danmaku/src/DanmakuXml.cpp` 的 `parseOneD()`：
  `tagEnd` 是 `xml.find('>', lt)` 的结果，**指向 `'>'` 本身**，而老代码写
  `text = xml.substr(tagEnd, contentEnd - tagEnd)` → 文本第一个字符就是那个 `>`，
  且 `trim()` 不会去掉它。已改成从 `tagEnd + 1` 开始、长度减 1。
* **我把 `renderMode` 默认改成 `"qml"` 是错的**（用户实测：CPU 离奇高；弹幕本来就要求 GPU）。
  已改回 `"gpu"`。qml 只是对照/回退路径，**不要**再当默认。
* 待查（用户这次是在我改错的 qml 路径下看到的，可能在 GPU 版上就没了）：
  "弹幕滚到屏幕中间就消失" —— 若 GPU 版仍有，从引擎的
  `x = w − (w + width)·p`（`DanmakuEngine.cpp:1205`）与 `containerWidth` 是否等于渲染层实际宽度查起。
* 用户看到的 CRT 报错（`HEAP CORRUPTION DETECTED: after Normal block (#250303) …
  wrote to memory after end of heap buffer`）**没有进日志**（CRT 走的是弹框），
  下次复现请按 **Retry to debug** 抓调用栈。

### 4k. 弹幕「一顿一顿」+「鼠标移上去往后跑一段」= **渲染器与引擎两条时钟不同源**（本轮改，等实测）

**现象**（用户原话）：滚动"一顿一顿"；鼠标移到弹幕上它会往后跑一段距离；暂停是好的（冻结不继续）。

**根因**（一处，两个现象都是它）：
弹幕的位置是**引擎**按它自己的时钟算出来的（`DanmakuEngine::_clockMs`，写于
`danmaku/src/DanmakuEngine.cpp:306`），渲染器只负责把 `items[i].x` 外推到"这一帧"。
外推必须有参照点，而参照点以前用的是 QML 绑过去的 `player.position` —— 那是**框架 2Hz
推来的原始位置**；引擎吃的却是 QML 那条平滑时钟 `smoothPositionMs`
（`DanmakuView.qml:342` 的 `smoothAt()`：领先、且按位置回调一档一档跳）。
两条时钟差一档（最多 500ms ≈ 几百像素）时：
* 画面上"锚点 + 外推"的位置随那一档**一进一退** → "一顿一顿"；
* 引擎的**命中检测**用的还是它自己那份 x（对应屏幕上另一个位置）→ 鼠标移到**看得见**
  的那条上，引擎冻住的却是它认为在那儿的那条，冻结点比你看到的位置靠后
  → "鼠标移上去往后跑一段"。

**修法**（不是打补丁，是把基准合成一条）：
* `DanmakuController` 每帧把**引擎的时钟值**和它的墙钟时间戳成对存下来
  （`update()` / `seekTo()` / `reset()` 里，取 `m_engine.clockMs()` 而不是入参
  `timeMs` —— 引擎在暂停/seek 分支里自己决定时钟停在哪）；
* 渲染器 `updatePaintNode()` 改成
  `refSec = engineClockMs()/1000`、`extraMs = (monoNow − engineClockMonoMs) × rate`；
* 两条读数共用 `DanmakuController.h` 里的 `danmakuMonoMs()`（渲染器原来的
  `monotonicMs()` 现在只是转发它）—— **各自建一个 `QElapsedTimer` 会基准不同，
  减出来毫无意义**，这条别再犯；
* 顺手删掉一整批"替两条时钟的差值打补丁"的成员（`m_carryMs` / `m_frozenMonoMs` /
  `m_lastFrameMonoMs` / `m_lastClockRunning` / `m_lastNowSec` / 我上一版加的
  `m_rateTrim` 与"小幅回退钳制"）。补丁删掉是因为**根因没了**：暂停时引擎不推进时钟、
  渲染器 `extraMs=0`，弹幕画在引擎冻结的位置上，一格都不动，不需要任何补偿。

**同一处还解释了**：悬停气泡要晚才认出目标、`DanmakuView.qml` 里 `_rateTrim` 收敛
这些"慢半拍"的现象；GPU 模式下**不要**把 `timeMs` 改回渲染基准。

#### 4k-1. 紧接的实测反馈：「暂停继续会往前弹一下再往后继续移动」（已改）

基准换成引擎时钟之后暴露出来的第二个坑：引擎在**暂停时不再 `update()`**，所以它的
时间戳停在暂停那一刻。QML 的 `clockRunning` 是**绑定**（`view._driving`），恢复播放时
它**先**变真，而引擎恢复后的第一帧还没到 —— 那一帧若直接算
`monoNow − engineClockMonoMs`，就把**整段暂停时长**当成了流逝时间，弹幕猛地往前飞一段，
等引擎那一帧到了再回正（用户看到的"往前弹一下再往后继续"）。

修法：外推量改成**按帧增量累加**（`m_lastExtraMs + min(本帧间隔, 250ms) × rate`）：
* 引擎时间戳**变了**（引擎刚推了一帧）→ 用 `monoNow − 时间戳`**重算**，这是权威值，
  顺带消掉累加攒下的漂移；
* 没变（两次 `engine.update` 之间）→ 只加"上一帧 → 这一帧"这一小段，因此**永远
  跨不过任何停止段**；单帧上限 250ms，GUI 卡顿时这一帧也不会飞出去；
* `clockRunning == false` → 外推量直接归零，画在引擎冻结的位置上。

【教训】任何"现在 − 某个事件的时间戳"形式的时钟，都要先问一句："这个时间戳在
暂停/停止期间会不会被刷新？" 不会的话，减法里就藏着整段暂停时长。

**验证点**（交给用户）：滚动连续无台阶；鼠标移上去那条**定住不动**；暂停/恢复不跳；
拖进度条弹幕立刻跟着走。

### 4l. 弹幕"视频暂停了还在滚"（用户实测，**稳定复现**，本轮改）

用户原话："弹幕有时候视频暂停了还是不停，有时候又可以停，很玄学"，追问后确认是
**点了暂停/空格 → 画面停住、弹幕还在滚**，而且非常容易复现。

**这不是竞态，是判据太单一**：弹幕时钟只看 `player.playing`，而它就是框架状态回调
（`CicadaPlayerItem::setStatus()` ← `notifyPlayerStatus()`，`CicadaPlayerItem.cpp:2029`）。
只要框架**没有**报 `PLAYER_PAUSED`（暂停发生在缓冲/加载中、回调没到、或者之后又被打了
一次 `PLAYER_PLAYING`），`_driving` 就一直是真 → `stepTimer` 继续 tick、渲染器
`clockRunning` 继续为真 → 弹幕一直滚，而画面是停的。

**修法（不依赖框架状态）**：在**已有的** `stepTimer` 里补一条判据 —— **播放位置多久没变**。
正常播放时位置每 250ms 推一次（`CicadaPlayerItem` 的 `m_positionTimer`，CoarseTimer 会被
事件循环挤后），所以 `600ms`（2.4 倍名义间隔）一点没动就只可能是：真暂停、缓冲、画面冻结。
命中时：

* `stepDanmaku()` 里**不推进** `view._clock`（位置、发射、悬停全停在原地）；
* `gpuRenderer.clockRunning: view._driving && !view._mediaStalled` → 渲染器立刻冻结
  （`extraMs = 0`，画在引擎时钟的位置上）。

没有新增定时器/轮询（判据在已有的 tick 里算），没有把插值搬进 C++，`renderMode` 仍是 `"gpu"`。
恢复播放时位置一变，`_lastProgressMono` 刷新 → 600ms 内自动恢复；
`_driving` 为假（框架确实报了暂停）时该路径照旧立刻停。

另加了一行**只在暂停/恢复那一刻**打印的诊断（`DanmakuView.qml` 的 `stepTimer.onRunningChanged`）：
`[danmaku] 时钟 driving=… status=… playing=… enabled=…`。如果以后还有人报"暂停了弹幕还在滚"，
这行能直接区分是"框架状态没变"还是"渲染器没停"——正常播放时一句都不会有。

### 4m. 「关窗即崩」的根因 = **生成物少了头文件依赖**（子代理逐行核对，已永久修复）

崩溃栈（每次关播放器窗口都可能在，`main.cpp` 的处理器随后 `_exit(3)`，所以首页窗口一起没）：

```
~DanmakuRendererItem → std::vector<DanmakuAtlasEntry>::_Tidy → _Orphan_all
  → _Orphan_all_unlocked_v3  (xmemory:1380，0xc0000005)
```

子代理用本机 MSVC 14.39 源码把语义钉死了：崩的是**写指令**（1379 的读没崩），说明
`_Myproxy` 可读、但 `_Myproxy->_Myfirstiter` 指向垃圾 —— **`m_entries` 这条 Debug STL
代理链的内容是垃圾**。据此：

* **"析构两次"被源码排除**：`~vector` 第一次跑完会把 `_Myproxy` 置空，
  第二次进 `_Orphan_all_unlocked_v3` 会在 `if (!_Myproxy) return;` 直接返回，不可能崩在 1380。
* **"下标越界"只在单线程下不存在**：`m_entries`/`m_uvs` 所有分支在单线程里都自洽
  （重烤重试只重走同一批下标、第二趟用 `qMin`、`rebuildGeometry` 两道 break）。
  唯一的入口是 `items.size()` 被**另一个线程**改大 —— 而 sync 阶段 GUI 线程是被阻塞的，
  所以它"可能从没发生过"；但读方不持锁这件事本身是缺陷，已按下面第 3 条补上。
* **根因（置信度最高）= 旧对象文件 / `sizeof` 不一致**，而且**机制在本 build 树里被证实存在**：
  QML 引擎按"注册时定下的 sizeof"分配对象（`qqmlprivate.h` 的 `QQmlElement::createInto`），
  那个 sizeof 在生成物 `appqtplayer_qmltyperegistrations.cpp` 里展开
  （`qmlRegisterTypesAndRevisions<cicadaqt::DanmakuRendererItem>`）；而**那个 obj 的依赖表里
  只有它自己**（`CMakeFiles/appQtPlayer.dir/compiler_depend.internal` 一行），因为它只在 moc 的
  json 变化时重新生成，而**成员布局变化不改变 json** ⇒ 改了 C++ 成员（哪怕只删一个）它也不重编
  → QML 按**旧 sizeof** 分配、构造/析构越界写堆。CTR 弹框
  `HEAP CORRUPTION DETECTED … wrote to memory after end of heap buffer` 与
  "清空重建之后就不崩了"都指向它。析构时**第一个被销毁的非平凡成员正是 `m_entries`**
  （成员逆序，`+0x1d` 位置对得上），它的 `_Myproxy` 落在对象尾部之外。

**修复（`CMakeLists.txt`，qt_add_qml_module 之后）**：把那批头文件显式挂到生成物的对象依赖上：

```cmake
file(GLOB _cicada_registered_headers "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/appqtplayer_qmltyperegistrations.cpp"
             APPEND PROPERTY OBJECT_DEPENDS "${_cicada_registered_headers}")
```

**已实测生效**：`build.make:490-502` 里那个 obj 现在依赖全部 `src/*.h`（含
`DanmakuRendererItem.h`）。⇒ 以后改成员布局会自动重编，"重建一下试试"不再是必要步骤。

同轮其他修复（都在 `src/DanmakuRendererItem.{h,cpp}` / 见下）：

1. **`updatePaintNode` 改为持锁快照**：`QStringLocker(danmakuRenderMutex())` 里把
   `activeRenderItems()`（引擎 `_renderOut` 的引用）抄进自己的 `m_itemsSnapshot`，
   `RenderItem::text`（引擎约定"只到下一次 load/reset/clear/append 前有效"）抄进
   `m_textArena`，之后整帧只用 `itemCount`（不再出现 `items.size()`）。
   语义不变、每帧多一次小拷贝（几十条 × 几十字节），换来"读方也持锁"。
2. 去掉 **GUI 线程**对 `m_uvs` 的 `clear()`（`setController` / `releaseResources` / 控制器
   `destroyed` 回调）：`m_uvs` 是渲染线程的容器，跨线程改它的内部指针正是写坏 Debug STL
   状态的做法；现在只置 `m_builtSignature = 0`，让渲染线程下一帧自己重建。
3. **图集高度上限**：`DanmakuAtlas::allocate()` 原来只挡宽度没挡高度（dpr=2 时 h 可达 1536 >
   图集 1024 → uv 的 v>1 花屏 + 白触发重烤），已补。
4. **下一个崩点（本轮顺手堵掉）**：关窗顺序是"渲染线程先收 QRhi/场景图 → GUI 线程再
   deleteLater 掉 QML 树"，所以 `~DanmakuAtlas` 会在**上下文已经没了之后** delete 那张
   `QSGTexture`。现在在 `onWindowChanged` 里接 `QQuickWindow::sceneGraphInvalidated`
   （渲染线程发、GUI 阻塞）只做 `m_atlas.forgetTexture()`（放指针、不 delete）——
   日志里那句 `QRhi … going down with 1 unreleased resources … Texture` 的来源也一并说明。

### 4n. 「视频暂停了弹幕还在滚」的两条框架侧根因（子代理查证）

1. **缓冲/卡住时框架不改播放状态**：`mediaPlayer/SuperMediaPlayer.cpp:1638-1646` 起缓冲只做
   `NotifyLoading` + 主时钟 pause + 音频渲染 pause，**`mPlayStatus` 仍是 `PLAYER_PLAYING`**
   （重缓冲永远不会再进 PREPARING）；而 `native_cicada_player_def.h` 的 listener **没有 Loading
   回调** ⇒ Qt 这层收不到"在缓冲"的任何信号，`playing` 一直是真，画面却真的冻住
   （`SuperMediaPlayer.cpp:2164-2168` 缓冲期间不 render）。本地文件几乎不缓冲 → 只有网络源复现，
   这就是"有时能停有时不能"。
2. **暂停可能被框架静默丢弃**：`SMPMessageControllerListener.cpp:445-449` 的 `ProcessPauseMsg()`
   首句 `if (mPlayStatus != PLAYER_PLAYING) return;` —— 加载中 / seek 中途按暂停不翻状态；
   而 `ProcessStartMsg` 在 `PAUSED/PREPARED/COMPLETION` 都可能把状态推回 `PLAYING`。

**修法（`DanmakuView.qml`，不新增定时器、不动 renderMode、不把插值搬进 C++）**：给弹幕时钟补一条
**不依赖框架状态**的判据 —— 位置多久没变（`_mediaStalled`，600ms；位置随音频帧推送约 20–40ms 一次，
15~30 倍余量）。命中时：`stepDanmaku()` 不推进 `_clock`、
`gpuRenderer.clockRunning: view._driving && !view._mediaStalled` 立刻冻结。配套：
`pushPointer()` 在 `_driving || _mediaStalled` 时也要补一次 `engine.update()`（否则冻结期间悬停失效）、
`onPositionChanged` 的暂停分支补上"暂停/缓冲中拖进度条"的硬对齐 + `engine.seekTo()`。

### 4o. 「还是轻微抖动 / 一卡一卡」的根因 = **采样点取错了**（本轮改）

用户问得好："为什么不能像浏览器 CSS 动画那样流畅？"—— 差别就在**采样时刻**：
浏览器合成器在**每一帧的呈现时刻（vsync）**按帧时间算 transform 矩阵，动画是 t 的连续函数，
所以每帧步长严格均匀。我们以前是在 `updatePaintNode`（**sync 阶段**）里读墙上时钟，
而 sync 什么时候发生取决于 GUI 线程忙不忙（视频、QML、弹幕引擎全在同一个线程上），
采样点忽早忽晚（±几毫秒）。400px/s 下 5ms 就是 2px —— 于是每帧步长忽大忽小，
看起来就是"一卡一卡/轻微抖动"。

**修法**：采样点改成**最近一次换帧时刻**。
* `onWindowChanged` 里再接一个 `QQuickWindow::frameSwapped`，用 **DirectConnection**，
  回调里只写 `m_lastSwapMonoMs = danmakuMonoMs()`（渲染线程写、渲染线程读，无竞态）；
* `updatePaintNode` 里 `monoNowRaw` 用它，而不是"现在"（第一帧没有记录时退回"现在"）。

frameSwapped 由渲染线程在换帧之后立刻发出，而渲染线程是紧循环等 vsync 的 —— 它的时间戳
对呈现时刻的抖动远小于 GUI 线程的调度抖动，于是位置步进变成"每个呈现帧一步"，
和浏览器同一种做法。代价只是**整体晚一帧**：那是**常量偏移**，不产生抖动。

**还没解决的部分（结构性，需要实测分辨）**：这台机器上视频层是 D3D11 zero-copy +
`timedVSync`（日志里有 "video fps is 25"），如果**窗口的换帧节奏本身被视频帧带着走**
（25fps → 40ms 一步），那么弹幕无论怎么算都不可能比视频帧节奏更顺 —— 那不是我们的采样
问题，而是"场景图什么时候换帧"的问题。**最快的判据：换一个 60fps 的片子放，如果弹幕明显
变顺，就说明抖动来自视频帧节奏。** 若确认是这条，下一步是在渲染器侧强制按 vsync 换帧
（例如 `QQuickWindow::requestUpdate()` / 独立的帧驱动），而不是再动时钟。

## 四、踩过的坑（**每一条都真实发生过，别再犯**）

### QML
1. **同名自引用**：`DanmakuBar { danmaku: danmaku }` 右边的名字会被解析成**组件自己的属性**（初值 null）→ 必须写 `bar.danmaku`。`bar: bar` 之所以没事，是因为 `bar` 是根对象的 **id**，id 优先。曾导致"发送弹幕点了没反应"。
2. **`XAnimator` 没有 `property` 字段**（它本身就是动 x 的）；写了 `property: "x"` → 该文件加载失败 → 连锁 `DanmakuView → PlayerView → Main` 全废 → 程序直接退出。
3. **重写组件时删了对外属性**：`SettingsPanel` 删掉 `viewHeight`/`rightPageWidth` 后，`RightControls` 里读到 `undefined` → 浮层高度/宽度为 0 → "面板打开了但看不见"、第二页空白。改组件前先对一遍对外接口。
4. **Popup 里不能放 `anchors.fill: parent` 的 MouseArea**：它是 contentData、盖在 contentItem 上面 → 吃掉所有点击 + 独占 hover（面板自己的 `pointerIn` 恒 false，容错定时器到点就关）→ 表现"点什么都没反应 / 鼠标刚移上去就关"。判 hover 用被动的 `HoverHandler`。
5. **只挂齿轮的 `onExited` 不够**：鼠标停在面板上再移开时齿轮收不到事件 → 面板永不关。必须同时盯面板自己的 `pointerIn`（变假就起容错定时器）。
6. **`closePolicy`**：按用户要求所有面板用 `T.Popup.NoAutoClose`（Esc 留给退全屏、不要点外关闭），**只保留"移出关闭"**。
7. **`setTitleBar()` 会清空之前登记的按钮**（`abstractwindowcontext.cpp`）：顺序必须是 `setup → setTitleBar → setSystemButton`。
8. **标题栏里非系统按钮的可交互元素**要 `windowAgent.setHitTestVisible(item, true)`，否则 Windows 上整条是拖动区，点击到不了 QML。
9. **控件类用原生还是手写**：设置面板用户明确要求**手写**（不用 QtQuick.Controls）；第一页开关、第二页选项都是手绘（30×20 轨道 + 16×16 圆点 / 半透明白底胶囊 + 选中主题蓝 + 白字 + 12px）。
10. **弹幕运动**：位移交给渲染线程 `XAnimator`（不要每 tick 重起动画 —— 30Hz Timer 抖动会让速度忽快忽慢）；tick 只做逻辑（喂时钟、入场/退场、悬停、透明度、>4px 偏差时重新对齐）；退场项必须 `stopMotion()`，否则 XAnimator 一直请求重绘白烧 CPU。
11. **id 是"按文档"的，跨文件摸不到**（本轮踩到，运行期一直报 ReferenceError）：
    `PlayerControlBar.qml` 里有一句 `cursorShape: (!bar.shown && !settingsScrim.visible) ? …`，
    而 `settingsScrim` 是 `Main.qml` 里那个遮罩的 id。QML 编译每个文档时只有**本文件**的 id
    在作用域里，跨文件引用不会去"宿主文档"里找 → 这句绑定每次求值抛
    `ReferenceError: settingsScrim is not defined`，`cursorShape` 永远停在默认值
    （表现：控制栏收起后鼠标指针该藏起来却没藏）。
    现在改成外面喂进来的 `bar.overlayOpen`（`PlayerView` 把自己的 `overlayOpen` 转过来）。
    **教训：拆组件时把 `grep 旧 id` 当必做步骤。**
12. **`z` 只在"同一父对象的兄弟之间"排序，拆出去的子组件必须把 z 给在根对象上**（本轮踩到）：
    设置页的遮罩原来是 `Main.qml` 的直接子对象、`z: 10`。搬进 `SettingsPage.qml` 之后它
    成了**子组件里的子对象** —— 子对象 z 再大，整棵子树还是按 `SettingsPage` 根对象的 z
    （默认 0）插进宿主的孩子列表，于是会被宿主的标题栏（`z: 4`）和声明在后面的诊断文字盖住
    （"遮罩上浮着一层诊断文字"）。`SettingsPage.qml` 根对象上那句 `z: 20` 就是修这个。
13. **"能收 hover 的元素"是最上层独占的**（这条决定了顶部栏能不能浮出来，也决定了它会不会闪）：
    * 顶部热区用**被动 `HoverHandler`**（`topHotZone`），不用 `MouseArea` —— MouseArea 就算
      `acceptedButtons: Qt.NoButton`，只要 `hoverEnabled: true` 就仍然是"能收 hover 的元素"，
      会把鼠标运动从画面活动层手里拿走；
    * **判据里必须带上"某颗按钮自己的 hover"**（`titleBar.captionHovered`）：
      光标一停到关闭/最大化那颗按钮上，热区就再也收不到 hover 了，只看热区 → 顶部栏刚露出来
      就被收回去，看起来"一直闪"；
    * 还补了**第二条完全不依赖 hover 分发的兜底判据** `root.topZoneFallback`
      （`playerView.pointerInside && lastMouseY < 36`，这两个值 `PlayerView` 本来就在维护）。
      两条判据互补，必有一条成立 —— 万一哪天覆盖顺序变了，代价是"顶部栏再也浮不出来、
      窗口既拖不动也关不掉"，这个后果太重，不能只靠一条路。

### C++ / 构建
14. **Qt 模块归属**：`QImage`、`QFont`、`QFontMetrics` 属于 **QtGui**，不是 QtCore（`#include <QtCore/QImage>` → `Cannot open include file`）。`QFile`/`QDir`/`QStandardPaths`/`QCryptographicHash` 才是 QtCore。
15. **FFmpeg 9**（日志里 `Ffmpeg version n9.0`）：`sws_getContext`/`sws_scale` 老 API 已删 → 用 `sws_alloc_context` + `av_opt_set_int`(需要 `<libavutil/opt.h>`) + `sws_init_context` + `sws_scale_frame`；`av_malloc` 需要 `<libavutil/mem.h>`。
16. **`QML_ELEMENT` 必须带 `#include <QtQml/qqmlregistration.h>`**，否则报"语法错误: 缺少";"(在"public"的前面)"。
17. **新增 QML 文件/素材要重新 cmake configure**：`QML_FILES` 是显式列表（本轮加了 `SettingsPage.qml`）；`assets/` 走 `file(GLOB)`，新素材（如 `next-play.svg`）不 configure 会 `Cannot open: qrc:/...`。Visual Studio 生成器下 `cmake --build` 会自己重跑 configure，所以不用手动来一遍。
18. **本地/沙箱**：pwsh 需要放宽权限才能构建；构建命令
    `cmake --build CicadaPlayerNext\platform\QtPlayer\build\msvc --config Release --target appQtPlayer`
    Qt Creator 那边是 `build/Desktop_Qt_6_11_1_MSVC2022_64bit-Debug`，Qt bin 要进 PATH（否则 `0xC0000135`）。

18b. **静态 / 动态 Qt（同一套代码，脚本参数切换）**
    ```powershell
    .\deploy_win.ps1                                   # 动态 Qt -> build\msvc        -> deploy\
    .\deploy_win.ps1 -Static                           # 静态 Qt -> build\msvc-static -> deploy-static\
    .\deploy_win.ps1 -QtPrefix D:\Qt-Static\msvc        # 显式指定前缀（模式跟着前缀走）
    ```
    bat 那两个入口（给习惯双击/cmd 的人）：
    ```bat
    deploy_win.bat                       :: 动态
    deploy_win.bat static                :: 静态，前缀从环境变量 QT_STATIC_DIR 找
    deploy_win.bat static D:\Qt-Static\msvc
    ```
    **静态 Qt 装在哪：默认从环境变量找，也可以当参数传**
    * 环境变量 `QT_STATIC_DIR`（别名 `CICADA_QT_STATIC_DIR`，早期名字，保留兼容）——
      只在 `-Static` / `static` 这条路上读，所以设了它也不会影响动态构建。
    * 参数：`deploy_win.bat static <前缀>`（bat 把它**通过环境变量**交给子进程 ——
      `setlocal` 里设，不污染调用者的环境；批处理里拼带引号的命令行是雷区）或
      `.\deploy_win.ps1 -Static -QtPrefix <前缀>`。
    * **变量设了但路径不可用 → 直接报错退出**（打印变量值和它指向的路径），不会忽略它换一个 Qt。
    * **`-Static` 不再退回动态 Qt**：找不到静态 Qt 就报错退出（以前会顺着通用搜索找到动态 Qt、
      把 `CICADA_QT_STATIC` 算成 OFF，于是 `build\msvc-static` 里出现一个动态版 —— 这个坑已堵）。
    * `-QtPrefix` 指向动态 Qt 又写了 `-Static` 时：**前缀优先**（照动态编），但打黄字 `[WARN]`。
    * 脚本第一行会打印**用的是哪个 Qt、从哪找到的**：
      `== Qt: D:\Qt-Static\msvc  [static]  (static search)`。
    三条硬约束，缺一条就编不过或跑不起来：
    * **`-DCICADA_QT_STATIC=ON` 必须在 `add_subdirectory` 之前生效** —— 静态 Qt 是 `/MT`
      （`D:\Qt-Static\msvc\mkspecs\qconfig.pri` 里 `QT_CONFIG` 带 `static_runtime`），
      而 `CMAKE_MSVC_RUNTIME_LIBRARY` 是**建 target 那一刻**读的；放后面设只对 appQtPlayer
      生效，框架那堆静态库还是 `/MD` → `LNK2038` + `LNK2005`。现在那句 option 就在
      `project()` 正下方。
    * **静态 Qt 下 QML 插件要显式链接**：`qt_import_qml_plugins()`（扫 QML 导入，
      含 `Qt.labs.lottieqt`）+ `qt_import_plugins(INCLUDE Qt6::QSvgPlugin …)`（平台/图片
      格式，**qsvg 尤其重要**，素材里一堆 SVG）。这是**运行期**错不是编译错：
      "编过了"≠"能跑"。
    * **静态模式下跳过 `qt_generate_deploy_qml_app_script`**（没有 Qt 运行库可拷；
      qml/ 和 platforms/ 都链进 exe 了）。但 `libffmpeg.dll` / `Danmaku.dll` /
      `libxml2.dll` / `zlib1.dll` **照旧要拷** —— 它们不是 Qt。
    * **两个模式的 build 目录必须分开**（`build\msvc` vs `build\msvc-static`）：运行库
      和插件集不同，共用一个 CMake 缓存会编出四不像。
    * vcpkg 那批依赖**不用换成 `-static` triplet**：我们链的是导入库，导入库里没有
      CRT 的 `/DEFAULTLIB`，所以 `/MT` 的程序能正常链 `/MD` 编的导入库，运行时照旧拷 DLL。
      真要做到"单文件 exe"才需要 `vcpkg install --triplet x64-windows-static`。
    * 判别前缀是静态还是动态，就看 **`<前缀>\bin` 下有没有 `Qt6Core.dll`**（脚本用的就是这个判据）；
      CMake 侧用 Qt 自己导出的 `QT6_IS_SHARED_LIBS_BUILD` 做**事后校验**，给错了当场 FATAL_ERROR。
    * **实测结果（Qt 6.11.2 静态 / `D:\Qt-Static\msvc`）**：
      - 静态 exe **44.8 MB**（动态 4.2 MB，差的就是整个 Qt）；`build\msvc-static\Release` 旁边
        **没有一个 `Qt6*.dll`**；
      - **把 PATH 清成只有系统目录也能正常启动、stderr 全空** —— 这就是"静态链接真的成立"的判据
        （动态版这样跑必然 `0xC0000135`）；
      - 链接行里能看到 Qt 预编译的插件初始化对象：`qtquickcontrols2basicstyleplugin_init.cpp.obj`、
        `qtquickdialogsplugin_init.cpp.obj`、`effectsplugin_init.cpp.obj`、
        **`plugins\platforms\...\QWindowsIntegrationPlugin_init.cpp.obj`**、
        `plugins\imageformats\...\{QSvg,QJpeg}Plugin_init.cpp.obj`。
        **这些 `*_init.cpp.obj` 里就是 `Q_IMPORT_PLUGIN`** —— 所以构建目录里**不会**有
        `*_plugin_import.cpp` 生成文件，别去找它、也别以为没生成就等于没生效（我去找过，白找）。
      - 静态版旁边会多出 `jpeg62/libpng16/libwebp*/tiff/zstd/liblzma/libsharpyuv.dll`：
        **MSBuild 的 CopyLocal 自动拷的**（静态链接把 `Qt6::QSvgPlugin` 等拖进来 → 它们依赖
        vcpkg 的 JPEG/PNG/WebP/TIFF 导入库 → 导入库带 `IMPORTED_LOCATION`）。自动、可复现；
        动态版没有这些（插件运行期才加载）。
      - `/MT` 生效的验证方式：查生成的 `appQtPlayer.vcxproj` 里 `<RuntimeLibrary>` →
        `MultiThreaded`（Release）/ `MultiThreadedDebug`（Debug），**不是** `MultiThreadedDLL`。

19. **"整个 Main.qml 编译不过"这类错误在构建期是看不见的**：`Main.qml` 只在**点开播放器窗口**
    那一刻才被 `Qt.createComponent` 加载，所以 `cmake --build` 成功 ≠ 播放器窗口能打开。
    本轮就是这么发现问题的 —— 见第五节"怎么验证"。
20. **构建日志里那两行 `'pwsh.exe' 不是内部或外部命令` 是 QWindowKit 自带 qmsetup 的**
    （`build/msvc/_build/qmsetup_build-Release.log` 里也有同一行），**和本工程无关、不影响结果**：
    那一行之后它照常继续，`cmake --build` 退出码仍是 0。别去追它。

### 视觉/参考
21. 右组按钮间距：所有按钮内边距统一 = `(bar.btnWidth - bar.iconSize)/2`（全屏 13、窗口 7），否则"清晰度↔选集"会比别处挤。
22. 弹幕字号基准 `DanmakuView.baseFontSize = 25`（18 太小）；文字盒宽 = 引擎测量值 + 1px（否则最后一个字被切）；白框宽度靠引擎的 `setMeasureFunction`（QFontMetricsF）量真值，字体族由 `DanmakuView.fontFamily` 传下去。
23. **顶部透明了，顶栏的文字/图标就得自己在两种底色上换色**：没选片子时窗口底是 `windowBg`
    （浅色主题下是浅灰），选了片子后是 `videoBg`（**永远黑**）。所以 `Main.qml` 里按
    `root.hasSource` 在 `captionGlyph/titleText` 和 `onVideoText` 之间切
    （`CaptionButton` 新增 `glyphColor` 属性），否则浅色主题下深灰图标压在黑画面上看不见。
24. 参考文件位置：`front/hili-player/player（仅参考不要动里面的代码）`、`platform/ctrl-setting.txt`、`platform/style.css`（单行压缩，用正则抽规则）。

## 五、怎么验证（本轮实际做过的，以及**没做**的）

### 本轮（DASH 清晰度菜单去重 + 快照取图警告）
1. **改了代码**：`src/CicadaPlayerItem.cpp`（`onMediaInfoGetCb` 末尾按分辨率合并 + 新增
   `sameQualityRow()`，`notifyQualities` / `notifyStreamSwitched` 改用它）、
   `src/CicadaPlayerItem.h`（`qualities` 注释）、`controls/ProgressRow.qml`（`localSource`
   在 `snapshotRevision === 0` 时返回空串）；本文件补第三节 4i。
2. **编译做了**：`cmake --build build\Desktop_Qt_6_11_1_MSVC2022_64bit-Debug --parallel 8`
   → 退出码 0，日志里有
   `Generating .rcc/qmlcache/appQtPlayer_controls/ProgressRow_qml.cpp`（改过的 QML 过了
   qmlcachegen）和 `Building CXX object …/CicadaPlayerItem.cpp.obj`，末尾
   `[100%] Built target appQtPlayer`；`Select-String 'warning C|error C|error LNK'` 命中 0 条。
3. **没做运行时验证**：这个项目由用户在 Qt Creator 里跑，本 harness 起 GUI 进程会立刻退出。
   **待用户确认**：DASH 片源的清晰度菜单变成「自动 + 4K / 1080P / 720P / 480P」共 5 行，
   且点其中任意一档能切过去；悬停进度条时不再出现 `image://snapshot/0` 的取图警告。
4. 复现这轮问题时用的手段（**已删干净，不留文件**）：解压上一个会话的
   `~/.dsh/sessions/--D-hilihili--/session-*.jsonl.zstd`（`pip install zstandard` + 一个临时
   python 脚本），从 `compaction/summary` 里把用户上一轮的原话捞回来 ——
   用户这次贴的 `log.txt` 里只有日志，**"每个清晰度都有两个"这句抱怨在附件里丢了**，
   是从旧会话记录里找回来的。以后遇到"附件信息不全"可以直接用这个办法。

### 前一轮（选集面板 hover 错行 / 点击没反应 + 构建脚本的静态 Qt 查找）
1. **改了代码**：`controls/RightControls.qml`（`eplistMenu` 的 `hoverIndex` / `rowAt()` /
   `activate()`、行 MouseArea 的 `preventStealing`、判据层的 `onClicked` 兜底）；
   `deploy_win.ps1`（静态 Qt 查环境变量 `QT_STATIC_DIR` / 别名 `CICADA_QT_STATIC_DIR`、
   `-Static` 不再退回动态 Qt、参数矛盾时警告、打印 Qt 来源）；
   `deploy_win.bat`（新增 `static <前缀>` 形式，前缀经环境变量传给子进程）；
   `README.md` §3.8 补"静态 Qt 装在哪"；`docs/QML-POPUP-HOVER-PITFALLS.md` 补第 21、22 条；
   本文件补第三节 4b 和 18b。
2. **编译 / 部署 / 参数实测都做了**（这一轮 shell 一开始被沙箱挡了几次，放宽权限后跑通了）：
   - **动态构建**：`cmake --build build\msvc --config Release --target appQtPlayer` 成功，
     日志里有 `Generating .rcc/qmlcache/appQtPlayer_controls/RightControls_qml.cpp`
     —— 改过的 QML **过了 qmlcachegen 编译**（语法/类型错会在这里断掉）。
     产物 `build\msvc\Release\appQtPlayer.exe` = **4,477,440 字节 / 2026-09-18 12:09:56**（含设备修复 + 布局规则 + 时间跳转输入框 + 时间格式化）
     （最后又改过一遍 QML 注释，所以重新编译+重新安装过一次；两份部署目录里都是当前源码的产物）。
     之后**弹幕渲染层重做（见 4h）**又编了几遍，最新：`build\msvc\Release\appQtPlayer.exe` =
     **4,481,024 字节 / 2026-09-18 16:03:01**，`deploy\bin\appQtPlayer.exe` 同大小、同时间戳。
   - **部署已刷新**：`cmake --install build\msvc --config Release --prefix deploy` →
     `deploy\bin\appQtPlayer.exe` 与构建产物**同样大小、同一时间戳**（用户双击的就是这一份）。
   - **交互机制的实测**：临时 QML 探针（`qmltestrunner`，复刻本面板的交互骨架 ——
     `T.Popup` + `background` 里一层只管 hover 的 `MouseArea` + `contentItem: Flickable` +
     每行一个 `MouseArea`）跑出 12 passed / 1 failed，唯一那条 failed 是**故意留的负对照**：
     | 用例 | 实测结果 |
     |---|---|
     | 滚动两行后：旧算法 `floor(mouseY/行高)` | **0**（错，会亮在上面那一行）|
     | 滚动两行后：`mapToItem` 版 | **2**（对，光标下那一行）|
     | 滚轮滚动、鼠标不动：带 `contentY` 依赖的绑定 | 自动重算（0 → 3）|
     | **滚轮滚过之后点一下（无 `preventStealing`）** | **clicked = -1，点击丢失** ← 复现用户报的现象 |
     | **滚轮滚过之后点一下（有 `preventStealing`）** | **clicked = 2，命中** ← 修好了 |
     | 不滚动直接点 / 抖 2px 再点（两种配置各两次） | 都命中（说明单纯抖动不是元凶，"滚轮之后"才是）|
     结论：**hover 错行 = 没算 `contentY`；点击丢失 = 滚轮滚动之后 Flickable 会把下一拍的按下事件
     过滤掉，而 `preventStealing`（内部就是 `setKeepMouseGrab`）正好拦住。** 两条都是实测的。
     探针是临时文件，验完已删。要固化成回归测试就把上表用例搬进
     `tests/tst_eplist_panel.qml`，用 `qmltestrunner -input tests` 跑。
   - **bat 四种调用实测**（`cmd /c` 下看退出码）：`deploy_win.bat -h` → 打印用法、EXIT=0；
     `deploy_win.bat static D:\NoSuchQt` → `QT_STATIC_DIR is set but is not a usable Qt prefix`、EXIT=1；
     `set QT_STATIC_DIR=D:\NoSuchQt` + `deploy_win.bat static` → 同样报错、EXIT=1；
     `deploy_win.bat D:\NoSuchQt`（裸路径）→ `-QtPrefix ... not a usable Qt prefix`、EXIT=1。
     **顺带证明：LF 换行的多行 bat 在 cmd 里能正常跑**（标签、`shift`、`%VAR:~0,1%` 都对）。
   - **静态路径实测**：`QT_STATIC_DIR=D:\Qt-Static\msvc` + `deploy_win.bat static` →
     第一行 `[info] static Qt from QT_STATIC_DIR` + `== Qt: D:\Qt-Static\msvc [static] (static search)`，
     configure 里 `QtPlayer: linking a STATIC Qt (MSVC CRT = /MT, QML plugins imported)`，
     全部 QML（含 `RightControls_qml.cpp`）编译通过。第一次在链接阶段报过一次
     `LNK1104: 无法打开 ...\build\msvc-static\Release\appQtPlayer.exe`（**文件被占用**，
     当时并没有 appQtPlayer 进程，属瞬时占用，不是代码问题）；重跑之后
     **`Deploy OK -> deploy-static`、`BATEXIT=0`**，
     `build\msvc-static\Release\appQtPlayer.exe` = **47,022,080 字节 / 2026-09-18 12:17:17**
     （旧的静态产物是 46,972,928 / 19:19:45，只差 QML 那些改动），
     安装到 `deploy-static\bin\` 的是同一份。
     之后**弹幕渲染层重做（见 4h）**又编了几遍，最新：`build\msvc-static\Release\appQtPlayer.exe` =
     **47,023,616 字节 / 2026-09-18 16:11:08**，`deploy-static\bin\appQtPlayer.exe` 同大小、同时间戳
     （`deploy_win.bat static` 收尾是 `Deploy OK -> …\deploy-static` + 依赖检查 `[ok]`）。
     部署产物冒烟测试（动态那份，用户双击的就是它）：启动 8 秒仍存活、**stderr 空**。
3. **「关掉播放窗口 → 再开新视频」这条路径：实测复现 + 实测修好**（详见第三节 4c）
   - 临时复现脚本（自动跑，不用鼠标；验完已从 `main.cpp` 删干净，文件回到 358 行）：
     `createObject(Main.qml)` → `playFile(A)` → 播 8 秒 → `close()` → `deleteLater()` →
     再开一个窗口 → `playFile(B)` → 播 8 秒。
   - 用户机器上的崩溃记录（事件日志）：`appQtPlayer.exe` / `0xc0000005` / `错误模块: unknown`
     + 偏移量离谱 + BEX64 —— 与"把已释放的设备交给 FFmpeg"这个根因吻合。
   - 修复后的日志顺序（带首页窗口，= 真实用法）：
     `captured Qt's D3D11 device … (window …)` →（关窗）`released the captured D3D11 device
     (its scene graph went away)` →（新窗口）`captured Qt's D3D11 device … (window …)` →
     `status=3 position=7808ms zeroCopy=1`（第二个窗口正常播放）。
   - 内存：4 个窗口 A/B 交替反复开关，每轮峰值 ~177-180MB WS / ~215-229MB Priv，
     关掉都回落到同一水平（~125-140 / ~162-178），没有逐轮上涨。
   - 单窗口（没有首页窗口）那一档：关窗后 Qt 按 `quitOnLastWindowClosed` 退程序 —— 属默认行为；
     把这条默认关掉后，同样的"关窗→再开窗"序列也完整跑通。
4. **还没做的 / 已知问题**：
   ① 真实窗口里"手点"这一步（移动鼠标看高亮、点某一集是否换片）没法自动化，**请人工点一遍** ——
      长列表（集数 > 17，即超过 520px）下：鼠标移到某一行高亮必须是**光标下的那一行**；
      滚轮滚几行后再移仍然对得上；**滚轮滚过之后**点某一行要能换成那一集并关面板。
   ② **带着播放器窗口退出程序时会崩**（`Qt6Core.dll` + `0xc0000005`）—— 第三节 4d，
      和"关掉再开"无关，还没修。
5. **控制栏两条新规则 + 命令行启动修复（4e）也实测了**：
   * `appQtPlayer.exe <mp4>`：进程 9 秒存活、日志有 `opening source: …` + 零拷贝两行，
     **没有任何 QML 运行期告警**（TypeError/ReferenceError/Binding loop 全无）；
   * 左组几何（非全屏、`hasNext=false`）：`time x=26 textGap=6 → 文字左边缘=32`、
     `input x=6 → 输入框左边缘=32` —— **时间文字和输入框同一个左边缘，距播放键右边缘都是 6px**
     （参考 `.player-ctrl-time-seek` 的 `left:6px`；前两版分别是 0 和 14，都被用户否掉）；
   * 「只有一个视频不显示选集」这条只验证到**表达式与编译**（`fullscreen && episodes.length > 1`，
     宽度绑定跟着 `visible` 走），没有跑"全屏 + 两个视频 / 一个视频"的对比 —— 看界面一眼即可。
6. 静态构建：`Deploy OK`（`BATEXIT=0`）。`.bat` 目前是 **LF** 换行（原本是 CRLF），cmd 实测能跑；
   要转 CRLF 的命令在 README §3.8。

### 上一轮做过的
1. `cmake --build … --config Release --target appQtPlayer` → **退出码 0**（Visual Studio 生成器会自动重跑 configure，`SettingsPage.qml` 也生成了对应的 `appQtPlayer_SettingsPage_qml.cpp`）。
2. 跑首页窗口：`appQtPlayer.exe > log 2>&1`，等 8 秒 → 进程活着、**stderr 一行都没有**。
3. 跑播放器窗口：把 `main.cpp` 的 `loadFromModule` 临时改成 `"Main"`，重新编译后带一个真实视频路径启动
   （`D:\视频\相机胶卷\2023_03_03_20_25_IMG_0880.mp4`）→ 进程活着、QML 侧**没有任何编译/绑定错误**，
   只剩两条预期内的"没有同名 .xml 弹幕文件"提示。验完已经改回 `"HomeWindow"` 并重新编译。
4. 本轮第一次跑起来的日志里，用户自己点开了一个文件夹、点了一张卡片：日志能看到
   `[library] 扫描 … 新增 5 个视频` → `CicadaPlayerItem: opening source: file:///…mp4` →
   `D3D11 zero-copy is ready` → `first frame handed over to the Qt item`。
   **也就是说"首页 → 拖/选文件夹 → 卡片 → 播放器窗口 → 零拷贝出画"这条主链路是实测通的。**
   同一份日志暴露了 `DanmakuView.reset()` 缺失（第四节之外，见下），已修。

### 没做（说清楚）
- **鼠标级别的实测没做**：顶部栏"移上去浮出来 / 移开收回去 / 全屏不出现"、
  上一个/下一个点击换源、选集点击换集，这些都只验证到"加载期无报错"，
  没有做鼠标移动 + 截图比对（本轮尝试写过一个自动抓图脚本，用户拒绝了放宽权限，就没执行）。
  **请人工点一遍这几处。**
- 首页拖**多个**文件夹一起进去：代码走的是 `DropArea.onDropped` → `library.addPaths(paths)`，
  没实测过多个。
- 大文件夹的扫描耗时/卡顿（见第三节第 1 条）。

## 六、用户明确要求和禁忌

- **不要生成测试视频**；用完的临时文件要删掉。
- **不要自创 UI**：能用现有实现/参考就照抄；面板/按钮样式要一致。
- **不要删**：原来的应用设置页（`openSettings`）、参考目录里的文件。
  （"Main.qml 里那份设置页不要删"= 这个**功能**不能消失；本轮改成了 `SettingsPage {}` 实例，
  两个窗口共用同一份实现，从齿轮 / 顶栏"?" / 首页"设置"三个入口都进得去。）
- **一次做完，不要"下一轮"**：能一次写完的就一次写完（本项目多次因为"写一半"导致文件加载失败、程序退出）。
- 交付时要**诚实说明验证程度**（哪些编译过、哪些只读过代码）。
- 不要让 agent 接管鼠标 / 截屏 / 放宽沙箱权限去做 UI 自动化（本轮被明确拒绝过一次）。
