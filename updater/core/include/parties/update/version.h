#pragma once

#include <parties/update/sha256_digest.h>

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace parties::update {

// Release version A.B.C.D, the same shape as the vA.B.C.D git tags. The single
// ordering used for newer, older and downgrade decisions.
struct Version {
    std::array<uint32_t, 4> parts{};

    // Strict: four dot-separated decimal parts, no sign, no leading zeros, no
    // "v" prefix, each part fits in 32 bits. to_string(parse(s)) == s.
    static std::optional<Version> parse(std::string_view text);
    std::string to_string() const;

    friend bool operator==(const Version&, const Version&) = default;
    friend auto operator<=>(const Version&, const Version&) = default;
};

// Identity of a build: its version plus a digest over its whole file list, so
// two builds that differ in any file never share a directory.
struct BuildId {
    Version version;
    Sha256 digest;

    // "1.6.0.0-ab12cd34", the directory name under builds\.
    std::string dir_name() const;

    friend bool operator==(const BuildId&, const BuildId&) = default;
};

} // namespace parties::update
