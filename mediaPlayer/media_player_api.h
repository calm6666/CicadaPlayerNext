#ifndef CICADA_PLAYER_H_
#define CICADA_PLAYER_H_

#include "native_cicada_player_def.h"
#include <cacheModule/cache/CacheConfig.h>
#include <demuxer/manifest/MediaManifest.h>
#include <drm/DrmHandler.h>
#include <render/video/IVideoRender.h>
#include <utils/AFMediaType.h>
#include <vector>

//typedef struct Stream_meta_t Stream_meta;

typedef struct playerHandle_t playerHandle;

/*
 *create the Cicada player
 */
playerHandle *CicadaCreatePlayer(const char *opts);

/*
 * release Cicada player
 */
void CicadaReleasePlayer(playerHandle **player);

/*
 *set player listener
 */
int CicadaSetListener(playerHandle *pHandle, const playerListener &Listener);


void CicadaSetOnRenderCallBack(playerHandle *pHandle, onRenderFrame cb, void *userData);

void CicadaSetAudioRenderingCallBack(playerHandle *pHandle, onRenderFrame cb, void *userData);

void CicadaSetVideoRenderingCallBack(playerHandle *pHandle, videoRenderingFrameCB cb, void *userData);

void CicadaSetUpdateViewCallback(playerHandle *pHandle, UpdateViewCB cb, void *userData);

void CicadaSetUrlHashCallback(playerHandle *pHandle, UrlHashCB cb, void *userData);

/*
 * set external component callback
 */
attribute_deprecated
void CicadaSetComponentCb(playerHandle *pHandle, player_component_type type, void *factory);

/*
 * set bit stream data cb, if don't support seek, set it to null
 */
void CicadaSetBitStreamCb(playerHandle *pHandle, readCB read, seekCB seek, void *arg);

/*
 * set error code converter
 */
void CicadaSetErrorConverter(playerHandle *player, ErrorConverter *converter);

/*
 * set decoder type
 */
void CicadaSetDecoderType(playerHandle *player, DecoderType type);

/*
 * get decoder type
 */
DecoderType CicadaGetDecoderType(playerHandle *player);

/*
 * 【当前视频解码器实际在用硬解还是软解】
 *
 * 与 CicadaGetDecoderType() 的口径不同（那个读的是解码器打开时留下的标志位）：
 * 这里问的是**活动解码器实例**眼下真正走的那条路，包含"解码途中从硬解退回软解"
 * 的情况，所以适合拿来做界面上的"解码方式"显示。没有视频解码器（没有片源 /
 * 还没建起来 / 已销毁）时返回 false。
 */
bool CicadaIsVideoDecoderHardware(playerHandle *player);

/*
 *set player view
 */
void CicadaSetView(playerHandle *player, void *view);

void CicadaClearScreen(playerHandle *player);

/*
 *set play url source
 */
void CicadaSetDataSourceWithUrl(playerHandle *player, const char *url);

/*
 * 对象模式播放：传入统一的 MediaManifest 清单对象
 * （对应 hili-player 的 MediaManifest JSON 结构），无需 m3u8/mpd 文本。
 * 支持 AES-128 分段加密与 DRM（Widevine/FairPlay/ClearKey）。
 */
void CicadaSetDataSourceWithManifestObject(playerHandle *player, const Cicada::Manifest::MediaManifest &manifest);

/*
 * 对象模式播放：传入 MediaManifest 结构的 JSON 文本。
 */
void CicadaSetDataSourceWithManifest(playerHandle *player, const char *jsonManifest);

/*
 * switch stream by stream index
 */
StreamType CicadaSwitchStreamIndex(playerHandle *player, int index);

/*
 * 【ABR 让路】当前是否有清晰度切换在途（见 ICicadaPlayer::IsStreamSwitchInFlight）。
 * ABR 每秒一次决策，切档在途时让过这一 tick，避免把用户手动切档判成 CANCELED。
 */
bool CicadaIsStreamSwitchInFlight(playerHandle *player);

void CicadaAddCustomHttpHeader(playerHandle *pHandle, const char *header);

void CicadaRemoveAllCustomHttpHeader(playerHandle *pHandle);

void CicadaCaptureScreen(playerHandle *pHandle);

int64_t CicadaGetMasterClockPts(playerHandle *pHandle);

void CicadaSetClockRefer(playerHandle *pHandle,clockRefer cb, void *arg);

/*
* prepare to play
*/
void CicadaPreparePlayer(playerHandle *player);

/*
* start to play
*/
void CicadaStartPlayer(playerHandle *player);

/*
 * pause the player
 */
void CicadaPausePlayer(playerHandle *player);

