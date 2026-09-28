//
// Created by moqi on 2018/9/21.
//

#ifndef CICADA_PLAYER_ICICADAPLAYER_H
#define CICADA_PLAYER_ICICADAPLAYER_H

#include "native_cicada_player_def.h"
#include <cacheModule/cache/CacheConfig.h>
#include <demuxer/manifest/MediaManifest.h>
#include <drm/DrmHandler.h>
#include <render/video/IVideoRender.h>
#include <utils/AFMediaType.h>

namespace Cicada{
    class IDemuxerFactory;
}

namespace Cicada {

    class ICicadaPlayer {
    public:

        ICicadaPlayer() = default;

        virtual ~ICicadaPlayer() = default;

        virtual string getName() = 0;

        /*
        * 初始化;(同步)
        */
        virtual int SetListener(const playerListener &Listener) = 0;


        virtual void SetOnRenderCallBack(onRenderFrame cb, void *userData) = 0;

        virtual void SetAudioRenderingCallBack(onRenderFrame cb, void *userData) = 0;

        virtual void SetVideoRenderingCallBack(videoRenderingFrameCB cb, void *userData) = 0;

        virtual void SetUpdateViewCB(UpdateViewCB cb, void *userData) = 0;

        virtual void SetUrlHashCB(UrlHashCB cb, void *userData) = 0;

        /*
         *设置显示窗口
         */
        virtual void SetView(void *view) = 0;

        virtual void ClearScreen() = 0;

        /*
         *设置播放url
         */
        virtual void SetDataSource(const char *url) = 0;

        /*
         * 对象模式播放（Object-based playback）：
         * 传入统一的 MediaManifest 清单对象（对应 hili-player 的 MediaManifest
         * JSON 结构），无需 m3u8/mpd 文本与清单网络请求。
         * 支持 AES-128 分段加密与 DRM（Widevine/FairPlay/ClearKey）内容保护。
         */
        virtual void SetDataSource(const Manifest::MediaManifest &manifest) = 0;

        /*
         * 对象模式播放的 JSON 形式：传入 MediaManifest 结构的 JSON 文本。
         */
        virtual void SetDataSource(const std::string &jsonManifest) = 0;

        /*
        * 准备播放
        * url  播放url
        * startPos 播放起始位置
        * key 加密key
        * circleCount 加密count循环次数
        */
        virtual void Prepare() = 0;

        /*
        * 开始播放
        */
        virtual void Start() = 0;

        /*
         * 暂停播放
         */
        virtual void Pause() = 0;


        virtual StreamType SwitchStream(int index) = 0;

        /*
         * 跳转到指定位置
         * seekPos 跳转位置
         * bAccurate 是否精准seek
         */
        virtual void SeekTo(int64_t seekPos, bool bAccurate = false) = 0;

        /*
         * 停止播放
         */
        virtual int Stop() = 0;

        /*
         * 获取播放状态
         */
        virtual PlayerStatus GetPlayerStatus() const = 0;

        /*
         * 获取视频长度
         */
        virtual int64_t GetDuration() const = 0;

        /*
         * 获取视频当前播放位置
         */
        virtual int64_t GetPlayingPosition() = 0;

        /*
         * 获取视频当前缓存位置
         */
        virtual int64_t GetBufferPosition() = 0;

        /*
         * 设置静音
         */
        virtual void Mute(bool bMute) = 0;

        virtual bool IsMute() const = 0;

        /*
         * 设置音量
         */
        virtual void SetVolume(float volume) = 0;

        virtual float GetVideoRenderFps() = 0;

        virtual void EnterBackGround(bool back) = 0;

        virtual void SetScaleMode(ScaleMode mode) = 0;

        virtual ScaleMode GetScaleMode() = 0;

        virtual void SetRotateMode(RotateMode mode) = 0;

        virtual RotateMode GetRotateMode() = 0;

        virtual void SetMirrorMode(MirrorMode mode) = 0;

