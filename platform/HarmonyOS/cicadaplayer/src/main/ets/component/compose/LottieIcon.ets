/*
 * Lottie 图标（播放/暂停键、进度条上的 dot 徽标）—— ArkTS 版。
 *
 * 【走的是哪条路，为什么】
 * Compose 用 lottie-compose 播 `app/src/main/assets/lottie-icon/*.json`。
 * 鸿蒙这边先试 `ohpm install @ohos/lottie`，但本次会话里 shell 完全不可用
 * （pwsh 一启动就被文件沙箱拒掉），装不上 ⇒ 按约定的第二条路走：
 * **不加 @ohos/lottie 依赖**，改成用 ArkUI `Shape` / `Path` 按 JSON 里的矢量数据精确复刻。
 * 三个 JSON 仍然原样随 HAR 交付（resources/rawfile/lottie-icon/），只是运行时不加载它们。
 *
 * 【复刻的数据来源与对应关系】（全部取自那两个 JSON 的关键帧）
 *   play-to-pause-animation.json（"播放->暂停"，fr 60 / op 60 ⇒ 1000ms）
 *     末帧 = 播放三角（暂停态该显示的样子）；三角路径就是 JSON layer 1 的
 *     v/o/i 三段贝塞尔，按 28x28 画布（JSON 的 w/h）展开，层位置末帧 [14,14] 已并进坐标。
 *   pause-to-play-animation.json（"暂停->播放"，fr 60 / op 60 ⇒ 1000ms）
 *     末帧 = 两条竖线（播放态该显示的样子）；两条竖线是 JSON 里那个圆角长条路径
 *     （宽 5.024、高 19.99、圆角 2.5），末帧中心在 x = 8.504 / 19.316、y = 14。
 *   槽位映射与 Compose/Qt 一致：**正在播放 → 槽 0（pause-to-play）末帧 = 两条竖线；
 *   暂停 → 槽 1（play-to-pause）末帧 = 播放三角**；挂载时按真实播放状态停帧
 *   （Compose 的 restProgressOverride）。
 *   Thumb-animation.json（fr 100 / op 300）18x18 画布，静止在第 0 帧当静态徽标：
 *     白色圆角框（x 4..18、y 4.41..15.41、圆角 2.5）+ #333 描边 1.5
 *     + 框内两条 #333 竖条（1.8x4 / 1.75x4）+ 上沿两根 #333 小斜条（按 JSON 的
 *     scale 136.67%/115.06%、rotate 39.13° 与 122.63%/114.39%、rotate -42.32° 换算出的四边形）。
 *     整层还被 asset "0" 的 18x18 alpha 蒙版裁切、并整体 +1 平移（那一步已并进坐标）。
 *
 * 【与 Compose 的差异】
 *   · Compose 的形变由 lottie 逐帧采样，这里用一段 1000ms 线性 animateTo 复刻
 *     "同一段形变 + 同一个末帧"。JSON 里那点弹性（竖线 scale 110% → 90% → 100%）没复刻；
 *   · `LottieIcon` 这个"通用 lottie 播放器"组件没有对应实现（没有运行时），
 *     只保留它的两个使用者（PlayPauseIconButton / DanmakuDotThumb）；
 *   · Thumb 的动画（两条竖条左右滑动）在 Compose 里也没被用起来（running = false），
 *     所以这里只画静止帧。
 */

import { PlayerSize } from '../theme/PlayerTheme';

/** 随包交付的 lottie 资源（与 Android 同文件名，内容一字不改）。 */
export class PlayerLottieAsset {
  static readonly PLAY_TO_PAUSE: string = 'lottie-icon/play-to-pause-animation.json';
  static readonly PAUSE_TO_PLAY: string = 'lottie-icon/pause-to-play-animation.json';
  static readonly THUMB: string = 'lottie-icon/Thumb-animation.json';
}

/** 槽位映射（Qt / Compose 同款）：正在播放用槽 0，暂停用槽 1。 */
export const LOTTIE_SLOT_PLAYING: number = 0;
export const LOTTIE_SLOT_PAUSED: number = 1;

