/*
 * 播放器浮层面板 —— ArkTS 版，来源是 Compose 安卓版的
 *   platform/Android/ComposePlayer/app/src/main/java/com/cicada/player/compose/components/video/PlayerPanels.kt
 *
 * 逐个对应：PlayerPanelShell / PanelTitle / PanelRow / PlayerQualityPanel / PlayerSpeedPanel /
 * PlayerSubtitlePanel / PlayerDanmakuPanel / PlayerVolumePanel / VerticalVolumeSlider / PlayerInfoPanel。
 * 面板规格：底 #212121 @90%、圆角 4、上下留白 9dp、每项左右留白 20dp、选中项用主题色；
 * 高度上限 220dp + 可滚动（density 放大时面板太高会把控制条顶出画面）。
 *
 * 与 Compose 的差异（逐条）：
 *   · **数据来源改成"现读"**：Compose 的面板直接吃 `List<QualityOption>` / `PlayerStats` 这类
 *     类实例（Compose 有重组机制）。ArkUI 里"无装饰器的成员变量"只在子组件创建时赋值，
 *     父组件后续更新不会同步过来，所以这里让面板自己拿**内核事实 + 父组件传进来的原始读数**
 *     现算：清晰度/字幕面板收 `streams`（@Prop，纯数据接口数组）自己调 buildQualities /
 *     buildSubtitles；信息面板收播放器句柄（普通成员，身份稳定）+ 位置/缓冲/网速（@Prop），
 *     自己调 `PlayerStats.from(...)`。这样面板永远显示最新值，且不依赖类实例的跨组件传递；
 *   · 面板的 `width` 成员改名 `panelWidth`（ArkUI 的通用属性方法已占用 `width`）；
 *     弹幕面板的 `enabled` / `opacity` 改名 `danmakuEnabled` / `panelOpacity`（同上：`opacity` 重名）；
 *   · 镜像三态用 ArkTS enum `MirrorUiMode` + 行表；色觉辅助在 Kotlin 里是带数组成员的 enum，
 *     ArkTS 的 enum 不能带数组，所以拆成 `ColorVisionKind` + `COLOR_VISION_ROWS`（数值一字不改）；
 *   · 后台播放开关：Compose 的 Material3 `Switch` → ArkUI `Toggle(ToggleType.Switch)`；
 *   · 面板自己吃掉点击（点面板空白处不能穿到画面区把视频点暂停）用 `onClick` 空实现。
 */

import { CicadaPlayer } from '../../api/CicadaPlayer';
import { StreamInfo } from '../../api/CicadaTypes';
import { PlayerSize, PlayerTheme } from '../theme/PlayerTheme';
import { PlayerStats, QualityOption, SubtitleOption, buildQualities, buildSubtitles } from './PlayerStats';

/** 倍速档位（Compose：`listOf(0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f)`）。 */
const SPEED_OPTIONS: number[] = [0.5, 0.75, 1, 1.25, 1.5, 2];
/** 弹幕面板的三组档位（与 Compose 的 listOf 一一对应）。 */
const DANMAKU_OPACITIES: number[] = [100, 75, 50, 25];
const DANMAKU_FONT_SCALES: number[] = [50, 75, 100, 125, 150];
const DANMAKU_DENSITIES: number[] = [4, 6, 8, 12];

/**
 * 镜像画面（**内核 setMirrorMode**，不是 App 层翻转）。三态：关 / 水平 / 垂直（只镜像，不旋转）。
 * 与内核枚举 MirrorMode 分开命名，下发时在播放器里显式映射一次。
 */
export enum MirrorUiMode {
  None = 0,
  Horizontal = 1,
  Vertical = 2
}

/** 「镜像画面」那三行的文案与顺序（与 MirrorUiMode 一一对应）。 */
export class MirrorRow {
  mode: MirrorUiMode = MirrorUiMode.None;
  label: string = '';

  constructor(mode: MirrorUiMode, label: string) {
    this.mode = mode;
    this.label = label;
  }
}

export const MIRROR_ROWS: MirrorRow[] = [
  new MirrorRow(MirrorUiMode.None, '关闭'),
  new MirrorRow(MirrorUiMode.Horizontal, '水平镜像'),
  new MirrorRow(MirrorUiMode.Vertical, '垂直镜像')
];

