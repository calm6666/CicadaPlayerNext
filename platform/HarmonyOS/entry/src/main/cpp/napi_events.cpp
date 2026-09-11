// napi_events.cpp
//
// Bridges every callback of playerListener_t to a single ArkTS listener.
//
// Player callbacks are invoked on native player threads, so they must not touch
// napi directly. Each player owns one napi_threadsafe_function; callbacks build
// a small payload and post it, and the JS side receives a uniform signature:
//
//   normal events : (event: string, a: number, b: number, text: string) => void
//   "snapshot"    : (event: 'snapshot', width: number, height: number,
//                    rgba: ArrayBuffer) => void
//   "mediaInfo"   : (event: 'mediaInfo', count: number, 0, json: string) => void

#include "napi_player.h"

#include <cstring>

namespace cicada_ohos {
    namespace {

        enum class EventKind {
            Void,
            Number,
            Pair,
            CodeMsg,
            Subtitle,
            MediaInfo,
            Snapshot,
        };

        struct EventPayload {
            PlayerPtr owner;
            const char *name{nullptr};
            EventKind kind{EventKind::Void};
            double a{0};
            double b{0};
            std::string text;
            std::vector<uint8_t> pixels;
        };

        void callJs(napi_env env, napi_value jsCallback, void * /*context*/, void *data);

        void finalizeHolder(napi_env /*env*/, void *finalizeData, void * /*finalizeHint*/)
        {
            // Frees the shared_ptr holder handed to the native listener as
            // userData. Runs once the threadsafe function is fully drained.
            delete static_cast<PlayerPtr *>(finalizeData);
        }

        PlayerPtr entryFrom(void *userData)
        {
            if (userData == nullptr) {
                return nullptr;
            }
            auto *holder = static_cast<PlayerPtr *>(userData);
            return *holder;
        }

        void post(const PlayerPtr &entry, const char *name, EventKind kind, double a, double b,
                  const char *text, size_t textLen, std::vector<uint8_t> &&pixels)
        {
            if (entry == nullptr || entry->tsfn == nullptr || entry->released.load()) {
                return;
            }
            auto *payload = new EventPayload();
            payload->owner = entry;
            payload->name = name;
            payload->kind = kind;
            payload->a = a;
            payload->b = b;
            if (text != nullptr && textLen > 0) {
                payload->text.assign(text, textLen);
            }
            payload->pixels = std::move(pixels);

            if (napi_call_threadsafe_function(entry->tsfn, payload, napi_tsfn_nonblocking) != napi_ok) {
                delete payload;
            }
        }

        void postVoid(void *userData, const char *name)
        {
            post(entryFrom(userData), name, EventKind::Void, 0, 0, nullptr, 0, {});
        }

        void postNumber(void *userData, const char *name, int64_t value)
        {
            post(entryFrom(userData), name, EventKind::Number, static_cast<double>(value), 0, nullptr,
                 0, {});
        }

        void postPair(void *userData, const char *name, int64_t a, int64_t b)
        {
            post(entryFrom(userData), name, EventKind::Pair, static_cast<double>(a),
                 static_cast<double>(b), nullptr, 0, {});
        }

        void postCodeMsg(void *userData, const char *name, int64_t code, const void *msg)
        {
            const char *text = static_cast<const char *>(msg);
            post(entryFrom(userData), name, EventKind::CodeMsg, static_cast<double>(code), 0, text,
                 text != nullptr ? strlen(text) : 0, {});
        }

        // ---- the 26 playerListener_t fields -----------------------------
        void cbLoopingStart(void *d) { postVoid(d, "loopingStart"); }
        void cbPrepared(void *d) { postVoid(d, "prepared"); }
        void cbCompletion(void *d) { postVoid(d, "completion"); }
        void cbFirstFrameShow(void *d) { postVoid(d, "firstFrameShow"); }
        void cbLoadingStart(void *d) { postVoid(d, "loadingStart"); }
        void cbLoadingEnd(void *d) { postVoid(d, "loadingEnd"); }
        void cbAutoPlayStart(void *d) { postVoid(d, "autoPlayStart"); }

        void cbSeeking(int64_t pos, void *d) { postNumber(d, "seeking", pos); }
        void cbSeekEnd(int64_t pos, void *d) { postNumber(d, "seekEnd", pos); }
        void cbPositionUpdate(int64_t pos, void *d) { postNumber(d, "positionUpdate", pos); }
        void cbUtcTimeUpdate(int64_t t, void *d) { postNumber(d, "utcTimeUpdate", t); }
        void cbBufferPositionUpdate(int64_t pos, void *d) { postNumber(d, "bufferPositionUpdate", pos); }
        void cbLoadingProgress(int64_t percent, void *d) { postNumber(d, "loadingProgress", percent); }
        void cbCurrentDownLoadSpeed(int64_t speed, void *d) { postNumber(d, "currentDownLoadSpeed", speed); }

