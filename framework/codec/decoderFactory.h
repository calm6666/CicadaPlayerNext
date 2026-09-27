//
// Created by moqi on 2019-08-20.
//

#ifndef CICADA_PLAYER_DECODERFACTORY_H
#define CICADA_PLAYER_DECODERFACTORY_H

#include <memory>
#include <map>
#include "IDecoder.h"

class decoderFactory {

public:
    static std::unique_ptr<Cicada::IDecoder> create(const Stream_meta & meta, uint64_t flags, int maxSize ,
                                                    const Cicada::DrmInfo *drmInfo);

    /*
     * ============ 设备硬解能力查询（ABR 的"硬解优先"判据）============
     *
     * 语义（**两态收敛**，调用方只需要一个 bool）：
     *   true  = 平台明确回答"这个编码有硬件解码器"，**或者平台查不到**；
     *   false = 平台明确回答"这个编码在本设备上没有硬件解码器"。
     *
     * "查不到就当支持"是刻意的：这条信息只用来**降级**选择（把解不了的编码让开），
     * 平台答不上来的时候宁可保持原有行为（只按效率序排），也不许把一条其实能硬解
     * 的流误判成"解不了"而白白降清晰度。同时 codec == AF_CODEC_ID_NONE（编码未知，
     * 例如只有清单字符串没解析出来）也一律返回 true。
     *
     * **不改变解码器创建路径的容错**：它只回答"理论上有没有"，真正的失败仍然由
     * CreateVideoDecoder 的 hw -> sw 回退链路兜底（见 SuperMediaPlayer.cpp 的
     * "release + init with hw->sw fallback"）。
     *
     * 实现见 decoderFactory.cpp：按平台分派到各平台解码器自己的静态查询
     * （Android 走 MediaCodecList 的硬件编解码器枚举、Apple 走
     * VTIsHardwareDecodeSupported、桌面走 FFmpeg 的 avcodec_get_hw_config），
     * 结果**缓存**在 decoderFactory.cpp 里的静态表（每个编码只探测一次，没有计时器）。
     *
     * 本头文件里没有任何平台宏，L1 的调用方（mediaPlayer/abr）可以直接用。
     */
    static bool isHardwareDecodeSupported(enum AFCodecID codec);

private:

    static std::unique_ptr<Cicada::IDecoder> createBuildIn(const AFCodecID &codec, uint64_t flags,
                                                           const Cicada::DrmInfo *drmInfo);
};


#endif //CICADA_PLAYER_DECODERFACTORY_H
