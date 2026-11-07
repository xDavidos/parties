#pragma once

// Each platform defines these with its own file calls.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace parties::update::launcher {

std::optional<std::string> read_file(const std::filesystem::path& path);

// Write <file>.tmp, flush it, then replace <file> with it durably (Windows:
// MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)): a reader sees the old bytes
// or the new bytes, never a mix.
bool durable_replace(const std::filesystem::path& file, std::string_view bytes, std::string& error);

} // namespace parties::update::launcher
