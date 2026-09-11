if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface SourceChooseListPage_Params {
    groupName?: string;
    samples?: SourceSample[];
    checked?: boolean[];
}
import type common from "@ohos:app.ability.common";
import promptAction from "@ohos:promptAction";
import router from "@ohos:router";
import { TitleBar } from "@bundle:com.cicada.player.demo/entry/ets/view/TitleBar";
import { SourceType } from "@bundle:com.cicada.player.demo/entry/ets/model/CicadaTypes";
import type { MultiPlayerSelection, PlayerRouteParams, SourceGroup, SourceGroupParams, SourceSample } from "@bundle:com.cicada.player.demo/entry/ets/model/CicadaTypes";
import { SourceListParser } from "@bundle:com.cicada.player.demo/entry/ets/util/SourceListParser";
import { PLAYER_TYPE_AVPLAYER, Settings } from "@bundle:com.cicada.player.demo/entry/ets/util/Settings";
import { S } from "@bundle:com.cicada.player.demo/entry/ets/util/Strings";
/** AppStorage key holding the entries chosen for multi-instance playback. */
export const MULTI_PLAYER_KEY: string = 'multiPlayerSelection';
class SourceChooseListPage extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__groupName = new ObservedPropertySimplePU('', this, "groupName");
        this.__samples = new ObservedPropertyObjectPU([], this, "samples");
        this.__checked = new ObservedPropertyObjectPU([], this, "checked");
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: SourceChooseListPage_Params) {
        if (params.groupName !== undefined) {
            this.groupName = params.groupName;
        }
        if (params.samples !== undefined) {
            this.samples = params.samples;
        }
        if (params.checked !== undefined) {
            this.checked = params.checked;
        }
    }
    updateStateVars(params: SourceChooseListPage_Params) {
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__groupName.purgeDependencyOnElmtId(rmElmtId);
        this.__samples.purgeDependencyOnElmtId(rmElmtId);
        this.__checked.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__groupName.aboutToBeDeleted();
        this.__samples.aboutToBeDeleted();
        this.__checked.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __groupName: ObservedPropertySimplePU<string>;
    get groupName() {
        return this.__groupName.get();
    }
    set groupName(newValue: string) {
        this.__groupName.set(newValue);
    }
    private __samples: ObservedPropertyObjectPU<SourceSample[]>;
    get samples() {
        return this.__samples.get();
    }
    set samples(newValue: SourceSample[]) {
        this.__samples.set(newValue);
    }
    /** Parallel to `samples`; kept as plain state so re-render is reliable. */
    private __checked: ObservedPropertyObjectPU<boolean[]>;
    get checked() {
        return this.__checked.get();
    }
    set checked(newValue: boolean[]) {
        this.__checked.set(newValue);
    }
    aboutToAppear(): void {
        const params = router.getParams() as SourceGroupParams;
        if (params !== undefined && params !== null && params.groupName !== undefined) {
            this.groupName = params.groupName;
        }
        const context = getContext(this) as common.UIAbilityContext;
        const groups: SourceGroup[] = SourceListParser.parse(context);
        const found: SourceSample[] = SourceListParser.samplesOf(groups, this.groupName);
        this.samples = found;
        const flags: boolean[] = [];
        for (let i = 0; i < found.length; i++) {
            flags.push(false);
        }
        this.checked = flags;
        if (found.length === 0) {
            // Android falls back to scanning the external-storage root for media
            // files. There is no shared storage root on HarmonyOS; use the picker
            // from the launcher page instead.
            promptAction.showToast({ message: S.noneMediaInfo, duration: 2000 });
        }
    }
    private toggle(index: number): void {
        const next: boolean[] = this.checked.slice();
        next[index] = !next[index];
        this.checked = next;
    }
    private play(sample: SourceSample): void {
        if (!SourceListParser.isPlayable(sample)) {
            return;
        }
        const params: PlayerRouteParams = {
            url: sample.url,
            playType: SourceType.URL,
            groupName: this.groupName
        };
        if (SourceListParser.hasExternalSubtitle(sample) && sample.subtitle !== undefined) {
            params.subtitleCn = sample.subtitle.cn;
            params.subtitleEn = sample.subtitle.en;
        }
        // The settings kernel switch decides which player handles the source.
        const target: string = Settings.getPlayerType() === PLAYER_TYPE_AVPLAYER
            ? 'pages/AvPlayerPage'
            : 'pages/Index';
        router.pushUrl({ url: target, params: params });
    }
    private collectChecked(): SourceSample[] {
        const out: SourceSample[] = [];
        for (let i = 0; i < this.samples.length; i++) {
            if (this.checked[i]) {
                out.push(this.samples[i]);
            }
        }
        return out;
    }
    private openMultiPlayer(): void {
        const chosen: SourceSample[] = this.collectChecked();
        if (chosen.length === 0) {
            promptAction.showToast({ message: S.noneMediaInfo, duration: 2000 });
            return;
        }
        // Android pushes every checked TypeInfo into a static list on the activity;
        // here the selection travels as JSON through AppStorage.
        const selections: MultiPlayerSelection[] = [];
        for (const sample of chosen) {
            selections.push({ url: sample.url, name: sample.name });
        }
        AppStorage.setOrCreate(MULTI_PLAYER_KEY, JSON.stringify(selections));
        router.pushUrl({ url: 'pages/MultiPlayerPage' });
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/pages/SourceChooseListPage.ets(114:5)", "entry");
            Column.width('100%');
            Column.height('100%');
            Column.backgroundColor('#FFFFFF');
        }, Column);
        {
            this.observeComponentCreation2((elmtId, isInitialRender) => {
                if (isInitialRender) {
                    let componentCall = new TitleBar(this, {
                        title: this.groupName,
                        rightText: S.titleMultipleInstances,
                        onBack: () => router.back(),
                        onRight: () => this.openMultiPlayer()
                    }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/SourceChooseListPage.ets", line: 115, col: 7 });
                    ViewPU.create(componentCall);
                    let paramsLambda = () => {
                        return {
                            title: this.groupName,
                            rightText: S.titleMultipleInstances,
                            onBack: () => router.back(),
                            onRight: () => this.openMultiPlayer()
                        };
                    };
                    componentCall.paramsGenerator_ = paramsLambda;
                }
                else {
                    this.updateStateVarsOfChildByElmtId(elmtId, {
                        title: this.groupName,
                        rightText: S.titleMultipleInstances
                    });
                }
            }, { name: "TitleBar" });
        }
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            List.create();
            List.debugLine("entry/src/main/ets/pages/SourceChooseListPage.ets(122:7)", "entry");
            List.layoutWeight(1);
            List.width('100%');
        }, List);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            ForEach.create();
            const forEachItemGenFunction = (_item, index: number) => {
                const sample = _item;
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
                        ListItem.onClick(() => this.play(sample));
                        ListItem.debugLine("entry/src/main/ets/pages/SourceChooseListPage.ets(124:11)", "entry");
                    };
                    const deepRenderFunction = (elmtId, isInitialRender) => {
                        itemCreation(elmtId, isInitialRender);
                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                            Column.create();
                            Column.debugLine("entry/src/main/ets/pages/SourceChooseListPage.ets(125:13)", "entry");
                            Column.width('100%');
                        }, Column);
                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                            Row.create();
                            Row.debugLine("entry/src/main/ets/pages/SourceChooseListPage.ets(126:15)", "entry");
                            Row.width('100%');
                            Row.padding({ left: 10, right: 10, top: 10, bottom: 10 });
                        }, Row);
                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                            Column.create();
                            Column.debugLine("entry/src/main/ets/pages/SourceChooseListPage.ets(127:17)", "entry");
                            Column.layoutWeight(1);
                            Column.alignItems(HorizontalAlign.Start);
                        }, Column);
                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                            Text.create(sample.name);
                            Text.debugLine("entry/src/main/ets/pages/SourceChooseListPage.ets(128:19)", "entry");
                            Text.fontSize(15);
                            Text.fontColor('#3F51B5');
                            Text.maxLines(1);
                            Text.textOverflow({ overflow: TextOverflow.Ellipsis });
                            Text.width('100%');
                        }, Text);
                        Text.pop();
                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                            Text.create(sample.url);
                            Text.debugLine("entry/src/main/ets/pages/SourceChooseListPage.ets(134:19)", "entry");
                            Text.fontSize(11);
                            Text.fontColor('#666666');
                            Text.maxLines(1);
                            Text.textOverflow({ overflow: TextOverflow.Ellipsis });
                            Text.width('100%');
                            Text.margin({ top: 4 });
                        }, Text);
                        Text.pop();
                        Column.pop();
                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                            Checkbox.create();
                            Checkbox.debugLine("entry/src/main/ets/pages/SourceChooseListPage.ets(145:17)", "entry");
                            Checkbox.select(this.checked[index]);
                            Checkbox.onChange(() => this.toggle(index));
                        }, Checkbox);
                        Checkbox.pop();
                        Row.pop();
                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                            Divider.create();
                            Divider.debugLine("entry/src/main/ets/pages/SourceChooseListPage.ets(152:15)", "entry");
                            Divider.strokeWidth(1);
                            Divider.color('#000000');
                        }, Divider);
                        Column.pop();
                        ListItem.pop();
                    };
                    this.observeComponentCreation2(itemCreation2, ListItem);
                    ListItem.pop();
                }
            };
            this.forEachUpdateFunction(elmtId, this.samples, forEachItemGenFunction, (sample: SourceSample, index: number) => `${index}-${sample.url}`, true, true);
        }, ForEach);
        ForEach.pop();
        List.pop();
        Column.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    static getEntryName(): string {
        return "SourceChooseListPage";
    }
}
registerNamedRoute(() => new SourceChooseListPage(undefined, {}), "", { bundleName: "com.cicada.player.demo", moduleName: "entry", pagePath: "pages/SourceChooseListPage", pageFullPath: "entry/src/main/ets/pages/SourceChooseListPage", integratedHsp: "false", moduleType: "followWithHap" });