/**
 * 色觉辅助（红绿色盲等）4 档：关闭 / 红·绿 / 绿·红 / 蓝·黄。
 * 矩阵是 Machado, Oliveira & Fernandes (2009) 的色盲模拟矩阵，强度 1.0，**行主序 3×3**，
 * 数值照 Compose 的三组原样抄写，未做任何调整。
 * 【关闭】用**单位矩阵**表示（不用 null）：与内核默认值一致，少一个分支。
 */
export enum ColorVisionKind {
  None = 0,
  Protan = 1,
  Deutan = 2,
  Tritan = 3
}

/** 色觉辅助的一档：档位 + 文案 + 3x3 行主序矩阵。 */
export class ColorVisionRow {
  kind: ColorVisionKind = ColorVisionKind.None;
  label: string = '';
  matrix: number[] = [];

  constructor(kind: ColorVisionKind, label: string, matrix: number[]) {
    this.kind = kind;
    this.label = label;
    this.matrix = matrix;
  }
}

export const COLOR_VISION_ROWS: ColorVisionRow[] = [
  new ColorVisionRow(ColorVisionKind.None, '关闭', [
    1, 0, 0,
    0, 1, 0,
    0, 0, 1
  ]),
  new ColorVisionRow(ColorVisionKind.Protan, '红/绿（红色盲）', [
    0.152286, 1.052583, -0.204868,
    0.114503, 0.786281, 0.099216,
    -0.003882, -0.048116, 1.051998
  ]),
  new ColorVisionRow(ColorVisionKind.Deutan, '绿/红（绿色盲）', [
    0.367322, 0.860646, -0.227968,
    0.280085, 0.672501, 0.047413,
    -0.011820, 0.042940, 0.968881
  ]),
  new ColorVisionRow(ColorVisionKind.Tritan, '蓝/黄（蓝色盲）', [
    1.255528, -0.076749, -0.178779,
    -0.078411, 0.930809, 0.147602,
    0.004733, 0.691367, 0.303900
  ])
];

/** 取某一档的矩阵（内核 setColorMatrix 的入参）。 */
export function colorVisionMatrix(kind: ColorVisionKind): number[] {
  for (let i = 0; i < COLOR_VISION_ROWS.length; i++) {
    if (COLOR_VISION_ROWS[i].kind === kind) {
      return COLOR_VISION_ROWS[i].matrix;
    }
  }
  return COLOR_VISION_ROWS[0].matrix;
}

/** PlayerPanelShell 的默认（空）内容，供 @BuilderParam 初始化用。 */
@Builder
function PanelEmpty() {
  Row() {
  }
}

/**
 * 浮层面板外壳。宽度默认 160dp；高度上限 220dp + 垂直可滚动；
 * 面板自己吃掉点击，点空白处不会穿到画面区。
 */
@Component
export struct PlayerPanelShell {
  @BuilderParam content: () => void = PanelEmpty;
  /** Compose：width = 160.dp（名字不能叫 width —— 与 ArkUI 的通用属性方法重名） */
  panelWidth: number = PlayerSize.panelWidth;

  build() {
    Scroll() {
      Column() {
        this.content()
      }
      .width('100%')
      .padding({ top: PlayerSize.panelPaddingV, bottom: PlayerSize.panelPaddingV })
    }
    .width(this.panelWidth)
    .constraintSize({ maxHeight: PlayerSize.panelMaxHeight })
    .borderRadius(PlayerSize.panelRadius)
    .backgroundColor(PlayerTheme.panelBg)
    .scrollBar(BarState.Off)
    .onClick(() => {
    })
  }
}

/** 面板里的小标题（10sp、次要文字色）。 */
@Component
struct PanelTitle {
  @Prop text: string = '';

  build() {
    Text(this.text)
      .fontSize(PlayerSize.panelTitleFontSize)
      .fontColor(PlayerTheme.panelSubText)
      .padding({
        left: PlayerSize.panelTitlePaddingH,
        right: PlayerSize.panelTitlePaddingH,
        top: PlayerSize.panelTitlePaddingV,
        bottom: PlayerSize.panelTitlePaddingV
      })
  }
}

