//
// DrmSchemes.cpp
//
// 见 DrmSchemes.h 的说明。这里只有纯字符串处理与一张静态表，没有 I/O、没有日志、
// 没有平台宏 —— 所以它可以被 L1 的任何一层调用，也可以被单文件自检工具单独编译。
//

#include "DrmSchemes.h"

#include "DrmHandlerPrototype.h"
#include "DrmInfo.h"

#include <cctype>
#include <climits>
#include <cstring>

using namespace Cicada;

namespace {

/*
 * ============ DASH-IF 内容保护标识登记表 ============
 *
 * 来源：https://dashif.org/identifiers/content_protection/（2024 版页面，本轮抓取）。
 * 页面把标识分两类：
 *   · 通用标识（generic）：urn:mpeg:dash:mp4protection:2011 / urn:mpeg:dash:13818:1:CA_descriptor:2011
 *   · 保护系统专有标识（protection system specific）：28 个 UUID，清单里写成
 *     `urn:uuid:<uuid>` 使用。
 * 下面把这两类**逐条**落表（28 + 2），另外补一条 DASH-IF 已废弃但历史上真实使用的
 * `urn:mpeg:dash:sea:2012`（SEA），并显式标注它**不是** CENC。
 *
 * 【cencCapable 这一列的判据】DASH 里以"DRM 系统 UUID"指名的内容一律按 ISO/IEC 23001-7
 * 的 CENC（`cenc`/`cens`/`cbc1`/`cbcs`）封装；因此表里除下面这几条外都是 true：
 *   · urn:mpeg:dash:13818:1:CA_descriptor:2011 —— MPEG-2 TS 的条件接收，不是 CENC；
 *   · 3ea8778f-… / be58615b-… —— 登记表原话就是它们描述的是 **HLS** 的 Clear Key
 *     AES-128(CBC) / SAMPLE-AES(CBCS)，不是 CENC 的样本加密格式；
 *   · urn:mpeg:dash:sea:2012 —— SEA 有自己的密钥分发（密钥在清单里），不是 CENC。
 * 这一列的作用只有一个：让"内核解不了这个方案"变成一句**明确的话**，
 * 而不是让 CENCDecrypter 拿着一堆它不认识的字节去试。
 */
const DrmSchemes::Entry kEntries[] = {
    // ---- 通用标识 ----
    {"urn:mpeg:dash:mp4protection:2011",     "", "MPEG-DASH CENC (mp4protection)",       DrmSchemes::Kind::CencBaseline, true},
    {"urn:mpeg:dash:13818:1:ca_descriptor:2011", "", "MPEG-2 TS conditional access",     DrmSchemes::Kind::CaDescriptor, false},
    {"urn:mpeg:dash:sea:2012",               "", "MPEG-DASH SEA (deprecated)",           DrmSchemes::Kind::Sea,          false},

    // ---- 保护系统专有标识（DASH-IF 登记表；按 UUID 升序便于对照）----
    {"urn:uuid:1f83e1e8-6ee9-4f0d-ba2f-5ec4e3ed1a66", "1f83e1e8-6ee9-4f0d-ba2f-5ec4e3ed1a66", "SecureMedia",                     DrmSchemes::Kind::System, true},
    {"urn:uuid:279fe473-512c-48fe-ade8-d176fee6b40f", "279fe473-512c-48fe-ade8-d176fee6b40f", "Arris Titanium",                  DrmSchemes::Kind::System, true},
    {"urn:uuid:35bf197b-530e-42d7-8b65-1b4bf415070f", "35bf197b-530e-42d7-8b65-1b4bf415070f", "DivX DRM Series 5",               DrmSchemes::Kind::System, true},
    {"urn:uuid:37c33258-7b99-4c7e-b15d-19af74482154", "37c33258-7b99-4c7e-b15d-19af74482154", "Commscope Titanium V3",           DrmSchemes::Kind::System, true},
    {"urn:uuid:3d5e6d35-9b9a-41e8-b843-dd3c6e72c42c", "3d5e6d35-9b9a-41e8-b843-dd3c6e72c42c", "ChinaDRM",                        DrmSchemes::Kind::System, true},
    {"urn:uuid:3ea8778f-7742-4bf9-b18b-e834b2acbd47", "3ea8778f-7742-4bf9-b18b-e834b2acbd47", "Clear Key AES-128 (HLS CBC)",     DrmSchemes::Kind::HlsClearKeyCbc, false},
    {"urn:uuid:45d481cb-8fe0-49c0-ada9-ab2d2455b2f2", "45d481cb-8fe0-49c0-ada9-ab2d2455b2f2", "CoreCrypt",                       DrmSchemes::Kind::System, true},
    {"urn:uuid:5e629af5-38da-4063-8977-97ffbd9902d4", "5e629af5-38da-4063-8977-97ffbd9902d4", "Marlin Adaptive Streaming SP",    DrmSchemes::Kind::System, true},
    {"urn:uuid:616c7469-6361-7374-2d50-726f74656374", "616c7469-6361-7374-2d50-726f74656374", "Alticast",                        DrmSchemes::Kind::System, true},
    {"urn:uuid:644fe7b5-260f-4fad-949a-0762ffb054b4", "644fe7b5-260f-4fad-949a-0762ffb054b4", "CMLA (OMA DRM)",                  DrmSchemes::Kind::System, true},
    {"urn:uuid:6a99532d-869f-5922-9a91-113ab7b1e2f3", "6a99532d-869f-5922-9a91-113ab7b1e2f3", "MobiTV DRM (MobiDRM)",            DrmSchemes::Kind::System, true},
    {"urn:uuid:6dd8b3c3-45f4-4a68-bf3a-64168d01a4a6", "6dd8b3c3-45f4-4a68-bf3a-64168d01a4a6", "ABV DRM (MoDRM)",                 DrmSchemes::Kind::System, true},
    {"urn:uuid:793b7956-9f94-4946-a942-23e7ef7e44b4", "793b7956-9f94-4946-a942-23e7ef7e44b4", "VisionCrypt",                     DrmSchemes::Kind::System, true},
    {"urn:uuid:80a6be7e-1448-4c37-9e70-d5aebe04c8d2", "80a6be7e-1448-4c37-9e70-d5aebe04c8d2", "Irdeto Content Protection",       DrmSchemes::Kind::System, true},
    {"urn:uuid:94ce86fb-07ff-4f43-adb8-93d2fa968ca2", "94ce86fb-07ff-4f43-adb8-93d2fa968ca2", "Apple FairPlay",                  DrmSchemes::Kind::System, true},
    {"urn:uuid:992c46e6-c437-4899-b6a0-50fa91ad0e39", "992c46e6-c437-4899-b6a0-50fa91ad0e39", "SecureMedia SteelKnot",           DrmSchemes::Kind::System, true},
    {"urn:uuid:9a04f079-9840-4286-ab92-e65be0885f95", "9a04f079-9840-4286-ab92-e65be0885f95", "Microsoft PlayReady",             DrmSchemes::Kind::System, true},
    {"urn:uuid:9a27dd82-fde2-4725-8cbc-4234aa06ec09", "9a27dd82-fde2-4725-8cbc-4234aa06ec09", "Verimatrix VCAS",                 DrmSchemes::Kind::System, true},
    {"urn:uuid:a68129d3-575b-4f1a-9cba-3223846cf7c3", "a68129d3-575b-4f1a-9cba-3223846cf7c3", "Synamedia/Cisco/NDS VideoGuard",  DrmSchemes::Kind::System, true},
    {"urn:uuid:aa11967f-cc01-4a4a-8e99-c5d3dddfea2d", "aa11967f-cc01-4a4a-8e99-c5d3dddfea2d", "Unitend DRM (UDRM)",              DrmSchemes::Kind::System, true},
    {"urn:uuid:adb41c24-2dbf-4a6d-958b-4457c0d27b95", "adb41c24-2dbf-4a6d-958b-4457c0d27b95", "Nagra MediaAccess PRM 3.0",       DrmSchemes::Kind::System, true},
    {"urn:uuid:b4413586-c58c-ffb0-94a5-d4896c1af6c3", "b4413586-c58c-ffb0-94a5-d4896c1af6c3", "Viaccess-Orca DRM (VODRM)",       DrmSchemes::Kind::System, true},
    {"urn:uuid:be58615b-19c4-4684-88b3-c8c57e99e957", "be58615b-19c4-4684-88b3-c8c57e99e957", "Clear Key SAMPLE-AES (HLS CBCS)", DrmSchemes::Kind::HlsClearKeyCbcs, false},
    {"urn:uuid:dcf4e3e3-62f1-5818-7ba6-0a6fe33ff3dd", "dcf4e3e3-62f1-5818-7ba6-0a6fe33ff3dd", "DigiCAP SmartXess",               DrmSchemes::Kind::System, true},
    {"urn:uuid:e2719d58-a985-b3c9-781a-b030af78d30e", "e2719d58-a985-b3c9-781a-b030af78d30e", "W3C ClearKey (DASH-IF Clear Key)", DrmSchemes::Kind::System, true},
    {"urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed", "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed", "Widevine",                        DrmSchemes::Kind::System, true},
    {"urn:uuid:f239e769-efa3-4850-9c16-a903c6932efb", "f239e769-efa3-4850-9c16-a903c6932efb", "Adobe Primetime DRM v4",          DrmSchemes::Kind::System, true},
    {"urn:uuid:1077efec-c0b2-4d02-ace3-3c1e52e2fb4b", "1077efec-c0b2-4d02-ace3-3c1e52e2fb4b", "W3C Common PSSH box",             DrmSchemes::Kind::System, true},
};

const std::size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

/*
 * EME / HLS KEYFORMAT 里惯用的别名。这些字符串不写进 DASH 的 schemeIdUri 才是常态，
 * 但它们确实是"同一个 DRM 系统"的另一种写法，清单里偶有出现（尤其是对象化清单与
 * 手写 KEYFORMAT），认出来比认不出来强。**别名表只做映射，不发明新系统。**
 */
struct Alias {
    const char *name;
    const char *schemeIdUri;
};

const Alias kAliases[] = {
    {"com.widevine.alpha",                     "urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed"},
    {"com.microsoft.playready",                "urn:uuid:9a04f079-9840-4286-ab92-e65be0885f95"},
    {"com.microsoft.playready.recommendation", "urn:uuid:9a04f079-9840-4286-ab92-e65be0885f95"},
    {"com.apple.fps",                          "urn:uuid:94ce86fb-07ff-4f43-adb8-93d2fa968ca2"},
    {"com.apple.fps.1_0",                      "urn:uuid:94ce86fb-07ff-4f43-adb8-93d2fa968ca2"},
    {"com.apple.fps.2_0",                      "urn:uuid:94ce86fb-07ff-4f43-adb8-93d2fa968ca2"},
    {"com.apple.fps.3_0",                      "urn:uuid:94ce86fb-07ff-4f43-adb8-93d2fa968ca2"},
    {"com.adobe.primetime",                    "urn:uuid:f239e769-efa3-4850-9c16-a903c6932efb"},
    {"com.adobe.access",                       "urn:uuid:f239e769-efa3-4850-9c16-a903c6932efb"},
    {"org.w3.clearkey",                        "urn:uuid:e2719d58-a985-b3c9-781a-b030af78d30e"},
    {"org.w3.clearkey.common",                 "urn:uuid:1077efec-c0b2-4d02-ace3-3c1e52e2fb4b"},
};

const std::size_t kAliasCount = sizeof(kAliases) / sizeof(kAliases[0]);

const char kUuidPrefix[] = "urn:uuid:";
const std::size_t kUuidPrefixLen = 9;

std::string trimAndLower(const std::string &in)
{
    std::size_t begin = 0;
    std::size_t end = in.size();

    while (begin < end && std::isspace(static_cast<unsigned char>(in[begin])) != 0) {
        ++begin;
    }

    while (end > begin && std::isspace(static_cast<unsigned char>(in[end - 1])) != 0) {
        --end;
    }

    std::string out;
    out.reserve(end - begin);

    for (std::size_t i = begin; i < end; ++i) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(in[i]))));
    }

    return out;
}

