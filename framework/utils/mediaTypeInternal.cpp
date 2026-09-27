//
// Created by moqi on 2019-08-20.
//

#include "mediaTypeInternal.h"
#include "mediaFrame.h"
#include <cstring>

streamMeta::streamMeta(const Stream_meta *meta)
{
    mMeta = *meta;
}

streamMeta::~streamMeta()
{
    releaseMeta(&mMeta);
}

streamMeta::operator Stream_meta *()
{
    return &mMeta;
}

/*
 * ================== 编码短名归一化 ==================
 *
 * 为什么放在这个文件：AFMediaType.h 是**只有头文件**的类型定义（AFCodecID /
 * Stream_meta 都在这里），工程里没有 AFMediaType.cpp；而 mediaTypeInternal.cpp
 * 就是与它配套的那个实现文件（已在 framework/utils/CMakeLists.txt:54 编进
 * framework_utils），改它不需要动任何构建脚本。
 *
 * 只输出 6 个短名：H.264 / H.265 / AV1 / VP9 / MPEG-4 / MPEG-2。
 * 认不出来就返回空串 —— 界面显示"没有徽标"，不允许猜。
 */

namespace {

    /* RFC 6381 的 token 里可能出现的字符：字母数字和 . - _ + 。
     * 其它字符（两端的双引号、空白、逗号）在归一化时直接丢掉。 */
    bool afCodecTokenChar(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == '+';
    }

    char afCodecLower(char c)
    {
        if (c >= 'A' && c <= 'Z') {
            return (char) (c - 'A' + 'a');
        }

        return c;
    }

    /* 取一个 token 的小写规范形式：只保留 afCodecTokenChar 允许的字符并转小写。
     * 于是 "avc1.64001f"、"\"avc1.64001f\""、" AVC1.64001F " 归一化后完全一样。 */
    void afCodecNormalizeToken(const char *begin, size_t length, char *out, size_t outSize)
    {
        size_t n = 0;

        for (size_t i = 0; i < length && n + 1 < outSize; i++) {
            if (afCodecTokenChar(begin[i])) {
                out[n++] = afCodecLower(begin[i]);
            }
        }

        out[n] = '\0';
    }

    bool afCodecTokenHasPrefix(const char *token, const char *prefix)
    {
        return strncmp(token, prefix, strlen(prefix)) == 0;
    }

    /*
     * 一个归一化后的 token -> AFCodecID（只认视频编码）。
     *
     * 前缀表按 RFC 6381 / DASH-IF 的通用写法整理：
     *   H.265 : hvc1.* / hev1.* / hvc2.* / hev2.* / hvc / hevc
     *   H.264 : avc1.* / avc2.* / avc3.* / avc4.* / avc（h264/x264 是编码器名，一并认）
     *   AV1   : av01.* / av1
     *   VP9   : vp09.* / vp9
     *   MPEG-4: mp4v.* / mpeg4 / m4v / xvid / divx
     *   MPEG-2: mp2v.* / mpeg2* / mpg2 / m2v
     *
     * 注意 mp4a（AAC 音频）**不能**被 mp4v 的前缀吃到，所以两边都是 4 字符精确前缀；
     * 音频 token（mp4a / ac-3 / ec-3 / opus / flac / alac / dts* / vorbis ...）
     * 在这里一律返回 NONE，由调用方跳过。
     */
    enum AFCodecID afCodecIDFromToken(const char *token)
    {
        if (token == nullptr || token[0] == '\0') {
            return AF_CODEC_ID_NONE;
        }

        if (afCodecTokenHasPrefix(token, "hvc") || afCodecTokenHasPrefix(token, "hev") ||
            afCodecTokenHasPrefix(token, "hevc")) {
            return AF_CODEC_ID_HEVC;
        }

        if (afCodecTokenHasPrefix(token, "avc") || afCodecTokenHasPrefix(token, "h264") ||
            afCodecTokenHasPrefix(token, "x264")) {
            return AF_CODEC_ID_H264;
        }

        if (afCodecTokenHasPrefix(token, "av01") || afCodecTokenHasPrefix(token, "av1")) {
            return AF_CODEC_ID_AV1;
        }

