//
// Created by moqi on 2019-08-20.
//
#define LOG_TAG "decoderFactory"

#include <drm/DrmInfo.h>
#include "decoderFactory.h"
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <utils/AFMediaType.h>
#include <utils/cJSON.h>
#include <utils/frame_work_log.h>

#if defined(__APPLE__)
/* 判断要不要走 FFmpeg 的 videotoolbox 硬解（零拷贝用），见 property.h。 */
#include <utils/globalSettings.h>
#include <utils/property.h>
#endif

#ifdef ANDROID

    #include "Android/mediaCodecDecoder.h"

#endif

#ifdef __APPLE__

    #include "Apple/AppleVideoToolBox.h"

#endif

#ifdef __OHOS__

    #include "OHOS/OhosAVCodecDecoder.h"

#endif

#include "avcodecDecoder.h"

using namespace Cicada;
using namespace std;

/*
 * ==================== 设备硬解能力：平台分派与缓存 ====================
 *
 * 这一块是**唯一**知道"本平台怎么问硬解能力"的地方，所有平台宏都关在这个文件里
 * （decoderFactory.cpp 本来就是解码器的平台分派点，createBuildIn 里已经有同样的
 * ANDROID / __APPLE__ / __OHOS__ / _WIN32 分支）。上层（mediaPlayer/abr）只看到
 * decoderFactory::isHardwareDecodeSupported(enum AFCodecID) 这一个无平台宏的接口。
 *
 * 三态在内部收敛：1 = 明确有，0 = 明确没有，-1 = 未知；对外 -1 按"视为支持"返回 true。
 */
namespace {

    /*
     * 平台分派。返回 1 / 0 / -1，语义见上。
     *
     * 注意各分支的条件与下面的 createBuildIn() 保持一一对应 —— "能交给哪个平台解码器"
     * 就是"哪个平台解码器能回答硬解能力"。
     */
    int probeHardwareDecodeSupport(enum AFCodecID codec)
    {
#if defined(ANDROID) && defined(ENABLE_MEDIA_CODEC_DECODER)
        /*
         * Android：问 MediaCodecList —— mediaCodecDecoder 里的实现在 Java 侧只认
         * 硬件解码器（MediaCodecInfo.isHardwareAccelerated()，老 API 用编解码器名
         * 前缀判断），MIME 映射 video/avc->H.264、video/hevc->H.265、
         * video/av01->AV1、video/x-vnd.on2.vp9->VP9、video/mp4v-es->MPEG-4。
         */
        return mediaCodecDecoder::isHardwareDecodeSupported(codec) ? 1 : 0;
#elif defined(__APPLE__) && defined(ENABLE_VTB_DECODER)
        /*
         * Apple：VideoToolbox 的 VTIsHardwareDecodeSupported（iOS 11+ / macOS 10.13+）。
         * 即使应用打开了 "video.decoder.ffmpeg_videotoolbox"（只是换一层包装，解码仍然
         * 由 VideoToolbox 做），可硬解的编码集合不变，所以这里统一问 AFVTBDecoder。
         * 它答不上来的编码（VideoToolbox 没有对应 CMVideoCodecType 的，例如 AV1）返回
         * "未知" -> 对外视为支持，不会误降。
         */
        return AFVTBDecoder::is_hardware_decode_supported(codec) ? 1 : 0;
#elif defined(__OHOS__) && defined(ENABLE_OHOS_AVCODEC_DECODER)
        /*
         * OpenHarmony：硬解后端是系统 OH_AVCodec，createBuildIn() 里能交给
         * OhosAVCodecDecoder 的视频编码就是下面这几个 —— 这里必须是**同一份列表**，
         * 将来加编码两处一起改。列表之外的编码返回"未知"（OH_AVCodec 的能力无法在
         * 内核侧静态枚举），按"视为支持"处理。
         */
        return (codec == AF_CODEC_ID_H264 || codec == AF_CODEC_ID_HEVC
                || codec == AF_CODEC_ID_MPEG4 || codec == AF_CODEC_ID_VP9
                || codec == AF_CODEC_ID_AV1) ? 1 : -1;
#elif defined(ENABLE_AVCODEC_DECODER) && defined(CICADA_HW_DEVICE_TYPE)
        /*
         * 桌面（Windows D3D11VA / Linux VAAPI，以及 Apple 上没编 VTB 时的
         * FFmpeg VideoToolbox）：查这一份 FFmpeg 构建里有没有该编码的硬解配置
         * （avcodec_get_hw_config），**不创建设备**。
         *
         * 不创建设备是刻意的：创建 D3D11/VAAPI 设备有真实成本，而且会为一个不一定
         * 真正解码的编码白占 GPU 资源；"FFmpeg 有硬解配置、但这块显卡/驱动解不了"
         * 这种情况本来就有兜底 —— 解码器创建失败时 CreateVideoDecoder 会 release +
         * 用软解重建（SuperMediaPlayer.cpp 的 hw->sw 回退），与本次改动无关。
         */
        return avcodecDecoder::is_hardware_decode_supported(codec) ? 1 : 0;
#else
        /* 没有可问的平台后端：未知，对外按"视为支持"处理（只用效率序）。 */
        (void) codec;
        return -1;
#endif
    }

