/*
 * System-player page, built on HarmonyOS AVPlayer (@kit.MediaKit).
 *
 * This is the HarmonyOS counterpart of the Android demo's system-player slot:
 * Android offered an ExoPlayer kernel (@kit-less, Android-only) and a
 * MediaPlayer kernel through CicadaPlayerFactory; on HarmonyOS the system player
 * is AVPlayer, so ExoPlayer is dropped and this page fills the "system player"
 * role instead. It is reachable from the settings kernel switch and from the
 * AVPlayer button on the URL page.
 *
 * AVPlayer state machine notes (it is stricter than the Cicada player):
 *   - surfaceId may only be assigned while idle;
 *   - url may only be assigned while idle (it moves the player to initialized);
 *   - prepare() requires initialized, play() requires prepared, and so on.
 * Every call below is guarded and failures are reported through the error event.
 */

import { media } from '@kit.MediaKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { fileIo } from '@kit.CoreFileKit';
import { promptAction, router } from '@kit.ArkUI';
import { TitleBar } from '../view/TitleBar';
import { PlayerRouteParams } from '../model/CicadaTypes';
import { LocalVideoSource } from '../util/LocalVideoSource';
import { formatMs } from '../util/TimeFormater';
import { S } from '../util/Strings';

@Entry
@Component
struct AvPlayerPage {
  private avPlayer: media.AVPlayer | null = null;
  private controller: XComponentController = new XComponentController();
  private surfaceId: string = '';
  private pendingUrl: string = '';
  private autoPlayed: boolean = false;
  /** Descriptor backing an fdSrc source; must stay open while playing. */
  private heldFile: fileIo.File | null = null;

  @State url: string = '';
  @State state: string = 'idle';
  @State positionMs: number = 0;
  @State durationMs: number = 0;
  @State videoSize: string = '-';

  aboutToAppear(): void {
    const params = router.getParams() as PlayerRouteParams;
    if (params !== undefined && params !== null && params.url !== undefined) {
      this.url = params.url;
      this.pendingUrl = params.url;
    }
    this.createPlayer();
  }

  aboutToDisappear(): void {
    this.releasePlayer();
  }

  private async createPlayer(): Promise<void> {
    try {
      const player: media.AVPlayer = await media.createAVPlayer();
      this.avPlayer = player;

      player.on('stateChange', (playerState: media.AVPlayerState) => {
        this.state = playerState;
        if (playerState === 'initialized' && !this.autoPlayed) {
          // prepare() is only valid from 'initialized'.
          this.autoPlayed = true;
          this.prepare();
        }
      });

      player.on('error', (error: BusinessError) => {
        this.state = `error ${error.code}`;
        promptAction.showToast({ message: `${error.code}: ${error.message}`, duration: 3000 });
      });

      player.on('timeUpdate', (time: number) => {
        this.positionMs = time;
      });

      player.on('durationUpdate', (duration: number) => {
        this.durationMs = duration;
      });

      player.on('videoSizeChange', (width: number, height: number) => {
        this.videoSize = `${width}x${height}`;
      });

      // surfaceId and the source may only be set while idle.
      if (this.surfaceId.length > 0) {
        player.surfaceId = this.surfaceId;
      }
      if (this.pendingUrl.length > 0) {
        this.applySource(player, this.pendingUrl);
      }
    } catch (err) {
      this.state = 'create failed';
    }
  }

  /**
   * AVPlayer accepts a path/URL through `url`, or a descriptor through `fdSrc`.
   * Only a *file* URI from the picker needs the fdSrc route; a network URL or a
   * plain sandbox path is handed to `url` directly.
   */
  private applySource(player: media.AVPlayer, source: string): void {
    if (!LocalVideoSource.isFileUri(source)) {
      player.url = source;
      return;
    }
    try {
      if (this.heldFile !== null) {
        fileIo.closeSync(this.heldFile);
        this.heldFile = null;
      }
      const file: fileIo.File = fileIo.openSync(source, fileIo.OpenMode.READ_ONLY);
      const size: number = fileIo.statSync(source).size;
      this.heldFile = file;
      const descriptor: media.AVFileDescriptor = { fd: file.fd, offset: 0, length: size };
      player.fdSrc = descriptor;
    } catch (err) {
      this.state = 'open failed';
    }
  }

