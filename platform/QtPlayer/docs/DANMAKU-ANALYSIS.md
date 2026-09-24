# 弹幕问题分析（暂停跳变 / 滚动抖动 / 固定弹幕重叠 / 字体）

> 本轮**只做分析，不改代码**。所有结论都标了 `文件:行号`，可以逐条核对。
> 文末给了"要确认某一条该看哪几个数"的实测方案。

---

## 0. 结论速览

| # | 现象 | 根因（按可能性排序） | 修法方向 | 风险 |
|---|---|---|---|---|
| 1 | 暂停→恢复"往后弹一下再往前" | ① 停帧判据 600ms vs 框架位置节拍 **500ms**（余量只有 100ms，会误判）② 恢复瞬间 `|pos - current| > 600ms` 走**硬对齐** ③ 渲染线程"外推量要还回去" | 暂停/恢复都在**切换那一刻取当前外推值当锚点**（把外推量清零）；恢复不做硬对齐只走速率；停帧阈值放到 ≥2× 框架节拍 | 低（都是时钟策略，不动引擎） |
| 2 | 滚动"有点抖动" | ① 同上那个**周期性误判停帧**（一秒一次冻一下再追）② `_rateTrim` 在每次位置回调被重置（±1% 速度微变）③ 逻辑 30Hz 每次 tick 把外推**重新锚定** | 让每条弹幕"自带出生时刻 + 时长"，位置只由 `(now - birthMono)/duration` 决定（= CSS animation 语义），GUI 只在 spawn/seek/暂停时改锚点 | 中（要动 DanmakuView 的时钟策略与渲染锚点协议） |
| 3 | 固定弹幕重叠 | **参考实现本身就这么写**，我们 1:1 照搬：固定弹幕只跟**同类型**比；滚动弹幕**主动跳过**固定弹幕；底部固定**从上往下**找 | ① 固定占用不看类型 ② 滚动判定把固定算进去 ③ 底部改从下往上（现成开关） | 低（引擎里三个判定函数） |
| 4 | 直播弹幕比播放器"粗/好看" | 两个界面**用的是同一套默认值**；你比的是 **B 站直播网页**：它是 SimHei/雅黑 **Bold** + **四向 1px 黑描边** | 换 `fontFamily` + 调 `outlineType`（两个现成属性）；字体可打包（注意授权） | 低 |

---

## 1. 弹幕的渲染架构（先对齐概念，后面全靠它）

```
媒体时钟 player.position（框架每 ~500ms 推一次）
        │
        ▼
DanmakuView.qml  逻辑层（30Hz Timer）
  · _clock 本地秒表：每次 tick 加"实测帧间隔"（DanmakuView.qml:429-432）
  · smoothPositionMs = 锚点 + (本地秒表 − 锚点时刻) × 倍速 × _rateTrim（:386-391）
  · 每 tick engine.update(smoothPositionMs)（:444）→ 引擎算每条弹幕的 x/y
        │
        ▼
DanmakuController/DanmakuEngine（C++）  轨道分配 + 位置（danmaku/src/DanmakuEngine.cpp）
        │
        ▼
DanmakuRendererItem（渲染线程）  ← 关键：位移在这里**逐帧**插值
  · 接 QQuickWindow::frameSwapped → update()（DanmakuRendererItem.cpp:156-157）
  · 换帧时刻采一次单调时间戳当采样点（:160-168）
  · 位置 = 锚点 + (现在 − 引擎打戳那一刻) × 倍速（:681-699）
  · 画的时候把左上角对齐到设备像素栅格（:550-551）
```

**这套架构本身是对的**：位移交给渲染线程逐帧算（等价于 CSS 把动画交给合成器），
逻辑只在 30Hz 上跑（省 CPU）。所以"抖"不该来自"没有逐帧插值"，而来自**喂进去的位置信号在抖**（见第 3 节）。

---

## 2. 问题 1：暂停 → 恢复"往后弹一下再往前"

### 2.1 三个机制（都有代码证据）

