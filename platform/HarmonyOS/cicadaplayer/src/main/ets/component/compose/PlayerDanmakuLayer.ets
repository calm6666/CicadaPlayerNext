/*
 * 弹幕层 —— ArkTS 版，来源是 Compose 安卓版的
 *   components/danmaku/DanmakuModel.kt（Danmaku / DanmakuType / DanmakuSpeed / DanmakuConfig）
 *   components/danmaku/DanmakuGpu.kt（DanmakuSprite / DanmakuLaneEngine）
 *   components/danmaku/DanmakuView.kt（DanmakuView）
 *
 * 轨道规则照 Qt 引擎（滚动弹幕做安全距离 + 追尾判定；固定弹幕跨类型占轨，底部自下而上找空轨）
 * 原样搬过来，是纯计算、无副作用。
 *
 * 与 Compose 的差异（逐条）：
 *   · Compose 是"文字只在进场时排版一次 → ImageBitmap，之后每帧只 drawImage"（GPU blit）。
 *     ArkUI 这边的 Canvas 没有等价的位图缓存 API，所以改为每帧 `strokeText + fillText`
 *     直接画文字（同一套轨道/时钟/透明度算法，绘制手段不同）。字号/颜色/描边宽度、
 *     行高、安全距离、存活时长全部与 Compose 同值，视觉一致；
 *   · 帧时钟：Compose 用 `withFrameNanos`。这里用一个 16ms 的 UI 帧定时器推进，
 *     播放时前进、暂停时冻结；与播放位置的软/硬对齐阈值（250ms / 0.2）与 Compose 相同；
 *   · 位图 LRU 缓存（DanmakuBitmapCache）没有对应物，随之取消；
 *   · **弹幕上传/网络拉取不做**（DanmakuSender.kt / 弹幕接口那一套），见报告里的"有意不做"一节。
 */

import { PlayerSize } from '../theme/PlayerTheme';

/** 弹幕类型。 */
export enum DanmakuType {
  SCROLL = 0,
  TOP = 1,
  BOTTOM = 2
}

/** 弹幕速度（倍率与 Compose 的 DanmakuSpeed 一致）。 */
export enum DanmakuSpeed {
  SLOW = 0,
  NORMAL = 1,
  FAST = 2,
  VERY_FAST = 3
}

export function danmakuSpeedMultiplier(speed: DanmakuSpeed): number {
  if (speed === DanmakuSpeed.SLOW) {
    return 0.8;
  }
  if (speed === DanmakuSpeed.FAST) {
    return 1.2;
  }
  if (speed === DanmakuSpeed.VERY_FAST) {
    return 1.5;
  }
  return 1.0;
}

/** 一条弹幕。 */
export class Danmaku {
  id: string = '';
  text: string = '';
  /** 颜色（'#AARRGGBB'；Compose 是 Compose Color） */
  color: string = '#FFFFFFFF';
  textSize: number = PlayerSize.danmakuBaseTextSize;
  type: DanmakuType = DanmakuType.SCROLL;
  position: number = 0;
  /** 出现时间（毫秒，相对影片） */
  time: number = 0;

  constructor(text: string, time: number, type: DanmakuType = DanmakuType.SCROLL, color: string = '#FFFFFFFF') {
    this.id = `${time}-${text}`;
    this.text = text;
    this.time = time;
    this.type = type;
    this.color = color;
  }
}

/** 弹幕配置（GPU/Canvas 渲染层直接读，改完立刻生效）。 */
export class DanmakuConfig {
  isEnabled: boolean = true;
  speed: DanmakuSpeed = DanmakuSpeed.NORMAL;
  textSize: number = PlayerSize.danmakuBaseTextSize;
  opacity: number = 1;
  maxLines: number = 8;
  showTop: boolean = true;
  showBottom: boolean = true;
  showScroll: boolean = true;
}

/** 一条正在飞的弹幕。 */
export class DanmakuSprite {
  id: string = '';
  text: string = '';
  color: string = '#FFFFFFFF';
  widthPx: number = 0;
  laneIndex: number = 0;
  spawnClockMs: number = 0;
  durationMs: number = 0;
  type: DanmakuType = DanmakuType.SCROLL;

  constructor(id: string, text: string, color: string, widthPx: number, laneIndex: number,
    spawnClockMs: number, durationMs: number, type: DanmakuType) {
    this.id = id;
    this.text = text;
    this.color = color;
    this.widthPx = widthPx;
    this.laneIndex = laneIndex;
    this.spawnClockMs = spawnClockMs;
    this.durationMs = durationMs;
    this.type = type;
  }

