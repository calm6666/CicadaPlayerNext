// ===========================================================================
// Danmaku —— 纯 C++11 弹幕引擎：类型与配置
//
// 行为照着下面两份参考实现来（数值出处逐条标了注释）：
//   1. CicadaPlayerNext/platform/QtPlayer/DanmakuLayer.qml
//      —— 当前 Qt/QML 实现（引擎 + 渲染层），**本模块以此为准**
//   2. front/hilihili-components/src/utils/danmaku（README + types/trackManager/
//      domEngine/scheduler/scaleHelper）
//      —— 最初的参考实现，用来确认语义与命名
//
// 约定：
//   * 纯标准库（string/vector/deque/map/functional/cstdint），无 Qt、无框架、无第三方
//   * 没有全局/静态可变状态，同进程里可以同时存在多个 DanmakuEngine 互不影响
//   * 引擎不读墙钟、不用随机数：同样配置 + 同样数据 + 同样时间序列 → 同样输出
//   * 引擎只在宿主调用的那几个方法里干活（单线程用；多线程请自己在外面加锁）
// ===========================================================================
#ifndef DANMAKU_TYPES_H
#define DANMAKU_TYPES_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <Danmaku/DanmakuExport.h>

/* ===========================================================================
 * 弹幕类型（数值与参考 types.ts:9-18 的 DanmakuType 一致）
 * =========================================================================== */
enum DanmakuType {
    /** 滚动弹幕 */
    DanmakuTypeScroll = 1,
    /** 顶部固定 */
    DanmakuTypeTop = 2,
    /** 底部固定 */
    DanmakuTypeBottom = 3,
    /** 高级弹幕：参考里有这个枚举，但两套实现都不渲染（本引擎直接丢弃） */
    DanmakuTypeAdvanced = 4
};

/* ===========================================================================
 * 文字宽度测量回调
 *
 * 引擎必须知道每条弹幕的像素宽度才能算位置、算时长、判碰撞。宿主把自
 * 己的文字度量接进来（Qt 用 QFontMetricsF::horizontalAdvance，Skia 用
 * SkFont::measureText ...）；不接就用内置兜底估算。
 *
 * 参数：text UTF-8 文本；fontPixelSize 字号（像素）；bold 是否粗体
 * 返回：文字前进宽度（像素）。返回 <= 0 视为"测不出来"，引擎走兜底。
 * 回调里抛出的任何异常都会被引擎吞掉并退回兜底，不会外泄。
 * =========================================================================== */
typedef std::function<float(const std::string &text, int fontPixelSize, bool bold)> DanmakuMeasureFn;

/* ===========================================================================
 * 配置
 *
 * 默认值全部取自参考实现（QML:56-97 / trackManager.ts:43-52 / domEngine.ts:111-124
 * / index.ts:115-134 的 defaultConfig）。
 *
 * 【重要】速度/区域/字号/透明度这类 setter 只影响**新**弹幕（README:1565-1573），
 * 已经在屏上的弹幕保持出生时的参数。唯一例外是 rescaleActive()（改容器尺寸 /
 * 字号缩放时实时重算在屏弹幕，见 QML:937-972 的注释）。
 * =========================================================================== */
struct DANMAKU_API DanmakuConfig {
    /* ------------------------------------------------------------------
     * 容器与区域
     * ------------------------------------------------------------------ */

    /** 弹幕层宽（像素）。宿主必须设成真实的视频区宽度，默认 1280 */
    float containerWidth;
    /** 弹幕层高（像素）。默认 720 */
    float containerHeight;
    /** 区域档位：0.25 / 0.5 / 0.75 / 1（types.ts:43-52），默认 1 */
    float areaRatio;
    /** 区域顶部再让出的高度（像素），默认 0（0 = 与 QML 完全一致） */
    float areaTop;
    /** 区域底部再让出的高度（像素），默认 0（0 = 与 QML 完全一致） */
    float areaBottom;

    /* ------------------------------------------------------------------
     * 速度（domEngine.ts:52-58 / 543-552）
     * ------------------------------------------------------------------ */