**(a) 停帧判据的余量只有 100ms —— 会误判**
```qml
// DanmakuView.qml:409-415
if (mediaPos !== view._lastProgressPos) { view._lastProgressPos = mediaPos; view._lastProgressMono = clockNow }
view._mediaStalled = (view._lastProgressMono > 0 && (clockNow - view._lastProgressMono) > 600)
```
框架侧位置上报的节拍是 **500ms**：
```cpp
// mediaPlayer/SuperMediaPlayer.cpp:323  mTimerInterval = 500;   （:1529 / :1618 用它做周期工作）
// :5071 NotifyPosition(getCurrentPosition()) 就在这个周期路径里
```
600ms 的阈值只比 500ms 节拍多 100ms。GUI 一忙、或定时器晚一档，位置就"600ms 没变" →
`_mediaStalled = true` → **弹幕时钟冻结**（`:429-430` 不推进）→ 下一个位置回调来了又解冻并追上去。
**表现就是一秒一次"顿一下"**（这正是问题 2 的主因，也参与问题 1）。
注意它还有第二条判据兜底（明确信号，更准）：
```qml
// :426-427  if (view.player.buffering) view._mediaStalled = true
```
**暂停**却不在这条明确信号里（`buffering` 只在缓冲时为真）→ 暂停后**最多 600ms 内弹幕还在往前走**
（`_driving = player.playing` 为假时引擎仍会被 `engine.update(smoothPositionMs)` 用**还在推进**的时间喂，见 `:173-178`）。

**(b) 恢复那一下的"对齐策略"**
```qml
// 暂停期间：位置回调只记 _lastPushedPos，不动锚点（:326-340）
// 恢复后第一次回调：
const current = view.smoothAt(view._clock)
if (Math.abs(pos - current) > 600) {           // :365-370
    view._anchorPositionMs = pos; view._anchorClock = view._clock; view._rateTrim = 1.0; return
}
view._anchorPositionMs = current               // 否则"锚点=当前外推值"，只用速率吃掉误差（:372-377）
```
* 差值 > 600ms → **硬对齐**：画面整体瞬移一次（向前或向后）；
* 差值 ≤ 600ms → 只把速率改 ±1%（`wanted = 1 ± (pos-current)/500`，且被 clamp 到 ±1%，`:376-377`）——
  也就是 **600ms 的偏差要 60 秒才能拉回来**，肉眼看就是"轻微错位 + 慢慢飘"。
暂停时 `_clock` 曾多走了最多 600ms（见 (a)），恢复后 `pos` 却停在暂停点 ⇒ 差值常在几百 ms 量级 ⇒
**要么瞬移、要么长期错位**，两者都符合你说的"弹一下再往前"。

**(c) 渲染线程要"还回外推量"**（代码里已经写明过一次）
```cpp
// DanmakuRendererItem.cpp:480-482（QML 那侧的注释）
// "暂停冻结那一帧要把这截外推量还回去 —— 100ms（GUI 再卡一下更久）在屏幕上就是几十像素的『往后弹一下』"
```
暂停/恢复时锚点如果是 33ms（30Hz）之前打的，渲染器已经按外推把画面往前推了几像素，
下一帧拿到冻结锚点就**往回弹几像素**。锚点越旧（10Hz 时代是 100ms）弹得越明显。

### 2.2 修法方向（不动引擎）

1. **暂停/恢复都在"切换那一刻"取一次当前外推值当锚点**：
   `_anchorPositionMs = smoothAt(_clock); _anchorClock = _clock; _anchorMonoMs = now` ——
   这样渲染器的"待还外推量"就是 0，屏幕上不会有那一弹。
2. **恢复不做硬对齐**：把"恢复"和"seek"分开处理 —— 恢复永远走"锚点=当前外推值 + 速率修正"，
   且把速率修正的**幅度上限放宽到 ±10%、并按剩余差值自动收敛**（例如按 1~2 秒内吃掉差值算），
   这样几百 ms 的偏差在 1~2 秒内平滑吃掉，而不是 60 秒或瞬移。
