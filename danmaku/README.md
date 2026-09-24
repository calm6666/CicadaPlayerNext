# Danmaku

**独立、可复用的纯 C++11 弹幕（bullet comment）引擎。**

只做逻辑：什么时候准入、放哪条轨道、现在在哪、透明度多少。**不画东西**——渲染
（Qt Quick / Skia / Canvas / OpenGL）由宿主自己做，引擎每帧给一份"该画什么"的清单。

- 纯标准库（`string / vector / deque / map / functional / cstdint`），**没有 Qt、没有
  CicadaPlayer 框架、没有第三方库**
- 没有全局/静态可变状态，同进程里可以同时跑多个 `DanmakuEngine` 互不影响
- 不读墙钟、不用随机数：**同样配置 + 同样数据 + 同样时间序列 → 同样输出**（可复现）
- 公开头文件按 C++11 写；单线程使用（多线程请自己加锁）
- 公开接口内部把异常全部吞掉（包括宿主自己的测量回调抛的），不会让异常穿出 API

行为是从两份现有实现搬过来的（数值出处逐条标在代码注释里）：

| 来源 | 位置 | 本模块里的角色 |
| --- | --- | --- |
| QML 实现 | `CicadaPlayerNext/platform/QtPlayer/DanmakuLayer.qml` | **行为基准**（当前 Qt 播放器的实现） |
| 原始参考 | `front/hilihili-components/src/utils/danmaku`（README + types / trackManager / domEngine / scheduler / scaleHelper） | 确认语义与命名，个别地方 QML 改过就以 QML 为准 |

---

## 1. 目录结构

```
danmaku/
├── CMakeLists.txt                     独立工程，也能 add_subdirectory
├── include/Danmaku/
│   ├── Danmaku.h                总入口（包含下面三个）
│   ├── DanmakuExport.h                DANMAKU_API 导出宏
│   ├── DanmakuTypes.h                 DanmakuConfig / DanmakuItem / RenderItem / 过滤 / 统计 / 颜色
│   └── DanmakuEngine.h                class DanmakuEngine
├── src/
│   ├── DanmakuTypes.cpp               默认值（数值出处）+ 颜色工具
│   └── DanmakuEngine.cpp              引擎（逐段对应 QML 里的同名函数）
├── tests/selftest.cpp                 main() + CHECK，无测试框架
└── README.md
```

---

## 2. 五分钟上手

```cpp
#include <Danmaku/Danmaku.h>

DanmakuEngine engine;

// 1) 文字宽度必须由宿主告诉引擎（引擎不知道你用什么字体）
engine.setMeasureFunction([](const std::string &text, int px, bool bold) -> float {
    // 例：QFontMetricsF fm(QFont(fontFamily, -1, bold ? QFont::Bold : QFont::Normal));
    //     fm.setPixelSize(px); return fm.horizontalAdvance(QString::fromUtf8(text.c_str()));
    return (float)text.size() * (float)px * 0.6f;   // 这里只是示例
});

// 2) 配置（拷贝一份再改）
DanmakuConfig cfg = engine.config();
cfg.containerWidth  = 1920.0f;      // 弹幕层尺寸，宿主必须给真实值
cfg.containerHeight = 1080.0f;
cfg.areaRatio       = 1.0f;         // 0.25 / 0.5 / 0.75 / 1
engine.setConfig(cfg);

// 3) 载入数据（内部按 timeMs 升序稳定排序，不要求宿主先排好）
std::vector<DanmakuItem> items = fetchDanmaku();
engine.load(items);

// 4) 每帧
void onFrame(int64_t playerPositionMs) {
    engine.update(playerPositionMs);            // 退场 + 准入 + 分轨 + 算位置
    for (const RenderItem &it : engine.activeItems())
        renderer.draw(it);                      // 宿主只管画
    for (int64_t id : engine.retiredIds())
        renderer.release(id);                   // 退场的 id，释放缓存资源
    playerHud.showTotal(engine.stats().admitted);   // 统计（可选）
}
```

关键点：

* **`update()` 每帧调一次**，传播放时间（毫秒）。引擎自己不做时钟平滑——QML 那套
  "每帧最多推 100ms + 漂移补 5%" 是播放器时钟的活，交给宿主。