/**
 * 一行面板条目（所有面板共用）。
 *
 * `badge` 是行尾那个**小圆角徽标**的文本（目前只有清晰度面板用它显示编码）：
 *   · 非空 → 在文字右边的行尾画一个小标签（半透明白底 + 次要文字色）；
 *   · 空串 → **整个徽标不渲染**，这一行的排版和没有徽标时完全一样。
 */
@Component
export struct PanelRow {
  @Prop text: string = '';
  @Prop selected: boolean = false;
  @Prop badge: string = '';
  onTap: () => void = () => {
  };

  build() {
    Row() {
      Text(this.text)
        .fontSize(PlayerSize.panelRowFontSize)
        .fontColor(this.selected ? PlayerTheme.accent : PlayerTheme.panelText)
        .fontWeight(this.selected ? FontWeight.Medium : FontWeight.Normal)
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })

      if (this.badge.length > 0) {
        Row() {
          Text(this.badge)
            .fontSize(PlayerSize.panelBadgeFontSize)
            .fontColor(PlayerTheme.panelSubText)
            .maxLines(1)
        }
        .margin({ left: PlayerSize.panelBadgeGap })
        .borderRadius(PlayerSize.panelBadgeRadius)
        .backgroundColor(PlayerSize.panelBadgeBg)
        .padding({
          left: PlayerSize.panelBadgePaddingH,
          right: PlayerSize.panelBadgePaddingH,
          top: PlayerSize.panelBadgePaddingV,
          bottom: PlayerSize.panelBadgePaddingV
        })
      }
    }
    .width('100%')
    .alignItems(VerticalAlign.Center)
    .padding({
      left: PlayerSize.panelRowPaddingH,
      right: PlayerSize.panelRowPaddingH,
      top: PlayerSize.panelRowPaddingV,
      bottom: PlayerSize.panelRowPaddingV
    })
    .onClick(() => {
      this.onTap();
    })
  }
}

/** 面板里的"没有数据"那一行（字号 12sp、次要文字色）。 */
@Component
struct PanelEmptyText {
  @Prop text: string = '';

  build() {
    Text(this.text)
      .fontSize(PlayerSize.panelEmptyFontSize)
      .fontColor(PlayerTheme.panelSubText)
      .padding({
        left: PlayerSize.panelRowPaddingH,
        right: PlayerSize.panelRowPaddingH,
        top: PlayerSize.panelRowPaddingV,
        bottom: PlayerSize.panelRowPaddingV
      })
  }
}

/**
 * 清晰度（数据来自内核 MediaInfo 的视频轨；面板自己现算，见文件头的差异说明）。
 * 行的身份 = 内核 streamIndex，点击时原样回传。
 */
@Component
export struct PlayerQualityPanel {
  @Prop streams: StreamInfo[] = [];
  @Prop currentIndex: number = -1;
  @Prop autoQuality: boolean = true;
  onPick: (index: number) => void = () => {
  };

  build() {
    PlayerPanelShell({ panelWidth: PlayerSize.panelWidth }) {
      PanelTitle({ text: '清晰度' })
      if (this.options().length === 0) {
        PanelEmptyText({ text: '当前片源没有清晰度信息' })
      } else {
        ForEach(this.options(), (item: QualityOption) => {
          PanelRow({
            text: item.label,
            selected: item.selected,
            badge: item.codec,
            onTap: () => {
              this.onPick(item.index);
            }
          })
        }, (item: QualityOption) => `${item.index}-${item.label}-${item.selected}`)
      }
    }
  }

  private options(): QualityOption[] {
    return buildQualities(this.streams, this.currentIndex, this.autoQuality);
  }
}

/** 倍速。 */
@Component
export struct PlayerSpeedPanel {
  @Prop current: number = 1;
  onPick: (speed: number) => void = () => {
  };

  build() {
    PlayerPanelShell({ panelWidth: PlayerSize.panelWidth }) {
      PanelTitle({ text: '倍速播放' })
      ForEach(SPEED_OPTIONS, (speed: number) => {
        PanelRow({
          text: speed === 1 ? '正常' : `${speed}x`,
          selected: Math.abs(this.current - speed) < 0.01,
          badge: '',
          onTap: () => {
            this.onPick(speed);
          }
        })
      }, (speed: number) => `${speed}`)
    }
  }
}

