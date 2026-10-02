//
// DrmSchemes.h
//
// DASH 的 <ContentProtection> 方案注册表 + 选优。
//
// 【为什么需要它】DASH 里"这一路流受什么保护"是**由清单里的 schemeIdUri 指名**的，
// 而 DASH-IF 登记过的方案标识有 30 来个（Widevine / PlayReady / FairPlay / ClearKey /
// Marlin / Nagra / Irdeto / Verimatrix / ChinaDRM / Adobe Primetime / …，见
// https://dashif.org/identifiers/content_protection/ ，本文件逐条落表）。
// 在本文件之前，工程里只有**零散的字面量**：
//   · DashStream 把 schemeIdUri 原样塞进 Stream_meta.keyFormat；
//   · WideVineDrmHandler / mediaCodecDecoder / SuperMediaPlayer / Java 各自硬编码
//     Widevine 一条；
//   · DrmUtils::isSupport 只认 Widevine 且带 #ifdef ANDROID。
// 后果有三个，都是真问题：
//   1. 一个只有 <ContentProtection schemeIdUri="urn:mpeg:dash:mp4protection:2011"/>
//      的标准 MPD（没有指名任何 DRM 系统）会把 `urn:mpeg:dash:mp4protection:2011`
//      当成"DRM 系统标识"填给解码器 —— 没有任何 CDM 认得出它，
//      而 `codecPrototype::create` 的判据是
//      `drmInfo == nullptr || codec->is_drmSupport(drmInfo)`
//      （avcodecDecoder::is_drmSupport 恒返回 false），于是**所有解码器都被排除**，
//      起播直接失败；
//   2. 同时声明多个 DRM 系统时是"最后一条赢"（把 ContentProtection 从后往前扫），
//      在 Android 上可能选中设备根本不支持的 PlayReady 而不是 Widevine；
//   3. 各家写 UUID 的大小写/前缀/别名不统一（`urn:uuid:EDEF8BA9-…`、裸 UUID、
//      `com.widevine.alpha`），字符串直比必然漏。
//
// 【本文件做什么】只做两件事，都是**纯函数**（不做 I/O、不打日志、没有平台宏）：
//   · 把 schemeIdUri 归一化成规范形式，回答"这是哪个 DRM 系统 / 是不是 CENC 内容"；
//   · 在若干条候选里选一条交给上层，并说清"谁来解"（平台 CDM 还是内核软解）。
// 具体"怎么解"仍由各自的解密器负责（CENCDecrypter / 各平台 DrmHandler）。
//
// 【没有任何布尔开关】这里全是值：候选是清单里写的东西，结论是"选中的那一条 +
// 平台认不认 + 有没有可取密钥的地址"，下游按值决定行为。
//

#ifndef CICADA_PLAYER_DRM_SCHEMES_H
#define CICADA_PLAYER_DRM_SCHEMES_H

#include <cstddef>
#include <string>
#include <vector>

#include <utils/CicadaType.h>

namespace Cicada {

    class CICADA_CPLUS_EXTERN DrmSchemes {
    public:
        /**
         * 方案类别。区分它是因为"下游能不能用它解密"完全不同：
         *   · System / CencBaseline ⇒ 内容按 CENC(cenc/cbcs) 封的，可以软解也可以交给 CDM；
         *   · HlsClearKey*          ⇒ HLS 侧的整片 / SAMPLE-AES Clear Key 标识，不是 CENC；
         *   · CaDescriptor          ⇒ MPEG-2 TS 的条件接收，不是 CENC；
         *   · Sea                   ⇒ DASH-IF 的 SEA（CENC 之前的方案，密钥在清单里）。
         */
        enum class Kind {
            Unknown = 0,
            CencBaseline,       // urn:mpeg:dash:mp4protection:2011
            System,             // urn:uuid:<uuid> —— 指名某个 DRM 系统
            HlsClearKeyCbc,     // 3ea8778f-… HLS Clear Key AES-128(CBC)
            HlsClearKeyCbcs,    // be58615b-… HLS Clear Key SAMPLE-AES(CBCS)
            CaDescriptor,       // urn:mpeg:dash:13818:1:CA_descriptor:2011
            Sea,                // urn:mpeg:dash:sea:2012
        };