* `activeItems()` 返回的是**引擎内部成员向量的引用**，下一次 `update()` 之前有效，
  不会每帧新建容器。顺序 = 准入顺序（先准入的在前，后来的在上层）。
* `RenderItem::text` 是**指针**，指向引擎内部稳定存储（`std::deque` 里的
  `std::string`）：有效期到下一次 `load() / reset() / clear() / append()` 之前。
  不要跨帧缓存这个指针，每帧重新取 `activeItems()`。

---

## 3. 语义速查（参考实现里搬来的数值）

| 项目 | 规则 | 出处 |
| --- | --- | --- |
| 三种类型 | 滚动 1 / 顶部固定 2 / 底部固定 3（高级 4 不渲染，直接丢弃） | types.ts:9-18 |
| 滚动时长 | `distance = 容器宽 + 整盒宽`，`duration = distance / (150 × 档位倍率) × 1000` | domEngine.ts:543-549 |
| 速度档位倍率 | `{0.5, 0.75, 1.0, 1.5, 2.0}`（档位 1..5，默认 3） | domEngine.ts:52-58 |
| 滚动位置 | `x = 容器宽 − (容器宽 + 宽) × progress`，progress = 已播时长 / 时长（夹 0..1） | QML:815-827 |
| 滚动退场 | `已播时长 >= 时长` → 退场、释放轨道 | QML:821-826 |
| 固定时长 | `4000ms` | domEngine.ts:551 |
| 固定动画 | 0→10% 透明度 0→1、缩放 0.8→1；10→90% 停留；90→100% 透明度 →0、缩放 →0.9 | domEngine.ts:361-378 |
| 轨道高度/间距 | `trackHeight = 24`、`trackGap = 4` | trackManager.ts:45-46 |
| 上下边距 | `topMargin = bottomMargin = 10` | trackManager.ts:47-48 |
| 区域 | `areaHeight = floor(容器高 × areaRatio)`（本模块还支持 `areaTop/areaBottom` 再让出，默认 0） | trackManager.ts:78 |
| 轨道数上限 | `floor((areaHeight − top − bottom) / (24 + 4))` | trackManager.ts:84 |
| 初始轨道数 | `max(1, floor(上限 × 0.7))`（只开 70%，不够再扩） | trackManager.ts:87 |
| 底部安全区 | 默认 `80px`：初始不开，轨道不够时扩进去（上限用"不减安全区"的那份） | index.ts:166 / README:737 |
| 轨道扩展 | 不够时 `+5 / +4` 交替扩，且单条 y+高 不超出区域 | trackManager.ts:137-138 |
| 碰撞判定（滚动） | ①`existingX > 容器宽 − 新宽 − 安全距离×2` → 冲突；②旧弹幕还在屏内且新弹幕更快且旧弹幕在右半边 → 冲突；③旧弹幕剩余 < `安全距离>80 ? 2000 : 1000` ms → 不算冲突 | trackManager.ts:284-321 |
| 动态安全距离 | 按"平均密度"分档：`≤1→150`、`≤5→100`、`≤15→60`、`≤25→40`、否则 `20` | trackManager.ts:249-274 |
| 顶部固定 | 只用**前 1/3** 轨道 | trackManager.ts:335 |
| 底部固定 | 从上往下找，但**不用最下面 20%** 的轨道 | trackManager.ts:358-364 |
| 固定轨道复用 | 同类固定弹幕剩余 `<= 500ms` 就不算占位 | trackManager.ts:344, 373 |
| 一帧准入上限 | 最多 10 条（`maxEmitsPerFrame`） | index.ts:238 |
| 同时在屏上限 | `maxRenderCount = 2000`（README 里的 500 是旧值） | index.ts:128 |
| 分段 | 30 秒一段，建"分段起点表"+ 前进游标，只有 seek 才二分 | QML:292-347 |
| seek | 跳变 `> 1000ms` → 清场重来；回退 `> 50ms` → 只在屏不动、重置发射标记 | QML:883-906 |
| 字号 | `(自带字号 || 基准字号) × fontSizeScale × (autoScale ? 自动缩放比例 : 1)`，基准字号那条路径夹到 12..36 | QML:139-145, 569-572 |
| 自动缩放 | `clamp(弹幕层宽 / 1280, 0.75, 1.5)`（**不含**参考实现的 `dpr^-0.3`，见 QML:147-158） | QML:159-162 |
| 尺寸口径 | 宽高都是**整盒**：文字宽 + 左右内边距 + 边框；高 = 字号 + 上下内边距 + 边框 | QML:580-585 |
| 内边距/边框 | 固定弹幕 `padH = 8`；本人弹幕 `padH = 6`、白框 `2px`；固定或本人 `padV = 2` | QML:580-582 |
| 中文字号兜底 | 没有测量回调时：`max(字号, 码点数 × 字号 × 0.6)`，再向上取整 | QML:655-666 |
| 悬停 | 鼠标动过才重做命中检测（一帧最多一次）；命中后**该条冻结**（已播时长不涨），强制不透明；满 `3000ms`（播放时钟计）自动恢复 | QML:999-1091 |
| 过滤 | `scroll / fixed / top / bottom / colorful / advanced`，置 true = 过滤掉；**立即生效，只影响新准入的弹幕** | QML:534-557 |
| 本人弹幕 | `isSelf` 或 `uid == "1"` → 白框（`borderWidth = 2`） | QML:573 / domEngine.ts:516 |
| 设置生效范围 | 速度/区域/字号/透明度等设置**只影响新弹幕**；唯一例外是 `setContainerSize()`（在屏弹幕实时重算，跟 QML 的实时缩放一致） | README:1565-1573 / QML:937-972 |

