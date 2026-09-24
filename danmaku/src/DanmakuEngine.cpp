// ===========================================================================
// Danmaku —— 引擎实现
//
// 逐段对应 QML（DanmakuLayer.qml）里的同名函数，数值出处标在注释里。
// 设计约束：不读墙钟、不用随机数、没有全局/静态可变状态、每帧不新建容器。
// ===========================================================================
#include <Danmaku/DanmakuEngine.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
/* 本库是**独立纯 C++**（不依赖框架的 AF_LOG*），所以诊断日志只能用标准 C 的 stderr */
#include <cstdio>

namespace {

/* 内置兜底速度（配置里 scrollSpeedPxPerSec <= 0 时用） */
const float kFallbackScrollSpeed = 150.0f;

/* 动态安全距离分档（trackManager.ts:263-273 / QML:177-189）：
 * 密度 <=1 → 150，<=5 → 100，<=15 → 60，<=25 → 40，否则 20 */
const double kSafeDistanceLevels[5] = {150.0, 100.0, 60.0, 40.0, 20.0};

/* 固定弹幕动画分界（QML:832-841） */
const double kFixedFadeInEnd = 0.1;
const double kFixedFadeOutStart = 0.9;
const float kFixedStartScale = 0.8f;
const float kFixedEndScale = 0.9f;

/* 轨道数量上限的防御性夹取（配置异常时不至于建几十万条轨道） */
const int kMaxTracksHard = 4096;

/** Math.round 的等价实现（正数；QML 用的是 Math.round） */
inline int roundToInt(float v) {
    return (int)std::floor((double)v + 0.5);
}

inline double clampDouble(double v, double lo, double hi) {
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/*
 * 轨道几何（高 / 步长）**必须跟着字号走**，不能只读参考的常量。
 *
 * 两套参考的数值：
 *   * hili-player 的 trackManager.ts:45-46：trackHeight 24 / trackGap 4，配 types/index.ts:120 的
 *     fontSize 18（24 = 18 × 4/3，步长 28 = 字号 × 1.56）；
 *   * halihili-player 的 rowdm/index.ts:141：`--top = index * 30 + 10`，**行步长固定 30px**
 *     （配它默认的 25px 字号 = 1.2×）。
 * 而本项目的弹幕盒是"字号 + 上下内边距 4px"、外面还有描边/重墨，按 1.2× 排上下两行会贴在一起
 * （用户实测"轨道还是挨着的"）。
 *
 * 所以这里按 **1.8×字号** 留出看得见的间距，并且**随字号等比变化** —— 字号里含
 * "容器宽 / 1280" 的自动缩放（scaleHelper.ts:87 同一套），于是不同播放器尺寸/全屏下间距
 * 自然不同（用户要求："不同尺寸下间距也不一样"）。行的"高度"按 1.25×字号（含描边余量）算。
 */
const float kTrackHeightFontRatio = 1.25f;   /* 一行的高度 / 字号 */
const float kTrackStepFontRatio = 1.8f;      /* 行步长 / 字号（= 行高 + 可视间距） */

/*
 * seek 后"补上屏"往回扫多长一段。
 * 滚动弹幕的生命周期 = (容器宽 + 盒宽) / 速度（几秒量级），固定弹幕 4 秒；15 秒足够覆盖，
 * 再往前的即使还在飞也早就该退场了，多扫只是浪费。
 */
const int64_t kReseedWindowMs = 15000;

/** UTF-8 码点数量（兜底测量用；JS 那边 String.length 数的是 UTF-16 单元，
 *  中文一个字算 1，这里按码点算才等价，所以不能直接用 std::string::size()） */
inline size_t utf8Length(const std::string &s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xC0u) != 0x80u)
            ++n;
    }
    return n;
}

/** load() 用的稳定排序比较：timeMs 升序（同刻保持输入顺序 → 可复现） */
inline bool timeLess(const DanmakuItem &a, const DanmakuItem &b) {
    return a.timeMs < b.timeMs;
}

} // namespace

/* ===========================================================================
 * 构造 / 析构
 * =========================================================================== */
DanmakuEngine::DanmakuEngine()
    : _config(),
      _filter(),
      _cursor(0),
      _clockMs(0),
      _lastSyncedMs(-1),
      _extendCount(0),
      _paused(false),
      _pointerX(-1.0f),
      _pointerY(-1.0f),
      _pointerInside(false),
      _reportedX(-1.0f),
      _reportedY(-1.0f),
      _reportedInside(false),
      _hoverDirty(false),
      _hoveredUid(0),
      _hoverStartMs(0),
      _nextAutoId(0),
      _admitted(0),
      _dropped(0)
{
    rebuildTrackList();
    rebuildSegments();
}

DanmakuEngine::~DanmakuEngine() {
}

/* ===========================================================================
 * 配置
 * =========================================================================== */
void DanmakuEngine::setConfig(const DanmakuConfig &config) {
    _config = config;
    try {
        rebuildSegments();
        rebuildTrackList();
    } catch (...) {
    }
    rebuildRenderOut();
}

const DanmakuConfig &DanmakuEngine::config() const {
    return _config;
}

void DanmakuEngine::setContainerSize(float width, float height) {
    if (width > 0.0f)
        _config.containerWidth = width;
    if (height > 0.0f)
        _config.containerHeight = height;

    /* QML:923-926 宽高变化 → 重建轨道；QML:929 自动缩放比例变化 → 在屏弹幕实时重算 */
    try {
        rebuildTrackList();
    } catch (...) {
    }

    /*
     * ★ 尺寸一变，量宽缓存必须作废 ★
     *
     * 字号里含"自动缩放比例"（容器宽 / 设计稿宽 1280，夹在 0.75~1.5），所以窗口/全屏一变，
     * 每条弹幕的字号都要跟着变 —— 而 measureBoxTextWidth() 是**按弹幕 id 缓存**的，
     * 缓存里不含字号：不清的话 rescaleActive() 会把字号改大、盒子宽度却还是旧值，
     * 表现就是"窗口一变，白框和文字对不上 / 缩放看起来没生效"。
     * （和改字号设置时是同一个坑，那边在 DanmakuController 里清，这里必须在引擎里清 ——
     *   改窗口尺寸走的是 setContainerSize，不经过控制器的那些 setter。）
     */
    clearMeasuredWidths();

    rescaleActive();
    rebuildRenderOut();
}

/* ===========================================================================
 * 数据
 * =========================================================================== */
void DanmakuEngine::clear() {
    try {
        _retired.clear();
        clearActive();
        for (size_t i = 0; i < _emitted.size(); ++i)
            _emitted[i] = 0;

        /* QML:1180 游标跟当前时钟走 */
        int64_t from = _clockMs > 0 ? _clockMs : 0;
        _cursor = lowerBoundMs(from);
    } catch (...) {
    }
    rebuildRenderOut();
}

void DanmakuEngine::reset() {
    try {
        _retired.clear();
        clearActive();
        _immediate.clear();
        _items.clear();
        _emitted.clear();
        _segmentStart.clear();
        _cursor = 0;
        _hoveredUid = 0;
    } catch (...) {
    }
    rebuildRenderOut();
}

