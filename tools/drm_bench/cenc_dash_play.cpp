//
// tools/drm_bench/cenc_dash_play.cpp
//
// ============================================================================
// 真 CENC-in-DASH 走**内核自己的 DASH 读取管线**，并**解出视频帧**
// ============================================================================
//
// 【这个方法为什么能证明结论】
//
// 上一件工具（cenc_demux_check.cpp）喂的是**单个 mp4 文件**，证明的是
// "CENCDecrypter 在真 CENC 样本上解得对"。它证明不了清单那一层：
// DASH 的密钥是**按需**从 MPD 的 <ContentProtection cenc:licenseUrl> 取的，
// 而"按需取"这条链（DashStream::fetchSoftwareCencKey -> ContentKeyFetcher::fetch
// -> demuxer_service::resolveCencKey）在单文件路径上根本不会被走到。
//
// 所以这个 harness 把**同一个真 CENC 产物**换成 MPD + init.mp4 + seg-N.m4s，
// 让 demuxerPrototype 探到 <MPD 从而实例化 playList_demuxer（= DASH 管线），
// 然后**不看"有没有报错"，只看解出来的帧**：
//
//   阶段①  不注册任何密钥
//           ⇒ 每个包必须**仍然带** AV_PKT_DATA_ENCRYPTION_INFO（内核没有偷偷
//             把它当明文交出去 —— 这就是"有 CDM 的平台走硬解"那条路的形态），
//             并且把这些字节交给 libavcodec 的 h264 解码器，**帧数必须是 0**。
//   阶段②a 显式 setCencKey(kid, key)（KID 取自包自带的 encryption info）
//           ⇒ 同一个 MPD、同一批包，帧数必须 > 0。
//   阶段②b **不手工注册密钥**，改用带 licenseUrl 的 MPD，让产品自己按
//           cenc:licenseUrl 去 http://127.0.0.1:9101 取密钥
//           （DashStream 在 createDemuxer 里装的 setCencKeyResolver）
//           ⇒ 帧数同样必须 > 0，且**逐帧哈希**与 ②a、与明片参考一致。
//
// 【为什么"帧数"是判据而不是"没报错"】
// 明文 H.264 与"把密文当明文"在解码器眼里都是"一坨没见过的字节"：前者出帧，
// 后者不出帧但也**不会**必然报错（h264 解码器只会在日志里吐 non-existing PPS /
// SEI truncated 之类的东西，然后安静地返回 EAGAIN）。所以能不能出帧、出多少帧，
// 是唯一没法糊弄的判据。
//
// 【为什么①能证明"软解没有抢走 CDM 那条路"】
// ①不注册密钥、也不给可取密钥的地址（用不带 licenseUrl 的 stream.mpd），
// 包必须原样（= 密文）交出来，而且**必须一个帧都解不出**。如果内核在没有密钥的
// 情况下把包"解"开了，那它在有 CDM 的平台上就会跟 platform decoder 抢着解同一份
// 数据（解两遍 = 把明文当密文再解一次 = 花屏），①会立刻红。
// ②a 与①**只差一次 setCencKey 调用**，其余（同一个 MPD、同一批分片、同一份
// 解码代码、同一种喂法）完全一样 —— 于是"①出 0 帧、②出 N 帧"这件事把差异
// 唯一地钉在"密钥"上，而不是"解码器喂法不对"。
//
// 【解码器喂法：为什么这里自己判 AVCC / Annex B】
// 内核这条路上包到底是长度前缀（AVCC，mp4 容器原形态）还是 00 00 01 起始码
// （Annex B），取决于清单层传下来的 mMergeVideoHeader
// （IDemuxer.h:346 / PlaylistManager.h:101 默认 header_type_no_touch ⇒ 不建 bsf
// ⇒ 保持容器原形态；而 avFormatDemuxer::createBsf 一看到包上有加密 side data
// 就直接返回 0，效果相同）。但这个默认值是**配置的函数**，不该让 harness 依赖它：
// 所以这里自己看第一个样本的头几个字节判形态、自己做 AVCC->Annex B 转换，
// 并把判出来的形态打出来。判错了会同样影响 ①②，于是①②的对比依然成立。
//
// 【三层证据分开打】
// 要求里写明了：如果②拿不到帧，必须分清是
//   (1) 密钥没取到、(2) 包没被解密、(3) 解码器喂法不对。
// 所以每一阶段都分三段打印：密钥层 / 包层 / 解码层。
// 密钥层的证据同时来自**产品自己的日志**（log_set_log_level(AF_LOG_LEVEL_INFO)
// 之后，DashStream 会打 "DASH CENC software decryption is set up ..." 或
// "cannot set up software CENC decryption for key id ..."，demuxer_service 会打
// "CENC software decryption is active ..."）。
//
// 【不改产品代码】本文件只用公开 API（data_source / demuxer_service），
// 不 include 任何 *Internal* 头文件。
//

#define LOG_TAG "cencdashplay"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
/* 读循环要"等分片"（见下面的 GetRemainSegmentCount 那段）：DASH 的分片是异步下载的，
   拉取还没回来时 readPacket 给不出包，这不是 EOS。等待用标准库，避免再拖一个平台头。 */
#include <chrono>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
}

#include "demuxer/demuxer_service.h"
#include "data_source/dataSourcePrototype.h"
#include "utils/AFMediaType.h"
#include "utils/frame_work_log.h"

using namespace Cicada;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

static bool hexToKey(const std::string &hex, uint8_t *key)
{
    if (hex.size() != 32) {
        return false;
    }

    for (int i = 0; i < 16; i++) {
        key[i] = static_cast<uint8_t>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    }

    return true;
}

static inline uint64_t fnvStep(uint64_t h, uint8_t b)
{
    h ^= b;
    h *= 1099511628211ULL;
    return h;
}

// ---------------------------------------------------------------------------
// AVCC <-> Annex B
// ---------------------------------------------------------------------------

struct EsForm {
    bool annexb{false};        // 包内带 00 00 01 起始码
    int naluLengthSize{4};     // AVCC 时 NAL 长度字段占几个字节
};