/** 画布尺寸（JSON 里的 w/h）。 */
const PLAY_PAUSE_CANVAS: number = PlayerSize.lottiePlayPauseCanvas;
const THUMB_CANVAS: number = PlayerSize.lottieThumbCanvas;

/** 播放三角：play-to-pause-animation.json layer 1 的路径，末帧位置 [14,14] 已并入坐标。 */
const PLAY_PATH_COMMANDS: string =
  'M6.969 3.125C6.969 3.125 5.672 2.75 4.578 3.531C3.89 4.093 4 6.008 4 6.008' +
  'C4 6.008 4 22.016 4 22.016C4 22.016 3.875 24.242 5 24.992C6.125 25.742 9 24.031 9 24.031' +
  'C9 24.031 21.969 15.875 21.969 15.875C21.969 15.875 23 15.062 23 14' +
  'C23 12.938 21.969 12.062 21.969 12.062C21.969 12.062 6.969 3.125 6.969 3.125Z';

/**
 * 两条竖线：pause-to-play-animation.json 里那个圆角长条路径的末帧位置。
 * 长条等价于"宽 5.024、高 19.99、圆角 2.5"的圆角矩形，中心分别在 x 8.504 / 19.316、y 14。
 */
const PAUSE_PATH_COMMANDS: string =
  'M8.492 4.005H8.516A2.5 2.5 0 0 1 11.016 6.505V21.495A2.5 2.5 0 0 1 8.516 23.995' +
  'H8.492A2.5 2.5 0 0 1 5.992 21.495V6.505A2.5 2.5 0 0 1 8.492 4.005Z' +
  'M19.304 4.005H19.328A2.5 2.5 0 0 1 21.828 6.505V21.495A2.5 2.5 0 0 1 19.328 23.995' +
  'H19.304A2.5 2.5 0 0 1 16.804 21.495V6.505A2.5 2.5 0 0 1 19.304 4.005Z';

/** Thumb：上沿两根小斜条（JSON 里的 scale/rotate 已换算成四边形坐标）。 */
const THUMB_TILT_COMMANDS: string =
  'M13.6026 5.1618L12.0124 3.8678L14.9174 0.2982L16.5076 1.5922Z' +
  'M10.0001 3.8227L8.6399 5.0609L5.5599 1.6773L6.9201 0.4391Z';

/** Thumb：白色圆角框（JSON 里白色填充层 + 描边层合并成"填充 + 描边"）。 */
const THUMB_BOX_COMMANDS: string =
  'M6.5 4.41H15.5A2.5 2.5 0 0 1 18 6.91V12.91A2.5 2.5 0 0 1 15.5 15.41' +
  'H6.5A2.5 2.5 0 0 1 4 12.91V6.91A2.5 2.5 0 0 1 6.5 4.41Z';

/** Thumb：框内两条 #333 竖条（1.8x4 与 1.75x4，圆角为半宽）。 */
const THUMB_BARS_COMMANDS: string =
  'M13.48 7.87H13.52A0.88 0.88 0 0 1 14.4 8.75V11.11A0.88 0.88 0 0 1 13.52 11.87' +
  'H13.48A0.88 0.88 0 0 1 12.6 11.11V8.75A0.88 0.88 0 0 1 13.48 7.87Z' +
  'M8.385 7.87A0.875 0.875 0 0 1 9.26 8.745V11.115A0.875 0.875 0 0 1 8.385 11.87' +
  'A0.875 0.875 0 0 1 7.51 11.115V8.745A0.875 0.875 0 0 1 8.385 7.87Z';

/** 图标本体（白色，和两个 JSON 里的填充色一致）。 */
const LOTTIE_ICON_FILL: string = '#FFFFFFFF';
/** Thumb 里描边/竖条的深色（JSON 的 [0.2,0.2,0.2]）。 */
const THUMB_DARK: string = '#FF333333';

