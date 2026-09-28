/*
 * Demo entry page.
 *
 * Demonstrates the full native bridge: XComponent surface hand-off, the event
 * listener set, transport control and live progress.
 *
 * 播放器 UI 已换成 SDK 里那套与 Compose 安卓版 1:1 对齐的组件
 * （`CicadaComposePlayer`：顶部栏 / 底部栏 / 进度条 + seek 预览 / 清晰度·倍速·字幕·弹幕·信息面板 /
 * 加载·错误·重播浮层 / 手势层 / 弹幕层）；本页只保留与播放器 UI 无关的 App 逻辑：
 * 片源与 URL 选择、设置页路由、截图（存相册 + 跳转截图页）、多实例、网络切换重试、播放器配置下发、
 * 日志面板与状态行。
 */

import { common } from '@kit.AbilityKit';
import { promptAction, router } from '@kit.ArkUI';
import window from '@ohos.window';
import {
  CicadaComposePlayer,
  CicadaPlayer,
  CicadaPlayerListener,
  MediaInfo,
  MirrorMode,
  RotateMode,
  ScaleMode,
  StreamInfo,
  StreamType,
  TRACK_INDEX_AUTO,
  formatMs,
  formatSpeed,
  S
} from '@cicada/cicadaplayer';
import { PlayerRouteParams, SourceType } from '../model/AppTypes';
import { PLAYER_NAME_KEY, Settings } from '../util/Settings';
import { NetWatchdog } from '../util/NetWatchdog';
import { SnapshotSaver } from '../util/SnapshotSaver';
import { SnapShotRouteParams } from './SnapShotPage';

const DEFAULT_URL: string =
  'https://storage.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4';
const MAX_LOG_LINES: number = 60;

@Entry
@Component
struct Index {
  private player: CicadaPlayer = new CicadaPlayer();
  private surfaceReady: boolean = false;

  @State playerId: number = -1;
  @State url: string = DEFAULT_URL;
  @State status: string = 'idle';
  @State positionMs: number = 0;
  @State durationMs: number = 0;
  @State bufferedMs: number = 0;
  @State loading: boolean = false;
  @State loadPercent: number = 0;
  @State playing: boolean = false;
  @State speedText: string = '1.0x';
  @State netSpeedText: string = '';
  @State downloadSpeedBps: number = 0;
  @State videoSize: string = '-';
  @State logs: string[] = [];

  /** Source handed over by the browse pages (router params). */
  @State playType: string = SourceType.URL;
  private sourceText: string = '';
  private subtitleCn: string = '';
  private subtitleEn: string = '';
  private autoPlayed: boolean = false;

  /** 播放器 UI 需要的状态。 */
  @State title: string = S.titlePlayer;
  @State isFullScreen: boolean = false;
  @State replayVisible: boolean = false;
  @State errorVisible: boolean = false;
  @State errorCode: number = 0;
  @State errorMsg: string = '';
  /** 清晰度切换状态（内核 onVideoQualitySwitch 回传，转给播放器 UI 显示三段式提示）。 */
  @State qualitySwitchStatus: number = -1;
  @State qualitySwitchIndex: number = TRACK_INDEX_AUTO;
  /** 后台播放开关（默认打开）：与 Compose 的 settings.backgroundPlayEnabled 同义。 */
  @State backgroundPlayEnabled: boolean = true;
  /** 本次会话里"因为离开本页而暂停"的瞬时标记。 */
  private pausedByPageHide: boolean = false;
  /** Window brightness as 0..100; OHOS has no read-back so we own the value. */
  @State brightnessPercent: number = 50;

