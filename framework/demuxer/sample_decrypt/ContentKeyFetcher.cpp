//
// ContentKeyFetcher.cpp
//
// 见 ContentKeyFetcher.h。这里只有两段逻辑：读 URL（沿用本工程既有的 IDataSource），
// 以及把响应体解析成 16 字节密钥（纯函数，可被自检工具直接调用）。
//

#define LOG_TAG "ContentKeyFetcher"

#include "ContentKeyFetcher.h"

#include <utils/frame_work_log.h>

#include <cctype>
#include <cstring>
#include <vector>

using namespace Cicada;

namespace {

/** 一次最多读这么多。许可证 JSON 是十几到几百字节，64 KiB 已经宽到不可能误伤。 */
const int kMaxKeyResponseBytes = 64 * 1024;

int hexValue(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }

    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }

    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }

    return -1;
}

bool isSpaceChar(char c)
{
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

/** 把 `0x` + 32 hex（或裸 32 hex）解成 16 字节。不是这个形状就返回 false。 */
bool decodeHex16(const std::string &text, uint8_t *out)
{
    std::size_t begin = 0;
    std::size_t end = text.size();

    while (begin < end && isSpaceChar(text[begin])) {
        ++begin;
    }

    while (end > begin && isSpaceChar(text[end - 1])) {
        --end;
    }

    if (end - begin >= 2 && text[begin] == '0' && (text[begin + 1] == 'x' || text[begin + 1] == 'X')) {
        begin += 2;
    }

    if (end - begin != 32) {
        return false;
    }

    for (int i = 0; i < 16; ++i) {
        const int high = hexValue(text[begin + static_cast<std::size_t>(i) * 2]);
        const int low = hexValue(text[begin + static_cast<std::size_t>(i) * 2 + 1]);

        if (high < 0 || low < 0) {
            return false;
        }

        out[i] = static_cast<uint8_t>((high << 4) | low);
    }

    return true;
}

/**
 * base64 / base64url 解码。**两种字母表都接受**：
 * W3C ClearKey 规定用 base64url（`-`/`_`，无填充），但实际服务端也常直接发标准
 * base64（`+`/`/`，带 `=` 填充）。16 字节的密钥编码出来 24 个字符（含 2 个 `=`），
 * 所以两种写法都会出现，认死一种就会在真服务端上翻车。
 * 只解到一个 16 字节密钥为止；解出来不是 16 字节由调用方判定。
 */
bool decodeBase64(const std::string &text, std::vector<uint8_t> &out)
{
    out.clear();
    int accumulator = 0;
    int bits = 0;

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];

        if (isSpaceChar(c) || c == '"') {
            continue;
        }

        if (c == '=') {
            break;      // 填充：后面不会再有有效数据
        }

        int value = -1;

        if (c >= 'A' && c <= 'Z') {
            value = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            value = c - 'a' + 26;
        } else if (c >= '0' && c <= '9') {
            value = c - '0' + 52;
        } else if (c == '+' || c == '-') {
            value = 62;
        } else if (c == '/' || c == '_') {
            value = 63;
        } else {
            return false;
        }

        accumulator = (accumulator << 6) | value;
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xFF));
        }
    }

    return true;
}

/**
 * 在 JSON 文本里找 `"<key>":"<value>"` 并返回 value 的原始文本。
 *
 * 【为什么手写而不是引 JSON 库】这一层的输入只有十几到几百字节、结构固定，
 * 而要取的东西只有两个短字符串；引一个 JSON 依赖会让 framework_demuxer 多一层
 * 与清单解析无关的耦合。手写的风险点是"把 `"k"` 误匹配到 `"kid"`"，所以这里
 * 要求匹配到的键名**后面紧跟引号**（`"k"` 而不是 `"k` 开头），
 * 于是 `"kid"` / `"kty"` / `"keys"` 都不会命中 —— 这一点由自检工具覆盖。
 */
bool findJsonString(const std::string &json, const char *key, std::string &value)
{
    const std::string token = std::string("\"") + key + "\"";
    std::size_t pos = 0;

    while ((pos = json.find(token, pos)) != std::string::npos) {
        std::size_t cursor = pos + token.size();

        while (cursor < json.size() && isSpaceChar(json[cursor])) {
            ++cursor;
        }

        if (cursor >= json.size() || json[cursor] != ':') {
            pos += token.size();
            continue;
        }

        ++cursor;

        while (cursor < json.size() && isSpaceChar(json[cursor])) {
            ++cursor;
        }

        if (cursor >= json.size() || json[cursor] != '"') {
            pos += token.size();
            continue;
        }

        ++cursor;
        value.clear();

        while (cursor < json.size() && json[cursor] != '"') {
            if (json[cursor] == '\\' && cursor + 1 < json.size()) {
                ++cursor;   // 转义：原样收下被转义的字符（密钥/许可证里不会出现，但别把解析弄断）
            }

            value.push_back(json[cursor]);
            ++cursor;
        }

        return true;
    }

    return false;
}

