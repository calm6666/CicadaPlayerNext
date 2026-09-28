/*
 * 播放器控制层 —— ArkTS 版，来源是 Compose 安卓版的
 *   platform/Android/ComposePlayer/app/src/main/java/com/cicada/player/compose/components/video/PlayerControls.kt
 *
 * 逐个对应：formatPlayerTime / PlayerThinProgressBar / PlayerProgressBar / SeekPreviewPopup /
 * PlayerBottomBar / BarText / DanmakuInputBar / PlayerTopBar。
 * 尺寸/位置/颜色全部走 theme/PlayerTheme.ets 里的常量（那一份的注释写了每个数在 Compose 的出处）。
 *
 * 布局手法上的等价替换（结果一致，机制不同）：
 *   · Compose 用 `BoxWithConstraints` 拿宽度再算 dp；ArkUI 这边栏内元素用百分比宽度
 *     （`width('37%')`）表达，只有需要 px 的地方（进度条 dot 的偏移、预览卡片的位置）
 *     才在 `.onAreaChange` 里记下容器宽度；
 *   · Compose 的 `Modifier.offset(x, y)` 浮层在这里是 Stack 里 `Alignment.TopStart` +
 *     `.offset({x, y})`（父容器显式给了宽高，浮层溢出不会被计入父尺寸）。
 *
 * 与 Compose 的差异（逐条）：
 *   · 预览图的"已就绪"判定：Compose 用 Coil 的 AsyncImagePainter.state（Success 才切卡片）。
 *     这里用 `Image.onComplete` 置位同一个语义；就绪前显示圆形加载占位（截图 4 的样子）；
 *   · 圆形加载指示：Compose 是 CircularProgressIndicator(18dp, strokeWidth 2dp)，
 *     ArkUI 用 `LoadingProgress`（尺寸可设，线宽是系统固定样式，不可配）；
 *   · 弹幕输入框的"发送"：Compose 在文字非空时清空输入；这里同款，空串不发；
 *   · 进度条的 `data:` 前缀帧：ArkUI 的 Image 直接支持 base64 data URI，所以不做
 *     Compose 那种"先 decode 成 Bitmap 再切卡片"的分支，两种来源统一交给 Image。
 */

import { PlayerSize, PlayerTheme } from '../theme/PlayerTheme';
import { DanmakuDotThumb, PlayPauseIconButton } from './LottieIcon';
import { IconButtonBox, PlayerBackIcon, PlayerIconButton } from './PlayerIcons';

/**
 * Compose：`formatPlayerTime(timeMs: Long)` —— 不足 1 小时是 mm:ss，超过是 hh:mm:ss，
 * 非正值一律 "00:00"。
 */
export function formatPlayerTime(timeMs: number): string {
  if (timeMs <= 0) {
    return '00:00';
  }
  const total: number = Math.floor(timeMs / 1000);
  const s: number = total % 60;
  const m: number = Math.floor(total / 60) % 60;
  const h: number = Math.floor(total / 3600);
  const ss: string = s < 10 ? `0${s}` : `${s}`;
  const mm: string = m < 10 ? `0${m}` : `${m}`;
  if (h > 0) {
    const hh: string = h < 10 ? `0${h}` : `${h}`;
    return `${hh}:${mm}:${ss}`;
  }
  return `${mm}:${ss}`;
}

/** 比例夹到 0..1（Compose 到处用的 coerceIn(0f, 1f)）。 */
function clamp01(value: number): number {
  if (value < 0) {
    return 0;
  }
  return value > 1 ? 1 : value;
}

/** 预览帧每 5 秒一档（取帧语义：和上游预览接口 videoshot/preview.bin 一致，不是 UI 数值）。 */
const PREVIEW_FRAME_INTERVAL_MS: number = 5000;

// ---------------------------------------------------------------------------
// 常驻细进度条
// ---------------------------------------------------------------------------

/**
 * **常驻细进度条**（控制栏隐藏后仍然显示，贴在播放器**最底部**）。
 * 已播 = 主题色，未播 = 白 20%，中间压一层已缓冲（白 30%）。**不带 dot**。
 */
@Component
export struct PlayerThinProgressBar {
  @Prop positionMs: number = 0;
  @Prop durationMs: number = 0;
  @Prop bufferedMs: number = 0;
  /** Compose：trackHeight = 2.dp */
  trackHeight: number = PlayerSize.thinTrackHeight;

