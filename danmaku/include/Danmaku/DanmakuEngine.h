// ===========================================================================
// Danmaku —— 弹幕引擎（纯逻辑，不画东西）
//
// 用法（每帧一次，宿主自己驱动播放时间）：
//
//     DanmakuEngine engine;
//     engine.setMeasureFunction([](const std::string &t, int px, bool b) { ... }); // C++11 可用普通函数
//     DanmakuConfig cfg = engine.config();
//     cfg.containerWidth  = 1920;
//     cfg.containerHeight = 1080;
//     engine.setConfig(cfg);
//
//     engine.load(items);              // 数据（按 timeMs 排序，引擎内部再排一次）
//
//     // 每帧：
//     engine.update(playerPositionMs); // 推进时钟：退场 + 准入 + 分轨 + 算位置
//     for (const RenderItem &it : engine.activeItems())
//         renderer.draw(it);           // 宿主只管画
//
// 语义要点（细节与数值出处见 .cpp 与 README）：
//   * 滚动：distance = 容器宽 + 弹幕宽，基础速度 150px/s × 档位倍率（domEngine.ts:543-549）
//   * 固定：4000ms，0→10% 淡入放大、10→90% 停留、90→100% 淡出到 0.9（domEngine.ts:361-378）
//   * 轨道：初始只开 70%，不够时 +5/+4 交替扩；碰撞判定 = 右边缘 + 动态安全距离（150/100/60/40/20）
//   * 顶部固定只用前 1/3 轨道；底部固定不用最下面 20%（trackManager.ts:329-387）
//   * 轨道复用：固定弹幕剩余 <= 500ms 就不算占位
//   * seek：清场 + 定位游标（不重建数据、不刷分配）；回退超过 50ms 重置发射标记
//
// 线程模型：单线程。所有状态都是成员变量，没有全局/静态可变状态，多个实例互不干扰。
// 异常：公开接口内部把异常全部吞掉（包括宿主测量回调抛出的），不会让异常穿出 API。
// ===========================================================================
#ifndef DANMAKU_ENGINE_H
#define DANMAKU_ENGINE_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include <Danmaku/DanmakuExport.h>
#include <Danmaku/DanmakuTypes.h>

class DANMAKU_API DanmakuEngine {
public:
    /** 文字测量回调；不设置就用内置兜底（码点数 × 字号 × 0.6） */
    typedef DanmakuMeasureFn MeasureFn;
    /** 统计结构（DanmakuStats 的别名） */
    typedef DanmakuStats Stats;

    DanmakuEngine();
    ~DanmakuEngine();

    /* ------------------------------------------------------------------
     * 配置
     * ------------------------------------------------------------------ */

    /** 整体替换配置。会重建分段表与轨道（轨道重建后旧记录不再占位，
     *  与 QML:923-926 换尺寸时的语义一致），但**不会**改动在屏弹幕的尺寸。 */
    void setConfig(const DanmakuConfig &config);
    /** 当前配置（引用，拷贝一份再改） */
    const DanmakuConfig &config() const;

    /** 改弹幕层尺寸：重建轨道 + 实时重算在屏弹幕字号/宽度（QML:928-972）。
     *  等价于改 config 里的 containerWidth/Height 再把 config 交回来。 */
    void setContainerSize(float width, float height);

    /* ------------------------------------------------------------------
     * 数据
     * ------------------------------------------------------------------ */

    /** 清屏：只清在屏弹幕 + 重置发射记录，**已载入的数据保留**
     *  （QML:1174-1181 对应参考 DanmakuManager.clear()）。
     *  要连数据一起丢用 reset() 或 load(空)。 */
    void clear();

    /** 清屏 + 丢掉全部数据与 id 分配（对应 QML 的 destroy 里的数据部分） */
    void reset();

    /** 载入一批数据：内部拷贝 → timeMs 升序稳定排序 → 建分段表。
     *  游标从"当前时钟"开始，不是在视频播到一半时载入会把之前的弹幕一次喷完。 */
    void load(const std::vector<DanmakuItem> &items);

    /** 追加一条数据（按 timeMs 插到正确位置，游标与发射标记一起修正）。
     *  时间递增的追加是 O(1) 尾插，不会让在屏弹幕失真。 */
    void append(const DanmakuItem &item);

    /** 已载入数据条数 */
    size_t itemCount() const;
    /** 按下标取数据（越界返回 0） */
    const DanmakuItem *itemAt(size_t index) const;

