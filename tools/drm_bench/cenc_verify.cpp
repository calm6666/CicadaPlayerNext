// Independent verification that CicadaPlayerNext's CENCDecrypter recovers the
// ORIGINAL samples from real Common Encryption content.
//
// Method: the same source is muxed twice, once clear and once with FFmpeg's
// `-encryption_scheme cenc-aes-ctr`.  Both carry identical sample sizes and
// timestamps, so:
//
//     decrypt(CENC sample i, IV and subsamples from its senc side data)
//         ==  clear sample i            (byte for byte)
//
// is a one-to-one proof against content this repository did not produce.  It
// uses FFmpeg only to demux and to hand over the per-sample encryption
// side data; all decryption is done by the repository's own class.
#define LOG_TAG "cencverify"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <map>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/packet.h>
#include <libavutil/encryption_info.h>
#include <libavutil/mem.h>
// Needed by the rival-hypothesis reference decryptors below.  It must be inside
// this extern "C" block: including it later (outside) would declare the AES
// entry points with C++ linkage and the link would fail on mangled names.
#include <libavutil/aes.h>
}

#include "demuxer/sample_decrypt/CENCDecrypter.h"

extern "C" int __log_print(int, const char *, const char *, ...)
{
    return 0;
}

using namespace Cicada;

// ---------------------------------------------------------------------------
// Reference CTR decryptors used to decide BETWEEN RIVAL COUNTER HYPOTHESES.
//
// The repository's CENCDecrypter is the thing under test; these exist only so a
// mismatch can be attributed by measurement instead of guesswork.  Both are
// built on FFmpeg's raw AES block function, independently of the class under
// test.
// ---------------------------------------------------------------------------
namespace {
struct RefCtr {
    AVAES *aes = nullptr;
    uint8_t counter[16];

    explicit RefCtr(const uint8_t *key)
    {
        aes = av_aes_alloc();
        av_aes_init(aes, key, 128, 0);
    }

    ~RefCtr()
    {
        av_free(aes);
    }

    void arm(const uint8_t *iv, uint32_t ivSize)
    {
        memset(counter, 0, 16);
        memcpy(counter, iv, ivSize);
    }

    void nextBlock(uint8_t *ks)
    {
        av_aes_crypt(aes, ks, counter, 1, nullptr, 0);

        for (int i = 15; i >= 8; --i) {
            if (++counter[i] != 0) {
                break;
            }
        }
    }
};

// Hypothesis A: the block counter runs continuously across the whole sample,
// and clear ranges do not advance it.
static void ctrContinuous(RefCtr &c, const uint8_t *iv, uint32_t ivSize,
                          const std::vector<SubsampleInfo> &subs, uint8_t *buf)
{
    c.arm(iv, ivSize);
    uint8_t ks[16];
    size_t off = 0;

    for (auto &s : subs) {
        off += s.clearBytes;                    // clear bytes: skipped, counter untouched

        for (size_t i = 0; i < s.protectedBytes;) {
            c.nextBlock(ks);
            size_t n = std::min<size_t>(16, s.protectedBytes - i);

            for (size_t k = 0; k < n; k++) {
                buf[off + i + k] ^= ks[k];
            }

            i += n;
        }

        off += s.protectedBytes;
    }
}

// Hypothesis B: the counter restarts at zero for every protected range.
static void ctrResetPerSubsample(RefCtr &c, const uint8_t *iv, uint32_t ivSize,
                                 const std::vector<SubsampleInfo> &subs, uint8_t *buf)
{
    uint8_t ks[16];
    size_t off = 0;

    for (auto &s : subs) {
        off += s.clearBytes;
        c.arm(iv, ivSize);                      // <-- the only difference

        for (size_t i = 0; i < s.protectedBytes;) {
            c.nextBlock(ks);
            size_t n = std::min<size_t>(16, s.protectedBytes - i);

            for (size_t k = 0; k < n; k++) {
                buf[off + i + k] ^= ks[k];
            }

            i += n;
        }

        off += s.protectedBytes;
    }
}
// Hypothesis C: the whole sample is one continuous encrypted range; the senc
// subsample list is metadata only and does NOT create clear regions.
static void ctrWholeSample(RefCtr &c, const uint8_t *iv, uint32_t ivSize,
                           uint8_t *buf, size_t size)
{
    c.arm(iv, ivSize);
    uint8_t ks[16];

    for (size_t i = 0; i < size;) {
        c.nextBlock(ks);
        size_t n = std::min<size_t>(16, size - i);

        for (size_t k = 0; k < n; k++) {
            buf[i + k] ^= ks[k];
        }

        i += n;
    }
}

// Hypothesis E: range 0 starts at counter 0 (which is known to be right, since
// A and B both reproduce the first protected range), and range 1 starts at an
// explicitly supplied counter.  Sweeping this value says exactly what the
// encryptor did, instead of leaving it to guesswork.
static void ctrSecondRangeAt(RefCtr &c, const uint8_t *iv, uint32_t ivSize,
                             const std::vector<SubsampleInfo> &subs, uint8_t *buf,
                             uint64_t secondRangeCounter)
{
    uint8_t ks[16];
    size_t off = 0;

    for (size_t r = 0; r < subs.size(); r++) {
        off += subs[r].clearBytes;
        c.arm(iv, ivSize);

        if (r == 1) {
            // Set the low 8 bytes of the counter block to the supplied value.
            for (int b = 0; b < 8; b++) {
                c.counter[8 + b] = static_cast<uint8_t>((secondRangeCounter >> (8 * (7 - b))) & 0xFF);
            }
        }

        for (size_t i = 0; i < subs[r].protectedBytes;) {
            c.nextBlock(ks);
            size_t n = std::min<size_t>(16, subs[r].protectedBytes - i);

            for (size_t k = 0; k < n; k++) {
                buf[off + i + k] ^= ks[k];
            }

            i += n;
        }

        off += subs[r].protectedBytes;
    }
}
} // namespace

