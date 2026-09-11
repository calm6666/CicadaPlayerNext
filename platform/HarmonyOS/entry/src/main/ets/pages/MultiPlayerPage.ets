/*
 * Multi-instance playback — HarmonyOS counterpart of the Android demo's
 * MultiPlayerActivity: a vertical list of sources, each with its own player and
 * video surface.
 *
 * The selection is handed over as JSON through AppStorage (the Android demo
 * uses a static ArrayList on the activity), so the list survives the router
 * push without a static mutable field.
 */

import { router } from '@kit.ArkUI';
import { TitleBar } from '../view/TitleBar';
import { MultiPlayerItem } from '../view/MultiPlayerItem';
import { MultiPlayerSelection } from '../model/CicadaTypes';
import { MULTI_PLAYER_KEY } from './SourceChooseListPage';
import { S } from '../util/Strings';

@Entry
@Component
struct MultiPlayerPage {
  @State items: MultiPlayerSelection[] = [];

  aboutToAppear(): void {
    const stored: string | undefined = AppStorage.get<string>(MULTI_PLAYER_KEY);
    if (stored !== undefined && stored !== null && stored.length > 0) {
      try {
        this.items = JSON.parse(stored) as MultiPlayerSelection[];
      } catch (err) {
        this.items = [];
      }
    }
  }

  build() {
    Column() {
      TitleBar({
        title: S.titleMultipleInstances,
        onBack: () => router.back()
      })

      List({ space: 10 }) {
        ForEach(this.items, (item: MultiPlayerSelection, index: number) => {
          ListItem() {
            MultiPlayerItem({ url: item.url, label: item.name })
          }
        }, (item: MultiPlayerSelection, index: number) => `${index}-${item.url}`)
      }
      .layoutWeight(1)
      .width('100%')
      .padding(6)
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#FFFFFF')
  }
}
