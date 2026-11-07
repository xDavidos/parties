#include <parties/update/sha256_digest.h>

namespace parties::update {

namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

constexpr char kHexDigits[] = "0123456789abcdef";

} // namespace

std::optional<Sha256> Sha256::from_hex(std::string_view hex) {
    if (hex.size() != 64) return std::nullopt;
    Sha256 out;
    for (size_t i = 0; i < 32; ++i) {
        int hi = hex_value(hex[2 * i]);
        int lo = hex_value(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.bytes[i] = static_cast<uint8_t>(hi * 16 + lo);
    }
    return out;
}

std::string Sha256::to_hex() const {
    std::string out;
    out.reserve(64);
    for (uint8_t b : bytes) {
        out.push_back(kHexDigits[b >> 4]);
        out.push_back(kHexDigits[b & 0x0f]);
    }
    return out;
}

std::string Sha256::short_hex() const {
    return to_hex().substr(0, 8);
}

} // namespace parties::update
