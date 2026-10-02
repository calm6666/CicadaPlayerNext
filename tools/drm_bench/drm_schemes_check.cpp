// Verification harness for framework/drm/DrmSchemes and
// framework/demuxer/sample_decrypt/ContentKeyFetcher.
//
// Two things are checked here, and both are checked against the **production**
// code rather than a copy of its logic:
//
//   1. DrmSchemes: the DASH-IF content-protection registry is complete and the
//      arguments that go on to Stream_meta.keyFormat are the canonical
//      "urn:uuid:<lowercase>" spelling, never a raw mp4protection string.
//   2. DrmSchemes::decide: the ranking rules actually produce the documented
//      answer — a platform CDM wins over a software-fetchable scheme, a scheme
//      with a key URL wins over a named system without one, and the
//      last-declared entry wins a tie (the DASH "Representation overrides
//      AdaptationSet" inheritance rule).
//
// A fake platform CDM is registered through DrmHandlerPrototype, which is how
// every real platform registers itself, so "handledByPlatform" is exercised
// exactly as it is on a device.
//
// Build (MSVC, from the repository root; see tools/drm_bench/README.md):
//   cl /nologo /EHsc /MT /O2 /utf-8 /I framework drm_schemes_check.cpp
//      framework/drm/DrmSchemes.cpp framework/drm/DrmHandlerPrototype.cpp
//      framework/drm/DrmHandler.cpp framework/drm/DrmInfo.cpp
//      framework/demuxer/sample_decrypt/ContentKeyFetcher.cpp
//      /Fe:drm_schemes_check.exe
//
// ContentKeyFetcher.cpp references dataSourcePrototype::create() and
// __log_print(); the harness never calls fetch(), so both are stubbed below
// (the same technique tools/drm_bench/drm_bench.cpp uses for the logging
// symbol). If the parse path is ever extended to touch the data source, this
// harness will fail to link, which is the intended early warning.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "drm/DrmSchemes.h"
#include "drm/DrmHandlerPrototype.h"
#include "drm/DrmInfo.h"
#include "demuxer/sample_decrypt/ContentKeyFetcher.h"

using namespace Cicada;

extern "C" int __log_print(int, const char *, const char *, ...)
{
    return 0;
}

// Stub for the data source factory: the harness only exercises parse(), which is
// a pure function over a byte buffer.
namespace Cicada {
    class IDataSource;
}
Cicada::IDataSource *dataSourcePrototype::create(const std::string &, const Cicada::options *, int)
{
    return nullptr;
}

static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const std::string &what)
{
    ++g_checks;

    if (ok) {
        printf("  ok   %s\n", what.c_str());
        return;
    }

    ++g_failures;
    printf("  FAIL %s\n", what.c_str());
}

static void checkEq(const std::string &got, const std::string &want, const std::string &what)
{
    if (got == want) {
        ++g_checks;
        printf("  ok   %s\n", what.c_str());
        return;
    }

    ++g_failures;
    printf("  FAIL %s\n         got  %s\n         want %s\n", what.c_str(), got.c_str(), want.c_str());
}

// ---------------------------------------------------------------------------
// A fake platform CDM: claims exactly the schemes listed in `accepted`.
// ---------------------------------------------------------------------------
static std::vector<std::string> g_accepted;

namespace Cicada {
    class FakeDrmHandler : public DrmHandler, private DrmHandlerPrototype {
    public:
        explicit FakeDrmHandler(const DrmInfo &info) : DrmHandler(info) {}

        static void install()
        {
            static FakeDrmHandler prototype(0);
            (void) prototype;
        }

    private:
        explicit FakeDrmHandler(int dummy) : DrmHandler(DrmInfo())
        {
            addPrototype(this);
        }

        DrmHandler *clone(const DrmInfo &info) override
        {
            return new FakeDrmHandler(info);
        }

        bool is_supported(const DrmInfo &info) override
        {
            const std::string scheme = DrmSchemes::canonical(info.format);

            for (std::size_t i = 0; i < g_accepted.size(); ++i) {
                if (g_accepted[i] == scheme) {
                    return true;
                }
            }

            return false;
        }
    };
}