static inline int be16(const uint8_t *p)
{
    return (p[0] << 8) | p[1];
}

/*
 * 只看**第一个非空样本**的前 4 个字节判形态。
 * 为什么够：H.264 的 AVCC 样本第一个字段是 4 字节 NAL 长度，长度必然远小于
 * 样本大小，不可能恰好等于 0x00000001（那意味着一个 1 字节的 NAL）；而 Annex B
 * 的第一个 NAL 如果是 AUD/SPS 一定以起始码开头。所以这一个判断在本用例上是可靠的，
 * 而且判出来的结果会**打出来**，不会变成"看不见的假设"。
 */
static EsForm detectForm(const std::vector<std::vector<uint8_t>> &payloads)
{
    EsForm f;

    for (const auto &p : payloads) {
        if (p.size() < 4) {
            continue;
        }

        if ((p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) ||
                (p[0] == 0 && p[1] == 0 && p[2] == 1)) {
            f.annexb = true;
        }

        break;
    }

    return f;
}

/*
 * 把 avcC（ISO/IEC 14496-15 的 AVCDecoderConfigurationRecord）摊成 Annex B 的
 * SPS/PPS 序列。返回 false 表示"这不是 avcC"（多半本来就是 Annex B 的 extradata）。
 *
 * 为什么必须做这一步：mp4dash 的 init 段把 SPS/PPS 放在 stsd/avcC 里，**分片里不重复**。
 * 不把 extradata 交给解码器，h264 解码器拿不到 SPS/PPS，一个帧都出不来 ——
 * 那正是"解码器喂法不对"这一层最容易踩的坑，所以这里的失败必须显式报出来。
 */
static bool avcCToAnnexB(const uint8_t *extra, int size, int *naluLengthSize, std::vector<uint8_t> &out)
{
    if (size < 7 || extra[0] != 1) {
        return false;
    }

    *naluLengthSize = (extra[4] & 0x03) + 1;

    const uint8_t *p = extra + 5;
    const uint8_t *end = extra + size;
    static const uint8_t sc[4] = {0, 0, 0, 1};

    int numSps = (*p++) & 0x1F;

    for (int i = 0; i < numSps; i++) {
        if (p + 2 > end) {
            return false;
        }

        int len = be16(p);
        p += 2;

        if (len <= 0 || p + len > end) {
            return false;
        }

        out.insert(out.end(), sc, sc + 4);
        out.insert(out.end(), p, p + len);
        p += len;
    }

    if (p >= end) {
        return false;
    }

    int numPps = *p++;

    for (int i = 0; i < numPps; i++) {
        if (p + 2 > end) {
            return false;
        }

        int len = be16(p);
        p += 2;

        if (len <= 0 || p + len > end) {
            return false;
        }

        out.insert(out.end(), sc, sc + 4);
        out.insert(out.end(), p, p + len);
        p += len;
    }

    return true;
}

/*
 * 把 extradata 统一成 Annex B 交给解码器（不管是 avcC 还是本来就是 Annex B）。
 * extraForm 出来是给人看的说明。
 */
static std::vector<uint8_t> extradataToAnnexB(const std::vector<uint8_t> &extra, int *naluLengthSize,
        std::string &extraForm)
{
    std::vector<uint8_t> out;

    if (extra.empty()) {
        extraForm = "空（没有 extradata）";
        return out;
    }

    if (avcCToAnnexB(extra.data(), static_cast<int>(extra.size()), naluLengthSize, out)) {
        extraForm = "avcC -> Annex B（SPS/PPS 已抽出）";
        return out;
    }

    out = extra;
    extraForm = "原样当 Annex B 用";
    return out;
}

/*
 * AVCC 样本 -> Annex B。返回 false 表示"一个 NAL 都没切出来"（密文样本就是这样，
 * 不是错误，只是没有可解的 NAL）。
 */
static bool payloadToAnnexB(const std::vector<uint8_t> &in, const EsForm &form, int naluLengthSize,
                            std::vector<uint8_t> &out)
{
    out.clear();

    if (form.annexb) {
        out = in;
        return !in.empty();
    }

    static const uint8_t sc[4] = {0, 0, 0, 1};
    const int64_t n = static_cast<int64_t>(in.size());
    int64_t pos = 0;
    int nalu = 0;

    while (pos + naluLengthSize <= n) {
        int64_t len = 0;

        for (int i = 0; i < naluLengthSize; i++) {
            len = (len << 8) | in[static_cast<size_t>(pos + i)];
        }

        pos += naluLengthSize;

        if (len <= 0 || pos + len > n) {
            break;
        }

        out.insert(out.end(), sc, sc + 4);
        out.insert(out.end(), in.begin() + pos, in.begin() + pos + len);
        pos += len;
        nalu++;
    }

    return nalu > 0;
}

// ---------------------------------------------------------------------------
// H.264 帧计数器
// ---------------------------------------------------------------------------

struct DecodeResult {
    int frames{0};
    uint64_t lumaHash{0};       // 每一帧亮度平面的 FNV-1a，逐帧累加
    std::string firstError;
    int droppedPackets{0};
};

class H264Counter {
public:
    ~H264Counter()
    {
        close();
    }

    bool open(const std::vector<uint8_t> &extradata, std::string &what)
    {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);

        if (codec == nullptr) {
            mError = "avcodec_find_decoder(AV_CODEC_ID_H264) 返回空";
            return false;
        }

        mCtx = avcodec_alloc_context3(codec);

        if (mCtx == nullptr) {
            mError = "avcodec_alloc_context3 失败";
            return false;
        }

        if (!extradata.empty()) {
            mCtx->extradata = static_cast<uint8_t *>(av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));

