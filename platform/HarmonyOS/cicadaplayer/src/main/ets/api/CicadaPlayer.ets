/*
 * CicadaPlayer — typed ArkTS wrapper over the native player.
 *
 * This is the HarmonyOS counterpart of the Android SDK's CicadaPlayer /
 * CicadaPlayerImpl: it owns a native player handle and turns the flat native
 * event stream into the listener callbacks the pages use.
 */

import cicada from 'libcicadaplayer.so';
import {
  CicadaPlayerListener,
  DecoderType,
  CicadaDrmCallback,
  MediaInfo,
  MirrorMode,
  PlayerEventName,
  PlayerStatus,
  PropertyKey,
  RotateMode,
  ScaleMode,
  StreamInfo,
  StreamType,
  VideoCodecSupport,
  VideoResolution
} from './CicadaTypes';

export class CicadaPlayer {
  private id: number = -1;
  private listener: CicadaPlayerListener = {};
  private prepared: boolean = false;

  /** Creates the native player and registers the event listener. */
  create(listener?: CicadaPlayerListener, opts?: string): number {
    if (this.id >= 0) {
      return this.id;
    }
    this.listener = listener ?? {};
    this.id = cicada.create(opts ?? '');
    if (this.id < 0) {
      return this.id;
    }
    cicada.setListener(this.id, (event: string, a: number, b: number, payload: string | ArrayBuffer) => {
      this.handleEvent(event, a, b, payload);
    });
    return this.id;
  }

  /** True once create() succeeded. */
  isValid(): boolean {
    return this.id >= 0;
  }

  getPlayerId(): number {
    return this.id;
  }

  release(): void {
    if (this.id < 0) {
      return;
    }
    cicada.stop(this.id);
    cicada.release(this.id);
    this.id = -1;
    this.prepared = false;
  }

  isPrepared(): boolean {
    return this.prepared;
  }

  // ---------------------------------------------------------------- view
  setSurface(surfaceId: string): boolean {
    if (this.id < 0) {
      return false;
    }
    return cicada.setSurface(this.id, surfaceId);
  }

  clearScreen(): void {
    if (this.id >= 0) {
      cicada.clearScreen(this.id);
    }
  }

  // -------------------------------------------------------------- source
  setDataSource(url: string): void {
    if (this.id >= 0) {
      cicada.setDataSource(this.id, url);
    }
  }

  /** Object-based playback with a unified MediaManifest JSON (DRM capable). */
  setDataSourceWithManifest(manifestJson: string): void {
    if (this.id >= 0) {
      cicada.setDataSourceManifest(this.id, manifestJson);
    }
  }

  addExtSubtitle(uri: string): void {
    if (this.id >= 0) {
      cicada.addExtSubtitle(this.id, uri);
    }
  }

  selectExtSubtitle(index: number, select: boolean): void {
    if (this.id >= 0) {
      cicada.selectExtSubtitle(this.id, index, select);
    }
  }

  setStreamDelayTime(index: number, timeMs: number): number {
    if (this.id < 0) {
      return -1;
    }
    return cicada.setStreamDelayTime(this.id, index, timeMs);
  }

  // ----------------------------------------------------------- transport
  prepare(): void {
    if (this.id >= 0) {
      cicada.prepare(this.id);
    }
  }

  start(): void {
    if (this.id >= 0) {
      cicada.start(this.id);
    }
  }

  pause(): void {
    if (this.id >= 0) {
      cicada.pause(this.id);
    }
  }

  stop(): number {
    if (this.id < 0) {
      return -1;
    }
    this.prepared = false;
    return cicada.stop(this.id);
  }

  /** Reloads the current source (used for the network-retry flow). */
  reload(): void {
    if (this.id >= 0) {
      cicada.reload(this.id);
    }
  }

  seekTo(positionMs: number, accurate: boolean = false): void {
    if (this.id >= 0) {
      cicada.seek(this.id, positionMs, accurate);
    }
  }

  // ------------------------------------------------- speed / volume / etc
  setSpeed(speed: number): void {
    if (this.id >= 0) {
      cicada.setSpeed(this.id, speed);
    }
  }

  getSpeed(): number {
    return this.id < 0 ? 1.0 : cicada.getSpeed(this.id);
  }

  setVolume(volume: number): void {
    if (this.id >= 0) {
      cicada.setVolume(this.id, volume);
    }
  }

  getVolume(): number {
    return this.id < 0 ? 0 : cicada.getVolume(this.id);
  }

  setMute(mute: boolean): void {
    if (this.id >= 0) {
      cicada.setMute(this.id, mute);
    }
  }

