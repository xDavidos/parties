#pragma once

// Where signed manifests and build files come from. Everything a source
// returns is untrusted: the caller verifies manifests against the pinned
// keyring and every file against the digest in the verified manifest. A
// source only moves bytes.

#include <parties/update/manifest.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace parties::update::launcher {

struct SourceError {
    std::string detail;
};

using Progress = std::function<void(uint64_t bytes_done)>;

class UpdateSource {
public:
    virtual ~UpdateSource() = default;

    // The channel's envelope, at most kMaxEnvelopeBytes.
    virtual std::expected<std::vector<uint8_t>, SourceError> fetch_manifest(Channel channel,
                                                                           const std::atomic<bool>& cancel) = 0;

    // Writes the file to `dest`, which does not exist yet. The caller checks
    // size and SHA-256 afterwards and deletes `dest` on a mismatch.
    virtual std::expected<void, SourceError> fetch_file(const FileEntry& file, const std::filesystem::path& dest,
                                                        const Progress& progress,
                                                        const std::atomic<bool>& cancel) = 0;

    // Called from another thread after `cancel` was set, to end a blocked
    // network read now instead of at its timeout.
    virtual void abort() {}
};

inline constexpr std::string_view kFeedBase = "https://github.com/emcifuntik/parties/releases/download/channels/";

// A folder holding <channel>.manifest.json and files\<sha256 hex>.
std::unique_ptr<UpdateSource> make_local_dir_source(std::filesystem::path dir);

// WinHTTP over `base` (ending in '/'). Manifests are <base><channel>.manifest.json
// and files come from the URLs their entries list, in order. Only https,
// except that `loopback` (testing builds only) sends every request to an
// http://127.0.0.1:<port>/ base, keeping each URL's path.
std::unique_ptr<UpdateSource> make_https_source(std::string base, bool loopback);

} // namespace parties::update::launcher
