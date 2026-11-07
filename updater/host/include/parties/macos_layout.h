// Where Parties keeps its files on macOS, under ~/Library/Application Support.
// For both the host and the launcher, so each name is spelled once. The
// runtime root (builds, downloads, launcher state) and the data root are
// disjoint, so clearing the runtime never touches the user's data.

#pragma once

#include <filesystem>
#include <string_view>

namespace parties::update::macos {

inline constexpr std::string_view kRuntimeRoot = "Parties Runtime";

// A dev build keeps its own data root, so its schema never migrates the
// database a release build reads. Every target that includes this header
// defines PARTIES_RETAIL the way the host does.
#ifdef PARTIES_RETAIL
inline constexpr std::string_view kDataRoot = "Parties";
#else
inline constexpr std::string_view kDataRoot = "Parties Dev";
#endif

// A persisted build path is the token "builds\<dir>" on every platform, and
// this is its one join to a runtime root. The token must have passed
// boot::is_build_dir, as every front-door Assign has.
inline std::filesystem::path build_path(const std::filesystem::path& runtime_root, std::string_view token) {
    return runtime_root / "builds" / token.substr(std::string_view("builds\\").size());
}

} // namespace parties::update::macos