bool isHexString(const std::string &s)
{
    if (s.empty()) {
        return false;
    }

    for (std::size_t i = 0; i < s.size(); ++i) {
        if (std::isxdigit(static_cast<unsigned char>(s[i])) == 0) {
            return false;
        }
    }

    return true;
}

/** 36 个字符、连字符固定在 8/13/18/23 位 —— 只认这个形状。 */
bool looksLikeDashedUuid(const std::string &s)
{
    if (s.size() != 36) {
        return false;
    }

    static const std::size_t kDashPositions[] = {8, 13, 18, 23};

    for (std::size_t i = 0; i < 4; ++i) {
        if (s[kDashPositions[i]] != '-') {
            return false;
        }
    }

    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '-') {
            continue;
        }

        if (std::isxdigit(static_cast<unsigned char>(s[i])) == 0) {
            return false;
        }
    }

    return true;
}

/** 32 个 hex 字符 → 8-4-4-4-12 的连字符形式（RFC 4122 的文本表示）。 */
std::string insertUuidDashes(const std::string &hex32)
{
    static const std::size_t kGroupLengths[] = {8, 4, 4, 4, 12};
    std::string out;
    out.reserve(36);
    std::size_t pos = 0;

    for (std::size_t g = 0; g < 5; ++g) {
        if (g != 0) {
            out.push_back('-');
        }

        out.append(hex32, pos, kGroupLengths[g]);
        pos += kGroupLengths[g];
    }

    return out;
}

