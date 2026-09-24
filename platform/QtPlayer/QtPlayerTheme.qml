// ===========================================================================
// 主题：深浅两套 + 跟随系统
//
// 对应 HuskarUI 里的 `HusTheme`：那边是一整套主题引擎（几百个 token、主题文件、
// 跟随系统的开关），我们这一屏界面用不上那么多，所以这里就是**一张扁平的调色板**，
// 键名按"用在哪"来取，每个键都是 `dark ? 深色 : 浅色` 的三元绑定 —— `dark` 一变，
// 整屏所有颜色自动重算，不需要任何"重新应用主题"的代码。
//
// 为什么单独一个文件、还要 `pragma Singleton`：
//   * 颜色要用在 **inline component**（CaptionButton / SettingSwitch / SettingChoices）
//     和 Repeater 的 delegate 里。这些地方在 QML 里是**独立作用域**，摸不到外层文档的
//     id（qmllint 会报 unqualified access，除非开 pragma ComponentBehavior: Bound），
//     而**单例是一个类型名**，任何作用域里都能直接用；
//   * 和 HuskarUI 的 `HusTheme.Primary.colorBgBase` 那种写法对得上，读起来一致。
//
// 注意：本文件必须在 CMake 里登记成单例，见 CMakeLists.txt 里的
// `set_source_files_properties(QtPlayerTheme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)`，
// 少了那一行 QML 会把 QtPlayerTheme 当成普通类型，用的时候会报 "is not a type"。
// ===========================================================================
pragma Singleton
import QtQuick

