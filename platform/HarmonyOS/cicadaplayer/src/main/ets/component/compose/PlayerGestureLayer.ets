/*
 * 播放器手势层 —— 画面的触摸层（压在 XComponent 之上、控制层之下）。
 *
 * 【Compose 的部分（源真值）】
 *   · 单击 = 显示/隐藏顶部栏 + 控制栏；有面板开着时先关面板（CicadaVideoPlayer.kt 的
 *     `detectTapGestures(onTap)`）；
 *   · 双击 = 播放/暂停（`onDoubleTap`）；
 *   · 两种都算一次交互（把控制栏的自动隐藏往后推）。
 *
 * 【Compose 里没有、按任务清单从既有鸿蒙手势层保留的部分】
 *   · 左半屏竖向拖动 = 亮度，右半屏竖向拖动 = 音量（按下位置**一次性**锁定左右侧）；
 *   · 横向拖动 = seek 预览，**松手才提交**（与进度条同一条"松手才 seek"的约定）；
 *   · 长按 = 临时倍速（松手恢复）；
 *   · 手势指示（旧的 155dp 圆形浮层，配色改成播放器面板色，与其它浮层统一）。
 *   这几项在 Compose 源码里不存在（Compose 安卓版只有单击/双击），是任务清单明确要求的手势层能力，
 *   所以保留；如果只按 Compose 对齐，删掉本文件里对应分支即可（其余部分不动）。
 *
 * 与旧鸿蒙手势层的差异：单击不再是空实现（Compose 的单击要显隐控制栏），
 * 所以那个"单击空实现"的旧行为在这里被 Compose 的行为取代。
 */

import { PlayerSize, PlayerTheme } from '../theme/PlayerTheme';
import { formatPlayerTime } from './PlayerControls';

type GestureAxis = 'none' | 'vertical' | 'horizontal';
type GestureSide = 'none' | 'left' | 'right';

/** 旧鸿蒙手势层的浮层尺寸（Android dimen/cicada_gesture_dialog_size）。 */
const DIALOG_SIZE: number = 155;
/** 长按触发临时倍速的时长。 */
const LONG_PRESS_MS: number = 500;

@Component
export struct PlayerGestureLayer {
  /** 锁屏时不响应任何手势（沿用旧鸿蒙手势层的约定）。 */
  @Prop locked: boolean = false;
  @Prop durationMs: number = 0;
  @Prop positionMs: number = 0;
  /** 音量 0..100；手势开始时采样一次。 */
  getVolumePercent: () => number = () => 100;
  /** 亮度 0..100；手势开始时采样一次。 */
  getBrightnessPercent: () => number = () => 50;
  onBrightness: (percent: number) => void = () => {
  };
  onVolume: (percent: number) => void = () => {
  };
  onSeekTo: (positionMs: number) => void = () => {
  };
  onTogglePlay: () => void = () => {
  };
  /** 单击：显隐控制栏 / 关面板。 */
  onSingleTap: () => void = () => {
  };
  /** 长按临时倍速：true = 提速开始，false = 恢复。 */
  onSpeedBoost: (active: boolean) => void = () => {
  };

  private viewWidth: number = 0;
  private viewHeight: number = 0;

  private startX: number = 0;
  private startY: number = 0;
  private axis: GestureAxis = 'none';
  private side: GestureSide = 'none';
  private startVolumePercent: number = 100;
  private startBrightnessPercent: number = 50;
  private pendingSeekMs: number = 0;

  @State indicatorVisible: boolean = false;
  @State indicatorIcon: string = '';
  @State indicatorText: string = '';

  private clampPercent(value: number): number {
    if (value < 0) {
      return 0;
    }
    return value > 100 ? 100 : value;
  }

  /** 屏幕高度百分比：Android 的 `(int)((nowY - downY) * 100 / viewHeight)`。 */
  private changePercent(deltaY: number): number {
    if (this.viewHeight <= 0) {
      return 0;
    }
    return Math.trunc((deltaY * 100) / this.viewHeight);
  }

  /** 媒体越短，横向拖动的除数越小（Android SeekDialog 的算法）。 */
  private seekDivisor(): number {
    const totalMinutes: number = Math.floor(this.durationMs / 1000 / 60);
    const hours: number = Math.floor(totalMinutes / 60);
    const minutes: number = totalMinutes % 60;
    if (hours >= 1) {
      return 10;
    }
    if (minutes > 30) {
      return 5;
    }
    if (minutes > 10) {
      return 3;
    }
    if (minutes > 3) {
      return 2;
    }
    return 1;
  }

