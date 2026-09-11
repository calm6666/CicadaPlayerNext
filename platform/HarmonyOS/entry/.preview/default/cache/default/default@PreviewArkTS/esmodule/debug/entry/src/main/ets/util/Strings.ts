/*
 * Demo UI strings.
 *
 * These are copied verbatim from the Android demo's
 * res/values/strings.xml (English) and res/values-zh-rCN/strings.xml (Chinese)
 * so the HarmonyOS demo reads the same. They live in code for now; move them to
 * resources/base/element/string.json + resources/zh_CN/... when localisation
 * matters.
 *
 * `BUILT_IN_BILINGUAL` picks the Chinese set, matching the demo's primary
 * audience; set it to false for the English labels.
 */
const BUILT_IN_BILINGUAL: boolean = true;
export class S {
    // ---- shared title bar ----
    static get titleChooseItem(): string {
        return BUILT_IN_BILINGUAL ? '选择URL界面' : 'URL';
    }
    static get titleInputUrlItem(): string {
        return BUILT_IN_BILINGUAL ? '输入URL界面' : 'Input URL';
    }
    static get titleScanning(): string {
        return BUILT_IN_BILINGUAL ? '二维码扫描' : 'QR code Scanning';
    }
    static get titleSetting(): string {
        return BUILT_IN_BILINGUAL ? '设置' : 'Setting';
    }
    static get titlePlayer(): string {
        return BUILT_IN_BILINGUAL ? '播放' : 'Play';
    }
    static get titleScreenShot(): string {
        return BUILT_IN_BILINGUAL ? '截图' : 'SnapShot';
    }
    static get titleMultipleInstances(): string {
        return BUILT_IN_BILINGUAL ? '多实例' : 'Multiple instances';
    }
    // ---- source choose ----
    static get btnInputUrl(): string {
        return BUILT_IN_BILINGUAL ? '输入URL' : 'Input Url';
    }
    static get btnLocalVideo(): string {
        // The Android demo has no zh translation for this one.
        return 'Select Local Video';
    }
    static get pickVideoFailed(): string {
        return 'No file picker available on this device';
    }
    static get importVideoIng(): string {
        return 'Importing local video…';
    }
    static get importVideoFailed(): string {
        return 'Import local video failed';
    }
    /** Shown when a picked file cannot be opened without copying it. */
    static get openVideoFailed(): string {
        return BUILT_IN_BILINGUAL ? '无法打开所选文件' : 'Cannot open the selected file';
    }
    static get success(): string {
        return BUILT_IN_BILINGUAL ? '成功' : 'Success';
    }
    /** Android: the literal "Scan failed!" toast in CaptureActivity. */
    static get scanFailed(): string {
        return 'Scan failed!';
    }
    /** strings.xml no_camera_permission. */
    static get noCameraPermission(): string {
        return BUILT_IN_BILINGUAL
            ? '没有权限使用摄像头，请开启摄像头权限'
            : 'Not Camera Permission';
    }
    // ---- transport ----
    static get prepare(): string {
        return BUILT_IN_BILINGUAL ? '准备' : 'Prepare';
    }
    static get play(): string {
        return BUILT_IN_BILINGUAL ? '播放' : 'Play';
    }
    static get pause(): string {
        return BUILT_IN_BILINGUAL ? '暂停' : 'Pause';
    }
    static get stop(): string {
        return BUILT_IN_BILINGUAL ? '停止' : 'Stop';
    }
    static get snapshot(): string {
        return BUILT_IN_BILINGUAL ? '截图' : 'ScreenShot';
    }
    static get retry(): string {
        return BUILT_IN_BILINGUAL ? '重试' : 'Retry';
    }
    static get bufferLabel(): string {
        return 'buffer: ';
    }
    static get screenModelSmall(): string {
        return BUILT_IN_BILINGUAL ? '全屏' : 'Full Screen';
    }
    static get screenModelLarge(): string {
        return BUILT_IN_BILINGUAL ? '缩放' : 'Scale Screen';
    }
    // ---- tips ----
    static get loading(): string {
        return 'Loading...';
    }
    static get replayMsg(): string {
        return BUILT_IN_BILINGUAL ? '视频播放结束' : 'Completion';
    }
    static get replay(): string {
        return BUILT_IN_BILINGUAL ? '重播' : 'Replay';
    }
    static get errorCode(): string {
        return BUILT_IN_BILINGUAL ? '错误码:' : 'Error Code:';
    }
    static get netStateMobile(): string {
        return BUILT_IN_BILINGUAL ? '当前为移动网络，请点击播放' : 'You are connected to a mobile network. Continue?';
    }
    static get netStateMobileYes(): string {
        return BUILT_IN_BILINGUAL ? '继续播放' : 'Continue';
    }
    static get netStateMobileNo(): string {
        return BUILT_IN_BILINGUAL ? '退出播放' : 'Exit';
    }
    static get netDisable(): string {
        return BUILT_IN_BILINGUAL ? '无网络连接，检查网络后点击重新播放' : 'NetWork Error';
    }
    static get netReconnectFailed(): string {
        return BUILT_IN_BILINGUAL ? '网络重连失败' : 'NetWork Reconnect Failed';
    }
    static get netReconnectSuccess(): string {
        return BUILT_IN_BILINGUAL ? '网络连接成功' : 'NetWork Reconnect Success';
    }
    // ---- toasts ----
    static get prepareSuccess(): string {
        return BUILT_IN_BILINGUAL ? '准备完成' : 'Prepare Success';
    }
    static get snapShotFailure(): string {
        return BUILT_IN_BILINGUAL ? '获取截图失败' : 'SnapShot Failure';
    }
    static get snapShotting(): string {
        return BUILT_IN_BILINGUAL ? '正在截图中,请稍后' : 'Waitting...';
    }
    static get changeTrackFailure(): string {
        return BUILT_IN_BILINGUAL ? '切换失败' : 'change failure';
    }
    static get changeSubtitleTrack(): string {
        return BUILT_IN_BILINGUAL ? '切换字幕' : 'change subtitle';
    }
    static get changeAudioTrack(): string {
        return BUILT_IN_BILINGUAL ? '切换音轨' : 'change audio';
    }
    static get changeBitrateTrack(): string {
        return BUILT_IN_BILINGUAL ? '切换码率' : 'change bitrate';
    }
    static get changeToSoftDecoder(): string {
        return BUILT_IN_BILINGUAL ? '切换到软解' : 'change to soft decoder';
    }
    static get refreshConfigSuccess(): string {
        return BUILT_IN_BILINGUAL ? '刷新配置成功' : 'Set Config Success';
    }
    static get noneMediaInfo(): string {
        return BUILT_IN_BILINGUAL ? '暂无媒体信息' : 'None';
    }
    static get stsInputUrlFirst(): string {
        return '请先输入播放链接';
    }
    static get stsManifestFirst(): string {
        return '请先粘贴接口返回的 MediaManifest JSON';
    }
    static get hardwareOnNextPlay(): string {
        return '已开启硬解，重新播放后生效';
    }
    static get hardwareOffNextPlay(): string {
        return '已关闭硬解，重新播放后生效';
    }
    // ---- speed (cicada_speed_*) ----
    static get speedMode(): string {
        return BUILT_IN_BILINGUAL ? '倍速播放' : 'Speed Mode';
    }
    static get speedOneTimes(): string {
        return BUILT_IN_BILINGUAL ? '正常' : 'Normal';
    }
    static get speedOptfTimes(): string {
        return BUILT_IN_BILINGUAL ? '0.5倍速' : '0.5';
    }
    static get speedOptTimes(): string {
        return BUILT_IN_BILINGUAL ? '1.5倍速' : '1.5';
    }
    static get speedTwiceTimes(): string {
        return BUILT_IN_BILINGUAL ? '2.0倍速' : '2.0';
    }
    // ---- track panel (media_info_track_* / auto_bitrate / cancel ext) ----
    static get trackDefinition(): string {
        return BUILT_IN_BILINGUAL ? '清晰度' : 'Definition';
    }
    static get trackBitrate(): string {
        return BUILT_IN_BILINGUAL ? '码率' : 'Bitrate';
    }
    static get trackAudio(): string {
        return BUILT_IN_BILINGUAL ? '音轨' : 'Audio';
    }
    static get trackSubtitle(): string {
        return BUILT_IN_BILINGUAL ? '字幕' : 'Subtitle';
    }
    static get trackSubtitleExt(): string {
        return BUILT_IN_BILINGUAL ? '外挂字幕' : 'External Subtitle';
    }
    static get autoBitrate(): string {
        return BUILT_IN_BILINGUAL ? '自动码率' : 'Auto Bitrate';
    }
    static get cancelSubtitleExt(): string {
        return BUILT_IN_BILINGUAL ? '取消外挂字幕' : 'Cancel Subtitle';
    }
}
/** URL-input defaults, hardcoded in the Android layout. */
export class UrlDefaults {
    static readonly INPUT_TEXT: string = 'https://alivc-demo-vod.aliyuncs.com/b4da45beb07b4d5b81b54b1ac50fb502/88839cae0df0489584ca3d8217ef8048.m3u8';
    static readonly INPUT_HINT: string = 'http://player.alicdn.com/video/aliyunmedia.mp4';
    static readonly DASH_HINT: string = 'DASH 播放链接（.mpd）';
    static readonly HLS_HINT: string = 'HLS 播放链接（.m3u8）';
    static readonly NORMAL_HINT: string = '普通在线视频链接（mp4/flv 等，非 dash/hls）';
    static readonly MANIFEST_HINT: string = '接口返回的 MediaManifest JSON（DASH/HLS 对象播放，反序列化后直接传入播放器）';
}
