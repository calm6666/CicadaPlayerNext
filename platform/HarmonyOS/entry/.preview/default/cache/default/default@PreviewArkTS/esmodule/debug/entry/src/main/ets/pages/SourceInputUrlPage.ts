if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface SourceInputUrlPage_Params {
    url?: string;
    dashUrl?: string;
    hlsUrl?: string;
    normalUrl?: string;
    manifestJson?: string;
}
import type common from "@ohos:app.ability.common";
import promptAction from "@ohos:promptAction";
import router from "@ohos:router";
import scanBarcode from "@hms:core.scan.scanBarcode";
import { TitleBar } from "@bundle:com.cicada.player.demo/entry/ets/view/TitleBar";
import { SourceType } from "@bundle:com.cicada.player.demo/entry/ets/model/CicadaTypes";
import type { PlayerRouteParams } from "@bundle:com.cicada.player.demo/entry/ets/model/CicadaTypes";
import { S, UrlDefaults } from "@bundle:com.cicada.player.demo/entry/ets/util/Strings";
class SourceInputUrlPage extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__url = new ObservedPropertySimplePU(UrlDefaults.INPUT_TEXT, this, "url");
        this.__dashUrl = new ObservedPropertySimplePU('', this, "dashUrl");
        this.__hlsUrl = new ObservedPropertySimplePU('', this, "hlsUrl");
        this.__normalUrl = new ObservedPropertySimplePU('', this, "normalUrl");
        this.__manifestJson = new ObservedPropertySimplePU('', this, "manifestJson");
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: SourceInputUrlPage_Params) {
        if (params.url !== undefined) {
            this.url = params.url;
        }
        if (params.dashUrl !== undefined) {
            this.dashUrl = params.dashUrl;
        }
        if (params.hlsUrl !== undefined) {
            this.hlsUrl = params.hlsUrl;
        }
        if (params.normalUrl !== undefined) {
            this.normalUrl = params.normalUrl;
        }
        if (params.manifestJson !== undefined) {
            this.manifestJson = params.manifestJson;
        }
    }
    updateStateVars(params: SourceInputUrlPage_Params) {
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__url.purgeDependencyOnElmtId(rmElmtId);
        this.__dashUrl.purgeDependencyOnElmtId(rmElmtId);
        this.__hlsUrl.purgeDependencyOnElmtId(rmElmtId);
        this.__normalUrl.purgeDependencyOnElmtId(rmElmtId);
        this.__manifestJson.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__url.aboutToBeDeleted();
        this.__dashUrl.aboutToBeDeleted();
        this.__hlsUrl.aboutToBeDeleted();
        this.__normalUrl.aboutToBeDeleted();
        this.__manifestJson.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __url: ObservedPropertySimplePU<string>;
    get url() {
        return this.__url.get();
    }
    set url(newValue: string) {
        this.__url.set(newValue);
    }
    private __dashUrl: ObservedPropertySimplePU<string>;
    get dashUrl() {
        return this.__dashUrl.get();
    }
    set dashUrl(newValue: string) {
        this.__dashUrl.set(newValue);
    }
    private __hlsUrl: ObservedPropertySimplePU<string>;
    get hlsUrl() {
        return this.__hlsUrl.get();
    }
    set hlsUrl(newValue: string) {
        this.__hlsUrl.set(newValue);
    }
    private __normalUrl: ObservedPropertySimplePU<string>;
    get normalUrl() {
        return this.__normalUrl.get();
    }
    set normalUrl(newValue: string) {
        this.__normalUrl.set(newValue);
    }
    private __manifestJson: ObservedPropertySimplePU<string>;
    get manifestJson() {
        return this.__manifestJson.get();
    }
    set manifestJson(newValue: string) {
        this.__manifestJson.set(newValue);
    }
    private toast(message: string): void {
        try {
            promptAction.showToast({ message: message, duration: 2000 });
        }
        catch (error) {
            // TODO: Implement error handling.
        }
    }
    /**
     * QR scan — the counterpart of the Android demo's zxing CaptureActivity.
     *
     * The Android contract (REQ_CODE 156, extra "qr_scan_result") has no ArkTS
     * equivalent: ScanKit brings up its own viewfinder with its own camera
     * permission handling and returns the decoded string directly. Kept
     * behaviour: the result is only written into the URL field, it is not
     * auto-played, and an empty/cancelled scan reports "Scan failed!".
     */
    private async scanQrCode(): Promise<void> {
        try {
            const context = getContext(this) as common.UIAbilityContext;
            // scanTypes keeps its default (all formats), which covers the Android
            // format set (ONE_D + QR_CODE + DATA_MATRIX).
            const options: scanBarcode.ScanOptions = { enableAlbum: true };
            const result: scanBarcode.ScanResult = await scanBarcode.startScanForResult(context, options);
            if (result === undefined || result === null || result.originalValue === undefined ||
                result.originalValue.length === 0) {
                this.toast(S.scanFailed);
                return;
            }
            this.url = result.originalValue;
        }
        catch (err) {
            this.toast(S.scanFailed);
        }
    }
    /**
     * HarmonyOS system-player route. Android's second button here was
     * "exoPlayer"; ExoPlayer has no HarmonyOS counterpart and is not ported, so
     * the slot is filled by AVPlayer. Like Android's exoPlayer button, an empty
     * field falls back to the hint.
     */
    private playAvPlayer(url: string): void {
        const target: string = this.valueOrHint(url, UrlDefaults.INPUT_HINT);
        if (target.length === 0) {
            this.toast(S.stsInputUrlFirst);
            return;
        }
        const params: PlayerRouteParams = { url: target, playType: SourceType.URL };
        router.pushUrl({ url: 'pages/AvPlayerPage', params: params });
    }
    private playUrl(url: string, playType: string): void {
        if (url.length === 0) {
            this.toast(S.stsInputUrlFirst);
            return;
        }
        const params: PlayerRouteParams = { url: url, playType: playType };
        router.pushUrl({ url: 'pages/Index', params: params });
    }
    /** Falls back to the field's hint, matching the Android behaviour. */
    private valueOrHint(value: string, hint: string): string {
        return value.length > 0 ? value : hint;
    }
    private playManifest(): void {
        const json: string = this.valueOrHint(this.manifestJson, UrlDefaults.MANIFEST_HINT);
        if (json.length === 0) {
            this.toast(S.stsManifestFirst);
            return;
        }
        const params: PlayerRouteParams = { url: json, playType: SourceType.MANIFEST };
        router.pushUrl({ url: 'pages/Index', params: params });
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/pages/SourceInputUrlPage.ets(100:5)", "entry");
            Column.width('100%');
            Column.height('100%');
            Column.backgroundColor('#FFFFFF');
        }, Column);
        {
            this.observeComponentCreation2((elmtId, isInitialRender) => {
                if (isInitialRender) {
                    let componentCall = new TitleBar(this, {
                        title: S.titleInputUrlItem,
                        rightText: S.titleScanning,
                        onBack: () => router.back(),
                        onRight: () => {
                            this.scanQrCode();
                        }
                    }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/SourceInputUrlPage.ets", line: 101, col: 7 });
                    ViewPU.create(componentCall);
                    let paramsLambda = () => {
                        return {
                            title: S.titleInputUrlItem,
                            rightText: S.titleScanning,
                            onBack: () => router.back(),
                            onRight: () => {
                                this.scanQrCode();
                            }
                        };
                    };
                    componentCall.paramsGenerator_ = paramsLambda;
                }
                else {
                    this.updateStateVarsOfChildByElmtId(elmtId, {
                        title: S.titleInputUrlItem,
                        rightText: S.titleScanning
                    });
                }
            }, { name: "TitleBar" });
        }
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Scroll.create();
            Scroll.debugLine("entry/src/main/ets/pages/SourceInputUrlPage.ets(110:7)", "entry");
            Scroll.layoutWeight(1);
            Scroll.width('100%');
        }, Scroll);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/pages/SourceInputUrlPage.ets(111:9)", "entry");
            Column.width('100%');
            Column.padding(12);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            // ---- plain URL + cicadaPlayer ----
            TextInput.create({ text: this.url, placeholder: UrlDefaults.INPUT_HINT });
            TextInput.debugLine("entry/src/main/ets/pages/SourceInputUrlPage.ets(113:11)", "entry");
            // ---- plain URL + cicadaPlayer ----
            TextInput.fontSize(13);
            // ---- plain URL + cicadaPlayer ----
            TextInput.width('100%');
            // ---- plain URL + cicadaPlayer ----
            TextInput.onChange((text: string) => {
                this.url = text;
            });
        }, TextInput);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Button.createWithLabel('cicadaPlayer');
            Button.debugLine("entry/src/main/ets/pages/SourceInputUrlPage.ets(119:11)", "entry");
            Button.width('100%');
            Button.height(44);
            Button.margin({ top: 6 });
            Button.backgroundColor('#3F51B5');
            Button.fontColor(Color.White);
            Button.onClick(() => this.playUrl(this.url, SourceType.URL));
        }, Button);
        Button.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            // Replaces Android's "exoPlayer" button (ExoPlayer is Android-only).
            Button.createWithLabel('AVPlayer');
            Button.debugLine("entry/src/main/ets/pages/SourceInputUrlPage.ets(128:11)", "entry");
            // Replaces Android's "exoPlayer" button (ExoPlayer is Android-only).
            Button.width('100%');
            // Replaces Android's "exoPlayer" button (ExoPlayer is Android-only).
            Button.height(44);
            // Replaces Android's "exoPlayer" button (ExoPlayer is Android-only).
            Button.margin({ top: 6 });
            // Replaces Android's "exoPlayer" button (ExoPlayer is Android-only).
            Button.backgroundColor('#3F51B5');
            // Replaces Android's "exoPlayer" button (ExoPlayer is Android-only).
            Button.fontColor(Color.White);
            // Replaces Android's "exoPlayer" button (ExoPlayer is Android-only).
            Button.onClick(() => this.playAvPlayer(this.url));
        }, Button);
        // Replaces Android's "exoPlayer" button (ExoPlayer is Android-only).
        Button.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            // ---- DASH ----
            TextInput.create({ text: this.dashUrl, placeholder: UrlDefaults.DASH_HINT });
            TextInput.debugLine("entry/src/main/ets/pages/SourceInputUrlPage.ets(137:11)", "entry");
            // ---- DASH ----
            TextInput.fontSize(13);
            // ---- DASH ----
            TextInput.width('100%');
            // ---- DASH ----
            TextInput.margin({ top: 16 });
            // ---- DASH ----
            TextInput.onChange((text: string) => {
                this.dashUrl = text;
            });
        }, TextInput);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Button.createWithLabel('播放 DASH (URL)');
            Button.debugLine("entry/src/main/ets/pages/SourceInputUrlPage.ets(144:11)", "entry");
            Button.width('100%');
            Button.height(44);
            Button.margin({ top: 6 });
            Button.backgroundColor('#3F51B5');
            Button.fontColor(Color.White);
            Button.onClick(() => this.playUrl(this.valueOrHint(this.dashUrl, UrlDefaults.DASH_HINT), SourceType.URL));
        }, Button);
        Button.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            // ---- HLS ----
            TextInput.create({ text: this.hlsUrl, placeholder: UrlDefaults.HLS_HINT });
            TextInput.debugLine("entry/src/main/ets/pages/SourceInputUrlPage.ets(153:11)", "entry");
            // ---- HLS ----
            TextInput.fontSize(13);
            // ---- HLS ----
            TextInput.width('100%');
            // ---- HLS ----
            TextInput.margin({ top: 16 });
            // ---- HLS ----
            TextInput.onChange((text: string) => {
                this.hlsUrl = text;
            });
        }, TextInput);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Button.createWithLabel('播放 HLS (URL)');
            Button.debugLine("entry/src/main/ets/pages/SourceInputUrlPage.ets(160:11)", "entry");
            Button.width('100%');
            Button.height(44);
            Button.margin({ top: 6 });
            Button.backgroundColor('#3F51B5');
            Button.fontColor(Color.White);
            Button.onClick(() => this.playUrl(this.valueOrHint(this.hlsUrl, UrlDefaults.HLS_HINT), SourceType.URL));
        }, Button);
        Button.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            // ---- plain progressive ----
            TextInput.create({ text: this.normalUrl, placeholder: UrlDefaults.NORMAL_HINT });
            TextInput.debugLine("entry/src/main/ets/pages/SourceInputUrlPage.ets(169:11)", "entry");
            // ---- plain progressive ----
            TextInput.fontSize(13);
            // ---- plain progressive ----
            TextInput.width('100%');
            // ---- plain progressive ----
            TextInput.margin({ top: 16 });
            // ---- plain progressive ----
            TextInput.onChange((text: string) => {
                this.normalUrl = text;
            });
        }, TextInput);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Button.createWithLabel('播放普通视频 (URL)');
            Button.debugLine("entry/src/main/ets/pages/SourceInputUrlPage.ets(176:11)", "entry");
            Button.width('100%');
            Button.height(44);
            Button.margin({ top: 6 });
            Button.backgroundColor('#3F51B5');
            Button.fontColor(Color.White);
            Button.onClick(() => this.playUrl(this.valueOrHint(this.normalUrl, UrlDefaults.NORMAL_HINT), SourceType.URL));
        }, Button);
        Button.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            // ---- object-based manifest (DRM capable) ----
            TextArea.create({ text: this.manifestJson, placeholder: UrlDefaults.MANIFEST_HINT });
            TextArea.debugLine("entry/src/main/ets/pages/SourceInputUrlPage.ets(185:11)", "entry");
            // ---- object-based manifest (DRM capable) ----
            TextArea.fontSize(12);
            // ---- object-based manifest (DRM capable) ----
            TextArea.width('100%');
            // ---- object-based manifest (DRM capable) ----
            TextArea.height(140);
            // ---- object-based manifest (DRM capable) ----
            TextArea.margin({ top: 16 });
            // ---- object-based manifest (DRM capable) ----
            TextArea.onChange((text: string) => {
                this.manifestJson = text;
            });
        }, TextArea);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Button.createWithLabel('对象播放 (JSON 传入)');
            Button.debugLine("entry/src/main/ets/pages/SourceInputUrlPage.ets(193:11)", "entry");
            Button.width('100%');
            Button.height(44);
            Button.margin({ top: 6, bottom: 20 });
            Button.backgroundColor('#3F51B5');
            Button.fontColor(Color.White);
            Button.onClick(() => this.playManifest());
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
        return "SourceInputUrlPage";
    }
}
registerNamedRoute(() => new SourceInputUrlPage(undefined, {}), "", { bundleName: "com.cicada.player.demo", moduleName: "entry", pagePath: "pages/SourceInputUrlPage", pageFullPath: "entry/src/main/ets/pages/SourceInputUrlPage", integratedHsp: "false", moduleType: "followWithHap" });