        if (afCodecTokenHasPrefix(token, "vp09") || afCodecTokenHasPrefix(token, "vp9")) {
            return AF_CODEC_ID_VP9;
        }

        /* 顺序要紧：mpeg4 必须排在 mpeg2 之前判断（前缀不同，但可读性上先窄后宽）。 */
        if (afCodecTokenHasPrefix(token, "mp4v") || afCodecTokenHasPrefix(token, "mpeg4") ||
            afCodecTokenHasPrefix(token, "m4v") || afCodecTokenHasPrefix(token, "xvid") ||
            afCodecTokenHasPrefix(token, "divx")) {
            return AF_CODEC_ID_MPEG4;
        }

        /*
         * MPEG-2：AFCodecID 里没有对应枚举项（AF_CODEC_ID_MPEG2VIDEO 在
         * AFMediaType.h 里是注释掉的，加进去会移动后面所有枚举值），所以这里
         * 只把它归一化成短名 "MPEG-2"，拿不到 AFCodecID。
         */
        if (afCodecTokenHasPrefix(token, "mp2v") || afCodecTokenHasPrefix(token, "mpeg2") ||
            afCodecTokenHasPrefix(token, "mpg2") || afCodecTokenHasPrefix(token, "m2v")) {
            return AF_CODEC_ID_NONE;
        }

        return AF_CODEC_ID_NONE;
    }

    /* 原始 token（未归一化）是否是 MPEG-2：afCodecIDFromToken 给不出 id，短名那层
     * 要单独认它一次。 */
    bool afCodecTokenIsMpeg2(const char *token)
    {
        return afCodecTokenHasPrefix(token, "mp2v") || afCodecTokenHasPrefix(token, "mpeg2") ||
               afCodecTokenHasPrefix(token, "mpg2") || afCodecTokenHasPrefix(token, "m2v");
    }

    const char *afCodecShortNameById(enum AFCodecID id)
    {
        switch (id) {
            case AF_CODEC_ID_H264:
                return "H.264";

            case AF_CODEC_ID_HEVC:
                return "H.265";

            case AF_CODEC_ID_AV1:
                return "AV1";

            case AF_CODEC_ID_VP9:
                return "VP9";

            case AF_CODEC_ID_MPEG4:
                return "MPEG-4";

            default:
                return "";
        }
    }

    /* 在逗号分隔的 token 列表里找**第一个能识别的视频编码**。
     * 由 rawCodecs 得到短名的完整对照见 afCodecShortName()。 */
    const char *afCodecShortNameFromManifestCodecs(const char *rawCodecs)
    {
        if (rawCodecs == nullptr) {
            return "";
        }

        const char *p = rawCodecs;
        char token[64];

        while (*p != '\0') {
            const char *comma = strchr(p, ',');
            const size_t length = (comma != nullptr) ? (size_t) (comma - p) : strlen(p);
            afCodecNormalizeToken(p, length, token, sizeof(token));

            if (afCodecTokenIsMpeg2(token)) {
                return "MPEG-2";
            }

            const char *name = afCodecShortNameById(afCodecIDFromToken(token));

            if (name[0] != '\0') {
                return name;
            }

            if (comma == nullptr) {
                break;
            }

            p = comma + 1;
        }

        return "";
    }

}// namespace

const char *afCodecShortName(enum AFCodecID id, const char *rawCodecs)
{
    const char *name = afCodecShortNameById(id);

    if (name[0] != '\0') {
        return name;
    }

    return afCodecShortNameFromManifestCodecs(rawCodecs);
}

enum AFCodecID afCodecIDFromManifestCodecs(const char *rawCodecs)
{
    if (rawCodecs == nullptr) {
        return AF_CODEC_ID_NONE;
    }

    const char *p = rawCodecs;
    char token[64];

    while (*p != '\0') {
        const char *comma = strchr(p, ',');
        const size_t length = (comma != nullptr) ? (size_t) (comma - p) : strlen(p);
        afCodecNormalizeToken(p, length, token, sizeof(token));

        const enum AFCodecID id = afCodecIDFromToken(token);

        if (id != AF_CODEC_ID_NONE) {
            return id;
        }

        if (comma == nullptr) {
            break;
        }

        p = comma + 1;
    }

