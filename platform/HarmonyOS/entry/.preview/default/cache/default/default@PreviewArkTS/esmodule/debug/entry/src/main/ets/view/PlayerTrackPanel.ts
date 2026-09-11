if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface PlayerTrackPanel_Params {
    bitrateLabels?: string[];
    audioLabels?: string[];
    subtitleLabels?: string[];
    externalLabels?: string[];
    definitionLabels?: string[];
    selectedBitrate?: number;
    selectedAudio?: number;
    selectedSubtitle?: number;
    selectedExternal?: number;
    selectedDefinition?: number;
    speed?: number;
    onSelectBitrate?: (position: number) => void;
    onSelectAudio?: (position: number) => void;
    onSelectSubtitle?: (position: number) => void;
    onSelectExternal?: (position: number) => void;
    onSelectDefinition?: (position: number) => void;
    onSpeedChange?: (speed: number) => void;
}
import { S } from "@bundle:com.cicada.player.demo/entry/ets/util/Strings";
/** Sentinel index meaning "auto bitrate" / "cancel external subtitle". */
export const TRACK_INDEX_AUTO: number = -1;
export class PlayerTrackPanel extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__bitrateLabels = new SynchedPropertyObjectOneWayPU(params.bitrateLabels, this, "bitrateLabels");
        this.__audioLabels = new SynchedPropertyObjectOneWayPU(params.audioLabels, this, "audioLabels");
        this.__subtitleLabels = new SynchedPropertyObjectOneWayPU(params.subtitleLabels, this, "subtitleLabels");
        this.__externalLabels = new SynchedPropertyObjectOneWayPU(params.externalLabels, this, "externalLabels");
        this.__definitionLabels = new SynchedPropertyObjectOneWayPU(params.definitionLabels, this, "definitionLabels");
        this.__selectedBitrate = new SynchedPropertySimpleOneWayPU(params.selectedBitrate, this, "selectedBitrate");
        this.__selectedAudio = new SynchedPropertySimpleOneWayPU(params.selectedAudio, this, "selectedAudio");
        this.__selectedSubtitle = new SynchedPropertySimpleOneWayPU(params.selectedSubtitle, this, "selectedSubtitle");
        this.__selectedExternal = new SynchedPropertySimpleOneWayPU(params.selectedExternal, this, "selectedExternal");
        this.__selectedDefinition = new SynchedPropertySimpleOneWayPU(params.selectedDefinition, this, "selectedDefinition");
        this.__speed = new SynchedPropertySimpleOneWayPU(params.speed, this, "speed");
        this.onSelectBitrate = () => { };
        this.onSelectAudio = () => { };
        this.onSelectSubtitle = () => { };
        this.onSelectExternal = () => { };
        this.onSelectDefinition = () => { };
        this.onSpeedChange = () => { };
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: PlayerTrackPanel_Params) {
        if (params.bitrateLabels === undefined) {
            this.__bitrateLabels.set([]);
        }
        if (params.audioLabels === undefined) {
            this.__audioLabels.set([]);
        }
        if (params.subtitleLabels === undefined) {
            this.__subtitleLabels.set([]);
        }
        if (params.externalLabels === undefined) {
            this.__externalLabels.set([]);
        }
        if (params.definitionLabels === undefined) {
            this.__definitionLabels.set([]);
        }
        if (params.selectedBitrate === undefined) {
            this.__selectedBitrate.set(-1);
        }
        if (params.selectedAudio === undefined) {
            this.__selectedAudio.set(-1);
        }
        if (params.selectedSubtitle === undefined) {
            this.__selectedSubtitle.set(-1);
        }
        if (params.selectedExternal === undefined) {
            this.__selectedExternal.set(-1);
        }
        if (params.selectedDefinition === undefined) {
            this.__selectedDefinition.set(-1);
        }
        if (params.speed === undefined) {
            this.__speed.set(1.0);
        }
        if (params.onSelectBitrate !== undefined) {
            this.onSelectBitrate = params.onSelectBitrate;
        }
        if (params.onSelectAudio !== undefined) {
            this.onSelectAudio = params.onSelectAudio;
        }
        if (params.onSelectSubtitle !== undefined) {
            this.onSelectSubtitle = params.onSelectSubtitle;
        }
        if (params.onSelectExternal !== undefined) {
            this.onSelectExternal = params.onSelectExternal;
        }
        if (params.onSelectDefinition !== undefined) {
            this.onSelectDefinition = params.onSelectDefinition;
        }
        if (params.onSpeedChange !== undefined) {
            this.onSpeedChange = params.onSpeedChange;
        }
    }
    updateStateVars(params: PlayerTrackPanel_Params) {
        this.__bitrateLabels.reset(params.bitrateLabels);
        this.__audioLabels.reset(params.audioLabels);
        this.__subtitleLabels.reset(params.subtitleLabels);
        this.__externalLabels.reset(params.externalLabels);
        this.__definitionLabels.reset(params.definitionLabels);
        this.__selectedBitrate.reset(params.selectedBitrate);
        this.__selectedAudio.reset(params.selectedAudio);
        this.__selectedSubtitle.reset(params.selectedSubtitle);
        this.__selectedExternal.reset(params.selectedExternal);
        this.__selectedDefinition.reset(params.selectedDefinition);
        this.__speed.reset(params.speed);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__bitrateLabels.purgeDependencyOnElmtId(rmElmtId);
        this.__audioLabels.purgeDependencyOnElmtId(rmElmtId);
        this.__subtitleLabels.purgeDependencyOnElmtId(rmElmtId);
        this.__externalLabels.purgeDependencyOnElmtId(rmElmtId);
        this.__definitionLabels.purgeDependencyOnElmtId(rmElmtId);
        this.__selectedBitrate.purgeDependencyOnElmtId(rmElmtId);
        this.__selectedAudio.purgeDependencyOnElmtId(rmElmtId);
        this.__selectedSubtitle.purgeDependencyOnElmtId(rmElmtId);
        this.__selectedExternal.purgeDependencyOnElmtId(rmElmtId);
        this.__selectedDefinition.purgeDependencyOnElmtId(rmElmtId);
        this.__speed.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__bitrateLabels.aboutToBeDeleted();
        this.__audioLabels.aboutToBeDeleted();
        this.__subtitleLabels.aboutToBeDeleted();
        this.__externalLabels.aboutToBeDeleted();
        this.__definitionLabels.aboutToBeDeleted();
        this.__selectedBitrate.aboutToBeDeleted();
        this.__selectedAudio.aboutToBeDeleted();
        this.__selectedSubtitle.aboutToBeDeleted();
        this.__selectedExternal.aboutToBeDeleted();
        this.__selectedDefinition.aboutToBeDeleted();
        this.__speed.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __bitrateLabels: SynchedPropertySimpleOneWayPU<string[]>;
    get bitrateLabels() {
        return this.__bitrateLabels.get();
    }
    set bitrateLabels(newValue: string[]) {
        this.__bitrateLabels.set(newValue);
    }
    private __audioLabels: SynchedPropertySimpleOneWayPU<string[]>;
    get audioLabels() {
        return this.__audioLabels.get();
    }
    set audioLabels(newValue: string[]) {
        this.__audioLabels.set(newValue);
    }
    private __subtitleLabels: SynchedPropertySimpleOneWayPU<string[]>;
    get subtitleLabels() {
        return this.__subtitleLabels.get();
    }
    set subtitleLabels(newValue: string[]) {
        this.__subtitleLabels.set(newValue);
    }
    private __externalLabels: SynchedPropertySimpleOneWayPU<string[]>;
    get externalLabels() {
        return this.__externalLabels.get();
    }
    set externalLabels(newValue: string[]) {
        this.__externalLabels.set(newValue);
    }
    private __definitionLabels: SynchedPropertySimpleOneWayPU<string[]>;
    get definitionLabels() {
        return this.__definitionLabels.get();
    }
    set definitionLabels(newValue: string[]) {
        this.__definitionLabels.set(newValue);
    }
    /** Selected position inside each group (-1 = nothing selected). */
    private __selectedBitrate: SynchedPropertySimpleOneWayPU<number>;
    get selectedBitrate() {
        return this.__selectedBitrate.get();
    }
    set selectedBitrate(newValue: number) {
        this.__selectedBitrate.set(newValue);
    }
    private __selectedAudio: SynchedPropertySimpleOneWayPU<number>;
    get selectedAudio() {
        return this.__selectedAudio.get();
    }
    set selectedAudio(newValue: number) {
        this.__selectedAudio.set(newValue);
    }
    private __selectedSubtitle: SynchedPropertySimpleOneWayPU<number>;
    get selectedSubtitle() {
        return this.__selectedSubtitle.get();
    }
    set selectedSubtitle(newValue: number) {
        this.__selectedSubtitle.set(newValue);
    }
    private __selectedExternal: SynchedPropertySimpleOneWayPU<number>;
    get selectedExternal() {
        return this.__selectedExternal.get();
    }
    set selectedExternal(newValue: number) {
        this.__selectedExternal.set(newValue);
    }
    private __selectedDefinition: SynchedPropertySimpleOneWayPU<number>;
    get selectedDefinition() {
        return this.__selectedDefinition.get();
    }
    set selectedDefinition(newValue: number) {
        this.__selectedDefinition.set(newValue);
    }
    /** Current speed, one of 0.5 / 1.0 / 1.5 / 2.0. */
    private __speed: SynchedPropertySimpleOneWayPU<number>;
    get speed() {
        return this.__speed.get();
    }
    set speed(newValue: number) {
        this.__speed.set(newValue);
    }
    private onSelectBitrate: (position: number) => void;
    private onSelectAudio: (position: number) => void;
    private onSelectSubtitle: (position: number) => void;
    private onSelectExternal: (position: number) => void;
    private onSelectDefinition: (position: number) => void;
    private onSpeedChange: (speed: number) => void;
    section(title: string, parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(title);
            Text.debugLine("entry/src/main/ets/view/PlayerTrackPanel.ets(56:5)", "entry");
            Text.fontSize(16);
            Text.fontColor(Color.Black);
            Text.margin({ top: 12, bottom: 6 });
            Text.width('100%');
        }, Text);
        Text.pop();
    }
    option(label: string, selected: boolean, action: () => void, parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(label);
            Text.debugLine("entry/src/main/ets/view/PlayerTrackPanel.ets(65:5)", "entry");
            Text.fontSize(14);
            Text.fontColor(selected ? Color.White : '#3F51B5');
            Text.textAlign(TextAlign.Center);
            Text.width('100%');
            Text.height(36);
            Text.borderRadius(4);
            Text.borderWidth(1);
            Text.borderColor('#3F51B5');
            Text.backgroundColor(selected ? '#3F51B5' : Color.Transparent);
            Text.margin({ bottom: 6 });
            Text.onClick(action);
        }, Text);
        Text.pop();
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Scroll.create();
            Scroll.debugLine("entry/src/main/ets/view/PlayerTrackPanel.ets(80:5)", "entry");
            Scroll.width('100%');
            Scroll.layoutWeight(1);
            Scroll.backgroundColor('#F5F5F5');
        }, Scroll);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/view/PlayerTrackPanel.ets(81:7)", "entry");
            Column.width('100%');
            Column.padding(12);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.definitionLabels.length > 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.section.bind(this)('清晰度');
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        ForEach.create();
                        const forEachItemGenFunction = (_item, position: number) => {
                            const label = _item;
                            this.option.bind(this)(label, position === this.selectedDefinition, () => this.onSelectDefinition(position));
                        };
                        this.forEachUpdateFunction(elmtId, this.definitionLabels, forEachItemGenFunction, (label: string, position: number) => `d-${position}-${label}`, true, true);
                    }, ForEach);
                    ForEach.pop();
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
            if (this.bitrateLabels.length > 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.section.bind(this)('码率');
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        ForEach.create();
                        const forEachItemGenFunction = (_item, position: number) => {
                            const label = _item;
                            this.option.bind(this)(label, position === this.selectedBitrate, () => this.onSelectBitrate(position));
                        };
                        this.forEachUpdateFunction(elmtId, this.bitrateLabels, forEachItemGenFunction, (label: string, position: number) => `b-${position}-${label}`, true, true);
                    }, ForEach);
                    ForEach.pop();
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
            if (this.audioLabels.length > 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.section.bind(this)('音轨');
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        ForEach.create();
                        const forEachItemGenFunction = (_item, position: number) => {
                            const label = _item;
                            this.option.bind(this)(label, position === this.selectedAudio, () => this.onSelectAudio(position));
                        };
                        this.forEachUpdateFunction(elmtId, this.audioLabels, forEachItemGenFunction, (label: string, position: number) => `a-${position}-${label}`, true, true);
                    }, ForEach);
                    ForEach.pop();
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
            if (this.externalLabels.length > 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.section.bind(this)('外挂字幕');
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        ForEach.create();
                        const forEachItemGenFunction = (_item, position: number) => {
                            const label = _item;
                            this.option.bind(this)(label, position === this.selectedExternal, () => this.onSelectExternal(position));
                        };
                        this.forEachUpdateFunction(elmtId, this.externalLabels, forEachItemGenFunction, (label: string, position: number) => `x-${position}-${label}`, true, true);
                    }, ForEach);
                    ForEach.pop();
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
            if (this.subtitleLabels.length > 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.section.bind(this)('字幕');
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        ForEach.create();
                        const forEachItemGenFunction = (_item, position: number) => {
                            const label = _item;
                            this.option.bind(this)(label, position === this.selectedSubtitle, () => this.onSelectSubtitle(position));
                        };
                        this.forEachUpdateFunction(elmtId, this.subtitleLabels, forEachItemGenFunction, (label: string, position: number) => `s-${position}-${label}`, true, true);
                    }, ForEach);
                    ForEach.pop();
                });
            }
            // ---- 倍速 (PlayerOperationFragment's speedMode group) ----
            else {
                this.ifElseBranchUpdateFunction(1, () => {
                });
            }
        }, If);
        If.pop();
        // ---- 倍速 (PlayerOperationFragment's speedMode group) ----
        this.section.bind(this)('倍速播放');
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            ForEach.create();
            const forEachItemGenFunction = _item => {
                const value = _item;
                this.option.bind(this)(this.speedLabel(value), value === this.speed, () => this.onSpeedChange(value));
            };
            this.forEachUpdateFunction(elmtId, [0.5, 1.0, 1.5, 2.0], forEachItemGenFunction, (value: number) => `sp-${value}`, false, false);
        }, ForEach);
        ForEach.pop();
        Column.pop();
        Scroll.pop();
    }
    /** Labels taken from strings.xml (cicada_speed_*). */
    private speedLabel(value: number): string {
        if (value === 0.5) {
            return S.speedOptfTimes;
        }
        if (value === 1.0) {
            return S.speedOneTimes;
        }
        if (value === 1.5) {
            return S.speedOptTimes;
        }
        return S.speedTwiceTimes;
    }
    rerender() {
        this.updateDirtyElements();
    }
}
