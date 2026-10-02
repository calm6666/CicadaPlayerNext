//
// Created by moqi on 2018/10/22.
//

#ifndef CICADA_PLAYER_SEGMENTENCRYPTION_H
#define CICADA_PLAYER_SEGMENTENCRYPTION_H

#include <cstdint>
#include <vector>
#include <string>


class SegmentEncryption {
public:
    SegmentEncryption();

    //TODO
    enum encryption_method {
        NONE,
        AES_128,
        AES_SAMPLE,
        AES_PRIVATE,
        /*
         * ============ CENC（ISO/IEC 23001-7 的样本级加密）============
         *
         * 【为什么它必须是**另一个**方法，不能复用 AES_SAMPLE】
         * AES_SAMPLE 是 Apple HLS 的 SAMPLE-AES：整个样本按 CBC 加密，解密器是
         * HLSSampleAesDecrypter，密钥来自 #EXT-X-KEY 的 URI。
         * CENC 完全是另一回事：容器里带 `tenc`/`senc`/`saiz`/`saio`，**每个样本自带
         * 每样本 IV 与 subsample 表**（哪几段明文、哪几段密文），`cenc` 用 AES-CTR、
         * `cbcs` 用 AES-CBC，解密器是 CENCDecrypter，解密的判据是**包上带的 KID**。
         * 两者的密文布局、IV 语义、密钥查表方式全不一样 —— 拿 SAMPLE-AES 的解密器去解
         * CENC 的样本只会把流解坏（而且没有任何日志）。
         *
         * 【行为】method == CENC 时，HLSStream **不建任何解密器**：样本由内层 demuxer 的
         * 数据流原样带出，由 demuxer_service::readPacket 按包上的加密信息就地解
         * （软解），或者由平台 CDM 在解码器里解（硬解）。谁解由 keyFormat / DrmSchemes
         * 的选优结果决定，见 ManifestDemuxer::buildSegmentEncryptions。
         */
        CENC,
    } method;
//    std::vector<uint8_t> key;
    std::string keyUrl;
    std::vector<uint8_t> iv;
    std::string keyFormat;
    bool ivStatic = false;
    // DRM init data (base64 PSSH) and default key id for CENC systems,
    // populated from ContentProtection by the manifest demuxer.
    std::string pssh;
    std::string keyId;
};


#endif //CICADA_PLAYER_SEGMENTENCRYPTION_H
