// ===========================================================================
// Danmaku —— 自测（无测试框架，assert 风格 + 逐条打印）
//
// 覆盖：多弹幕同时准入的分轨、同轨道不重叠、固定顶部/底部、跑完退场、
//       悬停冻结、seek 前进/后退、maxRenderCount 夹取、可复现性（同样输入 → 同样输出）。
//
// 构建：cmake -S . -B build -DDANMAKU_BUILD_TESTS=ON && cmake --build build
// 退出码：0 = 全过，非 0 = 有失败项数
// ===========================================================================
#include <Danmaku/Danmaku.h>

#include <cmath>
#include <cstdio>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;

/* 支持带逗号的表达式：CHECK(near(a, b, 0.01f)) */
#define CHECK(...)                                                                        \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (__VA_ARGS__) {                                                                \
            std::printf("  ok   | %s\n", #__VA_ARGS__);                                    \
        } else {                                                                          \
            ++g_failures;                                                                 \
            std::printf("  FAIL | %s   (selftest.cpp:%d)\n", #__VA_ARGS__, __LINE__);      \
        }                                                                                 \
    } while (0)

static const float kEps = 0.001f;

static bool near(float a, float b, float eps) {
    return std::fabs((double)a - (double)b) <= (double)eps;
}

static bool near(float a, float b) {
    return near(a, b, kEps);
}

static DanmakuItem mkItem(int64_t id, const char *text, int64_t timeMs, int type) {
    DanmakuItem it;
    it.id = id;
    it.text = text;
    it.timeMs = timeMs;
    it.type = type;
    return it;
}

/* 时间推进器：每次最多推 250ms，避免撞上"跳变 > 1000ms 就 seek"的判定 */
struct Clock {
    int64_t now;
    Clock() : now(0) {}

    void stepTo(DanmakuEngine &engine, int64_t targetMs, int64_t stepMs = 250) {
        while (now < targetMs) {
            now += stepMs;
            if (now > targetMs)
                now = targetMs;
            engine.update(now);
        }
    }
};

static const RenderItem *findRender(const std::vector<RenderItem> &items, int64_t id) {
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].id == id)
            return &items[i];
    }
    return 0;
}

static bool containsId(const std::vector<RenderItem> &items, int64_t id) {
    return findRender(items, id) != 0;
}

static std::string textOf(const RenderItem &item) {
    return item.text != 0 ? std::string(item.text) : std::string();
}

/* 把一帧的渲染输出序列化成字符串：可复现性就是拿它逐字节比 */
static std::string snapshot(const std::vector<RenderItem> &items) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(4);
    os << items.size() << ';';
    for (size_t i = 0; i < items.size(); ++i) {
        const RenderItem &it = items[i];
        os << it.id << ',' << it.x << ',' << it.y << ',' << it.width << ',' << it.height << ','
           << it.alpha << ',' << it.scale << ',' << it.fontPixelSize << ',' << it.trackIndex << ','
           << it.type << ',' << (it.isSelf ? 1 : 0) << ',' << (it.hovered ? 1 : 0) << ','
           << it.progress << ',' << it.colorRGBA << ',' << textOf(it) << ';';
    }
    return os.str();
}

static DanmakuConfig defaultConfig() {
    DanmakuEngine probe;
    return probe.config();
}

/* ===========================================================================
 * 1. 基本：准入、轨道 0、右边缘入场、整盒尺寸
 * =========================================================================== */
static void testBasic() {
    std::printf("== 1. 基本准入 / 几何 ==\n");

    DanmakuEngine engine;
    DanmakuStats s0 = engine.stats();
    CHECK(s0.trackCount == 15);        /* max(1, floor(floor((700-80)/28) * 0.7)) */
    CHECK(s0.maxTrackCount == 25);     /* floor(700 / 28) */
    CHECK(s0.activeCount == 0);
    CHECK(engine.autoScaleFactor() == 1.0f);
    CHECK(engine.currentFontSize() == 18);
    CHECK(engine.clockMs() == 0);

    std::vector<DanmakuItem> data;
    data.push_back(mkItem(1, "hello", 0, DanmakuTypeScroll));
    engine.load(data);
    CHECK(engine.itemCount() == 1);
    CHECK(engine.activeItems().empty());     /* 载入不发射 */

    engine.update(0);
    CHECK(engine.activeItems().size() == 1);

    const RenderItem &r = engine.activeItems()[0];
    CHECK(r.id == 1);
    CHECK(textOf(r) == "hello");
    CHECK(r.textLength == 5);
    CHECK(r.type == DanmakuTypeScroll);
    CHECK(r.x == 1280.0f);                   /* 从容器右边缘进入 */
    CHECK(r.y == 10.0f);                     /* 第一条轨道 y = topMargin */
    CHECK(r.trackIndex == 0);
    CHECK(r.fontPixelSize == 18);
    CHECK(r.textAdvance == 54.0f);           /* 兜底测量：5 × 18 × 0.6 = 54 */
    CHECK(r.width == 54.0f);                 /* 滚动非本人：无内边距/边框 */
    CHECK(r.height == 18.0f);
    CHECK(r.alpha == 1.0f);
    CHECK(r.scale == 1.0f);
    CHECK(r.progress == 0.0f);
    CHECK(r.isSelf == false);
    CHECK(r.borderWidth == 0.0f);
    CHECK(r.hovered == false);
    CHECK(near((float)r.durationMs, (1280.0f + 54.0f) / 150.0f * 1000.0f, 0.01f));

    DanmakuStats s1 = engine.stats();
    CHECK(s1.activeCount == 1);
    CHECK(s1.admitted == 1);
    CHECK(s1.dropped == 0);
    CHECK(s1.pending == 0);

    /* 时间推进：x 沿 容器右边缘 → −宽度 这条线走 */
    engine.update(1000);
    CHECK(engine.activeItems().size() == 1);
    CHECK(engine.activeItems()[0].x < 1280.0f);
    CHECK(engine.activeItems()[0].x > 0.0f);

    /* 非法类型（高级弹幕）不渲染 */
    DanmakuEngine e2;
    std::vector<DanmakuItem> adv;
    adv.push_back(mkItem(7, "advanced", 0, DanmakuTypeAdvanced));
    e2.load(adv);
    e2.update(0);
    CHECK(e2.activeItems().empty());
    CHECK(e2.stats().dropped == 1);
}

