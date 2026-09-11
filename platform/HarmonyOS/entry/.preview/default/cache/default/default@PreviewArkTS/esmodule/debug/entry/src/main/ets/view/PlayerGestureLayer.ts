if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface PlayerGestureLayer_Params {
    locked?: boolean;
    durationMs?: number;
    positionMs?: number;
    /** Volume as 0..100; sampled once at gesture start, matching Android. */
    getVolumePercent?: () => number;
    /**
     * Current brightness as 0..100, also sampled at gesture start. Android reads
     * activity.window.attributes.screenBrightness, which is a "system default"
     * sentinel until the app sets it; HarmonyOS offers no read-back, so the page
     * owns the value it last applied.
     */
    getBrightnessPercent?: () => number;
    onBrightness?: (percent: number) => void;
    onVolume?: (percent: number) => void;
    onSeekTo?: (positionMs: number) => void;
    onTogglePlay?: () => void;
    viewWidth?: number;
    viewHeight?: number;
    startX?: number;
    startY?: number;
    axis?: GestureAxis;
    side?: GestureSide;
    startVolumePercent?: number;
    startBrightnessPercent?: number;
    pendingSeekMs?: number;
    indicatorVisible?: boolean;
    indicatorIcon?: string;
    indicatorText?: string;
}
import { formatMs } from "@bundle:com.cicada.player.demo/entry/ets/util/TimeFormater";
const ENABLE_HORIZONTAL_SEEK: boolean = true;
/** Android: dimen/cicada_gesture_dialog_size. */
const DIALOG_SIZE: number = 155;
type GestureAxis = 'none' | 'vertical' | 'horizontal';
type GestureSide = 'none' | 'left' | 'right';
export class PlayerGestureLayer extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__locked = new SynchedPropertySimpleOneWayPU(params.locked, this, "locked");
        this.__durationMs = new SynchedPropertySimpleOneWayPU(params.durationMs, this, "durationMs");
        this.__positionMs = new SynchedPropertySimpleOneWayPU(params.positionMs, this, "positionMs");
        this.getVolumePercent = () => 100;
        this.getBrightnessPercent = () => 50;
        this.onBrightness = () => { };
        this.onVolume = () => { };
        this.onSeekTo = () => { };
        this.onTogglePlay = () => { };
        this.viewWidth = 0;
        this.viewHeight = 0;
        this.startX = 0;
        this.startY = 0;
        this.axis = 'none';
        this.side = 'none';
        this.startVolumePercent = 100;
        this.startBrightnessPercent = 50;
        this.pendingSeekMs = 0;
        this.__indicatorVisible = new ObservedPropertySimplePU(false, this, "indicatorVisible");
        this.__indicatorIcon = new ObservedPropertySimplePU('', this, "indicatorIcon");
        this.__indicatorText = new ObservedPropertySimplePU('', this, "indicatorText");
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: PlayerGestureLayer_Params) {
        if (params.locked === undefined) {
            this.__locked.set(false);
        }
        if (params.durationMs === undefined) {
            this.__durationMs.set(0);
        }
        if (params.positionMs === undefined) {
            this.__positionMs.set(0);
        }
        if (params.getVolumePercent !== undefined) {
            this.getVolumePercent = params.getVolumePercent;
        }
        if (params.getBrightnessPercent !== undefined) {
            this.getBrightnessPercent = params.getBrightnessPercent;
        }
        if (params.onBrightness !== undefined) {
            this.onBrightness = params.onBrightness;
        }
        if (params.onVolume !== undefined) {
            this.onVolume = params.onVolume;
        }
        if (params.onSeekTo !== undefined) {
            this.onSeekTo = params.onSeekTo;
        }
        if (params.onTogglePlay !== undefined) {
            this.onTogglePlay = params.onTogglePlay;
        }
        if (params.viewWidth !== undefined) {
            this.viewWidth = params.viewWidth;
        }
        if (params.viewHeight !== undefined) {
            this.viewHeight = params.viewHeight;
        }
        if (params.startX !== undefined) {
            this.startX = params.startX;
        }
        if (params.startY !== undefined) {
            this.startY = params.startY;
        }
        if (params.axis !== undefined) {
            this.axis = params.axis;
        }
        if (params.side !== undefined) {
            this.side = params.side;
        }
        if (params.startVolumePercent !== undefined) {
            this.startVolumePercent = params.startVolumePercent;
        }
        if (params.startBrightnessPercent !== undefined) {
            this.startBrightnessPercent = params.startBrightnessPercent;
        }
        if (params.pendingSeekMs !== undefined) {
            this.pendingSeekMs = params.pendingSeekMs;
        }
        if (params.indicatorVisible !== undefined) {
            this.indicatorVisible = params.indicatorVisible;
        }
        if (params.indicatorIcon !== undefined) {
            this.indicatorIcon = params.indicatorIcon;
        }
        if (params.indicatorText !== undefined) {
            this.indicatorText = params.indicatorText;
        }
    }
    updateStateVars(params: PlayerGestureLayer_Params) {
        this.__locked.reset(params.locked);
        this.__durationMs.reset(params.durationMs);
        this.__positionMs.reset(params.positionMs);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__locked.purgeDependencyOnElmtId(rmElmtId);
        this.__durationMs.purgeDependencyOnElmtId(rmElmtId);
        this.__positionMs.purgeDependencyOnElmtId(rmElmtId);
        this.__indicatorVisible.purgeDependencyOnElmtId(rmElmtId);
        this.__indicatorIcon.purgeDependencyOnElmtId(rmElmtId);
        this.__indicatorText.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__locked.aboutToBeDeleted();
        this.__durationMs.aboutToBeDeleted();
        this.__positionMs.aboutToBeDeleted();
        this.__indicatorVisible.aboutToBeDeleted();
        this.__indicatorIcon.aboutToBeDeleted();
        this.__indicatorText.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __locked: SynchedPropertySimpleOneWayPU<boolean>;
    get locked() {
        return this.__locked.get();
    }
    set locked(newValue: boolean) {
        this.__locked.set(newValue);
    }
    private __durationMs: SynchedPropertySimpleOneWayPU<number>;
    get durationMs() {
        return this.__durationMs.get();
    }
    set durationMs(newValue: number) {
        this.__durationMs.set(newValue);
    }
    private __positionMs: SynchedPropertySimpleOneWayPU<number>;
    get positionMs() {
        return this.__positionMs.get();
    }
    set positionMs(newValue: number) {
        this.__positionMs.set(newValue);
    }
    /** Volume as 0..100; sampled once at gesture start, matching Android. */
    private getVolumePercent: () => number;
    /**
     * Current brightness as 0..100, also sampled at gesture start. Android reads
     * activity.window.attributes.screenBrightness, which is a "system default"
     * sentinel until the app sets it; HarmonyOS offers no read-back, so the page
     * owns the value it last applied.
     */
    private getBrightnessPercent: () => number;
    private onBrightness: (percent: number) => void;
    private onVolume: (percent: number) => void;
    private onSeekTo: (positionMs: number) => void;
    private onTogglePlay: () => void;
    private viewWidth: number;
    private viewHeight: number;
    private startX: number;
    private startY: number;
    private axis: GestureAxis;
    private side: GestureSide;
    private startVolumePercent: number;
    private startBrightnessPercent: number;
    private pendingSeekMs: number;
    private __indicatorVisible: ObservedPropertySimplePU<boolean>;
    get indicatorVisible() {
        return this.__indicatorVisible.get();
    }
    set indicatorVisible(newValue: boolean) {
        this.__indicatorVisible.set(newValue);
    }
    private __indicatorIcon: ObservedPropertySimplePU<string>;
    get indicatorIcon() {
        return this.__indicatorIcon.get();
    }
    set indicatorIcon(newValue: string) {
        this.__indicatorIcon.set(newValue);
    }
    private __indicatorText: ObservedPropertySimplePU<string>;
    get indicatorText() {
        return this.__indicatorText.get();
    }
    set indicatorText(newValue: string) {
        this.__indicatorText.set(newValue);
    }
    private clampPercent(value: number): number {
        if (value < 0) {
            return 0;
        }
        return value > 100 ? 100 : value;
    }
    /** Android: (int)((nowY - downY) * 100 / playerView.getHeight()). */
    private changePercent(deltaY: number): number {
        if (this.viewHeight <= 0) {
            return 0;
        }
        return Math.trunc((deltaY * 100) / this.viewHeight);
    }
    /** Android SeekDialog: the divisor shrinks as the media gets shorter. */
    private seekDivisor(): number {
        const totalMinutes: number = Math.floor(this.durationMs / 1000 / 60);
        const hours: number = Math.floor(totalMinutes / 60);
        const minutes: number = totalMinutes % 60;
        if (hours >= 1) {
            return 10;
        }
        if (minutes > 30) {
            return 5;
        }
        if (minutes > 10) {
            return 3;
        }
        if (minutes > 3) {
            return 2;
        }
        return 1;
    }
    /** Android: target = delta / divisor + currentPosition, then clamped. */
    private seekTarget(deltaX: number): number {
        let target: number = deltaX / this.seekDivisor() + this.positionMs;
        if (target < 0) {
            target = 0;
        }
        if (target > this.durationMs) {
            target = this.durationMs;
        }
        // Caller-side clamp from the Android code: never land exactly on the end.
        if (this.durationMs > 0 && target >= this.durationMs) {
            target = this.durationMs - 1000;
        }
        return target < 0 ? 0 : target;
    }
    private showIndicator(icon: string, text: string): void {
        this.indicatorIcon = icon;
        this.indicatorText = text;
        this.indicatorVisible = true;
    }
    private onActionStart(event: GestureEvent): void {
        if (this.locked) {
            return;
        }
        const fingers = event.fingerList;
        if (fingers === undefined || fingers.length === 0) {
            return;
        }
        this.startX = fingers[0].localX;
        this.startY = fingers[0].localY;
        // The side is latched from the DOWN position only.
        this.side = this.startX < this.viewWidth / 2 ? 'left' : 'right';
        // Baseline volume/brightness are sampled once, as Android does when it
        // creates the gesture dialog.
        this.startVolumePercent = this.getVolumePercent();
        this.startBrightnessPercent = this.getBrightnessPercent();
        this.axis = 'none';
    }
    private onActionUpdate(event: GestureEvent): void {
        if (this.locked) {
            return;
        }
        const deltaX: number = event.offsetX;
        const deltaY: number = event.offsetY;
        if (this.axis === 'none') {
            this.axis = Math.abs(deltaX) > Math.abs(deltaY) ? 'horizontal' : 'vertical';
        }
        if (this.axis === 'horizontal') {
            if (!ENABLE_HORIZONTAL_SEEK) {
                return;
            }
            this.pendingSeekMs = this.seekTarget(deltaX);
            this.showIndicator(deltaX >= 0 ? '⏩' : '⏪', formatMs(this.pendingSeekMs));
            return;
        }
        const percent: number = this.changePercent(deltaY);
        if (this.side === 'left') {
            const target: number = this.clampPercent(this.startBrightnessPercent - percent);
            this.showIndicator('☀', `${target}%`);
            this.onBrightness(target);
        }
        else {
            const target: number = this.clampPercent(this.startVolumePercent - percent);
            this.showIndicator(target <= 5 ? '🔇' : '🔊', `${target}%`);
            this.onVolume(target);
        }
    }
    private onActionEnd(): void {
        this.indicatorVisible = false;
        if (!this.locked && this.axis === 'horizontal' && ENABLE_HORIZONTAL_SEEK) {
            if (this.pendingSeekMs > 0) {
                this.onSeekTo(this.pendingSeekMs);
            }
        }
        this.axis = 'none';
        this.side = 'none';
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Stack.create();
            Stack.debugLine("entry/src/main/ets/view/PlayerGestureLayer.ets(194:5)", "entry");
            Stack.width('100%');
            Stack.height('100%');
        }, Stack);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            // Transparent full-area touch target.
            Column.create();
            Column.debugLine("entry/src/main/ets/view/PlayerGestureLayer.ets(196:7)", "entry");
            // Transparent full-area touch target.
            Column.width('100%');
            // Transparent full-area touch target.
            Column.height('100%');
            // Transparent full-area touch target.
            Column.backgroundColor(Color.Transparent);
            // Transparent full-area touch target.
            Column.onAreaChange((oldValue: Area, newValue: Area) => {
                this.viewWidth = Number(newValue.width);
                this.viewHeight = Number(newValue.height);
            });
            Gesture.create(GesturePriority.Low);
            GestureGroup.create(GestureMode.Exclusive);
            PanGesture.create({ fingers: 1, distance: 5 });
            PanGesture.onActionStart((event: GestureEvent) => this.onActionStart(event));
            PanGesture.onActionUpdate((event: GestureEvent) => this.onActionUpdate(event));
            PanGesture.onActionEnd(() => this.onActionEnd());
            PanGesture.onActionCancel(() => this.onActionEnd());
            PanGesture.pop();
            TapGesture.create({ count: 2 });
            TapGesture.onAction(() => {
                // Android: onDoubleTap -> switchPlayerState(); suppressed while locked.
                if (!this.locked) {
                    this.onTogglePlay();
                }
            });
            TapGesture.pop();
            TapGesture.create({ count: 1 });
            TapGesture.onAction(() => {
                // Android's onSingleTap() body is commented out: nothing happens.
            });
            TapGesture.pop();
            GestureGroup.pop();
            Gesture.pop();
        }, Column);
        // Transparent full-area touch target.
        Column.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            // Gesture indicator (Android: a 155dp centred PopupWindow).
            if (this.indicatorVisible) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Column.create();
                        Column.debugLine("entry/src/main/ets/view/PlayerGestureLayer.ets(227:9)", "entry");
                        Column.width(DIALOG_SIZE);
                        Column.height(DIALOG_SIZE);
                        Column.justifyContent(FlexAlign.Center);
                        Column.backgroundColor('#66ffffff');
                        Column.borderRadius(5);
                    }, Column);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(this.indicatorIcon);
                        Text.debugLine("entry/src/main/ets/view/PlayerGestureLayer.ets(228:11)", "entry");
                        Text.fontSize(34);
                        Text.fontColor('#333333');
                    }, Text);
                    Text.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(this.indicatorText);
                        Text.debugLine("entry/src/main/ets/view/PlayerGestureLayer.ets(229:11)", "entry");
                        Text.fontSize(24);
                        Text.fontColor('#333333');
                        Text.margin({ top: 4 });
                    }, Text);
                    Text.pop();
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