        /** 注册表里的一条。schemeIdUri / systemId / name 都是静态字面量，不会为空指针。 */
        struct Entry {
            const char *schemeIdUri;   // 规范形式（urn:uuid: 已小写、无花括号）
            const char *systemId;      // 裸 UUID（小写）；非 DRM 系统的方案是空串
            const char *name;          // 人读名字，只用于日志与文档
            Kind kind;
            // 在 DASH 里这个方案的内容是不是按 CENC（ISO/IEC 23001-7）封的。
            // false 表示"内核的 CENCDecrypter 对它无能为力"，必须显式说出来。
            bool cencCapable;
        };

        /** 一条候选：清单里某一层声明的 ContentProtection。字段就是清单里的值。 */
        struct Candidate {
            std::string schemeIdUri;
            std::string licenseUrl;   // 这一条上声明的许可/密钥地址（清单没写就是空）
            std::string pssh;         // <cenc:pssh> 的 base64 文本
            std::string keyId;        // cenc:default_KID
        };

        /** 选优结果。全部是**值**，没有开关。 */
        struct Decision {
            int index{-1};                  // 选中的候选下标；-1 表示一条都不能用
            bool usable{false};             // 上层能不能拿它去播
            bool handledByPlatform{false};  // 本平台有 CDM 认它 ⇒ 交给平台硬解
            std::string schemeIdUri;        // 规范形式
            std::string systemId;           // 裸 UUID（可能为空）
            std::string name;               // 人读名字
            std::string licenseUrl;
            std::string pssh;
            std::string keyId;
        };

        /**
         * 归一化 schemeIdUri。能接受清单里实际会出现的各种写法：
         *   urn:uuid:<uuid>（任意大小写）、裸 UUID（带/不带花括号）、32 个 hex，
         *   以及 EME 惯用的别名（com.widevine.alpha / com.microsoft.playready /
         *   com.apple.fps.* / org.w3.clearkey / com.adobe.primetime）。
         * 认不出来的一律**小写去空白后原样返回**（不发明、不丢弃）；空输入返回空串。
         */
        static std::string canonical(const std::string &schemeIdUri);

        /** 规范形式对应的裸 UUID（小写，带连字符）。不是 DRM 系统时返回空串。 */
        static std::string systemIdOf(const std::string &schemeIdUri);

        /** 人读名字；注册表里没有就返回空串（**不猜**）。 */
        static std::string nameOf(const std::string &schemeIdUri);

        static Kind kindOf(const std::string &schemeIdUri);

        /** 注册表里有没有这一条（按规范形式查）。 */
        static bool isKnown(const std::string &schemeIdUri);

        /**
         * 这个方案的内容能不能被内核的 CENCDecrypter 解。
         * 判据：注册表里有就以表里的 cencCapable 为准；表里没有但形如 urn:uuid:<uuid>
         * 的按 true 处理 —— DASH 里以"DRM 系统 UUID"指名的内容一律是 CENC 封装的，
         * 新出现的、本表还没登记的 DRM 系统也该走同一条软解路。
         */
        static bool isCencCapable(const std::string &schemeIdUri);

        /** 注册表条目数与逐条访问（自检与文档用）。 */
        static std::size_t count();

        static const Entry &at(std::size_t index);

        /**
         * 本平台有没有 CDM 认这条候选。判据**没有任何平台宏**：就是问一句
         * DrmHandlerPrototype::isSupport（各平台各自 addPrototype）。
         */
        static bool isPlatformSupported(const Candidate &candidate);

        /**
         * 在候选里选一条。排序判据（从高到低）：
         *   1. 不是 CENC 方案的一律排到最后（内核解不了，平台也基本不认）；
         *   2. 本平台 CDM 认的最高 —— 能硬解就硬解；
         *   3. 指了具体 DRM 系统的（urn:uuid:）高于只有 mp4protection 的通用声明；
         *   4. 带许可/密钥地址的更高（软解兜底要它）；
         *   5. 同分时**后声明的赢** —— MPDParser 先塞 AdaptationSet 层再塞 Representation 层，
         *      所以这正好是 DASH 的"Rep 覆盖 AS"继承语义。
         * 全都不满足时返回 index = -1 / usable = false，由调用方明确报错，**不猜**。
         */
        static Decision decide(const std::vector<Candidate> &candidates);
    };

} // namespace Cicada

#endif // CICADA_PLAYER_DRM_SCHEMES_H