/* ===========================================================================
 * 2. 文字宽度：回调 + 手工指定
 * =========================================================================== */
static float measureHundred(const std::string &text, int fontPixelSize, bool bold) {
    (void)text;
    (void)fontPixelSize;
    (void)bold;
    return 100.0f;
}

static void testMeasure() {
    std::printf("== 2. 文字测量（回调 / setMeasuredWidth / 兜底）==\n");

    DanmakuEngine engine;
    engine.setMeasureFunction(measureHundred);
    engine.setMeasuredWidth(2, 42.0f);       /* 手工宽度优先于回调 */

    std::vector<DanmakuItem> data;
    data.push_back(mkItem(1, "callback", 0, DanmakuTypeScroll));
    data.push_back(mkItem(2, "manual", 0, DanmakuTypeScroll));
    engine.load(data);
    engine.update(0);

    CHECK(engine.activeItems().size() == 2);
    const RenderItem *a = findRender(engine.activeItems(), 1);
    const RenderItem *b = findRender(engine.activeItems(), 2);
    CHECK(a != 0);
    CHECK(b != 0);
    CHECK(a->width == 100.0f);
    CHECK(a->textAdvance == 100.0f);
    CHECK(b->width == 42.0f);
    CHECK(b->textAdvance == 42.0f);
    CHECK(engine.measureText("x", 18, true) == 100.0f);

    engine.clearMeasuredWidth(2);
    CHECK(engine.measureText("x", 18, false) == 100.0f);

    /* 没有回调时走兜底：码点数 × 字号 × 0.6，且不小于字号 */
    DanmakuEngine e2;
    std::vector<DanmakuItem> d2;
    d2.push_back(mkItem(1, "\xe4\xb8\xad\xe6\x96\x87", 0, DanmakuTypeScroll)); /* 中文 2 字 → 9 字节 */
    e2.load(d2);
    e2.update(0);
    CHECK(e2.activeItems().size() == 1);
    CHECK(e2.activeItems()[0].textAdvance == 22.0f);   /* ceil(2 × 18 × 0.6) = 22 */
}

/* ===========================================================================
 * 3. 大量同时准入：轨道分配（每条占一条轨道，占满封顶）
 * =========================================================================== */
static void testManySimultaneous() {
    std::printf("== 3. 大量同时弹幕的轨道分配 ==\n");

    DanmakuEngine engine;
    std::vector<DanmakuItem> data;
    for (int i = 0; i < 500; ++i)
        data.push_back(mkItem(1000 + i, "aaaa", 0, DanmakuTypeScroll));
    engine.load(data);

    /* 时钟不动，每帧最多准入 10 条 → 多跑几帧把到点的都放出来 */
    for (int frame = 0; frame < 200; ++frame)
        engine.update(0);

    DanmakuStats s = engine.stats();
    std::printf("     activeCount=%d trackCount=%d maxTrackCount=%d admitted=%d dropped=%d\n",
                s.activeCount, s.trackCount, s.maxTrackCount, s.admitted, s.dropped);

    CHECK(s.activeCount > 0);
    CHECK(s.activeCount <= s.trackCount);
    CHECK(s.activeCount == s.trackCount);          /* 轨道被用满 */
    CHECK(s.trackCount == 25);                     /* 15 初始 + 5 + 4 + 1 */
    CHECK(s.admitted == s.activeCount);
    CHECK(s.admitted == 25);
    CHECK(s.dropped == 475);                       /* 剩下的没轨道可放 */
    CHECK(s.pending == 0);

    /* 同一条轨道上不会同时有两条（各自占一条） */
    std::set<int> used;
    bool duplicated = false;
    for (size_t i = 0; i < engine.activeItems().size(); ++i) {
        const RenderItem &it = engine.activeItems()[i];
        CHECK(it.trackIndex >= 0 && it.trackIndex < s.trackCount);
        if (!used.insert(it.trackIndex).second)
            duplicated = true;
    }
    CHECK(used.size() == (size_t)s.trackCount);
    CHECK(!duplicated);
}

/* ===========================================================================
 * 4. 同轨道不重叠：时间推进全过程两两判定
 * =========================================================================== */