---

## 4. 公开 API

```cpp
// ---- 类型（DanmakuTypes.h）-----------------------------------------------
enum DanmakuType { DanmakuTypeScroll = 1, DanmakuTypeTop = 2,
                   DanmakuTypeBottom = 3, DanmakuTypeAdvanced = 4 };

typedef std::function<float(const std::string &text, int fontPixelSize, bool bold)>
        DanmakuMeasureFn;

struct DanmakuConfig { /* 见下面字段表 */ DanmakuConfig(); };
struct DanmakuItem   { int64_t id; std::string text; int64_t timeMs; int type;
                       int fontSize; int speed; uint32_t colorRGBA; bool isSelf;
                       std::string uid; DanmakuItem(); };
struct RenderItem    { int64_t id; const char *text; size_t textLength;
                       float x, y, width, height, textAdvance, padH, padV;
                       int fontPixelSize; float alpha, scale; uint32_t colorRGBA;
                       int type; bool isSelf; float borderWidth; bool bold;
                       int outlineType; int trackIndex; bool hovered;
                       float progress; double durationMs; RenderItem(); };
typedef RenderItem DanmakuRenderItem;

struct DanmakuFilter  { bool scroll, fixed, top, bottom, colorful, advanced; DanmakuFilter(); };
struct DanmakuStats   { int activeCount, trackCount, maxTrackCount, admitted, dropped, pending;
                        DanmakuStats(); };
struct DanmakuSendOptions { int type, fontSize, speed; uint32_t colorRGBA; bool self;
                            int64_t timeMs, id; std::string uid; DanmakuSendOptions(); };

uint32_t danmakuColorFromHex(const std::string &color, uint32_t fallback = 0xFFFFFFFFu);
std::string danmakuColorToHex(uint32_t colorRGBA);       // 0xRRGGBBAA → "#rrggbbaa"
bool danmakuColorIsWhite(uint32_t colorRGBA);
uint32_t danmakuColorFromRgb(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);

// ---- 引擎（DanmakuEngine.h）---------------------------------------------
class DanmakuEngine {
public:
    typedef DanmakuMeasureFn MeasureFn;
    typedef DanmakuStats Stats;

    DanmakuEngine();
    ~DanmakuEngine();

    // 配置
    void setConfig(const DanmakuConfig &config);
    const DanmakuConfig &config() const;
    void setContainerSize(float width, float height);

    // 数据
    void clear();                                   // 清屏 + 重置发射记录，数据保留
    void reset();                                   // 清屏 + 丢数据
    void load(const std::vector<DanmakuItem> &items);
    void append(const DanmakuItem &item);
    size_t itemCount() const;
    const DanmakuItem *itemAt(size_t index) const;

    // 时间
    void seekTo(int64_t timeMs);                    // 只重定位，不发射（下一帧 update 才发）
    void update(int64_t timeMs);                     // 每帧调一次
    int64_t clockMs() const;

    // 渲染输出
    const std::vector<RenderItem> &activeItems() const;
    const std::vector<int64_t> &retiredIds() const;  // 本次退场的 id

    // 文字测量
    void setMeasureFunction(MeasureFn fn);
    void setMeasuredWidth(int64_t id, float widthPx); // 优先于回调
    void clearMeasuredWidth(int64_t id);
    void clearMeasuredWidths();
    float measureText(const std::string &text, int fontPixelSize, bool bold) const;

    // 暂停 / 悬停
    void setPaused(bool paused);
    bool isPaused() const;
    void setPointerPosition(float x, float y, bool inside); // 层内坐标
    int64_t hoveredId() const;                              // 0 = 没有

    // 过滤
    void setFilter(const DanmakuFilter &filter);
    const DanmakuFilter &filter() const;
    void resetFilter();

    // 立即显示 / 发送
    bool addDanmaku(const DanmakuItem &item, int64_t currentMs = -1);
    int  addDanmakuBatch(const std::vector<DanmakuItem> &items, int64_t currentMs = -1);
    bool sendDanmaku(const std::string &text,
                     const DanmakuSendOptions &options = DanmakuSendOptions());

    // 轨道 / 统计 / 分段
    int trackCount() const;
    float trackY(int index) const;
    Stats stats() const;
    int  segmentIndexForTimeMs(int64_t timeMs) const;
    int  segmentCount() const;
    bool segmentRange(int segmentIndex, size_t &begin, size_t &end) const;

    // 工具 / 维护
    int currentFontSize() const;
    float autoScaleFactor() const;
    void rebuildTracks();
    void rescaleActive();
};
```

