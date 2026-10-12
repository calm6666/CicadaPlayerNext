// ===========================================================================
// 交互卡片层 —— 五类卡片（点赞关注 / 外链 / 投票 / 评分 / 评分总结）的
// 时间窗驱动与装配 + 左下角缩小条目（照 hili-player interaction/index.ts 复刻：
// 展示 + 编辑两种模式）
//
// 数据流（和 viewPoints / pbData / previewFrames 同一条路）：
//   Main.qml fetchProgressMetaFrom() → GET /x/player/interaction →
//   playerView.interactionData → 本层 → 五类卡片
//   数据结构照 hili-player 的 InteractionCard：
//     { guideList: [{type, top, left, timeStart, timeEnd}],
//       linkList:  [{top, left, linkContent, timeStart, timeEnd}],
//       voteList:  [{top, left, question, options, timeStart, timeEnd}],
//       scoreList: [{top, left, title, scoreType, timeStart, timeEnd}],
//       scoreSumList: [{top, left, timeStart, timeEnd}] }
//   top/left 是百分比（相对整个播放区 —— 见下面内容区处的坐标系说明），
//   timeStart/timeEnd 单位秒。
//   没有数据 / 接口不通 = 一张卡片都不显示（静默降级，界面不变）。
//
// 【时间窗互斥（照 web 端第六轮定稿）】一次只能存在一张卡片：
//   五类列表全部卡片的时间窗两两不相交（半开区间相交判定，
//   边界相接不算重叠）；addCard / updateCardContent 的时间补丁先做
//   重叠预检，重叠即拒绝（返回 false，不入数据）；scoreSum 前置：
//   必须已有至少一张评分卡才允许添加。
//
// 【左下角缩小条目（照 web ShrinkBar）】展示模式下非 guide 卡片被手动
//   关闭（关闭环未归零时点 ×）→ 卡片淡出 + 左下角生成缩小条目（图标 +
//   标题 + 26px 进度环继续倒数）：归零移除；源数据被删 / 重开 / seek 回
//   关闭时刻之前 → 移除；点击条目 → 清关闭标记 → 卡片立即重建显示。
//   关闭态存数据对象（__isClose / __closeTime，照 web 的 isClose/closeTime
//   字段），delegate 重建后按数据恢复。
//
// 布局（照 interaction.scss 的 .nova-player-cmd-dm-wrap / -inside）：
//   层铺满播放区；卡片以 top/left 百分比定位 + 中心锚定
//   （参考的 transform: translate(-50%,-50%)）。参考的 padding: 9.5952024%
//   只作用于流式子元素 —— absolute 定位的百分比相对 padding box（整个区域），
//   所以这里不做内缩（详见下面内容区处的注释）。
//
// 时间窗状态机（照 index.ts，CARD_CREATE_LEAD = 0.6s / CARD_FADE_MS = 350ms）：
//   now = player.position / 1000（毫秒 → 秒；position 的更新频率驱动整棵绑定树，
//   暂停时 position 不变 → 状态自然冻结，seek 时跳变 → 状态即时重算）
//   * now < timeStart - 0.6   —— 窗口外：直接隐藏（不渐隐，参考"seek 回窗口前直接删"）
//                                且此时复位 closed（"seek 回退可恢复"，照 closeEntry）
//   * timeStart <= now <= timeEnd 且未被关闭 —— 显示（渐入 350ms）
//                                remaining = 1 - clamp((now-timeStart)/
//                                (timeEnd-timeStart), 0, 1)（驱动关闭环倒数）
//   * now > timeEnd 或被用户关闭 —— 淡出 350ms 后不再可见（对象保留，状态不丢）
//
// 编辑模式（照 hili-player demo 的编辑面板 + dragEditor.ts，全局开关
// QtPlayerAppMode.interactionMode 在首页切换）：
//   * 卡片常驻：不看时间窗、closed 也不隐藏（编辑的是布局与内容，不是观看）；
//     进出编辑模式时 opacity 的 Behavior 关掉 → 瞬切，无"闪一下"；
//   * 拖拽定位：照 dragEditor.ts 的 SAFE_MARGIN（左 3 / 右 3 / 上 5 / 下 15，%）——
//     卡片是中心锚定（x = left%*W - w/2），所以"视觉左缘 ≥ 3%W"直接等价于
//     "x ≥ 0.03W"，夹取边界在按下时按卡片尺寸现算；拖完换算百分比回写数据；
//   * 贴边辅助线：照 AlignLines.ts —— 四根 1px #858688 线画在「视觉安全边界」
//     （左 3% / 右 97% / 上 5% / 下 85%，底部预留控制栏），贴边时显示（0.2s 过渡）；
//   * 编辑 API（照 index.ts 的公共 API，语义一致；第六轮起返回 boolean ——
//     重叠 / 前置不满足返回 false，调用方 toast 提示）：
//       addCard(kind, data) / removeCard(kind, i) / updateCardContent(kind, i, patch)
//       / closeCard(kind, i) / wouldOverlap(kind, i, ts, te) / getStatus() / updateData(data)
//     QML 的 property var 没有"字段级"属性通知 → 补丁落数据后**全量重赋**
//     interactionData 触发 rebuildModel（Repeater 重建 delegate，数据引用不变但
//     新 delegate 会读到新字段值，文本/选项数量即刻正确）；
//   * 交互状态外置：投票（votedIndex/totalVotes/extras）、评分（selected/count/
//     totalScore）、三连（like/coin/collect/following）这些"观众交互态"存在本层
//     persistStates（key = "kind:listIndex"），卡片经 stateMutated/restoreState
//     协议读写 —— delegate 因数据重赋被重建时状态不丢（对应参考里"交互状态按
//     listIndex 存 Map 保留"）。
//
// 与参考的两处刻意差异（QML 侧更优，行为等价）：
//   1. 参考"提前 0.6s 创建隐藏 DOM"是为避免 DOM 创建卡顿；QML 里 delegate 常驻，
//      显隐只是 opacity/visible，没有创建成本 → 0.6s 只保留在"窗口外直接隐藏"的判据里；
//   2. 参考"淡出 350ms 后删 DOM、交互状态按 listIndex 存 Map 保留"；QML 里 delegate
//      不销毁 → 展示模式的交互状态天然跨显隐保留（编辑模式的重建则由 persistStates 兜）。
// ===========================================================================
import QtQuick
import QtPlayer

/*
 * 【根 id 不能叫 layer】Item 有内建属性 layer（QQuickItemLayer，图层特效）：
 * delegate 内的绑定按"宿主属性优先于文档 id"解析，`layer.stateFor(...)` 会解析到
 * delegate 自己的 Item.layer（QQuickItemLayer）→ TypeError + masterModel/editMode
 * 全部取空 → 整层一张卡片都不出（这是上一版的线上事故，勿再犯）。
 * 改叫 interactionLayer，避开一切内建属性名。
 */