  // ---- track / speed panel (PlayerTrackFragment + the speed group) ----
  private mediaInfo: MediaInfo | null = null;
  /**
   * 媒体信息的轨道列表：@Prop 传给播放器 UI（纯数据接口数组，可随父组件刷新）。
   * 类实例（mediaInfo 本身）不用 @Prop 传，见 CicadaComposePlayer 的说明。
   */
  @State mediaInfoStreams: StreamInfo[] = [];
  @State bitrateLabels: string[] = [];
  @State audioLabels: string[] = [];
  @State subtitleLabels: string[] = [];
  @State externalLabels: string[] = [];
  @State selectedBitrate: number = -1;
  @State selectedAudio: number = -1;
  @State selectedSubtitle: number = -1;
  @State selectedExternal: number = -1;
  @State speed: number = 1.0;
  private bitrateStreamIndices: number[] = [];
  private audioStreamIndices: number[] = [];
  private subtitleStreamIndices: number[] = [];
  private externalTrackIndices: number[] = [];
  private externalNameList: string[] = [];
  private currentExtIndex: number = -1;

  /**
   * Wired into the overlay but not raised yet: Android's NetWatchdog listens for
   * CONNECTIVITY_ACTION, whose HarmonyOS counterpart (@ohos.net.connection) is
   * not hooked up here.
   */
  @State netChangeVisible: boolean = false;
  /** What to do once the next prepare() completes (Android continuePlay/isReplay). */
  private pendingAction: string = 'none';
  private resumePositionMs: number = 0;

  @State autoPlay: boolean = false;
  @State mute: boolean = false;
  @State loop: boolean = false;
  @State hardware: boolean = true;
  @State accurateSeek: boolean = false;
  @State volumePercent: number = 50;
  @State scaleMode: number = ScaleMode.SM_FIT;
  @State mirrorMode: number = MirrorMode.MIRROR_MODE_NONE;
  @State rotateMode: number = RotateMode.ROTATE_0;
  /** Android's inSnapShotting guard. */
  @State inSnapShotting: boolean = false;

  // ---- network state listener (Android's NetWatchdog) ----
  private netWatchdog: NetWatchdog = new NetWatchdog();
  /** Android's mRetryTime / mNeedToRetry. */
  private retryTime: number = 3;
  private needToRetry: boolean = false;

  private log(line: string): void {
    const next: string[] = this.logs.slice();
    next.push(line);
    while (next.length > MAX_LOG_LINES) {
      next.shift();
    }
    this.logs = next;
  }