### DanmakuConfig 字段（默认值）

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `containerWidth` / `containerHeight` | `1280` / `720` | 弹幕层尺寸，宿主必须设成真实值 |
| `areaRatio` | `1.0` | 区域档位 0.25 / 0.5 / 0.75 / 1 |
| `areaTop` / `areaBottom` | `0` | 区域上下再让出的像素（0 = 与 QML 完全一致） |
| `scrollSpeedPxPerSec` | `150` | 基础速度（px/秒） |
| `speedMultipliers[5]` | `{0.5,0.75,1,1.5,2}` | 5 档倍率 |
| `defaultSpeed` | `3` | 默认速度档位 |
| `fixedDurationMs` | `4000` | 固定弹幕时长 |
| `baseFontSize` | `18` | 基准字号（= 参考的 `options.fontSize`） |
| `fontSizeScale` | `1.0` | 用户字号缩放 |
| `autoScale` | `true` | 是否随层宽自动缩放 |
| `autoScaleBaseWidth` | `1280` | 自动缩放基准宽 |
| `autoScaleMin` / `autoScaleMax` | `0.75` / `1.5` | 自动缩放夹取（上限跟 QML 取 1.5） |
| `fontSizeMin` / `fontSizeMax` | `12` / `36` | 字号夹取（只作用于"用基准字号"那条路径） |
| `trackHeight` / `trackGap` | `24` / `4` | 轨道尺寸 |
| `topMargin` / `bottomMargin` | `10` / `10` | 上下边距 |
| `bottomSafeArea` | `80` | 底部安全区（字幕区） |
| `initialTrackRatio` | `0.7` | 初始只开 70% 轨道 |
| `opacity` | `1.0` | 整体透明度 |
| `maxRenderCount` | `2000` | 同时在屏上限 |
| `segmentDurationSec` | `30` | 分段时长 |
| `maxEmitsPerFrame` | `10` | 一帧最多准入几条 |
| `jumpThresholdMs` | `1000` | 超过就当 seek |
| `rewindToleranceMs` | `50` | 回退容忍 |
| `fixedTrackReuseMs` | `500` | 固定弹幕轨道复用阈值 |
| `fontBold` | `true` | 粗体（随每条弹幕下发） |
| `outlineType` | `0` | 0 重墨 / 1 描边 / 2 45°投影 |
| `fixedPadH` / `selfPadH` / `contentPadV` / `selfBorderWidth` | `8` / `6` / `2` / `2` | 尺寸口径用的内边距与白框宽 |
| `hoverTimeoutMs` | `3000` | 悬停自动恢复（播放时钟计；`<= 0` 关闭） |
| `bottomFixedFromBottom` | `false` | `true` = 底部固定改成从最下面一条轨道往上找（可选，默认与 QML 一致） |

