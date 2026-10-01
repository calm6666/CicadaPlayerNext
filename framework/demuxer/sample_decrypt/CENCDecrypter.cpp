//
// CENCDecrypter.cpp
//
// ISO/IEC 23001-7 Common Encryption 的软件样本解密实现。
//
// AES 原语复用内核已有的 decrypto/avAESDecrypt（封装 FFmpeg libavutil/aes.c）：
//   - CTR 需要的是 AES 分组「加密」：av_aes_crypt(..., iv = nullptr, 0)。FFmpeg 的
//     aes_encrypt() 只有在 iv != nullptr 时才做 CBC 链接，因此 iv = nullptr 等价于单分组 ECB；
//   - CBC 需要的是 AES 分组「解密」：av_aes_crypt(..., iv = nullptr, 1)，同理是 ECB 单分组解密，
//     分组间的 XOR 链由本文件自己做。
// 特别说明：avAESDecrypt::decrypt(dst, src, count, iv) 传非空 iv 时是「CBC 解密」入口，但
// FFmpeg 实现里 iv 是每个分组就地回写（不是逐块推进的链），所以它并不提供真正的分组间
// CBC 链。本文件不依赖它的链式语义，只把它当作 ECB 单分组原语使用。
//

#define LOG_TAG "CENCDecrypter"

#include "CENCDecrypter.h"

#include <cerrno>
#include <cstring>
#include <memory>
#include <utils/frame_work_log.h>

#include "../decrypto/avAESDecrypt.h"

using namespace Cicada;

namespace Cicada {
    /**
     * 一个 key id 对应的 AES 上下文。
     * enc 做分组加密（CTR 的 keystream），dec 做分组解密（cbc1/cbcs 的密文还原）。
     * CENC 内容密钥固定 16 字节 / 128bit（ISO/IEC 23001-7 8.2），所以两个上下文都用 128bit key。
     */
    struct CENCDecrypter::KeyEntry {
        std::unique_ptr<avAESEncrypt> enc{};   // av_aes_init(..., decrypt = 0)
        std::unique_ptr<avAESDecrypt> dec{};   // av_aes_init(..., decrypt = 1)
    };
} // namespace Cicada

/* ------------------------------------------------------------------ */
/* 小工具（文件内静态函数，不进头文件）                                  */
/* ------------------------------------------------------------------ */

static inline bool isHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static inline char lowerHex(uint8_t v)
{
    return static_cast<char>((v < 10) ? ('0' + static_cast<char>(v)) : ('a' + static_cast<char>(v - 10)));
}

bool CENCDecrypter::normalizeKeyId(const std::string &keyIdHex, std::string &normalized)
{
    normalized.clear();
    normalized.reserve(keyIdHex.size());

    for (char c : keyIdHex) {
        if (c == ':' || c == '-' || c == ' ' || c == '\t') {
            // 分隔符直接吞掉：带横线的 UUID 形式、"kid:" 前缀拼接形式都能对上同一个 key id
            continue;
        }

        if (!isHexDigit(c)) {
            normalized.clear();
            return false;
        }

        if (c >= 'A' && c <= 'F') {
            c = static_cast<char>(c - 'A' + 'a');
        }

        normalized.push_back(c);
    }

    if (normalized.empty() || (normalized.size() % 2) != 0) {
        normalized.clear();
        return false;
    }

    return true;
}

std::string CENCDecrypter::toHex(const uint8_t *data, uint32_t size)
{
    std::string out;
    out.reserve(static_cast<size_t>(size) * 2);

    for (uint32_t i = 0; i < size; ++i) {
        const uint8_t byte = data[i];
        out.push_back(lowerHex(static_cast<uint8_t>(byte >> 4)));
        out.push_back(lowerHex(static_cast<uint8_t>(byte & 0x0F)));
    }

    return out;
}

void CENCDecrypter::buildIv(const uint8_t *iv, uint32_t ivSize, uint8_t *ivOut)
{
    // ISO/IEC 23001-7 9.2/9.3：per-sample IV 的有效长度由 senc/saio 给出（cenc 通常 8，
    // cbc1/cbcs 为 16）；不足 16 字节时在右侧补 0 组成 16 字节 IV。
    memset(ivOut, 0, 16);
    memcpy(ivOut, iv, ivSize);
}

/* ------------------------------------------------------------------ */
/* 生命周期与 key 管理                                                  */
/* ------------------------------------------------------------------ */

