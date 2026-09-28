// napi_player.cpp
//
// Player registry, argument marshalling and the full NAPI surface over the
// Cicada C API (media_player_api.h). The event bridge lives in napi_events.cpp
// and the module registration in napi_init.cpp.

#include "napi_player.h"

#include <cstdio>
#include <cstring>
#include <condition_variable>

// base64 解码：DRM 请求/响应在 JS 侧走 base64 文本（ArrayBuffer 亦可）。
#include <utils/CicadaUtils.h>

// CicadaGetVideoRenderFps / CicadaGetVideoDecodeFps are defined in
// media_player_api.cpp but are missing from media_player_api.h, so they are
// declared here rather than patching the shared header. They MUST be declared
// at global scope: a declaration inside cicada_ohos (or an anonymous namespace)
// would name a different function and fail to link.
float CicadaGetVideoRenderFps(playerHandle *pHandle);
float CicadaGetVideoDecodeFps(playerHandle *pHandle);

namespace cicada_ohos {

    // ---------------------------------------------------------------------
    // Registry
    // ---------------------------------------------------------------------
    namespace {
        std::mutex gMutex;
        std::map<int, PlayerPtr> gPlayers;
        int gNextId = 1;
    } // namespace

    PlayerPtr findPlayer(int id)
    {
        std::lock_guard<std::mutex> lock(gMutex);
        auto it = gPlayers.find(id);
        return it == gPlayers.end() ? nullptr : it->second;
    }

    int addPlayer(const PlayerPtr &entry)
    {
        std::lock_guard<std::mutex> lock(gMutex);
        entry->id = gNextId++;
        gPlayers[entry->id] = entry;
        return entry->id;
    }

