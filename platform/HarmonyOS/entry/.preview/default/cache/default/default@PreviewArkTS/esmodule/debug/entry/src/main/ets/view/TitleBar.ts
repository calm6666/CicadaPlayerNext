if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface TitleBar_Params {
    title?: string;
    rightText?: string;
    showBack?: boolean;
    onBack?: () => void;
    onRight?: () => void;
}
export class TitleBar extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__title = new SynchedPropertySimpleOneWayPU(params.title, this, "title");
        this.__rightText = new SynchedPropertySimpleOneWayPU(params.rightText, this, "rightText");
        this.__showBack = new SynchedPropertySimpleOneWayPU(params.showBack, this, "showBack");
        this.onBack = () => { };
        this.onRight = () => { };
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: TitleBar_Params) {
        if (params.title === undefined) {
            this.__title.set('');
        }
        if (params.rightText === undefined) {
            this.__rightText.set('');
        }
        if (params.showBack === undefined) {
            this.__showBack.set(true);
        }
        if (params.onBack !== undefined) {
            this.onBack = params.onBack;
        }
        if (params.onRight !== undefined) {
            this.onRight = params.onRight;
        }
    }
    updateStateVars(params: TitleBar_Params) {
        this.__title.reset(params.title);
        this.__rightText.reset(params.rightText);
        this.__showBack.reset(params.showBack);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__title.purgeDependencyOnElmtId(rmElmtId);
        this.__rightText.purgeDependencyOnElmtId(rmElmtId);
        this.__showBack.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__title.aboutToBeDeleted();
        this.__rightText.aboutToBeDeleted();
        this.__showBack.aboutToBeDeleted();
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
    private __rightText: SynchedPropertySimpleOneWayPU<string>;
    get rightText() {
        return this.__rightText.get();
    }
    set rightText(newValue: string) {
        this.__rightText.set(newValue);
    }
    /** When false the back arrow is hidden (used by the launcher page). */
    private __showBack: SynchedPropertySimpleOneWayPU<boolean>;
    get showBack() {
        return this.__showBack.get();
    }
    set showBack(newValue: boolean) {
        this.__showBack.set(newValue);
    }
    private onBack: () => void;
    private onRight: () => void;
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/view/TitleBar.ets(16:5)", "entry");
            Column.width('100%');
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create();
            Row.debugLine("entry/src/main/ets/view/TitleBar.ets(17:7)", "entry");
            Row.width('100%');
            Row.height(50);
            Row.backgroundColor('#1e222d');
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.showBack) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create('←');
                        Text.debugLine("entry/src/main/ets/view/TitleBar.ets(19:11)", "entry");
                        Text.fontSize(22);
                        Text.fontColor('#FFFFFF');
                        Text.width(40);
                        Text.height('100%');
                        Text.textAlign(TextAlign.Center);
                        Text.onClick(() => this.onBack());
                    }, Text);
                    Text.pop();
                });
            }
            else {
                this.ifElseBranchUpdateFunction(1, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Blank.create();
                        Blank.debugLine("entry/src/main/ets/view/TitleBar.ets(27:11)", "entry");
                        Blank.width(40);
                    }, Blank);
                    Blank.pop();
                });
            }
        }, If);
        If.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.title);
            Text.debugLine("entry/src/main/ets/view/TitleBar.ets(30:9)", "entry");
            Text.fontSize(16);
            Text.fontWeight(FontWeight.Bold);
            Text.fontColor('#FFFFFF');
            Text.layoutWeight(1);
            Text.textAlign(TextAlign.Center);
            Text.maxLines(1);
            Text.textOverflow({ overflow: TextOverflow.Ellipsis });
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.rightText);
            Text.debugLine("entry/src/main/ets/view/TitleBar.ets(39:9)", "entry");
            Text.fontSize(14);
            Text.fontColor('#379DF2');
            Text.width(90);
            Text.height('100%');
            Text.textAlign(TextAlign.End);
            Text.onClick(() => this.onRight());
        }, Text);
        Text.pop();
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Divider.create();
            Divider.debugLine("entry/src/main/ets/view/TitleBar.ets(51:7)", "entry");
            Divider.strokeWidth(1);
            Divider.color('#000000');
        }, Divider);
        Column.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
}