/*
 * set playback speed, perhaps no effect if the range not be supported
 */
void CicadaPlayerSetSpeed(playerHandle *pHandle, float speed);

/*
 * get the playback speed
 */
float CicadaPlayerGetSpeed(playerHandle *pHandle);

/*
* Set the playback volume, 0-1
*/
void CicadaSetVolume(playerHandle *player, float volumne);

/**
 * Get the playback volume, 0-1
 */
float CicadaGetVolume(playerHandle *player);

/**
 * seek to the new position, Accurate or inAccurate
 */
void CicadaSeekToTime(playerHandle *player, int64_t seekPos, bool bAccurate);

/*
 * stop the playback
 */
int CicadaStopPlayer(playerHandle *player);

/*
 * get the duration
 */
int64_t CicadaGetDuration(playerHandle *player);

/*
 * get current stream index by stream type
 */
int CicadaGetCurrentStreamIndex(playerHandle *pHandle, StreamType type);

/*
 * get current stream info by stream type
 */
StreamInfo *CicadaGetCurrentStreamInfo(playerHandle *player, StreamType type);

/*
 * get the plaback current position
 */
int64_t CicadaGetCurrentPosition(playerHandle *player);

/*
 * get the playback buffered position
 */
int64_t CicadaGetCurrentBufferedPosition(playerHandle *player);

/*
 * enter background or enter forground, just for ios
 */
void CicadaEnterBackGround(playerHandle *player, bool back);

/*
 * set the playback mute
 */
void CicadaSetMute(playerHandle *player, bool bMute);

/**
 * get the playback muted or not
 */
bool CicadaIsMute(playerHandle *player);

/*
* set network timeout,defalut is 15000
*/
void CicadaSetTimeout(playerHandle *player, int timeout);

/*
 * set the max delay time when in live mode
 */
void CicadaSetDropBufferThreshold(playerHandle *player, int dropValue);

/*
 * set referer
 */
void CicadaSetRefer(playerHandle *player, const char *referer);

/*
 * set referer
 */
void CicadaSetUserAgent(playerHandle *player, const char *userAgent);

/*
 * set rending scaling mode
 */
void CicadaSetScaleMode(playerHandle *player, ScaleMode mode);

/**
 * get rending scaling mode
 */
ScaleMode CicadaGetScaleMode(playerHandle *player);

/**
 * set rending rotate mode .
 */
void CicadaSetRotateMode(playerHandle *pHandle, RotateMode mode);

/**
 * get rending rotate mode
 */
RotateMode CicadaGetRotateMode(playerHandle *player);

/**
 * set rending mirror mode .
 */
void CicadaSetMirrorMode(playerHandle *pHandle, MirrorMode mode);

/*
 * set clear color
 */
void CicadaSetVideoBackgroundColor(playerHandle *pHandle, uint32_t color);

/**
 * 【色觉辅助滤镜】设置 3x3 颜色矩阵（行主序 9 个 float，单位矩阵 = 关闭）。
 *
 * 【回退点 C3】删掉本声明 + media_player_api.cpp 里的实现即可。
 */
void CicadaSetColorMatrix(playerHandle *pHandle, const float *matrix);

/**
 * get rending mirror mode
 */
MirrorMode CicadaGetMirrorMode(playerHandle *player);

/*
 * set playback to loop mode
 */
void CicadaSetLoop(playerHandle *player, bool bLoop);

/*
 * get playback is loop or not
 */
bool CicadaGetLoop(playerHandle *pHandle);

/*
 * set playback to autoplay mode
 */
void CicadaSetAutoPlay(playerHandle *player, bool bAutoPlay);

/*
 * get playback is autoplay or not
 */
bool CicadaIsAutoPLay(playerHandle *pHandle);


/**
 * Set filter config. call this before prepare. If want update filter config, call updateFilterConfig()
 * @param filterConfig
 */
void CicadaSetFilterConfig(playerHandle *pHandle, const std::string &filterConfig);

/**
 * upadate filter config.
 * @param target  Empty or not found, will make no effect. Should not be empty.
 * @param options
 */
void CicadaUpdateFilterConfig(playerHandle *pHandle, const std::string &target, const std::string &options);

/**
 * disable/enable filter.
 * @param target  if empty , disable all filters.
 * @param invalid  true: enable(default); false: disable
 */
void CicadaSetFilterInvalid(playerHandle *pHandle, const std::string &target, bool invalid);


void CicadaAddExtSubtitle(playerHandle *pHandle, const char *uri);

void CicadaSelectExtSubtitle(playerHandle *pHandle, int index, bool select);

int CicadaSetStreamDelayTime(playerHandle *pHandle, int index, int64_t time);

