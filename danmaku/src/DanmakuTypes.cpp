// ===========================================================================
// Danmaku —— 类型与配置的默认值 + 颜色工具
//
// 这里的每一个默认值都能在两份参考实现里找到出处，注释标了位置。
// ===========================================================================
#include <Danmaku/DanmakuTypes.h>

#include <cstdio>

namespace {

inline int hexDigit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

std::string toLowerAscii(const std::string &s) {
    std::string out(s);
    for (size_t i = 0; i < out.size(); ++i) {
        char c = out[i];
        if (c >= 'A' && c <= 'Z')
            out[i] = (char)(c - 'A' + 'a');
    }
    return out;
}

} // namespace

/* ===========================================================================
 * DanmakuConfig
 * =========================================================================== */
DanmakuConfig::DanmakuConfig()
    : containerWidth(1280.0f),
      containerHeight(720.0f),
      areaRatio(1.0f),
      areaTop(0.0f),
      areaBottom(0.0f),
      scrollSpeedPxPerSec(150.0f),          // domEngine.ts:544 基础速度 150px/s
      defaultSpeed(3),                      // DanmakuSpeed.NORMAL
      fixedDurationMs(4000.0f),             // domEngine.ts:551 固定弹幕 4 秒
      baseFontSize(18),                     // types/index.ts:120 fontSize 默认 18
      fontSizeScale(1.0f),
      autoScale(true),                      // index.ts:121 默认开自动缩放
      autoScaleBaseWidth(1280.0f),          // scaleHelper.ts:23 设计稿宽 1280
      autoScaleMin(0.75f),                  // 下限 0.75
      autoScaleMax(1.5f),                   // QML:161 上限取 1.5（参考实现是 1.25，见 QML 注释）
      fontSizeMin(12),                      // scaleHelper.ts:88 字号夹到 12
      fontSizeMax(36),                      // scaleHelper.ts:88 字号夹到 36
      trackHeight(24.0f),                   // trackManager.ts:45
      trackGap(4.0f),                       // trackManager.ts:46
      topMargin(10.0f),                     // trackManager.ts:47
      bottomMargin(10.0f),                  // trackManager.ts:48
      bottomSafeArea(80.0f),                // index.ts:166 底部安全区 80px
      initialTrackRatio(0.7f),              // trackManager.ts:87 初始只开 70%
      opacity(1.0f),
      maxRenderCount(2000),                 // index.ts:128（README 里的 500 是旧值）
      segmentDurationSec(30.0f),            // index.ts:126 分段 30 秒
      maxEmitsPerFrame(10),                 // index.ts:238 一帧最多 10 条
      jumpThresholdMs(1000),                // QML:883 跳变 > 1000ms 当 seek
      rewindToleranceMs(50),                // QML:884 回退 > 50ms 重置发射标记
      fixedTrackReuseMs(500),               // trackManager.ts:344/373 剩余 <= 500ms 可复用
      fontBold(true),                       // QML:87 默认粗体
      outlineType(0),                       // QML:88 默认 0 重墨
      fixedPadH(8.0f),                      // QML:580 固定弹幕左右 padding 8
      selfPadH(6.0f),                       // QML:580 本人弹幕左右 padding 6
      contentPadV(2.0f),                    // QML:581 上下 padding 2
      selfBorderWidth(2.0f),                // QML:582 本人白框 2px
      hoverTimeoutMs(2000),                 // 悬停气泡的寿命：参考 dialog/index.ts:96-103 是 2000ms
      /*
       * 【★底部固定弹幕改成"从下往上找"★（默认 true，**有意偏离参考**）】
       *
       * 参考实现（trackManager.ts:356-364）里"底部固定"是从**上往下**找第一条空轨道的，
       * 只跳过最下面 20% —— 结果底部弹幕全挤在上半屏、和顶部弹幕抢同一批轨道
       * （再叠加"固定弹幕只跟同类比"，就出现用户实测的固定弹幕重叠）。
       *
       * 改成从最下面一条轨道往上找之后：
       *   * 底部弹幕真的显示在**下方**；
       *   * 它与顶部弹幕（只用前 1/3 轨道）天然错开，不再互相压。
       * 想回到参考行为就把这里改成 false。
       */
      bottomFixedFromBottom(true)
{
    /* 速度档位倍率（domEngine.ts:52-58） */
    speedMultipliers[0] = 0.5f;   // 1 极慢
    speedMultipliers[1] = 0.75f;  // 2 较慢
    speedMultipliers[2] = 1.0f;   // 3 适中
    speedMultipliers[3] = 1.5f;   // 4 较快
    speedMultipliers[4] = 2.0f;   // 5 极快
}

/* ===========================================================================
 * DanmakuItem
 * =========================================================================== */
DanmakuItem::DanmakuItem()
    : id(0),
      timeMs(0),
      type(DanmakuTypeScroll),
      fontSize(0),
      speed(0),
      colorRGBA(0xFFFFFFFFu),               // 默认白
      isSelf(false),
      uid()
{
}

