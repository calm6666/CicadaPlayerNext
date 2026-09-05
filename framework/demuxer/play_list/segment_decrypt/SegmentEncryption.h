//
// Created by moqi on 2018/10/22.
//

#ifndef CICADA_PLAYER_SEGMENTENCRYPTION_H
#define CICADA_PLAYER_SEGMENTENCRYPTION_H

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