        virtual void SetVideoBackgroundColor(uint32_t color) = 0;

        virtual MirrorMode GetMirrorMode() = 0;

        /*
         * 【色觉辅助滤镜】设置 3x3 颜色矩阵（行主序，9 个 float）。
         *
         * 这是照 SetMirrorMode 那条已验证通道**新增的独立一条通道**，不改任何已有 API。
         * 语义：显示时 rgb = clamp(uColorMatrix * rgb, 0, 1)；单位矩阵 = 关闭（无效果）。
         *
         * 已知限制（不打算解决，写明即可）：
         *  · 隧道/direct 渲染（IVideoRender::FLAG_DUMMY，HDR/Widevine 被内核强制走这条）没有
         *    着色器 ⇒ 滤镜无效；
         *  · GLRender::captureScreen 抓的是"加滤镜之前"的画面 ⇒ 截图不带滤镜
         *    （Qt 的截图链路同源，同样不带）。
         *
         * 【回退点 C1】删掉本函数 + 各实现类里的 override 即可回到加此通道之前（见各文件注释）。
         *
         * @param matrix 9 个 float，行主序；传单位矩阵表示关闭
         */
        virtual void SetColorMatrix(const float matrix[9]) = 0;

        virtual int GetCurrentStreamIndex(StreamType type) = 0;

        virtual StreamInfo *GetCurrentStreamInfo(StreamType type) = 0;

        virtual int64_t GetMasterClockPts() = 0;

        /*
         * 设置网络超时时间
         */
        virtual void SetTimeout(int timeout) = 0;

        /*
         * 设置丢帧时间
         */
        virtual void SetDropBufferThreshold(int dropValue) = 0;


        virtual void SetDecoderType(DecoderType type) = 0;

        virtual DecoderType GetDecoderType() = 0;

        virtual float GetVolume() const = 0;

        /*
         * 设置referer
         */
        virtual void SetRefer(const char *refer) = 0;

        /*
         * 设置user Agent
         */
        virtual void SetUserAgent(const char *userAgent) = 0;

        /*
         * 设置循环播放
         */
        virtual void SetLooping(bool bCirclePlay) = 0;

        /*
         * 获取是否循环播放
         */
        virtual bool isLooping() = 0;

        /*
         * 截屏
         */
        virtual void CaptureScreen() = 0;

        /*
         * 获取视频分辨率
         */
        virtual void GetVideoResolution(int &width, int &height) = 0;

        /*
         * 获取视频旋转角度
         */
        virtual void GetVideoRotation(int &rotation) = 0;

        virtual std::string GetPropertyString(PropertyKey key, const CicadaJSONItem &param)
        {
            return "";
        };

        virtual int64_t GetPropertyInt(PropertyKey key)
        {
            return 0;
        };

        virtual long GetPropertyLong(int key)
        {
            return 0;
        };

        virtual float GetVideoDecodeFps() = 0;

        virtual int SetOption(const char *key, const char *value) = 0;

        virtual void GetOption(const char *key, char *value) = 0;

        virtual void setSpeed(float speed) = 0;

        virtual float getSpeed() = 0;

        virtual void AddCustomHttpHeader(const char *httpHeader) = 0;

        virtual void RemoveAllCustomHttpHeader() = 0;

        virtual void addExtSubtitle(const char *uri) = 0;

        virtual int selectExtSubtitle(int index, bool bSelect) = 0;

        virtual int setStreamDelay(int index, int64_t time) = 0;

        virtual void setMediaFrameCb(playerMediaFrameCb func, void *arg)
        {
            if (mMediaFrameCb == func) {
                return;
            }
            mMediaFrameCbArg = arg;
            mMediaFrameCb = func;
        }

        virtual void setBitStreamCb(readCB read, seekCB seek, void *arg){};

        virtual void setClockRefer(clockRefer cb, void *arg)
        {
            mClockRef = cb;
            mCRArg = arg;
        }