QtObject {
    id: theme

    // -----------------------------------------------------------------------
    // 深浅状态
    // -----------------------------------------------------------------------

    /*
     * 主题模式："system" 跟随系统 / "light" 强制浅色 / "dark" 强制深色。
     * 设置页里那行「界面主题」和标题栏那颗太阳/月亮按钮改的都是它。
     */
    property string mode: "system"

    /*
     * 系统现在是深色还是浅色。
     *
     * 用 Qt 自己的 `Application.styleHints.colorScheme`（C++ 那边是
     * `QStyleHints::colorScheme`，Qt 6.5 起有；QML 的 `Application` 单例早就存在）——
     * 它在三端都是真的去问系统：
     *   * Windows：读注册表里的"应用模式"（深色 / 浅色 / 自定义）；
     *   * macOS：NSAppearance（系统"自动"切换时它也会跟着变）；
     *   * Linux：桌面 portal / GTK 主题。
     * 系统没表态（或者 Qt 太老没有这个属性）时值是 Qt.Unknown，这里按浅色处理。
     *
     * 千万别反过来去**写** colorScheme：它是"应用覆盖系统"的开关，写过之后这里读到的
     * 就是自己写进去的值，"跟随系统"当场失效（而且写接口是 Qt 6.8 才有的，见 README §3.7）。
     */
    readonly property bool systemDark: Application.styleHints.colorScheme === Qt.Dark

    /*
     * 最终用深色还是浅色 —— 界面上所有颜色都只看这一个值。
     * HuskarUI 里对应 HusTheme.darkMode（那边是直接给用户改的）；这边多了一层
     * "跟随系统"，所以把 mode（用户选的）和 dark（算出来的）分开。
     */
    readonly property bool dark: mode === "dark" || (mode === "system" && systemDark)

    /* 给界面用的模式显示名（设置页里那三颗按钮的顺序也是它）。 */
    readonly property var modeKeys: ["system", "light", "dark"]

    /* -----------------------------------------------------------------------
     * 调色板。每一行都是「深色 : 浅色」两套，改主题时只改这里。
     * ----------------------------------------------------------------------- */

    /* =======================================================================
     * ★★★ 播放器主题色（**全项目唯一来源**）★★★
     *
     * 用户要求："把原来的蓝色（hover 变色、进度条、高能进度条…）全部改成主题色；
     *          深色 #D44E7D、浅色 #FF6699；在主题里定义变量、写进文档，
     *          以后换主题色只改这两个变量，不用一个一个改。"
     *
     * 【怎么改主题色】只动下面 `accentDark` / `accentLight` 两行
     * （外加 hover/active 两个派生值，见注释）。**其它文件里一律不许再出现蓝色字面量** ——
     * 需要强调色就用这里的 `playerAccent` 系列。
     *
     * 【这两个值的出处】B 站 v11 主题令牌 `--Pi5`：
     *   浅色 #FF6699、深色 #D44E7D（`s1.hdslb.com/bfs/static/jinkela/long/laputa-css/{light,dark}.css`）。
     *   和直播播放器 bundle（room-player.min.js）里 `.quality-it.selected{color:#23ade5}` 那种
     *   "播放器自己的功能蓝"是两回事：用户明确要求**统一成品牌粉**，所以：
     *     进度条已播色、高能进度条已播层、清晰度选中文字、弹幕面板 hover/选中、
     *     设置面板选中、音量滑块、直播控制条的高亮、卡片 hover 边框、应用 accent
     *   ——全部指向 playerAccent。
     *
     * 【三条使用规则】
     *   1. `playerAccent`      —— 常规强调色（进度条已播、选中文字、图标 hover、边框）
     *   2. `playerAccentHover` —— 比常规更亮的 hover（原 `#00aeec` 那一档）
     *   3. `playerAccentFaint` —— 半透明底（浅色主题下的"按下/选中底"）；深色用 playerAccentActive
     *   4. `playerAccentSoft`  —— 主色 20% 透明（高能进度条"已播"那一层的填充）
     * 细节与"哪些蓝不许改"见 docs/THEME-COLORS.md。
     * ======================================================================= */
    readonly property color accentDark: "#D44E7D"
    readonly property color accentLight: "#FF6699"
    /* 主题色本体：全项目的强调色都用它 */
    readonly property color playerAccent: dark ? accentDark : accentLight
    /* hover 档：亮一档（取代原来的 #00aeec） */
    readonly property color playerAccentHover: dark ? "#E0698F" : "#FF8CB0"
    /* 按下/激活档：暗一档 */
    readonly property color playerAccentActive: dark ? "#B03E63" : "#E84B85"
    /* 18% 透明底：取代 App 设置页那种"浅蓝底"（#cfe0f5） */
    readonly property color playerAccentFaint: Qt.rgba(playerAccent.r, playerAccent.g, playerAccent.b, 0.18)
    /* 主色 20%：高能进度条"已播"那一层（原来是 #00a1d6 的 20%） */
    readonly property color playerAccentSoft: Qt.rgba(playerAccent.r, playerAccent.g, playerAccent.b, 0.2)

    // ---- 窗口、标题栏、播放区 ----
    /*
     * 窗口底色。**没选片子时**整个窗口都是它 —— 深浅色主题就体现在这块中间 UI 上
     * （提示文字、控制条、设置页那些）。选了片子之后窗口底色换成 videoBg。
     */
    readonly property color windowBg: dark ? "#101014" : "#f2f3f6"
    /*
     * 播放区底色：**永远是黑的**，不跟主题。
     * 这样"选了片子"之后播放区整块是黑的，画面比窗口小时露出来的也是黑边
     * （浅色主题下不会再出现"视频后面一片白"）。见 Main.qml 里 root.color 的绑定。
     */
    readonly property color videoBg: "#000000"
    /*
     * 标题栏**和窗口同色**（windowBg），不再单独配一个颜色。效果正是要的那两条：
     *   * 没选片子：顶栏和中间连成一片，不会"上面一条、下面一块"；
     *   * 选了片子：窗口底色变黑、顶栏还是 windowBg —— 于是"只有顶部有深浅色"。
     * 也正因为顶栏和播放区颜色本来就不一样（黑 vs 主题色），两者之间不需要分隔线。
     */
    /* 标题文字 */
    readonly property color titleText: dark ? "#c8c8d4" : "#33363d"
    /* 标题栏按钮的图标颜色 */
    readonly property color captionGlyph: dark ? "#d8d8e0" : "#4a4e57"
    /* 标题栏按钮悬停 / 按下的底色：半透明，压在标题栏底色上深浅两套都好看 */
    readonly property color captionHover: dark ? "#33ffffff" : "#33000000"
    readonly property color captionPressed: dark ? "#59ffffff" : "#59000000"
    /*
     * 播放器窗口那条"浮出来的标题栏"的底色与分隔线（用户要求：要带背景）。
     *
     * 为什么不直接复用 windowBg：那条标题栏是**压在视频画面上**的浮层，用一条
     * 略透明、比窗口底色稍深/稍浅的实色才既能看清"这里能拖窗口"，又不至于像一块
     * 突兀的色块。0xE6 = 90% 不透明度（和参考实现那些浮层面板的 rgba(...,0.9) 同量级）。
     */
    readonly property color captionBarBg: dark ? "#E61B1E24" : "#E6F2F3F5"
    readonly property color captionBarBorder: dark ? "#1Affffff" : "#1A000000"

    // ---- 播放器控制层（数值全部来自参考实现的 CSS，见注释里的出处）----
    /*
     * 控制层是**压在画面上的**，参考实现里它不跟应用主题走：文字/图标是白色系，
     * 下方垫一层"透明 -> 黑"的自绘遮罩。所以这一段不是 dark?: 两套，
     * 而是照抄参考里那些固定值（出处：packages/player/src/styles/*.scss）。
     */

    /* 底部控制条本体：.player-control-bottom（controls.scss:326-341） */
    readonly property int controlBottomHeight: 35
    readonly property int controlPaddingH: 12          /* padding: 0 12px */
    readonly property int controlBottomMarginTop: 20   /* margin: 20px 0 0 */
    readonly property int controlLineHeight: 22        /* line-height: 22px */

    /* 进度条那一行：.player-control-top（controls.scss:36-48） */
    readonly property int progressRowBottom: 44        /* bottom: 44px */
    readonly property int progressRowPaddingH: 12      /* padding: 0 12px */

    /* 进度条：.player-progress / -wrap / -schedule-wrap（controls.scss:50-152） */
    readonly property int progressHeight: 4            /* height: 4px */
    readonly property int progressWrapPaddingBottom: 6 /* padding-bottom: 6px */
    readonly property real progressHoverScale: 1.5     /* hover 时 scaleY(1.5) */
    readonly property real progressRadius: 1.5         /* border-radius: 1.5px */
    readonly property color progressTrackBg: Qt.rgba(1, 1, 1, 0.2)   /* hsla(0,0%,100%,.2) */
    readonly property color progressBufferBg: Qt.rgba(1, 1, 1, 0.3)  /* hsla(0,0%,100%,.3) */
    readonly property color progressPlayedBg: playerAccent           /* 原来是 --player-primary-color #00a1d6，现统一到主题色 */
    readonly property int progressPaddingHeight: 14    /* .player-progress-schedule-padding height:14px（热区） */

    /* 分段进度条（视点）：.player-progress-schedule-segment（index.scss:580-587、1560-1572）
       每段一条独立横条（不是"分界点"），条本身的数值和普通进度条完全一样
       （高 4px / 底 hsla(0,0%,100%,.2) / 缓冲 hsla(0,0%,100%,.3) / 已播 = playerAccent）。 */
    /*
     * 【圆角：用户要求覆盖参考值 —— 别再改回 0】
     * 参考写的是
     *     .player-progress-schedule.player-progress-schedule-segment { border-radius: 0 }
     * （index.scss:1560-1561）—— 分段态是**方角**，这是参考原样。
     * 用户明确要求："分段后每一段的圆角要和未分段一样"，所以这里取和本体同一个值
     * （progressRadius = 1.5，见上面那条）：底/缓冲/已播三条都用它。
     */
    readonly property real progressSegmentRadius: 1.5       /* 用户要求 = progressRadius(1.5)；参考原文是 0（index.scss:1560-1561） */
    /*
     * 压在某一段上时的纵向放大倍数。
     *
     * 参考有**两档且是叠乘**的：外层 wrap `scaleY(1.5)`（index.scss:506-513）
     * × 本段 `.…-segment.hover { transform: scaleY(1.8) }`（:1569-1572）= **2.7**。
     * 本项目按需求把两档分开用：只是扫过进度区 = progressHoverScale(1.5)，
     * 压在某一段上 = 这一条。
     *
     * 【用户要求 + 为什么取 2.4，不是参考的 2.7】用户要求"压在某一段上时，缩放要比非分段
     * （1.5）那档大"。取 2.4（非分段那档的 1.6 倍）而不是 2.7，理由是几何：
     *   4px 轨道以**中线**为中心放大 2.4 倍 → 9.6px 高，上下各长出 2.8px；
     *   而 pbp 曲线底边离轨道上沿是 pbpDockedGap(3px)（ProgressRow 里 pbp 的 y 减了这条），
     *   2.8 < 3 → 留 0.2px 余量，正好"贴着曲线但不顶进去"；
     *   若用 2.7 则要长出 3.4px > 3 → 会钻进曲线里，和"pbp 与进度条之间留缝"打架。
     */
    readonly property real progressSegmentHoverScale: 2.4   /* 参考 .…-segment.hover scaleY(1.8)（:1569-1572）× 外层 wrap scaleY(1.5)（:506-513）= 2.7；用户要求比非分段(1.5)大 → 取 2.4（见上面说明） */
    readonly property int progressSegmentTransitionMs: 100  /* transition: transform 0.1s cubic-bezier(0,0,.2,1)（index.scss:1562-1566） */
    /*
     * 分段的横向留白，单位是"占整条轨道宽度的百分比"（controls/index.ts:844-859 的三条策略）：
     *   first   width 再减 0.15%；last  left 加 0.15%、width 再减 0.15%；
     *   default left 加 0.15%、width 再减 0.3%。
     * 相邻两段各自让 0.15%，中间的空隙正好就是参考里写的那个 0.3%（marginRight: 0.3%）
     * —— 那个 margin 对"绝对定位且同时给了 left/width"的元素其实不生效，见 ProgressRow.qml 里的说明。
     */
    readonly property real progressSegmentInset: 0.15       /* 0.15%（单侧让位） */
    readonly property real progressSegmentGap: 0.3          /* 0.3%（两段之间的空隙 = 两侧各 0.15%） */
    /* 分段归一化的时间余量：adjustment = duration * ADJUSTMENT_RATIO（controls/index.ts:167 定义、1645 使用） */
    readonly property real progressSegmentAdjustRatio: 0.0015

    /* 悬停气泡里的"当前段标题"：.player-progress-hotspot（index.scss:730-746） */
    readonly property int progressHotspotLineHeight: 18     /* line-height: 18px */
    readonly property int progressHotspotMaxHeight: 48      /* max-height: 48px（正好两行 + 上下 padding） */
    readonly property int progressHotspotPaddingH: 8        /* padding: 6px 8px */
    readonly property int progressHotspotPaddingV: 6
    /* 标题的底色 hsla(0,0%,8%,.9) 与圆角 2px 和菜单同值（index.scss:731 与 679/746 是同一个色），
       所以不再单开 token，直接用 menuBg / menuRadius：
       参考里标题自己没写圆角，是被 .player-progress-popup{overflow:hidden;border-radius:2px} 裁出来的
       （index.scss:685-693），我们的气泡没有这层外壳，所以要自己带上。 */

    /* 高能进度条（pbp）：.player-pbp（index.scss:1592-1605）+ 曲线素材（component/icons/index.ts:408-444） */
    readonly property int pbpHeight: 28                /* height: 28px */
    readonly property int pbpOverhang: 12              /* left: -12px; width: calc(100% + 24px)（左右各外扩 12px） */
    readonly property int pbpPaddingH: 12              /* padding: 0 12px：曲线左右和进度条对齐 */
    readonly property int pbpFullscreenLift: 7         /* 全屏 .player-pbp.show { bottom: calc(100% + 7px) }（index.scss:2486-2488） */
    /*
     * 常驻 + 控制栏收起时，pbp 底边与"播放器最底部那条进度条"之间的间隙：3px。
     * 出处：参考 `.player-pbp { bottom: 3px }`（index.scss:1593）—— 参考里那条 pbp 是相对
     * 控制层底边量 3px 的；我们这边控制栏收起后底部还在的是那条 2px 影子进度条，
     * 所以"不重叠、留一道缝"的等价做法就是：底边 = 影子条上沿再往上 3px。
     */
    readonly property int pbpDockedGap: 3
    readonly property int pbpFadeMs: 200               /* 淡入淡出时长：参考 .player-control-top 的 opacity 0.2s（index.scss:498-500），和进度行同节奏 */
    readonly property color pbpCurveBg: Qt.rgba(1, 1, 1, 0.2)    /* 未播曲线：白色 rect + fill-opacity .2（icons/index.ts:424-429） */
    /*
     * 已播曲线：**同样是 20% 透明度的填充**，只是把颜色换成主题色。
     * 参考里那两层 rect 都在同一个 `<g fill-opacity="0.2">` 里（icons/index.ts:424-435），
     * 所以已播部分并不是实心蓝 —— 用户实测也反馈"太实了，要更透明"。
     * 值 = **主题色的 20%**（原来是 #00a1d6 的 20%：Qt.rgba(0,0.63,0.84,0.2)）；现在用 playerAccentSoft，换主题色时自动跟着变。
     */
    readonly property color pbpCurvePlayedBg: playerAccentSoft
    readonly property color pbpCursorLine: Qt.rgba(1, 1, 1, 0.2) /* 进度竖线 stroke: rgba(255,255,255,.2)（icons/index.ts:439） */
    readonly property int pbpCursorLineWidth: 1                  /* stroke-width: 1 */
    /*
     * pbp 曲线的画法参数（用户实测反馈："两端不能是直上直下，要缓缓归零"）。
     *
     * pbpEndFadeRatio —— 两端各留多少**曲线宽度**做"收敛到基线"的过渡段：0.03（3%）。
     *   为什么是 3%：1200px 宽的进度条上约 36px，足够看出"平滑落到基线"的弧线，
     *   又不至于把有效数据压掉（只有首尾那 3% 被渐隐加权）；取 0 就退回旧画法
     *   （两端立起来），要做对比时可以临时改。
     * pbpSamplePx —— 重采样步长（像素）：2px。原始采样点是 step_sec 一个（mock 是 30 秒），
     *   横向可能有几十像素的间距，直接连折线是硬折角；重采样之后
     *   "相邻点线性插值 + 两端 smoothstep 渐隐"这两级平滑才画得出来。
     */
    readonly property real pbpEndFadeRatio: 0.03
    readonly property int pbpSamplePx: 2
    /*
     * 高能进度条右端那颗 pin（"常驻"开关）：.player-pbp-pin / -pin-icon（index.scss:1607-1621）。
     *
     * 两个状态各一枚素材（**用户提供，path 与参考完全一致**，只去掉了 class/data-pointer）：
     *     assets/images/pbp-pin-off.svg —— 未常驻（默认状态；就是参考里那一枚 PinIcon）
     *     assets/images/pbp-pin-on.svg  —— 已常驻
     * pin 本身没有任何可见样式：白色来自 SVG 里的 fill="#fff"（icons/index.ts:453），
     * 所以主题这里**不需要颜色 token**（Image 直接按素材画，别用 MultiEffect 再着色一遍）。
     */
    readonly property int pbpPinSize: 16               /* .player-pbp-pin-icon { height:16px; width:16px }（index.scss:1614-1621） */
    readonly property int pbpPinRight: 4               /* .player-pbp-pin { right: 4px }（index.scss:1607-1612） */
    readonly property real pbpPinBottomRatio: 0.08     /* .player-pbp-pin { bottom: 8% }（同一处；QML 的 y 是顶边，换算见 ProgressRow） */
    readonly property real pbpPinRotation: 45          /* .player-pbp-pin-icon { transform: rotate(45deg) }（index.scss:1616-1617） */
    /*
     * 命中区外扩：参考里这颗 pin 是**死的**（没有点击/悬停行为），所以没有对应数值。
     * 转 45° 后 16×16 方块的轴对齐包围盒约 22.6px，16 + 4×2 = 24 已经盖住，
     * 也不会像"铺一大块"那样形成死区（见 docs/QML-POPUP-HOVER-PITFALLS.md 第 10 条）。
     */
    readonly property int pbpPinHitPadding: 4

    /*
     * 提示气泡：.player-pbp-pin-tip（index.scss:1623-1639）。
     * 参考里它是 display:none 的**死代码**（谁也没把它打开过），本需求做成真的 hover 提示。
     * 底色/字色参考写的就是那对 tooltip 变量（`var(--player-tooltip-bgcolor, #000)` /
     * `var(--player-tooltip-color, #fff)`，:1624-1628），所以**复用**主题里已有的
     * tooltipBg / tooltipText（出处 index.scss:35-36），不另开两个同值 token。
     */
    readonly property int pbpPinTipFontSize: 12         /* font-size: 12px */
    readonly property real pbpPinTipLineHeight: 1.5     /* line-height: 1.5（参考是倍数；QML 的 lineHeight 默认也是倍数，照抄） */
    readonly property int pbpPinTipPaddingH: 8          /* padding: 4px 8px */
    readonly property int pbpPinTipPaddingV: 4
    readonly property int pbpPinTipRadius: 2            /* border-radius: 2px */
    readonly property int pbpPinTipFadeMs: 300          /* transition: all 0.3s ease-in-out（index.scss:1636-1637） */
    readonly property int pbpPinTipGap: 4               /* 气泡与 pin 之间的空隙（参考没写：它那份 tip 贴着图标且永不显示，直接贴会压住图标） */
    readonly property int pbpPinTipMaxWidth: 220        /* 参考是 white-space:nowrap 不折行；给个上限，长文案不会横着跑出画面 */

    /* 进度条上的圆点：.player-progress-thumb（controls.scss:179-199） */
    readonly property int progressThumbSize: 20        /* 20px */
    readonly property int progressThumbIconSize: 18    /* 里面 svg 18px */

    /* 拖动时上下两个小三角：.player-progress-move-indicator（controls.scss:201-232） */
    readonly property int moveIndicatorWidth: 8
    readonly property int moveIndicatorHeight: 16
    readonly property int moveIndicatorArrow: 4        /* border-width: 4px */

    /* 悬停预览：.player-progress-popup/-preview（controls.scss:234-277） */
    readonly property int progressPopupWidth: 160
    readonly property int progressPopupHeight: 90
    readonly property int progressPopupBottom: 22      /* bottom: 22px */
    readonly property int progressPopupRadius: 2
    readonly property int progressPopupTimeHeight: 18  /* 时间标签 18px */
    readonly property int progressPopupFontSize: 12
    readonly property int progressPopupTimePaddingH: 5
    /*
     * 悬停预览"每几秒一帧"：参考 `Math.floor(this.popup.currentTime / 5)` 的 5
     * （controls/index.ts:1410-1417）—— 同一档内鼠标怎么动都不重新取图，跨档才换。
     * 这是"缩略图不闪"的关键判据（见 ProgressRow 里 requestSnapshotForFrame 的说明）。
     */
    readonly property int progressPreviewFrameSec: 5

    /* 控制条下方的渐变遮罩：.player-control-mask（controls.scss:8-20） */
    readonly property int controlMaskHeight: 100       /* height: 100px，透明 -> 黑 */

    /* 按钮：.player-ctrl-btn / -icon（index.scss:309-346） */
    readonly property int ctrlBtnWidth: 36             /* width: 36px */
    readonly property int ctrlBtnHeight: 22            /* height: 22px */
    readonly property int ctrlIconSize: 22             /* svg 22px */
    readonly property real ctrlIconOpacity: 0.9
    readonly property color ctrlBtnText: Qt.rgba(1, 1, 1, 0.8)   /* color: hsla(0,0%,100%,.8) */
    readonly property color ctrlBtnTextHover: "#ffffff"          /* hover 变纯白 */

    /* 时间：.player-ctrl-time（index.scss:326-330）—— 非全屏那一档的 12px；
       全屏那一档是 14（index.scss:4978-4982），由 PlayerControlBar.timeFontSize 分档给出。
       时间文字和"点时间跳转"的输入框都用那个值，别在这里各写各的。 */
    readonly property int ctrlTimeFontSize: 12
    readonly property int ctrlTimeMinWidth: 90
    readonly property int ctrlTimeMarginRight: 10
    readonly property int ctrlTimeDividePadding: 2     /* .player-ctrl-time-divide padding: 0 2px */

    /*
     * 控制栏左组最后那一颗「分段 / 视点」按钮：.player-ctrl-viewpoint（index.scss:1076-1164）
     *
     * 【为什么文字色单独开 token，不复用 ctrlBtnText】参考里这颗按钮**不是** .player-ctrl-btn 的
     * 那套 `.player-ctrl-btn { color: hsla(0,0%,100%,.8) }`（index.scss:285-287）：
     * 它的文字色写在 `.player-ctrl-viewpoint-content` 上，是 **hsla(0,0%,100%,.9)**
     * （index.scss:1088-1096），和同一个块里的 fill 是同一个声明。两者透明度不同（.8 vs .9），
     * 混用会在控制栏里出现两种白，所以按参考原值分开写。
     */
    readonly property int viewPointFontSize: 14              /* font-size: 14px（index.scss:1094） */
    readonly property color viewPointText: Qt.rgba(1, 1, 1, 0.9)   /* hsla(0,0%,100%,.9)（index.scss:1091） */
    readonly property color viewPointTextHover: "#ffffff"    /* .player-ctrl-btn:hover { color: #fff }（index.scss:297-299） */

    /* 按钮盒子：margin: 0 10px; max-width: 240px; min-width: 45px（index.scss:1081-1085） */
    readonly property int viewPointMarginH: 10
    readonly property int viewPointMaxWidth: 240
    readonly property int viewPointMinWidth: 45

    /* 右箭头图标容器 `.player-ctrl-viewpoint-icon`：18x18（index.scss:1114-1126） */
    readonly property int viewPointIconSize: 18
    /*
     * 图标颜色。参考那份 ArrowRight 是**内联 svg、path 上没有 fill**（component/icons/index.ts:15-28），
     * 颜色靠 CSS 继承 `.player-ctrl-btn { fill: #fff }`（index.scss:285-287）——**不透明**。
     * 注意它和左组另外三颗不同：那三颗的图标挂的是 `.player-ctrl-btn-icon > .common-svg-icon
     * { opacity: .9 }`（index.scss:313-316），视点这颗**没有**那条规则（它不叫 -btn-icon），
     * 所以这里给 1（全白），不要顺手写成 ctrlIconOpacity。
     */
    readonly property real viewPointIconOpacity: 1.0

    /* 展开面板 `.player-ctrl-viewpoint-menu-wrap`（index.scss:1128-1142 + 全屏 2621-2623） */
    readonly property int viewPointPanelWidth: 210          /* width: 210px */
    readonly property int viewPointPanelLeft: -36           /* left: -36px（参考里相对按钮盒左边缘） */
    readonly property int viewPointPanelMaxHeight: 320      /* max-height: 320px（配合裁剪，不是滚动） */
    readonly property color viewPointPanelBg: Qt.rgba(0.08, 0.08, 0.08, 0.9)   /* hsla(0,0%,8%,.9) */
    readonly property int viewPointPanelRadius: 2           /* border-radius: 2px */

    /* 面板里的列表项 `.player-ctrl-viewpoint-menu-item`（index.scss:1149-1163） */
    readonly property int viewPointItemHeight: 30           /* height: 30px（line-height 同为 30） */
    readonly property int viewPointItemFontSize: 12         /* font-size: 12px */
    readonly property int viewPointItemPaddingH: 20         /* padding: 0 20px */
    readonly property color viewPointItemHoverBg: Qt.rgba(1, 1, 1, 0.1)   /* hsla(0,0%,100%,.1) */

    /*
     * hover 开合各延迟 300ms。参考是两个 rafTimeout（controls/index.ts:1729-1750 的
     * `rafTimeout(..., 300)`），按钮 mouseenter/mouseleave 各挂一个（controls/index.ts:360-369），
     * 到点才切 `state-show`（该 class 的样式见 index.scss:1027-1032）。
     */
    readonly property int viewPointToggleDelayMs: 300
    /* 面板底边到按钮盒底边的距离：非全屏 bottom:41px（index.scss:1131）、全屏 bottom:74px（index.scss:2621-2623） */
    readonly property int viewPointPanelBottomFs: 74

    /* 菜单（清晰度/倍速/设置/音量弹层共用）：hsla(0,0%,8%,.9) + 圆角 2（controls.scss:679,746,910,949） */
    readonly property color menuBg: Qt.rgba(0.08, 0.08, 0.08, 0.9)
    readonly property color menuHoverBg: Qt.rgba(1, 1, 1, 0.1)    /* hsla(0,0%,100%,.1) */
    readonly property color menuText: "#ffffff"
    readonly property int menuRadius: 2
    readonly property int menuBottom: 41               /* bottom: 41px */

    /* 清晰度：.player-ctrl-quality（controls.scss:731-808） */
    readonly property int qualityMarginRight: 10
    readonly property int qualityResultFontSize: 14    /* 结果文字 14px 粗体 */
    readonly property int qualityItemWidth: 145
    readonly property int qualityItemHeight: 36
    readonly property int qualityItemPaddingH: 12
    readonly property int qualityMenuMaxHeight: 580
    readonly property color qualityActiveText: playerAccent        /* 选中项用主色（主题色） */
    readonly property int qualityBadgeHeight: 16       /* 角标 16px 圆角 8px */
    readonly property int qualityBadgeRadius: 8
    readonly property int qualityBadgeRight: 12
    readonly property int qualityBadgePaddingH: 5
    readonly property color qualityBadgeBigVipBg: "#f25d8e"       /* 大会员角标底色 */

    /* 倍速：.player-ctrl-playbackrate（controls.scss:899-941）
       列表项顺序照抄 RightControls.ts:59 的 rates = [2, 1.5, 1.25, 1, 0.75, 0.5] */
    readonly property int rateWidth: 50
    readonly property int rateMenuWidth: 70
    readonly property int rateItemHeight: 36
    readonly property int rateResultFontSize: 14
    readonly property var rateOptions: [2, 1.5, 1.25, 1, 0.75, 0.5]

    /* 音量：.player-ctrl-volume（controls.scss:943-1040）
       注意轴是**竖的**：弹层 32x100，滑轨宽 2px，圆点 12px */
    readonly property int volumeBoxWidth: 32
    readonly property int volumeBoxHeight: 100
    readonly property int volumeNumberHeight: 28       /* 里面的数字 28px 高 */
    readonly property int volumeProgressHeight: 60     /* 滑轨区高 60px */
    readonly property int volumeSliderWidth: 2         /* 轨道宽 2px */
    readonly property real volumeSliderRadius: 1.5
    readonly property int volumeThumbSize: 12          /* 圆点 12px */
    readonly property color volumeSliderBg: "#e7e7e7"  /* 轨道底色 */
    readonly property color volumeThumbBg: playerAccent            /* 原来是 --player-fn-color #00aeec，现统一到主题色 */
    readonly property color volumeNumberText: "#e5e9ef"

    /* 提示气泡：--player-tooltip-color / -bgcolor（index.scss:35-36） */
    readonly property color tooltipText: "#ffffff"
    readonly property color tooltipBg: "#000000"

    /*
     * 控制栏齿轮那颗「设置」面板：.bpx-player-ctrl-setting-box / -menu-left / -menu-right
     * （DOM 见 platform/ctrl-setting.txt，尺寸是 style.css 同名 class 的实测值：
     *  左页 132x140、右页 286 宽、内边距 12px 20px、组间距 12px）。
     * 这些只在 SettingsPanel.qml 里用；左页那三行开关 + "更多播放设置"的布局不受影响。
     */
    readonly property int settingsPanelLeftWidth: 132
    readonly property int settingsPanelRightWidth: 286
    readonly property int settingsPanelLeftHeight: 140
    /*
     * 右页高度的**下限**（参考里那一页实测就是 228）。
     *
     * 【为什么只是一个下限】右页高度不能再写死：原来 236 那个值把"四组单选"刚好装下，
     * 后面又加进了「高能进度条」那一行开关，写死的值会把最后两行裁在面板外
     * （看得见半截、点也点不到）。现在由 SettingsPanel.rightHeight 按内容算，
     * 这个 token 只负责"内容再少也别比参考矮"。
     */
    readonly property int settingsPanelRightMinHeight: 228
    readonly property int settingsPanelPadH: 20             /* padding: 12px 20px */
    readonly property int settingsPanelPadV: 12
    readonly property int settingsPanelGroupSpacing: 12     /* .…-content { margin-bottom: 12px } */
    readonly property int settingsPanelSwitchRowHeight: 32  /* 一行开关（.bui-switch 那一行） */
    readonly property int settingsPanelRadioGroupHeight: 44 /* 一组单选 = 标题 16 + 缝 4 + 选项行 24 */
    /*
     * 分组标题那一行：右页「播放方式/视频比例/播放策略/音量均衡」四个标题、
     * 以及最后那个「其他设置」标题都用它 —— 12px 白字、行高 16、垂直居中。
     * 参考：`.bpx-player-ctrl-setting-others-title`（platform\ctrl-setting.txt:176）
     * 与本页那四个分组标题（SettingRadioGroup 里的 title）是同一套写法。
     */
    readonly property int settingsPanelGroupTitleHeight: 16
    /*
     * 分组标题与**它自己那份内容**之间的间距（右页「其他设置」那一组用：
     * 标题下面紧跟着就是「高能进度条」这一项）。分组**之间**才是 settingsPanelGroupSpacing(12)，
     * 组内要更紧 —— 参考里 `.bpx-player-ctrl-setting-others-title` 自己的 margin 在给到的
     * DOM 里没有（只有 class 名），6px 取的是和弹幕设置面板"标题→内容"那一档接近的口径。
     */
    readonly property int settingsPanelGroupTitleGap: 6
    readonly property int settingsPanelBackRowHeight: 16    /* 右页底部那条不可见的「返回左页」热区 */

    /*
     * 「高能进度条」那一行的控件 —— 就是弹幕设置面板里「弹幕随屏幕缩放」那一行用的 **SettingCheck**
     * （DanmakuSettingPanel.qml:772-827 的组件、:247-257 那一行的用法）。
     * 数值全部照抄那一段（在那边是写死的字面量，这里按项目规矩收进主题做 token）：
     *   Image 16×16（sourceSize 16,16）、图标与文字间距 6、整行高 20、文字 12px、未选图标 opacity 0.75；
     *   未选文字 = 那边 panel.textColor（rgba(255,255,255,.8)）、选中或悬停 = 纯白；
     *   图标两态用项目里已有的 assets/images/dm/checkbox.svg / checkbox-checked.svg
     *   （就是参考那两个 32×32 的 bui-checkbox 图标，未选空心方框、选中方框 + 对勾）。
     */
    readonly property int settingsPanelCheckRowHeight: 20       /* SettingCheck: height / implicitHeight = 20 */
    readonly property int settingsPanelCheckIconSize: 16        /* Image 16×16（sourceSize: Qt.size(16,16)） */
    readonly property int settingsPanelCheckGap: 6              /* 图标与文字之间 leftMargin: 6 */
    readonly property int settingsPanelCheckFontSize: 12        /* 文字 12px */
    readonly property real settingsPanelCheckIdleOpacity: 0.75  /* 未选图标 opacity: 0.75 */
    readonly property color settingsPanelCheckText: Qt.rgba(1, 1, 1, 0.8)  /* 未选文字（那边是 panel.textColor） */
    readonly property color settingsPanelCheckTextActive: "#ffffff"        /* 选中或悬停：纯白 */

    /* （原「其他设置」组里那个手填 mock 基址的输入框已按用户要求删除，
       下面这组 settingsPanelInput* 现在没有引用，留着只是记录当初照参考定的尺寸。） */
    readonly property int settingsPanelInputHeight: 22
    readonly property int settingsPanelInputRadius: 2
    readonly property color settingsPanelInputBg: Qt.rgba(1, 1, 1, 0.08)
    readonly property int settingsPanelInputFontSize: 12
    readonly property int settingsPanelInputPaddingH: 8
    readonly property int settingsPanelInputLabelWidth: 56   /* 标签宽 56（该行已删，保留记录） */
    readonly property int settingsPanelInputGap: 8           /* 标签与输入框之间 */

    // ---- 窗口内的普通 UI（设置页、对话框、开关）：这些**跟主题**走 ----
    /* 按钮常态 / 按下底色（设置页里的按钮、关闭按钮） */
    readonly property color buttonBg: dark ? "#2a2a35" : "#e3e6eb"
    readonly property color buttonPressedBg: dark ? playerAccentActive : playerAccentFaint
    /* 这些按钮上的文字 */
    readonly property color controlText: dark ? "#ffffff" : "#2a2d33"
    /* 强调色：开关打开、单选项选中（设置页） */
    readonly property color accent: playerAccent

    /* "拖文件进来"那行提示（只在没选片子时出现，压在 windowBg 上） */
    readonly property color hintText: dark ? "#808090" : "#6b7280"

    // ---- 设置页 ----
    /* 遮罩：压在画面上的半透明黑，浅色主题下淡一点 */
    readonly property color scrim: dark ? "#aa000000" : "#88000000"
    readonly property color panelBg: dark ? "#1e1e26" : "#ffffff"
    readonly property color panelBorder: dark ? "#3a3a48" : "#d5d8de"
    /* 面板里的标题/标签文字 */
    readonly property color panelText: dark ? "#d0d0e0" : "#2a2d33"
    /* 面板里的小字说明 */
    readonly property color panelHintText: dark ? "#808090" : "#6b7280"
    /* 一排"多选一"按钮的常态 / 选中底色 */
    readonly property color choiceBg: dark ? "#2a2a35" : "#e3e6eb"
    readonly property color choiceActiveBg: dark ? playerAccentActive : playerAccentFaint
    /* 开关：关着时的轨道色（打开时用 accent），滑块 */
    readonly property color switchOffBg: dark ? "#3a3a48" : "#c9ccd3"
    readonly property color switchKnob: "#ffffff"

    // ---- 左上角那几行诊断文字 ----
    /*
     * 诊断文字分两种底色：
     *   * 没选片子时它压在 windowBg 上（用主题里的 diagText）；
     *   * 选了片子之后窗口是黑的（videoBg），浅色主题那套深色字在黑底上就看不清了，
     *     所以这几行要换成 onVideoText（浅灰）。
     * Main.qml 里的绑定就是按"有没有片源"在这两个色之间切。
     */
    readonly property color diagText: dark ? "#9090a0" : "#5a6070"
    readonly property color onVideoText: dark ? "#9090a0" : "#c6cad2"
    /* 零拷贝开着 / CPU 回退（警告色）。黑底上这两色本身就够亮，不需要分两套 */
    readonly property color okText: dark ? "#7fe07f" : "#1a7f37"
    readonly property color warnText: dark ? "#e0c060" : "#9a6700"
    readonly property color errorText: dark ? "#ff7070" : "#c0392b"
}