/** 去掉一层花括号（清单/EME 里 `{uuid}` 的写法也出现过）。 */
std::string stripBraces(const std::string &s)
{
    if (s.size() >= 2 && s[0] == '{' && s[s.size() - 1] == '}') {
        return s.substr(1, s.size() - 2);
    }

    return s;
}

const DrmSchemes::Entry *findEntry(const std::string &canonicalScheme)
{
    if (canonicalScheme.empty()) {
        return nullptr;
    }

    for (std::size_t i = 0; i < kEntryCount; ++i) {
        if (canonicalScheme == kEntries[i].schemeIdUri) {
            return &kEntries[i];
        }
    }

    return nullptr;
}

bool hasUuidSchemePrefix(const std::string &canonicalScheme)
{
    return canonicalScheme.compare(0, kUuidPrefixLen, kUuidPrefix) == 0;
}

} // namespace

std::string DrmSchemes::canonical(const std::string &schemeIdUri)
{
    const std::string s = trimAndLower(schemeIdUri);

    if (s.empty()) {
        return s;
    }

    // 1) urn:uuid:<uuid>（大小写已被统一；花括号与 32-hex 这两种变体也在这里收口）
    if (s.compare(0, kUuidPrefixLen, kUuidPrefix) == 0) {
        const std::string body = stripBraces(s.substr(kUuidPrefixLen));

        if (looksLikeDashedUuid(body)) {
            return std::string(kUuidPrefix) + body;
        }

        if (body.size() == 32 && isHexString(body)) {
            return std::string(kUuidPrefix) + insertUuidDashes(body);
        }

        // 认不出来的 urn:uuid: 也**不改写**：上游拿到什么就是什么，
        // 下游按"未知方案"处理，绝不猜成某个已知系统。
        return s;
    }

    // 2) 裸 UUID / 裸 32-hex（有些工具会把前缀省掉）
    {
        const std::string bare = stripBraces(s);

        if (looksLikeDashedUuid(bare)) {
            return std::string(kUuidPrefix) + bare;
        }

        if (bare.size() == 32 && isHexString(bare)) {
            return std::string(kUuidPrefix) + insertUuidDashes(bare);
        }
    }

    // 3) EME / KEYFORMAT 别名
    for (std::size_t i = 0; i < kAliasCount; ++i) {
        if (s == kAliases[i].name) {
            return kAliases[i].schemeIdUri;
        }
    }

    // 4) 其余（urn:mpeg:dash:… 等）原样返回
    return s;
}

