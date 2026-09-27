//
// Created by moqi on 2019-08-20.
//

#ifndef CICADA_PLAYER_DECODERFACTORY_H
#define CICADA_PLAYER_DECODERFACTORY_H

#include <memory>
#include <map>
#include <string>
#include <vector>
#include "IDecoder.h"

class decoderFactory {

public:
    static std::unique_ptr<Cicada::IDecoder> create(const Stream_meta & meta, uint64_t flags, int maxSize ,
                                                    const Cicada::DrmInfo *drmInfo);

    /*
     * ============ 设备硬解能力查询（ABR 的"硬解优先"判据）============
     *
     * 语义（**两态收敛**，调用方只需要一个 bool）：
     *   true  = 平台明确回答"这个编码有硬件解码器"，**或者平台查不到**；
     *   false = 平台明确回答"这个编码在本设备上没有硬件解码器"。
     *
     * "查不到就当支持"是刻意的：这条信息只用来**降级**选择（把解不了的编码让开），
     * 平台答不上来的时候宁可保持原有行为（只按效率序排），也不许把一条其实能硬解
     * 的流误判成"解不了"而白白降清晰度。同时 codec == AF_CODEC_ID_NONE（编码未知，
     * 例如只有清单字符串没解析出来）也一律返回 true。
     *
     * **先看应用层有没有传**：应用层通过 CicadaSetVideoCodecSupport() 传过
     * "设备能硬解的编码集合"之后，这里**只信应用层的那份集合**（直接成员判定），
     * 既不探测也不读探测缓存 —— 应用层可以持久化它，避免每次起播都探测设备。
     * 应用层没传（或已清除）时才走下面的平台探测 + 缓存。
     *
     * **不改变解码器创建路径的容错**：它只回答"理论上有没有"，真正的失败仍然由
     * CreateVideoDecoder 的 hw -> sw 回退链路兜底（见 SuperMediaPlayer.cpp 的
     * "release + init with hw->sw fallback"）。
     *
     * 平台探测实现见 decoderFactory.cpp：按平台分派到各平台解码器自己的静态查询
     * （Android 走 MediaCodecList 的硬件编解码器枚举、Apple 走
     * VTIsHardwareDecodeSupported、桌面走 FFmpeg 的 avcodec_get_hw_config），
     * 结果**缓存**在 decoderFactory.cpp 里的静态表（每个编码只探测一次，没有计时器）。
     *
     * 本头文件里没有任何平台宏，L1 的调用方（mediaPlayer/abr）可以直接用。
     */
    static bool isHardwareDecodeSupported(enum AFCodecID codec);

    /*
     * ============ 应用层传入的"设备硬解能力 + 编码偏好" ============
     *
     * 为什么落在 decoderFactory：这个类的静态接口是 L1 里**唯一**同时被三条路径
     * 用到的"能力判据"——ABR（AbrAlgoStrategy）、起播默认档
     * （SMPMessageControllerListener）都在问它，而它们都拿不到 player 句柄。
     * 应用层传入的覆盖值因此必须放在一个进程内共享、且线程安全的落点上，就放在这里
     * （SuperMediaPlayer 另外持有同一份 shared_ptr 作为"本播放器传进来的状态"）。
     *
     * 线程模型：
     *   · 写 = API 线程（CicadaSetVideoCodecSupport）；
     *   · 读 = ABR 线程 / 起播回调线程（isHardwareDecodeSupported、
     *     getEffectiveCodecEfficiencyRank、getEffectiveCodecSupportJson）；
     *   读写之间用 decoderFactory.cpp 里的一把 mutex + **发布后不再改动的**
     *   AppCodecSupport 对象（shared_ptr 快照）隔开：读方取一份快照就走，没有数据竞争，
     *   也不会读到"半套用"的中间状态。
     */

    /*
     * 应用层传入的一份能力/偏好快照。语义上**不可变**：一旦发布就不许再改
     * （改 = 造一份新的再发布），读方才敢不加锁地用它。
     *
     * hwDecode：设备能硬解的编码短名集合，顺序无关，只认 afCodecShortName() 的
     *           那 6 个短名（未知项在解析时已被丢掉）。
     * preference：可选，从高到低的效率序覆盖；空 = 用内核默认序
     *           （afCodecEfficiencyOrder()）。
     * preferred：可选，应用层**手动指定**的"默认视频格式"。空 = 不指定（全自动，
     *           沿用硬解优先 + 效率序那套规则）。非空时它是那 6 个规范短名之一，
     *           语义见 getEffectivePreferredCodec()。
     */
    class AppCodecSupport {
    public:
        /* 成员判定：短名为空或不在集合里都是 false。注意"编码未知"的收敛在调用方
         * （isHardwareDecodeSupported）里做，本函数只回答"在不在这个集合里"。 */
        bool hwDecodeSupported(const char *shortName) const;

        /* 应用层偏好序里的名次：越靠前越大（只用于比大小，具体数值无外部含义）；
         * 不在 preference 里（或短名为空）返回 0，与"未知编码"同一档。 */
        int efficiencyRank(const char *shortName) const;

        std::vector<std::string> hwDecode;
        std::vector<std::string> preference;
        /* 新成员一律追加在类末尾（本工程既定规则）。 */
        std::string preferred;
    };

