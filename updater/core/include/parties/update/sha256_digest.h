#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace parties::update {

// Raw SHA-256 digest. The text form is 64 lowercase hex characters, unlike
// parties::sha256_hex, which inserts colons for fingerprints.
struct Sha256 {
    std::array<uint8_t, 32> bytes{};

    // Strict: exactly 64 lowercase hex characters.
    static std::optional<Sha256> from_hex(std::string_view hex);
    std::string to_hex() const;
    std::string short_hex() const;  // first 8 hex characters

    friend bool operator==(const Sha256&, const Sha256&) = default;
    friend auto operator<=>(const Sha256&, const Sha256&) = default;
};

} // namespace parties::update