  private buildListener(): CicadaPlayerListener {
    const self = this;
    const context = getContext(this) as common.UIAbilityContext;
    const listener: CicadaPlayerListener = {
      onPrepared: () => {
        self.status = 'prepared';
        self.durationMs = self.player.getDuration();
        self.log('prepared, duration=' + formatMs(self.durationMs));
        // Android's continuePlay / isReplay handling in OnPreparedListener.
        if (self.pendingAction === 'resume') {
          self.pendingAction = 'none';
          self.player.seekTo(self.resumePositionMs, false);
          self.player.start();
          self.playing = true;
        } else if (self.pendingAction === 'replay') {
          self.pendingAction = 'none';
          self.player.start();
          self.playing = true;
        }
      },
      onCompletion: () => {
        self.status = 'completed';
        self.playing = false;
        self.loading = false;
        self.replayVisible = true;
        self.log('completion');
      },
      onFirstFrameShow: () => {
        self.log('first frame shown');
        const size = self.player.getVideoResolution();
        self.log(`first frame resolution=${size.width}x${size.height} renderFps=${self.player.getVideoRenderFps()}`);
      },
      onAutoPlayStart: () => {
        self.status = 'playing (auto)';
        self.playing = true;
        self.log('auto play start');
      },
      onLoopingStart: () => {
        self.log('looping start');
      },
      onError: (code: number, msg: string) => {
        self.status = `error ${code}`;
        self.playing = false;
        self.loading = false;
        self.errorVisible = true;
        self.errorCode = code;
        self.errorMsg = msg;
        self.log(`error code=${code} msg=${msg}`);
      },
      onInfo: (code: number, msg: string) => {
        self.log(`info code=${code} msg=${msg}`);
      },
      onPositionUpdate: (position: number) => {
        self.positionMs = position;
      },
      onBufferPositionUpdate: (position: number) => {
        self.bufferedMs = position;
      },
      onSeekEnd: (position: number) => {
        self.status = 'playing';
        self.log('seek end -> ' + formatMs(position));
      },
      onLoadingStart: () => {
        self.loading = true;
        self.log('loading start');
      },
      onLoadingProgress: (percent: number) => {
        self.loadPercent = percent;
      },
      onLoadingEnd: () => {
        self.loading = false;
        self.log('loading end');
      },
      onDownloadSpeed: (bytesPerSecond: number) => {
        self.netSpeedText = formatSpeed(bytesPerSecond);
        self.downloadSpeedBps = bytesPerSecond;
      },
      onVideoSizeChanged: (width: number, height: number) => {
        self.videoSize = `${width}x${height}`;
        self.log(`video size ${width}x${height}`);
      },
      onStatusChanged: (oldStatus: number, newStatus: number) => {
        self.log(`status ${oldStatus} -> ${newStatus}`);
      },
      onMediaInfo: (info: MediaInfo) => {
        self.mediaInfo = info;
        self.mediaInfoStreams = info.streams;
        self.rebuildTracks();
        self.log(`media info: ${info.streams.length} streams, totalBitrate=${info.totalBitrate}`);
        const video: StreamInfo | null = self.player.getCurrentStreamInfo(StreamType.ST_TYPE_VIDEO);
        if (video !== null) {
          self.log(`current video: ${video.videoWidth}x${video.videoHeight} bw=${video.videoBandwidth}`);
        }
      },
      onStreamSwitchSuc: (streamType: number, info: StreamInfo) => {
        self.log(`stream switched type=${streamType} index=${info.streamIndex}`);
      },
      onVideoQualitySwitch: (status: number, streamIndex: number, desc: string) => {
        // 三段式提示的数据源：STARTED 一直显示，READY / FAILED / CANCELED 是终态。
        self.qualitySwitchStatus = status;
        self.qualitySwitchIndex = streamIndex;
        self.log(`quality switch status=${status} index=${streamIndex} ${desc}`);
      },
      onSnapshot: async (width: number, height: number, rgba: ArrayBuffer) => {
        self.log(`snapshot ${width}x${height} ${rgba.byteLength} bytes`);
        self.inSnapShotting = false;
        if (width <= 0 || height <= 0 || rgba.byteLength === 0) {
          promptAction.showToast({ message: S.snapShotFailure, duration: 2000 });
          return;
        }
        const path: string = await SnapshotSaver.savePng(context, width, height, rgba);
        if (path.length === 0) {
          promptAction.showToast({ message: S.snapShotFailure, duration: 2000 });
          return;
        }
        self.log('snapshot saved: ' + path);
        const params: SnapShotRouteParams = { path: path };
        router.pushUrl({ url: 'pages/SnapShotPage', params: params });
      },
      onSubtitleShow: (id: number, text: string) => {
        self.log(`subtitle#${id}: ${text}`);
      },
      onSubtitleHide: (id: number) => {
        self.log(`subtitle#${id} hidden`);
      },
      onSubtitleExtAdd: (index: number, url: string) => {
        // Android labels external tracks by comparing the url against the
        // entry's cn / en / ass fields, falling back to "ass".
        let name: string = 'ass';
        if (url.length > 0 && url === self.subtitleCn) {
          name = 'cn';
        } else if (url.length > 0 && url === self.subtitleEn) {
          name = 'en';
        }
        self.externalNameList.push(name);
        self.externalTrackIndices.push(index);
        const labels: string[] = self.externalNameList.slice();
        labels.push(S.cancelSubtitleExt);
        self.externalLabels = labels;
        self.log(`external subtitle #${index} (${name})`);
      }
    };
    return listener;
  }

