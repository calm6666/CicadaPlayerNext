// Type declarations for the CicadaPlayerNext NAPI module (libentry.so).
// Kept in sync with platform/HarmonyOS/entry/src/main/cpp/napi_init.cpp.
//
// Usage from ArkTS:  import cicada from 'libentry.so';

declare namespace cicadaNapi {
    /**
     * Player event callback.
     *
     * - normal events: (event, a, b, text)  -- text carries a message or JSON
     * - 'snapshot'   : ('snapshot', width, height, rgba ArrayBuffer)
     */
    type PlayerEventCallback =
        (event: string, a: number, b: number, payload: string | ArrayBuffer) => void;

    // ---- lifecycle ----
    const create: (opts?: string) => number;
    const release: (id: number) => void;
    const setListener: (id: number, callback: PlayerEventCallback) => boolean;

    // ---- view / surface ----
    const setSurface: (id: number, surfaceId: string) => boolean;
    const clearScreen: (id: number) => void;

    // ---- source ----
    const setDataSource: (id: number, url: string) => void;
    const setDataSourceManifest: (id: number, manifestJson: string) => void;
    const addExtSubtitle: (id: number, uri: string) => void;
    const selectExtSubtitle: (id: number, index: number, select: boolean) => void;
    const setStreamDelayTime: (id: number, index: number, timeMs: number) => number;

    // ---- transport ----
    const prepare: (id: number) => void;
    const start: (id: number) => void;
    const pause: (id: number) => void;
    const stop: (id: number) => number;
    const reload: (id: number) => void;
    const seek: (id: number, positionMs: number, accurate: boolean) => void;

    // ---- speed / volume / modes ----
    const setSpeed: (id: number, speed: number) => void;
    const getSpeed: (id: number) => number;
    const setVolume: (id: number, volume: number) => void;
    const getVolume: (id: number) => number;
    const setMute: (id: number, mute: boolean) => void;
    const isMute: (id: number) => boolean;
    const setLoop: (id: number, loop: boolean) => void;
    const getLoop: (id: number) => boolean;
    const setAutoPlay: (id: number, autoPlay: boolean) => void;
    const isAutoPlay: (id: number) => boolean;

    // ---- picture modes ----
    const setScaleMode: (id: number, mode: number) => void;
    const getScaleMode: (id: number) => number;
    const setRotateMode: (id: number, mode: number) => void;
    const getRotateMode: (id: number) => number;
    const setMirrorMode: (id: number, mode: number) => void;
    const getMirrorMode: (id: number) => number;
    const setVideoBackgroundColor: (id: number, color: number) => void;

    // ---- progress / geometry ----
    const getDuration: (id: number) => number;
    const getCurrentPosition: (id: number) => number;
    const getCurrentBufferedPosition: (id: number) => number;
    const getMasterClockPts: (id: number) => number;
    const getVideoResolution: (id: number) => string;
    const getVideoRotation: (id: number) => number;

    // ---- tracks ----
    const switchStreamIndex: (id: number, index: number) => number;
    const getCurrentStreamIndex: (id: number, streamType: number) => number;
    const getCurrentStreamInfo: (id: number, streamType: number) => string;

    // ---- snapshot ----
    const captureScreen: (id: number) => void;

    // ---- network / config ----
    const setTimeout: (id: number, timeoutMs: number) => void;
    const setDropBufferThreshold: (id: number, dropValueMs: number) => void;
    const setRefer: (id: number, referer: string) => void;
    const setUserAgent: (id: number, userAgent: string) => void;
    const addCustomHttpHeader: (id: number, header: string) => void;
    const removeAllCustomHttpHeader: (id: number) => void;
    const setDefaultBandWidth: (id: number, bandwidth: number) => void;
    const setDecoderType: (id: number, type: number) => void;
    const getDecoderType: (id: number) => number;
    const enterBackGround: (id: number, back: boolean) => void;

    // ---- generic options / diagnostics ----
    const setOption: (id: number, key: string, value: string) => number;
    const getOption: (id: number, key: string) => string;
    const getPropertyLong: (id: number, key: number) => number;
    const getPlayerName: (id: number) => string;
    const invokeComponent: (id: number, content: string) => number;
    const getVideoRenderFps: (id: number) => number;
    const getVideoDecodeFps: (id: number) => number;
}

export default cicadaNapi;
