if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface SettingPage_Params {
    probePlayer?: CicadaPlayer;
    playerVersion?: string;
    hardwareDecoder?: boolean;
    deviceModel?: string;
    selectedPlayer?: string;
}
import type common from "@ohos:app.ability.common";
import deviceInfo from "@ohos:deviceInfo";
import promptAction from "@ohos:promptAction";
import router from "@ohos:router";
import { TitleBar } from "@bundle:com.cicada.player.demo/entry/ets/view/TitleBar";
import { CicadaPlayer } from "@bundle:com.cicada.player.demo/entry/ets/player/CicadaPlayer";
import { PLAYER_NAME_KEY, PLAYER_TYPE_AVPLAYER, Settings } from "@bundle:com.cicada.player.demo/entry/ets/util/Settings";
import { S } from "@bundle:com.cicada.player.demo/entry/ets/util/Strings";
class SettingPage extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.probePlayer = new CicadaPlayer();
        this.__playerVersion = new ObservedPropertySimplePU('', this, "playerVersion");
        this.__hardwareDecoder = new ObservedPropertySimplePU(true, this, "hardwareDecoder");
        this.__deviceModel = new ObservedPropertySimplePU('', this, "deviceModel");
        this.__selectedPlayer = new ObservedPropertySimplePU('CicadaPlayer', this, "selectedPlayer");
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: SettingPage_Params) {
        if (params.probePlayer !== undefined) {
            this.probePlayer = params.probePlayer;
        }
        if (params.playerVersion !== undefined) {
            this.playerVersion = params.playerVersion;
        }
        if (params.hardwareDecoder !== undefined) {
            this.hardwareDecoder = params.hardwareDecoder;
        }
        if (params.deviceModel !== undefined) {
            this.deviceModel = params.deviceModel;
        }
        if (params.selectedPlayer !== undefined) {
            this.selectedPlayer = params.selectedPlayer;
        }
    }
    updateStateVars(params: SettingPage_Params) {
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__playerVersion.purgeDependencyOnElmtId(rmElmtId);
        this.__hardwareDecoder.purgeDependencyOnElmtId(rmElmtId);
        this.__deviceModel.purgeDependencyOnElmtId(rmElmtId);
        this.__selectedPlayer.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__playerVersion.aboutToBeDeleted();
        this.__hardwareDecoder.aboutToBeDeleted();
        this.__deviceModel.aboutToBeDeleted();
        this.__selectedPlayer.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private probePlayer: CicadaPlayer;
    private __playerVersion: ObservedPropertySimplePU<string>;
    get playerVersion() {
        return this.__playerVersion.get();
    }
    set playerVersion(newValue: string) {
        this.__playerVersion.set(newValue);
    }
    private __hardwareDecoder: ObservedPropertySimplePU<boolean>;
    get hardwareDecoder() {
        return this.__hardwareDecoder.get();
    }
    set hardwareDecoder(newValue: boolean) {
        this.__hardwareDecoder.set(newValue);
    }
    private __deviceModel: ObservedPropertySimplePU<string>;
    get deviceModel() {
        return this.__deviceModel.get();
    }
    set deviceModel(newValue: string) {
        this.__deviceModel.set(newValue);
    }
    private __selectedPlayer: ObservedPropertySimplePU<string>;
    get selectedPlayer() {
        return this.__selectedPlayer.get();
    }
    set selectedPlayer(newValue: string) {
        this.__selectedPlayer.set(newValue);
    }
    aboutToAppear(): void {
        const context = getContext(this) as common.UIAbilityContext;
        Settings.init(context);
        this.hardwareDecoder = Settings.isHardwareDecoderEnabled();
        this.selectedPlayer = Settings.getPlayerType();
        // Android reads Build.MODEL; the OHOS deviceInfo module has no `model`
        // field, so the closest equivalent is productModel.
        this.deviceModel = deviceInfo.productModel;
        // Prefer the name cached by the player page; only build a throwaway player if
        // none was created yet in this session (CicadaGetPlayerName needs a handle and
        // there is no global version API).
        const cached: string | undefined = AppStorage.get<string>(PLAYER_NAME_KEY);
        if (cached !== undefined && cached !== null && cached.length > 0) {
            this.playerVersion = cached;
        }
        else if (this.selectedPlayer !== PLAYER_TYPE_AVPLAYER) {
            this.probePlayer.create();
            this.playerVersion = this.probePlayer.getPlayerName();
            this.probePlayer.release();
        }
        else {
            this.playerVersion = PLAYER_TYPE_AVPLAYER;
        }
    }
    private toast(message: string): void {
        promptAction.showToast({ message: message, duration: 2000 });
    }
    private onHardwareDecoderChange(checked: boolean): void {
        this.hardwareDecoder = checked;
        Settings.setHardwareDecoder(checked);
        this.toast(checked ? S.hardwareOnNextPlay : S.hardwareOffNextPlay);
    }
    private onBlacklist(): void {
        // Android registers the device model with CicadaPlayerFactory.addBlackDevice
        // and permanently forces software decoding. There is no C API for that, so
        // the local switch is turned off instead.
        this.hardwareDecoder = false;
        Settings.setHardwareDecoder(false);
        this.toast(S.success);
    }
    row(label: string, value: string, parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create();
            Row.debugLine("entry/src/main/ets/pages/SettingPage.ets(79:5)", "entry");
            Row.width('100%');
            Row.height(50);
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(label);
            Text.debugLine("entry/src/main/ets/pages/SettingPage.ets(80:7)", "entry");
            Text.fontSize(14);
            Text.fontColor('#333333');
            Text.layoutWeight(1);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(value);
            Text.debugLine("entry/src/main/ets/pages/SettingPage.ets(81:7)", "entry");
            Text.fontSize(13);
            Text.fontColor('#666666');
            Text.maxLines(1);
            Text.textOverflow({ overflow: TextOverflow.Ellipsis });
        }, Text);
        Text.pop();
        Row.pop();
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/pages/SettingPage.ets(89:5)", "entry");
            Column.width('100%');
            Column.height('100%');
            Column.backgroundColor('#FFFFFF');
        }, Column);
        {
            this.observeComponentCreation2((elmtId, isInitialRender) => {
                if (isInitialRender) {
                    let componentCall = new TitleBar(this, {
                        title: S.titleSetting,
                        onBack: () => router.back()
                    }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/SettingPage.ets", line: 90, col: 7 });
                    ViewPU.create(componentCall);
                    let paramsLambda = () => {
                        return {
                            title: S.titleSetting,
                            onBack: () => router.back()
                        };
                    };
                    componentCall.paramsGenerator_ = paramsLambda;
                }
                else {
                    this.updateStateVarsOfChildByElmtId(elmtId, {
                        title: S.titleSetting
                    });
                }
            }, { name: "TitleBar" });
        }
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Scroll.create();
            Scroll.debugLine("entry/src/main/ets/pages/SettingPage.ets(95:7)", "entry");
            Scroll.layoutWeight(1);
            Scroll.width('100%');
        }, Scroll);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/pages/SettingPage.ets(96:9)", "entry");
            Column.width('100%');
            Column.padding(10);
        }, Column);
        this.row.bind(this)('版本号 : ', this.playerVersion);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create();
            Row.debugLine("entry/src/main/ets/pages/SettingPage.ets(99:11)", "entry");
            Row.width('100%');
            Row.height(50);
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('硬解码');
            Text.debugLine("entry/src/main/ets/pages/SettingPage.ets(100:13)", "entry");
            Text.fontSize(14);
            Text.fontColor('#333333');
            Text.layoutWeight(1);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Toggle.create({ type: ToggleType.Switch, isOn: this.hardwareDecoder });
            Toggle.debugLine("entry/src/main/ets/pages/SettingPage.ets(101:13)", "entry");
            Toggle.onChange((isOn: boolean) => this.onHardwareDecoderChange(isOn));
        }, Toggle);
        Toggle.pop();
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('播放器选择');
            Text.debugLine("entry/src/main/ets/pages/SettingPage.ets(107:11)", "entry");
            Text.fontSize(14);
            Text.fontColor('#333333');
            Text.margin({ top: 10 });
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            // Only the Cicada kernel exists on HarmonyOS.
            Row.create();
            Row.debugLine("entry/src/main/ets/pages/SettingPage.ets(111:11)", "entry");
            // Only the Cicada kernel exists on HarmonyOS.
            Row.width('100%');
            // Only the Cicada kernel exists on HarmonyOS.
            Row.height(44);
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Radio.create({ value: 'CicadaPlayer', group: 'playerType' });
            Radio.debugLine("entry/src/main/ets/pages/SettingPage.ets(112:13)", "entry");
            Radio.checked(this.selectedPlayer === 'CicadaPlayer');
            Radio.onChange((isChecked: boolean) => {
                if (isChecked) {
                    this.selectedPlayer = 'CicadaPlayer';
                    Settings.setPlayerType('CicadaPlayer');
                }
            });
        }, Radio);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('CicadaPlayer');
            Text.debugLine("entry/src/main/ets/pages/SettingPage.ets(120:13)", "entry");
            Text.fontSize(14);
            Text.fontColor('#333333');
        }, Text);
        Text.pop();
        // Only the Cicada kernel exists on HarmonyOS.
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            // The ExoPlayer kernel has no HarmonyOS counterpart and is not ported.
            // Android's "MediaPlayer" (system player) slot is filled by AVPlayer,
            // which is HarmonyOS's own system player, so it is selectable here.
            Row.create();
            Row.debugLine("entry/src/main/ets/pages/SettingPage.ets(128:11)", "entry");
            // The ExoPlayer kernel has no HarmonyOS counterpart and is not ported.
            // Android's "MediaPlayer" (system player) slot is filled by AVPlayer,
            // which is HarmonyOS's own system player, so it is selectable here.
            Row.width('100%');
            // The ExoPlayer kernel has no HarmonyOS counterpart and is not ported.
            // Android's "MediaPlayer" (system player) slot is filled by AVPlayer,
            // which is HarmonyOS's own system player, so it is selectable here.
            Row.height(44);
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Radio.create({ value: 'AVPlayer', group: 'playerType' });
            Radio.debugLine("entry/src/main/ets/pages/SettingPage.ets(129:13)", "entry");
            Radio.checked(this.selectedPlayer === 'AVPlayer');
            Radio.onChange((isChecked: boolean) => {
                if (isChecked) {
                    this.selectedPlayer = 'AVPlayer';
                    Settings.setPlayerType('AVPlayer');
                }
            });
        }, Radio);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('AVPlayer (系统播放器)');
            Text.debugLine("entry/src/main/ets/pages/SettingPage.ets(137:13)", "entry");
            Text.fontSize(14);
            Text.fontColor('#333333');
        }, Text);
        Text.pop();
        // The ExoPlayer kernel has no HarmonyOS counterpart and is not ported.
        // Android's "MediaPlayer" (system player) slot is filled by AVPlayer,
        // which is HarmonyOS's own system player, so it is selectable here.
        Row.pop();
        this.row.bind(this)('机型 : ', this.deviceModel);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Button.createWithLabel('黑名单');
            Button.debugLine("entry/src/main/ets/pages/SettingPage.ets(144:11)", "entry");
            Button.onClick(() => this.onBlacklist());
            Button.width('100%');
            Button.height(44);
            Button.backgroundColor('#3F51B5');
            Button.fontColor(Color.White);
            Button.margin({ top: 10, bottom: 20 });
        }, Button);
        Button.pop();
        Column.pop();
        Scroll.pop();
        Column.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    static getEntryName(): string {
        return "SettingPage";
    }
}
registerNamedRoute(() => new SettingPage(undefined, {}), "", { bundleName: "com.cicada.player.demo", moduleName: "entry", pagePath: "pages/SettingPage", pageFullPath: "entry/src/main/ets/pages/SettingPage", integratedHsp: "false", moduleType: "followWithHap" });
