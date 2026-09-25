//
// Created by moqi on 2018/9/21.
//

#ifndef CICADA_PLAYER_PLAYER_TYPES_H
#define CICADA_PLAYER_PLAYER_TYPES_H


#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <demuxer/manifest/MediaManifest.h>
#include "ICicadaPlayer.h"
#include "playerOptions.h"
#include <atomic>

namespace Cicada {

    class player_type_set {
    public:
        player_type_set();

        ~player_type_set();

        void reset();

    public:
        int64_t startBufferDuration = 0;
        int64_t highLevelBufferDuration = 0;
        int64_t maxBufferDuration = 0;
        uint64_t lowMemSize = 0;
        std::string url;
        // Object-based playback: unified manifest (takes precedence over url
        // when set; also indicates manifest mode in Prepare()).
        std::unique_ptr<Manifest::MediaManifest> manifest;
        std::string refer;
        std::string userAgent;
        int timeout_ms{15000};
        int RTMaxDelayTime{0};
        IpResolveType mIpType{IpResolveWhatEver};
        bool bLooping{false};
        bool bDisableAudio{false};
        bool bDisableVideo{false};
        bool bMute = false;
        bool bLowLatency = false;

        std::atomic<void *> mView{nullptr};
        ScaleMode scaleMode = ScaleMode::SM_FIT;
        RotateMode rotateMode = RotateMode::ROTATE_MODE_0;
        MirrorMode mirrorMode = MirrorMode::MIRROR_MODE_NONE;
        /*
         * 【色觉辅助滤镜 / 回退点 C4】3x3 颜色矩阵（行主序）。默认**单位矩阵 = 关闭**：
         * 这样"建渲染器时下发一次启动配置"（SuperMediaPlayer::CreateVideoRender）拿到的
         * 就是"无效果"，不需要额外判断"是否设置过"。
         *
         * 注意：全 0 矩阵在语义上等于"画面变全黑"（clamp(0,0,1)），**不是**"关闭"；
         * 要关闭请传单位矩阵（Java 层 setColorMatrix(null) 也走单位矩阵）。
         */
        float colorMatrix[9] = {1.0f, 0.0f, 0.0f,
                                0.0f, 1.0f, 0.0f,
                                0.0f, 0.0f, 1.0f};
        int64_t mAutoSwitchTime{INT64_MIN};
        atomic<float> mVolume{1.0};
        playerListener mPlayerListener{};
        atomic<float> rate {1.0};
        std::string http_proxy{};
        std::vector<std::string> customHeaders;
        bool clearShowWhenStop = false;
        bool bEnableTunnelRender = true;
        bool bEnableHwVideoDecode = true;
        int64_t AnalyticsID = -1;
        int mDefaultBandWidth = 0;
        playerOptions mOptions;
        uint32_t mVideoBackgroundColor = 0xFF000000;
        bool bEnableVRC = false;
        int maxASeekDelta = 21 * 1000 * 1000;//us

        int maxVideoRecoverSize{};
        bool mFastStart{true};
        uint32_t pixelBufferOutputFormat{};
        string drmMagicKey;
        string sessionId{};
        int netWorkRetryCount{0};
        bool preferAudio{false};
    };
}


#endif //CICADA_PLAYER_PLAYER_TYPES_H