        virtual void setDrmRequestCallback(const std::function<DrmResponseData*(const DrmRequestParam& drmRequestParam)>  &drmCallback) = 0;

        virtual int getCurrentStreamMeta(Stream_meta *meta, StreamType type) = 0;

        virtual void setErrorConverter(ErrorConverter *converter)
        {
            mErrorConverter = converter;
        }

        /*
         * Reload network connection at the break point
         */

        virtual void reLoad() = 0;

        virtual void SetAutoPlay(bool bAutoPlay) = 0;

        virtual bool IsAutoPlay() = 0;

        virtual void SetFilterConfig(const std::string &filterConfig)
        {}

        virtual void UpdateFilterConfig(const std::string &target, const std::string &options)
        {}

        virtual void SetFilterInvalid(const std::string &target, bool invalid)
        {}

        virtual int invokeComponent(std::string content) = 0;

        virtual float getCurrentDownloadSpeed() = 0;

        /*
         * 【ABR 让路 / 切档去重】当前是否有清晰度切换在途。
         *
         * 用途只有一个：**ABR 线程不要在"用户手动切档还没结束"时再发一次切档请求**。
         * 真机症状（用户反馈）：点一次清晰度，界面先弹"已取消切换"再弹结果 —— 因为
         * ABR 那一 tick 的请求把在途的手动切档判成了 CANCELED（内核的"新请求取代旧请求"
         * 语义）。ABR 让这一 tick 过去，下一秒会自己重新评估，**不会丢任何意图**。
         *
         * 纯状态查询、无副作用；默认实现返回 false（不改变任何既有实现的行为）。
         * 追加在 vtable 末尾，保证增量 ABI 安全。
         */
        virtual bool IsStreamSwitchInFlight() const
        {
            return false;
        }

        /*
         * 【应用层视频编码"硬解能力 + 效率偏好"】C 接口
         * CicadaGetVideoCodecSupport / CicadaSetVideoCodecSupport 的 C++ 落点。
         *
         * JSON 契约（get/set 同一份，字段名固定）：
         *   {"source":"app"|"kernel",
         *    "hwDecode":["H.265","H.264"],
         *    "preference":["AV1","H.265","VP9","H.264","MPEG-4","MPEG-2"],
         *    "preferred":"H.265"}
         *   · source     ：get 时说明这份数据来自应用层还是内核探测；set 时忽略；
         *   · hwDecode   ：设备能硬解的编码短名集合（顺序无关），set 时必填；
         *   · preference ：可选，从高到低的效率序覆盖；缺省用内核默认序；
         *   · preferred  ：可选，手动指定的"默认视频格式"（应用侧"设置默认视频格式"）。
         *                  空串 / 不在表内 = 不指定（全自动）。指定后**下一次**等级选择
         *                  （起播默认档、ABR 下一次决策/切档）优先用该编码，压过自动的
         *                  效率序与硬解偏好；该分辨率下没有它的流、或内核根本解不了它
         *                  （硬解与软解都没有）时回退到自动规则并打日志。设置本身不发起
         *                  立即切换，且只在同一分辨率内换编码。
         * 短名只认 H.264 / H.265 / AV1 / VP9 / MPEG-4 / MPEG-2（见 afCodecShortName），
         * 数组里的未知项忽略；preferred 不是字符串 = 结构畸形。畸形 JSON 整份拒绝
         * （set 返回非 0 且不改变当前状态）。
         *
         * set 传空串 = 清除应用层覆盖，恢复内核自己探测；此时 get 返回
         * source = "kernel"。
         *
         * 两个都是**带默认实现**的虚函数，且**追加在 vtable 末尾**（本工程硬规则：
         * 既有实现的槽位编号不变）。默认实现不参与任何状态，所以只有
         * SuperMediaPlayer 需要覆盖它，AppleAVPlayer / JavaExternalPlayer 等
         * 实现类一行都不用改（它们保持默认实现：get 返回空、set 返回 -1）。
         */
        virtual std::string GetVideoCodecSupportJson()
        {
            return {};
        }

