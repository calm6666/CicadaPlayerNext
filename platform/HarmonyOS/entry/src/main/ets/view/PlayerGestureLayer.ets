/*
 * Player gesture layer — HarmonyOS counterpart of the Android demo's
 * GestureView / GestureControl.
 *
 * Behaviour taken 1:1 from the Android source:
 *  - the vertical side is latched ONCE from the down-X position
 *    (x < width/2 -> left = brightness, x > width/2 -> right = volume) and
 *    stays latched even if the finger crosses the middle;
 *  - the change is a screen-height percentage:
 *        changePercent = (int)((nowY - downY) * 100 / viewHeight)
 *    so dragging UP yields a negative percent;
 *  - brightness: target = (int)(windowBrightness * 100) - changePercent,
 *    clamped [0, 100];
 *  - volume: target = volumeAtGestureStart - changePercent, clamped [0, 100],
 *    where the baseline is sampled once when the gesture begins;
 *  - double tap toggles play/pause; single tap is a no-op (as on Android, where
 *    onSingleTap()'s body is commented out);
 *  - while the screen is locked only single tap is delivered.
 *
 * DELIBERATE DEVIATION: horizontal-swipe seek is implemented here. On Android
 * the implementation exists but the calls that would show/commit the SeekDialog
 * are commented out, so the gesture does nothing. The documented SeekDialog
 * algorithm is reproduced below; set ENABLE_HORIZONTAL_SEEK to false for strict
 * parity with the Android build.
 *
 * The Android indicators use mipmap icons (cicada_brightness, cicada_volume_mute/
 * unmute, cicada_seek_forward/rewind). Those drawables have not been re-authored
 * for HarmonyOS yet, so the indicator uses text glyphs of the same meaning.
 */

import { formatMs } from '../util/TimeFormater';

const ENABLE_HORIZONTAL_SEEK: boolean = true;
/** Android: dimen/cicada_gesture_dialog_size. */
const DIALOG_SIZE: number = 155;

type GestureAxis = 'none' | 'vertical' | 'horizontal';
type GestureSide = 'none' | 'left' | 'right';

@Component
export struct PlayerGestureLayer {
  @Prop locked: boolean = false;
  @Prop durationMs: number = 0;
  @Prop positionMs: number = 0;
  /** Volume as 0..100; sampled once at gesture start, matching Android. */
  getVolumePercent: () => number = () => 100;
  /**
   * Current brightness as 0..100, also sampled at gesture start. Android reads
   * activity.window.attributes.screenBrightness, which is a "system default"
   * sentinel until the app sets it; HarmonyOS offers no read-back, so the page
   * owns the value it last applied.
   */
  getBrightnessPercent: () => number = () => 50;
  onBrightness: (percent: number) => void = () => {};
  onVolume: (percent: number) => void = () => {};
  onSeekTo: (positionMs: number) => void = () => {};
  onTogglePlay: () => void = () => {};

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

  /** Android: (int)((nowY - downY) * 100 / playerView.getHeight()). */
  private changePercent(deltaY: number): number {
    if (this.viewHeight <= 0) {
      return 0;
    }
    return Math.trunc((deltaY * 100) / this.viewHeight);
  }

  /** Android SeekDialog: the divisor shrinks as the media gets shorter. */
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

  /** Android: target = delta / divisor + currentPosition, then clamped. */
  private seekTarget(deltaX: number): number {
    let target: number = deltaX / this.seekDivisor() + this.positionMs;
    if (target < 0) {
      target = 0;
    }
    if (target > this.durationMs) {
      target = this.durationMs;
    }
    // Caller-side clamp from the Android code: never land exactly on the end.
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
    const fingers = event.fingerList;
    if (fingers === undefined || fingers.length === 0) {
      return;
    }
    this.startX = fingers[0].localX;
    this.startY = fingers[0].localY;
    // The side is latched from the DOWN position only.
    this.side = this.startX < this.viewWidth / 2 ? 'left' : 'right';
    // Baseline volume/brightness are sampled once, as Android does when it
    // creates the gesture dialog.
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
      if (!ENABLE_HORIZONTAL_SEEK) {
        return;
      }
      this.pendingSeekMs = this.seekTarget(deltaX);
      this.showIndicator(deltaX >= 0 ? '⏩' : '⏪', formatMs(this.pendingSeekMs));
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
    if (!this.locked && this.axis === 'horizontal' && ENABLE_HORIZONTAL_SEEK) {
      if (this.pendingSeekMs > 0) {
        this.onSeekTo(this.pendingSeekMs);
      }
    }
    this.axis = 'none';
    this.side = 'none';
  }

  build() {
    Stack() {
      // Transparent full-area touch target.
      Column()
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
              .onActionStart((event: GestureEvent) => this.onActionStart(event))
              .onActionUpdate((event: GestureEvent) => this.onActionUpdate(event))
              .onActionEnd(() => this.onActionEnd())
              .onActionCancel(() => this.onActionEnd()),
            TapGesture({ count: 2 })
              .onAction(() => {
                // Android: onDoubleTap -> switchPlayerState(); suppressed while locked.
                if (!this.locked) {
                  this.onTogglePlay();
                }
              }),
            TapGesture({ count: 1 })
              .onAction(() => {
                // Android's onSingleTap() body is commented out: nothing happens.
              })
          )
        )

      // Gesture indicator (Android: a 155dp centred PopupWindow).
      if (this.indicatorVisible) {
        Column() {
          Text(this.indicatorIcon).fontSize(34).fontColor('#333333')
          Text(this.indicatorText).fontSize(24).fontColor('#333333').margin({ top: 4 })
        }
        .width(DIALOG_SIZE)
        .height(DIALOG_SIZE)
        .justifyContent(FlexAlign.Center)
        .backgroundColor('#66ffffff')
        .borderRadius(5)
      }
    }
    .width('100%')
    .height('100%')
  }
}
