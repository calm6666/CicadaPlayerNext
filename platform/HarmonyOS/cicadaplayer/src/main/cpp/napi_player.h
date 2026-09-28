// napi_player.h
//
// Shared declarations for the CicadaPlayerNext HarmonyOS NAPI bridge.
//
// IMPORTANT: media_player_api.h is a C++ header (it uses std::string,
// std::function, reference parameters and pulls <list>/<vector> in through
// framework/base/media/IAFPacket.h). It must therefore NEVER be wrapped in
// `extern "C" { }` -- doing so makes libc++ templates be declared with C
// linkage and clang fails with "templates must have C++ linkage".

#ifndef CICADA_OHOS_NAPI_PLAYER_H
#define CICADA_OHOS_NAPI_PLAYER_H

#include <napi/native_api.h>
#include <native_window/external_window.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "media_player_api.h"

namespace cicada_ohos {
    // DRM 许可证握手对象（定义在 napi_player.cpp）：内核在播放线程上同步要
    // 许可证响应，这里用它把"原生线程 -> JS 线程 -> 原生线程"串起来。
    struct DrmHandshake;

    // ---------------------------------------------------------------------
    // One native player plus everything ArkTS needs to talk to it.
    //
    // Entries are held by shared_ptr: an event that is already in flight when
    // release() runs keeps its own reference, so the payload stays valid until
    // the JS callback has been delivered (or dropped, if `released` is set).
    // ---------------------------------------------------------------------
    struct PlayerEntry {
        int id{0};
        playerHandle *handle{nullptr};
        OHNativeWindow *window{nullptr};

        // The surface id string last handed in by ArkTS, kept for diagnostics.
        std::string surfaceId;

        // Event delivery to ArkTS. Created by setListener(), released on
        // release().
        napi_threadsafe_function tsfn{nullptr};
        napi_env env{nullptr};

        // Set once release() has started; in-flight events check this.
        std::atomic<bool> released{false};

        // Last video size reported by the decoder, so the UI can query it
        // synchronously (matches Android's getVideoWidth/Height behaviour).
        std::atomic<int> videoWidth{0};
        std::atomic<int> videoHeight{0};
        std::atomic<int> videoRotation{0};
        // DRM 请求回调：内核在播放线程上同步索要许可证，所以这里单独挂一条
        // 可阻塞的 threadsafe function（事件桥那条是 nonblocking 的，不能混用）。
        napi_threadsafe_function drmTsfn{nullptr};
        std::mutex drmMutex;
        std::vector<std::shared_ptr<DrmHandshake>> drmPending;
    };

    using PlayerPtr = std::shared_ptr<PlayerEntry>;

    // ---- registry -------------------------------------------------------
    PlayerPtr findPlayer(int id);
    int addPlayer(const PlayerPtr &entry);
    void removePlayer(int id);

    // ---- napi helpers ---------------------------------------------------
    bool argInt32(napi_env env, napi_value v, int32_t &out);
    bool argInt64(napi_env env, napi_value v, int64_t &out);
    bool argDouble(napi_env env, napi_value v, double &out);
    bool argBool(napi_env env, napi_value v, bool &out);
    std::string argString(napi_env env, napi_value v);

    napi_value makeUndefined(napi_env env);
    napi_value makeInt32(napi_env env, int32_t v);
    napi_value makeInt64(napi_env env, int64_t v);
    napi_value makeDouble(napi_env env, double v);
    napi_value makeBool(napi_env env, bool v);
    napi_value makeString(napi_env env, const std::string &s);

    // ---- event bridge (napi_events.cpp) ---------------------------------
    // Registers `callback` as the single JS listener for this player and wires
    // every field of playerListener_t to it.
    bool attachListener(napi_env env, const PlayerPtr &entry, napi_value callback);

    // Detaches the JS listener and drains the threadsafe function.
    void detachListener(const PlayerPtr &entry);

    // Serialises a StreamInfo / MediaInfo to JSON for the ArkTS panels.
    std::string streamInfoToJson(const StreamInfo &info);
    std::string mediaInfoToJson(const MediaInfo &mediaInfo);

    // ---- exported NAPI functions (napi_player.cpp / napi_events.cpp) ----
    napi_value Create(napi_env env, napi_callback_info info);
    napi_value Release(napi_env env, napi_callback_info info);
    napi_value SetListener(napi_env env, napi_callback_info info);

    napi_value SetSurface(napi_env env, napi_callback_info info);
    napi_value ClearScreen(napi_env env, napi_callback_info info);

    napi_value SetDataSource(napi_env env, napi_callback_info info);
    napi_value SetDataSourceManifest(napi_env env, napi_callback_info info);
    napi_value AddExtSubtitle(napi_env env, napi_callback_info info);
    napi_value SelectExtSubtitle(napi_env env, napi_callback_info info);
    napi_value SetStreamDelayTime(napi_env env, napi_callback_info info);