void DanmakuEngine::load(const std::vector<DanmakuItem> &items) {
    try {
        _retired.clear();
        clearActive();
        _immediate.clear();
        _items.clear();
        _emitted.clear();

        for (size_t i = 0; i < items.size(); ++i) {
            DanmakuItem copy = items[i];
            if (copy.id == 0)
                copy.id = allocId();
            _items.push_back(copy);
        }

        std::stable_sort(_items.begin(), _items.end(), timeLess);
        _emitted.assign(_items.size(), (unsigned char)0);
        rebuildSegments();

        /* QML:286 游标从当前播放位置开始：播到一半才载入数据，不会把之前的弹幕一次喷完 */
        int64_t from = _clockMs > 0 ? _clockMs : 0;
        _cursor = lowerBoundMs(from);
        _hoveredUid = 0;
        rebuildRenderOut();
    } catch (...) {
        /* 公开接口不让异常外泄（最坏情况：这一批数据没载进来） */
    }
}

void DanmakuEngine::append(const DanmakuItem &item) {
    try {
        DanmakuItem copy = item;
        if (copy.id == 0)
            copy.id = allocId();

        if (_items.empty() || copy.timeMs >= _items.back().timeMs) {
            /* 常见情况：时间递增 → 尾插（deque 尾插不会让已有元素的地址失效） */
            _items.push_back(copy);
            _emitted.push_back((unsigned char)0);
        } else {
            /* 插到中间：游标、发射标记、在屏记录的下标一起顺移 */
            size_t pos = 0;
            while (pos < _items.size() && _items[pos].timeMs <= copy.timeMs)
                ++pos;

            _items.insert(_items.begin() + (std::ptrdiff_t)pos, copy);
            _emitted.insert(_emitted.begin() + (std::ptrdiff_t)pos, (unsigned char)0);

            if (_cursor >= pos)
                ++_cursor;

            for (size_t i = 0; i < _active.size(); ++i) {
                if (!_active[i].ref.immediate && _active[i].ref.index >= pos)
                    ++_active[i].ref.index;
            }
        }

        rebuildSegments();
        rebuildRenderOut();
    } catch (...) {
    }
}

size_t DanmakuEngine::itemCount() const {
    return _items.size();
}

const DanmakuItem *DanmakuEngine::itemAt(size_t index) const {
    if (index >= _items.size())
        return 0;
    return &_items[index];
}

/* ===========================================================================
 * 时间
 * =========================================================================== */
void DanmakuEngine::seekTo(int64_t timeMs) {
    try {
        _retired.clear();
        applySeek(timeMs);
        _lastSyncedMs = _clockMs;
    } catch (...) {
    }
    rebuildRenderOut();
}

void DanmakuEngine::update(int64_t timeMs) {
    try {
        _retired.clear();
        pruneImmediate();

        const int64_t prev = _lastSyncedMs;
        const bool jumped = (prev >= 0 && absDiff(timeMs, prev) > _config.jumpThresholdMs);
        const bool rewound = (prev >= 0 && (prev - timeMs) > _config.rewindToleranceMs);

        if (_paused) {
            /* 暂停：时钟不动（QML 里播放器暂停 → _step 不推时钟）；只有大跳变才当 seek */
            if (jumped)
                applySeek(timeMs);
        } else {
            if (jumped) {
                /* QML:887-896 跳转：清场重来 + 游标按新位置定位 + 重置发射标记 */
                applySeek(timeMs);
            } else {
                /* QML:898-906 小幅回退：不清场，只把这一时刻之后的发射标记放掉 */
                if (rewound)
                    rewindEmission(timeMs);
                _clockMs = timeMs;
            }
        }

        _lastSyncedMs = timeMs;

        emitDue();
        advance();
        rebuildRenderOut();
    } catch (...) {
        /* 公开接口不让异常外泄（最坏情况：这一帧少走一点） */
    }
}

int64_t DanmakuEngine::clockMs() const {
    return _clockMs;
}

/* ===========================================================================
 * 渲染输出
 * =========================================================================== */
const std::vector<RenderItem> &DanmakuEngine::activeItems() const {
    return _renderOut;
}

const std::vector<int64_t> &DanmakuEngine::retiredIds() const {
    return _retired;
}

/* ===========================================================================
 * 文字测量
 * =========================================================================== */
void DanmakuEngine::setMeasureFunction(MeasureFn fn) {
    try {
        _measure = fn;
    } catch (...) {
    }
}

void DanmakuEngine::setMeasuredWidth(int64_t id, float widthPx) {
    if (id == 0)
        return;
    try {
        if (widthPx > 0.0f)
            _measuredWidths[id] = widthPx;
        else
            _measuredWidths.erase(id);
    } catch (...) {
    }
}

void DanmakuEngine::clearMeasuredWidth(int64_t id) {
    _measuredWidths.erase(id);
}

void DanmakuEngine::clearMeasuredWidths() {
    _measuredWidths.clear();
}

float DanmakuEngine::measureText(const std::string &text, int fontPixelSize, bool bold) const {
    if (_measure) {
        try {
            float w = _measure(text, fontPixelSize, bold);
            if (w > 0.0f)
                return w;
        } catch (...) {
            /* 宿主的测量回调抛异常：当作测不出来，交给兜底 */
        }
    }
    return 0.0f;
}

/* ===========================================================================
 * 暂停 / 悬停
 * =========================================================================== */
void DanmakuEngine::setPaused(bool paused) {
    _paused = paused;
}

bool DanmakuEngine::isPaused() const {
    return _paused;
}

void DanmakuEngine::setPointerPosition(float x, float y, bool inside) {
    /* 与 QML 的属性去重等价：宿主重复报同一个位置不算"鼠标动了" */
    if (x == _reportedX && y == _reportedY && inside == _reportedInside)
        return;

    _reportedX = x;
    _reportedY = y;
    _reportedInside = inside;

    if (inside) {
        _pointerX = x;
        _pointerY = y;
        _pointerInside = true;
    } else {
        _pointerX = -1.0f;
        _pointerY = -1.0f;
        _pointerInside = false;
    }

    /* 只打脏标记，命中检测放到下一帧末尾做（QML:1028-1040） */
    _hoverDirty = true;
}

int64_t DanmakuEngine::hoveredId() const {
    return _hoveredUid;
}

/* ===========================================================================
 * 过滤
 * =========================================================================== */
void DanmakuEngine::setFilter(const DanmakuFilter &filter) {
    _filter = filter;
}

const DanmakuFilter &DanmakuEngine::filter() const {
    return _filter;
}

void DanmakuEngine::resetFilter() {
    _filter = DanmakuFilter();
}

/* ===========================================================================
 * 立即显示 / 发送
 * =========================================================================== */
bool DanmakuEngine::addDanmaku(const DanmakuItem &item, int64_t currentMs) {
    try {
        int64_t id = 0;
        return addImmediate(item, currentMs, &id);
    } catch (...) {
        return false;
    }
}

int DanmakuEngine::addDanmakuBatch(const std::vector<DanmakuItem> &items, int64_t currentMs) {
    int ok = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        if (addDanmaku(items[i], currentMs))
            ++ok;
    }
    return ok;
}

