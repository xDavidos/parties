#pragma once

// Every edit starts from a payload that already passed verify_manifest (or from
// nothing) and returns the next payload to sign.

#include <parties/update/build_info.h>
#include <parties/update/manifest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace parties::update::release {

template <class T>
using Result = std::expected<T, std::string>;

Result<BuildInfo> parse_build_info(std::string_view json);

struct StagedFile {
    std::string name;
    std::string asset_name;
    uint64_t size = 0;
    Sha256 sha256;
};

// url_base is an https prefix ending in '/'; each file's url is url_base + asset_name.
Result<BuildEntry> make_entry(const BuildInfo& info, std::span<const StagedFile> files, std::string_view url_base);

// The header every edit rewrites. serial must exceed the base's serial, so a
// replayed or reordered run can never publish a payload clients would reject.
struct Issue {
    uint64_t serial = 0;
    Timestamp issued_at{};
    std::chrono::days lifetime{45};
};

// Keeps the newest `keep` builds, plus, for every launcher link protocol up to
// the newest one listed, the newest build that protocol can stage. A launcher
// in the field always keeps a path forward even when newer builds need a newer
// link protocol than it speaks.
std::vector<BuildEntry> retained(std::vector<BuildEntry> builds, size_t keep);

// Adds the entry, replacing one with the same version. Refuses a revoked
// version and an entry the retention would drop at once.
Result<ManifestPayload> add_build(const ManifestPayload* base, Channel channel, const BuildEntry& entry,
                                  const Issue& issue, size_t keep);

Result<ManifestPayload> promote(const ManifestPayload& beta, const ManifestPayload* stable, const Version& version,
                                const Issue& issue, size_t keep);

// Drops the build and lists its version as revoked, so launchers running it
// move off it. A null base is a channel with no manifest yet. Repeating a
// revocation only reissues. The tool revokes from every channel at once.
Result<ManifestPayload> revoke(const ManifestPayload* base, Channel channel, const Version& version,
                               const Issue& issue);

Result<ManifestPayload> reissue(const ManifestPayload& base, const Issue& issue);

} // namespace parties::update::release
