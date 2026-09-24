// ===========================================================================
// 弹幕层：一个完整的弹幕系统（引擎 + 渲染层）
//
// 功能照着参考 front/hilihili-components/src/utils/danmaku（README + types/trackManager/
// domEngine/scheduler/scaleHelper/objectPool）来做，**实现方式换一套**：
//
//   参考的做法                          这一版的做法
//   ----------------------------------  ------------------------------------------
//   DOM 元素 + 每条弹幕一条 CSS 过渡     一条 FrameAnimation 驱动全层，位置按时钟解析算出
//   Canvas 引擎（>200 条自动切）        没有第二套引擎：一套"视图对象池"扛到底
//   Worker 里建时间索引 + 每帧二分       数据排序一次 + 分段起点表 + 前进游标（seek 才二分）
//   用 getBoundingClientRect 量宽度      TextMetrics 同步量（不用等一次布局）
//   createTime/elapsed 反推 x            直接以"播放时钟"为唯一时间基准
//   mask-image 三层 CSS 复合             未实现（见文件末尾的 TODO，防挡是下一件）
//
// 行为一致的地方（数值都标了参考出处）：
//   * 三种弹幕：滚动 / 顶部固定 / 底部固定（types.ts:9-18）
//   * 滚动：distance = 容器宽 + 弹幕宽，基础速度 150px/s × 档位倍率（domEngine.ts:543-549）
//   * 固定：时长 4000ms，0→10% 淡入放大、10→90% 停留、90→100% 淡出（domEngine.ts:361-378）
//   * 轨道：初始只开 70%，不够时再扩；碰撞用"右边缘 + 动态安全距离"两段判定
//     （trackManager.ts:70-110 / 205-321）
//   * 顶部固定只用前 1/3 轨道、底部固定不用最下面 20%（trackManager.ts:329-387）
//   * 自动缩放：clamp(宽/1280 × dpr^-0.3, 0.75, 1.25)，字号再夹到 12-36（scaleHelper.ts:35-89）
//   * 底部安全区（默认 80px）：默认不占，轨道不够时扩进去（README:737）
//   * 速度/区域/字号/透明度的 setter 只影响**新**弹幕（README:1565-1573）
// ===========================================================================
import QtQuick

