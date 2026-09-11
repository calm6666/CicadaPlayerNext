if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface AvPlayerPage_Params {
    avPlayer?: media.AVPlayer | null;
    controller?: XComponentController;
    surfaceId?: string;
    pendingUrl?: string;
    autoPlayed?: boolean;
    heldFile?: fileIo.File | null;
    url?: string;
    state?: string;
    positionMs?: number;
    durationMs?: number;
    videoSize?: string;
}
import media from "@ohos:multimedia.media";
import type { BusinessError } from "@ohos:base";
import fileIo from "@ohos:file.fs";
import promptAction from "@ohos:promptAction";
import router from "@ohos:router";
import { TitleBar } from "@bundle:com.cicada.player.demo/entry/ets/view/TitleBar";
import type { PlayerRouteParams } from '../model/CicadaTypes';
import { LocalVideoSource } from "@bundle:com.cicada.player.demo/entry/ets/util/LocalVideoSource";
import { formatMs } from "@bundle:com.cicada.player.demo/entry/ets/util/TimeFormater";
import { S } from "@bundle:com.cicada.player.demo/entry/ets/util/Strings";
class AvPlayerPage extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.avPlayer = null;
        this.controller = new XComponentController();
        this.surfaceId = '';
        this.pendingUrl = '';
        this.autoPlayed = false;
        this.heldFile = null;
        this.__url = new ObservedPropertySimplePU('', this, "url");
        this.__state = new ObservedPropertySimplePU('idle', this, "state");
        this.__positionMs = new ObservedPropertySimplePU(0, this, "positionMs");
        this.__durationMs = new ObservedPropertySimplePU(0, this, "durationMs");
        this.__videoSize = new ObservedPropertySimplePU('-', this, "videoSize");
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: AvPlayerPage_Params) {
        if (params.avPlayer !== undefined) {
            this.avPlayer = params.avPlayer;
        }
        if (params.controller !== undefined) {
            this.controller = params.controller;
        }
        if (params.surfaceId !== undefined) {
            this.surfaceId = params.surfaceId;
        }
        if (params.pendingUrl !== undefined) {
            this.pendingUrl = params.pendingUrl;
        }
        if (params.autoPlayed !== undefined) {
            this.autoPlayed = params.autoPlayed;
        }
        if (params.heldFile !== undefined) {
            this.heldFile = params.heldFile;
        }
        if (params.url !== undefined) {
            this.url = params.url;
        }
        if (params.state !== undefined) {
            this.state = params.state;
        }
        if (params.positionMs !== undefined) {
            this.positionMs = params.positionMs;
        }
        if (params.durationMs !== undefined) {
            this.durationMs = params.durationMs;
        }
        if (params.videoSize !== undefined) {
            this.videoSize = params.videoSize;
        }
    }
    updateStateVars(params: AvPlayerPage_Params) {
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__url.purgeDependencyOnElmtId(rmElmtId);
        this.__state.purgeDependencyOnElmtId(rmElmtId);
        this.__positionMs.purgeDependencyOnElmtId(rmElmtId);
        this.__durationMs.purgeDependencyOnElmtId(rmElmtId);
        this.__videoSize.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__url.aboutToBeDeleted();
        this.__state.aboutToBeDeleted();
        this.__positionMs.aboutToBeDeleted();
        this.__durationMs.aboutToBeDeleted();
        this.__videoSize.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private avPlayer: media.AVPlayer | null;
    private controller: XComponentController;
    private surfaceId: string;
    private pendingUrl: string;
    private autoPlayed: boolean;
    /** Descriptor backing an fdSrc source; must stay open while playing. */
    private heldFile: fileIo.File | null;
    private __url: ObservedPropertySimplePU<string>;
    get url() {
        return this.__url.get();
    }
    set url(newValue: string) {
        this.__url.set(newValue);
    }
    private __state: ObservedPropertySimplePU<string>;
    get state() {
        return this.__state.get();
    }
    set state(newValue: string) {
        this.__state.set(newValue);
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
    private __videoSize: ObservedPropertySimplePU<string>;
    get videoSize() {
        return this.__videoSize.get();
    }
    set videoSize(newValue: string) {
        this.__videoSize.set(newValue);
    }
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
        }
        catch (err) {
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
        }
        catch (err) {
            this.state = 'open failed';
        }
    }
    private async releasePlayer(): Promise<void> {
        const player = this.avPlayer;
        this.avPlayer = null;
        if (this.heldFile !== null) {
            try {
                fileIo.closeSync(this.heldFile);
            }
            catch (err) {
                // Best effort.
            }
            this.heldFile = null;
        }
        if (player === null) {
            return;
        }
        try {
            await player.release();
        }
        catch (err) {
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
            }
            else if (this.state === 'prepared') {
                await player.play();
            }
        }
        catch (err) {
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
            }
            else {
                this.prepare();
            }
        }
        catch (err) {
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
        }
        catch (err) {
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
        }
        catch (err) {
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
    actionButton(label: string, action: () => void, parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Button.createWithLabel(label);
            Button.debugLine("entry/src/main/ets/pages/AvPlayerPage.ets(234:5)", "entry");
            Button.fontSize(12);
            Button.layoutWeight(1);
            Button.height(36);
            Button.backgroundColor('#3F51B5');
            Button.fontColor(Color.White);
            Button.onClick(action);
        }, Button);
        Button.pop();
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/pages/AvPlayerPage.ets(244:5)", "entry");
            Column.width('100%');
            Column.height('100%');
            Column.backgroundColor('#FFFFFF');
        }, Column);
        {
            this.observeComponentCreation2((elmtId, isInitialRender) => {
                if (isInitialRender) {
                    let componentCall = new TitleBar(this, {
                        title: 'AVPlayer',
                        onBack: () => router.back()
                    }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/AvPlayerPage.ets", line: 245, col: 7 });
                    ViewPU.create(componentCall);
                    let paramsLambda = () => {
                        return {
                            title: 'AVPlayer',
                            onBack: () => router.back()
                        };
                    };
                    componentCall.paramsGenerator_ = paramsLambda;
                }
                else {
                    this.updateStateVarsOfChildByElmtId(elmtId, {
                        title: 'AVPlayer'
                    });
                }
            }, { name: "TitleBar" });
        }
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Stack.create();
            Stack.debugLine("entry/src/main/ets/pages/AvPlayerPage.ets(250:7)", "entry");
            Stack.width('100%');
            Stack.height(240);
        }, Stack);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            XComponent.create({
                id: 'avPlayerSurface',
                type: XComponentType.SURFACE,
                controller: this.controller
            }, "com.cicada.player.demo/entry");
            XComponent.debugLine("entry/src/main/ets/pages/AvPlayerPage.ets(251:9)", "entry");
            XComponent.onLoad(() => this.onSurfaceLoad());
            XComponent.width('100%');
            XComponent.height('100%');
            XComponent.backgroundColor(Color.Black);
        }, XComponent);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(`${this.state}  ${this.videoSize}`);
            Text.debugLine("entry/src/main/ets/pages/AvPlayerPage.ets(261:9)", "entry");
            Text.fontSize(11);
            Text.fontColor('#FF9800');
            Text.margin(6);
            Text.align(Alignment.TopStart);
        }, Text);
        Text.pop();
        Stack.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 6 });
            Row.debugLine("entry/src/main/ets/pages/AvPlayerPage.ets(270:7)", "entry");
            Row.width('100%');
            Row.padding({ left: 8, right: 8, top: 6 });
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(`${formatMs(this.positionMs)} / ${formatMs(this.durationMs)}`);
            Text.debugLine("entry/src/main/ets/pages/AvPlayerPage.ets(271:9)", "entry");
            Text.fontSize(12);
            Text.layoutWeight(1);
        }, Text);
        Text.pop();
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 6 });
            Row.debugLine("entry/src/main/ets/pages/AvPlayerPage.ets(278:7)", "entry");
            Row.width('100%');
            Row.padding(8);
        }, Row);
        this.actionButton.bind(this)(S.prepare, () => {
            this.prepare();
        });
        this.actionButton.bind(this)(S.play, () => {
            this.play();
        });
        this.actionButton.bind(this)(S.pause, () => {
            this.pause();
        });
        this.actionButton.bind(this)(S.stop, () => {
            this.stopPlayback();
        });
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 6 });
            Row.debugLine("entry/src/main/ets/pages/AvPlayerPage.ets(295:7)", "entry");
            Row.width('100%');
            Row.padding({ left: 8, right: 8 });
        }, Row);
        this.actionButton.bind(this)('-10s', () => this.seekTo(Math.max(0, this.positionMs - 10000)));
        this.actionButton.bind(this)('+10s', () => this.seekTo(this.positionMs + 10000));
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            TextInput.create({ text: this.url, placeholder: 'media url' });
            TextInput.debugLine("entry/src/main/ets/pages/AvPlayerPage.ets(302:7)", "entry");
            TextInput.fontSize(12);
            TextInput.width('100%');
            TextInput.margin({ top: 8 });
            TextInput.padding({ left: 8, right: 8 });
            TextInput.onChange((value: string) => {
                this.url = value;
            });
        }, TextInput);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Blank.create();
            Blank.debugLine("entry/src/main/ets/pages/AvPlayerPage.ets(311:7)", "entry");
            Blank.layoutWeight(1);
        }, Blank);
        Blank.pop();
        Column.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    static getEntryName(): string {
        return "AvPlayerPage";
    }
}
registerNamedRoute(() => new AvPlayerPage(undefined, {}), "", { bundleName: "com.cicada.player.demo", moduleName: "entry", pagePath: "pages/AvPlayerPage", pageFullPath: "entry/src/main/ets/pages/AvPlayerPage", integratedHsp: "false", moduleType: "followWithHap" });