            if (mCtx->extradata != nullptr) {
                memcpy(mCtx->extradata, extradata.data(), extradata.size());
                mCtx->extradata_size = static_cast<int>(extradata.size());
            }
        }

        mCtx->thread_count = 1;     // 单线程：帧的输出顺序与内容必须可复现

        const int ret = avcodec_open2(mCtx, codec, nullptr);

        if (ret < 0) {
            char buf[128] = {0};
            av_strerror(ret, buf, sizeof(buf));
            mError = std::string("avcodec_open2 失败: ") + buf;
            return false;
        }

        mPkt = av_packet_alloc();
        mFrame = av_frame_alloc();

        if (mPkt == nullptr || mFrame == nullptr) {
            mError = "av_packet_alloc / av_frame_alloc 失败";
            return false;
        }

        what = std::string("extradata ") + (extradata.empty() ? "无" : "有") +
               "，解码器已打开（" + codec->name + "）";
        return true;
    }

    void send(const std::vector<uint8_t> &annexbSample)
    {
        if (mCtx == nullptr || mPkt == nullptr || annexbSample.empty()) {
            return;
        }

        drain();
        av_packet_unref(mPkt);

        if (av_new_packet(mPkt, static_cast<int>(annexbSample.size())) < 0) {
            return;
        }

        memcpy(mPkt->data, annexbSample.data(), annexbSample.size());
        const int ret = avcodec_send_packet(mCtx, mPkt);

        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            mResult.droppedPackets++;
            recordError(ret);
        }

        drain();
    }

    void flush()
    {
        if (mCtx == nullptr) {
            return;
        }

        avcodec_send_packet(mCtx, nullptr);
        drain();
    }

    /* 取结果（顺带把"第一条错误"带上，否则失败时只剩一个 0 帧，看不出是解码器没打开）。 */
    DecodeResult take() const
    {
        DecodeResult r = mResult;
        r.firstError = mError;
        return r;
    }

    void close()
    {
        if (mFrame != nullptr) {
            av_frame_free(&mFrame);
        }

        if (mPkt != nullptr) {
            av_packet_free(&mPkt);
        }

        if (mCtx != nullptr) {
            avcodec_free_context(&mCtx);
        }
    }

private:
    void recordError(int ret)
    {
        if (!mError.empty()) {
            return;
        }

        char buf[128] = {0};
        av_strerror(ret, buf, sizeof(buf));
        mError = buf;
    }

    void drain()
    {
        for (;;) {
            const int ret = avcodec_receive_frame(mCtx, mFrame);

            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }

            if (ret < 0) {
                recordError(ret);
                break;
            }

            mResult.frames++;

            // 亮度平面的逐字节哈希：用来把"帧数对上了"加强成"帧的内容也对上了"。
            // 只取亮度是因为色度平面可能带解码器自己的对齐/填充差异，与内容无关。
            if (mFrame->data[0] != nullptr) {
                for (int y = 0; y < mFrame->height; y++) {
                    const uint8_t *row = mFrame->data[0] + static_cast<int64_t>(y) * mFrame->linesize[0];

                    for (int x = 0; x < mFrame->width; x++) {
                        mResult.lumaHash = fnvStep(mResult.lumaHash, row[x]);
                    }
                }
            }

            av_frame_unref(mFrame);
        }
    }

private:
    AVCodecContext *mCtx{nullptr};
    AVFrame *mFrame{nullptr};
    AVPacket *mPkt{nullptr};
    DecodeResult mResult{};
    std::string mError{};
};

/*
 * 把某一阶段的包按 Annex B 落盘，交给**另一个实现**（ffmpeg.exe）去数帧。
 *
 * 【为什么必须有这个】本 harness 自己的解码计数出现过自相矛盾的读数（阶段①的包已证明
 * 是密文，却报"解出 360 帧、亮度哈希与明片参考完全相同"），而连"计数没按阶段重置"这个
 * 解释也被代码否掉了（H264Counter 是 decodeAll 里的局部对象）。既然自己的读数解释不通，
 * 就不要再猜：把字节原样落盘，让 ffmpeg 这个独立实现去说"到底出没出帧"。
 */
static void dumpAnnexB(const char *path, const std::vector<std::vector<uint8_t>> &payloads,
                       const std::vector<uint8_t> &extradata)
{
    const EsForm form = detectForm(payloads);
    int naluLengthSize = 4;
    std::string note;
    const std::vector<uint8_t> extra = extradataToAnnexB(extradata, &naluLengthSize, note);
    FILE *fp = fopen(path, "wb");

    if (fp == nullptr) {
        printf("  [dump] 打不开 %s\n", path);
        return;
    }

    if (!extra.empty()) {
        fwrite(extra.data(), 1, extra.size(), fp);
    }

    std::vector<uint8_t> annexb;
    size_t written = 0;

    for (const auto &p : payloads) {
        if (payloadToAnnexB(p, form, naluLengthSize, annexb)) {
            fwrite(annexb.data(), 1, annexb.size(), fp);
            written++;
        }
    }

    fclose(fp);
    printf("  [dump] %s：%zu/%zu 个样本落盘（包形态 %s，extradata %zu 字节）\n",
           path, written, payloads.size(), form.annexb ? "Annex B" : "AVCC->AnnexB", extra.size());
}

static DecodeResult decodeAll(const std::vector<std::vector<uint8_t>> &payloads,
                              const std::vector<uint8_t> &extradata,
                              std::string &packetFormNote, std::string &extraFormNote,
                              std::string &decoderNote)
{
    const EsForm form = detectForm(payloads);
    int naluLengthSize = 4;
    const std::vector<uint8_t> annexbExtra = extradataToAnnexB(extradata, &naluLengthSize, extraFormNote);

    if (form.annexb) {
        packetFormNote = "Annex B（00 00 01 起始码）";
    } else {
        char buf[64] = {0};
        snprintf(buf, sizeof(buf), "AVCC（%d 字节长度前缀）", form.naluLengthSize);
        packetFormNote = buf;
    }

    H264Counter counter;
    decoderNote = "";

    if (!counter.open(annexbExtra, decoderNote)) {
        decoderNote = "解码器打开失败";
        return counter.take();
    }

    std::vector<uint8_t> annexb;

    for (const auto &p : payloads) {
        if (payloadToAnnexB(p, form, naluLengthSize, annexb)) {
            counter.send(annexb);
        }
    }

    counter.flush();

    return counter.take();
}