### RenderItem 怎么用

* **锚点**：滚动弹幕 `x/y` 是整盒左上角；固定弹幕整盒水平居中，`x/y` 仍是左上角。
* **缩放**：固定弹幕 `scale` 在 0.8→1→0.9 之间走，渲染时要**绕整盒中心**
  （中心 = `x + width/2, y + height/2`），滚动弹幕 `scale` 恒为 1。
* **文字摆放**：文字区域 = `x + padH, y + padV`，宽度用 `textAdvance`；
  白框（`isSelf`）用 `borderWidth`、整盒轮廓用 `width/height` 画。
* **透明度**：直接用 `alpha`（已含 `opacity × 淡入淡出`；悬停中的那条是 1.0）。
* **颜色**：`colorRGBA` 是 `0xRRGGBBAA`（R 在最高字节）。
* **资源池**：用 `id` 当 key 缓存文字/贴图对象，`retiredIds()` 里的 id 记得释放。

---

## 5. 单独构建

```bash
cmake -S . -B build
cmake --build build --config Release

# 自测（默认不编）
cmake -S . -B build -DDANMAKU_BUILD_TESTS=ON
cmake --build build --config Release
./build/danmaku_selftest          # Windows: build\Release\danmaku_selftest.exe
# 或： ctest --test-dir build -C Release
```

选项：

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `DANMAKU_BUILD_SHARED` | `ON` | ON = 动态库（Windows 上是 `Danmaku.dll`）；OFF = 静态库 |
| `DANMAKU_BUILD_TESTS` | `OFF` | 编 `danmaku_selftest` 可执行文件并注册 ctest 用例 |

链接库名都是 `Danmaku`。安装规则只在"独立构建"时挂上（`add_subdirectory`
时不会往父工程塞 install）：`cmake --install build --prefix <dir>`。

## 6. 嵌到别的工程

```cmake
add_subdirectory(third_party/danmaku)     # 或者 add_subdirectory(CicadaPlayerNext/danmaku)
target_link_libraries(your_target PRIVATE Danmaku)
```

* 动态库：使用方不需要任何宏。
* 静态库（`-DDANMAKU_BUILD_SHARED=OFF`）：本模块会用 `PUBLIC` 透出
  `DANMAKU_STATIC`；如果宿主不用 CMake、手写编译命令，就要自己加
  `-DDANMAKU_STATIC`（否则 Windows 上会按 `dllimport` 处理）。
* 导出宏：`DANMAKU_API`（`include/Danmaku/DanmakuExport.h`），
  编译本库时由 `DANMAKU_BUILDING` 决定 `dllexport`，使用方走 `dllimport`。
* 头文件按 `<Danmaku/Xxx.h>` 包含（`include/` 已经在目标的 include 路径上）。

### Qt 侧接入示例

```cpp
// 测量：QFontMetricsF 和渲染用同一套字体
struct QtMeasure {
    QString family; bool bold;
    float operator()(const std::string &text, int px, bool b) const {
        QFont f(family); f.setPixelSize(px); f.setBold(b || bold);
        QFontMetricsF fm(f);
        return (float)fm.horizontalAdvance(QString::fromUtf8(text.c_str(), (int)text.size()));
    }
};
engine.setMeasureFunction(QtMeasure{"Microsoft YaHei", true});

// 成员（不要绑 QML 绑定）：m_engine; m_measure; m_frame(frameless)
// 每帧：FrameAnimation.onTriggered → m_engine.update(playerPosition); 遍历 activeItems() 更新视图
// 悬停：画面区 MouseArea 喂 setPointerPosition(x, y, inside)（QML:985-997 说的"最上层独占"问题，
//       所以位置由外面喂进来，弹幕层自己不抢 hover）
```

---

## 7. 已知行为与取舍（和参考实现/README 不一致的地方，都在这儿说明）