  private playedRatio(): number {
    return this.durationMs > 0 ? clamp01(this.positionMs / this.durationMs) : 0;
  }

  private bufRatio(): number {
    return this.durationMs > 0 ? clamp01(this.bufferedMs / this.durationMs) : 0;
  }

  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      Row() {
      }
      .width('100%')
      .height(this.trackHeight)
      .backgroundColor(PlayerTheme.progressTrack)
      Row() {
      }
      .width(`${this.bufRatio() * 100}%`)
      .height(this.trackHeight)
      .backgroundColor(PlayerTheme.progressBuffer)
      Row() {
      }
      .width(`${this.playedRatio() * 100}%`)
      .height(this.trackHeight)
      .backgroundColor(PlayerTheme.accent)
    }
    .width('100%')
    .height(this.trackHeight)
  }
}

// ---------------------------------------------------------------------------
// 控制栏里的进度条 + 快进预览
// ---------------------------------------------------------------------------

/**
 * 控制栏里的进度条：**点一下也能跳 + 拖动跳**，拖动时上方弹**快进预览图**。
 *
 * 松手后**不立刻交还给 positionMs**：seek 是异步的，松手那一帧位置还是旧值，
 * 直接交还会让 dot 先弹回去再跳过来。所以松手后把目标位置挂起（pendingSeekMs >= 0），
 * 直到播放位置真的追到目标（±seekSettleToleranceMs）才撤掉 —— 判据是**双向**的 abs()，
 * 且只由位置事件收敛。
 */
@Component
export struct PlayerProgressBar {
  @Prop @Watch('onPositionChanged') positionMs: number = 0;
  @Prop durationMs: number = 0;
  @Prop bufferedMs: number = 0;
  /** 快进预览帧列表（App 层喂进来；空 = 没有预览图，只显示圆形占位） */
  @Prop previewFrames: string[] = [];
  @Prop previewPositionMs: number = 0;
  @Prop previewActive: boolean = false;
  onSeek: (positionMs: number) => void = () => {
  };
  onSeekStart: () => void = () => {
  };
  onSeekEnd: () => void = () => {
  };
  /** Compose：touchHeight = 20.dp / trackHeight = 4.dp / thumbSize = 20.dp */
  touchHeight: number = PlayerSize.progressTouchHeight;
  trackHeight: number = PlayerSize.progressTrackHeight;
  thumbSize: number = PlayerSize.progressThumbSize;

  @State dragging: boolean = false;
  @State dragRatio: number = 0;
  /** 挂起的目标位置；-1 = 没有挂起（Compose 的 pendingSeekMs = null） */
  @State pendingSeekMs: number = -1;
  /** 容器宽度（vp）：dot 偏移与预览卡片位置要用 */
  @State trackWidthVp: number = 0;

  /**
   * 播放位置追到目标（±容差）即视为定位完成，撤掉挂起。
   * 判据是双向 abs()：往回拖时 positionMs 仍是"更靠后"的旧值，单向判据会立刻误判。
   */
  private onPositionChanged(): void {
    if (this.pendingSeekMs >= 0 &&
      Math.abs(this.positionMs - this.pendingSeekMs) <= PlayerSize.seekSettleToleranceMs) {
      this.pendingSeekMs = -1;
    }
  }

  private safeDuration(): number {
    return this.durationMs > 1 ? this.durationMs : 1;
  }

  private pendingRatio(): number {
    return this.pendingSeekMs >= 0 ? clamp01(this.pendingSeekMs / this.safeDuration()) : -1;
  }

  private playedRatio(): number {
    if (this.dragging) {
      return this.dragRatio;
    }
    const pending: number = this.pendingRatio();
    if (pending >= 0) {
      return pending;
    }
    return clamp01(this.positionMs / this.safeDuration());
  }

  private bufRatio(): number {
    return clamp01(this.bufferedMs / this.safeDuration());
  }

  /** 指示点（拖动 / 播放）在轨道上的比例 */
  private indicatorRatio(): number {
    if (this.dragging) {
      return this.dragRatio;
    }
    const pending: number = this.pendingRatio();
    if (pending >= 0) {
      return pending;
    }
    return clamp01(this.previewPositionMs / this.safeDuration());
  }

  private report(x: number): void {
    if (this.trackWidthVp <= 0) {
      return;
    }
    this.dragRatio = clamp01(x / this.trackWidthVp);
  }