bool DanmakuEngine::sendDanmaku(const std::string &text, const DanmakuSendOptions &options) {
    try {
        DanmakuItem item;
        item.text = text;
        item.type = options.type;
        item.fontSize = options.fontSize;
        item.speed = options.speed;
        item.colorRGBA = options.colorRGBA;
        item.isSelf = options.self;
        item.uid = options.uid;
        item.id = options.id;
        item.timeMs = (options.timeMs >= 0) ? options.timeMs : _clockMs;

        /* QML:1149 sendDanmaku 也是直接 _spawn(item, _clockMs)：不进调度数据，立刻上屏 */
        int64_t id = 0;
        return addImmediate(item, _clockMs, &id);
    } catch (...) {
        return false;
    }
}

/* ===========================================================================
 * 轨道 / 统计 / 分段
 * =========================================================================== */
int DanmakuEngine::trackCount() const {
    return (int)_tracks.size();
}

float DanmakuEngine::trackY(int index) const {
    if (index < 0 || index >= (int)_tracks.size())
        return -1.0f;
    return _tracks[(size_t)index].y;
}

DanmakuStats DanmakuEngine::stats() const {
    DanmakuStats s;
    s.activeCount = (int)_active.size();
    s.trackCount = (int)_tracks.size();
    s.maxTrackCount = (int)maxTrackCountValue(false);
    s.admitted = (int)_admitted;
    s.dropped = (int)_dropped;
    s.pending = (_cursor < _items.size()) ? (int)(_items.size() - _cursor) : 0;
    return s;
}

int DanmakuEngine::segmentIndexForTimeMs(int64_t timeMs) const {
    int64_t dur = segDurationMs();
    if (dur <= 0)
        dur = 30000;
    double idx = std::floor((double)timeMs / (double)dur);
    if (idx < 0.0)
        idx = 0.0;
    return (int)idx;
}

int DanmakuEngine::segmentCount() const {
    return (int)_segmentStart.size();
}

bool DanmakuEngine::segmentRange(int segmentIndex, size_t &begin, size_t &end) const {
    if (_segmentStart.empty() || segmentIndex < 0)
        return false;

    int s = segmentIndex;
    if (s > (int)_segmentStart.size() - 1)
        s = (int)_segmentStart.size() - 1;

    begin = _segmentStart[(size_t)s];
    end = (s + 1 < (int)_segmentStart.size()) ? _segmentStart[(size_t)(s + 1)] : _items.size();
    return true;
}

/* ===========================================================================
 * 工具 / 维护
 * =========================================================================== */
int DanmakuEngine::currentFontSize() const {
    /* QML:139-145 */
    float base = (float)_config.baseFontSize * _config.fontSizeScale;
    float v = _config.autoScale ? (base * autoScaleFactor()) : base;
    int r = roundToInt(v);
    /* QML 是 Math.max(12, Math.min(36, ...))：先夹上限再夹下限 */
    if (r > _config.fontSizeMax)
        r = _config.fontSizeMax;
    if (r < _config.fontSizeMin)
        r = _config.fontSizeMin;
    if (r < 1)
        r = 1;
    return r;
}

float DanmakuEngine::trackHeightForLayout() const {
    /* 见文件头那段：一行的高度按"字号 × 1.25"（含描边余量），配置值只是下限。 */
    float byFont = (float)currentFontSize() * kTrackHeightFontRatio;
    float h = _config.trackHeight;
    if (byFont > h)
        h = byFont;
    return h > 0.0f ? h : 0.0f;
}

float DanmakuEngine::trackStepForLayout() const {
    /*
     * 步长 = max(行高 + trackGap, 字号 × 1.8)：保证上下两行之间有看得见的间距，
     * 而且**跟着字号变**（字号跟着容器宽变 → 不同窗口/全屏尺寸下间距不同）。
     * 兜底：> 0 以免除零/死循环。
     */
    float byFont = (float)currentFontSize() * kTrackStepFontRatio;
    float step = trackHeightForLayout() + _config.trackGap;

    if (byFont > step)
        step = byFont;

    if (!(step > 0.5f))
        step = 1.0f;

    return step;
}

float DanmakuEngine::autoScaleFactor() const {
    /* QML:159-162：只按层宽算，夹在 [autoScaleMin, autoScaleMax]。
     * 参考的 dpr^-0.3 那一项被 QML 去掉了（逻辑像素已经含系统缩放，会双重缩小），这里跟 QML。 */
    float base = _config.autoScaleBaseWidth > 0.0f ? _config.autoScaleBaseWidth : 1280.0f;
    float w = _config.containerWidth > 0.0f ? _config.containerWidth : base;
    float f = w / base;
    if (f < _config.autoScaleMin)
        f = _config.autoScaleMin;
    if (f > _config.autoScaleMax)
        f = _config.autoScaleMax;
    return f;
}

void DanmakuEngine::rebuildTracks() {
    try {
        rebuildTrackList();
    } catch (...) {
    }
    rebuildRenderOut();
}

void DanmakuEngine::rescaleActive() {
    try {
        rescaleActiveImpl();
    } catch (...) {
    }
    rebuildRenderOut();
}

void DanmakuEngine::rescaleActiveImpl() {
    if (_active.empty())
        return;

    const float scaleAll = _config.fontSizeScale * (_config.autoScale ? autoScaleFactor() : 1.0f);

    for (size_t i = 0; i < _active.size(); ++i) {
        ActiveRec &rec = _active[i];
        const DanmakuItem &src = itemOf(rec.ref);

        int newSize = (src.fontSize > 0) ? roundToInt((float)src.fontSize * scaleAll) : currentFontSize();
        if (newSize < 1)
            newSize = 1;

        if (newSize == rec.fontSize)
            continue;

        const bool isFixed = (rec.type == DanmakuTypeTop || rec.type == DanmakuTypeBottom);
        const float padH = isFixed ? _config.fixedPadH : (rec.isSelf ? _config.selfPadH : 0.0f);
        const float padV = (isFixed || rec.isSelf) ? _config.contentPadV : 0.0f;
        const float borderW = rec.isSelf ? _config.selfBorderWidth : 0.0f;
        const float textW = measureBoxTextWidth(src, newSize);

        rec.fontSize = newSize;
        rec.textWidth = textW;
        rec.padH = padH;
        rec.padV = padV;
        rec.borderWidth = borderW;
        rec.width = textW + 2.0f * (padH + borderW);
        rec.height = (float)newSize + 2.0f * (padV + borderW);

        if (isFixed) {
            /* 宽度变了，固定弹幕要重新水平居中（QML:967-970） */
            rec.x = (float)roundToInt((_config.containerWidth - rec.width) * 0.5f);
        }
    }
}

/* ===========================================================================
 * 内部：数据引用
 * =========================================================================== */
const DanmakuItem &DanmakuEngine::itemOf(const ItemRef &ref) const {
    if (ref.immediate)
        return _immediate[ref.index];
    return _items[ref.index];
}