    /*
     * 探测结果缓存。MediaCodecList 枚举要跨 JNI 遍历设备上全部编解码器，
     * VTIsHardwareDecodeSupported 也不是免费的，而编码种类是个很小的固定集合，
     * 所以每个编码只探测一次。
     *
     * 用函数内静态量（C++11 起初始化本身线程安全）+ 一把锁保护 map：
     * mediaInfoGet 回调线程和 ABR 线程都会问，必须是线程安全的。
     * 这里没有任何计时器/看门狗，也没有失效逻辑 —— 设备能力在一次进程生命周期内不变。
     */
    struct HardwareDecodeCapabilityCache {
        std::mutex mutex;
        std::map<int, bool> known;
    };

    HardwareDecodeCapabilityCache &hardwareDecodeCapabilityCache()
    {
        static HardwareDecodeCapabilityCache cache;
        return cache;
    }

}// namespace

/*
 * ==================== 应用层传入的能力/偏好（进程内唯一一份覆盖值） ====================
 *
 * 存放点为什么在这里：见 decoderFactory.h 里那一段"为什么落在 decoderFactory"。
 * 一句话：decoderFactory 的静态接口是 L1 里 ABR 与起播回调共用的能力判据，而它们
 * 都拿不到 player 句柄，所以覆盖值必须放在这个进程内共享、线程安全的落点上。
 *
 * 唯一性说明（限制，不是缺陷）：进程里同时存在多个播放器实例时，**最后 set 的那一份
 * 生效**（公共能力本来就是按设备算的，两个实例不该给出不同答案）；SuperMediaPlayer
 * 析构时只清"还是自己发布的那一份"，不会误清后来者的状态。
 */
namespace {

    struct AppCodecSupportStore {
        std::mutex mutex;
        std::shared_ptr<const decoderFactory::AppCodecSupport> support;
    };

    AppCodecSupportStore &appCodecSupportStore()
    {
        static AppCodecSupportStore store;
        return store;
    }

    /*
     * 取一份不可变快照（shared_ptr 拷贝）后立刻解锁：调用方拿着快照用，不再持锁。
     * 快照对象发布之后就不再改动，所以读方之间、读写之间都没有数据竞争。
     */
    std::shared_ptr<const decoderFactory::AppCodecSupport> loadAppCodecSupport()
    {
        AppCodecSupportStore &store = appCodecSupportStore();
        std::lock_guard<std::mutex> lock(store.mutex);
        return store.support;
    }

    /* 是不是那 6 个规范短名之一（名字表只有 afCodecEfficiencyOrder() 那一份）。 */
    bool isKnownCodecShortName(const char *shortName)
    {
        if (shortName == nullptr || shortName[0] == '\0') {
            return false;
        }

        for (const char *const *name = afCodecEfficiencyOrder(); *name != nullptr; name++) {
            if (strcmp(*name, shortName) == 0) {
                return true;
            }
        }

        return false;
    }

