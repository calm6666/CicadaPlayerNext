# 字幕按钮 + 字幕面板（Qt 版实现说明）

对应参考：bilibili 播放器控制栏里那颗「字幕」按钮，以及它展开的两页面板。

## 一、素材（两份原始 dump）

| 文件 | 内容 | 怎么用 |
|---|---|---|
| `CicadaPlayerNext/platform/subtitles.txt` | 整棵 DOM（`.bpx-player-ctrl-btn.bpx-player-ctrl-subtitle` 那 369 行） | 结构、文案、初始选中项、内联尺寸（168×311 / 434 / 266×260 / 135px）全按它 |
| `CicadaPlayerNext/platform/style.css` | 该页面的完整 CSS | 所有数值（尺寸/间距/颜色/交互态）全按它 |

`style.css` 是**压缩成一整行**的，`read` 工具每行只给 2000 字符、`grep` 也只回整行，
所以要用脚本按"带 `@media` 上下文"的方式把规则抽出来看。可复用的抽取脚本：

```powershell
$css = [System.IO.File]::ReadAllText('CicadaPlayerNext\platform\style.css')
$out = New-Object System.Text.StringBuilder
$filter = 'bpx-player-ctrl-subtitle|bui-(panel|select|slider|track|bar|thumb|checkbox|switch|button|dark)'
$stack = New-Object System.Collections.Generic.List[string]
$i = 0; $n = $css.Length; $selStart = 0
while ($i -lt $n) {
  $c = $css[$i]
  if ($c -eq '{') {
    $sel = $css.Substring($selStart, $i - $selStart).Trim()
    if ($sel.Length -gt 0 -and $sel[0] -eq '@') { $stack.Add($sel); $i++; $selStart = $i; continue }
    $end = $css.IndexOf('}', $i); if ($end -lt 0) { break }
    $body = $css.Substring($i + 1, $end - $i - 1)
    if ($sel -match $filter) {
      if ($stack.Count -gt 0) { [void]$out.AppendLine(($stack -join ' ') + '  >>>  ' + $sel + ' { ' + $body + ' }') }
      else { [void]$out.AppendLine($sel + ' { ' + $body + ' }') }
    }
    $i = $end + 1; $selStart = $i; continue
  }
  if ($c -eq '}') { if ($stack.Count -gt 0) { $stack.RemoveAt($stack.Count - 1) }; $selStart = $i + 1 }
  $i++
}
$out.ToString() | Set-Content out.txt -Encoding UTF8
```

**为什么必须带 `@media` 上下文**：同一批选择器在 `@media screen and (min-width:750px)` +
`[data-screen=full|web]` 里有一档完全不同的值。第一遍没带上下文时，
`font-size:16px` / `bottom:74px` 看起来像是**通用**规则；带上上下文才看清它们只在
**全屏/网页全屏**生效 —— 普通窗口是 14px / 41px，和 `QtPlayerTheme.menuBottom(41)` 一致。

关键的两档（`style.css` 里 `.bpx-player-ctrl-subtitle` 相关的全部规则共 111 条）：

| 值 | 普通 | 全屏/网页全屏（min-width:750px） |
|---|---|---|
| 按钮上"字幕"字号 | 14px | 16px |
| 面板底边距按钮底边 | 41px | 74px |
| 按钮盒 | 36×22（`width:auto` 覆盖成文字宽） | 54×43、行盒 32 |

## 二、实现文件

| 文件 | 内容 |
|---|---|
| `SubtitlePanel.qml`（新） | 面板本体：左页（168×311）+ 右页（266×260），`bui-panel-move` 那条 434 宽的轨道 |
| `PlayerControlBar.qml` | 新增 `subtitleButton`（插在**音量**和**倍速**之间）+ `subtitleMenu` 浮层 + `subtitleHoverZone` 判据区 |
| `assets/images/subtitle-badge.svg`（新） | 语言项后面那个"AI 字幕"小胶囊（DOM 里内联的那段 svg，**原样**） |
| `assets/images/select-arrow.svg`（新） | 下拉框右侧的小三角（CSS 里是 `border-width:4px 3px 0` 的三边框，等形状 svg） |
| `assets/images/dm/{checkbox,checkbox-checked,arrow}.svg` | 复用（和 DOM 里的内联 svg 逐字一致） |