/** 字幕（数据来自内核 MediaInfo 的字幕轨；第一行「关闭字幕」= AUTO_SELECT_INDEX）。 */
@Component
export struct PlayerSubtitlePanel {
  @Prop streams: StreamInfo[] = [];
  @Prop currentIndex: number = -1;
  @Prop subtitlesEnabled: boolean = false;
  onPick: (index: number) => void = () => {
  };

  build() {
    PlayerPanelShell({ panelWidth: PlayerSize.panelWidth }) {
      PanelTitle({ text: '字幕' })
      if (this.tracks().length === 0) {
        PanelEmptyText({ text: '当前片源没有字幕轨' })
      } else {
        ForEach(this.tracks(), (item: SubtitleOption) => {
          PanelRow({
            text: item.label,
            selected: item.selected,
            badge: '',
            onTap: () => {
              this.onPick(item.index);
            }
          })
        }, (item: SubtitleOption) => `${item.index}-${item.label}-${item.selected}`)
      }
    }
  }

  private tracks(): SubtitleOption[] {
    return buildSubtitles(this.streams, this.currentIndex, this.subtitlesEnabled);
  }
}

/** 弹幕设置（开关 / 不透明度 / 字号缩放 / 同屏密度 / 显示区域）。 */
@Component
export struct PlayerDanmakuPanel {
  /** 名字不能叫 enabled —— 与 ArkUI 的通用属性方法重名 */
  @Prop danmakuEnabled: boolean = true;
  /** 名字不能叫 opacity —— 同上 */
  @Prop panelOpacity: number = 100;
  @Prop fontScale: number = 100;
  @Prop density: number = 8;
  @Prop showScroll: boolean = true;
  @Prop showTop: boolean = true;
  @Prop showBottom: boolean = true;
  onToggleEnabled: () => void = () => {
  };
  onOpacity: (value: number) => void = () => {
  };
  onFontScale: (value: number) => void = () => {
  };
  onDensity: (value: number) => void = () => {
  };
  onToggleScroll: () => void = () => {
  };
  onToggleTop: () => void = () => {
  };
  onToggleBottom: () => void = () => {
  };

  build() {
    PlayerPanelShell({ panelWidth: PlayerSize.panelDanmakuWidth }) {
      PanelTitle({ text: '弹幕设置' })
      PanelRow({
        text: '显示弹幕', selected: this.danmakuEnabled, badge: '', onTap: () => {
          this.onToggleEnabled();
        }
      })

      PanelTitle({ text: '不透明度' })
      ForEach(DANMAKU_OPACITIES, (value: number) => {
        PanelRow({
          text: `${value}%`, selected: this.panelOpacity === value, badge: '', onTap: () => {
            this.onOpacity(value);
          }
        })
      }, (value: number) => `o${value}`)

      PanelTitle({ text: '字号缩放' })
      ForEach(DANMAKU_FONT_SCALES, (value: number) => {
        PanelRow({
          text: `${value}%`, selected: this.fontScale === value, badge: '', onTap: () => {
            this.onFontScale(value);
          }
        })
      }, (value: number) => `f${value}`)

      PanelTitle({ text: '同屏密度（最多几行）' })
      ForEach(DANMAKU_DENSITIES, (value: number) => {
        PanelRow({
          text: `${value} 行`, selected: this.density === value, badge: '', onTap: () => {
            this.onDensity(value);
          }
        })
      }, (value: number) => `d${value}`)

      PanelTitle({ text: '显示区域' })
      PanelRow({
        text: '滚动弹幕', selected: this.showScroll, badge: '', onTap: () => {
          this.onToggleScroll();
        }
      })
      PanelRow({
        text: '顶部弹幕', selected: this.showTop, badge: '', onTap: () => {
          this.onToggleTop();
        }
      })
      PanelRow({
        text: '底部弹幕', selected: this.showBottom, badge: '', onTap: () => {
          this.onToggleBottom();
        }
      })
    }
  }
}

/** 音量（竖向滑条：轨道 2dp、已填充主题色、圆点 12dp）。 */
@Component
export struct PlayerVolumePanel {
  @Prop volume: number = 0.9;
  @Prop muted: boolean = false;
  onVolume: (volume: number) => void = () => {
  };
  onToggleMute: () => void = () => {
  };