static void testNoOverlap() {
    std::printf("== 4. 同轨道滚动弹幕不重叠 ==\n");

    DanmakuEngine engine;
    std::vector<DanmakuItem> data;
    for (int i = 0; i < 15; ++i)
        data.push_back(mkItem(2000 + i, "danmaku", (int64_t)i * 400, DanmakuTypeScroll));
    engine.load(data);

    bool overlap = false;
    size_t maxSimultaneous = 0;
    int64_t overlapAt = -1;
    long pairsChecked = 0;

    for (int64_t t = 0; t <= 20000; t += 50) {
        engine.update(t);
        const std::vector<RenderItem> &act = engine.activeItems();
        if (act.size() > maxSimultaneous)
            maxSimultaneous = act.size();

        for (size_t i = 0; i < act.size(); ++i) {
            for (size_t j = i + 1; j < act.size(); ++j) {
                if (act[i].type != DanmakuTypeScroll || act[j].type != DanmakuTypeScroll)
                    continue;
                if (act[i].trackIndex != act[j].trackIndex)
                    continue;
                ++pairsChecked;
                const bool separated = (act[i].x + act[i].width <= act[j].x) ||
                                       (act[j].x + act[j].width <= act[i].x);
                if (!separated) {
                    overlap = true;
                    if (overlapAt < 0)
                        overlapAt = t;
                }
            }
        }
    }

    DanmakuStats s = engine.stats();
    std::printf("     admitted=%d dropped=%d maxSimultaneous=%d pairsChecked=%ld overlapAt=%lld\n",
                s.admitted, s.dropped, (int)maxSimultaneous, pairsChecked, (long long)overlapAt);

    CHECK(s.admitted == 15);
    CHECK(s.dropped == 0);
    CHECK(maxSimultaneous >= 10);
    CHECK(pairsChecked > 0);            /* 确实有同轨道同时存在的样本 */
    CHECK(!overlap);

    /* 全部跑完后清空 */
    Clock clock;
    clock.now = 20000;
    clock.stepTo(engine, 30000, 500);
    CHECK(engine.activeItems().empty());
    CHECK(engine.stats().activeCount == 0);
}

/* ===========================================================================
 * 5. 固定弹幕：顶部只用前 1/3 轨道、居中、淡入淡出
 * =========================================================================== */
static void testFixed() {
    std::printf("== 5. 固定弹幕（顶部 / 底部 / 淡入淡出）==\n");

    DanmakuEngine engine;
    std::vector<DanmakuItem> data;
    data.push_back(mkItem(1, "TOP1", 0, DanmakuTypeTop));
    data.push_back(mkItem(2, "TOP2", 0, DanmakuTypeTop));
    data.push_back(mkItem(3, "BOTTOM1", 0, DanmakuTypeBottom));
    data.push_back(mkItem(4, "BOTTOM2", 0, DanmakuTypeBottom));
    engine.load(data);
    engine.update(0);

    CHECK(engine.activeItems().size() == 4);
    const RenderItem *t1 = findRender(engine.activeItems(), 1);
    const RenderItem *t2 = findRender(engine.activeItems(), 2);
    const RenderItem *b1 = findRender(engine.activeItems(), 3);
    const RenderItem *b2 = findRender(engine.activeItems(), 4);
    CHECK(t1 != 0);
    CHECK(t2 != 0);
    CHECK(b1 != 0);
    CHECK(b2 != 0);

    const int trackCount = engine.trackCount();
    CHECK(t1->trackIndex < trackCount / 3);      /* 顶部固定只用前 1/3 */
    CHECK(t2->trackIndex < trackCount / 3);
    CHECK(t1->trackIndex != t2->trackIndex);     /* 同类固定不共用轨道 */
    CHECK(b1->trackIndex != b2->trackIndex);
    CHECK(b1->y <= std::floor(720.0f * 0.8f));   /* 底部固定避开最下面 20% */
    CHECK(b2->y <= std::floor(720.0f * 0.8f));

    /* 固定弹幕水平居中：x = round((容器宽 - 整盒宽) / 2) */
    CHECK(near(t1->x, (float)(int)std::floor((1280.0f - t1->width) * 0.5f + 0.5), 0.51f));
    CHECK(near(b1->x, (float)(int)std::floor((1280.0f - b1->width) * 0.5f + 0.5), 0.51f));
    CHECK(near(t1->x + t1->width * 0.5f, 640.0f, 1.0f));

    /* 字号：固定弹幕有 8px 左右内边距、2px 上下内边距 */
    CHECK(t1->padH == 8.0f);
    CHECK(t1->padV == 2.0f);
    CHECK(t1->height == 18.0f + 4.0f);
    CHECK(t1->width == t1->textAdvance + 16.0f);

    /* 淡入淡出：0→10% 淡入放大，10→90% 停留，90→100% 淡出到 0.9 */
    DanmakuEngine e2;
    std::vector<DanmakuItem> d2;
    d2.push_back(mkItem(9, "TOP", 0, DanmakuTypeTop));
    e2.load(d2);

    Clock fade;
    e2.update(0);
    CHECK(e2.activeItems().size() == 1);
    CHECK(near(e2.activeItems()[0].alpha, 0.0f, 0.0001f));
    CHECK(near(e2.activeItems()[0].scale, 0.8f, 0.0001f));

    fade.stepTo(e2, 400);        /* t = 0.10 → 进入停留段 */
    CHECK(e2.activeItems().size() == 1);
    CHECK(near(e2.activeItems()[0].alpha, 1.0f, 0.0001f));
    CHECK(near(e2.activeItems()[0].scale, 1.0f, 0.0001f));

    fade.stepTo(e2, 2000);       /* t = 0.50 → 停留 */
    CHECK(near(e2.activeItems()[0].alpha, 1.0f, 0.0001f));
    CHECK(near(e2.activeItems()[0].scale, 1.0f, 0.0001f));

    fade.stepTo(e2, 3800);       /* t = 0.95 → 淡出 */
    CHECK(near(e2.activeItems()[0].alpha, 0.5f, 0.001f));
    CHECK(near(e2.activeItems()[0].scale, 0.95f, 0.001f));

    fade.stepTo(e2, 4000);       /* 到时长 → 退场 */
    CHECK(e2.activeItems().empty());

    /* 可选开关：底部固定改成"从最下面往上找" */
    DanmakuEngine e3;
    DanmakuConfig cfg = defaultConfig();
    cfg.bottomFixedFromBottom = true;
    e3.setConfig(cfg);
    std::vector<DanmakuItem> d3;
    d3.push_back(mkItem(1, "TOP", 0, DanmakuTypeTop));
    d3.push_back(mkItem(2, "BOTTOM", 0, DanmakuTypeBottom));
    e3.load(d3);
    e3.update(0);
    const RenderItem *tb = findRender(e3.activeItems(), 1);
    const RenderItem *bb = findRender(e3.activeItems(), 2);
    CHECK(tb != 0);
    CHECK(bb != 0);
    CHECK(bb->trackIndex > tb->trackIndex);
    CHECK(bb->y <= std::floor(720.0f * 0.8f));
}