Item {
    id: interactionLayer

    // ---------------------------------------------------------------------
    // 对外接口
    // ---------------------------------------------------------------------

    /* 播放器（读 position 毫秒 —— 与 DanmakuView/ProgressRow 同一个源） */
    property var player: null

    /* 交互卡片数据（InteractionCard 结构，null/空 = 无卡片） */
    property var interactionData: null

    /* 是否全屏（大尺寸档判据之一） */
    property bool fullscreen: false

    /* 编辑模式（首页 QtPlayerAppMode 切换；卡片常驻 + 可拖拽 + 编辑 API 生效） */
    readonly property bool editMode: QtPlayerAppMode.interactionMode === "edit"

    /*
     * 交互状态外置表：key = "kind:listIndex" → 快照对象（卡片自己定义字段）。
     * 整体重赋触发属性通知（QML var 属性的常规做法），卡片经 restoreState 恢复。
     */
    property var persistStates: ({})

    /* 数据结构变化（增删改/换数据）后发 —— 右侧 API 面板据此刷新列表 */
    signal cardsChanged()

    /* 拖拽结束（编辑模式里挪了卡片）—— top/left 是新的百分比 */
    signal cardMoved(string kind, int listIndex, real top, real left)

    /* 播放时钟（秒）：position 每次变化都重算下面整棵绑定树 —— 卡片就几张，
       绑定都是简单算术，代价可忽略 */
    readonly property real clockSec: interactionLayer.player !== null ? interactionLayer.player.position / 1000 : 0

    /* 大尺寸档（照参考媒体查询 min-width:1366.9px 的大档规格，用户指定的 Qt 侧
       判据：**只看播放区宽度** > 1366.9 —— Qt 没有网页的媒体查询/data-screen，
       播放区够宽就放大、拉窄就缩回；不再叠加全屏条件。五类卡片都按 large
       属性切换两套尺寸（interaction.scss L774-879 的大档数值）。 */
    readonly property bool large: interactionLayer.width > 1366.9

    // ---------------------------------------------------------------------
    // 左下角缩小条目（照 web 端 shrinkActive / ShrinkBar 的生命周期）
    //
    // 条目数据：{ id: "kind:listIndex", kind, listIndex, icon, title,
    //            timeStart, timeEnd, closedAt, removed }
    // （id 与 web 端 "shrink:类型:下标" 同构，用作幂等替换的键；
    //   timeStart/timeEnd/closedAt 为关闭时刻的时间窗与播放位置快照 ——
    //   条目的进度环与生死判定**只用快照 + 播放时钟**，绝不回读源卡片
    //   数据：__isClose 读不到 / 数据被替换 / 下标位移都不会让条目
    //   提前消失（用户实测两轮的根因即源回读路径的失效误判）；
    //   removed 为点击重开的自我移除标记）。
    // ---------------------------------------------------------------------

    property var shrinkModel: []

    /* 编辑模式切换：条目清空（编辑模式卡片常驻显示，缩小条目无意义 —— 且
       web 端编辑模式不生成条目，这里同样保证切换后不残留） */
    onEditModeChanged: interactionLayer.shrinkModel = []

    /* 播放时钟每拍同步缩小条目（归零 / 源失效移除 —— 照 syncShrinkEntries） */
    onClockSecChanged: interactionLayer.syncShrinkEntries()

    /* 缩小条目图标 / 标题映射（照 icons.ts 的 SHRINK_META_MAP：
       link→link-video+"推荐"；vote→vote+"投票"；score/scoreSum→score+"推荐"） */
    function shrinkMetaOf(kind) {
        if (kind === "link")
            return { icon: "../../assets/images/interaction/link-video.png",
                     title: qsTr("推荐") }
        if (kind === "vote")
            return { icon: "../../assets/images/interaction/vote.png",
                     title: qsTr("投票") }
        /* score / scoreSum 同款（guide 无关闭概念不参与，兜底同 score） */
        return { icon: "../../assets/images/interaction/score.png",
                 title: qsTr("推荐") }
    }

    /*
     * 生成缩小条目（卡片关闭时调用）：同卡片同下标幂等替换（互斥时间窗下
     * 理论上不会共存，照 web appendShrinkEntry 的 filter-then-push）。
     * 条目自带全套快照：时间窗（ts/te）+ 关闭时刻播放位置（closedAt）。
     * 进度环与生死判定都用快照计算，不回读源数据 —— 数据替换 / __isClose
     * 读写异常 / 下标位移都不影响条目按原窗口倒数到归零。
     */
    function appendShrinkEntry(kind, listIndex) {
        var meta = interactionLayer.shrinkMetaOf(kind)
        var id = kind + ":" + listIndex

        /* 时间窗快照（照 web 条目创建时从源数据取值的语义） */
        var ts = 0
        var te = 0
        var d = interactionLayer.interactionData
        if (d !== null && d !== undefined) {
            var list = d[kind + "List"]
            var src = (list !== undefined && list !== null) ? list[listIndex] : undefined
            if (src !== undefined) {
                ts = (src.timeStart !== undefined) ? src.timeStart : 0
                te = (src.timeEnd !== undefined) ? src.timeEnd : 0
            }
        }

        var next = []

        for (var i = 0; i < interactionLayer.shrinkModel.length; i++) {
            if (interactionLayer.shrinkModel[i].id !== id)
                next.push(interactionLayer.shrinkModel[i])
        }

        next.push({ id: id, kind: kind, listIndex: listIndex,
                    icon: meta.icon, title: meta.title,
                    timeStart: ts, timeEnd: te,
                    closedAt: interactionLayer.clockSec, removed: false })
        interactionLayer.shrinkModel = next
    }

    /*
     * 缩小条目的进度环剩余比例（照 web syncShrinkEntries 的公式）：
     * 用条目自带的时间窗快照 + 播放时钟现算（1 → 0）—— 不回读源卡片数据，
     * 数据替换 / 下标位移都不影响进度环倒数。绑定调用：delegate 的绑定
     * 显式引用 interactionLayer.clockSec 作第二参数（直接属性引用保证
     * 每拍重算，进度环绝不失步）。
     */
    function shrinkRemainingOf(entry, nowSec) {
        if (entry === null || entry === undefined)
            return 0

        var ts = entry.timeStart !== undefined ? entry.timeStart : 0
        var te = entry.timeEnd !== undefined ? entry.timeEnd : 0
        var now = (nowSec !== undefined && nowSec !== null)
                   ? nowSec : interactionLayer.clockSec

        if (te <= ts)
            return 0
        if (now <= ts)
            return 1
        if (now >= te)
            return 0
        return 1 - (now - ts) / (te - ts)
    }

    /*
     * 条目是否该被移除的判定（**完全自治** —— 只用条目快照 + 播放时钟，
     * 不回读源卡片数据）：供 syncShrinkEntries 每拍扫描与 commitShrinkRemoval
     * 延迟提交共用同一口径。三条移除路径：
     *   1. removed：条目被点击重开（自我移除标记 —— 重开复位卡片后条目使命结束）；
     *   2. 归零：播放越过快照 timeEnd（倒计时走完）；
     *   3. seek 回退：播放位置退回关闭时刻之前（复位语义 —— 与 slot 的
     *      rewindBeforeClose / beforeWindow 复位口径对齐，卡片下次进窗重现，
     *      条目随之退场）。
     * 【为什么删掉 __isClose 回读】源回读在两轮用户实测中都出现"读不到 /
     * 误判失效"导致条目提前消失（读写对象引用在数据替换/重建路径下不再
     * 可靠）；自治快照让条目生死只由时间决定，任何数据层扰动都侵扰不到。
     */
    function shrinkEntryDone(entry) {
        /* 点击重开的自我移除标记 */
        if (entry.removed === true)
            return true

        /* 归零（播放越过快照 timeEnd）→ 移除 */
        var te = entry.timeEnd !== undefined ? entry.timeEnd : 0
        if (interactionLayer.clockSec >= te)
            return true

        /* seek 回关闭时刻之前（复位语义）→ 移除 */
        var closedAt = entry.closedAt !== undefined ? entry.closedAt : 0
        return interactionLayer.clockSec < closedAt
    }

    /*
     * 每拍同步缩小条目（照 web syncShrinkEntries + scheduleShrinkRemoval 的
     * 两段式淡出协议）：
     *   * 命中移除条件 → 对应 delegate 播 0.35s 淡出（beginClose 幂等），
     *     350ms 后由 commitShrinkRemoval 真正从数组剔除（DOM 销毁）；
     *   * 淡出期间条目仍在数组中重复扫描 —— beginClose 幂等，无副作用。
     * 【为什么不再直接改数组】之前判定即删（shrinkModel = kept）导致
     * Repeater 立即销毁 delegate —— 条目无过渡"瞬间消失"（用户实测）。
     * 【timer 只认"新淡出"】只有本轮新翻 closing 的条目才 restart 计时器：
     * 已在淡出中的条目重复 restart 会把 350ms 到点时间无限推后
     * （position 上报快于 350ms 时条目永不删除，成"透明僵尸"）。
     */
    function syncShrinkEntries() {
        var cur = interactionLayer.shrinkModel
        if (cur.length === 0)
            return

        var pending = false

        for (var i = 0; i < cur.length; i++) {
            if (interactionLayer.shrinkEntryDone(cur[i])) {
                /* 触发对应 delegate 淡出（Repeater 顺序 = model 顺序；
                   已 closing 的条目不重复计时 —— timer 到点时间不被推后） */
                var bar = shrinkRepeater.itemAt(i)
                if (bar !== null && bar !== undefined && !bar.closing) {
                    bar.beginClose()
                    pending = true
                }
            }
        }

        if (pending)
            shrinkFadeTimer.restart()
    }

    /*
     * 淡出完成后的真正移除（shrinkFadeTimer 触发）：按同一判定口径把已
     * 完成使命的条目从数组剔除（delegate 随之销毁 —— 此时透明度已归 0，
     * 销毁不产生视觉跳变）。重开路径中条目点击时也已 beginClose，由本
     * 函数统一收尾（与 web 的 scheduleShrinkRemoval 延时 filter 对齐）。
     */
    function commitShrinkRemoval() {
        var cur = interactionLayer.shrinkModel
        if (cur.length === 0)
            return

        var kept = []
        var changed = false

        for (var i = 0; i < cur.length; i++) {
            if (interactionLayer.shrinkEntryDone(cur[i])) {
                changed = true
            } else {
                kept.push(cur[i])
            }
        }

        if (changed)
            interactionLayer.shrinkModel = kept
    }

    /*
     * 点击缩小条目重开原卡片（照 web reopenFromShrink + scheduleShrinkRemoval
     * 的淡出协议）：条目先播 0.35s 淡出（beginClose + removed 自我移除标记），
     * 350ms 后由 commitShrinkRemoval 统一从数组剔除；清源数据关闭标记 →
     * 对应 slot 复位 closed → 卡片按当前时间立即重建显示（仍在时间窗内即刻恢复）。
     */
    function reopenFromShrink(entry) {
        /* 条目淡出（点过的条目不再响应点击；removed 标记供 commitShrinkRemoval
           判定剔除 —— 自治协议不再回读 __isClose） */
        for (var i = 0; i < interactionLayer.shrinkModel.length; i++) {
            if (interactionLayer.shrinkModel[i].id === entry.id) {
                var bar = shrinkRepeater.itemAt(i)
                if (bar !== null && bar !== undefined)
                    bar.beginClose()
                interactionLayer.shrinkModel[i].removed = true
                break
            }
        }
        shrinkFadeTimer.restart()

        var d = interactionLayer.interactionData
        if (d === null || d === undefined)
            return

        var list = d[entry.kind + "List"]
        if (list === undefined || list === null)
            return

        var src = list[entry.listIndex]
        if (src === undefined)
            return

        /* 清关闭标记：卡片回到「窗口内即显示」状态 */
        src.__isClose = false

        /* 对应 slot 复位（卡片立即重建显示） */
        for (var j = 0; j < repeater.count; j++) {
            var slotItem = repeater.itemAt(j)
            if (slotItem !== null && slotItem.kind === entry.kind
                    && slotItem.listIndex === entry.listIndex) {
                slotItem.closed = false
                break
            }
        }
    }

    /*
     * 聚合评分总结数据（照 web renderScoreSumEntry 的条目创建时一次性聚合）：
     * 全部评分卡题目 + 当前平均分（persistStates 里 score:i 快照的
     * totalScore/count，保留一位小数）+ 总参与人数。
     */
    function aggregateScoreSum() {
        var items = []
        var totalCount = 0
        var d = interactionLayer.interactionData

        if (d !== null && d !== undefined
                && d.scoreList !== undefined && d.scoreList !== null) {
            for (var i = 0; i < d.scoreList.length; i++) {
                var title = d.scoreList[i].title !== undefined
                            ? d.scoreList[i].title : ""
                var state = interactionLayer.stateFor("score:" + i)
                var c = (state !== undefined && state.count !== undefined)
                        ? state.count : 0
                var t = (state !== undefined && state.totalScore !== undefined)
                        ? state.totalScore : 0
                items.push({ name: title,
                             average: c > 0 ? (t / c).toFixed(1) : "0.0" })
                totalCount += c
            }
        }

        return { items: items, count: totalCount }
    }

    /* 展开成带类型标记的一维模型（顺序照参考创建顺序：guide → link → vote →
       score → scoreSum，后声明的压在上面 —— Repeater 同 z 按序绘制，与 DOM 顺序一致） */
    property var masterModel: []

    onInteractionDataChanged: {
        /* 数据整体替换（换片重拉 / 编辑 API 的全量重赋）：旧条目引用的源
           已不存在，统一清空（自治协议下条目不回读源数据，数据替换必须
           显式清场；展示模式下走到这里的只有换片，编辑模式条目本来就空） */
        interactionLayer.shrinkModel = []
        interactionLayer.rebuildModel()
    }

    function rebuildModel() {
        var arr = []
        var d = interactionLayer.interactionData

        if (d !== null && d !== undefined) {
            var lists = [["guide", d.guideList], ["link", d.linkList],
                         ["vote", d.voteList], ["score", d.scoreList],
                         ["scoreSum", d.scoreSumList]]

            for (var i = 0; i < lists.length; i++) {
                var list = lists[i][1]

                if (list === undefined || list === null)
                    continue

                /* listIndex 一并存进去：编辑 API 的下标口径和参考一致（各自列表内的
                   下标），持久化状态的 key 也用它 —— rebuildModel 后数组虽然全换，
                   但 {kind, listIndex} → 数据 的映射不变，状态能对上号 */
                for (var j = 0; j < list.length; j++)
                    arr.push({ kind: lists[i][0], data: list[j], listIndex: j })
            }
        }

        interactionLayer.masterModel = arr

        /*
         * 【数据初装也要通知面板】右侧编辑面板的列表靠 cardsChanged 刷新 ——
         * 之前只有编辑 API（notifyDataChanged）发它，mock 数据第一次落到
         * interactionData 时走的是这里 → 面板 rows 一直空 → "页面上有卡片、
         * 右侧却显示没有数据"（用户实测）。web 端面板是每次 getStatus 现拉，
         * Qt 端用信号对齐这个时点。
         */
        interactionLayer.cardsChanged()
    }

    // ---------------------------------------------------------------------
    // 编辑 API（照 index.ts 的公共 API；kind = "guide" / "link" / "vote" /
    //         "score" / "scoreSum"；第六轮起 add/update 返回 boolean ——
    //         时间窗重叠 / scoreSum 前置不满足返回 false，调用方 toast 提示）
    // ---------------------------------------------------------------------

    /*
     * 数据落完之后统一走这里：全量重赋 interactionData（浅拷贝新对象）→
     * onInteractionDataChanged → rebuildModel → Repeater 重建 delegate。
     * QML property var 没有字段级通知，这是最可靠的最小路径；交互状态由
     * persistStates 兜住不丢。最后发 cardsChanged 让面板刷新。
     */
    function notifyDataChanged() {
        var d = interactionLayer.interactionData

        if (d === null || d === undefined)
            return

        interactionLayer.interactionData = {
            guideList: d.guideList,
            linkList: d.linkList,
            voteList: d.voteList,
            scoreList: d.scoreList,
            scoreSumList: d.scoreSumList
        }
        cardsChanged()
    }

    /*
     * 时间窗互斥预检（照 web wouldOverlap）：半开区间相交判定
     * （itemStart < newEnd && itemEnd > newStart，边界相接不算重叠）；
     * timeStart/timeEnd 缺省的条目不参与；五类列表全查、排除自身
     * （同类型同下标 —— 新增时 index 传 -1 不排除任何条目）。
     */
    function wouldOverlap(kind, index, timeStart, timeEnd) {
        var d = interactionLayer.interactionData
        if (d === null || d === undefined)
            return false

        var lists = [["guide", d.guideList], ["link", d.linkList],
                     ["vote", d.voteList], ["score", d.scoreList],
                     ["scoreSum", d.scoreSumList]]

        for (var i = 0; i < lists.length; i++) {
            var list = lists[i][1]
            if (list === undefined || list === null)
                continue

            for (var j = 0; j < list.length; j++) {
                /* 排除自身（同类型同下标） */
                if (lists[i][0] === kind && j === index)
                    continue

                var item = list[j]
                if (item.timeStart === undefined || item.timeEnd === undefined)
                    continue

                if (item.timeStart < timeEnd && item.timeEnd > timeStart)
                    return true
            }
        }

        return false
    }

    /*
     * 添加卡片（照 addGuide/addLink/addVote/addScore/addScoreSum，boolean 语义）：
     * 时间窗与任何已有卡片重叠 → 拒绝；scoreSum 前置（必须已有至少一张
     * 评分卡）→ 拒绝；成功入列表 + 立即可见并返回 true。
     */
    function addCard(kind, cardData) {
        var d = interactionLayer.interactionData

        if (d === null || d === undefined)
            return false

        /* scoreSum 前置：无可汇总的评分卡（照 addScoreSum 的前置校验） */
        if (kind === "scoreSum") {
            if (d.scoreList === undefined || d.scoreList === null
                    || d.scoreList.length === 0)
                return false
            /* 新字段兜底：旧数据可能没有 scoreSumList */
            if (d.scoreSumList === undefined || d.scoreSumList === null)
                d.scoreSumList = []
        }

        /* 时间窗完整才参与互斥校验（照 web addXxx 的判定口径） */
        if (cardData.timeStart !== undefined && cardData.timeEnd !== undefined
                && interactionLayer.wouldOverlap(kind, -1,
                                                 cardData.timeStart,
                                                 cardData.timeEnd))
            return false

        if (kind === "guide")
            d.guideList.push(cardData)
        else if (kind === "link")
            d.linkList.push(cardData)
        else if (kind === "vote")
            d.voteList.push(cardData)
        else if (kind === "score")
            d.scoreList.push(cardData)
        else if (kind === "scoreSum")
            d.scoreSumList.push(cardData)
        else
            return false

        interactionLayer.notifyDataChanged()
        return true
    }

    /* 移除卡片（照 removeCard：数据从列表删除，跨快照不再出现；
       下标位移后缩小条目的源映射失效 → 一并清空，照 web removeCard 清空 shrinkActive） */
    function removeCard(kind, index) {
        var d = interactionLayer.interactionData

        if (d === null || d === undefined)
            return

        var list = null

        if (kind === "guide")
            list = d.guideList
        else if (kind === "link")
            list = d.linkList
        else if (kind === "vote")
            list = d.voteList
        else if (kind === "score")
            list = d.scoreList
        else if (kind === "scoreSum")
            list = d.scoreSumList

        if (list === null || index < 0 || index >= list.length)
            return

        /* 下标位移后，后面卡片的状态 key 全部对不上号 —— 参考也是"重建全部条目"。
           同步把持久化状态按新下标搬移（参考 Map 按 listIndex 存、splice 后整体刷新）。 */
        list.splice(index, 1)
        interactionLayer.shiftPersistStates(kind, index)
        interactionLayer.shrinkModel = []
        interactionLayer.notifyDataChanged()
    }

    /*
     * 编辑卡片内容（照 updateCardContent 的补丁语义：未提供的字段保持原值；
     * 返回 boolean —— 时间补丁与任何已有卡片重叠 → 整个补丁拒绝）。
     * 可用字段（CardContentPatch）：guideType / linkContent / question /
     * optionTexts / title / scoreType / timeStart / timeEnd / top / left。
     */
    function updateCardContent(kind, index, patch) {
        var d = interactionLayer.interactionData

        if (d === null || d === undefined || patch === null || patch === undefined)
            return false

        var item = null

        if (kind === "guide")
            item = d.guideList[index]
        else if (kind === "link")
            item = d.linkList[index]
        else if (kind === "vote")
            item = d.voteList[index]
        else if (kind === "score")
            item = d.scoreList[index]
        else if (kind === "scoreSum")
            item = d.scoreSumList[index]

        if (item === undefined || item === null)
            return false

        /* 时间补丁先合并现值再预检重叠（照 web applySchedulePatch：
           只改一端时另一端用现值参与判定，避免漏判）；重叠 → 整体拒绝 */
        if (patch.timeStart !== undefined || patch.timeEnd !== undefined) {
            var ts = patch.timeStart !== undefined ? patch.timeStart : item.timeStart
            var te = patch.timeEnd !== undefined ? patch.timeEnd : item.timeEnd

            if (ts !== undefined && te !== undefined
                    && interactionLayer.wouldOverlap(kind, index, ts, te))
                return false
        }

        /* 时间窗 / 定位（五类通用） */
        if (patch.timeStart !== undefined)
            item.timeStart = patch.timeStart
        if (patch.timeEnd !== undefined)
            item.timeEnd = patch.timeEnd
        if (patch.top !== undefined)
            item.top = patch.top
        if (patch.left !== undefined)
            item.left = patch.left

        /* 各类型自己的内容字段（scoreSum 仅 title 可编辑 —— 题目列表 /
           平均分 / 总人数运行期从 scoreList 聚合） */
        if (kind === "guide") {
            if (patch.guideType !== undefined)
                item.type = patch.guideType
        } else if (kind === "link") {
            if (patch.linkContent !== undefined)
                item.linkContent = patch.linkContent
        } else if (kind === "vote") {
            if (patch.question !== undefined)
                item.question = patch.question
            if (patch.optionTexts !== undefined) {
                /* 照 index.ts：重叠区间原地改写文案（保留票数），尾部增删选项 */
                var texts = patch.optionTexts
                var oldCount = item.options.length
                var overlap = Math.min(texts.length, oldCount)

                for (var i = 0; i < overlap; i++)
                    item.options[i].optionText = texts[i]
                for (var j = oldCount; j < texts.length; j++)
                    item.options.push({ optionText: texts[j] })
                if (oldCount > texts.length)
                    item.options.length = texts.length
            }
        } else if (kind === "score") {
            if (patch.title !== undefined)
                item.title = patch.title
            if (patch.scoreType !== undefined)
                item.scoreType = patch.scoreType
        } else if (kind === "scoreSum") {
            if (patch.title !== undefined)
                item.title = patch.title
        }

        interactionLayer.notifyDataChanged()
        return true
    }

    /* 关闭卡片（照 closeCard：本次播放关闭 —— 展示模式淡出并生成缩小条目；
       guide 无关闭概念） */
    function closeCard(kind, index) {
        if (kind === "guide")
            return

        for (var i = 0; i < repeater.count; i++) {
            var row = repeater.itemAt(i)

            if (row !== null && row.kind === kind && row.listIndex === index) {
                row.requestClose()
                return
            }
        }
    }

    /* 获取状态（照 getStatus：返回数据快照，改动不回写） */
    function getStatus() {
        if (interactionLayer.interactionData === null || interactionLayer.interactionData === undefined)
            return null

        return JSON.parse(JSON.stringify(interactionLayer.interactionData))
    }

    /* 更新数据（照 updateData：替换显式传入的列表，交互状态清零重建；
       换数据后缩小条目的源映射失效 → 一并清空，照 web updateData） */
    function updateData(data) {
        var d = interactionLayer.interactionData

        if (d === null || d === undefined || data === null || data === undefined)
            return

        if (data.guideList !== undefined)
            d.guideList = data.guideList
        if (data.linkList !== undefined)
            d.linkList = data.linkList
        if (data.voteList !== undefined)
            d.voteList = data.voteList
        if (data.scoreList !== undefined)
            d.scoreList = data.scoreList
        if (data.scoreSumList !== undefined)
            d.scoreSumList = data.scoreSumList

        interactionLayer.persistStates = ({})
        interactionLayer.shrinkModel = []
        interactionLayer.notifyDataChanged()
    }

    // ---------------------------------------------------------------------
    // 交互状态持久化（key = "kind:listIndex"）
    // ---------------------------------------------------------------------

    function stateKeyOf(kind, listIndex) {
        return kind + ":" + listIndex
    }

    function stateFor(key) {
        var s = interactionLayer.persistStates[key]
        return (s === undefined) ? undefined : s
    }

    function persistState(key, snapshot) {
        var next = {}

        for (var k in interactionLayer.persistStates)
            next[k] = interactionLayer.persistStates[k]

        next[key] = snapshot
        interactionLayer.persistStates = next
    }

    /* removeCard 后下标位移：kind 列表里 > removed 的 key 整体前移一位 */
    function shiftPersistStates(kind, removedIndex) {
        var next = {}
        var prefix = kind + ":"

        for (var k in interactionLayer.persistStates) {
            if (k.indexOf(prefix) === 0) {
                var idx = parseInt(k.substring(prefix.length), 10)

                if (idx === removedIndex)
                    continue           /* 被删那行：状态一并丢弃 */
                if (idx > removedIndex)
                    k = prefix + (idx - 1)
            }
            next[k] = interactionLayer.persistStates[k]
        }

        interactionLayer.persistStates = next
    }

    // ---------------------------------------------------------------------
    // 贴边辅助线（照 AlignLines.ts + interaction.scss：1px #858688，
    //            位置 = 视觉安全边界 左3% / 右97% / 上5% / 下85%，0.2s 过渡）
    // ---------------------------------------------------------------------

    /* 当前显示哪些线（整体重赋发通知） */
    property var alignShow: ({ left: false, right: false, top: false, bottom: false })

    function updateAlignLines(bounds) {
        interactionLayer.alignShow = {
            left: bounds.left === true,
            right: bounds.right === true,
            top: bounds.top === true,
            bottom: bounds.bottom === true
        }
    }

    function hideAlignLines() {
        interactionLayer.updateAlignLines({})
    }

    // ---------------------------------------------------------------------
    // 展示模式的点击消费（消费式拦截 —— 与 DanmakuView.consumeTipClick 同一模式）
    //
    // 【为什么卡身/关闭环的点击会穿透】QML 的鼠标命中只发生在"父对象的矩形
    // 范围内"（见 PlayerView.qml 里 timeEditCatcher 的注释）：关闭环经 -13 边距
    // 悬在卡身矩形之外，它的 MouseArea（40x40 热区）落在卡身外的那部分收不到
    // 事件 → 落到 activityArea 触发暂停/继续；卡身上没有 MouseArea 的区域
    // （外框、问题文字、行间距）同样穿透。web 端 DOM 命中不受父元素边界限制，
    // 所以参考实现里没有这层处理 —— 这是 QML 事件模型的固有差异。
    //
    // 修复：activityArea 在走 clickTimer 之前先问本层"要不要消费这一次点击"：
    //   * 命中关闭环热区 → 展示模式置 closed（执行关闭，与 CloseCircle 的
    //     onClicked → closeRequested → slot.closed = true 完全同一条路径）并
    //     消费；编辑模式不置 closed（web 编辑模式不注册 close 回调），同样消费
    //     防穿透；
    //   * 命中卡身矩形 → 纯消费（点卡片外框不该暂停播放，用户实测要求）；
    //   * 都没命中 → 返回 false（空白处单击暂停/双击全屏照旧）。
    //
    // 热区几何：可见圆环 16x16 悬在卡右上角（right/top -13 → 卡身坐标系
    // x ∈ [宽-3, 宽+13]、y ∈ [-13, 3]），MouseArea 热区再向四周外扩 12px
    // → x ∈ [宽-15, 宽+25]、y ∈ [-25, 15]。guide 卡没有关闭环，跳过。
    //
    // 遍历从最上层（Repeater 末位，绘制在最上面）往下 —— 卡片重叠时最上面
    // 那张优先命中，与视觉层级一致。
    // ---------------------------------------------------------------------

    function consumeDisplayClick(layerX, layerY) {
        for (var i = repeater.count - 1; i >= 0; i--) {
            var slotItem = repeater.itemAt(i)

            /* 不在显示中（时间窗外/已关闭且淡出完毕）不参与命中 */
            if (slotItem === null || !slotItem.visible)
                continue

            var px = layerX - slotItem.x
            var py = layerY - slotItem.y

            /* 关闭环热区（先判 —— 圆环画在卡身之上，语义上优先于卡身） */
            if (slotItem.kind !== "guide"
                    && px >= slotItem.width - 15 && px <= slotItem.width + 25
                    && py >= -25 && py <= 15) {
                /* 展示模式走统一关闭路径（置 closed + 数据标记 + 生成缩小条目）；
                   编辑模式不关闭（web 编辑模式不注册 close 回调），同样消费防穿透 */
                if (!interactionLayer.editMode)
                    slotItem.requestClose()
                return true
            }

            /* 卡身矩形：纯消费（不暂停、不关卡片） */
            if (px >= 0 && px <= slotItem.width
                    && py >= 0 && py <= slotItem.height)
                return true
        }
        return false
    }

    /*
     * 【纯查询版命中测试】光标悬停要不要变成小手（Qt.PointingHandCursor）。
     *
     * 与 consumeDisplayClick 的分工：
     *   * 关闭环热区在卡身矩形**外**，它自己的 MouseArea 收不到 hover（QML 只把
     *     事件投给父对象矩形范围内的子项）→ hover 落在 activityArea 上，由
     *     activityArea 的 onPositionChanged 调本函数、动态切换光标（用户要求：
     *     展示模式鼠标移到关闭按钮上指针变小手）；
     *   * 卡身**内**的可点区域（投票行/评分星星/三连/外链/guide 按钮）hover 正常，
     *     由它们各自 MouseArea 的 cursorShape 负责，不经本函数。
     *
     * 与 consumeDisplayClick 的差别：**无副作用** —— hover 绝不能把卡片关了，
     * 所以只测环热区、只在展示模式命中（编辑模式点环是吞掉不关闭，光标给箭头）。
     */
    function hitInteractiveAt(layerX, layerY) {
        if (interactionLayer.editMode)
            return false

        for (var i = repeater.count - 1; i >= 0; i--) {
            var slotItem = repeater.itemAt(i)

            if (slotItem === null || !slotItem.visible)
                continue

            if (slotItem.kind === "guide")
                continue

            var px = layerX - slotItem.x
            var py = layerY - slotItem.y

            if (px >= slotItem.width - 15 && px <= slotItem.width + 25
                    && py >= -25 && py <= 15)
                return true
        }

        return false
    }

    // ---------------------------------------------------------------------
    // 内容区（坐标系 = 整个播放区，不做内缩）
    //
    // 【对齐 web】参考 .nova-player-cmd-dm-inside 的 padding: 9.5952024% 只影响
    // 流式子元素；卡片和对齐线都是 absolute 定位，top/left 百分比相对的是
    // padding box（= 整个区域）。之前这里照字面做了四边内缩，导致卡片和四根
    // 范围限制线整体偏内（用户实测"线太靠里"）。现在 inside 直接铺满整层：
    // 卡片定位、拖拽边界、对齐线全部与 web 同一个基准。
    // ---------------------------------------------------------------------

    Item {
        id: inside

        anchors.fill: parent

        Repeater {
            id: repeater

            model: interactionLayer.masterModel

            /* ---------------------------------------------------------------
             * 卡片槽：时间窗状态机 + 定位 + 按类型装载卡片
             * --------------------------------------------------------------- */
            delegate: Item {
                id: slot

                /* 卡片数据与类型（modelData 是 {kind, data, listIndex}；
                   注意不能叫 item —— Loader 有内置 item 属性） */
                readonly property var cardEntry: modelData
                readonly property string kind: modelData.kind
                readonly property var cardData: modelData.data
                readonly property int listIndex: modelData.listIndex

                /* 持久化 key（编辑 API 重建 delegate 后状态按它找回） */
                readonly property string stateKey: interactionLayer.stateKeyOf(slot.kind, slot.listIndex)

                /* 时间窗（秒；缺省 0 —— 参考里 timeStart/timeEnd 可选） */
                readonly property real timeStart: (slot.cardData.timeStart !== undefined)
                                                  ? slot.cardData.timeStart : 0
                readonly property real timeEnd: (slot.cardData.timeEnd !== undefined)
                                                ? slot.cardData.timeEnd : 0

                /*
                 * 用户关闭标记（点击关闭环置位；seek 回窗口前复位 —— 照 closeEntry）。
                 * 关闭态同步写进数据对象（__isClose / __closeTime，照 web 的
                 * isClose/closeTime 字段）：缩小条目的生命周期据此判定，
                 * delegate 被 rebuildModel 重建时也据此恢复关闭态
                 * （绑定初始值，命令式赋值后打断 —— 重建后新 delegate 重新求值）。
                 */
                property bool closed: (slot.cardData !== undefined
                                       && slot.cardData.__isClose === true)

                /* 关闭时刻（秒，0 = 未关闭过） */
                readonly property real closeTimeSec: (slot.cardData !== undefined
                                                      && slot.cardData.__closeTime !== undefined
                                                      && slot.cardData.__closeTime !== null)
                                                     ? slot.cardData.__closeTime : 0

                /* seek 回关闭时刻之前：关闭态复位（照 web syncCard 的
                   closeTime > currentTime 分支 —— 复位后缩小条目随之移除） */
                readonly property bool rewindBeforeClose: slot.closeTimeSec > 0
                                                          && slot.now < slot.closeTimeSec

                onRewindBeforeCloseChanged: {
                    if (slot.rewindBeforeClose && slot.closed) {
                        slot.closed = false
                        slot.cardData.__isClose = false
                    }
                }

                /*
                 * 统一关闭路径（照 web closeEntry）：置 closed（卡片淡出）+
                 * 数据打标记（缩小条目的源依据）+ 展示模式下非 guide 卡片
                 * 生成左下角缩小条目（编辑模式不生成 —— 卡片常驻无关闭语义）。
                 */
                function requestClose() {
                    slot.closed = true
                    slot.cardData.__isClose = true
                    slot.cardData.__closeTime = interactionLayer.clockSec

                    if (!interactionLayer.editMode && slot.kind !== "guide")
                        interactionLayer.appendShrinkEntry(slot.kind, slot.listIndex)
                }

                /* 播放时钟（秒）—— 每次变化重算下面所有派生态 */
                readonly property real now: interactionLayer.clockSec

                /*
                 * 窗口外（提前 0.6s 判据的前沿）：直接隐藏 + 复位 closed。
                 * 【对齐 web】编辑/展示同一套时间窗（diffAlgorithm 不区分模式，
                 * 编辑模式只是"不注册点击回调 + 绑拖拽"）—— 之前这里写成
                 * "编辑模式全部常驻显示"，和 web 端行为不一致（用户实测报的差异）。
                 */
                readonly property bool beforeWindow: slot.now < slot.timeStart - 0.6

                /* 显示窗口内且未被关闭（编辑模式下关闭回调不注册 → closed 恒 false，
                   但窗口判据不变：没到时间点照样不显示，和 web 端一致） */
                readonly property bool inShow: !slot.closed && slot.now >= slot.timeStart
                                               && slot.now <= slot.timeEnd

                /* 关闭环剩余进度（1 → 0；窗口前恒 1 满环、窗口后恒 0 空环） */
                readonly property real remaining: slot.now <= slot.timeStart ? 1
                        : (slot.now >= slot.timeEnd ? 0
                           : 1 - (slot.now - slot.timeStart)
                             / Math.max(0.001, slot.timeEnd - slot.timeStart))

                /* 定位：top/left 百分比（相对内容区）+ 中心锚定（translate(-50%,-50%)） */
                x: (slot.cardData.left !== undefined ? slot.cardData.left : 50) / 100
                   * inside.width - width / 2
                y: (slot.cardData.top !== undefined ? slot.cardData.top : 50) / 100
                   * inside.height - height / 2

                /* 显隐协议（照 .nova-danmaku-x-show 的 opacity/visibility 过渡）：
                   显示 → 渐入 350ms；离开/关闭 → 渐出 350ms；
                   窗口外（seek 回退）→ Behavior 关掉、瞬切（参考"直接删"）；
                   编辑模式进出 → Behavior 关掉、瞬切（常驻卡片不"闪一下"） */
                opacity: slot.inShow ? 1 : 0
                visible: slot.inShow || slot.opacity > 0.001

                Behavior on opacity {
                    /* 【对齐 web】显隐过渡不区分编辑/展示（参考的 CSS transition 只有一套） */
                    enabled: !slot.beforeWindow
                    NumberAnimation {
                        duration: 350
                        easing.type: Easing.Linear
                    }
                }

                onBeforeWindowChanged: {
                    /* seek 回到窗口前：复位关闭标记（下次进窗重新显示）；
                       数据标记同步清（缩小条目的源判定据此失效） */
                    if (slot.beforeWindow) {
                        slot.closed = false
                        slot.cardData.__isClose = false
                    }
                }

                /* 尺寸跟随装载的卡片（Loader 不设尺寸 → 自动取内容 implicitSize） */
                width: cardLoader.width
                height: cardLoader.height

                Loader {
                    id: cardLoader

                    /* 按 kind 装载对应卡片（组件常驻由 Repeater 管，这里只挑类型） */
                    sourceComponent: slot.kind === "guide" ? guideComponent
                                   : slot.kind === "link" ? linkComponent
                                   : slot.kind === "vote" ? voteComponent
                                   : slot.kind === "scoreSum" ? scoreSumComponent
                                   : scoreComponent

                    /* 卡片本体的状态由组件内部自己管；时间窗/显隐相关的
                       属性在这里喂 */
                    onLoaded: {
                        item.cardData = Qt.binding(function () { return slot.cardData })
                        item.large = Qt.binding(function () { return interactionLayer.large })
                        if (slot.kind !== "guide") {
                            /* guide 没有关闭环和 remaining（参考里 guide 卡不带关闭钮） */
                            item.remaining = Qt.binding(function () { return slot.remaining })
                            /* 统一关闭路径（置 closed + 数据标记 + 展示模式生成缩小条目） */
                            item.closeRequested.connect(function () { slot.requestClose() })
                        }
                        if (slot.kind === "scoreSum") {
                            /* 聚合注入（照 web renderScoreSumEntry —— 条目创建时
                               一次性算好：全部评分卡题目 + 平均分 + 总参与人数） */
                            var summary = interactionLayer.aggregateScoreSum()
                            item.summaryItems = summary.items
                            item.totalCount = summary.count
                        } else {
                            /* 交互状态协议（编辑 API 重建 delegate 后状态找回；
                               scoreSum 无交互状态不参与） */
                            item.stateKey = slot.stateKey
                            item.stateMutated.connect(function (snapshot) {
                                interactionLayer.persistState(slot.stateKey, snapshot)
                            })
                            var snap = interactionLayer.stateFor(slot.stateKey)
                            if (snap !== undefined)
                                item.restoreState(snap)
                        }
                    }
                }

                /* ---- 编辑模式拖拽（照 dragEditor.ts；仅 editMode 启用）----
                 * 放在 Loader 外层 z 之上：编辑模式下拖拽优先（编辑的是布局，
                 * 投票/评分/跳转这类观看交互让位 —— 与参考 mousedown 独占一致）。 */
                MouseArea {
                    id: dragArea

                    anchors.fill: parent
                    /* 【必须 visible，不能只 enabled】disabled 的 MouseArea
                       虽不收事件，但 cursorShape 仍然生效（光标只看 visible）——
                       展示模式下卡片上会一直挂着"拖动"指针（用户实测）。visible:false
                       连光标一起撤掉，事件零干扰：展示模式下评分/投票/关闭环正常响应；
                       编辑模式再整体回来（此时拖拽独占，与参考 mousedown 独占一致）。 */
                    visible: interactionLayer.editMode
                    enabled: interactionLayer.editMode
                    /* grab / grabbing（参考里拖拽时的 cursor 提示） */
                    cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor

                    /* 夹取边界（px，相对 inside；按下时按卡片尺寸现算）：
                       中心锚定下"视觉左缘 ≥ 3%W" ⇔ "x ≥ 0.03W"，直接用安全边界算 */
                    property real minX: 0
                    property real minY: 0
                    property real maxX: 0
                    property real maxY: 0

                    /* 参考 SAFE_MARGIN：左 3 / 右 3 / 上 5 / 下 15（%）*/
                    readonly property real marginX: inside.width * 0.03
                    readonly property real marginTop: inside.height * 0.05
                    readonly property real marginBottom: inside.height * 0.15

                    drag.target: slot
                    drag.axis: Drag.XAndYAxis
                    drag.minimumX: dragArea.minX
                    drag.maximumX: dragArea.maxX
                    drag.minimumY: dragArea.minY
                    drag.maximumY: dragArea.maxY

                    onPressed: function (mouse) {
                        dragArea.minX = dragArea.marginX
                        dragArea.maxX = inside.width - dragArea.marginX - slot.width
                        dragArea.minY = dragArea.marginTop
                        dragArea.maxY = inside.height - dragArea.marginBottom - slot.height
                        /* 拖拽时提升层级（照参考 zIndex 9999） */
                        slot.z = 9999
                    }

                    onPositionChanged: {
                        /* 贴边判定（照 dragEditor 的 onMouseMove）：到边界即显示对应线 */
                        var bounds = {}
                        bounds.left = slot.x <= dragArea.minX + 0.5
                        bounds.right = slot.x >= dragArea.maxX - 0.5
                        bounds.top = slot.y <= dragArea.minY + 0.5
                        bounds.bottom = slot.y >= dragArea.maxY - 0.5
                        interactionLayer.updateAlignLines(bounds)
                    }

                    onReleased: {
                        slot.z = 0
                        interactionLayer.hideAlignLines()

                        /* 换算百分比回写数据（中心锚定的逆运算），并通知外面（面板显示） */
                        var newLeft = Math.round((slot.x + slot.width / 2)
                                                 / inside.width * 10000) / 100
                        var newTop = Math.round((slot.y + slot.height / 2)
                                                / inside.height * 10000) / 100
                        slot.cardData.left = newLeft
                        slot.cardData.top = newTop

                        /* 恢复 x/y 的绑定（拖拽是命令式写值，已把绑定打断）；
                           绑定立即求值一次 —— 读的是刚写回的新百分比，位置不动 */
                        slot.x = Qt.binding(function () {
                            return (slot.cardData.left !== undefined ? slot.cardData.left : 50)
                                   / 100 * inside.width - slot.width / 2
                        })
                        slot.y = Qt.binding(function () {
                            return (slot.cardData.top !== undefined ? slot.cardData.top : 50)
                                   / 100 * inside.height - slot.height / 2
                        })

                        interactionLayer.cardMoved(slot.kind, slot.listIndex, newTop, newLeft)
                    }
                }

                /* ---- 五类卡片的 Component（sourceComponent 按类型挑一个）---- */
                Component {
                    id: guideComponent
                    GuideCard {}
                }

                Component {
                    id: linkComponent
                    LinkCard {}
                }

                Component {
                    id: voteComponent
                    VoteCard {}
                }

                Component {
                    id: scoreComponent
                    ScoreCard {}
                }

                Component {
                    id: scoreSumComponent
                    ScoreSumCard {}
                }
            }
        }

        /* -------------------------------------------------------------------
         * 左下角缩小条目（照 web ShrinkBar / shrinkActive 的 For 流）：
         * 展示模式下卡片被手动关闭时由 appendShrinkEntry 生成；进度环随播放
         * 继续倒数（shrinkRemainingOf 每拍从源数据时间窗现算）；点击重开。
         * 位置 left:23px / bottom:71px（照 scss 常量，相对整个播放区）。
         * ------------------------------------------------------------------- */
        Repeater {
            id: shrinkRepeater

            model: interactionLayer.shrinkModel

            delegate: ShrinkBar {
                x: 23
                anchors.bottom: parent.bottom
                /* 普通模式 71px（照 scss 常量）；全屏抬到 96px —— 全屏控制栏
                   高 73px（PlayerControlBar 的 fullscreen 档，照参考
                   index.scss:4969 的 height:73）会盖住 71px 档，
                   96 = 73 + 23px 间距（与普通档的间距构成一致），
                   web 端 scss 的 [data-screen="full"]/["web"] 段同款 */
                anchors.bottomMargin: interactionLayer.fullscreen ? 96 : 71
                title: modelData.title
                iconSource: modelData.icon
                /* 显式引用 interactionLayer.clockSec（表达式直接依赖，
                   每拍必重算 —— 进度环绝不失步；nowSec 参数见函数注释） */
                remaining: interactionLayer.shrinkRemainingOf(modelData,
                                                              interactionLayer.clockSec)
                onClicked: interactionLayer.reopenFromShrink(modelData)
            }
        }

        /* 淡出收尾定时器（照 web scheduleShrinkRemoval 的 CARD_FADE_MS）：
           beginClose 播 0.35s 淡出 → 到点 commitShrinkRemoval 真正删条目 */
        Timer {
            id: shrinkFadeTimer

            interval: 350
            repeat: false
            onTriggered: interactionLayer.commitShrinkRemoval()
        }

        /* ---- 贴边辅助线（z 100，照参考；拖拽贴边时才显示） ---- */
        Item {
            id: alignLines

            anchors.fill: parent
            z: 100
            /* 线位置 = 视觉安全边界（与卡片尺寸无关）：左 3% / 右 97% / 上 5% / 下 85% */

            Rectangle {
                /* 左竖线 */
                width: 1
                height: parent.height
                x: inside.width * 0.03 - 0.5
                color: "#858688"
                opacity: interactionLayer.alignShow.left ? 1 : 0

                Behavior on opacity {
                    NumberAnimation { duration: 200; easing.type: Easing.Linear }
                }
            }

            Rectangle {
                /* 右竖线 */
                width: 1
                height: parent.height
                x: inside.width * 0.97 - 0.5
                color: "#858688"
                opacity: interactionLayer.alignShow.right ? 1 : 0

                Behavior on opacity {
                    NumberAnimation { duration: 200; easing.type: Easing.Linear }
                }
            }

            Rectangle {
                /* 上横线 */
                width: parent.width
                height: 1
                y: inside.height * 0.05 - 0.5
                color: "#858688"
                opacity: interactionLayer.alignShow.top ? 1 : 0

                Behavior on opacity {
                    NumberAnimation { duration: 200; easing.type: Easing.Linear }
                }
            }

            Rectangle {
                /* 下横线（底部预留控制栏） */
                width: parent.width
                height: 1
                y: inside.height * 0.85 - 0.5
                color: "#858688"
                opacity: interactionLayer.alignShow.bottom ? 1 : 0

                Behavior on opacity {
                    NumberAnimation { duration: 200; easing.type: Easing.Linear }
                }
            }
        }
    }
}
