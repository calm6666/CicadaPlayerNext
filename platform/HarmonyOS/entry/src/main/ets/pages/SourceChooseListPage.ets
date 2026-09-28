/*
 * Sources of one group — HarmonyOS counterpart of the Android demo's
 * SourceChooseListActivity: title = group name, right action = "Multiple
 * instances", a list of entries with a selection checkbox, tap to play.
 *
 * Only entries whose type is url/live are playable from here, matching the
 * Android demo (which compares with equalsIgnoreCase, so the local-file
 * fallback's "URL" type also works).
 */

import { common } from '@kit.AbilityKit';
import { promptAction, router } from '@kit.ArkUI';
import { TitleBar } from '../view/TitleBar';
import {
  MultiPlayerSelection,
  PlayerRouteParams,
  SourceGroup,
  SourceGroupParams,
  SourceSample,
  SourceType
} from '../model/AppTypes';
import { SourceListParser } from '../util/SourceListParser';
import { PLAYER_TYPE_AVPLAYER, Settings } from '../util/Settings';
import { S } from '@cicada/cicadaplayer';

/** AppStorage key holding the entries chosen for multi-instance playback. */
export const MULTI_PLAYER_KEY: string = 'multiPlayerSelection';

@Entry
@Component
struct SourceChooseListPage {
  @State groupName: string = '';
  @State samples: SourceSample[] = [];
  /** Parallel to `samples`; kept as plain state so re-render is reliable. */
  @State checked: boolean[] = [];

  aboutToAppear(): void {
    const params = router.getParams() as SourceGroupParams;
    if (params !== undefined && params !== null && params.groupName !== undefined) {
      this.groupName = params.groupName;
    }

    const context = getContext(this) as common.UIAbilityContext;
    const groups: SourceGroup[] = SourceListParser.parse(context);
    const found: SourceSample[] = SourceListParser.samplesOf(groups, this.groupName);
    this.samples = found;
    const flags: boolean[] = [];
    for (let i = 0; i < found.length; i++) {
      flags.push(false);
    }
    this.checked = flags;

    if (found.length === 0) {
      // Android falls back to scanning the external-storage root for media
      // files. There is no shared storage root on HarmonyOS; use the picker
      // from the launcher page instead.
      promptAction.showToast({ message: S.noneMediaInfo, duration: 2000 });
    }
  }

  private toggle(index: number): void {
    const next: boolean[] = this.checked.slice();
    next[index] = !next[index];
    this.checked = next;
  }

  private play(sample: SourceSample): void {
    if (!SourceListParser.isPlayable(sample)) {
      return;
    }
    const params: PlayerRouteParams = {
      url: sample.url,
      playType: SourceType.URL,
      groupName: this.groupName
    };
    if (SourceListParser.hasExternalSubtitle(sample) && sample.subtitle !== undefined) {
      params.subtitleCn = sample.subtitle.cn;
      params.subtitleEn = sample.subtitle.en;
    }
    // The settings kernel switch decides which player handles the source.
    const target: string = Settings.getPlayerType() === PLAYER_TYPE_AVPLAYER
      ? 'pages/AvPlayerPage'
      : 'pages/Index';
    router.pushUrl({ url: target, params: params });
  }

  private collectChecked(): SourceSample[] {
    const out: SourceSample[] = [];
    for (let i = 0; i < this.samples.length; i++) {
      if (this.checked[i]) {
        out.push(this.samples[i]);
      }
    }
    return out;
  }

  private openMultiPlayer(): void {
    const chosen: SourceSample[] = this.collectChecked();
    if (chosen.length === 0) {
      promptAction.showToast({ message: S.noneMediaInfo, duration: 2000 });
      return;
    }
    // Android pushes every checked TypeInfo into a static list on the activity;
    // here the selection travels as JSON through AppStorage.
    const selections: MultiPlayerSelection[] = [];
    for (const sample of chosen) {
      selections.push({ url: sample.url, name: sample.name });
    }
    AppStorage.setOrCreate(MULTI_PLAYER_KEY, JSON.stringify(selections));
    router.pushUrl({ url: 'pages/MultiPlayerPage' });
  }

  build() {
    Column() {
      TitleBar({
        title: this.groupName,
        rightText: S.titleMultipleInstances,
        onBack: () => router.back(),
        onRight: () => this.openMultiPlayer()
      })

      List() {
        ForEach(this.samples, (sample: SourceSample, index: number) => {
          ListItem() {
            Column() {
              Row() {
                Column() {
                  Text(sample.name)
                    .fontSize(15)
                    .fontColor('#3F51B5')
                    .maxLines(1)
                    .textOverflow({ overflow: TextOverflow.Ellipsis })
                    .width('100%')
                  Text(sample.url)
                    .fontSize(11)
                    .fontColor('#666666')
                    .maxLines(1)
                    .textOverflow({ overflow: TextOverflow.Ellipsis })
                    .width('100%')
                    .margin({ top: 4 })
                }
                .layoutWeight(1)
                .alignItems(HorizontalAlign.Start)

                Checkbox()
                  .select(this.checked[index])
                  .onChange(() => this.toggle(index))
              }
              .width('100%')
              .padding({ left: 10, right: 10, top: 10, bottom: 10 })

              Divider().strokeWidth(1).color('#000000')
            }
            .width('100%')
          }
          .onClick(() => this.play(sample))
        }, (sample: SourceSample, index: number) => `${index}-${sample.url}`)
      }
      .layoutWeight(1)
      .width('100%')
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#FFFFFF')
  }
}