  /** 当前正在预览的时间（拖动中用拖动比例算，否则用传入的预览位置） */
  private previewTimeMs(): number {
    return this.dragging ? Math.round(this.dragRatio * this.safeDuration()) : this.previewPositionMs;
  }

  private trackOffsetY(): number {
    return (this.touchHeight - this.trackHeight) / 2;
  }

  private thumbOffsetX(): number {
    const raw: number = this.trackWidthVp * this.playedRatio() - this.thumbSize / 2;
    return raw > 0 ? raw : 0;
  }

  /**
   * 按下即进入拖动并上报位置；移动只更新预览；**松手才 seek**（顺序：先挂起目标再结束拖动）。
   */
  private handleTouch(event: TouchEvent): void {
    if (event.type === TouchType.Down) {
      const touches: TouchObject[] = event.touches;
      this.dragging = true;
      /* 新的一次拖动 = 新的意图：清掉上一次还挂着的目标 */
      this.pendingSeekMs = -1;
      this.onSeekStart();
      if (touches.length > 0) {
        this.report(touches[0].x);
      }
      return;
    }
    if (event.type === TouchType.Move) {
      const touches: TouchObject[] = event.touches;
      if (touches.length > 0) {
        this.report(touches[0].x);
      }
      return;
    }
    if (event.type === TouchType.Up) {
      const target: number = Math.round(this.dragRatio * this.safeDuration());
      this.pendingSeekMs = target;
      this.onSeek(target);
      this.dragging = false;
      this.onSeekEnd();
      return;
    }
    /* Cancel：只收尾，不 seek */
    this.dragging = false;
    this.onSeekEnd();
  }

  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      /* 轨道底色（圆角 = 轨道高的一半） */
      Row() {
      }
      .width('100%')
      .height(this.trackHeight)
      .borderRadius(this.trackHeight / 2)
      .backgroundColor(PlayerTheme.progressTrack)
      .offset({ x: 0, y: this.trackOffsetY() })
      .hitTestBehavior(HitTestMode.None)
      /* 已缓冲 */
      Row() {
      }
      .width(`${this.bufRatio() * 100}%`)
      .height(this.trackHeight)
      .borderRadius(this.trackHeight / 2)
      .backgroundColor(PlayerTheme.progressBuffer)
      .offset({ x: 0, y: this.trackOffsetY() })
      .hitTestBehavior(HitTestMode.None)
      /* 已播 */
      Row() {
      }
      .width(`${this.playedRatio() * 100}%`)
      .height(this.trackHeight)
      .borderRadius(this.trackHeight / 2)
      .backgroundColor(PlayerTheme.accent)
      .offset({ x: 0, y: this.trackOffsetY() })
      .hitTestBehavior(HitTestMode.None)
      /* 进度点（Thumb 动画的静止帧）：外面套一层内置容器，布局属性加在容器上 */
      Column() {
        DanmakuDotThumb({ dotSize: this.thumbSize })
      }
      .offset({ x: this.thumbOffsetX(), y: this.trackOffsetY() + this.trackHeight / 2 - this.thumbSize / 2 })
      .hitTestBehavior(HitTestMode.None)
      /* 快进预览：拖动中 / 预览中显示，松手即收起 */
      if (this.dragging || this.previewActive) {
        Column() {
          SeekPreviewPopup({
            positionMs: this.previewTimeMs(),
            durationMs: this.durationMs,
            previewFrames: this.previewFrames,
            indicatorX: this.trackWidthVp * this.indicatorRatio(),
            trackWidth: this.trackWidthVp,
            trackTop: this.trackOffsetY()
          })
        }
        .hitTestBehavior(HitTestMode.None)
      }
    }
    .width('100%')
    .height(this.touchHeight)
    .onAreaChange((oldValue: Area, newValue: Area) => {
      this.trackWidthVp = Number(newValue.width);
    })
    .onTouch((event: TouchEvent) => {
      this.handleTouch(event);
    })
  }
}

/**
 * 快进预览（**唯一依据 = 两张手机截图**）。
 *
 * 截图 3（该档的图已就绪）：圆角矩形卡片 + 浅色描边，**水平居中于播放器**；
 *   宽 = 播放器宽度的 23%、高 = 16:9；卡片**底边离轨道上沿 90dp**；
 *   卡片**正下方居中**一行 `当前 / 总时长`（大号白字 + 半透明黑底）。
 * 截图 4（图还没就绪 / 该档没有帧）：**小圆形占位 + 中间加载指示**，
 *   锚在**当前拖动/播放的指示点**上（拇指右侧一点、贴着进度条），不居中。
 *
 * 帧下标 = `floor(毫秒 / 5000) + 1` —— 这是**取帧语义**（每 5 秒一档、arr[0] 是空占位）。
 */