bool DanmakuEngine::matchesFilter(const DanmakuFilter &filter, const DanmakuItem &item) {
    /* QML:534-557 / domEngine.ts:469-496：置 true = 过滤掉 */
    if (filter.scroll && item.type == DanmakuTypeScroll)
        return true;
    if (filter.fixed && (item.type == DanmakuTypeTop || item.type == DanmakuTypeBottom))
        return true;
    if (filter.top && item.type == DanmakuTypeTop)
        return true;
    if (filter.bottom && item.type == DanmakuTypeBottom)
        return true;
    if (filter.advanced && item.type == DanmakuTypeAdvanced)
        return true;
    if (filter.colorful && !danmakuColorIsWhite(item.colorRGBA))
        return true;
    return false;
}

int DanmakuEngine::normalizeType(int rawType) {
    if (rawType == DanmakuTypeScroll || rawType == DanmakuTypeTop || rawType == DanmakuTypeBottom)
        return rawType;
    return 0;
}

bool DanmakuEngine::isSelfItem(const DanmakuItem &item) {
    return item.isSelf || item.uid == "1";
}

float DanmakuEngine::measureBoxTextWidth(const DanmakuItem &item, int fontPixelSize) const {
    /* 1) 宿主手工指定的宽度优先（原样使用，不再 ceil） */
    if (item.id != 0) {
        std::map<int64_t, float>::const_iterator it = _measuredWidths.find(item.id);
        if (it != _measuredWidths.end() && it->second > 0.0f)
            return it->second;
    }

    /* 2) 宿主测量回调 */
    float w = measureText(item.text, fontPixelSize, _config.fontBold);

    /* 3) 兜底估算（QML:655-666）：最大(字号, 字数 × 字号 × 0.6)，再向上取整 */
    if (!(w > 0.0f)) {
        double est = (double)utf8Length(item.text) * (double)fontPixelSize * 0.6;
        double minW = (double)fontPixelSize;
        if (est < minW)
            est = minW;
        w = (float)est;
    }

    return (float)std::ceil((double)w);
}

int64_t DanmakuEngine::allocId() {
    return ++_nextAutoId;
}

int64_t DanmakuEngine::absDiff(int64_t a, int64_t b) {
    int64_t d = a - b;
    return d < 0 ? -d : d;
}

int64_t DanmakuEngine::segDurationMs() const {
    double s = (_config.segmentDurationSec > 0.0f) ? (double)_config.segmentDurationSec : 30.0;
    double ms = s * 1000.0;
    if (ms < 1.0)
        ms = 1000.0;
    return (int64_t)ms;
}

/* ===========================================================================
 * 内部：几何
 * =========================================================================== */
float DanmakuEngine::areaHeightPx() const {
    float h = _config.containerHeight - _config.areaTop - _config.areaBottom;
    if (h < 0.0f)
        h = 0.0f;

    float ratio = _config.areaRatio;
    if (!(ratio > 0.0f))
        ratio = 1.0f;
    if (ratio > 1.0f)
        ratio = 1.0f;

    return (float)std::floor((double)h * (double)ratio);
}

float DanmakuEngine::maxTrackCountValue(bool useSafeArea) const {
    /* trackManager.ts:78-84 / QML:361-369 */
    float areaHeight = areaHeightPx();
    float available = areaHeight - _config.topMargin - _config.bottomMargin;
    if (useSafeArea)
        available -= _config.bottomSafeArea;

    float v = (float)std::floor((double)available / (double)trackStepForLayout());
    if (v < 1.0f)
        v = 1.0f;
    return v;
}

void DanmakuEngine::rebuildTrackList() {
    /* QML:371-388 */
    _tracks.clear();

    const float areaHeight = areaHeightPx();
    const float maxCount = maxTrackCountValue(true);
    float countF = (float)std::floor((double)maxCount * (double)_config.initialTrackRatio);
    if (countF < 1.0f)
        countF = 1.0f;
    int count = (int)countF;
    if (count > kMaxTracksHard)
        count = kMaxTracksHard;

    const float step = trackStepForLayout();
    const float th = trackHeightForLayout();

    for (int i = 0; i < count; ++i) {
        const float y = _config.topMargin + (float)i * step;
        if (y + th > areaHeight)
            break;
        Track t;
        t.y = y;
        t.itemCount = 0;
        _tracks.push_back(t);
    }

    _extendCount = 0;

    /* 轨道重建后旧记录不再占位（QML 的 _rebuildTracks 也是这个语义：在屏弹幕保持原位置原地飘完） */
    for (size_t i = 0; i < _active.size(); ++i)
        _active[i].trackIndex = -1;
}

bool DanmakuEngine::extendTracks() {
    /* QML:391-416：不够时往下扩，+5 / +4 交替，上限是不减安全区的那份 */
    const float areaHeight = areaHeightPx();
    const float maxCount = maxTrackCountValue(false);

    if ((float)_tracks.size() >= maxCount)
        return false;

    const int step = (_extendCount % 2 == 0) ? 5 : 4;
    float room = maxCount - (float)_tracks.size();
    float wantF = (float)step;
    if (wantF > room)
        wantF = room;

    int want = (int)wantF;
    if (want <= 0)
        return false;
    if ((int)_tracks.size() + want > kMaxTracksHard)
        want = kMaxTracksHard - (int)_tracks.size();
    if (want <= 0)
        return false;

    const float spacing = trackStepForLayout();
    const float th = trackHeightForLayout();

    int added = 0;
    int nextIndex = (int)_tracks.size();
    float y = _config.topMargin + (float)nextIndex * spacing;

    while (added < want && y + th <= areaHeight) {
        Track t;
        t.y = y;
        t.itemCount = 0;
        _tracks.push_back(t);
        y += spacing;
        ++added;
    }

    if (added > 0) {
        ++_extendCount;
        return true;
    }
    return false;
}

float DanmakuEngine::dynamicSafeDistance() const {
    /* trackManager.ts:249-274 / QML:169-190：按当前平均密度分档 */
    int total = 0;
    for (size_t i = 0; i < _tracks.size(); ++i)
        total += _tracks[i].itemCount;

    int trackN = (int)_tracks.size();
    if (trackN < 1)
        trackN = 1;

    const double density = (double)total / (double)trackN;

    if (density <= 1.0)
        return (float)kSafeDistanceLevels[0];
    if (density <= 5.0)
        return (float)kSafeDistanceLevels[1];
    if (density <= 15.0)
        return (float)kSafeDistanceLevels[2];
    if (density <= 25.0)
        return (float)kSafeDistanceLevels[3];
    return (float)kSafeDistanceLevels[4];
}

double DanmakuEngine::elapsedOf(const ActiveRec &rec, int64_t nowMs) const {
    /* QML:675-682：悬停期间冻结 —— 不只扣已结算的 pauseAccum，还要扣正在进行的那段 */
    double paused = rec.pauseAccumMs;
    if (rec.pauseStartMs >= 0.0)
        paused += (double)nowMs - rec.pauseStartMs;

    return (double)(nowMs - rec.startMs) - paused;
}

/* ===========================================================================
 * 内部：轨道判定
 * =========================================================================== */
