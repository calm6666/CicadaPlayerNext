// Verification harness for the NEW DASH <ContentProtection> parsing.
//
// It drives the repository's real MPDParser over a hand-written MPD and prints
// what each Representation ended up carrying.  This is the only way to check the
// parser short of playing a protected DASH stream end to end, and it is
// deliberately built on the production class rather than a copy of its logic.
#define LOG_TAG "mpddrmcheck"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#include "demuxer/dash/MPDParser.h"
#include "demuxer/play_list/AdaptationSet.h"
#include "demuxer/play_list/Period.h"
#include "demuxer/play_list/Representation.h"
#include "demuxer/play_list/playList.h"
#include "data_source/dataSourceIO.h"

using namespace Cicada;

// NOTE: no __log_print stub here.  Unlike the standalone decrypter benchmarks,
// this harness links framework_utils (the parser lives above it), so the real
// logging symbol is present and defining a stub would be a duplicate symbol.

// A read/seek pair over an in-memory buffer, so the parser needs no file or
// network.  The contract (0 == clean EOF) matches what HLSStream::read_callback
// and friends provide.
struct MemSource {
    const uint8_t *data;
    size_t size;
    size_t pos = 0;

    static int read(void *arg, uint8_t *buffer, int size)
    {
        auto *self = static_cast<MemSource *>(arg);
        size_t left = self->size - self->pos;
        size_t n = left < static_cast<size_t>(size) ? left : static_cast<size_t>(size);

        if (n == 0) {
            return 0;
        }

        memcpy(buffer, self->data + self->pos, n);
        self->pos += n;
        return static_cast<int>(n);
    }

    static int64_t seek(void *arg, int64_t offset, int whence)
    {
        auto *self = static_cast<MemSource *>(arg);

        if (whence == SEEK_SET) {
            self->pos = static_cast<size_t>(offset);
        } else if (whence == SEEK_CUR) {
            self->pos = static_cast<size_t>(static_cast<int64_t>(self->pos) + offset);
        } else if (whence == SEEK_END) {
            self->pos = static_cast<size_t>(static_cast<int64_t>(self->size) + offset);
        } else {
            return -1;
        }

        return static_cast<int64_t>(self->pos);
    }
};

static int g_failures = 0;

static void check(bool ok, const char *what)
{
    printf("  [%s] %s\n", ok ? "OK" : "FAIL", what);

    if (!ok) {
        g_failures++;
    }
}

