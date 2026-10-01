//
// cencDecrypterTest.cpp
//
// CENCDecrypter 的单元测试。
//
// 测试思路（按重要性排序）：
//   1) 先用 NIST SP 800-38A 的公开已知答案向量证明「内核 AES 原语 + 本类构造的
//      counter block / CBC 链」与标准一致，而不是拿被测实现自证；
//   2) 再用测试文件内独立写的 CTR / CBC / CBC-CS3 参考实现给多 subsample 样本造密文，
//      让 CENCDecrypter 解密，覆盖 subsample 展开、per-subsample IV/counter 重置、
//      cbcs pattern、尾部 CTS；
//   3) 失败路径必须返回负值且不改动 buffer（缺 key / 非法 subsample / ivSize 非法等）。
//
// 明确不证明什么（详见 docs/CENC-DECRYPTION.md）：不证明与 demuxer/播放链路的集成，
// 不证明真实 mp4 端到端可播，也不覆盖 cens。
//

#define LOG_TAG "cencDecrypterTest"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include <demuxer/sample_decrypt/CENCDecrypter.h>
#include <demuxer/decrypto/avAESDecrypt.h>
#include <utils/frame_work_log.h>

using namespace Cicada;

namespace {
    const char *const kKeyHex = "2b7e151628aed2a6abf7158809cf4f3c";
    const char *const kKeyIdHex = "1077efecc0b24d02ace33c1e52e2fb4b";

    /* -------------------------------------------------------------- */
    /* 测试用的小工具                                                   */
    /* -------------------------------------------------------------- */

    // "2b7e1516..." / "2B7E-1516 ..." 都能解析；分隔符忽略，大小写不敏感。
    std::vector<uint8_t> fromHex(const std::string &hex)
    {
        std::vector<uint8_t> out;
        int hi = -1;

        for (char c : hex) {
            int v = -1;

            if (c >= '0' && c <= '9') {
                v = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                v = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                v = c - 'A' + 10;
            } else {
                continue;   // 分隔符
            }

            if (hi < 0) {
                hi = v;
            } else {
                out.push_back(static_cast<uint8_t>((hi << 4) | v));
                hi = -1;
            }
        }

        EXPECT_LT(hi, 0) << "fromHex: odd number of hex digits in '" << hex << "'";
        return out;
    }

    std::string toHexLower(const std::vector<uint8_t> &data)
    {
        static const char *digits = "0123456789abcdef";
        std::string out;
        out.reserve(data.size() * 2);

        for (uint8_t b : data) {
            out.push_back(digits[b >> 4]);
            out.push_back(digits[b & 0x0F]);
        }

        return out;
    }

    std::vector<uint8_t> makeDeterministicBytes(size_t size, uint32_t seed)
    {
        std::vector<uint8_t> out(size, 0);

        for (size_t i = 0; i < size; ++i) {
            out[i] = static_cast<uint8_t>((i * 37 + seed) & 0xFF);
        }

        return out;
    }

    /* -------------------------------------------------------------- */
    /* 独立参考实现（只依赖 avAESEncrypt/avAESDecrypt 的单分组 ECB 通路）   */
    /* -------------------------------------------------------------- */

    // CTR：counter block = iv[iySize) || 0...0，后 8 字节是大端计数器，从 0 起、每 16 字节 +1。
    // 每个 protected 区间（subsample）重置 counter，IV 本身不变。
    std::vector<uint8_t> referenceCtrCrypt(const std::vector<uint8_t> &key, const std::vector<uint8_t> &iv,
                                           const std::vector<uint8_t> &input,
                                           const std::vector<SubsampleInfo> &subsamples)
    {
        avAESEncrypt aes;
        EXPECT_EQ(aes.setKey(key.data(), 8 * 16), 0);

        std::vector<uint8_t> out = input;

        std::vector<std::pair<size_t, size_t>> ranges;
        size_t offset = 0;

        for (const SubsampleInfo &sub : subsamples) {
            offset += sub.clearBytes;

            if (sub.protectedBytes > 0) {
                ranges.emplace_back(offset, sub.protectedBytes);
            }

            offset += sub.protectedBytes;
        }

        EXPECT_EQ(offset, out.size()) << "referenceCtrCrypt: subsample layout does not cover the buffer";

        for (const auto &range : ranges) {
            uint8_t counter[16] = {0};
            memcpy(counter, iv.data(), iv.size());

            size_t done = 0;

            while (done < range.second) {
                uint8_t ks[16] = {0};
                aes.encrypt(ks, counter, 1, nullptr);

                const size_t n = std::min<size_t>(16, range.second - done);

                for (size_t i = 0; i < n; ++i) {
                    out[range.first + done + i] = static_cast<uint8_t>(out[range.first + done + i] ^ ks[i]);
                }

                for (int i = 15; i >= 8; --i) {
                    counter[i] = static_cast<uint8_t>(counter[i] + 1);

                    if (counter[i] != 0) {
                        break;
                    }
                }

                done += n;
            }
        }

        return out;
    }