    /* 内核默认效率序（从高到低），就是 afCodecEfficiencyOrder() 那份的字符串形式。 */
    std::vector<std::string> kernelCodecPreferenceOrder()
    {
        std::vector<std::string> order;

        for (const char *const *name = afCodecEfficiencyOrder(); *name != nullptr; name++) {
            order.push_back(*name);
        }

        return order;
    }

    /*
     * 解析一个"短名数组"：必须是 JSON 数组，元素必须都是字符串，不认识的短名忽略。
     * 返回 false = 畸形（缺字段 / 不是数组 / 元素不是字符串），调用方据此整份拒绝。
     */
    bool parseShortNameArray(const cJSON *array, std::vector<std::string> &out)
    {
        if (array == nullptr || !cJSON_IsArray(array)) {
            return false;
        }

        const int count = cJSON_GetArraySize(array);

        for (int i = 0; i < count; i++) {
            const cJSON *item = cJSON_GetArrayItem(array, i);

            if (item == nullptr || !cJSON_IsString(item) || item->valuestring == nullptr) {
                return false;
            }

            if (!isKnownCodecShortName(item->valuestring)) {
                /* 未知项忽略（契约：两边都只认那 6 个短名）。 */
                continue;
            }

            out.push_back(item->valuestring);
        }

        return true;
    }

    void addShortNameArray(cJSON *object, const char *name, const std::vector<std::string> &values)
    {
        cJSON *array = cJSON_CreateArray();

        if (array == nullptr) {
            return;
        }

        for (size_t i = 0; i < values.size(); i++) {
            cJSON *item = cJSON_CreateString(values[i].c_str());

            if (item != nullptr) {
                cJSON_AddItemToArray(array, item);
            }
        }

        cJSON_AddItemToObject(object, name, array);
    }

}// namespace

bool decoderFactory::AppCodecSupport::hwDecodeSupported(const char *shortName) const
{
    if (shortName == nullptr || shortName[0] == '\0') {
        return false;
    }

    for (size_t i = 0; i < hwDecode.size(); i++) {
        if (hwDecode[i] == shortName) {
            return true;
        }
    }

    return false;
}

int decoderFactory::AppCodecSupport::efficiencyRank(const char *shortName) const
{
    if (shortName == nullptr || shortName[0] == '\0') {
        return 0;
    }

    for (size_t i = 0; i < preference.size(); i++) {
        if (preference[i] == shortName) {
            /* 越靠前名次越大（只用于比大小，数值本身无外部含义）。 */
            return (int) (preference.size() - i);
        }
    }

    return 0;
}

int decoderFactory::parseAppCodecSupportJson(const std::string &json,
                                             std::shared_ptr<const AppCodecSupport> &out)
{
    out.reset();

    if (json.empty()) {
        return -EINVAL;
    }

    /* require_null_terminated = 1：后面还跟着别的非空白内容也算畸形。 */
    cJSON *root = cJSON_ParseWithOpts(json.c_str(), nullptr, 1);

    if (root == nullptr) {
        return -EINVAL;
    }

    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -EINVAL;
    }

    std::shared_ptr<AppCodecSupport> parsed(new AppCodecSupport());
    bool ok = true;
    const cJSON *hwDecode = cJSON_GetObjectItemCaseSensitive(root, "hwDecode");

    if (hwDecode == nullptr) {
        /*
         * hwDecode 是**必填**：缺了它就分不清"设备一个都硬解不了"和"应用根本没传能力"，
         * 而后者必须继续走内核探测。与其猜，不如判畸形整份拒绝。
         */
        ok = false;
    } else {
        ok = parseShortNameArray(hwDecode, parsed->hwDecode);
    }

    if (ok) {
        const cJSON *preference = cJSON_GetObjectItemCaseSensitive(root, "preference");

        if (preference != nullptr) {
            ok = parseShortNameArray(preference, parsed->preference);
        }
    }

    if (ok) {
        /*
         * preferred（应用层手动指定的"默认视频格式"）：
         *   · 字段不在 => 不指定（自动），合法；
         *   · 是字符串但为空串 / 不在那 6 个规范短名里 => 也不指定，**不算畸形**
         *     （契约：取值只认那 6 个短名，其余一律当"没指定"）；
         *   · 在但**不是字符串**（数字/数组/对象…）=> 结构畸形，整份拒绝 ——
         *     与 hwDecode、preference 一样的"畸形不半套用"。
         */
        const cJSON *preferred = cJSON_GetObjectItemCaseSensitive(root, "preferred");

        if (preferred != nullptr && !cJSON_IsString(preferred)) {
            ok = false;
        } else if (preferred != nullptr && preferred->valuestring != nullptr &&
                   isKnownCodecShortName(preferred->valuestring)) {
            parsed->preferred = preferred->valuestring;
        }
    }

    /* source 以及其它不认识的字段一律忽略（set 时内核自己写 "app"）。 */
    cJSON_Delete(root);

    if (!ok) {
        return -EINVAL;
    }

    out = parsed;
    return 0;
}