  isMute(): boolean {
    return this.id < 0 ? false : cicada.isMute(this.id);
  }

  setLoop(loop: boolean): void {
    if (this.id >= 0) {
      cicada.setLoop(this.id, loop);
    }
  }

  getLoop(): boolean {
    return this.id < 0 ? false : cicada.getLoop(this.id);
  }

  setAutoPlay(autoPlay: boolean): void {
    if (this.id >= 0) {
      cicada.setAutoPlay(this.id, autoPlay);
    }
  }

  isAutoPlay(): boolean {
    return this.id < 0 ? false : cicada.isAutoPlay(this.id);
  }

  // ------------------------------------------------------- picture modes
  setScaleMode(mode: ScaleMode): void {
    if (this.id >= 0) {
      cicada.setScaleMode(this.id, mode);
    }
  }

  getScaleMode(): ScaleMode {
    return this.id < 0 ? ScaleMode.SM_FIT : cicada.getScaleMode(this.id) as ScaleMode;
  }

  setRotateMode(mode: RotateMode): void {
    if (this.id >= 0) {
      cicada.setRotateMode(this.id, mode);
    }
  }

  getRotateMode(): RotateMode {
    return this.id < 0 ? RotateMode.ROTATE_0 : cicada.getRotateMode(this.id) as RotateMode;
  }

  setMirrorMode(mode: MirrorMode): void {
    if (this.id >= 0) {
      cicada.setMirrorMode(this.id, mode);
    }
  }

  getMirrorMode(): MirrorMode {
    return this.id < 0 ? MirrorMode.MIRROR_MODE_NONE : cicada.getMirrorMode(this.id) as MirrorMode;
  }

  setVideoBackgroundColor(color: number): void {
    if (this.id >= 0) {
      cicada.setVideoBackgroundColor(this.id, color);
    }
  }

  // ------------------------------------------------------------ progress
  getDuration(): number {
    return this.id < 0 ? -1 : cicada.getDuration(this.id);
  }

  getCurrentPosition(): number {
    return this.id < 0 ? -1 : cicada.getCurrentPosition(this.id);
  }

  getCurrentBufferedPosition(): number {
    return this.id < 0 ? -1 : cicada.getCurrentBufferedPosition(this.id);
  }

  getMasterClockPts(): number {
    return this.id < 0 ? -1 : cicada.getMasterClockPts(this.id);
  }

  getVideoResolution(): VideoResolution {
    if (this.id < 0) {
      return new VideoResolution();
    }
    const json: string = cicada.getVideoResolution(this.id);
    return JSON.parse(json) as VideoResolution;
  }

  getVideoRotation(): number {
    return this.id < 0 ? 0 : cicada.getVideoRotation(this.id);
  }

  // -------------------------------------------------------------- tracks
  /** Switches to the given stream index; returns the resulting StreamType. */
  selectTrack(index: number): StreamType {
    if (this.id < 0) {
      return StreamType.ST_TYPE_UNKNOWN;
    }
    return cicada.switchStreamIndex(this.id, index) as StreamType;
  }

  getCurrentStreamIndex(type: StreamType): number {
    return this.id < 0 ? -1 : cicada.getCurrentStreamIndex(this.id, type);
  }

  getCurrentStreamInfo(type: StreamType): StreamInfo | null {
    if (this.id < 0) {
      return null;
    }
    const json: string = cicada.getCurrentStreamInfo(this.id, type);
    if (json.length === 0 || json === 'null') {
      return null;
    }
    return JSON.parse(json) as StreamInfo;
  }

  // ------------------------------------------------------------ snapshot
  /** Result is delivered through listener.onSnapshot. */
  snapshot(): void {
    if (this.id >= 0) {
      cicada.captureScreen(this.id);
    }
  }

  // ------------------------------------------------------------- network
  setTimeout(timeoutMs: number): void {
    if (this.id >= 0) {
      cicada.setTimeout(this.id, timeoutMs);
    }
  }

  setDropBufferThreshold(dropValueMs: number): void {
    if (this.id >= 0) {
      cicada.setDropBufferThreshold(this.id, dropValueMs);
    }
  }

  setReferer(referer: string): void {
    if (this.id >= 0) {
      cicada.setRefer(this.id, referer);
    }
  }

  setUserAgent(userAgent: string): void {
    if (this.id >= 0) {
      cicada.setUserAgent(this.id, userAgent);
    }
  }

  addCustomHttpHeader(header: string): void {
    if (this.id >= 0) {
      cicada.addCustomHttpHeader(this.id, header);
    }
  }