bool DanmakuEngine::trackAvailable(int trackIndex, const ActiveRec &rec, int64_t nowMs) const {
    /* QML:419-451（trackManager.ts:284-321 的两段式判定） */
    const double safe = (double)dynamicSafeDistance();
    const double w = (double)_config.containerWidth;
    const double newWidth = (double)rec.width;

    for (size_t i = 0; i < _active.size(); ++i) {
        const ActiveRec &other = _active[i];

        if (other.trackIndex != trackIndex)
            continue;

        /*
         * 【★固定弹幕（顶部/底部）也必须挡路 —— 修"滚动弹幕穿过固定弹幕"★】
         *
         * 参考实现这里是 `if (existingItem.type !== SCROLL) continue`（trackManager.ts:294），
         * 也就是"滚动弹幕看不见固定弹幕"：一条滚动弹幕于是会直接从顶部/底部弹幕身上穿过去。
         * 用户实测反馈的"固定弹幕会重叠"里，这一类就是这么来的。
         *
         * 固定弹幕在整条轨道上一直占着（直到它消失），所以这里改成：
         * 固定弹幕还没结束（remaining > 0）→ 这条轨道不给滚动弹幕用。
         * （固定弹幕已经很接近结束时不挡，免得白白空一条轨道。）
         */
        if (other.type != DanmakuTypeScroll) {
            const double fixedRemaining = other.durationMs - elapsedOf(other, nowMs);

            if (fixedRemaining > 0.0)
                return false;

            continue;
        }

        const double elapsed = elapsedOf(other, nowMs);
        const double otherDuration = other.durationMs > 0.0 ? other.durationMs : 1.0;
        const double existingX = w - (w + (double)other.width) * (elapsed / otherDuration);

        /* 还在右侧、离得不够远 → 不能放 */
        if (existingX > w - newWidth - safe * 2.0)
            return false;

        if (existingX + (double)other.width > 0.0) {
            const double remaining = otherDuration - elapsed;
            /* 快出屏了就不算冲突（安全距离 > 80 时 2000ms，否则 1000ms） */
            const double minRemaining = (safe > 80.0) ? 2000.0 : 1000.0;
            if (remaining < minRemaining)
                continue;

            const double recDuration = rec.durationMs > 0.0 ? rec.durationMs : 1.0;
            const double newSpeed = (w + newWidth) / recDuration;
            const double oldSpeed = (w + (double)other.width) / otherDuration;

            /* 新弹幕更快且旧弹幕还在右半边 → 会追尾 */
            if (newSpeed > oldSpeed && existingX > w / 2.0)
                return false;
        }
    }

    return true;
}

bool DanmakuEngine::fixedTrackAvailable(int trackIndex, int type, int64_t nowMs) const {
    /* QML:453-468：固定弹幕占用判定（**已按用户反馈改成"跨类型也占"**，见下） */
    for (size_t i = 0; i < _active.size(); ++i) {
        const ActiveRec &other = _active[i];

        if (other.trackIndex != trackIndex)
            continue;
        /* 只跟"会占住整条轨道"的弹幕比：滚动弹幕一会儿就过去了，不算 */
        if (other.type == DanmakuTypeScroll)
            continue;

        const double elapsed = elapsedOf(other, nowMs);
        const double remaining = other.durationMs - elapsed;

        /*
         * 【★跨类型（底部 vs 顶部）必须严格占满 —— 修"底部压在顶部上"★】
         *
         * 参考实现只跟**同类型**比（trackManager.ts:341/368 的 `existingItem.type === type`），
         * 于是"底部固定"可以被放到"顶部固定"正在用的那条轨道上 —— 两条弹幕画在同一个 y 上，
         * 这就是用户实测到的固定弹幕重叠。
         *
         * 现在：
         *   * 不同类型（底部 对 顶部）：必须等它**完全结束**（remaining > 0 就不放）；
         *   * 同类型：沿用参考的 fixedTrackReuseMs(500ms) 提前复用（密集时不会留下空档）。
         */
        const double grace = (other.type == type) ? (double)_config.fixedTrackReuseMs : 0.0;

        if (remaining > grace)
            return false;
    }
    return true;
}

int DanmakuEngine::pickFixedTrack(int type, int64_t nowMs) const {
    /* QML:500-525 */
    const int trackN = (int)_tracks.size();
    const float bottomBoundary = (float)std::floor((double)_config.containerHeight * 0.8);

    if (type == DanmakuTypeTop) {
        /* 顶部固定：只用前 1/3 的轨道（trackManager.ts:335） */
        const int maxIndex = trackN / 3;
        for (int i = 0; i < maxIndex && i < trackN; ++i) {
            if (fixedTrackAvailable(i, type, nowMs))
                return i;
        }
        return -1;
    }

    if (_config.bottomFixedFromBottom) {
        /* 可选：从最下面一条轨道往上找（默认关，保持与 QML 一致） */
        for (int j = trackN - 1; j >= 0; --j) {
            if (_tracks[(size_t)j].y > bottomBoundary)
                continue;
            if (fixedTrackAvailable(j, type, nowMs))
                return j;
        }
        return -1;
    }

    /* QML:515-524：从上往下找，但不使用最下面 20% 的轨道 */
    for (int j = 0; j < trackN; ++j) {
        if (_tracks[(size_t)j].y > bottomBoundary)
            continue;
        if (fixedTrackAvailable(j, type, nowMs))
            return j;
    }
    return -1;
}

int DanmakuEngine::pickTrack(const ActiveRec &rec, int64_t nowMs) {
    /* QML:471-498 */
    if (rec.type == DanmakuTypeTop || rec.type == DanmakuTypeBottom)
        return pickFixedTrack(rec.type, nowMs);

    const int count = (int)_tracks.size();
    for (int i = 0; i < count; ++i) {
        if (_tracks[(size_t)i].itemCount == 0)
            return i;
        if (trackAvailable(i, rec, nowMs))
            return i;
    }

    /* 没有可用轨道 → 扩一次再扫一遍（QML 也是重新从 0 扫） */
    if (extendTracks()) {
        const int count2 = (int)_tracks.size();
        for (int i = 0; i < count2; ++i) {
            if (_tracks[(size_t)i].itemCount == 0)
                return i;
            if (trackAvailable(i, rec, nowMs))
                return i;
        }
    }

    return -1;
}

/* ===========================================================================
 * 内部：准入 / 退场
 * =========================================================================== */
