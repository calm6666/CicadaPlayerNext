if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface Index_Params {
    controller?: XComponentController;
    player?: CicadaPlayer;
    surfaceReady?: boolean;
    playerId?: number;
    url?: string;
    status?: string;
    positionMs?: number;
    durationMs?: number;
    bufferedMs?: number;
    loading?: boolean;
    loadPercent?: number;
    playing?: boolean;
    speedText?: string;
    netSpeedText?: string;
    videoSize?: string;
    logs?: string[];
    playType?: string;
    sourceText?: string;
    subtitleCn?: string;
    subtitleEn?: string;
    autoPlayed?: boolean;
    title?: string;
    isFullScreen?: boolean;
    locked?: boolean;
    dragging?: boolean;
    seekPreviewMs?: number;
    brightnessPercent?: number;
    mediaInfo?: MediaInfo | null;
    showTrackPanel?: boolean;
    bitrateLabels?: string[];
    audioLabels?: string[];
    subtitleLabels?: string[];
    externalLabels?: string[];
    selectedBitrate?: number;
    selectedAudio?: number;
    selectedSubtitle?: number;
    selectedExternal?: number;
    speed?: number;
    bitrateStreamIndices?: number[];
    audioStreamIndices?: number[];
    subtitleStreamIndices?: number[];
    externalTrackIndices?: number[];
    externalNameList?: string[];
    currentExtIndex?: number;
    errorVisible?: boolean;
    errorCode?: number;
    errorMsg?: string;
    replayVisible?: boolean;
    netChangeVisible?: boolean;
    pendingAction?: string;
    resumePositionMs?: number;
    showOptionPanel?: boolean;
    autoPlay?: boolean;
    mute?: boolean;
    loop?: boolean;
    hardware?: boolean;
    accurateSeek?: boolean;
    volumePercent?: number;
    scaleMode?: number;
    mirrorMode?: number;
    rotateMode?: number;
    inSnapShotting?: boolean;
    netWatchdog?: NetWatchdog;
    retryTime?: number;
    needToRetry?: boolean;
}
import type common from "@ohos:app.ability.common";
import promptAction from "@ohos:promptAction";
import router from "@ohos:router";
import window from "@ohos:window";
import { CicadaPlayer } from "@bundle:com.cicada.player.demo/entry/ets/player/CicadaPlayer";
import { PlayerControlBar } from "@bundle:com.cicada.player.demo/entry/ets/view/PlayerControlBar";
import { PlayerGestureLayer } from "@bundle:com.cicada.player.demo/entry/ets/view/PlayerGestureLayer";
import { PlayerTrackPanel, TRACK_INDEX_AUTO } from "@bundle:com.cicada.player.demo/entry/ets/view/PlayerTrackPanel";
import { PlayerTipsOverlay } from "@bundle:com.cicada.player.demo/entry/ets/view/PlayerTipsOverlay";
import { PlayerOptionPanel } from "@bundle:com.cicada.player.demo/entry/ets/view/PlayerOptionPanel";
import { MirrorMode, RotateMode, ScaleMode, SourceType, StreamType } from "@bundle:com.cicada.player.demo/entry/ets/model/CicadaTypes";
import type { CicadaPlayerListener, MediaInfo, PlayerRouteParams, StreamInfo } from "@bundle:com.cicada.player.demo/entry/ets/model/CicadaTypes";
import { PLAYER_NAME_KEY, Settings } from "@bundle:com.cicada.player.demo/entry/ets/util/Settings";
import { NetWatchdog } from "@bundle:com.cicada.player.demo/entry/ets/util/NetWatchdog";
import { SnapshotSaver } from "@bundle:com.cicada.player.demo/entry/ets/util/SnapshotSaver";
import type { SnapShotRouteParams } from './SnapShotPage';
import { formatMs, formatSpeed } from "@bundle:com.cicada.player.demo/entry/ets/util/TimeFormater";
import { S } from "@bundle:com.cicada.player.demo/entry/ets/util/Strings";
const DEFAULT_URL: string = 'https://storage.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4';
const MAX_LOG_LINES: number = 60;
class Index extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.controller = new XComponentController();
        this.player = new CicadaPlayer();
        this.surfaceReady = false;
        this.__playerId = new ObservedPropertySimplePU(-1, this, "playerId");
        this.__url = new ObservedPropertySimplePU(DEFAULT_URL, this, "url");
        this.__status = new ObservedPropertySimplePU('idle', this, "status");
        this.__positionMs = new ObservedPropertySimplePU(0, this, "positionMs");
        this.__durationMs = new ObservedPropertySimplePU(0, this, "durationMs");
        this.__bufferedMs = new ObservedPropertySimplePU(0, this, "bufferedMs");
        this.__loading = new ObservedPropertySimplePU(false, this, "loading");
        this.__loadPercent = new ObservedPropertySimplePU(0, this, "loadPercent");
        this.__playing = new ObservedPropertySimplePU(false, this, "playing");
        this.__speedText = new ObservedPropertySimplePU('1.0x', this, "speedText");
        this.__netSpeedText = new ObservedPropertySimplePU('', this, "netSpeedText");
        this.__videoSize = new ObservedPropertySimplePU('-', this, "videoSize");
        this.__logs = new ObservedPropertyObjectPU([], this, "logs");
        this.__playType = new ObservedPropertySimplePU(SourceType.URL, this, "playType");
        this.sourceText = '';
        this.subtitleCn = '';
        this.subtitleEn = '';
        this.autoPlayed = false;
        this.__title = new ObservedPropertySimplePU(S.titlePlayer, this, "title");
        this.__isFullScreen = new ObservedPropertySimplePU(false, this, "isFullScreen");
        this.__locked = new ObservedPropertySimplePU(false, this, "locked");
        this.dragging = false;
        this.__seekPreviewMs = new ObservedPropertySimplePU(0, this, "seekPreviewMs");
        this.__brightnessPercent = new ObservedPropertySimplePU(50, this, "brightnessPercent");
        this.mediaInfo = null;
        this.__showTrackPanel = new ObservedPropertySimplePU(false, this, "showTrackPanel");
        this.__bitrateLabels = new ObservedPropertyObjectPU([], this, "bitrateLabels");
        this.__audioLabels = new ObservedPropertyObjectPU([], this, "audioLabels");
        this.__subtitleLabels = new ObservedPropertyObjectPU([], this, "subtitleLabels");
        this.__externalLabels = new ObservedPropertyObjectPU([], this, "externalLabels");
        this.__selectedBitrate = new ObservedPropertySimplePU(-1, this, "selectedBitrate");
        this.__selectedAudio = new ObservedPropertySimplePU(-1, this, "selectedAudio");
        this.__selectedSubtitle = new ObservedPropertySimplePU(-1, this, "selectedSubtitle");
        this.__selectedExternal = new ObservedPropertySimplePU(-1, this, "selectedExternal");
        this.__speed = new ObservedPropertySimplePU(1.0, this, "speed");
        this.bitrateStreamIndices = [];
        this.audioStreamIndices = [];
        this.subtitleStreamIndices = [];
        this.externalTrackIndices = [];
        this.externalNameList = [];
        this.currentExtIndex = -1;
        this.__errorVisible = new ObservedPropertySimplePU(false, this, "errorVisible");
        this.__errorCode = new ObservedPropertySimplePU(0, this, "errorCode");
        this.__errorMsg = new ObservedPropertySimplePU('', this, "errorMsg");
        this.__replayVisible = new ObservedPropertySimplePU(false, this, "replayVisible");
        this.__netChangeVisible = new ObservedPropertySimplePU(false, this, "netChangeVisible");
        this.pendingAction = 'none';
        this.resumePositionMs = 0;
        this.__showOptionPanel = new ObservedPropertySimplePU(false, this, "showOptionPanel");
        this.__autoPlay = new ObservedPropertySimplePU(false, this, "autoPlay");
        this.__mute = new ObservedPropertySimplePU(false, this, "mute");
        this.__loop = new ObservedPropertySimplePU(false, this, "loop");
        this.__hardware = new ObservedPropertySimplePU(true, this, "hardware");
        this.__accurateSeek = new ObservedPropertySimplePU(false, this, "accurateSeek");
        this.__volumePercent = new ObservedPropertySimplePU(50, this, "volumePercent");
        this.__scaleMode = new ObservedPropertySimplePU(ScaleMode.SM_FIT, this, "scaleMode");
        this.__mirrorMode = new ObservedPropertySimplePU(MirrorMode.MIRROR_MODE_NONE, this, "mirrorMode");
        this.__rotateMode = new ObservedPropertySimplePU(RotateMode.ROTATE_0, this, "rotateMode");
        this.__inSnapShotting = new ObservedPropertySimplePU(false, this, "inSnapShotting");
        this.netWatchdog = new NetWatchdog();
        this.retryTime = 3;
        this.needToRetry = false;
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: Index_Params) {
        if (params.controller !== undefined) {
            this.controller = params.controller;
        }
        if (params.player !== undefined) {
            this.player = params.player;
        }
        if (params.surfaceReady !== undefined) {
            this.surfaceReady = params.surfaceReady;
        }
        if (params.playerId !== undefined) {
            this.playerId = params.playerId;
        }
        if (params.url !== undefined) {
            this.url = params.url;
        }
        if (params.status !== undefined) {
            this.status = params.status;
        }
        if (params.positionMs !== undefined) {
            this.positionMs = params.positionMs;
        }
        if (params.durationMs !== undefined) {
            this.durationMs = params.durationMs;
        }
        if (params.bufferedMs !== undefined) {
            this.bufferedMs = params.bufferedMs;
        }
        if (params.loading !== undefined) {
            this.loading = params.loading;
        }
        if (params.loadPercent !== undefined) {
            this.loadPercent = params.loadPercent;
        }
        if (params.playing !== undefined) {
            this.playing = params.playing;
        }
        if (params.speedText !== undefined) {
            this.speedText = params.speedText;
        }
        if (params.netSpeedText !== undefined) {
            this.netSpeedText = params.netSpeedText;
        }
        if (params.videoSize !== undefined) {
            this.videoSize = params.videoSize;
        }
        if (params.logs !== undefined) {
            this.logs = params.logs;
        }
        if (params.playType !== undefined) {
            this.playType = params.playType;
        }
        if (params.sourceText !== undefined) {
            this.sourceText = params.sourceText;
        }
        if (params.subtitleCn !== undefined) {
            this.subtitleCn = params.subtitleCn;
        }
        if (params.subtitleEn !== undefined) {
            this.subtitleEn = params.subtitleEn;
        }
        if (params.autoPlayed !== undefined) {
            this.autoPlayed = params.autoPlayed;
        }
        if (params.title !== undefined) {
            this.title = params.title;
        }
        if (params.isFullScreen !== undefined) {
            this.isFullScreen = params.isFullScreen;
        }
        if (params.locked !== undefined) {
            this.locked = params.locked;
        }
        if (params.dragging !== undefined) {
            this.dragging = params.dragging;
        }
        if (params.seekPreviewMs !== undefined) {
            this.seekPreviewMs = params.seekPreviewMs;
        }
        if (params.brightnessPercent !== undefined) {
            this.brightnessPercent = params.brightnessPercent;
        }
        if (params.mediaInfo !== undefined) {
            this.mediaInfo = params.mediaInfo;
        }
        if (params.showTrackPanel !== undefined) {
            this.showTrackPanel = params.showTrackPanel;
        }
        if (params.bitrateLabels !== undefined) {
            this.bitrateLabels = params.bitrateLabels;
        }
        if (params.audioLabels !== undefined) {
            this.audioLabels = params.audioLabels;
        }
        if (params.subtitleLabels !== undefined) {
            this.subtitleLabels = params.subtitleLabels;
        }
        if (params.externalLabels !== undefined) {
            this.externalLabels = params.externalLabels;
        }
        if (params.selectedBitrate !== undefined) {
            this.selectedBitrate = params.selectedBitrate;
        }
        if (params.selectedAudio !== undefined) {
            this.selectedAudio = params.selectedAudio;
        }
        if (params.selectedSubtitle !== undefined) {
            this.selectedSubtitle = params.selectedSubtitle;
        }
        if (params.selectedExternal !== undefined) {
            this.selectedExternal = params.selectedExternal;
        }
        if (params.speed !== undefined) {
            this.speed = params.speed;
        }
        if (params.bitrateStreamIndices !== undefined) {
            this.bitrateStreamIndices = params.bitrateStreamIndices;
        }
        if (params.audioStreamIndices !== undefined) {
            this.audioStreamIndices = params.audioStreamIndices;
        }
        if (params.subtitleStreamIndices !== undefined) {
            this.subtitleStreamIndices = params.subtitleStreamIndices;
        }
        if (params.externalTrackIndices !== undefined) {
            this.externalTrackIndices = params.externalTrackIndices;
        }
        if (params.externalNameList !== undefined) {
            this.externalNameList = params.externalNameList;
        }
        if (params.currentExtIndex !== undefined) {
            this.currentExtIndex = params.currentExtIndex;
        }
        if (params.errorVisible !== undefined) {
            this.errorVisible = params.errorVisible;
        }
        if (params.errorCode !== undefined) {
            this.errorCode = params.errorCode;
        }
        if (params.errorMsg !== undefined) {
            this.errorMsg = params.errorMsg;
        }
        if (params.replayVisible !== undefined) {
            this.replayVisible = params.replayVisible;
        }
        if (params.netChangeVisible !== undefined) {
            this.netChangeVisible = params.netChangeVisible;
        }
        if (params.pendingAction !== undefined) {
            this.pendingAction = params.pendingAction;
        }
        if (params.resumePositionMs !== undefined) {
            this.resumePositionMs = params.resumePositionMs;
        }
        if (params.showOptionPanel !== undefined) {
            this.showOptionPanel = params.showOptionPanel;
        }
        if (params.autoPlay !== undefined) {
            this.autoPlay = params.autoPlay;
        }
        if (params.mute !== undefined) {
            this.mute = params.mute;
        }
        if (params.loop !== undefined) {
            this.loop = params.loop;
        }
        if (params.hardware !== undefined) {
            this.hardware = params.hardware;
        }
        if (params.accurateSeek !== undefined) {
            this.accurateSeek = params.accurateSeek;
        }
        if (params.volumePercent !== undefined) {
            this.volumePercent = params.volumePercent;
        }
        if (params.scaleMode !== undefined) {
            this.scaleMode = params.scaleMode;
        }
        if (params.mirrorMode !== undefined) {
            this.mirrorMode = params.mirrorMode;
        }
        if (params.rotateMode !== undefined) {
            this.rotateMode = params.rotateMode;
        }
        if (params.inSnapShotting !== undefined) {
            this.inSnapShotting = params.inSnapShotting;
        }
        if (params.netWatchdog !== undefined) {
            this.netWatchdog = params.netWatchdog;
        }
        if (params.retryTime !== undefined) {
            this.retryTime = params.retryTime;
        }
        if (params.needToRetry !== undefined) {
            this.needToRetry = params.needToRetry;
        }
    }
    updateStateVars(params: Index_Params) {
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__playerId.purgeDependencyOnElmtId(rmElmtId);
        this.__url.purgeDependencyOnElmtId(rmElmtId);
        this.__status.purgeDependencyOnElmtId(rmElmtId);
        this.__positionMs.purgeDependencyOnElmtId(rmElmtId);
        this.__durationMs.purgeDependencyOnElmtId(rmElmtId);
        this.__bufferedMs.purgeDependencyOnElmtId(rmElmtId);
        this.__loading.purgeDependencyOnElmtId(rmElmtId);
        this.__loadPercent.purgeDependencyOnElmtId(rmElmtId);
        this.__playing.purgeDependencyOnElmtId(rmElmtId);
        this.__speedText.purgeDependencyOnElmtId(rmElmtId);
        this.__netSpeedText.purgeDependencyOnElmtId(rmElmtId);
        this.__videoSize.purgeDependencyOnElmtId(rmElmtId);
        this.__logs.purgeDependencyOnElmtId(rmElmtId);
        this.__playType.purgeDependencyOnElmtId(rmElmtId);
        this.__title.purgeDependencyOnElmtId(rmElmtId);
        this.__isFullScreen.purgeDependencyOnElmtId(rmElmtId);
        this.__locked.purgeDependencyOnElmtId(rmElmtId);
        this.__seekPreviewMs.purgeDependencyOnElmtId(rmElmtId);
        this.__brightnessPercent.purgeDependencyOnElmtId(rmElmtId);
        this.__showTrackPanel.purgeDependencyOnElmtId(rmElmtId);
        this.__bitrateLabels.purgeDependencyOnElmtId(rmElmtId);
        this.__audioLabels.purgeDependencyOnElmtId(rmElmtId);
        this.__subtitleLabels.purgeDependencyOnElmtId(rmElmtId);
        this.__externalLabels.purgeDependencyOnElmtId(rmElmtId);
        this.__selectedBitrate.purgeDependencyOnElmtId(rmElmtId);
        this.__selectedAudio.purgeDependencyOnElmtId(rmElmtId);
        this.__selectedSubtitle.purgeDependencyOnElmtId(rmElmtId);
        this.__selectedExternal.purgeDependencyOnElmtId(rmElmtId);
        this.__speed.purgeDependencyOnElmtId(rmElmtId);
        this.__errorVisible.purgeDependencyOnElmtId(rmElmtId);
        this.__errorCode.purgeDependencyOnElmtId(rmElmtId);
        this.__errorMsg.purgeDependencyOnElmtId(rmElmtId);
        this.__replayVisible.purgeDependencyOnElmtId(rmElmtId);
        this.__netChangeVisible.purgeDependencyOnElmtId(rmElmtId);
        this.__showOptionPanel.purgeDependencyOnElmtId(rmElmtId);
        this.__autoPlay.purgeDependencyOnElmtId(rmElmtId);
        this.__mute.purgeDependencyOnElmtId(rmElmtId);
        this.__loop.purgeDependencyOnElmtId(rmElmtId);
        this.__hardware.purgeDependencyOnElmtId(rmElmtId);
        this.__accurateSeek.purgeDependencyOnElmtId(rmElmtId);
        this.__volumePercent.purgeDependencyOnElmtId(rmElmtId);
        this.__scaleMode.purgeDependencyOnElmtId(rmElmtId);
        this.__mirrorMode.purgeDependencyOnElmtId(rmElmtId);
        this.__rotateMode.purgeDependencyOnElmtId(rmElmtId);
        this.__inSnapShotting.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__playerId.aboutToBeDeleted();
        this.__url.aboutToBeDeleted();
        this.__status.aboutToBeDeleted();
        this.__positionMs.aboutToBeDeleted();
        this.__durationMs.aboutToBeDeleted();
        this.__bufferedMs.aboutToBeDeleted();
        this.__loading.aboutToBeDeleted();
        this.__loadPercent.aboutToBeDeleted();
        this.__playing.aboutToBeDeleted();
        this.__speedText.aboutToBeDeleted();
        this.__netSpeedText.aboutToBeDeleted();
        this.__videoSize.aboutToBeDeleted();
        this.__logs.aboutToBeDeleted();
        this.__playType.aboutToBeDeleted();
        this.__title.aboutToBeDeleted();
        this.__isFullScreen.aboutToBeDeleted();
        this.__locked.aboutToBeDeleted();
        this.__seekPreviewMs.aboutToBeDeleted();
        this.__brightnessPercent.aboutToBeDeleted();
        this.__showTrackPanel.aboutToBeDeleted();
        this.__bitrateLabels.aboutToBeDeleted();
        this.__audioLabels.aboutToBeDeleted();
        this.__subtitleLabels.aboutToBeDeleted();
        this.__externalLabels.aboutToBeDeleted();
        this.__selectedBitrate.aboutToBeDeleted();
        this.__selectedAudio.aboutToBeDeleted();
        this.__selectedSubtitle.aboutToBeDeleted();
        this.__selectedExternal.aboutToBeDeleted();
        this.__speed.aboutToBeDeleted();
        this.__errorVisible.aboutToBeDeleted();
        this.__errorCode.aboutToBeDeleted();
        this.__errorMsg.aboutToBeDeleted();
        this.__replayVisible.aboutToBeDeleted();
        this.__netChangeVisible.aboutToBeDeleted();
        this.__showOptionPanel.aboutToBeDeleted();
        this.__autoPlay.aboutToBeDeleted();
        this.__mute.aboutToBeDeleted();
        this.__loop.aboutToBeDeleted();
        this.__hardware.aboutToBeDeleted();
        this.__accurateSeek.aboutToBeDeleted();
        this.__volumePercent.aboutToBeDeleted();
        this.__scaleMode.aboutToBeDeleted();
        this.__mirrorMode.aboutToBeDeleted();
        this.__rotateMode.aboutToBeDeleted();
        this.__inSnapShotting.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private controller: XComponentController;
    private player: CicadaPlayer;
    private surfaceReady: boolean;
    private __playerId: ObservedPropertySimplePU<number>;
    get playerId() {
        return this.__playerId.get();
    }
    set playerId(newValue: number) {
        this.__playerId.set(newValue);
    }
    private __url: ObservedPropertySimplePU<string>;
    get url() {
        return this.__url.get();
    }
    set url(newValue: string) {
        this.__url.set(newValue);
    }
    private __status: ObservedPropertySimplePU<string>;
    get status() {
        return this.__status.get();
    }
    set status(newValue: string) {
        this.__status.set(newValue);
    }
    private __positionMs: ObservedPropertySimplePU<number>;
    get positionMs() {
        return this.__positionMs.get();
    }
    set positionMs(newValue: number) {
        this.__positionMs.set(newValue);
    }
    private __durationMs: ObservedPropertySimplePU<number>;
    get durationMs() {
        return this.__durationMs.get();
    }
    set durationMs(newValue: number) {
        this.__durationMs.set(newValue);
    }
    private __bufferedMs: ObservedPropertySimplePU<number>;
    get bufferedMs() {
        return this.__bufferedMs.get();
    }
    set bufferedMs(newValue: number) {
        this.__bufferedMs.set(newValue);
    }
    private __loading: ObservedPropertySimplePU<boolean>;
    get loading() {
        return this.__loading.get();
    }
    set loading(newValue: boolean) {
        this.__loading.set(newValue);
    }
    private __loadPercent: ObservedPropertySimplePU<number>;
    get loadPercent() {
        return this.__loadPercent.get();
    }
    set loadPercent(newValue: number) {
        this.__loadPercent.set(newValue);
    }
    private __playing: ObservedPropertySimplePU<boolean>;
    get playing() {
        return this.__playing.get();
    }
    set playing(newValue: boolean) {
        this.__playing.set(newValue);
    }
    private __speedText: ObservedPropertySimplePU<string>;
    get speedText() {
        return this.__speedText.get();
    }
    set speedText(newValue: string) {
        this.__speedText.set(newValue);
    }
    private __netSpeedText: ObservedPropertySimplePU<string>;
    get netSpeedText() {
        return this.__netSpeedText.get();
    }
    set netSpeedText(newValue: string) {
        this.__netSpeedText.set(newValue);
    }
    private __videoSize: ObservedPropertySimplePU<string>;
    get videoSize() {
        return this.__videoSize.get();
    }
    set videoSize(newValue: string) {
        this.__videoSize.set(newValue);
    }
    private __logs: ObservedPropertyObjectPU<string[]>;
    get logs() {
        return this.__logs.get();
    }
    set logs(newValue: string[]) {
        this.__logs.set(newValue);
    }
    /** Source handed over by the browse pages (router params). */
    private __playType: ObservedPropertySimplePU<string>;
    get playType() {
        return this.__playType.get();
    }
    set playType(newValue: string) {
        this.__playType.set(newValue);
    }
    private sourceText: string;
    private subtitleCn: string;
    private subtitleEn: string;
    private autoPlayed: boolean;
    /** Control-bar / screen-mode state (mirrors CicadaVodPlayerView). */
    private __title: ObservedPropertySimplePU<string>;
    get title() {
        return this.__title.get();
    }
    set title(newValue: string) {
        this.__title.set(newValue);
    }
    private __isFullScreen: ObservedPropertySimplePU<boolean>;
    get isFullScreen() {
        return this.__isFullScreen.get();
    }
    set isFullScreen(newValue: boolean) {
        this.__isFullScreen.set(newValue);
    }
    private __locked: ObservedPropertySimplePU<boolean>;
    get locked() {
        return this.__locked.get();
    }
    set locked(newValue: boolean) {
        this.__locked.set(newValue);
    }
    private dragging: boolean;
    private __seekPreviewMs: ObservedPropertySimplePU<number>;
    get seekPreviewMs() {
        return this.__seekPreviewMs.get();
    }
    set seekPreviewMs(newValue: number) {
        this.__seekPreviewMs.set(newValue);
    }
    /** Window brightness as 0..100; OHOS has no read-back so we own the value. */
    private __brightnessPercent: ObservedPropertySimplePU<number>;
    get brightnessPercent() {
        return this.__brightnessPercent.get();
    }
    set brightnessPercent(newValue: number) {
        this.__brightnessPercent.set(newValue);
    }
    // ---- track / speed panel (PlayerTrackFragment + the speed group) ----
    private mediaInfo: MediaInfo | null;
    private __showTrackPanel: ObservedPropertySimplePU<boolean>;
    get showTrackPanel() {
        return this.__showTrackPanel.get();
    }
    set showTrackPanel(newValue: boolean) {
        this.__showTrackPanel.set(newValue);
    }
    private __bitrateLabels: ObservedPropertyObjectPU<string[]>;
    get bitrateLabels() {
        return this.__bitrateLabels.get();
    }
    set bitrateLabels(newValue: string[]) {
        this.__bitrateLabels.set(newValue);
    }
    private __audioLabels: ObservedPropertyObjectPU<string[]>;
    get audioLabels() {
        return this.__audioLabels.get();
    }
    set audioLabels(newValue: string[]) {
        this.__audioLabels.set(newValue);
    }
    private __subtitleLabels: ObservedPropertyObjectPU<string[]>;
    get subtitleLabels() {
        return this.__subtitleLabels.get();
    }
    set subtitleLabels(newValue: string[]) {
        this.__subtitleLabels.set(newValue);
    }
    private __externalLabels: ObservedPropertyObjectPU<string[]>;
    get externalLabels() {
        return this.__externalLabels.get();
    }
    set externalLabels(newValue: string[]) {
        this.__externalLabels.set(newValue);
    }
    private __selectedBitrate: ObservedPropertySimplePU<number>;
    get selectedBitrate() {
        return this.__selectedBitrate.get();
    }
    set selectedBitrate(newValue: number) {
        this.__selectedBitrate.set(newValue);
    }
    private __selectedAudio: ObservedPropertySimplePU<number>;
    get selectedAudio() {
        return this.__selectedAudio.get();
    }
    set selectedAudio(newValue: number) {
        this.__selectedAudio.set(newValue);
    }
    private __selectedSubtitle: ObservedPropertySimplePU<number>;
    get selectedSubtitle() {
        return this.__selectedSubtitle.get();
    }
    set selectedSubtitle(newValue: number) {
        this.__selectedSubtitle.set(newValue);
    }
    private __selectedExternal: ObservedPropertySimplePU<number>;
    get selectedExternal() {
        return this.__selectedExternal.get();
    }
    set selectedExternal(newValue: number) {
        this.__selectedExternal.set(newValue);
    }
    private __speed: ObservedPropertySimplePU<number>;
    get speed() {
        return this.__speed.get();
    }
    set speed(newValue: number) {
        this.__speed.set(newValue);
    }
    private bitrateStreamIndices: number[];
    private audioStreamIndices: number[];
    private subtitleStreamIndices: number[];
    private externalTrackIndices: number[];
    private externalNameList: string[];
    private currentExtIndex: number;
    // ---- overlays (TipsView) ----
    private __errorVisible: ObservedPropertySimplePU<boolean>;
    get errorVisible() {
        return this.__errorVisible.get();
    }
    set errorVisible(newValue: boolean) {
        this.__errorVisible.set(newValue);
    }
    private __errorCode: ObservedPropertySimplePU<number>;
    get errorCode() {
        return this.__errorCode.get();
    }
    set errorCode(newValue: number) {
        this.__errorCode.set(newValue);
    }
    private __errorMsg: ObservedPropertySimplePU<string>;
    get errorMsg() {
        return this.__errorMsg.get();
    }
    set errorMsg(newValue: string) {
        this.__errorMsg.set(newValue);
    }
    private __replayVisible: ObservedPropertySimplePU<boolean>;
    get replayVisible() {
        return this.__replayVisible.get();
    }
    set replayVisible(newValue: boolean) {
        this.__replayVisible.set(newValue);
    }
    /**
     * Wired into the overlay but not raised yet: Android's NetWatchdog listens for
     * CONNECTIVITY_ACTION, whose HarmonyOS counterpart (@ohos.net.connection) is
     * not hooked up here.
     */
    private __netChangeVisible: ObservedPropertySimplePU<boolean>;
    get netChangeVisible() {
        return this.__netChangeVisible.get();
    }
    set netChangeVisible(newValue: boolean) {
        this.__netChangeVisible.set(newValue);
    }
    /** What to do once the next prepare() completes (Android continuePlay/isReplay). */
    private pendingAction: string;
    private resumePositionMs: number;
    // ---- option / config / cache panel (PlayerOperationFragment +
    //      PlayerConfigFragment + PlayerCacheConfigFragment) ----
    private __showOptionPanel: ObservedPropertySimplePU<boolean>;
    get showOptionPanel() {
        return this.__showOptionPanel.get();
    }
    set showOptionPanel(newValue: boolean) {
        this.__showOptionPanel.set(newValue);
    }
    private __autoPlay: ObservedPropertySimplePU<boolean>;
    get autoPlay() {
        return this.__autoPlay.get();
    }
    set autoPlay(newValue: boolean) {
        this.__autoPlay.set(newValue);
    }
    private __mute: ObservedPropertySimplePU<boolean>;
    get mute() {
        return this.__mute.get();
    }
    set mute(newValue: boolean) {
        this.__mute.set(newValue);
    }
    private __loop: ObservedPropertySimplePU<boolean>;
    get loop() {
        return this.__loop.get();
    }
    set loop(newValue: boolean) {
        this.__loop.set(newValue);
    }
    private __hardware: ObservedPropertySimplePU<boolean>;
    get hardware() {
        return this.__hardware.get();
    }
    set hardware(newValue: boolean) {
        this.__hardware.set(newValue);
    }
    private __accurateSeek: ObservedPropertySimplePU<boolean>;
    get accurateSeek() {
        return this.__accurateSeek.get();
    }
    set accurateSeek(newValue: boolean) {
        this.__accurateSeek.set(newValue);
    }
    private __volumePercent: ObservedPropertySimplePU<number>;
    get volumePercent() {
        return this.__volumePercent.get();
    }
    set volumePercent(newValue: number) {
        this.__volumePercent.set(newValue);
    }
    private __scaleMode: ObservedPropertySimplePU<number>;
    get scaleMode() {
        return this.__scaleMode.get();
    }
    set scaleMode(newValue: number) {
        this.__scaleMode.set(newValue);
    }
    private __mirrorMode: ObservedPropertySimplePU<number>;
    get mirrorMode() {
        return this.__mirrorMode.get();
    }
    set mirrorMode(newValue: number) {
        this.__mirrorMode.set(newValue);
    }
    private __rotateMode: ObservedPropertySimplePU<number>;
    get rotateMode() {
        return this.__rotateMode.get();
    }
    set rotateMode(newValue: number) {
        this.__rotateMode.set(newValue);
    }
    /** Android's inSnapShotting guard. */
    private __inSnapShotting: ObservedPropertySimplePU<boolean>;
    get inSnapShotting() {
        return this.__inSnapShotting.get();
    }
    set inSnapShotting(newValue: boolean) {
        this.__inSnapShotting.set(newValue);
    }
    // ---- network watchdog (Android's NetWatchdog) ----
    private netWatchdog: NetWatchdog;
    /** Android's mRetryTime / mNeedToRetry. */
    private retryTime: number;
    private needToRetry: boolean;
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
                }
                else if (self.pendingAction === 'replay') {
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
                // Android shows the error tip and auto-unlocks the screen so the user
                // can leave.
                self.errorVisible = true;
                self.errorCode = code;
                self.errorMsg = msg;
                self.locked = false;
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
                }
                else if (url.length > 0 && url === self.subtitleEn) {
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
    private onSurfaceLoad(): void {
        const surfaceId: string = this.controller.getXComponentSurfaceId();
        this.surfaceReady = this.player.setSurface(surfaceId);
        // Diagnostic: on OHOS the software path cannot display anything (there is no
        // video renderer -- renderFactory returns DummyVideoRender), so only hardware
        // surface mode can put pixels on screen. These lines tell us whether the
        // surface was bound at all.
        this.log(`surface ${this.surfaceReady ? 'ready' : 'FAILED'} id=${surfaceId}`);
        // A source arrived from the browse pages: start it as soon as the surface
        // is available.
        if (this.surfaceReady && !this.autoPlayed && this.sourceText.length > 0) {
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
        }
        else {
            this.player.setDataSource(this.url);
        }
        this.player.prepare();
        this.player.start();
        this.status = 'playing';
        this.playing = true;
        // Diagnostic: 0 = hardware requested, 1 = software requested. On OHOS the
        // software path has no video renderer, so a 1 here means no picture is
        // possible; check the 设置 -> 操作 panel's 硬解码 switch.
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
    private seekBy(deltaMs: number): void {
        const target: number = Math.max(0, this.positionMs + deltaMs);
        this.player.seekTo(target, true);
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
    private applySpeed(speed: number): void {
        this.player.setSpeed(speed);
        this.speedText = `${speed.toFixed(1)}x`;
    }
    private switchQuality(): void {
        const video: StreamInfo | null = this.player.getCurrentStreamInfo(StreamType.ST_TYPE_VIDEO);
        if (video === null) {
            this.log('no current video stream');
            return;
        }
        // Cycles to the next stream index; the real demo lists them in a panel.
        this.player.selectTrack(video.streamIndex + 1);
    }
    logList(parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            List.create({ space: 2 });
            List.debugLine("entry/src/main/ets/pages/Index.ets(431:5)", "entry");
            List.width('100%');
            List.layoutWeight(1);
            List.backgroundColor('#111111');
            List.padding(6);
        }, List);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            ForEach.create();
            const forEachItemGenFunction = _item => {
                const line = _item;
                {
                    const itemCreation = (elmtId, isInitialRender) => {
                        ViewStackProcessor.StartGetAccessRecordingFor(elmtId);
                        ListItem.create(deepRenderFunction, true);
                        if (!isInitialRender) {
                            ListItem.pop();
                        }
                        ViewStackProcessor.StopGetAccessRecording();
                    };
                    const itemCreation2 = (elmtId, isInitialRender) => {
                        ListItem.create(deepRenderFunction, true);
                        ListItem.debugLine("entry/src/main/ets/pages/Index.ets(433:9)", "entry");
                    };
                    const deepRenderFunction = (elmtId, isInitialRender) => {
                        itemCreation(elmtId, isInitialRender);
                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                            Text.create(line);
                            Text.debugLine("entry/src/main/ets/pages/Index.ets(434:11)", "entry");
                            Text.fontSize(11);
                            Text.fontColor('#B0BEC5');
                            Text.width('100%');
                        }, Text);
                        Text.pop();
                        ListItem.pop();
                    };
                    this.observeComponentCreation2(itemCreation2, ListItem);
                    ListItem.pop();
                }
            };
            this.forEachUpdateFunction(elmtId, this.logs, forEachItemGenFunction, (line: string, index: number) => `${index}-${line}`, false, true);
        }, ForEach);
        ForEach.pop();
        List.pop();
    }
    /**
     * Applies the left-half vertical gesture. Android writes
     * activity.window.attributes.screenBrightness and skips the write when the
     * target is <= 0 (a Samsung lock-screen guard); mirrored here.
     */
    private async applyBrightness(percent: number): Promise<void> {
        this.brightnessPercent = percent;
        if (percent <= 0) {
            return;
        }
        try {
            const win = await window.getLastWindow(getContext(this));
            await win.setWindowBrightness(percent / 100);
        }
        catch (err) {
            this.log('setWindowBrightness failed');
        }
    }
    /** Double-tap: Android's switchPlayerState(). */
    private togglePlay(): void {
        if (this.playing) {
            this.pause();
        }
        else {
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
            }
            else if (stream.type === 'audio' && stream.audioLang.length > 0) {
                audios.push(stream.audioLang);
                aIdx.push(stream.streamIndex);
            }
            else if (stream.type === 'subtitle' && stream.subtitleLang.length > 0) {
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
        this.applySpeed(value);
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
        }
        else {
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
        // Android re-sets the data source when the player is idle/stopped.
        if (this.player.isPrepared()) {
            this.resume();
        }
        else {
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
     */
    private onApplyConfig(startBuffer: string, highBuffer: string, maxBuffer: string, maxDelay: string, netTimeout: string, probeSize: string, referrer: string, httpProxy: string, retryCount: string, clearFrameWhenStop: boolean): void {
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
        const sizeMB: number = maxSizeMB.length > 0 ? Number.parseInt(maxSizeMB) : 200;
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
            await win.setPreferredOrientation(this.isFullScreen ? window.Orientation.LANDSCAPE : window.Orientation.PORTRAIT);
        }
        catch (err) {
            this.log('setPreferredOrientation failed');
        }
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 8 });
            Column.debugLine("entry/src/main/ets/pages/Index.ets(726:5)", "entry");
            Column.width('100%');
            Column.height('100%');
            Column.padding(this.isFullScreen ? 0 : 10);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Stack.create();
            Stack.debugLine("entry/src/main/ets/pages/Index.ets(727:7)", "entry");
            Stack.width('100%');
            Stack.height(this.isFullScreen ? '100%' : 240);
        }, Stack);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            XComponent.create({
                id: 'videoSurface',
                type: XComponentType.SURFACE,
                controller: this.controller
            }, "com.cicada.player.demo/entry");
            XComponent.debugLine("entry/src/main/ets/pages/Index.ets(728:9)", "entry");
            XComponent.onLoad(() => this.onSurfaceLoad());
            XComponent.width('100%');
            XComponent.height('100%');
            XComponent.backgroundColor(Color.Black);
        }, XComponent);
        {
            this.observeComponentCreation2((elmtId, isInitialRender) => {
                if (isInitialRender) {
                    let componentCall = new 
                    // The gesture layer sits ABOVE the surface and BELOW the control bar,
                    // matching CicadaVodPlayerView's child order (GestureView #5,
                    // ControlView #6).
                    PlayerGestureLayer(this, {
                        locked: this.locked,
                        durationMs: this.durationMs,
                        positionMs: this.positionMs,
                        getVolumePercent: () => Math.trunc(this.player.getVolume() * 100),
                        getBrightnessPercent: () => this.brightnessPercent,
                        onBrightness: (percent: number) => {
                            this.applyBrightness(percent);
                        },
                        onVolume: (percent: number) => {
                            this.player.setVolume(percent / 100);
                        },
                        onSeekTo: (ms: number) => {
                            this.player.seekTo(ms, false);
                        },
                        onTogglePlay: () => this.togglePlay()
                    }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/Index.ets", line: 741, col: 9 });
                    ViewPU.create(componentCall);
                    let paramsLambda = () => {
                        return {
                            locked: this.locked,
                            durationMs: this.durationMs,
                            positionMs: this.positionMs,
                            getVolumePercent: () => Math.trunc(this.player.getVolume() * 100),
                            getBrightnessPercent: () => this.brightnessPercent,
                            onBrightness: (percent: number) => {
                                this.applyBrightness(percent);
                            },
                            onVolume: (percent: number) => {
                                this.player.setVolume(percent / 100);
                            },
                            onSeekTo: (ms: number) => {
                                this.player.seekTo(ms, false);
                            },
                            onTogglePlay: () => this.togglePlay()
                        };
                    };
                    componentCall.paramsGenerator_ = paramsLambda;
                }
                else {
                    this.updateStateVarsOfChildByElmtId(elmtId, {
                        locked: this.locked,
                        durationMs: this.durationMs,
                        positionMs: this.positionMs
                    });
                }
            }, { name: "PlayerGestureLayer" });
        }
        {
            this.observeComponentCreation2((elmtId, isInitialRender) => {
                if (isInitialRender) {
                    let componentCall = new PlayerControlBar(this, {
                        title: this.title,
                        isFullScreen: this.isFullScreen,
                        locked: this.locked,
                        playing: this.playing,
                        durationMs: this.durationMs,
                        bufferMs: this.bufferedMs,
                        positionMs: this.dragging ? this.seekPreviewMs : this.positionMs,
                        onBack: () => this.onBack(),
                        onToggleScreenMode: () => {
                            this.toggleScreenMode();
                        },
                        onToggleLock: () => {
                            this.locked = !this.locked;
                        },
                        onSeekStart: () => {
                            this.dragging = true;
                            this.seekPreviewMs = this.positionMs;
                        },
                        onSeekChange: (ms: number) => {
                            this.seekPreviewMs = ms;
                        },
                        onSeekEnd: (ms: number) => {
                            this.dragging = false;
                            // The demo's "Accurate Seek" switch defaults to off.
                            this.player.seekTo(ms, false);
                        }
                    }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/Index.ets", line: 759, col: 9 });
                    ViewPU.create(componentCall);
                    let paramsLambda = () => {
                        return {
                            title: this.title,
                            isFullScreen: this.isFullScreen,
                            locked: this.locked,
                            playing: this.playing,
                            durationMs: this.durationMs,
                            bufferMs: this.bufferedMs,
                            positionMs: this.dragging ? this.seekPreviewMs : this.positionMs,
                            onBack: () => this.onBack(),
                            onToggleScreenMode: () => {
                                this.toggleScreenMode();
                            },
                            onToggleLock: () => {
                                this.locked = !this.locked;
                            },
                            onSeekStart: () => {
                                this.dragging = true;
                                this.seekPreviewMs = this.positionMs;
                            },
                            onSeekChange: (ms: number) => {
                                this.seekPreviewMs = ms;
                            },
                            onSeekEnd: (ms: number) => {
                                this.dragging = false;
                                // The demo's "Accurate Seek" switch defaults to off.
                                this.player.seekTo(ms, false);
                            }
                        };
                    };
                    componentCall.paramsGenerator_ = paramsLambda;
                }
                else {
                    this.updateStateVarsOfChildByElmtId(elmtId, {
                        title: this.title,
                        isFullScreen: this.isFullScreen,
                        locked: this.locked,
                        playing: this.playing,
                        durationMs: this.durationMs,
                        bufferMs: this.bufferedMs,
                        positionMs: this.dragging ? this.seekPreviewMs : this.positionMs
                    });
                }
            }, { name: "PlayerControlBar" });
        }
        {
            this.observeComponentCreation2((elmtId, isInitialRender) => {
                if (isInitialRender) {
                    let componentCall = new 
                    // Overlays sit above everything (CicadaVodPlayerView child #9).
                    PlayerTipsOverlay(this, {
                        loading: this.loading,
                        loadPercent: this.loadPercent,
                        errorVisible: this.errorVisible,
                        errorCode: this.errorCode,
                        errorMsg: this.errorMsg,
                        replayVisible: this.replayVisible,
                        netChangeVisible: this.netChangeVisible,
                        onRetry: () => this.reTry(),
                        onReplay: () => this.rePlay(),
                        onContinuePlay: () => this.onContinuePlay(),
                        onStopPlay: () => this.onStopPlay()
                    }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/Index.ets", line: 789, col: 9 });
                    ViewPU.create(componentCall);
                    let paramsLambda = () => {
                        return {
                            loading: this.loading,
                            loadPercent: this.loadPercent,
                            errorVisible: this.errorVisible,
                            errorCode: this.errorCode,
                            errorMsg: this.errorMsg,
                            replayVisible: this.replayVisible,
                            netChangeVisible: this.netChangeVisible,
                            onRetry: () => this.reTry(),
                            onReplay: () => this.rePlay(),
                            onContinuePlay: () => this.onContinuePlay(),
                            onStopPlay: () => this.onStopPlay()
                        };
                    };
                    componentCall.paramsGenerator_ = paramsLambda;
                }
                else {
                    this.updateStateVarsOfChildByElmtId(elmtId, {
                        loading: this.loading,
                        loadPercent: this.loadPercent,
                        errorVisible: this.errorVisible,
                        errorCode: this.errorCode,
                        errorMsg: this.errorMsg,
                        replayVisible: this.replayVisible,
                        netChangeVisible: this.netChangeVisible
                    });
                }
            }, { name: "PlayerTipsOverlay" });
        }
        Stack.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (!this.isFullScreen) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Row.create({ space: 6 });
                        Row.debugLine("entry/src/main/ets/pages/Index.ets(807:9)", "entry");
                        Row.width('100%');
                    }, Row);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(`${formatMs(this.positionMs)} | ${this.videoSize}`);
                        Text.debugLine("entry/src/main/ets/pages/Index.ets(808:11)", "entry");
                        Text.fontSize(12);
                        Text.width(220);
                    }, Text);
                    Text.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(this.netSpeedText);
                        Text.debugLine("entry/src/main/ets/pages/Index.ets(811:11)", "entry");
                        Text.fontSize(12);
                        Text.layoutWeight(1);
                    }, Text);
                    Text.pop();
                    Row.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        TextInput.create({ text: this.url, placeholder: 'media url' });
                        TextInput.debugLine("entry/src/main/ets/pages/Index.ets(815:9)", "entry");
                        TextInput.fontSize(12);
                        TextInput.width('100%');
                        TextInput.onChange((value: string) => {
                            this.url = value;
                        });
                    }, TextInput);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Row.create({ space: 6 });
                        Row.debugLine("entry/src/main/ets/pages/Index.ets(822:9)", "entry");
                        Row.width('100%');
                    }, Row);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Button.createWithLabel('播放');
                        Button.debugLine("entry/src/main/ets/pages/Index.ets(823:11)", "entry");
                        Button.onClick(() => this.play());
                    }, Button);
                    Button.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Button.createWithLabel(this.playing ? '暂停' : '继续');
                        Button.debugLine("entry/src/main/ets/pages/Index.ets(824:11)", "entry");
                        Button.onClick(() => {
                            if (this.playing) {
                                this.pause();
                            }
                            else {
                                this.resume();
                            }
                        });
                    }, Button);
                    Button.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Button.createWithLabel('停止');
                        Button.debugLine("entry/src/main/ets/pages/Index.ets(832:11)", "entry");
                        Button.onClick(() => this.stopPlayback());
                    }, Button);
                    Button.pop();
                    Row.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Row.create({ space: 6 });
                        Row.debugLine("entry/src/main/ets/pages/Index.ets(836:9)", "entry");
                        Row.width('100%');
                    }, Row);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Button.createWithLabel('轨道');
                        Button.debugLine("entry/src/main/ets/pages/Index.ets(837:11)", "entry");
                        Button.onClick(() => {
                            this.showTrackPanel = !this.showTrackPanel;
                            this.showOptionPanel = false;
                        });
                    }, Button);
                    Button.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Button.createWithLabel('设置');
                        Button.debugLine("entry/src/main/ets/pages/Index.ets(841:11)", "entry");
                        Button.onClick(() => {
                            this.showOptionPanel = !this.showOptionPanel;
                            this.showTrackPanel = false;
                        });
                    }, Button);
                    Button.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Button.createWithLabel('截图');
                        Button.debugLine("entry/src/main/ets/pages/Index.ets(845:11)", "entry");
                        Button.onClick(() => this.shoot());
                    }, Button);
                    Button.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Button.createWithLabel('切流');
                        Button.debugLine("entry/src/main/ets/pages/Index.ets(846:11)", "entry");
                        Button.onClick(() => this.switchQuality());
                    }, Button);
                    Button.pop();
                    Row.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        If.create();
                        if (this.showOptionPanel) {
                            this.ifElseBranchUpdateFunction(0, () => {
                                this.observeComponentCreation2((elmtId, isInitialRender) => {
                                    __Common__.create();
                                    __Common__.height(300);
                                }, __Common__);
                                {
                                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                                        if (isInitialRender) {
                                            let componentCall = new PlayerOptionPanel(this, {
                                                autoPlay: this.autoPlay,
                                                mute: this.mute,
                                                loop: this.loop,
                                                hardware: this.hardware,
                                                accurateSeek: this.accurateSeek,
                                                volumePercent: this.volumePercent,
                                                scaleMode: this.scaleMode,
                                                mirrorMode: this.mirrorMode,
                                                rotateMode: this.rotateMode,
                                                onAutoPlayChange: (v: boolean) => {
                                                    this.autoPlay = v;
                                                    this.player.setAutoPlay(v);
                                                },
                                                onMuteChange: (v: boolean) => {
                                                    this.mute = v;
                                                    this.player.setMute(v);
                                                },
                                                onLoopChange: (v: boolean) => {
                                                    this.loop = v;
                                                    this.player.setLoop(v);
                                                },
                                                onHardwareChange: (v: boolean) => {
                                                    this.hardware = v;
                                                    this.player.enableHardwareDecoder(v);
                                                    Settings.setHardwareDecoder(v);
                                                    promptAction.showToast({
                                                        message: v ? S.hardwareOnNextPlay : S.hardwareOffNextPlay,
                                                        duration: 2000
                                                    });
                                                },
                                                onAccurateSeekChange: (v: boolean) => {
                                                    this.accurateSeek = v;
                                                },
                                                onVolumeChange: (percent: number) => {
                                                    this.volumePercent = percent;
                                                    // Android maps slider progress/50 onto a 0..2.0 volume range.
                                                    this.player.setVolume(percent / 50);
                                                },
                                                onScaleModeChange: (mode: number) => {
                                                    this.scaleMode = mode;
                                                    this.player.setScaleMode(mode as ScaleMode);
                                                },
                                                onMirrorModeChange: (mode: number) => {
                                                    this.mirrorMode = mode;
                                                    this.player.setMirrorMode(mode as MirrorMode);
                                                },
                                                onRotateModeChange: (mode: number) => {
                                                    this.rotateMode = mode;
                                                    this.player.setRotateMode(mode as RotateMode);
                                                },
                                                onShowMediaInfo: () => this.showMediaInfo(),
                                                onApplyConfig: (startBuffer: string, highBuffer: string, maxBuffer: string, maxDelay: string, netTimeout: string, probeSize: string, referrer: string, httpProxy: string, retryCount: string, clearFrameWhenStop: boolean) => {
                                                    this.onApplyConfig(startBuffer, highBuffer, maxBuffer, maxDelay, netTimeout, probeSize, referrer, httpProxy, retryCount, clearFrameWhenStop);
                                                },
                                                onApplyCacheConfig: (enable: boolean, maxDurationS: string, maxSizeMB: string, dir: string) => {
                                                    this.onApplyCacheConfig(enable, maxDurationS, maxSizeMB, dir);
                                                }
                                            }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/Index.ets", line: 851, col: 11 });
                                            ViewPU.create(componentCall);
                                            let paramsLambda = () => {
                                                return {
                                                    autoPlay: this.autoPlay,
                                                    mute: this.mute,
                                                    loop: this.loop,
                                                    hardware: this.hardware,
                                                    accurateSeek: this.accurateSeek,
                                                    volumePercent: this.volumePercent,
                                                    scaleMode: this.scaleMode,
                                                    mirrorMode: this.mirrorMode,
                                                    rotateMode: this.rotateMode,
                                                    onAutoPlayChange: (v: boolean) => {
                                                        this.autoPlay = v;
                                                        this.player.setAutoPlay(v);
                                                    },
                                                    onMuteChange: (v: boolean) => {
                                                        this.mute = v;
                                                        this.player.setMute(v);
                                                    },
                                                    onLoopChange: (v: boolean) => {
                                                        this.loop = v;
                                                        this.player.setLoop(v);
                                                    },
                                                    onHardwareChange: (v: boolean) => {
                                                        this.hardware = v;
                                                        this.player.enableHardwareDecoder(v);
                                                        Settings.setHardwareDecoder(v);
                                                        promptAction.showToast({
                                                            message: v ? S.hardwareOnNextPlay : S.hardwareOffNextPlay,
                                                            duration: 2000
                                                        });
                                                    },
                                                    onAccurateSeekChange: (v: boolean) => {
                                                        this.accurateSeek = v;
                                                    },
                                                    onVolumeChange: (percent: number) => {
                                                        this.volumePercent = percent;
                                                        // Android maps slider progress/50 onto a 0..2.0 volume range.
                                                        this.player.setVolume(percent / 50);
                                                    },
                                                    onScaleModeChange: (mode: number) => {
                                                        this.scaleMode = mode;
                                                        this.player.setScaleMode(mode as ScaleMode);
                                                    },
                                                    onMirrorModeChange: (mode: number) => {
                                                        this.mirrorMode = mode;
                                                        this.player.setMirrorMode(mode as MirrorMode);
                                                    },
                                                    onRotateModeChange: (mode: number) => {
                                                        this.rotateMode = mode;
                                                        this.player.setRotateMode(mode as RotateMode);
                                                    },
                                                    onShowMediaInfo: () => this.showMediaInfo(),
                                                    onApplyConfig: (startBuffer: string, highBuffer: string, maxBuffer: string, maxDelay: string, netTimeout: string, probeSize: string, referrer: string, httpProxy: string, retryCount: string, clearFrameWhenStop: boolean) => {
                                                        this.onApplyConfig(startBuffer, highBuffer, maxBuffer, maxDelay, netTimeout, probeSize, referrer, httpProxy, retryCount, clearFrameWhenStop);
                                                    },
                                                    onApplyCacheConfig: (enable: boolean, maxDurationS: string, maxSizeMB: string, dir: string) => {
                                                        this.onApplyCacheConfig(enable, maxDurationS, maxSizeMB, dir);
                                                    }
                                                };
                                            };
                                            componentCall.paramsGenerator_ = paramsLambda;
                                        }
                                        else {
                                            this.updateStateVarsOfChildByElmtId(elmtId, {
                                                autoPlay: this.autoPlay,
                                                mute: this.mute,
                                                loop: this.loop,
                                                hardware: this.hardware,
                                                accurateSeek: this.accurateSeek,
                                                volumePercent: this.volumePercent,
                                                scaleMode: this.scaleMode,
                                                mirrorMode: this.mirrorMode,
                                                rotateMode: this.rotateMode
                                            });
                                        }
                                    }, { name: "PlayerOptionPanel" });
                                }
                                __Common__.pop();
                            });
                        }
                        else {
                            this.ifElseBranchUpdateFunction(1, () => {
                            });
                        }
                    }, If);
                    If.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        If.create();
                        if (this.showTrackPanel) {
                            this.ifElseBranchUpdateFunction(0, () => {
                                this.observeComponentCreation2((elmtId, isInitialRender) => {
                                    __Common__.create();
                                    __Common__.height(260);
                                }, __Common__);
                                {
                                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                                        if (isInitialRender) {
                                            let componentCall = new PlayerTrackPanel(this, {
                                                bitrateLabels: this.bitrateLabels,
                                                audioLabels: this.audioLabels,
                                                subtitleLabels: this.subtitleLabels,
                                                externalLabels: this.externalLabels,
                                                selectedBitrate: this.selectedBitrate,
                                                selectedAudio: this.selectedAudio,
                                                selectedSubtitle: this.selectedSubtitle,
                                                selectedExternal: this.selectedExternal,
                                                speed: this.speed,
                                                onSelectBitrate: (position: number) => this.onSelectBitrate(position),
                                                onSelectAudio: (position: number) => this.onSelectAudio(position),
                                                onSelectSubtitle: (position: number) => this.onSelectSubtitle(position),
                                                onSelectExternal: (position: number) => this.onSelectExternal(position),
                                                onSpeedChange: (value: number) => this.onSpeedChange(value)
                                            }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/Index.ets", line: 917, col: 11 });
                                            ViewPU.create(componentCall);
                                            let paramsLambda = () => {
                                                return {
                                                    bitrateLabels: this.bitrateLabels,
                                                    audioLabels: this.audioLabels,
                                                    subtitleLabels: this.subtitleLabels,
                                                    externalLabels: this.externalLabels,
                                                    selectedBitrate: this.selectedBitrate,
                                                    selectedAudio: this.selectedAudio,
                                                    selectedSubtitle: this.selectedSubtitle,
                                                    selectedExternal: this.selectedExternal,
                                                    speed: this.speed,
                                                    onSelectBitrate: (position: number) => this.onSelectBitrate(position),
                                                    onSelectAudio: (position: number) => this.onSelectAudio(position),
                                                    onSelectSubtitle: (position: number) => this.onSelectSubtitle(position),
                                                    onSelectExternal: (position: number) => this.onSelectExternal(position),
                                                    onSpeedChange: (value: number) => this.onSpeedChange(value)
                                                };
                                            };
                                            componentCall.paramsGenerator_ = paramsLambda;
                                        }
                                        else {
                                            this.updateStateVarsOfChildByElmtId(elmtId, {
                                                bitrateLabels: this.bitrateLabels,
                                                audioLabels: this.audioLabels,
                                                subtitleLabels: this.subtitleLabels,
                                                externalLabels: this.externalLabels,
                                                selectedBitrate: this.selectedBitrate,
                                                selectedAudio: this.selectedAudio,
                                                selectedSubtitle: this.selectedSubtitle,
                                                selectedExternal: this.selectedExternal,
                                                speed: this.speed
                                            });
                                        }
                                    }, { name: "PlayerTrackPanel" });
                                }
                                __Common__.pop();
                            });
                        }
                        else {
                            this.ifElseBranchUpdateFunction(1, () => {
                            });
                        }
                    }, If);
                    If.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(this.loading ? `缓冲中 ${this.loadPercent}%` : this.status);
                        Text.debugLine("entry/src/main/ets/pages/Index.ets(936:9)", "entry");
                        Text.fontSize(12);
                        Text.fontColor('#FF9800');
                        Text.width('100%');
                    }, Text);
                    Text.pop();
                    this.logList.bind(this)();
                });
            }
            else {
                this.ifElseBranchUpdateFunction(1, () => {
                });
            }
        }, If);
        If.pop();
        Column.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    static getEntryName(): string {
        return "Index";
    }
}
registerNamedRoute(() => new Index(undefined, {}), "", { bundleName: "com.cicada.player.demo", moduleName: "entry", pagePath: "pages/Index", pageFullPath: "entry/src/main/ets/pages/Index", integratedHsp: "false", moduleType: "followWithHap" });