3. **停帧判据改成"明确信号 + 宽阈值"**：`buffering` 或 `!playing` 立刻冻结；
   推断那条阈值从 600ms 提到 **1200~1500ms**（≥2× 框架 500ms 节拍），避免周期性误判。

---

## 3. 问题 2：滚动抖动 —— 浏览器 CSS 为什么不抖，我们差在哪

**浏览器为什么顺**（对照）：CSS 动画把"起点 + 时长 + 缓动"一次性交给**合成器线程**，
之后每帧位置 = `(now - startTime) / duration`，**纯时间函数**：
* 主线程 JS 卡顿、掉帧都不影响它（动画在合成器上按 vsync 走）；
* 位置**不会**被"每 33ms 重新对齐到某个逻辑时钟"——它只有一个绝对时间基准；
* 变换是 `transform: translate3d(...)`（浮点、GPU），不是每帧重排。

**我们的差距**（同样的思路，但两处"重新对齐"把平滑度吃掉了）：

1. **逻辑 30Hz 每 tick 重新锚定**：`engine.update(smoothPositionMs)` → 引擎按**媒体时钟**算位置 →
   渲染器再按这个新锚点外推。**媒体时钟本身是"软件时钟"**：
   ```qml
   // :553-555  dt 由 QML Timer 的实测间隔得到（Timer 默认 CoarseTimer，可 ±5% 抖动）
   var dt = view._lastTickMono > 0 ? Math.min((mono - view._lastTickMono) / 1000, 0.25) : 0
   ```
   `dt` 测得准倒还好，但**每个 tick 都拿"引擎算出来的位置"当新锚点**，
   等于每 33ms 把动画的"绝对时间基准"重设一次；`_rateTrim` 又会在每次位置回调（500ms）
   改动 ≤±1% 的速度 ⇒ 每 33ms 一次"基准微移"、每 500ms 一次"速度微变"，累积起来就是**细碎抖动**。
2. **周期性误判停帧**（第 2.1 节 (a)）：一秒一次"冻结→追赶"，这是**最显眼**的那档抖动。
3. **设备像素对齐**（`DanmakuRendererItem.cpp:550-551` `round(x*dpr)/dpr`）：
   150px/s ÷ 60fps ≈ 2.5px/帧 → 取整成 2/3/2/3… 的轻微不均匀。
   这条**浏览器也有**（同为像素对齐），且人眼基本看不出，**不是主因**，保留即可（它换来的是文字更锐利）。

**修法方向（把 CSS 的语义搬过来）**：
* 每条弹幕在 spawn 时**记下 `birthMonoMs` 和 `durationMs`**（引擎里已经有 `startMs`/`durationMs`，
  只是现在算位置用的是"媒体时钟"，`DanmakuEngine.cpp:843-850` 的 `elapsedOf`）；
* 渲染线程位置 = `f(spawn 时的 anchor + (monoNow − anchorMono))`，**中途不接受新的位置锚点**；
  GUI 只在 **spawn / seek / 暂停-恢复 / 改倍速** 这四种事件上重设锚点；
* 于是 30Hz 逻辑只负责"该不该发射、该不该退场、悬停命中"，**完全不参与位移** ——
  位移变成纯时间函数，和 CSS 一样：主线程偶尔卡一下，弹幕也不抖。
* 逻辑 tick 建议用 `Qt.PreciseTimer`（或干脆用 `frameSwapped` 驱动），把 30Hz 的 33/66ms 抖动也去掉。

---

## 4. 问题 3：固定弹幕重叠 —— 轨道逻辑是不是有问题

**是"有"，但不是我们移植错：参考实现本身就这么规定，我们 1:1 照搬了。**
对照两边代码（行号一一对应）：

