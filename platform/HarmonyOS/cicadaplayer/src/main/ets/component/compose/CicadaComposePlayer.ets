/*
 * 播放器总装 —— ArkTS 版，来源是 Compose 安卓版的
 *   platform/Android/ComposePlayer/app/src/main/java/com/cicada/player/compose/components/video/CicadaVideoPlayer.kt
 *
 * 层级顺序（自下而上，与 Compose 的 Box 书写顺序一一对应）：
 *   ① 画面（XComponent SURFACE）
 *   ② 手势层（单击显隐控制栏、双击播放/暂停、竖向亮度/音量、横向 seek、长按临时倍速）
 *   ③ 弹幕层
 *   ④ 加载/缓冲指示（loading 或 seeking 时都在；画面保持上一帧）
 *   ⑤ 错误卡片
 *   ⑥ 重播卡片（Compose 没有，见下面「与 Compose 的差异」）
 *   ⑦ 控制层（全屏：顶部栏 + 底部栏；非全屏：只有底部栏）—— 单击显隐、播放中 3 秒自动隐藏
 *   ⑧ 清晰度切换提示（画面左下角、距底 76dp）
 *   ⑨ 非全屏时的左上角返回键
 *   ⑩ 常驻细进度条（**控制栏隐藏后才显示**，贴最底部）
 *
 * 界面尺寸/配色全部走 theme/PlayerTheme.ets，行为与 Compose 一致的地方在各自注释里写了。
 *
 * 与 Compose 的差异（逐条，都是"内核 C API 给不出这个读数/这条通道"或"红线"造成的）：
 *   · `controller.autoQuality` / `currentVideoIndex` / `subtitlesEnabled` / `currentSubtitleIndex`
 *     在鸿蒙侧现读内核：`getCurrentStreamIndex(type)` < 0 视为"自动/未选"，否则就是当前档的
 *     streamIndex（Compose 是 controller 上的显式状态）；
 *   · `previewActive`（Compose 的 `controller.previewing`，靠 beginPreview/endPreview）在 C API 里
 *     没有对应通道，所以快进预览只在**拖动中**显示；
 *   · 截图：只做"请求内核截帧 → 内核回调到了就交给页面存相册"，`captureInFlight` 仅由内核回调收敛；
 *   · 「音量」面板与 Compose 一样**没有从控制栏打开的入口**（Compose 只在调试入口 `--es panel volume`
 *     才打开它）；面板本身照做并导出，需要的话由消费方接一条入口即可；
 *   · 重播卡片：Compose 在 onCompletion 后不出 UI。鸿蒙 Demo 的播放页本来就有"播放结束 → 重播"
 *     这条流程（replayVisible / rePlay()），所以这里补一张**同配色同圆角**的卡片承载它，
 *     不影响与 Compose 一致的其余部分；
 *   · 面板进出、控制层显隐的动画：Compose 是 AnimatedVisibility(fadeIn/fadeOut)，这里用
 *     `TransitionEffect.OPACITY.animation({duration: 200})` + `animateTo`，时长取 200ms（Compose 的
 *     fadeIn 默认是 spring，无法逐帧等同，这里取一个相近的线性淡入淡出）。
 */

import { CicadaPlayer } from '../../api/CicadaPlayer';
import { MirrorMode, StreamInfo, StreamType, VideoQualitySwitchStatus } from '../../api/CicadaTypes';
import { PlayerSize, PlayerTheme } from '../theme/PlayerTheme';
import { Danmaku, DanmakuConfig, DanmakuType, DanmakuView } from './PlayerDanmakuLayer';
import { PlayerBottomBar, PlayerThinProgressBar, PlayerTopBar } from './PlayerControls';
import { PlayerGestureLayer } from './PlayerGestureLayer';
import { IconButtonBox, PlayerBackIcon } from './PlayerIcons';
import {
  ColorVisionKind,
  MirrorUiMode,
  PlayerDanmakuPanel,
  PlayerInfoPanel,
  PlayerQualityPanel,
  PlayerSpeedPanel,
  PlayerSubtitlePanel,
  PlayerVolumePanel,
  colorVisionMatrix
} from './PlayerPanels';
import {
  QualityOption,
  TRACK_INDEX_AUTO,
  buildQualities,
  qualityLabelFor
} from './PlayerStats';

