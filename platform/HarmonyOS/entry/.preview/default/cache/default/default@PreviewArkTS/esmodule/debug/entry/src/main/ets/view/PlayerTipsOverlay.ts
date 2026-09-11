if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface PlayerTipsOverlay_Params {
    loading?: boolean;
    loadPercent?: number;
    errorVisible?: boolean;
    errorCode?: number;
    errorMsg?: string;
    replayVisible?: boolean;
    netChangeVisible?: boolean;
    onRetry?: () => void;
    onReplay?: () => void;
    onContinuePlay?: () => void;
    onStopPlay?: () => void;
}
import { S } from "@bundle:com.cicada.player.demo/entry/ets/util/Strings";
export class PlayerTipsOverlay extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__loading = new SynchedPropertySimpleOneWayPU(params.loading, this, "loading");
        this.__loadPercent = new SynchedPropertySimpleOneWayPU(params.loadPercent, this, "loadPercent");
        this.__errorVisible = new SynchedPropertySimpleOneWayPU(params.errorVisible, this, "errorVisible");
        this.__errorCode = new SynchedPropertySimpleOneWayPU(params.errorCode, this, "errorCode");
        this.__errorMsg = new SynchedPropertySimpleOneWayPU(params.errorMsg, this, "errorMsg");
        this.__replayVisible = new SynchedPropertySimpleOneWayPU(params.replayVisible, this, "replayVisible");
        this.__netChangeVisible = new SynchedPropertySimpleOneWayPU(params.netChangeVisible, this, "netChangeVisible");
        this.onRetry = () => { };
        this.onReplay = () => { };
        this.onContinuePlay = () => { };
        this.onStopPlay = () => { };
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: PlayerTipsOverlay_Params) {
        if (params.loading === undefined) {
            this.__loading.set(false);
        }
        if (params.loadPercent === undefined) {
            this.__loadPercent.set(0);
        }
        if (params.errorVisible === undefined) {
            this.__errorVisible.set(false);
        }
        if (params.errorCode === undefined) {
            this.__errorCode.set(0);
        }
        if (params.errorMsg === undefined) {
            this.__errorMsg.set('');
        }
        if (params.replayVisible === undefined) {
            this.__replayVisible.set(false);
        }
        if (params.netChangeVisible === undefined) {
            this.__netChangeVisible.set(false);
        }
        if (params.onRetry !== undefined) {
            this.onRetry = params.onRetry;
        }
        if (params.onReplay !== undefined) {
            this.onReplay = params.onReplay;
        }
        if (params.onContinuePlay !== undefined) {
            this.onContinuePlay = params.onContinuePlay;
        }
        if (params.onStopPlay !== undefined) {
            this.onStopPlay = params.onStopPlay;
        }
    }
    updateStateVars(params: PlayerTipsOverlay_Params) {
        this.__loading.reset(params.loading);
        this.__loadPercent.reset(params.loadPercent);
        this.__errorVisible.reset(params.errorVisible);
        this.__errorCode.reset(params.errorCode);
        this.__errorMsg.reset(params.errorMsg);
        this.__replayVisible.reset(params.replayVisible);
        this.__netChangeVisible.reset(params.netChangeVisible);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__loading.purgeDependencyOnElmtId(rmElmtId);
        this.__loadPercent.purgeDependencyOnElmtId(rmElmtId);
        this.__errorVisible.purgeDependencyOnElmtId(rmElmtId);
        this.__errorCode.purgeDependencyOnElmtId(rmElmtId);
        this.__errorMsg.purgeDependencyOnElmtId(rmElmtId);
        this.__replayVisible.purgeDependencyOnElmtId(rmElmtId);
        this.__netChangeVisible.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__loading.aboutToBeDeleted();
        this.__loadPercent.aboutToBeDeleted();
        this.__errorVisible.aboutToBeDeleted();
        this.__errorCode.aboutToBeDeleted();
        this.__errorMsg.aboutToBeDeleted();
        this.__replayVisible.aboutToBeDeleted();
        this.__netChangeVisible.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __loading: SynchedPropertySimpleOneWayPU<boolean>;
    get loading() {
        return this.__loading.get();
    }
    set loading(newValue: boolean) {
        this.__loading.set(newValue);
    }
    private __loadPercent: SynchedPropertySimpleOneWayPU<number>;
    get loadPercent() {
        return this.__loadPercent.get();
    }
    set loadPercent(newValue: number) {
        this.__loadPercent.set(newValue);
    }
    private __errorVisible: SynchedPropertySimpleOneWayPU<boolean>;
    get errorVisible() {
        return this.__errorVisible.get();
    }
    set errorVisible(newValue: boolean) {
        this.__errorVisible.set(newValue);
    }
    private __errorCode: SynchedPropertySimpleOneWayPU<number>;
    get errorCode() {
        return this.__errorCode.get();
    }
    set errorCode(newValue: number) {
        this.__errorCode.set(newValue);
    }
    private __errorMsg: SynchedPropertySimpleOneWayPU<string>;
    get errorMsg() {
        return this.__errorMsg.get();
    }
    set errorMsg(newValue: string) {
        this.__errorMsg.set(newValue);
    }
    private __replayVisible: SynchedPropertySimpleOneWayPU<boolean>;
    get replayVisible() {
        return this.__replayVisible.get();
    }
    set replayVisible(newValue: boolean) {
        this.__replayVisible.set(newValue);
    }
    private __netChangeVisible: SynchedPropertySimpleOneWayPU<boolean>;
    get netChangeVisible() {
        return this.__netChangeVisible.get();
    }
    set netChangeVisible(newValue: boolean) {
        this.__netChangeVisible.set(newValue);
    }
    private onRetry: () => void;
    private onReplay: () => void;
    private onContinuePlay: () => void;
    private onStopPlay: () => void;
    outlinedButton(label: string, outlineColor: string, textColor: string, action: () => void, parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(label);
            Text.debugLine("entry/src/main/ets/view/PlayerTipsOverlay.ets(33:5)", "entry");
            Text.fontSize(13);
            Text.fontColor(textColor);
            Text.textAlign(TextAlign.Center);
            Text.width(82);
            Text.height(30);
            Text.borderRadius(4);
            Text.borderWidth(1);
            Text.borderColor(outlineColor);
            Text.onClick(action);
        }, Text);
        Text.pop();
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Stack.create();
            Stack.debugLine("entry/src/main/ets/view/PlayerTipsOverlay.ets(46:5)", "entry");
            Stack.width('100%');
            Stack.height('100%');
        }, Stack);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            // ---- loading (cicada_dialog_loading.xml) ----
            if (this.loading && !this.errorVisible && !this.replayVisible) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Column.create();
                        Column.debugLine("entry/src/main/ets/view/PlayerTipsOverlay.ets(49:9)", "entry");
                        Column.justifyContent(FlexAlign.Center);
                        Column.backgroundColor('#99000000');
                    }, Column);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        LoadingProgress.create();
                        LoadingProgress.debugLine("entry/src/main/ets/view/PlayerTipsOverlay.ets(50:11)", "entry");
                        LoadingProgress.width(28);
                        LoadingProgress.height(28);
                        LoadingProgress.color('#379DF2');
                    }, LoadingProgress);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(`${S.loading} ${this.loadPercent}%`);
                        Text.debugLine("entry/src/main/ets/view/PlayerTipsOverlay.ets(54:11)", "entry");
                        Text.fontSize(14);
                        Text.fontColor(Color.White);
                        Text.margin({ top: 10 });
                    }, Text);
                    Text.pop();
                    Column.pop();
                });
            }
            // ---- error (cicada_dialog_error.xml) ----
            else {
                this.ifElseBranchUpdateFunction(1, () => {
                });
            }
        }, If);
        If.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            // ---- error (cicada_dialog_error.xml) ----
            if (this.errorVisible) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Column.create();
                        Column.debugLine("entry/src/main/ets/view/PlayerTipsOverlay.ets(65:9)", "entry");
                        Column.justifyContent(FlexAlign.Center);
                        Column.padding(10);
                        Column.backgroundColor('#b2000000');
                    }, Column);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(this.errorMsg);
                        Text.debugLine("entry/src/main/ets/view/PlayerTipsOverlay.ets(66:11)", "entry");
                        Text.fontSize(15);
                        Text.fontColor(Color.White);
                        Text.textAlign(TextAlign.Center);
                        Text.maxLines(3);
                        Text.textOverflow({ overflow: TextOverflow.Ellipsis });
                    }, Text);
                    Text.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        // Android prints the code twice ("Error Code:2003 - 2003").
                        Text.create(`${S.errorCode}${this.errorCode} - ${this.errorCode}`);
                        Text.debugLine("entry/src/main/ets/view/PlayerTipsOverlay.ets(73:11)", "entry");
                        // Android prints the code twice ("Error Code:2003 - 2003").
                        Text.fontSize(12);
                        // Android prints the code twice ("Error Code:2003 - 2003").
                        Text.fontColor('#cccccc');
                        // Android prints the code twice ("Error Code:2003 - 2003").
                        Text.margin({ top: 6 });
                    }, Text);
                    // Android prints the code twice ("Error Code:2003 - 2003").
                    Text.pop();
                    this.outlinedButton.bind(this)(S.retry, '#379DF2', '#379DF2', () => this.onRetry());
                    Column.pop();
                });
            }
            // ---- completion / replay (cicada_dialog_replay.xml) ----
            else {
                this.ifElseBranchUpdateFunction(1, () => {
                });
            }
        }, If);
        If.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            // ---- completion / replay (cicada_dialog_replay.xml) ----
            if (this.replayVisible) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Column.create();
                        Column.debugLine("entry/src/main/ets/view/PlayerTipsOverlay.ets(86:9)", "entry");
                        Column.justifyContent(FlexAlign.Center);
                        Column.padding(10);
                        Column.backgroundColor('#b2000000');
                    }, Column);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(S.replayMsg);
                        Text.debugLine("entry/src/main/ets/view/PlayerTipsOverlay.ets(87:11)", "entry");
                        Text.fontSize(15);
                        Text.fontColor(Color.White);
                    }, Text);
                    Text.pop();
                    this.outlinedButton.bind(this)(S.replay, '#379DF2', '#379DF2', () => this.onReplay());
                    Column.pop();
                });
            }
            // ---- mobile-network change (cicada_dialog_netchange.xml) ----
            else {
                this.ifElseBranchUpdateFunction(1, () => {
                });
            }
        }, If);
        If.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            // ---- mobile-network change (cicada_dialog_netchange.xml) ----
            if (this.netChangeVisible) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Column.create();
                        Column.debugLine("entry/src/main/ets/view/PlayerTipsOverlay.ets(99:9)", "entry");
                        Column.justifyContent(FlexAlign.Center);
                        Column.padding(10);
                        Column.backgroundColor('#b2000000');
                    }, Column);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(S.netStateMobile);
                        Text.debugLine("entry/src/main/ets/view/PlayerTipsOverlay.ets(100:11)", "entry");
                        Text.fontSize(15);
                        Text.fontColor(Color.White);
                        Text.textAlign(TextAlign.Center);
                    }, Text);
                    Text.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Row.create({ space: 30 });
                        Row.debugLine("entry/src/main/ets/view/PlayerTipsOverlay.ets(104:11)", "entry");
                        Row.margin({ top: 12 });
                    }, Row);
                    this.outlinedButton.bind(this)(S.netStateMobileYes, '#FFFFFF', '#FFFFFF', () => this.onContinuePlay());
                    this.outlinedButton.bind(this)(S.netStateMobileNo, '#379DF2', '#379DF2', () => this.onStopPlay());
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
        Stack.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
}
