/*
 * Snapshot viewer — HarmonyOS counterpart of the Android demo's
 * SnapShotActivity: shows the captured frame centred and finishes itself after
 * 5000 ms (the Android activity posts a 5 s delayed finish()).
 */

import { router } from '@kit.ArkUI';
import { TitleBar } from '../view/TitleBar';
import { S } from '@cicada/cicadaplayer';

/** router params for this page. */
export interface SnapShotRouteParams {
  path: string;
}

const AUTO_FINISH_MS: number = 5000;

@Entry
@Component
struct SnapShotPage {
  @State imagePath: string = '';
  private timerId: number = -1;

  aboutToAppear(): void {
    const params = router.getParams() as SnapShotRouteParams;
    if (params !== undefined && params !== null && params.path !== undefined) {
      this.imagePath = params.path;
    }
    this.timerId = setTimeout(() => {
      router.back();
    }, AUTO_FINISH_MS);
  }

  aboutToDisappear(): void {
    if (this.timerId >= 0) {
      clearTimeout(this.timerId);
      this.timerId = -1;
    }
  }

  build() {
    Column() {
      TitleBar({
        title: S.titleScreenShot,
        onBack: () => router.back()
      })

      Column() {
        if (this.imagePath.length > 0) {
          Image(`file://${this.imagePath}`)
            .objectFit(ImageFit.Contain)
            .width('100%')
            .layoutWeight(1)
        } else {
          Text(S.snapShotFailure).fontSize(15).fontColor('#666666')
        }
      }
      .layoutWeight(1)
      .width('100%')
      .justifyContent(FlexAlign.Center)
      .backgroundColor(Color.Black)
    }
    .width('100%')
    .height('100%')
  }
}