    // 标准 CBC 加密（整块，长度必须是 16 的倍数）；F.2.1 向量已证明它与 NIST 一致
    std::vector<uint8_t> referenceCbcEncrypt(const std::vector<uint8_t> &key, const std::vector<uint8_t> &iv16,
                                             const uint8_t *plain, size_t length)
    {
        avAESEncrypt aes;
        EXPECT_EQ(aes.setKey(key.data(), 8 * 16), 0);
        EXPECT_EQ(length % 16, 0u) << "referenceCbcEncrypt 只处理整块输入";

        std::vector<uint8_t> out(length, 0);
        std::vector<uint8_t> chain = iv16;

        for (size_t off = 0; off + 16 <= length; off += 16) {
            std::vector<uint8_t> block(16, 0);

            for (size_t i = 0; i < 16; ++i) {
                block[i] = static_cast<uint8_t>(plain[off + i] ^ chain[i]);
            }

            aes.encrypt(&out[off], block.data(), 1, nullptr);
            memcpy(chain.data(), &out[off], 16);
        }

        return out;
    }

    // 对「一段连续 protected 数据」做 CBC 解密，尾部不满 16 字节时按 CBC-CS3 处理 CTS。
    // chain 进/出都是这段的链值（出去时是最后一段密文）。返回解密后的字节。
    std::vector<uint8_t> referenceCbcCtsDecrypt(const std::vector<uint8_t> &key,
                                                const std::vector<uint8_t> &iv16,
                                                const uint8_t *cipher, size_t length)
    {
        avAESDecrypt aes;
        EXPECT_EQ(aes.setKey(key.data(), 8 * 16), 0);

        std::vector<uint8_t> out(cipher, cipher + length);
        std::vector<uint8_t> chain = iv16;
        size_t off = 0;

        for (; length - off >= 16; off += 16) {
            uint8_t block[16] = {0};
            aes.decrypt(block, cipher + off, 1, nullptr);

            for (size_t i = 0; i < 16; ++i) {
                out[off + i] = static_cast<uint8_t>(block[i] ^ chain[i]);
            }

            memcpy(chain.data(), cipher + off, 16);
        }

        const size_t tail = length - off;

        if (tail == 0) {
            return out;
        }

        EXPECT_GE(off, 16u) << "CTS 需要至少一个前置整块";

        uint8_t *cPrev = &out[off - 16];
        const uint8_t *cTail = cipher + off;

        uint8_t x[16] = {0};
        aes.decrypt(x, cTail, 1, nullptr);

        for (size_t i = 0; i < 16; ++i) {
            x[i] = static_cast<uint8_t>(x[i] ^ cPrev[i]);
        }

        std::vector<uint8_t> pPrev(x, x + 16);
        memcpy(cPrev, x, tail);

        uint8_t z[16] = {0};
        aes.decrypt(z, x, 1, nullptr);

        for (size_t i = 0; i < tail; ++i) {
            out[off + i] = static_cast<uint8_t>(z[i] ^ pPrev[i]);
        }

        return out;
    }

    // CBC-CS3 加密：只给 cbc1/cbcs 造测试输入，不是被测对象。
    // chain 进来是 CBC 链值（IV 或上一个 run 的最后一段密文）。输出长度 == length。
    std::vector<uint8_t> referenceCbcCtsEncrypt(const std::vector<uint8_t> &key, std::vector<uint8_t> &chain,
                                                const uint8_t *plain, size_t length)
    {
        avAESEncrypt aes;
        EXPECT_EQ(aes.setKey(key.data(), 8 * 16), 0);

        std::vector<uint8_t> out(length, 0);
        size_t off = 0;

        while (length - off >= 16) {
            const size_t remain = length - off;

            if (remain == 16) {
                std::vector<uint8_t> block(16, 0);

                for (size_t i = 0; i < 16; ++i) {
                    block[i] = static_cast<uint8_t>(plain[off + i] ^ chain[i]);
                }

                aes.encrypt(&out[off], block.data(), 1, nullptr);
                memcpy(chain.data(), &out[off], 16);
                return out;
            }

            if (remain < 32) {
                // 末两段：P(n-1) 整块 + P(n) 的 r 字节，按 CBC-CS3 处理
                const size_t r = remain - 16;
                std::vector<uint8_t> buf0(16, 0);
                std::vector<uint8_t> buf1(16, 0);

                for (size_t i = 0; i < 16; ++i) {
                    buf0[i] = static_cast<uint8_t>(plain[off + i] ^ chain[i]);
                }

                uint8_t mid[16] = {0};
                aes.encrypt(mid, buf0.data(), 1, nullptr);
                memcpy(buf1.data(), mid, 16);
                memcpy(buf1.data(), plain + off + 16, r);

                uint8_t cTail[16] = {0};
                aes.encrypt(cTail, buf1.data(), 1, nullptr);

                memcpy(&out[off], mid, 16 - r);            // C(n) 的 16-r 字节
                memcpy(&out[off + (16 - r)], cTail, r);    // C(n-1) 的末 r 字节

                // 链值取 CS3 之后的最后一段密文 = 前 16-r 字节的 C(n) || 后 r 字节的 C(n-1)
                std::vector<uint8_t> last(out.begin() + static_cast<std::ptrdiff_t>(off),
                                          out.begin() + static_cast<std::ptrdiff_t>(off + 16));
                memcpy(chain.data(), last.data(), 16);
                return out;
            }

            std::vector<uint8_t> block(16, 0);

            for (size_t i = 0; i < 16; ++i) {
                block[i] = static_cast<uint8_t>(plain[off + i] ^ chain[i]);
            }

            aes.encrypt(&out[off], block.data(), 1, nullptr);
            memcpy(chain.data(), &out[off], 16);
            off += 16;
        }

        return out;
    }