int main()
{
    /*
     * The MPD deliberately exercises every case the parser has to get right:
     *   · an AdaptationSet-level ContentProtection that BOTH of its
     *     Representations must inherit;
     *   · a Representation-level one that must NOT leak to its sibling
     *     (this is what the recursion bug would have caused);
     *   · a self-hosted / second scheme whose license URL lives in an attribute;
     *   · an EMPTY <ContentProtection/> placeholder that must be ignored.
     */
    static const char *kMpd =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\"\n"
        "     xmlns:cenc=\"urn:mpeg:cenc:2013\"\n"
        "     type=\"static\" mediaPresentationDuration=\"PT10S\" minBufferTime=\"PT2S\"\n"
        "     profiles=\"urn:mpeg:dash:profile:isoff-live:2011\">\n"
        "  <Period id=\"0\" start=\"PT0S\">\n"
        "    <AdaptationSet mimeType=\"video/mp4\" segmentAlignment=\"true\">\n"
        "      <ContentProtection schemeIdUri=\"urn:mpeg:dash:mp4protection:2011\"\n"
        "                         value=\"cenc\" cenc:default_KID=\"1077efec-c0b2-4d02-ace3-3c1e52e2fb4b\"/>\n"
        "      <ContentProtection schemeIdUri=\"urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed\"\n"
        "                         cenc:licenseUrl=\"https://license.example.com/widevine\">\n"
        "        <cenc:pssh>AAAAVnBzc2gAAAAA7e+LqXnWSs6jyCfc1R0h7Q==</cenc:pssh>\n"
        "      </ContentProtection>\n"
        "      <ContentProtection/>\n"
        "      <Representation id=\"v0\" width=\"640\" height=\"360\" bandwidth=\"800000\" codecs=\"avc1.64001f\">\n"
        "        <BaseURL>v0.mp4</BaseURL>\n"
        "        <SegmentBase indexRangeExact=\"true\" indexRange=\"0-100\"><Initialization range=\"0-50\"/></SegmentBase>\n"
        "      </Representation>\n"
        "      <Representation id=\"v1\" width=\"1280\" height=\"720\" bandwidth=\"2400000\" codecs=\"avc1.64001f\">\n"
        "        <ContentProtection schemeIdUri=\"urn:example:selfhosted\"\n"
        "                           cenc:default_KID=\"00112233445566778899aabbccddeeff\"\n"
        "                           licenseUrl=\"http://127.0.0.1:9310/key/00112233445566778899aabbccddeeff\"/>\n"
        "        <BaseURL>v1.mp4</BaseURL>\n"
        "        <SegmentBase indexRangeExact=\"true\" indexRange=\"0-100\"><Initialization range=\"0-50\"/></SegmentBase>\n"
        "      </Representation>\n"
        "    </AdaptationSet>\n"
        "    <AdaptationSet mimeType=\"audio/mp4\" lang=\"en\">\n"
        "      <Representation id=\"a0\" bandwidth=\"128000\" codecs=\"mp4a.40.2\">\n"
        "        <BaseURL>a0.mp4</BaseURL>\n"
        "        <SegmentBase indexRangeExact=\"true\" indexRange=\"0-100\"><Initialization range=\"0-50\"/></SegmentBase>\n"
        "      </Representation>\n"
        "    </AdaptationSet>\n"
        "  </Period>\n"
        "</MPD>\n";

    const std::string mpdText = kMpd;
    MemSource source{reinterpret_cast<const uint8_t *>(mpdText.data()), mpdText.size(), 0};

    Cicada::Dash::MPDParser parser("mem://test.mpd");    parser.SetDataCallBack(MemSource::read, MemSource::seek, &source);

    playList *list = parser.parse("mem://test.mpd");

    if (list == nullptr) {
        printf("FATAL: MPDParser::parse returned null\n");
        return 1;
    }

    printf("MPDParser produced a playList; checking ContentProtection\n");

    int videoReps = 0, audioReps = 0;

    for (Period *period : list->GetPeriods()) {
        if (period == nullptr) {
            continue;
        }

        for (AdaptationSet *as : period->GetAdaptSets()) {
            if (as == nullptr) {
                continue;
            }

            for (Representation *rep : as->getRepresentations()) {
                if (rep == nullptr) {
                    continue;
                }

                const bool isVideo = rep->getMimeType().find("video") != std::string::npos;

                if (isVideo) {
                    videoReps++;
                } else {
                    audioReps++;
                }

                const std::vector<Representation::ContentProtection> &cps = rep->getContentProtections();
                printf("\n  repo %s (%s) : %zu protection entr%s, hasContentProtection=%d\n",
                       rep->getID().c_str(), rep->getMimeType().c_str(), cps.size(),
                       cps.size() == 1 ? "y" : "ies", rep->hasContentProtection() ? 1 : 0);

                for (size_t i = 0; i < cps.size(); i++) {
                    const Representation::ContentProtection &cp = cps[i];
                    printf("      [%zu] schemeIdUri=%s\n", i,
                           cp.schemeIdUri.empty() ? "(empty)" : cp.schemeIdUri.c_str());
                    printf("          value=%s keyId=%s\n",
                           cp.value.empty() ? "(empty)" : cp.value.c_str(),
                           cp.keyId.empty() ? "(empty)" : cp.keyId.c_str());
                    printf("          pssh=%s\n", cp.pssh.empty() ? "(empty)" : cp.pssh.c_str());
                    printf("          licenseUrl=%s\n",
                           cp.licenseUrl.empty() ? "(empty)" : cp.licenseUrl.c_str());
                }

                // ---- assertions -------------------------------------------------
                if (rep->getID() == "v0") {
                    // Inherited the AdaptationSet's two entries; the empty
                    // placeholder must NOT have produced a third.
                    check(cps.size() == 2, "v0 inherits exactly the 2 AdaptationSet entries (empty placeholder ignored)");
                    check(rep->hasContentProtection(), "v0 reports hasContentProtection()");
                    check(!cps.empty() && cps[0].schemeIdUri == "urn:mpeg:dash:mp4protection:2011",
                          "v0 entry 0 is mp4protection");
                    check(!cps.empty() && cps[0].keyId == "1077efec-c0b2-4d02-ace3-3c1e52e2fb4b",
                          "v0 default_KID parsed (with hyphens, verbatim)");
                    check(!cps.empty() && cps[0].value == "cenc", "v0 @value parsed");
                    check(cps.size() >= 2 && cps[1].schemeIdUri == "urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed",
                          "v0 entry 1 is the Widevine scheme");
                    check(cps.size() >= 2 && cps[1].licenseUrl == "https://license.example.com/widevine",
                          "v0 Widevine licenseUrl parsed");
                    check(cps.size() >= 2 && cps[1].pssh == "AAAAVnBzc2gAAAAA7e+LqXnWSs6jyCfc1R0h7Q==",
                          "v0 <cenc:pssh> text parsed");
                } else if (rep->getID() == "v1") {
                    // v1 has its OWN entry; it must ALSO inherit the two from the
                    // AdaptationSet (those come first, so the later Rep-level one
                    // wins when the consumer reads back-to-front).
                    check(cps.size() == 3, "v1 has the 2 inherited AS entries plus its own");
                    check(cps.size() == 3 && cps[2].schemeIdUri == "urn:example:selfhosted",
                          "v1's own Rep-level entry comes LAST (so Rep overrides AS)");
                    check(cps.size() == 3 && cps[2].licenseUrl == "http://127.0.0.1:9310/key/00112233445566778899aabbccddeeff",
                          "v1 self-hosted key URL parsed from the bare licenseUrl attribute");
                    check(cps.size() == 3 && cps[2].keyId == "00112233445566778899aabbccddeeff",
                          "v1 bare default_KID (no cenc: prefix) parsed");
                } else if (rep->getID() == "a0") {
                    // The audio AS declares no protection: a0 must carry none.
                    // This is the assertion that catches the old recursion bug
                    // (which would have attached the video AS's entries here).
                    check(cps.empty(), "a0 (unprotected audio) carries no ContentProtection");
                    check(!rep->hasContentProtection(), "a0 reports hasContentProtection() == false");
                }
            }
        }
    }

    printf("\nvideo representations: %d, audio: %d\n", videoReps, audioReps);
    check(videoReps == 2, "found both video representations");
    check(audioReps == 1, "found the audio representation");

    printf("\n%s (%d failure%s)\n", g_failures == 0 ? "RESULT: PASS" : "RESULT: FAIL",
           g_failures, g_failures == 1 ? "" : "s");

    delete list;
    return g_failures == 0 ? 0 : 1;
}