/* ===========================================================================
 * 6. 跑完退场
 * =========================================================================== */
static void testRetire() {
    std::printf("== 6. 滚动弹幕跑完退场 ==\n");

    DanmakuEngine engine;
    std::vector<DanmakuItem> data;
    data.push_back(mkItem(11, "retire", 0, DanmakuTypeScroll));
    engine.load(data);
    engine.update(0);
    CHECK(engine.activeItems().size() == 1);

    const float boxWidth = engine.activeItems()[0].width;
    const double expectedDuration = (1280.0 + (double)boxWidth) / 150.0 * 1000.0;
    std::printf("     width=%.2f expectedDuration=%.2f ms\n", (double)boxWidth, expectedDuration);

    int64_t lastActive = -1;
    int64_t retiredAt = -1;
    float lastActiveX = 0.0f;
    bool sawRetiredId = false;

    for (int64_t t = 100; t <= (int64_t)expectedDuration + 500; t += 100) {
        engine.update(t);
        if (!engine.activeItems().empty()) {
            lastActive = t;
            lastActiveX = engine.activeItems()[0].x;
        } else if (retiredAt < 0) {
            retiredAt = t;
            const std::vector<int64_t> &ids = engine.retiredIds();
            for (size_t i = 0; i < ids.size(); ++i) {
                if (ids[i] == 11)
                    sawRetiredId = true;
            }
        } else {
            CHECK(engine.retiredIds().empty());
        }
    }

    std::printf("     lastActive=%lld x=%.2f retiredAt=%lld\n", (long long)lastActive,
                (double)lastActiveX, (long long)retiredAt);

    CHECK(lastActive > 0);
    CHECK(retiredAt > 0);
    CHECK((double)retiredAt >= expectedDuration);
    CHECK((double)retiredAt <= expectedDuration + 100.0);
    CHECK(lastActiveX < 0.0f);                       /* 跑出左边缘 */
    CHECK(sawRetiredId);                             /* 退场 id 上报给渲染层 */
    CHECK(engine.activeItems().empty());
    CHECK(engine.stats().activeCount == 0);
    CHECK(engine.stats().admitted == 1);
}

/* ===========================================================================
 * 7. 悬停冻结 / 悬停超时自动恢复
 * =========================================================================== */
static void testHover() {
    std::printf("== 7. 悬停冻结 ==\n");

    DanmakuEngine engine;
    std::vector<DanmakuItem> data;
    data.push_back(mkItem(21, "hover", 0, DanmakuTypeScroll));
    engine.load(data);
    engine.update(0);
    CHECK(engine.activeItems().size() == 1);
    CHECK(engine.activeItems()[0].x == 1280.0f);
    CHECK(engine.hoveredId() == 0);

    /* 盒子 = x 1280..1334, y 10..28 */
    engine.setPointerPosition(1300.0f, 15.0f, true);
    engine.update(0);
    CHECK(engine.hoveredId() == 21);
    CHECK(engine.activeItems()[0].hovered == true);
    CHECK(engine.activeItems()[0].alpha == 1.0f);

    /* 冻结：时钟前进 900ms，位置一动不动 */
    engine.update(900);
    CHECK(engine.hoveredId() == 21);
    CHECK(engine.activeItems()[0].x == 1280.0f);
    CHECK(engine.activeItems()[0].progress == 0.0f);

    /* 鼠标移开：这一帧位置还是冻结时算出来的，下一帧才恢复 */
    engine.setPointerPosition(5.0f, 5.0f, true);
    engine.update(900);
    CHECK(engine.hoveredId() == 0);
    CHECK(engine.activeItems()[0].hovered == false);
    CHECK(engine.activeItems()[0].x == 1280.0f);     /* 冻结到 900ms 为止 */

    engine.update(1800);                             /* 解冻后又走了 900ms */
    CHECK(engine.hoveredId() == 0);
    const float xAfterResume = engine.activeItems()[0].x;
    CHECK(xAfterResume < 1280.0f);
    CHECK(near(engine.activeItems()[0].progress, 900.0f / (float)((1280.0 + 54.0) / 150.0 * 1000.0),
               0.001f));
    std::printf("     xAfterResume=%.3f\n", (double)xAfterResume);

    /* 鼠标不动 → 不算悬停（弹幕从静止光标底下划过去不触发） */
    DanmakuEngine e2;
    std::vector<DanmakuItem> d2;
    d2.push_back(mkItem(22, "hover", 0, DanmakuTypeScroll));
    e2.load(d2);
    e2.update(0);
    e2.setPointerPosition(1300.0f, 15.0f, true);
    e2.update(0);
    CHECK(e2.hoveredId() == 22);

    /* 悬停 3 秒自动恢复（QML 里是 3000ms 墙钟定时器，这里用播放时钟保证可复现） */
    DanmakuEngine e3;
    std::vector<DanmakuItem> d3;
    d3.push_back(mkItem(23, "hover", 0, DanmakuTypeScroll));
    e3.load(d3);
    e3.update(0);
    e3.setPointerPosition(1300.0f, 15.0f, true);
    e3.update(0);
    CHECK(e3.hoveredId() == 23);

    Clock clock;
    clock.stepTo(e3, 2900);
    CHECK(e3.hoveredId() == 23);                     /* 还没到 3 秒 */
    CHECK(e3.activeItems()[0].x == 1280.0f);

    clock.stepTo(e3, 3000);
    CHECK(e3.hoveredId() == 0);                      /* 到点自动恢复 */
    CHECK(e3.activeItems()[0].x == 1280.0f);

    clock.stepTo(e3, 3250);
    CHECK(e3.activeItems()[0].x < 1280.0f);
}

