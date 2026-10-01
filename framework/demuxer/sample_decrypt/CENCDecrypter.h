//
// CENCDecrypter.h
//
// 纯软件 CENC（ISO/IEC 23001-7 Common Encryption）样本解密器。
//
// 为什么需要它：本播放器此前只能依赖平台 CDM（Widevine/PlayReady 等）解密 CENC
// 内容，缺少 CDM 的平台（或 ClearKey 场景）拿到的是密文，直接喂给解码器只会花屏。
// 这个类只用内核已有的 AES 原语（decrypto/avAESDecrypt）实现 23001-7 规定的
// 四种 scheme，不依赖任何平台宏、不依赖 CDM。
//
// 本头文件刻意不引入包（IAFPacket）相关头文件：SubsampleInfo 定义在命名空间作用域，
// 调用方用 (clearBytes, protectedBytes) 二元组即可构造 subsample 列表；需要把
// IAFPacket::EncryptionInfo 转过来的地方，交给集成层做一次平庸的拷贝即可。
//

#ifndef CICADA_CENCDECRYPTER_H
#define CICADA_CENCDECRYPTER_H

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <utils/CicadaType.h>

namespace Cicada {
    /**
     * ISO/IEC 23001-7 中一个 subsample 的布局：先是 clearBytes 字节明文（原样保留），
     * 接着是 protectedBytes 字节需要解密的负载。放在命名空间作用域而不是嵌套在
     * CENCDecrypter 里，是为了让调用方不必为了构造它而包含任何包/解复用头文件。
     */
    struct SubsampleInfo {
        uint32_t clearBytes{0};
        uint32_t protectedBytes{0};
    };

    /**
     * 软件 CENC 样本解密器。
     *
     * 支持的 scheme（ISO/IEC 23001-7 第 9 章 / 第 10 章）：
     *   - "cenc"：AES-CTR，整样本或 subsample 各自重置计数器；
     *   - "cens"：同 "cenc"（23001-7 里 cens 是 cenc 的 pattern-encryption 版本，
     *             但 CTR 模式下 pattern 没有意义，FFmpeg 对 cens 也走 CTR 全加密路径，
     *             这里按 CTR 处理，与 avFormatDemuxer 拿到的元数据一致）；
     *   - "cbc1"：AES-CBC，无 pattern，最后一个不满 16 字节的块用 CTS（ciphertext
     *             stealing，即 NIST SP 800-38A Addendum 的 CBC-CS3）处理；
     *   - "cbcs"：AES-CBC + pattern（先加密 cryptByteBlock 块、再跳过 skipByteBlock 块），
     *             第一个 run 的起点一定是 subsample 起点，尾部不满块同样用 CTS。
     *
     * 线程安全：与工程里其它 decrypter 一致——setKey/clearKeys 只允许在
     * demuxer 线程调用；decrypt 可以在读线程反复调用。类内部持有少量临时缓冲，
     * 因此 decrypt 不可重入（同一实例不要被多个线程同时调用）。
     */
    class CICADA_CPLUS_EXTERN CENCDecrypter {
    public:
        CENCDecrypter();

        ~CENCDecrypter();

        // 禁止拷贝：内部持有 AES 上下文与临时缓冲
        CENCDecrypter(const CENCDecrypter &) = delete;

        CENCDecrypter &operator=(const CENCDecrypter &) = delete;

        /**
         * 为 16 字节 key id 注册 16 字节内容密钥。
         *
         * keyIdHex 是十六进制字符串，大小写不敏感；分隔符 ':'、'-'、空格会被忽略，
         * 所以 "1077efecc0b24d02ace33c1e52e2fb4b"、"1077EFEC-C0B2-4D02-ACE3-3C1E52E2FB4B"
         * 和 "1077efec c0b2 4d02 ace3 3c1e52e2fb4b" 指向同一个 key id。
         * 同一个 key id 重复注册会替换旧 key。
         *
         * 成功返回 0；keyIdHex 为空/非法、key 为 nullptr、keySize != 16 返回 -EINVAL；
         * AES 上下文初始化失败时返回底层 av_aes_init 的负返回值。
         */
        int setKey(const std::string &keyIdHex, const uint8_t *key, int keySize);

        // 清空所有已注册的 key。
        void clearKeys();

        // 该 key id 是否已注册 key（十六进制串大小写、分隔符处理同 setKey）。
        bool hasKey(const std::string &keyIdHex) const;

        /**
         * 就地解密一个样本。
         *
         * keyIdHex        ：来自 IAFPacket::EncryptionInfo::key_id 的十六进制串。
         * scheme（ISO/IEC 23001-7 里规定的四种，见该类顶部说明）：
         *   "cenc" / "cens" / "cbc1" / "cbcs"。
         * iv/ivSize       ：来自 EncryptionInfo::iv/iv_size；ivSize 必须在 [1,16]，
         *                   "cenc"/"cens" 通常是 8，"cbc1"/"cbcs" 是 16。
         * subsamples      ：subsample 列表；为空表示整个样本是一个 protected subsample
         *                   （与 FFmpeg mov.c 的 subsample_count == 0 语义一致）。
         * cryptByteBlock  ：cbcs 的 pattern 前半段（块数），1..9。
         * skipByteBlock   ：cbcs 的 pattern 后半段（块数），0..9，且 crypt+skip <= 10
         *                   （skip != 0 时）。
         * buffer/size     ：样本负载，就地解密。
         *
         * 成功返回写入 buffer 的字节数（等于 size）；失败返回负的 errno 风格值：
         *   -EINVAL：buffer 为空、size < 0、ivSize 越界、scheme 未知、pattern 非法、
         *            subsample 长度之和超过样本长度；
         *   -ENOENT：该 key id 没有注册密钥。
         *
         * 任何失败路径都不会写入 buffer——绝不会把密文当明文返回。
         */
        int64_t decrypt(const std::string &keyIdHex, const std::string &scheme,
                        const uint8_t *iv, uint32_t ivSize,
                        const std::vector<SubsampleInfo> &subsamples,
                        uint32_t cryptByteBlock, uint32_t skipByteBlock,
                        uint8_t *buffer, int64_t size);