@Component
export struct SeekPreviewPopup {
  @Prop positionMs: number = 0;
  @Prop durationMs: number = 0;
  @Prop previewFrames: string[] = [];
  /** 拖动/播放指示点在轨道上的 x（截图 4 的小圆锚点） */
  @Prop indicatorX: number = 0;
  /** 轨道（播放器）宽度 */
  @Prop trackWidth: number = 0;
  /** 轨道上沿相对本层的 y */
  @Prop trackTop: number = 0;

  /** 当前这一档的图是否已就绪（Compose 用 Coil 的 Success；这里用 Image.onComplete） */
  @State frameReady: boolean = false;
  @State readyUrl: string = '';

  private frameUrl(): string {
    const index: number = Math.floor(this.positionMs / PREVIEW_FRAME_INTERVAL_MS) + 1;
    if (index < 0 || index >= this.previewFrames.length) {
      return '';
    }
    const url: string = this.previewFrames[index];
    return url !== undefined && url.length > 0 ? url : '';
  }

  private cardWidth(): number {
    return this.trackWidth * PlayerSize.previewCardWidthRatio;
  }

  private cardHeight(): number {
    return this.cardWidth() * 9 / 16;
  }

  private cardX(): number {
    return (this.trackWidth - this.cardWidth()) / 2;
  }

  private cardY(): number {
    return this.trackTop - PlayerSize.previewCardGapAboveTrack - this.cardHeight();
  }

  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      /*
       * 卡片：图未就绪时整体透明，但**仍然渲染** —— 不然 Image 永远不会 onComplete。
       * 就绪判定按 URL 键控：换了帧（URL 变了）要重新等待。
       */
      if (this.frameUrl().length > 0) {
        Column() {
          Stack({ alignContent: Alignment.Center }) {
            Row() {
            }
            .width('100%')
            .height('100%')
            .borderRadius(PlayerSize.previewCardRadius)
            .backgroundColor('#FF000000')
            .border({
              width: PlayerSize.previewCardBorderWidth,
              color: PlayerSize.previewCardBorder,
              radius: PlayerSize.previewCardRadius
            })
            Image(this.frameUrl())
              .width('100%')
              .height('100%')
              .objectFit(ImageFit.Fill)
              .borderRadius(PlayerSize.previewCardRadius)
              .onComplete(() => {
                this.readyUrl = this.frameUrl();
                this.frameReady = true;
              })
              .onError(() => {
                this.frameReady = false;
              })
          }
          .width('100%')
          .height(this.cardHeight())

          Row() {
          }
          .height(PlayerSize.previewCardTimeGap)

          Text(`${formatPlayerTime(this.positionMs)} / ${formatPlayerTime(this.durationMs)}`)
            .fontSize(PlayerSize.previewTimeFontSize)
            .fontColor('#FFFFFFFF')
            .maxLines(1)
            .borderRadius(PlayerSize.previewTimeRadius)
            .backgroundColor(PlayerTheme.previewTimeBg)
            .padding({
              left: PlayerSize.previewTimePaddingH,
              right: PlayerSize.previewTimePaddingH,
              top: PlayerSize.previewTimePaddingV,
              bottom: PlayerSize.previewTimePaddingV
            })
        }
        .width(this.cardWidth())
        .alignItems(HorizontalAlign.Center)
        .offset({ x: this.cardX(), y: this.cardY() })
        .opacity(this.frameReady && this.readyUrl === this.frameUrl() ? 1 : 0)
      }

      /* 圆形加载占位（截图 4）：锚在指示点上，拇指右侧一点、底边贴着轨道上沿 */
      if (this.frameUrl().length === 0 || !this.frameReady || this.readyUrl !== this.frameUrl()) {
        PreviewLoadingDot({ indicatorX: this.indicatorX, trackTop: this.trackTop })
      }
    }
    .width('100%')
    .height(0)
  }
}

/** 截图 4：小圆形占位 + 中间加载指示。 */
@Component
export struct PreviewLoadingDot {
  @Prop indicatorX: number = 0;
  @Prop trackTop: number = 0;