  removeAllCustomHttpHeader(): void {
    if (this.id >= 0) {
      cicada.removeAllCustomHttpHeader(this.id);
    }
  }

  setDefaultBandWidth(bandwidth: number): void {
    if (this.id >= 0) {
      cicada.setDefaultBandWidth(this.id, bandwidth);
    }
  }

  enableHardwareDecoder(enable: boolean): void {
    if (this.id >= 0) {
      cicada.setDecoderType(this.id, enable ? DecoderType.DT_HARDWARE : DecoderType.DT_SOFTWARE);
    }
  }

  getDecoderType(): DecoderType {
    return this.id < 0 ? DecoderType.DT_HARDWARE : cicada.getDecoderType(this.id) as DecoderType;
  }

  enterBackground(back: boolean): void {
    if (this.id >= 0) {
      cicada.enterBackGround(this.id, back);
    }
  }

  // ---------------------------------------------------- options / misc
  setOption(key: string, value: string): number {
    return this.id < 0 ? -1 : cicada.setOption(this.id, key, value);
  }

  getOption(key: string): string {
    return this.id < 0 ? '' : cicada.getOption(this.id, key);
  }

  getPropertyLong(key: PropertyKey): number {
    return this.id < 0 ? 0 : cicada.getPropertyLong(this.id, key);
  }

  getPlayerName(): string {
    return this.id < 0 ? '' : cicada.getPlayerName(this.id);
  }

  getVideoRenderFps(): number {
    return this.id < 0 ? 0 : cicada.getVideoRenderFps(this.id);
  }

  getVideoDecodeFps(): number {
    return this.id < 0 ? 0 : cicada.getVideoDecodeFps(this.id);
  }

  // ------------------------------------------------- 解码事实 / 切档状态
  /** 活动视频解码器此刻走的是硬解还是软解（含解码途中回退），用于界面显示"解码方式"。 */
  isVideoDecoderHardware(): boolean {
    return this.id < 0 ? false : cicada.isVideoDecoderHardware(this.id);
  }

  /** 当前是否有清晰度切换在途（ABR 让路用）。 */
  isStreamSwitchInFlight(): boolean {
    return this.id < 0 ? false : cicada.isStreamSwitchInFlight(this.id);
  }

  // ------------------------------------------------------------- 画面滤镜
  /** 色觉辅助 3x3 颜色矩阵（行主序 9 个数）；传 null 或不传 = 关闭。 */
  setColorMatrix(matrix: number[] | null): boolean {
    if (this.id < 0) {
      return false;
    }
    return cicada.setColorMatrix(this.id, matrix);
  }

  /** 设置滤镜链配置，必须在 prepare 之前调用。 */
  setFilterConfig(filterConfig: string): number {
    return this.id < 0 ? -1 : cicada.setFilterConfig(this.id, filterConfig);
  }

  updateFilterConfig(target: string, options: string): number {
    return this.id < 0 ? -1 : cicada.updateFilterConfig(this.id, target, options);
  }

  /** target 传空串 = 对全部滤镜生效；invalid=true 表示启用。 */
  setFilterInvalid(target: string, invalid: boolean): number {
    return this.id < 0 ? -1 : cicada.setFilterInvalid(this.id, target, invalid);
  }

  // --------------------------------------------- 硬解能力 / 编码偏好
  /** 取当前生效的能力与偏好；取不到返回 null。 */
  getVideoCodecSupport(): VideoCodecSupport | null {
    if (this.id < 0) {
      return null;
    }
    const json: string = cicada.getVideoCodecSupport(this.id);
    if (json.length === 0 || json === 'null') {
      return null;
    }
    return JSON.parse(json) as VideoCodecSupport;
  }

  /** 应用侧直接告诉内核设备能力与偏好；传 null = 清除，恢复内核自行探测。 */
  setVideoCodecSupport(support: VideoCodecSupport | null): number {
    if (this.id < 0) {
      return -1;
    }
    return cicada.setVideoCodecSupport(this.id, support === null ? null : JSON.stringify(support));
  }

  // -------------------------------------------------------------- 属性
  /** 字符串属性（PropertyKey 见 CicadaTypes），paramJson 视属性而定，可为空串。 */
  getPropertyString(key: PropertyKey, paramJson: string): string {
    return this.id < 0 ? '' : cicada.getPropertyString(this.id, key, paramJson);
  }