/** 同时只开一个浮层面板（与 Compose 的 PlayerPanel 一致；**没有 Color**）。 */
export enum PlayerPanelKind {
  None = 0,
  Quality = 1,
  Speed = 2,
  Subtitle = 3,
  Danmaku = 4,
  Info = 5,
  Volume = 6
}

/** 没有终态提示（Compose 的 QUALITY_NOTICE_NONE）。 */
const QUALITY_NOTICE_NONE: number = -1;

/** 两段时间轴（控制层淡入淡出 / 面板淡入淡出）。 */
const FADE_DURATION_MS: number = 200;

/** UI 三态 → 内核枚举（镜像只翻转，不做旋转）。 */
function toKernelMirror(mode: MirrorUiMode): MirrorMode {
  if (mode === MirrorUiMode.Horizontal) {
    return MirrorMode.MIRROR_MODE_HORIZONTAL;
  }
  if (mode === MirrorUiMode.Vertical) {
    return MirrorMode.MIRROR_MODE_VERTICAL;
  }
  return MirrorMode.MIRROR_MODE_NONE;
}

@Component
export struct CicadaComposePlayer {
  /* ---------------- 由页面注入的引擎与状态 ---------------- */
  /** 播放器句柄（类实例用普通成员传）：本组件直接调内核接口 */
  player: CicadaPlayer = new CicadaPlayer();

  @Prop title: string = '';
  @Prop watchingText: string = '';
  @Prop isFullscreen: boolean = false;
  @Prop @Watch('onPlayStateChanged') isPlaying: boolean = false;
  @Prop positionMs: number = 0;
  @Prop durationMs: number = 0;
  @Prop bufferedMs: number = 0;
  @Prop @Watch('onPlayStateChanged') loading: boolean = false;
  @Prop errorMessage: string = '';
  @Prop replayVisible: boolean = false;
  @Prop downloadSpeedBps: number = 0;
  @Prop decoderNotice: string = '';
  /** 起播前下发的硬解开关（只影响下一次起播，不在播放中偷换解码器） */
  @Prop hardwareDecodeEnabled: boolean = true;
  @Prop backgroundPlayEnabled: boolean = true;
  @Prop captureInFlight: boolean = false;
  /** 清晰度切换状态（内核 onVideoQualitySwitch 的 status / streamIndex；-1 = 无） */
  @Prop @Watch('onQualitySwitchChanged') qualitySwitchStatus: number = QUALITY_NOTICE_NONE;
  @Prop qualitySwitchIndex: number = TRACK_INDEX_AUTO;
  /** 快进预览帧（App 层喂进来；空 = 只显示圆形占位） */
  @Prop previewFrames: string[] = [];
  /**
   * 媒体信息的**轨道列表**（`MediaInfo.streams`）。
   * 用 @Prop 传纯数据接口数组：ArkUI 里无装饰器的成员只在子组件创建时赋值，
   * 父组件后续更新不会同步；媒体信息是**播放中随时会来**的，必须跟着刷新。
   */
  @Prop streams: StreamInfo[] = [];

  /* ---------------- 回调 ---------------- */
  /** 画面就绪/失效（页面据此在就绪后起播） */
  onSurfaceReady: (ready: boolean) => void = () => {
  };
  onBack: () => void = () => {
  };
  onToggleFullscreen: () => void = () => {
  };
  onTogglePortraitFullscreen: () => void = () => {
  };
  onRequestReplay: () => void = () => {
  };
  onRequestErrorExit: () => void = () => {
  };
  onRequestSnapshot: () => void = () => {
  };
  onToggleBackgroundPlay: () => void = () => {
  };
  /** 亮度手势的目标值（0..100）：鸿蒙没有"读回窗口亮度"，所以由页面持有并施加 */
  onBrightnessChange: (percent: number) => void = () => {
  };
  onDanmakuSend: (text: string) => void = () => {
  };

