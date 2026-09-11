/*
 * One row of the multi-instance list — HarmonyOS counterpart of the Android
 * demo's MultiPlayerAdapter item: a 200dp video surface with a row of four
 * equal-width buttons (prepare / play / pause / stop).
 *
 * Android keys one CicadaPlayer per URL in a HashMap and rebinds the
 * TextureView surface on every onBindViewHolder; ArkUI components are not
 * recycled the same way, so each item simply owns its own player for its whole
 * lifetime (created in aboutToAppear, released in aboutToDisappear).
 */

import { CicadaPlayer } from '../player/CicadaPlayer';
import { S } from '../util/Strings';

@Component
export struct MultiPlayerItem {
  @Prop url: string = '';
  @Prop label: string = '';

  private controller: XComponentController = new XComponentController();
  private player: CicadaPlayer = new CicadaPlayer();
  private surfaceReady: boolean = false;

  @State status: string = 'idle';
  @State playing: boolean = false;

  aboutToAppear(): void {
    const self = this;
    this.player.create({
      onPrepared: () => {
        self.status = 'prepared';
      },
      onFirstFrameShow: () => {
        self.status = 'playing';
        self.playing = true;
      },
      onVideoSizeChanged: (width: number, height: number) => {
        self.status = `${width}x${height}`;
      },
      onError: (code: number, msg: string) => {
        self.status = `error ${code}`;
        self.playing = false;
      },
      onCompletion: () => {
        self.status = 'completed';
        self.playing = false;
      }
    });
  }

  aboutToDisappear(): void {
    this.player.release();
  }

  private onSurfaceLoad(): void {
    this.surfaceReady = this.player.setSurface(this.controller.getXComponentSurfaceId());
  }

  @Builder
  actionButton(label: string, action: () => void) {
    Button(label)
      .fontSize(12)
      .layoutWeight(1)
      .height(36)
      .backgroundColor('#3F51B5')
      .fontColor(Color.White)
      .onClick(action)
  }

  build() {
    Column() {
      Stack() {
        XComponent({
          id: `multiSurface-${this.label}`,
          type: XComponentType.SURFACE,
          controller: this.controller
        })
          .onLoad(() => this.onSurfaceLoad())
          .width('100%')
          .height('100%')
          .backgroundColor(Color.Black)

        Text(this.status)
          .fontSize(11)
          .fontColor('#FF9800')
          .margin(6)
          .align(Alignment.TopStart)
      }
      .width('100%')
      .height(200)

      Text(this.label)
        .fontSize(12)
        .fontColor('#333333')
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
        .width('100%')
        .padding({ left: 6, right: 6, top: 4, bottom: 4 })

      Row({ space: 4 }) {
        this.actionButton(S.prepare, () => {
          this.player.setDataSource(this.url);
          this.player.prepare();
          this.status = 'preparing';
        })
        this.actionButton(S.play, () => {
          this.player.start();
          this.playing = true;
        })
        this.actionButton(S.pause, () => {
          this.player.pause();
          this.playing = false;
        })
        this.actionButton(S.stop, () => {
          this.player.stop();
          this.playing = false;
          this.status = 'stopped';
        })
      }
      .width('100%')
      .padding({ left: 4, right: 4, bottom: 6 })
    }
    .width('100%')
    .margin({ bottom: 10 })
    .backgroundColor('#F5F5F5')
  }
}