std::string DrmSchemes::systemIdOf(const std::string &schemeIdUri)
{
    const std::string scheme = canonical(schemeIdUri);

    if (!hasUuidSchemePrefix(scheme)) {
        return std::string();
    }

    return scheme.substr(kUuidPrefixLen);
}

std::string DrmSchemes::nameOf(const std::string &schemeIdUri)
{
    const Entry *entry = findEntry(canonical(schemeIdUri));

    if (entry == nullptr) {
        return std::string();
    }

    return std::string(entry->name);
}

DrmSchemes::Kind DrmSchemes::kindOf(const std::string &schemeIdUri)
{
    const std::string scheme = canonical(schemeIdUri);
    const Entry *entry = findEntry(scheme);

    if (entry != nullptr) {
        return entry->kind;
    }

    // 没登记的 urn:uuid: 仍然是"指名了某个 DRM 系统"，这一点不能丢。
    return hasUuidSchemePrefix(scheme) ? Kind::System : Kind::Unknown;
}

bool DrmSchemes::isKnown(const std::string &schemeIdUri)
{
    return findEntry(canonical(schemeIdUri)) != nullptr;
}

bool DrmSchemes::isCencCapable(const std::string &schemeIdUri)
{
    const std::string scheme = canonical(schemeIdUri);
    const Entry *entry = findEntry(scheme);

    if (entry != nullptr) {
        return entry->cencCapable;
    }

    /*
     * 表里没有的方案：形如 urn:uuid:<uuid> 的按"是 CENC 内容"处理。
     * 理由（不是猜测）：DASH 里用 DRM 系统 UUID 指名的 Representation，其媒体段
     * 就是按 ISO/IEC 23001-7 封装的 —— 这是 schemeIdUri="urn:uuid:…" 这个写法的
     * **定义**（DASH-IF 登记表开头那段：UUID 表示 PSSH 的 SystemID）。
     * 所以新出现、本表还没登记的 DRM 系统也该走同一条软解路，而不是被当成"看不懂"。
     */
    return hasUuidSchemePrefix(scheme);
}

