/*
 * Player overlays — HarmonyOS counterpart of the Android demo's TipsView with
 * its four children (ErrorView / LoadingView / NetChangeView / ReplayView).
 * Only one of them is visible at a time: Android's every show* call starts with
 * hideAll().
 *
 * Text taken from strings.xml:
 *   LoadingView    : cicada_loading + percent + "%"      ("Loading... 0%")
 *   ErrorView      : msg, then cicada_error_code + code + " - " + code
 *   ReplayView     : cicada_replay_msg ("视频播放结束") + cicada_replay
 *   NetChangeView  : cicada_net_state_mobile + cicada_net_state_mobile_yes/no
 */

import { S } from '../util/Strings';

@Component
export struct PlayerTipsOverlay {
  @Prop loading: boolean = false;
  @Prop loadPercent: number = 0;
  @Prop errorVisible: boolean = false;
  @Prop errorCode: number = 0;
  @Prop errorMsg: string = '';
  @Prop replayVisible: boolean = false;
  @Prop netChangeVisible: boolean = false;

  onRetry: () => void = () => {};
  onReplay: () => void = () => {};
  onContinuePlay: () => void = () => {};
  onStopPlay: () => void = () => {};

  @Builder
  outlinedButton(label: string, outlineColor: string, textColor: string, action: () => void) {
    Text(label)
      .fontSize(13)
      .fontColor(textColor)
      .textAlign(TextAlign.Center)
      .width(82)
      .height(30)
      .borderRadius(4)
      .borderWidth(1)
      .borderColor(outlineColor)
      .onClick(action)
  }

  build() {
    Stack() {
      // ---- loading (cicada_dialog_loading.xml) ----
      if (this.loading && !this.errorVisible && !this.replayVisible) {
        Column() {
          LoadingProgress()
            .width(28)
            .height(28)
            .color('#379DF2')
          Text(`${S.loading} ${this.loadPercent}%`)
            .fontSize(14)
            .fontColor(Color.White)
            .margin({ top: 10 })
        }
        .justifyContent(FlexAlign.Center)
        .backgroundColor('#99000000')
      }

      // ---- error (cicada_dialog_error.xml) ----
      if (this.errorVisible) {
        Column() {
          Text(this.errorMsg)
            .fontSize(15)
            .fontColor(Color.White)
            .textAlign(TextAlign.Center)
            .maxLines(3)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
          // Android prints the code twice ("Error Code:2003 - 2003").
          Text(`${S.errorCode}${this.errorCode} - ${this.errorCode}`)
            .fontSize(12)
            .fontColor('#cccccc')
            .margin({ top: 6 })
          this.outlinedButton(S.retry, '#379DF2', '#379DF2', () => this.onRetry())
        }
        .justifyContent(FlexAlign.Center)
        .padding(10)
        .backgroundColor('#b2000000')
      }

      // ---- completion / replay (cicada_dialog_replay.xml) ----
      if (this.replayVisible) {
        Column() {
          Text(S.replayMsg)
            .fontSize(15)
            .fontColor(Color.White)
          this.outlinedButton(S.replay, '#379DF2', '#379DF2', () => this.onReplay())
        }
        .justifyContent(FlexAlign.Center)
        .padding(10)
        .backgroundColor('#b2000000')
      }

      // ---- mobile-network change (cicada_dialog_netchange.xml) ----
      if (this.netChangeVisible) {
        Column() {
          Text(S.netStateMobile)
            .fontSize(15)
            .fontColor(Color.White)
            .textAlign(TextAlign.Center)
          Row({ space: 30 }) {
            this.outlinedButton(S.netStateMobileYes, '#FFFFFF', '#FFFFFF', () => this.onContinuePlay())
            this.outlinedButton(S.netStateMobileNo, '#379DF2', '#379DF2', () => this.onStopPlay())
          }
          .margin({ top: 12 })
        }
        .justifyContent(FlexAlign.Center)
        .padding(10)
        .backgroundColor('#b2000000')
      }
    }
    .width('100%')
    .height('100%')
  }
}