        /**
         * 便捷重载：样本整体作为一个 protected subsample。等价于传入空的 subsamples。
         */
        int64_t decrypt(const std::string &keyIdHex, const std::string &scheme,
                        const uint8_t *iv, uint32_t ivSize,
                        uint32_t cryptByteBlock, uint32_t skipByteBlock,
                        uint8_t *buffer, int64_t size);

        /**
         * 表里是否**一把** key 都没有。
         *
         * 调用方（demuxer_service）用它做"本次播放到底有没有走软件解密"的判据：
         * 注册失败后如果表还是空的，就把解密器整个丢掉，让读取路径回到零开销。
         */
        bool hasAnyKey() const
        {
            return !mKeys.empty();
        }

        /**
         * 把密钥 ID 字节串转成十六进制串（小写）。
         *
         * 公开出来是因为调用方拿到的是 IAFPacket::EncryptionInfo 里的**字节**形式的
         * key_id，而注册/查表用的是十六进制串，两边必须用同一个转换，否则会出现
         * "明明注册过却查不到"这种最难查的错。
         */
        static std::string toHex(const uint8_t *data, uint32_t size);

    private:
        struct KeyEntry;

        // 把 keyIdHex 归一化成小写、去分隔符的形式；非法（奇数长度/非十六进制）返回 false。
        static bool normalizeKeyId(const std::string &keyIdHex, std::string &normalized);

        // 按 key id 查表；未命中返回 nullptr。
        const KeyEntry *findKey(const std::string &keyIdHex) const;

        // 把 subsamples（可能为空）解析成 [起始偏移, 长度) 的 protected 区间列表，写进
        // 成员 mRanges（保留 capacity，避免每包重新分配）。校验失败返回 false（不写 buffer）。
        bool buildProtectedRanges(const std::vector<SubsampleInfo> &subsamples, int64_t size);

        // cenc/cens：AES-CTR。counter block 在每个样本开头构造一次，**样本内跨
        // subsample 连续递增**（实测定的，理由见 .cpp 里 decryptCtr 的长注释）。
        void decryptCtr(const uint8_t *iv, uint32_t ivSize, uint8_t *buffer, const KeyEntry *entry,
                        const std::vector<std::pair<int64_t, int64_t>> &ranges);

        // 对一段 protected 数据做 CBC 解密，尾部不满 16 字节时按 CBC-CS3 做 CTS。
        // chain 进来是这段的链值（IV），出去时被更新为该段最后一段密文；tmpPrev/tmpTail 是
        // 调用方提供的 16 字节临时缓冲（避免每包 malloc）。
        static void decryptCbcRegion(const KeyEntry *entry, uint8_t *data, int64_t length,
                                     uint8_t *chain, uint8_t *tmpPrev, uint8_t *tmpTail);

        // cbcs：AES-CBC + pattern + 尾部 CTS。每个 protected 区间重置 IV，区间内 CBC 链连续。
        void decryptCbcs(const uint8_t *iv, uint32_t ivSize, uint32_t cryptByteBlock,
                         uint32_t skipByteBlock, uint8_t *buffer, const KeyEntry *entry,
                         const std::vector<std::pair<int64_t, int64_t>> &ranges);

        // 入口：把 iv（ivSize 字节）铺成 16 字节 IV，其余字节补 0。
        static void buildIv(const uint8_t *iv, uint32_t ivSize, uint8_t *ivOut);

    private:
        // AES-CTR 的 counter block：前 8 字节放 IV，后 8 字节是大端计数器。
        uint8_t mCounterBlock[16]{0};
        // 加密出的 keystream block。
        uint8_t mKeyStream[16]{0};
        // CBC 解密时要保留的链值（上一段密文）。
        uint8_t mChainBlock[16]{0};
        // CTS 尾部需要的两个 16 字节临时块。
        uint8_t mCtsTmp0[16]{0};
        uint8_t mCtsTmp1[16]{0};

        std::map<std::string, KeyEntry *> mKeys{};

        // 上一次解密展开出的 protected 区间；作为成员保留只是为了复用 capacity
        std::vector<std::pair<int64_t, int64_t>> mRanges{};

        // 一次性日志去重：缺少密钥 / 输入非法各只打一条，避免每包刷屏
        // （做法与 HLSStream 的 mKeyFetchFailedLogged 一致）。
        bool mMissingKeyLogged{false};
        bool mMalformedLogged{false};
    };
} // namespace Cicada

#endif // CICADA_CENCDECRYPTER_H