    /* ------------------------------------------------------------------
     * 时间
     * ------------------------------------------------------------------ */

    /** 直接跳到某个播放时间（毫秒）：清场 + 重定位游标 + 重置该时刻之后的发射标记。
     *  **不发射**任何弹幕，下一次 update() 才会准入到点的那些（与 QML 跳转后下一帧
     *  才 _emitDue 的时序一致）。前进/后退都支持。 */
    void seekTo(int64_t timeMs);

    /** 推进到播放时间 timeMs（毫秒），每帧调一次：
     *  退场到期弹幕 → 准入到点弹幕（一帧最多 maxEmitsPerFrame 条）→ 分轨 → 算位置/透明度。 */
    void update(int64_t timeMs);

    /** 当前引擎时钟（毫秒） */
    int64_t clockMs() const;

    /* ------------------------------------------------------------------
     * 渲染输出
     * ------------------------------------------------------------------ */

    /** 现在该画的弹幕。顺序 = 准入顺序（先准入的在前，后准入的在上层）。
     *  返回的是引擎成员向量的引用，内容在下次 update() 前有效，不每帧新建。 */
    const std::vector<RenderItem> &activeItems() const;

    /** 本次 update()/seekTo()/clear() 里退场的 id 列表：渲染层拿它释放
     *  自己按 id 缓存的文字/贴图资源。每次 update() 开头会被清空。 */
    const std::vector<int64_t> &retiredIds() const;

    /* ------------------------------------------------------------------
     * 文字测量
     * ------------------------------------------------------------------ */

    /** 接入宿主的文字度量（可随时替换；也可以不接，走内置兜底） */
    void setMeasureFunction(MeasureFn fn);

    /** 直接指定某条弹幕的文字宽度（像素，不含内边距/边框）。
     *  优先级高于回调：宿主提前量好宽度时可以省掉一次同步测量。
     *  id 传 0 无效（0 是"引擎分配"的哨兵）。 */
    void setMeasuredWidth(int64_t id, float widthPx);

    /** 取消一条的手工宽度 */
    void clearMeasuredWidth(int64_t id);

    /** 取消所有手工宽度 */
    void clearMeasuredWidths();

    /** 按当前测量回调算一次文字宽度（0 表示测不出来；宿主要模板测量时可用） */
    float measureText(const std::string &text, int fontPixelSize, bool bold) const;

    /* ------------------------------------------------------------------
     * 暂停 / 悬停
     * ------------------------------------------------------------------ */

    /** 全局暂停：update() 不再推进时钟（连 seek 判定以外的变化都忽略），
     *  所以在屏弹幕原地停住（QML 是播放器暂停 → 时钟停走）。 */
    void setPaused(bool paused);
    bool isPaused() const;

    /** 喂鼠标位置（弹幕层内坐标，左上角为原点）；inside=false 表示鼠标不在层里。
     *  只有位置真的变了才重做命中检测（一帧最多一次），与 QML:1028-1040 一致：
     *  弹幕自己从静止的光标底下划过去不算悬停。 */
    void setPointerPosition(float x, float y, bool inside);

    /** 当前悬停的弹幕 id；0 = 没有 */
    int64_t hoveredId() const;

    /* ------------------------------------------------------------------
     * 过滤
     * ------------------------------------------------------------------ */

    /** 设置过滤器：立即生效，只影响新准入的弹幕（types.ts:55-62 / QML:534-557） */
    void setFilter(const DanmakuFilter &filter);
    /** 当前过滤器 */
    const DanmakuFilter &filter() const;
    /** 清空过滤器（等价 setFilter(DanmakuFilter())） */
    void resetFilter();

    /* ------------------------------------------------------------------
     * 立即显示 / 发送
     * ------------------------------------------------------------------ */

    /** 立刻显示一条（对应 QML 的 addDanmaku）：不进调度数据，直接占轨道上屏。
     *  currentMs <= 0 时用当前时钟。返回是否成功（被过滤/没轨道/到上限 → false）。 */
    bool addDanmaku(const DanmakuItem &item, int64_t currentMs = -1);

    /** 立刻显示一批，返回成功条数 */
    int addDanmakuBatch(const std::vector<DanmakuItem> &items, int64_t currentMs = -1);

    /** 用户发弹幕（对应 QML:1137-1150 的 sendDanmaku：按 options 造一条并立刻显示） */
    bool sendDanmaku(const std::string &text, const DanmakuSendOptions &options = DanmakuSendOptions());

