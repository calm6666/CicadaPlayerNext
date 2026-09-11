if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface MultiPlayerPage_Params {
    items?: MultiPlayerSelection[];
}
import router from "@ohos:router";
import { TitleBar } from "@bundle:com.cicada.player.demo/entry/ets/view/TitleBar";
import { MultiPlayerItem } from "@bundle:com.cicada.player.demo/entry/ets/view/MultiPlayerItem";
import type { MultiPlayerSelection } from '../model/CicadaTypes';
import { MULTI_PLAYER_KEY } from "@bundle:com.cicada.player.demo/entry/ets/pages/SourceChooseListPage";
import { S } from "@bundle:com.cicada.player.demo/entry/ets/util/Strings";
class MultiPlayerPage extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__items = new ObservedPropertyObjectPU([], this, "items");
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: MultiPlayerPage_Params) {
        if (params.items !== undefined) {
            this.items = params.items;
        }
    }
    updateStateVars(params: MultiPlayerPage_Params) {
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__items.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__items.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __items: ObservedPropertyObjectPU<MultiPlayerSelection[]>;
    get items() {
        return this.__items.get();
    }
    set items(newValue: MultiPlayerSelection[]) {
        this.__items.set(newValue);
    }
    aboutToAppear(): void {
        const stored: string | undefined = AppStorage.get<string>(MULTI_PLAYER_KEY);
        if (stored !== undefined && stored !== null && stored.length > 0) {
            try {
                this.items = JSON.parse(stored) as MultiPlayerSelection[];
            }
            catch (err) {
                this.items = [];
            }
        }
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/pages/MultiPlayerPage.ets(35:5)", "entry");
            Column.width('100%');
            Column.height('100%');
            Column.backgroundColor('#FFFFFF');
        }, Column);
        {
            this.observeComponentCreation2((elmtId, isInitialRender) => {
                if (isInitialRender) {
                    let componentCall = new TitleBar(this, {
                        title: S.titleMultipleInstances,
                        onBack: () => router.back()
                    }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/MultiPlayerPage.ets", line: 36, col: 7 });
                    ViewPU.create(componentCall);
                    let paramsLambda = () => {
                        return {
                            title: S.titleMultipleInstances,
                            onBack: () => router.back()
                        };
                    };
                    componentCall.paramsGenerator_ = paramsLambda;
                }
                else {
                    this.updateStateVarsOfChildByElmtId(elmtId, {
                        title: S.titleMultipleInstances
                    });
                }
            }, { name: "TitleBar" });
        }
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            List.create({ space: 10 });
            List.debugLine("entry/src/main/ets/pages/MultiPlayerPage.ets(41:7)", "entry");
            List.layoutWeight(1);
            List.width('100%');
            List.padding(6);
        }, List);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            ForEach.create();
            const forEachItemGenFunction = (_item, index: number) => {
                const item = _item;
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
                        ListItem.debugLine("entry/src/main/ets/pages/MultiPlayerPage.ets(43:11)", "entry");
                    };
                    const deepRenderFunction = (elmtId, isInitialRender) => {
                        itemCreation(elmtId, isInitialRender);
                        {
                            this.observeComponentCreation2((elmtId, isInitialRender) => {
                                if (isInitialRender) {
                                    let componentCall = new MultiPlayerItem(this, { url: item.url, label: item.name }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/MultiPlayerPage.ets", line: 44, col: 13 });
                                    ViewPU.create(componentCall);
                                    let paramsLambda = () => {
                                        return {
                                            url: item.url,
                                            label: item.name
                                        };
                                    };
                                    componentCall.paramsGenerator_ = paramsLambda;
                                }
                                else {
                                    this.updateStateVarsOfChildByElmtId(elmtId, {
                                        url: item.url, label: item.name
                                    });
                                }
                            }, { name: "MultiPlayerItem" });
                        }
                        ListItem.pop();
                    };
                    this.observeComponentCreation2(itemCreation2, ListItem);
                    ListItem.pop();
                }
            };
            this.forEachUpdateFunction(elmtId, this.items, forEachItemGenFunction, (item: MultiPlayerSelection, index: number) => `${index}-${item.url}`, true, true);
        }, ForEach);
        ForEach.pop();
        List.pop();
        Column.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    static getEntryName(): string {
        return "MultiPlayerPage";
    }
}
registerNamedRoute(() => new MultiPlayerPage(undefined, {}), "", { bundleName: "com.cicada.player.demo", moduleName: "entry", pagePath: "pages/MultiPlayerPage", pageFullPath: "entry/src/main/ets/pages/MultiPlayerPage", integratedHsp: "false", moduleType: "followWithHap" });