        void cbVideoSizeChanged(int64_t width, int64_t height, void *d)
        {
            PlayerPtr entry = entryFrom(d);
            if (entry != nullptr) {
                entry->videoWidth.store(static_cast<int>(width));
                entry->videoHeight.store(static_cast<int>(height));
            }
            post(entry, "videoSizeChanged", EventKind::Pair, static_cast<double>(width),
                 static_cast<double>(height), nullptr, 0, {});
        }

        void cbStatusChanged(int64_t oldStatus, int64_t newStatus, void *d)
        {
            postPair(d, "statusChanged", oldStatus, newStatus);
        }

        void cbVideoRendered(int64_t timeMs, int64_t pts, void *d)
        {
            postPair(d, "videoRendered", timeMs, pts);
        }

        void cbAudioRendered(int64_t timeMs, int64_t pts, void *d)
        {
            postPair(d, "audioRendered", timeMs, pts);
        }

        void cbError(int64_t code, const void *msg, void *d) { postCodeMsg(d, "error", code, msg); }
        void cbEvent(int64_t code, const void *msg, void *d) { postCodeMsg(d, "event", code, msg); }

        void cbMediaInfoGet(int64_t count, const void *infos, void *d)
        {
            const auto *mediaInfo = static_cast<const MediaInfo *>(infos);
            if (mediaInfo == nullptr) {
                return;
            }
            const std::string json = mediaInfoToJson(*mediaInfo);
            post(entryFrom(d), "mediaInfo", EventKind::MediaInfo, static_cast<double>(count), 0,
                 json.c_str(), json.size(), {});
        }

        void cbStreamSwitchSuc(int64_t type, const void *item, void *d)
        {
            const auto *info = static_cast<const StreamInfo *>(item);
            if (info == nullptr) {
                return;
            }
            const std::string json = streamInfoToJson(*info);
            post(entryFrom(d), "streamSwitchSuc", EventKind::CodeMsg, static_cast<double>(type), 0,
                 json.c_str(), json.size(), {});
        }

        // CaptureScreen carries (width, height, RGBA8888 buffer, w*h*4 bytes).
        // The (-1,-1) form is the "release this image" notification and is not
        // a real snapshot.
        void cbCaptureScreen(int64_t width, int64_t height, const void *buffer, void *d)
        {
            if (width <= 0 || height <= 0 || buffer == nullptr) {
                return;
            }
            const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
            std::vector<uint8_t> pixels(bytes);
            memcpy(pixels.data(), buffer, bytes);
            post(entryFrom(d), "snapshot", EventKind::Snapshot, static_cast<double>(width),
                 static_cast<double>(height), nullptr, 0, std::move(pixels));
        }

        void cbSubtitleHide(int64_t id, int64_t size, const void *content, void *d)
        {
            const char *text = static_cast<const char *>(content);
            post(entryFrom(d), "subtitleHide", EventKind::Subtitle, static_cast<double>(id),
                 static_cast<double>(size), text, size > 0 ? static_cast<size_t>(size) : 0, {});
        }

        void cbSubtitleShow(int64_t id, int64_t size, const void *content, void *d)
        {
            const char *text = static_cast<const char *>(content);
            post(entryFrom(d), "subtitleShow", EventKind::Subtitle, static_cast<double>(id),
                 static_cast<double>(size), text, size > 0 ? static_cast<size_t>(size) : 0, {});
        }

        void cbSubtitleExtAdd(int64_t index, const void *url, void *d)
        {
            postCodeMsg(d, "subtitleExtAdd", index, url);
        }

        void cbSubtitleHeader(int64_t id, const void *header, void *d)
        {
            postCodeMsg(d, "subtitleHeader", id, header);
        }

        // ---- JS side ------------------------------------------------------
        void callJs(napi_env env, napi_value jsCallback, void * /*context*/, void *data)
        {
            std::unique_ptr<EventPayload> payload(static_cast<EventPayload *>(data));
            if (payload == nullptr) {
                return;
            }
            // env/jsCallback are null on the finalising call; the owning entry
            // may also have been released in the meantime.
            if (env == nullptr || jsCallback == nullptr) {
                return;
            }
            if (payload->owner != nullptr && payload->owner->released.load()) {
                return;
            }

            napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
            napi_create_string_utf8(env, payload->name != nullptr ? payload->name : "", NAPI_AUTO_LENGTH,
                                    &argv[0]);
            argv[1] = makeDouble(env, payload->a);
            argv[2] = makeDouble(env, payload->b);

            if (payload->kind == EventKind::Snapshot) {
                void *raw = nullptr;
                napi_value buffer = nullptr;
                if (napi_create_arraybuffer(env, payload->pixels.size(), &raw, &buffer) == napi_ok) {
                    if (raw != nullptr && !payload->pixels.empty()) {
                        memcpy(raw, payload->pixels.data(), payload->pixels.size());
                    }
                    argv[3] = buffer;
                } else {
                    argv[3] = makeUndefined(env);
                }
            } else {
                argv[3] = makeString(env, payload->text);
            }

            napi_value global = nullptr;
            napi_value result = nullptr;
            napi_get_global(env, &global);
            napi_call_function(env, global, jsCallback, 4, argv, &result);
        }

    } // namespace