/**
 * 播放 / 暂停键（Compose：PlayPauseIconButton）。
 *
 * 交互与 Compose 一致：点一下回调 onTap，图标形变由 isPlaying 驱动；
 * **挂载时按真实播放状态停帧**（正在播放就显示两条竖线，暂停就显示播放三角），
 * 不会出现"正在播放却显示播放三角"。
 */
@Component
export struct PlayPauseIconButton {
  @Prop @Watch('onPlayingChanged') isPlaying: boolean = false;
  /** Compose：size = 40.dp（名字不能叫 size —— 与 ArkUI 的通用属性方法重名） */
  iconSize: number = PlayerSize.lottiePlayPauseSize;
  onTap: () => void = () => {
  };

  /* 静止帧：正在播放 → 两条竖线（槽 0 末帧）；暂停 → 播放三角（槽 1 末帧） */
  @State triangleOpacity: number = 1;
  @State barsOpacity: number = 0;
  @State barsScale: number = 0;

  aboutToAppear(): void {
    this.applyRestFrame(this.isPlaying);
  }

  private applyRestFrame(playing: boolean): void {
    this.triangleOpacity = playing ? 0 : 1;
    this.barsOpacity = playing ? 1 : 0;
    this.barsScale = playing ? 1 : 0;
  }

  /** isPlaying 变了才播一遍形变（与 JSON 的 1000ms 线性时长对齐）。 */
  private onPlayingChanged(): void {
    const playing: boolean = this.isPlaying;
    this.getUIContext().animateTo({ duration: PlayerSize.lottiePlayPauseDurationMs, curve: Curve.Linear }, () => {
      this.applyRestFrame(playing);
    });
  }

  build() {
    Stack() {
      /* 播放三角 */
      Shape() {
        Path()
          .commands(PLAY_PATH_COMMANDS)
          .fill(LOTTIE_ICON_FILL)
      }
      .viewPort({ x: 0, y: 0, width: PLAY_PAUSE_CANVAS, height: PLAY_PAUSE_CANVAS })
      .width(this.iconSize)
      .height(this.iconSize)
      .opacity(this.triangleOpacity)

      /* 两条竖线 */
      Shape() {
        Path()
          .commands(PAUSE_PATH_COMMANDS)
          .fill(LOTTIE_ICON_FILL)
      }
      .viewPort({ x: 0, y: 0, width: PLAY_PAUSE_CANVAS, height: PLAY_PAUSE_CANVAS })
      .width(this.iconSize)
      .height(this.iconSize)
      .opacity(this.barsOpacity)
      .scale({ x: this.barsScale, y: this.barsScale })
    }
    .width(this.iconSize)
    .height(this.iconSize)
    .onClick(() => {
      this.onTap();
    })
    .accessibilityText(this.isPlaying ? '暂停' : '播放')
  }
}

/**
 * 进度条上的 dot 徽标（Compose：DanmakuDotThumb(size = 20.dp)，Thumb 动画静止在第 0 帧）。
 */
@Component
export struct DanmakuDotThumb {
  /** 名字不能叫 size —— 与 ArkUI 的通用属性方法重名 */
  dotSize: number = PlayerSize.lottieThumbSize;

  build() {
    Shape() {
      /* 上沿两根小斜条（最底层） */
      Path()
        .commands(THUMB_TILT_COMMANDS)
        .fill(THUMB_DARK)
      /* 白色圆角框 + #333 描边 */
      Path()
        .commands(THUMB_BOX_COMMANDS)
        .fill('#FFFFFFFF')
        .stroke(THUMB_DARK)
        .strokeWidth(1.5)
      /* 框内两条竖条（最上层） */
      Path()
        .commands(THUMB_BARS_COMMANDS)
        .fill(THUMB_DARK)
    }
    .viewPort({ x: 0, y: 0, width: THUMB_CANVAS, height: THUMB_CANVAS })
    .width(this.dotSize)
    .height(this.dotSize)
  }
}
