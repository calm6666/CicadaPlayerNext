#include <Danmaku/DanmakuXml.h>

#include <cstdlib>
#include <cstring>

namespace {

/* ---------------------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------------------- */

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* 跳过空白 */
size_t skipSpace(const std::string &s, size_t i) {
    while (i < s.size() && isSpace(s[i]))
        ++i;
    return i;
}

/* 大小写无关地比较一个字符 */
bool lowerIs(char c, char want) {
    if (c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');
    return c == want;
}

/*
 * 在 [from, to) 里找子串（大小写无关）。找不到返回 std::string::npos。
 * <d 标签的属性名都是小写，但为了容错还是按大小写无关比较。
 */
size_t findNoCase(const std::string &s, size_t from, size_t to, const char *needle) {
    const size_t n = std::strlen(needle);

    if (n == 0 || to > s.size() || from + n > to)
        return std::string::npos;

    for (size_t i = from; i + n <= to; ++i) {
        size_t k = 0;

        while (k < n && lowerIs(s[i + k], needle[k]))
            ++k;

        if (k == n)
            return i;
    }

    return std::string::npos;
}

/* 取一个属性的值：key="value" / key='value'（只支持双引号，B 站的 XML 就是双引号） */
bool attributeValue(const std::string &s, size_t from, size_t to, const char *key, std::string *out) {
    const size_t at = findNoCase(s, from, to, key);

    if (at == std::string::npos)
        return false;

    size_t i = skipSpace(s, at + std::strlen(key));

    if (i >= to || s[i] != '=')
        return false;

    ++i;
    i = skipSpace(s, i);

    if (i >= to)
        return false;

    const char quote = s[i];

    if (quote != '"' && quote != '\'')
        return false;

    ++i;

    const size_t begin = i;

    while (i < to && s[i] != quote)
        ++i;

    if (out != nullptr)
        out->assign(s, begin, i - begin);

    return true;
}

/* UTF-8 文件里的 &amp; 这类实体还原 */
std::string decodeEntities(const std::string &in) {
    std::string out;
    out.reserve(in.size());

    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '&') {
            out.push_back(in[i]);
            continue;
        }

        const size_t semi = in.find(';', i + 1);

        if (semi == std::string::npos || semi - i > 10) {
            out.push_back('&');
            continue;
        }

        const std::string name = in.substr(i + 1, semi - i - 1);

        if (name == "amp") {
            out.push_back('&');
        } else if (name == "lt") {
            out.push_back('<');
        } else if (name == "gt") {
            out.push_back('>');
        } else if (name == "quot") {
            out.push_back('"');
        } else if (name == "apos") {
            out.push_back('\'');
        } else if (!name.empty() && name[0] == '#') {
            /* &#NNNN;（十进制）或 &#xHH;（十六进制）：转回 UTF-8 */
            unsigned long code = 0;
            bool ok = false;

            if (name.size() > 2 && (name[1] == 'x' || name[1] == 'X'))
                code = std::strtoul(name.c_str() + 2, nullptr, 16);
            else
                code = std::strtoul(name.c_str() + 1, nullptr, 10);

            ok = (code > 0 && code <= 0x10FFFF);

            if (ok && code < 0x80) {
                out.push_back((char)code);
            } else if (ok && code < 0x800) {
                out.push_back((char)(0xC0 | (code >> 6)));
                out.push_back((char)(0x80 | (code & 0x3F)));
            } else if (ok && code < 0x10000) {
                out.push_back((char)(0xE0 | (code >> 12)));
                out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
                out.push_back((char)(0x80 | (code & 0x3F)));
            } else if (ok) {
                out.push_back((char)(0xF0 | (code >> 18)));
                out.push_back((char)(0x80 | ((code >> 12) & 0x3F)));
                out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
                out.push_back((char)(0x80 | (code & 0x3F)));
            } else {
                out.push_back('&');
                out.append(name);
                out.push_back(';');
            }
        } else {
            /* 不认识的实体：原样留着，别把用户的文本吃掉 */
            out.push_back('&');
            out.append(name);
            out.push_back(';');
        }

        i = semi;
    }

    return out;
}

/* 把 "a,b,c" 拆成字段（不 trim，调用方自己处理） */
std::vector<std::string> splitCommas(const std::string &s) {
    std::vector<std::string> parts;
    size_t start = 0;

    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ',') {
            parts.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }

    return parts;
}

std::string trim(const std::string &s) {
    size_t b = 0;
    size_t e = s.size();

    while (b < e && isSpace(s[b]))
        ++b;

    while (e > b && isSpace(s[e - 1]))
        --e;

    return s.substr(b, e - b);
}

/*
 * B 站 XML 的 <d> 有两种写法（都要吃下）：
 *   <d p="...">文本</d>
 *   <d p="..."/>              ← 空内容，没有文本，丢掉
 */
