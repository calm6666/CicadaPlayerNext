if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface MultiPlayerItem_Params {
    url?: string;
    label?: string;
    controller?: XComponentController;
    player?: CicadaPlayer;
    surfaceReady?: boolean;
    status?: string;
    playing?: boolean;
}
import { CicadaPlayer } from "@bundle:com.cicada.player.demo/entry/ets/player/CicadaPlayer";
import { S } from "@bundle:com.cicada.player.demo/entry/ets/util/Strings";
export class MultiPlayerItem extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__url = new SynchedPropertySimpleOneWayPU(params.url, this, "url");
        this.__label = new SynchedPropertySimpleOneWayPU(params.label, this, "label");
        this.controller = new XComponentController();
        this.player = new CicadaPlayer();
        this.surfaceReady = false;
        this.__status = new ObservedPropertySimplePU('idle', this, "status");
        this.__playing = new ObservedPropertySimplePU(false, this, "playing");
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: MultiPlayerItem_Params) {
        if (params.url === undefined) {
            this.__url.set('');
        }
        if (params.label === undefined) {
            this.__label.set('');
        }
        if (params.controller !== undefined) {
            this.controller = params.controller;
        }
        if (params.player !== undefined) {
            this.player = params.player;
        }
        if (params.surfaceReady !== undefined) {
            this.surfaceReady = params.surfaceReady;
        }
        if (params.status !== undefined) {
            this.status = params.status;
        }
        if (params.playing !== undefined) {
            this.playing = params.playing;
        }
    }
    updateStateVars(params: MultiPlayerItem_Params) {
        this.__url.reset(params.url);
        this.__label.reset(params.label);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__url.purgeDependencyOnElmtId(rmElmtId);
        this.__label.purgeDependencyOnElmtId(rmElmtId);
        this.__status.purgeDependencyOnElmtId(rmElmtId);
        this.__playing.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__url.aboutToBeDeleted();
        this.__label.aboutToBeDeleted();
        this.__status.aboutToBeDeleted();
        this.__playing.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __url: SynchedPropertySimpleOneWayPU<string>;
    get url() {
        return this.__url.get();
    }
    set url(newValue: string) {
        this.__url.set(newValue);
    }
    private __label: SynchedPropertySimpleOneWayPU<string>;
    get label() {
        return this.__label.get();
    }
    set label(newValue: string) {
        this.__label.set(newValue);
    }
    private controller: XComponentController;
    private player: CicadaPlayer;
    private surfaceReady: boolean;
    private __status: ObservedPropertySimplePU<string>;
    get status() {
        return this.__status.get();
    }
    set status(newValue: string) {
        this.__status.set(newValue);
    }
    private __playing: ObservedPropertySimplePU<boolean>;
    get playing() {
        return this.__playing.get();
    }
    set playing(newValue: boolean) {
        this.__playing.set(newValue);
    }
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
    actionButton(label: string, action: () => void, parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Button.createWithLabel(label);
            Button.debugLine("entry/src/main/ets/view/MultiPlayerItem.ets(61:5)", "entry");
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
            Column.debugLine("entry/src/main/ets/view/MultiPlayerItem.ets(71:5)", "entry");
            Column.width('100%');
            Column.margin({ bottom: 10 });
            Column.backgroundColor('#F5F5F5');
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Stack.create();
            Stack.debugLine("entry/src/main/ets/view/MultiPlayerItem.ets(72:7)", "entry");
            Stack.width('100%');
            Stack.height(200);
        }, Stack);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            XComponent.create({
                id: `multiSurface-${this.label}`,
                type: XComponentType.SURFACE,
                controller: this.controller
            }, "com.cicada.player.demo/entry");
            XComponent.debugLine("entry/src/main/ets/view/MultiPlayerItem.ets(73:9)", "entry");
            XComponent.onLoad(() => this.onSurfaceLoad());
            XComponent.width('100%');
            XComponent.height('100%');
            XComponent.backgroundColor(Color.Black);
        }, XComponent);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.status);
            Text.debugLine("entry/src/main/ets/view/MultiPlayerItem.ets(83:9)", "entry");
            Text.fontSize(11);
            Text.fontColor('#FF9800');
            Text.margin(6);
            Text.align(Alignment.TopStart);
        }, Text);
        Text.pop();
        Stack.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.label);
            Text.debugLine("entry/src/main/ets/view/MultiPlayerItem.ets(92:7)", "entry");
            Text.fontSize(12);
            Text.fontColor('#333333');
            Text.maxLines(1);
            Text.textOverflow({ overflow: TextOverflow.Ellipsis });
            Text.width('100%');
            Text.padding({ left: 6, right: 6, top: 4, bottom: 4 });
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 4 });
            Row.debugLine("entry/src/main/ets/view/MultiPlayerItem.ets(100:7)", "entry");
            Row.width('100%');
            Row.padding({ left: 4, right: 4, bottom: 6 });
        }, Row);
        this.actionButton.bind(this)(S.prepare, () => {
            this.player.setDataSource(this.url);
            this.player.prepare();
            this.status = 'preparing';
        });
        this.actionButton.bind(this)(S.play, () => {
            this.player.start();
            this.playing = true;
        });
        this.actionButton.bind(this)(S.pause, () => {
            this.player.pause();
            this.playing = false;
        });
        this.actionButton.bind(this)(S.stop, () => {
            this.player.stop();
            this.playing = false;
            this.status = 'stopped';
        });
        Row.pop();
        Column.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
}
