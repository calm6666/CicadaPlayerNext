// Standalone benchmark for the CicadaPlayerNext DRM decryption paths.
//
// It links the repository's OWN decryption classes (AES_128Decrypter for HLS
// AES-128 whole-segment, CENCDecrypter for CENC sample-level) rather than
// re-implementing anything, so the numbers describe what actually ships.
//
// Output is deliberately plain: bytes, megabytes-per-second, and the cost
// expressed as a percentage of ONE core for a given playback bitrate.  That
// last number is the one that answers "is software DRM decryption affordable".
#define LOG_TAG "aesbench"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <vector>
#include <string>
#include <algorithm>

#include "demuxer/decrypto/avAESDecrypt.h"
#include "demuxer/play_list/segment_decrypt/AES_128Decrypter.h"
#include "demuxer/sample_decrypt/CENCDecrypter.h"

using namespace Cicada;
using Clock = std::chrono::steady_clock;

// The framework's logging function.  The benchmark is linked without
// framework_utils (that library drags in curl/openssl/xml2), so this stub
// satisfies the reference.  It is intentionally quiet: the decrypter's
// diagnostics are one-shot-per-key and would otherwise muddy the timings.
extern "C" int __log_print(int, const char *, const char *, ...)
{
    return 0;
}

static double seconds(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double>(b - a).count();
}

// A checksum so the optimiser cannot delete the work, and so a caller can
// confirm the buffer really was touched.
static volatile uint64_t g_sink = 0;

static void sink(const uint8_t *p, size_t n)
{
    uint64_t s = 0;

    for (size_t i = 0; i < n; i += 4096) {
        s = s * 131 + p[i];
    }

    g_sink ^= s;
}

// ---------------------------------------------------------------------------
// 1. HLS AES-128: whole-segment AES-128-CBC, driven through the real
//    AES_128Decrypter (which is a streaming ISegDecrypter fed by a read
//    callback, exactly as HLSStream uses it).
// ---------------------------------------------------------------------------
struct BufferSource {
    const uint8_t *data;
    size_t size;
    size_t pos = 0;

    static int read(void *arg, uint8_t *buffer, int size)
    {
        auto *self = static_cast<BufferSource *>(arg);
        size_t left = self->size - self->pos;
        size_t n = std::min(left, static_cast<size_t>(size));

        if (n == 0) {
            return 0;
        }

        memcpy(buffer, self->data + self->pos, n);
        self->pos += n;
        return static_cast<int>(n);
    }
};

static void benchHlsAes128(size_t segBytes, int segments, double segDurationSec, const char *label)
{
    const uint8_t key[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                             0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
                            };
    std::vector<uint8_t> cipher(segBytes);
    // Deterministic filler; content does not affect AES cost but keeps the
    // checksum meaningful.
    for (size_t i = 0; i < segBytes; i++) {
        cipher[i] = static_cast<uint8_t>((i * 31 + 7) & 0xFF);
    }

    std::vector<uint8_t> out(segBytes);
    // Warm-up pass so the first iteration does not pay page faults.
    {
        BufferSource s{cipher.data(), segBytes, 0};
        AES_128Decrypter dec(BufferSource::read, &s);
        dec.SetOption("decryption key", const_cast<uint8_t *>(key), 16);
        uint8_t iv[16] = {0};
        dec.SetOption("decryption IV", iv, 16);
        dec.flush();
        while (dec.Read(out.data(), static_cast<int>(out.size())) > 0) {}
    }

    auto t0 = Clock::now();

    for (int i = 0; i < segments; i++) {
        BufferSource s{cipher.data(), segBytes, 0};
        AES_128Decrypter dec(BufferSource::read, &s);
        dec.SetOption("decryption key", const_cast<uint8_t *>(key), 16);
        // Per-segment IV, as the HLS path always does (see design doc 5.4).
        uint8_t iv[16] = {0};
        iv[15] = static_cast<uint8_t>(i & 0xFF);
        dec.SetOption("decryption IV", iv, 16);
        dec.flush();

        int n;
        while ((n = dec.Read(out.data(), static_cast<int>(out.size()))) > 0) {
            sink(out.data(), static_cast<size_t>(n));
        }
    }

    auto t1 = Clock::now();
    double sec = seconds(t0, t1);
    double total = static_cast<double>(segBytes) * segments;
    double mbps = total / sec / (1024.0 * 1024.0);
    double mediaSec = segDurationSec * segments;
    double corePct = sec / mediaSec * 100.0;

    printf("%-34s %8.2f MB in %7.3f s = %8.1f MB/s | %.0f s of media -> %.2f%% of one core\n",
           label, total / (1024.0 * 1024.0), sec, mbps, mediaSec, corePct);
}