| 规则 | 参考 `front/hili-player/packages/plugins/src/utils/danmaku/trackManager.ts` | 我们的引擎 `danmaku/src/DanmakuEngine.cpp` |
|---|---|---|
| 滚动弹幕判定时**跳过非滚动项** | `:294 if (existingItem.type !== DanmakuType.SCROLL) continue` | `:866 if (other.type != DanmakuTypeScroll) continue` |
| 固定弹幕只跟**同类型**比 | `:341 / :368 if (existingItem.type === type)` | `:904 if (other.type != type) continue` |
| 顶部固定只用前 1/3 轨道 | `:335` | `:922` |
| **底部固定"从上往下"找**，只跳过最下 20% | `:356-364` | `:941-947` |
| 固定轨道复用阈值 500ms | `:344 / :373` | `fixedTrackReuseMs(500)`（DanmakuTypes.cpp:66） |

由此会产生**三类真实重叠**：

1. **底部压顶部**：底部固定"从上往下找第一条空轨" ⇒ 它会选到**顶部弹幕正在用的那条轨道**，
   而固定弹幕的占用判定只认同类型 ⇒ 两条都放行，画在同一 y 上。
2. **滚动穿过固定**：滚动弹幕的可用性判定主动跳过固定项 ⇒ 一条滚动弹幕会直接从
   顶部/底部固定弹幕身上穿过去（这一条在 B 站也很常见，所以他们后来加了"防挡/屏蔽"）。
3. **同类型"快过期"时**：`remaining > 500ms` 才算占着 ⇒ 剩余 <500ms 时新弹幕可以进来，
   如果新弹幕字号更大/更长，视觉上会紧贴甚至压上（参考也是这个阈值）。

**修法方向（按性价比）**：
1. **固定弹幕的占用判定不看类型**：同一轨道上任何类型还在显示（`remaining > 0`）就不能放固定
   —— 直接消灭第 1 类（一行改动量级，风险低）。
2. **滚动判定把固定弹幕算进去**：固定弹幕视为"占整条轨道直到 duration 结束"
   —— 消灭第 2 类（这会**降低**滚动弹幕的密度，需要你确认是否接受）。
3. **底部固定改成"从下往上找"**：`_config.bottomFixedFromBottom` 这个开关已经存在（`:930-939`），
   默认关着；打开后底部弹幕才真的在**下方**（现在它们其实在上半屏，和顶部挤在同一片区域）。
   注意这与参考不一致 —— 是**有意偏离**参考、换取"底部就是底部 + 不与顶部抢轨"。
4. 可选：固定弹幕使用**独立轨道池**（顶部前 1/3、底部后 1/3），彻底与滚动解耦。

---

## 5. 问题 4：字体（直播观感 vs 播放器）+ 能否把字体打包进软件

### 5.1 现状：两个界面**本来用的是同一套默认值**

```qml
// DanmakuView.qml
property bool fontBold: true            // :60
property int outlineType: 0             // :62
property string fontFamily: "Microsoft YaHei"   // :65
property int baseFontSize: 25           // :80
```
`PlayerView.qml:447` 和 `LivePlayerView.qml:254` 实例化 `DanmakuView` 时**都没有覆盖**这几项
（我逐行查过）⇒ 点播和直播的弹幕字体、字重、描边**完全一样**。

**所以"直播弹幕更粗更好看"是拿 B 站直播网页在比**。官方直播弹幕的样式（前面从官方 bundle/样式表核到的）：
```
--fontFamily: SimHei, "Microsoft JhengHei", Arial, Helvetica, sans-serif
--fontWeight: bold
--fontSize: 25px
--textShadow: 1px 0 1px #000, 0 1px 1px #000, 0 -1px 1px #000, -1px 0 1px #000   ← 四向 1px 黑描边
```
我们 `outlineType = 0` 的观感自然更"细"。**要一致：换字体族 + 调描边档**，两个都是现成属性，
引擎支持（`DanmakuTypes.cpp:121 outlineType`，渲染走 `DanmakuAtlas` 的 `QFont(family)+setBold+QFontMetricsF`，
`DanmakuAtlas.cpp:138-152`），**不用改引擎**。

### 5.2 能不能把字体打包进软件？—— 能，但要选对字体（授权）

