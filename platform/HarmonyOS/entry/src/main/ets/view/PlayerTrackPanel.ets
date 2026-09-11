/*
 * Track & speed selection panel — HarmonyOS counterpart of the Android demo's
 * PlayerTrackFragment (the bottom "track" tab) plus the speed选择 that the demo
 * exposes through PlayerOperationFragment's speedMode radio group.
 *
 * Section order mirrors fragment_player_track.xml:
 *   清晰度 (definition) | 码率 (bitrate) | 音轨 (soundtrack) | 外挂字幕 | 字幕
 * with 倍速 appended.
 *
 * Faithful details:
 *  - 清晰度 is always empty on Android (the TYPE_VOD branch is commented out), so
 *    it is rendered only when it actually has options, and the section is hidden
 *    otherwise — same rule Android applies to ll_definition/ll_bitrate/
 *    ll_soundtrack/ll_subtitle;
 *  - 码率 gets an "自动码率" entry first, whose tag is null (passed here as
 *    index -1) and whose label gains the current bitrate in parentheses once a
 *    track is known;
 *  - 外挂字幕 gets a trailing "取消外挂字幕" entry tagged -1, which is also the
 *    hide call.
 *
 * Primitive-array props are used deliberately: @Prop deep-copies, and arrays of
 * numbers/strings are the safest shape across ArkTS versions.
 */

import { S } from '../util/Strings';

/** Sentinel index meaning "auto bitrate" / "cancel external subtitle". */
export const TRACK_INDEX_AUTO: number = -1;

@Component
export struct PlayerTrackPanel {
  @Prop bitrateLabels: string[] = [];
  @Prop audioLabels: string[] = [];
  @Prop subtitleLabels: string[] = [];
  @Prop externalLabels: string[] = [];
  @Prop definitionLabels: string[] = [];

  /** Selected position inside each group (-1 = nothing selected). */
  @Prop selectedBitrate: number = -1;
  @Prop selectedAudio: number = -1;
  @Prop selectedSubtitle: number = -1;
  @Prop selectedExternal: number = -1;
  @Prop selectedDefinition: number = -1;
  /** Current speed, one of 0.5 / 1.0 / 1.5 / 2.0. */
  @Prop speed: number = 1.0;

  onSelectBitrate: (position: number) => void = () => {};
  onSelectAudio: (position: number) => void = () => {};
  onSelectSubtitle: (position: number) => void = () => {};
  onSelectExternal: (position: number) => void = () => {};
  onSelectDefinition: (position: number) => void = () => {};
  onSpeedChange: (speed: number) => void = () => {};

  @Builder
  section(title: string) {
    Text(title)
      .fontSize(16)
      .fontColor(Color.Black)
      .margin({ top: 12, bottom: 6 })
      .width('100%')
  }

  @Builder
  option(label: string, selected: boolean, action: () => void) {
    Text(label)
      .fontSize(14)
      .fontColor(selected ? Color.White : '#3F51B5')
      .textAlign(TextAlign.Center)
      .width('100%')
      .height(36)
      .borderRadius(4)
      .borderWidth(1)
      .borderColor('#3F51B5')
      .backgroundColor(selected ? '#3F51B5' : Color.Transparent)
      .margin({ bottom: 6 })
      .onClick(action)
  }

  build() {
    Scroll() {
      Column() {
        if (this.definitionLabels.length > 0) {
          this.section('清晰度')
          ForEach(this.definitionLabels, (label: string, position: number) => {
            this.option(label, position === this.selectedDefinition, () => this.onSelectDefinition(position))
          }, (label: string, position: number) => `d-${position}-${label}`)
        }

        if (this.bitrateLabels.length > 0) {
          this.section('码率')
          ForEach(this.bitrateLabels, (label: string, position: number) => {
            this.option(label, position === this.selectedBitrate, () => this.onSelectBitrate(position))
          }, (label: string, position: number) => `b-${position}-${label}`)
        }

        if (this.audioLabels.length > 0) {
          this.section('音轨')
          ForEach(this.audioLabels, (label: string, position: number) => {
            this.option(label, position === this.selectedAudio, () => this.onSelectAudio(position))
          }, (label: string, position: number) => `a-${position}-${label}`)
        }

        if (this.externalLabels.length > 0) {
          this.section('外挂字幕')
          ForEach(this.externalLabels, (label: string, position: number) => {
            this.option(label, position === this.selectedExternal, () => this.onSelectExternal(position))
          }, (label: string, position: number) => `x-${position}-${label}`)
        }

        if (this.subtitleLabels.length > 0) {
          this.section('字幕')
          ForEach(this.subtitleLabels, (label: string, position: number) => {
            this.option(label, position === this.selectedSubtitle, () => this.onSelectSubtitle(position))
          }, (label: string, position: number) => `s-${position}-${label}`)
        }

        // ---- 倍速 (PlayerOperationFragment's speedMode group) ----
        this.section('倍速播放')
        ForEach([0.5, 1.0, 1.5, 2.0], (value: number) => {
          this.option(this.speedLabel(value), value === this.speed, () => this.onSpeedChange(value))
        }, (value: number) => `sp-${value}`)
      }
      .width('100%')
      .padding(12)
    }
    .width('100%')
    .layoutWeight(1)
    .backgroundColor('#F5F5F5')
  }

  /** Labels taken from strings.xml (cicada_speed_*). */
  private speedLabel(value: number): string {
    if (value === 0.5) {
      return S.speedOptfTimes;
    }
    if (value === 1.0) {
      return S.speedOneTimes;
    }
    if (value === 1.5) {
      return S.speedOptTimes;
    }
    return S.speedTwiceTimes;
  }
}
