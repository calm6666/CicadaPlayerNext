// napi_init.cpp
//
// Module registration for the HarmonyOS NAPI bridge. The implementation lives
// in napi_player.cpp (registry + control/config API) and napi_events.cpp
// (playerListener_t -> ArkTS event bridge).
//
// From ArkTS:  import cicada from 'libentry.so'

#include "napi_player.h"

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        // ---- lifecycle ----
        {"create", nullptr, cicada_ohos::Create, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"release", nullptr, cicada_ohos::Release, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setListener", nullptr, cicada_ohos::SetListener, nullptr, nullptr, nullptr, napi_default, nullptr},

        // ---- view / surface ----
        {"setSurface", nullptr, cicada_ohos::SetSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"clearScreen", nullptr, cicada_ohos::ClearScreen, nullptr, nullptr, nullptr, napi_default, nullptr},

        // ---- source ----
        {"setDataSource", nullptr, cicada_ohos::SetDataSource, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDataSourceManifest", nullptr, cicada_ohos::SetDataSourceManifest, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"addExtSubtitle", nullptr, cicada_ohos::AddExtSubtitle, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"selectExtSubtitle", nullptr, cicada_ohos::SelectExtSubtitle, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setStreamDelayTime", nullptr, cicada_ohos::SetStreamDelayTime, nullptr, nullptr, nullptr, napi_default, nullptr},

        // ---- transport ----
        {"prepare", nullptr, cicada_ohos::Prepare, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"start", nullptr, cicada_ohos::Start, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"pause", nullptr, cicada_ohos::Pause, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stop", nullptr, cicada_ohos::Stop, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"reload", nullptr, cicada_ohos::Reload, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"seek", nullptr, cicada_ohos::Seek, nullptr, nullptr, nullptr, napi_default, nullptr},

        // ---- speed / volume / modes ----
        {"setSpeed", nullptr, cicada_ohos::SetSpeed, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getSpeed", nullptr, cicada_ohos::GetSpeed, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setVolume", nullptr, cicada_ohos::SetVolume, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getVolume", nullptr, cicada_ohos::GetVolume, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setMute", nullptr, cicada_ohos::SetMute, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"isMute", nullptr, cicada_ohos::IsMute, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setLoop", nullptr, cicada_ohos::SetLoop, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getLoop", nullptr, cicada_ohos::GetLoop, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setAutoPlay", nullptr, cicada_ohos::SetAutoPlay, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"isAutoPlay", nullptr, cicada_ohos::IsAutoPlay, nullptr, nullptr, nullptr, napi_default, nullptr},

        // ---- picture modes ----
        {"setScaleMode", nullptr, cicada_ohos::SetScaleMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getScaleMode", nullptr, cicada_ohos::GetScaleMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setRotateMode", nullptr, cicada_ohos::SetRotateMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getRotateMode", nullptr, cicada_ohos::GetRotateMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setMirrorMode", nullptr, cicada_ohos::SetMirrorMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getMirrorMode", nullptr, cicada_ohos::GetMirrorMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setVideoBackgroundColor", nullptr, cicada_ohos::SetVideoBackgroundColor, nullptr, nullptr, nullptr, napi_default, nullptr},

        // ---- progress / geometry ----
        {"getDuration", nullptr, cicada_ohos::GetDuration, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getCurrentPosition", nullptr, cicada_ohos::GetCurrentPosition, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getCurrentBufferedPosition", nullptr, cicada_ohos::GetCurrentBufferedPosition, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getMasterClockPts", nullptr, cicada_ohos::GetMasterClockPts, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getVideoResolution", nullptr, cicada_ohos::GetVideoResolution, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getVideoRotation", nullptr, cicada_ohos::GetVideoRotation, nullptr, nullptr, nullptr, napi_default, nullptr},

        // ---- tracks ----
        {"switchStreamIndex", nullptr, cicada_ohos::SwitchStreamIndex, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getCurrentStreamIndex", nullptr, cicada_ohos::GetCurrentStreamIndex, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getCurrentStreamInfo", nullptr, cicada_ohos::GetCurrentStreamInfo, nullptr, nullptr, nullptr, napi_default, nullptr},

        // ---- snapshot ----
        {"captureScreen", nullptr, cicada_ohos::CaptureScreen, nullptr, nullptr, nullptr, napi_default, nullptr},

        // ---- network / config ----
        {"setTimeout", nullptr, cicada_ohos::SetTimeout, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDropBufferThreshold", nullptr, cicada_ohos::SetDropBufferThreshold, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setRefer", nullptr, cicada_ohos::SetRefer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setUserAgent", nullptr, cicada_ohos::SetUserAgent, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"addCustomHttpHeader", nullptr, cicada_ohos::AddCustomHttpHeader, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"removeAllCustomHttpHeader", nullptr, cicada_ohos::RemoveAllCustomHttpHeader, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDefaultBandWidth", nullptr, cicada_ohos::SetDefaultBandWidth, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDecoderType", nullptr, cicada_ohos::SetDecoderType, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getDecoderType", nullptr, cicada_ohos::GetDecoderType, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"enterBackGround", nullptr, cicada_ohos::EnterBackGround, nullptr, nullptr, nullptr, napi_default, nullptr},

        // ---- generic options / diagnostics ----
        {"setOption", nullptr, cicada_ohos::SetOption, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getOption", nullptr, cicada_ohos::GetOption, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getPropertyLong", nullptr, cicada_ohos::GetPropertyLong, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getPlayerName", nullptr, cicada_ohos::GetPlayerName, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"invokeComponent", nullptr, cicada_ohos::InvokeComponent, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getVideoRenderFps", nullptr, cicada_ohos::GetVideoRenderFps, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getVideoDecodeFps", nullptr, cicada_ohos::GetVideoDecodeFps, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

// C++11-compatible positional initializer (designated initializers are a C99
// feature not accepted under strict -std=c++11).
static napi_module cicadaModule = {
        1,          // nm_version
        0,          // nm_flags
        nullptr,    // nm_filename
        Init,       // nm_register_func
        "entry",    // nm_modname -> libentry.so
        nullptr,    // nm_priv
        {0},        // reserved
};

extern "C" __attribute__((constructor)) void RegisterEntryModule(void)
{
    napi_module_register(&cicadaModule);
}