bool DanmakuEngine::spawn(const ItemRef &ref, int64_t nowMs) {
    const DanmakuItem &item = itemOf(ref);

    /* 类型：只认 1/2/3（Advanced=4 与非法值直接丢） */
    const int type = normalizeType(item.type);
    if (type == 0) {
        ++_dropped;
        return false;
    }

    /*
     * 过滤（立即生效，只影响新弹幕）。
     *
     * 【自己发的**不过滤**】屏蔽规则（顶部/底部/彩色/高级）是拿来挡"别人刷屏"的；
     * 自己刚发的那条要是被自己的屏蔽规则挡掉，用户看到的就是"点了发送没反应"
     * （而且日志里只有一句"已发送"）。参考那边也是自己发的立刻上屏。
     */
    if (!isSelfItem(item) && matchesFilter(_filter, item)) {
        ++_dropped;
        return false;
    }

    /* 同时在屏上限（QML:222-237 的视图池上限，等价于"活着的视图数 <= maxRenderCount"） */
    const int cap = _config.maxRenderCount > 1 ? _config.maxRenderCount : 1;
    if ((int)_active.size() >= cap) {
        ++_dropped;
        return false;
    }

    const bool isSelf = isSelfItem(item);
    const bool isFixed = (type == DanmakuTypeTop || type == DanmakuTypeBottom);

    /* 字号：自带字号也要乘 fontSizeScale × 自动缩放比例（QML:569-572 的踩坑注释） */
    const float scaleAll = _config.fontSizeScale * (_config.autoScale ? autoScaleFactor() : 1.0f);
    int fontSize = (item.fontSize > 0) ? roundToInt((float)item.fontSize * scaleAll) : currentFontSize();
    if (fontSize < 1)
        fontSize = 1;

    /* 尺寸口径 = 整盒（QML:580-585） */
    const float padH = isFixed ? _config.fixedPadH : (isSelf ? _config.selfPadH : 0.0f);
    const float padV = (isFixed || isSelf) ? _config.contentPadV : 0.0f;
    const float borderW = isSelf ? _config.selfBorderWidth : 0.0f;
    const float textWidth = measureBoxTextWidth(item, fontSize);
    const float width = textWidth + 2.0f * (padH + borderW);
    const float height = (float)fontSize + 2.0f * (padV + borderW);

    /* 时长：滚动 = (容器宽 + 弹幕宽) / (基础速度 × 倍率)；固定 = 4000ms */
    const float baseSpeed = (_config.scrollSpeedPxPerSec > 0.0f) ? _config.scrollSpeedPxPerSec
                                                                : kFallbackScrollSpeed;
    int useSpeed = item.speed;
    if (useSpeed < 1 || useSpeed > 5)
        useSpeed = _config.defaultSpeed;
    if (useSpeed < 1 || useSpeed > 5)
        useSpeed = 3;

    float mult = _config.speedMultipliers[useSpeed - 1];
    if (!(mult > 0.0f))
        mult = 1.0f;

    const double distance = (double)_config.containerWidth + (double)width;
    double durationMs = 0.0;
    if (type == DanmakuTypeScroll)
        durationMs = distance / ((double)baseSpeed * (double)mult) * 1000.0;
    else
        durationMs = (double)_config.fixedDurationMs;
    if (!(durationMs > 0.0))
        durationMs = 1.0;

    ActiveRec rec;
    rec.ref = ref;
    rec.uid = item.id;
    rec.type = type;
    rec.isSelf = isSelf;
    rec.textWidth = textWidth;
    rec.padH = padH;
    rec.padV = padV;
    rec.borderWidth = borderW;
    rec.width = width;
    rec.height = height;
    rec.fontSize = fontSize;
    rec.startMs = nowMs;
    rec.durationMs = durationMs;
    rec.pauseAccumMs = 0.0;
    rec.pauseStartMs = -1.0;
    rec.baseOpacity = 1.0f;
    rec.baseScale = 1.0f;

    /* 先占轨道：占不到就不显示（QML:610-613） */
    const int trackIndex = pickTrack(rec, nowMs);
    /*
     * 【★ 上界也必须查 ★】
     *
     * 原来只判了 `trackIndex < 0`，可是 `retireRec()`（:1088）两头都判 —— 说明这套代码
     * 里"轨道下标可能是陈旧的"是被承认的事实（`_tracks` 会被 rebuildTrackList() /
     * rebuildTracks() / 换容器尺寸重建）。`pickTrack` 内部用的是固定弹幕那条
     * `pickFixedTrack(type, nowMs)`，它算出来的下标来自**当时的**轨道表；表被重建过
     * （条数变少）之后返回的下标就可能 >= `_tracks.size()`，于是
     *     _tracks[(size_t)trackIndex].y / .itemCount += 1
     * 就是**往 vector 之外写** —— 写坏堆，随后在任意时刻崩（用户日志里
     * "载入 1000 条弹幕之后 0.3 秒必崩"就是这个窗口：一帧内几十条一起准入、
     * 同时容器尺寸/轨道表也可能刚被重建过）。
     * 纯 C++11、无平台依赖，不影响跨平台。
     */
    if (trackIndex < 0 || trackIndex >= static_cast<int>(_tracks.size())) {
        ++_dropped;
        return false;
    }

    rec.trackIndex = trackIndex;
    rec.y = _tracks[(size_t)trackIndex].y;
    rec.x = (type == DanmakuTypeScroll)
                ? _config.containerWidth
                : (float)roundToInt((_config.containerWidth - width) * 0.5f);

    _active.push_back(rec);
    _tracks[(size_t)trackIndex].itemCount += 1;
    ++_admitted;
    return true;
}

void DanmakuEngine::retireRec(const ActiveRec &rec) {
    if (rec.trackIndex >= 0 && rec.trackIndex < (int)_tracks.size()) {
        Track &track = _tracks[(size_t)rec.trackIndex];
        if (track.itemCount > 0)
            --track.itemCount;
    }

    if (_hoveredUid != 0 && _hoveredUid == rec.uid)
        _hoveredUid = 0;

    _retired.push_back(rec.uid);
}

bool DanmakuEngine::removeDanmaku(int64_t id) {
    if (id == 0)
        return false;

    try {
        for (size_t i = 0; i < _active.size(); ++i) {
            if (_active[i].uid != id)
                continue;

            /* 退场：归还轨道计数 + 清悬停 + 记进 _retired（渲染层据此丢掉这一条） */
            retireRec(_active[i]);
            _active.erase(_active.begin() + (std::ptrdiff_t)i);

            /* 这一帧的渲染输出立刻重算（宿主马上就会来取） */
            rebuildRenderOut();
            return true;
        }
    } catch (...) {
        /* 公开接口不让异常外泄（最坏情况：这条没被撤掉） */
    }

    return false;
}

void DanmakuEngine::clearActive() {
    for (size_t i = 0; i < _active.size(); ++i)
        _retired.push_back(_active[i].uid);

    _active.clear();

    for (size_t i = 0; i < _tracks.size(); ++i)
        _tracks[i].itemCount = 0;

    _hoveredUid = 0;
}

DanmakuEngine::ActiveRec *DanmakuEngine::findActive(int64_t uid) {
    for (size_t i = 0; i < _active.size(); ++i) {
        if (_active[i].uid == uid)
            return &_active[i];
    }
    return 0;
}

const DanmakuEngine::ActiveRec *DanmakuEngine::findActive(int64_t uid) const {
    for (size_t i = 0; i < _active.size(); ++i) {
        if (_active[i].uid == uid)
            return &_active[i];
    }
    return 0;
}

/* ===========================================================================
 * 内部：发射 / 帧推进
 * =========================================================================== */