  build() {
    Stack({ alignContent: Alignment.Center }) {
      LoadingProgress()
        .width(PlayerSize.previewSpinnerSize)
        .height(PlayerSize.previewSpinnerSize)
        .color('#FFFFFFFF')
    }
    .width(PlayerSize.previewDotSize)
    .height(PlayerSize.previewDotSize)
    .borderRadius(PlayerSize.previewDotSize / 2)
    .backgroundColor(PlayerTheme.previewPlaceholderBg)
    .offset({
      x: this.indicatorX + PlayerSize.previewDotOffsetX,
      y: this.trackTop - PlayerSize.previewDotGap - PlayerSize.previewDotSize
    })
  }
}

// ---------------------------------------------------------------------------
// 底部栏
// ---------------------------------------------------------------------------

/**
 * 底部栏。两种模式内容不同（照两张截图）：
 *   · 非全屏：进度条一行 + `[播放键][时间] …… [清晰度][倍速][字幕][竖屏胶囊][竖屏][全屏]`
 *   · 全屏：`[播放键][弹幕开关][弹幕设置][弹幕输入框] …… [清晰度][倍速][字幕]`
 * 整块吃掉空白处的点击，避免穿到画面区把视频点暂停。
 */
@Component
export struct PlayerBottomBar {
  @Prop isPlaying: boolean = false;
  @Prop positionMs: number = 0;
  @Prop durationMs: number = 0;
  @Prop bufferedMs: number = 0;
  @Prop isFullscreen: boolean = false;
  @Prop danmakuEnabled: boolean = true;
  @Prop qualityLabel: string = '自动';
  @Prop speedLabel: string = '倍速';
  @Prop previewFrames: string[] = [];
  @Prop previewPositionMs: number = 0;
  @Prop previewActive: boolean = false;

  onPlayPause: () => void = () => {
  };
  onSeek: (positionMs: number) => void = () => {
  };
  onSeekStart: () => void = () => {
  };
  onSeekEnd: () => void = () => {
  };
  onTogglePortraitFullscreen: () => void = () => {
  };
  onToggleFullscreen: () => void = () => {
  };
  onToggleDanmaku: () => void = () => {
  };
  onDanmakuSettings: () => void = () => {
  };
  onDanmakuSend: (text: string) => void = () => {
  };
  onSubtitleClick: () => void = () => {
  };
  onSpeedClick: () => void = () => {
  };
  onQualityClick: () => void = () => {
  };
  onInteraction: () => void = () => {
  };
  onDanmakuInputFocusChanged: (focused: boolean) => void = () => {
  };

