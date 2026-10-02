//
// Created by lifujun on 2019/11/4.
//

#include "Md5Utils.h"

extern "C" {
#include <libavutil/md5.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

#include <cstring>

namespace Cicada {

    std::string Md5Utils::getMd5(const std::string &source)
    {
        unsigned char outTmp[16] = {0};
        struct AVMD5 *ctx = av_md5_alloc();
        av_md5_init(ctx);
        av_md5_update(ctx, (const uint8_t *) source.c_str(), source.length());
        av_md5_final(ctx, outTmp);
        av_free(ctx);
        char out[33] = {0};
        static const char kHex[] = "0123456789abcdef";
        int p = 0;

        /*
         * 直接填两位十六进制，而不是 `sprintf(out + p, "%02x", i)`：
         * sprintf 在 macOS 15 SDK 上被标记为 deprecated（"use snprintf(3) instead"）。
         * 这里逐个字符写是等价的、且完全不用格式化函数 —— 比换成 snprintf 再算剩余
         * 长度更直接，也不会因为 out 缓冲只有 33 字节而需要再解释一次边界。
         */
        for (unsigned char i : outTmp) {
            out[p++] = kHex[(i >> 4) & 0x0F];
            out[p++] = kHex[i & 0x0F];
        }

        return std::string(out);
    }

//    std::string SHA1Utils::getSha256(const std::string &source)
//    {
//        struct AVSHA *sha = av_sha_alloc();
//        unsigned char outTmp[32] = {0};
//        av_sha_init(sha, 256);
//        av_sha_update(sha, (const uint8_t *) source.c_str(), source.length());
//        av_sha_final(sha, outTmp);
//        av_free(sha);
//        int p = 0;
//        char out[65] = {0};
//        for (unsigned char i : outTmp) {
//            sprintf(out + p, "%02x", i);
//            p += 2;
//        }
//        return std::string(out);
//
//    }


    unsigned char *SHA1Utils::SHA256(const unsigned char *d, size_t n, unsigned char *md)
    {
        struct AVSHA *sha = av_sha_alloc();
        static unsigned char m[32];

        if (md == nullptr) {
            md = m;
        }

        av_sha_init(sha, 256);
        av_sha_update(sha, d, n);
        av_sha_final(sha, md);
        av_free(sha);
        return md;
    }

    void  Md5Utils::MD5(unsigned char *src, int len, unsigned char *dst)
    {
        static unsigned char outTmp[16] = {0};

        if (dst == nullptr) {
            dst = outTmp;
        }

        struct AVMD5 *ctx = av_md5_alloc();

        av_md5_init(ctx);

        av_md5_update(ctx, src, len);

        av_md5_final(ctx, dst);

        av_free(ctx);
    }
}
