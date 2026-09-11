if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface PlayerControlBar_Params {
    title?: string;
    isFullScreen?: boolean;
    locked?: boolean;
    playing?: boolean;
    durationMs?: number;
    bufferMs?: number;
    positionMs?: number;
    onBack?: () => void;
    onToggleScreenMode?: () => void;
    onToggleLock?: () => void;
    onSeekStart?: () => void;
    onSeekChange?: (ms: number) => void;
    onSeekEnd?: (ms: number) => void;
}
import { formatMs } from "@bundle:com.cicada.player.demo/entry/ets/util/TimeFormater";
import { S } from "@bundle:com.cicada.player.demo/entry/ets/util/Strings";
export class PlayerControlBar extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__title = new SynchedPropertySimpleOneWayPU(params.title, this, "title");
        this.__isFullScreen = new SynchedPropertySimpleOneWayPU(params.isFullScreen, this, "isFullScreen");
        this.__locked = new SynchedPropertySimpleOneWayPU(params.locked, this, "locked");
        this.__playing = new SynchedPropertySimpleOneWayPU(params.playing, this, "playing");
        this.__durationMs = new SynchedPropertySimpleOneWayPU(params.durationMs, this, "durationMs");
        this.__bufferMs = new SynchedPropertySimpleOneWayPU(params.bufferMs, this, "bufferMs");
        this.__positionMs = new SynchedPropertySimpleOneWayPU(params.positionMs, this, "positionMs");
        this.onBack = () => { };
        this.onToggleScreenMode = () => { };
        this.onToggleLock = () => { };
        this.onSeekStart = () => { };
        this.onSeekChange = () => { };
        this.onSeekEnd = () => { };
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: PlayerControlBar_Params) {
        if (params.title === undefined) {
            this.__title.set('');
        }
        if (params.isFullScreen === undefined) {
            this.__isFullScreen.set(false);
        }
        if (params.locked === undefined) {
            this.__locked.set(false);
        }
        if (params.playing === undefined) {
            this.__playing.set(false);
        }
        if (params.durationMs === undefined) {
            this.__durationMs.set(0);
        }
        if (params.bufferMs === undefined) {
            this.__bufferMs.set(0);
        }
        if (params.positionMs === undefined) {
            this.__positionMs.set(0);
        }
        if (params.onBack !== undefined) {
            this.onBack = params.onBack;
        }
        if (params.onToggleScreenMode !== undefined) {
            this.onToggleScreenMode = params.onToggleScreenMode;
        }
        if (params.onToggleLock !== undefined) {
            this.onToggleLock = params.onToggleLock;
        }
        if (params.onSeekStart !== undefined) {
            this.onSeekStart = params.onSeekStart;
        }
        if (params.onSeekChange !== undefined) {
            this.onSeekChange = params.onSeekChange;
        }
        if (params.onSeekEnd !== undefined) {
            this.onSeekEnd = params.onSeekEnd;
        }
    }
    updateStateVars(params: PlayerControlBar_Params) {
        this.__title.reset(params.title);
        this.__isFullScreen.reset(params.isFullScreen);
        this.__locked.reset(params.locked);
        this.__playing.reset(params.playing);
        this.__durationMs.reset(params.durationMs);
        this.__bufferMs.reset(params.bufferMs);
        this.__positionMs.reset(params.positionMs);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__title.purgeDependencyOnElmtId(rmElmtId);
        this.__isFullScreen.purgeDependencyOnElmtId(rmElmtId);
        this.__locked.purgeDependencyOnElmtId(rmElmtId);
        this.__playing.purgeDependencyOnElmtId(rmElmtId);
        this.__durationMs.purgeDependencyOnElmtId(rmElmtId);
        this.__bufferMs.purgeDependencyOnElmtId(rmElmtId);
        this.__positionMs.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__title.aboutToBeDeleted();
        this.__isFullScreen.aboutToBeDeleted();
        this.__locked.aboutToBeDeleted();
        this.__playing.aboutToBeDeleted();
        this.__durationMs.aboutToBeDeleted();
        this.__bufferMs.aboutToBeDeleted();
        this.__positionMs.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __title: SynchedPropertySimpleOneWayPU<string>;
    get title() {
        return this.__title.get();
    }
    set title(newValue: string) {
        this.__title.set(newValue);
    }
    private __isFullScreen: SynchedPropertySimpleOneWayPU<boolean>;
    get isFullScreen() {
        return this.__isFullScreen.get();
    }
    set isFullScreen(newValue: boolean) {
        this.__isFullScreen.set(newValue);
    }
    private __locked: SynchedPropertySimpleOneWayPU<boolean>;
    get locked() {
        return this.__locked.get();
    }
    set locked(newValue: boolean) {
        this.__locked.set(newValue);
    }
    private __playing: SynchedPropertySimpleOneWayPU<boolean>;
    get playing() {
        return this.__playing.get();
    }
    set playing(newValue: boolean) {
        this.__playing.set(newValue);
    }
    private __durationMs: SynchedPropertySimpleOneWayPU<number>;
    get durationMs() {
        return this.__durationMs.get();
    }
    set durationMs(newValue: number) {
        this.__durationMs.set(newValue);
    }
    private __bufferMs: SynchedPropertySimpleOneWayPU<number>;
    get bufferMs() {
        return this.__bufferMs.get();
    }
    set bufferMs(newValue: number) {
        this.__bufferMs.set(newValue);
    }
    /** Playback position; while dragging the page passes the preview value. */
    private __positionMs: SynchedPropertySimpleOneWayPU<number>;
    get positionMs() {
        return this.__positionMs.get();
    }
    set positionMs(newValue: number) {
        this.__positionMs.set(newValue);
    }
    private onBack: () => void;
    private onToggleScreenMode: () => void;
    private onToggleLock: () => void;
    private onSeekStart: () => void;
    private onSeekChange: (ms: number) => void;
    private onSeekEnd: (ms: number) => void;
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/view/PlayerControlBar.ets(38:5)", "entry");
            Column.width('100%');
            Column.height('100%');
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            // ---- top: title row (hidden while the screen is locked) ----
            if (!this.locked) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Row.create();
                        Row.debugLine("entry/src/main/ets/view/PlayerControlBar.ets(41:9)", "entry");
                        Row.width('100%');
                        Row.height(44);
                        Row.backgroundColor('#99000000');
                    }, Row);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create('←');
                        Text.debugLine("entry/src/main/ets/view/PlayerControlBar.ets(42:11)", "entry");
                        Text.fontSize(20);
                        Text.fontColor('#e7e7e7');
                        Text.width(40);
                        Text.height('100%');
                        Text.textAlign(TextAlign.Center);
                        Text.onClick(() => this.onBack());
                    }, Text);
                    Text.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(this.title);
                        Text.debugLine("entry/src/main/ets/view/PlayerControlBar.ets(50:11)", "entry");
                        Text.fontSize(18);
                        Text.fontColor('#e7e7e7');
                        Text.layoutWeight(1);
                        Text.maxLines(1);
                        Text.textOverflow({ overflow: TextOverflow.Ellipsis });
                        Text.margin({ left: 8, right: 8 });
                    }, Text);
                    Text.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        If.create();
                        // Lock toggle: only meaningful in full screen, as on Android.
                        if (this.isFullScreen) {
                            this.ifElseBranchUpdateFunction(0, () => {
                                this.observeComponentCreation2((elmtId, isInitialRender) => {
                                    Text.create(this.locked ? '🔒' : '🔓');
                                    Text.debugLine("entry/src/main/ets/view/PlayerControlBar.ets(60:13)", "entry");
                                    Text.fontSize(18);
                                    Text.fontColor('#e7e7e7');
                                    Text.width(44);
                                    Text.height('100%');
                                    Text.textAlign(TextAlign.Center);
                                    Text.onClick(() => this.onToggleLock());
                                }, Text);
                                Text.pop();
                            });
                        }
                        else {
                            this.ifElseBranchUpdateFunction(1, () => {
                            });
                        }
                    }, If);
                    If.pop();
                    Row.pop();
                });
            }
            else {
                this.ifElseBranchUpdateFunction(1, () => {
                });
            }
        }, If);
        If.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Blank.create();
            Blank.debugLine("entry/src/main/ets/view/PlayerControlBar.ets(74:7)", "entry");
            Blank.layoutWeight(1);
        }, Blank);
        Blank.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            // ---- bottom: info + seek bar ----
            if (!this.locked) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Column.create();
                        Column.debugLine("entry/src/main/ets/view/PlayerControlBar.ets(78:9)", "entry");
                        Column.width('100%');
                        Column.padding({ left: 8, right: 8, top: 4, bottom: 4 });
                        Column.backgroundColor('#99000000');
                    }, Column);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Row.create();
                        Row.debugLine("entry/src/main/ets/view/PlayerControlBar.ets(79:11)", "entry");
                        Row.width('100%');
                    }, Row);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(S.bufferLabel + formatMs(this.bufferMs));
                        Text.debugLine("entry/src/main/ets/view/PlayerControlBar.ets(80:13)", "entry");
                        Text.fontSize(12);
                        Text.fontColor('#e7e7e7');
                        Text.width('100%');
                    }, Text);
                    Text.pop();
                    Row.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Row.create();
                        Row.debugLine("entry/src/main/ets/view/PlayerControlBar.ets(87:11)", "entry");
                        Row.width('100%');
                        Row.height(38);
                    }, Row);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(formatMs(this.positionMs));
                        Text.debugLine("entry/src/main/ets/view/PlayerControlBar.ets(88:13)", "entry");
                        Text.fontSize(12);
                        Text.fontColor('#e7e7e7');
                    }, Text);
                    Text.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create('/');
                        Text.debugLine("entry/src/main/ets/view/PlayerControlBar.ets(91:13)", "entry");
                        Text.fontSize(12);
                        Text.fontColor('#e7e7e7');
                        Text.margin({ left: 4, right: 4 });
                    }, Text);
                    Text.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(formatMs(this.durationMs));
                        Text.debugLine("entry/src/main/ets/view/PlayerControlBar.ets(95:13)", "entry");
                        Text.fontSize(12);
                        Text.fontColor('#9f9e9e');
                    }, Text);
                    Text.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Slider.create({
                            value: this.positionMs,
                            min: 0,
                            max: this.durationMs > 0 ? this.durationMs : 100,
                            step: 1,
                            style: SliderStyle.OutSet
                        });
                        Slider.debugLine("entry/src/main/ets/view/PlayerControlBar.ets(99:13)", "entry");
                        Slider.layoutWeight(1);
                        Slider.trackThickness(3);
                        Slider.selectedColor('#379DF2');
                        Slider.trackColor('#999999');
                        Slider.blockColor('#379DF2');
                        Slider.margin({ left: 8, right: 8 });
                        Slider.onChange((value: number, mode: SliderChangeMode) => {
                            if (mode === SliderChangeMode.Begin) {
                                this.onSeekStart();
                            }
                            else if (mode === SliderChangeMode.Moving) {
                                this.onSeekChange(value);
                            }
                            else {
                                this.onSeekChange(value);
                                this.onSeekEnd(value);
                            }
                        });
                    }, Slider);
                    Row.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Row.create();
                        Row.debugLine("entry/src/main/ets/view/PlayerControlBar.ets(126:11)", "entry");
                        Row.width('100%');
                    }, Row);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(this.isFullScreen ? S.screenModelLarge : S.screenModelSmall);
                        Text.debugLine("entry/src/main/ets/view/PlayerControlBar.ets(127:13)", "entry");
                        Text.fontSize(12);
                        Text.fontColor('#379DF2');
                        Text.padding(5);
                        Text.onClick(() => this.onToggleScreenMode());
                    }, Text);
                    Text.pop();
                    Row.pop();
                    Column.pop();
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
}
