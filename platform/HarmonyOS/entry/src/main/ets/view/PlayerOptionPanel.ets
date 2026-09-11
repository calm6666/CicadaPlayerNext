/*
 * Player option panel — HarmonyOS counterpart of three Android bottom tabs:
 *   PlayerOperationFragment  ("options")      -> tab 0 操作
 *   PlayerConfigFragment     ("playerConfig") -> tab 1 播放器配置
 *   PlayerCacheConfigFragment("cacheConfig")  -> tab 2 缓存配置
 * They are merged into one component with a small tab row (ArkUI has no
 * FragmentManager); the field sets and labels are taken 1:1 from the Android
 * layouts.
 *
 * Android defaults preserved:
 *   cache Max Duration 300 s, Max Size 200 MB, Open Cache Config unchecked;
 *   retry-count hint "2" (the Android code hardcodes 2 on apply);
 *   ScaleMode default fit, Mirror default none, Rotation default 0,
 *   Accurate Seek default off (i.e. INACCURATE seeking), Speed default 1.0.
 */

import { S } from '../util/Strings';

@Component
export struct PlayerOptionPanel {
  @Prop autoPlay: boolean = false;
  @Prop mute: boolean = false;
  @Prop loop: boolean = false;
  @Prop hardware: boolean = true;
  @Prop accurateSeek: boolean = false;
  /** Volume as 0..100 on the slider (Android's IndicatorSeekBar max is 100). */
  @Prop volumePercent: number = 50;
  /** ScaleMode: 0 fit / 1 aspect-fill / 2 fill. */
  @Prop scaleMode: number = 0;
  /** MirrorMode: 0 none / 1 horizontal / 2 vertical. */
  @Prop mirrorMode: number = 0;
  /** RotateMode in degrees: 0 / 90 / 180 / 270. */
  @Prop rotateMode: number = 0;
  /** Initial text for the config fields (from the player's options). */
  @Prop initialStartBuffer: string = '';
  @Prop initialHighBuffer: string = '';
  @Prop initialMaxBuffer: string = '';
  @Prop initialMaxDelay: string = '';
  @Prop initialNetTimeout: string = '';
  @Prop initialProbeSize: string = '';
  @Prop initialReferrer: string = '';
  @Prop initialHttpProxy: string = '';
  @Prop initialRetryCount: string = '2';

  onAutoPlayChange: (value: boolean) => void = () => {};
  onMuteChange: (value: boolean) => void = () => {};
  onLoopChange: (value: boolean) => void = () => {};
  onHardwareChange: (value: boolean) => void = () => {};
  onAccurateSeekChange: (value: boolean) => void = () => {};
  onVolumeChange: (percent: number) => void = () => {};
  onScaleModeChange: (mode: number) => void = () => {};
  onMirrorModeChange: (mode: number) => void = () => {};
  onRotateModeChange: (mode: number) => void = () => {};
  onShowMediaInfo: () => void = () => {};
  onApplyConfig: (startBuffer: string, highBuffer: string, maxBuffer: string, maxDelay: string,
    netTimeout: string, probeSize: string, referrer: string, httpProxy: string,
    retryCount: string, clearFrameWhenStop: boolean) => void = () => {};
  onApplyCacheConfig: (enable: boolean, maxDurationS: string, maxSizeMB: string, dir: string) => void = () => {};

  @State tab: number = 0;
  @State startBuffer: string = '';
  @State highBuffer: string = '';
  @State maxBuffer: string = '';
  @State maxDelay: string = '';
  @State netTimeout: string = '';
  @State probeSize: string = '';
  @State referrer: string = '';
  @State httpProxy: string = '';
  @State retryCount: string = '2';
  @State clearFrameWhenStop: boolean = false;
  @State cacheEnable: boolean = false;
  @State cacheMaxDurationS: string = '300';
  @State cacheMaxSizeMB: string = '200';
  @State cacheDir: string = '';

  aboutToAppear(): void {
    this.startBuffer = this.initialStartBuffer;
    this.highBuffer = this.initialHighBuffer;
    this.maxBuffer = this.initialMaxBuffer;
    this.maxDelay = this.initialMaxDelay;
    this.netTimeout = this.initialNetTimeout;
    this.probeSize = this.initialProbeSize;
    this.referrer = this.initialReferrer;
    this.httpProxy = this.initialHttpProxy;
    this.retryCount = this.initialRetryCount;
  }

  @Builder
  switchRow(label: string, on: boolean, action: (value: boolean) => void) {
    Row() {
      Text(label).fontSize(14).fontColor('#333333').layoutWeight(1)
      Toggle({ type: ToggleType.Switch, isOn: on }).onChange(action)
    }
    .width('100%')
    .height(44)
  }

  @Builder
  choiceRow(label: string, current: number, values: number[], labels: string[], action: (mode: number) => void) {
    Column() {
      Text(label).fontSize(14).fontColor('#333333').width('100%')
      Row({ space: 6 }) {
        ForEach(values, (value: number, position: number) => {
          Text(labels[position])
            .fontSize(13)
            .fontColor(current === value ? Color.White : '#3F51B5')
            .textAlign(TextAlign.Center)
            .layoutWeight(1)
            .height(32)
            .borderRadius(4)
            .borderWidth(1)
            .borderColor('#3F51B5')
            .backgroundColor(current === value ? '#3F51B5' : Color.Transparent)
            .onClick(() => action(value))
        }, (value: number) => `${label}-${value}`)
      }
      .width('100%')
      .margin({ top: 6 })
    }
    .width('100%')
    .margin({ top: 8, bottom: 4 })
  }