CENCDecrypter::CENCDecrypter()
{
    // AES 上下文按 key id 惰性创建（见 setKey），临时缓冲都是定长成员，构造时已清零，
    // 因此每次 decrypt 不会产生堆分配。
}

CENCDecrypter::~CENCDecrypter()
{
    clearKeys();
}

void CENCDecrypter::clearKeys()
{
    for (auto &item : mKeys) {
        delete item.second;
    }

    mKeys.clear();
    mMissingKeyLogged = false;
}

int CENCDecrypter::setKey(const std::string &keyIdHex, const uint8_t *key, int keySize)
{
    if (key == nullptr || keySize != 16) {
        AF_LOGE("setKey: content key must be exactly 16 bytes, got %d\n", keySize);
        return -EINVAL;
    }

    std::string kid;

    if (!normalizeKeyId(keyIdHex, kid)) {
        AF_LOGE("setKey: malformed key id '%s'\n", keyIdHex.c_str());
        return -EINVAL;
    }

    auto it = mKeys.find(kid);
    const bool isNew = (it == mKeys.end());

    // 复用已存在的 KeyEntry（同 key id 重复注册 = 替换密钥），否则新建一个
    std::unique_ptr<KeyEntry> entry(isNew ? new KeyEntry() : nullptr);

    std::unique_ptr<avAESEncrypt> enc(new avAESEncrypt());
    std::unique_ptr<avAESDecrypt> dec(new avAESDecrypt());

    // av_aes_init 只接受 128/192/256 bit；CENC 内容密钥固定 128bit
    int ret = enc->setKey(key, 8 * 16);

    if (ret < 0) {
        AF_LOGE("setKey: aes encrypt init failed ret=%d\n", ret);
        return ret;
    }

    ret = dec->setKey(key, 8 * 16);

    if (ret < 0) {
        AF_LOGE("setKey: aes decrypt init failed ret=%d\n", ret);
        return ret;
    }

    if (isNew) {
        entry->enc = std::move(enc);
        entry->dec = std::move(dec);
        mKeys[kid] = entry.release();
    } else {
        // 同一 key id 重复注册 = 替换旧 key（例如 key 轮换后重新下发）
        it->second->enc = std::move(enc);
        it->second->dec = std::move(dec);
    }

    // 注册了新 key，之前那条「缺少密钥」的一次性日志重新武装
    mMissingKeyLogged = false;
    return 0;
}

const CENCDecrypter::KeyEntry *CENCDecrypter::findKey(const std::string &keyIdHex) const
{
    std::string kid;

    if (!normalizeKeyId(keyIdHex, kid)) {
        return nullptr;
    }

    auto it = mKeys.find(kid);

    if (it == mKeys.end()) {
        return nullptr;
    }

    return it->second;
}

bool CENCDecrypter::hasKey(const std::string &keyIdHex) const
{
    return findKey(keyIdHex) != nullptr;
}

/* ------------------------------------------------------------------ */
/* subsample 展开                                                      */
/* ------------------------------------------------------------------ */