**渲染链路支持**：弹幕文字是用 `QFont(fontFamily)` 走 Qt 字体库量宽 + 画进图集
（`DanmakuAtlas.cpp:138-152`、`DanmakuController.cpp:205-217`）⇒
**只要有办法让这个 family 在 Qt 里可用**（系统装的、或程序自己注册的）就行 ✓。

**打包步骤（三步，改的是工程配置不是引擎）**：
1. 字体文件放 `platform/QtPlayer/assets/fonts/`（现在工程里**还没有**任何打包字体，我查过了）；
2. `CMakeLists.txt` 里给 `assets/fonts/*` 加一条 `file(GLOB ... CONFIGURE_DEPENDS)` 并并进
   `qt_add_qml_module(... RESOURCES ...)`（现有两条 glob 只覆盖 `assets/lottie-icon/*` 和 `assets/images/*`，
   新目录不写进去就**不会**进 qrc）；
3. 程序启动时注册：`QFontDatabase::addApplicationFont(":/qt/qml/QtPlayer/assets/fonts/xxx.ttf")`
   （`main.cpp` 里一行），或者 QML 侧用 `FontLoader { source: "assets/fonts/xxx.ttf" }` 再把它
   `name` 赋给 `DanmakuView.fontFamily`；之后弹幕就用这个族渲染。

**⚠️ 授权（这条最重要）**：`微软雅黑（Microsoft YaHei）`、`中易黑体（SimHei）` 都是**商业字体** ——
Windows 自带 ≠ 可以随你的软件分发。打包请用**免费商用/OFL**字体：

| 字体 | 授权 | 适合弹幕吗 |
|---|---|---|
| **思源黑体 Source Han Sans SC**（= Noto Sans SC） | OFL | ✓ 有 Bold/Heavy，最接近官方观感 |
| **阿里巴巴普惠体** | 免费商用 | ✓ |
| **HarmonyOS Sans / MiSans** | 免费商用 | ✓ 现代感强、字重全 |
| **霞鹜文楷 LXGW WenKai** | OFL | ✓ 但偏楷体，弹幕不常见 |

体积参考：全量 CJK 单字重 **≈10~20 MB**（打包 1 个字重就够；想再小就按常用字子集化）。
`SimHei` 只在你自己机器上"看起来一样"，换台机器（或没有中易黑体的系统）就会掉回别的字体 ——
**这正是打包的意义**。

---

## 6. 要确认"到底是哪一条"，需要一次带日志的实测（不改逻辑，只加 4 行日志）

建议在**暂停→恢复**和**滚动抖动**时各抓一次下面这些数（现成的都能拿到）：

| 想确认的 | 抓什么 | 期望 |
|---|---|---|
| 停帧误判（问题 1a / 2 主因） | 每次 `_mediaStalled` 翻转时打印 `mono, player.position, _lastProgressMono` | 若正常播放时也频繁翻转 ⇒ 阈值 600ms 太小（改 1200~1500ms） |
| 恢复硬对齐（问题 1b） | 恢复后第一次位置回调打印 `pos, current, delta` | 若 `delta` 常在几百 ms ⇒ 恢复不该走硬对齐 |
| 外推量（问题 1c） | 暂停那一刻打印 `smoothPositionMs` 与渲染锚点 `_anchorMonoMs` 的差 | 差越大、"弹"得越多；修法是暂停时把锚点取成当前外推值（差=0） |
| 抖动来源（问题 2） | 连续 100 个 tick 打印 `dt` 与 `smoothPositionMs` 增量 | `dt` 抖动大（31/33/47ms）⇒ 换 PreciseTimer/ vsync 驱动；`smoothPositionMs` 增量不均匀 ⇒ 每个 tick 重锚是主因 |

---

## 7. 修复记录（第 28 轮：三个问题全修，**字体除外**）

> 用户原话："修复固定弹幕重叠问题和弹幕抖动和暂停播放弹一下的问题，除了字体的全部修复"。
> 第 5 节的字体方案**本轮不动**，只留档。构建：`build/build-round37.log` → `[100%] Built target appQtPlayer`，无 error。

