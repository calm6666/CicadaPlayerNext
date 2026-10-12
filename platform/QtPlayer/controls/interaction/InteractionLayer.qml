// ===========================================================================
// 交互卡片层 —— 四类卡片（点赞关注 / 外链 / 投票 / 评分）的时间窗驱动与装配
// （照 hili-player packages/plugins/src/interaction/index.ts 复刻：展示 + 编辑两种模式）
//
// 数据流（和 viewPoints / pbpData / previewFrames 同一条路）：
//   Main.qml fetchProgressMetaFrom() → GET /x/player/interaction →
//   playerView.interactionData → 本层 → 四类卡片
//   数据结构照 hili-player 的 InteractionCard：
//     { guideList: [{type, top, left, timeStart, timeEnd}],
//       linkList:  [{top, left, linkContent, timeStart, timeEnd}],
//       voteList:  [{top, left, question, options, timeStart, timeEnd}],
//       scoreList: [{top, left, title, scoreType, timeStart, timeEnd}] }
//   top/left 是百分比（相对整个播放区 —— 见下面内容区处的坐标系说明），
//   timeStart/timeEnd 单位秒。
//   没有数据 / 接口不通 = 一张卡片都不显示（静默降级，界面不变）。
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
//   * 编辑 API（照 index.ts 的公共 API，语义一致）：
//       addCard(kind, data) / removeCard(kind, i) / updateCardContent(kind, i, patch)
//       / closeCard(kind, i) / getStatus() / updateData(data)
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
       播放区够宽就放大、拉窄就缩回；不再叠加全屏条件。四类卡片都按 large
       属性切换两套尺寸（interaction.scss L774-879 的大档数值）。 */
    readonly property bool large: interactionLayer.width > 1366.9

    /* 展开成带类型标记的一维模型（顺序照参考创建顺序：guide → link → vote → score，
       后声明的压在上面 —— Repeater 同 z 按序绘制，与 DOM 顺序一致） */
    property var masterModel: []

    onInteractionDataChanged: interactionLayer.rebuildModel()

    function rebuildModel() {
        var arr = []
        var d = interactionLayer.interactionData

        if (d !== null && d !== undefined) {
            var lists = [["guide", d.guideList], ["link", d.linkList],
                         ["vote", d.voteList], ["score", d.scoreList]]

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
    // 编辑 API（照 index.ts 的公共 API；kind = "guide" / "link" / "vote" / "score"）
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
            scoreList: d.scoreList
        }
        cardsChanged()
    }

    /* 添加卡片（照 addGuide/addLink/addVote/addScore：入列表 + 立即可见） */
    function addCard(kind, cardData) {
        var d = interactionLayer.interactionData

        if (d === null || d === undefined)
            return

        if (kind === "guide")
            d.guideList.push(cardData)
        else if (kind === "link")
            d.linkList.push(cardData)
        else if (kind === "vote")
            d.voteList.push(cardData)
        else if (kind === "score")
            d.scoreList.push(cardData)
        else
            return

        interactionLayer.notifyDataChanged()
    }

    /* 移除卡片（照 removeCard：数据从列表删除，跨快照不再出现） */
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

        if (list === null || index < 0 || index >= list.length)
            return

        /* 下标位移后，后面卡片的状态 key 全部对不上号 —— 参考也是"重建全部条目"。
           同步把持久化状态按新下标搬移（参考 Map 按 listIndex 存、splice 后整体刷新）。 */
        list.splice(index, 1)
        interactionLayer.shiftPersistStates(kind, index)
        interactionLayer.notifyDataChanged()
    }

    /*
     * 编辑卡片内容（照 updateCardContent 的补丁语义：未提供的字段保持原值）。
     * 可用字段（CardContentPatch）：guideType / linkContent / question /
     * optionTexts / title / scoreType / timeStart / timeEnd / top / left。
     */
    function updateCardContent(kind, index, patch) {
        var d = interactionLayer.interactionData

        if (d === null || d === undefined || patch === null || patch === undefined)
            return

        var item = null

        if (kind === "guide")
            item = d.guideList[index]
        else if (kind === "link")
            item = d.linkList[index]
        else if (kind === "vote")
            item = d.voteList[index]
        else if (kind === "score")
            item = d.scoreList[index]

        if (item === undefined || item === null)
            return

        /* 时间窗 / 定位（四类通用） */
        if (patch.timeStart !== undefined)
            item.timeStart = patch.timeStart
        if (patch.timeEnd !== undefined)
            item.timeEnd = patch.timeEnd
        if (patch.top !== undefined)
            item.top = patch.top
        if (patch.left !== undefined)
            item.left = patch.left

        /* 各类型自己的内容字段 */
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
        }

        interactionLayer.notifyDataChanged()
    }

    /* 关闭卡片（照 closeCard：本次播放关闭 —— 展示模式淡出；guide 无关闭概念） */
    function closeCard(kind, index) {
        if (kind === "guide")
            return

        for (var i = 0; i < repeater.count; i++) {
            var row = repeater.itemAt(i)

            if (row !== null && row.kind === kind && row.listIndex === index) {
                row.closed = true
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

    /* 更新数据（照 updateData：替换显式传入的列表，交互状态清零重建） */
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

        interactionLayer.persistStates = ({})
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
                if (!interactionLayer.editMode)
                    slotItem.closed = true
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

                /* 用户关闭标记（点击关闭环置位；seek 回窗口前复位 —— 照 closeEntry） */
                property bool closed: false

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
                    /* seek 回到窗口前：复位关闭标记（下次进窗重新显示） */
                    if (slot.beforeWindow)
                        slot.closed = false
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
                                   : scoreComponent

                    /* 卡片本体的状态由组件内部自己管；时间窗/显隐相关的
                       三个属性在这里喂 */
                    onLoaded: {
                        item.cardData = Qt.binding(function () { return slot.cardData })
                        item.large = Qt.binding(function () { return interactionLayer.large })
                        if (slot.kind !== "guide") {
                            /* guide 没有关闭环和 remaining（参考里 guide 卡不带关闭钮） */
                            item.remaining = Qt.binding(function () { return slot.remaining })
                            item.closeRequested.connect(function () { slot.closed = true })
                        }
                        /* 交互状态协议（编辑 API 重建 delegate 后状态找回） */
                        item.stateKey = slot.stateKey
                        item.stateMutated.connect(function (snapshot) {
                            interactionLayer.persistState(slot.stateKey, snapshot)
                        })
                        var snap = interactionLayer.stateFor(slot.stateKey)
                        if (snap !== undefined)
                            item.restoreState(snap)
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

                /* ---- 四类卡片的 Component（sourceComponent 按类型挑一个）---- */
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
            }
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
