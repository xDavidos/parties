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
#include <vector>

namespace parties::update::launcher {

struct SourceError {
    std::string detail;
};

// Bytes written so far for the current file.
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
};

// A folder holding <channel>.manifest.json and files\<sha256 hex>. Used by the
// e2e harness, and by anyone testing a release from disk.
std::unique_ptr<UpdateSource> make_local_dir_source(std::filesystem::path dir);

} // namespace parties::update::launcher
