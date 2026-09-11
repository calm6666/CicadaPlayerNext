/*
 * Player control bar — HarmonyOS counterpart of the Android demo's ControlView
 * (cicada_view_control.xml).
 *
 * Faithful to the Android behaviour:
 *  - title bar at the top: back arrow + title (the demo's tv_title is empty for
 *    the in-player bar, so the page passes its own);
 *  - bottom info bar: "buffer: " label, position / "/" / duration, seek bar;
 *  - a screen-mode text; "全屏" while windowed, "缩放" while full screen;
 *  - a screen-lock button that only exists in full-screen mode.
 * Deliberately NOT ported (they are dead code on Android too): the play/pause
 * button (the layout has none — the listener is registered but unreachable) and
 * the quality button (permanently hidden by mForceQuality = true).
 */

import { formatMs } from '../util/TimeFormater';
import { S } from '../util/Strings';

@Component
export struct PlayerControlBar {
  @Prop title: string = '';
  @Prop isFullScreen: boolean = false;
  @Prop locked: boolean = false;
  @Prop playing: boolean = false;
  @Prop durationMs: number = 0;
  @Prop bufferMs: number = 0;
  /** Playback position; while dragging the page passes the preview value. */
  @Prop positionMs: number = 0;

  onBack: () => void = () => {};
  onToggleScreenMode: () => void = () => {};
  onToggleLock: () => void = () => {};
  onSeekStart: () => void = () => {};
  onSeekChange: (ms: number) => void = () => {};
  onSeekEnd: (ms: number) => void = () => {};

  build() {
    Column() {
      // ---- top: title row (hidden while the screen is locked) ----
      if (!this.locked) {
        Row() {
          Text('←')
            .fontSize(20)
            .fontColor('#e7e7e7')
            .width(40)
            .height('100%')
            .textAlign(TextAlign.Center)
            .onClick(() => this.onBack())

          Text(this.title)
            .fontSize(18)
            .fontColor('#e7e7e7')
            .layoutWeight(1)
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
            .margin({ left: 8, right: 8 })

          // Lock toggle: only meaningful in full screen, as on Android.
          if (this.isFullScreen) {
            Text(this.locked ? '🔒' : '🔓')
              .fontSize(18)
              .fontColor('#e7e7e7')
              .width(44)
              .height('100%')
              .textAlign(TextAlign.Center)
              .onClick(() => this.onToggleLock())
          }
        }
        .width('100%')
        .height(44)
        .backgroundColor('#99000000')
      }

      Blank().layoutWeight(1)

      // ---- bottom: info + seek bar ----
      if (!this.locked) {
        Column() {
          Row() {
            Text(S.bufferLabel + formatMs(this.bufferMs))
              .fontSize(12)
              .fontColor('#e7e7e7')
              .width('100%')
          }
          .width('100%')

          Row() {
            Text(formatMs(this.positionMs))
              .fontSize(12)
              .fontColor('#e7e7e7')
            Text('/')
              .fontSize(12)
              .fontColor('#e7e7e7')
              .margin({ left: 4, right: 4 })
            Text(formatMs(this.durationMs))
              .fontSize(12)
              .fontColor('#9f9e9e')

            Slider({
              value: this.positionMs,
              min: 0,
              max: this.durationMs > 0 ? this.durationMs : 100,
              step: 1,
              style: SliderStyle.OutSet
            })
              .layoutWeight(1)
              .trackThickness(3)
              .selectedColor('#379DF2')
              .trackColor('#999999')
              .blockColor('#379DF2')
              .margin({ left: 8, right: 8 })
              .onChange((value: number, mode: SliderChangeMode) => {
                if (mode === SliderChangeMode.Begin) {
                  this.onSeekStart();
                } else if (mode === SliderChangeMode.Moving) {
                  this.onSeekChange(value);
                } else {
                  this.onSeekChange(value);
                  this.onSeekEnd(value);
                }
              })
          }
          .width('100%')
          .height(38)

          Row() {
            Text(this.isFullScreen ? S.screenModelLarge : S.screenModelSmall)
              .fontSize(12)
              .fontColor('#379DF2')
              .padding(5)
              .onClick(() => this.onToggleScreenMode())
          }
          .width('100%')
        }
        .width('100%')
        .padding({ left: 8, right: 8, top: 4, bottom: 4 })
        .backgroundColor('#99000000')
      }
    }
    .width('100%')
    .height('100%')
  }
}
