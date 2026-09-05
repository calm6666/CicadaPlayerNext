//
// Created by SuperMan on 11/27/20.
//

#ifndef SOURCE_DRMINFO_H
#define SOURCE_DRMINFO_H

#include <string>

namespace Cicada {
    class DrmInfo {
    public:
        std::string uri;
        std::string format;
        // Optional DRM init data: base64 PSSH box and/or default key id,
        // populated from ContentProtection in object-based playback.
        std::string pssh;
        std::string keyId;

        bool operator==(const DrmInfo &drmInfo) const {
            return uri == drmInfo.uri &&
                   format == drmInfo.format &&
                   pssh == drmInfo.pssh &&
                   keyId == drmInfo.keyId;
        }

        bool empty() const {
            return uri.empty() &&
                   format.empty() &&
                   pssh.empty() &&
                   keyId.empty();
        }

        struct DrmInfoCompare
        {
            bool operator() (const DrmInfo& lhs, const DrmInfo& rhs) const
            {
                if (lhs.format != rhs.format) {
                    return lhs.format < rhs.format;
                }
                if (lhs.uri != rhs.uri) {
                    return lhs.uri < rhs.uri;
                }
                if (lhs.pssh != rhs.pssh) {
                    return lhs.pssh < rhs.pssh;
                }
                return lhs.keyId < rhs.keyId;
            }
        };
    };
}


#endif //SOURCE_DRMINFO_H
