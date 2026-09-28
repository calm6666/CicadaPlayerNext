/*
 * Manual URL entry — HarmonyOS counterpart of the Android demo's
 * SourceInputUrlActivity: one plain URL field plus the DASH / HLS / normal /
 * MediaManifest-JSON shortcuts. Every button ends up in the same player page;
 * the player auto-detects the container, exactly as on Android.
 */

import { common } from '@kit.AbilityKit';
import { promptAction, router } from '@kit.ArkUI';
import { scanBarcode } from '@kit.ScanKit';
import { TitleBar } from '../view/TitleBar';
import { PlayerRouteParams, SourceType } from '../model/AppTypes';
import { S, UrlDefaults } from '@cicada/cicadaplayer';

@Entry
@Component
struct SourceInputUrlPage {
  @State url: string = UrlDefaults.INPUT_TEXT;
  @State dashUrl: string = '';
  @State hlsUrl: string = '';
  @State normalUrl: string = '';
  @State manifestJson: string = '';

  private toast(message: string): void {
    try {
      promptAction.showToast({ message: message, duration: 2000 });
    } catch (error) {
      // TODO: Implement error handling.
    }
  }

  /**
   * QR scan — the counterpart of the Android demo's zxing CaptureActivity.
   *
   * The Android contract (REQ_CODE 156, extra "qr_scan_result") has no ArkTS
   * equivalent: ScanKit brings up its own viewfinder with its own camera
   * permission handling and returns the decoded string directly. Kept
   * behaviour: the result is only written into the URL field, it is not
   * auto-played, and an empty/cancelled scan reports "Scan failed!".
   */
  private async scanQrCode(): Promise<void> {
    try {
      const context = getContext(this) as common.UIAbilityContext;
      // scanTypes keeps its default (all formats), which covers the Android
      // format set (ONE_D + QR_CODE + DATA_MATRIX).
      const options: scanBarcode.ScanOptions = { enableAlbum: true };
      const result: scanBarcode.ScanResult = await scanBarcode.startScanForResult(context, options);
      if (result === undefined || result === null || result.originalValue === undefined ||
        result.originalValue.length === 0) {
        this.toast(S.scanFailed);
        return;
      }
      this.url = result.originalValue;
    } catch (err) {
      this.toast(S.scanFailed);
    }
  }

  /**
   * HarmonyOS system-player route. Android's second button here was
   * "exoPlayer"; ExoPlayer has no HarmonyOS counterpart and is not ported, so
   * the slot is filled by AVPlayer. Like Android's exoPlayer button, an empty
   * field falls back to the hint.
   */
  private playAvPlayer(url: string): void {
    const target: string = this.valueOrHint(url, UrlDefaults.INPUT_HINT);
    if (target.length === 0) {
      this.toast(S.stsInputUrlFirst);
      return;
    }
    const params: PlayerRouteParams = { url: target, playType: SourceType.URL };
    router.pushUrl({ url: 'pages/AvPlayerPage', params: params });
  }

  private playUrl(url: string, playType: string): void {
    if (url.length === 0) {
      this.toast(S.stsInputUrlFirst);
      return;
    }
    const params: PlayerRouteParams = { url: url, playType: playType };
    router.pushUrl({ url: 'pages/Index', params: params });
  }

  /** Falls back to the field's hint, matching the Android behaviour. */
  private valueOrHint(value: string, hint: string): string {
    return value.length > 0 ? value : hint;
  }

  private playManifest(): void {
    const json: string = this.valueOrHint(this.manifestJson, UrlDefaults.MANIFEST_HINT);
    if (json.length === 0) {
      this.toast(S.stsManifestFirst);
      return;
    }
    const params: PlayerRouteParams = { url: json, playType: SourceType.MANIFEST };
    router.pushUrl({ url: 'pages/Index', params: params });
  }

  build() {
    Column() {
      TitleBar({
        title: S.titleInputUrlItem,
        rightText: S.titleScanning,
        onBack: () => router.back(),
        onRight: () => {
          this.scanQrCode();
        }
      })

      Scroll() {
        Column() {
          // ---- plain URL + cicadaPlayer ----
          TextInput({ text: this.url, placeholder: UrlDefaults.INPUT_HINT })
            .fontSize(13)
            .width('100%')
            .onChange((text: string) => {
              this.url = text;
            })
          Button('cicadaPlayer')
            .width('100%')
            .height(44)
            .margin({ top: 6 })
            .backgroundColor('#3F51B5')
            .fontColor(Color.White)
            .onClick(() => this.playUrl(this.url, SourceType.URL))

          // Replaces Android's "exoPlayer" button (ExoPlayer is Android-only).
          Button('AVPlayer')
            .width('100%')
            .height(44)
            .margin({ top: 6 })
            .backgroundColor('#3F51B5')
            .fontColor(Color.White)
            .onClick(() => this.playAvPlayer(this.url))

          // ---- DASH ----
          TextInput({ text: this.dashUrl, placeholder: UrlDefaults.DASH_HINT })
            .fontSize(13)
            .width('100%')
            .margin({ top: 16 })
            .onChange((text: string) => {
              this.dashUrl = text;
            })
          Button('播放 DASH (URL)')
            .width('100%')
            .height(44)
            .margin({ top: 6 })
            .backgroundColor('#3F51B5')
            .fontColor(Color.White)
            .onClick(() => this.playUrl(this.valueOrHint(this.dashUrl, UrlDefaults.DASH_HINT), SourceType.URL))

          // ---- HLS ----
          TextInput({ text: this.hlsUrl, placeholder: UrlDefaults.HLS_HINT })
            .fontSize(13)
            .width('100%')
            .margin({ top: 16 })
            .onChange((text: string) => {
              this.hlsUrl = text;
            })
          Button('播放 HLS (URL)')
            .width('100%')
            .height(44)
            .margin({ top: 6 })
            .backgroundColor('#3F51B5')
            .fontColor(Color.White)
            .onClick(() => this.playUrl(this.valueOrHint(this.hlsUrl, UrlDefaults.HLS_HINT), SourceType.URL))

          // ---- plain progressive ----
          TextInput({ text: this.normalUrl, placeholder: UrlDefaults.NORMAL_HINT })
            .fontSize(13)
            .width('100%')
            .margin({ top: 16 })
            .onChange((text: string) => {
              this.normalUrl = text;
            })
          Button('播放普通视频 (URL)')
            .width('100%')
            .height(44)
            .margin({ top: 6 })
            .backgroundColor('#3F51B5')
            .fontColor(Color.White)
            .onClick(() => this.playUrl(this.valueOrHint(this.normalUrl, UrlDefaults.NORMAL_HINT), SourceType.URL))

          // ---- object-based manifest (DRM capable) ----
          TextArea({ text: this.manifestJson, placeholder: UrlDefaults.MANIFEST_HINT })
            .fontSize(12)
            .width('100%')
            .height(140)
            .margin({ top: 16 })
            .onChange((text: string) => {
              this.manifestJson = text;
            })
          Button('对象播放 (JSON 传入)')
            .width('100%')
            .height(44)
            .margin({ top: 6, bottom: 20 })
            .backgroundColor('#3F51B5')
            .fontColor(Color.White)
            .onClick(() => this.playManifest())
        }
        .width('100%')
        .padding(12)
      }
      .layoutWeight(1)
      .width('100%')
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#FFFFFF')
  }
}