/* ===========================================================================
 * RenderItem
 * =========================================================================== */
RenderItem::RenderItem()
    : id(0),
      text(0),
      textLength(0),
      x(0.0f),
      y(0.0f),
      width(0.0f),
      height(0.0f),
      textAdvance(0.0f),
      padH(0.0f),
      padV(0.0f),
      fontPixelSize(0),
      alpha(1.0f),
      scale(1.0f),
      colorRGBA(0xFFFFFFFFu),
      type(DanmakuTypeScroll),
      isSelf(false),
      borderWidth(0.0f),
      bold(false),
      outlineType(0),
      trackIndex(-1),
      hovered(false),
      progress(0.0f),
      durationMs(0.0)
{
}

/* ===========================================================================
 * DanmakuFilter
 * =========================================================================== */
DanmakuFilter::DanmakuFilter()
    : scroll(false), fixed(false), top(false), bottom(false), colorful(false), advanced(false)
{
}

/* ===========================================================================
 * DanmakuStats
 * =========================================================================== */
DanmakuStats::DanmakuStats()
    : activeCount(0), trackCount(0), maxTrackCount(0), admitted(0), dropped(0), pending(0)
{
}

/* ===========================================================================
 * DanmakuSendOptions（QML:1137-1150 的 options 默认值）
 * =========================================================================== */
DanmakuSendOptions::DanmakuSendOptions()
    : type(DanmakuTypeScroll),
      fontSize(0),
      speed(0),
      colorRGBA(0xFFFFFFFFu),
      self(true),
      timeMs(-1),
      id(0),
      uid("1")                                // uid == "1" 即本人
{
}

/* ===========================================================================
 * 颜色工具
 * =========================================================================== */

uint32_t danmakuColorFromRgb(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | (uint32_t)a;
}

bool danmakuColorIsWhite(uint32_t colorRGBA) {
    /* 与参考的 isWhite 一致：只看 RGB，忽略 alpha（参考比较的是 "#ffffff"/"#fff"/"white"） */
    return ((colorRGBA >> 24) & 0xFFu) == 0xFFu && ((colorRGBA >> 16) & 0xFFu) == 0xFFu &&
           ((colorRGBA >> 8) & 0xFFu) == 0xFFu;
}

std::string danmakuColorToHex(uint32_t colorRGBA) {
    static const char *digits = "0123456789abcdef";
    std::string out;
    out.reserve(9);
    out.push_back('#');
    for (int shift = 28; shift >= 0; shift -= 4)
        out.push_back(digits[(colorRGBA >> shift) & 0xFu]);
    return out;
}

uint32_t danmakuColorFromHex(const std::string &color, uint32_t fallback) {
    std::string s = toLowerAscii(color);

    /* 去掉空白 */
    std::string trimmed;
    trimmed.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            trimmed.push_back(c);
    }
    s = trimmed;

    if (s.empty())
        return fallback;

    /* 关键字（参考里颜色是 CSS 字符串，这里至少认常用的几个） */
    if (s == "white")
        return 0xFFFFFFFFu;
    if (s == "black")
        return 0x000000FFu;
    if (s == "red")
        return 0xFF0000FFu;
    if (s == "green")
        return 0x00FF00FFu;
    if (s == "blue")
        return 0x0000FFFFu;
    if (s == "yellow")
        return 0xFFFF00FFu;
    if (s == "transparent")
        return 0xFFFFFF00u;

    if (s.size() > 0 && s[0] == '#')
        s = s.substr(1);
    else if (s.size() > 2 && s[0] == '0' && s[1] == 'x')
        s = s.substr(2);

    if (s.empty())
        return fallback;

    /* 必须是纯十六进制 */
    for (size_t i = 0; i < s.size(); ++i) {
        if (hexDigit(s[i]) < 0)
            return fallback;
    }

    unsigned int value = 0;
    if (s.size() == 3) {
        /* #rgb → #rrggbb */
        unsigned int r = (unsigned int)hexDigit(s[0]);
        unsigned int g = (unsigned int)hexDigit(s[1]);
        unsigned int b = (unsigned int)hexDigit(s[2]);
        return danmakuColorFromRgb((uint8_t)(r * 17u), (uint8_t)(g * 17u), (uint8_t)(b * 17u), 255);
    }
    if (s.size() == 6) {
        for (size_t i = 0; i < 6; ++i)
            value = (value << 4) | (unsigned int)hexDigit(s[i]);
        return ((uint32_t)value << 8) | 0xFFu;   // RGB → RRGGBBAA
    }
    if (s.size() == 8) {
        for (size_t i = 0; i < 8; ++i)
            value = (value << 4) | (unsigned int)hexDigit(s[i]);
        return (uint32_t)value;
    }

    return fallback;
}