    /*
     * 撤回/移除一条**正在屏上**的弹幕（悬停气泡里那颗"撤回"按钮用）。
     *
     * 语义照参考：index.ts:473 的 `removeDanmaku(renderId)` → domEngine 把那条渲染项摘掉。
     * 所以这里只让它从**活动列表**退场（轨道计数归还、进 retiredIds 让渲染层丢掉它），
     * 调度数据（_items）不动 —— 往回拖进度条它还会照常出现，和参考一致。
     * id 是 RenderItem::id（= 悬停时 hoveredId() 给的那个）。返回是否真的移除了。
     */
    bool removeDanmaku(int64_t id);

    /* ------------------------------------------------------------------
     * 轨道 / 统计 / 分段
     * ------------------------------------------------------------------ */

    /** 当前轨道数 */
    int trackCount() const;
    /** 某条轨道的 y（越界返回 -1） */
    float trackY(int index) const;

    /** 统计快照 */
    Stats stats() const;

    /** 某个播放时间落在第几段（segmentDurationSec 一段） */
    int segmentIndexForTimeMs(int64_t timeMs) const;
    /** 分段数（分段起点表长度） */
    int segmentCount() const;
    /** 第 segmentIndex 段对应的数据区间 [begin, end)；越界会被夹到有效范围，
     *  分段表为空时返回 false */
    bool segmentRange(int segmentIndex, size_t &begin, size_t &end) const;

    /* ------------------------------------------------------------------
     * 工具 / 维护
     * ------------------------------------------------------------------ */

    /** 当前字号（autoScale 后的基准字号，新弹幕没有自带字号时用它） */
    int currentFontSize() const;

    /** 自动缩放比例 clamp(弹幕层宽 / autoScaleBaseWidth, autoScaleMin, autoScaleMax)。
     *  注意：这里**不含**参考的 dpr^-0.3 那一项（见 QML:147-158 的说明）。 */
    float autoScaleFactor() const;

    /** 按当前尺寸/区域重建轨道（换尺寸、换区域档位时用；QML:923-926 的 resize） */
    void rebuildTracks();

    /** 把在屏弹幕按当前缩放比例实时重算字号/宽度（固定弹幕重新居中）。
     *  参考的语义是"只影响新弹幕"，QML 按用户要求改成了实时，这里跟 QML。 */
    void rescaleActive();

private:
    /* 非拷贝：引擎持有大量内部索引，拷贝没有意义 */
    DanmakuEngine(const DanmakuEngine &);
    DanmakuEngine &operator=(const DanmakuEngine &);

    /** 数据引用：调度数据里的一条，或"立即显示"的一条 */
    struct ItemRef {
        bool immediate;
        size_t index;
        ItemRef() : immediate(false), index(0) {}
    };

    /** 在屏记录（对应 QML rec + trackManager 的 DanmakuRenderItem） */
    struct ActiveRec {
        ItemRef ref;
        int64_t uid;          /** 弹幕 id（RenderItem::id） */
        int type;
        bool isSelf;
        float textWidth;      /** 文字前进宽度（不含内边距/边框） */
        float padH;
        float padV;
        float borderWidth;
        float width;          /** 整盒宽 */
        float height;         /** 整盒高 */
        int fontSize;
        int64_t startMs;      /** 出生时钟 */
        double durationMs;
        double pauseAccumMs;  /** 已经冻结掉的累计时长 */
        double pauseStartMs;  /** >= 0 表示正在冻结（悬停中） */
        float x;
        float y;
        float baseOpacity;    /** 固定弹幕淡入淡出算出来的透明度（滚动恒 1） */
        float baseScale;
        int trackIndex;       /** -1 = 不占轨道 */
        ActiveRec()
            : uid(0), type(DanmakuTypeScroll), isSelf(false), textWidth(0), padH(0), padV(0),
              borderWidth(0), width(0), height(0), fontSize(0), startMs(0), durationMs(0),
              pauseAccumMs(0), pauseStartMs(-1), x(0), y(0), baseOpacity(1), baseScale(1),
              trackIndex(-1) {}
    };

    /** 轨道（车道）。items 只记条数：逐条扫描在屏记录，换取退场时的零成本压实 */
    struct Track {
        float y;
        int itemCount;
        Track() : y(0), itemCount(0) {}
    };