  /* ---------------- 组件内部状态（与 Compose 的 remember 一一对应） ---------------- */
  private xcomponentController: XComponentController = new XComponentController();
  private surfaceReady: boolean = false;

  @State showControls: boolean = true;
  @State openPanel: PlayerPanelKind = PlayerPanelKind.None;
  @State danmakuInputFocused: boolean = false;

  /* 弹幕（总开关先活在本次会话内；条目由输入框追加） */
  @State danmakuEnabled: boolean = true;
  @State danmakuOpacity: number = 100;
  @State danmakuFontScale: number = 100;
  @State danmakuDensity: number = 8;
  @State danmakus: Danmaku[] = [];
  @State danmakuRevision: number = 0;
  private danmakuConfig: DanmakuConfig = new DanmakuConfig();

  /* 音量 / 倍速 / 镜像 / 色觉 / 亮度 */
  @State volume: number = 0.9;
  @State muted: boolean = false;
  @State speed: number = 1;
  @State mirrorMode: MirrorUiMode = MirrorUiMode.None;
  @State colorVision: ColorVisionKind = ColorVisionKind.None;
  @State brightnessPercent: number = 50;

  /* 清晰度终态提示 */
  @State noticeStatus: number = QUALITY_NOTICE_NONE;
  @State noticeIndex: number = TRACK_INDEX_AUTO;
  private noticeTimer: number = -1;

  /* 控制层自动隐藏 */
  private hideTimer: number = -1;
  /** 长按前的倍速，松手恢复用 */
  private speedBeforeBoost: number = 1;
  /** 是否正处在"长按临时倍速"里（避免未长按也被恢复） */
  private speedBoostActive: boolean = false;

  aboutToAppear(): void {
    /* 与 Compose 相同：起播前把解码方式下发给内核（真正读它的是建视频路的时候） */
    this.player.enableHardwareDecoder(this.hardwareDecodeEnabled);
    this.restartAutoHide();
  }

  aboutToDisappear(): void {
    this.clearHideTimer();
    this.clearNoticeTimer();
    if (this.surfaceReady) {
      this.surfaceReady = false;
      this.player.clearScreen();
    }
  }

  // -------------------------------------------------------------------------
  // 内核读数（Compose 里读 controller 的那几处）
  // -------------------------------------------------------------------------

  private autoQuality(): boolean {
    return this.player.getCurrentStreamIndex(StreamType.ST_TYPE_VIDEO) < 0;
  }

  private currentVideoIndex(): number {
    return this.player.getCurrentStreamIndex(StreamType.ST_TYPE_VIDEO);
  }

  private currentSubtitleIndex(): number {
    return this.player.getCurrentStreamIndex(StreamType.ST_TYPE_SUB);
  }

  private subtitlesEnabled(): boolean {
    return this.currentSubtitleIndex() >= 0;
  }

  private qualityOptions(): QualityOption[] {
    return buildQualities(this.streams, this.currentVideoIndex(), this.autoQuality());
  }

  /** 控制栏那颗按钮上的清晰度文案 = 当前真实在播的那一档（自动档下是 ABR 实际选中的档）。 */
  private currentQualityLabel(): string {
    const label: string = qualityLabelFor(this.streams, this.currentVideoIndex());
    if (label.length > 0) {
      return label;
    }
    return this.autoQuality() ? '自动' : '清晰度';
  }

  private switchingQuality(): boolean {
    return this.qualitySwitchStatus === VideoQualitySwitchStatus.STARTED ||
      this.player.isStreamSwitchInFlight();
  }

  // -------------------------------------------------------------------------
  // 控制层显隐与自动隐藏
  // -------------------------------------------------------------------------

  private clearHideTimer(): void {
    if (this.hideTimer >= 0) {
      clearTimeout(this.hideTimer);
      this.hideTimer = -1;
    }
  }

  /**
   * 控制栏自动隐藏（Compose 的 3 秒）：
   * 正在播放、没有面板、弹幕输入框没聚焦、没在缓冲、没在切档时，3 秒后收起；
   * 任一条件变化或用户交互都会重新计时。
   */
  private restartAutoHide(): void {
    this.clearHideTimer();
    if (!this.showControls || !this.isPlaying || this.openPanel !== PlayerPanelKind.None ||
      this.danmakuInputFocused || this.loading || this.switchingQuality()) {
      return;
    }
    this.hideTimer = setTimeout(() => {
      this.hideTimer = -1;
      this.showControls = false;
    }, PlayerSize.controlsAutoHideMs);
  }