/* ===========================================================================
 * 8. seek 前进 / 后退
 * =========================================================================== */
static void testSeek() {
    std::printf("== 8. seekTo 前进 / 后退 ==\n");

    DanmakuEngine engine;
    std::vector<DanmakuItem> data;
    for (int i = 0; i < 10; ++i)
        data.push_back(mkItem(300 + i, "seek", (int64_t)i * 1000, DanmakuTypeScroll));
    engine.load(data);
    CHECK(engine.stats().pending == 10);

    engine.update(0);
    CHECK(engine.activeItems().size() == 1);
    CHECK(engine.activeItems()[0].id == 300);

    /* 前进到 5s：只重定位，不发射 */
    engine.seekTo(5000);
    CHECK(engine.clockMs() == 5000);
    CHECK(engine.activeItems().empty());
    CHECK(engine.retiredIds().size() == 1);
    CHECK(engine.retiredIds()[0] == 300);
    CHECK(engine.stats().pending == 5);

    engine.update(5000);
    CHECK(engine.activeItems().size() == 1);
    CHECK(engine.activeItems()[0].id == 305);         /* 中间的 301..304 不该被喷出来 */

    /* 后退到 1s */
    engine.seekTo(1000);
    CHECK(engine.clockMs() == 1000);
    CHECK(engine.activeItems().empty());

    engine.update(1000);
    CHECK(engine.activeItems().size() == 1);
    CHECK(engine.activeItems()[0].id == 301);

    /* 回退重置过发射标记 → 时间到时后面那条还能正常出现 */
    Clock clock;
    clock.now = 1000;
    clock.stepTo(engine, 2000);
    CHECK(containsId(engine.activeItems(), 302));

    /* 大步前进到 9s：只有 309 */
    engine.seekTo(9000);
    engine.update(9000);
    CHECK(engine.activeItems().size() == 1);
    CHECK(engine.activeItems()[0].id == 309);

    /* 回到 0 */
    engine.seekTo(0);
    CHECK(engine.clockMs() == 0);
    engine.update(0);
    CHECK(engine.activeItems().size() == 1);
    CHECK(engine.activeItems()[0].id == 300);
    CHECK(engine.stats().pending == 9);

    /* 小幅回退（< 跳变阈值）不清场，只重置发射标记 */
    DanmakuEngine e2;
    std::vector<DanmakuItem> d2;
    d2.push_back(mkItem(1, "rewind", 0, DanmakuTypeScroll));
    e2.load(d2);
    e2.update(0);
    CHECK(e2.activeItems().size() == 1);
    e2.update(300);
    CHECK(e2.activeItems().size() == 1);
    const float xBefore = e2.activeItems()[0].x;
    e2.update(200);                                   /* 回退 100ms */
    CHECK(e2.activeItems().size() == 1);              /* 不清场 */
    CHECK(e2.activeItems()[0].x > xBefore);
}

/* ===========================================================================
 * 9. maxRenderCount 夹取
 * =========================================================================== */
static void testMaxRenderCount() {
    std::printf("== 9. maxRenderCount 夹取 ==\n");

    DanmakuEngine engine;
    DanmakuConfig cfg = defaultConfig();
    cfg.maxRenderCount = 5;
    engine.setConfig(cfg);

    std::vector<DanmakuItem> data;
    for (int i = 0; i < 20; ++i)
        data.push_back(mkItem(400 + i, "cap", 0, DanmakuTypeScroll));
    engine.load(data);
    engine.update(0);

    DanmakuStats s = engine.stats();
    std::printf("     activeCount=%d admitted=%d dropped=%d\n", s.activeCount, s.admitted, s.dropped);
    CHECK(s.activeCount == 5);
    CHECK(s.admitted == 5);
    CHECK(s.dropped == 15);
    CHECK(s.pending == 0);
    CHECK(engine.activeItems().size() == 5);

    /* 夹到 1 也成立（max(1, maxRenderCount)） */
    DanmakuEngine e2;
    DanmakuConfig cfg2 = defaultConfig();
    cfg2.maxRenderCount = 1;
    e2.setConfig(cfg2);
    std::vector<DanmakuItem> d2;
    for (int i = 0; i < 3; ++i)
        d2.push_back(mkItem(500 + i, "one", 0, DanmakuTypeScroll));
    e2.load(d2);
    e2.update(0);
    CHECK(e2.activeItems().size() == 1);
    CHECK(e2.stats().dropped == 2);
}

/* ===========================================================================
 * 10. 可复现性：同样配置 + 同样数据 + 同样时间序列 → 同样输出
 * =========================================================================== */
static void buildMixedData(std::vector<DanmakuItem> &out) {
    out.clear();
    for (int i = 0; i < 30; ++i) {
        DanmakuItem it;
        it.id = 600 + i;
        it.text = (i % 3 == 0) ? "repro-a" : ((i % 3 == 1) ? "repro-bb" : "复现测试");
        it.timeMs = (int64_t)i * 500;
        it.type = (i % 10 == 3) ? DanmakuTypeTop : ((i % 10 == 7) ? DanmakuTypeBottom : DanmakuTypeScroll);
        it.fontSize = (i % 4 == 0) ? 24 : 0;
        it.speed = (i % 5 == 0) ? 5 : 0;
        it.colorRGBA = (i % 6 == 0) ? danmakuColorFromHex("#ff0000") : 0xFFFFFFFFu;
        it.isSelf = (i % 8 == 0);
        if (it.isSelf)
            it.uid = "1";
        out.push_back(it);
    }
}

