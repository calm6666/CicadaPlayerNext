// napi_init.cpp
//
// NAPI bridge between the ArkTS page and the Cicada native player.
//
// Exposed JS API (import testNapi from 'libentry.so'):
//   create(): number                 - player handle id
//   release(id: number): void
//   setDataSource(id, url): void
//   setDataSourceManifest(id, json): void     - object-based playback + DRM
//   setSurface(id, surfaceId: string): void   - XComponent surfaceId string
//   setDrmCallback(id, cb): void              - license request callback
//   prepare(id): void
//   start(id): void
//   pause(id): void
//   seek(id, positionMs, accurate): void
//   stop(id): void
//   release(id): void
//

#include <napi/native_api.h>
#include <native_window/external_window.h>

#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

extern "C" {
#include "media_player_api.h"
}

namespace {

    struct PlayerEntry {
        playerHandle *handle{nullptr};
        OHNativeWindow *window{nullptr};
        napi_threadsafe_function drmTsfn{nullptr};
    };

    std::mutex gMutex;
    int gNextId = 1;
    std::map<int, PlayerEntry> gPlayers;

    int getPlayerId(napi_env env, napi_value value)
    {
        int32_t id = 0;
        napi_get_value_int32(env, value, &id);
        return id;
    }

    PlayerEntry *getPlayer(int id)
    {
        auto it = gPlayers.find(id);
        return it == gPlayers.end() ? nullptr : &it->second;
    }

    napi_value create(napi_env env, napi_callback_info info)
    {
        std::lock_guard<std::mutex> lock(gMutex);
        PlayerEntry entry;
        entry.handle = CicadaCreatePlayer(nullptr);
        int id = gNextId++;
        gPlayers[id] = entry;
        napi_value result;
        napi_create_int32(env, id, &result);
        return result;
    }

    napi_value release(napi_env env, napi_callback_info info)
    {
        size_t argc = 1;
        napi_value args[1];
        napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
        int id = getPlayerId(env, args[0]);
        std::lock_guard<std::mutex> lock(gMutex);
        auto it = gPlayers.find(id);
        if (it != gPlayers.end()) {
            if (it->second.window != nullptr) {
                OH_NativeWindow_DestroyNativeWindow(it->second.window);
            }
            if (it->second.handle != nullptr) {
                CicadaReleasePlayer(&it->second.handle);
            }
            gPlayers.erase(it);
        }
        return nullptr;
    }

    napi_value setDataSource(napi_env env, napi_callback_info info)
    {
        size_t argc = 2;
        napi_value args[2];
        napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
        int id = getPlayerId(env, args[0]);
        size_t len = 0;
        napi_get_value_string_utf8(env, args[1], nullptr, 0, &len);
        std::string url(len, '\0');
        napi_get_value_string_utf8(env, args[1], &url[0], len + 1, &len);
        if (PlayerEntry *entry = getPlayer(id)) {
            CicadaSetDataSourceWithUrl(entry->handle, url.c_str());
        }
        return nullptr;
    }

    napi_value setDataSourceManifest(napi_env env, napi_callback_info info)
    {
        size_t argc = 2;
        napi_value args[2];
        napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
        int id = getPlayerId(env, args[0]);
        size_t len = 0;
        napi_get_value_string_utf8(env, args[1], nullptr, 0, &len);
        std::string json(len, '\0');
        napi_get_value_string_utf8(env, args[1], &json[0], len + 1, &len);
        if (PlayerEntry *entry = getPlayer(id)) {
            // Object-based playback: unified MediaManifest JSON, DRM-capable.
            CicadaSetDataSourceWithManifest(entry->handle, json.c_str());
        }
        return nullptr;
    }

    napi_value setSurface(napi_env env, napi_callback_info info)
    {
        size_t argc = 2;
        napi_value args[2];
        napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
        int id = getPlayerId(env, args[0]);
        size_t len = 0;
        napi_get_value_string_utf8(env, args[1], nullptr, 0, &len);
        std::string surfaceId(len, '\0');
        napi_get_value_string_utf8(env, args[1], &surfaceId[0], len + 1, &len);

        uint64_t id64 = strtoull(surfaceId.c_str(), nullptr, 10);
        OHNativeWindow *window = OH_NativeWindow_CreateNativeWindowFromSurfaceId(id64);
        if (window == nullptr) {
            return nullptr;
        }

        if (PlayerEntry *entry = getPlayer(id)) {
            if (entry->window != nullptr) {
                OH_NativeWindow_DestroyNativeWindow(entry->window);
            }
            entry->window = window;
            // The OHOS decoder (surface mode) receives the window through SetView.
            CicadaSetView(entry->handle, window);
        }
        return nullptr;
    }

