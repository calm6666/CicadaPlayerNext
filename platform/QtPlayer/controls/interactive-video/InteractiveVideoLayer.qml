// ===========================================================================
// 互动视频层（分支剧情视频）—— 照 hili-player 的 interactive-video 插件
// （packages/plugins/src/interactive-video）完整复刻，功能对准 web 端：
//
//   * 剧情进度条（storyBar）：底部居中半透明胶囊条，走过的节点圆点
//     双态图标（正在看 = 位置水滴 / 已看完 = 打勾，悬停换蓝底），
//     悬停弹出标题 + 封面卡片（双层三角箭头）；
//   * 分支选择（questionCanvas → QML 声明式组件）：时间窗内全屏居中的
//     选项按钮组（300×52 背景图 + 遮罩 + 描边 + 文字），悬停小手光标 +
//     围绕中心平滑放大 1.07 倍 / 移出平滑缩回（web 是 canvas rAF 指数
//     逼近，QML 用 scale + Behavior 等价实现）；
//   * 倒计时进度条（countdown）：贴底 4px 白条，宽度按剩余比例收缩；
//   * 重播按钮（replay）：路径走完后右上角显示，点击回到起始节点；
//   * 播完场景（ENDED）：视频播完弹出选择时，控制栏与其它 UI 收起
//     （发 hideControlsRequested 信号由 PlayerView 收控制栏），问题画布
//     以**视频最后一帧**为背景（player.requestSnapshot() 异步截屏，
//     image://snapshot/<rev> 取图，cover 等比裁剪铺满 + 45% 黑遮罩，
//     对齐 web 端 canvas 绘制 video 末帧的做法），问题 / 重播不随
//     控制栏隐藏、保持可点击。
//
// 数据流（和 viewPoints / interactionData 同一条路）：
//   Main.qml fetchProgressMetaFrom() → GET /interactive/data.json →
//   （相对 URL 已在 Main.qml 用 mock 基址解析成绝对地址）→
//   playerView.interactiveVideoData → 本层
//   数据结构照 web 端 InteractiveVideoPluginConfig：
//     { startNodeId, autoPlay,
//       nodes: { id: { title, cover, video, duration,
//                      question: { timeStart, timeEnd, title, background,
//                                  defaultOptionIndex,
//                                  options: [{label, text, targetNodeId}] } } } }
//   没有数据 / 接口不通 = 整层静默隐藏（界面不变）。
//
// 与 web 端插件的语义逐条对齐：
//   * 路径追踪（pathTracker.ts）：path = [{nodeId, selectedOptionIndex}]
//     （-1 = 未选）；jumpTo 截断 / 追加、selectOption 写末项、reset 回起点；
//   * 时间窗状态机（index.ts handleTimeUpdate）：
//     clockSec = player.position / 1000（毫秒→秒，position 每拍驱动；
//     暂停时 position 不变 → 状态冻结，seek 跳变 → 状态即时重算）
//     - clockSec >= timeEnd 且未选 → 自动选默认项（defaultOptionIndex ?? 0）
//     - timeStart <= clockSec < timeEnd 且未选 → 显示选择 + 倒计时
//       ratio = (timeEnd - now) / (timeEnd - timeStart)（宽度过渡抹平阶梯感）
//     - clockSec < timeStart（seek 回退跳过分支点）→ 隐藏选择 + 停倒计时
//   * ENDED（handleEnded）：storyVisible 收起；有未选分支 → 弹出选择
//     （ended 模式：末帧背景、无倒计时）；无分支 → 显示重播；
//   * 切源（switchToNode）：CicadaPlayerItem.setSource 会销毁重建播放器，
//     autoPlay 自动起播 —— 同一地址重播时 setSource 短路，改为 seek(0)+play()。
//
// 显隐协议（照 web 的 data-ctrl-hidden + :has(active) 联动例外）：
//   整层 opacity 随控制栏显隐（controlsShown 由 PlayerView 喂
//   controlBar.shown）淡入淡出 300ms；问题显示中（时间窗 / 播完两种
//   模式）或重播显示中 = 激活例外，整层保持全亮可交互（对应 web 的
//   .nova-player-interaction-active 标志 + :has 规则）。
//
// 【根 id 不能叫 layer】Item 有内建属性 layer（QQuickItemLayer），见
// InteractionLayer.qml 顶部的事故注释 —— 这里叫 interactiveVideoLayer。
// ===========================================================================
import QtQuick
import QtQuick.Effects
import QtPlayer

