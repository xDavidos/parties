#pragma once

// Authenticode publisher pinning, defense in depth behind the signed manifest:
// a downloaded executable must also be signed by the expected publisher.

#include "paths.h"

#include <optional>
#include <string>
#include <string_view>

namespace parties::update::launcher {

// The PARTIES_AUTHENTICODE_PUBLISHER cache variable. Empty: not pinned.
std::string_view pinned_publisher();

// nullopt when `file` carries a valid Authenticode signature, with revocation
// checked along the whole chain, whose signer certificate's common name is
// exactly `publisher`. Otherwise, why not.
std::optional<std::string> publisher_mismatch(const fs::path& file, std::string_view publisher);

} // namespace parties::update::launcher