  private async releasePlayer(): Promise<void> {
    const player = this.avPlayer;
    this.avPlayer = null;
    if (this.heldFile !== null) {
      try {
        fileIo.closeSync(this.heldFile);
      } catch (err) {
        // Best effort.
      }
      this.heldFile = null;
    }
    if (player === null) {
      return;
    }
    try {
      await player.release();
    } catch (err) {
      // Nothing useful to do while tearing down.
    }
  }

  private onSurfaceLoad(): void {
    this.surfaceId = this.controller.getXComponentSurfaceId();
    const player = this.avPlayer;
    if (player !== null && this.state === 'idle') {
      player.surfaceId = this.surfaceId;
    }
  }

  private async prepare(): Promise<void> {
    const player = this.avPlayer;
    if (player === null) {
      return;
    }
    try {
      if (this.state === 'idle' && this.url.length > 0) {
        this.applySource(player, this.url);
        return; // the stateChange handler calls prepare() from 'initialized'
      }
      if (this.state === 'initialized' || this.state === 'stopped' ||
        this.state === 'completed' || this.state === 'paused') {
        await player.prepare();
        await player.play();
      } else if (this.state === 'prepared') {
        await player.play();
      }
    } catch (err) {
      this.state = 'prepare failed';
    }
  }

  private async play(): Promise<void> {
    const player = this.avPlayer;
    if (player === null) {
      return;
    }
    try {
      if (this.state === 'prepared' || this.state === 'paused' || this.state === 'completed') {
        await player.play();
      } else {
        this.prepare();
      }
    } catch (err) {
      this.state = 'play failed';
    }
  }

  private async pause(): Promise<void> {
    const player = this.avPlayer;
    if (player === null) {
      return;
    }
    try {
      if (this.state === 'playing') {
        await player.pause();
      }
    } catch (err) {
      this.state = 'pause failed';
    }
  }

  private async stopPlayback(): Promise<void> {
    const player = this.avPlayer;
    if (player === null) {
      return;
    }
    try {
      if (this.state === 'playing' || this.state === 'paused' || this.state === 'prepared') {
        await player.stop();
      }
    } catch (err) {
      this.state = 'stop failed';
    }
  }

  private seekTo(positionMs: number): void {
    const player = this.avPlayer;
    if (player === null) {
      return;
    }
    // AVPlayer.seek is synchronous; SEEK_CLOSEST maps to the demo's accurate mode.
    if (this.state === 'playing' || this.state === 'paused' || this.state === 'prepared') {
      player.seek(positionMs, media.SeekMode.SEEK_CLOSEST);
    }
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
      TitleBar({
        title: 'AVPlayer',
        onBack: () => router.back()
      })

      Stack() {
        XComponent({
          id: 'avPlayerSurface',
          type: XComponentType.SURFACE,
          controller: this.controller
        })
          .onLoad(() => this.onSurfaceLoad())
          .width('100%')
          .height('100%')
          .backgroundColor(Color.Black)

        Text(`${this.state}  ${this.videoSize}`)
          .fontSize(11)
          .fontColor('#FF9800')
          .margin(6)
          .align(Alignment.TopStart)
      }
      .width('100%')
      .height(240)

      Row({ space: 6 }) {
        Text(`${formatMs(this.positionMs)} / ${formatMs(this.durationMs)}`)
          .fontSize(12)
          .layoutWeight(1)
      }
      .width('100%')
      .padding({ left: 8, right: 8, top: 6 })

      Row({ space: 6 }) {
        this.actionButton(S.prepare, () => {
          this.prepare();
        })
        this.actionButton(S.play, () => {
          this.play();
        })
        this.actionButton(S.pause, () => {
          this.pause();
        })
        this.actionButton(S.stop, () => {
          this.stopPlayback();
        })
      }
      .width('100%')
      .padding(8)

      Row({ space: 6 }) {
        this.actionButton('-10s', () => this.seekTo(Math.max(0, this.positionMs - 10000)))
        this.actionButton('+10s', () => this.seekTo(this.positionMs + 10000))
      }
      .width('100%')
      .padding({ left: 8, right: 8 })

      TextInput({ text: this.url, placeholder: 'media url' })
        .fontSize(12)
        .width('100%')
        .margin({ top: 8 })
        .padding({ left: 8, right: 8 })
        .onChange((value: string) => {
          this.url = value;
        })

      Blank().layoutWeight(1)
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#FFFFFF')
  }
}
