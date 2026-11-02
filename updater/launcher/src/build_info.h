#pragma once

// builds\<dir>\build.json: the signed manifest entry the directory was sealed
// from, minus URLs. Written into the staging directory before the seal
// rename, so every sealed directory carries it; setup writes the same file
// for the bundled build. It is what first-run adoption and state
// reconstruction read, because a build directory alone cannot say which data
// schema or link protocol its app speaks.

#include <parties/update/manifest.h>

#include <optional>
#include <string>
#include <string_view>

namespace parties::update::launcher {

inline constexpr std::string_view kBuildInfoFile = "build.json";

std::string write_build_info(const BuildEntry& entry);
std::optional<BuildEntry> parse_build_info(std::string_view text);

} // namespace parties::update::launcher