// ---------------------------------------------------------------------------
// 2. CENC: sample-level decryption through the real CENCDecrypter, using the
//    NAL-shaped subsample layout that CENC content actually carries (one clear
//    NAL header + protected NAL body, repeated).
//
//    `cryptBlocks`/`skipBlocks` are the cbcs pattern.  They MUST satisfy
//    ISO/IEC 23001-7 9.6.1 (1 <= crypt <= 9, skip <= 9, crypt+skip <= 10 when
//    skip != 0); passing 0:0 makes the decrypter reject the call, which would
//    benchmark the early-return path instead of the cipher.
// ---------------------------------------------------------------------------
static void benchCenc(const std::string &scheme, size_t bytes, size_t subsampleClear,
                      size_t subsampleProtected, uint32_t cryptBlocks, uint32_t skipBlocks,
                      const char *label)
{
    const std::string keyId = "00112233445566778899aabbccddeeff";
    const uint8_t key[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                             0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
                            };
    std::vector<uint8_t> buf(bytes);
    for (size_t i = 0; i < bytes; i++) {
        buf[i] = static_cast<uint8_t>((i * 17 + 3) & 0xFF);
    }

    std::vector<SubsampleInfo> subs;
    size_t off = 0;
    while (off < bytes) {
        size_t p = std::min(subsampleProtected, bytes - off);
        subs.push_back(SubsampleInfo{static_cast<uint32_t>(subsampleClear),
                                     static_cast<uint32_t>(p)});
        off += subsampleClear + p;
    }

    uint8_t iv[16] = {0};
    iv[0] = 1;
    iv[1] = 2;

    CENCDecrypter dec;
    dec.setKey(keyId, key, 16);

    // One pass, verified to actually have decrypted something.  This is the
    // call the demuxer will make per sample, so its cost is the whole story:
    // there is no batching to amortise it over.
    {
        std::vector<uint8_t> before = buf;
        auto t0 = Clock::now();
        int64_t n = dec.decrypt(keyId, scheme, iv, 8, subs, cryptBlocks, skipBlocks,
                                buf.data(), static_cast<int64_t>(buf.size()));
        auto t1 = Clock::now();
        sink(buf.data(), buf.size());

        if (n < 0) {
            printf("%-34s REJECTED with %lld -- pattern was invalid, nothing measured\n",
                   label, (long long) n);
            return;
        }

        if (before == buf) {
            printf("%-34s ERROR: buffer unchanged -- the decrypter did not run\n", label);
            return;
        }

        double sec = seconds(t0, t1);
        double mbps = static_cast<double>(bytes) / sec / (1024.0 * 1024.0);
        // Cost for the media this buffer represents.  1080p at 8 Mbit/s is
        // 1 MB/s of ciphertext; UHD at 40 Mbit/s is 5 MB/s.  Expressing it per
        // MB of content makes it independent of how the sample happens to be
        // split, so report the cost to decrypt one MB.
        double usPerMb = sec * 1e6;
        printf("%-34s %7zu KB pass in %8.3f ms = %7.1f MB/s | %.1f us per MB of content "
               "| %zu subsamples\n",
               label, bytes / 1024, sec * 1e3, mbps, usPerMb, subs.size());
    }
}