static void testReproducible() {
    std::printf("== 10. 可复现性 ==\n");

    std::vector<DanmakuItem> data;
    buildMixedData(data);

    DanmakuEngine a;
    DanmakuEngine b;
    a.load(data);
    b.load(data);

    bool everythingEqual = true;
    int64_t firstDiffAt = -1;
    for (int64_t t = 0; t <= 20000; t += 250) {
        a.update(t);
        b.update(t);
        if (snapshot(a.activeItems()) != snapshot(b.activeItems())) {
            everythingEqual = false;
            if (firstDiffAt < 0)
                firstDiffAt = t;
        }
    }
    std::printf("     firstDiffAt=%lld\n", (long long)firstDiffAt);
    CHECK(everythingEqual);

    const std::string finalA = snapshot(a.activeItems());

    /* 再跑一遍（新实例、独立推进）→ 同样的结果 */
    DanmakuEngine c;
    c.load(data);
    for (int64_t t = 0; t <= 20000; t += 250)
        c.update(t);
    CHECK(snapshot(c.activeItems()) == finalA);

    /* 时间步长一样但实例之间穿插调用（没有任何全局状态被互相污染） */
    DanmakuEngine d;
    DanmakuEngine e;
    d.load(data);
    e.load(data);
    for (int64_t t = 0; t <= 20000; t += 250) {
        e.update(t);
        d.update(t);
    }
    CHECK(snapshot(d.activeItems()) == finalA);
    CHECK(snapshot(e.activeItems()) == finalA);

    /* 时钟没推进时反复 update 不该改变任何东西 */
    const std::string before = snapshot(e.activeItems());
    e.update(20000);
    e.update(20000);
    CHECK(snapshot(e.activeItems()) == before);
}

/* ===========================================================================
 * 11. 过滤器
 * =========================================================================== */
static void testFilter() {
    std::printf("== 11. 过滤器 ==\n");

    /* colorful：非白全过滤 */
    DanmakuEngine engine;
    DanmakuFilter f;
    f.colorful = true;
    engine.setFilter(f);
    CHECK(engine.filter().colorful == true);

    std::vector<DanmakuItem> data;
    DanmakuItem white = mkItem(1, "white", 0, DanmakuTypeScroll);
    white.colorRGBA = 0xFFFFFFFFu;
    DanmakuItem red = mkItem(2, "red", 0, DanmakuTypeScroll);
    red.colorRGBA = danmakuColorFromHex("#ff0000");
    data.push_back(white);
    data.push_back(red);
    engine.load(data);
    engine.update(0);
    CHECK(engine.activeItems().size() == 1);
    CHECK(engine.activeItems()[0].id == 1);
    CHECK(engine.stats().dropped == 1);

    /* scroll：只过滤滚动，固定照常显示 */
    DanmakuEngine e2;
    DanmakuFilter f2;
    f2.scroll = true;
    e2.setFilter(f2);
    std::vector<DanmakuItem> d2;
    d2.push_back(mkItem(3, "scroll", 0, DanmakuTypeScroll));
    d2.push_back(mkItem(4, "top", 0, DanmakuTypeTop));
    e2.load(d2);
    e2.update(0);
    CHECK(e2.activeItems().size() == 1);
    CHECK(e2.activeItems()[0].id == 4);
    CHECK(e2.stats().dropped == 1);

    /* top / bottom 单独过滤 */
    DanmakuEngine e3;
    DanmakuFilter f3;
    f3.bottom = true;
    e3.setFilter(f3);
    std::vector<DanmakuItem> d3;
    d3.push_back(mkItem(5, "top", 0, DanmakuTypeTop));
    d3.push_back(mkItem(6, "bottom", 0, DanmakuTypeBottom));
    e3.load(d3);
    e3.update(0);
    CHECK(e3.activeItems().size() == 1);
    CHECK(e3.activeItems()[0].id == 5);

    /* advanced + resetFilter */
    DanmakuEngine e4;
    DanmakuFilter f4;
    f4.advanced = true;
    e4.setFilter(f4);
    CHECK(e4.filter().advanced == true);
    e4.resetFilter();
    CHECK(e4.filter().advanced == false);
    CHECK(e4.filter().scroll == false);
    CHECK(e4.filter().colorful == false);

    /* 颜色工具 */
    CHECK(danmakuColorFromHex("#ffffff") == 0xFFFFFFFFu);
    CHECK(danmakuColorFromHex("white") == 0xFFFFFFFFu);
    CHECK(danmakuColorFromHex("#fff") == 0xFFFFFFFFu);
    CHECK(danmakuColorFromHex("  #FF0000 ") == 0xFF0000FFu);
    CHECK(danmakuColorFromHex("0x00ff00") == 0x00FF00FFu);
    CHECK(danmakuColorFromHex("#11223344") == 0x11223344u);
    CHECK(danmakuColorFromHex("nonsense", 0x12345678u) == 0x12345678u);
    CHECK(danmakuColorIsWhite(0xFFFFFFFFu));
    CHECK(!danmakuColorIsWhite(0xFF0000FFu));
    CHECK(danmakuColorToHex(0xFF0000FFu) == "#ff0000ff");
    CHECK(danmakuColorFromRgb(255, 0, 0, 255) == 0xFF0000FFu);
}

/* ===========================================================================
 * 12. 本人弹幕 / sendDanmaku
 * =========================================================================== */