void DanmakuEngine::emitDue() {
    /* QML:777-795：游标前进是均摊 O(1)；一帧最多 maxEmitsPerFrame 条 */
    int spawned = 0;
    const int limit = _config.maxEmitsPerFrame;

    while (_cursor < _items.size()) {
        if (limit > 0 && spawned >= limit)
            break;

        /*
         * 【★ 发射标记必须和调度数据一一对应，少一个就是越界写堆 ★】
         *
         * `_emitted` 的语义是"与 `_items` 一一对应的发射标记"（见头文件），
         * 而 `load()` / `append()` / `rewindEmission()` 三条路都会动这两个数组。
         * 只要有一次只改了 `_items` 没同步 `_emitted`（或反过来），`_emitted[_cursor] = 1`
         * 就是往堆里写越界 —— 之后在任意地方崩（用户那边表现为"载入 1000 条弹幕之后
         * 0.3 秒必崩，栈在 QML 的 JS 引擎里 / 析构里 CRT 断言"）。
         * 这里先按**两者中较小的那个**限制游标：宁可少发一条，绝不越界。
         */
        if (_cursor >= _emitted.size())
            break;

        if (_items[_cursor].timeMs > _clockMs)
            break;

        if (!_emitted[_cursor]) {
            _emitted[_cursor] = 1;

            ItemRef ref;
            ref.immediate = false;
            ref.index = _cursor;

            if (spawn(ref, _clockMs))
                ++spawned;
        }

        ++_cursor;
    }
}

void DanmakuEngine::advance() {
    if (_active.empty())
        return;

    /* 悬停满 hoverTimeoutMs（播放时钟计）自动恢复（QML:1010-1026 的 3 秒定时器） */
    if (_hoveredUid != 0 && _config.hoverTimeoutMs > 0 &&
        (_clockMs - _hoverStartMs) >= _config.hoverTimeoutMs) {
        forceResumeHover();
    }

    const float w = _config.containerWidth;
    const size_t count = _active.size();
    size_t retired = 0;

    for (size_t i = 0; i < count; ++i) {
        /* 取一份拷贝：退场时会把后面的记录原地往前压（QML:854-856 同样的就地压实） */
        ActiveRec rec = _active[i];
        const double elapsed = elapsedOf(rec, _clockMs);

        if (rec.type == DanmakuTypeScroll) {
            /* 滚动：x 从容器右边缘线性走到 −宽度（QML:815-827） */
            double p = rec.durationMs > 0.0 ? (elapsed / rec.durationMs) : 1.0;
            p = clampDouble(p, 0.0, 1.0);
            rec.x = w - (float)((double)(w + rec.width) * p);
            rec.baseOpacity = 1.0f;
            rec.baseScale = 1.0f;

            if (elapsed >= rec.durationMs) {
                retireRec(rec);
                ++retired;
                continue;
            }
        } else {
            /* 固定：0→10% 淡入放大、10→90% 停留、90→100% 淡出（QML:828-852） */
            double t = rec.durationMs > 0.0 ? (elapsed / rec.durationMs) : 1.0;
            t = clampDouble(t, 0.0, 1.0);

            if (t < kFixedFadeInEnd) {
                const double k = t / kFixedFadeInEnd;
                rec.baseOpacity = (float)k;
                rec.baseScale = kFixedStartScale + (1.0f - kFixedStartScale) * (float)k;
            } else if (t > kFixedFadeOutStart) {
                const double k = (t - kFixedFadeOutStart) / (1.0 - kFixedFadeOutStart);
                rec.baseOpacity = (float)(1.0 - k);
                rec.baseScale = 1.0f - (1.0f - kFixedEndScale) * (float)k;
            } else {
                rec.baseOpacity = 1.0f;
                rec.baseScale = 1.0f;
            }

            if (elapsed >= rec.durationMs) {
                retireRec(rec);
                ++retired;
                continue;
            }
        }

        /* 总是写回：rec 是拷贝，不写回就等于这一帧算出来的位置丢了
         * （QML 里 rec 是 JS 对象引用，改了就生效，所以那边只在 retired > 0 时才写回数组） */
        _active[i - retired] = rec;
    }

    if (retired > 0)
        _active.resize(count - retired);

    /* 命中检测只在鼠标真的动过时做（QML:863-867） */
    if (_hoverDirty) {
        syncHover();
        _hoverDirty = false;
    }
}

void DanmakuEngine::rebuildRenderOut() {
    _renderOut.clear();

    const size_t n = _active.size();
    if (_renderOut.capacity() < n)
        _renderOut.reserve(n);

    float opacity = _config.opacity;
    if (!(opacity > 0.0f))
        opacity = 0.0f;
    if (opacity > 1.0f)
        opacity = 1.0f;

    for (size_t i = 0; i < n; ++i) {
        const ActiveRec &rec = _active[i];
        const DanmakuItem &src = itemOf(rec.ref);
        const bool hovered = (_hoveredUid != 0 && rec.uid == _hoveredUid);

        double progress = 1.0;
        if (rec.durationMs > 0.0)
            progress = clampDouble(elapsedOf(rec, _clockMs) / rec.durationMs, 0.0, 1.0);

        RenderItem out;
        out.id = rec.uid;
        out.text = src.text.c_str();
        out.textLength = src.text.size();
        out.x = rec.x;
        out.y = rec.y;
        out.width = rec.width;
        out.height = rec.height;
        out.textAdvance = rec.textWidth;
        out.padH = rec.padH;
        out.padV = rec.padV;
        out.fontPixelSize = rec.fontSize;
        /* 悬停中的那条强制不透明（QML:1084 contentOpacity = 1） */
        out.alpha = hovered ? 1.0f : (opacity * rec.baseOpacity);
        out.scale = rec.baseScale;
        out.colorRGBA = src.colorRGBA;
        out.type = rec.type;
        out.isSelf = rec.isSelf;
        out.borderWidth = rec.borderWidth;
        out.bold = _config.fontBold;
        out.outlineType = _config.outlineType;
        out.trackIndex = rec.trackIndex;
        out.hovered = hovered;
        out.progress = (float)progress;
        out.durationMs = rec.durationMs;

        _renderOut.push_back(out);
    }
}

/* ===========================================================================
 * 内部：seek / 游标
 * =========================================================================== */