Item {
    id: layer

    /* =======================================================================
     * 对外接口
     * ======================================================================= */

    /* 播放器组件：引擎的时间基准就是它的 position（毫秒） */
    property var player: null

    /* 是否全屏（只用来自动缩放要用屏幕 DPR；轨道跟着自己的高度走） */
    property bool fullscreen: false

    /* 弹幕开关 */
    property bool danmakuEnabled: true

    /* 悬停回调：danmaku 为 null 表示移开。x/y 是"水平中心 + 底边"（参考 domEngine.ts:1256-1264） */
    signal danmakuHovered(var danmaku, real x, real y)

    /* 渲染中的条数（统计用）—— 用显式维护的 activeCount，不能直接绑 _active.length：
       JS 数组 push 不会发信号，绑上去只在整体重新赋值时才刷新 */
    readonly property int renderCount: activeCount

    /* =======================================================================
     * 设置（默认值逐条来自参考的 defaultConfig，README:830-855）
     * ======================================================================= */

    /* DanmakuSpeed：1 极慢 / 2 较慢 / 3 适中 / 4 较快 / 5 极快（types.ts:21-32） */
    property int speed: 3
    /* DanmakuArea：0.25 / 0.5 / 0.75 / 1（types.ts:43-52） */
    property real area: 1.0
    property int baseFontSize: 18
    property real fontSizeScale: 1.0
    property real danmakuOpacity: 1.0
    property bool autoScale: true
    property real trackHeight: 24
    property real trackGap: 4
    property real topMargin: 10
    property real bottomMargin: 10
    /* 底部安全区（字幕区），默认 80（README:844） */
    property real bottomSafeArea: 80
    property real safeDistance: 20
    /* 同时在屏上限：options.maxRenderCount 默认 2000（README 里写的 500 是旧值，以实现为准）。
       池是"用到才建"，所以这个数只是天花板，不会真的先建 2000 个 item。 */
    property int maxRenderCount: 2000
    /* 分段时长（秒） */
    property real segmentDuration: 30
    /* 过滤器：{ scroll, fixed, colorful }（types.ts:55-62） */
    property var filter: ({})

    /*
     * 渲染设置（对应弹幕面板右页那几项，由面板写入）：
     *   fontFamily      弹幕字体（参考右页"弹幕字体"下拉）
     *   fontBold        粗体
     *   outlineType     描边类型：0 重墨 / 1 描边 / 2 45°投影
     *   speedFollowRate 弹幕速度同步播放倍数：开 = 倍速播放时弹幕也跟着快（关就恒按 1 倍速走）
     */
    property string fontFamily: "Microsoft YaHei"
    property bool fontBold: true
    property int outlineType: 0
    property bool speedFollowRate: true

    /* 速度档位倍率（domEngine.ts:52-58） */
    readonly property var speedMultipliers: ({ 1: 0.5, 2: 0.75, 3: 1.0, 4: 1.5, 5: 2.0 })

    /* 基础速度：150px/秒（domEngine.ts:544） */
    readonly property real baseSpeed: 150
    /* 固定弹幕时长：4000ms（domEngine.ts:551） */
    readonly property int fixedDuration: 4000

    /* =======================================================================
     * 内部状态
     * ======================================================================= */

    /* 全部弹幕（按 time 升序），每项是参考的 DanmakuItem 那份字段 */
    property var _items: []
    /* 每条是否已经发过（seek 回退时清掉对应区段） */
    property var _emitted: []
    /* 发到哪一条了 */
    property int _cursor: 0
    /* 每个分段的起始下标（分段 + 二分用） */
    property var _segmentStart: []
    /* 正在显示的弹幕记录 */
    property var _active: []
    /* 轨道（车道）*/
    property var _tracks: []
    /* 视图对象池：空闲的 */
    property var _freeViews: []
    /* 全部创建过的视图 */
    property var _allViews: []
    /* 已扩展过几次（交替 +5 / +4，trackManager.ts:137-138） */
    property int _extendCount: 0
    /* 鼠标位置（层内坐标），-1 = 不在层里 */
    property real _pointerX: -1
    property real _pointerY: -1
    /* 鼠标动过、还没重做命中检测（见 _advance 末尾） */
    property bool _pointerMoved: false
    /* 当前悬停的那条 */
    property var _hovered: null

    /* 播放时钟（毫秒）：引擎唯一的时间基准 */
    property real _clockMs: 0
    property real _lastWallMs: 0
    /* 与播放器 position 的偏差（正 = 时钟快了），由 _step 每帧补 5% 收敛，不硬对齐 */
    property real _driftMs: 0
    property bool _started: false
    /* 最近一次同步到的播放器位置（用来判 seek） */
    property real _lastSyncedMs: -1

    /* 当前字号（autoScale 后），新弹幕用它 */
    readonly property int currentFontSize: {
        var base = baseFontSize * fontSizeScale
        if (!autoScale)
            return Math.max(12, Math.min(36, Math.round(base)))

        return Math.max(12, Math.min(36, Math.round(base * layer.autoScaleFactor)))
    }

    /*
     * 自动缩放比例（参考 scaleHelper.ts:35-62 的思路，但**去掉 dpr 那一项**）：
     *   参考：scale = 容器宽/1280，再 × dpr^-0.3（浏览器里 devicePixelRatio 表示 CSS 像素与设备像素之比），
     *         最后夹在 [0.75, 1.25]。
     *   这里：**只按层宽算**，夹在 [0.75, 1.5]。
     *
     * 【为什么去掉 dpr】浏览器那套 dpr 修正是因为 CSS 像素不随系统缩放变；Qt 这边逻辑像素
     * **已经**含了系统缩放（150% 时全屏的逻辑宽还是 1280），再乘一次 dpr^-0.3 等于**双重缩小** ——
     * 表现就是"全屏后弹幕非常小"（150% 缩放的机器上比例只有 0.887）。
     * 【为什么上限提到 1.5】参考 README 写的"最大 1.5 倍"就是期望值（实现里是 1.25），
     * 按 README 取 1.5，全屏（1920/1280 = 1.5）时弹幕才能明显跟着变大。
     */
    readonly property real autoScaleFactor: {
        var w = layer.width > 0 ? layer.width : 1280
        return Math.max(0.75, Math.min(1.5, w / 1280))
    }

    /*
     * 动态安全距离（trackManager.ts:249-274，按当前平均密度分档）。
     * 刻意写成函数而不是绑定：密度来自各轨道 items 数组的长度，那些数组是原地 push/splice 的，
     * QML 不会因此重算绑定（绑上去就永远停在初始值了）。
     */
    function _dynamicSafeDistance() {
        var total = 0

        for (var i = 0; i < _tracks.length; ++i)
            total += _tracks[i].items.length

        var density = total / Math.max(1, _tracks.length)

        if (density <= 1)
            return 150

        if (density <= 5)
            return 100

        if (density <= 15)
            return 60

        if (density <= 25)
            return 40

        return 20
    }

    visible: danmakuEnabled
    /*
     * 不要写 enabled: false —— 那会把这个 Item（连同里面的 HoverHandler）一起禁掉。
     * 弹幕层不吃点击靠的是"HoverHandler 只处理 hover、不抢按键"这一点，
     * 单击/双击仍然落到下面视频区那层 MouseArea 上。
     */

    /* 正在显示的条数（显式维护：JS 数组 push 不会触发 QML 绑定，绑定里不能直接用它的 length） */
    property int activeCount: 0

    /* =======================================================================
     * 测量 + 视图池
     * ======================================================================= */

    /* 同步量文字宽度 / 行高（参考是 append 到 DOM 再读 getBoundingClientRect） */
    TextMetrics {
        id: metrics

        /* 量宽度必须用和渲染同一套字体/粗体，否则本人白框和碰撞判定会偏 */
        font.family: layer.fontFamily
        font.bold: layer.fontBold
    }

    Component {
        id: itemComponent

        DanmakuItem {}
    }

    /* 拿一个视图（池空就新建，到 maxRenderCount 就不再新建） */
    function _acquireView() {
        if (_freeViews.length > 0)
            return _freeViews.pop()

        if (_allViews.length >= Math.max(1, maxRenderCount))
            return null

        var view = itemComponent.createObject(layer)

        if (view === null)
            return null

        _allViews.push(view)
        view.visible = false
        return view
    }

    function _releaseView(view) {
        view.visible = false
        view.z = 0
        _freeViews.push(view)
    }

    /* =======================================================================
     * 数据装入（参考是 DanmakuScheduler：30s 分段 + 二分 + 已发射去重）
     * 这里是"排一次序 + 建分段起点表 + 前进游标"，只有 seek 才二分定位
     * ======================================================================= */

    function loadDanmaku(list) {
        var data = []

        for (var i = 0; i < list.length; ++i) {
            var it = list[i]

            if (it === null || it === undefined || it.time === undefined)
                continue

            data.push({
                "id": it.id !== undefined ? it.id : i,
                "text": it.text !== undefined ? String(it.text) : "",
                "time": Number(it.time),
                "type": it.type !== undefined ? Number(it.type) : 1,
                "fontSize": it.fontSize,
                "color": it.color !== undefined ? String(it.color) : "#ffffff",
                "userId": it.userId,
                "userName": it.userName,
                "isVip": it.isVip === true,
                "weight": it.weight !== undefined ? Number(it.weight) : 0,
                "speed": it.speed,
                "uid": it.uid
            })
        }

        data.sort(function (a, b) { return a.time - b.time })
        _items = data
        _emitted = new Array(data.length)
        for (var k = 0; k < data.length; ++k)
            _emitted[k] = false

        _rebuildSegments()
        /*
         * 游标从"当前播放位置"开始，不是从 0 —— 否则在视频播到一半时载入弹幕数据，
         * 会把这一时刻之前的所有弹幕一次性喷出来。
         */
        _cursor = _lowerBound(Math.max(0, _clockMs / 1000))
        _hovered = null
        _clearActive()
    }

    /* 分段起点表：第 i 段的第一条在 _items 里的下标（升序、可线性推进） */
    function _rebuildSegments() {
        var segs = []
        var i = 0
        var last = _items.length > 0 ? _items[_items.length - 1].time : 0
        var maxSeg = Math.floor(last / segmentDuration)

        for (var seg = 0; seg <= maxSeg + 1; ++seg) {
            var t = seg * segmentDuration

            while (i < _items.length && _items[i].time < t)
                ++i

            segs.push(i)
        }

        _segmentStart = segs
    }

    /* 某个时间属于第几段 */
    function getSegmentIndex(tSec) {
        return Math.max(0, Math.floor(tSec / segmentDuration))
    }

    /* 某一段里有哪些弹幕（对应参考 DanmakuScheduler.getSegmentDanmaku） */
    function getSegmentDanmaku(tSec) {
        var seg = getSegmentIndex(tSec)

        if (_segmentStart.length === 0)
            return []

        var from = _segmentStart[Math.min(seg, _segmentStart.length - 1)]
        var to = (seg + 1 < _segmentStart.length) ? _segmentStart[seg + 1] : _items.length
        var out = []

        for (var i = from; i < to; ++i)
            out.push(_items[i])

        return out
    }

    /* 二分：找出第一条 time >= t 的下标（seek 用） */
    function _lowerBound(t) {
        var lo = 0
        var hi = _items.length

        while (lo < hi) {
            var mid = (lo + hi) >> 1

            if (_items[mid].time < t)
                lo = mid + 1
            else
                hi = mid
        }

        return lo
    }

    /* =======================================================================
     * 轨道（车道）
     *
     * trackManager.ts:70-110 ——
     *   areaHeight   = floor(容器高 × 区域档)
     *   available    = areaHeight − topMargin − bottomMargin
     *   maxCount     = floor(available / (trackHeight + trackGap))
     *   初始只开 max(1, floor(maxCount × 0.7))
     * 参考的"扩到安全区"那段实际是死代码（候选位置和已有轨道恒差 24px < 26px 判定，
     * 永远加不进去），这里按它的**意图**实现：默认避开底部安全区，不够时再往下扩。
     * ======================================================================= */

    function _maxTrackCount(useSafeArea) {
        var areaHeight = Math.floor(height * area)
        var available = areaHeight - topMargin - bottomMargin

        if (useSafeArea)
            available -= bottomSafeArea

        return Math.max(1, Math.floor(available / (trackHeight + trackGap)))
    }

    function _rebuildTracks() {
        var areaHeight = Math.floor(height * area)
        var maxCount = _maxTrackCount(true)
        var count = Math.max(1, Math.floor(maxCount * 0.7))
        var tracks = []

        for (var i = 0; i < count; ++i) {
            var y = topMargin + i * (trackHeight + trackGap)

            if (y + trackHeight > areaHeight)
                break

            tracks.push({ "y": y, "items": [] })
        }

        _tracks = tracks
        _extendCount = 0
    }

    /* 轨道不够时往下扩（+5 / +4 交替，trackManager.ts:137-138 的节奏） */
    function _extendTracks() {
        var areaHeight = Math.floor(height * area)
        var maxCount = _maxTrackCount(false)

        if (_tracks.length >= maxCount)
            return false

        var step = (_extendCount % 2 === 0) ? 5 : 4
        var want = Math.min(step, maxCount - _tracks.length)
        var added = 0
        var nextIndex = _tracks.length
        var y = topMargin + nextIndex * (trackHeight + trackGap)

        while (added < want && y + trackHeight <= areaHeight) {
            _tracks.push({ "y": y, "items": [] })
            y += trackHeight + trackGap
            ++added
        }

        if (added > 0) {
            ++_extendCount
            return true
        }

        return false
    }

    /* 轨道可用性判定（trackManager.ts:284-321 的两段式判定，原样搬语义） */
    function _trackAvailable(track, item, currentMs) {
        var safe = _dynamicSafeDistance()

        for (var i = 0; i < track.items.length; ++i) {
            var rec = track.items[i]

            if (rec.type !== 1)
                continue

            var elapsed = _elapsedOf(rec, currentMs)
            var existingX = layer.width - (layer.width + rec.width) * (elapsed / rec.duration)

            /* 还在右侧、离得不够远 → 不能放 */
            if (existingX > layer.width - item.width - safe * 2)
                return false

            if (existingX + rec.width > 0) {
                var remaining = rec.duration - elapsed
                /* 快出屏了就不算冲突（参考：安全距离 > 80 时 2000ms，否则 1000ms） */
                if (remaining < (safe > 80 ? 2000 : 1000))
                    continue

                var newSpeed = (layer.width + item.width) / item.duration
                var oldSpeed = (layer.width + rec.width) / rec.duration

                /* 新弹幕更快且旧弹幕还在右半边 → 会追尾 */
                if (newSpeed > oldSpeed && existingX > layer.width / 2)
                    return false
            }
        }

        return true
    }

    function _fixedTrackAvailable(track, type, currentMs) {
        for (var i = 0; i < track.items.length; ++i) {
            var rec = track.items[i]

            if (rec.type !== type)
                continue

            var elapsed = _elapsedOf(rec, currentMs)

            /* 还剩 500ms 以上就算占着（trackManager.ts:344/373） */
            if (rec.duration - elapsed > 500)
                return false
        }

        return true
    }

    /* 选轨道：返回下标，-1 = 没位置（trackManager.ts:205-243） */
    function _pickTrack(item, currentMs) {
        if (item.type === 2 || item.type === 3)
            return _pickFixedTrack(item.type, currentMs)

        for (var i = 0; i < _tracks.length; ++i) {
            var track = _tracks[i]

            if (track.items.length === 0)
                return i

            if (_trackAvailable(track, item, currentMs))
                return i
        }

        if (_extendTracks()) {
            for (var j = 0; j < _tracks.length; ++j) {
                var t2 = _tracks[j]

                if (t2.items.length === 0)
                    return j

                if (_trackAvailable(t2, item, currentMs))
                    return j
            }
        }

        return -1
    }

    function _pickFixedTrack(type, currentMs) {
        var bottomBoundary = Math.floor(height * 0.8)

        if (type === 2) {
            /* 顶部固定：只用前 1/3 的轨道（trackManager.ts:335） */
            var maxIndex = Math.floor(_tracks.length / 3)

            for (var i = 0; i < maxIndex && i < _tracks.length; ++i) {
                if (_fixedTrackAvailable(_tracks[i], type, currentMs))
                    return i
            }

            return -1
        }

        /* 底部固定：不用最下面 20%（trackManager.ts:358-364） */
        for (var j = 0; j < _tracks.length; ++j) {
            if (_tracks[j].y > bottomBoundary)
                continue

            if (_fixedTrackAvailable(_tracks[j], type, currentMs))
                return j
        }

        return -1
    }

    /* =======================================================================
     * 发射一条弹幕（新建视图 + 占轨道）
     * ======================================================================= */

    function _spawn(item, currentMs) {
        /* 过滤（types.ts:55-62 的 scroll/fixed/colorful；另外参考的设置面板把固定分成了
           "顶部/底部"两个勾选项，所以这里也认 top / bottom 两个键）：立即生效，只影响新弹幕 */
        if (filter !== null && filter !== undefined) {
            if (filter.scroll === true && item.type === 1)
                return false

            if (filter.fixed === true && (item.type === 2 || item.type === 3))
                return false

            if (filter.top === true && item.type === 2)
                return false

            if (filter.bottom === true && item.type === 3)
                return false

            /* 「高级」那一项对应参考 DanmakuType.ADVANCED = 4（我们暂时不渲染这类，但过滤键认它） */
            if (filter.advanced === true && item.type === 4)
                return false

            if (filter.colorful === true) {
                var c = String(item.color !== undefined ? item.color : "#ffffff").toLowerCase()

                if (c !== "#ffffff" && c !== "#fff" && c !== "white")
                    return false
            }
        }

        /*
         * 字号 = （弹幕自带字号，没有就用基准字号）× 用户缩放 × **自动缩放比例**。
         *
         * 【踩过的坑】原来这条只乘了 fontSizeScale，没乘 autoScaleFactor ——
         * 而所有弹幕数据都自带 fontSize（示例/载入的数据里每条都是 18），
         * 于是永远走这个分支，"弹幕随屏幕缩放"勾了也没用、全屏后字一样大。
         * 参考那边是 (item.fontSize || baseFontSize) × fontSizeScale，
         * 而它的 fontSizeScale 本身就含自动缩放（resize 时 calculateFontSize()/baseFontSize），
         * 所以自带字号的弹幕也必须乘这个比例。
         */
        var scaleAll = fontSizeScale * (autoScale ? autoScaleFactor : 1)
        var fontSize = (item.fontSize !== undefined && item.fontSize > 0)
                ? Math.round(item.fontSize * scaleAll)
                : currentFontSize
        var isSelf = (item.uid === 1 || item.uid === "1")
        var isFixed = (item.type === 2 || item.type === 3)
        /*
         * 尺寸口径：参考是量元素（此时已经把 padding/border/内边距都算进去了）的
         * getBoundingClientRect()，所以 width/height 是**整盒**尺寸，运动和碰撞判定都用它。
         * 这里同样：文字宽 + 左右 padding + 边框；否则本人白框会画在文字上（就是"白框压字"）。
         */
        var padH = isFixed ? 8 : (isSelf ? 6 : 0)
        var padV = (isFixed || isSelf) ? 2 : 0
        var borderW = isSelf ? 2 : 0
        var textWidth = _measureWidth(item.text, fontSize)
        var width = textWidth + 2 * (padH + borderW)
        var height = fontSize + 2 * (padV + borderW)
        var distance = layer.width + width
        var useSpeed = (item.speed !== undefined && item.speed >= 1 && item.speed <= 5)
                ? item.speed : speed
        var duration = item.type === 1
                ? (distance / (baseSpeed * speedMultipliers[useSpeed])) * 1000
                : fixedDuration

        /* 先把记录建起来，轨道判定要用它（width/duration） */
        var rec = {
            "item": item,
            "type": item.type,
            "width": width,
            "height": height,
            "fontSize": fontSize,
            "startMs": currentMs,
            "duration": duration,
            "pauseAccum": 0,
            "pauseStart": -1,
            "view": null,
            "x": 0,
            "y": 0,
            "baseOpacity": 1,
            "baseScale": 1
        }
        var trackIndex = _pickTrack(rec, currentMs)

        if (trackIndex < 0)
            return false

        var view = _acquireView()

        if (view === null)
            return false

        var track = _tracks[trackIndex]

        rec.view = view
        rec.y = track.y
        rec.x = item.type === 1 ? layer.width : Math.round((layer.width - width) / 2)
        view.text = item.text
        view.textColor = item.color !== undefined ? item.color : "#ffffff"
        view.fontSize = fontSize
        /* 文字尺寸和整盒尺寸分开给：item 内部按 padding/border 摆文字，外面按整盒定位 */
        view.textAdvanceWidth = textWidth
        view.textLineHeight = fontSize
        view.selfBorder = isSelf
        view.fixedStyle = isFixed
        /* 渲染设置随每条弹幕下发：字体/粗体/描边类型（参考：这些只影响**新**弹幕） */
        view.fontFamily = fontFamily
        view.fontBold = fontBold
        view.outlineType = outlineType
        view.contentOpacity = danmakuOpacity
        /* 池里复用来的视图要清干净：上一条如果是固定弹幕，scale 会停在 0.9 */
        view.scale = 1
        view.width = width
        view.height = height
        /* 固定弹幕水平居中且会缩放，缩放要绕中心；滚动弹幕按左上角定位 */
        view.transformOrigin = item.type === 1 ? Item.TopLeft : Item.Center
        view.x = rec.x
        view.y = rec.y
        view.visible = true
        view.z = 1

        track.items.push(rec)
        _active.push(rec)
        activeCount = _active.length
        return true
    }

    function _measureWidth(text, fontSize) {
        metrics.font.pixelSize = fontSize
        metrics.text = text

        var w = metrics.width

        /* 兜底：万一 TextMetrics 还没算出来，按字数粗估，避免宽度是 0 引发除零 */
        if (!(w > 0))
            w = Math.max(fontSize, String(text).length * fontSize * 0.6)

        return Math.ceil(w)
    }

    /*
     * 某条弹幕"已经播放了多久"（毫秒）。悬停期间**冻结**：不只是把已结算的 pauseAccum 扣掉，
     * 还要把**正在进行**的那段暂停也扣掉。
     *
     * 【踩过的坑】原来只在移出时才 `pauseAccum += now - pauseStart`，悬停期间 elapsed 照旧在涨，
     * 于是"鼠标移上去弹幕根本不停"。所有算进度的地方都必须走这个函数。
     */
    function _elapsedOf(rec, nowMs) {
        var paused = rec.pauseAccum

        if (rec.pauseStart >= 0)
            paused += nowMs - rec.pauseStart

        return nowMs - rec.startMs - paused
    }

    function _releaseRec(rec) {
        var track = null

        for (var i = 0; i < _tracks.length; ++i) {
            var list = _tracks[i].items
            var at = list.indexOf(rec)

            if (at >= 0) {
                list.splice(at, 1)
                break
            }
        }

        if (_hovered === rec)
            _hovered = null

        if (rec.view !== null) {
            _releaseView(rec.view)
            rec.view = null
        }
    }

    function _clearActive() {
        for (var i = 0; i < _active.length; ++i)
            _releaseRec(_active[i])

        _active = []
        activeCount = 0

        for (var j = 0; j < _tracks.length; ++j)
            _tracks[j].items = []
    }

    /* =======================================================================
     * 帧循环
     * ======================================================================= */

    FrameAnimation {
        running: layer.danmakuEnabled && layer._started
                 && (layer.playing || layer.activeCount > 0)

        onTriggered: layer._step()
    }

    readonly property bool playing: player !== null && player.playing

    /*
     * 播放器暂停时：已经有弹幕的话还要继续收尾（停在原地，不需要动）——
     * 所以只有播放中才推进时钟，暂停时一次 tick 只是停住画面。
     *
     * 【平滑的两个要点】
     *   1. 单帧最多推进 100ms：卡顿/断点后的巨大 delta 直接丢掉，否则弹幕会"跳"一大段；
     *   2. 与播放器 position 的偏差**不硬对齐**，每帧只纠 5%（见 _driftMs）——
     *      框架的 position 更新是稀疏的，硬对齐每次都变成一次可见跳动（就是"很卡"的来源）。
     */
    function _step() {
        var wall = Date.now()
        var delta = _lastWallMs > 0 ? Math.min(100, wall - _lastWallMs) : 0

        _lastWallMs = wall

        if (playing) {
            var rate = (player !== null && player.playbackRate > 0) ? player.playbackRate : 1

            /* "弹幕速度同步播放倍数"关掉时，弹幕恒按 1 倍速走（面板右页那个复选框） */
            if (!speedFollowRate)
                rate = 1

            _clockMs += delta * rate

            /* 漂移收敛：只补 5%，肉眼看不出来，但几百毫秒内就对齐了 */
            if (_driftMs !== 0) {
                var fix = _driftMs * 0.05
                _clockMs += fix
                _driftMs -= fix

                if (Math.abs(_driftMs) < 1)
                    _driftMs = 0
            }
        }

        _emitDue()
        _advance()

        if (_active.length === 0 && !playing)
            _lastWallMs = 0
    }

    /*
     * 把到点的弹幕发出来：游标前进是均摊 O(1)，不用像参考那样每帧二分。
     * 一帧最多发 10 条 —— 参考在 Manager 里也是 min(10, 剩余额度) 的额度控制
     * （index.ts:238），攒了一大批时不会一帧全喷出来。
     */
    function _emitDue() {
        var spawned = 0

        while (_cursor < _items.length && spawned < 10) {
            var item = _items[_cursor]

            if (item.time * 1000 > _clockMs)
                break

            if (_emitted[_cursor] !== true) {
                _emitted[_cursor] = true

                if (_spawn(item, _clockMs))
                    ++spawned
            }

            ++_cursor
        }
    }

    /*
     * 推进所有在显示的弹幕（每帧都跑，所以这里刻意写得不分配内存：
     * 退场的条目**原地压实**，不每帧新建数组 —— 每帧一个 N 元数组会持续制造 GC 抖动，
     * 表现出来就是"弹幕一动一卡"）。
     */
    function _advance() {
        var count = _active.length

        if (count === 0)
            return

        var w = layer.width
        var retired = 0

        for (var i = 0; i < count; ++i) {
            var rec = _active[i]
            var elapsed = _elapsedOf(rec, _clockMs)

            if (rec.type === 1) {
                /* 滚动：x 从容器右边缘线性走到 −宽度 —— 参考是 CSS translate3d 走同一条线 */
                var p = rec.duration > 0 ? Math.max(0, Math.min(1, elapsed / rec.duration)) : 1
                rec.x = w - (w + rec.width) * p
                rec.view.x = rec.x

                if (elapsed >= rec.duration) {
                    /* 跑完了：把视图还给池、把轨道放开（不释放的话轨道会一直被占着，
                       几秒之后所有轨道都"满"了，新弹幕就再也发不出来） */
                    _releaseRec(rec)
                    ++retired
                    continue
                }
            } else {
                /* 固定：0→10% 淡入放大、10→90% 停留、90→100% 淡出（domEngine.ts:361-378） */
                var t = rec.duration > 0 ? Math.max(0, Math.min(1, elapsed / rec.duration)) : 1

                if (t < 0.1) {
                    rec.baseOpacity = t / 0.1
                    rec.baseScale = 0.8 + 0.2 * (t / 0.1)
                } else if (t > 0.9) {
                    rec.baseOpacity = (1 - t) / 0.1
                    rec.baseScale = 1 - 0.1 * ((t - 0.9) / 0.1)
                } else {
                    rec.baseOpacity = 1
                    rec.baseScale = 1
                }

                rec.view.x = rec.x
                rec.view.contentOpacity = danmakuOpacity * rec.baseOpacity
                rec.view.scale = rec.baseScale

                if (elapsed >= rec.duration) {
                    _releaseRec(rec)
                    ++retired
                    continue
                }
            }

            if (retired > 0)
                _active[i - retired] = rec
        }

        if (retired > 0) {
            _active.length = count - retired
            activeCount = _active.length
        }

        /* 命中检测只在鼠标真的动过时做，不必每帧扫一遍 */
        if (_pointerMoved) {
            _syncHover()
            _pointerMoved = false
        }
    }

    /* =======================================================================
     * 播放器同步：以 position 为准，漂了就用它校正
     * ======================================================================= */

    Connections {
        target: layer.player

        function onPositionChanged() {
            if (layer.player === null)
                return

            var pos = layer.player.position
            var prev = layer._lastSyncedMs
            var jumped = prev >= 0 && Math.abs(pos - prev) > 1000
            /* 回退超过 50ms（参考 scheduler 的 0.05s 阈值）→ 这一时刻之后的弹幕要能重发 */
            var rewound = prev >= 0 && (prev - pos) > 50

            if (jumped) {
                /* 跳转：清场重来（在屏弹幕按新位置重新出发） */
                layer._clearActive()
                layer._clockMs = pos
                layer._driftMs = 0
                layer._lastWallMs = 0
                layer._cursor = layer._lowerBound(pos / 1000 - 0.05)

                for (var i = layer._cursor; i < layer._items.length; ++i)
                    layer._emitted[i] = false
            } else {
                if (rewound) {
                    var from = layer._lowerBound(Math.max(0, pos / 1000))

                    for (var j = from; j < layer._items.length; ++j)
                        layer._emitted[j] = false

                    if (layer._cursor > from)
                        layer._cursor = from
                }

                /* 只是漂移：记下来交给 _step 每帧补 5% 平滑收敛（硬对齐 = 看得见的跳动） */
                layer._driftMs = pos - layer._clockMs
            }

            layer._lastSyncedMs = pos
        }

        function onPlayingChanged() {
            if (layer.player !== null && !layer.player.playing) {
                /* 暂停：时钟停住（参考是给弹幕层加 is-paused 暂停所有 CSS 动画） */
                layer._lastWallMs = 0
            }
        }
    }

    onWidthChanged: _rebuildTracks()
    onHeightChanged: _rebuildTracks()
    onAreaChanged: _rebuildTracks()
    onBottomSafeAreaChanged: _rebuildTracks()

    /* 缩放比例一变（窗口/全屏尺寸变了、或面板勾了"弹幕随屏幕缩放"）→ 在屏弹幕也实时重算 */
    onAutoScaleFactorChanged: _rescaleActive()
    onAutoScaleChanged: _rescaleActive()

    /*
     * 把**已经在屏上**的弹幕按当前缩放比例重算一遍（实时缩放）。
     * 参考的语义是"只影响新弹幕"，这里按用户要求改成实时：字号、文字宽、整盒宽高一起更新，
     * 固定弹幕还要重新水平居中（宽度变了）。
     */
    function _rescaleActive() {
        if (_active.length === 0)
            return

        var scaleAll = fontSizeScale * (autoScale ? autoScaleFactor : 1)

        for (var i = 0; i < _active.length; ++i) {
            var rec = _active[i]
            var newSize = (rec.item.fontSize !== undefined && rec.item.fontSize > 0)
                    ? Math.round(rec.item.fontSize * scaleAll) : currentFontSize

            if (newSize === rec.fontSize)
                continue

            var isSelf = (rec.item.uid === 1 || rec.item.uid === "1")
            var isFixed = (rec.type === 2 || rec.type === 3)
            var padH = isFixed ? 8 : (isSelf ? 6 : 0)
            var padV = (isFixed || isSelf) ? 2 : 0
            var borderW = isSelf ? 2 : 0
            var textW = _measureWidth(rec.item.text, newSize)

            rec.fontSize = newSize
            rec.width = textW + 2 * (padH + borderW)
            rec.height = newSize + 2 * (padV + borderW)
            rec.view.fontSize = newSize
            rec.view.textAdvanceWidth = textW
            rec.view.textLineHeight = newSize
            rec.view.width = rec.width
            rec.view.height = rec.height

            if (isFixed) {
                rec.x = Math.round((layer.width - rec.width) / 2)
                rec.view.x = rec.x
            }
        }
    }

    Component.onCompleted: {
        _rebuildTracks()
        _clockMs = player !== null ? player.position : 0
        _started = true
    }

    /* =======================================================================
     * 鼠标悬停：参考给每个元素挂 mouseenter/mouseleave（domEngine.ts:1201-1276）；
     * 这里一个几何命中就够，弹幕再多也只有一个 hover 目标。
     * 命中后该条暂停（保留进度）+ 提到最上层 + 强制不透明（.danmaku-x-roll:hover）。
     *
     * 【鼠标位置为什么是外面喂进来的】
     * 这个 App 的 hover 是"最上层独占"（见 Main.qml:536-545 的注释：光标一进控制层，
     * 画面区那层 MouseArea 就不再更新位置）。弹幕层自己挂 HoverHandler 会跟画面区
     * 那层抢 hover，把自动隐藏/自动出现带坏 —— 所以位置由画面区那层 MouseArea 提供，
     * 这里只负责判"鼠标压在哪条弹幕上"。
     * ======================================================================= */

    /* 鼠标在**本层坐标**里的位置；不在画面里时由 pointerInside = false 表示 */
    property point pointerPosition: Qt.point(-1, -1)
    property bool pointerInside: false

    onPointerPositionChanged: _applyPointer()
    onPointerInsideChanged: _applyPointer()

    function _applyPointer() {
        if (pointerInside)
            _updatePointer(pointerPosition.x, pointerPosition.y)
        else
            _updatePointer(-1, -1)
    }

    /*
     * 悬停满 3 秒自动恢复（参考 domEngine.ts:65 那个 3000ms 定时器：
     * 到点恢复动画、清 z-index/opacity 并回调 (null, null)）。
     */
    Timer {
        id: hoverResumeTimer

        interval: 3000
        repeat: false
        onTriggered: layer._forceResumeHover()
    }

    function _forceResumeHover() {
        if (_hovered === null)
            return

        /* 当作鼠标已经移开；鼠标再动一下就会重新命中 */
        _pointerX = -1
        _pointerY = -1
        _syncHover()
    }

    function _updatePointer(px, py) {
        if (px === _pointerX && py === _pointerY)
            return

        _pointerX = px
        _pointerY = py
        /*
         * 只打脏标记，命中检测放到下一帧的 _advance 里做（一帧最多一次）。
         * 和参考一致：只有鼠标**动**的时候才重新判定，弹幕自己从静止的光标底下划过去不算悬停
         * （参考的命中检测挂在弹幕层 mousemove 上，也是这个语义）。
         */
        _pointerMoved = true
    }

    function _syncHover() {
        var hit = null

        if (_pointerX >= 0 && _pointerY >= 0) {
            /* 后加的在上层，倒着找就是"最上面那条" */
            for (var i = _active.length - 1; i >= 0; --i) {
                var rec = _active[i]

                if (rec.view === null || !rec.view.visible)
                    continue

                if (_pointerX >= rec.view.x && _pointerX <= rec.view.x + rec.view.width
                        && _pointerY >= rec.view.y && _pointerY <= rec.view.y + rec.view.height) {
                    hit = rec
                    break
                }
            }
        }

        /* 悬停目标没变就不要重发信号（否则每帧都在回调） */
        if (hit === _hovered)
            return

        /* 离开旧的那条：恢复动画 */
        if (_hovered !== null) {
            if (_hovered.pauseStart >= 0) {
                _hovered.pauseAccum += _clockMs - _hovered.pauseStart
                _hovered.pauseStart = -1
            }

            if (_hovered.view !== null) {
                _hovered.view.z = 1
                _hovered.view.contentOpacity = danmakuOpacity
            }
        }

        _hovered = hit

        if (hit !== null) {
            /* 暂停：记下停住的那一刻，之后把这段时间补回 elapsed */
            hit.pauseStart = _clockMs
            hit.view.z = 1000
            hit.view.contentOpacity = 1
            hoverResumeTimer.restart()
            _emitHover(hit)
        } else {
            hoverResumeTimer.stop()
            danmakuHovered(null, 0, 0)
        }
    }

    function _emitHover(rec) {
        /* 位置 = 水平中心 + 底边（参考 domEngine.ts:1256-1264） */
        var cx = rec.view.x + rec.view.width / 2
        var by = rec.view.y + rec.view.height

        danmakuHovered(rec.item, cx, by)
    }

    /* =======================================================================
     * 对外方法（名字和参考的 DanmakuEngine / DanmakuManager 对齐）
     * ======================================================================= */

    function start() {
        _started = true
        _lastWallMs = 0
        _driftMs = 0
        _clockMs = player !== null ? player.position : _clockMs
    }

    function stop() {
        _started = false
        _lastWallMs = 0
    }

    /* 立刻显示一条（用户自己发的、或者外部数据源推来的） */
    function addDanmaku(item, currentMs) {
        if (item === null || item === undefined)
            return false

        var now = (currentMs !== undefined && currentMs > 0) ? currentMs : _clockMs
        return _spawn(_normalize(item), now)
    }

    function addDanmakuBatch(list, currentMs) {
        if (list === null || list === undefined)
            return

        var now = (currentMs !== undefined && currentMs > 0) ? currentMs : _clockMs

        for (var i = 0; i < list.length; ++i)
            _spawn(_normalize(list[i]), now)
    }

    /* 用户发弹幕（参考 sendDanmaku：text + options{type,color,fontSize,uid,speed}） */
    function sendDanmaku(text, options) {
        var item = _normalize({
            "id": "self-" + Date.now(),
            "text": text,
            "time": _clockMs / 1000,
            "type": options !== undefined && options.type !== undefined ? options.type : 1,
            "color": options !== undefined && options.color !== undefined ? options.color : "#ffffff",
            "fontSize": options !== undefined ? options.fontSize : undefined,
            "uid": options !== undefined ? options.uid : 1,
            "speed": options !== undefined ? options.speed : undefined
        })

        return _spawn(item, _clockMs)
    }

    function _normalize(it) {
        return {
            "id": it.id !== undefined ? it.id : ("x-" + Math.random()),
            "text": it.text !== undefined ? String(it.text) : "",
            "time": it.time !== undefined ? Number(it.time) : _clockMs / 1000,
            "type": it.type !== undefined ? Number(it.type) : 1,
            "fontSize": it.fontSize,
            "color": it.color !== undefined ? String(it.color) : "#ffffff",
            "userId": it.userId,
            "userName": it.userName,
            "isVip": it.isVip === true,
            "weight": it.weight !== undefined ? Number(it.weight) : 0,
            "speed": it.speed,
            "uid": it.uid
        }
    }

    /*
     * 清屏（对应参考 DanmakuManager.clear()）：只清掉在显示的弹幕 + 重置"已发射"记录，
     * **已载入的弹幕数据保留**（参考里数据在调度器里，clear() 只调 resetEmission()）。
     * 要连数据一起丢就用 loadDanmaku([]) 或 destroy()。
     */
    function clear() {
        _clearActive()

        for (var i = 0; i < _emitted.length; ++i)
            _emitted[i] = false

        _cursor = _lowerBound(Math.max(0, _clockMs / 1000))
    }

    function resize() {
        _rebuildTracks()
    }

    function setSpeed(v) {
        speed = Math.max(1, Math.min(5, Math.round(v)))
    }

    function getSpeed() {
        return speed
    }

    function setArea(v) {
        area = v
        /* 参考：改区域会重建轨道，已存在的弹幕保持原位置（README:391-394） */
        _rebuildTracks()
    }

    function getArea() {
        return area
    }

    function setFontSize(v) {
        baseFontSize = Math.max(12, Math.min(32, Math.round(v)))
    }

    function getFontSize() {
        return baseFontSize
    }

    function setAutoScale(v) {
        autoScale = v === true
    }

    function getAutoScale() {
        return autoScale
    }

    function setFilter(f) {
        filter = f !== undefined && f !== null ? f : ({})
    }

    function getFilter() {
        return filter
    }

    function resetFilter() {
        filter = ({})
    }

    function setBottomSafeArea(h) {
        bottomSafeArea = Math.max(0, h)
    }

    function getTrackInfo() {
        return {
            "count": _tracks.length,
            "height": trackHeight,
            "screenMode": fullscreen ? "fullscreen" : "normal",
            "containerWidth": width,
            "containerHeight": height
        }
    }

    function getStats() {
        return {
            "renderCount": _active.length,
            "poolTotal": _allViews.length,
            "poolFree": _freeViews.length,
            "poolInUse": _allViews.length - _freeViews.length,
            "trackCount": _tracks.length,
            "pending": _items.length - _cursor
        }
    }

    function updateConfig(cfg) {
        if (cfg === undefined || cfg === null)
            return

        if (cfg.fontSize !== undefined)
            setFontSize(cfg.fontSize)

        if (cfg.opacity !== undefined)
            danmakuOpacity = Math.max(0, Math.min(1, cfg.opacity))

        if (cfg.area !== undefined)
            setArea(cfg.area)

        if (cfg.speed !== undefined)
            setSpeed(cfg.speed)

        if (cfg.autoScale !== undefined)
            setAutoScale(cfg.autoScale)

        if (cfg.trackHeight !== undefined)
            trackHeight = cfg.trackHeight

        if (cfg.trackGap !== undefined)
            trackGap = cfg.trackGap

        if (cfg.topMargin !== undefined)
            topMargin = cfg.topMargin

        if (cfg.bottomMargin !== undefined)
            bottomMargin = cfg.bottomMargin

        if (cfg.bottomSafeArea !== undefined)
            setBottomSafeArea(cfg.bottomSafeArea)

        if (cfg.safeDistance !== undefined)
            safeDistance = cfg.safeDistance

        if (cfg.maxRenderCount !== undefined)
            maxRenderCount = cfg.maxRenderCount

        if (cfg.segmentDuration !== undefined)
            segmentDuration = cfg.segmentDuration

        _rebuildSegments()
        _rebuildTracks()
    }

    /*
     * 造一批测试弹幕（对应参考的 utils/danmakuGenerator，那边是独立工具，
     * 这里给个同功能的小生成器，方便没有弹幕数据源时直接看效果）。
     */
    function generateSample(count, durationSec, seed) {
        var words = ["2333333", "前方高能", "哈哈哈哈", "这也太强了吧", "名场面", "awsl",
                     "第一次看哭了", "弹幕护体", "此处应有掌声", "爷青回", "好活", "泪目",
                     "打卡", "BGM 神了", "有一起看的吗", "截图了", "这波操作可以", "经典"]
        var colors = ["#ffffff", "#ffffff", "#ffffff", "#ff0000", "#00ff00", "#ffff00", "#00a1d6", "#ff8800"]
        var list = []
        var rnd = seed !== undefined ? seed : 20240601

        function next() {
            rnd = (rnd * 1103515245 + 12345) % 2147483648
            return rnd / 2147483648
        }

        var total = count !== undefined && count > 0 ? count : 200
        var dur = durationSec !== undefined && durationSec > 0 ? durationSec : 120

        for (var i = 0; i < total; ++i) {
            var r = next()
            var type = r < 0.86 ? 1 : (r < 0.93 ? 2 : 3)

            list.push({
                "id": i,
                "text": words[Math.floor(next() * words.length)],
                "time": Math.round(next() * dur * 100) / 100,
                "type": type,
                "fontSize": 18,
                "color": colors[Math.floor(next() * colors.length)],
                "userId": "u" + Math.floor(next() * 9999),
                "userName": "用户" + Math.floor(next() * 9999),
                "isVip": next() < 0.05,
                "weight": 0,
                "speed": 3,
                "uid": next() < 0.03 ? 1 : 0
            })
        }

        loadDanmaku(list)
        return list.length
    }

    /* destroy：池和视图都归 Qt 管；这里只把数据放掉（对应参考的 destroy） */
    function destroy() {
        clear()

        for (var i = 0; i < _allViews.length; ++i)
            _allViews[i].destroy()

        _allViews = []
        _freeViews = []
    }

    /* TODO（下一件）：防挡遮罩 —— 参考 domEngine.ts:150-316 用 mask-image 三层
       （两个渐变 + 镂空 PNG，contain 居中，留白区用渐变补齐）实现"弹幕避开人物"。
       Qt 这边计划用 ShaderEffectSource + 一层 mask 贴图合成，接口按参考的
       setMaskConfig / enableMask / disableMask / maskLoader(时间→PNG) 来。 */
}