void decoderFactory::publishAppCodecSupport(std::shared_ptr<const AppCodecSupport> support)
{
    AppCodecSupportStore &store = appCodecSupportStore();
    std::lock_guard<std::mutex> lock(store.mutex);
    store.support = std::move(support);
}

void decoderFactory::clearAppCodecSupportIf(const std::shared_ptr<const AppCodecSupport> &expected)
{
    AppCodecSupportStore &store = appCodecSupportStore();
    std::lock_guard<std::mutex> lock(store.mutex);

    if (store.support == expected) {
        store.support.reset();
    }
}

std::shared_ptr<const decoderFactory::AppCodecSupport> decoderFactory::getAppCodecSupport()
{
    return loadAppCodecSupport();
}

std::string decoderFactory::getEffectiveCodecSupportJson()
{
    const std::shared_ptr<const AppCodecSupport> app = loadAppCodecSupport();
    cJSON *root = cJSON_CreateObject();

    if (root == nullptr) {
        return "";
    }

    std::vector<std::string> hwDecode;
    std::vector<std::string> preference;
    /* preferred 字段总是要出现：应用层指定过就是那个短名，没指定就是空串。 */
    std::string preferred;

    if (app != nullptr) {
        /* 应用层传入：直接回实际生效的那份，顺序归一化成短名规范序。 */
        cJSON_AddStringToObject(root, "source", "app");

        for (const char *const *name = afCodecEfficiencyOrder(); *name != nullptr; name++) {
            if (app->hwDecodeSupported(*name)) {
                hwDecode.push_back(*name);
            }
        }

        preference = app->preference.empty() ? kernelCodecPreferenceOrder() : app->preference;
        preferred = app->preferred;
    } else {
        /* 内核自己探测：只列能映射到 AFCodecID 的编码（MPEG-2 没有 id，见头文件说明）。 */
        cJSON_AddStringToObject(root, "source", "kernel");

        for (const char *const *name = afCodecEfficiencyOrder(); *name != nullptr; name++) {
            const enum AFCodecID id = afCodecIDFromShortName(*name);

            if (id == AF_CODEC_ID_NONE) {
                continue;
            }

            if (isHardwareDecodeSupported(id)) {
                hwDecode.push_back(*name);
            }
        }

        preference = kernelCodecPreferenceOrder();
    }

    addShortNameArray(root, "hwDecode", hwDecode);
    addShortNameArray(root, "preference", preference);
    /* 手动指定的编码原样回给应用层（空串 = 不指定 = 自动）。 */
    cJSON_AddStringToObject(root, "preferred", preferred.c_str());

    char *printed = cJSON_PrintUnformatted(root);
    std::string result = (printed != nullptr) ? std::string(printed) : std::string();

    if (printed != nullptr) {
        cJSON_free(printed);
    }

    cJSON_Delete(root);
    return result;
}

int decoderFactory::getEffectiveCodecEfficiencyRank(const char *shortName)
{
    const std::shared_ptr<const AppCodecSupport> app = loadAppCodecSupport();

    /*
     * 两套顺序在这里二选一，**绝不混用**：应用层传了非空 preference 就整份用它的
     * 位置序（不在表里的编码 rank = 0 = 未知档），否则整份用内核等级表。
     */
    if (app != nullptr && !app->preference.empty()) {
        return app->efficiencyRank(shortName);
    }

    return afCodecEfficiencyRankByShortName(shortName);
}

