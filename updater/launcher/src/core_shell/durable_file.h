#pragma once

// Each platform defines these with its own file calls.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace parties::update::launcher {

std::string utf8(const std::filesystem::path& path);

std::optional<std::string> read_file(const std::filesystem::path& path);

// Write <file>.tmp, flush it, then replace <file> with it durably (Windows:
// MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)): a reader sees the old bytes
// or the new bytes, never a mix.
bool durable_replace(const std::filesystem::path& file, std::string_view bytes, std::string& error);

// Flushes the file's contents to the disk itself, not just the OS cache.
bool flush_file(const std::filesystem::path& path, std::string& error);

// Renames `from` over `to`, replacing it, and makes the rename durable.
bool durable_rename(const std::filesystem::path& from, const std::filesystem::path& to, std::string& error);

// Copies `from` to `to`, replacing it. The copy is not flushed.
bool copy_file(const std::filesystem::path& from, const std::filesystem::path& to, std::string& error);

} // namespace parties::update::launcher