static void testSend() {
    std::printf("== 12. 本人弹幕 / sendDanmaku ==\n");

    DanmakuEngine engine;
    DanmakuSendOptions options;
    CHECK(options.self == true);
    CHECK(options.uid == "1");
    CHECK(options.type == DanmakuTypeScroll);

    CHECK(engine.sendDanmaku("\xe6\x88\x91\xe5\x8f\x91\xe7\x9a\x84", options));
    CHECK(engine.activeItems().size() == 1);

    const RenderItem &self = engine.activeItems()[0];
    CHECK(self.isSelf == true);
    CHECK(self.borderWidth == 2.0f);
    CHECK(self.id > 0);
    CHECK(self.width == self.textAdvance + 16.0f);   /* 2 × (selfPadH 6 + border 2) */
    CHECK(self.height == 18.0f + 8.0f);              /* 字号 + 2 × (padV 2 + border 2) */
    CHECK(self.padH == 6.0f);
    CHECK(self.padV == 2.0f);

    /* 别人的弹幕：没有白框 */
    DanmakuSendOptions other;
    other.self = false;
    other.uid = "0";
    other.id = 999;
    CHECK(engine.sendDanmaku("other", other));
    CHECK(engine.activeItems().size() == 2);

    const RenderItem *o = findRender(engine.activeItems(), 999);
    CHECK(o != 0);
    CHECK(o->isSelf == false);
    CHECK(o->borderWidth == 0.0f);
    CHECK(o->height == 18.0f);
    CHECK(o->width == o->textAdvance);

    /* 带 options 的自定义：字号 / 颜色 / 速度 / 固定位置 */
    DanmakuSendOptions custom;
    custom.id = 1000;
    custom.type = DanmakuTypeTop;
    custom.fontSize = 24;
    custom.colorRGBA = danmakuColorFromHex("#00a1d6");
    CHECK(engine.sendDanmaku("custom", custom));
    const RenderItem *c = findRender(engine.activeItems(), 1000);
    CHECK(c != 0);
    CHECK(c->type == DanmakuTypeTop);
    CHECK(c->fontPixelSize == 24);
    CHECK(c->colorRGBA == danmakuColorFromHex("#00a1d6"));
    CHECK(c->padH == 8.0f);

    /* addDanmaku 指定 currentMs <= 0 → 用当前时钟 */
    DanmakuItem item = mkItem(0, "immediate", 0, DanmakuTypeScroll);
    CHECK(engine.addDanmaku(item, 0));
    CHECK(engine.activeItems().size() == 4);
    CHECK(engine.stats().admitted == 4);
}

/* ===========================================================================
 * 13. 暂停
 * =========================================================================== */
static void testPaused() {
    std::printf("== 13. 全局暂停 ==\n");

    DanmakuEngine engine;
    std::vector<DanmakuItem> data;
    data.push_back(mkItem(31, "pause", 0, DanmakuTypeScroll));
    engine.load(data);
    engine.update(0);
    CHECK(engine.activeItems().size() == 1);
    const float x0 = engine.activeItems()[0].x;
    CHECK(x0 == 1280.0f);

    engine.setPaused(true);
    CHECK(engine.isPaused() == true);
    engine.update(500);
    CHECK(engine.clockMs() == 0);                     /* 时钟不动 */
    CHECK(engine.activeItems()[0].x == x0);

    engine.setPaused(false);
    CHECK(engine.isPaused() == false);
    engine.update(500);
    CHECK(engine.clockMs() == 500);
    CHECK(engine.activeItems()[0].x < x0);
}

/* ===========================================================================
 * 14. 配置：容器尺寸 / 自动缩放 / 区域
 * =========================================================================== */
static void testConfig() {
    std::printf("== 14. 容器尺寸 / 自动缩放 / 区域 ==\n");

    DanmakuEngine engine;
    CHECK(near(engine.autoScaleFactor(), 1.0f));

    engine.setContainerSize(1920.0f, 1080.0f);
    CHECK(near(engine.autoScaleFactor(), 1.5f));      /* 1.5 → 夹在上限 */
    CHECK(engine.currentFontSize() == 27);            /* round(18 × 1.5) */
    CHECK(engine.trackCount() == 24);                 /* floor((1060-80)/28 × 0.7) */
    CHECK(engine.config().containerWidth == 1920.0f);

    engine.setContainerSize(640.0f, 360.0f);
    CHECK(near(engine.autoScaleFactor(), 0.75f));     /* 0.5 → 夹在下限 */
    CHECK(engine.currentFontSize() == 14);            /* round(18 × 0.75) = round(13.5) */
    CHECK(engine.trackCount() == 6);

    /* 区域档位：只影响可用高度 */
    DanmakuEngine e2;
    DanmakuConfig cfg = defaultConfig();
    cfg.areaRatio = 0.5f;
    e2.setConfig(cfg);
    CHECK(e2.trackCount() == 6);

    /* 显式字号也乘 fontSizeScale × 自动缩放比例 */
    DanmakuEngine e3;
    DanmakuConfig cfg3 = defaultConfig();
    cfg3.fontSizeScale = 2.0f;
    e3.setConfig(cfg3);
    std::vector<DanmakuItem> d3;
    d3.push_back(mkItem(1, "scale", 0, DanmakuTypeScroll));
    e3.load(d3);
    e3.update(0);
    CHECK(e3.activeItems().size() == 1);
    CHECK(e3.activeItems()[0].fontPixelSize == 36);   /* 18 × 2 */

    DanmakuItem sized = mkItem(2, "scale", 0, DanmakuTypeScroll);
    sized.fontSize = 10;
    DanmakuEngine e4;
    DanmakuConfig cfg4 = defaultConfig();
    cfg4.fontSizeScale = 2.0f;
    e4.setConfig(cfg4);
    std::vector<DanmakuItem> d4;
    d4.push_back(sized);
    e4.load(d4);
    e4.update(0);
    CHECK(e4.activeItems()[0].fontPixelSize == 20);   /* 自带 10 × 2 */

    /* 尺寸变化 → 在屏弹幕实时重算（QML 的行为） */
    DanmakuEngine e5;
    std::vector<DanmakuItem> d5;
    d5.push_back(mkItem(1, "resize", 0, DanmakuTypeTop));
    e5.load(d5);
    e5.update(0);
    CHECK(e5.activeItems()[0].fontPixelSize == 18);
    const float widthBefore = e5.activeItems()[0].width;
    e5.setContainerSize(1920.0f, 1080.0f);
    CHECK(e5.activeItems().size() == 1);
    CHECK(e5.activeItems()[0].fontPixelSize == 27);
    CHECK(e5.activeItems()[0].width > widthBefore);
    CHECK(near(e5.activeItems()[0].x + e5.activeItems()[0].width * 0.5f, 960.0f, 1.0f));
}