  private setControlsVisible(visible: boolean): void {
    this.getUIContext().animateTo({ duration: FADE_DURATION_MS, curve: Curve.EaseInOut }, () => {
      this.showControls = visible;
    });
    this.restartAutoHide();
  }

  /** 单击画面：有面板先关面板，否则显隐控制栏（Compose 的 onTap）。 */
  private onSurfaceTap(): void {
    if (this.openPanel !== PlayerPanelKind.None) {
      this.setPanel(PlayerPanelKind.None);
      return;
    }
    this.setControlsVisible(!this.showControls);
  }

  private setPanel(panel: PlayerPanelKind): void {
    this.getUIContext().animateTo({ duration: FADE_DURATION_MS, curve: Curve.EaseInOut }, () => {
      this.openPanel = panel;
    });
    this.restartAutoHide();
  }

  private notifyInteraction(): void {
    this.restartAutoHide();
  }

  /** 播放状态 / 缓冲状态一变就重新计时（Compose 那条 LaunchedEffect 的 key 里的两项）。 */
  private onPlayStateChanged(): void {
    this.restartAutoHide();
  }

  // -------------------------------------------------------------------------
  // 清晰度切换提示
  // -------------------------------------------------------------------------

  private clearNoticeTimer(): void {
    if (this.noticeTimer >= 0) {
      clearTimeout(this.noticeTimer);
      this.noticeTimer = -1;
    }
  }

  /** 内核状态机的三条终态（READY / FAILED / CANCELED）各自收一条提示，2.6 秒后自动收起。 */
  private onQualitySwitchChanged(): void {
    const status: number = this.qualitySwitchStatus;
    if (status === QUALITY_NOTICE_NONE || status === VideoQualitySwitchStatus.STARTED) {
      this.clearNoticeTimer();
      this.noticeStatus = QUALITY_NOTICE_NONE;
      this.noticeIndex = TRACK_INDEX_AUTO;
      this.restartAutoHide();
      return;
    }
    this.noticeStatus = status;
    this.noticeIndex = this.qualitySwitchIndex;
    this.clearNoticeTimer();
    this.noticeTimer = setTimeout(() => {
      this.noticeTimer = -1;
      this.noticeStatus = QUALITY_NOTICE_NONE;
      this.noticeIndex = TRACK_INDEX_AUTO;
    }, PlayerSize.noticeDismissMs);
    this.restartAutoHide();
  }

  private noticeVisibleText(): string {
    if (this.switchingQuality()) {
      const label: string = qualityLabelFor(this.streams, this.qualitySwitchIndex);
      return label.length > 0 ? `正在切换到 ${label}…` : '正在切换清晰度…';
    }
    if (this.noticeStatus === QUALITY_NOTICE_NONE) {
      return '';
    }
    const label: string = qualityLabelFor(this.streams, this.noticeIndex);
    if (this.noticeStatus === VideoQualitySwitchStatus.READY) {
      return label.length > 0 ? `已切换到 ${label}` : '清晰度切换完成';
    }
    if (this.noticeStatus === VideoQualitySwitchStatus.FAILED) {
      return '切换失败，保持当前清晰度';
    }
    return '已取消切换';
  }

  // -------------------------------------------------------------------------
  // 弹幕
  // -------------------------------------------------------------------------

  /** 弹幕设置 → DanmakuConfig（渲染层直接读，改完立刻生效）。 */
  private pushDanmakuConfig(): void {
    /* **原地改**这个对象：弹幕层持的是同一个引用，换新对象它看不到 */
    this.danmakuConfig.isEnabled = this.danmakuEnabled;
    this.danmakuConfig.opacity = this.danmakuOpacity / 100;
    this.danmakuConfig.textSize = PlayerSize.danmakuBaseTextSize * this.danmakuFontScale / 100;
    this.danmakuConfig.maxLines = this.danmakuDensity;
    this.danmakuRevision++;
  }