    void removePlayer(int id)
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gPlayers.erase(id);
    }

    // ---------------------------------------------------------------------
    // napi helpers
    // ---------------------------------------------------------------------
    bool argInt32(napi_env env, napi_value v, int32_t &out)
    {
        napi_valuetype type = napi_undefined;
        if (napi_typeof(env, v, &type) != napi_ok) {
            return false;
        }
        if (type == napi_number) {
            return napi_get_value_int32(env, v, &out) == napi_ok;
        }
        // Be permissive: ArkTS numbers are doubles and callers occasionally
        // pass a numeric string.
        double d = 0;
        if (napi_get_value_double(env, v, &d) == napi_ok) {
            out = static_cast<int32_t>(d);
            return true;
        }
        return false;
    }

    bool argInt64(napi_env env, napi_value v, int64_t &out)
    {
        napi_valuetype type = napi_undefined;
        if (napi_typeof(env, v, &type) != napi_ok) {
            return false;
        }
        if (type == napi_number) {
            return napi_get_value_int64(env, v, &out) == napi_ok;
        }
        double d = 0;
        if (napi_get_value_double(env, v, &d) == napi_ok) {
            out = static_cast<int64_t>(d);
            return true;
        }
        return false;
    }

    bool argDouble(napi_env env, napi_value v, double &out)
    {
        return napi_get_value_double(env, v, &out) == napi_ok;
    }

    bool argBool(napi_env env, napi_value v, bool &out)
    {
        return napi_get_value_bool(env, v, &out) == napi_ok;
    }

    std::string argString(napi_env env, napi_value v)
    {
        size_t len = 0;
        if (napi_get_value_string_utf8(env, v, nullptr, 0, &len) != napi_ok) {
            return std::string();
        }
        std::string out(len, '\0');
        size_t written = 0;
        napi_get_value_string_utf8(env, v, &out[0], len + 1, &written);
        out.resize(written);
        return out;
    }

    napi_value makeUndefined(napi_env env)
    {
        napi_value v = nullptr;
        napi_get_undefined(env, &v);
        return v;
    }

    napi_value makeInt32(napi_env env, int32_t value)
    {
        napi_value v = nullptr;
        napi_create_int32(env, value, &v);
        return v;
    }

    napi_value makeInt64(napi_env env, int64_t value)
    {
        napi_value v = nullptr;
        napi_create_int64(env, value, &v);
        return v;
    }

    napi_value makeDouble(napi_env env, double value)
    {
        napi_value v = nullptr;
        napi_create_double(env, value, &v);
        return v;
    }

    napi_value makeBool(napi_env env, bool value)
    {
        napi_value v = nullptr;
        napi_get_boolean(env, value, &v);
        return v;
    }

    napi_value makeString(napi_env env, const std::string &s)
    {
        napi_value v = nullptr;
        napi_create_string_utf8(env, s.c_str(), s.size(), &v);
        return v;
    }

    // ---------------------------------------------------------------------
    // Argument unpacking shared by every player function
    // ---------------------------------------------------------------------
    namespace {
        const size_t kMaxArgs = 6;

        struct CallArgs {
            size_t argc{0};
            napi_value argv[kMaxArgs]{};
            int32_t id{0};
            PlayerPtr player;
        };

        // Returns false when the first argument is not a live player id; the
        // exported function then resolves to undefined.
        bool unpack(napi_env env, napi_callback_info info, CallArgs &out)
        {
            out.argc = kMaxArgs;
            if (napi_get_cb_info(env, info, &out.argc, out.argv, nullptr, nullptr) != napi_ok) {
                return false;
            }
            if (out.argc < 1 || !argInt32(env, out.argv[0], out.id)) {
                return false;
            }
            out.player = findPlayer(out.id);
            return out.player != nullptr && !out.player->released.load();
        }

        std::string jsonEscape(const char *s)
        {
            std::string out;
            if (s == nullptr) {
                return out;
            }
            for (const char *p = s; *p != '\0'; ++p) {
                const unsigned char c = static_cast<unsigned char>(*p);
                switch (c) {
                    case '"':
                        out += "\\\"";
                        break;
                    case '\\':
                        out += "\\\\";
                        break;
                    case '\n':
                        out += "\\n";
                        break;
                    case '\r':
                        out += "\\r";
                        break;
                    case '\t':
                        out += "\\t";
                        break;
                    default:
                        if (c < 0x20) {
                            char buf[8];
                            snprintf(buf, sizeof(buf), "\\u%04x", c);
                            out += buf;
                        } else {
                            out += static_cast<char>(c);
                        }
                        break;
                }
            }
            return out;
        }

        const char *streamTypeName(StreamType type)
        {
            switch (type) {
                case ST_TYPE_VIDEO:
                    return "video";
                case ST_TYPE_AUDIO:
                    return "audio";
                case ST_TYPE_SUB:
                    return "subtitle";
                default:
                    return "unknown";
            }
        }
    } // namespace

    std::string streamInfoToJson(const StreamInfo &info)
    {
        std::string out = "{";
        out += "\"streamIndex\":" + std::to_string(info.streamIndex);
        out += ",\"type\":\"" + std::string(streamTypeName(info.type)) + "\"";
        out += ",\"description\":\"" + jsonEscape(info.description) + "\"";
        out += ",\"bitrate\":" + std::to_string(info.bitrate);
        out += ",\"videoBandwidth\":" + std::to_string(info.videoBandwidth);
        out += ",\"videoWidth\":" + std::to_string(info.videoWidth);
        out += ",\"videoHeight\":" + std::to_string(info.videoHeight);
        out += ",\"hdrType\":" + std::to_string(static_cast<int>(info.HDRType));
        out += ",\"audioLang\":\"" + jsonEscape(info.audioLang) + "\"";
        out += ",\"nChannels\":" + std::to_string(info.nChannels);
        out += ",\"sampleRate\":" + std::to_string(info.sampleRate);
        out += ",\"sampleFormat\":" + std::to_string(info.sampleFormat);
        out += ",\"subtitleLang\":\"" + jsonEscape(info.subtitleLang) + "\"";
        out += "}";
        return out;
    }

    std::string mediaInfoToJson(const MediaInfo &mediaInfo)
    {
        std::string out = "{\"totalBitrate\":" + std::to_string(mediaInfo.totalBitrate);
        out += ",\"streams\":[";
        bool first = true;
        for (const StreamInfo *info : mediaInfo.mStreamInfoQueue) {
            if (info == nullptr) {
                continue;
            }
            if (!first) {
                out += ",";
            }
            first = false;
            out += streamInfoToJson(*info);
        }
        out += "]}";
        return out;
    }

    // ---------------------------------------------------------------------
    // Lifecycle
    // ---------------------------------------------------------------------
    napi_value Create(napi_env env, napi_callback_info info)
    {
        size_t argc = 1;
        napi_value argv[1] = {nullptr};
        napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

        std::string opts;
        if (argc >= 1) {
            opts = argString(env, argv[0]);
        }

        PlayerPtr entry(new PlayerEntry());
        entry->handle = CicadaCreatePlayer(opts.empty() ? nullptr : opts.c_str());
        if (entry->handle == nullptr) {
            return makeInt32(env, -1);
        }
        return makeInt32(env, addPlayer(entry));
    }

    napi_value Release(napi_env env, napi_callback_info info)
    {
        size_t argc = 1;
        napi_value argv[1] = {nullptr};
        napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

        int32_t id = 0;
        if (argc < 1 || !argInt32(env, argv[0], id)) {
            return makeUndefined(env);
        }

        // Take a local reference first so the entry outlives the teardown even
        // if another thread is mid-callback.
        PlayerPtr entry = findPlayer(id);
        if (entry == nullptr) {
            return makeUndefined(env);
        }

        entry->released.store(true);

        // Tear the native player down FIRST. CicadaReleasePlayer stops and joins
        // the notifier thread, so after it returns no native callback can still be
        // holding the threadsafe function. (The reverse order used to make
        // detachListener call CicadaSetListener on a running player, which
        // deadlocks the calling thread -- see detachListener's comment.)
        if (entry->window != nullptr) {
            OH_NativeWindow_DestroyNativeWindow(entry->window);
            entry->window = nullptr;
        }
        if (entry->handle != nullptr) {
            CicadaReleasePlayer(&entry->handle);
            entry->handle = nullptr;
        }

        // Now safe to close the JS side; pending payloads are still delivered and
        // dropped by the `released` flag above.
        detachListener(entry);

        removePlayer(id);
        return makeUndefined(env);
    }

    // ---------------------------------------------------------------------
    // View / surface
    // ---------------------------------------------------------------------
    napi_value SetSurface(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a) || a.argc < 2) {
            return makeUndefined(env);
        }
        const std::string surfaceId = argString(env, a.argv[1]);

        uint64_t id64 = strtoull(surfaceId.c_str(), nullptr, 10);
        // This SDK's form takes an out-parameter and returns a status code.
        OHNativeWindow *window = nullptr;
        if (OH_NativeWindow_CreateNativeWindowFromSurfaceId(id64, &window) != 0 || window == nullptr) {
            return makeBool(env, false);
        }

        if (a.player->window != nullptr) {
            OH_NativeWindow_DestroyNativeWindow(a.player->window);
        }
        a.player->window = window;
        a.player->surfaceId = surfaceId;
        // The OHOS decoder runs in surface mode and receives the window through
        // SetView (zero-copy straight to the XComponent).
        CicadaSetView(a.player->handle, window);
        return makeBool(env, true);
    }

    napi_value ClearScreen(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeUndefined(env);
        }
        CicadaClearScreen(a.player->handle);
        return makeUndefined(env);
    }

    // ---------------------------------------------------------------------
    // Source
    // ---------------------------------------------------------------------
    napi_value SetDataSource(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a) || a.argc < 2) {
            return makeUndefined(env);
        }
        const std::string url = argString(env, a.argv[1]);
        CicadaSetDataSourceWithUrl(a.player->handle, url.c_str());
        return makeUndefined(env);
    }

    napi_value SetDataSourceManifest(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a) || a.argc < 2) {
            return makeUndefined(env);
        }
        const std::string json = argString(env, a.argv[1]);
        // Object-based playback: unified MediaManifest JSON, DRM capable.
        CicadaSetDataSourceWithManifest(a.player->handle, json.c_str());
        return makeUndefined(env);
    }

    napi_value AddExtSubtitle(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a) || a.argc < 2) {
            return makeUndefined(env);
        }
        const std::string uri = argString(env, a.argv[1]);
        CicadaAddExtSubtitle(a.player->handle, uri.c_str());
        return makeUndefined(env);
    }

    napi_value SelectExtSubtitle(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        int32_t index = 0;
        bool select = false;
        if (!unpack(env, info, a) || a.argc < 3 || !argInt32(env, a.argv[1], index) ||
            !argBool(env, a.argv[2], select)) {
            return makeUndefined(env);
        }
        CicadaSelectExtSubtitle(a.player->handle, index, select);
        return makeUndefined(env);
    }

    napi_value SetStreamDelayTime(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        int32_t index = 0;
        int64_t time = 0;
        if (!unpack(env, info, a) || a.argc < 3 || !argInt32(env, a.argv[1], index) ||
            !argInt64(env, a.argv[2], time)) {
            return makeUndefined(env);
        }
        return makeInt32(env, CicadaSetStreamDelayTime(a.player->handle, index, time));
    }

    // ---------------------------------------------------------------------
    // Transport
    // ---------------------------------------------------------------------
    napi_value Prepare(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeUndefined(env);
        }
        CicadaPreparePlayer(a.player->handle);
        return makeUndefined(env);
    }

    napi_value Start(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeUndefined(env);
        }
        CicadaStartPlayer(a.player->handle);
        return makeUndefined(env);
    }

    napi_value Pause(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeUndefined(env);
        }
        CicadaPausePlayer(a.player->handle);
        return makeUndefined(env);
    }

    napi_value Stop(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeUndefined(env);
        }
        return makeInt32(env, CicadaStopPlayer(a.player->handle));
    }

    napi_value Reload(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeUndefined(env);
        }
        CicadaReload(a.player->handle);
        return makeUndefined(env);
    }

    napi_value Seek(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        int64_t positionMs = 0;
        bool accurate = false;
        if (!unpack(env, info, a) || a.argc < 2 || !argInt64(env, a.argv[1], positionMs)) {
            return makeUndefined(env);
        }
        if (a.argc >= 3) {
            argBool(env, a.argv[2], accurate);
        }
        CicadaSeekToTime(a.player->handle, positionMs, accurate);
        return makeUndefined(env);
    }

    // ---------------------------------------------------------------------
    // Speed / volume / mute / loop / autoplay
    // ---------------------------------------------------------------------
    napi_value SetSpeed(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        double speed = 1.0;
        if (!unpack(env, info, a) || a.argc < 2 || !argDouble(env, a.argv[1], speed)) {
            return makeUndefined(env);
        }
        CicadaPlayerSetSpeed(a.player->handle, static_cast<float>(speed));
        return makeUndefined(env);
    }

    napi_value GetSpeed(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeDouble(env, 1.0);
        }
        return makeDouble(env, CicadaPlayerGetSpeed(a.player->handle));
    }

    napi_value SetVolume(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        double volume = 1.0;
        if (!unpack(env, info, a) || a.argc < 2 || !argDouble(env, a.argv[1], volume)) {
            return makeUndefined(env);
        }
        CicadaSetVolume(a.player->handle, static_cast<float>(volume));
        return makeUndefined(env);
    }

    napi_value GetVolume(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeDouble(env, 0);
        }
        return makeDouble(env, CicadaGetVolume(a.player->handle));
    }

    napi_value SetMute(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        bool mute = false;
        if (!unpack(env, info, a) || a.argc < 2 || !argBool(env, a.argv[1], mute)) {
            return makeUndefined(env);
        }
        CicadaSetMute(a.player->handle, mute);
        return makeUndefined(env);
    }

    napi_value IsMute(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeBool(env, false);
        }
        return makeBool(env, CicadaIsMute(a.player->handle));
    }

    napi_value SetLoop(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        bool loop = false;
        if (!unpack(env, info, a) || a.argc < 2 || !argBool(env, a.argv[1], loop)) {
            return makeUndefined(env);
        }
        CicadaSetLoop(a.player->handle, loop);
        return makeUndefined(env);
    }

    napi_value GetLoop(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeBool(env, false);
        }
        return makeBool(env, CicadaGetLoop(a.player->handle));
    }

    napi_value SetAutoPlay(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        bool autoPlay = false;
        if (!unpack(env, info, a) || a.argc < 2 || !argBool(env, a.argv[1], autoPlay)) {
            return makeUndefined(env);
        }
        CicadaSetAutoPlay(a.player->handle, autoPlay);
        return makeUndefined(env);
    }

    napi_value IsAutoPlay(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeBool(env, false);
        }
        return makeBool(env, CicadaIsAutoPLay(a.player->handle));
    }

    // ---------------------------------------------------------------------
    // Picture: scale / rotate / mirror / background
    // ---------------------------------------------------------------------
    napi_value SetScaleMode(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        int32_t mode = 0;
        if (!unpack(env, info, a) || a.argc < 2 || !argInt32(env, a.argv[1], mode)) {
            return makeUndefined(env);
        }
        CicadaSetScaleMode(a.player->handle, static_cast<ScaleMode>(mode));
        return makeUndefined(env);
    }

    napi_value GetScaleMode(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeInt32(env, 0);
        }
        return makeInt32(env, static_cast<int32_t>(CicadaGetScaleMode(a.player->handle)));
    }

    napi_value SetRotateMode(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        int32_t mode = 0;
        if (!unpack(env, info, a) || a.argc < 2 || !argInt32(env, a.argv[1], mode)) {
            return makeUndefined(env);
        }
        CicadaSetRotateMode(a.player->handle, static_cast<RotateMode>(mode));
        return makeUndefined(env);
    }

    napi_value GetRotateMode(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeInt32(env, 0);
        }
        return makeInt32(env, static_cast<int32_t>(CicadaGetRotateMode(a.player->handle)));
    }

    napi_value SetMirrorMode(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        int32_t mode = 0;
        if (!unpack(env, info, a) || a.argc < 2 || !argInt32(env, a.argv[1], mode)) {
            return makeUndefined(env);
        }
        CicadaSetMirrorMode(a.player->handle, static_cast<MirrorMode>(mode));
        return makeUndefined(env);
    }

    napi_value GetMirrorMode(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeInt32(env, 0);
        }
        return makeInt32(env, static_cast<int32_t>(CicadaGetMirrorMode(a.player->handle)));
    }

    napi_value SetVideoBackgroundColor(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        double color = 0;
        if (!unpack(env, info, a) || a.argc < 2 || !argDouble(env, a.argv[1], color)) {
            return makeUndefined(env);
        }
        CicadaSetVideoBackgroundColor(a.player->handle, static_cast<uint32_t>(color));
        return makeUndefined(env);
    }

    // ---------------------------------------------------------------------
    // Progress / geometry
    // ---------------------------------------------------------------------
    napi_value GetDuration(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeInt64(env, -1);
        }
        return makeInt64(env, CicadaGetDuration(a.player->handle));
    }

    napi_value GetCurrentPosition(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeInt64(env, -1);
        }
        return makeInt64(env, CicadaGetCurrentPosition(a.player->handle));
    }

    napi_value GetCurrentBufferedPosition(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeInt64(env, -1);
        }
        return makeInt64(env, CicadaGetCurrentBufferedPosition(a.player->handle));
    }

    napi_value GetMasterClockPts(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeInt64(env, -1);
        }
        return makeInt64(env, CicadaGetMasterClockPts(a.player->handle));
    }

    napi_value GetVideoResolution(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeString(env, "{\"width\":0,\"height\":0}");
        }
        int width = 0;
        int height = 0;
        CicadaGetVideoResolution(a.player->handle, width, height);
        if (width <= 0 || height <= 0) {
            // Fall back to the last VideoSizeChanged event so the UI can still
            // lay out before/without a queryable decoder.
            width = a.player->videoWidth.load();
            height = a.player->videoHeight.load();
        }
        return makeString(env, "{\"width\":" + std::to_string(width) + ",\"height\":" +
                                   std::to_string(height) + "}");
    }

    napi_value GetVideoRotation(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeInt32(env, 0);
        }
        int rotation = 0;
        CicadaGetVideoRotation(a.player->handle, rotation);
        return makeInt32(env, rotation);
    }

    // ---------------------------------------------------------------------
    // Tracks
    // ---------------------------------------------------------------------
    napi_value SwitchStreamIndex(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        int32_t index = 0;
        if (!unpack(env, info, a) || a.argc < 2 || !argInt32(env, a.argv[1], index)) {
            return makeInt32(env, -1);
        }
        return makeInt32(env, static_cast<int32_t>(CicadaSwitchStreamIndex(a.player->handle, index)));
    }

    napi_value GetCurrentStreamIndex(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        int32_t type = 0;
        if (!unpack(env, info, a) || a.argc < 2 || !argInt32(env, a.argv[1], type)) {
            return makeInt32(env, -1);
        }
        return makeInt32(env, CicadaGetCurrentStreamIndex(a.player->handle, static_cast<StreamType>(type)));
    }

    napi_value GetCurrentStreamInfo(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        int32_t type = 0;
        if (!unpack(env, info, a) || a.argc < 2 || !argInt32(env, a.argv[1], type)) {
            return makeString(env, "null");
        }
        StreamInfo *streamInfo = CicadaGetCurrentStreamInfo(a.player->handle, static_cast<StreamType>(type));
        if (streamInfo == nullptr) {
            return makeString(env, "null");
        }
        return makeString(env, streamInfoToJson(*streamInfo));
    }

    // ---------------------------------------------------------------------
    // Snapshot
    // ---------------------------------------------------------------------
    napi_value CaptureScreen(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeUndefined(env);
        }
        // Result arrives asynchronously through the "snapshot" event carrying
        // width, height and an RGBA ArrayBuffer.
        CicadaCaptureScreen(a.player->handle);
        return makeUndefined(env);
    }

    // ---------------------------------------------------------------------
    // Network / config
    // ---------------------------------------------------------------------
    napi_value SetTimeout(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        int32_t timeout = 0;
        if (!unpack(env, info, a) || a.argc < 2 || !argInt32(env, a.argv[1], timeout)) {
            return makeUndefined(env);
        }
        CicadaSetTimeout(a.player->handle, timeout);
        return makeUndefined(env);
    }

    napi_value SetDropBufferThreshold(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        int32_t value = 0;
        if (!unpack(env, info, a) || a.argc < 2 || !argInt32(env, a.argv[1], value)) {
            return makeUndefined(env);
        }
        CicadaSetDropBufferThreshold(a.player->handle, value);
        return makeUndefined(env);
    }

    napi_value SetRefer(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a) || a.argc < 2) {
            return makeUndefined(env);
        }
        const std::string refer = argString(env, a.argv[1]);
        CicadaSetRefer(a.player->handle, refer.c_str());
        return makeUndefined(env);
    }

    napi_value SetUserAgent(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a) || a.argc < 2) {
            return makeUndefined(env);
        }
        const std::string ua = argString(env, a.argv[1]);
        CicadaSetUserAgent(a.player->handle, ua.c_str());
        return makeUndefined(env);
    }

    napi_value AddCustomHttpHeader(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a) || a.argc < 2) {
            return makeUndefined(env);
        }
        const std::string header = argString(env, a.argv[1]);
        CicadaAddCustomHttpHeader(a.player->handle, header.c_str());
        return makeUndefined(env);
    }

    napi_value RemoveAllCustomHttpHeader(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeUndefined(env);
        }
        CicadaRemoveAllCustomHttpHeader(a.player->handle);
        return makeUndefined(env);
    }

    napi_value SetDefaultBandWidth(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        int32_t bandwidth = 0;
        if (!unpack(env, info, a) || a.argc < 2 || !argInt32(env, a.argv[1], bandwidth)) {
            return makeUndefined(env);
        }
        CicadaSetDefaultBandWidth(a.player->handle, bandwidth);
        return makeUndefined(env);
    }

    napi_value SetDecoderType(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        int32_t type = 0;
        if (!unpack(env, info, a) || a.argc < 2 || !argInt32(env, a.argv[1], type)) {
            return makeUndefined(env);
        }
        CicadaSetDecoderType(a.player->handle, static_cast<DecoderType>(type));
        return makeUndefined(env);
    }

    napi_value GetDecoderType(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeInt32(env, 0);
        }
        return makeInt32(env, static_cast<int32_t>(CicadaGetDecoderType(a.player->handle)));
    }

    napi_value EnterBackGround(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        bool back = false;
        if (!unpack(env, info, a) || a.argc < 2 || !argBool(env, a.argv[1], back)) {
            return makeUndefined(env);
        }
        CicadaEnterBackGround(a.player->handle, back);
        return makeUndefined(env);
    }

    // ---------------------------------------------------------------------
    // Generic options / properties
    // ---------------------------------------------------------------------
    napi_value SetOption(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a) || a.argc < 3) {
            return makeInt32(env, -1);
        }
        const std::string key = argString(env, a.argv[1]);
        const std::string value = argString(env, a.argv[2]);
        return makeInt32(env, CicadaSetOption(a.player->handle, key.c_str(), value.c_str()));
    }

    napi_value GetOption(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a) || a.argc < 2) {
            return makeString(env, "");
        }
        const std::string key = argString(env, a.argv[1]);
        char buffer[MAX_OPT_VALUE_LENGTH] = {0};
        CicadaGetOption(a.player->handle, key.c_str(), buffer);
        return makeString(env, std::string(buffer));
    }

    napi_value GetPropertyLong(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        int32_t key = 0;
        if (!unpack(env, info, a) || a.argc < 2 || !argInt32(env, a.argv[1], key)) {
            return makeInt64(env, 0);
        }
        return makeInt64(env, CicadaGetPropertyLong(a.player->handle, static_cast<PropertyKey>(key)));
    }

    napi_value GetPlayerName(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeString(env, "");
        }
        return makeString(env, CicadaGetPlayerName(a.player->handle));
    }

    napi_value InvokeComponent(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a) || a.argc < 2) {
            return makeInt32(env, -1);
        }
        const std::string content = argString(env, a.argv[1]);
        return makeInt32(env, CicadaInvokeComponent(a.player->handle, content.c_str()));
    }

    napi_value GetVideoRenderFps(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeDouble(env, 0);
        }
        return makeDouble(env, CicadaGetVideoRenderFps(a.player->handle));
    }

    napi_value GetVideoDecodeFps(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeDouble(env, 0);
        }
        return makeDouble(env, CicadaGetVideoDecodeFps(a.player->handle));
    }

    // ---------------------------------------------------------------------
    // 解码事实 / 切档状态
    //
    // 这两个都不是"配置开关"，而是问内核当前的真实状态：
    //   IsVideoDecoderHardware：活动解码器此刻走的是硬解还是软解（含中途回退），
    //     用来在界面上显示"解码方式"；
    //   IsStreamSwitchInFlight：当前是否有清晰度切换在途，给 ABR 让路用。
    // ---------------------------------------------------------------------
    napi_value IsVideoDecoderHardware(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeBool(env, false);
        }
        return makeBool(env, CicadaIsVideoDecoderHardware(a.player->handle));
    }

    napi_value IsStreamSwitchInFlight(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeBool(env, false);
        }
        return makeBool(env, CicadaIsStreamSwitchInFlight(a.player->handle));
    }

    // ---------------------------------------------------------------------
    // 画面滤镜 / 色觉辅助矩阵
    // ---------------------------------------------------------------------
    napi_value SetColorMatrix(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a) || a.argc < 2) {
            return makeBool(env, false);
        }
        napi_valuetype type = napi_undefined;
        napi_typeof(env, a.argv[1], &type);
        if (type == napi_null || type == napi_undefined) {
            // 空 = 关闭滤镜（等价于单位矩阵）。
            CicadaSetColorMatrix(a.player->handle, nullptr);
            return makeBool(env, true);
        }
        bool isArray = false;
        uint32_t length = 0;
        if (napi_is_array(env, a.argv[1], &isArray) != napi_ok || !isArray) {
            return makeBool(env, false);
        }
        if (napi_get_array_length(env, a.argv[1], &length) != napi_ok || length != 9) {
            return makeBool(env, false);
        }
        float matrix[9] = {0};
        for (uint32_t i = 0; i < 9; i++) {
            napi_value item = nullptr;
            double value = 0;
            if (napi_get_element(env, a.argv[1], i, &item) != napi_ok) {
                return makeBool(env, false);
            }
            if (napi_get_value_double(env, item, &value) != napi_ok) {
                return makeBool(env, false);
            }
            matrix[i] = static_cast<float>(value);
        }
        CicadaSetColorMatrix(a.player->handle, matrix);
        return makeBool(env, true);
    }

    napi_value SetFilterConfig(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a) || a.argc < 2) {
            return makeInt32(env, -1);
        }
        CicadaSetFilterConfig(a.player->handle, argString(env, a.argv[1]));
        return makeInt32(env, 0);
    }

    napi_value UpdateFilterConfig(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a) || a.argc < 3) {
            return makeInt32(env, -1);
        }
        CicadaUpdateFilterConfig(a.player->handle, argString(env, a.argv[1]), argString(env, a.argv[2]));
        return makeInt32(env, 0);
    }

    napi_value SetFilterInvalid(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        bool invalid = false;
        if (!unpack(env, info, a) || a.argc < 3 || !argBool(env, a.argv[2], invalid)) {
            return makeInt32(env, -1);
        }
        CicadaSetFilterInvalid(a.player->handle, argString(env, a.argv[1]), invalid);
        return makeInt32(env, 0);
    }

    // ---------------------------------------------------------------------
    // 硬解能力与效率偏好（应用层可持久化后回传，内核不再自行探测）
    // ---------------------------------------------------------------------
    napi_value GetVideoCodecSupport(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a)) {
            return makeString(env, "");
        }
        const char *json = CicadaGetVideoCodecSupport(a.player->handle);
        if (json == nullptr) {
            return makeString(env, "");
        }
        // 内核返回的是 malloc 出来的副本，必须交回内核释放。
        const std::string out(json);
        CicadaFreeString(json);
        return makeString(env, out);
    }

    napi_value SetVideoCodecSupport(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a) || a.argc < 2) {
            return makeInt32(env, -1);
        }
        napi_valuetype type = napi_undefined;
        napi_typeof(env, a.argv[1], &type);
        const bool clear = (type == napi_null || type == napi_undefined);
        const std::string json = clear ? std::string() : argString(env, a.argv[1]);
        return makeInt32(env,
                         CicadaSetVideoCodecSupport(a.player->handle, json.empty() ? nullptr : json.c_str()));
    }

    // ---------------------------------------------------------------------
    // 字符串属性（PropertyKey + 可选 JSON 参数）
    // ---------------------------------------------------------------------
    napi_value GetPropertyString(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        int32_t key = 0;
        if (!unpack(env, info, a) || a.argc < 2 || !argInt32(env, a.argv[1], key)) {
            return makeString(env, "");
        }
        const std::string paramJson = a.argc >= 3 ? argString(env, a.argv[2]) : std::string();
        const CicadaJSONItem param = paramJson.empty() ? CicadaJSONItem() : CicadaJSONItem(paramJson);
        return makeString(env, CicadaGetPropertyString(a.player->handle, static_cast<PropertyKey>(key), param));
    }

    // ---------------------------------------------------------------------
    // DRM 许可证回调
    //
    // 语义与 Android 侧 NativeBase 的同步 JNI 调用一致：内核在播放线程上调用
    // drmCallback，必须当场拿到许可证响应才能继续。这里用一条可阻塞的
    // threadsafe function 把请求投到 JS 线程，播放线程在条件变量上等 JS 的
    // 返回值 —— 放行只有两条确定性路径：JS 回调返回，或 JS 环境拆除
    // （finalize 把在途请求全部标记为 aborted），不引入任何按时间判定的分支。
    // ---------------------------------------------------------------------
    struct DrmHandshake {
        std::mutex mutex;
        std::condition_variable cond;
        bool done{false};
        bool aborted{false};
        std::string drmType;
        std::string requestType;
        std::string url;
        std::string requestData;
        std::string response;
    };

    namespace {
        void finishHandshake(DrmHandshake *handshake, bool aborted)
        {
            if (handshake == nullptr) {
                return;
            }
            std::lock_guard<std::mutex> lock(handshake->mutex);
            handshake->aborted = aborted;
            handshake->done = true;
            handshake->cond.notify_all();
        }

        void drmCallJs(napi_env env, napi_value jsCallback, void * /*context*/, void *data)
        {
            auto *handshake = static_cast<DrmHandshake *>(data);
            if (handshake == nullptr) {
                return;
            }
            if (env == nullptr || jsCallback == nullptr) {
                finishHandshake(handshake, true);
                return;
            }

            napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
            argv[0] = makeString(env, handshake->drmType);
            argv[1] = makeString(env, handshake->requestType);
            argv[2] = makeString(env, handshake->url);
            void *raw = nullptr;
            napi_value buffer = nullptr;
            if (napi_create_arraybuffer(env, handshake->requestData.size(), &raw, &buffer) == napi_ok &&
                raw != nullptr) {
                if (!handshake->requestData.empty()) {
                    memcpy(raw, handshake->requestData.data(), handshake->requestData.size());
                }
                argv[3] = buffer;
            } else {
                napi_get_undefined(env, &argv[3]);
            }

            napi_value recv = nullptr;
            napi_value result = nullptr;
            napi_get_undefined(env, &recv);
            const napi_status status = napi_call_function(env, recv, jsCallback, 4, argv, &result);
            bool pending = false;
            napi_is_exception_pending(env, &pending);
            if (pending) {
                napi_value ignored = nullptr;
                napi_get_and_clear_last_exception(env, &ignored);
            }

            if (status == napi_ok && result != nullptr) {
                napi_valuetype type = napi_undefined;
                napi_typeof(env, result, &type);
                if (type == napi_string) {
                    char *decoded = nullptr;
                    const int size = CicadaUtils::base64dec(argString(env, result), &decoded);
                    if (decoded != nullptr && size > 0) {
                        handshake->response.assign(decoded, static_cast<size_t>(size));
                    }
                    if (decoded != nullptr) {
                        free(decoded);
                    }
                } else if (type == napi_object) {
                    bool isBuffer = false;
                    if (napi_is_arraybuffer(env, result, &isBuffer) == napi_ok && isBuffer) {
                        void *bytes = nullptr;
                        size_t length = 0;
                        if (napi_get_arraybuffer_info(env, result, &bytes, &length) == napi_ok &&
                            bytes != nullptr && length > 0) {
                            handshake->response.assign(static_cast<const char *>(bytes), length);
                        }
                    }
                }
            }
            finishHandshake(handshake, false);
        }

        // JS 环境拆除时，把还在等响应的播放线程全部放行（标记 aborted）。
        void drmFinalize(napi_env /*env*/, void *finalizeData, void * /*finalizeHint*/)
        {
            auto *holder = static_cast<PlayerPtr *>(finalizeData);
            if (holder != nullptr) {
                PlayerPtr entry = *holder;
                if (entry != nullptr) {
                    std::vector<std::shared_ptr<DrmHandshake>> pending;
                    {
                        std::lock_guard<std::mutex> lock(entry->drmMutex);
                        entry->drmTsfn = nullptr;
                        pending.swap(entry->drmPending);
                    }
                    for (const auto &handshake : pending) {
                        finishHandshake(handshake.get(), true);
                    }
                }
                delete holder;
            }
        }
    } // namespace

    napi_value SetDrmRequestCallback(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        if (!unpack(env, info, a) || a.argc < 2) {
            return makeBool(env, false);
        }
        napi_valuetype type = napi_undefined;
        napi_typeof(env, a.argv[1], &type);
        if (type != napi_function) {
            return makeBool(env, false);
        }

        const PlayerPtr entry = a.player;
        // 换回调：旧的 threadsafe function 必须先撤掉（只能释放一次）。
        {
            std::lock_guard<std::mutex> lock(entry->drmMutex);
            if (entry->drmTsfn != nullptr) {
                napi_release_threadsafe_function(entry->drmTsfn, napi_tsfn_abort);
                entry->drmTsfn = nullptr;
            }
        }

        napi_value resourceName = makeString(env, "CicadaDrmRequest");
        napi_threadsafe_function tsfn = nullptr;
        const napi_status status = napi_create_threadsafe_function(env, a.argv[1], nullptr, resourceName, 0, 1,
                                                                  new PlayerPtr(entry), drmFinalize, nullptr,
                                                                  drmCallJs, &tsfn);
        if (status != napi_ok || tsfn == nullptr) {
            return makeBool(env, false);
        }
        {
            std::lock_guard<std::mutex> lock(entry->drmMutex);
            entry->drmTsfn = tsfn;
        }

        CicadaSetDrmRequestCallback(
                entry->handle,
                [entry](const Cicada::DrmRequestParam &param) -> Cicada::DrmResponseData * {
                    if (entry->released.load()) {
                        return nullptr;
                    }
                    auto *jsonParam = static_cast<CicadaJSONItem *>(param.mParam);
                    std::string requestType;
                    std::string url;
                    std::string data;
                    if (jsonParam != nullptr) {
                        requestType = jsonParam->getString("requestType");
                        url = jsonParam->getString("url");
                        data = jsonParam->getString("data");
                    }

                    auto handshake = std::make_shared<DrmHandshake>();
                    handshake->drmType = param.mDrmType;
                    handshake->requestType = requestType;
                    handshake->url = url.empty() ? param.mLicenseUrl : url;
                    if (!data.empty()) {
                        char *decoded = nullptr;
                        const int size = CicadaUtils::base64dec(data, &decoded);
                        if (decoded != nullptr && size > 0) {
                            handshake->requestData.assign(decoded, static_cast<size_t>(size));
                        }
                        if (decoded != nullptr) {
                            free(decoded);
                        }
                    }

                    napi_threadsafe_function target = nullptr;
                    {
                        std::lock_guard<std::mutex> lock(entry->drmMutex);
                        target = entry->drmTsfn;
                        if (target != nullptr) {
                            entry->drmPending.push_back(handshake);
                        }
                    }
                    if (target == nullptr) {
                        return nullptr;
                    }
                    if (napi_call_threadsafe_function(target, handshake.get(), napi_tsfn_blocking) != napi_ok) {
                        std::lock_guard<std::mutex> lock(entry->drmMutex);
                        for (auto it = entry->drmPending.begin(); it != entry->drmPending.end(); ++it) {
                            if (it->get() == handshake.get()) {
                                entry->drmPending.erase(it);
                                break;
                            }
                        }
                        return nullptr;
                    }
                    {
                        std::unique_lock<std::mutex> lock(handshake->mutex);
                        handshake->cond.wait(lock, [&handshake]() { return handshake->done; });
                    }
                    {
                        std::lock_guard<std::mutex> lock(entry->drmMutex);
                        for (auto it = entry->drmPending.begin(); it != entry->drmPending.end(); ++it) {
                            if (it->get() == handshake.get()) {
                                entry->drmPending.erase(it);
                                break;
                            }
                        }
                    }
                    if (handshake->aborted || handshake->response.empty()) {
                        return nullptr;
                    }
                    return new Cicada::DrmResponseData(const_cast<char *>(handshake->response.data()),
                                                       static_cast<int>(handshake->response.size()));
                });
        return makeBool(env, true);
    }
    // ---------------------------------------------------------------------
    // 流元数据（Stream_meta 的原始字段）
    //
    // 与 getCurrentStreamInfo 的分工：那个给的是 StreamInfo（码率/分辨率/语言等
    // 展示用字段），这里给的是内核 Stream_meta 的原样数值（profile、frame_size、
    // sample_fmt、extradata 大小、ptsTimeBase、interlaced…），排查"某个流到底被
    // 解成了什么"时用它。codec 是内核的 AFCodecID 枚举值，编码短名（H.264/H.265…）
    // 统一走 getVideoCodecSupport()，内核里那份短名表只有一份。
    // ---------------------------------------------------------------------
    namespace {
        const char *streamMetaTypeName(Stream_type type)
        {
            switch (type) {
                case STREAM_TYPE_VIDEO:
                    return "video";
                case STREAM_TYPE_AUDIO:
                    return "audio";
                case STREAM_TYPE_SUB:
                    return "subtitle";
                case STREAM_TYPE_MIXED:
                    return "mixed";
                default:
                    return "unknown";
            }
        }

        std::string streamMetaToJson(const Stream_meta &meta)
        {
            std::string out = "{";
            out += "\"index\":" + std::to_string(meta.index);
            out += ",\"type\":\"" + std::string(streamMetaTypeName(meta.type)) + "\"";
            out += ",\"codec\":" + std::to_string(static_cast<int>(meta.codec));
            out += ",\"codecTag\":" + std::to_string(meta.codec_tag);
            out += ",\"duration\":" + std::to_string(meta.duration);
            out += ",\"bitrate\":" + std::to_string(meta.bitrate);
            out += ",\"bandwidth\":" + std::to_string(meta.bandwidth);
            out += ",\"width\":" + std::to_string(meta.width);
            out += ",\"height\":" + std::to_string(meta.height);
            out += ",\"displayWidth\":" + std::to_string(meta.displayWidth);
            out += ",\"displayHeight\":" + std::to_string(meta.displayHeight);
            out += ",\"rotate\":" + std::to_string(meta.rotate);
            out += ",\"avgFps\":" + std::to_string(meta.avg_fps);
            out += ",\"interlaced\":" + std::to_string(static_cast<int>(meta.interlaced));
            out += ",\"pixelFormat\":" + std::to_string(static_cast<int>(meta.pixel_fmt));
            out += ",\"channels\":" + std::to_string(meta.channels);
            out += ",\"sampleRate\":" + std::to_string(meta.samplerate);
            out += ",\"frameSize\":" + std::to_string(meta.frame_size);
            out += ",\"profile\":" + std::to_string(meta.profile);
            out += ",\"sampleFormat\":" + std::to_string(static_cast<int>(meta.sample_fmt));
            out += ",\"bitsPerCodedSample\":" + std::to_string(meta.bits_per_coded_sample);
            out += ",\"ptsTimeBase\":" + std::to_string(meta.ptsTimeBase);
            out += ",\"extradataSize\":" + std::to_string(meta.extradata_size);
            out += ",\"hasKeyUrl\":";
            out += (meta.keyUrl != nullptr ? "true" : "false");
            out += ",\"lang\":\"";
            out += jsonEscape(meta.lang != nullptr ? meta.lang : meta.language);
            out += "\",\"title\":\"" + jsonEscape(meta.title);
            out += "\",\"description\":\"" + jsonEscape(meta.description);
            out += "\"}";
            return out;
        }
    } // namespace

    napi_value GetCurrentStreamMeta(napi_env env, napi_callback_info info)
    {
        CallArgs a;
        int32_t type = 0;
        if (!unpack(env, info, a) || a.argc < 2 || !argInt32(env, a.argv[1], type)) {
            return makeString(env, "");
        }
        Stream_meta meta{};
        if (CicadaGetCurrentStreamMeta(a.player->handle, &meta, static_cast<StreamType>(type)) != 0) {
            return makeString(env, "");
        }
        return makeString(env, streamMetaToJson(meta));
    }
} // namespace cicada_ohos