    /** 基础速度：150 px/秒 */
    float scrollSpeedPxPerSec;
    /** 5 档速度倍率：{0.5, 0.75, 1.0, 1.5, 2.0} */
    float speedMultipliers[5];
    /** 默认速度档位 1..5（DanmakuSpeed.NORMAL = 3） */
    int defaultSpeed;
    /** 固定弹幕时长（毫秒）：4000 */
    float fixedDurationMs;

    /* ------------------------------------------------------------------
     * 字号（scaleHelper.ts:72-89 / QML:139-162）
     * ------------------------------------------------------------------ */

    /** 基准字号，默认 18（对应参考的 options.fontSize） */
    int baseFontSize;
    /** 用户字号缩放，默认 1.0 */
    float fontSizeScale;
    /** 是否随屏幕（弹幕层宽）自动缩放，默认 true */
    bool autoScale;
    /** 自动缩放的基准宽度，默认 1280 */
    float autoScaleBaseWidth;
    /** 自动缩放比例下限，默认 0.75 */
    float autoScaleMin;
    /** 自动缩放比例上限，默认 1.5（QML 把参考的 1.25 提到 1.5，见 QML:147-158） */
    float autoScaleMax;
    /** 字号下限（只作用于"用基准字号"的那条路径），默认 12 */
    int fontSizeMin;
    /** 字号上限，默认 36 */
    int fontSizeMax;

    /* ------------------------------------------------------------------
     * 轨道（trackManager.ts:70-110 / 137-138 / 249-274）
     * ------------------------------------------------------------------ */

    /** 轨道高，默认 24 */
    float trackHeight;
    /** 轨道间距，默认 4 */
    float trackGap;
    /** 顶部边距，默认 10 */
    float topMargin;
    /** 底部边距，默认 10 */
    float bottomMargin;
    /** 底部安全区（字幕区），默认 80：默认不占，轨道不够时扩进去 */
    float bottomSafeArea;
    /** 初始只开的轨道比例，默认 0.7（max(1, floor(maxTrackCount * 0.7))） */
    float initialTrackRatio;

    /* ------------------------------------------------------------------
     * 行为
     * ------------------------------------------------------------------ */

    /** 整体透明度 0..1，默认 1 */
    float opacity;
    /** 同时在屏上限，默认 2000（README 写的 500 是旧值，以实现为准） */
    int maxRenderCount;
    /** 分段时长（秒），默认 30（用于分段起点表 + seek 定位） */
    float segmentDurationSec;
    /** 一帧最多准入多少条，默认 10（index.ts:238 的额度控制） */
    int maxEmitsPerFrame;
    /** 播放时间跳变判定阈值（毫秒），默认 1000：超过就当作 seek 清场重来 */
    int64_t jumpThresholdMs;
    /** 回退容忍（毫秒），默认 50：回退超过就重置发射标记（scheduler 的 0.05s 阈值） */
    int64_t rewindToleranceMs;
    /** 固定弹幕的轨道复用阈值（毫秒），默认 500：剩余时间 <= 500ms 就不占轨道了 */
    int64_t fixedTrackReuseMs;

    /* ------------------------------------------------------------------
     * 渲染设置（弹幕面板右页那几项，随每条弹幕下发）
     * ------------------------------------------------------------------ */

    /** 粗体，默认 true */
    bool fontBold;
    /** 描边类型：0 重墨 / 1 描边 / 2 45°投影，默认 0 */
    int outlineType;

    /* ------------------------------------------------------------------
     * 尺寸口径（QML:580-585）：宽高都是"整盒"尺寸，运动和碰撞都用它
     * ------------------------------------------------------------------ */

    /** 固定弹幕左右内边距，默认 8 */
    float fixedPadH;
    /** 本人弹幕左右内边距（非固定时），默认 6 */
    float selfPadH;
    /** 固定/本人弹幕上下内边距，默认 2 */
    float contentPadV;
    /** 本人弹幕边框宽，默认 2（画成白框） */
    float selfBorderWidth;

    /* ------------------------------------------------------------------
     * 悬停
     * ------------------------------------------------------------------ */