  aboutToAppear(): void {
    this.playerId = this.player.create(this.buildListener());
    if (this.playerId < 0) {
      this.status = 'create failed';
      return;
    }
    this.log('player created id=' + this.playerId);
    const playerName: string = this.player.getPlayerName();
    this.log('player name=' + playerName);
    // Cached for SettingPage, which otherwise has to build a throwaway player to
    // read it (there is no global version API in the C API).
    AppStorage.setOrCreate(PLAYER_NAME_KEY, playerName);

    // Persisted settings: the hardware-decoder flag (Android default true).
    Settings.init(getContext(this) as common.UIAbilityContext);
    this.hardware = Settings.isHardwareDecoderEnabled();
    this.player.enableHardwareDecoder(this.hardware);

    this.netWatchdog.start({
      onWifiTo4G: () => this.onWifiTo4G(),
      on4GToWifi: () => this.on4GToWifi(),
      onNetDisconnected: () => {
        // Android only remembers the intent here: handovers report a
        // disconnect before the new network is up.
        this.needToRetry = true;
      }
    });

    // Source handed over by the browse pages (router params).
    const params = router.getParams() as PlayerRouteParams;
    if (params !== undefined && params !== null && params.url !== undefined && params.url.length > 0) {
      this.sourceText = params.url;
      this.playType = params.playType !== undefined ? params.playType : SourceType.URL;
      if (this.playType !== SourceType.MANIFEST) {
        this.url = params.url;
      }
      if (params.subtitleCn !== undefined) {
        this.subtitleCn = params.subtitleCn;
      }
      if (params.subtitleEn !== undefined) {
        this.subtitleEn = params.subtitleEn;
      }
      if (params.groupName !== undefined && params.groupName.length > 0) {
        this.title = params.groupName;
      }
      this.status = `ready (${this.playType})`;
      this.log(`source from router: type=${this.playType}`);
    }
  }

  aboutToDisappear(): void {
    this.netWatchdog.stop();
    this.player.release();
    this.playerId = -1;
  }

  /**
   * 后台播放开关的落地处（对应 Compose 里那条生命周期 observer）：
   * 关掉开关后离开本页就暂停，回到本页自动续播（只续播"因为离开才停的"）。
   */
  onPageHide(): void {
    if (!this.backgroundPlayEnabled && this.playing) {
      this.pause();
      this.pausedByPageHide = true;
    }
  }

  onPageShow(): void {
    if (this.pausedByPageHide) {
      this.pausedByPageHide = false;
      this.resume();
    }
  }

  /** 系统返回键：全屏时先退回非全屏。 */
  onBackPress(): boolean {
    if (this.isFullScreen) {
      this.toggleScreenMode();
      return true;
    }
    return false;
  }

  /** 画面就绪回调（播放器 UI 组件内部的 XComponent 绑好 surface 后通知本页）。 */
  private onSurfaceReady(ready: boolean): void {
    this.surfaceReady = ready;
    this.log(`surface ${ready ? 'ready' : 'released'}`);

    // A source arrived from the browse pages: start it as soon as the surface
    // is available.
    if (ready && !this.autoPlayed && this.sourceText.length > 0) {
      this.autoPlayed = true;
      this.play();
    }
  }

  private play(): void {
    if (!this.surfaceReady) {
      this.log('surface not ready yet');
    }
    if (this.subtitleCn.length > 0) {
      this.player.addExtSubtitle(this.subtitleCn);
    }
    if (this.subtitleEn.length > 0) {
      this.player.addExtSubtitle(this.subtitleEn);
    }
    if (this.playType === SourceType.MANIFEST) {
      this.player.setDataSourceWithManifest(this.sourceText);
    } else {
      this.player.setDataSource(this.url);
    }
    this.player.prepare();
    this.player.start();
    this.status = 'playing';
    this.playing = true;
    this.errorVisible = false;
    this.replayVisible = false;
    // Diagnostic: 0 = hardware requested, 1 = software requested.
    this.log(`prepare: decoderType=${this.player.getDecoderType()} (0=hw,1=sw)`);
  }