static DrmSchemes::Candidate candidate(const char *scheme, const char *url = "",
                                       const char *pssh = "", const char *kid = "")
{
    DrmSchemes::Candidate c;
    c.schemeIdUri = scheme;
    c.licenseUrl = url;
    c.pssh = pssh;
    c.keyId = kid;
    return c;
}

int main()
{
    const std::string kWidevine = "urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed";
    const std::string kPlayReady = "urn:uuid:9a04f079-9840-4286-ab92-e65be0885f95";
    const std::string kFairPlay = "urn:uuid:94ce86fb-07ff-4f43-adb8-93d2fa968ca2";
    const std::string kClearKey = "urn:uuid:e2719d58-a985-b3c9-781a-b030af78d30e";
    const std::string kClearKeyHls = "urn:uuid:3ea8778f-7742-4bf9-b18b-e834b2acbd47";
    const std::string kMp4Protection = "urn:mpeg:dash:mp4protection:2011";

    // -----------------------------------------------------------------------
    printf("\n== 1. canonical(): every spelling a manifest may use ==\n");
    // -----------------------------------------------------------------------
    checkEq(DrmSchemes::canonical("urn:uuid:EDEF8BA9-79D6-4ACE-A3C8-27DCD51D21ED"), kWidevine,
            "uppercase urn:uuid: is lowercased");
    checkEq(DrmSchemes::canonical("  urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed  "), kWidevine,
            "surrounding whitespace is trimmed");
    checkEq(DrmSchemes::canonical("edef8ba9-79d6-4ace-a3c8-27dcd51d21ed"), kWidevine,
            "bare UUID gets the urn:uuid: prefix");
    checkEq(DrmSchemes::canonical("{edef8ba9-79d6-4ace-a3c8-27dcd51d21ed}"), kWidevine,
            "braces are stripped");
    checkEq(DrmSchemes::canonical("EDEF8BA979D64ACEA3C827DCD51D21ED"), kWidevine,
            "32 hex chars are dashed and prefixed");
    checkEq(DrmSchemes::canonical("com.widevine.alpha"), kWidevine, "EME alias com.widevine.alpha");
    checkEq(DrmSchemes::canonical("org.w3.clearkey"), kClearKey, "EME alias org.w3.clearkey");
    checkEq(DrmSchemes::canonical("com.microsoft.playready"), kPlayReady,
            "EME alias com.microsoft.playready");
    checkEq(DrmSchemes::canonical(kMp4Protection), kMp4Protection,
            "mp4protection is returned as-is (it is not a DRM system)");
    checkEq(DrmSchemes::canonical(""), "", "empty input stays empty");
    checkEq(DrmSchemes::canonical("urn:uuid:not-a-uuid"), "urn:uuid:not-a-uuid",
            "an unrecognised urn:uuid: is NOT rewritten into something else");

    // -----------------------------------------------------------------------
    printf("\n== 2. registry: the DASH-IF list is complete ==\n");
    // -----------------------------------------------------------------------
    // 28 protection-system UUIDs + 3 generic schemes (mp4protection,
    // CA_descriptor, SEA). If somebody deletes a row, this number changes.
    check(DrmSchemes::count() == 31, "registry holds 31 entries (28 systems + 3 generic)");

    struct Expect {
        const char *scheme;
        const char *name;
    };
    const Expect kSpotChecks[] = {
        {kWidevine.c_str(), "Widevine"},
        {kPlayReady.c_str(), "Microsoft PlayReady"},
        {kFairPlay.c_str(), "Apple FairPlay"},
        {kClearKey.c_str(), "W3C ClearKey (DASH-IF Clear Key)"},
        {"urn:uuid:1077efec-c0b2-4d02-ace3-3c1e52e2fb4b", "W3C Common PSSH box"},
        {"urn:uuid:5e629af5-38da-4063-8977-97ffbd9902d4", "Marlin Adaptive Streaming SP"},
        {"urn:uuid:adb41c24-2dbf-4a6d-958b-4457c0d27b95", "Nagra MediaAccess PRM 3.0"},
        {"urn:uuid:3d5e6d35-9b9a-41e8-b843-dd3c6e72c42c", "ChinaDRM"},
        {"urn:uuid:f239e769-efa3-4850-9c16-a903c6932efb", "Adobe Primetime DRM v4"},
        {"urn:uuid:80a6be7e-1448-4c37-9e70-d5aebe04c8d2", "Irdeto Content Protection"},
        {"urn:uuid:9a27dd82-fde2-4725-8cbc-4234aa06ec09", "Verimatrix VCAS"},
        {"urn:uuid:3ea8778f-7742-4bf9-b18b-e834b2acbd47", "Clear Key AES-128 (HLS CBC)"},
        {"urn:uuid:be58615b-19c4-4684-88b3-c8c57e99e957", "Clear Key SAMPLE-AES (HLS CBCS)"},
    };

    for (std::size_t i = 0; i < sizeof(kSpotChecks) / sizeof(kSpotChecks[0]); ++i) {
        checkEq(DrmSchemes::nameOf(kSpotChecks[i].scheme), kSpotChecks[i].name,
                std::string("registered: ") + kSpotChecks[i].scheme);
    }

    check(DrmSchemes::isKnown(kMp4Protection), "mp4protection is known");
    check(!DrmSchemes::isKnown("urn:uuid:00000000-0000-0000-0000-000000000000"),
          "an unregistered urn:uuid: is not 'known'");
    check(DrmSchemes::systemIdOf(kFairPlay) == "94ce86fb-07ff-4f43-adb8-93d2fa968ca2",
          "systemIdOf returns the bare UUID");
    check(DrmSchemes::systemIdOf(kMp4Protection).empty(),
          "systemIdOf is empty for a non-system scheme");

    // -----------------------------------------------------------------------
    printf("\n== 3. isCencCapable(): 'can the in-kernel decrypter touch it' ==\n");
    // -----------------------------------------------------------------------
    check(DrmSchemes::isCencCapable(kMp4Protection), "mp4protection is CENC content");
    check(DrmSchemes::isCencCapable(kWidevine), "Widevine is CENC content");
    check(DrmSchemes::isCencCapable(kClearKey), "ClearKey (DASH) is CENC content");
    check(!DrmSchemes::isCencCapable(kClearKeyHls),
          "Clear Key AES-128 (HLS CBC) is NOT CENC content");
    check(!DrmSchemes::isCencCapable("urn:mpeg:dash:13818:1:ca_descriptor:2011"),
          "MPEG-2 TS CA is NOT CENC content");
    check(!DrmSchemes::isCencCapable("urn:mpeg:dash:sea:2012"), "SEA is NOT CENC content");
    check(DrmSchemes::isCencCapable("urn:uuid:11111111-2222-3333-4444-555555555555"),
          "an unregistered urn:uuid: is treated as CENC content (forward compatible)");
    check(!DrmSchemes::isCencCapable("urn:example:something"), "an unknown non-UUID scheme is not");

    // -----------------------------------------------------------------------
    printf("\n== 4. decide(): hardware first, then software, never a guess ==\n");
    // -----------------------------------------------------------------------
    FakeDrmHandler::install();

    // 4a. No platform CDM at all: an mp4protection + licence URL manifest is
    //     playable in software, and keyFormat must stay a real system id.
    {
        g_accepted.clear();
        std::vector<DrmSchemes::Candidate> c;
        c.push_back(candidate(kMp4Protection.c_str(), "https://ka.example/key", "", "0011"));
        const DrmSchemes::Decision d = DrmSchemes::decide(c);
        check(d.usable && !d.handledByPlatform && d.index == 0,
              "no CDM: mp4protection + key URL is usable in software");
        checkEq(d.systemId, "", "no CDM: systemId is empty (no DRM system was named)");
    }

    // 4b. Same manifest, but a platform CDM that handles Widevine, and the
    //     manifest also names Widevine: hardware wins.
    {
        g_accepted.clear();
        g_accepted.push_back(kWidevine);
        std::vector<DrmSchemes::Candidate> c;
        c.push_back(candidate(kMp4Protection.c_str(), "https://ka.example/key"));
        c.push_back(candidate(kWidevine.c_str(), "https://ka.example/key"));
        const DrmSchemes::Decision d = DrmSchemes::decide(c);
        check(d.usable && d.handledByPlatform && d.index == 1,
              "Widevine CDM present: the named Widevine entry wins over mp4protection");
        checkEq(d.schemeIdUri, kWidevine, "the decision carries the canonical scheme id");
    }

    // 4c. The bug this whole change fixes: a manifest that names only
    //     mp4protection must NOT publish a keyFormat that no CDM recognises,
    //     because a non-null DrmInfo makes codecPrototype::create reject every
    //     software decoder (avcodecDecoder::is_drmSupport returns false).
    //
    //     handleFormat() below is exactly what DashStream writes into
    //     Stream_meta.keyFormat: "handledByPlatform ? decision.schemeIdUri : empty".
    {
        g_accepted.clear();
        std::vector<DrmSchemes::Candidate> c;
        c.push_back(candidate(kMp4Protection.c_str(), "https://ka.example/key", "", "00112233"));
        const DrmSchemes::Decision d = DrmSchemes::decide(c);
        const std::string keyFormat = d.handledByPlatform ? d.schemeIdUri : std::string();
        check(keyFormat.empty(),
              "mp4protection-only manifest publishes an EMPTY keyFormat (so software decoders "
              "are still eligible)");
        check(d.usable, "…and is still usable: the key comes from the declared URL");
    }

    // 4d. A named system WITHOUT a key URL loses to a generic declaration WITH
    //     one: on a platform with no CDM, only the latter can actually play.
    {
        g_accepted.clear();
        std::vector<DrmSchemes::Candidate> c;
        c.push_back(candidate(kMp4Protection.c_str(), "https://ka.example/key"));
        c.push_back(candidate(kPlayReady.c_str(), ""));
        const DrmSchemes::Decision d = DrmSchemes::decide(c);
        check(d.usable && !d.handledByPlatform && d.index == 0,
              "no CDM: the entry that can actually supply a key wins");
    }

    // 4e. A named system without any key URL and no CDM: not usable, and the
    //     decision says so instead of pretending.
    {
        g_accepted.clear();
        std::vector<DrmSchemes::Candidate> c;
        c.push_back(candidate(kMp4Protection.c_str(), ""));
        c.push_back(candidate(kPlayReady.c_str(), ""));
        const DrmSchemes::Decision d = DrmSchemes::decide(c);
        check(!d.usable, "no CDM and no key URL anywhere: not usable");
    }

    // 4f. An HLS-only ClearKey identifier must never be chosen for the DASH
    //     CENC path, even when it carries a URL.
    {
        g_accepted.clear();
        std::vector<DrmSchemes::Candidate> c;
        c.push_back(candidate(kClearKeyHls.c_str(), "https://ka.example/key"));
        c.push_back(candidate(kWidevine.c_str(), ""));
        const DrmSchemes::Decision d = DrmSchemes::decide(c);
        check(d.index == 1 && !d.usable,
              "a non-CENC HLS ClearKey declaration is ranked below a real CENC system");
    }

    // 4g. Ties go to the last declaration: MPDParser appends AdaptationSet
    //     entries before Representation entries, so this is DASH inheritance.
    {
        g_accepted.clear();
        std::vector<DrmSchemes::Candidate> c;
        c.push_back(candidate(kWidevine.c_str(), "https://ka.example/key"));
        c.push_back(candidate(kPlayReady.c_str(), "https://ka.example/key"));
        const DrmSchemes::Decision d = DrmSchemes::decide(c);
        check(d.index == 1, "an exact tie is won by the later declaration (Rep overrides AS)");
    }

    // 4h. Empty input is -1, not a crash and not a fake success.
    {
        std::vector<DrmSchemes::Candidate> c;
        const DrmSchemes::Decision d = DrmSchemes::decide(c);
        check(d.index < 0 && !d.usable, "no declarations at all gives index -1 / not usable");
    }

    // -----------------------------------------------------------------------
    printf("\n== 5. ContentKeyFetcher::parse(): the three licence shapes ==\n");
    // -----------------------------------------------------------------------
    {
        const uint8_t raw[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
        const ContentKeyFetcher::Result r = ContentKeyFetcher::parse(raw, 16, "test");
        check(r.ok() && r.keySize == 16 && memcmp(r.key, raw, 16) == 0,
              "a raw 16-byte body is the key itself");
    }
    {
        // W3C ClearKey: base64url (URL-safe alphabet, no padding) of
        //   key = fbffbf00112233445566778899aabbcc
        //   kid = 0123456789abcdef0123456789abcdef
        // The 0xfb/0xff/0xbf prefix is deliberate: standard base64 would emit
        // "+/" here, so this fixture proves the URL-safe alphabet is accepted.
        const char *json = "{\"keys\":[{\"kty\":\"oct\","
                           "\"k\":\"-_-_ABEiM0RVZneImaq7zA\","
                           "\"kid\":\"ASNFZ4mrze8BI0VniavN7w\"}],\"type\":\"temporary\"}";
        const ContentKeyFetcher::Result r =
            ContentKeyFetcher::parse(reinterpret_cast<const uint8_t *>(json),
                                     static_cast<int>(strlen(json)), "test");
        const uint8_t want[16] = {0xfb, 0xff, 0xbf, 0x00, 0x11, 0x22, 0x33, 0x44,
                                  0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc
                                 };
        check(r.ok() && memcmp(r.key, want, 16) == 0,
              "a W3C ClearKey licence is decoded from base64url \"k\"");
    }
    {
        const char *json = "{\"kid\":\"00112233445566778899aabbccddeeff\","
                           "\"key\":\"2b7e151628aed2a6abf7158809cf4f3c\",\"scheme\":\"cenc\"}";
        const ContentKeyFetcher::Result r =
            ContentKeyFetcher::parse(reinterpret_cast<const uint8_t *>(json),
                                     static_cast<int>(strlen(json)), "test");
        const uint8_t want[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                                  0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
                                 };
        check(r.ok() && memcmp(r.key, want, 16) == 0,
              "the key server's own JSON view is decoded from hex \"key\"");
    }
    {
        const char *json = "{\"keys\":[{\"kty\":\"oct\",\"k\":\"AAAA\",\"kid\":\"AA\"}]}";
        const ContentKeyFetcher::Result r =
            ContentKeyFetcher::parse(reinterpret_cast<const uint8_t *>(json),
                                     static_cast<int>(strlen(json)), "test");
        check(r.status == ContentKeyFetcher::Result::Status::Malformed,
              "a 3-byte key is Malformed, never used");
    }
    {
        const char *text = "not a licence at all";
        const ContentKeyFetcher::Result r =
            ContentKeyFetcher::parse(reinterpret_cast<const uint8_t *>(text),
                                     static_cast<int>(strlen(text)), "test");
        check(r.status == ContentKeyFetcher::Result::Status::Unrecognized,
              "an unrecognised body is refused, not half-used");
    }
    {
        const ContentKeyFetcher::Result r = ContentKeyFetcher::parse(nullptr, 0, "test");
        check(!r.ok(), "an empty response is a failure");
    }
    {
        // "kid" must not be mistaken for the key field "k", and "keys"/"kty" must
        // not be mistaken for "key".
        const char *json = "{\"kty\":\"oct\",\"kid\":\"ASNFZ4mrze8BI0VniavN7w\","
                           "\"key\":\"2b7e151628aed2a6abf7158809cf4f3c\"}";
        const ContentKeyFetcher::Result r =
            ContentKeyFetcher::parse(reinterpret_cast<const uint8_t *>(json),
                                     static_cast<int>(strlen(json)), "test");
        check(r.ok() && r.key[0] == 0x2b && r.key[15] == 0x3c,
              "the JSON field scan does not confuse kid/kty/keys with k/key");
    }

    printf("\n%s: %d checks, %d failures\n",
           g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