面板里复用的既有控件风格：`bui-panel`（底 `hsla(0,0%,8%,.9)`、圆角 2）、
`bui-select`（深色：外框 1px `hsla(0,0%,100%,.2)`、高 22、列表底 `#212121`、项高 20）、
`bui-slider`（轨 2px `#e7e7e7`、已播放 `#00a1d6`、圆点 12px）、
`bui-checkbox`（16px 图标 + 12px 文字）、`bui-switch`（30×20 轨 + 16px 圆点）、
`bui-button.bui-button-transparent`（1px 边框的"恢复默认设置"）。

## 三、和参考的**有意偏离**（其余逐条对照）

1. **面板横向"居中于按钮"**（用户要求，2026-09-16 改）：
   参考是 `.bpx-player-ctrl-subtitle-box{right:-115px}` —— 右边缘比按钮右边缘外扩 115px，
   结果**面板底边的中心根本不在"字幕"两个字下面（偏右约 98px）**。
   现在和音量/倍速/清晰度一样用 `x: (parent.width - width) / 2`。
   实测 1280 窗口（窗口化）下：左页 168 宽完全居中；右页 266 宽时右边缘 1274 < 1280，
   也不会被窗口切掉。（全屏更宽松：右组有 370 最小宽，夹取不会生效。）
2. **右页让内容正好落地 260px，不滚动**（用户要求，2026-09-16 改）：
   按参考逐条累加是 266px（12+43+12+43+12+34+12+40+10+16+10+22），
   比这一页的 260 高 6px —— 参考自己就是让最后那截被面板裁掉的，
   现象是"恢复默认设置"下半截看不见。这里把"其它设置"的下边距和 reset 的上边距
   各收 3px（10 → 7）→ 正好 260：**整页不滚动，滚动条也就不出现**。
   面板里唯一还可能出现的滚动条是左页语言列表那条（6 项 × 40 = 240 > 135 可视高度），
   它贴着面板右边缘（参考里那条也是 `right:0`，因为承载它的容器伸出了 20px 内边距）。
   这条要是也不想要：删掉 `SubtitlePanel.qml` 里 `languageBox` 底部那个
   `Rectangle { visible: languageFlick.contentHeight > languageFlick.height; ... }` 即可，
   滚轮照常能滚。
3. **列表行的高亮块上下各收 2px**（用户反馈"左右有边距、上下没有，看上去很别扭"）：
   参考里"关闭"行和语言项都是 40px 高、`:hover` 高亮铺满整行，相邻两行的高亮上下贴死。
   现在高亮块上下各收 2px（36px 高）→ 相邻高亮之间有 4px 缝；
   **行高和点击区域仍是 40px**（不产生点不到的死区），下拉列表项（20px）同样上下各收 1px。

## 四、参考 DOM 里**没实现**的部分（都是 `display:none` 的隐藏块）

这些在 dump 里都是隐藏的（或者属于"AI 原声翻译"那套功能，本播放器没有）：

* `.bpx-player-ctrl-subtitle-menu-translation`（翻译字幕页）、
  `.bpx-player-ctrl-subtitle-menu-description`（AI 翻译说明页）、
  `.bpx-player-ctrl-subtitle-add`（添加字幕）、`-fake-switch`（登录才能用的假开关）、
  `-language-unlogin`（登录可享）、`-minor`（副字幕列）、`-nolan`（暂无字幕）、
  `-separator-above`、`-bilingual-above`、`-result-wrap .bpx-common-svg-icon`（按钮上的胶囊图标）；
* **右页没有"返回"元素** —— 参考 DOM 那一页里就只有 4 个设置项 + 恢复默认设置，
  没有任何返回上一页的控件，所以这里也没加。离开面板再悬停回来就是左页
  （`onClosed` 里调 `showPage(false)`）。

## 五、状态是**纯 UI 状态**

播放器本身还没有字幕渲染（没有字幕数据源），所以面板里的选择只改面板自己的状态，
不驱动任何播放行为（和弹幕设置面板里那两个防挡复选框一样）。
初始值照 dump：语言=关闭（"关闭"那一行是主色）、字幕大小=适中、颜色=白色、
描边=无描边、位置=底部居中、不透明度 87%、等比缩放=勾选、淡入淡出=未勾选、双语字幕=关。