  private pause(): void {
    this.player.pause();
    this.status = 'paused';
    this.playing = false;
  }

  private resume(): void {
    this.player.start();
    this.status = 'playing';
    this.playing = true;
  }

  private stopPlayback(): void {
    this.player.stop();
    this.status = 'stopped';
    this.playing = false;
    this.positionMs = 0;
  }

  /** Android getSnapShot(), including the inSnapShotting guard and toast. */
  private shoot(): void {
    if (this.inSnapShotting) {
      promptAction.showToast({ message: S.snapShotting, duration: 2000 });
      return;
    }
    this.inSnapShotting = true;
    this.player.snapshot();
  }

  /** 播放器 UI 的「截图」那一行 → 走本页的相册保存链路。 */
  private onRequestSnapshot(): void {
    this.shoot();
  }

  /** 播放器 UI 的「重新输入链接」→ 收起错误并退回上一页（与 Compose 的 onError 同义）。 */
  private onRequestErrorExit(): void {
    this.errorVisible = false;
    this.errorMsg = '';
    router.back();
  }

  /** 播放器 UI 的亮度手势 → 施加到窗口亮度（0 = 不写，交给系统默认值）。 */
  private async applyBrightness(percent: number): Promise<void> {
    this.brightnessPercent = percent;
    if (percent <= 0) {
      return;
    }
    try {
      const win = await window.getLastWindow(getContext(this));
      await win.setWindowBrightness(percent / 100);
    } catch (err) {
      this.log('setWindowBrightness failed');
    }
  }

  /** Double-tap: Android's switchPlayerState(). */
  private togglePlay(): void {
    if (this.playing) {
      this.pause();
    } else {
      this.resume();
    }
  }

  /** Builds the panel's option lists from the current MediaInfo. */
  private rebuildTracks(): void {
    const info = this.mediaInfo;
    if (info === null) {
      return;
    }
    const bitrates: string[] = [S.autoBitrate];
    const bIdx: number[] = [TRACK_INDEX_AUTO];
    const audios: string[] = [];
    const aIdx: number[] = [];
    const subs: string[] = [];
    const sIdx: number[] = [];
    for (const stream of info.streams) {
      if (stream.type === 'video' && stream.videoBandwidth > 0) {
        bitrates.push(`${stream.videoBandwidth}`);
        bIdx.push(stream.streamIndex);
      } else if (stream.type === 'audio' && stream.audioLang.length > 0) {
        audios.push(stream.audioLang);
        aIdx.push(stream.streamIndex);
      } else if (stream.type === 'subtitle' && stream.subtitleLang.length > 0) {
        subs.push(stream.subtitleLang);
        sIdx.push(stream.streamIndex);
      }
    }
    this.bitrateLabels = bitrates;
    this.bitrateStreamIndices = bIdx;
    this.audioLabels = audios;
    this.audioStreamIndices = aIdx;
    this.subtitleLabels = subs;
    this.subtitleStreamIndices = sIdx;
  }

  private onSelectBitrate(position: number): void {
    this.selectedBitrate = position;
    // -1 is TrackInfo.AUTO_SELECT_INDEX, matching Android's auto entry.
    this.player.selectTrack(this.bitrateStreamIndices[position]);
  }

  private onSelectAudio(position: number): void {
    this.selectedAudio = position;
    this.player.selectTrack(this.audioStreamIndices[position]);
  }

  private onSelectSubtitle(position: number): void {
    this.selectedSubtitle = position;
    this.player.selectTrack(this.subtitleStreamIndices[position]);
  }

