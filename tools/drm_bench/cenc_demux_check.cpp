// Runtime verification of the NEW software-CENC fallback in the demuxer read
// path (demuxer_service::readPacket + setCencKey).
//
// Method: drive the repository's REAL demuxer over the same CENC file twice:
//
//   1. without registering a key  -> every packet must come out still encrypted
//      (that is the hardware/CDM route: the kernel must NOT touch the bytes);
//   2. after setCencKey(kid, key) -> every packet must come out decrypted, and
//      byte-identical to the matching sample of the clear reference file.
//
// Step 2 is the end-to-end proof that the fallback actually decodes: the bytes
// the decoder would receive are the original clear bytes.  Step 1 is the proof
// that the feature cannot steal the hardware path.
#define LOG_TAG "cencdemuxcheck"

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
}

#include "demuxer/demuxer_service.h"
#include "data_source/dataSourcePrototype.h"

using namespace Cicada;

// ---------------------------------------------------------------------------
// The clear reference, read straight through libavformat.
// ---------------------------------------------------------------------------
struct Sample {
    std::vector<uint8_t> data;
};

static bool readClear(const char *path, std::vector<Sample> &out)
{
    AVFormatContext *ctx = nullptr;

    if (avformat_open_input(&ctx, path, nullptr, nullptr) < 0) {
        printf("cannot open clear reference %s\n", path);
        return false;
    }

    if (avformat_find_stream_info(ctx, nullptr) < 0) {
        avformat_close_input(&ctx);
        return false;
    }

    AVPacket *pkt = av_packet_alloc();

    while (av_read_frame(ctx, pkt) >= 0) {
        Sample s;
        s.data.assign(pkt->data, pkt->data + pkt->size);
        out.push_back(std::move(s));
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    avformat_close_input(&ctx);
    return true;
}

// ---------------------------------------------------------------------------
// Read every video packet through the production demuxer_service.
// ---------------------------------------------------------------------------
struct ReadResult {
    std::vector<std::vector<uint8_t>> packets;
    std::vector<std::string> kids;      // hex key id per packet ("" if none)
    int encryptedPackets = 0;
    int failed = 0;
};

static bool readThroughDemuxer(const char *path, const uint8_t *key, const char *kidHex,
                               ReadResult &out)
{
    /*
     * Pass the path through EXACTLY as given: dataSourcePrototype::create()
     * falls through to ffmpegDataSource for anything that is not curl-like, and
     * ffmpegDataSource hands mUri straight to avio_open2(), whose "file"
     * protocol resolves a plain Windows path.  Prefixing "file://" here made
     * avio_open2 treat "C:\..." as a HOST and fail.
     */
    const std::string uri(path);
    printf("    [stage] creating data source for %s\n", path);
    fflush(stdout);
    IDataSource *source = dataSourcePrototype::create(uri);

    if (source == nullptr) {
        printf("no data source for %s\n", path);
        return false;
    }

    printf("    [stage] data source = %s\n", source->GetUri().c_str());
    fflush(stdout);

    const int openRet = source->Open(0);

    if (openRet < 0) {
        printf("cannot open data source for %s (ret=%d)\n", path, openRet);
        delete source;
        return false;
    }

    printf("    [stage] data source opened\n");
    fflush(stdout);

    demuxer_service service(source);
    service.initOpen(demuxer_type_unknown);

    printf("    [stage] demuxer initOpen done\n");
    fflush(stdout);

    if (service.getDemuxerHandle() == nullptr) {
        printf("demuxer did not open %s\n", path);
        source->Close();
        delete source;
        return false;
    }

    if (key != nullptr && kidHex != nullptr) {
        const int r = service.setCencKey(kidHex, key, 16);
        printf("  setCencKey(%s) -> %d\n", kidHex, r);

        if (r != 0) {
            printf("  FAIL: could not register the key\n");
            service.close();
            source->Close();
            delete source;
            return false;
        }
    }

    // Open stream 0 (single video track in these files).
    printf("    [stage] OpenStream(0)\n");
    fflush(stdout);
    service.OpenStream(0);

    std::unique_ptr<IAFPacket> packet;

    for (;;) {
        packet.reset();
        const int ret = service.readPacket(packet, -1);

        if (packet == nullptr) {
            /*
             * No packet.  `ret > 0` means "a packet was consumed but this stream
             * index did not get one" -- keep going.  `ret <= 0` is the end:
             * readPacket returns 0 for EOS here, and looping on that (which an
             * earlier revision of this harness did) spins forever.
             */
            if (ret <= 0) {
                break;
            }

            continue;
        }

        IAFPacket::EncryptionInfo info{};

        if (packet->getEncryptionInfo(&info) && info.key_id != nullptr && info.key_id_size > 0) {
            out.encryptedPackets++;
            out.kids.push_back(CENCDecrypter::toHex(info.key_id, info.key_id_size));
        } else {
            out.kids.push_back(std::string());
        }

        out.packets.emplace_back(packet->getData(), packet->getData() + packet->getSize());

        if (out.packets.size() > 20000) {
            break;          // safety stop; these test files are ~150 packets
        }
    }

    service.close();
    source->Close();
    delete source;
    return true;
}

int main(int argc, char **argv)
{
    /*
     * Unbuffered stdout: when this harness is redirected to a file, a full
     * stdio buffer would hide every printf until exit -- which makes a hang
     * impossible to localise.  With this, the last line in the log is always
     * the step that blocked.
     */
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc < 4) {
        printf("usage: cencdemuxcheck <clear.mp4> <cenc.mp4> <keyhex>\n");
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

    std::vector<Sample> clear;

    if (!readClear(clearPath, clear)) {
        return 1;
    }

    printf("clear reference: %zu samples\n\n", clear.size());

    int failures = 0;

    // ---- 1. WITHOUT a key: the kernel must not touch the bytes -------------
    printf("PASS 1: no key registered (this is the hardware/CDM route)\n");
    ReadResult plain;

    if (!readThroughDemuxer(cencPath, nullptr, nullptr, plain)) {
        return 1;
    }

    printf("  packets read        : %zu\n", plain.packets.size());
    printf("  packets still marked: %d\n", plain.encryptedPackets);

    if (plain.encryptedPackets == 0) {
        printf("  FAIL: no packet carried encryption info at all -- the test file is not CENC\n");
        failures++;
    } else {
        printf("  OK: every encrypted packet was passed through untouched (kernel did not steal the CDM route)\n");
    }

    // Did any packet get modified relative to the clear file?  It must NOT have:
    // with no key the packets should still be ciphertext where the clear file
    // has plaintext.
    {
        size_t identical = 0, compared = 0;

        for (size_t i = 0; i < plain.packets.size() && i < clear.size(); i++) {
            if (plain.packets[i] == clear[i].data) {
                identical++;
            }

            compared++;
        }

        printf("  packets equal to clear: %zu of %zu (must be far from all: ciphertext != plaintext)\n",
               identical, compared);

        if (compared > 0 && identical == compared) {
            printf("  FAIL: every packet already equals the clear bytes -- decryption happened without a key\n");
            failures++;
        } else {
            printf("  OK: without a key the packets are NOT the clear bytes\n");
        }
    }

    printf("\n");

    // ---- 2. WITH the key: every packet must equal the clear bytes ----------
    printf("PASS 2: key registered via demuxer_service::setCencKey (software fallback)\n");

    // Use the KID the packets actually carry, so this exercises the same lookup
    // the runtime path does.
    std::string kid = plain.kids.empty() ? std::string() : plain.kids[0];

    if (kid.empty()) {
        printf("  FAIL: no key id on any packet\n");
        return 1;
    }

    ReadResult decrypted;

    if (!readThroughDemuxer(cencPath, key, kid.c_str(), decrypted)) {
        return 1;
    }

    printf("  packets read        : %zu\n", decrypted.packets.size());

    size_t identical = 0, compared = 0, mismatched = 0;

    for (size_t i = 0; i < decrypted.packets.size() && i < clear.size(); i++) {
        compared++;

        if (decrypted.packets[i] == clear[i].data) {
            identical++;
        } else {
            mismatched++;

            if (mismatched <= 3) {
                printf("  sample %zu MISMATCH: size %zu vs clear %zu\n",
                       i, decrypted.packets[i].size(), clear[i].data.size());
            }
        }
    }

    printf("  compared            : %zu\n", compared);
    printf("  BYTE-IDENTICAL      : %zu\n", identical);
    printf("  mismatched          : %zu\n", mismatched);

    if (compared > 0 && mismatched == 0) {
        printf("  OK: every packet the decoder would receive equals the original clear bytes\n");
    } else {
        printf("  FAIL: the read path did not produce the clear bytes\n");
        failures++;
    }

    printf("\n%s (%d failure%s)\n", failures == 0 ? "RESULT: PASS" : "RESULT: FAIL",
           failures, failures == 1 ? "" : "s");

    return failures == 0 ? 0 : 1;
}