    napi_value prepare(napi_env env, napi_callback_info info)
    {
        size_t argc = 1;
        napi_value args[1];
        napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
        if (PlayerEntry *entry = getPlayer(getPlayerId(env, args[0]))) {
            CicadaPreparePlayer(entry->handle);
        }
        return nullptr;
    }

    napi_value start(napi_env env, napi_callback_info info)
    {
        size_t argc = 1;
        napi_value args[1];
        napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
        if (PlayerEntry *entry = getPlayer(getPlayerId(env, args[0]))) {
            CicadaStartPlayer(entry->handle);
        }
        return nullptr;
    }

    napi_value pause(napi_env env, napi_callback_info info)
    {
        size_t argc = 1;
        napi_value args[1];
        napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
        if (PlayerEntry *entry = getPlayer(getPlayerId(env, args[0]))) {
            CicadaPausePlayer(entry->handle);
        }
        return nullptr;
    }

    napi_value seek(napi_env env, napi_callback_info info)
    {
        size_t argc = 3;
        napi_value args[3];
        napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
        int64_t positionMs = 0;
        bool accurate = false;
        napi_get_value_int64(env, args[1], &positionMs);
        napi_get_value_bool(env, args[2], &accurate);
        if (PlayerEntry *entry = getPlayer(getPlayerId(env, args[0]))) {
            CicadaSeekToTime(entry->handle, positionMs, accurate);
        }
        return nullptr;
    }

    napi_value stop(napi_env env, napi_callback_info info)
    {
        size_t argc = 1;
        napi_value args[1];
        napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
        if (PlayerEntry *entry = getPlayer(getPlayerId(env, args[0]))) {
            CicadaStopPlayer(entry->handle);
        }
        return nullptr;
    }

    napi_value setVolume(napi_env env, napi_callback_info info)
    {
        size_t argc = 2;
        napi_value args[2];
        napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
        double volume = 1.0;
        napi_get_value_double(env, args[1], &volume);
        if (PlayerEntry *entry = getPlayer(getPlayerId(env, args[0]))) {
            CicadaSetVolume(entry->handle, static_cast<float>(volume));
        }
        return nullptr;
    }

    napi_value getDuration(napi_env env, napi_callback_info info)
    {
        size_t argc = 1;
        napi_value args[1];
        napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
        int64_t duration = -1;
        if (PlayerEntry *entry = getPlayer(getPlayerId(env, args[0]))) {
            duration = CicadaGetDuration(entry->handle);
        }
        napi_value result;
        napi_create_int64(env, duration, &result);
        return result;
    }

    napi_value getCurrentPosition(napi_env env, napi_callback_info info)
    {
        size_t argc = 1;
        napi_value args[1];
        napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
        int64_t position = -1;
        if (PlayerEntry *entry = getPlayer(getPlayerId(env, args[0]))) {
            position = CicadaGetCurrentPosition(entry->handle);
        }
        napi_value result;
        napi_create_int64(env, position, &result);
        return result;
    }
} // anonymous namespace

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        {"create", nullptr, create, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"release", nullptr, release, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDataSource", nullptr, setDataSource, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDataSourceManifest", nullptr, setDataSourceManifest, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setSurface", nullptr, setSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"prepare", nullptr, prepare, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"start", nullptr, start, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"pause", nullptr, pause, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"seek", nullptr, seek, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stop", nullptr, stop, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setVolume", nullptr, setVolume, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getDuration", nullptr, getDuration, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getCurrentPosition", nullptr, getCurrentPosition, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

// C++11-compatible positional initializer (designated initializers are a C99
// feature not accepted under strict -std=c++11).
static napi_module demoModule = {
        1,      // nm_version
        0,      // nm_flags
        nullptr,// nm_filename
        Init,   // nm_register_func
        "entry",// nm_modname
        nullptr,// nm_priv
        {0},    // reserved
};

extern "C" __attribute__((constructor)) void RegisterEntryModule(void)
{
    napi_module_register(&demoModule);
}