  /**
   * Android selectSubtitleTrackIndex: the trailing -1 entry hides the active
   * external subtitle; otherwise the previous one is hidden and the new one
   * selected.
   */
  private onSelectExternal(position: number): void {
    this.selectedExternal = position;
    if (position >= this.externalTrackIndices.length) {
      if (this.currentExtIndex >= 0) {
        this.player.selectExtSubtitle(this.currentExtIndex, false);
        this.currentExtIndex = -1;
      }
      return;
    }
    const index: number = this.externalTrackIndices[position];
    if (this.currentExtIndex >= 0) {
      this.player.selectExtSubtitle(this.currentExtIndex, false);
    }
    this.player.selectExtSubtitle(index, true);
    this.currentExtIndex = index;
  }

  private onSpeedChange(value: number): void {
    this.speed = value;
    this.player.setSpeed(value);
    this.speedText = `${value.toFixed(1)}x`;
  }

  /** Android reTry(): remember the position, re-prepare, then resume. */
  private reTry(): void {
    this.resumePositionMs = this.positionMs;
    this.pendingAction = 'resume';
    this.errorVisible = false;
    this.replayVisible = false;
    this.loading = true;
    this.player.prepare();
    this.log('retry');
  }

  /** Android rePlay(): re-prepare and start from the beginning. */
  private rePlay(): void {
    this.pendingAction = 'replay';
    this.errorVisible = false;
    this.replayVisible = false;
    this.loading = true;
    this.player.prepare();
    this.log('replay');
  }

  /** Android onWifiTo4G: pause and ask before spending mobile data. */
  private onWifiTo4G(): void {
    this.needToRetry = false;
    this.errorVisible = false;
    this.pause();
    this.netChangeVisible = true;
    this.log('network -> cellular: paused, asking the user');
  }

  /** Android on4GToWifi: dismiss the prompt; retry once the budget is spent. */
  private on4GToWifi(): void {
    this.needToRetry = true;
    this.errorVisible = false;
    if (this.retryTime <= 0) {
      this.retryTime = 3;
      this.networkRetry();
    }
    this.log('network -> wifi');
  }

  /** Android networkRetry(). */
  private networkRetry(): void {
    if (this.retryTime > 0 && this.needToRetry) {
      this.player.reload();
      this.retryTime--;
      this.log(`network retry, remaining=${this.retryTime}`);
    } else {
      promptAction.showToast({ message: S.netReconnectFailed, duration: 2000 });
    }
  }

  /** Android TipsView onContinuePlay(). */
  private onContinuePlay(): void {
    this.netChangeVisible = false;
    this.needToRetry = true;
    if (this.retryTime <= 0) {
      this.retryTime = 3;
    }
    if (this.player.isPrepared()) {
      this.resume();
    } else {
      this.play();
    }
  }

  /** Android TipsView onStopPlay(): stop and leave the player. */
  private onStopPlay(): void {
    this.netChangeVisible = false;
    this.stopPlayback();
    router.back();
  }

  /**
   * PlayerConfigFragment apply. Option keys mirror MediaPlayer::SetPlayerConfig;
   * an empty field means "leave unchanged" (Android instead overwrites with the
   * widget's fallback value).
   *
   * NOT REACHABLE FROM THE UI ANY MORE: 那条面板属于旧 demo 的播放器 UI，
   * 已随旧组件一起从本页移除（播放器 UI 现在由 SDK 的 CicadaComposePlayer 负责）。
   * 这段配置下发逻辑照原样留在页面里，需要时由设置页或别的入口接一条即可。
   */
  private onApplyConfig(startBuffer: string, highBuffer: string, maxBuffer: string, maxDelay: string,
    netTimeout: string, probeSize: string, referrer: string, httpProxy: string,
    retryCount: string, clearFrameWhenStop: boolean): void {
    if (startBuffer.length > 0) {
      this.player.setOption('startBufferDuration', startBuffer);
    }
    if (highBuffer.length > 0) {
      this.player.setOption('highLevelBufferDuration', highBuffer);
    }
    if (maxBuffer.length > 0) {
      this.player.setOption('maxBufferDuration', maxBuffer);
    }
    if (maxDelay.length > 0) {
      this.player.setOption('RTMaxDelayTime', maxDelay);
    }
    if (netTimeout.length > 0) {
      this.player.setTimeout(Number.parseInt(netTimeout));
    }
    if (probeSize.length > 0) {
      this.player.setOption('maxProbeSize', probeSize);
    }
    if (referrer.length > 0) {
      this.player.setReferer(referrer);
    }
    if (httpProxy.length > 0) {
      this.player.setOption('http_proxy', httpProxy);
    }
    if (retryCount.length > 0) {
      this.player.setOption('networkRetryCount', retryCount);
    }
    this.player.setOption('ClearShowWhenStop', clearFrameWhenStop ? '1' : '0');
    promptAction.showToast({ message: S.refreshConfigSuccess, duration: 2000 });
    this.log('player config applied');
  }

