if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface SnapShotPage_Params {
    imagePath?: string;
    timerId?: number;
}
import router from "@ohos:router";
import { TitleBar } from "@bundle:com.cicada.player.demo/entry/ets/view/TitleBar";
import { S } from "@bundle:com.cicada.player.demo/entry/ets/util/Strings";
/** router params for this page. */
export interface SnapShotRouteParams {
    path: string;
}
const AUTO_FINISH_MS: number = 5000;
class SnapShotPage extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__imagePath = new ObservedPropertySimplePU('', this, "imagePath");
        this.timerId = -1;
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: SnapShotPage_Params) {
        if (params.imagePath !== undefined) {
            this.imagePath = params.imagePath;
        }
        if (params.timerId !== undefined) {
            this.timerId = params.timerId;
        }
    }
    updateStateVars(params: SnapShotPage_Params) {
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__imagePath.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__imagePath.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __imagePath: ObservedPropertySimplePU<string>;
    get imagePath() {
        return this.__imagePath.get();
    }
    set imagePath(newValue: string) {
        this.__imagePath.set(newValue);
    }
    private timerId: number;
    aboutToAppear(): void {
        const params = router.getParams() as SnapShotRouteParams;
        if (params !== undefined && params !== null && params.path !== undefined) {
            this.imagePath = params.path;
        }
        this.timerId = setTimeout(() => {
            router.back();
        }, AUTO_FINISH_MS);
    }
    aboutToDisappear(): void {
        if (this.timerId >= 0) {
            clearTimeout(this.timerId);
            this.timerId = -1;
        }
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/pages/SnapShotPage.ets(42:5)", "entry");
            Column.width('100%');
            Column.height('100%');
        }, Column);
        {
            this.observeComponentCreation2((elmtId, isInitialRender) => {
                if (isInitialRender) {
                    let componentCall = new TitleBar(this, {
                        title: S.titleScreenShot,
                        onBack: () => router.back()
                    }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/SnapShotPage.ets", line: 43, col: 7 });
                    ViewPU.create(componentCall);
                    let paramsLambda = () => {
                        return {
                            title: S.titleScreenShot,
                            onBack: () => router.back()
                        };
                    };
                    componentCall.paramsGenerator_ = paramsLambda;
                }
                else {
                    this.updateStateVarsOfChildByElmtId(elmtId, {
                        title: S.titleScreenShot
                    });
                }
            }, { name: "TitleBar" });
        }
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/pages/SnapShotPage.ets(48:7)", "entry");
            Column.layoutWeight(1);
            Column.width('100%');
            Column.justifyContent(FlexAlign.Center);
            Column.backgroundColor(Color.Black);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.imagePath.length > 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Image.create(`file://${this.imagePath}`);
                        Image.debugLine("entry/src/main/ets/pages/SnapShotPage.ets(50:11)", "entry");
                        Image.objectFit(ImageFit.Contain);
                        Image.width('100%');
                        Image.layoutWeight(1);
                    }, Image);
                });
            }
            else {
                this.ifElseBranchUpdateFunction(1, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(S.snapShotFailure);
                        Text.debugLine("entry/src/main/ets/pages/SnapShotPage.ets(55:11)", "entry");
                        Text.fontSize(15);
                        Text.fontColor('#666666');
                    }, Text);
                    Text.pop();
                });
            }
        }, If);
        If.pop();
        Column.pop();
        Column.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    static getEntryName(): string {
        return "SnapShotPage";
    }
}
registerNamedRoute(() => new SnapShotPage(undefined, {}), "", { bundleName: "com.cicada.player.demo", moduleName: "entry", pagePath: "pages/SnapShotPage", pageFullPath: "entry/src/main/ets/pages/SnapShotPage", integratedHsp: "false", moduleType: "followWithHap" });