bool parseOneD(const std::string &xml, size_t tagBegin, size_t tagEnd, size_t contentEnd,
               DanmakuItem *out, int64_t id) {
    std::string p;

    if (!attributeValue(xml, tagBegin, tagEnd, "p", &p))
        return false;

    const std::vector<std::string> f = splitCommas(p);

    if (f.size() < 4)
        return false;

    const double timeSec = std::atof(trim(f[0]).c_str());
    const int mode = std::atoi(trim(f[1]).c_str());
    const int fontSize = std::atoi(trim(f[2]).c_str());
    const unsigned long rgb = std::strtoul(trim(f[3]).c_str(), nullptr, 10);

    int type = 0;

    switch (mode) {
        case 1:
        case 2:
        case 3:
        case 6:             /* 逆向滚动：按普通滚动处理（两套参考实现都没有逆向） */
            type = DanmakuTypeScroll;
            break;
        case 4:
            type = DanmakuTypeBottom;
            break;
        case 5:
            type = DanmakuTypeTop;
            break;
        default:            /* 7 高级 / 8 代码 / 其它：丢弃 */
            return false;
    }

    /*
     * 文本在标签结束符之后、</d> 之前（自闭合标签时 contentEnd == tagEnd，文本为空）。
     *
     * 【★ 这里错过一次：必须从 tagEnd + 1 开始 ★】
     * tagEnd 是调用方用 `xml.find('>', lt)` 得到的，指向的就是那个 **'>' 字符本身**。
     * 老代码写 `xml.substr(tagEnd, contentEnd - tagEnd)` —— 于是每条弹幕的文本都
     * 白白带上一个前导 '>'（用户实测："每条弹幕前面有 > 这个符号"），
     * 而且 trim() 不会去掉它（'>' 不是空白）。所以起点是 tagEnd + 1，
     * 长度也要跟着减 1。
     */
    std::string text;

    if (contentEnd > tagEnd + 1)
        text = xml.substr(tagEnd + 1, contentEnd - tagEnd - 1);

    text = trim(decodeEntities(text));

    if (text.empty())
        return false;

    if (timeSec < 0.0)
        return false;

    out->id = id;
    out->text = text;
    out->timeMs = (int64_t)(timeSec * 1000.0 + 0.5);
    out->type = type;
    out->fontSize = (fontSize > 0) ? fontSize : 0;          /* 0 = 用配置算 */
    out->speed = 0;                                        /* 0 = 用配置的默认档 */
    out->colorRGBA = ((uint32_t)(rgb & 0xFFFFFFu) << 8) | 0xFFu;
    /* uid 是第 7 个字段；"1" 或空 = 本人（引擎也认 uid == "1"） */
    out->uid = (f.size() > 6) ? trim(f[6]) : std::string();
    out->isSelf = (out->uid == "1");
    return true;
}

template <typename Sink>
int scanXml(const std::string &xml, Sink sink) {
    int count = 0;
    size_t i = 0;

    while (i < xml.size()) {
        /* 找下一个 <d */
        const size_t lt = xml.find("<d", i);

        if (lt == std::string::npos)
            break;

        /* 必须是 <d 后面跟空白或 '>'，别把 <duration> 这种当成弹幕 */
        const char after = (lt + 2 < xml.size()) ? xml[lt + 2] : '\0';

        if (!(after == ' ' || after == '\t' || after == '\r' || after == '\n' || after == '>'
              || after == '/')) {
            i = lt + 2;
            continue;
        }

        const size_t tagEnd = xml.find('>', lt);

        if (tagEnd == std::string::npos)
            break;

        const bool selfClosing = (tagEnd > lt && xml[tagEnd - 1] == '/');
        size_t contentEnd = tagEnd;

        if (!selfClosing) {
            contentEnd = xml.find("</d", tagEnd);

            if (contentEnd == std::string::npos)
                contentEnd = tagEnd;                 /* 半个文件也不崩，当空内容处理 */
        }

        DanmakuItem item;

        if (parseOneD(xml, lt, tagEnd, contentEnd, &item, 1000 + count)) {
            sink(item);
            ++count;
        }

        i = (contentEnd > tagEnd) ? contentEnd : (tagEnd + 1);
    }

    return count;
}

} // namespace

std::vector<DanmakuItem> danmakuParseBilibiliXml(const std::string &xml) {
    std::vector<DanmakuItem> out;

    try {
        scanXml(xml, [&out](const DanmakuItem &item) { out.push_back(item); });
    } catch (...) {
        /* 解析绝不让异常外泄：已经解析到的照常返回 */
    }

    return out;
}

int danmakuCountBilibiliXml(const std::string &xml) {
    int count = 0;

    try {
        count = scanXml(xml, [](const DanmakuItem &) {});
    } catch (...) {
        /* 同上 */
    }

    return count;
}
