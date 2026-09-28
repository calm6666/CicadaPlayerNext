/*
 * Settings page — HarmonyOS counterpart of the Android demo's SettingActivity.
 *
 * Rows: SDK/player version, hardware-decoder switch, player-kernel selection,
 * device model + blacklist action.
 *
 * Ported differences (documented rather than faked):
 *  - the Android "VersionCode" comes from CicadaPlayerFactory.getSdkVersion();
 *    the C API has no such call, so getPlayerName() is shown instead;
 *  - the ExoPlayer kernel is Android-only and is not ported; its slot here is
 *    taken by AVPlayer, HarmonyOS's own system player, which fills the role of
 *    Android's "MediaPlayer" kernel (see AvPlayerPage);
 *  - CicadaPlayerFactory.addBlackDevice() is a Java-factory API with no C
 *    entry point, so the blacklist action only disables the hardware decoder
 *    locally instead of registering a global device blacklist.
 */

import { common } from '@kit.AbilityKit';
import { deviceInfo } from '@kit.BasicServicesKit';
import { promptAction, router } from '@kit.ArkUI';
import { TitleBar } from '../view/TitleBar';
import { CicadaPlayer, S } from '@cicada/cicadaplayer';
import { PLAYER_NAME_KEY, PLAYER_TYPE_AVPLAYER, Settings } from '../util/Settings';

@Entry
@Component
struct SettingPage {
  private probePlayer: CicadaPlayer = new CicadaPlayer();
  @State playerVersion: string = '';
  @State hardwareDecoder: boolean = true;
  @State deviceModel: string = '';
  @State selectedPlayer: string = 'CicadaPlayer';

  aboutToAppear(): void {
    const context = getContext(this) as common.UIAbilityContext;
    Settings.init(context);
    this.hardwareDecoder = Settings.isHardwareDecoderEnabled();
    this.selectedPlayer = Settings.getPlayerType();
    // Android reads Build.MODEL; the OHOS deviceInfo module has no `model`
    // field, so the closest equivalent is productModel.
    this.deviceModel = deviceInfo.productModel;
    // Prefer the name cached by the player page; only build a throwaway player if
    // none was created yet in this session (CicadaGetPlayerName needs a handle and
    // there is no global version API).
    const cached: string | undefined = AppStorage.get<string>(PLAYER_NAME_KEY);
    if (cached !== undefined && cached !== null && cached.length > 0) {
      this.playerVersion = cached;
    } else if (this.selectedPlayer !== PLAYER_TYPE_AVPLAYER) {
      this.probePlayer.create();
      this.playerVersion = this.probePlayer.getPlayerName();
      this.probePlayer.release();
    } else {
      this.playerVersion = PLAYER_TYPE_AVPLAYER;
    }
  }

  private toast(message: string): void {
    promptAction.showToast({ message: message, duration: 2000 });
  }

  private onHardwareDecoderChange(checked: boolean): void {
    this.hardwareDecoder = checked;
    Settings.setHardwareDecoder(checked);
    this.toast(checked ? S.hardwareOnNextPlay : S.hardwareOffNextPlay);
  }

  private onBlacklist(): void {
    // Android registers the device model with CicadaPlayerFactory.addBlackDevice
    // and permanently forces software decoding. There is no C API for that, so
    // the local switch is turned off instead.
    this.hardwareDecoder = false;
    Settings.setHardwareDecoder(false);
    this.toast(S.success);
  }

  @Builder
  row(label: string, value: string) {
    Row() {
      Text(label).fontSize(14).fontColor('#333333').layoutWeight(1)
      Text(value).fontSize(13).fontColor('#666666').maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
    }
    .width('100%')
    .height(50)
  }

  build() {
    Column() {
      TitleBar({
        title: S.titleSetting,
        onBack: () => router.back()
      })

      Scroll() {
        Column() {
          this.row('版本号 : ', this.playerVersion)

          Row() {
            Text('硬解码').fontSize(14).fontColor('#333333').layoutWeight(1)
            Toggle({ type: ToggleType.Switch, isOn: this.hardwareDecoder })
              .onChange((isOn: boolean) => this.onHardwareDecoderChange(isOn))
          }
          .width('100%')
          .height(50)

          Text('播放器选择').fontSize(14).fontColor('#333333').margin({ top: 10 })
            .width('100%')

          // Only the Cicada kernel exists on HarmonyOS.
          Row() {
            Radio({ value: 'CicadaPlayer', group: 'playerType' })
              .checked(this.selectedPlayer === 'CicadaPlayer')
              .onChange((isChecked: boolean) => {
                if (isChecked) {
                  this.selectedPlayer = 'CicadaPlayer';
                  Settings.setPlayerType('CicadaPlayer');
                }
              })
            Text('CicadaPlayer').fontSize(14).fontColor('#333333')
          }
          .width('100%')
          .height(44)

          // The ExoPlayer kernel has no HarmonyOS counterpart and is not ported.
          // Android's "MediaPlayer" (system player) slot is filled by AVPlayer,
          // which is HarmonyOS's own system player, so it is selectable here.
          Row() {
            Radio({ value: 'AVPlayer', group: 'playerType' })
              .checked(this.selectedPlayer === 'AVPlayer')
              .onChange((isChecked: boolean) => {
                if (isChecked) {
                  this.selectedPlayer = 'AVPlayer';
                  Settings.setPlayerType('AVPlayer');
                }
              })
            Text('AVPlayer (系统播放器)').fontSize(14).fontColor('#333333')
          }
          .width('100%')
          .height(44)

          this.row('机型 : ', this.deviceModel)

          Button('黑名单').onClick(() => this.onBlacklist())
            .width('100%')
            .height(44)
            .backgroundColor('#3F51B5')
            .fontColor(Color.White)
            .margin({ top: 10, bottom: 20 })
        }
        .width('100%')
        .padding(10)
      }
      .layoutWeight(1)
      .width('100%')
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#FFFFFF')
  }
}
