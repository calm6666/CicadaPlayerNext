if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface PlayerOptionPanel_Params {
    autoPlay?: boolean;
    mute?: boolean;
    loop?: boolean;
    hardware?: boolean;
    accurateSeek?: boolean;
    volumePercent?: number;
    scaleMode?: number;
    mirrorMode?: number;
    rotateMode?: number;
    initialStartBuffer?: string;
    initialHighBuffer?: string;
    initialMaxBuffer?: string;
    initialMaxDelay?: string;
    initialNetTimeout?: string;
    initialProbeSize?: string;
    initialReferrer?: string;
    initialHttpProxy?: string;
    initialRetryCount?: string;
    onAutoPlayChange?: (value: boolean) => void;
    onMuteChange?: (value: boolean) => void;
    onLoopChange?: (value: boolean) => void;
    onHardwareChange?: (value: boolean) => void;
    onAccurateSeekChange?: (value: boolean) => void;
    onVolumeChange?: (percent: number) => void;
    onScaleModeChange?: (mode: number) => void;
    onMirrorModeChange?: (mode: number) => void;
    onRotateModeChange?: (mode: number) => void;
    onShowMediaInfo?: () => void;
    onApplyConfig?: (startBuffer: string, highBuffer: string, maxBuffer: string, maxDelay: string, netTimeout: string, probeSize: string, referrer: string, httpProxy: string, retryCount: string, clearFrameWhenStop: boolean) => void;
    onApplyCacheConfig?: (enable: boolean, maxDurationS: string, maxSizeMB: string, dir: string) => void;
    tab?: number;
    startBuffer?: string;
    highBuffer?: string;
    maxBuffer?: string;
    maxDelay?: string;
    netTimeout?: string;
    probeSize?: string;
    referrer?: string;
    httpProxy?: string;
    retryCount?: string;
    clearFrameWhenStop?: boolean;
    cacheEnable?: boolean;
    cacheMaxDurationS?: string;
    cacheMaxSizeMB?: string;
    cacheDir?: string;
}
export class PlayerOptionPanel extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__autoPlay = new SynchedPropertySimpleOneWayPU(params.autoPlay, this, "autoPlay");
        this.__mute = new SynchedPropertySimpleOneWayPU(params.mute, this, "mute");
        this.__loop = new SynchedPropertySimpleOneWayPU(params.loop, this, "loop");
        this.__hardware = new SynchedPropertySimpleOneWayPU(params.hardware, this, "hardware");
        this.__accurateSeek = new SynchedPropertySimpleOneWayPU(params.accurateSeek, this, "accurateSeek");
        this.__volumePercent = new SynchedPropertySimpleOneWayPU(params.volumePercent, this, "volumePercent");
        this.__scaleMode = new SynchedPropertySimpleOneWayPU(params.scaleMode, this, "scaleMode");
        this.__mirrorMode = new SynchedPropertySimpleOneWayPU(params.mirrorMode, this, "mirrorMode");
        this.__rotateMode = new SynchedPropertySimpleOneWayPU(params.rotateMode, this, "rotateMode");
        this.__initialStartBuffer = new SynchedPropertySimpleOneWayPU(params.initialStartBuffer, this, "initialStartBuffer");
        this.__initialHighBuffer = new SynchedPropertySimpleOneWayPU(params.initialHighBuffer, this, "initialHighBuffer");
        this.__initialMaxBuffer = new SynchedPropertySimpleOneWayPU(params.initialMaxBuffer, this, "initialMaxBuffer");
        this.__initialMaxDelay = new SynchedPropertySimpleOneWayPU(params.initialMaxDelay, this, "initialMaxDelay");
        this.__initialNetTimeout = new SynchedPropertySimpleOneWayPU(params.initialNetTimeout, this, "initialNetTimeout");
        this.__initialProbeSize = new SynchedPropertySimpleOneWayPU(params.initialProbeSize, this, "initialProbeSize");
        this.__initialReferrer = new SynchedPropertySimpleOneWayPU(params.initialReferrer, this, "initialReferrer");
        this.__initialHttpProxy = new SynchedPropertySimpleOneWayPU(params.initialHttpProxy, this, "initialHttpProxy");
        this.__initialRetryCount = new SynchedPropertySimpleOneWayPU(params.initialRetryCount, this, "initialRetryCount");
        this.onAutoPlayChange = () => { };
        this.onMuteChange = () => { };
        this.onLoopChange = () => { };
        this.onHardwareChange = () => { };
        this.onAccurateSeekChange = () => { };
        this.onVolumeChange = () => { };
        this.onScaleModeChange = () => { };
        this.onMirrorModeChange = () => { };
        this.onRotateModeChange = () => { };
        this.onShowMediaInfo = () => { };
        this.onApplyConfig = () => { };
        this.onApplyCacheConfig = () => { };
        this.__tab = new ObservedPropertySimplePU(0, this, "tab");
        this.__startBuffer = new ObservedPropertySimplePU('', this, "startBuffer");
        this.__highBuffer = new ObservedPropertySimplePU('', this, "highBuffer");
        this.__maxBuffer = new ObservedPropertySimplePU('', this, "maxBuffer");
        this.__maxDelay = new ObservedPropertySimplePU('', this, "maxDelay");
        this.__netTimeout = new ObservedPropertySimplePU('', this, "netTimeout");
        this.__probeSize = new ObservedPropertySimplePU('', this, "probeSize");
        this.__referrer = new ObservedPropertySimplePU('', this, "referrer");
        this.__httpProxy = new ObservedPropertySimplePU('', this, "httpProxy");
        this.__retryCount = new ObservedPropertySimplePU('2', this, "retryCount");
        this.__clearFrameWhenStop = new ObservedPropertySimplePU(false, this, "clearFrameWhenStop");
        this.__cacheEnable = new ObservedPropertySimplePU(false, this, "cacheEnable");
        this.__cacheMaxDurationS = new ObservedPropertySimplePU('300', this, "cacheMaxDurationS");
        this.__cacheMaxSizeMB = new ObservedPropertySimplePU('200', this, "cacheMaxSizeMB");
        this.__cacheDir = new ObservedPropertySimplePU('', this, "cacheDir");
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: PlayerOptionPanel_Params) {
        if (params.autoPlay === undefined) {
            this.__autoPlay.set(false);
        }
        if (params.mute === undefined) {
            this.__mute.set(false);
        }
        if (params.loop === undefined) {
            this.__loop.set(false);
        }
        if (params.hardware === undefined) {
            this.__hardware.set(true);
        }
        if (params.accurateSeek === undefined) {
            this.__accurateSeek.set(false);
        }
        if (params.volumePercent === undefined) {
            this.__volumePercent.set(50);
        }
        if (params.scaleMode === undefined) {
            this.__scaleMode.set(0);
        }
        if (params.mirrorMode === undefined) {
            this.__mirrorMode.set(0);
        }
        if (params.rotateMode === undefined) {
            this.__rotateMode.set(0);
        }
        if (params.initialStartBuffer === undefined) {
            this.__initialStartBuffer.set('');
        }
        if (params.initialHighBuffer === undefined) {
            this.__initialHighBuffer.set('');
        }
        if (params.initialMaxBuffer === undefined) {
            this.__initialMaxBuffer.set('');
        }
        if (params.initialMaxDelay === undefined) {
            this.__initialMaxDelay.set('');
        }
        if (params.initialNetTimeout === undefined) {
            this.__initialNetTimeout.set('');
        }
        if (params.initialProbeSize === undefined) {
            this.__initialProbeSize.set('');
        }
        if (params.initialReferrer === undefined) {
            this.__initialReferrer.set('');
        }
        if (params.initialHttpProxy === undefined) {
            this.__initialHttpProxy.set('');
        }
        if (params.initialRetryCount === undefined) {
            this.__initialRetryCount.set('2');
        }
        if (params.onAutoPlayChange !== undefined) {
            this.onAutoPlayChange = params.onAutoPlayChange;
        }
        if (params.onMuteChange !== undefined) {
            this.onMuteChange = params.onMuteChange;
        }
        if (params.onLoopChange !== undefined) {
            this.onLoopChange = params.onLoopChange;
        }
        if (params.onHardwareChange !== undefined) {
            this.onHardwareChange = params.onHardwareChange;
        }
        if (params.onAccurateSeekChange !== undefined) {
            this.onAccurateSeekChange = params.onAccurateSeekChange;
        }
        if (params.onVolumeChange !== undefined) {
            this.onVolumeChange = params.onVolumeChange;
        }
        if (params.onScaleModeChange !== undefined) {
            this.onScaleModeChange = params.onScaleModeChange;
        }
        if (params.onMirrorModeChange !== undefined) {
            this.onMirrorModeChange = params.onMirrorModeChange;
        }
        if (params.onRotateModeChange !== undefined) {
            this.onRotateModeChange = params.onRotateModeChange;
        }
        if (params.onShowMediaInfo !== undefined) {
            this.onShowMediaInfo = params.onShowMediaInfo;
        }
        if (params.onApplyConfig !== undefined) {
            this.onApplyConfig = params.onApplyConfig;
        }
        if (params.onApplyCacheConfig !== undefined) {
            this.onApplyCacheConfig = params.onApplyCacheConfig;
        }
        if (params.tab !== undefined) {
            this.tab = params.tab;
        }
        if (params.startBuffer !== undefined) {
            this.startBuffer = params.startBuffer;
        }
        if (params.highBuffer !== undefined) {
            this.highBuffer = params.highBuffer;
        }
        if (params.maxBuffer !== undefined) {
            this.maxBuffer = params.maxBuffer;
        }
        if (params.maxDelay !== undefined) {
            this.maxDelay = params.maxDelay;
        }
        if (params.netTimeout !== undefined) {
            this.netTimeout = params.netTimeout;
        }
        if (params.probeSize !== undefined) {
            this.probeSize = params.probeSize;
        }
        if (params.referrer !== undefined) {
            this.referrer = params.referrer;
        }
        if (params.httpProxy !== undefined) {
            this.httpProxy = params.httpProxy;
        }
        if (params.retryCount !== undefined) {
            this.retryCount = params.retryCount;
        }
        if (params.clearFrameWhenStop !== undefined) {
            this.clearFrameWhenStop = params.clearFrameWhenStop;
        }
        if (params.cacheEnable !== undefined) {
            this.cacheEnable = params.cacheEnable;
        }
        if (params.cacheMaxDurationS !== undefined) {
            this.cacheMaxDurationS = params.cacheMaxDurationS;
        }
        if (params.cacheMaxSizeMB !== undefined) {
            this.cacheMaxSizeMB = params.cacheMaxSizeMB;
        }
        if (params.cacheDir !== undefined) {
            this.cacheDir = params.cacheDir;
        }
    }
    updateStateVars(params: PlayerOptionPanel_Params) {
        this.__autoPlay.reset(params.autoPlay);
        this.__mute.reset(params.mute);
        this.__loop.reset(params.loop);
        this.__hardware.reset(params.hardware);
        this.__accurateSeek.reset(params.accurateSeek);
        this.__volumePercent.reset(params.volumePercent);
        this.__scaleMode.reset(params.scaleMode);
        this.__mirrorMode.reset(params.mirrorMode);
        this.__rotateMode.reset(params.rotateMode);
        this.__initialStartBuffer.reset(params.initialStartBuffer);
        this.__initialHighBuffer.reset(params.initialHighBuffer);
        this.__initialMaxBuffer.reset(params.initialMaxBuffer);
        this.__initialMaxDelay.reset(params.initialMaxDelay);
        this.__initialNetTimeout.reset(params.initialNetTimeout);
        this.__initialProbeSize.reset(params.initialProbeSize);
        this.__initialReferrer.reset(params.initialReferrer);
        this.__initialHttpProxy.reset(params.initialHttpProxy);
        this.__initialRetryCount.reset(params.initialRetryCount);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__autoPlay.purgeDependencyOnElmtId(rmElmtId);
        this.__mute.purgeDependencyOnElmtId(rmElmtId);
        this.__loop.purgeDependencyOnElmtId(rmElmtId);
        this.__hardware.purgeDependencyOnElmtId(rmElmtId);
        this.__accurateSeek.purgeDependencyOnElmtId(rmElmtId);
        this.__volumePercent.purgeDependencyOnElmtId(rmElmtId);
        this.__scaleMode.purgeDependencyOnElmtId(rmElmtId);
        this.__mirrorMode.purgeDependencyOnElmtId(rmElmtId);
        this.__rotateMode.purgeDependencyOnElmtId(rmElmtId);
        this.__initialStartBuffer.purgeDependencyOnElmtId(rmElmtId);
        this.__initialHighBuffer.purgeDependencyOnElmtId(rmElmtId);
        this.__initialMaxBuffer.purgeDependencyOnElmtId(rmElmtId);
        this.__initialMaxDelay.purgeDependencyOnElmtId(rmElmtId);
        this.__initialNetTimeout.purgeDependencyOnElmtId(rmElmtId);
        this.__initialProbeSize.purgeDependencyOnElmtId(rmElmtId);
        this.__initialReferrer.purgeDependencyOnElmtId(rmElmtId);
        this.__initialHttpProxy.purgeDependencyOnElmtId(rmElmtId);
        this.__initialRetryCount.purgeDependencyOnElmtId(rmElmtId);
        this.__tab.purgeDependencyOnElmtId(rmElmtId);
        this.__startBuffer.purgeDependencyOnElmtId(rmElmtId);
        this.__highBuffer.purgeDependencyOnElmtId(rmElmtId);
        this.__maxBuffer.purgeDependencyOnElmtId(rmElmtId);
        this.__maxDelay.purgeDependencyOnElmtId(rmElmtId);
        this.__netTimeout.purgeDependencyOnElmtId(rmElmtId);
        this.__probeSize.purgeDependencyOnElmtId(rmElmtId);
        this.__referrer.purgeDependencyOnElmtId(rmElmtId);
        this.__httpProxy.purgeDependencyOnElmtId(rmElmtId);
        this.__retryCount.purgeDependencyOnElmtId(rmElmtId);
        this.__clearFrameWhenStop.purgeDependencyOnElmtId(rmElmtId);
        this.__cacheEnable.purgeDependencyOnElmtId(rmElmtId);
        this.__cacheMaxDurationS.purgeDependencyOnElmtId(rmElmtId);
        this.__cacheMaxSizeMB.purgeDependencyOnElmtId(rmElmtId);
        this.__cacheDir.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__autoPlay.aboutToBeDeleted();
        this.__mute.aboutToBeDeleted();
        this.__loop.aboutToBeDeleted();
        this.__hardware.aboutToBeDeleted();
        this.__accurateSeek.aboutToBeDeleted();
        this.__volumePercent.aboutToBeDeleted();
        this.__scaleMode.aboutToBeDeleted();
        this.__mirrorMode.aboutToBeDeleted();
        this.__rotateMode.aboutToBeDeleted();
        this.__initialStartBuffer.aboutToBeDeleted();
        this.__initialHighBuffer.aboutToBeDeleted();
        this.__initialMaxBuffer.aboutToBeDeleted();
        this.__initialMaxDelay.aboutToBeDeleted();
        this.__initialNetTimeout.aboutToBeDeleted();
        this.__initialProbeSize.aboutToBeDeleted();
        this.__initialReferrer.aboutToBeDeleted();
        this.__initialHttpProxy.aboutToBeDeleted();
        this.__initialRetryCount.aboutToBeDeleted();
        this.__tab.aboutToBeDeleted();
        this.__startBuffer.aboutToBeDeleted();
        this.__highBuffer.aboutToBeDeleted();
        this.__maxBuffer.aboutToBeDeleted();
        this.__maxDelay.aboutToBeDeleted();
        this.__netTimeout.aboutToBeDeleted();
        this.__probeSize.aboutToBeDeleted();
        this.__referrer.aboutToBeDeleted();
        this.__httpProxy.aboutToBeDeleted();
        this.__retryCount.aboutToBeDeleted();
        this.__clearFrameWhenStop.aboutToBeDeleted();
        this.__cacheEnable.aboutToBeDeleted();
        this.__cacheMaxDurationS.aboutToBeDeleted();
        this.__cacheMaxSizeMB.aboutToBeDeleted();
        this.__cacheDir.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __autoPlay: SynchedPropertySimpleOneWayPU<boolean>;
    get autoPlay() {
        return this.__autoPlay.get();
    }
    set autoPlay(newValue: boolean) {
        this.__autoPlay.set(newValue);
    }
    private __mute: SynchedPropertySimpleOneWayPU<boolean>;
    get mute() {
        return this.__mute.get();
    }
    set mute(newValue: boolean) {
        this.__mute.set(newValue);
    }
    private __loop: SynchedPropertySimpleOneWayPU<boolean>;
    get loop() {
        return this.__loop.get();
    }
    set loop(newValue: boolean) {
        this.__loop.set(newValue);
    }
    private __hardware: SynchedPropertySimpleOneWayPU<boolean>;
    get hardware() {
        return this.__hardware.get();
    }
    set hardware(newValue: boolean) {
        this.__hardware.set(newValue);
    }
    private __accurateSeek: SynchedPropertySimpleOneWayPU<boolean>;
    get accurateSeek() {
        return this.__accurateSeek.get();
    }
    set accurateSeek(newValue: boolean) {
        this.__accurateSeek.set(newValue);
    }
    /** Volume as 0..100 on the slider (Android's IndicatorSeekBar max is 100). */
    private __volumePercent: SynchedPropertySimpleOneWayPU<number>;
    get volumePercent() {
        return this.__volumePercent.get();
    }
    set volumePercent(newValue: number) {
        this.__volumePercent.set(newValue);
    }
    /** ScaleMode: 0 fit / 1 aspect-fill / 2 fill. */
    private __scaleMode: SynchedPropertySimpleOneWayPU<number>;
    get scaleMode() {
        return this.__scaleMode.get();
    }
    set scaleMode(newValue: number) {
        this.__scaleMode.set(newValue);
    }
    /** MirrorMode: 0 none / 1 horizontal / 2 vertical. */
    private __mirrorMode: SynchedPropertySimpleOneWayPU<number>;
    get mirrorMode() {
        return this.__mirrorMode.get();
    }
    set mirrorMode(newValue: number) {
        this.__mirrorMode.set(newValue);
    }
    /** RotateMode in degrees: 0 / 90 / 180 / 270. */
    private __rotateMode: SynchedPropertySimpleOneWayPU<number>;
    get rotateMode() {
        return this.__rotateMode.get();
    }
    set rotateMode(newValue: number) {
        this.__rotateMode.set(newValue);
    }
    /** Initial text for the config fields (from the player's options). */
    private __initialStartBuffer: SynchedPropertySimpleOneWayPU<string>;
    get initialStartBuffer() {
        return this.__initialStartBuffer.get();
    }
    set initialStartBuffer(newValue: string) {
        this.__initialStartBuffer.set(newValue);
    }
    private __initialHighBuffer: SynchedPropertySimpleOneWayPU<string>;
    get initialHighBuffer() {
        return this.__initialHighBuffer.get();
    }
    set initialHighBuffer(newValue: string) {
        this.__initialHighBuffer.set(newValue);
    }
    private __initialMaxBuffer: SynchedPropertySimpleOneWayPU<string>;
    get initialMaxBuffer() {
        return this.__initialMaxBuffer.get();
    }
    set initialMaxBuffer(newValue: string) {
        this.__initialMaxBuffer.set(newValue);
    }
    private __initialMaxDelay: SynchedPropertySimpleOneWayPU<string>;
    get initialMaxDelay() {
        return this.__initialMaxDelay.get();
    }
    set initialMaxDelay(newValue: string) {
        this.__initialMaxDelay.set(newValue);
    }
    private __initialNetTimeout: SynchedPropertySimpleOneWayPU<string>;
    get initialNetTimeout() {
        return this.__initialNetTimeout.get();
    }
    set initialNetTimeout(newValue: string) {
        this.__initialNetTimeout.set(newValue);
    }
    private __initialProbeSize: SynchedPropertySimpleOneWayPU<string>;
    get initialProbeSize() {
        return this.__initialProbeSize.get();
    }
    set initialProbeSize(newValue: string) {
        this.__initialProbeSize.set(newValue);
    }
    private __initialReferrer: SynchedPropertySimpleOneWayPU<string>;
    get initialReferrer() {
        return this.__initialReferrer.get();
    }
    set initialReferrer(newValue: string) {
        this.__initialReferrer.set(newValue);
    }
    private __initialHttpProxy: SynchedPropertySimpleOneWayPU<string>;
    get initialHttpProxy() {
        return this.__initialHttpProxy.get();
    }
    set initialHttpProxy(newValue: string) {
        this.__initialHttpProxy.set(newValue);
    }
    private __initialRetryCount: SynchedPropertySimpleOneWayPU<string>;
    get initialRetryCount() {
        return this.__initialRetryCount.get();
    }
    set initialRetryCount(newValue: string) {
        this.__initialRetryCount.set(newValue);
    }
    private onAutoPlayChange: (value: boolean) => void;
    private onMuteChange: (value: boolean) => void;
    private onLoopChange: (value: boolean) => void;
    private onHardwareChange: (value: boolean) => void;
    private onAccurateSeekChange: (value: boolean) => void;
    private onVolumeChange: (percent: number) => void;
    private onScaleModeChange: (mode: number) => void;
    private onMirrorModeChange: (mode: number) => void;
    private onRotateModeChange: (mode: number) => void;
    private onShowMediaInfo: () => void;
    private onApplyConfig: (startBuffer: string, highBuffer: string, maxBuffer: string, maxDelay: string, netTimeout: string, probeSize: string, referrer: string, httpProxy: string, retryCount: string, clearFrameWhenStop: boolean) => void;
    private onApplyCacheConfig: (enable: boolean, maxDurationS: string, maxSizeMB: string, dir: string) => void;
    private __tab: ObservedPropertySimplePU<number>;
    get tab() {
        return this.__tab.get();
    }
    set tab(newValue: number) {
        this.__tab.set(newValue);
    }
    private __startBuffer: ObservedPropertySimplePU<string>;
    get startBuffer() {
        return this.__startBuffer.get();
    }
    set startBuffer(newValue: string) {
        this.__startBuffer.set(newValue);
    }
    private __highBuffer: ObservedPropertySimplePU<string>;
    get highBuffer() {
        return this.__highBuffer.get();
    }
    set highBuffer(newValue: string) {
        this.__highBuffer.set(newValue);
    }
    private __maxBuffer: ObservedPropertySimplePU<string>;
    get maxBuffer() {
        return this.__maxBuffer.get();
    }
    set maxBuffer(newValue: string) {
        this.__maxBuffer.set(newValue);
    }
    private __maxDelay: ObservedPropertySimplePU<string>;
    get maxDelay() {
        return this.__maxDelay.get();
    }
    set maxDelay(newValue: string) {
        this.__maxDelay.set(newValue);
    }
    private __netTimeout: ObservedPropertySimplePU<string>;
    get netTimeout() {
        return this.__netTimeout.get();
    }
    set netTimeout(newValue: string) {
        this.__netTimeout.set(newValue);
    }
    private __probeSize: ObservedPropertySimplePU<string>;
    get probeSize() {
        return this.__probeSize.get();
    }
    set probeSize(newValue: string) {
        this.__probeSize.set(newValue);
    }
    private __referrer: ObservedPropertySimplePU<string>;
    get referrer() {
        return this.__referrer.get();
    }
    set referrer(newValue: string) {
        this.__referrer.set(newValue);
    }
    private __httpProxy: ObservedPropertySimplePU<string>;
    get httpProxy() {
        return this.__httpProxy.get();
    }
    set httpProxy(newValue: string) {
        this.__httpProxy.set(newValue);
    }
    private __retryCount: ObservedPropertySimplePU<string>;
    get retryCount() {
        return this.__retryCount.get();
    }
    set retryCount(newValue: string) {
        this.__retryCount.set(newValue);
    }
    private __clearFrameWhenStop: ObservedPropertySimplePU<boolean>;
    get clearFrameWhenStop() {
        return this.__clearFrameWhenStop.get();
    }
    set clearFrameWhenStop(newValue: boolean) {
        this.__clearFrameWhenStop.set(newValue);
    }
    private __cacheEnable: ObservedPropertySimplePU<boolean>;
    get cacheEnable() {
        return this.__cacheEnable.get();
    }
    set cacheEnable(newValue: boolean) {
        this.__cacheEnable.set(newValue);
    }
    private __cacheMaxDurationS: ObservedPropertySimplePU<string>;
    get cacheMaxDurationS() {
        return this.__cacheMaxDurationS.get();
    }
    set cacheMaxDurationS(newValue: string) {
        this.__cacheMaxDurationS.set(newValue);
    }
    private __cacheMaxSizeMB: ObservedPropertySimplePU<string>;
    get cacheMaxSizeMB() {
        return this.__cacheMaxSizeMB.get();
    }
    set cacheMaxSizeMB(newValue: string) {
        this.__cacheMaxSizeMB.set(newValue);
    }
    private __cacheDir: ObservedPropertySimplePU<string>;
    get cacheDir() {
        return this.__cacheDir.get();
    }
    set cacheDir(newValue: string) {
        this.__cacheDir.set(newValue);
    }
    aboutToAppear(): void {
        this.startBuffer = this.initialStartBuffer;
        this.highBuffer = this.initialHighBuffer;
        this.maxBuffer = this.initialMaxBuffer;
        this.maxDelay = this.initialMaxDelay;
        this.netTimeout = this.initialNetTimeout;
        this.probeSize = this.initialProbeSize;
        this.referrer = this.initialReferrer;
        this.httpProxy = this.initialHttpProxy;
        this.retryCount = this.initialRetryCount;
    }
    switchRow(label: string, on: boolean, action: (value: boolean) => void, parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create();
            Row.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(90:5)", "entry");
            Row.width('100%');
            Row.height(44);
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(label);
            Text.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(91:7)", "entry");
            Text.fontSize(14);
            Text.fontColor('#333333');
            Text.layoutWeight(1);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Toggle.create({ type: ToggleType.Switch, isOn: on });
            Toggle.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(92:7)", "entry");
            Toggle.onChange(action);
        }, Toggle);
        Toggle.pop();
        Row.pop();
    }
    choiceRow(label: string, current: number, values: number[], labels: string[], action: (mode: number) => void, parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(100:5)", "entry");
            Column.width('100%');
            Column.margin({ top: 8, bottom: 4 });
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(label);
            Text.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(101:7)", "entry");
            Text.fontSize(14);
            Text.fontColor('#333333');
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 6 });
            Row.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(102:7)", "entry");
            Row.width('100%');
            Row.margin({ top: 6 });
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            ForEach.create();
            const forEachItemGenFunction = (_item, position: number) => {
                const value = _item;
                this.observeComponentCreation2((elmtId, isInitialRender) => {
                    Text.create(labels[position]);
                    Text.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(104:11)", "entry");
                    Text.fontSize(13);
                    Text.fontColor(current === value ? Color.White : '#3F51B5');
                    Text.textAlign(TextAlign.Center);
                    Text.layoutWeight(1);
                    Text.height(32);
                    Text.borderRadius(4);
                    Text.borderWidth(1);
                    Text.borderColor('#3F51B5');
                    Text.backgroundColor(current === value ? '#3F51B5' : Color.Transparent);
                    Text.onClick(() => action(value));
                }, Text);
                Text.pop();
            };
            this.forEachUpdateFunction(elmtId, values, forEachItemGenFunction, (value: number) => `${label}-${value}`, true, false);
        }, ForEach);
        ForEach.pop();
        Row.pop();
        Column.pop();
    }
    textField(label: string, value: string, action: (text: string) => void, parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create();
            Row.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(126:5)", "entry");
            Row.width('100%');
            Row.height(50);
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(label);
            Text.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(127:7)", "entry");
            Text.fontSize(13);
            Text.fontColor('#333333');
            Text.layoutWeight(1);
            Text.textAlign(TextAlign.Center);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            TextInput.create({ text: value });
            TextInput.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(128:7)", "entry");
            TextInput.fontSize(13);
            TextInput.layoutWeight(2);
            TextInput.onChange(action);
        }, TextInput);
        Row.pop();
    }
    operationTab(parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(139:5)", "entry");
            Column.width('100%');
        }, Column);
        this.switchRow.bind(this)('自动播放', this.autoPlay, (v: boolean) => this.onAutoPlayChange(v));
        this.switchRow.bind(this)('静音', this.mute, (v: boolean) => this.onMuteChange(v));
        this.switchRow.bind(this)('循环', this.loop, (v: boolean) => this.onLoopChange(v));
        this.switchRow.bind(this)('硬解码', this.hardware, (v: boolean) => this.onHardwareChange(v));
        this.switchRow.bind(this)('精准seek', this.accurateSeek, (v: boolean) => this.onAccurateSeekChange(v));
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(146:7)", "entry");
            Column.width('100%');
            Column.margin({ top: 8 });
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(`音量 ${this.volumePercent}`);
            Text.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(147:9)", "entry");
            Text.fontSize(14);
            Text.fontColor('#333333');
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Slider.create({ value: this.volumePercent, min: 0, max: 100, step: 1 });
            Slider.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(148:9)", "entry");
            Slider.width('100%');
            Slider.onChange((value: number, mode: SliderChangeMode) => {
                if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                    // Android maps progress/50 to a 0..2.0 volume range.
                    this.onVolumeChange(value);
                }
            });
        }, Slider);
        Column.pop();
        this.choiceRow.bind(this)('缩放模式', this.scaleMode, [0, 1, 2], ['比例适应', '比例填充', '拉伸全屏'], (m: number) => this.onScaleModeChange(m));
        this.choiceRow.bind(this)('镜像模式', this.mirrorMode, [0, 1, 2], ['无镜像', '水平镜像', '垂直镜像'], (m: number) => this.onMirrorModeChange(m));
        this.choiceRow.bind(this)('旋转模式', this.rotateMode, [0, 90, 180, 270], ['0°', '90°', '180°', '270°'], (m: number) => this.onRotateModeChange(m));
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Button.createWithLabel('媒体信息');
            Button.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(167:7)", "entry");
            Button.onClick(() => this.onShowMediaInfo());
            Button.width('100%');
            Button.height(40);
            Button.backgroundColor('#3F51B5');
            Button.fontColor(Color.White);
            Button.margin({ top: 12, bottom: 12 });
        }, Button);
        Button.pop();
        Column.pop();
    }
    configTab(parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(179:5)", "entry");
            Column.width('100%');
        }, Column);
        this.textField.bind(this)('启播缓冲时长', this.startBuffer, (t: string) => { this.startBuffer = t; });
        this.textField.bind(this)('卡顿恢复', this.highBuffer, (t: string) => { this.highBuffer = t; });
        this.textField.bind(this)('最大缓冲时长', this.maxBuffer, (t: string) => { this.maxBuffer = t; });
        this.textField.bind(this)('直播最大延迟', this.maxDelay, (t: string) => { this.maxDelay = t; });
        this.textField.bind(this)('网络超时', this.netTimeout, (t: string) => { this.netTimeout = t; });
        this.textField.bind(this)('probe大小', this.probeSize, (t: string) => { this.probeSize = t; });
        this.textField.bind(this)('请求referer', this.referrer, (t: string) => { this.referrer = t; });
        this.textField.bind(this)('httpProxy代理', this.httpProxy, (t: string) => { this.httpProxy = t; });
        this.textField.bind(this)('重连次数', this.retryCount, (t: string) => { this.retryCount = t; });
        this.switchRow.bind(this)('停止隐藏最后帧', this.clearFrameWhenStop, (v: boolean) => { this.clearFrameWhenStop = v; });
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Button.createWithLabel('应用配置');
            Button.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(192:7)", "entry");
            Button.onClick(() => this.onApplyConfig(this.startBuffer, this.highBuffer, this.maxBuffer, this.maxDelay, this.netTimeout, this.probeSize, this.referrer, this.httpProxy, this.retryCount, this.clearFrameWhenStop));
            Button.width('100%');
            Button.height(40);
            Button.backgroundColor('#3F51B5');
            Button.fontColor(Color.White);
            Button.margin({ top: 12, bottom: 12 });
        }, Button);
        Button.pop();
        Column.pop();
    }
    cacheTab(parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(207:5)", "entry");
            Column.width('100%');
        }, Column);
        this.textField.bind(this)('最大时长(单位s)', this.cacheMaxDurationS, (t: string) => { this.cacheMaxDurationS = t; });
        this.textField.bind(this)('最大size(单位MB)', this.cacheMaxSizeMB, (t: string) => { this.cacheMaxSizeMB = t; });
        this.textField.bind(this)('保存路径', this.cacheDir, (t: string) => { this.cacheDir = t; });
        this.switchRow.bind(this)('开启缓存配置', this.cacheEnable, (v: boolean) => { this.cacheEnable = v; });
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Button.createWithLabel('应用配置');
            Button.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(213:7)", "entry");
            Button.onClick(() => this.onApplyCacheConfig(this.cacheEnable, this.cacheMaxDurationS, this.cacheMaxSizeMB, this.cacheDir));
            Button.width('100%');
            Button.height(40);
            Button.backgroundColor('#3F51B5');
            Button.fontColor(Color.White);
            Button.margin({ top: 12, bottom: 12 });
        }, Button);
        Button.pop();
        Column.pop();
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(226:5)", "entry");
            Column.width('100%');
            Column.layoutWeight(1);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 4 });
            Row.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(227:7)", "entry");
            Row.width('100%');
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            ForEach.create();
            const forEachItemGenFunction = (_item, position: number) => {
                const label = _item;
                this.observeComponentCreation2((elmtId, isInitialRender) => {
                    Text.create(label);
                    Text.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(229:11)", "entry");
                    Text.fontSize(13);
                    Text.fontColor(this.tab === position ? '#3F51B5' : '#333333');
                    Text.textAlign(TextAlign.Center);
                    Text.layoutWeight(1);
                    Text.height(36);
                    Text.onClick(() => { this.tab = position; });
                }, Text);
                Text.pop();
            };
            this.forEachUpdateFunction(elmtId, ['options', 'playerConfig', 'cacheConfig'], forEachItemGenFunction, (label: string) => label, true, false);
        }, ForEach);
        ForEach.pop();
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Divider.create();
            Divider.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(240:7)", "entry");
            Divider.strokeWidth(1);
            Divider.color('#DDDDDD');
        }, Divider);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Scroll.create();
            Scroll.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(242:7)", "entry");
            Scroll.layoutWeight(1);
            Scroll.width('100%');
        }, Scroll);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/view/PlayerOptionPanel.ets(243:9)", "entry");
            Column.width('100%');
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.tab === 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.operationTab.bind(this)();
                });
            }
            else if (this.tab === 1) {
                this.ifElseBranchUpdateFunction(1, () => {
                    this.configTab.bind(this)();
                });
            }
            else {
                this.ifElseBranchUpdateFunction(2, () => {
                    this.cacheTab.bind(this)();
                });
            }
        }, If);
        If.pop();
        Column.pop();
        Scroll.pop();
        Column.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
}
