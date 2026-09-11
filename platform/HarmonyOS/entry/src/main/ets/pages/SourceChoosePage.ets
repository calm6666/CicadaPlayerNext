/*
 * Launcher page — HarmonyOS counterpart of the Android demo's
 * SourceChooseActivity: title bar with a Setting action, an "Input Url" button,
 * a "Select Local Video" button, and the list of source groups read from
 * sourceList.json.
 *
 * Differences from Android, by design:
 *  - no READ/WRITE_EXTERNAL_STORAGE permission dance (the list is read from
 *    resources/rawfile and never copied to shared storage);
 *  - the picked video is NOT copied: the picker URI is turned into an openable
 *    path (see LocalVideoSource) or, for the AVPlayer kernel, handed over as a
 *    file descriptor via fdSrc.
 */

import { common } from '@kit.AbilityKit';
import { picker } from '@kit.CoreFileKit';
import { promptAction, router } from '@kit.ArkUI';
import { TitleBar } from '../view/TitleBar';
import { SourceGroup, SourceGroupParams, SourceType } from '../model/CicadaTypes';
import { SourceListParser } from '../util/SourceListParser';
import { LocalVideoSource } from '../util/LocalVideoSource';
import { PLAYER_TYPE_AVPLAYER, Settings } from '../util/Settings';
import { S } from '../util/Strings';

@Entry
@Component
struct SourceChoosePage {
  private groups: SourceGroup[] = [];
  @State groupNames: string[] = [];
  @State pickedPath: string = '';

  aboutToAppear(): void {
    const context = getContext(this) as common.UIAbilityContext;
    this.groups = SourceListParser.parse(context);
    this.groupNames = SourceListParser.groupNames(this.groups);
  }

  private toast(message: string): void {
    promptAction.showToast({ message: message, duration: 2000 });
  }

  private openGroup(groupName: string): void {
    const params: SourceGroupParams = { groupName: groupName };
    router.pushUrl({ url: 'pages/SourceChooseListPage', params: params });
  }

  private openInputUrl(): void {
    router.pushUrl({ url: 'pages/SourceInputUrlPage' });
  }

  private openSetting(): void {
    router.pushUrl({ url: 'pages/SettingPage' });
  }

  /**
   * Local video pick: ACTION_OPEN_DOCUMENT + copyVideoToCache.
   *
   * The picker returns a document URI (file://docs/...), which the player cannot
   * open — feeding it straight to CicadaSetDataSourceWithUrl fails in PREPARINIT
   * with ENOENT ("No such file or directory"). So the file is copied into the
   * app cache first and the sandbox path is what gets played, exactly like the
   * Android demo's copy step.
   */
  private async pickLocalVideo(): Promise<void> {
    let uri: string = '';
    try {
      const documentPicker = new picker.DocumentViewPicker(getContext(this));
      const options = new picker.DocumentSelectOptions();
      options.maxSelectNumber = 1;
      options.fileSuffixFilters = ['.mp4', '.mkv', '.flv', '.m3u8', '.mov', '.mp3'];
      const uris: string[] = await documentPicker.select(options);
      if (uris === undefined || uris === null || uris.length === 0) {
        return;
      }
      uri = uris[0];
    } catch (err) {
      // The picker itself could not be opened.
      this.toast(S.pickVideoFailed);
      return;
    }

    // The AVPlayer kernel accepts a descriptor directly (fdSrc), so the raw URI
    // is handed over and the page opens the fd itself — no copy, no path
    // resolution needed.
    if (Settings.getPlayerType() === PLAYER_TYPE_AVPLAYER) {
      router.pushUrl({
        url: 'pages/AvPlayerPage',
        params: { url: uri, playType: SourceType.LOCAL, groupName: 'Local' }
      });
      return;
    }

    // Cicada kernel: FFmpeg needs a path. LocalVideoSource resolves one without
    // copying (real path when reachable, otherwise /proc/self/fd/<fd>).
    const localPath: string = LocalVideoSource.resolve(uri);
    if (localPath.length === 0) {
      this.toast(S.openVideoFailed);
      return;
    }

    this.pickedPath = localPath;
    router.pushUrl({
      url: 'pages/Index',
      params: { url: this.pickedPath, playType: SourceType.LOCAL, groupName: 'Local' }
    });
  }

  @Builder
  primaryButton(label: string, action: () => void) {
    Button(label)
      .width('100%')
      .height(50)
      .margin({ top: 8 })
      .backgroundColor('#3F51B5')
      .fontColor(Color.White)
      .onClick(action)
  }

  build() {
    Column() {
      TitleBar({
        title: S.titleChooseItem,
        rightText: S.titleSetting,
        showBack: false,
        onRight: () => this.openSetting()
      })

      Column() {
        this.primaryButton(S.btnInputUrl, () => this.openInputUrl())
        this.primaryButton(S.btnLocalVideo, () => {
          this.pickLocalVideo();
        })

        List({ space: 10 }) {
          ForEach(this.groupNames, (groupName: string) => {
            ListItem() {
              Text(groupName)
                .width('100%')
                .height(50)
                .fontSize(16)
                .fontColor(Color.White)
                .textAlign(TextAlign.Center)
                .backgroundColor('#dddddd')
                .borderRadius(4)
            }
            .onClick(() => this.openGroup(groupName))
          }, (groupName: string) => groupName)
        }
        .layoutWeight(1)
        .width('100%')
        .margin({ top: 20 })
      }
      .layoutWeight(1)
      .width('100%')
      .padding(10)
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#FFFFFF')
  }
}