  @Builder
  textField(label: string, value: string, action: (text: string) => void) {
    Row() {
      Text(label).fontSize(13).fontColor('#333333').layoutWeight(1).textAlign(TextAlign.Center)
      TextInput({ text: value })
        .fontSize(13)
        .layoutWeight(2)
        .onChange(action)
    }
    .width('100%')
    .height(50)
  }

  @Builder
  operationTab() {
    Column() {
      this.switchRow('自动播放', this.autoPlay, (v: boolean) => this.onAutoPlayChange(v))
      this.switchRow('静音', this.mute, (v: boolean) => this.onMuteChange(v))
      this.switchRow('循环', this.loop, (v: boolean) => this.onLoopChange(v))
      this.switchRow('硬解码', this.hardware, (v: boolean) => this.onHardwareChange(v))
      this.switchRow('精准seek', this.accurateSeek, (v: boolean) => this.onAccurateSeekChange(v))

      Column() {
        Text(`音量 ${this.volumePercent}`).fontSize(14).fontColor('#333333').width('100%')
        Slider({ value: this.volumePercent, min: 0, max: 100, step: 1 })
          .width('100%')
          .onChange((value: number, mode: SliderChangeMode) => {
            if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
              // Android maps progress/50 to a 0..2.0 volume range.
              this.onVolumeChange(value);
            }
          })
      }
      .width('100%')
      .margin({ top: 8 })

      this.choiceRow('缩放模式', this.scaleMode, [0, 1, 2], ['比例适应', '比例填充', '拉伸全屏'],
        (m: number) => this.onScaleModeChange(m))
      this.choiceRow('镜像模式', this.mirrorMode, [0, 1, 2], ['无镜像', '水平镜像', '垂直镜像'],
        (m: number) => this.onMirrorModeChange(m))
      this.choiceRow('旋转模式', this.rotateMode, [0, 90, 180, 270], ['0°', '90°', '180°', '270°'],
        (m: number) => this.onRotateModeChange(m))

      Button('媒体信息').onClick(() => this.onShowMediaInfo())
        .width('100%')
        .height(40)
        .backgroundColor('#3F51B5')
        .fontColor(Color.White)
        .margin({ top: 12, bottom: 12 })
    }
    .width('100%')
  }

  @Builder
  configTab() {
    Column() {
      this.textField('启播缓冲时长', this.startBuffer, (t: string) => { this.startBuffer = t; })
      this.textField('卡顿恢复', this.highBuffer, (t: string) => { this.highBuffer = t; })
      this.textField('最大缓冲时长', this.maxBuffer, (t: string) => { this.maxBuffer = t; })
      this.textField('直播最大延迟', this.maxDelay, (t: string) => { this.maxDelay = t; })
      this.textField('网络超时', this.netTimeout, (t: string) => { this.netTimeout = t; })
      this.textField('probe大小', this.probeSize, (t: string) => { this.probeSize = t; })
      this.textField('请求referer', this.referrer, (t: string) => { this.referrer = t; })
      this.textField('httpProxy代理', this.httpProxy, (t: string) => { this.httpProxy = t; })
      this.textField('重连次数', this.retryCount, (t: string) => { this.retryCount = t; })
      this.switchRow('停止隐藏最后帧', this.clearFrameWhenStop,
        (v: boolean) => { this.clearFrameWhenStop = v; })

      Button('应用配置')
        .onClick(() => this.onApplyConfig(this.startBuffer, this.highBuffer, this.maxBuffer,
          this.maxDelay, this.netTimeout, this.probeSize, this.referrer, this.httpProxy,
          this.retryCount, this.clearFrameWhenStop))
        .width('100%')
        .height(40)
        .backgroundColor('#3F51B5')
        .fontColor(Color.White)
        .margin({ top: 12, bottom: 12 })
    }
    .width('100%')
  }

  @Builder
  cacheTab() {
    Column() {
      this.textField('最大时长(单位s)', this.cacheMaxDurationS, (t: string) => { this.cacheMaxDurationS = t; })
      this.textField('最大size(单位MB)', this.cacheMaxSizeMB, (t: string) => { this.cacheMaxSizeMB = t; })
      this.textField('保存路径', this.cacheDir, (t: string) => { this.cacheDir = t; })
      this.switchRow('开启缓存配置', this.cacheEnable, (v: boolean) => { this.cacheEnable = v; })

      Button('应用配置')
        .onClick(() => this.onApplyCacheConfig(this.cacheEnable, this.cacheMaxDurationS,
          this.cacheMaxSizeMB, this.cacheDir))
        .width('100%')
        .height(40)
        .backgroundColor('#3F51B5')
        .fontColor(Color.White)
        .margin({ top: 12, bottom: 12 })
    }
    .width('100%')
  }

  build() {
    Column() {
      Row({ space: 4 }) {
        ForEach(['options', 'playerConfig', 'cacheConfig'], (label: string, position: number) => {
          Text(label)
            .fontSize(13)
            .fontColor(this.tab === position ? '#3F51B5' : '#333333')
            .textAlign(TextAlign.Center)
            .layoutWeight(1)
            .height(36)
            .onClick(() => { this.tab = position; })
        }, (label: string) => label)
      }
      .width('100%')

      Divider().strokeWidth(1).color('#DDDDDD')

      Scroll() {
        Column() {
          if (this.tab === 0) {
            this.operationTab()
          } else if (this.tab === 1) {
            this.configTab()
          } else {
            this.cacheTab()
          }
        }
        .width('100%')
      }
      .layoutWeight(1)
      .width('100%')
    }
    .width('100%')
    .layoutWeight(1)
  }
}