struct Sample {
    std::vector<uint8_t> data;
    int64_t pts = 0;
};

// Drains one input into memory, keyed by stream index then read order.
static bool readAll(const char *path, std::map<int, std::vector<Sample>> &out, int *nbStreams)
{
    AVFormatContext *ctx = nullptr;

    if (avformat_open_input(&ctx, path, nullptr, nullptr) < 0) {
        printf("cannot open %s\n", path);
        return false;
    }

    if (avformat_find_stream_info(ctx, nullptr) < 0) {
        printf("no stream info for %s\n", path);
        avformat_close_input(&ctx);
        return false;
    }

    *nbStreams = static_cast<int>(ctx->nb_streams);

    AVPacket *pkt = av_packet_alloc();

    while (av_read_frame(ctx, pkt) >= 0) {
        Sample s;
        s.data.assign(pkt->data, pkt->data + pkt->size);
        s.pts = pkt->pts;
        out[pkt->stream_index].push_back(std::move(s));
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    avformat_close_input(&ctx);
    return true;
}

// A second pass that keeps the encryption side data, which av_read_frame
// normally leaves attached but which we need to pair with the sample order.
struct EncSample {
    std::vector<uint8_t> data;
    std::vector<uint8_t> keyId;
    std::vector<uint8_t> iv;
    std::vector<SubsampleInfo> subs;
    std::string scheme;
    int64_t pts = 0;
};

static bool readEncrypted(const char *path, std::map<int, std::vector<EncSample>> &out, int *nbStreams)
{
    AVFormatContext *ctx = nullptr;

    if (avformat_open_input(&ctx, path, nullptr, nullptr) < 0) {
        printf("cannot open %s\n", path);
        return false;
    }

    if (avformat_find_stream_info(ctx, nullptr) < 0) {
        avformat_close_input(&ctx);
        return false;
    }

    *nbStreams = static_cast<int>(ctx->nb_streams);

    AVPacket *pkt = av_packet_alloc();

    while (av_read_frame(ctx, pkt) >= 0) {
        EncSample s;
        s.data.assign(pkt->data, pkt->data + pkt->size);
        s.pts = pkt->pts;

        size_t sz = 0;
        const uint8_t *raw = av_packet_get_side_data(pkt, AV_PKT_DATA_ENCRYPTION_INFO, &sz);

        if (raw != nullptr && sz > 0) {
            AVEncryptionInfo *info = av_encryption_info_get_side_data(raw, sz);

            if (info != nullptr) {
                if (info->scheme == MKBETAG('c', 'e', 'n', 'c')) {
                    s.scheme = "cenc";
                } else if (info->scheme == MKBETAG('c', 'e', 'n', 's')) {
                    s.scheme = "cens";
                } else if (info->scheme == MKBETAG('c', 'b', 'c', '1')) {
                    s.scheme = "cbc1";
                } else if (info->scheme == MKBETAG('c', 'b', 'c', 's')) {
                    s.scheme = "cbcs";
                }

                s.keyId.assign(info->key_id, info->key_id + info->key_id_size);
                s.iv.assign(info->iv, info->iv + info->iv_size);

                if (info->subsample_count > 0) {
                    for (uint32_t i = 0; i < info->subsample_count; i++) {
                        s.subs.push_back(SubsampleInfo{
                            info->subsamples[i].bytes_of_clear_data,
                            info->subsamples[i].bytes_of_protected_data
                        });
                    }
                }

                av_encryption_info_free(info);
            }
        }

        out[pkt->stream_index].push_back(std::move(s));
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    avformat_close_input(&ctx);
    return true;
}

static std::string hex(const std::vector<uint8_t> &v)
{
    static const char *d = "0123456789abcdef";
    std::string s;

    for (uint8_t b : v) {
        s.push_back(d[(b >> 4) & 0xF]);
        s.push_back(d[b & 0xF]);
    }

    return s;
}

// Offset of the first byte where two buffers differ, or the common length when
// one is a prefix of the other.  Used to characterise a hypothesis failure
// precisely instead of just reporting "does not match".
static size_t firstDiff(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b)
{
    size_t n = a.size() < b.size() ? a.size() : b.size();

    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return i;
        }
    }

    return n;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        printf("usage: cencverify <clear.mp4> <cenc.mp4> <keyhex>\n");
        return 2;
    }

    const char *clearPath = argv[1];
    const char *cencPath = argv[2];
    const std::string keyHex = argv[3];

    if (keyHex.size() != 32) {
        printf("key must be 32 hex chars\n");
        return 2;
    }

    uint8_t key[16];
    for (int i = 0; i < 16; i++) {
        key[i] = static_cast<uint8_t>(std::stoul(keyHex.substr(i * 2, 2), nullptr, 16));
    }

    std::map<int, std::vector<Sample>> clear;
    std::map<int, std::vector<EncSample>> enc;
    int nbClear = 0, nbEnc = 0;

    if (!readAll(clearPath, clear, &nbClear)) {
        return 1;
    }

    if (!readEncrypted(cencPath, enc, &nbEnc)) {
        return 1;
    }

    printf("clear: %d streams, cenc: %d streams\n", nbClear, nbEnc);

    int totalSamples = 0, compared = 0, identical = 0, mismatched = 0;
    // Counts samples that carry no encryption side data at all (parameter sets
    // and similar), which must therefore already equal the clear bytes.
    int cleartextSamples = 0;
    CENCDecrypter dec;

    for (auto &kv : enc) {
        int idx = kv.first;

        if (clear.find(idx) == clear.end()) {
            continue;
        }

        std::vector<Sample> &ref = clear[idx];
        std::vector<EncSample> &es = kv.second;

        if (ref.size() != es.size()) {
            printf("stream %d: sample count differs (clear %zu, cenc %zu) -- comparing the common prefix\n",
                   idx, ref.size(), es.size());
        }

        size_t n = ref.size() < es.size() ? ref.size() : es.size();

        for (size_t i = 0; i < n; i++) {
            totalSamples++;

            if (es[i].scheme.empty()) {
                // Not an encrypted sample (e.g. the clear init/parameter-set
                // packets).  It must therefore already match the clear file.
                if (es[i].data == ref[i].data) {
                    identical++;
                } else {
                    mismatched++;
                }

                compared++;
                cleartextSamples++;
                continue;
            }

            const std::string keyId = hex(es[i].keyId);

            if (!dec.hasKey(keyId)) {
                int r = dec.setKey(keyId, key, 16);

                if (r != 0) {
                    printf("setKey(%s) failed: %d\n", keyId.c_str(), r);
                    return 1;
                }
            }

            std::vector<uint8_t> work = es[i].data;
            int64_t got = dec.decrypt(keyId, es[i].scheme, es[i].iv.data(),
                                      static_cast<uint32_t>(es[i].iv.size()),
                                      es[i].subs, 1, 9, work.data(),
                                      static_cast<int64_t>(work.size()));

            if (got < 0) {
                printf("stream %d sample %zu: decrypt failed %lld (scheme=%s iv=%zu)\n",
                       idx, i, (long long) got, es[i].scheme.c_str(), es[i].iv.size());
                return 1;
            }

            compared++;

            if (work == ref[i].data) {
                identical++;
            } else {
                mismatched++;

                if (mismatched <= 3) {
                    printf("stream %d sample %zu: MISMATCH (size %zu vs %zu, scheme=%s, iv=%s)\n",
                           idx, i, work.size(), ref[i].data.size(), es[i].scheme.c_str(),
                           hex(es[i].iv).c_str());
                    printf("    subsamples: %zu%s", es[i].subs.size(),
                           es[i].subs.empty() ? "  (empty => whole sample treated as protected)\n" : "\n");
                    size_t shown = es[i].subs.size() < 8 ? es[i].subs.size() : 8;

                    for (size_t k = 0; k < shown; k++) {
                        printf("      [%zu] clear=%u protected=%u\n", k,
                               es[i].subs[k].clearBytes, es[i].subs[k].protectedBytes);
                    }

                    size_t sum = 0;

                    for (auto &s : es[i].subs) {
                        sum += s.clearBytes + s.protectedBytes;
                    }

                    printf("    subsample sum=%zu sample size=%zu (%s)\n",
                           sum, es[i].data.size(),
                           sum == es[i].data.size() ? "exact" : "MISMATCH -- subsamples do not cover the sample");

                    // Attribute the failure by measurement: run both rival
                    // counter hypotheses over the same ciphertext and see which
                    // one reproduces the clear original.
                    {
                        RefCtr refCtr(key);
                        std::vector<uint8_t> a = es[i].data;
                        std::vector<uint8_t> b = es[i].data;
                        ctrContinuous(refCtr, es[i].iv.data(),
                                      static_cast<uint32_t>(es[i].iv.size()), es[i].subs, a.data());
                        ctrResetPerSubsample(refCtr, es[i].iv.data(),
                                             static_cast<uint32_t>(es[i].iv.size()), es[i].subs, b.data());
                        printf("    hypothesis A (counter continuous across subsamples): %s (first diff %zu)\n",
                               a == ref[i].data ? "MATCHES the clear original" : "does not match", firstDiff(a, ref[i].data));
                        printf("    hypothesis B (counter reset per subsample)         : %s (first diff %zu)\n",
                               b == ref[i].data ? "MATCHES the clear original" : "does not match", firstDiff(b, ref[i].data));

                        std::vector<uint8_t> c2 = es[i].data;
                        ctrWholeSample(refCtr, es[i].iv.data(),
                                       static_cast<uint32_t>(es[i].iv.size()), c2.data(), c2.size());
                        printf("    hypothesis C (whole sample one range, no clear)    : %s (first diff %zu)\n",
                               c2 == ref[i].data ? "MATCHES the clear original" : "does not match", firstDiff(c2, ref[i].data));

                        // Sweep the second range's starting counter and report
                        // which value reproduces the original.  This turns "my
                        // model is wrong" into a specific number.
                        if (es[i].subs.size() >= 2) {
                            for (uint64_t ctr = 30; ctr <= 50; ctr++) {
                                std::vector<uint8_t> e = es[i].data;
                                ctrSecondRangeAt(refCtr, es[i].iv.data(),
                                                 static_cast<uint32_t>(es[i].iv.size()),
                                                 es[i].subs, e.data(), ctr);

                                if (e == ref[i].data) {
                                    printf("    range 1 starts at counter %llu -- THAT is what the encryptor did\n",
                                           (unsigned long long) ctr);
                                }
                            }
                        }
                    }

                    // Where do the two buffers first differ?  If the first
                    // difference is at a byte offset covered by a clear range,
                    // the bug is in how clear ranges are applied.
                    size_t d = 0;
                    while (d < work.size() && d < ref[i].data.size() && work[d] == ref[i].data[d]) {
                        d++;
                    }
                    printf("    first differing byte offset = %zu\n", d);
                }
            }
        }
    }

    printf("\nstreams compared        : %zu\n", enc.size());
    printf("samples total           : %d\n", totalSamples);
    printf("samples compared        : %d\n", compared);
    printf("  of which encrypted    : %d\n", compared - cleartextSamples);
    printf("  of which clear        : %d\n", cleartextSamples);
    printf("BYTE-IDENTICAL to clear : %d\n", identical);
    printf("mismatched              : %d\n", mismatched);

    if (mismatched == 0 && identical > 0) {
        printf("\nRESULT: PASS -- every sample decrypted back to the original clear bytes\n");
        return 0;
    }

    printf("\nRESULT: FAIL\n");
    return 1;
}