  private seekTarget(deltaX: number): number {
    let target: number = deltaX / this.seekDivisor() + this.positionMs;
    if (target < 0) {
      target = 0;
    }
    if (this.durationMs > 0 && target >= this.durationMs) {
      target = this.durationMs - 1000;
    }
    return target < 0 ? 0 : target;
  }

  private showIndicator(icon: string, text: string): void {
    this.indicatorIcon = icon;
    this.indicatorText = text;
    this.indicatorVisible = true;
  }

  private onActionStart(event: GestureEvent): void {
    if (this.locked) {
      return;
    }
    const fingers: FingerInfo[] = event.fingerList;
    if (fingers === undefined || fingers.length === 0) {
      return;
    }
    this.startX = fingers[0].localX;
    this.startY = fingers[0].localY;
    /* 左右侧从**按下位置**一次性锁定，越过中线也不改（Android 同款） */
    this.side = this.startX < this.viewWidth / 2 ? 'left' : 'right';
    this.startVolumePercent = this.getVolumePercent();
    this.startBrightnessPercent = this.getBrightnessPercent();
    this.axis = 'none';
  }

  private onActionUpdate(event: GestureEvent): void {
    if (this.locked) {
      return;
    }
    const deltaX: number = event.offsetX;
    const deltaY: number = event.offsetY;

    if (this.axis === 'none') {
      this.axis = Math.abs(deltaX) > Math.abs(deltaY) ? 'horizontal' : 'vertical';
    }

    if (this.axis === 'horizontal') {
      this.pendingSeekMs = this.seekTarget(deltaX);
      this.showIndicator(deltaX >= 0 ? '⏩' : '⏪', formatPlayerTime(this.pendingSeekMs));
      return;
    }

    const percent: number = this.changePercent(deltaY);
    if (this.side === 'left') {
      const target: number = this.clampPercent(this.startBrightnessPercent - percent);
      this.showIndicator('☀', `${target}%`);
      this.onBrightness(target);
    } else {
      const target: number = this.clampPercent(this.startVolumePercent - percent);
      this.showIndicator(target <= 5 ? '🔇' : '🔊', `${target}%`);
      this.onVolume(target);
    }
  }

  private onActionEnd(): void {
    this.indicatorVisible = false;
    if (!this.locked && this.axis === 'horizontal' && this.pendingSeekMs > 0) {
      this.onSeekTo(this.pendingSeekMs);
    }
    this.axis = 'none';
    this.side = 'none';
  }

  build() {
    Stack({ alignContent: Alignment.Center }) {
      /* 透明全屏触摸层 */
      Column() {
      }
      .width('100%')
      .height('100%')
      .backgroundColor(Color.Transparent)
      .onAreaChange((oldValue: Area, newValue: Area) => {
        this.viewWidth = Number(newValue.width);
        this.viewHeight = Number(newValue.height);
      })
      .gesture(
        GestureGroup(GestureMode.Exclusive,
          PanGesture({ fingers: 1, distance: 5 })
            .onActionStart((event: GestureEvent) => {
              this.onActionStart(event);
            })
            .onActionUpdate((event: GestureEvent) => {
              this.onActionUpdate(event);
            })
            .onActionEnd(() => {
              this.onActionEnd();
            })
            .onActionCancel(() => {
              this.onActionEnd();
            }),
          LongPressGesture({ repeat: false, duration: LONG_PRESS_MS })
            .onAction(() => {
              if (!this.locked) {
                this.onSpeedBoost(true);
              }
            })
            .onActionEnd(() => {
              this.onSpeedBoost(false);
            })
            .onActionCancel(() => {
              this.onSpeedBoost(false);
            }),
          /* 双击优先于单击（Compose：双击 = 播放/暂停，单击 = 显隐控制栏） */
          TapGesture({ count: 2 })
            .onAction(() => {
              if (!this.locked) {
                this.onTogglePlay();
              }
            }),
          TapGesture({ count: 1 })
            .onAction(() => {
              if (!this.locked) {
                this.onSingleTap();
              }
            })
        )
      )

      /* 手势指示（旧的 155dp 浮层；配色换成播放器面板色，与其它浮层统一） */
      if (this.indicatorVisible) {
        Column() {
          Text(this.indicatorIcon)
            .fontSize(34)
            .fontColor(PlayerTheme.panelText)
          Text(this.indicatorText)
            .fontSize(24)
            .fontColor(PlayerTheme.panelText)
            .margin({ top: 4 })
        }
        .width(DIALOG_SIZE)
        .height(DIALOG_SIZE)
        .justifyContent(FlexAlign.Center)
        .backgroundColor(PlayerTheme.panelBg)
        .borderRadius(PlayerSize.panelRadius)
        .hitTestBehavior(HitTestMode.None)
      }
    }
    .width('100%')
    .height('100%')
  }
}