// ---------------------------------------------------------------------------
// 走内核 DASH 管线读一遍
// ---------------------------------------------------------------------------

struct DashRun {
    bool ok{false};
    int nbStreams{-1};
    bool isPlayList{false};
    int videoIndex{-1};
    int packets{0};
    int encryptedPackets{0};
    std::vector<std::string> kids;                  // 每个包的 KID（没有就是空串）
    std::vector<std::vector<uint8_t>> payloads;     // 每个包的字节（解码器的输入）
    std::vector<uint8_t> extradata;
    std::string extradataFrom;
    std::string failReason;
};

static void freeSource(IDataSource *source)
{
    if (source != nullptr) {
        source->Close();
        delete source;
    }
}

static bool readThroughDash(const char *mpdPath, const uint8_t *key, const std::string &kidHex, DashRun &out)
{
    /*
     * URI 按**原样**传：dataSourcePrototype::create() 对非 curl 形状的东西落到
     * ffmpegDataSource，后者把 mUri 直接交给 avio_open2()，其 "file" 协议能解析
     * 普通 Windows 路径。加 "file://" 前缀会让 avio_open2 把 "D:\..." 当 HOST。
     * （这与 cenc_demux_check.cpp 里的注释是同一条，别改成 file://。）
     */
    const std::string uri(mpdPath);
    printf("    [stage] dataSourcePrototype::create(\"%s\")\n", mpdPath);
    fflush(stdout);

    IDataSource *source = dataSourcePrototype::create(uri);

    if (source == nullptr) {
        out.failReason = "dataSourcePrototype::create 返回空";
        return false;
    }

    printf("    [stage] 数据源 = %s\n", source->GetUri().c_str());
    fflush(stdout);

    const int openRet = source->Open(0);

    if (openRet < 0) {
        printf("    [stage] 数据源 Open(0) 失败 ret=%d\n", openRet);
        freeSource(source);
        out.failReason = "数据源打不开";
        return false;
    }

    /*
     * service 放在自己的作用域里，保证析构（以及 close()）都发生在
     * source->Close()/delete 之前 —— 反过来的话 service 析构时会碰已经没了的源。
     */
    {
        demuxer_service service(source);
        service.initOpen(demuxer_type_unknown);

        if (service.getDemuxerHandle() == nullptr) {
            printf("    [stage] 解复用器没有打开（getDemuxerHandle() == null）\n");
            out.failReason = "解复用器打不开";
            freeSource(source);
            return false;
        }

        out.isPlayList = service.isPlayList();
        out.nbStreams = service.GetNbStreams();
        printf("    [stage] isPlayList()=%d（1 = 走的是 playList_demuxer / DashStream，"
               "不是单文件解复用）GetNbStreams()=%d\n", out.isPlayList ? 1 : 0, out.nbStreams);
        fflush(stdout);

        for (int i = 0; i < out.nbStreams; i++) {
            Stream_meta meta;
            memset(&meta, 0, sizeof(meta));
            const int r = service.GetStreamMeta(&meta, i, false);
            printf("    [stage]   stream %d: GetStreamMeta=%d type=%d codec=%d %dx%d extradata=%d\n",
                   i, r, static_cast<int>(meta.type), static_cast<int>(meta.codec),
                   meta.width, meta.height, meta.extradata_size);

            if (r >= 0 && meta.type == STREAM_TYPE_VIDEO && out.videoIndex < 0) {
                out.videoIndex = i;

                if (meta.extradata != nullptr && meta.extradata_size > 0) {
                    out.extradata.assign(meta.extradata, meta.extradata + meta.extradata_size);
                    out.extradataFrom = "demuxer_service::GetStreamMeta(Stream_meta::extradata)";
                }
            }
        }

        fflush(stdout);

        if (out.videoIndex < 0) {
            printf("    [stage] 清单里找不到视频流\n");
            out.failReason = "清单里没有视频流";
            service.close();
            freeSource(source);
            return false;
        }

        if (key != nullptr) {
            const int r = service.setCencKey(kidHex, key, 16);
            printf("    [stage] setCencKey(%s, <16 bytes>) -> %d\n", kidHex.c_str(), r);

            if (r != 0) {
                out.failReason = "setCencKey 失败";
                service.close();
                freeSource(source);
                return false;
            }
        } else {
            printf("    [stage] 本阶段**不注册**任何密钥（key == nullptr）\n");
        }

        printf("    [stage] OpenStream(%d)\n", out.videoIndex);
        fflush(stdout);
        service.OpenStream(out.videoIndex);

        std::unique_ptr<IAFPacket> packet;
        /*
         * 【实测教训：播放列表的"没包"不等于 EOS】
         *
         * 第一版照抄 cenc_demux_check 的写法（`if (ret <= 0) break;`）—— 那份工具喂的是
         * **单个 mp4**，包是同步就绪的，所以那条判据没问题。换成 MPD 之后同一个写法
         * 会在 **1 毫秒**内"读完 0 个包"：DashStream 线程才刚起来、curl 那头下一句就是
         * `didn't get any data from stream`（2026-10-03 实测 harness3.log:35-37）。
         * 也就是说：**播放列表的分片是异步下载的**，拉取没回来时 readPacket 给不出包。
         *
         * 判据改用 demuxer_service::GetRemainSegmentCount()（还有剩余分片 ⇒ 继续等），
         * 另加一个"整体墙钟上限"当安全阀：它是把'无限等'变成'有限等'，不是用来掩盖错误
         *（真超时会明确报出来，见下面的 failReason）。
         */
        const auto waitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        int idlePolls = 0;      /* 连续无新包的次数（有界等待，见下面的判据） */

        for (;;) {
            packet.reset();
            /*
             * index = -1：只 OpenStream 了视频那一路，DashManager::ReadPacket 的
             * 循环里只有它是 selected，所以拿回来的必然是视频包。用 -1 而不是
             * 编码过的 streamIndex，是为了不去猜 GEN_STREAM_ID(id, innerIndex)
             * 里那个 id —— 猜错就会读到 0 个包，而这种失败看着像"片源坏了"。
             */
            const int ret = service.readPacket(packet, -1);

            if (packet == nullptr) {
                /* ret > 0 = "消费了包但这条流没拿到"，继续 */
                if (ret > 0) {
                    continue;
                }

                /* ret <= 0：对播放列表要看"还有没有剩余分片"（见上面那段说明）。
                 *
                 * ★索引必须是**这条视频流自己的** out.videoIndex，不能写 0：
                 *   本片源第 0 条不一定是视频（Bento4 的 stream.mpd 里它就是 video，
                 *   但换一份 MPD 就可能不是），而"别的流的剩余分片数"永远读不完 ⇒
                 *   实测白等满 120 秒（harness4.log：全程 244 秒里两次 120 秒空等）。
                 *
                 * ★再加一条**有界等待**：即便上面那个计数不准，也不能白等 ——
                 *   已经读到过包之后，连续 200 次（≈2 秒）没有新包就收工，
                 *   并把这件事打出来（是"等够了"不是"读完了"，两者在日志里能分辨）。
                 *   它是 harness 的安全阀，不是产品行为。 */
                const int remain = service.GetRemainSegmentCount(out.videoIndex);

                /* 已经读到过包之后，连续 200 次（约 2 秒）没有新包就收工。
                 * 它是 harness 的安全阀，不是产品行为；日志里明确写【等够了】而不是
                 * 【读完了】，两者必须能分辨（上面那个计数被实测证明不可靠）。 */
                if (out.packets > 0 && ++idlePolls > 200) {
                    printf("    [stage] 连续 %d 次没有新包（约 2 秒），按【等够了】收工"
                           "（剩余分片计数=%d，不可靠）\n", idlePolls, remain);
                    break;
                }

                if (remain <= 0) {
                    break;
                }

                if (std::chrono::steady_clock::now() > waitDeadline) {
                    out.failReason = "等待分片超时（120 秒内没有再读到包）";
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            IAFPacket::EncryptionInfo info{};

            if (packet->getEncryptionInfo(&info) && info.key_id != nullptr && info.key_id_size > 0) {
                out.encryptedPackets++;
                out.kids.push_back(CENCDecrypter::toHex(info.key_id, info.key_id_size));
            } else {
                out.kids.push_back(std::string());
            }

            if (out.extradata.empty()) {
                IAFPacket::packetInfo &pi = packet->getInfo();

                if (pi.extra_data != nullptr && pi.extra_data_size > 0) {
                    out.extradata.assign(pi.extra_data, pi.extra_data + pi.extra_data_size);
                    out.extradataFrom = "IAFPacket::getInfo().extra_data（包上带的 extradata）";
                }
            }

            out.payloads.emplace_back(packet->getData(), packet->getData() + packet->getSize());
            out.packets++;

            if (out.packets > 20000) {
                break;      // 安全阀：本用例只有 ~300 个包
            }
        }

        printf("    [stage] 读完：%d 个包（其中 %d 个带 encryption info）\n",
               out.packets, out.encryptedPackets);

        /*
         * 【本轮修：extradata 必须在**读完包之后**再取一次】
         *
         * 实测（2026-10-04 harness4.log）：上面那次"读包之前"的 GetStreamMeta 里，
         * CENC-DASH 的视频流报 extradata_size=0 ⇒ 解码器拿不到 SPS/PPS、一帧都解不出
         *（h264 报 Invalid data found when processing input），而 HTTP 服务端日志
         * 证明 video/avc1/init.mp4 **确实被取回来了**。
         *
         * 原因是**时序**，不是"没取到"：
         *   · DASH 的 init 段是"读起来之后"才解析的；
         *   · avFormatDemuxer 对**加密流**故意**不在 OpenStream 时建 bsf**
         *     （avFormatDemuxer.cpp:552-556），extradata 要等内层解复用器真正开起来才有值；
         *   · 包上也不会带（只有 AV_PKT_DATA_NEW_EXTRADATA 时才 setExtraData，
         *     见 avFormatDemuxer.cpp:500-501）。
         * 所以读完之后再取一次，才是这条流真正的 extradata；两次的字节数都打出来，
         * 让"读之前为空、读之后有值"这件事一眼可见（而不是靠猜）。
         */
        if (out.videoIndex >= 0) {
            Stream_meta metaAfter;
            memset(&metaAfter, 0, sizeof(metaAfter));

            if (service.GetStreamMeta(&metaAfter, out.videoIndex, false) >= 0
                && metaAfter.extradata != nullptr && metaAfter.extradata_size > 0) {
                printf("    [stage] 读完之后再取一次 GetStreamMeta：extradata=%d 字节"
                       "（读之前那次是 %d 字节）\n",
                       metaAfter.extradata_size, static_cast<int>(out.extradata.size()));
                out.extradata.assign(metaAfter.extradata, metaAfter.extradata + metaAfter.extradata_size);
                out.extradataFrom = "读完之后再取的 GetStreamMeta（init 段此时已解析）";
            } else {
                printf("    [stage] 读完之后再取一次 GetStreamMeta：仍然没有 extradata"
                       "（那就要查 init 段的 avcC 有没有被解析出来了）\n");
            }
        }

        fflush(stdout);

        service.close();
    }

    freeSource(source);
    out.ok = true;
    return true;
}

// ---------------------------------------------------------------------------
// 明片参考：直接走 libavformat 读 input.mp4，再用同一份解码代码数帧
// ---------------------------------------------------------------------------

static bool readReference(const char *path, std::vector<std::vector<uint8_t>> &payloads,
                          std::vector<uint8_t> &extradata)
{
    AVFormatContext *ctx = nullptr;

    if (avformat_open_input(&ctx, path, nullptr, nullptr) < 0) {
        printf("打不开明片参考 %s\n", path);
        return false;
    }

    if (avformat_find_stream_info(ctx, nullptr) < 0) {
        printf("明片参考 %s 找不到流信息\n", path);
        avformat_close_input(&ctx);
        return false;
    }

    int vIdx = -1;

    for (unsigned i = 0; i < ctx->nb_streams; i++) {
        if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vIdx = static_cast<int>(i);
            break;
        }
    }

    if (vIdx < 0) {
        printf("明片参考 %s 里没有视频流\n", path);
        avformat_close_input(&ctx);
        return false;
    }

    AVCodecParameters *par = ctx->streams[vIdx]->codecpar;

    if (par->extradata != nullptr && par->extradata_size > 0) {
        extradata.assign(par->extradata, par->extradata + par->extradata_size);
    }

    AVPacket *pkt = av_packet_alloc();

    while (av_read_frame(ctx, pkt) >= 0) {
        if (pkt->stream_index == vIdx && pkt->size > 0) {
            payloads.emplace_back(pkt->data, pkt->data + pkt->size);
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    avformat_close_input(&ctx);
    return true;
}

// ---------------------------------------------------------------------------
// 比较
// ---------------------------------------------------------------------------

/* 两个批次里逐包比较，返回"完全相同的包数"与"比较过的包数"。 */
static void comparePayloads(const std::vector<std::vector<uint8_t>> &a,
const std::vector<std::vector<uint8_t>> &b,
size_t &identical, size_t &compared)
{
    identical = 0;
    compared = 0;

    for (size_t i = 0; i < a.size() && i < b.size(); i++) {
        compared++;

        if (a[i] == b[i]) {
            identical++;
        }
    }
}

/* 解码失败时把"第一条错误"挂到行尾 —— 否则"0 帧"看不出是解码器根本没打开。 */
static std::string errNote(const DecodeResult &r)
{
    if (r.firstError.empty()) {
        return std::string();
    }

    return "（解码器首条错误: " + r.firstError + "）";
}

static std::string kidSummary(const std::vector<std::string> &kids)
{
    std::string out;

    for (const auto &k : kids) {
        if (k.empty()) {
            continue;
        }

        if (out.find(k) == std::string::npos) {
            if (!out.empty()) {
                out += ", ";
            }

            out += k;
        }
    }

    return out.empty() ? std::string("(没有任何包带 KID)") : out;
}

int main(int argc, char **argv)
{
    /*
     * 无缓冲 stdout：重定向到文件时，满缓冲会把所有 printf 藏到退出那一刻，
     * 于是"卡在哪一步"就看不出来了。有它，日志的最后一行永远是阻塞的那一步。
     */
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc < 5) {
        printf("usage: cenc_dash_play <clear.mp4> <plain.mpd> <drm.mpd> <keyhex>\n");
        printf("  clear.mp4  未加密的源（明片参考，用它解出的帧数/哈希做基准）\n");
        printf("  plain.mpd  没有 cenc:licenseUrl 的 MPD（阶段①与②a 用）\n");
        printf("  drm.mpd    带 cenc:licenseUrl 的 MPD（阶段②b 用）\n");
        printf("  keyhex     32 个十六进制字符的内容密钥\n");
        return 2;
    }

    const char *clearPath = argv[1];
    const char *plainMpd = argv[2];
    const char *drmMpd = argv[3];
    const std::string keyHex = argv[4];

    uint8_t key[16];

    if (!hexToKey(keyHex, key)) {
        printf("key 必须是 32 个十六进制字符\n");
        return 2;
    }

    /*
     * 把内核自己的日志打开：阶段②b 的"密钥到底取到没有"最直接的证据就是
     * DashStream / demuxer_service 自己打的那几行。不开日志，②b 失败时就只剩
     * "帧数是 0"这一个现象，分不清是取密钥失败还是别的。
     */
    log_set_log_level(AF_LOG_LEVEL_INFO);

    int failures = 0;

    /* ---------------- 基准：明片参考 ---------------- */
    printf("===== 基准：明片参考 %s =====\n", clearPath);
    std::vector<std::vector<uint8_t>> refPayloads;
    std::vector<uint8_t> refExtra;

    if (!readReference(clearPath, refPayloads, refExtra)) {
        return 1;
    }

    std::string refForm, refExtraForm, refDecoderNote;
    const DecodeResult refDecode = decodeAll(refPayloads, refExtra, refForm, refExtraForm, refDecoderNote);
    printf("  参考样本数        : %zu\n", refPayloads.size());
    printf("  包形态            : %s\n", refForm.c_str());
    printf("  extradata         : %s（%zu 字节）\n", refExtraForm.c_str(), refExtra.size());
    printf("  解出帧数          : %d\n", refDecode.frames);
    printf("  亮度哈希          : 0x%016llx\n", static_cast<unsigned long long>(refDecode.lumaHash));
    printf("\n");

    if (refDecode.frames <= 0) {
        /*
         * 基准都解不出帧 ⇒ 问题在"解码器喂法"这一层，后面三个阶段的对比全部失去意义。
         * 明确在这里停，而不是让它以"②也是 0 帧"的形式混过去。
         */
        printf("FAIL: 明片参考一个帧都解不出来 —— 这一层是解码器喂法的问题，"
               "后面的阶段对比没有意义。%s\n", errNote(refDecode).c_str());
        return 1;
    }

    /* ---------------- 阶段①：不注册密钥 ---------------- */
    printf("===== 阶段①：不注册密钥（这是「有 CDM 的平台走硬解」那条路的形态）=====\n");
    DashRun s1;
    bool s1ok = readThroughDash(plainMpd, nullptr, std::string(), s1);

    if (!s1ok) {
        printf("FAIL: 阶段① 管线都没能读完：%s\n", s1.failReason.c_str());
        return 1;
    }

    std::string s1Form, s1ExtraForm, s1DecoderNote;
    const DecodeResult s1Decode = decodeAll(s1.payloads, s1.extradata, s1Form, s1ExtraForm, s1DecoderNote);
    dumpAnnexB("stage1_cipher.h264", s1.payloads, s1.extradata);   /* 交给 ffmpeg 独立数帧 */

    /*
     * 【本轮加的判据：阶段①的包到底是不是明文】
     *
     * 为什么必须有这一条：阶段①的读数出现了自相矛盾 —— 它报"解出 360 帧、亮度哈希与
     * 明片参考完全相同"，而阶段②a 的包与它 360/360 全不同（说明两者不可能都是明文）。
     * 只靠"包带 encryption info"**不能**判定"没被解密"：解密的实现只负责改字节，
     * 不负责清掉包上的 AV_PKT_DATA_ENCRYPTION_INFO，所以"带 encryption info"与
     * "字节是密文"是两件事。
     *
     * 判据：
     *   · 与明片参考**逐包完全相同** ⇒ 内核在**没有密钥**的情况下也把字节解开了
     *     ⇒ 这是**产品侧严重 bug**（软解抢了 CDM 那条路，且在无密钥时凭空"解"出明文）；
     *   · 完全相同 0 ⇒ 阶段①读到的确实是密文，那"360 帧"就是本 harness 解码侧的计数
     *     没按阶段重置（解出 0 帧才该是它的结果）。
     */
    {
        size_t refS1Same = 0, refS1Compared = 0;
        comparePayloads(refPayloads, s1.payloads, refS1Same, refS1Compared);
        printf("  [包层]   与明片参考逐包比较：比较 %zu，完全相同 %zu"
               "（完全相同 ⇒ 阶段①读到的就是明文 = 没密钥也解密了；0 ⇒ 它是密文）\n",
               refS1Compared, refS1Same);
    }

    printf("  [密钥层] 未注册任何密钥（见上面 setCencKey 那行：本阶段跳过）\n");
    printf("  [包层]   包数 %d，带 encryption info 的 %d，KID: %s\n",
           s1.packets, s1.encryptedPackets, kidSummary(s1.kids).c_str());
    printf("  [解码层] 包形态 %s / extradata %s（%zu 字节，来源：%s）\n",
           s1Form.c_str(), s1ExtraForm.c_str(), s1.extradata.size(), s1.extradataFrom.c_str());
    printf("  [解码层] 解出帧数 %d（期望 0）%s%s\n", s1Decode.frames,
           s1DecoderNote.empty() ? "" : (" [" + s1DecoderNote + "]").c_str(), errNote(s1Decode).c_str());

    if (s1.encryptedPackets != s1.packets || s1.packets == 0) {
        printf("  FAIL: 有包没有带 encryption info（%d/%d）—— 要么片源不是 CENC，"
               "要么内核把它当明文交出来了\n", s1.packets - s1.encryptedPackets, s1.packets);
        failures++;
    } else {
        printf("  OK: 每一个包都仍然带 encryption info（内核没有偷偷解密）\n");
    }

    if (s1Decode.frames != 0) {
        printf("  FAIL: 没有密钥却解出了 %d 帧 —— 软解在抢 CDM 那条路\n", s1Decode.frames);
        failures++;
    } else {
        printf("  OK: 没有密钥，一帧都解不出来\n");
    }

    printf("\n");

    /* ---------------- 阶段②a：显式注册密钥 ---------------- */
    /*
     * KID 取自**包自己带的** encryption info（而不是从 MPD 属性或命令行抄一个），
     * 这样走的正是运行时那条查表键 —— 抄错 KID 会变成"注册了但查不到"，
     * 那是这个 harness 最不希望出现的假阴性。
     */
    std::string kid;

    for (const auto &k : s1.kids) {
        if (!k.empty()) {
            kid = k;
            break;
        }
    }

    printf("===== 阶段②a：显式 setCencKey（KID 取自包上的 encryption info）=====\n");

    if (kid.empty()) {
        printf("FAIL: 阶段①的包里没有任何 KID，无法注册密钥\n");
        return 1;
    }

    printf("  使用 KID: %s\n", kid.c_str());

    DashRun s2a;
    bool s2aok = readThroughDash(plainMpd, key, kid, s2a);

    if (!s2aok) {
        printf("FAIL: 阶段②a 管线都没能读完：%s\n", s2a.failReason.c_str());
        return 1;
    }

    std::string s2aForm, s2aExtraForm, s2aDecoderNote;
    const DecodeResult s2aDecode = decodeAll(s2a.payloads, s2a.extradata, s2aForm, s2aExtraForm, s2aDecoderNote);
    dumpAnnexB("stage2a_decrypted.h264", s2a.payloads, s2a.extradata);   /* 交给 ffmpeg 独立数帧 */

    size_t s1s2aSame = 0, s1s2aCompared = 0;
    comparePayloads(s1.payloads, s2a.payloads, s1s2aSame, s1s2aCompared);

    printf("  [密钥层] 上面 setCencKey 的返回值（0 = 已登记）\n");
    printf("  [包层]   包数 %d，与阶段①逐包比较：比较 %zu，完全相同 %zu（期望 0）\n",
           s2a.packets, s1s2aCompared, s1s2aSame);
    printf("  [解码层] 包形态 %s / extradata %s（%zu 字节，来源：%s）\n",
           s2aForm.c_str(), s2aExtraForm.c_str(), s2a.extradata.size(), s2a.extradataFrom.c_str());
    printf("  [解码层] 解出帧数 %d，亮度哈希 0x%016llx %s%s\n",
           s2aDecode.frames, static_cast<unsigned long long>(s2aDecode.lumaHash),
           s2aDecoderNote.c_str(), errNote(s2aDecode).c_str());

    if (s1s2aCompared > 0 && s1s2aSame == s1s2aCompared) {
        printf("  FAIL: 阶段②a 的包与阶段①逐字节相同 —— 注册密钥没有改变任何字节，"
               "也就是说解密根本没发生\n");
        failures++;
    } else if (s1s2aCompared > 0) {
        printf("  OK: 注册密钥之后字节确实变了（%zu/%zu 个包不同）—— 解密发生了\n",
               s1s2aCompared - s1s2aSame, s1s2aCompared);
    }

    if (s2aDecode.frames <= 0) {
        printf("  FAIL: 注册了密钥却一帧都没解出来（密钥层/包层/解码层的证据见上面三行）\n");
        failures++;
    } else {
        printf("  OK: 解出 %d 帧\n", s2aDecode.frames);
    }

    printf("\n");

    /* ---------------- 阶段②b：产品自己按 licenseUrl 取密钥 ---------------- */
    /*
     * 这个阶段**故意不调用 setCencKey**。带 licenseUrl 的 MPD 会让
     * DashStream::needsSoftwareCencDecryption() 为真，于是 createDemuxer 里装上
     * setCencKeyResolver（DashStream.cpp:413-418）→ demuxer_service 在读到第一个
     * 带加密信息的包时回调 fetchSoftwareCencKey → ContentKeyFetcher::fetch(licenseUrl, ...)
     * → GET http://127.0.0.1:9101/key/<kid>。
     * 所以这里能出帧，就等于"产品自己从清单声明的地址取到了密钥并解开了"。
     */
    printf("===== 阶段②b：不手工注册密钥，让产品按 MPD 的 cenc:licenseUrl 自己取 =====\n");
    printf("  MPD: %s\n", drmMpd);

    DashRun s2b;
    bool s2bok = readThroughDash(drmMpd, nullptr, std::string(), s2b);

    if (!s2bok) {
        printf("FAIL: 阶段②b 管线都没能读完：%s\n", s2b.failReason.c_str());
        return 1;
    }

    std::string s2bForm, s2bExtraForm, s2bDecoderNote;
    const DecodeResult s2bDecode = decodeAll(s2b.payloads, s2b.extradata, s2bForm, s2bExtraForm, s2bDecoderNote);

    size_t s1s2bSame = 0, s1s2bCompared = 0;
    comparePayloads(s1.payloads, s2b.payloads, s1s2bSame, s1s2bCompared);
    size_t s2as2bSame = 0, s2as2bCompared = 0;
    comparePayloads(s2a.payloads, s2b.payloads, s2as2bSame, s2as2bCompared);

    printf("  [密钥层] **本进程一次 setCencKey 都没调**；密钥由产品自己按 licenseUrl 取"
           "（证据是上面内核日志里的 DASH CENC software decryption is set up / "
           "cannot set up software CENC decryption 那几行）\n");
    printf("  [包层]   包数 %d，带 encryption info 的 %d，KID: %s\n",
           s2b.packets, s2b.encryptedPackets, kidSummary(s2b.kids).c_str());
    printf("  [包层]   与阶段①比较：比较 %zu，完全相同 %zu（期望 0 = ②b 不是密文）\n",
           s1s2bCompared, s1s2bSame);
    printf("  [包层]   与阶段②a比较：比较 %zu，完全相同 %zu（期望 = 全部：两条路解出的明文一致）\n",
           s2as2bCompared, s2as2bSame);
    printf("  [解码层] 包形态 %s / extradata %s（%zu 字节，来源：%s）\n",
           s2bForm.c_str(), s2bExtraForm.c_str(), s2b.extradata.size(), s2b.extradataFrom.c_str());
    printf("  [解码层] 解出帧数 %d，亮度哈希 0x%016llx %s%s\n",
           s2bDecode.frames, static_cast<unsigned long long>(s2bDecode.lumaHash),
           s2bDecoderNote.c_str(), errNote(s2bDecode).c_str());

    if (s1s2bCompared > 0 && s1s2bSame == s1s2bCompared) {
        printf("  FAIL: 阶段②b 的包与阶段①（密文）逐字节相同 —— 产品没有解开它\n");
        failures++;
    }

    if (s2as2bCompared > 0 && s2as2bSame != s2as2bCompared) {
        printf("  FAIL: 阶段②b 与阶段②a 解出的明文不一致（%zu/%zu 相同）\n",
               s2as2bSame, s2as2bCompared);
        failures++;
    } else if (s2as2bCompared > 0) {
        printf("  OK: 自动取密钥与显式注册密钥解出**完全一样**的明文\n");
    }

    if (s2bDecode.frames <= 0) {
        printf("  FAIL: 产品按 licenseUrl 取密钥这条路没有解出帧\n");
        failures++;
    } else {
        printf("  OK: 解出 %d 帧\n", s2bDecode.frames);
    }

    printf("\n");

    /* ---------------- 与明片参考对齐 ---------------- */
    printf("===== 与明片参考对齐 =====\n");
    printf("  参考明片          : %d 帧，哈希 0x%016llx\n",
           refDecode.frames, static_cast<unsigned long long>(refDecode.lumaHash));
    printf("  ②a 显式注册密钥   : %d 帧，哈希 0x%016llx\n",
           s2aDecode.frames, static_cast<unsigned long long>(s2aDecode.lumaHash));
    printf("  ②b 产品自动取密钥 : %d 帧，哈希 0x%016llx\n",
           s2bDecode.frames, static_cast<unsigned long long>(s2bDecode.lumaHash));

    if (s2aDecode.frames != refDecode.frames || s2aDecode.lumaHash != refDecode.lumaHash) {
        printf("  FAIL: ②a 与明片参考不是同一批帧\n");
        failures++;
    } else {
        printf("  OK: ②a 与明片参考**帧数一致、逐帧亮度哈希一致**\n");
    }

    if (s2bDecode.frames != refDecode.frames || s2bDecode.lumaHash != refDecode.lumaHash) {
        printf("  FAIL: ②b 与明片参考不是同一批帧（若帧数一致而哈希不同，先怀疑"
               "「DASH 分片的样本边界与源文件不同」，而不是解密）\n");
        failures++;
    } else {
        printf("  OK: ②b 与明片参考**帧数一致、逐帧亮度哈希一致**\n");
    }

    printf("\n%s (%d failure%s)\n", failures == 0 ? "RESULT: PASS" : "RESULT: FAIL",
           failures, failures == 1 ? "" : "s");

    return failures == 0 ? 0 : 1;
}