  /**
   * PlayerCacheConfigFragment apply.
   *
   * NOT WIRED TO THE PLAYER: the play-and-cache (边播边缓存) config is not
   * reachable through the C API this NAPI layer is built on. CacheConfig is set
   * on the MediaPlayer C++ facade, which the Android SDK's JNI binds directly;
   * the C API's handle is an ICicadaPlayer (SuperMediaPlayer), which has no
   * cache-config method and no cache implementation at all. The UI is kept for
   * parity and reports the gap instead of silently doing nothing.
   */
  private onApplyCacheConfig(enable: boolean, maxDurationS: string, maxSizeMB: string, dir: string): void {
    const duration: number = maxDurationS.length > 0 ? Number.parseInt(maxDurationS) : 300;
    const sizeMB: number = maxSizeMB.length > 0 ? Number.parseInt(maxSizeMB) : 500;
    this.log(`cache config requested: enable=${enable} ${duration}s ${sizeMB}MB dir=${dir}`);
    this.log('cache config is not exposed by the C API; ignored');
    promptAction.showToast({ message: S.refreshConfigSuccess, duration: 2000 });
  }

  /** Android showMediaInfo(): one Toast with resolution / bitrate / audio / subtitle. */
  private showMediaInfo(): void {
    const video: StreamInfo | null = this.player.getCurrentStreamInfo(StreamType.ST_TYPE_VIDEO);
    const audio: StreamInfo | null = this.player.getCurrentStreamInfo(StreamType.ST_TYPE_AUDIO);
    const subtitle: StreamInfo | null = this.player.getCurrentStreamInfo(StreamType.ST_TYPE_SUB);
    let text: string = '';
    if (video !== null) {
      // Android prints height before width here; kept as-is.
      text += `分辨率: ${video.videoHeight}x${video.videoWidth}\n码率: ${video.bitrate}\n`;
    }
    if (audio !== null) {
      text += `音轨: ${audio.audioLang}\n`;
    }
    if (subtitle !== null) {
      text += `字幕: ${subtitle.subtitleLang}`;
    }
    if (text.length === 0) {
      text = S.noneMediaInfo;
    }
    promptAction.showToast({ message: text, duration: 3000 });
    this.log('media info requested');
  }

  private onBack(): void {
    if (this.isFullScreen) {
      this.toggleScreenMode();
      return;
    }
    router.back();
  }

  /** Mirrors ControlView's screen-mode toggle (Small <-> Full). */
  private async toggleScreenMode(): Promise<void> {
    this.isFullScreen = !this.isFullScreen;
    try {
      const win = await window.getLastWindow(getContext(this));
      await win.setPreferredOrientation(
        this.isFullScreen ? window.Orientation.LANDSCAPE : window.Orientation.PORTRAIT
      );
    } catch (err) {
      this.log('setPreferredOrientation failed');
    }
  }