    napi_value SetListener(napi_env env, napi_callback_info info)
    {
        size_t argc = 2;
        napi_value argv[2] = {nullptr, nullptr};
        napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
        if (argc < 2) {
            return makeBool(env, false);
        }

        int32_t id = 0;
        if (!argInt32(env, argv[0], id)) {
            return makeBool(env, false);
        }

        napi_valuetype type = napi_undefined;
        napi_typeof(env, argv[1], &type);
        if (type != napi_function) {
            return makeBool(env, false);
        }

        PlayerPtr entry = findPlayer(id);
        if (entry == nullptr || entry->released.load()) {
            return makeBool(env, false);
        }
        return makeBool(env, attachListener(env, entry, argv[1]));
    }

    bool attachListener(napi_env env, const PlayerPtr &entry, napi_value callback)
    {
        if (entry == nullptr || entry->handle == nullptr) {
            return false;
        }

        // A previous listener is replaced wholesale.
        detachListener(entry);

        napi_value resourceName = nullptr;
        napi_create_string_utf8(env, "CicadaPlayerEvent", NAPI_AUTO_LENGTH, &resourceName);

        auto *holder = new PlayerPtr(entry);
        napi_threadsafe_function tsfn = nullptr;
        const napi_status status =
                napi_create_threadsafe_function(env, callback, nullptr, resourceName, 0, 1, holder,
                                                finalizeHolder, nullptr, callJs, &tsfn);
        if (status != napi_ok || tsfn == nullptr) {
            delete holder;
            return false;
        }

        entry->env = env;
        entry->tsfn = tsfn;

        playerListener listener{};
        listener.LoopingStart = cbLoopingStart;
        listener.Prepared = cbPrepared;
        listener.Completion = cbCompletion;
        listener.FirstFrameShow = cbFirstFrameShow;
        listener.LoadingStart = cbLoadingStart;
        listener.LoadingEnd = cbLoadingEnd;
        listener.AutoPlayStart = cbAutoPlayStart;

        listener.Seeking = cbSeeking;
        listener.SeekEnd = cbSeekEnd;
        listener.PositionUpdate = cbPositionUpdate;
        listener.UtcTimeUpdate = cbUtcTimeUpdate;
        listener.BufferPositionUpdate = cbBufferPositionUpdate;
        listener.LoadingProgress = cbLoadingProgress;
        listener.CurrentDownLoadSpeed = cbCurrentDownLoadSpeed;

        listener.VideoSizeChanged = cbVideoSizeChanged;
        listener.StatusChanged = cbStatusChanged;
        listener.VideoRendered = cbVideoRendered;
        listener.AudioRendered = cbAudioRendered;

        listener.ErrorCallback = cbError;

        listener.EventCallback = cbEvent;
        listener.MediaInfoGet = cbMediaInfoGet;
        listener.StreamSwitchSuc = cbStreamSwitchSuc;

        listener.CaptureScreen = cbCaptureScreen;
        listener.SubtitleHide = cbSubtitleHide;
        listener.SubtitleShow = cbSubtitleShow;
        listener.SubtitleExtAdd = cbSubtitleExtAdd;
        listener.SubtitleHeader = cbSubtitleHeader;

        listener.userData = holder;

        if (CicadaSetListener(entry->handle, listener) != 0) {
            detachListener(entry);
            return false;
        }
        return true;
    }

    void detachListener(const PlayerPtr &entry)
    {
        if (entry == nullptr) {
            return;
        }
        // Deliberately does NOT call CicadaSetListener(handle, {}).
        //
        // PlayerNotifier::setListener() runs afThread::pause(), which blocks until
        // the notifier loop comes back around the top of its while loop. That loop
        // sits inside PlayerNotifier::post_loop(), which waits on
        //   mCondition.wait(uMutex, [this]{ return !mRunning || !mEventQueue.empty(); })
        // -- there is no pause flag and no timeout. So re-setting the listener on a
        // RUNNING but IDLE player never returns, and on the UI thread that is an
        // immediate APP_INPUT_BLOCK freeze.
        //
        // (The first setListener, from create(), is safe only because the notifier
        // thread has not been started yet: afThread::pause() checks
        // "THREAD_STATUS_RUNNING == mThreadStatus" and returns at once otherwise.)
        //
        // CicadaReleasePlayer() tears the notifier down correctly instead: its
        // destructor sets mRunning = false and notifies, post_loop returns, and the
        // thread is joined. Late events are already dropped by the `released` flag
        // and by the closing threadsafe function, so clearing the native listener
        // is unnecessary.
        if (entry->tsfn != nullptr) {
            napi_release_threadsafe_function(entry->tsfn, napi_tsfn_release);
            entry->tsfn = nullptr;
        }
        entry->env = nullptr;
    }

} // namespace cicada_ohos