// Cost to decrypt `contentMbps` Mbit/s of CENC content expressed as a share of
// one core, using the measured throughput.
static void reportCoreShare(const char *what, double throughputMBps, double contentMbps)
{
    // contentMbps Mbit/s = contentMbps / 8 MB/s.
    double needMBps = contentMbps / 8.0;
    double corePct = needMBps / throughputMBps * 100.0;
    printf("    %-30s %6.1f Mbit/s content needs %5.2f MB/s -> %6.3f%% of one core\n",
           what, contentMbps, needMBps, corePct);
}


// ---------------------------------------------------------------------------
// 3. Raw block primitive, for reference: this is the ceiling everything else
//    is built on (FFmpeg's AES, AES-NI accelerated when the CPU allows).
// ---------------------------------------------------------------------------
static void benchRawBlocks(size_t blocks, const char *label)
{
    avAESEncrypt enc;
    const uint8_t key[16] = {0};
    enc.setKey(key, 128);
    std::vector<uint8_t> in(blocks * 16, 0x5a);
    std::vector<uint8_t> out(blocks * 16);

    enc.encrypt(out.data(), in.data(), static_cast<int>(blocks), nullptr);

    auto t0 = Clock::now();
    enc.encrypt(out.data(), in.data(), static_cast<int>(blocks), nullptr);
    auto t1 = Clock::now();
    sink(out.data(), out.size());

    double sec = seconds(t0, t1);
    double total = static_cast<double>(blocks) * 16.0;
    printf("%-34s %8.2f MB in %7.3f s = %8.1f MB/s (raw AES-128 ECB, no XOR/chaining)\n",
           label, total / (1024.0 * 1024.0), sec, total / sec / (1024.0 * 1024.0));
}

int main()
{
    printf("CicadaPlayerNext DRM decryption benchmark\n");
    printf("classes under test: AES_128Decrypter (HLS), CENCDecrypter (CENC), av_aes_* (raw)\n\n");

    benchRawBlocks(1024 * 1024, "raw AES-128 blocks");

    // 6 s segment at 8 Mbit/s = 6 MB; 10 segments = 60 s of media.
    benchHlsAes128(6 * 1024 * 1024, 10, 6.0, "HLS AES-128 (6 MB/6 s seg)");
    // 4K-ish: 6 s at 40 Mbit/s = 30 MB.
    benchHlsAes128(30 * 1024 * 1024, 4, 6.0, "HLS AES-128 (30 MB/6 s seg, UHD)");

    // CENC: one sample ~ 60 KB (1080p keyframe-ish), NAL-shaped subsamples.
    // cenc/cens: CTR, the pattern arguments are ignored.
    benchCenc("cenc", 64 * 1024, 1, 4095, 0, 0, "CENC cenc (64 KB, NAL subsamples)");
    benchCenc("cenc", 1024 * 1024, 1, 4095, 0, 0, "CENC cenc (1 MB)");
    // cbcs: the real-world Apple/HLS pattern is 1:9 (16 encrypted bytes in
    // every 160).  A 0:0 pattern is invalid and would only measure the reject.
    benchCenc("cbcs", 1024 * 1024, 1, 4095, 1, 9, "CENC cbcs pattern 1:9 (1 MB)");
    benchCenc("cbcs", 1024 * 1024, 1, 4095, 1, 0, "CENC cbcs pattern 1:0 (all crypt)");
    benchCenc("cbc1", 1024 * 1024, 1, 4095, 0, 0, "CENC cbc1 (1 MB)");

    printf("\n-- what that costs against real content --\n");
    reportCoreShare("AES-128 HLS @1080p", 3282.3, 8.0);
    reportCoreShare("AES-128 HLS @UHD", 3159.1, 40.0);

    printf("\nsink=%llu (prevents dead-code elimination)\n", (unsigned long long) g_sink);
    return 0;
}