    // 把「一个 subsample 的 protected 区间」按 cbcs pattern 组装成密文。
    // 链值在区间起点重置为 IV，区间内跨 run 连续；每个 run 固定 cryptBytes 长
    // （尾部残余 run 更短，走 CTS）。
    std::vector<uint8_t> referenceCbcsEncryptRegion(const std::vector<uint8_t> &key,
                                                    const std::vector<uint8_t> &iv16,
                                                    const uint8_t *plain, size_t protectedLength,
                                                    uint32_t cryptBlock, uint32_t skipBlock)
    {
        const size_t cryptBytes = static_cast<size_t>(cryptBlock) * 16;
        const size_t skipBytes = static_cast<size_t>(skipBlock) * 16;

        std::vector<uint8_t> out(plain, plain + protectedLength);
        std::vector<uint8_t> chain = iv16;
        size_t done = 0;

        while (done < protectedLength) {
            const size_t run = std::min(cryptBytes, protectedLength - done);
            std::vector<uint8_t> enc = referenceCbcCtsEncrypt(key, chain, plain + done, run);
            memcpy(&out[done], enc.data(), enc.size());

            done += run;
            done += std::min(skipBytes, protectedLength - done);
        }

        return out;
    }
} // namespace

/* ================================================================== */
/* 测试 0：证明 avAESEncrypt 的 iv == nullptr 通路确实是 ECB 单分组加密   */
/* ================================================================== */

// 这是后面所有「独立参考实现」的地基：av_aes_crypt 只有在 iv == nullptr 时才是无链接的
// 单分组 ECB（见 external/external/ffmpeg/libavutil/aes.c 的 aes_encrypt/aes_decrypt）。
// 前提不成立的话参考实现全是错的，所以先钉死它。
TEST(cencAesPrimitive, ecbPathIsUnchained)
{
    avAESEncrypt aes;
    ASSERT_EQ(aes.setKey(fromHex(kKeyHex).data(), 8 * 16), 0);

    std::vector<uint8_t> block0 = fromHex("6bc1bee22e409f96e93d7e117393172a");
    std::vector<uint8_t> block1 = fromHex("ae2d8a571e03ac9c9eb76fac45af8e51");

    uint8_t c0[16] = {0};
    uint8_t c0Again[16] = {0};
    uint8_t c1[16] = {0};

    aes.encrypt(c0, block0.data(), 1, nullptr);
    aes.encrypt(c0Again, block0.data(), 1, nullptr);
    aes.encrypt(c1, block1.data(), 1, nullptr);

    // 同一明文块两次得到同一密文 => 没有链接
    EXPECT_EQ(toHexLower(std::vector<uint8_t>(c0, c0 + 16)),
              toHexLower(std::vector<uint8_t>(c0Again, c0Again + 16)));
    // 不同明文块密文不同
    EXPECT_NE(toHexLower(std::vector<uint8_t>(c0, c0 + 16)),
              toHexLower(std::vector<uint8_t>(c1, c1 + 16)));
}

/* ================================================================== */
/* 测试 A：NIST SP 800-38A F.2.1 CBC-AES128 已知答案向量                 */
/* ================================================================== */

// F.2.1 的四个密文块同时钉死两件事：
//   (a) 内核 AES 是标准 AES-128；
//   (b) 「ECB 原语 + 手工 XOR 链」搭出的 CBC 与标准一致（cbc1/cbcs 的参考实现靠它）。
// 顺带证明 avAESDecrypt 的 iv == nullptr（ECB 解密）通路也是对的。
TEST(cencAesPrimitive, nistSp80038aCbcAes128Vectors)
{
    const std::string key = "2b7e151628aed2a6abf7158809cf4f3c";
    const std::string iv = "000102030405060708090a0b0c0d0e0f";
    const std::string plain = "6bc1bee22e409f96e93d7e117393172a"
                              "ae2d8a571e03ac9c9eb76fac45af8e51"
                              "30c81c46a35ce411e5fbc1191a0a52ef"
                              "f69f2445df4f9b17ad2b417be66c3710";
    const std::string cipher = "7649abac8119b246cee98e9b12e9197d"
                               "5086cb9b507219ee95db113a917678b2"
                               "73bed6b8e3c1743b7116e69e22229516"
                               "3ff1caa1681fac09120eca307586e1a7";

    std::vector<uint8_t> keyBytes = fromHex(key);
    std::vector<uint8_t> ivBytes = fromHex(iv);
    std::vector<uint8_t> plainBytes = fromHex(plain);
    std::vector<uint8_t> cipherBytes = fromHex(cipher);

    // 加密方向
    std::vector<uint8_t> got = referenceCbcEncrypt(keyBytes, ivBytes, plainBytes.data(), plainBytes.size());
    EXPECT_EQ(toHexLower(got), toHexLower(cipherBytes));

    // 解密方向
    std::vector<uint8_t> back = referenceCbcCtsDecrypt(keyBytes, ivBytes, cipherBytes.data(), cipherBytes.size());
    EXPECT_EQ(toHexLower(back), toHexLower(plainBytes));
}

/* ================================================================== */
/* 测试 B：NIST SP 800-38A F.5.1/F.5.2 CTR-AES128，走 CENCDecrypter     */
/* ================================================================== */