/*
 * get video with and height
 */
void CicadaGetVideoResolution(playerHandle *player, int &width, int &height);

/*
 * get video rotation
 */
void CicadaGetVideoRotation(playerHandle *player, int &rotation);

/*
 * get string property
 */
std::string CicadaGetPropertyString(playerHandle *player, PropertyKey key, const CicadaJSONItem &param);

/*
 * get long property
 */
int64_t CicadaGetPropertyLong(playerHandle *player, PropertyKey key);

/*
 * set option by key
 */
int CicadaSetOption(playerHandle *player, const char *key, const char *value);

/*
 * get option by key
 */
void CicadaGetOption(playerHandle *pHandle, const char *key, char *value);

void CicadaSetMediaFrameCb(playerHandle *player, playerMediaFrameCb func, void *arg);

int CicadaGetCurrentStreamMeta(playerHandle *player, Stream_meta *meta, StreamType type);

void CicadaReload(playerHandle *pHandle);

void CicadaSetDefaultBandWidth(playerHandle *player, int bandWidth);

int CicadaInvokeComponent(playerHandle *player, const char *content);

void CicadaSetDrmRequestCallback(playerHandle *player, const std::function<Cicada::DrmResponseData*(const Cicada::DrmRequestParam& drmRequestParam)> & drmCallback);

std::string CicadaGetPlayerName(playerHandle *player);

/*
 * ==================== 【应用层视频编码"硬解能力 + 效率偏好"】====================
 *
 * 应用侧可以直接拿到"这台设备支持硬解的编码 + 偏好"，也可以直接传进来。
 * 一旦传进来，内核就**不再自己探测**（应用侧可以持久化它，避免每次探测设备）；
 * 传空串（或在 Java/Qt 那边传 null）即清除，恢复内核自行探测。
 *
 * JSON 契约（get 与 set 同一份，字段名固定）：
 *   {"source":"app"|"kernel",
 *    "hwDecode":["H.265","H.264"],
 *    "preference":["AV1","H.265","VP9","H.264","MPEG-4","MPEG-2"],
 *    "preferred":"H.265"}
 *   · source     ：get 时说明这份数据来自应用层（"app"）还是内核探测（"kernel"）；
 *                  set 时忽略（内核写入 "app"）；
 *   · hwDecode   ：设备**能硬解**的编码短名集合，顺序无关；set 时必填；
 *   · preference ：可选，**从高到低**的效率序覆盖；缺省时内核用自己的效率序；
 *   · preferred  ：可选，**手动指定**的"默认视频格式"（应用侧"设置默认视频格式"
 *                  就是它）。取值是那 6 个规范短名之一；缺省 / 空串 / 不在表内
 *                  都表示**不指定**（全自动，沿用硬解优先 + 效率序那套规则）。
 *                  指定之后：**下一次**等级选择（起播默认档、ABR 下一次决策、
 *                  ABR 触发的切档）优先用该编码 —— 目标分辨率下有它的流、且内核解得了
 *                  它（硬解或软解任一可用）就用它，**压过自动的效率序与硬解偏好**；
 *                  两种情况回退到自动规则并打日志：该分辨率下没有该编码的流、
 *                  或内核根本解不了它。设置 preferred **本身不发起任何立即切换**
 *                  （用户要的是"下一次切换清晰度生效"）。始终只在**同一分辨率内**
 *                  换编码，绝不为了它跨分辨率（不为省流降分辨率）。
 *                  语义细节见 framework/codec/decoderFactory.h 与
 *                  mediaPlayer/abr/AbrAlgoStrategy.h。
 * 两个数组都只认这 6 个短名：H.264 / H.265 / AV1 / VP9 / MPEG-4 / MPEG-2；
 * 数组里的未知项忽略。preferred 不是字符串属于结构畸形（整份拒绝）；
 * 是字符串但不在表内不算畸形（等价于不指定）。其它字段忽略。
 * get 返回里 preferred **总是**出现：指定过就是那个短名，没指定就是空串。
 * 应用侧无需新增接口：Qt 的 setVideoCodecSupport(json) / videoCodecSupport() 与
 * Android 的 setVideoCodecSupport(String) / getVideoCodecSupport() 原样可用。
 *
 * 字符串所有权 / 释放方式（本文件此前没有字符串返回约定 —— 返回字符串的接口
 * 都是 C++ 的 std::string，这里为了 C/JNI 侧好接而返回 const char *）：
 *   · CicadaGetVideoCodecSupport() 返回的是 **malloc 出来的、以 '\0' 结尾的副本**，
 *     调用方**必须**用 CicadaFreeString() / CicadaFree() 释放（两者完全等价），
 *     不能对这些指针做 free/delete/重分配；失败返回 NULL；
 *   · 传入的 json 由调用方持有，内核只读不保存这个指针（内核保存的是解析后的副本）。
 */