  // --------------------------------------------------------------- DRM
  /** 注册 DRM 许可证回调（见 CicadaDrmCallback 的线程约定）。 */
  setDrmCallback(callback: CicadaDrmCallback): boolean {
    if (this.id < 0) {
      return false;
    }
    return cicada.setDrmRequestCallback(this.id, callback);
  }
  // ------------------------------------------------------------- events
  private handleEvent(name: string, a: number, b: number, payload: string | ArrayBuffer): void {
    const listener = this.listener;
    switch (name) {
      case PlayerEventName.PREPARED:
        this.prepared = true;
        if (listener.onPrepared) {
          listener.onPrepared();
        }
        break;
      case PlayerEventName.COMPLETION:
        if (listener.onCompletion) {
          listener.onCompletion();
        }
        break;
      case PlayerEventName.FIRST_FRAME_SHOW:
        if (listener.onFirstFrameShow) {
          listener.onFirstFrameShow();
        }
        break;
      case PlayerEventName.AUTO_PLAY_START:
        this.prepared = true;
        if (listener.onAutoPlayStart) {
          listener.onAutoPlayStart();
        }
        break;
      case PlayerEventName.LOOPING_START:
        if (listener.onLoopingStart) {
          listener.onLoopingStart();
        }
        break;
      case PlayerEventName.ERROR:
        if (listener.onError) {
          listener.onError(a, payload as string);
        }
        break;
      case PlayerEventName.INFO:
        if (listener.onInfo) {
          listener.onInfo(a, payload as string);
        }
        break;
      case PlayerEventName.POSITION_UPDATE:
        if (listener.onPositionUpdate) {
          listener.onPositionUpdate(a);
        }
        break;
      case PlayerEventName.BUFFER_POSITION_UPDATE:
        if (listener.onBufferPositionUpdate) {
          listener.onBufferPositionUpdate(a);
        }
        break;
      case PlayerEventName.SEEKING:
        if (listener.onSeeking) {
          listener.onSeeking(a);
        }
        break;
      case PlayerEventName.SEEK_END:
        if (listener.onSeekEnd) {
          listener.onSeekEnd(a);
        }
        break;
      case PlayerEventName.LOADING_START:
        if (listener.onLoadingStart) {
          listener.onLoadingStart();
        }
        break;
      case PlayerEventName.LOADING_PROGRESS:
        if (listener.onLoadingProgress) {
          listener.onLoadingProgress(a);
        }
        break;
      case PlayerEventName.LOADING_END:
        if (listener.onLoadingEnd) {
          listener.onLoadingEnd();
        }
        break;
      case PlayerEventName.DOWNLOAD_SPEED:
        if (listener.onDownloadSpeed) {
          listener.onDownloadSpeed(a);
        }
        break;
      case PlayerEventName.VIDEO_SIZE_CHANGED:
        if (listener.onVideoSizeChanged) {
          listener.onVideoSizeChanged(a, b);
        }
        break;
      case PlayerEventName.STATUS_CHANGED:
        if (a === PlayerStatus.PLAYER_ERROR) {
          this.prepared = false;
        }
        if (listener.onStatusChanged) {
          listener.onStatusChanged(a, b);
        }
        break;
      case PlayerEventName.VIDEO_RENDERED:
        if (listener.onVideoRendered) {
          listener.onVideoRendered(a, b);
        }
        break;
      case PlayerEventName.AUDIO_RENDERED:
        if (listener.onAudioRendered) {
          listener.onAudioRendered(a, b);
        }
        break;
      case PlayerEventName.MEDIA_INFO:
        if (listener.onMediaInfo) {
          listener.onMediaInfo(JSON.parse(payload as string) as MediaInfo);
        }
        break;
      case PlayerEventName.STREAM_SWITCH_SUC:
        if (listener.onStreamSwitchSuc) {
          listener.onStreamSwitchSuc(a, JSON.parse(payload as string) as StreamInfo);
        }
        break;
      case PlayerEventName.SNAPSHOT:
        if (listener.onSnapshot) {
          listener.onSnapshot(a, b, payload as ArrayBuffer);
        }
        break;
      case PlayerEventName.SUBTITLE_SHOW:
        if (listener.onSubtitleShow) {
          listener.onSubtitleShow(a, payload as string);
        }
        break;
      case PlayerEventName.SUBTITLE_HIDE:
        if (listener.onSubtitleHide) {
          listener.onSubtitleHide(a);
        }
        break;
      case PlayerEventName.VIDEO_QUALITY_SWITCH:
        if (listener.onVideoQualitySwitch) {
          listener.onVideoQualitySwitch(a, b, payload as string);
        }
        break;
      case PlayerEventName.SUBTITLE_EXT_ADD:
        if (listener.onSubtitleExtAdd) {
          listener.onSubtitleExtAdd(a, payload as string);
        }
        break;
      default:
        break;
    }
  }
}