    /*
     * JSON（契约固定，get/set 同一份）：
     *   {"source":"app"|"kernel",
     *    "hwDecode":["H.265","H.264"],
     *    "preference":["AV1","H.265","VP9","H.264","MPEG-4","MPEG-2"],
     *    "preferred":"H.265"}
     *
     * parseAppCodecSupportJson：
     *   解析并**完整校验**成功后写 out，返回 0；任何不认识/畸形的输入都返回 -EINVAL
     *   且 out 保持 nullptr —— 调用方据此拒绝，绝不半套用。
     *   规则：根必须是 JSON 对象；hwDecode **必填**且必须是字符串数组（元素必须是
     *   6 个已知短名之一，未知项忽略）；preference 可选，同样必须是字符串数组；
     *   preferred 可选，必须是**字符串**（不是字符串 = 结构畸形，整份拒绝；是字符串
     *   但为空串或不在那 6 个短名表里 = 合法，就是"不指定"，与缺省等价）；
     *   其它字段（含 source）忽略。空数组合法（hwDecode 空 = "这 6 个一个都硬解不了"；
     *   preference 空 = 用内核默认序）。
     */
    static int parseAppCodecSupportJson(const std::string &json,
                                        std::shared_ptr<const AppCodecSupport> &out);

    /* 发布/清除应用层覆盖值（nullptr = 清除，恢复内核自己探测）。 */
    static void publishAppCodecSupport(std::shared_ptr<const AppCodecSupport> support);

    /* 只清除"还是 expected 这一份"的覆盖值：SuperMediaPlayer 析构时用它，
     * 避免把一个更晚发布覆盖值的播放器实例的状态一起清掉。 */
    static void clearAppCodecSupportIf(const std::shared_ptr<const AppCodecSupport> &expected);

    /* 当前生效的应用层覆盖值；没有（内核自己探测）时返回 nullptr。 */
    static std::shared_ptr<const AppCodecSupport> getAppCodecSupport();

    /*
     * 当前生效的能力 + 偏好，序列化成上面那份 JSON 交给应用层。
     *   · 应用层传过：source = "app"，hwDecode/preference 都是**实际生效**的那份
     *     （hwDecode 按短名规范序给出，preference 缺省时补上内核默认序）；
     *   · 没传：source = "kernel"，hwDecode 是内核探测结果，preference 是内核默认序。
     * 内核来源的 hwDecode 只列能映射到 AFCodecID 的编码：MPEG-2 在 AFCodecID 里没有
     * 枚举项、能力恒为"未知 = 视为支持"，列不列都不影响任何判定，故不列。
     * preferred 字段**总是**出现在返回里：应用层指定过就是那个短名，没指定
     * （或传了个不在表里的值）就是空串 ""，调用方据此判断"现在是全自动还是手动指定"。
     * 失败（内存不足）返回空串，调用方按"取不到"处理。
     */
    static std::string getEffectiveCodecSupportJson();

    /*
     * 当前生效的效率序名次（**只有这一处**把两套顺序分开选，绝不混用）：
     *   · 应用层传了非空 preference => 用它（按数组位置定序）；
     *   · 否则 => 内核默认序 afCodecEfficiencyRankByShortName()。
     * ABR 与起播默认档都必须调这个函数，不允许自己再选一次。
     */
    static int getEffectiveCodecEfficiencyRank(const char *shortName);

    /*
     * ============ 应用层手动指定的"默认视频格式"（preferred） ============
     *
     * 语义：应用层在同一个 JSON 里多传一个 "preferred":"H.265" 之后，**下一次**
     * 等级选择（起播默认档、ABR 下一次决策、ABR 触发的切档）就优先用这个编码；
     * 指定的编码在目标分辨率下没有流、或者内核根本解不了它时，回退到原来的
     * "硬解优先 + 效率序"规则。设置 preferred **本身不做任何事**：这里只提供读数
     * 和"解不解得了"这两个判据，谁在下一次选流时读它、读到之后怎么选，
     * 由起播默认档（SMPMessageControllerListener）与 ABR
     * （AbrAlgoStrategy::FindSameResolutionEfficientCodec）决定 —— 两处读的是
     * 同一个函数、同一份快照，不允许各自判断。
     *
     * 返回**当前生效**的手动指定编码短名；空串 = 不指定（全自动）。返回 std::string
     * 而不是 const char*：快照是 shared_ptr 的不可变对象，值拷贝出来调用方才安全。
     */
    static std::string getEffectivePreferredCodec();

    /*
     * 内核**能不能解**这个编码短名 —— "手动指定"能否成立的第 2 个判据。
     *
     * 两条路任一可用即为 true：
     *   · 硬解：decoderFactory::isHardwareDecodeSupported()（应用层传过硬解集合时
     *     直接用应用层的集合，不探测；编码/能力未知时按"视为支持"返回 true）；
     *   · 软解：内核自己编进来的软解后端（avcodecDecoder）里有这个编码的解码器
     *     （avcodec_find_decoder()，与真正建软解解码器用的是同一个判据）。
     *
     * 刻意**不**把"设备只能软解它"当成"解不了"：手动指定是用户的明确意图，
     * 只要内核有软解路径就允许（取舍说明见 AbrAlgoStrategy.cpp 里的注释）。
     * 只有"既不能硬解、也没有软解路径"才返回 false，那时调用方回退到自动规则。
     */
    static bool isCodecDecodable(const char *shortName);

private:

    static std::unique_ptr<Cicada::IDecoder> createBuildIn(const AFCodecID &codec, uint64_t flags,
                                                           const Cicada::DrmInfo *drmInfo);
};


#endif //CICADA_PLAYER_DECODERFACTORY_H