bool parseJsonKey(const std::string &json, uint8_t *out)
{
    std::string value;

    // W3C ClearKey：{"keys":[{"kty":"oct","k":"<base64url>","kid":"…"}],…}
    if (findJsonString(json, "k", value)) {
        if (decodeHex16(value, out)) {
            return true;
        }

        std::vector<uint8_t> decoded;

        if (decodeBase64(value, decoded) && decoded.size() == 16) {
            memcpy(out, decoded.data(), 16);
            return true;
        }

        return false;
    }

    // 本工程密钥服务器的 JSON 视图：{"kid":"…","key":"<32 hex>","scheme":"cenc",…}
    if (findJsonString(json, "key", value)) {
        if (decodeHex16(value, out)) {
            return true;
        }

        std::vector<uint8_t> decoded;

        if (decodeBase64(value, decoded) && decoded.size() == 16) {
            memcpy(out, decoded.data(), 16);
            return true;
        }
    }

    return false;
}

std::string trimToText(const uint8_t *data, int size)
{
    std::size_t begin = 0;
    std::size_t end = static_cast<std::size_t>(size);

    // UTF-8 BOM（有些服务端会带）先跳掉，否则下面看不到开头的 `{`。
    if (end >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        begin = 3;
    }

    while (begin < end && isSpaceChar(static_cast<char>(data[begin]))) {
        ++begin;
    }

    while (end > begin && isSpaceChar(static_cast<char>(data[end - 1]))) {
        --end;
    }

    return std::string(reinterpret_cast<const char *>(data) + begin, end - begin);
}

} // namespace

ContentKeyFetcher::Result ContentKeyFetcher::parse(const uint8_t *data, int size, const char *what)
{
    Result result;
    const std::string source = (what == nullptr) ? std::string("key response") : std::string(what);

    if (data == nullptr || size <= 0) {
        result.status = Result::Status::NotFetched;
        result.detail = source + " is empty";
        return result;
    }

    // 形状 1：正好 16 字节的裸密钥（本工程密钥服务器 /key/{kid} 的默认响应，
    // 也是 HLS #EXT-X-KEY URI 必须返回的东西）。
    if (size == 16) {
        memcpy(result.key, data, 16);
        result.keySize = 16;
        result.status = Result::Status::Ok;
        result.detail = source + ": raw 16-byte content key";
        return result;
    }

    // 形状 2/3：JSON 许可证。
    const std::string text = trimToText(data, size);

    if (!text.empty() && text[0] == '{') {
        if (parseJsonKey(text, result.key)) {
            result.keySize = 16;
            result.status = Result::Status::Ok;
            result.detail = source + ": JSON licence containing a 16-byte content key";
            return result;
        }

        result.status = Result::Status::Malformed;
        result.detail = source + " is a JSON licence but carries no usable 16-byte key "
                        "(looked for \"k\" (base64url/hex) and \"key\" (hex/base64))";
        return result;
    }

    result.status = Result::Status::Unrecognized;
    result.detail = source + " is neither a raw 16-byte key nor a JSON licence (" +
                    std::to_string(size) + " bytes)";
    return result;
}

ContentKeyFetcher::Result ContentKeyFetcher::fetch(const std::string &url,
        const options *opts,
        IDataSource::SourceConfig &config)
{
    Result result;

    if (url.empty()) {
        result.status = Result::Status::NotFetched;
        result.detail = "no key/licence URL was declared";
        return result;
    }

    IDataSource *source = dataSourcePrototype::create(url, opts);

    if (source == nullptr) {
        result.status = Result::Status::NotFetched;
        result.detail = "no data source can open " + url;
        return result;
    }

    source->Set_config(config);
    const int openRet = source->Open(0);

    if (openRet < 0) {
        result.status = Result::Status::NotFetched;
        result.detail = "cannot open " + url;
        AF_LOGE("cannot fetch the content key from %s (ret=%d)\n", url.c_str(), openRet);
        delete source;
        return result;
    }

    std::vector<uint8_t> body;
    body.reserve(1024);

    while (static_cast<int>(body.size()) < kMaxKeyResponseBytes) {
        uint8_t chunk[1024];
        const int len = source->Read(chunk, static_cast<size_t>(sizeof(chunk)));

        if (len <= 0) {
            break;      // 0 = 正常 EOF（与 HLSStream::updateKey 同一口径）
        }

        body.insert(body.end(), chunk, chunk + len);
    }

    source->Close();
    delete source;

    result = parse(body.empty() ? nullptr : body.data(), static_cast<int>(body.size()), url.c_str());

    if (result.status != Result::Status::Ok) {
        AF_LOGE("%s\n", result.detail.c_str());
    }

    return result;
}