std::string decoderFactory::getEffectivePreferredCodec()
{
    /*
     * 与 getEffectiveCodecEfficiencyRank() 读的是**同一个落点、同一份快照**：
     * 起播默认档与 ABR 都调这一个函数，所以两处对"应用层到底指定了什么"永远一致。
     * 没有应用层数据（内核自己探测）时就是"不指定"。
     *
     * 这里**只读**，不做任何动作：设置 preferred 不会触发任何立即切换，
     * 它只在调用方下一次选流（起播默认档 / ABR 下一次决策）时被读到。
     */
    const std::shared_ptr<const AppCodecSupport> app = loadAppCodecSupport();

    if (app == nullptr) {
        return std::string();
    }

    return app->preferred;
}

bool decoderFactory::isCodecDecodable(const char *shortName)
{
    if (shortName == nullptr || shortName[0] == '\0') {
        /* 没给编码名（调用方问错了）：按"解不了"回答，调用方会回退到自动规则。 */
        return false;
    }

    const enum AFCodecID id = afCodecIDFromShortName(shortName);

    /*
     * 第一条路：硬解。已知编码问平台（应用层传过硬解集合时直接用应用层的集合，不探测）；
     * 编码未知（例如 MPEG-2 没有 AFCodecID）时它按"未知 = 视为支持"返回 true ——
     * 与本内核其它地方的处理一致：认不出来就不许否定用户的手动指定。
     */
    if (isHardwareDecodeSupported(id)) {
        return true;
    }

#if defined(ENABLE_AVCODEC_DECODER)
    /*
     * 第二条路：软解。avcodecDecoder 是本内核里唯一的软解后端
     * （见下面 createBuildIn() 的 DECFLAG_SW 分支），它的 is_supported() 就是
     * avcodec_find_decoder() != nullptr —— "这一份 FFmpeg 构建里有没有这个编码的
     * 解码器"，与真正创建软解解码器时用的是同一个判据，不会各说各话。
     *
     * 走到这里说明硬解明确不支持（不是"未知"），所以这就是最后一条路：有软解就
     * 允许手动指定（用户意图优先，允许软解），没有才回退。
     */
    return avcodecDecoder::is_supported(id);
#else
    /*
     * 这一份构建里没有编入 FFmpeg 软解后端时，createBuildIn(DECFLAG_SW) 返回 nullptr，
     * 内核既不能硬解也不能软解这个编码 —— 如实回答 false（调用方回退到自动规则）。
     */
    return false;
#endif
}

bool decoderFactory::isHardwareDecodeSupported(enum AFCodecID codec)
{
    /*
     * 应用层传过"设备能硬解的编码集合"时，这里**只信应用层的**：直接做成员判定，
     * 既不探测，也不读探测缓存（缓存里可能还存着应用层传入之前的旧答案）。
     */
    const std::shared_ptr<const AppCodecSupport> app = loadAppCodecSupport();

    if (app != nullptr) {
        const char *shortName = afCodecShortName(codec, nullptr);

        if (shortName == nullptr || shortName[0] == '\0') {
            /*
             * 应用层的集合只表达那 6 个短名；编码在这里表达不出来（AF_CODEC_ID_NONE、
             * VP8、音频编码等）⇒ 能力未知 ⇒ 沿用"未知 = 视为支持"，
             * 不允许因为认不出编码就把它降级。
             */
            return true;
        }

        return app->hwDecodeSupported(shortName);
    }

    /*
     * 编码未知（AF_CODEC_ID_NONE，例如清单里没给出 codecs 字符串）：能力也未知，
     * 按"视为支持"处理 —— 不能因为认不出编码就把这一路排到最后。
     */
    if (codec == AF_CODEC_ID_NONE) {
        return true;
    }

    HardwareDecodeCapabilityCache &cache = hardwareDecodeCapabilityCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    const int key = (int) codec;
    const auto iter = cache.known.find(key);

    if (iter != cache.known.end()) {
        return iter->second;
    }

    const int probed = probeHardwareDecodeSupport(codec);
    /* -1（未知）对外收敛成 true："查不到就当支持"。 */
    const bool supported = (probed != 0);
    cache.known[key] = supported;
    AF_LOGI("hardware decode capability: codec=%d probe=%d -> supported=%d (cached)\n",
            key, probed, (int) supported);
    return supported;
}