/* ===========================================================================
 * 15. 分段表 / clear / reset / append
 * =========================================================================== */
static void testData() {
    std::printf("== 15. 分段表 / clear / reset / append ==\n");

    DanmakuEngine engine;
    std::vector<DanmakuItem> data;
    data.push_back(mkItem(1, "a", 0, DanmakuTypeScroll));
    data.push_back(mkItem(2, "b", 5000, DanmakuTypeScroll));
    data.push_back(mkItem(3, "c", 25000, DanmakuTypeScroll));
    data.push_back(mkItem(4, "d", 65000, DanmakuTypeScroll));
    engine.load(data);

    CHECK(engine.segmentCount() == 4);                /* 0 / 30s / 60s / 90s */
    CHECK(engine.segmentIndexForTimeMs(0) == 0);
    CHECK(engine.segmentIndexForTimeMs(31000) == 1);
    CHECK(engine.segmentIndexForTimeMs(70000) == 2);

    size_t begin = 0;
    size_t end = 0;
    CHECK(engine.segmentRange(0, begin, end));
    CHECK(begin == 0);
    CHECK(end == 3);                                  /* 0 / 5000 / 25000 都在第 0 段 */
    CHECK(engine.segmentRange(1, begin, end));
    CHECK(begin == 3);
    CHECK(end == 3);
    CHECK(engine.segmentRange(2, begin, end));
    CHECK(begin == 3);
    CHECK(end == 4);

    /* append：时间递增走尾插，时间落在中间插到正确位置 */
    engine.reset();
    CHECK(engine.itemCount() == 0);
    CHECK(engine.activeItems().empty());

    std::vector<DanmakuItem> base;
    base.push_back(mkItem(10, "one", 1000, DanmakuTypeScroll));
    base.push_back(mkItem(11, "two", 3000, DanmakuTypeScroll));
    engine.load(base);
    engine.append(mkItem(12, "middle", 2000, DanmakuTypeScroll));
    engine.append(mkItem(13, "last", 5000, DanmakuTypeScroll));
    CHECK(engine.itemCount() == 4);
    CHECK(engine.itemAt(0) != 0 && engine.itemAt(0)->timeMs == 1000);
    CHECK(engine.itemAt(1) != 0 && engine.itemAt(1)->timeMs == 2000);
    CHECK(engine.itemAt(2) != 0 && engine.itemAt(2)->timeMs == 3000);
    CHECK(engine.itemAt(3) != 0 && engine.itemAt(3)->timeMs == 5000);
    CHECK(engine.itemAt(4) == 0);

    Clock clock;
    clock.stepTo(engine, 2000);
    CHECK(engine.activeItems().size() == 2);
    CHECK(containsId(engine.activeItems(), 10));
    CHECK(containsId(engine.activeItems(), 12));
    CHECK(engine.stats().pending == 2);

    /* clear：只清屏 + 重置发射记录，数据保留；游标跟当前时钟走（QML:1180），
       所以时钟之前那些不会被重发，当前时刻及之后的照常 */
    engine.clear();
    CHECK(engine.activeItems().empty());
    CHECK(engine.itemCount() == 4);
    engine.update(2000);
    CHECK(engine.activeItems().size() == 1);
    CHECK(containsId(engine.activeItems(), 12));
    CHECK(engine.stats().pending == 2);

    /* 载入时按当前时钟定位游标：播到一半载入不会把之前的弹幕一次喷出来 */
    DanmakuEngine e2;
    e2.seekTo(4000);
    std::vector<DanmakuItem> d2;
    for (int i = 0; i < 10; ++i)
        d2.push_back(mkItem(700 + i, "load", (int64_t)i * 1000, DanmakuTypeScroll));
    e2.load(d2);
    e2.update(4000);
    CHECK(e2.activeItems().size() == 1);
    CHECK(e2.activeItems()[0].id == 704);
    CHECK(e2.stats().pending == 5);
}

/* ===========================================================================
 * main
 * =========================================================================== */
int main() {
    std::printf("Danmaku selftest\n");
    std::printf("----------------------\n");

    testBasic();
    testMeasure();
    testManySimultaneous();
    testNoOverlap();
    testFixed();
    testRetire();
    testHover();
    testSeek();
    testMaxRenderCount();
    testReproducible();
    testFilter();
    testSend();
    testPaused();
    testConfig();
    testData();

    std::printf("----------------------\n");
    std::printf("checks=%d failures=%d\n", g_checks, g_failures);

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }

    std::printf("FAILED\n");
    return g_failures > 125 ? 125 : g_failures;
}