/*
 * 取当前生效的能力与偏好（JSON，见上）。失败（player 为空 / 内存不足）返回 NULL；
 * 返回的字符串由调用方用 CicadaFreeString() / CicadaFree() 释放。
 */
const char *CicadaGetVideoCodecSupport(playerHandle *player);

/*
 * 传入应用层的能力与偏好（JSON，见上）。
 * NULL 或空串 = 清除，恢复内核自行探测。
 * 返回 0 成功；非 0 失败（-EINVAL = 参数非法 / JSON 不合法 / 结构不符合契约，
 * 此时**当前状态不变**，不会半套用）。
 */
int CicadaSetVideoCodecSupport(playerHandle *player, const char *json);

/*
 * 释放 CicadaGetVideoCodecSupport() 返回的字符串。传 NULL 安全（空操作）；
 * 同一个指针只能释放一次。
 */
void CicadaFreeString(const char *str);

/* CicadaFreeString() 的同义函数（名字更短），两者完全等价。 */
void CicadaFree(const char *str);

/*
 * ==================== 【播放缓存 play-and-cache】====================
 *
 * 这是 C API 句柄这条路（CicadaCreatePlayer → playerHandle*）上的播放缓存入口，
 * 与 C++ 门面 MediaPlayer::SetCacheConfig / GetCachePathByURL 是同一份能力、
 * 同一层语义（内核落点都在 ICicadaPlayer::SetCacheConfig / GetCachePathByURL，
 * 只有 SuperMediaPlayer 实现它）。原来缓存只存在于 C++ 门面，句柄这条路拿不到；
 * 这两个函数把它补上。
 *
 * 配置 JSON 契约（字段名与 cacheModule/cache/CacheConfig.h 的成员一一对应，
 * **不要另造字段名**）：
 *
 *   {"enable":true,"maxDurationS":3600,"maxDirSizeMB":2048,
 *    "cacheDir":"/data/.../cache","cacheFileName":"xxx","sourceSize":0}
 *
 *   · enable        bool  → CacheConfig::mEnable      ；默认 false（缺省=不开）
 *   · maxDurationS  int64 → CacheConfig::mMaxDurationS ；默认 0
 *   · maxDirSizeMB  int64 → CacheConfig::mMaxDirSizeMB ；默认 0
 *   · cacheDir      string→ CacheConfig::mCacheDir     ；默认 ""
 *   · cacheFileName string→ CacheConfig::mCacheFileName；默认 ""
 *   · sourceSize    int64 → CacheConfig::mSourceSize   ；默认 0
 *
 *   缺字段一律取上表默认值；多余字段忽略。**必须在使用同一个 player 调
 *   CicadaSetDataSourceWithUrl() 之前调用** —— 缓存代理 URL 是那一刻算出来的
 *   （见 SuperMediaPlayer::SetDataSource），之后再改对本次播放无效。
 *   json 传 NULL 或空串 = 关闭缓存（等价于所有字段取默认值）。
 *   解析失败（畸形 JSON / 顶层不是对象）**不改变当前配置**，只打一条日志。
 *
 * 缓存事件：启用后内核会通过已注册的 listener 的 EventCallback 上报两条事件
 *   MEDIA_PLAYER_EVENT_CACHE_SUCCESS（code = 11，msg 为 NULL）
 *   MEDIA_PLAYER_EVENT_CACHE_ERROR  （code = 12，msg 为失败原因）
 * 数值见 mediaPlayer/media_player_error_def.h 的 MediaPlayerEventType 枚举顺序。
 */
void CicadaSetCacheConfig(playerHandle *player, const char *json);

/*
 * 取某个**源 URL** 按当前缓存配置对应的缓存文件路径（未命中/未开启返回空串）。
 *
 * 返回的是 malloc 出来的、以 '\0' 结尾的**副本**，调用方必须用
 * CicadaFreeString() / CicadaFree() 释放（与 CicadaGetVideoCodecSupport 同一个约定；
 * 返回空串时也是一个可释放的有效指针，不是 NULL —— 绝不返回 NULL，只有内存不足才可能）。
 * 传 url == NULL 视为空串。这个函数只"算路径"，不看文件在不在（存在与否由内核
 * 在 SetDataSource 时判断：命中就直接播缓存文件）。
 */
const char *CicadaGetCachePath(playerHandle *player, const char *url);

#endif // CICADA_PLAYER_H_