Item {
    id: interactiveVideoLayer

    // ---------------------------------------------------------------------
    // 对外接口
    // ---------------------------------------------------------------------

    /* 播放器（读 position 毫秒 / status / source；调 seek/play/requestSnapshot） */
    property var player: null

    /* 互动视频数据（InteractiveVideoPluginConfig 结构，null = 无数据整层隐藏；
       相对 URL 已在 Main.qml 解析成绝对地址） */
    property var interactiveData: null

    /*
     * 视频源解析器（经 PlayerView 注入）：
     * function(node, onDone)，onDone(source) —— source 为播放地址字符串或
     * SegmentBase 清单对象，解析失败回调 null。
     * 节点带 avid 且未直接给 video 时（mock 剧情树场景），本层按 avid 调它
     * 换取真实播放源（对齐 web 端插件 config.resolveVideo 的角色）；
     * null = 旧 data.json 链路（节点必须自带 video 字段）。
     */
    property var videoResolver: null

    /* 是否全屏（剧情进度条上移到 98px，照 web 的 [data-screen="full"] 档） */
    property bool fullscreen: false

    /* 控制栏当前是否显示（PlayerView 喂 controlBar.shown）—— 驱动整层联动显隐 */
    property bool controlsShown: true

    /*
     * 请求切换片源（PlayerView 接到后分流：字符串地址写 playerView.source
     * 销毁重建自动起播；清单对象走 player.setManifest —— 对齐 web 端
     * "URL 注入 / 清单对象注入"两种模式）。name 为节点标题（对象源时
     * 作为 manifestHint 显示用）。
     */
    signal sourceSwitchRequested(var source, string name)

    /*
     * 请求收起控制栏（播完弹出问题 / 显示重播时：控制栏与其它 UI 隐藏，
     * 只留问题 / 重播可见可点，对齐 web 端播完场景）
     */
    signal hideControlsRequested()

    // ---------------------------------------------------------------------
    // 会话状态（全部是简单属性 + 绑定，变化驱动下方整棵 UI）
    // ---------------------------------------------------------------------

    /*
     * 观看路径（pathTracker 的 path）：[{nodeId, selectedOptionIndex}]
     * 【QML 通知规则】嵌套对象 / 数组元素的字段赋值不发通知 —— 所有变更
     * 都整体替换数组引用（照 InteractionLayer.persistStates 的做法）。
     */
    property var path: []

    /* 剧情进度条显隐（ENDED 时收起，切节点 / 重播时恢复） */
    property bool storyVisible: false

    /* 分支选择是否显示（时间窗进入 / 播完弹出两种来源） */
    property bool questionActive: false

    /* 播完弹出模式（true = 末帧定格背景；false = 时间窗模式全透明） */
    property bool questionEnded: false

    /* 倒计时剩余比例（1 → 0，clockSec 每拍驱动） */
    property real countdownRatio: 1

    /* 倒计时显隐 */
    property bool countdownVisible: false

    /* 重播按钮显隐 */
    property bool replayVisible: false

    /*
     * 播完定格帧的快照版本号（0 = 还没拿到）：
     * handleEnded 时 requestSnapshot() 异步截屏，onSnapshotRevisionChanged
     * 回来后记录版本号 → 末帧背景 Image 用 image://snapshot/<rev> 取图。
     * 不直接绑 player.snapshotRevision：那可能还是进度条悬停时的旧帧。
     */
    property int endFrameRevision: 0

    /*
     * 选项按钮横向流式布局（对齐 web 端 questionCanvas.layoutOptionRects）：
     * 一行放不下自动换到下一行，每行按该行实际按钮数水平居中，
     * 题目（36 高）+ 按钮组（300×52、间距 20 行内 / 行间一致）整体垂直居中。
     * 绑定求值每次生成新对象引用，currentQuestion 变化（path /
     * interactiveData 整体替换驱动）与层尺寸变化（窗口缩放 / 全屏切换）
     * 都会重算 —— 等价 web 端 effect 重绘 + ResizeObserver 重排。
     */
    readonly property var optionLayout: {
        var q = interactiveVideoLayer.currentQuestion
        var opts = (q !== undefined && q !== null && q.options !== undefined)
                   ? q.options : []
        var n = opts.length
        var rects = []
        var titleY = 0
        var titleVisible = false
        if (n > 0) {
            var buttonWidth = 300
            var buttonHeight = 52
            var gap = 20
            var hasTitle = q.title !== undefined && q.title !== ""
            var titleHeight = hasTitle ? 36 : 0
            titleVisible = hasTitle

            var W = interactiveVideoLayer.width
            var H = interactiveVideoLayer.height

            /* 每行可容纳的按钮数（层过窄时至少 1 个：单按钮独占一行） */
            var cols = Math.max(1, Math.floor((W + gap) / (buttonWidth + gap)))
            var rows = Math.ceil(n / cols)

            var totalH = rows * buttonHeight + (rows - 1) * gap + titleHeight
            var startY = Math.max(0, (H - totalH) / 2)
            titleY = startY

            for (var i = 0; i < n; i++) {
                var row = Math.floor(i / cols)
                var col = i % cols
                /* 该行实际按钮数（末行可能不满 cols 个） */
                var rowCount = Math.min(cols, n - row * cols)
                var rowW = rowCount * buttonWidth + (rowCount - 1) * gap
                rects.push({
                    x: (W - rowW) / 2 + col * (buttonWidth + gap),
                    y: startY + titleHeight + row * (buttonHeight + gap)
                })
            }
        }
        return { rects: rects, titleY: titleY, titleVisible: titleVisible }
    }

    /* 播放时钟（秒）：position 每次变化都重算下方整棵绑定树 */
    readonly property real clockSec: interactiveVideoLayer.player !== null
                                     ? interactiveVideoLayer.player.position / 1000 : 0

    /* 节点表（id → 节点配置） */
    readonly property var storyNodes: (interactiveData !== null
                                       && interactiveData !== undefined
                                       && interactiveData.nodes !== undefined)
                                      ? interactiveData.nodes : null

    /* 当前所在节点 id（路径末项；路径为空回落起始节点） */
    readonly property string currentNodeId: {
        if (path.length > 0)
            return path[path.length - 1].nodeId
        if (interactiveData !== null && interactiveData !== undefined
                && interactiveData.startNodeId !== undefined)
            return interactiveData.startNodeId
        return ""
    }

    /* 当前节点配置 */
    readonly property var currentNode: (storyNodes !== null
                                        && storyNodes[currentNodeId] !== undefined)
                                       ? storyNodes[currentNodeId] : undefined

    /* 当前节点的分支配置（无分支 = undefined） */
    readonly property var currentQuestion: (currentNode !== undefined
                                            && currentNode.question !== undefined)
                                           ? currentNode.question : undefined

    /* 当前节点是否已选择过分支（路径末项 selectedOptionIndex >= 0） */
    readonly property bool hasSelected: path.length > 0
                                        && path[path.length - 1].selectedOptionIndex >= 0

    /*
     * 剧情进度条条目（rebuildStoryEntries 整组重建）：
     * { index: 视觉下标（悬停卡片定位用）, title, cover, isCurrent }
     */
    property var storyEntries: []

    /*
     * avid → 已解析播放源的缓存（对齐 web 端 videoSourceCache）：
     * 同一节点回看 / 重播直接命中，不重复请求 playurl。
     * 会话结束（interactiveData 置空）/ 新会话到达时整体清空。
     */
    property var sourceCache: ({})

    /*
     * 正在异步解析视频源的节点 id（对齐 web 端 pendingVideoNodeId 过期守卫）：
     * 进入解析前标记，回调到达先校验 —— 期间用户又选了新分支的话，
     * 过期请求的结果直接丢弃，不覆盖新分支。
     */
    property string pendingNodeId: ""

    /*
     * 当前已加载源的记账 key（nodeKey：avid 优先，无 avid 用 "url:地址"）：
     * 同 key 再应用 = 同源重播，走 seek(0)+play()（对象源没有地址可比，
     * 清单对象的 source 在内核里是空的 —— 见 PlayerView L338 的注释，
     * 所以对象源的同源判断只能靠这份记账）。
     */
    property string loadedSourceKey: ""

    // ---------------------------------------------------------------------
    // 数据到达：初始化会话（对齐 web initAfterMounted 的初始渲染 + 起播）
    // ---------------------------------------------------------------------

    onInteractiveDataChanged: interactiveVideoLayer.initFromData()

    function initFromData() {
        /* 无数据（换片 resetProgressMeta 置空 / 接口不通）：整层清场 */
        if (storyNodes === null || interactiveData.startNodeId === undefined) {
            path = []
            storyEntries = []
            storyVisible = false
            questionActive = false
            questionEnded = false
            countdownVisible = false
            replayVisible = false
            endFrameRevision = 0
            /* 会话级状态一并清空（在飞解析结果作废 + 缓存丢弃 + 记账复位） */
            pendingNodeId = ""
            sourceCache = ({})
            loadedSourceKey = ""
            return
        }

        /* 初始路径：仅起始节点，未选任何选项（照 PathTracker 构造） */
        path = [{ nodeId: interactiveData.startNodeId, selectedOptionIndex: -1 }]
        storyVisible = true
        questionActive = false
        questionEnded = false
        countdownVisible = false
        replayVisible = false
        endFrameRevision = 0
        /* 新会话 = 新缓存（对齐 web 端"插件实例级缓存，随实例销毁"的口径） */
        sourceCache = ({})
        pendingNodeId = ""
        loadedSourceKey = ""
        rebuildStoryEntries()

        /*
         * 起始节点起播：
         *   * 旧 data.json 链路：当前源不是起始节点视频时切过去（是的话
         *     applyNodeSource 的同源短路会走 seek(0)+play()，不重开）；
         *   * 华强买瓜会话（skipStartApply=true）：起始源已由 Main.qml 的
         *     playInteractiveVideo 主动起播（URL 走 playFile / 清单对象走
         *     playManifestObject），这里只记账不重切 —— 对齐 web 端
         *     "业务已配置 src 时插件不覆盖起播内容"的分支。
         */
        var node = storyNodes[interactiveData.startNodeId]
        if (node === undefined || player === null)
            return
        if (interactiveData.skipStartApply === true)
            loadedSourceKey = nodeKey(interactiveData.startNodeId)
        else
            applyNodeSource(interactiveData.startNodeId)
    }

    /*
     * 节点的源记账 key：avid 优先（华强买瓜节点都带），无 avid 的旧链路
     * 用 "url:地址"，再兜底 "node:节点id"。空 = 拿不到 key。
     */
    function nodeKey(nodeId) {
        var node = storyNodes !== null ? storyNodes[nodeId] : undefined
        if (node === undefined)
            return ""
        if (node.avid !== undefined && ("" + node.avid) !== "")
            return "" + node.avid
        if (node.video !== undefined && node.video !== null
                && node.video.toString() !== "")
            return "url:" + node.video.toString()
        return "node:" + nodeId
    }

    /*
     * 应用节点片源（对齐 web 端 loadNodeVideo 的三级顺序）：
     *   A. 节点自带 video（旧 data.json 链路 / 起始节点动态补全）→ 同步应用；
     *   B. 节点带 avid 且有解析器 → 查缓存，未命中异步请求 playurl
     *      （协议 / 格式由解析器闭包携带），解析期间新切换请求到达则
     *      过期丢弃（pendingNodeId 守卫）；
     *   C. 都没有 → 警告并放弃（与 web 端 console.warn 同口径）。
     */
    function applyNodeSource(nodeId) {
        if (player === null || storyNodes === null)
            return

        var node = storyNodes[nodeId]
        if (node === undefined)
            return

        /* A. 现成源：直接应用（起始节点动态补全 / 旧链路节点） */
        if (node.video !== undefined && node.video !== null
                && node.video.toString() !== "") {
            applyResolvedSource(node.video, nodeId)
            return
        }

        /* B. avid 异步解析 */
        var resolver = videoResolver
        var avid = node.avid !== undefined ? ("" + node.avid) : ""

        if (avid === "" || resolver === null) {
            console.warn("[interactiveVideo] 节点 " + nodeId
                         + " 缺少视频源：需要 video 或（avid + 解析器）")
            return
        }

        /* 缓存命中：回看 / 重播不重复请求（对齐 web 端 videoSourceCache） */
        if (Object.prototype.hasOwnProperty.call(sourceCache, avid)) {
            applyResolvedSource(sourceCache[avid], nodeId)
            return
        }

        /* 异步解析：进入即标记最新请求（必须先于任何回调），回调先校验代次 */
        pendingNodeId = nodeId
        resolver(node, function (source) {
            if (pendingNodeId !== nodeId || player === null)
                return
            pendingNodeId = ""
            if (source === null || source === undefined) {
                console.warn("[interactiveVideo] 节点 " + nodeId + "（avid="
                             + avid + "）playurl 解析失败")
                return
            }
            sourceCache[avid] = source
            applyResolvedSource(source, nodeId)
        })
    }

    /*
     * 应用已解析的播放源（字符串地址或清单对象）：
     *   * 字符串地址与播放器当前地址一致 = 同源重播（原 applyNodeSource
     *     的比较口径：setSource 同地址会短路且不重播，必须显式 seek(0)+play()）；
     *   * 记账 key 与当前一致（对象源 / 已缓存的地址源）= 同源重播同上；
     *   * 不同源 → 发信号由 PlayerView 分流换源（字符串写 source /
     *     对象走 setManifest），并更新记账。
     */
    function applyResolvedSource(source, nodeId) {
        var key = nodeKey(nodeId)

        /* 字符串地址与当前一致：同源重播（原语义保留） */
        if (typeof source === "string" && player !== null
                && source === player.source.toString()) {
            player.seek(0)
            player.play()
            if (key !== "")
                loadedSourceKey = key
            return
        }

        /* 记账一致（对象源重播 / 缓存命中的地址源）：seek(0)+play() */
        if (key !== "" && key === loadedSourceKey && player !== null) {
            player.seek(0)
            player.play()
            return
        }

        var node = storyNodes !== null ? storyNodes[nodeId] : undefined
        var name = (node !== undefined && node.title !== undefined)
                ? ("" + node.title) : ""
        sourceSwitchRequested(source, name)
        if (key !== "")
            loadedSourceKey = key
    }

    // ---------------------------------------------------------------------
    // 剧情进度条条目重建（照 rebuildStoryEntries：过滤无配置节点 + 重排下标）
    // ---------------------------------------------------------------------

    function rebuildStoryEntries() {
        var entries = []

        if (path.length > 0 && storyNodes !== null) {
            for (var i = 0; i < path.length; i++) {
                var node = storyNodes[path[i].nodeId]
                if (node === undefined)
                    continue

                entries.push({
                    /* 视觉下标（过滤后连续，悬停卡片 left 定位用） */
                    index: entries.length,
                    title: node.title !== undefined ? node.title : "",
                    cover: node.cover !== undefined ? node.cover : "",
                    /* 最后一项 = 正在观看（位置图标），其余为已看完（打勾） */
                    isCurrent: i === path.length - 1
                })
            }
        }

        storyEntries = entries
    }

    // ---------------------------------------------------------------------
    // 时间窗状态机（照 index.ts handleTimeUpdate + handleSeeked 合并：
    // clockSec 跳变天然覆盖 seek 场景 —— 回退到窗口前即隐藏）
    // ---------------------------------------------------------------------

    onClockSecChanged: interactiveVideoLayer.updateQuestionByClock()

    function updateQuestionByClock() {
        if (storyNodes === null || questionEnded)
            return

        var q = currentQuestion
        /* 当前节点无分支或已选择：不处理（保留隐藏态） */
        if (q === undefined || q === null || hasSelected)
            return

        /* 播放越过窗口终点仍未选择：自动选默认项（照 web handleTimeUpdate） */
        if (clockSec >= q.timeEnd) {
            handleSelect(q.defaultOptionIndex !== undefined ? q.defaultOptionIndex : 0)
            return
        }

        /* 窗口内：显示选择 + 驱动倒计时比例 */
        if (clockSec >= q.timeStart) {
            questionActive = true
            countdownVisible = true
            countdownRatio = Math.max(0, (q.timeEnd - clockSec)
                                             / Math.max(0.001, q.timeEnd - q.timeStart))
            return
        }

        /* 窗口前（seek 回退跳过分支点）：隐藏选择 + 停止倒计时 */
        if (questionActive || countdownVisible) {
            questionActive = false
            countdownVisible = false
        }
    }

    // ---------------------------------------------------------------------
    // 播完处理（照 index.ts handleEnded）
    // ---------------------------------------------------------------------

    Connections {
        target: interactiveVideoLayer.player

        function onStatusChanged(status) {
            if (status === CicadaPlayerItem.Status.End)
                interactiveVideoLayer.handleEnded()
        }

        /*
         * 播完定格帧到达：handleEnded 发起的 requestSnapshot 是异步的，
         * 版本号回来时才把 endFrameRevision 顶上去（末帧背景 Image 随之
         * 换图）。只在播完弹出模式下接收，避免把进度条悬停的旧帧记进来。
         */
        function onSnapshotRevisionChanged() {
            if (interactiveVideoLayer.questionEnded && interactiveVideoLayer.player !== null)
                interactiveVideoLayer.endFrameRevision = interactiveVideoLayer.player.snapshotRevision
        }
    }

    function handleEnded() {
        if (storyNodes === null)
            return

        /* 播完场景：剧情进度条与其它 UI 随控制栏一起收起，只留问题 / 重播 */
        storyVisible = false
        countdownVisible = false

        var q = currentQuestion

        if (q !== undefined && q !== null && !hasSelected) {
            /* 播完弹选择：无倒计时（时间轴已停）；ended 模式画末帧背景 */
            questionActive = true
            questionEnded = true
            /* 异步抓当前帧（= 最后一帧）作定格背景；拿不到时遮罩兜底变暗 */
            if (player !== null)
                player.requestSnapshot()
            hideControlsRequested()
            return
        }

        /* 路径走完：显示重播按钮 */
        replayVisible = true
        hideControlsRequested()
    }

    // ---------------------------------------------------------------------
    // 选项选择（照 handleSelect：记录 → 清 UI → 跳目标分支）
    // ---------------------------------------------------------------------

    function handleSelect(index) {
        var q = currentQuestion
        if (q === undefined || q === null || q.options === undefined)
            return

        var option = q.options[index]
        if (option === undefined)
            return

        /* 记录选择（整体替换数组触发通知） */
        if (path.length > 0) {
            var p = path.slice()
            p[p.length - 1] = {
                nodeId: p[p.length - 1].nodeId,
                selectedOptionIndex: index
            }
            path = p
        }

        questionActive = false
        questionEnded = false
        countdownVisible = false

        /* 跳转目标分支节点 */
        switchToNode(option.targetNodeId)
    }

    // ---------------------------------------------------------------------
    // 节点切换（照 switchToNode + pathTracker.jumpTo 的截断 / 追加语义）
    // ---------------------------------------------------------------------

    function switchToNode(nodeId) {
        var node = storyNodes !== null ? storyNodes[nodeId] : undefined
        if (node === undefined)
            return

        /* jumpTo 语义：目标在历史中 → 截断到它并重置选择；全新 → 追加 */
        var idx = -1
        for (var i = 0; i < path.length; i++) {
            if (path[i].nodeId === nodeId) {
                idx = i
                break
            }
        }

        var p = []
        if (idx >= 0) {
            for (var j = 0; j <= idx; j++) {
                p.push(j === idx
                       ? { nodeId: nodeId, selectedOptionIndex: -1 }
                       : { nodeId: path[j].nodeId,
                           selectedOptionIndex: path[j].selectedOptionIndex })
            }
        } else {
            p = path.slice()
            p.push({ nodeId: nodeId, selectedOptionIndex: -1 })
        }
        path = p
        rebuildStoryEntries()

        /* 清理上一节点残留 UI + 恢复剧情进度条 */
        questionActive = false
        questionEnded = false
        countdownVisible = false
        replayVisible = false
        endFrameRevision = 0
        storyVisible = true

        applyNodeSource(nodeId)
    }

    // ---------------------------------------------------------------------
    // 重播（照 handleReplay：重置路径 → 跳回起始节点）
    // ---------------------------------------------------------------------

    function handleReplay() {
        if (storyNodes === null || interactiveData.startNodeId === undefined)
            return

        path = [{ nodeId: interactiveData.startNodeId, selectedOptionIndex: -1 }]
        rebuildStoryEntries()

        replayVisible = false
        questionActive = false
        questionEnded = false
        countdownVisible = false
        endFrameRevision = 0
        storyVisible = true

        /* reset 后路径已含起始节点，switchToNode 对其做截断重置并切源 */
        switchToNode(interactiveData.startNodeId)
    }

    // ---------------------------------------------------------------------
    // 选项按钮背景图（节点 question.background 优先，回落插件自带默认图）
    // ---------------------------------------------------------------------

    function questionBackgroundUrl() {
        var q = currentQuestion
        if (q !== undefined && q !== null
                && q.background !== undefined && q.background !== null
                && q.background.toString() !== "")
            return q.background
        return "../../assets/images/interactive-video/bg-default.png"
    }

    // ---------------------------------------------------------------------
    // 整层显隐协议（照 web 的 data-ctrl-hidden 联动 + active 例外）
    // ---------------------------------------------------------------------

    /*
     * 控制栏隐藏 → 整层淡出 300ms 并禁用交互（避免透明状态误触选项）；
     * 激活例外：问题显示中（时间窗 / 播完）或重播显示中 → 保持全亮可交互
     * （对应 web 的 .nova-player-interaction-active + :has 规则）。
     */
    opacity: controlsShown || questionActive || replayVisible ? 1 : 0
    visible: opacity > 0.001
    enabled: controlsShown || questionActive || replayVisible

    Behavior on opacity {
        NumberAnimation {
            duration: 300
            easing.type: Easing.Linear
        }
    }

    // ---------------------------------------------------------------------
    // 剧情进度条（照 storyBar.ts + interactive-video.scss 的 story 段）
    // ---------------------------------------------------------------------

    /*
     * 胶囊条：底部居中悬浮（普通 70px / 全屏 98px），黑 30% 圆角 20，
     * 高 40；宽度 = 圆点行宽（每节点 50px：40 圆点 + 左右各 5px 间距），
     * 节点数变化时 600ms 平滑伸缩（照 .nova-player-interaction-oval 的
     * transition: 0.6s ease-out）。
     */
    Rectangle {
        id: storyBar

        visible: interactiveVideoLayer.storyVisible
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        /* 全屏档 98px（web 的 [data-screen="full"]/["web"] 段同款） */
        anchors.bottomMargin: interactiveVideoLayer.fullscreen ? 98 : 70
        height: 40
        radius: 20
        color: Qt.rgba(0, 0, 0, 0.3)
        width: 10 + interactiveVideoLayer.storyEntries.length * 50

        Behavior on width {
            NumberAnimation {
                duration: 600
                easing.type: Easing.OutCubic
            }
        }

        /* 圆点行（40px 圆点 + 10px 间距 = 每项 50px；左右各 5px 内边距） */
        Row {
            id: storyRow

            x: 5
            height: 40
            spacing: 10

            Repeater {
                model: interactiveVideoLayer.storyEntries

                delegate: Item {
                    id: storyDot

                    width: 40
                    height: 40

                    /* 路径条目（storyEntries 的元素） */
                    readonly property var entry: modelData

                    /* 悬停态（驱动双图标切换 + 悬停卡片显隐） */
                    property bool dotHovered: false

                    /* 圆点内圈 36×36（web 的 oval-round，居中偏 2px） */
                    Item {
                        x: 2
                        y: 2
                        width: 36
                        height: 36

                        /* 双图标层叠放：normal 常态 / hover 悬停（蓝底），
                           isCurrent 挑位置 / 打勾（照 renderOvalItem） */
                        Image {
                            anchors.fill: parent
                            sourceSize: Qt.size(36, 36)
                            source: storyDot.entry.isCurrent
                                    ? "../../assets/images/interactive-video/story-position.svg"
                                    : "../../assets/images/interactive-video/story-check.svg"
                            visible: !storyDot.dotHovered
                        }

                        Image {
                            anchors.fill: parent
                            sourceSize: Qt.size(36, 36)
                            source: storyDot.entry.isCurrent
                                    ? "../../assets/images/interactive-video/story-position-hover.svg"
                                    : "../../assets/images/interactive-video/story-check-hover.svg"
                            visible: storyDot.dotHovered
                        }
                    }

                    /* 悬停热区（web 的 oval-item cursor:pointer） */
                    MouseArea {
                        id: dotHoverArea

                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onContainsMouseChanged: storyDot.dotHovered = containsMouse
                    }

                    /*
                     * 悬停卡片（照 renderHoverItem / scss 的 hover-item）：
                     * 119×20 白底标题条 + 上方 117×73 封面 + 下指小三角。
                     * 定位：卡片左缘 = 圆点中心 - 35（web 的 margin-left:-35px
                     * 折算到圆点坐标系为 x = -15）、顶缘 = -27px（margin-top）。
                     */
                    Item {
                        id: hoverCard

                        z: 10
                        visible: storyDot.dotHovered
                        x: -15
                        y: -27
                        width: 119
                        height: 20

                        /* 标题条背景（纯白） */
                        Rectangle {
                            anchors.fill: parent
                            color: "#ffffff"
                        }

                        /* 标题文本（单行溢出省略，#222 12px） */
                        Text {
                            x: 10
                            width: parent.width - 20
                            height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            text: storyDot.entry.title
                            color: "#222222"
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }

                        /* 下指小三角（web 双层白三角在白底上无视觉差，单层等价） */
                        Canvas {
                            x: 30
                            y: parent.height
                            width: 10
                            height: 5

                            onPaint: {
                                var ctx = getContext("2d")
                                ctx.reset()
                                ctx.fillStyle = "#ffffff"
                                ctx.beginPath()
                                ctx.moveTo(0, 0)
                                ctx.lineTo(width, 0)
                                ctx.lineTo(width / 2, height)
                                ctx.closePath()
                                ctx.fill()
                            }
                        }

                        /*
                         * 节点封面（标题条上方悬浮：117×73、白 1px 描边，
                         * top:-65 / left:3 折算为 y:-65 / x:3）
                         */
                        Item {
                            x: 3
                            y: -65
                            width: 117
                            height: 73

                            Image {
                                anchors.fill: parent
                                fillMode: Image.PreserveAspectCrop
                                source: storyDot.entry.cover
                                asynchronous: true
                            }

                            /* 白 1px 描边（照 .nova-player-interaction-cover 的 border） */
                            Rectangle {
                                anchors.fill: parent
                                color: "transparent"
                                border.width: 1
                                border.color: "#ffffff"
                            }
                        }
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------------
    // 分支选择（照 questionCanvas.ts —— QML 声明式等价实现）
    // ---------------------------------------------------------------------

    Item {
        id: questionRoot

        visible: interactiveVideoLayer.questionActive
        anchors.fill: parent

        /*
         * 播完模式：视频最后一帧定格背景（requestSnapshot 异步截屏 →
         * image://snapshot/<rev>，cover 等比裁剪铺满 —— 对齐 web 的
         * drawSourceCover(video)）。
         */
        Image {
            visible: interactiveVideoLayer.questionEnded
                     && interactiveVideoLayer.endFrameRevision > 0
            anchors.fill: parent
            fillMode: Image.PreserveAspectCrop
            source: visible ? ("image://snapshot/" + interactiveVideoLayer.endFrameRevision) : ""
        }

        /*
         * 全屏遮罩：播完模式压暗定格画面（有末帧 45% / 无末帧 70% 兜底，
         * 照 drawQuestion 的 rgba(0,0,0,0.45) / 0.7）；时间窗模式画布
         * 全透明（0），视频画面 / 弹幕从下层透出。
         */
        Rectangle {
            anchors.fill: parent
            color: "#000000"
            opacity: interactiveVideoLayer.questionEnded
                     ? (interactiveVideoLayer.endFrameRevision > 0 ? 0.45 : 0.7)
                     : 0
        }

        /*
         * 点击拦截：web 的 canvas pointer-events:auto 盖满全屏 —— 问题显示
         * 期间点击画布空白处不穿透到底层（不触发暂停/继续）。
         */
        MouseArea {
            anchors.fill: parent
            hoverEnabled: false
            onClicked: function (mouse) { mouse.accepted = true }
        }

        /*
         * 题目 + 选项按钮组（横向流式布局：坐标由 optionLayout 计算 ——
         * 一行放不下自动换行、每行按实际按钮数水平居中、整体含题目
         * 垂直居中，对齐 web 端 layoutOptionRects 的同一套算法）
         */
        Item {
            anchors.fill: parent

            /* 题目文案（可选；白 18px 600，Raised 样式近似 canvas 阴影） */
            Text {
                visible: interactiveVideoLayer.optionLayout.titleVisible
                y: interactiveVideoLayer.optionLayout.titleY
                anchors.horizontalCenter: parent.horizontalCenter
                height: 36
                verticalAlignment: Text.AlignVCenter
                text: (interactiveVideoLayer.currentQuestion !== undefined
                       && interactiveVideoLayer.currentQuestion !== null
                       && interactiveVideoLayer.currentQuestion.title !== undefined)
                      ? interactiveVideoLayer.currentQuestion.title : ""
                color: "#ffffff"
                font.pixelSize: 18
                font.weight: Font.DemiBold
                style: Text.Raised
                styleColor: "#99000000"
            }

            Repeater {
                model: (interactiveVideoLayer.currentQuestion !== undefined
                        && interactiveVideoLayer.currentQuestion !== null
                        && interactiveVideoLayer.currentQuestion.options !== undefined)
                       ? interactiveVideoLayer.currentQuestion.options : []

                delegate: Item {
                    id: optionButton

                    /* 流式布局坐标（optionLayout 重算时整体刷新；
                       过渡瞬间的越界下标以 null 兜底到 0） */
                    readonly property var rect: index < interactiveVideoLayer.optionLayout.rects.length
                                                ? interactiveVideoLayer.optionLayout.rects[index]
                                                : null
                    x: rect !== null ? rect.x : 0
                    y: rect !== null ? rect.y : 0

                    width: 300
                    height: 52

                    /* 悬停态（遮罩减淡 / 文字转蓝 / 中心放大 1.07；描边恒定白色） */
                    property bool hovered: optionArea.containsMouse

                    /*
                     * 悬停缩放：围绕中心平滑放大 / 缩回（web 是 rAF 指数
                     * 逼近 HOVER_SCALE_TARGET 1.07，QML 的 scale 默认围绕
                     * Item.Center + Behavior 平滑过渡，视觉等价）
                     */
                    scale: optionButton.hovered ? 1.07 : 1

                    Behavior on scale {
                        NumberAnimation {
                            duration: 150
                            easing.type: Easing.OutCubic
                        }
                    }

                    /* 1. 背景图（cover 等比裁剪，隐藏供 MultiEffect 取圆角源） */
                    Image {
                        id: optionBg

                        anchors.fill: parent
                        visible: false
                        fillMode: Image.PreserveAspectCrop
                        source: interactiveVideoLayer.questionBackgroundUrl()
                        asynchronous: true
                    }

                    /* 圆角 8 裁剪的遮罩源（照 roundRectPath 的 radius 8） */
                    Rectangle {
                        id: optionMaskSource

                        anchors.fill: parent
                        visible: false
                        radius: 8
                        color: "#ffffff"
                    }

                    MultiEffect {
                        source: optionBg
                        anchors.fill: optionBg
                        maskEnabled: true
                        maskSource: optionMaskSource
                    }

                    /* 2. 半透明黑遮罩（保证文字可读；悬停减淡突出高亮） */
                    Rectangle {
                        anchors.fill: parent
                        radius: 8
                        color: "#000000"
                        opacity: optionButton.hovered ? 0.35 : 0.55
                    }

                    /* 3. 描边（白色边框恒定不变色：悬停反馈只保留平滑放大，对齐 web 端） */
                    Rectangle {
                        anchors.fill: parent
                        radius: 8
                        color: "transparent"
                        border.width: optionButton.hovered ? 2 : 1.5
                        border.color: Qt.rgba(1, 1, 1, 0.8)
                    }

                    /* 4. 选项文字（前缀 + 文案居中；悬停转蓝） */
                    Text {
                        anchors.centerIn: parent
                        text: (modelData.label !== undefined && modelData.label !== ""
                               ? modelData.label + "  " : "") + modelData.text
                        color: optionButton.hovered ? "#00A1D6" : "#ffffff"
                        font.pixelSize: 16
                        font.weight: Font.Medium
                    }

                    /* 命中 + 小手光标（web：hover 命中时 cursor:pointer） */
                    MouseArea {
                        id: optionArea

                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: interactiveVideoLayer.handleSelect(index)
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------------
    // 倒计时进度条（照 countdown.ts / scss 的 countdown 段：贴底 4px 白 80%）
    // ---------------------------------------------------------------------

    Item {
        visible: interactiveVideoLayer.countdownVisible
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 4

        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            /* 剩余比例宽度（500ms 线性过渡抹平 position 上报周期阶梯感，
               照 .nova-player-interaction-countdown-bar 的 transition） */
            width: parent.width * interactiveVideoLayer.countdownRatio
            color: "#ffffff"
            opacity: 0.8

            Behavior on width {
                NumberAnimation {
                    duration: 500
                    easing.type: Easing.Linear
                }
            }
        }
    }

    // ---------------------------------------------------------------------
    // 重播按钮（照 replay.ts / scss 的 replay 段：top 5% / right 7%）
    // ---------------------------------------------------------------------

    Item {
        visible: interactiveVideoLayer.replayVisible
        anchors.top: parent.top
        anchors.topMargin: parent.height * 0.05
        anchors.right: parent.right
        anchors.rightMargin: parent.width * 0.07
        height: 26
        width: replayRow.width
        opacity: 0.7

        Row {
            id: replayRow

            spacing: 4
            height: 26

            Image {
                width: 26
                height: 26
                sourceSize: Qt.size(26, 26)
                source: "../../assets/images/interactive-video/replay.svg"
            }

            Text {
                height: 26
                verticalAlignment: Text.AlignVCenter
                text: qsTr("重播")
                color: "#ffffff"
                font.pixelSize: 16
                style: Text.Raised
                styleColor: "#99000000"
            }
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: interactiveVideoLayer.handleReplay()
        }
    }
}