  private appendDanmaku(text: string): void {
    const item: Danmaku = new Danmaku(text, this.positionMs, DanmakuType.SCROLL, '#FFFFFFFF');
    const next: Danmaku[] = this.danmakus.slice();
    next.push(item);
    this.danmakus = next;
    this.danmakuRevision++;
    this.onDanmakuSend(text);
  }

  // -------------------------------------------------------------------------
  // 手势 / 面板 → 内核
  // -------------------------------------------------------------------------

  private togglePlay(): void {
    if (this.isPlaying) {
      this.player.pause();
    } else {
      this.player.start();
    }
    this.notifyInteraction();
  }

  private applyBrightness(percent: number): void {
    this.brightnessPercent = percent;
    this.onBrightnessChange(percent);
    this.notifyInteraction();
  }

  private applyVolume(percent: number): void {
    const value: number = percent / 100;
    this.volume = value;
    this.muted = false;
    this.player.setMute(false);
    this.player.setVolume(value);
    this.notifyInteraction();
  }

  private beginSpeedBoost(): void {
    /* 只有真的在长按（LongPressGesture.onAction 触发过）才提速；重复触发不做第二次 */
    if (this.speedBoostActive) {
      return;
    }
    this.speedBoostActive = true;
    this.speedBeforeBoost = this.speed;
    this.player.setSpeed(2);
    this.notifyInteraction();
  }

  private endSpeedBoost(): void {
    /* 没在提速就什么都不做（长按被取消时也会走到这里） */
    if (!this.speedBoostActive) {
      return;
    }
    this.speedBoostActive = false;
    this.player.setSpeed(this.speedBeforeBoost);
    this.notifyInteraction();
  }

  private onSurfaceLoad(): void {
    const surfaceId: string = this.xcomponentController.getXComponentSurfaceId();
    this.surfaceReady = this.player.setSurface(surfaceId);
    this.onSurfaceReady(this.surfaceReady);
  }

  private onSurfaceDestroy(): void {
    this.surfaceReady = false;
    this.player.clearScreen();
    this.onSurfaceReady(false);
  }

  // -------------------------------------------------------------------------
  // 各个浮层面板
  // -------------------------------------------------------------------------

