//
// ContentKeyFetcher.h
//
// 从一个 URL 取回 **16 字节内容密钥**（CENC / ClearKey 的软解兜底用）。
//
// 【为什么单独抽出来】取密钥这件事有两个调用点（DASH 的 DashStream、HLS/对象化清单的
// HLSStream），而"取回来的东西到底是什么形状"至少有三类服务端行为：
//   1. **裸 16 字节**：`GET <url>` 直接返回响应体 = 密钥。本工程自己的密钥服务器
//      `/key/{kid}` 就是这个形状（HLS 的 #EXT-X-KEY URI 也必须是它）；
//   2. **W3C ClearKey 许可证**：EME / ClearKey 的标准响应是一个 JSON：
//      `{"keys":[{"kty":"oct","k":"<base64url>","kid":"<base64url>"}],"type":"temporary"}`。
//      base64url 是 **URL-safe** 字母表（`-`/`_`），不是标准 base64 的 `+`/`/`。
//   3. **本工程密钥服务器的 JSON 视图**：`?format=json` 返回
//      `{"kid":"…","key":"<32 hex>","scheme":"cenc","iv":"0x…"}`。
// 只认第 1 种的话，"ClearKey DASH"这条 DRM 就只能配一个"非标准"的密钥服务器；
// 只认第 2 种的话，本工程自己的裸字节端点又会失效。所以两种都认，且**只认这三种**，
// 认不出来就明确失败（绝不把半截字节当密钥去解，那就是"密文当明文"）。
//
// 【不做的事】不设重试、不设超时、不做缓存：取不到就是取不到，由调用方按既有错误
// 通路报出来（本工程的硬约束）。
//

#ifndef CICADA_PLAYER_CONTENTKEYFETCHER_H
#define CICADA_PLAYER_CONTENTKEYFETCHER_H

#include <cstdint>
#include <string>

#include "data_source/dataSourcePrototype.h"

namespace Cicada {

    class ContentKeyFetcher {
    public:
        /** 取密钥的结果。key 只在 status == Ok 时有效。 */
        struct Result {
            enum class Status {
                Ok = 0,
                /** 打不开 / 读不到（网络、权限、404 …）。 */
                NotFetched,
                /** 取到了内容，但它不是 16 字节，也不像任何一种已知的 JSON 形状。 */
                Unrecognized,
                /** 是 JSON，也找到了密钥字段，但编码/长度不对。 */
                Malformed,
            };

            Status status{Status::NotFetched};
            uint8_t key[16];
            int keySize{0};
            /** 人读的失败原因与识别到的形状。**只描述形状，绝不包含密钥内容**。 */
            std::string detail;

            Result()
            {
                for (int i = 0; i < 16; ++i) {
                    key[i] = 0;
                }
            }

            bool ok() const
            {
                return status == Status::Ok;
            }
        };

        /**
         * 取密钥。url 为空或打不开都返回 NotFetched 并在 detail 里说明。
         *
         * @param url     密钥/许可地址（清单里原样给出的那一条）
         * @param opts    播放器选项（与打开媒体用同一份，缓存/代理等配置要一致）
         * @param config  数据源配置（同上；调用方通常直接传本流的 mSourceConfig）
         */
        static Result fetch(const std::string &url,
                            const options *opts,
                            IDataSource::SourceConfig &config);

        /**
         * 把一段响应体解析成 16 字节密钥。**纯函数**，不碰网络 ——
         * 所以它能被自检工具直接喂各种形状的输入（见 tools/drm_bench/drm_schemes_check.cpp）。
         *
         * 可接受的形状（按判据顺序）：
         *   · 长度正好 16 的二进制 ⇒ 就是密钥；
         *   · 文本（去掉首尾空白后）以 `{` 开头 ⇒ 按 JSON 找密钥字段：
         *       `"k"`   —— base64url，或 32 个 hex 字符，或 `0x` + 32 hex；
         *       `"key"` —— 32 个 hex 字符，或 base64（本工程密钥服务器的 JSON 视图）。
         */
        static Result parse(const uint8_t *data, int size, const char *what);
    };

} // namespace Cicada

#endif // CICADA_PLAYER_CONTENTKEYFETCHER_H
