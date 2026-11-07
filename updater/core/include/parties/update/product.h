#pragma once

// A product the updater keeps current. Every build of either product carries
// exactly one launcher file and one app file, each a bare name in the build
// directory, so BuildEntry, BuildRecord and boot::BootContract mean the same
// for both. The id is inside the signed payload, so a manifest for one
// product never feeds the other.

#include <string_view>

namespace parties::update {

struct ProductSpec {
    std::string_view id;
    std::string_view launcher_file;  // its digest is BuildRecord::launcher_sha256
    std::string_view app_file;       // its digest is BuildRecord::app_sha256
};

inline constexpr ProductSpec kWindowsClient{"parties-client-windows", "PartiesLauncher.exe", "parties_client.exe"};
// Dylibs the macOS host loads into its own processes.
inline constexpr ProductSpec kMacClient{"parties-client-macos-arm64", "PartiesLauncher.dylib", "PartiesClient.dylib"};

} // namespace parties::update