  @Builder
  panelContent() {
    if (this.openPanel === PlayerPanelKind.Quality) {
      PlayerQualityPanel({
        streams: this.streams,
        currentIndex: this.currentVideoIndex(),
        autoQuality: this.autoQuality(),
        onPick: (index: number) => {
          this.player.selectTrack(index);
          this.setPanel(PlayerPanelKind.None);
        }
      })
    } else if (this.openPanel === PlayerPanelKind.Speed) {
      PlayerSpeedPanel({
        current: this.speed,
        onPick: (value: number) => {
          this.speed = value;
          this.player.setSpeed(value);
          this.setPanel(PlayerPanelKind.None);
        }
      })
    } else if (this.openPanel === PlayerPanelKind.Subtitle) {
      PlayerSubtitlePanel({
        streams: this.streams,
        currentIndex: this.currentSubtitleIndex(),
        subtitlesEnabled: this.subtitlesEnabled(),
        onPick: (index: number) => {
          /* -1 = 关闭字幕（内核的 AUTO_SELECT_INDEX） */
          this.player.selectTrack(index);
          this.setPanel(PlayerPanelKind.None);
        }
      })
    } else if (this.openPanel === PlayerPanelKind.Danmaku) {
      PlayerDanmakuPanel({
        danmakuEnabled: this.danmakuEnabled,
        panelOpacity: this.danmakuOpacity,
        fontScale: this.danmakuFontScale,
        density: this.danmakuDensity,
        showScroll: this.danmakuConfig.showScroll,
        showTop: this.danmakuConfig.showTop,
        showBottom: this.danmakuConfig.showBottom,
        onToggleEnabled: () => {
          this.danmakuEnabled = !this.danmakuEnabled;
          this.pushDanmakuConfig();
        },
        onOpacity: (value: number) => {
          this.danmakuOpacity = value;
          this.pushDanmakuConfig();
        },
        onFontScale: (value: number) => {
          this.danmakuFontScale = value;
          this.pushDanmakuConfig();
        },
        onDensity: (value: number) => {
          this.danmakuDensity = value;
          this.pushDanmakuConfig();
        },
        onToggleScroll: () => {
          this.danmakuConfig.showScroll = !this.danmakuConfig.showScroll;
          this.pushDanmakuConfig();
        },
        onToggleTop: () => {
          this.danmakuConfig.showTop = !this.danmakuConfig.showTop;
          this.pushDanmakuConfig();
        },
        onToggleBottom: () => {
          this.danmakuConfig.showBottom = !this.danmakuConfig.showBottom;
          this.pushDanmakuConfig();
        }
      })
    } else if (this.openPanel === PlayerPanelKind.Volume) {
      PlayerVolumePanel({
        volume: this.volume,
        muted: this.muted,
        onVolume: (value: number) => {
          this.applyVolume(value * 100);
        },
        onToggleMute: () => {
          this.muted = !this.muted;
          this.player.setMute(this.muted);
          this.notifyInteraction();
        }
      })
    } else if (this.openPanel === PlayerPanelKind.Info) {
      PlayerInfoPanel({
        player: this.player,
        positionMs: this.positionMs,
        durationMs: this.durationMs,
        bufferedMs: this.bufferedMs,
        downloadSpeedBps: this.downloadSpeedBps,
        decoderNotice: this.decoderNotice,
        backgroundPlayEnabled: this.backgroundPlayEnabled,
        mirror: this.mirrorMode,
        colorVision: this.colorVision,
        captureInFlight: this.captureInFlight,
        onToggleBackgroundPlay: () => {
          this.onToggleBackgroundPlay();
        },
        onMirrorChange: (mode: MirrorUiMode) => {
          this.mirrorMode = mode;
          this.player.setMirrorMode(toKernelMirror(mode));
        },
        onColorVisionChange: (kind: ColorVisionKind) => {
          this.colorVision = kind;
          this.player.setColorMatrix(colorVisionMatrix(kind));
        },
        onCapture: () => {
          this.onRequestSnapshot();
        }
      })
    }
  }

  // -------------------------------------------------------------------------
  // 浮层
  // -------------------------------------------------------------------------

  @Builder
  loadingOverlay() {
    Column() {
      LoadingProgress()
        .width(PlayerSize.loadingIndicatorSize)
        .height(PlayerSize.loadingIndicatorSize)
        .color(PlayerTheme.accent)
    }
    .width('100%')
    .height('100%')
    .justifyContent(FlexAlign.Center)
    .hitTestBehavior(HitTestMode.None)
  }

  @Builder
  errorOverlay() {
    Column() {
      Column() {
        Text('播放出错')
          .fontSize(PlayerSize.errorTitleFontSize)
          .fontColor(PlayerTheme.panelText)
        Row() {
        }
        .height(PlayerSize.errorTitleGap)
        Text(this.errorMessage)
          .fontSize(PlayerSize.errorMessageFontSize)
          .fontColor(PlayerTheme.panelSubText)
        Row() {
        }
        .height(PlayerSize.errorButtonGap)
        Button('重新输入链接')
          .backgroundColor(PlayerTheme.accent)
          .fontColor('#FFFFFFFF')
          .onClick(() => {
            this.onRequestErrorExit();
          })
      }
      .borderRadius(PlayerSize.errorCardRadius)
      .backgroundColor(PlayerTheme.panelBg)
      .padding(PlayerSize.errorCardPadding)
      .alignItems(HorizontalAlign.Center)
    }
    .width('100%')
    .height('100%')
    .justifyContent(FlexAlign.Center)
  }

  @Builder
  replayOverlay() {
    Column() {
      Column() {
        Text('视频播放结束')
          .fontSize(PlayerSize.errorTitleFontSize)
          .fontColor(PlayerTheme.panelText)
        Row() {
        }
        .height(PlayerSize.errorButtonGap)
        Button('重播')
          .backgroundColor(PlayerTheme.accent)
          .fontColor('#FFFFFFFF')
          .onClick(() => {
            this.onRequestReplay();
          })
      }
      .borderRadius(PlayerSize.errorCardRadius)
      .backgroundColor(PlayerTheme.panelBg)
      .padding(PlayerSize.errorCardPadding)
      .alignItems(HorizontalAlign.Center)
    }
    .width('100%')
    .height('100%')
    .justifyContent(FlexAlign.Center)
  }

