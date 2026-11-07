#pragma once

// The pinned code requirement of retail builds: Developer ID, the Team ID the
// build was configured with (PARTIES_APPLE_TEAM_ID), and for the host its
// bundle identifier. It is defense in depth behind the SHA-256 the signed
// manifest pins. Builds that are not retail skip every check here, because
// ad-hoc signatures have no Team ID.

#include <filesystem>
#include <optional>
#include <string>

namespace parties::update::launcher::macos {

// nullopt when the file (a dylib, or the Parties.app bundle) meets the
// requirement; otherwise why not.
std::optional<std::string> code_requirement_failure(const std::filesystem::path& code, bool is_host_bundle);

// The same for the process at the other end of a connected Unix socket,
// judged by its audit token, so a recycled pid cannot stand in for it.
std::optional<std::string> peer_requirement_failure(int socket);

} // namespace parties::update::launcher::macos