    /** 悬停多久自动恢复（毫秒，播放时钟计），默认 3000；<= 0 表示不自动恢复 */
    int64_t hoverTimeoutMs;
    /** 底部固定弹幕是否改成"从最下面一条轨道往上找"。
     *  默认 false = 与 QML/参考一致（从上往下找第一条不冲突的轨道）；
     *  true 则是字面意义上的"贴底"（README:705-709 描述的那种）。 */
    bool bottomFixedFromBottom;

    DanmakuConfig();
};

/* ===========================================================================
 * 单条弹幕数据（对应 types.ts:95-120 的 DanmakuItem）
 * =========================================================================== */
struct DANMAKU_API DanmakuItem {
    /** 不透明 id：0 = 交给引擎分配（自分配 id 从 1 开始递增）。
     *  非 0 时引擎原样使用，宿主请自行保证唯一（渲染层通常拿它当资源池的 key） */
    int64_t id;
    /** 文本，UTF-8 */
    std::string text;
    /** 出现时间（毫秒，播放时间轴）。参考里是秒，这里统一成毫秒 */
    int64_t timeMs;
    /** 类型，取 DanmakuType；1/2/3 有效，其它值（含 Advanced=4）会被丢弃 */
    int type;
    /** 字号，0 = 用配置算出来的字号 */
    int fontSize;
    /** 速度档位 1..5，0 = 用配置的 defaultSpeed */
    int speed;
    /** 颜色，0xRRGGBBAA（白 = 0xFFFFFFFF）。见 danmakuColorFromHex() */
    uint32_t colorRGBA;
    /** 是否本人发送（引擎也会认 uid == "1"，与参考/QML 一致） */
    bool isSelf;
    /** 发送者 uid，字符串；"1" 视为本人 */
    std::string uid;

    DanmakuItem();
};

/* ===========================================================================
 * 渲染项：每帧宿主真正要画的东西
 *
 * 文字是**指针**：指向引擎内部稳定存储（std::deque 里的 std::string）。
 * 有效期到下一次 load()/reset()/clear()/append() 之前，之后必须重新取
 * activeItems()。也就是说：宿主每帧调完 update() 再取 activeItems() 用，
 * 不要把 RenderItem::text 存起来跨帧用。
 *
 * 锚点：
 *   * 滚动弹幕：x/y 是整盒左上角
 *   * 固定弹幕（TOP/BOTTOM）：整盒水平居中，x/y 仍是左上角；缩放要绕**中心**
 *     （QML:643 transformOrigin = Center），所以渲染时用 scale 绕 (x+w/2, y+h/2)
 * =========================================================================== */
struct DANMAKU_API RenderItem {
    /** 弹幕 id（= DanmakuItem::id，或引擎分配的 id）。同一条弹幕在生命周期内不变 */
    int64_t id;
    /** UTF-8 文本指针，见上面的有效期说明 */
    const char *text;
    /** text 的字节长度（省掉渲染层的 strlen） */
    size_t textLength;
    /** 整盒左上角 x（像素） */
    float x;
    /** 整盒左上角 y（像素） */
    float y;
    /** 整盒宽（文字宽 + 内边距 + 边框） */
    float width;
    /** 整盒高（字号 + 内边距 + 边框） */
    float height;
    /** 文字前进宽度（不含内边距/边框），用来摆文字 */
    float textAdvance;
    /** 文字左右内边距 */
    float padH;
    /** 文字上下内边距 */
    float padV;
    /** 字号（像素） */
    int fontPixelSize;
    /** 最终 alpha：整体透明度 × 淡入淡出；悬停中的那条强制 1.0 */
    float alpha;
    /** 缩放：固定弹幕淡入 0.8→1、淡出 1→0.9；滚动弹幕恒为 1 */
    float scale;
    /** 颜色 0xRRGGBBAA */
    uint32_t colorRGBA;
    /** 类型 DanmakuType */
    int type;
    /** 是否本人弹幕（渲染层据此画白框，框宽 = borderWidth） */
    bool isSelf;
    /** 边框宽（非本人为 0） */
    float borderWidth;
    /** 是否粗体（渲染设置随每条弹幕下发） */
    bool bold;
    /** 描边类型 0/1/2 */
    int outlineType;
    /** 所在轨道下标；-1 = 不占轨道（例如轨道重建过） */
    int trackIndex;
    /** 是否是当前悬停目标 */
    bool hovered;
    /** 播放进度 0..1（滚动：0 = 刚进右边缘；固定：0..1 映射淡入/停留/淡出） */
    float progress;
    /** 本条的总时长（毫秒） */
    double durationMs;