  @Builder
  qualityNotice() {
    Column() {
      Text(this.noticeVisibleText())
        .fontSize(PlayerSize.noticeFontSize)
        .fontColor(PlayerTheme.panelText)
        .borderRadius(PlayerSize.noticeRadius)
        .backgroundColor(PlayerTheme.panelBg)
        .padding({
          left: PlayerSize.noticePaddingH,
          right: PlayerSize.noticePaddingH,
          top: PlayerSize.noticePaddingV,
          bottom: PlayerSize.noticePaddingV
        })
    }
    .width('100%')
    .height('100%')
    .justifyContent(FlexAlign.End)
    .alignItems(HorizontalAlign.Start)
    .padding({ left: PlayerSize.noticePaddingStart, bottom: PlayerSize.noticePaddingBottom })
    .hitTestBehavior(HitTestMode.None)
  }

  @Builder
  controlLayer() {
    Stack({ alignContent: Alignment.TopStart }) {
      if (this.isFullscreen) {
        Column() {
          PlayerTopBar({
            title: this.title,
            watchingText: this.watchingText,
            onBack: this.onBack,
            onTitleClick: () => {
              this.setPanel(this.openPanel === PlayerPanelKind.Info
                ? PlayerPanelKind.None
                : PlayerPanelKind.Info);
            }
          })
        }
        .width('100%')
        .linearGradient({
          angle: 180,
          colors: [[PlayerSize.topBarMaskTop, 0.0], [PlayerSize.topBarMaskBottom, 1.0]]
        })
      }

      Column() {
        /* 浮层面板：贴在控制条上方、右对齐 */
        Row() {
          Column() {
            this.panelContent()
          }
          .padding({ right: PlayerSize.panelAnchorEnd, bottom: PlayerSize.panelAnchorBottom })
        }
        .width('100%')
        .justifyContent(FlexAlign.End)

        Column() {
          PlayerBottomBar({
            isPlaying: this.isPlaying,
            positionMs: this.positionMs,
            durationMs: this.durationMs,
            bufferedMs: this.bufferedMs,
            isFullscreen: this.isFullscreen,
            danmakuEnabled: this.danmakuEnabled,
            qualityLabel: this.currentQualityLabel(),
            speedLabel: this.speed === 1 ? '倍速' : `${this.speed}x`,
            previewFrames: this.previewFrames,
            previewPositionMs: this.positionMs,
            previewActive: false,
            onPlayPause: () => {
              this.togglePlay();
            },
            onSeek: (ms: number) => {
              this.showControls = true;
              this.player.seekTo(ms, false);
              this.notifyInteraction();
            },
            onSeekStart: () => {
              this.showControls = true;
              this.notifyInteraction();
            },
            onSeekEnd: () => {
              this.notifyInteraction();
            },
            onTogglePortraitFullscreen: this.onTogglePortraitFullscreen,
            onToggleFullscreen: this.onToggleFullscreen,
            onToggleDanmaku: () => {
              this.danmakuEnabled = !this.danmakuEnabled;
              this.pushDanmakuConfig();
            },
            onDanmakuSettings: () => {
              this.setPanel(PlayerPanelKind.Danmaku);
            },
            onDanmakuSend: (text: string) => {
              this.appendDanmaku(text);
            },
            onSubtitleClick: () => {
              this.setPanel(PlayerPanelKind.Subtitle);
            },
            onSpeedClick: () => {
              this.setPanel(PlayerPanelKind.Speed);
            },
            onQualityClick: () => {
              /* 列表非空才允许打开（Qt：hasQuality = qualities.length > 0） */
              if (this.qualityOptions().length > 0) {
                this.setPanel(PlayerPanelKind.Quality);
              }
            },
            onInteraction: () => {
              this.notifyInteraction();
            },
            onDanmakuInputFocusChanged: (focused: boolean) => {
              this.danmakuInputFocused = focused;
              this.restartAutoHide();
            }
          })
        }
        .width('100%')
        .linearGradient({
          angle: 180,
          colors: [[PlayerTheme.colors.controlMaskTop, 0.0], [PlayerTheme.colors.controlMaskBottom, 1.0]]
        })
      }
      .width('100%')
      .height('100%')
      .justifyContent(FlexAlign.End)
    }
    .width('100%')
    .height('100%')
    .transition(TransitionEffect.OPACITY.animation({ duration: FADE_DURATION_MS, curve: Curve.EaseInOut }))
  }

  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      /* ① 画面：XComponent 的 surface 直接交给内核（零拷贝直出） */
      XComponent({
        id: 'cicadaComposeSurface',
        type: XComponentType.SURFACE,
        controller: this.xcomponentController
      })
        .onLoad(() => {
          this.onSurfaceLoad();
        })
        .onDestroy(() => {
          this.onSurfaceDestroy();
        })
        .width('100%')
        .height('100%')
        .backgroundColor(PlayerTheme.videoBg)

