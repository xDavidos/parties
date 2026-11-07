#pragma once

// The macOS launcher's file calls beyond durable_file.h. Durability on macOS
// needs F_FULLFSYNC, since fsync only reaches the drive's cache, and a
// rename survives power loss only once its directory is fsynced too.

#include "durable_file.h"

#include <parties/update/sha256_digest.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace parties::update::launcher {

namespace fs = std::filesystem;

struct FileDigest {
    uint64_t size = 0;
    Sha256 sha256;
    friend bool operator==(const FileDigest&, const FileDigest&) = default;
};

std::optional<FileDigest> digest_file(const fs::path& path);

// Renames `dir` to a unique ".trash-*" sibling, then deletes that, so a
// half-deleted directory never carries a sealed build's name.
bool remove_tree(const fs::path& dir, std::string& error);

// clonefile(2), or a copy where the volume cannot clone (EXDEV, not APFS).
// `to` must not exist.
bool clone_file(const fs::path& from, const fs::path& to, std::string& error);

bool flush_directory(const fs::path& dir, std::string& error);

} // namespace parties::update::launcher
