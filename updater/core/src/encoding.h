#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace parties::update {

std::string base64_encode(std::span<const uint8_t> bytes);
// Strict: standard alphabet, required padding, no whitespace, canonical
// trailing bits.
std::optional<std::vector<uint8_t>> base64_decode(std::string_view text);

// "YYYY-MM-DDTHH:MM:SSZ", UTC only.
std::string format_utc(std::chrono::sys_seconds t);
std::optional<std::chrono::sys_seconds> parse_utc(std::string_view text);

} // namespace parties::update
