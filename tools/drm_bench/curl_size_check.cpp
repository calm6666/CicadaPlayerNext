/*
 * Verification for the curl CURLINFO_*_DOWNLOAD -> *_T migration.
 *
 * It drives the REPOSITORY'S OWN data sources over HTTP (not a private copy of
 * the getinfo call), because the point of the change is that the produced file
 * size still feeds the range-request logic correctly.
 *
 *   curl_data_source  / curl_data_source2 : curl_easy_getinfo(..._T) -> mFileSize
 *   ffmpeg_data_source                    : avio_size()          -> mFileSize
 *
 * If the relaxed _T variant were wired to the wrong variable or the wrong type
 * had been used, the reported size would differ from the server's
 * Content-Length.
 */
#define LOG_TAG "curlsizecheck"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#include "data_source/dataSourcePrototype.h"

using namespace Cicada;

// NOTE: no __log_print stub here.  Unlike the standalone decrypter benchmarks,
// this harness links framework_utils (the data_source library needs it), so the
// real logging symbol is present and a stub would be a duplicate definition.

static bool sizeVia(const char *what, const std::string &uri, int64_t *out, int *openRet)
{
    IDataSource *source = dataSourcePrototype::create(uri);

    if (source == nullptr) {
        printf("  %-18s : no data source created\n", what);
        return false;
    }

    const int ret = source->Open(0);
    *openRet = ret;

    if (ret < 0) {
        printf("  %-18s : Open failed (%d)\n", what, ret);
        delete source;
        return false;
    }

    const int64_t size = source->Seek(0, SEEK_SIZE);
    *out = size;
    printf("  %-18s : Open=%d Seek(SEEK_SIZE)=%lld\n", what, ret, (long long) size);

    source->Close();
    delete source;
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: curlsizecheck <http-url> <expected-bytes>\n");
        return 2;
    }

    const std::string uri = argv[1];
    const int64_t expected = std::strtoll(argv[2], nullptr, 10);
    printf("expected Content-Length = %lld\n", (long long) expected);

    int failures = 0;

    // `dataSourcePrototype::create` probes registered prototypes first and falls
    // through to curl for http, so this exercises the migrated getinfo.
    int64_t size = -1;
    int ret = 0;

    if (!sizeVia("raw http (curl)", uri, &size, &ret)) {
        printf("RESULT: FAIL (could not read over http)\n");
        return 1;
    }

    if (size == expected) {
        printf("  OK: size matches Content-Length\n");
    } else {
        printf("  FAIL: size %lld != expected %lld\n", (long long) size, (long long) expected);
        failures++;
    }

    // Range request: must still work after the type change (curl_off_t instead
    // of double), because mFileSize feeds the range logic.
    {
        IDataSource *source = dataSourcePrototype::create(uri);

        if (source != nullptr && source->Open(0) >= 0) {
            source->Seek(4096, SEEK_SET);
            uint8_t buf[64];
            const int n = source->Read(buf, sizeof(buf));
            printf("  %-18s : Seek(4096)+Read -> %d bytes\n", "range read", n);

            if (n == (int) sizeof(buf)) {
                printf("  OK: ranged read still works\n");
            } else {
                printf("  FAIL: ranged read returned %d, expected %zu\n", n, sizeof(buf));
                failures++;
            }
        } else {
            printf("  FAIL: could not reopen for the range read\n");
            failures++;
        }

        if (source != nullptr) {
            source->Close();
            delete source;
        }
    }

    printf("\n%s (%d failure%s)\n", failures == 0 ? "RESULT: PASS" : "RESULT: FAIL",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