      /* ② 手势层：压在画面之上、控制层之下 */
      PlayerGestureLayer({
        locked: false,
        durationMs: this.durationMs,
        positionMs: this.positionMs,
        getVolumePercent: () => Math.trunc(this.player.getVolume() * 100),
        getBrightnessPercent: () => this.brightnessPercent,
        onBrightness: (percent: number) => {
          this.applyBrightness(percent);
        },
        onVolume: (percent: number) => {
          this.applyVolume(percent);
        },
        onSeekTo: (ms: number) => {
          this.player.seekTo(ms, false);
        },
        onTogglePlay: () => {
          this.togglePlay();
        },
        onSingleTap: () => {
          this.onSurfaceTap();
        },
        onSpeedBoost: (active: boolean) => {
          if (active) {
            this.beginSpeedBoost();
          } else {
            this.endSpeedBoost();
          }
        }
      })

      /* ③ 弹幕层 */
      DanmakuView({
        danmakus: this.danmakus,
        config: this.danmakuConfig,
        isPlaying: this.isPlaying,
        currentTime: this.positionMs,
        revision: this.danmakuRevision
      })

      /* ④ 加载 / 缓冲 / seek 反馈：画面保持上一帧，不清屏 */
      if (this.loading) {
        this.loadingOverlay()
      }

      /* ⑤ 出错 */
      if (this.errorMessage.length > 0) {
        this.errorOverlay()
      }

      /* ⑥ 播放结束 → 重播 */
      if (this.replayVisible && this.errorMessage.length === 0) {
        this.replayOverlay()
      }

      /* ⑦ 控制层（单击显隐；播放中 3 秒自动隐藏） */
      if (this.showControls && this.errorMessage.length === 0) {
        this.controlLayer()
      }

      /* ⑧ 清晰度切换提示（画面左下角） */
      if (this.noticeVisibleText().length > 0 && this.errorMessage.length === 0) {
        this.qualityNotice()
      }

      /* ⑨ 非全屏：左上角返回键（全屏时的返回在顶部栏里；根 Stack 默认就是左上角对齐） */
      if (!this.isFullscreen) {
        Column() {
          IconButtonBox({ boxSize: PlayerSize.iconButtonSize, onTap: this.onBack }) {
            PlayerBackIcon({ iconSize: PlayerSize.backButtonIconSize, label: '返回' })
          }
        }
        .margin({ left: PlayerSize.backButtonPadding, top: PlayerSize.backButtonPadding })
      }

      /* ⑩ 常驻细进度条（**控制栏隐藏后才显示**，贴最底部、通宽、不带 dot） */
      if (!this.showControls) {
        Column() {
          PlayerThinProgressBar({
            positionMs: this.positionMs,
            durationMs: this.durationMs,
            bufferedMs: this.bufferedMs
          })
        }
        .width('100%')
        .height('100%')
        .justifyContent(FlexAlign.End)
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(PlayerTheme.videoBg)
  }
}
