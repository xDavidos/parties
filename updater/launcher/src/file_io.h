#pragma once

#include <parties/update/sha256_digest.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace parties::update::launcher {

namespace fs = std::filesystem;

std::string utf8(const fs::path& path);
std::string last_error_text();

std::optional<std::string> read_file(const fs::path& path);

// Write <file>.tmp, flush it, then MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH):
// a reader sees the old bytes or the new bytes, never a mix.
bool durable_replace(const fs::path& file, std::string_view bytes, std::string& error);

// Flushes an existing file to stable storage.
bool flush_file(const fs::path& path, std::string& error);

struct FileDigest {
    uint64_t size = 0;
    Sha256 sha256;
    friend bool operator==(const FileDigest&, const FileDigest&) = default;
};

std::optional<FileDigest> digest_file(const fs::path& path);
// Hashes through an open handle from offset 0; the handle needs read access.
std::optional<FileDigest> digest_handle(void* handle);

// Renames `dir` to a unique ".trash-*" sibling, then deletes that, so a
// half-deleted directory never carries a sealed build's name.
bool remove_tree(const fs::path& dir, std::string& error);

} // namespace parties::update::launcher