    napi_value Prepare(napi_env env, napi_callback_info info);
    napi_value Start(napi_env env, napi_callback_info info);
    napi_value Pause(napi_env env, napi_callback_info info);
    napi_value Stop(napi_env env, napi_callback_info info);
    napi_value Reload(napi_env env, napi_callback_info info);
    napi_value Seek(napi_env env, napi_callback_info info);

    napi_value SetSpeed(napi_env env, napi_callback_info info);
    napi_value GetSpeed(napi_env env, napi_callback_info info);
    napi_value SetVolume(napi_env env, napi_callback_info info);
    napi_value GetVolume(napi_env env, napi_callback_info info);
    napi_value SetMute(napi_env env, napi_callback_info info);
    napi_value IsMute(napi_env env, napi_callback_info info);
    napi_value SetLoop(napi_env env, napi_callback_info info);
    napi_value GetLoop(napi_env env, napi_callback_info info);
    napi_value SetAutoPlay(napi_env env, napi_callback_info info);
    napi_value IsAutoPlay(napi_env env, napi_callback_info info);

    napi_value SetScaleMode(napi_env env, napi_callback_info info);
    napi_value GetScaleMode(napi_env env, napi_callback_info info);
    napi_value SetRotateMode(napi_env env, napi_callback_info info);
    napi_value GetRotateMode(napi_env env, napi_callback_info info);
    napi_value SetMirrorMode(napi_env env, napi_callback_info info);
    napi_value GetMirrorMode(napi_env env, napi_callback_info info);
    napi_value SetVideoBackgroundColor(napi_env env, napi_callback_info info);

    napi_value GetDuration(napi_env env, napi_callback_info info);
    napi_value GetCurrentPosition(napi_env env, napi_callback_info info);
    napi_value GetCurrentBufferedPosition(napi_env env, napi_callback_info info);
    napi_value GetMasterClockPts(napi_env env, napi_callback_info info);
    napi_value GetVideoResolution(napi_env env, napi_callback_info info);
    napi_value GetVideoRotation(napi_env env, napi_callback_info info);

    napi_value SwitchStreamIndex(napi_env env, napi_callback_info info);
    napi_value GetCurrentStreamIndex(napi_env env, napi_callback_info info);
    napi_value GetCurrentStreamInfo(napi_env env, napi_callback_info info);

    napi_value CaptureScreen(napi_env env, napi_callback_info info);

    napi_value SetTimeout(napi_env env, napi_callback_info info);
    napi_value SetDropBufferThreshold(napi_env env, napi_callback_info info);
    napi_value SetRefer(napi_env env, napi_callback_info info);
    napi_value SetUserAgent(napi_env env, napi_callback_info info);
    napi_value AddCustomHttpHeader(napi_env env, napi_callback_info info);
    napi_value RemoveAllCustomHttpHeader(napi_env env, napi_callback_info info);
    napi_value SetDefaultBandWidth(napi_env env, napi_callback_info info);
    napi_value SetDecoderType(napi_env env, napi_callback_info info);
    napi_value GetDecoderType(napi_env env, napi_callback_info info);
    napi_value EnterBackGround(napi_env env, napi_callback_info info);

    napi_value SetOption(napi_env env, napi_callback_info info);
    napi_value GetOption(napi_env env, napi_callback_info info);
    napi_value GetPropertyLong(napi_env env, napi_callback_info info);
    napi_value GetPlayerName(napi_env env, napi_callback_info info);
    napi_value InvokeComponent(napi_env env, napi_callback_info info);

    napi_value GetVideoRenderFps(napi_env env, napi_callback_info info);
    napi_value GetVideoDecodeFps(napi_env env, napi_callback_info info);

    // ---- 解码事实 / 切档状态 ----
    napi_value IsVideoDecoderHardware(napi_env env, napi_callback_info info);
    napi_value IsStreamSwitchInFlight(napi_env env, napi_callback_info info);

    // ---- 画面滤镜 ----
    napi_value SetColorMatrix(napi_env env, napi_callback_info info);
    napi_value SetFilterConfig(napi_env env, napi_callback_info info);
    napi_value UpdateFilterConfig(napi_env env, napi_callback_info info);
    napi_value SetFilterInvalid(napi_env env, napi_callback_info info);

    // ---- 硬解能力 / 编码偏好 ----
    napi_value GetVideoCodecSupport(napi_env env, napi_callback_info info);
    napi_value SetVideoCodecSupport(napi_env env, napi_callback_info info);

    // ---- 属性字符串 ----
    napi_value GetPropertyString(napi_env env, napi_callback_info info);

    // ---- DRM ----
    napi_value SetDrmRequestCallback(napi_env env, napi_callback_info info);
} // namespace cicada_ohos

#endif // CICADA_OHOS_NAPI_PLAYER_H
