import { XComponent, XComponentController } from '@ohos.arkui.xcomponent';
import testNapi from 'libentry.so';

@Entry
@Component
struct Index {
  private xComponentController: XComponentController = new XComponentController();
  private playerId: number = -1;
  @State surfaceId: string = '';
  @State status: string = 'idle';

  aboutToAppear(): void {
    this.playerId = testNapi.create();
    this.status = 'created';
  }

  aboutToDisappear(): void {
    if (this.playerId >= 0) {
      testNapi.stop(this.playerId);
      testNapi.release(this.playerId);
      this.playerId = -1;
    }
  }

  onSurfaceLoad(): void {
    // XComponentController.getXComponentSurfaceId() returns the surface id
    // (decimal string). Native turns it into an OHNativeWindow for
    // OH_AVCodec surface-mode hardware decoding.
    this.surfaceId = this.xComponentController.getXComponentSurfaceId();
    testNapi.setSurface(this.playerId, this.surfaceId);
    this.status = 'surface ready';
  }

  playUrl(): void {
    // classic URL playback
    testNapi.setDataSource(this.playerId,
      'https://storage.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4');
    testNapi.prepare(this.playerId);
    testNapi.start(this.playerId);
    this.status = 'playing url';
  }

  playManifestObject(): void {
    // object-based playback: unified MediaManifest JSON (no m3u8/mpd),
    // ClearKey DRM via contentProtection + licenseServer.
    const manifest = {
      duration: 596.0,
      live: false,
      mediaSourceType: 'hls',
      contentProtection: [
        {
          schemeIdUri: 'urn:mpeg:dash:mp4protection:2011',
          value: 'cenc'
        },
        {
          schemeIdUri: 'urn:uuid:e2719d58-a985-b3c9-781a-b030af78d30e',
          keyId: '01234567-89ab-cdef-0123-456789abcdef'
        }
      ],
      licenseServer: {
        url: 'http://localhost:8765/api/clearkey/bbb',
        keyType: 'clearkey'
      },
      video: [
        {
          id: 'v0',
          baseUrl: 'https://cdn.example.com/bbb/',
          bandwidth: 2500000,
          mimeType: 'video/mp4',
          codecs: 'avc1.640033',
          width: 1920,
          height: 1080,
          segmentInfo: {
            mode: 'template',
            initialization: 'init-0.m4s',
            media: 'seg-*.m4s',
            startNumber: 1,
            targetDuration: 4,
            totalCount: 149
          }
        }
      ],
      audio: [
        {
          id: 'a0',
          baseUrl: 'https://cdn.example.com/bbb/',
          bandwidth: 128000,
          mimeType: 'audio/mp4',
          codecs: 'mp4a.40.2',
          lang: 'en',
          segmentInfo: {
            mode: 'template',
            initialization: 'init-a0.m4s',
            media: 'seg-a-*.m4s',
            startNumber: 1,
            targetDuration: 4,
            totalCount: 149
          }
        }
      ]
    };
    testNapi.setDataSourceManifest(this.playerId, JSON.stringify(manifest));
    testNapi.prepare(this.playerId);
    testNapi.start(this.playerId);
    this.status = 'playing manifest object (ClearKey DRM)';
  }

  pause(): void {
    testNapi.pause(this.playerId);
    this.status = 'paused';
  }

  resume(): void {
    testNapi.start(this.playerId);
    this.status = 'playing';
  }

  seekTo(positionMs: number): void {
    testNapi.seek(this.playerId, positionMs, true);
  }

  build() {
    Column({ space: 12 }) {
      XComponent({
        id: 'videoSurface',
        type: XComponentType.SURFACE,
        controller: this.xComponentController
      })
        .onLoad(() => this.onSurfaceLoad())
        .width('100%')
        .height(320)
        .backgroundColor(Color.Black)

      Text(this.status).fontSize(14)

      Row({ space: 8 }) {
        Button('URL 播放').onClick(() => this.playUrl())
        Button('对象播放(DRM)').onClick(() => this.playManifestObject())
      }
      Row({ space: 8 }) {
        Button('暂停').onClick(() => this.pause())
        Button('继续').onClick(() => this.resume())
        Button('跳转 60s').onClick(() => this.seekTo(60000))
      }
    }
    .width('100%')
    .height('100%')
    .padding(16)
  }
}
