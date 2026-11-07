#pragma once

#include <parties/update/link_protocol.h>

#include <cstdint>
#include <string>

namespace parties::update {

// What a build says about itself. `parties_client.exe --write-build-info <path>`
// writes it and the release tool turns it into the build's manifest entry.
// Header-only so the client writes it without linking the update core.
struct BuildInfo {
    std::string version;  // A.B.C.D, digits and dots only
    uint32_t data_schema = 1;
    uint32_t data_compat_floor = 1;
    uint32_t link_protocol = kLinkProtocol;
    uint32_t min_supported_launcher_link = kMinLauncherLink;
};

inline constexpr uint32_t kBuildInfoSchema = 1;

inline std::string format_build_info(const BuildInfo& b) {
    return "{\"schema\":" + std::to_string(kBuildInfoSchema) +
           ",\"version\":\"" + b.version +
           "\",\"data_schema\":" + std::to_string(b.data_schema) +
           ",\"data_compat_floor\":" + std::to_string(b.data_compat_floor) +
           ",\"link_protocol\":" + std::to_string(b.link_protocol) +
           ",\"min_supported_launcher_link\":" + std::to_string(b.min_supported_launcher_link) + "}\n";
}

} // namespace parties::update