  progress(clockMs: number): number {
    if (this.durationMs <= 0) {
      return 0;
    }
    const value: number = (clockMs - this.spawnClockMs) / this.durationMs;
    return value < 0 ? 0 : (value > 1 ? 1 : value);
  }

  elapsedMs(clockMs: number): number {
    return clockMs - this.spawnClockMs;
  }

  isExpired(clockMs: number): boolean {
    return this.elapsedMs(clockMs) >= this.durationMs;
  }
}

/**
 * 轨道引擎（规则照 Qt / Compose 的引擎）：
 *   · 滚动弹幕：安全距离判"塞不塞得下"，并做追尾判定（新弹幕更快且旧弹幕还在右半边就换道）；
 *   · 固定弹幕（顶/底）：**跨类型也占**；底部**自下而上**找空轨。
 */
export class DanmakuLaneEngine {
  /** 固定弹幕停留时长（Compose：FIXED_DURATION_MS） */
  static readonly FIXED_DURATION_MS: number = PlayerSize.danmakuFixedDurationMs;

  private laneCount: number = 1;
  private canvasWidthPx: number = 0;

  constructor(laneCount: number, canvasWidthPx: number) {
    this.laneCount = laneCount;
    this.canvasWidthPx = canvasWidthPx;
  }

  /** 滚动弹幕存活时长（整屏穿越约 8 秒，速度倍率放大则更短）。 */
  static scrollDurationMs(config: DanmakuConfig): number {
    const multiplier: number = danmakuSpeedMultiplier(config.speed);
    return PlayerSize.danmakuScrollDurationMs / (multiplier > 0.1 ? multiplier : 0.1);
  }

  /** 安全距离（字号越大越宽）。 */
  static safeDistancePx(textSizePx: number): number {
    return textSizePx * PlayerSize.danmakuSafeDistanceScale + PlayerSize.danmakuSafeDistanceExtra;
  }

  /** 同屏最多几行。 */
  static laneCountFor(canvasHeightPx: number, lineHeightPx: number, maxLines: number): number {
    if (lineHeightPx <= 0 || canvasHeightPx <= 0) {
      return 1;
    }
    const raw: number = Math.floor(canvasHeightPx / lineHeightPx);
    const max: number = maxLines > 1 ? maxLines : 1;
    const clamped: number = raw < 1 ? 1 : raw;
    return clamped > max ? max : clamped;
  }

  pickLane(type: DanmakuType, spriteWidthPx: number, durationMs: number,
    active: DanmakuSprite[], clockMs: number, safeDistancePx: number): number {
    if (type === DanmakuType.SCROLL) {
      return this.pickScrollLane(spriteWidthPx, durationMs, active, clockMs, safeDistancePx);
    }
    if (type === DanmakuType.TOP) {
      for (let lane = 0; lane < this.laneCount; lane++) {
        if (this.fixedLaneFree(lane, active, clockMs)) {
          return lane;
        }
      }
      return 0;
    }
    for (let lane = this.laneCount - 1; lane >= 0; lane--) {
      if (this.fixedLaneFree(lane, active, clockMs)) {
        return lane;
      }
    }
    return this.laneCount - 1;
  }

  /** 滚动弹幕的 x：从右侧进，滚到完全出左侧。 */
  scrollX(sprite: DanmakuSprite, clockMs: number): number {
    return this.canvasWidthPx - (this.canvasWidthPx + sprite.widthPx) * sprite.progress(clockMs);
  }

  private fixedLaneFree(lane: number, active: DanmakuSprite[], clockMs: number): boolean {
    for (let i = 0; i < active.length; i++) {
      const sprite: DanmakuSprite = active[i];
      if (sprite.laneIndex === lane && sprite.type !== DanmakuType.SCROLL && !sprite.isExpired(clockMs)) {
        return false;
      }
    }
    return true;
  }