// 期望的 keystream（由 F.5.1/F.5.2 的明文与密文逐字节异或得到；固定 counter block 的 CTR
// 有这个性质：keystream_i = plaintext_i XOR ciphertext_i）：
//   E(K, f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff) = ec0f8a4bc34b9a2a2c3a29b294822eaf
//   E(K, f0f1f2f3f4f5f6f7f8f9fafbfcfdff00) = 3603b7ca778d09d6a2a0f6136db1d0d2
//   E(K, f0f1f2f3f4f5f6f7f8f9fafbfcfdff01) = 6a3b8bd6b0b1b1ecbeaa89d6b41ae5f4
//   E(K, f0f1f2f3f4f5f6f7f8f9fafbfcfdff02) = e857601d0cbc7dbdb4b16f0e6b3e2ed0
//
// 用 iv = f0f1f2f3f4f5f6f7（8 字节）驱动 CENCDecrypter：如果 counter block 不是
// 「前 8 字节 IV + 后 8 字节大端计数器从 0 起、每块 +1」，下面这些断言会全部失败。
TEST(cencDecrypter, nistSp80038aCtrVectorsThroughDecrypter)
{
    const std::string plainHex = "6bc1bee22e409f96e93d7e117393172a"
                                 "ae2d8a571e03ac9c9eb76fac45af8e51"
                                 "30c81c46a35ce411e5fbc1191a0a52ef"
                                 "f69f2445df4f9b17ad2b417be66c3710";
    const std::string cipherHex = "874d6191b620e3261bef6864990db6ce"
                                  "9806f66b7970fdff8617187bb9fffdff"
                                  "5ae4df3edbd5d35e5b4f09020db03eab"
                                  "1e031dda2fbe03d1792170a0f3009cee";

    const std::vector<uint8_t> iv8 = fromHex("f0f1f2f3f4f5f6f7");
    const std::vector<uint8_t> iv16 = fromHex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");

    CENCDecrypter dec;
    ASSERT_EQ(dec.setKey(kKeyIdHex, fromHex(kKeyHex).data(), 16), 0);

    // 1) 四个 F.5.1 向量各自单独成一个样本：任何一块错都能定位到具体 counter
    for (int i = 0; i < 4; ++i) {
        std::vector<uint8_t> buffer = fromHex(cipherHex.substr(static_cast<size_t>(i) * 32, 32));
        const std::vector<uint8_t> expected = fromHex(plainHex.substr(static_cast<size_t>(i) * 32, 32));

        const int64_t ret = dec.decrypt(kKeyIdHex, "cenc", iv8.data(), static_cast<uint32_t>(iv8.size()),
                                        0, 0, buffer.data(), static_cast<int64_t>(buffer.size()));

        ASSERT_EQ(ret, static_cast<int64_t>(buffer.size())) << "block " << i;
        EXPECT_EQ(toHexLower(buffer), toHexLower(expected)) << "block " << i;
    }

    // 2) 四块拼成 64 字节样本一次解密：证明 counter 按 16 字节分组递增
    std::vector<uint8_t> whole = fromHex(cipherHex);
    const std::vector<uint8_t> expected = fromHex(plainHex);

    const int64_t ret = dec.decrypt(kKeyIdHex, "cenc", iv8.data(), static_cast<uint32_t>(iv8.size()),
                                    0, 0, whole.data(), static_cast<int64_t>(whole.size()));
    ASSERT_EQ(ret, 64);
    EXPECT_EQ(toHexLower(whole), toHexLower(expected));

    // 3) 传完整 16 字节 counter block 作为 IV，必须得到同一组 keystream
    std::vector<uint8_t> whole16 = fromHex(cipherHex);
    const int64_t ret16 = dec.decrypt(kKeyIdHex, "cenc", iv16.data(), static_cast<uint32_t>(iv16.size()),
                                      0, 0, whole16.data(), static_cast<int64_t>(whole16.size()));
    ASSERT_EQ(ret16, 64);
    EXPECT_EQ(toHexLower(whole16), toHexLower(expected));
}

/* ================================================================== */
/* 测试 C：多 subsample + per-subsample counter 重置                     */
/* ================================================================== */