    return AF_CODEC_ID_NONE;
}

int afCodecEfficiencyRankByShortName(const char *shortName)
{
    if (shortName == nullptr) {
        return 0;
    }

    if (strcmp(shortName, "AV1") == 0) {
        return 5;
    }

    if (strcmp(shortName, "H.265") == 0) {
        return 4;
    }

    if (strcmp(shortName, "VP9") == 0) {
        return 3;
    }

    if (strcmp(shortName, "H.264") == 0) {
        return 2;
    }

    if (strcmp(shortName, "MPEG-4") == 0 || strcmp(shortName, "MPEG-2") == 0) {
        return 1;
    }

    return 0;
}

int afCodecEfficiencyRank(enum AFCodecID id, const char *rawCodecs)
{
    return afCodecEfficiencyRankByShortName(afCodecShortName(id, rawCodecs));
}

namespace {

    /*
     * 内核认得的全部视频编码短名（就是 afCodecShortNameById 的值域），**只此一份**。
     * 这里的书写次序没有含义 —— 效率序由下面的 AfCodecEfficiencyOrder 按等级表排出来。
     */
    const char *const kAfCodecShortNames[] = {
        "H.264", "H.265", "AV1", "VP9", "MPEG-4", "MPEG-2",
    };

    /*
     * 首次调用时把 kAfCodecShortNames 按 afCodecEfficiencyRankByShortName() 的等级
     * **降序稳定排序**，存成 nullptr 结尾的数组。等级表只有那一份（就是这个函数），
     * 所以"效率序"在内核里只有一个定义点，这里不可能与它漂移。
     */
    struct AfCodecEfficiencyOrder {
        const char *names[sizeof(kAfCodecShortNames) / sizeof(kAfCodecShortNames[0]) + 1];

        AfCodecEfficiencyOrder()
        {
            const size_t count = sizeof(kAfCodecShortNames) / sizeof(kAfCodecShortNames[0]);

            for (size_t i = 0; i < count; i++) {
                names[i] = kAfCodecShortNames[i];
            }

            /* 插入排序（稳定，等级相同保持上面那张表的次序）。元素只有 6 个，
             * 没必要为此引 <algorithm>。 */
            for (size_t i = 1; i < count; i++) {
                const char *key = names[i];
                const int keyRank = afCodecEfficiencyRankByShortName(key);
                size_t j = i;

                while (j > 0 && afCodecEfficiencyRankByShortName(names[j - 1]) < keyRank) {
                    names[j] = names[j - 1];
                    j--;
                }

                names[j] = key;
            }

            names[count] = nullptr;
        }
    };

}// namespace

const char *const *afCodecEfficiencyOrder()
{
    /* C++11 起函数内静态量的初始化线程安全：第一次调用时构造一次，之后只读。 */
    static const AfCodecEfficiencyOrder order;
    return order.names;
}

enum AFCodecID afCodecIDFromShortName(const char *shortName)
{
    if (shortName == nullptr) {
        return AF_CODEC_ID_NONE;
    }

    if (strcmp(shortName, "H.264") == 0) {
        return AF_CODEC_ID_H264;
    }

    if (strcmp(shortName, "H.265") == 0) {
        return AF_CODEC_ID_HEVC;
    }

    if (strcmp(shortName, "AV1") == 0) {
        return AF_CODEC_ID_AV1;
    }

    if (strcmp(shortName, "VP9") == 0) {
        return AF_CODEC_ID_VP9;
    }

    if (strcmp(shortName, "MPEG-4") == 0) {
        return AF_CODEC_ID_MPEG4;
    }

    /*
     * "MPEG-2" 与未知短名一样落到这里：AFCodecID 里没有 MPEG-2（加了会移动后面
     * 所有枚举值），返回 AF_CODEC_ID_NONE 即"编码未知"。
     */
    return AF_CODEC_ID_NONE;
}
