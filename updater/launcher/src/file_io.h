#pragma once

#include <parties/update/sha256_digest.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace parties::update::launcher {

namespace fs = std::filesystem;

std::string utf8(const fs::path& path);
// Invalid UTF-8 becomes U+FFFD instead of throwing: last_error_text() is in the ANSI code page.
std::wstring widen(std::string_view s);
std::string last_error_text();

fs::path self_exe();

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

class Sha256Hasher {
public:
    Sha256Hasher();
    ~Sha256Hasher();
    Sha256Hasher(const Sha256Hasher&) = delete;
    Sha256Hasher& operator=(const Sha256Hasher&) = delete;

    bool ok() const { return hash_ != nullptr; }
    bool update(const void* data, unsigned long size);
    std::optional<Sha256> finish();

private:
    void* alg_ = nullptr;
    void* hash_ = nullptr;
};

std::optional<FileDigest> digest_file(const fs::path& path);
// Hashes through an open handle from offset 0; the handle needs read access.
std::optional<FileDigest> digest_handle(void* handle);
std::optional<FileDigest> digest_bytes(std::span<const uint8_t> bytes);

// Renames `dir` to a unique ".trash-*" sibling, then deletes that, so a
// half-deleted directory never carries a sealed build's name.
bool remove_tree(const fs::path& dir, std::string& error);

} // namespace parties::update::launcher