  private pickScrollLane(newWidthPx: number, newDurationMs: number, active: DanmakuSprite[],
    clockMs: number, safeDistancePx: number): number {
    const w: number = this.canvasWidthPx;
    const minRemaining: number = safeDistancePx > 80 ? 2000 : 1000;

    for (let lane = 0; lane < this.laneCount; lane++) {
      let ok: boolean = true;
      for (let i = 0; i < active.length; i++) {
        const sprite: DanmakuSprite = active[i];
        if (sprite.laneIndex !== lane) {
          continue;
        }
        if (sprite.type !== DanmakuType.SCROLL) {
          if (!sprite.isExpired(clockMs)) {
            ok = false;
            break;
          }
          continue;
        }
        const existingX: number = this.scrollX(sprite, clockMs);
        if (existingX > w - newWidthPx - safeDistancePx * 2) {
          ok = false;
          break;
        }
        if (existingX + sprite.widthPx > 0) {
          if (sprite.durationMs - sprite.elapsedMs(clockMs) < minRemaining) {
            continue;
          }
          const newSpeed: number = (w + newWidthPx) / (newDurationMs > 1 ? newDurationMs : 1);
          const oldSpeed: number = (w + sprite.widthPx) / (sprite.durationMs > 1 ? sprite.durationMs : 1);
          if (newSpeed > oldSpeed && existingX > w / 2) {
            ok = false;
            break;
          }
        }
      }
      if (ok) {
        return lane;
      }
    }

    /* 全都塞不下：挑"最近最闲"的一条轨（最新一条弹幕最早的那一轨） */
    let bestLane: number = 0;
    let bestSpawn: number = Number.NEGATIVE_INFINITY;
    for (let lane = 0; lane < this.laneCount; lane++) {
      let latest: number = Number.NEGATIVE_INFINITY;
      let found: boolean = false;
      for (let i = 0; i < active.length; i++) {
        const sprite: DanmakuSprite = active[i];
        if (sprite.laneIndex === lane && sprite.type === DanmakuType.SCROLL) {
          if (!found || sprite.spawnClockMs > latest) {
            latest = sprite.spawnClockMs;
            found = true;
          }
        }
      }
      if (!found) {
        latest = Number.NEGATIVE_INFINITY;
      }
      if (lane === 0 || latest < bestSpawn) {
        bestSpawn = latest;
        bestLane = lane;
      }
    }
    return bestLane;
  }
}

/**
 * 弹幕层。播放时按帧推进时钟（暂停即冻结），与播放位置做软/硬对齐；
 * 轨道交给 DanmakuLaneEngine，不会随机落行、固定弹幕也不会重叠。
 */
@Component
export struct DanmakuView {
  /**
   * 弹幕数据（记录型类数组，没有方法）；用 @Prop 才能跟着父组件刷新
   * —— ArkUI 里无装饰器的成员只在子组件创建时赋值。
   */
  @Prop danmakus: Danmaku[] = [];
  /** 配置（类实例用普通成员传；父组件**原地改**它，引用始终是同一个） */
  config: DanmakuConfig = new DanmakuConfig();
  @Prop isPlaying: boolean = false;
  @Prop @Watch('onTimeChanged') currentTime: number = 0;
  /** 弹幕列表/配置变了就 +1，用来清掉正在飞的旧弹幕 */
  @Prop @Watch('onSourceChanged') revision: number = 0;

  private settings: RenderingContextSettings = new RenderingContextSettings(true);
  private ctx: CanvasRenderingContext2D = new CanvasRenderingContext2D(this.settings);
  private active: DanmakuSprite[] = [];
  private spawned: Set<string> = new Set<string>();
  private engine: DanmakuLaneEngine | null = null;
  private clockMs: number = 0;
  private lastTickMs: number = 0;
  private frameTimer: number = -1;
  private viewWidth: number = 0;
  private viewHeight: number = 0;
  private canvasReady: boolean = false;

  aboutToAppear(): void {
    this.clockMs = this.currentTime;
    this.frameTimer = setInterval(() => {
      this.tick();
    }, 16);
  }

  aboutToDisappear(): void {
    if (this.frameTimer >= 0) {
      clearInterval(this.frameTimer);
      this.frameTimer = -1;
    }
  }

  /** 显示开关/字号/速度变了：已有弹幕与时长失效，清掉重来（与 Compose 同款）。 */
  private onSourceChanged(): void {
    this.active = [];
    this.spawned = new Set<string>();
  }

  /** 与播放器位置对齐：小偏差软收敛（防抖），大偏差硬对齐（seek/切档）。 */
  private onTimeChanged(): void {
    const delta: number = this.currentTime - this.clockMs;
    if (delta > PlayerSize.danmakuResyncThresholdMs || delta < -PlayerSize.danmakuResyncThresholdMs) {
      this.clockMs = this.currentTime;
      this.active = [];
      this.spawned = new Set<string>();
    } else {
      this.clockMs += delta * PlayerSize.danmakuResyncFactor;
    }
  }

  private tick(): void {
    const now: number = Date.now();
    if (this.isPlaying) {
      if (this.lastTickMs > 0) {
        this.clockMs += now - this.lastTickMs;
      }
      this.lastTickMs = now;
    } else {
      this.lastTickMs = now;
    }
    this.advanceSprites();
    this.draw();
  }