### 7.1 固定弹幕重叠 —— 根因是"轨道判定照抄参考实现"，而参考实现是给**网页 DOM** 写的

| 位置 | 原逻辑 | 现在 | 为什么 |
|---|---|---|---|
| `danmaku/src/DanmakuEngine.cpp:878` `trackAvailable()` | `if (other.type != SCROLL) continue`（= 参考 `trackManager.ts:294`）——滚动弹幕**看不见**固定弹幕 | 固定弹幕 `remaining > 0` ⇒ 这条轨道对滚动弹幕直接不可用 | 滚动弹幕会从顶部/底部弹幕身上穿过去，实测就是"重叠"的一大类 |
| `DanmakuEngine.cpp:940` `fixedTrackAvailable()` | 只跟**同类型**比（参考 `trackManager.ts:341/368`）⇒ 底部弹幕能落在顶部弹幕正占着的轨道上，两条弹幕同一个 y | `grace = (other.type == type) ? fixedTrackReuseMs(500ms) : 0.0`：**跨类型必须等它完全结束**；同类型仍保留 500ms 提前复用 | 这是"两条字叠在一起"的主因；保留同类型 grace 是为了密集时不白白空一条轨道 |
| `DanmakuTypes.cpp` `bottomFixedFromBottom()` | 参考实现自上而下找空轨道 | 维持上一轮改动：底部弹幕**自下而上**选轨道 | 让底部弹幕沉在画面下方，不和顶部弹幕抢中间几条，两类固定弹幕自然分开 |

### 7.2 滚动抖动 —— 两个来源，都堵掉

1. **`_mediaStalled` 误判（主因）**：阈值本来是 **600ms**，而框架推位置的节拍是 **500ms**
   （`SuperMediaPlayer.cpp:323 mTimerInterval = 500`），余量只有 100ms。GUI 线程随便卡一下
   就够翻成"卡住了" ⇒ 时钟冻一下再恢复 ⇒ 屏幕上就是规律性的"一顿一顿"。
   → `DanmakuView.qml:425` 阈值提到 **1500ms（3× 框架节拍）**，并且新增两条**明确信号**：
   `DanmakuView.qml:438`（框架报 buffering）、`:449`（`!player.playing` 显式冻结），
   真暂停/真缓冲不再依赖推断。
2. **tick 间隔不齐 —— 这条后来被证伪，已回退，但记在案**：我原来在 `Timer` 上写了
   `timerType: Qt.PreciseTimer`，构建能过（qmlcachegen **不做类型检查**），运行时 QML 加载直接失败：
   `DanmakuView.qml:546: Cannot assign to non-existent property "timerType"` ⇒
   `PlayerView unavailable` ⇒ **播放器窗口根本打不开**（2026-09-23 实测，RC-AP）。
   事实：QML 的 `Timer` 不是 `QTimer`，而是 `QQmlTimer` = `QPauseAnimationJob` 挂在**动画时钟**上
   （`qqmltimer.cpp` 里只有 interval/running/repeat/triggeredOnStart，没有任何 timerType），
   它的分辨率上限是动画时钟的 16ms。
   **而且它本来就不构成抖动源**：位置是每个 tick 用实测 `dt` 积分出来的
   （`stepDanmaku(dt, mono)`），tick 早来晚来几毫秒只是改变积分步长，
   `smoothPositionMs` 依然单调按真实时间走。抖动真正来自误判停帧（上面第 1 条）。
   → 已删除该行，**此处不再配置任何东西**。
3. 翻转那一帧**重新取锚点**（`DanmakuView.qml:466`，`view._mediaStalled !== wasStalled`）并把
   `_rateTrim` 归 1，避免"冻结前积累的外推量"和"解冻后的新基准"打架。

### 7.3 暂停 → 恢复"弹一下" —— 三条路一起走的最后一环

渲染线程的时钟（`src/DanmakuRendererItem.cpp:655-734`）本身是对的：引擎帧号没变时按
**增量**累积（避免把整段暂停时长算进去），`clockRunning=false` 时 `extraMs=0` 冻在引擎位置上。
剩下的"弹"来自 QML 侧：