  build() {
    Column() {
      /* 进度条（全屏在底部栏里也有一条；常驻那条在播放器层，见 PlayerThinProgressBar） */
      Column() {
        PlayerProgressBar({
          positionMs: this.positionMs,
          durationMs: this.durationMs,
          bufferedMs: this.bufferedMs,
          previewFrames: this.previewFrames,
          previewPositionMs: this.previewPositionMs,
          previewActive: this.previewActive,
          onSeek: (ms: number) => {
            this.onInteraction();
            this.onSeek(ms);
          },
          onSeekStart: () => {
            this.onInteraction();
            this.onSeekStart();
          },
          onSeekEnd: this.onSeekEnd
        })
      }
      .width('100%')
      .margin({ bottom: PlayerSize.bottomBarProgressGap })

      Row() {
        PlayPauseIconButton({
          isPlaying: this.isPlaying,
          iconSize: PlayerSize.playPauseSize,
          onTap: () => {
            this.onInteraction();
            this.onPlayPause();
          }
        })

        if (this.isFullscreen) {
          /* 弹幕开关 + 弹幕设置（两颗图标从 Qt 工程拷贝复用） */
          PlayerIconButton({
            iconRes: this.danmakuEnabled
              ? $rawfile('icons/ic_danmaku_switch_on.svg')
              : $rawfile('icons/ic_danmaku_switch_off.svg'),
            description: this.danmakuEnabled ? '关闭弹幕' : '开启弹幕',
            onTap: () => {
              this.onInteraction();
              this.onToggleDanmaku();
            }
          })
          PlayerIconButton({
            iconRes: $rawfile('icons/ic_danmaku_setting.svg'),
            description: '弹幕设置',
            onTap: () => {
              this.onInteraction();
              this.onDanmakuSettings();
            }
          })

          /* 弹幕输入框（截图 2 那条胶囊）：外面套一层内置容器承载布局属性 */
          Row() {
            DanmakuInputBar({
              onSend: (text: string) => {
                this.onDanmakuSend(text);
                this.onInteraction();
              },
              onInteraction: this.onInteraction,
              onFocusChanged: this.onDanmakuInputFocusChanged
            })
          }
          .layoutWeight(1)
          .margin({ left: PlayerSize.bottomBarInputPaddingH, right: PlayerSize.bottomBarInputPaddingH })

          /* 顺序与位置对齐 Qt 的 RightControls：清晰度 → 倍速 → 字幕，同级横排 */
          BarText({
            text: this.qualityLabel.length > 0 ? this.qualityLabel : '自动',
            onTap: () => {
              this.onInteraction();
              this.onQualityClick();
            }
          })
          BarText({
            text: this.speedLabel.length > 0 ? this.speedLabel : '倍速',
            onTap: () => {
              this.onInteraction();
              this.onSpeedClick();
            }
          })
          BarText({
            text: '字幕',
            onTap: () => {
              this.onInteraction();
              this.onSubtitleClick();
            }
          })
        } else {
          Row()
            .width(PlayerSize.bottomBarTimeGap)
          Text(`${formatPlayerTime(this.positionMs)} / ${formatPlayerTime(this.durationMs)}`)
            .fontSize(PlayerSize.bottomBarTimeFontSize)
            .fontColor(PlayerTheme.controlText)
            .maxLines(1)
            .padding({ left: PlayerSize.bottomBarTimePaddingH, right: PlayerSize.bottomBarTimePaddingH })

          Blank()

          BarText({
            text: this.qualityLabel.length > 0 ? this.qualityLabel : '自动',
            onTap: () => {
              this.onInteraction();
              this.onQualityClick();
            }
          })
          BarText({
            text: this.speedLabel.length > 0 ? this.speedLabel : '倍速',
            onTap: () => {
              this.onInteraction();
              this.onSpeedClick();
            }
          })
          BarText({
            text: '字幕',
            onTap: () => {
              this.onInteraction();
              this.onSubtitleClick();
            }
          })

          /* 竖屏胶囊 + 竖屏 SVG + 全屏 SVG */
          Text('竖屏')
            .fontSize(PlayerSize.portraitPillFontSize)
            .fontColor(PlayerTheme.controlTextActive)
            .maxLines(1)
            .margin({ left: PlayerSize.portraitPillMarginH, right: PlayerSize.portraitPillMarginH })
            .borderRadius(PlayerSize.portraitPillRadius)
            .backgroundColor(PlayerTheme.thumbBadgeBg)
            .padding({
              left: PlayerSize.portraitPillPaddingH,
              right: PlayerSize.portraitPillPaddingH,
              top: PlayerSize.portraitPillPaddingV,
              bottom: PlayerSize.portraitPillPaddingV
            })
            .onClick(() => {
              this.onInteraction();
              this.onTogglePortraitFullscreen();
            })
          PlayerIconButton({
            iconRes: $rawfile('icons/ic_player_portrait_fullscreen.svg'),
            description: '竖屏全屏',
            onTap: () => {
              this.onInteraction();
              this.onTogglePortraitFullscreen();
            }
          })
          PlayerIconButton({
            iconRes: $rawfile('icons/ic_player_fullscreen.svg'),
            description: '全屏',
            onTap: () => {
              this.onInteraction();
              this.onToggleFullscreen();
            }
          })
        }
      }
      .width('100%')
      .height(PlayerSize.bottomBarRowHeight)
      .alignItems(VerticalAlign.Center)
    }
    .width('100%')
    .padding({
      left: PlayerSize.bottomBarPaddingH,
      right: PlayerSize.bottomBarPaddingH,
      top: PlayerSize.bottomBarPaddingV,
      bottom: PlayerSize.bottomBarPaddingV
    })
    .onClick(() => {
      this.onInteraction();
    })
  }
}

/** 底部栏的文字入口（字幕 / 倍速 / 自动这种小字按钮）。 */
@Component
export struct BarText {
  @Prop text: string = '';
  onTap: () => void = () => {
  };