bool CENCDecrypter::buildProtectedRanges(const std::vector<SubsampleInfo> &subsamples, int64_t size)
{
    mRanges.clear();

    if (subsamples.empty()) {
        // ISO/IEC 23001-7 9.2：没有 subsample 信息（senc 里 subsample_count == 0）时，
        // 整个样本都是 protected 数据。FFmpeg mov.c 的 cenc_scheme_decrypt 也是这样。
        if (size > 0) {
            mRanges.emplace_back(static_cast<int64_t>(0), size);
        }

        return true;
    }

    int64_t offset = 0;

    for (const SubsampleInfo &sub : subsamples) {
        // 用减法比较，避免 clearBytes + protectedBytes 先溢出再比较
        if (static_cast<int64_t>(sub.clearBytes) > size - offset) {
            return false;
        }

        offset += static_cast<int64_t>(sub.clearBytes);

        if (static_cast<int64_t>(sub.protectedBytes) > size - offset) {
            return false;
        }

        if (sub.protectedBytes > 0) {
            mRanges.emplace_back(offset, static_cast<int64_t>(sub.protectedBytes));
        }

        offset += static_cast<int64_t>(sub.protectedBytes);
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* cenc / cens：AES-CTR（ISO/IEC 23001-7 9.2，计数器构造见 9.2 与 NIST SP 800-38A F.5） */
/* ------------------------------------------------------------------ */

/*
 * ============ 计数器在样本内**跨 subsample 连续**（与两个生产实现一致）============
 *
 * 这一条是**实测定的**，不是从规范条文推的（第一版按"每个 subsample 重置"写，
 * 结果在真 CENC 产物上错了）：
 *
 *   用 `ffmpeg -encryption_scheme cenc-aes-ctr` 产出的真 CENC 文件与同一编码的明文
 *   逐样本比对，150 个样本里 **149 个逐字节解对**；唯一错的是第 0 个样本 ——
 *   它带 2 个 subsample（5 clear + 593 protected，5 clear + 6929 protected），
 *   第一个受保护区间解对，第二个区间整段错（首个错字节 = 603，正是第二区间起点）。
 *
 * 之所以判定"连续"才对，是因为两个互相独立、都在海量设备上跑的生产实现都这么做：
 *   · FFmpeg `libavformat/mov.c` 的 `cenc_scheme_decrypt()`：对每个 subsample 调
 *     `av_aes_ctr_crypt()`，**中间不重新 arm IV**；而它的加密端
 *     `libavformat/movenccenc.c` 的 `mov_cenc_write_encrypted()` 把**整个样本**
 *     一次性 crypt 出去 —— 计数器天然连续。
 *   · Shaka Packager `media/base/aes_encryptor.cc`：`AesCtrEncryptor::Encrypt()` 在
 *     `UpdateIv()` 之前一直沿用同一个 `counter_` 与 `block_offset_`，而 `UpdateIv()`
 *     是**每个样本**才调一次。
 *   规范口径也一致：计数器是"每个后续的样本数据分组 +1"，按**分组**递增。
 *
 * 【仍未解释的一处】上面那个多 subsample 样本的**第二个**受保护区间，用"连续"
 * （counter 38 起）和"重置"（counter 0 起）都复现不出来，把第二区间起始 counter
 * 从 30 扫到 50 也没有命中的值。也就是说：**该样本第二区间用的不是"同一把 IV +
 * 某个 counter"**。这是 ffmpeg 那个 muxer 在多 subsample 上的一个未定行为，
 * 尚未查清（见 tools/drm_bench/README.md 的复现方法）。结论按"与两个生产实现
 * 一致"落地：**连续**。
 *
 * 影响面：单 subsample 的样本两种写法等价（149/150 都是这一类），所以这条差异只在
 * "一个样本里既有 clear 前缀又有多个受保护区间"的片源上才会体现。
 */
void CENCDecrypter::decryptCtr(const uint8_t *iv, uint32_t ivSize, uint8_t *buffer,
                               const KeyEntry *entry,
                               const std::vector<std::pair<int64_t, int64_t>> &ranges)
{
    // 每个样本只在这里建一次 counter block；样本内所有受保护区间共用同一条计数器。
    buildIv(iv, ivSize, mCounterBlock);

    for (const auto &range : ranges) {
        uint8_t *pos = buffer + range.first;
        int64_t remaining = range.second;

        // counter block：高 ivSize 字节 = 该样本的 IV，低 8 字节 = 大端计数器，
        // 初值 0，每 16 字节分组 +1，**跨 range 连续**（见上面的长注释）。
        while (remaining > 0) {
            // av_aes_crypt 支持 dst == src，原地把 counter block 加密成 keystream
            entry->enc->encrypt(mKeyStream, mCounterBlock, 1, nullptr);

            const int64_t n = (remaining < 16) ? remaining : 16;

            for (int64_t i = 0; i < n; ++i) {
                pos[i] = static_cast<uint8_t>(pos[i] ^ mKeyStream[i]);
            }

            // 低 8 字节大端自增（进位只可能在低 64bit 内，与 av_aes_ctr_increment_be64 一致）
            for (int i = 15; i >= 8; --i) {
                mCounterBlock[i] = static_cast<uint8_t>(mCounterBlock[i] + 1);

                if (mCounterBlock[i] != 0) {
                    break;
                }
            }

            pos += n;
            remaining -= n;
        }
    }
}

/* ------------------------------------------------------------------ */
/* cbc1 / cbcs：AES-CBC（含 pattern 与尾部 CTS）                        */
/* ------------------------------------------------------------------ */

void CENCDecrypter::decryptCbcRegion(const KeyEntry *entry, uint8_t *data, int64_t length,
                                     uint8_t *chain, uint8_t *tmpPrev, uint8_t *tmpTail)
{
    // CBC 解密：P = AES-ECB-decrypt(C) XOR 前一段密文；链值在分段（senc 的每个 subsample、
    // cbc1/cbcs 每个 protected 区间）开头重置成该样本 IV。
    int64_t offset = 0;

    for (; length - offset >= 16; offset += 16) {
        uint8_t *c = data + offset;
        entry->dec->decrypt(tmpPrev, c, 1, nullptr);

        for (int i = 0; i < 16; ++i) {
            c[i] = static_cast<uint8_t>(tmpPrev[i] ^ chain[i]);
        }

        memcpy(chain, c, 16);
    }

    const int64_t tail = length - offset;

    if (tail == 0) {
        return;
    }

    // 尾部不满 16 字节：CTS（ciphertext stealing）。
    // 与 ISO/IEC 23001-7 对 cbc1/cbcs 的最后部分块处理一致，具体是 NIST SP 800-38A
    // Addendum 的 CBC-CS3：末两段密文块交换次序。
    if (offset < 16) {
        // 整个区间只有一段不满 16 字节：没有可偷的密文块，CTS 无定义。
        // 走到这里说明外部校验漏了，宁可原地不动也不猜。
        AF_LOGE("cbc: protected range shorter than two blocks (%lld bytes), left untouched\n",
                static_cast<long long>(length));
        return;
    }

    uint8_t *cPrev = data + offset - 16;   // C(n-1)
    uint8_t *cTail = data + offset;        // C(n)（tail 字节，尾部被截断）

    // X = AES-ECB-decrypt(C(n)) XOR C(n-1)
    entry->dec->decrypt(tmpPrev, cTail, 1, nullptr);

    for (int i = 0; i < 16; ++i) {
        chain[i] = static_cast<uint8_t>(tmpPrev[i] ^ cPrev[i]);
    }

    // P(n-1) = X（前 tail 字节是明文，后 16-tail 字节是「被偷走」的密文段 C(n-1) 残余）
    memcpy(tmpTail, chain, 16);
    memcpy(cPrev, chain, static_cast<size_t>(tail));

    // Z = AES-ECB-decrypt(chain) XOR P(n-1)  => P(n) = Z 的前 tail 字节
    entry->dec->decrypt(tmpPrev, chain, 1, nullptr);
    memcpy(chain, tmpTail, 16);

    for (int64_t i = 0; i < tail; ++i) {
        cTail[i] = static_cast<uint8_t>(tmpPrev[i] ^ chain[i]);
    }

    memcpy(chain, tmpTail, 16);
}

void CENCDecrypter::decryptCbcs(const uint8_t *iv, uint32_t ivSize, uint32_t cryptByteBlock,
                                uint32_t skipByteBlock, uint8_t *buffer, const KeyEntry *entry,
                                const std::vector<std::pair<int64_t, int64_t>> &ranges)
{
    // ISO/IEC 23001-7 9.6（cbcs）：cryptByteBlock 个 16 字节块加密、skipByteBlock 个 16 字节块
    // 保持明文，如此重复；每个 protected 区间的起点一定重新开始 pattern（也就是第一段
    // 16*cryptByteBlock 字节一定被加密）。
    const int64_t cryptBytes = static_cast<int64_t>(cryptByteBlock) * 16;
    const int64_t skipBytes = static_cast<int64_t>(skipByteBlock) * 16;

    for (const auto &range : ranges) {
        uint8_t *pos = buffer + range.first;
        int64_t remaining = range.second;

        // IV 每个 protected 区间重置；区间内 CBC 链连续（pattern 的 skip 段不打断链）
        buildIv(iv, ivSize, mChainBlock);

        while (remaining > 0) {
            if (remaining < cryptBytes) {
                // 尾部残余（可能正好是一整块，也可能不满）交给 CTS
                decryptCbcRegion(entry, pos, remaining, mChainBlock, mCtsTmp0, mCtsTmp1);
                remaining = 0;
                break;
            }

            decryptCbcRegion(entry, pos, cryptBytes, mChainBlock, mCtsTmp0, mCtsTmp1);
            pos += cryptBytes;
            remaining -= cryptBytes;

            const int64_t skip = (skipBytes < remaining) ? skipBytes : remaining;
            pos += skip;
            remaining -= skip;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 对外入口                                                             */
/* ------------------------------------------------------------------ */

int64_t CENCDecrypter::decrypt(const std::string &keyIdHex, const std::string &scheme,
                              const uint8_t *iv, uint32_t ivSize,
                              const std::vector<SubsampleInfo> &subsamples,
                              uint32_t cryptByteBlock, uint32_t skipByteBlock,
                              uint8_t *buffer, int64_t size)
{
    if (buffer == nullptr || size < 0) {
        if (!mMalformedLogged) {
            mMalformedLogged = true;
            AF_LOGE("decrypt: invalid buffer (buffer=%p, size=%lld), refusing to guess\n",
                    static_cast<const void *>(buffer), static_cast<long long>(size));
        }

        return -EINVAL;
    }

    if (iv == nullptr || ivSize == 0 || ivSize > 16) {
        if (!mMalformedLogged) {
            mMalformedLogged = true;
            AF_LOGE("decrypt: invalid iv (iv=%p, ivSize=%u), must be 1..16 bytes\n",
                    static_cast<const void *>(iv), ivSize);
        }

        return -EINVAL;
    }

    // scheme 判断放在 key 查询之前：scheme 错误时不该报「缺 key」
    const bool isCtr = (scheme == "cenc" || scheme == "cens");
    const bool isCbc1 = (scheme == "cbc1");
    const bool isCbcs = (scheme == "cbcs");

    if (!isCtr && !isCbc1 && !isCbcs) {
        if (!mMalformedLogged) {
            mMalformedLogged = true;
            AF_LOGE("decrypt: unsupported scheme '%s'\n", scheme.c_str());
        }

        return -EINVAL;
    }

    if (isCbcs) {
        // ISO/IEC 23001-7 9.6.1：1 <= cryptByteBlock <= 9，0 <= skipByteBlock <= 9，
        // 且 skipByteBlock != 0 时 crypt + skip <= 10。
        // 用减法写上限比较，避免 crypt + skip 在 uint32_t 上先溢出再比较。
        const bool patternOk = (cryptByteBlock >= 1 && cryptByteBlock <= 9) &&
                               (skipByteBlock <= 9) &&
                               (skipByteBlock == 0 || (cryptByteBlock <= 10 - skipByteBlock));

        if (!patternOk) {
            if (!mMalformedLogged) {
                mMalformedLogged = true;
                AF_LOGE("decrypt: invalid cbcs pattern crypt=%u skip=%u\n", cryptByteBlock, skipByteBlock);
            }

            return -EINVAL;
        }
    }

    const KeyEntry *entry = findKey(keyIdHex);

    if (entry == nullptr) {
        if (!mMissingKeyLogged) {
            mMissingKeyLogged = true;
            AF_LOGE("decrypt: no content key registered for key id '%s' (scheme=%s). "
                    "Refusing to pass ciphertext through as if it were plaintext\n",
                    keyIdHex.c_str(), scheme.c_str());
        }

        return -ENOENT;
    }

    if (size == 0) {
        // 空样本没有任何数据要解密，也不算错误
        return 0;
    }

    if (!buildProtectedRanges(subsamples, size)) {
        if (!mMalformedLogged) {
            mMalformedLogged = true;
            AF_LOGE("decrypt: subsample layout exceeds sample size %lld\n", static_cast<long long>(size));
        }

        return -EINVAL;
    }

    if (isCtr) {
        decryptCtr(iv, ivSize, buffer, entry, mRanges);
    } else if (isCbc1) {
        // cbc1 = 无 pattern 的 CBC（等价于 cryptByteBlock 覆盖整个区间）
        for (const auto &range : mRanges) {
            buildIv(iv, ivSize, mChainBlock);
            decryptCbcRegion(entry, buffer + range.first, range.second, mChainBlock, mCtsTmp0, mCtsTmp1);
        }
    } else {
        decryptCbcs(iv, ivSize, cryptByteBlock, skipByteBlock, buffer, entry, mRanges);
    }

    return size;
}

int64_t CENCDecrypter::decrypt(const std::string &keyIdHex, const std::string &scheme,
                              const uint8_t *iv, uint32_t ivSize,
                              uint32_t cryptByteBlock, uint32_t skipByteBlock,
                              uint8_t *buffer, int64_t size)
{
    // 无 subsample 信息 = 整个样本一个 protected 区间（与 FFmpeg subsample_count == 0 一致）
    static const std::vector<SubsampleInfo> kNoSubsamples{};
    return decrypt(keyIdHex, scheme, iv, ivSize, kNoSubsamples, cryptByteBlock, skipByteBlock, buffer, size);
}
