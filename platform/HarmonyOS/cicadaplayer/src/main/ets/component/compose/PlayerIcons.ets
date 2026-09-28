/*
 * 播放器图标 —— Compose 安卓版用 vector drawable（res/drawable/ic_*.xml），
 * 鸿蒙这边把同一段 pathData 原样搬成 SVG 资源，放在本 HAR 的 resources/rawfile/icons/ 下，
 * 用 `Image($rawfile(...))` 渲染（HAR 会把 rawfile 带给消费者，entry 里不重复放一份）。
 *
 * 图标资源对照（pathData 与 Compose 的 drawable 一字不改）：
 *   ic_arrow_back.svg                 ← Material Icons.AutoMirrored.Filled.ArrowBack（顶部栏 + 左上角返回键）
 *   ic_player_fullscreen.svg          ← res/drawable/ic_player_fullscreen.xml
 *   ic_player_portrait_fullscreen.svg ← res/drawable/ic_player_portrait_fullscreen.xml
 *   ic_danmaku_switch_on.svg          ← res/drawable/ic_danmaku_switch_on.xml
 *   ic_danmaku_switch_off.svg         ← res/drawable/ic_danmaku_switch_off.xml
 *   ic_danmaku_setting.svg            ← res/drawable/ic_danmaku_setting.xml
 *
 * 着色方式：SVG 里的 path 固定 fill="#FFFFFF"（与 Android vector 的 fillColor="#FFFFFFFF" 相同），
 * 显示时用 opacity 表达 Compose 的 tint：
 *   PlayerTheme.controlText（#CCFFFFFF，白 80%）→ opacity 0.8
 *   PlayerTheme.controlTextActive（#FFFFFFFF）  → opacity 1
 * 这样既不用依赖 fillColor 对 SVG 的覆盖行为，又能与 Compose 逐项对上。
 */

import { PlayerSize } from '../theme/PlayerTheme';

/** Compose：tint = PlayerTheme.controlText（白 80%）。 */
export const ICON_OPACITY_CONTROL_TEXT: number = 0.8;
/** Compose：tint = PlayerTheme.controlTextActive（纯白）。 */
export const ICON_OPACITY_ACTIVE: number = 1;

/** IconButtonBox 的默认（空）内容，供 @BuilderParam 初始化用。 */
@Builder
function IconButtonEmpty() {
  Row() {
  }
}

/** 一颗图标。尺寸/透明度由调用方给，图标本身是 rawfile 里的 SVG。 */
@Component
export struct PlayerIcon {
  iconPath: Resource = $rawfile('icons/ic_player_fullscreen.svg');
  iconSize: number = PlayerSize.topBarIconSize;
  iconOpacity: number = ICON_OPACITY_CONTROL_TEXT;
  /** 无障碍描述（对应 Compose 的 contentDescription）；空串 = 不设。 */
  label: string = '';

  build() {
    Image(this.iconPath)
      .width(this.iconSize)
      .height(this.iconSize)
      .objectFit(ImageFit.Contain)
      .opacity(this.iconOpacity)
      .draggable(false)
      .accessibilityText(this.label)
  }
}

/**
 * 圆形热区按钮（Compose：`Box(Modifier.size(size).clip(CircleShape).clickable(onClick))`）。
 * 按压反馈就是这个可点区域的默认水波纹；手机端没有 hover 状态。
 */
@Component
export struct IconButtonBox {
  @BuilderParam content: () => void = IconButtonEmpty;
  boxSize: number = PlayerSize.iconButtonSize;
  onTap: () => void = () => {
  };

  build() {
    Column() {
      this.content()
    }
    .width(this.boxSize)
    .height(this.boxSize)
    .borderRadius(this.boxSize / 2)
    .justifyContent(FlexAlign.Center)
    .alignItems(HorizontalAlign.Center)
    .onClick(() => {
      this.onTap();
    })
  }
}

/**
 * 36dp 热区的图标按钮（Compose：PlayerIconButton(buttonSize = 36.dp, iconSize = 26.dp)，
 * tint = PlayerTheme.controlText）。
 */
@Component
export struct PlayerIconButton {
  iconRes: Resource = $rawfile('icons/ic_player_fullscreen.svg');
  description: string = '';
  onTap: () => void = () => {
  };
  buttonSize: number = PlayerSize.iconButtonSize;
  iconSize: number = PlayerSize.iconButtonIconSize;

  build() {
    IconButtonBox({ boxSize: this.buttonSize, onTap: this.onTap }) {
      PlayerIcon({
        iconPath: this.iconRes,
        iconSize: this.iconSize,
        iconOpacity: ICON_OPACITY_CONTROL_TEXT,
        label: this.description
      })
    }
  }
}

/** 顶部栏 / 左上角返回键用的箭头图标（tint = controlTextActive，纯白）。 */
@Component
export struct PlayerBackIcon {
  iconSize: number = PlayerSize.topBarIconSize;
  label: string = '返回';

  build() {
    PlayerIcon({
      iconPath: $rawfile('icons/ic_arrow_back.svg'),
      iconSize: this.iconSize,
      iconOpacity: ICON_OPACITY_ACTIVE,
      label: this.label
    })
  }
}
