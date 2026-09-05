//
// MediaManifestParser.h
//
// JSON <-> MediaManifest conversion for object-based playback.
//
// The JSON schema matches hili-player's `MediaManifest` TypeScript type
// (camelCase field names, see manifest.ts). Parsing is a one-shot conversion:
// the JSON object is consumed directly, never serialized into m3u8/mpd text.
//

#ifndef FRAMEWORK_DEMUXER_MANIFEST_MEDIA_MANIFEST_PARSER_H
#define FRAMEWORK_DEMUXER_MANIFEST_MEDIA_MANIFEST_PARSER_H

#include "MediaManifest.h"
#include <string>

namespace Cicada {
namespace Manifest {

class MediaManifestParser {
public:
    /**
     * Parse a JSON document into a MediaManifest.
     *
     * @param json  JSON text following the hili-player MediaManifest schema.
     * @param[out] manifest  parsed manifest.
     * @param[out] error  on failure, a human-readable error message.
     * @return true on success.
     */
    static bool parse(const std::string &json, MediaManifest &manifest, std::string &error);

    /** Validate a manifest has enough information to play (duration, video). */
    static bool validate(const MediaManifest &manifest, std::string &error);

    /** Serialize a manifest back to JSON (used by tools and debug output). */
    static std::string toJson(const MediaManifest &manifest);
};

} // namespace Manifest
} // namespace Cicada

#endif // FRAMEWORK_DEMUXER_MANIFEST_MEDIA_MANIFEST_PARSER_H