unique_ptr<Cicada::IDecoder> decoderFactory::create(const Stream_meta& meta, uint64_t flags, int maxSize,
                                                    const DrmInfo *drmInfo)
{
    IDecoder *decoder = codecPrototype::create(meta, flags, maxSize, drmInfo);

    if (decoder != nullptr) {
        return unique_ptr<IDecoder>(decoder);
    }

    return createBuildIn(meta.codec, flags, drmInfo);
}

unique_ptr<IDecoder> decoderFactory::createBuildIn(const AFCodecID &codec, uint64_t flags,
                                                   const DrmInfo *drmInfo)
{
    if (flags & DECFLAG_HW) {
#ifdef ANDROID
#ifdef ENABLE_MEDIA_CODEC_DECODER
        return std::unique_ptr<IDecoder>(new mediaCodecDecoder());
#endif
#endif
#ifdef __APPLE__
        /*
         * Apple 默认用自研的 AFVTBDecoder。但如果应用显式打开了
         * "video.decoder.ffmpeg_videotoolbox"（Qt 集成为了零拷贝会打开它），
         * 就走 FFmpeg 的 videotoolbox hwaccel：那条路交出来的是 CVPixelBuffer，
         * macOS 上可以用 CVMetalTextureCache 直接包成 MTLTexture 交给 Metal
         * 场景图，硬解 + 零拷贝。默认不打开，其它 Apple 应用的行为完全不变。
         */
        if (globalSettings::getSetting().getProperty(PROPERTY_KEY_DECODER_FFMPEG_VT) == "ON") {
            return unique_ptr<IDecoder>(new avcodecDecoder());
        }
#endif
#ifdef __APPLE__
#ifdef ENABLE_VTB_DECODER

        if (AFVTBDecoder::is_supported(codec)) {
            return unique_ptr<IDecoder>(new AFVTBDecoder());
        }

#endif
#endif
#ifdef __OHOS__
#ifdef ENABLE_OHOS_AVCODEC_DECODER
        // OH_AVCodec hardware decode (surface or buffer mode); unsupported
        // codecs fall through to the software decoder below.
        if (codec == AF_CODEC_ID_H264 || codec == AF_CODEC_ID_HEVC
                || codec == AF_CODEC_ID_MPEG4 || codec == AF_CODEC_ID_VP9
                || codec == AF_CODEC_ID_AV1 || codec == AF_CODEC_ID_AAC) {
            return std::unique_ptr<IDecoder>(new OhosAVCodecDecoder());
        }
#endif
#endif
#if defined(_WIN32) || defined(__linux__)
        /*
         * Neither Windows nor Linux has a separate hardware decoder class:
         * FFmpeg's hwaccel runs inside avcodecDecoder, with the device type
         * chosen per platform (D3D11VA on Windows, VAAPI on Linux - see
         * CICADA_HW_DEVICE_TYPE in avcodecDecoder.h) and every decoded surface
         * downloaded back to system memory.
         *
         * avcodecDecoder::open() decides at run time, through
         * avcodec_get_hw_config(), whether the hwaccel is actually present in the
         * linked FFmpeg, and otherwise keeps decoding in software. That is why
         * every codec can be handed to it instead of keeping a hardcoded list.
         *
         * Leaving Linux out of this branch was a real bug: a DECFLAG_HW request
         * fell through to the software case below, matched nothing, and returned
         * nullptr, so CreateVideoDecoder reported codec_error_video_not_support
         * (-512) and the framework quietly switched to the software decoder -
         * the VAAPI support in avcodecDecoder was never reached at all.
         */
        return unique_ptr<IDecoder>(new avcodecDecoder());
#endif
    }

    if (flags & DECFLAG_SW) {
#ifdef ENABLE_AVCODEC_DECODER
        return unique_ptr<IDecoder>(new avcodecDecoder());
#endif
    }

    return nullptr;
}