  build() {
    Text(this.text)
      .fontSize(PlayerSize.barTextFontSize)
      .fontColor(PlayerTheme.controlTextActive)
      .maxLines(1)
      .padding({ left: PlayerSize.barTextPaddingH, right: PlayerSize.barTextPaddingH })
      .onClick(() => {
        this.onTap();
      })
  }
}

/**
 * 弹幕输入框（截图 2 的胶囊：占位文案 + 发送）。
 *   · 宽度：外层用 layoutWeight(1) 吃掉剩余空间；
 *   · 光标：白色；
 *   · 聚焦状态上报给控制栏：输入时不自动隐藏。
 */
@Component
export struct DanmakuInputBar {
  onSend: (text: string) => void = () => {
  };
  onInteraction: () => void = () => {
  };
  onFocusChanged: (focused: boolean) => void = () => {
  };

  @State text: string = '';

  build() {
    Row() {
      TextInput({ text: this.text, placeholder: '发个友善的弹幕见证当下' })
        .placeholderColor(PlayerTheme.controlText)
        .fontColor(PlayerTheme.controlTextActive)
        .fontSize(PlayerSize.danmakuInputFontSize)
        .caretColor(PlayerTheme.controlTextActive)
        .backgroundColor(Color.Transparent)
        .padding(0)
        .layoutWeight(1)
        .onChange((value: string) => {
          this.text = value;
          this.onInteraction();
        })
        .onFocus(() => {
          this.onFocusChanged(true);
        })
        .onBlur(() => {
          this.onFocusChanged(false);
        })

      Text('发送')
        .fontSize(PlayerSize.danmakuInputFontSize)
        .fontColor(PlayerTheme.controlText)
        .onClick(() => {
          const value: string = this.text.trim();
          if (value.length > 0) {
            this.onSend(value);
            this.text = '';
          }
        })
    }
    .height(PlayerSize.danmakuInputHeight)
    .borderRadius(PlayerSize.danmakuInputRadius)
    .backgroundColor(PlayerTheme.previewPlaceholderBg)
    .padding({ left: PlayerSize.danmakuInputPaddingH, right: PlayerSize.danmakuInputPaddingH })
    .alignItems(VerticalAlign.Center)
  }
}

// ---------------------------------------------------------------------------
// 顶部栏（返回 + 标题 + 在线人数 + 关注）
// ---------------------------------------------------------------------------

@Component
export struct PlayerTopBar {
  @Prop title: string = '';
  @Prop watchingText: string = '';
  onBack: () => void = () => {
  };
  onTitleClick: () => void = () => {
  };
  /** 不传就不渲染「+ 关注」那一颗（对应 Compose 的 onFollowClick == null） */
  @Prop showFollow: boolean = false;
  onFollowClick: () => void = () => {
  };

  build() {
    Row() {
      IconButtonBox({ boxSize: PlayerSize.iconButtonSize, onTap: this.onBack }) {
        PlayerBackIcon({ iconSize: PlayerSize.topBarIconSize, label: '返回' })
      }

      Column() {
        if (this.title.length > 0) {
          Text(this.title)
            .fontSize(PlayerSize.topBarTitleFontSize)
            .fontWeight(FontWeight.Medium)
            .fontColor(PlayerTheme.controlTextActive)
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
        }
        if (this.watchingText.length > 0) {
          Text(this.watchingText)
            .fontSize(PlayerSize.topBarWatchingFontSize)
            .fontColor(PlayerTheme.controlText)
            .maxLines(1)
        }
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)
      .padding({ left: PlayerSize.topBarTitleGap })
      .onClick(() => {
        this.onTitleClick();
      })

      if (this.showFollow) {
        Text('+ 关注')
          .fontSize(PlayerSize.followFontSize)
          .fontColor(PlayerTheme.controlTextActive)
          .borderRadius(PlayerSize.followRadius)
          .backgroundColor(PlayerTheme.previewPlaceholderBg)
          .padding({
            left: PlayerSize.followPaddingH,
            right: PlayerSize.followPaddingH,
            top: PlayerSize.followPaddingV,
            bottom: PlayerSize.followPaddingV
          })
          .onClick(() => {
            this.onFollowClick();
          })
      }
    }
    .width('100%')
    .padding({
      left: PlayerSize.topBarPaddingH,
      right: PlayerSize.topBarPaddingH,
      top: PlayerSize.topBarPaddingV,
      bottom: PlayerSize.topBarPaddingV
    })
    .alignItems(VerticalAlign.Center)
  }
}