    RenderItem();
};

/** 参考实现里的名字，方便对照 */
typedef RenderItem DanmakuRenderItem;

/* ===========================================================================
 * 过滤器（types.ts:55-62 + QML:534-557 的 _isFiltered/_spawn）
 *
 * 语义：置 true = **过滤掉**这类弹幕。只影响新准入的弹幕，已经在屏上的不动。
 * =========================================================================== */
struct DANMAKU_API DanmakuFilter {
    /** 过滤滚动弹幕 */
    bool scroll;
    /** 过滤固定弹幕（顶部 + 底部） */
    bool fixed;
    /** 只过滤顶部固定（QML 扩展出来的键） */
    bool top;
    /** 只过滤底部固定（QML 扩展出来的键） */
    bool bottom;
    /** 过滤彩色弹幕（非白色） */
    bool colorful;
    /** 过滤高级弹幕（type 4） */
    bool advanced;

    DanmakuFilter();
};

/* ===========================================================================
 * 统计
 * =========================================================================== */
struct DANMAKU_API DanmakuStats {
    /** 当前在屏条数 */
    int activeCount;
    /** 当前轨道数（初始只开 70%，不够时会扩） */
    int trackCount;
    /** 轨道数上限（不含底部安全区，即扩展的硬上限） */
    int maxTrackCount;
    /** 累计成功准入条数 */
    int admitted;
    /** 累计丢弃条数（被过滤 / 没轨道 / 到 maxRenderCount / 类型不支持） */
    int dropped;
    /** 数据里还没发射的条数 */
    int pending;

    DanmakuStats();
};

/* ===========================================================================
 * sendDanmaku 的选项（对应 QML:1137-1150 的 options{type,color,fontSize,uid,speed}）
 * =========================================================================== */
struct DANMAKU_API DanmakuSendOptions {
    /** 类型，默认滚动 */
    int type;
    /** 字号，0 = 用配置算 */
    int fontSize;
    /** 速度档位 1..5，0 = 用配置 */
    int speed;
    /** 颜色 0xRRGGBBAA，默认白 */
    uint32_t colorRGBA;
    /** 是否本人，默认 true（自己发的） */
    bool self;
    /** 出现时间（毫秒），-1 = 用当前引擎时钟 */
    int64_t timeMs;
    /** id，0 = 引擎分配 */
    int64_t id;
    /** uid，默认 "1"（"1" 即本人） */
    std::string uid;

    DanmakuSendOptions();
};

/* ===========================================================================
 * 颜色小工具：0xRRGGBBAA（R 在最高字节）
 * =========================================================================== */

/** 解析 "#ffffff" / "#fff" / "white" / "0xRRGGBB" / "RRGGBBAA"；
 *  解析不出来返回 fallback。参考实现里颜色是 CSS 字符串，这里是数值 + 解析函数 */
DANMAKU_API uint32_t danmakuColorFromHex(const std::string &color, uint32_t fallback = 0xFFFFFFFFu);

/** 0xRRGGBBAA → "#rrggbbaa" */
DANMAKU_API std::string danmakuColorToHex(uint32_t colorRGBA);

/** RGB 分量是否全 255（colorful 过滤器用的判定，与参考的 isWhite 一致） */
DANMAKU_API bool danmakuColorIsWhite(uint32_t colorRGBA);

/** 拼一个颜色，a 默认不透明 */
DANMAKU_API uint32_t danmakuColorFromRgb(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);

#endif // DANMAKU_TYPES_H