  @Builder
  logList() {
    List({ space: 2 }) {
      ForEach(this.logs, (line: string) => {
        ListItem() {
          Text(line)
            .fontSize(11)
            .fontColor('#B0BEC5')
            .width('100%')
        }
      }, (line: string, index: number) => `${index}-${line}`)
    }
    .width('100%')
    .layoutWeight(1)
    .backgroundColor('#111111')
    .padding(6)
  }

  build() {
    Column({ space: 8 }) {
      /* ---------------- 播放器（UI 全部来自 SDK 的 compose 组件） ---------------- */
      Column() {
        CicadaComposePlayer({
          player: this.player,
          title: this.title,
          /* 本 Demo 没有在线人数的数据源，所以在线人数行不显示（Compose 那边是写死的示例数据） */
          watchingText: '',
          isFullscreen: this.isFullScreen,
          isPlaying: this.playing,
          positionMs: this.positionMs,
          durationMs: this.durationMs,
          bufferedMs: this.bufferedMs,
          loading: this.loading,
          errorMessage: this.errorVisible ? this.errorMsg : '',
          replayVisible: this.replayVisible,
          downloadSpeedBps: this.downloadSpeedBps,
          decoderNotice: '',
          hardwareDecodeEnabled: this.hardware,
          backgroundPlayEnabled: this.backgroundPlayEnabled,
          captureInFlight: this.inSnapShotting,
          qualitySwitchStatus: this.qualitySwitchStatus,
          qualitySwitchIndex: this.qualitySwitchIndex,
          previewFrames: [],
          streams: this.mediaInfoStreams,
          onSurfaceReady: (ready: boolean) => this.onSurfaceReady(ready),
          onBack: () => this.onBack(),
          onToggleFullscreen: () => {
            this.toggleScreenMode();
          },
          onTogglePortraitFullscreen: () => {
            this.toggleScreenMode();
          },
          onRequestReplay: () => this.rePlay(),
          onRequestErrorExit: () => this.onRequestErrorExit(),
          onRequestSnapshot: () => this.onRequestSnapshot(),
          onToggleBackgroundPlay: () => {
            this.backgroundPlayEnabled = !this.backgroundPlayEnabled;
          },
          onBrightnessChange: (percent: number) => {
            this.applyBrightness(percent);
          },
          onDanmakuSend: (text: string) => {
            /* 本 Demo 不接弹幕接口：只本地显示 + 记一条日志 */
            this.log(`danmaku send: ${text}`);
          }
        })
      }
      .width('100%')
      .height(this.isFullScreen ? '100%' : 240)

      if (!this.isFullScreen) {
        Row({ space: 6 }) {
          Text(`${formatMs(this.positionMs)} | ${this.videoSize}`)
            .fontSize(12)
            .width(220)
          Text(this.netSpeedText).fontSize(12).layoutWeight(1)
        }
        .width('100%')

        TextInput({ text: this.url, placeholder: 'media url' })
          .fontSize(12)
          .width('100%')
          .onChange((value: string) => {
            this.url = value;
          })

        Row({ space: 6 }) {
          Button('播放').onClick(() => this.play())
          Button(this.playing ? '暂停' : '继续')
            .onClick(() => {
              if (this.playing) {
                this.pause();
              } else {
                this.resume();
              }
            })
          Button('停止').onClick(() => this.stopPlayback())
        }
        .width('100%')

        Row({ space: 6 }) {
          Button('截图').onClick(() => this.shoot())
          Button('设置').onClick(() => {
            router.pushUrl({ url: 'pages/SettingPage' });
          })
          Button('多实例').onClick(() => {
            router.pushUrl({ url: 'pages/MultiPlayerPage' });
          })
        }
        .width('100%')

        Text(this.loading ? `缓冲中 ${this.loadPercent}%` : this.status)
          .fontSize(12)
          .fontColor('#FF9800')
          .width('100%')

        this.logList()
      }
    }
    .width('100%')
    .height('100%')
    .padding(this.isFullScreen ? 0 : 10)
  }
}