TEST(cencDecrypter, multiSubsampleCtrRecoversPlaintext)
{
    CENCDecrypter dec;
    ASSERT_EQ(dec.setKey(kKeyIdHex, fromHex(kKeyHex).data(), 16), 0);

    // subsample 长度故意混合：有一个 > 16 字节的（走多块 counter），其余有不满 16 字节的
    // 尾部残余，还有一个 clear-only 的空 subsample。
    const std::vector<SubsampleInfo> subsamples = {
            {5, 10},    // 5 明文 + 10 保护（< 16）
            {0, 40},    // 0 明文 + 40 保护（2.5 块：多块 counter + 残余）
            {17, 1},    // 17 明文 + 1 保护（最小保护长度）
            {31, 33},   // 31 明文 + 33 保护（2 块 + 1 字节残余）
            {0, 0},     // 空 subsample 必须被容忍
    };

    size_t total = 0;

    for (const SubsampleInfo &sub : subsamples) {
        total += sub.clearBytes + sub.protectedBytes;
    }

    ASSERT_EQ(total, 138u);

    const std::vector<uint8_t> plain = makeDeterministicBytes(total, 11);
    const std::vector<uint8_t> iv = fromHex("0f1e2d3c4b5a6978");

    // CTR 是自逆的，参考实现与 decrypter 用的是同一套 counter 规则，因此参考实现直接
    // 当「加密」用，再看 decrypter 能否还原出 plain。
    std::vector<uint8_t> cipher = referenceCtrCrypt(fromHex(kKeyHex), iv, plain, subsamples);
    ASSERT_EQ(cipher.size(), plain.size());
    EXPECT_NE(toHexLower(cipher), toHexLower(plain)) << "参考实现没有改变数据，测试无意义";

    std::vector<uint8_t> buffer = cipher;
    const int64_t ret = dec.decrypt(kKeyIdHex, "cenc", iv.data(), static_cast<uint32_t>(iv.size()),
                                    subsamples, 0, 0, buffer.data(), static_cast<int64_t>(buffer.size()));

    ASSERT_EQ(ret, static_cast<int64_t>(buffer.size()));
    EXPECT_EQ(toHexLower(buffer), toHexLower(plain));

    // clear 字节一个都不能被动过
    size_t off = 0;

    for (const SubsampleInfo &sub : subsamples) {
        for (uint32_t i = 0; i < sub.clearBytes; ++i) {
            EXPECT_EQ(buffer[off + i], plain[off + i]) << "clear byte at " << (off + i) << " was modified";
        }

        off += sub.clearBytes + sub.protectedBytes;
    }
}

/* ================================================================== */
/* 测试 D：失败路径（缺 key / 非法 subsample / ivSize 非法）              */
/* ================================================================== */

TEST(cencDecrypter, failuresReturnNegativeAndLeaveBufferUntouched)
{
    const std::vector<uint8_t> iv8 = fromHex("0f1e2d3c4b5a6978");
    CENCDecrypter dec;
    ASSERT_EQ(dec.setKey(kKeyIdHex, fromHex(kKeyHex).data(), 16), 0);

    // 清掉 key（同时重置一次性日志标志），后面 11 种失败路径都不该改动 buffer
    dec.clearKeys();

    const std::vector<uint8_t> zeros(64, 0);

    // 1) 未知 key id
    {
        std::vector<uint8_t> buffer = zeros;
        const int64_t ret = dec.decrypt("00000000000000000000000000000000", "cenc",
                                        iv8.data(), static_cast<uint32_t>(iv8.size()), 0, 0,
                                        buffer.data(), static_cast<int64_t>(buffer.size()));
        EXPECT_LT(ret, 0);
        EXPECT_EQ(toHexLower(buffer), toHexLower(zeros)) << "buffer must not be touched without a key";
        EXPECT_FALSE(dec.hasKey("00000000000000000000000000000000"));
    }

    // 2) key id 非法（奇数个十六进制字符）
    {
        std::vector<uint8_t> buffer = zeros;
        const int64_t ret = dec.decrypt("abc", "cenc", iv8.data(), 8, 0, 0,
                                        buffer.data(), static_cast<int64_t>(buffer.size()));
        EXPECT_LT(ret, 0);
        EXPECT_EQ(toHexLower(buffer), toHexLower(zeros));
    }

    // 3) 未知 scheme
    {
        std::vector<uint8_t> buffer = zeros;
        const int64_t ret = dec.decrypt(kKeyIdHex, "cenc2", iv8.data(), 8, 0, 0,
                                        buffer.data(), static_cast<int64_t>(buffer.size()));
        EXPECT_LT(ret, 0);
        EXPECT_EQ(toHexLower(buffer), toHexLower(zeros));
    }

    // 4) ivSize == 0
    {
        std::vector<uint8_t> buffer = zeros;
        const int64_t ret = dec.decrypt(kKeyIdHex, "cenc", iv8.data(), 0, 0, 0,
                                        buffer.data(), static_cast<int64_t>(buffer.size()));
        EXPECT_LT(ret, 0);
        EXPECT_EQ(toHexLower(buffer), toHexLower(zeros));
    }

    // 5) ivSize > 16
    {
        std::vector<uint8_t> buffer = zeros;
        const std::vector<uint8_t> iv17(17, 0xAB);
        const int64_t ret = dec.decrypt(kKeyIdHex, "cenc", iv17.data(), 17, 0, 0,
                                        buffer.data(), static_cast<int64_t>(buffer.size()));
        EXPECT_LT(ret, 0);
        EXPECT_EQ(toHexLower(buffer), toHexLower(zeros));
    }

    // 6) iv 为空
    {
        std::vector<uint8_t> buffer = zeros;
        const int64_t ret = dec.decrypt(kKeyIdHex, "cenc", nullptr, 8, 0, 0,
                                        buffer.data(), static_cast<int64_t>(buffer.size()));
        EXPECT_LT(ret, 0);
        EXPECT_EQ(toHexLower(buffer), toHexLower(zeros));
    }

    // 7) subsample 长度之和超过样本长度
    {
        std::vector<uint8_t> buffer = zeros;
        const std::vector<SubsampleInfo> bad = {{4, 8}, {0, 100}};
        const int64_t ret = dec.decrypt(kKeyIdHex, "cenc", iv8.data(), 8, bad, 0, 0,
                                        buffer.data(), static_cast<int64_t>(buffer.size()));
        EXPECT_LT(ret, 0);
        EXPECT_EQ(toHexLower(buffer), toHexLower(zeros));
    }

    // 8) 单个 subsample 的 protected 部分就超过样本长度（且两个字段都接近 UINT32_MAX，
    //    用来验证校验里没有先加后比导致的溢出）
    {
        std::vector<uint8_t> buffer = zeros;
        const std::vector<SubsampleInfo> bad = {{0xFFFFFFFFu, 0xFFFFFFFFu}};
        const int64_t ret = dec.decrypt(kKeyIdHex, "cenc", iv8.data(), 8, bad, 0, 0,
                                        buffer.data(), static_cast<int64_t>(buffer.size()));
        EXPECT_LT(ret, 0);
        EXPECT_EQ(toHexLower(buffer), toHexLower(zeros));
    }

    // 9) 负数 size
    {
        std::vector<uint8_t> buffer = zeros;
        const int64_t ret = dec.decrypt(kKeyIdHex, "cenc", iv8.data(), 8, 0, 0, buffer.data(), -1);
        EXPECT_LT(ret, 0);
    }

    // 10) 空 buffer
    {
        const int64_t ret = dec.decrypt(kKeyIdHex, "cenc", iv8.data(), 8, 0, 0, nullptr, 16);
        EXPECT_LT(ret, 0);
    }

    // 11) 没有注册 key 时，合法输入也必须失败且不改 buffer
    {
        std::vector<uint8_t> buffer = zeros;
        const int64_t ret = dec.decrypt(kKeyIdHex, "cenc", iv8.data(), 8, 0, 0,
                                        buffer.data(), static_cast<int64_t>(buffer.size()));
        EXPECT_LT(ret, 0);
        EXPECT_EQ(toHexLower(buffer), toHexLower(zeros));
    }
}