size_t DanmakuEngine::lowerBoundMs(int64_t timeMs) const {
    /* 二分：第一条 timeMs >= t 的下标（QML:333-347） */
    size_t lo = 0;
    size_t hi = _items.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (_items[mid].timeMs < timeMs)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

void DanmakuEngine::rewindEmission(int64_t timeMs) {
    const int64_t t = timeMs > 0 ? timeMs : 0;
    const size_t from = lowerBoundMs(t);

    for (size_t i = from; i < _emitted.size(); ++i)
        _emitted[i] = 0;

    if (_cursor > from)
        _cursor = from;
}

void DanmakuEngine::applySeek(int64_t timeMs) {
    const int64_t t = timeMs > 0 ? timeMs : 0;

    /* QML:887-896：清场 → 时钟设成新位置 → 游标按 (位置 - 回退容忍) 定位 → 重置之后的发射标记 */
    clearActive();
    _clockMs = t;

    int64_t from = t - _config.rewindToleranceMs;
    if (from < 0)
        from = 0;

    _cursor = lowerBoundMs(from);
    for (size_t i = _cursor; i < _emitted.size(); ++i)
        _emitted[i] = 0;

    _hoveredUid = 0;

    /*
     * 【seek 后按进度补上屏】
     * 参考实现（web 端是 DOM + CSS 动画）seek 之后是"按新时间重新创建这一秒该有的元素"，
     * 所以拖完进度条立刻就有弹幕在飞。我们这边弹幕位置由 elapsed 算出来，
     * 只清场不补的话，屏幕上要等到"下一条弹幕的时间点"才会有东西（用户实测："直接没有了"）。
     */
    reseedActiveAfterSeek(t);
}

void DanmakuEngine::reseedActiveAfterSeek(int64_t t) {
    if (_items.empty() || _tracks.empty())
        return;

    const int64_t from = (t > kReseedWindowMs) ? (t - kReseedWindowMs) : 0;

    for (size_t i = lowerBoundMs(from); i < _items.size(); ++i) {
        const DanmakuItem &item = _items[i];

        if (item.timeMs > t)
            break;                              /* 后面的还没到时间，交给正常发射 */

        if (_emitted[i])
            continue;

        ItemRef ref;
        ref.immediate = false;
        ref.index = i;

        /* 准入走和正常发射完全同一条路（过滤/轨道/上限都在里面） */
        if (!spawn(ref, t))
            continue;

        _emitted[i] = 1;

        /*
         * 【关键一步】把出生时钟回填成弹幕自己的时间点：
         * 这样 elapsedOf() = t − item.timeMs 就是"它已经走了多久"，
         * 滚动弹幕的 x 自然落在正确进度上（不是从右边缘重新开始）。
         */
        if (!_active.empty() && !_active.back().ref.immediate
            && _active.back().ref.index == i) {
            _active.back().startMs = item.timeMs;
            _active.back().pauseAccumMs = 0.0;
            _active.back().pauseStartMs = -1.0;
        }
    }
}

/* ===========================================================================
 * 内部：悬停
 * =========================================================================== */
void DanmakuEngine::releaseHoverAt(int64_t resumeMs) {
    if (_hoveredUid == 0)
        return;

    ActiveRec *rec = findActive(_hoveredUid);
    if (rec != 0 && rec->pauseStartMs >= 0.0) {
        rec->pauseAccumMs += (double)resumeMs - rec->pauseStartMs;
        rec->pauseStartMs = -1.0;
    }

    _hoveredUid = 0;
}

void DanmakuEngine::forceResumeHover() {
    if (_hoveredUid == 0)
        return;

    /* 冻结到"悬停时刻 + 超时"为止，多出来的时间照常播放（参考用墙钟定时器，
     * 这里用播放时钟才能保证同样输入 → 同样输出） */
    int64_t resumeAt = _hoverStartMs + _config.hoverTimeoutMs;
    if (resumeAt > _clockMs)
        resumeAt = _clockMs;

    releaseHoverAt(resumeAt);

    /* 当作鼠标已经移开；宿主再报一次新位置就会重新命中（QML:1018-1026） */
    _pointerX = -1.0f;
    _pointerY = -1.0f;
    _pointerInside = false;
    _hoverDirty = false;
}

void DanmakuEngine::syncHover() {
    /* QML:1042-1091：后加的在上层 → 倒着找就是"最上面那条"；忽略缩放（用未缩放的整盒） */
    int64_t hit = 0;

    if (_pointerInside && _pointerX >= 0.0f && _pointerY >= 0.0f) {
        for (size_t i = _active.size(); i > 0; --i) {
            const ActiveRec &rec = _active[i - 1];
            if (_pointerX >= rec.x && _pointerX <= rec.x + rec.width && _pointerY >= rec.y &&
                _pointerY <= rec.y + rec.height) {
                hit = rec.uid;
                break;
            }
        }
    }

    if (hit == _hoveredUid)
        return;

    /* 离开旧的那条：恢复 */
    if (_hoveredUid != 0)
        releaseHoverAt(_clockMs);

    _hoveredUid = hit;

    if (hit != 0) {
        /* 暂停：记下停住的那一刻，之后把这段时间补回 elapsed */
        ActiveRec *rec = findActive(hit);
        if (rec != 0)
            rec->pauseStartMs = (double)_clockMs;
        _hoverStartMs = _clockMs;
    }
}

/* ===========================================================================
 * 内部：立即显示
 * =========================================================================== */
bool DanmakuEngine::addImmediate(const DanmakuItem &item, int64_t currentMs, int64_t *outId) {
    /* QML:1122：currentMs <= 0 就用当前时钟 */
    const int64_t now = (currentMs > 0) ? currentMs : _clockMs;

    DanmakuItem copy = item;
    if (copy.id == 0)
        copy.id = allocId();
    if (copy.timeMs <= 0)
        copy.timeMs = now;

    _immediate.push_back(copy);

    ItemRef ref;
    ref.immediate = true;
    ref.index = _immediate.size() - 1;

    if (outId != 0)
        *outId = copy.id;

    const bool ok = spawn(ref, now);

    /*
     * 【自己发的弹幕也走"从右边缘外面滑进来"这条正常路径】
     *
     * 这里以前有一段时间偏移：把 self 那条的 startMs 往前挪
     * durationMs × (宽/(容器宽+宽) + 0.02)，让它一出现就整条在画面里 ——
     * 本意是给"暂停时发送"一个立即可见的反馈，但实测观感是错的：
     * 弹幕**凭空出现在播放器中间**再往左走。用户明确要求"应该从屏幕外面开始出现，
     * 再往左移进播放器内部"，参考实现（QML:1149 `_spawn(item, _clockMs)`）本来也是
     * 从右边缘外开始，所以这段偏移已经删掉，不再做任何时间补偿。
     *
     * 代价（有意接受）：暂停状态下发送的那条会停在右边缘外，恢复播放才滑进来 ——
     * 这是"动作要和别人一致"换来的，用户要的就是这个。
     */
    rebuildRenderOut();
    return ok;
}

void DanmakuEngine::pruneImmediate() {
    /* 把已经没人引用的"立即显示"数据丢掉，避免长时间运行堆积。
     * 只用 pop_front，deque 两端增删不会让其它元素挪窝。 */
    while (!_immediate.empty()) {
        bool referenced = false;
        for (size_t i = 0; i < _active.size(); ++i) {
            if (_active[i].ref.immediate && _active[i].ref.index == 0) {
                referenced = true;
                break;
            }
        }
        if (referenced)
            break;

        _immediate.pop_front();
        for (size_t i = 0; i < _active.size(); ++i) {
            if (_active[i].ref.immediate && _active[i].ref.index > 0)
                --_active[i].ref.index;
        }
    }
}

/* ===========================================================================
 * 内部：分段表
 * =========================================================================== */
void DanmakuEngine::rebuildSegments() {
    /* QML:292-308：第 i 段的第一条在 _items 里的下标（升序、可线性推进） */
    _segmentStart.clear();

    int64_t dur = segDurationMs();
    if (dur <= 0)
        dur = 30000;

    int maxSeg = 0;
    if (!_items.empty()) {
        maxSeg = (int)std::floor((double)_items.back().timeMs / (double)dur);
        if (maxSeg < 0)
            maxSeg = 0;
    }

    size_t i = 0;
    for (int seg = 0; seg <= maxSeg + 1; ++seg) {
        const double t = (double)seg * (double)dur;
        while (i < _items.size() && (double)_items[i].timeMs < t)
            ++i;
        _segmentStart.push_back(i);
    }
}
