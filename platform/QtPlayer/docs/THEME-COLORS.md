# 播放器主题色 —— 只改两个变量，全项目生效

> **一句话规则**：全项目的强调色（不管是 hover 变色、进度条已播、高能进度条、清晰度选中、
> 弹幕/设置面板的选中与 hover、直播控制条高亮、卡片边框、应用 accent）**只有一处定义**：
> `QtPlayerTheme.qml` 顶部的 `accentDark` / `accentLight`。**换主题色只改这两行。**

---

## 1. 定义在哪

`platform/QtPlayer/QtPlayerTheme.qml`（QML 单例，所有界面都 `import QtPlayer` 就能用）：

| 变量 | 值 | 用途 |
|---|---|---|
| `accentDark` | **`#D44E7D`** | 深色主题下的主题色（用户指定） |
| `accentLight` | **`#FF6699`** | 浅色主题下的主题色（用户指定） |
| `playerAccent` | `dark ? accentDark : accentLight` | **全项目的强调色**（最常用这一个） |
| `playerAccentHover` | 深 `#E0698F` / 浅 `#FF8CB0` | 比常规亮一档的 hover（取代原来的 `#00aeec`） |
| `playerAccentActive` | 深 `#B03E63` / 浅 `#E84B85` | 按下/激活档（深色主题的"按下底"） |
| `playerAccentFaint` | 主色 18% 透明 | 浅色主题的"按下/选中底"（取代 `#cfe0f5`） |
| `playerAccentSoft` | 主色 20% 透明 | 高能进度条"已播"那一层的填充 |

两个值取自 B 站 v11 主题令牌 `--Pi5`（浅色 `#FF6699` / 深色 `#D44E7D`），
出处：`s1.hdslb.com/bfs/static/jinkela/long/laputa-css/{light,dark}.css`。

> 为什么 hover/active 也单独定义、而不是自动算：QML 里"把颜色调亮 10%"没有稳定写法
> （`Qt.lighter()` 在不同基色上效果不一致），而 B 站自己的令牌表也是**显式给三档**
> （`--Pi5` / `--Pi5_hover` / `--Pi5_active`）。所以这里照那个结构来，值也写死、好对照。

---

## 2. 已经统一到主题色的地方（这一轮改的）

`find-blue-colors.js`（按**色相**判定，不靠人肉记忆）扫全项目后，**43 处**蓝色字面量被替换：

| 文件 | 处数 | 换成 |
|---|---|---|
| `controls/DanmakuBar.qml` | 14 | `playerAccent`（选中/hover 文字、图标着色）、`playerAccentHover`（原来的 `#00aeec`） |
| `SettingsPanel.qml` | 9 | `playerAccent` / `playerAccentHover` |
| `PlayerColorPanel.qml` | 5 | `playerAccent` |
| `SubtitlePanel.qml` | 4 | `playerAccent`（面板自身的 fnColor） |
| `controls/LiveControlBar.qml` | 4 | `playerAccent`（直播控制条的功能蓝 `#23ade5`） |
| `controls/LiveMenu.qml` | 4 | `playerAccent`（清晰度"当前档"文字） |
| `DanmakuSettingPanel.qml` | 2 | `playerAccentHover`（fnColor） |
| `HomeWindow.qml` | 1 | `playerAccent`（卡片 hover 边框） |
| `QtPlayerTheme.qml` 自身 | 4 个键 | `progressPlayedBg`（进度条已播）、`qualityActiveText`（清晰度选中）、`volumeThumbBg`（音量滑块）、`pbpCurvePlayedBg`（高能进度条已播层）、`accent`、`buttonPressedBg`、`choiceActiveBg` |

所以：**进度条、高能进度条、hover 变色、弹幕面板、设置面板、直播控制条**全部跟着主题色走。

## 3. **不许**改的地方（它们是"数据"，不是主题色）

`find-blue-colors.js` 以后还会列出这几处 —— **属于预期，别动**：

| 位置 | 为什么保留 |
|---|---|
| `controls/DanmakuBar.qml` 的 `colorList`（`#019899`、`#4266BE`、`#89D5FF` …） | 那是**弹幕可选颜色**的调色板（用户能挑来发出去的颜色），换了等于改功能 |
| `SubtitlePanel.qml` 的 `colorList`（`#673AB7`、`#3F51B5`、`#2196F3`、`#03A9F4`） | 同上，是**字幕颜色**选项 |
| `HomeWindow.qml` 的 `#20242b` / `#2a2f38` | 卡片深色底（近灰，不是强调色） |

两处调色板都已经在代码里写了注释说明"这是数据，不要换成主题色"。

---

## 4. 怎么改主题色（三步）

1. 打开 `QtPlayerTheme.qml`，只改这两行：
   ```qml
   readonly property color accentDark: "#D44E7D"
   readonly property color accentLight: "#FF6699"
   ```
2. （可选）如果你还想让 hover/按下更贴近新色，改 `playerAccentHover` / `playerAccentActive` 两行。
3. 重新编译即可。**不需要**去任何别的文件找蓝色。

改完可以自查一遍：

```powershell
# 1) 全项目还有没有偏蓝的强调色（应只剩第 3 节那两处"数据"）
node platform/QtPlayer/build/find-blue-colors.js platform/QtPlayer

# 2) QML 有没有"编译能过、运行才炸"的写法
node platform/QtPlayer/build/check-qml-shapes.js platform/QtPlayer
```

---

## 5. 相关文件

| 文件 | 作用 |
|---|---|
| `QtPlayerTheme.qml` | 主题色**唯一定义处**（含 hover/active/faint/soft 派生） |
| `build/find-blue-colors.js` | 按色相扫出所有偏蓝颜色（含 `Qt.rgba(...)` 形式），改主题时用它兜底 |
| `build/check-qml-shapes.js` | 抓"`color:` 被赋 CSS 函数串 / 十六进制位数不对 / `Qt.rgba` 参数不足"这类运行期才报的错 |
| `build/find-dup-handlers.js` | 抓"同一对象上重复声明同名信号处理器"（QML 会静默顶掉一个） |