/* ================================================================== */
/* 测试 E：key 管理（归一化、替换、清空）                                 */
/* ================================================================== */

TEST(cencDecrypter, keyIdNormalizationAndReplacement)
{
    CENCDecrypter dec;
    std::vector<uint8_t> key = fromHex(kKeyHex);

    EXPECT_FALSE(dec.hasKey(kKeyIdHex));
    ASSERT_EQ(dec.setKey("1077EFEC-C0B2-4D02-ACE3-3C1E52E2FB4B", key.data(), 16), 0);
    EXPECT_TRUE(dec.hasKey("1077efecc0b24d02ace33c1e52e2fb4b"));
    EXPECT_TRUE(dec.hasKey("1077EFECC0B24D02ACE33C1E52E2FB4B"));
    EXPECT_TRUE(dec.hasKey("1077efec c0b2 4d02 ace3 3c1e52e2fb4b"));
    EXPECT_FALSE(dec.hasKey("1077efecc0b24d02ace33c1e52e2fb4c"));

    // 非法参数
    EXPECT_LT(dec.setKey(kKeyIdHex, nullptr, 16), 0);
    EXPECT_LT(dec.setKey(kKeyIdHex, key.data(), 15), 0);
    EXPECT_LT(dec.setKey("xyz", key.data(), 16), 0);
    EXPECT_LT(dec.setKey("", key.data(), 16), 0);
    EXPECT_LT(dec.setKey("abc", key.data(), 16), 0);

    // 重复注册 = 替换：换成另一个 key 后，只有用新 key 加密的数据才能被还原
    const std::vector<uint8_t> key2 = fromHex("000102030405060708090a0b0c0d0e0f");
    ASSERT_EQ(dec.setKey(kKeyIdHex, key2.data(), 16), 0);

    const std::vector<uint8_t> iv = fromHex("0011223344556677");
    const std::vector<uint8_t> plain = makeDeterministicBytes(32, 1);
    const std::vector<SubsampleInfo> whole = {{0, 32}};

    std::vector<uint8_t> cipher = referenceCtrCrypt(key2, iv, plain, whole);
    const int64_t ret = dec.decrypt(kKeyIdHex, "cenc", iv.data(), static_cast<uint32_t>(iv.size()),
                                    whole, 0, 0, cipher.data(), static_cast<int64_t>(cipher.size()));
    ASSERT_EQ(ret, 32);
    EXPECT_EQ(toHexLower(cipher), toHexLower(plain));

    dec.clearKeys();
    EXPECT_FALSE(dec.hasKey(kKeyIdHex));
}

/* ================================================================== */
/* 测试 F：cbc1（无 pattern 的 CBC），含不满块的 CTS 尾部                  */
/* ================================================================== */