        virtual int SetVideoCodecSupportJson(const std::string &json)
        {
            return -1;
        }

        /*
         * 【当前视频解码器实际在用硬解还是软解（事实读数，**不是**能力查询）】
         *
         * 与 GetDecoderType() 的区别是这个方法存在的全部理由：GetDecoderType() 读的是
         * 解码器打开时留下的标志位（"配置/标志"口径），而这里问的是**活动解码器实例**
         * 眼下真正走的那条路 —— 包括
         *   * 建解码器时硬解没起来（构建里没有该编码的硬解配置、GPU 设备建不出来）
         *     而由 CreateVideoDecoder 的 hw → sw 兜底链换成软解的情况；
         *   * 解码途中硬解掉了、这条流退回软解的情况（avcodecDecoder 的 getHwFormat）。
         * 界面（Qt 的"解码方式"那一栏）据此显示，不许读"配置想用什么"。
         *
         * 语义：true = 当前视频解码器在硬解；false = 软解，或者**当前根本没有视频解码器**
         * （没有片源 / 还没建起来 / 已销毁）。调用方拿不到"有没有解码器"时按"没有"处理。
         *
         * 带默认实现的虚函数，且**追加在 vtable 末尾**（本工程硬规则：既有实现的槽位
         * 编号不变）。默认实现返回 false：只有 SuperMediaPlayer 覆盖它，
         * AppleAVPlayer / JavaExternalPlayer 等实现类一行都不用改。
         */
        virtual bool IsVideoDecoderHardware()
        {
            return false;
        }

        /*
         * ==================== 【播放缓存 play-and-cache】C 接口 ====================
         *
         * 这两个是播放缓存的两个落点，对应 C API 的 CicadaSetCacheConfig /
         * CicadaGetCachePath（media_player_api.h），也是鸿蒙 NAPI 层
         * setCacheConfig / getCachePath 唯一的向下出口。
         *
         * 语义：
         *   · SetCacheConfig(config)：设置本实例的缓存配置（字段见 CacheConfig.h：
         *     mEnable / mMaxDurationS / mMaxDirSizeMB / mCacheDir / mCacheFileName /
         *     mSourceSize）。**必须在 SetDataSource 之前调用** —— 缓存代理 URL 是在
         *     SetDataSource 那一刻算出来的，之后改配置对已经开始的这次播放不起作用。
         *     配置与当前值相同（CacheConfig::isSame）时不动任何状态；不同时把已建好的
         *     缓存管理器停掉（下一句注释里那条 stop 理由），再存下新配置。
         *   · GetCachePathByURL(url)：给定一个**源 URL**，返回它按当前配置对应的
         *     缓存文件路径；未开启缓存 / 没有 cacheDir / URL 为空时返回空串。
         *     它只是"算路径"，不看文件是否存在（存在性判断在内核的 getCachedFilePath 里）。
         *
         * 两个都是**带默认实现**的虚函数，且**追加在 vtable 末尾**（本工程硬规则：
         * 既有实现的槽位编号不变）。默认实现不参与任何状态，所以只有
         * SuperMediaPlayer 需要覆盖它们，AppleAVPlayer / JavaExternalPlayer /
         * MediaPlayer 等实现类一行都不用改（它们保持默认实现：set 无操作、get 返回空串）。
         */
        virtual void SetCacheConfig(const CacheConfig &config)
        {}

        virtual std::string GetCachePathByURL(const std::string &url)
        {
            return "";
        }


    protected:
        playerMediaFrameCb mMediaFrameCb = nullptr;
        void *mMediaFrameCbArg = nullptr;

        clockRefer mClockRef = nullptr;
        void* mCRArg = nullptr;

        ErrorConverter *mErrorConverter = nullptr;
    };

}


#endif //CICADA_PLAYER_ICICADAPLAYER_H