1. **`dpr^-0.3` 去掉了**。参考的 `scaleHelper.calculateScale` 会乘 `dpr^-0.3`，QML 把它去掉了
   （Qt 的逻辑像素已经含系统缩放，再乘一次等于双重缩小），上限也从 1.25 提到 1.5。
   本模块跟 QML。见 `autoScaleFactor()`。
2. **`maxRenderCount` 取 2000**。参考 README 写的是 500，实现（`index.ts:128`）是 2000；
   按实现算。
3. **高级弹幕（type 4）不渲染，直接丢弃**。参考有 `showAdvanced` 开关但两套实现都没画；
   QML 里 type 4 会掉进"固定弹幕"分支被当成未知类型画出来，这里选择明确丢弃。
4. **底部固定弹幕的语义**。README 说"从下方轨道开始分配"，但实现（trackManager.ts:358-364）
   是**从上往下找第一条不冲突的轨道**，只是跳过最下面 20%。默认跟实现/QML 一致；
   想要字面意义的"贴底"，把 `config.bottomFixedFromBottom = true`。
5. **顶部与底部固定可以共用一条轨道**。参考的 `getFixedTrack` 只比同类弹幕，所以同一时刻
   的 TOP 和 BOTTOM 可能落在同一条轨道上（视觉上会压在一起）。这是参考行为，保留。
6. **安全距离是启发式的**。同速同宽时能保证同轨道不重叠（selftest 逐帧验证了这一点）；
   但 1) 新旧弹幕速度差异很大、2) 宽度差异极大（例如一条超长弹幕）时，参考的判定可能放过
   一次追尾。本模块保持参考行为。
7. **回退后可能重发**。回退 `> 50ms` 会重置该时刻之后的发射标记，如果旧的那条还在屏上，
   同一个 id 可能短暂出现两份（参考与 QML 同样如此；`seekTo()` 会先清场，不会有这个问题）。
8. **悬停超时用播放时钟**。QML 用 3000ms 墙钟定时器，本模块改用播放时钟（`hoverStart + 3000`），
   这样同样输入才有同样输出；播放器暂停时不会自动恢复。
9. **`setContainerSize()` 会实时重算在屏弹幕**（字号/宽度/固定弹幕重新居中），这是 QML 按
   用户要求改过的实时缩放；其它配置项仍然是"只影响新弹幕"。
10. **`clear()` 保留数据**（对应 QML/参考 `DanmakuManager.clear()`）：只清屏 + 重置发射记录，
    游标跟当前时钟走。要连数据一起丢用 `reset()` 或 `load(空)`。
11. **`activeItems()` 的顺序**是准入顺序（不是按 y、不是按 id）。渲染层要按 z 序画就自己排，
    悬停目标在引擎里按"后准入的在上层"命中。

## 8. 自测覆盖

`tests/selftest.cpp` 用 `main()` + `CHECK` 宏（无测试框架），每条检查打印一行，退出码 = 失败数：

1. 基本准入 / 轨道 0 / 右边缘入场 / 整盒尺寸 / 非法类型丢弃
2. 文字测量（回调 / `setMeasuredWidth` / 兜底，含中文按码点数）
3. 大量弹幕同时准入的轨道分配（填满 + 扩展 +5/+4/+1 到 25 条上限，其余丢弃）
4. **同轨道滚动弹幕不重叠**（15 条、每 50ms 采样一次、逐对判定）
5. 固定顶部（前 1/3 轨道）/ 底部（避开最下面 20%）/ 居中 / 淡入淡出数值
6. 跑完退场（含 x < 0、退场 id 上报、退场时刻落在时长 ±100ms 内）
7. 悬停冻结（位置不动）→ 移开恢复 → 3 秒自动恢复
8. `seekTo` 前进 / 后退 / 大步跳过 / 中间不喷 / 小幅回退不清场
9. `maxRenderCount` 夹取（5 和 1）
10. **可复现性**（两个实例逐帧比对快照、穿插调用、重复跑、空转不漂）
11. 过滤器（colorful / scroll / bottom / advanced）+ 颜色解析
12. 本人弹幕白框 / `sendDanmaku` / 自定义 options
13. 全局暂停（时钟不动）
14. 容器尺寸 / 自动缩放 / 区域档位 / 实时缩放
15. 分段表 / `clear` / `reset` / `append`（中间插入与尾插）