std::size_t DrmSchemes::count()
{
    return kEntryCount;
}

const DrmSchemes::Entry &DrmSchemes::at(std::size_t index)
{
    static const Entry kEmpty = {"", "", "", Kind::Unknown, false};

    if (index >= kEntryCount) {
        return kEmpty;
    }

    return kEntries[index];
}

bool DrmSchemes::isPlatformSupported(const Candidate &candidate)
{
    const std::string scheme = canonical(candidate.schemeIdUri);

    if (scheme.empty()) {
        return false;
    }

    DrmInfo probe{};
    probe.format = scheme;
    probe.uri = candidate.licenseUrl;
    probe.pssh = candidate.pssh;
    probe.keyId = candidate.keyId;

    return DrmHandlerPrototype::isSupport(&probe);
}

DrmSchemes::Decision DrmSchemes::decide(const std::vector<Candidate> &candidates)
{
    Decision best;

    if (candidates.empty()) {
        return best;
    }

    long bestScore = LONG_MIN;

    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const std::string scheme = canonical(candidates[i].schemeIdUri);

        if (scheme.empty()) {
            continue;
        }

        const bool cenc = isCencCapable(scheme);
        const bool platform = isPlatformSupported(candidates[i]);
        const bool named = kindOf(scheme) == Kind::System;
        const bool hasUrl = !candidates[i].licenseUrl.empty();

        /*
         * 排序判据（与 DrmSchemes.h 的声明一一对应）。刻意把"带许可地址"排在
         * "指了具体 DRM 系统"之前：在没有 CDM 的平台上，能取到密钥比"名字好听"重要 ——
         * 一个只有 mp4protection + laurl 的清单是**能软解**的，而一个指名 Widevine
         * 却没有 laurl 的清单在没有 CDM 的平台上**不能**。
         */
        long score = 0;

        if (!cenc) {
            score -= 100000;    // 内核解不了这个方案（HLS Clear Key / CA / SEA）
        }

        if (platform) {
            score += 100000;    // 本平台 CDM 认它 ⇒ 硬解优先
        }

        if (hasUrl) {
            score += 1000;      // 软解兜底要它
        }

        if (named) {
            score += 100;       // 指名了具体 DRM 系统（给解码器的 keyFormat 才有个真 UUID）
        }

        if (cenc) {
            score += 10;
        }

        score += static_cast<long>(i);   // 同分时后声明的赢（Rep 覆盖 AS）

        if (score > bestScore) {
            bestScore = score;
            best.index = static_cast<int>(i);
            best.schemeIdUri = scheme;
            best.systemId = systemIdOf(scheme);
            best.name = nameOf(scheme);
            best.licenseUrl = candidates[i].licenseUrl;
            best.pssh = candidates[i].pssh;
            best.keyId = candidates[i].keyId;
        }
    }

    if (best.index < 0) {
        return best;
    }

    best.handledByPlatform = isPlatformSupported(candidates[static_cast<std::size_t>(best.index)]);

    /*
     * usable 的定义：**要么平台 CDM 会解，要么清单给了可取密钥的地址**（且内容确实是 CENC）。
     * 刻意不把"有 KID"算进来：KID 常常只写在 init 段的 tenc 里，要等解复用器打开之后
     * 才知道，在这里判会把"其实能播"的流误判成不能播。缺 KID 由注册密钥那一步单独报错。
     */
    best.usable = best.handledByPlatform ||
                  (isCencCapable(best.schemeIdUri) && !best.licenseUrl.empty());

    return best;
}