  /** 进场：清过期 + 按轨道分配（文字宽度在进场时量一次）。 */
  private advanceSprites(): void {
    if (!this.config.isEnabled || !this.canvasReady) {
      this.active = [];
      this.spawned = new Set<string>();
      return;
    }

    const engine: DanmakuLaneEngine | null = this.engine;
    if (engine === null) {
      return;
    }

    const clock: number = this.clockMs;
    const alive: DanmakuSprite[] = [];
    for (let i = 0; i < this.active.length; i++) {
      if (!this.active[i].isExpired(clock)) {
        alive.push(this.active[i]);
      }
    }
    this.active = alive;

    const textSizePx: number = this.config.textSize;
    const lineHeightPx: number = textSizePx + PlayerSize.danmakuLineHeightExtra;
    const scrollDuration: number = DanmakuLaneEngine.scrollDurationMs(this.config);
    const safeDistance: number = DanmakuLaneEngine.safeDistancePx(textSizePx);

    this.ctx.font = `bold ${textSizePx}px sans-serif`;

    for (let i = 0; i < this.danmakus.length; i++) {
      const item: Danmaku = this.danmakus[i];
      if (this.spawned.has(item.id)) {
        continue;
      }
      const age: number = clock - item.time;
      if (age < 0 || age > PlayerSize.danmakuSpawnWindowMs) {
        continue;
      }
      let show: boolean = this.config.showScroll;
      if (item.type === DanmakuType.TOP) {
        show = this.config.showTop;
      } else if (item.type === DanmakuType.BOTTOM) {
        show = this.config.showBottom;
      }
      if (!show) {
        this.spawned.add(item.id);
        continue;
      }

      const width: number = this.ctx.measureText(item.text).width;
      const duration: number = item.type === DanmakuType.SCROLL
        ? scrollDuration
        : DanmakuLaneEngine.FIXED_DURATION_MS;
      const lane: number = engine.pickLane(item.type, width, duration, this.active, clock, safeDistance);

      this.active.push(new DanmakuSprite(
        item.id, item.text, item.color, width, lane, clock, duration, item.type
      ));
      this.spawned.add(item.id);
    }

    while (this.active.length > PlayerSize.danmakuMaxActive) {
      this.active.shift();
    }
  }

  /** 绘制：每帧重画当前在飞的弹幕（黑描边 + 实心文字，B 站风格）。 */
  private draw(): void {
    if (!this.canvasReady) {
      return;
    }
    this.ctx.clearRect(0, 0, this.viewWidth, this.viewHeight);
    const engine: DanmakuLaneEngine | null = this.engine;
    if (!this.config.isEnabled || engine === null) {
      return;
    }

    const clock: number = this.clockMs;
    const textSizePx: number = this.config.textSize;
    const lineHeightPx: number = textSizePx + PlayerSize.danmakuLineHeightExtra;

    this.ctx.textBaseline = 'top';
    this.ctx.font = `bold ${textSizePx}px sans-serif`;
    this.ctx.lineWidth = PlayerSize.danmakuStrokeWidth;
    this.ctx.strokeStyle = '#FF000000';

    for (let i = 0; i < this.active.length; i++) {
      const sprite: DanmakuSprite = this.active[i];
      const x: number = sprite.type === DanmakuType.SCROLL
        ? engine.scrollX(sprite, clock)
        : (this.viewWidth - sprite.widthPx) / 2;

      let alpha: number = this.config.opacity;
      if (sprite.type !== DanmakuType.SCROLL) {
        const remain: number = sprite.durationMs - sprite.elapsedMs(clock);
        const ratio: number = remain / PlayerSize.danmakuFadeOutMs;
        alpha = this.config.opacity * (ratio < 0 ? 0 : (ratio > 1 ? 1 : ratio));
      }
      if (alpha <= 0.01) {
        continue;
      }

      this.ctx.globalAlpha = alpha;
      const y: number = sprite.laneIndex * lineHeightPx + 4;
      this.ctx.strokeText(sprite.text, x, y);
      this.ctx.fillStyle = sprite.color;
      this.ctx.fillText(sprite.text, x, y);
    }
    this.ctx.globalAlpha = 1;
  }

  build() {
    Canvas(this.ctx)
      .width('100%')
      .height('100%')
      /* 弹幕层只是画布：不吃触摸，手势一律交给下面的手势层 */
      .hitTestBehavior(HitTestMode.None)
      .onReady(() => {
        this.canvasReady = true;
      })
      .onAreaChange((oldValue: Area, newValue: Area) => {
        this.viewWidth = Number(newValue.width);
        this.viewHeight = Number(newValue.height);
        const textSizePx: number = this.config.textSize;
        const lineHeightPx: number = textSizePx + PlayerSize.danmakuLineHeightExtra;
        const laneCount: number = DanmakuLaneEngine.laneCountFor(this.viewHeight, lineHeightPx, this.config.maxLines);
        this.engine = new DanmakuLaneEngine(laneCount, this.viewWidth);
      })
  }
}
