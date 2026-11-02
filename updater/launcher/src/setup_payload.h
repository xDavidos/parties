#pragma once

// PartiesSetup.exe is PartiesLauncher.exe with a payload in RCDATA resources:
//   PARTIES_MANIFEST  a signed channel manifest that lists the bundled build
//   PARTIES_INDEX     {"format":1,"version":"A.B.C.D","stub_size":N,"stub_sha256":"<hex>"}
//   PARTIES_STUB      Parties.exe, which is not part of any build
//   PARTIES_FILE_<i>  the i-th file of the bundled build's manifest entry
// Resources live inside the image, so the Authenticode signature applied
// after packing covers all of them, and reading them needs no PE parsing.

#include "paths.h"
#include "update_source.h"

#include <parties/update/manifest.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace parties::update::launcher {

// What a payload directory holds besides Parties.exe and the build's files.
inline constexpr std::string_view kPayloadManifest = "manifest.json";

// Proven before setup writes anything: a pinned key signed the manifest, the
// manifest lists `entry`, and every byte below matches its digest. The
// stub's digest comes from the index, so for the stub this is an integrity
// check; its authenticity is the setup's own Authenticode signature.
struct Payload {
    BuildEntry entry;
    std::span<const uint8_t> stub;
    std::vector<std::span<const uint8_t>> files;  // in entry.files order
};

// nullopt when this executable carries no payload, which makes it a plain
// launcher.
std::optional<std::expected<Payload, std::string>> embedded_payload(Keyring keys);

// Writes `out`: a copy of this executable carrying `dir` as its payload. The
// bundled build is the manifest entry whose files `dir` holds. Refuses a
// payload that embedded_payload would reject.
bool pack_payload(const fs::path& dir, const fs::path& out, Keyring keys, std::string& error);

// Serves the payload's files to BuildStore::seal.
std::unique_ptr<UpdateSource> make_payload_source(const Payload& payload);

} // namespace parties::update::launcher
