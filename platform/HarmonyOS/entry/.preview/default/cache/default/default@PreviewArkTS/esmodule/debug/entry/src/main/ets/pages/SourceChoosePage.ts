if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface SourceChoosePage_Params {
    groups?: SourceGroup[];
    groupNames?: string[];
    pickedPath?: string;
}
import type common from "@ohos:app.ability.common";
import picker from "@ohos:file.picker";
import promptAction from "@ohos:promptAction";
import router from "@ohos:router";
import { TitleBar } from "@bundle:com.cicada.player.demo/entry/ets/view/TitleBar";
import { SourceType } from "@bundle:com.cicada.player.demo/entry/ets/model/CicadaTypes";
import type { SourceGroup, SourceGroupParams } from "@bundle:com.cicada.player.demo/entry/ets/model/CicadaTypes";
import { SourceListParser } from "@bundle:com.cicada.player.demo/entry/ets/util/SourceListParser";
import { LocalVideoSource } from "@bundle:com.cicada.player.demo/entry/ets/util/LocalVideoSource";
import { PLAYER_TYPE_AVPLAYER, Settings } from "@bundle:com.cicada.player.demo/entry/ets/util/Settings";
import { S } from "@bundle:com.cicada.player.demo/entry/ets/util/Strings";
class SourceChoosePage extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.groups = [];
        this.__groupNames = new ObservedPropertyObjectPU([], this, "groupNames");
        this.__pickedPath = new ObservedPropertySimplePU('', this, "pickedPath");
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: SourceChoosePage_Params) {
        if (params.groups !== undefined) {
            this.groups = params.groups;
        }
        if (params.groupNames !== undefined) {
            this.groupNames = params.groupNames;
        }
        if (params.pickedPath !== undefined) {
            this.pickedPath = params.pickedPath;
        }
    }
    updateStateVars(params: SourceChoosePage_Params) {
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__groupNames.purgeDependencyOnElmtId(rmElmtId);
        this.__pickedPath.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__groupNames.aboutToBeDeleted();
        this.__pickedPath.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private groups: SourceGroup[];
    private __groupNames: ObservedPropertyObjectPU<string[]>;
    get groupNames() {
        return this.__groupNames.get();
    }
    set groupNames(newValue: string[]) {
        this.__groupNames.set(newValue);
    }
    private __pickedPath: ObservedPropertySimplePU<string>;
    get pickedPath() {
        return this.__pickedPath.get();
    }
    set pickedPath(newValue: string) {
        this.__pickedPath.set(newValue);
    }
    aboutToAppear(): void {
        const context = getContext(this) as common.UIAbilityContext;
        this.groups = SourceListParser.parse(context);
        this.groupNames = SourceListParser.groupNames(this.groups);
    }
    private toast(message: string): void {
        promptAction.showToast({ message: message, duration: 2000 });
    }
    private openGroup(groupName: string): void {
        const params: SourceGroupParams = { groupName: groupName };
        router.pushUrl({ url: 'pages/SourceChooseListPage', params: params });
    }
    private openInputUrl(): void {
        router.pushUrl({ url: 'pages/SourceInputUrlPage' });
    }
    private openSetting(): void {
        router.pushUrl({ url: 'pages/SettingPage' });
    }
    /**
     * Local video pick: ACTION_OPEN_DOCUMENT + copyVideoToCache.
     *
     * The picker returns a document URI (file://docs/...), which the player cannot
     * open — feeding it straight to CicadaSetDataSourceWithUrl fails in PREPARINIT
     * with ENOENT ("No such file or directory"). So the file is copied into the
     * app cache first and the sandbox path is what gets played, exactly like the
     * Android demo's copy step.
     */
    private async pickLocalVideo(): Promise<void> {
        let uri: string = '';
        try {
            const documentPicker = new picker.DocumentViewPicker(getContext(this));
            const options = new picker.DocumentSelectOptions();
            options.maxSelectNumber = 1;
            options.fileSuffixFilters = ['.mp4', '.mkv', '.flv', '.m3u8', '.mov', '.mp3'];
            const uris: string[] = await documentPicker.select(options);
            if (uris === undefined || uris === null || uris.length === 0) {
                return;
            }
            uri = uris[0];
        }
        catch (err) {
            // The picker itself could not be opened.
            this.toast(S.pickVideoFailed);
            return;
        }
        // The AVPlayer kernel accepts a descriptor directly (fdSrc), so the raw URI
        // is handed over and the page opens the fd itself — no copy, no path
        // resolution needed.
        if (Settings.getPlayerType() === PLAYER_TYPE_AVPLAYER) {
            router.pushUrl({
                url: 'pages/AvPlayerPage',
                params: { url: uri, playType: SourceType.LOCAL, groupName: 'Local' }
            });
            return;
        }
        // Cicada kernel: FFmpeg needs a path. LocalVideoSource resolves one without
        // copying (real path when reachable, otherwise /proc/self/fd/<fd>).
        const localPath: string = LocalVideoSource.resolve(uri);
        if (localPath.length === 0) {
            this.toast(S.openVideoFailed);
            return;
        }
        this.pickedPath = localPath;
        router.pushUrl({
            url: 'pages/Index',
            params: { url: this.pickedPath, playType: SourceType.LOCAL, groupName: 'Local' }
        });
    }
    primaryButton(label: string, action: () => void, parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Button.createWithLabel(label);
            Button.debugLine("entry/src/main/ets/pages/SourceChoosePage.ets(110:5)", "entry");
            Button.width('100%');
            Button.height(50);
            Button.margin({ top: 8 });
            Button.backgroundColor('#3F51B5');
            Button.fontColor(Color.White);
            Button.onClick(action);
        }, Button);
        Button.pop();
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/pages/SourceChoosePage.ets(120:5)", "entry");
            Column.width('100%');
            Column.height('100%');
            Column.backgroundColor('#FFFFFF');
        }, Column);
        {
            this.observeComponentCreation2((elmtId, isInitialRender) => {
                if (isInitialRender) {
                    let componentCall = new TitleBar(this, {
                        title: S.titleChooseItem,
                        rightText: S.titleSetting,
                        showBack: false,
                        onRight: () => this.openSetting()
                    }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/SourceChoosePage.ets", line: 121, col: 7 });
                    ViewPU.create(componentCall);
                    let paramsLambda = () => {
                        return {
                            title: S.titleChooseItem,
                            rightText: S.titleSetting,
                            showBack: false,
                            onRight: () => this.openSetting()
                        };
                    };
                    componentCall.paramsGenerator_ = paramsLambda;
                }
                else {
                    this.updateStateVarsOfChildByElmtId(elmtId, {
                        title: S.titleChooseItem,
                        rightText: S.titleSetting,
                        showBack: false
                    });
                }
            }, { name: "TitleBar" });
        }
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/pages/SourceChoosePage.ets(128:7)", "entry");
            Column.layoutWeight(1);
            Column.width('100%');
            Column.padding(10);
        }, Column);
        this.primaryButton.bind(this)(S.btnInputUrl, () => this.openInputUrl());
        this.primaryButton.bind(this)(S.btnLocalVideo, () => {
            this.pickLocalVideo();
        });
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            List.create({ space: 10 });
            List.debugLine("entry/src/main/ets/pages/SourceChoosePage.ets(134:9)", "entry");
            List.layoutWeight(1);
            List.width('100%');
            List.margin({ top: 20 });
        }, List);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            ForEach.create();
            const forEachItemGenFunction = _item => {
                const groupName = _item;
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
                        ListItem.onClick(() => this.openGroup(groupName));
                        ListItem.debugLine("entry/src/main/ets/pages/SourceChoosePage.ets(136:13)", "entry");
                    };
                    const deepRenderFunction = (elmtId, isInitialRender) => {
                        itemCreation(elmtId, isInitialRender);
                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                            Text.create(groupName);
                            Text.debugLine("entry/src/main/ets/pages/SourceChoosePage.ets(137:15)", "entry");
                            Text.width('100%');
                            Text.height(50);
                            Text.fontSize(16);
                            Text.fontColor(Color.White);
                            Text.textAlign(TextAlign.Center);
                            Text.backgroundColor('#dddddd');
                            Text.borderRadius(4);
                        }, Text);
                        Text.pop();
                        ListItem.pop();
                    };
                    this.observeComponentCreation2(itemCreation2, ListItem);
                    ListItem.pop();
                }
            };
            this.forEachUpdateFunction(elmtId, this.groupNames, forEachItemGenFunction, (groupName: string) => groupName, false, false);
        }, ForEach);
        ForEach.pop();
        List.pop();
        Column.pop();
        Column.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    static getEntryName(): string {
        return "SourceChoosePage";
    }
}
registerNamedRoute(() => new SourceChoosePage(undefined, {}), "", { bundleName: "com.cicada.player.demo", moduleName: "entry", pagePath: "pages/SourceChoosePage", pageFullPath: "entry/src/main/ets/pages/SourceChoosePage", integratedHsp: "false", moduleType: "followWithHap" });
