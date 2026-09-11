/*
 * Shared title bar — the counterpart of the Android demo's `include view_title`
 * (50dp row: back arrow, centred bold title, right-hand action text).
 */

@Component
export struct TitleBar {
  @Prop title: string = '';
  @Prop rightText: string = '';
  /** When false the back arrow is hidden (used by the launcher page). */
  @Prop showBack: boolean = true;
  onBack: () => void = () => {};
  onRight: () => void = () => {};

  build() {
    Column() {
      Row() {
        if (this.showBack) {
          Text('←')
            .fontSize(22)
            .fontColor('#FFFFFF')
            .width(40)
            .height('100%')
            .textAlign(TextAlign.Center)
            .onClick(() => this.onBack())
        } else {
          Blank().width(40)
        }

        Text(this.title)
          .fontSize(16)
          .fontWeight(FontWeight.Bold)
          .fontColor('#FFFFFF')
          .layoutWeight(1)
          .textAlign(TextAlign.Center)
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })

        Text(this.rightText)
          .fontSize(14)
          .fontColor('#379DF2')
          .width(90)
          .height('100%')
          .textAlign(TextAlign.End)
          .onClick(() => this.onRight())
      }
      .width('100%')
      .height(50)
      .backgroundColor('#1e222d')

      Divider().strokeWidth(1).color('#000000')
    }
    .width('100%')
  }
}