    /* ---- 内部实现 ---- */
    bool spawn(const ItemRef &ref, int64_t nowMs);
    void emitDue();
    void advance();
    void rebuildRenderOut();
    void rebuildSegments();
    void rebuildTrackList();
    bool extendTracks();
    int pickTrack(const ActiveRec &rec, int64_t nowMs);
    int pickFixedTrack(int type, int64_t nowMs) const;
    bool trackAvailable(int trackIndex, const ActiveRec &rec, int64_t nowMs) const;
    bool fixedTrackAvailable(int trackIndex, int type, int64_t nowMs) const;
    float dynamicSafeDistance() const;
    double elapsedOf(const ActiveRec &rec, int64_t nowMs) const;
    void retireRec(const ActiveRec &rec);
    void clearActive();
    ActiveRec *findActive(int64_t uid);
    const ActiveRec *findActive(int64_t uid) const;
    void applySeek(int64_t timeMs);
    /*
     * seek 之后把"生命周期覆盖当前时刻"的弹幕按进度补上屏（B 站那种观感）。
     * 做法：往回扫一段窗口，正常准入之后把 ActiveRec::startMs **回填成弹幕自己的时间点**，
     * 于是 elapsed = 时钟 − startMs 就是它已经走过的进度，x 自然落在正确位置。
     */
    void reseedActiveAfterSeek(int64_t timeMs);
    void rewindEmission(int64_t timeMs);
    size_t lowerBoundMs(int64_t timeMs) const;
    void syncHover();
    void releaseHoverAt(int64_t resumeMs);
    void forceResumeHover();
    void pruneImmediate();
    const DanmakuItem &itemOf(const ItemRef &ref) const;
    static bool matchesFilter(const DanmakuFilter &filter, const DanmakuItem &item);
    static int normalizeType(int rawType);
    static bool isSelfItem(const DanmakuItem &item);
    float measureBoxTextWidth(const DanmakuItem &item, int fontPixelSize) const;
    void rescaleActiveImpl();
    float areaHeightPx() const;
    float maxTrackCountValue(bool useSafeArea) const;
    /* 轨道几何：按**当前字号**算（字号的 4/3 与配置值取大，见 .cpp 里 kReferenceTrackHeight 那段） */
    float trackHeightForLayout() const;
    float trackStepForLayout() const;
    int64_t segDurationMs() const;
    int64_t allocId();
    bool addImmediate(const DanmakuItem &item, int64_t currentMs, int64_t *outId);
    static int64_t absDiff(int64_t a, int64_t b);

    /* ---- 状态 ---- */
    DanmakuConfig _config;
    DanmakuFilter _filter;

    std::deque<DanmakuItem> _items;        /** 调度数据（按 timeMs 升序） */
    std::vector<unsigned char> _emitted;   /** 与 _items 一一对应的发射标记 */
    std::vector<size_t> _segmentStart;     /** 分段起点表 */
    size_t _cursor;                        /** 发到第几条了 */

    std::deque<DanmakuItem> _immediate;    /** 立刻显示的弹幕（不进 _items） */

    int64_t _clockMs;                      /** 引擎时钟（毫秒） */
    int64_t _lastSyncedMs;                 /** 上次见到的播放时间（判 seek），-1 = 还没同步过 */

    std::vector<ActiveRec> _active;        /** 在屏记录（准入顺序） */
    std::vector<Track> _tracks;            /** 轨道 */
    int _extendCount;                      /** 已扩展次数（+5/+4 交替） */

    std::vector<RenderItem> _renderOut;    /** activeItems() 的输出（复用，不每帧新建） */
    std::vector<int64_t> _retired;         /** 本次退场的 id */

    DanmakuMeasureFn _measure;             /** 宿主测量回调 */
    std::map<int64_t, float> _measuredWidths; /** 手工指定的宽度 */

    bool _paused;

    /* 有效指针（命中检测用） */
    float _pointerX;
    float _pointerY;
    bool _pointerInside;
    /* 宿主最近一次报上来的值（去重用，与 QML 的属性去重等价） */
    float _reportedX;
    float _reportedY;
    bool _reportedInside;
    bool _hoverDirty;

    int64_t _hoveredUid;                   /** 0 = 没有悬停目标 */
    int64_t _hoverStartMs;                 /** 悬停开始的时钟（自动恢复按它算） */

    int64_t _nextAutoId;                   /** 自分配 id 计数器 */
    int64_t _admitted;                     /** 累计准入 */
    int64_t _dropped;                      /** 累计丢弃 */
};

#endif // DANMAKU_ENGINE_H