| 环节 | 原来 | 现在 |
|---|---|---|
| 暂停后仍被判"没停" | 只能靠 600ms 推断，暂停后最多几百毫秒才冻 | `DanmakuView.qml:449` 用 `!view.player.playing` **立刻**冻（不用等阈值） |
| 恢复后第一次位置回调 | `|pos - current| > 600ms` 就**硬对齐**，而恢复瞬间 `pos/current` 本来就会差几百 ms ⇒ 画面直接跳 | `DanmakuView.qml` 非 seek 的硬对齐阈值提到 **1000ms**，暂停/恢复不再触发硬对齐（真 seek 仍走 `_pendingSeek` 那条精确路径） |
| 解冻第一帧的外推量 | 锚点还是暂停前那一帧的（可能差上百 ms ⇒ 几十像素） | `:466` 在冻结/解冻翻转时把锚点刷成当前 `smoothPositionMs`，`extraMs` 从 0 重新起算，向前向后都不弹 |
| 进入冻结那一 tick 的 `dt` | 冻结时 `_clock` 停在**上一帧**的值，渲染线程已经按外推多画了约一个 tick（≤33ms）⇒ 拿到冻结锚点必须**往回还**，这就是"往后弹一下" | `:451` 进入冻结的**那一次** tick 仍把 `dt` 加上（`if (!_mediaStalled \|\| !wasStalled)`）：冻结位置 = 渲染线程**已经画出来**的位置，要还的外推量 = 0；此后才真正冻结。多走一帧（≤33ms）视觉不可见，换"零回弹" |
| 引擎侧 | — | `DanmakuEngine` 的 `_paused` 分支只同步悬停、不推时钟（已有），配合上面三条闭环 |

### 7.4 本轮教训（写进流程，避免再犯）：**构建通过 ≠ QML 能用**

RC-AP 那次的失败模式很有代表性，记下来：

| 现象 | 原因 |
|---|---|
| `cmake --build` 一路 `[100%] Built target appQtPlayer`，无任何 error | `qmlcachegen` 只做**字节码编译**，**不做类型/属性检查**；属性名写错它照样“编译”成功 |
| 运行起来“播放器打不开” | QML 在**加载期**才解析属性：`Cannot assign to non-existent property "timerType"` ⇒ `DanmakuView unavailable` ⇒ `PlayerView unavailable` ⇒ `HomeWindow.qml:1254 播放器窗口创建失败` |
| 首页还是能显示、日志文件照写 | 失败被 `createObject` 兜住并打了一条 warn，**只有日志里那一行**能看出来 |

所以以后**改 QML 后必须补一步静态检查**（Qt 自带，秒级）：

```powershell
# 全量（29 个 QtPlayer QML 文件）；build 目录提供 QtPlayer 模块的 qmldir / qmltypes
& "D:\Qt\6.11.1\msvc2022_64\bin\qmllint.exe" `
  -I "D:\hilihili\CicadaPlayerNext\platform\QtPlayer" `
  -I "...\build\Desktop_Qt_6_11_1_MSVC2022_64bit-Debug" `
  -I "...\build\Desktop_Qt_6_11_1_MSVC2022_64bit-Debug\QtPlayer" `
  <所有 .qml>
```

判读方法（本轮实测结论）：

* **看 `Error:` 行 —— 必须 0 条**；`timerType` 那种会报成
  `Member "timerType" not found on type "Timer" [missing-property]`，**这一步就能拦住 RC-AP**。
* `Member "xxx" not found on type "QtPlayerTheme"` / `"QQuickItem"` / `"QObject"` 是**误报**：
  前者是单例属性在源码树里解析不到（运行时正常），后两者是弹幕池这种动态 `createComponent`
  出来的对象，qmllint 看不到自定义属性。本轮全量 663 warning / **0 error**。
* `QKeySequence.MediaPlay`、`LottieAnimation.playing` 同属误报（枚举/第三方类型解析不到）。