  build() {
    PlayerPanelShell({ panelWidth: PlayerSize.panelVolumeWidth }) {
      PanelTitle({ text: '音量' })
      Row() {
        Text(this.muted ? '静音' : `${Math.floor(this.volume * 100)}`)
          .fontSize(PlayerSize.volumeValueFontSize)
          .fontColor(PlayerSize.volumeValueColor)
          .fontWeight(FontWeight.Bold)
      }
      .width('100%')
      .justifyContent(FlexAlign.Center)
      .padding({ top: PlayerSize.volumeValueRowPaddingV, bottom: PlayerSize.volumeValueRowPaddingV })

      Row() {
        VerticalVolumeSlider({
          volume: this.muted ? 0 : this.volume,
          onVolume: this.onVolume
        })
      }
      .width('100%')
      .height(PlayerSize.volumeBoxHeight)
      .justifyContent(FlexAlign.Center)

      PanelRow({
        text: this.muted ? '取消静音' : '静音',
        selected: this.muted,
        badge: '',
        onTap: () => {
          this.onToggleMute();
        }
      })
    }
  }
}

/** 竖向音量滑条（按下即跳、拖动跟随；y 越靠上音量越大）。 */
@Component
struct VerticalVolumeSlider {
  @Prop volume: number = 0.9;
  onVolume: (volume: number) => void = () => {
  };
  @State trackHeight: number = PlayerSize.volumeSliderHeight;

  private report(y: number): void {
    const ratio: number = 1 - (y / this.trackHeight);
    const clamped: number = ratio < 0 ? 0 : (ratio > 1 ? 1 : ratio);
    this.onVolume(clamped);
  }

  private clampVolume(): number {
    if (this.volume < 0) {
      return 0;
    }
    return this.volume > 1 ? 1 : this.volume;
  }

  build() {
    Stack({ alignContent: Alignment.Bottom }) {
      Row() {
      }
      .width(PlayerSize.volumeTrackWidth)
      .height(this.trackHeight)
      .backgroundColor(PlayerSize.volumeTrackBg)

      Row() {
      }
      .width(PlayerSize.volumeTrackWidth)
      .height(this.trackHeight * this.clampVolume())
      .backgroundColor(PlayerTheme.accent)

      Row() {
      }
      .width(PlayerSize.volumeThumbSize)
      .height(PlayerSize.volumeThumbSize)
      .borderRadius(PlayerSize.volumeThumbSize / 2)
      .backgroundColor(PlayerTheme.accent)
      .offset({ x: 0, y: -(this.trackHeight * this.clampVolume() - PlayerSize.volumeThumbSize / 2) })
    }
    .width(PlayerSize.volumeSliderWidth)
    .height(this.trackHeight)
    .onAreaChange((oldValue: Area, newValue: Area) => {
      this.trackHeight = Number(newValue.height);
    })
    .onTouch((event: TouchEvent) => {
      const touches: TouchObject[] = event.touches;
      if (touches.length === 0) {
        return;
      }
      if (event.type === TouchType.Down || event.type === TouchType.Move) {
        this.report(touches[0].y);
      }
    })
  }
}

/**
 * 视频信息（真实读数）+ 后台播放开关 + **画面设置（镜像 / 色觉 / 截图）**。
 *
 * 统计读数**在面板内部现算**（Compose 是同一种做法：重组时读 controller 的实时状态）：
 * 播放器句柄是普通成员（身份稳定），位置/缓冲/网速由父组件用 @Prop 送进来驱动刷新。
 */
@Component
export struct PlayerInfoPanel {
  /** 播放器句柄（普通成员传递；身份稳定，不需要刷新同步） */
  player: CicadaPlayer = new CicadaPlayer();
  @Prop positionMs: number = 0;
  @Prop durationMs: number = 0;
  @Prop bufferedMs: number = 0;
  @Prop downloadSpeedBps: number = 0;
  @Prop decoderNotice: string = '';
  @Prop backgroundPlayEnabled: boolean = true;
  @Prop mirror: number = MirrorUiMode.None;
  @Prop colorVision: number = ColorVisionKind.None;
  @Prop captureInFlight: boolean = false;
  onToggleBackgroundPlay: () => void = () => {
  };
  onMirrorChange: (mode: MirrorUiMode) => void = () => {
  };
  onColorVisionChange: (kind: ColorVisionKind) => void = () => {
  };
  onCapture: () => void = () => {
  };