TEST(cencDecrypter, cbc1RecoversPlaintextIncludingCtsTail)
{
    const std::vector<uint8_t> key = fromHex("000102030405060708090a0b0c0d0e0f");
    const std::vector<uint8_t> iv16 = fromHex("0f0e0d0c0b0a09080706050403020100");
    const std::string keyId = "aabbccddeeff00112233445566778899";

    CENCDecrypter dec;
    ASSERT_EQ(dec.setKey(keyId, key.data(), 16), 0);

    // 41 字节 = 2 整块 + 9 字节残余 => 必然走 CTS
    const std::vector<uint8_t> plain = makeDeterministicBytes(41, 7);
    std::vector<uint8_t> chain = iv16;
    std::vector<uint8_t> cipher = referenceCbcCtsEncrypt(key, chain, plain.data(), plain.size());
    EXPECT_NE(toHexLower(cipher), toHexLower(plain));

    std::vector<uint8_t> buffer = cipher;
    const int64_t ret = dec.decrypt(keyId, "cbc1", iv16.data(), static_cast<uint32_t>(iv16.size()),
                                    0, 0, buffer.data(), static_cast<int64_t>(buffer.size()));
    ASSERT_EQ(ret, static_cast<int64_t>(buffer.size()));
    EXPECT_EQ(toHexLower(buffer), toHexLower(plain));

    // 参考实现对同一段密文的自解密也应还原（交叉验证参考实现自身）
    std::vector<uint8_t> refBack = referenceCbcCtsDecrypt(key, iv16, cipher.data(), cipher.size());
    EXPECT_EQ(toHexLower(refBack), toHexLower(plain));

    // 32 字节整块样本：不经过 CTS，只有普通 CBC 链
    const std::vector<uint8_t> plain32 = makeDeterministicBytes(32, 7);
    std::vector<uint8_t> cipher32 = referenceCbcEncrypt(key, iv16, plain32.data(), plain32.size());
    std::vector<uint8_t> buffer32 = cipher32;
    const int64_t ret32 = dec.decrypt(keyId, "cbc1", iv16.data(), static_cast<uint32_t>(iv16.size()),
                                      0, 0, buffer32.data(), static_cast<int64_t>(buffer32.size()));
    ASSERT_EQ(ret32, 32);
    EXPECT_EQ(toHexLower(buffer32), toHexLower(plain32));

    // 无 subsample 信息时整个样本是一个 protected 区间；41 字节会走 CTS
    std::vector<uint8_t> bufferWhole = cipher;
    const int64_t retWhole = dec.decrypt(keyId, "cbc1", iv16.data(), static_cast<uint32_t>(iv16.size()),
                                         0, 0, bufferWhole.data(), static_cast<int64_t>(bufferWhole.size()));
    ASSERT_EQ(retWhole, static_cast<int64_t>(bufferWhole.size()));
    EXPECT_EQ(toHexLower(bufferWhole), toHexLower(plain));
}

/* ================================================================== */
/* 测试 G：cbcs pattern + per-subsample IV 重置 + 尾部 CTS               */
/* ================================================================== */

TEST(cencDecrypter, cbcsPatternRecoversPlaintext)
{
    const std::vector<uint8_t> key = fromHex("00112233445566778899aabbccddeeff");
    const std::vector<uint8_t> iv16 = fromHex("112233445566778899aabbccddeeff00");

    // crypt=1 块(16B) 加密、skip=1 块(16B) 明文；crypt + skip = 2 <= 10（23001-7 9.6.1）
    const uint32_t cryptBlock = 1;
    const uint32_t skipBlock = 1;

    // 两个 subsample：第一个 protected 长度 33（一个 16 字节 run 后剩 17 字节 -> CTS），
    // 第二个 protected 长度 48（整 3 个 run），用来验证 IV 在每个 subsample 起点重置。
    const std::vector<SubsampleInfo> subsamples = {{4, 33}, {0, 48}};
    const size_t total = 4 + 33 + 48;

    const std::vector<uint8_t> plain = makeDeterministicBytes(total, 3);
    std::vector<uint8_t> cipher = plain;   // clear / skip 段保持明文

    size_t offset = 0;

    for (const SubsampleInfo &sub : subsamples) {
        offset += sub.clearBytes;
        const std::vector<uint8_t> region = referenceCbcsEncryptRegion(key, iv16, &plain[offset],
                                                                       sub.protectedBytes,
                                                                       cryptBlock, skipBlock);
        ASSERT_EQ(region.size(), static_cast<size_t>(sub.protectedBytes));
        memcpy(&cipher[offset], region.data(), region.size());
        offset += sub.protectedBytes;
    }

    EXPECT_NE(toHexLower(cipher), toHexLower(plain));

    CENCDecrypter dec;
    ASSERT_EQ(dec.setKey(kKeyIdHex, key.data(), 16), 0);

    std::vector<uint8_t> buffer = cipher;
    const int64_t ret = dec.decrypt(kKeyIdHex, "cbcs", iv16.data(), static_cast<uint32_t>(iv16.size()),
                                    subsamples, cryptBlock, skipBlock,
                                    buffer.data(), static_cast<int64_t>(buffer.size()));
    ASSERT_EQ(ret, static_cast<int64_t>(buffer.size()));
    EXPECT_EQ(toHexLower(buffer), toHexLower(plain));

    // pattern 参数非法（23001-7 9.6.1 约束）必须直接报错且不动 buffer
    {
        std::vector<uint8_t> buf = cipher;
        EXPECT_LT(dec.decrypt(kKeyIdHex, "cbcs", iv16.data(), 16, subsamples, 0, 0,
                              buf.data(), static_cast<int64_t>(buf.size())), 0);   // crypt == 0
        EXPECT_EQ(toHexLower(buf), toHexLower(cipher));
    }

    {
        std::vector<uint8_t> buf = cipher;
        EXPECT_LT(dec.decrypt(kKeyIdHex, "cbcs", iv16.data(), 16, subsamples, 10, 0,
                              buf.data(), static_cast<int64_t>(buf.size())), 0);   // crypt > 9
        EXPECT_EQ(toHexLower(buf), toHexLower(cipher));
    }

    {
        std::vector<uint8_t> buf = cipher;
        EXPECT_LT(dec.decrypt(kKeyIdHex, "cbcs", iv16.data(), 16, subsamples, 9, 9,
                              buf.data(), static_cast<int64_t>(buf.size())), 0);   // crypt + skip > 10
        EXPECT_EQ(toHexLower(buf), toHexLower(cipher));
    }

    {
        std::vector<uint8_t> buf = cipher;
        EXPECT_LT(dec.decrypt(kKeyIdHex, "cbcs", iv16.data(), 16, subsamples, 1, 10,
                              buf.data(), static_cast<int64_t>(buf.size())), 0);   // skip > 9
        EXPECT_EQ(toHexLower(buf), toHexLower(cipher));
    }
}