  /** 信息行（key / value）—— 每次刷新都从内核现读一次（与 Compose 同一条口径）。 */
  private infoRows(): InfoRow[] {
    const stats: PlayerStats = PlayerStats.from(
      this.player, this.positionMs, this.durationMs, this.bufferedMs,
      this.downloadSpeedBps, this.decoderNotice
    );
    return [
      new InfoRow('分辨率', stats.resolutionText()),
      new InfoRow('编解码器', stats.codecText()),
      new InfoRow('解码方式', stats.decodeMethodText()),
      new InfoRow('解码器', stats.decoderNameText()),
      new InfoRow('帧率', stats.frameRateText()),
      new InfoRow('视频码率', stats.bitrateText()),
      new InfoRow('丢帧/总帧', stats.droppedText()),
      new InfoRow('缓冲时长', stats.bufferText()),
      new InfoRow('播放位置', stats.positionText()),
      new InfoRow('下载速度（合计）', stats.speedText())
    ];
  }

  build() {
    PlayerPanelShell({ panelWidth: PlayerSize.panelInfoWidth }) {
      PanelTitle({ text: '视频信息' })
      ForEach(this.infoRows(), (row: InfoRow) => {
        Row() {
          Text(row.key)
            .fontSize(PlayerSize.infoRowFontSize)
            .fontColor(PlayerTheme.panelSubText)
          Text(row.value)
            .fontSize(PlayerSize.infoRowFontSize)
            .fontColor(PlayerTheme.panelText)
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
            .padding({ left: PlayerSize.infoValueGap })
        }
        .width('100%')
        .justifyContent(FlexAlign.SpaceBetween)
        .alignItems(VerticalAlign.Center)
        .padding({
          left: PlayerSize.panelRowPaddingH,
          right: PlayerSize.panelRowPaddingH,
          top: PlayerSize.infoRowPaddingV,
          bottom: PlayerSize.infoRowPaddingV
        })
      }, (row: InfoRow) => `${row.key}-${row.value}`)

      /* 后台播放开关（默认打开：切到后台继续播；关掉则切后台自动暂停、回来自动续播） */
      Row() {
        Text('后台播放')
          .fontSize(PlayerSize.infoSwitchRowFontSize)
          .fontColor(PlayerTheme.panelText)
          .layoutWeight(1)
        Toggle({ type: ToggleType.Switch, isOn: this.backgroundPlayEnabled })
          .selectedColor(PlayerTheme.accent)
          .onChange(() => {
            this.onToggleBackgroundPlay();
          })
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)
      .padding({
        left: PlayerSize.panelRowPaddingH,
        right: PlayerSize.panelRowPaddingH,
        top: PlayerSize.infoSwitchRowPaddingV,
        bottom: PlayerSize.infoSwitchRowPaddingV
      })
      .onClick(() => {
        this.onToggleBackgroundPlay();
      })

      /* 镜像画面（走内核 setMirrorMode；三态互斥、当前档用主题色高亮） */
      PanelTitle({ text: '镜像画面' })
      ForEach(MIRROR_ROWS, (row: MirrorRow) => {
        PanelRow({
          text: row.label,
          selected: this.mirror === row.mode,
          badge: '',
          onTap: () => {
            this.onMirrorChange(row.mode);
          }
        })
      }, (row: MirrorRow) => `m${row.mode}`)

      /* 色觉辅助（走内核 GL 着色器，与镜像不是一条路；4 档互斥） */
      PanelTitle({ text: '色觉辅助' })
      ForEach(COLOR_VISION_ROWS, (row: ColorVisionRow) => {
        PanelRow({
          text: row.label,
          selected: this.colorVision === row.kind,
          badge: '',
          onTap: () => {
            this.onColorVisionChange(row.kind);
          }
        })
      }, (row: ColorVisionRow) => `c${row.kind}`)

      /* 截图 → 存相册 */
      PanelTitle({ text: '截图' })
      PanelRow({
        text: this.captureInFlight ? '正在截图…' : '保存当前画面到相册',
        selected: false,
        badge: '',
        onTap: () => {
          this.onCapture();
        }
      })
    }
  }
}

/** 信息面板的一行（key / value）。 */
export class InfoRow {
  key: string = '';
  value: string = '';

  constructor(key: string, value: string) {
    this.key = key;
    this.value = value;
  }
}