/* ================================================================== */
/* 测试 H：cbcs 的 1:9 pattern（整块边界，一个完整 run）                  */
/* ================================================================== */

TEST(cencDecrypter, cbcsPatternSkipNineBlocks)
{
    const std::vector<uint8_t> key = fromHex("ffeeddccbbaa99887766554433221100");
    const std::vector<uint8_t> iv16 = fromHex("00112233445566778899aabbccddeeff");

    // 整样本 160 字节 = 10 块；pattern 1 块加密 + 9 块明文 = 恰好一个完整 run
    const uint32_t cryptBlock = 1;
    const uint32_t skipBlock = 9;
    const size_t total = 160;

    const std::vector<uint8_t> plain = makeDeterministicBytes(total, 17);
    std::vector<uint8_t> cipher = plain;
    std::vector<uint8_t> chain = iv16;
    const std::vector<uint8_t> encFirst = referenceCbcCtsEncrypt(key, chain, plain.data(), 16);
    memcpy(cipher.data(), encFirst.data(), 16);
    EXPECT_NE(toHexLower(cipher), toHexLower(plain));

    CENCDecrypter dec;
    ASSERT_EQ(dec.setKey(kKeyIdHex, key.data(), 16), 0);

    // 无 subsample 信息（便捷重载）：整个样本一个 protected 区间
    std::vector<uint8_t> buffer = cipher;
    const int64_t ret = dec.decrypt(kKeyIdHex, "cbcs", iv16.data(), static_cast<uint32_t>(iv16.size()),
                                    cryptBlock, skipBlock, buffer.data(), static_cast<int64_t>(buffer.size()));
    ASSERT_EQ(ret, static_cast<int64_t>(buffer.size()));
    EXPECT_EQ(toHexLower(buffer), toHexLower(plain));

    // 只有一个 run 时 skip 段必须完全没被碰过（前 16 字节解密、后 144 字节原样）
    for (size_t i = 16; i < total; ++i) {
        EXPECT_EQ(buffer[i], plain[i]) << "skip byte at " << i << " was modified";
    }
}

/* ================================================================== */
/* 测试 I：cenc 的边界（16 字节 IV、clear-only、空样本）                  */
/* ================================================================== */

TEST(cencDecrypter, cencWholeSampleAndClearOnlyBoundaries)
{
    const std::vector<uint8_t> key = fromHex("2b7e151628aed2a6abf7158809cf4f3c");
    const std::vector<uint8_t> iv16 = fromHex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");

    CENCDecrypter dec;
    ASSERT_EQ(dec.setKey(kKeyIdHex, key.data(), 16), 0);

    const std::vector<uint8_t> plain = makeDeterministicBytes(48, 0);

    // subsample 列表形式
    {
        const std::vector<SubsampleInfo> whole = {{0, 48}};
        std::vector<uint8_t> cipher = referenceCtrCrypt(key, iv16, plain, whole);
        std::vector<uint8_t> buffer = cipher;
        const int64_t ret = dec.decrypt(kKeyIdHex, "cenc", iv16.data(), static_cast<uint32_t>(iv16.size()),
                                        whole, 0, 0, buffer.data(), static_cast<int64_t>(buffer.size()));
        ASSERT_EQ(ret, 48);
        EXPECT_EQ(toHexLower(buffer), toHexLower(plain));
    }

    // 不带 subsample 列表的便捷重载：必须与上面一致
    {
        const std::vector<SubsampleInfo> whole = {{0, 48}};
        std::vector<uint8_t> cipher = referenceCtrCrypt(key, iv16, plain, whole);
        const int64_t ret = dec.decrypt(kKeyIdHex, "cenc", iv16.data(), static_cast<uint32_t>(iv16.size()),
                                        0, 0, cipher.data(), static_cast<int64_t>(cipher.size()));
        ASSERT_EQ(ret, 48);
        EXPECT_EQ(toHexLower(cipher), toHexLower(plain));
    }

    // 全是 clear 的 subsample：任何字节都不该被改动
    {
        const std::vector<SubsampleInfo> allClear = {{48, 0}};
        std::vector<uint8_t> buffer = plain;
        const int64_t ret = dec.decrypt(kKeyIdHex, "cenc", iv16.data(), static_cast<uint32_t>(iv16.size()),
                                        allClear, 0, 0, buffer.data(), static_cast<int64_t>(buffer.size()));
        ASSERT_EQ(ret, 48);
        EXPECT_EQ(toHexLower(buffer), toHexLower(plain));
    }

    // 空样本：size == 0 返回 0
    {
        const int64_t ret = dec.decrypt(kKeyIdHex, "cenc", iv16.data(), static_cast<uint32_t>(iv16.size()),
                                        0, 0, nullptr, 0);
        EXPECT_EQ(ret, 0);
    }
}
